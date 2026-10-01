#pragma once

#include "FormatLimitExceeded.hpp"
#include "Lang.hpp"
#include "ParsedTree.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class GrammarProfile;

struct ReadingRules;

// 解析の前に入力の表現だけを正規化するクラス（AST の判断も整形の判断も行わない）
class InputNormalizer {
private:

	static bool HasUnicodeSeparator(const GrammarProfile &Profile, const std::string_view Source); // Unicode の区切りの有無の判定関数

	static bool FoldCrLfOutside(
		std::string &Source,
		const std::vector<std::pair<uint32_t, uint32_t>> &Literals,
		const bool KeepsTailRun
	); // CRLF の正規化関数

	static bool NormalizeNewlines(
		const GrammarProfile &Profile,
		std::string &Source,
		const bool HasUnicodeSeparator,
		FailureReason &Reason,
		std::optional<ParsedTree> &Trial
	); // 改行コードの正規化関数

	static bool LooksForeignDialect(const ReadingRules &Rules, const std::string &Source); // 対象外の方言の宣言の判定関数
	static bool LooksMarkupDocument(const ReadingRules &Rules, const std::string &Source); // 対象外の文書の判定関数

public:

	// 入力の表現の正規化の結果（整形仕様 3.3 の NormalizedInput，原文は書き換えない）
	struct NormalizedInput {
		std::string Text; // 正規化した字面
		FailureReason Reason; // 受理しない入力の見送の理由（受理すれば無し）
		std::optional<ParsedTree> Trial; // 同じ字面と文法の試験解析の結果
		bool IsFormChanged = false; // BOM・改行の姿（CRLF・単独の CR・文法が読む Unicode の区切り）を変えたか（整形仕様 10.3 の整形差分に含める）
		size_t BomCount = 0; // 先頭から除いた UTF-8 の BOM の数
		size_t DataStart = std::string::npos; // 実行停止後のデータの正規化済原稿での開始位置（無ければ無し）
	};

	static size_t CountBom(const std::string_view Source); // 先頭の UTF-8 の BOM の数の取得関数
	static bool ReadsLoneCrAsNewline(const GrammarProfile &Profile); // 単独の CR を行の終わりに読む記法かの判定関数
	static NormalizedInput Normalize(const GrammarProfile &Profile, const std::string &RawSource); // 入力の受理の判定と表現の正規化関数

	InputNormalizer() = delete; // コンストラクタ（禁止）
};
