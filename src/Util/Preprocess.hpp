#pragma once

#include "Lang.hpp"
#include "TextEdit.hpp"
#include "TsSource.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 中核工程の前に原文とコメントを整えるクラス
class Preprocess {
private:

	static std::vector<std::pair<uint32_t, uint32_t>> CollectLiteralRanges(
		const std::string &Source,
		const Lang Language,
		const bool ShouldIncludeBlockComments
	); // 改行を値其の物として保つ範囲の収集関数

	static bool NormalizeMixedNewlines(std::string &Source, const Lang Language); // 改行様式が混在する入力の正規化関数

	static void NormalizeEmbeddedRawText(
		const std::string_view Raw,
		const uint32_t BaseStart,
		const TSLanguage *const SubLang,
		std::vector<TextEdit> &Edits
	); // 埋込生テキストのコメント正規化関数

public: // 前処理の公開入口

	static size_t CountTailNewlines(const std::string_view Source); // 末尾の改行の数の取得関数
	static bool NormalizeNewlines(std::string &Source, const Lang Language, std::string &MixedTail); // 改行コードと末尾改行の正規化関数
	static void PrepareHtmlComments(TSSource &Src); // HTML のコメント準備関数
	static bool ExpandJavaUnicodeEscapes(const TSSource &Src, std::string &Out); // Java の字句化前 Unicode エスケープの展開関数
	static std::string EscapeRubyEmbeddedDocs(const TSSource &Src, char &Marker); // Ruby の埋込ドキュメントの誤読の回避関数
	static bool LooksObjectiveC(const std::string &Source); // Objective-C 判定関数
	static bool LooksXmlDocument(const std::string &Source); // XML 文書判定関数
	static std::string_view HtmlSkipReason(const std::string &Source); // HTML を解析する前に見送る理由の判定関数（追えないネスト・閉じないコメント）
	static std::string_view CommentRunSkipReason(const std::string &Source, const Lang Language); // コメントの並びで解析する前に見送る理由の判定関数 (Python / JS / TS / Kotlin / PHP)
	static bool IsPhpHaltCall(const TSSource &Src, const TSNode Stmt); // PHP の実行を止める `__halt_compiler()` の文かの判定関数
	static size_t PhpHaltDataStart(const std::string &Source); // PHP の実行停止後のデータの開始位置の取得関数

	static bool DetachOuterText(
		TSSource &Src,
		const Lang Language,
		std::string &Lead,
		std::string &Tail,
		std::string &Body,
		bool &IsDetached
	); // 構文木の外の本文の切離関数（末尾が出力其の物かを返す）

	Preprocess() = delete; // コンストラクタ（禁止）
};
