#include "TextEdit.hpp"
#include "ParsedTree.hpp"
#include "../Profile/ReadingRules.hpp"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * UTF-8 の１符号点復号関数
 * @param Text 対象文字列
 * @param Pos 開始位置（成功時は次の符号点へ進む）
 * @param Point 復号結果（失敗時は変更しない）
 * @return 最短形の Unicode スカラー値を復号出来れば true
 */
bool TextEdit::DecodeUtf8(const std::string_view Text, size_t &Pos, uint32_t &Point) {
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
	uint32_t Value = Lead & LeadMask[Follow];
	for(size_t Index = 1; Index <= Follow; ++Index) {
		const unsigned char Continuation = static_cast<unsigned char>(Text[Pos + Index]);
		// 続きのバイトとして不正な事の返戻
		if((Continuation & 0XC0) != 0X80) return false;
		Value = Value << 6 | Continuation & 0X3F;
	}
	// 後続バイト数別の最小符号点
	static constexpr uint32_t LeastByFollow[] = { 0, 0X80, 0X800, 0X10000 };
	// 同じ符号点の複数表現を拒む最短形の強制
	// 冗長な符号化・範囲外・サロゲートの事の返戻
	if(Value < LeastByFollow[Follow] || Value > 0X10FFFF || Value > 0XD7FF && Value < 0XE000) return false;
	Pos += Follow + 1;
	Point = Value;
	// 復号成功の返戻
	return true;
}

/**
 * UTF-8 妥当性判定関数（３バイト窓での記号照合が文字境界を跨がない事の前提を確かめる）
 * @param Text 判定対象
 * @return 全体が妥当な UTF-8 なら true
 */
bool TextEdit::IsValidUtf8(const std::string_view Text) {
	// 復号器の進める符号点境界だけの走査
	for(size_t Pos = 0; Pos < Text.size();) {
		uint32_t Point = 0;
		// 不正な UTF-8 の並びが有る事の返戻
		if(!DecodeUtf8(Text, Pos, Point)) return false;
	}
	// 全体が妥当な UTF-8 である事の返戻
	return true;
}

/**
 * UTF-8 の１符号点符号化関数
 * @param Point 符号点
 * @param Dest 追記先
 */
void TextEdit::EncodeUtf8(const uint32_t Point, std::string &Dest) {
	// ASCII の先頭印を伴わない１バイト追記
	if(Point < 0X80) {
		Dest += static_cast<char>(Point);
		// 終了
		return;
	}
	// 符号化に要する後続バイト数
	const uint32_t Follow = Point < 0X800 ? 1 : Point < 0X10000 ? 2 : 3;
	static constexpr uint32_t LeadMark[] = { 0, 0XC0, 0XE0, 0XF0 };
	// 上位側から６ビットずつ切り出し，先頭と後続の印を組み立てる
	Dest += static_cast<char>(LeadMark[Follow] | Point >> 6 * Follow);
	for(int Part = static_cast<int>(Follow) - 1; Part > -1; --Part) Dest += static_cast<char>(0X80 | Point >> 6 * Part & 0X3F);
	// 終了
	return;
}

/**
 * 原文に無い制御文字の選択関数
 * 解析の為に原文へ置く印に使い，戻す時に原文の字面と取り違えない様にする（改行・空白類と NUL は除く）
 * @param Text 原文
 * @return 原文に無い制御文字（候補を全て含む場合は '\0'）
 */
char TextEdit::AbsentControlChar(const std::string_view Text) {
	// 制御文字の出現表の初期化
	bool IsPresent[0X20] = {};
	// 本文の大きさに依らない候補領域だけの固定探索
	for(const char Char : Text) if(static_cast<unsigned char>(Char) < 0X20) IsPresent[static_cast<unsigned char>(Char)] = true;
	for(unsigned char Char = 1; Char < 0X20; ++Char) {
		// 原文に無く改行・空白類 (`\t` ~ `\r`) でもない制御文字の返戻
		if(!IsPresent[Char] && (Char < '\t' || Char > '\r')) return static_cast<char>(Char);
	}
	// 候補を選べない事の返戻
	return '\0';
}

/**
 * テキスト編集の追加関数（空でない場合のみ）
 * @param Start 置換範囲の開始位置
 * @param End 置換範囲の終了位置
 * @param Text 置換後のテキスト
 * @param Edits 編集の格納先
 */
void TextEdit::Push(const uint32_t Start, const uint32_t End, std::string Text, std::vector<TextEdit> &Edits) {
	// 適用結果を変えず重複判定だけを増やす零幅空編集の除外
	if(Start < End || !Text.empty()) Edits.push_back({ .StartPos = Start, .EndPos = End, .NewText = std::move(Text) });
	// 終了
	return;
}

/**
 * 範囲 [Lower, Pos) 右端から TrimChars 文字の左方向スキップ関数
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
 * 位置から始まる行内空白の長さの取得関数
 * @param Src 元のソースコード
 * @param Pos 空白の始まりの位置
 * @param Reading 文法の読取規則
 * @return 空白の字の UTF-8 の長さ（空白で無ければ 0）
 */
size_t TextEdit::SpaceAt(const std::string_view Src, const uint32_t Pos, const ReadingRules &Reading) {
	// 範囲外の位置の返戻
	if(Pos >= Src.size()) return 0;
	const unsigned char Lead = static_cast<unsigned char>(Src[Pos]);
	// ASCII の字の空白の長さの返戻
	if(Lead < 0X80) return Reading.IsSpace(Lead) ? 1 : 0;
	size_t End = Pos;
	uint32_t Point = 0;
	// 復号した符号位置の空白の長さの返戻
	return DecodeUtf8(Src, End, Point) && Reading.IsSpace(Point) ? End - Pos : 0;
}

/**
 * 位置で終わる行内空白の長さの取得関数
 * @param Src 元のソースコード
 * @param Pos 空白の終わりの位置
 * @param Reading 文法の読取規則
 * @return 空白の字の UTF-8 の長さ（空白で無ければ 0）
 */
size_t TextEdit::SpaceBefore(const std::string_view Src, const uint32_t Pos, const ReadingRules &Reading) {
	// UTF-8 の１〜３バイトの空白の字で終わる場合の其の長さの返戻
	for(size_t Width = 1; Width < 4 && Width <= Pos; ++Width) if(SpaceAt(Src, Pos - Width, Reading) == Width) return Width;
	// 空白で終わらない事の返戻
	return 0;
}

/**
 * 指定位置から左方向への行内空白のスキップ関数
 * @param Src 元のソースコード
 * @param Pos 開始位置
 * @param Reading 文法の読取規則
 * @return スキップ後の位置
 */
uint32_t TextEdit::SkipSpLeft(const std::string_view Src, uint32_t Pos, const ReadingRules &Reading) {
	for(size_t Width = 0; Pos; Pos -= static_cast<uint32_t>(Width)) {
		Width = SpaceBefore(Src, Pos, Reading);
		if(!Width) break;
	}
	// 連続空白をスキップした位置の返戻
	return Pos;
}

/**
 * 空の本体を開く位置の取得関数
 * 位置迄の字面が空白だけを挟んだ `{}` で終わるなら其の `{` の位置を返す（畳んで空に為った本体の判定に使う）
 * @param Src 元のソースコード
 * @param End 範囲の終わりの位置
 * @param Reading 文法の読取規則
 * @return 開き波括弧の位置（空の本体で終わらなければ UINT32_MAX）
 */
uint32_t TextEdit::EmptyBodyOpener(const std::string_view Src, const uint32_t End, const ReadingRules &Reading) {
	// 閉じ波括弧で終わらない範囲の返戻
	if(!End || End > Src.size() || Src[End - 1] != '}') return std::numeric_limits<uint32_t>::max();
	// 閉じ波括弧の前の空白でない位置
	uint32_t Open = End - 1;
	while(Open) {
		if(Src[Open - 1] == '\r' || Src[Open - 1] == '\n') --Open;
		else if(const size_t Width = SpaceBefore(Src, Open, Reading)) Open -= static_cast<uint32_t>(Width);
		else break;
	}
	// 開き波括弧の位置の返戻
	return Open && Src[Open - 1] == '{' ? Open - 1 : std::numeric_limits<uint32_t>::max();
}

/**
 * 空の本体で終わるかの判定関数
 * @param Src 元のソースコード
 * @param End 範囲の終わりの位置
 * @param Reading 文法の読取規則
 * @return 空白だけを挟んだ `{}` で終われば true
 */
bool TextEdit::EndsWithEmptyBody(const std::string_view Src, const uint32_t End, const ReadingRules &Reading) {
	// 空の本体を開く位置の有無の返戻
	return EmptyBodyOpener(Src, End, Reading) != std::numeric_limits<uint32_t>::max();
}

/**
 * 同じ行の前に空白以外の字が在るかの判定関数（行の前へ移す独立コメントを其の行の後へ付けない為）
 * @param Src 元のソースコード
 * @param Pos 判定する位置
 * @param Reading 文法の読取規則
 * @return 行頭から位置迄に空白以外の字が在れば true
 */
bool TextEdit::IsAfterCode(const std::string_view Src, const uint32_t Pos, const ReadingRules &Reading) {
	// 空白を飛ばした位置の前の字
	const uint32_t Before = SkipSpLeft(Src, Pos, Reading);
	// 行頭でない事の返戻
	return Before && Src[Before - 1] != '\n';
}

/**
 * 位置から始まる字句間の空白又は改行の長さの取得関数
 * @param Src 元のソースコード
 * @param Pos 判定位置
 * @param Reading 文法の読取規則
 * @return 読飛ばす字の UTF-8 の長さ（該当しなければ 0）
 */
size_t TextEdit::IntertokenSpaceAt(const std::string_view Src, const uint32_t Pos, const ReadingRules &Reading) {
	// 範囲外の位置の返戻
	if(Pos >= Src.size()) return 0;
	// 改行の長さの返戻
	if(Src[Pos] == '\n' || Src[Pos] == '\r') return 1;
	// 行内空白の長さの返戻
	if(const size_t Width = SpaceAt(Src, Pos, Reading)) return Width;
	// 其の他の ASCII の字の返戻
	if(static_cast<unsigned char>(Src[Pos]) < 0X80) return 0;
	for(const std::string_view Separator : Reading.UnicodeLineSeparators) {
		// 文法が改行と読む Unicode の区切りの長さの返戻
		if(Src.substr(Pos).starts_with(Separator)) return Separator.size();
	}
	// 空白でも改行でも無い事の返戻
	return 0;
}

/**
 * 指定位置から右方向への行内空白のスキップ関数
 * @param Src 元のソースコード
 * @param Pos 開始位置
 * @param Reading 文法の読取規則
 * @return スキップ後の位置
 */
uint32_t TextEdit::SkipSpRight(const std::string_view Src, uint32_t Pos, const ReadingRules &Reading) {
	for(size_t Width = 0; Pos < Src.size(); Pos += static_cast<uint32_t>(Width)) {
		Width = SpaceAt(Src, Pos, Reading);
		if(!Width) break;
	}
	// 連続空白をスキップした位置の返戻
	return Pos;
}

/**
 * 前置区切りの範囲とタブの数の取得関数（行末コメントの前置区切り，整形仕様 7.1）
 * 位置の前の空白（言語が空白と読む全角スペース等を含む）の並びを遡る
 * @param Src 元のソースコード
 * @param Pos 区切りの終わり（コメントの開始記号の位置）
 * @param Reading 文法の読取規則
 * @return 区切りの始まりと其の中の水平タブの数
 */
std::pair<uint32_t, size_t> TextEdit::SeparatorBefore(const std::string_view Src, uint32_t Pos, const ReadingRules &Reading) {
	const uint32_t Start = SkipSpLeft(Src, Pos, Reading);
	// 区切りの始まりとタブの数の返戻
	return { Start, static_cast<size_t>(std::count(Src.begin() + Start, Src.begin() + Pos, '\t')) };
}

/**
 * Pos を含む行の行頭位置（直前の改行の次）の取得関数
 * @param Src 元のソースコード
 * @param Pos 基準位置
 * @return 行頭のバイト位置
 */
uint32_t TextEdit::LineStartOf(const std::string_view Src, uint32_t Pos) {
	// 直前の改行迄の後退
	while(Pos && Src[Pos - 1] != '\n') --Pos;
	// 行頭位置の返戻
	return Pos;
}

/**
 * ビュー前後の空白／タブ／改行の切詰関数
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
 * 編集数 E，入力と置換文字列の総バイト数 B，木の高さ H に対して時間 O(E log E + B + E H)，追加領域 O(B + E)
 * 座標走査は O(1) 領域
 * @param Src 適用対象のソースコード（結果で上書きされる）
 * @param Edits 適用する編集一覧
 * @param TracksParserBreaks 解析だけに用いる文の区切（Kotlin の `;` と改行を含む囲みコメントの後の改行）の挿入を記録するか
 * @param NewGroups 今回の編集で丸括弧を挿入する解析用の囲みの元の本文位置の組の列（無ければ nullptr）
 */
void TextEdit::Apply(
	ParsedTree &Src,
	std::vector<TextEdit> &Edits,
	const bool TracksParserBreaks,
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
			if(const bool IsLhsInsert = Lhs.StartPos == Lhs.EndPos; IsLhsInsert != (Rhs.StartPos == Rhs.EndPos)) return IsLhsInsert;
			// 同種では範囲広い側（EndPos 大）を先に処理の返戻
			return Lhs.EndPos > Rhs.EndPos;
		}
	);
	const std::string &Source = Src;
	// 編集数×木の深さの二乗化を避け，閾値超過は旧木を使わない全体再解析へ切替
	static constexpr size_t IncrementalEditLimit = 64;
	TSTree *const Tree = Edits.size() <= IncrementalEditLimit ? Src.GetMutableTree() : nullptr;
	// 増分通知を行わない適用は旧木を陳腐化させる為，既往の編集済の印も併せて降ろす
	if(!Tree) Src.SetTreeEdited(false);
	// 編集の座標は非減少の為，求めた位置迄の改行だけを単調に辿る（全行の位置表は保持しない）
	uint32_t MonoRow = 0, LineStart = 0;
	std::optional<size_t> NextNewline;
	const auto PointAt = [&Source, &MonoRow, &LineStart, &NextNewline](const uint32_t Byte) -> TSPoint {
		// 指定バイト位置の行列位置の算出
		if(!NextNewline) NextNewline = Source.find('\n');
		while(*NextNewline < Byte) {
			++MonoRow;
			LineStart = static_cast<uint32_t>(*NextNewline + 1);
			NextNewline = Source.find('\n', LineStart);
		}
		// 行先頭からのバイト距離を列位置とする座標の返戻
		return TSPoint{ MonoRow, Byte - LineStart };
	};
	std::string Result;
	Result.reserve(Src.size());
	uint32_t Pos = 0;
	// 原文座標の編集通知を末尾から適用する
	std::vector<TSInputEdit> TreeEdits;
	// 解析用の区切りの位置を本文の編集と同じ座標で移す為の受理済編集
	struct AppliedRange {
		uint32_t Start;
		uint32_t End;
		uint32_t NewSize;
	};
	std::vector<AppliedRange> AppliedRanges;
	const bool HasSourceMarkers =
	!Src.GetInsertedBreaks().empty() || !Src.GetSyntheticGroups().empty() || !Src.GetLexemeRestorations().empty();
	// 補修前の本文の位置へ戻す為の編集の組
	std::vector<AppliedEdit> Batch;
	std::vector<uint32_t> NewBreaks;
	bool IsAppliedAny = false;
	// 受理可能な編集の原文順での適用
	for(const TextEdit &Edit : Edits) {
		// 先に受理した編集を優先する不正範囲・空操作の除外
		if(
			Edit.StartPos < Pos || Edit.StartPos > Edit.EndPos || Edit.EndPos > Src.size() ||
			Edit.StartPos == Edit.EndPos && Edit.NewText.empty()
		) continue;
		if(const std::string_view Cur(Src.data() + Edit.StartPos, Edit.EndPos - Edit.StartPos); Cur == Edit.NewText) continue;
		// 後続の行・列位置との混在を避ける受理済編集の原文座標保持
		if(Tree) {
			const TSPoint StartPoint = PointAt(Edit.StartPos);
			// NewEndPoint は編集後のテキスト末尾 NewText 内の改行数を加算
			uint32_t NewRows = 0, LastNewlineIdx = 0;
			for(uint32_t Idx = 0; Idx < Edit.NewText.size(); ++Idx) if(Edit.NewText[Idx] == '\n') {
				++NewRows;
				LastNewlineIdx = Idx;
			}
			const TSPoint NewEndPoint = NewRows ?
			TSPoint{ StartPoint.row + NewRows, static_cast<uint32_t>(Edit.NewText.size() - LastNewlineIdx - 1) } :
			TSPoint{ StartPoint.row, StartPoint.column + static_cast<uint32_t>(Edit.NewText.size()) };
			TreeEdits.push_back(
				{
					.start_byte = Edit.StartPos,
					.old_end_byte = Edit.EndPos,
					.new_end_byte = Edit.StartPos + static_cast<uint32_t>(Edit.NewText.size()),
					.start_point = StartPoint,
					.old_end_point = PointAt(Edit.EndPos),
					.new_end_point = NewEndPoint
				}
			);
		}
		// 編集本文と位置の対応の記録
		Result.append(Src.data() + Pos, Edit.StartPos - Pos);
		if(
			TracksParserBreaks && Edit.StartPos == Edit.EndPos &&
			(Src.GetRules().IsSpelling(Edit.NewText, Core::Word::StatementEnd) || Edit.NewText == "\n")
		) NewBreaks.push_back(static_cast<uint32_t>(Result.size()));
		Batch.push_back(
			{ Edit.StartPos, Edit.EndPos, static_cast<uint32_t>(Result.size()), static_cast<uint32_t>(Result.size() + Edit.NewText.size()) }
		);
		Result.append(Edit.NewText);
		if(HasSourceMarkers) AppliedRanges.push_back({ Edit.StartPos, Edit.EndPos, static_cast<uint32_t>(Edit.NewText.size()) });
		Pos = Edit.EndPos;
		IsAppliedAny = true;
	}
	// 編集を１つも適用しなかった場合の終了（先頭への零幅挿入は Pos が０の儘の為，IsAppliedAny で末尾本文の消失を防止）
	if(!IsAppliedAny) return;
	// 原文の未編集部分の連結と解析用区切の移動
	Result.append(Src.data() + Pos, Src.size() - Pos);
	std::vector<uint32_t> Shifted;
	if(!AppliedRanges.empty() || !NewBreaks.empty()) {
		Shifted.reserve(Src.GetInsertedBreaks().size() + NewBreaks.size());
		size_t EditIndex = 0;
		int64_t Offset = 0;
		// 既存の解析用区切の位置の更新
		for(const uint32_t Marker : Src.GetInsertedBreaks()) {
			while(EditIndex < AppliedRanges.size() && AppliedRanges[EditIndex].End <= Marker) {
				const AppliedRange &Range = AppliedRanges[EditIndex++];
				Offset += static_cast<int64_t>(Range.NewSize) - (Range.End - Range.Start);
			}
			if(EditIndex < AppliedRanges.size() && AppliedRanges[EditIndex].Start <= Marker && Marker < AppliedRanges[EditIndex].End) {
				continue;
			}
			Shifted.push_back(static_cast<uint32_t>(static_cast<int64_t>(Marker) + Offset));
		}
		Shifted.insert(Shifted.end(), NewBreaks.begin(), NewBreaks.end());
		std::sort(Shifted.begin(), Shifted.end());
		Shifted.erase(std::unique(Shifted.begin(), Shifted.end()), Shifted.end());
	}
	// 既存の解析用の囲みの位置を受理した編集に合わせて移し，片側が置換された組は失う
	std::vector<std::pair<uint32_t, uint32_t>> ShiftedGroups;
	ShiftedGroups.reserve(Src.GetSyntheticGroups().size() + (NewGroups ? NewGroups->size() : 0));
	// 受理した編集の長さの差の累積（編集の終わりは原文順に単調な為，位置の移動を二分探索で求める）
	std::vector<int64_t> Offsets(AppliedRanges.size() + 1);
	for(size_t Index = 0; Index < AppliedRanges.size(); ++Index) {
		const AppliedRange &Range = AppliedRanges[Index];
		Offsets[Index + 1] = Offsets[Index] + static_cast<int64_t>(Range.NewSize) - (Range.End - Range.Start);
	}
	const auto ShiftPosition =
	[&AppliedRanges, &Offsets](const uint32_t Position, const bool IsEndBoundary = false) -> std::optional<uint32_t> {
		size_t Count = static_cast<size_t>(
			std::ranges::upper_bound(AppliedRanges, Position, std::less<uint32_t>(), &AppliedRange::End) - AppliedRanges.begin()
		);
		// 置換で失われた印の返戻
		if(Count < AppliedRanges.size() && AppliedRanges[Count].Start <= Position) return std::nullopt;
		// 終わりの境界は，其の位置への零幅の挿入より前に留める
		while(IsEndBoundary && Count && AppliedRanges[Count - 1].Start == Position && AppliedRanges[Count - 1].End == Position) --Count;
		// 現在の本文に於ける位置の返戻
		return static_cast<uint32_t>(static_cast<int64_t>(Position) + Offsets[Count]);
	};
	for(const auto &[Open, Close] : Src.GetSyntheticGroups()) {
		if(const std::optional<uint32_t> NewOpen = ShiftPosition(Open), NewClose = ShiftPosition(Close); NewOpen && NewClose) {
			ShiftedGroups.emplace_back(*NewOpen, *NewClose);
		}
	}
	// 今回挿入した囲みは受理済編集の新位置から直接対応付ける
	if(NewGroups) {
		const auto FindInsertion = [&Batch, &Result](const uint32_t Position, const char Character) -> std::optional<uint32_t> {
			for(const AppliedEdit &Applied : std::ranges::equal_range(Batch, Position, std::less<uint32_t>(), &AppliedEdit::Start)) {
				if(Applied.End == Position && Applied.NewEnd == Applied.NewStart + 1 && Result[Applied.NewStart] == Character) {
					// 其の位置へ挿入した１字の返戻
					return Applied.NewStart;
				}
			}
			// 対応する挿入の無い事の返戻
			return std::nullopt;
		};
		for(const auto &[Open, Close] : *NewGroups) if(
			const std::optional<uint32_t> NewOpen = FindInsertion(Open, '('), NewClose = FindInsertion(Close, ')');
			NewOpen && NewClose
		) ShiftedGroups.emplace_back(*NewOpen, *NewClose);
	}
	// 投影が開きの位置で引ける様に開きの順へ並べる
	std::ranges::sort(ShiftedGroups);
	// 字句の復元位置の移動
	std::vector<LexemeRestoration> ShiftedRestorations;
	ShiftedRestorations.reserve(Src.GetLexemeRestorations().size());
	for(const LexemeRestoration &Restoration : Src.GetLexemeRestorations()) if(
		const std::optional<uint32_t> Start = ShiftPosition(Restoration.Start), End = ShiftPosition(Restoration.End, true);
		Start && End
	) ShiftedRestorations.push_back({ *Start, *End, Restoration.Original });
	// 後方から通知すれば，未通知の編集のバイト・行・列は全て原文座標の儘である
	if(Tree) {
		for(std::vector<TSInputEdit>::reverse_iterator Iter = TreeEdits.rbegin(); Iter != TreeEdits.rend(); ++Iter) {
			ts_tree_edit(Tree, &*Iter);
		}
		Src.SetTreeEdited(true);
	}
	// 受理した編集に対応する解析用の区切の位置と，補修前の本文の位置へ戻す編集の組を保つ差替
	Src.AssignEdited(
		std::move(Result),
		std::move(Shifted),
		std::move(ShiftedGroups),
		std::move(ShiftedRestorations),
		std::move(Batch)
	);
	// 終了
	return;
}
