#pragma once

#include "../Core/ControlFlow.hpp"
#include "../Core/NodeId.hpp"
#include "../Core/OutputPosition.hpp"
#include "../Core/SignatureExtractor.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace StaticLint {
	using Core::OutputPoint;
	using Core::OutputRange;
	using Core::NumericLiteralKind;

	// 共通の制御解析とシグネチャ抽出の結果
	struct FunctionFacts {
		bool IsDocumentable = false; // ドキュメントを付ける定義か
		bool HasDocAttribute = false; // ドキュメントを処理系の読む属性で持つか
		bool IsMissingReturn = false; // 末尾へ確実に到達し明示の返戻を補う関数か
		Core::NodeId DocAttribute = Core::NoNode; // 本文の値を持つドキュメント属性のノード
		Core::NodeId DocAnchor = Core::NoNode; // ドキュメントのアンカー（ラッパー・前置の属性の前）
		Core::Signature Signature; // シグネチャ
		std::vector<size_t> Docs {}; // 確定済の説明対象が同じドキュメントの記録の番号
		std::vector<Core::NodeId> UnreachableReturningStmts; // 終端した後の返戻文を含む文（原文順）
	};

	// コメントの最終テキストと本文の対応
	struct CommentFacts {
		OutputRange Range; // 最終テキストの範囲
		size_t Record = 0; // コメントの記録の添字
	};

	// 完成構文の共通走査で確定する制御の所属
	struct ControlContext {
		Core::NodeId Flow = Core::NoNode; // 最も内の関数境界（境界外は根）
		Core::NodeId Exit = Core::NoNode; // ラムダを含む脱出の境界
		Core::NodeId Case = Core::NoNode; // 同じ関数内の最も内の切替の節
		Core::NodeId Switch = Core::NoNode; // 同じ関数内の最も内の切替
		Core::NodeId BreakLoop = Core::NoNode; // 段数付の脱出の対象と為る最も内の繰返
		Core::NodeId Loop = Core::NoNode; // 同じ関数内の最も内の繰返
		uint32_t BreakDepth = 0; // 段数付の脱出の対象と為る包含構文の数
		uint32_t LoopDepth = 0; // 同じ関数内で包含する繰返の数
	};

	// 最終出力を参照する検査データ（位置・本文の参照は ValidatedOutput の寿命内だけ有効）
	struct OutputFacts {
		std::shared_ptr<const Core::ControlFlow> Flow; // 同じ完成候補の収集と検査が共有する制御の流れ
		std::vector<Core::NodeId> Nodes; // 共通走査で訪れたノードの先行順（未接続のノードを含めない）
		std::vector<Core::NodeId> Declarations; // 導入名を調べる宣言だけの先行順
		std::vector<Core::NodeId> FirstTokens; // コメントと逐語本文の内側を除く各行の最初の実トークン
		std::unordered_set<std::string_view> PreprocNames; // 前処理条件へ直接現れる名前
		std::vector<NumericLiteralKind> NumericLiterals; // 数値の基数・全桁・接尾辞から確定した分類（対象の規則が無い場合は空）
		std::unordered_map<std::string_view, std::vector<Core::NodeId>> LabelExits; // ラベル名毎の脱出文の先行順
		std::vector<ControlContext> Controls; // ノード毎の制御の所属
		std::span<const uint32_t> LineStarts; // 検証で求めた最終テキストの行頭位置
		std::span<const OutputRange> Positions; // ノード毎の最終テキストの範囲
		std::unordered_map<Core::NodeId, FunctionFacts> Functions; // 関数のノード毎の解析結果
		std::vector<CommentFacts> Comments; // 最終テキストのコメント（原文順）
		std::span<const uint32_t> DirectiveStarts; // ディレクティブコメントの始まりの位置の昇順
	};
}
