#include "Canonicalizer.hpp"
#include "FormatIR.hpp"
#include "NodeId.hpp"
#include "SyntaxModel.hpp"
#include "SyntaxRole.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 規則から生成するトークンの綴りの取得関数
	 * 計算量：生成形式の数 F，各形式の部品数 P に対し O(F P)
	 * @param Syntax 構文モデル
	 * @param Token トークンの種類
	 * @return トークンの綴り
	 */
	std::string_view Canonicalizer::GetGeneratedSpelling(const SyntaxModel &Syntax, const GeneratedToken Token) {
		// 生成トークンから形式内の部品の役割への対応付
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
		case GeneratedToken::StmtTerminator:
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
		// 生成形式の中の固定トークンの探索
		for(const CanonicalForm &Form : CanonicalFormList(Syntax.GetRules().GeneratedForms)) {
			if(Form.Operation != FormOperation::GeneratedLexemes) continue;
			const FormPart *const Part = Form.GetPartOf(Role);
			// 文法が定めるトークンの返戻
			return Part ? Part->FixedLexeme : std::string_view();
		}
		// 形式を持たない事の返戻
		return {};
	}

	/**
	 * 端のトークンの役割の判定関数
	 * @param Syntax 構文モデル
	 * @param IR 中間表現
	 * @param Edge 端の葉
	 * @param Role トークンの役割
	 * @param Generated 生成したトークンの種類
	 * @return 何れかの役割に当れば true
	 */
	bool Canonicalizer::IsEdgeRole(
		const SyntaxModel &Syntax,
		const FormatIR &IR,
		const EdgeItem Edge,
		const Role Role,
		const GeneratedToken Generated
	) {
		// 端が無い場合の返戻
		if(Edge.Item == NoItem) return false;
		const FormatItem &Item = IR.Items[Edge.Item];
		// ノード又は生成トークンの役割の返戻
		return Edge.Node != NoNode && Item.Kind == FormatItemKind::Token && Edge.IsWholeToken && Syntax.IsTokenRole(Edge.Node, Role) ||
		Generated != GeneratedToken::None && Item.Generated == Generated;
	}

	/**
	 * 本体のブロックを閉じる端かの判定関数
	 * 値の波括弧を本体の閉じと混同せず，生成した閉じ又はブロックに直属するトークンだけを認める
	 * 計算量：O(1)
	 * @param Syntax 構文モデル
	 * @param IR 中間表現
	 * @param Edge 端の葉
	 * @return ブロックの閉じなら true
	 * @throws FormatLimitExceeded 構文の参照が作業予算を超える場合
	 */
	bool Canonicalizer::IsBlockClose(const SyntaxModel &Syntax, const FormatIR &IR, const EdgeItem Edge) {
		// 閉波括弧でない端の返戻
		if(!IsEdgeRole(Syntax, IR, Edge, Role::ClosingBraceToken, GeneratedToken::BlockClose)) return false;
		// 生成形式が本体を閉じる端の返戻
		if(IR.Items[Edge.Item].Generated == GeneratedToken::BlockClose) return true;
		Syntax.Charge(1);
		const NodeId Parent = Syntax.GetParentOf(Edge.Node);
		// インデントの本体を除き，波括弧を持つ本体の所属から閉じの役割を判定する
		return !Syntax.Is(Parent, Role::IndentedBody) &&
		Syntax.IsAny(Parent, { Role::DirectBraceLayout, Role::DirectControlBraceLayout, Role::IndentContainer });
	}

	/**
	 * 取得済の綴りから生成トークンの要素を追加する関数
	 * 計算量：テキスト長 B に対し償却 O(B)
	 * @param IR 中間表現
	 * @param Spelling 規則表から取得済の固定トークンの綴り
	 * @param Token トークンの種類
	 * @return 追加した要素
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	FormatItemId Canonicalizer::GeneratedItem(FormatIR &IR, const std::string_view Spelling, const GeneratedToken Token) {
		// 展開時に選ぶ生成トークンの要素の追加
		const FormatItemId Item = IR.Leaf(Spelling, FormatItemKind::Token);
		IR.Items[Item].Generated = Token;
		// トークンの要素の返戻
		return Item;
	}

	/**
	 * 展開時に選ぶ生成トークンの要素の追加関数
	 * 計算量：生成形式の数 F，各形式の部品数 P とテキスト長 B に対し償却 O(F P + B)
	 * @param Syntax 構文モデル
	 * @param IR 中間表現
	 * @param Token トークンの種類
	 * @return 追加した要素
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	FormatItemId Canonicalizer::GeneratedItem(const SyntaxModel &Syntax, FormatIR &IR, const GeneratedToken Token) {
		// 規則から取得した綴りに依る生成トークンの要素の返戻
		return GeneratedItem(IR, GetGeneratedSpelling(Syntax, Token), Token);
	}

	/**
	 * 要素の端に在る平坦時の葉の取得関数
	 * 計算量：中間表現で辿る要素数 N に対し O(N)
	 * @param IR 中間表現
	 * @param Item 辿る要素
	 * @param IsFirst 先頭側を探すか
	 * @param Node 親が持つトークンのノード
	 * @return 字を持つ葉又は行端空白の平坦形（無ければ `Item` が `NoItem`）
	 * @throws FormatLimitExceeded 端の探索が作業予算を超える場合
	 * @throws std::bad_alloc 端を探す作業列のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	Canonicalizer::EdgeItem Canonicalizer::FindEdge(
		const FormatIR &IR,
		const FormatItemId Item,
		const bool IsFirst,
		const NodeId Node
	) {
		// 平坦時に字を持つ端の葉の探索
		std::vector<EdgeItem> Pending { { Item, Node } };
		while(!Pending.empty()) {
			if(IR.NormalizedSyntax) IR.NormalizedSyntax->Charge(1);
			const EdgeItem Cur = Pending.back();
			Pending.pop_back();
			const FormatItem &Part = IR.Items[Cur.Item];
			if(Part.Kind == FormatItemKind::IfBroken && !Part.IsActivatedWhenFlat) continue;
			const NodeId Owner = Part.Node == NoNode ? Cur.Node : Part.Node;
			if(const std::span<const FormatItemId> Children = IR.GetChildrenOf(Part); !Children.empty()) {
				if(IsFirst) for(size_t Idx = Children.size(); Idx--;) Pending.push_back({ Children[Idx], Owner });
				else for(const FormatItemId Child : Children) Pending.push_back({ Child, Owner });
				continue;
			}
			if(Part.Kind == FormatItemKind::HardBreak || Part.Text.Size) {
				const std::string_view Flat = IR.GetTextOf(Part);
				// 平坦時に字を持つ葉の返戻
				return {
					Cur.Item,
					Owner,
					static_cast<unsigned char>(Flat.empty() ? '\0' : Flat.front()),
					static_cast<unsigned char>(Flat.empty() ? '\0' : Flat.back()),
					false,
					Part.Kind == FormatItemKind::HardBreak || Part.Kind == FormatItemKind::Verbatim && Flat.ends_with('\n'),
					(
						Part.Kind == FormatItemKind::Space || Part.Kind == FormatItemKind::SoftBreak || Part.Kind == FormatItemKind::Token ||
						Part.Kind == FormatItemKind::Verbatim
					) && Flat.ends_with(' ')
				};
			}
		}
		// 字を持つ葉が無い事の返戻
		return {};
	}

	/**
	 * 要素の末尾の直前に在る平坦時の葉の取得関数
	 * 計算量：中間表現で辿る要素数 N に対し O(N)
	 * @param IR 中間表現
	 * @param Item 辿る要素
	 * @return 末尾の直前の葉（無ければ `Item` が `NoItem`）
	 * @throws FormatLimitExceeded 端の探索が作業予算を超える場合
	 * @throws std::bad_alloc 端を探す作業列のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	Canonicalizer::EdgeItem Canonicalizer::FindPrecedingEdge(const FormatIR &IR, const FormatItemId Item) {
		// 末尾の直前に在る葉の探索
		EdgeItem Prev, Last;
		std::vector<EdgeItem> Pending { { Item, NoNode } };
		while(!Pending.empty()) {
			if(IR.NormalizedSyntax) IR.NormalizedSyntax->Charge(1);
			const EdgeItem Cur = Pending.back();
			Pending.pop_back();
			const FormatItem &Part = IR.Items[Cur.Item];
			if(Part.Kind == FormatItemKind::IfBroken && !Part.IsActivatedWhenFlat) continue;
			const NodeId Owner = Part.Node == NoNode ? Cur.Node : Part.Node;
			if(const std::span<const FormatItemId> Children = IR.GetChildrenOf(Part); !Children.empty()) {
				for(size_t Idx = Children.size(); Idx--;) Pending.push_back({ Children[Idx], Owner });
				continue;
			}
			if(Part.Kind == FormatItemKind::HardBreak || Part.Text.Size) {
				Prev = Last;
				const std::string_view Flat = IR.GetTextOf(Part);
				Last = {
					Cur.Item,
					Owner,
					static_cast<unsigned char>(Flat.empty() ? '\0' : Flat.front()),
					static_cast<unsigned char>(Flat.empty() ? '\0' : Flat.back())
				};
			}
		}
		// 直前の葉の返戻
		return Prev;
	}

	/**
	 * 端のトークン自身の字の取得関数
	 * @param IR 中間表現
	 * @param Edge 端の葉
	 * @param IsFirst 最初の字を取るか
	 * @return 字（無ければ零）
	 */
	unsigned char Canonicalizer::GetEdgeChar(const FormatIR &IR, const EdgeItem Edge, const bool IsFirst) {
		// 端が無い場合の返戻
		if(Edge.Item == NoItem) return IsFirst ? Edge.FirstChar : Edge.LastChar;
		// 必須改行の返戻
		if(IR.Items[Edge.Item].Kind == FormatItemKind::HardBreak) return '\n';
		// 葉を作る時に記録した端の返戻
		return IsFirst ? Edge.FirstChar : Edge.LastChar;
	}

	/**
	 * 端の括弧の判定関数
	 * @param Syntax 構文モデル
	 * @param IR 中間表現
	 * @param Edge 端の葉
	 * @param IsOpening 開括弧を探すか
	 * @return 括弧なら true
	 */
	bool Canonicalizer::IsEdgeBracket(const SyntaxModel &Syntax, const FormatIR &IR, const EdgeItem Edge, const bool IsOpening) {
		// 開き又は閉じの括弧の返戻
		return IsOpening ?
		IsEdgeRole(Syntax, IR, Edge, Role::OpeningBraceToken, GeneratedToken::BlockOpen) ||
		IsEdgeRole(Syntax, IR, Edge, Role::OpeningParenToken, GeneratedToken::CallOpen) ||
		IsEdgeRole(Syntax, IR, Edge, Role::OpeningBracketToken) :
		IsEdgeRole(Syntax, IR, Edge, Role::ClosingBraceToken, GeneratedToken::BlockClose) ||
		IsEdgeRole(Syntax, IR, Edge, Role::ClosingParenToken, GeneratedToken::CallClose) ||
		IsEdgeRole(Syntax, IR, Edge, Role::ClosingSquareToken);
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
	 * 要素の並びの取得関数
	 * @return 確保済の並び又は短い並びの格納先
	 */
	std::span<FormatItemId> Canonicalizer::Emitter::GetItems() {
		// 直接保持する要素又は連続した並びの返戻
		return InlineCount ? std::span(InlineItems.data(), InlineCount) : std::span(Items);
	}

	/**
	 * 並びの末尾への要素の追加関数
	 * 直接保持する枠を超える時だけ連続した保持領域を使う
	 * 計算量：格納済の要素数 N に対し償却 O(1)，領域の伸長時 O(N)
	 * @param Item 追加する要素
	 * @throws std::bad_alloc 並びのメモリを確保出来ない場合
	 * @throws std::length_error 並びが保持領域の最大長を超える場合
	 */
	void Canonicalizer::Emitter::PushItem(const FormatItemId Item) {
		// 短い並びは保持領域を確保しない
		if(Items.empty() && InlineCount < InlineItems.size()) InlineItems[InlineCount++] = Item;
		else {
			// 枠を超える最初の追加で，既存の要素と追加分を纏めて確保する
			if(InlineCount) {
				Items.reserve(InlineCount + 1);
				Items.insert(Items.end(), InlineItems.begin(), InlineItems.begin() + InlineCount);
				InlineCount = 0;
			}
			Items.push_back(Item);
		}
		// 終了
		return;
	}

	/**
	 * 要素の追加関数
	 * 計算量：格納済の要素数 N に対し償却 O(1)，領域の伸長時 O(N)
	 * @param Item 追加する要素
	 * @param Flat 要素の平坦時のテキスト
	 * @param IsBreak 必須改行を含む要素か
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::Emitter::Push(const FormatItemId Item, const std::string_view Flat, const bool IsBreak) {
		// 追加した要素に応じた先頭と末尾の更新
		FlushSpace();
		PushItem(Item);
		if(!Flat.empty()) {
			HasClauseBoundary = false;
			const FormatItemKind Kind = Target->Items[Item].Kind;
			const EdgeItem Cur {
				Item,
				Target->Items[Item].Node,
				static_cast<unsigned char>(Flat.front()),
				static_cast<unsigned char>(Flat.back()),
				false,
				Kind == FormatItemKind::HardBreak || Kind == FormatItemKind::Verbatim && Flat.ends_with('\n'),
				(
					Kind == FormatItemKind::Space || Kind == FormatItemKind::SoftBreak || Kind == FormatItemKind::Token ||
					Kind == FormatItemKind::Verbatim
				) && Flat.ends_with(' ')
			};
			if(Head.Item == NoItem) Head = Cur;
			BeforeTail = Tail;
			Tail = Cur;
		}
		// 改行状態の記録
		HasHardBreak = HasHardBreak || IsBreak;
		if(IsBreak) IsCodeOpenOnLine = false;
		// 終了
		return;
	}

	/**
	 * 後続が確定した空白の生成関数
	 * 計算量：固定数の要素の追加に対し償却 O(1)
	 * @throws FormatLimitExceeded 要素の生成が作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::Emitter::FlushSpace() {
		// 待つ空白が無い場合の終了
		if(!HasPendingSpace) return;
		HasPendingSpace = false;
		Push(Target->Leaf(" ", FormatItemKind::Space), " ", false);
		// 終了
		return;
	}

	/**
	 * 字を持つ要素の無い組立かの判定関数
	 * @return 字を持つ要素が無ければ true
	 */
	bool Canonicalizer::Emitter::IsEmpty() const {
		// 先頭の要素の有無の返戻
		return Head.Item == NoItem && !HasPendingSpace;
	}

	/**
	 * 末尾が改行かの判定関数
	 * @return 改行なら true
	 */
	bool Canonicalizer::Emitter::IsAtLineStart() const {
		// 末尾が有り，必須改行又は改行で終わる逐語かの返戻
		return !HasPendingSpace && Tail.Item != NoItem && Tail.DoesEndWithNewline;
	}

	/**
	 * 末尾が空白かの判定関数
	 * @return 空白なら true
	 */
	bool Canonicalizer::Emitter::HasTrailingSpace() const {
		// 末尾が有り，空白の葉又は平坦時に空白で終わる境界かの返戻
		return HasPendingSpace || Tail.Item != NoItem && Tail.DoesEndWithSpace;
	}

	/**
	 * 末尾のトークンの役割の判定関数
	 * @param Syntax 構文モデル
	 * @param Role トークンの役割
	 * @param Generated 生成したトークンの種類
	 * @return 何れかの役割に当れば true
	 */
	bool Canonicalizer::Emitter::IsTailRole(const SyntaxModel &Syntax, const Role Role, const GeneratedToken Generated) const {
		// 末尾の役割の返戻
		return !HasPendingSpace && IsEdgeRole(Syntax, *Target, Tail, Role, Generated);
	}

	/**
	 * 末尾の要素の取得関数
	 * @return 末尾の要素
	 */
	Canonicalizer::EdgeItem Canonicalizer::Emitter::GetLast() const {
		// 末尾の要素の返戻
		return HasPendingSpace ? EdgeItem{ NoItem, NoNode, ' ', ' ', false, false, true } : Tail;
	}

	/**
	 * 末尾の直前の要素の取得関数
	 * @return 直前の要素
	 */
	Canonicalizer::EdgeItem Canonicalizer::Emitter::GetPreceding() const {
		// 直前の要素の返戻
		return HasPendingSpace ? Tail : BeforeTail;
	}

	/**
	 * 要素を置く中間表現の取得関数
	 * @return 構築中の中間表現
	 */
	FormatIR &Canonicalizer::Emitter::GetStore() const {
		// 中間表現の返戻
		return *Target;
	}

	/**
	 * 必須改行の追加関数
	 * 計算量：格納済の要素数 N に対し償却 O(1)，領域の伸長時 O(N)
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::Emitter::Break() {
		// 必須改行の追加
		Push(Target->Leaf({}, FormatItemKind::HardBreak), "\n", true);
		// 終了
		return;
	}

	/**
	 * 種類で指定した固定トークンの追加関数
	 * 計算量：固定トークンのテキスト長 L に対し O(L) と出力生成の総費用
	 * @param Syntax 構文モデル
	 * @param Token 規則から生成するトークンの種類
	 * @throws FormatLimitExceeded 出力要素の生成が作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::Emitter::GetText(const SyntaxModel &Syntax, const GeneratedToken Token) {
		// 種類で指定した固定トークンの追加
		const std::string_view Spelling = GetGeneratedSpelling(Syntax, Token);
		// 綴りの無いトークンの場合の終了
		if(Spelling.empty()) return;
		if(Tail.ShouldBreakAfter) Break();
		Push(GeneratedItem(*Target, Spelling, Token), Spelling, false);
		// 終了
		return;
	}

	/**
	 * 言語毎の綴りを持つ語の追加関数
	 * 計算量：語のテキスト長 L に対し O(L) と出力生成の総費用
	 * @param Syntax 構文モデル
	 * @param Meaning 語の意味
	 * @param ShouldSpaceBefore 語の前に空白を置くか
	 * @param ShouldSpaceAfter 語の後に空白を置くか
	 * @throws FormatLimitExceeded 出力要素の生成が作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::Emitter::GetText(
		const SyntaxModel &Syntax,
		const Word Meaning,
		const bool ShouldSpaceBefore,
		const bool ShouldSpaceAfter
	) {
		// 言語毎の綴りを持つ語の追加
		std::string Rendered;
		if(Tail.ShouldBreakAfter) Break();
		else if(ShouldSpaceBefore) Rendered.push_back(' ');
		Rendered.append(Syntax.GetRules().GetSpelling(Meaning));
		if(ShouldSpaceAfter) Rendered.push_back(' ');
		// テキストの無い場合の終了
		if(Rendered.empty()) return;
		Push(Target->Leaf(Rendered, FormatItemKind::Token), Rendered, false);
		// 終了
		return;
	}

	/**
	 * 空白の追加関数
	 * 計算量：固定数の要素の追加に対し償却 O(1)
	 * @throws FormatLimitExceeded 出力要素の生成が作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::Emitter::Space() {
		// 空白の追加
		if(Tail.ShouldBreakAfter) Break();
		else HasPendingSpace = true;
		// 終了
		return;
	}

	/**
	 * 固定の区切記法の追加関数（整形仕様 6.1）
	 * 計算量：区切のテキスト長 L に対し O(L) と出力生成の総費用
	 * @param Text 前後のトークンから規則で決めた区切記法
	 * @throws FormatLimitExceeded 出力要素の生成が作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::Emitter::Separator(const std::string_view Text) {
		// 固定の区切記法の追加
		if(Tail.ShouldBreakAfter) Break();
		Push(Target->Leaf(Text, FormatItemKind::LexicalSeparator), Text, false);
		// 終了
		return;
	}

	/**
	 * 保持する原文の間隙の追加関数（整形仕様 4.4，4.6）
	 * マクロとリテラルの内部の間隙は其の儘コピーし，他の間隙のインデントは配置へ委ねる
	 * 計算量：間隙のテキスト長 L に対し O(L) と出力要素の生成の総費用
	 * @param Boundary 原文の間隙と其の由来
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::Emitter::Gap(const SemanticBoundary &Boundary) {
		// 保持する原文の間隙の追加
		const std::string_view Text = Boundary.Gap;
		if(Boundary.Src == PreservedSrc::MacroTokens || Boundary.Src == PreservedSrc::LiteralContent) {
			// 保つ間隙の無い場合の終了
			if(Text.empty()) return;
			const FormatItemId Item = Target->Leaf(Text, FormatItemKind::Verbatim);
			const bool IsLiteral = Boundary.Src == PreservedSrc::LiteralContent;
			if(IsLiteral) Target->EditDetail(Item).DoesExcludeCodeNewlines = true;
			Push(Item, Text, !IsLiteral && Target->Items[Item].HasNewline);
			// 逐語の間隙をコピーした場合の終了
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
	 * テキストを持たず，前後の宣言の複数行か構造種別の違いで空行を置くかを出力の具体化で決める
	 * 計算量：格納済の要素数 N に対し償却 O(1)，領域の伸長時 O(N)
	 * @param Before 前の宣言
	 * @param After 後の宣言
	 * @param IsForced 常に空行を置くか
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::Emitter::Blank(const NodeId Before, const NodeId After, const bool IsForced) {
		// 空行の前後と強制条件を持つ要素の記録
		const FormatItemId Item = Target->Leaf({}, FormatItemKind::BlankLine);
		FormatItemDetail &Detail = Target->EditDetail(Item);
		Detail.Before = Before;
		Detail.After = After;
		Detail.IsForced = IsForced;
		FlushSpace();
		PushItem(Item);
		// 終了
		return;
	}

	/**
	 * 選択可能な境界の追加関数
	 * 組立の完了時に此のビルダーのグループへ所属させる
	 * 計算量：平坦時のテキスト長 L に対し O(L) と出力生成の総費用
	 * @param Flat 平坦時のテキスト（空白又は密着）
	 * @throws FormatLimitExceeded 出力要素の生成が作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::Emitter::Soft(const std::string_view Flat) {
		// 選択可能な境界の追加
		if(Tail.ShouldBreakAfter) Break();
		else {
			Push(Target->Leaf(Flat, FormatItemKind::SoftBreak), Flat, false);
			HasSoft = true;
		}
		// 終了
		return;
	}

	/**
	 * 組立のグループの展開時又は平坦時だけ出す断片の追加関数（整形仕様 6.1）
	 * グループは組立の完了時に決め，選択可能な境界を出さなかった組立では出さない
	 * 計算量：断片の数 C に対し償却 O(C)
	 * @param Fragment 選んだ形だけで出す断片（トークン・空白・必須改行）
	 * @param IsActivatedWhenFlat 平坦時だけ有効な断片か
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::Emitter::IfBroken(const std::initializer_list<FormatItemId> Fragment, const bool IsActivatedWhenFlat) {
		// 組立のグループの選択形に応じた断片の追加
		const FormatItemId Item =
		Target->Parent(FormatItemKind::IfBroken, std::span<const FormatItemId>(Fragment.begin(), Fragment.size()));
		Target->Items[Item].Group = PendingGroup;
		Target->Items[Item].IsActivatedWhenFlat = IsActivatedWhenFlat;
		HasPendingConditions = true;
		Push(Item, {}, false);
		// 終了
		return;
	}

	/**
	 * 選択形に応じて出す断片のグループの確定関数
	 * 此の組立が出した断片（深さの範囲の中を含み，組立済の子の要素は除く）へグループを付け，グループが無ければ断片を除く
	 * 計算量：辿る要素数 V に対し O(V)
	 * @param IR 要素を置いた中間表現
	 * @param Targets 組立の要素
	 * @param Group 組立のグループ（グループへ所属させなければ `NoGroup`）
	 * @return 保持する要素数
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	size_t Canonicalizer::Emitter::BindConditions(FormatIR &IR, const std::span<FormatItemId> Targets, const uint32_t Group) {
		// 選択形に応じて出す断片のグループの確定
		// 条件付断片のグループを対応付ける作業枠
		struct Frame {
			std::span<FormatItemId> Items;
			FormatItemId Scope = NoItem; // 子列の保持数を更新する深さの範囲の要素
			size_t Next = 0;
			size_t Kept = 0; // グループの確定後に保持する子数
		};
		std::vector<Frame> Stack { { Targets } };
		size_t Count = 0;
		while(!Stack.empty()) {
			Frame &Cur = Stack.back();
			if(Cur.Next == Cur.Items.size()) {
				Count = Cur.Kept;
				const FormatItemId Scope = Cur.Scope;
				if(Scope != NoItem) IR.Items[Scope].Children.Size = static_cast<uint32_t>(Count);
				Stack.pop_back();
				if(!Stack.empty() && Count) Stack.back().Items[Stack.back().Kept++] = Scope;
				continue;
			}
			if(IR.NormalizedSyntax) IR.NormalizedSyntax->Charge(1);
			const FormatItemId Id = Cur.Items[Cur.Next++];
			FormatItem &Item = IR.Items[Id];
			if(Item.Kind == FormatItemKind::IndentScope) {
				Stack.push_back({ std::span<FormatItemId>(IR.Links).subspan(Item.Children.Begin, Item.Children.Size), Id });
				continue;
			}
			if(Item.Kind == FormatItemKind::IfBroken && Item.Group == PendingGroup) {
				if(Group == NoGroup) continue;
				Item.Group = Group;
			}
			Cur.Items[Cur.Kept++] = Id;
		}
		// 保持する要素数の返戻
		return Count;
	}

	/**
	 * 組立済の要素の追加関数
	 * 計算量：匿名の子列の長さ C に対し O(C)，空の組立への所有の移動と単独の要素の追加は償却 O(1)
	 * @param Subtree 組立済の要素（テキストの無い要素は何もしない）
	 * @throws FormatLimitExceeded 出力要素の生成が作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::Emitter::Append(Built &&Subtree) {
		// テキストの無い要素の場合の終了
		if(Subtree.Head.Item == NoItem) return;
		// 後送の終端の後のトークンは別の行へ置き，次の本文が既に持つ先頭改行は其の儘使う
		if(Tail.ShouldBreakAfter && Subtree.Head.FirstChar != '\n') Break();
		// 子の端と改行及びコード境界の状態の反映
		FlushSpace();
		if(Subtree.Item != NoItem) PushItem(Subtree.Item);
		else {
			const bool IsEmpty = !InlineCount && Items.empty();
			if(Target->NormalizedSyntax) Target->NormalizedSyntax->Charge(IsEmpty ? 1 : Subtree.Parts.size());
			if(IsEmpty) Items.swap(Subtree.Parts);
			else if(!Subtree.Parts.empty()) {
				if(Items.empty() && Subtree.Parts.size() <= InlineItems.size() - InlineCount) {
					std::copy(Subtree.Parts.begin(), Subtree.Parts.end(), InlineItems.begin() + InlineCount);
					InlineCount += Subtree.Parts.size();
				} else {
					if(InlineCount) {
						if(Subtree.Parts.size() > Items.max_size() - InlineCount) throw std::length_error("too many emitted items");
						Items.reserve(Subtree.Parts.size() + InlineCount);
						Items.insert(Items.end(), InlineItems.begin(), InlineItems.begin() + InlineCount);
						InlineCount = 0;
					}
					Items.insert(Items.end(), Subtree.Parts.begin(), Subtree.Parts.end());
				}
			}
		}
		if(Head.Item == NoItem) Head = Subtree.Head;
		BeforeTail = Subtree.BeforeTail.Item == NoItem ? Tail : Subtree.BeforeTail;
		Tail = Subtree.Tail;
		HasClauseBoundary = Subtree.HasClauseBoundary;
		HasHardBreak = HasHardBreak || Subtree.HasBreak;
		if(Subtree.HasBreak || Subtree.HasCodeBoundary) IsCodeOpenOnLine = Subtree.IsCodeOpenOnLine;
		HasCodeBoundary = HasCodeBoundary || Subtree.HasCodeBoundary;
		// 終了
		return;
	}

	/**
	 * 後続へ密着させる為の未生成の空白の省略関数
	 */
	void Canonicalizer::Emitter::OmitSpace() {
		// 後続へ密着させる為の未生成の空白の省略
		HasPendingSpace = false;
		// 終了
		return;
	}

	/**
	 * 深さの範囲の開始関数
	 * 以後の要素を範囲の中へ置き，範囲が行を跨ぐ時に内容を１段深くする（整形仕様 6.5）
	 * 計算量：開いて居る範囲数 D と格納済の要素数 N に対し償却 O(1)，領域の伸長時 O(D + N)
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::Emitter::OpenScope() {
		// 深さの範囲の開始
		FlushSpace();
		if(InlineCount) Outer.emplace_back(InlineItems.begin(), InlineItems.begin() + InlineCount);
		else Outer.push_back(std::move(Items));
		InlineCount = 0;
		Items.clear();
		// 終了
		return;
	}

	/**
	 * 深さの範囲の終了関数
	 * 計算量：閉じる範囲内の要素数 C に対し償却 O(C)
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::Emitter::CloseScope() {
		// 開いた範囲の無い場合の終了
		if(Outer.empty()) return;
		FlushSpace();
		const std::span<FormatItemId> Parts = GetItems();
		const FormatItemId Scope = Parts.empty() ? NoItem : Target->Parent(FormatItemKind::IndentScope, Parts);
		InlineCount = 0;
		Items = std::move(Outer.back());
		Outer.pop_back();
		if(Scope != NoItem) PushItem(Scope);
		// 終了
		return;
	}

	/**
	 * 組立の完了関数
	 * 選択可能な境界を出した組立はグループとして包み，ノードのアンカーで包む
	 * 計算量：開いた範囲数 D，範囲に所属する要素数 N に対し O(D + N) と親要素の生成の総費用
	 * @param Node アンカーのノード（無いなら包まない）
	 * @param Group グループの番号（無いならグループにしない）
	 * @return 組立済の要素
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	Canonicalizer::Built Canonicalizer::Emitter::Finish(const NodeId Node, const uint32_t Group) {
		// 深さの範囲と条件付断片の確定
		FlushSpace();
		while(!Outer.empty()) CloseScope();
		if(HasPendingConditions) {
			const size_t Count = BindConditions(*Target, GetItems(), HasSoft ? Group : NoGroup);
			if(InlineCount) InlineCount = Count;
			else Items.resize(Count);
		}
		const std::span<FormatItemId> Parts = GetItems();
		Built Result;
		if(HasSoft && Group != NoGroup) {
			Result.Item = Target->Parent(FormatItemKind::Group, Parts);
			Target->Items[Result.Item].Group = Group;
			Target->Items[Result.Item].Node = Node;
			if(Node != NoNode) Result.Item = Anchored(*Target, Node, Result.Item);
		} else if(Node != NoNode) {
			Result.Item = Target->Parent(FormatItemKind::AnchorMark, Parts);
			Target->Items[Result.Item].Node = Node;
		} else if(Parts.size() == 1) Result.Item = Parts.front();
		else if(InlineCount) Result.Parts.assign(Parts.begin(), Parts.end());
		else Result.Parts = std::move(Items);
		InlineCount = 0;
		Items.clear();
		Result.Head = Head;
		Result.Tail = Tail;
		Result.BeforeTail = BeforeTail;
		Result.HasBreak = HasHardBreak;
		Result.HasClauseBoundary = HasClauseBoundary;
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
