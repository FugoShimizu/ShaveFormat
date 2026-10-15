#pragma once

#include "FormatLimitExceeded.hpp"
#include "InputNormalization.hpp"
#include "ParsedTree.hpp"
#include "../Core/OutputPosition.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class GrammarProfile;

struct ReadingRules;

// 解析の前に入力の表現だけを正規化するクラス（AST の判断も整形の判断も行わない）
class InputNormalizer {
private:

	static bool HasUnicodeSeparator(const GrammarProfile &Profile, const std::string_view Src); // Unicode の区切の有無の判定関数
	static bool HasFoldedCrLfOutside(std::string &Src, const std::vector<std::pair<uint32_t, uint32_t>> &Literals); // CRLF の正規化関数
	static bool IsRootMarkupDoc(const ReadingRules &Rules, std::string_view Src); // 登録された根の XML ドキュメントの判定関数
	static std::string_view ReadEncodingLabel(const ReadingRules &Reading, const std::string_view Text); // 標準のラベルから正準名を取得する関数
	static std::optional<std::pair<std::string_view, std::string_view>> ReadEncodingAttribute(std::string_view &View); // エンコーディングの prescan の属性と値の読取関数
	static std::string_view ReadContentEncoding(const ReadingRules &Reading, std::string_view View); // content 属性の宣言の読取関数
	static std::string_view ReadXmlEncoding(const ReadingRules &Reading, std::string_view View); // prescan の XML 宣言の読取関数
	static std::string_view ReadMarkupEncoding(const ReadingRules &Reading, const std::string_view Text); // 先頭の meta 宣言の prescan 関数
	static std::string_view ReadByteEncoding(const ReadingRules &Reading, const std::string_view Text); // 厳密なバイト列の宣言の読取関数

	static FailureReason::Kind GetEncodingDeclFailure(
		const GrammarProfile &Profile,
		const std::string_view Text,
		const bool HasFilePreamble,
		const bool HasBom,
		size_t &DeclarationOffset
	); // 入力のエンコーディング宣言の位置と値の受理判定関数

	static bool IsWideEncodingCandidate(const std::string_view Src); // 対応外の幅のエンコーディングの判定関数
	static size_t CountBom(const std::string_view Src); // 先頭の UTF-8 の BOM の数の取得関数

	static bool HasNormalizedNewlines(
		const GrammarProfile &Profile,
		std::string &Src,
		const bool HasUnicodeSeparator,
		FailureReason &Reason,
		std::optional<ParsedTree> &Trial,
		InputNormalization &Changes
	); // 改行コードの正規化関数

	static bool IsMarkupDocCandidate(const ReadingRules &Rules, const std::string &Src); // 対象外のドキュメントの判定関数

public:

	// 入力の表現の正規化の結果（整形仕様 3.3 の NormalizedInput，原文は書き換えない）
	struct NormalizedInput {
	private:

		friend class InputNormalizer;

		std::vector<std::pair<uint32_t, uint32_t>> Offsets; // 正規化位置と原文位置の対応が変わる境界
		std::vector<uint32_t> LineStarts; // 原文上の論理行の開始

	public:

		bool HasFilePreamble = true; // ファイル先頭の BOM を持ち得る入力か
		std::string Text; // 正規化したテキスト
		std::optional<Core::OutputPoint> ReasonPoint; // 受理しない宣言の原文上の位置
		FailureReason Reason; // 受理しない入力の見送の理由（受理すれば無）
		std::optional<ParsedTree> Trial; // 同じテキストと文法の試行解析の結果
		bool IsFormChanged = false; // BOM・改行の姿（CRLF・単独の CR・文法が読む Unicode の区切）を変えたか（整形仕様 10.3 の整形差分に含める）
		InputNormalization Changes; // 表現の正規化で実際に変えた事実
		size_t DataStart = std::string::npos; // 実行停止後のデータの正規化済ソースでの開始位置（無ければ無）
		std::optional<Core::OutputPoint> GetPoint(const std::optional<uint32_t> Offset) const; // 入力の診断位置の取得関数
		std::span<const uint32_t> GetLineStarts() const; // 原文の変更不能な行頭表の取得関数
		std::vector<uint32_t> TakeLineStarts() &&; // 消費する正規化結果から原文の行頭表を移譲する関数
	};

	static bool ShouldReadLoneCrAsNewline(const GrammarProfile &Profile); // 単独の CR を行の終わりに読む記法かの判定関数

	static NormalizedInput Normalize(
		const GrammarProfile &Profile,
		const std::string &RawSrc,
		const bool HasFilePreamble = true,
		const bool IsAcceptedInput = false
	); // 入力の受理の判定と表現の正規化関数

	InputNormalizer() = delete; // コンストラクタ（禁止）
};
