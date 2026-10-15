#include "Formatter.hpp"
#include "PerformanceObservation.hpp"
#include "Util/FileIO.hpp"
#include "Util/FormatLimitExceeded.hpp"
#include "Util/Lang.hpp"
#include <cstddef>
#include <cstdio>
#include <exception>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

/**
 * 製品の整形の入口へ入力を渡し，各工程の実作業量を取得する関数
 * 計算量：製品の整形・独立検証・静的解析の総費用
 * @param Language 入力の言語
 * @param Src 固定原本から生成した入力
 * @throws std::runtime_error 入力を完全に整形出来ない場合又は計上値が上限を超える場合
 * @throws std::exception 製品の入口から伝わるメモリ・範囲等のエラーの場合
 */
static void Measure(const ::Lang Language, const std::string &Src) {
	// 製品の入口で，全工程と独立検証・静的解析を実行する
	PerformanceObservation::Parsing = PerformanceObservation::ParserWork();
	PerformanceObservation::Pipeline = PerformanceObservation::PipelineWork();
	const Formatter Formatting { Core::LineBreaker(128) };
	const FormatResult Result = Formatting.Format(Src, Language, true);
	if(Result.Classification != FormatClassification::Changed && Result.Classification != FormatClassification::Unchanged) {
		throw std::runtime_error("performance input was not formatted completely");
	}
	const PerformanceObservation::PipelineWork &Work = PerformanceObservation::Pipeline;
	if(
		!Work.Structure || !Work.Wrapping || Work.Structure > Work.Limit || Work.Wrapping > Work.Limit ||
		Work.WrappingLimit != Work.Limit
	) throw std::runtime_error("performance work is missing or exceeds its budget");
	const PerformanceObservation::ParserWork &Parsing = PerformanceObservation::Parsing;
	std::printf(
		"%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\n",
		PerformanceObservation::MetricVersion,
		Work.Structure,
		Work.Wrapping,
		Work.Limit,
		Work.Base,
		Parsing.Calls,
		Parsing.InputBytes,
		Parsing.ReadBytes,
		Parsing.RepairCalls,
		Parsing.RepairReadBytes,
		Work.ValidationCalls,
		Work.ValidationBytes,
		Work.PlacementTargets,
		Work.Relations,
		Work.CastRepairVisits,
		Work.LintNodes,
		Work.LintComments
	);
	// 終了
	return;
}

/**
 * 性能入力１件の決定的な作業単位を照合するテストの入口
 * @param Argc 実引数の数
 * @param Argv 言語名と入力のパス
 * @return 計数の成功時は０，失敗時は１，使方のエラーは２
 */
int main(const int Argc, char **const Argv) {
	// 引数と入力の読込の検査
	if(Argc != 3) {
		std::fprintf(stderr, "usage: verify_performance <language> <input>\n");
		// 使方のエラーの返戻
		return 2;
	}
	try {
		std::ifstream Input(FileIO::PathOf(Argv[2]), std::ios::binary);
		if(!Input) throw std::runtime_error("could not open performance input");
		std::ostringstream Buffer;
		Buffer << Input.rdbuf();
		if(Input.bad() || !Buffer) throw std::runtime_error("could not read performance input");
		// 既存の文法・構文・行分割の処理に依る計数
		Measure(Lang::FromName(Argv[1]), Buffer.str());
	} catch(const FormatLimitExceeded &E) {
		std::fprintf(stderr, "performance: %.*s\n", static_cast<int>(E.Reason.size()), E.Reason.data());
		// 作業予算の超過又は中核の不整合の返戻
		return 1;
	} catch(const std::exception &E) {
		std::fprintf(stderr, "performance: %s\n", E.what());
		// 入力・実行の失敗の返戻
		return 1;
	}
	// 成功の返戻
	return 0;
}
