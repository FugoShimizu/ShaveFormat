#pragma once

#include "Core/CommentRepository.hpp"
#include "Core/LineBreaker.hpp"
#include "Lint/LintAnalyzer.hpp"
#include "Profile/GrammarProfile.hpp"
#include "Util/Lang.hpp"
#include "Util/LayoutTypes.hpp"
#include "Util/ParseAdapter.hpp"
#include "Util/ParsedTree.hpp"
#include "Util/ValidationContext.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class FormatClassification { Unchanged, Changed, Failed, Partial, Skipped }; // 完全整形・部分整形・見送・失敗の分類

using LintWarningList = std::vector<LintWarning>; // 整形結果の規約警告列

// 整形結果と其の診断情報（整形仕様 3.3，本文も結果が持ち原稿を書き換えない）
struct FormatResult {
	std::string Text; // 整形後の字面（整形しなかった場合は原文）
	FormatClassification Classification = FormatClassification::Unchanged;
	LintWarningList Warnings;
	FailureReason Reason = FailureReason::None; // 見送又は失敗の理由と例外の文言
};

// 整形パイプライン制御クラス
class Formatter {
private:

	// HTML に埋め込んだコードの整形結果（文書順の埋込毎）
	struct EmbeddedCode {
		std::string Note; // 整形し切れなかった理由（部分整形の通知に使い，空なら無し）
		std::string Text; // 整形した中身の字面（開始・終了タグとは別の行へ置く前の物で，警告の位置を写す基準）
		std::vector<LintWarning> Warnings; // 中身の静的検査の警告（中身の字面の位置）
	};

	// 子から親の所有区画へ渡すコード行と逐語の中から始まる行
	struct EmbeddedOutput {
		std::string CodeText; // コメントを除いた中核の出力
		std::vector<uint32_t> CodeVerbatimLines; // 中核の出力の逐語の中から始まる行の番号
		std::vector<uint32_t> VerbatimLines; // 最終字面の逐語の中から始まる行の番号
	};

	const Core::LineBreaker Breaker; // 行幅を持つ行分割器（行幅を知るのは此の器だけで，工程へは値を渡さない）

	static FormatClassification FinalizeOutput(
		const GrammarProfile &Profile,
		std::shared_ptr<const LaidOutCode> &&Code,
		const ValidationContext &Context,
		const std::string &Source,
		const std::string &FinalText,
		const Core::OutputExpectation &Expectation,
		const Core::CommentRepository &Comments,
		const bool ShouldLint,
		const bool IsFormChanged,
		const bool IsPartialNoticed,
		const std::vector<EmbeddedCode> &Embeds,
		std::vector<LintWarning> &Warnings,
		bool &HasEmbedNote,
		FailureReason &Reason
	); // 整形結果の確定関数

	static std::string ObservedCore(const std::shared_ptr<const LaidOutCode> &Code); // 中核の出力の観測の字面の取得関数

	FormatClassification FormatEmbeddedCode(
		ParsedTree &Src,
		const bool ShouldLint,
		std::vector<EmbeddedCode> &Embeds,
		FormatResult &Result
	) const; // HTML に埋め込んだコードの整形関数

	void RunCore(
		const GrammarProfile &Profile,
		const ParseInput &Input,
		std::optional<ParsedTree> &Parsed,
		std::optional<ValidationContext> &Context,
		std::shared_ptr<const LaidOutCode> &Code,
		std::optional<Core::OutputExpectation> &Expectation,
		std::optional<Core::CommentRepository> &Comments,
		std::string &FinalText,
		EmbeddedOutput *const Embedded
	) const; // 中核の工程の適用関数

	FormatClassification FormatCode(
		const GrammarProfile &Profile,
		std::string &Source,
		const Lang Language,
		const bool ShouldLint,
		FormatResult &Result,
		EmbeddedOutput *const Embedded
	) const; // コードの整形関数

public:

	static constexpr std::string_view SyntaxErrorWarning = "Parser could not read part of the source; formatting is partial"; // 入力構文破損の固定通知
	FormatResult Format(const std::string &RawSource, const Lang Language, const bool ShouldLint) const; // ソース整形関数

	explicit Formatter(const Core::LineBreaker &LineBreaking); // コンストラクタ（行分割器の受取）
};
