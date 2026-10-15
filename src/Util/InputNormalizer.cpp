#include "InputNormalizer.hpp"
#include "CharClass.hpp"
#include "ParseAdapter.hpp"
#include "ParserAllocator.hpp"
#include "TextEdit.hpp"
#include "WorkingMemory.hpp"
#include "../Core/CommentRepository.hpp"
#include "../Profile/GrammarProfile.hpp"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * 入力の診断位置の取得関数
 * @param Offset 正規化済のテキストの位置（位置が無ければ無）
 * @return 原文の行とバイト桁（位置が無ければ無）
 */
std::optional<Core::OutputPoint> InputNormalizer::NormalizedInput::GetPoint(const std::optional<uint32_t> Offset) const {
	// 診断位置が無い場合の返戻
	if(!Offset) return std::nullopt;
	const uint32_t Pos = std::min<uint32_t>(*Offset, static_cast<uint32_t>(Text.size()));
	const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Bound =
	std::upper_bound(Offsets.begin(), Offsets.end(), std::pair<uint32_t, uint32_t>(Pos, UINT32_MAX));
	const std::pair<uint32_t, uint32_t> &Delta = *std::prev(Bound);
	const uint32_t Raw = Pos - Delta.first + Delta.second;
	const std::vector<uint32_t>::const_iterator Line = std::upper_bound(LineStarts.begin(), LineStarts.end(), Raw);
	// 原文上の診断位置の返戻
	return Core::OutputPoint{ static_cast<uint32_t>(Line - LineStarts.begin() - 1), Raw - *std::prev(Line) };
}

/**
 * 原文の変更不能な行頭表の取得関数
 * @return 正規化結果が所有する原文の行頭位置
 */
std::span<const uint32_t> InputNormalizer::NormalizedInput::GetLineStarts() const {
	return LineStarts;
}

/**
 * 原文の行頭表を消費する正規化結果から移譲する関数
 * 正規化でテキストを変えて居ない場合は，正規化済のテキストの行頭表としても使える
 * @return 原文上の論理行の開始位置（移譲後の結果は診断位置の照会へ使わない）
 */
std::vector<uint32_t> InputNormalizer::NormalizedInput::TakeLineStarts() && {
	// 所有する行頭表の移譲
	return std::move(LineStarts);
}

/**
 * 宣言を省略した登録済の XML ドキュメントの判定関数
 * 根の属性は引用された値を持ち，ドキュメントは自己閉鎖又は同じ根の閉じで終わる事を要求する
 * 計算量：原文長 B に対し O(B)，追加領域 O(1)
 * @param Rules 根要素と識別属性の登録
 * @param Src 先頭の空白を除いた原文
 * @return 登録済の根と属性を持つ XML ドキュメントなら true
 * @throws ProcessingInterrupted 書戻以外の処理の中断を要求済の場合
 */
bool InputNormalizer::IsRootMarkupDoc(const ReadingRules &Rules, std::string_view Src) {
	// 登録した根でない入力の返戻
	if(Rules.MarkupDocRoot.empty() || !Src.starts_with('<')) return false;
	Src.remove_prefix(1);
	// 根の名前が異なる事の返戻
	if(!Src.starts_with(Rules.MarkupDocRoot)) return false;
	Src.remove_prefix(Rules.MarkupDocRoot.size());
	bool HasAttribute = false;
	while(!Src.empty() && (Src.front() == ' ' || Src.front() == '\t' || Src.front() == '\r' || Src.front() == '\n')) {
		if(!(Src.size() & 0XFFF)) ParserAllocator::CheckInterruption();
		Src.remove_prefix(std::min(Src.find_first_not_of(" \t\r\n"), Src.size()));
		if(Src.starts_with('>') || Src.starts_with("/>")) break;
		const size_t NameEnd = Src.find_first_of("= \t\r\n");
		// 属性の名前と値の区切が無い事の返戻
		if(NameEnd == std::string_view::npos || !NameEnd) return false;
		const bool IsAttribute = Src.substr(0, NameEnd) == Rules.MarkupDocAttribute;
		Src.remove_prefix(NameEnd);
		Src.remove_prefix(std::min(Src.find_first_not_of(" \t\r\n"), Src.size()));
		// XML の属性でない事の返戻
		if(!Src.starts_with('=')) return false;
		Src.remove_prefix(1);
		Src.remove_prefix(std::min(Src.find_first_not_of(" \t\r\n"), Src.size()));
		// 引用した属性値でない事の返戻
		if(Src.empty() || Src.front() != '\'' && Src.front() != '"') return false;
		const size_t End = Src.find(Src.front(), 1);
		// 未閉じの属性値の返戻
		if(End == std::string_view::npos) return false;
		HasAttribute = HasAttribute || IsAttribute && End > 1;
		Src.remove_prefix(End + 1);
	}
	// ドキュメントを識別する属性が無い事の返戻
	if(!HasAttribute) return false;
	while(!Src.empty() && (Src.back() == ' ' || Src.back() == '\t' || Src.back() == '\r' || Src.back() == '\n')) {
		if(!(Src.size() & 0XFFF)) ParserAllocator::CheckInterruption();
		Src.remove_suffix(1);
	}
	// 自己閉鎖する根の返戻
	if(Src == "/>") return true;
	// 根の囲みが閉じない事の返戻
	if(!Src.starts_with('>') || !Src.ends_with('>')) return false;
	Src.remove_suffix(1);
	while(!Src.empty() && (Src.back() == ' ' || Src.back() == '\t' || Src.back() == '\r' || Src.back() == '\n')) {
		if(!(Src.size() & 0XFFF)) ParserAllocator::CheckInterruption();
		Src.remove_suffix(1);
	}
	// 閉じの名前が異なる事の返戻
	if(!Src.ends_with(Rules.MarkupDocRoot)) return false;
	Src.remove_suffix(Rules.MarkupDocRoot.size());
	// 同じ根の閉じで終わるドキュメントの返戻
	return Src.ends_with("</");
}

/**
 * Unicode の区切を改行と読む入力に其の区切が在るかの判定関数
 * NEL・行区切・段落区切は正規化で LF に為る為，見送る時に原文へ戻せる様に控える要否の判定にも用いる
 * 計算量：原文長 B と登録した区切の総長 S に対し O(B S)
 * @param Profile 対象の文法
 * @param Src 原文
 * @return 文法が改行と読む Unicode の区切を原文が含めば true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool InputNormalizer::HasUnicodeSeparator(const GrammarProfile &Profile, const std::string_view Src) {
	// 文法が改行と読む Unicode 記法の取得
	const std::vector<std::string_view> &Separators = Profile.GetReading().UnicodeLineSeparators;
	// 改行と読む区切の有無の返戻
	return std::any_of(
		Separators.begin(),
		Separators.end(),
		[&Src](const std::string_view Separator) -> bool {
			// 改行に読む区切を含むかの返戻
			return Src.find(Separator) != std::string_view::npos;
		}
	);
}

/**
 * 単独の CR を行の終わりとして読む記法かの判定関数（整形仕様 4.1）
 * 計算量：O(1)
 * @param Profile 対象の文法
 * @return 単独の CR を LF へ揃える記法なら true
 */
bool InputNormalizer::ShouldReadLoneCrAsNewline(const GrammarProfile &Profile) {
	// 文法が定める改行の読みの返戻
	return Profile.GetReading().ShouldReadLoneCrAsNewline;
}

/**
 * CRLF の正規化関数
 * リテラルとドキュメントの逐語内容の内側の CRLF は値其の物の為に保ち，其れ以外の CRLF を LF へ畳む
 * 計算量：原文長 B と逐語範囲数 R に対し O(B + R)
 * @param Src 正規化対象の原文（CRLF を含む，結果で上書きされる）
 * @param Literals 改行を値其の物として保つ範囲（原文順）
 * @return 逐語領域の外の CRLF を畳んだ場合 true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws ProcessingInterrupted 書戻以外の処理の中断を要求済の場合
 */
bool InputNormalizer::HasFoldedCrLfOutside(std::string &Src, const std::vector<std::pair<uint32_t, uint32_t>> &Literals) {
	// 正規化先と変更状態の準備
	std::string Normalized;
	size_t LiteralIdx = 0, Copied = 0;
	bool IsFolded = false;
	// 原文バイトの走査
	for(size_t Idx = 0; Idx < Src.size(); ++Idx) {
		if(!(Idx & 0XFFF)) ParserAllocator::CheckInterruption();
		while(LiteralIdx < Literals.size() && Literals[LiteralIdx].second <= Idx) ++LiteralIdx;
		// リテラルの外の CRLF だけを LF へ畳む
		if(
			Src[Idx] == '\r' && Idx + 1 < Src.size() && Src[Idx + 1] == '\n' &&
			!(LiteralIdx < Literals.size() && Literals[LiteralIdx].first <= Idx)
		) {
			if(!IsFolded) Normalized.reserve(Src.size());
			Normalized.append(Src, Copied, Idx - Copied);
			Copied = Idx + 1;
			IsFolded = true;
		}
	}
	// 保護範囲内の CRLF だけなら，本文の再構築と置換を行わない
	if(IsFolded) {
		Normalized.append(Src, Copied, std::string::npos);
		Src = std::move(Normalized);
	}
	// リテラルの外の CRLF を畳んだか（書戻で改行様式が変わるか）の返戻
	return IsFolded;
}

/**
 * 先頭の UTF-8 の BOM の数の取得関数
 * 計算量：先頭の BOM の数 C に対し O(C)
 * @param Src 原文
 * @return 先頭に続く BOM (EF BB BF) の数（リテラル・コメントの中の物は数えない）
 * @throws ProcessingInterrupted 書戻以外の処理の中断を要求済の場合
 */
size_t InputNormalizer::CountBom(const std::string_view Src) {
	// 先頭のエンコーディング標識の計数準備
	size_t Count = 0;
	while(Src.substr(3 * Count).starts_with("\xEF\xBB\xBF")) {
		if(!(Count & 0XFFF)) ParserAllocator::CheckInterruption();
		++Count;
	}
	// BOM の数の返戻
	return Count;
}

/**
 * 標準のラベルの正準名の取得関数
 * 計算量：ラベル長 L と固定のラベル表の総長 E に対し O(L + E)
 * @param Reading 宣言の読取定義
 * @param Text 宣言に書かれたラベル
 * @return 実際の読みに対応する正準名（未知のラベルなら空）
 * @throws std::bad_alloc ラベルの正規化用のメモリを確保出来ない場合
 * @throws std::length_error ラベルの長さが格納先の上限を超える場合
 */
std::string_view InputNormalizer::ReadEncodingLabel(const ReadingRules &Reading, const std::string_view Text) {
	// ASCII の空白と大小文字だけの正規化
	const size_t Start = Text.find_first_not_of("\t\n\f\r ");
	// 空白だけのラベルの返戻
	if(Start == std::string_view::npos) return std::string_view();
	std::string Name(Text.substr(Start, Text.find_last_not_of("\t\n\f\r ") - Start + 1));
	ToLowerAscii(Name);
	// 標準のラベルと，宣言の読みに適用する対応の照合
	for(const auto &[Canonical, Labels] : Reading.EncodingLabels) if(std::ranges::find(Labels, Name) != Labels.end()) {
		// 宣言の読みに対応する正準名の返戻
		for(const auto &[From, To] : Reading.EncodingPrescanReplacements) if(Canonical == From) return To;
		// 登録された正準名の返戻
		return Canonical;
	}
	// 未知のラベルの返戻
	return std::string_view();
}

/**
 * バイト列の prescan で属性を読む関数
 * 名前と値の ASCII 大小文字は呼出側で揃え，参照は其のバッファの中だけを指す
 * 計算量：消費するバイト数 B に対し O(B)，追加領域 O(1)
 * @param View 未読のバイト列（読んだ範囲だけ進める）
 * @return 属性の名前と値（タグの終端又は先頭範囲の終端なら無）
 */
std::optional<std::pair<std::string_view, std::string_view>> InputNormalizer::ReadEncodingAttribute(std::string_view &View) {
	// 属性名の開始位置の取得
	View.remove_prefix(std::min(View.find_first_not_of("\t\n\f\r /"), View.size()));
	// 属性が無い事の返戻
	if(View.empty() || View.front() == '>') return std::nullopt;
	size_t End = 0;
	while(
		End < View.size() && std::string_view("\t\n\f\r />").find(View[End]) == std::string_view::npos && (!End || View[End] != '=')
	) ++End;
	const std::string_view Name = View.substr(0, End);
	View.remove_prefix(End);
	View.remove_prefix(std::min(View.find_first_not_of("\t\n\f\r "), View.size()));
	// 値を持たない属性の返戻
	if(View.empty() || View.front() != '=') return std::pair(Name, std::string_view());
	View.remove_prefix(1);
	View.remove_prefix(std::min(View.find_first_not_of("\t\n\f\r "), View.size()));
	// 未完の属性又は空の属性値の返戻
	if(View.empty() || View.front() == '>') return std::pair(Name, std::string_view());
	// 引用された値又は空白迄の値の読取
	const bool IsQuoted = View.front() == '\'' || View.front() == '"';
	const char Quote = View.front();
	if(IsQuoted) View.remove_prefix(1);
	End = IsQuoted ? View.find(Quote) : View.find_first_of("\t\n\f\r >");
	// 先頭範囲の終端に達した属性の返戻（prescan を中断する）
	if(End == std::string_view::npos) {
		View = std::string_view();
		// 未完の属性の返戻
		return std::nullopt;
	}
	const std::string_view Value = View.substr(0, End);
	View.remove_prefix(End + static_cast<size_t>(IsQuoted));
	// 名前と値の返戻
	return std::pair(Name, Value);
}

/**
 * content 属性からエンコーディングを抽出する関数
 * 呼出側で ASCII 大小文字を揃えた属性値を読む
 * 計算量：属性値の長さ B と固定のラベル表の総長 E に対し O(B + E)
 * @param Reading 宣言の読取定義
 * @param View content 属性の未読の値
 * @return 最初に成立した charset の正準名（未知又は未成立なら空）
 * @throws std::bad_alloc ラベルの正規化用のメモリを確保出来ない場合
 * @throws std::length_error ラベルの長さが格納先の上限を超える場合
 */
std::string_view InputNormalizer::ReadContentEncoding(const ReadingRules &Reading, std::string_view View) {
	// charset と等号の位置の走査
	while(!View.empty()) {
		const size_t Start = View.find("charset");
		if(Start == std::string_view::npos) break;
		View.remove_prefix(Start + 7);
		View.remove_prefix(std::min(View.find_first_not_of("\t\n\f\r "), View.size()));
		if(View.empty()) break;
		if(View.front() != '=') continue;
		View.remove_prefix(1);
		View.remove_prefix(std::min(View.find_first_not_of("\t\n\f\r "), View.size()));
		if(View.empty()) break;
		// 引用されたラベル又は次の空白・セミコロン迄のラベルの取得
		if(View.front() == '\'' || View.front() == '"') {
			const char Quote = View.front();
			View.remove_prefix(1);
			const size_t End = View.find(Quote);
			// 引用符の閉じを持つラベルだけの返戻
			return End == std::string_view::npos ? std::string_view() : ReadEncodingLabel(Reading, View.substr(0, End));
		}
		// 引用符を持たないラベルの返戻
		return ReadEncodingLabel(Reading, View.substr(0, View.find_first_of("\t\n\f\r ;")));
	}
	// 成立する宣言が無い事の返戻
	return std::string_view();
}

/**
 * prescan の終了時に先頭の XML 宣言を読む関数
 * 計算量：先頭範囲の長さ B と固定のラベル表の総長 E に対し O(B + E)
 * @param Reading 宣言の読取定義
 * @param View 先頭範囲のバイト列
 * @return XML 宣言の正準名（未知又は未成立なら空）
 * @throws std::bad_alloc ラベルの正規化用のメモリを確保出来ない場合
 * @throws std::length_error ラベルの長さが格納先の上限を超える場合
 */
std::string_view InputNormalizer::ReadXmlEncoding(const ReadingRules &Reading, std::string_view View) {
	// 大小文字を区別する XML 宣言と最初の encoding の識別
	const size_t Close = View.find('>'), Start = View.find("encoding");
	// XML 宣言を持たないバイト列の返戻
	if(!View.starts_with("<?xml") || Close == std::string_view::npos || Start == std::string_view::npos || Start > Close) {
		// XML 宣言が無い事の返戻
		return std::string_view();
	}
	View.remove_prefix(Start + 8);
	while(!View.empty() && static_cast<unsigned char>(View.front()) < 0X21) View.remove_prefix(1);
	// 値の区切を持たない宣言の返戻
	if(View.empty() || View.front() != '=') return std::string_view();
	View.remove_prefix(1);
	while(!View.empty() && static_cast<unsigned char>(View.front()) < 0X21) View.remove_prefix(1);
	// 引用された値を持たない宣言の返戻
	if(View.empty() || View.front() != '\'' && View.front() != '"') return std::string_view();
	const char Quote = View.front();
	View.remove_prefix(1);
	const size_t End = View.find(Quote);
	// 閉じた値を持たない宣言の返戻
	if(End == std::string_view::npos) return std::string_view();
	View = View.substr(0, End);
	// XML の宣言値では空白と制御文字を許さない
	for(const unsigned char Byte : View) if(Byte < 0X21) return std::string_view();
	// ラベルに対応する正準名の返戻
	return ReadEncodingLabel(Reading, View);
}

/**
 * タグを持つドキュメントのエンコーディングの prescan 関数
 * 最初の有効な meta 宣言を読み，見付からなければ XML 宣言の読みに従う
 * 計算量：固定長の先頭範囲 B と固定のラベル表の総長 E に対し O(B E)，追加領域 O(B)
 * @param Reading 宣言の読取定義
 * @param Text BOM を除いたドキュメント
 * @return 宣言から得た正準名（宣言が無ければ空）
 * @throws std::bad_alloc prescan 又はラベルの正規化用のメモリを確保出来ない場合
 * @throws std::length_error 先頭範囲又はラベルの長さが格納先の上限を超える場合
 */
std::string_view InputNormalizer::ReadMarkupEncoding(const ReadingRules &Reading, const std::string_view Text) {
	// バイト数で制限した prescan の準備
	const std::string_view Head = Text.substr(0, Reading.EncodingPrescanBytes);
	// UTF-16 の XML 開始記法の返戻（meta の UTF-16 ラベルとは異なる）
	if(Head.starts_with(std::string_view("<\0?\0x\0", 6))) return "utf-16le";
	// UTF-16 ビッグエンディアンの開始記法の返戻
	if(Head.starts_with(std::string_view("\0<\0?\0x", 6))) return "utf-16be";
	std::string Lower(Head);
	ToLowerAscii(Lower);
	std::string_view View(Lower);
	// コメント・タグ・其の他のバイトの走査
	while(!View.empty()) {
		if(View.starts_with("<!--")) {
			const size_t End = View.find("-->", 2);
			if(End == std::string_view::npos) break;
			View.remove_prefix(End + 2);
		} else if(
			View.starts_with('<') && View.substr(1).starts_with(Reading.EncodingElementName) &&
			View.size() > Reading.EncodingElementName.size() + 1 &&
			std::string_view("\t\n\f\r /").find(View[Reading.EncodingElementName.size() + 1]) != std::string_view::npos
		) {
			View.remove_prefix(Reading.EncodingElementName.size() + 1);
			bool HasCharset = false, HasContent = false, HasPragma = false, GotPragma = false;
			std::optional<bool> NeedsPragma;
			std::string_view Charset;
			while(const std::optional<std::pair<std::string_view, std::string_view>> Attribute = ReadEncodingAttribute(View)) {
				const auto &[Name, Value] = *Attribute;
				if(Name == "http-equiv" && !HasPragma) {
					HasPragma = true;
					GotPragma = Value == "content-type";
				} else if(Name == Reading.EncodingContentAttribute && !HasContent) {
					HasContent = true;
					if(!NeedsPragma) {
						Charset = ReadContentEncoding(Reading, Value);
						if(!Charset.empty()) NeedsPragma = true;
					}
				} else if(Name == Reading.EncodingCharsetAttribute && !HasCharset) {
					HasCharset = true;
					Charset = ReadEncodingLabel(Reading, Value);
					NeedsPragma = false;
				}
			}
			if(View.empty()) break;
			// 最初に成立した宣言の読みに対応する正準名の返戻
			if(NeedsPragma && (!*NeedsPragma || GotPragma) && !Charset.empty()) {
				// meta 要素で置き換える正準名の返戻
				for(const auto &[From, To] : Reading.MetaEncodingReplacements) if(Charset == From) return To;
				// 宣言の正準名の返戻
				return Charset;
			}
		} else if(
			View.size() > 1 && View[0] == '<' &&
			(View[1] >= 'a' && View[1] <= 'z' || View.size() > 2 && View[1] == '/' && View[2] >= 'a' && View[2] <= 'z')
		) {
			const size_t End = View.find_first_of("\t\n\f\r >");
			if(End == std::string_view::npos) break;
			View.remove_prefix(End);
			while(ReadEncodingAttribute(View)) {}
			if(View.empty()) break;
		} else if(View.starts_with("<!") || View.starts_with("</") || View.starts_with("<?")) {
			const size_t End = View.find('>');
			if(End == std::string_view::npos) break;
			View.remove_prefix(End);
		}
		View.remove_prefix(1);
	}
	// meta で決まらない場合の XML 宣言の返戻
	return ReadXmlEncoding(Reading, Head);
}

/**
 * 先頭の厳密なバイト列のエンコーディング宣言の読取関数
 * 計算量：固定長の先頭範囲 B と固定のラベル表の総長 E に対し O(B + E)
 * @param Reading 宣言の読取定義
 * @param Text BOM を除いたドキュメント
 * @return 有効な宣言の正準名（未成立又は未知のラベルなら空）
 * @throws std::bad_alloc ラベルの正規化用のメモリを確保出来ない場合
 * @throws std::length_error ラベルの長さが格納先の上限を超える場合
 */
std::string_view InputNormalizer::ReadByteEncoding(const ReadingRules &Reading, const std::string_view Text) {
	// 厳密な先頭バイト列と ASCII のラベルの照合
	std::string_view View = Text.substr(0, Reading.EncodingPrescanBytes);
	// 宣言の開始記法が無い事の返戻
	if(!View.starts_with(Reading.EncodingBytePrefix)) return std::string_view();
	View.remove_prefix(Reading.EncodingBytePrefix.size());
	for(size_t End = 0; End < View.size(); ++End) {
		// 最初の引用符を閉じとし，後続のセミコロンも同時に照合する
		if(View[End] == Reading.EncodingByteSuffix.front()) {
			// 成立した宣言だけの返戻
			return View.substr(End).starts_with(Reading.EncodingByteSuffix) ?
			ReadEncodingLabel(Reading, View.substr(0, End)) :
			std::string_view();
		}
		// ASCII 外のバイトを含むラベルの返戻
		if(static_cast<unsigned char>(View[End]) > 0X7F) return std::string_view();
	}
	// 先頭範囲内で閉じない宣言の返戻
	return std::string_view();
}

/**
 * 入力のエンコーディング宣言の位置と値の受理判定関数
 * 宣言を処理系が読む範囲だけを調べ，登録された組の順と名前の規則を保つ
 * 計算量：頭部の長さ B と固定の名前表の総長 E に対し O(B E)
 * @param Profile 対象の文法
 * @param Text BOM を除いた入力
 * @param HasFilePreamble ファイル全体のバイト列として読む入力か
 * @param HasBom 原文の先頭に UTF-8 の BOM が在るか
 * @param DeclarationOffset 対応外の宣言の値の位置（其の理由でなければ変更しない）
 * @return BOM と宣言の矛盾又は対応外の読みの見送理由（受理すれば無）
 * @throws std::bad_alloc 名前の読取又は正規化用のメモリを確保出来ない場合
 * @throws std::length_error 名前の長さが格納先の上限を超える場合
 */
FailureReason::Kind InputNormalizer::GetEncodingDeclFailure(
	const GrammarProfile &Profile,
	const std::string_view Text,
	const bool HasFilePreamble,
	const bool HasBom,
	size_t &DeclarationOffset
) {
	const ReadingRules &Reading = Profile.GetReading();
	// 宣言を制限しない文法と，親がデコード済の埋込内容でバイト列の宣言を読まない事の返戻
	if(
		Reading.AllowedEncodingNames.empty() && Reading.BomEncodingName.empty() || Reading.EncodingPrescanBytes && !HasFilePreamble
	) return FailureReason::None;
	const auto Judge = [&](const std::string_view Name, const size_t Offset) -> FailureReason::Kind {
		// 原文で矛盾する BOM と宣言は，BOM の除去で正常な入力へ変えない
		if(HasBom && !Reading.BomEncodingName.empty() && Name != Reading.BomEncodingName) return FailureReason::InputSyntaxError;
		// 登録された状態の無い名前だけを受理する
		if(
			!Reading.AllowedEncodingNames.empty() &&
			std::ranges::find(Reading.AllowedEncodingNames, Name) == Reading.AllowedEncodingNames.end()
		) {
			// 値の位置を持たない先頭範囲・バイト列の宣言と起動オプションの見送理由の返戻
			if(Offset == std::string::npos) return FailureReason::UnsupportedEncoding;
			DeclarationOffset = Offset;
			// 宣言の値を直す対処を持つ見送理由の返戻
			return FailureReason::UnsupportedEncodingDeclaration;
		}
		// 受理の返戻
		return FailureReason::None;
	}; // 正準名の BOM との矛盾と受理の判定
	if(Reading.HasMarkupEncodingPrescan || !Reading.EncodingBytePrefix.empty()) {
		const std::string_view Name =
		Reading.HasMarkupEncodingPrescan ? ReadMarkupEncoding(Reading, Text) : ReadByteEncoding(Reading, Text);
		// 先頭範囲又は先頭のバイト列の宣言の判定の返戻
		return Name.empty() ? FailureReason::None : Judge(Name, std::string::npos);
	}
	const Core::RegisteredLineForms &Forms = Profile.GetRules().Notation.OutputForms.Conditions;
	const Core::CommentRepository::EncodingHeader Header = Core::CommentRepository::ReadEncodingHeader(Reading, Forms, Text);
	const std::string_view Declared = Text.substr(Header.NameStart, Header.NameEnd - Header.NameStart);
	std::string_view EncodingName = Declared, AlternateEncodingName;
	// 通常の宣言が無い場合だけ，処理系の起動オプションを読む
	if(
		Declared.empty() && Reading.HasSingleLineEncodingHeader &&
		Text.starts_with(Core::CommentRepository::GetOpenerOf(CommentNotation::HashBangLine))
	) {
		const auto [First, Last] = Core::CommentRepository::ReadShebangEncoding(Reading, Text.substr(0, Header.FirstLineEnd));
		EncodingName = Last;
		if(First != Last) AlternateEncodingName = First;
	}
	for(const std::string_view Encoding : { EncodingName, AlternateEncodingName }) {
		if(Encoding.empty()) continue;
		std::string Name(Encoding);
		for(char &Byte : Name) {
			Byte = Reading.EncodingNameJoiners.find(Byte) != std::string_view::npos ?
			'-' :
			static_cast<char>(std::tolower(static_cast<unsigned char>(Byte)));
		}
		for(const auto &[Prefix, Canonical] : Reading.EncodingNamePrefixes) if(Name.starts_with(Prefix)) {
			Name = Canonical;
			break;
		}
		// 行の宣言は値の位置を，起動オプションは位置無で判定し，見送の理由の返戻
		if(
			const FailureReason::Kind Reason = Judge(Name, Declared.empty() ? std::string::npos : Header.NameStart);
			Reason != FailureReason::None
		) return Reason;
	}
	// 宣言が無い入力と，原文の BOM と読みを保てる宣言の受理の返戻
	return FailureReason::None;
}

/**
 * マーカーの無い多バイトエンコーディングの周期的な零バイトの判定関数
 * 計算量：O(1)（先頭の固定長の標本）
 * @param Src 実行停止後の逐語データを除いた原文
 * @return UTF-16 又は UTF-32 の ASCII 部分に相当する零バイトの並びなら true
 */
bool InputNormalizer::IsWideEncodingCandidate(const std::string_view Src) {
	// 固定長のエンコーディング判別範囲の取得
	const size_t Size = std::min(Src.size(), size_t{ 32 });
	// 先頭で零バイトの周期を持つ広いエンコーディングの１文字以上を確かめる
	for(const size_t Width : { size_t{ 4 }, size_t{ 2 } }) {
		if(Src.size() % Width || Size < Width) continue;
		for(size_t TextByte = 0; TextByte < Width; ++TextByte) {
			bool DoesMatch = true;
			for(size_t Pos = 0; Pos + Width <= Size; Pos += Width) {
				for(size_t Part = 0; Part < Width; ++Part) if(Src[Pos + Part] == '\0' != (Part != TextByte)) {
					DoesMatch = false;
					break;
				}
				if(!DoesMatch) break;
			}
			// 周期的な零バイトを検出した事の返戻
			if(DoesMatch) return true;
		}
	}
	// 多バイトエンコーディングのマーカーが無い事の返戻
	return false;
}

/**
 * 共有拡張子の対象外のドキュメントの判定関数
 * 計算量：原文長 B と登録した記法の総長 S に対し O(B S)
 * @param Rules 入力の読取規則
 * @param Src 整形対象のソース
 * @return 対象外のドキュメントのマーカーで始まれば true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool InputNormalizer::IsMarkupDocCandidate(const ReadingRules &Rules, const std::string &Src) {
	// 最初の非空白位置
	size_t Head = Src.find_first_not_of(" \t\r\n");
	// 文法が行区切と読む先頭の Unicode の空白も，正規化前のテキストで読飛
	while(Head != std::string::npos) {
		const std::vector<std::string_view>::const_iterator Separator = std::find_if(
			Rules.UnicodeLineSeparators.begin(),
			Rules.UnicodeLineSeparators.end(),
			[&Src, Head](const std::string_view Candidate) -> bool {
				// 先頭の行区切のテキストとの一致の返戻
				return !Src.compare(Head, Candidate.size(), Candidate);
			}
		);
		if(Separator == Rules.UnicodeLineSeparators.end()) break;
		Head = Src.find_first_not_of(" \t\r\n", Head + Separator->size());
	}
	// 宣言・文書型又は登録した根を持つ XML ドキュメントかの返戻
	return Head != std::string::npos && (
		std::any_of(
			Rules.MarkupDocHeads.begin(),
			Rules.MarkupDocHeads.end(),
			[&Src, Head](const std::string_view Marker) -> bool {
				// ドキュメントのマーカーとの先頭一致の返戻
				return !Src.compare(Head, Marker.size(), Marker);
			}
		) || IsRootMarkupDoc(Rules, std::string_view(Src).substr(Head))
	);
}

/**
 * 改行コードの LF 基準の正規化関数
 * 逐語領域の内側の改行を原文の儘保ち，其れ以外の改行を LF へ揃える
 * 末尾の改行の数は揃えない（中身に為る末尾は投影が保ち，其の他は出力の終端契約が揃える）
 * 計算量：原文長 N と保護範囲の収集・解析の総費用 Q に対し O(N + Q)
 * @param Profile 対象の文法
 * @param Src 正規化対象の原文（結果で上書きされる）
 * @param HasUnicodeSeparator 原文が文法で改行と読む Unicode の区切を含むか
 * @param Reason 改行を値として保つ範囲を確定出来ず正規化しなかった理由の格納先
 * @param Trial 同じテキストを解析済なら再利用する試行解析の結果
 * @param Changes 実際に変えた表現の格納先
 * @return 末尾の改行の数以外の改行の姿（CRLF・単独の CR・文法が読む Unicode の区切）を変えた場合 true
 * @throws std::bad_alloc 正規化するテキストと保護範囲の確保に失敗した場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws ProcessingInterrupted 書戻以外の処理の中断を要求済の場合
 */
bool InputNormalizer::HasNormalizedNewlines(
	const GrammarProfile &Profile,
	std::string &Src,
	const bool HasUnicodeSeparator,
	FailureReason &Reason,
	std::optional<ParsedTree> &Trial,
	InputNormalization &Changes
) {
	// 改行表現を変えたマーカー
	bool IsFormChanged = false;
	// 単独 CR を改行と読む言語だけ LF 化し，空白・地の文・リテラルの CR は保持
	const bool IsLoneCrNewline = ShouldReadLoneCrAsNewline(Profile);
	bool IsLoneCr = false, HasCrLf = false, HasBareLf = false;
	for(size_t Pos = 0; Pos < Src.size(); ++Pos) {
		if(!(Pos & 0XFFF)) ParserAllocator::CheckInterruption();
		if(Src[Pos] == '\n') {
			if(Pos && Src[Pos - 1] == '\r') HasCrLf = true;
			else HasBareLf = true;
		} else if(IsLoneCrNewline && Src[Pos] == '\r' && (Pos + 1 == Src.size() || Src[Pos + 1] != '\n')) IsLoneCr = true;
	}
	Changes.HasMixedNewlines = HasCrLf && HasBareLf;
	// 個別の改行列を含む場合
	if(IsLoneCr || HasUnicodeSeparator) {
		// 保護範囲列（確定出来なければリテラルの中を書き換えない様に正規化しない）
		const std::optional<ParseAdapter::LiteralRangeList> Protected = ParseAdapter::LiteralRanges(Profile, Src, true, Trial, Reason);
		// 保護範囲を確定出来ない事の返戻
		if(!Protected) return false;
		const std::vector<std::pair<uint32_t, uint32_t>> &Literals = *Protected;
		const std::vector<std::string_view> &Separators = Profile.GetReading().UnicodeLineSeparators;
		// 個別改行を LF 化する出力
		std::string Normalized;
		size_t LiteralIdx = 0, Copied = 0;
		for(size_t Idx = 0; Idx < Src.size(); ++Idx) {
			if(!(Idx & 0XFFF)) ParserAllocator::CheckInterruption();
			while(LiteralIdx < Literals.size() && Literals[LiteralIdx].second <= Idx) ++LiteralIdx;
			// 保護範囲内の場合
			if(LiteralIdx < Literals.size() && Literals[LiteralIdx].first <= Idx) continue;
			// 単独 CR の LF 化
			if(IsLoneCr && Src[Idx] == '\r' && (Idx + 1 == Src.size() || Src[Idx + 1] != '\n')) {
				if(!IsFormChanged) Normalized.reserve(Src.size());
				Normalized.append(Src, Copied, Idx - Copied);
				Normalized += '\n';
				Copied = Idx + 1;
				Changes.HasNormalizedCrOnly = !HasCrLf && !HasBareLf;
				IsFormChanged = true;
				continue;
			}
			if(HasUnicodeSeparator) if(
				const std::vector<std::string_view>::const_iterator Separator = std::find_if(
					Separators.begin(),
					Separators.end(),
					[&Src, Idx](const std::string_view Candidate) -> bool {
						// 此処から改行に読む区切が始まるかの返戻
						return !Src.compare(Idx, Candidate.size(), Candidate);
					}
				); Separator != Separators.end()
			) {
				// 文法が読む Unicode の区切の LF 化
				if(!IsFormChanged) Normalized.reserve(Src.size());
				Normalized.append(Src, Copied, Idx - Copied);
				Normalized += '\n';
				Idx += Separator->size() - 1;
				Copied = Idx + 1;
				Changes.HasNormalizedUnicodeSeparator = true;
				IsFormChanged = true;
			}
		}
		// 変更の無い本文は組み直さず，同じ試行解析と其のテキストを保つ
		if(IsFormChanged) {
			Normalized.append(Src, Copied, std::string::npos);
			Src = std::move(Normalized);
			Trial.reset();
		}
	}
	// リテラル外の CRLF の正規化
	if(Src.find("\r\n") != std::string::npos) {
		// 値を構成するリテラルの内側の CRLF を保つ為の保護範囲（整形仕様 4.1, 10.3）
		const std::optional<ParseAdapter::LiteralRangeList> Literals = ParseAdapter::LiteralRanges(Profile, Src, false, Trial, Reason);
		// 保護範囲を確定出来ない事の返戻
		if(!Literals) return false;
		Changes.HasFoldedCrLf = HasFoldedCrLfOutside(Src, *Literals);
		if(Changes.HasFoldedCrLf) Trial.reset();
		// 改行の姿を変えた事の返戻
		return Changes.HasFoldedCrLf || IsFormChanged;
	}
	// 改行の姿を変えたかの返戻（末尾の改行の数は出力の終端契約が揃え，中身の改行は投影が保つ）
	return IsFormChanged;
}

/**
 * 入力の表現の受理の判定と正規化関数（整形仕様 3.4 の Normalize，4.1）
 * エンコーディング・BOM・改行様式を受理の条件と照らし，正規化したテキストを新しい値として返す（原文は書き換えない）
 * 計算量：原文長 N とテキストの正規化・試行解析の総費用 Q に対し O(N + Q)
 * @param Profile 対象の文法
 * @param RawSrc 整形対象の原文
 * @param HasFilePreamble ファイル先頭の BOM を持ち得る入力か
 * @param IsAcceptedInput 同じ原文と入口条件で両上限以内と受理済か（未受理なら此処で受理する）
 * @return 正規化の結果（受理しない入力は見送理由を持つ）
 * @throws std::bad_alloc 正規化するテキストと解析結果の確保に失敗した場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws FormatLimitExceeded 保護範囲又はデータ境界の試行解析を初期化出来ない場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 * @throws ProcessingInterrupted 書戻以外の処理の中断を要求済の場合
 */
InputNormalizer::NormalizedInput InputNormalizer::Normalize(
	const GrammarProfile &Profile,
	const std::string &RawSrc,
	const bool HasFilePreamble,
	const bool IsAcceptedInput
) {
	// 入力の正規化の結果の構築準備
	NormalizedInput Input;
	Input.HasFilePreamble = HasFilePreamble;
	if(!IsAcceptedInput) {
		Input.Reason = AcceptInput(
			RawSrc.size(),
			[&]() -> size_t {
				// 同じ入口条件の受理見積の返戻
				return EstimateWorkingBytes(Profile, RawSrc);
			}
		);
		// 受理上限を超えた入力の返戻
		if(Input.Reason != FailureReason::None) return Input;
	}
	// 文字として読めない原本の見送（UTF-16 のマーカーと，其れに前が重ならない UTF-32 ビッグエンディアンのマーカー）
	// 空文字は一律に拒まず，文法が受理するかで判る（整形仕様 4.1）
	if(
		RawSrc.starts_with("\xFF\xFE") || RawSrc.starts_with("\xFE\xFF") || RawSrc.starts_with(std::string_view("\0\0\xFE\xFF", 4))
	) {
		Input.Reason = FailureReason::UnsupportedEncoding;
		// 対応外エンコーディングの見送の返戻
		return Input;
	}
	// 先頭の UTF-8 の BOM １個だけを除いたテキスト（成功した出力は入口に依らず BOM 無の UTF-8，整形仕様 4.1, 10.3）
	const size_t BomCount = HasFilePreamble ? CountBom(RawSrc) : 0;
	Input.Changes.HasRemovedBom = BomCount;
	Input.Text.assign(RawSrc, BomCount ? 3 : 0);
	// ２個目以降は普通の U+FEFF であり，パーサーへ新しい先頭の BOM として渡さない
	if(BomCount > 1 && !Profile.GetReading().CanReadByteOrderMarkAsSpace) {
		// 処理系がコード内の U+FEFF を拒む入力と，BOM 無の出力の先頭では BOM と区別出来ない本文の U+FEFF を保てない入力（整形仕様 10.3）
		Input.Reason =
		Profile.GetReading().ShouldRejectExtraLeadingBom ? FailureReason::ParserUnreadable : FailureReason::LeadingTextLost;
		// 先頭の U+FEFF を読めない又は保てない入力の見送の返戻
		return Input;
	}
	// コードの後の逐語データを正規化とエンコーディング検査の対象から外す（試行解析は BOM を除いたテキストで行い，後の解析で再利用する）
	const size_t DataStart = ParseAdapter::GetHaltedDataStart(Profile, Input.Text, Input.Trial, Input.Reason);
	// 初期化失敗は通常の見送へ落とさず，入口の失敗処理へ渡す
	if(Input.Reason == FailureReason::ParserInitFailed) throw FormatLimitExceeded(Input.Reason.GetText(), Input.Reason.Val);
	// 境界解析の期限切で正規化を始めない事の返戻
	if(Input.Reason != FailureReason::None) return Input;
	std::string Data;
	if(DataStart != std::string::npos) {
		Data.assign(Input.Text, DataStart);
		Input.Text.resize(DataStart);
		Input.Trial.reset();
	}
	// 正規化対象のエンコーディングの検査
	if(IsWideEncodingCandidate(Input.Text) || !TextEdit::IsValidUtf8(Input.Text)) {
		Input.Reason = FailureReason::UnsupportedEncoding;
		// UTF-8 として読めない原本の見送の返戻
		return Input;
	}
	size_t DeclarationOffset = std::string::npos;
	Input.Reason = GetEncodingDeclFailure(Profile, Input.Text, HasFilePreamble, static_cast<bool>(BomCount), DeclarationOffset);
	if(DeclarationOffset != std::string::npos) {
		// 正規化前の頭部だけで診断位置を確定し，未構築の位置対応表を参照しない
		Core::OutputPoint Point;
		const size_t RawOffset = DeclarationOffset + (BomCount ? 3 : 0);
		const bool IsLoneCrNewline = ShouldReadLoneCrAsNewline(Profile);
		for(size_t Pos = 0; Pos < RawOffset; ++Pos) {
			if(!(Pos & 0XFFF)) ParserAllocator::CheckInterruption();
			if(RawSrc[Pos] == '\n' || RawSrc[Pos] == '\r' && IsLoneCrNewline) {
				if(RawSrc[Pos] == '\r' && Pos + 1 < RawOffset && RawSrc[Pos + 1] == '\n') ++Pos;
				++Point.Row;
				Point.Column = 0;
			} else ++Point.Column;
		}
		Input.ReasonPoint = Point;
	}
	// 原文の BOM と宣言の有効性・値を確定出来ない入力の見送の返戻
	if(Input.Reason != FailureReason::None) return Input;
	// 拡張子を共有する対象外のドキュメントの見送
	if(Profile.GetReading().CanContainMarkupDoc && IsMarkupDocCandidate(Profile.GetReading(), Input.Text)) {
		Input.Reason = FailureReason::MarkupDoc;
		// 対象外のドキュメントの返戻
		return Input;
	}
	// 改行の正規化（改行を値として保つ範囲を確定出来なければ正規化せずに見送る）
	const bool IsNewlineChanged =
	HasNormalizedNewlines(Profile, Input.Text, HasUnicodeSeparator(Profile, Input.Text), Input.Reason, Input.Trial, Input.Changes);
	Input.IsFormChanged = IsNewlineChanged || Input.Changes.HasRemovedBom;
	// 保護範囲の解析の初期化失敗は入口の失敗処理へ渡す
	if(Input.Reason == FailureReason::ParserInitFailed) throw FormatLimitExceeded(Input.Reason.GetText(), Input.Reason.Val);
	// 改行を正規化出来ない入力の見送の返戻
	if(Input.Reason != FailureReason::None) return Input;
	// コードの後の逐語データの復帰
	if(DataStart != std::string::npos) Input.DataStart = Input.Text.size();
	Input.Text += Data;
	// 正規化で確定したテキストと原文の対応を所有する（逐語範囲を再判定しない）
	Input.LineStarts.push_back(0);
	size_t RawPos = Input.Changes.HasRemovedBom ? 3 : 0;
	Input.Offsets.emplace_back(0, static_cast<uint32_t>(RawPos));
	// 原文と同じテキストは位置の差分を持たず，改行位置だけを検索して同じ行頭表を作る
	if(!Input.IsFormChanged) {
		for(size_t End = Input.Text.find('\n'); End != std::string::npos; End = Input.Text.find('\n', End + 1)) {
			Input.LineStarts.push_back(static_cast<uint32_t>(End + 1));
		}
		// テキストを変えて居ない正規化結果の返戻
		return Input;
	}
	for(size_t Pos = 0; Pos < Input.Text.size(); ++Pos) {
		if(!(Pos & 0XFFF)) ParserAllocator::CheckInterruption();
		size_t RawEnd = RawPos + 1;
		if(Input.Text[Pos] == '\n' && RawSrc[RawPos] != '\n') {
			if(RawSrc[RawPos] == '\r' && RawEnd < RawSrc.size() && RawSrc[RawEnd] == '\n') ++RawEnd;
			else if(const unsigned char Byte = static_cast<unsigned char>(RawSrc[RawPos]); Byte > 0XBF) {
				RawEnd = RawPos + (Byte > 0XDF ? 3 : 2);
			}
		}
		if(Input.Text[Pos] == '\n') Input.LineStarts.push_back(static_cast<uint32_t>(RawEnd));
		if(RawEnd != RawPos + 1) Input.Offsets.emplace_back(static_cast<uint32_t>(Pos + 1), static_cast<uint32_t>(RawEnd));
		RawPos = RawEnd;
	}
	// 正規化の結果の返戻
	return Input;
}
