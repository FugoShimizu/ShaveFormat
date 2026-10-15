#pragma once

// 入力表現の正規化で実際に行った変更（整形仕様 4.1, 10.4）
struct InputNormalization {
	bool HasRemovedBom = false; // 先頭の UTF-8 BOM を除いたか
	bool HasFoldedCrLf = false; // 逐語領域の外の CRLF を LF へ畳んだか
	bool HasNormalizedCrOnly = false; // LF を持たず単独 CR で行を区切る原文の逐語領域の外の改行を LF へ変えたか
	bool HasNormalizedUnicodeSeparator = false; // 文法が読む Unicode の区切を LF へ変えたか
	bool HasMixedNewlines = false; // 正規化前に CRLF と裸の LF が共存したか
};
