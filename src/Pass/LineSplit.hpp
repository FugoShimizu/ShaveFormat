#pragma once

#include "../Util/Lang.hpp"
#include "../Util/TextEdit.hpp"
#include "../Util/TsSource.hpp"
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 行分割パスクラス
class LineSplitPass {
private:

	enum class BreakSide { None, Tail, Head }; // 分割無，前行末，次行頭の区分

	// 分割位置エントリ構造体
	struct BreakEntry {
		uint32_t Pos; // 分割位置のバイトオフセット
		uint32_t ParentId; // 親ノードの識別子
		uint32_t Depth; // ネスト深さ
	};

	// 制御構文情報構造体
	struct CtrlFlowInfo {
		uint32_t NodeStart; // ノード開始オフセット
		uint32_t BodyStart; // 本体開始オフセット
		uint32_t BodyEnd; // 本体終了オフセット
		std::vector<std::pair<uint32_t, uint32_t>> Excluded; // 折返対象から除外する範囲
		bool IsNewlineWrap; // true: 親が if/while/for/do/else 且つ本体が制御構文ではない単一文の時，元ソースに `\n` が有れば折返
		bool IsElseAfterBlock = false; // true: 本体が波括弧ブロックの then 句に続く else 節（閉じ `}` と同行化される）
		uint32_t Chain = 0; // １行に畳んだ Kotlin の if の連鎖の印（連鎖の頭の開始位置 + 1，連鎖の外は 0：何れかの枝を包む巡に全ての枝を包む）
	};

	// 無名子ノード情報構造体
	struct UnnamedInfo {
		TSNode Child; // 対象の無名子ノード
		bool IsFirst; // 親内の最初の子か
		TSNode Prev; // 直前の兄弟ノード
		bool IsLast = false;
	};

	static size_t MaxChars; // １行の最大文字数
	static std::vector<std::pair<uint32_t, uint32_t>> PythonContinuationRanges(const TSSource &Src); // Python の暗黙継続が可能な最外括弧範囲の収集関数

	static bool IsInPythonContinuation(
		const std::vector<std::pair<uint32_t, uint32_t>> &Ranges,
		const uint32_t Pos,
		const uint32_t End
	); // Python の継続括弧内かの判定関数

	static bool IsTerminal(const std::string_view Type); // 終端ノードかの判定関数
	static bool IsCompactNode(const std::string_view Type); // 圧縮対象ノードかの判定関数
	static bool IsSingleChar(const std::string &Source, const uint32_t Start, const uint32_t End, const char Char); // 単一文字範囲かの判定関数
	static BreakSide SplitSide(const std::string_view Token, const Lang Language, const bool IsFirst); // トークンの分割側の判定関数

	static bool ComputeBreakPos(
		const std::string &Source,
		const uint32_t ChildStart,
		const uint32_t ChildEnd,
		const Lang Language,
		const bool IsFirst,
		uint32_t &OutPos
	); // 分割位置の算出関数

	static void CollectUnnamedBreaks(
		const std::string &Source,
		const Lang Language,
		const std::string_view TypeView,
		const bool IsTypeColonParent,
		const uint32_t ParentId,
		const uint32_t Depth,
		const std::vector<UnnamedInfo>::const_iterator Begin,
		const std::vector<UnnamedInfo>::const_iterator End,
		std::vector<BreakEntry> &Entries
	); // 無名子ノードの分割位置収集関数

	static void CollectJsxTagBreaks(
		const std::string &Source,
		const TSNode Node,
		const uint32_t ParentId,
		const uint32_t Depth,
		std::vector<BreakEntry> &Entries
	); // JSX タグの分割位置収集関数

	static void CollectBreaks(
		const std::string &Source,
		const TSNode Node,
		const Lang Language,
		const uint32_t Depth,
		std::vector<BreakEntry> &Entries,
		std::vector<UnnamedInfo> &UnnamedPool
	); // 分割位置収集関数

	static void CollectBreaksParallel(
		const std::string &Source,
		const TSNode Root,
		const Lang Language,
		std::vector<BreakEntry> &Entries
	); // 分割位置の並列収集関数

	static void CollectElseWhileJoins(const TSSource &Source, const Lang Language, std::vector<uint32_t> &JoinBraceEnds); // else/while を `}` の行へ続ける位置の収集関数

	static void GreedyMerge(
		const std::string &Source,
		std::vector<uint32_t> &Breaks,
		const std::vector<uint32_t> &LockedBreaks,
		const std::vector<BreakEntry> &Entries,
		const std::vector<uint32_t> &Newlines,
		const std::vector<uint32_t> &JoinBraceEnds
	); // 分割位置の貪欲統合関数

	static std::vector<TSNode> OuterPythonTypes(const TSSource &Src); // Python の最も外の型注釈の列挙関数
	static std::vector<uint32_t> WrapPythonExpressions(TSSource &Src); // Python の幅超過式への継続括弧付与関数
	static void CollectPythonBreaks(const TSSource &Src, std::vector<uint32_t> &Breaks, std::vector<uint32_t> &Newlines); // Python の合法な分割位置の収集関数
	static void ApplyPython(TSSource &Src); // Python の文境界とインデントを保つ行分割関数
	static void SplitWideVarDecls(TSSource &Src, const Lang Language); // 幅超過変数宣言の分割関数
	static bool ExpandWideRubyBranches(TSSource &Src, const std::vector<uint32_t> &Breaks, const std::vector<uint32_t> &Newlines); // 幅超過の Ruby の１行の分岐の展開関数
	static void DropHeredocLineBreaks(const std::string &Source, const TSNode Root, std::vector<BreakEntry> &Entries); // ヒアドキュメント開始行の分割候補除去関数
	static void DropInlineHtmlBreaks(const TSNode Root, std::vector<BreakEntry> &Entries); // PHP の地の文の境界の分割候補除去関数
	static void DropStringizedArgumentBreaks(const TSSource &Src, std::vector<BreakEntry> &Entries); // 字面の儘の実引数の中の分割候補除去関数

	static void FindCtrlFlows(
		const TSSource &Source,
		const TSNode Node,
		const Lang Language,
		const std::vector<std::pair<uint32_t, uint32_t>> &Blocks,
		std::vector<CtrlFlowInfo> &Out,
		const bool IsElseAfterBlock = false,
		const uint32_t Chain = 0
	); // 制御構文情報の再帰収集関数

	static void FindCtrlFlowsParallel(const TSSource &Source, const Lang Language, std::vector<CtrlFlowInfo> &Out); // 制御構文の並列探索関数

	static bool ApplyBraceWraps(
		TSSource &Work,
		const Lang Language,
		std::vector<uint32_t> &OutBreaks,
		std::vector<uint32_t> &OutNewlines,
		std::vector<uint32_t> &OutJoinBraceEnds
	); // 波括弧の折返付与関数

	static std::string BuildResult(
		const std::string &Source,
		const std::vector<uint32_t> &Breaks,
		const std::vector<uint32_t> &Newlines,
		const TSNode Root,
		const std::vector<uint32_t> &JoinBraceEnds
	); // 結果文字列の構築関数

public:

	static void SetMaxChars(const size_t Chars); // 最大文字数の設定関数
	static bool Apply(TSSource &Src, const Lang Language, std::string_view &SkipReason); // 行分割の適用関数

	LineSplitPass() = delete; // コンストラクタ（禁止）
};
