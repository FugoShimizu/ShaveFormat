#include "Canonicalizer.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

namespace Core {
	/**
	 * 開いた範囲演算子で終わるかの判定関数
	 * @param Syntax 構文モデル
	 * @param Id 判定する節点
	 * @return 最後の字句が範囲演算子 `..` / `...` なら true
	 */
	bool Canonicalizer::EndsWithOpenRange(const SyntaxModel &Syntax, NodeId Id) {
		// 末尾の子を辿った最後の字句
		while(Id != NoNode && Syntax.ChildCount(Id)) Id = Syntax.Child(Id, Syntax.ChildCount(Id) - 1);
		// 最後の字句が範囲演算子かの返戻
		return Id != NoNode && Syntax.IsTokenRole(Id, Role::RangeOperator);
	}

	/**
	 * `(` で始まる型かの判定関数
	 * @param Syntax 構文モデル
	 * @param Id 判定する節点
	 * @return 先頭の `(` が型の括弧（組型・関数型・DNF 型等）なら true
	 */
	bool Canonicalizer::StartsWithParenType(const SyntaxModel &Syntax, NodeId Id) {
		// 先頭の子を辿り，`(` を開く型に行着くかを見る（配列型 `(int, int)[]` 等は先頭の子の組型が `(` を開く）
		// `(` を開く型に行着いた場合の返戻
		for(; Id != NoNode && Syntax.IsNamed(Id); Id = Syntax.Child(Id, 0)) if(Syntax.Is(Id, Role::ParenLeadType)) return true;
		// 無名の字句に行着いた場合の返戻
		return false;
	}

	/**
	 * 子の前の境界が原文で密着して居たかの判定関数（整形仕様 4.2，投影が記録した構文事実を読む）
	 * @param Syntax 構文モデル
	 * @param Parent 親
	 * @param Index 境界の後に続く子の番号
	 * @return 密着と記録した境界なら true
	 */
	bool Canonicalizer::IsGluedBefore(const SyntaxModel &Syntax, const NodeId Parent, const uint32_t Index) {
		const SemanticBoundary *const Boundary = Syntax.GetNode(Parent).Facts.BoundaryBefore(Index);
		// 密着の記録の返戻
		return Boundary && Boundary->Kind == BoundaryKind::Glued;
	}

	/**
	 * 子の番号の前の保持する境界の取得関数（整形仕様 4.4, 4.6）
	 * @param Syntax 構文モデル
	 * @param Parent 親
	 * @param Index 境界の後の子の番号（０は最初の子の前，子の数は最後の子の後）
	 * @return 投影が原文の間隙を保つと記録した境界（無ければ nullptr）
	 */
	const SemanticBoundary *Canonicalizer::PreservedAt(const SyntaxModel &Syntax, const NodeId Parent, const uint32_t Index) {
		const SemanticBoundary *const Boundary = Parent == NoNode ? nullptr : Syntax.GetNode(Parent).Facts.BoundaryBefore(Index);
		// 保持する境界の返戻
		return Boundary && Boundary->Kind == BoundaryKind::Preserved ? Boundary : nullptr;
	}

	/**
	 * 子の前の保持する境界の取得関数（整形仕様 4.6）
	 * @param Syntax 構文モデル
	 * @param Child 子
	 * @return 投影が原文の間隙を保つと記録した境界（無ければ nullptr）
	 */
	const SemanticBoundary *Canonicalizer::PreservedBefore(const SyntaxModel &Syntax, const NodeId Child) {
		// 親の中の子の前の保持する境界の返戻
		return PreservedAt(Syntax, Syntax.ParentOf(Child), Syntax.GetNode(Child).Index);
	}

	/**
	 * 保持する境界の追加関数（整形仕様 4.6）
	 * 行の途中の境界では，構造化で落ちる前の子の末尾の `;` を戻して原文の間隙を写す
	 * @param Syntax 構文モデル
	 * @param Previous 境界の前の子
	 * @param Boundary 保持する境界
	 * @param Out 組立中の出力
	 */
	void Canonicalizer::AppendPreservedGap(
		const SyntaxModel &Syntax,
		const NodeId Previous,
		const SemanticBoundary &Boundary,
		Emitter &Out
	) {
		// マクロ内部の間隙は空白だけでも逐語の断片として保持する
		if(Boundary.Source == PreservedSource::MacroTokens) {
			Out.Gap(Boundary);
			// 逐語の間隙を写した場合の終了
			return;
		}
		// 行頭と空白の後は間隙を足さない事の終了
		if(Out.IsEmpty() || Out.IsAtLineStart() || Out.EndsWithBlank()) return;
		if(
			Previous != NoNode && Syntax.LastTokenRole(Previous, Role::StatementTerminatorTail) &&
			!Out.TailIs(Syntax, Role::StatementTerminatorToken, GeneratedToken::StatementTerminator)
		) Out.Text(Syntax, GeneratedToken::StatementTerminator);
		Out.Gap(Boundary);
		// 終了
		return;
	}

	/**
	 * 文の並びの誤り区画に接する保持する境界の取得関数（整形仕様 4.6）
	 * 文を行毎に置く並びでは，誤り区画に接する境界だけ原文の間隙を保ち，前処理指令の境界は条件の塊を保つ為に改行する
	 * @param Syntax 構文モデル
	 * @param Previous 前の文
	 * @param Child 後の文
	 * @return 保持する境界（無ければ nullptr）
	 */
	const SemanticBoundary *Canonicalizer::ErrorGapBefore(const SyntaxModel &Syntax, const NodeId Previous, const NodeId Child) {
		// 間の無名の字句（文の区切りの `;` 等）を越えた前の兄弟
		NodeId Adjacent = Syntax.PrevSibling(Child);
		while(Adjacent != Previous && Adjacent != NoNode && !Syntax.IsNamed(Adjacent)) Adjacent = Syntax.PrevSibling(Adjacent);
		// 隣り合う文でないか，誤り区画に接しない境界の返戻
		if(Previous == NoNode || Adjacent != Previous || !Syntax.Is(Previous, Role::Error) && !Syntax.Is(Child, Role::Error)) {
			// 隣接する誤り区画が無い事の返戻
			return nullptr;
		}
		// 前処理指令の境界の返戻
		if(
			Syntax.Rules().HasLineDirectives && (
				Syntax.Is(Previous, Role::Preprocessor) || Syntax.Is(Child, Role::Preprocessor) ||
				Syntax.GetNode(Child).Facts.Has(ConstraintKind::DirectiveZone) ||
				Syntax.GetNode(Previous).Facts.Has(ConstraintKind::DirectiveZone)
			)
		) return nullptr;
		// 投影が記録した保持する境界の返戻
		return PreservedBefore(Syntax, Child);
	}

	/**
	 * 端の字句が密着して融合するかの判定関数
	 * @param Syntax 構文モデル
	 * @param IR 中間表現
	 * @param Before 前の字句の末尾の要素
	 * @param After 後の字句の先頭の要素
	 * @return 融合する組なら true
	 */
	bool Canonicalizer::MergesEdgeTokens(
		const SyntaxModel &Syntax,
		const FormatIR &IR,
		const EdgeItem Before,
		const EdgeItem After
	) {
		// 節点の字句又は生成形式だけから，融合判定に必要な端の綴りを得る
		const auto SpellingOf = [&Syntax, &IR](const EdgeItem Edge) -> std::string_view {
			// 端の要素の無い事の返戻
			if(Edge.Item == NoItem) return {};
			const FormatItem &Item = IR.Items[Edge.Item];
			// 生成字句の綴りの返戻
			if(Item.Generated != GeneratedToken::None) return GeneratedSpelling(Syntax, Item.Generated);
			// 元の字句の綴りの返戻
			return Edge.Node != NoNode && Edge.IsWholeToken && Item.Kind == FormatItemKind::Token ?
			std::string_view(Syntax.GetNode(Edge.Node).Lexeme) :
			std::string_view();
		};
		const std::string_view Left = SpellingOf(Before), Right = SpellingOf(After);
		// 字句が揃わない境界の返戻
		if(Left.empty() || Right.empty()) return false;
		// 原文の字句又は生成字句の端が融合するかの返戻
		return Syntax.Rules().FusesDimensionTokens(Left.back(), Right);
	}

	/**
	 * 固定の区切記法の追加関数（整形仕様 6.1）
	 * CSS の区切記法で分けた境界で前後の字句が融合する時は，本文の無いブロックコメントの区切記法で分ける
	 * @param Syntax 構文モデル
	 * @param Child 後の子
	 * @param Head 後の子の組立済の先頭
	 * @param Out 組立中の出力
	 */
	void Canonicalizer::AppendLexicalSeparator(const SyntaxModel &Syntax, const NodeId Child, const EdgeItem Head, Emitter &Out) {
		// 空白が意味を持たない文法と，先頭・親の無い子の場合の終了
		if(!Syntax.Rules().IsWhitespaceSemantic || Out.IsEmpty() || Syntax.ParentOf(Child) == NoNode) return;
		const SemanticBoundary *const Boundary =
		Syntax.GetNode(Syntax.ParentOf(Child)).Facts.BoundaryBefore(Syntax.GetNode(Child).Index);
		if(Boundary && Boundary->Kind == BoundaryKind::Separated && MergesEdgeTokens(Syntax, Out.Store(), Out.Last(), Head)) {
			Out.Separator(Syntax.Rules().LexicalSeparator);
		}
		// 終了
		return;
	}

	/**
	 * 節点の前の字句の最後の字の取得関数
	 * @param Syntax 構文モデル
	 * @param Id 節点
	 * @return 文書の順で前の字句の最後の字（無ければ `\0`）
	 */
	char Canonicalizer::PrecedingChar(const SyntaxModel &Syntax, NodeId Id) {
		while(Id != NoNode) {
			for(NodeId Previous = Syntax.PrevSibling(Id); Previous != NoNode; Previous = Syntax.PrevSibling(Previous)) {
				// 字面を持つ前の兄弟の最後の字の返戻
				if(const char Last = Syntax.LastChar(Previous)) return Last;
			}
			Id = Syntax.ParentOf(Id);
		}
		// 前の字句が無い事の返戻
		return '\0';
	}

	/**
	 * 節点の後の字句の最初の字の取得関数
	 * @param Syntax 構文モデル
	 * @param Id 節点
	 * @return 文書の順で後の字句の最初の字（無ければ `\0`）
	 */
	char Canonicalizer::FollowingChar(const SyntaxModel &Syntax, NodeId Id) {
		while(Id != NoNode) {
			for(NodeId Next = Syntax.NextSibling(Id); Next != NoNode; Next = Syntax.NextSibling(Next)) {
				// 字面を持つ後の兄弟の最初の字の返戻
				if(const char First = Syntax.FirstChar(Next)) return First;
			}
			Id = Syntax.ParentOf(Id);
		}
		// 後の字句が無い事の返戻
		return '\0';
	}

	/**
	 * Ruby の符号を密着させても読みが変わらない被演算子かの判定関数
	 * 数字で始まる被演算子へ符号を密着させると符号付の数値に為り，後置の受け手の範囲が変わる
	 * @param Syntax 構文モデル
	 * @param Operand 被演算子
	 * @return 密着させて良いなら true
	 */
	bool Canonicalizer::AttachesNumericSign(const SyntaxModel &Syntax, const NodeId Operand) {
		// 数字で始まらない被演算子の返戻
		if(Operand == NoNode || !std::isdigit(static_cast<unsigned char>(Syntax.FirstChar(Operand)))) return true;
		const NodeId Unary = Syntax.ParentOf(Operand), Outer = Syntax.ParentOf(Unary);
		// 後置の受け手と読まれた離れた符号の返戻
		if(
			Syntax.Is(Unary, Role::UnaryValue) && !IsGluedBefore(Syntax, Unary, Syntax.GetNode(Operand).Index) &&
			Syntax.Is(Outer, Role::PostfixReceiverChain) && Syntax.NamedChild(Outer, 0) == Unary
		) return false;
		// 符号直下の数値の底
		const NodeId Base =
		Syntax.Is(Operand, Role::Binary) && Syntax.IsTokenRole(Syntax.FieldChild(Operand, ChildRole::Operator), Role::ExponentToken) ?
		Syntax.NamedChild(Operand, 0) :
		Operand;
		// 数値だけか，数値を底とする冪かの返戻
		return Syntax.Is(Base, Role::NumberLiteral) || Syntax.Is(Base, Role::NumericUnitSuffix);
	}

	/**
	 * 兄弟の字句の間の空白の要否の判定関数（字句間空白仕様第３章，第４章）
	 * 前後の子の構文上の役割から密着か空白１個かを決め，原文の空白量は読まない（読みを決める隣接は投影の構文事実から引く）
	 * @param Syntax 構文モデル
	 * @param Parent 親
	 * @param Cur 前の子
	 * @param Next 後の子（前の子の後の出力する子）
	 * @return 間に空白を置くなら true
	 */
	bool Canonicalizer::NeedsSpace(const SyntaxModel &Syntax, const NodeId Parent, const NodeId Cur, const NodeId Next) {
		const uint32_t CurIndex = Syntax.GetNode(Cur).Index, NextIndex = Syntax.GetNode(Next).Index;
		const SemanticBoundary *const Boundary = Syntax.GetNode(Parent).Facts.BoundaryBefore(NextIndex);
		// 投影が記録した境界の種類（子の組毎に一度だけ引き，記録が無ければ以下の何れの判定にも当たらない種類とする）
		const BoundaryKind Recorded = Boundary ? Boundary->Kind : BoundaryKind::Spaced;
		// 区切記法で分けた境界と同一行での空白不要に空白を置かない事の返戻
		if(Recorded == BoundaryKind::Separated || Recorded == BoundaryKind::NoInlineSpace) return false;
		// 投影した必須空白の返戻
		if(Recorded == BoundaryKind::RequiredSpace) return true;
		const bool IsCurNamed = Syntax.IsNamed(Cur), IsNextNamed = Syntax.IsNamed(Next);
		if(Syntax.Is(Parent, Role::CompactInterpolationOperator)) {
			for(NodeId Owner = Syntax.ParentOf(Parent); Owner != NoNode; Owner = Syntax.ParentOf(Owner)) {
				if(Syntax.Is(Owner, Role::Interpolation)) {
					// 補間内では字句の融合を起こさない演算子を密着させる事の返戻
					if(
						Syntax.Is(Parent, Role::Binary) ?
						Syntax.Is(Syntax.FieldChild(Parent, ChildRole::Right), Role::Identifier) :
						Syntax.IsTokenRole(Cur, Role::ColonToken) || Syntax.IsTokenRole(Next, Role::ColonToken)
					) return false;
					break;
				}
			}
		}
		const LexicalEdge PreviousEdge = Syntax.LastEdge(Cur), FollowingEdge = Syntax.FirstEdge(Next);
		const char PrevChar = PreviousEdge.Char, NextChar = FollowingEdge.Char;
		const SyntaxRules &Rules = Syntax.Rules();
		// 成員の参照の記号の字
		const char MemberAccess = Rules.Operators.MemberAccess.front();
		const auto PrevEndsAs = [&Syntax, &PreviousEdge](const Role Wanted) -> bool {
			// 前の子の最後の字句の役割の返戻
			return Syntax.IsEdgeRole(PreviousEdge, Wanted);
		};
		const auto NextStartsAs = [&Syntax, &FollowingEdge](const Role Wanted) -> bool {
			// 後の子の最初の字句の役割の返戻
			return Syntax.IsEdgeRole(FollowingEdge, Wanted);
		};
		// 原文で密着して居た境界か（投影が読みを決める隣接として記録した物）
		const bool IsGlued = Recorded == BoundaryKind::Glued;
		// Swift の後置演算子は原文の隣接を保持
		if(Syntax.Is(Parent, Role::AdjacencySensitiveInfix)) {
			const auto AttachedMark = [&Syntax, &Rules, Parent](const NodeId Operator) -> bool {
				// 左の字へ空白無で接する `?` `!` で始まる字句かの返戻
				return Rules.AttachedOperatorPrefixChars.find(Syntax.FirstChar(Operator)) != std::string_view::npos &&
				IsGluedBefore(Syntax, Parent, Syntax.GetNode(Operator).Index);
			};
			// 原文の隣接の返戻
			if(AttachedMark(Next) || CurIndex && AttachedMark(Cur)) return !IsGlued;
		}
		// C# の分解の宣言 (`var (m, n) = t`) の `var` と組の形は，呼出でない為に空白で区切る事の返戻
		if(Syntax.Is(Cur, Role::DeconstructionType) && NextStartsAs(Role::OpeningParenLead)) return true;
		// JS/TS の await の式を `await` 名への呼出・添字と誤読した形か
		const bool IsMisreadAwait = Syntax.Is(Cur, Role::AwaitCallHead) && Syntax.IsWord(Cur, Word::AwaitName);
		// 誤読した await の式を演算子の語の後として空白で区切る事の返戻
		if(IsMisreadAwait && (NextStartsAs(Role::OpeningParenLead) || NextStartsAs(Role::OpeningSquareLead))) return true;
		// PHP の地の文と PHP の字句の間は空白で区切る (`<?= $x ?>` / `: ?>` / `<?php endforeach`)
		// 地の文の境界の空白要，本体の `:` の密着の返戻
		if(Syntax.Is(Cur, Role::CodeBoundary) || Syntax.Is(Next, Role::CodeBoundary)) return !NextStartsAs(Role::ColonLead);
		// 交替本体のコロンの密着の返戻
		if(Syntax.Is(Next, Role::AlternateBodyLayout) && NextStartsAs(Role::ColonLead)) return false;
		{
			// 型引数の読違（投影が構文事実として記録した物）に対する原文空白へ依存しない出力
			const auto IsMisparsedGenericCall = [&Syntax](const NodeId Node) -> bool {
				// 総称呼出を比較と誤読した二項式かの返戻
				return Node != NoNode && Syntax.GetNode(Node).Facts.Has(ConstraintKind::MisparsedGenericCall);
			};
			const auto IsMisparsedDeclaration = [&Syntax](const NodeId Node) -> bool {
				// 型引数を持つ型の宣言を比較と誤読した二項式かの返戻
				return Node != NoNode && Syntax.GetNode(Node).Facts.Has(ConstraintKind::MisparsedTemplateDeclaration);
			};
			const auto IsMisparsedTemplate = [&Syntax](const NodeId Node) -> bool {
				// 比較を型引数と誤読した山括弧かの返戻
				return Node != NoNode && Syntax.GetNode(Node).Facts.Has(ConstraintKind::MisparsedTemplate);
			};
			// 総称呼出と型の宣言の型引数の字句を密着させる事の返戻（宣言の閉じの後は宣言子と空白で区切る）
			if(
				!IsCurNamed && Syntax.IsTokenRole(Cur, Role::LessThanToken) || !IsNextNamed && Syntax.IsTokenRole(Next, Role::LessThanToken)
			) {
				const NodeId Owner = Syntax.ParentOf(Parent);
				// 誤読した総称呼出又は宣言の型引数を密着させる事の返戻
				if(IsMisparsedGenericCall(Owner) || IsMisparsedDeclaration(Owner)) return false;
			} else if(
				(
					!IsCurNamed && Syntax.IsTokenRole(Cur, Role::GreaterThanToken) ||
					!IsNextNamed && Syntax.IsTokenRole(Next, Role::GreaterThanToken)
				) && IsMisparsedGenericCall(Parent) ||
				!IsNextNamed && Syntax.IsTokenRole(Next, Role::GreaterThanToken) && IsMisparsedDeclaration(Parent)
			// 誤読した総称呼出又は宣言の型引数を密着させる事の返戻
			) return false;
			// 誤解析で割れた比較演算子は元は１字句の為に密着
			if(!IsNextNamed && Rules.MisparsedComparisonLeadChars.find(NextChar) != std::string_view::npos) {
				// 正当な型引数の後だけ空白を許す事の返戻
				if(Syntax.Is(Cur, Role::RecoverableTemplate)) return !IsMisparsedTemplate(Cur);
				NodeId Right = Cur;
				while(Right != NoNode && !Syntax.Is(Right, Role::MisparsedTemplateHost)) {
					const uint32_t Count = Syntax.ChildCount(Right);
					Right = Count ? Syntax.Child(Right, Count - 1) : NoNode;
				}
				// 右端に包んだ誤読の型引数の後の割れた演算子を密着させる事の返戻
				if(IsMisparsedTemplate(Right)) return false;
			}
			// 同じ誤解析の左側は本来の比較演算子で，常に空白を置く
			// 比較演算子を空白で区切り，コンマを前へ密着させる事の返戻
			if(
				(
					Syntax.Is(Parent, Role::MisparsedTemplateHost) && Syntax.Is(Next, Role::AngleBracketList) ||
					Syntax.Is(Parent, Role::AngleBracketList)
				) && IsMisparsedTemplate(Parent) ||
				Syntax.Is(Parent, Role::CallExpression) && Syntax.Is(Cur, Role::GenericFunction) && Syntax.Is(Next, Role::ArgumentSequence) &&
				IsMisparsedTemplate(Cur)
			) return !Syntax.IsTokenRole(Next, Role::CommaToken);
		}
		// 無名の字句の種別が列挙した物かの判定
		const auto CurIs = [&Syntax, IsCurNamed, Cur](const std::initializer_list<Role> Roles) -> bool {
			// 前の字句が無名で何れかの役割を持つかの返戻
			return !IsCurNamed && std::ranges::any_of(
				Roles,
				[&Syntax, Cur](const Role Wanted) -> bool {
					// 役割を持つかの返戻
					return Syntax.IsTokenRole(Cur, Wanted);
				}
			);
		};
		const auto NextIs = [&Syntax, IsNextNamed, Next](const std::initializer_list<Role> Roles) -> bool {
			// 後の字句が無名で何れかの役割を持つかの返戻
			return !IsNextNamed && std::ranges::any_of(
				Roles,
				[&Syntax, Next](const Role Wanted) -> bool {
					// 役割を持つかの返戻
					return Syntax.IsTokenRole(Next, Wanted);
				}
			);
		};
		// 密着させる後の種別（コロン・ドット・三項・任意連鎖等）かの判定
		const auto NextAttachColonLike = [&]() -> bool {
			// TS の後置の任意の印の直前の字句への密着
			if(Syntax.Is(Next, Role::OptionalMarker)) {
				// 省略可能型の前に空白を置く事の返戻
				if(Syntax.Is(Next, Role::OptionalType)) return false;
				const NodeId FirstChild = Syntax.Child(Next, 0);
				// 後置形の任意の印なら true の返戻
				return FirstChild == NoNode || !Syntax.IsTokenRole(FirstChild, Role::QuestionToken);
			}
			// 密着させる後の種別なら true の返戻
			return Syntax.Is(Next, Role::AttachColonOrDot) ||
			Syntax.Is(Next, Role::SpaceSkip) && (NextStartsAs(Role::ColonLead) || NextStartsAs(Role::QuestionLead)) ||
			Syntax.Is(Next, Role::NormColon) && NextStartsAs(Role::ColonLead);
		};
		// Ruby・Python の密着構文
		// 型・名前空間の記号を密着させる事の返戻
		if(
			!IsCurNamed && (
				(
					Syntax.Is(Parent, Role::AttachedHashSplat) || Syntax.Is(Cur, Role::AttachedLowUnaryWord) && NextStartsAs(Role::OpeningParenLead)
				) || Syntax.IsWord(Cur, Word::TypeDeclaration) && Syntax.Is(Parent, Role::TypeAlias) && NextStartsAs(Role::OpeningParenLead)
			) || !PrevChar || !NextChar || Syntax.Is(Cur, Role::Error) || Syntax.Is(Next, Role::Error) ||
			Syntax.Is(Parent, Role::MarkupSyntax) && (
				CurIs(
					{ Role::ClosingTagOpenToken, Role::GreaterThanToken, Role::LessThanToken, Role::SelfClosingTagToken, Role::SlashToken }
				) || NextIs({ Role::GreaterThanToken })
			) || (
				Syntax.Is(Cur, Role::AttachedReferenceModifier) || Syntax.Is(Parent, Role::AttachedNamespaceImport) && (
					CurIs({ Role::BackslashToken, Role::OpeningBraceToken }) ||
					NextIs({ Role::BackslashToken, Role::ClosingBraceToken, Role::OpeningBraceToken })
				)
			)
		) return false;
		// 型を合成する `|` / `&` の前後の密着（値のビット演算・参照の記号と区別し，言語に依らず同じ役割へ同じ規則を当てる）
		// 型を合成する記号を密着させる事の返戻
		if(
			(CurIs({ Role::AmpersandToken, Role::PipeToken }) || NextIs({ Role::AmpersandToken, Role::PipeToken })) &&
			Syntax.Is(Parent, Role::TypeCombination)
		) return false;
		if(Syntax.Is(Parent, Role::AnnotationUnionOperator) && (CurIs({ Role::PipeToken }) || NextIs({ Role::PipeToken }))) {
			// 型注釈の中の合併の演算の連なりと，行分割の継続括弧の外側
			NodeId Outer = Parent;
			while(true) {
				const NodeId Owner = Syntax.ParentOf(Outer);
				if(!Syntax.IsNamed(Owner) || !Syntax.Is(Owner, Role::BinaryOperator) && !Syntax.Is(Owner, Role::ValueParentheses)) break;
				Outer = Owner;
			}
			// 型注釈の合併を密着させる事の返戻
			if(const NodeId Owner = Syntax.ParentOf(Outer); Syntax.IsNamed(Owner) && Syntax.Is(Owner, Role::TypeSlot)) return false;
		}
		// CSS/SCSS の結合子の選択子の被演算子の境界は常に空白で区切られる別要素の為，一律で空白を置く事の返戻
		if(Syntax.Is(Parent, Role::CombinatorSelector)) return true;
		// JSX の子の間で描画される空白は本文として現れる為に密着させる事の返戻
		if(Syntax.Is(Parent, Role::RenderedChildren)) return false;
		// 解析器が単位と演算子を関数名と読んだ呼出（`.5rem*(x)` の `rem*(x)`・`2px-(3px)` の `px-(3px)`）で始まる値かの判定
		const auto StartsWithUnitCall = [&Syntax, Next]() -> bool {
			for(NodeId Head = Next; Head != NoNode; Head = Syntax.NamedChild(Head, 0)) {
				// 左端の呼出の発見の返戻
				if(Syntax.IsNamed(Head) && Syntax.Is(Head, Role::CallExpression)) return true;
			}
			// 呼出で始まらない値の返戻
			return false;
		};
		// CSS/SCSS の単位断片と減算らしい値列，単位と演算子を関数名と読んだ呼出で始まる値は原文の隣接を保持
		if(
			Rules.HasNumericUnitAdjacency && IsGlued &&
			(Syntax.Is(Next, Role::UnitText) || Syntax.Is(Next, Role::DimensionNumber) || StartsWithUnitCall())
		) {
			// 参照型の末尾の節点
			NodeId Tail = Cur;
			while(!Syntax.Is(Tail, Role::DimensionNumber)) {
				const uint32_t Count = Syntax.NamedChildCount(Tail);
				if(!Count) break;
				Tail = Syntax.NamedChild(Tail, Count - 1);
			}
			// 数値と単位の接続を保つ事の返戻
			if(Syntax.Is(Tail, Role::DimensionNumber)) return false;
		}
		// Ruby の単語の配列の要素を空白で区切る事の返戻（記号で始まる要素 `%w(a ,b)` の `,b` も密着すると前の要素と繋がる）
		if(IsCurNamed && IsNextNamed && Syntax.Is(Parent, Role::WordArray)) return true;
		// 空文を直前の字句へ密着させる事の返戻 (`if(c);` / `else;`)
		if(
			IsNextNamed && NextStartsAs(Role::StatementTerminatorLead) &&
			!(Syntax.Is(Next, Role::EnumBodyDeclarations) && CurIs({ Role::OpeningBraceToken }))
		) return false;
		// ラムダの捕捉の `[` と接頭辞 `&` `*`(`*this`) の後，`]` `,` の前は密着させ，其れ以外は空白を置く事の返戻
		if(Syntax.Is(Parent, Role::CaptureList)) {
			// 捕捉の境界の空白の返戻
			return !CurIs({ Role::AmpersandToken, Role::AsteriskToken, Role::OpeningBracketToken }) &&
			!NextIs({ Role::ClosingSquareToken, Role::CommaToken });
		}
		// Kotlin の制御本体・ラベル・注釈と後続式の空白区切り
		// 本体・被修飾式との区切りの返戻
		if(Syntax.Is(Next, Role::SpacedControlBody) || Syntax.Is(Parent, Role::LabeledPrefix) && Syntax.Is(Cur, Role::Label)) {
			// 本体・被修飾式との区切りの返戻
			return true;
		}
		// Kotlin・Swift・CSS・C++・JS/TS・C#・Java の列挙した複合字句は言語別の不可分の字句として密着を保持
		// 言語固有の不可分の字句を密着させる事の返戻
		if(
			Syntax.Is(Parent, Role::AttachedNegationTest) && Syntax.IsTokenRole(Cur, Role::ExclamationToken) &&
			Syntax.Is(Next, Role::NegatableTestWord) || Syntax.Is(Parent, Role::AttachedMagnitudeSuffix) ||
			Syntax.Is(Parent, Role::AttachedNumericUnit) || Syntax.Is(Parent, Role::AttachedRangeOrUnary) ||
			(Syntax.Is(Cur, Role::AttachedPercent) && IsNextNamed || Syntax.Is(Parent, Role::AttachedSelector)) || IsNextNamed && (
				Syntax.Is(Parent, Role::VariadicTypeParameter) && Syntax.IsTokenRole(Cur, Role::SpreadToken) ||
				Syntax.Is(Parent, Role::GeneratorStarHost) && Syntax.IsTokenRole(Cur, Role::AsteriskToken)
			) || Syntax.Is(Parent, Role::NewParenCall) && Syntax.IsWord(Cur, Word::Construct) && NextStartsAs(Role::OpeningParenLead) ||
			Syntax.Is(Parent, Role::FunctionType) &&
			(Syntax.IsTokenRole(Cur, Role::DotToken) || Syntax.IsTokenRole(Next, Role::DotToken)) ||
			Syntax.Is(Parent, Role::SpreadParameter) && Syntax.IsTokenRole(Next, Role::SpreadToken) ||
			Syntax.Is(Parent, Role::PackExpansion) &&
			(Syntax.IsTokenRole(Next, Role::SpreadToken) || Syntax.IsTokenRole(Cur, Role::SpreadToken) && IsNextNamed) ||
			Syntax.Is(Parent, Role::SizeofExpression) &&
			(Syntax.IsTokenRole(Cur, Role::SpreadToken) || Syntax.IsTokenRole(Next, Role::SpreadToken)) ||
			Syntax.Is(Parent, Role::JumpExpression) && Syntax.Is(Next, Role::JumpLabel) ||
			Syntax.Is(Parent, Role::SelfReference) && Syntax.IsWord(Cur, Word::LabeledReceiver) ||
			Syntax.Is(Parent, Role::BaseReference) && (
				!IsCurNamed && (Syntax.IsWord(Cur, Word::BaseReference) || Syntax.IsWord(Cur, Word::LabeledReceiver)) ||
				CurIs({ Role::AtToken, Role::GreaterThanToken, Role::LessThanToken }) ||
				NextIs({ Role::AtToken, Role::GreaterThanToken, Role::LessThanToken })
			) || Syntax.Is(Parent, Role::Indexer) && Syntax.IsWord(Cur, Word::Self) && Syntax.Is(Next, Role::IndexerParameters) ||
			// C# の型を省いた配列の生成 `new[] { 1 }` は `new` と `[` を密着
			// 後置の `!` は前の名前付の要素と後続の型注釈へ密着
			// Rust のループのラベルの `:` は他の言語のラベルと同じくラベルへ密着させる (`'outer: loop`)
			Syntax.Is(Parent, Role::InferredArrayConstruction) && Syntax.IsWord(Cur, Word::Construct) || (
				IsCurNamed && Syntax.Is(Next, Role::PostfixAssertionMark) ||
				Syntax.Is(Cur, Role::PostfixAssertionMark) && Syntax.Is(Next, Role::TypeAnnotation)
			) || Syntax.Is(Parent, Role::RangeAssignment) && (
				NextIs({ Role::AssignmentToken }) && EndsWithOpenRange(Syntax, Cur) ||
				CurIs({ Role::AssignmentToken }) && EndsWithOpenRange(Syntax, Syntax.NamedChild(Parent, 0))
			) || Syntax.Is(Cur, Role::AttachedLabel) && NextIs({ Role::ColonToken })
		) return false;
		// HTML の属性の間と型を始める括弧の前には空白を置く
		// 属性・構文要素を空白で区切る事の返戻
		if(
			Syntax.Is(Parent, Role::SpacedAttributeTag) && Syntax.Is(Next, Role::Attribute) || NextStartsAs(Role::OpeningParenLead) && (
				Syntax.Is(Parent, Role::SingletonMethod) && Syntax.IsWord(Cur, Word::Define) ||
				(IsCurNamed || Rules.IsFusionLetter(PrevChar)) && StartsWithParenType(Syntax, Next)
			)
		) return true;
		if(Syntax.Is(Parent, Role::CastExpr)) {
			// 型変換の語だけを括弧の外側で区切る事の返戻
			return !IsCurNamed && Syntax.IsWord(Cur, Word::Cast) || !IsNextNamed && Syntax.IsWord(Next, Word::Cast);
		}
		// 括弧・引用等の内側を密着させる事の返戻
		if(
			Syntax.Is(Parent, Role::AttributeDeclaration) &&
			(CurIs({ Role::DoubleSquareOpenToken }) || NextIs({ Role::DoubleSquareCloseToken })) ||
			Syntax.Is(Parent, Role::InterpolatedLiteral) || Syntax.Is(Parent, Role::TemplateInterpolation) &&
			(CurIs({ Role::InterpolationOpenToken }) || NextIs({ Role::ClosingBraceToken })) || Syntax.Is(Parent, Role::Interpolation) ||
			Syntax.Is(Parent, Role::InterpolationAlignment) || Syntax.Is(Parent, Role::TypedLambda) && CurIs({ Role::OpeningParenToken }) ||
			Syntax.Is(Parent, Role::BlockArgument) && CurIs({ Role::AmpersandToken }) ||
			Syntax.Is(Parent, Role::ClassSelector) && (Syntax.Is(Cur, Role::NestingSelector) || Syntax.Is(Next, Role::SelectorName)) ||
			Syntax.Is(Parent, Role::IdSelector) ||
			Syntax.Is(Parent, Role::WordArray) && (Syntax.Is(Cur, Role::WordArrayOpen) || Syntax.Is(Next, Role::WordArrayClose)) ||
			Syntax.Is(Parent, Role::AttributeSelector) && !Syntax.Is(Cur, Role::Error) && !Syntax.Is(Next, Role::Error)
		) return false;
		// Kotlin の名前付の演算子と括弧の境界は呼出と誤認しない様に空白を置く事の返戻
		if(Syntax.Is(Parent, Role::NamedInfix)) return true;
		// `{` を密着で開く親（複合リテラル等）は，型の名前・閉じ括弧の何方の後も密着で開く事の返戻
		if(Syntax.Is(Parent, Role::BraceAttach) && NextStartsAs(Role::OpeningBraceLead)) return false;
		if(IsCurNamed && IsNextNamed) {
			// 親・境界別の密着規則と末尾ラムダだけの空白区切り
			// 接続する構文要素を密着させる事の返戻
			if(
				Syntax.Is(Parent, Role::MacroLike) || Syntax.Is(Cur, Role::OptionalChain) ||
				PrevChar == MemberAccess && !PrevEndsAs(Role::SpreadToken) || NextStartsAs(Role::CommaLead) ||
				Syntax.Is(Parent, Role::PrefixUnary) && Syntax.Is(Cur, Role::ExclamationMark) ||
				Syntax.Is(Parent, Role::PostfixExpression) && Syntax.Is(Next, Role::ExclamationMark) || Syntax.Is(Next, Role::AttachBracket) ||
				NextAttachColonLike() ||
				Syntax.Is(Next, Role::CallSuffix) && (NextStartsAs(Role::OpeningParenLead) || NextStartsAs(Role::OpeningAngleLead)) ||
				Syntax.Is(Parent, Role::LeadingTypeArguments) && Syntax.Is(Cur, Role::GenericArguments) ||
				Syntax.Is(Parent, Role::CallExpression) && Syntax.Is(Next, Role::InterpolatedLiteral)
			) return false;
			// Kotlin の型注釈は原文の空白で実引数と関数型を区別
			if(Syntax.Is(Parent, Role::AnnotationConstructor) && Syntax.Is(Next, Role::ValueArguments)) {
				if(const NodeId Annotation = Syntax.ParentOf(Parent); Syntax.Is(Annotation, Role::Annotation)) {
					// 原文の隣接の返戻
					if(Syntax.Is(Syntax.ParentOf(Annotation), Role::TypeModifiers)) return !IsGlued;
				}
			}
			// C# のラムダの仮引数の並びの前は名前でなく修飾・戻値の型・属性の為，空白で区切る事の返戻
			if(Syntax.Is(Parent, Role::TypedLambda)) return true;
			if(Syntax.Is(Next, Role::AttachParen)) {
				const bool IsNextParen = NextStartsAs(Role::OpeningParenLead);
				// Ruby の括弧無の実引数列（名前に密着した `(` は Ruby が実引数の括弧と読み，解析器の読違の為，原文で離れていた物だけ）
				const bool IsBareCallArguments =
				Syntax.Is(Next, Role::BareCallArguments) && IsNextParen && Syntax.IsNamed(Syntax.Child(Next, 0)) && !IsGlued;
				// `(` 直前は密着，但しタプル返戻の `func()(...)` の続き `(` と Ruby の括弧無の実引数列のみ空白有の返戻
				return !IsNextParen || Syntax.Is(Parent, Role::ReturnTuple) && PrevEndsAs(Role::ClosingParenTail) || IsBareCallArguments;
			}
			// 型仮引数の並びの前は修飾子の後だけ空白を置く事の返戻
			if(Syntax.Is(Next, Role::AngleBracketList)) return Syntax.Is(Cur, Role::ModifierSequence);
			if(NextStartsAs(Role::OpeningParenLead) || NextStartsAs(Role::OpeningSquareLead)) {
				// 文区切・コンマ・型・制御本体等の後の括弧前だけ空白を置き，条件に本体の括弧を密着させ，呼出も変えない事の返戻
				return PrevEndsAs(Role::StatementTerminatorTail) || PrevEndsAs(Role::CommaTail) ||
				PrevEndsAs(Role::ClosingParenTail) && (Syntax.Is(Parent, Role::ReturnTuple) || Syntax.Is(Parent, Role::Control)) ||
				NextStartsAs(Role::OpeningParenLead) && Syntax.Is(Next, Role::CompoundDeclarator) || NextStartsAs(Role::OpeningSquareLead) &&
				(Syntax.Is(Next, Role::BracketType) || Syntax.Is(Cur, Role::PlaceholderTypeSpecifier)) || Syntax.Is(Cur, Role::NamedKeyword);
			}
			// 其の他の名前の間へ空白を置く事の返戻
			return true;
		}
		// Python の except*（例外の群の標識）は except と * を密着，* と型の間に空白を置く (except* TypeError)
		if(Syntax.Is(Parent, Role::ExceptionClause)) {
			// 節の語と `*` を密着させる事の返戻
			if(Cur == Syntax.Child(Parent, 0) && Syntax.IsTokenRole(Next, Role::AsteriskToken)) return false;
			// `*` の後に空白を置く事の返戻
			if(Syntax.IsTokenRole(Cur, Role::AsteriskToken)) return true;
		}
		// C# の後置のポインタの記号は型名へ密着
		if(Syntax.Is(Parent, Role::PostfixPointerType) && IsCurNamed && NextIs({ Role::AmpersandToken, Role::AsteriskToken })) {
			// 誤読した乗算かの返戻
			return Syntax.Is(Parent, Role::PointerType) && Syntax.GetNode(Parent).Facts.Has(ConstraintKind::MisreadProduct);
		}
		if(Syntax.Is(Parent, Role::PointerDecl) && !IsCurNamed) {
			// 読違えた展開の原文の隣接の返戻
			if(Syntax.Is(Parent, Role::ArgumentSpread) && !Syntax.Is(Syntax.ParentOf(Parent), Role::ValueArgument)) return !IsGlued;
			// `*` / `&` / `...` の各記号の直後の内，開きの字句の後と名前でない字句の前を密着させる事の返戻
			if(Syntax.Is(Cur, Role::OpenTailToken) || !IsNextNamed) return false;
			// 英数字同士の境界（語 + 識別子）なら空白要の返戻
			return Rules.IsFusionAlnum(PrevChar) && Rules.IsFusionAlnum(NextChar);
		}
		// 前置単項・更新式の子の先頭が英字（語）なら字句の区切で空白要の返戻
		if(Syntax.Is(Parent, Role::UnaryPreOrUpdate) && !IsCurNamed && IsNextNamed) {
			// Ruby の符号を数値で始まる式へ密着させると符号付の数値に為る為，空白で分離する
			// 数値の符号の分離の返戻
			if(
				Syntax.Is(Parent, Role::SignedOperandHost) &&
				(PrevChar == Rules.Operators.Minus.back() || PrevChar == Rules.Operators.Plus.back()) && !AttachesNumericSign(Syntax, Next)
			) return true;
			// 演算子の字句が英字（語）で始まるなら空白要の返戻
			return Rules.IsFusionLetter(Syntax.FirstChar(Cur));
		}
		// 後置の演算子へ密着させる事の返戻
		if(Syntax.Is(Parent, Role::PostfixOrUpdateExpression) && IsCurNamed && !IsNextNamed) return false;
		// PHP の Elvis 演算子 `?:` は一体の記号として密着
		// Elvis 演算子の `?` `:` の間だけを密着させ，其の他の境界へ空白を置く事の返戻
		if(
			Syntax.Is(Parent, Role::ColonPairHost) &&
			(CurIs({ Role::ColonToken, Role::QuestionToken }) || NextIs({ Role::ColonToken, Role::QuestionToken }))
		) return !(CurIs({ Role::QuestionToken }) && NextIs({ Role::ColonToken }));
		// 範囲演算子同士 (Rust `.. ..`) を密着させると `....` が別の字句 `...` `.` と読まれる為，空白で区切る事の返戻
		if(Syntax.Is(Parent, Role::RangeLike) && Rules.FusesRangePair(PrevChar, NextChar)) return true;
		// Swift の二重 Optional 型は型への密着必須
		// 省略可能の記号・範囲演算子を密着させる事の返戻
		if(
			Syntax.Is(Parent, Role::OptionalMarker) && (CurIs({ Role::CoalescingToken }) || NextIs({ Role::CoalescingToken })) ||
			NextIs({ Role::OptionalAccessToken, Role::QuestionToken }) ||
			CurIs({ Role::OptionalAccessToken, Role::QuestionToken }) && !NextIs({ Role::AssignmentToken }) ||
			Syntax.Is(Parent, Role::RangeLike)
		) return false;
		if(Syntax.Is(Parent, Role::InfixOp)) {
			// 解析器が二項式へ誤読した SCSS の -$base と Rust の 0..*a は右被演算子の前置単項として密着を復元
			if(Syntax.Is(Parent, Role::BinaryOperation)) if(const NodeId FirstChild = Syntax.NamedChild(Parent, 0); FirstChild != NoNode) {
				// 幅の無い子（字面を持つ葉の無い部分木，前の子なら求めた端を使い右端から辿る）の後を密着させる事の返戻
				if((FirstChild == Cur ? PreviousEdge : Syntax.LastEdge(FirstChild)).Leaf == NoNode) return false;
				// 終端の無い範囲を密着させる事の返戻
				if(Syntax.Is(FirstChild, Role::OpenRangeOperand)) if(
					const uint32_t RangeChildren = Syntax.ChildCount(FirstChild);
					RangeChildren && Syntax.IsTokenRole(Syntax.Child(FirstChild, RangeChildren - 1), Role::OpenRangeToken)
				) return false;
			}
			// C 形式の型変換を二項式と誤読した時はキャストの記号を密着
			if(Syntax.Is(Parent, Role::CastLookalikeOperator)) {
				const auto IsCastLikeParen = [&Syntax](const NodeId Paren) -> bool {
					// 括弧の式でない事の返戻
					if(!Syntax.Is(Paren, Role::ValueParentheses)) return false;
					// 単一の子でない括弧はキャスト風でない事の返戻
					if(Syntax.NamedChildCount(Paren) != 1) return false;
					// 中身が型名相当の種別かの返戻
					return Syntax.Is(Syntax.NamedChild(Paren, 0), Role::CastTypeAtom);
				};
				// キャストに続く単項演算子を密着させる事の返戻
				if(
					IsCurNamed && NextStartsAs(Role::UnaryPrefixLead) && IsCastLikeParen(Cur) ||
					!IsCurNamed && Syntax.Is(Cur, Role::UnaryPrefixToken) && CurIndex && IsCastLikeParen(Syntax.Child(Parent, CurIndex - 1)) &&
					PrevChar != NextChar
				) return false;
			}
			// 名前付の子の最初と最後の位置
			const uint32_t Count = Syntax.ChildCount(Parent);
			uint32_t FirstNamed = Count, LastNamed = 0;
			for(uint32_t Index = 0; Index < Count; ++Index) {
				if(!Syntax.IsNamed(Syntax.Child(Parent, Index))) continue;
				FirstNamed = std::min(FirstNamed, Index);
				LastNamed = Index;
			}
			// 中置演算子の被演算子の間は空白で区切るが，関数の語の直後の仮引数の並びは演算子の境界でない為に密着
			// 中置演算子を空白で区切る事の返戻
			if(
				(
					!IsCurNamed && CurIndex > FirstNamed && CurIndex < LastNamed && !Syntax.Is(Cur, Role::SeparatorToken) ||
					!IsNextNamed && NextIndex > FirstNamed && NextIndex < LastNamed && !Syntax.Is(Next, Role::SeparatorToken) &&
					!Syntax.IsTokenRole(Next, Role::ClosingParenToken)
				) && !(IsNextNamed && Syntax.Is(Next, Role::ParameterContainer))
			) return true;
		}
		// HTML のタグの開閉の記号の内側は空白を置かないが `/>` の直前は１空白へ揃える事の返戻
		if(Syntax.Is(Parent, Role::TagSyntax)) {
			// タグの境界の空白の返戻
			return !CurIs({ Role::ClosingTagOpenToken, Role::LessThanToken }) && !NextIs({ Role::GreaterThanToken });
		}
		// 密着させると前後の記号と融合して別の字句に為る `=` か（`int>=0`, `x=~y`，前後の両側を同じ判定で区切る）
		const auto FusesEquals = [&Syntax, &Rules](const NodeId Equals) -> bool {
			const char Before = PrecedingChar(Syntax, Equals), After = FollowingChar(Syntax, Equals);
			// 記法の代入の融合条件の返戻
			return Rules.FusesAssignment(Before, After);
		};
		// 呼出側の名前付実引数，キーワードパターン，属性値，指示文の設定値は密着させる
		const bool IsMacroLike = Syntax.Is(Parent, Role::MacroLike), IsCurAssignment = !IsMacroLike && CurIs({ Role::AssignmentToken });
		const bool IsNextAssignment = !IsMacroLike && !IsCurAssignment && NextIs({ Role::AssignmentToken });
		if(
			IsMacroLike ||
			(IsCurAssignment || IsNextAssignment) && Syntax.Is(Parent, Role::EqualsTight) && !FusesEquals(IsCurAssignment ? Cur : Next)
		) {
			// `operator new` / `operator delete` 等の語の演算子の区切りの返戻
			return Syntax.Is(Parent, Role::OperatorName) && PrevChar && NextChar && Rules.IsFusionNameStart(PrevChar) &&
			Rules.IsFusionNameStart(NextChar);
		}
		// C# の終了子の `~` は名前の一部で，構築子の名前と同じく密着させる事の返戻
		if(Syntax.Is(Parent, Role::DestructorDeclaration) && CurIs({ Role::TildeToken })) return false;
		// 代入の `=` の前後と空の値の `:` `;` の間に空白を置く事の返戻
		if(
			(IsCurAssignment || IsNextAssignment) && (
				Syntax.Is(Parent, Role::EqualsSpaced) ||
				(Syntax.Is(Cur, Role::TightParameterEquals) || Syntax.Is(Syntax.PrevSibling(Cur), Role::TightParameterEquals))
			) || Rules.HasSpacedEmptyValue && CurIs({ Role::ColonToken }) && NextIs({ Role::StatementTerminatorToken })
		) return true;
		// 区切の記号を密着させる事の返戻
		if(
			Syntax.Is(Parent, Role::Compact) && (IsCurAssignment || IsNextAssignment) || CurIs({ Role::DoubleColonToken }) ||
			NextIs({ Role::DoubleColonToken }) || Syntax.Is(Parent, Role::ChannelType) && !IsCurNamed && !IsNextNamed ||
			CurIs({ Role::OpeningBracketToken, Role::OpeningParenToken }) ||
			NextIs({ Role::ClosingParenToken, Role::ClosingSquareToken }) || CurIs({ Role::MarkupDeclarationOpenToken }) ||
			Syntax.Is(Parent, Role::BracketedType) && CurIs({ Role::ClosingSquareToken }) ||
			Syntax.Is(Parent, Role::AngleBracketList) && (CurIs({ Role::LessThanToken }) || NextIs({ Role::GreaterThanToken })) ||
			NextIs({ Role::CommaToken, Role::StatementTerminatorToken }) || IsNextNamed && CurIs({ Role::AtToken }) ||
			Syntax.Is(Parent, Role::Parameter) && NextIs({ Role::SpreadToken }) ||
			Syntax.Is(Parent, Role::KeyPath) && CurIs({ Role::BackslashToken }) || CurIs({ Role::AttributeOpenToken, Role::HashToken }) ||
			Syntax.Is(Parent, Role::ReferenceAssignment) && CurIs({ Role::AmpersandToken })
		) return false;
		// `;`・`,`・`:` の直後に後続の括弧又は同じ字句以外が続けば空白を置く事の返戻
		if(CurIs({ Role::ColonToken, Role::CommaToken, Role::StatementTerminatorToken })) {
			// 閉じ括弧・同じ字句以外が続く場合の空白要の返戻
			return !NextStartsAs(Role::ClosingParenLead) && !NextStartsAs(Role::ClosingSquareLead) && !(
				Syntax.IsTokenRole(Cur, Role::ColonToken) ? NextStartsAs(Role::ClosingBraceLead) : NextStartsAs(Role::StatementTerminatorLead)
			) && !(NextStartsAs(Role::PipeLead) && Syntax.Is(Parent, Role::PipeParamList));
		}
		// 前の字句が `{` 又は後の字句が `}` の共通の分岐
		if(
			const bool IsCurOpen = CurIs({ Role::OpeningBraceToken }), IsNextClose = NextIs({ Role::ClosingBraceToken });
			IsCurOpen || IsNextClose
		) {
			// 空白を置かない波括弧の返戻
			if(Syntax.Is(Parent, Role::SpacelessBrace) || IsCurOpen && IsNextClose) return false;
			// `{ ... }` の内側に空白を置く波括弧の返戻
			if(
				Syntax.Is(Parent, Role::ObjectLike) || Syntax.Is(Parent, Role::IndentContainer) || Syntax.Is(Parent, Role::SwitchBodyLayout) ||
				Syntax.Is(Parent, Role::LambdaLiteral)
			) return true;
		}
		// 仮引数を囲む `|` と CSS の名前空間の区切り `|` を密着させる事の返戻
		if(
			(Syntax.Is(Parent, Role::PipeParamList) || Syntax.Is(Parent, Role::AttachedSelectorNamespace)) &&
			(CurIs({ Role::PipeToken }) || NextIs({ Role::PipeToken }))
		) return false;
		if(NextIs({ Role::ColonToken })) {
			// `:` の前は制御文用・中置演算子・通常のコロン・節の先頭の直後以外で空白を置く事の返戻
			return !(
				Syntax.Is(Parent, Role::ColonControl) || Syntax.Is(Parent, Role::InfixOp) || Syntax.Is(Parent, Role::NormColon) ||
				IsCurNamed && Syntax.Is(Cur, Role::SectionHeader)
			);
		}
		// Swift の `case` と名前付の節点に為る語の直後の `.` と組の照合の `(`，十進整数の後の `.` の間に空白を置く事の返戻
		if(
			!IsCurNamed && Syntax.IsWord(Cur, Word::Case) && NextIs({ Role::DotToken }) || IsCurNamed && (
				Syntax.Is(Cur, Role::NamedKeyword) && (NextIs({ Role::DotToken }) || NextStartsAs(Role::OpeningParenLead)) ||
				NextIs({ Role::DotToken }) && Syntax.Is(Cur, Role::IntegerMemberBoundary) && std::ranges::all_of(
					Syntax.Text(Cur),
					[&Rules](const char Char) -> bool {
						// 十進の数字か桁の区切りかの返戻
						return std::isdigit(static_cast<unsigned char>(Char)) || Char == Rules.DigitSeparator;
					}
				)
			)
		) return true;
		// メンバの参照とラムダの仮引数の並びを密着させる事の返戻
		if(
			CurIs({ Role::DotToken, Role::OptionalAccessToken, Role::SafeAccessToken }) ||
			NextIs({ Role::DotToken, Role::OptionalAccessToken, Role::SafeAccessToken }) ||
			IsNextNamed && (NextChar == MemberAccess && Syntax.Is(Parent, Role::Member) || Syntax.Is(Next, Role::OptionalChain)) ||
			IsCurNamed && (PrevChar == MemberAccess && !PrevEndsAs(Role::SpreadToken) || Syntax.Is(Cur, Role::OptionalChain)) ||
			Syntax.Is(Parent, Role::AttachedArrowLambda) && CurIs({ Role::RightArrowToken }) && NextStartsAs(Role::OpeningParenLead)
		) return false;
		// 成員参照でない矢印の演算子の前後へ空白を置く事の返戻
		if(
			CurIs({ Role::FatArrowToken, Role::OptionalArrowToken, Role::RightArrowToken }) ||
			NextIs({ Role::FatArrowToken, Role::OptionalArrowToken, Role::RightArrowToken })
		) return !Syntax.Is(Parent, Role::Member);
		// Go の空の構造体・インタフェースの本体は型名へ密着
		if(Syntax.Is(Parent, Role::BraceNoSpace) && NextStartsAs(Role::OpeningBraceLead)) {
			// 成員を持つ本体かの返戻
			return Syntax.NamedChildCount(Syntax.Is(Next, Role::FieldList) ? Next : Parent);
		}
		// 密着させる親は先に処理済の為，残る `{` の前に空白を置く事の返戻
		if(NextIs({ Role::OpeningBraceToken })) return true;
		// `func()(...)` の組の返戻に続く呼出だけ空白を置く事の返戻
		if(IsCurNamed && NextIs({ Role::OpeningBracketToken, Role::OpeningParenToken })) {
			// 組の返戻に続く括弧の空白の返戻
			return Syntax.IsTokenRole(Next, Role::OpeningParenToken) && Syntax.Is(Parent, Role::ReturnTuple) &&
			PrevEndsAs(Role::ClosingParenTail);
		}
		// C# の型演算子 `typeof` と，全言語の `sizeof` の開始の括弧への密着の返戻
		if(NextStartsAs(Role::OpeningParenLead) && Syntax.Is(Cur, Role::AttachedTypeQuery)) return false;
		// Go の一括宣言の括弧は語から空白で区切る事の返戻 (`const (`)
		if(NextIs({ Role::OpeningParenToken }) && Syntax.Is(Parent, Role::GroupedDeclaration)) return true;
		if(!IsCurNamed && (NextStartsAs(Role::OpeningParenLead) || NextStartsAs(Role::OpeningSquareLead))) {
			const bool IsControlKeyword = Syntax.Is(Cur, Role::ControlKeyword) ||
			Syntax.Is(Parent, Role::ConstantConditionHost) && Syntax.IsWord(Cur, Word::ConstantCondition);
			// 無名の括弧の前は文脈別に空白を決める事の返戻
			return Syntax.IsWord(Cur, Word::FunctionHead) && Syntax.Is(Parent, Role::MethodDeclaration) ||
			Syntax.Is(Parent, Role::ExportDeclaration) ||
			Syntax.Is(Parent, Role::SpacedFunctionHead) && !Syntax.IsWord(Cur, Word::FunctionHead) ||
			IsControlKeyword && (IsNextNamed && Syntax.Is(Next, Role::PatternKind) || Rules.HasBareControlHeader) || (
				IsNextNamed && Syntax.Is(Next, Role::ArgumentSequence) && NextStartsAs(Role::OpeningSquareLead) || !IsControlKeyword && !(
					Syntax.Is(Parent, Role::ValueJump) && IsNextNamed && Syntax.Is(Next, Role::ArgumentSequence) &&
					!Syntax.IsNamed(Syntax.Child(Next, 0))
				) && (
					// `=` / `:=` の右が括弧で始まる形は，密着させると `=(` が別の演算子として読まれる言語が有る為，常に空白を置く
					IsCurAssignment || CurIs({ Role::ShortAssignmentToken }) || Syntax.Is(Cur, Role::SpaceKeyword) ||
					Syntax.Is(Cur, Role::ArrowToken) ||
					!(IsNextNamed && (Syntax.Is(Next, Role::AttachParen) || Syntax.Is(Next, Role::AttachBracket))) && (
						IsNextNamed && Rules.IsFusionLetter(PrevChar) && !Syntax.Is(Parent, Role::BracketedType) ||
						Syntax.IsTokenRole(Cur, Role::ClosingParenToken) && Syntax.Is(Parent, Role::Control)
					)
				)
			);
		}
		if(!IsCurNamed && IsNextNamed) {
			// Swift の演算子の関数名と型仮引数の開始の字句の融合を防ぐ空白区切り
			if(Syntax.Is(Next, Role::AngleBracketList)) {
				// 修飾子・`fun`・演算子の関数の名前の後の空白要の返戻
				return Syntax.Is(Cur, Role::GenericFunctionHead) || Syntax.Is(Parent, Role::OperatorFunctionDeclaration);
			}
			// コロン類を密着させる事の返戻
			if(NextAttachColonLike()) return false;
		}
		// 既定では兄弟の間に空白を置く事の返戻
		return true;
	}
} // namespace Core
