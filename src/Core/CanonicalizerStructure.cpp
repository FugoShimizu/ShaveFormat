#include "Canonicalizer.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include "SignatureExtractor.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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
			ChildRole::Condition,
			ChildRole::Exceptions,
			ChildRole::Guard,
			ChildRole::Name,
			ChildRole::Object,
			ChildRole::Parameters,
			ChildRole::Pattern,
			ChildRole::Superclass,
			ChildRole::Value,
			ChildRole::Variable
		};
		// 見出の部品である事の返戻
		for(const ChildRole Field : HeaderFields) if(Syntax.HasField(Child, Field)) return true;
		// 見出の部品でない事の返戻
		return false;
	}

	/**
	 * 本体の文の数の取得関数（整形仕様 6.3）
	 * 「本体を展開するか」は何処でも此の数で決める為，数え方を此処だけで定める
	 * @param Syntax 構文モデル
	 * @param Id 節点
	 * @param Limit 数える上限（越えた分は数えずに打ち切る）
	 * @return 本体の文の数（上限で頭打ち）
	 */
	size_t Canonicalizer::BodyStatementCount(const SyntaxModel &Syntax, const NodeId Id, const size_t Limit) {
		size_t Count = 0;
		ForEachStatementOf(
			Syntax,
			Id,
			[&](const NodeId Child) -> bool {
				if(IsHeaderField(Syntax, Child)) return true;
				return ++Count < Limit;
			}
		);
		// 本体の文の数の返戻
		return Count;
	}

	/**
	 * 本体の文を持つかの判定関数
	 * @param Syntax 構文モデル
	 * @param Id 節点
	 * @return 本体の文を一つでも持てば true
	 */
	bool Canonicalizer::HasBodyStatement(const SyntaxModel &Syntax, const NodeId Id) {
		// 本体の文の有無の返戻
		return BodyStatementCount(Syntax, Id, 1);
	}

	/**
	 * Ruby の展開を続けるかの判定関数（整形仕様 1.4 の「空の本体は畳む」の裏返し）
	 * @param Syntax 構文モデル
	 * @param Id 節点
	 * @return 本体の文が在るか畳むと構文誤りに為れば true
	 */
	bool Canonicalizer::KeepsKeywordBodyExpanded(const SyntaxModel &Syntax, const NodeId Id) {
		// 展開を続けるかの返戻
		return HasBodyStatement(Syntax, Id) || Syntax.NeedsSeparatorToClose(Id);
	}

	/**
	 * JSX の内容の子の数の取得関数
	 * 空白だけの本文と空の式を数えず，全ての呼出側が「２以上か」だけを問う為に２で打ち切る
	 * @param Syntax 構文モデル
	 * @param Id JSX の要素
	 * @return 内容の子の数（上限２）
	 */
	uint32_t Canonicalizer::RenderedContentCount(const SyntaxModel &Syntax, const NodeId Id) {
		uint32_t Count = 0;
		bool IsInText = false, IsTextCounted = false;
		for(const NodeId Child : Syntax.ChildrenOf(Id)) {
			if(!Syntax.IsNamed(Child)) continue;
			const bool IsText = Syntax.Is(Child, Role::RenderedText);
			if(!IsText || !IsInText) IsTextCounted = false;
			IsInText = IsText;
			const std::string Text = Syntax.Is(Child, Role::ElementText) ? Syntax.Text(Child) : std::string();
			const bool IsBlank = Syntax.Is(Child, Role::ElementText) && Text.find_first_not_of(" \t\n") == std::string::npos;
			if(
				!IsTextCounted && !(
					Syntax.Is(Child, Role::ContentTagBoundary) || IsBlank ||
					Syntax.Is(Child, Role::EmbeddedExpression) && !Syntax.NamedChildCount(Child)
				)
			) {
				++Count;
				IsTextCounted = IsText;
			}
			// ２到達で閾値の判定が確定する事の返戻
			if(Count > 1) return Count;
		}
		// 内容の子の数の返戻
		return Count;
	}

	/**
	 * Kotlin の `->` の後の値の if 式の一括判定関数（`else` を次の行へ置くと解析器が `else "b"` を when の枝と読み違える）
	 * else if の連鎖の内側の if 式は，先に巡った連鎖の外の if 式の判定を引き継ぐ
	 * @param State 構築中の状態
	 * @param Top 巡る部分木の根
	 */
	void Canonicalizer::MarkArrowValueBodies(Builder &State, const NodeId Top) {
		const SyntaxModel &Syntax = State.Syntax;
		std::vector<NodeId> Stack { Top };
		while(!Stack.empty()) {
			const NodeId Node = Stack.back();
			Stack.pop_back();
			if(Syntax.Is(Node, Role::BracedConditionalValue)) {
				const NodeId Parent = Syntax.ParentOf(Node), Host = Syntax.ParentOf(Parent);
				const bool IsOuterArrow = Host != NoNode && State.Bits[Host] & ArrowValueBit;
				if(
					Syntax.Is(Parent, Role::BodyWrapper) &&
					(Syntax.Is(Host, Role::ConditionalValue) ? IsOuterArrow : Syntax.Is(Host, Role::ValueArm)) ||
					Syntax.Is(Parent, Role::ValueStatementSequence) && Syntax.Is(Host, Role::LambdaLiteral) &&
					BodyStatementCount(Syntax, Parent, 2) == 1
				) State.Bits[Node] |= ArrowValueBit;
			}
			const std::span<const NodeId> Children = Syntax.ChildrenOf(Node);
			Stack.insert(Stack.end(), Children.rbegin(), Children.rend());
		}
		// 終了
		return;
	}

	/**
	 * Kotlin の `->` の後の値の if 式かの判定関数
	 * @param State 構築中の状態（`MarkArrowValueBodies` が印を書いた物）
	 * @param Id if 式
	 * @return when の枝・ラムダの値の if 式なら true
	 */
	bool Canonicalizer::IsArrowValueBody(const Builder &State, const NodeId Id) {
		const std::unordered_map<NodeId, uint8_t>::const_iterator Found = State.Bits.find(Id);
		// 印の有無の返戻
		return Found != State.Bits.end() && Found->second & ArrowValueBit;
	}

	/**
	 * 展開要因の包含の判定関数（整形仕様 6.3）
	 * 本体の文を持つ塊，複数の内容を持つ JSX 要素等を部分木に含むかを，構文だけから決める
	 * 計算量：部分木の節点数 V に対し O(V)（控えで節点毎に１回）
	 * @param State 構築中の状態
	 * @param Id 節点
	 * @return 展開要因を含めば true
	 */
	bool Canonicalizer::ContainsStmtBlock(Builder &State, const NodeId Id) {
		// 子の結果を控える後行走査の準備
		const SyntaxModel &Syntax = State.Syntax;
		struct Frame {
			NodeId Id;
			uint32_t Next = 0;
			bool IsReady = false;
			bool IsResult = false;
		};
		std::vector<Frame> Stack { { Id } };
		bool FinalResult = false;
		while(!Stack.empty()) {
			Frame &Current = Stack.back();
			const NodeId CurrentId = Current.Id;
			if(!Current.IsReady) {
				// 控えた結果と誤り区画の処理
				uint8_t &Entry = State.Bits[CurrentId];
				if(Entry & ContainsComputedBit) {
					const bool Result = Entry & ContainsBit;
					Stack.pop_back();
					if(Stack.empty()) {
						FinalResult = Result;
						break;
					}
					if(Result) Stack.back().IsResult = true;
					continue;
				}
				if(Syntax.Is(CurrentId, Role::Error)) {
					Entry |= ContainsComputedBit;
					Stack.pop_back();
					if(Stack.empty()) break;
					continue;
				}
				// 当該節点の展開要因の判定
				Current.IsReady = true;
				const uint32_t NamedCount = Syntax.NamedChildCount(CurrentId);
				// 波括弧を持たない制御の本体と Ruby の一行の塊
				const bool IsBraceLessControlBody =
				Syntax.Is(CurrentId, Role::BodyWrapper) && !Syntax.IsTokenRole(Syntax.Child(CurrentId, 0), Role::OpeningBraceToken);
				NodeId ClosureBody = NoNode;
				if(Syntax.Is(CurrentId, Role::ClosureBlock)) {
					for(const NodeId Child : Syntax.ChildrenOf(CurrentId)) if(Syntax.Is(Child, Role::ClosureBody)) ClosureBody = Child;
				} else if(Syntax.Is(CurrentId, Role::ClosureStatements)) ClosureBody = CurrentId;
				const bool IsInlineKeywordBody = Syntax.Is(CurrentId, Role::AttachedArrowLambda) ||
				Syntax.Is(CurrentId, Role::InlineBraceBody) && (ClosureBody == NoNode || BodyStatementCount(Syntax, ClosureBody, 2) < 2);
				// 文書を同期する定義は，其の文書を定義の直前の独立行へ置く為に容器を展開させる（整形仕様 6.3, 7.4）
				Current.IsResult = SignatureExtractor::IsDocumentable(Syntax, CurrentId) || Syntax.Is(CurrentId, Role::IndentedBody) || (
					// 列挙子列はインデント可能でも文の本体でない為，幅に依らず展開しない
					Syntax.Is(CurrentId, Role::IndentContainer) && !Syntax.Is(CurrentId, Role::EnumeratorSequence) &&
					!Syntax.Is(CurrentId, Role::ObjectLike) && !Syntax.Is(CurrentId, Role::NonKeywordClass) ||
					Syntax.Is(CurrentId, Role::EnumBodyDeclarations) || Syntax.Is(CurrentId, Role::KeywordControlLayout) ||
					Syntax.Is(CurrentId, Role::SwitchBodyLayout) || Syntax.Is(CurrentId, Role::DoLoop)
				) && !IsBraceLessControlBody && !IsInlineKeywordBody && NamedCount &&
				(!Syntax.Is(CurrentId, Role::KeywordBlock) || KeepsKeywordBodyExpanded(Syntax, CurrentId)) ||
				Syntax.Is(CurrentId, Role::DeferredLiteralBody) || Syntax.Is(CurrentId, Role::BracedConditionalValue) && std::ranges::any_of(
					Syntax.ChildrenOf(CurrentId),
					[&Syntax](const NodeId Child) -> bool {
						// else の語かの返戻
						return Syntax.Is(Child, Role::ElseKeyword);
					}
				) && !IsArrowValueBody(State, CurrentId) ||
				(Syntax.Is(CurrentId, Role::DirectBraceLayout) || Syntax.Is(CurrentId, Role::DirectControlBraceLayout)) &&
				Syntax.FirstNamedChildWithRole(CurrentId, Role::ValueStatementSequence) != NoNode ||
				Syntax.Is(CurrentId, Role::RenderedChildren) && RenderedContentCount(Syntax, CurrentId) > 1;
				if(!NamedCount) Current.Next = static_cast<uint32_t>(Syntax.ChildrenOf(CurrentId).size());
			}
			// 未確定の子の探索と結果の控え
			if(const std::span<const NodeId> Children = Syntax.ChildrenOf(CurrentId); !Current.IsResult && Current.Next < Children.size()) {
				if(const NodeId Child = Children[Current.Next++]; Syntax.IsNamed(Child)) Stack.push_back({ Child });
				continue;
			}
			const bool Result = Current.IsResult;
			State.Bits[CurrentId] |= static_cast<uint8_t>(ContainsComputedBit | (Result ? ContainsBit : 0));
			Stack.pop_back();
			if(Stack.empty()) {
				FinalResult = Result;
				break;
			}
			if(Result) Stack.back().IsResult = true;
		}
		// 展開要因の有無の返戻
		return FinalResult;
	}

	/**
	 * 改行を跨いで前の文へ繋がる文の判定関数
	 * Kotlin・Swift は改行の後の `{` を前の式の末尾ラムダとして読み，Kotlin の解析器は改行の後の `(` `[` `::` も前の式へ繋ぐ
	 * @param Syntax 構文モデル
	 * @param IR 中間表現
	 * @param Head 次の文の先頭の要素
	 * @return 改行では区切れず `;` が要るなら true
	 */
	bool Canonicalizer::JoinsAcrossNewline(const SyntaxModel &Syntax, const FormatIR &IR, const EdgeItem Head) {
		// 空の文は繋がらない事の返戻
		if(Head.Item == NoItem) return false;
		// 次の文の始まりが前の式へ繋がるかの返戻
		return Syntax.Rules().CanContinueWithBrace && EdgeIs(Syntax, IR, Head, Role::OpeningBraceToken, GeneratedToken::BlockOpen) ||
		Syntax.Rules().CanContinueWithCall && (
			EdgeIs(Syntax, IR, Head, Role::OpeningParenToken, GeneratedToken::CallOpen) ||
			EdgeIs(Syntax, IR, Head, Role::OpeningBracketToken) || EdgeIs(Syntax, IR, Head, Role::DoubleColonToken)
		);
	}

	/**
	 * 展開した閉じ括弧の後の改行の判定関数
	 * 行頭の閉じ括弧の後に更に閉じ括弧が続く場合は次の行へ置き，其れ以外は同じ行へ続ける（整形仕様 6.3）
	 * @param Syntax 構文モデル
	 * @param NextHead 後続の先頭の要素
	 * @param Out 組立中の出力
	 * @return 後続の閉じ括弧を次の行へ送るなら true
	 */
	bool Canonicalizer::ShouldBreakAfterExpandedBrace(const SyntaxModel &Syntax, const EdgeItem NextHead, const Emitter &Out) {
		// 閉じ波括弧の後の改行が自動のセミコロンを招く文法と，行頭の閉じ波括弧で終わらない場合に送らない事の返戻
		if(
			Syntax.Rules().HasAutomaticLineTerminator || NextHead.Item == NoItem ||
			!Out.TailIs(Syntax, Role::ClosingBraceToken, GeneratedToken::BlockClose) || !Out.Preceding().EndsWithNewline
		) return false;
		// 後続が閉じ括弧かの返戻
		return EdgeIsBracket(Syntax, Out.Store(), NextHead, false);
	}

	/**
	 * 節点毎の部分木のヒアドキュメントの数の集計関数（整形仕様 9.2，節点毎に部分木を走査し直さない）
	 * 計算量：節点数 V に対し O(V)
	 * @param Syntax 構文モデル
	 * @return 節点の識別子毎の部分木の開始と本文の数
	 */
	std::vector<Canonicalizer::HeredocSummary> Canonicalizer::SummarizeHeredocs(const SyntaxModel &Syntax) {
		std::vector<HeredocSummary> Summaries(Syntax.NodeCount());
		// 前順の並び（逆に辿ると子が親より先に為る）
		std::vector<NodeId> Order, Stack { Syntax.GetRoot() };
		while(!Stack.empty()) {
			const NodeId Node = Stack.back();
			Stack.pop_back();
			Order.push_back(Node);
			const std::span<const NodeId> Children = Syntax.ChildrenOf(Node);
			Stack.insert(Stack.end(), Children.begin(), Children.end());
		}
		// 節点毎の１回の集計を１作業単位とする
		Syntax.Charge(Order.size());
		for(size_t Index = Order.size(); Index--;) {
			const NodeId Node = Order[Index];
			HeredocSummary &Summary = Summaries[Node];
			Summary.Begins += Syntax.Is(Node, Role::DeferredLiteralStart);
			Summary.Bodies += Syntax.Is(Node, Role::DeferredLiteralBody);
			if(const NodeId Parent = Syntax.ParentOf(Node); Parent != NoNode) {
				Summaries[Parent].Begins += Summary.Begins;
				Summaries[Parent].Bodies += Summary.Bodies;
			}
		}
		// 集計の返戻
		return Summaries;
	}

	/**
	 * 本文を控えたヒアドキュメントの開始の数の更新関数
	 * 部分木の開始の数を足し，本文の数を引く（本文が前の兄弟の開始に対応する形も有る為，負に為れば０）
	 * @param State 構築中の状態（部分木の集計を持つ）
	 * @param Id 部分木の根（自身を含む）
	 * @param Open 本文を控えた開始の数（更新する）
	 */
	void Canonicalizer::TrackOpenHeredocs(const Builder &State, const NodeId Id, uint32_t &Open) {
		// 本文を後送する字句を持たない記法の終了
		if(State.Heredocs.empty()) return;
		const HeredocSummary &Summary = State.Heredocs[Id];
		Open = Open + Summary.Begins > Summary.Bodies ? Open + Summary.Begins - Summary.Bodies : 0;
		// 終了
		return;
	}

	/**
	 * ヒアドキュメントの本体の追加関数
	 * 本体は開始の行の次の行から始まり終端の語が独自の行を要する為，逐語の儘前後に改行を置く
	 * @param State 構築中の状態
	 * @param Body ヒアドキュメントの本体
	 * @param Out 組立中の出力
	 * @return 本文を追加すれば true（空の本体は読み飛ばす）
	 */
	Canonicalizer::Pending<bool> Canonicalizer::AppendHeredocBody(Builder &State, const NodeId Body, Emitter &Out) {
		const SyntaxModel &Syntax = State.Syntax;
		if(!Syntax.ChildrenOf(Body).empty()) {
			if(Out.IsAtLineStart()) Out.PopBack();
			Out.Append(co_await TakeFlat(State, Body));
			Out.Break();
			// 補間式を含む本文を追加した事の返戻
			co_return true;
		}
		std::string_view Text = Syntax.GetNode(Body).Lexeme;
		// 空の本体の読飛の返戻
		if(Text.empty()) co_return false;
		// 開始の行の改行を重ねずに本文の値を保つ
		if(Out.IsAtLineStart()) Out.PopBack();
		FormatIR &IR = Out.Store();
		Built Piece;
		Piece.Item = IR.Leaf(Text, FormatItemKind::Verbatim);
		IR.Items[Piece.Item].Node = Body;
		Piece.Head = FindEdge(IR, Piece.Item, true);
		Piece.Tail = Piece.Head;
		Piece.HasBreak = true;
		Out.Append(std::move(Piece));
		if(!Out.IsAtLineStart()) Out.Break();
		// 追加した事の返戻
		co_return true;
	}

	/**
	 * 本文を控えたヒアドキュメントの後の文の区切りの追加関数
	 * 開始の後の文は同じ行へ `;` で区切って続ける
	 * @param Syntax 読取専用の期待構文
	 * @param Out 組立中の出力
	 */
	void Canonicalizer::AppendHeredocPendingSeparator(const SyntaxModel &Syntax, Emitter &Out) {
		Out.TrimSpaces();
		if(!Out.IsEmpty() && !Out.TailIs(Syntax, Role::StatementTerminatorToken, GeneratedToken::StatementTerminator)) {
			Out.Text(Syntax, GeneratedToken::StatementTerminator);
		}
		Out.Space();
		// 終了
		return;
	}

	/**
	 * PHP の代替構文の本体を開く `:` かの判定関数
	 * @param Syntax 構文モデル
	 * @param Parent `:` の親
	 * @param Colon `:` の字句
	 * @return 本体を開く `:` なら true
	 */
	bool Canonicalizer::IsAlternateBodyOpener(const SyntaxModel &Syntax, const NodeId Parent, const NodeId Colon) {
		// 代替構文の本体を持つ親の `:` かの返戻（先頭の `:` だけを開きとする親は先頭の子に限る）
		return Syntax.Is(Parent, Role::ColonBodyHost) && (!Syntax.Is(Parent, Role::LeadingColonBody) || !Syntax.GetNode(Colon).Index);
	}

	/**
	 * PHP の代替構文の本体で終わるかの判定関数
	 * @param Syntax 構文モデル
	 * @param Id 節点
	 * @return 代替構文の本体其の物か，其れで終わる節なら true
	 */
	bool Canonicalizer::EndsWithColonBody(const SyntaxModel &Syntax, const NodeId Id) {
		// 名前付でない節点の返戻
		if(!Syntax.IsNamed(Id)) return false;
		// 代替構文の本体其の物の返戻
		if(Syntax.Is(Id, Role::ColonBody)) return true;
		const uint32_t Count = Syntax.NamedChildCount(Id);
		// 最後の名前付の子が代替構文の本体かの返戻
		return Count && Syntax.Is(Syntax.NamedChild(Id, Count - 1), Role::ColonBody);
	}

	/**
	 * 子の括弧の対の閉じの位置の取得関数
	 * 同じ親の子の開き括弧と閉じ括弧を対応付け，対の中身を深さの範囲にする（整形仕様 6.5）
	 * @param Syntax 構文モデル
	 * @param Id 節点
	 * @return 子の番号毎の対応する閉じの子の番号（範囲にしない子は子の数）
	 */
	std::vector<uint32_t> Canonicalizer::ScopeClosers(const SyntaxModel &Syntax, const NodeId Id) {
		// 構文上の対を持つ容器の範囲の記録
		const std::span<const NodeId> Children = Syntax.ChildrenOf(Id);
		const uint32_t Count = static_cast<uint32_t>(Children.size());
		std::vector<uint32_t> Closers(Count, Count);
		if(Syntax.Is(Id, Role::MarkupElement) && Count > 1) Closers[0] = Count - 1;
		if(Syntax.Is(Id, Role::ExpressionTag) && Count > 2) {
			uint32_t Head = 1;
			if(Count > 3 && Syntax.Is(Children[2], Role::GenericArguments)) Head = 2;
			Closers[Head] = Count - 1;
		}
		std::vector<uint32_t> Opens;
		if(Count > 1 && (Syntax.Is(Id, Role::WordArray) || Syntax.Is(Id, Role::PipeParamList))) Closers[0] = Count - 1;
		// 子の字句による括弧の対応付け
		for(uint32_t Index = 0; Index < Count; ++Index) {
			if(Syntax.IsNamed(Children[Index])) continue;
			if(
				const NodeId Child = Children[Index];
				Syntax.IsTokenRole(Child, Role::OpeningParenToken) || Syntax.IsTokenRole(Child, Role::OpeningBracketToken) ||
				Syntax.IsTokenRole(Child, Role::OpeningBraceToken)
			) Opens.push_back(Index);
			else if(
				(
					Syntax.IsTokenRole(Child, Role::ClosingParenToken) || Syntax.IsTokenRole(Child, Role::ClosingSquareToken) ||
					Syntax.IsTokenRole(Child, Role::ClosingBraceToken)
				) && !Opens.empty()
			) {
				const uint32_t Open = Opens.back();
				Opens.pop_back();
				if(
					Syntax.IsTokenRole(Child, Role::ClosingParenToken) && Syntax.IsTokenRole(Children[Open], Role::OpeningParenToken) ||
					Syntax.IsTokenRole(Child, Role::ClosingSquareToken) && Syntax.IsTokenRole(Children[Open], Role::OpeningBracketToken) ||
					Syntax.IsTokenRole(Child, Role::ClosingBraceToken) && Syntax.IsTokenRole(Children[Open], Role::OpeningBraceToken)
				) Closers[Open] = Index;
			}
		}
		// 閉じの位置の返戻
		return Closers;
	}

	/**
	 * 構造化の控えの取得関数
	 * @param State 構築中の状態
	 * @param Id 節点
	 * @return 控えた構造化の要素
	 */
	Canonicalizer::Pending<const Canonicalizer::Built *> Canonicalizer::GetStructured(Builder &State, const NodeId Id) {
		if(const std::unordered_map<NodeId, Built>::const_iterator Found = State.Structured.find(Id); Found != State.Structured.end()) {
			// 控えた構造化の要素の返戻
			co_return &Found->second;
		}
		Built Result = co_await BuildStructured(State, Id);
		// 控えへ入れた構造化の要素の返戻
		co_return &State.Structured.emplace(Id, std::move(Result)).first->second;
	}

	/**
	 * 構造化の控えの取出関数
	 * @param State 構築中の状態
	 * @param Id 節点
	 * @return 構造化の要素（控えから除く）
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::TakeStructured(Builder &State, const NodeId Id) {
		if(const std::unordered_map<NodeId, Built>::iterator Found = State.Structured.find(Id); Found != State.Structured.end()) {
			Built Result = std::move(Found->second);
			State.Structured.erase(Found);
			// 控えから取り出した要素の返戻
			co_return Result;
		}
		// 新たに組み立てた要素の返戻
		co_return co_await BuildStructured(State, Id);
	}

	/**
	 * 末尾の区切りの追加関数
	 * 最上位の文の後の名前付の兄弟が所有しない `;` / `,` は，文の要素と共に出す
	 * @param Syntax 構文モデル
	 * @param Id 文の節点
	 * @param Out 組立中の出力
	 */
	void Canonicalizer::AppendTrailingSeparator(const SyntaxModel &Syntax, const NodeId Id, Emitter &Out) {
		const NodeId Next = Syntax.NextSibling(Id);
		// 後に無名の区切りが無い文の終了
		if(Next == NoNode || Syntax.IsNamed(Next)) return;
		// 後の区切りが文の終端でもコンマでもない場合の終了
		if(!Syntax.IsTokenRole(Next, Role::StatementTerminatorToken) && !Syntax.IsTokenRole(Next, Role::CommaToken)) return;
		// Go・Kotlin・Swift の最上位の文を改行で区切る場合の終了
		if(Syntax.Rules().CanOmitTrailingSemicolon && Syntax.IsTokenRole(Next, Role::StatementTerminatorToken)) return;
		// 前処理指令の塊の直下の区切りを親が出す場合の終了
		if(Syntax.Is(Syntax.ParentOf(Id), Role::PreprocessorBranch)) return;
		if(
			!Out.TailIs(Syntax, Role::StatementTerminatorToken, GeneratedToken::StatementTerminator) &&
			!Out.TailIs(Syntax, Role::CommaToken, GeneratedToken::ListSeparator)
		) Out.Append(BuildLeaf(Syntax, Next, Out.Store()));
		// 終了
		return;
	}

	/**
	 * 多行の並びの末尾のカンマの生成形式の取得関数
	 * 閉じの前で改行した並びにカンマを要するかは投影済の形式から決める
	 * @param Syntax 構文モデル
	 * @param Container 並び
	 * @param Last 閉じの前の子
	 * @param Closer 閉じの字句
	 * @param IsBroken 並びを複数行へ展開するか
	 * @return カンマを補う生成形式（使えなければ無し）
	 */
	const CanonicalForm *Canonicalizer::TrailingCommaForm(
		const SyntaxModel &Syntax,
		const NodeId Container,
		const NodeId Last,
		const NodeId Closer,
		const bool IsBroken
	) {
		const CanonicalForm *const Form = Syntax.FormOf(Container, FormOperation::TrailingComma);
		// 投影した適用条件を満たす並びの生成形式の返戻
		return Form && Form->CanAddTrailingComma(IsBroken, Syntax.IsNamed(Last), Syntax.FirstChar(Closer)) ? Form : nullptr;
	}

	/**
	 * 展開形の容器の組立関数
	 * 開きの後・要素の区切りの後で改行し，閉じを独自の行に置く（Go の複数行の並びは閉じの前に末尾のカンマを補う）
	 * @param State 構築中の状態
	 * @param Id 容器
	 * @return 組立済の展開形
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildExpandedContainer(Builder &State, const NodeId Id) {
		// 展開する容器の子の順次配置
		const SyntaxModel &Syntax = State.Syntax;
		Emitter Out(State.IR);
		NodeId Left = NoNode;
		uint32_t OpenHeredocs = 0;
		for(const NodeId Child : Syntax.ChildrenOf(Id)) {
			if(Syntax.Is(Child, Role::DeferredLiteralBody)) {
				Out.TrimSpaces();
				co_await AppendHeredocBody(State, Child, Out);
				TrackOpenHeredocs(State, Child, OpenHeredocs);
				Left = Child;
				continue;
			}
			// ヒアドキュメントの本文を控えた開始行の続きは折らない（行分割仕様第３章）
			Built Piece = !Syntax.IsNamed(Child) ?
			BuildLeaf(Syntax, Child, State.IR) :
			OpenHeredocs ? co_await BuildSealedFlat(State, Child) : co_await TakeStructured(State, Child);
			if(Piece.Head.Item == NoItem) continue;
			const bool IsSingle = Piece.Head.Item == Piece.Tail.Item;
			const bool IsOpen = IsSingle && EdgeIsBracket(Syntax, State.IR, Piece.Head, true);
			const bool IsClose = IsSingle && EdgeIsBracket(Syntax, State.IR, Piece.Head, false);
			const bool IsComma = IsSingle && EdgeIs(Syntax, State.IR, Piece.Head, Role::CommaToken, GeneratedToken::ListSeparator);
			Out.TrimSpaces();
			if(IsClose) Out.CloseScope();
			// Go の複数行の並びは閉じの前に末尾のカンマが要り，改行の前に補って構文を保つ
			if(IsClose) if(TrailingCommaForm(Syntax, Id, Left, Child, true)) {
				Out.Text(Syntax, GeneratedToken::ListSeparator);
				State.IR.EditDetail(Out.Last().Item).PunctuationOwner = Id;
			}
			if(ShouldBreakAfterExpandedBrace(Syntax, Piece.Head, Out) || IsClose && !Out.IsEmpty() && !Out.IsAtLineStart()) Out.Break();
			else if(!IsOpen && !IsClose && !IsComma && !Out.IsEmpty() && !Out.IsAtLineStart()) Out.Space();
			Out.Append(std::move(Piece));
			Left = Child;
			TrackOpenHeredocs(State, Child, OpenHeredocs);
			if(IsOpen) Out.OpenScope();
			if(!OpenHeredocs && (IsOpen || IsComma)) Out.Break();
		}
		// 展開形の返戻
		co_return Out.Finish(Id, NoGroup);
	}

	/**
	 * 文の組立関数
	 * 最上位・塊の中の文を構造化し，名前付の兄弟が所有しない末尾の区切りを添える
	 * @param State 構築中の状態
	 * @param Id 文の節点
	 * @return 組立済の文
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::TakeStatement(Builder &State, const NodeId Id) {
		Emitter Out(State.IR);
		Out.Append(co_await TakeStructured(State, Id));
		AppendTrailingSeparator(State.Syntax, Id, Out);
		// 組立済の文の返戻
		co_return Out.Finish(NoNode, NoGroup);
	}

	/**
	 * 前処理指令の条件の塊の組立関数
	 * 指令の行と本体の宣言を行毎に置き，空行は収集した規則に従う
	 * 計算量：辿る子の総数 V に対し O(V)
	 * @param State 構築中の状態
	 * @param Id 前処理指令の塊
	 * @return 組立済の塊
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildPreprocBlock(Builder &State, const NodeId Id) {
		// 入れ子の条件の塊を辿る作業枠の準備
		const SyntaxModel &Syntax = State.Syntax;
		struct Frame {
			NodeId Id;
			Emitter Out;
			size_t Next = 0;
			NodeId PendingChild = NoNode;
			bool HasPrev = false;
			bool IsAwaitingCondition = false;
		};
		std::vector<Frame> Stack;
		Stack.push_back({ Id, Emitter(State.IR) });
		Built Completed;
		// 作業枠毎の指令と子の配置
		while(!Stack.empty()) {
			Frame &Current = Stack.back();
			if(Current.PendingChild != NoNode) {
				AppendChild(State, Current.PendingChild, std::move(Completed), Current.Out);
				Current.PendingChild = NoNode;
				Current.HasPrev = true;
				continue;
			}
			const std::span<const NodeId> Children = Syntax.ChildrenOf(Current.Id);
			if(Current.Next == Children.size()) {
				Completed = Current.Out.Finish(Current.Id, NoGroup);
				Stack.pop_back();
				continue;
			}
			const NodeId Child = Children[Current.Next++];
			Emitter &Out = Current.Out;
			bool &HasPrev = Current.HasPrev, &IsAwaitingCondition = Current.IsAwaitingCondition;
			if(!Syntax.IsNamed(Child)) {
				const std::string &Text = Syntax.GetNode(Child).Lexeme;
				// 空白だけの無名の字句の読飛
				if(!Text.empty() && Text.find_first_not_of(" \t\n\r") == std::string::npos) continue;
				if(Syntax.Is(Child, Role::PreprocDirectiveOpen) || Syntax.Is(Child, Role::PreprocDirectiveClose)) {
					if(HasPrev) Out.Break();
					AppendChild(State, Child, BuildLeaf(Syntax, Child, State.IR), Out);
					HasPrev = Syntax.Is(Child, Role::PreprocDirectiveClose);
					IsAwaitingCondition = Syntax.Is(Child, Role::PreprocDirectiveOpen);
					continue;
				}
				Out.Append(BuildLeaf(Syntax, Child, State.IR));
				continue;
			}
			// 指令の条件式は同じ行へ続け，行末の改行が指令を終える為に内部を折らない
			if(IsAwaitingCondition) {
				Out.Space();
				Out.Append(co_await BuildSealedFlat(State, Child));
				IsAwaitingCondition = false;
				HasPrev = true;
				continue;
			}
			if(HasPrev) Out.Break();
			if(Syntax.Is(Child, Role::PreprocessorBranch)) {
				Current.PendingChild = Child;
				Stack.push_back({ Child, Emitter(State.IR) });
				continue;
			}
			AppendChild(State, Child, co_await TakeStatement(State, Child), Out);
			HasPrev = true;
		}
		// 組立済の塊の返戻
		co_return Completed;
	}

	/**
	 * 分割可能な統合宣言の要素の完了関数（行分割仕様 4.1）
	 * 共有する部分，宣言子の間の区切り，宣言の間の改行と宣言子毎の名前と初期化子の組を行分割が宣言へ区分する要素にし，端の字面は１つの宣言の形で測る
	 * @param Syntax 構文モデル
	 * @param Id 宣言
	 * @param Head 共有する接頭部
	 * @param Middle 共有する中間部（名前の並びと初期化子の並びの間）
	 * @param Tail 共有する終端部
	 * @param Names 宣言子毎の名前（宣言子を丸ごと並べる宣言は宣言子）
	 * @param Values 宣言子毎の初期化子（初期化子の並びを持たない宣言は空）
	 * @param IR 構築中の中間表現
	 * @return 組立済の統合宣言
	 * @throws FormatLimitExceeded 宣言子列又は元の節点が不正な場合（中間表現の不具合）
	 */
	Canonicalizer::Built Canonicalizer::FinishDeclarationSplit(
		const SyntaxModel &Syntax,
		const NodeId Id,
		Built &&Head,
		Built &&Middle,
		Built &&Tail,
		std::vector<Built> &&Names,
		std::vector<Built> &&Values,
		FormatIR &IR
	) {
		if(Id == NoNode || Names.empty() || !Values.empty() && Values.size() != Names.size()) {
			throw FormatLimitExceeded("the format IR has an invalid declaration split", FailureReason::FormatterDefect);
		}
		// 宣言子の間の区切りと宣言の間の改行（行分割が区分に使う固定の要素）
		const std::array<FormatItemId, 2> Comma {
			GeneratedItem(Syntax, GeneratedToken::ListSeparator, IR),
			IR.Leaf(" ", FormatItemKind::Space)
		};
		std::vector<FormatItemId> Parts {
			Head.Item,
			Middle.Item,
			Tail.Item,
			IR.Parent(FormatItemKind::Sequence, Comma),
			IR.Leaf({}, FormatItemKind::HardBreak)
		};
		Emitter NameRow(IR), ValueRow(IR), Probe(IR);
		// 宣言子毎の要素と平坦形の組立
		for(size_t Index = 0; Index < Names.size(); ++Index) {
			const std::array<FormatItemId, 2> Unit {
				Names[Index].Item,
				Values.empty() ? IR.Parent(FormatItemKind::Sequence, {}) : Values[Index].Item
			};
			Parts.push_back(IR.Parent(FormatItemKind::Sequence, Unit));
			if(Index) {
				NameRow.Text(Syntax, GeneratedToken::ListSeparator);
				NameRow.Space();
				if(!Values.empty()) {
					ValueRow.Text(Syntax, GeneratedToken::ListSeparator);
					ValueRow.Space();
				}
			}
			NameRow.Append(std::move(Names[Index]));
			if(!Values.empty()) ValueRow.Append(std::move(Values[Index]));
		}
		// 平坦形の端の情報と宣言の区分の組立
		Probe.Append(std::move(Head));
		Probe.Append(NameRow.Finish(NoNode, NoGroup));
		Probe.Append(std::move(Middle));
		Probe.Append(ValueRow.Finish(NoNode, NoGroup));
		Probe.Append(std::move(Tail));
		Built Result = Probe.Finish(NoNode, NoGroup);
		const FormatItemId Split = IR.Parent(FormatItemKind::DeclarationSplit, Parts);
		IR.Items[Split].Node = Id;
		Result.Item = Anchored(Id, Split, IR);
		// 組立済の統合宣言の返戻
		return Result;
	}

	/**
	 * カンマで並べる統合宣言の組立関数（整形仕様 5.6，行分割仕様 4.1）
	 * 最初の要素の前の共有する子を接頭部，名前と初期化子の間の共有する子を中間部，最後の要素の後の共有する子を終端部とし，宣言子（又は名前と初期化子の組）を区分の単位にする
	 * 子の間の空白は同じ親の子へ遡った組の構文上の役割で決め，要素の中の子の境界は並びの節点の折返の候補を群にする
	 * @param State 構築中の状態
	 * @param Id 宣言子又は名前が２つ以上の宣言
	 * @return 組立済の要素
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildCommaDecl(Builder &State, const NodeId Id) {
		const SyntaxModel &Syntax = State.Syntax;
		const CommaDecl Shape = *Syntax.CommaDeclOf(Id);
		const NodeId Host = Shape.Lists.front().Host;
		// 節点の宣言からの深さ
		const auto Depth = [&Syntax, Id](NodeId Node) -> uint32_t {
			uint32_t Result = 0;
			for(; Node != Id; Node = Syntax.ParentOf(Node)) ++Result;
			// 深さの返戻
			return Result;
		};
		// 前後の子の間の空白の要否（同じ親の子へ遡った組で判ずる）
		const auto IsSpaced = [&](NodeId Left, NodeId Right) -> bool {
			// 両節点の共通の親に属する子の組の取得
			uint32_t LeftDepth = Depth(Left), RightDepth = Depth(Right);
			for(; LeftDepth > RightDepth; --LeftDepth) Left = Syntax.ParentOf(Left);
			for(; RightDepth > LeftDepth; --RightDepth) Right = Syntax.ParentOf(Right);
			NodeId LeftParent = Syntax.ParentOf(Left), RightParent = Syntax.ParentOf(Right);
			while(LeftParent != RightParent) {
				Left = LeftParent;
				Right = RightParent;
				LeftParent = Syntax.ParentOf(Left);
				RightParent = Syntax.ParentOf(Right);
			}
			// 兄弟の組の空白の要否の返戻
			return NeedsSpace(Syntax, RightParent, Left, Right);
		};
		// 並びの節点毎の子の間の折返の候補
		std::vector<std::vector<uint8_t>> Candidates;
		for(const DeclList &List : Shape.Lists) Candidates.push_back(BreakBoundaries(Syntax, List.Host));
		// 宣言の外の接頭部，並びの節点の中の接頭部，中間部，終端部と，並び毎の要素
		Emitter Outer(State.IR), Inner(State.IR), Middle(State.IR), Tail(State.IR);
		std::vector<std::vector<Emitter>> Rows;
		for(const DeclList &List : Shape.Lists) Rows.emplace_back(List.Items.size(), Emitter(State.IR));
		Emitter *LastShared = nullptr;
		NodeId Previous = NoNode;
		// 最後に要素を置いた並び（無ければ -1）
		int32_t LastList = -1;
		// 宣言の共有部分と各要素への子の振分
		for(const DeclPiece &Piece : Shape.Pieces) {
			const bool IsShared = Piece.List < 0;
			Emitter &Out = !IsShared ? Rows[static_cast<size_t>(Piece.List)][Piece.Item] : LastList < 0 ?
			Host != Id && Syntax.ParentOf(Piece.Node) == Host ? Inner : Outer :
			static_cast<size_t>(LastList) + 1 < Shape.Lists.size() ? Middle : Tail;
			// 共有する子の後の空白は其の区画の末尾，要素の中は折返の候補，其の他は此の区画の中（別の要素の頭は区切りが出す）
			const bool HasSpace = Previous != NoNode && IsSpaced(Previous, Piece.Node);
			if(!IsShared && !Out.IsEmpty()) {
				if(Candidates[static_cast<size_t>(Piece.List)][Syntax.GetNode(Piece.Node).Index]) Out.Soft(HasSpace ? " " : "");
				else if(HasSpace) Out.Space();
			} else if(HasSpace) {
				if(LastShared) LastShared->Space();
				else if(IsShared) Out.Space();
			}
			Out.Append(Syntax.IsNamed(Piece.Node) ? co_await TakeStructured(State, Piece.Node) : BuildLeaf(Syntax, Piece.Node, State.IR));
			LastShared = IsShared ? &Out : nullptr;
			if(!IsShared) LastList = Piece.List;
			Previous = Piece.Node;
		}
		// 並びの節点の錨は其の中の接頭部，無ければ中間部に置き，初期化子の並びの錨は中間部に置く
		const bool HasInner = !Inner.IsEmpty();
		Built Shared = Middle.Finish(Shape.Lists.size() > 1 ? Shape.Lists.back().Host : NoNode, NoGroup);
		Built Type = Inner.Finish(NoNode, NoGroup);
		if(Host != Id) (HasInner ? Type : Shared).Item = Anchored(Host, (HasInner ? Type : Shared).Item, State.IR);
		Outer.Append(std::move(Type));
		std::vector<Built> Names, Values;
		for(Emitter &Row : Rows.front()) Names.push_back(Row.Finish(NoNode, State.NextGroup++));
		if(Rows.size() > 1) for(Emitter &Row : Rows.back()) Values.push_back(Row.Finish(NoNode, State.NextGroup++));
		// 組立済の統合宣言の返戻
		co_return FinishDeclarationSplit(
			Syntax,
			Id,
			Outer.Finish(NoNode, NoGroup),
			std::move(Shared),
			Tail.Finish(NoNode, NoGroup),
			std::move(Names),
			std::move(Values),
			State.IR
		);
	}

	/**
	 * 波括弧を行幅で付ける単一の文の制御本体かの判定関数（整形仕様 6.1）
	 * @param Syntax 構文モデル
	 * @param Parent 制御構文
	 * @param Child 子
	 * @return 波括弧の塊の生成形式を持つ構文の if の枝・繰返の本体で，塊でも空文でもない文なら true
	 */
	bool Canonicalizer::IsBraceOptionalBody(const SyntaxModel &Syntax, const NodeId Parent, const NodeId Child) {
		const CanonicalForm *const Form = Syntax.FormOf(Parent, FormOperation::BraceBody);
		// 波括弧を省略出来ない言語と，塊・空文の本体と，既知の正常な構文へ組み直さない誤りを含む構文の本体の返戻（整形仕様 4.6）
		if(
			Syntax.Is(Parent, Role::Labeled) || !Form || !Form->CanWrapBody(Syntax.IsBracedBody(Child)) || !Syntax.IsNamed(Child) ||
			Syntax.Is(Child, Role::EmptyStatement) ||
			Syntax.HasTokenRoleChild(Parent, Role::ColonToken) && Syntax.Is(Parent, Role::ColonBodyHost) ||
			Syntax.Is(Child, Role::DelimitedBody) || Syntax.GetNode(Parent).Facts.Has(ConstraintKind::ContainsError)
		) return false;
		// 本体の包み（場を持たない文法の本体の位置）か
		const bool IsContainer = Syntax.Is(Child, Role::BodyWrapper);
		if(Syntax.Is(Parent, Role::If)) {
			// `alternative` の else 節は節全体でなく其の中の文を包む為，節を包まない事の返戻
			if(Syntax.Is(Child, Role::ElseClause) || Syntax.Is(Child, Role::ElseIfClause)) return false;
			const bool IsAlternative = Child == Syntax.FieldChild(Parent, ChildRole::Alternative) ||
			IsContainer && Syntax.Is(Syntax.PrevSibling(Child), Role::ElseKeyword);
			// if の真枝と，else if の連鎖でない後続の枝かの返戻
			return (IsContainer || IsAlternative || Child == Syntax.IfConsequence(Parent)) &&
			!(IsAlternative && Syntax.Is(IsContainer ? Syntax.NamedChild(Child, 0) : Child, Role::If));
		}
		// else if の連鎖を塊で分断しない為に包まない事の返戻
		if(Syntax.Is(Parent, Role::ElseClause) && Syntax.Is(Child, Role::If)) return false;
		// 繰返の本体かの返戻
		return Syntax.Is(Parent, Role::BodyOwner) && (
			IsContainer || Child == Syntax.FieldChild(Parent, ChildRole::Body) ||
			Syntax.Is(Parent, Role::ElseClause) && Child == Syntax.NamedChild(Parent, 0)
		);
	}

	/**
	 * 行幅で波括弧を付ける制御本体の群の組立関数（整形仕様 6.1）
	 * 平坦時は見出と同じ行の単一の文，展開時は波括弧で包んだ本体の群とする（本体が複数行に為れば展開する）
	 * 本体が繰返・分岐・切替の連鎖なら其の中の本体の複数行で展開せず，else・while が続く本体は展開時に閉じと同じ行へ続ける
	 * @param State 構築中の状態
	 * @param Parent 制御構文
	 * @param Child 本体の文
	 * @param Body 本体の組立済の要素
	 * @return 組立済の群
	 */
	Canonicalizer::Built Canonicalizer::BuildBraceGroup(Builder &State, const NodeId Parent, const NodeId Child, Built &&Body) {
		const SyntaxModel &Syntax = State.Syntax;
		const bool IsChain = Syntax.Is(Child, Role::Loop) && !Syntax.Is(Child, Role::DoLoop) || Syntax.Is(Child, Role::If) ||
		Syntax.Is(Child, Role::SwitchStatementLike) || Syntax.Is(Child, Role::SwitchValue);
		// 後続の枝の前の本体か（連鎖の本体は末尾の閉じの有無で親が枝を続ける）
		const NodeId NextSibling = IsChain ? NoNode : Syntax.NextSibling(Child);
		const bool IsBeforeClause = !IsChain && (
			Syntax.Is(NextSibling, Role::ElseKeyword) || Syntax.Is(NextSibling, Role::ElseClause) ||
			Syntax.Is(NextSibling, Role::ElseIfClause)
		) || Syntax.Is(Parent, Role::DoLoop);
		// 平坦時も else を同じ行へ続ける `->` の後の値の if 式か（次の行の `else` は when の枝と読まれる）
		const bool IsSameLineClause = IsBeforeClause && IsArrowValueBody(State, Parent);
		FormatIR &IR = State.IR;
		Emitter Out(IR);
		Out.IfBroken({ GeneratedItem(Syntax, GeneratedToken::BlockOpen, IR) });
		Out.OpenScope();
		Out.Soft("");
		Out.Append(std::move(Body));
		Out.CloseScope();
		// `->` の後の値の if 式の連鎖は，全ての枝の包装を連鎖の根の１つの群で揃える（行分割仕様 4.2）
		uint32_t Group = State.NextGroup;
		if(IsArrowValueBody(State, Parent)) {
			NodeId Root = Parent;
			for(
				NodeId Container = Syntax.ParentOf(Root);
				Syntax.Is(Container, Role::BodyWrapper) && Syntax.Is(Syntax.PrevSibling(Container), Role::ElseKeyword) &&
				Syntax.Is(Syntax.ParentOf(Container), Role::If);
				Container = Syntax.ParentOf(Root)
			) Root = Syntax.ParentOf(Container);
			Group = State.ChainGroups.try_emplace(Root, Group).first->second;
		}
		if(Group == State.NextGroup) ++State.NextGroup;
		// 先行する分岐の見出で連鎖を分けた時は，後続の連鎖も同じ分岐深さへ配置
		uint32_t Dependency = NoGroup;
		if(IsChain && Syntax.Is(Parent, Role::DependentBranchLayout)) {
			if(Syntax.Is(Parent, Role::If) && Syntax.IfConsequence(Parent) == Child) State.ChainGroups[Parent] = Group;
			if(Syntax.Is(Parent, Role::ElseClause)) if(
				const std::unordered_map<NodeId, uint32_t>::const_iterator Found = State.ChainGroups.find(Syntax.ParentOf(Parent));
				Found != State.ChainGroups.end()
			) Dependency = Found->second;
		}
		// 本体の複数行を展開の要因にする末尾の候補（連鎖の中の本体の複数行は除く）
		if(IsSameLineClause) {
			// 平坦時は本体の後，展開時は閉じの後へ続く節（間の空白は親が置く）
			Out.Soft("");
			Out.IfBroken({ GeneratedItem(Syntax, GeneratedToken::BlockClose, IR) });
			Built Result = Out.Finish(NoNode, Group);
			IR.Items[Result.Item].Node = Child;
			IR.Items[Result.Item].Operation = FormOperation::BraceBody;
			// 群の返戻
			return Result;
		}
		if(!IsChain) Out.Soft("");
		if(IsBeforeClause) {
			// 平坦時は次の行，展開時は閉じの後へ続く節
			Out.Break();
			Out.IfBroken({ GeneratedItem(Syntax, GeneratedToken::BlockClose, IR), IR.Leaf(" ", FormatItemKind::Space) });
		} else if(IsChain) {
			Out.IfBroken({ IR.Leaf({}, FormatItemKind::HardBreak), GeneratedItem(Syntax, GeneratedToken::BlockClose, IR) });
		} else Out.IfBroken({ GeneratedItem(Syntax, GeneratedToken::BlockClose, IR) });
		Built Result = Out.Finish(NoNode, Group);
		IR.Items[Result.Item].Node = Child;
		IR.Items[Result.Item].Operation = FormOperation::BraceBody;
		IR.EditDetail(Result.Item).ExpansionDependency = Dependency;
		// 群の返戻
		return Result;
	}
} // namespace Core
