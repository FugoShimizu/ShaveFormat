#pragma once

#include <cstddef>
#include <string_view>

// 字句の文字数の計数クラス（整形仕様 3.1，状態を持たず何も変更しない）
class TextMetrics {
public:

	static size_t CountCharacters(const std::string_view Text); // 一般カテゴリ `Cc` の制御文字を除く文字数の計数関数

	TextMetrics() = delete; // コンストラクタ（禁止）
};
