#include "FormatLimitExceeded.hpp"
#include <cstddef>
#include <iterator>
#include <new>
#include <stdexcept>
#include <string_view>
#include <system_error>

/**
 * 例外の原因から失敗理由を決める関数
 * 確保を伴わず，例外の寿命にも依存しない理由を返す
 * 計算量：O(1)
 * @param Error 捕捉した例外
 * @return メモリ・保持領域・実行リソースの不足，又は内部の不具合
 */
FailureReason FailureReason::FromException(const std::exception &Error) {
	const std::system_error *const System = dynamic_cast<const std::system_error *>(&Error);
	// 保持領域の上限と，スレッド等を開始出来ないエラーコードだけをリソース不足と確定する
	const bool IsResource = dynamic_cast<const std::bad_alloc *>(&Error) || dynamic_cast<const std::length_error *>(&Error) ||
	System && (System->code() == std::errc::not_enough_memory || System->code() == std::errc::resource_unavailable_try_again);
	// 原因をリソース不足と確定出来ない例外は不具合として報告する理由の返戻
	return IsResource ?
	FailureReason(ResourceFailure) :
	FailureReason(FormatterDefect, "an internal exception interrupted formatting");
}

/**
 * 表示文言の取得関数
 * 計算量：O(1)
 * @return 理由に対応する表示文言
 */
std::string_view FailureReason::GetText() const {
	// 理由の列挙値を添字に使う為，表示文言は其の値の順に置く
	static constexpr std::string_view Texts[] = {
		{},
		"input exceeds size limit",
		"unsupported encoding",
		"unsupported encoding",
		"Objective-C header",
		"XML document",
		"parsing exceeds the time limit",
		"a stray backslash would end a line",
		"parser initialization failed",
		"nesting exceeds parser limit",
		"HTML-like comments are not supported",
		"parser could not read the source",
		"parser could not read the source",
		"parser could not read the source",
		"comments glued to operators exceed parser limit",
		"non-associative Swift operators need parentheses",
		"unknown Swift operator precedence in an unparenthesized chain",
		"partial formatting would change code the parser could not read",
		"leading U+FEFF text cannot be preserved without a BOM",
		"a line comment absorbed a brace",
		"break syntax",
		"change the meaning of the code",
		"change a directive comment",
		"change a documentation comment",
		"change blank lines",
		"remove a required block body",
		"change code structure",
		{},
		{},
		"estimated working memory exceeds resource limit",
		"out of memory or execution resources"
	};
	static_assert(std::size(Texts) == static_cast<size_t>(ResourceFailure) + 1);
	const size_t Idx = static_cast<size_t>(Val);
	// 例外の詳細と未知の分類の其の儘の返戻
	if(Texts[Idx].empty() && Val != None) return Detail;
	// 定義済の表示文言の返戻
	return Texts[Idx];
}

/**
 * 原文を保持した後の対処の取得関数
 * 計算量：O(1)
 * @return 原因に対応する実行可能な対処
 */
std::string_view FailureReason::GetRemedy() const {
	// 失敗区分毎の対処の定義
	static constexpr std::string_view Remedies[] = {
		{},
		"provide independently valid input within 16 MiB",
		"convert the input to supported UTF-8 before retrying",
		"correct or remove the encoding declaration before retrying",
		"exclude Objective-C sources from the formatting selection",
		"exclude XML documents from the TypeScript formatting selection",
		"retry when system load is lower",
		"correct the stray backslash before retrying",
		"reinstall a matching formatter binary and grammar runtime",
		"reduce the nesting depth before retrying",
		"use the correct JavaScript script or module language goal",
		"parser recognition failed; verify the source syntax and target language version",
		"input syntax error; correct the input syntax before retrying",
		"parser recognition failed; verify the source syntax and target language version",
		"format a smaller independently valid Swift input",
		"parenthesize the non-associative operator chain",
		"provide operator precedence declarations or parenthesize the chain",
		"verify the unrecognized syntax and target language version",
		"remove the leading U+FEFF text before retrying",
		"correct the comment and brace boundary before retrying",
		"report this input and the formatter version for investigation",
		"report this input and the formatter version for investigation",
		"report this input and the formatter version for investigation",
		"report this input and the formatter version for investigation",
		"report this input and the formatter version for investigation",
		"report this input and the formatter version for investigation",
		"report this input and the formatter version for investigation",
		"format smaller independently valid inputs",
		"report this input and the formatter version for investigation",
		"format smaller independently valid inputs",
		"retry after releasing memory or execution resources; report persistent failures with the formatter version"
	};
	static_assert(std::size(Remedies) == static_cast<size_t>(ResourceFailure) + 1);
	// 原因に対応する対処の返戻
	return Remedies[static_cast<size_t>(Val)];
}

/**
 * 検証差戻の理由かの判定関数
 * 計算量：O(1)
 * @return 検証差戻なら true
 */
bool FailureReason::IsValidation() const {
	// 検証差戻の理由の返戻
	return Val == SyntaxBroken || Val == EmptyBlock || Val == SemanticsChanged || Val == DirectiveChanged || Val == DocChanged ||
	Val == BlankLineChanged || Val == StructureChanged;
}

/**
 * リソース上限の理由かの判定関数（整形仕様 3.2，第９章）
 * 計算量：O(1)
 * @return 入力の大きさ・解析の期限・パーサーの上限・処理予算に依る理由なら true
 */
bool FailureReason::IsResourceLimit() const {
	// リソース上限の理由の返戻
	return Val == InputSizeExceeded || Val == ParseTimeExceeded || Val == ParserNestingExceeded || Val == GluedCommentsLimit ||
	Val == LimitExceeded;
}

/**
 * 打切の例外の構築関数
 * 計算量：O(1)
 * @param Text 利用者へ示す理由のテキスト（文法の静的領域か文字列リテラルを渡す）
 * @param Code 理由の種類（上限の超過，発火しては為らない防壁の不具合，又は保持契約の不成立）
 */
FormatLimitExceeded::FormatLimitExceeded(const std::string_view Text, const FailureReason::Kind Code) : Reason(Text),
Kind(Code) {}
