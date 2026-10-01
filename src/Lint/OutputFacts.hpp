#pragma once

#include "../Core/NodeId.hpp"
#include "../Core/OutputPosition.hpp"
#include "../Core/SignatureExtractor.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace StaticLint {
	using Core::OutputPoint;
	using Core::OutputRange;

	// 共通の制御解析と署名抽出の結果
	struct FunctionFacts {
		bool IsDocumentable = false; // 文書を付ける定義か
		bool HasDocumentAttribute = false; // 文書を処理系の読む属性で持つか
		bool IsMissingReturn = false; // 末尾へ確実に到達し明示の返戻を補う関数か
		bool IsReturnUncertain = false; // 末尾への到達を構文から確かめれない関数か
		Core::NodeId DocumentAnchor = Core::NoNode; // 文書の錨（包装・前置の属性の前）
		Core::Signature Signature; // 署名
		std::vector<Core::NodeId> UnreachableReturningStatements; // 終端した後の返戻文を含む文（原文順）
	};

	// コメントの最終字面と本文の対応
	struct CommentFacts {
		OutputRange Range; // 最終字面の範囲
		size_t Record = 0; // コメントの記録の添字
		bool IsDocumentAttribute = false; // 処理系の読む文書属性に為る記法か
		std::string Text; // 最終字面の記号を含む字面
	};

	// 最終投影だけが所有する検査資料（言語・解析器の所有物を持たない）
	struct OutputFacts {
		std::vector<OutputRange> Positions; // 節点毎の最終字面の範囲
		std::unordered_map<Core::NodeId, FunctionFacts> Functions; // 関数の節点毎の解析結果
		std::vector<CommentFacts> Comments; // 最終字面のコメント（原文順）
		std::vector<uint32_t> DirectiveStarts; // 指示コメントの始まりの位置の昇順
	};
}
