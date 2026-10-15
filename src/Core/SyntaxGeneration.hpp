#pragma once

#include "SyntaxModel.hpp"
#include <string>

namespace Core {
	// 文法の生成形式から構造を組み立てる共通部品
	class SyntaxGeneration {
	public:

		static std::string GetGeneratedFixed(const SyntaxModel &Syntax, const FormOperation Operation, const FormPartRole Part); // 固定トークンの取得関数
		static RewriteStep BraceBodyStep(const SyntaxModel &Syntax, const NodeId Body, const CanonicalForm &Form); // 本体のラッパーの構築関数
		static RewriteStep ContinuationStep(const SyntaxModel &Syntax, const NodeId Id); // 継続括弧の構築関数
	};
} // namespace Core
