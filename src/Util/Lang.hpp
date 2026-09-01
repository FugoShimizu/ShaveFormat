#pragma once

#include <tree_sitter/api.h>
#include <string>

// 言語クラス
class Lang {
private:

	static void ToLowerAscii(std::string &Text); // ASCII 小文字化関数
	static std::string GetExtension(const std::string &Path); // 拡張子取得関数

public:

	enum ID { C, Cpp, CSharp, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python, JSON, HTML, CSS, Unknown }; // 言語種別
	static constexpr size_t WideIndentNestingLimit = 32; // 表示幅４を推奨する言語のネストの段数の上限
	static constexpr size_t NarrowIndentNestingLimit = 64; // 表示幅２を推奨する言語のネストの段数の上限
	ID Id; // 自身の言語種別
	bool IsJsx = false; // TypeScript を JSX 付の文法で読むか（`.tsx`，JSX と衝突する型表明 `<T>x`・総称の矢印関数 `<T>(x) => x` は `.ts` の文法で読む）
	bool IsCShared = false; // C++ の文法で読む `.h` が C からも読まれ得るか（C と C++ で意味の同じ書換だけを掛ける）
	bool IsScss = false; // CSS の文法で読む入力が SCSS か（Sass はコメントを空白と読み，CSS のコメントは字句を生まない）
	static Lang Get(const ID Id); // 言語種別生成関数
	static Lang FromName(const std::string_view Name); // 言語名判定関数
	static Lang Detect(const std::string &Path); // パスからの言語判定関数
	static bool IsAmbiguousName(const std::string_view Name); // 複数言語が共有する拡張子の名前かの判定関数
	static bool IsAmbiguousExtension(const std::string &Path); // 複数言語が共有する拡張子かの判定関数
	static bool IsAmbiguousCandidate(const std::string &Path, const Lang Override); // 共有拡張子の候補言語かの判定関数
	const TSLanguage *TsLang() const; // tree-sitter 言語定義取得関数
	bool IsCFamily() const; // C 系判定関数
	bool IsJsTs() const; // JS / TS 判定関数
	bool IsBraceLang() const; // 波括弧言語判定関数
	bool IsIndentSensitive() const; // インデントが構文を成す言語の判定関数
	size_t NestingLimit() const; // ネストの段数の上限の取得関数
	bool IsApostropheAmbiguous() const; // `'` を文字列区切りとして扱えない言語判定関数
	bool JoinsBraceAcrossNewline() const; // 改行の後の `{` を前の式の末尾ラムダとして読む言語の判定関数
	bool operator==(const ID Target) const; // 等価比較演算子（`!=` は C++20 が書換で自動導出）
};
