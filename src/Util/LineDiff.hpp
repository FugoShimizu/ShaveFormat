#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

// 最長共通部分列に含まれない原文又は候補の行（本文は呼出側が所有）
struct LineChange {
	bool IsInsertion; // 候補へ挿入した行なら true
	size_t Line; // 削除は原文，挿入は候補の零基準行番号
	std::string_view Text; // LF を含む原文又は候補の行
};

std::vector<LineChange> CompareLines(const std::string_view Original, const std::string_view Candidate); // 最長共通部分列の差分の取得関数
