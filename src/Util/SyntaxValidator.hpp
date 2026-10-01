#pragma once

#include "../Core/CommentRepository.hpp"
#include "../Core/NodeId.hpp"
#include "../Core/SyntaxModel.hpp"
#include "../Core/ValidatedOutput.hpp"
#include "Lang.hpp"
#include "LayoutTypes.hpp"
#include "ParsedTree.hpp"
#include "ValidationContext.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Core {
	struct DeclarationPartition;
	struct OutputExpectation;
} // namespace Core

class CommentStructure;

// 独立した再解析に依る出力・保護区画の検証クラス（整形仕様 3.1, 8.1）
class SyntaxValidator {
private: // 個別節点の内部判定

	using ByteRanges = std::vector<std::pair<uint32_t, uint32_t>>; // （開始，終わり）の位置順の範囲の並び

	// コメントを除いた論理行の骨格（空行か，空白を除いたコードの字面か）
	struct SkeletonLine {
		bool IsBlank; // 空行か
		std::string Code; // 空白を除いたコードの字面（空行は空）

		bool operator==(const SkeletonLine &Other) const = default; // 等値比較演算子
	};

	// 入力側の記号対応を併用する共通種別の照合資料
	struct KindComparison {
		std::span<const TSSymbol> ExpectedSymbols; // 入力の投影元の記号
		std::span<const TSSymbol> ActualSymbols; // 完成候補の投影元の記号
		const GrammarProfile &Profile; // 記号から共通種別と節点名への対応
		std::string Name(const Core::SyntaxModel &Syntax, const Core::NodeId Node, const std::span<const TSSymbol> Symbols) const; // 診断用の種別名の取得関数

		bool Same(
			const Core::SyntaxModel &Expected,
			const Core::SyntaxModel &Actual,
			const Core::NodeId Before,
			const Core::NodeId After
		) const; // 種別の照合関数
	};

	static std::vector<SkeletonLine> LineSkeleton(
		const ParsedTree *const Decoder,
		const std::string_view Text,
		const std::span<const std::pair<uint32_t, uint32_t>> Comments,
		std::vector<size_t> *const CodeStarts
	); // 行の骨格の取得関数

	static bool KeepsBlankLines(
		const ParsedTree &Checked,
		const ByteRanges &Comments,
		const std::string_view LaidOut,
		const std::span<const std::pair<uint32_t, uint32_t>> Excluded,
		std::vector<size_t> &OutputStarts,
		std::vector<size_t> &LaidOutStarts
	); // 空行の不変性の判定関数

	static std::vector<std::string> PlacedLines(const std::string_view Text, const std::span<const uint8_t> KeptBlanks); // 置く論理行の取得関数

	static std::vector<std::vector<std::string>> OrdinaryBodies(
		const std::span<const CommentRecord> Outputs,
		const CommentStructure &Structure,
		const std::span<const Core::NodeId> Recognized
	); // 独立行の通常コメントの続いた記録毎の本文の論理行の取得関数

	static TSPoint FirstBrokenPoint(const ParsedTree &Src); // 最初の壊れたノードの位置の取得関数

	static bool KeepsBracesOutsideComments(
		const Core::NotationRules &Rules,
		const std::string_view Source,
		const std::string_view Output,
		const std::string_view UrlHead
	); // コメントの中の波括弧の数の保持の判定関数

	static bool MatchesEmbeddedContent(const std::string_view Actual, const Core::OwnedRegion &Region); // 独立したコードとして整形した埋込の中身の照合関数

	static bool KeepsPreservedBoundary(
		const Core::SyntaxModel &Expected,
		const Core::SyntaxModel &Actual,
		const KindComparison &Kinds,
		const Core::NodeId Before,
		const size_t BeforeIndex,
		const Core::NodeId After,
		const size_t AfterIndex,
		const bool IsInMacro,
		const bool ShouldDiagnose
	); // 対応する節点の子の境界を比較する関数

	static bool OptionalTypeParts(const Core::SyntaxModel &Syntax, Core::NodeId Node, Core::NodeId &Base, size_t &Marks); // 省略可能型の土台と印の数の取得関数

	static bool MatchesDeclarationPiece(
		const Core::SyntaxModel &Expected,
		const Core::SyntaxModel &Actual,
		const KindComparison &Kinds,
		const Core::NodeId Before,
		const Core::NodeId After,
		const std::unordered_set<Core::NodeId> *const RowCommas,
		const bool AcceptContextualType,
		std::unordered_set<Core::NodeId> *const UsedCommas
	); // 統合宣言の部品の一致の判定関数

	static bool MatchesContextualTypeRepresentation(
		const Core::SyntaxModel &Expected,
		const Core::SyntaxModel &Actual,
		const KindComparison &Kinds,
		const Core::NodeId Before,
		const Core::NodeId After,
		const std::unordered_set<Core::NodeId> *const RowCommas,
		std::unordered_set<Core::NodeId> *const UsedCommas
	); // 型の表記の違いを許す一致の判定関数

	static bool MatchesDeclarationPartition(
		const Core::SyntaxModel &Expected,
		const Core::SyntaxModel &Actual,
		const KindComparison &Kinds,
		const Core::NodeId Node,
		const std::span<const Core::NodeId> Rows,
		const Core::DeclarationPartition &Partition,
		const std::vector<std::unordered_set<Core::NodeId>> &RowCommas,
		const bool AcceptContextualType
	); // 統合宣言の区分の一致の判定関数

	static bool KeepsStructure(
		const Core::SyntaxModel &Actual,
		const Lang Language,
		const Core::OutputExpectation &Expectation,
		const KindComparison &Kinds,
		const bool ShouldDiagnose
	); // 期待構文と再投影した構文の一致の判定関数

	static size_t CountLostErrorZones(
		const ParsedTree &Src,
		const std::vector<ValidationContext::ErrorZone> &Zones,
		const std::vector<ValidationContext::ErrorZone> &OutputZones,
		const ByteRanges &Comments
	); // 構文誤りの周りの失った字面の数の取得関数

	static size_t CountUnmatchedErrorZones(
		const std::vector<ValidationContext::ErrorZone> &OutputZones,
		const std::vector<ValidationContext::ErrorZone> &Zones
	); // 入力の区画に一致しない構文誤りの区画の数の取得関数

public: // 構文検査の公開入口

	enum class ValidationStatus : uint8_t { Verified, Failed, Skipped }; // 検証の結果の分類（整形仕様 3.2，見送と失敗を真偽値へ併合しない）

	// 通常コメントへ変換した文書（整形仕様 7.6, 8.1）
	struct ConvertedDocument {
		std::vector<std::string> Lines; // 期待コメントの正規化後の本文の論理行（内容の無い論理行は構造として保つ物だけ）
		std::vector<uint32_t> Destinations; // 塊の後の最初のコード行として認める断片
	};

	// 完成候補のコメントだけの行の塊（整形仕様 7.6, 8.1）
	struct CommentRun {
		uint32_t Next; // 塊の後の最初のコード行の断片（無ければ最大値）
		std::vector<std::string> Lines; // 塊のコメントの論理行
		std::vector<uint8_t> Documents; // 論理行毎の通常コメントと区別する文書の記法の記録の物か
	};

	// 検証の結果（`Verified` 以外は候補を静的検査・書戻へ渡さず，原文の返戻は `Formatter` が行う）
	struct ValidationResult {
		ValidationStatus Status = ValidationStatus::Verified; // 検証の分類
		std::optional<Core::ValidatedOutput> Output; // 検証を通った候補（`Verified` の場合だけ持つ）
		FailureReason Reason = FailureReason::None; // 見送又は失敗の理由
	};

	static std::vector<ConvertedDocument> ConvertedDocuments(
		const LaidOutCode &Code,
		const std::span<const CommentRecord> Outputs,
		const CommentStructure &Structure,
		const std::span<const Core::NodeId> Recognized,
		const std::span<const Core::NodeId> Placed
	); // 通常コメントへ変換した文書の取得関数

	static bool KeepsDocuments(
		const std::span<const ValidationContext::DocumentFact> Expected,
		const std::span<const ValidationContext::DocumentFact> Actual,
		const std::span<const CommentRecord> Outputs,
		const std::span<const Core::NodeId> Placed
	); // 文書の説明対象と本文の保持の判定関数

	static bool KeepsConvertedDocuments(
		const std::span<const ConvertedDocument> Converted,
		const std::span<const std::vector<std::string>> Ordinary,
		const std::span<const CommentRun> Runs
	); // 通常コメントへ変換した文書の本文と移動先の保持の判定関数

	static std::vector<CommentRun> CommentRuns(
		const std::string_view Text,
		const ByteRanges &Comments,
		const Core::CommentRepository &Output,
		const std::span<const size_t> CodeStarts,
		const std::span<const uint32_t> CodeLines,
		const ReadingRules &Reading
	); // 完成候補のコメントだけの行の塊の取得関数

	static ValidationResult Validate(
		const GrammarProfile &Profile,
		std::shared_ptr<const LaidOutCode> Code,
		const std::string &Src,
		const Core::OutputExpectation &Expectation,
		const Core::CommentRepository &Comments,
		const ValidationContext &Context
	); // 最終字面の検証関数

	SyntaxValidator() = delete; // コンストラクタ（禁止）
};
