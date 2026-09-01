#pragma once

#include "Lang.hpp"
#include "TsSource.hpp"
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ドキュメントコメントのシグネチャ整合を司るクラス
class DocSig {
private:

	static void SkipSpace(std::string_view &View); // 先頭空白の読飛し関数
	static std::string StripBlockLine(const std::string_view Raw, const std::span<const unsigned char> Protected); // ブロック行の整形関数
	static TSNode NextDeclarator(const TSNode Node); // C/C++ の次の宣言子取得関数
	static bool HasBodyValueReturn(const TSNode Body, const Lang Language); // 本体の値返却有無の判定関数
	static void SkipBracketed(std::string_view &View, const char Open, const char Close); // 括弧範囲の読飛し関数
	static size_t TokenLength(const std::string_view View); // 先頭トークン長の取得関数
	static bool ConsumeParamHeader(std::string_view &View, const Lang Language); // `@param` 見出の消費関数
	static bool ConsumeReturnWord(std::string_view &View); // 戻値語の消費関数
	static bool ConsumeReturnHeader(std::string_view &View); // `@return` 見出の消費関数

public:

	enum class Style { Block, Hash }; // ドキュメントコメント記法スタイル（Block=`/** */`, Hash=`#` 行コメント）

	// 関数シグネチャ抽出結果（引数名列・戻値有無・無名引数の有無）
	struct Info {
		std::vector<std::string> Params;
		bool HasReturn = false;
		bool HasUnnamedParam = false; // C/C++ の無名引数（`const T &` 等）の有無
		bool ReturnsFromSignature = false; // 戻値型注釈からの HasReturn 確定導出か（C/C++/Java/Kotlin/TS は型注釈有，false は本体推論依存で信頼性が低い）
	};

	static bool IsTargetLanguage(const Lang Language); // 対象言語の判定関数
	static TSNode DocAnchor(const TSNode FuncNode, const Lang Language); // ドキュメント挿入位置アンカーの取得関数
	static Style StyleOf(const Lang Language); // 言語の記法スタイルの取得関数
	static std::string_view ReturnTag(const Lang Language); // 言語の戻値タグの取得関数
	static bool IsDocBlock(const std::string_view Text); // 文書化コメントブロックの判定関数（`/**`/`/*!` 開始且つ `====` 区切り線を含まない）
	static bool IsNextLineDirective(const std::string_view Text); // 次行へ作用する指令コメント判定関数（文書化コメントを其の手前へ置く為）
	static bool IsMagicComment(const std::string_view Text); // 処理系が読むシバン・マジックコメントの判定関数（文書化の本文へ取り込まない為）
	static std::string StripHashLine(const std::string_view Raw); // `#` 行の整形関数
	static std::vector<std::string> BlockLogicalLines(const std::string_view BlockText); // ブロックの論理行分解関数
	static bool ReturnsPointerOrReference(const TSNode FuncNode); // C / C++ のポインタ／参照返戻判定関数
	static Info Extract(const TSSource &Src, const TSNode FuncNode, const Lang Language); // シグネチャの抽出関数
	static bool HasTagLineDescription(const std::string_view LogicalLine, const Lang Language); // タグ行の説明有無の判定関数
	static std::vector<bool> CodeLineStarts(const std::vector<std::string> &Lines, bool *const Unclosed = nullptr); // 論理行の本文開始がコード内かの判定関数
	static std::string_view ParamTagName(const std::string_view LogicalLine, const Lang Language); // `@param` の引数名取得関数
	static bool IsReturnTagLine(const std::string_view LogicalLine); // 戻値タグ行の判定関数

	static std::vector<std::string> ReconcileLogicalLines(
		const std::vector<std::string> &Existing,
		const Info &Sig,
		const std::string_view ReturnTag,
		const Lang Language
	); // 既存論理行とシグネチャからの論理行整合関数

	DocSig() = delete; // コンストラクタ（禁止）
};
