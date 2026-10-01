#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class ParsedTree;

struct ReadingRules;

// テキスト編集構造体
struct TextEdit {
	uint32_t StartPos; // 置換範囲の開始バイト
	uint32_t EndPos; // 置換範囲の終端バイト（半開区間）
	std::string NewText; // 所有する置換後テキスト
	static bool DecodeUtf8(const std::string_view Text, size_t &Pos, uint32_t &Point); // UTF-8 の１符号点復号関数
	static bool IsValidUtf8(const std::string_view Text); // UTF-8 妥当性判定関数
	static void EncodeUtf8(const uint32_t Point, std::string &Dest); // UTF-8 の１符号点符号化関数
	static char AbsentControlChar(const std::string_view Text); // 原文に無い制御文字の選択関数
	static void Push(const uint32_t Start, const uint32_t End, std::string Text, std::vector<TextEdit> &Edits); // 編集追加関数

	static uint32_t SkipCharsLeftBounded(
		const std::string_view Src,
		uint32_t Pos,
		const uint32_t Lower,
		const std::string_view TrimChars
	); // 文字集合と下限境界を指定する左方向スキップ関数

	static size_t SpaceBefore(const std::string_view Src, const uint32_t Pos, const ReadingRules &Reading); // 位置で終わる行内空白の長さの取得関数
	static uint32_t SkipSpLeft(const std::string_view Src, const uint32_t Pos, const ReadingRules &Reading); // 左方向の空白スキップ関数
	static uint32_t EmptyBodyOpener(const std::string_view Src, const uint32_t End, const ReadingRules &Reading); // 空の本体を開く位置の取得関数
	static bool EndsWithEmptyBody(const std::string_view Src, const uint32_t End, const ReadingRules &Reading); // 空の本体で終わるかの判定関数
	static bool IsAfterCode(const std::string_view Src, const uint32_t Pos, const ReadingRules &Reading); // 同じ行の前に空白以外の字が在るかの判定関数
	static size_t SpaceAt(const std::string_view Src, const uint32_t Pos, const ReadingRules &Reading); // 位置から始まる行内空白の長さの取得関数
	static size_t IntertokenSpaceAt(const std::string_view Src, const uint32_t Pos, const ReadingRules &Reading); // 字句間の空白又は改行の長さの取得関数
	static uint32_t SkipSpRight(const std::string_view Src, uint32_t Pos, const ReadingRules &Reading); // 右方向の空白スキップ関数
	static std::pair<uint32_t, size_t> SeparatorBefore(const std::string_view Src, uint32_t Pos, const ReadingRules &Reading); // 前置区切りの範囲とタブの数の取得関数
	static uint32_t LineStartOf(const std::string_view Src, uint32_t Pos); // Pos を含む行の行頭位置の取得関数
	static void TrimView(std::string_view &View); // 前後の空白／タブ／改行の切詰関数

	static void Apply(
		ParsedTree &Src,
		std::vector<TextEdit> &Edits,
		const bool TracksParserBreaks = false,
		const std::vector<std::pair<uint32_t, uint32_t>> *const NewGroups = nullptr
	); // 編集適用関数
};
