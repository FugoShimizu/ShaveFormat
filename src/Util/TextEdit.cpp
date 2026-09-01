#include "TextEdit.hpp"
#include <algorithm>
#include <optional>
#include <string_view>

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
	// 後続バイト数 0〜3 に対応する先頭バイトの値部分
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
 * UTF-8 の１符号点符号化関数
 * @param Dest 追記先
 * @param Point 符号点
 */
void TextEdit::EncodeUtf8(std::string &Dest, const uint32_t Point) {
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
		// 原文に無く改行・空白類 (`\t` 〜 `\r`) でもない制御文字の返戻
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
	// 下限迄の対象文字の読飛し
	while(Pos > Lower && TrimChars.find(Src[Pos - 1]) != std::string_view::npos) --Pos;
	// 連続切詰対象文字をスキップした位置の返戻
	return Pos;
}

/**
 * 指定位置から左方向へのスペース／タブのスキップ関数
 * @param Src 元のソースコード
 * @param Pos 開始位置
 * @return スキップ後の位置
 */
uint32_t TextEdit::SkipSpLeft(const std::string_view Src, const uint32_t Pos) {
	// 連続空白をスキップした位置の返戻
	return SkipCharsLeftBounded(Src, Pos, 0, " \t");
}

/**
 * 指定位置から右方向へのスペース／タブのスキップ関数
 * @param Src 元のソースコード
 * @param Pos 開始位置
 * @return スキップ後の位置
 */
uint32_t TextEdit::SkipSpRight(const std::string_view Src, uint32_t Pos) {
	// 右側の空白及びタブの読飛し
	while(Pos < Src.size() && (Src[Pos] == ' ' || Src[Pos] == '\t')) ++Pos;
	// 連続空白をスキップした位置の返戻
	return Pos;
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
 * 制御文字を除く可視コードポイントの計数関数
 * @param Text 対象テキスト
 * @return 可視文字数
 */
size_t TextEdit::ContentChars(const std::string_view Text) {
	// 可視 ASCII と UTF-8 先頭だけの分岐無の計数
	size_t Chars = 0;
	const unsigned char *const Data = reinterpret_cast<const unsigned char *>(Text.data());
	const size_t Size = Text.size();
	for(size_t Idx = 0; Idx < Size; ++Idx) {
		const unsigned char Byte = Data[Idx];
		Chars += static_cast<size_t>(Byte > 0X1F && Byte < 0X7F) + static_cast<size_t>(Byte > 0XBF);
	}
	// 可視文字数の返戻
	return Chars;
}

/**
 * 収集した編集のソースへの適用関数
 * 編集数 E，入力と置換文字列の総バイト数 B，木の高さ H に対して時間 O(E log E + B + E H)，追加領域 O(B + E)
 * 座標走査は O(1) 領域
 * @param Src 適用対象のソースコード（結果で上書きされる）
 * @param Edits 適用する編集一覧
 */
void TextEdit::Apply(TSSource &Src, std::vector<TextEdit> &Edits) {
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
		Result.append(Src.data() + Pos, Edit.StartPos - Pos);
		Result.append(Edit.NewText);
		Pos = Edit.EndPos;
		IsAppliedAny = true;
	}
	// 先頭への零幅挿入は Pos が０の儘の為，IsAppliedAny で末尾本文の消失を防止
	if(!IsAppliedAny) return;
	Result.append(Src.data() + Pos, Src.size() - Pos);
	// 後方から通知すれば，未通知の編集のバイト・行・列は全て原文座標の儘である
	if(Tree) {
		for(std::vector<TSInputEdit>::reverse_iterator Iter = TreeEdits.rbegin(); Iter != TreeEdits.rend(); ++Iter) {
			ts_tree_edit(Tree, &*Iter);
		}
		Src.SetTreeEdited(true);
	}
	Src.Assign(std::move(Result));
	// 終了
	return;
}
