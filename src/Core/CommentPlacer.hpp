#pragma once

#include "../Util/LayoutTypes.hpp"
#include "CommentRepository.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class CommentStructure;

namespace Core {
	// コメント配置クラス（確定したコードの配置枠へコメントを割り当て，配置計画だけを返す，整形仕様 7.6）
	class CommentPlacer {
	private:

		enum class Adjacent : uint8_t { Space, Code, Newline, OpenBracket, Tab }; // 挿入位置の前の字の種類（区切りの空白を置くかの判断に使う）

		// 配置枠を決めたコメント（断片・種類・位置・原文順に並べて描画する）
		struct Candidate {
			CommentSlotKind Where = CommentSlotKind::BeforeLine; // 配置枠の種類
			uint32_t Fragment = 0; // 配置枠の断片
			uint32_t Offset = 0; // 文中の配置枠の字面の位置
			size_t Tabs = 0; // 独立した行へ置く時のタブの段数（後続のコード行の深さ）
			const CommentRecord *Record = nullptr; // 元の記録
			const CommentRecord *Output = nullptr; // 出力記法の記録
			std::string Text; // 記号と本文を合成した字面
			bool IsDocument = false; // 文書として描く記録か
			bool IsRequired = false; // 置けなくても脱落させない記録か（整形仕様 7.4 で認識した独立行の文書，通常コメントとして置く物を含む）
			bool IsVerbatim = false; // 本文の続きの行を原文の儘描く記録か（処理系が読むコメント）
			std::span<const uint8_t> KeptBlanks; // 出力記法の本文の論理行毎の構造として保つ空の論理行の印（整形仕様 7.3）
		};

		// 配置枠と描画した物理行の組（配置枠の順に並べてから計画へ加える）
		struct RenderedComment {
			CommentSlotId Slot; // 配置枠
			std::vector<std::string> Lines; // 物理行の並び（区切りの空白とインデントを含む）
			const std::string *Text; // 配置枠を決めたコメントの字面（行の列の条件を満たせず除く時の診断に使う）
			uint32_t Sequence; // 描画の順（同じ配置枠の中の出力順）
			bool IsRequired; // 置けなくても脱落させない記録か
		};

		static std::string LineFormOf(
			const NotationRules &Rules,
			const CommentRecord &Record,
			const std::string_view Text,
			const bool IsDocument,
			const std::span<const uint8_t> KeptBlanks
		); // 行コメントへ揃える記録の行形式の字面の取得関数

		static void DropEmptyLogicalLines(CommentRecord &Record, const std::span<const uint8_t> KeptBlanks); // 通常コメントの空の論理行の除去関数
		static bool OpensLineComment(const std::string_view Text); // 行末迄を本文とするコメントかの判定関数
		static std::string StackLineEnd(const std::string_view Text); // 行末へ積む形への変換関数
		static void ReportDrop(const std::string_view Text, const std::string_view Reason); // 置けないコメントの脱落の診断関数

		static std::string_view RenderBody(
			const NotationRules &Rules,
			const Candidate &Found,
			const CommentSlotKind Where,
			const bool IsOpaque,
			std::string &Buffer
		); // 配置枠に応じた出力記法の本文の取得関数

		static bool IsUnplaceableLineEnd(
			const std::string_view Text,
			const std::string_view Body,
			const bool IsVerbatim,
			const bool IsAfterLineComment,
			const bool IsLineCommentVerbatim
		); // 行末へ置けない行末コメントかの判定関数

		static std::vector<std::string> RenderLines(
			const std::string_view TextView,
			const size_t Tabs,
			const bool IsIndentFirst,
			const bool IsVerbatim,
			const bool HasHeadFrames,
			size_t &Written
		); // コメント本文の描画関数

		static CommentPlacementPlan BuildPlan(const LaidOutCode &Code, std::vector<RenderedComment> &&Items); // 配置計画の構築関数
		static uint32_t FragmentAt(const LaidOutCode &Code, const uint32_t Offset); // 字面の位置を含む断片の取得関数
		static const CodePiece *PieceBefore(const LaidOutCode &Code, const uint32_t Offset); // 字面の位置より前に始まる最後の字句片の取得関数
		static const CodePiece *PieceCovering(const LaidOutCode &Code, const uint32_t Offset); // 字面の位置の字を含む字句片の取得関数
		static bool HasShape(const CodePiece *const Piece, const uint16_t Shapes); // 字句片の形の判定関数
		static uint32_t ContentFrom(const LaidOutCode &Code, const uint32_t Offset); // 同じ行の位置以後の最初のコードの位置の取得関数
		static bool EndsWithEmptyBraces(const LaidOutCode &Code, const uint32_t Offset); // 字面の位置の直前が空の波括弧かの判定関数
		static Adjacent AdjacentBefore(const LaidOutCode &Code, const uint32_t Offset, const uint32_t Content); // 字面の位置の直前の字の種類の取得関数
		static Adjacent AdjacentOf(const char Char); // 描画したコメントの最後の字の種類の取得関数
		static bool IsAfterCode(const LaidOutCode &Code, const uint32_t Offset); // 字面の位置の前に同じ行のコードが在るかの判定関数
		static bool IsFollowedByCode(const LaidOutCode &Code, const uint32_t Offset); // 字面の位置の後に同じ行のコードが続くかの判定関数
		static uint32_t TokenRunStart(const LaidOutCode &Code, const SyntaxModel &Syntax, const NodeId Anchor, const uint16_t Limit); // 錨の直前に続く無名の字句の並びの始まりの取得関数

		static std::optional<uint32_t> InlineAfter(
			const LaidOutCode &Code,
			const SyntaxModel &Syntax,
			const CommentRecord &Record,
			const bool HasInner
		); // 錨の後の文中コメントの置場の取得関数

		static bool IsStatementHeadReadable(const LaidOutCode &Code, const SyntaxModel &Syntax, const NodeId Anchor, const uint32_t At); // 文中コメントを置く行頭の後の文を解析器が読めるかの判定関数

		static std::vector<Candidate> CollectCandidates(
			const LaidOutCode &Code,
			const SyntaxModel &Syntax,
			const CommentRepository &Comments,
			const std::span<const CommentRecord> Outputs,
			const CommentStructure &Structure
		); // 配置枠の決定関数

	public:

		static CommentPlacementPlan Place(const LaidOutCode &Code, const CommentRepository &Comments); // コメント配置関数

		CommentPlacer() = delete; // コンストラクタ（禁止）
	};
} // namespace Core
