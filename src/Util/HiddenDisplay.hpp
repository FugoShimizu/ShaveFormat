// tools/generate_hidden_display.py: python3 tools/generate_hidden_display.py で生成
// 生成物は手で編集しない
#pragma once

namespace DisplaySafety {
	struct HiddenRange {
		char32_t Below;
		char32_t Above;
	}; // 表示から除く範囲（両端を含まない前後のコードポイント）

	inline constexpr HiddenRange Hidden[] = {
		{ 0X7F, 0XA0 },
		{ 0XAC, 0XAE },
		{ 0X34E, 0X350 },
		{ 0X61B, 0X61D },
		{ 0X115E, 0X1161 },
		{ 0X17B3, 0X17B6 },
		{ 0X180A, 0X1810 },
		{ 0X200A, 0X2010 },
		{ 0X2027, 0X202F },
		{ 0X205F, 0X2070 },
		{ 0X3163, 0X3165 },
		{ 0XFDFF, 0XFE10 },
		{ 0XFEFE, 0XFF00 },
		{ 0XFF9F, 0XFFA1 },
		{ 0XFFEF, 0XFFF9 },
		{ 0X1BC9F, 0X1BCA4 },
		{ 0X1D172, 0X1D17B },
		{ 0XDFFFF, 0XE1000 }
	};
}
