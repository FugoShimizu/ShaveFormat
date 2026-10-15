#pragma once

#include "FormatLimitExceeded.hpp"
#include <cstddef>
#include <functional>
#include <span>
#include <string_view>

// 入力・出力上限とは独立した見積作業域の上限（中核の作業単位は変更しない）
inline constexpr size_t MaxWorkingBytes = 0X20000000;

class GrammarProfile;

size_t EstimateWorkingBytes(const GrammarProfile &Profile, const std::string_view Src, size_t *const Work = nullptr); // トークンと本文からの作業域の見積関数

size_t EstimateWorkingBytes(
	const GrammarProfile &Profile,
	const size_t Size,
	const std::function<size_t(size_t, std::span<char>)> &Read,
	size_t *const Work = nullptr
); // 固定長の読出口を使う同じ受理走査の見積関数

FailureReason AcceptInput(const size_t Size, const std::function<size_t()> &Estimate); // 入力の大きさと見積作業域の受理判定関数
