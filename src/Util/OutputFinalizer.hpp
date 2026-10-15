#pragma once

#include "LayoutTypes.hpp"
#include <string>

// 出力の確定を扱うクラス（合成結果の断片とコメントを其の順にシリアライズし，終端契約を満たす，整形仕様 6.7）
class OutputFinalizer {
public:

	static std::string Finalize(const OutputDocument &Doc); // 出力確定関数

	OutputFinalizer() = delete; // コンストラクタ（禁止）
};
