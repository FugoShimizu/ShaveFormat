#pragma once

#include "FormatIR.hpp"
#include "SyntaxModel.hpp"
#include "../Util/LayoutTypes.hpp"
#include <cstddef>
#include <cstdint>
#include <utility>

namespace Core {
	// 解決済の中間表現へのインデント・空行の具体化クラス（整形仕様 3.1, 6.5）
	class CodeLayout {
	private:

		friend struct CodeLayoutAccess;

		static uint16_t GetShapesOf(const SyntaxModel &Syntax, const ResolvedItem &Item); // トークン要素の先頭・末尾の形の取得関数
		static void PlaceChild(LaidOutCode &Code, OwnedRegionSpan &Region); // 子の断片・アンカー・配置枠の親の深さへの具体化関数
		static LaidOutCode MaterializeWithWork(const BrokenIR &IR, std::pair<size_t, size_t> *const TextWork); // 本文の参照数を観測する配置関数

	public:

		static LaidOutCode Materialize(const BrokenIR &IR); // 変更不能なコードの出力の構築関数

		CodeLayout() = delete; // コンストラクタ（禁止）
	};
} // namespace Core
