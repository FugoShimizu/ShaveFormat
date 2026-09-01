#pragma once

#include "TsSource.hpp"
#include <string>
#include <string_view>
#include <vector>

// テキスト編集構造体
struct TextEdit {
	uint32_t StartPos; // 置換範囲の開始バイト
	uint32_t EndPos; // 置換範囲の終端バイト（半開区間）
	std::string NewText; // 所有する置換後テキスト
	static bool DecodeUtf8(const std::string_view Text, size_t &Pos, uint32_t &Point); // UTF-8 の１符号点復号関数
	static void EncodeUtf8(std::string &Dest, const uint32_t Point); // UTF-8 の１符号点符号化関数
	static char AbsentControlChar(const std::string_view Text); // 原文に無い制御文字の選択関数
	static void Push(const uint32_t Start, const uint32_t End, std::string Text, std::vector<TextEdit> &Edits); // 編集追加関数

	static uint32_t SkipCharsLeftBounded(
		const std::string_view Src,
		uint32_t Pos,
		const uint32_t Lower,
		const std::string_view TrimChars
	); // 文字集合と下限境界を指定する左方向スキップ関数

	static uint32_t SkipSpLeft(const std::string_view Src, const uint32_t Pos); // 左方向の空白スキップ関数
	static uint32_t SkipSpRight(const std::string_view Src, uint32_t Pos); // 右方向の空白スキップ関数
	static uint32_t LineStartOf(const std::string_view Src, uint32_t Pos); // Pos を含む行の行頭位置の取得関数
	static void TrimView(std::string_view &View); // 前後の空白／タブ／改行の切詰関数
	static size_t ContentChars(const std::string_view Text); // 可視文字数の計数関数
	static void Apply(TSSource &Src, std::vector<TextEdit> &Edits); // 編集適用関数
};
