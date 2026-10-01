#pragma once

#include <tree_sitter/api.h>
#include <cstdint>
#include <string>
#include <string_view>

// 言語クラス
class Lang {
private:

	static Lang FromExtension(const std::string &Ext); // 拡張子からの言語判定関数
	static std::string GetExtension(const std::string &Path); // 拡張子取得関数
	static bool IsAmbiguousName(const std::string_view Name); // 複数言語が共有する拡張子の名前かの判定関数
	static bool IsAmbiguousExtension(const std::string &Path); // 複数言語が共有する拡張子かの判定関数

public:

	enum ID { C, Cpp, CSharp, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python, JSON, HTML, CSS, Unknown }; // 言語種別
	enum class SourceGoal : uint8_t { Undetermined, Module, Script }; // JavaScript の原稿を読む目標（古典スクリプトかモジュールか，定まらなければ中身で決める）
	ID Id; // 自身の言語種別
	SourceGoal Goal = SourceGoal::Undetermined; // 拡張子 (`.cjs` / `.mjs`) から定まる読みの目標
	bool IsJsx = false; // TypeScript を JSX 付の文法で読むか（`.tsx`，JSX と衝突する型表明 `<T>x`・総称の矢印関数 `<T>(x) => x` は `.ts` の文法で読む）
	bool IsCShared = false; // C++ の文法で読む `.h` が C からも読まれ得るか（C と C++ で意味の同じ書換だけを掛ける）
	bool IsScss = false; // CSS の文法で読む入力が SCSS か（Sass はコメントを空白と読み，CSS のコメントは字句を生まない）
	bool IsAmbiguous = false; // 拡張子だけでは言語が定まらず中身での判別が要るか（`.h` や経路の無い標準入力）
	bool IsAssumed = false; // 拡張子から言語が定まらず `--lang` の指定だけで決めたか
	static Lang Get(const ID Id); // 言語種別生成関数
	static Lang FromName(const std::string_view Name); // 言語名判定関数
	static Lang Detect(const std::string &Path); // パスからの言語判定関数
	static bool IsAmbiguousCandidate(const std::string &Path, const Lang Override); // 共有拡張子の候補言語かの判定関数
	const TSLanguage *TsLang() const; // tree-sitter 言語定義取得関数
	bool IsCFamily() const; // C 系判定関数
	bool IsJsTs() const; // JS / TS 判定関数
	bool operator==(const ID Target) const; // 等価比較演算子（`!=` は C++20 が書換で自動導出）
};
