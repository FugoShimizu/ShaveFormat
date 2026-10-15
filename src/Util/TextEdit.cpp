#include "TextEdit.hpp"
#include "ParsedTree.hpp"
#include "../Profile/GrammarProfile.hpp"
#include "../Profile/ReadingRules.hpp"
#include <tree_sitter/api.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

/**
 * UTF-8 の１コードポイントデコード関数
 * 計算量：O(1)
 * @param Text 対象文字列
 * @param Pos 開始位置（成功時は次のコードポイントへ進む）
 * @param Point デコード結果（失敗時は変更しない）
 * @return 最短形の Unicode スカラー値をデコード出来れば true
 */
bool TextEdit::IsUtf8Decoded(const std::string_view Text, size_t &Pos, char32_t &Point) {
	// 末尾に達した事の返戻
	if(Pos >= Text.size()) return false;
	// 先頭バイトから後続バイト数の判定
	const unsigned char Lead = static_cast<unsigned char>(Text[Pos]);
	size_t Follow = 0;
	if(Lead < 0X80) Follow = 0;
	else if((Lead & 0XE0) == 0XC0) Follow = 1;
	else if((Lead & 0XF0) == 0XE0) Follow = 2;
	else if((Lead & 0XF8) == 0XF0) Follow = 3;
	// 先頭のバイトとして不正な事の返戻
	else return false;
	// 続くバイトの足りない事の返戻
	if(Follow >= Text.size() - Pos) return false;
	// 後続バイト数０〜３に対応する先頭バイトの値部分
	static constexpr uint32_t LeadMask[] = { 0X7F, 0X1F, 0XF, 0X7 };
	char32_t Val = Lead & LeadMask[Follow];
	for(size_t Idx = 1; Idx <= Follow; ++Idx) {
		const unsigned char Continuation = static_cast<unsigned char>(Text[Pos + Idx]);
		// 続きのバイトとして不正な事の返戻
		if((Continuation & 0XC0) != 0X80) return false;
		Val = Val << 6 | Continuation & 0X3F;
	}
	// 後続バイト数別の最小コードポイント
	// 同じコードポイントの複数表現を拒む最短形の強制
	// 冗長なエンコーディング・範囲外・サロゲートの事の返戻
	if(
		static constexpr char32_t LeastByFollow[] = { 0, 0X80, 0X800, 0X10000 };
		Val < LeastByFollow[Follow] || Val > 0X10FFFF || Val > 0XD7FF && Val < 0XE000
	) return false;
	Pos += Follow + 1;
	Point = Val;
	// デコード成功の返戻
	return true;
}

/**
 * 位置から始まる行内空白の長さの取得関数
 * 計算量：登録した空白の記法の総長 S に対し O(S)
 * @param Src 元のソースコード
 * @param Reading 文法の読取規則
 * @param Pos 空白の始まりの位置
 * @return 空白の字の UTF-8 の長さ（空白で無ければ 0）
 */
size_t TextEdit::SpaceAt(const std::string_view Src, const ReadingRules &Reading, const uint32_t Pos) {
	// 範囲外の位置の返戻
	if(Pos >= Src.size()) return 0;
	// ASCII の字の空白の長さの返戻
	if(const unsigned char Lead = static_cast<unsigned char>(Src[Pos]); Lead < 0X80) return Reading.IsSpace(Lead) ? 1 : 0;
	size_t End = Pos;
	char32_t Point = 0;
	// デコードしたコードポイントの空白の長さの返戻
	return IsUtf8Decoded(Src, End, Point) && Reading.IsSpace(Point) ? End - Pos : 0;
}

/**
 * 位置で終わる行内空白の長さの取得関数
 * 計算量：登録した空白の記法の総長 S に対し O(S)
 * @param Src 元のソースコード
 * @param Reading 文法の読取規則
 * @param Pos 空白の終わりの位置
 * @return 空白の字の UTF-8 の長さ（空白で無ければ 0）
 */
size_t TextEdit::SpaceBefore(const std::string_view Src, const ReadingRules &Reading, const uint32_t Pos) {
	// UTF-8 の１〜３バイトの空白の字で終わる場合の其の長さの返戻
	// 位置の直前に在る空白幅の返戻
	for(size_t Width = 1; Width < 4 && Width <= Pos; ++Width) if(SpaceAt(Src, Reading, Pos - Width) == Width) return Width;
	// 空白で終わらない事の返戻
	return 0;
}

/**
 * 指定位置から左方向への行内空白のスキップ関数
 * 計算量：走査するテキスト長 B と空白の記法の総長 S に対し O(B S)
 * @param Src 元のソースコード
 * @param Reading 文法の読取規則
 * @param Pos 開始位置
 * @return スキップ後の位置
 */
uint32_t TextEdit::SkipSpaceLeft(const std::string_view Src, const ReadingRules &Reading, uint32_t Pos) {
	// 対象範囲の走査
	for(size_t Width = 0; Pos; Pos -= static_cast<uint32_t>(Width)) {
		Width = SpaceBefore(Src, Reading, Pos);
		if(!Width) break;
	}
	// 連続空白をスキップした位置の返戻
	return Pos;
}

/**
 * UTF-8 妥当性判定関数（３バイトのウィンドウでの記号照合が文字境界を跨がない事の前提を確かめる）
 * 計算量：テキスト長 B に対し O(B)
 * @param Text 判定対象
 * @return 全体が妥当な UTF-8 なら true
 */
bool TextEdit::IsValidUtf8(const std::string_view Text) {
	// デコード器の進めるコードポイント境界だけの走査
	// 不正な UTF-8 の並びが有る事の返戻
	for(size_t Pos = 0; Pos < Text.size();) if(char32_t Point = 0; !IsUtf8Decoded(Text, Pos, Point)) return false;
	// 全体が妥当な UTF-8 である事の返戻
	return true;
}

/**
 * UTF-8 の１コードポイントエンコーディング関数
 * 計算量：O(1)（コードポイント毎の固定長の出力）
 * @param Point コードポイント
 * @param Dst 追記先
 */
void TextEdit::EncodeUtf8(const char32_t Point, std::string &Dst) {
	// ASCII の先頭マーカーを伴わない１バイト追記
	if(Point < 0X80) {
		Dst += static_cast<char>(Point);
		// １バイトエンコーディングを終えた場合の返戻
		return;
	}
	// エンコーディングに要する後続バイト数
	const uint32_t Follow = Point < 0X800 ? 1 : Point < 0X10000 ? 2 : 3;
	static constexpr uint32_t LeadMark[] = { 0, 0XC0, 0XE0, 0XF0 };
	// 上位側から６ビットずつ切り出し，先頭と後続のマーカーを組み立てる
	Dst += static_cast<char>(LeadMark[Follow] | Point >> 6 * Follow);
	for(int Part = static_cast<int>(Follow) - 1; Part > -1; --Part) Dst += static_cast<char>(0X80 | Point >> 6 * Part & 0X3F);
	// 終了
	return;
}

/**
 * 原文に無い制御文字の選択関数
 * 解析の為に原文へ置くマーカーに使い，戻す時に原文のテキストと取り違えない様にする（改行・空白類と NUL は除く）
 * 計算量：テキスト長 B に対し O(B)
 * @param Text 原文
 * @return 原文に無い制御文字（候補を全て含む場合は '\0'）
 */
unsigned char TextEdit::AbsentControlChar(const std::string_view Text) {
	// 制御文字の出現表の初期化
	bool IsPresent[0X20] = {};
	// 本文の大きさに依らない候補領域だけの固定探索
	for(const unsigned char Char : Text) if(Char < 0X20) IsPresent[Char] = true;
	for(unsigned char Char = 1; Char < 0X20; ++Char) if(!IsPresent[Char] && (Char < '\t' || Char > '\r')) {
		// 原文に無く改行・空白類 (`\t` ~ `\r`) でもない制御文字の返戻
		return static_cast<char>(Char);
	}
	// 候補を選べない事の返戻
	return '\0';
}

/**
 * テキスト編集の追加関数（空でない場合のみ）
 * 計算量：置換のテキスト長 L に対し O(L)，編集列への追加は償却 O(1)
 * @param Start 置換範囲の開始位置
 * @param End 置換範囲の終了位置
 * @param Text 置換後のテキスト
 * @param Edits 編集の格納先
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void TextEdit::Push(const uint32_t Start, const uint32_t End, std::string Text, std::vector<TextEdit> &Edits) {
	// 適用結果を変えず重複判定だけを増やす零幅空編集の除外
	if(Start < End || !Text.empty()) Edits.push_back({ .StartPos = Start, .EndPos = End, .NewText = std::move(Text) });
	// 終了
	return;
}

/**
 * 範囲 [Lower, Pos) 右端から TrimChars 文字の左方向スキップ関数
 * 計算量：走査するテキスト長 B と許容文字数 K に対し O(B K)
 * @param Src 元のソースコード
 * @param Pos 開始位置（此の直前から左方向にスキップ）
 * @param Lower 下限境界（此れより左には進まない）
 * @param TrimChars スキップ対象文字集合（例 ` \t\n` ` \t\n,`）
 * @return スキップ後の位置
 */
uint32_t TextEdit::SkipCharsLeftBounded(
	const std::string_view Src,
	uint32_t Pos,
	const uint32_t Lower,
	const std::string_view TrimChars
) {
	// 下限迄の対象文字の読飛
	while(Pos > Lower && TrimChars.find(Src[Pos - 1]) != std::string_view::npos) --Pos;
	// 連続切詰対象文字をスキップした位置の返戻
	return Pos;
}

/**
 * 同じ行の前に空白以外の字が在るかの判定関数（行の前へ移す独立コメントを其の行の後へ付けない為）
 * 計算量：前のテキスト長 B と空白の記法の総長 S に対し O(B S)
 * @param Src 元のソースコード
 * @param Reading 文法の読取規則
 * @param Pos 判定する位置
 * @return 行頭から位置迄に空白以外の字が在れば true
 */
bool TextEdit::IsAfterCode(const std::string_view Src, const ReadingRules &Reading, const uint32_t Pos) {
	// 空白を飛ばした位置の前の字
	const uint32_t Before = SkipSpaceLeft(Src, Reading, Pos);
	// 行頭でない事の返戻
	return Before && Src[Before - 1] != '\n';
}

/**
 * 位置から始まるトークン間の空白又は改行の長さの取得関数
 * 計算量：走査するテキスト長 B と空白の記法の総長 S に対し O(B S)
 * @param Src 元のソースコード
 * @param Reading 文法の読取規則
 * @param Pos 判定位置
 * @return 読み飛ばす字の UTF-8 の長さ（該当しなければ 0）
 */
size_t TextEdit::IntertokenSpaceAt(const std::string_view Src, const ReadingRules &Reading, const uint32_t Pos) {
	// 範囲外の位置の返戻
	if(Pos >= Src.size()) return 0;
	// 改行の長さの返戻
	if(Src[Pos] == '\n' || Src[Pos] == '\r') return 1;
	// 行内空白の長さの返戻
	if(const size_t Width = SpaceAt(Src, Reading, Pos)) return Width;
	// 其の他の ASCII の字の返戻
	if(static_cast<unsigned char>(Src[Pos]) < 0X80) return 0;
	for(const std::string_view Separator : Reading.UnicodeLineSeparators) if(Src.substr(Pos).starts_with(Separator)) {
		// 文法が改行と読む Unicode の区切の長さの返戻
		return Separator.size();
	}
	// 空白でも改行でもない事の返戻
	return 0;
}

/**
 * 指定位置から右方向への行内空白のスキップ関数
 * 計算量：走査するテキスト長 B と空白の記法の総長 S に対し O(B S)
 * @param Src 元のソースコード
 * @param Reading 文法の読取規則
 * @param Pos 開始位置
 * @return スキップ後の位置
 */
uint32_t TextEdit::SkipSpaceRight(const std::string_view Src, const ReadingRules &Reading, uint32_t Pos) {
	for(size_t Width = 0; Pos < Src.size(); Pos += static_cast<uint32_t>(Width)) {
		Width = SpaceAt(Src, Reading, Pos);
		if(!Width) break;
	}
	// 連続空白をスキップした位置の返戻
	return Pos;
}

/**
 * 前置区切の範囲とタブの数の取得関数（行末コメントの前置区切，整形仕様 7.1）
 * 位置の前の空白（言語が空白と読む全角スペース等を含む）の並びを遡る
 * 計算量：走査するテキスト長 B と空白の記法の総長 S に対し O(B S)
 * @param Src 元のソースコード
 * @param Reading 文法の読取規則
 * @param Pos 区切の終わり（コメントの開始記号の位置）
 * @return 区切の始まりと其の中の水平タブの数
 */
std::pair<uint32_t, size_t> TextEdit::SeparatorBefore(
	const std::string_view Src,
	const ReadingRules &Reading,
	const uint32_t Pos
) {
	// 空白を除いた境界の取得
	const uint32_t Start = SkipSpaceLeft(Src, Reading, Pos);
	// 区切の始まりとタブの数の返戻
	return { Start, static_cast<size_t>(std::count(Src.begin() + Start, Src.begin() + Pos, '\t')) };
}

/**
 * Pos を含む行の行頭位置（直前の改行の次）の取得関数
 * 計算量：前の行のテキスト長 B に対し O(B)
 * @param Src 元のソースコード
 * @param Pos 基準位置
 * @return 行頭のバイト位置
 */
uint32_t TextEdit::GetLineStartOf(const std::string_view Src, uint32_t Pos) {
	// 直前の改行迄の後退
	while(Pos && Src[Pos - 1] != '\n') --Pos;
	// 行頭位置の返戻
	return Pos;
}

/**
 * ビュー前後の空白／タブ／改行の切詰関数
 * 計算量：テキスト長 B に対し O(B)
 * @param View 処理対象のビュー（其の場で更新する）
 */
void TextEdit::TrimView(std::string_view &View) {
	// 所有文字列を動かさず，両端の表示範囲だけを狭める
	while(!View.empty() && (View.front() == ' ' || View.front() == '\t' || View.front() == '\n')) View.remove_prefix(1);
	while(!View.empty() && (View.back() == ' ' || View.back() == '\t' || View.back() == '\n')) View.remove_suffix(1);
	// 終了
	return;
}

/**
 * 収集した編集のソースへの適用関数
 * 座標走査は O(1) 領域
 * 計算量：編集数 E，入力と置換文字列の総バイト数 B，木の高さ H に対して時間 O(E log E + B + H)，追加領域 O(B + E)
 * @param LocalSrc 適用対象のソースコード（結果で上書きされる）
 * @param Edits 適用する編集一覧
 * @param ShouldTrackParserBreaks 解析だけに用いる文の区切（Kotlin の `;` と改行を含むブロックコメントの後の改行）の挿入を記録するか
 * @param NewGroups 今回の編集で丸括弧を挿入する解析用の囲みの元の本文位置の組の昇順の列（重複無，同じ開きでは終わりの昇順，無ければ nullptr）
 * @throws FormatLimitExceeded 編集対象の解析が期限を超えた場合
 * @throws std::bad_alloc 編集後の本文又は対応位置の確保に失敗した場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void TextEdit::Apply(
	ParsedTree &LocalSrc,
	std::vector<TextEdit> &Edits,
	const bool ShouldTrackParserBreaks,
	const std::vector<std::pair<uint32_t, uint32_t>> *const NewGroups
) {
	// 編集が無い場合の返戻
	if(Edits.empty()) return;
	// 開始位置順，同位置は零幅挿入優先，同種は終端降順に整列（Ruby の `return ` 挿入と引用符置換等を両立）
	std::stable_sort(
		Edits.begin(),
		Edits.end(),
		[](const TextEdit &Lhs, const TextEdit &Rhs) -> bool {
			// 開始位置の順の比較の返戻
			if(Lhs.StartPos != Rhs.StartPos) return Lhs.StartPos < Rhs.StartPos;
			// 挿入を置換より先にする比較の返戻
			// 条件に該当する値の返戻
			if(const bool IsLhsInsert = Lhs.StartPos == Lhs.EndPos; IsLhsInsert != (Rhs.StartPos == Rhs.EndPos)) return IsLhsInsert;
			// 同種では範囲広い側（EndPos 大）を先に処理の返戻
			return Lhs.EndPos > Rhs.EndPos;
		}
	);
	const std::string &Src = LocalSrc;
	TSTree *const Tree = LocalSrc.GetMutableTree();
	// 編集の座標は非減少の為，求めた位置迄の改行だけを単調に辿る（全行の位置表は保持しない）
	uint32_t MonoRow = 0, LineStart = 0;
	std::optional<size_t> NextNewline;
	// バイト位置の行・桁への変換
	const auto PointAt = [&Src, &MonoRow, &LineStart, &NextNewline](const uint32_t Byte) -> TSPoint {
		// 指定バイト位置の行列位置の算出
		if(!NextNewline) NextNewline = Src.find('\n');
		while(*NextNewline < Byte) {
			++MonoRow;
			LineStart = static_cast<uint32_t>(*NextNewline + 1);
			NextNewline = Src.find('\n', LineStart);
		}
		// 行先頭からのバイト距離を列位置とする座標の返戻
		return TSPoint{ MonoRow, Byte - LineStart };
	};
	std::string Result;
	Result.reserve(LocalSrc.size());
	uint32_t LocalPos = 0;
	// 旧木へ通知する一括区間の始まりと終わり
	std::optional<TSPoint> FirstPoint;
	TSPoint LastPoint {};
	// 解析用の区切の位置を本文の編集と同じ座標で移す為の受理済編集
	struct AppliedRange {
		uint32_t Start;
		uint32_t End;
		uint32_t NewSize;
	};
	std::vector<AppliedRange> AppliedRanges;
	const bool HasSrcMarkers =
	!LocalSrc.GetInsertedBreaks().empty() || !LocalSrc.GetSyntheticGroups().empty() || !LocalSrc.GetLexemeRestorations().empty();
	// 修復前の本文の位置へ戻す為の編集の組
	std::vector<AppliedEdit> Batch;
	std::vector<uint32_t> NewBreaks;
	bool IsAppliedAny = false;
	// 受理可能な編集の原文順での適用
	for(const TextEdit &Edit : Edits) {
		// 先に受理した編集を優先する不正範囲・空操作の除外
		if(
			Edit.StartPos < LocalPos || Edit.StartPos > Edit.EndPos || Edit.EndPos > LocalSrc.size() ||
			Edit.StartPos == Edit.EndPos && Edit.NewText.empty() ||
			std::string_view(LocalSrc.data() + Edit.StartPos, Edit.EndPos - Edit.StartPos) == Edit.NewText
		) continue;
		// 通知の両端だけを保持し，原文座標を前進する
		if(Tree) {
			if(!FirstPoint) FirstPoint = PointAt(Edit.StartPos);
			LastPoint = PointAt(Edit.EndPos);
		}
		// 編集本文と位置の対応の記録
		Result.append(LocalSrc.data() + LocalPos, Edit.StartPos - LocalPos);
		if(
			ShouldTrackParserBreaks && Edit.StartPos == Edit.EndPos &&
			(LocalSrc.GetProfile().IsSpelling(Edit.NewText, Core::Word::StmtEnd) || Edit.NewText == "\n")
		) NewBreaks.push_back(static_cast<uint32_t>(Result.size()));
		Batch.push_back(
			{ Edit.StartPos, Edit.EndPos, static_cast<uint32_t>(Result.size()), static_cast<uint32_t>(Result.size() + Edit.NewText.size()) }
		);
		Result.append(Edit.NewText);
		if(HasSrcMarkers) AppliedRanges.push_back({ Edit.StartPos, Edit.EndPos, static_cast<uint32_t>(Edit.NewText.size()) });
		LocalPos = Edit.EndPos;
		IsAppliedAny = true;
	}
	// 編集を１つも適用しなかった場合の終了（先頭への零幅挿入は LocalPos が０の儘の為，IsAppliedAny で末尾本文の消失を防止）
	if(!IsAppliedAny) return;
	// 原文の未編集部分の連結と解析用区切の移動
	Result.append(LocalSrc.data() + LocalPos, LocalSrc.size() - LocalPos);
	std::vector<uint32_t> Shifted;
	if(!AppliedRanges.empty() || !NewBreaks.empty()) {
		Shifted.reserve(LocalSrc.GetInsertedBreaks().size() + NewBreaks.size());
		size_t EditIdx = 0;
		int64_t Offset = 0;
		// 既存の解析用区切の位置の更新
		for(const uint32_t Marker : LocalSrc.GetInsertedBreaks()) {
			while(EditIdx < AppliedRanges.size() && AppliedRanges[EditIdx].End <= Marker) {
				const AppliedRange &Range = AppliedRanges[EditIdx++];
				Offset += static_cast<int64_t>(Range.NewSize) - (Range.End - Range.Start);
			}
			if(EditIdx < AppliedRanges.size() && AppliedRanges[EditIdx].Start <= Marker && Marker < AppliedRanges[EditIdx].End) continue;
			Shifted.push_back(static_cast<uint32_t>(static_cast<int64_t>(Marker) + Offset));
		}
		Shifted.insert(Shifted.end(), NewBreaks.begin(), NewBreaks.end());
		std::sort(Shifted.begin(), Shifted.end());
		Shifted.erase(std::unique(Shifted.begin(), Shifted.end()), Shifted.end());
	}
	// 既存の解析用の囲みの位置を受理した編集に合わせて移し，片側が置換された組は失う
	std::vector<std::pair<uint32_t, uint32_t>> ShiftedGroups;
	ShiftedGroups.reserve(LocalSrc.GetSyntheticGroups().size() + (NewGroups ? NewGroups->size() : 0));
	// 受理した編集の長さの差の累積（編集の終わりは原文順に単調な為，位置の移動を二分探索で求める）
	std::vector<int64_t> Offsets(AppliedRanges.size() + 1);
	for(size_t Idx = 0; Idx < AppliedRanges.size(); ++Idx) {
		const AppliedRange &Range = AppliedRanges[Idx];
		Offsets[Idx + 1] = Offsets[Idx] + static_cast<int64_t>(Range.NewSize) - (Range.End - Range.Start);
	}
	// 編集後の位置の移動
	const auto ShiftPos =
	[&AppliedRanges, &Offsets](const uint32_t Pos, const bool IsEndBoundary = false) -> std::optional<uint32_t> {
		// 編集位置迄に適用した区間の計数
		size_t Count = static_cast<size_t>(
			std::ranges::upper_bound(AppliedRanges, Pos, std::less<uint32_t>(), &AppliedRange::End) - AppliedRanges.begin()
		);
		// 置換で失われたマーカーの返戻
		if(Count < AppliedRanges.size() && AppliedRanges[Count].Start <= Pos) return std::nullopt;
		// 終わりの境界は，其の位置への零幅の挿入より前に留める
		while(IsEndBoundary && Count && AppliedRanges[Count - 1].Start == Pos && AppliedRanges[Count - 1].End == Pos) --Count;
		// 現在の本文に於ける位置の返戻
		return static_cast<uint32_t>(static_cast<int64_t>(Pos) + Offsets[Count]);
	};
	for(const std::pair<uint32_t, uint32_t> &Group : LocalSrc.GetSyntheticGroups()) {
		if(const std::optional<uint32_t> NewOpen = ShiftPos(Group.first), NewClose = ShiftPos(Group.second); NewOpen && NewClose) {
			ShiftedGroups.emplace_back(*NewOpen, *NewClose);
		}
	}
	// 今回挿入した囲みは受理済編集の新位置から直接対応付ける
	if(NewGroups) {
		using BoundaryPositions = std::unordered_map<uint32_t, std::vector<uint32_t>>;
		BoundaryPositions Openings, Closings;
		for(const AppliedEdit &Applied : Batch) {
			if(Applied.Start == Applied.End && Applied.NewEnd == Applied.NewStart + 1) switch(Result[Applied.NewStart]) {
			case '(':
				Openings[Applied.Start].push_back(Applied.NewStart);
				break;
			case ')':
				Closings[Applied.Start].push_back(Applied.NewStart);
				break;
			}
		}
		// 同じ開きでは内側，同じ閉じでは外側の領域から対応付ける
		for(const std::pair<uint32_t, uint32_t> &Group : *NewGroups) {
			std::vector<uint32_t> &Open = Openings[Group.first], &Close = Closings[Group.second];
			ShiftedGroups.emplace_back(Open.back(), Close.back());
			Open.pop_back();
			Close.pop_back();
		}
	}
	// 投影が開きの位置で引ける様に開きの順へ整列
	std::ranges::sort(ShiftedGroups);
	// トークンの復元位置の移動
	std::vector<LexemeRestoration> ShiftedRestorations;
	ShiftedRestorations.reserve(LocalSrc.GetLexemeRestorations().size());
	for(const LexemeRestoration &Restoration : LocalSrc.GetLexemeRestorations()) {
		if(const std::optional<uint32_t> Start = ShiftPos(Restoration.Start), End = ShiftPos(Restoration.End, true); Start && End) {
			ShiftedRestorations.push_back({ *Start, *End, Restoration.Original, Restoration.HasKindRestoration, Restoration.IsIdentifier });
		}
	}
	// 件数に依らず旧木を保ち，変更した区間の合成座標を１度だけ通知する
	if(Tree) {
		TSPoint NewEnd = *FirstPoint;
		for(uint32_t Pos = Batch.front().NewStart; Pos < Batch.back().NewEnd; ++Pos) {
			if(Result[Pos] == '\n') {
				++NewEnd.row;
				NewEnd.column = 0;
			} else ++NewEnd.column;
		}
		const TSInputEdit Change {
			.start_byte = Batch.front().Start,
			.old_end_byte = Batch.back().End,
			.new_end_byte = Batch.back().NewEnd,
			.start_point = *FirstPoint,
			.old_end_point = LastPoint,
			.new_end_point = NewEnd
		};
		ts_tree_edit(Tree, &Change);
		LocalSrc.MarkTreeEdited();
	}
	// 受理した編集に対応する解析用の区切の位置と，修復前の本文の位置へ戻す編集の組を保つ差替
	LocalSrc.AssignEdited(
		std::move(Result),
		std::move(Shifted),
		std::move(ShiftedGroups),
		std::move(ShiftedRestorations),
		std::move(Batch)
	);
	// 終了
	return;
}
