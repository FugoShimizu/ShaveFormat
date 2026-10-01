#pragma once

#include "../Core/NodeId.hpp"
#include "../Core/OutputPosition.hpp"
#include <tree_sitter/api.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 投影が解析木から組み立て，検証が照合する資料（整形仕様 3.2, 3.3）
struct ValidationContext {
	// 構文誤りの周りの字面
	struct ErrorZone {
		std::string Text; // 空白を除きコメントを印とした字面
		uint32_t Count = 0; // 入力の全体に現れる数（整形後に減れば何処かの其の字面を失った）
	};

	// 逐語区画の字面に残したコメントの範囲（整形仕様 8.1 の空行の照合基準で除く範囲で，本文を持たない）
	struct KeptComment {
		Core::NodeId Node = Core::NoNode; // 逐語区画の節点
		uint32_t Start = 0; // 字面の中の開始
		uint32_t End = 0; // 字面の中の終了
	};

	// 解析木で確かめる意味上の区別（整形仕様 3.2, 8.1，投影が組み替える前の解析器の結付を含む）
	struct SemanticCheck {
		TSSymbol Node; // 区別する構文の記号
		uint32_t Bound; // 構文の結付を表す量（演算子は右の被演算子の意味上の範囲の名前の字句の数，制御構文は本体が空か，其の他は０）

		bool operator==(const SemanticCheck &Other) const = default; // 等価比較演算子
	};

	// 文書の説明対象と本文（整形仕様 7.4, 8.1）
	struct DocumentFact {
		std::string Target; // 説明対象の識別
		std::vector<std::string> Lines; // 本文の論理行（説明・札・参照・コード例を原文の順に持つ）
		bool IsDedicated = false; // 通常コメントと区別する記法の文書か
	};

	// 保護区画（整形仕様 4.4, 4.5, 8.1，整形後も本文・位置・有効性を保つかを確かめる区画）
	struct ProtectedRegionSet {
		std::vector<std::string> Directives; // 言語標準の指示コメント（整形仕様 4.5，本文・有効性・位置条件）
		std::vector<uint32_t> DirectiveStarts; // 指示コメントの補修後の開始位置
		std::vector<DocumentFact> Documents; // 完成候補の原文順の文書の説明対象と本文（整形仕様 7.4, 8.1，期待側は検証が期待コメントから求める）
		std::vector<std::pair<uint32_t, uint32_t>> ObservedPositions; // 位置を実行時の値として観測する構文の行と桁（整形仕様 4.4）
		std::vector<KeptComment> KeptComments; // 逐語区画の字面に残したコメントの範囲（投影が記録し，検証が出力の位置へ写す）
		std::vector<std::string> AnalysisMarks; // 入力側が解析の為に置いた印（最終字面に残らない事を確かめる，整形仕様 4.2）
		std::string_view InputText; // 入力の字面（整形仕様 8.1 の字句と値の保持を字面で確かめる検査に使う）
	};

	// 意味上の区別の並び（整形仕様 3.2, 8.1）
	struct SemanticCheckList {
		std::vector<SemanticCheck> Checks; // 解析木で確かめた意味上の区別
		std::vector<TSSymbol> Symbols; // 投影元の記号（節点の番号順，生成節点は記号表の外）
	};

	// 行末の記号の前置区切り（整形仕様 8.1）
	struct LeadingSeparator {
		std::string Line; // 記号の後の最初の論理行（期待コメントの記録との照合用）
		size_t Tabs = 0; // 記号の前置区切りのタブの数（不正な区切りは最大値）
	};

	// 入力の診断（整形仕様 8.1, 8.3）
	struct DiagnosticList {
		uint32_t InputBrokenCount = 0; // 入力時点で壊れて居た節点の数（整形後の増加の判定に使う）
		uint32_t BrokenCount = 0; // 補修後の壊れた節点の数
		bool HasBrokenNode = false; // 補修後に構文誤りが在るか
		std::vector<std::pair<uint32_t, uint32_t>> CommentRanges; // 補修後のコメント節点の範囲
		std::vector<Core::OutputRange> Positions; // 補修前の本文へ写した節点毎の診断位置（静的検査が読む検証の構文だけが持つ）
		std::vector<LeadingSeparator> LeadingSeparators; // 行末の各記号の論理行と前置区切り（原文順，検証の構文だけが持つ，整形仕様 8.1）
	};

	std::vector<ErrorZone> ErrorZones; // 入力の構文誤りの周りの字面（部分整形が保つかの判定に使う）
	ProtectedRegionSet ProtectedRegions; // 保護区画
	SemanticCheckList SemanticChecks; // 意味上の区別
	DiagnosticList InputDiagnostics; // 入力の診断
};
