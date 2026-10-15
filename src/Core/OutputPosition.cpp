#include "OutputPosition.hpp"
#include <algorithm>
#include <cstdint>

namespace Core {
	/**
	 * バイト位置から行番号と行内のバイト位置を取得する関数
	 * 先頭の０を含む行頭位置の配列を登録した後に使用する
	 * 計算量：行頭の数 L に対し O(log(L + 1))
	 * @param Offset 最終テキストのバイト位置（テキストの終端以下）
	 * @return ０始まりの行番号と，行頭からのバイト数
	 */
	OutputPoint DiagnosticLocationMap::GetPointAt(const uint32_t Offset) const {
		// バイト位置に対応する行番号の取得
		const uint32_t Row =
		static_cast<uint32_t>(std::upper_bound(LineStarts.begin(), LineStarts.end(), Offset) - LineStarts.begin() - 1);
		// 行番号と行内のバイト位置の返戻
		return { Row, Offset - LineStarts[Row] };
	}
} // namespace Core
