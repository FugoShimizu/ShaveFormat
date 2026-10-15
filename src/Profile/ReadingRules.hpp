#pragma once

#include "../Core/SyntaxRole.hpp"
#include "../Util/Lang.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

enum class BoolNameSource : uint8_t { Literal, Macro, Shadow }; // 真偽値名の判定の供給元 // 予約されたリテラル // 前処理のマクロ // 有効範囲の遮蔽
enum class MagicCommentScope : uint8_t { Header, CommentLine, Anywhere }; // 実行時設定のコメントを処理系が読む範囲（後の値程広い） // 最初のトークンより前の注記だけ // コメントだけの行 // 行末のコメントも含む全て

// 実行時設定のコメントの登録語（名前と有効な値は `Core::RegisteredLineForms` へ投影する）
struct MagicCommentWord {
	std::string_view Name; // 設定の名前（小文字，連結の字は `MagicCommentNameJoiners` の最後の字で表す）
	MagicCommentScope Scope; // 処理系が読む範囲
	std::vector<std::string_view> Vals; // 有効な値（小文字，処理系は大小文字を問わない）
};

enum class LiteralDelimiter : uint8_t { Fixed, LineWord, Paired, Repeated }; // 区切の種類（先頭は既定値） // 固定長 // 行頭語 // 対応１文字 // 連続数

// 引用又は専用区切毎の入口の読取事実
struct LiteralReading {
	std::string_view Open; // 開きの固定のテキスト（長い開きを先に登録する）
	std::string_view Close; // 固定の閉じ・対応区切の閉列又は行頭の語を囲む引用符
	std::string_view PairedOpen = {}; // Close と同じ順の対応する開列
	std::string_view Escape = "\\"; // 次の１文字を逃がす記法（空なら逃がさない）
	std::vector<std::string_view> InterpolationHeads = {}; // 本文で補間を始めるテキスト
	std::string_view InterpolationPrefixes = {}; // 補間を有効にする接頭辞の文字（空なら常に有効）
	bool HasDoubledInterpolationEscape = false; // 補間の開きを二重に書くと本文として読むか
	bool HasRepeatedInterpolationHead = false; // 接頭辞の文字数で補間の開きと閉じの長さを定めるか
	std::string_view RepeatedInterpolationPrefix = {}; // 連続数で補間の開きを定める接頭辞
	std::string_view UnescapedInterpolationChars = {}; // エスケープ記号を前置しても補間を始める字
	LiteralDelimiter Delimiter = LiteralDelimiter::Fixed; // 閉じを定める方法
	bool CanShareOperatorHead = false; // 開きが演算子とも読める為，値の開始位置でだけ照合するか
	bool CanCrossLine = false; // 物理行を跨げるか
	bool CanNest = false; // 対応区切の中で同じ開きをネスト出来るか
	bool CanSpaceBeforeWord = false; // 開きと終端の語の間へ行内空白を置けるか
	bool CanIndentEnd = false; // 行頭の終端の前へ空白を置けるか
	std::string_view EndFollowers = "\r\n"; // 行頭の終端の語の直後に置ける字（入力末尾も可）
};

enum class PrefixTypePart : uint8_t { None, Operand, Operator, Prefix, Weak, Wrapper }; // 誤読した前置型の修復時の役割 // 対象外 // 被演算子 // 外側の前置語 // 内側の前置語 // 結合の弱い型 // 内側の前置型

// 自動の文終端を挿入するトークンと，区切を受ける構文の入力側の定義
struct AutomaticTerminatorReading {
	bool IsHost = false; // 直接子の間と末尾へ文終端を置けるか
	bool IsToken = false; // 後の改行で文終端を挿入するトークンか
	bool IsWholeToken = false; // 内部の子を別のトークンとして読まないリテラルか
};

enum class ShebangArgument : uint8_t { None, Optional, Required }; // 起動オプションの引数の読取 // 引数無し // 同じ語の残りだけ // 同じ語の残り又は次の語

struct ShebangOptionReading {
	std::string_view Name; // 起動オプションの名前
	ShebangArgument Argument; // 引数を消費する条件
	std::string_view Separators = {}; // 長い名前の引数又は制限の無い引数を始める字
	std::string_view ArgumentChars = {}; // 同じ語で個数を制限して読む引数の字（空なら残りの全体）
	uint8_t ArgumentLimit = 0; // 同じ語から読む引数の字の最大数
};

// 入力のトークン・構文解析と投影だけが参照する文法の読取規則
struct ReadingRules {
	std::vector<PrefixTypePart> PrefixTypes; // 前置型の誤読を修復する文法の記号毎の役割（空なら修復しない）
	std::string_view MisparsedCmpLeadChars = {}; // 読違えた比較の断片に続く字の候補
	std::vector<std::string_view> LineCommentCodeHeads {}; // 行コメントの記号で始まるコードの接頭辞（空なら無）
	bool HasCommentDocContent = false; // コメントをドキュメントの内容として保持する
	bool HasDirectiveHeader = false; // 頭部のディレクティブコメントを処理系が読む
	std::vector<std::string_view> AllowedEncodingNames; // ASCII の値を保つ状態の無い入力エンコーディングの名前（空なら宣言の値で制限しない）
	std::string_view BomEncodingName; // UTF-8 の BOM と共存出来る宣言の正準名（空なら BOM との組で制限しない）
	std::string_view EncodingNameJoiners; // 宣言の名前でハイフンへ揃える字（空なら大小文字だけを揃える）
	std::vector<std::pair<std::string_view, std::string_view>> EncodingNamePrefixes; // 接尾辞を含む宣言の名前を処理系の正準名へ対応させる接頭部
	std::vector<std::pair<std::string_view, std::vector<std::string_view>>> EncodingLabels; // 正準名と Encoding Standard のラベル（空ならラベルの照合を行わない）
	size_t EncodingPrescanBytes = 0; // バイト列の宣言を読む先頭の範囲（零なら行コメントだけを読む）
	bool HasMarkupEncodingPrescan = false; // タグと属性を走査して先頭のエンコーディング宣言を読むか
	std::string_view EncodingElementName; // 先頭の範囲でエンコーディングを指定する要素の名前
	std::string_view EncodingCharsetAttribute; // エンコーディングのラベルを直接指定する属性の名前
	std::string_view EncodingContentAttribute; // メディア型と共にエンコーディングを指定する属性の名前
	std::vector<std::pair<std::string_view, std::string_view>> EncodingPrescanReplacements; // ASCII の宣言から得たエンコーディングを実際の読みに対応させる組
	std::vector<std::pair<std::string_view, std::string_view>> MetaEncodingReplacements; // meta 要素だけに適用するエンコーディングの対応
	std::string_view EncodingBytePrefix; // 厳密なバイト列で宣言を識別する先頭記法
	std::string_view EncodingByteSuffix; // 厳密な宣言のラベルを閉じるバイト列
	bool HasSingleLineEncodingHeader = false; // エンコーディング宣言を原則として先頭行だけで読む
	bool HasConditionalSecondLineEncoding = false; // 先頭行のコメントを条件に２行目のエンコーディング宣言を読む
	std::string_view ShebangInterpreterName; // シバンの起動オプションを読む処理系の名前（空なら読まない）
	char ShebangEncodingOption = '\0'; // 原文のエンコーディングを指定する起動オプション
	std::vector<std::pair<char, std::string_view>> ShebangEncodingNames; // 起動オプションの１文字の値とエンコーディングの正準名
	std::string_view ShebangFlagOptions; // 同じ語の後続オプションへ進む引数無しの名前
	std::vector<ShebangOptionReading> ShebangOptions; // 原文のエンコーディングの指定を探す時に消費する他の起動オプション
	bool HasOutputCommentGroups = false; // 関数末尾のコメントグループを出力の検査に使う
	bool HasToolchainCommentDirectives = false; // コメントの位置と対象宣言で処理系の指示を定める
	bool HasHeaderBuildConstraints = false; // 他の原始の頭部からもビルド制約を読む
	bool HasPosDirectiveComments = false; // コメントで後続トークンの位置を指定する
	bool HasHaltedData = false; // 実行停止の宣言より後を解釈しないデータとして持つ
	std::string_view HaltCallName = {}; // 実行停止を宣言する呼出の名前
	std::string_view TrailingDataMarker = {}; // 行頭単独のマーカーの直後から始まる逐語データ（空なら無）
	bool ShouldReadLoneCrAsNewline = true; // 単独の CR を行の終わりとして読む
	std::vector<std::string_view> UnicodeLineSeparators {}; // 改行として読む Unicode の区切のテキスト
	bool HasOuterText = false; // コードの前後に逐語の地の文を持つ
	bool HasLimitedElementNameSerialization = false; // tree-sitter-html 0.23.2 が開いた要素名のシリアライズに上限を持つか
	bool CanMisreadCssLineComments = false; // 素の CSS の連続する斜線を SCSS の行コメントと読むか
	std::vector<std::string_view> OrdinalSelectorNames; // 算術式でなく係数・符号・整数の組を引数に取るセレクタの名前
	std::string_view ValueInterpolationOpen; // 値の位置でパーサーが誤読する補間の開始記号
	std::string_view ValueInterpolationJoinChars; // 語の字の他に値の補間と密着する単位・符号の字
	std::string_view ValueInterpolationClose; // 値の補間の終了記号
	std::string_view ColonPairStandIn; // キー値のコロンを読む為の解析用の二項演算子（空なら置換しない）
	std::vector<std::string_view> ColonPairValueHeads; // 値のマップを取り得る文の見出
	bool HasEmbeddedScripts = false; // タグの中に別の文法のコードを持つ
	bool HasJsxText = false; // JSX の本文を読める文法を使う
	bool HasAmbiguousHeaderDialect = false; // 共有拡張子で別の方言の宣言を持ち得る
	bool CanContainMarkupDoc = false; // 共有拡張子で XML ドキュメントを持ち得る
	std::vector<std::string_view> ForeignDialectHeads {}; // 共有拡張子で対象外の方言を示す行頭の語
	std::vector<std::string_view> MarkupDocHeads {}; // 共有拡張子で対象外のドキュメントを示す先頭の語
	std::string_view MarkupDocRoot = {}; // 宣言を省略出来る対象外のドキュメントの根要素
	std::string_view MarkupDocAttribute = {}; // 根要素をドキュメントとして識別する属性
	bool ShouldNormalizeLineCommentBreaks = false; // パーサーが本文と誤読する行コメントの１バイトの行境界を LF へ揃える
	bool ShouldMaskEscapedBackslashes = false; // 文字列のバックスラッシュを CSS のパーサーが読み違える
	bool CanSelectorCommentActAsSpace = false; // セレクタ内のコメント其の物を空白として読む
	bool CanMisreadTemplateArgs = false; // 山括弧の式を型引数へ読み違え得る
	bool CanMisreadGenericCalls = false; // ジェネリック呼出を二項式へ読み違え得る
	bool CanMisreadTemplateDecls = false; // 型引数を持つ型の宣言を比較の式文へ読み違え得る
	bool CanMisreadCommandArgBoundary = false; // 括弧無の呼出引数の境界を読み違える
	bool HasKeywordClosedBlocks = false; // 語で本体を閉じるブロックを持つ
	bool CanMisreadCasts = false; // 二項式を型変換へ読み違える
	bool HasIndentBlocks = false; // インデントで本体の範囲を定める
	uint8_t SourceIndentTabWidth = 0; // 本体の所属を読む原文のタブの幅
	std::string_view IndentResetChars {}; // 原文のインデントを零へ戻す空白の字
	bool HasLineSensitiveBoundary = false; // トークン間の改行で読みが変わり得る
	bool CanReadUnicodeSpaces = false; // パーサーが Unicode の空白（Zs の字）をトークンの間の空白と読む（行頭・前置区切・トークンの間の判定に含める）
	bool ShouldRejectExtraLeadingBom = false; // 最初の BOM に続く U+FEFF をソースのコードとして受理しない
	bool CanReadByteOrderMarkAsSpace = false; // トークン間の U+FEFF を空白と読む
	bool CanCommentLineSeparatorsEndStmts = false; // コメント内の行区切が文の境界に為り得る
	std::vector<std::string_view> CommentLineBreaks {}; // パーサーが読まず処理系がブロックコメントの中で行の区切と読むテキスト
	bool HasRestrictedLineWords = false; // 特定の語と後続の間に改行を置けない
	std::string_view RestrictedLineArrow = {}; // 改行を直前に置けない矢印の記法
	std::string_view LeadingBinaryOperatorChars = {}; // 行頭で前の式へ結び得る演算子の文字
	std::string_view ExprContinuationChars = {}; // 後続の式を要求する記号の文字
	bool HasLeadingBinaryOperators = false; // 行頭の二項演算子を前の式へ結ぶ
	bool HasShortVarDecls = false; // 短い宣言で既存の束縛へ代入し得る
	bool HasCompositeHeaderExpr = false; // 複合リテラルが本体の開始と紛れる
	bool HasStructHeaderExpr = false; // 構造体の式が本体の開始と紛れる
	bool HasPrefixCallHeaderExpr = false; // 前置呼出の括弧が見出の本体と紛れる
	bool HasContextualTypeRepresentation = false; // 型の文脈でパーサーの表現が変わる
	bool ShouldRequireFunctionDeclarator = false; // 関数本体の前に関数宣言子又は其れを供給するマクロを要求するか
	bool HasPreprocMacros = false; // 前処理のマクロに依りトークンと宣言が変わる
	BoolNameSource BoolNames = BoolNameSource::Literal; // 真偽値名の同一性を読む供給元
	std::vector<Core::Role> ParenthesizedDeclOwners {}; // 括弧付宣言を許す所有構文（空は所有構文を限定しない）
	std::unordered_set<std::string_view> PredefinedMacroNames {}; // 翻訳開始時に定義済の標準マクロ
	std::unordered_set<std::string_view> ConditionalMacroNames {}; // バージョン又は処理系に依り定義される標準マクロ
	bool HasCommentAbsorbingDecls = false; // 直前のコメントを宣言の内容へ取り込む
	bool HasUnsupportedMarkupComments = false; // HTML 風コメントをパーサーだけが受ける
	Lang::SrcGoal SrcGoal = Lang::SrcGoal::Undetermined; // ソースの読みの目標
	bool CanScriptAcceptMarkupComments = false; // 古典スクリプトの文法が HTML 風コメントを認める（モジュール等は認めない）
	std::string_view MarkupCommentOpen = {}; // 行中で始まる HTML 風コメントの開きのテキスト
	std::string_view MarkupCommentClose = {}; // 行頭で始まる HTML 風コメントの閉じのテキスト
	bool HasArrowChainMisreads = false; // アロー関数の結合をパーサーが読み違える
	std::vector<std::string_view> RefDirectiveElements {}; // 三重スラッシュディレクティブの登録要素
	std::string_view RefDirectiveTerminators = {}; // 三重スラッシュディレクティブの要素名の後に続く字
	std::string_view EncodingLineIndent = {}; // 条件付で読む２行目のエンコーディング宣言の記号の前に置ける空白の字
	std::vector<MagicCommentWord> MagicCommentWords {}; // 実行時設定の登録語と其の有効な値・読まれる範囲
	std::string_view OutputFunctionPrefix = {}; // 出力を比べる関数名の接頭辞
	std::vector<Core::ChildRole> OutputFunctionEmptyFields; // 出力を比べる関数で空でなければ為らないシグネチャの場
	std::span<const uint64_t> OutputFunctionExcludedInitialRanges; // 接頭辞の直後へ置けない文字種の区間
	std::vector<AutomaticTerminatorReading> AutomaticTerminators; // 文法の記号毎の自動の文終端の成立条件（空なら無）
	std::vector<std::string_view> OutputCommentWords {}; // 出力の注記の登録語
	Core::FormPart EmptyStmtDef {}; // 空の文の生成定義
	std::unordered_map<std::string_view, const Core::KindDef *> HiddenLexemeKinds = {}; // 文法が子に出さない語を持つノードの名前と其の語の種別
	bool CanMisreadCallContinuation = false; // 改行後の呼出と文の境界を読み違える
	bool CanMisreadSelectorCommentBoundary = false; // セレクタ内のコメントと空白の境界を読み違える
	bool CanMisbindConstructionCallee = false; // パーサーが実引数の無い構築の後の後置の式と型引数の前の単項演算子を構築の外へ結ぶ文法
	bool CanMisreadSoftKeywordCalls = false; // パーサーが文脈上の語と同じ名前の呼出を其の語の文と読む文法
	std::string_view SoftKeywordCallHead = std::string_view(); // 修復候補の文の先頭の語（空なら本文で候補を絞らない）
	bool DoesMisbindEmbeddedBranches = false; // パーサーが閉タグで終える空の本体と其の後の枝を別の構文へ結ぶ文法
	bool DoesMisbindPostfixOperand = false; // パーサーが二項・前置の式の後の後置の式を式の全体へ結ぶ文法
	bool CanMisassociateOperators = false; // パーサーが前置の演算子の範囲と二項演算の優先順位を標準と異なって結ぶ文法
	bool CanRecoveryRestructureDoc = false; // エラーの回復がドキュメントの構造を組み直す（エラー領域の前後の境界を確定出来ない）文法
	bool CanAbsorbVerbatimOnError = false; // エラーの回復がヒアドキュメント・地の文等の逐語領域を取り込む文法
	std::string_view VerbatimStart = {}; // エラー領域で逐語本文の開始を探す記法
	bool CanMisreadCustomProperties = false; // パーサーがカスタムプロパティの値と関数の中のコメントを規則・セレクタと読み違える文法
	bool CanAbsorbRulesIntoValComments = false; // パーサーが宣言の値の中の行コメントの後の行のセレクタと波括弧を宣言へ取り込む文法
	bool CanMisreadRuleArgs = false; // パーサーが規則の空の実引数又は閉括弧前の空白をエラーと読む文法
	std::string_view UrlFunctionHead = {}; // 関数内のコメントを誤読し得る値の開始記法
	std::string_view BoolNameHeader = {}; // 真偽値の名前を使える様にするインクルードのヘッダ名
	std::string_view ImportedForeignPackage = {}; // インポート前のコメントを前文として読む外部のパッケージのインポートパス
	std::string_view CustomPropertyHead = {}; // カスタムプロパティの名前の開始記法
	std::string_view NullDirectivePlaceholder = {}; // 空のディレクティブをパーサーへ渡す置換トークン
	std::vector<std::string_view> ModuleClauseWords {}; // パーサーが読めないモジュールの節の語
	std::string_view PropertySeparator = {}; // 宣言の名前と値を分ける記法
	std::vector<LiteralReading> Literals = {}; // 引用と専用区切の読取表
	std::string_view LineBoundInterpolationQuotes = {}; // 本文が物理行を跨げない引用では補間のコードも同行へ保つ引用符
	bool ShouldKeepInterpolationHead = false; // 複合補間の開始記号と値の先頭を密着させるか
	bool ShouldKeepInterpolationQuotes = false; // 補間内の引用符の変更で文法の要求を強めないか
	bool HasRegexLiterals = false; // 斜線の演算子と正規表現の開きを構文で区別する
	bool CanRegexFollowName = false; // 名前の後にも括弧無の実引数として正規表現を置ける
	bool CanRegexCrossLine = false; // 正規表現の本文が物理行を跨げる
	bool CanRegexStartLine = false; // 改行の後に正規表現を文の値として置ける
	bool HasHashRegexDelimiter = false; // 正規表現を斜線と井桁の並びで区切れる
	bool HasNestedRegexClasses = false; // 正規表現の文字集合の括弧をネスト出来る
	std::vector<std::string_view> RegexWords {}; // 後に正規表現の値を置ける語
	std::vector<std::string_view> RegexBridgeWords {}; // 文の見出と条件の括弧の間の語
	std::vector<std::string_view> RegexStmtHeads {}; // 条件の括弧の後に文を置ける見出
	bool CanMisreadEscapedRegexEnd = false; // パーサーが逃がした `/` を正規表現の終わりと読む文法
	bool CanDropTokensOnRecovery = false; // パーサーがエラーの回復で子の間のトークンを落とす文法
	bool CanTolerateTrailingCommaError = false; // パーサーがエラーとする並びの末尾のカンマを受理する記法
	bool ShouldRejectComments = false; // コメントを通常の構文として受理しない方言か
	bool HasSingleRootVal = false; // コメントを除いた最上位の値を高々１つ認める
	bool HasStrictStrEscapes = false; // 文字列の制御文字と桁の不足した Unicode のエスケープを拒む
	bool ShouldHaveFractionDigits = false; // 数値の小数点の後へ少なくとも１桁を求める
	bool CanAcceptArrowOperand = false; // パーサーが処理系の拒むアロー関数の被演算子をエラーとしない文法
	bool CanAcceptContinuationComment = false; // パーサーが処理系の拒む行継続の後のコメントをエラーとしない文法
	bool CanAcceptStmtLineBreak = false; // パーサーが処理系の拒む括弧の外の文の途中の改行をエラーとしない文法
	bool CanAcceptVerticalTabSpace = false; // パーサーが処理系の拒むトークンの間の垂直タブを空白と読む文法
	bool CanAcceptFormFeedSpace = false; // パーサーが処理系の拒むトークンの間の改ページを空白と読む文法
	std::vector<std::string_view> StmtWords {}; // マクロの置換本体の先頭で文を始める語
	std::vector<std::string_view> CondDirectives {}; // 条件を開くプリプロセッサディレクティブの名前
	std::vector<std::string_view> CondPunctuators {}; // 条件の最長一致で読む複合の前処理トークン
	std::vector<std::string_view> CondAlternativeDirectives {}; // 条件枝を切り替えるプリプロセッサディレクティブの名前
	std::string_view CondEndDirective = {}; // 条件を閉じるプリプロセッサディレクティブの名前
	std::string_view DefDirective = {}; // マクロを定義するプリプロセッサディレクティブの名前
	std::string_view UndefinitionDirective = {}; // マクロの定義を取り消すプリプロセッサディレクティブのテキスト
	std::string_view LinkageHead = {}; // 前処理条件を跨ぐ連結指定の先頭の語
	std::string_view LinkageClose = {}; // 前処理条件を跨ぐ連結指定の閉じのテキスト
	std::string_view LinkageMarkerDirective = {}; // 連結指定をパーサーへ読ませるマーカー付ディレクティブの先頭
	std::vector<std::string_view> StrPrefixes {}; // 文字列の開きの前に置く接頭辞
	std::vector<std::string_view> RawStrPrefixes {}; // 区切の括弧で囲み行継続を取消して読む生文字列の接頭辞
	std::vector<std::string_view> MacroRawStrPrefixes {}; // マクロの置換列で生文字列と読む接頭辞
	std::vector<std::string_view> MeasuredStrPrefixes {}; // 引用の幅の計数へ含める接頭辞
	std::vector<std::string_view> MeasuredRawStrPrefixes {}; // 幅の計数で生文字列として読む接頭辞
	std::vector<std::string_view> UncertainRawStrPrefixes {}; // 引用の読みが方言で変わり得る接頭辞
	std::string_view NameQuoteHeads; // 引用でなく名前の開始にも為り得る記号
	std::vector<std::string_view> HashQuotePrefixes {}; // 井桁の並びで区切る生文字列の接頭辞（接頭辞の無い形は常に認める）
	std::vector<std::string_view> VerbatimStrPrefixes {}; // 行を跨げる逐語文字列の接頭辞
	std::string_view VariadicParam = {}; // 可変長マクロの仮引数を表す名前
	std::vector<std::string_view> HeaderNameDirectives {}; // ヘッダ名を引数に取るプリプロセッサディレクティブの名前
	std::vector<std::string_view> ParsedDirectives {}; // 引数を解析するプリプロセッサディレクティブの名前（其の他のディレクティブの引数とマクロの本体は生の本文）
	std::vector<std::string_view> ClauseWords {}; // 式を始めれない節・終端の語
	std::vector<std::string_view> SeparatingModifiers {}; // 後の改行でメンバを区切る修飾語
	std::vector<std::string_view> JoiningModifiers {}; // 後の改行を跨いで次のメンバへ掛かる修飾語
	std::vector<std::string_view> LineContinuingTokens {}; // 改行の後で前の式を続けるトークン
	std::string_view IdentifierRawPrefix = {}; // 比較する識別子から除く生の接頭辞
	std::string_view IdentifierQuote = {}; // 比較する識別子を囲む引用記号
	std::string_view LabelDefSuffix = {}; // ラベルの定義のトークンの末尾に付く記号
	bool ShouldDecodeIdentifierEscapes = false; // 識別子の中の Unicode のエスケープを字として読む
	bool ShouldRestrictBasicIdentifierEscapes = false; // 識別子のエスケープで基本文字を指せない
	bool ShouldIgnoreIdentifierFormatChars = false; // 識別子の比較で書式文字（一般カテゴリ `Cf`）を無視する
	std::string_view PreserveSpaceVal = {}; // 内部空白の保持を指定する属性値
	std::string_view NativeNamespaceVal = {}; // 同じ表示規則を持つ名前空間の属性値
	std::string_view MarkupEncodingAttribute; // HTML 統合点のエンコーディングを指定する属性名
	std::string_view EmbeddedModuleType; // モジュールを読む埋込の型
	std::string_view StyleRefElement; // 外部様式を参照する要素名
	std::string_view StyleRefAttribute; // 参照の用途を指定する属性名
	std::string_view StyleRefVal; // 外部様式の用途の語
	std::vector<std::string_view> EmbeddedScriptTypes {}; // 埋込の実行可能な内容の型
	std::vector<std::string_view> EmbeddedDataTypes {}; // 埋込の構造化された内容の型
	std::string_view RawTextEscapeElement = {}; // エスケープ状態を持つ逐語要素の名前
	std::string_view RawTextEscapeOpen = {}; // 逐語要素の中でエスケープ状態を開くテキスト
	std::string_view RawTextEndOpen = {}; // 逐語要素の終了タグを開くテキスト
	std::string_view RawTextNameTerminators = {}; // 逐語要素のタグ名の後に続く区切の字
	std::string_view RawTextPlainElement = {}; // エスケープ状態を持たない逐語要素の名前
	std::string_view EmbeddedStyleType = {}; // 埋込の様式の内容の型
	std::string_view EmbeddedTypeAttribute = {}; // 埋込の内容の型を指定する属性の名前
	std::string_view AttributeValQuotes = {}; // 属性値の外側を囲む引用符の字
	Lang::ID EmbeddedScriptLang = Lang::Unknown; // 埋込の実行可能な内容の言語
	Lang::ID EmbeddedStyleLang = Lang::Unknown; // 埋込の様式の内容の言語
	Lang::ID EmbeddedDataLang = Lang::Unknown; // 埋込の構造化された内容の言語
	std::string_view StructuredDataTypePrefix = {}; // 構造化された内容の型の接頭辞
	std::string_view StructuredDataTypeSuffix = {}; // 構造化された内容の型の接尾辞
	std::unordered_set<std::string_view> MisboundBinaryOperators = {}; // 投影時に標準の優先順位へ結び直す二項演算子
	std::unordered_set<std::string_view> ReassociatedOperators = {}; // パーサーの結合を補う対象として優先順位を確定した演算子
	std::unordered_map<std::string_view, std::string_view> OperatorStandIns = {}; // パーサーが利用者定義と読む標準の演算子を同じ種類の二項式として読ませるテキスト
	bool IsSpace(const char32_t Point) const; // トークンの間の行内空白の判定関数
	bool IsJsxSpace(const char32_t Point) const; // JSX 本文の行内空白の判定関数
};
