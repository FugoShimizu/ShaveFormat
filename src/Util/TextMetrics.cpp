#include "TextMetrics.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

/**
 * 改行と一般カテゴリ `Cc` を除く行幅の計数関数
 * 改行の認識とコードポイントの計数は１度のバイト走査で行う
 * 計算量：テキストのバイト数 B に対し O(B)
 * @param Text 対象の UTF-8 テキスト
 * @return 先頭行・末尾行・最大行の幅と改行の有無
 */
TextMetrics::LineSummary TextMetrics::MeasureLines(const std::string_view Text) {
	// 可視のコードポイントと物理行の幅の計数
	TextMetrics::LineSummary Summary;
	uint32_t Width = 0;
	bool IsControlPrefix = false;
	for(size_t Idx = 0; Idx < Text.size(); ++Idx) {
		const unsigned char Byte = static_cast<unsigned char>(Text[Idx]);
		if(Byte == '\n') {
			if(!Summary.HasNewline) Summary.Head = Width;
			Summary.Max = std::max(Summary.Max, Width);
			Summary.HasNewline = true;
			Width = 0;
		} else {
			Width += static_cast<uint32_t>(Byte > 0X1F && Byte < 0X7F || Byte > 0XBF);
			// U+0080〜U+009F の継続バイトで，直前に数えた先頭バイトの１文字を除く
			if(IsControlPrefix && Byte < 0XA0) --Width;
		}
		IsControlPrefix = Byte == 0XC2;
	}
	if(!Summary.HasNewline) Summary.Head = Width;
	Summary.Tail = Width;
	Summary.Max = std::max(Summary.Max, Width);
	// トークンの物理行の幅要約の返戻
	return Summary;
}
