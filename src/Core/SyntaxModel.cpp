#include "SyntaxModel.hpp"
#include "ControlFlow.hpp"
#include "SyntaxGeneration.hpp"
#include "SyntaxRole.hpp"
#include "../Profile/UnicodeNormalization.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include "../Util/LayoutTypes.hpp"
#include "../Util/ParserAllocator.hpp"
#include "../Util/TextEdit.hpp"
#include "../Util/WorkSort.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * テキストを持たない境界のコンストラクタ
	 * 計算量：O(1)
	 * @param Before 境界の後に続く子の番号
	 * @param Kind 境界の種類
	 */
	SemanticBoundary::SemanticBoundary(const uint32_t Before, const BoundaryKind Kind) : Before(Before), Kind(Kind) {}

	/**
	 * 保持するテキストと由来を持つ境界のコンストラクタ
	 * 計算量：O(1)（引数へコピーする場合はテキスト長に比例する）
	 * @param Before 境界の後に続く子の番号
	 * @param Kind 境界の種類
	 * @param Gap 保持する間隙のテキスト
	 * @param Src 保持する間隙の由来
	 */
	SemanticBoundary::SemanticBoundary(
		const uint32_t Before,
		const BoundaryKind Kind,
		std::string Gap,
		const PreservedSrc Src
	) : Before(Before), Kind(Kind), Src(Src), Gap(std::move(Gap)) {}

	/**
	 * リソースの同一性の判定関数
	 * 計算量：O(1)
	 * @param Other 比較するリソース
	 * @return 同じ実体なら true
	 */
	bool SyntaxModel::NodeMemory::do_is_equal(const std::pmr::memory_resource &Other) const noexcept {
		return this == &Other;
	} // 同一性の返戻

	/**
	 * システムのブロックの確保と計数関数
	 * 計算量：要求バイト数 B に対し O(B)
	 * @param Bytes 要求バイト数
	 * @param Alignment 要求する整列
	 * @return システムから確保したブロック
	 * @throws std::bad_alloc システムのメモリを確保出来ない場合
	 */
	void *SyntaxModel::NodeMemory::do_allocate(const size_t Bytes, const size_t Alignment) {
		void *const Result = std::pmr::new_delete_resource()->allocate(Bytes, Alignment);
		++Blocks;
		// システムのブロックの返戻
		return Result;
	}

	/**
	 * システムのブロックの解放関数
	 * 計算量：O(1)
	 * @param Pointer 確保したブロック
	 * @param Bytes 確保時のバイト数
	 * @param Alignment 確保時の整列
	 */
	void SyntaxModel::NodeMemory::do_deallocate(void *const Pointer, const size_t Bytes, const size_t Alignment) {
		std::pmr::new_delete_resource()->deallocate(Pointer, Bytes, Alignment);
		// 終了
		return;
	}

	/**
	 * 同期した共有ブロックの取得関数
	 * 同じ所有者を持つノード表の子列が共有リソースを使う為，確保だけを同期する
	 * 計算量：要求バイト数 B に対し O(B)
	 * @param Count 要求バイト数
	 * @param Alignment 要求する整列
	 * @return 世代の所有者が保持する領域
	 * @throws std::bad_alloc メモリを確保出来ない場合
	 * @throws std::system_error 同期に失敗した場合
	 */
	void *SyntaxModel::NodeMemory::Allocate(const size_t Count, const size_t Alignment) {
		const std::lock_guard<std::mutex> Lock(Mutex);
		void *const Result = Resource.allocate(Count, Alignment);
		++Requests;
		Bytes += Count;
		// 整列済の領域の返戻
		return Result;
	}

	/**
	 * ノード表のブロックとマップを要求量だけ確保する関数
	 * ノード表は追加したブロックを破棄迄保つ為，プールの予約を要しない
	 * 小さな所有領域毎の最初のチャンクの余りを保持しない
	 * 計算量：要求バイト数 B に対し O(B)
	 * @param Count 要求バイト数
	 * @param Alignment 要求する整列
	 * @return システムから確保した領域
	 * @throws std::bad_alloc メモリを確保出来ない場合
	 * @throws std::system_error 同期に失敗した場合
	 */
	void *SyntaxModel::NodeMemory::AllocateNodes(const size_t Count, const size_t Alignment) {
		const std::lock_guard<std::mutex> Lock(Mutex);
		void *const Result = do_allocate(Count, Alignment);
		++Requests;
		Bytes += Count;
		// 要求量に合った領域の返戻
		return Result;
	}

	/**
	 * 子列のリソースの同一性の判定関数
	 * @param Other 比較するリソース
	 * @return 同じ実体なら true
	 */
	bool SyntaxModel::NodeMemory::ChildMemory::do_is_equal(const std::pmr::memory_resource &Other) const noexcept {
		return this == &Other;
	} // 同一性の返戻

	/**
	 * 同期した共有メモリからの子列の確保関数
	 * 計算量：要求バイト数 B に対し O(B)
	 * @param Bytes 要求バイト数
	 * @param Alignment 要求する整列
	 * @return 所有者が保持する領域
	 * @throws std::bad_alloc メモリを確保出来ない場合
	 * @throws std::system_error 確保の同期に失敗した場合
	 */
	void *SyntaxModel::NodeMemory::ChildMemory::do_allocate(const size_t Bytes, const size_t Alignment) {
		return Owner.Allocate(Bytes, Alignment);
	} // 子列の領域の返戻

	/**
	 * 子列の解放を所有者の破棄迄遅延する関数
	 * @param Pointer 解放対象の領域（所有者が保持する為に未使用）
	 * @param Bytes 確保時のバイト数（未使用）
	 * @param Alignment 確保時の整列（未使用）
	 */
	void SyntaxModel::NodeMemory::ChildMemory::do_deallocate(
		[[maybe_unused]] void *const Pointer,
		[[maybe_unused]] const size_t Bytes,
		[[maybe_unused]] const size_t Alignment
	) {
		return;
	} // 終了

	/**
	 * 子列のメモリの所有者を保持する生成関数
	 * @param Owner ノード表の共有メモリ
	 */
	SyntaxModel::NodeMemory::ChildMemory::ChildMemory(NodeMemory &Owner) : Owner(Owner) {} // 所有者の設定

	/**
	 * 構文のフラグの初期化関数
	 * 計算量：O(1)
	 */
	SyntaxFacts::SyntaxFacts() noexcept : DoesDiscardBinding(false),
	CanReassignExistingBinding(false),
	NeedsAngleParens(false),
	HasFalseName(true),
	HasTrueName(true),
	DoesAbsorbLeadingComment(false),
	IsKeywordName(false),
	DoesObserveText(false),
	DoesRequireInlineCode(false),
	DoesPreserveQuoteForm(false) {} // フラグの既定値の設定

	/**
	 * 子列を指定メモリへ移すノードの生成関数
	 * @param Storage ノードの破棄迄生存するリソース
	 * @param Node 移すノード
	 * @throws std::bad_alloc 子列のメモリを確保出来ない場合
	 * @throws std::system_error 確保の同期に失敗した場合
	 */
	SyntaxNode::SyntaxNode(std::pmr::memory_resource *const Storage, SyntaxNode &&Node) : OrderFirst(Node.OrderFirst),
	OrderLast(Node.OrderLast),
	Parent(Node.Parent),
	Idx(Node.Idx),
	Kind(Node.Kind),
	Lexeme(std::move(Node.Lexeme)),
	Facts(std::move(Node.Facts)),
	Children(std::move(Node.Children), Storage),
	Fields(Node.Fields) {} // 全フィールドと子列の移動

	/**
	 * 自身を共有メモリの上流にする生成関数
	 * チャンク毎の予約数だけを抑え，固定サイズのプールへ入るブロックの上限は処理系の既定値を保つ
	 * 計算量：O(1)
	 */
	SyntaxModel::NodeMemory::NodeMemory() : Resource(std::pmr::pool_options{ 64, 0 }, this), Children(*this) {} // 子列のプールの上流の設定

	/**
	 * 子の役割のビットの取得関数
	 * @param Role 子の役割
	 * @return 子の役割のビット（文法に無い役割は０）
	 */
	uint64_t SyntaxModel::GetFieldBit(const ChildRole Role) const {
		// 子の役割のビットの取得
		const uint64_t Bit = Role == ChildRole::None ? 0 : uint64_t{ 1 } << static_cast<uint8_t>(Role);
		// 文法に在る役割のビットだけの返戻
		return Bit & ChildRoleMask;
	}

	/**
	 * 子の並びの取得関数
	 * 計算量：O(1)
	 * @param Id ノードの識別子
	 * @return 子の並び（ノードが無ければ空）
	 */
	std::span<const NodeId> SyntaxModel::GetChildrenOf(const NodeId Id) const {
		// 子の無い対象の返戻
		if(Id == NoNode || Id >= GetNodeCount()) return {};
		if(Validation) {
			const ValidationRecord &Node = Validation->Records[Id], &Next = Validation->Records[Id + 1];
			// 縮約した所有列の返戻
			return std::span<const NodeId>(Validation->Children).subspan(Node.ChildrenBegin, Next.ChildrenBegin - Node.ChildrenBegin);
		}
		// 現在の子の並びの返戻
		return Nodes[Id].Children;
	}

	/**
	 * ノードの取得関数
	 * @param Id ノードの識別子
	 * @return ノード
	 * @throws std::out_of_range 識別子がノード表に無い場合
	 */
	const SyntaxNode &SyntaxModel::GetNode(const NodeId Id) const {
		// ノード全体の取得は兄弟の編集で振り直す `Idx` も公開する
		// ノードの返戻
		return Nodes.at(Id);
	}

	/**
	 * 選択したメンバ区切の兄弟位置の取得関数
	 * @param Id メンバ区切の対象
	 * @return 元の兄弟位置
	 * @throws std::out_of_range 対象が位置の表に無い場合
	 */
	uint32_t SyntaxModel::GetValidationSiblingIndex(const NodeId Id) const {
		// 選択時に保持した兄弟位置の返戻
		return Validation ? Validation->SiblingIndexes.at(Id) : Nodes.at(Id).Idx;
	}

	/**
	 * 構造照合用のノードの取得関数
	 * @param Id ノードの識別子
	 * @return 所有者の本文と子列を参照するノードの値
	 * @throws std::out_of_range 識別子がノード表に無い場合
	 */
	ValidationNode SyntaxModel::GetValidationNode(const NodeId Id) const {
		if(!Validation) {
			const SyntaxNode &Node = Nodes.at(Id);
			// 照会結果の返戻
			return { Node.Kind, Node.Fields, Node.Lexeme, Node.Children };
		}
		const ValidationRecord &Node = Validation->Records.at(Id), &Next = Validation->Records.at(static_cast<size_t>(Id) + 1);
		// 照会結果の返戻
		return {
			Node.Kind,
			Node.Fields,
			std::string_view(Validation->Lexemes).substr(Node.LexemeBegin, Next.LexemeBegin - Node.LexemeBegin),
			std::span<const NodeId>(Validation->Children).subspan(Node.ChildrenBegin, Next.ChildrenBegin - Node.ChildrenBegin)
		};
	} // 所有する表の読取値の返戻

	/**
	 * 構造照合用の意味境界の取得関数
	 * @param Id ノードの識別子
	 * @return 子の位置順の境界（所有者の破棄迄有効）
	 * @throws std::out_of_range 識別子がノード表に無い場合
	 */
	std::span<const SemanticBoundary> SyntaxModel::GetValidationBoundaries(const NodeId Id) const {
		// 照会結果の返戻
		if(!Validation) return Nodes.at(Id).Facts.Boundaries.GetView();
		const ValidationRecord &Node = Validation->Records.at(Id), &Next = Validation->Records.at(static_cast<size_t>(Id) + 1);
		// 照会結果の返戻
		return std::span<const SemanticBoundary>(Validation->Boundaries).subspan(
			Node.BoundariesBegin,
			Next.BoundariesBegin - Node.BoundariesBegin
		);
	} // 所有する境界の返戻

	/**
	 * 構造照合用の所有領域の有無の判定関数
	 * @param Id ノードの識別子
	 * @return 埋込の所有領域を持てば true
	 */
	bool SyntaxModel::HasValidationRegion(const NodeId Id) const {
		// 照会結果の返戻
		return Validation ? !Validation->Regions.empty() && Id < GetNodeCount() && Validation->Regions[Id] : GetRegionOf(Id) != nullptr;
	} // 所有領域の有無の返戻

	/**
	 * 構造照合用の所有領域が空かの判定関数
	 * @param Id ノードの識別子
	 * @return 所有領域が在り其の断片が空なら true
	 */
	bool SyntaxModel::IsValidationRegionEmpty(const NodeId Id) const {
		// 照会結果の返戻
		if(Validation) return !Validation->Regions.empty() && Id < GetNodeCount() && Validation->Regions[Id] == 2;
		const OwnedRegion *const Region = GetRegionOf(Id);
		// 照会結果の返戻
		return Region && Region->Code->GetFragments().empty();
	} // 空の所有領域かの返戻

	/**
	 * 親の取得関数
	 * 現在の接続を読み，親を持たないノードでは無を返す
	 * 計算量：O(1)
	 * @param Id ノードの識別子
	 * @return 親（根と無いノードは無）
	 */
	NodeId SyntaxModel::GetParentOf(const NodeId Id) const {
		// 現在の親の返戻
		return Id < GetNodeCount() ? Validation ? Validation->Records[Id].Parent : Nodes[Id].Parent : NoNode;
	}

	/**
	 * 子の取得関数
	 * @param Id ノードの識別子
	 * @param Idx 子の位置
	 * @return 子（位置が範囲の外なら無）
	 */
	NodeId SyntaxModel::GetChild(const NodeId Id, const uint32_t Idx) const {
		// 子の取得
		const std::span<const NodeId> Children = GetChildrenOf(Id);
		// 位置の子の返戻
		return Idx < Children.size() ? Children[Idx] : NoNode;
	}

	/**
	 * 語の演算子へ融合する英字かの判定関数
	 * @param Char 境界の字
	 * @return 英字なら true
	 */
	bool SyntaxRules::IsFusionLetter(const unsigned char Char) {
		// 英字かの返戻
		return std::isalpha(Char);
	}

	/**
	 * 生成形式の役割に対応する部品の取得関数
	 * 計算量：固定の生成部品の表の探索は O(1)
	 * @param Role 子の役割
	 * @return 部品（無ければ無）
	 */
	const FormPart *CanonicalForm::GetPartOf(const FormPartRole Role) const {
		// 役割の一致する部品の返戻
		for(const FormPart &Part : Parts) if(Part.Role == Role) return &Part;
		// 部品の無い事の返戻
		return nullptr;
	}

	/**
	 * 所有者の役割に形式を適用出来るかの判定関数
	 * @param Roles 所有者の構文上の役割
	 * @return 適用出来れば true
	 */
	bool CanonicalForm::CanApplyTo(const RoleSet &Roles) const {
		// 定義した役割と一致するかの返戻
		return (ApplicableRoles & Roles).any();
	}

	/**
	 * 前の兄弟の取得関数
	 * @param Id ノードの識別子
	 * @return 前の兄弟（先頭の子と根は無）
	 * @throws std::out_of_range 親の識別子がノード表に無い場合
	 */
	NodeId SyntaxModel::GetPrevSibling(const NodeId Id) const {
		// 前の兄弟の取得
		const NodeId Parent = GetParentOf(Id);
		// 先頭かの判定はノードの兄弟位置を使う
		// 前の兄弟の返戻
		return Parent == NoNode || !Nodes[Id].Idx ? NoNode : GetNode(Parent).Children[Nodes[Id].Idx - 1];
	}

	/**
	 * 変更する境界の並びの取得関数
	 * @return 子の番号順の境界（持たなければ空の並びを確保した物）
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 */
	std::vector<SemanticBoundary> &SemanticBoundaryList::Edit() {
		// 変更する境界の並びの取得
		if(!Entries) Entries = std::make_unique<std::vector<SemanticBoundary>>();
		// 境界の並びの返戻
		return *Entries;
	}

	/**
	 * 子の数の取得関数
	 * @param Id ノードの識別子
	 * @return 子の数（ノードが無ければ０）
	 */
	uint32_t SyntaxModel::GetChildCount(const NodeId Id) const {
		// 子の数の返戻
		return static_cast<uint32_t>(GetChildrenOf(Id).size());
	}

	/**
	 * コンストラクタ
	 * @param Def 文法の構文・意味の固定の定義
	 * @param Mask 文法に在る子の役割
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 */
	SyntaxModel::SyntaxModel(const SyntaxRules &Def, const uint64_t Mask) : ChildRoleMask(Mask), Def(&Def) {
		// 終了
		return;
	}

	/**
	 * 役割を持つノードかの判定関数
	 * @param Id ノードの識別子
	 * @param Wanted 役割
	 * @return ノードの種別が其の役割を持てば true
	 */
	bool SyntaxModel::Is(const NodeId Id, const Role Wanted) const {
		// 種別の役割の返戻
		const KindDef *const Kind = Id < GetNodeCount() ? Validation ? Validation->Records[Id].Kind.Def : Nodes[Id].Kind.Def : nullptr;
		return Kind && Kind->Roles.test(static_cast<size_t>(Wanted));
	}

	/**
	 * 何れかの役割を持つノードかの判定関数
	 * 計算量：役割の候補数 R に対し O(R)
	 * @param Id ノードの識別子
	 * @param Wanted 役割の候補
	 * @return ノードの種別が候補の何れかの役割を持てば true
	 */
	bool SyntaxModel::IsAny(const NodeId Id, const std::initializer_list<Role> Wanted) const {
		// 種別を持たないノードの返戻
		if(Id >= Nodes.size() || !Nodes[Id].Kind.Def) return false;
		const RoleSet &Roles = Nodes[Id].Kind.Def->Roles;
		// 候補の何れかの役割の有無の返戻
		return std::ranges::any_of(
			Wanted,
			[&Roles](const Role Candidate) -> bool {
				// 候補の役割の有無の返戻
				return Roles.test(static_cast<size_t>(Candidate));
			}
		);
	}

	/**
	 * 文法の構文・意味の固定の定義の取得関数
	 * @return 投影側が文法から定めた構文・意味の固定の定義（コメントの記法の定義を含む）
	 */
	const SyntaxRules &SyntaxModel::GetRules() const {
		// 固定の定義の返戻
		return *Def;
	}

	/**
	 * 制約の有無の判定関数
	 * @param Kind 制約の種類
	 * @return ノードが其の制約を持てば true
	 */
	bool SyntaxFacts::Has(const ConstraintKind Kind) const {
		// 制約のビットの有無の返戻
		return Constraints >> static_cast<uint8_t>(Kind) & 1;
	}

	/**
	 * フィールドの子の取得関数
	 * 計算量：探索する子の数 C に対し O(C)
	 * @param Id ノードの識別子
	 * @param Role 子の役割
	 * @return 最初の其のフィールドの子（無ければ無）
	 * @throws std::out_of_range 照会する子の識別子がノード表に無い場合
	 */
	NodeId SyntaxModel::GetFieldChild(const NodeId Id, const ChildRole Role) const {
		// フィールドのビット
		if(const uint64_t Bit = GetFieldBit(Role); Bit) for(const NodeId Child : GetChildrenOf(Id)) if(GetNode(Child).Fields & Bit) {
			// 其のフィールドの子の返戻
			return Child;
		}
		// 其のフィールドの子が無い事の返戻
		return NoNode;
	}

	/**
	 * ========== ノード・トークン・構文事実の取得 ==========
	 * トークンのテキストの取得関数
	 * @param Id ノードの識別子
	 * @return 出力するトークンのテキスト（ノードが無ければ空）
	 */
	std::string_view SyntaxModel::GetLexemeOf(const NodeId Id) const {
		// トークンのテキストの返戻
		if(!Validation) return Id < Nodes.size() ? std::string_view(Nodes[Id].Lexeme) : std::string_view();
		return Id < GetNodeCount() ? GetValidationNode(Id).Lexeme : std::string_view();
	}

	/**
	 * 後の兄弟の取得関数
	 * @param Id ノードの識別子
	 * @return 後の兄弟（最後の子と根は無）
	 */
	NodeId SyntaxModel::GetNextSibling(const NodeId Id) const {
		// 後の兄弟の取得
		const NodeId Parent = GetParentOf(Id);
		// 後の兄弟の返戻
		return Parent == NoNode ? NoNode : GetChild(Parent, Nodes[Id].Idx + 1);
	}

	/**
	 * ノード表の大きさの取得関数
	 * @return 除いたノードを含むノードの数（識別子の上限）
	 */
	size_t SyntaxModel::GetNodeCount() const {
		// ノードの数の返戻
		return Validation ? Validation->Records.size() - 1 : Nodes.size();
	}

	/**
	 * 根の取得関数
	 * @return 根の識別子
	 */
	NodeId SyntaxModel::GetRoot() const {
		// 根の返戻
		return Root;
	}

	/**
	 * 親の中のフィールドかの判定関数
	 * @param Id ノードの識別子
	 * @param Role 子の役割
	 * @return ノードが親の中で其のフィールドを持てば `true`
	 */
	bool SyntaxModel::HasField(const NodeId Id, const ChildRole Role) const {
		// フィールドのビットマスクに其の役割を含むかの返戻
		return Id < GetNodeCount() && (Validation ? Validation->Records[Id].Fields : Nodes[Id].Fields) & GetFieldBit(Role);
	}

	/**
	 * 合成した行先のキーの取得関数
	 * @param Anchor 元のアンカー
	 * @param Side 元の置方
	 * @param IsInline 文中の記録か（空に畳む本体への移譲で置方が分かれる）
	 * @return キー
	 */
	uint64_t RewriteTrace::GetMoveKey(const NodeId Anchor, const AnchorSide Side, const bool IsInline) {
		// アンカー・置方・文中かを詰めたキーの返戻
		return uint64_t{ Anchor } << 3 | uint64_t{ static_cast<uint8_t>(Side) } << 1 | uint64_t{ IsInline };
	}

	/**
	 * 固定定義の役割列と呼出名の導入の候補の構築関数
	 * 計算量：定義する役割の総数 R に対し O(R)
	 * @throws std::bad_alloc 役割列を確保出来ない場合
	 * @throws std::length_error 役割列の上限を超える場合
	 */
	void KindDef::InitRoleList() {
		// 固定定義の役割の収集
		RoleList.clear();
		HasCallableIntroductionRole = false;
		for(size_t Idx = 0; Idx < Roles.size(); ++Idx) if(Roles.test(Idx)) {
			const Role Wanted = static_cast<Role>(Idx);
			RoleList.push_back(Wanted);
			// 名前の収集で読む役割の和を，記号の固定定義へ１度だけ纏める
			switch(Wanted) {
			case Role::AnnotatedDefaultParam:
			case Role::AnnotatedParam:
			case Role::AssignmentOperation:
			case Role::ClassLike:
			case Role::DeclWithType:
			case Role::DeleteStmt:
			case Role::FlowBoundary:
			case Role::GenericParams:
			case Role::ImportedMemberBinding:
			case Role::ImportedModuleBinding:
			case Role::ImportedPathBinding:
			case Role::MacroDef:
			case Role::MacroRuleDef:
			case Role::MemberTypeRank:
			case Role::NamedParamKind:
			case Role::NamespaceDef:
			case Role::Param:
			case Role::ShortVarDecl:
			case Role::TypeParamList:
			case Role::TypedDecl:
			case Role::VarDeclarator:
			case Role::VarSpec:
			case Role::VariadicParamDecl:
				HasCallableIntroductionRole = true;
				break;
			default:
				break;
			}
		}
		// 終了
		return;
	}

	/**
	 * 語の意味の生成する綴りの取得関数
	 * @param Meaning 語の意味
	 * @return 文法が其の意味に定める最初の綴り（綴りを持たなければ空）
	 */
	std::string_view SyntaxRules::GetSpelling(const Word Meaning) const {
		// 語の意味の生成する綴りの取得
		const std::vector<std::string_view> &Spellings = Words[static_cast<size_t>(Meaning)];
		// 最初の綴りの返戻
		return Spellings.empty() ? std::string_view() : Spellings.front();
	}

	/**
	 * 字が演算子の融合に関わるかの判定関数
	 * 計算量：登録した記号の総長 L に対し O(L)
	 * @param Char 境界の字
	 * @return 演算子の融合に関われば true
	 */
	bool SyntaxRules::CanFuseOperatorChar(const unsigned char Char) const {
		// 融合する字の返戻
		return OperatorFusionChars.find(Char) != std::string_view::npos;
	}

	/**
	 * 代入のトークンと隣接する字が融合するかの判定関数
	 * 計算量：登録した記号の総長 L に対し O(L)
	 * @param Before 代入の前の字
	 * @param After 代入の後の字
	 * @return 何れかの側で融合すれば true
	 */
	bool SyntaxRules::CanFuseAssignment(const unsigned char Before, const unsigned char After) const {
		// 左右の何れかで融合するかの返戻
		return Before && AssignmentFusionBefore.find(Before) != std::string_view::npos ||
		After && AssignmentFusionAfter.find(After) != std::string_view::npos;
	}

	/**
	 * 範囲の区切同士が融合するかの判定関数
	 * 計算量：登録した記号の総長 L に対し O(L)
	 * @param Before 前のトークンの最後の字
	 * @param After 後のトークンの最初の字
	 * @return 別の範囲トークンへ融合すれば true
	 */
	bool SyntaxRules::CanFuseRangePair(const unsigned char Before, const unsigned char After) const {
		// 範囲の区切同士が融合するかの判定
		const char Pair[] { static_cast<char>(Before), static_cast<char>(After) };
		// 融合する範囲の組の返戻
		return FusedRangePairs.contains(std::string_view(Pair, 2));
	}

	/**
	 * 名前へ融合する英数字かの判定関数
	 * @param Char 境界の字
	 * @return 英数字なら true
	 */
	bool SyntaxRules::IsFusionAlnum(const unsigned char Char) {
		// 英数字かの返戻
		return std::isalnum(Char);
	}

	/**
	 * 演算子名へ融合する英字又は下線かの判定関数
	 * @param Char 境界の字
	 * @return 英字又は下線なら true
	 */
	bool SyntaxRules::IsFusionNameStart(const unsigned char Char) {
		// 演算子名を始める字かの返戻
		return IsFusionLetter(Char) || Char == '_';
	}

	/**
	 * 隣接するトークンが次元値等へ融合するかの判定関数
	 * 計算量：登録した記号の総長 L に対し O(L)
	 * @param Last 前のトークンの最後の字
	 * @param After 後のトークンの綴り
	 * @return 融合する組なら true
	 */
	bool SyntaxRules::CanFuseDimensionTokens(const unsigned char Last, const std::string_view After) {
		// 後のトークンの無い事の返戻
		if(After.empty()) return false;
		const unsigned char First = static_cast<unsigned char>(After.front());
		const unsigned char Second = After.size() > 1 ? static_cast<unsigned char>(After[1]) : '\0';
		// 名前・数の字かの判定の定義
		const auto IsNameChar = [](const unsigned char Char) -> bool {
			// 英数字・下線・連結符・非 ASCII・エスケープの返戻
			return std::isalnum(Char) || Char == '_' || Char == '-' || Char > 0X7F || Char == '\\';
		};
		// 後のトークンが数で始まるか
		const bool IsNumStart = std::isdigit(First) || First == '.' && std::isdigit(Second);
		// 融合する組かの返戻
		return IsNameChar(Last) &&
		(IsNameChar(First) || First == '%' || IsNumStart || First == '+' && (std::isdigit(Second) || Second == '.')) ||
		(Last == '+' || Last == '.') && IsNumStart || (Last == '#' || Last == '@' || Last == '$' || Last == '%') && IsNameChar(First) ||
		Last == ':' && First == ':' || Last == '|' && First == '|' ||
		std::string_view("~|^$*").find(static_cast<char>(Last)) != std::string_view::npos && First == '=' ||
		Last == '/' && (First == '*' || First == '/') || Last == '*' && First == '/' || Last == '<' && First == '!';
	}

	/**
	 * 波括弧で囲まれて居ない本体を包めるかの判定関数
	 * @param IsBraced 本体に波括弧が既に有るか
	 * @return 包めるなら true
	 */
	bool CanonicalForm::CanWrapBody(const bool IsBraced) const {
		// ラッパーを必要とする形式かの返戻
		return Operation == FormOperation::BraceBody && Cond == FormCond::UnbracedBody && !IsBraced;
	}

	/**
	 * 展開した並びの閉じの前にカンマを生成出来るかの判定関数
	 * @param IsBroken 並びを複数行へ展開したか
	 * @param HasLastElement 閉じの直前に要素が在るか
	 * @param Closer 閉じのトークン
	 * @return カンマを生成出来れば true
	 */
	bool CanonicalForm::CanAddTrailingComma(const bool IsBroken, const bool HasLastElement, const unsigned char Closer) const {
		// 展開形の有効な閉じの前に要素が在るかの返戻
		return Operation == FormOperation::TrailingComma && Cond == FormCond::BrokenListWithLastElement &&
		GetPartOf(FormPartRole::LastElement) && GetPartOf(FormPartRole::Close) && IsBroken && HasLastElement &&
		ClosingLexemes.find(Closer) != std::string_view::npos;
	}

	/**
	 * 空の実引数の並びを除いて結果の種別へ組み直せるかの判定関数
	 * @param IsEmpty 実引数の並びが空か
	 * @return 結果の種別を持つ形式で，実引数が空なら true
	 */
	bool CanonicalForm::CanOmitEmptyArgs(const bool IsEmpty) const {
		// 空の実引数の並びを除いて結果の種別へ組み直せるかの判定
		const FormPart *const Result = GetPartOf(FormPartRole::Result);
		// 空の実引数を持ち，結果の種別を定義した形式かの返戻
		return Operation == FormOperation::OmitEmptyArgs && Cond == FormCond::EmptyArgs && IsEmpty && Result && Result->Def;
	}

	/**
	 * 生成形式の並びの取得関数
	 * @return 記号毎の生成形式の定義（無ければ空）
	 */
	std::span<const CanonicalForm> CanonicalFormList::GetView() const {
		// 定義の並びの返戻
		return Defs ? std::span<const CanonicalForm>(*Defs) : std::span<const CanonicalForm>();
	}

	/**
	 * 境界の並びの取得関数
	 * @return 子の番号順の境界（持たなければ空）
	 */
	std::span<const SemanticBoundary> SemanticBoundaryList::GetView() const {
		// 境界の並びの返戻
		return Entries ? std::span<const SemanticBoundary>(*Entries) : std::span<const SemanticBoundary>();
	}

	/**
	 * ノードの生成形式の取得関数
	 * 計算量：固定の生成形式の表の探索は O(1)
	 * @param Id ノードの識別子
	 * @param Operation 共通操作
	 * @return ノードの種別が其の操作に使える生成形式（無ければ無）
	 */
	const CanonicalForm *SyntaxModel::GetFormOf(const NodeId Id, const FormOperation Operation) const {
		// 種別の定義を持たないノードに生成形式が無い事の返戻
		if(Id >= GetNodeCount()) return nullptr;
		const KindDef *const Def = Validation ? Validation->Records[Id].Kind.Def : Nodes[Id].Kind.Def;
		// 種別を持たないノードの返戻
		if(!Def) return nullptr;
		// 投影済の構文事実が持つ生成形式の探索
		const CanonicalFormList Forms =
		Validation ? Validation->Forms.empty() ? CanonicalFormList() : Validation->Forms[Id] : Nodes[Id].Facts.Forms;
		// 操作と所有者の役割に適用出来る形式の返戻
		for(const CanonicalForm &Form : Forms.GetView()) if(Form.Operation == Operation && Form.CanApplyTo(Def->Roles)) return &Form;
		// 使える生成形式の無い事の返戻
		return nullptr;
	}

	/**
	 * 生成形式の並びの先頭の取得関数
	 * @return 並びの先頭
	 */
	std::span<const CanonicalForm>::iterator CanonicalFormList::begin() const {
		// 並びの先頭の返戻
		return GetView().begin();
	}

	/**
	 * 境界の並びの先頭の取得関数
	 * @return 並びの先頭
	 */
	std::span<const SemanticBoundary>::iterator SemanticBoundaryList::begin() const {
		// 並びの先頭の返戻
		return GetView().begin();
	}

	/**
	 * 生成形式の並びの終端の取得関数
	 * @return 並びの終端
	 */
	std::span<const CanonicalForm>::iterator CanonicalFormList::end() const {
		// 並びの終端の返戻
		return GetView().end();
	}

	/**
	 * 境界の並びの終端の取得関数
	 * @return 並びの終端
	 */
	std::span<const SemanticBoundary>::iterator SemanticBoundaryList::end() const {
		// 並びの終端の返戻
		return GetView().end();
	}

	/**
	 * 子の前の意味を持つ境界の取得関数
	 * 計算量：境界の数 B に対し O(log(B + 1))
	 * @param Idx 境界の後に続く子の番号
	 * @return 境界（記録が無ければ nullptr）
	 */
	const SemanticBoundary *SyntaxFacts::GetBoundaryBefore(const uint32_t Idx) const {
		// 子の前の意味を持つ境界の取得
		const std::span<const SemanticBoundary> Recorded = Boundaries.GetView();
		const std::span<const SemanticBoundary>::iterator Found = std::lower_bound(
			Recorded.begin(),
			Recorded.end(),
			Idx,
			[](const SemanticBoundary &Boundary, const uint32_t Wanted) -> bool {
				// 子の番号の大小の返戻
				return Boundary.Before < Wanted;
			}
		);
		// 境界の記録の返戻
		return Found == Recorded.end() || Found->Before != Idx ? nullptr : &*Found;
	}

	/**
	 * 部分木の端のトークンの役割の判定関数
	 * 葉の全文を照合し，リテラル本文の端の記号を独立したトークンと扱わない
	 * @param Edge 部分木の端のトークンの葉
	 * @param Wanted 判定する役割
	 * @return 端のトークンが役割を持てば true
	 */
	bool SyntaxModel::IsEdgeRole(const LexicalEdge &Edge, const Role Wanted) const {
		// 端のトークンの役割の返戻
		return IsTokenRole(Edge.Leaf, Wanted);
	}

	/**
	 * 部分木の端の文字が持つ文字の接続の役割の判定関数
	 * 括弧を除いた後の接続の判定に用い，文の区切トークンの判定とは区別する
	 * @param Edge 部分木の端の葉と文字
	 * @param Wanted 判定する役割
	 * @return 端の文字の綴りが役割を持てば true
	 */
	bool SyntaxModel::IsEdgeCharRole(const LexicalEdge &Edge, const Role Wanted) const {
		// 端の文字を持たない場合の返戻
		if(Edge.Leaf == NoNode || !Def) return false;
		const char Char = static_cast<char>(Edge.Char);
		// １文字の綴りに対応する役割の返戻
		return Def->HasTokenRole(std::string_view(&Char, 1), Wanted);
	}

	/**
	 * 投影又は生成形式で確定したトークンの役割の判定関数
	 * @param Id トークンのノード
	 * @param Wanted 役割
	 * @return 入力又は生成形式で確認した役割を持てば true
	 */
	bool SyntaxModel::IsTokenRole(const NodeId Id, const Role Wanted) const {
		// ノード又は記法が無い場合と，部分木をトークンとして扱わない事の返戻
		if(
			Id >= GetNodeCount() || !Def || !(Validation ? Validation->Records[Id].Kind.Def : Nodes[Id].Kind.Def) ||
			!GetChildrenOf(Id).empty()
		) return false;
		// 補間の区切の意味は，接頭辞が定める長さに依らず投影済の定義を読む
		const KindDef *const Delimiter = Validation ? Validation->Records[Id].DelimiterDef : Nodes[Id].Facts.DelimiterDef;
		// 投影済の区切の役割の返戻
		if(Delimiter) return Delimiter->Roles.test(static_cast<size_t>(Wanted));
		const KindDef *const Token = Validation ? Validation->Records[Id].TokenDef : Nodes[Id].Facts.TokenDef;
		// 投影又は生成形式から得たトークンの役割の返戻
		return Token && Token->Roles.test(static_cast<size_t>(Wanted));
	}

	/**
	 * 使用可能な記法かの判定関数（整形仕様 3.2）
	 * 計算量：登録した記法の数 N に対し O(N)
	 * @param Notation コメントの記法
	 * @return 文法が記法を読むなら true
	 */
	bool NotationRules::IsAvailable(const CommentNotation Notation) const {
		// 使用可能な記法の有無の返戻
		return std::ranges::find(AvailableNotations, Notation, &AvailableNotation::Notation) != AvailableNotations.end();
	}

	/**
	 * ドキュメントの記法のドキュメント形式の取得関数（整形仕様 3.2）
	 * 計算量：登録した記法の数 N に対し O(N)
	 * @param Notation コメントの記法
	 * @return 記法をドキュメントに使う時の入力のドキュメント形式（ドキュメントに使わない記法は無）
	 */
	std::optional<DocumentFormId> NotationRules::GetDocFormOf(const CommentNotation Notation) const {
		// ドキュメントの記法のドキュメント形式の取得
		const std::vector<DocumentNotation>::const_iterator Found =
		std::ranges::find(DocumentForms, Notation, &DocumentNotation::Notation);
		// 記法のドキュメント形式の返戻（ドキュメントに使わない記法は無）
		return Found == DocumentForms.end() ? std::nullopt : std::optional<DocumentFormId>(Found->Form);
	}

	/**
	 * 囲む本体のドキュメント記法かの判定関数
	 * 計算量：登録した記法の数 N に対し O(N)
	 * @param Notation コメントの記法
	 * @return 囲む本体を説明する記法なら true
	 */
	bool NotationRules::IsInnerDoc(const CommentNotation Notation) const {
		// 入力記法の説明する側の返戻
		const std::vector<DocumentNotation>::const_iterator Found =
		std::ranges::find(DocumentForms, Notation, &DocumentNotation::Notation);
		return Found != DocumentForms.end() && Found->IsInner;
	}

	/**
	 * 用途と説明する側の採用記法の取得関数（整形仕様 3.2, 7.6）
	 * 計算量：登録した記法の数 N に対し O(N)
	 * @param Use コメントの用途
	 * @param IsInner 囲む本体のドキュメントを選ぶか
	 * @return 用途の採用記法（其の用途の記法を揃えない文法は無）
	 */
	const CommentOutputForm *NotationRules::GetAdoptedFormOf(const CommentUse Use, const bool IsInner) const {
		// 用途の採用記法の取得
		const std::vector<CommentOutputForm>::const_iterator Found = std::ranges::find_if(
			OutputForms.Forms,
			[Use, IsInner](const CommentOutputForm &Form) -> bool {
				// 用途と説明する側が一致するかの返戻
				return Form.Use == Use && Form.IsInner == IsInner;
			}
		);
		// 用途の採用記法の返戻（無ければ無）
		return Found == OutputForms.Forms.end() ? nullptr : &*Found;
	}

	/**
	 * コンストラクタ（記号毎の定義を参照）
	 * @param Forms 文法の定義が所有する記号毎の生成形式（参照の間は変更しない）
	 */
	CanonicalFormList::CanonicalFormList(const std::vector<CanonicalForm> &Forms) : Defs(&Forms) {}

	/**
	 * コンストラクタ（空の並びは領域を確保しない）
	 * 計算量：O(1)（境界と間隙の並びを移動する）
	 * @param Boundaries 子の番号順の境界
	 * @throws std::bad_alloc 境界の並びを持つメモリを確保出来ない場合
	 */
	SemanticBoundaryList::SemanticBoundaryList(std::vector<SemanticBoundary> &&Boundaries) : Entries(
		Boundaries.empty() ? nullptr : std::make_unique<std::vector<SemanticBoundary>>(std::move(Boundaries))
	) {}

	/**
	 * コピーコンストラクタ
	 * 計算量：コピーする境界の数 B と間隙の総長 L に対し O(B + L)
	 * @param Other コピーする境界の並び
	 * @throws std::bad_alloc 境界と間隙のコピー先のメモリを確保出来ない場合
	 * @throws std::length_error コピーする境界又は間隙の長さが上限を超える場合
	 */
	SemanticBoundaryList::SemanticBoundaryList(const SemanticBoundaryList &Other) : Entries(
		Other.Entries ? std::make_unique<std::vector<SemanticBoundary>>(*Other.Entries) : nullptr
	) {}

	/**
	 * コピー代入演算子
	 * 計算量：コピーする境界の数 B と間隙の総長 L に対し O(B + L)
	 * @param Other コピーする境界の並び
	 * @return 自身
	 * @throws std::bad_alloc 境界と間隙のコピー先のメモリを確保出来ない場合
	 * @throws std::length_error コピーする境界又は間隙の長さが上限を超える場合
	 */
	SemanticBoundaryList &SemanticBoundaryList::operator=(const SemanticBoundaryList &Other) {
		// 境界の並びと間隙のコピー
		Entries = Other.Entries ? std::make_unique<std::vector<SemanticBoundary>>(*Other.Entries) : nullptr;
		// 自身の返戻
		return *this;
	}

	/**
	 * 制約の追加関数
	 * @param Kind 制約の種類
	 */
	void SyntaxFacts::Add(const ConstraintKind Kind) {
		// 制約の追加
		Constraints |= static_cast<uint16_t>(1 << static_cast<uint8_t>(Kind));
		// 終了
		return;
	}

	/**
	 * 名前付のノードかの判定関数
	 * @return 共通のノード種別が構文なら true
	 */
	bool SyntaxKind::IsNamed() const {
		// 名前付かの返戻
		return Def->IsNamed;
	}

	/**
	 * 名前付のノードかの判定関数
	 * @param Id ノードの識別子
	 * @return 名前付のノードなら true（ノードが無ければ false）
	 */
	bool SyntaxModel::IsNamed(const NodeId Id) const {
		// 名前付かの返戻
		return Id < GetNodeCount() && (Validation ? Validation->Records[Id].Kind : Nodes[Id].Kind).IsNamed();
	}

	/**
	 * 生成するノードの種別と構文事実の定義の取得関数
	 * 計算量：テキストの長さ L に対し期待値 O(L)，明示した生成形式は O(1)
	 * @param Generated 生成するノード
	 * @return 生成形式が与える定義，又はトークンのテキストで引いた定義
	 * @throws FormatLimitExceeded 文法が其の種別を持たない場合（正規化の不具合）
	 */
	const FormPart &SyntaxModel::GetDefFor(const GeneratedNode &Generated) const {
		// 生成するノードの種別と構文事実の定義の取得
		const FormPart *Chosen = Generated.Def.Def ? &Generated.Def : nullptr;
		// 名前の無いトークンは固定の定義のテキスト毎の種別と事実で引く
		if(!Chosen && !Generated.IsNamed) {
			if(
				const std::unordered_map<std::string_view, FormPart>::const_iterator Found = Def->TokenDefs.find(Generated.Kind);
				Found != Def->TokenDefs.end()
			) Chosen = &Found->second;
		}
		if(!Chosen) throw FormatLimitExceeded("the grammar has no kind for a generated node", FailureReason::FormatterDefect);
		// 生成する種別と構文事実の定義の返戻
		return *Chosen;
	}

	/**
	 * 前の名前付の兄弟の取得関数
	 * 計算量：探索する子又は兄弟の数 C に対し O(C)
	 * @param Id ノードの識別子
	 * @return 前の名前付の兄弟（無ければ無）
	 * @throws std::out_of_range 親の識別子がノード表に無い場合
	 */
	NodeId SyntaxModel::GetPrevNamedSibling(const NodeId Id) const {
		// 前の名前付の兄弟の取得
		NodeId Prev = GetPrevSibling(Id);
		while(Prev != NoNode && !IsNamed(Prev)) Prev = GetPrevSibling(Prev);
		// 前の名前付の兄弟の返戻
		return Prev;
	}

	/**
	 * 現存ノードの生成部品の取得関数
	 * @param Id 種別と空行比較を保持するノード
	 * @return 同じノードの種別と空行比較を持つ生成部品
	 * @throws std::out_of_range ノードの識別子がノード表の範囲外の場合
	 */
	FormPart SyntaxModel::GetGenerationPartOf(const NodeId Id) const {
		// 現存ノードの生成部品の取得
		const SyntaxNode &Node = GetNode(Id);
		// 元の構文に対応する生成部品の返戻
		return {
			FormPartRole::Result,
			Node.Kind.Def,
			{},
			FormPartRole::Result,
			ChildRole::None,
			Node.Kind.IsNamed(),
			Node.Facts.BlankKey,
			Node.Facts.Forms,
			Node.Facts.Meanings,
			Node.Facts.TokenDef
		};
	}

	/**
	 * シグネチャ・ドキュメントを含む実作業の計上関数（整形仕様 9.2）
	 * 計算量：O(1)
	 * @param Amount 計上する作業単位
	 * @throws FormatLimitExceeded 作業単位の合計が上限を超える場合（規定したリソース上限の見送）
	 * @throws ProcessingInterrupted 書戻以外の処理の中断を要求済の場合
	 */
	void StructureWorkAccount::Charge(const size_t Amount) {
		ParserAllocator::CheckInterruption();
		// 上限超過の見送
		if(Amount > Limit - Used) throw FormatLimitExceeded("structured text exceeds formatting limit");
		Used += Amount;
		// 終了
		return;
	}

	/**
	 * 構文の正規化と中間表現の構築の作業単位の計上関数
	 * 計算量：O(1)
	 * @param Amount 計上する作業単位
	 * @throws FormatLimitExceeded 作業単位の合計が上限を超える場合
	 * @throws ProcessingInterrupted 書戻以外の処理の中断を要求済の場合
	 */
	void SyntaxModel::Charge(const size_t Amount) const {
		// 予算を閉じた後も，中断は書戻区間以外で受け付ける
		if(Work->IsClosed) {
			ParserAllocator::CheckInterruption();
			// 計上しない工程の終了
			return;
		}
		Work->Charge(Amount);
		// 終了
		return;
	}

	/**
	 * 種別毎の照会対象の役割列の取得関数
	 * 同じ種別のノードは選別済の固定の列を共有する
	 * 計算量：初回は種別の役割数 R に対し O(R)，以後は期待値 O(1)
	 * @param Kind 固定の種別
	 * @return 此のインデックスで照会する役割だけの昇順の列
	 * @throws FormatLimitExceeded 役割の選別が作業予算を超える場合
	 * @throws std::bad_alloc 役割列のメモリを確保出来ない場合
	 * @throws std::length_error 役割列の上限を超える場合
	 */
	const std::vector<Role> &SyntaxModel::IndexedRolesOf(const KindDef &Kind) {
		// 種別毎の照会対象の役割列の取得
		Charge(1);
		const std::pair<std::unordered_map<const KindDef *, std::vector<Role>>::iterator, bool> Entry =
		IndexedRoleLists.try_emplace(&Kind);
		if(Entry.second) for(const Role Wanted : Kind.RoleList) {
			Charge(1);
			if(IndexedRoles.test(static_cast<size_t>(Wanted))) {
				Charge(1);
				Entry.first->second.push_back(Wanted);
			}
		}
		// 此の種別の選別済の役割列の返戻
		return Entry.first->second;
	}

	/**
	 * 書換が変更したノードの記録関数
	 * 計算量：変更ノードの記録は償却 O(1)，名前のテキストの事実の破棄は O(1)
	 * @param Id 変更したノード
	 * @throws FormatLimitExceeded 変更前の事実の記録が作業予算を超えた場合
	 * @throws std::bad_alloc ノードの事実のメモリを確保出来ない場合
	 * @throws std::length_error ノードの事実の要素数上限を超える場合
	 */
	void SyntaxModel::Touch(const NodeId Id) {
		// 対象の無い記録の終了
		if(Id == NoNode) return;
		if(HasRoleIdx) {
			Charge(1);
			LastTouched.insert(Id);
		}
		Nodes[Id].Facts.Spelling.reset();
		// 終了
		return;
	}

	/**
	 * ノードの現在の役割のインデックスへの登録関数
	 * 計算量：変更前後の種別の役割数 R に対し O(R log(R + 1))
	 * @param Id 登録するノード
	 * @throws FormatLimitExceeded 役割の登録又は除去が作業予算を超えた場合
	 * @throws std::bad_alloc 役割又は登録位置のメモリを確保出来ない場合
	 * @throws std::length_error 役割又は登録位置の列の上限を超える場合
	 */
	void SyntaxModel::IdxRoles(const NodeId Id) {
		// ノードの現在の役割のインデックスへの登録
		if(IndexedKinds.size() <= Id) {
			IndexedKinds.resize(Id + 1, nullptr);
			IndexedPositionRanges.resize(Id + 1);
		}
		const KindDef *const Prev = IndexedKinds[Id], *const Cur = IsRemoved[Id] ? nullptr : Nodes[Id].Kind.Def;
		// 同じ種別のインデックスの終了
		if(Prev == Cur) return;
		PositionBuffer.clear();
		const std::vector<Role> *const PrevRoles = Prev ? &IndexedRolesOf(*Prev) : nullptr;
		const std::vector<Role> *const CurRoles = Cur ? &IndexedRolesOf(*Cur) : nullptr;
		if(CurRoles) PositionBuffer.resize(CurRoles->size());
		if(PrevRoles) for(size_t Idx = 0; Idx < PrevRoles->size(); ++Idx) {
			const Role Wanted = (*PrevRoles)[Idx];
			const size_t RoleNum = static_cast<size_t>(Wanted), Pos = IndexedPositions[IndexedPositionRanges[Id].first + Idx];
			if(Cur && Cur->Roles.test(RoleNum)) continue;
			Charge(1);
			std::vector<NodeId> &Indexed = RoleNodes[RoleNum];
			const NodeId Last = Indexed.back();
			const std::vector<Role> &Roles = IndexedRolesOf(*IndexedKinds[Last]);
			Indexed[Pos] = Last;
			IndexedPositions[
				IndexedPositionRanges[Last].first + static_cast<size_t>(std::lower_bound(Roles.begin(), Roles.end(), Wanted) - Roles.begin())
			] = Pos;
			Indexed.pop_back();
		}
		if(CurRoles) for(size_t Idx = 0; Idx < CurRoles->size(); ++Idx) {
			const Role Wanted = (*CurRoles)[Idx];
			if(const size_t RoleNum = static_cast<size_t>(Wanted); Prev && Prev->Roles.test(RoleNum)) {
				const std::vector<Role>::const_iterator Found = std::lower_bound(PrevRoles->begin(), PrevRoles->end(), Wanted);
				PositionBuffer[Idx] = IndexedPositions[IndexedPositionRanges[Id].first + static_cast<size_t>(Found - PrevRoles->begin())];
			} else {
				Charge(1);
				PositionBuffer[Idx] = RoleNodes[RoleNum].size();
				RoleNodes[RoleNum].push_back(Id);
			}
		}
		// 旧役割の参照を終えてから同じ範囲を再使用し，容量が不足する時だけ共有の列へ追加
		std::pair<size_t, size_t> &Range = IndexedPositionRanges[Id];
		if(Range.second < PositionBuffer.size()) {
			Range = { IndexedPositions.size(), PositionBuffer.size() };
			IndexedPositions.resize(Range.first + Range.second);
		}
		std::copy(PositionBuffer.begin(), PositionBuffer.end(), IndexedPositions.begin() + Range.first);
		IndexedKinds[Id] = Cur;
		// 終了
		return;
	}

	/**
	 * 保持した区間内での部分木の採番関数
	 * 前順と後順の間へ子孫を置き，区間の外の番号は変更しない
	 * 計算量：対象のノード数 N に対し O(N)
	 * @param Id 更新する部分木
	 * @return 区間の余裕が足りれば `true`
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc 走査の列を確保出来ない場合
	 * @throws std::length_error 走査の列の上限を超える場合
	 */
	bool SyntaxModel::IsOrderNumberingSuccessful(const NodeId Id) const {
		// 書込先の列を修復間で再使用し，追加で移動しないノードを後から引き直さない
		std::vector<uint64_t *> &Events = OrderEvents;
		Events.clear();
		NodeId Current = Id;
		bool IsExit = false;
		while(1) {
			Charge(1);
			const SyntaxNode &Node = Nodes[Current];
			Events.push_back(IsExit ? &Node.OrderLast : &Node.OrderFirst);
			if(!IsExit) {
				if(!Node.Children.empty()) Current = Node.Children.front();
				else IsExit = true;
			} else {
				if(Current == Id) break;
				const SyntaxNode &Parent = Nodes[Node.Parent];
				if(Node.Idx + 1 < Parent.Children.size()) {
					Current = Parent.Children[Node.Idx + 1];
					IsExit = false;
				} else Current = Node.Parent;
			}
		}
		const uint64_t First = Id == Root ? 1 : Nodes[Id].OrderFirst, Last = Id == Root ? UINT64_MAX : Nodes[Id].OrderLast;
		const uint64_t Step = (Last - First) / (Events.size() - 1);
		// 新しい子孫を収容出来ない区間だけを外側へ広げる
		if(!Step) return false;
		for(size_t Pos = 0; Pos < Events.size(); ++Pos) {
			Charge(1);
			*Events[Pos] = First + Pos * Step;
		}
		Nodes[Id].OrderLast = Last;
		// 区間内の更新の完了の返戻
		return true;
	}

	/**
	 * 子列の変更に依る順序区間の修復関数
	 * 順序を保つ子の区間は固定し，其の間に移動・挿入した子だけを採番する
	 * 計算量：直接の子数 C と採番する部分木のノード数 N に対し O(C + N)
	 * @param Id 子列を変更した親
	 * @param FirstChild 変更が始まる子の位置
	 * @return 区間の余裕が足りれば `true`
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc 走査の列を確保出来ない場合
	 * @throws std::length_error 走査の列の上限を超える場合
	 */
	bool SyntaxModel::IsOrderRepairSuccessful(const NodeId Id, const uint32_t FirstChild) const {
		// 既存の子の間に残る番号を，新しい部分木へ割り当てる
		const SyntaxNode &Parent = Nodes[Id];
		size_t Start = std::min<size_t>(FirstChild, Parent.Children.size());
		uint64_t Before = Start ? Nodes[Parent.Children[Start - 1]].OrderLast : Parent.OrderFirst;
		for(size_t Pos = Start; Pos <= Parent.Children.size(); ++Pos) {
			Charge(1);
			const SyntaxNode *const Child = Pos == Parent.Children.size() ? nullptr : &Nodes[Parent.Children[Pos]];
			if(Child && !(Before < Child->OrderFirst && Child->OrderLast < Parent.OrderLast)) continue;
			const uint64_t After = Child ? Child->OrderFirst : Parent.OrderLast;
			if(Start != Pos) {
				const uint64_t Step = (After - Before - 1) / (Pos - Start);
				// 子の間の余裕が無ければ親の区間内で配り直す
				if(Step < 2) return IsOrderNumberingSuccessful(Id);
				for(size_t Next = Start; Next < Pos; ++Next) {
					const NodeId Moved = Parent.Children[Next];
					Nodes[Moved].OrderFirst = Before + 1 + (Next - Start) * Step;
					Nodes[Moved].OrderLast = Nodes[Moved].OrderFirst + Step - 1;
					// 移した部分木が収まらなければ親の区間を使う
					if(!IsOrderNumberingSuccessful(Moved)) return IsOrderNumberingSuccessful(Id);
				}
			}
			if(Child) Before = Child->OrderLast;
			Start = Pos + 1;
		}
		// 既存の区間を保った更新の完了の返戻
		return true;
	}

	/**
	 * 順序照会の前の変更部分木の更新関数
	 * 同じ親の変更を纏め，順序を保つ子と無関係な部分木は再採番しない
	 * 計算量：変更根数 Q，直接の子数 C，採番量 N に対し O(Q log(Q + 1) + C + N)
	 * @throws FormatLimitExceeded 作業予算又は順序番号の容量を超える場合
	 * @throws std::bad_alloc 変更根又は走査の列を確保出来ない場合
	 * @throws std::length_error 走査の列の上限を超える場合
	 */
	void SyntaxModel::EnsureOrder() const {
		// 初回又は根その物を置換した場合だけ全体を採番する
		if(OrderRoot != Root) {
			if(!IsOrderNumberingSuccessful(Root)) throw FormatLimitExceeded("syntax order exceeds formatting limit");
			OrderRoot = Root;
			OrderChanges.clear();
		}
		while(!OrderChanges.empty()) {
			std::vector<std::pair<NodeId, uint32_t>> Changed(OrderChanges.begin(), OrderChanges.end());
			OrderChanges.clear();
			// 変更前の順で修復し，移動先の区間は子列の現在の順で確定する
			WorkSort::Stable(
				std::span(Changed),
				[this](const std::pair<NodeId, uint32_t> &Left, const std::pair<NodeId, uint32_t> &Right) -> bool {
					Charge(2);
					// 変更前の前順の大小の返戻
					return Nodes[Left.first].OrderFirst < Nodes[Right.first].OrderFirst;
				},
				[this](const size_t Amount) -> void {
					Charge(Amount);
				}
			);
			for(const std::pair<NodeId, uint32_t> &Entry : Changed) {
				const NodeId Id = Entry.first;
				Charge(1);
				if(IsRemoved[Id] || Id != Root && Nodes[Id].Parent == NoNode) continue;
				if(!IsOrderRepairSuccessful(Id, Entry.second)) {
					NodeId Outer = Nodes[Id].Parent;
					while(Outer != NoNode && !IsOrderNumberingSuccessful(Outer)) Outer = Nodes[Outer].Parent;
					if(Outer == NoNode) throw FormatLimitExceeded("syntax order exceeds formatting limit");
				}
			}
		}
		// 構築後の最初の照会で残った変更も確定した場合は，書込先を保持しない
		if(Work->IsClosed && !OrderEvents.empty()) std::vector<uint64_t *>().swap(OrderEvents);
		// 終了
		return;
	}

	/**
	 * 子列が変わる部分木の順序の失効関数
	 * 新規ノードは未だ番号を持たない為，既存の最内の区間へ変更を纏める
	 * @param Id 子列を変える親
	 * @param FirstChild 変更を始める子の位置
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc 変更根の集合を確保出来ない場合
	 */
	void SyntaxModel::MarkOrderChange(NodeId Id, uint32_t FirstChild) {
		// 未使用の順序インデックスの為には走査しない
		if(OrderRoot == NoNode) return;
		Charge(1);
		while(Id != NoNode && !Nodes[Id].OrderFirst) {
			Charge(1);
			Id = Nodes[Id].Parent;
			FirstChild = 0;
		}
		if(Id != NoNode) {
			const std::map<NodeId, uint32_t>::iterator Found = OrderChanges.try_emplace(Id, FirstChild).first;
			Found->second = std::min(Found->second, FirstChild);
		}
		// 終了
		return;
	}

	/**
	 * 親の子の並びからの切離関数
	 * 計算量：対象以後の兄弟数 S と境界数 B に対し O(S + B)
	 * @param Id 切り離すノード
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	void SyntaxModel::Detach(const NodeId Id) {
		// 親の子の並び
		const NodeId Parent = Nodes[Id].Parent;
		MarkOrderChange(Parent, Nodes[Id].Idx);
		// 親子の接続と子の並びの更新
		Touch(Id);
		Touch(Parent);
		if(Parent != NoNode) {
			const uint32_t Removed = Nodes[Id].Idx;
			std::pmr::vector<NodeId> &Siblings = Nodes[Parent].Children;
			Charge(Siblings.size() - Removed);
			Siblings.erase(Siblings.begin() + Removed);
			UpdateNamedChildCount(Parent, IsNamed(Id), false);
			// 後の兄弟の位置の振直
			for(uint32_t Idx = Removed; Idx < Siblings.size(); ++Idx) Nodes[Siblings[Idx]].Idx = Idx;
			// 除いた子の前の境界の除去と後続の境界の番号の更新
			if(!Nodes[Parent].Facts.Boundaries.GetView().empty()) {
				std::vector<SemanticBoundary> &Boundaries = Nodes[Parent].Facts.Boundaries.Edit();
				Charge(Boundaries.size() << 1);
				std::erase_if(
					Boundaries,
					[Removed](const SemanticBoundary &Boundary) -> bool {
						// 除いた子の前の境界かの返戻
						return Boundary.Before == Removed;
					}
				);
				for(SemanticBoundary &Boundary : Boundaries) if(Boundary.Before > Removed) --Boundary.Before;
			}
		}
		InvalidateBindingLocation(Id);
		Nodes[Id].Parent = NoNode;
		// 終了
		return;
	}

	/**
	 * ノードの追加関数
	 * 識別子は追加の順に振り，除いたノードの識別子も再利用しない
	 * 計算量：種別が持つ役割数 R に対し O(R log(R + 1))，ノード表の追加は償却 O(1)
	 * @param Node 追加するノード（識別子は此処で振る）
	 * @return 振った識別子
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	NodeId SyntaxModel::AddNode(SyntaxNode &&Node) {
		// ノードの追加
		Charge(1);
		// 振る識別子
		const NodeId Target = static_cast<NodeId>(Nodes.size());
		// ノード表と役割インデックスへの追加
		Nodes.emplace_back(std::move(Node));
		IsRemoved.push_back(0);
		if(HasRoleIdx) IdxRoles(Target);
		Touch(Target);
		// 振った識別子の返戻
		return Target;
	}

	/**
	 * 部分木のトークンのテキストを区切無で繋いだテキストの取得関数
	 * トークンの葉の比較（`await` 等）に使い，トークンの間の空白を持たない
	 * 計算量：部分木のノード数 V とテキストの長さ N に対し O(V + N)
	 * @param Id ノードの識別子
	 * @return テキスト
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	std::string SyntaxModel::GetText(const NodeId Id) const {
		// 葉は走査列を確保せず，同じノード・バイト数を計上する
		if(Id >= Nodes.size()) return {};
		if(Nodes[Id].Children.empty()) {
			std::string Text = Nodes[Id].Lexeme;
			Charge(1 + Text.size());
			// 単一トークンの返戻
			return Text;
		}
		// 部分木のトークンの連結
		std::string Joined;
		std::vector<NodeId> Stack { Id };
		size_t Visited = 0;
		while(!Stack.empty()) {
			const NodeId Top = Stack.back();
			Stack.pop_back();
			if(Top >= Nodes.size()) continue;
			++Visited;
			Joined += Nodes[Top].Lexeme;
			Stack.insert(Stack.end(), Nodes[Top].Children.rbegin(), Nodes[Top].Children.rend());
		}
		// 辿ったノードとコピーしたトークンのバイトの計上（整形仕様 9.2）
		Charge(Visited + Joined.size());
		// 繋いだテキストの返戻
		return Joined;
	}

	/**
	 * 同じ親の既存ノードの一括切離関数
	 * 子の位置と境界を親毎に１度詰め直す
	 * 束縛の変更記録の総費用を Q とする
	 * 計算量：移動数 M，親毎の子数の総和 C，境界数 B，間隙のコピー総長 L に対し期待値 O(M + C + B + L + Q)
	 * @param Moved 切り離す既存ノード
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc 親毎の集合又は境界のメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 指定したノード又は境界の位置が表の範囲外の場合
	 */
	void SyntaxModel::DetachBatch(const std::vector<NodeId> &Moved) {
		// 同じ親の既存ノードの一括切離
		std::unordered_map<NodeId, std::unordered_set<NodeId>> ByParent;
		for(const NodeId Node : Moved) {
			Charge(1);
			if(Nodes[Node].Parent == NoNode) continue;
			MarkBindingChange(Node);
			ByParent[Nodes[Node].Parent].insert(Node);
		}
		for(const std::pair<const NodeId, std::unordered_set<NodeId>> &Entry : ByParent) {
			const NodeId Parent = Entry.first;
			MarkOrderChange(Parent, 0);
			Touch(Parent);
			std::pmr::vector<NodeId> &Children = Nodes[Parent].Children;
			std::vector<uint32_t> &Positions = DetachPositions;
			Positions.resize(Children.size() + 1);
			uint32_t Kept = 0;
			for(uint32_t Idx = 0; Idx < Children.size(); ++Idx) {
				Charge(1);
				Positions[Idx] = Kept;
				if(const NodeId Child = Children[Idx]; Entry.second.contains(Child)) {
					UpdateNamedChildCount(Parent, IsNamed(Child), false);
					Touch(Child);
					InvalidateBindingLocation(Child);
					Nodes[Child].Parent = NoNode;
				} else {
					Children[Kept] = Child;
					Nodes[Child].Idx = Kept++;
				}
			}
			Positions.back() = Kept;
			std::vector<SemanticBoundary> Boundaries;
			for(const SemanticBoundary &Boundary : Nodes[Parent].Facts.Boundaries.GetView()) {
				Charge(1);
				if(Boundary.Before < Positions.size() - 1 && Positions[Boundary.Before] == Positions[Boundary.Before + 1]) continue;
				SemanticBoundary Next = Boundary;
				Charge(Next.Gap.size());
				Next.Before = Positions.at(Boundary.Before);
				Boundaries.push_back(std::move(Next));
			}
			Children.resize(Kept);
			Nodes[Parent].Facts.Boundaries = SemanticBoundaryList(std::move(Boundaries));
		}
		DetachPositions.clear();
		// 終了
		return;
	}

	/**
	 * 規則から生成するノードの部分木の追加関数
	 * 生成部分木のノードを追加し，既存ノードを指定した位置へ接続する
	 * 部分木の根を親の子の並びへ挿入する処理は呼出側が行う
	 * 束縛の変更記録の総費用を Q とする
	 * 計算量：生成部品数 G，トークン・境界のコピー量 B と既存ノードの切離費用 D に対し O(G + B + D + Q)
	 * @param Generated 生成するノード
	 * @param Parent 親
	 * @param Idx 親の子の並びの中の位置
	 * @param ShouldMarkBinding 束縛の構造の変更を記録するか（値を包む括弧だけなら偽）
	 * @return 加えた部分木の根
	 * @throws FormatLimitExceeded 文法が其の種別を持たない場合，又は束縛の記録が作業予算を超えた場合
	 * @throws std::bad_alloc ノード・生成の作業列・変更記録又はアンカーのメモリを確保出来ない場合
	 * @throws std::length_error ノードのコピー・作業列又はアンカーの要素数上限を超える場合
	 * @throws std::out_of_range 指定したノードがノード表に無い場合
	 */
	NodeId SyntaxModel::AddGenerated(
		const GeneratedNode &Generated,
		const NodeId Parent,
		const uint32_t Idx,
		const bool ShouldMarkBinding
	) {
		// 生成部分木の組立の状態
		struct Frame {
			const GeneratedNode *Src; // 組み立てる生成部分木
			NodeId Parent;
			uint32_t Idx;
			NodeId Id = NoNode;
			uint32_t NextChild = 0;
			bool HasEntered = false;
		};
		// 生成部分木の後行順の構築
		std::vector<Frame> Pending { { &Generated, Parent, Idx } };
		NodeId Result = NoNode;
		while(!Pending.empty()) {
			Frame &Cur = Pending.back();
			if(!Cur.HasEntered) {
				Cur.HasEntered = true;
				Charge(1);
				Cur.Id = Cur.Src->Existing;
				// 移す既存のノードの元の親からの切離
				if(Cur.Id != NoNode && Nodes[Cur.Id].Parent != NoNode) {
					if(ShouldMarkBinding) MarkBindingChange(Cur.Id);
					Detach(Cur.Id);
				}
				if(Cur.Id == NoNode) {
					SyntaxNode Added;
					const FormPart &Part = GetDefFor(*Cur.Src);
					Added.Kind = { Part.Def };
					for(const SemanticBoundary &Boundary : Cur.Src->Facts.Boundaries.GetView()) Charge(Boundary.Gap.size() + 1);
					Added.Facts = Cur.Src->Facts;
					Added.Facts.BlankKey = Part.BlankKey;
					Added.Facts.Forms = Part.Forms;
					Added.Facts.Meanings |= Part.Meanings;
					Added.Facts.TokenDef = Part.TokenDef;
					// 原文のコピーは元の事実を保ち，新しく作るノードは置く位置の親から引き継ぐ
					if(Cur.Src->Src == NoNode && Cur.Parent != NoNode) {
						Added.Facts.HasFalseName = Nodes[Cur.Parent].Facts.HasFalseName;
						Added.Facts.HasTrueName = Nodes[Cur.Parent].Facts.HasTrueName;
					}
					if(Cur.Src->Children.empty()) {
						Added.Lexeme = !Cur.Src->Lexeme.empty() ? Cur.Src->Lexeme : Cur.Src->Src != NoNode ? Nodes[Cur.Src->Src].Lexeme : Cur.Src->Kind;
					}
					Charge(Added.Lexeme.size());
					Cur.Id = AddNode(std::move(Added));
					Nodes[Cur.Id].Children.reserve(Cur.Src->Children.size());
				}
			}
			if(Cur.Src->Existing == NoNode && Cur.NextChild < Cur.Src->Children.size()) {
				const uint32_t Child = Cur.NextChild++;
				Pending.push_back({ &Cur.Src->Children[Child], Cur.Id, Child });
				continue;
			}
			if(Cur.Src->Existing == NoNode && Nodes[Cur.Id].Lexeme.empty() && Nodes[Cur.Id].Children.size() == 1) {
				Nodes[Cur.Id].Facts.Meanings = Nodes[Nodes[Cur.Id].Children.front()].Facts.Meanings;
			}
			// 親との接続と生成元の判定
			if(Cur.Src->Existing != NoNode) Touch(Cur.Id);
			InvalidateBindingLocation(Cur.Id);
			Nodes[Cur.Id].Parent = Cur.Parent;
			Nodes[Cur.Id].Idx = Cur.Idx;
			Nodes[Cur.Id].Fields = Cur.Src->Fields;
			if(ShouldMarkBinding) MarkBindingChange(Cur.Id);
			// 原文のノードを移さずコピーせずに作ったノードで，子も全て其の様な物なら加えた部分木
			if(Cur.Src->Existing == NoNode) {
				const bool IsInserted = Cur.Src->Src == NoNode && std::ranges::all_of(
					Nodes[Cur.Id].Children,
					[this](const NodeId Child) -> bool {
						// 加えた部分木の子かの返戻
						return IsInsertedNode(Child);
					}
				);
				if(IsInserted || !InsertedNodes.empty()) {
					InsertedNodes.resize(Nodes.size(), false);
					InsertedNodes[Cur.Id] = IsInserted;
				}
			}
			const NodeId Node = Cur.Id;
			Pending.pop_back();
			if(Pending.empty()) Result = Node;
			else {
				const NodeId Host = Pending.back().Id;
				Nodes[Host].Children.push_back(Node);
				UpdateNamedChildCount(Host, false, IsNamed(Node));
			}
		}
		// 加えた部分木の根の返戻
		return Result;
	}

	/**
	 * 無名の空文も含む内容子数の取得関数
	 * 計算量：子数 C に対し O(C)
	 * @param Id 親ノード
	 * @return 名前付の子と独立した空文の子の数
	 * @throws FormatLimitExceeded 子の照会が作業予算を超える場合
	 */
	uint32_t SyntaxModel::GetContentChildCount(const NodeId Id) const {
		// 文として意味を持つトークンも含む内容子数の取得
		uint32_t Count = 0;
		for(const NodeId Child : GetChildrenOf(Id)) {
			Charge(1);
			Count += IsNamed(Child) || Is(Child, Role::EmptyStmt);
		}
		// 内容子数の返戻
		return Count;
	}

	/**
	 * 無名の空文も含む内容子の取得関数
	 * 計算量：参照する子数 C に対し O(C)
	 * @param Id 親ノード
	 * @param Idx 内容子の中の位置
	 * @return 指定した内容子（位置が範囲外なら無）
	 * @throws FormatLimitExceeded 子の照会が作業予算を超える場合
	 */
	NodeId SyntaxModel::GetContentChild(const NodeId Id, const uint32_t Idx) const {
		// 名前付の子と独立した空文だけの位置の照会
		uint32_t Seen = 0;
		for(const NodeId Child : GetChildrenOf(Id)) {
			Charge(1);
			if(!IsNamed(Child) && !Is(Child, Role::EmptyStmt)) continue;
			// 指定した内容子の返戻
			if(Seen++ == Idx) return Child;
		}
		// 範囲外の返戻
		return NoNode;
	}

	/**
	 * 無名の空文も含む最後の内容子の取得関数
	 * 計算量：参照する子数 C に対し O(C)
	 * @param Id 親ノード
	 * @return 最後の内容子（無ければ無）
	 * @throws FormatLimitExceeded 子の照会が作業予算を超える場合
	 */
	NodeId SyntaxModel::GetLastContentChild(const NodeId Id) const {
		// 後ろから名前付の子又は独立した空文を探す
		const std::span<const NodeId> Children = GetChildrenOf(Id);
		for(std::span<const NodeId>::reverse_iterator Child = Children.rbegin(); Child != Children.rend(); ++Child) {
			Charge(1);
			// 最後の内容子の返戻
			if(IsNamed(*Child) || Is(*Child, Role::EmptyStmt)) return *Child;
		}
		// 内容子の無い事の返戻
		return NoNode;
	}

	/**
	 * 名前付の子の数の取得関数
	 * 計算量：通常側の再照会は O(1)，初回は子数 C と新しい記録の追加量 A に対し O(C + A)，基準側は O(C)
	 * @param Id ノードの識別子
	 * @return 名前付の子の数
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 子数の記録を確保出来ない場合
	 * @throws std::length_error 子数の記録の要素数上限を超える場合
	 */
	uint32_t SyntaxModel::GetNamedChildCount(const NodeId Id) const {
		#if !defined(ShaveFormatRefFacts)
		if(Id < NamedChildCounts.size() && NamedChildCounts[Id] != UINT32_MAX) {
			Charge(1);
			// 同じ構造で共有する子数の返戻
			return NamedChildCounts[Id];
		}
		#endif
		// 無又は未存在のノードは従来通り空の子列として扱う
		const std::span<const NodeId> Children = GetChildrenOf(Id);
		#if !defined(ShaveFormatRefFacts)
		// 未存在のノードの子数の返戻
		if(Id >= GetNodeCount()) return 0;
		if(NamedChildCounts.size() <= Id) {
			Charge(static_cast<size_t>(Id) + 1 - NamedChildCounts.size());
			NamedChildCounts.resize(static_cast<size_t>(Id) + 1, UINT32_MAX);
		}
		#endif
		// 初回又は独立した基準側だけでの計数
		Charge(Children.size());
		const uint32_t Count = static_cast<uint32_t>(
			std::count_if(
				Children.begin(),
				Children.end(),
				[this](const NodeId Child) -> bool {
					// 名前付の子かの返戻
					return IsNamed(Child);
				}
			)
		);
		#if !defined(ShaveFormatRefFacts)
		NamedChildCounts[Id] = Count;
		#endif
		// 名前付の子数の返戻
		return Count;
	}

	/**
	 * 接続・切離・種別変更に依る既知の子数の更新関数
	 * 未照会の親には記録を作らず，既知の親だけを差分で保つ
	 * 計算量：O(1)
	 * @param Parent 子数を保つ親（無も可）
	 * @param HadNamedChild 変更前に名前付の子を持って居たか
	 * @param HasNamedChild 変更後に名前付の子を持つか
	 * @throws FormatLimitExceeded 更新の作業予算を超える場合
	 */
	void SyntaxModel::UpdateNamedChildCount(const NodeId Parent, const bool HadNamedChild, const bool HasNamedChild) {
		// 未照会又は名前付の数を変えない更新の終了
		if(Parent >= NamedChildCounts.size() || NamedChildCounts[Parent] == UINT32_MAX || HadNamedChild == HasNamedChild) return;
		Charge(1);
		NamedChildCounts[Parent] -= static_cast<uint32_t>(HadNamedChild);
		NamedChildCounts[Parent] += static_cast<uint32_t>(HasNamedChild);
		// 終了
		return;
	}

	/**
	 * 名前付の子の取得関数
	 * 計算量：子の数 C に対し O(C)
	 * @param Id ノードの識別子
	 * @param Idx 名前付の子の中の位置
	 * @return 名前付の子（位置が範囲の外なら無）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 */
	NodeId SyntaxModel::GetNamedChild(const NodeId Id, const uint32_t Idx) const {
		// 名前付の子の取得
		uint32_t Seen = 0;
		for(const NodeId Child : GetChildrenOf(Id)) {
			Charge(1);
			if(!IsNamed(Child)) continue;
			// 位置の名前付の子の返戻
			if(Seen == Idx) return Child;
			++Seen;
		}
		// 範囲の外の返戻
		return NoNode;
	}

	/**
	 * 最後の名前付の子の取得関数
	 * 計算量：探索する子又は兄弟の数 C に対し O(C)
	 * @param Id ノードの識別子
	 * @return 最後の名前付の子（無ければ無）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 */
	NodeId SyntaxModel::GetLastNamedChild(const NodeId Id) const {
		// 最後の名前付の子の取得
		const std::span<const NodeId> Children = GetChildrenOf(Id);
		for(std::span<const NodeId>::reverse_iterator Child = Children.rbegin(); Child != Children.rend(); ++Child) {
			Charge(1);
			// 最後の名前付の子の返戻
			if(IsNamed(*Child)) return *Child;
		}
		// 名前付の子の無い事の返戻
		return NoNode;
	}

	/**
	 * 投影又は生成で確定した語の意味の判定関数
	 * @param Id 対象のノード
	 * @param Meaning 確認する意味
	 * @return 対象が意味を持つ場合は真
	 */
	bool SyntaxModel::IsWord(const NodeId Id, const Word Meaning) const {
		// 対象が無い場合の偽の返戻
		if(Id >= GetNodeCount()) return false;
		const WordSet &Meanings = Validation ? Validation->Records[Id].Meanings : Nodes[Id].Facts.Meanings;
		// 入力又は生成形式で確認した意味の返戻
		return Meanings.test(static_cast<size_t>(Meaning));
	}

	/**
	 * 識別子の NFKC 比較名の生成関数
	 * 互換分解，結合クラス順の安定整列，正準合成を固定した Unicode の表から求める
	 * Hangul は表を展開せず，Unicode が定める算術的な分解・合成を使う
	 * 計算量：展開後のコードポイント数 M に対し O(M log M)，作業域 O(M)
	 * @param Name 非 ASCII を含む識別子
	 * @return 正規化した比較名（不正な UTF-8 は入力名）
	 * @throws FormatLimitExceeded 正規化の走査又は整列が作業予算を超える場合
	 * @throws std::bad_alloc 正規化の作業域を確保出来ない場合
	 * @throws std::length_error 名前又は作業列の上限を超える場合
	 */
	std::string SyntaxModel::NormalizeIdentifierNfkc(const std::string_view Name) const {
		struct ClassifiedPoint {
			char32_t Point; // 展開したコードポイント
			uint8_t Class; // 正準結合クラス
			size_t Order; // 同じ結合クラスの原順序
		}; // 整列と合成に使うコードポイント
		std::vector<ClassifiedPoint> Expanded;
		const auto Append = [this, &Expanded](const char32_t Point) -> void {
			Charge(12);
			const decltype(UnicodeNormalization::CombiningClasses)::const_iterator Found = std::ranges::lower_bound(
				UnicodeNormalization::CombiningClasses,
				static_cast<uint32_t>(Point),
				{},
				[](const uint32_t Entry) -> uint32_t {
					// 結合クラスのキーの返戻
					return Entry >> 8;
				}
			);
			const uint8_t Class =
			Found != UnicodeNormalization::CombiningClasses.end() && *Found >> 8 == Point ? static_cast<uint8_t>(*Found) : 0;
			Expanded.push_back({ Point, Class, Expanded.size() });
			// 終了
			return;
		};
		for(size_t Pos = 0; Pos < Name.size();) {
			char32_t Point = 0;
			Charge(18);
			// デコード出来ない名前の保持
			if(!TextEdit::IsUtf8Decoded(Name, Pos, Point)) return std::string(Name);
			if(Point > 0XABFF && Point < 0XD7A4) {
				const char32_t Syllable = Point - 0XAC00;
				Append(0X1100 + Syllable / 588);
				Append(0X1161 + Syllable % 588 / 28);
				if(const char32_t Tail = Syllable % 28; Tail) Append(0X11A7 + Tail);
				continue;
			}
			const decltype(UnicodeNormalization::Decompositions)::const_iterator Found = std::ranges::lower_bound(
				UnicodeNormalization::Decompositions,
				static_cast<uint64_t>(Point),
				{},
				[](const uint64_t Entry) -> uint64_t {
					// 互換分解のキーの返戻
					return Entry >> 32;
				}
			);
			if(Found == UnicodeNormalization::Decompositions.end() || *Found >> 32 != Point) Append(Point);
			else {
				const size_t Begin = static_cast<size_t>(*Found >> 8 & 0XFFFFFF), Count = static_cast<size_t>(*Found & 0XFF);
				for(size_t Idx = Begin; Idx < Begin + Count; ++Idx) Append(UnicodeNormalization::Points[Idx]);
			}
		}
		// 結合文字の長い列でも挿入整列の２乗走査を行わない
		for(size_t Begin = 0; Begin < Expanded.size();) {
			if(!Expanded[Begin].Class) ++Begin;
			size_t End = Begin;
			while(End < Expanded.size() && Expanded[End].Class) ++End;
			const size_t Count = End - Begin;
			Charge(Count * (1 + std::bit_width(Count)));
			std::sort(
				Expanded.begin() + Begin,
				Expanded.begin() + End,
				[](const ClassifiedPoint &Left, const ClassifiedPoint &Right) -> bool {
					// 正準結合クラスの昇順かの返戻
					return std::tie(Left.Class, Left.Order) < std::tie(Right.Class, Right.Order);
				}
			);
			Begin = End;
		}
		size_t Starter = 0, Written = 0;
		bool HasStarter = false;
		uint8_t PreviousClass = 0;
		for(const ClassifiedPoint Current : Expanded) {
			Charge(14);
			char32_t Composite = 0;
			if(HasStarter && (!PreviousClass || PreviousClass < Current.Class)) {
				const char32_t First = Expanded[Starter].Point, Second = Current.Point;
				if(First > 0X10FF && First < 0X1113 && Second > 0X1160 && Second < 0X1176) {
					Composite = 0XAC00 + (First - 0X1100) * 588 + (Second - 0X1161) * 28;
				} else if(First > 0XABFF && First < 0XD7A4 && !((First - 0XAC00) % 28) && Second > 0X11A7 && Second < 0X11C3) {
					Composite = First + Second - 0X11A7;
				} else {
					const uint64_t Key = static_cast<uint64_t>(First) << 21 | static_cast<uint64_t>(Second);
					const decltype(UnicodeNormalization::Compositions)::const_iterator Found = std::ranges::lower_bound(
						UnicodeNormalization::Compositions,
						Key,
						{},
						[](const uint64_t Entry) -> uint64_t {
							// 正準合成のキーの返戻
							return Entry >> 21;
						}
					);
					if(Found != UnicodeNormalization::Compositions.end() && *Found >> 21 == Key) {
						Composite = static_cast<char32_t>(*Found & 0X1FFFFF);
					}
				}
			}
			if(Composite) Expanded[Starter].Point = Composite;
			else {
				if(!Current.Class) {
					Starter = Written;
					HasStarter = true;
				}
				Expanded[Written++] = Current;
				PreviousClass = Current.Class;
			}
		}
		std::string Result;
		Charge(Written << 2);
		for(size_t Idx = 0; Idx < Written; ++Idx) TextEdit::EncodeUtf8(Expanded[Idx].Point, Result);
		// 比較用の正規化名の返戻
		return Result;
	}

	/**
	 * 比較用の名前番号の共有関数
	 * 計算量：名前のバイト数 B と正規化後のコードポイント数 M に対し O(B + M log M)
	 * @param Name 投影又はトークン編集で確定した名前
	 * @param IsIdentifier 識別子の読取規則を適用する名前か
	 * @return 同じ綴りに共通の番号（空なら０）
	 * @throws FormatLimitExceeded 名前番号又は作業予算を使い切った場合
	 * @throws std::bad_alloc 名前表のメモリを確保出来ない場合
	 * @throws std::length_error 名前表の要素数上限を超える場合
	 */
	uint32_t SyntaxModel::InternCmpName(std::string Name, const bool IsIdentifier) {
		// 空の名前の返戻
		if(Name.empty()) return 0;
		Charge(Name.size());
		if(
			IsIdentifier && GetRules().ShouldNormalizeIdentifierNfkc && std::ranges::any_of(
				Name,
				[](const unsigned char Byte) -> bool {
					// 非 ASCII のコードポイントを含む事の返戻
					return Byte > 0X7F;
				}
			)
		) Name = NormalizeIdentifierNfkc(Name);
		// 登録済の名前番号の返戻
		if(
			const std::unordered_map<std::string, uint32_t>::const_iterator Found = CmpNameIndexes.find(Name);
			Found != CmpNameIndexes.end()
		) return Found->second;
		if(CmpNames.size() == UINT32_MAX) throw FormatLimitExceeded("comparison names exhausted");
		// 表と逆引の双方へコピーするバイトの計上（整形仕様 9.2）
		Charge(Name.size());
		CmpNames.push_back(std::move(Name));
		const uint32_t Idx = static_cast<uint32_t>(CmpNames.size());
		CmpNameIndexes.emplace(CmpNames.back(), Idx);
		// 新しい名前番号の返戻
		return Idx;
	}

	/**
	 * 親の子の並びへの接続関数
	 * 親の子の並びの指定位置へノードを加え，隣接を記録した親ではノードの前の境界も記録する
	 * 末尾へ加えるノードは，親の末尾の間隙の境界を其の儘前の境界とする
	 * 計算量：挿入位置以後の兄弟数 S と境界数 B に対し O(S + B)
	 * @param Id 接続するノード（親を持たない物）
	 * @param Parent 親
	 * @param Idx 親の子の並びの中の位置
	 * @param Boundary 新しい境界の種類
	 * @param RecordedBoundary 元のノードの前から移す境界（有ればテキストと由来も保持する）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	void SyntaxModel::Attach(
		const NodeId Id,
		const NodeId Parent,
		const uint32_t Idx,
		const BoundaryKind Boundary,
		const std::optional<SemanticBoundary> &RecordedBoundary
	) {
		// 親子の接続の更新
		MarkOrderChange(Parent, Idx);
		Touch(Id);
		Touch(Parent);
		InvalidateBindingLocation(Id);
		Nodes[Id].Parent = Parent;
		std::pmr::vector<NodeId> &Siblings = Nodes[Parent].Children;
		Charge(Siblings.size() - Idx + 1);
		Siblings.insert(Siblings.begin() + Idx, Id);
		UpdateNamedChildCount(Parent, false, IsNamed(Id));
		// ノードと後の兄弟の位置と境界の番号の振直
		for(uint32_t Pos = Idx; Pos < Siblings.size(); ++Pos) Nodes[Siblings[Pos]].Idx = Pos;
		// 境界を記録した親か移す境界を持つ子だけが境界の並びを持つ
		if(const bool IsRecorded = !Nodes[Parent].Facts.Boundaries.GetView().empty(); IsRecorded || RecordedBoundary && Idx) {
			std::vector<SemanticBoundary> &Boundaries = Nodes[Parent].Facts.Boundaries.Edit();
			Charge(Boundaries.size());
			// 末尾へ加えた子が親の末尾の間隙を前の境界として受け継ぐ場合の終了
			if(Idx + 1 == Siblings.size() && !Boundaries.empty() && Boundaries.back().Before == Idx) return;
			for(SemanticBoundary &Recorded : Boundaries) if(Recorded.Before >= Idx) ++Recorded.Before;
			if(Idx) {
				const std::vector<SemanticBoundary>::iterator At = std::lower_bound(
					Boundaries.begin(),
					Boundaries.end(),
					Idx,
					[](const SemanticBoundary &Recorded, const uint32_t Wanted) -> bool {
						// 子の番号の大小の返戻
						return Recorded.Before < Wanted;
					}
				);
				SemanticBoundary Inserted = RecordedBoundary.value_or(SemanticBoundary{ Idx, Boundary });
				Inserted.Before = Idx;
				Boundaries.insert(At, std::move(Inserted));
			}
		}
		// 終了
		return;
	}

	/**
	 * 識別子を保ったノードの組直関数（整形仕様 3.2）
	 * 対象のノードの種別と子を生成ノードの定義へ替え，既存ノードを新しい位置へ接続する
	 * 残さない子のアンカーは指定した移譲先へ移す
	 * 束縛の変更記録の総費用を Q とする
	 * 計算量：生成・除去部品数 G，トークン・境界のコピー量 B と既存ノードの切離費用 D に対し O(G + B + D + Q)
	 * @param Step 組直の操作
	 * @param HasDetachedChildren 計画全体で移す既存の子を切離済か
	 * @param Trace アンカーの移譲の記録の追加先
	 * @throws FormatLimitExceeded 文法が其の種別を持たない場合，又は束縛の記録が作業予算を超えた場合
	 * @throws std::bad_alloc 生成ノード・作業列・変更範囲又はアンカーのメモリを確保出来ない場合
	 * @throws std::length_error 生成ノード・作業列又はアンカーの要素数上限を超える場合
	 * @throws std::out_of_range 組み直すノードがノード表に無い場合
	 */
	void SyntaxModel::ReshapeNode(const RewriteStep &Step, const bool HasDetachedChildren, RewriteTrace &Trace) {
		// 識別子を保ったノードの組直
		Touch(Step.Target);
		// 一括組立の計画が既に切り離した子は再走査しない
		if(!HasDetachedChildren) {
			// 移す既存のノード
			std::vector<NodeId> &Reused = RewriteReused;
			Reused.clear();
			std::vector<const GeneratedNode *> &Pending = RewritePending;
			Pending.clear();
			Pending.push_back(&Step.Inserted);
			while(!Pending.empty()) {
				const GeneratedNode *const Generated = Pending.back();
				Pending.pop_back();
				Charge(1);
				if(Generated->Existing != NoNode) Reused.push_back(Generated->Existing);
				for(const GeneratedNode &Child : Generated->Children) Pending.push_back(&Child);
			}
			DetachBatch(Reused);
			Reused.clear();
		}
		// 残さない子の部分木の除去とアンカーの移譲
		for(std::vector<NodeId> Stack(Nodes.at(Step.Target).Children.begin(), Nodes.at(Step.Target).Children.end()); !Stack.empty();) {
			Charge(1);
			const NodeId Removed = Stack.back();
			Stack.pop_back();
			Touch(Removed);
			IsRemoved[Removed] = 1;
			Trace.Transfers.push_back(Step.Transfer);
			Trace.Transfers.back().From = Removed;
			Stack.insert(Stack.end(), Nodes[Removed].Children.begin(), Nodes[Removed].Children.end());
		}
		// 対象の種別と子の組直
		SyntaxNode &Node = Nodes[Step.Target];
		const FormPart &Part = GetDefFor(Step.Inserted);
		UpdateNamedChildCount(Node.Parent, Node.Kind.IsNamed(), Part.Def->IsNamed);
		Node.Kind = { Part.Def };
		// テキストも種別のテキストも指定しない葉の組直は識別子と共に元のテキストを保つ
		if(!Step.Inserted.Children.empty()) Node.Lexeme.clear();
		else if(!Step.Inserted.Lexeme.empty()) Node.Lexeme = Step.Inserted.Lexeme;
		else if(Step.Inserted.Src != NoNode) Node.Lexeme = Nodes[Step.Inserted.Src].Lexeme;
		else if(!Step.Inserted.Kind.empty()) Node.Lexeme = Step.Inserted.Kind;
		Charge(Node.Lexeme.size());
		// テキストの位置に依る事実は同じ位置に留まる組替の前後で保つ
		const bool HasFalseName = Node.Facts.HasFalseName, HasTrueName = Node.Facts.HasTrueName;
		for(const SemanticBoundary &Boundary : Step.Inserted.Facts.Boundaries.GetView()) Charge(Boundary.Gap.size() + 1);
		Node.Facts = Step.Inserted.Facts;
		Node.Facts.Spelling.reset();
		Node.Facts.BlankKey = Part.BlankKey;
		Node.Facts.Forms = Part.Forms;
		Node.Facts.Meanings |= Part.Meanings;
		Node.Facts.TokenDef = Part.TokenDef;
		Node.Facts.HasFalseName = HasFalseName;
		Node.Facts.HasTrueName = HasTrueName;
		InvalidateBindingLocation(Step.Target);
		MarkOrderChange(Step.Target, 0);
		Node.Children.clear();
		if(Step.Target < NamedChildCounts.size()) NamedChildCounts[Step.Target] = 0;
		for(uint32_t Child = 0; Child < Step.Inserted.Children.size(); ++Child) {
			const NodeId ChildIdentifier = AddGenerated(Step.Inserted.Children[Child], Step.Target, Child);
			Nodes[Step.Target].Children.push_back(ChildIdentifier);
			UpdateNamedChildCount(Step.Target, false, IsNamed(ChildIdentifier));
		}
		if(Node.Lexeme.empty() && Node.Children.size() == 1) Node.Facts.Meanings = Nodes[Node.Children.front()].Facts.Meanings;
		// 終了
		return;
	}

	/**
	 * ラッパーの除去関数（整形仕様 3.2）
	 * 名前付の子が１つなら其れを選び，其の他は子が３つの時に中央の子を中身とする
	 * 中身をラッパーの親の同じ位置・フィールドへ移し，ラッパーと其の他の子を除く
	 * ラッパーの先行・内部のアンカーと，中身の前のトークンのアンカーは中身の前へ移す
	 * ラッパーの末尾のアンカーと，中身の後のトークンのアンカーは中身の後へ移す
	 * 計算量：ラッパーの子の数 C と束縛の変更記録の総費用 Q に対し O(C + Q)
	 * @param Id ラッパーのノード
	 * @param Trace アンカーの移譲の記録の追加先
	 * @param ShouldMarkBinding 中身の移動が束縛に作用するか（値を包む括弧だけなら偽）
	 * @throws FormatLimitExceeded ラッパーが中身を１つに決めれない場合，又は束縛の記録が作業予算を超えた場合
	 * @throws std::bad_alloc 変更範囲・変更ノード又はアンカーのメモリを確保出来ない場合
	 * @throws std::length_error 変更ノード・アンカー又は構文分類の作業列の要素数上限を超える場合
	 * @throws std::out_of_range 照会するノードの識別子がノード表に無い場合
	 */
	void SyntaxModel::UnwrapNode(const NodeId Id, RewriteTrace &Trace, const bool ShouldMarkBinding) {
		// ラッパーの除去
		if(ShouldMarkBinding) MarkBindingChange(Id);
		MarkOrderChange(Nodes[Id].Parent, Nodes[Id].Idx);
		Touch(Id);
		Touch(Nodes[Id].Parent);
		// 唯一の名前付の子，又は３つの子の中央の子の選択
		NodeId Content = NoNode;
		uint32_t NamedCount = 0;
		const size_t TransferStart = Trace.Transfers.size();
		for(const NodeId Child : GetChildrenOf(Id)) {
			Charge(1);
			Trace.Transfers.push_back({ Child, {}, {}, {} });
			if(!IsNamed(Child)) continue;
			if(!NamedCount) Content = Child;
			++NamedCount;
		}
		if(NamedCount != 1) Content = GetChildCount(Id) == 3 ? GetChild(Id, 1) : NoNode;
		if(Content == NoNode) throw FormatLimitExceeded("an unwrapped node has no single content", FailureReason::FormatterDefect);
		const AnchorTarget Before { Content, false }, After { Content, true };
		Touch(Content);
		// 収集済の移譲記録を確定し，中身自身の記録だけを詰める
		size_t Kept = TransferStart;
		for(size_t Pos = TransferStart; Pos < Trace.Transfers.size(); ++Pos) {
			Charge(1);
			const NodeId Child = Trace.Transfers[Pos].From;
			if(Child == Content) continue;
			Touch(Child);
			IsRemoved[Child] = 1;
			const AnchorTarget &Target = Nodes[Child].Idx < Nodes[Content].Idx ? Before : After;
			Trace.Transfers[Kept++] = { Child, Target, Target, Target };
		}
		Trace.Transfers.resize(Kept);
		Trace.Transfers.push_back({ Id, Before, After, Before });
		// 中身に依るラッパーの置換
		InvalidateBindingLocation(Id);
		SyntaxNode &Wrapper = Nodes[Id], &Moved = Nodes[Content];
		Moved.Parent = Wrapper.Parent;
		Moved.Fields = Wrapper.Fields;
		Moved.Idx = Wrapper.Idx;
		if(Wrapper.Parent == NoNode) Root = Content;
		else {
			Nodes[Wrapper.Parent].Children[Wrapper.Idx] = Content;
			UpdateNamedChildCount(Wrapper.Parent, Wrapper.Kind.IsNamed(), Moved.Kind.IsNamed());
		}
		// 除いたラッパーの親子の切離（後の操作がラッパーの中身の位置を正しく読む為）
		Wrapper.Parent = NoNode;
		Wrapper.Children.clear();
		if(Id < NamedChildCounts.size()) NamedChildCounts[Id] = 0;
		IsRemoved[Id] = 1;
		// 終了
		return;
	}

	/**
	 * 登録済トークン全体の役割集合の取得関数
	 * @param Text トークン全体の綴り
	 * @return 登録した綴りの役割集合又は無
	 */
	const RoleSet *SyntaxRules::GetTokenRoles(const std::string_view Text) const {
		if(Text.size() == 1) {
			const KindDef *const Kind = SingleByteTokens[static_cast<unsigned char>(Text.front())];
			// 同じ登録定義の役割の返戻
			return Kind ? &Kind->Roles : nullptr;
		}
		const std::unordered_map<std::string_view, FormPart>::const_iterator Found = TokenDefs.find(Text);
		// 登録した綴りの役割集合の返戻
		return Found == TokenDefs.end() ? nullptr : &Found->second.Def->Roles;
	}

	/**
	 * 登録済トークンの綴りの役割の判定関数
	 * @param Text トークン全体の綴り
	 * @param Wanted 判定する役割
	 * @return 登録したトークン全体が役割を持てば true
	 */
	bool SyntaxRules::HasTokenRole(const std::string_view Text, const Role Wanted) const {
		const RoleSet *const Roles = GetTokenRoles(Text);
		// 登録した綴りに対応する役割の返戻
		return Roles && Roles->test(static_cast<size_t>(Wanted));
	}

	/**
	 * 名前のテキストの初回走査関数
	 * 計算量：テキストのバイト数 B に対し O(B)
	 * @param Spelling 投影又は編集で確定したテキスト
	 * @return 非 ASCII とエスケープの開始記号の有無
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 */
	NameSpelling SyntaxModel::ReadNameSpelling(const std::string_view Spelling) const {
		// 名前のテキストの初回走査
		Charge(Spelling.size());
		NameSpelling Result;
		for(const unsigned char Char : Spelling) {
			Result.HasNonAscii |= Char > 0X7F;
			Result.HasEscape |= Char == '\\';
		}
		// 名前のテキストの事実の返戻
		return Result;
	}

	/**
	 * 比較名を記録・照会するノードの共通判定関数
	 * 完全名は宣言・インポートの直接の名前だけを対象とし，修飾名の内側の接尾部分木は含めない
	 * 計算量：名前付インポートの子は直接子数 C に対し O(C)，其の他は O(1)
	 * @param Id 判定するノード
	 * @return 名前の役割又は比較名を必要とする直接のフィールドを持てば真
	 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
	 * @throws std::out_of_range 読むノードの番号が範囲外の場合
	 */
	bool SyntaxModel::IsComparisonName(const NodeId Id) const {
		// ノードが無い場合は名前でない事の返戻
		if(Id == NoNode) return false;
		// 名前の役割で決まる対象は，完全名の場を照会し直さない
		if(
			Is(Id, Role::NameToken) || Is(Id, Role::ParamName) || Is(Id, Role::TypeName) || Is(Id, Role::NamespaceName) ||
			Is(Id, Role::BindingName)
		) return true;
		const bool IsLeaf = GetChildrenOf(Id).empty();
		// 葉の名前フィールドだけで決まる対象の返戻
		if(IsLeaf && HasField(Id, ChildRole::Name)) return true;
		// 無名の葉は完全名の対象でなく，残る束縛の分類だけを照会する
		if(IsLeaf && !IsNamed(Id)) return GetBindingSyntax(Id) == BindingSyntax::Name;
		const NodeId Parent = GetParentOf(Id);
		const bool IsWholeName = IsNamed(Id) && Is(Parent, Role::NamedImports) && GetFieldChild(Id, ChildRole::Name) == NoNode ||
		!IsLeaf && HasField(Id, ChildRole::Name) && (
			GetBindingSyntax(Parent) == BindingSyntax::ModuleDecl || IsAny(
				Parent,
				{
					Role::ClassLike,
					Role::Decl,
					Role::FieldDef,
					Role::FunctionDeclarator,
					Role::FunctionScope,
					Role::ImportDeclNode,
					Role::ImportItem,
					Role::ImportPathItem,
					Role::MethodDecl,
					Role::NamedParamKind,
					Role::NamespaceDef,
					Role::TypedDecl,
					Role::VarDeclarator
				}
			)
		);
		// 投影と名前の照会で共有する対象の返戻
		return IsWholeName || GetBindingSyntax(Id) == BindingSyntax::Name;
	}

	/**
	 * 投影済の識別子の比較用の名前番号の取得関数
	 * 計算量：期待値 O(1)，比較でテキストを読まない
	 * @param Id 識別子又は名前の位置に在るノード
	 * @return 同じモデルの同じ綴りに共通の番号
	 * @throws FormatLimitExceeded 比較用の名前が未投影の場合又は作業予算超過の場合
	 * @throws std::out_of_range ノードがノード表に無い場合
	 */
	uint32_t SyntaxModel::GetCmpNameIdx(const NodeId Id) const {
		// 投影済の識別子の比較用の名前番号の取得
		Charge(1);
		const uint32_t Idx = GetNode(Id).Facts.NameIdx;
		if(!Idx) throw FormatLimitExceeded("an identifier comparison name was not projected", FailureReason::FormatterDefect);
		// 比較用の名前番号の返戻
		return Idx;
	}

	/**
	 * 製品の予算を変更しない所有済の比較名の読取関数
	 * 観測は此の読取を使い，製品の照会は計上する入口を使う
	 * 計算量：O(1)
	 * @param Id 比較名を投影済のノード
	 * @return 名前表が所有するテキストの参照
	 * @throws FormatLimitExceeded 比較名が未投影の場合
	 * @throws std::out_of_range ノード又は名前番号が表に無い場合
	 */
	std::string_view SyntaxModel::ReadCmpNameView(const NodeId Id) const {
		const SyntaxNode &Node = GetNode(Id);
		if(!Node.Facts.NameIdx) {
			throw FormatLimitExceeded("an identifier comparison name was not projected", FailureReason::FormatterDefect);
		}
		// 既に正規化した名前の返戻
		return CmpNames.at(Node.Facts.NameIdx - 1);
	}

	/**
	 * 比較用の名前の所有テキスト参照の取得関数
	 * 識別子・フィールド名・特殊変数の比較名は投影済の共有表から読む
	 * 計算量：期待値 O(1)
	 * @param Id 比較する名前のノード
	 * @return モデルが所有する名前への参照（モデルの名前表を書き換える迄有効）
	 * @throws FormatLimitExceeded 識別子の比較名が未投影の場合又は作業予算超過の場合
	 * @throws std::out_of_range ノード又は名前番号が表に無い場合
	 */
	std::string_view SyntaxModel::GetCmpNameView(const NodeId Id) const {
		Charge(1);
		// 計上した名前の照会の返戻
		return ReadCmpNameView(Id);
	}

	/**
	 * 部分木の最初のトークンと字の取得関数
	 * 計算量：対象部分木のノード数 N に対し O(N)
	 * @param Id ノードの識別子
	 * @return 最初のトークンの葉と字（無ければノード無と零）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	LexicalEdge SyntaxModel::GetFirstEdge(const NodeId Id) const {
		// 既存のトークンだけで決まる端には作業列を作らない
		if(Id >= GetNodeCount()) return {};
		if(!GetLexemeOf(Id).empty()) {
			Charge(1);
			// 対象自身のトークンの端の返戻
			return { Id, static_cast<unsigned char>(GetLexemeOf(Id).front()) };
		}
		if(GetChildrenOf(Id).empty()) {
			Charge(1);
			// 空の葉の返戻
			return {};
		}
		// 部分木の最初のトークンと字の取得
		std::vector<NodeId> Stack { Id };
		while(!Stack.empty()) {
			const NodeId Top = Stack.back();
			Stack.pop_back();
			if(Top >= GetNodeCount()) continue;
			// 辿ったノードの計上（整形仕様 9.2）
			Charge(1);
			// テキストを持つ最初のトークンの頭の字の返戻
			if(!GetLexemeOf(Top).empty()) return { Top, static_cast<unsigned char>(GetLexemeOf(Top).front()) };
			Stack.insert(Stack.end(), GetChildrenOf(Top).rbegin(), GetChildrenOf(Top).rend());
		}
		// テキストが無い事の返戻
		return {};
	}

	/**
	 * 部分木の最後のトークンと字の取得関数
	 * 計算量：対象部分木のノード数 N に対し O(N)
	 * @param Id ノードの識別子
	 * @return 最後のトークンの葉と字（無ければノード無と零）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	LexicalEdge SyntaxModel::GetLastEdge(const NodeId Id) const {
		// 既存のトークンだけで決まる端には作業列を作らない
		if(Id >= Nodes.size()) return {};
		if(!Nodes[Id].Lexeme.empty()) {
			Charge(1);
			// 対象自身のトークンの端の返戻
			return { Id, static_cast<unsigned char>(Nodes[Id].Lexeme.back()) };
		}
		if(Nodes[Id].Children.empty()) {
			Charge(1);
			// 空の葉の返戻
			return {};
		}
		// 部分木の最後のトークンと字の取得
		std::vector<NodeId> Stack { Id };
		while(!Stack.empty()) {
			const NodeId Top = Stack.back();
			Stack.pop_back();
			if(Top >= Nodes.size()) continue;
			Charge(1);
			// テキストを持つ最後のトークンの末尾の字の返戻
			if(!Nodes[Top].Lexeme.empty()) return { Top, static_cast<unsigned char>(Nodes[Top].Lexeme.back()) };
			Stack.insert(Stack.end(), Nodes[Top].Children.begin(), Nodes[Top].Children.end());
		}
		// テキストが無い事の返戻
		return {};
	}

	/**
	 * 生きた呼出先の要約の解除関数
	 * 計算量：O(1)
	 * @param Node 解除する要約のノード
	 * @return 生きた要約を解除した場合は true
	 * @throws FormatLimitExceeded 解除の作業予算を超える場合
	 */
	bool SyntaxModel::IsCalleeErasureSuccessful(const NodeId Node) const {
		// 未取得又は解除済の要約の返戻
		if(Node >= Callees.size() || !Callees[Node]) return false;
		Charge(1);
		Callees[Node].reset();
		--LiveCallees;
		// 生きた要約を解除した事の返戻
		return true;
	}

	/**
	 * 変更したノードから祖先の呼出先の要約の解除関数
	 * 計算量：解除する祖先の要約数 H に対し O(H + 1)
	 * @param Node テキスト又は子構造が変わる対象
	 * @throws FormatLimitExceeded 解除の作業予算を超える場合
	 */
	void SyntaxModel::InvalidateCallees(const NodeId Node) const {
		for(NodeId Up = Node; Up != NoNode && IsCalleeErasureSuccessful(Up); Up = GetParentOf(Up)) {}
		// 終了
		return;
	}

	/**
	 * 書換の影響範囲の保持の終了関数
	 * 計算量：変更部分木の総ノード数 S，祖先の総段数 H，解放する事実数 F に対し O(S + H + F)
	 * @param Plan 此の構文へ適用する書換
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 変更部分木の作業列を確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノードの識別子がノード表に無い場合
	 */
	void SyntaxModel::InvalidateFacts(const RewritePlan &Plan) {
		// 書換の影響範囲の保持の終了
		#if defined(ShaveFormatRefFacts)
		static_cast<void>(Plan);
		#else
		const SyntaxModel &Syntax = *this;
		// 定義の判定は，種別・親子・兄弟の順が変わる所有範囲だけ失効する
		if(!Documentation.empty()) {
			std::unordered_set<NodeId> Owners;
			const auto Include = [&](const NodeId Node) -> void {
				if(Node != NoNode) {
					Owners.insert(Node);
					Owners.insert(Syntax.GetParentOf(Node));
				}
				// 終了
				return;
			};
			for(const RewriteStep &Step : Plan.Steps) {
				if(Step.Kind == RewriteKind::SetLexeme || Step.Kind == RewriteKind::Retarget) continue;
				Include(Step.Target);
				Include(Step.Dst);
				std::vector<const GeneratedNode *> Pending { &Step.Inserted };
				while(!Pending.empty()) {
					const GeneratedNode &Node = *Pending.back();
					Pending.pop_back();
					Syntax.Charge(1);
					if(Node.Existing != NoNode) Include(Node.Existing);
					else for(const GeneratedNode &Child : Node.Children) Pending.push_back(&Child);
				}
			}
			for(const NodeId Owner : Owners) {
				Syntax.Charge(1);
				Documentation.erase(Owner);
				if(
					const std::unordered_map<NodeId, std::unordered_map<NodeId, DocumentationFact>>::iterator Parent =
					Documentation.find(Syntax.GetParentOf(Owner));
					Parent != Documentation.end()
				) {
					Parent->second.erase(Owner);
					if(Parent->second.empty()) Documentation.erase(Parent);
				}
				// ラッパーの外側の所属が変わると，其の直下の定義も影響を受ける
				for(const NodeId Child : Syntax.GetChildrenOf(Owner)) {
					Syntax.Charge(1);
					Documentation.erase(Child);
				}
			}
		}
		// 後置連鎖の要約は直接の受手へ依存し，未照会の親より先へ依存を飛ばさない
		const auto ErasePostfix = [&](const NodeId Node) -> size_t {
			// 未照会なら失効する物が無い事の返戻
			if(!Normalization.Parens) return 0;
			Charge(1);
			NormalizationFacts::ParenScanMemo &Memo = Normalization.Parens->Scan;
			const size_t Calls = Memo.ConstructionCalls.erase(Node), Await = Memo.LeadingAwait.erase(Node);
			const size_t Atoms = Memo.PostfixAtoms.erase(Node), Lead = Memo.PostfixLead.erase(Node);
			const size_t Conditional = Memo.ConditionalReceivers.erase(Node);
			// 失効した要約数の返戻
			return Calls + Await + Atoms + Lead + Conditional;
		};
		// 外した括弧の控えの失効
		const auto EraseParen = [&](const NodeId Node, const bool HasAngleChange, const bool HasTypeChange) -> void {
			// 括弧の事実を未だ取得して居ない場合の終了
			if(!Normalization.Parens) return;
			NormalizationFacts::ParenCtx &Facts = *Normalization.Parens;
			Facts.Prec.erase(Node);
			if(HasAngleChange) Facts.Angles.erase(Node);
			Facts.BraceHeaders.erase(Node);
			Facts.UnnamedAncestors.erase(Node);
			Facts.Scan.After.erase(Node);
			Facts.Scan.BlockLead.erase(Node);
			Facts.Scan.BlockTop.erase(Node);
			Facts.Scan.FirstLeaf.erase(Node);
			Facts.Scan.OperatorTop.erase(Node);
			Facts.Scan.LastLeaf.erase(Node);
			Facts.Scan.DeclArgs.erase(Node);
			Facts.Scan.DeclCalls.erase(Node);
			Facts.Scan.LeftConstraint.erase(Node);
			Facts.Scan.RightJump.erase(Node);
			if(HasTypeChange) {
				Facts.Scan.TypeEdges.erase(Node);
				ErasePostfix(Node);
			}
			// 終了
			return;
		};
		constexpr uint8_t BarrierInput = 1, ValInput = 2;
		std::unordered_map<NodeId, uint8_t> Visited;
		std::unordered_set<NodeId> Ancestors;
		for(const RewriteStep &Step : Plan.Steps) {
			if(Step.Kind == RewriteKind::Retarget) continue;
			const bool IsStructure = Step.Kind != RewriteKind::SetLexeme;
			const bool IsTransparentWrapper = Step.Kind == RewriteKind::Unwrap &&
			(Syntax.Is(Step.Target, Role::GroupingParen) || Syntax.Is(Step.Target, Role::GroupingTypeParen)) ||
			Step.Kind == RewriteKind::Wrap && Step.Inserted.Def.Def && (
				Step.Inserted.Def.Def->Roles.test(static_cast<size_t>(Role::GroupingParen)) ||
				Step.Inserted.Def.Def->Roles.test(static_cast<size_t>(Role::GroupingTypeParen))
			);
			if(IsTransparentWrapper) {
				// 内側の確定済の要約を保ち，括弧の有無へ依存する親の要約だけを失効する
				ErasePostfix(Step.Target);
				for(NodeId Up = Syntax.GetParentOf(Step.Target); Up != NoNode; Up = Syntax.GetParentOf(Up)) if(!ErasePostfix(Up)) break;
				EraseParen(Step.Target, true, false);
				EraseParen(Syntax.GetNamedChild(Step.Target, 0), false, false);
				EraseParen(Syntax.GetParentOf(Step.Target), false, false);
			}
			// 文の並びへの値・名前を新しく導入しない返戻の追加は，既存の束縛を変えない
			bool HasBindingChanges = true;
			if(
				Step.Kind == RewriteKind::Insert && Syntax.Is(Step.Target, Role::StmtSeq) &&
				Syntax.GetDefFor(Step.Inserted).Def->Roles.test(static_cast<size_t>(Role::ReturnStmt))
			) {
				HasBindingChanges = false;
				for(std::vector<const GeneratedNode *> Pending { &Step.Inserted }; !Pending.empty();) {
					const GeneratedNode &Added = *Pending.back();
					Pending.pop_back();
					Syntax.Charge(1);
					const KindDef *const Def = Syntax.GetDefFor(Added).Def;
					const WordSet Meanings = Syntax.GetDefFor(Added).Meanings | Added.Facts.Meanings;
					if(
						Added.Existing != NoNode || Added.Src != NoNode || !Def || Def->Binding != BindingSyntax::None ||
						Def->Roles.test(static_cast<size_t>(Role::BindingMarker)) || Meanings.test(static_cast<size_t>(Word::FunctionScopedBinding)) ||
						Meanings.test(static_cast<size_t>(Word::ConstantDecl))
					) {
						HasBindingChanges = true;
						break;
					}
					for(const GeneratedNode &Child : Added.Children) Pending.push_back(&Child);
				}
			}
			if(!Normalization.FlowFacts) for(const NodeId Node : { Step.Target, Step.Dst }) {
				if(Node == NoNode) continue;
				InvalidateCallees(Node);
				if(HasBindingChanges && IsStructure && !IsTransparentWrapper && LiveCallees) {
					HasStoppedTraversal(
						Syntax,
						Node,
						[&](const NodeId Part) -> WalkStep {
							IsCalleeErasureSuccessful(Part);
							// 変更部分木の呼出先の要約の解除の返戻
							return WalkStep::Descend;
						}
					);
				}
			} else if(IsTransparentWrapper) {
				Normalization.FlowFacts->InvalidateCallees(Step.Target);
				if(Step.Dst != NoNode) Normalization.FlowFacts->InvalidateCallees(Step.Dst);
			} else {
				Normalization.FlowFacts->Invalidate(Step.Target, IsStructure, HasBindingChanges);
				if(Step.Dst != NoNode) Normalization.FlowFacts->Invalidate(Step.Dst, IsStructure, HasBindingChanges);
			}
			// テキストだけの書換と，未だ使って居ない事実は解放を要しない
			if(
				!IsStructure && !Normalization.Parens ||
				Normalization.NormalizationBarriers.empty() && Normalization.ValFacts.empty() && !Normalization.Parens
			) continue;
			if(IsTransparentWrapper) {
				Normalization.ValFacts.erase(Step.Target);
				if(Step.Kind == RewriteKind::Unwrap) {
					Normalization.NormalizationBarriers.erase(Step.Target);
					Normalization.ValFacts.erase(Syntax.GetNamedChild(Step.Target, 0));
				}
				// ラッパーの内側の障壁と値の位置は其の親を保持する
				continue;
			}
			// 移動先の既存の子と，一括除去で残す兄弟の祖先は変わらない
			std::vector<std::pair<NodeId, uint8_t>> Pending;
			switch(Step.Kind) {
			case RewriteKind::Insert:
				{
					std::vector<const GeneratedNode *> Generated { &Step.Inserted };
					while(!Generated.empty()) {
						const GeneratedNode &Node = *Generated.back();
						Generated.pop_back();
						Syntax.Charge(1);
						if(Node.Existing != NoNode) Pending.emplace_back(Node.Existing, BarrierInput | ValInput);
						for(const GeneratedNode &Child : Node.Children) Generated.push_back(&Child);
					}
					break;
				}
			case RewriteKind::RemoveSiblings:
				for(const AnchorTransfer &Transfer : Step.RemovedSiblings) Pending.emplace_back(Transfer.From, BarrierInput | ValInput);
				break;
			case RewriteKind::Remove:
			case RewriteKind::Reshape:
				Pending.emplace_back(Step.Target, BarrierInput | ValInput);
				break;
			default:
				Pending.emplace_back(Step.Target, BarrierInput | ValInput);
				break;
			}
			for(const NodeId Root : { Step.Target, Step.Dst }) {
				if(Root == NoNode) continue;
				for(NodeId Up = Root; Up != NoNode && Ancestors.insert(Up).second; Up = Syntax.GetParentOf(Up)) {
					Syntax.Charge(1);
					Normalization.ValFacts.erase(Up);
					EraseParen(Up, true, true);
					if(Syntax.Is(Up, Role::TailValFunction)) break;
				}
				if(
					const NodeId Sequence = Syntax.Is(Root, Role::StmtSeq) ? Root : Syntax.GetParentOf(Root);
					Sequence != NoNode && Syntax.Is(Sequence, Role::StmtSeq)
				) if(const NodeId Last = Syntax.GetLastNamedChild(Sequence); Last != NoNode) {
					Pending.emplace_back(Last, ValInput);
					if(const NodeId Prev = Syntax.GetPrevNamedSibling(Last); Prev != NoNode) Pending.emplace_back(Prev, ValInput);
				}
			}
			// 読取る入力が変わった事実の解放
			while(!Pending.empty()) {
				const std::pair<NodeId, uint8_t> Cur = Pending.back();
				Pending.pop_back();
				if(Cur.first == NoNode) continue;
				uint8_t &Prev = Visited[Cur.first];
				const uint8_t Changed = Cur.second & ~Prev;
				if(!Changed) continue;
				Prev |= Changed;
				Syntax.Charge(1);
				EraseParen(Cur.first, true, true);
				if(Changed & BarrierInput) Normalization.NormalizationBarriers.erase(Cur.first);
				if(Changed & ValInput) Normalization.ValFacts.erase(Cur.first);
				for(const NodeId Child : Syntax.GetChildrenOf(Cur.first)) Pending.emplace_back(Child, Changed);
			}
		}
		#endif
		// 終了
		return;
	}

	/**
	 * 書換の１操作の適用関数
	 * 対象のノードと子の並び・境界・事実を一体で更新し，除いたノードのアンカーの行先を記録する
	 * 値を囲む括弧の追加・除去では束縛の変更を記録しない
	 * 名前・書込先・領域構成を変える操作だけが束縛の失効費用 Q を負う
	 * 計算量：操作固有の編集費用 E と束縛の失効範囲の記録費用 Q に対し O(E + Q)
	 * @param Step 書換の操作
	 * @param HasDetachedChildren 計画全体で移す既存の子を切離済か
	 * @param Trace アンカーの移譲の記録の追加先
	 * @param RemovedRoots 除いた部分木の根のアンカーの行先の追加先（内部のアンカーは全ての操作の後に移す）
	 * @throws FormatLimitExceeded 纏めて除く子が対象の子でない場合，文法の種別が無い場合，又は作業予算を超えた場合
	 * @throws std::bad_alloc ノード・生成の作業列・変更記録又はアンカーのメモリを確保出来ない場合
	 * @throws std::length_error ノードのコピー・作業列又はアンカーの要素数上限を超える場合
	 * @throws std::out_of_range 指定したノードがノード表に無い場合
	 */
	void SyntaxModel::ApplyStep(
		const RewriteStep &Step,
		const bool HasDetachedChildren,
		RewriteTrace &Trace,
		std::vector<AnchorTransfer> &RemovedRoots
	) {
		// 実際の書換対象が供給する束縛構造の変更前の記録
		bool ShouldMarkBinding = true;
		switch(Step.Kind) {
		case RewriteKind::Insert:
		case RewriteKind::RebuildChildren:
		case RewriteKind::Retarget:
		case RewriteKind::RemoveSiblings:
			ShouldMarkBinding = false;
			break;
		case RewriteKind::SetLexeme:
		case RewriteKind::Reshape:
			{
				// 導入語の可変・不変の切替は，名前の導入・探索・書込の構造を変えない
				if(
					const WordSet Meanings = Step.Kind == RewriteKind::SetLexeme ?
					Nodes.at(Step.Target).Facts.Meanings :
					GetDefFor(Step.Inserted).Meanings | Step.Inserted.Facts.Meanings;
					GetBindingSyntax(Step.Target) == BindingSyntax::None && Nodes.at(Step.Target).Children.empty() && (
						Step.Kind == RewriteKind::SetLexeme ||
						Step.Inserted.Children.empty() && GetDefFor(Step.Inserted).Def->Binding == BindingSyntax::None
					) && (IsWord(Step.Target, Word::ReassignableBinding) || IsWord(Step.Target, Word::ImmutableBinding)) &&
					!IsWord(Step.Target, Word::FunctionScopedBinding) &&
					(Meanings.test(static_cast<size_t>(Word::ReassignableBinding)) || Meanings.test(static_cast<size_t>(Word::ImmutableBinding)))
				) ShouldMarkBinding = false;
				break;
			}
		case RewriteKind::Wrap:
			{
				// 値の括弧は導入名と書込先の探索で単一の中身へ透過する
				switch(const KindDef &Wrapper = *GetDefFor(Step.Inserted).Def; Wrapper.Binding) {
				case BindingSyntax::None:
				case BindingSyntax::RestPattern:
					ShouldMarkBinding =
					!(Wrapper.Roles.test(static_cast<size_t>(Role::GroupingParen)) || Wrapper.Roles.test(static_cast<size_t>(Role::ValParens)));
					break;
				}
				break;
			}
		case RewriteKind::Unwrap:
			// 束縛名と書込先の探索が透過する単一内容の括弧の判定
			switch(GetBindingSyntax(Step.Target)) {
			case BindingSyntax::None:
			case BindingSyntax::RestPattern:
				ShouldMarkBinding = !(Is(Step.Target, Role::GroupingParen) || Is(Step.Target, Role::ValParens));
				break;
			}
			break;
		}
		// ラッパーの除去は，移す中身と外側を含む変更前の範囲を UnwrapNode が１度失効する
		if(ShouldMarkBinding && Step.Kind != RewriteKind::Unwrap) MarkBindingChange(Step.Target);
		if(Step.Kind == RewriteKind::Reshape) InvalidateBindingLocation(Step.Target);
		// アンカーの移譲は構文・役割・テキストを変えない為，変更ノードとして再登録しない
		if(Step.Kind != RewriteKind::Retarget) {
			Touch(Step.Target);
			Touch(Nodes.at(Step.Target).Parent);
			Touch(Step.Dst);
		}
		// 操作に対応するノードとアンカーの更新
		switch(Step.Kind) {
		case RewriteKind::SetLexeme:
			Charge(Step.Text.size());
			Nodes[Step.Target].Lexeme = Step.Text;
			Nodes[Step.Target].Facts.HasCodeNewline.reset();
			if(GetBindingSyntax(Step.Target) == BindingSyntax::Name || Nodes[Step.Target].Kind.Def->Binding == BindingSyntax::TypeName) {
				Nodes[Step.Target].Facts.DoesDiscardBinding = IsWord(Step.Target, Word::DiscardName);
				std::string Name = Step.Text;
				if(
					const std::string_view Quote = GetRules().BindingNameQuote;
					!Quote.empty() && Name.size() >= Quote.size() << 1 && Name.starts_with(Quote) && Name.ends_with(Quote)
				) Name = Name.substr(Quote.size(), Name.size() - (Quote.size() << 1));
				Nodes[Step.Target].Facts.NameIdx = 0;
				if(!Name.empty() && Name.find('\\') == std::string::npos) {
					Nodes[Step.Target].Facts.NameIdx = InternCmpName(std::move(Name), true);
				}
			}
			break;
		case RewriteKind::Insert:
			Attach(AddGenerated(Step.Inserted, Step.Target, Step.Idx), Step.Target, Step.Idx, Step.Boundary, Step.RecordedBoundary);
			break;
		case RewriteKind::Wrap:
			{
				// 対象の位置・フィールドと，原文の間隙を保つ前の境界をラッパーへ引き継ぐ（切り離す前に控える）
				const NodeId Host = Nodes.at(Step.Target).Parent;
				const uint32_t Pos = Nodes[Step.Target].Idx;
				const uint64_t Roles = Nodes[Step.Target].Fields;
				const SemanticBoundary *const Before = Host == NoNode ? nullptr : Nodes[Host].Facts.GetBoundaryBefore(Pos);
				const std::optional<SemanticBoundary> Kept =
				Step.RecordedBoundary || !Before || Before->Kind != BoundaryKind::Preserved ? Step.RecordedBoundary : *Before;
				Detach(Step.Target);
				const NodeId Wrapper = AddGenerated(Step.Inserted, Host, Pos, ShouldMarkBinding);
				Nodes[Wrapper].Fields = Roles;
				Attach(Wrapper, Host, Pos, Step.Boundary, Kept);
				// 括弧の外側の先行・末尾のアンカーをラッパーの前後へ移譲（括弧の内側のコメントとの関係を保つ，整形仕様 7.2）
				if(Step.Transfer.From != NoNode) {
					Trace.Transfers.push_back({ Step.Target, { Wrapper, false }, { Wrapper, true }, { Step.Target, false } });
				}
				break;
			}
		case RewriteKind::RebuildChildren:
			{
				SyntaxNode &Node = Nodes[Step.Target];
				MarkOrderChange(Step.Target, 0);
				Node.Children.clear();
				if(Step.Target < NamedChildCounts.size()) NamedChildCounts[Step.Target] = 0;
				Node.Facts.Boundaries = Step.Inserted.Facts.Boundaries;
				for(uint32_t Idx = 0; Idx < Step.Inserted.Children.size(); ++Idx) {
					const NodeId Child = AddGenerated(Step.Inserted.Children[Idx], Step.Target, Idx);
					Nodes[Step.Target].Children.push_back(Child);
					UpdateNamedChildCount(Step.Target, false, IsNamed(Child));
				}
				break;
			}
		case RewriteKind::Reshape:
			ReshapeNode(Step, HasDetachedChildren, Trace);
			break;
		case RewriteKind::Retarget:
			Trace.Transfers.push_back(Step.Transfer);
			Trace.Transfers.back().From = Step.Target;
			break;
		case RewriteKind::Move:
			Detach(Step.Target);
			Attach(Step.Target, Step.Dst, Step.Idx, Step.Boundary, Step.RecordedBoundary);
			if(Step.DstFields) Nodes[Step.Target].Fields = *Step.DstFields;
			break;
		case RewriteKind::Unwrap:
			UnwrapNode(Step.Target, Trace, ShouldMarkBinding);
			break;
		case RewriteKind::Remove:
			Detach(Step.Target);
			IsRemoved[Step.Target] = 1;
			Trace.Transfers.push_back(Step.Transfer);
			Trace.Transfers.back().From = Step.Target;
			RemovedRoots.push_back(Trace.Transfers.back());
			break;
		case RewriteKind::RemoveSiblings:
			{
				MarkOrderChange(Step.Target, 0);
				std::pmr::vector<NodeId> &Children = Nodes[Step.Target].Children;
				Charge(Children.size());
				// 除く子の検査とマーカー付
				std::vector<uint8_t> Removed(Children.size(), 0);
				for(const AnchorTransfer &Transfer : Step.RemovedSiblings) {
					Charge(1);
					const SyntaxNode &Child = Nodes.at(Transfer.From);
					if(Child.Parent != Step.Target || Removed[Child.Idx]) {
						throw FormatLimitExceeded("a grouped removal has an invalid child", FailureReason::FormatterDefect);
					}
					MarkBindingChange(Transfer.From);
					Removed[Child.Idx] = 1;
				}
				Charge(Children.size() + 1);
				// 除去数の累積
				std::vector<uint32_t> Before(Children.size() + 1, 0);
				for(size_t Idx = 0; Idx < Children.size(); ++Idx) {
					Charge(1);
					Before[Idx + 1] = Before[Idx] + Removed[Idx];
				}
				// 境界の番号の詰直
				if(!Nodes[Step.Target].Facts.Boundaries.GetView().empty()) {
					std::vector<SemanticBoundary> &Boundaries = Nodes[Step.Target].Facts.Boundaries.Edit();
					Charge(Boundaries.size());
					std::erase_if(
						Boundaries,
						[&Removed](const SemanticBoundary &Boundary) -> bool {
							return Boundary.Before < Removed.size() && Removed[Boundary.Before];
						}
					);
					for(SemanticBoundary &Boundary : Boundaries) {
						Charge(1);
						Boundary.Before -= Before.at(Boundary.Before);
					}
				}
				// 残す子の並びの詰直
				uint32_t Kept = 0;
				for(uint32_t Idx = 0; Idx < Children.size(); ++Idx) {
					Charge(1);
					const NodeId Child = Children[Idx];
					if(Removed[Idx]) {
						UpdateNamedChildCount(Step.Target, IsNamed(Child), false);
						Touch(Child);
						InvalidateBindingLocation(Child);
						Nodes[Child].Parent = NoNode;
						IsRemoved[Child] = 1;
						continue;
					}
					Children[Kept] = Child;
					Nodes[Child].Idx = Kept++;
				}
				Children.resize(Kept);
				Trace.Transfers.insert(Trace.Transfers.end(), Step.RemovedSiblings.begin(), Step.RemovedSiblings.end());
				RemovedRoots.insert(RemovedRoots.end(), Step.RemovedSiblings.begin(), Step.RemovedSiblings.end());
				break;
			}
		}
		// 接続又は組直で変わった対象の変更後の記録
		if(ShouldMarkBinding && !IsRemoved[Step.Target]) MarkBindingChange(Step.Target);
		// 終了
		return;
	}

	/**
	 * 除くノードの内部のアンカーの移譲関数（整形仕様 3.2, 7.2）
	 * 除いたノードに繋がった儘の子孫は出力に現れない為，其のアンカーを除いたノードの内部のアンカーの行先へ移す
	 * 同じ計画で個別に除いた子孫（宣言の統合の部品等）は其の移譲に従う為に移さない
	 * 計算量：移譲する部分木のノード数 N に対し期待値 O(N)
	 * @param Transfer 除いたノードのアンカーの移譲
	 * @param Trace アンカーの移譲の記録の追加先
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノードの識別子がノード表に無い場合
	 */
	void SyntaxModel::TransferInnerAnchors(const AnchorTransfer &Transfer, RewriteTrace &Trace) {
		// 除くノードの内部のアンカーの移譲
		std::vector<NodeId> Pending(Nodes.at(Transfer.From).Children.begin(), Nodes.at(Transfer.From).Children.end());
		while(!Pending.empty()) {
			const NodeId Inner = Pending.back();
			Pending.pop_back();
			// 子孫の１回の検査を１作業単位とする（整形仕様 9.2）
			Charge(1);
			// 個別に除いた部分木の読飛（内部のアンカーは其の部分木の移譲が扱う）
			if(IsRemoved[Inner]) continue;
			Touch(Inner);
			IsRemoved[Inner] = 1;
			Trace.Transfers.push_back({ Inner, Transfer.Inner, Transfer.Inner, Transfer.Inner });
			Pending.insert(Pending.end(), Nodes[Inner].Children.begin(), Nodes[Inner].Children.end());
		}
		// 終了
		return;
	}

	/**
	 * 固定の種別契約だけを読む定義の取得関数
	 * 計算量：O(1)
	 * @param Id 読んだノード
	 * @return 固定の定義（ノードが無ければ無）
	 */
	const KindDef *SyntaxModel::GetDefOf(const NodeId Id) const {
		// 子の構成と独立した種別の定義の返戻
		return Id < Nodes.size() ? Nodes[Id].Kind.Def : nullptr;
	}

	/**
	 * 役割を持つノードの候補の取得関数
	 * 計算量：通常側は O(1)，基準側は現在のノード数 N に対し O(N)
	 * @param Wanted `BuildRoleIdx` でインデックスを構築した役割
	 * @return 現在の役割を持つノード
	 * @throws FormatLimitExceeded 役割インデックスが未構築の場合（フォーマッターの不具合），又は基準側の候補収集が作業予算を超える場合
	 * @throws std::bad_alloc 役割インデックスのメモリを確保出来ない場合
	 * @throws std::length_error インデックスの要素数上限を超える場合
	 */
	std::span<const NodeId> SyntaxModel::GetNodesWith(const Role Wanted) const {
		// 役割を持つノードの候補の取得
		#if defined(ShaveFormatRefFacts)
		const std::shared_ptr<std::vector<NodeId>> Cur = std::make_shared<std::vector<NodeId>>();
		std::vector<NodeId> Pending { Root };
		while(!Pending.empty()) {
			const NodeId Id = Pending.back();
			Pending.pop_back();
			Charge(1);
			if(Is(Id, Wanted)) Cur->push_back(Id);
			const std::span<const NodeId> Children = Nodes[Id].Children;
			Pending.insert(Pending.end(), Children.rbegin(), Children.rend());
		}
		RefFacts.push_back(Cur);
		// 現在の構造から選んだ候補の返戻
		return std::span<const NodeId>(*Cur);
		#else
		if(!HasRoleIdx) throw FormatLimitExceeded("the role index was not built before normalization", FailureReason::FormatterDefect);
		// 役割の候補の返戻
		return std::span<const NodeId>(RoleNodes[static_cast<size_t>(Wanted)]);
		#endif
	}

	/**
	 * 正規化の役割インデックスの構築関数
	 * 正規化の始めに１度だけ作り（検証の構文はインデックスを使わない為に作らない），以後は追加と組直で更新する
	 * 基準側はインデックスを持たず，照会時に現在の構造から候補を求める
	 * 計算量：ノード数 N，種別毎の選別費用 K と必要な所属する役割数の総和 S に対し O(N + K + S)
	 * @param WantedRoles 此の正規化で照会する役割
	 * @throws FormatLimitExceeded 選別又はインデックスの生成が作業予算を超える場合
	 * @throws std::bad_alloc インデックスのメモリを確保出来ない場合
	 * @throws std::length_error インデックスの上限を超える場合
	 */
	void SyntaxModel::BuildRoleIdx(const RoleSet &WantedRoles) {
		// 正規化の役割インデックスの構築
		IndexedRoles = WantedRoles;
		IndexedRoleLists.clear();
		#if !defined(ShaveFormatRefFacts)
		IndexedKinds.assign(Nodes.size(), nullptr);
		IndexedPositionRanges.assign(Nodes.size(), {});
		IndexedPositions.clear();
		PositionBuffer.clear();
		RoleNodes.assign(static_cast<size_t>(Role::Count), {});
		for(NodeId Id = 0; Id < Nodes.size(); ++Id) {
			Charge(1);
			if(IsRemoved[Id]) continue;
			const KindDef &Kind = *Nodes[Id].Kind.Def;
			IndexedKinds[Id] = &Kind;
			const std::vector<Role> &Roles = IndexedRolesOf(Kind);
			IndexedPositionRanges[Id] = { IndexedPositions.size(), Roles.size() };
			for(const Role Wanted : Roles) {
				Charge(1);
				std::vector<NodeId> &Indexed = RoleNodes[static_cast<size_t>(Wanted)];
				IndexedPositions.push_back(Indexed.size());
				Indexed.push_back(Id);
			}
		}
		HasRoleIdx = true;
		#endif
		// 終了
		return;
	}

	/**
	 * 全役割のインデックスの構築関数
	 * 計算量：ノード数 N と全所属数 S と固定の役割数 R に対し O(N + S + R)
	 * @throws FormatLimitExceeded インデックスの生成が作業予算を超える場合
	 * @throws std::bad_alloc インデックスのメモリを確保出来ない場合
	 * @throws std::length_error インデックスの上限を超える場合
	 */
	void SyntaxModel::BuildRoleIdx() {
		// 全役割のインデックスの構築
		RoleSet WantedRoles;
		WantedRoles.set();
		BuildRoleIdx(WantedRoles);
		// 終了
		return;
	}

	/**
	 * 正規化の役割インデックスの解放関数
	 * 正規化の後の工程はインデックスを使わない為，メモリを空けて以後の書換で更新しない
	 */
	void SyntaxModel::ReleaseRoleIdx() {
		// 正規化の役割インデックスの解放
		IndexedRoles.reset();
		std::unordered_map<const KindDef *, std::vector<Role>>().swap(IndexedRoleLists);
		std::vector<std::vector<NodeId>>().swap(RoleNodes);
		std::vector<size_t>().swap(IndexedPositions);
		std::vector<std::pair<size_t, size_t>>().swap(IndexedPositionRanges);
		std::vector<size_t>().swap(PositionBuffer);
		std::vector<const KindDef *>().swap(IndexedKinds);
		HasRoleIdx = false;
		// 終了
		return;
	}

	/**
	 * 予約語から組込の算術型を確認する判定関数
	 * 符号と幅の指定は其の基底型を確認し，別名を組込型の証拠にしない
	 * 計算量：基底型の子の探索費用 C と予約語の照合費用 Q に対し O(C + Q)
	 * @param Id 宣言の型のノード
	 * @return 予約語だけから算術型が確定すれば true
	 * @throws std::out_of_range 照会する子の識別子がノード表に無い場合
	 */
	bool SyntaxModel::IsBuiltinArithmeticType(const NodeId Id) const {
		// 予約語から組込の算術型を確認する判定
		const bool IsSized = Is(Id, Role::SizedTypeSpecifier);
		const NodeId Base = IsSized ? GetFieldChild(Id, ChildRole::Type) : Id;
		// 基底型を省いた符号・幅の指定，又は予約語の算術型かの返戻
		return IsSized && Base == NoNode ||
		(Is(Base, Role::PrimitiveType) || Is(Base, Role::TypeName)) && Is(Base, Role::BuiltinArithmeticType);
	}

	/**
	 * 保護領域毎の投影時の所有ノードの取得関数
	 * 領域の識別と順序は検証データと共有し，原文の位置・テキストは中核へ持ち込まない
	 * 計算量：O(1)
	 * @return 投影時に記録した領域毎の所有ノード
	 */
	const std::vector<std::vector<NodeId>> &SyntaxModel::GetProtectedErrorOwners() const {
		return ErrorZoneOwners;
	}

	/**
	 * 原文のノードを含まない生成部分木かの判定関数
	 * 計算量：O(1)
	 * @param Id 現存するノードの識別子
	 * @return 新しく補った部分木なら true（原文のコピーと移動は false）
	 */
	bool SyntaxModel::IsInsertedNode(const NodeId Id) const {
		// 原文のノードには生成の記録を作らない
		return Id < InsertedNodes.size() && InsertedNodes[Id];
	}

	/**
	 * 除いたノードかの判定関数
	 * @param Id ノード
	 * @return 書換で除いたノード（識別子を再利用しない為にノード表に残る）なら true
	 * @throws std::out_of_range 識別子が除去状態の表の範囲外の場合
	 */
	bool SyntaxModel::IsRemovedNode(const NodeId Id) const {
		// 除いたかの返戻
		return IsRemoved.at(Id);
	}

	/**
	 * 構文とは独立した作業予算の取得関数
	 * @return 正規化と中間表現の構築で共有する予算
	 */
	std::shared_ptr<const StructureWorkAccount> SyntaxModel::GetWorkAccount() const {
		// 共有する作業予算の返戻
		return Work;
	}

	/**
	 * 作業単位の上限の取得関数
	 * @return 投影側が入力の基準量から定めた上限（定めて居なければ最大値）
	 */
	size_t SyntaxModel::GetWorkLimit() const {
		// 上限の返戻
		return Work->Limit;
	}

	/**
	 * 所有領域の取得関数（整形仕様 4.4）
	 * @param Id ノード
	 * @return ノードの置く所有領域（所有領域でないノードは無）
	 */
	const OwnedRegion *SyntaxModel::GetRegionOf(const NodeId Id) const {
		// 所有領域の返戻
		return Id < Nodes.size() && Nodes[Id].Facts.RegionIdx ? Regions[Nodes[Id].Facts.RegionIdx - 1].get() : nullptr;
	}

	/**
	 * 名前のテキストの事実の取得関数
	 * 計算量：投影済の葉は期待値 O(1)，未確定の部分木はノード数 N とテキスト長 B に対し O(N + B)
	 * @param Id 名前のノード
	 * @return 現在のテキストの非 ASCII とエスケープの開始記号の有無
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 未確定のテキスト又は走査のメモリを確保出来ない場合
	 * @throws std::length_error 未確定のテキスト又は走査の要素数上限を超える場合
	 * @throws std::out_of_range ノードがノード表に無い場合
	 */
	NameSpelling SyntaxModel::GetNameSpellingOf(const NodeId Id) const {
		// 名前のテキストの事実の取得
		if(const SyntaxNode &Node = GetNode(Id); Node.Children.empty() && Node.Facts.Spelling) {
			Charge(1);
			// 投影済の葉のテキストの事実の返戻
			return *Node.Facts.Spelling;
		}
		// 現在の部分木のテキストの事実の返戻
		return ReadNameSpelling(GetText(Id));
	}

	/**
	 * 投影済の識別子の比較用の名前の取得関数
	 * 計算量：返戻する名前のバイト数 B に対し O(B)
	 * @param Id 識別子又は名前の位置に在るノード
	 * @return 投影時に確定した比較用の名前
	 * @throws FormatLimitExceeded 投影が比較用の名前を確定しなかったノードの場合（フォーマッターの不具合）又は作業予算超過の場合
	 * @throws std::bad_alloc 名前のコピー又は前提のメモリを確保出来ない場合
	 * @throws std::length_error 名前の文字数上限を超える場合
	 * @throws std::out_of_range ノードがノード表に無い場合
	 */
	std::string SyntaxModel::GetCmpName(const NodeId Id) const {
		// 投影済の識別子の比較用の名前の取得
		const std::string_view Name = GetCmpNameView(Id);
		// 名前のコピーの計上（整形仕様 9.2）
		Charge(Name.size());
		// 比較用の名前の返戻
		return std::string(Name);
	}

	/**
	 * 部分木の最初の字の取得関数
	 * 計算量：対象部分木のノード数 N に対し O(N)
	 * @param Id ノードの識別子
	 * @return 最初の字（テキストが無ければ `\0`）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	unsigned char SyntaxModel::GetFirstChar(const NodeId Id) const {
		// 最初の字の返戻
		return GetFirstEdge(Id).Char;
	}

	/**
	 * 部分木の最後の字の取得関数
	 * 計算量：対象部分木のノード数 N に対し O(N)
	 * @param Id ノードの識別子
	 * @return 最後の字（テキストが無ければ `\0`）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	unsigned char SyntaxModel::GetLastChar(const NodeId Id) const {
		// 最後の字の返戻
		return GetLastEdge(Id).Char;
	}

	/**
	 * 部分木の最後のトークンの役割の判定関数
	 * 計算量：対象部分木のノード数 N に対し O(N)
	 * @param Id 部分木のノード
	 * @param Wanted 判定する役割
	 * @return 最後のトークンが役割を持てば true
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	bool SyntaxModel::HasLastTokenRole(const NodeId Id, const Role Wanted) const {
		// 最後のトークンの役割の返戻
		return IsEdgeRole(GetLastEdge(Id), Wanted);
	}

	/**
	 * 括弧を開閉するトークンの深さの変化の取得関数
	 * @param Id トークンのノード
	 * @return 開きなら１，閉じなら負の１，其の他なら０
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 */
	int SyntaxModel::GetBracketLeadDelta(const NodeId Id) const {
		Charge(1);
		// トークンでない対象の返戻
		if(Id >= Nodes.size() || !Def || !Nodes[Id].Kind.Def || !Nodes[Id].Children.empty()) return 0;
		const KindDef *const Delimiter = Nodes[Id].Facts.DelimiterDef;
		const KindDef *const Token = Delimiter ? Delimiter : Nodes[Id].Facts.TokenDef;
		const RoleSet *const Roles = Token ? &Token->Roles : nullptr;
		// 未登録の綴りの返戻
		if(!Roles) return 0;
		// 同じ集合から開き・閉じの順で判定した深さの変化の返戻
		return Roles->test(static_cast<size_t>(Role::OpeningBracketLead)) ?
		1 :
		Roles->test(static_cast<size_t>(Role::ClosingBracketLead)) ? -1 : 0;
	}

	/**
	 * 括弧を開閉するトークンの役割の判定関数
	 * @param Id トークンのノード
	 * @param Wanted 開き又は閉じの役割
	 * @return 指定の役割を持てば true
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 */
	bool SyntaxModel::IsBracketLead(const NodeId Id, const Role Wanted) const {
		// 括弧を開閉するトークンの役割の判定
		Charge(1);
		// 括弧の役割の返戻
		return IsTokenRole(Id, Wanted);
	}

	/**
	 * 文の並びの最後の文の取得関数
	 * 計算量：末尾の注記・区切・後送本文の数 S に対し O(S + 1)
	 * @param Id 文を並べるノード
	 * @return 注記と後送本文を除く最後の名前付の子（無ければ無）
	 * @throws FormatLimitExceeded ノードの検査又は事実の収集が作業予算を超えた場合
	 */
	NodeId SyntaxModel::GetLastStmt(const NodeId Id) const {
		// 文の並びの最後の文の取得
		const std::span<const NodeId> Children = GetChildrenOf(Id);
		for(std::span<const NodeId>::reverse_iterator Child = Children.rbegin(); Child != Children.rend(); ++Child) {
			Charge(1);
			// 最後の文の返戻
			if(IsNamed(*Child) && !Is(*Child, Role::Comment) && !Is(*Child, Role::DeferredLiteralBody)) return *Child;
		}
		// 文の無い事の返戻
		return NoNode;
	}

	/**
	 * 後の名前付の兄弟の取得関数
	 * 計算量：探索する子又は兄弟の数 C に対し O(C)
	 * @param Id ノードの識別子
	 * @return 後の名前付の兄弟（無ければ無）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 */
	NodeId SyntaxModel::GetNextNamedSibling(const NodeId Id) const {
		// 後の名前付の兄弟の取得
		NodeId Next = GetNextSibling(Id);
		while(Next != NoNode) {
			Charge(1);
			if(IsNamed(Next)) break;
			Next = GetNextSibling(Next);
		}
		// 後の名前付の兄弟の返戻
		return Next;
	}

	/**
	 * 構文の順でノードの後に続く物の取得関数（整形仕様 7.6）
	 * ノードの後の兄弟（要素の区切のトークンは飛ばす）を，無ければ親の後に続く物を，正規化の後に作ったインデックスから引く
	 * @param Id ノードの識別子
	 * @return 後に続くノード（無ければ無）
	 */
	NodeId SyntaxModel::GetFollowingOf(const NodeId Id) const {
		// 後に続くノードの返戻
		return Id < Following.size() ? Following[Id] : NoNode;
	}

	/**
	 * 構文の順でノード毎の後に続く物のインデックスの構築関数（整形仕様 7.6）
	 * 正規化の後に根から１度の走査で作り，ドキュメントの同期とコメントの配置が記録毎に祖先を辿らずに引く（整形仕様 9.2）
	 * 計算量：ノード数 V に対し O(V)
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	void SyntaxModel::BuildFollowingIdx() {
		// 構文の順でノード毎の後に続く物のインデックスの構築
		Following.assign(GetNodeCount(), NoNode);
		for(std::vector<NodeId> Pending { GetRoot() }; !Pending.empty();) {
			const NodeId Parent = Pending.back();
			Pending.pop_back();
			const std::span<const NodeId> Children = GetChildrenOf(Parent);
			Charge(Children.size());
			// 後の兄弟から順に確定する後続の兄弟の登録（区切の兄弟は除外）
			NodeId Next = Following[Parent];
			for(std::span<const NodeId>::reverse_iterator Child = Children.rbegin(); Child != Children.rend(); ++Child) {
				Following[*Child] = Next;
				if(!Is(*Child, Role::SeparatorToken)) Next = *Child;
				Pending.push_back(*Child);
			}
		}
		// 終了
		return;
	}

	/**
	 * 指定のテキストの無名の子を持つかの判定関数
	 * 計算量：子の数 C と照合するテキストの総長 L に対し O(C + L)
	 * @param Id ノードの識別子
	 * @param Token テキスト
	 * @return テキストの一致する無名の子が有れば true
	 * @throws FormatLimitExceeded 変更範囲の記録又は子の走査が作業予算を超えた場合
	 */
	bool SyntaxModel::HasTokenChild(const NodeId Id, const std::string_view Token) const {
		// 指定のテキストの無名の子を持つかの判定
		for(const NodeId Child : GetChildrenOf(Id)) {
			Charge(1);
			// 一致する無名の子の返戻
			if(!IsNamed(Child) && GetLexemeOf(Child) == Token) return true;
		}
		// 一致する子の無い事の返戻
		return false;
	}

	/**
	 * 指定の役割の無名の子を持つかの判定関数
	 * 計算量：子の数 C と照合するテキストの総長 L に対し O(C + L)
	 * @param Id 親のノード
	 * @param Wanted トークンの役割
	 * @return 役割を持つ無名の子が有れば true
	 */
	bool SyntaxModel::HasTokenRoleChild(const NodeId Id, const Role Wanted) const {
		// 役割を持つ無名の子を見つけた場合の返戻
		for(const NodeId Child : GetChildrenOf(Id)) if(!IsNamed(Child) && IsTokenRole(Child, Wanted)) return true;
		// 役割を持つ子が無い事の返戻
		return false;
	}

	/**
	 * 語の意味の綴りの無名の子を持つかの判定関数
	 * 計算量：子の数 C と照合するテキストの総長 L に対し O(C + L)
	 * @param Id ノードの識別子
	 * @param Meaning 語の意味
	 * @return 文法が其の意味に定める綴りの無名の子が有れば true
	 * @throws FormatLimitExceeded 変更範囲の記録又は子の走査が作業予算を超えた場合
	 */
	bool SyntaxModel::HasWordChild(const NodeId Id, const Word Meaning) const {
		// 語の綴りの定義の照会
		Charge(1);
		// 綴りを持たない意味の語の不在の返戻
		if(Def->Words[static_cast<size_t>(Meaning)].empty()) return false;
		// 語の意味の綴りの無名の子を持つかの判定
		for(const NodeId Child : GetChildrenOf(Id)) {
			Charge(1);
			// 一致する無名の子の返戻
			if(!IsNamed(Child) && IsWord(Child, Meaning)) return true;
		}
		// 一致する子の無い事の返戻
		return false;
	}

	/**
	 * 波括弧で包んだ文のブロックかの判定関数
	 * @param Node ノード
	 * @return 文を包むブロックの種別で，`{` で始まる物なら true
	 */
	bool SyntaxModel::IsBracedBody(const NodeId Node) const {
		// ブロックの種別で波括弧を開くノードかの返戻
		return Is(Node, Role::Block) && Is(GetChild(Node, 0), Role::OpenBrace);
	}

	/**
	 * 空の本体を閉じの語と同じ行へ畳むと構文エラーに為るかの判定関数
	 * 見出が式・名前で終わる形 (`class A < B end` / `class << obj end` / `def n end` / `def m a end`) は畳めない
	 * @param Id ノード
	 * @return 閉じの前に区切が要れば true
	 * @throws std::out_of_range 照会する子の識別子がノード表に無い場合
	 */
	bool SyntaxModel::ShouldSeparateBeforeClose(const NodeId Id) const {
		// 親クラスと値で開く形は仮引数に関わらず区切を要する事の返戻
		if(GetFieldChild(Id, ChildRole::Superclass) != NoNode || GetFieldChild(Id, ChildRole::Val) != NoNode) return true;
		// メソッド以外は区切を要しない事の返戻
		if(!Is(Id, Role::EndDelimitedMethod)) return false;
		const NodeId Params = GetFieldChild(Id, ChildRole::Params);
		// 括弧で囲んだ仮引数を持つか
		const bool HasParenthesized = Params != NoNode && IsTokenRole(GetChild(Params, 0), Role::OpeningParenToken);
		// 仮引数の括弧を持たない `def` の返戻
		return !HasParenthesized;
	}

	/**
	 * １対象の枝又は文の値が結果に為るかの判定関数
	 * 計算量：祖先と其の子の総探索量 Q に対し期待値 O(Q + 1)，通常側で同じノードの判定を共有する照会は期待値 O(1)
	 * @param Node 枝を持つ式又は文の並びの文
	 * @return 値が結果として使われる位置なら `true`
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	bool SyntaxModel::IsBranchValUsed(const NodeId Node) const {
		// １対象の枝又は文の値が結果に為るかの判定
		std::vector<NodeId> Path;
		NormalizationFacts::ValFact Fact;
		NodeId Cur = Node;
		while(Cur != NoNode && Cur != GetRoot() && IsNamed(Cur)) {
			Charge(1);
			#if !defined(ShaveFormatRefFacts)
			if(
				const std::unordered_map<NodeId, NormalizationFacts::ValFact>::const_iterator Found =
				EditNormalizationFacts().ValFacts.find(Cur);
				Found != EditNormalizationFacts().ValFacts.end()
			) {
				Fact = Found->second;
				break;
			}
			Path.push_back(Cur);
			#endif
			NodeId Holder = GetParentOf(Cur);
			if(Is(Holder, Role::StmtSeq)) {
				const NodeId Parent = GetLastContentChild(Holder) == Cur ? GetParentOf(Holder) : NoNode;
				// 最後の文でない文と最上位の文の値を使わない場合の終了
				if(Parent == NoNode) break;
				Holder = Parent;
			} else if(!Is(Holder, Role::BodyWrapper)) {
				Fact.IsUsed = true;
				// 値の位置を確定した場合の中断
				break;
			}
			if(Is(Holder, Role::LastValLambda)) {
				Fact.IsUsed = true;
				// ラムダの結果を確定した場合の中断
				break;
			}
			NodeId Branch = Holder;
			if(Is(Holder, Role::BranchBody)) {
				Branch = GetParentOf(Holder);
				// 根の直下の本体の場合の中断
				if(Branch == NoNode) break;
				if(Is(Branch, Role::CaseArm)) Branch = GetParentOf(Branch);
				// 値を持たない本体の場合の中断
				if(Is(Holder, Role::BodyWrapper) && !Is(Branch, Role::ValBranching)) break;
			// 値を持たない境界の場合の中断
			} else if(!Is(Holder, Role::ValTry)) break;
			Cur = Branch;
		}
		#if !defined(ShaveFormatRefFacts)
		for(const NodeId Part : Path) EditNormalizationFacts().ValFacts.insert_or_assign(Part, Fact);
		#endif
		// 現在の規則の評価の値位置の判定の返戻
		return Fact.IsUsed;
	}

	/**
	 * 値が使われない文の位置かの判定関数
	 * 増減の前置化と其の検査が同じ位置を見る為，判定を此処だけで定める
	 * 計算量：親の子数 C と値位置の祖先の照会費用 Q に対し O(C + Q)
	 * @param Node 判定する式（要素１つの括弧は外側）
	 * @param Parent 式の親
	 * @return 文の位置か `for` の更新の位置なら `true`
	 * @throws FormatLimitExceeded 値位置の探索が作業予算を超える場合
	 * @throws std::bad_alloc 値位置の要約又は作業列を確保出来ない場合
	 * @throws std::length_error 要約又は作業列の上限を超える場合
	 * @throws std::out_of_range 照会する子の識別子がノード表に無い場合
	 */
	bool SyntaxModel::IsValUnusedCtx(const NodeId Node, const NodeId Parent) const {
		// 値を返す切替の規則では，切替全体が文の位置に在る場合だけ値を捨てる
		if(
			Is(GetParentOf(Parent), Role::ValSwitchRule) &&
			!Is(GetParentOf(GetParentOf(GetParentOf(GetParentOf(Parent)))), Role::DiscardedBranchValHost)
		) return false;
		// 文列の末尾と単一文の本体も，同じ値位置の事実で照合する
		if(Is(Parent, Role::UnusedValHost)) return !Is(Parent, Role::ValStmtSeq) || !IsBranchValUsed(Node);
		if(Is(Parent, Role::BodyWrapper) && GetContentChildCount(Parent) == 1 && GetContentChild(Parent, 0) == Node) {
			// 単一文の本体の値を使わない事の返戻
			return !IsBranchValUsed(Node);
		}
		// `for` の更新フィールド（JavaScript・TypeScript では `increment`）かの返戻
		return Is(Parent, Role::CountedLoop) &&
		(GetFieldChild(Parent, ChildRole::Update) == Node || GetFieldChild(Parent, ChildRole::Increment) == Node);
	}

	/**
	 * カンマで並べる宣言の部品の取得関数（整形仕様 5.6）
	 * Go の短い変数宣言と，単独の `var`・`const` の宣言仕様では，名前と初期化子の並びを読む
	 * 其の他は宣言子の開始からカンマで区切った宣言子の並びを読み，並びの外の子を共有する
	 * 名前と初期化子の数が異なる多値初期化と，初期化子も型も無い宣言仕様は，統合・分割しない
	 * 宣言子毎に型が決まる型推論の `var` も，統合・分割しない
	 * Swift の計算型・監視付の宣言等，本体を持つ宣言子も，統合・分割しない
	 * 共有指定にラムダ又は名義型の定義が在る宣言は，分離・統合で型や束縛が変わる為，対象にしない
	 * 計算量：宣言のノード数 N と共有指定のテキスト総長 L に対し O(N + L)
	 * @param Node 宣言
	 * @param NeedsKey 宣言同士の比較キーを作るか（型・束縛の検査と部品の結果は変えない）
	 * @return 宣言の部品（カンマで並べない宣言・エラーを含む宣言は無）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノードの識別子がノード表に無い場合
	 */
	std::optional<CommaDecl> SyntaxModel::GetCommaDeclOf(const NodeId Node, const bool NeedsKey) const {
		if(Validation) {
			const ValidationStorage::Declaration &Entry = Validation->Declarations.at(Node);
			if(Entry.Error) std::rethrow_exception(Entry.Error);
			std::optional<CommaDecl> Parts = Entry.Parts;
			if(Parts && !NeedsKey) Parts->Key.clear();
			// 元の判断条件で確定した部品の返戻
			return Parts;
		}
		// カンマで並べる宣言の部品の取得
		const SyntaxModel &Syntax = *this;
		// カンマで併記出来ない宣言又はエラーを含む宣言の返戻
		if(
			!Syntax.Is(Node, Role::CommaDecl) || Syntax.GetNode(Node).Facts.Has(ConstraintKind::ContainsError) ||
			Syntax.GetNode(Node).Facts.Has(ConstraintKind::AmbiguousDecl)
		) return std::nullopt;
		CommaDecl Found { Node };
		// 並びのノードの最初の要素の子から `,` で区切った要素（要素に含める子が続く限り）の追加
		const auto AddList =
		[&Syntax, &Found]<class Function>(const NodeId Host, const NodeId First, const Function &IsItemPiece) -> void {
			DeclList List { Host, { {} } };
			if(const CanonicalForm *const Form = Syntax.GetFormOf(Host, FormOperation::GeneratedLexemes)) {
				List.SeparatorField = Form->GetPartOf(FormPartRole::Separator)->Field;
			}
			const std::span<const NodeId> Children = Syntax.GetChildrenOf(Host);
			const uint32_t Count = static_cast<uint32_t>(Children.size());
			for(uint32_t Idx = Syntax.GetNode(First).Idx; Idx < Count; ++Idx) {
				if(const NodeId Child = Children[Idx]; Syntax.IsTokenRole(Child, Role::CommaToken)) List.Items.emplace_back();
				else if(IsItemPiece(Child)) List.Items.back().push_back(Child);
				else break;
			}
			Found.Lists.push_back(std::move(List));
		};
		// 宣言の名前付の部品かの判定
		const auto IsNamedPiece = [&Syntax](const NodeId Child) -> bool {
			// 名前付の子かの返戻
			return Syntax.IsNamed(Child);
		};
		const NodeId Left = Syntax.GetFieldChild(Node, ChildRole::Left);
		NodeId Spec = NoNode;
		// 左辺を持たない宣言だけ，唯一の名前付の子を宣言仕様として照会する
		if(Left == NoNode) {
			for(const NodeId Child : Syntax.GetChildrenOf(Node)) {
				Syntax.Charge(1);
				if(!Syntax.IsNamed(Child)) continue;
				if(Spec != NoNode) {
					Spec = NoNode;
					break;
				}
				Spec = Child;
			}
			if(Syntax.Is(Spec, Role::DeclaratorHead) || Syntax.Is(Spec, Role::DeclaratorItem)) Spec = NoNode;
		}
		if(Left != NoNode) {
			AddList(Left, Syntax.GetNamedChild(Left, 0), IsNamedPiece);
			const NodeId Right = Syntax.GetFieldChild(Node, ChildRole::Right);
			AddList(Right, Syntax.GetNamedChild(Right, 0), IsNamedPiece);
		} else if(const NodeId Name = Syntax.GetFieldChild(Spec, ChildRole::Name); Name != NoNode) {
			AddList(
				Spec,
				Name,
				[&Syntax](const NodeId Child) -> bool {
					// 名前のフィールドの子かの返戻
					return Syntax.HasField(Child, ChildRole::Name);
				}
			);
			const NodeId Vals = Syntax.GetFieldChild(Spec, ChildRole::Val);
			// 初期化子も型も無い仕様の返戻
			if(Vals == NoNode && Syntax.GetFieldChild(Spec, ChildRole::Type) == NoNode) return std::nullopt;
			if(Vals != NoNode) AddList(Vals, Syntax.GetNamedChild(Vals, 0), IsNamedPiece);
		} else {
			// 宣言の中の変数の宣言 (C#) か宣言自身
			NodeId Host = Node;
			for(const NodeId Child : Syntax.GetChildrenOf(Node)) if(Host == Node && Syntax.Is(Child, Role::CommaDecl)) Host = Child;
			const std::span<const NodeId> Children = Syntax.GetChildrenOf(Host);
			const std::span<const NodeId>::iterator Head = std::ranges::find_if(
				Children,
				[&Syntax](const NodeId Child) -> bool {
					// 宣言子のフィールドの子か宣言子を始めるノードかの返戻
					return Syntax.HasField(Child, ChildRole::Declarator) || Syntax.Is(Child, Role::DeclaratorItem) ||
					Syntax.Is(Child, Role::DeclaratorHead);
				}
			);
			// 宣言子の無い宣言と型推論の `var` の返戻
			if(Head == Children.end() || Syntax.IsWord(Syntax.GetFieldChild(Host, ChildRole::Type), Word::InferredType)) {
				// 条件に該当する値の返戻
				return std::nullopt;
			}
			AddList(
				Host,
				*Head,
				[&Syntax](const NodeId Child) -> bool {
					// 終端の前の子かの返戻
					return !Syntax.IsTokenRole(Child, Role::StmtTerminatorToken);
				}
			);
		}
		// 本体のブロックかの判定
		const auto IsBody = [&Syntax](const NodeId Piece) -> bool {
			// 本体のブロックかの返戻
			return Syntax.Is(Piece, Role::IndentContainer);
		};
		for(const DeclList &List : Found.Lists) {
			// 空の要素（末尾のカンマ）と本体を持つ宣言子の返戻
			for(const std::vector<NodeId> &Item : List.Items) if(Item.empty() || std::ranges::any_of(Item, IsBody)) return std::nullopt;
		}
		// 名前と初期化子の数の異なる宣言の返戻
		if(Found.Lists.size() > 1 && Found.Lists[0].Items.size() != Found.Lists[1].Items.size()) return std::nullopt;
		// 並びの所有者の照会（宣言子だけ，又は名前と初期化子の２つの並び）
		// 計算量：宣言が持つ並びは２つ以下の為 O(1)
		// @param Host 並びを所有するノード
		// @return 並びの位置（共有部なら負）
		// @throws FormatLimitExceeded 並びの照合が作業予算を超える場合
		const auto GetListIdx = [&Syntax, &Found](const NodeId Host) -> int32_t {
			for(size_t Idx = 0; Idx < Found.Lists.size(); ++Idx) {
				Syntax.Charge(1);
				// 同じ所有者の並びの返戻
				if(Found.Lists[Idx].Host == Host) return static_cast<int32_t>(Idx);
			}
			// 共有指定の所有者の返戻
			return -1;
		};
		// 宣言種別と共有指定の構造の比較値
		if(NeedsKey) Found.Key.push_back({ Syntax.GetNode(Node).Kind.Def });
		// 宣言の共有指定の構造の追加
		const auto CanAppendShape = [&Syntax, &Found, NeedsKey](const NodeId Part) -> bool {
			std::vector<NodeId> Pending { Part };
			while(!Pending.empty()) {
				const NodeId CurIdentifier = Pending.back();
				Pending.pop_back();
				Syntax.Charge(1);
				const SyntaxNode &Cur = Syntax.GetNode(CurIdentifier);
				// 共有指定のコピー又は統合で型や束縛が変わる定義の拒否（Go の構造型の本体は含めない）
				if(
					Syntax.Is(CurIdentifier, Role::LambdaExpr) || Syntax.HasField(CurIdentifier, ChildRole::Body) &&
					(Syntax.Is(Cur.Parent, Role::ClassLike) || Syntax.Is(Cur.Parent, Role::EnumSpecifier))
				) return false;
				if(NeedsKey) {
					Found.Key.push_back({ Cur.Kind.Def, Cur.Fields, Cur.Lexeme, static_cast<uint32_t>(Cur.Children.size()), Cur.Facts.Meanings });
				}
				Pending.insert(Pending.end(), Cur.Children.rbegin(), Cur.Children.rend());
			}
			// 型と束縛を保つ共有指定の返戻
			return true;
		};
		// 並びのノードと共有部の原文順の収集
		struct DeclFrame {
			NodeId Node; // 走査する所有者
			size_t NextChild; // 次の子の位置
			int32_t List; // 所有する並び（共有部なら負）
			uint32_t Item = 0; // 次の宣言子又は初期化子の番号
			size_t Piece = 0; // 要素内の次の部品の番号
			NodeId PreviousPiece = NoNode; // 直前に消費した部品
		};
		std::vector<DeclFrame> Pending { { Node, 0, GetListIdx(Node) } };
		while(!Pending.empty()) {
			DeclFrame &Frame = Pending.back();
			const std::span<const NodeId> Children = Syntax.GetChildrenOf(Frame.Node);
			if(Frame.NextChild == Children.size()) {
				if(Frame.List > -1 && !Found.Pieces.empty()) {
					Syntax.Charge(1);
					Found.Lists[static_cast<size_t>(Frame.List)].LastPiece = Found.Pieces.size() - 1;
				}
				Pending.pop_back();
				continue;
			}
			const NodeId Child = Children[Frame.NextChild++];
			Syntax.Charge(1);
			if(const int32_t List = GetListIdx(Child); Child != Frame.Node && List > -1) {
				Pending.push_back({ Child, 0, List });
				continue;
			}
			// 各並びの既知の子順を消費し，部品の再識別用の表を作らない
			if(Frame.List > -1) {
				const DeclList &List = Found.Lists[static_cast<size_t>(Frame.List)];
				const NodeId NextPiece = Frame.Item < List.Items.size() ? List.Items[Frame.Item][Frame.Piece] : NoNode;
				if(Child == NextPiece) {
					Found.Pieces.push_back({ Child, Frame.List, Frame.Item });
					Frame.PreviousPiece = Child;
					if(++Frame.Piece == List.Items[Frame.Item].size()) {
						Frame.Piece = 0;
						++Frame.Item;
					}
					continue;
				}
				// 前後が要素の部品であるカンマだけを共有指定から除く
				if(
					Syntax.IsTokenRole(Child, Role::CommaToken) && Frame.PreviousPiece != NoNode &&
					Frame.PreviousPiece == Syntax.GetPrevSibling(Child) && NextPiece != NoNode && NextPiece == Syntax.GetNextSibling(Child)
				) continue;
			}
			Found.Pieces.push_back({ Child });
			// 文末の明示的な区切は，共有する宣言の構文を変えない為，比較キーに含めない
			if(Syntax.IsTokenRole(Child, Role::StmtTerminatorToken) && Syntax.GetNextSibling(Child) == NoNode) continue;
			// 型と束縛を保てない共有指定の返戻
			if(!CanAppendShape(Child)) return std::nullopt;
		}
		for(const DeclPiece &Piece : Found.Pieces) {
			// 宣言子の記号を含むマクロが在る宣言の非併記の返戻
			if(Piece.List < 0 && Syntax.GetNode(Piece.Node).Facts.Has(ConstraintKind::DeclaratorMacro)) return std::nullopt;
		}
		// 宣言の部品の返戻
		return Found;
	}

	/**
	 * 現在の親子構造の根の共有取得関数
	 * 計算量：未取得の祖先数 H と表の追加数 N に対し O(H + N)，再照会は O(1)
	 * @param Node 所属を調べるノード
	 * @return 未接続の部分木も区別した根
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc 所属又は作業列のメモリを確保出来ない場合
	 * @throws std::length_error 所属又は作業列の上限を超える場合
	 */
	NodeId SyntaxModel::GetTreeRoot(const NodeId Node) const {
		if(TreeRoots.size() < GetNodeCount()) {
			Charge(GetNodeCount() - TreeRoots.size());
			TreeRoots.resize(GetNodeCount(), NoNode);
		}
		std::vector<NodeId> Path;
		NodeId Tree = NoNode;
		for(NodeId Up = Node; Up != NoNode; Up = GetParentOf(Up)) {
			Charge(1);
			if(TreeRoots[Up] != NoNode) {
				Tree = TreeRoots[Up];
				break;
			}
			Path.push_back(Up);
			Tree = Up;
		}
		for(const NodeId Up : Path) {
			Charge(1);
			TreeRoots[Up] = Tree;
		}
		// 現在の親子構造の根の返戻
		return Tree;
	}

	/**
	 * 現在の構文順の共有区間の取得関数
	 * 計算量：採番又は修復の費用 Q に対し O(Q + 1)，更新の無い再照会は O(1)
	 * @param Node 接続済のノード
	 * @return 前順と後順の番号（子孫の前順は其の間に在る）
	 * @throws FormatLimitExceeded 作業予算又は採番の容量を超える場合
	 * @throws std::bad_alloc 採番の作業列を確保出来ない場合
	 * @throws std::length_error 採番の作業列の上限を超える場合
	 * @throws std::out_of_range ノードが表に無い場合
	 */
	std::pair<uint64_t, uint64_t> SyntaxModel::GetOrderRange(const NodeId Node) const {
		// 編集用の表を解放した後は，確定時に所有表へ移した区間を読む
		if(Validation) {
			Charge(1);
			const ValidationRecord &Record = Validation->Records.at(Node);
			// 所有した区間の返戻
			return { Record.OrderFirst, Record.OrderLast };
		}
		EnsureOrder();
		Charge(1);
		const SyntaxNode &Record = GetNode(Node);
		// 同じモデルで共有する現在の区間の返戻
		return { Record.OrderFirst, Record.OrderLast };
	}

	/**
	 * ノードが部分木に含まれるかの判定関数
	 * 計算量：所属の初回収集量 H と採番・修復費用 Q に対し O(H + Q + 1)，接続済の再照会は O(1)
	 * @param Ancestor 部分木の根
	 * @param Node ノード
	 * @return ノードが根自身か其の子孫なら true
	 * @throws FormatLimitExceeded 構文の探索・採番が作業予算を超える場合
	 * @throws std::bad_alloc 所属又は採番の作業列を確保出来ない場合
	 * @throws std::length_error 所属又は採番の作業列の上限を超える場合
	 */
	bool SyntaxModel::HasNodeWithin(const NodeId Ancestor, const NodeId Node) const {
		// 空の部分木又は名前は何も含まない事の返戻
		if(Ancestor == NoNode || Node == NoNode) return false;
		// 根自身を含む事の返戻
		if(Ancestor == Node) return true;
		#if !defined(ShaveFormatRefFacts)
		const NodeId Tree = GetTreeRoot(Node);
		// 異なる部分木へ属するノードの返戻
		if(Tree != GetTreeRoot(Ancestor)) return false;
		if(Tree == Root) {
			const std::pair<uint64_t, uint64_t> Range = GetOrderRange(Ancestor);
			Charge(1);
			// 共有する構文順の位置の取得
			const uint64_t First = Validation ? Validation->Records.at(Node).OrderFirst : Nodes[Node].OrderFirst;
			// 区間に含まれるかの返戻
			return Range.first < First && First < Range.second;
		}
		#endif
		// 構築中の未接続の部分木と，独立した基準側は親子関係を直接読む
		for(NodeId Up = GetParentOf(Node); Up != NoNode; Up = GetParentOf(Up)) {
			Charge(1);
			// 構築中の部分木の祖先に届いた事の返戻
			if(Up == Ancestor) return true;
		}
		// 部分木の外の返戻
		return false;
	}

	/**
	 * 部分木のコピーの生成ノードの取得関数
	 * 宣言の分割で共有する型・修飾を各宣言へコピーする（コピーはアンカーを持たない）
	 * 計算量：部分木のノード数 N と，コピーするトークン・境界の総バイト数 B に対し O(N + B)
	 * @param Target コピーする部分木の根
	 * @return 種別・フィールド・テキスト・構文事実・子をコピーした生成ノード
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノードの識別子がノード表に無い場合
	 */
	GeneratedNode SyntaxModel::CopyOf(const NodeId Target) const {
		// ノード単位のコピーの作成
		const auto MakeCopy = [this](const NodeId CurIdentifier) -> GeneratedNode {
			const SyntaxNode &Node = GetNode(CurIdentifier);
			Charge(Node.Lexeme.size() + 1);
			GeneratedNode Copy { {}, Node.Kind.IsNamed(), {}, Node.Fields, NoNode, Node.Lexeme, GetGenerationPartOf(CurIdentifier) };
			for(const SemanticBoundary &Boundary : Node.Facts.Boundaries.GetView()) Charge(Boundary.Gap.size() + 1);
			Copy.Facts = Node.Facts;
			Copy.Src = CurIdentifier;
			// ノードのコピーの返戻
			return Copy;
		};
		GeneratedNode Copy = MakeCopy(Target);
		// 既存部分木のコピーの状態
		struct Frame {
			NodeId Id;
			GeneratedNode *Out; // 生成したノードの格納先
			size_t NextChild = 0;
		};
		// 子孫のコピーの展開
		std::vector<Frame> Pending { { Target, &Copy } };
		while(!Pending.empty()) {
			Frame &Cur = Pending.back();
			const SyntaxNode &Node = GetNode(Cur.Id);
			if(Cur.NextChild == Node.Children.size()) {
				Pending.pop_back();
				continue;
			}
			const NodeId Child = Node.Children[Cur.NextChild++];
			Cur.Out->Children.push_back(MakeCopy(Child));
			Pending.push_back({ Child, &Cur.Out->Children.back() });
		}
		// コピーの返戻
		return Copy;
	}

	/**
	 * 元のノードから生成するノードの子の境界の組直関数
	 * 元と同じ子が隣接する境界と，同じ先頭・末尾の子の外側の境界をコピーする
	 * 除いた宣言子や区切を越える境界は引き継がない
	 * 計算量：生成部品数 G，親毎の境界数の最大値 K，コピーする間隙の総長 B に対し O(G log(K + 1) + B)
	 * @param Generated 子を組み立てた生成ノード
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノードの識別子がノード表に無い場合
	 */
	void SyntaxModel::RemapGeneratedBoundaries(GeneratedNode &Generated) const {
		// 生成部分木の子の境界の組直
		std::vector<GeneratedNode *> Pending { &Generated };
		while(!Pending.empty()) {
			GeneratedNode &Cur = *Pending.back();
			Pending.pop_back();
			Charge(1);
			if(Cur.Src != NoNode) {
				const SyntaxNode &Original = GetNode(Cur.Src);
				std::vector<SemanticBoundary> Boundaries;
				// 生成した子が参照する元のノードの取得
				const auto SrcOf = [](const GeneratedNode &Child) -> NodeId {
					// 移すノード又はコピーするノードの識別子の返戻
					return Child.Existing != NoNode ? Child.Existing : Child.Src;
				};
				// 元の隣接を保つ境界のコピー
				const auto CopyBoundary = [this, &Original, &Boundaries](const uint32_t Before, const uint32_t After) -> void {
					if(const SemanticBoundary *const Found = Original.Facts.GetBoundaryBefore(Before)) {
						Charge(Found->Gap.size() + 1);
						SemanticBoundary Copy = *Found;
						Copy.Before = After;
						Boundaries.push_back(std::move(Copy));
					}
				};
				if(!Original.Children.empty() && !Cur.Children.empty()) {
					if(SrcOf(Cur.Children.front()) == Original.Children.front()) CopyBoundary(0, 0);
					for(uint32_t Idx = 1; Idx < Cur.Children.size(); ++Idx) {
						Charge(1);
						const NodeId Prev = SrcOf(Cur.Children[Idx - 1]), Next = SrcOf(Cur.Children[Idx]);
						if(Prev == NoNode || Next == NoNode) continue;
						if(
							const SyntaxNode &Left = GetNode(Prev), &Right = GetNode(Next);
							Left.Parent == Cur.Src && Right.Parent == Cur.Src && Left.Idx + 1 == Right.Idx
						) CopyBoundary(Right.Idx, Idx);
					}
					if(SrcOf(Cur.Children.back()) == Original.Children.back()) {
						CopyBoundary(static_cast<uint32_t>(Original.Children.size()), static_cast<uint32_t>(Cur.Children.size()));
					}
				}
				Cur.Facts.Boundaries = SemanticBoundaryList(std::move(Boundaries));
			}
			for(std::vector<GeneratedNode>::reverse_iterator Child = Cur.Children.rbegin(); Child != Cur.Children.rend(); ++Child) {
				Pending.push_back(&*Child);
			}
		}
		// 終了
		return;
	}

	/**
	 * 構文の順で前に在るかの判定関数
	 * 共通の祖先の下の子の位置で比べ，祖先は子孫より前とする
	 * 計算量：更新済なら O(1)，未反映の変更は影響部分木だけを更新する
	 * @param Lhs 左のノード
	 * @param Rhs 右のノード
	 * @return `Lhs` が `Rhs` より前に始まれば `true`
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	bool SyntaxModel::IsBefore(const NodeId Lhs, const NodeId Rhs) const {
		// 同じノードと空のノードは前に無い事の返戻
		if(Lhs == Rhs || Lhs == NoNode || Rhs == NoNode) return false;
		#if defined(ShaveFormatRefFacts)
		// 根からノード迄の経路
		const auto PathOf = [this](NodeId Node) -> std::vector<NodeId> {
			std::vector<NodeId> Path;
			for(; Node != NoNode; Node = GetParentOf(Node)) {
				Charge(1);
				Path.push_back(Node);
			}
			std::reverse(Path.begin(), Path.end());
			// 経路の返戻
			return Path;
		};
		const std::vector<NodeId> Left = PathOf(Lhs), Right = PathOf(Rhs);
		// 共通の祖先と分岐位置の比較
		size_t Depth = 0;
		while(Depth < Left.size() && Depth < Right.size()) {
			Charge(2);
			if(Left[Depth] != Right[Depth]) break;
			++Depth;
		}
		// 一方が他方の祖先なら祖先が前である事の返戻
		if(Depth == Left.size() || Depth == Right.size()) return Depth == Left.size();
		// 分かれた子の位置の比較の返戻
		return Nodes[Left[Depth]].Idx < Nodes[Right[Depth]].Idx;
		#else
		EnsureOrder();
		Charge(2);
		// 前順の番号の比較の返戻
		return Nodes[Lhs].OrderFirst < Nodes[Rhs].OrderFirst;
		#endif
	}

	/**
	 * 分岐の真枝の取得関数
	 * @param Id 分岐のノード
	 * @return `consequence` の子（無ければ `body` の子）
	 * @throws std::out_of_range 照会する子の識別子がノード表に無い場合
	 */
	NodeId SyntaxModel::GetIfConsequence(const NodeId Id) const {
		// 分岐の真枝の取得
		const NodeId Consequence = GetFieldChild(Id, ChildRole::Consequence);
		// 真枝の返戻
		return Consequence == NoNode ? GetFieldChild(Id, ChildRole::Body) : Consequence;
	}

	/**
	 * 投影を終えた構文の基準量からの作業単位の上限の確定関数（整形仕様 9.2）
	 * 基準量 `W = max(1, N + B)` はノード数 N とトークンの葉・逐語内容のバイト数 B の和で，配置の空白とコメントを含まない
	 * 計算量：ノード数 V と保持境界の総バイト数 G に対し O(V + G)
	 */
	void SyntaxModel::FixWorkLimit() {
		#if defined(ShaveFormatRefFacts)
		// 基準側は再利用しない実作業を全て計上し，製品の予算に依る照合の欠落を避ける
		Work->Limit = std::numeric_limits<size_t>::max();
		#else
		// 投影を終えた構文の基準量からの作業単位の上限の確定
		size_t Bytes = 0;
		for(const SyntaxNode &Node : Nodes) {
			Bytes += Node.Lexeme.size();
			for(const SemanticBoundary &Boundary : Node.Facts.Boundaries.GetView()) {
				if(Boundary.Kind != BoundaryKind::Preserved || Boundary.Src == PreservedSrc::RenderedLineEdges) continue;
				// 値・トークン列・エラー領域の内部の逐語内容は，其の中の空白も含む
				if(
					Boundary.Src == PreservedSrc::LiteralContent || Boundary.Src == PreservedSrc::MacroTokens ||
					Boundary.Src == PreservedSrc::ErrorContent
				) Bytes += Boundary.Gap.size();
				// エラー領域の外縁と隠れたトークンの間隙では，トークンだけを数え配置の空白を含めない
				else for(const unsigned char Char : Boundary.Gap) if(!std::isspace(Char)) ++Bytes;
			}
		}
		// 小さな入力でも足りる固定費と入力に比例する費用の大きい方（投影の作業は此の予算へ数えない）
		Work->Base = std::max<size_t>(1, Nodes.size() + Bytes);
		Work->Limit = std::max(WorkBudget, Work->Base << WorkPerBaseShift);
		#endif
		Work->Used = 0;
		// 終了
		return;
	}

	/**
	 * 移譲の連鎖の合成関数（整形仕様 7.2）
	 * 適用順の逆から，アンカーの置方と文中かの組毎に，後続の移譲で確定した行先を読む
	 * 同じ元を再び移す場合も，其の移譲より後の対応を読んでから６通りの対応を一括で置換する
	 * 計算量：移譲の数 T に対し期待値 O(T)
	 * @param Syntax 作業予算の計上先
	 * @throws FormatLimitExceeded アンカーの合成が作業予算を超える場合
	 * @throws std::bad_alloc アンカーのメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	void RewriteTrace::Compose(const SyntaxModel &Syntax) {
		// 移譲の連鎖の合成
		Moves.clear();
		// 置方から１回の移譲の行先を選ぶ
		const auto TargetOf = [](const AnchorTransfer &Transfer, const AnchorSide Side) -> const AnchorTarget & {
			switch(Side) {
			case AnchorSide::Before:
				// 先行の行先の返戻
				return Transfer.Leading;
			case AnchorSide::Inside:
				// 内部の行先の返戻
				return Transfer.Inner;
			default:
				// 末尾の行先の返戻
				return Transfer.Trailing;
			}
		};
		for(
			std::vector<AnchorTransfer>::const_reverse_iterator Transfer = Transfers.rbegin();
			Transfer != Transfers.rend();
			++Transfer
		) {
			Syntax.Charge(1);
			std::array<AnchorMove, 6> Arrivals;
			size_t Idx = 0;
			for(const AnchorSide Side : { AnchorSide::Before, AnchorSide::After, AnchorSide::Inside }) {
				for(const bool IsInline : { false, true }) {
					Syntax.Charge(1);
					AnchorMove Move { Transfer->From, Side };
					const AnchorTarget &Target = TargetOf(*Transfer, Side);
					Move.Node = Target.Node;
					if(Target.Node != NoNode) {
						if(Target.IsCaseBody) {
							Move.Side = AnchorSide::After;
							Move.Reset = TokenReset::None;
							Move.IsRemovedCaseBody = true;
						} else if(Target.IsEmptyBody) {
							Move.Side = IsInline ? AnchorSide::Inside : AnchorSide::Before;
							if(IsInline) Move.Reset = TokenReset::AllInsideAnchor;
							else Move.IsEmptyBody = true;
						} else switch(Side) {
						case AnchorSide::Before:
						case AnchorSide::After:
							Move.Side = Target.IsAfter ? AnchorSide::After : AnchorSide::Before;
							break;
						case AnchorSide::Inside:
							break;
						}
						Syntax.Charge(1);
						if(
							const std::unordered_map<uint64_t, AnchorMove>::const_iterator Following =
							Moves.find(GetMoveKey(Move.Node, Move.Side, IsInline));
							Following != Moves.end()
						) {
							const AnchorMove &Final = Following->second;
							Move.Node = Final.Node;
							Move.Side = Final.Side;
							Move.IsEmptyBody |= Final.IsEmptyBody;
							Move.IsRemovedCaseBody |= Final.IsRemovedCaseBody;
							if(Final.Reset != TokenReset::Preserve) Move.Reset = Final.Reset;
						}
					}
					Arrivals[Idx++] = Move;
				}
			}
			Idx = 0;
			for(const AnchorSide Side : { AnchorSide::Before, AnchorSide::After, AnchorSide::Inside }) {
				for(const bool IsInline : { false, true }) {
					Syntax.Charge(1);
					Moves.insert_or_assign(GetMoveKey(Transfer->From, Side, IsInline), Arrivals[Idx++]);
				}
			}
		}
		std::vector<AnchorTransfer>().swap(Transfers);
		// 終了
		return;
	}

	/**
	 * ネストの段数の検査関数（整形仕様 9.2，深さを走査の呼出でなく構造の段数で測る）
	 * 計算量：ノード数 V に対し O(V)
	 * @throws FormatLimitExceeded 根からの段数が上限を超える場合（規定したリソース上限の見送）
	 */
	void SyntaxModel::CheckNesting() const {
		// 親と兄弟の位置を共有し，深い本体でも呼出スタックを使わない
		NodeId Current = Root;
		uint32_t Depth = 1;
		while(1) {
			if(!Nodes[Current].Children.empty()) {
				Current = Nodes[Current].Children.front();
				++Depth;
				// 深さが増えた時だけ，上限を超える段数を見送る
				if(Depth > MaxNestingDepth) throw FormatLimitExceeded("nesting exceeds formatting limit");
				continue;
			}
			while(Current != Root) {
				const SyntaxNode &Node = Nodes[Current];
				const SyntaxNode &Parent = Nodes[Node.Parent];
				if(Node.Idx + 1 < Parent.Children.size()) break;
				Current = Node.Parent;
				--Depth;
			}
			if(Current == Root) break;
			Current = Nodes[Nodes[Current].Parent].Children[Nodes[Current].Idx + 1];
		}
		// 終了
		return;
	}

	/**
	 * 書換の計画の適用関数
	 * 操作を順に適用し，除いたノードのアンカーの行先と変更したノードを記録する（再解析しない，整形仕様 3.4，5.1）
	 * 計算量：失効・書換・役割更新・アンカー移譲の総量 Q に対し O(Q)
	 * @param Plan 書換の計画
	 * @return アンカーの移譲の記録
	 * @throws std::out_of_range 計画がノード表に無いノードを指す場合
	 * @throws FormatLimitExceeded 作業予算を超えるか生成形式が不足した場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	RewriteTrace SyntaxModel::Apply(const RewritePlan &Plan) {
		// 書換の計画の適用
		if(!SourceReaders.empty()) RewriteSourceOwners.emplace();
		try {
			InvalidateFacts(Plan);
			Charge(LastTouched.size());
			LastTouched.clear();
			RewriteTrace Trace;
			Charge(Plan.Steps.size());
			bool HasChildAssembly = false;
			for(const RewriteStep &Step : Plan.Steps) HasChildAssembly |= Step.Kind == RewriteKind::RebuildChildren;
			if(HasChildAssembly) {
				std::vector<NodeId> &Reused = RewriteReused;
				Reused.clear();
				std::vector<const GeneratedNode *> &Pending = RewritePending;
				Pending.clear();
				for(const RewriteStep &Step : Plan.Steps) {
					if(Step.Kind != RewriteKind::Insert && Step.Kind != RewriteKind::Reshape && Step.Kind != RewriteKind::RebuildChildren) continue;
					Pending.push_back(&Step.Inserted);
					while(!Pending.empty()) {
						const GeneratedNode &Node = *Pending.back();
						Pending.pop_back();
						Charge(1);
						if(Node.Existing != NoNode) Reused.push_back(Node.Existing);
						else for(const GeneratedNode &Child : Node.Children) Pending.push_back(&Child);
					}
				}
				DetachBatch(Reused);
				Reused.clear();
			}
			std::vector<AnchorTransfer> &RemovedRoots = RewriteRemovedRoots;
			RemovedRoots.clear();
			for(const RewriteStep &Step : Plan.Steps) ApplyStep(Step, HasChildAssembly, Trace, RemovedRoots);
			for(const AnchorTransfer &Transfer : RemovedRoots) TransferInnerAnchors(Transfer, Trace);
			RemovedRoots.clear();
			if(HasRoleIdx) for(const NodeId Id : LastTouched) IdxRoles(Id);
			RewriteSourceOwners.reset();
			// アンカーの移譲の記録の返戻
			return Trace;
		} catch(...) {
			RewriteSourceOwners.reset();
			throw;
		}
	}

	/**
	 * 役割を持つ最初の名前付の子の取得関数
	 * 計算量：探索する子又は兄弟の数 C に対し O(C)
	 * @param Id 親のノード
	 * @param Desired 求める役割
	 * @return 一致する子（無ければ無）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 */
	NodeId SyntaxModel::GetFirstNamedChildWithRole(const NodeId Id, const Role Desired) const {
		// 指定の役割を持つ子の探索
		for(const NodeId Child : GetChildrenOf(Id)) {
			Charge(1);
			// 指定の役割を持つ最初の子の返戻
			if(IsNamed(Child) && Is(Child, Desired)) return Child;
		}
		// 一致する子の無い事の返戻
		return NoNode;
	}

	/**
	 * 正規化工程だけの共有事実の更新口
	 * @return 現在の構造に対応する可変の事実
	 */
	NormalizationFacts &SyntaxModel::EditNormalizationFacts() const {
		// 所有する事実の返戻
		return Normalization;
	}

	/**
	 * 構文を所有する工程が作業予算を閉じる操作
	 */
	void SyntaxModel::CloseWork() const {
		Work->IsClosed = true;
		// 採番は毎回書込先を集め直す為，残る変更の有無に依らず作業列だけを解放する
		std::vector<uint64_t *>().swap(OrderEvents);
		// 終了
		return;
	}

	/**
	 * 正規化後の共有事実の解放関数
	 * 計算量：保持する事実数 F に対し O(F)
	 */
	void SyntaxModel::ReleaseFacts() {
		// 正規化後の共有事実の解放
		Normalization = {};
		std::deque<std::optional<CalleeSummary>>().swap(Callees);
		LiveCallees = 0;
		std::unordered_map<NodeId, BindingScope>().swap(BindingScopes);
		std::unordered_map<NodeId, std::unordered_set<NodeId>>().swap(SourceReaders);
		std::unordered_map<NodeId, BindingRegion>().swap(BindingRegions);
		std::unordered_multimap<NodeId, NodeId>().swap(ParamScopes);
		std::vector<RefFact>().swap(RefFacts);
		// 終了
		return;
	}

	/**
	 * 縮約時に確定した生成部品の参照の取得関数
	 * 計算量：ノードと操作の組の探索は期待値 O(1)
	 * @param Id 生成形式の対象
	 * @param Operation 選択した操作
	 * @return 縮約したモデルが所有する部品（編集用のモデルでは nullptr）
	 * @throws FormatLimitExceeded 元の部品の生成が失敗した場合
	 * @throws std::bad_alloc 元の部品を確保出来なかった場合
	 * @throws std::length_error 元の部品又は本文の要素数上限を超えた場合
	 * @throws std::out_of_range ノード又は操作の組が表に無い場合
	 */
	const GeneratedNode *SyntaxModel::GetPreparedValidationGeneration(const NodeId Id, const FormOperation Operation) const {
		// 編集用のモデルに縮約済の部品が無い事の返戻
		if(!Validation) return nullptr;
		const ValidationStorage::Generation &Entry =
		Validation->Generations.at(static_cast<uint64_t>(Id) << 32 | static_cast<uint32_t>(Operation));
		if(Entry.Error) std::rethrow_exception(Entry.Error);
		// 所有する部品の参照の返戻
		return &Entry.Node;
	}

	/**
	 * 選択した生成部品の取得関数
	 * 計算量：部品のノード数 G と本文のバイト数 B に対し O(G + B)
	 * @param Id 生成形式の対象
	 * @param Operation 選択した操作
	 * @return 既存の識別子を参照する生成部品
	 * @throws FormatLimitExceeded 生成形式又は必要な部品が無い場合
	 * @throws std::bad_alloc 部品のメモリを確保出来ない場合
	 * @throws std::length_error 部品又は本文の要素数上限を超える場合
	 * @throws std::out_of_range ノード又は操作の組が表に無い場合
	 */
	GeneratedNode SyntaxModel::GetValidationGeneration(const NodeId Id, const FormOperation Operation) const {
		// 所有する部品のコピーを要求した時の返戻
		if(const GeneratedNode *const Prepared = GetPreparedValidationGeneration(Id, Operation)) return *Prepared;
		RewriteStep Step;
		if(Operation == FormOperation::Continuation) Step = SyntaxGeneration::ContinuationStep(*this, Id);
		else {
			const CanonicalForm *const Form = GetFormOf(GetParentOf(Id), Operation);
			if(!Form) throw FormatLimitExceeded("a selected form has no syntax definition", FailureReason::FormatterDefect);
			Step = SyntaxGeneration::BraceBodyStep(*this, Id, *Form);
		}
		Step.Inserted.Fields = GetNode(Id).Fields;
		// 照会結果の返戻
		return std::move(Step.Inserted);
	} // 既存の生成規則で確定した部品の返戻

	/**
	 * 構造照合だけが読む期待の所有表の生成関数
	 * 読取の識別子を保ち，本文・境界・宣言の比較キーを自身で所有する
	 * 計算量：ノード数 N，子の数 C，本文のバイト数 B と選択した宣言・生成部品の処理量 S に対し O(N + C + B + S)
	 * @param Syntax 正規化・配置の終わった期待構文
	 * @param Declarations 照合が読む宣言区分の所有者
	 * @param Generations 照合が読むノードと生成操作の組
	 * @param KeepBoundary 構造照合が読む境界の判定
	 * @return 編集用の表を保持しない独立した期待の所有者
	 * @throws FormatLimitExceeded 構造の作業予算を超える場合
	 * @throws std::bad_alloc 期待の表を確保出来ない場合
	 * @throws std::length_error 期待の列が位置の上限を超える場合
	 */
	SyntaxModel SyntaxModel::CompactForValidation(
		const SyntaxModel &Syntax,
		const std::span<const NodeId> Declarations,
		const std::span<const std::pair<NodeId, FormOperation>> Generations,
		bool (*const KeepBoundary)(const SemanticBoundary &)
	) {
		Syntax.EnsureOrder();
		SyntaxModel Result(Syntax.GetRules(), Syntax.ChildRoleMask);
		const std::shared_ptr<ValidationStorage> Storage = std::make_shared<ValidationStorage>();
		Storage->Records.reserve(Syntax.GetNodeCount() + 1);
		if(!Syntax.Regions.empty()) Storage->Regions.resize(Syntax.GetNodeCount());
		// 各列の開始を番兵にも記録し，空の列にも特別な領域を作らない
		const auto Append = [&Storage, KeepBoundary](const SyntaxNode *const Node, const uint32_t Region) -> void {
			if(Storage->Children.size() > UINT32_MAX || Storage->Lexemes.size() > UINT32_MAX || Storage->Boundaries.size() > UINT32_MAX) {
				throw std::length_error("the validation expectation exceeds its offset range");
			}
			ValidationRecord Record;
			Record.ChildrenBegin = static_cast<uint32_t>(Storage->Children.size());
			Record.LexemeBegin = static_cast<uint32_t>(Storage->Lexemes.size());
			Record.BoundariesBegin = static_cast<uint32_t>(Storage->Boundaries.size());
			if(Node) {
				Record.OrderFirst = Node->OrderFirst;
				Record.OrderLast = Node->OrderLast;
				Record.Kind = Node->Kind;
				Record.Meanings = Node->Facts.Meanings;
				Record.TokenDef = Node->Facts.TokenDef;
				Record.DelimiterDef = Node->Facts.DelimiterDef;
				Record.Fields = Node->Fields;
				Record.Parent = Node->Parent;
				if(!Storage->Regions.empty()) Storage->Regions[Storage->Records.size()] = static_cast<uint8_t>(Region);
				Storage->Children.insert(Storage->Children.end(), Node->Children.begin(), Node->Children.end());
				Storage->Lexemes += Node->Lexeme;
				const std::span<const SemanticBoundary> Boundaries = Node->Facts.Boundaries.GetView();
				for(const SemanticBoundary &Boundary : Boundaries) if(KeepBoundary(Boundary)) Storage->Boundaries.push_back(Boundary);
			}
			Storage->Records.push_back(Record);
			// 追加の終了
			return;
		}; // 所有表の末尾への追加
		for(const SyntaxNode &Node : Syntax.Nodes) {
			const OwnedRegion *const Region = Node.Facts.RegionIdx ? Syntax.Regions[Node.Facts.RegionIdx - 1].get() : nullptr;
			Append(&Node, Region ? Region->Code->GetFragments().empty() ? 2 : 1 : 0);
		}
		Append(nullptr, 0);
		for(const NodeId Node : Declarations) {
			if(Storage->Declarations.contains(Node)) continue;
			ValidationStorage::Declaration Entry;
			try {
				Entry.Parts = Syntax.GetCommaDeclOf(Node);
				if(Entry.Parts) for(DeclShape &Shape : Entry.Parts->Key) {
					Storage->DeclarationText.push_back(std::string(Shape.Lexeme));
					Shape.Lexeme = Storage->DeclarationText.back();
				}
			} catch(...) {
				Entry.Error = std::current_exception();
			}
			Storage->Declarations.emplace(Node, std::move(Entry));
		}
		for(const std::pair<NodeId, FormOperation> &Selection : Generations) {
			if(Selection.second == FormOperation::TrailingComma) {
				if(Storage->Forms.empty()) Storage->Forms.resize(Syntax.GetNodeCount());
				if(Selection.first < Syntax.GetNodeCount()) Storage->Forms[Selection.first] = Syntax.Nodes[Selection.first].Facts.Forms;
				continue;
			}
			if(Selection.second == FormOperation::MemberSeparator) {
				if(Storage->SiblingIndexes.empty()) Storage->SiblingIndexes.resize(Syntax.GetNodeCount());
				if(Selection.first < Syntax.GetNodeCount()) Storage->SiblingIndexes[Selection.first] = Syntax.Nodes[Selection.first].Idx;
				continue;
			}
			const uint64_t Key = static_cast<uint64_t>(Selection.first) << 32 | static_cast<uint32_t>(Selection.second);
			if(Storage->Generations.contains(Key)) continue;
			ValidationStorage::Generation Entry;
			try {
				Entry.Node = Syntax.GetValidationGeneration(Selection.first, Selection.second);
			} catch(...) {
				Entry.Error = std::current_exception();
			}
			Storage->Generations.emplace(Key, std::move(Entry));
		}
		Result.Root = Syntax.Root;
		Result.Validation = Storage;
		Result.Work->IsClosed = true;
		// 照会結果の返戻
		return Result;
	} // 元のモデルから独立した期待の返戻
} // namespace Core
