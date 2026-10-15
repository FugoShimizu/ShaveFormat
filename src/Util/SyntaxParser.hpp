#pragma once

#include <tree_sitter/api.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

class GrammarProfile;
class ParsedTree;

struct ParseInput;

// 差分解析で実際に読み出す残余予算（再利用した領域を含めない）
struct ParserReadBudget {
	size_t Remaining; // 未使用のバイト数
	bool IsCancelled = false; // 予算を使い切った後の読出で立てる打切の合図
	std::string_view Take(const std::string_view Src, const size_t Pos); // 残余予算内の連続領域の取得関数
};

// 指定文法に依る構文解析と適応済の解析結果の受理を担うクラス（整形仕様 3.1, 3.4 の Parse）
class SyntaxParser {
private:

	// パーサーの削除子（解放を所有者へ任せ，ムーブ・破棄を既定の物にする）
	struct Deleter {
		void operator()(TSParser *const Target) const; // パーサー解放関数
	};

	std::unique_ptr<TSParser, Deleter> Handle; // tree-sitter のパーサー

	static TSTree *ParseUntil(
		TSParser *const Parser,
		const TSTree *const Old,
		const TSInput Input,
		const std::chrono::steady_clock::time_point &Until,
		const ParserReadBudget *const Budget
	); // 期限と読込予算を使う解析関数

	static bool IsToleratedBreak(const ParsedTree &Src, const TSNode Node, const std::span<const TSNode> Ancestors); // 許容破損の判定関数
	static bool HasContinuationComment(const ParsedTree &Src, std::optional<uint32_t> &IssueOffset); // 行継続の直後のコメントの有無の判定関数
	static bool HasArrowOperandMisuse(const ParsedTree &Src, std::optional<uint32_t> &IssueOffset); // アロー関数を被演算子に置いたエラーの有無の判定関数
	static bool HasImplicitTerminatorMisuse(const ParsedTree &Src, std::optional<uint32_t> &IssueOffset); // 自動の文終端が構文の内部へ入る入力エラーの判定関数
	static bool HasStmtLineBreak(const ParsedTree &Src, std::optional<uint32_t> &IssueOffset); // 論理行の改行とインデントの成立条件の判定関数
	static bool HasRejectedVal(const ParsedTree &Src, std::optional<uint32_t> &IssueOffset); // 処理系が拒む値の数とトークンの有無の判定関数
	static bool HasRejectedNul(const ParsedTree &Src, std::optional<uint32_t> &IssueOffset); // 破損又は名前の無いトークンへ入った NUL の有無の判定関数
	static bool HasRejectedSpace(const ParsedTree &Src, std::optional<uint32_t> &IssueOffset); // 処理系が拒むトークンの間の空白の字の有無の判定関数
	static bool ReadFunctionDeclarator(const ParsedTree &Src, const TSNode Node, TSNode &Declarator, size_t &Work); // 宣言子のラッパーを透過して関数の成立を確認する関数
	static std::optional<uint32_t> EmptyBlockAt(const ParsedTree &Src); // 本体を失ったインデントのブロックの有無の判定関数

public:

	static constexpr int64_t DeadlineBaseMicros = 5000000; // 解析１回の期限の固定分（整形仕様 9.1 の５秒）
	static bool IsBrokenNode(const ParsedTree &Src, const TSNode Node, const std::span<const TSNode> Ancestors); // 構文破損ノードの判定関数
	static TSNode JsonTrailingCommaToken(const ParsedTree &Src, const TSNode Node); // JSONC の末尾カンマを表す破損からカンマを得る関数
	static bool HasUntrustedErrorZone(const ParsedTree &Src); // 境界を確定出来ないエラー領域の有無の判定関数
	static uint32_t CountBrokenNodes(const ParsedTree &Src, const TSNode Node, TSPoint *const FirstPoint = nullptr); // 壊れたノード (`ERROR` / `MISSING`) の計数関数
	static uint32_t CountMalformedCDefs(const ParsedTree &Src); // 文法が要求する関数宣言子を確認出来ない定義の共有計数関数
	static ParsedTree Parse(const GrammarProfile &Profile, ParseInput &Input); // 適応済の木を受理する関数

	TSTree *Parse(
		const TSTree *const Old,
		const std::string_view Text,
		bool &IsExpired,
		ParserReadBudget *const Budget = nullptr
	) const; // 期限付の構文解析関数

	bool CanSetGrammar(const TSLanguage *const Grammar); // 文法の設定関数（実行時ライブラリと非互換なら false）

	SyntaxParser(); // コンストラクタ（文法を設定しないパーサーの構築）
};
