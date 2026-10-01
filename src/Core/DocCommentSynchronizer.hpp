#pragma once

#include "../Util/LayoutTypes.hpp"
#include "CommentRepository.hpp"
#include "SignatureExtractor.hpp"
#include "SyntaxModel.hpp"
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Core {
	enum class DocItemKind : uint8_t { Lead, Other, Param, ParamList, Return, Summary }; // 文書の項目の種類（仮引数・戻値より前の其の他の項目，其の他，仮引数，仮引数の項目を並べる見出，戻値，概要）

	// 文書の項目（論理行の範囲）
	struct DocItem {
		size_t Begin; // 最初の論理行
		size_t End; // 最後の論理行の次
		DocItemKind Kind; // 種類
		std::string_view Name; // 仮引数名（仮引数の項目だけ）
	};

	// 文書化コメントの同期クラス（整形仕様 3.1, 7.4，関数の署名と既存の文書を突合し，コメントの記録だけを更新する，文書の本文の構造化は同期と静的検査で共有する）
	class DocCommentSynchronizer {
	private:

		// 説明対象を求める節点毎の表（記録・節点毎に祖先を辿らない為に一度の走査で求める，整形仕様 7.4, 9.2）
		struct TargetTables {
			std::vector<NodeId> Declarations; // 子から辿り着いた時の所属する宣言
			std::vector<uint8_t> IsInDefinitionBody; // 定義の本体の中か
			std::vector<NodeId> Definitions; // 文書化対象の定義の錨・包装と見出の中の節点毎の其の定義（見出の中は最も外の定義）
		};

		static void SkipSpace(std::string_view &View); // 先頭空白の読飛関数
		static void SkipBracketed(const char Open, const char Close, std::string_view &View); // 括弧範囲の読飛関数
		static size_t TokenLength(const std::string_view View); // 先頭トークン長の取得関数
		static bool ConsumeTagWord(const std::span<const std::string_view> Words, std::string_view &View); // 札の語の消費関数
		static bool ConsumeParamHeader(const NotationRules &Rules, std::string_view &View); // 仮引数の札の見出の消費関数
		static bool ConsumeReturnHeader(std::string_view &View); // 戻値の札の見出の消費関数
		static std::string_view ParamTagName(const NotationRules &Rules, const std::string_view LogicalLine); // 仮引数の札の引数名取得関数
		static bool IsReturnTagLine(const std::string_view LogicalLine); // 戻値タグ行の判定関数

		static std::string Label(
			const DocumentationStyle Dialect,
			const std::string_view Name,
			const bool IsParam,
			const std::string_view Word
		); // 項目を指す字面の組立関数

		static std::string_view ParentName(const std::string_view ItemName); // 仮引数の成員の項目の親の名前の取得関数
		static bool IsTypeParamItem(const std::string_view ItemName, const Signature &Sig); // 型仮引数の項目かの判定関数
		static std::string_view LeadingName(std::string_view View); // 先頭の名前の取得関数
		static std::string_view XmlAttribute(const std::string_view Tag, const std::string_view Attribute); // XML の要素の開始札の属性値の取得関数
		static void ScanXmlDepth(const std::string_view Line, int &Depth, bool &IsInCdata); // XML の要素の深さの更新関数
		static std::string_view Callout(const std::string_view Line, std::string_view &Rest); // 呼出の語の取得関数
		static std::string NewParamLine(const DocumentationStyle Dialect, const std::string_view Name); // 仮引数の新しい項目の論理行の生成関数
		static std::string_view NewReturnLine(const DocumentationStyle Dialect); // 戻値の新しい項目の論理行の取得関数
		static void NormalizeTagSynonym(const NotationRules &Rules, std::string &Line); // 同義の札の規約の綴りへの統一関数
		static bool IsMergeable(const CommentRecord &Comment); // 文書へ統合し得る独立コメントかの判定関数
		static std::string_view NewDocumentMarker(const NotationRules &Rules); // 新しい文書を書く行形式の記号の取得関数
		static bool IsDirectiveLine(const std::string_view Text); // 文書の本文から除く指示子の行かの判定関数
		static std::vector<std::pair<size_t, size_t>> LogicalLineRanges(const CommentRecord &Record); // 文書の記録の論理行の範囲の取得関数
		static std::optional<std::vector<bool>> CodeLineStarts(const NotationRules &Rules, const std::vector<std::string> &Lines); // 論理行の本文開始がコード内かの判定関数（閉じないコード区画を持つ文書は無し）

		static std::optional<std::vector<DocItem>> ItemsOf(
			const NotationRules &Rules,
			const std::vector<std::string> &Lines,
			const std::optional<std::vector<bool>> &CodeStarts,
			const Signature &Sig
		); // 論理行の本文開始のコード判定に従う項目への分割関数

		static size_t FirstItemLine(const NotationRules &Rules, const std::vector<std::string> &Lines, const Signature &Sig); // 説明の末尾の論理行の取得関数
		static bool IsTypeMemberSequence(const SyntaxModel &Syntax, const NodeId Node); // 型の成員の並びかの判定関数
		static bool DeclaresType(const SyntaxModel &Syntax, const NodeId Node); // 型を宣言する節点かの判定関数
		static NodeId DeclarationTargetOf(const SyntaxModel &Syntax, const NodeId Node, const bool IsAnchor, const NodeId Inherited); // 節点の所属する宣言の取得関数
		static bool EndsHeader(const SyntaxModel &Syntax, const NodeId Definition, const NodeId Child); // 定義の見出を終える子かの判定関数
		static TargetTables TablesOf(const SyntaxModel &Syntax); // 説明対象を求める節点毎の表の構築関数
		static NodeId DocumentTargetOf(const SyntaxModel &Syntax, const TargetTables &Tables, const NodeId Anchor); // 錨の文書の説明対象の取得関数

		static size_t MergeDescription(
			const NotationRules &Rules,
			const Signature &Sig,
			const std::vector<std::string> &Before,
			const std::vector<std::string> &After,
			std::vector<std::string> &Lines
		); // 定義の直前の通常コメントの文書の説明への統合関数

	public:

		static bool CoversParam(const std::string_view ItemName, const std::string_view Param); // 項目が仮引数を説明するかの判定関数
		static bool CoversAnyParam(const std::string_view ItemName, const Signature &Sig); // 項目が署名の仮引数・型仮引数・仮引数の成員を説明するかの判定関数
		static bool IsDocBlock(const NotationRules &Rules, const std::string_view Text); // 文書化コメントブロックの判定関数（`/**`/`/*!` 開始且つ区切コメントで無い）
		static size_t LastDocBlockIndex(const NotationRules &Rules, const std::span<const std::string_view> Texts); // 並びの中で文書とするブロックの位置の取得関数（無ければ要素数）
		static std::string_view DocLineMarker(const NotationRules &Rules, const std::string_view Text); // 行形式の文書化コメントの記号の取得関数（文書で無ければ空）
		static bool IsDocComment(const NotationRules &Rules, const std::string_view Text); // 文書化コメントの判定関数
		static bool IsDedicatedDocument(const NotationRules &Rules, const CommentRecord &Comment); // 通常コメントと区別する記法の独立した文書かの判定関数
		static std::vector<std::string> LogicalLines(const CommentRecord &Record); // 文書の記録の論理行の取得関数

		static std::optional<std::vector<DocItem>> ParseItems(
			const NotationRules &Rules,
			const std::vector<std::string> &Lines,
			const Signature &Sig
		); // 文書の論理行の項目への分割関数（同期出来ない文書は空）

		static bool HasItemDescription(const NotationRules &Rules, const DocItem &Item, const std::vector<std::string> &Lines); // 仮引数・戻値・失敗条件の項目の説明の有無の判定関数

		static bool HasSummary(
			const DocumentationStyle Dialect,
			const std::vector<std::string> &Lines,
			const std::vector<DocItem> &Items,
			const std::string_view Name
		); // 文書の概要の有無の判定関数

		static std::string ItemLabel(const DocumentationStyle Dialect, const std::vector<std::string> &Lines, const DocItem &Item); // 静的検査の通知で既存の項目を指す字面の取得関数
		static std::vector<uint8_t> DefinitionsOf(const SyntaxModel &Syntax); // 節点毎の文書を同期する定義かの取得関数
		static std::vector<NodeId> DocumentTargets(const SyntaxModel &Syntax, const CommentRepository &Comments); // 記録毎の文書の説明対象の取得関数
		static bool StartsLine(const LaidOutCode &Code, const NodeId Node); // 確定した配置で節点が行の最初の内容から始まるかの判定関数

		static std::vector<NodeId> PlacedTargets(
			const LaidOutCode &Code,
			const std::span<const CommentRecord> Records,
			std::vector<NodeId> Targets
		); // 確定した配置で文書として置く記録毎の説明対象の取得関数

		static std::string MissingLabel(const DocumentationStyle Dialect, const std::string_view Name); // 静的検査の通知で不足の項目を指す字面の取得関数

		static std::string_view FailureTitle(
			const DocumentationStyle Dialect,
			const std::vector<std::string> &Lines,
			const DocItem &Item
		); // 失敗条件の項目の語の取得関数（失敗条件の項目で無ければ空）

		static std::string FailureLabel(const DocumentationStyle Dialect, const std::string_view Title); // 静的検査の通知で失敗条件の項目を指す字面の組立関数

		static std::vector<std::string> ReconcileLogicalLines(
			const NotationRules &Rules,
			const std::vector<std::string> &Existing,
			const Signature &Sig
		); // 既存論理行とシグネチャからの論理行整合関数

		static void Synchronize(const SyntaxModel &Syntax, CommentRepository &Comments); // 文書化コメントの生成と補正関数
		static void ConfirmGroups(const LaidOutCode &Code, CommentRepository &Comments); // 文書の位置が確定した後の群の所属の確定関数

		DocCommentSynchronizer() = delete; // コンストラクタ（禁止）
	};
} // namespace Core
