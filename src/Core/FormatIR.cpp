#include "FormatIR.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 要素の字面の取得関数
	 * @param Item 要素
	 * @return 共有する字面の範囲
	 */
	std::string_view FormatIR::TextOf(const FormatItem &Item) const {
		// 字面の範囲の返戻
		return std::string_view(Texts).substr(Item.Text.Begin, Item.Text.Size);
	}

	/**
	 * 要素の子の取得関数
	 * @param Item 要素
	 * @return 子の番号の並び
	 */
	std::span<const FormatItemId> FormatIR::ChildrenOf(const FormatItem &Item) const {
		// 子の番号の範囲の返戻
		return std::span<const FormatItemId>(Links).subspan(Item.Children.Begin, Item.Children.Size);
	}

	/**
	 * 要素の属性の取得関数
	 * @param Detail 要素の属性の番号
	 * @return 要素の属性（無しなら既定値）
	 */
	const FormatItemDetail &FormatIR::DetailOf(const uint32_t Detail) const {
		static const FormatItemDetail None;
		// 属性を持たない要素の既定値と，持つ要素の属性の返戻
		return Detail == NoDetail ? None : Details[Detail];
	}

	/**
	 * 字面の追加関数
	 * @param Text 字面
	 * @return 共有領域の中の範囲
	 * @throws FormatLimitExceeded 共有領域の大きさ又は作業予算が上限を超える場合
	 */
	FormatRange FormatIR::Keep(const std::string_view Text) {
		// 共有する字面の大きさの上限の確認
		if(Text.size() > std::numeric_limits<uint32_t>::max() - Texts.size()) {
			throw FormatLimitExceeded("structured text exceeds formatting limit");
		}
		const FormatRange Range { static_cast<uint32_t>(Texts.size()), static_cast<uint32_t>(Text.size()) };
		// 複写する字句本文のバイト数を計上する
		if(NormalizedSyntax) NormalizedSyntax->Charge(Text.size());
		Texts.append(Text);
		// 追加した字面の範囲の返戻
		return Range;
	}

	/**
	 * 中間表現の構築予算の終了関数
	 * 構築後の読取は期待構文と独立した予算へ計上しない
	 */
	void FormatIR::CloseWork() {
		if(NormalizedSyntax) NormalizedSyntax->WorkAccount()->IsClosed = true;
		// 終了
		return;
	}

	/**
	 * 子の並びの追加関数
	 * @param Children 子の番号の並び
	 * @return 共有領域の中の範囲
	 * @throws FormatLimitExceeded 共有領域の大きさが番号の範囲を超える場合
	 */
	FormatRange FormatIR::Link(const std::span<const FormatItemId> Children) {
		// 子の番号の範囲の上限の確認
		if(Children.size() > std::numeric_limits<uint32_t>::max() - Links.size()) {
			throw FormatLimitExceeded("structured text exceeds formatting limit");
		}
		const FormatRange Range { static_cast<uint32_t>(Links.size()), static_cast<uint32_t>(Children.size()) };
		Links.insert(Links.end(), Children.begin(), Children.end());
		// 追加した子の並びの範囲の返戻
		return Range;
	}

	/**
	 * 要素の追加関数
	 * @param Item 要素
	 * @return 要素の番号
	 * @throws FormatLimitExceeded 要素の数が番号の範囲を超える場合
	 */
	FormatItemId FormatIR::Add(const FormatItem &Item) {
		// 要素の番号の上限の確認
		if(Items.size() >= NoItem) throw FormatLimitExceeded("structured text exceeds formatting limit");
		// 中間表現の要素の１回の生成を１作業単位とする（整形仕様 9.2）
		if(NormalizedSyntax) NormalizedSyntax->Charge(1);
		Items.push_back(Item);
		// 追加した要素の番号の返戻
		return static_cast<FormatItemId>(Items.size() - 1);
	}

	/**
	 * 子を持たない要素の追加関数
	 * @param Text 字面（字句・逐語・選択可能な境界の平坦時の字面）
	 * @param Kind 要素の種類
	 * @return 要素の番号
	 */
	FormatItemId FormatIR::Leaf(const std::string_view Text, const FormatItemKind Kind) {
		FormatItem Item;
		Item.Kind = Kind;
		Item.Text = Keep(Text);
		// 追加した要素の番号の返戻
		return Add(Item);
	}

	/**
	 * 子を持つ要素の追加関数
	 * @param Kind 要素の種類
	 * @param Children 子の番号の並び
	 * @return 要素の番号
	 */
	FormatItemId FormatIR::Parent(const FormatItemKind Kind, const std::span<const FormatItemId> Children) {
		FormatItem Item;
		Item.Kind = Kind;
		Item.Children = Link(Children);
		// 追加した要素の番号の返戻
		return Add(Item);
	}

	/**
	 * 要素の属性の変更関数
	 * @param Id 要素の番号
	 * @return 変更出来る属性（持たなければ既定値で追加した物）
	 * @throws FormatLimitExceeded 属性の数が番号の範囲を超える場合
	 */
	FormatItemDetail &FormatIR::EditDetail(const FormatItemId Id) {
		uint32_t &Detail = Items[Id].Detail;
		// 属性を持たない要素への領域の割当
		if(Detail == NoDetail) {
			if(Details.size() >= NoDetail) throw FormatLimitExceeded("structured text exceeds formatting limit");
			Detail = static_cast<uint32_t>(Details.size());
			Details.emplace_back();
		}
		// 要素の属性の返戻
		return Details[Detail];
	}

	/**
	 * ネストの段数の検査関数（整形仕様 9.2，平坦形・展開形が共有する部分木は一度だけ測る）
	 * 段数は並び・錨等の容れ物を数えず，構造の単位である群の入れ子で測る
	 * 計算量：要素数 N と子の並びの長さ L に対し O(N + L)
	 * @throws FormatLimitExceeded 根からの段数が上限を超える場合（規定した資源上限の見送）
	 */
	void FormatIR::CheckNesting() const {
		// 根の無い中間表現の終了
		if(Root == NoItem) return;
		// 要素毎の最も深い葉迄の群の段数と１の和（０は未計測）と，後行順に測る為の未展開・展開済の印
		std::vector<uint32_t> Heights(Items.size(), 0);
		std::vector<std::pair<FormatItemId, bool>> Stack { { Root, false } };
		// 後行順の走査に依る群の段数の算出
		while(!Stack.empty()) {
			const auto [Id, IsExpanded] = Stack.back();
			Stack.pop_back();
			if(Heights[Id]) continue;
			const std::span<const FormatItemId> Children = ChildrenOf(Items[Id]);
			if(!IsExpanded) {
				Stack.emplace_back(Id, true);
				for(const FormatItemId Child : Children) if(!Heights[Child]) Stack.emplace_back(Child, false);
				continue;
			}
			// 子の計測後の段数の確定
			uint32_t Height = 1;
			for(const FormatItemId Child : Children) Height = std::max(Height, Heights[Child]);
			if(Items[Id].Kind == FormatItemKind::Group) ++Height;
			// 上限を超える段数の見送
			if(Height - 1 > MaxNestingDepth) throw FormatLimitExceeded("nesting exceeds formatting limit");
			Heights[Id] = Height;
		}
		// 終了
		return;
	}

	/**
	 * 解決済の要素の字面の取得関数
	 * @param Item 解決済の要素
	 * @return 共有する字面の範囲
	 */
	std::string_view BrokenIR::TextOf(const ResolvedItem &Item) const {
		// 字面の範囲の返戻
		return std::string_view(Texts).substr(Item.Text.Begin, Item.Text.Size);
	}
} // namespace Core
