#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

/**
 * 識別子構成文字の判定関数
 * @param Char 判定対象の文字
 * @return 英数字又は下線なら true
 */
inline bool IsIdentifierChar(const char Char) {
	// 識別子を構成し得る文字かの返戻
	return std::isalnum(static_cast<unsigned char>(Char)) || Char == '_';
}

/**
 * 語を構成する文字の判定関数
 * @param Char 判定対象の文字
 * @return 英数字・下線・`$`・UTF-8 の多バイト文字のバイトなら true
 */
inline bool IsWordChar(const char Char) {
	// 隣り合うと一続きの語に為る文字かの返戻
	return IsIdentifierChar(Char) || Char == '$' || static_cast<unsigned char>(Char) > 0X7F;
}

/**
 * 閉じ括弧文字の判定関数
 * @param Char 判定対象の文字
 * @return 閉じ括弧文字 (`)` `]` `}`) なら true
 */
inline bool IsCloseBracketChar(const char Char) {
	// 閉じ括弧の判定の返戻
	return Char == ')' || Char == ']' || Char == '}';
}

/**
 * 開き括弧文字の判定関数
 * @param Char 判定対象の文字
 * @return 開き括弧文字 (`(` `[` `{`) なら true
 */
inline bool IsOpenBracketChar(const char Char) {
	// 開き括弧の判定の返戻
	return Char == '(' || Char == '[' || Char == '{';
}

/**
 * 丸括弧で囲んだ字面かの判定関数
 * @param Text 字面
 * @return `(` で始まり `)` で終わる２字以上の字面なら true
 */
inline bool IsParenthesized(const std::string_view Text) {
	// 丸括弧の対で囲んだかの返戻
	return Text.size() > 1 && Text.front() == '(' && Text.back() == ')';
}

/**
 * 並びの区切り文字の判定関数
 * @param Char 判定対象の文字
 * @return カンマ又はセミコロンなら true
 */
inline bool IsListSeparatorChar(const char Char) {
	// 並びの区切りの判定の返戻
	return Char == ',' || Char == ';';
}

/**
 * 括弧の対の字の取得関数
 * 計算量：O(1)
 * @param Char 開き又は閉じの括弧
 * @return 対の字（括弧でなければ零文字）
 */
inline char MatchingBracketChar(const char Char) {
	// 対になる括弧の返戻
	return Char == '(' ?
	')' :
	Char == ')' ? '(' : Char == '[' ? ']' : Char == ']' ? '[' : Char == '{' ? '}' : Char == '}' ? '{' : '\0';
}

/**
 * 演算子を成す記号の判定関数（記号同士が接すると１字句へ繋がり，別の演算子に読まれる）
 * @param Char 文字
 * @return 演算子を成す記号なら true
 */
inline bool IsOperatorChar(const char Char) {
	// 演算子を成す記号かの返戻
	return std::string_view("!%&*+-/:<=>?^|~").find(Char) != std::string_view::npos;
}

/**
 * ASCII 小文字化関数（大小文字を問わない照合の共通前処理，其の場変換）
 * @param Text 変換対象の文字列
 */
inline void ToLowerAscii(std::string &Text) {
	// 負値に為る非 ASCII バイトを `tolower` へ直接渡すと未定義動作の為，符号無へ広げてから渡す
	std::transform(
		Text.begin(),
		Text.end(),
		Text.begin(),
		[](const char Char) -> char {
			// 小文字化した文字の返戻
			return static_cast<char>(std::tolower(static_cast<unsigned char>(Char)));
		}
	);
	// 終了
	return;
}
