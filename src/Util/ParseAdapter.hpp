#pragma once

#include "InputNormalizer.hpp"
#include "Lang.hpp"
#include "ParsedTree.hpp"
#include "TextEdit.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

struct ParseInput;
struct ReadingRules;

// 解析器が正しく読める表現へ可逆に変換するクラス（書き方は変更しない）
class ParseAdapter {
private:

	static std::optional<uint32_t> ReadUnicodeEscape(const std::string_view Source, size_t &Pos); // 字句化前に展開する Unicode の逃避の値の読取関数
	static bool KeepsRawTextEnd(const ReadingRules &Rules, const std::string_view Content, const bool IsScript); // 埋込の中身が要素の終わりの直前迄を占めるかの判定関数
	static bool IsClassicScript(const ParsedTree &Src); // 古典スクリプトとして読む原稿かの判定関数
	static bool HasOperatorMarkupComment(const ParsedTree &Src); // 解析器が演算子と読んだ HTML 風コメントの有無の判定関数

	static std::optional<bool> MaskLinkageGuards(
		const ReadingRules &Rules,
		const std::string &Source,
		std::string &Text,
		char &Marker,
		std::vector<EscapedComment> &Escaped
	); // 前処理の条件を跨ぐ `extern "C"` の波括弧の印への置換関数

	static std::optional<std::string> PrepareCSource(
		const ReadingRules &Rules,
		const std::string &Source,
		char &Marker,
		bool &HasStrayBackslash,
		std::vector<EscapedComment> &Escaped
	); // C / C++ の行継続と前処理指令の整理関数

	static void RestoreLinkageGuards(const ReadingRules &Rules, std::string &Text, const char Marker); // 印へ置き換えた連結指定の波括弧の復元関数

	static std::optional<std::string> MaskCssEscapedBackslashes(
		const std::string &Source,
		const bool HasSlashLineComment,
		char &Marker
	); // CSS の文字列の逃避した逆斜線の誤読の回避関数

	static bool LosesHtmlElementNames(const ParsedTree &Src); // 開いた要素の名前を解析器が失い閉じタグを読み違えたかの判定関数
	static void RepairCommentLineBreaks(ParsedTree &Src); // 改行を含む囲みコメントの行の区切の補修関数
	static void RepairCssSelectorComments(ParsedTree &Src, const bool CommentActsAsSpace); // CSS / SCSS の選択子のコメントの後の結合子の補修関数
	static void RepairEmptyArgumentErrors(ParsedTree &Src); // 空の実引数を誤りと読む解析結果の補修関数
	static void FoldRubyOpenRanges(ParsedTree &Src); // Ruby の終端の無い範囲の次の行の結合関数
	static void RepairRubyCommandArguments(ParsedTree &Src); // Ruby の曖昧な実引数の読みの統一関数
	static FailureReason RepairSwiftMisreads(ParsedTree &Src); // Swift の解析器の読違の補正関数
	static FailureReason RepairOperatorBindings(ParsedTree &Src); // 前置と二項の演算子の結合の補修関数
	static void RepairConstructionBindings(ParsedTree &Src); // 構築の被呼出側と型引数の結合の補修関数
	static void RepairSoftKeywordCalls(ParsedTree &Src); // 文脈上の語と同じ名前の呼出を其の語の文と読んだ誤りの読直関数
	static void RepairCSharpMisreadCasts(ParsedTree &Src); // C# のキャストと誤読した二項式の読みの統一関数
	static void RepairJsJoints(ParsedTree &Src); // JS/TS の改行を跨ぐ継ぎ目の読みの統一関数
	static void RepairJsArrowChains(ParsedTree &Src); // JS/TS の矢印関数を被演算子と読んだ誤りの区切り関数
	static void RepairKotlinObjectBodies(ParsedTree &Src); // Kotlin の物体式の本体をラムダと読んだ誤りの読直関数
	static void CollectKotlinJointEdit(const ParsedTree &Src, const TSNode Node, std::vector<TextEdit> &Edits); // Kotlin の改行を跨ぐ継ぎ目の区切り編集の収集関数
	static void RepairKotlinJoints(ParsedTree &Src); // Kotlin の改行を跨ぐ継ぎ目の区切り関数
	static void RepairKotlinSemicolons(ParsedTree &Src); // Kotlin の自動の `;` の補修関数
	static bool ExpandJavaUnicodeEscapes(const ParsedTree &Src, std::string &Out); // Java の字句化前 Unicode エスケープの展開関数
	static std::optional<std::string> EscapeRubyEmbeddedDocs(const ParsedTree &Src, char &Marker); // Ruby の埋込ドキュメントの誤読の回避関数

	static std::optional<std::string> PrepareSource(
		const GrammarProfile &Profile,
		const std::string &Source,
		char &Marker,
		bool &HasStrayBackslash,
		std::vector<EscapedComment> &Escaped
	); // 字面の段の解析用の入力の準備関数

	static void RecordMark(ParsedTree &Src, const char Marker, std::vector<EscapedComment> &&Escaped); // 解析の為に原文へ置いた印と退避したコメントの記録関数
	static void MarkJsxTextEdges(ParsedTree &Src); // JSX の本文の端の空白の読みの印付関数
	static FailureReason RepairBeforeProjection(ParsedTree &Src); // コメント分離前に解析器の読違を補修する共通入口

	static ParseInput ReadPrepared(
		const GrammarProfile &Profile,
		const std::string &Source,
		const size_t DataStart,
		std::optional<ParsedTree> *const Trial
	); // 解析用の入力の読取の実装関数

public:

	using ParseInput = ::ParseInput; // 適応した解析結果の型

	// HTML に埋め込んだコードの中身（言語と境界を確定した物，整形仕様 4.4）
	struct EmbeddedContent {
		TSNode Element; // 埋込の要素
		TSNode RawText; // 中身の節点（中身が無ければ空）
		Lang::ID Language; // 中身の言語
	};

	static bool HasEscapedLineBreak(const std::string_view Text); // 字句化前に改行へ展開する Unicode の逃避を持つかの判定関数

	static std::optional<std::vector<std::pair<uint32_t, uint32_t>>> LiteralRanges(
		const GrammarProfile &Profile,
		const std::string &Source,
		const bool ShouldIncludeBlockComments,
		FailureReason &Reason,
		std::optional<ParsedTree> &Trial
	); // 改行を値其の物として保つ範囲の収集関数

	static std::string DecodeMarks(const ParsedTree &Src, std::string Text); // 解析用の印の本来の字への復号関数
	static std::string DecodeLexeme(const ParsedTree &Src, std::string Text, const uint32_t Start); // 解析用の印の本来の字句への復号関数
	static std::string DecodeJsxBlanks(const ParsedTree &Src, std::string Text); // JSX の本文の端の空白・タブの印の復号関数
	static std::vector<std::pair<uint32_t, uint32_t>> MacroBodyTokens(const std::string_view Body, const bool IsHeaderName); // マクロの本体の字句の範囲の取得関数
	static bool IsLinkageGuard(const ParsedTree &Src, const TSNode Node); // 印へ置き換えた `extern "C"` の波括弧の指令かの判定関数
	static bool IsHaltCall(const ParsedTree &Src, const TSNode Stmt); // 実行を止める呼出の文かの判定関数
	static size_t HaltedDataStart(const GrammarProfile &Profile, const std::string &Source, std::optional<ParsedTree> &Trial); // 実行停止後のデータの開始位置の取得関数
	static std::pair<uint32_t, uint32_t> OuterTextBounds(const ParsedTree &Src); // PHP の本体の範囲の取得関数
	static bool IsEmptyCssCustomValue(const ParsedTree &Src, const TSNode Node); // CSS の空値を偽の数値節点と読む誤りの判定関数
	static std::vector<EmbeddedContent> EmbeddedContents(const ParsedTree &Src); // 文書順の埋込の中身の列挙関数

	static std::shared_ptr<const Core::OwnedRegion> OwnEmbeddedOutput(
		ParsedTree &Src,
		const EmbeddedContent &Embed,
		std::string Output,
		std::string CodeText,
		std::vector<uint32_t> &&CodeVerbatimLines,
		std::vector<uint32_t> &&VerbatimLines
	); // 独立して整形した埋込の出力の所有区画としての控え関数

	static ParseInput Read(const GrammarProfile &Profile, InputNormalizer::NormalizedInput &Input); // 正規化結果からの解析用の入力の読取関数
	static bool Repair(ParseInput &Input); // 解析器の読違の補修関数
	static ParseInput Adapt(const GrammarProfile &Profile, InputNormalizer::NormalizedInput &Normalized); // 解析用の入力の適応関数

	ParseAdapter() = delete; // コンストラクタ（禁止）
};

// 解析用の入力の適応の結果（整形仕様 3.3 の ParseInput，試験解析の結果を後の工程が再利用する）
struct ParseInput {
	struct FormattingSource {
		std::string_view Text; // 入力側の正規化した原文
	};

	struct VerificationSource {
		std::string_view Text; // 検証側の完成候補
		std::string_view BeforeEdits; // 補修前の解析用の本文
		std::string_view BeforeText; // 補修前の解析用の字面全体
		std::span<const std::pair<uint32_t, uint32_t>> Comments; // 補修前のコメント範囲
	};

	ParseDecision Decision = ParseDecision::Continue; // 継続・見送・失敗の区分
	FailureReason Failure; // 適応が確定した見送・失敗の理由
	std::optional<ParsedTree> Src; // 適応した入力と其の解析結果
	const GrammarProfile *Profile = nullptr; // 確定した文法（共有ヘッダを選び直した場合も以後へ引き継ぐ）
	uint32_t PreparedBroken = 0; // 言語別の補修の前に壊れて居た節点の数（健全な入力は０）
	std::variant<FormattingSource, VerificationSource> Source = FormattingSource{}; // 投影する字面の出所と補修前の本文
	std::string_view SourceText() const; // 補修前の正規化した字面の取得関数
};
