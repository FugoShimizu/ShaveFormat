#pragma once

#include "SyntaxModel.hpp"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Core {
	using FormatItemId = uint32_t; // 中間表現の要素の番号

	inline constexpr uint32_t NoGroup = std::numeric_limits<uint32_t>::max(); // 群の無い印
	inline constexpr FormatItemId NoItem = std::numeric_limits<FormatItemId>::max(); // 要素の無い印
	inline constexpr uint32_t NoDetail = std::numeric_limits<uint32_t>::max(); // 属性の無い印

	enum class FormatItemKind : uint8_t {
		Sequence, // 子の並び
		AnchorMark, // 節点へ戻れる出力上の錨（子の範囲が其の節点の出力）
		BlankLine, // 構造から決めた空行
		DeclarationSplit, // 行幅に依り複数の宣言へ分ける統合宣言（子は接頭部・中間部・終端部・宣言子の区切り・宣言の間の改行と宣言子毎の名前と初期化子の組，行分割仕様 4.1）
		Group, // 共通の選択単位
		HardBreak, // 必須の改行
		IfBroken, // 指定した群の展開時だけ出す断片
		IndentScope, // 構文上の本体又は継続範囲の深さ
		LexicalSeparator, // 字句列を再現する固定の区切記法
		LineEdgeSpace, // 行端では子の字句列（空白の式）で表す表示上の空白（字面は行の内側の表現）
		OwnedRegion, // 所有区画の出力を置く位置（錨の節点の所有区画の行を配置の段が此の深さへ置く）
		SoftBreak, // 平坦時と折返時の表現を持つ選択可能な境界（最も内の群に所属する）
		Space, // 字句の区切り又は規約が要求する空白
		Token, // 復号済の字句又は規則から生成する固定字句
		Verbatim // 変更不能な値・照合対象
	}; // 中間表現の要素の種類（整形仕様 6.1）

	enum class GeneratedToken : uint8_t { None, BlockClose, BlockOpen, CallClose, CallOpen, ListSeparator, StatementTerminator }; // 規則から生成した字句の構文上の種類 // 節点由来の字句又は字句以外の要素 // 本体を閉じる波括弧 // 本体を包む開き波括弧 // 呼出又は一覧を閉じる丸括弧 // 呼出又は一覧を開く丸括弧 // 展開した並びの末尾の区切り // 規則から補う文の終端

	// 共有領域の中の連続範囲（字面の字又は子の番号の範囲）
	struct FormatRange {
		uint32_t Begin = 0; // 先頭の位置
		uint32_t Size = 0; // 要素の数
	};

	// 一部の要素だけが持つ属性
	struct FormatItemDetail {
		uint32_t ExpansionDependency = NoGroup; // 此の群の展開を必要とする先行群
		NodeId PunctuationOwner = NoNode; // 選択して生成する句読点の所属節点
		NodeId DeclarationOwner = NoNode; // 複写した共有部分の元の統合宣言
		uint32_t DeclarationRow = NoGroup; // 複写した共有部分の出力宣言番号
		NodeId Before = NoNode; // 空行の規則の前の宣言（何方かが複数行なら空行を置く）
		NodeId After = NoNode; // 空行の規則の後の宣言
		bool IsForced = false; // 構造種別の違い等で常に空行を置く規則か
	};

	// 中間表現の要素（整形仕様 6.1，任意の文字列に埋めた印を持たず，子と字面は共有領域の範囲で参照する）
	struct FormatItem {
		FormatItemKind Kind = FormatItemKind::Sequence; // 要素の種類
		GeneratedToken Generated = GeneratedToken::None; // 規則から生成した字句の種類
		std::optional<FormOperation> Operation; // 群の展開時に選ぶ構文上の操作（単なる折返には無し）
		uint32_t Group = NoGroup; // 群の番号（群自身と，展開時だけ出す断片が参照する群）
		NodeId Node = NoNode; // 錨の節点（字句の葉と逐語区画は其の字面の要素自身が錨を兼ねる）
		FormatRange Text; // 字句・分離記法・逐語の字面と，選択可能な境界の平坦時の字面
		FormatRange Children; // 子の番号の並び
		uint32_t Detail = NoDetail; // 一部の要素だけが持つ属性の番号（無ければ無し）
	};

	// 幅非依存の中間表現（整形仕様 6.1，要素・子の並び・字面を共有し，構築した時点から期待構文を変更しない）
	struct FormatIR {
		std::vector<FormatItem> Items; // 要素（番号で参照し，平坦形・展開形が同じ部分木を共有する）
		std::vector<FormatItemId> Links; // 要素毎の子の番号の並び
		std::vector<FormatItemDetail> Details; // 一部の要素だけが持つ属性
		std::string Texts; // 字面
		FormatItemId Root = NoItem; // 根の要素
		std::shared_ptr<const SyntaxModel> NormalizedSyntax; // 読取専用の期待構文を検証完了迄共有
		std::string_view TextOf(const FormatItem &Item) const; // 要素の字面の取得関数
		std::span<const FormatItemId> ChildrenOf(const FormatItem &Item) const; // 要素の子の取得関数
		const FormatItemDetail &DetailOf(const uint32_t Detail) const; // 要素の属性の取得関数
		void CheckNesting() const; // ネストの段数の検査関数
		FormatRange Keep(const std::string_view Text); // 字面の追加関数
		void CloseWork(); // 中間表現の構築予算の終了関数
		FormatRange Link(const std::span<const FormatItemId> Children); // 子の並びの追加関数
		FormatItemId Add(const FormatItem &Item); // 要素の追加関数
		FormatItemId Leaf(const std::string_view Text, const FormatItemKind Kind); // 子を持たない要素の追加関数
		FormatItemId Parent(const FormatItemKind Kind, const std::span<const FormatItemId> Children); // 子を持つ要素の追加関数
		FormatItemDetail &EditDetail(const FormatItemId Id); // 要素の属性の変更関数
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
		ResolvedKind Kind = ResolvedKind::Text; // 要素の種類
		bool IsForced = false; // 常に空行を置く規則か
		FormatRange Text; // 字面の範囲
		NodeId Node = NoNode; // 錨の節点（字面の要素は其の字面自身が錨を兼ねる）
		NodeId Before = NoNode; // 空行の規則の前の宣言
		NodeId After = NoNode; // 空行の規則の後の宣言
		GeneratedToken Generated = GeneratedToken::None; // 規則から生成した字句の種類
	};

	// 元の統合宣言の宣言子の連続範囲（終端は含まない）
	struct DeclaratorRange {
		size_t Begin = 0; // 先頭の宣言子の番号（０始まり）
		size_t End = 0; // 次の範囲の先頭の番号
	};

	// 幅で確定した統合宣言の区分（Rows の要素数が出力する宣言の数）
	struct DeclarationPartition {
		NodeId Node = NoNode; // 元の統合宣言の節点
		std::vector<DeclaratorRange> Rows; // 出力する宣言毎の宣言子の範囲
	};

	// 群の展開に伴う構文上の操作
	struct SelectedFormEffect {
		NodeId Node = NoNode; // 包装等の対象の節点
		FormOperation Operation = FormOperation::BraceBody; // 生成する構文の共通操作
		NodeId DeclarationOwner = NoNode; // 共有部分を複写した統合宣言（其の外では無し）
		uint32_t DeclarationRow = NoGroup; // 複写した共有部分の出力宣言番号
	};

	// 群又は必須展開の採用形式（共有する群の全対象を失わない）
	struct SelectedForm {
		uint8_t IsBroken = 0; // 展開形を選んだか
		std::vector<NodeId> Nodes; // 群が受け持つ節点の並び
		std::vector<SelectedFormEffect> Effects; // 展開時に選んだ構文上の操作
	};

	// 検証へ渡す期待結果（整形仕様 6.1，正規化後の構文と選択した表現の対応）
	struct OutputExpectation {
		std::shared_ptr<const SyntaxModel> NormalizedSyntax; // 正規化後の構文を検証完了迄共有
		std::vector<SelectedForm> SelectedForms; // 群の選択を後行順で先頭へ置き，必須展開の形式を其の後へ置く
		std::vector<DeclarationPartition> DeclarationPartitions; // 元の統合宣言毎に選択した出力区分
	};

	// 全ての選択を解決した中間表現（整形仕様 6.1）
	struct BrokenIR {
		std::vector<ResolvedItem> Items; // 解決済の要素の並び
		std::string Texts; // 中間表現から引き継いだ字面
		OutputExpectation Expectation; // 検証用の期待結果
		std::string_view TextOf(const ResolvedItem &Item) const; // 解決済の要素の字面の取得関数
	};
} // namespace Core
