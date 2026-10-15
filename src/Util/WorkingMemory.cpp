#include "WorkingMemory.hpp"
#include "FormatLimitExceeded.hpp"
#include "ParserAllocator.hpp"
#include "../Core/CommentRepository.hpp"
#include "../Profile/GrammarProfile.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

struct QuotePrefix; // 引用の接頭辞から確定する補間とエスケープの条件

// ソース又は固定長の入出力領域を参照する受理走査の読出口
class WorkingText {
private:

	static constexpr size_t BufShift = 16; // 固定長の読込領域のバイナリの幅
	const size_t Size; // 既知の原文長
	const std::function<size_t(size_t, std::span<char>)> *const Read; // 記述子上の読出口（ソースを参照する時は無）
	const std::string_view Text; // 呼出側が持つソース
	// 領域に入って居る原文の範囲
	size_t Begin = 0;
	size_t Len = 0;
	std::array<char, size_t{ 1 } << BufShift> Buf; // 原文を保持しない固定長の読込領域（64 KiB）

public:

	size_t GetSize() const; // 原文長の取得関数
	bool IsMatch(const size_t Pos, const std::string_view Pattern, const bool IsFolded = false); // 固定の記法の照合関数
	std::string Copy(const size_t Pos, const size_t Count); // 可変長の区切の取得関数
	std::string_view GetPrefix(const size_t Pos, const size_t Count, std::string &Buffer); // 引用の接頭辞の参照の取得関数
	bool IsLineHeadWord(const size_t Pos, const std::string_view Word); // 行頭のタグと語の区切の照合関数
	size_t LineSpliceEnd(const Core::NotationRules &Notation, const size_t Pos); // 行連結の終端の取得関数
	size_t MatchSplicedBoundary(const Core::NotationRules &Notation, size_t Pos, const std::string_view Pattern); // 行連結を挟む境界の照合関数
	bool IsLineCommentStart(const ReadingRules &Reading, const Core::NotationRules &Notation, const size_t Pos); // 行コメントの開始の判定関数

	const LiteralReading *FindLiteralReading(
		const ReadingRules &Reading,
		const size_t Start,
		const bool CanStartValue,
		size_t *const Work
	); // 引用の読取規則の取得関数

	std::string_view MatchInterpolationHead(
		const LiteralReading &Literal,
		const QuotePrefix &Prefix,
		const size_t Pos,
		size_t &BodyLen
	); // 補間の開きの照合関数

	size_t EstimateWorkingBytes(const GrammarProfile &Profile, size_t *const Work); // 同じ前進走査による作業域の見積関数
	unsigned char operator[](const size_t Pos); // 原文のバイトの取得演算子

	explicit WorkingText(const std::string_view Src); // コンストラクタ（既存のソースを参照する）
	WorkingText(const size_t Bytes, const std::function<size_t(size_t, std::span<char>)> &Reader); // コンストラクタ（固定長の領域で読み出す）
};

/**
 * 原文長の取得関数
 * 計算量：O(1)
 * @return 原文のバイト数
 */
size_t WorkingText::GetSize() const {
	// 既知の原文長の返戻
	return Size;
}

/**
 * 原文のバイトの取得関数
 * 計算量：読込領域長 B と読込の総費用 Q に対し，領域内 O(1)，領域外 O(B + Q)
 * @param Pos 原文の位置
 * @return 原文のバイト
 * @throws std::system_error 予定した領域を読み切れない場合
 * @throws ProcessingInterrupted 書戻以外の処理の中断を要求済の場合
 */
unsigned char WorkingText::operator[](const size_t Pos) {
	// 連続する本文の走査を固定長の確認点で打ち切る
	if(!(Pos & 0XFFF)) ParserAllocator::CheckInterruption();
	// 既存のソースを参照する場合の返戻
	if(!Read) return Text[Pos];
	if(Pos < Begin || Pos >= Begin + Len) {
		Begin = Pos >> BufShift << BufShift;
		Len = std::min(Buf.size(), Size - Begin);
		for(size_t Done = 0; Done < Len;) {
			const size_t Count = (*Read)(Begin + Done, std::span<char>(Buf).subspan(Done, Len - Done));
			if(!Count) throw std::system_error(std::make_error_code(std::errc::state_not_recoverable));
			Done += Count;
		}
	}
	// 固定長の領域内のバイトの返戻
	return Buf[Pos - Begin];
}

/**
 * 位置から始まる固定の記法の照合関数
 * 計算量：記法長 D に対し O(D)
 * @param Pos 始まり
 * @param Pattern 文法の記法
 * @param IsFolded 英字の大小を区別しないか
 * @return 位置から記法が始まれば true
 * @throws std::system_error 原文を読み切れない場合
 */
bool WorkingText::IsMatch(const size_t Pos, const std::string_view Pattern, const bool IsFolded) {
	// 原文の終端を越える記法の返戻
	if(Pattern.size() > Size - Pos) return false;
	// 連続した原文の大小を区別する照合は，読出の分岐を字毎に繰り返さない
	if(!Read && !IsFolded) return Text.substr(Pos, Pattern.size()) == Pattern;
	// 記法の違う事の返戻
	for(size_t Offset = 0; Offset < Pattern.size(); ++Offset) {
		const unsigned char Byte = (*this)[Pos + Offset];
		// 記法と一致しない場合の返戻
		if((IsFolded ? std::tolower(Byte) : Byte) != Pattern[Offset]) return false;
	}
	// 記法の一致の返戻
	return true;
}

/**
 * 引用の可変長の区切の取得関数
 * 計算量：区切長 D に対し O(D)，追加領域 O(D)
 * @param Pos 始まり
 * @param Count 長さ
 * @return 区切のテキスト
 * @throws std::bad_alloc 区切の領域を確保出来ない場合
 * @throws std::system_error 原文を読み切れない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::string WorkingText::Copy(const size_t Pos, const size_t Count) {
	// 結果の格納先
	if(!Read) return std::string(Text.substr(Pos, Count));
	std::string Result;
	Result.reserve(Count);
	for(size_t Offset = 0; Offset < Count; ++Offset) Result += (*this)[Pos + Offset];
	// 可変長の区切の返戻
	return Result;
}

/**
 * 引用の接頭辞の参照を取得する関数
 * 計算量：ソース参照は O(1)，記述子読込は区間長 P に対し O(P)
 * @param Pos 始まり
 * @param Count 長さ
 * @param Buffer 記述子から読んだ接頭辞を保持する格納先
 * @return ソース又は格納先に在る接頭辞の参照
 * @throws std::bad_alloc 接頭辞の領域を確保出来ない場合
 * @throws std::system_error 原文を読み切れない場合
 * @throws std::length_error 接頭辞の長さが文字列の上限を超える場合
 */
std::string_view WorkingText::GetPrefix(const size_t Pos, const size_t Count, std::string &Buffer) {
	// 連続したソースを持つ呼出では接頭辞を複写しない事の返戻
	if(!Read) return Text.substr(Pos, Count);
	Buffer = Copy(Pos, Count);
	// 読込領域の切替に依って無効に為らない参照の返戻
	return Buffer;
}

/**
 * 受理計数で語を構成するバイトの判定関数
 * 計算量：O(1)
 * @param Byte 原文のバイト
 * @return 語の構成バイトなら true
 */
static bool IsWordByte(const unsigned char Byte) {
	// ASCII の英数字・名前の記号と非 ASCII バイトの返戻
	return Byte > 0X7F || Byte == '_' || Byte == '$' || Byte >= '0' && Byte <= '9' || Byte >= 'A' && Byte <= 'Z' ||
	Byte >= 'a' && Byte <= 'z';
}

/**
 * 行頭の埋込ドキュメントのタグの照合関数
 * 計算量：タグ長 D に対し O(D)
 * @param Pos 行頭
 * @param Word 文法のタグ
 * @return タグと語の区切が有れば true
 * @throws std::system_error 原文を読み切れない場合
 */
bool WorkingText::IsLineHeadWord(const size_t Pos, const std::string_view Word) {
	// 照合する範囲の終端
	const size_t End = Pos + Word.size();
	// 行頭のタグと語の区切の一致の返戻
	return IsMatch(Pos, Word) &&
	(End == GetSize() || (*this)[End] == '\n' || (*this)[End] == '\r' || (*this)[End] == ' ' || (*this)[End] == '\t');
}

/**
 * 登録した行連結の原文上の終端の取得関数
 * 計算量：記法の総長 F に対し O(F)
 * @param Notation 文法の行連結の記法
 * @param Pos 候補の位置
 * @return 行連結の直後の位置，一致しなければ０
 * @throws std::system_error 原文を読み切れない場合
 */
size_t WorkingText::LineSpliceEnd(const Core::NotationRules &Notation, const size_t Pos) {
	for(const std::string_view Splice : Notation.LineSplices) {
		const size_t End = Pos + Splice.size();
		if(End >= GetSize() || !IsMatch(Pos, Splice)) continue;
		// LF 又は CRLF・単独 CR の後の位置の返戻
		if((*this)[End] == '\n') return End + 1;
		// CR と，続く LF が在れば其の後の位置の返戻
		if((*this)[End] == '\r') return End + 1 + (End + 1 < GetSize() && (*this)[End + 1] == '\n');
	}
	// 行連結でない場合の返戻
	return 0;
}

/**
 * 行連結の後の語・記号の境界の照合関数
 * 原文の位置を返し，バイトと LF の計数は呼出側の同じ走査で行う
 * 計算量：境界長 D，間の行連結の総長 S，登録数 F に対し O(D + S F)
 * @param Notation 文法の行連結の記法
 * @param Pos 境界の開始
 * @param Pattern 照合する語又は記号
 * @return 一致した境界の原文の終端，不一致なら０
 * @throws std::system_error 原文を読み切れない場合
 */
size_t WorkingText::MatchSplicedBoundary(const Core::NotationRules &Notation, size_t Pos, const std::string_view Pattern) {
	for(size_t Index = 0; Index < Pattern.size(); ++Index) {
		// 境界の文字と一致しない場合の返戻
		if(Pos == GetSize() || (*this)[Pos++] != Pattern[Index]) return 0;
		if(Index + 1 == Pattern.size()) break;
		while(const size_t After = LineSpliceEnd(Notation, Pos)) Pos = After;
	}
	// 原文上の境界の終端の返戻
	return Pos;
}

/**
 * 文法の登録から行コメントの開始を判定する関数
 * コメントの記号で始まる専用構文はコードの接頭辞で区別する
 * 計算量：コメント記法数 A，接頭辞の総長 P，行連結の総長 S と登録数 F に対し O(A + P + S F + 1)
 * @param Reading 文法の読取規則
 * @param Notation コメントと行連結の記法
 * @param Pos 開始の位置
 * @return 行コメントの開始なら true
 * @throws std::system_error 原文を読み切れない場合
 */
bool WorkingText::IsLineCommentStart(const ReadingRules &Reading, const Core::NotationRules &Notation, const size_t Pos) {
	// コメントの記法との一致と，コードの接頭辞に該当しない事の返戻
	return (
		(*this)[Pos] == '/' && Notation.IsAvailable(CommentNotation::SlashLine) && MatchSplicedBoundary(Notation, Pos, "//") ||
		(*this)[Pos] == '#' && Notation.IsAvailable(CommentNotation::HashLine)
	) && std::ranges::none_of(
		Reading.LineCommentCodeHeads,
		[&](const std::string_view Head) -> bool {
			// 同じ記号で始まる専用構文かの返戻
			return IsMatch(Pos, Head);
		}
	);
}

struct PendingLiteral; // 開始行で控えたヒアドキュメントと本文の対応

// 原文の計数と，トークン化前の展開後の境界を同じ前進走査で保持する状態
struct WorkingScan {
	enum class UnicodeRegion : uint8_t { Code, BlockComment, Invalid, LineComment, Quoted, TextBlock }; // 展開後の境界状態（先頭は既定値）
	const bool HasUnicodePrelexing; // Unicode エスケープをトークン化前に展開する文法か
	size_t *const LiteralWork; // 引用の記法の照合回数（無ければ観測しない）
	const std::array<bool, 256> &LiteralStarts; // 此の文法の引用が始まり得るバイト
	WorkingText &Src; // 原文
	size_t Pos = 0; // 次に計数する位置
	bool IsWord = false; // 直前が語の構成バイトか
	size_t Units = 0; // 領域を畳まない語・記号の数
	size_t Lines = 0; // 原文の LF の数
	bool HasEligibleUnicodeBackslash = false; // 適格な生のバックスラッシュを保留して居るか
	bool IsOddUnicodeBackslashes = false; // 展開後の連続するバックスラッシュが奇数個か
	bool WasUnicodeEscape = false; // 直前の文字が Unicode 展開の結果か
	bool IsUnicodePrefix = false; // Unicode エスケープの u の並びを読む途中か
	uint8_t UnicodeDigits = 0; // 読取済の１６進の桁数
	char32_t UnicodeVal = 0; // エスケープの値
	size_t UnicodePointUnits = 0; // 保留したバックスラッシュの前の計数
	UnicodeRegion Region = UnicodeRegion::Code; // 展開後のコメント・引用の状態
	char32_t PrevUnicodePoint = 0; // コメントの境界の１文字目
	size_t PrevUnicodeUnits = 0; // 境界の１文字目の前の計数
	char32_t UnicodeQuote = 0; // 引用の区切
	uint8_t UnicodeQuoteRun = 0; // 引用の開き又はテキストブロックの閉じの連続数
	bool IsUnicodeQuotedEscape = false; // 引用の中の直前のバックスラッシュ
	size_t UnicodeRegionUnits = 0; // 最外領域の開始前の計数
	size_t EmptyQuoteUnits = 0; // 空の文字列の終端迄の計数
	size_t CollapsedUnicodeUnits = 0; // 閉じた最外領域の内部計数
	size_t UnicodeRegions = 0; // 閉じた最外領域の数
	size_t PrefixStart = 0; // 引用の接頭辞を含み得る連続範囲の始まり
	size_t PrefixUnits = 0; // 引用の接頭辞の前のトークン数
	void CloseUnicodeRegion(const size_t EndUnits); // 閉じた最外領域の計数の採用関数
	void ReadUnicodePoint(const char32_t Point, const size_t BeforeUnits, const bool IsExpanded); // 展開後の境界の更新関数
	void Advance(); // 原文の１バイトの受理計数関数
	void ScanLineComment(const Core::NotationRules &Notation); // 行コメントの受理走査関数
	size_t ReadLiteralHeader(const LiteralReading &Literal, std::string &Close); // 行頭の語で閉じるリテラルの開始の読取関数
	bool HasClosedLiteralBody(const PendingLiteral &Pending, bool &HasInterpolation); // 本文と行頭の終端の受理走査関数
	void ScanInterpolation(const GrammarProfile &Profile, const std::string_view Close); // 補間の式と内部リテラルの受理走査関数

	size_t QuotedEnd(
		const GrammarProfile &Profile,
		const LiteralReading &Literal,
		const size_t Start,
		bool &HasInterpolation,
		size_t &PrefixUnits
	); // 引用範囲の終端の取得関数

	bool CanCollapseRegexCandidate(const GrammarProfile &Profile); // 正規表現候補の受理走査関数
	void ScanEmbeddedCandidate(const ReadingRules &Reading, const std::string_view Element); // 埋込本文の受理走査関数
};

/**
 * 展開後に閉じた最外領域の計数の採用関数
 * @param EndUnits 領域の終端迄の原文の計数
 */
void WorkingScan::CloseUnicodeRegion(const size_t EndUnits) {
	CollapsedUnicodeUnits += EndUnits - UnicodeRegionUnits;
	++UnicodeRegions;
	Region = UnicodeRegion::Code;
	PrevUnicodePoint = 0;
	// 展開した閉じが原文の語の途中に在る場合，後続の領域外の語を独立して数える
	if(EndUnits == Units) IsWord = false;
	// 終了
	return;
}

/**
 * 展開後の１文字に依るコメント・引用の境界の更新関数
 * 計算量：O(1)
 * @param Point 原文の文字又は展開した値
 * @param BeforeUnits 此の文字の原文の開始前の計数
 * @param IsExpanded Unicode 展開の結果か
 */
void WorkingScan::ReadUnicodePoint(const char32_t Point, const size_t BeforeUnits, const bool IsExpanded) {
	IsOddUnicodeBackslashes = Point == '\\' && !IsOddUnicodeBackslashes;
	WasUnicodeEscape = IsExpanded;
	// ２つの引用符は空の文字列で，３つ目を読んだ場合だけテキストブロックを開く
	if(Region == UnicodeRegion::Quoted && UnicodeQuoteRun) {
		if(Point == '"') {
			if(++UnicodeQuoteRun == 3) {
				Region = UnicodeRegion::TextBlock;
				UnicodeQuoteRun = 0;
			} else EmptyQuoteUnits = Units;
			// 開き又は空の文字列の引用符の更新の終了
			return;
		}
		if(UnicodeQuoteRun == 2) CloseUnicodeRegion(EmptyQuoteUnits);
		UnicodeQuoteRun = 0;
	}
	switch(Region) {
	case UnicodeRegion::Code:
		if(PrevUnicodePoint == '/' && (Point == '/' || Point == '*')) {
			Region = Point == '/' ? UnicodeRegion::LineComment : UnicodeRegion::BlockComment;
			UnicodeRegionUnits = PrevUnicodeUnits;
			PrevUnicodePoint = 0;
		} else if(Point == '"' || Point == '\'') {
			Region = UnicodeRegion::Quoted;
			UnicodeRegionUnits = BeforeUnits;
			UnicodeQuote = Point;
			UnicodeQuoteRun = Point == '"' ? 1 : 0;
			PrevUnicodePoint = 0;
		} else {
			PrevUnicodePoint = Point == '/' ? Point : 0;
			PrevUnicodeUnits = BeforeUnits;
		}
		break;
	case UnicodeRegion::BlockComment:
		if(PrevUnicodePoint == '*' && Point == '/') CloseUnicodeRegion(Units);
		else PrevUnicodePoint = Point == '*' ? Point : 0;
		break;
	case UnicodeRegion::LineComment:
		if(Point == '\r' || Point == '\n') CloseUnicodeRegion(BeforeUnits);
		break;
	case UnicodeRegion::Quoted:
		if(IsUnicodeQuotedEscape) IsUnicodeQuotedEscape = false;
		else if(Point == '\\') IsUnicodeQuotedEscape = true;
		else if(Point == UnicodeQuote) CloseUnicodeRegion(Units);
		else if(Point == '\r' || Point == '\n') Region = UnicodeRegion::Invalid;
		break;
	case UnicodeRegion::TextBlock:
		if(IsUnicodeQuotedEscape) {
			IsUnicodeQuotedEscape = false;
			UnicodeQuoteRun = 0;
		} else if(Point == '"') {
			if(++UnicodeQuoteRun == 3) {
				CloseUnicodeRegion(Units);
				UnicodeQuoteRun = 0;
			}
		} else {
			UnicodeQuoteRun = 0;
			IsUnicodeQuotedEscape = Point == '\\';
		}
		break;
	case UnicodeRegion::Invalid:
		break;
	}
	// 終了
	return;
}

/**
 * 原文の１バイトの受理計数関数
 * 計算量：原文の読込費用 Q に対し O(1 + Q)
 * @throws std::system_error 原文を読み切れない場合
 */
void WorkingScan::Advance() {
	const unsigned char Byte = Src[Pos];
	// 展開で閉じた空の文字列と，直後の原文の語を別々に数える
	if(
		HasUnicodePrelexing && Region == UnicodeRegion::Quoted && UnicodeQuoteRun == 2 && !HasEligibleUnicodeBackslash &&
		!IsUnicodePrefix && !UnicodeDigits && Byte != '"' && Byte != '\\'
	) {
		CloseUnicodeRegion(EmptyQuoteUnits);
		UnicodeQuoteRun = 0;
	}
	const size_t BeforeUnits = Units;
	const bool IsNextWord = IsWordByte(Byte);
	if(IsNextWord) {
		if(!IsWord) ++Units;
	} else if(Byte != 0X20 && (Byte < '\t' || Byte > '\r')) ++Units;
	if(Byte == '\n') ++Lines;
	IsWord = IsNextWord;
	if(HasUnicodePrelexing) {
		// 適格なバックスラッシュは次の文字を待ち，u でなければ元の文字として境界へ渡す
		if(IsUnicodePrefix || UnicodeDigits) {
			if(!(IsUnicodePrefix && Byte == 'u')) {
				const uint32_t Digit = Byte >= '0' && Byte <= '9' ?
				Byte - '0' :
				Byte >= 'A' && Byte <= 'F' ? Byte + 10 - 'A' : Byte >= 'a' && Byte <= 'f' ? Byte + 10 - 'a' : 16;
				IsUnicodePrefix = false;
				if(Digit < 16) {
					UnicodeVal = UnicodeVal << 4 | Digit;
					if(++UnicodeDigits == 4) {
						ReadUnicodePoint(UnicodeVal, UnicodePointUnits, true);
						UnicodeDigits = 0;
					}
				} else {
					Region = UnicodeRegion::Invalid;
					UnicodeDigits = 0;
				}
			}
		} else if(Byte == 'u' && HasEligibleUnicodeBackslash) {
			HasEligibleUnicodeBackslash = false;
			IsUnicodePrefix = true;
			UnicodeVal = 0;
		} else {
			if(HasEligibleUnicodeBackslash) ReadUnicodePoint('\\', UnicodePointUnits, false);
			HasEligibleUnicodeBackslash = Byte == '\\' && (WasUnicodeEscape || !IsOddUnicodeBackslashes);
			if(HasEligibleUnicodeBackslash) UnicodePointUnits = BeforeUnits;
			else ReadUnicodePoint(Byte, BeforeUnits, false);
		}
	}
	++Pos;
	if(!IsNextWord && Byte != '@' && Byte != '#') {
		PrefixStart = Pos;
		PrefixUnits = Units;
	}
	// 終了
	return;
}

/**
 * 位置で開く固定引用又は専用区切の読取規則の取得関数
 * 計算量：登録数 F と最大開長 D に対し O(FD)
 * @param Reading 入口の読取規則
 * @param Start 開きを照合する位置
 * @param CanStartValue 値を始めれる位置か（演算子と共有する開きの区別）
 * @param Work 引用の記法の照合回数の格納先
 * @return 一致する規則，無ければ nullptr
 * @throws std::system_error 原文を読み切れない場合
 */
const LiteralReading *WorkingText::FindLiteralReading(
	const ReadingRules &Reading,
	const size_t Start,
	const bool CanStartValue,
	size_t *const Work
) {
	const unsigned char Byte = (*this)[Start];
	// 長い開きからの照合
	for(const LiteralReading &Rule : Reading.Literals) {
		if(Work) ++*Work;
		if(Rule.CanShareOperatorHead && !CanStartValue || Rule.Open.front() != Byte || !IsMatch(Start, Rule.Open)) continue;
		if(Rule.Delimiter == LiteralDelimiter::LineWord) {
			size_t After = Start + Rule.Open.size();
			if(Rule.CanSpaceBeforeWord) while(After < GetSize() && ((*this)[After] == ' ' || (*this)[After] == '\t')) ++After;
			if(!IsMatch(After, Rule.Close)) continue;
		}
		if(Rule.Delimiter == LiteralDelimiter::Paired) {
			const size_t After = Start + Rule.Open.size();
			if(After == GetSize() || std::isalnum((*this)[After]) || (*this)[After] == '_' || std::isspace((*this)[After])) continue;
		}
		// 一致した開きの返戻
		return &Rule;
	}
	// 登録された開きが無い事の返戻
	return nullptr;
}

/**
 * 引用の表からエスケープ記号で始まる補間の記法を引く関数
 * 計算量：固定の登録と補間の記法の総数 F に対し O(F)
 * @param Reading 入口の読取規則
 * @return 正規表現と引用が共有する補間の開き，無ければ空
 */
static std::string_view GetEscapedInterpolationHead(const ReadingRules &Reading) {
	// 引用毎の補間の記法の照合
	for(const LiteralReading &Literal : Reading.Literals) {
		// エスケープ記号で開く補間の返戻
		for(const std::string_view Head : Literal.InterpolationHeads) if(Head.front() == '\\') return Head;
	}
	// 対象の補間が無い事の返戻
	return {};
}

// 引用の接頭辞から確定する補間とエスケープの条件
struct QuotePrefix {
	bool IsHash = false; // 井桁の区切を持つか
	bool IsVerbatim = false; // 引用符の二重化で逃がすか
	bool CanInterpolate = false; // 此の接頭辞で補間を読めるか
	bool HasRepeatedPrefix = false; // 連続数で定める接頭辞が在るか
	std::string InterpolationOpen = {}; // 可変長の補間の開き
	std::string InterpolationWord = {}; // 式の括弧を持たない補間の開き
	static QuotePrefix Read(const ReadingRules &Reading, const LiteralReading &Literal, const std::string_view Prefix); // 接頭辞と補間区切の読取関数
};

/**
 * 登録した引用の接頭辞と補間区切を読む関数
 * 計算量：接頭辞長 P と固定の登録総長 F に対し O(P + F)，追加領域 O(P)
 * @param Reading 入口の読取規則
 * @param Literal 引用毎の境界の事実
 * @param Prefix 引用符の前の接頭辞
 * @return 補間とエスケープの条件
 * @throws std::bad_alloc 補間の区切を確保出来ない場合
 * @throws std::length_error 区切の長さの上限を超える場合
 */
QuotePrefix QuotePrefix::Read(const ReadingRules &Reading, const LiteralReading &Literal, const std::string_view Prefix) {
	// 井桁と逐語文字列の接頭辞の区別
	QuotePrefix Result;
	const size_t HashStart = std::min(Prefix.find('#'), Prefix.size());
	Result.IsHash = HashStart < Prefix.size() && std::ranges::all_of(
		Prefix.substr(HashStart),
		[](const char Byte) -> bool {
			// 井桁だけの区切かの返戻
			return Byte == '#';
		}
	) && std::ranges::find(Reading.HashQuotePrefixes, Prefix.substr(0, HashStart)) != Reading.HashQuotePrefixes.end();
	Result.IsVerbatim = std::ranges::find(Reading.VerbatimStrPrefixes, Prefix) != Reading.VerbatimStrPrefixes.end();
	// 接頭辞を要する補間は登録した接頭辞又は連続数の区切だけに限る
	const size_t PrefixCount = std::ranges::count_if(
		Prefix,
		[&Literal](const char Byte) -> bool {
			// 補間を有効にする文字かの返戻
			return Literal.InterpolationPrefixes.find(Byte) != std::string_view::npos;
		}
	);
	const bool IsRepeated = !Prefix.empty() && !Literal.RepeatedInterpolationPrefix.empty() && std::ranges::all_of(
		Prefix,
		[&Literal](const char Byte) -> bool {
			// 連続数を定める接頭辞の文字かの返戻
			return Literal.RepeatedInterpolationPrefix.find(Byte) != std::string_view::npos;
		}
	);
	Result.HasRepeatedPrefix = IsRepeated;
	Result.CanInterpolate = Literal.InterpolationPrefixes.empty() ||
	PrefixCount && (IsRepeated || std::ranges::find(Reading.StrPrefixes, Prefix) != Reading.StrPrefixes.end());
	if(IsRepeated) {
		Result.InterpolationOpen.assign(Prefix.size(), Literal.InterpolationHeads.front().front());
		if(!Literal.HasRepeatedInterpolationHead) {
			Result.InterpolationWord = Result.InterpolationOpen;
			Result.InterpolationOpen.append(Literal.InterpolationHeads.front().substr(1));
		}
	} else if(Result.IsHash) {
		Result.InterpolationOpen = GetEscapedInterpolationHead(Reading);
		if(!Result.InterpolationOpen.empty()) Result.InterpolationOpen.insert(1, Prefix.substr(HashStart));
	}
	// 同じ事実を最外と補間内の引用へ渡す為の返戻
	return Result;
}

/**
 * 引用本文で補間を始める登録済の区切の照合関数
 * 計算量：区切総長 D と同じ開きの連続数 R に対し O(D + R)
 * @param Literal 引用の読取事実
 * @param Prefix 接頭辞から確定した補間の条件
 * @param Pos 本文の位置
 * @param BodyLen 補間でなく本文として読み進める長さの格納先
 * @return 一致する補間の開き，不一致なら空
 * @throws std::system_error 原文を読み切れない場合
 */
std::string_view WorkingText::MatchInterpolationHead(
	const LiteralReading &Literal,
	const QuotePrefix &Prefix,
	const size_t Pos,
	size_t &BodyLen
) {
	// 接頭辞が補間を無効にする引用の返戻
	if(!Prefix.CanInterpolate) return {};
	for(const std::string_view Pattern : Literal.InterpolationHeads) {
		const std::string_view Head = !Prefix.InterpolationWord.empty() && Pattern.size() == 1 ?
		std::string_view(Prefix.InterpolationWord) :
		!Prefix.InterpolationOpen.empty() &&
		(Literal.HasRepeatedInterpolationHead || Pattern.front() == '\\' || !Literal.RepeatedInterpolationPrefix.empty()) ?
		std::string_view(Prefix.InterpolationOpen) :
		Pattern;
		const size_t RepeatLen = Literal.HasRepeatedInterpolationHead ? Head.size() : Prefix.InterpolationWord.size();
		if(RepeatLen && (*this)[Pos] == Head.front()) {
			// 連続数を１度数え，短い区切や余分な本文の開きで同じ列を繰り返し照合しない
			size_t End = Pos + 1;
			while(End < GetSize() && (*this)[End] == Head.front()) ++End;
			const size_t Run = End - Pos;
			if(Run != RepeatLen) {
				BodyLen = Run < RepeatLen ? Run : Run - RepeatLen;
				// 本文だけを読み進める事の返戻
				return {};
			}
		}
		if(!IsMatch(Pos, Head)) continue;
		if(Literal.HasDoubledInterpolationEscape && IsMatch(Pos + Head.size(), Head)) {
			BodyLen = Head.size() << 1;
			// 二重化した開きを本文として読む事の返戻
			return {};
		}
		// 一致した区切の返戻
		return Head;
	}
	// 補間を始めない位置の返戻
	return {};
}

/**
 * 文法の終端と行継続に従う行コメントの受理走査関数
 * 開きから走査し，終端の直前又は入力末尾迄位置と保留計数を更新する
 * 計算量：本文長 B と固定の終端・行継続の記法数 F に対し O(B(F + 1))
 * @param Notation 入口の文法のコメント記法
 * @throws std::system_error 原文を読み切れない場合
 */
void WorkingScan::ScanLineComment(const Core::NotationRules &Notation) {
	// 終端と行継続の照合
	for(; Pos < Src.GetSize(); Advance()) {
		const size_t SpliceEnd = Src[Pos] == '\n' && Pos && Src[Pos - 1] == '\r' ? Pos - 1 : Pos;
		if(
			const bool IsLineEnd =
			Src[Pos] == '\n' || !Notation.LineCommentTerminator.empty() && Src.IsMatch(Pos, Notation.LineCommentTerminator) ||
			std::ranges::any_of(
				Notation.LineCommentBreaks,
				[this](const std::string_view Break) -> bool {
					// 文法の行区切で終わるかの返戻
					return Src.IsMatch(Pos, Break);
				}
			), IsSpliced = IsLineEnd && std::ranges::any_of(
				Notation.LineSplices,
				[this, SpliceEnd](const std::string_view Splice) -> bool {
					// LF 又は CRLF の前の行継続かの返戻
					return Splice.size() <= SpliceEnd && Src.IsMatch(SpliceEnd - Splice.size(), Splice);
				}
			); IsLineEnd && !IsSpliced
		) break;
	}
	// 終了
	return;
}

// 開始行で控えたヒアドキュメントと本文の対応
struct PendingLiteral {
	const LiteralReading *Reading; // 入口で確定した終端と補間の記法
	std::string Close = {}; // 行頭で閉じる語
	size_t HeaderUnits = 0; // 本文が閉じた時だけ取り除く開始のトークン数
};

/**
 * 行頭の語で閉じるリテラルの開始の読取関数
 * 開きからの位置と保留計数を更新する
 * 計算量：開始長 D に対し O(D)，追加領域 O(D)
 * @param Literal 開始行の引用と終端の規則
 * @param Close 本文を閉じる語（開始を確定出来なければ空）
 * @return 開始行の残りを含めない開きの終端
 * @throws std::bad_alloc 終端の語を確保出来ない場合
 * @throws std::length_error 終端の語の長さの上限を超える場合
 * @throws std::system_error 原文を読み切れない場合
 */
size_t WorkingScan::ReadLiteralHeader(const LiteralReading &Literal, std::string &Close) {
	// 開きの記法と，許される行内空白の後の語を囲む引用符の読取
	const size_t After = Pos + Literal.Open.size();
	while(Pos < After) Advance();
	if(Literal.CanSpaceBeforeWord) while(Pos < Src.GetSize() && (Src[Pos] == ' ' || Src[Pos] == '\t')) Advance();
	const size_t QuoteEnd = Pos + Literal.Close.size();
	while(Pos < QuoteEnd) Advance();
	const size_t Start = Pos;
	if(!Literal.Close.empty()) {
		while(Pos < Src.GetSize() && Src[Pos] != '\r' && Src[Pos] != '\n' && !Src.IsMatch(Pos, Literal.Close)) Advance();
	} else if(Start < Src.GetSize() && (Src[Start] == '_' || std::isalpha(Src[Start]) || Src[Start] > 0X7F)) {
		do Advance();
		while(Pos < Src.GetSize() && (Src[Pos] == '_' || std::isalnum(Src[Pos]) || Src[Pos] > 0X7F));
	}
	if(Pos > Start && Src.IsMatch(Pos, Literal.Close)) {
		Close = Src.Copy(Start, Pos - Start);
		const size_t End = Pos + Literal.Close.size();
		while(Pos < End) Advance();
	}
	// 読取済の開始行の終端の返戻
	return Pos;
}

/**
 * 開始行の後に並ぶ本文と行頭の終端の受理走査関数
 * 次の行の本文から位置と保留計数を更新する
 * 計算量：本文長 B，固定記法数 F，終端長 D に対し O(B(F + 1) + D)
 * @param Pending 対応する開始行の読取事実
 * @param HasInterpolation 補間の有無の格納先（内側を既に T とする場合は true）
 * @return 本文と終端を確定出来たか
 * @throws std::system_error 原文を読み切れない場合
 */
bool WorkingScan::HasClosedLiteralBody(const PendingLiteral &Pending, bool &HasInterpolation) {
	// 本文と終端の区別
	const LiteralReading &Literal = *Pending.Reading;
	while(Pos < Src.GetSize()) {
		if(!Pos || Src[Pos - 1] == '\n') {
			size_t Head = Pos;
			if(Literal.CanIndentEnd) while(Head < Src.GetSize() && (Src[Head] == ' ' || Src[Head] == '\t')) ++Head;
			const size_t End = Head + Pending.Close.size();
			if(
				Src.IsMatch(Head, Pending.Close) && (End == Src.GetSize() || Literal.EndFollowers.find(Src[End]) != std::string_view::npos)
			) {
				while(Pos < End) Advance();
				// 本文と終端が閉じた事の返戻
				return true;
			}
		}
		HasInterpolation = HasInterpolation || std::ranges::any_of(
			Literal.InterpolationHeads,
			[this](const std::string_view Head) -> bool {
				// 本文の補間の開始かの返戻
				return Src.IsMatch(Pos, Head);
			}
		);
		if(!Literal.Escape.empty() && Src.IsMatch(Pos, Literal.Escape) && Pos + 1 < Src.GetSize()) Advance();
		Advance();
	}
	// 未閉じの本文の返戻
	return false;
}

/**
 * 専用区切の開きの１文字に対応する閉じの取得関数
 * 計算量：固定の対応数 F に対し O(F)
 * @param Literal 専用区切の読取事実
 * @param Open 開きの１文字（ネストしない区切なら０へ更新）
 * @return 対応する閉じ，対応が無ければ元の開きと同じ１文字
 */
static char GetPairedLiteralClose(const LiteralReading &Literal, char &Open) {
	// 固定の対応列の照合
	const size_t Pair = Literal.PairedOpen.find(Open);
	const char Close = Pair == std::string_view::npos ? Open : Literal.Close[Pair];
	if(Pair == std::string_view::npos || !Literal.CanNest) Open = 0;
	// １文字の区切の返戻
	return Close;
}

/**
 * 補間の式の括弧と内部リテラルを区別する受理走査関数
 * 補間の開きの直後から位置と保留計数を更新する
 * 計算量：本文長 B と固定の記法総長 F に対し O(B(F + 1))，ネストの段数 H と区切総長 D に対し追加領域 O(H + D)
 * @param Profile 入口の文法
 * @param Close 補間の式を閉じる区切
 * @throws std::bad_alloc 作業枠と区切を確保出来ない場合
 * @throws std::length_error 作業枠又は区切の上限を超える場合
 * @throws std::system_error 原文を読み切れない場合
 */
void WorkingScan::ScanInterpolation(const GrammarProfile &Profile, const std::string_view Close) {
	// 式・引用・正規表現の作業枠
	struct Frame {
		std::string Close; // 此の範囲の終端
		const LiteralReading *Literal = nullptr; // 引用の本文なら読取事実
		char NestedOpen = 0; // 対応区切のネストの開き
		size_t Depth = 0; // 対応区切又は文字集合の段数
		bool IsRegex = false; // 正規表現の本文か
		bool CanStartRegex = true; // 式で正規表現を開始出来るか
		bool IsDot = false; // 語が点の後に在るか
		QuotePrefix Prefix = {}; // 引用と共有する接頭辞と補間の条件
	};
	std::vector<Frame> Frames { { std::string(Close) } };
	const ReadingRules &Reading = Profile.GetReading();
	const Core::NotationRules &Notation = Profile.GetRules().Notation;
	const std::string_view EscapedInterpolationHead = GetEscapedInterpolationHead(Reading);
	std::vector<PendingLiteral> PendingLiterals;
	bool HasBodyInterpolation = true; // 補間内の本文は終端だけを読み，内部計数を C へ畳まない
	while(!Frames.empty() && Pos < Src.GetSize()) {
		if(!PendingLiterals.empty() && Pos && Src[Pos - 1] == '\n') {
			for(size_t Index = 0; Index < PendingLiterals.size(); ++Index) {
				HasClosedLiteralBody(PendingLiterals[Index], HasBodyInterpolation);
				if(Index + 1 == PendingLiterals.size()) break;
				while(Pos < Src.GetSize() && Src[Pos] != '\n') Advance();
				if(Pos < Src.GetSize()) Advance();
			}
			PendingLiterals.clear();
			continue;
		}
		Frame &Current = Frames.back();
		const unsigned char Byte = Src[Pos];
		if(
			Current.Close.size() > 1 && (!Current.Literal || Current.Literal->Delimiter == LiteralDelimiter::Repeated) &&
			Byte == Current.Close.front()
		) {
			// 短い閉じの連続列も１度だけ読み，接尾部毎の再照合を避ける
			size_t End = Pos + 1;
			while(End < Src.GetSize() && Src[End] == Byte) ++End;
			if(End - Pos < Current.Close.size()) {
				while(Pos < End) Advance();
				continue;
			}
		}
		if(Current.Literal || Current.IsRegex) {
			// 本文で始まる次の補間の区別
			size_t HeadLen = 0;
			char ExpressionClose = 0;
			if(Current.Literal) {
				size_t BodyLen = 0;
				const std::string_view Head = Src.MatchInterpolationHead(*Current.Literal, Current.Prefix, Pos, BodyLen);
				if(BodyLen) {
					const size_t End = Pos + BodyLen;
					while(Pos < End) Advance();
					continue;
				}
				if(!Head.empty()) {
					HeadLen = Head.size();
					ExpressionClose = Head.front() == '{' || Head.back() == '{' ? '}' : Head.back() == '(' ? ')' : 0;
				}
			} else if(!EscapedInterpolationHead.empty() && Src.IsMatch(Pos, EscapedInterpolationHead)) {
				HeadLen = EscapedInterpolationHead.size();
				ExpressionClose = EscapedInterpolationHead.back() == '(' ? ')' : '}';
			} else if(
				Byte == '#' && Pos + 1 < Src.GetSize() && Profile.GetRules().HashInterpolationHeads.find(Src[Pos + 1]) != std::string_view::npos
			) {
				HeadLen = 2;
				ExpressionClose = Src[Pos + 1] == '{' ? '}' : 0;
			}
			if(ExpressionClose) {
				const size_t End = Pos + HeadLen;
				while(Pos < End) Advance();
				Frames.push_back(
					{ std::string(Current.Literal && Current.Literal->HasRepeatedInterpolationHead ? HeadLen : 1, ExpressionClose) }
				);
				continue;
			}
			const std::string_view Escape = Current.Literal ? Current.Literal->Escape : std::string_view("\\");
			if(
				!Current.Prefix.IsHash && !Current.Prefix.IsVerbatim && !Escape.empty() && Src.IsMatch(Pos, Escape) && Pos + 1 < Src.GetSize()
			) {
				Advance();
				if(
					!Current.Literal || !Current.Prefix.CanInterpolate ||
					Current.Literal->UnescapedInterpolationChars.find(Src[Pos]) == std::string_view::npos
				) Advance();
				continue;
			}
			if(Current.IsRegex && Byte == '[') {
				if(!Current.Depth || Reading.HasNestedRegexClasses) ++Current.Depth;
			} else if(Current.IsRegex && Byte == ']' && Current.Depth) --Current.Depth;
			else if(Current.NestedOpen && Byte == Current.NestedOpen) ++Current.Depth;
			if(Src.IsMatch(Pos, Current.Close)) {
				if(Current.Prefix.IsVerbatim && Src.IsMatch(Pos + Current.Close.size(), Current.Close)) {
					const size_t End = Pos + (Current.Close.size() << 1);
					while(Pos < End) Advance();
					continue;
				}
				if(Current.Depth && Current.NestedOpen) --Current.Depth;
				else if(!Current.Depth) {
					const size_t End = Pos + Current.Close.size();
					while(Pos < End) Advance();
					Frames.pop_back();
					continue;
				}
			}
		} else if(Src.IsMatch(Pos, Current.Close)) {
			const size_t End = Pos + Current.Close.size();
			while(Pos < End) Advance();
			Frames.pop_back();
			continue;
		} else if(Src.IsLineCommentStart(Reading, Notation, Pos)) {
			ScanLineComment(Notation);
			continue;
		} else if(Notation.IsAvailable(CommentNotation::Block) && Src.IsMatch(Pos, "/*")) {
			size_t Depth = 1;
			Advance();
			Advance();
			while(Depth && Pos < Src.GetSize()) {
				if(Src.IsMatch(Pos, "*/")) {
					--Depth;
					Advance();
					Advance();
				} else if(Notation.IsBlockCommentNested && Src.IsMatch(Pos, "/*")) {
					++Depth;
					Advance();
					Advance();
				} else Advance();
			}
			continue;
		} else if(
			const LiteralReading *Literal =
			LiteralStarts[Src[Pos]] ? Src.FindLiteralReading(Reading, Pos, Current.CanStartRegex, LiteralWork) : nullptr
		) {
			// 補間の開始行と本文の対応も同じ読取表で控える
			if(Literal->Delimiter == LiteralDelimiter::LineWord) {
				PendingLiteral Pending { Literal };
				ReadLiteralHeader(*Literal, Pending.Close);
				if(!Pending.Close.empty()) PendingLiterals.push_back(std::move(Pending));
				continue;
			}
			Frame Quoted { std::string(Literal->Close), Literal };
			const size_t Start = Pos, End = Start + Literal->Open.size();
			// 補間内でも引用の井桁・逐語接頭辞の閉じとエスケープを保つ
			const std::string PrefixText = Src.Copy(PrefixStart, Start - PrefixStart);
			const std::string_view Prefix = PrefixText;
			const size_t HashStart = std::min(Prefix.find('#'), Prefix.size());
			Quoted.Prefix = QuotePrefix::Read(Reading, *Literal, Prefix);
			while(Pos < End) Advance();
			if(Literal->Delimiter == LiteralDelimiter::Repeated) {
				while(Pos < Src.GetSize() && Src[Pos] == Byte) Advance();
				Quoted.Close.assign(Pos - Start, Byte);
			} else if(Literal->Delimiter == LiteralDelimiter::Paired) {
				Quoted.NestedOpen = Src[Pos];
				Quoted.Close.assign(1, GetPairedLiteralClose(*Literal, Quoted.NestedOpen));
				Advance();
			}
			if(Quoted.Prefix.IsHash) Quoted.Close.append(Prefix.substr(HashStart));
			Current.CanStartRegex = false;
			Frames.push_back(std::move(Quoted));
			continue;
		} else if(Byte > 0X7F || Byte == '_' || Byte == '$' || std::isalnum(Byte)) {
			const size_t Start = Pos;
			do Advance();
			while(Pos < Src.GetSize() && (Src[Pos] > 0X7F || Src[Pos] == '_' || Src[Pos] == '$' || std::isalnum(Src[Pos])));
			Current.CanStartRegex = !std::isdigit(Byte) && (
				Reading.CanRegexFollowName || !Current.IsDot && std::ranges::any_of(
					Reading.RegexWords,
					[this, Start](const std::string_view Word) -> bool {
						// 正規表現の値を導入する語かの返戻
						return Word.size() == Pos - Start && Src.IsMatch(Start, Word);
					}
				)
			);
			Current.IsDot = false;
			continue;
		} else if(Byte != 0X20 && (Byte < '\t' || Byte > '\r')) {
			const bool CanStartRegex = Current.CanStartRegex;
			Current.IsDot = Byte == '.';
			Current.CanStartRegex = Byte != ')' && Byte != ']' && Byte != '}' && Byte != '.';
			switch(Byte) {
			case '(':
			case '[':
			case '{':
				Current.CanStartRegex = false;
				Frames.push_back({ std::string(1, Byte == '(' ? ')' : Byte == '[' ? ']' : '}') });
				break;
			case '/':
				if(Reading.HasRegexLiterals && CanStartRegex) {
					Current.CanStartRegex = false;
					Frames.push_back({ "/", nullptr, 0, 0, true });
				}
				break;
			}
		}
		Advance();
	}
	// 終了
	return;
}

/**
 * 引用符の範囲をソースから読む関数
 * 入力上限以下の原文を参照し，同じ受理走査の位置と保留計数を更新する
 * 文法の開き・エスケープ・生文字列の記法を使い，閉じを確定出来ない範囲は内部の語と記号も数える
 * 計算量：引用本文長 B と固定記法総長 F に対し O(B(F + 1))，区切総長 D とネストの段数 H に対し追加領域 O(D + H)
 * @param Profile 入口で候補に為る文法
 * @param Literal 開きに対応する読取事実
 * @param Start 開きの引用符
 * @param HasInterpolation 補間を持ち得る範囲かの格納先
 * @param PrefixUnits トークンの接頭辞として領域へ含める計数
 * @return 引用範囲の終端（未閉じならソースの末尾，記法が違う場合は Start + 1）
 * @throws std::bad_alloc 生文字列の区切のメモリを確保出来ない場合
 * @throws std::system_error 記述子の原文を読み切れない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
size_t WorkingScan::QuotedEnd(
	const GrammarProfile &Profile,
	const LiteralReading &Literal,
	const size_t Start,
	bool &HasInterpolation,
	size_t &PrefixUnits
) {
	// 入力の読取規則
	const ReadingRules &Reading = Profile.GetReading();
	const unsigned char Quote = Src[Start];
	std::string PrefixText;
	const std::string_view Prefix = Src.GetPrefix(PrefixStart, Start - PrefixStart, PrefixText);
	const QuotePrefix PrefixFacts = QuotePrefix::Read(Reading, Literal, Prefix);
	const bool IsVerbatim = PrefixFacts.IsVerbatim, IsHash = PrefixFacts.IsHash;
	bool IsRaw = std::ranges::find(Reading.MeasuredRawStrPrefixes, Prefix) != Reading.MeasuredRawStrPrefixes.end();
	const size_t HashStart = std::min(Prefix.find('#'), Prefix.size());
	const bool HasPrefix = IsRaw || IsHash || IsVerbatim || PrefixFacts.HasRepeatedPrefix ||
	std::ranges::find(Reading.MeasuredStrPrefixes, Prefix) != Reading.MeasuredStrPrefixes.end();
	PrefixUnits = HasPrefix ? Units - this->PrefixUnits : 0;
	HasInterpolation = IsRaw && std::ranges::find(Reading.UncertainRawStrPrefixes, Prefix) != Reading.UncertainRawStrPrefixes.end();
	std::string Close(1, Quote);
	Advance();
	const bool IsMultiline = IsRaw || IsHash || IsVerbatim || Literal.CanCrossLine;
	const bool HasRepeatedDelimiter = Literal.Delimiter == LiteralDelimiter::Repeated;
	char NestedOpen = 0;
	size_t Depth = 0;
	if(IsRaw && Quote == '"') {
		// 生文字列の区切は最大１６文字で，空白と制御文字を含まない
		size_t Open = Pos;
		while(
			Open < Src.GetSize() && Open - Pos < 17 && Src[Open] != '(' && Src[Open] != ')' && Src[Open] != '\\' && Src[Open] > 0X20 &&
			Src[Open] < 0X7F
		) ++Open;
		// 生文字列の区切を確定出来ない場合の返戻
		if(Open == Src.GetSize() || Open - Pos > 16 || Src[Open] != '(') return Pos;
		Close = ")" + Src.Copy(Pos, Open - Pos) + "\"";
		while(Pos <= Open) Advance();
	} else if(IsHash) {
		Close = Literal.Close;
		Close.append(Prefix.substr(HashStart));
		while(Pos < Start + Literal.Open.size()) Advance();
	} else {
		Close = Literal.Close;
		while(Pos < Start + Literal.Open.size()) Advance();
		if(HasRepeatedDelimiter) {
			while(Pos < Src.GetSize() && Src[Pos] == Quote) Advance();
			Close.assign(Pos - Start, Quote);
		} else if(Literal.Delimiter == LiteralDelimiter::Paired) {
			// 専用区切の１文字と対応する閉じ
			NestedOpen = Src[Pos];
			Close.assign(1, GetPairedLiteralClose(Literal, NestedOpen));
			Advance();
		}
		IsRaw = Literal.Escape.empty();
	}
	// 接頭辞で補間を指定した引用は，式が現れなくても内部計数を保つ
	HasInterpolation = HasInterpolation || !Literal.InterpolationPrefixes.empty() && PrefixFacts.CanInterpolate;
	if(Reading.NameQuoteHeads.find(static_cast<char>(Quote)) != std::string_view::npos && Pos < Src.GetSize() && Src[Pos] != '\\') {
		size_t End = Pos + 1;
		while(End < Src.GetSize() && (Src[End] & 0XC0) == 0X80) ++End;
		if(End == Src.GetSize() || Src[End] != '\'') {
			HasInterpolation = true;
			// 寿命の名前を引用範囲として読まない事の返戻
			return Pos;
		}
	}
	while(Pos < Src.GetSize()) {
		if(!IsMultiline && Src[Pos] == '\n') {
			HasInterpolation = true;
			// 未閉じの行内のテキストを数える範囲の返戻
			return Pos;
		}
		if(HasRepeatedDelimiter && Src[Pos] == Quote) {
			size_t End = Pos + 1;
			while(End < Src.GetSize() && End - Pos < Close.size() && Src[End] == Quote) ++End;
			if(End - Pos < Close.size()) {
				while(Pos < End) Advance();
				continue;
			}
			// 複数引用符の閉じた範囲の返戻
			return Pos + Close.size();
		}
		if(NestedOpen && Literal.CanNest && Src[Pos] == NestedOpen) ++Depth;
		if(!HasRepeatedDelimiter && Src.IsMatch(Pos, Close)) {
			if(Depth) {
				--Depth;
				Advance();
				continue;
			}
			if(IsVerbatim && Pos + 1 < Src.GetSize() && Src[Pos + 1] == Quote) {
				Advance();
				Advance();
				continue;
			}
			// 引用範囲の終端の返戻
			return Pos + Close.size();
		}
		size_t BodyLen = 0;
		const std::string_view Head = Src.MatchInterpolationHead(Literal, PrefixFacts, Pos, BodyLen);
		if(BodyLen || !Head.empty()) {
			const size_t End = Pos + (BodyLen ? BodyLen : Head.size());
			while(Pos < End) Advance();
			HasInterpolation = HasInterpolation || !Head.empty();
			if(!Head.empty() && (Head.front() == '{' || Head.back() == '(' || Head.back() == '{')) {
				const std::string ExpressionClose(Literal.HasRepeatedInterpolationHead ? Head.size() : 1, Head.back() == '(' ? ')' : '}');
				ScanInterpolation(Profile, ExpressionClose);
			}
			continue;
		}
		if(
			!IsRaw && !IsHash && !IsVerbatim && Src.IsMatch(Pos, Literal.Escape) && Pos + 1 < Src.GetSize() &&
			(!PrefixFacts.CanInterpolate || Literal.UnescapedInterpolationChars.find(Src[Pos + 1]) == std::string_view::npos)
		) Advance();
		Advance();
	}
	HasInterpolation = true;
	// 未閉じの引用本文もトークン単位で数える範囲の返戻
	return Src.GetSize();
}

/**
 * 正規表現と除算の区別が未確定な範囲の受理走査関数
 * 同じ受理走査の位置と内部の保留計数を更新する
 * 開閉・エスケープ・文字集合の事実で候補範囲を区切り，区別が未確定な内部は領域として畳まない
 * 計算量：候補本文長 B と固定の記法数 F に対し O(B(F + 1))，区切総長 D と補間のネストの段数 H に対し追加領域 O(D + H)
 * @param Profile 入口の文法
 * @return 井桁で区切った補間無の閉じたリテラルなら true
 * @throws std::bad_alloc 区切又は補間の作業列を確保出来ない場合
 * @throws std::length_error 区切又は補間の作業列の要素数上限を超える場合
 * @throws std::system_error 原文を読み切れない場合
 */
bool WorkingScan::CanCollapseRegexCandidate(const GrammarProfile &Profile) {
	// 可変の井桁の区切を含む開きの後の準備
	const ReadingRules &Reading = Profile.GetReading();
	const std::string Prefix = Reading.HasHashRegexDelimiter ? Src.Copy(PrefixStart, Pos - PrefixStart) : std::string();
	const bool IsHash = !Prefix.empty() && std::ranges::all_of(
		Prefix,
		[](const char Byte) -> bool {
			// 井桁だけの区切かの返戻
			return Byte == '#';
		}
	);
	const std::string Close = IsHash ? "/" + Prefix : "/";
	const std::string_view EscapedInterpolationHead = GetEscapedInterpolationHead(Reading);
	std::string InterpolationOpen(EscapedInterpolationHead);
	if(IsHash && !InterpolationOpen.empty()) InterpolationOpen.insert(1, Prefix);
	// 本文・文字集合・補間の式の区別
	size_t ClassDepth = 0;
	bool HasInterpolation = false;
	Advance();
	while(Pos < Src.GetSize()) {
		const unsigned char Byte = Src[Pos];
		// 行を跨げない未閉じの候補の返戻
		if(!Reading.CanRegexCrossLine && (Byte == '\r' || Byte == '\n')) return false;
		const bool IsEscapedInterpolation = !InterpolationOpen.empty() && Src.IsMatch(Pos, InterpolationOpen);
		if(
			IsEscapedInterpolation ||
			Byte == '#' && Pos + 1 < Src.GetSize() && Profile.GetRules().HashInterpolationHeads.find(Src[Pos + 1]) != std::string_view::npos
		) {
			HasInterpolation = true;
			const size_t End = Pos + (IsEscapedInterpolation ? InterpolationOpen.size() : 2);
			const char Open = static_cast<char>(Src[End - 1]);
			while(Pos < End) Advance();
			if(Open == '(' || Open == '{') ScanInterpolation(Profile, Open == '(' ? ")" : "}");
			continue;
		}
		if(Byte == '\\' && Pos + 1 < Src.GetSize()) {
			Advance();
			if(Reading.CanRegexCrossLine || Src[Pos] != '\r' && Src[Pos] != '\n') Advance();
			continue;
		}
		if(!ClassDepth && Src.IsMatch(Pos, Close)) {
			const size_t End = Pos + Close.size();
			while(Pos < End) Advance();
			// 補間無で井桁の区切に依り区別を確定した領域かの返戻
			return IsHash && !HasInterpolation;
		}
		if(Byte == '[' && (!ClassDepth || Reading.HasNestedRegexClasses)) ++ClassDepth;
		else if(Byte == ']' && ClassDepth) --ClassDepth;
		Advance();
	}
	// 未閉じの候補の返戻
	return false;
}

/**
 * 埋込の本文を受理走査する関数
 * 同じ受理走査の位置と保留計数を更新する
 * 別の文法の引用・コメントを外側の確定領域にせず，所有要素の終端で通常の走査へ戻る
 * 計算量：本文長 B と固定の記法長 F に対し O(B(F + 1))
 * @param Reading 外側の読取記法
 * @param Element 所有要素の名前
 * @throws std::system_error 原文を読み切れない場合
 */
void WorkingScan::ScanEmbeddedCandidate(const ReadingRules &Reading, const std::string_view Element) {
	// 埋込の終端名の照合関数
	const auto IsName = [this, &Reading, Element](const size_t Pos) -> bool {
		const size_t End = Pos + Element.size();
		// 終端の区切を持つ要素名かの返戻
		return Src.IsMatch(Pos, Element, true) && End < Src.GetSize() &&
		Reading.RawTextNameTerminators.find(Src[End]) != std::string_view::npos;
	};
	bool IsEscaped = false, IsDoubleEscaped = false;
	while(Pos < Src.GetSize()) {
		if(Element == Reading.RawTextEscapeElement) {
			if(Src.IsMatch(Pos, Reading.RawTextEscapeOpen)) IsEscaped = true;
			else if(Src.IsMatch(Pos, "-->")) {
				IsEscaped = false;
				IsDoubleEscaped = false;
			} else if(IsEscaped && Src[Pos] == '<' && IsName(Pos + 1)) IsDoubleEscaped = true;
		}
		if(Src.IsMatch(Pos, Reading.RawTextEndOpen) && IsName(Pos + Reading.RawTextEndOpen.size())) {
			// 二重にエスケープした終端は本文の区切であり，所有要素を閉じない
			if(IsDoubleEscaped) IsDoubleEscaped = false;
			// 所有要素の閉タグの前での終了
			else return;
		}
		Advance();
	}
	// 終了
	return;
}

/**
 * 見積の乗算と加算の飽和関数
 * 計算量：O(1)
 * @param Total 確定済の見積
 * @param Count 加算する単位の数
 * @param Weight 単位毎のバイト数
 * @return オーバーフロー前に上限の次の値へ飽和した見積
 */
static size_t AddWorkingBytes(const size_t Total, const size_t Count, const size_t Weight) {
	// 見積の飽和上限の算出
	const size_t Ceiling = MaxWorkingBytes + 1;
	// 乗算・加算の前に飽和する見積の返戻
	return Count > (Ceiling - Total) / Weight ? Ceiling : Count * Weight + Total;
}

/**
 * 文法の記法・語・記号・本文からの作業域の見積関数
 * 入力上限以下の原文を同じ前進走査で計数する
 * 語・記号毎１２８０バイト，コメント・補間無の引用範囲毎２０４８バイト，本文の１６倍，LF 毎４８バイト，基礎領域 16 MiB を見込む
 * 補間・トークン化前の Unicode 展開・正規表現の読違が曖昧な範囲は，内部の語と記号も数える
 * 此の見積はメモリの受理条件であり，整形仕様 9.2 の W 又は作業単位へ使わない
 * 計算量：原文長 B と固定の記法数 F に対し O(B(F + 1))，追加領域は区切総長 D と括弧の深さ H に対し O(D + H)
 * @param Profile 入口で候補に為る文法
 * @param Work 引用の記法の準備・照合回数の格納先
 * @return 基礎領域とトークン・本文の見積作業域（上限超過は上限の次の値）
 * @throws std::bad_alloc 区切又は括弧の走査の領域を確保出来ない場合
 * @throws std::system_error 記述子の原文を読み切れない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
size_t WorkingText::EstimateWorkingBytes(const GrammarProfile &Profile, size_t *const Work) {
	// 文法のコメントと引用の記法の取得
	const Core::NotationRules &Notation = Profile.GetRules().Notation;
	const bool HasBlock = Notation.IsAvailable(CommentNotation::Block);
	const bool HasEmbeddedDoc = Notation.IsAvailable(CommentNotation::RubyEmbeddedDocument);
	const ReadingRules &Reading = Profile.GetReading();
	const bool HasRegex = Reading.HasRegexLiterals;
	std::array<bool, 256> LiteralStarts {};
	bool HasQuotedDigitSeparator = false;
	for(const LiteralReading &Literal : Reading.Literals) {
		LiteralStarts[static_cast<unsigned char>(Literal.Open.front())] = true;
		HasQuotedDigitSeparator =
		HasQuotedDigitSeparator || Literal.Open.size() == 1 && Literal.Open.front() == Profile.GetRules().DigitSeparator;
		if(Work) ++*Work;
	}
	bool CanStartRegex = true, IsStmtHead = false, IsDot = false;
	std::vector<uint8_t> Parens;
	// マークアップの本文・タグ・式の区分は，同じ前進走査のスタックで保つ
	struct MarkupFrame {
		size_t Depth = 0; // 未完の要素の深さ
		size_t Braces = 0; // タグ又は本文の中の式の波括弧の深さ
		bool IsTag = true; // 属性を含むタグを読んで居るか
		bool IsClosing = false; // 終了タグか
		bool IsVoid = false; // 本文を持たない要素か
		bool IsAttributeVal = false; // 等号の後の属性値の開始を待つか
		std::string RawElement {}; // 逐語本文又は埋込本文を持つ要素名
	};
	std::vector<MarkupFrame> Markup;
	if(Reading.HasEmbeddedScripts) Markup.push_back({ .IsTag = false });
	std::string EmbeddedElement;
	bool IsOuterText = Reading.HasOuterText, IsData = false, IsDirectiveHead = false, IsHeaderName = false;
	WorkingScan Scan {
		.HasUnicodePrelexing = Notation.ShouldDecodeUnicodeEscapes,
		.LiteralWork = Work,
		.LiteralStarts = LiteralStarts,
		.Src = *this
	};
	std::vector<PendingLiteral> PendingLiterals;
	bool HasPendingBodies = false;
	size_t CollapsedUnits = 0, Regions = 0;
	while(Scan.Pos < GetSize()) {
		// Java の展開後の境界も，同じ原文のバイトを数える前進走査で確定する
		if(Scan.HasUnicodePrelexing) {
			Scan.Advance();
			continue;
		}
		if(HasPendingBodies) {
			for(size_t Index = 0; Index < PendingLiterals.size(); ++Index) {
				const PendingLiteral &Pending = PendingLiterals[Index];
				const size_t Before = Scan.Units;
				bool HasInterpolation = false;
				if(Scan.HasClosedLiteralBody(Pending, HasInterpolation) && !HasInterpolation) {
					CollapsedUnits += Scan.Units - Before + Pending.HeaderUnits;
					++Regions;
				}
				// 最後の終端の後は通常走査に渡し，次の本文は終端の語の行の後から始める
				if(Index + 1 == PendingLiterals.size()) break;
				while(Scan.Pos < GetSize() && (*this)[Scan.Pos] != '\n') Scan.Advance();
				if(Scan.Pos < GetSize()) Scan.Advance();
			}
			PendingLiterals.clear();
			HasPendingBodies = false;
			continue;
		}
		if(!EmbeddedElement.empty()) {
			Scan.ScanEmbeddedCandidate(Reading, EmbeddedElement);
			EmbeddedElement.clear();
			continue;
		}
		const size_t Start = Scan.Pos, UnitsBefore = Scan.Units;
		const unsigned char Byte = (*this)[Start];
		// 逐語データとタグの外の本文は，引用・コメントの開始記号だけでは領域へ畳まない
		if(IsData) {
			Scan.Advance();
			continue;
		}
		if(IsOuterText) {
			for(const std::string_view Open : Profile.GetRules().CodeOpeningSuffixes) {
				const size_t After = Start + Open.size();
				if(
					IsMatch(Start, Open, true) &&
					(Open.back() == '=' || After == GetSize() || (*this)[After] == ' ' || (*this)[After] >= '\t' && (*this)[After] <= '\r')
				) {
					IsOuterText = false;
					while(Scan.Pos < After) Scan.Advance();
					break;
				}
			}
			if(IsOuterText) Scan.Advance();
			continue;
		}
		if(Reading.HasOuterText && !Notation.LineCommentTerminator.empty() && IsMatch(Start, Notation.LineCommentTerminator)) {
			IsOuterText = true;
			Scan.Advance();
			continue;
		}
		// データ境界の候補の後は，解析前にはコードとの区別が未確定なので内部計数を保つ
		if(
			!Reading.TrailingDataMarker.empty() && (!Start || (*this)[Start - 1] == '\n') && IsMatch(Start, Reading.TrailingDataMarker) && (
				Start + Reading.TrailingDataMarker.size() == GetSize() || (*this)[Start + Reading.TrailingDataMarker.size()] == '\n' ||
				(*this)[Start + Reading.TrailingDataMarker.size()] == '\r' && Start + Reading.TrailingDataMarker.size() + 1 < GetSize() &&
				(*this)[Start + Reading.TrailingDataMarker.size() + 1] == '\n'
			) || Reading.HasHaltedData && !Scan.IsWord && IsMatch(Start, Reading.HaltCallName, true) &&
			(Start + Reading.HaltCallName.size() == GetSize() || !IsWordByte((*this)[Start + Reading.HaltCallName.size()]))
		) IsData = true;
		if(IsData) {
			Scan.Advance();
			continue;
		}
		if(IsDirectiveHead || IsHeaderName) if(const size_t Stop = LineSpliceEnd(Notation, Start)) {
			while(Scan.Pos < Stop) Scan.Advance();
			continue;
		}
		if(Byte == '\n' || Byte == '\r') IsDirectiveHead = IsHeaderName = false;
		if(!Reading.HeaderNameDirectives.empty() && Byte == '#') IsDirectiveHead = true;
		else if(IsDirectiveHead && IsWordByte(Byte)) {
			IsDirectiveHead = false;
			for(const std::string_view Directive : Reading.HeaderNameDirectives) {
				const size_t After = MatchSplicedBoundary(Notation, Start, Directive);
				if(After && (After == GetSize() || !IsWordByte((*this)[After]))) IsHeaderName = true;
			}
		} else if(IsHeaderName && (Byte == '<' || Byte == '\"')) {
			const unsigned char Close = Byte == '<' ? '>' : '\"';
			Scan.Advance();
			while(Scan.Pos < GetSize() && (*this)[Scan.Pos] != '\n' && (*this)[Scan.Pos] != Close) Scan.Advance();
			if(Scan.Pos < GetSize() && (*this)[Scan.Pos] == Close) Scan.Advance();
			IsHeaderName = false;
			continue;
		}
		// 引用符の無い URL の本文はコメントを含めて通常の語・記号へ数える
		if(!Reading.UrlFunctionHead.empty() && !Scan.IsWord && IsMatch(Start, Reading.UrlFunctionHead, true)) {
			const size_t After = Start + Reading.UrlFunctionHead.size();
			while(Scan.Pos < After) Scan.Advance();
			while(Scan.Pos < GetSize() && ((*this)[Scan.Pos] == ' ' || (*this)[Scan.Pos] >= '\t' && (*this)[Scan.Pos] <= '\r')) {
				Scan.Advance();
			}
			if(Scan.Pos < GetSize() && (*this)[Scan.Pos] != '\"' && (*this)[Scan.Pos] != '\'') {
				while(Scan.Pos < GetSize() && (*this)[Scan.Pos] != ')') {
					const bool IsEscape = (*this)[Scan.Pos] == '\\';
					Scan.Advance();
					if(IsEscape && Scan.Pos < GetSize()) Scan.Advance();
				}
			}
			continue;
		}
		if(
			Byte == '<' && (Reading.HasEmbeddedScripts || Reading.HasJsxText) &&
			(Markup.empty() ? CanStartRegex : !Markup.back().Braces || CanStartRegex)
		) {
			const size_t NameStart = Start + 1 + (Start + 1 < GetSize() && (*this)[Start + 1] == '/');
			if(
				NameStart < GetSize() &&
				(IsWordByte((*this)[NameStart]) && ((*this)[NameStart] < '0' || (*this)[NameStart] > '9') || (*this)[NameStart] == '>')
			) {
				if(Markup.empty() || Markup.back().Braces) Markup.emplace_back();
				MarkupFrame &Frame = Markup.back();
				Frame.IsTag = true;
				Frame.IsClosing = NameStart != Start + 1;
				Frame.IsVoid = false;
				Frame.IsAttributeVal = false;
				Frame.RawElement.clear();
				size_t NameEnd = NameStart;
				while(NameEnd < GetSize() && (IsWordByte((*this)[NameEnd]) || (*this)[NameEnd] == '-' || (*this)[NameEnd] == ':')) ++NameEnd;
				if(Reading.HasEmbeddedScripts && NameEnd - NameStart < 32 && !Frame.IsClosing) {
					std::string Name = Copy(NameStart, NameEnd - NameStart);
					for(char &Char : Name) Char = static_cast<char>(std::tolower(static_cast<unsigned char>(Char)));
					const uint8_t Traits = GrammarProfile::GetMarkupTraits(GrammarProfile::MarkupContext::Html, Name);
					Frame.IsVoid = Traits & GrammarProfile::Void;
					if(Traits & GrammarProfile::Raw || Name == Reading.RawTextEscapeElement || Name == Reading.RawTextPlainElement) {
						Frame.RawElement = std::move(Name);
					}
				}
				while(Scan.Pos < NameEnd) Scan.Advance();
				continue;
			}
		}
		if(!Markup.empty() && !Markup.back().Braces) {
			MarkupFrame &Frame = Markup.back();
			if(Reading.HasJsxText && Byte == '{') {
				Frame.Braces = 1;
				CanStartRegex = true;
				Frame.IsAttributeVal = false;
			} else if(Frame.IsTag && Byte == '>') {
				const bool IsSelfClosing = Start && (*this)[Start - 1] == '/';
				if(Frame.IsClosing) {
					if(Frame.Depth) --Frame.Depth;
				} else if(!IsSelfClosing && !Frame.IsVoid) ++Frame.Depth;
				Frame.IsTag = false;
				if(!Frame.IsClosing && !IsSelfClosing) EmbeddedElement = std::move(Frame.RawElement);
				if(!Reading.HasEmbeddedScripts && !Frame.Depth) {
					Markup.pop_back();
					CanStartRegex = false;
				}
			} else if(
				Frame.IsTag ?
				!Frame.IsAttributeVal || Byte != '\"' && Byte != '\'' :
				!(Notation.HasMarkupComments && IsMatch(Start, Notation.MarkupCommentOpen))
			) {
				if(Frame.IsTag && Byte == '=') Frame.IsAttributeVal = true;
				else if(Frame.IsTag && Byte != ' ' && (Byte < '\t' || Byte > '\r')) Frame.IsAttributeVal = false;
				Scan.Advance();
				continue;
			}
			if(Byte == '{' || Byte == '>') {
				Scan.Advance();
				continue;
			}
		}
		size_t End = Start + 1, PrefixUnits = 0;
		bool IsCollapsed = false;
		if(const size_t OpenEnd = HasBlock && Byte == '/' ? MatchSplicedBoundary(Notation, Start, "/*") : 0; OpenEnd) {
			size_t Depth = 1;
			while(Scan.Pos < OpenEnd) Scan.Advance();
			while(Depth && Scan.Pos < GetSize()) {
				if(const size_t CloseEnd = MatchSplicedBoundary(Notation, Scan.Pos, "*/"); CloseEnd) {
					--Depth;
					while(Scan.Pos < CloseEnd) Scan.Advance();
				} else if(
					const size_t NestedEnd = Notation.IsBlockCommentNested ? MatchSplicedBoundary(Notation, Scan.Pos, "/*") : 0;
					NestedEnd
				) {
					++Depth;
					while(Scan.Pos < NestedEnd) Scan.Advance();
				} else Scan.Advance();
			}
			End = Scan.Pos;
			IsCollapsed = !Depth;
		} else if(Notation.HasMarkupComments && IsMatch(Start, Notation.MarkupCommentOpen)) {
			for(; Scan.Pos < GetSize(); Scan.Advance()) if(IsMatch(Scan.Pos, Notation.MarkupCommentClose)) {
				End = Scan.Pos + Notation.MarkupCommentClose.size();
				IsCollapsed = true;
				break;
			}
			if(!IsCollapsed) End = Scan.Pos;
		} else if(HasEmbeddedDoc && (!Start || (*this)[Start - 1] == '\n') && IsLineHeadWord(Start, "=begin")) {
			for(; Scan.Pos < GetSize(); Scan.Advance()) if(Scan.Pos && (*this)[Scan.Pos - 1] == '\n' && IsLineHeadWord(Scan.Pos, "=end")) {
				IsCollapsed = true;
				while(Scan.Pos < GetSize() && (*this)[Scan.Pos] != '\n') Scan.Advance();
				break;
			}
			End = Scan.Pos;
		} else if((Byte == '/' || Byte == '#') && IsLineCommentStart(Reading, Notation, Start)) {
			Scan.ScanLineComment(Notation);
			End = Scan.Pos;
			IsCollapsed = true;
		} else if(Byte >= '0' && Byte <= '9' && !Scan.IsWord && HasQuotedDigitSeparator) {
			// 数値のプリプロセッサトークンを１度辿り，１６進の桁・指数・接尾辞の中の桁区切を引用の開始へ渡さない
			unsigned char Prev = Byte;
			Scan.Advance();
			while(Scan.Pos < GetSize()) {
				if(const size_t Stop = LineSpliceEnd(Notation, Scan.Pos)) {
					while(Scan.Pos < Stop) Scan.Advance();
					continue;
				}
				const unsigned char Part = (*this)[Scan.Pos];
				// 不完全な数値内の区切も引用範囲へ畳まず，通常の語・記号の計数を保つ
				if(
					!(
						Part > 0X7F || Part == '_' || std::isalnum(Part) || Part == '.' || Part == Profile.GetRules().DigitSeparator ||
						(Part == '+' || Part == '-') && (Prev == 'e' || Prev == 'E' || Prev == 'p' || Prev == 'P')
					)
				) break;
				Prev = Part;
				Scan.Advance();
			}
			End = Scan.Pos;
			CanStartRegex = false;
			IsStmtHead = false;
		} else if(
			const LiteralReading *Literal = Scan.LiteralStarts[Byte] ? FindLiteralReading(Reading, Start, CanStartRegex, Work) : nullptr
		) {
			bool HasInterpolation = false;
			if(Literal->Delimiter == LiteralDelimiter::LineWord) {
				// 本文の開始迄，開始行の残りは通常のコードとして計数する
				PendingLiteral Pending { Literal };
				End = Scan.ReadLiteralHeader(*Literal, Pending.Close);
				if(!Pending.Close.empty()) {
					Pending.HeaderUnits = Scan.Units - UnitsBefore;
					PendingLiterals.push_back(std::move(Pending));
				}
				HasInterpolation = true;
			} else End = Scan.QuotedEnd(Profile, *Literal, Start, HasInterpolation, PrefixUnits);
			IsCollapsed = End > Start + 1 && !HasInterpolation;
			if(!Markup.empty() && Markup.back().IsTag && !Markup.back().Braces) Markup.back().IsAttributeVal = false;
			CanStartRegex = false;
			IsStmtHead = false;
		} else if(HasRegex && CanStartRegex && Byte == '/') {
			// 未確定なのは此の正規表現の候補だけで，後続の領域の確定を止めない
			PrefixUnits = Scan.Units - Scan.PrefixUnits;
			IsCollapsed = Scan.CanCollapseRegexCandidate(Profile);
			End = Scan.Pos;
			CanStartRegex = false;
			IsStmtHead = false;
		} else if(HasRegex && (Byte > 0X7F || Byte == '_' || Byte == '$' || std::isalnum(Byte))) {
			// 語の最長列を１度読み，数値又は名前の後の斜線と，語が導入する値を区別する
			do Scan.Advance();
			while(
				Scan.Pos < GetSize() &&
				((*this)[Scan.Pos] > 0X7F || (*this)[Scan.Pos] == '_' || (*this)[Scan.Pos] == '$' || std::isalnum((*this)[Scan.Pos]))
			);
			End = Scan.Pos;
			const auto IsWord = [this, Start, End](const std::string_view Word) -> bool {
				// 同じ長さの固定の語かの返戻
				return Word.size() == End - Start && IsMatch(Start, Word);
			};
			IsStmtHead = !IsDot &&
			(IsStmtHead && std::ranges::any_of(Reading.RegexBridgeWords, IsWord) || std::ranges::any_of(Reading.RegexStmtHeads, IsWord));
			CanStartRegex = !(Byte >= '0' && Byte <= '9') &&
			(Reading.CanRegexFollowName || Byte > 0X7F || IsStmtHead || !IsDot && std::ranges::any_of(Reading.RegexWords, IsWord));
			IsDot = false;
		} else {
			if(!Markup.empty() && Markup.back().Braces) {
				if(Byte == '{') ++Markup.back().Braces;
				else if(Byte == '}') --Markup.back().Braces;
			}
			if(HasRegex && Reading.CanRegexStartLine && (Byte == '\r' || Byte == '\n')) CanStartRegex = true;
			else if(HasRegex && Byte != 0X20 && (Byte < '\t' || Byte > '\r')) {
				IsDot = Byte == '.';
				if(Byte == '(') Parens.push_back(IsStmtHead);
				if(Byte == ')') {
					CanStartRegex = Parens.empty() || Parens.back();
					if(!Parens.empty()) Parens.pop_back();
				// 前置・後置の双方で，増減の前の値の開始可否を保つ
				} else if((Byte == '+' || Byte == '-') && Start + 1 < GetSize() && (*this)[Start + 1] == Byte) End = Start + 2;
				else CanStartRegex = Byte != ']' && Byte != '.';
				IsStmtHead = false;
			}
		}
		while(Scan.Pos < End) Scan.Advance();
		HasPendingBodies = !PendingLiterals.empty() && Scan.Pos && (*this)[Scan.Pos - 1] == '\n';
		if(IsCollapsed) {
			CollapsedUnits += Scan.Units + PrefixUnits - UnitsBefore;
			++Regions;
		}
	}
	if(Scan.HasUnicodePrelexing) {
		// 未完のエスケープは内部計数に保ち，EOF で終わる行コメントと空の文字列だけを確定する
		if(!Scan.IsUnicodePrefix && !Scan.UnicodeDigits) {
			if(Scan.Region == WorkingScan::UnicodeRegion::LineComment) Scan.CloseUnicodeRegion(Scan.Units);
			else if(Scan.Region == WorkingScan::UnicodeRegion::Quoted && Scan.UnicodeQuoteRun == 2) {
				Scan.CloseUnicodeRegion(Scan.EmptyQuoteUnits);
			}
		}
		CollapsedUnits = Scan.CollapsedUnicodeUnits;
		Regions = Scan.UnicodeRegions;
	}
	// メモリの受理で使うバイト単位の見積係数
	constexpr size_t TextWeight = 0X10, TokenWeight = 0X500, RegionWeight = 0X800, LineWeight = 0X30;
	size_t WorkingBytes = AddWorkingBytes(MaxInputBytes, GetSize(), TextWeight);
	WorkingBytes = AddWorkingBytes(WorkingBytes, Scan.Units - CollapsedUnits, TokenWeight);
	WorkingBytes = AddWorkingBytes(WorkingBytes, Regions, RegionWeight);
	// 受理範囲の見積の返戻
	return AddWorkingBytes(WorkingBytes, Scan.Lines, LineWeight);
}

/**
 * コンストラクタ（既存のソースを参照する）
 * 計算量：O(1)
 * @param Src ソース
 */
WorkingText::WorkingText(const std::string_view Src) : Size(Src.size()), Read(nullptr), Text(Src) {}

/**
 * コンストラクタ（原文の複写を作らず固定長の領域で読み出す）
 * 計算量：O(1)
 * @param Bytes 原文長
 * @param Reader 記述子上の読出口
 */
WorkingText::WorkingText(const size_t Bytes, const std::function<size_t(size_t, std::span<char>)> &Reader) : Size(Bytes),
Read(&Reader) {}

/**
 * ソースを参照する作業域の受理見積関数
 * 計算量：原文長 B と固定の記法数 F に対し O(B(F + 1))，追加領域は可変の区切総長 D と括弧の深さ H に対し O(D + H)
 * @param Profile 入口で候補に為る文法
 * @param Src ソース
 * @param Work 引用の記法の準備・照合回数の格納先（無ければ観測しない）
 * @return 上限の次の値迄に飽和した見積
 * @throws std::bad_alloc 区切又は括弧の走査の領域を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
size_t EstimateWorkingBytes(const GrammarProfile &Profile, const std::string_view Src, size_t *const Work) {
	// 作業状態の準備
	WorkingText Text(Src);
	// 同じ受理走査の見積の返戻
	return Text.EstimateWorkingBytes(Profile, Work);
}

/**
 * 固定長の入出力領域を使う作業域の受理見積関数
 * 計算量：原文長 B と固定の記法数 F に対し O(B(F + 1))，追加領域は可変の区切総長 D と括弧の深さ H に対し O(D + H)
 * @param Profile 入口で候補に為る文法
 * @param Size 既知の原文長
 * @param Read 記述子から読み出す関数
 * @param Work 引用の記法の準備・照合回数の格納先（無ければ観測しない）
 * @return 上限の次の値迄に飽和した見積
 * @throws std::bad_alloc 区切又は括弧の走査の領域を確保出来ない場合
 * @throws std::system_error 原文を読み切れない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
size_t EstimateWorkingBytes(
	const GrammarProfile &Profile,
	const size_t Size,
	const std::function<size_t(size_t, std::span<char>)> &Read,
	size_t *const Work
) {
	WorkingText Text(Size, Read);
	// 同じ受理走査の見積の返戻
	return Text.EstimateWorkingBytes(Profile, Work);
}

/**
 * 入力の大きさと見積作業域の受理判定関数（整形仕様 9.1）
 * 並列予約は受理走査の計数を渡し，独立入力は大きさの受理後に同じ走査を行う
 * 計算量：見積の取得費用を E として O(E)
 * @param Size 元の入口での入力のバイト数
 * @param Estimate 同じ入口条件の作業域見積を返す操作
 * @return 受理なら None，サイズ超過なら見送理由，作業域超過なら失敗理由
 * @throws std::bad_alloc 受理走査の領域を確保出来ない場合
 * @throws std::length_error 受理走査の領域の上限を超える場合
 * @throws std::system_error 受理走査で入力を読み切れない場合
 */
FailureReason AcceptInput(const size_t Size, const std::function<size_t()> &Estimate) {
	// 作業域を走査する前のサイズ超過の見送理由の返戻
	if(Size > MaxInputBytes) return FailureReason::InputSizeExceeded;
	// 見積作業域の超過の失敗理由の返戻
	if(Estimate() > MaxWorkingBytes) return FailureReason::WorkingMemoryExceeded;
	// 受理した入力の理由無の返戻
	return FailureReason::None;
}
