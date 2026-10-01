#include "CommentNormalizer.hpp"
#include "CharClass.hpp"
#include "TextEdit.hpp"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iterator>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

/**
 * 二重引用符の中身が説明文かの判定関数
 * 和文の句点か，文末の記号の後に空白を挟んで続く文を持つ中身は，字句・値でなく説明文の引用とする（整形仕様 7.3）
 * @param Inner 引用符の内側
 * @return 文境界を含む説明文なら true
 */
static bool IsQuotedProse(const std::string_view Inner) {
	// 和文の句点を含む事の返戻
	if(Inner.find("。") != std::string_view::npos || Inner.find("．") != std::string_view::npos) return true;
	for(size_t Pos = 0; Pos + 2 < Inner.size(); ++Pos) {
		if(Inner[Pos] != '.' && Inner[Pos] != '?' && Inner[Pos] != '!' || Inner[Pos + 1] != ' ') continue;
		// 空白の後に続く文を持つ事の返戻
		if(Inner.find_first_not_of(' ', Pos + 1) != std::string_view::npos) return true;
	}
	// 文境界を含まない字句・値の返戻
	return false;
}

/**
 * コメント内コードの保護範囲取得関数
 * 引用記号と箇条の印の後の空白を数え，本文中のコード例を判定する（整形仕様 7.3, 7.4）
 * 構造として保つ空の論理行は，コード例の中の行と，段落とコード例を分ける行とする（整形仕様 7.3, 7.7）
 * @param Text 記号を除いた本文の論理行（改行で繋いだ物）
 * @param Forms 文法のコード例の書式
 * @param IsUnclosed 未閉鎖のコード区画が有るかの出力先
 * @param KeptBlanks 論理行毎の構造として保つ空の論理行かの出力先
 * @return コード又は其のインデントに属するバイトの保護標識
 */
std::vector<unsigned char> CommentNormalizer::CommentCodeMask(
	const std::string_view Text,
	const Core::CodeExampleForms &Forms,
	bool &IsUnclosed,
	std::vector<uint8_t> &KeptBlanks
) {
	struct LineInfo {
		size_t Start; // 本文中の行開始位置
		size_t End; // 本文中の行終端位置
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
		// 指定範囲のコード保護
		std::fill(Code.begin() + Begin, Code.begin() + End, 1);
	};
	// コード区画と段落の走査状態
	size_t FenceLength = 0;
	// 開いている囲み区切りの文字
	char FenceChar = 0;
	// 段落とインデントコードの中かの状態
	bool IsParagraph = false, IsIndented = false;
	// 現在のリスト本文のインデント幅
	size_t ListIndent = 0;
	KeptBlanks.assign(Lines.size(), 0);
	// 最後の空でない行の後に続く空の論理行と，其の空でない行が段落・字下げのコード例だったか
	std::vector<size_t> Blanks;
	bool IsAfterParagraph = false, IsAfterIndented = false;
	// 囲みコードとインデントコードの行単位での先行保護
	for(size_t Index = 0; Index < Lines.size(); ++Index) {
		const LineInfo &Line = Lines[Index];
		// 行本文先頭と引用・箇条内のインデント幅
		size_t First = Line.Start, Columns = 0;
		// 引用とリストの接頭辞を除くコード区画インデントの算出
		while(First < Line.End && Text[First] == '>' && Columns < 4) {
			++First;
			if(First < Line.End && Text[First] == ' ') ++First;
			// 引用接頭辞後のインデント再計数
			Columns = 0;
			while(First < Line.End && (Text[First] == ' ' || Text[First] == '\t')) {
				// 引用内のインデント加算
				Columns += Text[First] == '\t' ? 4 - Columns % 4 : 1;
				++First;
			}
		}
		if(Columns >= ListIndent) Columns -= ListIndent;
		else ListIndent = 0;
		// リスト開始記号の終端候補
		size_t MarkerEnd = First;
		if(First < Line.End && (Text[First] == '-' || Text[First] == '+' || Text[First] == '*')) ++MarkerEnd;
		else {
			while(MarkerEnd < Line.End && std::isdigit(static_cast<unsigned char>(Text[MarkerEnd]))) ++MarkerEnd;
			if(MarkerEnd > First && MarkerEnd < Line.End && (Text[MarkerEnd] == '.' || Text[MarkerEnd] == ')')) ++MarkerEnd;
			else MarkerEnd = First;
		}
		// コード例の途中でない箇条の印の判定
		if(
			!FenceLength && (Forms.MarksCodeByIndent ? !IsIndented : Columns < 4) && MarkerEnd > First && MarkerEnd < Line.End &&
			Text[MarkerEnd] == ' '
		) {
			// リスト本文の基準インデント加算
			ListIndent += Columns + MarkerEnd - First + 1;
			// リスト本文先頭への移動
			First = MarkerEnd + 1;
			// リスト接頭辞後のインデント再計数
			Columns = 0;
			while(First < Line.End && (Text[First] == ' ' || Text[First] == '\t')) {
				// リスト本文内のインデント加算
				Columns += Text[First] == '\t' ? 4 - Columns % 4 : 1;
				++First;
			}
			// リスト開始行の段落状態の初期化
			IsParagraph = false;
		}
		// 空行の該当有無
		const bool IsBlank = First == Line.End;
		// 囲み区切りの終端候補
		size_t RunEnd = First;
		if(!IsBlank && (Text[First] == '`' || Text[First] == '~')) while(RunEnd < Line.End && Text[RunEnd] == Text[First]) ++RunEnd;
		// 連続する囲み文字数
		const size_t Run = RunEnd - First;
		if(FenceLength) {
			// 閉じ区切りを含む行全体の原文保持と，後に区画の中の行が続く囲みの中の空の論理行の保持（閉じない区画の末尾の空の行は保たない）
			Protect(Line.Start, Line.End);
			if(IsBlank) Blanks.push_back(Index);
			else {
				for(const size_t Blank : Blanks) KeptBlanks[Blank] = 1;
				Blanks.clear();
			}
			// 閉じ区切りの行（区切りの字の連続を持つ行だけを読み，空行の字を読まない）
			if(Run >= FenceLength && Columns < 4 && Text[First] == FenceChar && Text.find_first_not_of(" \t", RunEnd) >= Line.End) {
				FenceLength = 0;
			}
			// 囲みコード内の段落状態解除
			IsParagraph = false;
			continue;
		}
		if(Forms.HasFencedCode && Columns < 4 && Run > 2 && (Text[First] != '`' || Text.find('`', RunEnd) >= Line.End)) {
			// 囲み区切り文字の記録
			FenceChar = Text[First];
			// 囲み区切り長の記録
			FenceLength = Run;
			// 囲みコード開始行の全体保護
			Protect(Line.Start, Line.End);
			// 囲みコード開始時の段落状態解除
			IsParagraph = IsAfterParagraph = IsAfterIndented = false;
			Blanks.clear();
			continue;
		}
		if(IsBlank) Blanks.push_back(Index);
		if(Forms.MarksCodeByIndent ? Columns > 0 : Columns > 3 && (!IsParagraph || IsIndented)) {
			// インデントコード行の全体保護
			Protect(Line.Start, Line.End);
			if(!IsBlank && !Blanks.empty()) {
				// 字下げのコード例の中の空の論理行と，段落の後の字下げのコード例の直前の空の論理行の保持
				if(IsAfterIndented) for(const size_t Blank : Blanks) KeptBlanks[Blank] = 1;
				else if(IsAfterParagraph && !Forms.MarksCodeByIndent) KeptBlanks[Blanks.back()] = 1;
			}
			// インデントコードの状態設定
			IsIndented = true;
			// インデントコード内の段落状態解除
			IsParagraph = false;
		} else if(IsBlank) IsParagraph = false;
		else {
			// インデントコード状態の解除
			IsIndented = false;
			// 通常段落の開始状態設定
			IsParagraph = true;
		}
		if(IsBlank) continue;
		// 空でない行の段落・字下げのコード例の記録
		IsAfterParagraph = IsParagraph;
		IsAfterIndented = IsIndented;
		Blanks.clear();
	}
	// 閉じない囲みのコード区画の記録
	IsUnclosed = FenceLength > 0;
	// 同長の次区切りの一括収集による反復探索の回避
	std::unordered_map<size_t, size_t> PreviousTicks, NextTicks;
	// 直前行の空行状態
	bool IsBlankLine = true;
	// 同一段落内の同長バッククォートの対応付け
	for(size_t Pos = 0; Pos < Text.size();) {
		if(Code[Pos]) {
			// コード保護範囲での未対応区切り破棄
			PreviousTicks.clear();
			++Pos;
		} else if(Text[Pos] == '`') {
			// バッククォート列の終端位置
			size_t End = Pos + 1;
			while(End < Text.size() && Text[End] == '`') ++End;
			// バッククォート区切りの長さ
			const size_t Length = End - Pos;
			if(const std::unordered_map<size_t, size_t>::const_iterator Found = PreviousTicks.find(Length); Found != PreviousTicks.end()) {
				// 開始区切りに対応する閉じ位置の登録
				NextTicks[Found->second] = Pos;
			}
			// 同じ長さの最新開始位置の記録
			PreviousTicks[Length] = Pos;
			// 非空行状態への更新
			IsBlankLine = false;
			// 区切り列終端への移動
			Pos = End;
		} else {
			if(Text[Pos] == '\n') {
				// 空行と文書化タグ行を境界とするコードスパン段落の区分
				const size_t Next = Pos + 1;
				if(
					// 段落境界での未対応区切り破棄
					IsBlankLine || Next + 1 < Text.size() && Forms.TagMarks.find(Text[Next]) != std::string_view::npos &&
					std::isalpha(static_cast<unsigned char>(Text[Next + 1]))
				) PreviousTicks.clear();
				// 次行の空行状態初期化
				IsBlankLine = true;
			} else if(Text[Pos] != ' ' && Text[Pos] != '\t') IsBlankLine = false;
			++Pos;
		}
	}
	// 区切り長の一致するコードスパンと文書用コードタグだけの保護
	for(size_t Pos = 0; Pos < Text.size();) {
		if(Code[Pos]) {
			++Pos;
			continue;
		}
		// 文書生成系コード引用の保護範囲への追加
		// 文書用コードタグの開始長
		size_t InlineTag = 0;
		for(const std::string_view Tag : Forms.InlineTags) {
			if(Tag.empty() || Text.compare(Pos, Tag.size(), Tag)) continue;
			InlineTag = Tag.size();
			break;
		}
		if(
			InlineTag && Pos + InlineTag < Text.size() &&
			(std::isspace(static_cast<unsigned char>(Text[Pos + InlineTag])) || Text[Pos + InlineTag] == '}')
		) {
			// 文書用コードタグの走査位置と波括弧深さ
			size_t End = Pos + 1, Depth = 1;
			while(End < Text.size() && Depth) {
				if(Text[End] == '{') ++Depth;
				else if(Text[End] == '}') --Depth;
				++End;
			}
			if(Depth) IsUnclosed = true;
			// 文書用コードタグ範囲の保護
			Protect(Pos, End);
			// タグ終端への移動
			Pos = End;
			continue;
		}
		if(Forms.TagMarks.find(Text[Pos]) != std::string_view::npos) {
			// Doxygen の開始指令から対応終了指令迄のコード塊化
			bool IsCommand = false;
			for(const std::string_view Name : Forms.BlockCommands) {
				// 開始指令名の直後位置
				const size_t After = Pos + Name.size() + 1;
				if(Text.compare(Pos + 1, Name.size(), Name) || After < Text.size() && IsIdentifierChar(Text[After])) continue;
				// 対応する終了指令
				const std::string Close = std::string(1, Text[Pos]).append(Forms.EndCommandPrefix).append(Name);
				// 終了指令位置と保護終端
				const size_t Found = Text.find(Close, After), End = Found == std::string::npos ? Text.size() : Found + Close.size();
				if(Found == std::string::npos) IsUnclosed = true;
				// Doxygen コード指令範囲の保護
				Protect(Pos, End);
				// 終了指令直後への移動
				Pos = End;
				// 開始指令の検出状態設定
				IsCommand = true;
				break;
			}
			if(IsCommand) continue;
			// 枠組が実行時に読む注釈引数の括弧範囲の原文保持
			size_t NameEnd = Pos + 1;
			while(NameEnd < Text.size() && (IsIdentifierChar(Text[NameEnd]) || Text[NameEnd] == '\\')) ++NameEnd;
			if(Text[Pos] == Forms.AnnotationMark && NameEnd > Pos + 1 && NameEnd < Text.size() && Text[NameEnd] == '(') {
				// 注釈引数の走査位置と括弧深さ
				size_t End = NameEnd + 1, Depth = 1;
				for(char Quote = 0; End < Text.size() && Depth; ++End) {
					if(Quote) Quote = Text[End] == Quote ? 0 : Quote;
					else switch(Text[End]) {
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
				// 注釈引数範囲の保護
				Protect(Pos, End);
				// 注釈引数終端への移動
				Pos = End;
				continue;
			}
		}
		// 同じ行で閉じる二重引用符内の原文保持（文境界を含む説明文の引用は，引用である事を句点保持の理由にせず文毎に分ける，整形仕様 7.3）
		if(Text[Pos] == '"') if(
			const size_t Close = Text.find_first_of("\"\n", Pos + 1);
			Close != std::string::npos && Text[Close] == '"' && !IsQuotedProse(Text.substr(Pos + 1, Close - Pos - 1))
		) {
			// 二重引用符内の保護
			Protect(Pos, Close + 1);
			// 閉じ引用符直後への移動
			Pos = Close + 1;
			continue;
		}
		// 識別子の字に接しない単引用符対を同じ行内で保護（型・参照経路は保ち英文の省略は除外）
		if(Text[Pos] == '\'' && (!Pos || !IsIdentifierChar(Text[Pos - 1]))) if(
			const size_t Close = Text.find_first_of("'\n", Pos + 1);
			Close != std::string::npos && Text[Close] == '\'' && (Close + 1 >= Text.size() || !IsIdentifierChar(Text[Close + 1]))
		) {
			// 単引用符内の保護
			Protect(Pos, Close + 1);
			// 閉じ引用符直後への移動
			Pos = Close + 1;
			continue;
		}
		if(Text[Pos] == '\\' && Pos + 1 < Text.size()) {
			Pos += 2;
			continue;
		}
		if(Text[Pos] == '`') {
			// 対応する閉じ印を持つコードスパンだけの保護
			size_t End = Pos + 1;
			while(End < Text.size() && Text[End] == '`') ++End;
			// コードスパン区切りの長さ
			const size_t Length = End - Pos;
			if(const std::unordered_map<size_t, size_t>::const_iterator Next = NextTicks.find(Pos); Next != NextTicks.end()) {
				// 閉じ区切り直後への拡張
				End = Next->second + Length;
				// 対応するコードスパンの保護
				Protect(Pos, End);
			}
			// コードスパン終端への移動
			Pos = End;
			continue;
		}
		// 内容を字面の儘読む文書の要素の検出有無
		bool IsTagged = false;
		// 内容を字面の儘読む文書の要素の同名ネスト数による閉じ位置の決定
		if(Text[Pos] == '<') for(const std::string_view Tag : Forms.CodeElements) {
			if(Text.compare(Pos + 1, Tag.size(), Tag)) continue;
			// 開始タグ名の終端位置
			const size_t NameEnd = Pos + Tag.size() + 1;
			if(NameEnd >= Text.size() || Text[NameEnd] != '>' && Text[NameEnd] != ' ' && Text[NameEnd] != '\t' && Text[NameEnd] != '\n') {
				continue;
			}
			const auto TagEnd = [&](size_t Start) -> size_t {
				// 属性内で開いている引用符
				char Quote = 0;
				for(; Start < Text.size(); ++Start) {
					if(const char Char = Text[Start]; Quote && Char == Quote) Quote = 0;
					else if(!Quote && (Char == '\'' || Char == '"')) Quote = Char;
					// 閉じ記号の位置の返戻
					else if(!Quote && Char == '>') return Start;
				}
				// 閉じ記号不在の返戻
				return Text.size();
			};
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
							// 区画の開始かの返戻
							return !Text.compare(Scan, Markup.first.size(), Markup.first);
						}
					); Section != Forms.MarkupSections.end()
				) {
					// 要素の対応付けで読み飛ばす区画の閉じ位置
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
	// 前後の改行が保護範囲の中に在る空の論理行（複数行の要素・指令・注釈の引数の中の行）の保持
	for(size_t Index = 1; Index + 1 < Lines.size(); ++Index) {
		if(const LineInfo &Line = Lines[Index]; Line.Start == Line.End && Code[Line.Start - 1] && Code[Line.End]) KeptBlanks[Index] = 1;
	}
	// 保護標識の返戻
	return Code;
}

/**
 * 引用括弧の深度更新関数（鉤括弧・二重鉤括弧・隅付括弧の開閉を数える）
 * @param Text 対象テキスト
 * @param Pos 判定位置（UTF-8 の符号点先頭）
 * @param Depth 引用深度（開きで加算，閉じで減算される）
 * @return 位置が引用括弧の符号点なら true（呼出側は３バイト進める）
 */
bool CommentNormalizer::IsQuoteDepthChar(const std::string_view Text, const size_t Pos, size_t &Depth) {
	// 呼出側の走査条件による３バイト窓の範囲内保証
	const std::string_view Here(Text.data() + Pos, 3);
	if(Here == "「" || Here == "『" || Here == "【") {
		++Depth;
		// 開き括弧に合致した事の返戻
		return true;
	}
	if(Here == "」" || Here == "』" || Here == "】") {
		if(Depth) --Depth;
		// 閉じ括弧に合致した事の返戻
		return true;
	}
	// 引用括弧ではない事の返戻
	return false;
}

/**
 * 文末の記号の幅の取得関数
 * 文末の記号の集合は此処だけで定める
 * @param Text 走査対象
 * @param Pos 判定位置
 * @return 全角の句点・疑問符・感嘆符なら３，半角の `.` `?` `!` なら１，其の他は０
 */
static size_t SentenceMarkWidth(const std::string_view Text, const size_t Pos) {
	// 半角の文末の記号の幅の返戻
	if(Text[Pos] == '.' || Text[Pos] == '?' || Text[Pos] == '!') return 1;
	const std::string_view Cell = Text.substr(Pos, 3);
	// 全角の文末の記号の幅の返戻
	return Cell == "。" || Cell == "．" || Cell == "？" || Cell == "！" ? 3 : 0;
}

/**
 * 本文へ残す文末の記号の判定関数
 * 記号自身が意味を持つ物は文末でも除かない
 * @param Mark 判定する記号
 * @return 疑問符又は感嘆符なら true
 */
static bool IsKeptSentenceMark(const std::string_view Mark) {
	// 疑問符と感嘆符との一致の返戻
	return Mark == "？" || Mark == "！" || Mark == "?" || Mark == "!";
}

// 文境界で閉じ直す括弧・引用の対（開き，閉じ，半角の `"` は開閉が同じ字の為に別に数える）
static constexpr std::pair<std::string_view, std::string_view> SentenceBrackets[] =
{ { "(", ")" }, { "「", "」" }, { "『", "』" }, { "【", "】" }, { "（", "）" } };

/**
 * 括弧・引用の閉じの取得関数
 * @param Open 開きの字面
 * @return 対応する閉じの字面
 */
static std::string_view CloserOf(const std::string_view Open) {
	// 開きに対応する閉じの返戻
	for(const auto &[Opening, Closing] : SentenceBrackets) if(Opening == Open) return Closing;
	// 半角の引用符の返戻
	return "\"";
}

/**
 * 括弧・引用の閉じかの判定関数
 * @param Text 走査対象
 * @param Pos 判定位置
 * @return 閉じの字の始まりなら true
 */
static bool IsCloserAt(const std::string_view Text, const size_t Pos) {
	// 半角の引用符と XML・HTML の終了タグの返戻
	if(Text[Pos] == '"' || !Text.compare(Pos, 2, "</")) return true;
	// 閉じとの一致の返戻
	for(const auto &[Opening, Closing] : SentenceBrackets) if(!Text.compare(Pos, Closing.size(), Closing)) return true;
	// 閉じでない事の返戻
	return false;
}

/**
 * 略語の点かの判定関数
 * 一字毎に点を打つ語 (`e.g.` `i.e.` `U.S.`) と慣用の略語の末尾の点は文を終えない
 * 番号の前に置く略語 (`No. 5` `Fig. 3`) は数字が続く時だけ略語とし，小文字の一字毎の点の語は慣用の物だけを略語とする（`x.y.` は字句の後の句点）
 * @param Text 走査対象
 * @param Pos 点の位置
 * @return 略語の一部の点なら true
 */
static bool IsAbbreviationPeriod(const std::string_view Text, const size_t Pos) {
	// 名前の前に置く略語と文中の略語
	static constexpr std::string_view Abbreviations[] =
	{ "al", "approx", "cf", "dr", "etc", "inc", "jr", "ltd", "mr", "mrs", "ms", "prof", "sr", "st", "vs" };
	// 番号の前に置く略語
	static constexpr std::string_view NumberPrefixes[] = { "eq", "fig", "no", "vol" };
	// 小文字の一字毎に点を打つ慣用の略語
	static constexpr std::string_view LowerDotted[] = { "a.k.a", "a.m", "e.g", "i.e", "n.b", "p.m" };
	// 点の前の英字と点の並び
	size_t Start = Pos;
	while(Start && (std::isalpha(static_cast<unsigned char>(Text[Start - 1])) || Text[Start - 1] == '.')) --Start;
	const std::string_view Word = Text.substr(Start, Pos - Start);
	std::string Lower(Word);
	ToLowerAscii(Lower);
	// 一字毎に点を打つ語かの判定
	bool IsDotted = Word.size() > 2 && Word.size() % 2;
	for(size_t At = 0; IsDotted && At < Word.size(); ++At) IsDotted = At % 2 ? Word[At] == '.' : Word[At] != '.';
	// 大文字を含む物と小文字の慣用の略語の返戻
	if(IsDotted) return Lower != Word || std::find(std::begin(LowerDotted), std::end(LowerDotted), Lower) != std::end(LowerDotted);
	if(std::find(std::begin(NumberPrefixes), std::end(NumberPrefixes), Lower) != std::end(NumberPrefixes)) {
		const size_t Next = Text.find_first_not_of(' ', Pos + 1);
		// 数字が続く番号の前の略語かの返戻
		return Next != std::string_view::npos && std::isdigit(static_cast<unsigned char>(Text[Next]));
	}
	// 慣用の略語かの返戻
	return std::find(std::begin(Abbreviations), std::end(Abbreviations), Lower) != std::end(Abbreviations);
}

/**
 * 半角の疑問符・感嘆符が字句又は値の一部かの判定関数
 * URL と識別子の接尾記号は，後続の説明文の文字種に依らず保持する
 * 計算量：記号の手前の識別子の長さ N に対し O(N)
 * @param Text 走査対象
 * @param Pos 記号の位置
 * @param MarkEnd 記号の並びの終わり
 * @param BodyStart 行の本文の始まり
 * @param OpenEnd 最も内側の開いた括弧・引用の開きの終わり
 * @param IsInUrl 記号が `://` を含む字句（空白で区切った並び）の中に在るか
 * @param IsWideNext 記号の後に同じ行で日本語等の全角の字が続くか
 * @return 字句又は値の一部なら true
 */
static bool IsLexicalSentenceMark(
	const std::string_view Text,
	const size_t Pos,
	const size_t MarkEnd,
	const size_t BodyStart,
	const size_t OpenEnd,
	const bool IsInUrl,
	const bool IsWideNext
) {
	// 単独の半角の疑問符・感嘆符でない記号の並びの返戻
	if(MarkEnd != Pos + 1 || Text[Pos] != '?' && Text[Pos] != '!') return false;
	// URL の末尾に在る問合せ記号（値の一部）の返戻
	if(IsInUrl) return true;
	// 識別子に接しない記号（接尾記号でない）の返戻
	if(Pos <= BodyStart || !IsIdentifierChar(Text[Pos - 1])) return false;
	size_t WordStart = Pos;
	while(WordStart > BodyStart && IsIdentifierChar(Text[WordStart - 1])) --WordStart;
	const std::string_view Word = Text.substr(WordStart, Pos - WordStart);
	// 英文の文末と紛れない字句の形（`_`・数字を含む語，小文字１字の語）の返戻
	if(
		Word.find_first_of("_0123456789") != std::string_view::npos ||
		Word.size() == 1 && std::islower(static_cast<unsigned char>(Word[0]))
	) return true;
	// 小文字と２字目以降の大文字を併せ持つ識別子（`parseJSON` 等，大文字だけの略語は英文の語）の返戻
	if(
		std::any_of(
			Word.begin(),
			Word.end(),
			[](const char Char) -> bool {
				// 小文字かの返戻
				return std::islower(static_cast<unsigned char>(Char));
			}
		) && std::any_of(
			Word.begin() + 1,
			Word.end(),
			[](const char Char) -> bool {
				// 大文字かの返戻
				return std::isupper(static_cast<unsigned char>(Char));
			}
		)
	) return true;
	// 本文の頭に在るか，日本語に密着するか，後に日本語が続く小文字の識別子（`as?`・`defined?`・`println!` 等）かの返戻
	return std::islower(static_cast<unsigned char>(Word.front())) && (
		IsWideNext || WordStart == BodyStart ||
		WordStart > BodyStart && WordStart != OpenEnd && static_cast<unsigned char>(Text[WordStart - 1]) > 0X7F
	);
}

/**
 * 文末の記号の並びが文を終えるかの判定関数
 * @param Text 走査対象
 * @param Pos 記号の並びの始まり
 * @param MarkEnd 記号の並びの終わり
 * @param BodyStart 行の本文の始まり
 * @param LineEnd 行の終わり
 * @param OpenEnd 最も内側の開いた括弧・引用の開きの終わり（開いて居なければ０）
 * @param IsInUrl 記号が `://` を含む字句（空白で区切った並び）の中に在るか
 * @return 文境界なら true
 */
static bool IsSentenceEnd(
	const std::string_view Text,
	const size_t Pos,
	const size_t MarkEnd,
	const size_t BodyStart,
	const size_t LineEnd,
	const size_t OpenEnd,
	const bool IsInUrl
) {
	// 手前に文が無い記号（本文の頭・括弧や引用の開きの直後に置いた記号の例）の返戻
	if(Pos <= BodyStart || Pos == OpenEnd) return false;
	const unsigned char Previous = static_cast<unsigned char>(Text[Pos - 1]);
	// 語・数・閉じに密着しない記号（空白・記法の後に置いた `!!` や `<summary>!` 等の記号の例）の返戻
	if(
		!(
			std::isalnum(Previous) || Previous > 0X7F ||
			std::string_view(")]\"'%`").find(static_cast<char>(Previous)) != std::string_view::npos
		)
	) return false;
	// 全角の記号だけの並びの判定（並びの間の空白は数えない）
	bool IsWide = true;
	for(size_t At = Pos; IsWide && At < MarkEnd;) {
		const size_t Width = SentenceMarkWidth(Text, At);
		IsWide = !Width || Width == 3;
		At += Width ? Width : 1;
	}
	if(IsWide) {
		// 全角数字かの判定の定義
		const auto IsWideDigit = [Text](const size_t At) -> bool {
			// 全角数字０〜９の UTF-8 範囲に在るかの返戻
			return At + 3 <= Text.size() && Text[At] == '\xEF' && Text[At + 1] == '\xBC' && Text[At + 2] >= '\x90' &&
			Text[At + 2] <= '\x99';
		};
		// 全角数字に挟まれた全角の小数点を除く事の返戻
		return !(MarkEnd == Pos + 3 && Text.substr(Pos, 3) == "．" && Pos > 2 && IsWideDigit(Pos - 3) && IsWideDigit(MarkEnd));
	}
	// 省略記号の点の返戻
	if(Text.substr(Pos, MarkEnd - Pos).find("..") != std::string_view::npos) return false;
	// 記号の直後が字（小数・経路・URL・版番号の内部の点）の返戻
	const auto IsCloseAt = [Text, LineEnd](const size_t At) -> bool {
		// 括弧・引用の閉じかの返戻
		return At < LineEnd && IsCloserAt(Text, At);
	};
	// 字に続く点の返戻
	if(MarkEnd < LineEnd && Text[MarkEnd] != ' ' && Text[MarkEnd] != '\t' && !IsCloseAt(MarkEnd)) return false;
	// 同じ行に続く本文の始まり（閉じが続けば無し）
	size_t After = MarkEnd;
	while(After < LineEnd && (Text[After] == ' ' || Text[After] == '\t')) ++After;
	const unsigned char Next = After < LineEnd && !IsCloseAt(After) ? static_cast<unsigned char>(Text[After]) : '\0';
	// 語・コード片・値の一部である疑問符・感嘆符の返戻
	if(IsLexicalSentenceMark(Text, Pos, MarkEnd, BodyStart, OpenEnd, IsInUrl, Next > 0X7F)) return false;
	// 次の文の始まりでない続き（英小文字で始まる続きは同じ文の中の記号）の返戻
	if(Next && !(Next >= 'A' && Next <= 'Z' || Next > 0X7F || Next == '(' || Next == '"')) return false;
	// 疑問符・感嘆符の返戻
	if(Text[Pos] != '.') return true;
	// 略語の点の返戻（列挙を締める `etc.` だけは，次の文が続けば点を残した儘で文を終える）
	if(IsAbbreviationPeriod(Text, Pos)) return Pos > 2 && !Text.compare(Pos - 3, 3, "etc") && MarkEnd < LineEnd;
	size_t Digits = Pos;
	while(Digits > BodyStart && std::isdigit(static_cast<unsigned char>(Text[Digits - 1]))) --Digits;
	// 本文の頭の数字に続く列挙番号の点でない事の返戻
	return !(Digits == BodyStart && Digits < Pos);
}

/**
 * 文境界の収集関数（整形仕様 7.3）
 * 全角の句点・疑問符・感嘆符と英文の文末の `.` `?` `!` を，使用言語・引用・記法に依らず文境界とする
 * 小数・経路・URL・版番号の内部の点，略語・列挙番号の点，括弧・引用の開きの直後に置いた記号の例は文を終えない
 * 語・コード片・値の一部である疑問符・感嘆符は文を終えない
 * 括弧・引用の中の境界は，此の論理行の末尾で閉じ，次の論理行の頭で開き直す字面を記録する（記号の直後の閉じは其の中の文の終わりで，外側の文は続く）
 * 計算量：本文の長さ N に対し O(N)
 * @param Text コメント本文
 * @param Protected コードの保護標識
 * @return 文境界の位置順の並び
 */
std::vector<CommentNormalizer::SentenceBoundary> CommentNormalizer::FindSentenceBoundaries(
	const std::string_view Text,
	const std::span<const unsigned char> Protected
) {
	std::vector<SentenceBoundary> Boundaries;
	for(size_t LineBegin = 0; LineBegin < Text.size();) {
		const size_t LineEnd = std::min(Text.find('\n', LineBegin), Text.size());
		// 開いた括弧・引用（開きの字面，開きの終わりの位置）
		std::vector<std::pair<std::string_view, size_t>> Opens;
		// 空白で区切った字句の始まりと，行の中の最後の `://` の位置（記号毎に字句を遡らない）
		size_t TokenStart = LineBegin, SchemeAt = std::string_view::npos;
		for(size_t Pos = LineBegin; Pos < LineEnd;) {
			if(Text[Pos] == ' ' || Text[Pos] == '\t') TokenStart = Pos + 1;
			else if(!Text.compare(Pos, 3, "://")) SchemeAt = Pos;
			// 保護範囲の読み飛ばし
			if(Protected[Pos]) {
				++Pos;
				continue;
			}
			// 半角の引用符の開閉
			if(Text[Pos] == '"') {
				if(!Opens.empty() && Opens.back().first == "\"") Opens.pop_back();
				else Opens.push_back({ "\"", Pos + 1 });
				++Pos;
				continue;
			}
			// 括弧・引用の開閉
			size_t Bracket = 0;
			for(const auto &[Opening, Closing] : SentenceBrackets) {
				if(!Text.compare(Pos, Opening.size(), Opening)) {
					Opens.push_back({ Opening, Pos + Opening.size() });
					Bracket = Opening.size();
				} else if(!Text.compare(Pos, Closing.size(), Closing)) {
					if(!Opens.empty() && Opens.back().first == Opening) Opens.pop_back();
					Bracket = Closing.size();
				}
				if(Bracket) break;
			}
			if(Bracket) {
				Pos += Bracket;
				continue;
			}
			if(!SentenceMarkWidth(Text, Pos)) {
				++Pos;
				continue;
			}
			// 続く記号は空白を挟んでも同じ文末の一部（別々に数えると分割が二重に為り記号だけの行が出来る）
			size_t MarkEnd = Pos;
			for(size_t Next = Pos; Next < LineEnd && !Protected[Next];) {
				if(const size_t Width = SentenceMarkWidth(Text, Next)) MarkEnd = Next += Width;
				else if(Text[Next] == ' ' || Text[Next] == '\t') ++Next;
				else break;
			}
			if(
				const bool IsInUrl = SchemeAt != std::string_view::npos && SchemeAt >= TokenStart;
				!IsSentenceEnd(Text, Pos, MarkEnd, LineBegin, LineEnd, Opens.empty() ? 0 : Opens.back().second, IsInUrl)
			) {
				Pos = MarkEnd;
				continue;
			}
			// 此の論理行の末尾で閉じ，次の論理行の頭で開き直す字面
			SentenceBoundary Boundary { Pos, MarkEnd, {}, {}, Text[Pos] == '.' && IsAbbreviationPeriod(Text, Pos) };
			for(size_t Index = Opens.size(); Index--;) Boundary.Close += CloserOf(Opens[Index].first);
			for(const std::pair<std::string_view, size_t> &Open : Opens) Boundary.Reopen += Open.first;
			Boundaries.push_back(std::move(Boundary));
			Pos = MarkEnd;
		}
		LineBegin = LineEnd + 1;
	}
	// 文境界の並びの返戻
	return Boundaries;
}

/**
 * 文末の記号の後が論理行の終わりかの判定関数
 * 行・括弧・引用の閉じで終わる文と，行末へ積んだ次の文の前の文は，記号を除くだけで論理行を分けない
 * @param Text 走査対象
 * @param MarkEnd 文末の記号の並びの終わり
 * @param LineBegin 記号の在る行の始まり
 * @param LineEnd 記号の在る行の終わり
 * @param StackMarker 行末へ積んだ次の文の始まりの字面（行記法の記号と空白，囲みは空）
 * @return 記号の後に空白を除いて同じ行の本文が続かなければ true
 */
static bool EndsLogicalLine(
	const std::string_view Text,
	const size_t MarkEnd,
	const size_t LineBegin,
	const size_t LineEnd,
	const std::string_view StackMarker
) {
	size_t Rest = MarkEnd;
	while(Rest < LineEnd && (Text[Rest] == ' ' || Text[Rest] == '\t')) ++Rest;
	// 行・括弧・引用の閉じ，又は行末へ積んだ次の文の始まりで終わるかの返戻
	return Rest == LineEnd || IsCloserAt(Text, Rest) ||
	!StackMarker.empty() && Rest > LineBegin && !Text.compare(Rest, StackMarker.size(), StackMarker);
}

/**
 * 文境界での論理行の分割関数（整形仕様 7.3）
 * 文毎に論理行を分け，文末の句点を除く（疑問符・感嘆符は残す）
 * 除くと行が `\` で終わり次の行を続ける記号は残す（C 系の行継続が次の行をコメントへ取り込む為）
 * 記号の一部の頭で始まる最初の論理行から分けた行は，行記法では同じ頭を継ぐ
 * @param Text 本文の論理行（破壊的に書き換えられる）
 * @param Boundaries 文境界の位置順の並び
 * @param StackMarker 行末へ積んだ次の文の始まりの字面（行記法の記号と空白，囲みは空）
 * @param SymbolHead 最初の論理行が始まる記号の一部の頭（行記法で最初の論理行から分けた行も継ぐ，無ければ空）
 * @param Protected コードの保護標識（書換に合わせて更新される）
 */
void CommentNormalizer::SplitSentences(
	std::string &Text,
	const std::span<const SentenceBoundary> Boundaries,
	const std::string_view StackMarker,
	const std::string_view SymbolHead,
	std::vector<unsigned char> &Protected
) {
	// 境界が無い時の終了
	if(Boundaries.empty()) return;
	std::string Out;
	std::vector<unsigned char> OutProtected;
	Out.reserve(Text.size() + (Boundaries.size() << 3));
	OutProtected.reserve(Out.capacity());
	size_t Copied = 0;
	// 原文の範囲の写しの定義
	const auto Copy = [&](const size_t To) -> void {
		Out.append(Text, Copied, To - Copied);
		OutProtected.insert(OutProtected.end(), Protected.begin() + Copied, Protected.begin() + To);
		Copied = To;
	};
	// 補う字面の追記の定義（保護する字面は保護の標識を立てる）
	const auto Insert = [&](const std::string_view Added, const bool IsProtected = false) -> void {
		Out += Added;
		OutProtected.resize(Out.size(), IsProtected);
	};
	// 境界の在る行の範囲と次の論理行の頭（境界は位置順の為，行が変わった時だけ求め直す）
	size_t LineBegin = 0, LineEnd = 0;
	std::string Continuation;
	for(const SentenceBoundary &Boundary : Boundaries) {
		if(!LineEnd || Boundary.Pos >= LineEnd) {
			LineBegin = Text.rfind('\n', Boundary.Pos) + 1;
			LineEnd = std::min(Text.find('\n', Boundary.Pos), Text.size());
			Continuation = !LineBegin && !SymbolHead.empty() ? '\n' + std::string(SymbolHead) + ' ' : "\n";
		}
		// 記号の後の本文
		size_t Rest = Boundary.MarkEnd;
		while(Rest < LineEnd && (Text[Rest] == ' ' || Text[Rest] == '\t')) ++Rest;
		const bool IsLast = EndsLogicalLine(Text, Boundary.MarkEnd, LineBegin, LineEnd, StackMarker);
		// 除くと行が `\` で終わる記号を残すか
		size_t Before = Boundary.Pos;
		while(Before > LineBegin && (Text[Before - 1] == ' ' || Text[Before - 1] == '\t')) --Before;
		const bool KeepsAll = Boundary.IsAbbreviation || Before > LineBegin && Text[Before - 1] == '\\';
		Copy(Boundary.Pos);
		// 残す記号の追記
		bool IsDropped = false;
		for(size_t At = Boundary.Pos; At < Boundary.MarkEnd;) {
			const size_t Width = SentenceMarkWidth(Text, At);
			// 記号の間の空白の読飛
			if(!Width) {
				++At;
				continue;
			}
			if(const std::string_view Mark = std::string_view(Text).substr(At, Width); KeepsAll || IsKeptSentenceMark(Mark)) Insert(Mark);
			else IsDropped = true;
			At += Width;
		}
		Copied = Boundary.MarkEnd;
		if(IsLast) {
			// 記号を除いて本文が行末へ積んだ次の文へ接する場合の空白の補完
			if(IsDropped && Rest == Boundary.MarkEnd && Rest < LineEnd && !IsCloserAt(Text, Rest)) Insert(" ");
			continue;
		}
		// 閉じ直す括弧・引用，次の論理行の頭と開き直す括弧・引用
		Insert(Boundary.Close);
		// 次の論理行の頭の改行と，記号へ密着する字面
		Insert(std::string_view(Continuation).substr(0, 1));
		Insert(std::string_view(Continuation).substr(1));
		Insert(Boundary.Reopen);
		Copied = Rest;
	}
	Copy(Text.size());
	Text = std::move(Out);
	Protected = std::move(OutProtected);
	// 終了
	return;
}

/**
 * コメントの文区切の違反の判定関数（説明文は１文１論理行とし，句点を書かない）
 * 論理行の途中の文境界と，除く記号（句点）の残る文末を違反とし，疑問符・感嘆符で終わる１文は違反としない
 * @param Text 記号を除いた本文の論理行
 * @param Protected 本文のコード保護標識（正規化と同じ判定の物）
 * @param StackMarker 行末へ積んだ次の文の始まりの字面（行記法の記号と空白，囲みは空）
 * @return 違反が在れば true
 */
bool CommentNormalizer::HasSentenceViolation(
	const std::string_view Text,
	const std::span<const unsigned char> Protected,
	const std::string_view StackMarker
) {
	// 境界の在る行の範囲（境界は位置順の為，行が変わった時だけ求め直す）
	size_t LineBegin = 0, LineEnd = 0;
	for(const SentenceBoundary &Boundary : FindSentenceBoundaries(Text, Protected)) {
		if(!LineEnd || Boundary.Pos >= LineEnd) {
			LineBegin = Text.rfind('\n', Boundary.Pos) + 1;
			LineEnd = std::min(Text.find('\n', Boundary.Pos), Text.size());
		}
		// 同じ論理行に続く文の返戻
		if(!EndsLogicalLine(Text, Boundary.MarkEnd, LineBegin, LineEnd, StackMarker)) return true;
		for(size_t At = Boundary.Pos; !Boundary.IsAbbreviation && At < Boundary.MarkEnd;) {
			const size_t Width = SentenceMarkWidth(Text, At);
			// 文末に残る句点の返戻
			if(Width && !IsKeptSentenceMark(Text.substr(At, Width))) return true;
			At += Width ? Width : 1;
		}
	}
	// 違反の無い事の返戻
	return false;
}

/**
 * コメントの文区切と句点の指摘位置の取得関数
 * @param Text コメントの本文
 * @param Protected コード例等の保護位置
 * @param StackMarker 論理行末の積上記号
 * @return 本文中の指摘を出現順に並べた列
 */
std::vector<CommentNormalizer::SentenceIssue> CommentNormalizer::FindSentenceIssues(
	const std::string_view Text,
	const std::span<const unsigned char> Protected,
	const std::string_view StackMarker
) {
	std::vector<SentenceIssue> Issues;
	// 境界の在る行の範囲（境界は位置順の為，行が変わった時だけ求め直す）
	size_t LineBegin = 0, LineEnd = 0;
	for(const SentenceBoundary &Boundary : FindSentenceBoundaries(Text, Protected)) {
		if(!LineEnd || Boundary.Pos >= LineEnd) {
			LineBegin = Text.rfind('\n', Boundary.Pos) + 1;
			LineEnd = std::min(Text.find('\n', Boundary.Pos), Text.size());
		}
		// 同じ論理行に続く文の返戻
		if(!EndsLogicalLine(Text, Boundary.MarkEnd, LineBegin, LineEnd, StackMarker)) {
			size_t Next = Boundary.MarkEnd;
			while(Next < LineEnd && (Text[Next] == ' ' || Text[Next] == '\t')) ++Next;
			if(Next < LineEnd) Issues.push_back({ Next, false });
		}
		for(size_t At = Boundary.Pos; !Boundary.IsAbbreviation && At < Boundary.MarkEnd;) {
			const size_t Width = SentenceMarkWidth(Text, At);
			// 文末に残る句点の返戻
			if(Width && !IsKeptSentenceMark(Text.substr(At, Width))) {
				Issues.push_back({ At, true });
				break;
			}
			At += Width ? Width : 1;
		}
	}
	// 検出した問題の並びの返戻
	return Issues;
}

/**
 * 前の論理行の英文が次の論理行へ続き得るかの判定関数（整形仕様 7.3, 8.4 の要確認の候補）
 * 句点を除いた後は文境界を字面から確定出来ない為，前の行が語又は読点で終わり次の行が英小文字で始まる語の並びを，行幅で折った１文の候補とする
 * @param Previous 前の論理行
 * @param Next 次の論理行
 * @return 行幅で折った１文の候補なら true
 */
bool CommentNormalizer::ContinuesSentence(std::string_view Previous, std::string_view Next) {
	TextEdit::TrimView(Previous);
	TextEdit::TrimView(Next);
	// 空の行と，語の並びでない前の行（空白を含まない字句・値）の返戻
	if(Previous.empty() || Next.empty() || Previous.find(' ') == std::string_view::npos) return false;
	const char Last = Previous.back(), First = Next.front();
	// 英小文字又は読点で終わる行に英小文字で始まる行が続くかの返戻
	return (Last >= 'a' && Last <= 'z' || Last == ',') && First >= 'a' && First <= 'z';
}

/**
 * コメント本文の空白の正規化関数（規約の半角と全角の境界のスペースの規則を本文へ適用する）
 * @param Text 対象テキスト（破壊的に書き換えられる）
 * @param TagMarks 文書の札・指令を始める字
 * @param Protected コードの保護標識（書換に合わせて更新される）
 */
void CommentNormalizer::NormalizeCommentSpaces(
	std::string &Text,
	const std::string_view TagMarks,
	std::vector<unsigned char> &Protected
) {
	// ASCII 外の表音文字・結合記号・合字・表示形を半角扱い
	const auto IsPhoneticLetter = [](const uint32_t Point) -> bool {
		// 表音文字（乗除の記号を除く）かの返戻
		return Point - 0XC0 < 0X1F40 && Point != 0XD7 && Point != 0XF7 || Point == 0XAA || Point == 0XB2 || Point == 0XB3 ||
		Point == 0XB5 || Point == 0XB9 || Point == 0XBA || Point - 0X2070 < 0X30 || Point - 0X2C60 < 0X20 || Point - 0X2DE0 < 0X20 ||
		Point - 0X3130 < 0X60 || Point - 0XA640 < 0X60 || Point - 0XA720 < 0XE0 || Point - 0XA960 < 0X20 || Point - 0XAB30 < 0X40 ||
		Point - 0XAC00 < 0X2C00 || Point - 0XFB00 < 0X300 || Point - 0XFE70 < 0X8D;
	};
	// 字形を持たない書式制御・異体字選択子・BOM・タグ文字は前の文字判定を継承
	const auto IsFormatControl = [](const uint32_t Point) -> bool {
		// 書式の制御かの返戻
		return Point == 0XAD || Point - 0X200B < 5 || Point - 0X202A < 5 || Point - 0X2060 < 0X10 || Point - 0XFE00 < 0X10 ||
		Point == 0XFEFF || Point - 0XE0000 < 0X1F0;
	};
	// 半角語として扱う単位・通貨記号
	const auto IsHalfWidthQuantityMark = [](const uint32_t Point) -> bool {
		// パーセント・パーミル・度・分・秒又は Unicode 17.0.0 の互換幅形を除く通貨記号かの返戻
		return Point == '%' || Point == 0XB0 || Point == 0X2030 || Point - 0X2032 < 2 || Point == '$' || Point - 0XA2 < 4 ||
		Point == 0X58F || Point == 0X60B || Point - 0X7FE < 2 || Point - 0X9F2 < 2 || Point == 0X9FB || Point == 0XAF1 ||
		Point == 0XBF9 || Point == 0XE3F || Point == 0X17DB || Point - 0X20A0 < 0X22 || Point == 0XA838 || Point == 0XFDFC ||
		Point - 0X11FDD < 4 || Point == 0X1E2FF || Point == 0X1ECB0;
	};
	// 空白・書式制御・制御文字を除く非半角文字の全角扱い
	const auto IsFullWidth = [&IsPhoneticLetter, &IsFormatControl, &IsHalfWidthQuantityMark](const uint32_t Point) -> bool {
		// ASCII・表音文字・空白・書式の制御の何れでもないかの返戻
		return Point > 0XA0 && !IsPhoneticLetter(Point) && !IsFormatControl(Point) && !IsHalfWidthQuantityMark(Point) &&
		Point != 0X1680 && Point - 0X2000 > 0XA && Point - 0X2028 > 1 && Point != 0X202F && Point != 0X205F && Point != 0X3000;
	};
	// 字形の左右に余白を含む全角の区切子・括弧・連結記号
	const auto IsFullWidthMark = [](const uint32_t Point) -> bool {
		// 全角の区切記号・括弧・引用符・連結記号の判定結果の返戻
		return Point == 0X3001 || Point == 0X3002 || Point - 0X3008 < 10 || Point - 0X3014 < 8 || Point == 0X30FB || Point == 0XFF01 ||
		Point == 0XFF08 || Point == 0XFF09 || Point - 0XFF0C < 4 || Point - 0XFF1A < 3 || Point == 0XFF1E || Point == 0XFF1F ||
		Point == 0XFF3B || Point == 0XFF3D || Point == 0XFF5B || Point == 0XFF5D || Point - 0X2018 < 8 || Point - 0X301D < 3 ||
		Point - 0X2010 < 4 || Point == 0X301C || Point == 0XFF5E || Point == 0XFF1D || Point == 0XFF0B || Point == 0XD7 ||
		Point == 0XF7 || Point == 0XFF06 || Point == 0XFF0A;
	};
	// 規約が前後の空白を許す矢印とダッシュの集合
	const auto IsSpacedMark = [](const uint32_t Point) -> bool {
		// 右矢印・左右矢印・ダッシュかの返戻
		return Point == 0X2192 || Point == 0X2194 || Point == 0X2014;
	};
	// 半角語との間へ空白を置く全角文字の集合
	const auto IsFullWidthWord = [&](const uint32_t Point) -> bool {
		// 区切り・矢印でない全角の字かの返戻
		return IsFullWidth(Point) && !IsFullWidthMark(Point) && !IsSpacedMark(Point);
	};
	// 括弧・引用符・コンマ・セミコロン以外の半角可視文字を語の続きとして判定
	const auto IsWordChar = [](const char Char) -> bool {
		// 半角の語の続きの字かの返戻
		return Char > ' ' && Char < 0X7F && std::string_view("()[]{}\"'`,;").find(Char) == std::string_view::npos;
	};
	// 語の端に該当する半角文字の集合
	const auto IsWordEdgeChar = [&IsPhoneticLetter, &IsHalfWidthQuantityMark](const uint32_t Point) -> bool {
		// 英数字・`_`・`+`・`#`・表音文字かの返戻
		return Point < 0X80 ?
		std::isalnum(static_cast<int>(Point)) || Point == '_' || Point == '+' || Point == '#' || IsHalfWidthQuantityMark(Point) :
		IsPhoneticLetter(Point) || IsHalfWidthQuantityMark(Point);
	};
	// 半角の数字かの判定
	const auto IsAsciiDigit = [](const uint32_t Point) -> bool {
		// 半角の `0`~`9` の符号点かの返戻
		return Point - '0' < 10;
	};
	// 位置の字の符号点（不正な並びと末尾は 0）
	const auto PointAt = [&Text](size_t At) -> uint32_t {
		// 指定位置の符号点の復号
		uint32_t Point = 0;
		// 復号した符号点の返戻
		return At < Text.size() && TextEdit::DecodeUtf8(Text, At, Point) ? Point : 0;
	};
	// 位置から始まる半角の語が字（英字・上付と下付の数字以外の表音文字）を含むか
	const auto HasLetterAt = [&Text, &IsWordChar, &IsPhoneticLetter](size_t Pos) -> bool {
		// 位置から続く半角の語の字の走査
		for(uint32_t Point = 0; Pos < Text.size() && TextEdit::DecodeUtf8(Text, Pos, Point);) {
			if(Point < 0X80 ? !IsWordChar(static_cast<char>(Point)) : !IsPhoneticLetter(Point)) break;
			// 字を含む語である事の返戻
			if(Point < 0X80 ? std::isalpha(static_cast<int>(Point)) : Point > 0XBF && Point - 0X2070 > 0X2F) return true;
		}
		// 字を含まない語である事の返戻
		return false;
	};
	// 位置から始まる数量の単位（`N回` / `M個` の数量の英字と単位の間は，規約が英字を全角にする所，`X回転` の様に漢字の続く語の一部は除く）
	const auto IsQuantityUnitAt = [&Text, &PointAt](const size_t At) -> bool {
		// 数量に続く単位の照合
		static constexpr std::string_view Units[] = { "件", "個", "倍", "列", "回", "字", "文字", "本", "枚", "桁", "段", "番目", "秒", "箇所", "行" };
		// 単位の何れかで始まるかの返戻
		return std::any_of(
			std::begin(Units),
			std::end(Units),
			[&](const std::string_view Unit) -> bool {
				const uint32_t After = PointAt(At + Unit.size());
				// 語の一部でない（漢字が続かない，但し順序を表す `目` は続けられる）単位の字面の一致の返戻
				return !Text.compare(At, Unit.size(), Unit) && (After - 0X4E00 > 0X51FF && After - 0X3400 > 0X19BF || After == 0X76EE);
			}
		);
	};
	// 箇条書・番号・引用・例示の印と後続空白の終端取得（無ければ元位置）
	const auto ListMarkerEnd = [&Text, &Protected](const size_t At) -> size_t {
		// 箇条書見出の終端探索
		size_t End = At;
		// 保護された字（コード）は記号でない事の返戻
		if(End >= Text.size() || Protected[End]) return At;
		if(std::string_view("-*+>").find(Text[End]) != std::string_view::npos) ++End;
		else if(!Text.compare(End, 3, "❌") || !Text.compare(End, 3, "✅") || !Text.compare(End, 3, "⚠")) {
			End += 3;
			// 絵表示を選ぶ異体字選択子 U+FE0F の読飛
			if(!Text.compare(End, 3, "\xEF\xB8\x8F")) End += 3;
		} else {
			while(End < Text.size() && std::isdigit(static_cast<unsigned char>(Text[End]))) ++End;
			// 番号の後に `.` か `)` が続かなければ記号でない事の返戻
			if(End == At || End >= Text.size() || Text[End] != '.' && Text[End] != ')') return At;
			++End;
		}
		// 記号の後に空白が続かなければ記号でない事の返戻
		if(End >= Text.size() || Text[End] != ' ' && Text[End] != '\t') return At;
		while(End < Text.size() && (Text[End] == ' ' || Text[End] == '\t')) ++End;
		// 記号と空白の終わりの返戻
		return End;
	};
	std::string Out;
	// 正規化後位置別の保護標識
	std::vector<unsigned char> OutProtected;
	Out.reserve(Text.size() + (Text.size() >> 4));
	OutProtected.reserve(Out.capacity());
	// 原文範囲と保護標識の同時転写
	const auto Emit = [&](const size_t Begin, const size_t End) -> void {
		// 原文と保護標識の同時転写
		Out.append(Text, Begin, End - Begin);
		OutProtected.insert(OutProtected.end(), Protected.begin() + Begin, Protected.begin() + End);
	};
	// 補完済空白の転写
	const auto EmitSpace = [&]() -> void {
		// 空白の追記
		Out += ' ';
		// 挿入空白の非保護標識追加
		OutProtected.push_back(0);
	};
	// 直前に空白を挟まずに書いた本文の字（行頭と空白の後は 0）
	uint32_t Last = 0;
	// 直前文字の保護・半角語・リンク末尾と，英字を含む語・全角直後の密着語の状態
	bool IsLastProtected = false, IsLastWord = false, IsLastLinkEnd = false, HasLetterWord = false, IsAttachedRun = false;
	// 半角語の始点と引用・半角括弧の深さ（語の `>` と記法の `>` を区別）
	size_t WordStart = 0, Depth = 0, BracketDepth = 0;
	// 深さ別の半角開き括弧前の空白有無
	uint64_t SpacedBrackets = 0;
	// リンク先の中か・空白を詰めない行か
	bool IsInLink = false, IsVerbatimLine = false;
	// 走査位置以降の最初の閉じ角括弧か改行位置の再利用
	size_t LinkClose = 0;
	// 最初の空白・`/`・`://`・非 ASCII の位置を通過迄再利用（長い無空白行で塊毎に再探索する二乗費用を回避）
	const auto FindWide = [&Text](const size_t From) -> size_t {
		// 非 ASCII の字の位置（無ければ文の長さ）の返戻
		return static_cast<size_t>(
			std::find_if(
				Text.begin() + static_cast<std::ptrdiff_t>(From),
				Text.end(),
				[](const char Byte) -> bool {
					// 非 ASCII の字かの返戻
					return static_cast<unsigned char>(Byte) > 0X7F;
				}
			) - Text.begin()
		);
	};
	size_t SpaceAt = Text.find_first_of(" \t\n"), SlashAt = Text.find('/'), UrlAt = Text.find("://"), WideAt = FindWide(0);
	// 本文の符号点毎の空白と保護範囲の走査
	for(size_t Pos = 0; Pos < Text.size();) {
		const bool IsLineStart = !Pos || Text[Pos - 1] == '\n';
		if(IsLineStart) {
			size_t Body = Pos;
			IsVerbatimLine = false;
			// 箇条書・番号・引用・例示の印と後続空白を保持（`❌` 行は悪い例の為，本文も変更対象外）
			for(size_t Item = ListMarkerEnd(Body); Item != Body; Item = ListMarkerEnd(Body)) {
				IsVerbatimLine = IsVerbatimLine || !Text.compare(Body, 3, "❌");
				Emit(Body, Item);
				Body = Item;
			}
			Pos = Body;
			Last = 0;
			IsLastProtected = IsLastWord = IsLastLinkEnd = IsAttachedRun = IsInLink = false;
			if(Pos >= Text.size()) break;
		}
		const char Char = Text[Pos];
		const size_t Start = Pos;
		if(Char == ' ' && !Protected[Pos] && !Depth && !IsInLink && !IsVerbatimLine) {
			while(Pos < Text.size() && Text[Pos] == ' ' && !Protected[Pos]) ++Pos;
			const uint32_t Next = PointAt(Pos);
			// 規約が空白を許す章番号直後の境界
			const auto IsAfterChapter = [&]() -> bool {
				// 直前の字が「章」又は「節」でない場合の返戻
				if(Last != 0X7AE0 && Last != 0X7BC0) return false;
				size_t At = Out.size() - 3;
				// 章番号を構成する全角数字と半角数字の後方走査
				while(At > 2 && !Out.compare(At - 3, 2, "\xEF\xBC") && static_cast<unsigned char>(Out[At - 1]) - 0X90U < 10) At -= 3;
				while(At && std::isdigit(static_cast<unsigned char>(Out[At - 1]))) --At;
				// 番号の前が「第」かの返戻
				return At > 2 && !Out.compare(At - 3, 3, "第");
			};
			// 文書化タグと説明の間の区切り（`@return （甲）` の空白を詰めるとタグの本文が失われ，同期が空のタグを足す）
			const auto IsAfterDocTag = [&Out, TagMarks]() -> bool {
				size_t At = Out.size();
				// タグ名の後方走査
				while(At && IsIdentifierChar(Out[At - 1])) --At;
				// タグの名前と札・指令を始める字を持たない事の返戻
				if(At == Out.size() || !At || TagMarks.find(Out[At - 1]) == std::string_view::npos) return false;
				--At;
				// 札の前の空白と箇条の印の後方走査
				while(At && (Out[At - 1] == ' ' || Out[At - 1] == '\t')) --At;
				if(At && Out[At - 1] == '*') --At;
				while(At && (Out[At - 1] == ' ' || Out[At - 1] == '\t')) --At;
				// 行の初めのタグかの返戻
				return !At || Out[At - 1] == '\n';
			};
			// 保護文字周辺は縮約に留め，日本語字・全角記号間の不要な空白だけ除去
			if(
				const bool IsRemovable =
				Last && Pos < Text.size() && Text[Pos] != '\n' && !IsLastProtected && !Protected[Pos] && Text.compare(Pos, 2, "//") &&
				!(Start > 1 && Text[Start - 1] == '/' && Text[Start - 2] == '/') && Text.compare(Pos, 2, "# ") && !IsSpacedMark(Last) &&
				!IsSpacedMark(Next);
				IsRemovable && !IsAfterDocTag() &&
				(IsFullWidthMark(Last) || IsFullWidthMark(Next) || IsFullWidth(Last) && IsFullWidth(Next) && !IsAfterChapter())
			) continue;
			// 行末と表区切り両側を除く連続空白の一文字への縮約
			Emit(Start, !Last || Pos >= Text.size() || Text[Pos] == '\n' || Last == '|' || Text[Pos] == '|' ? Pos : Start + 1);
			Last = 0;
			IsLastProtected = IsLastWord = IsLastLinkEnd = IsAttachedRun = false;
			continue;
		}
		uint32_t Point = 0;
		if(!TextEdit::DecodeUtf8(Text, Pos, Point)) Pos = Start + 1;
		// 書式の制御は字の判定を前の字の儘に引き継ぐ（`❤️OK` は `❤` と `O` の間）
		if(IsFormatControl(Point)) {
			Emit(Start, Pos);
			continue;
		}
		if(Char == '\n' || Protected[Start] || Depth || IsInLink || IsVerbatimLine) {
			// 保護・引用・リンク先・悪い例の行の文字の原文転写
			if(!Protected[Start] && Depth && Pos - Start == 3) IsQuoteDepthChar(Text, Start, Depth);
			IsLastLinkEnd = !Protected[Start] && IsInLink && Char == ')';
			if(IsLastLinkEnd) IsInLink = false;
			Emit(Start, Pos);
			Last = Char == '\n' ? 0 : Point;
			IsLastProtected = Protected[Start];
			IsLastWord = IsAttachedRun = false;
			continue;
		}
		// URL・経路を１語として保護し，経路の後の本文だけ分離（終端不明なら全体保持）
		const bool IsAfterSpace = !Last && (!Start || Text[Start - 1] == ' ' || Text[Start - 1] == '\t' || IsLineStart);
		const bool IsFullWidthLast = IsFullWidthWord(Last);
		if(
			const bool IsAfterWide = !IsLastProtected && IsFullWidthLast && std::isalpha(static_cast<unsigned char>(Char));
			IsAfterSpace || IsAfterWide
		) {
			// 長い経路の再探索を避ける次の空白・斜線・URL 印の再利用
			if(SpaceAt < Start) SpaceAt = Text.find_first_of(" \t\n", Start);
			if(SlashAt < Start) SlashAt = Text.find('/', Start);
			if(UrlAt < Start) UrlAt = Text.find("://", Start);
			size_t ChunkEnd = std::min(SpaceAt, Text.size());
			const std::string_view Chunk(Text.data() + Start, ChunkEnd - Start);
			const size_t Slash = SlashAt < ChunkEnd ? SlashAt - Start : std::string_view::npos;
			const bool IsUrl = UrlAt != std::string::npos && UrlAt + 3 <= ChunkEnd;
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
				// 塊内の引用開始も数え，後続の引用内空白を次の整形と同じく保護
				for(size_t At = Start; At + 3 <= ChunkEnd; ++At) IsQuoteDepthChar(Text, At, Depth);
				Pos = ChunkEnd;
				// 経路の末尾の半角の字を後続の境界判定へ渡す（全角で終わる経路も半角の語として扱う）
				const unsigned char Tail = static_cast<unsigned char>(Text[ChunkEnd - 1]);
				Last = Tail < 0X80 ? Tail : 'a';
				WordStart = Start;
				IsLastWord = true;
				HasLetterWord = HasLetterAt(Start);
				IsLastProtected = IsLastLinkEnd = IsAttachedRun = false;
				continue;
			}
		}
		// 半角語・半角括弧と全角字の境界へ空白を補完（`[説明](先)`・`<summary>` の記法は保持）
		const bool IsWord = Point < 0X80 ? IsWordChar(Char) : IsPhoneticLetter(Point) || IsHalfWidthQuantityMark(Point);
		if(IsWord && !IsLastWord) {
			// 新しい半角語の英字保持有無
			HasLetterWord = HasLetterAt(Start) || IsHalfWidthQuantityMark(Point);
			WordStart = Start;
		}
		const auto OpensLink = [&]() -> bool {
			// 同じ行の閉じ角括弧の位置の探索（前の探索位置が今の位置より前の時だけ探し直す）
			if(LinkClose < Start) LinkClose = std::min(Text.find_first_of("]\n", Start), Text.size());
			// 同じ行の閉じ角括弧の直後が丸括弧かの返戻
			return LinkClose < Text.size() && Text[LinkClose] == ']' && LinkClose + 1 < Text.size() && Text[LinkClose + 1] == '(';
		};
		// 前の語の端（英数字・`C++` の記号，`vector<int>` の様に英数字で始まる語の `>`）
		const bool IsLastWordEdge = IsWordEdgeChar(Last) || Last == '>' && std::isalnum(static_cast<unsigned char>(Text[WordStart]));
		// 英字・数字を含む半角の語と全角の語の境界の空白（数との間へ空白を置かない単位記号の前は除き，全角へ揃えない半角の数字も全角の語との間を区切る）
		const bool ShouldInsert = Last && !IsLastProtected && (
			!IsAttachedRun && IsLastWord && (HasLetterWord || IsHalfWidthQuantityMark(Last) || IsAsciiDigit(Last)) &&
			IsFullWidthWord(Point) && IsLastWordEdge &&
			!(Start - WordStart == 1 && std::isalpha(static_cast<unsigned char>(Text[WordStart])) && IsQuantityUnitAt(Start)) ||
			IsFullWidthLast && (
				IsWord && (HasLetterWord || IsAsciiDigit(Point)) &&
				(std::isalnum(static_cast<unsigned char>(Char)) || Char == '_' || Point > 0X7F) || Char == '(' || Char == '{' ||
				Char == '[' && !OpensLink()
			) || !IsAttachedRun && (Last == ')' || Last == ']' || Last == '}') && !IsLastLinkEnd && IsFullWidthWord(Point)
		);
		if(ShouldInsert) EmitSpace();
		// 開き前の空白に対応する閉じ後の空白補完
		bool IsSpacedCloser = false;
		// ６４段迄の括弧別の開き前空白有無のビット保持
		if((Char == '(' || Char == '[' || Char == '{') && !(Char == '(' && Last == ']')) {
			if(BracketDepth < 64) {
				SpacedBrackets =
				ShouldInsert || !Last ? SpacedBrackets | uint64_t{ 1 } << BracketDepth : SpacedBrackets & ~(uint64_t{ 1 } << BracketDepth);
			}
			++BracketDepth;
		} else if((Char == ')' || Char == ']' || Char == '}') && BracketDepth) {
			IsSpacedCloser = --BracketDepth < 64 && SpacedBrackets >> BracketDepth & 1;
		}
		// 全角直後に密着した記号始まりの半角列後の空白抑止
		if(IsFullWidth(Point) || IsSpacedCloser) IsAttachedRun = false;
		else if(!ShouldInsert && IsFullWidthLast) IsAttachedRun = true;
		if(Pos - Start == 3) IsQuoteDepthChar(Text, Start, Depth);
		IsInLink = Char == '(' && Last == ']';
		Emit(Start, Pos);
		Last = Point;
		// 通常文字転写後の保護・リンク末尾状態解除
		IsLastProtected = IsLastLinkEnd = false;
		IsLastWord = IsWord;
	}
	Text = std::move(Out);
	Protected = std::move(OutProtected);
	// 終了
	return;
}

/**
 * コメント本文の正規化関数（読点の全角カンマ化・文毎の論理行への分割と句点の除去・空白の規約への正規化・行末へ積んだ記号の前置区切りの統一・行末の空白の除去）
 * 処理系が読む注記は投影の段が役割を分け，此処へは通常コメントの本文だけが来る（整形仕様 3.1, 7.3）
 * @param Text 記号を除いた本文の論理行（破壊的に書き換えられる）
 * @param Protected コードの保護標識
 * @param StackMarker 行末へ積んだ次の文の始まりの字面（行記法の記号と空白，囲みは空）
 * @param SymbolHead 最初の論理行が始まる記号の一部の頭（行記法で最初の論理行から分けた行も継ぐ，無ければ空）
 * @param TagMarks 文書の札・指令を始める字
 * @param IsLineEnd 行末コメントの本文か（積んだ記号の前置区切りを揃える）
 */
void CommentNormalizer::NormalizeBody(
	std::string &Text,
	std::vector<unsigned char> Protected,
	const std::string_view StackMarker,
	const std::string_view SymbolHead,
	const std::string_view TagMarks,
	const bool IsLineEnd
) {
	// UTF-8 を確認出来ない本文は３バイト窓の誤検出を避け，記号の正規化を省く
	if(TextEdit::IsValidUtf8(Text)) {
		// 読点の全角カンマへの統一（説明文の引用にも記載先の表記を当て，括弧・引用の開きの直後に置いた記号の例は保つ）
		for(size_t Pos = Text.find("、"); Pos != std::string::npos; Pos = Text.find("、", Pos + 3)) {
			const bool IsExample = Pos && (Text[Pos - 1] == '(' || Text[Pos - 1] == '"') || Pos > 2 && std::any_of(
				std::begin(SentenceBrackets),
				std::end(SentenceBrackets),
				[&Text, Pos](const std::pair<std::string_view, std::string_view> &Bracket) -> bool {
					// 開きの直後かの返戻
					return Bracket.first.size() == 3 && !Text.compare(Pos - 3, 3, Bracket.first);
				}
			);
			if(!Protected[Pos] && !IsExample) Text.replace(Pos, 3, "，");
		}
		// 文境界を先に確定してからの文毎の論理行への分割と句点の除去（整形仕様 7.3）
		SplitSentences(Text, FindSentenceBoundaries(Text, Protected), StackMarker, SymbolHead, Protected);
		// 分割後の行で空白を調整し，経路・箇条書の判定を次の整形の行頭と統一
		NormalizeCommentSpaces(Text, TagMarks, Protected);
	}
	// コードの範囲の外の行末の空白と，行の終わりに読まない単独の CR の除去（CR はコードの範囲でも除き，出力の改行と CRLF を成さない，生成コードと同じ扱い）
	std::string Out;
	Out.reserve(Text.size());
	for(size_t LineStart = 0; LineStart <= Text.size();) {
		const size_t LineEnd = std::min(Text.find('\n', LineStart), Text.size());
		size_t Kept = LineEnd;
		while(
			Kept > LineStart && (Text[Kept - 1] == '\r' || (Text[Kept - 1] == ' ' || Text[Kept - 1] == '\t') && !Protected[Kept - 1])
		) --Kept;
		// 行末へ積んだ記号の前置区切りは，タブが在れば其の数のタブだけ，無ければ半角スペース１つ（行頭と空白を挟まない物を除く，整形仕様 7.1, 7.6）
		for(size_t At = LineStart; At < Kept;) {
			size_t End = At, Tabs = 0;
			for(; End < Kept && !Protected[End]; ++End) {
				if(Text[End] == '\t') ++Tabs;
				else if(!Text.compare(End, 3, "\xE3\x80\x80")) End += 2;
				else if(Text[End] != ' ') break;
			}
			if(
				IsLineEnd && !StackMarker.empty() && At > LineStart && End > At && End < Kept && !Protected[End] &&
				!Text.compare(End, StackMarker.size(), StackMarker)
			) Out.append(Tabs ? Tabs : 1, Tabs ? '\t' : ' ');
			else Out.append(Text, At, std::max(End, At + 1) - At);
			At = std::max(End, At + 1);
		}
		if(LineEnd == Text.size()) break;
		Out += '\n';
		LineStart = LineEnd + 1;
	}
	Text = std::move(Out);
	// 終了
	return;
}

/**
 * 行末へ積んだ次の文の始まりの字面の取得関数
 * @param Notation 記法
 * @return 行記法の記号と空白（行毎の記号を持たない囲み・埋込文書は空）
 */
std::string CommentNormalizer::StackMarkerOf(const CommentNotation Notation) {
	// 終端記号を持ち行毎の記号を持たない記法の返戻
	if(!Core::CommentRepository::CloserOf(Notation).empty()) return {};
	// 行記法の記号と空白の返戻
	return std::string(Core::CommentRepository::OpenerOf(Notation)) + ' ';
}

/**
 * 最初の論理行の取得関数（行末コメントの前置区切りを記録と対応させる鍵，整形仕様 8.1）
 * 改行又は空白の後に積んだ次の文の記号の前迄を，後の空白を除いて返す
 * @param Text 記号を除いた本文
 * @param StackMarker 行末へ積んだ次の文の始まりの字面（行記法の記号と空白，囲みは空）
 * @return 最初の論理行
 */
std::string_view CommentNormalizer::FirstLogicalLine(std::string_view Text, const std::string_view StackMarker) {
	Text = Text.substr(0, Text.find('\n'));
	for(
		size_t At = StackMarker.empty() ? std::string_view::npos : Text.find(StackMarker);
		At != std::string_view::npos;
		At = Text.find(StackMarker, At + 1)
	) if(At && (Text[At - 1] == ' ' || Text[At - 1] == '\t')) {
		Text = Text.substr(0, At);
		break;
	}
	while(!Text.empty() && (Text.back() == ' ' || Text.back() == '\t')) Text.remove_suffix(1);
	// 最初の論理行の返戻
	return Text;
}

/**
 * 記録の数の取得関数
 * @return 見方が持つ記録の数
 */
size_t ReadOnlyCommentView::GetSize() const {
	// 記録の数の返戻
	return Records.size();
}

/**
 * 記法の定義のコード例の書式の取得関数
 * @param Index 記録の位置
 * @return 記録が共有する文法のコード例の書式
 */
const Core::CodeExampleForms &ReadOnlyCommentView::GetCodeExamples(const size_t Index) const {
	// 記法の定義のコード例の書式の返戻
	return Core::CommentRepository::RulesOf(Records[Index]).CodeExamples;
}

/**
 * 本文の取得関数
 * @param Index 記録の位置
 * @return 記号を除いた本文
 */
std::string_view ReadOnlyCommentView::GetText(const size_t Index) const {
	// 本文の返戻
	return Records[Index].Text;
}

/**
 * 原文での順の取得関数
 * @param Index 記録の位置
 * @return 原文での順
 */
size_t ReadOnlyCommentView::GetOrder(const size_t Index) const {
	// 原文での順の返戻
	return Records[Index].OriginalOrder;
}

/**
 * 所属する群の取得関数
 * @param Index 記録の位置
 * @return 文書の位置で確定した所属（単独の記録は `NoCommentGroup`，本文を値として保つ指示コメントは無し）
 */
std::optional<Core::CommentGroupId> ReadOnlyCommentView::GetGroup(const size_t Index) const {
	// 群に属さない指示コメントの返戻
	if(Records[Index].Role == CommentRole::Directive) return std::nullopt;
	// 所属する群の返戻
	return Membership.empty() ? Records[Index].Format.Group : Membership[Index];
}

/**
 * 所属を与えるコンストラクタ
 * @param Source 判定する記録の並び（見方の存続中は書き換えない）
 * @param Groups 文書の位置で確定した記録毎の所属
 */
ReadOnlyCommentView::ReadOnlyCommentView(
	const std::span<const CommentRecord> Source,
	const std::span<const Core::CommentGroupId> Groups
) : Records(Source), Membership(Groups) {}

/**
 * 記録の群を所属とするコンストラクタ
 * @param Source 判定する記録の並び（見方の存続中は書き換えない）
 */
ReadOnlyCommentView::ReadOnlyCommentView(const std::span<const CommentRecord> Source) : Records(Source) {}

/**
 * 群毎の記録の位置の取得関数
 * @return 空でない群毎の記録の位置（原文の順）
 */
std::span<const std::vector<size_t>> CommentStructure::GetGroups() const {
	// 群毎の記録の位置の返戻
	return Groups;
}

/**
 * 記録の群が閉じないコード区画を持つかの判定関数
 * @param Index 記録の位置
 * @return 閉じないコード区画を持つ群の記録なら true
 */
bool CommentStructure::HasUnclosedCode(const size_t Index) const {
	// 閉じないコード区画の有無の返戻
	return IsUnclosed[Index];
}

/**
 * 記録の本文のコード例の保護標識の取得関数
 * @param Index 記録の位置
 * @return 本文の位置別の保護標識（群に属さない記録は保護しない）
 */
std::span<const unsigned char> CommentStructure::GetCodeMask(const size_t Index) const {
	// 保護標識の返戻
	return CodeMasks[Index];
}

/**
 * 記録の論理行毎の構造として保つ空の論理行の印の取得関数
 * @param Index 記録の位置
 * @return 論理行毎のコード例の中の空の論理行か，段落とコード例を分ける空の論理行か
 */
std::span<const uint8_t> CommentStructure::GetKeptBlanks(const size_t Index) const {
	// 空の論理行の印の返戻
	return KeptBlanks[Index];
}

/**
 * 群毎の記録の位置の取得関数
 * 見方の所属（文書の位置が確定した後の群，群を持たない記録は単独）毎に，指示コメントを除く記録を原文の順に並べる（同期で順の揃った記録は記録の並びの順）
 * 計算量：記録の数 C に対し O(C log C)
 * @param View 記録の読取専用の見方
 * @return 空でない群毎の記録の位置
 */
std::vector<std::vector<size_t>> CommentNormalizer::GroupsOf(const ReadOnlyCommentView &View) {
	// 群に属する記録（指示コメントを除く）の群と原文の順の並び
	std::vector<size_t> Order;
	Order.reserve(View.GetSize());
	for(size_t Index = 0; Index < View.GetSize(); ++Index) if(View.GetGroup(Index)) Order.push_back(Index);
	std::stable_sort(
		Order.begin(),
		Order.end(),
		[&View](const size_t Lhs, const size_t Rhs) -> bool {
			// 群と原文の順の比較の返戻
			return std::pair(*View.GetGroup(Lhs), View.GetOrder(Lhs)) < std::pair(*View.GetGroup(Rhs), View.GetOrder(Rhs));
		}
	);
	// 同じ群の範囲毎の群
	std::vector<std::vector<size_t>> Groups;
	for(size_t Begin = 0; Begin < Order.size();) {
		// 同じ群の範囲（群を持たない記録は単独）
		const Core::CommentGroupId Group = *View.GetGroup(Order[Begin]);
		size_t End = Begin + 1;
		if(Group != Core::NoCommentGroup) while(End < Order.size() && *View.GetGroup(Order[End]) == Group) ++End;
		Groups.emplace_back(Order.begin() + static_cast<std::ptrdiff_t>(Begin), Order.begin() + static_cast<std::ptrdiff_t>(End));
		Begin = End;
	}
	// 群毎の記録の位置の返戻
	return Groups;
}

/**
 * 本文構造の判定関数（整形仕様 3.3, 7.3, 7.4, 7.7）
 * 投影済の原文記録を文書位置の確定後に所属（`CommentFormat::Group` 又は見方が与える所属，整形仕様 3.3, 3.4）で群分けし，本文を改行で繋ぐ
 * コード例と，コード例中又は段落とコード例を分ける空の論理行を判定する
 * 見方の記録を読むだけで書き換えず，同じ記法・本文・順序・所属には同じ結果を返す（投影・同期・配置・静的検査・正規化が共有する）
 * 計算量：記録の数 C と本文の長さの和 L に対し O(C log C + L)
 * @param View 記録の読取専用の見方
 * @return 変更不能な判定結果
 */
CommentStructure CommentNormalizer::Inspect(const ReadOnlyCommentView &View) {
	// 記録毎の判定結果の初期化（群に属さない指示コメントは保護・保つ空の論理行を持たない）
	CommentStructure Structure;
	const size_t Count = View.GetSize();
	Structure.IsUnclosed.resize(Count, 0);
	Structure.CodeMasks.reserve(Count);
	Structure.KeptBlanks.reserve(Count);
	for(size_t Index = 0; Index < Count; ++Index) {
		const std::string_view Text = View.GetText(Index);
		Structure.CodeMasks.emplace_back(Text.size(), 0);
		Structure.KeptBlanks.emplace_back(std::ranges::count(Text, '\n') + 1, 0);
	}
	Structure.Groups = GroupsOf(View);
	// 連結本文の論理行と記録の論理行の対応
	struct LineRange {
		size_t Record; // 記録の位置
		size_t Line; // 記録の中の論理行
		size_t Offset; // 記録の本文の中の論理行の始まり
		size_t Start; // 連結本文の中の始まり
		size_t Copied; // 改行を含む転写の長さ
	};
	std::vector<LineRange> Lines;
	std::vector<uint8_t> KeptBlanks;
	for(const std::vector<size_t> &Group : Structure.Groups) {
		// 群の本文を改行で繋いだ物（各論理行の後に改行を置く）
		std::string Joined;
		Lines.clear();
		for(const size_t Index : Group) {
			const std::string_view Text = View.GetText(Index);
			for(size_t Start = 0, Line = 0; Start <= Text.size(); ++Line) {
				const size_t End = std::min(Text.find('\n', Start), Text.size());
				Lines.push_back({ Index, Line, Start, Joined.size(), End - Start + (End < Text.size()) });
				Joined.append(Text.substr(Start, End - Start)).push_back('\n');
				Start = End + 1;
			}
		}
		bool IsUnclosed = false;
		const std::vector<unsigned char> Protected =
		CommentCodeMask(Joined, View.GetCodeExamples(Group.front()), IsUnclosed, KeptBlanks);
		for(const size_t Index : Group) Structure.IsUnclosed[Index] = IsUnclosed;
		// 記録の本文の位置への保護標識と，記録の論理行への構造として保つ空の論理行の転写
		for(size_t Position = 0; Position < Lines.size(); ++Position) {
			const LineRange &Range = Lines[Position];
			std::copy_n(
				Protected.begin() + static_cast<std::ptrdiff_t>(Range.Start),
				Range.Copied,
				Structure.CodeMasks[Range.Record].begin() + static_cast<std::ptrdiff_t>(Range.Offset)
			);
			Structure.KeptBlanks[Range.Record][Range.Line] = KeptBlanks[Position];
		}
	}
	// 判定結果の返戻
	return Structure;
}

/**
 * 通常コメント本文の表記の正規化関数（整形仕様 3.3, 3.4, 7.3）
 * 文書の位置が確定した後の群 (`DocCommentSynchronizer::ConfirmGroups`) 毎に，`Inspect` のコード例の判定に従って本文を正規化する
 * 記法・種別・所属・役割は変えない（指示コメントは本文を保つ，整形仕様 7.5）
 * 計算量：コメントの数 C と本文の長さの和 L に対し O(C log C + L)
 * @param Comments 錨の節点毎のコメント（本文だけを更新する）
 */
void CommentNormalizer::Normalize(Core::CommentRepository &Comments) {
	const std::span<const CommentRecord> Records = Comments.GetRecords();
	const CommentStructure Structure = Inspect(ReadOnlyCommentView(Records));
	// 群の記録の位置と正規化した本文（群に属さない指示コメントは写さない）
	std::vector<std::pair<size_t, std::string>> Texts;
	// 群の記録毎のコード保護標識に従う正規化
	for(const std::vector<size_t> &Group : Structure.GetGroups()) for(const size_t Index : Group) {
		std::string Text = Records[Index].Text;
		const std::span<const unsigned char> Protected = Structure.GetCodeMask(Index);
		NormalizeBody(
			Text,
			{ Protected.begin(), Protected.end() },
			StackMarkerOf(Core::CommentRepository::OutputNotationOf(Records[Index])),
			Records[Index].Format.AttachFirstLine &&
			!Core::CommentRepository::IsBlockNotation(Core::CommentRepository::OutputNotationOf(Records[Index])) ?
			std::string(Core::CommentRepository::SymbolHeadOf(Records[Index], std::string_view(Text).substr(0, Text.find('\n')))) :
			std::string(),
			Core::CommentRepository::RulesOf(Records[Index]).CodeExamples.TagMarks,
			Records[Index].Kind == CommentKind::Trailing
		);
		Texts.emplace_back(Index, std::move(Text));
	}
	Comments.UpdateTexts(std::move(Texts));
	// 終了
	return;
}

/**
 * コメントの間の字面が空白だけかの判定関数
 * @param Gap 原文でのコメント間の文字列
 * @param AllowsBlankLines 間の空行を越えるか（出力で空行を除いて並べる同じ錨の同じ側のコメント）
 * @return 空白と改行（空行を越えない時は改行１つ）だけなら true
 */
bool CommentNormalizer::IsCommentGap(const std::string_view Gap, const bool AllowsBlankLines) {
	// 空白だけの隙間かの返戻
	return Gap.find_first_not_of(" \t\r\n") == std::string::npos &&
	(AllowsBlankLines || std::count(Gap.begin(), Gap.end(), '\n') == 1);
}

/**
 * 同じ群の隣接行コメント判定関数（整形仕様 3.3, 7.3）
 * @param First 先行コメントの群を纏める記法
 * @param Next 後続コメントの群を纏める記法
 * @param Gap 原文でのコメント間の文字列
 * @return 同じ行記法で改行１つだけを挟んで隣接するなら true
 */
bool CommentNormalizer::CanJoinComments(const CommentNotation First, const CommentNotation Next, const std::string_view Gap) {
	// 同じ群かの返戻
	return First == Next && !Core::CommentRepository::IsBlockNotation(First) && IsCommentGap(Gap, false);
}
