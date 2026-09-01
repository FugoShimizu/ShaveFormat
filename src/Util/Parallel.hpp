#pragma once

#include <cstddef>
#include <exception>
#include <functional>

// 並列実行ユーティリティクラス
class Parallel {
private: // 走脈実装の内部資源

	// POSIX で主走脈と同じ大きさの領域を持つワーカーの引数
	#if !defined(_WIN32) // POSIX 専用のワーカー定義

	struct WorkerArg {
		const std::function<void(size_t, size_t, size_t)> *const Func; // 呼出側の関数オブジェクトを指す（合流迄呼出側フレームが生存する為，所有コピー不要）
		std::exception_ptr *const Error; // 開始関数の境界で捕捉した例外の格納先
		const size_t Start; // チャンク開始位置
		const size_t End; // チャンク終了位置（半開区間の上端）
		const size_t Tid; // ワーカー識別番号
	};

	struct FreshStackArg {
		const std::function<void()> *const Body; // 呼出側が合流迄保持する処理本体
		std::exception_ptr *const Error; // 開始関数の境界で捕捉した例外の格納先
		const bool IsInRegion; // 呼出側の並列区間状態
	};

	static constexpr size_t WorkerStackSize = 0X4000000; // 主走脈と同じワーカー領域 (64 MiB)

	#endif

	static constexpr size_t EarlyThreshold = 16; // 走脈の起動費を上回る最小仕事数
	static thread_local bool IsInParallelRegion; // 並列区間内か否かのスレッド局所フラグ

	#if !defined(_WIN32)

	static void *WorkerEntry(void *const Raw) noexcept; // pthread ワーカーエントリ関数
	static void *FreshStackEntry(void *const Raw) noexcept; // 新しい走脈のエントリ関数

	#endif

public: // 並列実行の公開入口

	using Callback = std::function<void(size_t, size_t, size_t)>; // (Start, End, Tid) を取るワーカー関数型

	static size_t DecideThreads(const size_t Total, const size_t MaxThreads, const size_t Threshold = EarlyThreshold); // 実効スレッド数決定関数（Threshold 未満は逐次実行）
	static void ForChunks(const size_t Total, const size_t NumThreads, const Callback &Callback); // チャンク分割並列実行関数
	static void RunOnFreshStack(const std::function<void()> &Body); // 新しい走脈での実行関数

	Parallel() = delete; // コンストラクタ（禁止）
};
