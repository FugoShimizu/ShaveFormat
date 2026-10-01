#include "ParseAdapter.hpp"
#include "../Core/CommentRepository.hpp"
#include "../Profile/GrammarProfile.hpp"
#include "CharClass.hpp"
#include "HtmlTag.hpp"
#include "SyntaxParser.hpp"
#include "TextEdit.hpp"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

/**
 * 字句化前に展開する Unicode の逃避の値の読取関数（Java の逆斜線と１個以上の `u` に続く１６進４桁）
 * @param Source 字面
 * @param Pos 逆斜線の位置（読んだ逃避の終わりへ進める）
 * @return 逃避の UTF-16 の値（逆斜線と `u` で始まらなければ無し，桁不足なら範囲外の値）
 */
std::optional<uint32_t> ParseAdapter::ReadUnicodeEscape(const std::string_view Source, size_t &Pos) {
	// 逃避で始まらない字面の返戻
	if(Pos + 1 >= Source.size() || Source[Pos] != '\\' || Source[Pos + 1] != 'u') return std::nullopt;
	// 重ねた u の読飛
	size_t Digits = Pos + 1;
	while(Digits < Source.size() && Source[Digits] == 'u') ++Digits;
	// 桁不足を範囲外の値で示す事の返戻
	if(Digits + 4 > Source.size()) return 0X110000;
	// １６進４桁の値
	uint32_t Value = 0;
	// 各１６進桁の走査
	for(size_t Digit = 0; Digit < 4; ++Digit) {
		// 現在の桁
		const char Char = Source[Digits + Digit];
		const int Hex = Char >= '0' && Char <= '9' ?
		Char - '0' :
		Char >= 'a' && Char <= 'f' ? Char + 10 - 'a' : Char >= 'A' && Char <= 'F' ? Char + 10 - 'A' : -1;
		// 桁不足の返戻
		if(Hex < 0) return 0X110000;
		// 次の桁の取込
		Value = Value << 4 | static_cast<uint32_t>(Hex);
	}
	// 逃避終端への移動
	Pos = Digits + 4;
	// 読んだ値の返戻
	return Value;
}

/**
 * 字句化前に改行へ展開する Unicode の逃避を持つかの判定関数
 * 連続した逆斜線の数が奇数の時だけ其の最後の逆斜線が逃避を始める
 * 計算量：字面の長さ L に対し O(L)
 * @param Text 字面
 * @return LF・CR へ展開する逃避を持てば true
 */
bool ParseAdapter::HasEscapedLineBreak(const std::string_view Text) {
	// 逆斜線の並びの走査
	for(size_t Pos = 0; Pos < Text.size();) {
		if(Text[Pos] != '\\') {
			++Pos;
			continue;
		}
		const size_t Begin = Pos;
		while(Pos < Text.size() && Text[Pos] == '\\') ++Pos;
		// 偶数本の逆斜線は互いに逃避し合う
		if(!((Pos - Begin) % 2)) continue;
		size_t End = Pos - 1;
		const std::optional<uint32_t> Value = ReadUnicodeEscape(Text, End);
		// LF・CR へ展開する逃避の返戻
		if(Value == '\n' || Value == '\r') return true;
		// 読んだ逃避の後（逃避で無ければ逆斜線の列の後）からの走査
		Pos = std::max(Pos, End);
	}
	// 改行へ展開する逃避を持たない事の返戻
	return false;
}

/**
 * Java の字句化前 Unicode エスケープの展開関数
 * @param Src 解析済の原文
 * @param Out 展開後の原文（展開する物が無ければ空の儘）
 * @return 桁不足のエスケープ，又はリテラル・コメントの外で対を成さないサロゲートのエスケープ（javac が拒む字句誤り）が在れば false
 * @throws FormatLimitExceeded 展開後の再解析が時間の上限を超える場合
 * @throws std::bad_alloc 展開後の再解析の解析器を用意出来ない場合
 */
bool ParseAdapter::ExpandJavaUnicodeEscapes(const ParsedTree &Src, std::string &Out) {
	// 解析済原文
	const std::string &Source = Src;
	// 逃避の印が無ければ展開する物が無い事の返戻（字面の印は此の工程の知識で，統括へ出さない）
	if(Source.find("\\u") == std::string::npos) return true;
	// 逃避１個（サロゲートの対は１文字に纏める）の原文の範囲・値・全て展開した原文での位置
	struct Escape {
		size_t Start;
		size_t End;
		size_t Expanded; // 逃避を全て展開した本文内の開始バイト位置
		uint32_t Value;
	};
	// 展開候補の逃避列
	std::vector<Escape> Escapes;
	// 原文字面の走査
	for(size_t Pos = 0; Pos < Source.size();) {
		// 逆斜線以外の場合
		if(Source[Pos] != '\\') {
			// 次のバイトへ移動
			++Pos;
			continue;
		}
		// 逆斜線列の先頭
		const size_t Begin = Pos;
		// 逆斜線列の終端探索
		while(Pos < Source.size() && Source[Pos] == '\\') ++Pos;
		// 偶数本の逆斜線は互いに逃避し合い，最後の逆斜線も展開の対象外
		if(!((Pos - Begin) % 2) || Pos >= Source.size() || Source[Pos] != 'u') continue;
		// 展開対象の逆斜線位置
		const size_t Start = Pos - 1;
		Pos = Start;
		// 最初の UTF-16 値
		uint32_t Value = ReadUnicodeEscape(Source, Pos).value();
		// 上位サロゲートは直後の下位サロゲートの逃避と組んで１文字に為る
		if(Value > 0XD7FF && Value < 0XDC00) {
			// 下位サロゲートの逆斜線位置
			size_t Next = Pos;
			if(const std::optional<uint32_t> Low = ReadUnicodeEscape(Source, Next); Low && *Low > 0XDBFF && *Low < 0XE000) {
				// 上位の偏り (0XD800 << 10) と下位の偏り 0XDC00 を除いて 0X10000 を足す（定数部を畳んだ値）
				Value = (Value << 10) + *Low - 0X35FDC00;
				// 対の終端への移動
				Pos = Next;
			}
		}
		// 桁不足は，リテラル・コメントの中でも字句化の前の展開で javac が拒む字句誤りの為，失敗の返戻
		if(Value > 0X10FFFF) return false;
		// 逃避範囲と値の記録
		Escapes.push_back({ .Start = Start, .End = Pos, .Expanded = 0, .Value = Value });
	}
	// 昇順の逃避と文書順のリテラル・コメントを１度突合して型を取得（コード内は空，逃避毎の祖先走査を回避）
	const auto ContextsOf = [&Src, &Escapes](const TSNode Root, const bool IsInExpanded) -> std::vector<Core::Role> {
		// 逃避位置に対応する字句種別の収集
		std::vector<Core::Role> Contexts;
		Contexts.reserve(Escapes.size());
		WalkChildrenCursor(
			Root,
			[&](const TSNode Node) -> bool {
				// 包む字句以外の子孫走査を続ける事の返戻
				if(!Src.Is(Node, Core::Role::UnicodeEscapeHost)) return true;
				const Core::Role Context =
				Src.Is(Node, Core::Role::EscapeLiteralHost) ? Core::Role::EscapeLiteralHost : Src.Is(Node, Core::Role::LineCommentNode) ?
				Core::Role::LineCommentNode :
				Src.Is(Node, Core::Role::BlockCommentNode) ? Core::Role::BlockCommentNode : Core::Role::Count;
				const size_t Start = ts_node_start_byte(Node), End = ts_node_end_byte(Node);
				// 未割当逃避の走査
				for(size_t Next = Contexts.size(); Next < Escapes.size(); ++Next) {
					const size_t At = IsInExpanded ? Escapes[Next].Expanded : Escapes[Next].Start;
					if(At + 1 > End) break;
					// 行コメントの開きを成す逃避は，コメント内部でなくコードとして展開
					Contexts.push_back(At < Start || Context == Core::Role::LineCommentNode && At < Start + 2 ? Core::Role::Count : Context);
				}
				// 包む字句の内側へ降りない事の返戻
				return false;
			}
		);
		Contexts.resize(Escapes.size(), Core::Role::Count);
		// 逃避毎の場所の返戻
		return Contexts;
	};
	// 其の場所で区切り・逃避・コメントの終端を作らず，字句の境界を変えない値かの判定
	const auto IsBenign = [](const Core::Role Context, const uint32_t Value) -> bool {
		// 字句の境界を変えない事の判定結果の返戻
		return Context == Core::Role::EscapeLiteralHost ?
		Value != '"' && Value != '\'' && Value != '\\' && Value != '\r' && Value != '\n' :
		Context == Core::Role::LineCommentNode ?
		Value != '\r' && Value != '\n' :
		Context == Core::Role::BlockCommentNode && Value != '*' && Value != '/';
	};
	// 展開前の木で全て境界を変えなければ，先に在る展開が後の場所を変える事も無い為，原文の儘で確定する
	const std::vector<Core::Role> Before = ContextsOf(Src.GetRoot(), false);
	// 先頭から安全な逃避数
	size_t Benign = 0;
	while(Benign < Escapes.size() && IsBenign(Before[Benign], Escapes[Benign].Value)) ++Benign;
	// 展開不要の返戻
	if(Benign == Escapes.size()) return true;
	// javac の字句化と同じく全て展開した原文を解析し直し，其の木で場所を判定する
	std::string Full;
	Full.reserve(Source.size());
	size_t Copied = 0;
	for(Escape &Sequence : Escapes) {
		// 逃避間の原文と代替字面の結合
		Full.append(Source, Copied, Sequence.Start - Copied);
		Sequence.Expanded = Full.size();
		// 対を成さないサロゲートの代替文字に依る位置保持
		TextEdit::EncodeUtf8(Sequence.Value > 0XD7FF && Sequence.Value < 0XE000 ? 0XFFFD : Sequence.Value, Full);
		Copied = Sequence.End;
	}
	Full.append(Source, Copied, std::string::npos);
	const ParsedTree Reparsed(Src.GetProfile(), std::move(Full));
	// 解析器を用意出来ないのは資源の枯渇で，整形を諦める（期限に達した解析は見送りの理由を分けて伝える）
	if(!Reparsed.IsParsed()) {
		if(Reparsed.HasExpiredParse()) throw FormatLimitExceeded("parsing exceeds the time limit");
		throw std::bad_alloc();
	}
	const std::vector<Core::Role> After = ContextsOf(Reparsed.GetRoot(), true);
	// 再解析で確定した文脈に応じた逃避の展開
	Copied = 0;
	// 各逃避の走査
	for(size_t Idx = 0; Idx < Escapes.size(); ++Idx) {
		const Escape &Sequence = Escapes[Idx];
		if(IsBenign(After[Idx], Sequence.Value)) {
			// 解析器はリテラルの中の u を重ねた逃避を逃避と読まない為，u を１個に揃える（値は変わらない）
			if(
				After[Idx] != Core::Role::EscapeLiteralHost ||
				std::string_view(Source).substr(Sequence.Start, Sequence.End - Sequence.Start).find("uu") == std::string_view::npos
			) continue;
			Out.append(Source, Copied, Sequence.Start - Copied);
			// 対を成す各逃避の正規化
			for(size_t Pos = Sequence.Start; Pos < Sequence.End; Pos += 4) {
				Pos = Source.find_first_not_of('u', Pos + 1);
				Out += "\\u";
				Out.append(Source, Pos, 4);
			}
			Copied = Sequence.End;
			continue;
		}
		// 対を成さないサロゲートをリテラル・コメント内で保持し，外への不正な展開を失敗とする事の返戻
		if(Sequence.Value > 0XD7FF && Sequence.Value < 0XE000) return false;
		// javac と同じ UTF-8 で展開し，解析器がコメント内で改行と読めない CR は LF 化（直後の LF と組む物は１改行へ）
		Out.append(Source, Copied, Sequence.Start - Copied);
		if(
			const bool IsCrBeforeLf = Sequence.Value == '\r' && (
				Sequence.End < Source.size() && Source[Sequence.End] == '\n' ||
				Idx + 1 < Escapes.size() && Escapes[Idx + 1].Start == Sequence.End && Escapes[Idx + 1].Value == '\n'
			);
			!IsCrBeforeLf
		) TextEdit::EncodeUtf8(Sequence.Value == '\r' ? '\n' : Sequence.Value, Out);
		Copied = Sequence.End;
	}
	if(Copied) Out.append(Source, Copied, std::string::npos);
	// 字句誤りが無かった事の返戻
	return true;
}

/**
 * Ruby の埋込ドキュメントの誤読の回避関数
 * Ruby と解析器で異なる埋込ドキュメントの終端判定を補正する
 * 早く閉じると残りの本文をコードと読み，整形で構文を壊す為，本文中の `=end` の `=` の直後へ原文に無い制御文字を挟んで解析し直す
 * 挟んだ文字はコメントの分離で本文から除く (`SourceProjector::Project`)
 * @param Src 解析済の原文
 * @param Marker 挟んだ制御文字の格納先（挟まない場合は '\0'）
 * @return 印を挟んだ原文（挟む箇所が無い場合は空，原文が候補の制御文字を全て含み印を選べない場合は無し）
 */
std::optional<std::string> ParseAdapter::EscapeRubyEmbeddedDocs(const ParsedTree &Src, char &Marker) {
	// 解析済原文
	const std::string &Source = Src;
	Marker = '\0';
	// 埋込文書の開始記号と終端記号
	const std::string_view Opener = Core::CommentRepository::OpenerOf(CommentNotation::RubyEmbeddedDocument);
	const std::string_view Closer = Core::CommentRepository::CloserOf(CommentNotation::RubyEmbeddedDocument);
	// 埋込文書の印が無ければ退避する物が無い事の返戻（字面の印は此の工程の知識で，統括へ出さない）
	if(!Src.IsParsed() || Source.find(Opener) == std::string::npos) return std::string();
	std::vector<size_t> Escapes;
	size_t DocEnd = 0;
	WalkChildrenCursor(
		Src.GetRoot(),
		[&](const TSNode Node) -> bool {
			// コメント以外の子孫走査を続ける事の返戻
			if(!Src.Is(Node, Core::Role::Comment)) return true;
			const size_t Start = Src.Start(Node);
			// 前の埋込ドキュメントの内側と，行頭の `=begin` で始まらないコメントは対象外の返戻
			if(
				Start < DocEnd || Start && Source[Start - 1] != '\n' ||
				!Core::CommentRepository::IsLineHeadWord(std::string_view(Source).substr(Start), Opener)
			) return false;
			const size_t Found = Escapes.size();
			for(size_t LineEnd = Source.find('\n', Start); LineEnd != std::string::npos;) {
				const size_t Head = LineEnd + 1;
				LineEnd = Source.find('\n', Head);
				if(Core::CommentRepository::IsLineHeadWord(std::string_view(Source).substr(Head), Closer)) {
					DocEnd = Head;
					// Ruby が閉じる行頭の `=end` に行着いた場合の走査打切の返戻
					return false;
				}
				// 行の中だけを探す（行に無い時に文書の終わり迄探すと，長い埋込ドキュメントで二乗の費用に為る）
				const std::string_view Line(Source.data() + Head, (LineEnd == std::string::npos ? Source.size() : LineEnd) - Head);
				for(size_t Pos = Line.find(Closer); Pos != std::string_view::npos; Pos = Line.find(Closer, Pos + Closer.size())) {
					Escapes.push_back(Head + Pos + 1);
				}
			}
			// 閉じの無い埋込ドキュメント（Ruby の構文誤り）は挟まない為，控えを取り消す
			Escapes.resize(Found);
			// コメントの内側へ降りない事の返戻
			return false;
		}
	);
	// 印を挟んだ再解析用原文
	std::string Escaped;
	// 挟む箇所が無い場合の空の返戻
	if(Escapes.empty()) return Escaped;
	// 印は原文に無い制御文字から選び，本文へ戻す時に原文の字面と取り違えない様にする
	Marker = TextEdit::AbsentControlChar(Source);
	// 印を選べず誤読を補修出来ない事の返戻
	if(!Marker) return std::nullopt;
	// 印の数を含む領域確保
	Escaped.reserve(Source.size() + Escapes.size());
	// 誤読する終端の直後への印の挿入
	size_t Copied = 0;
	for(const size_t Pos : Escapes) {
		Escaped.append(Source, Copied, Pos - Copied);
		Escaped += Marker;
		Copied = Pos;
	}
	Escaped.append(Source, Copied, std::string::npos);
	// 印を挟んだ原文の返戻
	return Escaped;
}

/**
 * 前処理の条件を跨ぐ `extern "C"` の波括弧の印への置換関数
 * 対を成す物だけを印の指令（`#pragma <印>O` / `#pragma <印>C`）へ置き換えて解析させ，RestoreLinkageGuards で戻す
 * @param Rules 入力の読取規則（条件を開く指令の名前）
 * @param Source 原文（印に使う制御文字を選ぶ）
 * @param Text 整理中の原文（行継続を繋いだ後，置換は此処へ行う）
 * @param Marker 印の格納先（既に選んで有れば其れを使う）
 * @param Escaped 退避したコメント（置換の後の位置へ動かす）
 * @return 置き換えた場合 true（置き換える対が在っても原文が候補の制御文字を全て含み印を選べない場合は無し）
 */
std::optional<bool> ParseAdapter::MaskLinkageGuards(
	const ReadingRules &Rules,
	const std::string &Source,
	std::string &Text,
	char &Marker,
	std::vector<EscapedComment> &Escaped
) {
	// 字面に無ければ走査を省く事の返戻
	if(Text.find(Rules.LinkageHead) == std::string::npos) return false;
	const std::string_view BlockOpen = Core::CommentRepository::OpenerOf(CommentNotation::Block);
	const std::string_view BlockClose = Core::CommentRepository::CloserOf(CommentNotation::Block);
	const std::string_view LineOpen = Core::CommentRepository::OpenerOf(CommentNotation::SlashLine);
	// コメントを空白へ置き換えた写し（位置は原文と同じ，コメントの有無で照合を変えない）
	std::string Blank(Text);
	// 字面を保った儘のコメント区間空白化
	for(size_t Pos = 0; Pos < Blank.size(); ++Pos) {
		// 現在字の分類とリテラルの読飛
		if(const char Char = Blank[Pos]; Char == '"' || Char == '\'') {
			// 文字列・文字リテラル内のコメント記号の読飛
			for(++Pos; Pos < Blank.size() && Blank[Pos] != Char && Blank[Pos] != '\n'; ++Pos) if(Blank[Pos] == '\\') ++Pos;
			continue;
		}
		const bool IsLine = !Blank.compare(Pos, LineOpen.size(), LineOpen);
		if(!IsLine && Blank.compare(Pos, BlockOpen.size(), BlockOpen)) continue;
		// コメント終端の決定と範囲の空白化
		const size_t Close = IsLine ? Blank.find('\n', Pos) : Blank.find(BlockClose, Pos + BlockOpen.size());
		const size_t End = Close == std::string::npos ? Blank.size() : IsLine ? Close : Close + BlockClose.size();
		for(; Pos < End; ++Pos) if(Blank[Pos] != '\n') Blank[Pos] = ' ';
		--Pos;
	}
	// 空行を除いた各行のインデントを除いた中身（整形で指令の前後に置く空行は並びを変えない）
	std::vector<std::string_view> Lines;
	for(size_t Start = 0; Start < Blank.size();) {
		// 各行の非空白本文抽出
		const size_t End = std::min(Blank.find('\n', Start), Blank.size());
		if(const size_t Head = Blank.find_first_not_of(" \t", Start); Head < End) Lines.emplace_back(Blank.data() + Head, End - Head);
		Start = End + 1;
	}
	const auto Directive = [](const std::string_view Content) -> std::string_view {
		// 指令でない場合の返戻
		if(!Content.starts_with('#')) return {};
		// 指令名候補の開始位置設定
		size_t Start = 1;
		// 指令印後の水平空白読飛
		while(Start < Content.size() && (Content[Start] == ' ' || Content[Start] == '\t')) ++Start;
		// 指令名の終端探索
		size_t End = Start;
		while(End < Content.size() && IsIdentifierChar(Content[End])) ++End;
		// 指令名の返戻
		return Content.substr(Start, End - Start);
	};
	struct Guard {
		size_t Start; // 置き換える字句の開始位置
		size_t Length; // 置き換える字句の長さ（後続のコメントは残す）
		bool IsOpen; // `extern "C" {` か（偽は `}`）
	};
	// 置換候補列の準備
	std::vector<Guard> Guards;
	// 前処理条件１組に挟まれた連結指定の字句だけを候補化
	for(size_t Index = 1; Index + 1 < Lines.size(); ++Index) {
		// 前後指令と其の間の本体の照合
		const std::string_view Body = Lines[Index];
		if(
			std::ranges::find(Rules.ConditionDirectives, Directive(Lines[Index - 1])) == Rules.ConditionDirectives.end() ||
			Directive(Lines[Index + 1]) != Rules.ConditionEndDirective
		) continue;
		// 字句を空白を除いて照合する（コメントは写しで空白に為って居る）
		std::string_view Code = Body;
		while(!Code.empty() && (Code.back() == ' ' || Code.back() == '\t')) Code.remove_suffix(1);
		std::string Compact;
		for(const char Char : Code) if(Char != ' ' && Char != '\t') Compact += Char;
		if(Compact != Rules.CompactLinkageOpen && Compact != Rules.LinkageClose) continue;
		Guards.push_back({ static_cast<size_t>(Body.data() - Blank.data()), Code.size(), Compact != Rules.LinkageClose });
	}
	// 候補波括弧の均衡検査
	size_t Depth = 0;
	for(const Guard &Entry : Guards) {
		if(Entry.IsOpen) ++Depth;
		// 開いて居ない閉じが有る場合の失敗の返戻
		else if(!Depth--) return false;
	}
	// 対を成さない場合の返戻
	if(Guards.empty() || Depth) return false;
	// 印を選べず誤読を補修出来ない事の返戻
	if(!Marker && !(Marker = TextEdit::AbsentControlChar(Source))) return std::nullopt;
	// 前方位置保持の為の後方字句から印への置換
	// 置換先は行を占める前処理指令に為り，指令は１論理行を保つ為に折返の候補へ入らない（整形仕様 2.1, 4.2 の幅の計数へ印を残さない）
	for(size_t Index = Guards.size(); Index--;) {
		const Guard &Entry = Guards[Index];
		const std::string Placeholder = std::string(Rules.LinkageMarkerDirective) + Marker + (Entry.IsOpen ? 'O' : 'C');
		// 字句の後の同じ行のコメント（連結指定はコードの為，指令の中へ残さず解析器へ渡さずに退避する，配置はコメント経路が決める）
		const size_t LineEnd = std::min(Text.find('\n', Entry.Start), Text.size());
		const size_t CommentStart = std::min(Text.find_first_not_of(" \t", Entry.Start + Entry.Length), LineEnd);
		const size_t Removed = LineEnd - (Entry.Start + Entry.Length);
		// 退避したコメントの位置の置換後の位置への移動
		for(EscapedComment &Comment : Escaped) if(Comment.Position > Entry.Start) {
			Comment.Position = static_cast<uint32_t>(Comment.Position + Placeholder.size() - Entry.Length - Removed);
		}
		// 同じ行のコメントの退避
		if(CommentStart < LineEnd) {
			Escaped.push_back(
				{
					static_cast<uint32_t>(Entry.Start + Placeholder.size()),
					Text.substr(CommentStart, Text.find_last_not_of(" \t", LineEnd - 1) + 1 - CommentStart),
					CommentKind::Trailing
				}
			);
		}
		Text.replace(Entry.Start, Entry.Length + Removed, Placeholder);
	}
	// 置き換えた事の返戻
	return true;
}

/**
 * C / C++ の行継続と前処理指令の整理関数（構文解析より前に原文の字面で行う）
 * 生文字列外の行継続を翻訳段階と同様に除去する関数
 * 囲みコメントの中はコメントの外の意味に関わらない為，字面を保つ
 * 前処理指令の本体は，tree-sitter-c が読み違えるコメントの開きの形を整える（本体のコメントを参照）
 * @param Rules 入力の読取規則（前処理指令の名前）
 * @param Source 原文（改行は LF へ正規化済）
 * @param Marker 置き換えた制御文字の格納先（置き換えなければ '\0'）
 * @param HasStrayBackslash 後の字句が次の行へ移ると行末に立ち，原文に無い行継続に為る裸の逆斜線を含むかの格納先（整形を見送る印）
 * @param Escaped 解析器へ渡さずに退避したコメントの格納先（位置は整理後の原文の位置）
 * @return 整理後の原文（変更が無ければ空，印が要るのに原文が候補の制御文字を全て含み選べない場合は無し）
 */
std::optional<std::string> ParseAdapter::PrepareCSource(
	const ReadingRules &Rules,
	const std::string &Source,
	char &Marker,
	bool &HasStrayBackslash,
	std::vector<EscapedComment> &Escaped
) {
	// 出力状態の初期化
	Marker = '\0';
	const size_t Size = Source.size();
	const std::string_view BlockOpen = Core::CommentRepository::OpenerOf(CommentNotation::Block);
	const std::string_view BlockClose = Core::CommentRepository::CloserOf(CommentNotation::Block);
	const std::string_view LineOpen = Core::CommentRepository::OpenerOf(CommentNotation::SlashLine);
	// 水平空白を飛ばした位置
	const auto SkipHorizontal = [&Source, Size](size_t At) -> size_t {
		// 水平空白の読飛
		while(
			At < Size && (Source[At] == ' ' || Source[At] == '\t' || Source[At] == '\f' || Source[At] == '\v' || Source[At] == '\r')
		) ++At;
		// 水平空白の後の位置の返戻
		return At;
	};
	// 位置から後が水平空白とコメントだけで行が終わるか（改行を含む囲みコメントも行を終える物とする）
	const auto EndsLineAfter = [&Source, Size, &SkipHorizontal, BlockOpen, BlockClose, LineOpen](size_t At) -> bool {
		while(true) {
			At = SkipHorizontal(At);
			// 行の終わりか行コメントに達した事の返戻
			if(At >= Size || Source[At] == '\n' || !Source.compare(At, LineOpen.size(), LineOpen)) return true;
			// 字句が続く事の返戻
			if(Source.compare(At, BlockOpen.size(), BlockOpen)) return false;
			const size_t Close = Source.find(BlockClose, At + BlockOpen.size());
			// 閉じない囲みコメントか改行を含む囲みコメントの返戻
			if(Close == std::string::npos || Source.find('\n', At) < Close) return true;
			At = Close + BlockClose.size();
		}
	};
	// 行継続（`\` + 水平空白 + 改行）の長さ（行継続でなければ０）
	const auto SpliceLength = [&Source, Size, &SkipHorizontal](const size_t At) -> size_t {
		// 逆斜線でない場合の返戻
		if(Source[At] != '\\') return 0;
		// 行継続候補の水平空白の後の位置
		const size_t End = SkipHorizontal(At + 1);
		// 改行で終わる場合だけ行継続の長さの返戻
		return End < Size && Source[End] == '\n' ? End - At + 1 : 0;
	};
	const auto NextLogical = [&SpliceLength, Size](size_t At) -> size_t {
		// 連続する行継続の読飛
		// 次の物理断片への前進
		for(size_t Length; At < Size && (Length = SpliceLength(At));) At += Length;
		// 次の字の位置の返戻
		return At;
	};
	std::string Out;
	Out.reserve(Size);
	HasStrayBackslash = false;
	bool IsChanged = false, IsLineHead = true, IsInDirective = false, IsHeaderName = false, IsDefine = false, IsInParams = false;
	// 指令の生の本文（マクロの本体・`#pragma` の引数等，解析器が中のコメントで本文を切る所）の始まり（Out 上，無ければ npos）
	size_t Pos = 0, LastToken = 0, DirectiveTokens = 0, ParamDepth = 0, RawFrom = std::string::npos;
	// 指令の中の囲みコメントの範囲（Out 上）と，原文で独立コメント（行の頭と末尾の両方に在る）だったか
	// 原文での囲みコメントの範囲と行頭の該当（左から右への走査で順に積む為，始まりも終わりも昇順）
	std::vector<std::tuple<size_t, size_t, bool>> Comments, SourceComments;
	// 位置の前が行の頭か（同じ行の前に囲みコメントだけが並ぶ物も行の頭とする）
	const auto IsCommentLineHead = [&Source, &SourceComments](size_t At) -> bool {
		while(At && (Source[At - 1] == ' ' || Source[At - 1] == '\t')) --At;
		// 行の頭に達した事の返戻
		if(!At || Source[At - 1] == '\n') return true;
		// 終わりが此の位置に当たる囲みコメント（鎖を毎回遡ると二乗に為る為，積む時に継いだ該当を引く）
		const std::vector<std::tuple<size_t, size_t, bool>>::const_iterator Prev = std::lower_bound(
			SourceComments.begin(),
			SourceComments.end(),
			At,
			[](const std::tuple<size_t, size_t, bool> &Range, const size_t Pos) -> bool {
				// コメントの終わりとの比較の返戻
				return std::get<1>(Range) < Pos;
			}
		);
		// 前の囲みコメントの行頭の該当の返戻
		return Prev != SourceComments.end() && std::get<1>(*Prev) == At && std::get<2>(*Prev);
	};
	// 行の末尾の該当を求めた位置（鎖の全ての位置が同じ答に為る為，一度辿ったら控えて二乗を防ぐ）
	std::unordered_map<size_t, bool> LineEndOf;
	// 位置の後が原文の行の末尾か（同じ行の後に空白・コメント・行継続の `\` だけが続く物も行の末尾とする）
	const auto IsCommentLineEnd = [&Source, Size, &SpliceLength, &LineEndOf, BlockOpen, BlockClose, LineOpen](size_t At) -> bool {
		// 答の定まらない儘に辿った位置
		std::vector<size_t> Path;
		bool Result = false;
		while(true) {
			// 控え済の位置での打切り
			if(const std::unordered_map<size_t, bool>::const_iterator Found = LineEndOf.find(At); Found != LineEndOf.end()) {
				Result = Found->second;
				break;
			}
			Path.push_back(At);
			while(At < Size && (Source[At] == ' ' || Source[At] == '\t')) ++At;
			// 行の末尾（行継続を含む）と，行の末尾迄続く後の行コメントへ達した事の記録
			if(At >= Size || Source[At] == '\n' || SpliceLength(At) || !Source.compare(At, LineOpen.size(), LineOpen)) {
				Result = true;
				break;
			}
			// 後が囲みコメントで始まらない事の記録
			if(Source.compare(At, BlockOpen.size(), BlockOpen)) break;
			// 後の囲みコメントの終わり
			const size_t Close = Source.find(BlockClose, At + BlockOpen.size());
			// 閉じの無い事の記録
			if(Close == std::string::npos) break;
			At = Close + BlockClose.size();
		}
		// 辿った位置への答の記入
		for(const size_t Visited : Path) LineEndOf.emplace(Visited, Result);
		// 行の末尾の該当の返戻
		return Result;
	};
	const auto SkipSplices = [&]() -> void {
		// 現在位置から次の論理字句の位置
		const size_t Next = NextLogical(Pos);
		// 行継続の無い位置での終了
		if(Next == Pos) return;
		// 逃避した逆斜線の後の行継続は，後が空行か原稿の末尾の時だけ畳んだ跡の逆斜線が行末に立ち，原文に無い行継続が次の行をマクロの本体へ消す
		// 忠実な出力が原文其の物しか無い為，整形を見送る印を立てる（後に字句が続けば逆斜線は行の途中に立ち危険は無い）
		if(const size_t After = SkipHorizontal(Next); Pos && Source[Pos - 1] == '\\' && (After >= Size || Source[After] == '\n')) {
			HasStrayBackslash = true;
		}
		Pos = Next;
		IsChanged = true;
	};
	// 印が要るのに選べなかったか
	bool IsUnmarkable = false;
	const auto MaskLiteral = [&](const size_t From) -> void {
		// リテラル中に現れる囲みコメント開始記号の探索
		for(size_t Found = Out.find(BlockOpen, From); Found != std::string::npos; Found = Out.find(BlockOpen, Found + 1)) {
			if(!Marker && !(Marker = TextEdit::AbsentControlChar(Source))) {
				IsUnmarkable = true;
				// 印を選べない事の終了
				return;
			}
			// 開始記号を解析不能な印へ置換
			Out[Found + 1] = Marker;
			IsChanged = true;
		}
	};
	// 後に指令のコードが続く囲みコメント（文中コメント）の，解析器が読める形への調整（行末の物は其の儘）
	const auto EndDirective = [&]() -> void {
		// 指令状態の解除
		IsInDirective = false;
		// 位置を保つ後の物からの処理
		for(size_t Idx = Comments.size(); Idx--;) {
			const auto [Begin, End, IsStandalone] = Comments[Idx];
			// 最後の字句より後の物は指令の行末のコメントとして其の儘（独立の物は指令が一行に繋がって独立で居れない為に下で除く）
			if(End > LastToken && !IsStandalone) continue;
			// 生の本文の単一行の物は印で覆い，本文の字面として其の位置に保つ（出力で印を戻す）
			if(
				!IsStandalone && !std::memchr(Out.data() + Begin, '\n', End - Begin) && Begin >= RawFrom &&
				(Marker || (Marker = TextEdit::AbsentControlChar(Source)))
			) {
				Out[Begin + 1] = Marker;
				IsChanged = true;
			} else if(IsStandalone || std::memchr(Out.data() + Begin, '\n', End - Begin) || Begin >= RawFrom) {
				// 改行を含む物（指令を行の途中で終える）と，印を選べない生の本文の物と，指令が一行に繋がって独立で居れない物は，解析器へ渡さずに退避し跡に区切の空白を置く（置けるかはコメント経路が決める）
				const size_t Length = End - Begin;
				for(EscapedComment &Later : Escaped) if(Later.Position > Begin) Later.Position -= static_cast<uint32_t>(Length - 1);
				Escaped.push_back(
					{ static_cast<uint32_t>(Begin), Out.substr(Begin, Length), IsStandalone ? CommentKind::OwnLine : CommentKind::Inline }
				);
				Out.replace(Begin, Length, " ");
				IsChanged = true;
			}
		}
		Comments.clear();
		// 行末の水平空白の除去（本体が空白で終わる指令は tree-sitter-c が次の行を本体へ取り込む為，畳んだ跡とコメントの跡を詰める）
		// 水平空白しか無い時の `npos` の折返しで出力の全体を捨てない様に，見付かった時だけ詰める
		if(const size_t Last = Out.find_last_not_of(" \t"); Last != std::string::npos && Last + 1 < Out.size()) {
			Out.resize(Last + 1);
			IsChanged = true;
		}
	};
	// 論理字句単位の原文走査
	while(true) {
		SkipSplices();
		if(Pos >= Size) break;
		// 現在の論理字の取得
		const char Char = Source[Pos];
		// 行継続を飛ばした後に残る裸の逆斜線は，後の字句が次の行へ移ると行末に立ち，原文に無い行継続に為る
		// 元々 C として不正な字面で，忠実な出力が原文其の物しか無い為，整形を見送る印を立てる
		// 識別子やリテラルへ書けるユニバーサル文字名 (`\u00E9` / `\U0001F600`) は字が続く為に行末へ立たず，除く
		// 指令は１論理行の儘折らない為，後に同じ行の字句が続く物は行末に立たない（後が空白とコメントだけの物はコメントを分離すると行末に立つ）
		if(
			Char == '\\' && (Pos + 1 >= Size || Source[Pos + 1] != 'u' && Source[Pos + 1] != 'U') &&
			(!IsInDirective || EndsLineAfter(Pos + 1))
		) HasStrayBackslash = true;
		if(Char == '\n') {
			// 論理行終端の確定
			if(IsInDirective) EndDirective();
			Out += Char;
			++Pos;
			IsLineHead = true;
			continue;
		}
		if(Char == ' ' || Char == '\t' || Char == '\f' || Char == '\v' || Char == '\r') {
			// 水平空白の逐語複写
			Out += Char;
			++Pos;
			continue;
		}
		// 字句種別判定の準備
		const bool IsAtLineHead = IsLineHead;
		IsLineHead = false;
		// コメント開始記号候補の次の論理字位置取得
		const size_t Second = Char == '/' || Char == '.' ? NextLogical(Pos + 1) : Size;
		// 囲みコメントの抽出
		if(Second < Size && Source[Second] == '*' && Char == '/') {
			// 囲みコメントの出力開始位置と原文の開始位置の記録
			const size_t CommentStart = Out.size(), Start = Pos;
			if(Second != Pos + 1) IsChanged = true;
			Out += Core::CommentRepository::OpenerOf(CommentNotation::Block);
			Pos = Second + 1;
			// コメント閉鎖状態の初期化
			bool IsClosed = false;
			// 囲みコメント終端迄の逐語複写
			while(Pos < Size && !IsClosed) {
				if(Source[Pos] == '*') if(const size_t Slash = NextLogical(Pos + 1); Slash < Size && Source[Slash] == '/') {
					if(Slash != Pos + 1) IsChanged = true;
					Out += Core::CommentRepository::CloserOf(CommentNotation::Block);
					Pos = Slash + 1;
					IsClosed = true;
					continue;
				}
				Out += Source[Pos++];
			}
			// 原文での囲みコメントの範囲と行頭の該当の記録（行の頭の判定が直前のコメントから継いだ該当を引く）
			if(IsClosed) SourceComments.emplace_back(Start, Pos, IsCommentLineHead(Start));
			// 指令内で閉じたコメント範囲の記録
			if(IsInDirective && IsClosed) {
				Comments.emplace_back(CommentStart, Out.size(), IsCommentLineHead(Start) && IsCommentLineEnd(Pos));
			}
			// 行頭コメント後の # の指令開始扱い
			IsLineHead = IsAtLineHead && !IsInDirective;
			continue;
		}
		// 行コメント開始の検出
		if(Second < Size && Source[Second] == '/' && Char == '/') {
			// 指令の継続行の頭の行コメント（行の末尾迄続く独立コメント）は，指令が一行に繋がって独立で居れない為に解析器へ渡さずに退避する（置けるかはコメント経路が決める，整形仕様 7.6）
			const bool IsEscaped = IsInDirective && IsCommentLineHead(Pos);
			// 行コメント：行継続の除去と次の行の本文への結合
			if(Second != Pos + 1 || IsEscaped) IsChanged = true;
			std::string Comment(LineOpen);
			Pos = Second + 1;
			for(SkipSplices(); Pos < Size && Source[Pos] != '\n'; SkipSplices()) Comment += Source[Pos++];
			if(IsEscaped) Escaped.push_back({ static_cast<uint32_t>(Out.size()), std::move(Comment), CommentKind::OwnLine });
			else Out += Comment;
			continue;
		}
		// 通常字句の出力開始位置記録
		const size_t TokenStart = Out.size();
		// 論理行の先頭で前処理指令の状態を初期化
		if(IsAtLineHead && Char == '#') {
			IsInDirective = true;
			DirectiveTokens = ParamDepth = 0;
			IsHeaderName = IsDefine = IsInParams = false;
			RawFrom = std::string::npos;
		}
		// 引用形式に応じたリテラル又は見出名の処理
		if(Char == '"' || Char == '\'' || IsHeaderName && Char == '<') {
			// リテラルと見出名からの行継続除去と未終端時の行末打切
			const char Close = Char == '<' ? '>' : Char;
			Out += Char;
			++Pos;
			// 引用符迄のリテラル逐語複写
			for(SkipSplices(); Pos < Size && Source[Pos] != '\n'; SkipSplices()) {
				const char Inner = Source[Pos++];
				Out += Inner;
				if(Inner == Close) break;
				if(Inner != '\\' || Close == '>') continue;
				SkipSplices();
				if(Pos < Size && Source[Pos] != '\n') Out += Source[Pos++];
			}
			// 指令内リテラルに含まれるコメント印の無効化
			if(IsInDirective) MaskLiteral(TokenStart);
		} else if(IsWordChar(Char)) {
			// 識別子字句の収集
			for(SkipSplices(); Pos < Size && IsWordChar(Source[Pos]); SkipSplices()) Out += Source[Pos++];
			const std::string_view Ident(Out.data() + TokenStart, Out.size() - TokenStart);
			// 生文字列（`R"delim(…)delim"`）：翻訳段階２が取り消される為，中身の逐語複写
			if(
				const bool IsRawPrefix = std::ranges::find(Rules.RawStringPrefixes, Ident) != Rules.RawStringPrefixes.end();
				IsRawPrefix && Pos < Size && Source[Pos] == '"'
			) if(
				const size_t Open = Source.find_first_of("( )\\\t\v\f\n", Pos + 1);
				Open != std::string::npos && Source[Open] == '(' && Open - Pos < 18
			) if(const size_t End = Source.find(")" + Source.substr(Pos + 1, Open - Pos - 1) + "\"", Open); End != std::string::npos) {
				// 生文字列全体の逐語複写
				const size_t Stop = End + Open - Pos + 1;
				Out.append(Source, Pos, Stop - Pos);
				Pos = Stop;
				if(IsInDirective) MaskLiteral(TokenStart);
			}
			// 指令名から次の山括弧を見出名として読むかと，引数を解析しない指令の名前の後の生の本文の始まりの決定
			if(IsInDirective && DirectiveTokens == 1) {
				IsHeaderName = std::ranges::find(Rules.HeaderNameDirectives, Ident) != Rules.HeaderNameDirectives.end();
				IsDefine = Ident == Rules.DefinitionDirective;
				if(!IsDefine && std::ranges::find(Rules.ParsedDirectives, Ident) == Rules.ParsedDirectives.end()) RawFrom = Out.size();
			} else if(IsInDirective && IsDefine && DirectiveTokens == 2) {
				// マクロ名の直後の `(` は仮引数（閉じの後が生の本体），其れ以外はマクロ名の後が生の本体
				IsInParams = Pos < Size && Source[Pos] == '(';
				if(!IsInParams) RawFrom = Out.size();
			}
		} else if(
			std::isdigit(static_cast<unsigned char>(Char)) ||
			Char == '.' && Second < Size && std::isdigit(static_cast<unsigned char>(Source[Second]))
		) {
			// 数値（前処理数）：桁区切りの `'` と指数の符号の包含
			Out += Char;
			++Pos;
			// 前処理数の構成字走査
			for(SkipSplices(); Pos < Size; SkipSplices()) {
				const char Digit = Source[Pos];
				if(Digit == '\'') {
					// 桁区切り後の論理字確認
					const size_t After = NextLogical(Pos + 1);
					if(After == Size || !IsWordChar(Source[After])) break;
					Out += Digit;
					IsChanged = IsChanged || After != Pos + 1;
					Pos = After;
					continue;
				}
				if(!IsWordChar(Digit) && Digit != '.') break;
				Out += Digit;
				++Pos;
				if(Digit != 'e' && Digit != 'E' && Digit != 'p' && Digit != 'P') continue;
				// 指数部の符号収集
				SkipSplices();
				if(Pos < Size && (Source[Pos] == '+' || Source[Pos] == '-')) Out += Source[Pos++];
			}
		} else {
			// 単一記号字句の複写
			Out += Char;
			++Pos;
			// マクロの仮引数の括弧の対の追跡（閉じの後が生の本体）
			if(IsInParams && Char == '(') ++ParamDepth;
			else if(IsInParams && Char == ')' && !--ParamDepth) {
				IsInParams = false;
				RawFrom = Out.size();
			}
		}
		if(!IsInDirective) continue;
		// `#` 後の指令名と次字句迄の見出名候補
		if(DirectiveTokens++ > 1) IsHeaderName = false;
		LastToken = Out.size();
	}
	// 末尾の改行が無い指令も確定
	if(IsInDirective) EndDirective();
	// 連結指定保護の適用
	const std::optional<bool> IsGuarded = MaskLinkageGuards(Rules, Source, Out, Marker, Escaped);
	// 印を選べず誤読を補修出来ない事の返戻
	if(!IsGuarded || IsUnmarkable) return std::nullopt;
	// 整理後の原文（変更が無ければ空）の返戻
	return *IsGuarded || IsChanged ? Out : std::string();
}

/**
 * 印へ置き換えた `extern "C"` の波括弧の復元関数
 * @param Rules 入力の読取規則
 * @param Text 整形後の原文
 * @param Marker 置換に使った印
 */
void ParseAdapter::RestoreLinkageGuards(const ReadingRules &Rules, std::string &Text, const char Marker) {
	// 置換印の検索字句作成
	const std::string Placeholder = std::string(Rules.LinkageMarkerDirective) + Marker;
	// 各置換印の連結指定波括弧への復元
	for(size_t Found = Text.find(Placeholder); Found != std::string::npos; Found = Text.find(Placeholder, Found + 1)) {
		if(const size_t Kind = Found + Placeholder.size(); Kind < Text.size() && (Text[Kind] == 'O' || Text[Kind] == 'C')) {
			Text.replace(Found, Placeholder.size() + 1, Text[Kind] == 'O' ? Rules.LinkageOpen : Rules.LinkageClose);
		}
	}
	// 終了
	return;
}

/**
 * CSS の文字列の逃避した逆斜線の誤読の回避関数
 * tree-sitter-scss の文字列の型は逆斜線自身も通常の字として受ける為，最長一致で `"\\"` の `\\"` を逃避した引用符と読み，文字列を次の引用符迄延ばす（構文誤りも立たない）
 * 文字列の中の逃避した逆斜線の２字目を原文に無い制御文字へ置き換えて読ませ，整形後に逆斜線へ戻す
 * @param Source 原文（改行は LF へ正規化済）
 * @param HasSlashLineComment `//` を行コメントとして読むか
 * @param Marker 置き換えた制御文字の格納先（置き換えなければ '\0'）
 * @return 置き換えた原文（逃避した逆斜線が無ければ空，原文が候補の制御文字を全て含み印を選べない場合は無し）
 */
std::optional<std::string> ParseAdapter::MaskCssEscapedBackslashes(
	const std::string &Source,
	const bool HasSlashLineComment,
	char &Marker
) {
	Marker = '\0';
	const std::string_view BlockOpen = Core::CommentRepository::OpenerOf(CommentNotation::Block);
	const std::string_view BlockClose = Core::CommentRepository::CloserOf(CommentNotation::Block);
	const std::string_view LineOpen = Core::CommentRepository::OpenerOf(CommentNotation::SlashLine);
	// 逃避した逆斜線の無い原文の返戻
	if(Source.find("\\\\") == std::string::npos) return std::string();
	std::string Out;
	const size_t Size = Source.size();
	for(size_t Pos = 0; Pos < Size;) {
		const char Char = Source[Pos];
		// コメントの読飛（コメントの中の引用符で文字列を始めない）
		const bool IsBlock = !Source.compare(Pos, BlockOpen.size(), BlockOpen);
		if(const bool IsLine = HasSlashLineComment && !Source.compare(Pos, LineOpen.size(), LineOpen); IsBlock || IsLine) {
			const size_t End = IsBlock ? Source.find(BlockClose, Pos + BlockOpen.size()) : Source.find('\n', Pos + LineOpen.size());
			Pos = End == std::string::npos ? Size : IsBlock ? End + BlockClose.size() : End;
			continue;
		}
		// 引用符の無い `url(…)` の中身の読飛（`url(http://…)` の `//` をコメントと読まない）
		if(
			(Char | 0X20) == 'u' && Pos + 3 < Size && (Source[Pos + 1] | 0X20) == 'r' && (Source[Pos + 2] | 0X20) == 'l' &&
			Source[Pos + 3] == '(' && !(Pos && (IsIdentifierChar(Source[Pos - 1]) || Source[Pos - 1] == '-'))
		) if(
			const size_t Content = Source.find_first_not_of(" \t\n", Pos + 4);
			Content != std::string::npos && Source[Content] != '"' && Source[Content] != '\''
		) {
			const size_t Close = Source.find(')', Content);
			Pos = Close == std::string::npos ? Size : Close + 1;
			continue;
		}
		// 文字列以外の字の読み進め
		if(Char != '"' && Char != '\'') {
			++Pos;
			continue;
		}
		// 文字列の中の逃避の走査
		for(++Pos; Pos < Size && Source[Pos] != Char && Source[Pos] != '\n'; ++Pos) {
			// 逃避でない字の読み進め
			if(Source[Pos] != '\\' || Pos + 1 >= Size) continue;
			if(Source[Pos + 1] == '\\') {
				// 印を選べず誤読を補修出来ない事の返戻
				if(!Marker && !(Marker = TextEdit::AbsentControlChar(Source))) return std::nullopt;
				// 置き換える原文の複写
				if(Out.empty()) Out = Source;
				Out[Pos + 1] = Marker;
			}
			// 逃避の２字目の読み飛ばし
			++Pos;
		}
		// 閉じ引用符の読み飛ばし
		++Pos;
	}
	// 置き換えた原文の返戻
	return Out;
}

/**
 * 改行を値其の物として保つ範囲の収集関数
 * リテラル・PHP の地の文・HTML の逐語内容の範囲を原文順に集める
 * 保護範囲の確定に言語の走査が要る為，入力表現の正規化の為の試験解析を此処で行う（整形仕様 4.1, 4.2）
 * @param Profile 対象の文法
 * @param Source 原文
 * @param ShouldIncludeBlockComments 囲みコメントも保つ範囲に含めるか
 * @param Reason 解析の期限切れで範囲を確定出来なかった理由の格納先
 * @param Trial 同じ字面の解析結果が在れば再利用する格納先
 * @return 範囲（開始・終了のバイト位置）の並び（期限切れは無し）
 */
std::optional<std::vector<std::pair<uint32_t, uint32_t>>> ParseAdapter::LiteralRanges(
	const GrammarProfile &Profile,
	const std::string &Source,
	const bool ShouldIncludeBlockComments,
	FailureReason &Reason,
	std::optional<ParsedTree> &Trial
) {
	// 逐語保持する範囲列
	std::vector<std::pair<uint32_t, uint32_t>> Literals;
	// 構文木を持たない言語は保つ範囲も持たない事の返戻
	if(!Profile.GetLanguage().TsLang()) return Literals;
	// 範囲収集用の構文木
	if(!Trial || std::string_view(*Trial) != Source || &Trial->GetProfile() != &Profile) Trial.emplace(Profile, Source);
	const ParsedTree &Raw = *Trial;
	// 解析器の初期化失敗は後の解析が失敗として扱う
	if(Raw.HasExpiredParse()) {
		Reason = FailureReason::ParseTimeExceeded;
		// 期限切れで範囲を確定出来ない事の返戻
		return std::nullopt;
	}
	if(Raw.IsParsed()) {
		const ReadingRules &Reading = Profile.GetReading();
		WalkChildrenCursor(
			Raw.GetRoot(),
			[&](const TSNode Node) -> bool {
				const bool IsComment = Raw.Is(Node, Core::Role::Comment);
				// 文書に属するコメントと文書型宣言の字面は通常コメントと異なり逐語内容に為る
				if(
					Reading.HasCommentDocumentContent &&
					(Raw.Is(Node, Core::Role::CommentNode) || Raw.Is(Node, Core::Role::DocumentTypeDeclaration))
				) {
					Literals.emplace_back(Raw.Start(Node), Raw.End(Node));
					// 逐語内容の内側へ降りない事の返戻
					return false;
				}
				// 保持要素の開始・終了タグの間を一つの区画として保護し，子孫の空白も同じ契約で扱う
				if(Reading.HasCommentDocumentContent && Raw.Is(Node, Core::Role::MarkupElementNode)) {
					const TSNode Tag = Raw.FirstNamedChild(Node, Core::Role::StartTag);
					if(!ts_node_is_null(Tag)) {
						const TSNode Name = Raw.FirstNamedChild(Tag, Core::Role::TagNameNode);
						std::string Lower = ts_node_is_null(Name) ? std::string() : std::string(Raw.View(Name));
						ToLowerAscii(Lower);
						bool Preserves = GrammarProfile::MarkupTraits(Lower) & GrammarProfile::Preserve;
						ForEachNamedChild(
							Tag,
							[&](const TSNode Attribute) -> bool {
								// 名前と値を持つ属性でない子（タグ名等）を飛ばす事の返戻
								if(Raw.Is(Attribute, Core::Role::TagNameNode) || ts_node_named_child_count(Attribute) < 2) return true;
								std::string_view Value = Raw.View(ts_node_named_child(Attribute, 1));
								if(Value.size() > 1 && (Value.front() == '\'' || Value.front() == '"')) Value = Value.substr(1, Value.size() - 2);
								Preserves |= Profile.PreservesMarkupWhitespace(Raw.View(ts_node_named_child(Attribute, 0)), Value);
								// 次の属性へ進む事の返戻
								return true;
							}
						);
						if(Preserves) {
							const TSNode Close = LastNamedChild(Node);
							const uint32_t End = !ts_node_is_null(Close) && Raw.Is(Close, Core::Role::MarkupClose) ? Raw.Start(Close) : Raw.End(Node);
							if(Raw.End(Tag) < End) Literals.emplace_back(Raw.End(Tag), End);
							// 保持要素の内側へ降りない事の返戻
							return false;
						}
					}
				}
				// PHP の地の文も出力其の物の為，リテラルと同じく改行を保つ
				// リテラル以外の内側へ降りる事の返戻
				if(
					const bool IsBlock = ShouldIncludeBlockComments && IsComment &&
					Raw.View(Node).starts_with(Core::CommentRepository::OpenerOf(CommentNotation::Block));
					!IsBlock && (
						!Raw.Is(Node, Core::Role::InlineText) && !Raw.Is(Node, Core::Role::LiteralInterior) && !Raw.Is(Node, Core::Role::SourceLeaf) &&
						!Raw.Is(Node, Core::Role::RenderedText) || IsComment
					)
				) return true;
				// 逐語範囲の記録
				Literals.emplace_back(Raw.Start(Node), Raw.End(Node));
				// リテラルの内側へ降りない事の返戻
				return false;
			}
		);
	}
	// 保つ範囲の並びの返戻
	return Literals;
}

/**
 * 字面の段の解析用の入力の準備関数（整形仕様 4.2，解析の前に原文の字面で行う可逆な保護）
 * C / C++ は行継続と前処理指令を整え，CSS は文字列の逃避した逆斜線を印へ置き換える（其の他の言語は置き換えない）
 * @param Profile 対象の文法
 * @param Source 原文（改行は LF へ正規化済）
 * @param Marker 置き換えた制御文字の格納先（置き換えなければ '\0'）
 * @param HasStrayBackslash 後の字句が次の行へ移ると行末に立ち，原文に無い行継続に為る裸の逆斜線を含むかの格納先 (C / C++)
 * @param Escaped 解析器へ渡さずに退避したコメントの格納先 (C / C++)
 * @return 準備した原文（変更が無ければ空，印が要るのに原文が候補の制御文字を全て含み選べない場合は無し）
 */
std::optional<std::string> ParseAdapter::PrepareSource(
	const GrammarProfile &Profile,
	const std::string &Source,
	char &Marker,
	bool &HasStrayBackslash,
	std::vector<EscapedComment> &Escaped
) {
	Marker = '\0';
	HasStrayBackslash = false;
	const Core::SyntaxRules &Rules = Profile.GetRules();
	const ReadingRules &Reading = Profile.GetReading();
	// C / C++ の準備した原文の返戻
	if(Rules.HasLineDirectives) return PrepareCSource(Reading, Source, Marker, HasStrayBackslash, Escaped);
	// CSS の準備した原文か，置き換えない原文の返戻
	return Reading.MasksEscapedBackslashes ?
	MaskCssEscapedBackslashes(Source, Rules.Notation.IsAvailable(CommentNotation::SlashLine), Marker) :
	std::optional<std::string>(std::string());
}

/**
 * 解析の為に原文へ置いた印と退避したコメントの記録関数（印は投影が本来の字句へ復号し，退避したコメントは投影がコメントの記録にする，整形仕様 4.2）
 * @param Src 印を置いた原文の解析結果
 * @param Marker 置いた印（置いて居なければ '\0'）
 * @param Escaped 解析用の入力から退避したコメント（位置は印を置いた原文の位置）
 */
void ParseAdapter::RecordMark(ParsedTree &Src, const char Marker, std::vector<EscapedComment> &&Escaped) {
	Src.AnalysisMark = Marker;
	Src.EscapedComments.insert(
		Src.EscapedComments.end(),
		std::make_move_iterator(Escaped.begin()),
		std::make_move_iterator(Escaped.end())
	);
	// 終了
	return;
}

/**
 * 解析用の印の本来の字への復号関数（整形仕様 4.2，印は位置に依らず字毎に戻せる）
 * C / C++ の連結指定の波括弧と前処理リテラルの `*`，CSS の逃避した逆斜線を戻す（Ruby の埋込文書の印はコメントの分離で戻す）
 * @param Src 印を置いた原文の解析結果
 * @param Text 印を含み得る字面（コメントを除いて繋いだ物でも良い）
 * @return 印を本来の字へ戻した字面
 */
std::string ParseAdapter::DecodeMarks(const ParsedTree &Src, std::string Text) {
	const char Marker = Src.AnalysisMark;
	// 印を含まない字面の返戻
	if(!Marker || Text.find(Marker) == std::string::npos) return Text;
	if(Src.GetReading().MasksEscapedBackslashes) std::replace(Text.begin(), Text.end(), Marker, '\\');
	else if(Src.GetRules().HasLineDirectives) {
		// 連結指定の波括弧と前処理リテラルの `*` の復元
		RestoreLinkageGuards(Src.GetReading(), Text, Marker);
		std::replace(Text.begin(), Text.end(), Marker, '*');
	}
	// 復号した字面の返戻
	return Text;
}

/**
 * 解析用の印の本来の字句への復号関数（整形仕様 3.3, 4.2，中核の字句・幅の計数・最終出力へ印を残さない）
 * 原文の連続した断片の中に全体が在る置換した字句を原文の字面へ戻し，印を戻す
 * コメントを除いて不連続に為った字面は，断片毎に此の関数で復号してから繋ぐ（断片を跨いで位置を推定しない）
 * 計算量：断片の長さ N と中の置換の数 R，復号資料の数 M に対し O(N + R + log M)
 * @param Src 印を置いた原文の解析結果
 * @param Text 解析用の本文の連続した断片の字面
 * @param Start 解析用の本文に於ける断片の始まり
 * @return 本来の字句の字面
 */
std::string ParseAdapter::DecodeLexeme(const ParsedTree &Src, std::string Text, const uint32_t Start) {
	const std::vector<LexemeRestoration> &Restorations = Src.GetLexemeRestorations();
	const uint32_t End = Start + static_cast<uint32_t>(Text.size());
	const std::vector<LexemeRestoration>::const_iterator First =
	std::ranges::lower_bound(Restorations, Start, std::less<uint32_t>(), &LexemeRestoration::Start);
	// 断片の中に全体が在る置換の後ろからの復号（前の置換の位置を保つ）
	for(
		std::vector<LexemeRestoration>::const_iterator Last =
		std::ranges::lower_bound(First, Restorations.end(), End, std::less<uint32_t>(), &LexemeRestoration::Start);
		Last != First;
	) {
		--Last;
		if(Last->End <= End) Text.replace(Last->Start - Start, Last->End - Last->Start, Last->Original);
	}
	// 印を戻した字句の返戻
	return DecodeMarks(Src, std::move(Text));
}

/**
 * JSX の本文の端の空白・タブの印の復号関数（整形仕様 4.2，本文の行端を確定した後の字面へ戻す）
 * 計算量：本文の長さ N に対し O(N)
 * @param Src 印を置いた原文の解析結果
 * @param Text 本文の字面
 * @return 印を半角空白・タブへ戻した字面
 */
std::string ParseAdapter::DecodeJsxBlanks(const ParsedTree &Src, std::string Text) {
	// 印（私用領域の文字を選べなかった時は文字参照）
	const std::string_view SpaceMark = Src.GetJsxBlankMark(' '), TabMark = Src.GetJsxBlankMark('\t');
	// 印を半角空白とタブへ戻す出力の構築
	std::string Decoded;
	Decoded.reserve(Text.size());
	size_t Start = 0;
	while(Start < Text.size()) {
		const size_t Space = Text.find(SpaceMark, Start), Tab = Text.find(TabMark, Start), Offset = std::min(Space, Tab);
		if(Offset == std::string::npos) break;
		const bool IsSpace = Space <= Tab;
		Decoded.append(Text, Start, Offset - Start);
		Decoded.push_back(IsSpace ? ' ' : '\t');
		Start = Offset + (IsSpace ? SpaceMark.size() : TabMark.size());
	}
	Decoded.append(Text, Start, Text.size() - Start);
	// 復号した字面の返戻
	return Decoded;
}

/**
 * マクロの本体の字句の範囲の取得関数（整形仕様 4.2，字句間空白仕様 5.4）
 * 文字・文字列のリテラルとコメントの中の空白で分けず，其の外の空白の並びで分けた字句の範囲を返す（空白の量は読まない）
 * 計算量：本体の長さ N に対し O(N)
 * @param Body マクロの本体の字面（連結後の１論理行）
 * @param IsHeaderName 見出名を引数に取る指令の引数か（`<` から `>` 迄も分けない）
 * @return 字句の始まりと終わりの並び（原文順）
 */
std::vector<std::pair<uint32_t, uint32_t>> ParseAdapter::MacroBodyTokens(const std::string_view Body, const bool IsHeaderName) {
	// 字句の区切りとコメント記法の準備
	std::vector<std::pair<uint32_t, uint32_t>> Tokens;
	const std::string_view BlockOpen = Core::CommentRepository::OpenerOf(CommentNotation::Block);
	const std::string_view BlockClose = Core::CommentRepository::CloserOf(CommentNotation::Block);
	const std::string_view LineOpen = Core::CommentRepository::OpenerOf(CommentNotation::SlashLine);
	const auto IsBlank = [](const char Char) -> bool {
		// 字句を区切る空白かの返戻
		return Char == ' ' || Char == '\t' || Char == '\r' || Char == '\n' || Char == '\f' || Char == '\v';
	};
	// 空白の外に在る字句の範囲の収集
	for(size_t Pos = 0; Pos < Body.size();) {
		if(IsBlank(Body[Pos])) {
			++Pos;
			continue;
		}
		const size_t Start = Pos;
		while(Pos < Body.size() && !IsBlank(Body[Pos])) {
			if(const char Char = Body[Pos]; Char == '"' || Char == '\'' || IsHeaderName && Char == '<') {
				// リテラル・見出名の閉じ迄（リテラルの逃避した字を飛ばす）
				const char Close = Char == '<' ? '>' : Char;
				for(++Pos; Pos < Body.size() && Body[Pos] != Close; ++Pos) if(Body[Pos] == '\\' && Close != '>') ++Pos;
				Pos = std::min(Pos + 1, Body.size());
			} else if(Body.substr(Pos).starts_with(BlockOpen)) {
				Pos = std::min(Body.find(BlockClose, Pos + BlockOpen.size()), Body.size() - BlockClose.size()) + BlockClose.size();
			} else if(Body.substr(Pos).starts_with(LineOpen)) Pos = Body.size();
			else ++Pos;
		}
		Tokens.emplace_back(static_cast<uint32_t>(Start), static_cast<uint32_t>(Pos));
	}
	// 字句の範囲の返戻
	return Tokens;
}

/**
 * 印へ置き換えた `extern "C"` の波括弧の指令かの判定関数
 * @param Src 印を置いた原文の解析結果
 * @param Node 前処理指令の節点
 * @return 解析の為に置いた `#pragma <印>O` / `#pragma <印>C` なら true（字句へ分けず一つの葉として復号する）
 */
bool ParseAdapter::IsLinkageGuard(const ParsedTree &Src, const TSNode Node) {
	const std::string_view Text = Src.View(Node);
	const char Marker = Src.GetAnalysisMark();
	const std::string_view Head = Src.GetReading().LinkageMarkerDirective;
	// 印の指令かの返戻
	return Marker && !Head.empty() && Text.starts_with(Head) && Text.size() > Head.size() + 1 && Text[Head.size()] == Marker &&
	(Text[Head.size() + 1] == 'O' || Text[Head.size() + 1] == 'C');
}

/**
 * 埋込の中身が要素の終わりの直前迄を占めるかの判定関数（HTML 仕様の script / style の本文の字句化の状態を辿る）
 * script の本文は `<!--` で逃避状態に入り，其の中の `<script` で二重の逃避状態に入り，二重の逃避の中の `</script` は要素を閉じない
 * 計算量：中身の長さ N に対し O(N)
 * @param Rules 入力の読取規則
 * @param Content 埋込の中身
 * @param IsScript script の中身か（偽は style の中身で，逃避状態を持たない）
 * @return 中身の途中で要素が閉じず，中身の後の終了タグで要素が閉じるなら true
 */
bool ParseAdapter::KeepsRawTextEnd(const ReadingRules &Rules, const std::string_view Content, const bool IsScript) {
	const std::string_view Name = IsScript ? Rules.RawTextEscapeElement : Rules.RawTextPlainElement;
	// 位置からタグ名が始まり，其の後に名前の区切り（空白・`/`・`>`）が続くか
	const auto IsTagAt = [Content, Name, &Rules](const size_t At) -> bool {
		// 中身の終わりは後に続く終了タグの `<` で区切りでない事を含めた一致の返戻
		return Content.size() - At > Name.size() && IsHtmlNameEqual(Content.substr(At, Name.size()), Name) &&
		Rules.RawTextNameTerminators.find(Content[At + Name.size()]) != std::string_view::npos;
	};
	enum class ScriptState : uint8_t { Data, DoubleEscaped, Escaped }; // script 本文を読む状態（初期の Data を先頭に保持）
	ScriptState State = ScriptState::Data;
	size_t Dashes = 0;
	for(size_t Pos = 0; Pos < Content.size(); ++Pos) {
		const char Char = Content[Pos];
		const bool IsClosingTag = Content.substr(Pos).starts_with(Rules.RawTextEndOpen) && IsTagAt(Pos + Rules.RawTextEndOpen.size());
		// 中身の途中で要素を閉じる終了タグの返戻
		if(IsClosingTag && State != ScriptState::DoubleEscaped) return false;
		if(!IsScript) continue;
		if(State == ScriptState::Data) {
			// 逃避状態の開始
			if(!Rules.RawTextEscapeOpen.empty() && Content.substr(Pos).starts_with(Rules.RawTextEscapeOpen)) {
				State = ScriptState::Escaped;
				Dashes = 2;
				Pos += Rules.RawTextEscapeOpen.size() - 1;
			}
			continue;
		}
		if(Char == '-') {
			++Dashes;
			continue;
		}
		// `-->` での逃避状態の終了
		if(Char == '>' && Dashes > 1) State = ScriptState::Data;
		// 二重の逃避状態の開始と終了
		else if(Char == '<' && State == ScriptState::Escaped && IsTagAt(Pos + 1)) State = ScriptState::DoubleEscaped;
		else if(IsClosingTag) State = ScriptState::Escaped;
		Dashes = 0;
	}
	// 後に続く終了タグで要素が閉じるかの返戻
	return State != ScriptState::DoubleEscaped;
}

/**
 * 文書順の埋込の中身の列挙関数（整形と配置が同じ順で埋込を数える，整形仕様 4.4）
 * 中身の言語を確定出来る script / style を，中身の無い物も含めて集める
 * 計算量：節点数 N に対し O(N)
 * @param Src 解析済の HTML
 * @return 文書順の埋込の中身
 */
std::vector<ParseAdapter::EmbeddedContent> ParseAdapter::EmbeddedContents(const ParsedTree &Src) {
	std::vector<EmbeddedContent> Contents;
	WalkChildrenCursor(
		Src.GetRoot(),
		[&](const TSNode Node) -> bool {
			if(Src.Is(Node, Core::Role::CommentNode) || Src.Is(Node, Core::Role::ContentElement) && IsRawContentElement(Src, Node)) {
				// 文書の内容である HTML のコメントと，内容を文字として読む要素の内側へ降りない事の返戻
				return false;
			}
			// script / style 以外の子孫を走査し続ける事の返戻
			if(!Src.Is(Node, Core::Role::EmbeddedCodeElement)) return true;
			if(const Lang::ID Embedded = HtmlEmbeddedLanguage(Src, Node); Embedded != Lang::Unknown) {
				Contents.push_back({ Node, Src.FirstNamedChild(Node, Core::Role::RawTextNode), Embedded });
			}
			// script / style の内側へ降りない事の返戻
			return false;
		}
	);
	// 埋込の中身の返戻
	return Contents;
}

/**
 * 独立して整形した埋込の出力の所有区画としての控え関数（整形仕様 4.4，親の原文へ書き戻さず投影が所有区画とする）
 * 整形した中身は開始・終了タグとは別の行へ置く為，末尾の改行を除いて控える
 * 要素の終わりを動かす出力（`<!--` を行コメントへ揃えると二重の逃避状態が解ける等）は控えない
 * @param Src 解析済の HTML
 * @param Embed 埋込の中身
 * @param Output 中身を単独のコードとして整形し検証した出力
 * @param CodeText コメントを除いた子のコード行
 * @param CodeVerbatimLines 子のコード行の逐語の中から始まる行の番号
 * @param VerbatimLines 出力の逐語の中から始まる行の番号
 * @return 控えた所有区画（要素の終わりを動かす出力は nullptr）
 */
std::shared_ptr<const Core::OwnedRegion> ParseAdapter::OwnEmbeddedOutput(
	ParsedTree &Src,
	const EmbeddedContent &Embed,
	std::string Output,
	std::string CodeText,
	std::vector<uint32_t> &&CodeVerbatimLines,
	std::vector<uint32_t> &&VerbatimLines
) {
	while(!Output.empty() && Output.back() == '\n') Output.pop_back();
	while(!CodeText.empty() && CodeText.back() == '\n') CodeText.pop_back();
	const bool IsScript = Src.Is(Embed.Element, Core::Role::EmbeddedScriptElement);
	// 開始・終了タグとは別の行へ置いた中身の字面
	const std::string Placed = Output.empty() ? std::string() : '\n' + Output + '\n';
	const ReadingRules &Reading = Src.GetReading();
	// 要素の終わりを動かす出力の返戻
	if(!KeepsRawTextEnd(Reading, Placed, IsScript) || !KeepsRawTextEnd(Reading, Src.View(Embed.RawText), IsScript)) return nullptr;
	Src.SetOwnedRegion(
		Embed.RawText,
		{ std::move(CodeText), std::move(CodeVerbatimLines), std::move(Output), std::move(VerbatimLines) }
	);
	// 控えた所有区画の返戻
	return Src.OwnedRegionOf(Embed.RawText);
}

/**
 * PHP の実行を止める `__halt_compiler()` の文かの判定関数
 * @param Src ソースコード
 * @param Stmt 判定対象の文
 * @return `__halt_compiler()` の呼出の式文なら true
 */
bool ParseAdapter::IsHaltCall(const ParsedTree &Src, const TSNode Stmt) {
	const ReadingRules &Reading = Src.GetReading();
	// 実行停止後のデータを持たない記法の返戻
	if(!Reading.HasHaltedData) return false;
	// 式文の先頭呼出候補
	const TSNode Call = ts_node_named_child(Stmt, 0);
	// 関数呼出式文でない事の返戻
	if(
		!Src.Is(Stmt, Core::Role::ExpressionStatement) || ts_node_is_null(Call) || !Src.Is(Call, Core::Role::FunctionCallExpression)
	) return false;
	const std::string_view Callee = Src.View(Src.FieldChild(Call, Core::ChildRole::FunctionField));
	const std::string_view HaltName = Reading.HaltCallName;
	// 名前の一致の返戻
	return std::equal(
		Callee.begin(),
		Callee.end(),
		HaltName.begin(),
		HaltName.end(),
		[](const char Left, const char Right) -> bool {
			// 小文字へ揃えた一致の返戻
			return std::tolower(static_cast<unsigned char>(Left)) == Right;
		}
	);
}

/**
 * 開いた要素の名前を解析器が失い閉じタグを読み違えたかの判定関数（整形仕様 9.3 の tree-sitter-html の制限）
 * tree-sitter-html の走査器は開いた要素の並びを１０２４バイトの直列化領域へ書き，収まらない要素から後の名前を失う
 * 並びは要素数の欄４バイトの後に，既知の名前の要素を種別の１バイト，其の他の要素を長さと名前（２５５字迄）で持つ
 * 名前を失った要素を閉じる閉じタグは対応を失い，誤った閉じタグ又は誤り区画として解析結果に残る
 * 計算量：節点数 V に対し O(V)
 * @param Src 解析済の原稿 (HTML)
 * @return 名前を失う深さの要素と，誤った閉じタグ又は誤り区画の双方を解析結果が含めば true
 */
bool ParseAdapter::LosesHtmlElementNames(const ParsedTree &Src) {
	// 走査器の直列化領域の寸法と要素数の欄の寸法
	static constexpr uint32_t BufferSize = 0X400, Header = 4;
	bool IsLost = false, IsMisread = ts_node_has_error(Src.GetRoot());
	// 節点と，其の節点を囲む開いた要素の並びの直列化の大きさ
	std::vector<std::pair<TSNode, uint32_t>> Stack { { Src.GetRoot(), Header } };
	while(!Stack.empty() && !(IsLost && IsMisread)) {
		const auto [Node, Serialized] = Stack.back();
		Stack.pop_back();
		IsMisread = IsMisread || Src.Is(Node, Core::Role::ErroneousEndTag);
		// 要素の子を囲む並びの大きさ（要素の名前の分を加える）
		uint32_t Inner = Serialized;
		if(Src.Is(Node, Core::Role::MarkupElementNode)) {
			const TSNode Start = ts_node_named_child(Node, 0);
			const TSNode Name = ts_node_is_null(Start) ? Start : Src.FirstNamedChild(Start, Core::Role::TagNameNode);
			HtmlNameBuffer Buffer;
			const std::string_view Lower = ts_node_is_null(Name) ? std::string_view() : HtmlLowerName(Src.View(Name), Buffer);
			const uint32_t Bytes =
			Src.Is(Node, Core::Role::EmbeddedCodeElement) || GrammarProfile::MarkupTraits(Lower) & GrammarProfile::Scanned ?
			1 :
			std::min<uint32_t>(static_cast<uint32_t>(Lower.size()), 0XFF) + 2;
			IsLost = IsLost || Serialized + Bytes >= BufferSize;
			Inner = Serialized + Bytes;
		}
		// 要素の子を原文順に収集
		std::vector<TSNode> Children;
		HeldCursor Cursor(Node);
		if(ts_tree_cursor_goto_first_child(&Cursor)) {
			do if(const TSNode Child = ts_tree_cursor_current_node(&Cursor); ts_node_is_named(Child)) Children.push_back(Child);
			while(ts_tree_cursor_goto_next_sibling(&Cursor));
		}
		for(size_t Index = Children.size(); Index--;) Stack.emplace_back(Children[Index], Inner);
	}
	// 名前を失った要素が閉じタグを読み違えたかの返戻
	return IsLost && IsMisread;
}

/**
 * PHP の実行を止める `__halt_compiler()` の後に続くデータの開始位置の取得関数
 * データは `__COMPILER_HALT_OFFSET__` の位置から原文其の物として読まれる為，整形の対象から外す
 * @param Profile 対象の文法
 * @param Source 整形対象のソース（正規化前又は改行の正規化済）
 * @param Trial 同じ字面を解析済なら再利用する試験解析の結果
 * @return `__halt_compiler()` を終える `;` / `?>` の直後の位置（呼出が無ければ `std::string::npos`）
 */
size_t ParseAdapter::HaltedDataStart(
	const GrammarProfile &Profile,
	const std::string &Source,
	std::optional<ParsedTree> &Trial
) {
	const ReadingRules &Rules = Profile.GetReading();
	// 実行停止後のデータを持たない記法の返戻
	if(!Rules.HasHaltedData) return std::string::npos;
	const std::string_view HaltName = Rules.HaltCallName;
	// 関数名が字面に無い場合の構文解析を省く halt 名の不在の返戻
	if(
		std::search(
			Source.begin(),
			Source.end(),
			HaltName.begin(),
			HaltName.end(),
			[](const char Left, const char Right) -> bool {
				// 小文字へ揃えた一致の返戻
				return std::tolower(static_cast<unsigned char>(Left)) == Right;
			}
		) == Source.end()
	) return std::string::npos;
	Trial.emplace(Profile, Source);
	const ParsedTree &Raw = *Trial;
	// 構文木を得れない場合の返戻
	if(!Raw.IsParsed()) return std::string::npos;
	size_t DataStart = std::string::npos;
	// 呼出は最も外側の文に限られる為，根の直下だけを見る
	ForEachNamedChild(
		Raw.GetRoot(),
		[&](const TSNode Stmt) -> bool {
			// 他の文を飛ばす事の返戻
			if(!IsHaltCall(Raw, Stmt)) return true;
			// `;` で終えた文は其の直後，`?>` で終えた文は閉じタグの直後からデータ
			if(Raw.Is(ts_node_child(Stmt, ts_node_child_count(Stmt) - 1), Core::Role::StatementTerminatorToken)) DataStart = Raw.End(Stmt);
			else if(const TSNode Next = ts_node_next_named_sibling(Stmt); !ts_node_is_null(Next)) {
				if(const TSNode Close = Raw.FirstNamedChild(Next, Core::Role::PhpEndTag); !ts_node_is_null(Close)) DataStart = Raw.End(Close);
			}
			// 実行が止まる以降の文を見ない事の返戻
			return false;
		}
	);
	// データの開始位置の返戻
	return DataStart;
}

/**
 * PHP の本体の範囲の取得関数（整形仕様 4.4 の PHP の地の文）
 * PHP の地の文は出力其の物で，構文木の外に在る空白（最初の字句の前と，閉じタグの後に続く地の文）も出力に現れる
 * 最後の閉じタグの後に開始タグが戻らない範囲は，解析器が `//` 等をコメントと読んでも地の文とする
 * @param Src 解析済の原稿
 * @return 本体の始まりと終わり（其の外の字面は投影が逐語の地の文として保つ，PHP 以外と外に地の文の無い原稿は原稿全体）
 */
std::pair<uint32_t, uint32_t> ParseAdapter::OuterTextBounds(const ParsedTree &Src) {
	const uint32_t Size = static_cast<uint32_t>(Src.HaltedDataStart() == std::string::npos ? Src.size() : Src.HaltedDataStart());
	// 地の文を持たない言語と解析して居ない原稿の返戻
	if(!Src.GetReading().HasOuterText || !Src.IsParsed()) return { 0, Size };
	// 構文木範囲の取得
	const TSNode Root = Src.GetRoot();
	const uint32_t RootCount = ts_node_named_child_count(Root);
	// 地の文で終わる本体は末尾空白を覆う根でなく最後の字句で閉鎖（`?>` 直後の空白も地の文，開始タグ無は全体が地の文）
	bool IsTextTail = true;
	uint32_t BodyEnd = Src.End(Root);
	if(RootCount) {
		// 最終名前付子からの後続断片の切離
		const TSNode Last = ts_node_named_child(Root, RootCount - 1);
		IsTextTail = Src.Is(Last, Core::Role::InlineText) && ts_node_is_null(Src.FirstNamedChild(Last, Core::Role::CodeOpeningTag));
		BodyEnd = Src.End(Last);
		// `?>` の後に `<?php` が戻らない範囲は地の文だが，`//` で始まると解析器が之をコメントと読む
		// 其の儘だと地の文が通常コメントの規則で消える為，終了タグの区画の後を地の文とする
		for(uint32_t Idx = RootCount - 1; !IsTextTail && Idx;) {
			// 終了タグを含む区画の候補
			const TSNode Child = ts_node_named_child(Root, --Idx);
			// コメント以外の節点に達したら地の文で無い事の打切
			if(!Src.Is(Child, Core::Role::Comment) && !Src.Is(Child, Core::Role::InlineText)) break;
			if(
				!ts_node_is_null(Src.FirstNamedChild(Child, Core::Role::PhpEndTag)) &&
				ts_node_is_null(Src.FirstNamedChild(Child, Core::Role::CodeOpeningTag))
			) {
				IsTextTail = true;
				BodyEnd = Src.End(Child);
				break;
			}
		}
	}
	const uint32_t BodyStart = std::min(Src.Start(Root), Size), End = IsTextTail ? std::max(BodyStart, BodyEnd) : Size;
	// 閉じタグの直後の改行１個を処理系が読み捨てる為，其れを超える字面が在る時だけ地の文とした範囲の返戻
	return { BodyStart, End + 1 < Size ? End : Size };
}

/**
 * 古典スクリプトとして読む原稿かの判定関数（整形仕様 2.1, 10.4）
 * 拡張子で目標が定まらない原稿は，最上位に取込・公開の文を持たなければ古典スクリプトとする
 * @param Src 解析結果
 * @return 古典スクリプトが HTML 風コメントを認める文法で，古典スクリプトとして読む原稿なら true
 */
bool ParseAdapter::IsClassicScript(const ParsedTree &Src) {
	const Lang::SourceGoal Goal = Src.GetProfile().GetLanguage().Goal;
	if(
		const bool AcceptsMarkupComments = Src.GetReading().ScriptAcceptsMarkupComments;
		!AcceptsMarkupComments || Goal != Lang::SourceGoal::Undetermined
	// 古典スクリプトで HTML 風コメントを認めない文法と，拡張子で目標が定まる原稿の返戻
	) return AcceptsMarkupComments && Goal == Lang::SourceGoal::Script;
	// 最上位に取込・公開の文を持たない原稿かの返戻
	return !HasChildOf(
		Src.GetRoot(),
		[&Src](const TSNode Child) -> bool {
			// 取込・公開の文かの返戻
			return Src.Is(Child, Core::Role::ModuleStatement);
		}
	);
}

/**
 * 解析器が演算子と読んだ HTML 風コメントの有無の判定関数（古典スクリプトの言語仕様の付属書 B の読み）
 * 行中の開きと行頭の空白・囲みコメントの後の閉じを，解析器が演算子の字句として読んだ位置で見付ける
 * 計算量：原稿の長さ N と節点数 V に対し O(N + V)（位置表の引きを O(1) と看做す）
 * @param Src 解析結果
 * @return 演算子と読んだ HTML 風コメントを持てば true
 */
bool ParseAdapter::HasOperatorMarkupComment(const ParsedTree &Src) {
	// コメント範囲と演算子と読まれた字句位置の収集
	const ReadingRules &Rules = Src.GetReading();
	const std::string_view Text = Src.CodeText();
	std::unordered_set<uint32_t> OpenTokens, CloseTokens;
	std::vector<std::pair<uint32_t, uint32_t>> Comments;
	WalkChildrenCursor(
		Src.GetRoot(),
		[&Src, &Text, &OpenTokens, &CloseTokens, &Comments](const TSNode Node) -> bool {
			// コメント範囲の収集
			if(Src.Is(Node, Core::Role::Comment)) {
				const auto [Start, End] = Src.CommentRangeOf(Node);
				if(Start < Text.size() && Text.substr(Start).starts_with(Core::CommentRepository::OpenerOf(CommentNotation::Block))) {
					Comments.push_back({ Start, End });
				}
				// コメントの内側を走査しない事の返戻
				return false;
			}
			// 子に覆われない演算子字句の記録
			if(Src.Is(Node, Core::Role::LessThanToken) || Src.Is(Node, Core::Role::ChangeByOneToken)) {
				const uint32_t Start = Src.Start(Node);
				bool HasCoveringChild = false;
				HeldCursor Cursor(Node);
				if(ts_tree_cursor_goto_first_child(&Cursor)) {
					do {
						const TSNode Child = ts_tree_cursor_current_node(&Cursor);
						HasCoveringChild = ts_node_start_byte(Child) <= Start && ts_node_end_byte(Child) > Start;
					} while(!HasCoveringChild && ts_tree_cursor_goto_next_sibling(&Cursor));
				}
				if(!HasCoveringChild) {
					if(Src.Is(Node, Core::Role::LessThanToken)) OpenTokens.insert(Start);
					if(Src.Is(Node, Core::Role::ChangeByOneToken)) CloseTokens.insert(Start);
				}
			}
			// 字句の子の走査を続ける事の返戻
			return true;
		}
	);
	// 比較の演算子と読んだ行中の開き
	bool IsOpenRead = false;
	for(
		size_t At = Text.find(Rules.MarkupCommentOpen);
		!IsOpenRead && At != std::string_view::npos;
		At = Text.find(Rules.MarkupCommentOpen, At + 1)
	) IsOpenRead = OpenTokens.contains(static_cast<uint32_t>(At));
	// 比較の演算子と読んだ行中の開きの返戻
	if(IsOpenRead) return true;
	const auto IsLineEnd = [](const uint32_t Point) -> bool {
		// 行を区切る符号点かの返戻
		return Point == '\n' || Point == '\r' || Point == 0X2028 || Point == 0X2029;
	};
	// 囲みコメントを越えた行頭の閉じ記号の探索
	size_t CommentIndex = 0;
	bool IsLineHead = true;
	for(size_t At = 0; At < Text.size();) {
		while(CommentIndex < Comments.size() && Comments[CommentIndex].second <= At) ++CommentIndex;
		if(CommentIndex < Comments.size() && Comments[CommentIndex].first <= At && Comments[CommentIndex].second > At) {
			const size_t End = std::min<size_t>(Comments[CommentIndex].second, Text.size());
			for(size_t Pos = At; Pos < End;) {
				size_t Next = Pos;
				uint32_t Point = 0;
				if(!TextEdit::DecodeUtf8(Text, Next, Point)) Next = Pos + 1;
				if(IsLineEnd(Point)) IsLineHead = true;
				Pos = Next;
			}
			At = End;
			++CommentIndex;
			continue;
		}
		// コメント外の符号点と行頭状態の更新
		size_t Next = At;
		uint32_t Point = 0;
		if(!TextEdit::DecodeUtf8(Text, Next, Point)) {
			Next = At + 1;
			Point = 0XFFFFFFFF;
		}
		if(IsLineEnd(Point)) IsLineHead = true;
		else if(!Rules.IsSpace(Point)) {
			if(IsLineHead && Text.substr(At).starts_with(Rules.MarkupCommentClose) && CloseTokens.contains(static_cast<uint32_t>(At))) {
				// 減算の演算子と読んだ行頭の閉じの返戻
				return true;
			}
			IsLineHead = false;
		}
		At = Next;
	}
	// 演算子と読んだ HTML 風コメントが無い事の返戻
	return false;
}

/**
 * 解析用の入力の読取の実装関数（整形仕様 3.4 の Adapt の前段，4.2）
 * 字面の段の可逆な保護・解析・共有ヘッダの文法の選び直し・字句化前の逃避の展開迄を行い，解析器の読違の補修の前の解析結果を返す
 * 最終検証は補修の前の字面で空行・位置を照合する為，此の段と `Repair` を分けて呼ぶ
 * @param Profile 入力で選んだ文法（中身で判別する共有拡張子の変種を含む）
 * @param Source 整形対象のソース（改行の正規化済）
 * @param DataStart 正規化した原稿の逐語データの開始位置（無ければ `std::string::npos`）
 * @param Trial 確定した字面と文法が一致する試験解析の結果
 * @return 読取の結果（`.h` の中身が C++ と判れば言語を書き換える）
 */
ParseAdapter::ParseInput ParseAdapter::ReadPrepared(
	const GrammarProfile &Profile,
	const std::string &Source,
	const size_t DataStart,
	std::optional<ParsedTree> *const Trial
) {
	// 中身で判別する共有拡張子は，判別を終えた文法で読み，以後の工程へ其の定義を引き継ぐ（整形仕様 4.2）
	const bool CanPromote = Profile.GetReading().HeaderPromotion != Lang::Unknown;
	Lang Settled = Profile.GetLanguage();
	Settled.IsAmbiguous = false;
	ParseInput Input { .Profile = CanPromote ? &GrammarProfile::Resolve(Settled) : &Profile };
	// 適応を止めた区分の記録
	const auto Stop = [&Input](const ParseDecision Decision) -> ParseInput {
		Input.Decision = Decision;
		// 止めた結果の返戻
		return std::move(Input);
	};
	// C / C++ の行継続の字句化前結合と前処理コメント・リテラル，CSS の逃避した逆斜線の保護（整形仕様 4.2）
	char Marker = '\0';
	// 後の字句が次の行へ移ると行末に立ち，原文に無い行継続に為る裸の逆斜線を含むか
	bool HasStrayBackslash = false;
	std::vector<EscapedComment> Escaped;
	const std::string Code = DataStart == std::string::npos ? std::string() : Source.substr(0, DataStart);
	std::optional<std::string> Prepared =
	ParseAdapter::PrepareSource(Profile, DataStart == std::string::npos ? Source : Code, Marker, HasStrayBackslash, Escaped);
	if(!Prepared) {
		Input.Failure = FailureReason::ParserUnreadable;
		// 印を選べず解析器の誤読を補修出来ない原稿の返戻（整形仕様 4.2）
		return Stop(ParseDecision::Skip);
	}
	if(HasStrayBackslash) {
		Input.Failure = FailureReason::StrayBackslash;
		// 忠実な出力が原文其の物しか無い事の返戻
		return Stop(ParseDecision::Skip);
	}
	if(!Prepared->empty() && DataStart != std::string::npos) Prepared->append(Source, DataStart);
	const std::string &Text = Prepared->empty() ? Source : *Prepared;
	const size_t PreparedDataStart = DataStart == std::string::npos ? DataStart : Text.size() - (Source.size() - DataStart);
	// 構文木の初回構築
	const bool HasReusableTrial = Trial && *Trial && (*Trial)->IsParsed() && Prepared->empty() &&
	(*Trial)->GetProfile().GetLanguage().TsLang() == Input.Profile->GetLanguage().TsLang() && (
		DataStart == std::string::npos ?
		std::string_view(**Trial) == Source :
		(*Trial)->HaltedDataStart() == std::string::npos && std::string_view(**Trial) == std::string_view(Source).substr(0, DataStart)
	);
	if(HasReusableTrial) {
		// 同じ文法の試験木を確定後の記法規則へ結び直す
		(*Trial)->Grammar = Input.Profile;
		if(DataStart != std::string::npos) (*Trial)->AttachTrailingData(std::string_view(Source).substr(DataStart));
		Input.Src.emplace(std::move(**Trial));
		Trial->reset();
	} else Input.Src.emplace(*Input.Profile, Text, PreparedDataStart);
	// 初回解析の失敗
	if(!Input.Src->IsParsed()) {
		Input.Failure = Input.Src->HasExpiredParse() ? FailureReason::ParseTimeExceeded : FailureReason::ParserInitializationFailed;
		// 時間切れを通常の見送，解析器の初期化失敗を内部の失敗とする判定の返戻
		return Stop(Input.Src->HasExpiredParse() ? ParseDecision::Skip : ParseDecision::Fail);
	}
	// 開いた要素の名前を解析器が失い閉じタグを読み違えた HTML の見送（整形仕様 9.3）
	if(Input.Src->GetReading().HasEmbeddedScripts && LosesHtmlElementNames(*Input.Src)) {
		Input.Failure = FailureReason::ParserNestingExceeded;
		// 解析器の資源上限で読めない原稿の返戻
		return Stop(ParseDecision::Skip);
	}
	// 共有拡張子だけ破損減少時に C++ へ昇格し，C 利用側も保つ共通の書換に限定
	if(CanPromote) {
		const uint32_t CBroken =
		(ts_node_has_error(Input.Src->GetRoot()) ? SyntaxParser::CountBrokenNodes(*Input.Src, Input.Src->GetRoot()) : 0) +
		SyntaxParser::CountMalformedCDefinitions(*Input.Src);
		// 昇格先は C からも読まれ得る文法で，其の定義で木を組み，以後の投影・正規化・最終検証へ同じ定義を引き継ぐ（整形仕様 4.2）
		Lang Promoted = Lang::Get(Profile.GetReading().HeaderPromotion);
		Promoted.IsCShared = true;
		const GrammarProfile &PromotedProfile = GrammarProfile::Resolve(Promoted);
		std::optional<ParsedTree> CppSrc;
		if(CBroken) CppSrc.emplace(PromotedProfile, Text);
		// C++ の方が破損を減らす入力
		if(CppSrc && CppSrc->IsParsed() && SyntaxParser::CountBrokenNodes(*CppSrc, CppSrc->GetRoot()) < CBroken) {
			Input.Src = std::move(CppSrc);
			Input.Profile = &PromotedProfile;
		}
	}
	// HTML 風コメント（言語仕様の付属書 B）は，古典スクリプトでは整形仕様 3.2 の記法に無く投影出来ない為の見送とし，文法が認めないモジュール等では解析器の読みを信用出来ない入力誤りとする（整形仕様 4.6, 10.4）
	if(Input.Src->GetReading().HasUnsupportedMarkupComments) {
		const bool IsScript = IsClassicScript(*Input.Src);
		if(
			HasDescendantOf(
				Input.Src->GetRoot(),
				[&Input](const TSNode Node) -> bool {
					// 記法に無いコメントかの返戻
					return Input.Src->Is(Node, Core::Role::UnsupportedComment);
				}
			) || IsScript && HasOperatorMarkupComment(*Input.Src)
		) {
			Input.Failure = IsScript ? FailureReason::MarkupCommentsUnsupported : FailureReason::ParserUnreadable;
			// 投影出来ない原稿の返戻
			return Stop(ParseDecision::Skip);
		}
	}
	// Java Unicode エスケープの展開と構文木の実字句への一致
	if(Input.Src->GetRules().Notation.DecodesUnicodeEscapes) {
		std::string Expanded;
		// javac と同じ字句展開に失敗
		if(!ParseAdapter::ExpandJavaUnicodeEscapes(*Input.Src, Expanded)) {
			Input.Failure = FailureReason::ParserUnreadable;
			// Unicode エスケープ展開失敗時の返戻
			return Stop(ParseDecision::Skip);
		}
		if(!Expanded.empty()) Input.Src.emplace(*Input.Profile, Expanded);
	}
	// 原稿補正後の解析失敗
	if(!Input.Src->IsParsed()) {
		Input.Failure = Input.Src->HasExpiredParse() ? FailureReason::ParseTimeExceeded : FailureReason::ParserInitializationFailed;
		// 補正後の時間切れと解析器故障の区分の返戻
		return Stop(Input.Src->HasExpiredParse() ? ParseDecision::Skip : ParseDecision::Fail);
	}
	// 埋込文書を含む Ruby
	if(Input.Src->GetRules().Notation.IsAvailable(CommentNotation::RubyEmbeddedDocument)) {
		std::optional<std::string> Escaped = ParseAdapter::EscapeRubyEmbeddedDocs(*Input.Src, Marker);
		if(!Escaped) {
			Input.Failure = FailureReason::ParserUnreadable;
			// 印を選べず埋込文書の誤読を補修出来ない原稿の返戻（整形仕様 4.2）
			return Stop(ParseDecision::Skip);
		}
		if(!Escaped->empty()) {
			Input.Src.emplace(*Input.Profile, std::move(*Escaped));
			// 埋込文書退避後の解析失敗
			if(!Input.Src->IsParsed()) {
				Input.Failure = Input.Src->HasExpiredParse() ? FailureReason::ParseTimeExceeded : FailureReason::ParserInitializationFailed;
				// 埋込文書退避後の時間切れと解析器故障の区分の返戻
				return Stop(Input.Src->HasExpiredParse() ? ParseDecision::Skip : ParseDecision::Fail);
			}
		}
	}
	// 投影が本来の字句へ復号する解析用の印の記録（整形仕様 4.2）
	RecordMark(*Input.Src, Marker, std::move(Escaped));
	ParsedTree &Src = *Input.Src;
	// 実引数を逐語保持するマクロと文へ展開して本体の波括弧を要するマクロを収集
	if(Src.GetReading().HasPreprocessorMacros) Src.CollectMacros();
	// 言語別の補修自身が壊した節点も最終の構文検査で見付ける為，補修の前に壊れて居た節点の数を控える（健全な入力では数えない）
	Input.PreparedBroken = ts_node_has_error(Src.GetRoot()) ? SyntaxParser::CountBrokenNodes(Src, Src.GetRoot()) : 0;
	// 補修の前の読取結果の返戻
	return Input;
}

/**
 * 正規化結果からの解析用の入力の読取関数
 * @param Profile 対象の文法
 * @param Input 入力の指定と正規化した字面
 * @return 読取の結果
 */
ParseAdapter::ParseInput ParseAdapter::Read(const GrammarProfile &Profile, InputNormalizer::NormalizedInput &Input) {
	ParseInput Result = ReadPrepared(Profile, Input.Text, Input.DataStart, &Input.Trial);
	Result.Source = ParseInput::VerificationSource{ Input.Text, {}, {}, {} };
	// 完成候補の字面を持つ読取結果の返戻
	return Result;
}

/**
 * 空の実引数を誤りと読む解析結果の補修関数
 * @param Src 読違を含む解析用の入力
 */
void ParseAdapter::RepairEmptyArgumentErrors(ParsedTree &Src) {
	// 誤読された空の引数括弧の編集収集
	std::vector<TextEdit> Edits;
	WalkAst(
		Src.GetRoot(),
		[&](const TSNode Node) -> void {
			// 対象の呼出以外を編集しない場合の終了
			if(!Src.Is(Node, Core::Role::IncludeStatementNode)) return;
			for(uint32_t Index = 0; Index < ts_node_child_count(Node); ++Index) {
				const TSNode Child = ts_node_child(Node, Index);
				if(
					!Src.Is(Child, Core::Role::Error) || ts_node_child_count(Child) != 2 ||
					!Src.Is(ts_node_child(Child, 0), Core::Role::OpeningParenToken) ||
					!Src.Is(ts_node_child(Child, 1), Core::Role::ClosingParenToken)
				) continue;
				TextEdit::Push(Src.Start(Child), Src.End(Child), "", Edits);
				// 最初の括弧の並びだけを消した場合の終了
				return;
			}
		}
	);
	// 本文と構文木の世代及び補修前への座標写像を同じ編集で更新
	TextEdit::Apply(Src, Edits);
	// 終了
	return;
}

/**
 * 解析器の読違の補修関数（整形仕様 3.4 の Adapt の後段，4.2）
 * コメントを紐付ける前に解析器の読違を補修し，入力と完成候補で同じ投影の前提を用いる
 * @param Input 読取を済ませた入力（補修後の字面と解析結果へ更新する）
 * @return 補修を完了出来れば true（既知の読違を補修出来なければ見送へ更新して false）
 */
bool ParseAdapter::Repair(ParseInput &Input) {
	ParsedTree &Src = *Input.Src;
	if(Src.IsParsed()) if(const FailureReason Reason = RepairBeforeProjection(Src); Reason.Value != FailureReason::None) {
		Input.Decision = ParseDecision::Skip;
		Input.Failure = Reason;
		// 既知の読違を補修出来なかった原稿の見送の返戻
		return false;
	}
	if(Src.GetReading().MisreadsEmptyArguments) RepairEmptyArgumentErrors(Src);
	// 補修後の構文木と位置で名前の宣言と取込を収集
	if(Src.GetReading().HasPreprocessorMacros) Src.CollectDeclaredNames();
	// JSX の本文の端の空白の読みの確定
	MarkJsxTextEdges(Src);
	// 補修を完了した事の返戻
	return true;
}

/**
 * 解析用の入力の適応関数（整形仕様 3.4 の Adapt，4.2）
 * 字面の段の可逆な保護・解析・共有ヘッダの文法の選び直し・字句化前の逃避の展開・解析器の読違の補修を入力側で完結させる
 * 試験解析の結果は，確定した入力と文法が一致する限り後の工程が再利用する
 * @param Profile 入力で選んだ文法
 * @param Normalized 正規化した字面と入力の指定
 * @return 適応の結果（`.h` の中身が C++ と判れば言語を書き換える）
 */
ParseAdapter::ParseInput ParseAdapter::Adapt(const GrammarProfile &Profile, InputNormalizer::NormalizedInput &Normalized) {
	ParseInput Input = Read(Profile, Normalized);
	Input.Source = ParseInput::FormattingSource{ Normalized.Text };
	// 読取を続けれた入力の補修
	if(Input.Decision == ParseDecision::Continue) Repair(Input);
	// 適応した入力の返戻
	return Input;
}

/**
 * 補修前の正規化した字面の取得関数
 * @return 入力又は完成候補の本文
 */
std::string_view ParseInput::SourceText() const {
	// 型で区別した字面の返戻
	return std::visit(
		[](const auto &Origin) -> std::string_view {
			return Origin.Text;
		},
		Source
	);
}
