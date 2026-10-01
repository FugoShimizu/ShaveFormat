#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Core {
	// 最終字面上の位置
	struct OutputPoint {
		uint32_t Row = 0;
		uint32_t Column = 0;
	};

	struct OutputRange {
		uint32_t Start = 0;
		uint32_t End = 0;
		OutputPoint Point;
	};

	// 最終字面の診断位置（整形仕様 3.3 の DiagnosticLocationMap，検証が最終字面を一度だけ解析・投影した結果の位置）
	struct DiagnosticLocationMap {
		std::string Text; // 位置の基準の字面（解析用の印を置いた最終字面）
		std::vector<uint32_t> LineStarts; // 最終字面の行頭の位置（昇順）
		std::vector<OutputRange> Nodes; // 最終構文の節点毎の位置
		std::vector<OutputRange> Comments; // 最終字面のコメントの位置（原文順）
		std::vector<uint32_t> DirectiveStarts; // 標準の指示コメントの始まり（昇順）
		std::vector<std::pair<OutputPoint, OutputPoint>> Embedded; // HTML の埋込コードの要素と中身の始まり（文書順，中身の無い要素は要素の始まり）
		OutputPoint FirstBroken; // 解析器が読めなかった最初の位置（部分整形の通知）
		OutputPoint PointAt(const uint32_t Offset) const; // 字面の位置の行と桁の取得関数

		std::vector<OutputPoint> EmbeddedPoints(
			const size_t Ordinal,
			const std::string_view Content,
			std::vector<OutputPoint> Inner
		) const; // 埋込の中身の位置の最終字面の位置への写像関数
	};
} // namespace Core
