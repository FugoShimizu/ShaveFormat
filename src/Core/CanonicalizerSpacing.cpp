#include "Canonicalizer.hpp"
#include "FormatIR.hpp"
#include "NodeId.hpp"
#include "SyntaxModel.hpp"
#include "SyntaxRole.hpp"
#include "Util/FormatLimitExceeded.hpp"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 子の番号の前の保持する境界の取得関数（整形仕様 4.4, 4.6）
	 * 計算量：記録された境界の数 B に対し O(log(B + 1))
	 * @param Syntax 構文モデル
	 * @param Parent 親
	 * @param Idx 境界の後の子の番号（０は最初の子の前，子の数は最後の子の後）
	 * @return 投影が原文の間隙を保つと記録した境界（無ければ nullptr）
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	const SemanticBoundary *Canonicalizer::GetPreservedAt(const SyntaxModel &Syntax, const NodeId Parent, const uint32_t Idx) {
		// 子の番号の前の保持する境界の取得
		const SemanticBoundary *const Boundary = Syntax.GetNode(Parent).Facts.GetBoundaryBefore(Idx);
		// 保持する境界の返戻
		return Boundary && Boundary->Kind == BoundaryKind::Preserved ? Boundary : nullptr;
	}

	/**
	 * トークンの綴りの定義の取得関数
	 * 計算量：初回はトークン長 B に対し O(B)，以後 O(1)
	 * @param State トークンの定義を共有する構築状態
	 * @param Id トークン端を持つノード
	 * @return 綴り全体の定義（テキストが無いか未知の綴りでは nullptr）
	 * @throws FormatLimitExceeded トークンの照会が作業予算を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	const KindDef *Canonicalizer::GetSpacingTokenOf(Builder &State, const NodeId Id) {
		// トークンの綴りの定義の取得
		State.Syntax.Charge(1);
		NodeCtx &Ctx = State.Contexts[Id];
		if(!Ctx.HasSpacingTokenWork) {
			Ctx.HasSpacingTokenWork = true;
			if(const SyntaxNode &Node = State.Syntax.GetNode(Id); !Node.Lexeme.empty()) {
				State.Syntax.Charge(Node.Lexeme.size());
				if(Node.Facts.DelimiterDef) Ctx.SpacingToken = Node.Facts.DelimiterDef;
				else if(!Ctx.IsSpacingTokenKnown) {
					const std::unordered_map<std::string_view, FormPart> &Tokens = State.Syntax.GetRules().TokenDefs;
					if(
						const std::unordered_map<std::string_view, FormPart>::const_iterator Found = Tokens.find(Node.Lexeme);
						Found != Tokens.end()
					) Ctx.SpacingToken = Found->second.Def;
				}
				// 折返で先に定義を引いた場合も，最初の空白照会の本文の作業量は同じ時点で計上する
				if(Ctx.SpacingToken) State.Syntax.Charge(Node.Lexeme.size());
			}
			Ctx.IsSpacingTokenKnown = true;
		}
		// １度照会したトークンの定義の返戻
		return Ctx.SpacingToken;
	}

	/**
	 * 役割毎の空白規則の固定インデックスの取得関数
	 * 同じ順位の相反する実装は switch の重複した case として翻訳時に検出する
	 * 計算量：固定の登録数 R に対し初回 O(R)，以後 O(1)
	 * @return 境界の照会元と役割で引く変更不能なインデックス
	 */
	const Canonicalizer::SpacingRoleIdx &Canonicalizer::GetSpacingRoles() {
		// 役割毎の空白規則の固定インデックスの取得
		static_assert(static_cast<size_t>(SpacingRule::Count) < 65);
		// 空白規則の固定の役割インデックスの構築
		static const SpacingRoleIdx Idx = []() -> SpacingRoleIdx {
			SpacingRoleIdx Result {};
			// 照会元と役割に対応する規則の登録
			const auto Register =
			[&Result](const SpacingRule Rule, const SpacingSite Site, const std::initializer_list<Role> Roles) -> void {
				for(const Role Wanted : Roles) {
					Result[static_cast<size_t>(Wanted)].Masks[static_cast<size_t>(Site)] |= uint64_t{ 1 } << static_cast<size_t>(Rule);
				}
				// 終了
				return;
			};
			Register(SpacingRule::AttachedPostfix, SpacingSite::Parent, { Role::AdjacencySensitiveInfix });
			Register(SpacingRule::Deconstruction, SpacingSite::Cur, { Role::DeconstructionType });
			Register(SpacingRule::AwaitOperand, SpacingSite::Cur, { Role::AwaitCallHead });
			Register(SpacingRule::CodeBoundary, SpacingSite::Cur, { Role::CodeBoundary });
			Register(SpacingRule::CodeBoundary, SpacingSite::Following, { Role::CodeBoundary });
			Register(SpacingRule::AlternateColon, SpacingSite::Following, { Role::AlternateBodyLayout });
			Register(
				SpacingRule::IndivisibleType,
				SpacingSite::Parent,
				{
					Role::AnnotationUnionOperator,
					Role::AttachedHashSplat,
					Role::AttachedNamespaceImport,
					Role::MarkupSyntax,
					Role::TypeAlias,
					Role::TypeCombination
				}
			);
			Register(
				SpacingRule::IndivisibleType,
				SpacingSite::Cur,
				{ Role::AttachedLowUnaryWord, Role::AttachedRefModifier, Role::Error }
			);
			Register(SpacingRule::IndivisibleType, SpacingSite::Following, { Role::Error });
			Register(SpacingRule::SelectorOperands, SpacingSite::Parent, { Role::CombinatorSelector });
			Register(SpacingRule::RenderedContent, SpacingSite::Parent, { Role::RenderedChildren });
			Register(SpacingRule::WordArrayElements, SpacingSite::Parent, { Role::WordArray });
			Register(SpacingRule::EmptyStmt, SpacingSite::FollowingEdge, { Role::StmtTerminatorLead });
			Register(SpacingRule::CaptureElements, SpacingSite::Parent, { Role::CaptureList });
			Register(SpacingRule::ControlBody, SpacingSite::Parent, { Role::LabeledPrefix });
			Register(SpacingRule::ControlBody, SpacingSite::Cur, { Role::Label });
			Register(SpacingRule::ControlBody, SpacingSite::Following, { Role::SpacedControlBody });
			Register(SpacingRule::GeneratorName, SpacingSite::Parent, { Role::GeneratorStarHost });
			Register(
				SpacingRule::IndivisibleSyntax,
				SpacingSite::Parent,
				{
					Role::AttachedKeywordBrackets,
					Role::AttachedMagnitudeSuffix,
					Role::AttachedNegationTest,
					Role::AttachedNumericUnit,
					Role::AttachedRangeOrUnary,
					Role::AttachedSelector,
					Role::BaseRef,
					Role::FunctionType,
					Role::Indexer,
					Role::InferredArrayConstruction,
					Role::JumpExpr,
					Role::MethodDef,
					Role::NewParenCall,
					Role::PackExpansion,
					Role::RangeAssignment,
					Role::SelfRef,
					Role::SizeofExpr,
					Role::SpreadParam,
					Role::VariadicTypeParam
				}
			);
			Register(
				SpacingRule::IndivisibleSyntax,
				SpacingSite::Cur,
				{ Role::AttachedLabel, Role::AttachedPercent, Role::ExclamationMark, Role::PostfixAssertionMark }
			);
			Register(SpacingRule::IndivisibleSyntax, SpacingSite::Following, { Role::ExclamationMark, Role::PostfixAssertionMark });
			Register(SpacingRule::AttributesAndTypes, SpacingSite::Parent, { Role::SpacedAttributeTag });
			Register(SpacingRule::AttributesAndTypes, SpacingSite::FollowingEdge, { Role::OpeningParenLead });
			Register(SpacingRule::CastBoundary, SpacingSite::Parent, { Role::CastLikeExpr });
			Register(
				SpacingRule::LiteralInterior,
				SpacingSite::Parent,
				{
					Role::AttributeDecl,
					Role::AttributeSelector,
					Role::BlockArg,
					Role::ClassSelector,
					Role::IdSelector,
					Role::InterpolatedLiteral,
					Role::Interpolation,
					Role::InterpolationAlignment,
					Role::TemplateInterpolation,
					Role::TypedLambda,
					Role::WordArray
				}
			);
			Register(SpacingRule::NamedOperator, SpacingSite::Parent, { Role::NamedInfix });
			Register(SpacingRule::CompositeBrace, SpacingSite::Parent, { Role::BraceAttach });
			Register(
				SpacingRule::NamedAttachment,
				SpacingSite::Parent,
				{ Role::CallExpr, Role::LeadingTypeArgs, Role::MacroLike, Role::PostfixExpr, Role::PrefixUnary }
			);
			Register(SpacingRule::NamedAttachment, SpacingSite::Cur, { Role::OptionalChain });
			Register(
				SpacingRule::NamedAttachment,
				SpacingSite::Following,
				{
					Role::AttachBracket,
					Role::AttachColonOrDot,
					Role::CallSuffix,
					Role::ExclamationMark,
					Role::NormColon,
					Role::OptionalMarker,
					Role::SpaceSkip
				}
			);
			Register(SpacingRule::NamedAttachment, SpacingSite::FollowingEdge, { Role::CommaLead });
			Register(SpacingRule::AnnotationArgs, SpacingSite::Parent, { Role::AnnotationConstructor });
			Register(SpacingRule::LambdaParams, SpacingSite::Parent, { Role::TypedLambda });
			Register(SpacingRule::NamedCall, SpacingSite::Following, { Role::AttachParen });
			Register(SpacingRule::GenericArgs, SpacingSite::Following, { Role::AngleBracketList });
			Register(SpacingRule::NamedBracket, SpacingSite::FollowingEdge, { Role::OpeningParenLead, Role::OpeningSquareLead });
			Register(SpacingRule::ExceptionStar, SpacingSite::Parent, { Role::ExceptionClause });
			Register(SpacingRule::PostfixPtr, SpacingSite::Parent, { Role::PostfixPtrType });
			Register(SpacingRule::PtrDeclarator, SpacingSite::Parent, { Role::PtrDecl });
			Register(SpacingRule::PrefixOperator, SpacingSite::Parent, { Role::UnaryPreOrUpdate });
			Register(SpacingRule::PostfixOperator, SpacingSite::Parent, { Role::PostfixOrUpdateExpr });
			Register(SpacingRule::ConditionalColon, SpacingSite::Parent, { Role::ColonPairHost });
			Register(SpacingRule::RangeFusion, SpacingSite::Parent, { Role::RangeLike });
			Register(SpacingRule::OptionalAndRange, SpacingSite::Parent, { Role::OptionalMarker, Role::RangeLike });
			Register(SpacingRule::OptionalAndRange, SpacingSite::CurToken, { Role::OptionalAccessToken, Role::QuestionToken });
			Register(
				SpacingRule::OptionalAndRange,
				SpacingSite::FollowingToken,
				{ Role::CoalescingToken, Role::OptionalAccessToken, Role::QuestionToken }
			);
			Register(SpacingRule::BinaryOperands, SpacingSite::Parent, { Role::FoldExpr, Role::HardLineDirective, Role::InfixOperator });
			Register(SpacingRule::TagInterior, SpacingSite::Parent, { Role::TagSyntax });
			Register(SpacingRule::TightAssignment, SpacingSite::Parent, { Role::EqualsTight, Role::MacroLike });
			Register(SpacingRule::DestructorName, SpacingSite::Parent, { Role::DestructorDecl });
			Register(SpacingRule::SpacedAssignment, SpacingSite::CurToken, { Role::AssignmentToken });
			Register(SpacingRule::SpacedAssignment, SpacingSite::FollowingToken, { Role::AssignmentToken });
			Register(
				SpacingRule::AttachedSeparator,
				SpacingSite::Parent,
				{
					Role::AngleBracketList,
					Role::BracketedType,
					Role::ChannelType,
					Role::Compact,
					Role::KeyPath,
					Role::Param,
					Role::RefAssignment
				}
			);
			Register(
				SpacingRule::AttachedSeparator,
				SpacingSite::CurToken,
				{
					Role::AmpersandToken,
					Role::AssignmentToken,
					Role::AtToken,
					Role::AttributeOpenToken,
					Role::BackslashToken,
					Role::ClosingSquareToken,
					Role::DoubleColonToken,
					Role::HashToken,
					Role::LessThanToken,
					Role::MarkupDeclOpenToken,
					Role::OpeningBracketToken,
					Role::OpeningParenToken
				}
			);
			Register(
				SpacingRule::AttachedSeparator,
				SpacingSite::FollowingToken,
				{
					Role::AssignmentToken,
					Role::ClosingParenToken,
					Role::ClosingSquareToken,
					Role::CommaToken,
					Role::DoubleColonToken,
					Role::GreaterThanToken,
					Role::SpreadToken,
					Role::StmtTerminatorToken
				}
			);
			Register(SpacingRule::AfterSeparator, SpacingSite::CurToken, { Role::ColonToken, Role::CommaToken, Role::StmtTerminatorToken });
			Register(SpacingRule::BraceInterior, SpacingSite::CurToken, { Role::OpeningBraceToken });
			Register(SpacingRule::BraceInterior, SpacingSite::FollowingToken, { Role::ClosingBraceToken });
			Register(SpacingRule::PipeDelimiter, SpacingSite::Parent, { Role::AttachedSelectorNamespace, Role::PipeParamList });
			Register(SpacingRule::BeforeColon, SpacingSite::FollowingToken, { Role::ColonToken });
			Register(SpacingRule::KeywordAndInt, SpacingSite::Cur, { Role::NamedKeyword });
			Register(SpacingRule::KeywordAndInt, SpacingSite::FollowingToken, { Role::DotToken });
			Register(SpacingRule::MemberAccess, SpacingSite::Parent, { Role::AttachedArrowLambda, Role::Member });
			Register(SpacingRule::MemberAccess, SpacingSite::Cur, { Role::OptionalChain });
			Register(SpacingRule::MemberAccess, SpacingSite::Following, { Role::OptionalChain });
			Register(
				SpacingRule::MemberAccess,
				SpacingSite::CurToken,
				{ Role::DotToken, Role::OptionalAccessToken, Role::SafeAccessToken }
			);
			Register(
				SpacingRule::MemberAccess,
				SpacingSite::FollowingToken,
				{ Role::DotToken, Role::OptionalAccessToken, Role::SafeAccessToken }
			);
			Register(
				SpacingRule::ArrowOperator,
				SpacingSite::CurToken,
				{ Role::FatArrowToken, Role::OptionalArrowToken, Role::RightArrowToken }
			);
			Register(
				SpacingRule::ArrowOperator,
				SpacingSite::FollowingToken,
				{ Role::FatArrowToken, Role::OptionalArrowToken, Role::RightArrowToken }
			);
			Register(SpacingRule::TypeBody, SpacingSite::Parent, { Role::BraceNoSpace });
			Register(SpacingRule::BodyBrace, SpacingSite::FollowingToken, { Role::OpeningBraceToken });
			Register(SpacingRule::ValCall, SpacingSite::FollowingToken, { Role::OpeningBracketToken, Role::OpeningParenToken });
			Register(SpacingRule::TypeQuery, SpacingSite::Cur, { Role::AttachedTypeQuery });
			Register(SpacingRule::DeclParen, SpacingSite::Parent, { Role::GroupedDecl, Role::GroupedImport });
			Register(SpacingRule::KeywordParen, SpacingSite::FollowingEdge, { Role::OpeningParenLead, Role::OpeningSquareLead });
			Register(
				SpacingRule::UnnamedAttachment,
				SpacingSite::Following,
				{ Role::AngleBracketList, Role::AttachColonOrDot, Role::NormColon, Role::OptionalMarker, Role::SpaceSkip }
			);
			Register(SpacingRule::OrdinaryElements, SpacingSite::CurToken, { Role::AssignmentToken, Role::WildcardToken });
			Register(SpacingRule::OrdinaryElements, SpacingSite::FollowingToken, { Role::AssignmentToken, Role::WildcardToken });
			Register(SpacingRule::OrdinaryElements, SpacingSite::Cur, { Role::JointPunctuationToken });
			Register(SpacingRule::OrdinaryElements, SpacingSite::Following, { Role::JointPunctuationToken });
			// 固定の役割インデックスの返戻
			return Result;
		}();
		// 共有する固定インデックスの返戻
		return Idx;
	}

	/**
	 * 子の前の境界が原文で密着して居たかの判定関数（整形仕様 4.2，投影が記録した構文事実を読む）
	 * 計算量：記録された境界の数 B に対し O(log(B + 1))
	 * @param Syntax 構文モデル
	 * @param Parent 親
	 * @param Idx 境界の後に続く子の番号
	 * @return 密着と記録した境界なら true
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	bool Canonicalizer::IsGluedBefore(const SyntaxModel &Syntax, const NodeId Parent, const uint32_t Idx) {
		// 子の前の境界が原文で密着して居たかの判定
		const SemanticBoundary *const Boundary = Syntax.GetNode(Parent).Facts.GetBoundaryBefore(Idx);
		// 密着の記録の返戻
		return Boundary && (Boundary->Kind == BoundaryKind::Glued || Boundary->Kind == BoundaryKind::TokenGlued);
	}

	/**
	 * 子の前の保持する境界の取得関数（整形仕様 4.6）
	 * 計算量：親に記録された境界の数 B に対し O(log(B + 1))
	 * @param Syntax 構文モデル
	 * @param Child 子
	 * @return 投影が原文の間隙を保つと記録した境界（無ければ nullptr）
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	const SemanticBoundary *Canonicalizer::GetPreservedBefore(const SyntaxModel &Syntax, const NodeId Child) {
		// 親の中の子の前の保持する境界の返戻
		return GetPreservedAt(Syntax, Syntax.GetParentOf(Child), Syntax.GetNode(Child).Idx);
	}

	/**
	 * 端のトークンが密着して融合するかの判定関数
	 * 計算量：比較する端のトークンの総テキスト長 B に対し O(B)
	 * @param Syntax 構文モデル
	 * @param IR 中間表現
	 * @param Before 前のトークンの末尾の要素
	 * @param After 後のトークンの先頭の要素
	 * @return 融合する組なら true
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	bool Canonicalizer::CanMergeEdgeTokens(
		const SyntaxModel &Syntax,
		const FormatIR &IR,
		const EdgeItem Before,
		const EdgeItem After
	) {
		// ノードのトークン又は格納済の生成トークンから，融合判定に必要な端の綴りを得る
		const auto SpellingOf = [&Syntax, &IR](const EdgeItem Edge) -> std::string_view {
			// 端の要素の無い事の返戻
			if(Edge.Item == NoItem) return {};
			const FormatItem &Item = IR.Items[Edge.Item];
			// 此の照合中は IR を変更せず，生成時の固定トークンの綴りを参照する事の返戻
			if(Item.Generated != GeneratedToken::None) return IR.GetTextOf(Item);
			// 元のトークンの綴りの返戻
			return Edge.Node != NoNode && Edge.IsWholeToken && Item.Kind == FormatItemKind::Token ?
			std::string_view(Syntax.GetNode(Edge.Node).Lexeme) :
			std::string_view();
		};
		const std::string_view Left = SpellingOf(Before), Right = SpellingOf(After);
		// トークンが揃わない境界の返戻
		if(Left.empty() || Right.empty()) return false;
		// 原文のトークン又は生成トークンの端が融合するかの返戻
		return Syntax.GetRules().CanFuseDimensionTokens(Left.back(), Right);
	}

	/**
	 * 部分木の端のトークンの定義の取得関数
	 * 計算量：トークンの初回照会の長さ B に対し O(B)，以後 O(1)
	 * @param State トークンと端の字の照会を共有する構築状態
	 * @param Edge 部分木の端のトークン
	 * @param KnownToken 端のトークン全体の照会済の定義（未照会又は未知なら nullptr）
	 * @return トークン全体を優先した端の定義（端が無いか未知の字では nullptr）
	 * @throws FormatLimitExceeded トークン端の照会が作業予算を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	const KindDef *Canonicalizer::GetSpacingEdgeOf(Builder &State, const LexicalEdge Edge, const KindDef *const KnownToken) {
		// 部分木の端のトークンの定義の取得
		State.Syntax.Charge(1);
		// 端が無い場合の定義無の返戻
		if(Edge.Leaf == NoNode) return nullptr;
		// 同じ境界で照会済のトークン全体の役割の返戻
		if(KnownToken) return KnownToken;
		// 端の葉を今回初めて照会する場合のトークン全体の役割の返戻
		if(const KindDef *const Token = GetSpacingTokenOf(State, Edge.Leaf)) return Token;
		const size_t CharIdx = Edge.Char;
		if(!State.IsSpacingCharKnown[CharIdx]) {
			State.Syntax.Charge(1);
			State.IsSpacingCharKnown[CharIdx] = true;
			const std::unordered_map<std::string_view, FormPart> &Tokens = State.Syntax.GetRules().TokenDefs;
			const char Char = static_cast<char>(Edge.Char);
			if(
				const std::unordered_map<std::string_view, FormPart>::const_iterator Found = Tokens.find(std::string_view(&Char, 1));
				Found != Tokens.end()
			) State.SpacingChars[CharIdx] = Found->second.Def;
		}
		// 同じ文法で１度照会した端の字の定義の返戻
		return State.SpacingChars[CharIdx];
	}

	/**
	 * 所有範囲の前後のトークンの字の取得関数
	 * 兄弟のトークンは構築走査で対応付け，祖先から継ぐ字だけを必要時に１度確定する
	 * 計算量：初めて確定する祖先数 D に対し O(D)，確定済の範囲は O(1)
	 * @param State 所有範囲の境界を共有する構築状態
	 * @param Id ノード
	 * @param IsBefore 範囲の前の字を得るか
	 * @return 範囲の前又は後の字（無ければ零）
	 * @throws FormatLimitExceeded 境界の確定が作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	unsigned char Canonicalizer::GetAdjacentChar(Builder &State, const NodeId Id, const bool IsBefore) {
		// 所有範囲の前後のトークンの字の取得
		std::vector<NodeId> Pending;
		unsigned char Char = '\0';
		for(NodeId Owner = Id; Owner != NoNode; Owner = State.Syntax.GetParentOf(Owner)) {
			State.Syntax.Charge(1);
			if(const NodeCtx &Ctx = State.Contexts[Owner]; IsBefore ? Ctx.HasPreceding : Ctx.HasFollowing) {
				Char = IsBefore ? Ctx.Preceding : Ctx.Following;
				break;
			}
			Pending.push_back(Owner);
		}
		for(const NodeId Owner : Pending) {
			State.Syntax.Charge(1);
			NodeCtx &Ctx = State.Contexts[Owner];
			if(IsBefore) {
				Ctx.Preceding = Char;
				Ctx.HasPreceding = true;
			} else {
				Ctx.Following = Char;
				Ctx.HasFollowing = true;
			}
		}
		// 所有範囲の前後の字の返戻
		return Char;
	}

	/**
	 * １つの定義が選ぶ空白規則の取得関数
	 * 計算量：初回は定義の役割数 R に対し O(R)，以後は期待値 O(1)
	 * @param State 定義毎の候補を共有する構築状態
	 * @param Kind 共通の種別（定義が無い場合は nullptr）
	 * @return 照会元毎の規則の順位の集合
	 * @throws FormatLimitExceeded 役割の照会が作業予算を超える場合
	 * @throws std::bad_alloc 定義の控えのメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	const Canonicalizer::SpacingCandidates &Canonicalizer::GetSpacingRulesOf(Builder &State, const KindDef *const Kind) {
		// 定義が無い場合の空の候補
		static const SpacingCandidates Empty;
		// 空の候補の返戻
		if(!Kind) return Empty;
		State.Syntax.Charge(1);
		const std::pair<std::pmr::unordered_map<const KindDef *, SpacingCandidates>::iterator, bool> Entry =
		State.SpacingKinds.try_emplace(Kind);
		if(Entry.second) {
			if(State.SpacingPairs.empty()) State.SpacingPairs.emplace_back();
			Entry.first->second.PairIndex = State.SpacingPairs.size();
			State.SpacingPairs.emplace_back();
			const SpacingRoleIdx &Idx = GetSpacingRoles();
			for(const Role Wanted : Kind->RoleList) {
				State.Syntax.Charge(1);
				for(size_t SiteIdx = 0; SiteIdx < static_cast<size_t>(SpacingSite::Count); ++SiteIdx) {
					Entry.first->second.Masks[SiteIdx] |= Idx[static_cast<size_t>(Wanted)].Masks[SiteIdx];
				}
			}
		}
		// 定義の該当する候補の返戻
		return Entry.first->second;
	}

	/**
	 * ノードに対応付けた変更不能な空白候補を取得する関数
	 * 対応付の後も候補の参照１回を計上し，定義表への再照会を省く
	 * 計算量：初回は定義の役割数 R に対し O(R)，以後 O(1)
	 * @param State 定義毎の候補を共有する構築状態
	 * @param Kind 共通の種別（定義が無い場合は `nullptr`）
	 * @param Cached 同じ種別の候補の参照の格納先
	 * @return 照会元毎の規則の順位の集合
	 * @throws FormatLimitExceeded 候補の参照又は役割の照会が作業予算を超える場合
	 * @throws std::bad_alloc 定義の控えのメモリを確保出来ない場合
	 * @throws std::length_error 定義表の要素数が上限を超える場合
	 */
	const Canonicalizer::SpacingCandidates &Canonicalizer::GetNodeSpacingRules(
		Builder &State,
		const KindDef *const Kind,
		const SpacingCandidates *&Cached
	) {
		if(Cached) {
			if(Kind) State.Syntax.Charge(1);
		} else Cached = &GetSpacingRulesOf(State, Kind);
		// ノードに対応付けた候補の返戻
		return *Cached;
	}

	/**
	 * 記号の組の固定の候補と密着条件の取得関数
	 * 種別毎の表の番号で直接引き，実際に使用する組だけを１度合成する
	 * 計算量：初回の行の伸長時は使用済の定義数 K に対し O(K)，合成と再照会は O(1)
	 * @param State 定義と記号の組を共有する構築状態
	 * @param Cur 前のトークン全体の定義
	 * @param Following 後のトークン全体の定義
	 * @param CurRules 前のトークンの候補
	 * @param FollowingRules 後のトークンの候補
	 * @return 同じ記号の組で共有する固定の候補と密着条件
	 * @throws std::bad_alloc 記号の組の表を確保出来ない場合
	 * @throws std::length_error 記号の組の表が最大長を超える場合
	 */
	const Canonicalizer::SpacingPair &Canonicalizer::GetSpacingPairOf(
		Builder &State,
		const KindDef *const Cur,
		const KindDef *const Following,
		const SpacingCandidates &CurRules,
		const SpacingCandidates &FollowingRules
	) {
		// 定義無の組だけを使う場合も零番の行を用意する
		if(State.SpacingPairs.empty()) State.SpacingPairs.emplace_back();
		SpacingPairRow &Row = State.SpacingPairs[CurRules.PairIndex];
		if(Row.size() <= FollowingRules.PairIndex) Row.resize(State.SpacingPairs.size());
		SpacingPair &Pair = Row[FollowingRules.PairIndex];
		if(!Pair.IsKnown) {
			Pair.Candidates = CurRules.Masks[static_cast<size_t>(SpacingSite::CurToken)] |
			FollowingRules.Masks[static_cast<size_t>(SpacingSite::FollowingToken)];
			Pair.IsCurAttached =
			IsRole(Cur, Role::DoubleColonToken) || IsRole(Cur, Role::OpeningBracketToken) || IsRole(Cur, Role::OpeningParenToken) ||
			IsRole(Cur, Role::MarkupDeclOpenToken) || IsRole(Cur, Role::AttributeOpenToken) || IsRole(Cur, Role::HashToken);
			Pair.IsFollowingAttached = IsRole(Following, Role::DoubleColonToken) || IsRole(Following, Role::ClosingParenToken) ||
			IsRole(Following, Role::ClosingSquareToken) || IsRole(Following, Role::CommaToken) ||
			IsRole(Following, Role::StmtTerminatorToken);
			Pair.IsKnown = true;
		}
		// 記号の組で共有する結果の返戻
		return Pair;
	}

	/**
	 * 開いた範囲演算子で終わるかの判定関数
	 * 計算量：O(1)
	 * @param State 末端の経路を共有する構築状態
	 * @param Id ノード
	 * @return 最後の子の経路の末端が範囲演算子なら true
	 */
	bool Canonicalizer::HasTrailingOpenRange(const Builder &State, const NodeId Id) {
		// 開いた範囲の有無の返戻
		return Id != NoNode && State.Syntax.IsTokenRole(State.Contexts[Id].LastPathLeaf, Role::RangeOperator);
	}

	/**
	 * 括弧で始まる型かの判定関数
	 * 計算量：O(1)
	 * @param State 先頭の経路を共有する構築状態
	 * @param Id ノード
	 * @return 名前付の先頭経路に括弧で始まる型を持つなら true
	 */
	bool Canonicalizer::HasLeadingParenType(const Builder &State, const NodeId Id) {
		// 括弧で始まる型の返戻
		return State.Contexts[Id].IsLeadingParenType;
	}

	/**
	 * Ruby の符号を密着させても読みが変わらない被演算子かの判定関数
	 * 数字で始まる被演算子へ符号を密着させると符号付の数値に為り，後置の受手の範囲が変わる
	 * 計算量：端の探索で訪問するノード数 N，子数 C と記録された境界数 B に対し O(N + C + log(B + 1))
	 * @param Syntax 構文モデル
	 * @param Operand 被演算子
	 * @return 密着させて良いなら true
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	bool Canonicalizer::CanAttachNumericSign(const SyntaxModel &Syntax, const NodeId Operand) {
		// 数字で始まらない被演算子の返戻
		if(Operand == NoNode || !std::isdigit(Syntax.GetFirstChar(Operand))) return true;
		// 後置の受手と読まれた離れた符号の返戻
		if(
			const NodeId Unary = Syntax.GetParentOf(Operand), Outer = Syntax.GetParentOf(Unary);
			Syntax.Is(Unary, Role::UnaryVal) && !IsGluedBefore(Syntax, Unary, Syntax.GetNode(Operand).Idx) &&
			Syntax.Is(Outer, Role::PostfixReceiverChain) && Syntax.GetNamedChild(Outer, 0) == Unary
		) return false;
		// 符号直下の数値の底
		const NodeId Base = Syntax.Is(Operand, Role::Binary) &&
		Syntax.IsTokenRole(Syntax.GetFieldChild(Operand, ChildRole::Operator), Role::ExponentToken) ?
		Syntax.GetNamedChild(Operand, 0) :
		Operand;
		// 数値だけか，数値を底とする冪かの返戻
		return Syntax.Is(Base, Role::NumLiteral) || Syntax.Is(Base, Role::NumericUnitSuffix);
	}

	/**
	 * 保持する境界の追加関数（整形仕様 4.6）
	 * 行の途中の境界では，構造化で落ちる前の子の末尾の `;` を戻して原文の間隙をコピーする
	 * 計算量：保存する間隙のテキスト長 B に対し O(B)
	 * @param Syntax 構文モデル
	 * @param Prev 境界の前の子
	 * @param Boundary 保持する境界
	 * @param Output 組立中の出力
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::AppendPreservedGap(
		const SyntaxModel &Syntax,
		const NodeId Prev,
		const SemanticBoundary &Boundary,
		Emitter &Output
	) {
		// マクロ・リテラルの間隙と JSX の原文の行端は，文の区切を補わず，現在の行末状態に依らず保持する
		if(
			Boundary.Src == PreservedSrc::LiteralContent || Boundary.Src == PreservedSrc::MacroTokens ||
			Boundary.Src == PreservedSrc::RenderedLineEdges
		) {
			Output.Gap(Boundary);
			// 指定した間隙をコピーした場合の返戻
			return;
		}
		// 行頭と空白の後は間隙を足さない事の終了
		if(Output.IsEmpty() || Output.IsAtLineStart() || Output.HasTrailingSpace()) return;
		if(
			Prev != NoNode && Syntax.HasLastTokenRole(Prev, Role::StmtTerminatorTail) &&
			!Output.IsTailRole(Syntax, Role::StmtTerminatorToken, GeneratedToken::StmtTerminator)
		) Output.GetText(Syntax, GeneratedToken::StmtTerminator);
		Output.Gap(Boundary);
		// 終了
		return;
	}

	/**
	 * 文の並びのエラー領域に接する保持する境界の取得関数（整形仕様 4.6）
	 * 文を行毎に置く並びでは，エラー領域に接する境界だけ原文の間隙を保ち，プリプロセッサディレクティブの境界は条件のブロックを保つ為に改行する
	 * 計算量：間の名前無の兄弟数 C に対し O(C) と保存する間隙の取得費用
	 * @param Syntax 構文モデル
	 * @param Prev 前の文
	 * @param Child 後の文
	 * @return 保持する境界（無ければ nullptr）
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	const SemanticBoundary *Canonicalizer::GetErrorGapBefore(const SyntaxModel &Syntax, const NodeId Prev, const NodeId Child) {
		// 間の無名のトークン（文の区切の `;` 等）を越えた前の兄弟
		NodeId Adjacent = Syntax.GetPrevSibling(Child);
		while(Adjacent != Prev && Adjacent != NoNode && !Syntax.IsNamed(Adjacent)) {
			Syntax.Charge(1);
			Adjacent = Syntax.GetPrevSibling(Adjacent);
		}
		// 隣接するエラー領域が無い境界，又はプリプロセッサディレクティブの境界を保持対象外とする事の返戻
		if(
			Prev == NoNode || Adjacent != Prev || !Syntax.Is(Prev, Role::Error) && !Syntax.Is(Child, Role::Error) ||
			Syntax.GetRules().HasLineDirectives && (
				Syntax.Is(Prev, Role::Preproc) || Syntax.Is(Child, Role::Preproc) ||
				Syntax.GetNode(Child).Facts.Has(ConstraintKind::DirectiveZone) || Syntax.GetNode(Prev).Facts.Has(ConstraintKind::DirectiveZone)
			)
		) return nullptr;
		// 投影が記録した保持する境界の返戻
		return GetPreservedBefore(Syntax, Child);
	}

	/**
	 * 固定の区切記法の追加関数（整形仕様 6.1）
	 * CSS の区切記法で分けた境界で前後のトークンが融合する時は，本文の無いブロックコメントの区切記法で分ける
	 * 計算量：固定の区切記法のテキスト長 B に対し O(B)
	 * @param Syntax 構文モデル
	 * @param Child 後の子
	 * @param Head 後の子の組立済の先頭
	 * @param Output 組立中の出力
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::AppendLexicalSeparator(
		const SyntaxModel &Syntax,
		const NodeId Child,
		const EdgeItem Head,
		Emitter &Output
	) {
		// 空白が意味を持たない文法と，先頭・親の無い子の場合の終了
		if(!Syntax.GetRules().IsWhitespaceSemantic || Output.IsEmpty() || Syntax.GetParentOf(Child) == NoNode) return;
		if(
			const SemanticBoundary *const Boundary =
			Syntax.GetNode(Syntax.GetParentOf(Child)).Facts.GetBoundaryBefore(Syntax.GetNode(Child).Idx);
			Boundary && Boundary->Kind == BoundaryKind::Separated && CanMergeEdgeTokens(Syntax, Output.GetStore(), Output.GetLast(), Head)
		) Output.Separator(Syntax.GetRules().LexicalSeparator);
		// 終了
		return;
	}

	/**
	 * 兄弟のトークンの間の空白の要否の判定関数（トークン間空白仕様第３章，第４章）
	 * 前後の子の構文上の役割から密着か空白１個かを決め，原文の空白量は読まない（読みを決める隣接は投影の構文事実から引く）
	 * 計算量：初回照会する固定定義の役割数 R，該当する規則数 K と補助判定の総費用 Q に対し O(R + K + Q)
	 * @param State 構築中の状態
	 * @param Parent 親
	 * @param CurNode 前の子
	 * @param Next 前の子に続いて出力する子
	 * @param Boundary 取得済の子の前の意味を持つ境界（無ければ nullptr）
	 * @return 間に空白を置くなら true
	 * @throws FormatLimitExceeded 規則の照会が作業予算を超えるか，必要な境界の役割が無い場合
	 * @throws std::bad_alloc 定義の候補を共有するメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	bool Canonicalizer::ShouldInsertSpace(
		Builder &State,
		const NodeId Parent,
		const NodeId CurNode,
		const NodeId Next,
		const SemanticBoundary *const Boundary
	) {
		// 兄弟のトークンの間の空白の要否の判定
		const SyntaxModel &Syntax = State.Syntax;
		Syntax.Charge(1);
		const SyntaxNode &Cur = Syntax.GetNode(CurNode), &Following = Syntax.GetNode(Next), &Owner = Syntax.GetNode(Parent);
		const uint32_t CurIdx = Cur.Idx, NextIdx = Following.Idx;
		// 投影が記録した境界の種類（子の組毎に１度だけ引き，記録が無ければ以下の何れの判定にも当たらない種類とする）
		const BoundaryKind Recorded = Boundary ? Boundary->Kind : BoundaryKind::Spaced;
		// トークン列の意味を持つ区切の返戻
		if(Recorded == BoundaryKind::TokenGlued || Recorded == BoundaryKind::TokenSpaced) return Recorded == BoundaryKind::TokenSpaced;
		// 区切記法で分けた境界と同一行での空白不要に空白を置かない事の返戻
		if(Recorded == BoundaryKind::Separated || Recorded == BoundaryKind::NoInlineSpace) return false;
		// 投影した必須空白の返戻
		if(Recorded == BoundaryKind::RequiredSpace) return true;
		const bool IsCurNamed = Syntax.IsNamed(CurNode), IsNextNamed = Syntax.IsNamed(Next);
		const LexicalEdge PrevEdge = State.Contexts[CurNode].LastEdge, FollowingEdge = State.Contexts[Next].FirstEdge;
		const unsigned char PrevChar = PrevEdge.Char, NextChar = FollowingEdge.Char;
		const SyntaxRules &Rules = Syntax.GetRules();
		// 此の照合中は構文を変更せず，同じ３ノードの定義を直接参照する
		const auto IsParent = [&Owner](const Role Wanted) -> bool {
			// 照会済のノードの役割の返戻
			return IsRole(Owner.Kind.Def, Wanted);
		};
		const auto IsCurNode = [&Cur](const Role Wanted) -> bool {
			// 照会済のノードの役割の返戻
			return IsRole(Cur.Kind.Def, Wanted);
		};
		const auto IsNextNode = [&Following](const Role Wanted) -> bool {
			// 照会済のノードの役割の返戻
			return IsRole(Following.Kind.Def, Wanted);
		};
		// メンバの参照の記号の字
		const unsigned char MemberAccess = Rules.Operators.MemberAccess.front();
		const KindDef *const CurToken = Cur.Kind.Def && Cur.Children.empty() ? GetSpacingTokenOf(State, CurNode) : nullptr;
		const KindDef *const FollowingToken =
		Following.Kind.Def && Following.Children.empty() ? GetSpacingTokenOf(State, Next) : nullptr;
		const KindDef *const PrevToken = GetSpacingEdgeOf(State, PrevEdge, PrevEdge.Leaf == CurNode ? CurToken : nullptr);
		const KindDef *const NextToken = GetSpacingEdgeOf(State, FollowingEdge, FollowingEdge.Leaf == Next ? FollowingToken : nullptr);
		// 前の無名トークンの役割の照会
		const auto IsCurTokenRole = [CurToken](const Role Wanted) -> bool {
			// 前のトークン全体の役割の返戻
			return IsRole(CurToken, Wanted);
		};
		// 後の無名トークンの役割の照会
		const auto IsNextTokenRole = [FollowingToken](const Role Wanted) -> bool {
			// 後のトークン全体の役割の返戻
			return IsRole(FollowingToken, Wanted);
		};
		// 前の部分木の末尾の役割の照会
		const auto DoesPrevEndAs = [PrevToken](const Role Wanted) -> bool {
			// 前の子の最後のトークンの役割の返戻
			return IsRole(PrevToken, Wanted);
		};
		// 後の部分木の先頭の役割の照会
		const auto DoesNextStartAs = [NextToken](const Role Wanted) -> bool {
			// 後の子の最初のトークンの役割の返戻
			return IsRole(NextToken, Wanted);
		};
		// 原文で密着して居た境界か（投影が読みを決める隣接として記録した物）
		const bool IsGlued = Recorded == BoundaryKind::Glued;
		// 無名のトークンの種別が列挙した物かの判定
		const auto IsCur = [&IsCurTokenRole, IsCurNamed](const std::initializer_list<Role> Roles) -> bool {
			// 前のトークンが無名で何れかの役割を持つかの返戻
			return !IsCurNamed && std::ranges::any_of(
				Roles,
				// 前のトークンが列挙した役割を持つかの照合
				[&IsCurTokenRole](const Role Wanted) -> bool {
					// 役割を持つかの返戻
					return IsCurTokenRole(Wanted);
				}
			);
		};
		// 後の名前無トークンが候補の何れかの役割を持つかの判定
		const auto IsNext = [&IsNextTokenRole, IsNextNamed](const std::initializer_list<Role> Roles) -> bool {
			// 後のトークンが無名で何れかの役割を持つかの返戻
			return !IsNextNamed && std::ranges::any_of(
				Roles,
				// 後のトークンが列挙した役割を持つかの照合
				[&IsNextTokenRole](const Role Wanted) -> bool {
					// 役割を持つかの返戻
					return IsNextTokenRole(Wanted);
				}
			);
		};
		// 密着させる後の種別（コロン・ドット・三項・任意連鎖等）かの判定
		const auto CanNextAttachColonLike = [&]() -> bool {
			// TS の後置の任意のマーカーの直前のトークンへの密着
			if(IsNextNode(Role::OptionalMarker)) {
				// 省略可能型の前に空白を置く事の返戻
				if(IsNextNode(Role::OptionalType)) return false;
				const NodeId FirstChild = Syntax.GetChild(Next, 0);
				// 後置形の任意のマーカーなら true の返戻
				return FirstChild == NoNode || !Syntax.IsTokenRole(FirstChild, Role::QuestionToken);
			}
			// 密着させる後の種別なら true の返戻
			return IsNextNode(Role::AttachColonOrDot) ||
			IsNextNode(Role::SpaceSkip) && (DoesNextStartAs(Role::ColonLead) || DoesNextStartAs(Role::QuestionLead)) ||
			IsNextNode(Role::NormColon) && DoesNextStartAs(Role::ColonLead);
		};
		// パーサーが単位と演算子を関数名と読んだ呼出（`.5rem*(x)` の `rem*(x)`・`2px-(3px)` の `px-(3px)`）で始まる値かの判定
		const auto DoesStartWithUnitCall = [&Syntax, Next]() -> bool {
			for(NodeId Head = Next; Head != NoNode; Head = Syntax.GetNamedChild(Head, 0)) {
				Syntax.Charge(1);
				// 左端の呼出の発見の返戻
				if(Syntax.IsNamed(Head) && Syntax.Is(Head, Role::CallExpr)) return true;
			}
			// 呼出で始まらない値の返戻
			return false;
		};
		// 密着させると前後の記号と融合して別のトークンに為る `=` か（`int>=0`，`x=~y`，前後の両側を同じ判定で区切る）
		const auto DoesFuseEquals = [&State, &Rules](const NodeId Equals) -> bool {
			// 記法の代入の融合条件の返戻
			return Rules.CanFuseAssignment(GetAdjacentChar(State, Equals, true), GetAdjacentChar(State, Equals, false));
		};
		// 名前付実引数・キーワードパターン・属性値・指示文の設定値の密着の判定
		const bool IsMacroLike = IsParent(Role::MacroLike), IsCurAssignment = !IsMacroLike && IsCur({ Role::AssignmentToken });
		const bool IsNextAssignment = !IsMacroLike && !IsCurAssignment && IsNext({ Role::AssignmentToken });
		// ノード・無名のトークン・部分木の端を区別した空白規則の選択
		// ノードの固定の候補は変更不能な定義の控えを直接参照する
		const auto NodeRules =
		[&State, &Syntax](const NodeId Id, const SpacingSite Site, const KindDef *const KnownToken, const SpacingCandidates &KnownRules)
		-> uint64_t {
			Syntax.Charge(1);
			NodeCtx &Ctx = State.Contexts[Id];
			if(!Ctx.SpacingKind) {
				if(KnownToken && Syntax.GetNode(Id).Kind.Def == KnownToken) {
					// 同じ定義の参照の作業量を保ち，既に引いた記号の候補を直接対応付ける
					Syntax.Charge(1);
					Ctx.SpacingKind = &KnownRules;
				} else Ctx.SpacingKind = &GetSpacingRulesOf(State, Syntax.GetNode(Id).Kind.Def);
			}
			// 照会元の候補の返戻
			return Ctx.SpacingKind->Masks[static_cast<size_t>(Site)];
		};
		const SpacingCandidates &NextRules = FollowingEdge.Leaf == NoNode ?
		GetSpacingRulesOf(State, NextToken) :
		GetNodeSpacingRules(State, NextToken, State.Contexts[FollowingEdge.Leaf].SpacingLeadingRules);
		const SpacingCandidates &CurRules = GetNodeSpacingRules(State, CurToken, State.Contexts[CurNode].SpacingTokenRules);
		const SpacingCandidates &FollowingRules =
		FollowingToken == NextToken ? NextRules : GetNodeSpacingRules(State, FollowingToken, State.Contexts[Next].SpacingTokenRules);
		uint64_t Candidates =
		NodeRules(Parent, SpacingSite::Parent, nullptr, CurRules) | NodeRules(CurNode, SpacingSite::Cur, CurToken, CurRules);
		Candidates |= NodeRules(Next, SpacingSite::Following, FollowingToken, FollowingRules);
		Candidates |= NextRules.Masks[static_cast<size_t>(SpacingSite::FollowingEdge)];
		const SpacingPair &Pair = GetSpacingPairOf(State, CurToken, FollowingToken, CurRules, FollowingRules);
		Candidates |= Pair.Candidates;
		if(!PrevChar || !NextChar) Candidates |= uint64_t{ 1 } << static_cast<size_t>(SpacingRule::IndivisibleType);
		if(Rules.HasNumericUnitAdjacency && IsGlued) Candidates |= uint64_t{ 1 } << static_cast<size_t>(SpacingRule::NumericUnit);
		if(PrevChar == MemberAccess || NextChar == MemberAccess) {
			Candidates |= uint64_t{ 1 } << static_cast<size_t>(SpacingRule::NamedAttachment) |
			uint64_t{ 1 } << static_cast<size_t>(SpacingRule::MemberAccess);
		}
		// 名前付の境界の規則は１度だけ選別し，各規則で同じ分類を再評価しない
		static constexpr uint64_t NamedRules = (uint64_t{ 1 } << static_cast<size_t>(SpacingRule::NamedElements) + 1) -
		(uint64_t{ 1 } << static_cast<size_t>(SpacingRule::NamedAttachment));
		if(IsCurNamed && IsNextNamed) Candidates |= uint64_t{ 1 } << static_cast<size_t>(SpacingRule::NamedElements);
		else Candidates &= ~NamedRules;
		while(Candidates) {
			const SpacingRule Selected = static_cast<SpacingRule>(std::countr_zero(Candidates));
			Candidates &= Candidates - 1;
			Syntax.Charge(1);
			switch(Selected) {
			case SpacingRule::AttachedPostfix:
				{
					// Swift の後置演算子の原文の隣接の判定
					// 原文の隣接の返戻
					if(
						const auto IsAttachedMark = [&Syntax, &Rules, Parent](const NodeId Operator) -> bool {
							// 左の字へ空白無で接する `?` `!` で始まるトークンかの返戻
							return Rules.AttachedOperatorPrefixChars.find(Syntax.GetFirstChar(Operator)) != std::string_view::npos &&
							IsGluedBefore(Syntax, Parent, Syntax.GetNode(Operator).Idx);
						}; IsAttachedMark(Next) || CurIdx && IsAttachedMark(CurNode)
					) return !IsGlued;
					break;
				}
			case SpacingRule::Deconstruction:
				// C# の分解の宣言 (`var (m, n) = t`) の `var` と組の形は，呼出でない為に空白で区切る事の返戻
				if(DoesNextStartAs(Role::OpeningParenLead)) return true;
				break;
			case SpacingRule::AwaitOperand:
				// 誤読した await の式を演算子の語の後として空白で区切る事の返戻
				if(
					!Cur.Facts.IsKeywordName && HasSingleTokenRole(State, CurNode, Role::AwaitKeyword) &&
					(DoesNextStartAs(Role::OpeningParenLead) || DoesNextStartAs(Role::OpeningSquareLead))
				) return true;
				break;
			case SpacingRule::CodeBoundary:
				// PHP の地の文とトークンの間の区切 (`<?= $x ?>` / `: ?>` / `<?php endforeach`) と本体の `:` の密着の返戻
				return !DoesNextStartAs(Role::ColonLead);
			case SpacingRule::AlternateColon:
				// 代替構文の本体を開くコロンの密着の返戻
				if(DoesNextStartAs(Role::ColonLead)) return false;
				break;
			case SpacingRule::IndivisibleType:
				// 型の組合せの `|` / `&` と，型注釈のフィールドへ二項式と括弧だけを介して属する `|` の密着
				// 型・名前空間・マークアップ等の不可分な記号の密着の返戻
				if(
					!IsCurNamed && (
						IsParent(Role::AttachedHashSplat) || IsCurNode(Role::AttachedLowUnaryWord) && DoesNextStartAs(Role::OpeningParenLead) ||
						HasSingleTokenRole(State, CurNode, Role::TypeDeclarationKeyword) && IsParent(Role::TypeAlias) &&
						DoesNextStartAs(Role::OpeningParenLead)
					) || !PrevChar || !NextChar || IsCurNode(Role::Error) || IsNextNode(Role::Error) || IsParent(Role::MarkupSyntax) && (
						IsCur(
							{ Role::ClosingTagOpenToken, Role::GreaterThanToken, Role::LessThanToken, Role::SelfClosingTagToken, Role::SlashToken }
						) || IsNext({ Role::GreaterThanToken })
					) || IsCurNode(Role::AttachedRefModifier) || IsParent(Role::AttachedNamespaceImport) && (
						IsCur({ Role::BackslashToken, Role::OpeningBraceToken }) ||
						IsNext({ Role::BackslashToken, Role::ClosingBraceToken, Role::OpeningBraceToken })
					) || (IsCur({ Role::AmpersandToken, Role::PipeToken }) || IsNext({ Role::AmpersandToken, Role::PipeToken })) &&
					IsParent(Role::TypeCombination) || IsParent(Role::AnnotationUnionOperator) && State.Contexts[Parent].IsTypeUnion &&
					(IsCur({ Role::PipeToken }) || IsNext({ Role::PipeToken }))
				) return false;
				break;
			case SpacingRule::SelectorOperands:
				// CSS/SCSS の結合子のセレクタの被演算子の境界は常に空白で区切られる別要素の為，一律で空白を置く事の返戻
				return true;
			case SpacingRule::RenderedContent:
				// JSX の子の間で描画される空白は本文として現れる為に密着させる事の返戻
				return false;
			case SpacingRule::NumericUnit:
				// CSS/SCSS の単位断片と減算らしい値列，単位と演算子を関数名と読んだ呼出で始まる値は原文の隣接を保持
				if(IsNextNode(Role::UnitText) || IsNextNode(Role::DimensionNum) || DoesStartWithUnitCall()) {
					// 参照型の末尾のノード
					NodeId Tail = CurNode;
					while(!Syntax.Is(Tail, Role::DimensionNum)) {
						Syntax.Charge(1);
						const NodeCtx &Ctx = State.Contexts[Tail];
						if(!Ctx.NamedCount) break;
						Tail = Syntax.GetChild(Tail, Ctx.LastNamed);
					}
					// 数値と単位の接続を保つ事の返戻
					if(Syntax.Is(Tail, Role::DimensionNum)) return false;
				}
				break;
			case SpacingRule::WordArrayElements:
				// Ruby の単語の配列の要素を空白で区切る事（記号で始まる要素 `%w(a ,b)` の `,b` も密着すると前の要素と繋がる）の返戻
				if(IsCurNamed && IsNextNamed && IsParent(Role::WordArray)) return true;
				break;
			case SpacingRule::EmptyStmt:
				// 空文を直前のトークンへ密着させる事 (`if(c);` / `else;`) の返戻
				if(
					IsNextNamed && DoesNextStartAs(Role::StmtTerminatorLead) &&
					!(IsNextNode(Role::EnumBodyDecls) && IsCur({ Role::OpeningBraceToken }))
				) return false;
				break;
			case SpacingRule::CaptureElements:
				// ラムダの捕捉の `[` と接頭辞 `&` `*`(`*this`) の後，`]` `,` の前は密着させ，其れ以外は空白を置く事の返戻
				return !IsCur({ Role::AmpersandToken, Role::AsteriskToken, Role::OpeningBracketToken }) &&
				!IsNext({ Role::ClosingSquareToken, Role::CommaToken });
			case SpacingRule::ControlBody:
				// Kotlin の制御本体・ラベル・アノテーションと後続式の空白区切
				// 本体・被修飾式との区切の返戻
				if(IsNextNode(Role::SpacedControlBody) || IsParent(Role::LabeledPrefix) && IsCurNode(Role::Label)) return true;
				break;
			case SpacingRule::GeneratorName:
				// ジェネレータ関数の星と語の密着及び後続の名前の空白区切
				// ジェネレータの名前との区切の返戻
				if(!IsParent(Role::MethodDef) && (IsCurTokenRole(Role::AsteriskToken) || IsNextTokenRole(Role::AsteriskToken))) {
					return IsCurTokenRole(Role::AsteriskToken) && IsNextNode(Role::FunctionNameIdentifier);
				}
				break;
			case SpacingRule::IndivisibleSyntax:
				// C++, C#, Java, Kotlin, Swift, JavaScript, TypeScript, CSS の不可分の複合トークンの密着の返戻
				// 不透明なトークン列の名前と感嘆符，感嘆符と後続の囲みも密着させる
				if(
					Owner.Facts.TokenSpacing == TokenSpacingMode::Punctuation && (
						IsCurNamed && IsNextNode(Role::ExclamationMark) || IsCurNode(Role::ExclamationMark) && DoesNextStartAs(Role::OpeningBracketLead)
					) || IsParent(Role::AttachedKeywordBrackets) && IsCurNode(Role::SpaceKeyword) && IsNextTokenRole(Role::OpeningBracketToken) ||
					IsParent(Role::AttachedNegationTest) && IsCurTokenRole(Role::ExclamationToken) && IsNextNode(Role::NegatableTestWord) ||
					IsParent(Role::AttachedMagnitudeSuffix) || IsParent(Role::AttachedNumericUnit) || IsParent(Role::AttachedRangeOrUnary) ||
					IsCurNode(Role::AttachedPercent) && IsNextNamed || IsParent(Role::AttachedSelector) || IsNextNamed && (
						IsParent(Role::VariadicTypeParam) && IsCurTokenRole(Role::SpreadToken) ||
						IsParent(Role::MethodDef) && IsCurTokenRole(Role::AsteriskToken)
					) || IsParent(Role::NewParenCall) && HasSingleTokenRole(State, CurNode, Role::ConstructionKeyword) &&
					DoesNextStartAs(Role::OpeningParenLead) ||
					IsParent(Role::FunctionType) && (IsCurTokenRole(Role::DotToken) || IsNextTokenRole(Role::DotToken)) ||
					IsParent(Role::SpreadParam) && IsNextTokenRole(Role::SpreadToken) ||
					IsParent(Role::PackExpansion) && (IsNextTokenRole(Role::SpreadToken) || IsCurTokenRole(Role::SpreadToken) && IsNextNamed) ||
					IsParent(Role::SizeofExpr) && (IsCurTokenRole(Role::SpreadToken) || IsNextTokenRole(Role::SpreadToken)) ||
					IsParent(Role::JumpExpr) && IsNextNode(Role::JumpLabel) ||
					IsParent(Role::SelfRef) && HasSingleTokenRole(State, CurNode, Role::LabeledReceiverKeyword) || IsParent(Role::BaseRef) && (
						!IsCurNamed && (
							HasSingleTokenRole(State, CurNode, Role::BaseReceiverKeyword) ||
							HasSingleTokenRole(State, CurNode, Role::LabeledReceiverKeyword)
						) || IsCur({ Role::AtToken, Role::GreaterThanToken, Role::LessThanToken }) ||
						IsNext({ Role::AtToken, Role::GreaterThanToken, Role::LessThanToken })
					) ||
					IsParent(Role::Indexer) && HasSingleTokenRole(State, CurNode, Role::SelfReceiverKeyword) && IsNextNode(Role::IndexerParams) ||
					// C# の型を省いた配列の生成 `new[] { 1 }` は `new` と `[` を密着
					// 後置の `!` は前の名前付の要素と後続の型注釈へ密着
					// Rust のループのラベルの `:` は他の言語のラベルと同じくラベルへ密着させる (`'outer: loop`)
					IsParent(Role::InferredArrayConstruction) && HasSingleTokenRole(State, CurNode, Role::ConstructionKeyword) ||
					IsCurNamed && IsNextNode(Role::PostfixAssertionMark) ||
					IsCurNode(Role::PostfixAssertionMark) && IsNextNode(Role::TypeAnnotation) || IsParent(Role::RangeAssignment) && (
						IsNext({ Role::AssignmentToken }) && HasTrailingOpenRange(State, CurNode) ||
						IsCur({ Role::AssignmentToken }) && HasTrailingOpenRange(State, Syntax.GetNamedChild(Parent, 0))
					) || IsCurNode(Role::AttachedLabel) && IsNext({ Role::ColonToken })
				) return false;
				break;
			case SpacingRule::AttributesAndTypes:
				// HTML の属性間と型を始める括弧の前の空白区切の返戻
				if(
					IsParent(Role::SpacedAttributeTag) && IsNextNode(Role::Attribute) || DoesNextStartAs(Role::OpeningParenLead) && (
						IsParent(Role::SingletonMethod) && HasSingleTokenRole(State, CurNode, Role::DefinitionKeyword) ||
						(IsCurNamed || Rules.IsFusionLetter(PrevChar)) && HasLeadingParenType(State, Next)
					)
				) return true;
				break;
			case SpacingRule::CastBoundary:
				// 型変換の語だけを括弧の外側で区切る事の返戻
				return !IsCurNamed && HasSingleTokenRole(State, CurNode, Role::CastKeyword) ||
				!IsNextNamed && HasSingleTokenRole(State, Next, Role::CastKeyword);
			case SpacingRule::LiteralInterior:
				// 括弧・引用等の内側を密着させる事の返戻
				if(
					IsParent(Role::AttributeDecl) && (IsCur({ Role::DoubleSquareOpenToken }) || IsNext({ Role::DoubleSquareCloseToken })) ||
					IsParent(Role::InterpolatedLiteral) ||
					IsParent(Role::TemplateInterpolation) && (IsCur({ Role::InterpolationOpenToken }) || IsNext({ Role::ClosingBraceToken })) ||
					IsParent(Role::Interpolation) || IsParent(Role::InterpolationAlignment) ||
					IsParent(Role::TypedLambda) && IsCur({ Role::OpeningParenToken }) ||
					IsParent(Role::BlockArg) && IsCur({ Role::AmpersandToken }) ||
					IsParent(Role::ClassSelector) && (IsCurNode(Role::NestingSelector) || IsNextNode(Role::SelectorName)) ||
					IsParent(Role::IdSelector) ||
					IsParent(Role::WordArray) && (IsCurNode(Role::WordArrayOpen) || IsNextNode(Role::WordArrayClose)) ||
					IsParent(Role::AttributeSelector) && !IsCurNode(Role::Error) && !IsNextNode(Role::Error)
				) return false;
				break;
			case SpacingRule::NamedOperator:
				// Kotlin の名前付の演算子と括弧の境界は呼出と誤認しない様に空白を置く事の返戻
				return true;
			case SpacingRule::CompositeBrace:
				// `{` を密着で開く親（複合リテラル等）は，型の名前・閉括弧の何方の後も密着で開く事の返戻
				if(DoesNextStartAs(Role::OpeningBraceLead)) return false;
				break;
			case SpacingRule::NamedAttachment:
				// 親と境界の密着規則に従う，末尾ラムダ以外の構文要素の密着の返戻
				if(
					IsMacroLike || IsCurNode(Role::OptionalChain) || PrevChar == MemberAccess && !DoesPrevEndAs(Role::SpreadToken) ||
					DoesNextStartAs(Role::CommaLead) || IsParent(Role::PrefixUnary) && IsCurNode(Role::ExclamationMark) ||
					IsParent(Role::PostfixExpr) && IsNextNode(Role::ExclamationMark) || IsNextNode(Role::AttachBracket) ||
					CanNextAttachColonLike() ||
					IsNextNode(Role::CallSuffix) && (DoesNextStartAs(Role::OpeningParenLead) || DoesNextStartAs(Role::OpeningAngleLead)) ||
					IsParent(Role::LeadingTypeArgs) && IsCurNode(Role::GenericArgs) ||
					IsParent(Role::CallExpr) && IsNextNode(Role::InterpolatedLiteral)
				) return false;
				break;
			case SpacingRule::AnnotationArgs:
				// 原文の空白に依る Kotlin の型注釈の実引数と関数型の区別
				if(IsNextNode(Role::ValArgs)) {
					if(
						const NodeId Annotation = Syntax.GetParentOf(Parent);
						Syntax.Is(Annotation, Role::Annotation) && Syntax.Is(Syntax.GetParentOf(Annotation), Role::TypeModifiers)
					// 原文の隣接の返戻
					) return !IsGlued;
				}
				break;
			case SpacingRule::LambdaParams:
				// C# のラムダの仮引数の並びの前は名前でなく修飾・戻値の型・属性の為，空白で区切る事の返戻
				return true;
			case SpacingRule::NamedCall:
				{
					const bool IsNextParen = DoesNextStartAs(Role::OpeningParenLead);
					// Ruby の括弧無の実引数列（名前に密着した `(` は Ruby が実引数の括弧と読み，パーサーの読違の為，原文で離れて居た物だけ）
					const bool IsBareCallArgs =
					IsNextNode(Role::BareCallArgs) && IsNextParen && Syntax.IsNamed(Syntax.GetChild(Next, 0)) && !IsGlued;
					// `(` 直前は密着，但しタプル返戻の `func()(...)` の続き `(` と Ruby の括弧無の実引数列のみ空白有の返戻
					return !IsNextParen || IsParent(Role::ReturnTuple) && DoesPrevEndAs(Role::ClosingParenTail) || IsBareCallArgs;
				}
			case SpacingRule::GenericArgs:
				// 型仮引数の並びの前は修飾子の後だけ空白を置く事の返戻
				return IsCurNode(Role::ModifierSeq);
			case SpacingRule::NamedBracket:
				// トークン列の属性記号も，通常の角括弧と密着させる
				if(
					Owner.Facts.TokenSpacing != TokenSpacingMode::None && IsCurTokenRole(Role::HashToken) &&
					DoesNextStartAs(Role::OpeningSquareLead)
				) return false;
				// 文区切・カンマ・型・制御本体等の後の括弧前だけ空白を置き，条件に本体の括弧を密着させ，呼出も変えない事の返戻
				return DoesPrevEndAs(Role::StmtTerminatorTail) || DoesPrevEndAs(Role::CommaTail) ||
				DoesPrevEndAs(Role::ClosingParenTail) && (IsParent(Role::ReturnTuple) || IsParent(Role::Control)) ||
				DoesNextStartAs(Role::OpeningParenLead) && IsNextNode(Role::CompoundDeclarator) ||
				DoesNextStartAs(Role::OpeningSquareLead) && (IsNextNode(Role::BracketType) || IsCurNode(Role::PlaceholderTypeSpecifier)) ||
				IsNextNode(Role::SpacedValue) || IsCurNode(Role::NamedKeyword);
			case SpacingRule::NamedElements:
				// 其の他の名前の間へ空白を置く事の返戻
				return true;
			case SpacingRule::ExceptionStar:
				// Python の例外グループの `except*` の密着と，後続の型との空白区切の判定 (`except* TypeError`)
				// 節の語と `*` を密着させる事の返戻
				if(CurNode == Syntax.GetChild(Parent, 0) && IsNextTokenRole(Role::AsteriskToken)) return false;
				// `*` の後に空白を置く事の返戻
				if(IsCurTokenRole(Role::AsteriskToken)) return true;
				break;
			case SpacingRule::PostfixPtr:
				// C# の後置のポインタの記号と型名の密着
				// 型に付く後置の記号の密着の返戻
				if(IsCurNamed && IsNext({ Role::AmpersandToken, Role::AsteriskToken })) return false;
				break;
			case SpacingRule::PtrDeclarator:
				if(!IsCurNamed) {
					// 読違えた展開の原文の隣接の返戻
					if(IsParent(Role::ArgSpread) && !Syntax.Is(Syntax.GetParentOf(Parent), Role::ValArg)) return !IsGlued;
					// `*` / `&` / `...` の各記号の直後の内，開きのトークンの後と名前でないトークンの前を密着させる事の返戻
					if(IsCurNode(Role::OpenTailToken) || !IsNextNamed) return false;
					// 英数字同士の境界（語 + 識別子）なら空白要の返戻
					return Rules.IsFusionAlnum(PrevChar) && Rules.IsFusionAlnum(NextChar);
				}
				break;
			case SpacingRule::PrefixOperator:
				if(!IsCurNamed && IsNextNamed) {
					// Ruby の符号を数値で始まる式へ密着させると符号付の数値に為る為，空白で分離する
					// 数値の符号の分離の返戻
					if(
						IsParent(Role::SignedOperandHost) && (PrevChar == Rules.Operators.Minus.back() || PrevChar == Rules.Operators.Plus.back()) &&
						!CanAttachNumericSign(Syntax, Next)
					) return true;
					// 演算子のトークンが英字（語）で始まるなら空白要の返戻
					return Rules.IsFusionLetter(Syntax.GetFirstChar(CurNode));
				}
				break;
			case SpacingRule::PostfixOperator:
				// 後置の演算子へ密着させる事の返戻
				if(IsCurNamed && !IsNextNamed) return false;
				break;
			case SpacingRule::ConditionalColon:
				// PHP のエルビス演算子の一体の記号 `?` `:` の密着と，其の他の境界の空白区切の返戻
				if(IsCur({ Role::ColonToken, Role::QuestionToken }) || IsNext({ Role::ColonToken, Role::QuestionToken })) {
					return !(IsCur({ Role::QuestionToken }) && IsNext({ Role::ColonToken }));
				}
				break;
			case SpacingRule::RangeFusion:
				// 範囲演算子同士 (Rust `.. ..`) を密着させると `....` が別のトークン `...` `.` と読まれる為，空白で区切る事の返戻
				if(Rules.CanFuseRangePair(PrevChar, NextChar)) return true;
				break;
			case SpacingRule::OptionalAndRange:
				// Swift の二重の省略可能型を含む，省略可能の記号・範囲演算子の密着の返戻
				if(
					IsParent(Role::OptionalMarker) && (IsCur({ Role::CoalescingToken }) || IsNext({ Role::CoalescingToken })) ||
					IsNext({ Role::OptionalAccessToken, Role::QuestionToken }) ||
					IsCur({ Role::OptionalAccessToken, Role::QuestionToken }) && !IsNext({ Role::AssignmentToken }) || IsParent(Role::RangeLike)
				) return false;
				break;
			case SpacingRule::BinaryOperands:
				{
					// 条件の演算が直接のトークンに為るディレクティブの，見出・論理演算・前置否定の区切
					if(IsParent(Role::HardLineDirective)) {
						// 見出と中置の論理演算子を空白で区切る事の返戻
						if(!CurIdx || IsCur({ Role::LazyLogicalOperator }) || IsNext({ Role::LazyLogicalOperator })) return true;
						// 前置否定を被演算子へ密着させる事の返戻
						if(IsCur({ Role::ExclamationToken })) return false;
						break; // 呼出・括弧・区切の境界は各役割の規則へ渡し，無名の子を中置演算子と見做さない
					}
					// 畳込の省略記号は被演算子として扱い，演算子との境界を空白で区切る
					if(IsParent(Role::FoldExpr) && (IsCurTokenRole(Role::SpreadToken) || IsNextTokenRole(Role::SpreadToken))) {
						return !IsCurTokenRole(Role::OpeningParenToken) && !IsNextTokenRole(Role::ClosingParenToken) &&
						!IsNextTokenRole(Role::CommaToken);
					}
					// 幅の無い左辺を持つ二項式の境界は，投影した意味上の空白を優先し，其の他は右被演算子の前置単項として密着
					if(IsParent(Role::BinaryOperation)) {
						if(const NodeId FirstChild = Syntax.GetNamedChild(Parent, 0); FirstChild != NoNode) {
							if((FirstChild == CurNode ? PrevEdge : State.Contexts[FirstChild].LastEdge).Leaf == NoNode) {
								// 幅の無い左辺の式に記録した意味上の空白の返戻
								return Boundary && Recorded == BoundaryKind::Spaced;
							}
							if(Syntax.Is(FirstChild, Role::OpenRangeOperand)) {
								// 終端の無い範囲を密着させる事の返戻
								if(
									const uint32_t RangeChildren = Syntax.GetChildCount(FirstChild);
									RangeChildren && Syntax.IsTokenRole(Syntax.GetChild(FirstChild, RangeChildren - 1), Role::OpenRangeToken)
								) return false;
							}
						}
					}
					// C 形式の型変換を二項式と誤読した時はキャストの記号を密着
					// 型照会又は型変換の括弧の判定
					// キャストに続く単項演算子を密着させる事の返戻
					if(IsParent(Role::CastLookalikeOperator)) if(
						const auto IsCastLikeParen = [&Syntax, &State](const NodeId Paren) -> bool {
							// 単一の子を持つ括弧の式でない事の返戻
							if(!Syntax.Is(Paren, Role::ValParens) || State.Contexts[Paren].NamedCount != 1) return false;
							// 中身が型名相当の種別かの返戻
							return Syntax.Is(Syntax.GetChild(Paren, State.Contexts[Paren].FirstNamed), Role::CastTypeAtom);
						}; IsCurNamed && DoesNextStartAs(Role::UnaryPrefixLead) && IsCastLikeParen(CurNode) ||
						!IsCurNamed && IsCurNode(Role::UnaryPrefixToken) && CurIdx && IsCastLikeParen(Syntax.GetChild(Parent, CurIdx - 1)) &&
						PrevChar != NextChar
					) return false;
					// 名前付の子の最初と最後の位置
					const NodeCtx &Ctx = State.Contexts[Parent];
					// 関数の語の直後の仮引数の並びを除く，中置演算子の被演算子の空白区切の返戻
					if(
						const uint32_t FirstNamed = Ctx.FirstNamed, LastNamed = Ctx.LastNamed;
						(
							!IsCurNamed && CurIdx > FirstNamed && CurIdx < LastNamed && !IsCurNode(Role::SeparatorToken) ||
							!IsNextNamed && NextIdx > FirstNamed && NextIdx < LastNamed && !IsNextNode(Role::SeparatorToken) &&
							!IsNextTokenRole(Role::ClosingParenToken)
						) && !(IsNextNamed && IsNextNode(Role::ParamContainer))
					) return true;
					break;
				}
			case SpacingRule::TagInterior:
				// HTML のタグの開閉の記号の内側は空白を置かないが `/>` の直前は１空白へ揃える事の返戻
				return !IsCur({ Role::ClosingTagOpenToken, Role::LessThanToken }) && !IsNext({ Role::GreaterThanToken });
			case SpacingRule::TightAssignment:
				if(
					IsMacroLike ||
					(IsCurAssignment || IsNextAssignment) && IsParent(Role::EqualsTight) && !DoesFuseEquals(IsCurAssignment ? CurNode : Next)
				) {
					// `operator new` / `operator delete` 等の語の演算子の区切の返戻
					return IsParent(Role::OperatorName) && PrevChar && NextChar && Rules.IsFusionNameStart(PrevChar) &&
					Rules.IsFusionNameStart(NextChar);
				}
				break;
			case SpacingRule::DestructorName:
				// C# のファイナライザーの `~` を名前へ密着させる事の返戻
				if(IsCur({ Role::TildeToken })) return false;
				break;
			case SpacingRule::SpacedAssignment:
				// 代入の `=` の前後に空白を置く事の返戻
				if(
					(IsCurAssignment || IsNextAssignment) && (
						IsParent(Role::EqualsSpaced) || IsCurNode(Role::TightParamEquals) ||
						Syntax.Is(Syntax.GetPrevSibling(CurNode), Role::TightParamEquals)
					)
				) return true;
				break;
			case SpacingRule::AttachedSeparator:
				// 区切の記号を密着させる事の返戻
				if(
					IsParent(Role::Compact) && (IsCurAssignment || IsNextAssignment) || !IsCurNamed && Pair.IsCurAttached ||
					!IsNextNamed && Pair.IsFollowingAttached || IsParent(Role::ChannelType) && !IsCurNamed && !IsNextNamed ||
					IsParent(Role::BracketedType) && IsCur({ Role::ClosingSquareToken }) ||
					IsParent(Role::AngleBracketList) && (IsCur({ Role::LessThanToken }) || IsNext({ Role::GreaterThanToken })) ||
					IsNextNamed && IsCur({ Role::AtToken }) || (IsParent(Role::Arg) || IsParent(Role::Param)) && IsNext({ Role::SpreadToken }) ||
					IsParent(Role::KeyPath) && IsCur({ Role::BackslashToken }) || IsParent(Role::RefAssignment) && IsCur({ Role::AmpersandToken })
				) return false;
				break;
			case SpacingRule::AfterSeparator:
				if(IsCur({ Role::ColonToken, Role::CommaToken, Role::StmtTerminatorToken })) {
					// 閉括弧・同じトークン以外が続く場合の空白要の返戻
					return !DoesNextStartAs(Role::ClosingParenLead) && !DoesNextStartAs(Role::ClosingSquareLead) && !(
						IsCurTokenRole(Role::ColonToken) ?
						DoesNextStartAs(Role::ClosingBraceLead) ||
						IsParent(Role::StyleLexeme) && IsParent(Role::TypedDecl) && DoesNextStartAs(Role::StmtTerminatorLead) :
						DoesNextStartAs(Role::StmtTerminatorLead)
					) && !(DoesNextStartAs(Role::PipeLead) && IsParent(Role::PipeParamList));
				}
				break;
			case SpacingRule::BraceInterior:
				// 前のトークンが `{` 又は後のトークンが `}` の共通の分岐
				if(
					const bool IsCurOpen = IsCur({ Role::OpeningBraceToken }), IsNextClose = IsNext({ Role::ClosingBraceToken });
					IsCurOpen || IsNextClose
				) {
					// 空白を置かない波括弧の返戻
					if(IsParent(Role::SpacelessBrace) || IsCurOpen && IsNextClose) return false;
					// `{ ... }` の内側に空白を置く波括弧の返戻
					if(
						IsParent(Role::ObjectLike) || IsParent(Role::IndentContainer) || IsParent(Role::SwitchBodyLayout) ||
						IsParent(Role::LambdaLiteral) || IsParent(Role::MacroRuleDef) || Owner.Facts.TokenSpacing != TokenSpacingMode::None
					) return true;
				}
				break;
			case SpacingRule::PipeDelimiter:
				// 仮引数を囲む `|` と CSS の名前空間の区切 `|` を密着させる事の返戻
				if(
					(IsParent(Role::PipeParamList) || IsParent(Role::AttachedSelectorNamespace)) &&
					(IsCur({ Role::PipeToken }) || IsNext({ Role::PipeToken }))
				) return false;
				break;
			case SpacingRule::BeforeColon:
				if(IsNext({ Role::ColonToken })) {
					// `:` の前は制御文用・中置演算子・通常のコロン・節の先頭の直後以外で空白を置く事の返戻
					return !(
						IsParent(Role::ColonControl) || IsParent(Role::InfixOperator) || IsParent(Role::NormColon) ||
						Owner.Facts.TokenSpacing == TokenSpacingMode::Punctuation || IsCurNamed && IsCurNode(Role::SectionHeader)
					);
				}
				break;
			case SpacingRule::KeywordAndInt:
				if(
					!IsCurNamed && HasSingleTokenRole(State, CurNode, Role::CaseKeyword) && IsNext({ Role::DotToken }) || IsCurNamed && (
						IsCurNode(Role::NamedKeyword) && (IsNext({ Role::DotToken }) || DoesNextStartAs(Role::OpeningParenLead)) ||
						IsNext({ Role::DotToken }) && IsCurNode(Role::IntMemberBoundary) && std::ranges::all_of(
							Syntax.GetText(CurNode),
							// 数字又は桁区切の字の照合
							[&Rules, &Syntax](const unsigned char Char) -> bool {
								Syntax.Charge(1);
								// １０進の数字か桁の区切かの返戻
								return std::isdigit(Char) || Char == Rules.DigitSeparator;
							}
						)
					)
				) return true;
				break;
			case SpacingRule::MemberAccess:
				// メンバの参照とラムダの仮引数の並びを密着させる事の返戻
				if(
					IsCur({ Role::DotToken, Role::OptionalAccessToken, Role::SafeAccessToken }) ||
					IsNext({ Role::DotToken, Role::OptionalAccessToken, Role::SafeAccessToken }) ||
					IsNextNamed && (NextChar == MemberAccess && IsParent(Role::Member) || IsNextNode(Role::OptionalChain)) ||
					IsCurNamed && (PrevChar == MemberAccess && !DoesPrevEndAs(Role::SpreadToken) || IsCurNode(Role::OptionalChain)) ||
					IsParent(Role::AttachedArrowLambda) && IsCur({ Role::RightArrowToken }) && DoesNextStartAs(Role::OpeningParenLead)
				) return false;
				break;
			case SpacingRule::ArrowOperator:
				// メンバ参照でない矢印の演算子の前後へ空白を置く事の返戻
				if(
					IsCur({ Role::FatArrowToken, Role::OptionalArrowToken, Role::RightArrowToken }) ||
					IsNext({ Role::FatArrowToken, Role::OptionalArrowToken, Role::RightArrowToken })
				) return !IsParent(Role::Member);
				break;
			case SpacingRule::TypeBody:
				// Go の型本体はメンバの有無に依らず語と開きを空白で区切る事の返戻
				if(DoesNextStartAs(Role::OpeningBraceLead)) return true;
				break;
			case SpacingRule::BodyBrace:
				// 密着させる親は先に処理済の為，残る `{` の前に空白を置く事の返戻
				if(IsNext({ Role::OpeningBraceToken })) return true;
				break;
			case SpacingRule::ValCall:
				if(IsCurNamed && IsNext({ Role::OpeningBracketToken, Role::OpeningParenToken })) {
					// 組の返戻に続く括弧の空白の返戻
					return IsNextTokenRole(Role::OpeningParenToken) && IsParent(Role::ReturnTuple) && DoesPrevEndAs(Role::ClosingParenTail);
				}
				break;
			case SpacingRule::TypeQuery:
				// C# の型演算子 `typeof` と，全言語の `sizeof` の開始の括弧への密着の返戻
				if(DoesNextStartAs(Role::OpeningParenLead)) return false;
				break;
			case SpacingRule::DeclParen:
				// Go の一括宣言の括弧 (`const (`) を語から空白で区切る事の返戻
				if(DoesNextStartAs(Role::OpeningParenLead)) return true;
				break;
			case SpacingRule::KeywordParen:
				if(!IsCurNamed && (DoesNextStartAs(Role::OpeningParenLead) || DoesNextStartAs(Role::OpeningSquareLead))) {
					const bool IsControlKeyword = IsCurNode(Role::ControlKeyword) ||
					IsParent(Role::ConstantCondHost) && HasSingleTokenRole(State, CurNode, Role::ConstantConditionKeyword);
					// 無名の括弧の前は文脈別に空白を決める事の返戻
					return HasSingleTokenRole(State, CurNode, Role::FunctionKeyword) && IsParent(Role::MethodDecl) || IsParent(Role::ExportDecl) ||
					IsParent(Role::SpacedFunctionHead) && !HasSingleTokenRole(State, CurNode, Role::FunctionKeyword) ||
					IsControlKeyword && (IsNextNamed && IsNextNode(Role::PatternKind) || Rules.HasBareControlHeader) ||
					IsNextNamed && IsNextNode(Role::ArgSeq) && DoesNextStartAs(Role::OpeningSquareLead) || !IsControlKeyword &&
					!(IsParent(Role::ValJump) && IsNextNamed && IsNextNode(Role::ArgSeq) && !Syntax.IsNamed(Syntax.GetChild(Next, 0))) && (
						// `=` / `:=` の右が括弧で始まる形は，密着させると `=(` が別の演算子として読まれる言語が有る為，常に空白を置く
						IsCurAssignment || IsCur({ Role::ShortAssignmentToken }) || IsCurNode(Role::SpaceKeyword) || IsCurNode(Role::ArrowToken) ||
						!(IsNextNamed && (IsNextNode(Role::AttachParen) || IsNextNode(Role::AttachBracket))) && (
							IsNextNamed && Rules.IsFusionLetter(PrevChar) && !IsParent(Role::BracketedType) ||
							IsCurTokenRole(Role::ClosingParenToken) && IsParent(Role::Control)
						)
					);
				}
				break;
			case SpacingRule::UnnamedAttachment:
				if(!IsCurNamed && IsNextNamed) {
					// Swift の演算子の関数名と型仮引数の開始のトークンの融合を防ぐ空白区切
					// 修飾子・`fun`・演算子の関数の名前の後の空白要の返戻
					if(IsNextNode(Role::AngleBracketList)) return IsCurNode(Role::GenericFunctionHead) || IsParent(Role::OperatorFunctionDecl);
					// コロン類を密着させる事の返戻
					if(CanNextAttachColonLike()) return false;
				}
				break;
			case SpacingRule::OrdinaryElements:
				// 密着の規則に該当しない代入又は読捨の値の区切の返戻
				return true;
			case SpacingRule::Count:
				break;
			}
		}
		// 語・値・型の通常の要素の区切の返戻
		if(IsCurNamed || IsNextNamed || Rules.IsFusionAlnum(PrevChar) || Rules.IsFusionAlnum(NextChar)) return true;
		// 役割が無い境界の投影の不具合の報告
		throw FormatLimitExceeded("a token boundary has no spacing role", FailureReason::FormatterDefect);
	}
} // namespace Core
