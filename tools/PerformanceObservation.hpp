#pragma once

#include "Core/ValidatedOutput.hpp"
#include <cstddef>

namespace PerformanceObservation {
	inline constexpr std::size_t MetricVersion = 3; // 基準量と処理系に依存しない比較順を持つ計数の形式

	// 計数ツールだけが使う，解析の呼出と実際の受渡バイト数
	struct ParserWork {
		std::size_t Calls = 0; // 全ての解析の回数
		std::size_t InputBytes = 0; // 各解析へ投入したテキスト長の総和
		std::size_t ReadBytes = 0; // パーサーへ実際に渡したバイト数の総和
		std::size_t RepairCalls = 0; // 差分修復の解析の回数
		std::size_t RepairReadBytes = 0; // 差分修復で実際に渡したバイト数の総和
	};

	inline thread_local ParserWork Parsing; // 同じスレッドの入力側と独立した検証側で共有する観測先

	// 製品の工程の入口を通った呼出の計数と基準量
	struct PipelineWork {
		std::size_t Structure = 0; // 構文の正規化と中間表現の構築の作業量
		std::size_t Wrapping = 0; // 行分割の作業量
		std::size_t Base = 0; // 入力と埋込領域の投影直後の基準量の総和
		std::size_t Limit = 0; // 中核の作業上限
		std::size_t WrappingLimit = 0; // 行分割へ引き継いだ作業上限
		std::size_t ValidationCalls = 0; // 独立検証での解析の回数
		std::size_t ValidationBytes = 0; // 独立検証の解析へ投入したバイト数
		std::size_t PlacementTargets = 0; // 配置するコメントの数
		std::size_t Relations = 0; // 入力の解析の所属の作業量
		std::size_t CastRepairVisits = 0; // 入力の型変換の修復の作業量
		std::size_t LintNodes = 0; // 静的解析の候補構文のノード数
		std::size_t LintComments = 0; // 静的解析の候補コメント数
	};

	inline thread_local PipelineWork Pipeline; // 計測専用の翻訳でだけ記録する工程の観測先

	/**
	 * 検証済の候補から，静的解析が受け取る対象数だけを記録する
	 * 計算量：O(1)
	 * @param Final 製品の独立検証を通った候補
	 */
	inline void ObserveValidated(const Core::ValidatedOutput &Final) {
		Pipeline.LintNodes += Final.FinalSyntax.GetNodeCount();
		Pipeline.LintComments += Final.FinalComments.GetRecords().size();
		// 終了
		return;
	}
} // namespace PerformanceObservation
