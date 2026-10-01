#include "OutputPosition.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Core {
	/**
	 * 字面の位置の行と桁の取得関数
	 * @param Offset 最終字面の位置
	 * @return 其の位置の行と桁
	 */
	OutputPoint DiagnosticLocationMap::PointAt(const uint32_t Offset) const {
		const uint32_t Row =
		static_cast<uint32_t>(std::upper_bound(LineStarts.begin(), LineStarts.end(), Offset) - LineStarts.begin() - 1);
		// 行と桁の返戻
		return { Row, Offset - LineStarts[Row] };
	}

	/**
	 * 埋込の中身の位置の最終字面の位置への写像関数（整形仕様 4.4, 8.4）
	 * 中身は開始タグの後の改行の次の行から始まり，各行は最終字面と中身の字面の字下げの差だけ桁がずれる
	 * 計算量：中身の長さ M と位置の数 P に対し O(M + P)
	 * @param Ordinal 埋込の文書順の番号
	 * @param Content 独立したコードとして整形した中身の字面
	 * @param Inner 中身の字面の上の位置の並び（写した位置で置き換える）
	 * @return 最終字面の上の位置の並び（`Inner` と同じ順）
	 */
	std::vector<OutputPoint> DiagnosticLocationMap::EmbeddedPoints(
		const size_t Ordinal,
		const std::string_view Content,
		std::vector<OutputPoint> Inner
	) const {
		// 行頭の空白の長さの取得関数（字面の外の行は字下げを持たない）
		const auto IndentAt = [](const std::string_view Line, const size_t Head) -> uint32_t {
			// 字面の外の行の返戻
			if(Head > Line.size()) return 0;
			// 行頭の空白の長さの返戻
			return static_cast<uint32_t>(std::min(Line.find_first_not_of(" \t", Head), Line.size()) - Head);
		};
		// 中身の字面の行頭の位置
		std::vector<size_t> ContentStarts { 0 };
		for(size_t Newline = Content.find('\n'); Newline != std::string_view::npos; Newline = Content.find('\n', Newline + 1)) {
			ContentStarts.push_back(Newline + 1);
		}
		// 中身の各位置の最終字面への写像
		for(OutputPoint &Point : Inner) {
			const uint32_t Row = Embedded[Ordinal].second.Row + 1 + Point.Row;
			const size_t ContentHead = Point.Row < ContentStarts.size() ? ContentStarts[Point.Row] : Content.size() + 1;
			Point = {
				Row,
				Point.Column + IndentAt(Text, Row < LineStarts.size() ? LineStarts[Row] : Text.size() + 1) - IndentAt(Content, ContentHead)
			};
		}
		// 写した位置の返戻
		return Inner;
	}
} // namespace Core
