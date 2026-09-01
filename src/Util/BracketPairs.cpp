#include "BracketPairs.hpp"
#include "NodeKind.hpp"
#include "TsSource.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

/**
 * 括弧計数から除外する不透明リテラル範囲の収集関数（開始バイト昇順・追記式）
 * @param Src ソースコード（走査の起点は構文木の根）
 * @param Language 対象言語
 * @param Ranges 収集先の範囲列（開始バイト，終端バイト）
 */
void BracketPairs::CollectOpaqueRanges(
	const TSSource &Src,
	const Lang Language,
	std::vector<std::pair<uint32_t, uint32_t>> &Ranges
) {
	// 不透明範囲を集める走査根
	const TSNode Node = Src.GetRoot();
	// 起点が空ノードの場合は収集対象が無い為の終了
	if(ts_node_is_null(Node)) return;
	// `ERROR` 配下を除く先行順の不透明範囲収集（CSS の引用符無 `url` は生の内容を保護）
	const bool IsCss = Language == Lang::CSS;
	// 逐語節点の範囲収集
	const auto Visit = [&Ranges, &Src, IsCss](const TSNode Cur) -> bool {
		// 解析器の節点種別を先に確定し，範囲判定と子孫走査で共有する
		const std::string_view Type = ts_node_type(Cur);
		// `ERROR` の内側へ降りない事の返戻
		if(Type == "ERROR") return false;
		// 引用符無 URL の内容か
		const bool IsUrl = IsCss && Src.IsCssUrlArguments(Cur);
		if(IsUrl || NodeKind::BracketOpaque.Contains(Type) && !ts_node_has_error(Cur)) {
			// 後段の単調走査が使える様に先行順の範囲を其の儘追記する
			Ranges.emplace_back(ts_node_start_byte(Cur), ts_node_end_byte(Cur));
		}
		// url の中身を除く子孫へ降りる事の返戻
		return !IsUrl;
	};
	if(Visit(Node)) WalkChildrenCursor(Node, Visit);
	// 終了
	return;
}

/**
 * 開き→閉じ／閉じ→開きの括弧ペア対応の構築関数（文字列・コメント・不透明リテラル内の括弧は無視）
 * @param Source ソースコード
 * @param Language 対象言語
 * @param OpaqueRanges 括弧計数から除外する範囲（開始バイト昇順・非重複）
 * @param CloseOf 開き括弧位置→閉じ括弧位置の対応（出力）
 * @param OpenOf 閉じ括弧位置→開き括弧位置の対応（出力）
 */
void BracketPairs::Build(
	const std::string &Source,
	const Lang Language,
	const std::vector<std::pair<uint32_t, uint32_t>> &OpaqueRanges,
	std::unordered_map<uint32_t, uint32_t> &CloseOf,
	std::unordered_map<uint32_t, uint32_t> &OpenOf
) {
	// 呼出側が保持する既存の対応へ追記し，再確保による全要素移動を抑える
	const size_t Reserve = (Source.size() >> 5) + 16;
	// CloseOf の領域確保
	CloseOf.reserve(Reserve);
	// OpenOf の領域確保
	OpenOf.reserve(Reserve);
	// 積重の深さは括弧のネスト深度で入力の大きさに比例しない為，入力寸法での予約は無駄に大きい
	std::vector<uint32_t> OpenStack;
	// OpenStack の領域確保
	OpenStack.reserve(0X40);
	// 走査範囲の上端
	const size_t Size = Source.size();
	// 不透明範囲は昇順・非重複で走査位置も単調増加の為，単調前進カーソルで判定する（全体で線形量）
	size_t OpaqueCursor = 0;
	for(size_t Idx = 0; Idx < Size; ++Idx) {
		// 現在位置より前で終了した範囲の参照除外
		while(OpaqueCursor < OpaqueRanges.size() && OpaqueRanges[OpaqueCursor].second <= Idx) ++OpaqueCursor;
		// 終端が現在位置より後と確定した不透明リテラル（正規表現等）の一括読飛し
		if(OpaqueCursor < OpaqueRanges.size() && OpaqueRanges[OpaqueCursor].first <= Idx) {
			// 次の反復で範囲の次から再開
			Idx = std::min<size_t>(OpaqueRanges[OpaqueCursor].second, Size) - 1;
			continue;
		}
		// Char の初期化
		const char Char = Source[Idx];
		// コメントの行末又は `*/` 迄の一括読飛し
		if(Char == '/' && Idx + 1 < Size && (Source[Idx + 1] == '/' || Source[Idx + 1] == '*')) {
			// 行末が無い行コメントも残り全体が注釈なので走査を終える
			if(Source[Idx + 1] == '/') {
				// 行コメントの終端
				const size_t NewlinePos = Source.find('\n', Idx + 2);
				if(NewlinePos == std::string::npos) break;
				// 次の反復で `\n` の次から再開
				Idx = NewlinePos;
			} else {
				// 未閉鎖の囲みコメント以後の括弧計数除外
				const size_t End = Source.find("*/", Idx + 2);
				if(End == std::string::npos) break;
				Idx = End + 1;
			}
			continue;
		}
		// 引用符・逃避迄の一括読飛し（アポストロフィが曖昧な言語の文字内は構文木の範囲で保護）
		if(Char == '"' || Char == '\'' && !Language.IsApostropheAmbiguous() || Char == '`') {
			const char SearchChars[3] = { '\\', Char, '\0' };
			size_t Pos = Idx + 1;
			while(Pos < Size) {
				// 次の逃避又は同じ引用符迄を纏めて飛ばし，本文の逐字判定を避ける
				Pos = Source.find_first_of(SearchChars, Pos, 2);
				if(Pos == std::string::npos) {
					Pos = Size;
					break;
				}
				if(Source[Pos] == '\\' && Pos + 1 < Size) {
					Pos += 2;
					continue;
				}
				break;
			}
			Idx = Pos;
			continue;
		}
		// 種別混在時の最も近い開きへの対応付け
		if(IsOpenBracketChar(Char)) OpenStack.push_back(static_cast<uint32_t>(Idx));
		else if(IsCloseBracketChar(Char) && !OpenStack.empty()) {
			// 双方向表を同時に更新し，何方からの探索でも同じ対へ到達させる
			CloseOf[OpenStack.back()] = static_cast<uint32_t>(Idx);
			OpenOf[static_cast<uint32_t>(Idx)] = OpenStack.back();
			OpenStack.pop_back();
		}
	}
	// 終了
	return;
}

/**
 * 単一文字括弧トークンの深さ増減（開き +1／閉じ -1）の追記関数（括弧以外と複数文字は無視）
 * @param Brackets 括弧トークン（位置，増減）の昇順追記先
 * @param Source ソースコード
 * @param Start トークンの開始バイト
 * @param End トークンの終端バイト
 */
void BracketPairs::AppendBracketDelta(
	std::vector<std::pair<uint32_t, int>> &Brackets,
	const std::string &Source,
	const uint32_t Start,
	const uint32_t End
) {
	// 複数字句や空範囲は括弧一字の深度変化として扱わない
	if(End - Start == 1) {
		// 開閉括弧の深度差の追記
		if(const char Char = Source[Start]; IsOpenBracketChar(Char)) Brackets.push_back({ Start, 1 });
		else if(IsCloseBracketChar(Char)) Brackets.push_back({ Start, -1 });
	}
	// 終了
	return;
}

/**
 * JSX 要素・JSX フラグメントの仮想括弧対の追加関数
 * @param Source ソースコード
 * @param Node 走査対象のノード
 * @param CloseOf 開き括弧位置→閉じ括弧位置の対応（追記先）
 * @param OpenOf 閉じ括弧位置→開き括弧位置の対応（追記先）
 */
void BracketPairs::AddJsx(
	const std::string &Source,
	const TSNode Node,
	std::unordered_map<uint32_t, uint32_t> &CloseOf,
	std::unordered_map<uint32_t, uint32_t> &OpenOf
) {
	// 要素の開始タグ末尾と閉じタグ先頭，属性の `<` と `>` / `/>` の対応付（JSX 断片 `<>`・HTML を含む），空の起点なら終了
	if(ts_node_is_null(Node)) return;
	// カーソルを共用したタグ・子要素の先行順収集（親タグ終端と子の `<` が同位置でも親→子の登録順を保持）
	std::vector<TSNode> WorkStack, NamedKids;
	// 再帰を避け，深い要素列でも呼出走脈の領域を消費しない
	WorkStack.push_back(Node);
	HeldCursor Cursor(Node);
	// 未処理節点の先行順走査
	while(!WorkStack.empty()) {
		const TSNode Cur = WorkStack.back();
		WorkStack.pop_back();
		const std::string_view NodeType(ts_node_type(Cur));
		// 包装節点とタグ自身の対応境界字句の切替
		const bool IsWrapKind = NodeKind::JsxContainer.Contains(NodeType) || NodeKind::HtmlElementWrap.Contains(NodeType);
		const bool IsOpenKind = !IsWrapKind && NodeKind::JsxOrHtmlOpenLike.Contains(NodeType);
		uint32_t OpenEnd = 0, CloseStart = 0, OpenPos = std::numeric_limits<uint32_t>::max();
		uint32_t ClosePos = std::numeric_limits<uint32_t>::max();
		NamedKids.clear();
		// 保持カーソルを現在節点へ戻し，反復毎の生成と解放を避ける
		ts_tree_cursor_reset(&Cursor, Cur);
		// 対応境界と子節点の収集
		if(ts_tree_cursor_goto_first_child(&Cursor)) {
			do {
				const TSNode Child = ts_tree_cursor_current_node(&Cursor);
				if(IsWrapKind) {
					if(const std::string_view ChildType(ts_node_type(Child)); NodeKind::JsxOrHtmlOpenTag.Contains(ChildType)) {
						OpenEnd = ts_node_end_byte(Child);
					} else if(NodeKind::JsxOrHtmlCloseTag.Contains(ChildType)) CloseStart = ts_node_start_byte(Child);
				} else if(IsOpenKind && !ts_node_is_named(Child)) {
					if(
						const uint32_t ChildStart = ts_node_start_byte(Child), ChildEnd = ts_node_end_byte(Child);
						ChildStart < Source.size() && ChildEnd <= Source.size()
					) {
						if(
							const std::string_view Token(Source.data() + ChildStart, ChildEnd - ChildStart);
							Token == "<" && OpenPos == std::numeric_limits<uint32_t>::max()
						) OpenPos = ChildStart;
						else if(Token == ">" || Token == "/>") ClosePos = ChildStart;
					}
				}
				// JSX を含まない無名字句は除外し，名前付の子だけ探索
				if(ts_node_is_named(Child)) NamedKids.push_back(Child);
			} while(ts_tree_cursor_goto_next_sibling(&Cursor));
		}
		// 仮想括弧対の登録
		if(IsWrapKind && OpenEnd && CloseStart > OpenEnd) {
			// 要素包装は開始タグの直後から閉じタグの直前迄を本体として結ぶ
			CloseOf[OpenEnd] = CloseStart;
			OpenOf[CloseStart] = OpenEnd;
		} else if(
			!IsWrapKind && IsOpenKind && OpenPos != std::numeric_limits<uint32_t>::max() &&
			ClosePos != std::numeric_limits<uint32_t>::max() && ClosePos > OpenPos
		) {
			// 開きタグ自身は山括弧を結び，属性列の字下げ境界に使う
			CloseOf[OpenPos] = ClosePos;
			OpenOf[ClosePos] = OpenPos;
		}
		// 逆順への積込で先行順（兄弟は元ソース順）を維持
		for(size_t Idx = NamedKids.size(); Idx;) WorkStack.push_back(NamedKids[--Idx]);
	}
	// 終了
	return;
}

/**
 * PHP の代替構文の本体の仮想括弧対の追加関数
 * 波括弧の無い本体を字下げする為，開始の `:` と次の `elseif`・`else`・終端語を仮想括弧対とする
 * @param Source ソースコード
 * @param Node 走査対象のノード
 * @param CloseOf 開き括弧位置→閉じ括弧位置の対応（追記先）
 * @param OpenOf 閉じ括弧位置→開き括弧位置の対応（追記先）
 */
void BracketPairs::AddPhpColonBlocks(
	const std::string &Source,
	const TSNode Node,
	std::unordered_map<uint32_t, uint32_t> &CloseOf,
	std::unordered_map<uint32_t, uint32_t> &OpenOf
) {
	// 代替構文だけを全木から拾い，通常の波括弧ブロックには触れない
	WalkAst(
		Node,
		[&](const TSNode Cur) -> void {
			// PHP 代替構文を持つ節点の選別
			const std::string_view Type = ts_node_type(Cur);
			// 代替構文を持たない節点の返戻
			if(!NodeKind::PhpAltHost.Contains(Type)) return;
			// `colon_block` / `switch_block` は先頭の `:`，`for` / `declare` は直下の `:` が本体を開き，終端語が本体を閉じる
			uint32_t Open = std::numeric_limits<uint32_t>::max(), Close = std::numeric_limits<uint32_t>::max();
			const bool IsLeading = NodeKind::PhpAltLeadingColon.Contains(Type);
			// 名前付の本文を除き，構文を区切る無名字句だけを調べる
			ForEachChild(
				Cur,
				[&](const TSNode Child) -> void {
					// 代替構文の境界になる無名字句の探索
					if(ts_node_is_named(Child)) return;
					// 境界字句の位置取得
					const uint32_t Start = ts_node_start_byte(Child);
					// 開始・終了境界の更新
					if(
						const std::string_view Token(Source.data() + Start, ts_node_end_byte(Child) - Start);
						Token == ":" && Open == std::numeric_limits<uint32_t>::max() && (!IsLeading || Start == ts_node_start_byte(Cur))
					) Open = Start;
					else if(NodeKind::PhpAltCloser.Contains(Token)) Close = Start;
				}
			);
			// 開きコロンを持たない節点の返戻
			if(Open == std::numeric_limits<uint32_t>::max()) return;
			// 終端語を持たない `colon_block` は，本体の後の最初の字句で閉じる
			if(Close == std::numeric_limits<uint32_t>::max()) {
				Close = ts_node_end_byte(Cur);
				// 仮想の閉じを次の実字句へ置き，節点末尾の空白を本体へ含めない
				while(
					Close < Source.size() && (Source[Close] == ' ' || Source[Close] == '\t' || Source[Close] == '\n' || Source[Close] == '\r')
				) ++Close;
			}
			// 有効な閉じ位置を得られない節点の返戻
			if(Close >= Source.size() || Close <= Open) return;
			// 実括弧と同じ表へ登録し，後段の字下げ処理を共通化する
			CloseOf[Open] = Close;
			OpenOf[Close] = Open;
		}
	);
	// 終了
	return;
}
