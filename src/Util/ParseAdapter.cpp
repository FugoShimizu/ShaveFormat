#include "ParseAdapter.hpp"
#include "CharClass.hpp"
#include "HtmlTag.hpp"
#include "SyntaxParser.hpp"
#include "TextEdit.hpp"
#include "../Core/CommentRepository.hpp"
#include "../Profile/GrammarProfile.hpp"
#include <tree_sitter/api.h>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

/**
 * テキストの準備の長さの変更の記録関数
 * 計算量：O(1)，編集の列の拡張は償却 O(1)
 * @param Start 適用前の始まり
 * @param End 適用前の終わり
 * @param NewSize 置換後の長さ
 * @param Batches 適用順の編集の列
 * @throws std::bad_alloc 対応位置の領域を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void ParseAdapter::RecordPreparationEdit(
	const size_t Start,
	const size_t End,
	const size_t NewSize,
	std::vector<AppliedEdit> &Batches
) {
	// 長さを変えないマーカーの置換の終了
	if(End - Start == NewSize) return;
	Batches.push_back(
		{
			static_cast<uint32_t>(Start),
			static_cast<uint32_t>(End),
			static_cast<uint32_t>(Start),
			static_cast<uint32_t>(Start + NewSize)
		}
	);
	// 終了
	return;
}

/**
 * 前処理の条件を跨ぐリンケージ指定の波括弧のマーカーへの置換関数
 * 対を成す物だけをマーカーのディレクティブ（`#pragma <マーカー>O<エンコードしたリテラル><マーカー>` / `#pragma <マーカー>C`）へ置き換えて解析させ，`RestoreLinkageGuards` で戻す
 * 計算量：ソース長 L，保護領域数 G，退避コメント数 C，復元位置数 R，マーカー選択の費用 Q に対し O(L + C log(G + 1) + R + Q)
 * @param Rules 入力の読取規則（条件を開くディレクティブの名前）
 * @param Src 原文（マーカーに使う制御文字を選ぶ）
 * @param Text 整理中の原文（行継続を繋いだ後，置換は此処へ行う）
 * @param SourceComments 同じ論理トークン走査で記録したコメントの準備前後の位置（開始順）
 * @param Marker マーカーの格納先（既に選んで有れば其れを使う）
 * @param Escaped 退避したコメント（置換の後の位置へ動かす）
 * @param Batches 準備前へ戻す位置の対応（適用順）
 * @param Restorations 空のディレクティブを解析用のトークンから戻す位置とテキストの対応
 * @return 置き換えた場合 true（置き換える対が在っても原文が候補の制御文字を全て含みマーカーを選べない場合は無い）
 * @throws std::bad_alloc 保護する位置又は置換本文を確保出来ない場合
 * @throws std::length_error 置換本文が文字列の最大長を超える場合
 */
std::optional<bool> ParseAdapter::MaskLinkageGuards(
	const ReadingRules &Rules,
	const std::string &Src,
	std::string &Text,
	const std::span<const PreparedCommentRange> SourceComments,
	unsigned char &Marker,
	std::vector<EscapedComment> &Escaped,
	std::vector<AppliedEdit> &Batches,
	std::vector<LexemeRestoration> &Restorations
) {
	// テキストに無ければ走査を省く事の返戻
	if(Text.find(Rules.LinkageHead) == std::string::npos) return false;
	const std::string_view BlockOpen = Core::CommentRepository::GetOpenerOf(CommentNotation::Block);
	const std::string_view BlockClose = Core::CommentRepository::GetCloserOf(CommentNotation::Block);
	const std::string_view LineOpen = Core::CommentRepository::GetOpenerOf(CommentNotation::SlashLine);
	// リテラルの区切とエスケープを読み，リンケージの値の綴りへ意味を仮定しない
	const auto LiteralEnd = [&Rules](const std::string_view Code, const size_t Start, char &Quote) -> size_t {
		for(const std::string_view Prefix : Rules.RawStrPrefixes) {
			if(
				(!Start || !IsIdentifierChar(Code[Start - 1])) && Code.substr(Start).starts_with(Prefix) &&
				Start + Prefix.size() < Code.size() && Code[Start + Prefix.size()] == '"'
			) {
				Quote = '"';
				const size_t DelimStart = Start + Prefix.size() + 1, Open = Code.find('(', DelimStart);
				// 読み終えないリテラルの後をコードとして扱わない事の返戻
				if(
					Open == std::string_view::npos || Open - DelimStart > 16 ||
					Code.substr(DelimStart, Open - DelimStart).find_first_of(" \t\r\n\\)") != std::string_view::npos
				) return Code.size();
				const std::string Close = ")" + std::string(Code.substr(DelimStart, Open - DelimStart)) + '"';
				const size_t End = Code.find(Close, Open + 1);
				// 生文字列の終端の返戻
				return End == std::string_view::npos ? Code.size() : End + Close.size();
			}
		}
		size_t Opening = Start;
		if(Start < Code.size() && Code[Start] != '"' && Code[Start] != '\'') for(const std::string_view Prefix : Rules.StrPrefixes) if(
			Code.substr(Start).starts_with(Prefix) && Start + Prefix.size() < Code.size() &&
			(Code[Start + Prefix.size()] == '"' || Code[Start + Prefix.size()] == '\'')
		) {
			Opening = Start + Prefix.size();
			break;
		}
		// リテラルでない開始位置の返戻
		if(Opening >= Code.size() || Code[Opening] != '"' && Code[Opening] != '\'') return std::string_view::npos;
		Quote = Code[Opening];
		for(size_t Pos = Opening + 1; Pos < Code.size(); ++Pos) {
			if(Code[Pos] == '\\') ++Pos;
			// 閉じたリテラルの終端の返戻
			else if(Code[Pos] == Quote) return Pos + 1;
			// 不正な改行で終わるリテラルの返戻
			else if(Code[Pos] == '\n') return Pos;
		}
		// 読み終えないリテラルの返戻
		return Code.size();
	};
	// コメントを空白へ置き換えたコピーと，内部をコードとして読まないリテラルの範囲
	std::string Blank(Text);
	std::vector<std::pair<size_t, size_t>> Literals;
	struct GuardComment {
		size_t Start; // コメントの開始位置
		size_t End; // コメントの終了位置
		CommentKind Kind; // ソースのトークンと改行で決まる配置の種別
	};
	std::vector<GuardComment> Comments;
	bool IsAtLineHead = true;
	for(size_t Pos = 0; Pos < Blank.size(); ++Pos) {
		if(Blank[Pos] == '\n') {
			IsAtLineHead = true;
			continue;
		}
		if(std::isspace(static_cast<unsigned char>(Blank[Pos]))) continue;
		char Quote = '\0';
		if(const size_t End = LiteralEnd(Blank, Pos, Quote); End != std::string_view::npos) {
			Literals.emplace_back(Pos, End);
			IsAtLineHead = false;
			Pos = End - 1;
			continue;
		}
		const bool IsLine = !Blank.compare(Pos, LineOpen.size(), LineOpen);
		if(!IsLine && Blank.compare(Pos, BlockOpen.size(), BlockOpen)) {
			IsAtLineHead = false;
			continue;
		}
		const size_t Close = IsLine ? Blank.find('\n', Pos) : Blank.find(BlockClose, Pos + BlockOpen.size());
		const size_t End = Close == std::string::npos ? Blank.size() : IsLine ? Close : Close + BlockClose.size();
		Comments.push_back({ Pos, End, IsAtLineHead ? CommentKind::OwnLine : IsLine ? CommentKind::Trailing : CommentKind::Inline });
		for(; Pos < End; ++Pos) {
			if(Blank[Pos] == '\n') IsAtLineHead = true;
			else Blank[Pos] = ' ';
		}
		--Pos;
	}
	// 空行を除いた各行のインデントを除いた中身（整形でディレクティブの前後に置く空行は並びを変えない）
	std::vector<std::string_view> Lines;
	for(size_t Start = 0; Start < Blank.size();) {
		// 各行の非空白本文抽出
		const size_t End = std::min(Blank.find('\n', Start), Blank.size());
		if(const size_t Head = Blank.find_first_not_of(" \t\f\v\r", Start); Head < End) {
			Lines.emplace_back(Blank.data() + Head, End - Head);
		}
		Start = End + 1;
	}
	// プリプロセッサディレクティブの見出の取得
	const auto Directive = [](const std::string_view Content) -> std::string_view {
		// ディレクティブでない場合の返戻
		if(!Content.starts_with('#')) return {};
		// ディレクティブ名候補の開始位置設定
		size_t Start = 1;
		// ディレクティブマーカー後の水平空白読飛
		while(Start < Content.size() && std::isspace(static_cast<unsigned char>(Content[Start]))) ++Start;
		// ディレクティブ名の終端探索
		size_t End = Start;
		while(End < Content.size() && IsIdentifierChar(Content[End])) ++End;
		// ディレクティブ名の返戻
		return Content.substr(Start, End - Start);
	};
	// 条件のトークンを１度正規化し，同じ外側の条件と枝に同じ識別番号を与える
	const auto ConditionText = [&Rules, &LiteralEnd](const std::string_view Code) -> std::string {
		std::string Result;
		for(size_t Pos = 0; Pos < Code.size();) {
			if(std::isspace(static_cast<unsigned char>(Code[Pos]))) {
				++Pos;
				continue;
			}
			const size_t Start = Pos;
			char Quote = '\0';
			if(const size_t End = LiteralEnd(Code, Pos, Quote); End != std::string_view::npos) Pos = End;
			else if(
				std::isdigit(static_cast<unsigned char>(Code[Pos])) ||
				Code[Pos] == '.' && Pos + 1 < Code.size() && std::isdigit(static_cast<unsigned char>(Code[Pos + 1]))
			) {
				// 前処理の数値のトークンを分断せず，指数の符号と桁の区切も保持する
				for(++Pos; Pos < Code.size(); ++Pos) {
					if(
						IsIdentifierChar(Code[Pos]) || Code[Pos] == '.' || Code[Pos] == '\'' || (Code[Pos] == '+' || Code[Pos] == '-') &&
						(Code[Pos - 1] == 'e' || Code[Pos - 1] == 'E' || Code[Pos - 1] == 'p' || Code[Pos - 1] == 'P')
					) continue;
					break;
				}
			} else if(IsIdentifierChar(Code[Pos])) while(Pos < Code.size() && IsIdentifierChar(Code[Pos])) ++Pos;
			else {
				const std::vector<std::string_view>::const_iterator Found = std::ranges::find_if(
					Rules.CondPunctuators,
					[&](const std::string_view Token) -> bool {
						// 現在位置の最長の複合トークンかの返戻
						return Code.substr(Pos).starts_with(Token);
					}
				);
				Pos += Found == Rules.CondPunctuators.end() ? 1 : Found->size();
			}
			Result.append(Code.substr(Start, Pos - Start));
			Result += '\0';
		}
		// 空白に依らない条件のトークンと意味を持つ境界の返戻
		return Result;
	};
	struct Guard {
		size_t Start; // 置き換えるトークンの開始位置
		size_t Len; // 後続のコメントを除いたトークンの長さ
		bool IsOpen; // リンケージ指定の開きか
		std::string Value; // 開きの文字列リテラルのトークン列（閉じでは空）
		std::string Placeholder; // 解析用のディレクティブと可逆にエンコードしたリテラル
		size_t Condition; // 外側の条件と枝を含めた対応
		size_t Epoch; // 条件以外のディレクティブに依る状態の境界
		int64_t Shift = 0; // 自身の長さの差，置換後は此処迄の累積差
		size_t End = 0; // 後続の末尾コメントも含めた置換の終端
	};
	std::vector<Guard> Guards;
	std::unordered_map<size_t, std::unordered_map<std::string, size_t>> ConditionIds;
	std::vector<size_t> Conditions;
	size_t Literal = 0, Epoch = 0, NextCondition = 0;
	for(size_t Idx = 0; Idx < Lines.size(); ++Idx) {
		const std::string_view Body = Lines[Idx];
		const size_t Start = static_cast<size_t>(Body.data() - Blank.data());
		while(Literal < Literals.size() && Literals[Literal].second <= Start) ++Literal;
		if(Literal < Literals.size() && Literals[Literal].first <= Start) continue;
		const std::string_view Head = Directive(Body);
		const bool IsCondOpen = std::ranges::find(Rules.CondDirectives, Head) != Rules.CondDirectives.end();
		const bool IsAlternative = std::ranges::find(Rules.CondAlternativeDirectives, Head) != Rules.CondAlternativeDirectives.end();
		if(IsCondOpen || IsAlternative) {
			// 開いて居ない条件の別枝を読んだ場合の返戻
			if(IsAlternative && Conditions.empty()) return false;
			const size_t Parent = Conditions.empty() ? 0 : Conditions.back();
			const auto [Entry, IsInserted] = ConditionIds[Parent].try_emplace(ConditionText(Body), NextCondition + 1);
			NextCondition += IsInserted;
			const size_t Condition = Entry->second;
			if(IsAlternative) Conditions.back() = Condition;
			else Conditions.push_back(Condition);
			continue;
		}
		if(Head == Rules.CondEndDirective) {
			// 開いて居ない条件の閉じを読んだ場合の返戻
			if(Conditions.empty()) return false;
			Conditions.pop_back();
			continue;
		}
		if(!Head.empty()) {
			// 未追跡のディレクティブが条件名の意味を変える可能性を残す
			++Epoch;
			continue;
		}
		// 意味を持たない改行に依らず，条件の直後のリンケージ指定だけを読む
		if(
			!Idx || Conditions.empty() ||
			std::ranges::find(Rules.CondDirectives, Directive(Lines[Idx - 1])) == Rules.CondDirectives.end() ||
			!Body.starts_with(Rules.LinkageHead) && !Body.starts_with(Rules.LinkageClose)
		) continue;
		size_t Last = Idx + 1;
		while(Last < Lines.size() && Directive(Lines[Last]).empty()) ++Last;
		if(Last >= Lines.size() || Directive(Lines[Last]) != Rules.CondEndDirective) continue;
		std::string_view Code(Blank.data() + Start, static_cast<size_t>(Lines[Last].data() - Blank.data()) - Start);
		while(!Code.empty() && std::isspace(static_cast<unsigned char>(Code.back()))) Code.remove_suffix(1);
		const bool IsOpen = Code != Rules.LinkageClose;
		std::string Value;
		if(IsOpen) {
			// キーワード・文字列リテラル・開き波括弧の構造だけを照合する
			if(!Code.starts_with(Rules.LinkageHead)) continue;
			const size_t HeadEnd = Rules.LinkageHead.size();
			if(HeadEnd < Code.size() && IsIdentifierChar(Code[HeadEnd])) continue;
			size_t Pos = HeadEnd;
			while(Pos < Code.size()) {
				while(Pos < Code.size() && std::isspace(static_cast<unsigned char>(Code[Pos]))) ++Pos;
				char Quote = '\0';
				const size_t End = LiteralEnd(Code, Pos, Quote);
				if(End == std::string_view::npos || End == Code.size() || Quote != '"') break;
				if(!Value.empty()) Value += ' ';
				Value.append(Code.substr(Pos, End - Pos));
				Pos = End;
			}
			if(Value.empty() || Code.substr(Pos) != "{") continue;
		}
		Guards.push_back({ Start, Code.size(), IsOpen, std::move(Value), {}, Conditions.back(), Epoch });
	}
	// 条件の対応と，入れ子の波括弧が対を成す事の確認
	std::vector<size_t> OpenGuards;
	for(size_t Idx = 0; Idx < Guards.size(); ++Idx) {
		const Guard &Entry = Guards[Idx];
		if(Entry.IsOpen) OpenGuards.push_back(Idx);
		else {
			// 対を成す同じ条件と状態の開きが無い場合の返戻
			if(OpenGuards.empty()) return false;
			const Guard &Open = Guards[OpenGuards.back()];
			// 条件又は其の意味の状態が一致しない場合の返戻
			if(Open.Condition != Entry.Condition || Open.Epoch != Entry.Epoch) return false;
			OpenGuards.pop_back();
		}
	}
	// 対を成さない場合の返戻
	if(Guards.empty() || !OpenGuards.empty() || !Conditions.empty()) return false;
	// マーカーを選べず誤読を修復出来ない事の返戻
	if(!Marker && !(Marker = TextEdit::AbsentControlChar(Src))) return std::nullopt;
	// 前方位置保持の為の後方トークンからマーカーへの置換
	// 置換先は行を占めるプリプロセッサディレクティブに為り，ディレクティブは１論理行を保つ為に折返の候補へ入らない（整形仕様 2.1, 4.2 の幅の計数へマーカーを残さない）
	const size_t GuardBatchStart = Batches.size(), EscapedBeforeGuards = Escaped.size();
	size_t SourceComment = SourceComments.size(), CommentIdx = Comments.size();
	for(size_t Idx = Guards.size(); Idx--;) {
		Guard &Entry = Guards[Idx];
		Entry.Placeholder = std::string(Rules.LinkageMarkerDirective) + static_cast<char>(Marker) + (Entry.IsOpen ? 'O' : 'C');
		if(Entry.IsOpen) {
			// 生文字列内の改行も，解析用の１論理行の中へ可逆に保持する
			static constexpr char Hex[] = "0123456789ABCDEF";
			for(const unsigned char Byte : Entry.Value) {
				Entry.Placeholder += Hex[Byte >> 4];
				Entry.Placeholder += Hex[Byte & 0XF];
			}
			Entry.Placeholder += static_cast<char>(Marker);
		}
		// トークンの後の同じ行のコメント（連結指定はコードの為，ディレクティブの中へ残さずパーサーへ渡さずに退避する，配置はコメント経路が決める）
		size_t LineEnd = std::min(Text.find('\n', Entry.Start + Entry.Len), Text.size());
		// 本体の中と末尾のコメントを１度ずつ退避し，コードの候補化にコメントを使わない
		while(CommentIdx && Comments[CommentIdx - 1].Start >= Entry.Start) {
			const auto [Begin, End, Kind] = Comments[--CommentIdx];
			if(Begin >= LineEnd) continue;
			LineEnd = std::max(LineEnd, End);
			while(SourceComment && SourceComments[SourceComment - 1].Start > Begin) --SourceComment;
			std::optional<std::pair<uint32_t, uint32_t>> InputRange;
			if(SourceComment && SourceComments[SourceComment - 1].Start == Begin) {
				const PreparedCommentRange &Comment = SourceComments[--SourceComment];
				InputRange.emplace(Comment.InputStart, Comment.InputEnd);
			}
			const CommentKind Placement = Kind == CommentKind::Inline && Begin >= Entry.Start + Entry.Len ? CommentKind::Trailing : Kind;
			Escaped.push_back({ static_cast<uint32_t>(Entry.Start), Text.substr(Begin, End - Begin), Placement, InputRange });
		}
		const size_t Removed = LineEnd - (Entry.Start + Entry.Len);
		// 退避したコメントの終端も含めた置換の長さの控え
		Entry.Shift = static_cast<int64_t>(Entry.Placeholder.size()) - static_cast<int64_t>(Entry.Len + Removed);
		RecordPreparationEdit(Entry.Start, LineEnd, Entry.Placeholder.size(), Batches);
		Entry.End = LineEnd;
	}
	std::reverse(Escaped.begin() + static_cast<std::ptrdiff_t>(EscapedBeforeGuards), Escaped.end()); // 同じマーカーへ結び付けたコメントも原文の順を保つ
	// 保持する断片とマーカーを原文順に１度組み立てる
	std::string Prepared;
	Prepared.reserve(Text.size());
	size_t Read = 0;
	for(const Guard &Entry : Guards) {
		Prepared.append(Text, Read, Entry.Start - Read);
		Prepared += Entry.Placeholder;
		Read = Entry.End;
	}
	Prepared.append(Text, Read, Text.size() - Read);
	Text = std::move(Prepared);
	// 始まりの順の累積差から，各退避位置を１度だけ動かす
	int64_t GuardShift = 0;
	for(Guard &Entry : Guards) {
		GuardShift += Entry.Shift;
		Entry.Shift = GuardShift;
	}
	for(size_t Idx = 0; Idx < Escaped.size(); ++Idx) {
		EscapedComment &Comment = Escaped[Idx];
		const std::vector<Guard>::const_iterator After = std::lower_bound(
			Guards.begin(),
			Guards.end(),
			Comment.Pos,
			[](const Guard &Entry, const uint32_t Pos) -> bool {
				// 退避位置より厳密に前の置換かの返戻
				return Entry.Start < Pos;
			}
		);
		if(After != Guards.begin()) Comment.Pos = static_cast<uint32_t>(Comment.Pos + std::prev(After)->Shift);
		// 此の段で退避した末尾コメントは，自身の置換の累積差を含めずマーカーの直後へ置く
		if(Idx >= EscapedBeforeGuards && Comment.Kind == CommentKind::Trailing) {
			Comment.Pos += static_cast<uint32_t>(After->Placeholder.size());
		}
	}
	// 後方から記録した置換を前方から１度消費し，別の論理行に在る空のディレクティブのデコード範囲の移動
	size_t EditIdx = Batches.size();
	int64_t Offset = 0;
	for(LexemeRestoration &Restoration : Restorations) {
		while(EditIdx > GuardBatchStart && Batches[EditIdx - 1].End < Restoration.Start) {
			const AppliedEdit &Edit = Batches[--EditIdx];
			Offset += static_cast<int64_t>(Edit.NewEnd) - Edit.End;
		}
		Restoration.Start = static_cast<uint32_t>(Restoration.Start + Offset);
		Restoration.End = static_cast<uint32_t>(Restoration.End + Offset);
	}
	// 置き換えた事の返戻
	return true;
}

/**
 * マーカーへ置き換えたリンケージ指定の波括弧の復元関数
 * 計算量：ソースの長さ L に対し O(L)
 * @param Rules 入力の読取規則
 * @param Text 整形後の原文
 * @param Marker 置換に使ったマーカー
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void ParseAdapter::RestoreLinkageGuards(const ReadingRules &Rules, std::string &Text, const unsigned char Marker) {
	// 置換マーカーの検索トークン作成
	const std::string Placeholder = std::string(Rules.LinkageMarkerDirective) + static_cast<char>(Marker);
	// マーカーの無い本文は其の儘保持
	const size_t First = Text.find(Placeholder);
	// 復元するマーカーが無い場合の終了
	if(First == std::string::npos) return;
	// 元の本文を前進して，復元した断片を１度だけ組み立てる
	std::string Decoded;
	Decoded.reserve(Text.size());
	size_t Read = 0;
	for(size_t Found = First; Found != std::string::npos; Found = Text.find(Placeholder, Found + 1)) {
		const size_t Kind = Found + Placeholder.size();
		if(Kind >= Text.size() || Text[Kind] != 'O' && Text[Kind] != 'C') continue;
		const size_t End = Text[Kind] == 'O' ? Text.find(static_cast<char>(Marker), Kind + 1) : Kind;
		if(End == std::string::npos) continue;
		Decoded.append(Text, Read, Found - Read);
		if(Text[Kind] == 'O') {
			Decoded += Rules.LinkageHead;
			Decoded += ' ';
			static constexpr std::string_view Hex = "0123456789ABCDEF";
			for(size_t Pos = Kind + 1; Pos + 1 < End; Pos += 2) {
				const size_t High = Hex.find(Text[Pos]), Low = Hex.find(Text[Pos + 1]);
				Decoded += static_cast<char>(High << 4 | Low);
			}
			Decoded += " {";
		} else Decoded += Rules.LinkageClose;
		Read = End + 1;
	}
	// 未登録のマーカーは変更せず，元の残余も保持
	if(Read) {
		Decoded.append(Text, Read, Text.size() - Read);
		Text = std::move(Decoded);
	}
	// 終了
	return;
}

/**
 * PHP の実行を止める `__halt_compiler()` の文かの判定関数
 * 計算量：文の直接の子数 C と引数のテキスト長 B に対し O(C + B)
 * @param Src ソースコード
 * @param Stmt 判定対象の文
 * @return `__halt_compiler()` の呼出の式文なら true
 */
bool ParseAdapter::IsHaltCall(const ParsedTree &Src, const TSNode Stmt) {
	// 入力の読取規則
	const ReadingRules &Reading = Src.GetReading();
	// 実行停止後のデータを持たない記法の返戻
	if(!Reading.HasHaltedData) return false;
	// 式文の先頭呼出候補
	const TSNode Call = ts_node_named_child(Stmt, 0);
	// 関数呼出式文でない事の返戻
	if(!Src.Is(Stmt, Core::Role::ExprStmt) || ts_node_is_null(Call) || !Src.Is(Call, Core::Role::FunctionCallExpr)) return false;
	const std::string_view Callee = Src.GetView(Src.GetFieldChild(Call, Core::ChildRole::FunctionField));
	const std::string_view HaltName = Reading.HaltCallName;
	// 名前の一致の返戻
	return std::equal(
		Callee.begin(),
		Callee.end(),
		HaltName.begin(),
		HaltName.end(),
		[](const unsigned char Left, const unsigned char Right) -> bool {
			// 小文字へ揃えた一致の返戻
			return std::tolower(Left) == Right;
		}
	);
}

/**
 * 試行解析の未完了理由の取得関数
 * 構文エラーを含む木を得た解析は完了とし，木を得れなかった解析だけ期限と初期化を区別する
 * 計算量：O(1)
 * @param Src 試行解析の結果
 * @return 完了なら理由無，期限切なら見送理由，其の他の未完了なら初期化失敗
 */
FailureReason ParseAdapter::GetTrialFailure(const ParsedTree &Src) {
	// 木を得た解析の返戻
	if(Src.IsParsed()) return FailureReason::None;
	// 木を得れなかった解析の未完了理由の返戻
	return Src.HasExpiredParse() ? FailureReason::ParseTimeExceeded : FailureReason::ParserInitFailed;
}

/**
 * 行頭のデータのマーカーと，マーカーと誤読される通常の式の取得関数
 * 同じ長さの仮の名前を使う試行解析だけで，リテラルとヒアドキュメントの対応を保った範囲を取得する
 * 計算量：入力長 B，ノード数 N，式の端迄の祖先の総数 A，解析の総費用 Q に対し O(B + N + A + Q)
 * @param Profile 対象の文法
 * @param Src 原文
 * @param CanStopAtMarker データ境界の判定又は確定済の境界をコードの終端とする準備か
 * @param Trial 同じ長さのテキストで得た範囲照合用の構文木（製品の解析には再利用しない）
 * @param Parens 通常の式をデータと誤読させない解析用の括弧の範囲
 * @param Reason 試行解析の期限切又は初期化失敗の格納先
 * @return データ開始位置（無ければ `std::string::npos`）
 * @throws std::bad_alloc 解析又は範囲のメモリを確保出来ない場合
 */
size_t ParseAdapter::ReadTrailingDataMarker(
	const GrammarProfile &Profile,
	const std::string &Src,
	const bool CanStopAtMarker,
	std::optional<ParsedTree> &Trial,
	std::vector<std::pair<uint32_t, uint32_t>> &Parens,
	FailureReason &Reason
) {
	const std::string_view Marker = Profile.GetReading().TrailingDataMarker;
	// マーカーを持たない原文の返戻
	if(Marker.empty() || Src.find(Marker) == std::string::npos) return std::string::npos;
	// 処理系の境界条件は，通常の木と範囲照合の木で共有する
	const auto IsBoundary = [&Src, Marker](const size_t Begin, const size_t End) -> bool {
		// 行頭単独のマーカーと LF・CRLF・EOF の一致の返戻
		return End - Begin == Marker.size() && std::string_view(Src).substr(Begin, End - Begin) == Marker &&
		(!Begin || Src[Begin - 1] == '\n') &&
		(End == Src.size() || Src[End] == '\n' || Src[End] == '\r' && End + 1 < Src.size() && Src[End + 1] == '\n');
	};
	if(CanStopAtMarker) {
		// 正しく読めたデータは試行のコードにも渡さず，従来通り境界で解析を止める
		if(!Trial || static_cast<const std::string &>(*Trial) != Src) Trial.emplace(Profile, Src);
		// 構文の範囲を得れない場合の返戻
		if((Reason = GetTrialFailure(*Trial)) != FailureReason::None) return std::string::npos;
		size_t Confirmed = std::string::npos;
		WalkChildrenCursor(
			Trial->GetRoot(),
			[&](const TSNode Node) -> bool {
				// 境界を確定した後の降下の終了
				if(Confirmed != std::string::npos) return false;
				if(Trial->Is(Node, Core::Role::TrailingDataSection)) {
					const size_t End = Trial->GetStart(Node);
					if(End >= Marker.size() && IsBoundary(End - Marker.size(), End)) Confirmed = End;
				}
				// 境界を包んだエラーノードだけを開く事の返戻
				return ts_node_is_error(Node);
			}
		);
		// 正しく読めた境界の返戻
		if(Confirmed != std::string::npos) return Confirmed;
	}
	// 仮の名前は範囲照合だけに使い，誤読したデータ領域に後続のコードを隠させない
	std::string Probe = Src;
	for(size_t Pos = Probe.find(Marker); Pos != std::string::npos; Pos = Probe.find(Marker, Pos + Marker.size())) {
		Probe[Pos] = Probe[Pos] == 'a' ? 'b' : 'a';
	}
	Trial.emplace(Profile, std::move(Probe));
	const ParsedTree &Raw = *Trial;
	// 範囲照合用の解析を得れない場合の返戻
	if((Reason = GetTrialFailure(Raw)) != FailureReason::None) return std::string::npos;
	size_t DataStart = std::string::npos, NextLine = Src.find_first_of("\r\n"), FirstText = Src.find_first_not_of(" \t");
	WalkChildrenCursor(
		Raw.GetRoot(),
		[&](const TSNode Node, const std::span<const TSNode> Ancestors) -> bool {
			// 最初のデータ境界の後へ降りない事の返戻
			if(DataStart != std::string::npos) return false;
			// 名前以外の内側へ降りる事の返戻
			if(!Raw.Is(Node, Core::Role::Identifier)) return true;
			const uint32_t Begin = Raw.GetStart(Node), End = Raw.GetEnd(Node);
			// 原文のマーカーと同じトークンでない場合の返戻
			if(End - Begin != Marker.size() || std::string_view(Src).substr(Begin, End - Begin) != Marker) return false;
			// 名前はドキュメント順で訪れ，パーサーが行頭と読む CR・LF の後のテキストは１度だけ求める
			while(NextLine != std::string::npos && NextLine < Begin) {
				const size_t LineStart = NextLine + 1;
				NextLine = Src.find_first_of("\r\n", LineStart);
				FirstText = Src.find_first_not_of(" \t", LineStart);
			}
			// Ruby の処理系は行頭単独の `__END__` と LF・CRLF・EOF の組だけを認める
			if(CanStopAtMarker && IsBoundary(Begin, End)) {
				DataStart = End;
				// 処理系がデータ開始とするトークンの後へ降りない事の返戻
				return false;
			}
			// 行頭の名前をデータと誤読するパーサーへ，通常の式を囲んで渡す
			if(FirstText != Begin) return false;
			TSNode Expr = Node;
			for(size_t Idx = Ancestors.size(); Idx--;) {
				const TSNode Parent = Ancestors[Idx];
				const bool IsSequence = Raw.Is(Parent, Core::Role::StmtHost) || Raw.Is(Parent, Core::Role::DelimitedStmtSeq);
				if(IsSequence || Raw.GetStart(Parent) != Begin) break;
				Expr = Parent;
			}
			Parens.emplace_back(Begin, Raw.GetEnd(Expr));
			// 通常の名前の内側へ降りない事の返戻
			return false;
		}
	);
	// 処理系の条件を満たした境界の返戻
	return DataStart;
}

/**
 * コードの後に続く逐語データの開始位置の取得関数
 * PHP の `__halt_compiler()` と Ruby の `__END__` の直後から，データを正規化・コードの解析・最終検証から外す
 * 計算量：原文長 B と試行解析・照会の総費用 Q に対し O(B + Q)
 * @param Profile 対象の文法
 * @param Src 整形対象のソース（正規化前又は改行の正規化済）
 * @param Trial 同じテキストを解析済なら再利用する試行解析の結果
 * @param Reason 試行解析の期限切又は初期化失敗の格納先
 * @return 逐語データの開始位置（文法の境界を確定出来なければ `std::string::npos`）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 */
size_t ParseAdapter::GetHaltedDataStart(
	const GrammarProfile &Profile,
	const std::string &Src,
	std::optional<ParsedTree> &Trial,
	FailureReason &Reason
) {
	// 対象文法の記法
	const ReadingRules &Rules = Profile.GetReading();
	// コードの後のデータを持たない記法又はマーカーの無い原文の返戻
	if(!Rules.HasHaltedData && Rules.TrailingDataMarker.empty()) return std::string::npos;
	if(!Rules.TrailingDataMarker.empty()) {
		std::vector<std::pair<uint32_t, uint32_t>> Parens;
		// マーカーの有無と処理系の境界を同じ読取で判定した結果の返戻
		return ReadTrailingDataMarker(Profile, Src, true, Trial, Parens, Reason);
	}
	if(
		const std::string_view HaltName = Rules.HaltCallName; std::search(
			Src.begin(),
			Src.end(),
			HaltName.begin(),
			HaltName.end(),
			[](const unsigned char Left, const unsigned char Right) -> bool {
				// 小文字へ揃えた一致の返戻
				return std::tolower(Left) == Right;
			}
		) == Src.end()
	// 実行停止の呼出名が無い事の返戻
	) return std::string::npos;
	if(!Trial) Trial.emplace(Profile, Src);
	const ParsedTree &Raw = *Trial;
	// 構文木を得れない場合の返戻
	if((Reason = GetTrialFailure(Raw)) != FailureReason::None) return std::string::npos;
	size_t DataStart = std::string::npos;
	// 呼出は最も外側の文に限られる為，根の直下だけを見る
	ForEachNamedChild(
		Raw.GetRoot(),
		[&](const TSNode Stmt) -> bool {
			// 他の文を飛ばす事の返戻
			if(!IsHaltCall(Raw, Stmt)) return true;
			// `;` で終えた文は其の直後，`?>` で終えた文は閉タグの直後からデータ
			if(Raw.Is(ts_node_child(Stmt, ts_node_child_count(Stmt) - 1), Core::Role::StmtTerminatorToken)) DataStart = Raw.GetEnd(Stmt);
			else if(const TSNode Next = ts_node_next_named_sibling(Stmt); !ts_node_is_null(Next)) {
				if(const TSNode Close = Raw.GetFirstNamedChild(Next, Core::Role::PhpEndTag); !ts_node_is_null(Close)) {
					DataStart = Raw.GetEnd(Close);
				}
			}
			// 実行が止まる以降の文を見ない事の返戻
			return false;
		}
	);
	// データの開始位置の返戻
	return DataStart;
}

/**
 * C / C++ の行継続とプリプロセッサディレクティブの整理関数（構文解析より前に原文のテキストで行う）
 * 生文字列外の行継続を翻訳段階と同様に除去する関数
 * ブロックコメントの中はコメントの外の意味に関わらない為，テキストを保つ
 * プリプロセッサディレクティブの本体は，tree-sitter-c が読み違えるコメントの開きの形を整える（本体のコメントを参照）
 * 計算量：ソース長 L，連結数 S，コメント数 C，連結指定数 G，復元位置数 R，行継続の綴りの総長 P，マーカー選択の費用 Q に対し O(L + S + C(log(C + G + 1) + P) + R + Q)
 * @param Rules 入力の読取規則（プリプロセッサディレクティブの名前）
 * @param Src 原文（改行は LF へ正規化済）
 * @param LineSplices 出力側と共有する行継続の綴り
 * @param Marker 置き換えた制御文字の格納先（置き換えなければ '\0'）
 * @param HasStrayBackslash 後のトークンが次の行へ移ると行末に立ち，原文に無い行継続に為る裸のバックスラッシュを含むかの格納先（整形を見送るマーカー）
 * @param HasForeignDialect 共有ヘッダの対象外の方言の検出結果
 * @param Escaped パーサーへ渡さずに退避したコメントの格納先（位置は整理後の原文の位置）
 * @param Batches 準備前へ戻す位置の対応（適用順）
 * @param Restorations 空のディレクティブとコメント内の改行を解析用のトークンから戻す位置とテキストの対応
 * @param DirectiveLines 論理トークン走査で確定した前処理の見出の範囲（準備後の位置）の格納先
 * @return 整理後の原文（変更が無ければ空，行コメントの三連字又は空白を挟む行継続を読めない場合，又はマーカーが要るのに原文が候補の制御文字を全て含み選べない場合は無い）
 * @throws std::bad_alloc 本文又は位置の対応を確保出来ない場合
 * @throws std::length_error 本文が文字列の最大長を超える場合
 */
std::optional<std::string> ParseAdapter::PrepareCSrc(
	const ReadingRules &Rules,
	const std::string &Src,
	const std::span<const std::string_view> LineSplices,
	unsigned char &Marker,
	bool &HasStrayBackslash,
	bool &HasForeignDialect,
	std::vector<EscapedComment> &Escaped,
	std::vector<AppliedEdit> &Batches,
	std::vector<LexemeRestoration> &Restorations,
	std::vector<std::pair<uint32_t, uint32_t>> &DirectiveLines
) {
	// 出力状態の初期化
	Marker = '\0';
	HasForeignDialect = false;
	bool HasAtHead = false;
	const size_t Size = Src.size();
	const std::string_view BlockOpen = Core::CommentRepository::GetOpenerOf(CommentNotation::Block);
	const std::string_view BlockClose = Core::CommentRepository::GetCloserOf(CommentNotation::Block);
	const std::string_view LineOpen = Core::CommentRepository::GetOpenerOf(CommentNotation::SlashLine);
	// 保護が必要な入力だけ，コメントを消す前の位置を同じ走査で控える
	bool HasLinkage = false;
	std::vector<PreparedCommentRange> SourceComments;
	// 水平空白を飛ばした位置
	const auto SkipHorizontal = [&Src, Size](size_t At) -> size_t {
		// 水平空白の読飛
		while(At < Size && (Src[At] == ' ' || Src[At] == '\t' || Src[At] == '\f' || Src[At] == '\v' || Src[At] == '\r')) ++At;
		// 水平空白の後の位置の返戻
		return At;
	};
	// 位置から後が水平空白とコメントだけで行が終わるか（改行を含むブロックコメントも行を終える物とする）
	const auto HasLineEndAfter = [&Src, Size, &SkipHorizontal, BlockOpen, BlockClose, LineOpen](size_t At) -> bool {
		// 対象範囲の走査
		while(true) {
			At = SkipHorizontal(At);
			// 行の終わりか行コメントに達した事の返戻
			if(At >= Size || Src[At] == '\n' || !Src.compare(At, LineOpen.size(), LineOpen)) return true;
			// トークンが続く事の返戻
			if(Src.compare(At, BlockOpen.size(), BlockOpen)) return false;
			const size_t Close = Src.find(BlockClose, At + BlockOpen.size());
			// 閉じないブロックコメントか改行を含むブロックコメントの返戻
			if(Close == std::string::npos || Src.find('\n', At) < Close) return true;
			At = Close + BlockClose.size();
		}
	};
	// 入力の読みを確定出来ない行継続の検出結果
	bool HasUnreadableSplice = false;
	// 水平空白を挟む物も含めた行継続候補の長さ
	const auto SpliceLen = [&Src, Size, &SkipHorizontal](const size_t At) -> size_t {
		// バックスラッシュでない位置の返戻
		if(At >= Size || Src[At] != '\\') return 0;
		const size_t End = SkipHorizontal(At + 1);
		// 改行を含む候補の長さの返戻
		return End < Size && Src[End] == '\n' ? End - At + 1 : 0;
	};
	// 候補の長さから，CRLF を除く水平空白の有無を判定する
	const auto IsSpacedSplice = [&Src](const size_t At, const size_t Len) -> bool {
		// CRLF の CR は改行の一部として除く（呼出側が非空の候補だけを渡す）
		const size_t LineBreak = At + Len - 1 - static_cast<size_t>(Src[At + Len - 2] == '\r');
		// 方言で読みが割れる候補かの返戻
		return LineBreak != At + 1;
	};
	// 読めない行継続を記録し，直後の改行だけを連結する長さの取得
	const auto ReadSpliceLen = [&SpliceLen, &IsSpacedSplice, &HasUnreadableSplice](const size_t At) -> size_t {
		const size_t Len = SpliceLen(At);
		// 行継続でない位置の返戻
		if(!Len) return 0;
		if(IsSpacedSplice(At, Len)) {
			HasUnreadableSplice = true;
			// 方言で読みが割れる継続を連結しない事の返戻
			return 0;
		}
		// 読める行継続の長さの返戻
		return Len;
	};
	// 次の論理行の始まりの取得
	const auto NextLogical = [&ReadSpliceLen, Size](size_t At) -> size_t {
		// 連続する行継続の読飛
		for(size_t Len; At < Size && (Len = ReadSpliceLen(At));) At += Len;
		// 次の字の位置の返戻
		return At;
	};
	size_t PendingCommentBytes = 0;
	std::string Output;
	Output.reserve(Size);
	HasStrayBackslash = false;
	bool IsChanged = false, IsLineHead = true, IsInDirective = false, IsHeaderName = false, IsDefine = false, IsInParams = false;
	// ディレクティブの生の本文（マクロの本体・`#pragma` の引数等，パーサーが中のコメントで本文を切る所）の始まり（Output 上，無ければ npos）
	// 現在のディレクティブの中で退避したコメントの開始インデックス
	size_t Pos = 0, LastToken = 0, DirectiveStart = 0, DirectiveTokens = 0, ParamDepth = 0, RawFrom = std::string::npos;
	size_t DirectiveEscapedStart = 0;
	// ディレクティブの中のブロックコメントの範囲（Output 上），原文で独立コメントだったか，改行を含むか，準備前の範囲
	std::vector<std::tuple<size_t, size_t, bool, bool, size_t, size_t>> Comments;
	// 原文でのブロックコメントの範囲と行頭の該当（左から右への走査で順に積む為，始まりも終わりも昇順）
	std::vector<std::tuple<size_t, size_t, bool>> SrcComments;
	// 位置の前が行の頭か（同じ行の前にブロックコメントだけが並ぶ物も行の頭とする）
	const auto IsCommentLineHead = [&Src, &SrcComments](size_t At) -> bool {
		while(At && (Src[At - 1] == ' ' || Src[At - 1] == '\t')) --At;
		// 行の頭に達した事の返戻
		if(!At || Src[At - 1] == '\n') return true;
		// 終わりが此の位置に当たるブロックコメント（鎖を毎回遡ると２乗に為る為，積む時に継いだ該当を引く）
		const std::vector<std::tuple<size_t, size_t, bool>>::const_iterator Prev = std::lower_bound(
			SrcComments.begin(),
			SrcComments.end(),
			At,
			[](const std::tuple<size_t, size_t, bool> &Range, const size_t Pos) -> bool {
				// コメントの終わりとの比較の返戻
				return std::get<1>(Range) < Pos;
			}
		);
		// 前のブロックコメントの行頭の該当の返戻
		return Prev != SrcComments.end() && std::get<1>(*Prev) == At && std::get<2>(*Prev);
	};
	// 行の末尾の該当を求めた位置（鎖の全ての位置が同じ答に為る為，１度辿ったら控えて２乗を防ぐ）
	std::unordered_map<size_t, bool> LineEndOf;
	// 位置の後が原文の行の末尾か（同じ行の後に空白・コメント・行継続の `\` だけが続く物も行の末尾とする）
	const auto IsCommentLineEnd = [&Src, Size, &ReadSpliceLen, &LineEndOf, BlockOpen, BlockClose, LineOpen](size_t At) -> bool {
		// 答の定まらない儘に辿った位置
		std::vector<size_t> Path;
		bool IsAtLineEnd = false;
		while(true) {
			// 控済の位置での打切
			if(const std::unordered_map<size_t, bool>::const_iterator Found = LineEndOf.find(At); Found != LineEndOf.end()) {
				IsAtLineEnd = Found->second;
				break;
			}
			Path.push_back(At);
			while(At < Size && (Src[At] == ' ' || Src[At] == '\t')) ++At;
			// 行の末尾（行継続を含む）と，行の末尾迄続く後の行コメントへ達した事の記録
			if(At >= Size || Src[At] == '\n' || ReadSpliceLen(At) || !Src.compare(At, LineOpen.size(), LineOpen)) {
				IsAtLineEnd = true;
				break;
			}
			// 後がブロックコメントで始まらない事の記録
			if(Src.compare(At, BlockOpen.size(), BlockOpen)) break;
			// 後のブロックコメントの終わり
			const size_t Close = Src.find(BlockClose, At + BlockOpen.size());
			// 閉じの無い事の記録
			if(Close == std::string::npos) break;
			At = Close + BlockClose.size();
		}
		// 辿った位置への答の記入
		for(const size_t Visited : Path) LineEndOf.emplace(Visited, IsAtLineEnd);
		// 行の末尾の該当の返戻
		return IsAtLineEnd;
	};
	// 行継続の読飛
	const auto SkipSplices = [&]() -> void {
		// 現在位置から次の論理トークンの位置
		const size_t Next = NextLogical(Pos);
		// 行継続の無い位置での終了
		if(Next == Pos) return;
		// エスケープしたバックスラッシュの後の行継続は，後が空行かソースの末尾の時だけ畳んだ跡のバックスラッシュが行末に立ち，原文に無い行継続が次の行をマクロの本体へ消す
		// 忠実な出力が原文其の物しか無い為，整形を見送るマーカーを立てる（後にトークンが続けばバックスラッシュは行の途中に立ち危険は無い）
		if(const size_t After = SkipHorizontal(Next); Pos && Src[Pos - 1] == '\\' && (After >= Size || Src[After] == '\n')) {
			HasStrayBackslash = true;
		}
		RecordPreparationEdit(Output.size() + PendingCommentBytes, Output.size() + PendingCommentBytes + Next - Pos, 0, Batches);
		Pos = Next;
		IsChanged = true;
		// 終了
		return;
	};
	// マーカーが要るのに選べなかったか
	bool IsUnmarkable = false;
	std::optional<LexemeRestoration> DirectiveLineComment; // マクロ本文の行コメントの確定した範囲
	// 前処理リテラルの解析用のマーカー付
	const auto MaskLiteral = [&](const size_t From) -> void {
		// リテラル中に現れるブロックコメント開始記号の探索
		for(size_t Found = Output.find(BlockOpen, From); Found != std::string::npos; Found = Output.find(BlockOpen, Found + 1)) {
			if(!Marker && !(Marker = TextEdit::AbsentControlChar(Src))) {
				IsUnmarkable = true;
				// マーカーを選べない事の終了
				return;
			}
			// 開始記号を解析不能なマーカーへ置換
			Output[Found + 1] = Marker;
			IsChanged = true;
		}
		// 終了
		return;
	};
	// 後にディレクティブのコードが続くブロックコメント（文中コメント）の，パーサーが読める形への調整（行末の物は其の儘）
	const auto EndDirective = [&]() -> void {
		// ディレクティブ状態の解除
		IsInDirective = false;
		// 原文順の退避範囲と，既存の退避コメントの境界
		std::vector<std::pair<size_t, size_t>> Removed;
		const size_t EscapedEnd = Escaped.size();
		size_t Delta = 0;
		for(const std::tuple<size_t, size_t, bool, bool, size_t, size_t> &Comment : Comments) {
			const size_t Begin = std::get<0>(Comment), End = std::get<1>(Comment);
			const bool IsStandalone = std::get<2>(Comment);
			// 行末の文中コメントは其の儘保持
			if(End > LastToken && !IsStandalone) continue;
			if(!IsStandalone) {
				const bool ShouldMask = Begin >= RawFrom;
				if(ShouldMask && !Marker && !(Marker = TextEdit::AbsentControlChar(Src))) {
					IsUnmarkable = true;
					continue;
				}
				if(ShouldMask || std::get<3>(Comment)) {
					// 翻訳段階３の空白として解析し，本文は逐語又はコメントの経路へ戻す
					Restorations.push_back(
						{ static_cast<uint32_t>(Begin - Delta), static_cast<uint32_t>(End - Delta), Output.substr(Begin, End - Begin), false }
					);
					for(size_t At = Begin; At < End; ++At) if(Output[At] == '\n') Output[At] = ' ';
					if(ShouldMask) Output[Begin + 1] = Marker;
					IsChanged = true;
				}
			} else {
				// １空白に置き換えるコメントのテキストと，累積差を引いた位置の記録
				Removed.emplace_back(Begin, End);
				Escaped.push_back(
					{
						static_cast<uint32_t>(Begin - Delta),
						Output.substr(Begin, End - Begin),
						CommentKind::OwnLine,
						std::pair<uint32_t, uint32_t>{ static_cast<uint32_t>(std::get<4>(Comment)), static_cast<uint32_t>(std::get<5>(Comment)) }
					}
				);
				Delta += End - Begin - 1;
				IsChanged = true;
			}
		}
		// 最後の行コメントも，先行する退避範囲を除いた位置へ移す
		if(DirectiveLineComment) {
			DirectiveLineComment->Start -= static_cast<uint32_t>(Delta);
			DirectiveLineComment->End -= static_cast<uint32_t>(Delta);
			Restorations.push_back(std::move(*DirectiveLineComment));
			DirectiveLineComment.reset();
		}
		// 先に退避した行コメントの位置も，昇順の範囲を１度だけ辿って補正
		size_t Range = 0;
		Delta = 0;
		for(size_t Idx = DirectiveEscapedStart; Idx < EscapedEnd; ++Idx) {
			while(Range < Removed.size() && Removed[Range].first < Escaped[Idx].Pos) {
				Delta += Removed[Range].second - Removed[Range].first - 1;
				++Range;
			}
			Escaped[Idx].Pos -= static_cast<uint32_t>(Delta);
		}
		// 対応位置の編集と退避順は従来通り後ろから前へ
		for(size_t Idx = Removed.size(); Idx--;) RecordPreparationEdit(Removed[Idx].first, Removed[Idx].second, 1, Batches);
		std::reverse(Escaped.begin() + EscapedEnd, Escaped.end());
		// 残す断片を前へ詰め，末尾を１度だけ移動
		if(!Removed.empty()) {
			size_t Read = Removed.front().first, Write = Read;
			for(const std::pair<size_t, size_t> &Range : Removed) {
				const size_t Len = Range.first - Read;
				std::memmove(Output.data() + Write, Output.data() + Read, Len);
				Write += Len;
				Output[Write++] = ' ';
				Read = Range.second;
			}
			const size_t Len = Output.size() - Read;
			std::memmove(Output.data() + Write, Output.data() + Read, Len);
			Output.resize(Write + Len);
		}
		Comments.clear();
		// 行末の水平空白の除去（本体が空白で終わるディレクティブは tree-sitter-c が次の行を本体へ取り込む為，畳んだ跡とコメントの跡を詰める）
		// 水平空白しか無い時の `npos` の折返で出力の全体を捨てない様に，見付かった時だけ詰める
		if(const size_t Last = Output.find_last_not_of(" \t"); Last != std::string::npos && Last + 1 < Output.size()) {
			RecordPreparationEdit(Last + 1, Output.size(), 0, Batches);
			Output.resize(Last + 1);
			IsChanged = true;
		}
		// コメントを退避する前の論理トークンから，ディレクティブ名・本文の無いディレクティブを確定する
		if(DirectiveTokens == 1) {
			// パーサーが ERROR へ纏める裸の # を，１つのディレクティブとして読めるトークンへ可逆に置き換える
			const size_t Start = LastToken - 1;
			const std::string_view Placeholder = Rules.NullDirectivePlaceholder;
			const uint32_t Added = static_cast<uint32_t>(Placeholder.size() - 1);
			for(size_t Idx = DirectiveEscapedStart; Idx < Escaped.size(); ++Idx) Escaped[Idx].Pos += Added;
			RecordPreparationEdit(Start, Start + 1, Placeholder.size(), Batches);
			Output.replace(Start, 1, Placeholder);
			Restorations.push_back({ static_cast<uint32_t>(Start), static_cast<uint32_t>(Start + Placeholder.size()), "#", false });
			if(Pos == Size) {
				// ソース末尾でも空のディレクティブの論理行終端をパーサーへ伝える
				RecordPreparationEdit(Output.size(), Output.size(), 1, Batches);
				Output += '\n';
			}
			IsChanged = true;
		}
		// コメント・リテラル内の改行では終えなかった見出の範囲の共有
		DirectiveLines.emplace_back(static_cast<uint32_t>(DirectiveStart), static_cast<uint32_t>(Output.size()));
		// 終了
		return;
	};
	// 論理トークン単位の原文走査
	while(true) {
		SkipSplices();
		// 三連字又は空白を挟む行継続を読めない入力の見送の返戻
		if(HasUnreadableSplice) return std::nullopt;
		if(Pos >= Size) break;
		// 現在の論理字の取得
		const unsigned char Char = Src[Pos];
		// 行継続を飛ばした後に残る裸のバックスラッシュは，後のトークンが次の行へ移ると行末に立ち，原文に無い行継続に為る
		// 元々 C として不正なテキストで，忠実な出力が原文其の物しか無い為，整形を見送るマーカーを立てる
		// 識別子やリテラルへ書けるユニバーサル文字名 (`\u00E9` / `\U0001F600`) は字が続く為に行末へ立たず，除く
		// ディレクティブは１論理行の儘折らない為，後に同じ行のトークンが続く物は行末に立たない（後が空白とコメントだけの物はコメントを分離すると行末に立つ）
		if(
			Char == '\\' && (Pos + 2 > Size || Src[Pos + 1] != 'u' && Src[Pos + 1] != 'U') && (!IsInDirective || HasLineEndAfter(Pos + 1))
		) HasStrayBackslash = true;
		if(Char == '\n') {
			// 論理行終端の確定
			if(IsInDirective) EndDirective();
			Output += Char;
			++Pos;
			IsLineHead = true;
			HasAtHead = false;
			continue;
		}
		if(Char == ' ' || Char == '\t' || Char == '\f' || Char == '\v' || Char == '\r') {
			// 同じ論理行の水平空白を１つの連続領域として複写
			const size_t End = SkipHorizontal(Pos + 1);
			Output.append(Src, Pos, End - Pos);
			Pos = End;
			continue;
		}
		// トークン種別判定の準備
		const bool IsAtLineHead = IsLineHead;
		IsLineHead = false;
		// コメント開始記号候補の次の論理字位置取得
		const size_t Second = Char == '/' || Char == '.' ? NextLogical(Pos + 1) : Size;
		// ブロックコメントの抽出
		if(Second < Size && Src[Second] == '*' && Char == '/') {
			// ブロックコメントの出力開始位置と原文の開始位置の記録
			const size_t CommentStart = Output.size(), Start = Pos;
			if(Second != Pos + 1) {
				RecordPreparationEdit(Output.size() + 1, Output.size() + Second - Pos, 0, Batches);
				IsChanged = true;
			}
			Output += Core::CommentRepository::GetOpenerOf(CommentNotation::Block);
			Pos = Second + 1;
			// コメント閉鎖状態の初期化
			bool IsClosed = false, HasLineBreak = false;
			// ブロックコメント終端迄の逐語複写
			while(Pos < Size && !IsClosed) {
				// 本文は逐語コピーし，閉記号の間だけ行継続を読む
				if(Src[Pos] == '*') {
					size_t Slash = Pos + 1;
					bool HasSpacedSplice = false;
					for(size_t Len = SpliceLen(Slash); Len; Len = SpliceLen(Slash)) {
						HasSpacedSplice = HasSpacedSplice || IsSpacedSplice(Slash, Len);
						Slash += Len;
					}
					if(Slash < Size && Src[Slash] == '/') {
						// 閉記号を繋ぐ継続だけは，方言による読みの違いを許さない
						if(HasSpacedSplice) return std::nullopt;
						if(Slash != Pos + 1) {
							RecordPreparationEdit(Output.size() + 1, Output.size() + Slash - Pos, 0, Batches);
							IsChanged = true;
						}
						Output += Core::CommentRepository::GetCloserOf(CommentNotation::Block);
						Pos = Slash + 1;
						IsClosed = true;
						continue;
					}
				}
				HasLineBreak = HasLineBreak || Src[Pos] == '\n';
				Output += Src[Pos++];
			}
			// 原文でのブロックコメントの範囲と行頭の該当の記録（行の頭の判定が直前のコメントから継いだ該当を引く）
			if(IsClosed) {
				SrcComments.emplace_back(Start, Pos, IsCommentLineHead(Start));
				if(HasLinkage && !IsInDirective) {
					SourceComments.push_back({ static_cast<uint32_t>(CommentStart), static_cast<uint32_t>(Start), static_cast<uint32_t>(Pos) });
				}
			}
			// ディレクティブ内で閉じたコメント範囲の記録
			if(IsInDirective && IsClosed) {
				Comments.emplace_back(CommentStart, Output.size(), IsCommentLineHead(Start) && IsCommentLineEnd(Pos), HasLineBreak, Start, Pos);
			}
			// 行頭コメント後の # のディレクティブ開始扱い
			IsLineHead = (IsAtLineHead || HasLineBreak) && !IsInDirective;
			continue;
		}
		// 行コメント開始の検出
		if(Second < Size && Src[Second] == '/' && Char == '/') {
			const size_t CommentStart = Output.size(), Start = Pos;
			// ディレクティブの継続行の頭の行コメント（行の末尾迄続く独立コメント）は，ディレクティブが１行に繋がって独立で居れない為にパーサーへ渡さずに退避する（置けるかはコメント経路が決める，整形仕様 7.6）
			const bool IsEscaped = IsInDirective && IsCommentLineHead(Pos);
			// 行コメント：行継続の除去と次の行の本文への結合
			if(Second != Pos + 1 || IsEscaped) IsChanged = true;
			RecordPreparationEdit(Output.size() + 1, Output.size() + Second - Pos, 0, Batches);
			std::string Comment(LineOpen);
			Pos = Second + 1;
			PendingCommentBytes = Comment.size();
			for(SkipSplices(); Pos < Size && Src[Pos] != '\n'; SkipSplices()) {
				const size_t End = std::min(Src.find_first_of("\\\n", Pos), Size);
				if(End == Pos) Comment += Src[Pos++];
				else {
					Comment.append(Src, Pos, End - Pos);
					Pos = End;
				}
				PendingCommentBytes = Comment.size();
			}
			PendingCommentBytes = 0;
			if(HasLinkage && !IsInDirective) {
				SourceComments.push_back({ static_cast<uint32_t>(CommentStart), static_cast<uint32_t>(Start), static_cast<uint32_t>(Pos) });
			}
			// １字の行継続は上の走査で読み，原文の行末に残る２字以上の継続記法はパーサーへ渡さない
			const std::string_view Body = std::string_view(Src).substr(Second + 1, Pos - Second - 1);
			const std::string_view Tail = Body.substr(0, Body.find_last_not_of(" \t\f\v\r") + 1);
			HasUnreadableSplice = HasUnreadableSplice || std::ranges::any_of(
				LineSplices,
				[Tail](const std::string_view Splice) -> bool {
					// 既存の読取が扱わない行末の継続記法かの返戻
					return Splice.size() > 1 && Tail.ends_with(Splice);
				}
			);
			if(IsInDirective && !IsEscaped && Output.size() >= RawFrom) {
				// 指示の終端で除く水平空白は，コメントの復元にも含めない
				const size_t CommentEnd = Comment.find_last_not_of(" \t") + 1;
				DirectiveLineComment = LexemeRestoration{
					static_cast<uint32_t>(Output.size()),
					static_cast<uint32_t>(Output.size() + CommentEnd),
					Comment.substr(0, CommentEnd),
					false
				};
			}
			if(IsEscaped) RecordPreparationEdit(Output.size(), Output.size() + Comment.size(), 0, Batches);
			if(IsEscaped) {
				Escaped.push_back(
					{
						static_cast<uint32_t>(Output.size()),
						std::move(Comment),
						CommentKind::OwnLine,
						std::pair<uint32_t, uint32_t>{ static_cast<uint32_t>(Start), static_cast<uint32_t>(Pos) }
					}
				);
			} else Output += Comment;
			continue;
		}
		// 通常トークンの出力開始位置記録
		const bool IsAtKeyword = HasAtHead;
		HasAtHead = IsAtLineHead && Char == '@';
		const size_t TokenStart = Output.size();
		// 論理行の先頭でプリプロセッサディレクティブの状態を初期化
		if(IsAtLineHead && Char == '#') {
			IsInDirective = true;
			DirectiveStart = TokenStart;
			DirectiveTokens = ParamDepth = 0;
			DirectiveEscapedStart = Escaped.size();
			IsHeaderName = IsDefine = IsInParams = false;
			RawFrom = std::string::npos;
		}
		// 引用形式に応じたリテラル又はヘッダ名の処理
		if(Char == '"' || Char == '\'' || IsHeaderName && Char == '<') {
			// リテラルとヘッダ名からの行継続除去と未終端時の行末打切
			const unsigned char Close = Char == '<' ? '>' : Char;
			Output += Char;
			++Pos;
			// 引用符迄のリテラル逐語複写
			for(SkipSplices(); Pos < Size && Src[Pos] != '\n'; SkipSplices()) {
				const unsigned char Inner = Src[Pos++];
				Output += Inner;
				if(Inner == Close) break;
				if(Inner != '\\' || Close == '>') continue;
				SkipSplices();
				if(Pos < Size && Src[Pos] != '\n') Output += Src[Pos++];
			}
			// ディレクティブ内リテラルに含まれるコメントマーカーの無効化
			if(IsInDirective) MaskLiteral(TokenStart);
		} else if(IsWordChar(Char)) {
			// 識別子トークンの収集
			for(SkipSplices(); Pos < Size && IsWordChar(Src[Pos]); SkipSplices()) {
				const size_t Begin = Pos++;
				while(Pos < Size && IsWordChar(Src[Pos])) ++Pos;
				Output.append(Src, Begin, Pos - Begin);
			}
			const std::string_view Identifier(Output.data() + TokenStart, Output.size() - TokenStart);
			// 行継続を読んだトークンで，後の連結指定のコメント範囲の収集を始める
			HasLinkage = HasLinkage || !IsInDirective && Identifier == Rules.LinkageHead;
			// コメント・空白を除いた論理行頭の方言のキーワードの照合
			if(IsAtKeyword || IsInDirective && DirectiveTokens == 1) {
				const char Prefix = IsAtKeyword ? '@' : '#';
				HasForeignDialect = HasForeignDialect || std::ranges::any_of(
					Rules.ForeignDialectHeads,
					[Prefix, Identifier](const std::string_view Head) -> bool {
						// 記号と完全な名前が一致する事の返戻
						return !Head.empty() && Head.front() == Prefix && Head.substr(1) == Identifier;
					}
				);
			}
			// 生文字列（`R"delim(…)delim"`）：翻訳段階２が取り消される為，中身の逐語複写
			if(
				const bool IsRawPrefix = std::ranges::find(Rules.RawStrPrefixes, Identifier) != Rules.RawStrPrefixes.end();
				IsRawPrefix && Pos < Size && Src[Pos] == '"'
			) {
				if(
					const size_t Open = Src.find_first_of("( )\\\t\v\f\n", Pos + 1);
					Open != std::string::npos && Src[Open] == '(' && Open - Pos < 18
				) if(const size_t End = Src.find(")" + Src.substr(Pos + 1, Open - Pos - 1) + "\"", Open); End != std::string::npos) {
					// 生文字列全体の逐語複写
					const size_t Stop = End + Open - Pos + 1;
					Output.append(Src, Pos, Stop - Pos);
					Pos = Stop;
					if(IsInDirective) MaskLiteral(TokenStart);
				}
			}
			// ディレクティブ名から次の山括弧をヘッダ名として読むかと，引数を解析しないディレクティブの名前の後の生の本文の始まりの決定
			if(IsInDirective && DirectiveTokens == 1) {
				IsHeaderName = std::ranges::find(Rules.HeaderNameDirectives, Identifier) != Rules.HeaderNameDirectives.end();
				IsDefine = Identifier == Rules.DefDirective;
				if(!IsDefine && std::ranges::find(Rules.ParsedDirectives, Identifier) == Rules.ParsedDirectives.end()) RawFrom = Output.size();
			} else if(IsInDirective && IsDefine && DirectiveTokens == 2) {
				// マクロ名の直後の `(` は仮引数（閉じの後が生の本体），其れ以外はマクロ名の後が生の本体
				IsInParams = Pos < Size && Src[Pos] == '(';
				if(!IsInParams) RawFrom = Output.size();
			}
		} else if(std::isdigit(Char) || Char == '.' && Second < Size && std::isdigit(static_cast<unsigned char>(Src[Second]))) {
			// 数値（前処理数）：桁区切の `'` と指数の符号の包含
			Output += Char;
			++Pos;
			// 前処理数の構成字走査
			for(SkipSplices(); Pos < Size; SkipSplices()) {
				const unsigned char Digit = Src[Pos];
				if(Digit == '\'') {
					// 桁区切後の論理字確認
					const size_t After = NextLogical(Pos + 1);
					if(After == Size || !IsWordChar(Src[After])) break;
					RecordPreparationEdit(Output.size() + 1, Output.size() + After - Pos, 0, Batches);
					Output += Digit;
					IsChanged = IsChanged || After != Pos + 1;
					Pos = After;
					continue;
				}
				if(!IsWordChar(Digit) && Digit != '.') break;
				Output += Digit;
				++Pos;
				if(Digit != 'e' && Digit != 'E' && Digit != 'p' && Digit != 'P') continue;
				// 指数部の符号収集
				SkipSplices();
				if(Pos < Size && (Src[Pos] == '+' || Src[Pos] == '-')) Output += Src[Pos++];
			}
		} else {
			// 単一記号トークンの複写
			Output += Char;
			++Pos;
			// マクロの仮引数の括弧の対の追跡（閉じの後が生の本体）
			if(IsInParams && Char == '(') ++ParamDepth;
			else if(IsInParams && Char == ')' && !--ParamDepth) {
				IsInParams = false;
				RawFrom = Output.size();
			}
		}
		if(!IsInDirective) continue;
		// `#` 後のディレクティブ名と次トークン迄のヘッダ名候補
		if(DirectiveTokens++ > 1) IsHeaderName = false;
		LastToken = Output.size();
	}
	// 末尾の改行が無いディレクティブも確定
	if(IsInDirective) EndDirective();
	// 連結指定保護の適用
	const size_t GuardBatchStart = Batches.size();
	const std::optional<bool> IsGuarded =
	MaskLinkageGuards(Rules, Src, Output, SourceComments, Marker, Escaped, Batches, Restorations);
	// マーカーを選べず誤読を修復出来ない事の返戻
	if(!IsGuarded || IsUnmarkable) return std::nullopt;
	// 前の連結指定の置換に依る見出の範囲の移動（此の置換は見出の外のコードだけを扱う）
	size_t EditIdx = Batches.size();
	int64_t Offset = 0;
	for(std::pair<uint32_t, uint32_t> &Line : DirectiveLines) {
		while(EditIdx > GuardBatchStart && Batches[EditIdx - 1].End < Line.first) {
			const AppliedEdit &Edit = Batches[--EditIdx];
			Offset += static_cast<int64_t>(Edit.NewEnd) - Edit.End;
		}
		Line.first = static_cast<uint32_t>(Line.first + Offset);
		Line.second = static_cast<uint32_t>(Line.second + Offset);
	}
	// 整理後の原文（変更が無ければ空）の返戻
	return *IsGuarded || IsChanged ? std::move(Output) : std::string();
}

/**
 * 引用とコメントを保護して値の補間の終端を読む関数
 * 計算量：補間の長さ B に対し O(B)
 * @param Reading 対象の文法の読取規則
 * @param Text テキスト
 * @param Pos 補間の開始位置
 * @return 閉じの直後の位置（閉じが無ければ npos）
 */
size_t ParseAdapter::FindValueInterpolationEnd(const ReadingRules &Reading, const std::string_view Text, size_t Pos) {
	const std::string_view Open = Reading.ValueInterpolationOpen, Close = Reading.ValueInterpolationClose;
	const std::string_view BlockOpen = Core::CommentRepository::GetOpenerOf(CommentNotation::Block);
	const std::string_view BlockClose = Core::CommentRepository::GetCloserOf(CommentNotation::Block);
	const std::string_view LineOpen = Core::CommentRepository::GetOpenerOf(CommentNotation::SlashLine);
	size_t Depth = 1;
	Pos += Open.size();
	while(Pos < Text.size()) {
		if(Text[Pos] == '\\') Pos = std::min(Pos + 2, Text.size());
		else if(Text[Pos] == '\'' || Text[Pos] == '"') {
			const char Quote = Text[Pos++];
			while(Pos < Text.size() && Text[Pos] != Quote) Pos = std::min(Pos + 1 + static_cast<size_t>(Text[Pos] == '\\'), Text.size());
			if(Pos < Text.size()) ++Pos;
		} else if(Text.substr(Pos).starts_with(BlockOpen)) {
			const size_t End = Text.find(BlockClose, Pos + BlockOpen.size());
			Pos = End == std::string_view::npos ? Text.size() : End + BlockClose.size();
		} else if(Text.substr(Pos).starts_with(LineOpen)) Pos = std::min(Text.find('\n', Pos + LineOpen.size()), Text.size());
		else if(Text.substr(Pos).starts_with(Close)) {
			Pos += Close.size();
			// 対応する閉じの後の位置の返戻
			if(!--Depth) return Pos;
		} else {
			if(Text[Pos] == Open.back()) ++Depth;
			++Pos;
		}
	}
	// 閉じの無い補間の返戻
	return std::string_view::npos;
}

/**
 * 引用符の無い値の関数の本文の終端の取得関数
 * エスケープ・補間の内部の閉括弧と，本文の斜線を外側の境界と読まない
 * 計算量：本文長 B と固定の開始記号長 H に対し O(B + H)
 * @param Reading 対象の文法の読取規則
 * @param Text テキスト
 * @param Pos 開始記号の候補の位置
 * @param Interpolations 補間の範囲の格納先（要らなければ無い，未閉じの終端は `std::string_view::npos`）
 * @return 閉じの後の位置（引用符の無い値の関数でなければ無い）
 * @throws std::bad_alloc 補間の範囲のメモリを確保出来ない場合
 * @throws std::length_error 補間の範囲の要素数上限を超える場合
 */
std::optional<size_t> ParseAdapter::FindUnquotedUrlEnd(
	const ReadingRules &Reading,
	const std::string_view Text,
	const size_t Pos,
	std::vector<std::pair<size_t, size_t>> *const Interpolations
) {
	const std::string_view Head = Reading.UrlFunctionHead;
	// 引用符の無い値の開始記号でない位置の返戻
	if(
		Head.empty() || Pos >= Text.size() || (static_cast<unsigned char>(Text[Pos]) | 0X20) != Head.front() ||
		Pos + Head.size() > Text.size() || Pos && (IsIdentifierChar(Text[Pos - 1]) || Text[Pos - 1] == '-') || !std::equal(
			Text.begin() + Pos + 1,
			Text.begin() + Pos + Head.size(),
			Head.begin() + 1,
			[](const unsigned char Byte, const char Expected) -> bool {
				// 名前の大小文字を問わない一致の返戻
				return std::tolower(Byte) == Expected;
			}
		)
	) return std::nullopt;
	size_t End = Text.find_first_not_of(" \t\r\n\f", Pos + Head.size());
	// 引用符の無い本文を持たない関数の返戻
	if(End == std::string_view::npos || Text[End] == '"' || Text[End] == '\'') return std::nullopt;
	while(End < Text.size() && Text[End] != ')') {
		if(!Reading.ValueInterpolationOpen.empty() && Text.substr(End).starts_with(Reading.ValueInterpolationOpen)) {
			const size_t Next = FindValueInterpolationEnd(Reading, Text, End);
			if(Interpolations) Interpolations->emplace_back(End, Next);
			End = std::min(Next, Text.size());
		} else End = std::min(End + 1 + static_cast<size_t>(Text[End] == '\\'), Text.size());
	}
	// 保護する本文の終端の返戻
	return End < Text.size() ? End + 1 : End;
}

/**
 * CSS 系の行コメントの行境界と文字列のバックスラッシュを解析前に準備する関数
 * 行境界は LF に揃え，エスケープしたバックスラッシュの２字目は原文に無い制御文字へ置き換えて出力時に戻す
 * 引用文字列・ブロックコメント・引用符の無い url の本文の行境界は変えない
 * 計算量：ソースの長さ L，制御文字の選択の費用 Q に対し O(L + Q)
 * @param Profile 対象の文法
 * @param Src 原文（改行は LF へ正規化済）
 * @param Marker 置き換えた制御文字の格納先（置き換えなければ '\0'）
 * @return 準備した原文（変更が無ければ空，マーカーを選べなければ無い）
 * @throws std::bad_alloc 原文のコピーを確保出来ない場合
 * @throws std::length_error 原文が文字列の最大長を超える場合
 */
std::optional<std::string> ParseAdapter::PrepareCssSrc(
	const GrammarProfile &Profile,
	const std::string &Src,
	unsigned char &Marker
) {
	Marker = '\0';
	const ReadingRules &Reading = Profile.GetReading();
	const Core::NotationRules &Notation = Profile.GetRules().Notation;
	const std::string_view BlockOpen = Core::CommentRepository::GetOpenerOf(CommentNotation::Block);
	const std::string_view BlockClose = Core::CommentRepository::GetCloserOf(CommentNotation::Block);
	const std::string_view LineOpen = Core::CommentRepository::GetOpenerOf(CommentNotation::SlashLine);
	// 解析前の準備を要するテキストの無い原文の返戻
	if(
		Src.find("\\\\") == std::string::npos &&
		!(Reading.ShouldNormalizeLineCommentBreaks && Core::CommentRepository::HasLineCommentBreak(Notation, Src))
	) return std::string();
	std::string Output;
	const size_t Size = Src.size();
	for(size_t Pos = 0; Pos < Size;) {
		const unsigned char Char = Src[Pos];
		// ブロックコメントの本文の読飛
		if(!Src.compare(Pos, BlockOpen.size(), BlockOpen)) {
			const size_t End = Src.find(BlockClose, Pos + BlockOpen.size());
			Pos = End == std::string::npos ? Size : End + BlockClose.size();
			continue;
		}
		// 行コメントの最初の行境界を揃え，其の後はコードとして読む
		if(Notation.IsAvailable(CommentNotation::SlashLine) && !Src.compare(Pos, LineOpen.size(), LineOpen)) {
			Pos += LineOpen.size();
			while(Pos < Size && Src[Pos] != '\n') {
				const bool IsBreak = Reading.ShouldNormalizeLineCommentBreaks && std::ranges::any_of(
					Notation.LineCommentBreaks,
					[&Src, Pos](const std::string_view Break) -> bool {
						return Break.size() == 1 && Src[Pos] == Break.front();
					}
				);
				if(IsBreak) {
					if(Output.empty()) Output = Src;
					Output[Pos++] = '\n';
					break;
				}
				++Pos;
			}
			continue;
		}
		// 引用符の無い値の本文の斜線をコメントと読まない
		if(const std::optional<size_t> End = FindUnquotedUrlEnd(Reading, Src, Pos)) {
			Pos = *End;
			continue;
		}
		if(Char != '"' && Char != '\'') {
			Pos = std::min(Pos + 1 + static_cast<size_t>(Char == '\\'), Size);
			continue;
		}
		// 引用文字列の補間とエスケープの読取
		++Pos;
		while(Pos < Size && Src[Pos] != Char && Src[Pos] != '\n') {
			if(!Reading.ValueInterpolationOpen.empty() && std::string_view(Src).substr(Pos).starts_with(Reading.ValueInterpolationOpen)) {
				Pos = std::min(FindValueInterpolationEnd(Reading, Src, Pos), Size);
				continue;
			}
			if(Src[Pos] != '\\' || Pos + 2 > Size) {
				++Pos;
				continue;
			}
			if(Reading.ShouldMaskEscapedBackslashes && Src[Pos + 1] == '\\') {
				// マーカーを選べず誤読を修復出来ない事の返戻
				if(!Marker && !(Marker = TextEdit::AbsentControlChar(Src))) return std::nullopt;
				if(Output.empty()) Output = Src;
				Output[Pos + 1] = Marker;
			}
			Pos += 2;
		}
		if(Pos < Size) ++Pos;
	}
	// 準備した原文の返戻
	return Output;
}

/**
 * トークン化前に展開する Unicode のエスケープの値の読取関数（Java のバックスラッシュと１個以上の `u` に続く１６進４桁）
 * 計算量：連続する u の数 U に対し O(U + 1)（１６進桁の走査は最大４回）
 * @param Src テキスト
 * @param Pos バックスラッシュの位置（読んだエスケープの終わりへ進める）
 * @return エスケープの UTF-16 の値（バックスラッシュと `u` で始まらなければ無い，桁不足なら範囲外の値）
 */
std::optional<char32_t> ParseAdapter::ReadUnicodeEscape(const std::string_view Src, size_t &Pos) {
	// エスケープで始まらないテキストの返戻
	if(Pos + 2 > Src.size() || Src[Pos] != '\\' || Src[Pos + 1] != 'u') return std::nullopt;
	// 重ねた u の読飛
	size_t Digits = Pos + 1;
	while(Digits < Src.size() && Src[Digits] == 'u') ++Digits;
	// 桁不足を範囲外の値で示す事の返戻
	if(Digits + 4 > Src.size()) return 0X110000;
	// １６進４桁の値
	char32_t Val = 0;
	// 各１６進桁の走査
	for(size_t Digit = 0; Digit < 4; ++Digit) {
		// 現在の桁
		const unsigned char Char = Src[Digits + Digit];
		const int Hex = Char >= '0' && Char <= '9' ?
		Char - '0' :
		Char >= 'a' && Char <= 'f' ? Char + 10 - 'a' : Char >= 'A' && Char <= 'F' ? Char + 10 - 'A' : -1;
		// 桁不足の返戻
		if(Hex < 0) return 0X110000;
		// 次の桁の取込
		Val = Val << 4 | static_cast<char32_t>(Hex);
	}
	// エスケープ終端への移動
	Pos = Digits + 4;
	// 読んだ値の返戻
	return Val;
}

/**
 * テキストの段の解析用の入力の準備関数（整形仕様 4.2，解析の前に原文のテキストで行う可逆な保護）
 * C / C++ は行継続とディレクティブ，CSS は文字列内のバックスラッシュ，Ruby は誤読する行頭の式を保護する
 * 計算量：原文長 B，挿入数 K，準備の呼出の総費用 Q に対し O(B + Q + K log(K + 1))
 * @param Profile 対象の文法
 * @param Src 原文（改行は LF へ正規化済）
 * @param HasDataBoundary 入力で確定したデータ境界を末尾に持つコードか
 * @param Marker 置き換えた制御文字の格納先（置き換えなければ '\0'）
 * @param HasStrayBackslash 後のトークンが次の行へ移ると行末に立ち，原文に無い行継続に為る裸のバックスラッシュを含むかの格納先 (C / C++)
 * @param HasForeignDialect 共有ヘッダの対象外の方言の検出結果
 * @param Escaped パーサーへ渡さずに退避したコメントの格納先 (C / C++)
 * @param Batches 準備前へ戻す位置の対応（適用順）
 * @param Restorations 空のディレクティブとコメント内の改行を解析用のトークンから戻す位置とテキストの対応
 * @param DirectiveLines 論理トークン走査で確定した前処理の見出の範囲（準備後の位置）の格納先
 * @param Reason 試行解析の期限切又は初期化失敗の格納先
 * @return 準備した原文（変更が無ければ空，マーカーを選べないか試行解析を完了出来ない場合は無い）
 * @throws std::bad_alloc 本文又は位置の対応を確保出来ない場合
 * @throws std::length_error 本文が文字列の最大長を超える場合
 */
std::optional<std::string> ParseAdapter::PrepareSrc(
	const GrammarProfile &Profile,
	const std::string &Src,
	const bool HasDataBoundary,
	unsigned char &Marker,
	bool &HasStrayBackslash,
	bool &HasForeignDialect,
	std::vector<EscapedComment> &Escaped,
	std::vector<AppliedEdit> &Batches,
	std::vector<LexemeRestoration> &Restorations,
	std::vector<std::pair<uint32_t, uint32_t>> &DirectiveLines,
	FailureReason &Reason
) {
	// テキストの段の解析用の入力の準備関数（整形仕様 4.2，解析の前に原文のテキストで行う可逆な保護）
	Marker = '\0';
	HasStrayBackslash = false;
	HasForeignDialect = false;
	const Core::SyntaxRules &Rules = Profile.GetRules();
	const ReadingRules &Reading = Profile.GetReading();
	if(Rules.HasLineDirectives) {
		// C / C++ の準備した原文の返戻
		return PrepareCSrc(
			Reading,
			Src,
			Rules.Notation.LineSplices,
			Marker,
			HasStrayBackslash,
			HasForeignDialect,
			Escaped,
			Batches,
			Restorations,
			DirectiveLines
		);
	}
	if(!Reading.TrailingDataMarker.empty()) {
		std::optional<ParsedTree> Trial;
		std::vector<std::pair<uint32_t, uint32_t>> Parens;
		ReadTrailingDataMarker(Profile, Src, HasDataBoundary, Trial, Parens, Reason);
		// 試行解析を完了出来なかった場合の返戻
		if(Reason != FailureReason::None) return std::nullopt;
		// 括弧はトークンの間にだけ挟み，原文の名前・リテラル・コメントは変更しない
		std::vector<std::pair<uint32_t, char>> Insertions;
		for(const auto &[Begin, End] : Parens) {
			Insertions.emplace_back(Begin, '(');
			Insertions.emplace_back(End, ')');
		}
		std::ranges::sort(Insertions);
		// 誤読される通常の式が無い場合の返戻
		if(Insertions.empty()) return std::string();
		std::string Prepared;
		Prepared.reserve(Src.size() + Insertions.size());
		size_t Copied = 0;
		for(const auto &[Pos, Token] : Insertions) {
			Prepared.append(Src, Copied, Pos - Copied);
			const size_t At = Prepared.size();
			Prepared += Token;
			RecordPreparationEdit(At, At, 1, Batches);
			Copied = Pos;
		}
		Prepared.append(Src, Copied, std::string::npos);
		// トークンの位置を保護した原文の返戻
		return Prepared;
	}
	// CSS の準備した原文か，置き換えない原文の返戻
	std::optional<std::string> Prepared = Reading.ShouldMaskEscapedBackslashes || Reading.ShouldNormalizeLineCommentBreaks ?
	PrepareCssSrc(Profile, Src, Marker) :
	std::optional<std::string>(std::string());
	// 解析前の準備が完了出来ない場合の返戻
	if(!Prepared) return std::nullopt;
	if(!Reading.ValueInterpolationOpen.empty()) {
		std::string Protected = PrepareValueInterpolations(Profile, Prepared->empty() ? Src : *Prepared, Batches, Restorations);
		if(!Protected.empty()) *Prepared = std::move(Protected);
	}
	// 初回解析へ渡す本文の返戻
	return Prepared;
}

/**
 * 開いた要素の名前をパーサーが失い閉タグを読み違えたかの判定関数（整形仕様 9.3 の tree-sitter-html の制限）
 * tree-sitter-html のスキャナーは開いた要素の並びを１０２４バイトのシリアライズ領域へ書き，収まらない要素から後の名前を失う
 * 並びは要素数のフィールド４バイトの後に，既知の名前の要素を種別の１バイト，其の他の要素を長さと名前（２５５字迄）で持つ
 * 名前を失った要素を閉じる閉タグは対応を失い，誤った閉タグ又はエラー領域として解析結果に残る
 * 計算量：ノード数 V に対し O(V)
 * @param Src 解析済のソース (HTML)
 * @return 閉タグを読み違えた木で名前を失った最初の要素の位置（無ければ空）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::optional<uint32_t> ParseAdapter::LostHtmlElementNameAt(const ParsedTree &Src) {
	// スキャナーのシリアライズ領域の寸法と要素数のフィールドの寸法
	static constexpr uint32_t BufSize = 0X400, Header = 4;
	std::optional<uint32_t> LostAt;
	bool IsMisread = ts_node_has_error(Src.GetRoot());
	// ノードと，其のノードを囲む開いた要素の並びのシリアライズの大きさ
	std::vector<std::pair<TSNode, uint32_t>> Stack { { Src.GetRoot(), Header } };
	while(!Stack.empty() && !(LostAt && IsMisread)) {
		const std::pair<TSNode, uint32_t> Cur = Stack.back();
		const TSNode Node = Cur.first;
		const uint32_t Serialized = Cur.second;
		Stack.pop_back();
		IsMisread = IsMisread || Src.Is(Node, Core::Role::ErroneousEndTag);
		// 要素の子を囲む並びの大きさ（要素の名前の分を加える）
		uint32_t Inner = Serialized;
		if(Src.Is(Node, Core::Role::MarkupElementNode)) {
			const TSNode Start = ts_node_named_child(Node, 0);
			const TSNode Name = ts_node_is_null(Start) ? Start : Src.GetFirstNamedChild(Start, Core::Role::TagNameNode);
			HtmlNameBuf Buf;
			const std::string_view Lower = ts_node_is_null(Name) ? std::string_view() : HtmlLowerName(Src.GetView(Name), Buf);
			const uint32_t Bytes = Src.Is(Node, Core::Role::EmbeddedCodeElement) ||
			GrammarProfile::GetMarkupTraits(GrammarProfile::MarkupContext::Html, Lower) & GrammarProfile::Scanned ?
			1 :
			std::min<uint32_t>(static_cast<uint32_t>(Lower.size()), 0XFF) + 2;
			if(!LostAt && Serialized + Bytes >= BufSize) LostAt = Src.OffsetInInput(Src.GetStart(Node));
			Inner = Serialized + Bytes;
		}
		// 要素の子を原文順に収集
		std::vector<TSNode> Children;
		HeldCursor Cursor(Node);
		if(ts_tree_cursor_goto_first_child(&Cursor)) {
			do if(const TSNode Child = ts_tree_cursor_current_node(&Cursor); ts_node_is_named(Child)) Children.push_back(Child);
			while(ts_tree_cursor_goto_next_sibling(&Cursor));
		}
		for(size_t Idx = Children.size(); Idx--;) Stack.emplace_back(Children[Idx], Inner);
	}
	// 名前を失った要素が閉タグを読み違えたかの返戻
	return IsMisread ? LostAt : std::nullopt;
}

/**
 * 古典スクリプトとして読むソースかの判定関数（整形仕様 2.1, 10.4）
 * 拡張子で目標が定まらないソースは，最上位にインポート・公開の文を持たなければ古典スクリプトとする
 * 計算量：最上位の子数 C に対し O(C)
 * @param Src 解析結果
 * @return 古典スクリプトが HTML 風コメントを認める文法で，古典スクリプトとして読むソースなら true
 */
bool ParseAdapter::IsClassicScript(const ParsedTree &Src) {
	// 拡張子で定まる読取目標の取得
	const Lang::SrcGoal Goal = Src.GetReading().SrcGoal;
	if(
		const bool CanAcceptMarkupComments = Src.GetReading().CanScriptAcceptMarkupComments;
		!CanAcceptMarkupComments || Goal != Lang::SrcGoal::Undetermined
	// 古典スクリプトで HTML 風コメントを認めない文法と，拡張子で目標が定まるソースの返戻
	) return CanAcceptMarkupComments && Goal == Lang::SrcGoal::Script;
	// 最上位にインポート・公開の文を持たないソースかの返戻
	return !HasChildOf(
		Src.GetRoot(),
		[&Src](const TSNode Child) -> bool {
			// インポート・公開の文かの返戻
			return Src.Is(Child, Core::Role::ModuleStmt);
		}
	);
}

/**
 * パーサーが演算子と読んだ HTML 風コメントの有無の判定関数（古典スクリプトの言語仕様の付属書 B の読み）
 * 行中の開きと行頭の空白・ブロックコメントの後の閉じを，パーサーが演算子のトークンとして読んだ位置で見付ける
 * 計算量：ソースの長さ N とノード数 V に対し O(N + V)（位置表の引きを O(1) と看做す）
 * @param Src 解析結果
 * @return 演算子と読んだ HTML 風コメントを持てば true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool ParseAdapter::HasOperatorMarkupComment(const ParsedTree &Src) {
	// コメント範囲と演算子と読まれたトークン位置の収集
	const ReadingRules &Rules = Src.GetReading();
	const std::string_view Text = Src.GetCodeText();
	std::unordered_set<uint32_t> OpenTokens, CloseTokens;
	std::vector<std::pair<uint32_t, uint32_t>> Comments;
	WalkChildrenCursor(
		Src.GetRoot(),
		[&Src, &Text, &OpenTokens, &CloseTokens, &Comments](const TSNode Node) -> bool {
			// コメント範囲の収集
			if(Src.Is(Node, Core::Role::Comment)) {
				const std::pair<uint32_t, uint32_t> Range = Src.GetCommentRangeOf(Node);
				const uint32_t Start = Range.first, End = Range.second;
				if(Start < Text.size() && Text.substr(Start).starts_with(Core::CommentRepository::GetOpenerOf(CommentNotation::Block))) {
					Comments.push_back({ Start, End });
				}
				// コメントの内側を走査しない事の返戻
				return false;
			}
			// 子に覆われない演算子トークンの記録
			if(Src.Is(Node, Core::Role::LessThanToken) || Src.Is(Node, Core::Role::ChangeByOneToken)) {
				const uint32_t Start = Src.GetStart(Node);
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
			// トークンの子の走査を続ける事の返戻
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
	// 文法の行区切の判定
	const auto IsLineEnd = [](const char32_t Point) -> bool {
		// 行を区切るコードポイントかの返戻
		return Point == '\n' || Point == '\r' || Point == 0X2028 || Point == 0X2029;
	};
	// ブロックコメントを越えた行頭の閉記号の探索
	size_t CommentIdx = 0;
	bool IsLineHead = true;
	for(size_t At = 0; At < Text.size();) {
		while(CommentIdx < Comments.size() && Comments[CommentIdx].second <= At) ++CommentIdx;
		if(CommentIdx < Comments.size() && Comments[CommentIdx].first <= At && Comments[CommentIdx].second > At) {
			const size_t End = std::min<size_t>(Comments[CommentIdx].second, Text.size());
			for(size_t Pos = At; Pos < End;) {
				size_t Next = Pos;
				char32_t Point = 0;
				if(!TextEdit::IsUtf8Decoded(Text, Next, Point)) Next = Pos + 1;
				if(IsLineEnd(Point)) IsLineHead = true;
				Pos = Next;
			}
			At = End;
			++CommentIdx;
			continue;
		}
		// コメント外のコードポイントと行頭状態の更新
		size_t Next = At;
		char32_t Point = 0;
		if(!TextEdit::IsUtf8Decoded(Text, Next, Point)) {
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
 * Java のトークン化前 Unicode エスケープの展開関数
 * 計算量：原文の長さ B，展開前後に走査するノード数 V，再解析の費用 Q に対し O(B + V + Q)
 * @param LocalSrc 解析済の原文
 * @param Output 原文と独立した空の追記先（展開不要なら空の儘，失敗又は例外時は途中のテキストが残り得る）
 * @param Batches 展開前へ戻す位置の対応
 * @param Restorations 原文のエスケープのテキストへ戻すデコードデータ
 * @param Trial 確定テキストと一致する試行解析の格納先
 * @param Reason 入力エラー又は試行解析の失敗の格納先
 * @param IssueOffset 確定した入力エラーの原文位置の格納先
 * @return 展開完了又は展開不要なら true，桁不足・リテラルとコメントの外の不対サロゲート・試行解析の未完了なら false
 * @throws std::bad_alloc テキスト・エスケープ表・文脈表の確保に失敗する場合
 * @throws std::length_error テキスト又はデータが保持領域の上限を超えるか，展開後のテキストがパーサーの位置の範囲を超える場合
 */
bool ParseAdapter::CanExpandJavaUnicodeEscapes(
	const ParsedTree &LocalSrc,
	std::string &Output,
	std::vector<AppliedEdit> &Batches,
	std::vector<LexemeRestoration> &Restorations,
	std::optional<ParsedTree> &Trial,
	FailureReason &Reason,
	std::optional<uint32_t> &IssueOffset
) {
	// 解析済原文
	const std::string &Src = LocalSrc;
	// エスケープのマーカーが無ければ展開する物が無い事の返戻（テキストのマーカーは此の工程の知識で，統括へ出さない）
	if(Src.find("\\u") == std::string::npos) return true;
	// エスケープ１個（サロゲートの対は１文字に纏める）の原文の範囲・値・全て展開した原文での位置
	struct Escape {
		size_t Start;
		size_t End;
		size_t Expanded; // エスケープを全て展開した本文内の開始バイト位置
		char32_t Val;
	};
	// 展開候補のエスケープシーケンス
	std::vector<Escape> Escapes;
	// 原文テキストの走査
	for(size_t Pos = 0; Pos < Src.size();) {
		// バックスラッシュ以外の場合
		if(Src[Pos] != '\\') {
			// 次のバイトへ移動
			++Pos;
			continue;
		}
		// バックスラッシュ列の先頭
		const size_t Begin = Pos;
		// バックスラッシュ列の終端探索
		while(Pos < Src.size() && Src[Pos] == '\\') ++Pos;
		// 偶数本のバックスラッシュは互いにエスケープし合い，最後のバックスラッシュも展開の対象外
		if(!((Pos - Begin) % 2) || Pos >= Src.size() || Src[Pos] != 'u') continue;
		// 展開対象のバックスラッシュ位置
		const size_t Start = Pos - 1;
		Pos = Start;
		// 最初の UTF-16 値
		char32_t Val = ReadUnicodeEscape(Src, Pos).value();
		// 上位サロゲートは直後の下位サロゲートのエスケープと組んで１文字に為る
		if(Val > 0XD7FF && Val < 0XDC00) {
			// 下位サロゲートのバックスラッシュ位置
			size_t Next = Pos;
			if(const std::optional<char32_t> Low = ReadUnicodeEscape(Src, Next); Low && *Low > 0XDBFF && *Low < 0XE000) {
				// 上位の偏り (0XD800 << 10) と下位の偏り 0XDC00 を除いて 0X10000 を足す（定数部を畳んだ値）
				Val = (Val << 10) + *Low - 0X35FDC00;
				// 対の終端への移動
				Pos = Next;
			}
		}
		// 桁不足は，リテラル・コメントの中でもトークン化の前の展開で javac が拒むトークンエラーの為，失敗の返戻
		if(Val > 0X10FFFF) {
			Reason = FailureReason::InputSyntaxError;
			IssueOffset = LocalSrc.OffsetInInput(static_cast<uint32_t>(Start));
			// 不正なエスケープの返戻
			return false;
		}
		// エスケープ範囲と値の記録
		Escapes.push_back({ .Start = Start, .End = Pos, .Expanded = 0, .Val = Val });
	}
	// 昇順のエスケープとドキュメント順のリテラル・コメントを１度突合して型を取得（コード内は空，エスケープ毎の祖先走査を回避）
	const auto ContextsOf = [&LocalSrc, &Escapes](const TSNode Root, const bool IsInExpanded) -> std::vector<Core::Role> {
		// エスケープ位置に対応するトークン種別の収集
		std::vector<Core::Role> Contexts;
		Contexts.reserve(Escapes.size());
		WalkChildrenCursor(
			Root,
			[&](const TSNode Node) -> bool {
				// 包むトークン以外の子孫走査を続ける事の返戻
				if(!LocalSrc.Is(Node, Core::Role::UnicodeEscapeHost) && !LocalSrc.Is(Node, Core::Role::Identifier)) return true;
				const Core::Role Ctx = LocalSrc.Is(Node, Core::Role::EscapeLiteralHost) ?
				Core::Role::EscapeLiteralHost :
				LocalSrc.Is(Node, Core::Role::LineCommentNode) ? Core::Role::LineCommentNode : LocalSrc.Is(Node, Core::Role::BlockCommentNode) ?
				Core::Role::BlockCommentNode :
				LocalSrc.Is(Node, Core::Role::Identifier) ? Core::Role::Identifier : Core::Role::Count;
				const size_t Start = ts_node_start_byte(Node), End = ts_node_end_byte(Node);
				// 未割当エスケープの走査
				for(size_t Next = Contexts.size(); Next < Escapes.size(); ++Next) {
					const size_t At = IsInExpanded ? Escapes[Next].Expanded : Escapes[Next].Start;
					if(At >= End) break;
					// 行コメントの開きを成すエスケープは，コメント内部でなくコードとして展開
					Contexts.push_back(At < Start || Ctx == Core::Role::LineCommentNode && At < Start + 2 ? Core::Role::Count : Ctx);
				}
				// 包むトークンの内側へ降りない事の返戻
				return false;
			}
		);
		Contexts.resize(Escapes.size(), Core::Role::Count);
		// エスケープ毎の場所の返戻
		return Contexts;
	};
	// 其の場所で区切・エスケープ・コメントの終端を作らず，トークンの境界を変えない値かの判定
	const auto IsBenign = [](const Core::Role Ctx, const char32_t Val) -> bool {
		// トークンの境界を変えない事の判定結果の返戻
		return Ctx == Core::Role::EscapeLiteralHost ?
		Val != '"' && Val != '\'' && Val != '\\' && Val != '\r' && Val != '\n' :
		Ctx == Core::Role::LineCommentNode ?
		Val != '\r' && Val != '\n' :
		Ctx == Core::Role::BlockCommentNode && Val != '*' && Val != '/';
	};
	// 展開前の木で全て境界を変えなければ，先に在る展開が後の場所を変える事も無い為，原文の儘で確定する
	const std::vector<Core::Role> Before = ContextsOf(LocalSrc.GetRoot(), false);
	// 先頭から安全なエスケープ数
	size_t Benign = 0;
	while(Benign < Escapes.size() && IsBenign(Before[Benign], Escapes[Benign].Val)) ++Benign;
	// 展開不要の返戻
	if(Benign == Escapes.size()) return true;
	// javac のトークン化と同じく全て展開した原文を解析し直し，其の木で場所の判定
	std::string Full;
	Full.reserve(Src.size());
	size_t Copied = 0;
	for(Escape &Sequence : Escapes) {
		// エスケープ間の原文と代替テキストの結合
		Full.append(Src, Copied, Sequence.Start - Copied);
		Sequence.Expanded = Full.size();
		// 対を成さないサロゲートの代替文字に依る位置保持
		TextEdit::EncodeUtf8(Sequence.Val > 0XD7FF && Sequence.Val < 0XE000 ? 0XFFFD : Sequence.Val, Full);
		Copied = Sequence.End;
	}
	Full.append(Src, Copied, std::string::npos);
	Trial.emplace(LocalSrc.GetProfile(), std::move(Full));
	// 展開後の試行解析を完了出来なかった事の返戻
	if((Reason = GetTrialFailure(*Trial)) != FailureReason::None) return false;
	const std::vector<Core::Role> After = ContextsOf(Trial->GetRoot(), true);
	// 再解析で確定した文脈に応じたエスケープの展開
	Copied = 0;
	// 各エスケープの走査
	for(size_t Idx = 0; Idx < Escapes.size(); ++Idx) {
		const Escape &Sequence = Escapes[Idx];
		if(IsBenign(After[Idx], Sequence.Val)) {
			// パーサーはリテラルの中の u を重ねたエスケープをエスケープと読まない為，u を１個に揃える（値は変わらない）
			if(
				After[Idx] != Core::Role::EscapeLiteralHost ||
				std::string_view(Src).substr(Sequence.Start, Sequence.End - Sequence.Start).find("uu") == std::string_view::npos
			) continue;
			Output.append(Src, Copied, Sequence.Start - Copied);
			const size_t ChangedAt = Output.size();
			// 対を成す各エスケープの正規化
			for(size_t Pos = Sequence.Start; Pos < Sequence.End; Pos += 4) {
				Pos = Src.find_first_not_of('u', Pos + 1);
				Output += "\\u";
				Output.append(Src, Pos, 4);
			}
			RecordPreparationEdit(ChangedAt, ChangedAt + Sequence.End - Sequence.Start, Output.size() - ChangedAt, Batches);
			Restorations.push_back(
				{
					static_cast<uint32_t>(ChangedAt),
					static_cast<uint32_t>(Output.size()),
					Src.substr(Sequence.Start, Sequence.End - Sequence.Start),
					false
				}
			);
			Copied = Sequence.End;
			continue;
		}
		// 対を成さないサロゲートをリテラル・コメント内で保持し，外への不正な展開を失敗とする事の返戻
		if(Sequence.Val > 0XD7FF && Sequence.Val < 0XE000) {
			Reason = FailureReason::InputSyntaxError;
			IssueOffset = LocalSrc.OffsetInInput(static_cast<uint32_t>(Sequence.Start));
			// コード内の不対サロゲートの返戻
			return false;
		}
		// javac と同じ UTF-8 で展開し，パーサーがコメント内で改行と読めない CR は LF 化（直後の LF と組む物は１改行へ）
		Output.append(Src, Copied, Sequence.Start - Copied);
		const size_t ChangedAt = Output.size();
		if(
			const bool IsCrBeforeLf = Sequence.Val == '\r' && (
				Sequence.End < Src.size() && Src[Sequence.End] == '\n' ||
				Idx + 1 < Escapes.size() && Escapes[Idx + 1].Start == Sequence.End && Escapes[Idx + 1].Val == '\n'
			);
			!IsCrBeforeLf
		) TextEdit::EncodeUtf8(Sequence.Val == '\r' ? '\n' : Sequence.Val, Output);
		RecordPreparationEdit(ChangedAt, ChangedAt + Sequence.End - Sequence.Start, Output.size() - ChangedAt, Batches);
		if(After[Idx] != Core::Role::Count && ChangedAt < Output.size()) {
			Restorations.push_back(
				{
					static_cast<uint32_t>(ChangedAt),
					static_cast<uint32_t>(Output.size()),
					Src.substr(Sequence.Start, Sequence.End - Sequence.Start),
					false
				}
			);
		}
		Copied = Sequence.End;
	}
	if(Copied) Output.append(Src, Copied, std::string::npos);
	// 保持するリテラル等で本文が異なる場合だけ，確定後に新しく解析する
	if(Output != static_cast<const std::string &>(*Trial)) Trial.reset();
	// トークンエラーが無かった事の返戻
	return true;
}

/**
 * Ruby の埋込ドキュメントの誤読の回避関数
 * Ruby とパーサーで異なる埋込ドキュメントの終端判定を補正する
 * 早く閉じると残りの本文をコードと読み，整形で構文を壊す為，本文中の `=end` の `=` の直後へ原文に無い制御文字を挟んで解析し直す
 * 挟んだ文字はコメントの分離で本文から除く (`SourceProjector::Project`)
 * 計算量：ソースの長さ L，ノード数 N，行頭語の判定・制御文字の選択の費用 Q に対し O(L + N + Q)
 * @param LocalSrc 逐語データを切り分けた解析済の原文
 * @param Marker 挟んだ制御文字の格納先（挟まない場合は '\0'）
 * @param Preparation 挿入前へ戻す位置の対応
 * @return マーカーを挟んだ原文（挟む箇所が無い場合は空，コードが候補の制御文字を全て含みマーカーを選べない場合は無い）
 * @throws std::bad_alloc 本文又は位置の対応を確保出来ない場合
 * @throws std::length_error 本文が文字列の最大長を超える場合
 */
std::optional<std::string> ParseAdapter::EscapeRubyEmbeddedDoc(
	const ParsedTree &LocalSrc,
	unsigned char &Marker,
	std::vector<AppliedEdit> &Preparation
) {
	const std::string_view Src = LocalSrc.GetCodeText();
	Marker = '\0';
	// 埋込ドキュメントの開始記号と終端記号
	const std::string_view Opener = Core::CommentRepository::GetOpenerOf(CommentNotation::RubyEmbeddedDocument);
	const std::string_view Closer = Core::CommentRepository::GetCloserOf(CommentNotation::RubyEmbeddedDocument);
	// 埋込ドキュメントのマーカーが無ければ退避する物が無い事の返戻（テキストのマーカーは此の工程の知識で，統括へ出さない）
	if(!LocalSrc.IsParsed() || Src.find(Opener) == std::string::npos) return std::string();
	std::vector<size_t> Escapes;
	size_t DocEnd = 0;
	WalkChildrenCursor(
		LocalSrc.GetRoot(),
		[&](const TSNode Node) -> bool {
			// コメント以外の子孫走査を続ける事の返戻
			if(!LocalSrc.Is(Node, Core::Role::Comment)) return true;
			const size_t Start = LocalSrc.GetStart(Node);
			// 前の埋込ドキュメントの内側と，行頭の `=begin` で始まらないコメントは対象外の返戻
			if(
				Start < DocEnd || Start && Src[Start - 1] != '\n' ||
				!Core::CommentRepository::IsLineHeadWord(std::string_view(Src).substr(Start), Opener)
			) return false;
			const size_t Found = Escapes.size();
			for(size_t LineEnd = Src.find('\n', Start); LineEnd != std::string::npos;) {
				const size_t Head = LineEnd + 1;
				LineEnd = Src.find('\n', Head);
				if(Core::CommentRepository::IsLineHeadWord(std::string_view(Src).substr(Head), Closer)) {
					DocEnd = Head;
					// Ruby が閉じる行頭の `=end` に行着いた場合の走査打切の返戻
					return false;
				}
				// 行の中だけを探す（行に無い時にドキュメントの終わり迄探すと，長い埋込ドキュメントで２乗の費用に為る）
				const std::string_view Line(Src.data() + Head, (LineEnd == std::string::npos ? Src.size() : LineEnd) - Head);
				for(size_t Pos = Line.find(Closer); Pos != std::string_view::npos; Pos = Line.find(Closer, Pos + Closer.size())) {
					Escapes.push_back(Head + Pos + 1);
				}
			}
			// 閉じの無い埋込ドキュメント（Ruby の構文エラー）は挟まない為，控えを取り消す
			Escapes.resize(Found);
			// コメントの内側へ降りない事の返戻
			return false;
		}
	);
	// マーカーを挟んだ再解析用原文
	std::string Escaped;
	// 挟む箇所が無い場合の空の返戻
	if(Escapes.empty()) return Escaped;
	// マーカーはコードに無い制御文字から選び，本文へ戻す時にコードのテキストと取り違えない様にする
	Marker = TextEdit::AbsentControlChar(Src);
	// マーカーを選べず誤読を修復出来ない事の返戻
	if(!Marker) return std::nullopt;
	// マーカーの数を含む領域確保
	Escaped.reserve(LocalSrc.size() + Escapes.size());
	// 誤読する終端の直後へのマーカーの挿入
	size_t Copied = 0;
	for(const size_t Pos : Escapes) {
		Escaped.append(Src, Copied, Pos - Copied);
		const size_t InsertAt = Escaped.size();
		Escaped += Marker;
		RecordPreparationEdit(InsertAt, InsertAt, 1, Preparation);
		Copied = Pos;
	}
	// 逐語データも末尾へ其の儘コピーし，データ内の制御文字はマーカーとして扱わない
	Escaped.append(LocalSrc, Copied, std::string::npos);
	// マーカーを挟んだ原文の返戻
	return Escaped;
}

/**
 * 解析の為に原文へ置いたマーカーと退避したコメントの記録関数（マーカーは投影が本来のトークンへデコードし，退避したコメントは投影がコメントの記録にする，整形仕様 4.2）
 * 計算量：退避するコメント数 C とマーカーを置くテキスト長 B に対し O(C + B)
 * @param Src マーカーを置いた原文の解析結果
 * @param Marker 置いたマーカー（置いて居なければ '\0'）
 * @param Escaped 解析用の入力から退避したコメント（位置はマーカーを置いた原文の位置）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 */
void ParseAdapter::RecordMark(ParsedTree &Src, const unsigned char Marker, std::vector<EscapedComment> &&Escaped) {
	// 解析の為に原文へ置いたマーカーと退避したコメントの記録関数（マーカーは投影が本来のトークンへデコードし，退避したコメントは投影がコメントの記録にする，整形仕様 4.2）
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
 * 空の実引数の実トークンの判定関数
 * 計算量：直接の子数 C と子の間隙の総長 B に対し O(C + B)
 * @param Src 空引数を誤読する文法の解析結果
 * @param Node 仮引数のインクルードの子
 * @return エラーノードがコメント以外に実在する丸括弧の対だけを持つ場合 true
 */
bool ParseAdapter::HasEmptyArgTokens(const ParsedTree &Src, const TSNode Node) {
	// エラー以外のノードの返戻
	if(!Src.Is(Node, Core::Role::Error)) return false;
	uint32_t Tokens = 0, End = Src.GetStart(Node);
	for(uint32_t Idx = 0; Idx < ts_node_child_count(Node); ++Idx) {
		const TSNode Child = ts_node_child(Node, Idx);
		// パーサーが子へ出さなかった値を空の実引数と看做さない事の返戻
		if(std::string_view(Src).substr(End, Src.GetStart(Child) - End).find_first_not_of(" \t\r\n") != std::string_view::npos) {
			// 間隙に値を持つ事の返戻
			return false;
		}
		End = Src.GetEnd(Child);
		if(Src.Is(Child, Core::Role::Comment)) continue;
		// 補完された括弧は正常な空引数でない事の返戻
		if(ts_node_is_missing(Child)) return false;
		switch(Tokens) {
		case 0:
			if(!Src.Is(Child, Core::Role::OpeningParenToken)) return false;
			break;
		case 1:
			if(!Src.Is(Child, Core::Role::ClosingParenToken)) return false;
			break;
		default:
			return false;
		}
		++Tokens;
	}
	// コメントと空白を除く実在の丸括弧の対だけを空引数とする事の返戻
	return Tokens == 2 &&
	std::string_view(Src).substr(End, Src.GetEnd(Node) - End).find_first_not_of(" \t\r\n") == std::string_view::npos;
}

/**
 * 解析用の入力の読取の実装関数（整形仕様 3.4 の Adapt の前段，4.2）
 * テキストの段の可逆な保護・解析・共有ヘッダの文法の選直・トークン化前のエスケープの展開迄を行い，パーサーの読違の修復の前の解析結果を返す
 * 最終検証は修復の前のテキストで空行・位置を照合する為，此の段と `Repair` を分けて呼ぶ
 * 計算量：入力の準備・解析・読取事実の収集の総費用 Q に対し O(Q)
 * @param Profile 入力で選んだ文法（中身で判別する共有拡張子の変種を含む）
 * @param Src 整形対象のソース（改行の正規化済）
 * @param DataStart 正規化したソースの逐語データの開始位置（無ければ `std::string::npos`）
 * @param HasFilePreamble ファイル先頭の BOM を持ち得る入力か
 * @param Trial 確定したテキストと文法が一致する試行解析の結果
 * @return 読取の結果（`.h` の中身が C++ と判れば言語を書き換える）
 * @throws std::bad_alloc 入力・解析結果又は読取事実の確保に失敗した場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
ParseInput ParseAdapter::ReadPrepared(
	const GrammarProfile &Profile,
	const std::string &Src,
	const size_t DataStart,
	const bool HasFilePreamble,
	std::optional<ParsedTree> *const Trial
) {
	// 中身で判別する共有拡張子は，判別を終えた文法で読み，以後の工程へ其の定義を引き継ぐ（整形仕様 4.2）
	const bool CanPromote = Profile.GetReading().HasAmbiguousHeaderDialect;
	ParseInput Input { .Profile = CanPromote ? &Profile.GetSettledHeader() : &Profile, .HasFilePreamble = HasFilePreamble };
	// 適応を止めた区分の記録
	const auto Stop = [&Input](const ParseDecision Decision) -> ParseInput {
		// 対象の状態の更新
		Input.Decision = Decision;
		// 止めた結果の返戻
		return std::move(Input);
	};
	// C / C++ の行継続のトークン化前結合と前処理コメント・リテラル，CSS のエスケープしたバックスラッシュの保護（整形仕様 4.2）
	unsigned char Marker = '\0';
	// 後のトークンが次の行へ移ると行末に立ち，原文に無い行継続に為る裸のバックスラッシュを含むか
	bool HasStrayBackslash = false, HasForeignDialect = false;
	std::vector<EscapedComment> Escaped;
	std::vector<AppliedEdit> Preparation;
	std::vector<LexemeRestoration> PreparationRestorations;
	std::vector<std::pair<uint32_t, uint32_t>> DirectiveLines;
	const std::string Code = DataStart == std::string::npos ? std::string() : Src.substr(0, DataStart);
	std::optional<std::string> Prepared = ParseAdapter::PrepareSrc(
		Profile,
		DataStart == std::string::npos ? Src : Code,
		DataStart != std::string::npos,
		Marker,
		HasStrayBackslash,
		HasForeignDialect,
		Escaped,
		Preparation,
		PreparationRestorations,
		DirectiveLines,
		Input.Failure
	);
	// 行継続・コメント・引用領域を読む既存の論理トークン走査で確定した方言の判別
	if(CanPromote && HasForeignDialect) {
		Input.Failure = FailureReason::HeaderDialect;
		// 対象外の方言の返戻
		return Stop(ParseDecision::Skip);
	}
	if(!Prepared) {
		if(Input.Failure == FailureReason::None) Input.Failure = FailureReason::ParserUnreadable;
		// マーカーの不足・解析の期限切は見送，初期化失敗は失敗とする判定の返戻
		return Stop(Input.Failure == FailureReason::ParserInitFailed ? ParseDecision::Fail : ParseDecision::Skip);
	}
	if(HasStrayBackslash) {
		Input.Failure = FailureReason::StrayBackslash;
		// 忠実な出力が原文其の物しか無い事の返戻
		return Stop(ParseDecision::Skip);
	}
	// CSS のパーサーが名前の字として読めない先頭の U+FEFF を位置に対応するテキストで保護する
	if(
		const size_t First = std::min(Src.find_first_not_of(" \t\r\n"), Src.size());
		Profile.GetReading().ShouldMaskEscapedBackslashes && std::string_view(Src).substr(First).starts_with("\xEF\xBB\xBF")
	) {
		if(Prepared->empty()) *Prepared = DataStart == std::string::npos ? Src : Code;
		size_t End = First + 3;
		while(std::string_view(Src).substr(End).starts_with("\xEF\xBB\xBF")) End += 3;
		Prepared->replace(First, End - First, End - First, 'a');
		PreparationRestorations.push_back(
			{ static_cast<uint32_t>(First), static_cast<uint32_t>(End), Src.substr(First, End - First), false }
		);
	} else if(!HasFilePreamble && Src.starts_with("\xEF\xBB\xBF")) {
		// JSON は本文のエラーとして検出させ，JavaScript は本文の空白として読ませる
		if(Prepared->empty()) *Prepared = DataStart == std::string::npos ? Src : Code;
		RecordPreparationEdit(0, 0, 1, Preparation);
		Prepared->insert(0, 1, '\n');
		for(LexemeRestoration &Restoration : PreparationRestorations) {
			++Restoration.Start;
			++Restoration.End;
		}
		for(std::pair<uint32_t, uint32_t> &Line : DirectiveLines) {
			++Line.first;
			++Line.second;
		}
	}
	if(!Prepared->empty() && DataStart != std::string::npos) Prepared->append(Src, DataStart);
	Input.HasPreparedText = !Prepared->empty();
	const std::string &Text = Input.HasPreparedText ? *Prepared : Src;
	const size_t PreparedDataStart = DataStart == std::string::npos ? DataStart : Text.size() - (Src.size() - DataStart);
	// 構文木の初回構築
	if(
		const bool HasReusableTrial = Trial && *Trial && (*Trial)->IsParsed() && Prepared->empty() &&
		(*Trial)->GetProfile().GetLang().GetTreeSitterLang() == Input.Profile->GetLang().GetTreeSitterLang() &&
		static_cast<const std::string &>(**Trial) == Src;
		HasReusableTrial
	) {
		// 同じ文法のテスト木を確定後の記法規則へ結び直す
		(*Trial)->Grammar = Input.Profile;
		if(DataStart != std::string::npos) (*Trial)->AttachTrailingData(std::string_view(Src).substr(DataStart));
		Input.Src.emplace(std::move(**Trial));
		Trial->reset();
	} else if(Input.HasPreparedText && !CanPromote) Input.Src.emplace(*Input.Profile, std::move(*Prepared), PreparedDataStart);
	else Input.Src.emplace(*Input.Profile, Text, PreparedDataStart);
	Input.Src->PreparationEdits = std::move(Preparation);
	Input.Src->LexemeRestorations = std::move(PreparationRestorations);
	Input.Src->DirectiveLines = std::move(DirectiveLines);
	// 初回解析の失敗
	if(!Input.Src->IsParsed()) {
		Input.Failure = GetTrialFailure(*Input.Src);
		// 時間切を通常の見送，パーサーの初期化失敗を内部の失敗とする判定の返戻
		return Stop(Input.Failure == FailureReason::ParseTimeExceeded ? ParseDecision::Skip : ParseDecision::Fail);
	}
	const bool CanMisreadLineComments = Input.Profile->GetReading().CanMisreadCssLineComments;
	const Core::NotationRules &Notation = Input.Profile->GetRules().Notation;
	const std::string_view CodeText = Input.Src->GetCodeText();
	if(
		(CanMisreadLineComments || Core::CommentRepository::HasLineCommentBreak(Notation, CodeText)) && HasDescendantOf(
			Input.Src->GetRoot(),
			[&Input, CanMisreadLineComments, &Notation](const TSNode Node) -> bool {
				if(
					!Input.Src->Is(Node, Core::Role::Comment) ||
					!Input.Src->GetView(Node).starts_with(Core::CommentRepository::GetOpenerOf(CommentNotation::SlashLine)) ||
					!CanMisreadLineComments && !Core::CommentRepository::HasLineCommentBreak(Notation, Input.Src->GetView(Node))
				// 行コメントでないノード，又は行コメントの記法を読め本文を途中で終えるテキストも無いノードの返戻
				) return false;
				Input.IssueOffset = Input.Src->OffsetInInput(Input.Src->GetStart(Node));
				// 処理系とパーサーでコメントの終端が一致しないノードの返戻
				return true;
			}
		)
	) {
		Input.Failure = FailureReason::ParserUnreadable;
		// コメントの有無又は終端を読み違えた入力の見送の返戻
		return Stop(ParseDecision::Skip);
	}
	// 開いた要素の名前をパーサーが失い閉タグを読み違えた HTML の見送（整形仕様 9.3）
	if(Input.Src->GetReading().HasLimitedElementNameSerialization) {
		if(const std::optional<uint32_t> LostAt = LostHtmlElementNameAt(*Input.Src)) {
			Input.IssueOffset = LostAt;
			Input.Failure = FailureReason::ParserNestingExceeded;
			// パーサーのリソース上限で読めないソースの返戻
			return Stop(ParseDecision::Skip);
		}
	}
	// 共有拡張子だけ破損減少時に C++ へ昇格し，C 利用側も保つ共通の書換に限定
	if(CanPromote) {
		const uint32_t CBroken =
		(ts_node_has_error(Input.Src->GetRoot()) ? SyntaxParser::CountBrokenNodes(*Input.Src, Input.Src->GetRoot()) : 0) +
		SyntaxParser::CountMalformedCDefs(*Input.Src);
		// 昇格先は C からも読まれ得る文法で，其の定義で木を組み，以後の投影・正規化・最終検証へ同じ定義を引き継ぐ（整形仕様 4.2）
		const GrammarProfile &PromotedProfile = Profile.GetPromotedHeader();
		std::optional<ParsedTree> CppSrc;
		if(CBroken) CppSrc.emplace(PromotedProfile, Text);
		if(CppSrc && (Input.Failure = GetTrialFailure(*CppSrc)) != FailureReason::None) {
			// 方言判別の試行解析の未完了を C の通常解析へ戻さない判定の返戻
			return Stop(Input.Failure == FailureReason::ParserInitFailed ? ParseDecision::Fail : ParseDecision::Skip);
		}
		// C++ の方が破損を減らす入力
		if(
			CppSrc && SyntaxParser::CountBrokenNodes(*CppSrc, CppSrc->GetRoot()) + SyntaxParser::CountMalformedCDefs(*CppSrc) < CBroken
		) {
			CppSrc->PreparationEdits = std::move(Input.Src->PreparationEdits);
			CppSrc->LexemeRestorations = std::move(Input.Src->LexemeRestorations);
			CppSrc->DirectiveLines = std::move(Input.Src->DirectiveLines);
			Input.Src = std::move(CppSrc);
			Input.Profile = &PromotedProfile;
		}
	}
	// HTML 風コメント（言語仕様の付属書 B）は，古典スクリプトでは整形仕様 3.2 の記法に無く投影出来ない為の見送とし，文法が認めないモジュール等ではパーサーの読みを信用出来ない入力エラーとする（整形仕様 4.6, 10.4）
	if(Input.Src->GetReading().HasUnsupportedMarkupComments) if(
		const bool IsScript = IsClassicScript(*Input.Src); HasDescendantOf(
			Input.Src->GetRoot(),
			[&Input](const TSNode Node) -> bool {
				// 記法に無いコメントかの返戻
				const bool IsUnsupported = Input.Src->Is(Node, Core::Role::UnsupportedComment);
				if(IsUnsupported) Input.IssueOffset = Input.Src->OffsetInInput(ts_node_start_byte(Node));
				// 禁止された記法の返戻
				return IsUnsupported;
			}
		) || IsScript && HasOperatorMarkupComment(*Input.Src)
	) {
		Input.Failure = IsScript ? FailureReason::MarkupCommentsUnsupported : FailureReason::InputSyntaxError;
		// 投影出来ないソースの返戻
		return Stop(ParseDecision::Skip);
	}
	// Java Unicode エスケープの展開と構文木の実トークンへの一致
	if(Input.Src->GetRules().Notation.ShouldDecodeUnicodeEscapes) {
		std::string Expanded;
		std::vector<AppliedEdit> ExpandedEdits;
		std::vector<LexemeRestoration> Restorations;
		std::optional<ParsedTree> Trial;
		// javac と同じトークン展開に失敗
		if(
			!ParseAdapter::CanExpandJavaUnicodeEscapes(
				*Input.Src,
				Expanded,
				ExpandedEdits,
				Restorations,
				Trial,
				Input.Failure,
				Input.IssueOffset
			)
		// 展開不能又は期限切は見送，初期化失敗は失敗とする判定の返戻
		) return Stop(Input.Failure == FailureReason::ParserInitFailed ? ParseDecision::Fail : ParseDecision::Skip);
		if(!Expanded.empty()) {
			Input.HasPreparedText = true;
			std::vector<AppliedEdit> Prev = std::move(Input.Src->PreparationEdits);
			if(Trial) Input.Src = std::move(Trial);
			else Input.Src.emplace(*Input.Profile, std::move(Expanded));
			Prev.insert(Prev.end(), std::make_move_iterator(ExpandedEdits.begin()), std::make_move_iterator(ExpandedEdits.end()));
			Input.Src->PreparationEdits = std::move(Prev);
			Input.Src->LexemeRestorations = std::move(Restorations);
		}
	}
	// ソース補正後の解析失敗
	if(!Input.Src->IsParsed()) {
		Input.Failure = GetTrialFailure(*Input.Src);
		// 補正後の時間切とパーサー故障の区分の返戻
		return Stop(Input.Failure == FailureReason::ParseTimeExceeded ? ParseDecision::Skip : ParseDecision::Fail);
	}
	// 埋込ドキュメントを含む Ruby
	if(Input.Src->GetRules().Notation.IsAvailable(CommentNotation::RubyEmbeddedDocument)) {
		std::vector<AppliedEdit> EmbeddedEdits;
		std::optional<std::string> Escaped = ParseAdapter::EscapeRubyEmbeddedDoc(*Input.Src, Marker, EmbeddedEdits);
		if(!Escaped) {
			Input.Failure = FailureReason::ParserUnreadable;
			// マーカーを選べず埋込ドキュメントの誤読を修復出来ないソースの返戻（整形仕様 4.2）
			return Stop(ParseDecision::Skip);
		}
		if(!Escaped->empty()) {
			Input.HasPreparedText = true;
			std::vector<AppliedEdit> Prev = std::move(Input.Src->PreparationEdits);
			const size_t TailStart = Input.Src->GetHaltedDataStart();
			const size_t EscapedDataStart = TailStart == std::string::npos ? TailStart : Escaped->size() - (Input.Src->size() - TailStart);
			Input.Src.emplace(*Input.Profile, std::move(*Escaped), EscapedDataStart);
			Prev.insert(Prev.end(), std::make_move_iterator(EmbeddedEdits.begin()), std::make_move_iterator(EmbeddedEdits.end()));
			Input.Src->PreparationEdits = std::move(Prev);
			// 埋込ドキュメント退避後の解析失敗
			if(!Input.Src->IsParsed()) {
				Input.Failure = GetTrialFailure(*Input.Src);
				// 埋込ドキュメント退避後の時間切とパーサー故障の区分の返戻
				return Stop(Input.Failure == FailureReason::ParseTimeExceeded ? ParseDecision::Skip : ParseDecision::Fail);
			}
		}
	}
	// 投影が本来のトークンへデコードする解析用のマーカーの記録（整形仕様 4.2）
	RecordMark(*Input.Src, Marker, std::move(Escaped));
	ParsedTree &LocalSrc = *Input.Src;
	// 行頭の式を囲む解析用の括弧は，入力・完成候補の双方で内側の式だけを投影する
	if(!LocalSrc.GetReading().TrailingDataMarker.empty() && !LocalSrc.PreparationEdits.empty()) {
		WalkChildrenCursor(
			LocalSrc.GetRoot(),
			[&LocalSrc](const TSNode Node) -> bool {
				// 単一の式を囲むノード以外へ降りる事の返戻
				if(!LocalSrc.Is(Node, Core::Role::GroupingParen) || ts_node_named_child_count(Node) != 1) return true;
				const uint32_t Begin = LocalSrc.GetStart(Node), End = LocalSrc.GetEnd(Node);
				// 原文に在る括弧又は欠落した括弧を解析用の囲みとしない事の返戻
				if(
					End <= Begin + 1 || LocalSrc[Begin] != '(' || LocalSrc[End - 1] != ')' ||
					LocalSrc.OffsetBeforePreparation(Begin) != LocalSrc.OffsetBeforePreparation(Begin + 1) ||
					LocalSrc.OffsetBeforePreparation(End - 1) != LocalSrc.OffsetBeforePreparation(End)
				) return true;
				const TSNode Inner = ts_node_named_child(Node, 0);
				// 単独の `__END__` をデータ開始へ変えない括弧は出力にも保持する
				if(!LocalSrc.Is(Inner, Core::Role::Identifier) || LocalSrc.GetView(Inner) != LocalSrc.GetReading().TrailingDataMarker) {
					LocalSrc.SyntheticGroups.emplace_back(Begin, End - 1);
				}
				// 内側の式の走査を続ける事の返戻
				return true;
			}
		);
	}
	// 実引数を逐語保持するマクロと文へ展開して本体の波括弧を要するマクロを収集
	if(LocalSrc.GetReading().HasPreprocMacros) LocalSrc.CollectMacroStates();
	// 言語別の修復自身が壊したノードも最終の構文検査で見付ける為，修復の前に壊れて居たノードの数を控える（健全な入力では数えない）
	Input.PreparedBroken = ts_node_has_error(LocalSrc.GetRoot()) ?
	SyntaxParser::CountBrokenNodes(LocalSrc, LocalSrc.GetRoot(), &Input.PreparedFirstBroken) :
	0;
	// 修復の前の読取結果の返戻
	return Input;
}

/**
 * 前置型の連続を誤読した解析結果の修復関数
 * readonly の被演算子へ吸った合併・交差・条件を分け，keyof の直後の型と共に解析用に囲む
 * 計算量：ノード数 N，編集数 E，再解析費用 P に対し O(N + E log(E + 1) + P)
 * @param Src 読違を含む解析用の入力
 * @throws FormatLimitExceeded 構文の照会又は再解析が予算を超える場合
 * @throws std::bad_alloc 修復の編集又は構文木を確保出来ない場合
 * @throws std::length_error 編集後の本文が保持領域の上限を超える場合
 */
void ParseAdapter::RepairPrefixTypeErrors(ParsedTree &Src) {
	std::vector<TextEdit> Edits;
	std::vector<std::pair<uint32_t, uint32_t>> Groups;
	const std::vector<PrefixTypePart> &Parts = Src.GetReading().PrefixTypes;
	const auto PartOf = [&](const TSNode Node) -> PrefixTypePart {
		Src.PrefixTypeRepairWork.Charge(1);
		const TSSymbol Symbol = Src.GetSymbol(Node);
		// エラー等の文法の表に無い記号は対象外とする
		return Symbol < Parts.size() ? Parts[Symbol] : PrefixTypePart::None;
	};
	const auto FirstOperand = [&Src](const TSNode Node) -> TSNode {
		for(const ParsedChild &Part : Src.GetChildrenOf(Node)) {
			Src.PrefixTypeRepairWork.Charge(1);
			// コメントでない最初の型の返戻
			if(Part.IsNamed && !Src.Is(Part.Node, Core::Role::Comment)) return Part.Node;
		}
		// 被演算子の無い事の返戻
		return TSNode{};
	};
	TSNode Prefix {};
	PrefixTypePart Previous = PrefixTypePart::None;
	bool HasOuterPrefix = false;
	WalkRecordedChildren(
		Src,
		Src.GetRoot(),
		[&](const TSNode Node, const std::span<const TSNode> Ancestors) -> bool {
			Src.PrefixTypeRepairWork.Charge(1);
			// コメントの中をトークンの続きとして読まない
			if(Src.Is(Node, Core::Role::Comment)) return false;
			const PrefixTypePart Part = PartOf(Node);
			if(!ts_node_is_null(Prefix) && ts_node_is_named(Node) && !ts_node_has_error(Node)) {
				TSNode Operand = Node;
				// 前置型の直接の被演算子だけを取り，明示した括弧を越えない
				while(!ts_node_is_null(Operand) && PartOf(Operand) == PrefixTypePart::Weak) Operand = FirstOperand(Operand);
				if(
					!ts_node_is_null(Operand) && PartOf(Operand) == PrefixTypePart::Operand &&
					(HasOuterPrefix || Src.GetEnd(Operand) < Src.GetEnd(Node))
				) {
					const uint32_t Begin = Src.GetStart(Prefix), End = Src.GetEnd(Operand);
					TextEdit::Push(Begin, Begin, "(", Edits);
					TextEdit::Push(End, End, ")", Edits);
					Groups.emplace_back(Begin, End);
				}
				Prefix = TSNode{};
			}
			if(!ts_node_child_count(Node)) {
				// 回復の ERROR を跨いでも，隣接する前置語と健全な型の範囲だけを繋ぐ
				HasOuterPrefix = Previous == PrefixTypePart::Operator;
				Prefix = !ts_node_is_missing(Node) && Part == PrefixTypePart::Prefix &&
				(HasOuterPrefix || PartOf(Ancestors.back()) == PrefixTypePart::Wrapper) ? Node : TSNode{};
				Previous = ts_node_is_missing(Node) ? PrefixTypePart::None : Part;
			}
			// 型の中の別の前置演算も原文順の同じ走査で調べる
			return true;
		}
	);
	std::ranges::sort(Groups);
	TextEdit::Apply(Src, Edits, false, &Groups);
	// 終了
	return;
}

/**
 * 空の実引数と閉括弧前の空白を誤読した解析結果の修復関数
 * 計算量：構文木のノード数 N，閉境界の空白長 B，修復区間の登録・編集適用の費用 Q に対し O(N + B + Q)
 * @param Src 読違を含む解析用の入力
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void ParseAdapter::RepairArgumentErrors(ParsedTree &Src) {
	// 誤読された実引数の括弧と閉境界の編集収集
	std::vector<TextEdit> Edits;
	WalkAst(
		Src.GetRoot(),
		[&](const TSNode Node) -> void {
			// 対象の呼出以外を編集しない場合の終了
			if(!Src.Is(Node, Core::Role::IncludeStmtNode)) return;
			for(uint32_t Idx = 0; Idx < ts_node_child_count(Node); ++Idx) {
				const TSNode Child = ts_node_child(Node, Idx);
				const bool HasEmptyArgs = HasEmptyArgTokens(Src, Child);
				// 値の脱落と後続構文の吸込が在っても，実在する閉括弧の直前のコード外の空白だけを修復する
				if(
					!HasEmptyArgs && (Src.Is(Child, Core::Role::CallArgs) || Src.Is(Child, Core::Role::Error)) &&
					Src.Is(ts_node_child(Child, 0), Core::Role::OpeningParenToken)
				) {
					uint32_t LineCommentEnd = 0, SlashEnd = 0;
					const uint32_t Begin = Src.GetStart(Child);
					WalkChildrenCursor(
						Child,
						[&Src, &Edits, &LineCommentEnd, &SlashEnd, Begin](const TSNode Part) -> bool {
							// 回復時に２つのトークンへ分かれた行コメントも，本文と終端の改行を保つ
							if(Src.Is(Part, Core::Role::SlashToken)) {
								if(SlashEnd == Src.GetStart(Part)) LineCommentEnd = static_cast<uint32_t>(std::min(Src.find('\n', SlashEnd), Src.size()));
								SlashEnd = Src.GetEnd(Part);
							}
							if(Src.Is(Part, Core::Role::Comment)) {
								if(Src.GetView(Part).starts_with(Core::CommentRepository::GetOpenerOf(CommentNotation::SlashLine))) {
									LineCommentEnd = Src.GetEnd(Part);
								}
								// コメント本文を走査しない事の返戻
								return false;
							}
							// 別の規則の本体と URL・リテラル本文を編集しない事の返戻
							if(
								Src.Is(Part, Core::Role::IncludeStmtNode) || Src.Is(Part, Core::Role::CallArgs) && Src.IsCssUrlArgs(Part) ||
								Src.Is(Part, Core::Role::SpaceSkip) || Src.Is(Part, Core::Role::SrcLeaf)
							) return false;
							if(Src.Is(Part, Core::Role::ClosingParenToken) && !ts_node_is_missing(Part) && Src.GetStart(Part) >= LineCommentEnd) {
								const uint32_t End = Src.GetStart(Part);
								uint32_t Start = End;
								while(Start > Begin && std::string_view(" \t\r\n").find(Src[Start - 1]) != std::string_view::npos) --Start;
								if(Start < End && Start != LineCommentEnd) TextEdit::Push(Start, End, "", Edits);
							}
							// 回復ノードの内側も閉境界を収集する事の返戻
							return true;
						}
					);
				}
				if(!HasEmptyArgs) continue;
				// 括弧だけを除き，内側のコメントは解析・投影するテキストに保持する
				for(uint32_t Part = 0; Part < ts_node_child_count(Child); ++Part) {
					if(const TSNode Token = ts_node_child(Child, Part); !Src.Is(Token, Core::Role::Comment)) {
						TextEdit::Push(Src.GetStart(Token), Src.GetEnd(Token), "", Edits);
					}
				}
				// 最初の括弧の並びだけを消した場合の終了
				return;
			}
			// 終了
			return;
		}
	);
	// 本文と構文木の世代及び修復前への座標マッピングを同じ編集で更新
	TextEdit::Apply(Src, Edits);
	// 終了
	return;
}

/**
 * PHP の本体の範囲の取得関数（整形仕様 4.4 の PHP の地の文）
 * PHP の地の文は出力其の物で，構文木の外に在る空白（最初のトークンの前と，閉タグの後に続く地の文）も出力に現れる
 * 最後の閉タグの後に開始タグが戻らない範囲は，パーサーが `//` 等をコメントと読んでも地の文とする
 * 計算量：根の直接子数 C，先頭の役割を持つ子の取得費用の費用 Q に対し O(C + Q)
 * @param Src 解析済のソース
 * @return 本体の始まりと終わり（其の外のテキストは投影が逐語の地の文として保つ，PHP 以外と外に地の文の無いソースはソース全体）
 */
std::pair<uint32_t, uint32_t> ParseAdapter::GetOuterTextBounds(const ParsedTree &Src) {
	// 対象とする本文の長さの取得
	const uint32_t Size =
	static_cast<uint32_t>(Src.GetHaltedDataStart() == std::string::npos ? Src.size() : Src.GetHaltedDataStart());
	// 地の文を持たない言語と解析して居ないソースの返戻
	if(!Src.GetReading().HasOuterText || !Src.IsParsed()) return { 0, Size };
	// 構文木範囲の取得
	const TSNode Root = Src.GetRoot();
	const uint32_t RootCount = ts_node_named_child_count(Root);
	// 地の文で終わる本体は末尾空白を覆う根でなく最後のトークンで閉鎖（`?>` 直後の空白も地の文，開始タグ無は全体が地の文）
	bool IsTextTail = true;
	uint32_t BodyEnd = Src.GetEnd(Root);
	if(RootCount) {
		// 最終名前付子からの後続断片の切離
		const TSNode Last = ts_node_named_child(Root, RootCount - 1);
		IsTextTail = Src.Is(Last, Core::Role::InlineText) && ts_node_is_null(Src.GetFirstNamedChild(Last, Core::Role::CodeOpeningTag));
		BodyEnd = Src.GetEnd(Last);
		// `?>` の後に `<?php` が戻らない範囲は地の文だが，`//` で始まるとパーサーが之をコメントと読む
		// 其の儘だと地の文が通常コメントの規則で消える為，終了タグの領域の後を地の文とする
		for(uint32_t Idx = RootCount - 1; !IsTextTail && Idx;) {
			// 終了タグを含む領域の候補
			const TSNode Child = ts_node_named_child(Root, --Idx);
			// コメント以外のノードに達したら地の文でない事の打切
			if(!Src.Is(Child, Core::Role::Comment) && !Src.Is(Child, Core::Role::InlineText)) break;
			if(
				!ts_node_is_null(Src.GetFirstNamedChild(Child, Core::Role::PhpEndTag)) &&
				ts_node_is_null(Src.GetFirstNamedChild(Child, Core::Role::CodeOpeningTag))
			) {
				IsTextTail = true;
				BodyEnd = Src.GetEnd(Child);
				break;
			}
		}
	}
	const uint32_t BodyStart = std::min(Src.GetStart(Root), Size), End = IsTextTail ? std::max(BodyStart, BodyEnd) : Size;
	// 閉タグの直後の改行１個を処理系が読み捨てる為，其れを超えるテキストが在る時だけ地の文とした範囲の返戻
	return { BodyStart, End + 1 < Size ? End : Size };
}

/**
 * 解析用のマーカーの本来の字へのデコード関数（整形仕様 4.2，マーカーは位置に依らず字毎に戻せる）
 * C / C++ の連結指定の波括弧と前処理リテラルの `*`，CSS のエスケープしたバックスラッシュを戻す（Ruby の埋込ドキュメントのマーカーはコメントの分離で戻す）
 * 計算量：テキスト長 B に対し O(B)
 * @param Src マーカーを置いた原文の解析結果
 * @param Text マーカーを含み得るテキスト（コメントを除いて繋いだ物でも良い）
 * @return マーカーを本来の字へ戻したテキスト
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::string ParseAdapter::DecodeMarks(const ParsedTree &Src, std::string Text) {
	// 解析用の退避マーカーの取得
	const unsigned char Marker = Src.AnalysisMark;
	// マーカーを含まないテキストの返戻
	if(!Marker || Text.find(Marker) == std::string::npos) return Text;
	if(Src.GetReading().ShouldMaskEscapedBackslashes) std::replace(Text.begin(), Text.end(), static_cast<char>(Marker), '\\');
	else if(Src.GetRules().HasLineDirectives) {
		// 連結指定の波括弧と前処理リテラルの `*` の復元
		RestoreLinkageGuards(Src.GetReading(), Text, Marker);
		std::replace(Text.begin(), Text.end(), static_cast<char>(Marker), '*');
	}
	// デコードしたテキストの返戻
	return Text;
}

/**
 * 埋込の中身が要素の終わりの直前迄を占めるかの判定関数（HTML 仕様の script / style の本文のトークン化の状態を辿る）
 * script の本文は `<!--` でエスケープ状態に入り，其の中の `<script` で二重のエスケープ状態に入り，二重のエスケープの中の `</script` は要素を閉じない
 * 計算量：中身の長さ N に対し O(N)
 * @param Rules 入力の読取規則
 * @param Content 埋込の中身
 * @param IsScript script の中身か（偽は style の中身で，エスケープ状態を持たない）
 * @return 中身の途中で要素が閉じず，中身の後の終了タグで要素が閉じるなら true
 */
bool ParseAdapter::HasPreservedRawTextEnd(const ReadingRules &Rules, const std::string_view Content, const bool IsScript) {
	// 対象名の取得
	const std::string_view Name = IsScript ? Rules.RawTextEscapeElement : Rules.RawTextPlainElement;
	// 位置からタグ名が始まり，其の後に名前の区切（空白・`/`, `>`）が続くか
	const auto IsTagAt = [Content, Name, &Rules](const size_t At) -> bool {
		// 中身の終わりは後に続く終了タグの `<` で区切でない事を含めた一致の返戻
		return Content.size() - At > Name.size() && IsHtmlNameEqual(Content.substr(At, Name.size()), Name) &&
		Rules.RawTextNameTerminators.find(Content[At + Name.size()]) != std::string_view::npos;
	};
	enum class ScriptState : uint8_t { Data, DoubleEscaped, Escaped }; // script 本文を読む状態（初期の Data を先頭に保持）
	ScriptState State = ScriptState::Data;
	size_t Dashes = 0;
	for(size_t Pos = 0; Pos < Content.size(); ++Pos) {
		const unsigned char Char = Content[Pos];
		const bool IsClosingTag = Content.substr(Pos).starts_with(Rules.RawTextEndOpen) && IsTagAt(Pos + Rules.RawTextEndOpen.size());
		// 中身の途中で要素を閉じる終了タグの返戻
		if(IsClosingTag && State != ScriptState::DoubleEscaped) return false;
		if(!IsScript) continue;
		if(State == ScriptState::Data) {
			// エスケープ状態の開始
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
		if(Char == '>' && Dashes > 1) State = ScriptState::Data;
		// 二重のエスケープ状態の開始と終了
		else if(Char == '<' && State == ScriptState::Escaped && IsTagAt(Pos + 1)) State = ScriptState::DoubleEscaped;
		else if(IsClosingTag) State = ScriptState::Escaped;
		Dashes = 0;
	}
	// 後に続く終了タグで要素が閉じるかの返戻
	return State != ScriptState::DoubleEscaped;
}

/**
 * 正規化結果からの解析用の入力の読取関数
 * 計算量：入力の準備・解析・読取事実の収集の総費用 Q に対し O(Q)
 * @param Profile 対象の文法
 * @param Input 入力の指定と正規化したテキスト
 * @return 読取の結果
 * @throws std::bad_alloc 入力・解析結果又は読取事実の確保に失敗した場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
ParseInput ParseAdapter::Read(const GrammarProfile &Profile, InputNormalizer::NormalizedInput &Input) {
	// 読取結果の返戻
	return ReadPrepared(Profile, Input.Text, Input.DataStart, Input.HasFilePreamble, &Input.Trial);
}

/**
 * パーサーの読違の修復関数（整形仕様 3.4 の Adapt の後段，4.2）
 * コメントを紐付ける前にパーサーの読違を修復し，入力と完成候補で同じ投影の前提を用いる
 * 計算量：有効な修復の総費用 R，直接の宣言収集の費用 D，JSX の境界処理の費用 J に対し O(R + D + J)
 * @param Input Src と Profile を持つ読取済の入力（修復後へ更新し，例外時は修復途中の状態が残り得る）
 * @return 必要な修復手順を終えれば true，既知の読違を修復出来なければ見送へ更新して false
 * @throws FormatLimitExceeded 再解析の期限を超えるか，可逆修復が構造又は記号の対応を保てない場合
 * @throws std::bad_alloc 修復テキスト・編集列・位置マッピング・宣言又は演算子データの確保に失敗した場合
 * @throws std::length_error テキスト又はデータが保持領域の上限を超えるか，修復後のテキストがパーサーの位置の範囲を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
bool ParseAdapter::CanRepair(ParseInput &Input) {
	// 修復する解析済のソースへの参照
	ParsedTree &Src = *Input.Src;
	if(Src.GetReading().HasPreprocMacros) {
		// 見出へ吸った次の論理行は原文の構文に戻せる境界を持たない為，入力全体を保持する見送
		if(const std::optional<uint32_t> End = GetMisreadDirectiveEnd(Src)) {
			Input.IssueOffset = Src.OffsetInInput(*End);
			Input.Decision = ParseDecision::Skip;
			Input.Failure = FailureReason::ParserUnreadable;
			// 原文を保つ見送の返戻
			return false;
		}
		// 準備時の位置は修復前に１度だけ消費する
		Src.DirectiveLines.clear();
	}
	if(Src.IsParsed()) if(const FailureReason Reason = RepairBeforeProjection(Src); Reason.Val != FailureReason::None) {
		Input.Decision = ParseDecision::Skip;
		Input.Failure = Reason;
		// 既知の読違を修復出来なかったソースの見送の返戻
		return false;
	}
	if(!Src.GetReading().PrefixTypes.empty()) RepairPrefixTypeErrors(Src);
	if(Src.GetReading().CanMisreadRuleArgs && ts_node_has_error(Src.GetRoot())) RepairArgumentErrors(Src);
	// 修復後の前処理の状態を共有し，宣言の表は必要な照会でだけ収集する
	if(Src.GetReading().HasPreprocMacros) {
		Src.CollectMacroStates();
		RepairValDecls(Src);
	}
	if(Src.GetBoolNameSource() == ParsedTree::BoolNameSource::Shadow) Src.RecordNodeRelations();
	// JSX の本文の値と境界の意味付
	NormalizeJsxText(Src);
	// 修復を完了した事の返戻
	return true;
}

/**
 * トークン化前の Unicode エスケープを展開し，元のバイト境界を対応付ける関数
 * バックスラッシュ列の成立条件と繰り返した u を読み，生成したバックスラッシュは再展開しない
 * 計算量：本文長 B に対し O(B + 1)
 * @param Text トークン化前の本文
 * @return 展開後の値と元の境界（不正なエスケープは無い）
 * @throws std::bad_alloc 本文と境界のメモリを確保出来ない場合
 * @throws std::length_error 本文又は境界の要素数が上限を超える場合
 */
std::optional<LexemeReading> ParseAdapter::ReadUnicodeEscapes(const std::string_view Text) {
	LexemeReading Reading;
	Reading.OriginalOffsets.push_back(0);
	size_t Slashes = 0;
	for(size_t Pos = 0; Pos < Text.size();) {
		const size_t Begin = Pos;
		std::optional<char32_t> Point;
		if(Text[Pos] == '\\' && !(Slashes % 2)) Point = ReadUnicodeEscape(Text, Pos);
		if(Point) {
			// 不正なエスケープの返戻
			if(*Point > 0X10FFFF) return std::nullopt;
			if(*Point > 0XD7FF && *Point < 0XDC00) {
				size_t Next = Pos;
				if(const std::optional<char32_t> Low = ReadUnicodeEscape(Text, Next); Low && *Low > 0XDBFF && *Low < 0XE000) {
					*Point = (*Point << 10) + *Low - 0X35FDC00;
					Pos = Next;
				}
			}
			const size_t Start = Reading.Text.size();
			// 単独のサロゲートはコメントの記号を作らず，綴りを保持する
			if(*Point > 0XD7FF && *Point < 0XE000) {
				Reading.Text.append(Text.substr(Begin, Pos - Begin));
				for(size_t At = Begin + 1; At <= Pos; ++At) Reading.OriginalOffsets.push_back(At);
			} else {
				TextEdit::EncodeUtf8(*Point, Reading.Text);
				for(size_t Idx = Start + 1; Idx < Reading.Text.size(); ++Idx) Reading.OriginalOffsets.push_back(Begin);
				Reading.OriginalOffsets.push_back(Pos);
			}
			Slashes = 0;
		} else {
			Pos = Begin + 1;
			Reading.Text.push_back(Text[Begin]);
			Reading.OriginalOffsets.push_back(Pos);
			Slashes = Text[Begin] == '\\' ? Slashes + 1 : 0;
		}
	}
	// 展開後の値と元の境界の返戻
	return Reading;
}

/**
 * コメントのトークン検査用の Unicode エスケープの展開関数
 * 原文の位置と出力本文は変えず，共有する翻訳段階の読取を使う
 * @param Text 検査するテキスト
 * @return トークン化前のエスケープを展開したテキスト（不正なエスケープは無い）
 */
std::optional<std::string> ParseAdapter::DecodeCommentEscapes(const std::string_view Text) {
	std::optional<LexemeReading> Reading = ReadUnicodeEscapes(Text);
	// 不正なエスケープの返戻
	if(!Reading) return std::nullopt;
	// トークン検査用の展開済のテキストの返戻
	return std::move(Reading->Text);
}

/**
 * トークン化前に改行へ展開する Unicode のエスケープを持つかの判定関数
 * 連続したバックスラッシュの数が奇数の時だけ其の最後のバックスラッシュがエスケープを始める
 * 計算量：テキストの長さ L に対し O(L)
 * @param Text テキスト
 * @return LF, CR へ展開するエスケープを持てば true
 */
bool ParseAdapter::HasEscapedLineBreak(const std::string_view Text) {
	// バックスラッシュの並びの走査
	for(size_t Pos = 0; Pos < Text.size();) {
		if(Text[Pos] != '\\') {
			++Pos;
			continue;
		}
		const size_t Begin = Pos;
		while(Pos < Text.size() && Text[Pos] == '\\') ++Pos;
		// 偶数本のバックスラッシュは互いにエスケープし合う
		if(!((Pos - Begin) % 2)) continue;
		size_t End = Pos - 1;
		// LF, CR へ展開するエスケープの返戻
		if(const std::optional<char32_t> Val = ReadUnicodeEscape(Text, End); Val == '\n' || Val == '\r') return true;
		// 読んだエスケープの後（エスケープで無ければバックスラッシュの列の後）からの走査
		Pos = std::max(Pos, End);
	}
	// 改行へ展開するエスケープを持たない事の返戻
	return false;
}

/**
 * 改行を値其の物として保つ範囲の収集関数
 * リテラル・PHP の地の文・HTML の逐語内容の範囲を原文順に集める
 * 保護範囲の確定に言語の走査が要る為，入力表現の正規化の為の試行解析を此処で行う（整形仕様 4.1, 4.2）
 * 計算量：原文長 N，ノード数 V，試行解析の総費用 Q に対し O(N + V + Q)
 * @param Profile 対象の文法
 * @param Src 原文
 * @param ShouldIncludeBlockComments ブロックコメントも保つ範囲に含めるか
 * @param Trial 同じテキストの解析結果が在れば再利用する格納先
 * @param Reason 解析の期限切又は初期化失敗で範囲を確定出来なかった理由の格納先
 * @return 改行候補を含む保護範囲（開始・終了のバイト位置）の並び（解析の未完了時は無い）
 * @throws std::bad_alloc 逐語範囲と試行解析のテキストの確保に失敗した場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::optional<ParseAdapter::LiteralRangeList> ParseAdapter::LiteralRanges(
	const GrammarProfile &Profile,
	const std::string &Src,
	const bool ShouldIncludeBlockComments,
	std::optional<ParsedTree> &Trial,
	FailureReason &Reason
) {
	// 逐語保持する範囲列
	std::vector<std::pair<uint32_t, uint32_t>> Literals;
	// トークン化前の展開で誤読を解き，保護範囲だけを原文の位置へ戻す
	std::optional<LexemeReading> Expanded;
	std::optional<ParsedTree> ExpandedTrial;
	if(Profile.GetRules().Notation.ShouldDecodeUnicodeEscapes && Src.find("\\u") != std::string::npos) {
		Expanded = ReadUnicodeEscapes(Src);
		if(!Expanded) {
			Reason = FailureReason::InputSyntaxError;
			// トークン化前の不正なエスケープの返戻
			return std::nullopt;
		}
		ExpandedTrial.emplace(Profile, Expanded->Text);
	}
	if(!ExpandedTrial && !Trial) Trial.emplace(Profile, Src);
	const ParsedTree &Raw = ExpandedTrial ? *ExpandedTrial : *Trial;
	// 試行解析を完了出来ず範囲を確定出来ない事の返戻
	if((Reason = GetTrialFailure(Raw)) != FailureReason::None) return std::nullopt;
	if(Raw.IsParsed()) {
		const std::pair<uint32_t, uint32_t> Body = GetOuterTextBounds(Raw);
		const ReadingRules &Reading = Profile.GetReading();
		if(Reading.HasOuterText && Body.first) Literals.emplace_back(0, Body.first);
		// 改行の正規化だけが使う範囲なので，CR 又は Unicode の区切の先頭バイトを含まない枝へ降りない
		// UTF-8 の同じ先頭バイトを持つ其の他の文字も含め，保護範囲の取り漏しを防ぐ
		size_t Candidate = Raw.find_first_of("\r\xC2\xE2");
		WalkChildrenCursor(
			Raw.GetRoot(),
			[&](const TSNode Node) -> bool {
				const uint32_t Start = Raw.GetStart(Node), End = Raw.GetEnd(Node);
				while(Candidate < Start) Candidate = Raw.find_first_of("\r\xC2\xE2", Candidate + 1);
				// 原文順のカーソルで，改行候補を持たない部分木を除く事の返戻
				if(Candidate >= End) return false;
				// 不透明領域のコメント種別の取得
				const bool IsComment = Raw.Is(Node, Core::Role::Comment);
				// ドキュメントに属するコメントと文書型宣言のテキストは通常コメントと異なり逐語内容に為る
				if(Reading.HasCommentDocContent && (Raw.Is(Node, Core::Role::CommentNode) || Raw.Is(Node, Core::Role::DocTypeDecl))) {
					Literals.emplace_back(Raw.GetStart(Node), Raw.GetEnd(Node));
					// 逐語内容の内側へ降りない事の返戻
					return false;
				}
				// 保持要素の開始・終了タグの間を１つの領域として保護し，子孫の空白も同じ契約で扱う
				if(Reading.HasCommentDocContent && Raw.Is(Node, Core::Role::MarkupElementNode)) {
					if(const TSNode Tag = Raw.GetFirstNamedChild(Node, Core::Role::StartTag); !ts_node_is_null(Tag)) {
						const TSNode Name = Raw.GetFirstNamedChild(Tag, Core::Role::TagNameNode);
						std::string Lower = ts_node_is_null(Name) ? std::string() : std::string(Raw.GetView(Name));
						ToLowerAscii(Lower);
						bool ShouldPreserve = GrammarProfile::GetMarkupTraits(GrammarProfile::MarkupContext::Html, Lower) & GrammarProfile::Preserve;
						ForEachNamedChild(
							Tag,
							[&](const TSNode Attribute) -> bool {
								// 名前と値を持つ属性でない子（タグ名等）を飛ばす事の返戻
								if(Raw.Is(Attribute, Core::Role::TagNameNode) || ts_node_named_child_count(Attribute) < 2) return true;
								std::string_view Val = Raw.GetView(ts_node_named_child(Attribute, 1));
								if(Val.size() > 1 && (Val.front() == '\'' || Val.front() == '"')) Val = Val.substr(1, Val.size() - 2);
								ShouldPreserve |= Profile.ShouldPreserveMarkupWhitespace(Raw.GetView(ts_node_named_child(Attribute, 0)), Val);
								// 次の属性へ進む事の返戻
								return true;
							}
						);
						if(ShouldPreserve) {
							const TSNode Close = LastNamedChild(Node);
							if(
								const uint32_t End = !ts_node_is_null(Close) && Raw.Is(Close, Core::Role::MarkupClose) ? Raw.GetStart(Close) : Raw.GetEnd(Node);
								Raw.GetEnd(Tag) < End
							) Literals.emplace_back(Raw.GetEnd(Tag), End);
							// 保持要素の内側へ降りない事の返戻
							return false;
						}
					}
				}
				// PHP の地の文も出力其の物の為，リテラルと同じく改行を保つ
				// リテラル以外の内側へ降りる事の返戻
				if(
					const bool IsBlock = ShouldIncludeBlockComments && IsComment &&
					Raw.GetView(Node).starts_with(Core::CommentRepository::GetOpenerOf(CommentNotation::Block));
					!IsBlock && (
						!Raw.Is(Node, Core::Role::InlineText) && !Raw.Is(Node, Core::Role::LiteralInterior) && !Raw.Is(Node, Core::Role::SrcLeaf) &&
						!Raw.Is(Node, Core::Role::RenderedText) || IsComment
					)
				) return true;
				// 逐語範囲の記録
				Literals.emplace_back(Raw.GetStart(Node), Raw.GetEnd(Node));
				// リテラルの内側へ降りない事の返戻
				return false;
			}
		);
		if(Reading.HasOuterText && Body.second < Raw.size()) Literals.emplace_back(Body.second, static_cast<uint32_t>(Raw.size()));
	}
	if(ExpandedTrial) for(std::pair<uint32_t, uint32_t> &Range : Literals) {
		Range.first = static_cast<uint32_t>(Expanded->OriginalOffsets[Range.first]);
		Range.second = static_cast<uint32_t>(Expanded->OriginalOffsets[Range.second]);
	}
	// 保つ範囲の並びの返戻
	return Literals;
}

/**
 * 解析用のマーカーの本来のトークンへのデコード関数（整形仕様 3.3, 4.2，中核のトークン・幅の計数・最終出力へマーカーを残さない）
 * 原文の連続した断片の中に全体が在る置換したトークンを原文のテキストへ戻し，マーカーを戻す
 * コメントを除いて不連続に為ったテキストは，断片毎に此の関数でデコードしてから繋ぐ（断片を跨いで位置を推定しない）
 * 計算量：断片の長さ N，中の置換数 R，記録数 M，座標の逆変換の総費用 Q に対し O(N + R + log(M + 1) + Q)
 * @param Src マーカーを置いた原文の解析結果
 * @param Text 解析用の本文の連続した断片のテキスト
 * @param Start 解析用の本文に於ける断片の始まり
 * @param IsCodeText 原文のコード行として比較し，解析用に挿した括弧を戻すか
 * @return 本来のトークンのテキスト
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::string ParseAdapter::DecodeLexeme(const ParsedTree &Src, std::string Text, const uint32_t Start, const bool IsCodeText) {
	// 原表記へ戻すトークンデータの取得
	const std::vector<LexemeRestoration> &Restorations = Src.GetLexemeRestorations();
	const uint32_t End = Start + static_cast<uint32_t>(Text.size());
	const std::vector<LexemeRestoration>::const_iterator First =
	std::ranges::lower_bound(Restorations, Start, std::less<uint32_t>(), &LexemeRestoration::Start);
	const bool HasInsertedParens = IsCodeText &&
	(!Src.GetSyntheticGroups().empty() || !Src.GetReading().TrailingDataMarker.empty() && !Src.PreparationEdits.empty());
	// 復元するトークンと解析用の括弧の無い断片の返戻
	if((First == Restorations.end() || First->Start >= End) && !HasInsertedParens) return DecodeMarks(Src, std::move(Text));
	// 連続した断片を前進して合成し，多数の復元毎に同じ接尾部を移さない
	std::string Restored;
	Restored.reserve(Text.size());
	// 行比較だけでは，原文で幅を持たない解析用の括弧を除く（投影のトークンは保つ）
	const auto Append = [&](const uint32_t Begin, const uint32_t Limit) -> void {
		if(!HasInsertedParens) Restored.append(Text, Begin - Start, Limit - Begin);
		else for(uint32_t At = Begin; At < Limit; ++At) {
			const char Token = Text[At - Start];
			if((Token == '(' || Token == ')') && Src.OffsetInInput(At) == Src.OffsetInInput(At + 1)) continue;
			Restored += Token;
		}
		// 終了
		return;
	};
	uint32_t Pos = Start;
	for(std::vector<LexemeRestoration>::const_iterator Cur = First; Cur != Restorations.end(); ++Cur) {
		if(Cur->Start >= End) break;
		if(Cur->End > End) continue;
		Append(Pos, Cur->Start);
		Restored += Cur->Original;
		Pos = Cur->End;
	}
	Append(Pos, End);
	// マーカーを戻したトークンの返戻
	return DecodeMarks(Src, std::move(Restored));
}

/**
 * マクロの本体のトークンの範囲の取得関数（整形仕様 4.2，トークン間空白仕様 5.4）
 * 文字・文字列のリテラルとコメントの中の空白で分けず，其の外の空白の並びで分けたトークンの範囲を返す（空白の量は読まない）
 * 計算量：本体の長さ N に対し O(N)
 * @param Body マクロの本体のテキスト（連結後の１論理行）
 * @param IsHeaderName ヘッダ名を引数に取るディレクティブの引数か（`<` から `>` 迄も分けない）
 * @return トークンの始まりと終わりの並び（原文順）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::vector<std::pair<uint32_t, uint32_t>> ParseAdapter::MacroBodyTokens(const std::string_view Body, const bool IsHeaderName) {
	// トークンの区切とコメント記法の準備
	std::vector<std::pair<uint32_t, uint32_t>> Tokens;
	const std::string_view BlockOpen = Core::CommentRepository::GetOpenerOf(CommentNotation::Block);
	const std::string_view BlockClose = Core::CommentRepository::GetCloserOf(CommentNotation::Block);
	const std::string_view LineOpen = Core::CommentRepository::GetOpenerOf(CommentNotation::SlashLine);
	// 文字参照を含む空白の判定
	const auto IsBlank = [](const unsigned char Char) -> bool {
		// トークンを区切る空白かの返戻
		return Char == ' ' || Char == '\t' || Char == '\r' || Char == '\n' || Char == '\f' || Char == '\v';
	};
	// 空白の外に在るトークンの範囲の収集
	for(size_t Pos = 0; Pos < Body.size();) {
		if(IsBlank(Body[Pos])) {
			++Pos;
			continue;
		}
		const size_t Start = Pos;
		while(Pos < Body.size() && !IsBlank(Body[Pos])) {
			if(const unsigned char Char = Body[Pos]; Char == '"' || Char == '\'' || IsHeaderName && Char == '<') {
				// リテラル・ヘッダ名の閉じ迄（リテラルのエスケープした字を飛ばす）
				const unsigned char Close = Char == '<' ? '>' : Char;
				for(++Pos; Pos < Body.size() && Body[Pos] != Close; ++Pos) if(Body[Pos] == '\\' && Close != '>') ++Pos;
				Pos = std::min(Pos + 1, Body.size());
			} else if(Body.substr(Pos).starts_with(BlockOpen)) {
				Pos = std::min(Body.find(BlockClose, Pos + BlockOpen.size()), Body.size() - BlockClose.size()) + BlockClose.size();
			} else if(Body.substr(Pos).starts_with(LineOpen)) Pos = Body.size();
			else ++Pos;
		}
		Tokens.emplace_back(static_cast<uint32_t>(Start), static_cast<uint32_t>(Pos));
	}
	// トークンの範囲の返戻
	return Tokens;
}

/**
 * 論理トークンから確定した空のプリプロセッサディレクティブかの判定関数
 * 計算量：デコードデータの数 R とディレクティブの子の数 C に対し O(log(R + 1) + C)
 * @param Src 空のディレクティブのトークンのデコードデータを持つ解析結果
 * @param Node 判定対象のノード
 * @return 空のディレクティブを解析用のトークンへ置き換えたノードなら true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool ParseAdapter::IsNullDirective(const ParsedTree &Src, const TSNode Node) {
	// 空のディレクティブの投影先でない事の返戻
	if(!Src.Is(Node, Core::Role::PreprocCall)) return false;
	// ディレクティブの語に対応する確定済のデコードデータ
	const TSNode Directive = Src.GetFieldChild(Node, Core::ChildRole::DirectiveField);
	const std::vector<LexemeRestoration> &Restorations = Src.GetLexemeRestorations();
	const std::vector<LexemeRestoration>::const_iterator Found =
	std::ranges::lower_bound(Restorations, Src.GetStart(Directive), std::less<uint32_t>(), &LexemeRestoration::Start);
	// トークンの種別を保ち，テキストだけを # へ戻す対応の有無の返戻
	return Found != Restorations.end() && Found->Start == Src.GetStart(Directive) && Found->End == Src.GetEnd(Directive) &&
	!Found->HasKindRestoration && Found->Original == "#";
}

/**
 * マーカーへ置き換えたリンケージ指定の波括弧のディレクティブかの判定関数
 * 計算量：テキスト長 B に対し O(B)
 * @param Src マーカーを置いた原文の解析結果
 * @param Node プリプロセッサディレクティブのノード
 * @return 解析の為に置いた `#pragma <マーカー>O<エンコードしたリテラル><マーカー>` / `#pragma <マーカー>C` なら true（トークンへ分けず１つの葉としてデコードする）
 */
bool ParseAdapter::IsLinkageGuard(const ParsedTree &Src, const TSNode Node) {
	// 対象テキストの取得
	const std::string_view Text = Src.GetView(Node);
	const unsigned char Marker = Src.GetAnalysisMark();
	const std::string_view Head = Src.GetReading().LinkageMarkerDirective;
	// マーカーのディレクティブかの返戻
	return Marker && !Head.empty() && Text.starts_with(Head) && Text.size() > Head.size() + 1 && Text[Head.size()] == Marker &&
	(Text[Head.size() + 1] == 'O' || Text[Head.size() + 1] == 'C');
}

/**
 * 完成候補の埋込の終端の保持の判定関数
 * 独立した検証が HTML のトークン化の状態を確かめ，原文保持への置換では救済しない
 * 計算量：中身の長さ N に対し O(N)
 * @param Src 完成候補を独立して解析した HTML
 * @param Embed 完成候補の埋込
 * @return 終了タグの位置で要素が閉じるなら true
 */
bool ParseAdapter::HasPreservedEmbeddedEnd(const ParsedTree &Src, const EmbeddedContent &Embed) {
	// 親の文法で確定した開始・終了タグと，其の間のトークン化の終端の照合
	const TSNode Open = ts_node_named_child(Embed.Element, 0);
	const TSNode Close = Src.GetFirstNamedChild(Embed.Element, Core::Role::MarkupClose);
	// 欠落したタグ又は本文に吸われた終端を持たず，保持境界を確定出来る事の返戻
	return !ts_node_is_null(Open) && Src.Is(Open, Core::Role::StartTag) && !ts_node_has_error(Open) && !ts_node_is_null(Close) &&
	!ts_node_has_error(Close) && !ts_node_is_missing(Close) && (
		ts_node_is_null(Embed.RawText) ||
		HasPreservedRawTextEnd(Src.GetReading(), Src.GetView(Embed.RawText), Src.Is(Embed.Element, Core::Role::EmbeddedScriptElement))
	);
}

/**
 * ドキュメント順の埋込の中身の列挙関数（整形と配置が同じ順で埋込を数える，整形仕様 4.4）
 * script / style を，中身の無い物と言語を確定出来ない物も含めて集める
 * 計算量：ノード数 N に対し O(N)
 * @param Src 解析済の HTML
 * @return ドキュメント順の埋込の中身
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::vector<ParseAdapter::EmbeddedContent> ParseAdapter::EmbeddedContents(const ParsedTree &Src) {
	// 所有要素毎の埋込内容の収集準備
	std::vector<EmbeddedContent> Contents;
	WalkChildrenCursor(
		Src.GetRoot(),
		[&](const TSNode Node) -> bool {
			if(Src.Is(Node, Core::Role::CommentNode) || Src.Is(Node, Core::Role::ContentElement) && IsRawContentElement(Src, Node)) {
				// ドキュメントの内容である HTML のコメントと，内容を文字として読む要素の内側へ降りない事の返戻
				return false;
			}
			// script / style 以外の子孫を走査し続ける事の返戻
			if(!Src.Is(Node, Core::Role::EmbeddedCodeElement)) return true;
			Contents.push_back({ Node, Src.GetFirstNamedChild(Node, Core::Role::RawTextNode), HtmlEmbeddedLang(Src, Node) });
			// script / style の内側へ降りない事の返戻
			return false;
		}
	);
	// 埋込の中身の返戻
	return Contents;
}

/**
 * 独立して整形した埋込の出力を置く所有領域の生成関数（整形仕様 4.4，親の原文へ書き戻さず投影が所有領域とする）
 * 完成テキストとコメントは渡さず，親の中核は子の変更不能なコードだけを取得する
 * 親の配置後の中身は，子の期待構文と保護領域で独立して再検証する
 * 計算量：O(1)
 * @param Code 子の確定したコード・断片・配置枠
 * @return 子のコードだけを持つ所有領域
 */
Core::OwnedRegion ParseAdapter::OwnEmbeddedOutput(std::shared_ptr<const LaidOutCode> Code) {
	// 子のコードだけを親へ渡す所有領域の返戻（完成候補の終端は独立した検証が確かめる）
	return { std::move(Code) };
}

/**
 * 空の実引数の括弧をパーサーがエラーと読んだかの判定関数
 * 計算量：直接の子数 C に対し O(C)
 * @param Src 判定対象を含む解析済のソース
 * @param Node 判定対象のエラーノード
 * @param Parent Node の直接の親
 * @return 空の実引数のインクルードに実在する丸括弧を持つ場合だけ true
 */
bool ParseAdapter::IsEmptyArgError(const ParsedTree &Src, const TSNode Node, const TSNode Parent) {
	if(!Src.GetReading().CanMisreadRuleArgs || ts_node_is_null(Parent) || !Src.Is(Parent, Core::Role::IncludeStmtNode)) {
		// 空括弧を誤読する文法のインクルード以外は許容しない事の返戻
		return false;
	}
	// 空括弧の実トークンの判定の返戻
	return HasEmptyArgTokens(Src, Node);
}

/**
 * 解析用の入力の適応関数（整形仕様 3.4 の Adapt，4.2）
 * テキストの段の可逆な保護・解析・共有ヘッダの文法の選直・トークン化前のエスケープの展開・パーサーの読違の修復を入力側で完結させる
 * 試行解析の結果は，確定した入力と文法が一致する限り後の工程が再利用する
 * 計算量：原文長 B と準備・解析の総費用 Q に対し O(B + Q)
 * @param Profile 入力で選んだ文法
 * @param Normalized 正規化したテキストと入力の指定
 * @return 適応の結果（`.h` の中身が C++ と判れば言語を書き換える）
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
ParseInput ParseAdapter::Adapt(const GrammarProfile &Profile, InputNormalizer::NormalizedInput &Normalized) {
	// ソースの読取と適応結果の取得
	ParseInput Input = Read(Profile, Normalized);
	Input.Origin = ParseInput::FormattingSrc{ Normalized.Text };
	// 読取を続けれた入力の修復
	if(Input.Decision == ParseDecision::Continue) CanRepair(Input);
	// 適応した入力の返戻
	return Input;
}

/**
 * 修復前の正規化したテキストの取得関数
 * 計算量：O(1)
 * @return 入力又は完成候補の本文
 */
std::string_view ParseInput::GetSrcText() const {
	// 型で区別したテキストの返戻
	return std::visit(
		[]<typename OriginType>(const OriginType &Origin) -> std::string_view {
			return Origin.Text;
		},
		Origin
	);
}
