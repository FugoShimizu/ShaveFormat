#pragma once

#include "Core/CommentRepository.hpp"
#include "Core/LineBreaker.hpp"
#include "Core/OutputPosition.hpp"
#include "Core/SourceProjector.hpp"
#include "Lint/LintAnalyzer.hpp"
#include "Profile/GrammarProfile.hpp"
#include "Util/InputNormalization.hpp"
#include "Util/Lang.hpp"
#include "Util/LayoutTypes.hpp"
#include "Util/ParseAdapter.hpp"
#include "Util/ParsedTree.hpp"
#include "Util/SyntaxValidator.hpp"
#include "Util/ValidationContext.hpp"
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class FormatClassification { Unchanged, Changed, Failed, Partial, Skipped }; // 完全整形・部分整形・見送・失敗の分類

using LintWarningList = std::vector<LintWarning>; // 整形結果の規約警告列

// Main が表示する付加診断（理由又は規約警告とは別の１行）
struct FormatDiagnostic {
	std::optional<Core::OutputPoint> Point; // 得れた場合のテキスト上の位置
	std::string Text; // 区分・原因と対処，又は中核の観測
};

// 整形結果と其の診断情報（整形仕様 3.3，本文も結果が持ちソースを書き換えない）
struct FormatResult {
	std::string Text; // 整形後のテキスト（整形しなかった場合は原文）
	FormatClassification Classification = FormatClassification::Unchanged;
	LintWarningList Warnings;
	InputNormalization Normalization; // 検証済の出力へ反映した入力表現の正規化
	std::vector<FormatDiagnostic> Diagnostics; // Main が表示する付加診断
	std::string Observed; // Main が表示する読取専用な観測
	ParseIssue InputIssue = ParseIssue::None; // 部分整形の破損の確度
	std::optional<Core::OutputPoint> ReasonPoint; // 原文の見送・失敗位置（位置が得れた場合）
	FailureReason Reason = FailureReason::None; // 見送又は失敗の理由と例外の文言
};

// 整形パイプライン制御クラス
class Formatter {
private:

	friend class Main; // 入出力で受理済の原文の計数は同じ処理へ受渡

	// HTML に埋め込んだコードの処理状況（ドキュメント順の埋込毎）
	struct EmbeddedCode {
		std::string Observed; // 子の中核モデルの読取専用な観測
		std::string Note; // 部分整形又は見送の通知
		std::string Diagnostic; // 区分と対処を１度だけ表示する付加診断
		InputNormalization Normalization; // 親の最終候補へ反映する子の表現の正規化
	};

	// 子のコード・コメント・検証期待の別々の所有経路
	struct EmbeddedOutput {
		std::string Observed; // 親がドキュメント順に表示する読取専用な観測
		const GrammarProfile *Profile = nullptr; // 子の入力で確定した文法
		std::shared_ptr<const LaidOutCode> Code; // 親の中核へ渡すコードだけの経路
		std::shared_ptr<Core::OutputExpectation> Expectation; // 子の検証だけへ渡す期待構文
		std::shared_ptr<const Core::CommentRepository> Comments; // 子の配置・検証だけへ渡す所有記録
		std::shared_ptr<ValidationContext> Ctx; // 入力を所有する子の検証データ
	};

	// 入力構文破損の固定通知
	static constexpr std::string_view SyntaxErrorWarning = "Parser could not read part of the source; formatting is partial";
	const bool ShouldObserveSelections; // 選択の対象の読取専用な観測の指定
	const bool ShouldObserve; // 中核の読取専用な観測の指定
	const bool ShouldWarn; // コメント配置の補助診断の指定
	const SyntaxValidator Validator; // 診断指定を受け取った独立検証の工程
	const Core::LineBreaker Breaker; // 行幅を持つ `LineBreaker`（行幅を知るのは此のオブジェクトだけで，工程へは値を渡さない）
	static bool CanPreserveEmbedded(const FailureReason Reason); // 子の見送時に原文保持を許す理由の判定関数
	static std::string GetParseNotice(const ParseIssue Issue); // 破損の区分と対処の付加診断の取得関数
	std::string ObservedCore(const std::shared_ptr<const LaidOutCode> &Code) const; // 中核の出力の観測のテキストの取得関数

	FormatClassification FinalizeOutput(
		const GrammarProfile &Profile,
		ValidationContext Ctx,
		std::shared_ptr<const LaidOutCode> &&Code,
		const std::string &Src,
		Core::CommentRepository Comments,
		Core::OutputExpectation Expectation,
		const std::string &FinalText,
		const bool ShouldLint,
		const bool IsFormChanged,
		const bool IsPartialNoticed,
		const ParseIssue InputIssue,
		const std::vector<EmbeddedCode> &Embeds,
		std::vector<LintWarning> &Warnings,
		std::vector<FormatDiagnostic> &Diagnostics,
		FailureReason &Reason
	) const; // 整形結果の確定関数

	void RunCore(
		const GrammarProfile &Profile,
		ParseInput &Input,
		std::optional<ParsedTree> &Parsed,
		std::optional<ValidationContext> &Ctx,
		std::shared_ptr<const LaidOutCode> &Code,
		std::optional<Core::OutputExpectation> &Expectation,
		std::optional<Core::CommentRepository> &Comments,
		std::string &FinalText,
		std::string &Observed,
		std::vector<std::string> &Diagnostics,
		EmbeddedOutput *const Embedded
	) const; // 中核の工程の適用関数

	FormatClassification FormatEmbeddedCode(
		ParsedTree &Src,
		std::vector<EmbeddedCode> &Embeds,
		Core::OwnedInputRoutes &Owned,
		FormatResult &Result
	) const; // HTML に埋め込んだコードの整形関数

	FormatClassification FormatCode(
		const GrammarProfile &Profile,
		std::string &Src,
		const bool ShouldLint,
		FormatResult &Result,
		EmbeddedOutput *const Embedded,
		const bool IsAcceptedInput = false
	) const; // コードの整形関数

	FormatResult FormatAccepted(
		const std::string &RawSrc,
		const ::Lang Lang,
		const bool ShouldLint,
		const bool IsAcceptedInput
	) const; // 受理計数を共有する整形関数

public:

	FormatResult Format(const std::string &RawSrc, const ::Lang Lang, const bool ShouldLint) const; // ソース整形関数

	explicit Formatter(
		const Core::LineBreaker &LineBreaking,
		const bool Observe = false,
		const bool Warn = false,
		const bool Diagnose = false
	); // コンストラクタ（`LineBreaker` の受取）
};
