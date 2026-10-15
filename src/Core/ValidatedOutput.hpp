#pragma once

#include "CommentRepository.hpp"
#include "OutputPosition.hpp"
#include "SyntaxModel.hpp"
#include <functional>

namespace Core {
	using LintPreparation = std::function<void(SyntaxModel &, CommentRepository &, DiagnosticLocationMap &)>; // 同じ完成候補の静的解析専用の事実を初回に求める処理

	// 検証を通った候補（整形仕様 3.2，`Verified` の場合だけ持ち，最終テキストを１度だけ解析・投影した結果で，パーサーの型を持たない）
	struct ValidatedOutput {
		SyntaxModel FinalSyntax; // 最終構文（構造の照合に使った投影）
		CommentRepository FinalComments; // 最終テキストのコメントの記録（逐語領域に残る物も含め原文順）
		DiagnosticLocationMap Locations; // 最終テキストの診断位置
		LintPreparation PrepareFacts {}; // 此の候補の出所を所有する静的解析用の生成処理（実行前に取り出して空にする）
	};
} // namespace Core
