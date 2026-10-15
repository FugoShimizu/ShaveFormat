#include "Canonicalizer.hpp"
#include "FormatIR.hpp"
#include "NodeId.hpp"
#include "SignatureExtractor.hpp"
#include "SyntaxModel.hpp"
#include "SyntaxRole.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 見出の部品を指す場の子かの判定関数
	 * 本体の文を数える時に見出の部品（条件・名前・仮引数等）を除く為に使う
	 * @param Syntax 構文モデル
	 * @param Child 子
	 * @return 見出の部品の場の子なら true
	 */
	bool Canonicalizer::IsHeaderField(const SyntaxModel &Syntax, const NodeId Child) {
		// 見出の部品を指す場の名前
		static constexpr ChildRole HeaderFields[] = {
			ChildRole::Cond,
			ChildRole::Exceptions,
			ChildRole::Guard,
			ChildRole::Name,
			ChildRole::Object,
			ChildRole::Params,
			ChildRole::Pattern,
			ChildRole::Superclass,
			ChildRole::Val,
			ChildRole::Var
		};
		// 見出の部品である事の返戻
		for(const ChildRole Field : HeaderFields) if(Syntax.HasField(Child, Field)) return true;
		// 見出の部品でない事の返戻
		return false;
	}

	/**
	 * 本体の文の数の取得関数（整形仕様 6.3）
	 * 見出の部品と後送の本文を除き，０・１・２以上の文数を構築中に共有する
	 * 計算量：最初の照会で検査する子数 C に対し O(C)，以後の照会は O(1)
	 * @param State 構築中の状態
	 * @param Id ノード
	 * @return 本体の文の数（２で頭打）
	 * @throws FormatLimitExceeded 走査又は要約の参照が作業予算を超える場合
	 */
	size_t Canonicalizer::GetBodyStmtCount(Builder &State, const NodeId Id) {
		// 共有済の文数の参照
		const SyntaxModel &Syntax = State.Syntax;
		uint8_t &Count = State.Contexts[Id].BodyStmtCount;
		if(Count < 3) {
			Syntax.Charge(1);
			// 共有した文数の返戻
			return Count;
		}
		// 本体其の物が独立した空文なら１文として控える
		Count = IsEmptyStmt(Syntax, Id) ? 1 : 0;
		// 空文の文数の返戻
		if(Count) return Count;
		// 最初に必要に為った本体だけを，２文迄１度数える
		Core::SyntaxModel::ForEachStmtOf(
			Syntax,
			Id,
			// 見出の部品を除いた文の計数の継続条件
			[&](const NodeId Child) -> bool {
				if(IsHeaderField(Syntax, Child)) return true;
				return ++Count < 2;
			}
		);
		// 本体の文の数の返戻
		return Count;
	}

	/**
	 * 本体の文を持つかの判定関数
	 * 計算量：最初の照会で検査する子数 C に対し O(C)，以後の照会は O(1)
	 * @param State 構築中の状態
	 * @param Id ノード
	 * @return 本体の文を１つでも持てば true
	 * @throws FormatLimitExceeded 走査又は要約の参照が作業予算を超える場合
	 */
	bool Canonicalizer::HasBodyStmt(Builder &State, const NodeId Id) {
		// 本体の文の有無の返戻
		return GetBodyStmtCount(State, Id);
	}

	/**
	 * 併記した宣言子の有無の判定関数
	 * 型や初期化子の中のカンマを数えず，宣言子の並びの直接の区切だけを読む
	 * 計算量：直接の子数 C に対し O(C)，作業領域 O(1)
	 * @param Syntax 構文モデル
	 * @param Id 宣言
	 * @return 宣言子の並びの区切が在れば true
	 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
	 */
	bool Canonicalizer::HasMultipleDeclarators(const SyntaxModel &Syntax, const NodeId Id) {
		// 名前と値を別々に並べる宣言の左辺の照会
		NodeId Host = Syntax.GetFieldChild(Id, ChildRole::Left);
		if(Host == NoNode) {
			Host = Id;
			NodeId Spec = NoNode;
			for(const NodeId Child : Syntax.GetChildrenOf(Id)) {
				Syntax.Charge(1);
				if(Syntax.Is(Child, Role::CommaDecl)) {
					Host = Child;
					break;
				}
				if(!Syntax.IsNamed(Child)) continue;
				if(Spec == NoNode) Spec = Child;
				else {
					Spec = NoNode;
					break;
				}
			}
			if(Host == Id && Syntax.GetFieldChild(Spec, ChildRole::Name) != NoNode) Host = Spec;
		}
		// 宣言子の直接の区切の有無の返戻
		return SyntaxModel::HasChildMatching(
			Syntax,
			Host,
			[&Syntax](const NodeId Child) -> bool {
				// 並びの直接のカンマかの返戻
				return Syntax.IsTokenRole(Child, Role::CommaToken);
			}
		);
	}

	/**
	 * 宣言の分割で束縛が変わらないかの判定関数（整形仕様 5.6）
	 * 並列の名前と値を持つ宣言は，後の初期値が前の名前を参照すると束縛開始が変わる
	 * 計算量：名前数 D と初期値のノード数 N に対し平均 O(D + N)
	 * @param Syntax 構文モデル
	 * @param Shape 宣言の部品
	 * @return 分割しても束縛を保つなら true
	 * @throws FormatLimitExceeded 初期値の照会が作業予算を超える場合
	 * @throws std::bad_alloc 前の名前の集合のメモリを確保出来ない場合
	 */
	bool Canonicalizer::CanSplitDecl(const SyntaxModel &Syntax, const CommaDecl &Shape) {
		// 既存の束縛を再代入し得る宣言は，分割すると新しい束縛へ変わる
		// 既存の束縛を変える分割の拒否の返戻
		if(Syntax.GetNode(Shape.Node).Facts.CanReassignExistingBinding) return false;
		// 並列の初期化でない宣言の返戻
		if(Shape.Lists.size() < 2) return true;
		std::unordered_set<uint32_t> Names;
		Names.reserve(Shape.Lists.front().Items.size());
		for(size_t Idx = 0; Idx < Shape.Lists.front().Items.size(); ++Idx) {
			for(const NodeId Val : Shape.Lists[1].Items[Idx]) if(
				SyntaxModel::HasNodeMatching(
					Syntax,
					Val,
					[&Syntax, &Names](const NodeId Sub) -> bool {
						// 先行する名前への参照かの返戻
						return Syntax.Is(Sub, Role::Identifier) && Names.contains(Syntax.GetCmpNameIdx(Sub));
					}
				)
			// 後の初期値の束縛を変える分割の拒否の返戻
			) return false;
			const NodeId Name = Shape.Lists.front().Items[Idx].front();
			if(!Syntax.GetNode(Name).Facts.DoesDiscardBinding) Names.insert(Syntax.GetCmpNameIdx(Name));
		}
		// 束縛開始を変える依存が無い事の返戻
		return true;
	}

	/**
	 * セミコロンだけの空文の判定関数
	 * 全ノードのテキスト長を保持せず，候補の文だけを２バイトで打ち切って数える
	 * 計算量：空文候補の訪問ノード数 N に対し O(N)
	 * @param Syntax 構文モデル
	 * @param Id 文
	 * @return セミコロン１バイトだけを持つ式文なら true
	 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
	 * @throws std::bad_alloc 候補の走査枠のメモリを確保出来ない場合
	 */
	bool Canonicalizer::IsEmptyStmt(const SyntaxModel &Syntax, const NodeId Id) {
		// 独立した空文の役割の返戻
		if(Syntax.Is(Id, Role::EmptyStmt)) return true;
		// 元の空文の役割と先頭のセミコロンを持たない文の返戻
		if(
			!Syntax.Is(Id, Role::ExprStmt) ||
			!(Syntax.IsTokenRole(Id, Role::StmtTerminatorToken) || Syntax.IsTokenRole(Syntax.GetChild(Id, 0), Role::StmtTerminatorToken))
		) return false;
		size_t Bytes = 0;
		// ２バイト以上の本文が無く，１バイトのセミコロンを持つ事の返戻
		return !SyntaxModel::HasNodeMatching(
			Syntax,
			Id,
			[&Syntax, &Bytes](const NodeId Part) -> bool {
				const std::string_view Text = Syntax.GetNode(Part).Lexeme;
				// １バイトを超える候補の走査を止める事の返戻
				if(Text.size() > 1 || Bytes && !Text.empty()) return true;
				Bytes += Text.size();
				// 次の部品の計数を続ける事の返戻
				return false;
			}
		) && Bytes == 1;
	}

	/**
	 * 未登録のノードの構造化の控えを位置の変わらない枠へ格納する関数
	 * 取出済の枠を先に再使用する
	 * 計算量：格納回数 K に対し償却 O(K)
	 * @param State 構築中の状態
	 * @param Id 控えの所有者
	 * @param Piece 組立済の要素
	 * @return 格納した控え
	 * @throws std::bad_alloc 控えの領域又は其の管理列を確保出来ない場合
	 * @throws std::length_error 管理列の要素数が上限を超える場合
	 */
	const Canonicalizer::Built *Canonicalizer::StoreStructured(Builder &State, const NodeId Id, Built &&Piece) {
		BuiltSlot *Slot = State.FreeStructured;
		if(Slot) State.FreeStructured = Slot->Next;
		else {
			// 参照を移動させない領域を，必要に為った時だけ一定数ずつ確保する
			if(State.NextStructured == Builder::StructuredBlockSize) {
				State.StructuredBlocks.push_back(std::make_unique<std::array<BuiltSlot, Builder::StructuredBlockSize>>());
				State.NextStructured = 0;
			}
			Slot = &(*State.StructuredBlocks.back())[State.NextStructured++];
		}
		Slot->Value = std::move(Piece);
		State.Contexts[Id].Structured = Slot;
		// 控えの返戻
		return &Slot->Value;
	}

	/**
	 * 登録済の構造化の控えを取り出し，枠を空き列へ戻す関数
	 * 呼出側は此のノードが控えを持つ事を確認する
	 * 計算量：O(1)
	 * @param State 構築中の状態
	 * @param Id 控えを持つノード
	 * @return 所有を移した組立済の要素
	 */
	Canonicalizer::Built Canonicalizer::ReleaseStructured(Builder &State, const NodeId Id) {
		BuiltSlot *const Slot = std::exchange(State.Contexts[Id].Structured, nullptr);
		Built Result = std::move(Slot->Value);
		Slot->Next = State.FreeStructured;
		State.FreeStructured = Slot;
		// 控えから取り出した要素の返戻
		return Result;
	}

	/**
	 * 構造化の控えを作業列で取り出す関数
	 * 計算量：O(1)
	 * @param State 構築中の状態
	 * @param Id 控えを持つノード
	 * @return 控えた組立の所有を移した結果
	 * @throws std::bad_alloc 作業列又はフレームのメモリを確保出来ない場合
	 * @throws std::length_error 作業列が保持領域の最大長を超える場合
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::TakeStoredStructured(Builder &State, const NodeId Id) {
		// 控えの所有を移した結果の返戻
		co_return ReleaseStructured(State, Id);
	}

	/**
	 * 構造化の控えの取出関数
	 * 計算量：初回に訪問する構文・生成要素の延数 N とテキストの総長 B に対し期待値 O(N + B)，控えの取出は期待値 O(1)
	 * @param State 構築中の状態
	 * @param Id ノード
	 * @return 構造化の要素（控えから除く）
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::TakeStructured(Builder &State, const NodeId Id) {
		// 封止中の平坦形の返戻
		if(State.NoBreakDepth) return BuildFlat(State, Id);
		// 構造化の控えの返戻
		if(State.Contexts[Id].Structured) return TakeStoredStructured(State, Id);
		// 新たに組み立てた要素の返戻
		return BuildStructured(State, Id);
	}

	/**
	 * 末尾の区切の追加関数
	 * 最上位の文の後の，独立した文でない `;` / `,` は，前の文の要素と共に出す
	 * 計算量：末尾の区切記法のテキスト長 B に対し O(B)
	 * @param Syntax 構文モデル
	 * @param Id 文のノード
	 * @param Output 組立中の出力
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::AppendTrailingSeparator(const SyntaxModel &Syntax, const NodeId Id, Emitter &Output) {
		// 末尾の区切の追加
		const NodeId Next = Syntax.GetNextSibling(Id);
		// プリプロセッサディレクティブの区切は親が出し，最上位の文を改行で区切れる場合の `;` も重ねない
		// 後に必要な区切が無い文又は，後続の区切が独立した空文である場合の終了
		if(
			Next == NoNode || Syntax.IsNamed(Next) || Syntax.Is(Next, Role::EmptyStmt) ||
			!Syntax.IsTokenRole(Next, Role::StmtTerminatorToken) && !Syntax.IsTokenRole(Next, Role::CommaToken) ||
			Syntax.GetRules().CanOmitTrailingSemicolon && Syntax.IsTokenRole(Next, Role::StmtTerminatorToken) ||
			Syntax.Is(Syntax.GetParentOf(Id), Role::PreprocBranch)
		) return;
		if(
			!Output.IsTailRole(Syntax, Role::StmtTerminatorToken, GeneratedToken::StmtTerminator) &&
			!Output.IsTailRole(Syntax, Role::CommaToken, GeneratedToken::ListSeparator)
		) Output.Append(BuildLeaf(Syntax, Output.GetStore(), Next));
		// 終了
		return;
	}

	/**
	 * Ruby の展開を続けるかの判定関数（整形仕様 1.4 の「空の本体は畳む」の裏返）
	 * 計算量：最初の照会で検査する子数 C に対し O(C)，以後の照会は O(1)
	 * @param State 構築中の状態
	 * @param Id ノード
	 * @return 本体の文が在るか畳むと構文エラーに為れば true
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 */
	bool Canonicalizer::ShouldExpandKeywordBody(Builder &State, const NodeId Id) {
		// 展開を続けるかの返戻
		return HasBodyStmt(State, Id) || State.Syntax.ShouldSeparateBeforeClose(Id);
	}

	/**
	 * Kotlin の `->` の後の値の if 式かの判定関数
	 * @param State 構築中の状態（構築の文脈を持つ物）
	 * @param Id if 式
	 * @return when の枝・ラムダの値の if 式なら true
	 */
	bool Canonicalizer::IsArrowValBody(const Builder &State, const NodeId Id) {
		// 値の本体かの返戻
		return State.Contexts[Id].IsArrowVal;
	}

	/**
	 * ヒアドキュメントの本体の組立関数
	 * 本体の先頭の改行を保ち，終端の語の後の境界は後続の配置へ委ねる
	 * 計算量：本文のテキスト長 L とノード数 N に対し O(L + N) と子の組立・出力生成の総費用
	 * @param State 構築中の状態
	 * @param Body ヒアドキュメントの本体
	 * @return 後続の必須境界を持つ本文（空の本体なら空の組立）
	 * @throws FormatLimitExceeded 本文の構築が作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildHeredocBody(Builder &State, const NodeId Body) {
		// ヒアドキュメントの本体の追加
		const SyntaxModel &Syntax = State.Syntax;
		if(!Syntax.GetChildrenOf(Body).empty()) {
			Built Piece = co_await BuildFlat(State, Body);
			Piece.Tail.ShouldBreakAfter = true;
			// 補間式を含む本文の返戻
			co_return Piece;
		}
		const std::string_view Text = Syntax.GetNode(Body).Lexeme;
		// 空の本体の読飛の返戻
		if(Text.empty()) co_return Built();
		// 本文に含まれる先頭の改行を使い，次の文の境界は其の文の配置時に生成する
		FormatIR &IR = State.IR;
		Built Piece;
		Piece.Item = IR.Leaf(Text, FormatItemKind::Verbatim);
		IR.Items[Piece.Item].Node = Body;
		Piece.Head = FindEdge(IR, Piece.Item, true);
		Piece.Tail = Piece.Head;
		Piece.Tail.ShouldBreakAfter = true;
		Piece.HasBreak = true;
		// 組立済の本文の返戻
		co_return Piece;
	}

	/**
	 * 本文を控えたヒアドキュメントの開始の数の更新関数
	 * 部分木の開始の数を足し，本文の数を引く（本文が前の兄弟の開始に対応する形も有る為，負に為れば０）
	 * @param State 構築中の状態（部分木の集計を持つ）
	 * @param Id 部分木の根（自身を含む）
	 * @param Open 本文を控えた開始の数（更新する）
	 */
	void Canonicalizer::TrackOpenHeredocs(const Builder &State, const NodeId Id, uint32_t &Open) {
		// 本文を後送するトークンを持たない記法の終了
		if(State.Heredocs.empty()) return;
		const HeredocSummary &Summary = State.Heredocs[Id];
		Open = Open + Summary.Begins > Summary.Bodies ? Open + Summary.Begins - Summary.Bodies : 0;
		// 終了
		return;
	}

	/**
	 * 多行の並びの末尾のカンマの生成形式の取得関数
	 * 閉じの前で改行した並びにカンマを要するかは投影済の形式から決める
	 * 計算量：生成形式の数 F と閉じの端の探索で訪問するノード数 N に対し O(F + N)
	 * @param Syntax 構文モデル
	 * @param Container 並び
	 * @param Last 閉じの前の子
	 * @param Closer 閉じのトークン
	 * @param IsBroken 並びを複数行へ展開するか
	 * @return カンマを補う生成形式（適用条件を満たさなければ `nullptr`）
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 */
	const CanonicalForm *Canonicalizer::TrailingCommaForm(
		const SyntaxModel &Syntax,
		const NodeId Container,
		const NodeId Last,
		const NodeId Closer,
		const bool IsBroken
	) {
		// 多行の並びの末尾のカンマの生成形式の取得
		const CanonicalForm *const Form = Syntax.GetFormOf(Container, FormOperation::TrailingComma);
		// 投影した適用条件を満たす並びの生成形式の返戻
		return Form && Form->CanAddTrailingComma(IsBroken, Syntax.IsNamed(Last), Syntax.GetFirstChar(Closer)) ? Form : nullptr;
	}

	/**
	 * 展開した閉括弧の後の改行の判定関数
	 * 行頭の閉波括弧に閉括弧が続く場合は改行するが，自動の文区切を招く文法では改行しない（整形仕様 6.3）
	 * @param Syntax 構文モデル
	 * @param NextHead 後続の先頭の要素
	 * @param Output 組立中の出力
	 * @return 後続の閉括弧を次の行へ送るなら true
	 */
	bool Canonicalizer::ShouldBreakAfterExpandedBrace(const SyntaxModel &Syntax, const EdgeItem NextHead, const Emitter &Output) {
		// 閉波括弧の後の改行が自動のセミコロンを招く文法と，行頭の閉波括弧で終わらない場合に送らない事の返戻
		if(
			Syntax.GetRules().HasAutomaticLineTerminator || NextHead.Item == NoItem ||
			!Output.IsTailRole(Syntax, Role::ClosingBraceToken, GeneratedToken::BlockClose) || !Output.GetPreceding().DoesEndWithNewline
		) return false;
		// 後続が閉括弧かの返戻
		return IsEdgeBracket(Syntax, Output.GetStore(), NextHead, false);
	}

	/**
	 * 文の組立関数
	 * 最上位・ブロックの中の文を構造化し，独立した文でない末尾の区切を添える
	 * 計算量：部分木のノード数 N，テキスト長 B と補助照会費用 Q に対し O(N + B + Q)
	 * @param State 構築中の状態
	 * @param Id 文のノード
	 * @return 組立済の文
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::TakeStmt(Builder &State, const NodeId Id) {
		// 文の組立
		Emitter Output(State.IR);
		Output.Append(co_await TakeStructured(State, Id));
		AppendTrailingSeparator(State.Syntax, Id, Output);
		// 組立済の文の返戻
		co_return Output.Finish(NoNode, NoGroup);
	}

	/**
	 * 分割可能な統合宣言の要素の完了関数（行分割仕様 4.1）
	 * 共有部分・宣言子の区切・宣言間の改行・名前と初期化子の組を，行分割で宣言を区分する要素として記録する
	 * 端のテキストは１つの宣言の形で測る
	 * 計算量：宣言名と初期値の数 N に対し O(N) と出力要素の生成の総費用
	 * @param Syntax 構文モデル
	 * @param IR 構築中の中間表現
	 * @param Id 宣言
	 * @param Head 共有する接頭部
	 * @param Middle 共有する中間部（名前の並びと初期化子の並びの間）
	 * @param Tail 共有する終端部
	 * @param Names 宣言子毎の名前（宣言子を丸ごと並べる宣言は宣言子）
	 * @param Vals 宣言子毎の初期化子（初期化子の並びを持たない宣言は空）
	 * @return 組立済の統合宣言
	 * @throws FormatLimitExceeded 宣言子列又は元のノードが不正な場合（中間表現の不具合），又は作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	Canonicalizer::Built Canonicalizer::FinishDeclSplit(
		const SyntaxModel &Syntax,
		FormatIR &IR,
		const NodeId Id,
		Built &&Head,
		Built &&Middle,
		Built &&Tail,
		std::vector<Built> &&Names,
		std::vector<Built> &&Vals
	) {
		// 分割可能な統合宣言の要素の完了
		if(Id == NoNode || Names.empty() || !Vals.empty() && Vals.size() != Names.size()) {
			throw FormatLimitExceeded("the format IR has an invalid declaration split", FailureReason::FormatterDefect);
		}
		// 宣言子の間の区切と宣言の間の改行（行分割が区分に使う固定の要素）
		const std::array<FormatItemId, 2> Comma {
			GeneratedItem(Syntax, IR, GeneratedToken::ListSeparator),
			IR.Leaf(" ", FormatItemKind::Space)
		};
		std::vector<FormatItemId> Parts {
			GetItemOf(IR, Head),
			GetItemOf(IR, Middle),
			GetItemOf(IR, Tail),
			IR.Parent(FormatItemKind::Sequence, Comma),
			IR.Leaf({}, FormatItemKind::HardBreak)
		};
		Built Result;
		// 宣言の共有部分と端の要約の合成
		const auto Merge = [&Result, &Syntax](const Built &Part) -> void {
			Syntax.Charge(1);
			// 内容の無い部分の終了
			if(Part.Head.Item == NoItem) return;
			if(Result.Head.Item == NoItem) Result.Head = Part.Head;
			Result.BeforeTail = Part.BeforeTail.Item == NoItem ? Result.Tail : Part.BeforeTail;
			Result.Tail = Part.Tail;
			Result.HasBreak = Result.HasBreak || Part.HasBreak;
			if(Part.HasBreak || Part.HasCodeBoundary) Result.IsCodeOpenOnLine = Part.IsCodeOpenOnLine;
			Result.HasCodeBoundary = Result.HasCodeBoundary || Part.HasCodeBoundary;
			// 終了
			return;
		};
		Built Separator;
		Separator.Head = FindEdge(IR, Comma[0], true);
		Separator.Tail = FindEdge(IR, Comma[1], false);
		Separator.BeforeTail = Separator.Head;
		// 宣言子の番号と端の要約だけを記録し，使わない平坦候補を生成しない
		Merge(Head);
		for(size_t Idx = 0; Idx < Names.size(); ++Idx) {
			const std::array<FormatItemId, 2> Unit { GetItemOf(IR, Names[Idx]), Vals.empty() ? NoItem : GetItemOf(IR, Vals[Idx]) };
			Parts.push_back(IR.Parent(FormatItemKind::Sequence, Unit));
			if(Idx) Merge(Separator);
			Merge(Names[Idx]);
		}
		Merge(Middle);
		for(size_t Idx = 0; Idx < Vals.size(); ++Idx) {
			if(Idx) Merge(Separator);
			Merge(Vals[Idx]);
		}
		Merge(Tail);
		const FormatItemId Split = IR.Parent(FormatItemKind::DeclarationSplit, Parts);
		IR.Items[Split].Node = Id;
		Result.Item = Anchored(IR, Id, Split);
		// 組立済の統合宣言の返戻
		return Result;
	}

	/**
	 * 展開要因の包含の判定関数（整形仕様 6.3）
	 * 本体の文を持つブロック，必須展開の子を部分木に含むかを，構文だけから決める
	 * 計算量：部分木のノード数 V に対し O(V)（控えでノード毎に１回）
	 * @param State 構築中の状態
	 * @param Id ノード
	 * @return 展開要因を含めば true
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	bool Canonicalizer::HasExpansionReason(Builder &State, const NodeId Id) {
		// 子の結果を控える後行走査の準備
		const SyntaxModel &Syntax = State.Syntax;
		// 判定済の根は元の参照１回を計上し，作業列を作らず返す
		if(const uint8_t Entry = State.Contexts[Id].Bits; Entry & ContainsComputedBit) {
			Syntax.Charge(1);
			// 控えた展開要因の有無の返戻
			return Entry & ContainsBit;
		}
		std::vector<ExpansionFrame> &Stack = State.ExpansionStack;
		Stack.clear();
		Stack.push_back({ Id });
		bool IsFinalResult = false;
		while(!Stack.empty()) {
			Syntax.Charge(1);
			ExpansionFrame &Cur = Stack.back();
			const NodeId CurId = Cur.Id;
			const SyntaxNode &Node = Syntax.GetNode(CurId);
			const RoleSet *const Roles = Node.Kind.Def ? &Node.Kind.Def->Roles : nullptr;
			const auto Has = [Roles](const Role Wanted) -> bool {
				// 同じノードの固定定義から役割を照会する
				return Roles && Roles->test(static_cast<size_t>(Wanted));
			};
			if(!Cur.IsReady) {
				// 控えた結果とエラー領域の処理
				uint8_t &Entry = State.Contexts[CurId].Bits;
				if(
					Has(Role::Error) || Node.Facts.TokenSpacing != TokenSpacingMode::None || Node.Facts.DoesObserveText ||
					Node.Facts.DoesRequireInlineCode
				) {
					Entry |= ContainsComputedBit;
					Stack.pop_back();
					if(Stack.empty()) break;
					continue;
				}
				// 当該ノードの展開要因の判定
				Cur.IsReady = true;
				Syntax.Charge(1);
				const uint32_t ContentCount = State.Contexts[CurId].ContentCount;
				// 波括弧を持たない制御の本体と Ruby の１行のブロック
				const bool IsBraceLessControlBody =
				Has(Role::BodyWrapper) && !Syntax.IsTokenRole(Syntax.GetChild(CurId, 0), Role::OpeningBraceToken);
				NodeId ClosureBody = NoNode;
				if(Has(Role::ClosureBlock)) for(const NodeId Child : Node.Children) {
					Syntax.Charge(1);
					if(Syntax.Is(Child, Role::ClosureBody)) ClosureBody = Child;
				} else if(Has(Role::ClosureStmts)) ClosureBody = CurId;
				const bool IsInlineKeywordBody = Has(Role::AttachedArrowLambda) ||
				Has(Role::InlineBraceBody) && (ClosureBody == NoNode || GetBodyStmtCount(State, ClosureBody) < 2);
				// 本体を持たない単項の try・await 等は，例外処理の本体の展開要因にしない
				const bool IsPrefixVal = Has(Role::AwaitTryCastExpr) && !State.Contexts[CurId].HasBraceOpen &&
				Syntax.GetFirstNamedChildWithRole(CurId, Role::LexicalBlock) == NoNode &&
				Syntax.GetFirstNamedChildWithRole(CurId, Role::ValStmtSeq) == NoNode;
				// ドキュメントを同期する定義は，其のドキュメントを定義の直前の独立行へ置く為にコンテナを展開させる（整形仕様 6.3, 7.4）
				Cur.IsResult =
				State.Contexts[CurId].IsMemberSequence && (State.Contexts[CurId].HasStatementBody || State.Contexts[CurId].HasErrorChild) ||
				Has(Role::AccessSection) || SignatureExtractor::GetDocumentationFact(Syntax, CurId).IsDocumentable || Has(Role::IndentedBody) ||
				(
					// 列挙子列はインデント可能でも文の本体でない為，幅に依らず展開しない
					Has(Role::IndentContainer) && !Has(Role::EnumeratorSeq) &&
					!(Has(Role::ObjectLike) && (!Has(Role::TypeMemberBody) || State.Contexts[CurId].IsMemberSequence)) &&
					!Has(Role::NonKeywordClass) || Has(Role::EnumBodyDecls) || Has(Role::KeywordControlLayout) || Has(Role::SwitchBodyLayout) ||
					Has(Role::DoLoop)
				) && !IsBraceLessControlBody && !IsInlineKeywordBody && !IsPrefixVal && ContentCount &&
				(!Has(Role::KeywordBlock) || ShouldExpandKeywordBody(State, CurId)) || Has(Role::DeferredLiteralBody) ||
				Has(Role::BracedConditionalVal) && std::ranges::any_of(
					Node.Children,
					// 展開を要する子を見付ける述語
					[&Syntax](const NodeId Child) -> bool {
						Syntax.Charge(1);
						// else の語かの返戻
						return Syntax.Is(Child, Role::ElseKeyword);
					}
				) && !IsArrowValBody(State, CurId) || (Has(Role::DirectBraceLayout) || Has(Role::DirectControlBraceLayout)) &&
				Syntax.GetFirstNamedChildWithRole(CurId, Role::ValStmtSeq) != NoNode;
				if(!ContentCount) Cur.Next = static_cast<uint32_t>(Node.Children.size());
			}
			// 未確定の子の探索と結果の控え
			if(const std::span<const NodeId> Children = Node.Children; !Cur.IsResult && Cur.Next < Children.size()) {
				if(const NodeId Child = Children[Cur.Next++]; Syntax.IsNamed(Child) || Syntax.Is(Child, Role::EmptyStmt)) {
					if(const uint8_t Entry = State.Contexts[Child].Bits; Entry & ContainsComputedBit) {
						Syntax.Charge(1);
						Cur.IsResult = Entry & ContainsBit;
					} else Stack.push_back({ Child });
				}
				continue;
			}
			const bool IsResult = Cur.IsResult;
			State.Contexts[CurId].Bits |= static_cast<uint8_t>(ContainsComputedBit | (IsResult ? ContainsBit : 0));
			Stack.pop_back();
			if(Stack.empty()) {
				IsFinalResult = IsResult;
				break;
			}
			if(IsResult) Stack.back().IsResult = true;
		}
		// 展開要因の有無の返戻
		return IsFinalResult;
	}

	/**
	 * 改行を跨いで前の文へ繋がる文の判定関数
	 * Kotlin, Swift は改行の後の `{` を前の式の末尾ラムダとして読み，Kotlin のパーサーは改行の後の `(` `[` `::` も前の式へ繋ぐ
	 * @param Syntax 構文モデル
	 * @param IR 中間表現
	 * @param Head 次の文の先頭の要素
	 * @return 改行では区切れず `;` が要るなら true
	 */
	bool Canonicalizer::CanJoinAcrossNewline(const SyntaxModel &Syntax, const FormatIR &IR, const EdgeItem Head) {
		// 空の文は繋がらない事の返戻
		if(Head.Item == NoItem) return false;
		// 次の文の始まりが前の式へ繋がるかの返戻
		return Syntax.GetRules().CanContinueWithBrace &&
		IsEdgeRole(Syntax, IR, Head, Role::OpeningBraceToken, GeneratedToken::BlockOpen) || Syntax.GetRules().CanContinueWithCall && (
			IsEdgeRole(Syntax, IR, Head, Role::OpeningParenToken, GeneratedToken::CallOpen) ||
			IsEdgeRole(Syntax, IR, Head, Role::OpeningBracketToken) || IsEdgeRole(Syntax, IR, Head, Role::DoubleColonToken)
		);
	}

	/**
	 * 本文を控えたヒアドキュメントの後の文の区切の追加関数
	 * 開始の後の文は同じ行へ `;` で区切って続ける
	 * 計算量：生成する区切記法のテキスト長 B に対し O(B)
	 * @param Syntax 読取専用の期待構文
	 * @param Output 組立中の出力
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::AppendHeredocPendingSeparator(const SyntaxModel &Syntax, Emitter &Output) {
		// 本文を控えたヒアドキュメントの後の文の区切の追加
		Output.OmitSpace();
		if(!Output.IsEmpty() && !Output.IsTailRole(Syntax, Role::StmtTerminatorToken, GeneratedToken::StmtTerminator)) {
			Output.GetText(Syntax, GeneratedToken::StmtTerminator);
		}
		Output.Space();
		// 終了
		return;
	}

	/**
	 * PHP の代替構文の本体を開く `:` かの判定関数
	 * @param Syntax 構文モデル
	 * @param Parent `:` の親
	 * @param Colon `:` のトークン
	 * @return 本体を開く `:` なら true
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	bool Canonicalizer::IsAlternateBodyOpener(const SyntaxModel &Syntax, const NodeId Parent, const NodeId Colon) {
		// 代替構文の本体を持つ親の `:` かの返戻（先頭の `:` だけを開きとする親は先頭の子に限る）
		return Syntax.Is(Parent, Role::ColonBodyHost) && (!Syntax.Is(Parent, Role::LeadingColonBody) || !Syntax.GetNode(Colon).Idx);
	}

	/**
	 * PHP の代替構文の本体で終わるかの判定関数
	 * 計算量：O(1)
	 * @param State 構築中の状態（名前付の子の要約を持つ）
	 * @param Id ノード
	 * @return 代替構文の本体其の物か，其れで終わる節なら true
	 * @throws FormatLimitExceeded 子の要約の参照が作業予算を超える場合
	 */
	bool Canonicalizer::HasTrailingColonBody(const Builder &State, const NodeId Id) {
		// PHP の代替構文の本体で終わるかの判定
		const SyntaxModel &Syntax = State.Syntax;
		// 名前付でないノードの返戻
		if(!Syntax.IsNamed(Id)) return false;
		// 代替構文の本体其の物の返戻
		if(Syntax.Is(Id, Role::ColonBody)) return true;
		Syntax.Charge(1);
		const NodeCtx &Ctx = State.Contexts[Id];
		// 最後の名前付の子が代替構文の本体かの返戻
		return Ctx.NamedCount && Syntax.Is(Syntax.GetChild(Id, Ctx.LastNamed), Role::ColonBody);
	}

	/**
	 * 構造化の控えの取得関数
	 * 計算量：初回に訪問する構文・生成要素の延数 N とテキストの総長 B に対し期待値 O(N + B)，控えの取得は期待値 O(1)
	 * @param State 構築中の状態
	 * @param Id ノード
	 * @return 控えた構造化の要素
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	Canonicalizer::Pending<const Canonicalizer::Built *> Canonicalizer::GetStructured(Builder &State, const NodeId Id) {
		// 構造化の控えの取得
		if(const BuiltSlot *const Slot = State.Contexts[Id].Structured) co_return &Slot->Value;
		// 控えへ入れた構造化の要素の返戻
		co_return StoreStructured(State, Id, co_await BuildStructured(State, Id));
	}

	/**
	 * 展開形のコンテナの組立関数
	 * 開きの後・要素の区切の後で改行し，閉じを独自の行に置く（Go の複数行の並びは閉じの前に末尾のカンマを補う）
	 * 計算量：直接の子数 C に対し O(C) と子の組立・末尾判定・出力生成の総費用
	 * @param State 構築中の状態
	 * @param Id コンテナ
	 * @return 組立済の展開形
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildExpandedContainer(Builder &State, const NodeId Id) {
		// 展開するコンテナの子の順次配置
		const SyntaxModel &Syntax = State.Syntax;
		Emitter Output(State.IR);
		NodeId Left = NoNode;
		uint32_t OpenHeredocs = 0;
		for(const NodeId Child : Syntax.GetChildrenOf(Id)) {
			if(Syntax.Is(Child, Role::DeferredLiteralBody)) {
				Output.OmitSpace();
				Output.Append(co_await BuildHeredocBody(State, Child));
				TrackOpenHeredocs(State, Child, OpenHeredocs);
				Left = Child;
				continue;
			}
			// ヒアドキュメントの本文を控えた開始行の続きは折らない（行分割仕様第３章）
			Built Piece;
			if(!Syntax.IsNamed(Child)) Piece = BuildLeaf(Syntax, State.IR, Child);
			else if(OpenHeredocs) Piece = co_await BuildSealedFlat(State, Child);
			else Piece = co_await TakeStructured(State, Child);
			if(Piece.Head.Item == NoItem) continue;
			const bool IsSingle = Piece.Head.Item == Piece.Tail.Item;
			const bool IsOpen = IsSingle && IsEdgeBracket(Syntax, State.IR, Piece.Head, true);
			const bool IsClose = IsSingle && IsEdgeBracket(Syntax, State.IR, Piece.Head, false);
			const bool IsComma = IsSingle && IsEdgeRole(Syntax, State.IR, Piece.Head, Role::CommaToken, GeneratedToken::ListSeparator);
			Output.OmitSpace();
			if(IsClose) Output.CloseScope();
			// Go の複数行の並びは閉じの前に末尾のカンマが要り，改行の前に補って構文を保つ
			if(IsClose && TrailingCommaForm(Syntax, Id, Left, Child, true)) {
				Output.GetText(Syntax, GeneratedToken::ListSeparator);
				State.IR.EditDetail(Output.GetLast().Item).PunctuationOwner = Id;
			}
			if(ShouldBreakAfterExpandedBrace(Syntax, Piece.Head, Output) || IsClose && !Output.IsEmpty() && !Output.IsAtLineStart()) {
				Output.Break();
			} else if(!IsOpen && !IsClose && !IsComma && !Output.IsEmpty() && !Output.IsAtLineStart()) Output.Space();
			Output.Append(std::move(Piece));
			Left = Child;
			TrackOpenHeredocs(State, Child, OpenHeredocs);
			if(IsOpen) Output.OpenScope();
			if(!OpenHeredocs && (IsOpen || IsComma)) Output.Break();
		}
		// 展開形の返戻
		co_return Output.Finish(Id, NoGroup);
	}

	/**
	 * プリプロセッサディレクティブの条件のブロックの組立関数
	 * ディレクティブの行と本体の宣言を行毎に置き，空行は収集した規則に従う
	 * 計算量：辿る子の総数 V に対し O(V)
	 * @param State 構築中の状態
	 * @param Id プリプロセッサディレクティブのブロック
	 * @return 組立済のブロック
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildPreprocBlock(Builder &State, const NodeId Id) {
		// ネストの条件のブロックを辿る作業枠の準備
		const SyntaxModel &Syntax = State.Syntax;
		// プリプロセッサディレクティブの条件のブロックの走査の作業枠
		struct Frame {
			NodeId Id;
			Emitter Out;
			size_t Next = 0;
			NodeId PendingChild = NoNode; // 子の組立の完了を待つノード
			bool HasPrev = false;
			bool IsAwaitingCond = false; // 次の部品が条件の見出か
		};
		std::vector<Frame> Stack;
		Stack.push_back({ Id, Emitter(State.IR) });
		Built Completed;
		// 作業枠毎のディレクティブと子の配置
		while(!Stack.empty()) {
			Frame &Cur = Stack.back();
			if(Cur.PendingChild != NoNode) {
				AppendChild(State, Cur.PendingChild, std::move(Completed), Cur.Out);
				Cur.PendingChild = NoNode;
				Cur.HasPrev = true;
				continue;
			}
			const std::span<const NodeId> Children = Syntax.GetChildrenOf(Cur.Id);
			if(Cur.Next == Children.size()) {
				Completed = Cur.Out.Finish(Cur.Id, NoGroup);
				Stack.pop_back();
				continue;
			}
			const NodeId Child = Children[Cur.Next++];
			Emitter &Output = Cur.Out;
			bool &HasPrev = Cur.HasPrev, &IsAwaitingCond = Cur.IsAwaitingCond;
			if(!Syntax.IsNamed(Child)) {
				// 空白だけの無名のトークンの読飛
				if(
					const std::string &Text = Syntax.GetNode(Child).Lexeme;
					!Text.empty() && Text.find_first_not_of(" \t\n\r") == std::string::npos
				) continue;
				if(Syntax.Is(Child, Role::PreprocDirectiveOpen) || Syntax.Is(Child, Role::PreprocDirectiveClose)) {
					if(HasPrev) Output.Break();
					AppendChild(State, Child, BuildLeaf(Syntax, State.IR, Child), Output);
					HasPrev = Syntax.Is(Child, Role::PreprocDirectiveClose);
					IsAwaitingCond = Syntax.Is(Child, Role::PreprocDirectiveOpen);
					continue;
				}
				Output.Append(BuildLeaf(Syntax, State.IR, Child));
				continue;
			}
			// ディレクティブの条件式は同じ行へ続け，行末の改行がディレクティブを終える為に内部を折らない
			if(IsAwaitingCond) {
				Output.Space();
				Output.Append(co_await BuildSealedFlat(State, Child));
				IsAwaitingCond = false;
				HasPrev = true;
				continue;
			}
			if(HasPrev) Output.Break();
			if(Syntax.Is(Child, Role::PreprocBranch)) {
				Cur.PendingChild = Child;
				Stack.push_back({ Child, Emitter(State.IR) });
				continue;
			}
			AppendChild(State, Child, co_await TakeStmt(State, Child), Output);
			HasPrev = true;
		}
		// 組立済のブロックの返戻
		co_return Completed;
	}

	/**
	 * カンマで並べる統合宣言の組立関数（整形仕様 5.6，行分割仕様 4.1）
	 * 最初の要素の前の共有する子を接頭部，名前と初期化子の間の共有する子を中間部，最後の要素の後の共有する子を終端部とし，宣言子（又は名前と初期化子の組）を区分の単位にする
	 * 子の間の空白は同じ親の子へ遡った組の構文上の役割で決め，要素の中の子の境界は並びのノードの折返の候補をグループにする
	 * 計算量：宣言の部品数 P，列数 C，最大の祖先数 D に対し O((P + C) D) と子の組立・境界判定・出力生成の総費用
	 * @param State 構築中の状態
	 * @param Id 宣言子又は名前の数が >1 の宣言
	 * @param Shape 呼出側が抽出し，組立の完了迄保持する統合宣言の形
	 * @return 組立済の要素
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildCommaDecl(
		Builder &State,
		const NodeId Id,
		const CommaDecl &Shape
	) {
		// カンマで並べる統合宣言の組立
		const SyntaxModel &Syntax = State.Syntax;
		const NodeId Host = Shape.Lists.front().Host;
		// ノードの宣言からの深さ
		const auto Depth = [&Syntax, Id](NodeId Node) -> uint32_t {
			uint32_t Result = 0;
			for(; Node != Id; Node = Syntax.GetParentOf(Node)) ++Result;
			// 深さの返戻
			return Result;
		};
		std::unordered_map<NodeId, std::pmr::vector<uint8_t>> Candidates; // 並びと共有部分の親毎に，子の間の折返候補を１度だけ求める
		// 共通の親に属する子の組に依る空白と折返候補の判定
		const auto BoundaryOf = [&](NodeId Left, NodeId Right) -> std::pair<bool, bool> {
			// 両ノードの共通の親に属する子の組の取得
			uint32_t LeftDepth = Depth(Left), RightDepth = Depth(Right);
			for(; LeftDepth > RightDepth; --LeftDepth) Left = Syntax.GetParentOf(Left);
			for(; RightDepth > LeftDepth; --RightDepth) Right = Syntax.GetParentOf(Right);
			NodeId LeftParent = Syntax.GetParentOf(Left), RightParent = Syntax.GetParentOf(Right);
			while(LeftParent != RightParent) {
				Left = LeftParent;
				Right = RightParent;
				LeftParent = Syntax.GetParentOf(Left);
				RightParent = Syntax.GetParentOf(Right);
			}
			Syntax.Charge(1);
			std::unordered_map<NodeId, std::pmr::vector<uint8_t>>::iterator Found = Candidates.find(RightParent);
			if(Found == Candidates.end()) Found = Candidates.emplace(RightParent, BreakBoundaries(State, RightParent)).first;
			// 兄弟の組の空白と折返候補の返戻
			return {
				ShouldInsertSpace(
					State,
					RightParent,
					Left,
					Right,
					Syntax.GetNode(RightParent).Facts.GetBoundaryBefore(Syntax.GetNode(Right).Idx)
				),
				!Found->second.empty() && Found->second[Syntax.GetNode(Right).Idx]
			};
		};
		// 宣言の外の接頭部，並びのノードの中の接頭部，中間部，終端部と，並び毎の要素
		Emitter Outer(State.IR), Inner(State.IR), Middle(State.IR), Tail(State.IR);
		std::vector<std::vector<Emitter>> Rows;
		for(const DeclList &List : Shape.Lists) Rows.emplace_back(List.Items.size(), Emitter(State.IR));
		Emitter *LastShared = nullptr;
		NodeId Prev = NoNode;
		// 最後に要素を置いた並び（無ければ -1）
		int32_t LastList = -1;
		// 共有部分の構文上の所属と既配置の並びに従うビルダーの選択
		const auto SharedEmitterOf = [&](const NodeId Node) -> Emitter & {
			// 配置済の並びの後の中間部又は終端部の返戻
			if(LastList > -1) return static_cast<size_t>(LastList) + 1 < Shape.Lists.size() ? Middle : Tail;
			// 並びの内側又は外側の接頭部の返戻
			return Host != Id && Syntax.GetParentOf(Node) == Host ? Inner : Outer;
		};
		// 宣言の共有部分と各要素への子の振分
		for(const DeclPiece &Piece : Shape.Pieces) {
			const bool IsShared = Piece.List < 0;
			Emitter &Output = !IsShared ? Rows[static_cast<size_t>(Piece.List)][Piece.Item] : SharedEmitterOf(Piece.Node);
			// 共有する子の後の境界は其の領域の末尾，要素の中は其の要素，其の他は此の領域の中（共有部分の後でない別の要素の頭は区切が出す為に判定しない）
			std::pair<bool, bool> Boundary;
			if(Prev != NoNode && (IsShared || !Output.IsEmpty() || LastShared)) Boundary = BoundaryOf(Prev, Piece.Node);
			const auto [HasSpace, IsCandidate] = Boundary;
			if(!IsShared && !Output.IsEmpty()) {
				if(IsCandidate) Output.Soft(HasSpace ? " " : "");
				else if(HasSpace) Output.Space();
			} else if(LastShared || IsShared) {
				Emitter &BoundaryOutput = LastShared ? *LastShared : Output;
				if(IsCandidate) BoundaryOutput.Soft(HasSpace ? " " : "");
				else if(HasSpace) BoundaryOutput.Space();
			}
			if(Syntax.IsNamed(Piece.Node)) Output.Append(co_await TakeStructured(State, Piece.Node));
			else Output.Append(BuildLeaf(Syntax, State.IR, Piece.Node));
			LastShared = IsShared ? &Output : nullptr;
			if(!IsShared) LastList = Piece.List;
			Prev = Piece.Node;
		}
		// 並びの接頭部又は中間部へのアンカーと，初期化子の並びの中間部へのアンカーの配置
		const bool HasInner = !Inner.IsEmpty();
		Built Shared = Middle.Finish(NoNode, State.NextGroup++);
		const FormatItemId SharedItem = GetItemOf(State.IR, Shared);
		const uint32_t SharedGroup = SharedItem == NoItem ? NoGroup : State.IR.Items[SharedItem].Group;
		if(Shape.Lists.size() > 1) {
			if(SharedGroup != NoGroup) State.IR.Items[SharedItem].Node = Shape.Lists.back().Host;
			Shared.Item = Anchored(State.IR, Shape.Lists.back().Host, SharedItem);
		}
		Built Type = Inner.Finish(NoNode, State.NextGroup++);
		if(Host != Id) (HasInner ? Type : Shared).Item = Anchored(State.IR, Host, GetItemOf(State.IR, HasInner ? Type : Shared));
		Outer.Append(std::move(Type));
		std::vector<Built> Names, Vals;
		for(Emitter &Row : Rows.front()) Names.push_back(Row.Finish(NoNode, State.NextGroup++));
		if(Rows.size() > 1) for(Emitter &Row : Rows.back()) {
			Built Val = Row.Finish(NoNode, State.NextGroup++);
			// 代入の境界と右辺を同じ外側のグループへ所属させ，右辺の子を先に評価する
			if(SharedGroup != NoGroup) {
				const std::array<FormatItemId, 1> Parts { GetItemOf(State.IR, Val) };
				Val.Item = State.IR.Parent(FormatItemKind::Group, Parts);
				State.IR.Items[Val.Item].Group = SharedGroup;
			}
			Vals.push_back(std::move(Val));
		}
		Built Prefix = Outer.Finish(NoNode, State.NextGroup++), Suffix = Tail.Finish(NoNode, State.NextGroup++);
		// 組立済の統合宣言の返戻
		co_return FinishDeclSplit(
			Syntax,
			State.IR,
			Id,
			std::move(Prefix),
			std::move(Shared),
			std::move(Suffix),
			std::move(Names),
			std::move(Vals)
		);
	}

	/**
	 * 波括弧を行幅で付ける単一の文の制御本体かの判定関数（整形仕様 6.1）
	 * 計算量：本体の子の数 C に対し O(C)
	 * @param State 構築中の状態
	 * @param Form 親が選べる波括弧の生成形式
	 * @param Parent 制御構文
	 * @param Child 子
	 * @return 波括弧のブロックの生成形式を持つ構文の if の枝・繰返の本体で，ブロックでも空文でもない文なら true
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 */
	bool Canonicalizer::IsBraceOptionalBody(
		const Builder &State,
		const CanonicalForm &Form,
		const NodeId Parent,
		const NodeId Child
	) {
		// 波括弧を行幅で付ける単一の文の制御本体の判定
		const SyntaxModel &Syntax = State.Syntax;
		// 生成形式がラッパーを許さない本体と，名前付でないトークン・空文・区切を持つ本体を除く事の返戻
		if(
			!Form.CanWrapBody(Syntax.IsBracedBody(Child)) || !Syntax.IsNamed(Child) || Syntax.Is(Child, Role::EmptyStmt) ||
			Syntax.Is(Child, Role::DelimitedBody)
		) return false;
		// 本体の包み（場を持たない文法の本体の位置）か
		const bool IsContainer = Syntax.Is(Child, Role::BodyWrapper);
		if(Syntax.Is(Parent, Role::If)) {
			// `alternative` の else 節は節全体でなく其の中の文を包む為，節を包まない事の返戻
			if(Syntax.Is(Child, Role::ElseClause) || Syntax.Is(Child, Role::ElseIfClause)) return false;
			const bool IsAlternative = Child == Syntax.GetFieldChild(Parent, ChildRole::Alternative) ||
			IsContainer && Syntax.Is(Syntax.GetPrevSibling(Child), Role::ElseKeyword);
			// if の真枝と，else if の連鎖でない後続の枝かの返戻
			return (IsContainer || IsAlternative || Child == Syntax.GetIfConsequence(Parent)) &&
			!(IsAlternative && Syntax.Is(IsContainer ? Syntax.GetChild(Child, State.Contexts[Child].FirstNamed) : Child, Role::If));
		}
		// else if の連鎖をブロックで分断しない為に包まない事の返戻
		if(Syntax.Is(Parent, Role::ElseClause) && Syntax.Is(Child, Role::If)) return false;
		// 繰返の本体かの返戻
		return Syntax.Is(Parent, Role::BodyOwner) && (
			IsContainer || Child == Syntax.GetFieldChild(Parent, ChildRole::Body) ||
			Syntax.Is(Parent, Role::ElseClause) && Child == Syntax.GetChild(Parent, State.Contexts[Parent].FirstNamed)
		);
	}

	/**
	 * 行幅で波括弧を付ける制御本体のグループの組立関数（整形仕様 6.1）
	 * 平坦時は見出と同じ行の単一の文，展開時は波括弧で包んだ本体のグループとする（本体が複数行に為れば展開する）
	 * 本体が繰返・分岐・切替の連鎖なら，連鎖内の本体の複数行だけでは展開しない
	 * 後続の `else`, `while` は展開時の閉波括弧と同じ行へ置く
	 * 計算量：連鎖グループの登録費用 T に対し O(T) と出力要素の生成の総費用
	 * @param State 構築中の状態
	 * @param Parent 制御構文
	 * @param Child 本体の文
	 * @param Body 本体の組立済の要素
	 * @return 組立済のグループ
	 * @throws FormatLimitExceeded 本体グループの構築が作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	Canonicalizer::Built Canonicalizer::BuildBraceGroup(Builder &State, const NodeId Parent, const NodeId Child, Built &&Body) {
		// 行幅で波括弧を付ける制御本体のグループの組立
		const SyntaxModel &Syntax = State.Syntax;
		// 波括弧を持たない本体の包みは，中の文が制御見出の連鎖かを判ずる
		const NodeId Stmt = Syntax.Is(Child, Role::BodyWrapper) ? Syntax.GetChild(Child, State.Contexts[Child].FirstNamed) : Child;
		const bool IsChain = Syntax.Is(Stmt, Role::Loop) && !Syntax.Is(Stmt, Role::DoLoop) || Syntax.Is(Stmt, Role::If) ||
		Syntax.Is(Stmt, Role::SwitchStmtLike) || Syntax.Is(Stmt, Role::SwitchVal);
		// 後続の枝の前の本体か
		const NodeId NextSibling = Syntax.GetNextSibling(Child);
		const bool IsBeforeClause = Syntax.Is(NextSibling, Role::ElseKeyword) || Syntax.Is(NextSibling, Role::ElseClause) ||
		Syntax.Is(NextSibling, Role::ElseIfClause) || Syntax.Is(Parent, Role::DoLoop) || State.Contexts[Child].HasFollowingClause;
		// 平坦時も else を同じ行へ続ける `->` の後の値の if 式か（次の行の `else` は when の枝と読まれる）
		const bool IsArrowBody = IsArrowValBody(State, Parent), IsSameLineClause = IsBeforeClause && IsArrowBody;
		FormatIR &IR = State.IR;
		Emitter Output(IR);
		Output.IfBroken({ GeneratedItem(Syntax, IR, GeneratedToken::BlockOpen) });
		Output.OpenScope();
		Output.Soft("");
		const bool ShouldJoinFlatClause = IsBeforeClause && !IsSameLineClause && IsBlockClose(Syntax, IR, Body.Tail);
		Output.Append(std::move(Body));
		Output.CloseScope();
		// `->` の後の値の if 式の連鎖は，全ての枝のラッパーを連鎖の根の１つのグループで揃える（行分割仕様 4.2）
		uint32_t Group = State.NextGroup;
		if(IsArrowBody) {
			uint32_t &Stored = State.Contexts[State.Contexts[Parent].ArrowChainRoot].ChainGroup;
			if(Stored == NoGroup) Stored = Group;
			Group = Stored;
		}
		if(Group == State.NextGroup) ++State.NextGroup;
		// 先行する分岐の見出で連鎖を分けた時は，後続の連鎖も同じ分岐深さへ配置
		uint32_t Dependency = NoGroup;
		if(IsChain && Syntax.Is(Parent, Role::DependentBranchLayout)) {
			if(Syntax.Is(Parent, Role::If) && Syntax.GetIfConsequence(Parent) == Child) State.Contexts[Parent].ChainGroup = Group;
			if(Syntax.Is(Parent, Role::ElseClause)) if(const NodeId Owner = Syntax.GetParentOf(Parent); Owner != NoNode) {
				Dependency = State.Contexts[Owner].ChainGroup;
			}
		}
		// 本体の複数行を展開の要因にする末尾の候補（連鎖の中の本体の複数行は除く）
		if(IsSameLineClause) {
			// 平坦時の本体の後と展開時の閉じの後への，後続節の区切空白の配置
			Output.Soft("");
			Output.IfBroken({ GeneratedItem(Syntax, IR, GeneratedToken::BlockClose) });
			Output.Space();
			Built Result = Output.Finish(NoNode, Group);
			IR.Items[Result.Item].Node = Child;
			IR.Items[Result.Item].Operation = FormOperation::BraceBody;
			IR.EditDetail(Result.Item).BodyParent = Parent;
			IR.EditDetail(Result.Item).IsChainBody = IsChain;
			Result.HasClauseBoundary = IsBeforeClause;
			// グループの返戻
			return Result;
		}
		if(!IsChain) Output.Soft("");
		// 後続節を持たない本体の閉じの追加
		const auto AppendFinalClose = [&]() -> void {
			// 見出の連鎖の閉じ，又は単独の本体の閉じの追加
			if(IsChain) Output.IfBroken({ IR.Leaf({}, FormatItemKind::HardBreak), GeneratedItem(Syntax, IR, GeneratedToken::BlockClose) });
			else Output.IfBroken({ GeneratedItem(Syntax, IR, GeneratedToken::BlockClose) });
			// 終了
			return;
		};
		// 後続節の有無と平坦形の閉じに従う境界の選択
		if(!IsBeforeClause) AppendFinalClose();
		else if(ShouldJoinFlatClause) {
			Output.IfBroken({ IR.Leaf({}, FormatItemKind::HardBreak), GeneratedItem(Syntax, IR, GeneratedToken::BlockClose) });
			Output.Space();
		} else {
			// 最内の本体が平坦の時だけ節を別行へ置き，包んだ閉じへは空白で接続
			if(!Body.HasClauseBoundary) Output.IfBroken({ IR.Leaf({}, FormatItemKind::HardBreak) }, true);
			if(IsChain) Output.IfBroken({ IR.Leaf({}, FormatItemKind::HardBreak) });
			Output.IfBroken({ GeneratedItem(Syntax, IR, GeneratedToken::BlockClose), IR.Leaf(" ", FormatItemKind::Space) });
		}
		Built Result = Output.Finish(NoNode, Group);
		IR.Items[Result.Item].Node = Child;
		IR.Items[Result.Item].Operation = FormOperation::BraceBody;
		IR.EditDetail(Result.Item).BodyParent = Parent;
		IR.EditDetail(Result.Item).IsChainBody = IsChain;
		IR.EditDetail(Result.Item).ExpansionDependency = Dependency;
		Result.HasClauseBoundary = IsBeforeClause;
		// グループの返戻
		return Result;
	}
} // namespace Core
