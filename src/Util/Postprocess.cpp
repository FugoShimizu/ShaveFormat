#include "Postprocess.hpp"
#include "DocSig.hpp"
#include "HtmlTag.hpp"
#include "NodeKind.hpp"
#include "TextEdit.hpp"
#include "TsSource.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

/**
 * 収集済位置への１文字挿入と再構築関数
 * @param Src 対象のソース（結果で上書きされる）
 * @param Insertions 挿入位置の集合（昇順へ整列される）
 * @param Char 挿入する文字
 */
void Postprocess::InsertCharAt(TSSource &Src, std::vector<uint32_t> &Insertions, const char Char) {
	// 挿入位置が無い場合の返戻
	if(Insertions.empty()) return;
	// 挿入反復を避ける挿入位置の昇順整列と線形連結
	std::sort(Insertions.begin(), Insertions.end());
	const std::string &Source = Src;
	std::string Out;
	Out.reserve(Source.size() + Insertions.size());
	size_t Last = 0;
	// 原文の未処理区間と挿入文字の前方からの交互連結
	for(const uint32_t Off : Insertions) {
		Out.append(Source, Last, Off - Last);
		Out += Char;
		Last = Off;
	}
	// 末尾の未転写範囲の連結
	Out.append(Source, Last, Source.size() - Last);
	// 挿入結果の反映
	Src.Assign(std::move(Out));
	// 終了
	return;
}

/**
 * CSS 最終宣言のセミコロン補完関数
 * @param Src 整形後 TSSource（インプレース編集）
 */
void Postprocess::FixCssMissingSemicolon(TSSource &Src) {
	// 未解析原稿の場合の返戻
	if(!Src.IsParsed()) return;
	// セミコロンの挿入位置列
	std::vector<uint32_t> Insertions;
	const std::string &Source = Src;
	// ブロック内の最後の宣言は原文で `;` を省ける為，補って宣言の並びを揃える（規約の末尾セミコロン）
	WalkAst(
		Src.GetRoot(),
		[&](const TSNode Node) -> void {
			// 名前無字句の型名取得前での除外
			if(!ts_node_is_named(Node) || std::string_view(ts_node_type(Node)) != "declaration") return;
			// 宣言節点の終端位置
			const uint32_t End = ts_node_end_byte(Node);
			// 補完済又は範囲外の宣言の除外
			if(!End || End > Source.size() || Source[End - 1] == ';') return;
			// ブロック末尾直前の宣言だけへのセミコロン補完
			TSNode Next = ts_node_next_sibling(Node);
			// コメントの読飛しと `ERROR` 節点での停止
			while(!ts_node_is_null(Next) && ts_node_is_extra(Next) && std::string_view(ts_node_type(Next)) != "ERROR") {
				// 次の非コメント候補への移動
				Next = ts_node_next_sibling(Next);
			}
			// 最終宣言の終端位置登録
			if(ts_node_is_null(Next) || !ts_node_is_named(Next) && std::string_view(ts_node_type(Next)) == "}") Insertions.push_back(End);
		}
	);
	// 節点範囲を保つ走査後の一括挿入
	InsertCharAt(Src, Insertions, ';');
	// 終了
	return;
}

/**
 * HTML の空要素からの自己終端記号の除去関数
 * @param Src 整形後 TSSource（インプレース編集）
 */
void Postprocess::FixHtmlSelfClosing(TSSource &Src) {
	// 未解析原稿の場合の返戻
	if(!Src.IsParsed()) return;
	// HTML 空要素の `/` を除去（SVG / MathML と外来要素外で名前空間を宣言する XHTML は保持）
	std::vector<std::pair<uint32_t, uint32_t>> Removals;
	bool IsXmlSyntax = false;
	const std::string &Source = Src;
	// 自己終端記号の保持に用いる外来要素祖先の検査
	const auto IsInForeign = [&Src](const TSNode Tag) -> bool {
		// 外来コンテンツの根（タグ自身の要素を含む）の内側に在るかの返戻
		return HasAncestorOf(
			Tag,
			[&Src](const TSNode Ancestor) -> bool {
				// 祖先要素のタグ名
				const std::string_view TagName = HtmlTagName(Src, Ancestor);
				// 外来コンテンツの根に居る事の返戻
				return IsHtmlNameEqual(TagName, "svg") || IsHtmlNameEqual(TagName, "math");
			}
		);
	};
	WalkChildrenCursor(
		Src.GetRoot(),
		[&Src, &Source, &Removals, &IsXmlSyntax, &IsInForeign](const TSNode Node) -> bool {
			// 照合する型は名前付の為，無名字句は型名の取得すらせずに落とす事の返戻
			if(!ts_node_is_named(Node)) return false;
			// 走査中の節点型
			const std::string_view Type = ts_node_type(Node);
			// 内容を文字として読む要素（`<textarea>` 等）の内側は，構文解析器が要素と読んでも文字の為，降りない事の返戻
			if(Type == "element") return HtmlVerbatimOf(Src, Node) != HtmlVerbatim::RawContent;
			// 開始・自己終端タグ以外の子孫を走査する事の返戻
			if(!NodeKind::JsxOrHtmlOpenLike.Contains(Type)) return true;
			// 外来コンテンツを除く名前空間宣言属性の探索
			if(!IsXmlSyntax) {
				ForEachNamedChild(
					Node,
					[&Src, &IsXmlSyntax, &IsInForeign, Node](const TSNode Attribute) -> bool {
						// XML 名前空間属性の照合
						if(
							// 属性の名前
							const std::string_view Name =
							std::string_view(ts_node_type(Attribute)) == "attribute" ? Src.View(ts_node_named_child(Attribute, 0)) : std::string_view{};
							!IsHtmlNameEqual(Name.substr(0, 5), "xmlns") || Name.size() > 5 && Name[5] != ':'
							// 次の属性へ進む事の返戻
						) return true;
						// 外来要素外の名前空間宣言の記録
						IsXmlSyntax = !IsInForeign(Node);
						// 名前空間の宣言で打ち切る事の返戻
						return false;
					}
				);
			}
			// 開始タグの内側へ降りない事の返戻
			if(Type != "self_closing_tag") return false;
			HtmlNameBuffer Buffer;
			// 表記変更対象外の通常要素・外来要素の根・外来要素内の返戻
			if(!NodeKind::HtmlVoidTag.Contains(HtmlLowerName(Src.View(ts_node_named_child(Node, 0)), Buffer)) || IsInForeign(Node)) {
				// 変換対象外の要素の内側へ降りない事の返戻
				return false;
			}
			// 末尾の `/>` と直前空白の `>` への縮約
			if(const uint32_t End = ts_node_end_byte(Node); End > 1 && End <= Source.size() && !Source.compare(End - 2, 2, "/>")) {
				// 直前空白を含む除去範囲の登録
				Removals.emplace_back(TextEdit::SkipSpLeft(Source, End - 2), End - 1);
			}
			// 自己終端タグの内側へ降りない事の返戻
			return false;
		}
	);
	// XML 構文又は除去対象無の場合の返戻
	if(IsXmlSyntax || Removals.empty()) return;
	// 除去反復を避ける除去範囲の昇順整列と線形連結
	std::sort(Removals.begin(), Removals.end());
	std::string Out;
	Out.reserve(Source.size());
	size_t Last = 0;
	for(const std::pair<uint32_t, uint32_t> &Range : Removals) {
		// 除去範囲直前迄の原文転写
		Out.append(Source, Last, Range.first - Last);
		// 除去範囲終端への移動
		Last = Range.second;
	}
	// 末尾の未転写範囲の連結
	Out.append(Source, Last);
	// 全除去範囲の反映後に限る構文木保持元の更新
	Src.Assign(std::move(Out));
	// 終了
	return;
}

/**
 * Go 末尾コンマ補修関数
 * @param Src 整形後 TSSource（インプレース編集）
 */
void Postprocess::FixGoTrailingComma(TSSource &Src) {
	// 未解析原稿の場合の返戻
	if(!Src.IsParsed()) return;
	// 末尾コンマの挿入位置列
	std::vector<uint32_t> Insertions;
	const std::string &Source = Src;
	// ASI の構文解析エラーを防ぐ末尾コンマ欠落箇所の収集
	WalkAst(
		Src.GetRoot(),
		[&](const TSNode Node) -> void {
			// 複数行コンテナでない節点の除外
			if(!NodeKind::MultilineCommaContainer.Contains(Node)) return;
			// 名前付子と全子の個数
			const uint32_t NamedCount = ts_node_named_child_count(Node), ChildCount = ts_node_child_count(Node);
			// 空コンテナの除外
			if(!NamedCount || !ChildCount) return;
			// Go の末尾の名前付コメントを飛ばして実要素を取得（コメント本文への `,` 挿入と構文破壊を防止）
			uint32_t LastIdx = NamedCount;
			while(LastIdx && NodeKind::Comment.Contains(ts_node_named_child(Node, LastIdx - 1))) --LastIdx;
			// 名前付要素がコメントだけの場合の除外
			if(!LastIdx) return;
			// 末尾の実要素の終端位置
			const uint32_t LastEnd = ts_node_end_byte(ts_node_named_child(Node, LastIdx - 1));
			// 閉じ字句の開始位置
			const uint32_t ClosingStart = ts_node_start_byte(ts_node_child(Node, ChildCount - 1));
			// 補完不要な末尾位置の除外
			if(LastEnd >= ClosingStart || !LastEnd || LastEnd > Source.size() || Source[LastEnd - 1] == ',') return;
			// 閉じ字句直前の非コメント子による末尾コンマの確認
			for(uint32_t Idx = ChildCount - 1; Idx;) {
				// 閉じ字句直前の子候補
				const TSNode Previous = ts_node_child(Node, --Idx);
				if(NodeKind::Comment.Contains(Previous)) continue;
				// 既存コンマを見付けた場合の返戻
				if(Src.View(Previous) == ",") return;
				break;
			}
			// コンテナ内に改行が在る場合のみ補修（単一行コンテナは自動セミコロン挿入が走らない為，対象外）
			if(std::memchr(Source.data() + LastEnd, '\n', ClosingStart - LastEnd)) Insertions.push_back(LastEnd);
		}
	);
	// AST 走査位置を保つ収集後の一括挿入
	InsertCharAt(Src, Insertions, ',');
	// 終了
	return;
}

/**
 * JSX の本文の端の空白の表記確定関数
 * @param Src 整形後 TSSource（インプレース編集）
 */
void Postprocess::FixJsxEdgeSpaces(TSSource &Src) {
	const std::string &Source = Src;
	std::vector<TextEdit> Edits;
	// 出力表現の異なる空白印とタブ印の個別走査
	for(const char Blank : { ' ', '\t' }) {
		// 空白種別に対応する内部印
		const std::string_view Mark = Src.GetJsxBlankMark(Blank);
		// 印を置かなかった（私用領域の文字を選べず文字参照の儘の）場合は次の文字へ
		if(Mark.front() == '&') continue;
		for(size_t Found = Source.find(Mark); Found != std::string::npos; Found = Source.find(Mark, Found)) {
			const uint32_t Pos = static_cast<uint32_t>(Found);
			// 空白の印は，後に続く空白と印を最後の印迄１つの空白の並びとして書く（`{" "}{" "}` でなく `{"  "}`）
			std::string Spaces(1, ' ');
			Found += Mark.size();
			for(size_t Scan = Found, Count = 1; Blank == ' '; ++Count) {
				if(Scan < Source.size() && Source[Scan] == ' ') ++Scan;
				else if(!Source.compare(Scan, Mark.size(), Mark)) {
					Found = Scan += Mark.size();
					Spaces.resize(Count + 1, ' ');
				} else break;
			}
			// 印の前後の空白の外の字が改行か（行端に接するか）
			const uint32_t End = static_cast<uint32_t>(Found);
			uint32_t Before = Pos, After = End;
			while(Before && (Source[Before - 1] == ' ' || Source[Before - 1] == '\t')) --Before;
			while(After < Source.size() && (Source[After] == ' ' || Source[After] == '\t')) ++After;
			// 前後の行端接続有無
			const bool IsLeadBroken = !Before || Source[Before - 1] == '\n', IsTailBroken = After == Source.size() || Source[After] == '\n';
			if(Blank == ' ') {
				// 片側だけ行端に接する時は，反対側に続く本文の半角空白も同じ式に纏める（`Hello, {" "}` でなく `Hello,{"  "}`）
				uint32_t From = Pos, To = End;
				if(IsTailBroken && !IsLeadBroken) while(From && Source[From - 1] == ' ') --From;
				if(IsLeadBroken && !IsTailBroken) while(To < Source.size() && Source[To] == ' ') ++To;
				Spaces.append(Pos + To - From - End, ' ');
				TextEdit::Push(From, To, IsLeadBroken || IsTailBroken ? "{\"" + Spaces + "\"}" : Spaces, Edits);
				continue;
			}
			// 改行に接するタブ前後の空白と改行の縮約
			uint32_t From = Pos, To = End;
			if(IsLeadBroken) while(From && (Source[From - 1] == ' ' || Source[From - 1] == '\t' || Source[From - 1] == '\n')) --From;
			if(IsTailBroken) while(To < Source.size() && (Source[To] == ' ' || Source[To] == '\t' || Source[To] == '\n')) ++To;
			// 行端に接するタブへの置換登録
			TextEdit::Push(From, To, "\t", Edits);
		}
	}
	// 重複する印範囲の編集器による後方反映
	TextEdit::Apply(Src, Edits);
	// 終了
	return;
}

/**
 * UTF-8 妥当性判定関数（３バイト窓での記号照合が文字境界を跨がない事の前提を確かめる）
 * @param Text 判定対象
 * @return 全体が妥当な UTF-8 なら true
 */
bool Postprocess::IsValidUtf8(const std::string_view Text) {
	// 復号器の進める符号点境界だけの走査
	for(size_t Pos = 0; Pos < Text.size();) {
		uint32_t Point = 0;
		// 不正な UTF-8 の並びが有る事の返戻
		if(!TextEdit::DecodeUtf8(Text, Pos, Point)) return false;
	}
	// 全体が妥当な UTF-8 である事の返戻
	return true;
}

/**
 * コメント内コードの保護範囲取得関数
 * @param Text マーカを含む連続コメント
 * @param HasMarkers コメントマーカを含む本文か
 * @param IsUnclosed 未閉鎖のコード区画が有るかの出力先（不要なら nullptr）
 * @return コード又は其のインデントに属するバイトの保護標識
 */
std::vector<unsigned char> Postprocess::CommentCodeMask(
	const std::string_view Text,
	const bool HasMarkers,
	bool *const IsUnclosed
) {
	// 呼出側へ返す未閉鎖状態の走査開始時での消去
	if(IsUnclosed) *IsUnclosed = false;
	struct LineInfo {
		size_t Start; // 原文中の行開始位置
		size_t End; // 原文中の行終端位置
		size_t Body; // 元の Text 内でコメント開始記号を除いた本文の開始位置
		size_t OriginalStart; // 共通インデント除去後の本文に対応する元の Text 内の位置
		size_t ContentStart; // Content 内の開始位置
		size_t ContentEnd; // Content 内の終端位置
	};
	// 原文位置別の保護標識
	std::vector<unsigned char> Protected(Text.size(), 0);
	// 論理行別の位置情報
	std::vector<LineInfo> Lines;
	// 本文に共通する字下げ幅
	size_t CommonIndent = HasMarkers ? std::string::npos : 0;
	// 各論理行の範囲確定と本文の共通字下げ算出
	for(size_t Start = 0; Start < Text.size();) {
		// 改行位置と論理行終端
		const size_t Newline = Text.find('\n', Start), End = Newline == std::string::npos ? Text.size() : Newline;
		// コメント開始記号を除く本文位置
		size_t Body = Start;
		if(HasMarkers) {
			while(Body < End && (Text[Body] == ' ' || Text[Body] == '\t')) ++Body;
			if(Text.substr(Body, 2) == "//") {
				// 行コメント開始記号の位置
				const size_t Marker = Body;
				while(Body < End && Text[Body] == '/') ++Body;
				if(Body - Marker == 2 && Body < End && Text[Body] == '!') ++Body;
			} else if(Text.substr(Body, 2) == "/*") {
				Body += 2;
				while(Body < End && (Text[Body] == '*' || Text[Body] == '!')) ++Body;
			} else if(Text.substr(Body, 4) == "<!--") Body += 4;
			else if(Body < End && (Text[Body] == '#' || Text[Body] == '*')) ++Body;
			else Body = Start;
		}
		// 本文先頭と字下げ幅
		size_t First = Body, Indent = 0;
		while(First < End && (Text[First] == ' ' || Text[First] == '\t')) {
			// 表示桁に応じた字下げ加算
			Indent += Text[First] == '\t' ? 4 - Indent % 4 : 1;
			++First;
		}
		if(First < End && Text.substr(First, 2) != "*/" && Text.substr(First, 3) != "-->") {
			// 非空本文の最小字下げへの更新
			CommonIndent = std::min(CommonIndent, Indent);
		}
		// 論理行情報の登録
		Lines.push_back({ Start, End, Body, 0, 0, 0 });
		// 次の論理行への移動
		Start = End + 1;
	}
	// 文書マーカ除去後の本文と原文バイト位置の対応付け
	std::string Content;
	// 共通字下げ除去後の本文連結と原文への行別逆写像
	for(LineInfo &Line : Lines) {
		// 共通字下げ除去の走査位置と表示桁
		size_t First = Line.Body, Columns = 0;
		while(First < Line.End && Columns < CommonIndent && (Text[First] == ' ' || Text[First] == '\t')) {
			// 次の空白後の表示桁
			const size_t Next = Columns + (Text[First] == '\t' ? 4 - Columns % 4 : 1);
			if(Next > CommonIndent) break;
			// 除去済の表示桁の更新
			Columns = Next;
			++First;
		}
		// 連結本文に対応する原文開始位置
		Line.OriginalStart = First;
		// 連結本文中の行開始位置
		Line.ContentStart = Content.size();
		// 論理行本文の連結
		for(size_t Pos = First; Pos < Line.End; ++Pos) Content += Text[Pos];
		// 連結本文中の行終端位置
		Line.ContentEnd = Content.size();
		// 論理行間の改行復元
		Content += '\n';
	}
	// 連結本文位置別のコード標識
	std::vector<unsigned char> Code(Content.size(), 0);
	// 発見済コード範囲の連結本文上の標識への転写
	const auto Protect = [&](const size_t Begin, const size_t End) -> void {
		// 指定範囲のコード保護
		std::fill(Code.begin() + Begin, Code.begin() + End, 1);
	};
	// コード区画と段落の走査状態
	size_t FenceLength = 0;
	// 開いている囲み区切りの文字
	char FenceChar = 0;
	// 段落・字下げコード・コード塊の状態
	bool IsParagraph = false, IsIndented = false, HasBlock = false;
	// 現在のリスト本文の字下げ幅
	size_t ListIndent = 0;
	// 囲みコードと字下げコードの行単位での先行保護
	for(const LineInfo &Line : Lines) {
		// 行本文先頭と字下げ幅
		size_t First = Line.ContentStart, Columns = 0;
		while(First < Line.ContentEnd && (Content[First] == ' ' || Content[First] == '\t')) {
			// 表示桁に応じた字下げ加算
			Columns += Content[First] == '\t' ? 4 - Columns % 4 : 1;
			++First;
		}
		// 引用とリストの接頭辞を除くコード区画インデントの算出
		while(First < Line.ContentEnd && Content[First] == '>' && Columns < 4) {
			++First;
			if(First < Line.ContentEnd && Content[First] == ' ') ++First;
			// 引用接頭辞後の字下げ再計数
			Columns = 0;
			while(First < Line.ContentEnd && (Content[First] == ' ' || Content[First] == '\t')) {
				// 引用内の字下げ加算
				Columns += Content[First] == '\t' ? 4 - Columns % 4 : 1;
				++First;
			}
		}
		if(Columns >= ListIndent) Columns -= ListIndent;
		else ListIndent = 0;
		// リスト開始記号の終端候補
		size_t MarkerEnd = First;
		if(First < Line.ContentEnd && (Content[First] == '-' || Content[First] == '+' || Content[First] == '*')) ++MarkerEnd;
		else {
			while(MarkerEnd < Line.ContentEnd && std::isdigit(static_cast<unsigned char>(Content[MarkerEnd]))) ++MarkerEnd;
			if(MarkerEnd > First && MarkerEnd < Line.ContentEnd && (Content[MarkerEnd] == '.' || Content[MarkerEnd] == ')')) ++MarkerEnd;
			else MarkerEnd = First;
		}
		if(!FenceLength && Columns < 4 && MarkerEnd > First && MarkerEnd < Line.ContentEnd && Content[MarkerEnd] == ' ') {
			// リスト本文の基準字下げ加算
			ListIndent += Columns + MarkerEnd - First + 1;
			// リスト本文先頭への移動
			First = MarkerEnd + 1;
			// リスト接頭辞後の字下げ再計数
			Columns = 0;
			while(First < Line.ContentEnd && (Content[First] == ' ' || Content[First] == '\t')) {
				// リスト本文内の字下げ加算
				Columns += Content[First] == '\t' ? 4 - Columns % 4 : 1;
				++First;
			}
			// リスト開始行の段落状態の初期化
			IsParagraph = false;
		}
		// 空行の該当有無
		const bool IsBlank = First == Line.ContentEnd;
		// 囲み区切りの終端候補
		size_t RunEnd = First;
		if(!IsBlank && (Content[First] == '`' || Content[First] == '~')) {
			while(RunEnd < Line.ContentEnd && Content[RunEnd] == Content[First]) ++RunEnd;
		}
		// 連続する囲み文字数
		const size_t Run = RunEnd - First;
		if(FenceLength) {
			// 閉じ区切りを含む行全体の原文保持
			Protect(Line.ContentStart, Line.ContentEnd);
			if(
				Columns < 4 && Content[First] == FenceChar && Run >= FenceLength && Content.find_first_not_of(" \t", RunEnd) >= Line.ContentEnd
			) FenceLength = 0;
			// 囲みコード内の段落状態解除
			IsParagraph = false;
			continue;
		}
		if(Columns < 4 && Run > 2 && (Content[First] != '`' || Content.find('`', RunEnd) >= Line.ContentEnd)) {
			// 囲み区切り文字の記録
			FenceChar = Content[First];
			// 囲み区切り長の記録
			FenceLength = Run;
			// 囲みコード開始行の全体保護
			Protect(Line.ContentStart, Line.ContentEnd);
			// コード塊の検出状態設定
			HasBlock = true;
			// 囲みコード開始時の段落状態解除
			IsParagraph = false;
			continue;
		}
		if(Columns > 3 && (!IsParagraph || IsIndented)) {
			// 字下げコード行の全体保護
			Protect(Line.ContentStart, Line.ContentEnd);
			// 字下げコードとコード塊の状態設定
			IsIndented = HasBlock = true;
			// 字下げコード内の段落状態解除
			IsParagraph = false;
		} else if(IsBlank) IsParagraph = false;
		else {
			// 字下げコード状態の解除
			IsIndented = false;
			// 通常段落の開始状態設定
			IsParagraph = true;
		}
	}
	if(IsUnclosed && FenceLength) *IsUnclosed = true;
	// 同長の次区切りの一括収集による反復探索の回避
	std::unordered_map<size_t, size_t> PreviousTicks, NextTicks;
	// 直前行の空行状態
	bool IsBlankLine = true;
	// 同一段落内の同長バッククォートの対応付け
	for(size_t Pos = 0; Pos < Content.size();) {
		if(Code[Pos]) {
			// コード保護範囲での未対応区切り破棄
			PreviousTicks.clear();
			++Pos;
		} else if(Content[Pos] == '`') {
			// バッククォート列の終端位置
			size_t End = Pos + 1;
			while(End < Content.size() && Content[End] == '`') ++End;
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
			if(Content[Pos] == '\n') {
				// 空行と文書化タグ行を境界とするコードスパン段落の区分
				size_t Next = Pos + 1;
				while(Next < Content.size() && (Content[Next] == ' ' || Content[Next] == '\t')) ++Next;
				if(
					IsBlankLine || Next + 1 < Content.size() && (Content[Next] == '@' || Content[Next] == '\\') &&
					std::isalpha(static_cast<unsigned char>(Content[Next + 1]))
					// 段落境界での未対応区切り破棄
				) PreviousTicks.clear();
				// 次行の空行状態初期化
				IsBlankLine = true;
			} else if(Content[Pos] != ' ' && Content[Pos] != '\t') IsBlankLine = false;
			++Pos;
		}
	}
	// 区切り長の一致するコードスパンと文書用コードタグだけの保護
	for(size_t Pos = 0; Pos < Content.size();) {
		if(Code[Pos]) {
			++Pos;
			continue;
		}
		// 文書生成系コード引用の保護範囲への追加
		if(
			// 文書用コードタグの開始長
			const size_t InlineTag = !Content.compare(Pos, 6, "{@code") ? 6 : !Content.compare(Pos, 9, "{@literal") ? 9 : 0;
			InlineTag && Pos + InlineTag < Content.size() &&
			(std::isspace(static_cast<unsigned char>(Content[Pos + InlineTag])) || Content[Pos + InlineTag] == '}')
		) {
			// 文書用コードタグの走査位置と波括弧深さ
			size_t End = Pos + 1, Depth = 1;
			while(End < Content.size() && Depth) {
				if(Content[End] == '{') ++Depth;
				else if(Content[End] == '}') --Depth;
				++End;
			}
			if(IsUnclosed && Depth) *IsUnclosed = true;
			// 文書用コードタグ範囲の保護
			Protect(Pos, End);
			// タグ終端への移動
			Pos = End;
			continue;
		}
		if(Content[Pos] == '@' || Content[Pos] == '\\') {
			// Doxygen の開始指令から対応終了指令迄のコード塊化
			bool IsCommand = false;
			for(const std::string_view Name : { "code", "verbatim" }) {
				// 開始指令名の直後位置
				const size_t After = Pos + Name.size() + 1;
				if(Content.compare(Pos + 1, Name.size(), Name) || After < Content.size() && IsIdentifierChar(Content[After])) continue;
				// 対応する終了指令
				const std::string Close = std::string(1, Content[Pos]) + "end" + std::string(Name);
				// 終了指令位置と保護終端
				const size_t Found = Content.find(Close, After), End = Found == std::string::npos ? Content.size() : Found + Close.size();
				if(IsUnclosed && Found == std::string::npos) *IsUnclosed = true;
				// Doxygen コード指令範囲の保護
				Protect(Pos, End);
				// コード塊の検出状態設定
				HasBlock = true;
				// 終了指令直後への移動
				Pos = End;
				// 開始指令の検出状態設定
				IsCommand = true;
				break;
			}
			if(IsCommand) continue;
			// 枠組が実行時に読む注釈引数の括弧範囲の原文保持
			size_t NameEnd = Pos + 1;
			while(NameEnd < Content.size() && (IsIdentifierChar(Content[NameEnd]) || Content[NameEnd] == '\\')) ++NameEnd;
			if(Content[Pos] == '@' && NameEnd > Pos + 1 && NameEnd < Content.size() && Content[NameEnd] == '(') {
				// 注釈引数の走査位置と括弧深さ
				size_t End = NameEnd + 1, Depth = 1;
				for(char Quote = 0; End < Content.size() && Depth; ++End) {
					if(Quote) Quote = Content[End] == Quote ? 0 : Quote;
					else switch(Content[End]) {
					case '"':
					case '\'':
						Quote = Content[End];
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
		// 同じ行で閉じる二重引用符内の原文保持
		if(Content[Pos] == '"') {
			if(const size_t Close = Content.find_first_of("\"\n", Pos + 1); Close != std::string::npos && Content[Close] == '"') {
				// 二重引用符内の保護
				Protect(Pos, Close + 1);
				// 閉じ引用符直後への移動
				Pos = Close + 1;
				continue;
			}
		}
		// 識別子の字に接しない単引用符対を同じ行内で保護（型・参照経路は保ち英文の省略は除外）
		if(Content[Pos] == '\'' && (!Pos || !IsIdentifierChar(Content[Pos - 1]))) {
			if(
				const size_t Close = Content.find_first_of("'\n", Pos + 1);
				Close != std::string::npos && Content[Close] == '\'' && (Close + 1 >= Content.size() || !IsIdentifierChar(Content[Close + 1]))
			) {
				// 単引用符内の保護
				Protect(Pos, Close + 1);
				// 閉じ引用符直後への移動
				Pos = Close + 1;
				continue;
			}
		}
		if(Content[Pos] == '\\' && Pos + 1 < Content.size()) {
			Pos += 2;
			continue;
		}
		if(Content[Pos] == '`') {
			// 対応する閉じ印を持つコードスパンだけの保護
			size_t End = Pos + 1;
			while(End < Content.size() && Content[End] == '`') ++End;
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
		// HTML コード要素の検出有無
		bool IsTagged = false;
		// HTML コード系要素の同名ネスト数による閉じ位置の決定
		for(const std::string_view Tag : { "c", "code", "pre" }) {
			if(Content[Pos] != '<' || Content.compare(Pos + 1, Tag.size(), Tag)) continue;
			// 開始タグ名の終端位置
			const size_t NameEnd = Pos + Tag.size() + 1;
			if(
				NameEnd >= Content.size() ||
				Content[NameEnd] != '>' && Content[NameEnd] != ' ' && Content[NameEnd] != '\t' && Content[NameEnd] != '\n'
			) continue;
			const auto TagEnd = [&](size_t Start) -> size_t {
				// 属性内で開いている引用符
				char Quote = 0;
				for(; Start < Content.size(); ++Start) {
					if(const char Char = Content[Start]; Quote && Char == Quote) Quote = 0;
					else if(!Quote && (Char == '\'' || Char == '"')) Quote = Char;
					// 閉じ記号の位置の返戻
					else if(!Quote && Char == '>') return Start;
				}
				// 閉じ記号不在の返戻
				return Content.size();
			};
			// 開始タグの終端位置
			const size_t OpenEnd = TagEnd(NameEnd);
			if(OpenEnd < Content.size() && Content[OpenEnd - 1] == '/') continue;
			// コード要素の保護終端と同名タグ深さ
			size_t End = Content.size(), Depth = 1;
			for(size_t Scan = OpenEnd < Content.size() ? OpenEnd + 1 : Content.size(); Scan < Content.size();) {
				Scan = Content.find('<', Scan);
				if(Scan == std::string::npos) break;
				if(
					const bool IsCdata = !Content.compare(Scan, 9, "<![CDATA["), IsComment = !Content.compare(Scan, 4, "<!--");
					IsCdata || IsComment
				) {
					// 特殊区画の閉じ位置
					const size_t Close = Content.find(IsCdata ? "]]>" : "-->", Scan + (IsCdata ? 9 : 4));
					Scan = Close == std::string::npos ? Content.size() : Close + 3;
					continue;
				}
				// タグ候補の終端位置
				const size_t Close = TagEnd(Scan + 1);
				if(Close == Content.size()) break;
				// 終了タグの該当有無
				const bool IsClosing = Content[Scan + 1] == '/';
				if(
					const size_t NameStart = Scan + (IsClosing ? 2 : 1), AfterName = NameStart + Tag.size();
					!Content.compare(NameStart, Tag.size(), Tag) && AfterName <= Close &&
					(AfterName == Close || Content[AfterName] == ' ' || Content[AfterName] == '\t' || Content[AfterName] == '\n')
				) {
					if(IsClosing) --Depth;
					else if(Content[Close - 1] != '/') ++Depth;
				}
				Scan = Close + 1;
				if(!Depth) {
					// 対応する終了タグ直後の記録
					End = Scan;
					break;
				}
			}
			if(IsUnclosed && Depth) *IsUnclosed = true;
			// HTML コード要素範囲の保護
			Protect(Pos, End);
			// 複数行コード塊の検出状態更新
			HasBlock = HasBlock || Content.find('\n', Pos) < End;
			// コード要素終端への移動
			Pos = End;
			// HTML コード要素の検出状態設定
			IsTagged = true;
			break;
		}
		if(!IsTagged) ++Pos;
	}
	for(const LineInfo &Line : Lines) {
		// 連結本文の保護標識から元コメントのバイト位置への逆写像
		for(size_t Pos = Line.ContentStart; Pos < Line.ContentEnd; ++Pos) {
			if(Code[Pos]) Protected[Line.OriginalStart + Pos - Line.ContentStart] = 1;
		}
	}
	// 相対インデントを保つコードブロック群全行のマーカ後空白の保持
	if(HasMarkers && HasBlock) for(const LineInfo &Line : Lines) Protected[Line.Start] = 1;
	// 保護標識の返戻
	return Protected;
}

/**
 * 引用括弧の深度更新関数（鉤括弧・二重鉤括弧・隅付括弧の開閉を数える）
 * @param Text 対象テキスト
 * @param Pos 判定位置（UTF-8 の符号点先頭）
 * @param Depth 引用深度（開きで加算，閉じで減算される）
 * @return 位置が引用括弧の符号点なら true（呼出側は３バイト進める）
 */
bool Postprocess::IsQuoteDepthChar(const std::string_view Text, const size_t Pos, size_t &Depth) {
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
 * 引用外の句点探索関数（全角の小数点は数値の一部で句点ではない為，読み飛ばす）
 * @param Text 走査対象
 * @param From 走査開始位置
 * @param Limit 走査終端位置（此の位置以降は見ない）
 * @param Depth 引用深度（走査の進行に伴い更新される）
 * @param Protected コードと其のインデントの保護標識
 * @return 句点の開始位置（無ければ Limit）
 */
size_t Postprocess::FindSentencePeriod(
	const std::string_view Text,
	const size_t From,
	const size_t Limit,
	size_t &Depth,
	const std::span<const unsigned char> Protected
) {
	// ３バイト窓で確認する全角数字０〜９の UTF-8 範囲
	const auto IsFullWidthDigit = [Text](const size_t At) -> bool {
		// 窓が末尾を越える事の返戻
		if(At + 3 > Text.size()) return false;
		// 数字候補の三バイト文字
		const std::string_view Cell(Text.data() + At, 3);
		// 全角数字かの返戻
		return Cell[0] == '\xEF' && Cell[1] == '\xBC' && Cell[2] >= '\x90' && Cell[2] <= '\x99';
	};
	for(size_t Pos = From; Pos + 3 <= Limit;) {
		if(Protected[Pos]) {
			++Pos;
			continue;
		}
		if(IsQuoteDepthChar(Text, Pos, Depth)) {
			Pos += 3;
			continue;
		}
		// 記号例を含む引用内の句点候補の除外
		const std::string_view Here(Text.data() + Pos, 3);
		if(Depth || Here != "。" && Here != "．") {
			++Pos;
			continue;
		}
		// 数値内の全角小数点の句点候補からの除外
		if(Here == "．" && Pos > 2 && IsFullWidthDigit(Pos - 3) && IsFullWidthDigit(Pos + 3)) {
			Pos += 3;
			continue;
		}
		// 句点の開始位置の返戻
		return Pos;
	}
	// 句点不在の返戻
	return Limit;
}

/**
 * 文書化の札か次の行に作用する指令の行かの判定関数
 * @param Line コメントの行（マーカ込み）
 * @return 記号と空白の後が `@` で始まるか，次の行に作用する指令なら true
 */
bool Postprocess::IsTagOrDirectiveLine(const std::string_view Line) {
	// コメント開始記号後の本文位置
	const size_t Body = Line.find_first_not_of(" \t/*#!");
	// 札の行か指令の行かの返戻
	return Body != std::string_view::npos && Line[Body] == '@' || DocSig::IsNextLineDirective(Line);
}

/**
 * 行コメントのブロックコメント化関数
 * 行の途中へ置ける形へ書き換え，同じ行に続くコードが本文へ呑み込まれるのを防ぐ
 * @param Text 行コメント本文（マーカ込み）
 * @return ブロックコメント形の本文
 */
std::string Postprocess::LineCommentToBlock(const std::string_view Text) {
	// 連続する斜線コメントマーカの本文からの除外
	size_t Head = 0;
	while(Head < Text.size() && Text[Head] == '/') ++Head;
	std::string_view Body = Text.substr(Head);
	TextEdit::TrimView(Body);
	std::string Block = "/* ";
	Block.reserve(Body.size() + 8);
	// 本文内の `*/` を無力化する間への空白挿入
	for(size_t Pos = 0; Pos < Body.size(); ++Pos) {
		// 本文文字の転写
		Block += Body[Pos];
		if(Body[Pos] == '*' && Pos + 1 < Body.size() && Body[Pos + 1] == '/') Block += ' ';
	}
	Block += " */";
	// ブロックコメント形に書き換えた本文の返戻
	return Block;
}

/**
 * 行コメントの指令の判定関数（マーカ `//` に語が密着し，後続の空白も指令の字面に為る行）
 * @param Text コメント本文
 * @param Body マーカ `//` の直後の位置
 * @return Go のコンパイラ指令（`//go:linkname` 等の小文字で始まる `語:` 形）か，コロンを持たない指令の語（cgo の `//export` 等）なら true
 */
bool Postprocess::IsCommentDirective(const std::string_view Text, const size_t Body) {
	// 小文字か `_` で始まらない本文は指令でない事の返戻（`//TODO:` の様な大文字の語はコメントの見出）
	if(Body >= Text.size() || !std::islower(static_cast<unsigned char>(Text[Body])) && Text[Body] != '_') return false;
	size_t Scan = Body;
	while(Scan < Text.size() && IsIdentifierChar(Text[Scan])) ++Scan;
	// 語の後のコロンか，語が本文の末尾に達する形も含む指令の語の照合の返戻
	return Scan < Text.size() && Text[Scan] == ':' || NodeKind::BareDirective.Contains(Text.substr(Body, Scan - Body));
}

/**
 * 句点の分割適用関数（後続が有れば同じマーカを継いだ次行へ送り，無ければ句点を除去する）
 * 後方から句点を判定し，編集は前方から一度で適用する
 * 空白を除いた前の字が `\` の句点は，除くと行が `\` で終わり，C 系の行継続が次の行のコードをコメントへ取り込む為，除かず分割もしない
 * @param Text 対象テキスト（破壊的に書き換えられる）
 * @param Periods 句点の開始位置（昇順）
 * @param Enclosed 句点毎の括弧の中に在るかの標識（Periods と同じ順）
 * @param AllowsSplit 偽なら改行を作らず読点へ退避する（行末コメントは行位置が意味を持ち，分割するとインデントを失う為）
 * @param Protected コードと其のインデントの保護標識
 */
void Postprocess::BreakAtPeriods(
	std::string &Text,
	const std::span<const size_t> Periods,
	const std::span<const uint8_t> Enclosed,
	const bool AllowsSplit,
	std::vector<unsigned char> &Protected
) {
	// 句点範囲の読点・改行・空文字列への編集
	struct PeriodEdit {
		size_t Pos;
		size_t End;
		size_t PrefixBegin; // 改行の後に継ぐ接頭辞（行頭空白 + `//` `#` `*` `/*` 等 + 続く空白）の始まり
		size_t PrefixLength;
		bool IsComma;
		bool IsBlockClose = false;
		bool IsSpace = false; // 空白へ置き換えるか（閉じの記号に接する文末の句点：除くと本文と閉じの記号が接する）
	};
	std::vector<PeriodEdit> Edits;
	// 原文位置を保つ句点別置換内容の先行確定
	Edits.reserve(Periods.size());
	// 行頭・接頭辞末尾・分割可否・囲み形式と，後方編集で縮む有効行末の保持
	size_t LineBegin = std::string::npos, MarkerEnd = 0, TailStart = 0;
	bool IsSplittable = false, IsBlockLine = false;
	for(size_t Index = Periods.size(); Index--;) {
		const size_t Pos = Periods[Index];
		if(LineBegin == std::string::npos || Pos < LineBegin) {
			// 当該行の範囲とマーカ接頭辞を求める（行毎に１度）
			LineBegin = Text.rfind('\n', Pos) + 1;
			TailStart = std::min(Text.find('\n', Pos), Text.size());
			const std::string_view Line(Text.data() + LineBegin, TailStart - LineBegin);
			MarkerEnd = 0;
			while(MarkerEnd < Line.size() && (Line[MarkerEnd] == ' ' || Line[MarkerEnd] == '\t')) ++MarkerEnd;
			// １行の囲みコメントは句点で閉じ直して分割（文書化単位の `/**`・`/*!` は保持）
			IsBlockLine =
			AllowsSplit && !Line.compare(MarkerEnd, 2, "/*") && Line.compare(MarkerEnd, 3, "/**") && Line.compare(MarkerEnd, 3, "/*!") &&
			Line.size() >= MarkerEnd + 4 && Line.find("*/") == Line.size() - 2 &&
			Line.find("/*", MarkerEnd + 2) == std::string_view::npos && !IsTagOrDirectiveLine(Line);
			if(IsBlockLine) MarkerEnd += 2;
			else if(!Line.compare(MarkerEnd, 2, "//")) {
				// 文書化マーカと通常コメントの区別の次行への継承
				const size_t MarkerStart = MarkerEnd;
				MarkerEnd += 2;
				while(MarkerEnd < Line.size() && Line[MarkerEnd] == '/') ++MarkerEnd;
				if(MarkerEnd - MarkerStart == 2 && MarkerEnd < Line.size() && Line[MarkerEnd] == '!') ++MarkerEnd;
			} else if(MarkerEnd < Line.size() && (Line[MarkerEnd] == '#' || Line[MarkerEnd] == '*')) ++MarkerEnd;
			else MarkerEnd = 0;
			if(MarkerEnd) while(MarkerEnd < Line.size() && (Line[MarkerEnd] == ' ' || Line[MarkerEnd] == '\t')) ++MarkerEnd;
			// 囲みの開閉を含む行・文書化の札・次行指令は分割対象外
			IsSplittable = IsBlockLine ||
			AllowsSplit && MarkerEnd && Line.find("/*") == std::string_view::npos && Line.find("*/") == std::string_view::npos &&
			!IsTagOrDirectiveLine(Line);
		}
		// コメント終端を除く句点以降の本文有無の確認
		size_t Rest = Pos + 3;
		while(Rest < TailStart && (Text[Rest] == ' ' || Text[Rest] == '\t')) ++Rest;
		size_t Before = Pos;
		while(Before > LineBegin && (Text[Before - 1] == ' ' || Text[Before - 1] == '\t')) --Before;
		const bool IsAfterBackslash = Before > LineBegin && Text[Before - 1] == '\\';
		const bool IsBeforeCloser = !Text.compare(Rest, 3, "）") || Rest < TailStart && Text[Rest] == ')';
		if(Rest == TailStart || !Text.compare(Rest, 2, "*/") || !Text.compare(Rest, 3, "-->")) {
			// 文末句点を除去し，閉じ前は開き後の空白に合わせ補完（同じ行に開きが無ければ保持）
			if(IsAfterBackslash) continue;
			bool IsSpace = false;
			if(Rest == Pos + 3 && Rest < TailStart) {
				const bool IsHtml = !Text.compare(Rest, 3, "-->");
				const size_t Open = std::string_view(Text).substr(LineBegin, Pos - LineBegin).rfind(IsHtml ? "<!--" : "/*");
				const size_t After = LineBegin + Open + (IsHtml ? 4 : 2);
				IsSpace = Open == std::string_view::npos || Text[After] == ' ' || Text[After] == '\t';
			}
			Edits.push_back({ Pos, Pos + 3, 0, 0, false, false, IsSpace });
			if(Rest == TailStart) TailStart = Pos;
		} else if(Enclosed[Index] || IsBeforeCloser) Edits.push_back({ Pos, Pos + 3, 0, 0, !IsBeforeCloser });
		else if(!IsSplittable || IsAfterBackslash) Edits.push_back({ Pos, Pos + 3, 0, 0, true });
		else {
			Edits.push_back({ Pos, Rest, LineBegin, MarkerEnd, false, IsBlockLine });
			TailStart = Pos;
		}
	}
	// 編集が無い時の終了
	if(Edits.empty()) return;
	// 編集結果と保護標識の並行再構築
	std::string Out;
	// 置換後位置別の保護標識
	std::vector<unsigned char> OutProtected;
	// 本文と保護標識の同一編集列による並行再構築
	Out.reserve(Text.size() + (Edits.size() << 3));
	OutProtected.reserve(Out.capacity());
	size_t Copied = 0;
	for(size_t Index = Edits.size(); Index--;) {
		const PeriodEdit &Edit = Edits[Index];
		Out.append(Text, Copied, Edit.Pos - Copied);
		// 対応する保護標識の転写
		OutProtected.insert(OutProtected.end(), Protected.begin() + Copied, Protected.begin() + Edit.Pos);
		if(Edit.IsComma) Out += "，";
		else if(Edit.IsSpace) Out += ' ';
		else if(Edit.PrefixLength) {
			if(Edit.IsBlockClose) Out += " */";
			Out += '\n';
			Out.append(Text, Edit.PrefixBegin, Edit.PrefixLength);
		}
		// 挿入文字位置の非保護標識補完
		OutProtected.resize(Out.size(), 0);
		Copied = Edit.End;
	}
	Out.append(Text, Copied);
	OutProtected.insert(OutProtected.end(), Protected.begin() + Copied, Protected.end());
	Text = std::move(Out);
	Protected = std::move(OutProtected);
	// 終了
	return;
}

/**
 * コメント本文の空白の正規化関数（規約の半角と全角の境界のスペースの規則を本文へ適用する）
 * @param Text 対象テキスト（破壊的に書き換えられる）
 * @param Protected コードと其のインデントの保護標識（書換に合わせて更新される）
 */
void Postprocess::NormalizeCommentSpaces(std::string &Text, std::vector<unsigned char> &Protected) {
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
		// 全角の区切記号・括弧・引用符・ハイフン類の判定結果の返戻
		return Point == 0X3001 || Point == 0X3002 || Point - 0X3008 < 10 || Point - 0X3014 < 8 || Point == 0X30FB || Point == 0XFF01 ||
		Point == 0XFF08 || Point == 0XFF09 || Point - 0XFF0C < 4 || Point - 0XFF1A < 3 || Point == 0XFF1E || Point == 0XFF1F ||
		Point == 0XFF3B || Point == 0XFF3D || Point == 0XFF5B || Point == 0XFF5D || Point - 0X2018 < 8 || Point - 0X301D < 3 ||
		Point - 0X2010 < 4;
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
	// 位置の字の符号点（不正な並びと末尾は 0）
	const auto PointAt = [&Text](size_t At) -> uint32_t {
		// 指定位置の符号点の復号
		uint32_t Point = 0;
		// 復号した符号点の返戻
		return At < Text.size() && TextEdit::DecodeUtf8(Text, At, Point) ? Point : 0;
	};
	// 位置から始まる半角の語が字（英字・上付と下付の数字以外の表音文字）を含むか
	const auto HasLetterAt = [&Text, &IsWordChar, &IsPhoneticLetter](size_t Pos) -> bool {
		// 対象位置の処理
		for(uint32_t Point = 0; Pos < Text.size() && TextEdit::DecodeUtf8(Text, Pos, Point);) {
			if(Point < 0X80 ? !IsWordChar(static_cast<char>(Point)) : !IsPhoneticLetter(Point)) break;
			// 字を含む語である事の返戻
			if(Point < 0X80 ? std::isalpha(static_cast<int>(Point)) : Point > 0XBF && Point - 0X2070 > 0X2F) return true;
		}
		// 字を含まない語である事の返戻
		return false;
	};
	// 位置から始まる数量の単位（`N回` / `M個` の数量の英字と単位の間は，規約が英字を全角にする所）
	const auto IsQuantityUnitAt = [&Text](const size_t At) -> bool {
		// 数量に続く単位の照合
		static constexpr std::string_view Units[] = { "個", "行", "文字", "回", "本", "件", "桁", "段", "倍", "秒", "字", "列", "枚", "番目", "箇所" };
		// 単位の何れかで始まるかの返戻
		return std::any_of(
			std::begin(Units),
			std::end(Units),
			[&](const std::string_view Unit) -> bool {
				// 単位の字面の一致の返戻
				return !Text.compare(At, Unit.size(), Unit);
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
			// 絵表示を選ぶ異体字選択子 U+FE0F の読飛し
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
		// 遅延処理の実行
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
	for(size_t Pos = 0; Pos < Text.size();) {
		const bool IsLineStart = !Pos || Text[Pos - 1] == '\n';
		if(IsLineStart) {
			// 字下げ・マーカを保持し，囲み開始後の空白だけ１個へ正規化（継続行のリスト字下げは保持）
			size_t Marker = Pos;
			while(Marker < Text.size() && (Text[Marker] == ' ' || Text[Marker] == '\t')) ++Marker;
			size_t Spaces = Marker;
			while(Spaces < Text.size() && std::string_view("!#*-/<").find(Text[Spaces]) != std::string_view::npos) ++Spaces;
			size_t Body = Spaces;
			while(Body < Text.size() && (Text[Body] == ' ' || Text[Body] == '\t')) ++Body;
			// 空白も字面に含むシバン・指令・TS 三重斜線指令行の原文保持
			IsVerbatimLine = !Text.compare(Marker, 2, "#!") || !Text.compare(Marker, 2, "//") && IsCommentDirective(Text, Marker + 2) ||
			!Text.compare(Marker, 3, "///") && (!Text.compare(Body, 10, "<reference") || !Text.compare(Body, 5, "<amd-"));
			Emit(Pos, Spaces);
			if(
				(!Text.compare(Marker, 2, "/*") || !Text.compare(Marker, 4, "<!--")) && Body > Spaces && Body < Text.size() &&
				Text[Body] != '\n' && !Protected[Spaces]
			) EmitSpace();
			else Emit(Spaces, Body);
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
				// 遅延処理の実行
				if(Last != 0X7AE0 && Last != 0X7BC0) return false;
				size_t At = Out.size() - 3;
				// 章番号を構成する全角数字と半角数字の後方走査
				while(At > 2 && !Out.compare(At - 3, 2, "\xEF\xBC") && static_cast<unsigned char>(Out[At - 1]) - 0X90U < 10) At -= 3;
				while(At && std::isdigit(static_cast<unsigned char>(Out[At - 1]))) --At;
				// 番号の前が「第」かの返戻
				return At > 2 && !Out.compare(At - 3, 3, "第");
			};
			// 保護文字周辺は縮約に留め，日本語字・全角記号間の不要な空白だけ除去
			if(
				const bool IsRemovable =
				Last && Pos < Text.size() && Text[Pos] != '\n' && !IsLastProtected && !Protected[Pos] && Text.compare(Pos, 2, "//") &&
				Text.compare(Pos, 2, "# ") && Text.compare(Pos, 2, "*/") && Text.compare(Pos, 3, "-->") && !IsSpacedMark(Last) &&
				!IsSpacedMark(Next);
				IsRemovable && (IsFullWidthMark(Last) || IsFullWidthMark(Next) || IsFullWidth(Last) && IsFullWidth(Next) && !IsAfterChapter())
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
			// 保護・引用・リンク先・指令行の文字の原文転写
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
		if(
			const bool IsAfterWide = !IsLastProtected && IsFullWidthWord(Last) && std::isalpha(static_cast<unsigned char>(Char));
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
				if(!IsUrl) for(size_t At = Slash + 1; At < Chunk.size(); ++At) {
					if(
						static_cast<unsigned char>(Chunk[At]) > 0XBF && std::isalnum(static_cast<unsigned char>(Chunk[At - 1])) &&
						IsFullWidth(PointAt(Start + At))
					) {
						ChunkEnd = Start + At;
						break;
					}
				}
				if(IsAfterWide) EmitSpace();
				Emit(Start, ChunkEnd);
				// 塊内の引用開始も数え，後続の引用内空白を次巡と同じく保護
				for(size_t At = Start; At + 3 <= ChunkEnd; ++At) IsQuoteDepthChar(Text, At, Depth);
				Pos = ChunkEnd;
				// 経路後続の半角語直後としての扱い
				Last = 'a';
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
			// 遅延処理の実行
			if(LinkClose < Start) LinkClose = std::min(Text.find_first_of("]\n", Start), Text.size());
			// 同じ行の閉じ角括弧の直後が丸括弧かの返戻
			return LinkClose < Text.size() && Text[LinkClose] == ']' && LinkClose + 1 < Text.size() && Text[LinkClose + 1] == '(';
		};
		// 前の語の端（英数字・`C++` の記号，`vector<int>` の様に英数字で始まる語の `>`）
		const bool IsLastWordEdge = IsWordEdgeChar(Last) || Last == '>' && std::isalnum(static_cast<unsigned char>(Text[WordStart]));
		// 数との間へ空白を置かない単位記号前の空白抑止
		const bool ShouldInsert = Last && !IsLastProtected && (
			!IsAttachedRun && IsLastWord && (HasLetterWord || IsHalfWidthQuantityMark(Last)) && IsFullWidthWord(Point) && IsLastWordEdge &&
			!(Start - WordStart == 1 && std::isalpha(static_cast<unsigned char>(Text[WordStart])) && IsQuantityUnitAt(Start)) ||
			IsFullWidthWord(Last) && (
				IsWord && HasLetterWord && (std::isalnum(static_cast<unsigned char>(Char)) || Char == '_' || Point > 0X7F) || Char == '(' ||
				Char == '{' || Char == '[' && !OpensLink()
			) || !IsAttachedRun && (Last == ')' || Last == ']' || Last == '}') && !IsLastLinkEnd && IsFullWidthWord(Point)
		);
		if(ShouldInsert) EmitSpace();
		// 開き前の空白に対応する閉じ後の空白補完
		bool IsSpacedCloser = false;
		// 64 段迄の括弧別の開き前空白有無のビット保持
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
		else if(!ShouldInsert && IsFullWidthWord(Last)) IsAttachedRun = true;
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
 * コメント本文の正規化関数（空白の規約への正規化・読点の全角カンマ化・句点での文分割・行コメントマーカ直後スペースの均し）
 * @param Text 対象テキスト（破壊的に書き換えられる）
 * @param AllowsSplit 句点位置で行を分けて良いか（行末コメントは分けるとインデントを失う為，偽を渡す）
 * @param Protected コードと其のインデントの保護標識
 */
void Postprocess::NormalizeProtectedComment(std::string &Text, const bool AllowsSplit, std::vector<unsigned char> Protected) {
	// UTF-8 を確認出来ない本文は３バイト窓の誤検出を避け，記号正規化を省きマーカ直後の空白だけ調整
	if(IsValidUtf8(Text)) {
		// 引用内を原文保持し，丸括弧内の句点は分割せず読点化（半角括弧は同行だけ計数）
		// 句点候補走査の準備
		std::vector<size_t> Periods;
		std::vector<uint8_t> Enclosed;
		for(size_t Pos = 0, Depth = 0, ParenDepth = 0, AsciiDepth = 0; Pos + 3 <= Text.size();) {
			// 保護範囲の読み飛ばし
			if(Protected[Pos]) {
				++Pos;
				continue;
			}
			// 引用深さの更新
			if(IsQuoteDepthChar(Text, Pos, Depth)) {
				Pos += 3;
				continue;
			}
			if(Depth) {
				++Pos;
				continue;
			}
			// 全角括弧深さの更新
			if(const std::string_view Cell(Text.data() + Pos, 3); Cell == "（" || Cell == "）") {
				ParenDepth = Cell == "（" ? ParenDepth + 1 : ParenDepth - !!ParenDepth;
				Pos += 3;
				continue;
			}
			// 半角括弧深さの更新
			if(const char Byte = Text[Pos]; Byte == '(' || Byte == ')' || Byte == '\n') {
				AsciiDepth = Byte == '(' ? AsciiDepth + 1 : Byte == ')' ? AsciiDepth - !!AsciiDepth : 0;
				++Pos;
				continue;
			}
			// 読点の全角カンマへの統一
			if(std::string_view(Text.data() + Pos, 3) == "、") {
				Text.replace(Pos, 3, "，");
				Pos += 3;
				continue;
			}
			// 句点候補の探索
			const size_t Found = FindSentencePeriod(Text, Pos, Pos + 3, Depth, Protected);
			if(Found == Pos + 3) {
				++Pos;
				continue;
			}
			Periods.push_back(Found);
			// 括弧内句点を読点へ退避する位置との対応付け
			Enclosed.push_back(ParenDepth || AsciiDepth);
			Pos = Found + 3;
		}
		// 走査位置を保つ句点位置収集後の一括変換
		BreakAtPeriods(Text, Periods, Enclosed, AllowsSplit, Protected);
		// 分割後の行で空白を調整し，経路・箇条書の判定を次巡の行頭と統一
		NormalizeCommentSpaces(Text, Protected);
	}
	// 行頭マーカ `//` `#` 以外（ブロック `/* */`，HTML `<!-- -->`，Ruby `=begin` 等）の早期返戻
	if(Text.empty() || Text[0] != '#' && (Text[0] != '/' || Text.size() < 2 || Text[1] != '/')) return;
	// 各行冒頭のコメント形式確認とマーカ直後の連続空白の正規化
	std::string Out;
	size_t Copied = 0;
	// 空白量が変わる最初の行迄の出力構築の遅延
	for(size_t LineStart = 0; LineStart < Text.size();) {
		size_t MarkerLen = 0;
		bool IsSkipped = Protected[LineStart];
		if(!Text.compare(LineStart, 2, "//")) {
			MarkerLen = 2;
			if(LineStart + 2 < Text.size()) {
				const char Next = Text[LineStart + 2];
				// Doxygen・資源地図・折畳・Go / cgo 等の密着指令は空白補完対象外
				IsSkipped = IsSkipped || Next == '/' || Next == '!' || Next == '#' || Next == '@' || IsCommentDirective(Text, LineStart + 2);
			}
		} else if(Text[LineStart] == '#') {
			MarkerLen = 1;
			// シバン・装飾区切り・型注釈・属性文書・Ruby の文書化開始終了の密着を保持
			IsSkipped =
			IsSkipped || LineStart + 1 < Text.size() && std::string_view("!#:").find(Text[LineStart + 1]) != std::string_view::npos ||
			!Text.compare(LineStart + 1, 2, "--") || !Text.compare(LineStart + 1, 2, "++");
		}
		const size_t LineEnd = std::min(Text.find('\n', LineStart), Text.size());
		if(MarkerLen && !IsSkipped) {
			size_t SpaceEnd = LineStart + MarkerLen;
			while(SpaceEnd < LineEnd && (Text[SpaceEnd] == ' ' || Text[SpaceEnd] == '\t')) ++SpaceEnd;
			if(SpaceEnd < LineEnd && (SpaceEnd - LineStart - MarkerLen != 1 || Text[LineStart + MarkerLen] != ' ')) {
				if(Out.empty()) Out.reserve(Text.size() + (Text.size() >> 4));
				Out.append(Text, Copied, LineStart + MarkerLen - Copied);
				Out += ' ';
				Copied = SpaceEnd;
			}
		}
		LineStart = LineEnd + 1;
	}
	// 書き換えた行が無い時の終了
	if(!Copied) return;
	Out.append(Text, Copied);
	Text = std::move(Out);
	// 終了
	return;
}

/**
 * 連続コメント群の正規化関数
 * @param Comments 原文順に並ぶ隣接コメント
 */
void Postprocess::NormalizeCommentGroup(const std::span<CommentAttach *> Comments) {
	// 連続コメント群の連結本文
	std::string Text;
	for(const CommentAttach *const Comment : Comments) {
		Text += Comment->Text;
		Text += '\n';
	}
	// 連結本文位置別のコード保護標識
	const std::vector<unsigned char> Protected = CommentCodeMask(Text);
	size_t Offset = 0;
	for(CommentAttach *const Comment : Comments) {
		const size_t Size = Comment->Text.size();
		NormalizeProtectedComment(Comment->Text, Comment->IsLeading, { Protected.begin() + Offset, Protected.begin() + Offset + Size });
		Offset += Size + 1;
	}
	// 終了
	return;
}

/**
 * 同じ文書群の隣接行コメント判定関数
 * @param First 先行コメント
 * @param Next 後続コメント
 * @param Gap 原文でのコメント間の文字列
 * @return 同じマーカで隣接する行コメントなら true
 */
bool Postprocess::CanJoinComments(const std::string_view First, const std::string_view Next, const std::string_view Gap) {
	// 文書化記法のマーカ取得処理の定義
	const auto Marker = [](const std::string_view Text) -> std::string_view {
		// 文書化コメントのマーカ長の算出
		size_t Length = 0;
		if(Text.starts_with("//")) {
			while(Length < Text.size() && Text[Length] == '/') ++Length;
			if(Length == 2 && Length < Text.size() && Text[Length] == '!') ++Length;
		} else if(Text.starts_with("#")) Length = 1;
		// 行コメントマーカの返戻
		return Text.substr(0, Length);
	};
	const std::string_view Kind = Marker(First);
	// 同一文書群かの返戻
	return !Kind.empty() && Kind == Marker(Next) && Gap.find_first_not_of(" \t\r\n") == std::string::npos &&
	std::count(Gap.begin(), Gap.end(), '\n') == 1;
}

/**
 * 行末空白除去関数
 * @param Src 整形後 TSSource（インプレース編集）
 * @param Language 言語
 */
void Postprocess::TrimTrailingWhitespace(TSSource &Src, const Lang Language) {
	// 対象言語又は構文木を持たない場合の返戻
	if(Language == Lang::Unknown || !Src.IsParsed()) return;
	// 原文と保護範囲の準備
	const std::string &Source = Src;
	// 候補の在る時だけ，多行リテラル・ヒアドキュメント・コメント内部のトリム除外範囲を構築（通常の全木走査を省略）
	std::vector<std::pair<uint32_t, uint32_t>> Preserve;
	bool IsPreserveBuilt = false;
	const bool IsPhp = Language == Lang::PHP, IsHtml = Language == Lang::HTML;
	const auto EnsurePreserve = [&Preserve, &IsPreserveBuilt, &Src, &Source, IsPhp, IsHtml]() -> void {
		// 遅延処理の実行
		if(IsPreserveBuilt) return;
		IsPreserveBuilt = true;
		std::vector<std::pair<uint32_t, uint32_t>> Comments;
		// 値として空白を持つ葉とコメントの単一木走査での収集
		const auto Collect = [&](const TSNode Node) -> bool {
			// 空白葉とコメントの収集
			if(NodeKind::Comment.Contains(Node)) {
				uint32_t End = Src.End(Node);
				while(End > Src.Start(Node) && (Source[End - 1] == '\n' || Source[End - 1] == '\r')) --End;
				Comments.emplace_back(Src.Start(Node), End);
				// コメント内部への降下禁止の返戻
				return false;
			}
			// 出力其の物である PHP 地の文の行末空白保持
			if(IsPhp && NodeKind::PhpInlineHtml.Contains(Node)) {
				Preserve.push_back({ ts_node_start_byte(Node), ts_node_end_byte(Node) });
				// 地の文の内側へ降りない事の返戻
				return false;
			}
			// HTML の逐語要素全体の行末空白保持
			if(IsHtml && std::string_view(ts_node_type(Node)) == "element" && HtmlVerbatimOf(Src, Node) != HtmlVerbatim::None) {
				Preserve.push_back({ ts_node_start_byte(Node), ts_node_end_byte(Node) });
				// 逐語保持要素の内側へ降りない事の返戻
				return false;
			}
			// 言語別の型名漏れを避け，改行を含む葉を一律に逐語保護
			if(
				const std::string_view LeafType(ts_node_type(Node));
				!NodeKind::Leaf.Contains(LeafType) || NodeKind::NonVerbatimLeaf.Contains(LeafType)
			) return true; // 逐語保持しない節点の子孫を走査する事の返戻
			const uint32_t Start = ts_node_start_byte(Node), End = ts_node_end_byte(Node);
			if(
				Start >= End || End > Source.size() || std::string_view(Source.data() + Start, End - Start).find('\n') == std::string_view::npos
			) return false; // 範囲外又は単一行の葉の走査完了の返戻
			Preserve.push_back({ Start, End });
			// 逐語範囲を保護した葉の走査完了の返戻
			return false;
		};
		if(Collect(Src.GetRoot())) WalkChildrenCursor(Src.GetRoot(), Collect);
		// 隣接コメント群を跨ぐコード区画の確認
		for(size_t Index = 0; Index < Comments.size();) {
			const uint32_t Begin = Comments[Index].first;
			uint32_t End = Comments[Index].second;
			++Index;
			while(
				Index < Comments.size() && Comments[Index].first >= End && CanJoinComments(
					std::string_view(Source).substr(Begin, End - Begin),
					std::string_view(Source).substr(Comments[Index].first, Comments[Index].second - Comments[Index].first),
					std::string_view(Source).substr(End, Comments[Index].first - End)
				)
			) End = Comments[Index++].second;
			const std::vector<unsigned char> Code = CommentCodeMask(std::string_view(Source).substr(Begin, End - Begin));
			// コード標識連続区間だけの行末空白保持範囲への追加
			for(size_t Pos = 0; Pos < Code.size();) {
				if(!Code[Pos]) {
					++Pos;
					continue;
				}
				const size_t First = Pos;
				while(Pos < Code.size() && Code[Pos]) ++Pos;
				Preserve.emplace_back(Begin + static_cast<uint32_t>(First), Begin + static_cast<uint32_t>(Pos));
			}
		}
		std::sort(Preserve.begin(), Preserve.end());
		// 重複範囲の累積最大値化（二分探索の成立）
		uint32_t RunningMaxEnd = 0;
		for(std::pair<uint32_t, uint32_t> &Range : Preserve) {
			if(Range.second < RunningMaxEnd) Range.second = RunningMaxEnd;
			else RunningMaxEnd = Range.second;
		}
	};
	// 保護範囲の包含判定
	const auto IsInPreserve = [&Preserve, &EnsurePreserve](const uint32_t Byte) -> bool {
		// 指定位置を含む保護範囲の探索
		EnsurePreserve();
		// start_byte が Byte 超の最初の範囲の直前を見て包含を判定
		const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Iter = std::upper_bound(
			Preserve.begin(),
			Preserve.end(),
			Byte,
			[](const uint32_t Key, const std::pair<uint32_t, uint32_t> &Range) -> bool {
				// 保護範囲の開始位置との比較
				return Key < Range.first;
			}
		);
		// 保護範囲の有無の返戻
		return Iter != Preserve.begin() && Byte < (Iter - 1)->second;
	};
	// 初トリム迄の出力構築を遅延する行単位走査
	// 末尾空白除去結果の構築状態
	std::string Out;
	size_t LineStart = 0;
	bool IsAnyTrim = false;
	while(LineStart <= Source.size()) {
		// 保護外の末尾空白だけの現在行終端からの除去
		const size_t Newline = Source.find('\n', LineStart), LineEnd = Newline == std::string::npos ? Source.size() : Newline;
		size_t TrimEnd = LineEnd;
		while(TrimEnd > LineStart && (Source[TrimEnd - 1] == ' ' || Source[TrimEnd - 1] == '\t') && !IsInPreserve(TrimEnd - 1)) {
			--TrimEnd;
		}
		if(!IsAnyTrim) {
			if(TrimEnd == LineEnd) {
				if(Newline == std::string::npos) break;
				LineStart = Newline + 1;
				continue;
			}
			// 初トリム行での先行セグメント転写と構築モードへの移行
			Out.reserve(Source.size());
			Out.append(Source, 0, LineStart);
			IsAnyTrim = true;
		}
		Out.append(Source, LineStart, TrimEnd - LineStart);
		if(Newline == std::string::npos) break;
		Out += '\n';
		LineStart = Newline + 1;
	}
	// 末尾空白を除かなかった場合の返戻
	if(!IsAnyTrim) return;
	// 変更時に限る再構築済本文への差替え
	Src.Assign(std::move(Out));
	// 終了
	return;
}

/**
 * 効く位置に在る符号化宣言の行の取得関数
 * Python と Ruby の有効位置に在る符号化宣言だけを検出する
 * @param Text ソース
 * @param Language 対象言語
 * @return 効く宣言の行番号（１始まり，無ければ 0）
 */
size_t Postprocess::EncodingDeclarationLine(const std::string_view Text, const Lang Language) {
	// 符号化宣言候補行の走査位置の初期化
	size_t LineStart = 0;
	// 有効位置二行の走査
	for(size_t Line = 1; Line < 3; ++Line) {
		// 行のコメント状態判定
		const size_t End = std::min(Text.find('\n', LineStart), Text.size());
		const std::string_view Row = Text.substr(LineStart, End - LineStart);
		const size_t First = Row.find_first_not_of(" \t\f");
		const bool IsComment = First != std::string_view::npos && Row[First] == '#';
		if(
			IsComment && (Row.find("coding:", First) != std::string_view::npos || Row.find("coding=", First) != std::string_view::npos)
		) return Line; // 有効な符号化宣言の行番号の返戻
		if(End == Text.size() || !(Language == Lang::Python ? First == std::string_view::npos || IsComment : Row.starts_with("#!"))) {
			// ２行目を読むのは，１行目が Python ではコメントか空行，Ruby ではシバンの時だけの為，其れ以外で打ち切る事の返戻
			return 0;
		}
		LineStart = End + 1;
	}
	// 効く位置に宣言が無い事の返戻
	return 0;
}

/**
 * 符号化宣言の効き目の保持関数
 * 前の空行やコメントが詰まると，入力で効かない位置（３行目以降）に在った宣言が効く位置へ上がり，ファイルの読み方が変わる
 * 其の場合は宣言の前へ空行を補って３行目へ戻す
 * @param Src 整形結果（変更が有れば書き換える）
 * @param Source 整形前の入力
 * @param Language 対象言語
 */
void Postprocess::KeepEncodingDeclaration(TSSource &Src, const std::string_view Source, const Lang Language) {
	// 符号化宣言を扱わない言語の場合の返戻
	if(Language != Lang::Python && Language != Lang::Ruby) return;
	// 整形結果の有効宣言確認
	const size_t Line = EncodingDeclarationLine(Src, Language);
	// 出力で効く宣言が無いか，入力でも効いて居た場合の終了
	if(!Line || EncodingDeclarationLine(Source, Language)) return;
	// 宣言を三行目へ押戻す空行挿入
	std::string Text = Src;
	Text.insert(Line == 1 ? 0 : Text.find('\n') + 1, 3 - Line, '\n');
	Src.Assign(std::move(Text));
	// 終了
	return;
}

/**
 * 構文木の外の本文の再結合関数（`Preprocess::DetachOuterText` で切り離した PHP の地の文の空白を原文の儘戻す）
 * @param Output 整形結果（結果で上書きされる）
 * @param Lead 切り離した先頭の本文
 * @param Tail 切り離した末尾の本文
 */
void Postprocess::ReattachOuterText(std::string &Output, const std::string &Lead, const std::string &Tail) {
	// 原文改行を持つ末尾本文の整形済改行除去後の連結
	if(!Tail.empty() && !Output.empty() && Output.back() == '\n') Output.pop_back();
	Output.insert(0, Lead);
	Output += Tail;
	// 終了
	return;
}

/**
 * 末尾の改行の復元関数（末尾が出力其の物〈PHP の地の文・Ruby の `__END__` 以降のデータ〉の原文は，末尾の改行も中身として原文の数を保つ）
 * @param Result 整形結果（結果で上書きされる）
 * @param Count 原文の末尾の改行の数
 * @param MixedTail 改行様式が混在する原文の末尾の改行の並び（空なら LF を Count 個置く）
 */
void Postprocess::RestoreTailNewlines(std::string &Result, const size_t Count, const std::string &MixedTail) {
	// 確保失敗時の途中結果を避ける領域の先行確保
	Result.reserve(Result.size() + Count + MixedTail.size());
	while(!Result.empty() && Result.back() == '\n') Result.pop_back();
	if(MixedTail.empty()) Result.append(Count, '\n');
	else Result += MixedTail;
	// 終了
	return;
}
