#pragma once

#include "CommentRepository.hpp"
#include "NodeId.hpp"
#include "SignatureExtractor.hpp"
#include "SyntaxModel.hpp"
#include "SyntaxRole.hpp"
#include "../Util/LayoutTypes.hpp"
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
	enum class DocItemKind : uint8_t { Lead, NestedSig, Other, Param, ParamList, Return, Summary, TypeParam }; // ドキュメントの項目の種類 // 仮引数・戻値より前の其の他の項目 // 内側のシグネチャの項目 // 其の他 // 仮引数 // 仮引数の項目を並べる見出 // 戻値 // 概要 // 型仮引数

	// ドキュメントの項目（論理行と行内のバイト範囲）
	struct DocItem {
		size_t Begin; // 最初の論理行
		size_t End; // 最後の論理行の次
		DocItemKind Kind; // 種類
		std::string_view Name; // 比較用の仮引数名（通常又は型の仮引数の項目だけ，出力は元の論理行を使う）
		std::vector<std::string_view> Names {}; // カンマと前後の空白を除いた照合名（同期と静的解析で共有）
		std::string_view Parent {}; // メンバの説明の親名（其の他は空）
		uint32_t ParamOrdinal = 0; // JSDoc の説明名が表す分解の仮引数の番号（其の他は０）
		size_t BeginOffset = 0; // 最初の論理行内の開始位置
		size_t EndOffset = std::string_view::npos; // 最後の論理行内の終端位置（指定無は行末）
		size_t CloseLine = std::string_view::npos; // XML の閉タグの最初の論理行（其の他は指定無）
		size_t CloseOffset = 0; // XML の閉タグの行内の開始位置
		size_t OpenLine = std::string_view::npos; // XML の開始タグを終える論理行（其の他は指定無）
		size_t OpenOffset = 0; // XML の開始タグを終える `>` の行内の位置
		bool IsSelfClosing = false; // XML の自己終了要素か
		bool HasXmlDescription = false; // XML の属性と外側のタグを除いた内容の有無
		std::shared_ptr<const std::string> DecodedName {}; // 文字参照を持つ比較名の所有（項目の移動・コピー後も参照を保つ）
		std::shared_ptr<const DocItem> ReadingItem {}; // 展開後の値で確定した項目の範囲
		std::shared_ptr<const std::vector<std::string>> ReadingLines {}; // 比較名と説明を読む本文の所有
	};

	// ドキュメントの論理行の読取情報と元の綴りの対応
	struct DocReading {
		std::vector<bool> Code {}; // 展開後の論理行のコード判定
		std::vector<std::vector<unsigned char>> Protected {}; // 論理行内のコード例・引用値の保護範囲
		std::shared_ptr<const std::vector<std::string>> Lines {}; // 展開後の論理行（展開が無ければ元の行を読む）
		std::vector<std::vector<std::pair<size_t, size_t>>> OriginalPositions {}; // 各境界に対応する元の論理行と行内位置
		std::vector<size_t> GroupStarts {}; // 展開後のドキュメントの区間の開始
	};

	using DocParamIdx = std::optional<std::unordered_set<std::string_view>>; // 同じシグネチャの本文世代と静的解析で共有する仮引数名のインデックス（最初の必要時に構築）

	// ドキュメントの認識・論理行・項目・対象を読む共通判定（記録と構文を変更しない）
	class DocCommentReader {
	private:

		friend class DocCommentSynchronizer;
		friend struct DocCommentReaderAccess; // 所属の共有表の作業計数を観測する検証ツール

		// 説明対象を求めるノード毎の表（記録・ノード毎に祖先を辿らない為に１度の走査で求める，整形仕様 7.4, 9.2）
		using TargetTables = DocTargetTables; // コメント経路が共有する所属の表

		template<class Append> static void ForLogicalLines(const CommentRecord &Record, Append &&Write); // ドキュメントの論理行の範囲の訪問関数

		static std::string_view GetLogicalText(const CommentRecord &Record, size_t *const Work = nullptr); // 記法に従う論理行範囲の参照の取得関数

		static bool HasConsumedTagWord(
			const NotationRules &Rules,
			const std::span<const std::string_view> Words,
			std::string_view &View
		); // タグの語の消費関数

		static void SkipSpace(std::string_view &View); // 先頭空白の読飛関数
		static void SkipBracketed(const unsigned char Open, const unsigned char Close, std::string_view &View); // 括弧範囲の読飛関数
		static size_t TokenLen(const std::string_view View); // 先頭トークン長の取得関数
		static bool HasConsumedParamHeader(const NotationRules &Rules, std::string_view &View); // 仮引数のタグの見出の消費関数
		static std::string_view GetLineOf(const DocItem &Item, const std::vector<std::string> &Lines, const size_t Idx); // 項目の本文の取得関数
		static bool IsTypeMemberSeq(const SyntaxModel &Syntax, const NodeId Node); // 型のメンバの並びかの判定関数
		static NodeId GetDeclTargetOf(const SyntaxModel &Syntax, const NodeId Node, const bool IsAnchor, const NodeId Inherited); // ノードの所属する宣言の取得関数
		static bool IsHeaderEnd(const SyntaxModel &Syntax, const NodeId Def, const NodeId Child); // 定義の見出を終える子かの判定関数
		static std::string_view GetLeadingName(std::string_view View, size_t *const Consumed = nullptr); // 先頭の名前の取得関数
		static std::string_view Callout(const std::string_view Line, std::string_view &Rest); // Swift の説明項目のラベルの取得関数
		static std::string_view GetParamTagName(const NotationRules &Rules, const std::string_view LogicalLine); // 仮引数のタグの引数名取得関数
		static bool HasConsumedReturnHeader(const NotationRules &Rules, std::string_view &View); // 戻値のタグの見出の消費関数
		static bool IsTypeDecl(const SyntaxModel &Syntax, const NodeId Node); // 型を宣言するノードかの判定関数
		// ドキュメントの解析と整合の内部操作
		static std::string_view GetParentName(const std::string_view ItemName); // 仮引数のメンバの項目の親の名前の取得関数
		static std::optional<std::string> DecodeXmlName(const std::string_view Name); // XML 属性の文字参照をデコードした比較名の取得関数
		static std::string_view GetNewDocMarker(const NotationRules &Rules); // 新しいドキュメントを書く行形式の記号の取得関数
		static TargetTables TablesOf(const SyntaxModel &Syntax, size_t *const Work = nullptr); // 説明対象を求めるノード毎の表の構築関数
		static bool IsDirectiveBody(const std::string_view Body); // ドキュメントの本文から除くディレクティブの行の本文かの判定関数
		static bool IsMergeable(const CommentRecord &Comment); // ドキュメントへ統合し得る独立コメント又は同期する既存ドキュメントかの判定関数

		static bool IsCodeLine(
			const bool IsXmlDoc,
			const std::string_view Line,
			const std::span<const unsigned char> Protected,
			const std::span<const std::string_view> CodeElements
		); // 項目の解析から除くコード行かの判定関数

		static std::optional<std::vector<DocItem>> ItemsOf(
			const NotationRules &Rules,
			const std::vector<std::string> &Lines,
			const Signature &FunctionSig,
			const std::optional<DocReading> &CodeStarts,
			const std::span<const size_t> GroupStarts,
			DocParamIdx &ParamIdx
		); // 論理行の本文開始のコード判定に従う項目への分割関数

		static NodeId GetDocTargetOf(const SyntaxModel &Syntax, const TargetTables &Tables, const NodeId Anchor); // アンカーのドキュメントの説明対象の取得関数

	public:

		static bool IsDocBlock(const CommentRecord &Record); // 記録の出力がドキュメント専用の囲みの記法かの判定関数
		static std::string_view GetDocLineMarker(const CommentRecord &Record); // 記録の出力のドキュメントの行記号の取得関数
		static bool IsAtLineStart(const LaidOutCode &Code, const NodeId Node); // 確定した配置でノードが行の最初の内容から始まるかの判定関数
		static bool HasItemDescription(const NotationRules &Rules, const DocItem &Item, const std::vector<std::string> &Lines); // 仮引数・戻値・失敗条件の項目の説明の有無の判定関数
		static bool IsDoc(const CommentRecord &Record); // 記録の出力がドキュメントかの判定関数
		static void AppendLogicalLines(const CommentRecord &Record, std::vector<std::string> &Lines); // 記録の論理行を格納先へ直接追加する関数
		static void AppendLogicalLineRanges(const CommentRecord &Record, std::vector<std::pair<size_t, size_t>> &Out); // 論理行の範囲を共有の列へ追記する関数

		// ドキュメントの対象と構造の共通操作
		static std::vector<NodeId> GetDocTargets(
			const SyntaxModel &Syntax,
			const CommentRepository &Comments,
			std::optional<DocTargetTables> *const CollectedTables = nullptr,
			size_t *const Work = nullptr
		); // 記録毎のドキュメントの説明対象の取得関数

		static std::vector<NodeId> GetDocDefinitionTargets(
			const SyntaxModel &Syntax,
			const CommentRepository &Comments,
			size_t *const Work = nullptr
		); // 所属確定済の説明を関数の定義へ対応付ける取得関数

		static std::vector<NodeId> PlacedTargets(
			const LaidOutCode &Code,
			const std::span<const CommentRecord> Records,
			std::vector<NodeId> Targets
		); // 確定した配置でドキュメントとして置く記録毎の説明対象の取得関数

		// ドキュメントの解析の公開操作
		static std::optional<DocReading> CodeLineStarts(
			const NotationRules &Rules,
			const std::vector<std::string> &Lines,
			const CommentStructure *Previous = nullptr,
			const std::span<const size_t> GroupStarts = std::span<const size_t>()
		); // 項目の解析から除くコード行の判定関数（閉じないコード領域を持つドキュメントは無い）

		static bool HasSummary(
			const NotationRules &Rules,
			const std::vector<std::string> &Lines,
			const std::vector<DocItem> &Items,
			const std::string_view Name,
			size_t *const Work = nullptr
		); // ドキュメントの概要の有無の判定関数

		static bool IsDedicatedDoc(const NotationRules &Rules, const CommentRecord &Comment); // 通常コメントと区別する独立したドキュメントかの判定関数

		static std::optional<DocReading> DocCodeStarts(
			const CommentRecord &Record,
			const std::span<const unsigned char> Protected,
			const bool HasUnclosedCode
		); // 共有する本文構造からドキュメントの論理行のコード判定と保護範囲を取り出す関数

		static std::optional<std::vector<DocItem>> ParseItems(
			const NotationRules &Rules,
			const std::vector<std::string> &Lines,
			const Signature &FunctionSig,
			const std::optional<DocReading> &CodeStarts,
			DocParamIdx &ParamIdx,
			const std::span<const size_t> GroupStarts = std::span<const size_t>()
		); // ドキュメントの論理行の項目への分割関数（同期出来ないドキュメントは空）

		DocCommentReader() = delete; // コンストラクタ（禁止）
	};
} // namespace Core
