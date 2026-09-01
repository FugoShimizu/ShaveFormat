#pragma once

#include "../Util/Lang.hpp"
#include "../Util/TsSource.hpp"
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// LintPass が検出した規約違反１件を表す構造体
struct LintWarning {
	uint32_t Row; // 違反位置の行
	uint32_t Column; // 違反位置の列
	std::string Message; // 警告文（引数名等を含む動的メッセージにも対応する為 string_view でなく string で所有）
};

// 整形済の AST を走査し規約違反を検出する静的ユーティリティクラス
class LintPass {
private:

	using LabelTargetIndex = std::unordered_map<const void *, TSNode>; // ラベル節点毎の対象ループ索引（空節点は対象無）
	using CondContextIndex = std::unordered_map<const void *, bool>; // 真偽値として評価する節点索引（値は JSX 式内か）

	using StringEventIndex =
	std::unordered_map<const void *, std::unordered_map<std::string_view, std::vector<std::pair<uint32_t, bool>>>>; // スコープ毎の文字列宣言・代入事象を位置順に持つ索引

	struct GotoInfo {
		std::unordered_map<const void *, TSNode> Targets; // goto 節点毎の飛先（未解決は空節点）
		std::vector<uint32_t> Entries; // 到達候補となるラベルの開始位置
	}; // 関数内の goto と到達可能ラベルの索引構造体

	static constexpr size_t MessageMax = 0X100; // 警告本文の上限（利用者のソース片を挟む警告が長大に為るのを防ぐ）

private:

	static std::string_view NodeView(const std::string &Source, const TSNode Node); // ノードのソース上テキスト参照取得関数
	static std::string_view UnwrapAssignableName(const std::string &Source, TSNode Lhs); // 代入可能式の包み剥離識別子名取得関数
	static bool IsClassOrderLang(const Lang Language); // メンバ宣言順検査対象言語判定関数
	static void Push(const TSNode Node, const std::string_view Message, std::vector<LintWarning> &Out); // 違反１件追加関数
	static bool IsFlowBoundary(const std::string_view Type, const Lang Language); // 独立した制御フロー境界の判定関数

	static void CheckAccessSpecifierOrder(
		const TSNode Node,
		const std::string &Source,
		const Lang Language,
		std::vector<LintWarning> &Out
	); // アクセス修飾子順序違反検出関数

	static void CheckGotoOrLabel(
		const TSNode Node,
		const Lang Language,
		const bool HasGoto,
		LabelTargetIndex &Labels,
		std::vector<LintWarning> &Out
	); // goto／ラベル付 break・continue 検出関数

	static void CheckUselessLocalVariable(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out); // 無意味な局所変数検出関数
	static bool HasDescendantType(const TSNode Node, const std::string_view Type); // 部分木の指定型ノード包含判定関数
	static bool IsAutoExceptionContext(const TSNode AutoNode); // 型推論キーワードの例外文脈判定関数

	static void CheckTypeInferenceKeyword(
		const TSNode Node,
		const std::string &Source,
		const Lang Language,
		std::vector<LintWarning> &Out
	); // auto / var / any 使用検出関数

	static bool IsIdentifierFormat(const uint32_t Point); // 識別子内の Unicode 書式文字判定関数
	static std::string LabelName(std::string_view Name, const Lang Language); // ラベル識別子の表記統一関数

	static std::string_view FindUnnamedToken(
		const std::string &Source,
		const TSNode Node,
		const std::string_view Option1,
		const std::string_view Option2
	); // 名前無子トークンからの候補一致トークン検索関数

	static GotoInfo CollectGotoTargets(const TSNode Body, const std::string &Source, const Lang Language); // 関数内 goto 飛先収集関数
	static bool HasGotoEntry(const TSNode Node, const GotoInfo &Gotos); // 節点内への goto 入口有無判定関数
	static std::string_view FirstChildType(const TSNode Node); // 先頭の子の型名取得関数
	static bool HasReturnInFlow(const TSNode Node, const Lang Language); // 同じ制御フロー内の返戻文包含判定関数
	static bool IsWithinScope(const TSNode Node, const TSNode Scope); // 内部転送の判定範囲包含関数

	static bool HasLoopExit(
		const TSNode Body,
		const TSNode Loop,
		const std::string &Source,
		const Lang Language,
		const GotoInfo &Gotos,
		const TSNode TransferScope = TSNode{}
	); // 対象ループからの脱出有無判定関数

	static bool IsAlwaysTrueCondition(TSNode Cond, const std::string &Source); // ループ条件の真値リテラル判定関数

	static bool DoesSwitchTerminate(
		const TSNode Node,
		const std::string &Source,
		const Lang Language,
		const GotoInfo &Gotos,
		const TSNode TransferScope
	); // 切替文の全分岐終端判定関数

	static bool TerminatesAlways(
		const TSNode Node,
		const std::string &Source,
		const Lang Language,
		const GotoInfo &Gotos,
		const TSNode TransferScope = TSNode{}
	); // 制御フロー上の必ず終端する文の判定関数

	static void CheckMissingExplicitReturn(
		const TSNode Node,
		const std::string &Source,
		const Lang Language,
		std::vector<LintWarning> &Out
	); // 関数本体終端と到達不能 return 検証関数

	static bool HasJsxDescendant(const TSNode Node); // 部分木の JSX 要素包含判定関数

	static void CheckFunctionNamingConvention(
		const TSNode Node,
		const std::string &Source,
		const Lang Language,
		std::vector<LintWarning> &Out
	); // 関数命名規則違反検出関数

	static void CheckPythonReturnTypeHint(const TSNode Node, std::vector<LintWarning> &Out); // Python 関数戻値型ヒント不在検出関数

	static void CheckDocCommentPresence(
		const TSNode Node,
		const std::string &Source,
		const Lang Language,
		std::vector<LintWarning> &Out
	); // 関数ドキュメントコメント不在検出関数

	static void CheckDocCommentCompleteness(
		const TSNode Node,
		const std::string &Source,
		const Lang Language,
		std::vector<LintWarning> &Out
	); // ドキュメントコメント説明欠落検出関数（自動生成対象言語）

	static void CheckMemberDeclarationOrder(const TSNode Node, const Lang Language, std::vector<LintWarning> &Out); // クラスメンバ宣言順違反検出関数
	static void CheckConstexprCandidate(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out); // constexpr 化候補検出関数

	static void CheckCppConstCandidate(
		const TSNode Node,
		const std::string &Source,
		const std::unordered_set<std::string> &MutatedNames,
		std::vector<LintWarning> &Out
	); // C/C++ const 候補検出関数

	static bool IsFloatLiteral(const std::string_view Text); // リテラル文字列の浮動小数判定関数
	static void CheckFloatSuffixCandidate(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out); // C/C++ 浮動小数接尾辞候補検出関数
	static void CheckMacroConstant(const TSNode Node, std::vector<LintWarning> &Out); // #define 定数検出関数
	static void CheckHeaderGuardCandidate(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out); // C/C++ ヘッダガード候補検出関数
	static void CheckUsingNamespace(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out); // using namespace 検出関数

	static size_t CountCodeLines(
		const std::string &Source,
		const std::vector<std::pair<uint32_t, uint32_t>> &Comments,
		const uint32_t Start,
		const uint32_t End,
		const size_t Limit
	); // コメントを除くコードの行数の取得関数

	static void CheckReturnCommentPresence(
		const TSNode Node,
		const std::string &Source,
		const std::vector<std::pair<uint32_t, uint32_t>> &Comments,
		const Lang Language,
		std::vector<LintWarning> &Out
	); // return コメント不在検出関数

	static void MarkCondContext(const TSNode Host, const std::string &Source, CondContextIndex &Index); // 真偽値として評価される位置の印付関数

	static void CheckZeroComparisonCondition(
		const TSNode Node,
		const std::string &Source,
		const CondContextIndex &Conditions,
		const Lang Language,
		std::vector<LintWarning> &Out
	); // 制御構文条件の整数ゼロ比較検出関数 (C/C++/JS/TS)

	static void CheckNumberStrictEquality(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out); // 数値比較の厳密等価検出関数
	static void CheckNullishComparisonCandidate(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out); // JS/TS のヌリッシュ比較縮約候補検出関数
	static void CheckCStyleCast(const TSNode Node, std::vector<LintWarning> &Out); // C 形式キャスト検出関数
	static void CheckNullMacroCandidate(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out); // C++ NULL 候補検出関数
	static void CheckUnsafeIfMergeCandidate(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out); // 波括弧言語の if 統合候補検出関数
	static void CheckStandardsReference(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out); // 規約番号参照の検出関数
	static void CheckBoolFlipPattern(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out); // 真偽値反転代入パターン検出関数
	static void CheckCmpBoundary(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out); // 整数リテラル境界比較検出関数

	static bool LooksLikeStringVariable(
		const std::string &Source,
		const TSNode AssignNode,
		const std::string_view Name,
		StringEventIndex &Index
	); // 変数の文字列文脈推定判定関数

	static void CheckCompoundAssignIncrement(
		const TSNode Node,
		const std::string &Source,
		StringEventIndex &Index,
		std::vector<LintWarning> &Out
	); // 増分代入の前置インクリメント推奨検出関数

	static void CheckCppPostfixIncrement(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out); // C++ 後置増減候補検出関数
	static void CheckRedundantDoubleSign(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out); // 冗長な二重符号 (`-(-x)` / `+(+x)`) 検出関数
	static void CheckNestingDepth(const TSNode Root, const std::string &Source, const Lang Language, std::vector<LintWarning> &Out); // ネストの深さの検査関数

public:

	static std::vector<LintWarning> Run(const TSSource &Src, const Lang Language); // 違反警告リスト返戻関数

	LintPass() = delete; // コンストラクタ（禁止）
};
