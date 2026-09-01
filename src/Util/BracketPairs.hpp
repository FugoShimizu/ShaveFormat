#pragma once

#include "Lang.hpp"
#include <tree_sitter/api.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class TSSource;

// 括弧ペア対応構築クラス
class BracketPairs {
public:

	static void CollectOpaqueRanges(const TSSource &Src, const Lang Language, std::vector<std::pair<uint32_t, uint32_t>> &Ranges); // 括弧計数から除外する不透明リテラル範囲の収集関数

	static void Build(
		const std::string &Source,
		const Lang Language,
		const std::vector<std::pair<uint32_t, uint32_t>> &OpaqueRanges,
		std::unordered_map<uint32_t, uint32_t> &CloseOf,
		std::unordered_map<uint32_t, uint32_t> &OpenOf
	); // 括弧ペア対応の構築関数

	static void AppendBracketDelta(
		std::vector<std::pair<uint32_t, int>> &Brackets,
		const std::string &Source,
		const uint32_t Start,
		const uint32_t End
	); // 括弧トークンの深さ増減追記関数

	static void AddJsx(
		const std::string &Source,
		const TSNode Node,
		std::unordered_map<uint32_t, uint32_t> &CloseOf,
		std::unordered_map<uint32_t, uint32_t> &OpenOf
	); // JSX 仮想括弧対の追加関数

	static void AddPhpColonBlocks(
		const std::string &Source,
		const TSNode Node,
		std::unordered_map<uint32_t, uint32_t> &CloseOf,
		std::unordered_map<uint32_t, uint32_t> &OpenOf
	); // PHP の代替構文の本体の仮想括弧対の追加関数

	BracketPairs() = delete; // コンストラクタ（禁止）
};
