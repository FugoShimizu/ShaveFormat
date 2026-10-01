#include "FormatLimitExceeded.hpp"
#include <cstddef>
#include <iterator>

/**
 * 表示文言の取得関数
 * @return 理由に対応する表示文言
 */
std::string_view FailureReason::Text() const {
	// 理由の列挙値を添字に使う為，表示文言は其の値の順に置く
	static constexpr std::string_view Texts[] = {
		{},
		"unsupported language",
		"input exceeds size limit",
		"unsupported encoding",
		"Objective-C header",
		"XML document",
		"parsing exceeds the time limit",
		"a stray backslash would end a line",
		"parser initialization failed",
		"nesting exceeds parser limit",
		"HTML-like comments are not supported",
		"parser could not read the source",
		"comments glued to operators exceed parser limit",
		"non-associative Swift operators need parentheses",
		"unknown Swift operator precedence in an unparenthesized chain",
		"--lang does not match the file content",
		"partial formatting would change code the parser could not read",
		"source observation cannot be preserved",
		"a line comment absorbed a brace",
		"break syntax",
		"change the meaning of the code",
		"change a directive comment",
		"change a documentation comment",
		"change blank lines",
		"break syntax",
		"change code structure",
		{},
		{},
		"out of memory or threads, or an internal error"
	};
	static_assert(std::size(Texts) == static_cast<size_t>(ResourceFailure) + 1);
	const size_t Index = static_cast<size_t>(Value);
	// 例外の詳細と未知の分類の其の儘の返戻
	if(Index >= std::size(Texts) || Texts[Index].empty() && Value != None) return Detail;
	// 定義済の表示文言の返戻
	return Texts[Index];
}

/**
 * 検証差戻の理由かの判定関数
 * @return 検証差戻なら true
 */
bool FailureReason::IsValidation() const {
	// 検証差戻の理由の返戻
	return Value == SyntaxBroken || Value == EmptyBlock || Value == SemanticsChanged || Value == DirectiveChanged ||
	Value == DocumentChanged || Value == BlankLineChanged || Value == StructureChanged;
}

/**
 * 資源上限の理由かの判定関数（整形仕様 3.2，第９章）
 * @return 入力の大きさ・解析の期限・解析器の上限・処理予算に依る理由なら true
 */
bool FailureReason::IsResourceLimit() const {
	// 資源上限の理由の返戻
	return Value == InputSizeExceeded || Value == ParseTimeExceeded || Value == ParserNestingExceeded ||
	Value == GluedCommentsLimit || Value == LimitExceeded;
}

/**
 * 打切の例外の構築関数
 * @param Text 利用者へ示す理由の字面（文法の静的領域か文字列リテラルを渡す）
 * @param Code 理由の種類（上限の超過，発火してはならない防壁の不具合，又は保持契約の不成立）
 */
FormatLimitExceeded::FormatLimitExceeded(const std::string_view Text, const FailureReason::Kind Code) : Reason(Text),
Kind(Code) {}
