#pragma once

#include <tree_sitter/api.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

class GrammarProfile;
class ParsedTree;

struct ParseInput;

// 指定文法に依る構文解析と適応済の解析結果の受理を担うクラス（整形仕様 3.1, 3.4 の Parse）
class SyntaxParser {
private:

	// 解析器の削除子（解放を所有者へ任せ，ムーブ・破棄を既定の物にする）
	struct Deleter {
		void operator()(TSParser *const Target) const; // 解析器解放関数
	};

	std::unique_ptr<TSParser, Deleter> Handle; // tree-sitter の解析器
	static bool IsToleratedBreak(const ParsedTree &Src, const TSNode Node, const std::span<const TSNode> Ancestors); // 許容破損の判定関数
	static bool HasContinuationComment(const ParsedTree &Src); // 行継続の直後のコメントの有無の判定関数
	static bool HasArrowOperandMisuse(const ParsedTree &Src); // 矢印関数を被演算子に置いた誤りの有無の判定関数

public:

	static constexpr int64_t DeadlineBaseMicros = 5000000; // 解析１回の期限の固定分（整形仕様 9.1 の５秒）
	static bool IsBrokenNode(const ParsedTree &Src, const TSNode Node, const std::span<const TSNode> Ancestors); // 構文破損ノードの判定関数
	static TSNode JsonTrailingCommaToken(const ParsedTree &Src, const TSNode Node); // JSONC の末尾コンマを表す破損からコンマを得る関数
	static uint32_t CountBrokenNodes(const ParsedTree &Src, const TSNode Node); // 壊れたノード (`ERROR` / `MISSING`) の計数関数
	static uint32_t CountMalformedCDefinitions(const ParsedTree &Src); // C の文法に反する関数定義の計数関数
	static bool HasEmptyBlock(const ParsedTree &Src); // 本体を失ったインデントの塊の有無の判定関数
	static bool HasUntrustedErrorZone(const ParsedTree &Src); // 境界を確定出来ない誤り区画の有無の判定関数
	static ParsedTree Parse(const GrammarProfile &Profile, const ParseInput &Input); // 適応済の木の写しを受理する関数

	TSTree *Parse(
		const TSTree *const Old,
		const std::string_view Text,
		bool &OutIsExpired,
		size_t *const ReadBytes = nullptr,
		const std::span<const uint32_t> Insertions = std::span<const uint32_t>()
	) const; // 期限付の構文解析関数

	bool Use(const TSLanguage *const Grammar); // 文法の設定関数（実行時ライブラリと非互換なら false）

	SyntaxParser(); // コンストラクタ（文法を設定しない解析器の構築）
};
