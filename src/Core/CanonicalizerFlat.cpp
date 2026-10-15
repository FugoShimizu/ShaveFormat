#include "Canonicalizer.hpp"
#include "FormatIR.hpp"
#include "NodeId.hpp"
#include "SyntaxModel.hpp"
#include "SyntaxRole.hpp"
#include "../Util/CharClass.hpp"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 単独のトークンを包む部分木の役割の判定関数
	 * 空の葉とトークンを包むラッパーを除いた内容が１つなら，其の投影済の役割を読む
	 * 計算量：O(1)
	 * @param State 子列の要約を共有する構築状態
	 * @param Id 部分木
	 * @param Wanted トークンの役割
	 * @return 単独のトークンが役割を持てば true
	 * @throws FormatLimitExceeded 要約の参照が作業予算を超える場合
	 */
	bool Canonicalizer::HasSingleTokenRole(const Builder &State, const NodeId Id, const Role Wanted) {
		// ノードの無い事の返戻
		if(Id == NoNode) return false;
		State.Syntax.Charge(1);
		// 部分木を再探索せず，確定したトークンを取得する
		const NodeId Token = State.Contexts[Id].SingleToken;
		// 投影済の役割を持つ事の返戻
		return Token != NoNode && State.Syntax.Is(Token, Wanted);
	}

	/**
	 * コードを開く境界の末尾の判定関数
	 * 計算量：開始記号の長さ S に対し O(S)
	 * @param Text 境界のトークン
	 * @param Suffix 文法の定める開始記号
	 * @return 英字の大小を区別せずに末尾が一致すれば true
	 */
	bool Canonicalizer::HasCodeOpeningSuffix(const std::string_view Text, const std::string_view Suffix) {
		// 開始記号より短いトークンと，英字の大小を区別しない末尾の一致の返戻
		return Text.size() >= Suffix.size() && std::equal(
			Suffix.begin(),
			Suffix.end(),
			Text.end() - static_cast<std::ptrdiff_t>(Suffix.size()),
			// 大小を区別しない開始記号の字の比較
			[](const unsigned char Left, const unsigned char Right) -> bool {
				// 小文字へ揃えた一致の返戻
				return std::tolower(Left) == std::tolower(Right);
			}
		);
	}

	/**
	 * 定義が役割を持つかの判定関数
	 * @param Def トークンの綴りの定義（無ければ `nullptr`）
	 * @param Wanted 確認する役割
	 * @return 定義が役割を持てば `true`
	 */
	bool Canonicalizer::IsRole(const KindDef *const Def, const Role Wanted) {
		// 定義が役割を持つかの返戻
		return Def && Def->Roles.test(static_cast<size_t>(Wanted));
	}

	/**
	 * 折返の判定で共有するトークンの定義の取得関数
	 * 共有済の綴りの定義を使い，未照会の葉だけ固定表を引いて控える
	 * 計算量：未照会のトークン長 B に対し期待値 O(B)，共有済なら O(1)
	 * @param State ノード毎の要約を持つ構築状態
	 * @param Id トークンのノード（部分木なら定義無）
	 * @return トークンの綴りの定義（登録が無ければ `nullptr`）
	 */
	const KindDef *Canonicalizer::GetBreakTokenOf(Builder &State, const NodeId Id) {
		// 照会するノードの取得
		const SyntaxNode &Node = State.Syntax.GetNode(Id);
		// 葉でない場合又は種別が無い場合の返戻
		if(!Node.Kind.Def || !Node.Children.empty()) return nullptr;
		// 補間の区切は綴りでなく投影済の開閉の定義を使う
		if(Node.Facts.DelimiterDef) return Node.Facts.DelimiterDef;
		NodeCtx &Ctx = State.Contexts[Id];
		// 共有済の定義の返戻
		if(Ctx.IsSpacingTokenKnown && !Node.Lexeme.empty()) return Ctx.SpacingToken;
		const std::unordered_map<std::string_view, FormPart> &Tokens = State.Syntax.GetRules().TokenDefs;
		const std::unordered_map<std::string_view, FormPart>::const_iterator Found = Tokens.find(Node.Lexeme);
		const KindDef *const Def = Found == Tokens.end() ? nullptr : Found->second.Def;
		// 本文の計上を先取りせず，空白の照会でも使う非空の綴りの定義を控える
		if(!Node.Lexeme.empty()) {
			Ctx.IsSpacingTokenKnown = true;
			Ctx.SpacingToken = Def;
		}
		// 未照会の葉の定義の返戻
		return Def;
	}

	/**
	 * トークンの折返の側の判定関数（行分割仕様 2.2）
	 * 開き・要素の区切・演算子はトークンの後，閉じ・メンバ参照はトークンの前で折る
	 * 計算量：固定表へ照会するトークンの長さ B に対し O(B)
	 * @param Syntax 構文モデル
	 * @param Id トークンのノード
	 * @param Def 実際の綴りの定義（登録が無ければ `nullptr`）
	 * @param IsFirst 親の最初の名前付の子より前のトークンか
	 * @return 折返の側（折らない場合は `BreakSide::None`）
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	Canonicalizer::BreakSide Canonicalizer::SplitSide(
		const SyntaxModel &Syntax,
		const NodeId Id,
		const KindDef *const Def,
		const bool IsFirst
	) {
		// トークンの折返の側の判定
		const SyntaxRules &Rules = Syntax.GetRules();
		// 行末へ置く語の演算子の返戻
		if(!IsFirst && Syntax.GetChildrenOf(Id).empty() && Syntax.Is(Id, Role::TrailingOperatorToken)) return BreakSide::Tail;
		// 行頭へ置く節の語，又はメンバ参照の返戻
		if(
			Syntax.GetChildrenOf(Id).empty() && Syntax.Is(Id, Role::LeadingClauseToken) ||
			Rules.HasHeadMemberArrow && IsRole(Def, Role::MemberAccessArrow)
		) return BreakSide::Head;
		// 自動の文区切を避ける行末の返戻
		if(Rules.HasAutomaticLineTerminator && IsRole(Def, Role::DotToken)) return BreakSide::Tail;
		// 行頭へ置くトークン（行の先頭では折らない）の返戻
		if(IsRole(Def, Role::LeadingBreakToken)) return IsFirst ? BreakSide::None : BreakSide::Head;
		// 何処でも行末に置くトークンの返戻
		if(IsRole(Def, Role::TrailingBreakToken)) return BreakSide::Tail;
		// 演算子を行末に置く事の返戻
		return !IsFirst && IsRole(Def, Role::TrailingOperatorToken) ? BreakSide::Tail : BreakSide::None;
	}

	/**
	 * トークンの葉の組立関数
	 * 計算量：トークンのテキスト長 L に対し O(L) と端の取得の総費用
	 * @param Syntax 構文モデル
	 * @param IR 構築中の中間表現
	 * @param Id トークンの葉又はテキストを保つ領域のノード
	 * @return ノードのアンカーを兼ねるトークン（改行を含むテキストは変更不能な値）
	 * @throws FormatLimitExceeded 要素の生成又は端の探索が作業予算を超える場合
	 * @throws std::bad_alloc トークン又は本文の部品のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	Canonicalizer::Built Canonicalizer::BuildLeaf(const SyntaxModel &Syntax, FormatIR &IR, const NodeId Id) {
		// トークンの葉の組立
		const SyntaxNode &Node = Syntax.GetNode(Id);
		const std::string_view Lexeme = Node.Lexeme;
		Built Result;
		const bool IsCollapsibleText = IsRole(Node.Kind.Def, Role::CollapsibleText);
		const FormatRange Text = IsCollapsibleText ? IR.Keep(Lexeme) : FormatRange();
		std::vector<size_t> LineEnds;
		if(IsCollapsibleText) {
			Syntax.Charge(Lexeme.size());
			for(size_t Idx = 0; Idx < Lexeme.size(); ++Idx) {
				const unsigned char Char = Lexeme[Idx];
				if(Char == '\n') LineEnds.push_back(Idx);
			}
		}
		const bool IsMultiline = !LineEnds.empty();
		// 本文の必須改行だけをトークンから分け，空白は本文の値として保持
		if(IsCollapsibleText && IsMultiline) {
			std::vector<FormatItemId> Parts;
			if(LineEnds.size() > (Parts.max_size() - 1 >> 1)) throw std::length_error("too many line parts");
			Parts.reserve((LineEnds.size() << 1) + 1);
			for(size_t Idx = 0, Start = 0; Idx <= LineEnds.size(); ++Idx) {
				Syntax.Charge(1);
				const bool HasNext = Idx < LineEnds.size();
				const size_t End = HasNext ? LineEnds[Idx] : Lexeme.size();
				if(Start < End) {
					Parts.push_back(
						IR.LeafRange({ Text.Begin + static_cast<uint32_t>(Start), static_cast<uint32_t>(End - Start) }, FormatItemKind::Token, true)
					);
				}
				if(HasNext) {
					Parts.push_back(IR.Leaf({}, FormatItemKind::HardBreak));
					Start = End + 1;
				}
			}
			Result.Item = IR.Parent(FormatItemKind::AnchorMark, Parts);
		} else {
			Result.Item = IsCollapsibleText ?
			IR.LeafRange(Text, FormatItemKind::Token, true) :
			IR.Leaf(Lexeme, IsRole(Node.Kind.Def, Role::TrailingDataSection) ? FormatItemKind::Verbatim : FormatItemKind::Token);
		}
		if(!IsCollapsibleText && IR.Items[Result.Item].HasNewline) IR.Items[Result.Item].Kind = FormatItemKind::Verbatim;
		// 葉の端と改行及びコード境界の情報の記録
		IR.Items[Result.Item].Node = Id;
		const FormatItem &Item = IR.Items[Result.Item];
		const bool HasTextNewline = Node.Facts.HasCodeNewline.value_or(Item.HasNewline);
		const bool IsLiteral = IsRole(Node.Kind.Def, Role::StrLikeAll) || IsRole(Node.Kind.Def, Role::LiteralContent) ||
		IsRole(Node.Kind.Def, Role::LiteralInterior) && !IsRole(Node.Kind.Def, Role::CommentNode);
		const bool HasCodeNewline = HasTextNewline && !IsLiteral;
		if(!IsCollapsibleText && Item.HasNewline && IsLiteral) IR.EditDetail(Result.Item).DoesExcludeCodeNewlines = true;
		if(!IsCollapsibleText && Item.HasNewline && !HasTextNewline) IR.EditDetail(Result.Item).DoesExcludeTextNewlines = true;
		if(Item.Kind == FormatItemKind::Token || Item.Kind == FormatItemKind::Verbatim) {
			// 単独のトークンは本文を比べ直さず，生成時の範囲と改行の事実から端を確定する
			if(!Lexeme.empty()) {
				Result.Head = {
					Result.Item,
					Id,
					static_cast<unsigned char>(Lexeme.front()),
					static_cast<unsigned char>(Lexeme.back()),
					Item.Kind == FormatItemKind::Token,
					Item.Kind == FormatItemKind::Verbatim && Lexeme.back() == '\n',
					Lexeme.back() == ' '
				};
			}
			Result.Tail = Result.Head;
		} else {
			// 本文の行端空白を分けた並びの端は部分トークンで，元のトークン全体ではない
			Result.Head = FindEdge(IR, Result.Item, true);
			Result.Tail = FindEdge(IR, Result.Item, false);
			Result.BeforeTail = FindPrecedingEdge(IR, Result.Item);
			Result.BeforeTail.DoesEndWithNewline = Result.BeforeTail.Item != NoItem &&
			(IR.Items[Result.BeforeTail.Item].Kind == FormatItemKind::HardBreak || Result.BeforeTail.LastChar == '\n');
		}
		Result.Tail.ShouldBreakAfter = IsRole(Node.Kind.Def, Role::DeferredLiteralBody);
		Result.HasBreak = IsCollapsibleText ? IsMultiline : HasCodeNewline;
		Result.HasCodeBoundary = IsRole(Node.Kind.Def, Role::CodeOpener) || IsRole(Node.Kind.Def, Role::CodeBoundary);
		Result.IsCodeOpenOnLine = Result.HasCodeBoundary && std::ranges::any_of(
			Syntax.GetRules().CodeOpeningSuffixes,
			// 埋込コードを開くトークンの末尾記号の照合
			[Lexeme](const std::string_view Suffix) -> bool {
				// 開始記号の末尾の一致の返戻
				return HasCodeOpeningSuffix(Lexeme, Suffix);
			}
		);
		// トークンの葉の返戻
		return Result;
	}

	/**
	 * 選択可能な境界を作らない領域かの判定関数（整形仕様 4.4，4.6）
	 * 文を並べるコンテナでない親の中のエラー領域の兄弟はパーサーの回復が形を決めた断片の為，本体と文のコンテナを除いて内部を折らない（エラーを越える構造を折返の根拠にしない）
	 * 計算量：O(1)
	 * @param State 構築中の状態（エラー領域を直接の子に持つ事実を共有する）
	 * @param Id ノード
	 * @return エラー領域・マクロのトークン列・観測されるテキスト・改行を許さない補間か，文を並べない親の中の断片のエラー領域の兄弟なら true
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	bool Canonicalizer::IsUnbreakable(const Builder &State, const NodeId Id) {
		// 選択可能な境界を作らない領域の判定
		const SyntaxModel &Syntax = State.Syntax;
		const NodeId Parent = Syntax.GetParentOf(Id);
		const SyntaxFacts &Facts = Syntax.GetNode(Id).Facts;
		// テキスト又は構文上の制約で改行出来ない領域か，本体でないエラー領域の兄弟かの返戻
		return Syntax.Is(Id, Role::Error) || Facts.DoesObserveText || Facts.DoesRequireInlineCode ||
		Parent != NoNode && State.Contexts[Parent].HasDirectError && Parent != Syntax.GetRoot() && !Syntax.Is(Parent, Role::StmtHost) &&
		!Syntax.Is(Parent, Role::DocHostSeq) && !State.Contexts[Parent].IsMemberSequence && !Syntax.HasField(Id, ChildRole::Body) &&
		!Syntax.Is(Id, Role::StmtHost);
	}

	/**
	 * 密着する括弧内の改行の可否の判定関数（行分割仕様 2.1，2.2）
	 * 並びの開きの後と閉じの前だけを認め，外側の接頭記号との密着と内側の合法な境界を区別する
	 * 計算量：O(1)
	 * @param Syntax 構文と役割
	 * @param Id 並びのノード
	 * @param Idx 直接の子の前の境界
	 * @return 平坦時の密着を改行へ置き換えられる境界なら true
	 */
	bool Canonicalizer::CanBreakTightBoundary(const SyntaxModel &Syntax, const NodeId Id, const uint32_t Idx) {
		// 空白を許す並び又はコロンの構文でない場合の返戻
		if(!Syntax.Is(Id, Role::BreakableDelimiterInterior) && !Syntax.Is(Id, Role::NormColon)) return false;
		const std::span<const NodeId> Children = Syntax.GetChildrenOf(Id);
		// 並びの内部の開きの後又は閉じの前かの返戻
		return Idx && Idx < Children.size() && (
			Syntax.IsTokenRole(Children[Idx - 1], Role::OpeningParenToken) ||
			Syntax.IsTokenRole(Children[Idx - 1], Role::OpeningBracketToken) ||
			Syntax.IsTokenRole(Children[Idx - 1], Role::OpeningBraceToken) || Syntax.IsTokenRole(Children[Idx], Role::ClosingBraceToken) ||
			Syntax.IsTokenRole(Children[Idx], Role::ClosingParenToken) || Syntax.IsTokenRole(Children[Idx], Role::ClosingSquareToken)
		);
	}

	/**
	 * 子の間の選択可能な境界の判定関数（行分割仕様 2.1~2.3）
	 * トークンの折返の側でトークンの前後の境界を候補にし，意味を保って折れない境界を除く
	 * 計算量：直接の子数 C，辿る末端の列の長さの和 D に対し O(C + D) とテキスト・隣接トークン・補助判定の総費用
	 * @param State 構築中の状態（外側の継続の要約を持つ）
	 * @param Id ノード
	 * @return 子の番号毎の其の子の前の境界が候補か（封止中は空，其れ以外は子の数＋１の並び）
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	std::pmr::vector<uint8_t> Canonicalizer::BreakBoundaries(Builder &State, const NodeId Id) {
		// 封止中は候補の並びを確保しない
		if(State.NoBreakDepth) return std::pmr::vector<uint8_t>(&State.BoundaryMemory);
		// 子の間の選択可能な境界の判定
		const SyntaxModel &Syntax = State.Syntax;
		const std::span<const NodeId> Children = Syntax.GetChildrenOf(Id);
		Syntax.Charge(Children.size() + 1);
		std::pmr::vector<uint8_t> Result(Children.size() + 1, 0, &State.BoundaryMemory);
		// 保護するトークン列は括弧の入子と要素の区切をグループにし，意味を持つ密着を折らない
		if(Syntax.GetNode(Id).Facts.TokenSpacing != TokenSpacingMode::None) {
			const std::span<const SemanticBoundary> Boundaries = Syntax.GetNode(Id).Facts.Boundaries.GetView();
			size_t Cursor = 0;
			for(uint32_t Idx = 1; Idx < Children.size(); ++Idx) {
				Syntax.Charge(1);
				while(Cursor < Boundaries.size() && Boundaries[Cursor].Before < Idx) {
					Syntax.Charge(1);
					++Cursor;
				}
				const SemanticBoundary *const Boundary =
				Cursor < Boundaries.size() && Boundaries[Cursor].Before == Idx ? &Boundaries[Cursor] : nullptr;
				if(Boundary && Boundary->Kind == BoundaryKind::TokenGlued) continue;
				const LexicalEdge Prev = State.Contexts[Children[Idx - 1]].LastEdge, Next = State.Contexts[Children[Idx]].FirstEdge;
				const bool IsOpen = Syntax.IsEdgeCharRole(Prev, Role::OpeningBracketLead);
				const bool IsClose = Syntax.IsEdgeCharRole(Next, Role::ClosingBracketLead);
				Result[Idx] = !(IsOpen && IsClose) &&
				(IsOpen || IsClose || Syntax.IsEdgeCharRole(Prev, Role::CommaTail) || Syntax.IsEdgeCharRole(Prev, Role::StmtTerminatorTail));
			}
			// 括弧と区切の候補の返戻
			return Result;
		}
		// 物理行が必須又は意味を保つ為に内部で折らないノードと，名前付の子だけを折るノードの返戻
		if(
			Children.size() < 2 || Syntax.Is(Id, Role::Error) || Syntax.Is(Id, Role::PreprocLine) || Syntax.Is(Id, Role::BreakOpaque) ||
			Syntax.GetNode(Id).Facts.Has(ConstraintKind::NoOptionalBreak) || Syntax.Is(Id, Role::PreprocBranch) ||
			!Syntax.Is(Id, Role::CompactRecurse) && Syntax.Is(Id, Role::Compact)
		) return Result;
		// JSX の本文と文字参照は同じ本文の並びとして扱い，式・要素との合法な境界だけを折る
		if(Syntax.Is(Id, Role::RenderedChildren)) {
			const NodeId Content = Children.size() == 3 ? Children[1] : NoNode;
			const bool HasPlainText = Syntax.Is(Content, Role::CollapsibleText) && State.Contexts[Content].FirstEdge.Leaf != NoNode &&
			State.Contexts[Content].FirstEdge.Char != ' ' && State.Contexts[Content].FirstEdge.Char != '\t' &&
			State.Contexts[Content].LastEdge.Char != ' ' && State.Contexts[Content].LastEdge.Char != '\t';
			// 属性だけを幅で折る形では本文を同行に保ち，属性値の必須展開とは区別する
			const bool HasInlineAttributes = HasPlainText && Core::SyntaxModel::HasChildMatching(
				Syntax,
				Children.front(),
				// 開タグの属性の子の判定
				[&Syntax](const NodeId Child) -> bool {
					// 属性の子かの返戻
					return Syntax.Is(Child, Role::MarkupAttribute);
				}
			) && !HasExpansionReason(State, Children.front());
			for(uint32_t Idx = 1; Idx < Children.size(); ++Idx) {
				Syntax.Charge(1);
				const SemanticBoundary *const Boundary = Syntax.GetNode(Id).Facts.GetBoundaryBefore(Idx);
				const NodeId Prev = Children[Idx - 1], Next = Children[Idx];
				const bool IsPrevText = Syntax.Is(Prev, Role::RenderedText), IsNextText = Syntax.Is(Next, Role::RenderedText);
				const bool HasUnsafeEdge = IsPrevText && (
					!Syntax.Is(Prev, Role::CollapsibleText) ||
					(State.Contexts[Prev].LastEdge.Char == ' ' || State.Contexts[Prev].LastEdge.Char == '\t')
				) || IsNextText && (
					!Syntax.Is(Next, Role::CollapsibleText) ||
					(State.Contexts[Next].FirstEdge.Char == ' ' || State.Contexts[Next].FirstEdge.Char == '\t')
				);
				Result[Idx] = !(Boundary && (Boundary->Kind == BoundaryKind::Preserved || Boundary->Kind == BoundaryKind::NoBreak)) &&
				!(IsPrevText && IsNextText) && !HasUnsafeEdge && !((Idx == 1 || Idx + 1 == Children.size()) && HasInlineAttributes);
			}
			// 本文の意味を保つ折返候補の返戻
			return Result;
		}
		// メンバの並びは，囲みの内側と各要素の前を同じグループの候補にする
		if(State.Contexts[Id].IsMemberSequence) {
			bool HasMember = false;
			for(uint32_t Idx = 0; Idx < Children.size(); ++Idx) {
				Syntax.Charge(1);
				const NodeId Child = Children[Idx];
				if(
					Idx && (
						Syntax.IsNamed(Child) || Syntax.IsTokenRole(Child, Role::ClosingBraceToken) ||
						Syntax.IsTokenRole(Child, Role::ClosingParenToken) || Syntax.IsTokenRole(Child, Role::ClosingSquareToken)
					)
				) Result[Idx] = 1;
				if(Syntax.IsNamed(Child)) HasMember = true;
			}
			if(!HasMember) std::ranges::fill(Result, 0);
		}
		// パーサーが読めなかった領域を持つ名前付の子が有ればトークンの境界を候補にしない
		const bool HasErrorChild = State.Contexts[Id].HasErrorChild;
		// 区切の無い隣接する文字列の名前付の子の前（Ruby の単語の配列は先頭の要素の前も）
		if(Syntax.Is(Id, Role::StrSeqContainer)) {
			bool IsFirst = !Syntax.Is(Id, Role::WordArray);
			for(uint32_t Idx = 0; Idx < Children.size(); ++Idx) {
				Syntax.Charge(1);
				if(!Syntax.IsNamed(Children[Idx])) continue;
				if(!IsFirst) Result[Idx] = 1;
				IsFirst = false;
			}
		}
		// 宣言の値の開始と値同士の境界を候補にし，密着・保持境界は共通の組立で除く
		if(Syntax.Is(Id, Role::StyleLexeme) && Syntax.Is(Id, Role::TypedDecl)) {
			bool IsInVal = false;
			for(uint32_t Idx = 0; Idx < Children.size(); ++Idx) {
				Syntax.Charge(1);
				if(Syntax.IsTokenRole(Children[Idx], Role::ColonToken)) IsInVal = true;
				else if(Syntax.IsTokenRole(Children[Idx], Role::StmtTerminatorToken)) IsInVal = false;
				else if(IsInVal) Result[Idx] = 1;
			}
		}
		// トークンの役割に応じた折返候補の収集
		if(!Syntax.Is(Id, Role::UnescapedAttribute) && !HasErrorChild && !Syntax.Is(Id, Role::UnbrokenExceptionBinding)) {
			const SyntaxRules &Rules = Syntax.GetRules();
			const bool IsOptionalTypeMember = Syntax.Is(Id, Role::OptionalTypeMember);
			const bool IsOptionalCall = Syntax.Is(Id, Role::OptionalInvocation);
			const bool IsDelimiterBreakExcluded = Syntax.Is(Id, Role::NoDelimiterBreak);
			const bool IsInterpolatedLiteral = Syntax.Is(Id, Role::InterpolatedLiteral);
			// 不可分な括弧の構文か
			const bool IsUnsplittableParen =
			Syntax.Is(Id, Role::UnbrokenParenSyntax) || Syntax.Is(Id, Role::UnbrokenImport) || Syntax.Is(Id, Role::UnbrokenBaseRef);
			const bool IsBreakColon = Syntax.Is(Id, Role::BreakColonHost) ||
			Syntax.Is(Id, Role::NormColon) && Syntax.IsTokenRole(Children.front(), Role::OpeningParenToken) &&
			Syntax.IsTokenRole(Children.back(), Role::ClosingParenToken);
			const bool IsAdjacencySensitiveInfix = Syntax.Is(Id, Role::AdjacencySensitiveInfix);
			const bool IsInitHeader = Syntax.Is(Id, Role::InitHeader), IsInfix = Syntax.Is(Id, Role::InfixOperator);
			bool HasSeenNamed = Syntax.Is(Id, Role::NavigationSuffix); // 接尾部の受手は外側のノードに在る
			for(uint32_t Idx = 0; Idx < Children.size(); ++Idx) {
				Syntax.Charge(1);
				NodeId Token = Children[Idx];
				// 後置連鎖の先頭の折返も，同じ子の訪問で収集する
				if(Idx && Syntax.IsNamed(Token)) {
					const NodeId Leading = State.Contexts[Token].FirstPathLeaf;
					if(!Syntax.IsNamed(Leading)) {
						const KindDef *const Def = GetBreakTokenOf(State, Leading);
						const bool IsFirst = IsRole(Def, Role::ScopeQualifierToken); // 先頭のスコープから始まる名前の外側を，受手に続く参照として折らない
						if(SplitSide(Syntax, Leading, Def, IsFirst) == BreakSide::Head) Result[Idx] = 1;
					}
				}
				const bool IsOperator = IsInfix && Syntax.HasField(Token, ChildRole::Operator) ||
				Syntax.Is(Id, Role::FunctionBody) && Syntax.IsTokenRole(Token, Role::AssignmentToken);
				if(Syntax.IsNamed(Token) && !Syntax.GetNode(Token).Facts.DelimiterDef) {
					HasSeenNamed = true;
					// 演算子の子の役割を優先し，初期化文を終える `;` も見出のグループの区切として扱う
					if(!IsOperator) {
						const NodeId Last = IsInitHeader ? State.Contexts[Token].LastPathLeaf : Token;
						if(Last == Token || !Syntax.IsTokenRole(Last, Role::StmtTerminatorToken)) continue;
						Token = Last;
					}
				}
				const std::string &Text = Syntax.GetNode(Token).Lexeme;
				const KindDef *const Def = GetBreakTokenOf(State, Token);
				const unsigned char Single = Text.size() == 1 ? Text.front() : '\0';
				if(Syntax.Is(Id, Role::AnnotationUnionHost) && IsRole(Def, Role::PipeToken) && State.Contexts[Id].IsAnnotation) continue;
				// 型注釈・接頭辞等の不可分の位置と，TS の比較式を型引数へ誤読させる `>` の後の折返の除外
				// 型注釈・継承・ラベル・節見出・本体の開始等の `:` は演算子でも要素の区切でもない為に除外
				// Python の型注釈の `:` の後は，アノテーション全体を継続括弧で包む位置の合図として残す
				if(
					const NodeId Prev = Idx ? Children[Idx - 1] : NoNode;
					Syntax.Is(Id, Role::ContinuationHeader) && (
						IsRole(Def, Role::ColonToken) &&
						(Syntax.Is(Id, Role::Assignment) || Syntax.Is(Id, Role::AnnotatedParam) || Syntax.Is(Id, Role::AnnotatedDefaultParam)) ||
						IsRole(Def, Role::IterationKeyword) && Syntax.Is(Id, Role::CountedLoop) ||
						IsRole(Def, Role::PipeToken) && (Syntax.Is(Id, Role::UnionType) || Syntax.Is(Syntax.GetParentOf(Id), Role::TypeSlot))
					) || (IsRole(Def, Role::UnaryPrefixToken) || IsRole(Def, Role::AmpersandToken) || IsRole(Def, Role::AsteriskToken)) &&
					Idx + 1 == Children.size() || (IsOptionalTypeMember || IsOptionalCall) && IsRole(Def, Role::QuestionToken) ||
					Syntax.Is(Id, Role::UnbrokenOptionalType) ||
					IsAdjacencySensitiveInfix && !Text.empty() && Rules.AttachedOperatorPrefixChars.find(Text.front()) != std::string_view::npos &&
					IsGluedBefore(Syntax, Id, Idx) || IsRole(Def, Role::AssignmentToken) &&
					(Syntax.Is(Id, Role::EqualsTight) || Syntax.Is(Id, Role::TightHeaderEquals) || Syntax.Is(Prev, Role::TightParamEquals)) ||
					Syntax.Is(Id, Role::PipeParamList) && IsRole(Def, Role::PipeToken) || Syntax.Is(Id, Role::CaptureList) &&
					(IsRole(Def, Role::AmpersandToken) || IsRole(Def, Role::AssignmentToken) || IsRole(Def, Role::AsteriskToken)) ||
					IsRole(Def, Role::ClosingParenToken) && Prev != NoNode && Syntax.Is(Prev, Role::TightAnonymousForward) &&
					!State.Contexts[Prev].NamedCount ||
					IsRole(Def, Role::GreaterThanToken) && Syntax.Is(Id, Role::GenericLookalikeOperator) && Prev != NoNode &&
					Syntax.IsTokenRole(Syntax.GetFieldChild(Prev, ChildRole::Operator), Role::LessThanToken) ||
					IsRole(Def, Role::ColonToken) && !IsBreakColon && !Syntax.Is(Syntax.GetNextSibling(Token), Role::TypeSyntax)
				) continue;
				// 数値の小数点と空の括弧の内側と不可分な括弧の除外
				// リテラル直下の補間を開く `{` は後続の `$` へ密着させ，式の内部と閉じの前は別に評価する
				if(
					const unsigned char Closer = IsOpenBracketChar(Single) ? MatchingBracketChar(Single) : '\0',
					Opener = IsCloseBracketChar(Single) ? MatchingBracketChar(Single) : '\0';
					IsRole(Def, Role::DotToken) && std::isdigit(GetAdjacentChar(State, Token, true)) &&
					std::isdigit(GetAdjacentChar(State, Token, false)) || Closer && GetAdjacentChar(State, Token, false) == Closer ||
					Opener && GetAdjacentChar(State, Token, true) == Opener || IsInterpolatedLiteral && IsRole(Def, Role::OpeningBraceToken) ||
					IsDelimiterBreakExcluded && (
						IsRole(Def, Role::OpeningBracketToken) || IsRole(Def, Role::ClosingSquareToken) || IsRole(Def, Role::OpeningParenToken) ||
						IsRole(Def, Role::ClosingParenToken)
					) || IsUnsplittableParen
				) continue;
				// トークンの折返の側に応じた候補の記録
				if(
					const BreakSide Side = IsOperator ? BreakSide::Tail : SplitSide(Syntax, Token, Def, !HasSeenNamed);
					Side == BreakSide::Tail
				) Result[Idx + 1] = 1;
				else if(Side == BreakSide::Head) Result[Idx] = 1;
			}
		}
		// JSX の開タグ・自己閉タグの，タグ名の後の属性と閉トークンの前の候補の記録
		if(Syntax.Is(Id, Role::ExprTag) && !HasErrorChild) {
			bool IsTagNamePassed = false, HasAttribute = false;
			for(uint32_t Idx = 0; Idx < Children.size(); ++Idx) {
				Syntax.Charge(1);
				const NodeId Child = Children[Idx];
				if(!IsTagNamePassed) {
					IsTagNamePassed = Syntax.IsNamed(Child);
					continue;
				}
				// 型引数はタグ名の続きで属性でない
				if(Syntax.Is(Child, Role::GenericArgs)) continue;
				const bool IsNamed = Syntax.IsNamed(Child);
				HasAttribute = HasAttribute || IsNamed;
				if(
					IsNamed ||
					HasAttribute && (Syntax.IsTokenRole(Child, Role::GreaterThanToken) || Syntax.IsTokenRole(Child, Role::SelfClosingTagToken))
				) Result[Idx] = 1;
			}
		}
		// 意味を保つ為に密着する境界は，文法が与えた条件で候補から除く
		for(const SemanticBoundary &Boundary : Syntax.GetNode(Id).Facts.Boundaries) {
			Syntax.Charge(1);
			if(Boundary.Kind == BoundaryKind::NoBreak && Boundary.Before < Result.size()) Result[Boundary.Before] = 0;
		}
		// 括弧の外の改行が文を終える文法の，行の続かない境界の除外（ノードの中の括弧の深さは子を１度走査して積み上げ，外側の判定はノード毎に１度だけ求める）
		if(Syntax.GetRules().CanLineContinueOnlyInBrackets) {
			int Depth = 0;
			for(uint32_t Idx = 0; Idx < Result.size(); ++Idx) {
				Syntax.Charge(1);
				if(Idx) Depth += Syntax.GetBracketLeadDelta(Children[Idx - 1]);
				if(!Result[Idx] || Depth > 0) continue;
				if(!State.Contexts[Id].IsLineJoined) Result[Idx] = 0;
			}
		}
		// 括弧の対の開きの後と閉じの前は同じグループの境界の為，一方だけが候補なら何方も候補にしない（`catch(...)` の `...` の後，行分割仕様 2.2）
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
	 * 平坦形の組立関数（整形仕様 6.2，トークン間空白仕様）
	 * 子をトークン間の空白の規則で繋ぎ，選択可能な境界をノードのグループへ所属させる
	 * 計算量：部分木のノード数 V に対し O(V)
	 * @param State 構築中の状態
	 * @param Id ノード
	 * @return 組立済の平坦形
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildFlat(Builder &State, const NodeId Id) {
		// 通常の平坦な子は同じ作業列で組み立て，別の工程を待つ場合だけ中断する
		struct FlatFrame {
			Emitter Output; // 此のノードの組立
			std::pmr::vector<uint8_t> Candidates; // 子の間の改行候補
			std::pmr::vector<uint32_t> OpenScopes; // 閉じる子の位置（完了したフレームの領域を再使用する）
			NodeId Id; // 対象のノード
			const SyntaxNode *Node; // 同じノードを子の返戻毎に照会し直さない為の参照
			NodeId Child = NoNode; // 組立の完了を待つ子
			bool IsNamedChild = false; // 待つ子が名前付か
			uint32_t Next = 0; // 次に組み立てる子
			size_t BoundaryCursor = 0; // 前進して読む意味境界の位置
			NodeId LastChild = NoNode; // 直前に出した子
			uint32_t ClosingDependency = NoGroup; // 密着する開きに続く閉じが依存する内側の閉じのグループ
			bool HasFixedOpening = false; // 開きの直後を改行出来ない境界を持つか
			bool IsSingle = false; // アンカーだけで包む唯一の子か
			bool IsDelimitedMembers = false; // メンバを区切るか
			bool IsDelimitedStmts = false; // 文を区切るか
			bool IsDoLoop = false; // 後続の条件を持つループか
			bool IsEmptyCondBody = false; // 空の条件本体か
			bool HasDirectControlBrace = false; // 見出と波括弧を直接持つか
			bool IsSealed = false; // 改行候補を禁じるか
			bool IsContinuation = false; // 継続括弧を選ぶか
			bool IsContinuedList = false; // 並びと継続括弧のグループを共有するか
		};
		const SyntaxModel &Syntax = State.Syntax;
		// 作業列も解放済の領域を再使用し，通常のノード毎のフレーム確保を行わない
		std::pmr::vector<FlatFrame> Frames(&State.BoundaryMemory);
		NodeId NextNode = Id;
		std::optional<Built> Completed;
		while(true) {
			if(NextNode != NoNode) {
				const NodeId Current = std::exchange(NextNode, NoNode);
				const SyntaxNode &Node = Syntax.GetNode(Current);
				if(!State.NoBreakDepth && State.Contexts[Current].Structured) Completed = ReleaseStructured(State, Current);
				else if(Node.Children.empty() && (!State.Contexts[Current].ShouldWrap || State.NoBreakDepth)) {
					Completed = BuildLeaf(Syntax, State.IR, Current);
				} else {
					const bool IsUnbroken = IsUnbreakable(State, Current), UsesKeyword = State.NoBreakDepth && !IsUnbroken;
					if(UsesKeyword && IsRole(Node.Kind.Def, Role::KeywordSection) && !IsRole(Node.Kind.Def, Role::StmtSeq)) {
						Completed = co_await BuildKeywordSection(State, Current, false, false);
					} else if(
						UsesKeyword && IsRole(Node.Kind.Def, Role::KeywordBlock) &&
						!(IsRole(Node.Kind.Def, Role::TailValFunction) && !State.Contexts[Current].HasKeywordClose)
					) Completed = co_await BuildKeywordBlock(State, Current);
					else {
						Frames.push_back(
							{
								Emitter(State.IR),
								std::pmr::vector<uint8_t>(&State.BoundaryMemory),
								std::pmr::vector<uint32_t>(&State.BoundaryMemory),
								Current,
								&Node
							}
						);
						FlatFrame &Frame = Frames.back();
						uint32_t PendingHeredocs = 0;
						TrackOpenHeredocs(State, Current, PendingHeredocs);
						Frame.IsSealed = IsUnbroken || PendingHeredocs;
						Frame.IsSingle = Node.Children.size() == 1 && !IsRole(Node.Kind.Def, Role::ContinuationExpr) &&
						!IsRole(Node.Kind.Def, Role::DeferredLiteralBody) && !Frame.IsSealed;
						if(Frame.IsSingle) {
							NextNode = Node.Children.front();
							continue;
						}
						// 行区切を補う要素の並びの分類
						Frame.IsDelimitedMembers = Node.Facts.TokenSpacing == TokenSpacingMode::None && State.Contexts[Current].IsMemberSequence &&
						IsRole(Node.Kind.Def, Role::DelimitedMemberSeq);
						Frame.IsDelimitedStmts = Node.Facts.TokenSpacing == TokenSpacingMode::None && IsRole(Node.Kind.Def, Role::DelimitedStmtSeq);
						Frame.IsDoLoop = Node.Facts.TokenSpacing == TokenSpacingMode::None && IsRole(Node.Kind.Def, Role::DoLoop);
						Frame.IsEmptyCondBody = Node.Facts.TokenSpacing == TokenSpacingMode::None && IsRole(Node.Kind.Def, Role::EmptyCondBody);
						// 子を組み立てても変わらない親の役割は，境界毎に照会せず１度だけ取得する
						Frame.HasDirectControlBrace =
						Node.Facts.TokenSpacing == TokenSpacingMode::None && IsRole(Node.Kind.Def, Role::DirectControlBraceLayout);
						// 封止の必要なコード領域だけで候補を止め，補間を囲むリテラルの本文は保存境界で保つ
						Frame.IsContinuation = !State.NoBreakDepth && State.Contexts[Current].ShouldWrap;
						if(Frame.IsSealed) ++State.NoBreakDepth;
						Frame.Candidates = BreakBoundaries(State, Current);
						for(const SemanticBoundary &Boundary : Node.Facts.Boundaries) {
							Syntax.Charge(1);
							if(
								Boundary.Kind == BoundaryKind::NoBreak && Boundary.Before && Boundary.Before < Node.Children.size() &&
								Syntax.IsBracketLead(Node.Children[Boundary.Before - 1], Role::OpeningBracketLead)
							) Frame.HasFixedOpening = true;
						}
						if(Node.Children.empty()) Frame.Output.Append(BuildLeaf(Syntax, State.IR, Current));
						Frame.IsContinuedList = Frame.IsContinuation && IsRole(Node.Kind.Def, Role::BareTuple);
						// 展開時の継続括弧を共有するグループの準備
						if(Frame.IsContinuedList) {
							Frame.Output.IfBroken({ GeneratedItem(Syntax, State.IR, GeneratedToken::CallOpen) });
							Frame.Output.OpenScope();
							Frame.Output.Soft("");
						}
						// 頭に保つ原文の間隙（文法のトークンに無いテキスト，整形仕様 4.4）
						if(const SemanticBoundary *const Head = GetPreservedAt(Syntax, Current, 0)) Frame.Output.Gap(*Head);
					}
				}
			}
			// 根の結果を受け取った場合の返戻
			if(Frames.empty()) co_return std::move(*Completed);
			FlatFrame &Frame = Frames.back();
			const NodeId Owner = Frame.Id;
			const SyntaxNode &Node = *Frame.Node;
			Emitter &Output = Frame.Output;
			if(Frame.IsSingle) {
				Output.Append(std::move(*Completed));
				Completed = Output.Finish(Owner, NoGroup);
				Frames.pop_back();
				continue;
			}
			const bool IsDelimitedMembers = Frame.IsDelimitedMembers, IsDelimitedStmts = Frame.IsDelimitedStmts, IsDoLoop = Frame.IsDoLoop;
			const bool IsEmptyCondBody = Frame.IsEmptyCondBody, HasDirectControlBrace = Frame.HasDirectControlBrace;
			const bool IsSealed = Frame.IsSealed, IsContinuation = Frame.IsContinuation, IsContinuedList = Frame.IsContinuedList;
			const std::pmr::vector<uint8_t> &Candidates = Frame.Candidates;
			std::pmr::vector<uint32_t> &OpenScopes = Frame.OpenScopes;
			NodeId &LastChild = Frame.LastChild;
			if(Frame.Next == Node.Children.size()) {
				// 尾に保つ原文の間隙
				if(const SemanticBoundary *const Tail = GetPreservedAt(Syntax, Owner, static_cast<uint32_t>(Node.Children.size()))) {
					Output.Gap(*Tail);
				}
				if(IsSealed) --State.NoBreakDepth;
				// Kotlin の空の for の本体は文法が区切をノードの外へ置く為，後続の文を本体にしない様に補う
				if(
					Node.Facts.TokenSpacing == TokenSpacingMode::None && IsRole(Node.Kind.Def, Role::EmptyLoopBody) &&
					Syntax.GetFirstNamedChildWithRole(Owner, Role::BodyWrapper) == NoNode
				) Output.GetText(Syntax, GeneratedToken::StmtTerminator);
				if(IsContinuedList) {
					Output.CloseScope();
					Output.Soft("");
					Output.IfBroken({ GeneratedItem(Syntax, State.IR, GeneratedToken::CallClose) });
					Built Wrapped = Output.Finish(NoNode, State.NextGroup++);
					State.IR.Items[Wrapped.Item].Node = Owner;
					State.IR.Items[Wrapped.Item].Operation = FormOperation::Continuation;
					Wrapped.Item = Anchored(State.IR, Owner, Wrapped.Item);
					// 継続括弧と要素の改行を同じグループへ束ねた並びの返戻
					Completed = std::move(Wrapped);
					Frames.pop_back();
					continue;
				}
				if(IsContinuation) {
					Built Content = Output.Finish(NoNode, State.NextGroup++);
					Emitter WrappedOutput(State.IR);
					WrappedOutput.IfBroken({ GeneratedItem(Syntax, State.IR, GeneratedToken::CallOpen) });
					WrappedOutput.OpenScope();
					WrappedOutput.Soft("");
					WrappedOutput.Append(std::move(Content));
					WrappedOutput.CloseScope();
					WrappedOutput.Soft("");
					WrappedOutput.IfBroken({ GeneratedItem(Syntax, State.IR, GeneratedToken::CallClose) });
					Built Wrapped = WrappedOutput.Finish(NoNode, State.NextGroup++);
					State.IR.Items[Wrapped.Item].Node = Owner;
					State.IR.Items[Wrapped.Item].Operation = FormOperation::Continuation;
					Wrapped.Item = Anchored(State.IR, Owner, Wrapped.Item);
					// 選択可能なラッパーを持つ式の返戻
					Completed = std::move(Wrapped);
					Frames.pop_back();
					continue;
				}
				// ラッパーを補わない平坦形と後送の終端の条件の構築
				const uint32_t Group = State.NextGroup++;
				Built Result = Output.Finish(Owner, Group);
				const std::span<const FormatItemId> Parts = State.IR.GetChildrenOf(State.IR.Items[Result.Item]);
				if(
					!Parts.empty() && State.IR.Items[Parts.front()].Kind == FormatItemKind::Group && State.IR.Items[Parts.front()].Group == Group
				) {
					if(Frame.ClosingDependency != NoGroup) State.IR.EditDetail(Parts.front()).ExpansionDependency = Frame.ClosingDependency;
					if(Syntax.GetParentOf(Result.Tail.Node) == Owner && IsEdgeBracket(Syntax, State.IR, Result.Tail, false)) {
						State.Contexts[Owner].ClosingGroup = Group;
					}
				}
				Result.Tail.ShouldBreakAfter = Result.Tail.ShouldBreakAfter || IsRole(Node.Kind.Def, Role::DeferredLiteralBody);
				// 後送の終端の条件を持つ平坦形の返戻
				Completed = std::move(Result);
				Frames.pop_back();
				continue;
			}
			const uint32_t Idx = Frame.Next;
			const NodeId Child = Completed ? Frame.Child : Node.Children[Idx];
			const bool IsNamed = Completed ? Frame.IsNamedChild : Syntax.IsNamed(Child);
			Built Subtree;
			if(Completed) {
				Subtree = std::move(*Completed);
				Completed.reset();
			} else {
				Frame.Child = Child;
				Frame.IsNamedChild = IsNamed;
				// 括弧の対の閉じの直前の区切を含めた深さの範囲の終了
				if(!OpenScopes.empty() && OpenScopes.back() == Idx) {
					OpenScopes.pop_back();
					Output.CloseScope();
				}
				if(!IsNamed && Syntax.GetNode(Child).Lexeme.empty()) {
					++Frame.Next;
					continue;
				}
				// 本体の前の見出は独自のグループとし，本体の未評価境界を保って先に測る
				const bool IsDirectBodyOpen = HasDirectControlBrace && Syntax.IsTokenRole(Child, Role::OpeningBraceToken) && OpenScopes.empty();
				if(IsNamed && State.Contexts[Child].IsOptionalBody || IsDirectBodyOpen) {
					Built Header = Output.Finish(NoNode, State.NextGroup++);
					Output = Emitter(State.IR);
					Output.Append(std::move(Header));
				}
				if(!IsNamed) Subtree = BuildLeaf(Syntax, State.IR, Child);
				else if(Syntax.Is(Child, Role::PreprocBranch)) Subtree = co_await BuildPreprocBlock(State, Child);
				else if(!State.NoBreakDepth && State.Contexts[Child].Structured) Subtree = ReleaseStructured(State, Child);
				else if(Syntax.GetNode(Child).Children.empty() && (!State.Contexts[Child].ShouldWrap || State.NoBreakDepth)) {
					Subtree = BuildLeaf(Syntax, State.IR, Child);
				} else {
					NextNode = Child;
					continue;
				}
			}
			if(Subtree.Head.Item == NoItem) {
				++Frame.Next;
				continue;
			}
			// 開きを密着させるコンテナも，内側の独立した閉じに続く閉じを同行へ戻さない
			if(
				Frame.HasFixedOpening && LastChild != NoNode && Syntax.IsBracketLead(Child, Role::ClosingBracketLead) &&
				IsEdgeBracket(Syntax, State.IR, Output.GetLast(), false)
			) {
				const NodeId ClosingOwner = Syntax.GetParentOf(Output.GetLast().Node);
				Syntax.Charge(1);
				if(ClosingOwner != NoNode && ClosingOwner != Owner) Frame.ClosingDependency = State.Contexts[ClosingOwner].ClosingGroup;
			}
			if(LastChild != NoNode) {
				const std::span<const SemanticBoundary> Boundaries = Node.Facts.Boundaries.GetView();
				while(Frame.BoundaryCursor < Boundaries.size() && Boundaries[Frame.BoundaryCursor].Before < Idx) {
					Syntax.Charge(1);
					++Frame.BoundaryCursor;
				}
				const SemanticBoundary *const Recorded =
				Frame.BoundaryCursor < Boundaries.size() && Boundaries[Frame.BoundaryCursor].Before == Idx ?
				&Boundaries[Frame.BoundaryCursor] :
				nullptr;
				// 候補の境界か（読み飛ばしたトークンを挟む時は其の間の何れかの境界）
				bool IsCandidate = false;
				for(uint32_t Boundary = Syntax.GetNode(LastChild).Idx + 1; !Candidates.empty() && Boundary <= Idx; ++Boundary) {
					IsCandidate = IsCandidate || Candidates[Boundary];
				}
				const bool HasBodyClauseBoundary = (
					IsNamed && Syntax.Is(Child, Role::ElseLikeClause) && !IsRole(Node.Kind.Def, Role::GuardStmt) ||
					Syntax.Is(Child, Role::ElseKeyword) && IsRole(Node.Kind.Def, Role::If) ||
					IsDoLoop && HasSingleTokenRole(State, Child, Role::KeywordWhile)
				) && State.Contexts[LastChild].IsOptionalBody;
				// エラー領域に接する境界等の原文の間隙と，Ruby の文と Go のメンバの間の改行を代替する文区切（手前が右の被演算子を欠いた範囲なら区切を入れると値が変わる）
				if(Recorded && Recorded->Kind == BoundaryKind::Preserved) AppendPreservedGap(Syntax, LastChild, *Recorded, Output);
				else if(Recorded && (Recorded->Kind == BoundaryKind::TokenGlued || Recorded->Kind == BoundaryKind::TokenSpaced)) {
					if(IsCandidate) Output.Soft(Recorded->Kind == BoundaryKind::TokenSpaced ? " " : "");
					else if(Recorded->Kind == BoundaryKind::TokenSpaced) Output.Space();
				} else if((IsDelimitedStmts || IsDelimitedMembers) && IsNamed && Syntax.IsNamed(LastChild)) {
					if(!HasTrailingOpenRange(State, LastChild)) {
						if(IsDelimitedMembers) {
							const FormatItemId Separator = GeneratedItem(Syntax, State.IR, GeneratedToken::StmtTerminator);
							if(IsRole(Node.Kind.Def, Role::OptionalSemicolonHost)) State.IR.EditDetail(Separator).PunctuationOwner = Child;
							Output.IfBroken({ Separator }, true);
							Output.Soft(" ");
						} else {
							Output.GetText(Syntax, GeneratedToken::StmtTerminator);
							Output.Space();
						}
					}
				} else if(
					IsDelimitedMembers && Syntax.IsNamed(LastChild) && Syntax.Is(LastChild, Role::ClassMember) &&
					Syntax.IsTokenRole(Child, Role::ClosingBraceToken)
				) {
					// 同行の閉波括弧の前にも区切を要するメンバの終端
					const FormatItemId Separator = GeneratedItem(Syntax, State.IR, GeneratedToken::StmtTerminator);
					if(IsRole(Node.Kind.Def, Role::OptionalSemicolonHost)) State.IR.EditDetail(Separator).PunctuationOwner = Child;
					Output.IfBroken({ Separator }, true);
					Output.Soft(" ");
				} else if(
					// プリプロセッサディレクティブのブロックの終端ディレクティブの行の後と，閉じの後でない節（`else` 等）の前の改行
					Syntax.Is(LastChild, Role::HardLineDirective) || Syntax.Is(LastChild, Role::PreprocBranch) || Syntax.Is(Child, Role::Preproc) ||
					Node.Facts.TokenSpacing == TokenSpacingMode::None && !State.NoBreakDepth && IsNamed && Syntax.Is(Child, Role::ElseLikeClause) &&
					!IsRole(Node.Kind.Def, Role::GuardStmt) && !IsBlockClose(Syntax, State.IR, Output.GetLast()) && !HasBodyClauseBoundary
				) Output.Break();
				else if(!HasBodyClauseBoundary) {
					if(const bool IsSpaced = ShouldInsertSpace(State, Owner, LastChild, Child, Recorded); IsSpaced && !IsCandidate) Output.Space();
					else if(
						IsCandidate && (
							IsSpaced || Node.Facts.TokenSpacing == TokenSpacingMode::Punctuation || !Syntax.GetRules().IsWhitespaceSemantic ||
							CanBreakTightBoundary(Syntax, Owner, Idx)
						)
					) {
						// Go の折り返した並びの閉じの前の末尾のカンマ（グループの展開時だけ出し，閉じの内側の空白の有無に依らない）
						if(
							Node.Facts.TokenSpacing == TokenSpacingMode::None && !IsNamed && Syntax.IsBracketLead(Child, Role::ClosingBracketLead) &&
							TrailingCommaForm(Syntax, Owner, LastChild, Child, true)
						) {
							const FormatItemId Comma = GeneratedItem(Syntax, State.IR, GeneratedToken::ListSeparator);
							State.IR.EditDetail(Comma).PunctuationOwner = Owner;
							Output.IfBroken({ Comma });
						}
						Output.Soft(IsSpaced ? " " : "");
					}
				}
			// 密着する境界は，構文の役割で改行が合法と確定した括弧内だけを候補にする（行分割仕様 2.1）
			}
			// 本体の無い Ruby の条件の後は節の境を示す語が要る（`if a end` は構文エラー）
			if(!IsNamed && IsEmptyCondBody && HasSingleTokenRole(State, Child, Role::BlockCloseKeyword) && !HasBodyStmt(State, Owner)) {
				Output.GetText(Syntax, Word::CondThen);
				Output.Space();
			}
			if(IsNamed) {
				// 並びの中のプリプロセッサディレクティブの条件のブロックの行分離
				// 行幅で波括弧を付ける単一の文の制御本体のグループ（整形仕様 6.1）
				if(State.Contexts[Child].IsOptionalBody) Subtree = BuildBraceGroup(State, Owner, Child, std::move(Subtree));
				AppendLexicalSeparator(Syntax, Child, Subtree.Head, Output);
				AdjustErrorZoneScopes(Syntax, Child, Output);
				Output.Append(std::move(Subtree));
			} else {
				AppendLexicalSeparator(Syntax, Child, Subtree.Head, Output);
				Output.Append(std::move(Subtree));
			}
			LastChild = Child;
			// 括弧の対の開きの後の深さの範囲
			if(State.Contexts[Child].ScopeCloser < Node.Children.size()) {
				Output.OpenScope();
				OpenScopes.push_back(State.Contexts[Child].ScopeCloser);
			}
			++Frame.Next;
		}
	}

	/**
	 * 内部を折らない平坦形の組立関数
	 * 行末が構文を終えるプリプロセッサディレクティブと，ヒアドキュメントの本文を控えた開始行の続き（行分割仕様 2.3）は，１行の儘で選択可能な境界を作らない
	 * 計算量：部分木のノード数 N，テキスト長 B と補助照会費用 Q に対し O(N + B + Q)
	 * @param State 構築中の状態
	 * @param Id ノード
	 * @return 選択可能な境界を持たない平坦形
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildSealedFlat(Builder &State, const NodeId Id) {
		// 内部を折らない平坦形の組立
		++State.NoBreakDepth;
		Built Line = co_await BuildFlat(State, Id);
		--State.NoBreakDepth;
		// 内部を折らない平坦形の返戻
		co_return Line;
	}
} // namespace Core
