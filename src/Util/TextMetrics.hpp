#pragma once

#include <cstdint>
#include <string_view>

// トークンの文字数の計数クラス（整形仕様 3.1，状態を持たず何も変更しない）
class TextMetrics {
public:

	// トークンの物理行の幅要約
	struct LineSummary {
		uint32_t Head = 0; // 先頭行幅
		uint32_t Tail = 0; // 末尾行幅
		uint32_t Max = 0; // 最大行幅
		bool HasNewline = false; // 改行の有無
	};

	static LineSummary MeasureLines(const std::string_view Text); // 改行と一般カテゴリ `Cc` を除く行幅の計数関数

	TextMetrics() = delete; // コンストラクタ（禁止）
};
