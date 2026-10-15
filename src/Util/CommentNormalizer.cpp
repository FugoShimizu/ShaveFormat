#include "CommentNormalizer.hpp"
#include "CharClass.hpp"
#include "ParseAdapter.hpp"
#include "TextEdit.hpp"
#include "../Core/CommentFormat.hpp"
#include "../Core/CommentRepository.hpp"
#include "../Core/SyntaxRole.hpp"
#include "../Profile/UnicodePhonetic.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/**
 * 本文の空白のバイト幅の取得関数
 * 文末の後と論理行の端で，スペース・タブと Unicode の Zs を扱う（本文中の制御文字は転写する）
 * 計算量：O(1)
 * @param Text コメント本文
 * @param Pos 読取位置（末尾も可）
 * @return 空白の UTF-8 のバイト数，空白でなければ０
 */
size_t CommentNormalizer::GetSpaceWidth(const std::string_view Text, const size_t Pos) {
	// 本文末尾の返戻
	if(Pos >= Text.size()) return 0;
	// ASCII の空白幅の返戻
	if(const unsigned char Byte = static_cast<unsigned char>(Text[Pos]); Byte < 0X80) return Byte == ' ' || Byte == '\t' ? 1 : 0;
	size_t End = Pos;
	char32_t Point = 0;
	// 共通の Unicode 空白判定に依る幅の返戻
	return TextEdit::IsUtf8Decoded(Text, End, Point) && IsUnicodeSpace(Point) ? End - Pos : 0;
}

/**
 * 直前の本文の空白のバイト幅の取得関数
 * UTF-8 の先頭へ戻り，同じ空白判定を用いる
 * 計算量：O(1)
 * @param Text コメント本文
 * @param End 調べる境界（本文長以下）
 * @return 境界の直前の空白のバイト数，空白でなければ０
 */
size_t CommentNormalizer::GetSpaceWidthBefore(const std::string_view Text, const size_t End) {
	// 本文頭の返戻
	if(!End) return 0;
	size_t Start = End - 1;
	while(Start && End - Start < 4 && (static_cast<unsigned char>(Text[Start]) & 0XC0) == 0X80) --Start;
	const size_t Width = GetSpaceWidth(Text, Start);
	// 境界で終わる空白の幅の返戻
	return Start + Width == End ? Width : 0;
}

/**
 * 文末の記号の幅の取得関数
 * 文末の記号の集合は此処だけで定める
 * @param Text 走査対象
 * @param Pos 判定位置
 * @return 全角の句点・疑問符・感嘆符なら３，半角の `.` `?` `!` なら１，其の他は０
 */
size_t CommentNormalizer::GetSentenceMarkWidth(const std::string_view Text, const size_t Pos) {
	// 半角の文末の記号の幅の返戻
	if(Text[Pos] == '.' || Text[Pos] == '?' || Text[Pos] == '!') return 1;
	// 全角形を持たない ASCII の字の返戻
	if(static_cast<unsigned char>(Text[Pos]) < 0X80) return 0;
	const std::string_view Cell = Text.substr(Pos, 3); // 全角の文末記号の UTF-8 のトークン長
	// 全角の文末の記号の幅の返戻
	return Cell == "。" || Cell == "．" || Cell == "？" || Cell == "！" ? 3 : 0;
}

/**
 * 表音文字と其の結合文字の判定関数
 * 仮名・漢字・全角形と，文字でない記号を含めない
 * 計算量：固定した区間数 R に対し O(log R)，追加メモリ O(1)
 * @param Point 判定するコードポイント
 * @return 該当する文字なら true
 */
bool CommentNormalizer::IsPhoneticLetter(const char32_t Point) {
	// ASCII の英字の返戻
	if(Point < 0X80) return static_cast<bool>(std::isalpha(static_cast<unsigned char>(Point)));
	// Unicode の文字種で確認した表音文字の区間に含まれるかの返戻
	return static_cast<bool>(FindUnicodeRange(Core::UnicodePhonetic::Ranges, Point));
}

/**
 * 字形を持たない書式制御の判定関数
 * 計算量：O(1)
 * @param Point 判定するコードポイント
 * @return 該当する文字なら true
 */
bool CommentNormalizer::IsFormatControl(const char32_t Point) {
	// 書式の制御かの返戻
	return Point == 0XAD || Point > 0X200A && Point < 0X2010 || Point > 0X2029 && Point < 0X202F ||
	Point > 0X205F && Point < 0X2070 || Point > 0XFDFF && Point < 0XFE10 || Point == 0XFEFF || Point > 0XDFFFF && Point < 0XE01F0;
}

/**
 * 半角語として扱う単位・通貨記号の判定関数
 * 計算量：O(1)
 * @param Point 判定するコードポイント
 * @return 該当する文字なら true
 */
bool CommentNormalizer::IsHalfWidthQuantityMark(const char32_t Point) {
	// 数式の上付・下付の数字，パーセント・パーミル・度・分・秒又は Unicode 17.0.0 の互換幅形を除く通貨記号かの返戻
	return Point > 0XB1 && Point < 0XB4 || Point == 0XB9 || Point == 0X2070 || Point > 0X2073 && Point < 0X207A ||
	Point > 0X207F && Point < 0X208A || Point == '%' || Point == 0XB0 || Point == 0X2030 || Point > 0X2031 && Point < 0X2034 ||
	Point == '$' || Point > 0XA1 && Point < 0XA6 || Point == 0X58F || Point == 0X60B || Point > 0X7FD && Point < 0X800 ||
	Point > 0X9F1 && Point < 0X9F4 || Point == 0X9FB || Point == 0XAF1 || Point == 0XBF9 || Point == 0XE3F || Point == 0X17DB ||
	Point > 0X209F && Point < 0X20C2 || Point == 0XA838 || Point == 0XFDFC || Point > 0X11FDC && Point < 0X11FE1 ||
	Point == 0X1E2FF || Point == 0X1ECB0;
}

/**
 * 本文へ残す文末の記号の判定関数
 * 記号自身が意味を持つ物は文末でも除かない
 * @param Mark 判定する記号
 * @return 疑問符又は感嘆符なら true
 */
bool CommentNormalizer::IsKeptSentenceMark(const std::string_view Mark) {
	// 疑問符と感嘆符との一致の返戻
	return Mark == "？" || Mark == "！" || Mark == "?" || Mark == "!";
}

/**
 * 空白・書式制御を除く全角文字の判定関数
 * 計算量：O(1)
 * @param Point 判定するコードポイント
 * @return 該当する文字なら true
 */
bool CommentNormalizer::IsFullWidth(const char32_t Point) {
	// ASCII・表音文字・空白・書式の制御の何れでもないかの返戻
	return Point > 0XA0 && !IsPhoneticLetter(Point) && !IsFormatControl(Point) && !IsHalfWidthQuantityMark(Point) &&
	!IsUnicodeSpace(Point) && (Point < 0X2028 || Point > 0X2029);
}

/**
 * 全角の区切記号・括弧・引用符・連結記号の判定関数
 * 計算量：O(1)
 * @param Point 判定するコードポイント
 * @return 該当する文字なら true
 */
bool CommentNormalizer::IsFullWidthMark(const char32_t Point) {
	// 全角の区切記号・括弧・引用符・連結記号の判定結果の返戻
	return Point == 0X3001 || Point == 0X3002 || Point > 0X3007 && Point < 0X3012 || Point > 0X3013 && Point < 0X301C ||
	Point == 0X30FB || Point == 0XFF01 || Point == 0XFF08 || Point == 0XFF09 || Point > 0XFF0B && Point < 0XFF10 ||
	Point > 0XFF19 && Point < 0XFF1D || Point == 0XFF1E || Point == 0XFF1F || Point == 0XFF3B || Point == 0XFF3D ||
	Point == 0XFF5B || Point == 0XFF5D || Point > 0X2017 && Point < 0X2020 || Point > 0X301C && Point < 0X3020 ||
	Point > 0X200F && Point < 0X2014 || Point == 0X301C || Point == 0XFF5E || Point == 0XFF1D || Point == 0XFF0B || Point == 0XD7 ||
	Point == 0XF7 || Point == 0XFF06 || Point == 0XFF0A;
}

/**
 * 空白を許す矢印・ダッシュの判定関数
 * 計算量：O(1)
 * @param Point 判定するコードポイント
 * @return 該当する文字なら true
 */
bool CommentNormalizer::IsSpacedMark(const char32_t Point) {
	// 右矢印・左右矢印・ダッシュかの返戻
	return Point == 0X2192 || Point == 0X2194 || Point == 0X2014;
}

/**
 * 半角語との境界を持つ全角文字の判定関数
 * 計算量：O(1)
 * @param Point 判定するコードポイント
 * @return 該当する文字なら true
 */
bool CommentNormalizer::IsFullWidthWord(const char32_t Point) {
	// 区切・矢印でない全角の字かの返戻
	return IsFullWidth(Point) && !IsFullWidthMark(Point) && !IsSpacedMark(Point);
}

/**
 * 半角語の端に該当する文字の判定関数
 * 計算量：O(1)
 * @param Point 判定するコードポイント
 * @return 該当する文字なら true
 */
bool CommentNormalizer::IsWordEdgeChar(const char32_t Point) {
	// 英数字・`_`・`+`・`#`・表音文字かの返戻
	return Point < 0X80 ?
	std::isalnum(static_cast<int>(Point)) || Point == '_' || Point == '+' || Point == '#' || IsHalfWidthQuantityMark(Point) :
	IsPhoneticLetter(Point) || IsHalfWidthQuantityMark(Point);
}

/**
 * 指定境界の前後の英数字の判定関数
 * 直前の字は UTF-8 の先頭へ戻して読み，文字種の共通の固定表を使う
 * 計算量：固定区間の数 R に対し O(log R)，読取は最大４バイト，追加メモリ O(1)
 * @param Text 判定する本文
 * @param Pos 調べる境界（前側では末尾も可）
 * @param IsBefore 境界の直前の字を読むか（false なら境界から始まる字）
 * @param Work 文字の読取と固定表の照合回数（無ければ計上しない）
 * @return 境界に接する字が英数字なら true
 */
bool CommentNormalizer::IsAlnumAt(const std::string_view Text, const size_t Pos, const bool IsBefore, size_t *const Work) {
	size_t Start = Pos;
	if(IsBefore) {
		// 本文の外側の境界の返戻
		if(!Start || Start > Text.size()) return false;
		--Start;
		while(Start && Pos - Start < 4 && (static_cast<unsigned char>(Text[Start]) & 0XC0) == 0X80) {
			if(Work) ++*Work;
			--Start;
		}
	}
	size_t End = Start;
	char32_t Point = 0;
	const bool IsDecoded = TextEdit::IsUtf8Decoded(Text, End, Point);
	if(Work) *Work += IsDecoded ? End - Start : 1;
	// 指定境界に接するコードポイントの分類の返戻
	return IsDecoded && (!IsBefore || End == Pos) && IsUnicodeAlnum(Point, Work);
}

/**
 * エスケープした並びのバイト数の取得関数
 * バックスラッシュと次の字を１つの並びとして読む
 * 制約：Pos < Text.size()
 * 例外を送出しない
 * 計算量：O(1)
 * @param Text 走査対象
 * @param Pos 判定位置
 * @param IsMultiline 改行を跨ぐ並びを認めるか
 * @return エスケープした並びなら２，其の他は０
 */
size_t CommentNormalizer::GetEscapeWidth(const std::string_view Text, const size_t Pos, const bool IsMultiline) {
	// 許された範囲でエスケープする２バイトの並びかの返戻
	return Text[Pos] == '\\' && Pos + 1 < Text.size() && (IsMultiline || Text[Pos + 1] != '\n') ? 2 : 0;
}

/**
 * 引用値の終端位置の取得関数
 * エスケープした字は閉引用符として読まない
 * 例外を送出しない
 * 計算量：引用値のバイト数 N に対し O(N)
 * @param Text コメント本文
 * @param Pos 開引用符の位置
 * @param IsMultiline 改行を跨ぐ値か
 * @return 閉引用符の位置，又は許された範囲の終端
 */
size_t CommentNormalizer::GetQuoteEnd(const std::string_view Text, const size_t Pos, const bool IsMultiline) {
	// 閉引用符又は行末迄のエスケープを含む走査
	size_t End = Pos + 1;
	while(End < Text.size() && Text[End] != Text[Pos] && (IsMultiline || Text[End] != '\n')) {
		if(const size_t Width = GetEscapeWidth(Text, End, IsMultiline)) End += Width;
		else ++End;
	}
	// 引用値の終端の返戻
	return End;
}

/**
 * 引用符の中身が説明文かの判定関数
 * 和文の句点か，語の間の空白と文末の記号を持つ中身は，トークン・値でなく説明文の引用とする（整形仕様 7.3）
 * 例外を送出しない
 * 計算量：引用内のバイト数 N に対し O(N)，追加記憶は O(1)
 * @param Inner 引用符の内側
 * @return 文境界を含む説明文なら true
 */
bool CommentNormalizer::IsQuotedProse(std::string_view Inner) {
	// 引用内の末尾の空白を除いた説明範囲
	while(!Inner.empty()) {
		const size_t Width = Inner.back() == '\r' ? 1 : GetSpaceWidthBefore(Inner, Inner.size());
		if(!Width) break;
		Inner.remove_suffix(Width);
	}
	// 空の引用が説明文でない事の返戻
	if(Inner.empty()) return false;
	// 和文の句点を持つ説明である事の返戻
	if(Inner.find("。") != std::string_view::npos || Inner.find("．") != std::string_view::npos) return true;
	bool HasSpace = false, HasSeparatedMark = false, IsAfterMark = false;
	for(size_t Pos = 0; Pos < Inner.size();) {
		if(const size_t Width = GetSpaceWidth(Inner, Pos)) {
			HasSpace = true;
			HasSeparatedMark = HasSeparatedMark || IsAfterMark && (Width > 1 || Inner[Pos] == ' ');
			IsAfterMark = IsAfterMark && Inner[Pos] != '\t';
			Pos += Width;
			continue;
		}
		// 文末記号と空白の後に続く文を持つ事の返戻
		if(HasSeparatedMark) return true;
		IsAfterMark = Inner[Pos] == '.' || Inner[Pos] == '?' || Inner[Pos] == '!';
		++Pos;
	}
	// 語の間の空白と末尾の文末記号を持つ説明である事の返戻
	return HasSpace && IsAfterMark;
}

/**
 * 文境界での論理行の分割関数（整形仕様 7.3）
 * 文毎に論理行を分け，文末の句点を除く（疑問符・感嘆符は残す）
 * 除くと行が `\` で終わり次の行を続ける記号は残す（C 系の行継続が次の行をコメントへ取り込む為）
 * 記号の一部の頭で始まる最初の論理行から分けた行は，行記法では同じ頭を継ぐ
 * 計算量：本文の長さ N と境界で補うテキストの総長 S に対し O(N + S)
 * @param Text 本文の論理行（破壊的に書き換えられる）
 * @param Boundaries 文境界の位置順の並び
 * @param StackMarker 行末へ積んだ次の文の始まりのテキスト（行記法の記号と空白，囲みは空）
 * @param SymbolHead 最初の論理行が始まる記号の一部の頭（行記法で最初の論理行から分けた行も継ぐ，無ければ空）
 * @param Protected コードの保護標識（書換に合わせて更新される）
 * @throws std::bad_alloc 描画する本文のメモリを確保出来ない場合
 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
 */
void CommentNormalizer::SplitSentences(
	std::string &Text,
	const std::span<const SentenceBoundary> Boundaries,
	const std::string_view StackMarker,
	const std::string_view SymbolHead,
	std::vector<unsigned char> &Protected
) {
	// 文末記号が無い時の終了
	if(Boundaries.empty()) return;
	std::string Output;
	std::vector<unsigned char> OutputProtected;
	Output.reserve(Text.size());
	OutputProtected.reserve(Text.size());
	size_t Copied = 0;
	// 原文と保護標識の同時転写
	const auto Copy = [&](const size_t To) -> void {
		Output.append(Text, Copied, To - Copied);
		OutputProtected.insert(OutputProtected.end(), Protected.begin() + Copied, Protected.begin() + To);
		Copied = To;
		// 終了
		return;
	};
	// 判定を保つ空白と文区切の追記
	const auto Insert = [&](const std::string_view Added, const unsigned char IsProtected) -> void {
		Output += Added;
		OutputProtected.resize(Output.size(), IsProtected);
		// 終了
		return;
	};
	for(const SentenceBoundary &Boundary : Boundaries) {
		// 非句点の点だけを保護し，直後の密着を保ちながら其の次の文字との空白は通常通り整理する
		if(Boundary.IsAttached) Protected[Boundary.Pos] = 1;
		Copy(Boundary.Pos);
		if(Boundary.IsPeriod) {
			Copied = Boundary.MarkEnd;
			if(Boundary.IsSpace) Insert(" ", 1);
			else {
				// 句点除去で行末又は括弧の内側へ露出する，保護範囲の外の空白の除去
				const bool IsBodyEnd = Boundary.Next == Boundary.LineEnd || Boundary.Next < Boundary.LineEnd && !Protected[Boundary.Next] &&
				std::string_view(")]}").find(Text[Boundary.Next]) != std::string_view::npos;
				if(IsBodyEnd) while(!Output.empty() && !OutputProtected.back()) {
					const size_t Width = GetSpaceWidthBefore(Output, Output.size());
					if(!Width) break;
					Output.resize(Output.size() - Width);
					OutputProtected.resize(Output.size());
				}
				if(
					IsBodyEnd || Output.empty() || Output.back() == '\n' ||
					!OutputProtected.back() && std::string_view("([{").find(Output.back()) != std::string_view::npos
				) while(Copied < Boundary.LineEnd && !Protected[Copied]) {
					const size_t Width = GetSpaceWidth(Text, Copied);
					if(!Width) break;
					Copied += Width;
				}
			}
		} else Copy(Boundary.MarkEnd);
		// 文末記号の後の Zs だけを半角へ揃え，本文中のタブ・CR・書式送りは既存の転写に任せる
		if(Boundary.IsSentenceEnd && !Boundary.IsSplit) while(Copied < Boundary.Next && !Protected[Copied]) {
			const size_t Width = GetSpaceWidth(Text, Copied);
			if(!Width) break;
			if(Width == 1) Copy(Copied + Width);
			else {
				Copied += Width;
				Insert(" ", 0);
			}
		}
		// 行末へ積んだ記号の前では，新たな行を作らない
		if(
			Boundary.IsSplit && Boundary.Next < Boundary.LineEnd &&
			(StackMarker.empty() || Text.compare(Boundary.Next, StackMarker.size(), StackMarker))
		) {
			Insert("\n", 0);
			if(!Boundary.LineBegin && !SymbolHead.empty()) {
				Insert(SymbolHead, 0);
				Insert(" ", 0);
			}
			Copied = Boundary.Next;
		}
	}
	Copy(Text.size());
	Text = std::move(Output);
	Protected = std::move(OutputProtected);
	// 終了
	return;
}

/**
 * 文境界の収集関数（整形仕様 7.3）
 * 保持範囲を除いて入子の囲みを確定し，点列を後ろから１度処理する
 * 括弧・クォート内の句点も除くが，其の内側では論理行を分けない
 * 句点除去で変わるクォートの判定は，除いた位置の保護した半角スペースで保持する
 * 計算量：本文のバイト数 N に対し O(N + 1)，追加メモリは O(N + 1)
 * @param Text コメント本文
 * @param Protected コードの保護標識
 * @param IsBodyHead 本文頭を含む範囲か（記法の枠を除いた後の先頭も含む）
 * @param Work 本文と文末記号の訪問回数（無ければ計上しない）
 * @param EndingBytes 括弧・終了タグの後続判定の表のバイト数（無ければ計上しない）
 * @return 文境界の位置順の並び
 * @throws std::bad_alloc 文境界又は開いた記号のメモリを確保出来ない場合
 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
 */
std::vector<CommentNormalizer::SentenceBoundary> CommentNormalizer::FindSentenceBoundaries(
	const std::string_view Text,
	const std::span<const unsigned char> Protected,
	const bool IsBodyHead,
	size_t *const Work,
	size_t *const EndingBytes
) {
	// 記号・閉じ・クォートの判定を本文の位置へ対応付ける作業表
	std::vector<SentenceBoundary> Boundaries;
	std::vector<unsigned char> Endings, Spaces;
	// 括弧の閉じと説明文の終了タグは，後続の文末判定で同じ事実として参照する
	const auto MarkEnding = [&Endings, &Text, EndingBytes](const size_t Pos) -> void {
		if(Endings.empty()) {
			Endings.resize(Text.size(), 0);
			if(EndingBytes) *EndingBytes += Text.size();
		}
		Endings[Pos] = 1;
		// 終了
		return;
	};
	std::vector<SentenceQuote> Quotes;
	size_t Depth = 0;
	// タグの属性値を説明文と区別する走査状態
	unsigned char TagQuote = 0;
	bool IsInTag = false, HasQuotes = false;
	// 固定の対応表から先頭バイトの候補を１度作り，通常の字に１８組を照合しない
	static constexpr std::array<bool, 256> BracketHeads = []() -> std::array<bool, 256> {
		std::array<bool, 256> Heads {};
		for(const std::pair<std::string_view, std::string_view> &Pair : SentenceBrackets) {
			Heads[static_cast<unsigned char>(Pair.first.front())] = true;
			Heads[static_cast<unsigned char>(Pair.second.front())] = true;
		}
		// 開閉の候補の返戻
		return Heads;
	}();
	// 括弧・引用の対応は入力の改行を越えて保持する
	std::vector<SentenceOpen> Opens;
	for(size_t LineBegin = 0; LineBegin < Text.size();) {
		const size_t LineEnd = std::min(Text.find('\n', LineBegin), Text.size());
		bool HasBody = false;
		// 本文頭の数字列又は英字１字に続く列挙番号の点
		size_t Enumeration = IsBodyHead ? LineBegin : LineEnd;
		while(Enumeration < LineEnd && !Protected[Enumeration]) {
			const size_t Width = GetSpaceWidth(Text, Enumeration);
			if(!Width) break;
			Enumeration += Width;
		}
		bool IsNumber = false;
		for(size_t At = Enumeration; At < LineEnd;) {
			size_t End = At;
			char32_t Point = 0;
			const bool IsDecoded = TextEdit::IsUtf8Decoded(Text, End, Point);
			if(Work) *Work += IsDecoded ? End - At : 1;
			if(!IsDecoded) {
				Enumeration = LineEnd;
				break;
			}
			const UnicodeAlnumKind Kind = GetUnicodeAlnumKind(Point, Work);
			if(Kind == UnicodeAlnumKind::Number) {
				Enumeration = At = End;
				IsNumber = true;
			} else {
				if(!IsNumber) Enumeration = Kind == UnicodeAlnumKind::Letter ? End : LineEnd;
				break;
			}
		}
		for(size_t Pos = LineBegin; Pos < LineEnd;) {
			if(Work) ++*Work;
			// 属性値のクォートと句点を説明文の囲み・文末記号として読まない
			if(IsInTag) {
				const unsigned char Char = Text[Pos];
				if(TagQuote) {
					if(Char == TagQuote) TagQuote = 0;
				} else switch(Char) {
				case '"':
				case '\'':
					TagQuote = Char;
					break;
				case '>':
					// 自己閉タグは保持する本文の一部であり，説明文の閉括弧ではない
					HasBody = HasBody || Pos && Text[Pos - 1] == '/';
					IsInTag = false;
					break;
				case '<':
					IsInTag = false;
					break;
				}
				if(Char != '<' || TagQuote) {
					++Pos;
					continue;
				}
			}
			// 名前を持つタグの開始の識別（保護要素の終了タグの属性も説明文へ混ぜない）
			if(Text[Pos] == '<' && Pos + 1 < LineEnd && (!Protected[Pos] || Text[Pos + 1] == '/')) {
				const bool IsClosing = Text[Pos + 1] == '/';
				const size_t Start = Pos + (IsClosing ? 2 : 1);
				size_t End = Start;
				if(Start < LineEnd && (std::isalpha(static_cast<unsigned char>(Text[Start])) || Text[Start] == '_')) {
					while(End < LineEnd && (IsIdentifierChar(Text[End]) || Text[End] == ':' || Text[End] == '-')) ++End;
					if(End == LineEnd || std::isspace(static_cast<unsigned char>(Text[End])) || Text[End] == '/' || Text[End] == '>') {
						if(IsClosing) MarkEnding(Pos);
						IsInTag = true;
						TagQuote = 0;
						Pos = End;
						continue;
					}
				}
			}
			// 保護範囲の読飛
			if(Protected[Pos]) {
				HasBody = HasBody || !std::isspace(static_cast<unsigned char>(Text[Pos]));
				++Pos;
				continue;
			}
			// エスケープした字を文末記号・開閉記号として読まない
			if(const size_t Width = GetEscapeWidth(Text, Pos, false)) {
				HasBody = true;
				if(Text[Pos + 1] == '\'' || Text[Pos + 1] == '"') {
					if(Quotes.empty()) Quotes.resize(Text.size(), SentenceQuote::None);
					Quotes[Pos + 1] = SentenceQuote::Escaped;
				}
				Pos += Width;
				continue;
			}
			// 英数字間のアポストロフィと，開いて居るクォートの閉じを優先する判定
			if(Text[Pos] == '"' || Text[Pos] == '\'') {
				if(Quotes.empty()) Quotes.resize(Text.size(), SentenceQuote::None);
				HasBody = true;
				const bool HasAlnumBefore = IsAlnumAt(Text, Pos, true, Work), HasAlnumAfter = IsAlnumAt(Text, Pos + 1, false, Work);
				const std::string_view Mark = Text.substr(Pos, 1);
				if(Text[Pos] != '\'' || !HasAlnumBefore || !HasAlnumAfter) {
					if(!Opens.empty() && Opens.back().Mark == Mark) {
						Opens.pop_back();
						--Depth;
						MarkEnding(Pos);
						Quotes[Pos] = Text[Pos] == '\'' ? SentenceQuote::ClosedSingle : SentenceQuote::ClosedDouble;
					} else if(Text[Pos] == '"' || !HasAlnumBefore) {
						Opens.push_back({ Mark });
						++Depth;
						Quotes[Pos] = Text[Pos] == '\'' ? SentenceQuote::OpenSingle : SentenceQuote::OpenDouble;
					}
				}
				HasQuotes = HasQuotes || Quotes[Pos] != SentenceQuote::None;
				++Pos;
				continue;
			}
			// 括弧・引用の開閉
			size_t Bracket = 0;
			if(BracketHeads[static_cast<unsigned char>(Text[Pos])]) {
				for(const std::pair<std::string_view, std::string_view> &BracketPair : SentenceBrackets) {
					if(Work) ++*Work;
					if(
						const std::string_view Opening = BracketPair.first, Closing = BracketPair.second;
						!Text.compare(Pos, Opening.size(), Opening)
					) {
						Opens.push_back({ Opening });
						++Depth;
						Bracket = Opening.size();
					} else if(!Text.compare(Pos, Closing.size(), Closing)) {
						MarkEnding(Pos);
						if(!Opens.empty() && Opens.back().Mark == Opening) {
							--Depth;
							Opens.pop_back();
						}
						Bracket = Closing.size();
					}
					if(Bracket) break;
				}
			}
			if(Bracket) {
				HasBody = true;
				Pos += Bracket;
				continue;
			}
			if(const size_t Width = GetSentenceMarkWidth(Text, Pos)) {
				Boundaries.push_back({ Pos, Pos + Width, LineBegin, LineEnd, Pos + Width, static_cast<bool>(Depth), HasBody });
				// 列挙番号と，除くと行継続を露出する既存の記法の保持
				size_t Before = Pos;
				while(Before > LineBegin && !Protected[Before - 1]) {
					const size_t Width = GetSpaceWidthBefore(Text, Before);
					if(!Width) break;
					Before -= Width;
				}
				Boundaries.back().IsAttached = Text[Pos] == '.' && Pos == Enumeration || Before && Text[Before - 1] == '\\';
				Pos += Width;
			} else {
				const size_t SpaceWidth = GetSpaceWidth(Text, Pos);
				HasBody = HasBody || !SpaceWidth && !std::isspace(static_cast<unsigned char>(Text[Pos]));
				Pos += SpaceWidth ? SpaceWidth : 1;
			}
		}
		LineBegin = LineEnd + 1;
	}
	// 文末記号の無い本文は，後方の判定・転写を必要としない
	if(Boundaries.empty()) return Boundaries;
	// 句点除去・文区切と，疑問符・感嘆符の文末判定を隣接する記号から１度だけ継ぐ
	std::vector<unsigned char> Removed(HasQuotes ? Text.size() : 0, 0), BreakBefore(HasQuotes ? Text.size() + 1 : 0, 0);
	size_t RightPos = Text.size(), RightNext = Text.size();
	bool HasRightBreak = false, HasRightEnd = false;
	for(size_t Idx = Boundaries.size(); Idx--;) {
		if(Work) ++*Work;
		SentenceBoundary &Boundary = Boundaries[Idx];
		const size_t Pos = Boundary.Pos;
		const bool IsAdjacent = Boundary.MarkEnd == RightPos;
		const size_t Following = IsAdjacent ? RightNext : Boundary.MarkEnd;
		const bool IsGap =
		GetSpaceWidth(Text, Following) || Following < Text.size() && std::isspace(static_cast<unsigned char>(Text[Following]));
		const bool IsKept = IsKeptSentenceMark(Text.substr(Pos, Boundary.MarkEnd - Pos));
		Boundary.IsSentenceEnd = !Boundary.IsAttached && (
			Boundary.MarkEnd - Pos == 3 || Following == Text.size() || IsAdjacent && HasRightBreak || IsGap ||
			IsAdjacent && HasRightEnd && IsKept || !Endings.empty() && Endings[Following]
		);
		Boundary.IsPeriod = Boundary.IsSentenceEnd && !IsKept;
		Boundary.IsAttached = Text[Pos] == '.' && !Boundary.IsPeriod;
		if(HasQuotes) Removed[Pos] = Boundary.IsPeriod;
		RightNext = Boundary.IsPeriod ? Following : Pos;
		HasRightBreak = Boundary.IsPeriod && (IsAdjacent && HasRightBreak || !Boundary.IsEnclosed);
		HasRightEnd = Boundary.IsSentenceEnd;
		RightPos = Pos;
	}
	// 同じ点列の境界は１つに纏め，実際に改行を置く後続の位置を共有する
	for(size_t First = 0; First < Boundaries.size();) {
		size_t Last = First;
		bool IsSplit = false;
		while(1) {
			SentenceBoundary &Boundary = Boundaries[Last];
			IsSplit = IsSplit || Boundary.IsSentenceEnd && !Boundary.IsEnclosed;
			while(Boundary.Next < Boundary.LineEnd && !Protected[Boundary.Next]) {
				const size_t Width = GetSpaceWidth(Text, Boundary.Next);
				if(!Width) break;
				Boundary.Next += Width;
			}
			if(
				!Boundary.IsSentenceEnd || Last + 1 == Boundaries.size() || !Boundaries[Last + 1].IsSentenceEnd ||
				Boundary.LineEnd != Boundaries[Last + 1].LineEnd || Boundary.Next != Boundaries[Last + 1].Pos
			) break;
			++Last;
		}
		SentenceBoundary &Boundary = Boundaries[Last];
		const bool HasFollowingCloser = Boundary.Next < Text.size() && !Endings.empty() && Endings[Boundary.Next];
		Boundary.IsSplit = IsSplit && Boundaries[First].HasBody && !HasFollowingCloser;
		if(HasQuotes && Boundary.IsSplit && Boundary.Next < Boundary.LineEnd) BreakBefore[Boundary.Next] = 1;
		First = Last + 1;
	}
	// クォートの無い本文では，除去後の前後の文字を改めて走査しない
	if(!HasQuotes) return Boundaries;
	Spaces.resize(Text.size(), 0);
	// クォートの再判定だけが使う，句点除去後の次の位置
	std::vector<size_t> Next(Text.size() + 1, Text.size());
	for(size_t End = Text.size(); End;) {
		if(Work) ++*Work;
		size_t Pos = End - 1;
		while(Pos && (static_cast<unsigned char>(Text[Pos]) & 0XC0) == 0X80) --Pos;
		std::fill(Next.begin() + Pos, Next.begin() + End, Removed[Pos] ? Next[End] : Pos);
		End = Pos;
	}
	// 句点除去の前後でクォートの開閉・アポストロフィ・エスケープが変わる位置の空白
	size_t PriorRemoved = Text.size(), Slashes = 0;
	char32_t PreviousPoint = 0;
	for(size_t Pos = 0; Pos < Text.size();) {
		if(Work) ++*Work;
		// 文区切の後のクォートは前の文の文字・バックスラッシュと接しない
		if(BreakBefore[Pos]) {
			PriorRemoved = Text.size();
			PreviousPoint = 0;
			Slashes = 0;
		}
		size_t End = Pos;
		char32_t Point = 0;
		if(!TextEdit::IsUtf8Decoded(Text, End, Point)) End = Pos + 1;
		if(Removed[Pos]) {
			PriorRemoved = Pos;
			Pos = End;
			continue;
		}
		if(Quotes[Pos] != SentenceQuote::None && Quotes[Pos] != SentenceQuote::Escaped) {
			const size_t Following = Next[End];
			const bool HasAlnumBefore = IsUnicodeAlnum(PreviousPoint, Work);
			const bool HasAlnumAfter = !BreakBefore[Following] && IsAlnumAt(Text, Following, false, Work);
			if(
				Slashes % 2 || Quotes[Pos] == SentenceQuote::OpenSingle && HasAlnumBefore ||
				Quotes[Pos] == SentenceQuote::ClosedSingle && HasAlnumBefore && HasAlnumAfter
			) {
				// 直前に除いた句点は英数字でない為，元のクォートの役割を其の位置の空白で保つ
				if(PriorRemoved < Pos) Spaces[PriorRemoved] = 1;
				else if(End < Text.size() && Removed[End]) Spaces[End] = 1;
				Slashes = 0;
			}
		}
		PreviousPoint = Point;
		PriorRemoved = Text.size();
		Slashes = Point == '\\' ? Slashes + 1 : 0;
		Pos = End;
	}
	// クォートの判定を保つ空白を，転写する句点の位置へ登録
	for(SentenceBoundary &Boundary : Boundaries) Boundary.IsSpace = Spaces[Boundary.Pos];
	// 位置順の句点除去・空白保持・文区切の返戻
	return Boundaries;
}

/**
 * 数字と半角の点だけの語かの判定関数
 * 最初の字は数字とし，数字の文字種を変えず節番号の並びを識別する
 * 計算量：語のバイト数 N に対し O(N)，追加メモリ O(1)
 * @param Term 判定する語
 * @return 数字で始まり数字・半角の点だけを含むなら true
 */
bool CommentNormalizer::IsNumberTerm(const std::string_view Term) {
	// 空の語の返戻
	if(Term.empty()) return false;
	for(size_t Pos = 0; Pos < Term.size();) {
		const size_t Begin = Pos;
		char32_t Point = 0;
		// 数字又は最初の字以外の半角の点でない事の返戻
		if(
			!TextEdit::IsUtf8Decoded(Term, Pos, Point) || !(Begin && Point == '.' || GetUnicodeAlnumKind(Point) == UnicodeAlnumKind::Number)
		) return false;
	}
	// 数字と半角の点だけの語である事の返戻
	return true;
}

/**
 * 読点候補の判定関数
 * 計算量：O(1)
 * @param Point 本文のコードポイント
 * @return 半角・全角のカンマ又は読点なら true
 */
bool CommentNormalizer::IsComma(const char32_t Point) {
	// 読点候補との一致の返戻
	return Point == ',' || Point == 0XFF0C || Point == 0X3001;
}

/**
 * 同じ本文世代の UTF-8 と読点候補の共通走査関数
 * 計算量：本文のバイト数 B に対し O(B)，追加メモリ O(1)
 * @param Text 正規化前の本文
 * @param Work デコードした文字数の計上先（無ければ計上しない）
 * @return UTF-8 の成立と読点候補の有無
 */
CommentNormalizer::BodyFacts CommentNormalizer::GetBodyFacts(const std::string_view Text, size_t *const Work) {
	BodyFacts Facts;
	for(size_t Pos = 0; Pos < Text.size();) {
		if(Work) ++*Work;
		char32_t Point = 0;
		if(!TextEdit::IsUtf8Decoded(Text, Pos, Point)) {
			Facts.IsValidUtf8 = false;
			// 不正な並びを含む本文の事実の返戻
			return Facts;
		}
		Facts.HasCommas = Facts.HasCommas || IsComma(Point);
	}
	// 同じ本文世代を１度だけ読んだ事実の返戻
	return Facts;
}

/**
 * 列挙全体の文字種に従う読点の正規化関数（整形仕様 7.3）
 * 計算量：本文長 N，タグと仮引数名のマーカーの長さの和 K に対し O(N(K + 1))
 * @param Rules 投影済のドキュメント記法と名前・型の書式
 * @param Text UTF-8 を確認済の本文
 * @param Protected 同じ本文世代のコードの保護標識（書換に合わせて更新する）
 * @throws std::bad_alloc 読点と転写する本文のメモリを確保出来ない場合
 * @throws std::length_error 構築する文字列・作業表の要素数が格納先の上限を超える場合
 */
void CommentNormalizer::NormalizeCommas(
	const Core::NotationRules &Rules,
	std::string &Text,
	std::vector<unsigned char> &Protected
) {
	// 読点の原文位置と列挙全体から確定する文字種
	struct CommaChange {
		size_t Pos; // 原文の開始位置
		size_t Width; // 原文のバイト数
		bool IsWide; // 全角へ揃えるか
	};
	enum class HeaderTokenKind : uint8_t { None, Param, Type }; // ドキュメントの名前・型のトークンの役割
	HeaderTokenKind Header = HeaderTokenKind::None; // 変換しない次のドキュメントトークンの役割
	std::vector<CommaChange> Changes;
	std::vector<size_t> Enumeration;
	// 列挙の語・区切・文字種とドキュメント化タグの走査状態
	bool HasTerm = false, IsAfterComma = false, HasWideTerm = false, HasWideClause = false, IsTagHead = false, HasWideItem = false;
	bool HasHalfItem = false, HasWideWordItem = false, IsAmbiguous = false, IsNumList = false, IsPrevNumTerm = false; // １項目の文字種と，説明との境界が未確定か // 全角の説明の後に続く節番号だけの列挙か // 直前の語が数字と半角の点だけか
	// 括弧の内側の列挙と，外側の項目の文字種を分けて持つ
	struct CommaScope {
		std::string_view Close; // 対応する閉記号
		std::vector<size_t> Enumeration; // 外側の列挙の読点
		bool HasWideTerm; // 外側の列挙が全角の語を含むか
		bool HasWideClause; // 外側の説明が全角の語を含むか
		bool IsNumList; // 外側が節番号の列挙か
		bool HasWideItem; // 外側の項目が全角の語を含むか
		bool HasHalfItem; // 外側の項目が半角の語を含むか
		bool HasWideWordItem; // 外側の項目が全角の語の字を含むか
		bool IsAmbiguous; // 外側の列挙の範囲が未確定か
		bool HasWideContents = false; // 内側の項目が全角の語を含むか
	};
	std::vector<CommaScope> Scopes;
	// 後続の語迄読んだ列挙の読点の確定（読点数 C に対し O(C)）
	const auto Finish = [&]() -> void {
		// 同じ列挙の読点の文字種の共有
		if(!IsAmbiguous) for(const size_t Idx : Enumeration) Changes[Idx].IsWide = HasWideTerm;
		Enumeration.clear();
		HasTerm = IsAfterComma = HasWideTerm = IsNumList = IsPrevNumTerm = false;
		HasWideItem = HasHalfItem = HasWideWordItem = IsAmbiguous = false;
		// 終了
		return;
	};
	// 語・記法・読点の１度の走査
	for(size_t Pos = 0; Pos < Text.size();) {
		const size_t Begin = Pos;
		const unsigned char Byte = Text[Pos];
		if(Byte == ' ' || Byte == '	' || Byte == '\r') {
			++Pos;
			continue;
		}
		// ドキュメント化タグ直後の型・方向の記法を，説明文の列挙として変換しない
		if(
			IsTagHead && (
				Byte == '[' && (
					Rules.HasBracketedDocType || Core::HasDocumentForm(Rules, Core::DocumentFormId::Doxygen) ||
					Core::HasDocumentForm(Rules, Core::DocumentFormId::JSDoc)
				) || Byte == '{' && Core::HasDocumentForm(Rules, Core::DocumentFormId::JSDoc)
			)
		) {
			const unsigned char Close = Byte == '[' ? ']' : '}';
			size_t Depth = 1;
			for(++Pos; Pos < Text.size() && Depth && Text[Pos] != '\n'; ++Pos) {
				if(Text[Pos] == Byte) ++Depth;
				else if(Text[Pos] == Close) --Depth;
			}
			if(Byte == '[' && Header == HeaderTokenKind::Param && !Depth) {
				const std::span<const std::string_view> Directions =
				Core::VocabularyOf(Rules.TagVocabulary, Core::DocDescription::Param).Qualifiers;
				if(
					!Rules.HasBracketedDocType && !(
						Core::HasDocumentForm(Rules, Core::DocumentFormId::Doxygen) && std::ranges::any_of(
							Directions,
							// 入出力の修飾語と省略可能な仮引数名の区別
							[&Text, Begin, Pos](const std::string_view Direction) -> bool {
								// 空白・大文字を除いた修飾語の照合（修飾語長 L に対し O(L)）
								size_t At = 0;
								for(size_t Idx = Begin + 1; Idx + 1 < Pos; ++Idx) {
									const unsigned char Char = static_cast<unsigned char>(Text[Idx]);
									if(Char == ' ' || Char == '\t') continue;
									// 修飾語と一致しない事の返戻
									if(At == Direction.size() || std::tolower(Char) != Direction[At++]) return false;
								}
								// 修飾語と一致する事の返戻
								return At == Direction.size();
							}
						)
					)
				) Header = HeaderTokenKind::None;
			} else if(Header == HeaderTokenKind::Type) Header = HeaderTokenKind::None;
			continue;
		}
		// 仮引数の名前・裸の型のトークンを，説明文の列挙として変換しない
		if(Header != HeaderTokenKind::None && Byte != '\n') {
			size_t Depth = 0;
			for(; Pos < Text.size() && Text[Pos] != '\n'; ++Pos) {
				const unsigned char Char = Text[Pos];
				if(!Depth && (Char == ' ' || Char == '\t')) break;
				if(std::string_view("([{<").find(Char) != std::string_view::npos) ++Depth;
				else if(Depth && std::string_view(")]}>").find(Char) != std::string_view::npos) --Depth;
			}
			const std::string_view Token(Text.data() + Begin, Pos - Begin);
			IsTagHead = Header == HeaderTokenKind::Param && Rules.HasBracketedDocType;
			if(
				Header == HeaderTokenKind::Type || !Rules.HasBareDocType ||
				!Rules.DocParamSigil.empty() && Token.starts_with(Rules.DocParamSigil) || std::ranges::any_of(
					Rules.DocParamMarks,
					// 型トークンと名前トークンを区別する名前のマーカーの照合述語
					[Token](const std::string_view Mark) -> bool {
						// 仮引数名を始めるマーカーかの返戻
						return Token.starts_with(Mark);
					}
				)
			) Header = HeaderTokenKind::None;
			Finish();
			continue;
		}
		char32_t Point = 0;
		size_t End = Pos;
		TextEdit::IsUtf8Decoded(Text, End, Point);
		const size_t CommaWidth = IsComma(Point) ? End - Pos : 0;
		if(CommaWidth && !Protected[Pos]) {
			// 空白の無い識別子・数の並びと，開きの直後に置いた記号の例はトークンとして保持
			if(
				HasTerm && !(
					Byte == ',' && Pos && Pos + 1 < Text.size() && (IsAlnumAt(Text, Pos, true) || Text[Pos - 1] == '_') &&
					(IsAlnumAt(Text, Pos + 1, false) || Text[Pos + 1] == '_')
				)
			) {
				if(!IsNumList && HasWideClause && !HasWideTerm && CommaWidth == 3 && (!IsPrevNumTerm || !Enumeration.empty())) {
					Finish();
					Changes.push_back({ Pos, CommaWidth, HasWideClause });
					HasWideClause = false;
				} else {
					HasWideTerm = HasWideTerm || !IsNumList && HasWideClause && CommaWidth == 3; // 語内の点を跨ぐ列挙の文字種を引継ぐ
					const bool IsWide = Enumeration.empty() && HasWideItem || CommaWidth == 3;
					Enumeration.push_back(Changes.size());
					Changes.push_back({ Pos, CommaWidth, IsWide });
					IsAfterComma = true;
				}
				HasWideItem = HasHalfItem = HasWideWordItem = false;
			}
			Pos += CommaWidth;
			continue;
		}
		Pos = End;
		if(!Protected[Begin]) {
			// 呼出の引数は値として保持し，外側では既に読んだ名前と合わせて１つの項目とする
			if(Byte == '(' && Begin && IsWordChar(static_cast<unsigned char>(Text[Begin - 1]))) {
				size_t Depth = 1;
				for(; Pos < Text.size() && Text[Pos] != '\n' && Depth; ++Pos) if(!Protected[Pos]) {
					if(Text[Pos] == '(') ++Depth;
					else if(Text[Pos] == ')') --Depth;
				}
				continue;
			}
			if(!Scopes.empty() && std::string_view(Text).substr(Begin).starts_with(Scopes.back().Close)) {
				const bool HasWideContents = Scopes.back().HasWideContents;
				Pos = Begin + Scopes.back().Close.size();
				Finish();
				CommaScope Parent = std::move(Scopes.back());
				Scopes.pop_back();
				Enumeration = std::move(Parent.Enumeration);
				HasTerm = true;
				HasWideTerm = Parent.HasWideTerm || HasWideContents;
				HasWideClause = Parent.HasWideClause || HasWideContents;
				IsNumList = Parent.IsNumList;
				HasWideItem = Parent.HasWideItem || HasWideContents;
				HasHalfItem = Parent.HasHalfItem || !HasWideContents;
				HasWideWordItem = Parent.HasWideWordItem || HasWideContents;
				IsAmbiguous = Parent.IsAmbiguous || HasWideItem && HasHalfItem;
				if(!Scopes.empty()) Scopes.back().HasWideContents = Scopes.back().HasWideContents || HasWideContents;
				continue;
			}
			bool IsOpen = false;
			for(const std::pair<std::string_view, std::string_view> &Pair : SentenceBrackets) {
				if(!std::string_view(Text).substr(Begin).starts_with(Pair.first)) continue;
				Scopes.push_back(
					{
						Pair.second,
						std::move(Enumeration),
						HasWideTerm,
						HasWideClause,
						IsNumList,
						HasWideItem,
						HasHalfItem,
						HasWideWordItem,
						IsAmbiguous
					}
				);
				Finish();
				HasWideClause = IsTagHead = false;
				Header = HeaderTokenKind::None;
				Pos = Begin + Pair.first.size();
				IsOpen = true;
				break;
			}
			if(IsOpen) continue;
		}
		if(
			Byte == '\n' || !Protected[Begin] && (
				std::string_view("()[]{}<>:;!?\"").find(Byte) != std::string_view::npos || Point > 0X3007 && Point < 0X301C ||
				Point == 0XFF08 || Point == 0XFF09 || Point == 0XFF1A || Point == 0XFF1B
			)
		) {
			Finish();
			HasWideClause = IsTagHead = false;
			Header = HeaderTokenKind::None;
			continue;
		}
		// コード片は１つの語とし，内側の読点を保護する
		const char32_t HeadPoint = Point;
		bool IsWide = false, IsHalf = false, IsWideWord = false;
		if(Protected[Begin]) {
			IsWide = IsFullWidth(Point);
			for(; Pos < Text.size() && Protected[Pos] && Text[Pos] != '\n';) {
				TextEdit::IsUtf8Decoded(Text, Pos, Point);
				IsWide = IsWide || IsFullWidth(Point);
			}
			IsHalf = !IsWide;
			IsWideWord = IsWide;
		} else {
			// URL・パスは，内部の区切を含む１つの半角語として保持
			bool IsPath = false;
			const bool HasPathStart =
			IsUnicodeAlnum(HeadPoint) || IsPhoneticLetter(HeadPoint) || std::string_view("_./~\\").find(Byte) != std::string_view::npos;
			for(size_t At = Begin;;) {
				IsPath = IsPath || HasPathStart && (Point == '/' || Point == '\\' || Point == ':' && !Text.compare(Pos, 2, "//"));
				// 囲み・保護境界・区切記号で語を閉じる
				if(
					!IsPath && !Scopes.empty() && std::string_view(Text).substr(At).starts_with(Scopes.back().Close) || Point == '\n' ||
					Point == ' ' || Point == '\t' || Point == '\r' || !IsPath && (
						Point == ',' || Point == 0XFF0C || Point == 0X3001 ||
						Point < 0X80 && std::string_view("()[]{}<>:;!?\"").find(static_cast<char>(Point)) != std::string_view::npos ||
						Point > 0X3007 && Point < 0X301C || Point == 0XFF08 || Point == 0XFF09 || Point == 0XFF1A || Point == 0XFF1B
					)
				) {
					Pos = At;
					break;
				}
				const bool IsWidePoint = IsFullWidth(Point);
				IsWide = IsWide || IsWidePoint;
				IsHalf = IsHalf || !IsWidePoint;
				IsWideWord = IsWideWord || IsWidePoint && !IsFullWidthMark(Point);
				if(Pos == Text.size() || Protected[Pos]) break;
				const size_t Next = Pos;
				TextEdit::IsUtf8Decoded(Text, Pos, Point);
				At = Next;
			}
			if(IsPath) {
				IsWide = IsWideWord = false;
				IsHalf = true;
			}
		}
		const std::string_view Term(Text.data() + Begin, Pos - Begin);
		IsTagHead = !Term.empty() && Rules.CodeExamples.TagMarks.find(Term.front()) != std::string_view::npos;
		if(IsTagHead) {
			if(Core::DocItemTagLength(Rules, Term, Core::DocDescription::Param) == Term.size()) Header = HeaderTokenKind::Param;
			else if(
				Rules.HasBareDocType && Core::DocItemTagLength(Rules, Term, Core::DocDescription::Return) == Term.size() ||
				!Rules.HasOptionalFailureType && Core::DocItemTagLength(Rules, Term, Core::DocDescription::Failure) == Term.size()
			) Header = HeaderTokenKind::Type;
			else Header = HeaderTokenKind::None;
		}
		// 数字と区切だけの項目と，其の他の語の区別
		const bool IsNumTerm = IsNumberTerm(Term), HasAssignment = Term.find('=') != std::string_view::npos;
		IsNumList = IsNumList && (IsAfterComma || IsNumTerm);
		// 節番号の列挙の後の説明は，番号の列挙に含めず別の区切とする
		if(IsAfterComma && !Enumeration.empty() && IsNumList && !IsNumTerm) {
			const size_t Separator = Enumeration.back();
			Enumeration.pop_back();
			Changes[Separator].IsWide = Changes[Separator].Width == 3;
			Finish();
			HasWideClause = false;
		} else if(HasTerm && !IsAfterComma && !HasWideTerm && !Enumeration.empty() && IsWide) {
			// 単独の半角語対の後の説明は，列挙か説明の区切かが未確定なら元の区切を保持
			if(Enumeration.size() == 1 && !IsNumList) {
				Changes[Enumeration.front()].IsWide = Changes[Enumeration.front()].Width == 3;
				Enumeration.clear();
			}
			Finish();
		}
		// ドキュメント名の後の節番号だけの並びは，半角の語の列挙として説明から分離
		if(!IsAfterComma && HasWideTerm && IsNumTerm && Term.find('.') != std::string_view::npos) {
			Finish();
			IsNumList = true;
		} else if(
			HasTerm && !IsAfterComma && HasWideTerm && !IsWide && Enumeration.empty() &&
			(Protected[Begin] || HasWideWordItem && !Term.empty() && !HasAssignment && IsPhoneticLetter(HeadPoint))
		) Finish();
		// 同じ項目の文字種を，空白を補う前後で同じ条件から判定する
		HasWideItem = HasWideItem || IsWide;
		HasHalfItem = HasHalfItem || IsHalf;
		HasWideWordItem = HasWideWordItem || IsWideWord;
		IsAmbiguous = IsAmbiguous || HasWideItem && HasHalfItem;
		if(!Scopes.empty()) Scopes.back().HasWideContents = Scopes.back().HasWideContents || IsWide;
		HasWideTerm = HasWideTerm || IsWide;
		HasWideClause = HasWideClause || IsWide;
		HasTerm = !HasAssignment && (
			IsWide || IsUnicodeAlnum(HeadPoint) || IsPhoneticLetter(HeadPoint) || std::ranges::any_of(
				Term,
				// 名前・数の字を含む語かの照合述語（１字に対し O(1)，例外を送出しない）
				[](const unsigned char Char) -> bool {
					// 識別子の字を含むかの返戻
					return IsIdentifierChar(Char);
				}
			)
		);
		IsAfterComma = false;
		IsPrevNumTerm = IsNumTerm;
	}
	Finish();
	for(CommaScope &Scope : Scopes) if(!Scope.IsAmbiguous) for(const size_t Idx : Scope.Enumeration) {
		Changes[Idx].IsWide = Scope.HasWideTerm || Scope.HasWideContents;
	}
	// 書き換える読点の無い本文の終了
	if(Changes.empty()) return;
	// 原文と保護標識を，読点の幅の変更も含めて１度で転写
	std::string Output;
	std::vector<unsigned char> OutputProtected;
	Output.reserve(Text.size());
	OutputProtected.reserve(Text.size());
	size_t Copied = 0;
	for(const CommaChange &Change : Changes) {
		Output.append(Text, Copied, Change.Pos - Copied);
		OutputProtected.insert(OutputProtected.end(), Protected.begin() + Copied, Protected.begin() + Change.Pos);
		// 読点前と全角読点後の非保護の区切空白の除去
		while(!Output.empty() && (Output.back() == ' ' || Output.back() == '\t') && !OutputProtected.back()) {
			Output.pop_back();
			OutputProtected.pop_back();
		}
		Output += Change.IsWide ? "，" : ",";
		Copied = Change.Pos + Change.Width;
		if(Change.IsWide) while(Copied < Text.size() && (Text[Copied] == ' ' || Text[Copied] == '\t') && !Protected[Copied]) ++Copied;
		else if(Copied < Text.size()) {
			// 半角読点後の語との区切（括弧・区切子とは空白を挟まない）
			size_t After = Copied;
			char32_t Next = 0;
			TextEdit::IsUtf8Decoded(Text, After, Next);
			if(
				Next < 0X80 && !std::isspace(static_cast<unsigned char>(Next)) &&
				std::string_view("()[]{}<>.,:;!?").find(static_cast<char>(Next)) == std::string_view::npos || IsPhoneticLetter(Next) ||
				IsHalfWidthQuantityMark(Next)
			) Output += ' ';
		}
		OutputProtected.resize(Output.size(), 0);
	}
	Output.append(Text, Copied, Text.size() - Copied);
	OutputProtected.insert(OutputProtected.end(), Protected.begin() + Copied, Protected.end());
	Text = std::move(Output);
	Protected = std::move(OutputProtected);
	// 終了
	return;
}

/**
 * コメント本文の空白の正規化関数（規約の半角と全角の境界のスペースの規則を本文へ適用する）
 * 計算量：本文のバイト数 N，タグの開始記号のバイト数 M，登録語長 W に対し O((M + W + 1)(N + 1))
 * @param Forms 行コメントの登録形の判定条件
 * @param Text 対象テキスト（破壊的に書き換えられる）
 * @param TagMarks ドキュメントのタグ・ディレクティブを始める字
 * @param Protected コードの保護標識（書換に合わせて更新される）
 * @param CopiedBytes 本文の転写と補完空白の書込のバイト数の計上先（無ければ計上しない）
 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
 */
void CommentNormalizer::NormalizeSpaces(
	const Core::RegisteredLineForms &Forms,
	std::string &Text,
	const std::string_view TagMarks,
	std::vector<unsigned char> &Protected,
	size_t *const CopiedBytes
) {
	// 括弧・引用符・カンマ・セミコロン以外の半角可視文字を語の続きとして判定
	const auto IsWordChar = [](const unsigned char Char) -> bool {
		// 半角の語の続きの字かの返戻
		return Char >= '!' && Char <= '~' && std::string_view("()[]{}\"'`,;").find(Char) == std::string_view::npos;
	};
	// 位置の字のコードポイント（不正な並びと末尾は 0）
	const auto PointAt = [&Text](size_t At) -> char32_t {
		// 指定位置のコードポイントのデコード
		char32_t Point = 0;
		// デコードしたコードポイントの返戻
		return At < Text.size() && TextEdit::IsUtf8Decoded(Text, At, Point) ? Point : 0;
	};
	// 位置から始まる半角の語が字（英字・表音文字と其の結合文字）を含むか
	const auto HasLetterAt = [&Text, &IsWordChar](size_t Pos) -> bool {
		// 位置から続く半角の語の字の走査
		for(char32_t Point = 0; Pos < Text.size() && TextEdit::IsUtf8Decoded(Text, Pos, Point);) {
			if(Point < 0X80 ? !IsWordChar(static_cast<char>(Point)) : !IsPhoneticLetter(Point)) break;
			// 字を含む語である事の返戻
			if(Point > 0X7F || std::isalpha(static_cast<unsigned char>(Point))) return true;
		}
		// 字を含まない語である事の返戻
		return false;
	};
	std::string Output;
	// 正規化後位置別の保護標識
	std::vector<unsigned char> OutputProtected;
	bool HasChanges = false;
	size_t Copied = 0;
	// 変更が確定する迄は原文の連続範囲を参照し，最初の変更時だけ転写先を作る
	const auto Materialize = [&]() -> void {
		// 転写先を作成済の場合の終了
		if(HasChanges) return;
		Output.reserve(Text.size() + (Text.size() >> 4)); // 空白の補完用に原文の１／１６の余裕
		OutputProtected.reserve(Output.capacity());
		Output.append(Text, 0, Copied);
		OutputProtected.insert(OutputProtected.end(), Protected.begin(), Protected.begin() + Copied);
		if(CopiedBytes) *CopiedBytes += Copied;
		HasChanges = true;
		// 終了
		return;
	};
	// 判定時点で転写済と同じ本文の接頭範囲の参照
	const auto Written = [&]() -> std::string_view {
		// 遅延中は原文，変更後は転写先の同じ範囲の返戻
		return HasChanges ? std::string_view(Output) : std::string_view(Text).substr(0, Copied);
	};
	// 原文範囲と保護標識の同時転写
	const auto Emit = [&](const size_t Begin, const size_t End) -> void {
		if(!HasChanges && Begin == Copied) Copied = End;
		else {
			Materialize();
			Output.append(Text, Begin, End - Begin);
			OutputProtected.insert(OutputProtected.end(), Protected.begin() + Begin, Protected.begin() + End);
			if(CopiedBytes) *CopiedBytes += End - Begin;
		}
		// 終了
		return;
	};
	// 補完済空白の転写
	const auto EmitSpace = [&]() -> void {
		// 空白の追記
		Materialize();
		Output += ' ';
		// 挿入空白の非保護標識追加
		OutputProtected.push_back(0);
		if(CopiedBytes) ++*CopiedBytes; // 補完空白１バイトの書込の計上
		// 終了
		return;
	};
	// 直前に空白を挟まずに書いた本文の字（行頭と空白の後は 0）
	char32_t Last = 0;
	// 直前文字の保護・半角語と，英字を含む語・全角直後の密着語の状態
	bool IsLastProtected = false, IsLastWord = false, HasLetterWord = false, IsAttachedRun = false;
	// 半角語の始点と半角括弧の深さ（語の `>` と記法の `>` を区別）
	size_t WordStart = 0, BracketDepth = 0;
	// 深さ別の半角開括弧前の空白有無
	uint64_t SpacedBrackets = 0;
	// 最初の空白・`/`・`://`・非 ASCII の位置を通過迄再利用（長い無空白行でブロック毎に再探索する２乗費用を回避）
	const auto FindWide = [&Text](const size_t From) -> size_t {
		// 非 ASCII の字の位置（無ければ文の長さ）の返戻
		return static_cast<size_t>(
			std::find_if(
				Text.begin() + static_cast<std::ptrdiff_t>(From),
				Text.end(),
				[](const unsigned char Byte) -> bool {
					// 非 ASCII の字かの返戻
					return Byte > 0X7F;
				}
			) - Text.begin()
		);
	};
	size_t SpaceAt = Text.find_first_of(" \t\n"), SlashAt = Text.find('/'), UrlAt = Text.find("://"), WideAt = FindWide(0);
	// 本文のコードポイント毎の空白と保護範囲の走査
	for(size_t Pos = 0; Pos < Text.size();) {
		const bool IsLineStart = !Pos || Text[Pos - 1] == '\n';
		if(IsLineStart) {
			Last = 0;
			IsLastProtected = IsLastWord = IsAttachedRun = false;
		}
		const unsigned char Char = Text[Pos];
		const size_t Start = Pos;
		if(Char == ' ' && !Protected[Pos]) {
			while(Pos < Text.size() && Text[Pos] == ' ' && !Protected[Pos]) ++Pos;
			const char32_t Next = PointAt(Pos);
			// 規約が空白を許す章番号直後の境界
			const auto IsAfterChapter = [&]() -> bool {
				// 直前の字が「章」又は「節」でない場合の返戻
				if(Last != 0X7AE0 && Last != 0X7BC0) return false;
				const std::string_view Prefix = Written();
				size_t At = Prefix.size() - 3;
				// 半角数字との境界へ既に補ったスペースを透過し，数字の文字種は保つ
				if(At && Prefix[At - 1] == ' ') --At;
				bool HasNumber = false;
				while(At) {
					size_t Begin = At - 1;
					while(Begin && At - Begin < 4 && (static_cast<unsigned char>(Prefix[Begin]) & 0XC0) == 0X80) --Begin;
					size_t End = Begin;
					char32_t Point = 0;
					if(TextEdit::IsUtf8Decoded(Prefix, End, Point) && End == At && GetUnicodeAlnumKind(Point) == UnicodeAlnumKind::Number) {
						At = Begin;
					} else if(HasNumber && Prefix[At - 1] == ' ') {
						--At;
						continue;
					} else break;
					HasNumber = true;
				}
				// 番号の前が「第」かの返戻
				return HasNumber && At > 2 && !Prefix.compare(At - 3, 3, "第");
			};
			// ドキュメント化タグと説明の間の区切（`@return （甲）` の空白を詰めるとタグの本文が失われ，同期が空のタグを足す）
			const auto IsAfterDocTag = [&Written, TagMarks]() -> bool {
				// ドキュメントのタグの終端位置の探索
				const std::string_view Prefix = Written();
				size_t At = Prefix.size();
				// タグ名の後方走査
				while(At && IsIdentifierChar(Prefix[At - 1])) --At;
				// タグの名前とタグ・ディレクティブを始める字を持たない事の返戻
				if(At == Prefix.size() || !At || TagMarks.find(Prefix[At - 1]) == std::string_view::npos) return false;
				--At;
				// タグの前の空白の後方走査
				while(At && (Prefix[At - 1] == ' ' || Prefix[At - 1] == '\t')) --At;
				// 行の初めのタグかの返戻
				return !At || Prefix[At - 1] == '\n';
			};
			// 保護文字周辺は縮約に留め，半角の区切子の前と日本語字・全角記号間の不要な空白を除去
			if(
				const bool IsRemovable =
				Last && Pos < Text.size() && Text[Pos] != '\n' && !IsLastProtected && !Protected[Pos] && Text.compare(Pos, 2, "//") &&
				!(Start > 1 && Text[Start - 1] == '/' && Text[Start - 2] == '/') && Text.compare(Pos, 2, "# ") && !IsSpacedMark(Last) &&
				!IsSpacedMark(Next);
				IsRemovable && !IsAfterDocTag() && !Core::CommentRepository::WouldJoinEncodingDecl(Forms, Text, Start, Pos) && (
					Next == ',' || Next == ':' || Next == ';' || IsFullWidthMark(Last) || IsFullWidthMark(Next) ||
					IsFullWidth(Last) && IsFullWidth(Next) && !IsAfterChapter()
				)
			) continue;
			// 行末を除く連続空白の１文字への縮約
			Emit(Start, !Last || Pos >= Text.size() || Text[Pos] == '\n' ? Pos : Start + 1);
			Last = 0;
			IsLastProtected = IsLastWord = IsAttachedRun = false;
			continue;
		}
		char32_t Point = 0;
		if(!TextEdit::IsUtf8Decoded(Text, Pos, Point)) Pos = Start + 1;
		// 書式の制御は字の判定を前の字の儘に引き継ぐ（`❤️OK` は `❤` と `O` の間）
		if(IsFormatControl(Point)) {
			Emit(Start, Pos);
			continue;
		}
		if(Char == '\n' || Protected[Start]) {
			// 改行と保護した文字の原文転写
			Emit(Start, Pos);
			Last = Char == '\n' ? 0 : Point;
			IsLastProtected = Protected[Start];
			// 保護した半角の点も語の続きとし，点の直後から同じ半角語を再走査しない
			IsLastWord = Char == '.' && IsLastWord;
			IsAttachedRun = false;
			continue;
		}
		// URL・パスを１語として保護し，パスの後の本文だけ分離（終端不明なら全体保持）
		const bool IsWordStart =
		!Last && (!Start || Text[Start - 1] == ' ' || Text[Start - 1] == '\t' || IsLineStart) || Last == '(' || Last == '[' ||
		Last == '{';
		const bool IsFullWidthLast = IsFullWidthWord(Last);
		if(const bool IsAfterWide = !IsLastProtected && IsFullWidthLast && std::isalpha(Char); IsWordStart || IsAfterWide) {
			// 長いパスの再探索を避ける次の空白・斜線・URL マーカーの再利用
			if(SpaceAt < Start) SpaceAt = Text.find_first_of(" \t\n", Start);
			if(SlashAt < Start) SlashAt = Text.find('/', Start);
			if(UrlAt < Start) UrlAt = Text.find("://", Start);
			size_t ChunkEnd = std::min(SpaceAt, Text.size());
			const std::string_view Chunk(Text.data() + Start, ChunkEnd - Start);
			const size_t Slash = SlashAt < ChunkEnd ? SlashAt - Start : std::string_view::npos;
			const bool IsUrl = UrlAt != std::string::npos && UrlAt + 2 < ChunkEnd;
			if(IsWordChar(Char) && Slash != std::string_view::npos && WideAt < Start) WideAt = FindWide(Start);
			if(IsUrl || IsWordChar(Char) && Slash != std::string_view::npos && WideAt >= SlashAt) {
				if(!IsUrl) for(size_t At = Slash + 1; At < Chunk.size(); ++At) if(
					static_cast<unsigned char>(Chunk[At]) > 0XBF && std::isalnum(static_cast<unsigned char>(Chunk[At - 1])) &&
					IsFullWidth(PointAt(Start + At))
				) {
					ChunkEnd = Start + At;
					break;
				}
				if(IsAfterWide) EmitSpace();
				Emit(Start, ChunkEnd);
				Pos = ChunkEnd;
				// パスの末尾の半角の字の後続の境界判定への引渡（全角で終わるパスも半角の語として扱う）
				const unsigned char Tail = static_cast<unsigned char>(Text[ChunkEnd - 1]);
				Last = Tail < 0X80 ? Tail : 'a';
				WordStart = Start;
				IsLastWord = true;
				HasLetterWord = HasLetterAt(Start);
				IsLastProtected = IsAttachedRun = false;
				continue;
			}
		}
		// 半角語・半角括弧と全角字の境界への空白の補完
		const bool IsWord = Point < 0X80 ? IsWordChar(Char) : IsPhoneticLetter(Point) || IsHalfWidthQuantityMark(Point);
		if(IsWord && !IsLastWord) {
			// 新しい半角語の英字保持有無
			HasLetterWord = HasLetterAt(Start) || IsHalfWidthQuantityMark(Point);
			WordStart = Start;
		}
		// 前の語の端（英数字・`C++` の記号，`vector<int>` の様に英数字で始まる語の `>`）
		const bool IsLastWordEdge = IsWordEdgeChar(Last) || Last == '>' && IsAlnumAt(Text, WordStart, false);
		// 英字・数字を含む半角の語と全角の語の境界の空白（数との間へ空白を置かない単位記号の前は除き，全角へ揃えない半角の数字も全角の語との間を区切る）
		const bool ShouldInsert = Last && !IsLastProtected && (
			!IsAttachedRun && IsLastWord &&
			(HasLetterWord || IsHalfWidthQuantityMark(Last) || GetUnicodeAlnumKind(Last) == UnicodeAlnumKind::Number) &&
			IsFullWidthWord(Point) && IsLastWordEdge || IsFullWidthLast && (
				IsWord && (HasLetterWord || GetUnicodeAlnumKind(Point) == UnicodeAlnumKind::Number) &&
				(std::isalnum(Char) || Char == '_' || Point > 0X7F) || Char == '(' || Char == '{' || Char == '['
			) || !IsAttachedRun && (Last == ')' || Last == ']' || Last == '}') && IsFullWidthWord(Point)
		);
		if(ShouldInsert) EmitSpace();
		// 開前の空白に対応する閉後の空白補完
		bool IsSpacedCloser = false;
		// ６４段迄の括弧別の開前空白有無のビット保持
		if(Char == '(' || Char == '[' || Char == '{') {
			if(BracketDepth < std::numeric_limits<uint64_t>::digits) {
				SpacedBrackets =
				ShouldInsert || !Last ? SpacedBrackets | uint64_t{ 1 } << BracketDepth : SpacedBrackets & ~(uint64_t{ 1 } << BracketDepth);
			}
			++BracketDepth;
		} else if((Char == ')' || Char == ']' || Char == '}') && BracketDepth) {
			IsSpacedCloser = --BracketDepth < std::numeric_limits<uint64_t>::digits && SpacedBrackets >> BracketDepth & 1;
		}
		// 全角直後に密着した記号始まりの半角列後の空白抑止
		if(IsFullWidth(Point) || IsSpacedCloser) IsAttachedRun = false;
		else if(!ShouldInsert && IsFullWidthLast) IsAttachedRun = true;
		Emit(Start, Pos);
		Last = Point;
		// 通常文字転写後の保護状態解除
		IsLastProtected = false;
		IsLastWord = IsWord;
	}
	if(!HasChanges && Copied != Text.size()) Materialize();
	if(HasChanges) {
		Text = std::move(Output);
		Protected = std::move(OutputProtected);
	}
	// 終了
	return;
}

/**
 * 記録の数の取得関数
 * @return 見方が持つ記録の数
 */
size_t ReadOnlyCommentView::GetSize() const {
	// 記録の数の返戻
	return Selection.empty() ? Records.size() : Selection.size();
}

/**
 * 所属するグループの取得関数
 * @param Idx 記録の位置
 * @return ドキュメントの位置で確定した所属（単独の記録は `NoCommentGroup`，本文を値として保つディレクティブコメントは無）
 */
std::optional<Core::CommentGroupId> ReadOnlyCommentView::GetGroup(const size_t Idx) const {
	// グループに属さないディレクティブコメントの返戻
	if(Records[Selection.empty() ? Idx : Selection[Idx]].Role == CommentRole::Directive) return std::nullopt;
	// 所属するグループの返戻
	return Records[Selection.empty() ? Idx : Selection[Idx]].Format.Group;
}

/**
 * 同じ本文世代の共有構造の記録位置の取得関数
 * 計算量：O(1)
 * @param Idx 記録の位置
 * @return 未変更の本文が参照する前の構造の位置（本文変更時は無）
 */
std::optional<size_t> ReadOnlyCommentView::GetStructureIndex(const size_t Idx) const {
	// 前の構造の記録位置の返戻
	return Records[Selection.empty() ? Idx : Selection[Idx]].StructureIndex;
}

/**
 * 原文での順の取得関数
 * @param Idx 記録の位置
 * @return 原文での順
 */
size_t ReadOnlyCommentView::GetOrder(const size_t Idx) const {
	// 原文での順の返戻
	return Records[Selection.empty() ? Idx : Selection[Idx]].OriginalOrder;
}

/**
 * 本文の取得関数
 * @param Idx 記録の位置
 * @return 記号を除いた本文
 */
std::string_view ReadOnlyCommentView::GetText(const size_t Idx) const {
	// 本文の返戻
	return Core::CommentRepository::GetOutputTextOf(Records[Selection.empty() ? Idx : Selection[Idx]]);
}

/**
 * コメント内コードの保護範囲取得関数
 * 行頭のインデントと採用したコード記法から，本文中のコード例を判定する（整形仕様 7.3, 7.4）
 * 構造として保つ空の論理行は，コード例の中の行と，段落とコード例を分ける行とする（整形仕様 7.3, 7.7）
 * 計算量：本文のバイト数 N，コード記法の登録テキストの総バイト数 M に対し期待 O((M + 1)(N + 1))
 * @param Forms 文法のコード例の書式
 * @param Text 記号を除いた本文の論理行（改行で繋いだ物）
 * @param IsUnclosed 未閉鎖のコード領域が有るかの出力先
 * @param TickWork バッククォートの検出と対応付で読んだバイト数（無ければ計上しない）
 * @return コード又は其のインデントに属するバイトの保護標識
 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
 */
std::vector<unsigned char> CommentNormalizer::GetCodeMask(
	const Core::CodeExampleForms &Forms,
	const std::string_view Text,
	bool &IsUnclosed,
	size_t *const TickWork
) {
	// 論理行の開始・終端の位置の型
	struct LineInfo {
		size_t Start; // 本文中の行開始位置
		size_t End; // 本文中の行終端位置
	};
	// インデントのコード例の閾値とタブの列幅
	constexpr size_t IndentWidth = 4; // インデントのコード例の開始列
	// 同じ属性範囲の終端を共有し，引用内では対応する閉引用符の後の終端を使う作業表
	std::vector<size_t> TagEnds;
	const auto TagEnd = [&Text, &TagEnds](const size_t Start) -> size_t {
		// 最初のタグ候補でだけ，接尾範囲の終端を後ろから確定
		if(TagEnds.empty()) {
			TagEnds.resize(Text.size() + 1);
			TagEnds.back() = Text.size();
			// 後続の単引用符・二重引用符の位置（引用の種類に対応する順）
			std::array<size_t, 2> QuoteEnds { Text.size(), Text.size() };
			for(size_t Pos = Text.size(); Pos--;) {
				const char Char = Text[Pos];
				if(Char == '\'' || Char == '"') {
					const size_t Kind = Char == '\'' ? 0 : 1, Close = QuoteEnds[Kind];
					TagEnds[Pos] = Close == Text.size() ? Text.size() : TagEnds[Close + 1];
					QuoteEnds[Kind] = Pos;
				} else TagEnds[Pos] = Char == '>' ? Pos : TagEnds[Pos + 1];
			}
		}
		// 引用を開いて居ない開始位置からの終端の返戻
		return TagEnds[Start];
	};
	// 論理行別の位置情報
	std::vector<LineInfo> Lines;
	for(size_t Start = 0; Start <= Text.size();) {
		const size_t Newline = Text.find('\n', Start), End = Newline == std::string::npos ? Text.size() : Newline;
		Lines.push_back({ Start, End });
		if(Newline == std::string::npos) break;
		Start = End + 1;
	}
	// 連結本文位置別のコード標識
	std::vector<unsigned char> Code(Text.size(), 0);
	// 発見済コード範囲の連結本文上の標識への転写
	const auto Protect = [&](const size_t Begin, const size_t End) -> void {
		std::fill(Code.begin() + Begin, Code.begin() + End, 1);
		// 終了
		return;
	};
	// コード領域と段落の走査状態
	size_t FenceLen = 0;
	// 開いて居る囲区切の文字
	unsigned char FenceChar = 0;
	// 段落とインデントコードの中かの状態
	bool IsParagraph = false, IsIndented = false, HasTicks = false;
	// 囲コードとインデントコードの行単位での先行保護
	for(size_t Idx = 0; Idx < Lines.size(); ++Idx) {
		const LineInfo &Line = Lines[Idx];
		if(!HasTicks) {
			const size_t Tick = Text.substr(Line.Start, Line.End - Line.Start).find('`');
			HasTicks = Tick != std::string_view::npos;
			if(TickWork) *TickWork += (HasTicks ? Tick : Line.End - Line.Start) + 1;
		}
		// 行本文先頭とコード例のインデント幅
		size_t First = Line.Start, Columns = 0;
		while(First < Line.End && (Text[First] == ' ' || Text[First] == '\t')) {
			// 行頭のインデント幅の加算
			Columns += Text[First] == '\t' ? IndentWidth - Columns % IndentWidth : 1;
			++First;
		}
		// 空行の該当有無
		const bool IsBlank = First == Line.End;
		// 囲区切の終端候補
		size_t RunEnd = First;
		if(!IsBlank && (Text[First] == '`' || Text[First] == '~')) while(RunEnd < Line.End && Text[RunEnd] == Text[First]) ++RunEnd;
		// 連続する囲文字数
		const size_t Run = RunEnd - First;
		if(FenceLen) {
			// 閉区切を含む行全体の原文保持
			Protect(Line.Start, Line.End);
			// 閉区切の行（区切の字の連続を持つ行だけを読み，空行の字を読まない）
			if(Run >= FenceLen && Columns < IndentWidth && Text[First] == FenceChar && Text.find_first_not_of(" \t", RunEnd) >= Line.End) {
				FenceLen = 0;
			}
			// 囲コード内の段落状態解除
			IsParagraph = false;
			continue;
		}
		if(Forms.HasFencedCode && Columns < IndentWidth && Run > 2 && (Text[First] != '`' || Text.find('`', RunEnd) >= Line.End)) {
			// 囲区切文字の記録
			FenceChar = Text[First];
			// 囲区切長の記録
			FenceLen = Run;
			// 囲コード開始行の全体保護
			Protect(Line.Start, Line.End);
			// 囲コード開始時の段落状態解除
			IsParagraph = false;
			continue;
		}
		if(IsBlank) continue;
		if(Forms.ShouldMarkCodeByIndent ? Columns : Columns > IndentWidth - 1 && (!IsParagraph || IsIndented)) {
			// インデントコード行の全体保護
			Protect(Line.Start, Line.End);
			// インデントコードの状態設定
			IsIndented = true;
			// インデントコード内の段落状態解除
			IsParagraph = false;
		} else {
			// インデントコード状態の解除
			IsIndented = false;
			// 通常段落の開始状態設定
			IsParagraph = true;
		}
	}
	// 閉じない囲みのコード領域の記録
	IsUnclosed = FenceLen;
	// 同長の次区切の一括収集に依る反復探索の回避
	std::unordered_map<size_t, size_t> PrevTicks, NextTicks;
	// 同一段落内の同長バッククォートの対応付
	if(HasTicks) {
		for(size_t Pos = 0; Pos < Text.size();) {
			if(TickWork) ++*TickWork;
			if(Code[Pos]) {
				// コード保護範囲での未対応区切破棄
				PrevTicks.clear();
				++Pos;
			} else if(Text[Pos] == '`') {
				// バッククォート列の終端位置
				size_t End = Pos + 1;
				while(End < Text.size() && Text[End] == '`') {
					if(TickWork) ++*TickWork;
					++End;
				}
				// バッククォート区切の長さ
				const size_t Len = End - Pos;
				if(const std::unordered_map<size_t, size_t>::const_iterator Found = PrevTicks.find(Len); Found != PrevTicks.end()) {
					// 開始区切に対応する閉位置の登録
					NextTicks[Found->second] = Pos;
				}
				// 同じ長さの最新開始位置の記録
				PrevTicks[Len] = Pos;
				// 区切列終端への移動
				Pos = End;
			} else {
				if(Text[Pos] == '\n') {
					// ドキュメント化タグ行を境界とするコードスパンの区分
					if(
						const size_t Next = Pos + 1;
						// 段落境界での未対応区切破棄
						Next + 1 < Text.size() && Forms.TagMarks.find(Text[Next]) != std::string_view::npos &&
						std::isalpha(static_cast<unsigned char>(Text[Next + 1]))
					) PrevTicks.clear();
				}
				++Pos;
			}
		}
	}
	// 登録した開始バイトだけを候補とし，通常の字で数式・タグの文字列を照合しない
	std::array<uint64_t, 4> MathHeads {}, InlineHeads {};
	for(const std::pair<std::string_view, std::string_view> &Delims : Forms.MathDelimiters) {
		if(Delims.first.empty()) MathHeads.fill(std::numeric_limits<uint64_t>::max());
		else {
			const unsigned char Head = static_cast<unsigned char>(Delims.first.front());
			MathHeads[Head >> 6] |= uint64_t{ 1 } << (Head & 63);
		}
	}
	for(const std::string_view Tag : Forms.InlineTags) if(!Tag.empty()) {
		const unsigned char Head = static_cast<unsigned char>(Tag.front());
		InlineHeads[Head >> 6] |= uint64_t{ 1 } << (Head & 63);
	}
	// 区切長の一致するコードスパンとドキュメント用コードタグだけの保護
	// 呼出の括弧の中の引用値を，説明文の引用と区別する開き毎の標識
	std::vector<unsigned char> CallArgs;
	// 説明文の引用の閉位置を入子順に保持し，内部の説明は通常の走査を継続
	std::vector<size_t> ProseQuoteEnds;
	// ブロックの開始・終端コマンドに共通する前置の条件
	const auto CanStartBlockCommand = [&Forms, Text](const size_t Pos) -> bool {
		const std::string_view Prefix = Forms.BlockCommandExcludedPrefix;
		// 登録した別記法の内部でない事の返戻
		return Prefix.empty() || Pos + 1 < Prefix.size() || Text.compare(Pos + 1 - Prefix.size(), Prefix.size(), Prefix);
	};
	for(size_t Pos = 0; Pos < Text.size();) {
		// 保護領域を跨いだ閉位置の破棄と，確定済の閉引用符の読飛
		while(!ProseQuoteEnds.empty() && ProseQuoteEnds.back() < Pos) ProseQuoteEnds.pop_back();
		if(!ProseQuoteEnds.empty() && ProseQuoteEnds.back() == Pos) {
			ProseQuoteEnds.pop_back();
			++Pos;
			continue;
		}
		if(Code[Pos]) {
			++Pos;
			continue;
		}
		const unsigned char Byte = static_cast<unsigned char>(Text[Pos]);
		// 登録した数式の開閉の記法を含む範囲の保護
		if(
			MathHeads[Byte >> 6] >> (Byte & 63) & 1 && std::ranges::any_of(
				Forms.MathDelimiters,
				// 対応する数式の終端迄を共通の保護情報へ登録する述語
				[&](const std::pair<std::string_view, std::string_view> &Delims) -> bool {
					// 登録した数式の開始でない事の返戻
					if(Text.compare(Pos, Delims.first.size(), Delims.first)) return false;
					// 数式の終端位置と保護終端
					const size_t Found = Text.find(Delims.second, Pos + Delims.first.size());
					const size_t End = Found == std::string_view::npos ? Text.size() : Found + Delims.second.size();
					if(Found == std::string_view::npos) IsUnclosed = true;
					Protect(Pos, End);
					Pos = End;
					// 数式を保護した事の返戻
					return true;
				}
			)
		) continue;
		if(Text[Pos] == '(') {
			CallArgs.push_back(!CallArgs.empty() && CallArgs.back() || IsAlnumAt(Text, Pos, true) || Pos && Text[Pos - 1] == '_');
		} else if(Text[Pos] == ')' && !CallArgs.empty()) CallArgs.pop_back();
		// ドキュメント生成系コード引用の保護範囲への追加
		// ドキュメント用コードタグの開始長
		size_t InlineTag = 0;
		if(InlineHeads[Byte >> 6] >> (Byte & 63) & 1) for(const std::string_view Tag : Forms.InlineTags) {
			if(Tag.empty() || Text.compare(Pos, Tag.size(), Tag)) continue;
			InlineTag = Tag.size();
			break;
		}
		if(
			InlineTag && Pos + InlineTag < Text.size() &&
			(std::isspace(static_cast<unsigned char>(Text[Pos + InlineTag])) || Text[Pos + InlineTag] == '}')
		) {
			// ドキュメント用コードタグの走査位置と波括弧深さ
			size_t End = Pos + 1, Depth = 1;
			while(End < Text.size() && Depth) {
				if(Text[End] == '{') ++Depth;
				else if(Text[End] == '}') --Depth;
				++End;
			}
			if(Depth) IsUnclosed = true;
			// ドキュメント用コードタグ範囲の保護
			Protect(Pos, End);
			// タグ終端への移動
			Pos = End;
			continue;
		}
		if(Forms.TagMarks.find(Text[Pos]) != std::string_view::npos) {
			// Doxygen の開始ディレクティブから対応終了ディレクティブ迄のコードブロック化
			if(
				CanStartBlockCommand(Pos) && std::ranges::any_of(
					Forms.BlockCommands,
					// 対応する開始ディレクティブのコード例を保護する述語
					[&](const std::string_view Name) -> bool {
						// 開始ディレクティブ名の直後位置
						const size_t After = Pos + Name.size() + 1;
						// 登録した開始ディレクティブの条件に合わない事の返戻
						if(Text.compare(Pos + 1, Name.size(), Name) || After < Text.size() && IsIdentifierChar(Text[After])) return false;
						// 対応する終了ディレクティブ
						const std::string Close = std::string(1, Text[Pos]).append(Forms.EndCommandPrefix).append(Name);
						// 終了ディレクティブ名の接頭部だけの一致を除く終端探索
						size_t Found = Text.find(Close, After);
						while(
							Found != std::string::npos &&
							(!CanStartBlockCommand(Found) || Found + Close.size() < Text.size() && IsIdentifierChar(Text[Found + Close.size()]))
						) Found = Text.find(Close, Found + Close.size());
						// 保護範囲の終端
						const size_t End = Found == std::string::npos ? Text.size() : Found + Close.size();
						if(Found == std::string::npos) IsUnclosed = true;
						// Doxygen コードディレクティブ範囲の保護
						Protect(Pos, End);
						// 終了ディレクティブ直後への移動
						Pos = End;
						// 開始ディレクティブを保護した事の返戻
						return true;
					}
				)
			) continue;
			// フレームワークが実行時に読むアノテーション引数の括弧範囲の原文保持
			size_t NameEnd = Pos + 1;
			while(NameEnd < Text.size() && (IsIdentifierChar(Text[NameEnd]) || Text[NameEnd] == '\\')) ++NameEnd;
			if(Text[Pos] == Forms.AnnotationMark && NameEnd > Pos + 1 && NameEnd < Text.size() && Text[NameEnd] == '(') {
				// アノテーション引数の走査位置と括弧深さ
				size_t End = NameEnd + 1, Depth = 1;
				for(unsigned char Quote = 0; End < Text.size() && Depth; ++End) {
					if(Quote) {
						// 引用内のエスケープした字の読飛しと終端の判定
						if(const size_t Width = GetEscapeWidth(Text, End, true)) End += Width - 1;
						else if(Text[End] == Quote) Quote = 0;
					} else switch(Text[End]) {
					case '"':
					case '\'':
						Quote = Text[End];
						break;
					case '(':
						++Depth;
						break;
					case ')':
						--Depth;
						break;
					}
				}
				// アノテーション引数範囲の保護
				Protect(Pos, End);
				// アノテーション引数終端への移動
				Pos = End;
				continue;
			}
		}
		// 属性値・代入値・呼出の実引数は改行を跨いでも保持し，其の他の引用値は同じ行内で保護
		if(Text[Pos] == '"' || Text[Pos] == '\'') {
			size_t Before = Pos;
			while(Before && std::isspace(static_cast<unsigned char>(Text[Before - 1]))) --Before;
			const bool IsValue =
			Before && Text[Before - 1] == '=' && (Before == 1 || Text[Before - 2] != '=') || !CallArgs.empty() && CallArgs.back();
			// 英数字の後のアポストロフィを，外部の値と区別
			if(IsValue || Text[Pos] == '"' || !IsAlnumAt(Text, Pos, true)) {
				const size_t Close =
				GetQuoteEnd(IsValue || ProseQuoteEnds.empty() ? Text : Text.substr(0, ProseQuoteEnds.back()), Pos, IsValue);
				const bool IsClosed =
				Close < Text.size() && Text[Close] == Text[Pos] && (Text[Pos] == '"' || !IsAlnumAt(Text, Close + 1, false));
				// 説明文の引用と単引用符の後に続く識別子の字を，保護する引用値と区別
				if(IsValue || IsClosed && !IsQuotedProse(Text.substr(Pos + 1, Close - Pos - 1))) {
					// 未閉鎖の外部の値は本文の終端迄，閉じた値は閉引用符迄の保護
					const size_t End = Close < Text.size() ? Close + 1 : Close;
					Protect(Pos, End);
					Pos = End;
					continue;
				}
				if(IsClosed) ProseQuoteEnds.push_back(Close);
			}
		}
		if(const size_t Width = GetEscapeWidth(Text, Pos, true)) {
			Pos += Width;
			continue;
		}
		if(Text[Pos] == '`') {
			// 対応する閉マーカーを持つコードスパンだけの保護
			size_t End = Pos + 1;
			while(End < Text.size() && Text[End] == '`') ++End;
			if(const std::unordered_map<size_t, size_t>::const_iterator Next = NextTicks.find(Pos); Next != NextTicks.end()) {
				// コードスパン区切の長さを加えた閉区切直後への拡張
				End = Next->second + (End - Pos);
				// 対応するコードスパンの保護
				Protect(Pos, End);
			}
			// コードスパン終端への移動
			Pos = End;
			continue;
		}
		// 内容をテキストの儘読むドキュメントの要素の検出有無
		bool IsTagged = false;
		// 内容をテキストの儘読むドキュメントの要素の同名ネスト数に依る閉位置の決定
		if(Text[Pos] == '<') for(const std::string_view Tag : Forms.CodeElements) {
			if(Text.compare(Pos + 1, Tag.size(), Tag)) continue;
			// 開始タグ名の終端位置
			const size_t NameEnd = Pos + Tag.size() + 1;
			if(NameEnd >= Text.size() || Text[NameEnd] != '>' && Text[NameEnd] != ' ' && Text[NameEnd] != '\t' && Text[NameEnd] != '\n') {
				continue;
			}
			// 開始タグの終端位置
			const size_t OpenEnd = TagEnd(NameEnd);
			if(OpenEnd < Text.size() && Text[OpenEnd - 1] == '/') continue;
			// コード要素の保護終端と同名タグ深さ
			size_t End = Text.size(), Depth = 1;
			for(size_t Scan = OpenEnd < Text.size() ? OpenEnd + 1 : Text.size(); Scan < Text.size();) {
				Scan = Text.find('<', Scan);
				if(Scan == std::string::npos) break;
				if(
					const std::vector<std::pair<std::string_view, std::string_view>>::const_iterator Section = std::ranges::find_if(
						Forms.MarkupSections,
						[&Text, Scan](const std::pair<std::string_view, std::string_view> &Markup) -> bool {
							// 領域の開始かの返戻
							return !Text.compare(Scan, Markup.first.size(), Markup.first);
						}
					); Section != Forms.MarkupSections.end()
				) {
					// 要素の対応付で読み飛ばす領域の閉位置
					const size_t Close = Text.find(Section->second, Scan + Section->first.size());
					Scan = Close == std::string::npos ? Text.size() : Close + Section->second.size();
					continue;
				}
				// タグ候補の終端位置
				const size_t Close = TagEnd(Scan + 1);
				if(Close == Text.size()) break;
				// 終了タグの該当有無
				const bool IsClosing = Text[Scan + 1] == '/';
				if(
					const size_t NameStart = Scan + (IsClosing ? 2 : 1), AfterName = NameStart + Tag.size();
					!Text.compare(NameStart, Tag.size(), Tag) && AfterName <= Close &&
					(AfterName == Close || Text[AfterName] == ' ' || Text[AfterName] == '\t' || Text[AfterName] == '\n')
				) {
					if(IsClosing) --Depth;
					else if(Text[Close - 1] != '/') ++Depth;
				}
				Scan = Close + 1;
				if(!Depth) {
					// 対応する終了タグ直後の記録
					End = Scan;
					break;
				}
			}
			if(Depth) IsUnclosed = true;
			// HTML コード要素範囲の保護
			Protect(Pos, End);
			// コード要素終端への移動
			Pos = End;
			// HTML コード要素の検出状態設定
			IsTagged = true;
			break;
		}
		if(!IsTagged) ++Pos;
	}
	// 保護標識の返戻
	return Code;
}

/**
 * 記法の定義のコード例の書式の取得関数
 * @param Idx 記録の位置
 * @return 記録が共有する文法のコード例の書式
 * @throws FormatLimitExceeded 読む記録に記法の定義が無い場合
 */
const Core::CodeExampleForms &ReadOnlyCommentView::GetCodeExamples(const size_t Idx) const {
	// 記法の定義のコード例の書式の返戻
	return Core::CommentRepository::GetRulesOf(Records[Selection.empty() ? Idx : Selection[Idx]]).CodeExamples;
}

/**
 * 記録の本文のコード例の保護標識の取得関数
 * @param Idx 記録の位置
 * @return 本文の位置別の保護標識（グループに属さない記録は保護しない）
 */
std::span<const unsigned char> CommentStructure::GetCodeMask(const size_t Idx) const {
	// 本文の保護マスクの返戻
	return std::span<const unsigned char>(Records[Idx].Data->Code).subspan(Records[Idx].CodeOffset, Records[Idx].CodeSize);
}

/**
 * コメント本文の正規化関数（読点の文字種の統一・文毎の論理行への分割と句点の除去・空白の規約への正規化・行末へ積んだ記号の前置区切の統一・行末の空白の除去）
 * 処理系が読む注記は投影の段が役割を分け，此処へは通常コメントの本文だけが来る（整形仕様 3.1, 7.3）
 * 計算量：本文のバイト数 N，タグの開始記号のバイト数 M に対し O((M + 1)(N + 1))
 * @param Rules 投影済のドキュメント記法と名前・型の書式
 * @param Text 記号を除いた本文の論理行（破壊的に書き換えられる）
 * @param Boundaries グループ全体で確定し，此の記録の本文位置へ対応付けた文境界
 * @param StackMarker 行末へ積んだ次の文の始まりのテキスト（行記法の記号と空白，囲みは空）
 * @param SymbolHead 最初の論理行が始まる記号の一部の頭（行記法で最初の論理行から分けた行も継ぐ，無ければ空）
 * @param IsLineEnd 行末コメントの本文か（積んだ記号の前置区切を揃える）
 * @param Facts 同じ本文世代で確認済の UTF-8 と読点候補
 * @param Protected コードの保護標識
 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
 */
void CommentNormalizer::NormalizeBody(
	const Core::NotationRules &Rules,
	std::string &Text,
	const std::span<const SentenceBoundary> Boundaries,
	const std::string_view StackMarker,
	const std::string_view SymbolHead,
	const bool IsLineEnd,
	const BodyFacts &Facts,
	std::vector<unsigned char> Protected
) {
	// UTF-8 を確認出来ない本文は３バイトのウィンドウの誤検出を避け，記号の正規化を省く
	if(Facts.IsValidUtf8) {
		// 文境界を先に確定してからの文毎の論理行への分割と句点の除去（整形仕様 7.3）
		SplitSentences(Text, Boundaries, StackMarker, SymbolHead, Protected);
		// 文毎の列挙の範囲と文字種に従う読点の正規化
		if(Facts.HasCommas) NormalizeCommas(Rules, Text, Protected);
		// 分割後の行で空白を調整し，パスの判定を次の整形の行頭と統一
		NormalizeSpaces(Rules.OutputForms.Conditions, Text, Rules.CodeExamples.TagMarks, Protected);
	}
	FinishBody(Text, Protected, StackMarker, IsLineEnd);
	// 終了
	return;
}

/**
 * 論理行の端と行末へ積む記号の前置区切の正規化関数
 * 独立・文中の本文は同じバッファで詰め，行末へ積む本文だけ転写先を持つ
 * 計算量：本文長 N と積む記号長 M に対し O((M + 1)(N + 1))
 * @param Text 正規化中の本文
 * @param Protected 同じ本文世代の保護標識（詰め直す前の位置）
 * @param StackMarker 行末へ積む記号
 * @param IsLineEnd 行末コメントか
 * @param Work 行の読取・移動のバイト数（無ければ計上しない）
 * @throws std::bad_alloc 行末へ積む転写先を確保出来ない場合
 * @throws std::length_error 転写先の長さが上限を超える場合
 */
void CommentNormalizer::FinishBody(
	std::string &Text,
	const std::span<const unsigned char> Protected,
	const std::string_view StackMarker,
	const bool IsLineEnd,
	size_t *const Work
) {
	// コードの範囲の外の行末の空白と，行の終わりに読まない単独の CR の除去（CR はコードの範囲でも除き，出力の改行と CRLF を成さない，生成コードと同じ扱い）
	std::string Output;
	const bool ShouldStack = IsLineEnd && !StackMarker.empty();
	if(ShouldStack) Output.reserve(Text.size());
	size_t Write = 0;
	for(size_t LineStart = 0; LineStart <= Text.size();) {
		const size_t LineEnd = std::min(Text.find('\n', LineStart), Text.size());
		if(Work) *Work += LineEnd - LineStart + 1;
		size_t Kept = LineEnd;
		while(Kept > LineStart) {
			if(Work) ++*Work;
			if(Text[Kept - 1] == '\r') --Kept;
			else {
				const size_t Width = GetSpaceWidthBefore(Text, Kept);
				if(!Width || Protected[Kept - 1]) break;
				if(Work) *Work += Width - 1;
				Kept -= Width;
			}
		}
		size_t BodyStart = LineStart, ContentStart = LineStart;
		while(ContentStart < Kept) {
			if(Work) ++*Work;
			const size_t Width = GetSpaceWidth(Text, ContentStart);
			if(!Width) break;
			if(Work) *Work += Width - 1;
			if(BodyStart == ContentStart && !Protected[ContentStart]) BodyStart += Width;
			ContentStart += Width;
		}
		if(ContentStart == Kept) {
			LineStart = LineEnd + 1;
			continue;
		}
		if(ShouldStack) {
			if(!Output.empty()) Output += '\n';
		} else if(Write) Text[Write++] = '\n';
		// 行末へ積んだ記号の前置区切は，タブが在れば其の数のタブだけ，無ければ半角スペース１つ（行頭と空白を挟まない物を除く，整形仕様 7.1，7.6）
		if(!ShouldStack) {
			// 読取位置より前だけへ移し，本文と制御文字を行単位で其の儘保つ
			if(Write != BodyStart && Kept != BodyStart) {
				std::char_traits<char>::move(Text.data() + Write, Text.data() + BodyStart, Kept - BodyStart);
				if(Work) *Work += Kept - BodyStart;
			}
			Write += Kept - BodyStart;
		} else for(size_t At = BodyStart; At < Kept;) {
			size_t End = At, Tabs = 0;
			while(End < Kept && !Protected[End]) {
				if(Work) ++*Work;
				const size_t Width = GetSpaceWidth(Text, End);
				if(!Width) break;
				if(Work) *Work += Width - 1;
				if(Text[End] == '\t') ++Tabs;
				End += Width;
			}
			if(At > LineStart && End > At && End < Kept && !Protected[End] && !Text.compare(End, StackMarker.size(), StackMarker)) {
				Output.append(Tabs ? Tabs : 1, Tabs ? '\t' : ' ');
			} else Output.append(Text, At, std::max(End, At + 1) - At);
			if(Work) *Work += std::max(End, At + 1) - At;
			At = std::max(End, At + 1);
		}
		if(LineEnd == Text.size()) break;
		LineStart = LineEnd + 1;
	}
	if(ShouldStack) Text = std::move(Output);
	else Text.resize(Write);
	// 終了
	return;
}

/**
 * グループ毎の記録の位置の取得関数
 * @return 空でないグループ毎の記録の位置（原文の順）
 */
std::span<const std::vector<size_t>> CommentStructure::GetGroups() const {
	// グループ毎の記録の位置の返戻
	return Groups;
}

/**
 * 行末へ積んだ次の文の始まりのテキストの取得関数
 * @param Notation 記法
 * @return 行記法の記号と空白（行毎の記号を持たない囲み・埋込ドキュメントは空）
 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
 */
std::string CommentNormalizer::GetStackMarkerOf(const CommentNotation Notation) {
	// 終端記号を持ち行毎の記号を持たない記法の返戻
	if(!Core::CommentRepository::GetCloserOf(Notation).empty()) return {};
	// 行記法の記号と空白の返戻
	return std::string(Core::CommentRepository::GetOpenerOf(Notation)) + ' ';
}

/**
 * コメントの間のテキストが空白だけかの判定関数
 * 計算量：間隙のバイト数 N に対し O(N + 1)
 * @param Gap 原文でのコメント間の文字列
 * @param CanContainBlankLines 間の空行を越えるか（出力で空行を除いて並べる同じアンカーの同じ側のコメント）
 * @return 空白と改行（空行を越えない時は改行１つ）だけなら true
 */
bool CommentNormalizer::IsCommentGap(const std::string_view Gap, const bool CanContainBlankLines) {
	// 空行を越える場合は空白だけかを判定
	if(CanContainBlankLines) return Gap.find_first_not_of(" \t\r\n") == std::string::npos;
	// 空白の確認と改行数を同じ走査で確定
	size_t LineBreaks = 0;
	for(const char Char : Gap) {
		if(Char == '\n') {
			// 空行を含む間隙の返戻
			if(++LineBreaks > 1) return false;
		// 空白以外を含む間隙の返戻
		} else if(Char != ' ' && Char != '\t' && Char != '\r') return false;
	}
	// 空行を含まないコメントの間隙かの返戻
	return LineBreaks == 1;
}

/**
 * 本文を変更せずに独立した後続文の開始位置を得る関数
 * 括弧・クォート内はインラインタグも含めて分けない
 * 計算量：文境界の収集費用 Q と本文長 N に対し O(Q + N)
 * @param Text 記号を除いた本文
 * @param Protected 同じ本文世代の共有構造の保護標識
 * @return 括弧・引用・インラインタグを継がない後続文の開始位置
 * @throws std::bad_alloc 文境界と位置のメモリを確保出来ない場合
 * @throws std::length_error 文境界又は位置の要素数が格納先の上限を超える場合
 */
std::vector<size_t> CommentNormalizer::GetSentenceStarts(
	const std::string_view Text,
	const std::span<const unsigned char> Protected
) {
	std::vector<size_t> Starts;
	for(const SentenceBoundary &Boundary : FindSentenceBoundaries(Text, Protected, true)) {
		if(!Boundary.IsSplit) continue;
		const size_t Pos = Boundary.Next;
		if(Pos < Boundary.LineEnd) Starts.push_back(Pos);
	}
	// 後続文の開始位置の返戻
	return Starts;
}

/**
 * 最初の論理行の取得関数（行末コメントの前置区切を記録と対応させるキー）
 * 改行又は空白の後に積んだ次の文の記号の前迄を，後の空白を除いて返す
 * 計算量：最初の論理行のバイト数 N，積んだ記号のバイト数 M に対し O((M + 1)(N + 1))
 * @param Text 記号を除いた本文
 * @param StackMarker 行末へ積んだ次の文の始まりのテキスト（行記法の記号と空白，囲みは空）
 * @param Protected 本文のコード例の保護標識（未指定なら保護無）
 * @return 最初の論理行
 */
std::string_view CommentNormalizer::GetFirstLogicalLine(
	std::string_view Text,
	const std::string_view StackMarker,
	const std::span<const unsigned char> Protected
) {
	// 論理行の終端迄だけを読み，続く行の本文を重ねて走査しない
	size_t End = 0;
	while(End < Text.size() && Text[End] != '\n') {
		if(
			!StackMarker.empty() && (Protected.empty() || !Protected[End]) && Text.substr(End).starts_with(StackMarker) &&
			GetSpaceWidthBefore(Text, End)
		) break;
		++End;
	}
	Text = Text.substr(0, End);
	while(const size_t Width = GetSpaceWidthBefore(Text, Text.size())) Text.remove_suffix(Width);
	// 最初の論理行の返戻
	return Text;
}

/**
 * 記録のグループを所属とするコンストラクタ
 * @param Src 判定する記録の並び（見方の存続中は書き換えない）
 */
ReadOnlyCommentView::ReadOnlyCommentView(const std::span<const CommentRecord> Src) : Records(Src) {}

/**
 * グループの記録だけを読むコンストラクタ
 * @param Src 所有する記録（見方の存続中は書き換えない）
 * @param Indices 選んだ記録の位置（見方の存続中は生存する）
 */
ReadOnlyCommentView::ReadOnlyCommentView(
	const std::span<const CommentRecord> Src,
	const std::vector<size_t> &Indices
) : Records(Src), Selection(Indices) {}

/**
 * グループ毎の記録の位置の取得関数
 * 見方の所属（ドキュメントの位置が確定した後のグループ，グループを持たない記録は単独）毎に，ディレクティブコメントを除く記録を原文の順に並べる（同期で順の揃った記録は記録の並びの順）
 * 計算量：記録の数 C に対し O(C log C)
 * @param View 記録の読取専用の見方
 * @param Work 所属を照会した記録数（無ければ計上しない）
 * @return 空でないグループ毎の記録の位置
 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
 */
std::vector<std::vector<size_t>> CommentNormalizer::GetGroupsOf(const ReadOnlyCommentView &View, size_t *const Work) {
	// 所属と原文順は記録毎に１度読み，比較では確定したキーを使う
	struct Position {
		size_t Idx; // 見方の中の記録位置
		Core::CommentGroupId Group; // グループ
		size_t Order; // 原文での順
	};
	std::vector<Position> Order;
	Order.reserve(View.GetSize());
	for(size_t Idx = 0; Idx < View.GetSize(); ++Idx) {
		if(Work) ++*Work;
		if(const std::optional<Core::CommentGroupId> Group = View.GetGroup(Idx)) Order.push_back({ Idx, *Group, View.GetOrder(Idx) });
	}
	std::stable_sort(
		Order.begin(),
		Order.end(),
		[](const Position &Left, const Position &Right) -> bool {
			// グループと原文の順の比較の返戻
			return std::pair(Left.Group, Left.Order) < std::pair(Right.Group, Right.Order);
		}
	);
	// 同じグループの範囲毎のグループ
	std::vector<std::vector<size_t>> Groups;
	for(size_t Begin = 0; Begin < Order.size();) {
		const Core::CommentGroupId Group = Order[Begin].Group;
		size_t End = Begin + 1;
		if(Group != Core::NoCommentGroup) while(End < Order.size() && Order[End].Group == Group) ++End;
		std::vector<size_t> &Indices = Groups.emplace_back();
		Indices.reserve(End - Begin);
		for(size_t Pos = Begin; Pos < End; ++Pos) Indices.push_back(Order[Pos].Idx);
		Begin = End;
	}
	// 原文順の記録を纏めたグループの返戻
	return Groups;
}

/**
 * 本文構造の判定関数（整形仕様 3.3, 7.3, 7.4, 7.7）
 * 投影済の原文記録をドキュメント位置の確定後に所属（`CommentFormat::Group` 又は見方が与える所属，整形仕様 3.3，3.4）でグループ分けし，本文を改行で繋ぐ
 * コード例の範囲を判定する
 * 見方の記録を読むだけで書き換えず，同じ記法・本文・順序・所属には同じ結果を返す（投影・同期・配置・静的解析・正規化が共有する）
 * 計算量：記録の数 C，本文の長さの和 L，各グループのコード構造の判定費用の和 Q に対し O(C log(C + 1) + L + Q)
 * @param View 記録の読取専用の見方
 * @param Previous 共有する前の本文世代（無ければ初回）
 * @return 変更不能な判定結果
 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
 */
CommentStructure CommentNormalizer::Inspect(const ReadOnlyCommentView &View, const CommentStructure *Previous) {
	// グループの所属と，前の構造を参照する記録の対応
	CommentStructure Structure;
	Structure.Cache = Previous ? Previous->Cache : nullptr;
	Structure.Groups = GetGroupsOf(View);
	Structure.Records.resize(View.GetSize());
	Structure.RecordGroups.resize(View.GetSize(), Structure.Groups.size());
	for(size_t GroupIdx = 0; GroupIdx < Structure.Groups.size(); ++GroupIdx) {
		const std::vector<size_t> &Group = Structure.Groups[GroupIdx];
		for(const size_t Idx : Group) Structure.RecordGroups[Idx] = GroupIdx;
		const std::optional<size_t> First = View.GetStructureIndex(Group.front());
		// 記録の並びとコード例の書式が同じ未変更のグループの共有
		bool CanShare = Previous && First && Previous->RecordGroups[*First] < Previous->Groups.size();
		if(CanShare) {
			const std::vector<size_t> &Prior = Previous->Groups[Previous->RecordGroups[*First]];
			CanShare = Prior.size() == Group.size() && Previous->Records[*First].Data->Forms == &View.GetCodeExamples(Group.front());
			for(size_t Pos = 0; CanShare && Pos < Group.size(); ++Pos) CanShare = View.GetStructureIndex(Group[Pos]) == Prior[Pos];
			if(CanShare) for(size_t Pos = 0; Pos < Group.size(); ++Pos) Structure.Records[Group[Pos]] = Previous->Records[Prior[Pos]];
		}
		if(CanShare) continue;
		// 変更されたグループだけを連結（各記録の最後にも改行を置く）
		std::string Joined;
		size_t Size = 0;
		for(const size_t Idx : Group) Size += View.GetText(Idx).size() + 1;
		Joined.reserve(Size);
		for(const size_t Idx : Group) {
			const std::string_view Text = View.GetText(Idx);
			CommentStructure::RecordFacts &Record = Structure.Records[Idx];
			Record.CodeOffset = Joined.size();
			Record.CodeSize = Text.size();
			Joined.append(Text).push_back('\n');
		}
		// 連結本文の所有をキーへ移し，判定に成功した変更不能な結果だけをキャッシュへ登録
		std::pair<std::string, const Core::CodeExampleForms *> Key { std::move(Joined), &View.GetCodeExamples(Group.front()) };
		if(!Structure.Cache) Structure.Cache = std::make_shared<CommentStructure::GroupCache>();
		CommentStructure::GroupCache::iterator Found = Structure.Cache->find(Key);
		if(Found == Structure.Cache->end()) {
			std::shared_ptr<CommentStructure::GroupFacts> Facts = std::make_shared<CommentStructure::GroupFacts>();
			Facts->Forms = Key.second;
			std::optional<LexemeReading> Reading;
			const CommentRecord &Source = View.Records[View.Selection.empty() ? Group.front() : View.Selection[Group.front()]];
			if(Core::CommentRepository::GetRulesOf(Source).ShouldDecodeUnicodeEscapes && Key.first.find("\\u") != std::string::npos) {
				Reading = ParseAdapter::ReadUnicodeEscapes(Key.first);
				if(Reading && Reading->Text == Key.first) Reading.reset();
			}
			if(!Reading) Facts->Code = GetCodeMask(*Facts->Forms, Key.first, Facts->IsUnclosed);
			else {
				Facts->Reading = std::make_shared<const LexemeReading>(std::move(*Reading));
				Facts->ReadingCode = GetCodeMask(*Facts->Forms, Facts->Reading->Text, Facts->IsUnclosed);
				Facts->Code.resize(Key.first.size(), 0);
				// 展開後のコード例の範囲を元へ移し，エスケープ其の物も出力用の綴りとして保つ
				for(size_t Pos = 0; Pos < Facts->Reading->Text.size(); ++Pos) {
					const size_t Begin = Facts->Reading->OriginalOffsets[Pos], End = Facts->Reading->OriginalOffsets[Pos + 1];
					const unsigned char Mask = End == Begin + 1 && Key.first[Begin] == Facts->Reading->Text[Pos] ?
					Facts->ReadingCode[Pos] :
					static_cast<unsigned char>(2 | Facts->ReadingCode[Pos]);
					std::fill(
						Facts->Code.begin() + static_cast<std::ptrdiff_t>(Begin),
						Facts->Code.begin() + static_cast<std::ptrdiff_t>(End),
						Mask
					);
				}
			}
			Found = Structure.Cache->emplace(std::move(Key), std::move(Facts)).first;
		}
		for(const size_t Idx : Group) Structure.Records[Idx].Data = Found->second;
	}
	// グループに属さないディレクティブコメントの非保護マスク
	for(size_t Idx = 0; Idx < View.GetSize(); ++Idx) {
		if(Structure.RecordGroups[Idx] < Structure.Groups.size()) continue;
		if(
			const std::optional<size_t> Prior = View.GetStructureIndex(Idx);
			Previous && Prior && Previous->RecordGroups[*Prior] == Previous->Groups.size()
		) {
			Structure.Records[Idx] = Previous->Records[*Prior];
			continue;
		}
		const std::string_view Text = View.GetText(Idx);
		std::shared_ptr<CommentStructure::GroupFacts> Facts = std::make_shared<CommentStructure::GroupFacts>();
		Facts->Code.resize(Text.size(), 0);
		Structure.Records[Idx] = { Facts, 0, Facts->Code.size() };
	}
	// 判定結果の返戻
	return Structure;
}

/**
 * 記録のグループが閉じないコード領域を持つかの判定関数
 * @param Idx 記録の位置
 * @return 閉じないコード領域を持つグループの記録なら true
 */
bool CommentStructure::HasUnclosedCode(const size_t Idx) const {
	// 閉じないコード領域の有無の返戻
	return Records[Idx].Data->IsUnclosed;
}

/**
 * 記録の展開後の本文と元の位置の取得関数
 * 計算量：グループの本文長 B に対し O(log(B + 1))
 * @param Idx 記録の位置
 * @return 展開後の本文と位置（展開が無ければ無い）
 */
std::optional<CommentStructure::ReadBody> CommentStructure::GetReadBody(const size_t Idx) const {
	const RecordFacts &Record = Records[Idx];
	// 展開の無い本文の返戻
	if(!Record.Data->Reading) return std::nullopt;
	const LexemeReading &Reading = *Record.Data->Reading;
	const size_t Begin = std::lower_bound(Reading.OriginalOffsets.begin(), Reading.OriginalOffsets.end(), Record.CodeOffset) -
	Reading.OriginalOffsets.begin();
	const size_t End =
	std::lower_bound(Reading.OriginalOffsets.begin(), Reading.OriginalOffsets.end(), Record.CodeOffset + Record.CodeSize) -
	Reading.OriginalOffsets.begin();
	// 同じ記録の読取本文と元の境界の返戻
	return ReadBody{
		std::string_view(Reading.Text).substr(Begin, End - Begin),
		std::span<const size_t>(Reading.OriginalOffsets).subspan(Begin, End - Begin + 1),
		std::span<const unsigned char>(Record.Data->ReadingCode).subspan(Begin, End - Begin),
		Record.CodeOffset
	};
}

/**
 * 通常コメント本文の表記の正規化関数
 * 所属確定後の構造を共有し，本文が変わったグループだけを更新する
 * ドキュメントの枠の条件は，所属確定で保存した条件と本文の分割後の行数で決める
 * 計算量：記録数 C，本文長 B，構造判定・本文正規化・枠の合成の総費用 Q に対し O(C log(C + 1) + B + Q)
 * @param Comments 所属と出力記法を確定したコメント
 * @throws FormatLimitExceeded 記法の定義が無い場合
 * @throws std::bad_alloc 本文と構造のメモリを確保出来ない場合
 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
 */
void CommentNormalizer::Normalize(Core::CommentRepository &Comments) {
	// 通常コメントの記録と本文構造の取得
	const std::span<CommentRecord> Records = Comments.Records;
	// 記法に依り変わる本文だけを所有記録へ移し，全文のコピーを持たない
	for(CommentRecord &Record : Records) if(Record.Output && Record.Output->Text) {
		Record.Text = std::move(*Record.Output->Text);
		Record.Output->Text.reset();
	}
	const CommentStructure &Structure = Comments.GetOutputStructure();
	std::optional<CommentStructure> Next;
	for(const std::vector<size_t> &Group : Structure.GetGroups()) {
		// 本文の事実と，グループ全体での位置
		struct BodyRange {
			size_t Offset; // 連結本文内の本文頭
			BodyFacts Facts; // 記号の正規化と読点候補の事実
		};
		bool HasContent = false;
		std::vector<BodyRange> Bodies;
		Bodies.reserve(Group.size());
		std::string Joined;
		std::vector<unsigned char> JoinedProtected;
		for(const size_t Idx : Group) {
			const CommentRecord &Record = Records[Idx];
			const std::string_view Text = Record.Text;
			const BodyFacts Facts = GetBodyFacts(Text);
			Bodies.push_back({ Joined.size(), Facts });
			if(Group.size() == 1) continue;
			const std::span<const unsigned char> Protected = Structure.GetCodeMask(Idx);
			Joined.append(Text).push_back('\n');
			JoinedProtected.insert(JoinedProtected.end(), Protected.begin(), Protected.end());
			JoinedProtected.push_back(0);
			// 不正な UTF-8 の記録は既存の転写だけを行い，囲みの判定にも参加させない
			if(!Facts.IsValidUtf8) for(size_t Pos = Bodies.back().Offset; Pos + 1 < Joined.size(); ++Pos) {
				if(Joined[Pos] != '\n') Joined[Pos] = ' ';
				JoinedProtected[Pos] = 1;
			}
		}
		// 囲みの状態は記録境界を越えて共有し，元の改行と記録の区切は残す
		// 展開後の役割で文境界を確定し，出力の変更範囲だけを元の綴りへ戻す
		const auto ReadBoundaries = [&](
			const std::optional<CommentStructure::ReadBody> &Reading,
			const std::string_view Text,
			const std::span<const unsigned char> Protected
		) -> std::vector<SentenceBoundary> {
			std::vector<SentenceBoundary> Result =
			Reading ? FindSentenceBoundaries(Reading->Text, Reading->Code, true) : FindSentenceBoundaries(Text, Protected, true);
			if(Reading) for(SentenceBoundary &Boundary : Result) {
				Boundary.Pos = Reading->OriginalOffsets[Boundary.Pos] - Reading->SourceOffset;
				Boundary.MarkEnd = Reading->OriginalOffsets[Boundary.MarkEnd] - Reading->SourceOffset;
				Boundary.LineBegin = Reading->OriginalOffsets[Boundary.LineBegin] - Reading->SourceOffset;
				Boundary.LineEnd = Reading->OriginalOffsets[Boundary.LineEnd] - Reading->SourceOffset;
				Boundary.Next = Reading->OriginalOffsets[Boundary.Next] - Reading->SourceOffset;
			}
			// 元の綴りに対応する文境界の返戻
			return Result;
		};
		std::optional<CommentStructure::ReadBody> GroupReading;
		const CommentStructure::GroupFacts &GroupFacts = *Structure.Records[Group.front()].Data;
		if(GroupFacts.Reading) {
			GroupReading =
			CommentStructure::ReadBody{ GroupFacts.Reading->Text, GroupFacts.Reading->OriginalOffsets, GroupFacts.ReadingCode, 0 };
		}
		std::vector<SentenceBoundary> Boundaries =
		Group.size() > 1 ? ReadBoundaries(GroupReading, Joined, JoinedProtected) : std::vector<SentenceBoundary>();
		size_t BoundaryPos = 0;
		bool HasChanged = false;
		for(size_t Pos = 0; Pos < Group.size(); ++Pos) {
			const size_t Idx = Group[Pos];
			CommentRecord &Record = Records[Idx];
			const BodyRange &Body = Bodies[Pos];
			// 連結済の有効な本文を比較元に共有し，記録の所有バッファを編集
			const bool CanMove = Group.size() > 1 && Body.Facts.IsValidUtf8;
			const std::string_view Original =
			CanMove ? std::string_view(Joined).substr(Body.Offset, Record.Text.size()) : std::string_view(Record.Text);
			std::string Text = CanMove ? std::move(Record.Text) : Record.Text;
			const std::span<const unsigned char> Protected = Structure.GetCodeMask(Idx);
			if(Group.size() == 1 && Body.Facts.IsValidUtf8) Boundaries = ReadBoundaries(Structure.GetReadBody(Idx), Text, Protected);
			const size_t BoundaryBegin = BoundaryPos;
			// 位置順の境界を１度だけ対応付け，記録毎に全境界を探し直さない
			while(BoundaryPos < Boundaries.size() && Boundaries[BoundaryPos].Pos < Body.Offset + Text.size()) {
				SentenceBoundary &Boundary = Boundaries[BoundaryPos++];
				Boundary.Pos -= Body.Offset;
				Boundary.MarkEnd -= Body.Offset;
				Boundary.LineBegin -= Body.Offset;
				Boundary.LineEnd -= Body.Offset;
				Boundary.Next -= Body.Offset;
			}
			const CommentNotation Notation = Core::CommentRepository::GetOutputNotationOf(Record);
			NormalizeBody(
				Core::CommentRepository::GetRulesOf(Record),
				Text,
				std::span<const SentenceBoundary>(Boundaries).subspan(BoundaryBegin, BoundaryPos - BoundaryBegin),
				GetStackMarkerOf(Notation),
				Record.Format.ShouldAttachFirstLine && !Core::CommentRepository::IsBlockNotation(Notation) ?
				std::string(Core::CommentRepository::GetSymbolHeadOf(Record, std::string_view(Text).substr(0, Text.find('\n')))) :
				std::string(),
				Record.Kind == CommentKind::Trailing,
				Body.Facts,
				{ Protected.begin(), Protected.end() }
			);
			HasContent = HasContent || !Text.empty();
			if(
				Record.Output->ShouldFrame &&
				(Record.Format.ShouldSeparateLogicalLines || Record.Kind == CommentKind::OwnLine || Text.find('\n') != std::string::npos)
			) {
				Record.Format.ShouldSeparateLogicalLines = true;
				Record.Format.ShouldSeparateOpener = true;
				Record.Format.ShouldSeparateCloser = true;
			}
			if(Text != Original) {
				if(!CanMove) Record.Text = std::move(Text);
				HasChanged = true;
			}
			if(CanMove) Record.Text = std::move(Text);
		}
		if(!HasContent && Comments.PlacementTargets[Group.front()] != Core::NoNode) {
			Records[Group.front()].Format.HasSummaryPlaceholder = true;
		}
		if(!HasChanged) continue;
		// 変更されたグループだけを差し替え，未変更の本文と保護マスクは参照の儘共有
		for(const size_t Idx : Group) Records[Idx].StructureIndex.reset();
		if(!Next) Next.emplace(Structure);
		CommentStructure Updated = Inspect(ReadOnlyCommentView(Records, Group), &Structure);
		for(size_t Pos = 0; Pos < Group.size(); ++Pos) Next->Records[Group[Pos]] = std::move(Updated.Records[Pos]);
	}
	if(Next) Comments.SetStructure(std::move(*Next));
	// 終了
	return;
}
