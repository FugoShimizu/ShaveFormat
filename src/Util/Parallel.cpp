#include "Util/Parallel.hpp"
#include <algorithm>
#include <cstddef>
#include <exception>
#include <thread>
#include <vector>

// ネストの並列化を抑止
thread_local bool Parallel::IsInParallelRegion = false;

/**
 * ========== 並列度の決定 ==========
 * 実効スレッド数決定関数
 * 計算量：O(1)
 * @param Total 処理対象の総数
 * @param MaxThreads スレッド数上限
 * @param Threshold 逐次化する仕事数の上限未満値（仕事１件が重い段では小さく渡す）
 * @return 実効スレッド数（≧ 1）
 */
size_t Parallel::DecideThreads(const size_t Total, const size_t MaxThreads, const size_t Threshold) {
	// 並列区間内の場合の逐次実行用スレッド数の返戻
	if(IsInParallelRegion) return 1;
	static constexpr size_t DefaultThreads = 4; // 並列度未報告時の既定スレッド数
	// 実行環境が報告する並列度の取得
	const unsigned int Detected = std::thread::hardware_concurrency();
	const size_t MaxByCap = std::min<size_t>(Detected ? Detected : DefaultThreads, MaxThreads);
	// 逐次化条件を満たす場合の返戻
	if(!Total || Total < Threshold || MaxByCap < 2) return 1;
	// ハードウェアスレッド数と総タスク数の小さい方の返戻
	return std::min<size_t>(MaxByCap, Total);
}

/**
 * ========== 並列実行入口 ==========
 * チャンク分割並列実行関数（範囲 `[0, Total)` を ThreadCount 個に分割し各々で Callback を並列実行）
 * 計算量：処理対象数 N と作業関数の総費用 Q に対し O(N + Q)
 * @param Total 処理対象の総数
 * @param ThreadCount 実効スレッド数（DecideThreads で算出した値）
 * @param Callback 開始・終端・スレッド番号を引数に取る作業関数
 * @throws std::system_error 起動済のスレッドとの合流に失敗した場合
 * 作業関数が送出した例外は，全担当との合流後に其の型を保って呼出元へ再送出する
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void Parallel::ForChunks(const size_t Total, const size_t ThreadCount, const Parallel::Callback &Callback) {
	// スレッド数が２未満の場合の逐次実行
	if(ThreadCount < 2) {
		// 全範囲の逐次実行
		Callback(0, Total, 0);
		// 逐次実行を終えた場合の返戻
		return;
	}
	// 端数切上に依る担当幅の算出
	const size_t ChunkSize = (Total + ThreadCount - 1) / ThreadCount;
	// 区間毎の例外の退避と全担当合流後の再送出
	std::vector<std::exception_ptr> Errors(ThreadCount);
	// 担当区間を並列区間の状態で実行し，例外を境界外へ漏らさない呼出処理（生成に失敗した区間は呼出元のスレッドで同期実行する）
	const auto Run = [&Callback, &Errors](const size_t Start, const size_t End, const size_t ThreadId) -> void {
		// 呼出前の並列区間の状態の保持
		const bool HasOuterParallelRegion = IsInParallelRegion;
		IsInParallelRegion = true;
		// 担当区間の実行
		try {
			Callback(Start, End, ThreadId);
		} catch(...) {
			// 担当識別番号別の例外退避
			Errors[ThreadId] = std::current_exception();
		}
		IsInParallelRegion = HasOuterParallelRegion;
	};
	// 起動済スレッドの保持領域
	std::vector<std::thread> Threads;
	Threads.reserve(ThreadCount);
	for(size_t ThreadId = 0; ThreadId < ThreadCount; ++ThreadId) {
		const size_t Start = ThreadId * ChunkSize, End = std::min(Start + ChunkSize, Total);
		if(Start >= End) continue;
		// 生成失敗区間の呼出元スレッドでの同期実行（リソース・確保の何れの失敗でも起動済のスレッドを合流前に破棄しない）
		try {
			Threads.emplace_back(Run, Start, End, ThreadId);
		} catch(...) {
			Run(Start, End, ThreadId);
		}
	}
	// 起動済全担当の合流
	for(std::thread &Worker : Threads) Worker.join();
	// 全担当合流後の担当順に依る例外の再送出
	for(const std::exception_ptr &Error : Errors) if(Error) std::rethrow_exception(Error);
	// 終了
	return;
}
