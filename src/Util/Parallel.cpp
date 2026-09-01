#include "Util/Parallel.hpp"
#include <algorithm>
#include <exception>
#include <system_error>
#include <thread>
#include <vector>

#if !defined(_WIN32)

#include <pthread.h>

#endif

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

#if !defined(_WIN32)

/** ========== POSIX 走脈入口 ========== */
/**
 * pthread ワーカーエントリ関数
 * @param Raw WorkerArg ポインタ
 * @return 未使用（合流を介した同期のみ）
 */
void *Parallel::WorkerEntry(void *const Raw) noexcept {
	const WorkerArg *const Arg = static_cast<const WorkerArg *>(Raw);
	// 並列担当の開始関数を例外境界にして C の呼出規約外への送出を防ぐ
	try {
		IsInParallelRegion = true;
		(*Arg->Func)(Arg->Start, Arg->End, Arg->Tid);
	} catch(...) {
		*Arg->Error = std::current_exception();
	}
	// 並列担当を終えた局所状態の復元
	IsInParallelRegion = false;
	// 未使用値の返戻
	return nullptr;
}

/**
 * 新しい走脈のエントリ関数
 * @param Raw 実行する処理（`std::function<void()>`）へのポインタ
 * @return 未使用（合流を介した同期のみ）
 */
void *Parallel::FreshStackEntry(void *const Raw) noexcept {
	const FreshStackArg *const Arg = static_cast<const FreshStackArg *>(Raw);
	// 再帰継続の開始関数を例外境界にして C の呼出規約外への送出を防ぐ
	try {
		IsInParallelRegion = Arg->IsInRegion;
		(*Arg->Body)();
	} catch(...) {
		*Arg->Error = std::current_exception();
	}
	// 再帰継続を終えた局所状態の復元
	IsInParallelRegion = false;
	// 未使用値の返戻
	return nullptr;
}

#endif

/** ========== 並列実行入口 ========== */
/**
 * チャンク分割並列実行関数（範囲 `[0, Total)` を NumThreads 個に分割し各々で Callback を並列実行）
 * @param Total 処理対象の総数
 * @param NumThreads 実効スレッド数（DecideThreads で算出した値）
 * @param Callback (Start, End, Tid) を引数に取る関数オブジェクト
 * 計算量：処理対象の総数 N に対し O(N)
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
	// 例外を境界外へ漏らさない呼出器
	const Parallel::Callback Guarded = [&Callback, &Errors](const size_t Start, const size_t End, const size_t Tid) -> void {
		// 担当区間の実行
		try {
			Callback(Start, End, Tid);
		} catch(...) {
			// 担当識別番号別の例外退避
			Errors[Tid] = std::current_exception();
		}
	};
	#if !defined(_WIN32)
	// 呼出元で同期実行する担当にも通常ワーカーと同じ並列区間状態を通知
	const Parallel::Callback RunInCallingThread = [&Guarded](const size_t Start, const size_t End, const size_t Tid) -> void {
		const bool WasInRegion = IsInParallelRegion;
		IsInParallelRegion = true;
		Guarded(Start, End, Tid);
		IsInParallelRegion = WasInRegion;
	};
	// 深い構文木再帰に備えた走脈領域の確保
	pthread_attr_t Attributes;
	const int InitFailed = pthread_attr_init(&Attributes);
	const int StackFailed = InitFailed ? 0 : pthread_attr_setstacksize(&Attributes, WorkerStackSize);
	if(InitFailed || StackFailed) {
		if(const int DestroyFailed = InitFailed ? 0 : pthread_attr_destroy(&Attributes); DestroyFailed) {
			throw std::system_error(DestroyFailed, std::generic_category(), "cannot release thread attributes");
		}
		RunInCallingThread(0, Total, 0);
		if(Errors[0]) std::rethrow_exception(Errors[0]);
		// 終了
		return;
	}
	// ワーカー引数と起動済走脈の保持領域
	std::vector<WorkerArg> Args;
	Args.reserve(NumThreads);
	std::vector<pthread_t> Threads;
	Threads.reserve(NumThreads);
	for(size_t Tid = 0; Tid < NumThreads; ++Tid) {
		const size_t Start = Tid * ChunkSize, End = std::min(Start + ChunkSize, Total);
		if(Start >= End) continue;
		Args.push_back({ &Callback, &Errors[Tid], Start, End, Tid });
		pthread_t Handle {};
		// 走脈生成失敗時の呼出元での同期実行
		if(pthread_create(&Handle, &Attributes, &Parallel::WorkerEntry, &Args.back())) RunInCallingThread(Start, End, Tid);
		else Threads.push_back(Handle);
	}
	// 走脈属性の解放
	const int DestroyFailed = pthread_attr_destroy(&Attributes);
	// 起動済全担当の合流
	for(const pthread_t Handle : Threads) if(pthread_join(Handle, nullptr)) std::terminate();
	// 属性解放失敗の通知
	if(DestroyFailed) throw std::system_error(DestroyFailed, std::generic_category(), "cannot release thread attributes");
	#else
	// Windows 既定の 64 MiB 走脈領域に依る起動
	const auto Run = [&Guarded](const size_t Start, const size_t End, const size_t Tid) -> void {
		// 並列区間状態の設定と担当区間の守備付実行
		IsInParallelRegion = true;
		Guarded(Start, End, Tid);
		IsInParallelRegion = false;
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
			const bool WasInRegion = IsInParallelRegion;
			IsInParallelRegion = true;
			Guarded(Start, End, Tid);
			IsInParallelRegion = WasInRegion;
		}
	}
	// 起動済全担当の合流
	for(std::thread &Worker : Threads) Worker.join();
	#endif
	// 全担当合流後の担当順に依る例外の再送出
	for(const std::exception_ptr &Error : Errors) if(Error) std::rethrow_exception(Error);
	// 終了
	return;
}

/**
 * 新しい走脈での実行関数
 * 深いネストの再帰は走脈領域を使い切る前に，同じ大きさの走脈領域を持つ新しい走脈で続きを実行する
 * 呼出元は完了迄待つ為，処理は呼出元の変数を参照で捕捉して良い（同時に動く走脈は常に１つで，共有の表も競合しない）
 * @param Body 実行する処理（例外は呼出元へ運ぶ）
 */
void Parallel::RunOnFreshStack(const std::function<void()> &Body) {
	// 走脈内で送出された例外の保持領域の準備
	std::exception_ptr Error;
	// 新しい走脈への呼出位置のネスト抑止状態の継承
	const bool IsInRegion = IsInParallelRegion;
	#if !defined(_WIN32)
	// POSIX 走脈の構築と合流
	pthread_attr_t Attributes;
	if(const int InitFailed = pthread_attr_init(&Attributes); InitFailed) {
		throw std::system_error(InitFailed, std::generic_category(), "cannot initialize thread attributes");
	}
	if(const int StackFailed = pthread_attr_setstacksize(&Attributes, WorkerStackSize); StackFailed) {
		if(const int DestroyFailed = pthread_attr_destroy(&Attributes); DestroyFailed) {
			throw std::system_error(DestroyFailed, std::generic_category(), "cannot release thread attributes");
		}
		throw std::system_error(StackFailed, std::generic_category(), "cannot set the thread stack size");
	}
	// 新しい走脈と其の引数の保持領域
	pthread_t Handle {};
	const FreshStackArg Arg { &Body, &Error, IsInRegion };
	const int Failed = pthread_create(&Handle, &Attributes, &Parallel::FreshStackEntry, const_cast<FreshStackArg *>(&Arg));
	// 走脈属性の解放
	const int DestroyFailed = pthread_attr_destroy(&Attributes);
	// 走脈起動失敗の資源不足としての通知
	if(Failed) throw std::system_error(Failed, std::generic_category(), "cannot start a thread for deeply nested source");
	// 起動した走脈の合流
	if(pthread_join(Handle, nullptr)) std::terminate();
	// 属性解放失敗の通知
	if(DestroyFailed) throw std::system_error(DestroyFailed, std::generic_category(), "cannot release thread attributes");
	#else
	// 新しい走脈への並列区間状態の継承と例外の退避
	const auto Run = [&Body, &Error, IsInRegion]() -> void {
		try {
			IsInParallelRegion = IsInRegion;
			Body();
		} catch(...) {
			Error = std::current_exception();
		}
		IsInParallelRegion = false;
	};
	// 標準走脈の完了迄の合流に依る参照捕捉変数の存続
	std::thread(Run).join();
	#endif
	// 新しい走脈で捕捉した例外の再送出
	if(Error) std::rethrow_exception(Error);
	// 終了
	return;
}
