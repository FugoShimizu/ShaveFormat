#include "Canonicalizer.hpp"
#include "FormatIR.hpp"
#include "NodeId.hpp"
#include "SyntaxModel.hpp"
#include "SyntaxRole.hpp"
#include "../Util/CharClass.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * エラー領域の閉じない括弧に依る深さの範囲の開閉関数（整形仕様 4.6，6.5）
	 * エラー領域の中で閉じない開きの括弧は後の兄弟へ跨る為，領域の前で其の数だけ深さの範囲を開き，閉じだけの領域は其の数だけ閉じる
	 * 計算量：逐語の部分木のノード数 N，補正する深さ D に対し O(N + D)
	 * @param Syntax 構文モデル
	 * @param Child 追加する子
	 * @param Output 組立中の出力
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::AdjustErrorZoneScopes(const SyntaxModel &Syntax, const NodeId Child, Emitter &Output) {
		// エラー領域でない子の場合の終了
		if(!Syntax.Is(Child, Role::Error)) return;
		// 領域の中の開きの括弧の数から閉じの括弧の数を引いた数
		int Balance = 0;
		std::vector<NodeId> Stack { Child };
		while(!Stack.empty()) {
			const NodeId Id = Stack.back();
			Stack.pop_back();
			Syntax.Charge(1);
			const SyntaxNode &Node = Syntax.GetNode(Id);
			if(Node.Children.empty() && Node.Lexeme.size() == 1) {
				Balance += IsOpenBracketChar(Node.Lexeme.front()) ? 1 : IsCloseBracketChar(Node.Lexeme.front()) ? -1 : 0;
			}
			Stack.insert(Stack.end(), Node.Children.begin(), Node.Children.end());
		}
		for(; Balance > 0; --Balance) Output.OpenScope();
		for(; Balance < 0; ++Balance) Output.CloseScope();
		// 終了
		return;
	}

	/**
	 * 子の組立済の要素の追加関数
	 * 改行の後に置く子に空行の規則が記録されて居れば，其の規則を先に置く
	 * 計算量：追加する子の要素数 C と端を取得する走査費用 Q に対し O(C + Q)
	 * @param State 構築中の状態
	 * @param Child 子
	 * @param Piece 子の組立済の要素
	 * @param Output 組立中の出力
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::AppendChild(Builder &State, const NodeId Child, Built &&Piece, Emitter &Output) {
		// 子の組立済の要素の追加
		AdjustErrorZoneScopes(State.Syntax, Child, Output);
		if(Output.IsAtLineStart()) if(const BlankRule *const Rule = State.Contexts[Child].BlankBefore; Rule) {
			Output.Blank(Rule->Before, Rule->After, Rule->IsForced);
		}
		AppendLexicalSeparator(State.Syntax, Child, Piece.Head, Output);
		Output.Append(std::move(Piece));
		// 終了
		return;
	}

	/**
	 * 地の文との境界の追加関数
	 * 開きに続く単独の文は同行へ置き，複数の文は文毎に改行する
	 * 計算量：コード境界のテキスト長 B に対し O(B)
	 * @param State 構築中の状態
	 * @param Prev 直前の子
	 * @param Next 次の子
	 * @param Output 出力
	 * @return 境界を処理した場合は true
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	bool Canonicalizer::HasProcessedCodeBoundary(Builder &State, const NodeId Prev, const NodeId Next, Emitter &Output) {
		// 地の文との境界の追加
		const SyntaxModel &Syntax = State.Syntax;
		// 埋込コードの境界でない場合の返戻
		if(Prev == NoNode || Output.IsEmpty()) return false;
		// 地の文へ密着する境界の返戻
		if(Syntax.Is(Prev, Role::InlineText) && !Syntax.Is(Prev, Role::CodeBoundary)) return true;
		// ソースの最初の開始タグ（本体の外の先頭の空白の後を含む）か
		const NodeId Before = Syntax.GetPrevSibling(Prev);
		const bool IsInitial = Syntax.Is(Prev, Role::CodeOpener) && (Before == NoNode || Syntax.Is(Before, Role::OuterText)) &&
		!HasSingleTokenRole(State, Prev, Role::ValueOutputTag);
		if(Syntax.Is(Next, Role::CodeBoundary)) {
			if(!IsInitial && (Output.HasInlineCode() || Output.IsTailRole(Syntax, Role::ColonToken))) Output.Space();
			else if(!Output.IsAtLineStart()) Output.Break();
			// コードの閉じの前を処理した事の返戻
			return true;
		}
		if(Syntax.Is(Prev, Role::CodeBoundary) || Syntax.Is(Prev, Role::CodeOpener)) {
			if(
				const NodeId After = Syntax.GetNextSibling(Next);
				IsInitial || After != NoNode && Syntax.IsNamed(After) && !Syntax.Is(After, Role::CodeBoundary)
			) Output.Break();
			else Output.Space();
			// コードの開きの後を処理した事の返戻
			return true;
		}
		// 通常の境界の返戻
		return false;
	}

	/**
	 * 構造化の本体の組立関数（整形仕様 6.3）
	 * 計算量：直接の子と見出の部品数 C に対し O(C) と文数判定・子の組立・出力生成の総費用
	 * @param State 構築中の状態
	 * @param Id ノード
	 * @return 組立済の要素
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildStructuredCore(Builder &State, const NodeId Id) {
		// 特殊な領域と平坦形の選択
		const SyntaxModel &Syntax = State.Syntax;
		const SyntaxNode &Node = Syntax.GetNode(Id);
		const RoleSet *const Roles = Node.Kind.Def ? &Node.Kind.Def->Roles : nullptr;
		const auto Has = [Roles](const Role Wanted) -> bool {
			// 同じノードの固定定義から役割を照会する
			return Roles && Roles->test(static_cast<size_t>(Wanted));
		};
		// Ruby のヒアドキュメントの本体は終端の語が独自の行頭・行末を要する為，逐語に後続の必須改行の条件を付ける
		if(Has(Role::DeferredLiteralBody)) {
			Emitter Output(State.IR);
			Output.Append(co_await BuildHeredocBody(State, Id));
			// 本体の返戻
			co_return Output.Finish(NoNode, NoGroup);
		}
		// トークンの葉とテキストを保つ領域の返戻
		if(Node.Children.empty() && !State.Contexts[Id].ShouldWrap) co_return BuildLeaf(Syntax, State.IR, Id);
		// 改行を許さない領域の平坦形の返戻
		if(IsUnbreakable(State, Id) || Node.Facts.TokenSpacing != TokenSpacingMode::None) co_return co_await BuildFlat(State, Id);
		// 別の宣言へ分けれる文の並びのカンマで並べる宣言（行分割仕様 4.1）の返戻
		if(Syntax.Is(Node.Parent, Role::StmtHost) && Has(Role::CommaDecl) && HasMultipleDeclarators(Syntax, Id)) {
			if(
				const std::optional<CommaDecl> Shape = Syntax.GetCommaDeclOf(Id, false);
				Shape && Shape->Lists.front().Items.size() > 1 && CanSplitDecl(Syntax, *Shape)
			// 束縛を保つ分割候補の返戻
			) co_return co_await BuildCommaDecl(State, Id, *Shape);
		}
		// 表示の境界を持つドキュメントの返戻
		if(Has(Role::MarkupContainer)) co_return co_await BuildMarkup(State, Id);
		// タグと属性の平坦形の返戻
		if(Has(Role::MarkupSyntax)) co_return co_await BuildFlat(State, Id);
		Syntax.Charge(1);
		const uint32_t ContentCount = State.Contexts[Id].ContentCount;
		// Ruby の連結文字列の独立した文への誤読を避ける平坦形の返戻
		if(Has(Role::UnbrokenStrSeq)) co_return co_await BuildFlat(State, Id);
		// 本体の展開要因の判定
		const bool HasStmtBlock = HasExpansionReason(State, Id);
		bool ShouldExpand = HasStmtBlock;
		if(HasStmtBlock && Has(Role::KeywordExpandedContainer)) {
			ShouldExpand = false;
			for(const NodeId Child : Node.Children) {
				if(!Syntax.IsNamed(Child)) continue;
				ShouldExpand = (co_await GetStructured(State, Child))->HasBreak;
				if(ShouldExpand) break;
			}
		}
		const bool IsParen = Has(Role::ValParens);
		// 展開を要しないノードの平坦形の返戻
		if(!HasStmtBlock && (Has(Role::ExprTag) || IsParen || Has(Role::TagElement))) co_return co_await BuildFlat(State, Id);
		// 展開形のコンテナ（Go の括弧式は自動のセミコロンに依る構文の崩壊を防ぐ為に汎用の経路で密着を保つ）の返戻
		if(
			ShouldExpand && (Has(Role::ExpandContainer) || Has(Role::ExpandedTypeParams)) &&
			!(Syntax.GetRules().HasAutomaticLineTerminator && IsParen)
		) co_return co_await BuildExpandedContainer(State, Id);
		// 暗黙の結果を持つ本体の子毎の配置
		if(Has(Role::ImplicitResultBody)) {
			Emitter Output(State.IR);
			uint32_t OpenHeredocs = 0;
			for(const NodeId Child : Node.Children) {
				if(!Syntax.IsNamed(Child)) continue;
				if(Syntax.Is(Child, Role::DeferredLiteralBody)) {
					Output.Append(co_await BuildHeredocBody(State, Child));
					TrackOpenHeredocs(State, Child, OpenHeredocs);
					continue;
				}
				Built Piece;
				if(OpenHeredocs) Piece = co_await BuildSealedFlat(State, Child);
				else Piece = co_await TakeStructured(State, Child);
				if(Piece.Head.Item == NoItem) continue;
				if(OpenHeredocs) AppendHeredocPendingSeparator(Syntax, Output);
				else if(!Output.IsEmpty() && !Output.IsAtLineStart()) Output.Break();
				AppendChild(State, Child, std::move(Piece), Output);
				TrackOpenHeredocs(State, Child, OpenHeredocs);
			}
			// 子毎に改行した要素の返戻
			co_return Output.Finish(Id, NoGroup);
		}
		// 行のプリプロセッサディレクティブを１論理行の儘で内部を折らない形で構築した結果（整形仕様 4.2，行分割仕様 2.1）の返戻
		if(Has(Role::PreprocLine)) co_return co_await BuildSealedFlat(State, Id);
		// プリプロセッサディレクティブの条件のブロックの返戻
		if(Has(Role::Preproc)) co_return co_await BuildPreprocBlock(State, Id);
		// Ruby の節の返戻
		if(Has(Role::KeywordSection)) co_return co_await BuildKeywordSection(State, Id, false, false);
		// Ruby のブロックの返戻
		if(Has(Role::KeywordBlock)) co_return co_await BuildKeywordBlock(State, Id);
		// 名前空間の子と区切の配置
		if(Has(Role::NamespaceLike)) {
			Emitter Output(State.IR);
			for(const NodeId Child : Node.Children) {
				if(Syntax.IsNamed(Child)) {
					if(!Output.IsEmpty() && !Output.HasTrailingSpace() && !Output.IsAtLineStart()) Output.Space();
					Output.Append(co_await TakeStructured(State, Child));
				} else if(const std::string &Token = Syntax.GetNode(Child).Lexeme; !Token.empty()) {
					// 文末の区切（PHP の `namespace A\B;` 等）は直前の名前へ密着
					if(
						!Syntax.IsTokenRole(Child, Role::StmtTerminatorToken) && !Syntax.IsTokenRole(Child, Role::CommaToken) && !Output.IsEmpty() &&
						!Output.HasTrailingSpace() && !Output.IsAtLineStart()
					) Output.Space();
					Output.Append(BuildLeaf(Syntax, State.IR, Child));
				}
			}
			// 名前空間の要素の返戻
			co_return Output.Finish(Id, NoGroup);
		}
		// 透過する親とインデントした本体の文の配置
		if(Has(Role::Transparent) || Has(Role::IndentedBody)) {
			Emitter Output(State.IR);
			bool IsAfterLabel = false;
			NodeId Prev = NoNode;
			uint32_t OpenHeredocs = 0;
			for(const NodeId Child : Node.Children) {
				const bool IsNamed = Syntax.IsNamed(Child);
				// 文の区切の `;` 以外の無名の子も文其の物
				if(!IsNamed && !Syntax.Is(Child, Role::EmptyStmt) && Syntax.IsTokenRole(Child, Role::StmtTerminatorToken)) continue;
				// 後送本文が持つ先頭の改行を使い，文字列の値へ改行を足さない
				if(Syntax.Is(Child, Role::DeferredLiteralBody)) {
					Output.Append(co_await BuildHeredocBody(State, Child));
					TrackOpenHeredocs(State, Child, OpenHeredocs);
					IsAfterLabel = false;
					Prev = Child;
					continue;
				}
				Built Piece;
				if(OpenHeredocs) Piece = co_await BuildSealedFlat(State, Child);
				else if(IsNamed) Piece = co_await TakeStructured(State, Child);
				else Piece = BuildLeaf(Syntax, State.IR, Child);
				if(Piece.Head.Item == NoItem) continue;
				if(!Output.IsEmpty()) {
					// エラー領域に接する境界は原文の間隙を保つ
					if(const SemanticBoundary *const Preserved = GetErrorGapBefore(Syntax, Prev, Child)) {
						AppendPreservedGap(Syntax, Prev, *Preserved, Output);
					} else if(HasProcessedCodeBoundary(State, Prev, Child, Output)) {} else if(OpenHeredocs) {
						AppendHeredocPendingSeparator(Syntax, Output);
					} else if(IsAfterLabel) Output.Space();
					else if(CanJoinAcrossNewline(Syntax, Output.GetStore(), Piece.Head)) {
						Output.GetText(Syntax, GeneratedToken::StmtTerminator);
						Output.Break();
					} else Output.Break();
				}
				AppendChild(State, Child, std::move(Piece), Output);
				TrackOpenHeredocs(State, Child, OpenHeredocs);
				IsAfterLabel = Syntax.Is(Child, Role::PrefixLabel) || IsAfterLabel && Syntax.Is(Child, Role::TransparentLabelPrefix);
				Prev = Child;
			}
			// 透過する親の文を分けた要素の返戻
			co_return Output.Finish(Id, NoGroup);
		}
		// 継続括弧を選べる式の平坦形の返戻
		if(Has(Role::ContinuationExpr)) co_return co_await BuildFlat(State, Id);
		if(Node.Children.size() < 2) {
			if(ContentCount == 1) {
				Emitter Output(State.IR);
				Output.Append(co_await TakeStructured(State, Node.Children.front()));
				// 唯一の子で組み立てた要素の返戻
				co_return Output.Finish(Id, NoGroup);
			}
			// 構造の展開の要らないノードの平坦形の返戻
			co_return co_await BuildFlat(State, Id);
		}
		// 文の役割を持つ内容子の無いノードの平坦形の返戻
		if(!ContentCount) co_return co_await BuildFlat(State, Id);
		if(Has(Role::ClosureBlock)) {
			bool HasMulti = false;
			for(const NodeId Child : Node.Children) {
				HasMulti =
				HasMulti || Syntax.IsNamed(Child) && Syntax.Is(Child, Role::AmbiguousBodyContainer) && GetBodyStmtCount(State, Child) > 1;
			}
			// 単一の本体の Ruby のブロックの平坦形の返戻
			if(!HasMulti) co_return co_await BuildFlat(State, Id);
		}
		// 汎用の構造化の返戻
		co_return co_await BuildGenericStructure(
			State,
			Id,
			HasStmtBlock,
			ShouldExpand,
			Has(Role::ThenSection) || Has(Role::IterationSection) || Has(Role::Else) || Has(Role::EnsureSection) || Has(Role::RescueSection)
		);
	}

	/**
	 * ノードの空行の規則を共有領域へ記録する関数
	 * 既存の規則は同じ位置で更新し，初めて記録するノードだけ領域を使う
	 * 計算量：償却 O(1)
	 * @param State 構築中の状態
	 * @param Id 空行の規則を持つノード
	 * @param Rule 此のノードの前に置く空行の規則
	 * @throws std::bad_alloc 規則の共有領域を確保出来ない場合
	 * @throws std::length_error 規則の数が共有領域の最大長を超える場合
	 */
	void Canonicalizer::StoreBlankRule(Builder &State, const NodeId Id, const BlankRule Rule) {
		BlankRule *&Stored = State.Contexts[Id].BlankBefore;
		if(Stored) *Stored = Rule;
		else {
			State.BlankRules.push_back(Rule);
			Stored = &State.BlankRules.back();
		}
		// 終了
		return;
	}

	/**
	 * 空行の規則の収集関数（整形仕様 6.5）
	 * 最上位と型のメンバの並びで，構造種別の違い・複数行の宣言・裸のブロックの後に空行を置く規則を，後の宣言（前置のアノテーションが有れば其の先頭）へ記録する
	 * 計算量：辿る子の総数 V に対し O(V)
	 * @param State 構築中の状態
	 * @param Container メンバの並びのコンテナ
	 * @param IsRootCall 根の並びか（裸の波括弧の中の解析断片を読み飛ばす）
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::CollectBlankRules(Builder &State, const NodeId Container, const bool IsRootCall) {
		// ネストの宣言列を辿る作業枠の準備
		const SyntaxModel &Syntax = State.Syntax;
		// メンバの空行を集める作業枠
		struct Frame {
			NodeId Container;
			bool IsRootCall;
			std::vector<NodeId> Children {};
			size_t Next = 0;
			NodeId PendingClose = NoNode; // 条件付ディレクティブの終端の空行を記録するノード
			bool IsReady = false;
			bool HasPrev = false;
			bool IsPendingBlock = false; // 裸のブロックの後に空行を置くか
			bool IsAfterInlineOpen = false; // 地の文との同行のコード開始直後か
			bool IsPrevForced = false; // 直前の項目が必須の複数行か
			uint32_t PrevBlankKey = 0; // 直前の宣言の空行比較の値
			NodeId Prev = NoNode;
			NodeId FirstAttach = NoNode; // 次の実体へ付く前置項目の先頭
			uint32_t BraceDepth = 0; // 根の解析断片の裸の波括弧の深さ
		};
		std::vector<Frame> Stack;
		// 文・宣言の空行は要素の並びの内部へ登録しない
		if(
			Syntax.GetNode(Container).Facts.TokenSpacing == TokenSpacingMode::None && !Syntax.Is(Container, Role::MarkupContainer) && !(
				State.Contexts[Container].IsMemberSequence && !State.Contexts[Container].HasStatementBody &&
				!State.Contexts[Container].HasAccessSection
			) && !State.Contexts[Container].HasBlankRules
		) {
			State.Contexts[Container].HasBlankRules = true;
			Stack.push_back({ Container, IsRootCall });
		}
		while(!Stack.empty()) {
			Frame &Cur = Stack.back();
			// 列挙の宣言部を含む子の並びの取得
			if(!Cur.IsReady) {
				Cur.IsReady = true;
				for(const NodeId Child : Syntax.GetChildrenOf(Cur.Container)) {
					Syntax.Charge(1);
					if(Syntax.Is(Child, Role::EnumBodyDecls)) {
						const std::span<const NodeId> Inner = Syntax.GetChildrenOf(Child);
						Syntax.Charge(Inner.size());
						Cur.Children.insert(Cur.Children.end(), Inner.begin(), Inner.end());
					} else Cur.Children.push_back(Child);
				}
			}
			if(Cur.PendingClose != NoNode) {
				// 条件付ディレクティブの終端の空行の記録
				const NodeId Child = Cur.PendingClose;
				Cur.PendingClose = NoNode;
				if(Syntax.Is(Child, Role::PreprocConditional)) if(const uint32_t Count = Syntax.GetChildCount(Child); Count) {
					StoreBlankRule(State, Syntax.GetChild(Child, Count - 1), { NoNode, NoNode, true });
				}
			}
			// 完了した作業枠の除去と次の子の取得
			if(Cur.Next == Cur.Children.size()) {
				Stack.pop_back();
				continue;
			}
			Syntax.Charge(1);
			const NodeId Child = Cur.Children[Cur.Next++];
			bool &HasPrev = Cur.HasPrev, &IsPendingBlock = Cur.IsPendingBlock, &IsAfterInlineOpen = Cur.IsAfterInlineOpen;
			bool &IsPrevForced = Cur.IsPrevForced;
			uint32_t &PrevBlankKey = Cur.PrevBlankKey;
			NodeId &Prev = Cur.Prev, &FirstAttach = Cur.FirstAttach;
			uint32_t &BraceDepth = Cur.BraceDepth;
			const bool IsRootCall = Cur.IsRootCall;
			if(!Syntax.IsNamed(Child) && (!IsRootCall || !Syntax.Is(Child, Role::EmptyStmt))) {
				// 裸の波括弧の中に在る解析断片は最上位の空行の対象から除く
				if(IsRootCall && Syntax.IsTokenRole(Child, Role::OpeningBraceToken)) ++BraceDepth;
				else if(IsRootCall && Syntax.IsTokenRole(Child, Role::ClosingBraceToken) && BraceDepth && !--BraceDepth) IsPendingBlock = true;
				continue;
			}
			// 前置のアノテーションと次の実体の項目は空行無で連ねる
			if(Syntax.Is(Child, Role::AttachPrefix)) {
				if(FirstAttach == NoNode) FirstAttach = Child;
				continue;
			}
			// 直前のプロパティに属する Kotlin のアクセサは空行無の続きとし，後の項目との間を複数行の後として扱う
			if(HasPrev && Syntax.Is(Child, Role::AttachedAccessor)) {
				IsPrevForced = true;
				continue;
			}
			if(
				IsRootCall && BraceDepth || IsEmptyStmt(Syntax, Child) && (!IsRootCall || !Syntax.Is(Child, Role::EmptyStmt)) ||
				Syntax.Is(Child, Role::Error) || Syntax.Is(Child, Role::OuterText)
			) continue;
			// 前後の宣言の構造に従う空行の規則の記録
			const uint32_t BlankKey = Syntax.GetNode(Child).Facts.BlankKey;
			if(
				const bool IsInlineTextEdge = Syntax.Is(Child, Role::InlineText) || Syntax.Is(Child, Role::CodeOpener) || IsAfterInlineOpen;
				HasPrev && !IsInlineTextEdge && (
					!State.Contexts[Cur.Container].IsMemberSequence || State.Contexts[Cur.Container].HasStatementBody ||
					Syntax.Is(Prev, Role::AccessSection) || Syntax.Is(Child, Role::AccessSection)
				)
			) {
				const BlankRule Rule { Prev, Child, PrevBlankKey != BlankKey || IsPendingBlock || IsPrevForced };
				StoreBlankRule(State, FirstAttach == NoNode ? Child : FirstAttach, Rule);
			}
			FirstAttach = NoNode;
			IsPendingBlock = false;
			IsPrevForced = false;
			HasPrev = true;
			// 値を出力する `<?=` はファイルの先頭でも地の文の中の開始タグと同じく扱う
			IsAfterInlineOpen = Syntax.Is(Child, Role::EmbeddedOpeningBoundary) && (
				!Syntax.Is(Child, Role::CodeOpeningTag) || Syntax.Is(Prev, Role::LiteralText) ||
				HasSingleTokenRole(State, Child, Role::ValueOutputTag)
			);
			PrevBlankKey = BlankKey;
			Prev = Child;
			if(Syntax.Is(Child, Role::DirectiveBlankBoundary)) {
				// `#else` の最初の本体の文の前の空行
				if(Syntax.Is(Child, Role::PreprocElse)) if(const NodeId FirstBody = Syntax.GetNamedChild(Child, 0); FirstBody != NoNode) {
					StoreBlankRule(State, FirstBody, { NoNode, NoNode, true });
				}
				Cur.PendingClose = Child;
				if(!Syntax.Is(Child, Role::MarkupContainer) && !State.Contexts[Child].HasBlankRules) {
					State.Contexts[Child].HasBlankRules = true;
					Stack.push_back({ Child, false });
				}
			}
		}
		// 終了
		return;
	}

	/**
	 * 構造化の組立関数（整形仕様 6.3）
	 * 本体の文の数・宣言の構造・子の必須展開から幅に依らない改行を決め，其れ以外の子は平坦形で繋ぐ
	 * ヒアドキュメントの開始の後の改行で後続のトークンが本文へ入る場合は平坦形にする
	 * 計算量：平坦化する部分木のノード数 N に対し O(N) と各ノードの平坦形又は構造化の組立の総費用
	 * @param State 構築中の状態
	 * @param Id ノード
	 * @return 組立済の要素
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildStructured(Builder &State, const NodeId Id) {
		// 構造化の組立
		uint32_t Open = 0;
		TrackOpenHeredocs(State, Id, Open);
		// 本文の開始を変えない平坦形の返戻
		if(Open && !State.Heredocs[Id].HasPattern) return BuildSealedFlat(State, Id);
		// 構造化した形の返戻
		return BuildStructuredCore(State, Id);
	}
} // namespace Core
