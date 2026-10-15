#pragma once

#include "InputNormalizer.hpp"
#include "Lang.hpp"
#include "ParsedTree.hpp"
#include "TextEdit.hpp"
#include "../Core/OwnedInputRoutes.hpp"
#include <tree_sitter/api.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

class LaidOutCode;

struct ParseInput;
struct ReadingRules;

// トークン化前の値と元の綴りの境界
struct LexemeReading {
	std::string Text; // 展開後の値
	std::vector<size_t> OriginalOffsets; // 展開後の各境界に対応する元のバイト位置
};

// パーサーが正しく読める表現へ可逆に変換するクラス（書方は変更しない）
class ParseAdapter {
private:

	using RepairNodeRange = std::pair<uint32_t, TSNode>; // 位置から始まる最も内側の名前付ノード
	using RepairNodeRanges = std::vector<RepairNodeRange>; // 原文順の名前付ノードの領域
	using ExprGroups = std::set<std::pair<uint32_t, uint32_t>>; // ネストを許す修復領域

	struct ChainParts; // 演算子連鎖の部品の前方宣言

	using ChainIdx = std::unordered_map<const void *, ChainParts>; // ノード毎の連鎖の部品

	// 優先順位に従う二項式の直接の部品
	struct BinaryParts {
		TSNode Left {}; // 左の被演算子
		TSNode Right {}; // 右の被演算子
		int Prec = 0; // 修復の対象として確定した優先順位（対象外は０）
		bool IsUnknown = false; // 優先順位の分からない利用者定義の演算子か
	};

	// 連鎖の部分式の本文の範囲
	struct OperandSpan {
		uint32_t Start = 0; // 始まり
		uint32_t End = 0; // 終わり
	};

	// 演算子連鎖の１つの部品
	struct ChainParts {
		TSNode Left {}; // 単項なら被演算子，二項なら左側
		TSNode Right {}; // 二項の右側
		int Prec = 0; // 演算子の優先順位
		size_t RootIdx = 0; // 所属する連鎖の根のインデックス
		bool IsPrefix = false; // 前置演算子か
		bool IsPostfix = false; // 最も右の被演算子へ結ぶ後置の式か
	};

	// 準備後と入力のコメント範囲の対応
	struct PreparedCommentRange {
		uint32_t Start; // 準備後のコメントの開始位置
		uint32_t InputStart; // 準備前のコメントの開始位置
		uint32_t InputEnd; // 準備前のコメントの終了位置
	};

	static RepairNodeRanges CollectRepairNodeRanges(const ParsedTree &Src); // 名前付ノードの領域の一括収集関数

	static void RecordPreparationEdit(
		const size_t Start,
		const size_t End,
		const size_t NewSize,
		std::vector<AppliedEdit> &Batches
	); // テキストの準備の長さの変更の記録関数

	static FailureReason GetTrialFailure(const ParsedTree &Src); // 試行解析の未完了理由の取得関数

	static size_t ReadTrailingDataMarker(
		const GrammarProfile &Profile,
		const std::string &Src,
		const bool CanStopAtMarker,
		std::optional<ParsedTree> &Trial,
		std::vector<std::pair<uint32_t, uint32_t>> &Parens,
		FailureReason &Reason
	); // データのマーカーと，パーサーが誤読する通常の式の範囲の取得関数

	static TSNode RepairNodeAt(const ParsedTree &Src, const uint32_t Pos, std::optional<RepairNodeRanges> &Ranges); // 領域に属する名前付ノードの取得関数
	static size_t GetCodeChildren(const ParsedTree &Src, const TSNode Node, std::span<TSNode> Children); // 構文の直接子を格納し，上限を超えれば上限＋１を返す関数

	static std::optional<bool> MaskLinkageGuards(
		const ReadingRules &Rules,
		const std::string &Src,
		std::string &Text,
		const std::span<const PreparedCommentRange> SourceComments,
		unsigned char &Marker,
		std::vector<EscapedComment> &Escaped,
		std::vector<AppliedEdit> &Batches,
		std::vector<LexemeRestoration> &Restorations
	); // 前処理の条件を跨ぐリンケージ指定の波括弧のマーカーへの置換関数

	static void CollectRubyOpenRange(
		ParsedTree &Src,
		const TSNode Node,
		std::optional<RepairNodeRanges> &Ranges,
		std::vector<TextEdit> &Folds,
		std::vector<LexemeRestoration> &Restorations
	); // Ruby の終端の無い範囲の次の行の結合関数

	static void CollectKotlinJointEdit(const ParsedTree &Src, const TSNode Node, std::vector<TextEdit> &Edits); // Kotlin の改行を跨ぐ継目の区切編集の収集関数
	static std::vector<TextEdit> CollectSwiftCommentEdits(const ParsedTree &Src, const std::string_view OperatorChars); // Swift の演算子に密着したコメントの編集の収集関数
	static BinaryParts GetBinaryOf(const ParsedTree &Src, const TSNode Node); // 二項式の部品の取得関数
	static FailureReason CollectMisboundChain(ParsedTree &Src, const TSNode Root, const ChainIdx &Parts, ExprGroups &Groups); // 演算子連鎖の簡約関数
	static void WrapExprs(ParsedTree &Src, const ExprGroups &Selected, std::vector<TextEdit> &Edits); // 解析用の括弧の一括適用関数

	static std::optional<std::string> PrepareCSrc(
		const ReadingRules &Rules,
		const std::string &Src,
		const std::span<const std::string_view> LineSplices,
		unsigned char &Marker,
		bool &HasStrayBackslash,
		bool &HasForeignDialect,
		std::vector<EscapedComment> &Escaped,
		std::vector<AppliedEdit> &Batches,
		std::vector<LexemeRestoration> &Restorations,
		std::vector<std::pair<uint32_t, uint32_t>> &DirectiveLines
	); // C / C++ の行継続とプリプロセッサディレクティブの整理関数

	static std::optional<size_t> FindUnquotedUrlEnd(
		const ReadingRules &Reading,
		const std::string_view Text,
		const size_t Pos,
		std::vector<std::pair<size_t, size_t>> *const Interpolations = nullptr
	); // 引用符の無い値の本文の終端と，必要なら補間の範囲の取得関数

	static size_t FindValueInterpolationEnd(const ReadingRules &Reading, const std::string_view Text, size_t Pos); // 引用とコメントを保護して値の補間の終端を読む関数
	static std::optional<std::string> PrepareCssSrc(const GrammarProfile &Profile, const std::string &Src, unsigned char &Marker); // CSS 系の行コメントの行境界と文字列のバックスラッシュの準備関数
	static std::optional<char32_t> ReadUnicodeEscape(const std::string_view Src, size_t &Pos); // トークン化前に展開する Unicode のエスケープの値の読取関数
	static bool HasEmptyArgTokens(const ParsedTree &Src, const TSNode Node); // 空の実引数の実トークンの判定関数
	static void RepairRubyCommandArgs(ParsedTree &Src); // Ruby の曖昧な実引数の読みの統一関数
	static void RepairJavaScriptArrowChains(ParsedTree &Src); // JS/TS のアロー関数を被演算子と読んだエラーの区切関数
	static FailureReason RepairJavaScriptJoints(ParsedTree &Src); // JS/TS の改行を跨ぐ継目の読みの統一関数
	static std::optional<uint32_t> GetMisreadDirectiveEnd(const ParsedTree &Src); // 論理行を越えた前処理の見出の終端の取得関数
	static void RepairCommentLineBreaks(ParsedTree &Src); // 改行を含むブロックコメントの行の区切の修復関数
	static void RepairKotlinSemicolons(ParsedTree &Src); // Kotlin の自動の `;` の修復関数
	static void CollectKotlinObjectEdit(const ParsedTree &Src, const TSNode Node, std::vector<TextEdit> &Edits); // Kotlin のオブジェクト本体の編集の収集関数
	static bool RepairKotlinJoints(ParsedTree &Src); // Kotlin の改行を跨ぐ継目の区切関数
	static void RepairKotlinObjectBodies(ParsedTree &Src); // Kotlin のオブジェクト式の本体をラムダと読んだエラーの読直関数
	static void RepairCSharpMisreadCasts(ParsedTree &Src); // C# のキャストと誤読した二項式の読みの統一関数
	static void RepairCssSelectorComments(ParsedTree &Src, const bool IsCommentSpace); // CSS / SCSS のセレクタのコメントの後の結合子の修復関数
	static FailureReason RepairSwiftMisreads(ParsedTree &Src); // Swift のパーサーの読違の補正関数
	static FailureReason RepairOperatorBindings(ParsedTree &Src); // 前置・二項・後置の演算子の結合の修復関数
	static void RepairConstructionBindings(ParsedTree &Src); // 構築の被呼出側と型引数の結合の修復関数
	static void RepairSoftKeywordCalls(ParsedTree &Src); // 文脈上の語と同じ名前の呼出を其の語の文と読んだエラーの読直関数

	static std::optional<std::string> PrepareSrc(
		const GrammarProfile &Profile,
		const std::string &Src,
		const bool HasDataBoundary,
		unsigned char &Marker,
		bool &HasStrayBackslash,
		bool &HasForeignDialect,
		std::vector<EscapedComment> &Escaped,
		std::vector<AppliedEdit> &Batches,
		std::vector<LexemeRestoration> &Restorations,
		std::vector<std::pair<uint32_t, uint32_t>> &DirectiveLines,
		FailureReason &Reason
	); // テキストの段の解析用の入力の準備関数

	static std::optional<uint32_t> LostHtmlElementNameAt(const ParsedTree &Src); // 開いた要素の名前をパーサーが失い閉タグを読み違えたかの判定関数
	static bool IsClassicScript(const ParsedTree &Src); // 古典スクリプトとして読むソースかの判定関数
	static bool HasOperatorMarkupComment(const ParsedTree &Src); // パーサーが演算子と読んだ HTML 風コメントの有無の判定関数

	static bool CanExpandJavaUnicodeEscapes(
		const ParsedTree &Src,
		std::string &Output,
		std::vector<AppliedEdit> &Batches,
		std::vector<LexemeRestoration> &Restorations,
		std::optional<ParsedTree> &Trial,
		FailureReason &Reason,
		std::optional<uint32_t> &IssueOffset
	); // Java のトークン化前 Unicode エスケープの展開関数

	static std::optional<std::string> EscapeRubyEmbeddedDoc(
		const ParsedTree &Src,
		unsigned char &Marker,
		std::vector<AppliedEdit> &Preparation
	); // Ruby の埋込ドキュメントの誤読の回避関数

	static void RecordMark(ParsedTree &Src, const unsigned char Marker, std::vector<EscapedComment> &&Escaped); // 解析の為に原文へ置いたマーカーと退避したコメントの記録関数
	static bool HasPreservedRawTextEnd(const ReadingRules &Rules, const std::string_view Content, const bool IsScript); // 埋込の中身が要素の終わりの直前迄を占めるかの判定関数
	static void RestoreLinkageGuards(const ReadingRules &Rules, std::string &Text, const unsigned char Marker); // マーカーへ置き換えた連結指定の波括弧の復元関数
	static void RepairPrefixTypeErrors(ParsedTree &Src); // 前置型の連続を誤読した解析結果の修復関数
	static void RepairArgumentErrors(ParsedTree &Src); // 空の実引数と閉括弧前の空白を誤読した解析結果の修復関数
	static void RepairOrdinalArguments(ParsedTree &Src); // 順位の係数・符号・整数を算術式と読む誤りの修復関数

	static std::string PrepareValueInterpolations(
		const GrammarProfile &Profile,
		const std::string &Text,
		std::vector<AppliedEdit> &Batches,
		std::vector<LexemeRestoration> &Restorations
	); // 値の補間の解析前の可逆な保護関数

	static void RepairSizedMemberInitializers(ParsedTree &Src); // 幅の定数式へ初期化子を取り込むメンバ宣言の読違の修復関数
	static void RepairColonPairs(ParsedTree &Src); // キー値のコロンを値の本文へ吸収するエラーの修復関数
	static void RepairValDecls(ParsedTree &Src); // 値の名前を型と読んだ比較・乗算の読みの修復関数
	static void NormalizeJsxText(ParsedTree &Src); // JSX の本文の意味付関数
	static FailureReason RepairBeforeProjection(ParsedTree &Src); // コメント分離前にパーサーの読違を修復する共通入口

	static ParseInput ReadPrepared(
		const GrammarProfile &Profile,
		const std::string &Src,
		const size_t DataStart,
		const bool HasFilePreamble,
		std::optional<ParsedTree> *const Trial
	); // 解析用の入力の読取の実装関数

public:

	using LiteralRange = std::pair<uint32_t, uint32_t>; // 逐語内容を保つ始端・終端の組
	using LiteralRangeList = std::vector<LiteralRange>; // 原文順の逐語保護領域

	// HTML に埋め込んだコードの中身（言語と境界を確定した物，整形仕様 4.4）
	struct EmbeddedContent {
		TSNode Element; // 埋込の要素
		TSNode RawText; // 中身のノード（中身が無ければ空）
		::Lang Lang; // 中身の言語と読みの目標
	};

	static std::optional<LexemeReading> ReadUnicodeEscapes(const std::string_view Text); // 翻訳段階の読取テキストと元の境界の取得関数
	static bool IsHaltCall(const ParsedTree &Src, const TSNode Stmt); // 実行を止める呼出の文かの判定関数

	static size_t GetHaltedDataStart(
		const GrammarProfile &Profile,
		const std::string &Src,
		std::optional<ParsedTree> &Trial,
		FailureReason &Reason
	); // コードの後の逐語データの開始位置と試行解析の未完了理由の取得関数

	static std::pair<uint32_t, uint32_t> GetOuterTextBounds(const ParsedTree &Src); // PHP の本体の範囲の取得関数
	static std::string DecodeMarks(const ParsedTree &Src, std::string Text); // 解析用のマーカーの本来の字へのデコード関数
	static ParseInput Read(const GrammarProfile &Profile, InputNormalizer::NormalizedInput &Input); // 正規化結果からの解析用の入力の読取関数
	static bool CanRepair(ParseInput &Input); // パーサーの読違の修復関数
	static std::optional<std::string> DecodeCommentEscapes(const std::string_view Text); // コメントのトークン検査用の Unicode エスケープの展開
	static bool HasEscapedLineBreak(const std::string_view Text); // トークン化前に改行へ展開する Unicode のエスケープを持つかの判定関数

	static std::optional<LiteralRangeList> LiteralRanges(
		const GrammarProfile &Profile,
		const std::string &Src,
		const bool ShouldIncludeBlockComments,
		std::optional<ParsedTree> &Trial,
		FailureReason &Reason
	); // 改行を値其の物として保つ範囲の収集関数

	static std::string DecodeLexeme(const ParsedTree &Src, std::string Text, const uint32_t Start, const bool IsCodeText = false); // トークンの復元関数
	static std::vector<std::pair<uint32_t, uint32_t>> MacroBodyTokens(const std::string_view Body, const bool IsHeaderName); // マクロの本体のトークンの範囲の取得関数
	static bool IsNullDirective(const ParsedTree &Src, const TSNode Node); // 論理トークンから確定した空のプリプロセッサディレクティブかの判定関数
	static bool IsLinkageGuard(const ParsedTree &Src, const TSNode Node); // マーカーへ置き換えたリンケージ指定の波括弧のディレクティブかの判定関数
	static bool IsEmptyCssCustomVal(const ParsedTree &Src, const TSNode Node); // CSS の空値を偽の値ノードと読む誤りの判定関数
	static bool HasPreservedEmbeddedEnd(const ParsedTree &Src, const EmbeddedContent &Embed); // 完成候補の埋込の終端の保持の判定関数
	static std::vector<EmbeddedContent> EmbeddedContents(const ParsedTree &Src); // ドキュメント順の埋込の中身の列挙関数
	static Core::OwnedRegion OwnEmbeddedOutput(std::shared_ptr<const LaidOutCode> Code); // 独立して整形した埋込の出力を置く所有領域の生成関数
	static bool IsEmptyArgError(const ParsedTree &Src, const TSNode Node, const TSNode Parent); // 実在する空引数の丸括弧を誤読したかの判定関数
	static ParseInput Adapt(const GrammarProfile &Profile, InputNormalizer::NormalizedInput &Normalized); // 解析用の入力の適応関数

	ParseAdapter() = delete; // コンストラクタ（禁止）
};

// 解析用の入力の適応の結果（整形仕様 3.3 の ParseInput，試行解析の結果を後の工程が再利用する）
struct ParseInput {
	// 整形する原文の参照
	struct FormattingSrc {
		std::string_view Text; // 入力側の正規化した原文
	};

	// 独立検証する完成候補の参照
	struct VerificationSrc {
		std::string_view Text; // 検証側の完成候補
	};

	Core::OwnedInputRoutes Owned {}; // 子のコード・コメント・検証期待を分離した投影入力
	ParseDecision Decision = ParseDecision::Continue; // 継続・見送・失敗の区分
	FailureReason Failure {}; // 適応が確定した見送・失敗の理由
	std::optional<uint32_t> IssueOffset {}; // 修復前の本文で得れた見送位置
	std::optional<ParsedTree> Src {}; // 適応した入力と其の解析結果
	const GrammarProfile *Profile = nullptr; // 確定した文法（共有ヘッダを選び直した場合も以後へ引き継ぐ）
	bool HasFilePreamble = true; // 親がデコード済の埋込内容でなく，ファイル先頭の宣言を読むか
	bool HasPreparedText = false; // テキストの準備で入力を変えたか（修復前の検証データの所有に使う）
	uint32_t PreparedBroken = 0; // 言語別の修復の前に壊れて居たノードの数（健全な入力は０）
	TSPoint PreparedFirstBroken {}; // 同じ修復前の木で得た最初の破損位置（破損が無ければ先頭）
	std::variant<FormattingSrc, VerificationSrc> Origin = FormattingSrc{}; // 投影するテキストの出所
	std::string_view GetSrcText() const; // 修復前の正規化したテキストの取得関数
};
