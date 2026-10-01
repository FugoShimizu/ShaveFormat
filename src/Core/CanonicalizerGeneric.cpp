#include "Canonicalizer.hpp"
#include <cstdint>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 汎用の構造化の組立関数（整形仕様 6.3）
	 * 本体・文の並び・節見出の子を行へ分け，其の他の子を字句間の空白の規則で繋ぐ
	 * 括弧の対の中身を深さの範囲に置く（整形仕様 6.5）
	 * @param State 構築中の状態
	 * @param Id 節点
	 * @param HasStmtBlock 展開要因を含むか
	 * @param ShouldExpand 展開するか
	 * @param IsElse Ruby の `else` 等の節か
	 * @return 組立済の要素
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
		const uint32_t ChildCount = static_cast<uint32_t>(Node.Children.size()), NamedCount = Syntax.NamedChildCount(Id);
		const bool IsExpressionTag = Syntax.Is(Id, Role::ExpressionTag);
		const bool IsRenderedContainer = Syntax.Is(Id, Role::TagElement) && !IsExpressionTag;
		const bool IsSwitchBodyContainer = Syntax.Is(Id, Role::SwitchBodyLayout), IsObject = Syntax.Is(Id, Role::ObjectLike);
		bool IsObjectOpen = false, IsObjectClose = false;
		if(IsObject && ShouldExpand) {
			IsObjectOpen = Syntax.HasTokenRoleChild(Id, Role::OpeningBraceToken);
			IsObjectClose = Syntax.HasTokenRoleChild(Id, Role::ClosingBraceToken);
		}
		// Ruby 以外の class の節点は Ruby の定義として扱わない
		const bool IsForeignClass = Syntax.Is(Id, Role::NonKeywordClass);
		const bool IsBraceBodyHost = Syntax.Is(Id, Role::DirectBraceLayout) || Syntax.Is(Id, Role::DirectControlBraceLayout);
		// 見出の部品しか持たない構文は本体が空で，整形仕様 1.4 に従い畳む為にインデントの容器として扱わない
		const bool IsIndentContainer =
		(Syntax.Is(Id, Role::IndentContainer) && !IsForeignClass || IsSwitchBodyContainer || IsBraceBodyHost) &&
		(!Syntax.Is(Id, Role::KeywordBlock) || KeepsKeywordBodyExpanded(Syntax, Id)) && !IsObject || IsObjectOpen && IsObjectClose;
		// 文の並び（Go の一括宣言は括弧で囲む物だけ）
		const bool IsStatementList = Syntax.Is(Id, Role::StatementList) &&
		(!Syntax.Is(Id, Role::GroupedDeclaration) || Syntax.HasTokenRoleChild(Id, Role::OpeningParenToken));
		const bool IsSectionHeaderParent = Syntax.Is(Id, Role::SectionHeader), IsLambdaBody = Syntax.Is(Id, Role::LambdaLiteral);
		const bool IsStructural = IsIndentContainer || IsStatementList || (IsLambdaBody || IsExpressionTag) && ShouldExpand ||
		NamedCount && (IsRenderedContainer || IsSectionHeaderParent);
		const bool IsClassLike = Syntax.Is(Id, Role::InheritedClass) && !IsForeignClass, IsCompact = Syntax.Is(Id, Role::Compact);
		bool HasAlternateBody = false;
		if(Syntax.Is(Id, Role::AlternateBodyLayout)) for(const NodeId Child : Node.Children) {
			HasAlternateBody = HasAlternateBody ||
			!Syntax.IsNamed(Child) && Syntax.IsTokenRole(Child, Role::ColonToken) && IsAlternateBodyOpener(Syntax, Id, Child);
		}
		// 展開要因の無い節点の平坦形の選択
		if(!IsStructural && !HasStmtBlock && !HasAlternateBody) {
			// else 系の節を持たず，複数行の JSX の子も持たない節点の平坦形の返戻
			bool HasElseLike = false, HasMultilineElements = false;
			for(const NodeId Child : Node.Children) {
				HasElseLike = HasElseLike || Syntax.Is(Child, Role::ElseLikeClause);
				for(const NodeId Grandchild : Syntax.ChildrenOf(Child)) {
					HasElseLike = HasElseLike || Syntax.Is(Grandchild, Role::ElseLikeClause);
				}
			}
			if(!HasElseLike) {
				for(const NodeId Child : Node.Children) {
					HasMultilineElements = HasMultilineElements ||
					Syntax.IsNamed(Child) && Syntax.Is(Child, Role::RenderedChildren) && (co_await GetStructured(State, Child))->HasBreak;
				}
				// 複数行の JSX を持たない条件の平坦形の返戻
				if(!HasMultilineElements) co_return Built(*co_await GetFlat(State, Id));
			}
		}
		// 子の配置に使う役割と境界の準備
		const bool IsSwitchEntry = Syntax.Is(Id, Role::SwitchEntry);
		const bool IsStatementsOnlyBreak = Syntax.Is(Id, Role::StatementsOnlyBreak) || IsBraceBodyHost;
		const bool IsMethodDefinition = Syntax.Is(Id, Role::EndDelimitedMethod);
		const bool IsUnaryPreOrUpdateNode = Syntax.Is(Id, Role::UnaryPreOrUpdate), IsDoStatementNode = Syntax.Is(Id, Role::DoLoop);
		const bool IsIfParentNode = Syntax.Is(Id, Role::If), IsArrayValue = Syntax.Is(Id, Role::ArrayLiteral);
		const bool IsGroupedDeclaration = Syntax.Is(Id, Role::GroupedDeclaration);
		const bool IsTypedSwitch = Syntax.Is(Id, Role::TypedSwitchLayout);
		const bool IsDelimitedStatements = Syntax.Is(Id, Role::DelimitedStatementSequence);
		const std::vector<uint8_t> Candidates =
		State.NoBreakDepth ? std::vector<uint8_t>(ChildCount + 1, 0) : BreakBoundaries(Syntax, Id);
		const std::vector<uint32_t> Closers = ScopeClosers(Syntax, Id);
		std::vector<uint32_t> OpenScopes;
		Emitter Out(State.IR);
		// 候補の境界での空白（選択可能な境界は群へ所属させ，CSS の空白の無い境界は改行が新しい意味上の空白に為る為に候補にしない）
		const auto SpaceAt = [&](const uint32_t Index, const bool IsSpace) -> void {
			if(
				Candidates[Index] && (IsSpace || !Syntax.Rules().IsWhitespaceSemantic) &&
				!(Syntax.Is(Id, Role::KeywordValueAssignment) && Syntax.Is(Syntax.Child(Id, Index), Role::KeywordBlock))
			) Out.Soft(IsSpace ? " " : "");
			else if(IsSpace) Out.Space();
		};
		NodeId PrevChild = NoNode;
		uint32_t NamedSeen = 0, OpenHeredocs = 0;
		// 頭に保つ原文の間隙（文法の字句に無い字面，整形仕様 4.4）
		if(const SemanticBoundary *const Head = PreservedAt(Syntax, Id, 0)) Out.Gap(*Head);
		bool IsAfterInherit = false, IsAlternateBody = false;
		EdgeItem AlternateBodyOpenEnd;
		// 名前付の子と字句の境界の順次配置
		for(uint32_t ChildIdx = 0; ChildIdx < ChildCount; ++ChildIdx) {
			const NodeId Child = Node.Children[ChildIdx];
			// 括弧の対の閉じは，其の前の区切りの前に深さの範囲を閉じる（構造の閉じは下で扱う）
			const bool IsScopeCloser = !OpenScopes.empty() && OpenScopes.back() == ChildIdx;
			if(IsScopeCloser) OpenScopes.pop_back();
			// JS / TS のデコレータ (`@Foo`) の直後は必ず改行する
			if(!Out.IsEmpty() && !Out.IsAtLineStart() && Syntax.Is(PrevChild, Role::DeclarationDecorator)) Out.Break();
			// 行幅で波括弧を付ける本体の前の見出は独自の群とし，本体の群より先に評価する（見出を折る前に本体を波括弧で包む）
			const bool IsOptionalBody = IsBraceOptionalBody(Syntax, Id, Child);
			if(IsOptionalBody && OpenScopes.empty()) {
				Built Header = Out.Finish(NoNode, State.NextGroup++);
				Out = Emitter(State.IR);
				Out.Append(std::move(Header));
			}
			if(Syntax.IsNamed(Child)) {
				if(IsScopeCloser) Out.CloseScope();
				// Ruby のヒアドキュメントの本体は終端の語が独自の行頭・行末を要する為，逐語の儘前後の改行を強いる
				if(Syntax.Is(Child, Role::DeferredLiteralBody)) {
					co_await AppendHeredocBody(State, Child, Out);
					TrackOpenHeredocs(State, Child, OpenHeredocs);
					++NamedSeen;
					PrevChild = Child;
					continue;
				}
				// ヒアドキュメントの本文を控えた開始行の続きは折らない（行分割仕様 2.3）
				Built Sub = OpenHeredocs ? co_await BuildSealedFlat(State, Child) : co_await TakeStructured(State, Child);
				// 行幅で波括弧を付ける単一の文の制御本体の群（整形仕様 6.1）
				if(IsOptionalBody) Sub = BuildBraceGroup(State, Id, Child, std::move(Sub));
				if(Sub.Head.Item == NoItem) {
					++NamedSeen;
					PrevChild = Child;
					continue;
				}
				// JSX の連続する本文と，Rust の式の終端と Java の列挙の宣言部の先頭のセミコロンは前へ密着
				if(
					IsRenderedContainer && Syntax.Is(Child, Role::RenderedText) && Syntax.Is(PrevChild, Role::RenderedText) ||
					NamedSeen && (Syntax.Is(Child, Role::AttachedEmptyStatement) || Syntax.Is(Child, Role::EnumBodyDeclarations))
				) {
					AppendChild(State, Child, std::move(Sub), Out);
					++NamedSeen;
					PrevChild = Child;
					continue;
				}
				if(ShouldBreakAfterExpandedBrace(Syntax, Sub.Head, Out)) Out.Break();
				// 型宣言の型引数の開始の記号の分離
				if(IsClassLike && EdgeIs(Syntax, State.IR, Sub.Head, Role::LessThanToken)) {
					if(!Out.IsEmpty() && !Out.EndsWithBlank()) Out.Space();
					AppendChild(State, Child, std::move(Sub), Out);
					++NamedSeen;
					PrevChild = Child;
					continue;
				}
				const bool IsChildElseLike =
				(Syntax.Is(Child, Role::ElseLikeClause) || Syntax.Is(Child, Role::Clause)) && !Syntax.Is(Id, Role::GuardStatement);
				const SemanticBoundary *const Preserved = ChildIdx ? PreservedBefore(Syntax, Child) : nullptr;
				if(IsDelimitedStatements && OpenHeredocs) AppendHeredocPendingSeparator(Syntax, Out);
				// 誤り区画に接する境界と読み飛ばした字句は原文の間隙を写す
				else if(Preserved) AppendPreservedGap(Syntax, PrevChild, *Preserved, Out);
				else if(AppendCodeBoundary(State, PrevChild, Child, Out)) {} else if(
					Syntax.Is(PrevChild, Role::HardLineDirective) || IsAlternateBody || EndsWithColonBody(Syntax, PrevChild)
				) {
					if(!Out.IsEmpty() && !Out.IsAtLineStart()) Out.Break();
				} else if(Syntax.Is(Child, Role::IndentedBody)) {
					Out.OpenScope();
					if(!Out.IsEmpty() && !Out.IsAtLineStart()) Out.Break();
				} else if(Syntax.Is(PrevChild, Role::IndentedBody)) {
					if(!Out.IsEmpty() && !Out.IsAtLineStart()) Out.Break();
				} else if(IsClassLike && IsAfterInherit) {
					IsAfterInherit = false;
					if(Out.EndsWithBlank()) Out.PopBack();
					Out.Space();
				} else if(
					!Out.IsEmpty() && !Out.IsAtLineStart() && !Out.TailIs(Syntax, Role::ClosingBraceToken, GeneratedToken::BlockClose) &&
					IsChildElseLike
				) Out.Break();
				// ラムダの本体の文の並びは引数の後で改行して始める
				else if(
					IsLambdaBody && Syntax.Is(Child, Role::ValueStatementSequence) || IsDelimitedStatements && ChildIdx && Syntax.IsNamed(PrevChild)
				) {
					if(!Out.IsEmpty() && !Out.IsAtLineStart()) Out.Break();
				} else if(IsStatementList) {
					// Java の enum の本体の宣言部は最初の成員も `;` の後の行へ置く
					if((NamedSeen || Syntax.Is(Id, Role::EnumBodyDeclarations)) && !Out.IsEmpty() && !Out.IsAtLineStart()) Out.Break();
					else if(!Out.IsEmpty() && !Out.IsAtLineStart() && !Out.EndsWithBlank()) SpaceAt(ChildIdx, true);
				} else if(
					// `(` で始まる子を直前の識別子へ密着
					IsStructural && !Out.IsEmpty() && !Out.IsAtLineStart() && !(
						IsRenderedContainer &&
						(EdgeChar(Syntax, State.IR, Out.Last(), false) == '\t' || EdgeChar(Syntax, State.IR, Sub.Head, true) == '\t')
					) && (
						!IsLambdaBody && NamedSeen && !(
							EdgeIs(Syntax, State.IR, Sub.Head, Role::OpeningParenToken, GeneratedToken::CallOpen) && (
								Syntax.Rules().IsFusionAlnum(EdgeChar(Syntax, State.IR, Out.Last(), false)) || Out.TailIs(Syntax, Role::AssignmentToken) ||
								Out.TailIs(Syntax, Role::QuestionToken) || Out.TailIs(Syntax, Role::ExclamationToken)
							) && !(IsMethodDefinition && Syntax.Is(Child, Role::ImplicitResultBody))
						) && !(IsChildElseLike && Out.TailIs(Syntax, Role::ClosingBraceToken, GeneratedToken::BlockClose)) &&
						!(IsStatementsOnlyBreak && !Syntax.Is(Child, Role::ValueStatementSequence)) &&
						!(IsTypedSwitch && !Syntax.Is(Child, Role::SectionHeader)) &&
						!(IsMethodDefinition && !Syntax.Is(Child, Role::AmbiguousBodyContainer)) ||
						!NamedSeen && IsSectionHeaderParent && (Out.TailIs(Syntax, Role::ColonToken) || IsElse)
					)
				) Out.Break();
				// 密集した節点の中の語の単項演算子と被演算子の空白区切り
				else if(
					(!IsCompact || IsUnaryPreOrUpdateNode && !Syntax.IsNamed(PrevChild)) && !Out.IsEmpty() && !Out.IsAtLineStart() &&
					!Out.EndsWithBlank() && ChildIdx
				) {
					if(Syntax.Is(PrevChild, Role::PreprocessorBranch)) Out.Break();
					else if(
						IsUnaryPreOrUpdateNode && !Syntax.IsNamed(PrevChild) && Syntax.Rules().IsFusionLetter(Syntax.FirstChar(PrevChild)) ||
						NeedsSpace(Syntax, Id, PrevChild, Child)
					) SpaceAt(ChildIdx, true);
					else SpaceAt(ChildIdx, false);
				} else if(ChildIdx && !Out.IsAtLineStart() && !Out.EndsWithBlank()) SpaceAt(ChildIdx, false);
				AppendChild(State, Child, std::move(Sub), Out);
				if(Syntax.Is(Child, Role::IndentedBody)) Out.CloseScope();
				if(Closers[ChildIdx] < ChildCount) {
					Out.OpenScope();
					OpenScopes.push_back(Closers[ChildIdx]);
				}
				if(IsDelimitedStatements) TrackOpenHeredocs(State, Child, OpenHeredocs);
				++NamedSeen;
				PrevChild = Child;
				continue;
			}
			// PHP の代替構文：本体を開く `:` は見出へ密着させ，終端の語は行頭に置く（地の文の直後は同じ行）
			if(Syntax.Is(Id, Role::AlternateBodyLayout) || Syntax.Is(Child, Role::ColonBodyEnd)) {
				if(Syntax.IsTokenRole(Child, Role::ColonToken) && IsAlternateBodyOpener(Syntax, Id, Child)) {
					Out.TrimSpaces();
					Out.Append(BuildLeaf(Syntax, Child, State.IR));
					IsAlternateBody = true;
					AlternateBodyOpenEnd = Out.Last();
					Out.OpenScope();
					PrevChild = Child;
					continue;
				}
				if(Syntax.Is(Child, Role::ColonBodyEnd)) {
					if(IsAlternateBody) Out.CloseScope();
					IsAlternateBody = false;
					if(!Out.IsEmpty() && !Out.IsAtLineStart() && !Out.EndsWithBlank()) {
						if(Syntax.Is(PrevChild, Role::CodeTextBoundary) || Out.Last().Item == AlternateBodyOpenEnd.Item) Out.Space();
						else Out.Break();
					}
					Out.Append(BuildLeaf(Syntax, Child, State.IR));
					PrevChild = Child;
					continue;
				}
			}
			// Swift の fallthrough は文の並びの外に無名の字句として現れる事も在る
			if(IsSwitchEntry && Syntax.IsWord(Child, Word::Fallthrough)) {
				if(!Out.IsEmpty() && !Out.IsAtLineStart()) Out.Break();
				Out.Append(BuildLeaf(Syntax, Child, State.IR));
				PrevChild = Child;
				continue;
			}
			// 字面の無い字句の省略
			if(Syntax.GetNode(Child).Lexeme.empty()) {
				if(IsScopeCloser) Out.CloseScope();
				continue;
			}
			// Go の構文誤りを含む子の後の字句は原文の間隙を写す（誤読した塊の閉じの前へ改行を足すと自動の `;` が式を切る）
			if(
				const SemanticBoundary *const Preserved =
				Syntax.Rules().HasAutomaticLineTerminator && ChildIdx ? PreservedBefore(Syntax, Child) : nullptr
			) {
				if(IsScopeCloser) Out.CloseScope();
				AppendPreservedGap(Syntax, PrevChild, *Preserved, Out);
				Out.Append(BuildLeaf(Syntax, Child, State.IR));
				PrevChild = Child;
				continue;
			}
			// 通常の字句と型引数の配置
			Built TokenPiece = BuildLeaf(Syntax, Child, State.IR);
			if(ShouldBreakAfterExpandedBrace(Syntax, TokenPiece.Head, Out)) Out.Break();
			if(IsCompact) {
				if(IsScopeCloser) Out.CloseScope();
				Out.Append(std::move(TokenPiece));
				PrevChild = Child;
				continue;
			}
			if(IsClassLike && Syntax.IsTokenRole(Child, Role::LessThanToken)) {
				IsAfterInherit = true;
				Out.Space();
				Out.Append(std::move(TokenPiece));
				Out.Space();
				PrevChild = Child;
				continue;
			}
			// 構造の開き・閉じ（本体の波括弧，JSON の配列，Go の一括宣言の括弧）
			const bool IsStructuralOpen = IsStructural && (
				Syntax.IsTokenRole(Child, Role::OpeningBraceToken) || IsArrayValue && Syntax.IsTokenRole(Child, Role::OpeningBracketToken) ||
				IsGroupedDeclaration && Syntax.IsTokenRole(Child, Role::OpeningParenToken)
			);
			const bool IsStructuralClose = IsStructural && (
				Syntax.IsTokenRole(Child, Role::ClosingBraceToken) || Syntax.Is(Child, Role::CloserWord) ||
				IsArrayValue && Syntax.IsTokenRole(Child, Role::ClosingSquareToken) ||
				IsGroupedDeclaration && Syntax.IsTokenRole(Child, Role::ClosingParenToken)
			);
			if(IsStructuralClose) {
				// 見出だけを名前付の子に持つ空の本体は，開きの後に置いた改行を戻して `{}` に畳む
				if(Out.IsAtLineStart() && EdgeIs(Syntax, State.IR, Out.Preceding(), Role::OpeningBraceToken, GeneratedToken::BlockOpen)) {
					Out.PopBack();
					if(IsScopeCloser) Out.CloseScope();
				} else {
					const bool IsBroken = !Out.IsEmpty() && !Out.IsAtLineStart() && !EdgeIsBracket(Syntax, State.IR, Out.Last(), true);
					// Go の複数行の並びの閉じの前の末尾のカンマ
					if(IsBroken) if(TrailingCommaForm(Syntax, Id, PrevChild, Child, IsBroken)) {
						Out.Text(Syntax, GeneratedToken::ListSeparator);
						State.IR.EditDetail(Out.Last().Item).PunctuationOwner = Id;
					}
					if(IsScopeCloser) Out.CloseScope();
					if(IsBroken) Out.Break();
				}
				AppendChild(State, Child, std::move(TokenPiece), Out);
				PrevChild = Child;
				continue;
			}
			if(IsScopeCloser) Out.CloseScope();
			// Go の折り返した並びの閉じの前の末尾のカンマ（群の展開時だけ出す）
			if(Candidates[ChildIdx] && EdgeIsBracket(Syntax, State.IR, TokenPiece.Head, false)) {
				if(TrailingCommaForm(Syntax, Id, PrevChild, Child, true)) {
					const FormatItemId Comma = GeneratedItem(Syntax, GeneratedToken::ListSeparator, State.IR);
					State.IR.EditDetail(Comma).PunctuationOwner = Id;
					Out.IfBroken({ Comma });
				}
			}
			if(
				ChildIdx && !Out.IsEmpty() && !Out.IsAtLineStart() && !Out.EndsWithBlank() && !(
					IsStructural && IsExpressionTag && NamedSeen > 1 &&
					(Syntax.IsTokenRole(Child, Role::GreaterThanToken) || Syntax.IsTokenRole(Child, Role::SelfClosingTagToken))
				)
			) {
				// 前処理の条件の後は必ず改行し，do-while と Java 系の else は複数行にし，Kotlin の値の位置の if は一行を保つ
				if(
					Syntax.Is(PrevChild, Role::PreprocessorBranch) || !Out.TailIs(Syntax, Role::ClosingBraceToken, GeneratedToken::BlockClose) && (
						Syntax.IsWord(Child, Word::While) && IsDoStatementNode || Syntax.Is(Child, Role::ElseKeyword) && IsIfParentNode &&
						(!Syntax.Is(Id, Role::BracedConditionalValue) || ShouldExpand && !IsArrowValueBody(State, Id))
					)
				) Out.Break();
				else SpaceAt(ChildIdx, NeedsSpace(Syntax, Id, PrevChild, Child));
			}
			if(
				IsStructural && IsExpressionTag && NamedSeen > 1 &&
				(Syntax.IsTokenRole(Child, Role::GreaterThanToken) || Syntax.IsTokenRole(Child, Role::SelfClosingTagToken))
			) {
				// 属性を展開した開きタグ又は自己閉じタグの閉じの字句の独立した行への配置
				Out.TrimSpaces();
				if(!Out.IsEmpty() && !Out.IsAtLineStart()) Out.Break();
				Out.Append(std::move(TokenPiece));
				PrevChild = Child;
				continue;
			}
			Out.Append(std::move(TokenPiece));
			// 括弧の対の開きの後の深さの範囲（構造の開きは改行も範囲の中へ置く）
			if(Closers[ChildIdx] < ChildCount) {
				Out.OpenScope();
				OpenScopes.push_back(Closers[ChildIdx]);
			}
			if(IsStructuralOpen && ChildIdx + 1 < ChildCount && !IsLambdaBody) Out.Break();
			PrevChild = Child;
		}
		// 尾に保つ原文の間隙
		if(const SemanticBoundary *const Tail = PreservedAt(Syntax, Id, ChildCount)) Out.Gap(*Tail);
		// 子を順に繋いだ要素の返戻
		co_return Out.Finish(Id, State.NextGroup++);
	}
} // namespace Core
