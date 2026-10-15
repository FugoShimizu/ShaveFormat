#pragma once

#include "../Core/CommentRepository.hpp"
#include "../Core/SyntaxRole.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct LexemeReading; // トークン化前の値と元の境界の読取情報

// コメント本文の正規化の前方宣言
class CommentNormalizer;

// 本文構造の判定に読むコメントの記録の読取専用の見方（記法とドキュメント形式の定義が示すコード例の書式・本文・記録の順序と所属だけを公開する，整形仕様 3.3）
class ReadOnlyCommentView {
private:

	friend class CommentNormalizer;

	// 本文構造の判定対象の記録と選択範囲
	std::span<const CommentRecord> Records; // 判定する記録の並び
	std::span<const size_t> Selection; // 選んだ記録の位置（空なら全記録）
	std::optional<size_t> GetStructureIndex(const size_t Idx) const; // 未変更の本文が参照する構造の記録位置の取得関数

public:

	const Core::CodeExampleForms &GetCodeExamples(const size_t Idx) const; // 記法の定義のコード例の書式の取得関数
	// 記録の読取操作
	size_t GetSize() const; // 記録の数の取得関数
	std::string_view GetText(const size_t Idx) const; // 本文の取得関数
	size_t GetOrder(const size_t Idx) const; // 原文での順の取得関数
	std::optional<Core::CommentGroupId> GetGroup(const size_t Idx) const; // 所属するグループの取得関数（本文を値として保つディレクティブコメントはグループに属さない）

	ReadOnlyCommentView(const std::span<const CommentRecord> Src, const std::vector<size_t> &Indices); // グループの記録だけを読むコンストラクタ
	explicit ReadOnlyCommentView(const std::span<const CommentRecord> Src); // 記録のグループを所属とするコンストラクタ
};

// コメントの本文構造の判定結果（変更不能，`CommentNormalizer::Inspect` だけが構築する，整形仕様 3.3）
class CommentStructure {
private:

	friend class CommentNormalizer;

	struct GroupFacts; // グループ全体の本文構造の前方宣言

	using SharedFacts = std::shared_ptr<const GroupFacts>; // グループ全体の変更不能な本文構造への共有参照
	using GroupCache = std::map<std::pair<std::string, const Core::CodeExampleForms *>, SharedFacts>; // 記法と本文毎の共有結果

	struct GroupFacts {
		const Core::CodeExampleForms *Forms = nullptr; // 判定に用いたコード例の書式
		std::vector<unsigned char> Code; // 連結本文の保護標識
		bool IsUnclosed = false; // 閉じないコード領域か
		std::shared_ptr<const LexemeReading> Reading; // 展開後の読取値と元の綴りの境界
		std::vector<unsigned char> ReadingCode; // 展開後の本文のコード例の保護標識
	};

	struct RecordFacts {
		std::shared_ptr<const GroupFacts> Data; // グループ全体の変更不能な本文構造
		size_t CodeOffset = 0; // 本文の保護マスクの始まり
		size_t CodeSize = 0; // 本文の保護マスクの長さ
	};

	std::shared_ptr<GroupCache> Cache; // 記法と本文が一致する世代間の共有結果
	std::vector<std::vector<size_t>> Groups; // グループ毎の記録の位置（原文の順）
	std::vector<RecordFacts> Records; // 記録毎の共有構造への参照（本文とマスクをコピーしない）
	std::vector<size_t> RecordGroups; // 記録位置からグループへの対応（ディレクティブはグループ数を番兵とする）

	CommentStructure() = default; // コンストラクタ（`Inspect` だけが使う）

public:

	struct ReadBody {
		std::string_view Text; // 展開後の本文
		std::span<const size_t> OriginalOffsets; // 展開後の境界に対応する元の本文の位置
		std::span<const unsigned char> Code; // 展開後の本文の保護標識
		size_t SourceOffset; // グループ内の元の本文の開始位置
	};

	std::optional<ReadBody> GetReadBody(const size_t Idx) const; // 展開後の本文と元の位置の取得関数（展開が無ければ無い）
	std::span<const std::vector<size_t>> GetGroups() const; // グループ毎の記録の位置の取得関数
	bool HasUnclosedCode(const size_t Idx) const; // 記録のグループが閉じないコード領域を持つかの判定関数
	std::span<const unsigned char> GetCodeMask(const size_t Idx) const; // 記録の本文のコード例の保護標識の取得関数

	CommentStructure &operator=(const CommentStructure &) = delete; // コピー代入演算子（禁止）
	CommentStructure &operator=(CommentStructure &&) = delete; // ムーブ代入演算子（禁止）
	CommentStructure(const CommentStructure &) = default; // コピーコンストラクタ
	CommentStructure(CommentStructure &&) = default; // ムーブコンストラクタ
};

// コメントの本文構造の判定と本文の正規化を集約するクラス（配置と出力記法の選択は `CommentPlacer` が持つ，整形仕様 3.3，7.3）
class CommentNormalizer {
private:

	friend struct CommentNormalizerAccess;
	friend class Core::CommentRepository;

	enum class SentenceQuote : uint8_t { None, ClosedDouble, ClosedSingle, Escaped, OpenDouble, OpenSingle }; // クォートの役割

	// 開いた説明文の括弧・引用（ドキュメントのインラインタグも括弧として数える）
	struct SentenceOpen {
		std::string_view Mark; // 開きのテキスト
	};

	// 文末記号毎の判定と，同じ点列の末尾での文区切
	struct SentenceBoundary {
		size_t Pos; // 記号の始まり
		size_t MarkEnd; // 記号の終わり
		size_t LineBegin; // 所属する物理行の始まり
		size_t LineEnd; // 所属する物理行の終わり
		size_t Next; // 点列の後の本文の始まり
		bool IsEnclosed; // 説明文の括弧・クォートの中か
		bool HasBody; // 文末記号の前に本文が在るか
		bool IsPeriod = false; // 除く句点か
		bool IsSentenceEnd = false; // 後続を判定した文末記号か
		bool IsSpace = false; // クォートの判定を保つ空白へ置き換えるか
		bool IsSplit = false; // 点列の後で論理行を分けるか
		bool IsAttached = false; // 非句点の点と直後の密着を保つか
	};

	struct BodyFacts {
		bool IsValidUtf8 = true; // 本文のデコードが成功したか
		bool HasCommas = false; // 読点の候補が在るか
	}; // 同じ本文世代を読む段の共通の事実

	// 説明文の括弧・引用の対（開き，閉じ，半角の `"` は開閉が同じ字の為に別に数え，半角の `<` `>` は比較・シフト等の演算子と区別出来ない為に含めない）
	static constexpr std::pair<std::string_view, std::string_view> SentenceBrackets[] = {
		{ "(", ")" },
		{ "[", "]" },
		{ "{", "}" },
		{ "‘", "’" },
		{ "“", "”" },
		{ "〈", "〉" },
		{ "《", "》" },
		{ "「", "」" },
		{ "『", "』" },
		{ "【", "】" },
		{ "〔", "〕" },
		{ "〖", "〗" },
		{ "〘", "〙" },
		{ "〚", "〛" },
		{ "（", "）" },
		{ "＜", "＞" },
		{ "［", "］" },
		{ "｛", "｝" }
	};

	static size_t GetSpaceWidth(const std::string_view Text, const size_t Pos); // 文末の後と論理行の端の空白幅の取得関数
	static size_t GetSpaceWidthBefore(const std::string_view Text, const size_t End); // 直前の行端空白幅の取得関数
	static size_t GetSentenceMarkWidth(const std::string_view Text, const size_t Pos); // 文末の記号の幅の取得関数
	// 本文の文字種と読点の内部操作
	static bool IsPhoneticLetter(const char32_t Point); // 表音文字の判定関数
	static bool IsFormatControl(const char32_t Point); // 字形を持たない書式制御の判定関数
	static bool IsHalfWidthQuantityMark(const char32_t Point); // 半角語として扱う単位・通貨記号の判定関数
	static bool IsKeptSentenceMark(const std::string_view Mark); // 本文へ残す文末の記号の判定関数
	static bool IsFullWidth(const char32_t Point); // 全角文字の判定関数
	static bool IsFullWidthMark(const char32_t Point); // 全角の区切記号・括弧・引用符・連結記号の判定関数
	static bool IsSpacedMark(const char32_t Point); // 空白を許す矢印・ダッシュの判定関数
	static bool IsFullWidthWord(const char32_t Point); // 半角語との境界を持つ全角文字の判定関数
	static bool IsWordEdgeChar(const char32_t Point); // 半角語の端に該当する文字の判定関数
	static bool IsAlnumAt(const std::string_view Text, const size_t Pos, const bool IsBefore, size_t *const Work = nullptr); // 指定境界の前後の英数字の判定関数
	// 文境界と本文構造の内部操作
	static size_t GetEscapeWidth(const std::string_view Text, const size_t Pos, const bool IsMultiline); // エスケープした並びのバイト数の取得関数

	static void SplitSentences(
		std::string &Text,
		const std::span<const SentenceBoundary> Boundaries,
		const std::string_view StackMarker,
		const std::string_view SymbolHead,
		std::vector<unsigned char> &Protected
	); // 文境界での論理行の分割関数

	static std::vector<SentenceBoundary> FindSentenceBoundaries(
		const std::string_view Text,
		const std::span<const unsigned char> Protected,
		const bool IsBodyHead,
		size_t *const Work = nullptr,
		size_t *const EndingBytes = nullptr
	); // 文境界の収集関数

	static bool IsNumberTerm(const std::string_view Term); // 数字と半角の点だけの語かの判定関数
	static bool IsComma(const char32_t Point); // 読点候補の判定関数
	static BodyFacts GetBodyFacts(const std::string_view Text, size_t *const Work = nullptr); // UTF-8 と読点候補の共通走査関数
	static void NormalizeCommas(const Core::NotationRules &Rules, std::string &Text, std::vector<unsigned char> &Protected); // 読点

	static void NormalizeSpaces(
		const Core::RegisteredLineForms &Forms,
		std::string &Text,
		const std::string_view TagMarks,
		std::vector<unsigned char> &Protected,
		size_t *const CopiedBytes = nullptr
	); // コメント本文の空白の正規化関数

	static size_t GetQuoteEnd(const std::string_view Text, const size_t Pos, const bool IsMultiline); // 引用値の終端位置の取得関数
	static bool IsQuotedProse(std::string_view Inner); // 引用符の中身が説明文かの判定関数

	static void NormalizeBody(
		const Core::NotationRules &Rules,
		std::string &Text,
		const std::span<const SentenceBoundary> Boundaries,
		const std::string_view StackMarker,
		const std::string_view SymbolHead,
		const bool IsLineEnd,
		const BodyFacts &Facts,
		std::vector<unsigned char> Protected
	); // コメント本文の正規化関数

	static void FinishBody(
		std::string &Text,
		const std::span<const unsigned char> Protected,
		const std::string_view StackMarker,
		const bool IsLineEnd,
		size_t *const Work = nullptr
	); // 論理行の端と積む記号の前置区切の正規化関数

	static std::vector<unsigned char> GetCodeMask(
		const Core::CodeExampleForms &Forms,
		const std::string_view Text,
		bool &IsUnclosed,
		size_t *const TickWork = nullptr
	); // コメント内コードの保護範囲取得関数

	static std::vector<std::vector<size_t>> GetGroupsOf(const ReadOnlyCommentView &View, size_t *const Work = nullptr); // 所属順の記録

public:

	static CommentStructure Inspect(const ReadOnlyCommentView &View, const CommentStructure *Previous = nullptr); // 本文構造の判定関数
	// 本文構造の共有と通常コメントの正規化の公開操作
	static std::string GetStackMarkerOf(const CommentNotation Notation); // 行末へ積んだ次の文の始まりのテキストの取得関数
	static bool IsCommentGap(const std::string_view Gap, const bool CanContainBlankLines); // コメントの間のテキストが空白だけかの判定関数

	static std::string_view GetFirstLogicalLine(
		std::string_view Text,
		const std::string_view StackMarker,
		const std::span<const unsigned char> Protected = std::span<const unsigned char>()
	); // 最初の論理行の取得関数

	static std::vector<size_t> GetSentenceStarts(const std::string_view Text, const std::span<const unsigned char> Protected); // 本文を変更せずに独立した後続文の開始位置を得る関数
	static void Normalize(Core::CommentRepository &Comments); // 通常コメント本文の表記の正規化関数

	CommentNormalizer() = delete; // コンストラクタ（禁止）
};
