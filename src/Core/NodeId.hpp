#pragma once

#include <cstdint>
#include <limits>

namespace Core {
	using NodeId = uint32_t; // 同じ整形呼出の中で移動・並替に依って変えないノードの識別子（整形仕様 3.2）

	inline constexpr NodeId NoNode = std::numeric_limits<NodeId>::max(); // ノードの不在を表す番兵
} // namespace Core
