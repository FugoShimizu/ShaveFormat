#pragma once

#include "Pass/Lint.hpp"
#include "Util/Lang.hpp"
#include "Util/TsSource.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class FormatOutcome { Unchanged, Changed, Skipped, SyntaxGuard, Defect, Failed }; // 整形結果の種別（`Defect` は内部防壁の発火，`Failed` は資源不足）

// 整形結果と其の診断情報（本文は呼出元の参照先へ格納する）
struct FormatResult {
	FormatOutcome Outcome = FormatOutcome::Unchanged;
	std::vector<LintWarning> Warnings;
	std::string_view SkipReason; // 整形を行わなかった理由（静的な字面を指す）
	bool HasInputError = false;
};

// 整形パイプライン制御クラス
class Formatter {
private:

	static constexpr uint32_t RubyModifierRounds = 4; // Ruby の修飾子形式への変換と組直を繰り返す上限

	static bool RejectInvalidInput(
		const std::string &Source,
		Lang &Language,
		std::optional<TSSource> &OutSrc,
		std::string_view &SkipReason,
		const bool IsLangAmbiguous,
		char &Marker
	); // 入力の早期振分と `TSSource` 構築関数

	static void PrepareComments(TSSource &Src, const Lang Language, const char Marker); // コメント添付準備関数
	static bool RunCorePasses(TSSource &Src, const Lang Language, std::string_view &SkipReason); // 中核パイプライン適用関数

	static FormatOutcome FinalizeOutput(
		TSSource &Src,
		const Lang Language,
		const std::string &Source,
		std::string &Output,
		std::vector<LintWarning> &Warnings,
		const bool SkipsLint,
		const uint32_t InputBrokenCount,
		const bool IsTailChanged,
		const char Marker
	); // 整形結果の確定関数

	static FormatOutcome FormatCode(
		std::string &Source,
		const Lang Language,
		FormatResult &Result,
		const bool SkipsLint,
		const bool IsLangAmbiguous
	); // コードの整形関数

public:

	static constexpr std::string_view SyntaxErrorWarning = "Parser could not read part of the source; formatting is partial"; // 入力構文破損の固定通知
	static FormatResult Format(std::string &Source, const Lang Language, const bool SkipsLint, const bool IsLangAmbiguous); // ソース整形関数

	Formatter() = delete; // コンストラクタ（禁止）
};
