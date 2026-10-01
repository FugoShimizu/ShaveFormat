#pragma once

#include <cstdint>
#include <limits>

namespace Core {
	using NodeId = uint32_t; // 同じ整形呼出の中で移動・並替に依って変えない節点の識別子（整形仕様 3.2）

	inline constexpr NodeId NoNode = std::numeric_limits<NodeId>::max(); // 節点の無い印
} // namespace Core
