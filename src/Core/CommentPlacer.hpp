#pragma once

#include "CommentRepository.hpp"
#include "NodeId.hpp"
#include "SyntaxModel.hpp"
#include "SyntaxRole.hpp"
#include "../Util/LayoutTypes.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Core {
	// コメント配置クラス（確定したコードの配置枠へコメントを割り当て，配置計画だけを返す，整形仕様 7.6）
	class CommentPlacer {
	private:

		friend struct CommentPlacerAccess; // 検証ツールの観測用の入口

		enum class Adjacent : uint8_t { Code, Newline, OpenBracket, Space, Tab }; // 挿入位置の前の字の種類（区切の空白を置くかの判断に使う）

		// 配置枠を決めたコメント（断片・種類・位置・原文順に並べて描画する）
		struct Candidate {
			CommentSlotKind Where = CommentSlotKind::BeforeLine; // 配置枠の種類
			uint32_t Fragment = 0; // 配置枠の断片
			uint32_t Offset = 0; // 文中の配置枠のテキストの位置
			size_t Tabs = 0; // 独立した行へ置く時のタブの段数（後続のコード行の深さ）
			const CommentRecord *Record = nullptr; // 元の記録
			std::string Text; // 記号と本文を合成したテキスト
			bool IsDoc = false; // ドキュメントとして描く記録か
			bool ShouldConvertMemberDoc = false; // 同行の後続コードを持つメンバ専用の記号を通常記法へ替えるか
			bool IsRequired = false; // 置けなくても脱落させない記録か（整形仕様 7.4 で認識した独立行のドキュメント，通常コメントとして置く物を含む）
			bool IsLineHeadRequired = false; // 有効性を保つ為に桁０へ置く記録か
			bool IsVerbatim = false; // 本文の続きの行を原文の儘描く記録か（処理系が読むコメント）
		};

		// 配置枠と描画した物理行の組（配置枠の整列は合成が行う）
		struct RenderedComment {
			CommentSlotId Slot; // 配置枠
			std::vector<std::string> Lines; // 物理行の並び（区切の空白とインデントを含む）
			const std::string *Text; // 配置枠を決めたコメントのテキスト（行の列の条件を満たせず除く時の診断に使う）
			bool IsRequired; // 置けなくても脱落させない記録か
			bool IsDirective; // 保持違反をディレクティブコメントの失敗とするか
		};

		static uint32_t GetFragmentAt(const LaidOutCode &Code, const uint32_t Offset); // テキストの位置を含む断片の取得関数
		static bool IsFollowedByCode(const LaidOutCode &Code, const uint32_t Offset); // テキストの位置の後に同じ行のコードが続くかの判定関数
		static uint32_t GetContentFrom(const LaidOutCode &Code, const uint32_t Offset); // 同じ行の位置以後の最初のコードの位置の取得関数
		static bool HasShape(const CodePiece *const Piece, const uint16_t Shapes); // トークン片の形の判定関数
		static bool IsAfterCode(const LaidOutCode &Code, const uint32_t Offset); // テキストの位置の前に同じ行のコードが在るかの判定関数
		static const CodePiece *GetPieceCovering(const LaidOutCode &Code, const uint32_t Offset); // テキストの位置の字を含むトークン片の取得関数
		static const CodePiece *GetPieceBefore(const LaidOutCode &Code, const uint32_t Offset); // テキストの位置より前に始まる最後のトークン片の取得関数
		static bool IsLineCommentStart(const std::string_view Text); // 行末迄を本文とするコメントかの判定関数

		static void ReportDrop(
			const size_t Bytes,
			const std::string_view Reason,
			const bool IsRequired,
			const bool IsDirective,
			std::vector<std::string> *const Diagnostics,
			const CommentRecord *const Record = nullptr
		); // 置けないコメントの脱落の診断関数

		static std::optional<uint32_t> InlineAfter(
			const LaidOutCode &Code,
			const SyntaxModel &Syntax,
			const CommentRecord &Record,
			const bool HasInner,
			std::unordered_map<NodeId, std::vector<uint32_t>> &InsideTokens
		); // アンカーの後の文中コメントの置場の取得関数

		static bool IsAfterEmptyBraces(const LaidOutCode &Code, const uint32_t Offset); // テキストの位置の直前が空の波括弧かの判定関数
		static uint32_t TokenRunStart(const LaidOutCode &Code, const SyntaxModel &Syntax, const NodeId Anchor, const uint16_t Limit); // アンカーの直前に続く無名のトークンの並びの始まりの取得関数
		static bool IsStmtHeadReadable(const LaidOutCode &Code, const SyntaxModel &Syntax, const NodeId Anchor, const uint32_t At); // 文中コメントを置く行頭の後の文をパーサーが読めるかの判定関数

		// 配置枠と本文描画の内部操作
		static std::string LineFormOf(
			const NotationRules &Rules,
			const CommentRecord &Record,
			const std::string_view Text,
			const std::string_view Composed,
			const bool IsDoc
		); // 行コメントへ揃える記録の行形式のテキストの取得関数

		static std::string StackLineEnd(const std::string_view Text); // 行末へ積む形への変換関数

		static std::vector<Candidate> CollectCandidates(
			const LaidOutCode &Code,
			const SyntaxModel &Syntax,
			const CommentRepository &Comments,
			std::vector<std::pair<uint32_t, uint32_t>> &OpaqueRanges,
			std::vector<std::string> *const Diagnostics,
			StructureWorkAccount *const Work = nullptr
		); // 配置枠の決定関数

		static std::string_view RenderBody(
			const NotationRules &Rules,
			const Candidate &Found,
			const CommentSlotKind Where,
			const bool IsOpaque,
			std::string &Buf
		); // 配置枠に応じた出力記法の本文の取得関数

		static std::vector<std::string> RenderLines(
			const std::string_view TextView,
			const size_t Tabs,
			const bool IsIndentFirst,
			const bool IsVerbatim,
			const bool HasHeadFrames
		); // コメント本文の描画関数

		static Adjacent GetAdjacentBefore(const LaidOutCode &Code, const uint32_t Offset, const uint32_t Content); // テキストの位置の直前の字の種類の取得関数
		static Adjacent GetAdjacentOf(const unsigned char Char); // 描画したコメントの最後の字の種類の取得関数

		static bool IsUnplaceableLineEnd(
			const Candidate &Found,
			const std::string_view Body,
			const bool IsAfterLineComment,
			const bool IsLineCommentVerbatim,
			std::vector<std::string> *const Diagnostics
		); // 行末へ置けない行末コメントかの判定関数

		static CommentPlacementPlan BuildPlan(
			const LaidOutCode &Code,
			std::vector<RenderedComment> &&Items,
			std::vector<std::string> *const Diagnostics
		); // 配置計画の構築関数

		static CommentPlacementPlan PlaceLocal(
			const LaidOutCode &Code,
			const CommentRepository &Comments,
			std::vector<std::string> *const Diagnostics
		); // 単独の所有記録の共通配置関数

	public:

		// コメント配置の公開操作
		static CommentPlacementPlan Place(
			const LaidOutCode &Code,
			const CommentRepository &Comments,
			std::vector<std::string> *const Diagnostics = nullptr
		); // コメント配置関数

		CommentPlacer() = delete; // コンストラクタ（禁止）
	};
} // namespace Core
