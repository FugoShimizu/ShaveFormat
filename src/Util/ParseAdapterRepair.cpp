#include "ParseAdapter.hpp"
#include "../Core/CommentRepository.hpp"
#include "../Profile/GrammarProfile.hpp"
#include "FormatLimitExceeded.hpp"
#include "ParsedTree.hpp"
#include "SyntaxParser.hpp"
#include "TextEdit.hpp"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/** ========== 解析器の読違に対する補修 ========== */
/**
 * 此処の関数は投影がコメントを分離する前に走る（整形仕様 3 の設計要件）
 * 其の時点でコメントは未だ原文の一部の為，コメントの字面を読み書きして良い唯一の工程とする
 * 補修の範囲は言語処理系の読みを解析器へ伝える事に限り，書式の好みで字句を整えない（整形仕様 4.2）
 * JSX の本文の端の空白の読みの印付関数（整形仕様 4.2, 6.6）
 * 本文を先に意味付し，行端の空白と文字参照を原文に無い印へ置き換え，本文の中へ置けない空白の式のコメントを解析器へ渡さずに退避する（最も外の JSX 要素へ所属させ，配置はコメント経路が決める）
 * 印は投影が本来の字句へ復号し，コメントの分離に依って隣の本文を結合しない
 * @param Src 解析済の原稿
 */
void ParseAdapter::MarkJsxTextEdges(ParsedTree &Src) {
	// JSX の本文を持たない原稿の終了
	if(!Src.IsParsed() || !Src.GetReading().HasJsxText) return;
	// 原文に無い私用領域の文字を空白・タブの印に使い，利用者の参照と区別する
	// 半角空白の印は行端に接すると `{" "}` へ開く為，印を其の文字数だけ並べて行分割に開いた後の幅を測らせる
	Src.JsxSpaceMark.clear();
	Src.JsxTabMark.clear();
	for(uint32_t Point = 0XE000; Point < 0XF900 && Src.JsxTabMark.empty(); ++Point) {
		std::string Mark;
		TextEdit::EncodeUtf8(Point, Mark);
		if(Src.find(Mark) != Src.npos) continue;
		if(!Src.JsxSpaceMark.empty()) Src.JsxTabMark = std::move(Mark);
		// 開いた後の幅を持たせる空白の印の組立
		else for(size_t Count = 0; Count < ParsedTree::JsxSpaceMarkChars; ++Count) Src.JsxSpaceMark += Mark;
	}
	std::vector<TextEdit> Edits;
	WalkChildrenCursor(
		Src.GetRoot(),
		[&](const TSNode Node) -> bool {
			// 配置保持対象の JSX 要素の選別
			// 行端の描画が処理系で異なる要素は配置を保持（ネストは個別判定）
			const bool IsRendered = Src.Is(Node, Core::Role::RenderedChildren), IsDisputed = IsRendered && Src.HasDisputedJsxText(Node);
			// 配置を保つ要素も，両処理系が除く改行の前の半角空白とタブは除き，出力の行末の空白の除去と読みを揃える（整形仕様 6.6）
			if(IsDisputed) {
				ForEachNamedChild(
					Node,
					[&Src, &Edits](const TSNode Child) -> void {
						// 本文でない子の場合の終了
						if(!Src.Is(Child, Core::Role::RenderedText)) return;
						const std::string_view Text = Src.View(Child);
						for(size_t Newline = Text.find('\n'); Newline != std::string_view::npos; Newline = Text.find('\n', Newline + 1)) {
							if(const size_t Blank = Text.find_last_not_of(" \t", Newline - 1) + 1; Newline && Blank < Newline) {
								TextEdit::Push(Src.Start(Child) + Blank, Src.Start(Child) + Newline, "", Edits);
							}
						}
					}
				);
			}
			if(IsRendered && !IsDisputed) {
				// 本文の並び（空白１文字の式を含む）の範囲と，其の内の式を挟まない本文の範囲
				uint32_t Begin = 0, End = 0, TextBegin = 0, TextEnd = 0;
				std::string Normalized;
				const auto ReadPoint = [](const std::string_view Value, const size_t Pos) -> std::pair<uint32_t, size_t> {
					// 位置の符号点と其の次の位置（既定は置換文字）
					size_t Next = Pos;
					uint32_t Point = 0XFFFD;
					// 復号出来ないバイトは UTF-8 の復号器が置換文字に換える為，空白に数えない
					if(!TextEdit::DecodeUtf8(Value, Next, Point)) Next = Pos + 1;
					// 符号位置とバイト幅の返戻
					return std::pair{ Point, Next - Pos };
				};
				// 式を挟まない本文１つの表示内容を追記（本文毎に行端の空白を落とす）
				const auto AppendText = [&]() -> void {
					// 本文が無く追記を省く場合の終了
					if(TextBegin == TextEnd) return;
					// 今回の本文範囲
					const std::string_view Text(Src.data() + TextBegin, TextEnd - TextBegin);
					// 表示行毎の有効本文範囲の初期化
					size_t First = 0, Last = 0;
					bool HasText = false, HasLine = false;
					// 改行境界毎の端空白除去と本文連結
					for(size_t Pos = 0; Pos < Text.size();) {
						const auto [Point, Width] = ReadPoint(Text, Pos);
						if(const bool IsLineBreak = Point == '\n' || Point == '\r' || Point == 0X2028 || Point == 0X2029; IsLineBreak) {
							// 内容を持つ行の正規化本文への確定
							if(First != std::string_view::npos && HasText) {
								if(HasLine) Normalized += ' ';
								Normalized.append(Text.substr(First, Last - First));
								HasLine = true;
							}
							First = std::string_view::npos;
							HasText = false;
						} else if(!Src.GetReading().IsJsxSpace(Point)) {
							// 非空白符号を含む本文範囲の更新
							if(First == std::string_view::npos) First = Pos;
							Last = Pos + Width;
							HasText = true;
						}
						Pos += Width;
					}
					// 改行で終わらない最終行の確定
					if(First != std::string_view::npos) {
						if(HasLine) Normalized += ' ';
						Normalized.append(Text.substr(First));
					}
					// 確定済本文範囲の初期化
					TextBegin = TextEnd = 0;
				};
				// 最も外の JSX 要素
				const auto OutermostJsx = [&Src, &Node]() -> TSNode {
					TSNode Top = Node;
					for(
						TSNode Parent = ts_node_parent(Top);
						!ts_node_is_null(Parent) && Src.Is(Parent, Core::Role::RenderedChildren);
						Parent = ts_node_parent(Top)
					) Top = Parent;
					// 最も外の要素の返戻
					return Top;
				};
				// 空白・タブは印から改行境界毎に復元し，其の他は両処理系が保持する文字参照化
				const auto FlushText = [&]() -> void {
					// 溜めた本文の追記
					AppendText();
					// 本文の並びが無く編集しない場合の終了
					if(Begin == End) return;
					if(!Normalized.empty()) {
						// 利用者の端の空白参照も同じ印へ揃え，再整形時の幅を保持
						if(const size_t Amp = Normalized.rfind('&'); Amp != std::string::npos) {
							if(const auto [Length, Blank] = ParsedTree::BlankReference(Normalized, Amp); Length && Amp + Length == Normalized.size()) {
								Normalized.replace(Amp, Length, Src.GetJsxBlankMark(Blank));
							}
						}
						if(const auto [Length, Blank] = ParsedTree::BlankReference(Normalized, 0); Length) {
							Normalized.replace(0, Length, Src.GetJsxBlankMark(Blank));
						}
						// UTF-8 末尾符号の開始位置探索
						size_t Tail = Normalized.size() - 1;
						while(Tail && (static_cast<unsigned char>(Normalized[Tail]) & 0XC0) == 0X80) --Tail;
						// 先頭・末尾の空白符号の安定した表現への置換
						for(const size_t Pos : { Tail, size_t(0) }) {
							const auto [Point, Width] = ReadPoint(Normalized, Pos);
							if(Point == ' ' || Point == '\t') Normalized.replace(Pos, Width, Src.GetJsxBlankMark(static_cast<char>(Point)));
							else if(Src.GetReading().IsJsxSpace(Point)) Normalized.replace(Pos, Width, "&#" + std::to_string(Point) + ";");
						}
					}
					// 原文と異なる正規化範囲の編集登録
					if(Normalized != std::string_view(Src.data() + Begin, End - Begin)) TextEdit::Push(Begin, End, std::move(Normalized), Edits);
					// 次の本文並びに備えたバッファ初期化
					Normalized.clear();
					// 並び範囲の初期化
					Begin = End = 0;
				};
				// 半角空白だけの文字列の式 (`{" "}` / `{"  "}`) の空白の数（其れ以外の子は 0）
				const auto SpaceExpressionWidth = [&Src](const TSNode Child) -> size_t {
					// 式でない子の返戻
					if(!Src.Is(Child, Core::Role::EmbeddedExpression)) return 0;
					// 唯一の名前付非コメント子が半角空白だけの文字列か判定（タブは描画が異なる為に除外）
					uint32_t Values = 0;
					size_t Width = 0;
					// 名前付の値からコメントを除いて空白文字列幅を収集
					ForEachNamedChild(
						Child,
						[&](const TSNode Value) -> void {
							// JSX 式内のコメントを値に数えない場合の終了
							if(Src.Is(Value, Core::Role::Comment)) return;
							++Values;
							// 値の原文字列
							const std::string_view Text = Src.View(Value);
							// 値の引用文字列条件
							const bool IsQuoted = Text.size() > 2 && (Text.front() == '"' || Text.front() == '\'') && Text.back() == Text.front();
							Width = IsQuoted && Text.find_first_not_of(' ', 1) == Text.size() - 1 ? Text.size() - 2 : 0;
						}
					);
					// 空白だけの文字列を唯一の値に持つ式の空白の数の返戻
					return Values == 1 ? Width : 0;
				};
				// 改行を含む空白だけの隙間も置換範囲へ含め，非表示の改行の有無で出力を変えない
				const auto BlankGap = [&Src](const uint32_t From, const uint32_t To) -> bool {
					// 子間の空白だけの隙間の判定
					const std::string_view Gap(Src.data() + From, To - From);
					// 改行を含む空白だけの隙間かの返戻
					return From && From < To && Gap.find_first_not_of(" \t\r\n") == std::string_view::npos &&
					Gap.find('\n') != std::string_view::npos;
				};
				uint32_t PrevEnd = 0;
				bool IsTrailingSpace = false;
				// 文字参照と空白の式 (`{" "}`) は隣接本文と同じ文字列として扱い，式・要素の境界毎に JSX の行端空白を確定
				ForEachNamedChild(
					Node,
					[&](const TSNode Child) -> void {
						// JSX 子の幅と先行範囲の取得
						const bool IsText = Src.Is(Child, Core::Role::RenderedText);
						const size_t Width = IsText ? 0 : SpaceExpressionWidth(Child);
						const uint32_t ChildStart = Src.Start(Child), Before = PrevEnd;
						PrevEnd = Src.End(Child);
						// 表示本文でない子に於ける現在範囲の確定
						if(!IsText && !Width) {
							if(IsTrailingSpace && BlankGap(End, ChildStart)) End = ChildStart;
							FlushText();
							// 本文の並びの境界の終了
							return;
						}
						// 隣接本文又は空白式を含む範囲の拡張
						if(Begin == End) Begin = Width && BlankGap(Before, ChildStart) ? Before : ChildStart;
						End = PrevEnd;
						IsTrailingSpace = Width;
						// 本文と空白式の各蓄積先への振分
						if(IsText) {
							if(TextBegin == TextEnd) TextBegin = Src.Start(Child);
							TextEnd = End;
						} else {
							// 前の本文確定後に於ける式の空白の逐語追加
							AppendText();
							Normalized.append(Width, ' ');
							// 本文の中に置く所の無い吸収した式のコメントの，最も外の JSX 要素への退避（置けるかと配置はコメント経路が決める，整形仕様 7.7）
							ForEachNamedChild(
								Child,
								[&](const TSNode Comment) -> void {
									// コメントでない子の場合の終了
									if(!Src.Is(Comment, Core::Role::Comment)) return;
									// 原文でのコメントの種類（行末の物は要素の終わりの後，其の他は要素の前へ所属させる）
									const CommentKind Kind = Src.KindOf(Comment);
									const TSNode Top = OutermostJsx();
									Src.EscapedComments.push_back(
										{ Kind == CommentKind::Trailing ? Src.End(Top) : Src.Start(Top), std::string(Src.View(Comment)), Kind }
									);
								}
							);
						}
					}
				);
				FlushText();
			}
			// コメント本文の内側には降りず，構文要素の本文だけを走査する事の返戻
			return !Src.Is(Node, Core::Role::Comment);
		}
	);
	// 全要素範囲の確定後に於ける後ろ向き編集の一括適用
	TextEdit::Apply(Src, Edits);
	// 終了
	return;
}

/**
 * 改行を含む囲みコメントの行の区切の補修関数（整形仕様 4.2）
 * JS / TS・Swift の処理系は改行を含む囲みコメントを行の区切と読むが，解析器は読まない為，同じ行のコードが続くコメントの後へ改行を補う
 * JS / TS は文の並びの直下と，改行で読みの変わる字句の間（`return` の後，後置の `++` の前等）だけに補い，他の位置の読みを変えない
 * 補った改行は解析用の文の区切として控え，コメントの種類と原文の間隙から除く
 * @param Src 解析済の原稿
 */
void ParseAdapter::RepairCommentLineBreaks(ParsedTree &Src) {
	std::vector<TextEdit> Edits;
	WalkChildrenCursor(
		Src.GetRoot(),
		[&Src, &Edits](const TSNode Node, const TSNode Parent) -> bool {
			// コメントでない節点の子を辿る事の返戻
			if(!Src.Is(Node, Core::Role::Comment)) return true;
			const std::string_view Text = Src.View(Node);
			const bool HasLineBreak = Text.starts_with(Core::CommentRepository::OpenerOf(CommentNotation::Block)) && std::ranges::any_of(
				Src.GetReading().CommentLineBreaks,
				[&Text](const std::string_view Break) -> bool {
					// 行の区切の字面を含むかの返戻
					return Text.find(Break) != std::string_view::npos;
				}
			);
			// 改行を含まないコメントの中へ降りない事の返戻
			if(!HasLineBreak) return false;
			const ReadingRules &Reading = Src.GetReading();
			const uint32_t After = TextEdit::SkipSpRight(Src, Src.End(Node), Reading);
			const bool IsBreaking = !Reading.HasRestrictedLineWords || Src.Is(Parent, Core::Role::StatementOrMemberSequence) ||
			Src.BreaksReadingAt(TextEdit::SkipSpLeft(Src, Src.Start(Node), Reading), After);
			// 同じ行にコードが続き，改行で読みが変わる改行を含む囲みコメントの後への改行の追加
			if(IsBreaking && After < Src.size() && Src[After] != '\n') TextEdit::Push(Src.End(Node), Src.End(Node), "\n", Edits);
			// コメントの中へ降りない事の返戻
			return false;
		}
	);
	// 補った改行の一括適用
	if(!Edits.empty()) TextEdit::Apply(Src, Edits, true);
	// 終了
	return;
}

/**
 * CSS / SCSS の選択子のコメントの後の結合子の補修関数（整形仕様 4.2）
 * 処理系はコメントの前後の空白を子孫結合子と読み（Sass は選択子の中のコメント其の物も空白と読む），解析器はコメントに密着した後の選択子を前の選択子へ繋げる為，コメントの後へ空白を補う
 * @param Src 解析済の原稿
 * @param CommentActsAsSpace 選択子のコメント其の物を空白として読むか
 */
void ParseAdapter::RepairCssSelectorComments(ParsedTree &Src, const bool CommentActsAsSpace) {
	std::vector<TextEdit> Edits;
	WalkChildrenCursor(
		Src.GetRoot(),
		[&Src, &Edits, CommentActsAsSpace](const TSNode Node, const std::span<const TSNode> Ancestors) -> bool {
			// コメントでない節点の子を辿る事の返戻
			if(!Src.Is(Node, Core::Role::Comment)) return true;
			bool IsInSelectors = false;
			for(size_t Up = Ancestors.size(); Up-- && !IsInSelectors && !Src.Is(Ancestors[Up], Core::Role::ScopeBody);) {
				IsInSelectors = Src.Is(Ancestors[Up], Core::Role::SelectorList);
			}
			const uint32_t Start = Src.Start(Node), End = Src.End(Node);
			const bool IsSpaceBefore = Start && std::isspace(static_cast<unsigned char>(Src[Start - 1]));
			const bool IsGluedAfter =
			End < Src.size() && !std::isspace(static_cast<unsigned char>(Src[End])) && Src[End] != ',' && Src[End] != '{';
			// 空白と読む境界のコメントに密着した後の選択子の前への空白の追加
			if(IsInSelectors && IsGluedAfter && (IsSpaceBefore || CommentActsAsSpace)) TextEdit::Push(End, End, " ", Edits);
			// コメントの中へ降りない事の返戻
			return false;
		}
	);
	// 補った空白の一括適用
	if(!Edits.empty()) TextEdit::Apply(Src, Edits);
	// 終了
	return;
}

/**
 * Ruby の終端の無い範囲の次の行の結合関数
 * 範囲演算子で行が終わると解析器が次の行を別の文と読む為，改行を空白へ畳んで一つの式に読ませる（整形仕様 4.2）
 * コメントが間に在ると畳んだ行の残りが其の本文へ入る為，間のコメントは解析用の入力から退避してから畳む
 * 計算量：節点数 N に対し O(N)
 * @param Src ソースコード
 */
void ParseAdapter::FoldRubyOpenRanges(ParsedTree &Src) {
	// 解析出来ない原稿の返戻
	if(!Src.IsParsed()) return;
	const std::vector<std::string_view> &ClauseWords = Src.GetReading().ClauseWords;
	std::vector<TextEdit> Folds;
	WalkAst(
		Src.GetRoot(),
		// 二項式候補の処理
		[&](const TSNode Node) -> void {
			// 範囲でない節点の場合の終了
			if(!Src.Is(Node, Core::Role::Range)) return;
			// 式末尾の演算子節点
			const TSNode Operator = ts_node_child(Node, ts_node_child_count(Node) - 1);
			// 末尾が範囲の演算子でない場合の終了
			if(ts_node_is_named(Operator) || !Src.Is(Operator, Core::Role::RangeOperator)) return;
			const uint32_t After = TextEdit::SkipSpRight(Src, Src.End(Operator), Src.GetReading());
			// 演算子の後が改行でない場合の終了
			if(After >= Src.size() || Src[After] != '\n') return;
			// 次の実引数開始候補
			uint32_t Next = After;
			// 間に在る独立コメントの本文（畳むと行の残りが其の本文へ入る為，解析器へ渡さずに退避する）
			std::vector<std::string_view> Between;
			// 空白と独立コメントを越えて実引数先頭へ移動
			while(Next < Src.size()) {
				// 空白と改行の読飛
				if(Src[Next] == '\n' || Src[Next] == ' ' || Src[Next] == '\t') {
					++Next;
					continue;
				}
				// 其の位置で始まる節点（ヒアドキュメントの本文の `#` をコメントと取違えない）
				const TSNode At = ts_node_named_descendant_for_byte_range(Src.GetRoot(), Next, Next + 1);
				// コメント以外に達した事の脱出
				if(Src[Next] != '#' || ts_node_is_null(At) || !Src.Is(At, Core::Role::Comment)) break;
				// 其の行の終わり
				const uint32_t LineEnd = static_cast<uint32_t>(std::min(Src.find('\n', Next), Src.size()));
				Between.push_back(std::string_view(Src.data() + Next, LineEnd - Next));
				Next = LineEnd;
			}
			// 次の実引数が無いか閉じ括弧・区切りの場合の終了
			if(Next >= Src.size() || std::string_view(")]};").find(Src[Next]) != std::string_view::npos) return;
			uint32_t WordEnd = Next;
			// 実引数先頭語の終端迄走査
			while(WordEnd < Src.size() && IsIdentifierChar(Src[WordEnd])) ++WordEnd;
			// 実引数の先頭語が節の語の場合の終了
			if(std::ranges::find(ClauseWords, std::string_view(Src.data() + Next, WordEnd - Next)) != ClauseWords.end()) return;
			// 此の範囲の編集を取消す為の控え
			const size_t Kept = Folds.size();
			// 演算子後の改行の除去（逐語の区画では後段が空白を詰めない為，此処で詰める）
			// 次が `.` の場合だけ空白を残す（`..` と繋がると別の演算子に為る）
			TextEdit::Push(Src.End(Operator), Next, Src[Next] == '.' ? " " : "", Folds);
			// 逐語の区画（複数行の文字列・ヒアドキュメント・正規表現）の中なら，退避した行が本文の一部に為る為に畳まない
			if(!Between.empty()) for(TSNode Up = ts_node_parent(Node); !ts_node_is_null(Up); Up = ts_node_parent(Up)) {
				// 逐語で無い祖先の読飛
				if(!Src.Is(Up, Core::Role::LiteralInterior) && !Src.Is(Up, Core::Role::Regex)) continue;
				// 此の範囲の編集の取消（畳まずに原形を保ち，整形仕様 7.7 の複数行リテラルへ混入させない）
				Folds.resize(Kept);
				// 逐語の区画の中の場合の終了
				return;
			}
			// 独立コメントの畳んだ位置への退避（種別を変えず，置けるかと配置はコメント経路が決める，整形仕様 7.6）
			for(const std::string_view Text : Between) {
				Src.EscapedComments.push_back({ Src.End(Operator), std::string(Text), CommentKind::OwnLine });
			}
		}
	);
	// 範囲式の改行と構文木の整合
	if(!Folds.empty()) TextEdit::Apply(Src, Folds);
	// 終了
	return;
}

/**
 * Ruby の曖昧な実引数の読みの統一関数
 * Ruby の空白で実引数に為る式を構文木へ反映する
 * tree-sitter-ruby は其の一部を二項式・添字・範囲・受信子と読み，整形で空白を揃えると読みが変わる為，他の編集より前に読みを Ruby へ揃える
 * 必須の括弧は実引数列全体へ移し，内側の整理を後段へ委ねる
 * @param Src ソースコード
 */
void ParseAdapter::RepairRubyCommandArguments(ParsedTree &Src) {
	const auto IsBlank = [&Src](const uint32_t Pos) -> bool {
		// 指定位置が空白かの返戻
		return Pos < Src.size() && (Src[Pos] == ' ' || Src[Pos] == '\t' || Src[Pos] == '\n');
	};
	const auto ArgumentTop = [&Src](TSNode Top) -> TSNode {
		// 所有する最上位の実引数節点への遡及
		for(
			// 一段上の候補節点
			TSNode Up = ts_node_parent(Top);
			!ts_node_is_null(Up) && Src.Is(Up, Core::Role::CommandArgumentExtent);
			Up = ts_node_parent(Up)
		) {
			if(
				// 所有範囲を越える祖先で探索終了
				!ts_node_eq(ts_node_named_child(Up, 0), Top) ||
				Src.Is(Src.FieldChild(Up, Core::ChildRole::OperatorField), Core::Role::LowPrecedenceBinaryKeyword)
			) break;
			// 実引数所有元を一段上へ更新
			Top = Up;
		}
		// 最上位の実引数節点の返戻
		return Top;
	};
	const auto ArgumentEnd = [&Src, &ArgumentTop](const TSNode Node) -> uint32_t {
		// 最上位引数と所属引数列の取得
		const TSNode Top = ArgumentTop(Node), List = ts_node_parent(Top);
		if(!ts_node_is_null(List) && Src.Is(List, Core::Role::ElementSequence)) {
			// 実引数列の最終引数終端の返戻
			return Src.End(ts_node_named_child(List, ts_node_named_child_count(List) - 1));
		}
		// 単独引数の終端の返戻
		return Src.End(Top);
	};
	const auto Wrap = [](const uint32_t From, const uint32_t To, const uint32_t End, std::vector<TextEdit> &Into) -> void {
		// 実引数前への開き括弧挿入
		TextEdit::Push(From, To, "(", Into);
		TextEdit::Push(End, End, ")", Into);
	};
	// 終端の無い範囲式の行結合
	FoldRubyOpenRanges(Src);
	// 本文を誤編集しない様，ヒアドキュメントの開始だけを包んで誤読解消迄再解析
	while(true) {
		const ParsedTree::RubyLocals Locals = Src.IndexRubyLocals(Core::Role::CommandArgumentHost);
		std::vector<TextEdit> Edits, Heredoc;
		WalkAst(
			Src.GetRoot(),
			// Ruby 式候補の処理
			[&](const TSNode Node) -> void {
				// 命令の実引数を持つ節点でない場合の終了
				if(!Src.Is(Node, Core::Role::CommandArgumentHost)) return;
				// 括弧付実引数を持つ候補の場合
				if(Src.Is(Node, Core::Role::ArgumentListHost)) {
					const bool IsKeyword = Src.Is(Node, Core::Role::ValueTakingJump), IsReceiverCall = Src.Is(Node, Core::Role::ReceiverCall);
					const TSNode Args = IsReceiverCall ? Src.FieldChild(Node, Core::ChildRole::Arguments) : ts_node_named_child(Node, 0);
					// 実引数を持たない場合の終了
					if(ts_node_is_null(Args) || !Src.Is(Args, Core::Role::ArgumentSequence) || !ts_node_named_child_count(Args)) return;
					// 実引数列の子数と範囲
					const uint32_t Count = ts_node_named_child_count(Args), Begin = Src.Start(Args), End = Src.End(Args);
					const TSNode First = ts_node_named_child(Args, 0);
					// 明示括弧を持つ実引数列の場合
					if(Src.Start(First) != Begin) {
						// return の単一の実引数でない物と改行付の括弧を保つ場合の終了（此の後で最初の実引数の括弧を外し，曖昧な後続は共に包む）
						if(!IsKeyword || Count != 1 || std::memchr(Src.data() + Begin, '\n', End - Begin)) return;
						// 括弧除去後も先頭が曖昧でない場合
						if(!Src.LeadsRubyAmbiguously(First)) {
							TextEdit::Push(Begin, Begin + 1, " ", Edits);
							TextEdit::Push(End - 1, End, "", Edits);
						// 外側の呼出範囲を括弧で保護
						} else if(const TSNode Top = ArgumentTop(Node); !ts_node_eq(Top, Node)) Wrap(Begin, Begin, Src.End(Top), Edits);
						// 終了
						return;
					}
					const TSNode Method = IsReceiverCall ? Src.FieldChild(Node, Core::ChildRole::MethodField) : TSNode{};
					const TSNode Block = IsReceiverCall ? Src.FieldChild(Node, Core::ChildRole::BlockField) : TSNode{};
					const bool IsLocalHead = !ts_node_is_null(Method) && ts_node_is_null(Src.FieldChild(Node, Core::ChildRole::ReceiverField)) &&
					Src.Is(Method, Core::Role::Identifier) && Locals.IsDefined(Node, Src.View(Method), Src.Start(Node));
					// 親の子の１回走査による前兄弟の取得（根からの引直の二乗費用を回避）
					TSNode Name {}, Prev {};
					ForEachChild(
						Node,
						// 実引数直前の呼出名を探索
						[&](const TSNode Child) -> bool {
							// 実引数到達時の直前子記録と現在子への更新
							if(ts_node_eq(Child, Args)) Name = Prev;
							Prev = Child;
							// 実引数の並びへ届いたら走査を打ち切る事の返戻
							return !ts_node_eq(Child, Args);
						}
					);
					// 直前の子が無い場合に，空節点の終端０から原稿を潰さない為の終了
					if(ts_node_is_null(Name)) return;
					if(
						// 裸の実引数列を括弧で保護
						Src[Begin] == '(' && (IsLocalHead || Src.LeadsRubyAmbiguously(First)) ||
						!ts_node_is_null(Block) && Src[Src.Start(Block)] == '{' &&
						Src.Is(ts_node_named_child(Args, Count - 1), Core::Role::GroupingParen)
					) Wrap(Src.End(Name), Begin, End, Edits);
					// return 前置でのラベル誤認防止用空白の挿入
					else if(Src[Begin] == ':' && Src.End(Name) == Begin) TextEdit::Push(Begin, Begin, " ", Edits);
					// 終了
					return;
				}
				const TSNode Head = ts_node_named_child(Node, 0);
				const bool IsBinary = Src.Is(Node, Core::Role::Binary);
				TSNode Next {};
				// 二項式以外の後続子を探索
				if(!IsBinary && !ts_node_is_null(Head)) {
					// 先頭通過済の印
					bool IsAfterHead = false;
					ForEachChild(
						Node,
						// 各子の先頭相対位置を確認
						[&](const TSNode Child) -> void {
							// 先頭通過状態と直後の子の記録
							if(IsAfterHead && ts_node_is_null(Next)) Next = Child;
							if(ts_node_eq(Child, Head)) IsAfterHead = true;
						}
					);
				}
				const TSNode Operator = IsBinary ? Src.FieldChild(Node, Core::ChildRole::OperatorField) : Next;
				// 先頭か演算子が無い場合の終了
				if(ts_node_is_null(Head) || ts_node_is_null(Operator)) return;
				// 先頭型と演算子の字面
				const std::string_view OperatorText = Src.View(Operator);
				// 実引数開始キーワードの場合
				if(Src.Is(Head, Core::Role::ValueTakingJump)) {
					// 実引数を解析済の場合の終了（此の後で return 後の実引数を補修し，+・範囲・数値始まりは括弧化し，他は符号を密着）
					if(ts_node_named_child_count(Head)) return;
					if(
						// 曖昧なキーワード引数を括弧で保護
						const TSNode Right = IsBinary ? Src.FieldChild(Node, Core::ChildRole::Right) : TSNode{};
						OperatorText == Src.GetRules().Operators.Plus || Src.Is(Node, Core::Role::Range) ||
						OperatorText == Src.GetRules().Operators.Minus && !Src.AttachesRubySign(Right)
					) Wrap(Src.End(Head), Src.Start(Operator), ArgumentEnd(Node), Edits);
					// Ruby 実引数開始演算子の場合
					else if(Src.Is(Operator, Core::Role::ArgumentStartToken)) {
						// 呼出名と演算子の間を単一空白化
						TextEdit::Push(Src.End(Head), Src.Start(Operator), " ", Edits);
						// 演算子と右辺を密着
						if(!ts_node_is_null(Right)) TextEdit::Push(Src.End(Operator), Src.Start(Right), "", Edits);
					}
					// 終了
					return;
				}
				// 名前の後に空白が有り演算子の直後に空白が無い形（添字の `[` は前の空白だけで実引数に為る）でない場合の終了
				if(
					!Src.Is(Operator, Core::Role::ArgumentStartToken) || !IsBlank(Src.Start(Operator) - 1) || IsBinary && IsBlank(Src.End(Operator))
				) return;
				// 実引数を取れない呼出先の場合の終了（実引数を取るのは局所変数でない名前か，実引数の無いメソッドの呼出 (`foo.bar +1`)・`yield`・`super`）
				if(
					const bool IsMethod = Src.Is(Head, Core::Role::Identifier) ?
					!Locals.IsDefined(Node, Src.View(Head), Src.Start(Node)) :
					Src.Is(Head, Core::Role::ReceiverCall) ?
					ts_node_is_null(Src.FieldChild(Head, Core::ChildRole::Arguments)) &&
					ts_node_is_null(Src.FieldChild(Head, Core::ChildRole::BlockField)) :
					Src.Is(Head, Core::Role::SuperReference) || Src.Is(Head, Core::Role::YieldValue) && !ts_node_named_child_count(Head);
					!IsMethod
				) return;
				// シフト以外の曖昧な実引数を括弧で保護
				if(!Src.Is(Operator, Core::Role::HeredocAmbiguousShift)) Wrap(Src.End(Head), Src.Start(Operator), ArgumentEnd(Node), Edits);
				// ヒアドキュメントを伴うシフト式を別編集へ収集
				else if(Heredoc.empty()) Wrap(Src.End(Head), Src.Start(Operator), Src.End(ArgumentTop(Node)), Heredoc);
			}
		);
		// ヒアドキュメントの誤読が無くなってから通常の編集を適用
		if(Heredoc.empty()) {
			TextEdit::Apply(Src, Edits);
			// 終了
			return;
		}
		// 本文の範囲を読み直してから残る実引数を判定
		TextEdit::Apply(Src, Heredoc);
	}
}

/**
 * Swift の解析器の読違の補正関数
 * 計算量：原稿の長さ N と挿入数 K に対し挿入位置の参照は O((N + K) log(K + 1))，解析用読出は O(B + R log(K + 1))
 * B は予算内の読取量，R は読出回数で，差分解析自体の費用を別に要する
 * @param Src ソースコード
 * @return コメントの密着した演算子を直し切れなかった見送理由（直し切れれば理由無）
 */
FailureReason ParseAdapter::RepairSwiftMisreads(ParsedTree &Src) {
	const auto OperatorEnd = [](const std::string_view Text, const uint32_t Start) -> uint32_t {
		const std::string_view LineOpen = Core::CommentRepository::OpenerOf(CommentNotation::SlashLine);
		const std::string_view BlockOpen = Core::CommentRepository::OpenerOf(CommentNotation::Block);
		// 演算子終端の初期化と連続記号走査
		uint32_t End = Start + 1;
		while(
			End < Text.size() &&
			(std::string_view("/=-+!*%<>&|^~?").find(Text[End]) != std::string_view::npos || Text[End] == '.' && Text[Start] == '.') &&
			Text.compare(End, LineOpen.size(), LineOpen) && Text.compare(End, BlockOpen.size(), BlockOpen)
		) ++End;
		// 演算子の字の並びの終わりの返戻
		return End;
	};
	std::vector<uint32_t> Insertions;
	const auto Size = [&Src, &Insertions]() -> size_t {
		// 挿入を重ねた本文の長さの返戻
		return Src.size() + Insertions.size();
	};
	const auto CharacterAt = [&Src, &Insertions](const size_t At) -> char {
		const std::vector<uint32_t>::const_iterator Next = std::upper_bound(Insertions.begin(), Insertions.end(), At);
		// 挿入した空白又は原文の字の返戻
		return Next != Insertions.begin() && Next[-1] == At ? ' ' : Src[At - static_cast<size_t>(Next - Insertions.begin())];
	};
	// From より後の演算子内の最初のコメント位置の取得（補修済の前方部分木は飛ばす）
	// 誤りの読みの回復の木の形は実行時の版で変わる為，演算子の字に密着したコメントの開きを字面からも探し，早い方を採る
	const auto FirstGluedComment = [&Src, &Size, &CharacterAt](const TSNode Root, const uint32_t From) -> uint32_t {
		// 走査カーソルと発見位置の初期化
		HeldCursor Cursor(Root);
		uint32_t Opener = 0;
		for(size_t At = std::max<uint32_t>(From, 1); At + 1 < Size(); ++At) {
			if(CharacterAt(At) != '/') continue;
			// `/` 以外の演算子の字の直後のコメントの開きでない所の読飛（`/` の後の物は前から始まるコメントの一部）
			if(
				CharacterAt(At + 1) != '*' && CharacterAt(At + 1) != '/' ||
				std::string_view("=-+!*%<>&|^~?.").find(CharacterAt(At - 1)) == std::string_view::npos
			) continue;
			// 読めたコメントの閉じの後と，読めたコメント・文字列の中の物（補間の中の式を除く）の読飛
			bool IsCode = true;
			for(const uint32_t Probe : { static_cast<uint32_t>(At - 1), static_cast<uint32_t>(At) }) for(
				TSNode Up = ts_node_descendant_for_byte_range(Root, Probe, Probe + 1);
				IsCode && !ts_node_is_null(Up);
				Up = ts_node_parent(Up)
			) {
				if(Src.Is(Up, Core::Role::StringInterpolation)) break;
				IsCode = !Src.Is(Up, Core::Role::Comment) && !Src.Is(Up, Core::Role::OpaqueLiteral);
			}
			if(!IsCode) continue;
			Opener = static_cast<uint32_t>(At);
			break;
		}
		// From 以後の演算子節点の反復探索
		for(bool IsAdvanced = true; IsAdvanced;) {
			if(const TSNode Node = ts_tree_cursor_current_node(&Cursor); Src.Is(Node, Core::Role::UserDefinedOperator)) {
				// 演算子内のコメント開始記号の検索
				const uint32_t Start = ts_node_start_byte(Node), End = ts_node_end_byte(Node);
				size_t At = std::string::npos;
				for(uint32_t Pos = Start; End - Pos > 1; ++Pos) {
					if(CharacterAt(Pos) == '/' && (CharacterAt(Pos + 1) == '/' || CharacterAt(Pos + 1) == '*')) {
						At = Pos - Start;
						break;
					}
				}
				if(!At && End - Start > 1 && CharacterAt(Start + 1) == '/') {
					At = 0;
					while(Start + At < End && CharacterAt(Start + At) == '/') ++At;
					if(Start + At == End) At = std::string::npos;
				}
				// 有効な開始位置の記録（字面から見付けた物より前の時だけ）
				if(At && At != std::string_view::npos) {
					Opener = Opener ?
					std::min(Opener, ts_node_start_byte(Node) + static_cast<uint32_t>(At)) :
					ts_node_start_byte(Node) + static_cast<uint32_t>(At);
					break;
				}
			}
			// From より後に終わる最初の子へ降り，降りれなければ次の兄弟か祖先の次の兄弟へ進む（無ければ走査を終える）
			if(ts_tree_cursor_goto_first_child_for_byte(&Cursor, From) != -1) continue;
			IsAdvanced = ts_tree_cursor_goto_next_sibling(&Cursor);
			while(!IsAdvanced && ts_tree_cursor_goto_parent(&Cursor)) IsAdvanced = ts_tree_cursor_goto_next_sibling(&Cursor);
		}
		// コメントの始まりの返戻
		return Opener;
	};
	// 密着コメント補修の実行
	if(uint32_t Opener = FirstGluedComment(Src.GetRoot(), 0)) {
		// 補修の再解析で解析器が読んだ量の上限（初回の解析を除き，同じ範囲の再読も数える，整形仕様 9.3）
		static constexpr size_t GluedCommentBudget = 1 << 20;
		size_t ReadBytes = 0;
		// 局所補修用解析器の準備
		SyntaxParser Parser;
		// 文法を設定出来ない解析器の初期化失敗の返戻
		if(!Parser.Use(Src.GetProfile().GetLanguage().TsLang())) return FailureReason::ParserInitializationFailed;
		// 編集する構文木と行位置追跡状態の準備
		TSTree *Tree = ts_tree_copy(Src.GetRoot().tree);
		// 直す位置の行（直す位置は前へ進み，挿入は改行を含まない）
		uint32_t Row = 0;
		size_t Scanned = 0, LineStart = 0;
		// 補修前の本文の位置での空白の挿入（直す位置は前へ進むだけの為，其れ迄に挿した数を引けば補修前の位置に為る）
		std::vector<TextEdit> Inserted;
		// 密着位置毎の空白挿入と差分再解析
		for(size_t Count = 0; Opener && ReadBytes < GluedCommentBudget; ++Count) {
			// 演算子と誤読されたコメント開始位置の空白分離
			Insertions.push_back(Opener);
			Inserted.push_back({ static_cast<uint32_t>(Opener - Count), static_cast<uint32_t>(Opener - Count), " " });
			for(size_t At = Scanned; At < Opener; ++At) if(CharacterAt(At) == '\n') {
				++Row;
				LineStart = At + 1;
			}
			Scanned = Opener + 1;
			// 挿入位置からの増分再解析
			const TSPoint Point = { Row, static_cast<uint32_t>(Opener - LineStart) };
			const TSInputEdit Edit = { Opener, Opener, Opener + 1, Point, Point, { Row, Point.column + 1 } };
			ts_tree_edit(Tree, &Edit);
			bool IsExpired = false;
			TSTree *const Next = Parser.Parse(Tree, Src, IsExpired, &ReadBytes, Insertions);
			ts_tree_delete(Tree);
			Tree = Next;
			// 解析１回の期限の超過の見送理由の返戻（整形仕様 9.1）
			if(IsExpired) return FailureReason::ParseTimeExceeded;
			// 構文木を作れない解析器の見送理由の返戻
			if(!Tree) return FailureReason::ParserUnreadable;
			Opener = FirstGluedComment(ts_tree_root_node(Tree), Opener);
		}
		// 解析資源の解放
		ts_tree_delete(Tree);
		// 読取量の上限の内に直し切れない事の返戻
		if(Opener) return FailureReason::GluedCommentsLimit;
		// 補修を完了した挿入だけのソースへの適用（補修前の本文の位置を辿れる様に編集として適用する）
		TextEdit::Apply(Src, Inserted);
	}
	// 見出式補正編集列の準備
	std::vector<TextEdit> Edits;
	// 見出式の括弧による包装
	const auto Wrap = [&Edits](const uint32_t Start, const uint32_t End) -> void {
		TextEdit::Push(Start, Start, "(", Edits);
		TextEdit::Push(End, End, ")", Edits);
	};
	const auto Exposed = [&Src](const TSNode Expression) -> bool {
		// 要素１つの括弧でなく，紛れる式を持つかの返戻
		return (!Src.Is(Expression, Core::Role::TupleExpression) || ts_node_named_child_count(Expression) != 1) &&
		Src.HasBodyLikeExpression(Expression);
	};
	// 節点毎の補正（祖先は走査の道筋から受け，`ts_node_parent` で根から降り直さない）
	const auto Visit = [&](const TSNode Node, const std::span<const TSNode> Ancestors) -> void {
		// 節点型による演算子又は見出文の振分
		if(Src.Is(Node, Core::Role::UserDefinedOperator)) {
			// 演算子の開始位置取得
			const uint32_t Start = Src.Start(Node);
			// 密着した `!` / `?` の後置への分離（参照と２字以上の記号列は誤読を防いで保持）
			// 切り離さない演算子の終了
			if(
				Src.Len(Node) < 2 || Src[Start] != '!' && Src[Start] != '?' || Src[Start + 1] == '!' || Src[Start + 1] == '?' || !Start ||
				std::isspace(static_cast<unsigned char>(Src[Start - 1]))
			) return;
			// 中置の式の左の被演算子へ密着しない演算子の終了
			if(
				const TSNode Parent = Ancestors.empty() ? TSNode{} : Ancestors.back();
				ts_node_is_null(Parent) || !Src.Is(Parent, Core::Role::NamedInfix) || !ts_node_is_named(ts_node_child(Parent, 0)) ||
				Src.End(ts_node_child(Parent, 0)) != Start
			) return;
			// 演算子前後の空白分離編集
			const uint32_t End = std::max(OperatorEnd(Src, Start), Src.End(Node));
			TextEdit::Push(Start + 1, Start + 1, " ", Edits);
			if(End > Start + 1 && End < Src.size() && !std::isspace(static_cast<unsigned char>(Src[End]))) {
				TextEdit::Push(End, End, " ", Edits);
			}
			// 利用者定義の演算子の終了
			return;
		}
		// 波括弧の見出文でない場合の終了
		if(!Src.Is(Node, Core::Role::BracedHeaderStatement)) return;
		// 見出文の誤読範囲検出状態の初期化
		uint32_t MisreadEnd = 0;
		if(!Src.Is(ts_node_child(Node, ts_node_child_count(Node) - 1), Core::Role::IfStatementNode)) {
			// 親の祖先列を辿る見出文範囲の拡張
			size_t ParentNext = Ancestors.size();
			TSNode Child = Node, Parent = ParentNext ? Ancestors[ParentNext - 1] : TSNode{};
			// ネストの if 末尾から外側の見出への遡及
			while(
				!ts_node_is_null(Parent) && Src.Is(Parent, Core::Role::IfStatementNode) &&
				ts_node_eq(ts_node_child(Parent, ts_node_child_count(Parent) - 1), Child)
			) {
				Child = Parent;
				Parent = --ParentNext ? Ancestors[ParentNext - 1] : TSNode{};
			}
			// 後続クロージャ又は呼出接尾辞に依る誤読終端の検出
			if(
				const TSNode Next = ts_node_is_null(Parent) ? TSNode{} : ts_node_next_named_sibling(Parent);
				!ts_node_is_null(Next) && Src.Is(Parent, Core::Role::Error) && Src.End(Child) == Src.End(Parent) &&
				Src.Is(Next, Core::Role::LambdaLiteral)
			) MisreadEnd = Src.End(Parent);
			else if(
				const TSNode Follower = ts_node_next_named_sibling(Child);
				!ts_node_is_null(Follower) && Src.Is(Follower, Core::Role::StrayBlock) && Src[Src.Start(Follower)] == '{' &&
				!std::memchr(Src.data() + Src.End(Child), '\n', Src.Start(Follower) - Src.End(Child))
			) MisreadEnd = Src.End(Child);
			// 呼出の先頭側に連なる祖先から後続クロージャを探索
			else while(!ts_node_is_null(Parent) && ts_node_eq(ts_node_child(Parent, 0), Child)) {
				if(
					const TSNode Suffix = ts_node_child(Parent, ts_node_child_count(Parent) - 1);
					Src.Is(Parent, Core::Role::CallExpression) && Src.Is(Suffix, Core::Role::CallSuffix) &&
					ts_node_named_child_count(Suffix) == 1 && Src.Is(ts_node_named_child(Suffix, 0), Core::Role::LambdaLiteral)
				) MisreadEnd = Src.End(Child);
				Child = Parent;
				Parent = --ParentNext ? Ancestors[ParentNext - 1] : TSNode{};
			}
		}
		// 見出部の式候補走査
		TSNode Previous = {}, Last = {};
		ForEachChild(
			Node,
			[&](const TSNode Child) -> bool {
				// 本体に達した所で走査を打ち切る事の返戻
				if(IsNamedNode(Child) ? Src.Is(Child, Core::Role::Else) : Src.Is(Child, Core::Role::OpeningBraceToken)) return false;
				// 見出語の後に続く式候補の抽出
				if(
					const TSNode Expression = Src.Is(Child, Core::Role::WhereClauseNode) ?
					ts_node_named_child(Child, 1) :
					ts_node_is_named(Child) && !ts_node_is_null(Previous) && !ts_node_is_named(Previous) &&
					Src.Is(Previous, Core::Role::HeaderExpressionLead) ? Child : TSNode{};
					!ts_node_is_null(Expression)
				) {
					if(!ts_node_is_null(Last) && Exposed(Last)) Wrap(Src.Start(Last), Src.End(Last));
					Last = Expression;
				}
				// 次回用の直前子更新
				Previous = Child;
				// 走査を続ける事の返戻
				return true;
			}
		);
		// 最終見出の誤読本体範囲を含む終端設定
		if(MisreadEnd && !ts_node_is_null(Last)) Wrap(Src.Start(Last), MisreadEnd);
		else if(!ts_node_is_null(Last) && Exposed(Last)) Wrap(Src.Start(Last), Src.End(Last));
	};
	HasDescendantWithAncestorsOf(
		Src.GetRoot(),
		[&Visit](const TSNode Node, const std::span<const TSNode> Ancestors) -> bool {
			Visit(Node, Ancestors);
			// 全ての節点を訪ねる迄走査を続ける事の返戻
			return false;
		}
	);
	// 収集した見出式補正の適用
	TextEdit::Apply(Src, Edits);
	// 補正し切った事の返戻
	return {};
}

/**
 * C# のキャストと誤読した二項式の読みの統一関数
 * @param Src ソースコード
 */
void ParseAdapter::RepairCSharpMisreadCasts(ParsedTree &Src) {
	const std::string_view Source = Src;
	const std::string_view BlockOpen = Core::CommentRepository::OpenerOf(CommentNotation::Block);
	const std::string_view BlockClose = Core::CommentRepository::CloserOf(CommentNotation::Block);
	const std::string_view LineOpen = Core::CommentRepository::OpenerOf(CommentNotation::SlashLine);
	bool Candidate = false;
	// 閉じ括弧後の演算子候補を検索
	for(size_t Pos = Source.find(')'); Pos != std::string_view::npos && !Candidate; Pos = Source.find(')', Pos + 1)) {
		// 閉じ括弧直後の走査位置
		size_t Next = Pos + 1;
		// 空白とコメントを越える走査
		while(Next < Source.size()) {
			// 空白文字の読飛
			if(Source[Next] == ' ' || Source[Next] == '\t' || Source[Next] == '\n') ++Next;
			// 囲みコメントの読飛
			else if(Source.substr(Next).starts_with(BlockOpen)) {
				Next = std::min(Source.find(BlockClose, Next + BlockOpen.size()), Source.size() - BlockClose.size()) + BlockClose.size();
			} else if(Source.substr(Next).starts_with(LineOpen)) Next = std::min(Source.find('\n', Next), Source.size());
			// 最初のコード字句で探索終了
			else break;
		}
		// 前置演算子候補を記録
		Candidate = Next < Source.size() && std::string_view("-+*&^").find(Source[Next]) != std::string_view::npos;
	}
	// 誤読するキャストの無い場合の終了
	if(!Candidate) return;
	// キャストと誤読した式の括弧の除去
	std::vector<TextEdit> Edits;
	WalkAst(
		Src.GetRoot(),
		// C# cast 式候補の処理
		[&](const TSNode Node) -> void {
			// cast 式でない場合の終了
			if(!Src.Is(Node, Core::Role::CastExpression)) return;
			const TSNode Type = Src.FieldChild(Node, Core::ChildRole::Type), Value = Src.FieldChild(Node, Core::ChildRole::Value);
			// 型又は前置単項式を欠く候補の場合の終了
			if(ts_node_is_null(Type) || ts_node_is_null(Value) || !Src.Is(Value, Core::Role::PrefixUnaryOperation)) return;
			// 添字の付いた名前（大きさを持つ配列の型に読まれた `d[0]`）と，式として読める型名以外の場合の終了
			if(
				const bool IsIndexed = Src.Is(Type, Core::Role::ArrayTypeSyntax) && HasDescendantOf(
					Type,
					// 配列階数指定子の探索関数
					[&Src](const TSNode Descendant) -> bool {
						// 要素を持つ配列階数指定子かの返戻
						return Src.Is(Descendant, Core::Role::ArrayRank) && ts_node_named_child_count(Descendant);
					}
				); !IsIndexed && (!Src.Is(Type, Core::Role::ExpressionTypeName) || Src.View(Type).back() == '>')
			) return;
			// 値の前置の演算子が二項の演算子を兼ねない本当のキャストの場合の終了
			if(
				const std::string_view Operator = Src.View(ts_node_child(Value, 0));
				Operator.size() != 1 || std::string_view("-+*&^").find(Operator) == std::string_view::npos
			) return;
			TSNode Close = ts_node_next_sibling(Type);
			// 名前付子を越えて閉じ括弧へ移動
			while(!ts_node_is_null(Close) && ts_node_is_named(Close)) Close = ts_node_next_sibling(Close);
			// 閉じ括弧が無い場合の終了
			if(ts_node_is_null(Close) || !Src.Is(Close, Core::Role::ClosingParenToken)) return;
			// cast 式の開き括弧
			const TSNode Open = ts_node_child(Node, 0);
			TextEdit::Push(Src.Start(Open), Src.End(Open), "", Edits);
			TextEdit::Push(Src.Start(Close), Src.End(Close), "", Edits);
		}
	);
	TextEdit::Apply(Src, Edits);
	// 終了
	return;
}

/**
 * JS/TS の改行を跨ぐ継ぎ目の読みの統一関数
 * @param Src ソースコード
 */
void ParseAdapter::RepairJsJoints(ParsedTree &Src) {
	// 対象字句の有無と編集状態の準備
	const std::string &Source = Src;
	const Core::SyntaxRules &Rules = Src.GetRules();
	const std::string_view Let = Rules.Words[static_cast<size_t>(Core::Word::ReassignableBinding)].front();
	const std::string_view Await = Rules.Words[static_cast<size_t>(Core::Word::AwaitName)].front();
	const std::string_view Class = Rules.Words[static_cast<size_t>(Core::Word::ClassHead)].front();
	const bool HasLet = Source.find(Let) != std::string::npos, HasAwait = Source.find(Await) != std::string::npos;
	// let・await・class の何れの語も無い原稿の場合の終了
	if(!HasLet && !HasAwait && Source.find(Class) == std::string::npos) return;
	const ReadingRules &Reading = Src.GetReading();
	// モジュールの印の文の有無（原稿毎に１度だけ求める，`await` の位置に依らない）
	std::optional<bool> IsModule;
	// `await` を演算子と読む所（非同期の関数の中か，関数の外ならモジュールの最上位）かの判定
	const auto IsAwaitOperator = [&Src, &Rules, &IsModule](const TSNode From) -> bool {
		// 最も内の関数の探索
		for(TSNode Up = ts_node_parent(From); !ts_node_is_null(Up); Up = ts_node_parent(Up)) if(Src.Is(Up, Core::Role::FunctionScope)) {
			// 其の関数の非同期の印の有無の返戻
			return HasUnnamedTokenChild(Src, Up, Rules.Words[static_cast<size_t>(Core::Word::AsyncModifier)].front());
		}
		if(!IsModule) {
			// モジュールの印の文の有無の記録
			IsModule = HasChildOf(
				Src.GetRoot(),
				[&Src](const TSNode Child) -> bool {
					// モジュールの印の文かの返戻
					return Src.Is(Child, Core::Role::ModuleStatement);
				}
			);
		}
		// モジュールの印の文の有無の返戻
		return *IsModule;
	};
	// JS 継ぎ目補修の編集列
	std::vector<TextEdit> Edits;
	// 繋ぐ語と次の節点の間のコメント
	std::vector<TSNode> Comments;
	// 繋いだ行の行末へ置く行末コメント（本文・繋いだ次の節点の始まり・行コメントを含むか）と，繋いで改行の消える隙間
	struct JoinTail {
		std::string Text;
		uint32_t From;
		bool IsLineComment;
	};
	std::vector<JoinTail> Tails;
	std::vector<std::pair<uint32_t, uint32_t>> Gaps;
	// 語と次の節点の間を詰め，間のコメントを原文での種類の位置（独立は文の前の行・文中は繋いだ行内・行末は繋いだ行の行末）へ置く
	// 解析器はクラスの成員を囲みコメントの中の改行でも分ける為，成員の間 (`IsMemberGap`) の改行を含む文中コメントは隙間に置けず除く
	const auto Join = [&](const TSNode Head, const TSNode Word, const TSNode Next, const bool IsMemberGap) -> void {
		std::string Moved, InlineText = " ", Tail;
		bool IsTailLineComment = false;
		for(const TSNode Comment : Comments) {
			const std::string_view Text = Src.View(Comment);
			// 後に同じ行の次の節点が続く文中コメント・行にコメントだけが在る独立コメント・語の後で行が終わる行末コメント（行コメントの後へ積む改行を含む物は，行コメントの本文へ続けれず行末に置けない為に除く）
			if(const CommentKind Kind = Src.KindOf(Comment); Kind == CommentKind::Inline) {
				if(!IsMemberGap || Text.find('\n') == std::string_view::npos) InlineText.append(Text).push_back(' ');
			} else if(Kind == CommentKind::OwnLine) Moved.append(Text).push_back('\n');
			else if(!IsTailLineComment || Text.find('\n') == std::string_view::npos) {
				Tail.append(" ").append(Text);
				IsTailLineComment = IsTailLineComment || Text.starts_with(Core::CommentRepository::OpenerOf(CommentNotation::SlashLine));
			}
		}
		// 独立行コメントを見出前へ移動（見出の前に同じ行のコードが在れば改行を補い，其の行の行末コメントにしない）
		if(!Moved.empty()) {
			// 見出の前に同じ行で続くコメントの始まり（間へ入れると其のコメントが行末・独立へ変わる）
			uint32_t At = Src.Start(Head);
			for(
				uint32_t Before = TextEdit::SkipSpLeft(Source, At, Reading);
				Before > 1 && Source[Before - 1] == '/' && Source[Before - 2] == '*';
			) {
				// 位置で終わるコメントの節点
				const TSNode Prev = ts_node_named_descendant_for_byte_range(Src.GetRoot(), Before - 1, Before);
				if(ts_node_is_null(Prev) || !Src.Is(Prev, Core::Role::Comment) || Src.End(Prev) != Before) break;
				At = Src.Start(Prev);
				Before = TextEdit::SkipSpLeft(Source, At, Reading);
			}
			if(TextEdit::IsAfterCode(Source, At, Reading)) Moved.insert(0, 1, '\n');
			TextEdit::Push(At, At, std::move(Moved), Edits);
		}
		// 語と次節点の間を行内本文へ置換
		TextEdit::Push(Src.End(Word), Src.Start(Next), std::move(InlineText), Edits);
		Gaps.emplace_back(Src.End(Word), Src.Start(Next));
		// 行末コメントは全ての繋ぎを集めた後に置く
		if(!Tail.empty()) Tails.push_back({ std::move(Tail), Src.Start(Next), IsTailLineComment });
	};
	WalkAst(
		Src.GetRoot(),
		// JS 節点候補の処理
		[&](const TSNode Node) -> void {
			// 前候補のコメントの初期化
			Comments.clear();
			// クラス本体の場合
			if(Src.Is(Node, Core::Role::ClassMembers)) {
				ForEachNamedChild(
					Node,
					// クラスメンバ候補の処理
					[&](const TSNode Member) -> void {
						// 注記等の余分な子の場合の終了
						if(ts_node_is_extra(Member)) return;
						// 改行前の修飾語を同名の欄へ補修（解析器が ; を読める様，名前を引用符で包む）
						const uint32_t Count = ts_node_child_count(Member);
						// メンバ末尾より前の子を走査
						for(uint32_t Index = 0; Index + 1 < Count; ++Index) {
							const TSNode Modifier = ts_node_child(Member, Index);
							const std::string_view Word = Src.View(Modifier);
							// 次の字句の位置（間のコメントは判定に使わず跨ぐ，改行はコメントの中の物も文を終える）
							uint32_t Following = Index + 1;
							while(Following + 1 < Count && ts_node_is_extra(ts_node_child(Member, Following))) ++Following;
							if(
								// 文字列と誤読された修飾子を復元
								// 修飾子直後の間隙位置
								const uint32_t Gap = Src.End(Modifier);
								std::ranges::find(Reading.SeparatingModifiers, Word) != Reading.SeparatingModifiers.end() &&
								std::memchr(Source.data() + Gap, '\n', Src.Start(ts_node_child(Member, Following)) - Gap)
							) TextEdit::Push(Src.Start(Modifier), Gap, "\"" + std::string(Word) + "\";", Edits);
						}
						// 後続の成員へ掛かる修飾語だけの欄 (`static\nx = 1`) でない場合の終了（此の後で後続へ接続する）
						if(std::ranges::find(Reading.JoiningModifiers, Src.View(Member)) == Reading.JoiningModifiers.end()) return;
						TSNode Next = ts_node_next_sibling(Member);
						Comments.clear();
						// 間のコメントを回収しつつ次節点へ移動
						for(; !ts_node_is_null(Next) && ts_node_is_extra(Next); Next = ts_node_next_sibling(Next)) Comments.push_back(Next);
						// 次の名前付節点へ修飾子を接続
						if(!ts_node_is_null(Next) && ts_node_is_named(Next)) Join(Member, Member, Next, true);
					}
				);
				// クラスの本体の子の走査の終了
				return;
			}
			// 式文でない場合の終了
			if(!Src.Is(Node, Core::Role::ExpressionStatement)) return;
			// 対象節点先頭のキーワード
			const TSNode Word = ts_node_child(Node, 0);
			// 識別子でない語の場合の終了
			if(!Src.Is(Word, Core::Role::Identifier)) return;
			const bool IsLet = HasLet && Src.View(Word) == Let;
			// `let` と，演算子と読む所の `await`（解析器は後の改行で文を終えると読むが，JS / TS は改行を越えて後の束縛・被演算子へ掛ける）以外の場合の終了
			if(!IsLet && !(HasAwait && Src.View(Word) == Await && IsAwaitOperator(Node))) return;
			bool IsClosed = false;
			// キーワード後の兄弟走査
			for(TSNode Child = ts_node_next_sibling(Word); !ts_node_is_null(Child); Child = ts_node_next_sibling(Child)) {
				// 間のコメントを回収
				if(ts_node_is_extra(Child)) Comments.push_back(Child);
				// コードトークン通過を記録
				else IsClosed = true;
			}
			// 節点後の兄弟候補
			TSNode Next = ts_node_next_sibling(Node);
			// 間のコメントを回収しつつ次節点へ移動
			for(; !ts_node_is_null(Next) && ts_node_is_extra(Next); Next = ts_node_next_sibling(Next)) Comments.push_back(Next);
			// 閉じた文と後続の無い文を結合しない場合の終了
			if(IsClosed || ts_node_is_null(Next)) return;
			// `await` は後の式の文へ接続
			if(!IsLet) {
				if(Src.Is(Next, Core::Role::ExpressionStatement)) Join(Node, Word, Next, false);
				// 終了
				return;
			}
			TSNode Lead = Next;
			// 後続節点の最左葉へ降下
			while(ts_node_child_count(Lead)) Lead = ts_node_child(Lead, 0);
			// 識別子（TS の型注釈の付く名前はラベルと誤読する）又は物体リテラル開始の場合
			if(IsNamedNode(Lead) ? Src.Is(Lead, Core::Role::SplitBindingHead) : Src.Is(Lead, Core::Role::OpenBrace)) {
				Join(Node, Word, Next, false); // キーワードと後続節点を接続
			}
		}
	);
	// 繋いだ次の節点の行の最後のコードの後（行末の後続のコメントの前）への行末コメントの配置（隙間は二分探索の為に始まりの順へ並べる）
	std::sort(Gaps.begin(), Gaps.end());
	std::vector<TSNode> Following;
	for(JoinTail &Pending : Tails) {
		const uint32_t At = Src.LineEndAfter(Pending.From, Gaps, Following);
		// 行コメントを積むと其の行に続く改行を含むコメントが行コメントの本文に為る為，其の時は移す方の行末コメントを除く（其の行に元から在るコメントは動かさない）
		if(
			!Pending.IsLineComment || std::none_of(
				Following.begin(),
				Following.end(),
				[&Src](const TSNode Comment) -> bool {
					// 改行を含むコメントかの返戻
					return Src.View(Comment).find('\n') != std::string_view::npos;
				}
			)
		) TextEdit::Push(At, At, std::move(Pending.Text), Edits);
	}
	// JS 継ぎ目編集の適用
	TextEdit::Apply(Src, Edits);
	// 終了
	return;
}

/**
 * JS/TS の矢印関数を被演算子と読んだ誤りの区切り関数
 * 矢印関数は括弧で包まない限り参照・添字・呼出の被演算子に為れず，`() => {}[0]` は言語処理系が拒む
 * 解析器は此れを誤りを立てずに一つの式と読む為，文の区切りを補って読み直させる（整形仕様 4.2）
 * 計算量：節点数 N に対し O(N)
 * @param Src ソースコード
 */
void ParseAdapter::RepairJsArrowChains(ParsedTree &Src) {
	// 区切りの編集列
	std::vector<TextEdit> Edits;
	WalkAst(
		Src.GetRoot(),
		// 節点毎の被演算子の確認
		[&Src, &Edits](const TSNode Node) -> void {
			// 被演算子を先頭子に取らない節点を読飛ばす場合の終了
			if(!Src.Is(Node, Core::Role::OperandHeadedAccess)) return;
			// 先頭子（被演算子）
			const TSNode Object = ts_node_child(Node, 0);
			// 矢印関数でない被演算子を読飛ばす場合の終了
			if(ts_node_is_null(Object) || !Src.Is(Object, Core::Role::ArrowFunctionNode)) return;
			// 被演算子の次の字句
			const TSNode Next = ts_node_child(Node, 1);
			// 次の字句の無い物を読飛ばす場合の終了
			if(ts_node_is_null(Next)) return;
			const uint32_t From = Src.End(Object), To = Src.Start(Next);
			// 改行を挟まない物を読飛ばす場合の終了（自動セミコロン挿入が効かず，原文自体が言語処理系の拒む構文誤り）
			if(std::string_view(Src.data() + From, To - From).find('\n') == std::string_view::npos) return;
			// 矢印関数の後ろで文を終わらせる区切りの追加
			TextEdit::Push(From, From, std::string(Src.GetRules().Spelling(Core::Word::StatementEnd)), Edits);
		}
	);
	TextEdit::Apply(Src, Edits);
	// 終了
	return;
}

/**
 * Kotlin の物体宣言・物体式の本体をラムダと読んだ誤りの読直関数
 * 解析器は閉じの前に改行の無い物体の本体を後置ラムダと読む為，閉じの前へ改行を補って読み直させる
 * 名前付の `object O { … }` と，呼べない物体式 `object : A { … }` の双方を対象とする
 * 計算量：節点数 N に対し O(N)
 * @param Src ソースコード
 */
void ParseAdapter::RepairKotlinObjectBodies(ParsedTree &Src) {
	// 本体の閉じの直前へ挿す改行の収集
	std::vector<TextEdit> Edits;
	const auto SeparateClose = [&Src, &Edits](const TSNode Body) -> void {
		// 子の無い本体の場合の終了
		if(!ts_node_child_count(Body)) return;
		if(
			const TSNode Close = ts_node_child(Body, ts_node_child_count(Body) - 1);
			Src.SpellsToken(Close, Core::Role::ClosingBraceToken) && Src.Start(Close) && Src[Src.Start(Close) - 1] != '\n'
		) Edits.push_back({ Src.Start(Close), Src.Start(Close), "\n" });
	};
	WalkAst(
		Src.GetRoot(),
		[&](const TSNode Node) -> void {
			// 名前付の物体本体をラムダと読んだ箇所の検出
			if(Src.Is(Node, Core::Role::NamedInfix) && ts_node_named_child_count(Node) == 3) {
				const TSNode Object = ts_node_named_child(Node, 0), Name = ts_node_named_child(Node, 1);
				const TSNode Body = ts_node_named_child(Node, 2);
				if(
					IsNamedNode(Object) && Src.Is(Object, Core::Role::ObjectLiteralNode) &&
					Src.View(Object) == Src.GetRules().Words[static_cast<size_t>(Core::Word::ObjectHead)].front() && IsNamedNode(Name) &&
					Src.Is(Name, Core::Role::SimpleName) && IsNamedNode(Body) && Src.Is(Body, Core::Role::LambdaLiteral) &&
					ts_node_is_null(Src.FirstNamedChild(Body, Core::Role::LambdaParameters))
				) SeparateClose(Body);
				// 名前付の中置の式を分けた場合の終了
				return;
			}
			// 二つの子を持つ呼出でない場合の終了
			if(!Src.Is(Node, Core::Role::CallExpression) || ts_node_named_child_count(Node) != 2) return;
			const TSNode Object = ts_node_named_child(Node, 0), Suffix = ts_node_named_child(Node, 1);
			const TSNode Annotated = ts_node_named_child(Suffix, 0), Lambda = ts_node_named_child(Annotated, 0);
			// 成員の無い物体へ注釈付の引数の無いラムダを渡す呼出でない場合の終了
			if(
				!(IsNamedNode(Object) && Src.Is(Object, Core::Role::ObjectLiteralNode)) ||
				!ts_node_is_null(Src.FirstNamedChild(Object, Core::Role::ClassMembers)) || ts_node_named_child_count(Suffix) != 1 ||
				!(IsNamedNode(Annotated) && Src.Is(Annotated, Core::Role::AnnotatedLambda)) || ts_node_named_child_count(Annotated) != 1 ||
				!(IsNamedNode(Lambda) && Src.Is(Lambda, Core::Role::LambdaLiteral)) ||
				!ts_node_is_null(Src.FirstNamedChild(Lambda, Core::Role::LambdaParameters))
			) return;
			// 本体の閉じの前の改行
			SeparateClose(Lambda);
		}
	);
	TextEdit::Apply(Src, Edits);
	// 終了
	return;
}

/**
 * Kotlin の改行を跨ぐ継ぎ目の区切り編集の収集関数
 * Kotlin で文境界を跨ぐ誤結合を防ぐセミコロン補完関数
 * @param Src 整形対象ソース
 * @param Node 呼出の接尾辞・参照・返戻のノード
 * @param Edits 編集列追加先
 */
void ParseAdapter::CollectKotlinJointEdit(const ParsedTree &Src, const TSNode Node, std::vector<TextEdit> &Edits) {
	uint32_t Joint = Src.Start(Node);
	TSNode Lead {};
	// 改行後の `.` / `?.` が連鎖を継続する場合の終了（`::` だけは別文の参照）
	if(Src.Is(Node, Core::Role::NavigationSuffix) && !Src.Is(ts_node_child(Node, 0), Core::Role::DoubleColonToken)) return;
	// 呼出可能参照の場合
	else if(Src.Is(Node, Core::Role::CallableReference)) {
		// 受け手の無い参照と直前式の非接続
		TSNode Colons {};
		bool IsReceiver = false;
		ForEachChild(
			Node,
			// 呼出可能参照の各子を走査
			[&](const TSNode Child) -> bool {
				// `::` トークン又は名前付受け手の検出
				if(!ts_node_is_named(Child) && Src.Is(Child, Core::Role::DoubleColonToken)) {
					Colons = Child;
					// `::` の発見で打ち切る事の返戻
					return false;
				}
				// 名前付の受け手通過を記録
				IsReceiver = IsReceiver || ts_node_is_named(Child);
				// 次の子へ進む事の返戻
				return true;
			}
		);
		// 受け手の無い参照の返戻
		if(!IsReceiver || ts_node_is_null(Colons)) return;
		// 継ぎ目を `::` の開始へ更新
		Joint = Src.Start(Colons);
	// ジャンプ式の場合
	} else if(Src.Is(Node, Core::Role::JumpExpression)) {
		const std::string_view Keyword = Src.View(ts_node_child(Node, 0));
		const TSNode Value = ts_node_named_child(Node, ts_node_named_child_count(Node) - 1);
		// 値を返さない jump の場合の終了
		if(
			Keyword != Src.GetRules().Words[static_cast<size_t>(Core::Word::Return)].front() &&
			Keyword != Src.GetRules().Words[static_cast<size_t>(Core::Word::LabeledReturn)].front() || ts_node_is_null(Value) ||
			Src.Is(Value, Core::Role::JumpLabel)
		) return;
		// 継ぎ目を返戻値の開始へ更新
		Joint = Src.Start(Value);
		// 返戻値を先頭式候補へ設定
		Lead = Value;
	// 中置式の場合
	} else if(Src.Is(Node, Core::Role::NamedInfix)) {
		// Kotlin の改行後の単独呼出の文分離（裸の右辺は元から文でない為に除外）
		const TSNode Name = ts_node_named_child(Node, 1), Right = ts_node_named_child(Node, 2);
		// 呼出の形でない物の返戻
		if(ts_node_is_null(Name) || ts_node_is_null(Right) || Src[Src.Start(Right)] != '(' && Src[Src.Start(Right)] != '{') return;
		// 継ぎ目を中置演算子名へ更新
		Joint = Src.Start(Name);
	} else {
		const TSNode Arguments = ts_node_named_child(Node, 0);
		// 単一の字句を持つ単一の実引数でない場合の終了
		if(
			ts_node_is_null(Arguments) || !Src.Is(Arguments, Core::Role::ValueArguments) || ts_node_named_child_count(Arguments) != 1 ||
			ts_node_child_count(ts_node_named_child(Arguments, 0)) != 1
		) return;
		// 実引数を先頭式候補へ設定
		Lead = Arguments;
	}
	// 継ぎ目の空白遡及による文末改行の確認
	bool HasNewline = false;
	// 継ぎ目前の空白を後退走査
	for(; Joint && (Src[Joint - 1] == ' ' || Src[Joint - 1] == '\t' || Src[Joint - 1] == '\n'); --Joint) {
		// 改行の発見を記録
		if(Src[Joint - 1] == '\n') HasNewline = true;
	}
	// 改行を含まない場合の終了
	if(!HasNewline) return;
	// 文の並びだけで改行を区切にする（実引数・条件・添字等の括弧内と when の条件は除外）
	for(TSNode Cur = Node, Parent = ts_node_parent(Node); !ts_node_is_null(Parent); Cur = Parent, Parent = ts_node_parent(Parent)) {
		// 文境界の祖先で探索終了
		if(Src.Is(Parent, Core::Role::LineTerminatedContext)) break;
		// when の条件の中の場合の終了
		if(Src.Is(Parent, Core::Role::WhenCondition)) return;
		int Depth = 0;
		const uint32_t CurStart = Src.Start(Cur);
		ForEachChild(
			Parent,
			// 対象子までの括弧深さ集計
			[&Src, &Depth, CurStart](const TSNode Child) -> bool {
				// Cur に達した事の返戻
				if(Src.Start(Child) >= CurStart) return false;
				// 括弧字句に応じた深さ更新
				if(
					// 開き括弧で深さを加算
					!ts_node_is_named(Child) && (Src.Is(Child, Core::Role::OpeningBracketToken) || Src.Is(Child, Core::Role::OpeningParenToken))
				) ++Depth;
				// 閉じ括弧で深さを減算
				else if(
					!ts_node_is_named(Child) && (Src.Is(Child, Core::Role::ClosingParenToken) || Src.Is(Child, Core::Role::ClosingSquareToken))
				) --Depth;
				// 次の子へ進む事の返戻
				return true;
			}
		);
		// 括弧の中の場合の終了
		if(Depth > 0) return;
	}
	// 誤って繋がった文の区切りの挿入
	TextEdit::Push(Joint, Joint, std::string(Src.GetRules().Spelling(Core::Word::StatementEnd)), Edits);
	// tree-sitter-kotlin が読めない文頭の冗長括弧の除去
	while(!ts_node_is_null(Lead) && ts_node_child_count(Lead) && !Src.Is(ts_node_child(Lead, 0), Core::Role::OpeningParenToken)) {
		// 先頭子へ移動
		Lead = ts_node_child(Lead, 0);
	}
	// 先頭が単一の子を持たない場合の終了
	if(ts_node_is_null(Lead) || ts_node_named_child_count(Lead) != 1) return;
	// 括弧省略候補の実引数名
	TSNode Name = ts_node_named_child(Lead, 0);
	// 値引数包装を剥離
	if(Src.Is(Name, Core::Role::ValueArgument)) Name = ts_node_named_child(Name, 0);
	// 単純識別子の単独実引数の場合
	if(!ts_node_is_null(Name) && Src.Is(Name, Core::Role::SimpleName)) {
		const TSNode Open = ts_node_child(Lead, 0), Close = ts_node_child(Lead, ts_node_child_count(Lead) - 1);
		TextEdit::Push(Src.Start(Open), Src.End(Open), "", Edits);
		TextEdit::Push(Src.Start(Close), Src.End(Close), "", Edits);
	}
	// 終了
	return;
}

/**
 * Kotlin の改行を跨ぐ継ぎ目の区切り関数
 * tree-sitter-kotlin は改行の後の呼出・添字・参照を前の式へ繋ぐが，Kotlin は改行で文を終える為，継ぎ目の前へ `;` を補って読み直させる
 * 他の編集（括弧除去等）が誤って繋いだ木を元に判断すると，区切った後の巡で結果が変わり冪等性が崩れる為，コメントの分離と正規化より前に行う
 * @param Src ソースコード
 */
void ParseAdapter::RepairKotlinJoints(ParsedTree &Src) {
	// Kotlin 継ぎ目の編集列
	std::vector<TextEdit> Edits;
	WalkAst(
		Src.GetRoot(),
		// Kotlin 節点候補の処理
		[&](const TSNode Node) -> void {
			// 継ぎ目対象接尾辞の編集収集
			if(Src.Is(Node, Core::Role::LineJoiningSuffix)) CollectKotlinJointEdit(Src, Node, Edits);
		}
	);
	TextEdit::Apply(Src, Edits, true);
	// 終了
	return;
}

/**
 * Kotlin の自動の `;` の補修関数
 * @param Src ソースコード
 */
void ParseAdapter::RepairKotlinSemicolons(ParsedTree &Src) {
	// Kotlin 原文への参照
	const std::string &Source = Src;
	const std::string_view BlockOpen = Core::CommentRepository::OpenerOf(CommentNotation::Block);
	const bool HasBlockComment = Source.find(BlockOpen) != std::string::npos;
	// 囲みコメントも解析の誤りも無いソースの終了
	if(!HasBlockComment && !ts_node_has_error(Src.GetRoot())) return;
	std::vector<uint32_t> Inserts;
	uint32_t PrevEnd = static_cast<uint32_t>(Source.size()), CodeEnd = PrevEnd;
	// 前のコードの後に，走査器が呑んで節点に為らなかった囲みコメントが在ったか（其の後の実のコメントの節点を越えて持ち越す）
	bool IsSwallowed = false;
	WalkChildrenCursor(
		Src.GetRoot(),
		// Kotlin 節点の処理
		[&](const TSNode Node) -> bool {
			// 文字列は閉じの引用符が字句に為らず，中の字句の終わりが文字列の中に来る為，全体を１つの字句としての取扱
			if(!Src.Is(Node, Core::Role::StringLiteralNode) && ts_node_child_count(Node)) {
				// 破損したクラス本体の場合
				if(ts_node_has_error(Node) && Src.Is(Node, Core::Role::ClassBodyContainer)) {
					// クラス本体の閉じトークン
					const TSNode Close = ts_node_child(Node, ts_node_child_count(Node) - 1);
					TSNode Last = ts_node_prev_sibling(Close);
					// 末尾コメントを越えて最後のコードへ移動
					while(!ts_node_is_null(Last) && Src.Is(Last, Core::Role::Comment)) Last = ts_node_prev_sibling(Last);
					// 健全な成員末尾だけの区切補完（改行と ; が無い時，誤った成員は保持）
					// 破損メンバ後の挿入位置を記録
					if(
						!ts_node_is_null(Last) && Src.SpellsToken(Close, Core::Role::ClosingBraceToken) &&
						!Src.SpellsToken(Last, Core::Role::OpeningBraceToken) &&
						std::string_view(Source).substr(Src.End(Last), Src.Start(Close) - Src.End(Last)).find_first_of(";\n") ==
						std::string_view::npos && !HasChildOf(
							Node,
							[&Src](const TSNode Child) -> bool {
								const auto IsBroken = [&Src](const TSNode Member) -> bool {
									// 成員でない名前付の子か，誤りを含む成員かの返戻
									return ts_node_is_named(Member) && !Src.Is(Member, Core::Role::Comment) &&
									(!Src.Is(Member, Core::Role::ClassMember) || ts_node_has_error(Member));
								};
								// 健全な成員を包まない `ERROR` か，壊れた子かの返戻
								return Src.Is(Child, Core::Role::Error) ? !ts_node_named_child_count(Child) || HasChildOf(Child, IsBroken) : IsBroken(Child);
							}
						)
					) Inserts.push_back(Src.End(Last));
				}
				// 字句でない節点は子へ降りる事の返戻
				return true;
			}
			// 隙間の後のコメント節点の確認（幅０の節点では本文が隙間に残る）
			const uint32_t Start = ts_node_start_byte(Node);
			IsSwallowed = IsSwallowed || HasBlockComment && PrevEnd < Start &&
			std::string_view(Source).substr(PrevEnd, Start - PrevEnd).find(BlockOpen) != std::string_view::npos;
			PrevEnd = ts_node_end_byte(Node);
			// コード節点なら，呑まれたコメントの後が式を続ける字句（`when` の枝を始める `else` を除く）でなければ其の前のコード終端の挿入候補化と，コード終端の更新
			if(!Src.Is(Node, Core::Role::Comment)) {
				if(
					const std::string_view Token = Src.View(Node);
					IsSwallowed && (
						std::ranges::find(Src.GetReading().LineContinuingTokens, Token) == Src.GetReading().LineContinuingTokens.end() ||
						Src.Is(Node, Core::Role::ElseKeyword) &&
						(IsNamedNode(ts_node_parent(Node)) && Src.Is(ts_node_parent(Node), Core::Role::ValueArm))
					)
				) Inserts.push_back(CodeEnd);
				IsSwallowed = false;
				CodeEnd = PrevEnd;
			}
			// 字句の子へ降りない事の返戻
			return false;
		}
	);
	// 挿入が無い場合の終了
	if(Inserts.empty()) return;
	std::sort(Inserts.begin(), Inserts.end());
	// 重複した挿入位置を除去
	Inserts.erase(std::unique(Inserts.begin(), Inserts.end()), Inserts.end());
	// セミコロン挿入編集列
	std::vector<TextEdit> Edits;
	// 挿入位置数の領域を予約
	Edits.reserve(Inserts.size());
	// 各候補位置へセミコロンを挿入
	for(const uint32_t Pos : Inserts) Edits.push_back({ Pos, Pos, std::string(Src.GetRules().Spelling(Core::Word::StatementEnd)) });
	TextEdit::Apply(Src, Edits, true);
	// 補完後に読めない区間が無く残留 `;` を除かない場合の終了
	if(!Src.IsParsed() || !ts_node_has_error(Src.GetRoot())) return;
	// 誤りを増した挿入の取消編集列
	std::vector<TextEdit> Undo;
	// 各挿入位置の再解析結果を確認
	for(size_t Idx = 0; Idx < Inserts.size(); ++Idx) {
		const uint32_t Pos = Inserts[Idx] + static_cast<uint32_t>(Idx);
		for(
			// 挿入位置を含む最小節点
			TSNode Node = ts_node_descendant_for_byte_range(Src.GetRoot(), Pos, Pos + 1);
			!ts_node_is_null(Node);
			Node = ts_node_parent(Node)
		) {
			// `ERROR` 以外の祖先なら次候補へ移動
			if(!Src.Is(Node, Core::Role::Error)) continue;
			// 誤りを生んだセミコロンの除去編集
			Undo.push_back({ Pos, Pos + 1, "" });
			// 該当位置の祖先探索終了
			break;
		}
	}
	TextEdit::Apply(Src, Undo);
	// 終了
	return;
}

/**
 * コメント分離前に解析器の読違を補修する共通入口
 * 入力と完成候補の再投影で同じ文法上の補修を用いる
 * @param Src 解析済の原稿
 * @return 補修出来ない入力の見送理由，補修出来た場合は理由無
 * @throws FormatLimitExceeded 既知の読違を補修しても正しい木を得れない場合
 */
FailureReason ParseAdapter::RepairBeforeProjection(ParsedTree &Src) {
	// 文法の読取規則に応じた読違の補修
	const ReadingRules &Rules = Src.GetReading();
	if(Rules.MisreadsCommandArgumentBoundary) RepairRubyCommandArguments(Src);
	if(Rules.HasArrowChainMisreads) {
		RepairJsArrowChains(Src);
		RepairJsJoints(Src);
	}
	if(!Rules.CommentLineBreaks.empty()) RepairCommentLineBreaks(Src);
	if(Rules.MisreadsCallContinuation) {
		RepairKotlinSemicolons(Src);
		RepairKotlinJoints(Src);
		RepairKotlinObjectBodies(Src);
	}
	if(Rules.MisreadsCasts) RepairCSharpMisreadCasts(Src);
	if(Rules.MisreadsSelectorCommentBoundary) RepairCssSelectorComments(Src, Rules.SelectorCommentActsAsSpace);
	if(Rules.MisassociatesOperators) {
		// コメントに密着した演算子の読違を上限迄に解消出来ない場合の返戻
		if(const FailureReason Reason = RepairSwiftMisreads(Src); Reason.Value != FailureReason::None) return Reason;
		// 結合を組み直せない演算子の連鎖の見送理由の返戻
		if(const FailureReason Reason = RepairOperatorBindings(Src); Reason.Value != FailureReason::None) return Reason;
	}
	if(Rules.MisbindsConstructionCallee) RepairConstructionBindings(Src);
	if(Rules.MisreadsSoftKeywordCalls) RepairSoftKeywordCalls(Src);
	// 補修を完了した事の返戻
	return {};
}

/**
 * CSS の空のカスタムプロパティ値を偽の整数値と読む誤りの判定関数
 * 解析器は `--x:;` の `;` の前に幅０の integer_value を作り，`;` が無ければ同じ節点を MISSING とする
 * @param Src 解析済の原稿
 * @param Node 判定対象の節点
 * @return 空のカスタムプロパティ値を表す偽の節点なら true
 */
bool ParseAdapter::IsEmptyCssCustomValue(const ParsedTree &Src, const TSNode Node) {
	// 幅０の整数値でない節点の返戻
	if(!Src.Is(Node, Core::Role::IntegerLiteralValue) || Src.Len(Node)) return false;
	const TSNode Declaration = ts_node_parent(Node);
	// 宣言の値でない節点の返戻
	if(ts_node_is_null(Declaration) || !Src.Is(Declaration, Core::Role::TypedDeclaration)) return false;
	const TSNode Property = ts_node_named_child(Declaration, 0);
	const std::string_view Name = ts_node_is_null(Property) ? std::string_view() : Src.View(Property);
	const std::string_view Head = Src.GetReading().CustomPropertyHead;
	// カスタムプロパティの空値だけを解析器の偽の整数から区別する事の返戻
	return !Head.empty() && Src.Is(Property, Core::Role::PropertyNameNode) && Name.size() > Head.size() && Name.starts_with(Head);
}
