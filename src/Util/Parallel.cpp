#include "Util/Parallel.hpp"
#include <algorithm>
#include <exception>
#include <system_error>
#include <thread>
#include <vector>

// ネストの並列化を抑止
thread_local bool Parallel::IsInParallelRegion = false;

/** ========== 並列度の決定 ========== */
/**
 * 実効スレッド数決定関数
 * @param Total 処理対象の総数
 * @param MaxThreads スレッド数上限
 * @param Threshold 逐次化する仕事数の上限未満値（仕事１件が重い段では小さく渡す）
 * @return 実効スレッド数（１以上）
 */
size_t Parallel::DecideThreads(const size_t Total, const size_t MaxThreads, const size_t Threshold) {
	// 並列区間内の場合の逐次実行用走脈数の返戻
	if(IsInParallelRegion) return 1;
	static constexpr size_t DefaultThreads = 4; // 並列度未報告時の既定走脈数
	// 実行環境が報告する並列度の取得
	const unsigned int Detected = std::thread::hardware_concurrency();
	const size_t MaxByCap = std::min<size_t>(Detected ? Detected : DefaultThreads, MaxThreads);
	// 逐次化条件を満たす場合の返戻
	if(!Total || Total < Threshold || MaxByCap < 2) return 1;
	// ハードウェア走脈数と総タスク数の小さい方の返戻
	return std::min<size_t>(MaxByCap, Total);
}

/** ========== 並列実行入口 ========== */
/**
 * チャンク分割並列実行関数（範囲 `[0, Total)` を NumThreads 個に分割し各々で Callback を並列実行）
 * 計算量：処理対象の総数 N に対し O(N)
 * @param Total 処理対象の総数
 * @param NumThreads 実効スレッド数（DecideThreads で算出した値）
 * @param Callback (Start, End, Tid) を引数に取る関数オブジェクト
 */
void Parallel::ForChunks(const size_t Total, const size_t NumThreads, const Parallel::Callback &Callback) {
	// 走脈数が２未満の場合の逐次実行
	if(NumThreads < 2) {
		// 全範囲の逐次実行
		Callback(0, Total, 0);
		// 終了
		return;
	}
	// 端数切上に依る担当幅の算出
	const size_t ChunkSize = Total / NumThreads + !!(Total % NumThreads);
	// 区間毎の例外の退避と全担当合流後の再送出
	std::vector<std::exception_ptr> Errors(NumThreads);
	// 担当区間を並列区間の状態で実行し，例外を境界外へ漏らさない呼出器（生成に失敗した区間は呼出元の走脈で同期実行する）
	const auto Run = [&Callback, &Errors](const size_t Start, const size_t End, const size_t Tid) -> void {
		const bool WasInRegion = IsInParallelRegion;
		IsInParallelRegion = true;
		// 担当区間の実行
		try {
			Callback(Start, End, Tid);
		} catch(...) {
			// 担当識別番号別の例外退避
			Errors[Tid] = std::current_exception();
		}
		IsInParallelRegion = WasInRegion;
	};
	// 起動済走脈の保持領域
	std::vector<std::thread> Threads;
	Threads.reserve(NumThreads);
	for(size_t Tid = 0; Tid < NumThreads; ++Tid) {
		const size_t Start = Tid * ChunkSize, End = std::min(Start + ChunkSize, Total);
		if(Start >= End) continue;
		// 生成失敗区間の呼出元走脈での同期実行
		try {
			Threads.emplace_back(Run, Start, End, Tid);
		} catch(const std::system_error &) {
			Run(Start, End, Tid);
		}
	}
	// 起動済全担当の合流
	for(std::thread &Worker : Threads) Worker.join();
	// 全担当合流後の担当順に依る例外の再送出
	for(const std::exception_ptr &Error : Errors) if(Error) std::rethrow_exception(Error);
	// 終了
	return;
}
