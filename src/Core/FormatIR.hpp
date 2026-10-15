#pragma once

#include "NodeId.hpp"
#include "SyntaxModel.hpp"
#include "SyntaxRole.hpp"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Core {
	using FormatItemId = uint32_t; // 中間表現の要素の番号

	inline constexpr uint32_t NoGroup = std::numeric_limits<uint32_t>::max(); // グループの無いマーカー
	inline constexpr FormatItemId NoItem = std::numeric_limits<FormatItemId>::max(); // 要素の無いマーカー
	inline constexpr uint32_t NoDetail = std::numeric_limits<uint32_t>::max(); // 属性の無いマーカー

	enum class FormatItemKind : uint8_t {
		Sequence, // 子の並び
		AnchorMark, // ノードへ戻れる出力上のアンカー（子の範囲が其のノードの出力）
		BlankLine, // 構造から決めた空行
		DeclarationSplit, // 行幅に依り複数の宣言へ分ける統合宣言（子は接頭部・中間部・終端部・宣言子の区切・宣言の間の改行と宣言子毎の名前と初期化子の組，空の部分は NoItem，行分割仕様 4.1）
		Group, // 共通の選択単位
		HardBreak, // 必須の改行
		IfBroken, // 指定したグループの選択形だけ出す断片（既定は展開時）
		IndentScope, // 構文上の本体又は継続範囲の深さ
		LexicalSeparator, // トークン列を再現する固定の区切記法
		OwnedRegion, // 所有領域の出力を置く位置（アンカーのノードの所有領域の行を配置の段が此の深さへ置く）
		SoftBreak, // 平坦時と折返時の表現を持つ選択可能な境界（最も内のグループに所属する）
		Space, // トークンの区切又は規約が要求する空白
		Token, // デコード済のトークン又は規則から生成する固定トークン
		Verbatim // 変更不能な値・照合対象
	}; // 中間表現の要素の種類（整形仕様 6.1）

	enum class GeneratedToken : uint8_t { None, BlockClose, BlockOpen, CallClose, CallOpen, ListSeparator, StmtTerminator }; // 規則から生成したトークンの構文上の種類 // ノード由来のトークン又はトークン以外の要素 // 本体を閉じる波括弧 // 本体を包む開き波括弧 // 呼出又は一覧を閉じる丸括弧 // 呼出又は一覧を開く丸括弧 // 展開した並びの末尾の区切 // 規則から補う文の終端

	// 共有領域の中の連続範囲（テキストのバイト又は子の番号の範囲）
	struct FormatRange {
		uint32_t Begin = 0; // 先頭の位置
		uint32_t Size = 0; // 要素の数
	};

	// 一部の要素だけが持つ属性
	struct FormatItemDetail {
		NodeId BodyParent = NoNode; // 幅で包む本体の構文上の親
		uint32_t ExpansionDependency = NoGroup; // 此のグループの展開を必要とする先行グループ
		NodeId PunctuationOwner = NoNode; // 選択して生成する句読点の構文位置（末尾カンマの所有者，又はメンバ区切の直後の子）
		NodeId DeclOwner = NoNode; // コピーした共有部分の元の統合宣言
		uint32_t DeclRow = NoGroup; // コピーした共有部分の出力宣言番号
		NodeId Before = NoNode; // 空行の規則の前の宣言（何方かが複数行なら空行を置く）
		NodeId After = NoNode; // 空行の規則の後の宣言
		bool IsChainBody = false; // 本体が制御見出の連鎖か
		bool IsForced = false; // 構造種別の違い等で常に空行を置く規則か
		bool DoesExcludeCodeNewlines = false; // リテラルの値の改行をコードの展開要因にしないか
		bool DoesExcludeTextNewlines = false; // 逐語テキストの改行が全てコメント内で，空行判定では数えないか
	};

	// 中間表現の要素（整形仕様 6.1，任意の文字列に埋めたマーカーを持たず，子とテキストは共有領域の範囲で参照する）
	struct FormatItem {
		FormatItemKind Kind = FormatItemKind::Sequence; // 要素の種類
		GeneratedToken Generated = GeneratedToken::None; // 規則から生成したトークンの種類
		bool HasNewline = false; // テキストに改行を含むか
		bool IsActivatedWhenFlat = false; // 条件付断片が展開時でなく平坦時に有効か
		// 逐語の先頭行と末尾行の内容の有無
		bool HasHeadContent = false;
		bool HasTailContent = false;
		std::optional<FormOperation> Operation; // グループの展開時に選ぶ構文上の操作（単なる折返には無）
		uint32_t Group = NoGroup; // グループの番号（グループ自身と，条件付断片が参照するグループ）
		NodeId Node = NoNode; // アンカーのノード（トークンの葉と逐語領域は其のテキストの要素自身がアンカーを兼ねる）
		FormatRange Text; // トークン・分離記法・逐語のテキストと，選択可能な境界の平坦時のテキスト
		FormatRange Children; // 子の番号の並び
		uint32_t Detail = NoDetail; // 一部の要素だけが持つ属性の番号（無ければ無）
	};

	enum class DecisionProcedure : uint8_t { Ordinary, BraceBody, Continuation }; // 決定単位の直接の判定手順

	// 生成形式が対応付ける決定単位
	struct FormatDecision {
		uint32_t Group = NoGroup; // 元のグループ番号
		uint32_t UnitOwner = NoGroup; // 同時に確定する単位の代表順位
		uint32_t UnitRank = NoGroup; // 単位に含むグループの最小基準順位
		std::vector<uint32_t> Members; // 元の所属を保つ単位内のグループ
		uint32_t ExpansionDependency = NoGroup; // 展開を強制する先行順位
		uint32_t Prefix = NoGroup; // 継続括弧又は制御見出の連鎖で参照する外側のグループの順位
		uint32_t Annotation = NoGroup; // 右辺より先にラッパーを確定する型注釈の継続グループの順位
		uint32_t Rank = NoGroup; // 構築時の後行順の基準順位
		DecisionProcedure Procedure = DecisionProcedure::Ordinary; // 合法形の優先順位を定める手順
		std::vector<uint32_t> Children; // 次に必要なローカル要約を供給する直接の子のグループ
		bool IsContinuationContent = false; // 継続単位の括弧外の候補を所有するか
		bool IsContinuationList = false; // 継続括弧と要素区切を同じコンテナとして選ぶか
		bool IsContinuationMember = false; // 後続のコンテナより先に確定するメンバ参照か
		bool IsContinuationType = false; // 型の内側の評価で未確定の継続境界を保つか
		bool IsChainBody = false; // 本体の一般の式と見出の連鎖を区別するか
		std::vector<uint32_t> Dependencies; // 先に確定する直接の依存先
		std::vector<uint32_t> Successors; // 完了を１度通知する後続先
	};

	// 生成元の連続番号は直接参照し，任意の番号だけ別表へ置く順位表
	class GroupRanks {
	private:

		std::vector<uint32_t> Dense; // 生成元が定めた番号の範囲
		std::unordered_map<uint32_t, uint32_t> Sparse; // 範囲外又は任意の番号の対応
		size_t Count = 0; // 登録したグループの数

	public:

		std::optional<uint32_t> Find(const uint32_t Group) const; // 登録済の順位の取得関数
		uint32_t Get(const uint32_t Group) const; // 必ず存在する順位の取得関数
		size_t GetSize() const noexcept; // 登録済のグループ数の取得関数
		size_t GetCapacity() const noexcept; // 直接参照する番号の範囲の取得関数
		void Reset(const size_t Capacity); // 番号の範囲を指定して空の表へ戻す関数
		void Set(const uint32_t Group, const uint32_t Rank); // グループの順位の登録・更新関数
	};

	// 幅非依存の中間表現（整形仕様 6.1，要素・子の並び・テキストを共有し，構築した時点から期待構文を変更しない）
	struct FormatIR {
		std::vector<FormatItem> Items; // 要素（番号で参照し，平坦形・展開形が同じ部分木を共有する）
		std::vector<FormatItemId> Links; // 要素毎の子の番号の並び
		std::vector<FormatItemDetail> Details;
		std::vector<FormatDecision> Decisions; // 構築時に対応付けた決定単位（後行順）
		GroupRanks DecisionOf; // 構築時のグループ番号から基準順位への対応
		std::vector<FormatItemId> Decls; // 構築時に記録した統合宣言の後行順
		std::unordered_set<NodeId> BlankRefs; // 空行の比較が参照する宣言
		std::string Texts; // テキスト
		size_t LinearizedCount = 0; // 決定単位の走査で数えた原子の出現数（再構築前は０）
		FormatItemId Root = NoItem; // 根の要素
		std::shared_ptr<const SyntaxModel> NormalizedSyntax; // 読取専用の期待構文を検証完了迄共有
		std::span<const FormatItemId> GetChildrenOf(const FormatItem &Item) const; // 要素の子の取得関数
		std::string_view GetTextOf(const FormatItem &Item) const; // 要素のテキストの取得関数
		const FormatItemDetail &GetDetailOf(const uint32_t Detail) const; // 要素の属性の取得関数
		std::vector<FormatItemId> GetDeclChildrenOf(const FormatItem &Item) const; // 宣言区分の出力順の子の取得関数
		FormatRange Link(const std::span<const FormatItemId> Children); // 子の並びの追加関数
		FormatItemId Add(const FormatItem &Item); // 要素の追加関数
		FormatItemId Parent(const FormatItemKind Kind, const std::span<const FormatItemId> Children); // 子を持つ要素の追加関数
		FormatItemId LeafRange(const FormatRange Range, const FormatItemKind Kind, const bool IsUnbroken = false); // 共有テキストの範囲を持つ葉の追加関数
		FormatRange Keep(const std::string_view Text); // テキストの追加関数
		FormatItemDetail &EditDetail(const FormatItemId Id); // 要素の属性の変更関数
		void BuildDecisions(const size_t GroupCapacity = 0); // 生成形式の決定単位と直接依存の構築関数
		void CloseWork(); // 中間表現の構築予算の終了関数
		FormatItemId Leaf(const std::string_view Text, const FormatItemKind Kind); // 子を持たない要素の追加関数
	};

	enum class ResolvedKind : uint8_t {
		Text,
		AnchorBegin,
		AnchorEnd,
		BlankLine,
		IndentBegin,
		IndentEnd,
		Newline,
		OwnedRegion,
		Separator,
		Verbatim
	}; // 解決済の要素の種類

	// 解決済の要素（未解決の選択を持たない）
	struct ResolvedItem {
		ResolvedKind Kind = ResolvedKind::Text;
		bool IsForced = false; // 常に空行を置く規則か
		FormatRange Text; // テキストの範囲
		NodeId Node = NoNode; // アンカーのノード（テキストの要素は其のテキスト自身がアンカーを兼ねる）
		NodeId Before = NoNode; // 空行の規則の前の宣言
		NodeId After = NoNode;
		GeneratedToken Generated = GeneratedToken::None;
	};

	// 元の統合宣言の宣言子の連続範囲（終端は含まない）
	struct DeclaratorRange {
		size_t Begin = 0; // 先頭の宣言子の番号（０始まり）
		size_t End = 0; // 次の範囲の先頭の番号
	};

	// 幅で確定した統合宣言の区分（Rows の要素数が出力する宣言の数）
	struct DeclPartition {
		NodeId Node = NoNode; // 元の統合宣言のノード
		std::vector<DeclaratorRange> Rows; // 出力する宣言毎の宣言子の範囲
	};

	// グループの採用形に伴う構文上の操作
	struct SelectedFormEffect {
		NodeId Node = NoNode; // ラッパー等の対象のノード
		FormOperation Operation = FormOperation::BraceBody; // 生成する構文の共通操作
		NodeId DeclOwner = NoNode; // 共有部分をコピーした統合宣言（其の外では無）
		uint32_t DeclRow = NoGroup;
	};

	// グループ又は必須展開の採用形式（共有するグループの全対象を失わない）
	struct SelectedForm {
		bool IsBroken = false; // 展開形を選んだか
		std::vector<NodeId> Nodes; // グループが受け持つノードの並び
		std::vector<SelectedFormEffect> Effects; // 採用形で選んだ構文上の操作
	};

	// 検証へ渡す期待結果（整形仕様 6.1，正規化後の構文と選択した表現の対応）
	struct OutputExpectation {
		std::shared_ptr<const SyntaxModel> NormalizedSyntax; // 正規化後の構文を検証完了迄共有
		std::vector<SelectedForm> SelectedForms; // グループの選択を後行順で先頭へ置き，必須展開の形式を其の後へ置く
		std::vector<DeclPartition> DeclarationPartitions; // 元の統合宣言毎に選択した出力区分
	};

	// 全ての選択を解決した中間表現（整形仕様 6.1）
	struct BrokenIR {
		std::vector<ResolvedItem> Items; // 解決済の要素の並び
		std::unordered_set<NodeId> Multiline; // 解決済の要素の出力時に確定した複数行の宣言
		std::string Texts; // 中間表現から引き継いだテキスト
		OutputExpectation Expectation; // 検証用の期待結果
		size_t TextItemCount = 0; // 配置で本文とトークン片を出し得る要素数
		size_t LineBreakCount = 0; // 配置で行を増やし得る境界数（逐語と子の断片は別に追加する）
		std::string_view GetTextOf(const ResolvedItem &Item) const; // 解決済の要素のテキストの取得関数
	};
} // namespace Core
