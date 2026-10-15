#pragma once

#include <cstddef>
#include <functional>

// 並列実行ユーティリティクラス
class Parallel {
private: // スレッド実装の内部リソース

	static constexpr size_t EarlyThreshold = 16; // スレッドの起動費を上回る最小仕事数
	static thread_local bool IsInParallelRegion; // 並列区間内か否かのスレッドローカルフラグ

public: // 並列実行の公開入口

	using Callback = std::function<void(size_t, size_t, size_t)>; // 開始・終端・スレッド番号を取る作業関数型

	static size_t DecideThreads(const size_t Total, const size_t MaxThreads, const size_t Threshold = EarlyThreshold); // 実効スレッド数決定関数（Threshold 未満は逐次実行）
	static void ForChunks(const size_t Total, const size_t ThreadCount, const Callback &Callback); // チャンク分割並列実行関数

	Parallel() = delete; // コンストラクタ（禁止）
};
