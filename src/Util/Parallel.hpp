#pragma once

#include <cstddef>
#include <functional>

// 並列実行ユーティリティクラス
class Parallel {
private: // 走脈実装の内部資源

	static constexpr size_t EarlyThreshold = 16; // 走脈の起動費を上回る最小仕事数
	static thread_local bool IsInParallelRegion; // 並列区間内か否かのスレッド局所フラグ

public: // 並列実行の公開入口

	using Callback = std::function<void(size_t, size_t, size_t)>; // (Start, End, Tid) を取るワーカー関数型

	static size_t DecideThreads(const size_t Total, const size_t MaxThreads, const size_t Threshold = EarlyThreshold); // 実効スレッド数決定関数（Threshold 未満は逐次実行）
	static void ForChunks(const size_t Total, const size_t NumThreads, const Callback &Callback); // チャンク分割並列実行関数

	Parallel() = delete; // コンストラクタ（禁止）
};
