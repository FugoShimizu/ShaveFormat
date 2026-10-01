#include "Canonicalizer.hpp"
#include "../Util/CharClass.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * コードを開く境界の末尾の判定関数
	 * @param Text 境界の字句
	 * @param Suffix 文法の定める開始記号
	 * @return 英字の大小を区別せずに末尾が一致すれば true
	 */
	static bool EndsWithCodeOpening(const std::string_view Text, const std::string_view Suffix) {
		// 開始記号より短い字句と，英字の大小を区別しない末尾の一致の返戻
		return Text.size() >= Suffix.size() && std::equal(
			Suffix.begin(),
			Suffix.end(),
			Text.end() - static_cast<std::ptrdiff_t>(Suffix.size()),
			[](const char Left, const char Right) -> bool {
				// 小文字へ揃えた一致の返戻
				return std::tolower(static_cast<unsigned char>(Left)) == std::tolower(static_cast<unsigned char>(Right));
			}
		);
	}

	/**
	 * 継続用の包装を要する式の判定関数
	 * 文の境界から外側へ遡り，既存の囲みか同じ役割の外側の式が継続を受け持つ場合は重ねない
	 * @param Syntax 構文モデル
	 * @param Id 式
	 * @return 此の式の展開時に包装を選ぶなら true
	 */
	bool Canonicalizer::NeedsContinuation(const SyntaxModel &Syntax, const NodeId Id) {
		// 継続用の包装を選べない式の返戻
		if(!Syntax.Is(Id, Role::ContinuationExpression)) return false;
		// 型の内側に既に在る囲みが継続を担う事の返戻
		if(
			Syntax.Is(Id, Role::TypeSyntax) && Syntax.NamedChildCount(Id) == 1 &&
			Syntax.FirstTokenRole(Syntax.NamedChild(Id, 0), Role::OpeningParenLead)
		) return false;
		const NodeId ParentOfId = Syntax.ParentOf(Id);
		// 型の別名の名前と，並びでない代入・繰返の束縛先を包まない事の返戻（囲むと解析器が名前でなく組の模式と読む）
		if(
			Syntax.HasField(Id, ChildRole::Left) && (
				Syntax.Is(ParentOfId, Role::TypeAlias) ||
				!Syntax.Is(Id, Role::BareTuple) && (Syntax.Is(ParentOfId, Role::Assignment) || Syntax.Is(ParentOfId, Role::Loop))
			)
		) return false;
		// 祖先の式と既存の括弧による継続の確認
		for(NodeId Child = Id, Parent = ParentOfId; Parent != NoNode; Child = Parent, Parent = Syntax.ParentOf(Parent)) {
			if(Syntax.Is(Parent, Role::IndentedBody) || Syntax.ParentOf(Parent) == NoNode) break;
			if(
				Syntax.HasField(Child, ChildRole::Body) ||
				(Syntax.HasField(Child, ChildRole::Left) || Syntax.HasField(Child, ChildRole::Right)) &&
				(Syntax.Is(Parent, Role::Assignment) || Syntax.Is(Parent, Role::Loop))
			) break;
			// 外側の式が継続を担う事の返戻
			if(Syntax.Is(Parent, Role::ContinuationExpression)) return false;
			if(Syntax.Is(Parent, Role::PostfixReceiverHost)) {
				// 受け手より後の囲みの中身（字句の葉か名前付の子を持つ子）の有無
				bool HasBracketContent = false;
				for(const NodeId Part : Syntax.ChildrenOf(Parent)) {
					HasBracketContent =
					HasBracketContent || Part != Child && Syntax.IsNamed(Part) && (!Syntax.ChildCount(Part) || Syntax.NamedChildCount(Part));
				}
				// 後置の囲みの内側で折る事の返戻（空の囲みは折れない為，其の前の受け手は外側の継続へ委ねる）
				if(Syntax.NamedChild(Parent, 0) != Child || HasBracketContent) return false;
			}
			int Depth = 0;
			for(const NodeId Sibling : Syntax.ChildrenOf(Parent)) {
				if(Sibling == Child) break;
				if(Syntax.IsNamed(Sibling)) continue;
				Depth += Syntax.BracketLeadDelta(Sibling);
			}
			// 既存の括弧が継続を担う事の返戻
			if(Depth > 0) return false;
		}
		// 外の式又は囲みが継続を受け持たない事の返戻
		return true;
	}

	/**
	 * 字句の折返の側の判定関数（行分割仕様 2.2）
	 * 開き・要素の区切り・演算子は字句の後，閉じ・メンバ参照は字句の前で折る
	 * @param Syntax 構文モデル
	 * @param Token 字句の節点
	 * @param IsFirst 親の最初の名前付の子より前の字句か
	 * @return 折返の側（折らない字句は無し）
	 */
	Canonicalizer::BreakSide Canonicalizer::SplitSide(const SyntaxModel &Syntax, const NodeId Token, const bool IsFirst) {
		const std::string_view Text = Syntax.GetNode(Token).Lexeme;
		const SyntaxRules &Rules = Syntax.Rules();
		// 記法が定める語の演算子・節の区切り・メンバの参照
		// 行末へ置く語の演算子の返戻
		if(!IsFirst && std::ranges::find(Rules.TailOperatorWords, Text) != Rules.TailOperatorWords.end()) return BreakSide::Tail;
		// 行頭へ置く節の語の返戻
		if(std::ranges::find(Rules.HeadBoundaryWords, Text) != Rules.HeadBoundaryWords.end()) return BreakSide::Head;
		// 行頭へ置く成員参照の返戻
		if(Rules.HasHeadMemberArrow && Syntax.IsTokenRole(Token, Role::MemberAccessArrow)) return BreakSide::Head;
		// 自動の文区切りを避ける行末の返戻
		if(Rules.HasAutomaticLineTerminator && Syntax.IsTokenRole(Token, Role::DotToken)) return BreakSide::Tail;
		// 行頭へ置く字句（行の先頭では折らない）の返戻
		if(Syntax.IsTokenRole(Token, Role::LeadingBreakToken)) return IsFirst ? BreakSide::None : BreakSide::Head;
		// 何処でも行末に置く字句の返戻
		if(Syntax.IsTokenRole(Token, Role::TrailingBreakToken)) return BreakSide::Tail;
		// 演算子を行末に置く事の返戻
		return !IsFirst && Syntax.IsTokenRole(Token, Role::TrailingOperatorToken) ? BreakSide::Tail : BreakSide::None;
	}

	/**
	 * 節点の外側で行が続くかの判定関数（整形仕様 1.5, 2.1，行分割仕様 2.3）
	 * 括弧の外の改行が文を終える文法で，節点を含む外側の括弧の内側か，展開時に継続用の包装を選ぶ式の内側かを判定する（節点の中の境界の位置に依らない）
	 * 計算量：祖先の数 D と各祖先の節点より前の子の数の和 C に対し O(D + C)
	 * @param Syntax 構文モデル
	 * @param Id 節点
	 * @return 節点の中の括弧の外の境界で改行しても文が続くなら true
	 */
	bool Canonicalizer::IsLineJoinedOutside(const SyntaxModel &Syntax, const NodeId Id) {
		for(NodeId Child = Id, Parent = Syntax.ParentOf(Id);; Child = Parent, Parent = Syntax.ParentOf(Parent)) {
			// 展開時に継続用の包装を選ぶ式の内側の返戻
			if(NeedsContinuation(Syntax, Child)) return true;
			// 括弧の外の儘文の並びに達した事の返戻
			if(Parent == NoNode || Syntax.Is(Parent, Role::IndentedBody) || Syntax.ParentOf(Parent) == NoNode) return false;
			// 節点より前に開いて閉じて居ない外側の括弧の深さ
			int Depth = 0;
			const std::span<const NodeId> Children = Syntax.ChildrenOf(Parent);
			for(uint32_t At = 0; At < Syntax.GetNode(Child).Index && At < Children.size(); ++At) {
				if(!Syntax.IsNamed(Children[At])) Depth += Syntax.BracketLeadDelta(Children[At]);
			}
			// 外側の括弧の内側の返戻
			if(Depth > 0) return true;
		}
	}

	/**
	 * 子の間の選択可能な境界の判定関数（行分割仕様 2.1~2.3）
	 * 字句の折返の側で字句の前後の境界を候補にし，意味を保って折れない境界を除く
	 * @param Syntax 構文モデル
	 * @param Id 節点
	 * @return 子の番号毎の其の子の前の境界が候補か（子の数 + 1 の並び）
	 */
	std::vector<uint8_t> Canonicalizer::BreakBoundaries(const SyntaxModel &Syntax, const NodeId Id) {
		const std::span<const NodeId> Children = Syntax.ChildrenOf(Id);
		std::vector<uint8_t> Result(Children.size() + 1, 0);
		// 物理行が必須又は意味を保つ為に内部で折らない節点と，名前付の子だけを折る節点の返戻
		if(
			Children.size() < 2 || Syntax.Is(Id, Role::Error) || Syntax.Is(Id, Role::PreprocessorLine) ||
			Syntax.Is(Id, Role::BreakOpaque) || Syntax.GetNode(Id).Facts.Has(ConstraintKind::NoOptionalBreak) ||
			Syntax.Is(Id, Role::PreprocessorBranch) || !Syntax.Is(Id, Role::CompactRecurse) && Syntax.Is(Id, Role::Compact)
		) return Result;
		// 解析器が読めなかった区画を持つ名前付の子が有れば字句の境界を候補にしない
		bool HasErrorChild = false;
		for(const NodeId Child : Children) {
			HasErrorChild = HasErrorChild || Syntax.IsNamed(Child) && Syntax.GetNode(Child).Facts.Has(ConstraintKind::ContainsError);
		}
		// 区切りの無い隣接する文字列の名前付の子の前（Ruby の単語の配列は先頭の要素の前も）
		if(Syntax.Is(Id, Role::StringSeqContainer)) {
			bool IsFirst = !Syntax.Is(Id, Role::WordArray);
			for(uint32_t Index = 0; Index < Children.size(); ++Index) {
				if(!Syntax.IsNamed(Children[Index])) continue;
				if(!IsFirst) Result[Index] = 1;
				IsFirst = false;
			}
		}
		const bool IsExpressionTag = Syntax.Is(Id, Role::ExpressionTag);
		// 字句の役割に応じた折返候補の収集
		if(!Syntax.Is(Id, Role::UnescapedAttribute) && !HasErrorChild && !Syntax.Is(Id, Role::UnbrokenExceptionBinding)) {
			const SyntaxRules &Rules = Syntax.Rules();
			const bool IsOptionalTypeMember = Syntax.Is(Id, Role::OptionalTypeMember);
			const bool IsOptionalCall = Syntax.Is(Id, Role::OptionalInvocation);
			const bool IsDelimiterBreakExcluded = Syntax.Is(Id, Role::NoDelimiterBreak);
			// 不可分な括弧の構文か
			const bool IsUnsplittableParen =
			Syntax.Is(Id, Role::UnbrokenParenSyntax) || Syntax.Is(Id, Role::UnbrokenImport) || Syntax.Is(Id, Role::UnbrokenBaseReference);
			const bool IsBreakColon = Syntax.Is(Id, Role::BreakColonHost);
			const bool IsAdjacencySensitiveInfix = Syntax.Is(Id, Role::AdjacencySensitiveInfix);
			const bool IsInitHeader = Syntax.Is(Id, Role::InitHeader);
			bool HasSeenNamed = false;
			for(uint32_t Index = 0; Index < Children.size(); ++Index) {
				NodeId Token = Children[Index];
				if(Syntax.IsNamed(Token)) {
					HasSeenNamed = true;
					// 初期化文を持つ見出は，初期化文を終える `;` を見出の群の区切りとして扱う
					if(!IsInitHeader) continue;
					NodeId Last = Token;
					while(Syntax.ChildCount(Last)) Last = Syntax.Child(Last, Syntax.ChildCount(Last) - 1);
					if(Last == Token || !Syntax.IsTokenRole(Last, Role::StatementTerminatorToken)) continue;
					Token = Last;
				}
				const std::string &Text = Syntax.GetNode(Token).Lexeme;
				const char Single = Text.size() == 1 ? Text.front() : '\0';
				if(Syntax.Is(Id, Role::AnnotationUnionHost) && Syntax.IsTokenRole(Token, Role::PipeToken)) {
					bool IsAnnotation = false;
					for(NodeId Parent = Id; Parent != NoNode; Parent = Syntax.ParentOf(Parent)) {
						if(Syntax.Is(Parent, Role::TypeSyntax)) {
							IsAnnotation = true;
							break;
						}
						if(Syntax.Is(Parent, Role::IndentedBody)) break;
					}
					if(IsAnnotation) continue;
				}
				if(
					Syntax.IsTokenRole(Token, Role::AssignmentToken) && !Rules.CanBreakAfterAssignment ||
					Syntax.Is(Id, Role::ContinuationHeader) && (
						Syntax.IsTokenRole(Token, Role::ColonToken) && (
							Syntax.Is(Id, Role::Assignment) || Syntax.Is(Id, Role::AnnotatedParameter) || Syntax.Is(Id, Role::AnnotatedDefaultParameter)
						) || Rules.IsSpelling(Text, Word::In) && Syntax.Is(Id, Role::CountedLoop) ||
						Syntax.IsTokenRole(Token, Role::PipeToken) && (Syntax.Is(Id, Role::UnionType) || Syntax.Is(Syntax.ParentOf(Id), Role::TypeSlot))
					)
				) continue;
				const NodeId Previous = Index ? Children[Index - 1] : NoNode;
				// 型注釈・接頭辞・既定値等の不可分の位置の除外
				if(
					(
						Syntax.IsTokenRole(Token, Role::UnaryPrefixToken) || Syntax.IsTokenRole(Token, Role::AmpersandToken) ||
						Syntax.IsTokenRole(Token, Role::AsteriskToken)
					) && Index + 1 == Children.size() ||
					(IsOptionalTypeMember || IsOptionalCall) && Syntax.IsTokenRole(Token, Role::QuestionToken) ||
					Syntax.Is(Id, Role::UnbrokenOptionalType) ||
					IsAdjacencySensitiveInfix && !Text.empty() && Rules.AttachedOperatorPrefixChars.find(Text.front()) != std::string_view::npos &&
					IsGluedBefore(Syntax, Id, Index) || Syntax.IsTokenRole(Token, Role::AssignmentToken) && (
						Syntax.Is(Id, Role::EqualsTight) || Syntax.Is(Id, Role::DefaultParameter) ||
						(Syntax.Is(Id, Role::TightHeaderEquals) || Syntax.Is(Previous, Role::TightParameterEquals))
					) || Syntax.Is(Id, Role::PipeParamList) && Syntax.IsTokenRole(Token, Role::PipeToken) || Syntax.Is(Id, Role::CaptureList) && (
						Syntax.IsTokenRole(Token, Role::AmpersandToken) || Syntax.IsTokenRole(Token, Role::AssignmentToken) ||
						Syntax.IsTokenRole(Token, Role::AsteriskToken)
					) ||
					Syntax.IsTokenRole(Token, Role::ClosingParenToken) && Previous != NoNode && Syntax.Is(Previous, Role::TightAnonymousForward) &&
					!Syntax.NamedChildCount(Previous)
				) continue;
				// TS の比較式を型引数へ誤読させる `>` の後の折返の除外
				if(
					Syntax.IsTokenRole(Token, Role::GreaterThanToken) && Syntax.Is(Id, Role::GenericLookalikeOperator) && Previous != NoNode &&
					Syntax.IsTokenRole(Syntax.FieldChild(Previous, ChildRole::Operator), Role::LessThanToken)
				) continue;
				// 型注釈・継承・ラベル・節見出・本体の開始等の `:` は演算子でも要素の区切りでもない為に除外
				// Python の型注釈の `:` の後は，注釈全体を継続括弧で包む位置の合図として残す
				if(Syntax.IsTokenRole(Token, Role::ColonToken) && !IsBreakColon && !Syntax.Is(Syntax.NextSibling(Token), Role::TypeSyntax)) {
					continue;
				}
				// 数値の小数点と空の括弧の内側と不可分な括弧の除外
				const char Closer = IsOpenBracketChar(Single) ? MatchingBracketChar(Single) : '\0';
				const char Opener = IsCloseBracketChar(Single) ? MatchingBracketChar(Single) : '\0';
				if(
					Syntax.IsTokenRole(Token, Role::DotToken) && std::isdigit(static_cast<unsigned char>(PrecedingChar(Syntax, Token))) &&
					std::isdigit(static_cast<unsigned char>(FollowingChar(Syntax, Token))) || Closer && FollowingChar(Syntax, Token) == Closer ||
					Opener && PrecedingChar(Syntax, Token) == Opener || IsDelimiterBreakExcluded && (
						Syntax.IsTokenRole(Token, Role::OpeningBracketToken) || Syntax.IsTokenRole(Token, Role::ClosingSquareToken) ||
						Syntax.IsTokenRole(Token, Role::OpeningParenToken) || Syntax.IsTokenRole(Token, Role::ClosingParenToken)
					) || IsUnsplittableParen
				) continue;
				// 字句の折返の側に応じた候補の記録
				if(const BreakSide Side = SplitSide(Syntax, Token, !HasSeenNamed); Side == BreakSide::Tail) Result[Index + 1] = 1;
				else if(Side == BreakSide::Head) Result[Index] = 1;
			}
			// 行頭へ置く字句（メンバの参照）で始まる名前付の子（連鎖の接尾辞）の前
			for(uint32_t Index = 1; Index < Children.size(); ++Index) {
				NodeId Leading = Children[Index];
				if(!Syntax.IsNamed(Leading)) continue;
				while(Syntax.ChildCount(Leading)) Leading = Syntax.Child(Leading, 0);
				if(!Syntax.IsNamed(Leading) && SplitSide(Syntax, Leading, false) == BreakSide::Head) Result[Index] = 1;
			}
		}
		// JSX の開きタグ・自己閉じタグは，タグ名の後の属性と属性の後の閉じ字句の前を候補にする
		if(IsExpressionTag && !HasErrorChild) {
			bool IsTagNamePassed = false, HasAttribute = false;
			for(uint32_t Index = 0; Index < Children.size(); ++Index) {
				const NodeId Child = Children[Index];
				if(!IsTagNamePassed) {
					IsTagNamePassed = Syntax.IsNamed(Child);
					continue;
				}
				// 型引数はタグ名の続きで属性でない
				if(Syntax.Is(Child, Role::GenericArguments)) continue;
				const bool IsNamed = Syntax.IsNamed(Child);
				HasAttribute = HasAttribute || IsNamed;
				if(
					IsNamed ||
					HasAttribute && (Syntax.IsTokenRole(Child, Role::GreaterThanToken) || Syntax.IsTokenRole(Child, Role::SelfClosingTagToken))
				) Result[Index] = 1;
			}
		}
		// 本文の意味を保つ境界だけを開閉タグとの折返候補にする
		if(Syntax.Is(Id, Role::MarkupElement) && Children.size() > 2) {
			const NodeId Content = Children.size() == 3 ? Children[1] : NoNode;
			const std::string Text = Syntax.Is(Content, Role::CollapsibleText) ? Syntax.Text(Content) : std::string();
			const bool HasPlainText =
			!Text.empty() && Text.front() != ' ' && Text.front() != '\t' && Text.back() != ' ' && Text.back() != '\t';
			const bool HasAttributes = AnyChildOf(
				Syntax,
				Children.front(),
				[&](const NodeId Child) {
					return Syntax.Is(Child, Role::MarkupAttribute);
				}
			);
			if(
				!(HasPlainText && HasAttributes) && Text.find('\n') == std::string::npos && Text.find("\xE2\x80\xA8") == std::string::npos &&
				Text.find("\xE2\x80\xA9") == std::string::npos
			) {
				Result[1] = Syntax.FirstChar(Children[1]) != '\t';
				Result[Children.size() - 1] = Syntax.LastChar(Children[Children.size() - 2]) != '\t';
			}
		}
		// 括弧の外の改行が文を終える文法の，行の続かない境界の除外（節点の中の括弧の深さは子を一度走査して積み上げ，外側の判定は節点毎に一度だけ求める）
		if(Syntax.Rules().LineContinuesOnlyInBrackets) {
			int Depth = 0;
			std::optional<bool> IsJoinedOutside;
			for(uint32_t Index = 0; Index < Result.size(); ++Index) {
				if(Index && !Syntax.IsNamed(Children[Index - 1])) Depth += Syntax.BracketLeadDelta(Children[Index - 1]);
				if(!Result[Index] || Depth > 0) continue;
				if(!IsJoinedOutside) IsJoinedOutside = IsLineJoinedOutside(Syntax, Id);
				if(!*IsJoinedOutside) Result[Index] = 0;
			}
		}
		// 括弧の対の開きの後と閉じの前は同じ群の境界の為，一方だけが候補なら何方も候補にしない（`catch(...)` の `...` の後，行分割仕様 2.2）
		if(
			(
				Syntax.IsTokenRole(Children.front(), Role::OpeningParenToken) || Syntax.IsTokenRole(Children.front(), Role::OpeningBracketToken)
			) && (
				Syntax.IsTokenRole(Children.back(), Role::ClosingParenToken) || Syntax.IsTokenRole(Children.back(), Role::ClosingSquareToken)
			) && Result[1] != Result[Children.size() - 1]
		) Result[1] = Result[Children.size() - 1] = 0;
		// 候補の並びの返戻
		return Result;
	}

	/**
	 * 行端で表現を選ぶ本文の空白の要素の追加関数（整形仕様 6.6）
	 * 行の内側では本文の空白の儘とし，改行に接する行端では値の式（タブは文字参照）で表す字句列を選択肢として子に持たせる
	 * @param Syntax 構文モデル
	 * @param Blank 本文の空白の字面
	 * @param IR 構築中の中間表現
	 * @return 本文の空白を字面に持ち，行端の表現を子に持つ要素
	 */
	FormatItemId Canonicalizer::LineEdgeSpace(const SyntaxModel &Syntax, const std::string_view Blank, FormatIR &IR) {
		const SyntaxRules &Rules = Syntax.Rules();
		const std::string AlternativeText = Blank == "\t" ?
		std::string(Rules.LineEdgeTab) :
		std::string(Rules.LineEdgeSpaceOpen).append(Blank).append(Rules.LineEdgeSpaceClose);
		const std::array<FormatItemId, 1> Alternative { IR.Leaf(AlternativeText, FormatItemKind::Token) };
		const FormatItemId Item = IR.Parent(FormatItemKind::LineEdgeSpace, Alternative);
		IR.Items[Item].Text = IR.Keep(Blank);
		// 行端の表現を子に持つ本文の空白の返戻
		return Item;
	}

	/**
	 * 字句の葉の組立関数
	 * @param Syntax 構文モデル
	 * @param Id 字句の葉又は字面を保つ区画の節点
	 * @param IR 構築中の中間表現
	 * @return 節点の錨を兼ねる字句（改行を含む字面は変更不能な値）
	 */
	Canonicalizer::Built Canonicalizer::BuildLeaf(const SyntaxModel &Syntax, const NodeId Id, FormatIR &IR) {
		std::string_view Lexeme = Syntax.GetNode(Id).Lexeme;
		Built Result;
		const bool IsMultiline = Lexeme.find('\n') != std::string::npos;
		Result.Item =
		IR.Leaf(Lexeme, IsMultiline || Syntax.Is(Id, Role::TrailingDataSection) ? FormatItemKind::Verbatim : FormatItemKind::Token);
		// 本文の改行と表示上の空白を字句から分け，配置時に表現を選べる形へ投影
		if(
			const bool IsCollapsibleText = Syntax.Is(Id, Role::CollapsibleText);
			IsCollapsibleText && !Lexeme.empty() && Lexeme.find_first_not_of(' ') == std::string::npos
		) Result.Item = LineEdgeSpace(Syntax, Lexeme, IR);
		else if(IsCollapsibleText && IsMultiline) {
			std::vector<FormatItemId> Parts;
			for(size_t Start = 0; Start <= Lexeme.size();) {
				const size_t End = Lexeme.find('\n', Start);
				Parts.push_back(
					IR.Leaf(std::string_view(Lexeme).substr(Start, End == std::string::npos ? End : End - Start), FormatItemKind::Token)
				);
				if(End == std::string::npos) break;
				Parts.push_back(IR.Leaf({}, FormatItemKind::HardBreak));
				Start = End + 1;
			}
			Result.Item = IR.Parent(FormatItemKind::AnchorMark, Parts);
		} else if(
			IsCollapsibleText && !Lexeme.empty() &&
			(Lexeme.front() == ' ' || Lexeme.front() == '\t' || Lexeme.back() == ' ' || Lexeme.back() == '\t')
		) {
			std::vector<FormatItemId> Parts;
			size_t Begin = 0, End = Lexeme.size();
			if(Lexeme.front() == ' ' || Lexeme.front() == '\t') {
				Parts.push_back(LineEdgeSpace(Syntax, std::string_view(Lexeme).substr(Begin++, 1), IR));
			}
			if(End > Begin && (Lexeme.back() == ' ' || Lexeme.back() == '\t')) --End;
			if(End > Begin) Parts.push_back(IR.Leaf(std::string_view(Lexeme).substr(Begin, End - Begin), FormatItemKind::Token));
			if(End < Lexeme.size()) Parts.push_back(LineEdgeSpace(Syntax, std::string_view(Lexeme).substr(End), IR));
			Result.Item = IR.Parent(FormatItemKind::AnchorMark, Parts);
		}
		// 葉の端と改行及びコード境界の情報の記録
		IR.Items[Result.Item].Node = Id;
		Result.Head = FindEdge(IR, Result.Item, true);
		Result.Tail = FindEdge(IR, Result.Item, false);
		Result.BeforeTail = FindPrecedingEdge(IR, Result.Item);
		Result.BeforeTail.EndsWithNewline = Result.BeforeTail.Item != NoItem &&
		(IR.Items[Result.BeforeTail.Item].Kind == FormatItemKind::HardBreak || Result.BeforeTail.LastChar == '\n');
		if(Result.Head.Item != NoItem) {
			Result.Head.IsWholeToken =
			IR.Items[Result.Head.Item].Kind == FormatItemKind::Token && IR.TextOf(IR.Items[Result.Head.Item]) == Lexeme;
		}
		if(Result.Tail.Item != NoItem) {
			Result.Tail.IsWholeToken =
			IR.Items[Result.Tail.Item].Kind == FormatItemKind::Token && IR.TextOf(IR.Items[Result.Tail.Item]) == Lexeme;
		}
		if(Result.BeforeTail.Item != NoItem) {
			Result.BeforeTail.IsWholeToken =
			IR.Items[Result.BeforeTail.Item].Kind == FormatItemKind::Token && IR.TextOf(IR.Items[Result.BeforeTail.Item]) == Lexeme;
		}
		Result.HasBreak = IsMultiline;
		Result.HasCodeBoundary = Syntax.Is(Id, Role::CodeOpener) || Syntax.Is(Id, Role::CodeBoundary);
		Result.IsCodeOpenOnLine = Result.HasCodeBoundary && std::ranges::any_of(
			Syntax.Rules().CodeOpeningSuffixes,
			[Lexeme](const std::string_view Suffix) -> bool {
				// 開始記号の末尾の一致の返戻
				return EndsWithCodeOpening(Lexeme, Suffix);
			}
		);
		// 字句の葉の返戻
		return Result;
	}

	/**
	 * 選択可能な境界を作らない区画かの判定関数（整形仕様 4.4, 4.6）
	 * 文を並べる容器でない親の中の誤り区画の兄弟は解析器の回復が形を決めた断片の為，本体と文の容器を除いて内部を折らない（誤りを越える構造を折返の根拠にしない）
	 * @param State 構築中の状態（誤り区画を直接の子に持つ節点の表を持つ）
	 * @param Id 節点
	 * @return 原文の字句と間隙を写す誤り区画・Rust のマクロの字句列，埋込式を持つ文字列か，文を並べない親の中の断片の誤り区画の兄弟なら true
	 */
	bool Canonicalizer::IsUnbreakable(const Builder &State, const NodeId Id) {
		const SyntaxModel &Syntax = State.Syntax;
		const NodeId Parent = Syntax.ParentOf(Id);
		// 誤り区画と Rust のマクロの字句列と埋込式を持つ文字列か，本体でない誤り区画の兄弟かの返戻
		return Syntax.Is(Id, Role::Error) || Syntax.Is(Id, Role::PreservedTokenSequence) || AnyChildOf(
			Syntax,
			Id,
			[&Syntax](const NodeId Child) -> bool {
				// 埋込式の子かの返戻
				return Syntax.Is(Child, Role::StringInterpolation);
			}
		) || Parent != NoNode && Parent < State.ErrorParents.size() && State.ErrorParents[Parent] && Parent != Syntax.GetRoot() &&
		!Syntax.Is(Parent, Role::StatementHost) && !Syntax.Is(Parent, Role::DocHostSequence) && !Syntax.HasField(Id, ChildRole::Body) &&
		!Syntax.Is(Id, Role::StatementHost);
	}

	/**
	 * 平坦形の組立関数（整形仕様 6.2，字句間空白仕様）
	 * 子を字句間の空白の規則で繋ぎ，選択可能な境界を節点の群へ所属させる
	 * 計算量：部分木の節点数 V に対し O(V)
	 * @param State 構築中の状態
	 * @param Id 節点
	 * @return 組立済の平坦形
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildFlat(Builder &State, const NodeId Id) {
		const SyntaxModel &Syntax = State.Syntax;
		const SyntaxNode &Node = Syntax.GetNode(Id);
		// 字句の葉と字面を保つ区画の返戻
		if(Node.Children.empty()) co_return BuildLeaf(Syntax, Id, State.IR);
		// 範囲の同じ名前付の唯一の子は其の子の平坦形（錨は此の節点でも包む）
		if(Node.Children.size() == 1 && !Syntax.Is(Id, Role::ContinuationExpression)) {
			Emitter Only(State.IR);
			Only.Append(co_await TakeFlat(State, Node.Children.front()));
			// 唯一の子の平坦形の返戻
			co_return Only.Finish(Id, NoGroup);
		}
		// 行区切りを補う要素の並びの分類
		const bool IsDelimitedMembers = Syntax.Is(Id, Role::DelimitedMemberSequence);
		const bool IsDelimitedStatements = Syntax.Is(Id, Role::DelimitedStatementSequence);
		// 誤り区画と補間を持つ文字列の中は選択可能な境界を作らない（整形仕様 4.4, 4.6）
		uint32_t PendingHeredocs = 0;
		TrackOpenHeredocs(State, Id, PendingHeredocs);
		const bool IsSealed = IsUnbreakable(State, Id) || PendingHeredocs > 0;
		if(IsSealed) ++State.NoBreakDepth;
		const std::vector<uint8_t> Candidates =
		State.NoBreakDepth ? std::vector<uint8_t>(Node.Children.size() + 1, 0) : BreakBoundaries(Syntax, Id);
		const std::vector<uint32_t> Closers = ScopeClosers(Syntax, Id);
		std::vector<uint32_t> OpenScopes;
		Emitter Out(State.IR);
		const bool IsContinuation = !State.NoBreakDepth && NeedsContinuation(Syntax, Id);
		const bool IsContinuedList = IsContinuation && Syntax.Is(Id, Role::BareTuple);
		// 展開時の継続括弧を共有する群の準備
		if(IsContinuedList) {
			Out.IfBroken({ GeneratedItem(Syntax, GeneratedToken::CallOpen, State.IR) });
			Out.OpenScope();
			Out.Soft("");
		}
		NodeId LastChild = NoNode;
		uint32_t SharedTagGroup = NoGroup;
		// 頭に保つ原文の間隙（文法の字句に無い字面，整形仕様 4.4）
		if(const SemanticBoundary *const Head = PreservedAt(Syntax, Id, 0)) Out.Gap(*Head);
		// 子の境界と平坦形の順次配置
		for(uint32_t Index = 0; Index < Node.Children.size(); ++Index) {
			const NodeId Child = Node.Children[Index];
			const bool IsNamed = Syntax.IsNamed(Child);
			// 括弧の対の閉じは，其の前の区切りの前に深さの範囲を閉じる
			if(!OpenScopes.empty() && OpenScopes.back() == Index) {
				OpenScopes.pop_back();
				Out.CloseScope();
			}
			if(!IsNamed) if(const std::string &Token = Syntax.GetNode(Child).Lexeme; Token.empty()) continue;
			// 行幅で波括弧を付ける本体の前の見出は独自の群とし，本体の群より先に評価する（見出を折る前に本体を波括弧で包む）
			if(IsNamed && IsBraceOptionalBody(Syntax, Id, Child)) {
				Built Header = Out.Finish(NoNode, State.NextGroup++);
				Out = Emitter(State.IR);
				Out.Append(std::move(Header));
			}
			Emitter::Mark Before = Out.Save();
			if(LastChild != NoNode) {
				// 候補の境界か（読み飛ばした字句を挟む時は其の間の何れかの境界）
				bool IsCandidate = false;
				for(uint32_t Boundary = Syntax.GetNode(LastChild).Index + 1; Boundary <= Index; ++Boundary) {
					IsCandidate = IsCandidate || Candidates[Boundary];
				}
				// 誤り区画に接する境界等の原文の間隙と，Ruby の文と Go の成員の間の改行を代替する文区切り（手前が右の被演算子を欠いた範囲なら区切りを入れると値が変わる）
				if(const SemanticBoundary *const Preserved = PreservedBefore(Syntax, Child)) {
					AppendPreservedGap(Syntax, LastChild, *Preserved, Out);
				} else if((IsDelimitedStatements || IsDelimitedMembers) && IsNamed && Syntax.IsNamed(LastChild)) {
					if(!EndsWithOpenRange(Syntax, LastChild)) {
						Out.Text(Syntax, GeneratedToken::StatementTerminator);
						Out.Space();
					}
				} else if(Syntax.Is(LastChild, Role::HardLineDirective)) Out.Break();
				else if(
					// 前処理指令の塊の終端指令の行の後と，閉じの後でない節（`else` 等）の前の改行
					Syntax.Is(LastChild, Role::PreprocessorBranch) || Syntax.Is(Child, Role::Preprocessor) ||
					IsNamed && Syntax.Is(Child, Role::ElseLikeClause) && !Syntax.Is(Id, Role::GuardStatement) &&
					!Out.TailIs(Syntax, Role::ClosingBraceToken, GeneratedToken::BlockClose)
				) Out.Break();
				else if(const bool IsSpaced = NeedsSpace(Syntax, Id, LastChild, Child); IsSpaced && !IsCandidate) Out.Space();
				else if(IsCandidate && (IsSpaced || !Syntax.Rules().IsWhitespaceSemantic)) {
					// Go の折り返した並びの閉じの前の末尾のカンマ（群の展開時だけ出し，閉じの内側の空白の有無に依らない）
					if(!IsNamed && Syntax.IsBracketLead(Child, Role::ClosingBracketLead)) {
						if(TrailingCommaForm(Syntax, Id, LastChild, Child, true)) {
							const FormatItemId Comma = GeneratedItem(Syntax, GeneratedToken::ListSeparator, State.IR);
							State.IR.EditDetail(Comma).PunctuationOwner = Id;
							Out.IfBroken({ Comma });
						}
					}
					Out.Soft(IsSpaced ? " " : "");
				}
			// CSS の空白の無い境界は，改行が新しい意味上の空白に為る為に候補にしない（行分割仕様 2.1）
			}
			// 本体の無い Ruby の条件の後は節の境を示す語が要る（`if a end` は構文誤り）
			if(
				!IsNamed && Syntax.IsWord(Child, Word::BlockEnd) && Syntax.Is(Id, Role::EmptyConditionBody) && !HasBodyStatement(Syntax, Id)
			) {
				Out.Text(Syntax, Word::ConditionThen);
				Out.Space();
			}
			if(IsNamed) {
				// 並びの中の前処理指令の条件の塊も指令の行を分ける
				Built Sub =
				Syntax.Is(Child, Role::PreprocessorBranch) ? co_await BuildPreprocBlock(State, Child) : co_await TakeFlat(State, Child);
				// 行端の空白を持つ単一本文は，属性と本文の境界を同じ群で折る
				if(
					!Index && Syntax.Is(Id, Role::MarkupElement) && Node.Children.size() == 3 &&
					Syntax.Is(Node.Children[1], Role::CollapsibleText) && Syntax.NamedChildCount(Child) > 1 &&
					(Syntax.FirstChar(Node.Children[1]) == ' ' || Syntax.LastChar(Node.Children[1]) == ' ')
				) {
					std::vector<FormatItemId> Pending { Sub.Item };
					while(!Pending.empty()) {
						const FormatItem &Item = State.IR.Items[Pending.back()];
						Pending.pop_back();
						if(Item.Kind == FormatItemKind::Group && Item.Node == Child) SharedTagGroup = Item.Group;
						const std::span<const FormatItemId> Items = State.IR.ChildrenOf(Item);
						Pending.insert(Pending.end(), Items.begin(), Items.end());
					}
				}
				// 行幅で波括弧を付ける単一の文の制御本体の群（整形仕様 6.1）
				if(IsBraceOptionalBody(Syntax, Id, Child)) Sub = BuildBraceGroup(State, Id, Child, std::move(Sub));
				if(Sub.Head.Item == NoItem) {
					// 空の子の区切りを取り消し，直前の子を保つ
					Out.Restore(std::move(Before));
					continue;
				}
				AppendLexicalSeparator(Syntax, Child, Sub.Head, Out);
				AdjustErrorZoneScopes(Syntax, Child, Out);
				Out.Append(std::move(Sub));
			} else {
				Built Token = BuildLeaf(Syntax, Child, State.IR);
				AppendLexicalSeparator(Syntax, Child, Token.Head, Out);
				Out.Append(std::move(Token));
			}
			LastChild = Child;
			// 括弧の対の開きの後の深さの範囲
			if(Closers[Index] < Node.Children.size()) {
				Out.OpenScope();
				OpenScopes.push_back(Closers[Index]);
			}
		}
		// 尾に保つ原文の間隙
		if(const SemanticBoundary *const Tail = PreservedAt(Syntax, Id, static_cast<uint32_t>(Node.Children.size()))) Out.Gap(*Tail);
		if(IsSealed) --State.NoBreakDepth;
		// Kotlin の空の for の本体は文法が区切りを節点の外へ置く為，後続の文を本体にしない様に補う
		if(Syntax.Is(Id, Role::EmptyLoopBody) && Syntax.FirstNamedChildWithRole(Id, Role::BodyWrapper) == NoNode) {
			Out.Text(Syntax, GeneratedToken::StatementTerminator);
		}
		// 群を振った平坦形の返戻
		if(IsContinuedList) {
			Out.CloseScope();
			Out.Soft("");
			Out.IfBroken({ GeneratedItem(Syntax, GeneratedToken::CallClose, State.IR) });
			Built Wrapped = Out.Finish(NoNode, State.NextGroup++);
			State.IR.Items[Wrapped.Item].Node = Id;
			State.IR.Items[Wrapped.Item].Operation = FormOperation::Continuation;
			Wrapped.Item = Anchored(Id, Wrapped.Item, State.IR);
			// 継続括弧と要素の改行を同じ群へ束ねた並びの返戻
			co_return Wrapped;
		}
		if(IsContinuation) {
			Built Content = Out.Finish(NoNode, State.NextGroup++);
			Emitter WrappedOut(State.IR);
			WrappedOut.IfBroken({ GeneratedItem(Syntax, GeneratedToken::CallOpen, State.IR) });
			WrappedOut.OpenScope();
			WrappedOut.Soft("");
			WrappedOut.Append(std::move(Content));
			WrappedOut.CloseScope();
			WrappedOut.Soft("");
			WrappedOut.IfBroken({ GeneratedItem(Syntax, GeneratedToken::CallClose, State.IR) });
			Built Wrapped = WrappedOut.Finish(NoNode, State.NextGroup++);
			State.IR.Items[Wrapped.Item].Node = Id;
			State.IR.Items[Wrapped.Item].Operation = FormOperation::Continuation;
			Wrapped.Item = Anchored(Id, Wrapped.Item, State.IR);
			// 選択可能な包装を持つ式の返戻
			co_return Wrapped;
		}
		// 包装を補わない平坦形の返戻
		co_return Out.Finish(Id, SharedTagGroup == NoGroup ? State.NextGroup++ : SharedTagGroup);
	}

	/**
	 * 平坦形の控えの取得関数
	 * 折らない文脈の平坦形は選択可能な境界を持たない為，通常の文脈の控えと分けて控える
	 * @param State 構築中の状態
	 * @param Id 節点
	 * @return 控えた平坦形
	 */
	Canonicalizer::Pending<const Canonicalizer::Built *> Canonicalizer::GetFlat(Builder &State, const NodeId Id) {
		std::unordered_map<NodeId, Built> &Cache = State.NoBreakDepth ? State.SealedFlat : State.Flat;
		if(const std::unordered_map<NodeId, Built>::const_iterator Found = Cache.find(Id); Found != Cache.end()) {
			// 控えた平坦形の返戻
			co_return &Found->second;
		}
		Built Result = co_await BuildFlat(State, Id);
		// 控えへ入れた平坦形の返戻
		co_return &Cache.emplace(Id, std::move(Result)).first->second;
	}

	/**
	 * 平坦形の控えの取出関数
	 * 親の平坦形へ組み込む子の平坦形は控えから除き，部分木毎の写しを残さない（整形仕様 6.1, 9.2）
	 * @param State 構築中の状態
	 * @param Id 節点
	 * @return 平坦形（控えに無ければ新たに組み立てた物）
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::TakeFlat(Builder &State, const NodeId Id) {
		std::unordered_map<NodeId, Built> &Cache = State.NoBreakDepth ? State.SealedFlat : State.Flat;
		if(const std::unordered_map<NodeId, Built>::iterator Found = Cache.find(Id); Found != Cache.end()) {
			Built Result = std::move(Found->second);
			Cache.erase(Found);
			// 控えから取り出した平坦形の返戻
			co_return Result;
		}
		// 新たに組み立てた平坦形の返戻
		co_return co_await BuildFlat(State, Id);
	}

	/**
	 * 内部を折らない平坦形の組立関数
	 * 行末が構文を終える前処理指令と，ヒアドキュメントの本文を控えた開始行の続き（行分割仕様 2.3）は，１行の儘で選択可能な境界を作らない
	 * @param State 構築中の状態
	 * @param Id 節点
	 * @return 選択可能な境界を持たない平坦形
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildSealedFlat(Builder &State, const NodeId Id) {
		++State.NoBreakDepth;
		Built Line(*co_await GetFlat(State, Id));
		--State.NoBreakDepth;
		// 内部を折らない平坦形の返戻
		co_return Line;
	}
} // namespace Core
