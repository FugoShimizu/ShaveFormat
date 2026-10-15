#include "Util/Lang.hpp"
#include "Util/ParserAllocator.hpp"
#include <tree_sitter/api.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

/**
 * 入力の読込と出力バッファへの書込を確認したソースの取得
 * 計算量：入力のバイト数 B に対し O(B)
 * @param Input 読込元のストリーム
 * @param Src 完了したソースの格納先
 * @return 入力エラーの無い EOF に達し，全バイトを出力バッファへ書き込めれば true
 * @throws std::bad_alloc ソース又は出力バッファのメモリを確保出来ない場合
 * @throws std::length_error ソースの最大長を超える場合
 */
static bool IsSourceReadSuccessful(std::FILE *const Input, std::string &Src) {
	// 読込を開始出来ない状態の失敗の返戻
	if(!Input || std::ferror(Input)) return false;
	std::ostringstream Output;
	char Buffer[4096];
	while(1) {
		const size_t Count = std::fread(Buffer, 1, sizeof(Buffer), Input);
		if(Count) Output.write(Buffer, static_cast<std::streamsize>(Count));
		// 入力のエラーを EOF と区別し，出力バッファの失敗も伝える
		if(std::ferror(Input) || !Output) return false;
		if(Count < sizeof(Buffer)) {
			// 正常な EOF 以外の短い読込の失敗の返戻
			if(!std::feof(Input)) return false;
			break;
		}
	}
	Src = Output.str();
	// 読込と書込の完了の返戻
	return true;
}

/**
 * JSON 文字列としてのエンコーディング関数
 * 計算量：原文のバイト数 N に対し O(N)
 * @param Src 元の文字列
 * @return 制御文字・引用符を保護した文字列
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::string QuoteJson(const std::string_view Src) {
	// 引用符と制御文字エスケープに用いる出力領域と１６進数字
	std::string Text = "\"";
	static constexpr char Hex[] = "0123456789abcdef";
	// 原文バイトの順次エンコーディング
	for(const unsigned char Char : Src) {
		// JSON 特殊文字のエスケープ接頭辞
		if(Char == '"' || Char == '\\') Text += '\\';
		// 単一行出力を保つ表示不能制御文字の Unicode エスケープ化
		if(Char < 0X20) { // ASCII 制御文字の上限
			Text += "\\u00";
			Text += Hex[Char >> 4]; // 上位４ビット
			Text += Hex[Char & 0XF]; // 下位４ビットのマスク
		} else Text += static_cast<char>(Char);
	}
	Text += '"';
	// 可逆な文字列の返戻
	return Text;
}

/**
 * AST の単一ノードの階層付出力
 * 計算量：出力の文字数 M に対し O(M)
 * @param Src ソース文字列
 * @param Node 出力対象ノード
 * @param Prefix 行頭の接頭辞
 * @param IsLast 親内で最後の子か
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 文字列の要素数上限を超える場合
 */
void DumpNode(const std::string &Src, const TSNode Node, const std::string &Prefix, const bool IsLast) {
	// ノード状態と原文範囲の出力
	const uint32_t Start = ts_node_start_byte(Node), End = ts_node_end_byte(Node);
	const std::string Text = QuoteJson(std::string_view(Src).substr(Start, End - Start));
	std::printf(
		"%s%s[%s:%s] %s\n",
		Prefix.c_str(),
		IsLast ? "└─" : "├─",
		ts_node_is_named(Node) ? "N" : "U",
		QuoteJson(ts_node_is_missing(Node) ? "MISSING" : ts_node_type(Node)).c_str(),
		Text.c_str()
	);
	// 終了
	return;
}

/**
 * 明示した作業列に依る AST の先行順の出力
 * 計算量：ノード数 N と出力の文字数 M に対し O(N+M)
 * @param Src ソース文字列
 * @param Node 出力対象の根ノード
 * @param IsTypesOnly 型名だけを出力するか
 * @param IsSpans 階層とバイト範囲だけを出力するか
 * @return カーソルが子の対応を全て辿れたか
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool IsDumpSuccessful(const std::string &Src, const TSNode Node, const bool IsTypesOnly, const bool IsSpans) {
	// ノードが無い場合の終了
	if(ts_node_is_null(Node)) return true;
	// 走査中の祖先と共有する罫線接頭辞
	struct Frame {
		TSNode Node;
		uint32_t ChildCount;
		uint32_t NextChild;
		bool IsLast;
		size_t PrefixSize;
		const char *Field;
	}; // 先行順走査の作業枠
	std::vector<Frame> Pending = { { Node, ts_node_child_count(Node), 0, true, 0, nullptr } };
	std::string Prefix;
	TSTreeCursor Cursor = ts_tree_cursor_new(Node);
	// 各ノードの初訪問での出力と子の順次走査
	while(!Pending.empty()) {
		Frame &Current = Pending.back();
		if(!Current.NextChild) {
			if(IsTypesOnly) std::puts(ts_node_is_missing(Current.Node) ? "MISSING" : ts_node_type(Current.Node));
			else if(IsSpans) {
				std::printf(
					"[%zu,\"%s\",%s,%u,%u,%s]\n",
					Pending.size() - 1,
					ts_node_is_named(Current.Node) ? "N" : "U",
					QuoteJson(ts_node_is_missing(Current.Node) ? "MISSING" : ts_node_type(Current.Node)).c_str(),
					ts_node_start_byte(Current.Node),
					ts_node_end_byte(Current.Node),
					QuoteJson(Current.Field ? Current.Field : "").c_str()
				);
			} else DumpNode(Src, Current.Node, Prefix, Current.IsLast);
		}
		if(Current.NextChild < Current.ChildCount) {
			const bool HasMoved = Current.NextChild ? ts_tree_cursor_goto_next_sibling(&Cursor) : ts_tree_cursor_goto_first_child(&Cursor);
			if(!HasMoved) {
				ts_tree_cursor_delete(&Cursor);
				// 不完全な走査を成功として返さない
				return false;
			}
			const char *const Field = ts_tree_cursor_current_field_name(&Cursor);
			const TSNode Child = ts_tree_cursor_current_node(&Cursor);
			++Current.NextChild;
			const bool IsLast = Current.NextChild == Current.ChildCount;
			if(!IsTypesOnly && !IsSpans) Prefix += Current.IsLast ? "\t" : "│\t";
			Pending.push_back({ Child, ts_node_child_count(Child), 0, IsLast, Prefix.size(), Field });
		} else {
			if(Current.ChildCount && !ts_tree_cursor_goto_parent(&Cursor)) {
				ts_tree_cursor_delete(&Cursor);
				// 子から所有者へ戻れない走査の失敗の返戻
				return false;
			}
			Pending.pop_back();
			if(!IsTypesOnly && !IsSpans) Prefix.resize(Pending.empty() ? 0 : Pending.back().PrefixSize);
		}
	}
	ts_tree_cursor_delete(&Cursor);
	// 走査の完了の返戻
	return true;
}

/**
 * dumpast エントリポイント
 * 拡張子（標準入力では次の引数，既定は tsx）で言語を判定し AST をダンプする
 * 計算量：呼び出す処理・検査の総費用 Q に対し O(Q)
 * @param Argc コマンドライン引数の数
 * @param Argv コマンドライン引数の配列
 * @return 正常終了時０，引数不正時１
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
int main(const int Argc, char *const Argv[]) {
	// 入力原文と仮想を含む入力パス
	std::string Src, Path;
	// 型一覧出力指定の解析
	const bool IsTypesOnly = Argc > 1 && std::string(Argv[1]) == "--types-only";
	const bool IsSpans = Argc > 1 && std::string(Argv[1]) == "--spans", IsSexpr = Argc > 1 && std::string(Argv[1]) == "--sexp";
	const size_t Count = static_cast<size_t>(Argc), ArgIdx = IsTypesOnly || IsSpans || IsSexpr ? 2 : 1; // 引数の数と入力パスの位置（型一覧の指定の後）
	// 入力の指定が無い場合（空の入力は空の構文木として表示する為，中身の有無では判定しない）
	if(Count <= ArgIdx) {
		std::fprintf(stderr, "使用法：dumpast [--types-only|--spans|--sexp] <file>（言語は拡張子で自動判定）\n");
		std::fprintf(stderr, "　　　　dumpast [--types-only|--spans|--sexp] - [ext]（標準入力から，ext=tsx 等）\n");
		std::fprintf(stderr, "       echo \"code\" | dumpast --types-only - tsx\n");
		// 入力の指定無の場合の１の返戻
		return 1;
	}
	// 入力元に応じたソースと仮想パスの構築
	if(std::string(Argv[ArgIdx]) != "-") {
		Path = Argv[ArgIdx];
		std::unique_ptr<std::FILE, decltype(&std::fclose)> Input(std::fopen(Path.c_str(), "r"), std::fclose);
		if(!Input) {
			std::fprintf(stderr, "Cannot open: %s\n", Path.c_str());
			// オープン失敗時の１の返戻
			return 1;
		}
		// ファイル全体の読込
		const bool IsRead = IsSourceReadSuccessful(Input.get(), Src);
		const int Closed = std::fclose(Input.release());
		if(!IsRead || Closed) {
			std::fprintf(stderr, "Failed to read: %s\n", Path.c_str());
			// 読込失敗時の１の返戻
			return 1;
		}
	} else {
		// 標準入力からの読込
		if(!IsSourceReadSuccessful(stdin, Src)) {
			std::fputs("Failed to read standard input\n", stderr);
			// 読込失敗時の１の返戻
			return 1;
		}
		const std::string Ext = Count > ArgIdx + 1 ? Argv[ArgIdx + 1] : "tsx";
		Path = "x." + Ext;
	}
	// Lang::Detect での拡張子・基底名からの言語判定
	const TSLanguage *const Lang = Lang::Detect(Path).GetTreeSitterLang();
	if(!Lang) {
		std::fprintf(stderr, "Unsupported language for: %s\n", Path.c_str());
		// 未対応拡張子の場合の１の返戻
		return 1;
	}
	// パーサーの生成と言語設定
	ParserAllocator::Install();
	TSParser *const Parser = ts_parser_new();
	if(!ts_parser_set_language(Parser, Lang)) {
		std::fputs("Incompatible parser language\n", stderr);
		ts_parser_delete(Parser);
		// 文法を設定出来ない場合の失敗の返戻
		return 1;
	}
	// 構文木の生成
	TSTree *const Tree = ts_parser_parse_string(Parser, nullptr, Src.c_str(), static_cast<uint32_t>(Src.size()));
	if(!Tree) {
		std::fputs("Failed to parse source\n", stderr);
		ts_parser_delete(Parser);
		// 構文木を取得出来ない場合の失敗の返戻
		return 1;
	}
	// 根ノードの用途別出力
	const TSNode Root = ts_tree_root_node(Tree);
	bool WasDumped;
	if(IsSexpr) {
		// tree-sitter の S 式を加工せず返し，独立した文法の比較へ渡す
		const std::unique_ptr<char, decltype(&std::free)> Text(ts_node_string(Root), std::free);
		WasDumped = Text && std::puts(Text.get()) > -1;
	} else WasDumped = IsDumpSuccessful(Src, Root, IsTypesOnly, IsSpans);
	// 構文木とパーサーの解放
	ts_tree_delete(Tree);
	ts_parser_delete(Parser);
	if(!WasDumped) {
		std::fputs("Failed to traverse source tree\n", stderr);
		// 不完全な出力の失敗の返戻
		return 1;
	}
	// 標準出力の完了検査
	if(std::fflush(stdout) || std::ferror(stdout)) {
		std::fputs("Failed to write syntax tree\n", stderr);
		// 出力失敗時の１の返戻
		return 1;
	}
	// 全解析完了時の正常終了の返戻
	return 0;
}
