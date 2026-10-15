#include "FormatIR.hpp"
#include "NodeId.hpp"
#include "SyntaxRole.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 番号の範囲を指定して空の順位表へ戻す関数
	 * @param Capacity 生成元の番号の上限（０は任意の番号）
	 * @throws std::bad_alloc 順位列を確保出来ない場合
	 * @throws std::length_error 順位列の長さが上限を超える場合
	 */
	void GroupRanks::Reset(const size_t Capacity) {
		Dense.assign(Capacity, NoGroup);
		Sparse.clear();
		Count = 0;
		// 終了
		return;
	}

	/**
	 * グループの順位の登録・更新関数
	 * @param Group 構築時のグループ番号
	 * @param Rank 対応する順位
	 * @throws std::bad_alloc 範囲外の対応を確保出来ない場合
	 * @throws std::length_error 対応表の長さが上限を超える場合
	 */
	void GroupRanks::Set(const uint32_t Group, const uint32_t Rank) {
		if(Group < Dense.size()) {
			Count += Dense[Group] == NoGroup;
			Dense[Group] = Rank;
		} else {
			const std::pair<std::unordered_map<uint32_t, uint32_t>::iterator, bool> Entry = Sparse.insert_or_assign(Group, Rank);
			Count += Entry.second;
		}
		// 終了
		return;
	}

	/**
	 * 登録済の順位の取得関数
	 * @param Group 構築時のグループ番号
	 * @return 登録した順位（未登録なら無）
	 */
	std::optional<uint32_t> GroupRanks::Find(const uint32_t Group) const {
		// 範囲内の順位の返戻
		if(Group < Dense.size()) return Dense[Group] == NoGroup ? std::nullopt : std::optional<uint32_t>(Dense[Group]);
		const std::unordered_map<uint32_t, uint32_t>::const_iterator Entry = Sparse.find(Group);
		// 範囲外の順位の返戻
		return Entry == Sparse.end() ? std::nullopt : std::optional<uint32_t>(Entry->second);
	}

	/**
	 * 必ず存在する順位の取得関数
	 * @param Group 構築時のグループ番号
	 * @return 登録済の順位
	 * @throws std::out_of_range グループが登録されて居ない場合
	 */
	uint32_t GroupRanks::Get(const uint32_t Group) const {
		// 直接参照出来る順位の返戻
		if(Group < Dense.size() && Dense[Group] != NoGroup) return Dense[Group];
		// 範囲外又は未登録の番号は元と同じ表で取得し，同じ例外を伝える
		return Sparse.at(Group);
	}

	/**
	 * 登録済のグループ数の取得関数
	 * @return 登録済のグループ数
	 */
	size_t GroupRanks::GetSize() const noexcept {
		return Count;
	} // 登録数の返戻

	/**
	 * 直接参照する番号の範囲の取得関数
	 * @return 生成元が指定した番号の上限
	 */
	size_t GroupRanks::GetCapacity() const noexcept {
		return Dense.size();
	} // 番号の上限の返戻

	/**
	 * 子の並びの追加関数
	 * 計算量：子の数 C に対し償却 O(C)
	 * @param Children 子の番号の並び
	 * @return 共有領域の中の範囲
	 * @throws FormatLimitExceeded 共有領域の大きさが番号の範囲を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	FormatRange FormatIR::Link(const std::span<const FormatItemId> Children) {
		// 子の番号の範囲の上限の確認
		if(Children.size() > std::numeric_limits<uint32_t>::max() - Links.size()) {
			throw FormatLimitExceeded("structured text exceeds formatting limit");
		}
		const FormatRange Range { static_cast<uint32_t>(Links.size()), static_cast<uint32_t>(Children.size()) };
		LinearizedCount = 0;
		Links.insert(Links.end(), Children.begin(), Children.end());
		// 追加した子の並びの範囲の返戻
		return Range;
	}

	/**
	 * 要素の追加関数
	 * 計算量：格納済の要素数 N に対し償却 O(1)，領域の伸長時 O(N)
	 * @param Item 要素
	 * @return 要素の番号
	 * @throws FormatLimitExceeded 要素の数が番号の範囲を超える場合，又は作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	FormatItemId FormatIR::Add(const FormatItem &Item) {
		// 要素の番号の上限の確認
		if(Items.size() >= NoItem) throw FormatLimitExceeded("structured text exceeds formatting limit");
		// 中間表現の要素の１回の生成を１作業単位とする（整形仕様 9.2）
		if(NormalizedSyntax) NormalizedSyntax->Charge(1);
		LinearizedCount = 0;
		Items.push_back(Item);
		// 追加した要素の番号の返戻
		return static_cast<FormatItemId>(Items.size() - 1);
	}

	/**
	 * 要素の子の取得関数
	 * @param Item 要素
	 * @return 子の番号の並び
	 */
	std::span<const FormatItemId> FormatIR::GetChildrenOf(const FormatItem &Item) const {
		// 子の番号の範囲の返戻
		return std::span<const FormatItemId>(Links).subspan(Item.Children.Begin, Item.Children.Size);
	}

	/**
	 * テキストの追加関数
	 * 計算量：テキスト長 B に対し償却 O(B)
	 * @param Text テキスト
	 * @return 共有領域の中の範囲
	 * @throws FormatLimitExceeded 共有領域の大きさ又は作業予算が上限を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	FormatRange FormatIR::Keep(const std::string_view Text) {
		// 共有するテキストの大きさの上限の確認
		if(Text.size() > std::numeric_limits<uint32_t>::max() - Texts.size()) {
			throw FormatLimitExceeded("structured text exceeds formatting limit");
		}
		const FormatRange Range { static_cast<uint32_t>(Texts.size()), static_cast<uint32_t>(Text.size()) };
		// コピーするトークン本文のバイト数の計上
		if(NormalizedSyntax) NormalizedSyntax->Charge(Text.size());
		Texts.append(Text);
		// 追加したテキストの範囲の返戻
		return Range;
	}

	/**
	 * 要素の属性の取得関数
	 * @param Detail 要素の属性の番号
	 * @return 要素の属性（無なら既定値）
	 */
	const FormatItemDetail &FormatIR::GetDetailOf(const uint32_t Detail) const {
		// 要素の属性の取得
		static constexpr FormatItemDetail None;
		// 属性を持たない要素の既定値と，持つ要素の属性の返戻
		return Detail == NoDetail ? None : Details[Detail];
	}

	/**
	 * 子を持つ要素の追加関数
	 * 計算量：子の数 C に対し償却 O(C)
	 * @param Kind 要素の種類
	 * @param Children 子の番号の並び
	 * @return 要素の番号
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	FormatItemId FormatIR::Parent(const FormatItemKind Kind, const std::span<const FormatItemId> Children) {
		// 子を持つ要素の追加
		FormatItem Item;
		Item.Kind = Kind;
		Item.Children = Link(Children);
		// 追加した要素の番号の返戻
		return Add(Item);
	}

	/**
	 * 宣言区分を未分割の出力順で辿る子の取得関数
	 * 計算量：宣言子数 N に対し O(N)
	 * @param Item 共有部分５個と名前・初期化子の組を持つ宣言区分
	 * @return 空の部分と宣言間の改行を含まない出力順の番号
	 * @throws std::bad_alloc 子列のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	std::vector<FormatItemId> FormatIR::GetDeclChildrenOf(const FormatItem &Item) const {
		// 宣言区分を未分割の出力順で辿る子の取得
		const std::span<const FormatItemId> Parts = GetChildrenOf(Item);
		std::vector<FormatItemId> Result;
		// 宣言子毎の名前・初期化子・区切と共有部分の既知の上限を１度予約する
		const size_t Count = Parts.size() - 5;
		if(Count > (Result.max_size() - 1 >> 2)) throw std::length_error("too many declaration parts");
		Result.reserve((Count << 2) + 1);
		if(Parts[0] != NoItem) Result.push_back(Parts[0]);
		for(size_t Idx = 5; Idx < Parts.size(); ++Idx) { // 共有部分５個の後の宣言子
			if(Idx > 5) Result.push_back(Parts[3]); // 宣言子の区切は共有部分の４番目
			if(const FormatItemId Name = GetChildrenOf(Items[Parts[Idx]])[0]; Name != NoItem) Result.push_back(Name);
		}
		if(Parts[1] != NoItem) Result.push_back(Parts[1]);
		for(size_t Idx = 5; Idx < Parts.size(); ++Idx) {
			const FormatItemId Val = GetChildrenOf(Items[Parts[Idx]])[1];
			if(
				const bool HasVal = Val != NoItem && (Items[Val].Children.Size || Items[Val].Kind != FormatItemKind::Sequence);
				Idx > 5 && HasVal
			) Result.push_back(Parts[3]);
			if(Val != NoItem) Result.push_back(Val);
		}
		if(Parts[2] != NoItem) Result.push_back(Parts[2]);
		// 出力順の子の返戻
		return Result;
	}

	/**
	 * 要素のテキストの取得関数
	 * @param Item 要素
	 * @return 共有するテキストの範囲
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	std::string_view FormatIR::GetTextOf(const FormatItem &Item) const {
		// テキストの範囲の返戻
		return std::string_view(Texts).substr(Item.Text.Begin, Item.Text.Size);
	}

	/**
	 * 解決済の要素のテキストの取得関数
	 * @param Item 解決済の要素
	 * @return 共有するテキストの範囲
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	std::string_view BrokenIR::GetTextOf(const ResolvedItem &Item) const {
		// テキストの範囲の返戻
		return std::string_view(Texts).substr(Item.Text.Begin, Item.Text.Size);
	}

	/**
	 * 共有テキストの範囲を持つ葉の追加関数
	 * 計算量：改行無が確定済なら O(1)，其の他はテキスト長 L に対し O(L)
	 * @param Range テキストの共有範囲
	 * @param Kind 要素の種類
	 * @param IsUnbroken 呼出側が此の範囲の改行無を確定したか
	 * @return 葉の番号
	 * @throws FormatLimitExceeded 要素の生成又は改行の計数が作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	FormatItemId FormatIR::LeafRange(const FormatRange Range, const FormatItemKind Kind, const bool IsUnbroken) {
		// 共有テキストの範囲を持つ葉の追加
		FormatItem Item;
		Item.Kind = Kind;
		Item.Text = Range;
		const std::string_view Text = GetTextOf(Item);
		if(IsUnbroken) {
			Item.HasHeadContent = !Text.empty();
			Item.HasTailContent = !Text.empty();
		} else if(Kind == FormatItemKind::Token || Kind == FormatItemKind::Verbatim) {
			size_t First = Text.size(), Last = Text.size();
			if(NormalizedSyntax) NormalizedSyntax->Charge(Text.size());
			for(size_t Idx = 0; Idx < Text.size(); ++Idx) if(Text[Idx] == '\n') {
				First = std::min(First, Idx);
				Last = Idx;
			}
			Item.HasNewline = First != Text.size();
			Item.HasHeadContent = !Text.empty() && First;
			Item.HasTailContent = Item.HasNewline ? Last + 1 < Text.size() : !Text.empty();
		}
		// 追加した要素の番号の返戻
		return Add(Item);
	}

	/**
	 * 中間表現の構築予算の終了関数
	 * 構築後の読取は期待構文と独立した予算へ計上しない
	 */
	void FormatIR::CloseWork() {
		// 中間表現の構築予算の終了
		if(NormalizedSyntax) NormalizedSyntax->CloseWork();
		// 終了
		return;
	}

	/**
	 * 子を持たない要素の追加関数
	 * 計算量：テキスト長 B に対し償却 O(B)
	 * @param Text テキスト（トークン・逐語・選択可能な境界の平坦時のテキスト）
	 * @param Kind 要素の種類
	 * @return 要素の番号
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	FormatItemId FormatIR::Leaf(const std::string_view Text, const FormatItemKind Kind) {
		// テキストを１度控えた葉の返戻
		return LeafRange(Keep(Text), Kind);
	}

	/**
	 * 要素の属性の変更関数
	 * 計算量：格納済の属性数 D に対し償却 O(1)，領域の伸長時 O(D)
	 * @param Id 要素の番号
	 * @return 変更出来る属性（持たなければ既定値で追加した物）
	 * @throws FormatLimitExceeded 属性の数が番号の範囲を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	FormatItemDetail &FormatIR::EditDetail(const FormatItemId Id) {
		// 要素の属性の変更
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
	 * 共通の生成形式から決定単位と直接依存を構築する関数
	 * 計算量：要素数 N，グループ数 G，条件付境界数 C と直接依存数 D に対し O(N + (G + C) log(G + 1) + D)
	 * @param GroupCapacity 生成元が既に持つグループ番号の上限（０は任意の番号を対応付ける）
	 * @throws FormatLimitExceeded 構築が作業予算又はグループのネストの上限を超える場合
	 * @throws std::out_of_range 条件付表現又は依存が存在しないグループを参照する場合
	 * @throws std::bad_alloc 決定単位又は直接依存のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	void FormatIR::BuildDecisions(const size_t GroupCapacity) {
		// 決定単位と直接依存の構築
		LinearizedCount = 0;
		size_t AtomCount = 0;
		Decls.clear();
		BlankRefs.clear();
		// １つのグループの決定と走査上の位置を，同じ記録で共有する
		struct UnitRecord {
			FormatDecision Decision; // グループの生成形式と直接依存を持つ決定単位
			size_t Entry = 0; // 最後の出現の開始順位
			uint32_t Parent = NoGroup; // 最後の出現を囲むグループ
		};
		// 生成元の連続した番号は直接参照し，任意の番号を持つ中間表現だけハッシュで対応付ける
		std::pmr::monotonic_buffer_resource Storage;
		std::pmr::vector<UnitRecord> Units(&Storage);
		Units.reserve(GroupCapacity ? GroupCapacity : Items.size());
		std::pmr::vector<uint32_t> UnitIndices(GroupCapacity, NoGroup, &Storage);
		std::pmr::unordered_map<uint32_t, uint32_t> SparseUnits(&Storage);
		if(!GroupCapacity) SparseUnits.reserve(Items.size());
		// 登録済のグループの記録の取得
		const auto UnitAt = [&](const uint32_t Group) -> UnitRecord & {
			// 生成元の番号又は任意の番号から記録を引いた結果の返戻
			return Units.at(GroupCapacity ? UnitIndices.at(Group) : SparseUnits.at(Group));
		};
		// 初出のグループだけ記録を追加し，再出現では同じ記録を使う
		const auto RegisterUnit = [&](const uint32_t Group) -> UnitRecord & {
			uint32_t &Index = GroupCapacity ? UnitIndices.at(Group) : SparseUnits.try_emplace(Group, NoGroup).first->second;
			if(Index == NoGroup) {
				Index = static_cast<uint32_t>(Units.size());
				Units.emplace_back();
			}
			// 同じグループの記録の返戻
			return Units[Index];
		};
		// 決定単位と構文境界の対応付けの作業枠
		struct Frame {
			FormatItemId Id;
			uint32_t Enclosing = NoGroup; // 外側のグループ（NoGroup は所属無）
			uint32_t Cond = NoGroup; // 条件付断片のグループ（NoGroup は無条件）
			NodeId Control = NoNode; // 見出と本体を対応付ける制御構文（NoNode は対象無）
			uint32_t Next = 0;
			bool HasEntered = false;
			bool IsBody = false; // 制御本体の内側か
			uint32_t Segment = 0; // 必須境界で区切った区間
			uint32_t Height = 0; // 此の要素の下で完了したグループの最大の深さ
			uint32_t ParentGroup = NoGroup; // 此のグループを囲むグループ（NoGroup は親無）
			size_t Entry = 0; // 此の要素の開始順位
			uint32_t Continuation = NoGroup; // 所属する継続単位（NoGroup は所属無）
			int ContinuationDepth = 0; // 継続単位へ入った時の括弧の深さ
			std::vector<FormatItemId> DeclParts = {};
			uint32_t ChainPrefix = NoGroup; // 直に囲む連鎖本体のグループ
		};
		// 見出の所属区間とグループ列を，同じ所有者の記録から引く
		struct HeaderRecord {
			std::vector<uint32_t> Groups; // 同じ見出の区間に属するグループの出現順
			uint32_t Segment = 0; // 必須境界で区切った区間
		};
		std::pmr::unordered_map<NodeId, HeaderRecord> Headers(&Storage);
		std::pmr::unordered_map<NodeId, uint32_t> AnnotationGroups(&Storage);
		std::pmr::unordered_map<uint32_t, std::pmr::vector<uint32_t>> UnitMembers(&Storage);
		Headers.reserve(Details.size());
		AnnotationGroups.reserve(Details.size());
		int Depth = 0;
		uint32_t Segment = 0;
		std::pmr::unordered_set<uint64_t> GroupLinks(&Storage);
		std::vector<uint32_t> Closed;
		Closed.reserve(GroupCapacity ? GroupCapacity : Items.size());
		size_t NextEntry = 0;
		std::vector<std::pair<uint32_t, uint32_t>> LeftDependencies, RightDependencies;
		std::vector<uint32_t> PendingConditions;
		uint32_t PrevBoundary = NoGroup;
		// 条件付断片の左の境界は直ちに対応付け，右の境界は次の通常境界に到着した時に１度対応付ける
		const auto RecordBoundary = [&](const uint32_t Group, const uint32_t Cond, const bool IsHard) -> void {
			if(Cond != NoGroup) {
				if(NormalizedSyntax) NormalizedSyntax->Charge(1);
				if(
					const std::pair<uint32_t, uint32_t> Edge { PrevBoundary, Cond };
					PrevBoundary != NoGroup && (LeftDependencies.empty() || LeftDependencies.back() != Edge)
				) LeftDependencies.push_back(Edge);
				if(PendingConditions.empty() || PendingConditions.back() != Cond) PendingConditions.push_back(Cond);
			} else if(IsHard || Group != NoGroup) {
				if(!IsHard) for(const uint32_t Pending : PendingConditions) {
					if(NormalizedSyntax) NormalizedSyntax->Charge(1);
					RightDependencies.emplace_back(Group, Pending);
				}
				PendingConditions.clear();
				PrevBoundary = IsHard ? NoGroup : Group;
			}
			// 終了
			return;
		};
		std::vector<Frame> Pending { { Root } };
		while(!Pending.empty()) {
			if(NormalizedSyntax) NormalizedSyntax->Charge(1);
			Frame &Cur = Pending.back();
			const FormatItem &Item = Items[Cur.Id];
			if(!Cur.HasEntered) {
				Cur.HasEntered = true;
				Cur.Segment = Segment;
				// 線形化と同じ出現順で原子数を集計し，予約量の為だけに木を走査し直さない
				const size_t Added = Item.Kind == FormatItemKind::AnchorMark || Item.Kind == FormatItemKind::IndentScope ?
				2 :
				Item.Kind == FormatItemKind::Sequence || Item.Kind == FormatItemKind::Group || Item.Kind == FormatItemKind::IfBroken ||
				Item.Kind == FormatItemKind::DeclarationSplit ? 0 : 1;
				if(Added > std::numeric_limits<size_t>::max() - AtomCount) throw std::length_error("too many format atoms");
				AtomCount += Added;
				const bool HasCodeNewline =
				Item.HasNewline && !GetDetailOf(Item.Detail).DoesExcludeTextNewlines && !GetDetailOf(Item.Detail).DoesExcludeCodeNewlines;
				switch(Item.Kind) {
				case FormatItemKind::AnchorMark:
					if(
						!NormalizedSyntax || (
							NormalizedSyntax->Is(Item.Node, Role::BodyOwner) || NormalizedSyntax->Is(Item.Node, Role::If) ||
							NormalizedSyntax->Is(Item.Node, Role::ElseClause)
						)
					) {
						Cur.Control = Item.Node;
						Cur.IsBody = false;
						if(NormalizedSyntax && !NormalizedSyntax->Is(Item.Node, Role::Control)) Cur.ChainPrefix = NoGroup;
					}
					break;
				case FormatItemKind::Group:
					{
						if(
							Cur.Enclosing != NoGroup && Cur.Enclosing != Item.Group &&
							GroupLinks.insert(static_cast<uint64_t>(Cur.Enclosing) << 32 | Item.Group).second
						) RegisterUnit(Cur.Enclosing).Decision.Children.push_back(Item.Group);
						FormatDecision &Unit = RegisterUnit(Item.Group).Decision;
						const bool IsFirst = Unit.Group == NoGroup;
						Unit.Group = Item.Group;
						// 継続括弧と内側のコンテナの測定を同じ単位へ所属させる
						if(Cur.Continuation != NoGroup && Cur.Continuation != Item.Group && Cur.Cond == NoGroup) {
							UnitMembers[Cur.Continuation].push_back(Item.Group);
						}
						Cur.Entry = NextEntry++;
						Cur.ParentGroup = Cur.Enclosing;
						if(Item.Operation) switch(*Item.Operation) {
						case FormOperation::BraceBody:
							Unit.Procedure = DecisionProcedure::BraceBody;
							break;
						case FormOperation::Continuation:
							Unit.Procedure = DecisionProcedure::Continuation;
							break;
						}
						Unit.IsChainBody = GetDetailOf(Item.Detail).IsChainBody;
						if(Unit.Procedure == DecisionProcedure::Continuation) {
							Unit.Prefix = Cur.Enclosing;
							Unit.IsContinuationList = NormalizedSyntax && NormalizedSyntax->Is(Item.Node, Role::BareTuple);
							Unit.IsContinuationType = NormalizedSyntax && NormalizedSyntax->Is(Item.Node, Role::TypeSyntax);
							if(NormalizedSyntax && Item.Node != NoNode) {
								if(const NodeId Parent = NormalizedSyntax->GetParentOf(Item.Node); NormalizedSyntax->Is(Parent, Role::Assignment)) {
									if(NormalizedSyntax->HasField(Item.Node, ChildRole::Type)) AnnotationGroups[Parent] = Item.Group;
									else if(NormalizedSyntax->HasField(Item.Node, ChildRole::Right)) if(
										const std::pmr::unordered_map<NodeId, uint32_t>::const_iterator Annotation = AnnotationGroups.find(Parent);
										Annotation != AnnotationGroups.end()
									) {
										Unit.Annotation = Annotation->second;
										UnitMembers[Item.Group].push_back(Annotation->second);
										UnitAt(Annotation->second).Decision.IsContinuationContent = true;
									}
								}
							}
						}
						Cur.Enclosing = Item.Group;
						if(Unit.Procedure == DecisionProcedure::BraceBody) {
							if(IsFirst) Unit.Prefix = Cur.ChainPrefix;
							Cur.ChainPrefix = Unit.IsChainBody ? Item.Group : NoGroup;
							HeaderRecord &Header = Headers[GetDetailOf(Item.Detail).BodyParent];
							if(Header.Segment == Segment) {
								std::pmr::vector<uint32_t> &Members = UnitMembers[Item.Group];
								Members.insert(Members.end(), Header.Groups.begin(), Header.Groups.end());
							}
							Header.Groups.clear();
							Cur.IsBody = true;
						} else if(NormalizedSyntax && NormalizedSyntax->Is(Item.Node, Role::BodyWrapper)) Cur.ChainPrefix = NoGroup;
						Unit.ExpansionDependency = GetDetailOf(Item.Detail).ExpansionDependency;
						if(Unit.Procedure == DecisionProcedure::Continuation) {
							Cur.Continuation = Item.Group;
							Cur.ContinuationDepth = Depth;
						}
					}
					break;
				case FormatItemKind::IfBroken:
					Cur.Cond = Item.Group;
					break;
				case FormatItemKind::DeclarationSplit:
					Cur.DeclParts = GetDeclChildrenOf(Item);
					if(NormalizedSyntax) NormalizedSyntax->Charge((Item.Children.Size << 1) - 7);
					break;
				}
				switch(Item.Kind) {
				case FormatItemKind::SoftBreak:
					if(Cur.Continuation != NoGroup && Cur.Enclosing != Cur.Continuation && Depth == Cur.ContinuationDepth) {
						UnitAt(Cur.Enclosing).Decision.IsContinuationContent = true;
					}
					RecordBoundary(Cur.Enclosing, Cur.Cond, false);
					break;
				case FormatItemKind::HardBreak:
				case FormatItemKind::BlankLine:
				case FormatItemKind::OwnedRegion:
					RecordBoundary(NoGroup, Cur.Cond, true);
					break;
				case FormatItemKind::Verbatim:
					if(HasCodeNewline || Cur.Cond != NoGroup) RecordBoundary(NoGroup, Cur.Cond, HasCodeNewline);
					break;
				case FormatItemKind::Token:
				case FormatItemKind::Space:
				case FormatItemKind::LexicalSeparator:
					if(
						Cur.Continuation != NoGroup && Cur.Enclosing != Cur.Continuation && Cur.Cond == NoGroup && Depth == Cur.ContinuationDepth &&
						NormalizedSyntax &&
						(NormalizedSyntax->Is(Item.Node, Role::MemberAccessLead) || NormalizedSyntax->Is(Item.Node, Role::MemberAccessTail))
					) UnitAt(Cur.Enclosing).Decision.IsContinuationMember = true;
					if(Cur.Cond != NoGroup) RecordBoundary(NoGroup, Cur.Cond, false);
					if(Item.Kind == FormatItemKind::Token && Cur.Cond == NoGroup) switch(Item.Generated) {
					case GeneratedToken::CallOpen:
						++Depth;
						break;
					case GeneratedToken::CallClose:
						--Depth;
						break;
					default:
						if(Item.Node != NoNode && NormalizedSyntax) Depth += NormalizedSyntax->GetBracketLeadDelta(Item.Node);
						break;
					}
					break;
				}
				if(
					Cur.Cond == NoGroup && (
						Item.Kind == FormatItemKind::HardBreak || Item.Kind == FormatItemKind::BlankLine || Item.Kind == FormatItemKind::OwnedRegion ||
						Item.Kind == FormatItemKind::Verbatim && HasCodeNewline
					)
				) ++Segment;
			}
			if(
				const size_t ChildCount = Item.Kind == FormatItemKind::DeclarationSplit ? Cur.DeclParts.size() : Item.Children.Size;
				Cur.Next < ChildCount
			) {
				const FormatItemId Child =
				Item.Kind == FormatItemKind::DeclarationSplit ? Cur.DeclParts[Cur.Next++] : Links[Item.Children.Begin + Cur.Next++];
				Frame Next { Child, Cur.Enclosing, Cur.Cond, Cur.Control, 0, false, Cur.IsBody };
				Next.Continuation = Cur.Continuation;
				Next.ContinuationDepth = Cur.ContinuationDepth;
				Next.ChainPrefix = Cur.ChainPrefix;
				Pending.push_back(Next);
				continue;
			}
			if(Item.Kind == FormatItemKind::Group) {
				UnitRecord &Finished = UnitAt(Item.Group);
				Finished.Entry = Cur.Entry;
				Finished.Parent = Cur.ParentGroup;
				Finished.Decision.UnitRank = static_cast<uint32_t>(Closed.size());
				++Cur.Height;
				Closed.push_back(Item.Group);
				if(Cur.Control != NoNode && !Cur.IsBody && Cur.Segment == Segment) {
					HeaderRecord &Header = Headers[Cur.Control];
					if(Header.Segment != Segment) Header.Groups.clear();
					Header.Segment = Segment;
					Header.Groups.push_back(Item.Group);
				}
			}
			if(Item.Kind == FormatItemKind::DeclarationSplit) Decls.push_back(Cur.Id);
			if(Item.Kind == FormatItemKind::BlankLine) {
				const FormatItemDetail &Detail = GetDetailOf(Item.Detail);
				if(Detail.Before != NoNode) BlankRefs.insert(Detail.Before);
				if(Detail.After != NoNode) BlankRefs.insert(Detail.After);
			}
			const uint32_t Height = Cur.Height;
			if(Height > MaxNestingDepth) throw FormatLimitExceeded("nesting exceeds formatting limit");
			Pending.pop_back();
			if(!Pending.empty()) Pending.back().Height = std::max(Pending.back().Height, Height);
		}
		// 共有グループの最後の閉じを採り，走査順の儘で基準順位を詰める
		Decisions.clear();
		Decisions.reserve(Units.size());
		GroupRanks &RankOf = DecisionOf;
		RankOf.Reset(GroupCapacity);
		for(uint32_t Pos = 0; Pos < Closed.size(); ++Pos) {
			if(NormalizedSyntax) NormalizedSyntax->Charge(1);
			FormatDecision &Unit = UnitAt(Closed[Pos]).Decision;
			if(Unit.UnitRank != Pos) continue;
			RankOf.Set(Unit.Group, static_cast<uint32_t>(Decisions.size()));
			Unit.Rank = static_cast<uint32_t>(Decisions.size());
			Unit.UnitOwner = Unit.Rank;
			Unit.UnitRank = Unit.Rank;
			Decisions.push_back(std::move(Unit));
		}
		// 移動した記録もスカラーの順位を保つ為，構築時の直接参照を使って再識別を避ける
		const auto RankAt = [&](const uint32_t Group) -> uint32_t {
			// 構築時の記録が持つ確定した順位の返戻
			return UnitAt(Group).Decision.Rank;
		};
		// 見出と本体，又は括弧外の式と継続括弧の直接手順への対応付け
		for(const std::pair<const uint32_t, std::pmr::vector<uint32_t>> &Entry : UnitMembers) {
			const uint32_t Owner = RankAt(Entry.first);
			for(const uint32_t Member : Entry.second) {
				if(NormalizedSyntax) NormalizedSyntax->Charge(1);
				Decisions[RankAt(Member)].UnitOwner = Owner;
			}
		}
		// 相互依存するグループの代表順位の照会
		const auto OwnerOf = [&](const uint32_t Rank) -> uint32_t {
			uint32_t Owner = Rank;
			while(Decisions[Owner].UnitOwner != Owner) {
				if(NormalizedSyntax) NormalizedSyntax->Charge(1);
				Owner = Decisions[Owner].UnitOwner;
			}
			for(uint32_t Member = Rank; Member != Owner;) {
				if(NormalizedSyntax) NormalizedSyntax->Charge(1);
				const uint32_t Next = Decisions[Member].UnitOwner;
				Decisions[Member].UnitOwner = Owner;
				Member = Next;
			}
			// 直接の代表順位の返戻
			return Owner;
		};
		// コンテナの判断を先に行う為の，括弧外のグループとラッパーの間のコンテナの決定単位への対応付け
		for(const std::pair<const uint32_t, std::pmr::vector<uint32_t>> &Entry : UnitMembers) {
			const uint32_t Owner = RankAt(Entry.first);
			if(Decisions[Owner].Procedure != DecisionProcedure::Continuation) continue;
			for(const uint32_t Member : Entry.second) {
				if(NormalizedSyntax) NormalizedSyntax->Charge(1);
				if(UnitAt(Member).Entry < UnitAt(Entry.first).Entry) continue;
				const uint32_t TargetOwner = OwnerOf(Owner);
				for(uint32_t Parent = UnitAt(Member).Parent; Parent != NoGroup; Parent = UnitAt(Parent).Parent) {
					if(NormalizedSyntax) NormalizedSyntax->Charge(1);
					const uint32_t ParentOwner = OwnerOf(RankAt(Parent));
					if(ParentOwner == TargetOwner) break;
					Decisions[ParentOwner].UnitOwner = TargetOwner;
				}
			}
		}
		// 隣接する条件付トークンの直接依存の記録
		std::vector<std::pair<uint32_t, uint32_t>> ConditionalEdges;
		// 隣接する条件付断片の直接依存の登録
		const auto AddConditionalDependency = [&](const std::pair<uint32_t, uint32_t> &Edge) -> void {
			if(NormalizedSyntax) NormalizedSyntax->Charge(1);
			if(
				const uint32_t Receiver = RankAt(Edge.first), Provider = RankAt(Edge.second);
				Decisions[Provider].Procedure != DecisionProcedure::BraceBody && Receiver < Provider
			) ConditionalEdges.emplace_back(Receiver, Provider);
			// 終了
			return;
		};
		// 右から左，左から右の基準順を保ち，無関係な通常境界を走査し直さない
		for(size_t Idx = RightDependencies.size(); Idx--;) AddConditionalDependency(RightDependencies[Idx]);
		for(const std::pair<uint32_t, uint32_t> &Edge : LeftDependencies) AddConditionalDependency(Edge);
		// 祖先の断片が子の幅を変える相互依存は，直接の決定単位として束ねる
		if(!ConditionalEdges.empty()) {
			std::vector<uint32_t> OuterRank(Decisions.size(), 0);
			for(const FormatDecision &Unit : Decisions) {
				if(NormalizedSyntax) NormalizedSyntax->Charge(1);
				const uint32_t Owner = OwnerOf(Unit.Rank);
				OuterRank[Owner] = std::max(OuterRank[Owner], Unit.Rank);
			}
			for(const std::pair<uint32_t, uint32_t> &Edge : ConditionalEdges) {
				if(NormalizedSyntax) NormalizedSyntax->Charge(1);
				if(UnitAt(Decisions[Edge.second].Group).Entry > UnitAt(Decisions[Edge.first].Group).Entry) continue;
				uint32_t Receiver = OwnerOf(Edge.first);
				const uint32_t Provider = OwnerOf(Edge.second);
				// 祖先迄の直接の親子も同じ単位へ含め，途中の単位へ相互依存を残さない
				while(Receiver != OwnerOf(Provider)) {
					if(NormalizedSyntax) NormalizedSyntax->Charge(1);
					const uint32_t Parent = OwnerOf(RankAt(UnitAt(Decisions[OuterRank[Receiver]].Group).Parent));
					const uint32_t Owner =
					Decisions[Receiver].Procedure != DecisionProcedure::Ordinary && Decisions[Parent].Procedure == DecisionProcedure::Ordinary ?
					Receiver :
					Parent;
					Decisions[Owner == Receiver ? Parent : Receiver].UnitOwner = Owner;
					OuterRank[Owner] = std::max(OuterRank[Receiver], OuterRank[Parent]);
					Receiver = Owner;
				}
			}
		}
		// 親子と条件付トークンの直接依存及び完了通知先の対応付け
		std::pmr::unordered_set<uint64_t> Registered(&Storage);
		// 決定単位間の直接依存と後続先の登録
		const auto Depend = [&](const uint32_t ReceiverRank, const uint32_t ProviderRank) -> void {
			const uint32_t Receiver = OwnerOf(ReceiverRank), Provider = OwnerOf(ProviderRank);
			// 同じ単位の中の依存を省く場合の終了
			if(Receiver == Provider) return;
			if(Registered.insert(static_cast<uint64_t>(Receiver) << 32 | Provider).second) {
				if(NormalizedSyntax) NormalizedSyntax->Charge(1);
				Decisions[Receiver].Dependencies.push_back(Provider);
				Decisions[Provider].Successors.push_back(Receiver);
			}
			// 終了
			return;
		};
		// 後行順の列は役目を終えた為，子を持つ決定の順位の列へ再使用する
		std::vector<uint32_t> &ParentUnits = Closed;
		ParentUnits.clear();
		for(FormatDecision &Member : Decisions) {
			if(NormalizedSyntax) NormalizedSyntax->Charge(1);
			Member.UnitOwner = OwnerOf(Member.Rank);
			FormatDecision &Owner = Decisions[Member.UnitOwner];
			Owner.Members.push_back(Member.Rank);
			Owner.UnitRank = std::min(Owner.UnitRank, Member.Rank);
			// 昇順の最初のメンバで代表順位が確定し，後のメンバで小さく為らない
			Member.UnitRank = Owner.UnitRank;
			if(Member.Prefix != NoGroup) Member.Prefix = RankAt(Member.Prefix);
			if(Member.Annotation != NoGroup) Member.Annotation = RankAt(Member.Annotation);
			if(!Member.Children.empty()) ParentUnits.push_back(Member.Rank);
			if(Member.ExpansionDependency != NoGroup) {
				Member.ExpansionDependency = RankAt(Member.ExpansionDependency);
				Depend(Member.Rank, Member.ExpansionDependency);
			}
		}
		// 外側の条件付断片の対応付けと後行順を越える待機の分類
		for(const std::pair<uint32_t, uint32_t> &Edge : ConditionalEdges) {
			if(NormalizedSyntax) NormalizedSyntax->Charge(1);
			Depend(Edge.first, Edge.second);
		}
		// 後行順で遅れる子と直接の親の依存の対応付け
		for(const uint32_t Parent : ParentUnits) {
			FormatDecision &Unit = Decisions[Parent];
			if(NormalizedSyntax) NormalizedSyntax->Charge(Unit.Children.size() + 1);
			for(uint32_t &Child : Unit.Children) {
				Child = RankAt(Child);
				if(
					const FormatDecision &ChildUnit = Decisions[Decisions[Child].UnitOwner];
					Unit.UnitRank < ChildUnit.UnitRank || !ChildUnit.Dependencies.empty()
				) Depend(Unit.Rank, Child);
			}
		}
		LinearizedCount = AtomCount;
		// 終了
		return;
	}
} // namespace Core
