#pragma once

#include "FormatIR.hpp"
#include "../Util/LayoutTypes.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Core {
	// 検査指定時だけ所有する中核モデルの読取専用な観測
	class CoreObservation {
	private:

		std::vector<NodeId> Projected {}; // 投影時の前順のノード
		std::vector<NodeId> Normalized {}; // 正規化後の前順のノード
		std::unordered_map<NodeId, size_t> Positions {}; // 正規化後のノードの前順番号（１始まり）
		std::unordered_map<const CanonicalForm *, size_t> Forms {}; // 初めて現れた生成形式列の番号
		std::unordered_map<const KindDef *, size_t> Kinds {}; // 初めて現れた順の共通の種別の番号
		std::vector<std::vector<NodeId>> SelectionNodes; // 観測側だけで収集する決定順位毎のノード
		std::unordered_set<uint64_t> SeenSelections; // 同じ決定とノードの再出現の除外
		std::string Records {}; // 標準エラーへ渡す観測の行
		static std::string Hex(const std::string_view Text); // テキストの１６進表現の取得関数
		size_t GetPos(const NodeId Node) const; // 正規化後の前順番号の取得関数
		void Append(const size_t Val); // 数値フィールドの追記関数
		void AppendText(const std::string_view Text); // テキストフィールドの追記関数
		size_t ObserveKind(const KindDef *const Def); // 固定の種別定義の記録と番号の取得関数
		size_t ObserveForms(const CanonicalFormList &List); // 生成形式列の記録と番号の取得関数
		void ObserveFacts(const SyntaxModel &Syntax, const NodeId Id); // ノードの構文事実の記録関数

	public:

		void RecordSelection(const NodeId Node, const uint32_t Rank); // グループの閉じで観測するノードの記録関数
		void ReorderSelections(const std::span<const uint32_t> Ranks); // 選択の観測を出力の決定順位へ対応付ける関数
		void CompleteSelections(std::span<SelectedForm> Selections); // 観測したノードを診断だけが読む列へ移譲する関数
		void ObserveSyntax(const SyntaxModel &Syntax, const RewriteTrace &Trace); // 正規化した構文とアンカーの対応の記録関数
		std::string Take(); // 観測の所有権の受渡関数
		void ObserveFormat(const FormatIR &IR); // 幅非依存の中間表現の記録関数
		void ObserveBroken(const BrokenIR &IR); // 基準順位の選択状態と解決済要素の記録関数
		void ObserveLayout(const LaidOutCode &Code); // 確定したコードの配置計画の記録関数

		explicit CoreObservation(const SyntaxModel &Syntax); // 投影時の前順を記録するコンストラクタ
		explicit CoreObservation(); // ノード収集だけを使う観測のコンストラクタ
	};
} // namespace Core
