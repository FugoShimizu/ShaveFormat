#pragma once

#include "Lang.hpp"
#include "TsSource.hpp"
#include <span>
#include <string>
#include <vector>

// レイアウト工程後のテキスト修正を集約するクラス
class Postprocess {
private:

	static bool IsValidUtf8(const std::string_view Text); // UTF-8 妥当性判定関数
	static bool IsQuoteDepthChar(const std::string_view Text, const size_t Pos, size_t &Depth); // 引用括弧の深度更新関数

	static size_t FindSentencePeriod(
		const std::string_view Text,
		const size_t From,
		const size_t Limit,
		size_t &Depth,
		const std::span<const unsigned char> Protected
	); // 引用外の句点探索関数

	static bool IsTagOrDirectiveLine(const std::string_view Line); // 文書化の札か次の行に作用する指令の行かの判定関数
	static bool IsCommentDirective(const std::string_view Text, const size_t Body); // 行コメントの指令の判定関数

	static void BreakAtPeriods(
		std::string &Text,
		const std::span<const size_t> Periods,
		const std::span<const uint8_t> Enclosed,
		const bool AllowsSplit,
		std::vector<unsigned char> &Protected
	); // 句点の分割適用関数

	static void NormalizeCommentSpaces(std::string &Text, std::vector<unsigned char> &Protected); // コメント本文の空白の正規化関数
	static void NormalizeProtectedComment(std::string &Text, const bool AllowsSplit, std::vector<unsigned char> Protected); // 保護範囲付のコメント正規化関数
	static size_t EncodingDeclarationLine(const std::string_view Text, const Lang Language); // 効く位置に在る符号化宣言の行の取得関数

public:

	static void InsertCharAt(TSSource &Src, std::vector<uint32_t> &Insertions, const char Char); // 収集済位置への１文字挿入と再構築関数
	static void FixCssMissingSemicolon(TSSource &Src); // CSS の最終宣言へのセミコロン補完
	static void FixHtmlSelfClosing(TSSource &Src); // HTML の空要素からの自己終端記号の除去
	static void FixGoTrailingComma(TSSource &Src); // Go の複数行コンテナ末尾カンマ補修
	static void FixJsxEdgeSpaces(TSSource &Src); // JSX の本文の端の空白の表記確定

	static std::vector<unsigned char> CommentCodeMask(
		const std::string_view Text,
		const bool HasMarkers = true,
		bool *const IsUnclosed = nullptr
	); // コメント内コードの保護範囲取得関数

	static std::string LineCommentToBlock(const std::string_view Text); // 行コメントのブロックコメント化関数
	static void NormalizeCommentGroup(const std::span<CommentAttach *> Comments); // 連続コメント群の正規化関数
	static bool CanJoinComments(const std::string_view First, const std::string_view Next, const std::string_view Gap); // 同じ文書群の隣接行コメント判定関数
	static void TrimTrailingWhitespace(TSSource &Src, const Lang Language); // 行末空白の除去
	static void KeepEncodingDeclaration(TSSource &Src, const std::string_view Source, const Lang Language); // 符号化宣言の効き目の保持関数
	static void ReattachOuterText(std::string &Output, const std::string &Lead, const std::string &Tail); // 構文木の外の本文の再結合関数
	static void RestoreTailNewlines(std::string &Result, const size_t Count, const std::string &MixedTail); // 末尾の改行の復元関数

	Postprocess() = delete; // コンストラクタ（禁止）
};
