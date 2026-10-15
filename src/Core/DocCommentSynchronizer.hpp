#pragma once

#include "DocCommentReader.hpp"
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Core {
	// ドキュメントのシグネチャ同期と配置に従うグループの確定
	class DocCommentSynchronizer : public DocCommentReader {
	private:

		friend struct DocCommentSynchronizerAccess; // 埋込ドキュメントの分離の作業計数を観測する検証ツール

		struct DocCoverage {
			std::unordered_set<std::string> Names; // 同じ定義の既存項目が説明する仮引数名
			bool HasReturn = false; // 同じ定義の既存の戻値の項目の有無
		};

		static void CoverItems(const std::optional<std::vector<DocItem>> &Items, DocCoverage &Coverage); // 既存項目の補完済の対象の記録関数
		static void NormalizeTagSynonym(const NotationRules &Rules, std::string &Line); // 同義のタグの規約の綴りへの統一関数
		static std::string NewParamLine(const NotationRules &Rules, const std::string_view Name); // 仮引数の新しい項目の論理行の生成関数
		static std::string_view GetNewReturnLine(const NotationRules &Rules); // 戻値の新しい項目の論理行の取得関数

		static std::vector<std::string> EncodeDescription(
			const NotationRules &Rules,
			const CommentStructure *Previous,
			const std::vector<std::string> &Lines
		); // 通常の説明のドキュメント形式へのエスケープ関数

		static void MergeDescription(
			const NotationRules &Rules,
			const CommentStructure *Previous,
			const std::optional<std::vector<DocItem>> &Items,
			const std::vector<std::string> &Before,
			const std::vector<std::string> &After,
			std::vector<std::string> &Lines,
			std::vector<size_t> &GroupStarts
		); // 定義の直前の通常コメントのドキュメントの説明への統合関数

		static std::vector<std::string> SynchronizeLines(
			const NotationRules &Rules,
			const CommentStructure *Previous,
			const Signature &FunctionSig,
			std::vector<std::string> &&Lines,
			const std::vector<std::string> &Before,
			const std::vector<std::string> &After,
			std::vector<size_t> GroupStarts,
			const Signature *const Completion = nullptr,
			DocCoverage *const Coverage = nullptr
		); // 本文世代毎の項目を共有する説明の統合・整合関数

		static void SplitEmbedded(CommentRepository &Comments, StructureWorkAccount *const Work = nullptr); // 埋込ドキュメントの後続文の所属の分離関数

	public:

		static std::vector<std::string> ReconcileLogicalLines(
			const NotationRules &Rules,
			const std::vector<std::string> &Existing,
			const Signature &FunctionSig,
			const std::optional<std::vector<DocItem>> &Items,
			const Signature *const Completion = nullptr
		); // 既存論理行とシグネチャからの論理行整合関数

		static void Synchronize(
			const SyntaxModel &Syntax,
			CommentRepository &Comments,
			size_t *const Work = nullptr,
			size_t *const InitialIndexWork = nullptr
		); // ドキュメントの同期

		static void ConfirmGroups(const LaidOutCode &Code, CommentRepository &Comments); // ドキュメントの位置が確定した後のグループの所属の確定関数

		DocCommentSynchronizer() = delete; // コンストラクタ（禁止）
	};
} // namespace Core
