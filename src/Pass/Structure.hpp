#pragma once

#include "../Util/HtmlTag.hpp"
#include "../Util/Lang.hpp"
#include "../Util/TextEdit.hpp"
#include "../Util/TsSource.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// 構文木の構造化テキストを用いてソース全体を再構築する静的ユーティリティクラス
class StructurePass {
private:

	using NodeTextMap = std::unordered_map<const void *, std::string>; // 単一構文木内で一意な Node.id をキーにする節点別テキストキャッシュ型
	using ContainsStmtBlockMap = std::unordered_map<const void *, uint8_t>; // 文包含・コメントのみ・Kotlin 値式の判定を再利用する節点別ビットキャッシュ型

	enum class HtmlContext : uint8_t { Html, Figure, Text }; // HTML，空白を描画しない外来要素，空白を描画する外来文字の文脈
	enum class HtmlFlow : uint8_t { Inline, Block, Hidden, Edge }; // HTML の既定表示形式に依る子の流れの区分
	enum class HtmlBoundary : uint8_t { Tight, Space, Break, Verbatim }; // HTML の子境界に於ける描画空白と逐語間隙の区分
	static constexpr uint8_t ContainsBit = 0X1; // 文ブロック包含ビット
	static constexpr uint8_t ContainsComputedBit = 0X2; // 文ブロック包含算出済ビット
	static constexpr uint8_t OnlyCommentsBit = 0X4; // コメントのみビット
	static constexpr uint8_t OnlyCommentsComputedBit = 0X8; // コメントのみ算出済ビット
	static constexpr uint8_t KotlinArrowBit = 0X10; // Kotlin の `->` の後の値の if 式ビット
	static constexpr uint8_t MultilineVerbatimBit = 0X20; // 複数行の逐語の葉の包含ビット
	static constexpr uint8_t MultilineVerbatimComputedBit = 0X40; // 複数行の逐語の葉の包含算出済ビット
	static bool IsVerbatimLeaf(const std::string_view Type, const Lang Language); // 原文の儘写す葉の判定関数
	static void PrepareTemplateContexts(const TSSource &Src, const Lang Language); // 山括弧誤解析の一括判定関数
	static bool IsMisparsedTemplate(const TSSource &Src, const TSNode Node); // 山括弧誤解析判定関数
	static bool IsUnnamedToken(const bool IsNamed, const std::string_view Type, const std::initializer_list<std::string_view> Toks); // 無名トークン種別照合関数
	static bool EndsWithOpenRange(TSNode Node); // 開いた範囲演算子で終わるかの判定関数
	static bool StartsWithParenType(TSNode Node); // `(` 始まりの型の判定関数
	static bool ReadsAsCsProduct(const TSSource &Src, const TSNode PointerType); // C# の宣言式と誤読した乗算かの判定関数

	static bool NeedsGapBetween(
		const TSSource &Src,
		const TSNode Parent,
		const std::string_view ParentType,
		const TSNode Cur,
		const std::string_view CurType,
		const TSNode Next,
		const std::string_view NextType,
		const uint32_t CurIndex,
		const Lang Language
	); // 兄弟ノード間スペース有無決定関数

	static void PushGapEditIfNeeded(
		const TSSource &Src,
		const TSNode Parent,
		const std::string_view ParentType,
		const TSNode Prev,
		const std::string_view PrevType,
		const TSNode Next,
		const std::string_view NextType,
		const uint32_t PairIdx,
		const Lang Language,
		std::vector<TextEdit> &Edits
	); // 間隙置換エディット追記関数

	static void CollectInlineSpaceEdits(const TSSource &Src, const TSNode Node, const Lang Language, std::vector<TextEdit> &Edits); // 行内スペース収集関数
	static bool HoldsBrokenDirective(const TSSource &Src, const TSNode Node); // 解析に失敗した前処理指令を子に持つかの判定関数
	static bool HoldsLopsidedOperator(const TSSource &Src, const TSNode Node, const Lang Language); // 片側だけ空白の有る Swift の利用者定義演算子を持つかの判定関数
	static std::string CssUrlArgumentsText(const TSSource &Src, const TSNode Node); // CSS の `url(...)` の引数の並びの字面の取得関数
	static void AppendHtmlVerbatim(std::string &Out, const TSSource &Src, const TSNode Node, const HtmlVerbatim Kind); // HTML の逐語保持要素の組立関数
	static void TrimTrailingSpaces(std::string &Text); // 末尾空白削除関数
	static bool IsPreprocDirectiveAt(const std::string_view Raw, const size_t Pos); // プリプロセッサ指令判定関数
	static std::string BuildPreprocText(const std::string_view Raw); // プリプロセッサノードテキスト構築関数
	static bool IsKotlinImplicitCtor(const TSSource &Src, const TSNode Node, const std::string_view TypeView, const Lang Language); // Kotlin の暗黙の主構築子かの判定関数
	static bool HasSameByteRange(const TSSource &Src, const TSNode Lhs, const TSNode Rhs); // バイト範囲一致判定関数
	static HtmlContext HtmlContextOf(const TSSource &Src, const TSNode Container); // HTML の子の並びの文脈判定関数
	static HtmlFlow HtmlFlowOf(const TSSource &Src, const TSNode Child, const HtmlContext Context); // HTML の子の流れの区分判定関数
	static uint32_t HtmlContentEnd(const TSSource &Src, TSNode Node); // HTML の子の内容の終端取得関数
	static void CollectHtmlBoundaries(const TSSource &Src, const TSNode Container, std::vector<HtmlBoundary> &Out); // HTML の子の境界の区分算出関数
	static bool HasHiddenOpenParen(const TSSource &Src, const uint32_t From, const uint32_t To); // 隠れた開き括弧の判定関数
	static bool HasSkippedText(const TSSource &Src, const uint32_t From, const uint32_t To); // 解析器が読み飛ばした字句の判定関数
	static void AppendGapVerbatim(std::string &Out, const TSSource &Src, const TSNode Prev, const TSNode Next); // 間隙逐語複写追記関数

	static void AppendHtmlBoundary(
		std::string &Out,
		const TSSource &Src,
		const TSNode Prev,
		const TSNode Next,
		const HtmlBoundary Kind,
		const bool IsExpanded
	); // HTML の子の境界の区切り追記関数

	static void BuildFlatFromAST(const TSSource &Src, const TSNode Node, const Lang Language, std::string &Out); // １行テキスト生成関数
	static const std::string &GetFlatText(const TSSource &Src, const TSNode Node, const Lang Language, NodeTextMap &FlatByNode); // フラットテキストキャッシュ取得関数
	static std::string_view HeredocBodyText(const TSSource &Src, const TSNode Body); // ヒアドキュメント本体の本文の取得関数
	static bool AreAllNamedChildrenComments(const TSNode Node); // 全名前付子コメント判定関数
	static bool AreAllNamedChildrenCommentsCached(const TSNode Node, ContainsStmtBlockMap &Cache); // 全名前付子コメント判定関数（キャッシュ版）
	static uint32_t JsxContentCount(const TSSource &Src, const TSNode Node); // JSX 内容子ノード数取得関数
	static void MarkKotlinArrowBodies(const TSNode Top, const TSNode TopParent, ContainsStmtBlockMap &Cache); // Kotlin の `->` の後の値の if 式の一括判定関数
	static bool IsKotlinArrowBody(const TSNode Node, const ContainsStmtBlockMap &Cache); // Kotlin の `->` の後の値の if 式かの判定関数
	static bool ContainsStmtBlock(const TSSource &Src, const TSNode Node, const Lang Language, ContainsStmtBlockMap &Cache); // 展開要因包含判定関数
	static bool AppendHeredocBody(std::string &Dst, const TSSource &Src, const TSNode Child); // ヒアドキュメント本体追記関数

	static std::string TakeStructuredText(
		const TSSource &Src,
		const TSNode Node,
		const Lang Language,
		NodeTextMap &FlatByNode,
		NodeTextMap &StructuredByNode,
		ContainsStmtBlockMap &ContainsByNode
	); // 構造化テキストの取出・消費関数

	static std::string_view TrimmedView(const std::string &Text); // 前後空白除去ビュー取得関数

	static std::string BuildExpandedContainer(
		const TSSource &Src,
		const TSNode Node,
		const Lang Language,
		NodeTextMap &FlatByNode,
		NodeTextMap &StructuredByNode,
		ContainsStmtBlockMap &ContainsByNode
	); // 引数列・JSX 式等展開形式構築関数

	static TSNode TrackOpenHeredocs(uint32_t &Open, const TSSource &Src, const TSNode Node); // 本文を控えたヒアドキュメント開始の数の更新と最後の開始節点の取得関数
	static void AppendHeredocPendingSeparator(std::string &Base); // 本文を控えたヒアドキュメントの後の文の区切り追記関数
	static void AppendTrailingSeparator(std::string &Text, const TSSource &Src, const TSNode Node, const Lang Language); // ノード末尾区切り追記関数
	static bool HasMultilineVerbatimLeaf(const TSSource &Src, const TSNode Node, const Lang Language, ContainsStmtBlockMap &Cache); // 複数行の逐語の葉の包含判定関数

	static std::string CleanupLine(
		const TSSource &Src,
		const TSNode Node,
		const Lang Language,
		std::string Line,
		ContainsStmtBlockMap &ContainsByNode
	); // 構造化テキスト行整形関数

	static std::string BuildPreprocBlockText(
		const TSSource &Src,
		const TSNode Node,
		const Lang Language,
		NodeTextMap &FlatByNode,
		NodeTextMap &StructuredByNode,
		ContainsStmtBlockMap &ContainsByNode
	); // 条件付プリプロセッサブロック構造化整形関数

	static const std::string &GetStructuredText(
		const TSSource &Src,
		const TSNode Node,
		const Lang Language,
		NodeTextMap &FlatByNode,
		NodeTextMap &StructuredByNode,
		ContainsStmtBlockMap &ContainsByNode
	); // 構造化テキストメモ化取得関数

	static void AppendEndToken(std::string &Base, const bool IsMulti, const std::string_view Token); // 終端トークン追記関数
	static bool HasModifierBody(const TSNode Node); // Ruby の節本体に含まれる修飾子形式の判定関数
	static bool JoinsAcrossNewline(const Lang Language, const std::string_view Next); // 改行を跨いで前の文へ繋がる文の判定関数
	static bool IsPhpAltOpener(const TSNode Parent, const TSNode Colon); // PHP の代替構文の本体を開く `:` の判定関数
	static const char *PhpInlineHtmlGap(const TSSource &Src, const TSNode Prev, const TSNode Cur, const std::string_view Built); // PHP の地の文との境界の区切り決定関数
	static bool EndsWithPhpColonBlock(const TSNode Node); // PHP の代替構文の本体で終わるかの判定関数

	static std::string BuildStructuredCore(
		const TSSource &Src,
		const TSNode Node,
		const Lang Language,
		NodeTextMap &FlatByNode,
		NodeTextMap &StructuredByNode,
		ContainsStmtBlockMap &ContainsByNode
	); // 構造化テキストの本体の構築関数

	static std::string BuildStructuredText(
		const TSSource &Src,
		const TSNode Node,
		const Lang Language,
		NodeTextMap &FlatByNode,
		NodeTextMap &StructuredByNode,
		ContainsStmtBlockMap &ContainsByNode
	); // 構造化テキスト構築関数

public:

	static void FlattenBracketContinuations(TSSource &Src, const Lang Language); // 括弧内継続行平坦化関数
	static void NormalizeInlineSpaces(TSSource &Src, const Lang Language); // 行内トークン間スペース正規化関数
	static bool ShouldBreakAfterExpandedBrace(const std::string_view Text, const std::string_view Next, const Lang Language); // 展開閉じ括弧後の改行判定関数
	static std::vector<std::pair<std::string_view, std::string_view>> RubyExpressionShape(const TSSource &Text); // Ruby 式接続列の取得関数
	static bool HasBrokenGoContinuation(const TSSource &Text); // Go 式内の不正改行判定関数
	static bool NormalizeStructured(TSSource &Src, const Lang Language, std::string_view &SkipReason); // ソース全体構造化再構築関数
	static void ExpandPythonInlineBlocks(TSSource &Src); // Python の１行に並べた文の展開関数

	StructurePass() = delete; // コンストラクタ（禁止）
};
