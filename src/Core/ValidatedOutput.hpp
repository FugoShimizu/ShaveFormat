#pragma once

#include "CommentRepository.hpp"
#include "OutputPosition.hpp"
#include "SyntaxModel.hpp"

namespace Core {
	// 検証を通った候補（整形仕様 3.2，`Verified` の場合だけ持ち，最終字面を一度だけ解析・投影した結果で，解析器の型を持たない）
	struct ValidatedOutput {
		SyntaxModel FinalSyntax; // 最終構文（構造の照合に使った投影）
		CommentRepository FinalComments; // 最終字面のコメントの記録（逐語区画に残る物も含め原文順）
		DiagnosticLocationMap Locations; // 最終字面の診断位置
	};
} // namespace Core
