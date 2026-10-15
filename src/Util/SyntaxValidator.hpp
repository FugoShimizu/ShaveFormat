#pragma once

#include "LayoutTypes.hpp"
#include "ParsedTree.hpp"
#include "ValidationContext.hpp"
#include "../Core/CommentRepository.hpp"
#include "../Core/NodeId.hpp"
#include "../Core/SyntaxModel.hpp"
#include "../Core/ValidatedOutput.hpp"
#include <tree_sitter/api.h>
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
	struct DeclPartition;
	struct OutputExpectation;
}

// 独立した再解析に依る出力・保護領域の検証クラス（整形仕様 3.1, 8.1）
class SyntaxValidator {
private: // 個別ノードの内部判定

	friend struct SyntaxValidatorCmp; // テストだけが許可済の表現対応と破壊した候補を直接照合する

	using ByteRanges = std::vector<std::pair<uint32_t, uint32_t>>; // （開始，終わり）の位置順の範囲の並び

	enum class ErrorZonePart : uint8_t { Normal, Ancestor, Interior }; // 正常な部分木と，喪失した保護範囲の本体・祖先の区別

	// 空白と除外範囲を除いたコード及び元の位置との対応
	struct CodeIndex {
		std::vector<uint32_t> Positions; // 元の位置より前のコードのバイト数
		std::string Text; // 空白と除外範囲を除いたテキスト
		std::vector<uint8_t> Marks; // 各コード位置にコメント等の除外範囲が在るか（末尾を含む）
	};

	// コメントを除いた論理行の骨格（空行か，空白を除いたコードのテキストか）
	struct SkeletonLine {
		bool IsBlank; // 空行か
		size_t Start; // 共通のコードのテキストの始まり
		size_t Length; // 空白を除いたコードの長さ（空行は０）
	};

	// 入力側の記号対応を併用する共通種別の照合データ
	struct KindCmp {
		std::span<const TSSymbol> ExpectedSymbols; // 入力の投影元の記号
		std::span<const TSSymbol> ActualSymbols; // 完成候補の投影元の記号
		const GrammarProfile &Profile; // 記号から共通種別とノード名への対応
		std::string GetName(const Core::SyntaxModel &Syntax, const Core::NodeId Node, const std::span<const TSSymbol> Symbols) const; // 診断用の種別名の取得関数

		bool IsSame(
			const Core::SyntaxModel &Expected,
			const Core::SyntaxModel &Actual,
			const Core::NodeId Before,
			const Core::NodeId After
		) const; // 種別の照合関数
	};

	// 通常の子と分割宣言の部品が共有する照合の作業
	struct NodeMatch {
		Core::NodeId Before; // 期待ノード
		Core::NodeId After; // 出力ノード
		bool IsInMacro = false; // 保存するトークン列の内側か
		const std::unordered_set<Core::NodeId> *RowCommas = nullptr; // 此の出力宣言の共有指定に選択した末尾カンマ
		std::shared_ptr<std::unordered_set<Core::NodeId>> UsedCommas {}; // 共有指定の子孫で照合した句読点の構文位置
		bool IsRowEnd = false; // 共有指定の全照合後に選択の消費を確認する作業か
		const Core::GeneratedNode *Generated = nullptr; // 選択したラッパー又は組直の部品（既存の部分木は参照する）
		std::optional<uint64_t> Fields = std::nullopt; // 生成部品の中で選択した場
		bool HasSelectedForm = false; // 此のノードの選択を既に照合したか
		const std::unordered_set<Core::NodeId> *RowSeparators = nullptr; // 此の出力宣言の共有指定に選択したメンバ区切の直後の子
	};

	// 構造の対応と作業領域の再使用の観測値
	struct MatchWork {
		size_t BoundaryAdvances = 0; // 同じ親の照合で前進した境界の数
		size_t Pairs = 0; // 照合した構文対と宣言行の終端の数
		size_t Growth = 0; // 作業領域の容量を増したノードの数
		size_t Peak = 0; // 作業領域の最大容量
	};

	// 喪失の判定と遮蔽で共有する，領域の識別毎の出力位置
	struct ErrorZoneLayout {
		CodeIndex Before; // 配置済コードの位置とテキスト
		CodeIndex After; // 完成候補の位置とテキスト
		ByteRanges Ranges; // 所有ノードから求めた前後の境界を含むコード位置（領域順，無い物は UINT32_MAX）
	};

	// 喪失した保護領域の本体と祖先の分類
	struct ErrorZoneMasks {
		std::vector<ErrorZonePart> Expected; // 入力の所有ノードから求めた分類
		std::vector<ErrorZonePart> Actual; // 所有ノードの出力範囲に対応する完成候補の分類
	};

	// 空行の形と，行の対応を確定するテキストの一致
	struct LineComparison {
		bool HasSameBlanks = true; // コード行の順に対する空行の有無・数が同じか
		bool HasSameNormalCode = true; // エラー領域に接しないコード行のテキストが同じか
	};

	// 前処理の見出のコメントの独立照合用の事実
	struct DirectiveCommentFacts {
		std::vector<std::vector<std::string>> Lines; // 種別毎の配置した本文の論理行（各種別の原文順）
		std::vector<std::string> Kept; // 逐語トークンに残したコメント（原文順）

		bool operator==(const DirectiveCommentFacts &Other) const = default; // 保持事実の等価比較演算子
	};

	const bool ShouldDiagnoseStructure; // 構造の差の補助診断を出す指定
	static bool HasOptionalTypeParts(const Core::SyntaxModel &Syntax, Core::NodeId Node, Core::NodeId &Base, size_t &Marks); // 省略可能型を構成する基底の型と `?` の数の取得関数
	static CodeIndex IndexCode(const std::string_view Text, const std::span<const std::pair<uint32_t, uint32_t>> Excluded); // コード位置・テキスト・コメントのマーカーのインデックス化関数

	static std::vector<SkeletonLine> LineSkeleton(
		const std::string_view Text,
		const std::span<const std::pair<uint32_t, uint32_t>> Comments,
		std::string &Code,
		std::vector<size_t> *const CodeStarts
	); // 行の骨格の取得関数

	static std::vector<std::string> PlacedLines(
		const std::string_view Text,
		const std::string_view StackMarker = "",
		const std::span<const unsigned char> Protected = std::span<const unsigned char>()
	); // 置く論理行の取得関数

	static size_t CountBracesInComments(
		const Core::NotationRules &Rules,
		const std::string_view Text,
		const std::string_view UrlHead
	); // コメントの中の波括弧の計数関数

	template<class Match, class Cmp, class TypeFunction, class PartitionFunction> static bool HasMatchingStructure(
		const Core::SyntaxModel &Actual,
		const Core::OutputExpectation &Expectation,
		const Cmp &Kinds,
		const bool ShouldDiagnose,
		const std::span<const ErrorZonePart> ExpectedErrors,
		const std::span<const ErrorZonePart> ActualErrors,
		const TypeFunction TypeMatch,
		const PartitionFunction PartitionMatch,
		MatchWork *const Work = nullptr
	); // 正常領域の構造対応の照合関数

	static bool HasSignificantBoundary(const Core::SemanticBoundary &Boundary); // 構造照合が読む境界かの判定関数

	static bool HasSamePreservedBoundary(
		const Core::SyntaxModel &Expected,
		const Core::SyntaxModel &Actual,
		const KindCmp &Kinds,
		const Core::NodeId Before,
		const size_t BeforeIdx,
		const Core::NodeId After,
		const size_t AfterIdx,
		const Core::SemanticBoundary *const Original,
		const Core::SemanticBoundary *const Output,
		const bool ShouldDiagnose
	); // 対応するノードの子の境界を比較する関数

	static bool HasMatchingContextualTypeRepresentation(
		const Core::SyntaxModel &Expected,
		const Core::SyntaxModel &Actual,
		const NodeMatch &Frame,
		std::vector<NodeMatch> &Matched
	); // 文脈に依る型表現の対応を共通の照合へ登録する関数

	static bool HasMatchingDeclPartition(
		const Core::SyntaxModel &Expected,
		const Core::SyntaxModel &Actual,
		const KindCmp &Kinds,
		const Core::NodeId Node,
		const std::span<const Core::NodeId> Rows,
		const Core::DeclPartition &Partition,
		const std::vector<std::unordered_set<Core::NodeId>> &RowCommas,
		const bool IsInMacro,
		std::vector<NodeMatch> &Matched
	); // 統合宣言の区分の対応を共通の照合へ登録する関数

	static ErrorZoneLayout MapErrorZones(
		const std::span<const std::vector<Core::NodeId>> ExpectedOwners,
		const std::span<const ValidationContext::ErrorZone> Zones,
		const std::span<const NodeSpan> Spans,
		const std::string_view LaidOutput,
		const ByteRanges &Excluded,
		const std::string_view Candidate,
		const ByteRanges &Comments
	); // 所有ノードの出力範囲に依るエラー領域の位置の対応付関数

	static std::vector<size_t> LostErrorZones(
		const std::vector<ValidationContext::ErrorZone> &Zones,
		const ErrorZoneLayout &Layout
	); // 失ったエラー領域のインデックスの収集関数

	static ErrorZoneMasks ClassifyErrorZones(
		const Core::SyntaxModel &Expected,
		const Core::SyntaxModel &Actual,
		const std::span<const size_t> Lost,
		const std::span<const NodeSpan> Spans,
		const ErrorZoneLayout &Layout,
		const std::span<const Core::OutputRange> Locations
	); // 失ったエラー領域と独立した正常領域の分類関数

	static void MergeRanges(ByteRanges &Ranges); // 重なる除外範囲の統合関数

	static LineComparison CompareLayout(
		const ParsedTree &Checked,
		const std::string_view Text,
		const ByteRanges &Comments,
		const std::string_view LaidOutput,
		const std::span<const std::pair<uint32_t, uint32_t>> Excluded,
		const ByteRanges &Errors,
		std::vector<size_t> &OutputStarts,
		std::vector<size_t> &LaidOutputStarts
	); // 空行とコード行の対応の照合関数

	static std::vector<std::vector<std::string>> OrdinaryBodies(
		const std::span<const CommentRecord> Outputs,
		const std::span<const Core::NodeId> Recognized
	); // 独立行の通常コメントの続いた記録毎の本文の論理行の取得関数

	static DirectiveCommentFacts GetDirectiveCommentFacts(
		const Core::SyntaxModel &Syntax,
		const Core::CommentRepository &Comments,
		const std::span<const ValidationContext::KeptComment> Kept
	); // 前処理の見出のコメントの本文・種別・順序・出現数の取得関数

	static bool HasNoExtraCommentBraces(
		const Core::NotationRules &Rules,
		const std::string_view Src,
		const std::string_view Output,
		const std::string_view UrlHead
	); // コメントの中の波括弧の数の保持の判定関数

	static bool HasSameStructure(
		const Core::SyntaxModel &Actual,
		const Core::OutputExpectation &Expectation,
		const KindCmp &Kinds,
		const bool ShouldDiagnose,
		MatchWork *const Work = nullptr
	); // 期待構文と再投影した構文の一致の判定関数

	static size_t CountLostErrorZones(const std::vector<ValidationContext::ErrorZone> &Zones, const ErrorZoneLayout &Layout); // 構文エラーの周りの失ったテキストの数の取得関数

	static size_t CountUnmatchedErrorZones(
		const std::vector<ValidationContext::ErrorZone> &OutputZones,
		const std::vector<ValidationContext::ErrorZone> &Zones
	); // 入力の領域に一致しない構文エラーの領域の数の取得関数

public: // 構文検査の公開入口

	enum class ValidationStatus : uint8_t { Verified, Failed, Skipped }; // 検証の結果の分類（整形仕様 3.2，見送と失敗を真偽値へ併合しない）

	// 子の実候補の独立した検証の結果（事前の完成テキストとの比較はしない）
	struct ValidatedChild {
		size_t Ordinal; // 親ドキュメントの埋込の順
		uint32_t Start; // 子の検証対象の親の最終候補内の開始位置
		Core::ValidatedOutput Output; // 子の実候補の独立した投影と診断位置
	};

	// 通常コメントへ変換したドキュメント（整形仕様 7.6, 8.1）
	struct ConvertedDoc {
		std::vector<std::string> Lines; // 期待コメントの正規化後の本文の論理行（内容の無い論理行は構造として保つ物だけ）
		std::vector<uint32_t> Destinations; // ブロックの後の最初のコード行として認める断片
	};

	// 完成候補のコメントだけの行のブロック（整形仕様 7.6, 8.1）
	struct CommentRun {
		uint32_t Next; // ブロックの後の最初のコード行の断片（無ければ最大値）
		std::vector<std::string> Lines; // ブロックのコメントの論理行
		std::vector<uint8_t> Docs; // 論理行毎の通常コメントと区別するドキュメントの記法の記録の物か
	};

	// 親と子の独立した検証の結果
	class ValidationResult {
	private:

		friend class SyntaxValidator;

		ValidationStatus Status;
		std::optional<Core::ValidatedOutput> Output;
		FailureReason Reason;
		std::vector<ValidatedChild> Children;

		static ValidationResult ValidateImpl(
			const GrammarProfile &Profile,
			ValidationContext &Ctx,
			std::shared_ptr<const LaidOutCode> Code,
			const std::string &Src,
			const Core::CommentRepository &Comments,
			Core::OutputExpectation &Expectation,
			const bool ShouldDiagnose
		); // 最終テキストの共通の検証関数

		ValidationResult(
			const ValidationStatus Status,
			std::optional<Core::ValidatedOutput> Output,
			const FailureReason Reason,
			std::vector<ValidatedChild> Children = std::vector<ValidatedChild>()
		); // 検証工程だけの結果の構築口

	public:

		ValidationStatus GetStatus() const; // 検証分類の取得関数
		FailureReason GetReason() const; // 見送又は失敗の理由の取得関数
		std::optional<Core::ValidatedOutput> &GetOutput(); // 静的解析用の事実を補う検証済の候補の取得関数
		std::span<ValidatedChild> GetChildren(); // 静的解析用の事実を補う検証済の子の取得関数
	};

	static std::vector<ConvertedDoc> ConvertedDocs(
		const LaidOutCode &Code,
		const std::span<const CommentRecord> Outputs,
		const std::span<const Core::NodeId> Recognized,
		const std::span<const Core::NodeId> Placed
	); // 通常コメントへ変換したドキュメントの取得関数

	static bool HasSameDocs(
		const std::span<const ValidationContext::DocFact> Expected,
		const std::span<const ValidationContext::DocFact> Actual,
		const std::span<const CommentRecord> Outputs,
		const std::span<const Core::NodeId> Placed,
		size_t *const Work = nullptr,
		size_t *const TargetQueries = nullptr
	); // ドキュメントの説明対象と本文の保持の判定関数

	static bool HasMatchingConvertedDocs(
		const std::span<const ConvertedDoc> Converted,
		const std::span<const std::vector<std::string>> Ordinary,
		const std::span<const CommentRun> Runs
	); // 通常コメントへ変換したドキュメントの本文と移動先の保持の判定関数

	static std::vector<CommentRun> CommentRuns(
		const ReadingRules &Reading,
		const Core::CommentRepository &Output,
		const std::string_view Text,
		const ByteRanges &Comments,
		const std::span<const size_t> CodeStarts,
		const std::span<const uint32_t> CodeLines
	); // 完成候補のコメントだけの行のブロックの取得関数

	ValidationResult Validate(
		const GrammarProfile &Profile,
		std::shared_ptr<const LaidOutCode> Code,
		const std::string &Src,
		Core::OutputExpectation &&Expectation,
		const Core::CommentRepository &Comments,
		ValidationContext Ctx
	) const; // 所有する期待を解析前に縮約する検証関数

	explicit SyntaxValidator(const bool Diagnose = false); // 構造の差の診断指定を受け取るコンストラクタ
};
