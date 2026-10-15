#include "Canonicalizer.hpp"
#include "FormatIR.hpp"
#include "NodeId.hpp"
#include "SyntaxModel.hpp"
#include "SyntaxRole.hpp"
#include <cstdint>
#include <memory_resource>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 汎用の構造化の組立関数（整形仕様 6.3）
	 * 本体・文の並び・節見出の子を行へ分け，其の他の子をトークン間の空白の規則で繋ぐ
	 * 括弧の対の中身を深さの範囲に置く（整形仕様 6.5）
	 * 計算量：直接の子数 C に対し O(C) と境界判定・子の組立・出力生成の総費用
	 * @param State 構築中の状態
	 * @param Id ノード
	 * @param HasStmtBlock 展開要因を含むか
	 * @param ShouldExpand 展開するか
	 * @param IsElse Ruby の `else` 等の節か
	 * @return 組立済の要素
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildGenericStructure(
		Builder &State,
		const NodeId Id,
		const bool HasStmtBlock,
		const bool ShouldExpand,
		const bool IsElse
	) {
		// 子の形と展開に用いる構造上の役割の判定
		const SyntaxModel &Syntax = State.Syntax;
		const SyntaxNode &Node = Syntax.GetNode(Id);
		const NodeCtx &Ctx = State.Contexts[Id];
		Syntax.Charge(1);
		const uint32_t ChildCount = static_cast<uint32_t>(Node.Children.size());
		const bool IsExprTag = Syntax.Is(Id, Role::ExprTag), IsRenderedContainer = Syntax.Is(Id, Role::TagElement) && !IsExprTag;
		// Ruby 以外の class のノードは Ruby の定義として扱わない
		const bool IsObject = Syntax.Is(Id, Role::ObjectLike) && (!Syntax.Is(Id, Role::TypeMemberBody) || Ctx.IsMemberSequence);
		const bool IsForeignClass = Syntax.Is(Id, Role::NonKeywordClass);
		const bool IsBraceBodyHost = Syntax.Is(Id, Role::DirectBraceLayout) || Syntax.Is(Id, Role::DirectControlBraceLayout);
		// 見出の部品しか持たない構文は本体が空で，整形仕様 1.4 に従い畳む為にインデントのコンテナとして扱わない
		const bool IsIndentContainer =
		(Syntax.Is(Id, Role::IndentContainer) && !IsForeignClass || Syntax.Is(Id, Role::SwitchBodyLayout) || IsBraceBodyHost) &&
		(!Syntax.Is(Id, Role::KeywordBlock) || ShouldExpandKeywordBody(State, Id)) && !IsObject ||
		IsObject && ShouldExpand && Ctx.HasBraceOpen && Ctx.HasBraceClose;
		// 文の並び（Go の一括宣言は括弧で囲む物だけ）
		const bool IsStmtList = !IsObject && Syntax.Is(Id, Role::StmtList) && (!Syntax.Is(Id, Role::GroupedDecl) || Ctx.HasParenOpen);
		const bool IsSectionHeaderParent = Syntax.Is(Id, Role::SectionHeader), IsLambdaBody = Syntax.Is(Id, Role::LambdaLiteral);
		const bool IsLiteral = Syntax.Is(Id, Role::StrLikeAll) || Syntax.Is(Id, Role::InterpolatedLiteral);
		const bool IsStructural = IsIndentContainer || IsStmtList ||
		(IsLambdaBody || IsExprTag || IsLiteral || Syntax.Is(Id, Role::Interpolation)) && ShouldExpand ||
		Ctx.ContentCount && (IsRenderedContainer || IsSectionHeaderParent);
		const bool IsClassLike = Syntax.Is(Id, Role::InheritedClass) && !IsForeignClass, IsCompact = Syntax.Is(Id, Role::Compact);
		bool HasAlternateBody = false;
		if(Syntax.Is(Id, Role::AlternateBodyLayout)) for(const NodeId Child : Node.Children) {
			HasAlternateBody = HasAlternateBody ||
			!Syntax.IsNamed(Child) && Syntax.IsTokenRole(Child, Role::ColonToken) && IsAlternateBodyOpener(Syntax, Id, Child);
		}
		// 展開要因の無いノードの平坦形の選択
		if(!IsStructural && !HasStmtBlock && !HasAlternateBody) {
			// 後続節と複数行の JSX の子の確認
			bool HasMultilineElements = false;
			if(!(Ctx.HasElseChild || Ctx.HasElseGrandchild)) {
				if(Ctx.HasRenderedChild) for(const NodeId Child : Node.Children) {
					Syntax.Charge(1);
					if(!Syntax.IsNamed(Child) || !Syntax.Is(Child, Role::RenderedChildren)) continue;
					HasMultilineElements = (co_await GetStructured(State, Child))->HasBreak;
					if(HasMultilineElements) break;
				}
				// 複数行の JSX を持たない条件の平坦形の返戻
				if(!HasMultilineElements) co_return co_await BuildFlat(State, Id);
			}
		}
		// 子の配置に使う役割と境界の準備
		const bool IsSwitchEntry = Syntax.Is(Id, Role::SwitchEntry);
		const bool IsStmtsOnlyBreak = Syntax.Is(Id, Role::StmtsOnlyBreak) || IsBraceBodyHost;
		const bool IsMethodDef = Syntax.Is(Id, Role::EndDelimitedMethod), IsUnaryPrefixOrUpdate = Syntax.Is(Id, Role::UnaryPreOrUpdate);
		const bool IsDoStmt = Syntax.Is(Id, Role::DoLoop), IsIfParent = Syntax.Is(Id, Role::If);
		const bool IsArrayVal = Syntax.Is(Id, Role::ArrayLiteral), IsGroupedDecl = Syntax.Is(Id, Role::GroupedDecl);
		const bool IsTypedSwitch = Syntax.Is(Id, Role::TypedSwitchLayout), IsDelimitedStmts = Syntax.Is(Id, Role::DelimitedStmtSeq);
		const std::pmr::vector<uint8_t> Candidates = BreakBoundaries(State, Id);
		std::vector<uint32_t> OpenScopes;
		Emitter Output(State.IR);
		// 候補の境界はグループへ所属させ，平坦時に密着する括弧内も役割で合法な改行なら候補にする
		const auto SpaceAt = [&](const uint32_t Idx, const bool IsSpace) -> void {
			if(
				!Candidates.empty() && Candidates[Idx] &&
				(IsSpace || !Syntax.GetRules().IsWhitespaceSemantic || CanBreakTightBoundary(Syntax, Id, Idx)) &&
				!(Syntax.Is(Id, Role::KeywordValAssignment) && Syntax.Is(Syntax.GetChild(Id, Idx), Role::KeywordBlock))
			) Output.Soft(IsSpace ? " " : "");
			else if(IsSpace) Output.Space();
		};
		NodeId PrevChild = NoNode;
		uint32_t ContentSeen = 0, OpenHeredocs = 0;
		// 頭に保つ原文の間隙（文法のトークンに無いテキスト，整形仕様 4.4）
		if(const SemanticBoundary *const Head = GetPreservedAt(Syntax, Id, 0)) Output.Gap(*Head);
		bool IsAfterInherit = false, IsAlternateBody = false, HasPendingOpeningBreak = false;
		EdgeItem AlternateBodyOpenEnd;
		// 文の役割を持つ内容子と，其の他のトークンの境界の順次配置
		for(uint32_t ChildIdx = 0; ChildIdx < ChildCount; ++ChildIdx) {
			const NodeId Child = Node.Children[ChildIdx];
			// 括弧の対の閉じは，其の前の区切の前に深さの範囲を閉じる（構造の閉じは下で扱う）
			const bool IsScopeCloser = !OpenScopes.empty() && OpenScopes.back() == ChildIdx;
			if(IsScopeCloser) OpenScopes.pop_back();
			// JavaScript / TypeScript のデコレータの直後の必須改行
			if(!Output.IsEmpty() && !Output.IsAtLineStart() && Syntax.Is(PrevChild, Role::DeclDecorator)) Output.Break();
			// 本体の前の見出は独自のグループとし，本体の未評価境界を保って先に測る
			const bool IsOptionalBody = State.Contexts[Child].IsOptionalBody;
			const bool IsDirectBodyOpen =
			Syntax.Is(Id, Role::DirectControlBraceLayout) && Syntax.IsTokenRole(Child, Role::OpeningBraceToken);
			if((IsOptionalBody || IsDirectBodyOpen) && OpenScopes.empty()) {
				Built Header = Output.Finish(NoNode, State.NextGroup++);
				Output = Emitter(State.IR);
				Output.Append(std::move(Header));
			}
			if(Syntax.IsNamed(Child) && !Syntax.GetNode(Child).Facts.DelimiterDef || Syntax.Is(Child, Role::EmptyStmt)) {
				if(IsScopeCloser) Output.CloseScope();
				// Ruby のヒアドキュメントの本体は終端の語が独自の行頭・行末を要する為，逐語の儘前後の改行を強いる
				if(Syntax.Is(Child, Role::DeferredLiteralBody)) {
					Output.Append(co_await BuildHeredocBody(State, Child));
					TrackOpenHeredocs(State, Child, OpenHeredocs);
					++ContentSeen;
					PrevChild = Child;
					continue;
				}
				// ヒアドキュメントの本文を控えた開始行の続きは折らない（行分割仕様 2.3）
				Built Subtree;
				if(OpenHeredocs) Subtree = co_await BuildSealedFlat(State, Child);
				else if(State.Contexts[Child].Structured) Subtree = ReleaseStructured(State, Child);
				else if(Syntax.GetNode(Child).Children.empty() && !State.Contexts[Child].ShouldWrap) {
					Subtree = BuildLeaf(Syntax, State.IR, Child);
				} else Subtree = co_await BuildStructured(State, Child);
				// 行幅で波括弧を付ける単一の文の制御本体のグループ（整形仕様 6.1）
				if(IsOptionalBody) Subtree = BuildBraceGroup(State, Id, Child, std::move(Subtree));
				if(Subtree.Head.Item == NoItem) continue;
				if(HasPendingOpeningBreak) {
					Output.Break();
					HasPendingOpeningBreak = false;
				}
				const SemanticBoundary *const Preserved = ChildIdx ? GetPreservedBefore(Syntax, Child) : nullptr;
				// JSX の連続する本文と，Rust の式の終端と Java の列挙の宣言部の先頭のセミコロンは前へ密着
				if(
					IsRenderedContainer && Syntax.Is(Child, Role::RenderedText) && Syntax.Is(PrevChild, Role::RenderedText) && !Preserved ||
					ContentSeen && (Syntax.Is(Child, Role::AttachedEmptyStmt) || Syntax.Is(Child, Role::EnumBodyDecls))
				) {
					AppendChild(State, Child, std::move(Subtree), Output);
					++ContentSeen;
					PrevChild = Child;
					continue;
				}
				if(
					(!Preserved || Preserved->Src != PreservedSrc::RenderedLineEdges) && ShouldBreakAfterExpandedBrace(Syntax, Subtree.Head, Output)
				) Output.Break();
				// 型宣言の型引数の開始の記号の分離
				if(IsClassLike && IsEdgeRole(Syntax, State.IR, Subtree.Head, Role::LessThanToken)) {
					if(!Output.IsEmpty() && !Output.HasTrailingSpace()) Output.Space();
					AppendChild(State, Child, std::move(Subtree), Output);
					++ContentSeen;
					PrevChild = Child;
					continue;
				}
				const bool IsChildElseLike =
				(Syntax.Is(Child, Role::ElseLikeClause) || Syntax.Is(Child, Role::Clause)) && !Syntax.Is(Id, Role::GuardStmt);
				const bool HasBodyClauseBoundary = IsChildElseLike && PrevChild != NoNode && State.Contexts[PrevChild].IsOptionalBody;
				if(IsDelimitedStmts && OpenHeredocs) AppendHeredocPendingSeparator(Syntax, Output);
				// エラー領域に接する境界と読み飛ばしたトークンは原文の間隙をコピーする
				else if(Preserved) AppendPreservedGap(Syntax, PrevChild, *Preserved, Output);
				else if(HasProcessedCodeBoundary(State, PrevChild, Child, Output)) {} else if(
					Syntax.Is(PrevChild, Role::HardLineDirective) || Syntax.Is(PrevChild, Role::PreprocLine) || IsAlternateBody ||
					HasTrailingColonBody(State, PrevChild)
				) {
					if(!Output.IsEmpty() && !Output.IsAtLineStart()) Output.Break();
				} else if(Syntax.Is(Child, Role::IndentedBody)) {
					Output.OpenScope();
					if(!Output.IsEmpty() && !Output.IsAtLineStart()) Output.Break();
				} else if(Syntax.Is(PrevChild, Role::IndentedBody)) {
					if(!Output.IsEmpty() && !Output.IsAtLineStart()) Output.Break();
				} else if(IsClassLike && IsAfterInherit) {
					IsAfterInherit = false;
					if(!Output.HasTrailingSpace()) Output.Space();
				} else if(
					const bool ShouldJoinClause = IsChildElseLike && !HasBodyClauseBoundary && IsBlockClose(Syntax, State.IR, Output.GetLast());
					!Output.IsEmpty() && !Output.IsAtLineStart() && !ShouldJoinClause && IsChildElseLike && !HasBodyClauseBoundary
				) Output.Break();
				// ラムダの本体の文の並びは引数の後で改行して始める
				else if(
					IsLambdaBody && Syntax.Is(Child, Role::ValStmtSeq) ||
					IsDelimitedStmts && ChildIdx && (Syntax.IsNamed(PrevChild) || Syntax.Is(PrevChild, Role::EmptyStmt))
				) {
					if(!Output.IsEmpty() && !Output.IsAtLineStart()) Output.Break();
				} else if(IsStmtList) {
					// Java の enum の宣言部の最初のメンバの `;` の後の行への配置
					if((ContentSeen || Syntax.Is(Id, Role::EnumBodyDecls)) && !Output.IsEmpty() && !Output.IsAtLineStart()) Output.Break();
					else if(!Output.IsEmpty() && !Output.IsAtLineStart() && !Output.HasTrailingSpace()) SpaceAt(ChildIdx, true);
				} else if(
					// `(` で始まる子を直前の識別子へ密着
					IsStructural && !HasBodyClauseBoundary && !Output.IsEmpty() && !Output.IsAtLineStart() && !(
						IsRenderedContainer &&
						(GetEdgeChar(State.IR, Output.GetLast(), false) == '\t' || GetEdgeChar(State.IR, Subtree.Head, true) == '\t')
					) && (!Syntax.Is(Id, Role::RenderedChildren) || !Candidates.empty() && Candidates[ChildIdx]) && (
						!IsLambdaBody && ContentSeen && !(
							IsEdgeRole(Syntax, State.IR, Subtree.Head, Role::OpeningParenToken, GeneratedToken::CallOpen) && (
								Syntax.GetRules().IsFusionAlnum(GetEdgeChar(State.IR, Output.GetLast(), false)) ||
								Output.IsTailRole(Syntax, Role::AssignmentToken) || Output.IsTailRole(Syntax, Role::QuestionToken) ||
								Output.IsTailRole(Syntax, Role::ExclamationToken)
							) && !(IsMethodDef && Syntax.Is(Child, Role::ImplicitResultBody))
						) && !ShouldJoinClause && !(IsStmtsOnlyBreak && !Syntax.Is(Child, Role::ValStmtSeq)) &&
						!(IsTypedSwitch && !Syntax.Is(Child, Role::SectionHeader)) &&
						!(IsMethodDef && !Syntax.Is(Child, Role::AmbiguousBodyContainer)) ||
						!ContentSeen && IsSectionHeaderParent && (Output.IsTailRole(Syntax, Role::ColonToken) || IsElse)
					)
				) Output.Break();
				// 密集したノードの中の語の単項演算子と被演算子の空白区切
				else if(
					!HasBodyClauseBoundary && (!IsCompact || IsUnaryPrefixOrUpdate && !Syntax.IsNamed(PrevChild)) && !Output.IsEmpty() &&
					!Output.IsAtLineStart() && !Output.HasTrailingSpace() && ChildIdx
				) {
					if(Syntax.Is(PrevChild, Role::PreprocBranch)) Output.Break();
					else if(
						IsUnaryPrefixOrUpdate && !Syntax.IsNamed(PrevChild) && Syntax.GetRules().IsFusionLetter(Syntax.GetFirstChar(PrevChild)) ||
						ShouldInsertSpace(State, Id, PrevChild, Child, Syntax.GetNode(Id).Facts.GetBoundaryBefore(ChildIdx))
					) SpaceAt(ChildIdx, true);
					else SpaceAt(ChildIdx, false);
				} else if(!HasBodyClauseBoundary && ChildIdx && !Output.IsAtLineStart() && !Output.HasTrailingSpace()) SpaceAt(ChildIdx, false);
				AppendChild(State, Child, std::move(Subtree), Output);
				if(Syntax.Is(Child, Role::IndentedBody)) Output.CloseScope();
				if(State.Contexts[Child].ScopeCloser < ChildCount) {
					Output.OpenScope();
					OpenScopes.push_back(State.Contexts[Child].ScopeCloser);
				}
				if(IsDelimitedStmts) TrackOpenHeredocs(State, Child, OpenHeredocs);
				++ContentSeen;
				PrevChild = Child;
				continue;
			}
			// PHP の代替構文：本体を開く `:` は見出へ密着させ，終端の語は行頭に置く（地の文の直後は同じ行）
			if(Syntax.Is(Id, Role::AlternateBodyLayout) || Syntax.Is(Child, Role::ColonBodyEnd)) {
				if(Syntax.IsTokenRole(Child, Role::ColonToken) && IsAlternateBodyOpener(Syntax, Id, Child)) {
					Output.OmitSpace();
					Output.Append(BuildLeaf(Syntax, State.IR, Child));
					IsAlternateBody = true;
					AlternateBodyOpenEnd = Output.GetLast();
					Output.OpenScope();
					PrevChild = Child;
					continue;
				}
				if(Syntax.Is(Child, Role::ColonBodyEnd)) {
					if(IsAlternateBody) Output.CloseScope();
					IsAlternateBody = false;
					if(!Output.IsEmpty() && !Output.IsAtLineStart() && !Output.HasTrailingSpace()) {
						if(Syntax.Is(PrevChild, Role::CodeTextBoundary) || Output.GetLast().Item == AlternateBodyOpenEnd.Item) Output.Space();
						else Output.Break();
					}
					Output.Append(BuildLeaf(Syntax, State.IR, Child));
					PrevChild = Child;
					continue;
				}
			}
			// Swift の fallthrough は文の並びの外に無名のトークンとして現れる事も有る
			if(IsSwitchEntry && HasSingleTokenRole(State, Child, Role::FallthroughKeyword)) {
				if(!Output.IsEmpty() && !Output.IsAtLineStart()) Output.Break();
				Output.Append(BuildLeaf(Syntax, State.IR, Child));
				PrevChild = Child;
				continue;
			}
			// テキストの無いトークンの省略
			if(Syntax.GetNode(Child).Lexeme.empty()) {
				if(IsScopeCloser) Output.CloseScope();
				continue;
			}
			// Go の構文エラーを含む子の後のトークンは原文の間隙をコピーする（誤読したブロックの閉じの前へ改行を足すと自動の `;` が式を切る）
			if(
				const SemanticBoundary *const Preserved =
				Syntax.GetRules().HasAutomaticLineTerminator && ChildIdx ? GetPreservedBefore(Syntax, Child) : nullptr
			) {
				if(IsScopeCloser) Output.CloseScope();
				AppendPreservedGap(Syntax, PrevChild, *Preserved, Output);
				Output.Append(BuildLeaf(Syntax, State.IR, Child));
				PrevChild = Child;
				continue;
			}
			// 通常のトークンと型引数の配置
			Built TokenPiece = BuildLeaf(Syntax, State.IR, Child);
			if(ShouldBreakAfterExpandedBrace(Syntax, TokenPiece.Head, Output)) Output.Break();
			if(IsCompact) {
				if(IsScopeCloser) Output.CloseScope();
				Output.Append(std::move(TokenPiece));
				PrevChild = Child;
				continue;
			}
			if(IsClassLike && Syntax.IsTokenRole(Child, Role::LessThanToken)) {
				IsAfterInherit = true;
				Output.Space();
				Output.Append(std::move(TokenPiece));
				Output.Space();
				PrevChild = Child;
				continue;
			}
			// 構造の開き・閉じ（本体の波括弧，JSON の配列，Go の一括宣言の括弧）
			const bool IsStructuralOpen = IsStructural && (
				Syntax.IsTokenRole(Child, Role::OpeningBraceToken) || IsArrayVal && Syntax.IsTokenRole(Child, Role::OpeningBracketToken) ||
				IsGroupedDecl && Syntax.IsTokenRole(Child, Role::OpeningParenToken)
			);
			if(
				const bool IsStructuralClose = IsStructural && (
					Syntax.IsTokenRole(Child, Role::ClosingBraceToken) || Syntax.Is(Child, Role::CloserWord) ||
					IsArrayVal && Syntax.IsTokenRole(Child, Role::ClosingSquareToken) ||
					IsGroupedDecl && Syntax.IsTokenRole(Child, Role::ClosingParenToken)
				);
				IsStructuralClose
			) {
				HasPendingOpeningBreak = false;
				const bool IsBroken = !Output.IsEmpty() && !Output.IsAtLineStart() && !IsEdgeBracket(Syntax, State.IR, Output.GetLast(), true);
				// Go の複数行の並びの閉じの前の末尾のカンマ
				if(IsBroken && TrailingCommaForm(Syntax, Id, PrevChild, Child, IsBroken)) {
					Output.GetText(Syntax, GeneratedToken::ListSeparator);
					State.IR.EditDetail(Output.GetLast().Item).PunctuationOwner = Id;
				}
				if(IsScopeCloser) Output.CloseScope();
				if(IsBroken) Output.Break();
				AppendChild(State, Child, std::move(TokenPiece), Output);
				PrevChild = Child;
				continue;
			}
			if(HasPendingOpeningBreak) {
				Output.Break();
				HasPendingOpeningBreak = false;
			}
			if(IsScopeCloser) Output.CloseScope();
			// Go の折り返した並びの閉じの前の末尾のカンマ（グループの展開時だけ出す）
			if(
				!Candidates.empty() && Candidates[ChildIdx] && IsEdgeBracket(Syntax, State.IR, TokenPiece.Head, false) &&
				TrailingCommaForm(Syntax, Id, PrevChild, Child, true)
			) {
				const FormatItemId Comma = GeneratedItem(Syntax, State.IR, GeneratedToken::ListSeparator);
				State.IR.EditDetail(Comma).PunctuationOwner = Id;
				Output.IfBroken({ Comma });
			}
			if(
				const bool HasBodyClauseBoundary = PrevChild != NoNode && State.Contexts[PrevChild].IsOptionalBody &&
				(Syntax.Is(Child, Role::ElseKeyword) && IsIfParent || IsDoStmt && HasSingleTokenRole(State, Child, Role::KeywordWhile));
				!HasBodyClauseBoundary && ChildIdx && !Output.IsEmpty() && !Output.IsAtLineStart() && !Output.HasTrailingSpace() && !(
					IsStructural && IsExprTag && ContentSeen > 1 &&
					(Syntax.IsTokenRole(Child, Role::GreaterThanToken) || Syntax.IsTokenRole(Child, Role::SelfClosingTagToken))
				)
			) {
				// プリプロセッサの分岐の後は必ず改行
				// ブロックの閉じで終わらない `do` 本体と，後続の `while` の間は改行
				// ブロックの閉じで終わらない分岐本体に続く `else` は，値の `if` の展開状態に従い改行を選択
				if(
					Syntax.Is(PrevChild, Role::PreprocBranch) || !IsBlockClose(Syntax, State.IR, Output.GetLast()) && (
						IsDoStmt && HasSingleTokenRole(State, Child, Role::KeywordWhile) || Syntax.Is(Child, Role::ElseKeyword) && IsIfParent &&
						(!Syntax.Is(Id, Role::BracedConditionalVal) || ShouldExpand && !IsArrowValBody(State, Id))
					)
				) Output.Break();
				else SpaceAt(ChildIdx, ShouldInsertSpace(State, Id, PrevChild, Child, Syntax.GetNode(Id).Facts.GetBoundaryBefore(ChildIdx)));
			}
			if(
				IsStructural && IsExprTag && ContentSeen > 1 &&
				(Syntax.IsTokenRole(Child, Role::GreaterThanToken) || Syntax.IsTokenRole(Child, Role::SelfClosingTagToken))
			) {
				// 属性を展開した開タグ又は自己閉タグの閉じのトークンの独立した行への配置
				Output.OmitSpace();
				if(!Output.IsEmpty() && !Output.IsAtLineStart()) Output.Break();
				Output.Append(std::move(TokenPiece));
				PrevChild = Child;
				continue;
			}
			Output.Append(std::move(TokenPiece));
			// 括弧の対の開きの後の深さの範囲（構造の開きは改行も範囲の中へ置く）
			if(State.Contexts[Child].ScopeCloser < ChildCount) {
				Output.OpenScope();
				OpenScopes.push_back(State.Contexts[Child].ScopeCloser);
			}
			const SemanticBoundary *const Following = Node.Facts.GetBoundaryBefore(ChildIdx + 1);
			HasPendingOpeningBreak =
			IsStructuralOpen && ChildIdx + 1 < ChildCount && !IsLambdaBody && !(Following && Following->Kind == BoundaryKind::NoBreak);
			PrevChild = Child;
		}
		// 尾に保つ原文の間隙
		if(const SemanticBoundary *const Tail = GetPreservedAt(Syntax, Id, ChildCount)) Output.Gap(*Tail);
		// 子を順に繋いだ要素の返戻
		co_return Output.Finish(Id, State.NextGroup++);
	}
} // namespace Core
