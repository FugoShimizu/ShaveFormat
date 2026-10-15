#pragma once

#include "../Profile/UnicodeAlphanumeric.hpp"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

/**
 * Unicode の行内空白（一般カテゴリ Zs）の判定関数
 * 計算量：O(1)
 * @param Point コードポイント
 * @return Zs の字なら true
 */
inline bool IsUnicodeSpace(const char32_t Point) {
	// Unicode の行内空白かの返戻
	return Point == 0X20 || Point == 0XA0 || Point == 0X1680 || Point > 0X1FFF && Point < 0X200B || Point == 0X202F ||
	Point == 0X205F || Point == 0X3000;
}

enum class UnicodeAlnumKind : uint8_t { None, Letter, Number }; // Unicode の英数字の種類 // 英数字でない字 // 英字 // 数字

/**
 * 詰めた Unicode の固定表からコードポイントを含む区間を探す関数
 * 区間は末尾の昇順とし，最上位の種類ビットを除く上位ビットに先頭，下位２１ビットに末尾を持つ
 * 計算量：区間数 R に対し O(log R)，追加メモリ O(1)
 * @param Ranges 照合する固定区間表
 * @param Point 判定するコードポイント
 * @param Work 固定表の照合回数（無ければ計上しない）
 * @return 含む区間へのポインタ，無ければ nullptr
 */
inline const uint64_t *FindUnicodeRange(
	const std::span<const uint64_t> Ranges,
	const char32_t Point,
	size_t *const Work = nullptr
) {
	// 空の表の返戻
	if(Ranges.empty()) return nullptr;
	const uint64_t *const End = Ranges.data() + Ranges.size();
	const uint64_t *const Found = std::lower_bound(
		Ranges.data(),
		End,
		Point,
		[Work](const uint64_t Range, const char32_t Value) -> bool {
			if(Work) ++*Work;
			// 区間の末尾と照会するコードポイントの比較の返戻
			return (Range & 0X1FFFFF) < Value;
		}
	);
	// 末尾を通過して居ない最初の区間に含まれるかの返戻
	return Found == End || ((*Found & 0X7FFFFFFFFFFFFFFF) >> 21) > Point ? nullptr : Found;
}

/**
 * Unicode の英数字の種類の取得関数
 * ラテン文字・其の互換形と各文字種の数字を固定表で読み，仮名・漢字を英字に含めない
 * 計算量：固定区間の数 R に対し O(log R)，追加メモリ O(1)
 * @param Point 判定するコードポイント
 * @param Work 固定表の照合回数（無ければ計上しない）
 * @return 英字・数字・其の他の種類
 */
inline UnicodeAlnumKind GetUnicodeAlnumKind(const char32_t Point, size_t *const Work = nullptr) {
	if(Work) ++*Work;
	if(Point < 0X80) {
		// ASCII の既存の分類の返戻
		return std::isdigit(static_cast<unsigned char>(Point)) ?
		UnicodeAlnumKind::Number :
		std::isalpha(static_cast<unsigned char>(Point)) ? UnicodeAlnumKind::Letter : UnicodeAlnumKind::None;
	}
	const uint64_t *const Found = FindUnicodeRange(Core::UnicodeAlphanumeric::Ranges, Point, Work);
	// 末尾を通過して居ない最初の区間に含まれない事の返戻
	if(!Found) return UnicodeAlnumKind::None;
	// 固定表へ記録した文字・数字の種類の返戻
	return *Found >> 63 ? UnicodeAlnumKind::Number : UnicodeAlnumKind::Letter;
}

/**
 * Unicode の英数字の判定関数
 * 計算量：固定区間の数 R に対し O(log R)，追加メモリ O(1)
 * @param Point 判定するコードポイント
 * @param Work 固定表の照合回数（無ければ計上しない）
 * @return 文字又は数字なら true
 */
inline bool IsUnicodeAlnum(const char32_t Point, size_t *const Work = nullptr) {
	// 共通の文字種が英数字かの返戻
	return GetUnicodeAlnumKind(Point, Work) != UnicodeAlnumKind::None;
}

// 数値トークンの基数と，値を表す部分の終端
struct NumericHead {
	uint32_t Base = 10; // 整数の基数
	size_t End = 0; // 接尾辞又は小数点・指数の開始
};

/**
 * 数値トークンの基数の読取関数
 * 計算量：O(1)
 * @param Text 数値トークン
 * @param HasLeadingZeroOctal 先頭の零が８進を指定するか
 * @return 基数と接頭辞の終端
 */
inline NumericHead ReadNumericPrefix(const std::string_view Text, const bool HasLeadingZeroOctal) {
	NumericHead Head;
	if(Text.size() > 1 && Text[0] == '0') {
		const unsigned char Mark = static_cast<unsigned char>(Text[1] | 0X20);
		Head.Base = Mark == 'x' ? 16 : Mark == 'b' ? 2 : Mark == 'o' ? 8 : 10;
		if(Head.Base != 10) Head.End = 2;
		else if(HasLeadingZeroOctal && std::isdigit(static_cast<unsigned char>(Text[1]))) Head.Base = 8;
	}
	// 基数と接頭辞の返戻
	return Head;
}

/**
 * 数値トークンの整数部の読取関数
 * 桁区切と基数を先に読み，１６進の桁を型の接尾辞と区別する
 * 計算量：整数部のテキスト長 L に対し O(L)
 * @param Text 数値トークン
 * @return 基数と整数部の終端
 */
inline NumericHead ReadNumericHead(const std::string_view Text) {
	NumericHead Head = ReadNumericPrefix(Text, false);
	for(; Head.End < Text.size(); ++Head.End) {
		const unsigned char Char = Text[Head.End];
		if(Char == '_' || Char == '\'') continue;
		const int Lower = std::tolower(Char);
		const uint32_t Digit = std::isdigit(Char) ?
		static_cast<uint32_t>(Char - '0') :
		Lower >= 'a' && Lower <= 'f' ? static_cast<uint32_t>(Lower + 10 - 'a') : Head.Base;
		if(Digit >= Head.Base) break;
	}
	// 型の接尾辞を含めない整数部の返戻
	return Head;
}

/**
 * 識別子構成文字の判定関数
 * 計算量：O(1)
 * @param Char 判定対象の文字
 * @return 英数字又は下線なら true
 */
inline bool IsIdentifierChar(const unsigned char Char) {
	// 識別子を構成し得る文字かの返戻
	return std::isalnum(Char) || Char == '_';
}

/**
 * 語を構成する文字の判定関数
 * 計算量：O(1)
 * @param Char 判定対象の文字
 * @return 英数字・下線・`$`・UTF-8 の多バイト文字のバイトなら true
 */
inline bool IsWordChar(const unsigned char Char) {
	// 隣り合うと一続きの語に為る文字かの返戻
	return IsIdentifierChar(Char) || Char == '$' || Char > 0X7F;
}

/**
 * 閉括弧文字の判定関数
 * 計算量：O(1)
 * @param Char 判定対象の文字
 * @return 閉括弧文字 (`)` `]` `}`) なら true
 */
inline bool IsCloseBracketChar(const unsigned char Char) {
	// 閉括弧の判定の返戻
	return Char == ')' || Char == ']' || Char == '}';
}

/**
 * 開括弧文字の判定関数
 * 計算量：O(1)
 * @param Char 判定対象の文字
 * @return 開括弧文字 (`(` `[` `{`) なら true
 */
inline bool IsOpenBracketChar(const unsigned char Char) {
	// 開括弧の判定の返戻
	return Char == '(' || Char == '[' || Char == '{';
}

/**
 * 丸括弧で囲んだテキストかの判定関数
 * 計算量：O(1)
 * @param Text テキスト
 * @return `(` で始まり `)` で終わる２字以上のテキストなら true
 */
inline bool IsParenthesized(const std::string_view Text) {
	// 丸括弧の対で囲んだかの返戻
	return Text.size() > 1 && Text.front() == '(' && Text.back() == ')';
}

/**
 * 並びの区切文字の判定関数
 * 計算量：O(1)
 * @param Char 判定対象の文字
 * @return カンマ又はセミコロンなら true
 */
inline bool IsListSeparatorChar(const unsigned char Char) {
	// 並びの区切の判定の返戻
	return Char == ',' || Char == ';';
}

/**
 * 括弧の対の字の取得関数
 * 計算量：O(1)
 * @param Char 開き又は閉じの括弧
 * @return 対の字（括弧でなければ零文字）
 */
inline unsigned char MatchingBracketChar(const unsigned char Char) {
	// 対に為る括弧の返戻
	return Char == '(' ?
	')' :
	Char == ')' ? '(' : Char == '[' ? ']' : Char == ']' ? '[' : Char == '{' ? '}' : Char == '}' ? '{' : '\0';
}

/**
 * 演算子を成す記号の判定関数（記号同士が接すると１トークンへ繋がり，別の演算子に読まれる）
 * 計算量：O(1)
 * @param Char 文字
 * @return 演算子を成す記号なら true
 */
inline bool IsOperatorChar(const unsigned char Char) {
	// 演算子を成す記号かの返戻
	return std::string_view("!%&*+-/:<=>?^|~").find(Char) != std::string_view::npos;
}

/**
 * ASCII 小文字化関数（大小文字を問わない照合の共通前処理，其の場変換）
 * 計算量：テキスト長 L に対し O(L)
 * @param Text 変換対象の文字列
 */
inline void ToLowerAscii(std::string &Text) {
	// ASCII の大文字だけを変換し，其の他のバイトは保持
	for(char &Char : Text) if(Char >= 'A' && Char <= 'Z') Char += 'a' - 'A';
	// 終了
	return;
}
