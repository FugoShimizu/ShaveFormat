#include "Canonicalizer.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 規則から生成する字句の綴りの取得関数
	 * @param Syntax 構文モデル
	 * @param Token 字句の種類
	 * @return 字句の綴り
	 */
	std::string_view Canonicalizer::GeneratedSpelling(const SyntaxModel &Syntax, const GeneratedToken Token) {
		// 生成字句から形式内の部品の役割への対応付け
		FormPartRole Role = FormPartRole::Result;
		switch(Token) {
		case GeneratedToken::None:
			return {};
		case GeneratedToken::BlockOpen:
			Role = FormPartRole::BlockOpen;
			break;
		case GeneratedToken::ListSeparator:
			Role = FormPartRole::Separator;
			break;
		case GeneratedToken::StatementTerminator:
			Role = FormPartRole::Terminator;
			break;
		case GeneratedToken::BlockClose:
			Role = FormPartRole::BlockClose;
			break;
		case GeneratedToken::CallOpen:
			Role = FormPartRole::CallOpen;
			break;
		case GeneratedToken::CallClose:
			Role = FormPartRole::CallClose;
			break;
		}
		// 生成形式の中の固定字句の探索
		for(const CanonicalForm &Form : CanonicalFormList(Syntax.Rules().GeneratedForms)) {
			if(Form.Operation != FormOperation::GeneratedLexemes) continue;
			const FormPart *const Part = Form.PartOf(Role);
			// 文法が定める字句の返戻
			return Part ? Part->FixedLexeme : std::string_view();
		}
		// 形式を持たない事の返戻
		return {};
	}

	/**
	 * 展開時に選ぶ生成字句の要素の追加関数
	 * @param Syntax 構文モデル
	 * @param Token 字句の種類
	 * @param IR 中間表現
	 * @return 追加した要素
	 */
	FormatItemId Canonicalizer::GeneratedItem(const SyntaxModel &Syntax, const GeneratedToken Token, FormatIR &IR) {
		const FormatItemId Item = IR.Leaf(GeneratedSpelling(Syntax, Token), FormatItemKind::Token);
		IR.Items[Item].Generated = Token;
		// 字句の要素の返戻
		return Item;
	}

	/**
	 * 要素の端に在る平坦時の葉の取得関数
	 * @param IR 中間表現
	 * @param Item 辿る要素
	 * @param First 先頭側を探すか
	 * @param Node 親が持つ字句の節点
	 * @return 字を持つ葉又は行端空白の平坦形（無ければ無し）
	 */
	Canonicalizer::EdgeItem Canonicalizer::FindEdge(
		const FormatIR &IR,
		const FormatItemId Item,
		const bool First,
		const NodeId Node
	) {
		// 要素が無い場合の返戻
		if(Item == NoItem) return {};
		// 平坦時に字を持つ端の葉の探索
		std::vector<EdgeItem> Pending { { Item, Node } };
		while(!Pending.empty()) {
			const EdgeItem Current = Pending.back();
			Pending.pop_back();
			const FormatItem &Part = IR.Items[Current.Item];
			if(Part.Kind == FormatItemKind::IfBroken) continue;
			const NodeId Owner = Part.Node == NoNode ? Current.Node : Part.Node;
			if(
				const std::span<const FormatItemId> Children = IR.ChildrenOf(Part);
				!Children.empty() && Part.Kind != FormatItemKind::LineEdgeSpace
			) {
				if(First) for(size_t Index = Children.size(); Index--;) Pending.push_back({ Children[Index], Owner });
				else for(const FormatItemId Child : Children) Pending.push_back({ Child, Owner });
				continue;
			}
			if(Part.Kind == FormatItemKind::HardBreak || Part.Text.Size) {
				const std::string_view Flat = IR.TextOf(Part);
				// 平坦時に字を持つ葉の返戻
				return {
					Current.Item,
					Owner,
					Flat.empty() ? '\0' : Flat.front(),
					Flat.empty() ? '\0' : Flat.back(),
					false,
					Part.Kind == FormatItemKind::HardBreak || Part.Kind == FormatItemKind::Verbatim && Flat.ends_with('\n'),
					(
						Part.Kind == FormatItemKind::Space || Part.Kind == FormatItemKind::SoftBreak || Part.Kind == FormatItemKind::LineEdgeSpace ||
						Part.Kind == FormatItemKind::Token || Part.Kind == FormatItemKind::Verbatim
					) && Flat.ends_with(' ')
				};
			}
		}
		// 字を持つ葉が無い事の返戻
		return {};
	}

	/**
	 * 要素の末尾の直前に在る平坦時の葉の取得関数
	 * @param IR 中間表現
	 * @param Item 辿る要素
	 * @return 末尾の直前の葉（無ければ無し）
	 */
	Canonicalizer::EdgeItem Canonicalizer::FindPrecedingEdge(const FormatIR &IR, const FormatItemId Item) {
		// 要素が無い場合の返戻
		if(Item == NoItem) return {};
		// 末尾の直前に在る葉の探索
		EdgeItem Previous, Last;
		std::vector<EdgeItem> Pending { { Item, NoNode } };
		while(!Pending.empty()) {
			const EdgeItem Current = Pending.back();
			Pending.pop_back();
			const FormatItem &Part = IR.Items[Current.Item];
			if(Part.Kind == FormatItemKind::IfBroken) continue;
			const NodeId Owner = Part.Node == NoNode ? Current.Node : Part.Node;
			if(
				const std::span<const FormatItemId> Children = IR.ChildrenOf(Part);
				!Children.empty() && Part.Kind != FormatItemKind::LineEdgeSpace
			) {
				for(size_t Index = Children.size(); Index--;) Pending.push_back({ Children[Index], Owner });
				continue;
			}
			if(Part.Kind == FormatItemKind::HardBreak || Part.Text.Size) {
				Previous = Last;
				const std::string_view Flat = IR.TextOf(Part);
				Last = { Current.Item, Owner, Flat.empty() ? '\0' : Flat.front(), Flat.empty() ? '\0' : Flat.back() };
			}
		}
		// 直前の葉の返戻
		return Previous;
	}

	/**
	 * 端の字句自身の字の取得関数
	 * @param Syntax 構文モデル
	 * @param IR 中間表現
	 * @param Edge 端の葉
	 * @param First 最初の字を取るか
	 * @return 字（無ければ零）
	 */
	char Canonicalizer::EdgeChar(const SyntaxModel &Syntax, const FormatIR &IR, const EdgeItem Edge, const bool First) {
		// 端が無い場合の返戻
		if(Edge.Item == NoItem) return '\0';
		const FormatItem &Item = IR.Items[Edge.Item];
		// 必須改行の返戻
		if(Item.Kind == FormatItemKind::HardBreak) return '\n';
		if(Edge.Node != NoNode && Item.Kind == FormatItemKind::Token && Item.Generated == GeneratedToken::None && Edge.IsWholeToken) {
			// 元の字句の端の返戻
			return First ? Syntax.FirstChar(Edge.Node) : Syntax.LastChar(Edge.Node);
		}
		if(Item.Generated != GeneratedToken::None) {
			const std::string_view Text = GeneratedSpelling(Syntax, Item.Generated);
			// 生成字句の端の返戻
			return Text.empty() ? '\0' : First ? Text.front() : Text.back();
		}
		// 葉を作る時に記録した端の返戻
		return First ? Edge.FirstChar : Edge.LastChar;
	}

	/**
	 * 端の字句の役割の判定関数
	 * @param Syntax 構文モデル
	 * @param IR 中間表現
	 * @param Edge 端の葉
	 * @param Role 字句の役割
	 * @param Generated 生成した字句の種類
	 * @return 何れかの役割に当れば true
	 */
	bool Canonicalizer::EdgeIs(
		const SyntaxModel &Syntax,
		const FormatIR &IR,
		const EdgeItem Edge,
		const Role Role,
		const GeneratedToken Generated
	) {
		// 端が無い場合の返戻
		if(Edge.Item == NoItem) return false;
		const FormatItem &Item = IR.Items[Edge.Item];
		// 節点又は生成字句の役割の返戻
		return Edge.Node != NoNode && Item.Kind == FormatItemKind::Token && Edge.IsWholeToken && Syntax.IsTokenRole(Edge.Node, Role) ||
		Generated != GeneratedToken::None && Item.Generated == Generated;
	}

	/**
	 * 端の括弧の判定関数
	 * @param Syntax 構文モデル
	 * @param IR 中間表現
	 * @param Edge 端の葉
	 * @param Opening 開き括弧を探すか
	 * @return 括弧なら true
	 */
	bool Canonicalizer::EdgeIsBracket(const SyntaxModel &Syntax, const FormatIR &IR, const EdgeItem Edge, const bool Opening) {
		// 開き又は閉じの括弧の返戻
		return Opening ?
		EdgeIs(Syntax, IR, Edge, Role::OpeningBraceToken, GeneratedToken::BlockOpen) ||
		EdgeIs(Syntax, IR, Edge, Role::OpeningParenToken, GeneratedToken::CallOpen) ||
		EdgeIs(Syntax, IR, Edge, Role::OpeningBracketToken) :
		EdgeIs(Syntax, IR, Edge, Role::ClosingBraceToken, GeneratedToken::BlockClose) ||
		EdgeIs(Syntax, IR, Edge, Role::ClosingParenToken, GeneratedToken::CallClose) ||
		EdgeIs(Syntax, IR, Edge, Role::ClosingSquareToken);
	}

	/**
	 * 最後の行のコードの開きの判定関数
	 * @return 埋込コードの開きを持つ行なら true
	 */
	bool Canonicalizer::Emitter::HasInlineCode() const {
		// 同行の開きの有無の返戻
		return IsCodeOpenOnLine;
	}

	/**
	 * 要素の追加関数
	 * @param Item 追加する要素
	 * @param Flat 要素の平坦時の字面
	 * @param IsBreak 必須改行を含む要素か
	 */
	void Canonicalizer::Emitter::Push(const FormatItemId Item, const std::string_view Flat, const bool IsBreak) {
		// 追加した要素に応じた先頭と末尾の更新
		Items.push_back(Item);
		if(!Flat.empty()) {
			const FormatItemKind Kind = Target->Items[Item].Kind;
			const EdgeItem Current {
				Item,
				Target->Items[Item].Node,
				Flat.front(),
				Flat.back(),
				false,
				Kind == FormatItemKind::HardBreak || Kind == FormatItemKind::Verbatim && Flat.ends_with('\n'),
				(
					Kind == FormatItemKind::Space || Kind == FormatItemKind::SoftBreak || Kind == FormatItemKind::LineEdgeSpace ||
					Kind == FormatItemKind::Token || Kind == FormatItemKind::Verbatim
				) && Flat.ends_with(' ')
			};
			if(Head.Item == NoItem) Head = Current;
			BeforeTail = Tail;
			Tail = Current;
		}
		// 巻戻し用の端と改行状態の記録
		EdgeHistory.push_back({ Head, Tail, BeforeTail });
		HasHardBreak = HasHardBreak || IsBreak;
		if(IsBreak) IsCodeOpenOnLine = false;
		// 終了
		return;
	}

	/**
	 * 字を持つ要素の無い組立かの判定関数
	 * @return 字を持つ要素が無ければ true
	 */
	bool Canonicalizer::Emitter::IsEmpty() const {
		// 先頭の要素の有無の返戻
		return Head.Item == NoItem;
	}

	/**
	 * 末尾が改行かの判定関数
	 * @return 改行なら true
	 */
	bool Canonicalizer::Emitter::IsAtLineStart() const {
		// 末尾が有り，必須改行又は改行で終わる逐語かの返戻
		return Tail.Item != NoItem && Tail.EndsWithNewline;
	}

	/**
	 * 末尾が空白かの判定関数
	 * @return 空白なら true
	 */
	bool Canonicalizer::Emitter::EndsWithBlank() const {
		// 末尾が有り，空白の葉又は平坦時に空白で終わる境界かの返戻
		return Tail.Item != NoItem && Tail.EndsWithSpace;
	}

	/**
	 * 末尾の字句の役割の判定関数
	 * @param Syntax 構文モデル
	 * @param Role 字句の役割
	 * @param Generated 生成した字句の種類
	 * @return 何れかの役割に当れば true
	 */
	bool Canonicalizer::Emitter::TailIs(const SyntaxModel &Syntax, const Role Role, const GeneratedToken Generated) const {
		// 末尾の役割の返戻
		return EdgeIs(Syntax, *Target, Tail, Role, Generated);
	}

	/**
	 * 末尾の要素の取得関数
	 * @return 末尾の要素
	 */
	Canonicalizer::EdgeItem Canonicalizer::Emitter::Last() const {
		// 末尾の要素の返戻
		return Tail;
	}

	/**
	 * 末尾の直前の要素の取得関数
	 * @return 直前の要素
	 */
	Canonicalizer::EdgeItem Canonicalizer::Emitter::Preceding() const {
		// 直前の要素の返戻
		return BeforeTail;
	}

	/**
	 * 巻戻しの印の取得関数
	 * @return 現在の状態
	 */
	Canonicalizer::Emitter::Mark Canonicalizer::Emitter::Save() const {
		// 現在の状態の返戻
		return {
			Items.size(),
			EdgeHistory.size(),
			Outer.size(),
			Head,
			Tail,
			BeforeTail,
			HasHardBreak,
			HasCodeBoundary,
			IsCodeOpenOnLine,
			HasSoft
		};
	}

	/**
	 * 要素を置く中間表現の取得関数
	 * @return 構築中の中間表現
	 */
	FormatIR &Canonicalizer::Emitter::Store() const {
		// 中間表現の返戻
		return *Target;
	}

	/**
	 * 種類で指定した固定字句の追加関数
	 * @param Syntax 構文モデル
	 * @param Token 規則から生成する字句の種類
	 */
	void Canonicalizer::Emitter::Text(const SyntaxModel &Syntax, const GeneratedToken Token) {
		const std::string_view Spelling = GeneratedSpelling(Syntax, Token);
		// 綴りの無い字句の場合の終了
		if(Spelling.empty()) return;
		Push(GeneratedItem(Syntax, Token, *Target), Spelling, false);
		// 終了
		return;
	}

	/**
	 * 言語毎の綴りを持つ語の追加関数
	 * @param Syntax 構文モデル
	 * @param Meaning 語の意味
	 * @param Before 語の前に空白を置くか
	 * @param After 語の後に空白を置くか
	 */
	void Canonicalizer::Emitter::Text(const SyntaxModel &Syntax, const Word Meaning, const bool Before, const bool After) {
		const std::string_view Spelling = Syntax.Rules().Spelling(Meaning);
		std::string Rendered;
		if(Before) Rendered.push_back(' ');
		Rendered.append(Spelling);
		if(After) Rendered.push_back(' ');
		// 字面の無い場合の終了
		if(Rendered.empty()) return;
		Push(Target->Leaf(Rendered, FormatItemKind::Token), Rendered, false);
		// 終了
		return;
	}

	/**
	 * 空白の追加関数
	 */
	void Canonicalizer::Emitter::Space() {
		Push(Target->Leaf(" ", FormatItemKind::Space), " ", false);
		// 終了
		return;
	}

	/**
	 * 固定の区切記法の追加関数（整形仕様 6.1）
	 * @param Text 前後の字句から規則で決めた区切記法
	 */
	void Canonicalizer::Emitter::Separator(const std::string_view Text) {
		Push(Target->Leaf(Text, FormatItemKind::LexicalSeparator), Text, false);
		// 終了
		return;
	}

	/**
	 * 必須改行の追加関数
	 */
	void Canonicalizer::Emitter::Break() {
		Push(Target->Leaf({}, FormatItemKind::HardBreak), "\n", true);
		// 終了
		return;
	}

	/**
	 * 保持する原文の間隙の追加関数（整形仕様 4.4, 4.6）
	 * マクロ内部の間隙だけを逐語の断片として写し，他の間隙の字下げは配置へ委ねる
	 * @param Boundary 原文の間隙と其の由来
	 */
	void Canonicalizer::Emitter::Gap(const SemanticBoundary &Boundary) {
		const std::string_view Text = Boundary.Gap;
		if(Boundary.Source == PreservedSource::MacroTokens || Boundary.Source == PreservedSource::LiteralContent) {
			// 保つ間隙の無い場合の終了
			if(Text.empty()) return;
			Push(Target->Leaf(Text, FormatItemKind::Verbatim), Text, Text.find('\n') != std::string_view::npos);
			// 逐語の間隙を写した場合の終了
			return;
		}
		for(size_t Cursor = 0, Line = 0;; ++Line) {
			const size_t Newline = Text.find('\n', Cursor);
			std::string_view Piece = Text.substr(Cursor, Newline == std::string_view::npos ? std::string_view::npos : Newline - Cursor);
			// 行頭のインデントと行末の空白の除去
			if(Line) Piece.remove_prefix(std::min(Piece.find_first_not_of(" \t\r"), Piece.size()));
			if(Newline != std::string_view::npos) {
				Piece.remove_suffix(Piece.size() - std::min(Piece.find_last_not_of(" \t\r") + 1, Piece.size()));
			}
			if(Line) Break();
			if(!Piece.empty()) Push(Target->Leaf(Piece, FormatItemKind::Verbatim), Piece, false);
			if(Newline == std::string_view::npos) break;
			Cursor = Newline + 1;
		}
		// 終了
		return;
	}

	/**
	 * 空行の規則の追加関数（整形仕様 6.5）
	 * 字面を持たず，前後の宣言の複数行か構造種別の違いで空行を置くかを出力の具体化で決める
	 * @param Before 前の宣言
	 * @param After 後の宣言
	 * @param IsForced 常に空行を置くか
	 */
	void Canonicalizer::Emitter::Blank(const NodeId Before, const NodeId After, const bool IsForced) {
		// 空行の前後と強制条件を持つ要素の記録
		const FormatItemId Item = Target->Leaf({}, FormatItemKind::BlankLine);
		FormatItemDetail &Detail = Target->EditDetail(Item);
		Detail.Before = Before;
		Detail.After = After;
		Detail.IsForced = IsForced;
		Items.push_back(Item);
		EdgeHistory.push_back({ Head, Tail, BeforeTail });
		// 終了
		return;
	}

	/**
	 * 選択可能な境界の追加関数
	 * 組立の完了時に此の組立器の群へ所属させる
	 * @param Flat 平坦時の字面（空白又は密着）
	 */
	void Canonicalizer::Emitter::Soft(const std::string_view Flat) {
		Push(Target->Leaf(Flat, FormatItemKind::SoftBreak), Flat, false);
		HasSoft = true;
		// 終了
		return;
	}

	/**
	 * 組立の群の展開時だけ出す断片の追加関数（整形仕様 6.1）
	 * 群は組立の完了時に決め，選択可能な境界を出さなかった組立では出さない
	 * @param Fragment 展開時だけ出す断片（字句・空白・必須改行）
	 */
	void Canonicalizer::Emitter::IfBroken(const std::initializer_list<FormatItemId> Fragment) {
		const FormatItemId Item =
		Target->Parent(FormatItemKind::IfBroken, std::span<const FormatItemId>(Fragment.begin(), Fragment.size()));
		Target->Items[Item].Group = PendingGroup;
		Push(Item, {}, false);
		// 終了
		return;
	}

	/**
	 * 展開時だけ出す断片の群の確定関数
	 * 此の組立が出した断片（深さの範囲の中を含み，組立済の子の要素は除く）へ群を付け，群が無ければ断片を除く
	 * 計算量：辿る要素数 V に対し O(V)
	 * @param Targets 組立の要素
	 * @param Group 組立の群（無ければ無し）
	 * @param IR 要素を置いた中間表現
	 */
	void Canonicalizer::Emitter::BindConditions(const std::span<const FormatItemId> Targets, const uint32_t Group, FormatIR &IR) {
		// 深さの範囲内を含む条件付断片の探索
		struct Frame {
			std::span<const FormatItemId> Items;
			size_t Next = 0;
		};
		std::vector<Frame> Stack { { Targets } };
		while(!Stack.empty()) {
			Frame &Current = Stack.back();
			if(Current.Next == Current.Items.size()) {
				Stack.pop_back();
				continue;
			}
			FormatItem &Item = IR.Items[Current.Items[Current.Next++]];
			if(Item.Kind == FormatItemKind::IndentScope) Stack.push_back({ IR.ChildrenOf(Item) });
			if(Item.Kind != FormatItemKind::IfBroken || Item.Group != PendingGroup) continue;
			Item.Group = Group;
			if(Group == NoGroup) Item = FormatItem{};
		}
		// 終了
		return;
	}

	/**
	 * 組立済の要素の追加関数
	 * @param Sub 組立済の要素（字面の無い要素は何もしない）
	 */
	void Canonicalizer::Emitter::Append(Built &&Sub) {
		// 字面の無い要素の場合の終了
		if(Sub.Head.Item == NoItem) return;
		// 子の端と改行及びコード境界の状態の反映
		Items.push_back(Sub.Item);
		if(Head.Item == NoItem) Head = Sub.Head;
		BeforeTail = Sub.BeforeTail.Item == NoItem ? Tail : Sub.BeforeTail;
		Tail = Sub.Tail;
		EdgeHistory.push_back({ Head, Tail, BeforeTail });
		HasHardBreak = HasHardBreak || Sub.HasBreak;
		if(Sub.HasBreak || Sub.HasCodeBoundary) IsCodeOpenOnLine = Sub.IsCodeOpenOnLine;
		HasCodeBoundary = HasCodeBoundary || Sub.HasCodeBoundary;
		// 終了
		return;
	}

	/**
	 * 末尾を取り消した後の端の再取得関数
	 * 追加時の控えから，取り消した後に残る最後の葉へ戻す
	 */
	void Canonicalizer::Emitter::RecomputeTail() {
		const EdgeState Restored = EdgeHistory.empty() ? EdgeState{} : EdgeHistory.back();
		Head = Restored.Head;
		Tail = Restored.Tail;
		BeforeTail = Restored.BeforeTail;
		// 終了
		return;
	}

	/**
	 * 最後の字の除去関数
	 * 末尾の空白・改行を取り消す（取り消せるのは最後の空白・改行・選択可能な境界・１字の固定字句）
	 * @throws FormatLimitExceeded 最後の要素が取り消せない組立済の要素の場合（中間表現の組立の不具合）
	 */
	void Canonicalizer::Emitter::PopBack() {
		// 取り消す要素の無い場合の終了
		if(Items.empty()) return;
		const FormatItem &Last = Target->Items[Items.back()];
		const bool IsSingle = Last.Kind == FormatItemKind::Space || Last.Kind == FormatItemKind::HardBreak ||
		Last.Kind == FormatItemKind::SoftBreak && Last.Text.Size == 1 || Last.Kind == FormatItemKind::Token && Last.Text.Size == 1;
		if(!IsSingle) throw FormatLimitExceeded("the format IR cannot drop the last character", FailureReason::FormatterDefect);
		Items.pop_back();
		EdgeHistory.pop_back();
		RecomputeTail();
		// 終了
		return;
	}

	/**
	 * 末尾の空白の除去関数
	 */
	void Canonicalizer::Emitter::TrimSpaces() {
		while(!Items.empty() && Target->Items[Items.back()].Kind == FormatItemKind::Space) PopBack();
		// 終了
		return;
	}

	/**
	 * 深さの範囲の開始関数
	 * 以後の要素を範囲の中へ置き，範囲が行を跨ぐ時に内容を１段深くする（整形仕様 6.5）
	 */
	void Canonicalizer::Emitter::OpenScope() {
		Outer.push_back(std::move(Items));
		Items.clear();
		// 終了
		return;
	}

	/**
	 * 深さの範囲の終了関数
	 */
	void Canonicalizer::Emitter::CloseScope() {
		// 開いた範囲の無い場合の終了
		if(Outer.empty()) return;
		const FormatItemId Scope = Target->Parent(FormatItemKind::IndentScope, Items);
		Items = std::move(Outer.back());
		Outer.pop_back();
		Items.push_back(Scope);
		// 終了
		return;
	}

	/**
	 * 巻戻し関数
	 * @param Saved 巻き戻す印（同じ深さの範囲の中で取った物）
	 * @throws FormatLimitExceeded 印の後に深さの範囲の開閉が有った場合（中間表現の組立の不具合）
	 */
	void Canonicalizer::Emitter::Restore(Mark &&Saved) {
		if(Saved.Depth != Outer.size()) {
			throw FormatLimitExceeded("the format IR restores across an indent scope", FailureReason::FormatterDefect);
		}
		// 保存した要素と端及び境界の状態への巻戻し
		Items.resize(Saved.ItemCount);
		EdgeHistory.resize(Saved.HistoryCount);
		Head = Saved.Head;
		Tail = Saved.Tail;
		BeforeTail = Saved.BeforeTail;
		HasHardBreak = Saved.HasHardBreak;
		HasSoft = Saved.HasSoft;
		HasCodeBoundary = Saved.HasCodeBoundary;
		IsCodeOpenOnLine = Saved.IsCodeOpenOnLine;
		// 終了
		return;
	}

	/**
	 * 組立の完了関数
	 * 選択可能な境界を出した組立は群として包み，節点の錨で包む
	 * @param Node 錨の節点（無しなら包まない）
	 * @param Group 群の番号（無しなら群にしない）
	 * @return 組立済の要素
	 */
	Canonicalizer::Built Canonicalizer::Emitter::Finish(const NodeId Node, const uint32_t Group) {
		// 深さの範囲と条件付断片の確定
		while(!Outer.empty()) CloseScope();
		BindConditions(Items, HasSoft ? Group : NoGroup, *Target);
		const bool IsGroup = HasSoft && Group != NoGroup;
		const FormatItemId Content = Target->Parent(IsGroup ? FormatItemKind::Group : FormatItemKind::Sequence, Items);
		if(IsGroup) {
			Target->Items[Content].Group = Group;
			Target->Items[Content].Node = Node;
		}
		// 節点の錨と端の情報を持つ組立結果の生成
		Items.clear();
		Built Result;
		Result.Item = Node == NoNode ? Content : Anchored(Node, Content, *Target);
		Result.Head = Head;
		Result.Tail = Tail;
		Result.BeforeTail = BeforeTail;
		Result.HasBreak = HasHardBreak;
		Result.HasCodeBoundary = HasCodeBoundary;
		Result.IsCodeOpenOnLine = IsCodeOpenOnLine;
		// 組立済の要素の返戻
		return Result;
	}

	/**
	 * コンストラクタ（要素を置く中間表現を受け取る）
	 * @param IR 構築中の中間表現
	 */
	Canonicalizer::Emitter::Emitter(FormatIR &IR) : Target(&IR) {}
} // namespace Core
