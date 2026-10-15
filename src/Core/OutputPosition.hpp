#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Core {
	// 最終テキスト上の位置
	struct OutputPoint {
		uint32_t Row = 0; // ０始まりの行番号
		uint32_t Column = 0; // 行頭からのバイト数
	};

	// 最終テキスト上の範囲と開始位置
	struct OutputRange {
		uint32_t Start = 0; // 最終テキストの開始バイト位置
		uint32_t End = 0; // 最終テキストの終了バイト位置（範囲に含まない）
		OutputPoint Point; // 範囲の開始位置の行番号と行内のバイト位置
	};

	// 最終テキストの診断位置（整形仕様 3.3 の DiagnosticLocationMap，検証が最終テキストを１度だけ解析・投影した結果の位置）
	struct DiagnosticLocationMap {
		std::string Text; // 診断位置の基準とする最終テキスト
		std::vector<uint32_t> LineStarts; // 最終テキストの行頭の位置（昇順）
		std::vector<OutputRange> Nodes; // 最終構文のノード毎の位置
		std::vector<OutputRange> Comments; // 最終テキストのコメントの位置（出現順）
		std::vector<uint32_t> DirectiveStarts; // 標準のディレクティブコメントの始まり（昇順）
		std::vector<OutputPoint> Embedded; // HTML の埋込コードの要素の始まり（ドキュメント順）
		OutputPoint FirstBroken; // パーサーが読めなかった最初の位置（部分整形の通知）
		OutputPoint GetPointAt(const uint32_t Offset) const; // バイト位置から行番号と行内のバイト位置を取得する関数
	};
} // namespace Core
