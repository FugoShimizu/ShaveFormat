#include "TextMetrics.hpp"
#include <cstddef>
#include <string_view>

/**
 * 一般カテゴリ `Cc` の制御文字を除くコードポイントの計数関数（行分割仕様 3.2）
 * @param Text 対象テキスト
 * @return 制御文字を除く文字数
 */
size_t TextMetrics::CountCharacters(const std::string_view Text) {
	// 可視 ASCII と UTF-8 先頭だけの分岐無の計数
	size_t Chars = 0;
	const unsigned char *const Data = reinterpret_cast<const unsigned char *>(Text.data());
	const size_t Size = Text.size();
	for(size_t Idx = 0; Idx < Size; ++Idx) {
		const unsigned char Byte = Data[Idx];
		// C1 の制御文字 (U+0080~U+009F) は `0XC2` の後の `0X80`~`0X9F` で表し，`Cc` に属する為に数えない
		Chars += static_cast<size_t>(Byte > 0X1F && Byte < 0X7F) +
		static_cast<size_t>(Byte > 0XBF && !(Byte == 0XC2 && Idx + 1 < Size && Data[Idx + 1] < 0XA0));
	}
	// 制御文字を除く文字数の返戻
	return Chars;
}
