#pragma once

#include <string_view>

// 投影済の役割へ適用する静的解析の語彙と詳細順位の固定登録
struct LintRules {
	static constexpr std::string_view FloatTypeSuffixes[] { "f32", "f64" }; // 小数部を持たず浮動小数に為る型接尾辞
	static constexpr std::string_view MacroFloatSuffixes = "FLfl"; // マクロの浮動小数接尾辞

	static constexpr std::string_view MacroIntegerSuffixes[] {
		"L",
		"LL",
		"LLU",
		"LLu",
		"LU",
		"Lu",
		"U",
		"UL",
		"ULL",
		"UZ",
		"Ul",
		"Ull",
		"Uz",
		"Z",
		"ZU",
		"Zu",
		"l",
		"lU",
		"ll",
		"llU",
		"llu",
		"lu",
		"u",
		"uL",
		"uLL",
		"uZ",
		"ul",
		"ull",
		"uz",
		"z",
		"zU",
		"zu"
	}; // マクロの整数接尾辞

	static constexpr std::string_view ExplicitVirtualWords[] { "final", "override" }; // 明示した仮想メンバの指定
	static constexpr std::string_view ConstQualifier = "const"; // 読取専用の型修飾
	static constexpr std::string_view VolatileQualifier = "volatile"; // 揮発する型修飾
	static constexpr std::string_view StaticStorageWords[] { "static", "thread_local" }; // ローカル変数の静的寿命の指定
	static constexpr std::string_view SetterWord = "set"; // 戻値型を書かないアクセサ
	static constexpr std::string_view NullWord = "null"; // ヌリッシュ比較の欠如値
	static constexpr std::string_view UndefinedWord = "undefined"; // ヌリッシュ比較の未定義値
	static constexpr std::string_view ExclusiveOrWords[] { "^", "xor" }; // 真偽値の反転として検査する排他的論理和

	static constexpr std::string_view CastBuiltinWords[] {
		"bool",
		"char",
		"char16_t",
		"char32_t",
		"char8_t",
		"double",
		"float",
		"int",
		"long",
		"short",
		"signed",
		"unsigned",
		"void",
		"wchar_t"
	}; // 明示キャストの警告対象に為る組込型

	static constexpr std::string_view InvalidInstanceStateWords[] { "constexpr", "inline" }; // 非静的フィールドで許されない指定
};
