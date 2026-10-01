#pragma once

#include "../Util/Lang.hpp"
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Core {
	struct KindDefinition;
} // namespace Core

enum class MagicCommentScope : uint8_t { Header, CommentLine, Anywhere }; // 実行時設定のコメントを処理系が読む範囲（後の値ほど広い） // 最初の字句より前の注記だけ // コメントだけの行 // 行末のコメントも含む全て

// 実行時設定のコメントの登録語（名前は `Core::RegisteredLineForms` へ投影する）
struct MagicCommentWord {
	std::string_view Name; // 設定の名前（小文字，連結の字は `MagicCommentNameJoiners` の最後の字で表す）
	MagicCommentScope Scope; // 処理系が読む範囲
	std::vector<std::string_view> Values; // 有効な値（小文字，処理系は大小文字を問わない）
};

// 入力の字句・構文解析と投影だけが参照する文法の読取規則
struct ReadingRules {
	bool HasCommentDocumentContent = false; // コメントを文書の内容として保持する
	bool HasDirectiveHeader = false; // 頭部の指示コメントを処理系が読む
	bool HasSingleLineEncodingHeader = false; // 符号化宣言を原則として先頭行だけで読む
	bool HasConditionalSecondLineEncoding = false; // 先頭行のコメントを条件に２行目の符号化宣言を読む
	bool HasOutputCommentGroups = false; // 関数末尾のコメント群を出力の検査に使う
	bool HasToolchainCommentDirectives = false; // コメントの位置と対象宣言で処理系の指示を定める
	bool HasHeaderBuildConstraints = false; // 他の原始の頭部からもビルド制約を読む
	bool HasPositionDirectiveComments = false; // コメントで後続字句の位置を指定する
	bool HasHaltedData = false; // 実行停止の宣言より後を解釈しないデータとして持つ
	std::string_view HaltCallName = {}; // 実行停止を宣言する呼出の名前
	bool ReadsLoneCrAsNewline = true; // 単独の CR を行の終わりとして読む
	std::vector<std::string_view> UnicodeLineSeparators {}; // 改行として読む Unicode の区切りの字面
	bool HasOuterText = false; // コードの前後に逐語の地の文を持つ
	bool HasEmbeddedScripts = false; // タグの中に別の文法のコードを持つ
	bool HasJsxText = false; // JSX の本文を読める文法を使う
	bool HasAmbiguousHeaderDialect = false; // 共有拡張子で別の方言の宣言を持ち得る
	bool MayContainMarkupDocument = false; // 共有拡張子で XML 文書を持ち得る
	std::vector<std::string_view> ForeignDialectHeads {}; // 共有拡張子で対象外の方言を示す行頭の語
	std::vector<std::string_view> MarkupDocumentHeads {}; // 共有拡張子で対象外の文書を示す先頭の語
	Lang::ID HeaderPromotion = Lang::Unknown; // 共有ヘッダを読み直して選べる文法（選べなければ `Unknown`）
	bool MasksEscapedBackslashes = false; // 文字列の逆斜線を CSS の解析器が読み違える
	bool SelectorCommentActsAsSpace = false; // 選択子内のコメント其の物を空白として読む
	bool MayMisreadTemplateArguments = false; // 山括弧の式を型引数へ読み違え得る
	bool MayMisreadGenericCalls = false; // 総称呼出を二項式へ読み違え得る
	bool MayMisreadTemplateDeclarations = false; // 型引数を持つ型の宣言を比較の式文へ読み違え得る
	bool KeepsImportNamesTogether = false; // 取込の名前を途中で折らずに保つ
	bool MisreadsCommandArgumentBoundary = false; // 括弧無の呼出引数の境界を読み違える
	bool HasKeywordClosedBlocks = false; // 語で本体を閉じる塊を持つ
	bool MisreadsCasts = false; // 二項式を型変換へ読み違える
	bool HasIndentBlocks = false; // インデントで本体の範囲を定める
	bool HasLineSensitiveBoundary = false; // 字句間の改行で読みが変わり得る
	bool ReadsUnicodeSpaces = false; // 解析器が Unicode の空白（Zs の字）を字句の間の空白と読む（行頭・前置区切り・字句の間の判定に含める）
	bool ReadsByteOrderMarkAsSpace = false; // 字句間の U+FEFF を空白と読む
	bool CommentLineSeparatorsEndStatements = false; // コメント内の行区切りが文の境界に為り得る
	std::vector<std::string_view> CommentLineBreaks {}; // 解析器が読まず処理系が囲みコメントの中で行の区切と読む字面
	bool HasRestrictedLineWords = false; // 特定の語と後続の間に改行を置けない
	std::string_view RestrictedLineArrow = {}; // 改行を直前に置けない矢印の記法
	std::string_view LeadingBinaryOperatorChars = {}; // 行頭で前の式へ結び得る演算子の文字
	std::string_view ExpressionContinuationChars = {}; // 後続の式を要求する記号の文字
	bool HasLeadingBinaryOperators = false; // 行頭の二項演算子を前の式へ結ぶ
	bool HasShortVariableDeclarations = false; // 短い宣言で既存の束縛へ代入し得る
	bool HasCompositeHeaderExpression = false; // 複合リテラルが本体の開始と紛れる
	bool HasStructHeaderExpression = false; // 構造体の式が本体の開始と紛れる
	bool HasPrefixCallHeaderExpression = false; // 前置呼出の括弧が見出の本体と紛れる
	bool HasContextualTypeRepresentation = false; // 型の文脈で解析器の表現が変わる
	bool HasPreprocessorMacros = false; // 前処理のマクロに依り字句と宣言が変わる
	bool HasCommentAbsorbingDeclarations = false; // 直前のコメントを宣言の内容へ取り込む
	bool HasUnsupportedMarkupComments = false; // HTML 風コメントを解析器だけが受ける
	bool ScriptAcceptsMarkupComments = false; // 古典スクリプトの文法が HTML 風コメントを認める（モジュール等は認めない）
	std::string_view MarkupCommentOpen = {}; // 行中で始まる HTML 風コメントの開きの字面
	std::string_view MarkupCommentClose = {}; // 行頭で始まる HTML 風コメントの閉じの字面
	bool HasArrowChainMisreads = false; // 矢印関数の結合を解析器が読み違える
	std::vector<std::string_view> ReferenceDirectiveElements {}; // 三重スラッシュ指示子の登録要素
	std::string_view ReferenceDirectiveTerminators = {}; // 三重スラッシュ指示子の要素名の後に続く字
	std::string_view EncodingLineIndent = {}; // 条件付で読む２行目の符号化宣言の記号の前に置ける空白の字
	std::vector<MagicCommentWord> MagicCommentWords {}; // 実行時設定の登録語と其の有効な値・読まれる範囲
	std::string_view OutputFunctionPrefix = {}; // 出力を比べる関数名の接頭辞
	std::vector<std::string_view> OutputCommentWords {}; // 出力の注記の登録語
	const Core::KindDefinition *EmptyStatementDefinition = nullptr; // 空の文の生成定義
	std::unordered_map<std::string_view, const Core::KindDefinition *> HiddenLexemeKinds = {}; // 文法が子に出さない語を持つ節点の名前と其の語の種別
	bool MisreadsCallContinuation = false; // 改行後の呼出と文の境界を読み違える
	bool MisreadsSelectorCommentBoundary = false; // 選択子内のコメントと空白の境界を読み違える
	bool MisbindsConstructionCallee = false; // 解析器が実引数の無い構築の後の後置の式と型引数の前の単項演算子を構築の外へ結ぶ文法
	bool MisreadsSoftKeywordCalls = false; // 解析器が文脈上の語と同じ名前の呼出を其の語の文と読む文法
	bool MisbindsEmbeddedBranches = false; // 解析器が閉じタグで終える空の本体と其の後の枝を別の構文へ結ぶ文法
	bool MisbindsPostfixOperand = false; // 解析器が二項・前置の式の後の後置の式を式の全体へ結ぶ文法
	bool MisassociatesOperators = false; // 解析器が前置の演算子の範囲と二項演算の優先順位を標準と異なって結ぶ文法
	bool RecoveryRestructuresDocument = false; // 誤りの回復が文書の構造を組み直す（誤り区画の前後の境界を確定出来ない）文法
	bool AbsorbsVerbatimOnError = false; // 誤りの回復がヒアドキュメント・地の文等の逐語区画を取り込む文法
	std::string_view VerbatimStart = {}; // 誤り区画で逐語本文の開始を探す記法
	bool MisreadsCustomProperties = false; // 解析器がカスタムプロパティの値と関数の中のコメントを規則・選択子と読み違える文法
	bool AbsorbsRulesIntoValueComments = false; // 解析器が宣言の値の中の行コメントの後の行の選択子と波括弧を宣言へ取り込む文法
	bool MisreadsEmptyArguments = false; // 解析器が取込の空の実引数の括弧を誤りと読む文法
	std::string_view UrlFunctionHead = {}; // 関数内のコメントを誤読し得る値の開始記法
	std::string_view BooleanNameHeader = {}; // 真偽値の名前を使える様にする取込の見出名
	std::string_view ImportedForeignPackage = {}; // 取込前のコメントを前文として読む外部の包の見出名
	std::string_view CustomPropertyHead = {}; // カスタムプロパティの名前の開始記法
	std::vector<std::string_view> ModuleClauseWords {}; // 解析器が読めないモジュールの節の語
	std::string_view PropertySeparator = {}; // 宣言の名前と値を分ける記法
	bool MisreadsEscapedRegexEnd = false; // 解析器が逃がした `/` を正規表現の終わりと読む文法
	bool DropsTokensOnRecovery = false; // 解析器が誤りの回復で子の間の字句を落とす文法
	bool ToleratesTrailingCommaError = false; // 解析器が誤りとする並びの末尾のカンマを受理する記法
	bool AcceptsArrowOperand = false; // 解析器が処理系の拒む矢印関数の被演算子を誤りとしない文法
	bool AcceptsContinuationComment = false; // 解析器が処理系の拒む行継続の後のコメントを誤りとしない文法
	std::vector<std::string_view> StatementWords {}; // マクロの置換本体の先頭で文を始める語
	std::vector<std::string_view> ConditionDirectives {}; // 条件を開く前処理指令の名前
	std::string_view ConditionEndDirective = {}; // 条件を閉じる前処理指令の名前
	std::string_view DefinitionDirective = {}; // マクロを定義する前処理指令の名前
	std::string_view UndefinitionDirective = {}; // マクロの定義を取り消す前処理指令の字面
	std::string_view LinkageHead = {}; // 前処理条件を跨ぐ連結指定の先頭の語
	std::string_view LinkageOpen = {}; // 前処理条件を跨ぐ連結指定の開きの字面
	std::string_view LinkageClose = {}; // 前処理条件を跨ぐ連結指定の閉じの字面
	std::string_view CompactLinkageOpen = {}; // 空白を除いた連結指定の開きの字面
	std::string_view LinkageMarkerDirective = {}; // 連結指定を解析器へ読ませる印付指令の先頭
	std::vector<std::string_view> StringPrefixes {}; // 文字列の開きの前に置く接頭辞
	std::vector<std::string_view> RawStringPrefixes {}; // 区切りの括弧で囲み行継続を取消して読む生文字列の接頭辞
	std::vector<std::string_view> HashQuotePrefixes {}; // 井桁の並びで区切る生文字列の接頭辞（接頭辞の無い形は常に認める）
	std::vector<std::string_view> VerbatimStringPrefixes {}; // 行を跨げる逐語文字列の接頭辞
	std::vector<std::string_view> MultilineQuotes {}; // 行を跨げる文字列を開く引用符（同じ字の続く限りを引用符とし，同じ並びで閉じる）
	std::string_view VariadicParameter = {}; // 可変長マクロの仮引数を表す名前
	std::vector<std::string_view> HeaderNameDirectives {}; // 見出名を引数に取る前処理指令の名前
	std::vector<std::string_view> ParsedDirectives {}; // 引数を解析する前処理指令の名前（其の他の指令の引数とマクロの本体は生の本文）
	std::vector<std::string_view> ClauseWords {}; // 式を始めれない節・終端の語
	std::vector<std::string_view> SeparatingModifiers {}; // 後の改行で成員を区切る修飾語
	std::vector<std::string_view> JoiningModifiers {}; // 後の改行を跨いで次の成員へ掛かる修飾語
	std::vector<std::string_view> LineContinuingTokens {}; // 改行の後で前の式を続ける字句
	std::string_view IdentifierRawPrefix = {}; // 比較する識別子から除く生の接頭辞
	std::string_view IdentifierQuote = {}; // 比較する識別子を囲む引用記号
	std::string_view LabelDefinitionSuffix = {}; // ラベルの定義の字句の末尾に付く記号
	bool DecodesIdentifierEscapes = false; // 識別子の中の Unicode の逃避を字として読む
	bool RestrictsBasicIdentifierEscapes = false; // 識別子の逃避で基本文字を指せない
	bool IgnoresIdentifierFormatCharacters = false; // 識別子の比較で書式文字（一般カテゴリ `Cf`）を無視する
	std::string_view PreserveSpaceValue = {}; // 内部空白の保持を指定する属性値
	std::string_view NativeNamespaceValue = {}; // 同じ表示規則を持つ名前空間の属性値
	std::vector<std::string_view> EmbeddedScriptTypes {}; // 埋込の実行可能な内容の型
	std::vector<std::string_view> EmbeddedDataTypes {}; // 埋込の構造化された内容の型
	std::string_view RawTextEscapeElement = {}; // 逃避状態を持つ逐語要素の名前
	std::string_view RawTextEscapeOpen = {}; // 逐語要素の中で逃避状態を開く字面
	std::string_view RawTextEndOpen = {}; // 逐語要素の終了タグを開く字面
	std::string_view RawTextNameTerminators = {}; // 逐語要素のタグ名の後に続く区切りの字
	std::string_view RawTextPlainElement = {}; // 逃避状態を持たない逐語要素の名前
	std::string_view EmbeddedStyleType = {}; // 埋込の様式の内容の型
	std::string_view EmbeddedTypeAttribute = {}; // 埋込の内容の型を指定する属性の名前
	std::string_view AttributeValueEdges = {}; // 属性値の前後から除く引用符と空白の字
	Lang::ID EmbeddedScriptLanguage = Lang::Unknown; // 埋込の実行可能な内容の言語
	Lang::ID EmbeddedStyleLanguage = Lang::Unknown; // 埋込の様式の内容の言語
	Lang::ID EmbeddedDataLanguage = Lang::Unknown; // 埋込の構造化された内容の言語
	std::string_view StructuredDataTypePrefix = {}; // 構造化された内容の型の接頭辞
	std::string_view StructuredDataTypeSuffix = {}; // 構造化された内容の型の接尾辞
	std::unordered_set<std::string_view> ReassociatedOperators = {}; // 解析器の結合を補う対象として優先順位を確定した演算子
	std::unordered_map<std::string_view, std::string_view> OperatorStandIns = {}; // 解析器が利用者定義と読む標準の演算子を同じ種類の二項式として読ませる字面
	bool IsSpace(const uint32_t Point) const; // 字句の間の行内空白の判定関数
	bool IsJsxSpace(const uint32_t Point) const; // JSX 本文の行内空白の判定関数
};
