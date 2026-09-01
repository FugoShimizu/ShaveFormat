#include "Util/Lang.hpp"
#include <tree_sitter/api.h>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

/**
 * JSON 文字列としての符号化関数
 * 計算量：原文のバイト数 N に対し O(N)
 * @param Source 元の文字列
 * @return 制御文字・引用符を保護した文字列
 */
std::string QuoteJson(const std::string_view Source) {
	// 引用符と制御文字逃避に用いる出力領域と１６進数字
	std::string Text = "\"";
	static constexpr char Hex[] = "0123456789abcdef";
	// 原文バイトの順次符号化
	for(const unsigned char Char : Source) {
		// JSON 特殊文字の逃避接頭辞
		if(Char == '"' || Char == '\\') Text += '\\';
		// 単一行出力を保つ表示不能制御文字の Unicode 逃避化
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
 * AST を再帰的に階層付で標準出力にダンプする
 * 計算量：節点数 N と出力原文量 M に対し O(N+M)
 * @param Node ダンプ対象ノード
 * @param Source ソース文字列
 * @param Prefix 行頭の接頭辞（タブのインデントと罫線）
 * @param IsLast 親内で最後の子なら true
 */
void Dump(const TSNode Node, const std::string &Source, const std::string &Prefix, const bool IsLast) {
	// 条件成立時の返戻
	if(ts_node_is_null(Node)) return;
	// 節点状態と原文範囲の集約及び出力
	const uint32_t Start = ts_node_start_byte(Node), End = ts_node_end_byte(Node), ChildCount = ts_node_child_count(Node);
	const std::string Text = QuoteJson(std::string_view(Source).substr(Start, End - Start));
	std::printf(
		"%s%s[%s:%s] %s\n",
		Prefix.c_str(),
		IsLast ? "└─" : "├─",
		ts_node_is_named(Node) ? "N" : "U",
		QuoteJson(ts_node_is_missing(Node) ? "MISSING" : ts_node_type(Node)).c_str(),
		Text.c_str()
	);
	// 子階層へ渡す罫線接頭辞の構築と再帰出力
	const std::string Next = Prefix + (IsLast ? "\t" : "│\t");
	for(uint32_t Idx = 0; Idx < ChildCount; ++Idx) Dump(ts_node_child(Node, Idx), Source, Next, Idx + 1 == ChildCount);
	// 終了
	return;
}

/**
 * AST のノード型のみを標準出力に列挙する（`--types-only` 用）
 * 計算量：節点数 N に対し O(N)
 * @param Node 列挙対象ノード
 */
void DumpTypes(const TSNode Node) {
	// 条件成立時の返戻
	if(ts_node_is_null(Node)) return;
	// 型名又は欠落印と子節点の再帰出力
	std::puts(ts_node_is_missing(Node) ? "MISSING" : ts_node_type(Node));
	const uint32_t ChildCount = ts_node_child_count(Node);
	for(uint32_t Idx = 0; Idx < ChildCount; ++Idx) DumpTypes(ts_node_child(Node, Idx));
	// 終了
	return;
}

/**
 * dumpast エントリポイント
 * 拡張子（標準入力では次の引数，既定は tsx）で言語を判定し AST をダンプする
 * @param Argc コマンドライン引数の数
 * @param Argv コマンドライン引数の配列
 * @return 正常終了時０，引数不正時１
 */
int main(const int Argc, char *Argv[]) {
	// 入力原文と仮想を含む入力経路
	std::string Source, Path;
	bool IsTypesOnly = false;
	int ArgIdx = 1; // 最初の任意引数位置
	// 型一覧出力指定の解析
	if(Argc > 1 && std::string(Argv[1]) == "--types-only") {
		IsTypesOnly = true;
		ArgIdx = 2; // 型一覧指定後の入力経路位置
	}
	// 入力の指定が無い場合（空の入力は空の構文木として表示する為，中身の有無では判定しない）
	if(Argc <= ArgIdx) {
		std::fprintf(stderr, "使用法：dumpast [--types-only] <file>（言語は拡張子で自動判定）\n");
		std::fprintf(stderr, "　　　　dumpast [--types-only] - [ext]（標準入力から，ext=tsx 等）\n");
		std::fprintf(stderr, "       echo \"code\" | dumpast --types-only - tsx\n");
		// 入力の指定無で１を返戻
		return 1;
	}
	// 入力元に応じた原稿と仮想経路の構築
	if(Argc > ArgIdx && std::string(Argv[ArgIdx]) != "-") {
		Path = Argv[ArgIdx];
		std::ifstream Ifs(Path);
		if(!Ifs) {
			std::fprintf(stderr, "Cannot open: %s\n", Path.c_str());
			// オープン失敗時の１の返戻
			return 1;
		}
		// ファイル全体の読込
		std::ostringstream Buf;
		Buf << Ifs.rdbuf();
		if(Ifs.bad() || Buf.bad()) {
			std::fprintf(stderr, "Failed to read: %s\n", Path.c_str());
			// 読込失敗時の１の返戻
			return 1;
		}
		Source = Buf.str();
	} else {
		// 標準入力からの読込
		std::ostringstream Buf;
		Buf << std::cin.rdbuf();
		if(std::cin.bad() || Buf.bad()) {
			std::fputs("Failed to read standard input\n", stderr);
			// 読込失敗時の１の返戻
			return 1;
		}
		Source = Buf.str();
		const std::string Ext = Argc > ArgIdx + 1 ? Argv[ArgIdx + 1] : "tsx";
		Path = "x." + Ext;
	}
	// Lang::Detect での拡張子・基底名からの言語判定
	const TSLanguage *const Language = Lang::Detect(Path).TsLang();
	if(!Language) {
		std::fprintf(stderr, "Unsupported language for: %s\n", Path.c_str());
		// 未対応拡張子の場合の１の返戻
		return 1;
	}
	// 構文解析器の生成と言語設定
	TSParser *const Parser = ts_parser_new();
	if(!ts_parser_set_language(Parser, Language)) {
		std::fputs("Incompatible parser language\n", stderr);
		ts_parser_delete(Parser);
		// 文法を設定出来ない場合の失敗返戻
		return 1;
	}
	// 構文木の生成
	TSTree *const Tree = ts_parser_parse_string(Parser, nullptr, Source.c_str(), static_cast<uint32_t>(Source.size()));
	if(!Tree) {
		std::fputs("Failed to parse source\n", stderr);
		ts_parser_delete(Parser);
		// 構文木を取得出来ない場合の失敗返戻
		return 1;
	}
	// 根節点の用途別出力
	const TSNode Root = ts_tree_root_node(Tree);
	if(IsTypesOnly) DumpTypes(Root);
	else Dump(Root, Source, "", true);
	// 構文木と解析器の解放
	ts_tree_delete(Tree);
	ts_parser_delete(Parser);
	// 標準出力の完了検査
	if(std::fflush(stdout) || std::ferror(stdout)) {
		std::fputs("Failed to write syntax tree\n", stderr);
		// 出力失敗時の１の返戻
		return 1;
	}
	// 全解析完了時の正常終了
	return 0;
}
