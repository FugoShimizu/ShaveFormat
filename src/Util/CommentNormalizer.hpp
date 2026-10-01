#pragma once

#include "../Core/CommentRepository.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class CommentNormalizer;

// 本文構造の判定に読むコメントの記録の読取専用の見方（記法と文書形式の定義が示すコード例の書式・本文・記録の順序と所属だけを公開する，整形仕様 3.3）
class ReadOnlyCommentView {
private:

	std::span<const CommentRecord> Records; // 判定する記録の並び
	std::span<const Core::CommentGroupId> Membership; // 文書の位置で確定した記録毎の所属（空なら記録の群）

public:

	size_t GetSize() const; // 記録の数の取得関数
	const Core::CodeExampleForms &GetCodeExamples(const size_t Index) const; // 記法の定義のコード例の書式の取得関数
	std::string_view GetText(const size_t Index) const; // 本文の取得関数
	size_t GetOrder(const size_t Index) const; // 原文での順の取得関数
	std::optional<Core::CommentGroupId> GetGroup(const size_t Index) const; // 所属する群の取得関数（本文を値として保つ指示コメントは群に属さない）

	ReadOnlyCommentView(const std::span<const CommentRecord> Source, const std::span<const Core::CommentGroupId> Groups); // 所属を与えるコンストラクタ
	explicit ReadOnlyCommentView(const std::span<const CommentRecord> Source); // 記録の群を所属とするコンストラクタ
};

// コメントの本文構造の判定結果（変更不能，`CommentNormalizer::Inspect` だけが構築する，整形仕様 3.3）
class CommentStructure {
	friend class CommentNormalizer;

private:

	std::vector<std::vector<size_t>> Groups; // 群毎の記録の位置（原文の順）
	std::vector<uint8_t> IsUnclosed; // 記録毎の其の群が閉じないコード区画を持つか
	std::vector<std::vector<unsigned char>> CodeMasks; // 記録毎の本文の位置別のコード例の保護標識
	std::vector<std::vector<uint8_t>> KeptBlanks; // 記録毎の論理行毎の構造として保つ空の論理行か（コード例の中の行と，段落とコード例を分ける行）

	CommentStructure() = default; // コンストラクタ（`Inspect` だけが使う）

public:

	std::span<const std::vector<size_t>> GetGroups() const; // 群毎の記録の位置の取得関数
	bool HasUnclosedCode(const size_t Index) const; // 記録の群が閉じないコード区画を持つかの判定関数
	std::span<const unsigned char> GetCodeMask(const size_t Index) const; // 記録の本文のコード例の保護標識の取得関数
	std::span<const uint8_t> GetKeptBlanks(const size_t Index) const; // 記録の論理行毎の構造として保つ空の論理行の印の取得関数

	CommentStructure &operator=(const CommentStructure &) = delete; // コピー代入演算子（禁止）
	CommentStructure &operator=(CommentStructure &&) = delete; // ムーブ代入演算子（禁止）
	CommentStructure(const CommentStructure &) = default; // コピーコンストラクタ
	CommentStructure(CommentStructure &&) = default; // ムーブコンストラクタ
};

// コメントの本文構造の判定と本文の正規化を集約するクラス（配置と出力記法の選択は `CommentPlacer` が持つ，整形仕様 3.3, 7.3）
class CommentNormalizer {
private:

	// 文境界（文末の記号の並びと，其の文の論理行の末尾で閉じ直す括弧・引用）
	struct SentenceBoundary {
		size_t Pos; // 文末の記号の並びの始まり
		size_t MarkEnd; // 文末の記号の並びの終わり
		std::string Close; // 此の論理行の末尾で閉じる括弧・引用
		std::string Reopen; // 次の論理行の頭で開き直す括弧・引用
		bool IsAbbreviation = false; // 略語の点で終わる文か（点を句点として除かない）
	};

	static bool IsQuoteDepthChar(const std::string_view Text, const size_t Pos, size_t &Depth); // 引用括弧の深度更新関数

	static std::vector<SentenceBoundary> FindSentenceBoundaries(
		const std::string_view Text,
		const std::span<const unsigned char> Protected
	); // 文境界の収集関数

	static void SplitSentences(
		std::string &Text,
		const std::span<const SentenceBoundary> Boundaries,
		const std::string_view StackMarker,
		const std::string_view SymbolHead,
		std::vector<unsigned char> &Protected
	); // 文境界での論理行の分割関数

	static void NormalizeCommentSpaces(std::string &Text, const std::string_view TagMarks, std::vector<unsigned char> &Protected); // コメント本文の空白の正規化関数

	static void NormalizeBody(
		std::string &Text,
		std::vector<unsigned char> Protected,
		const std::string_view StackMarker,
		const std::string_view SymbolHead,
		const std::string_view TagMarks,
		const bool IsLineEnd
	); // コメント本文の正規化関数

	static std::vector<unsigned char> CommentCodeMask(
		const std::string_view Text,
		const Core::CodeExampleForms &Forms,
		bool &IsUnclosed,
		std::vector<uint8_t> &KeptBlanks
	); // コメント内コードの保護範囲取得関数

	static std::vector<std::vector<size_t>> GroupsOf(const ReadOnlyCommentView &View); // 群毎の記録の位置の取得関数

public:

	struct SentenceIssue {
		size_t Offset; // 本文中の指摘の開始位置
		bool IsPeriod; // 文末の句点なら真，文区切なら偽
	};

	static std::string StackMarkerOf(const CommentNotation Notation); // 行末へ積んだ次の文の始まりの字面の取得関数
	static std::string_view FirstLogicalLine(std::string_view Text, const std::string_view StackMarker); // 最初の論理行の取得関数

	static bool HasSentenceViolation(
		const std::string_view Text,
		const std::span<const unsigned char> Protected,
		const std::string_view StackMarker
	); // コメントの文区切の違反の判定関数

	static std::vector<SentenceIssue> FindSentenceIssues(
		const std::string_view Text,
		const std::span<const unsigned char> Protected,
		const std::string_view StackMarker
	); // 文末句点と同一論理行の複数文の位置の取得関数

	static bool ContinuesSentence(std::string_view Previous, std::string_view Next); // 前の論理行の英文が次の論理行へ続き得るかの判定関数
	static CommentStructure Inspect(const ReadOnlyCommentView &View); // 本文構造の判定関数
	static void Normalize(Core::CommentRepository &Comments); // 通常コメント本文の表記の正規化関数
	static bool IsCommentGap(const std::string_view Gap, const bool AllowsBlankLines); // コメントの間の字面が空白だけかの判定関数
	static bool CanJoinComments(const CommentNotation First, const CommentNotation Next, const std::string_view Gap); // 同じ群の隣接行コメント判定関数

	CommentNormalizer() = delete; // コンストラクタ（禁止）
};
