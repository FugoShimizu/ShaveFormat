#pragma once

#include "../Util/LayoutTypes.hpp"
#include "FormatIR.hpp"
#include <cstdint>
#include <unordered_set>

namespace Core {
	// 解決済の中間表現へのインデント・空行の具体化クラス（整形仕様 3.1, 6.5）
	class CodeLayout {
	private:

		static std::unordered_set<NodeId> MultilineNodes(const BrokenIR &IR); // 空行の規則が参照する宣言の内の複数行の宣言の収集関数
		static uint16_t ShapesOf(const SyntaxModel &Syntax, const ResolvedItem &Item); // 字句の要素の端の字句の形の取得関数

	public:

		static LaidOutCode Materialize(const BrokenIR &IR); // 変更不能なコードの出力の構築関数

		CodeLayout() = delete; // コンストラクタ（禁止）
	};
} // namespace Core
