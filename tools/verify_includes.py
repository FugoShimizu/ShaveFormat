#! /usr/bin/env python3
"""使う標準記号に対する include の過不足の検査

他環境 (libstdc++ / MSVC) では推移的なインクルードに頼れない為，使う記号の見出を自分で取り込んで居る事を字句解析と構文上の宣言・呼出で確かめる
"""

from cpp_lex import cpp_code
from verify_semantic_preservation import Node, dump_ast, parse_dump

import glob
import os

from pathlib import Path

import re
import sys

from typing import NamedTuple

# 標準記号と，其れを宣言する見出
NEED: dict[str, str] = {
	"all_of": "algorithm",
	"any_of": "algorithm",
	"array": "array",
	"atomic": "atomic",
	"back_inserter": "iterator",
	"bad_alloc": "new",
	"begin": "iterator",
	"binary_search": "algorithm",
	"chrono": "chrono",
	"condition_variable": "condition_variable",
	"copy": "algorithm",
	"copy_n": "algorithm",
	"count": "algorithm",
	"count_if": "algorithm",
	"current_exception": "exception",
	"decay_t": "type_traits",
	"distance": "iterator",
	"enable_if_t": "type_traits",
	"end": "iterator",
	"equal": "algorithm",
	"equal_to": "functional",
	"erase": "vector",
	"erase_if": "vector",
	"errc": "system_error",
	"error_code": "system_error",
	"error_condition": "system_error",
	"exception": "exception",
	"exception_ptr": "exception",
	"exchange": "utility",
	"exit": "cstdlib",
	"fclose": "cstdio",
	"ferror": "cstdio",
	"fflush": "cstdio",
	"filesystem": "filesystem",
	"fill": "algorithm",
	"find": "algorithm",
	"find_if": "algorithm",
	"fopen": "cstdio",
	"forward": "utility",
	"fprintf": "cstdio",
	"fputs": "cstdio",
	"fread": "cstdio",
	"from_chars": "charconv",
	"from_chars_result": "charconv",
	"function": "functional",
	"fwrite": "cstdio",
	"generic_category": "system_error",
	"get": "tuple",
	"getenv": "cstdlib",
	"hash": "functional",
	"initializer_list": "initializer_list",
	"int64_t": "cstdint",
	"invalid_argument": "stdexcept",
	"invoke_result_t": "type_traits",
	"iota": "numeric",
	"is_invocable_r_v": "type_traits",
	"is_invocable_v": "type_traits",
	"is_same_v": "type_traits",
	"isalnum": "cctype",
	"isalpha": "cctype",
	"isdigit": "cctype",
	"islower": "cctype",
	"isspace": "cctype",
	"length_error": "stdexcept",
	"lock_guard": "mutex",
	"lower_bound": "algorithm",
	"make_error_code": "system_error",
	"make_move_iterator": "iterator",
	"make_pair": "utility",
	"map": "map",
	"max": "algorithm",
	"memchr": "cstring",
	"memcmp": "cstring",
	"memcpy": "cstring",
	"memory_order_acquire": "atomic",
	"memory_order_relaxed": "atomic",
	"memory_order_release": "atomic",
	"min": "algorithm",
	"minmax_element": "algorithm",
	"move": "utility",
	"multimap": "map",
	"mutex": "mutex",
	"next": "iterator",
	"none_of": "algorithm",
	"nullopt": "optional",
	"numeric_limits": "limits",
	"optional": "optional",
	"out_of_range": "stdexcept",
	"pair": "utility",
	"prev": "iterator",
	"printf": "cstdio",
	"ptrdiff_t": "cstddef",
	"puts": "cstdio",
	"random_device": "random",
	"remove_if": "algorithm",
	"replace": "algorithm",
	"rethrow_exception": "exception",
	"runtime_error": "stdexcept",
	"search": "algorithm",
	"set": "set",
	"shared_ptr": "memory",
	"size_t": "cstddef",
	"sort": "algorithm",
	"span": "span",
	"stable_partition": "algorithm",
	"stable_sort": "algorithm",
	"strchr": "cstring",
	"strcmp": "cstring",
	"string": "string",
	"string_view": "string_view",
	"strlen": "cstring",
	"strncmp": "cstring",
	"strstr": "cstring",
	"swap": "utility",
	"system_category": "system_error",
	"system_error": "system_error",
	"terminate": "exception",
	"thread": "thread",
	"tie": "tuple",
	"to_string": "string",
	"tolower": "cctype",
	"toupper": "cctype",
	"transform": "algorithm",
	"tuple": "tuple",
	"uint32_t": "cstdint",
	"uint64_t": "cstdint",
	"uint8_t": "cstdint",
	"uncaught_exceptions": "exception",
	"unique": "algorithm",
	"unique_lock": "mutex",
	"unique_ptr": "memory",
	"unordered_map": "unordered_map",
	"unordered_multimap": "unordered_map",
	"unordered_set": "unordered_set",
	"upper_bound": "algorithm",
	"vector": "vector"
}

def includes_of(code: str) -> set[str]:
	"""非コード領域を除いた論理テキストから標準ヘッダを取得する
	引数：
		code: コメントとリテラルを除いた論理テキスト
	戻値：
		行頭のインクルードが指定する標準ヘッダ名の集合
	"""
	return set(re.findall(r"^[ \t]*#[ \t]*include[ \t]*<([A-Za-z_0-9./]+)>", code, re.M))

class HeaderUse(NamedTuple):
	"""標準の宣言の直接依存と，未確定の場合の候補"""

	symbol: str
	header: str|None
	candidates: tuple[str, ...]

# 標準の型を宣言するヘッダ（オーバーロードの対象型にも共有する）
TYPE_HEADERS: dict[str, str] = {
	"array": "array",
	"basic_string": "string",
	"basic_string_view": "string_view",
	"bitset": "bitset",
	"coroutine_handle": "coroutine",
	"deque": "deque",
	"error_code": "system_error",
	"error_condition": "system_error",
	"forward_list": "forward_list",
	"list": "list",
	"map": "map",
	"multimap": "map",
	"multiset": "set",
	"optional": "optional",
	"pair": "utility",
	"set": "set",
	"shared_ptr": "memory",
	"span": "span",
	"string": "string",
	"string_view": "string_view",
	"tuple": "tuple",
	"unique_ptr": "memory",
	"unordered_map": "unordered_map",
	"unordered_multimap": "unordered_map",
	"unordered_multiset": "unordered_set",
	"unordered_set": "unordered_set",
	"variant": "variant",
	"vector": "vector",
	"weak_ptr": "memory"
}

# 特殊化した swap を宣言する型（其の他の既知の型は utility の一般形を使う）
SWAP_TYPES: tuple[str, ...] = (
	"array",
	"basic_string",
	"deque",
	"forward_list",
	"list",
	"map",
	"multimap",
	"multiset",
	"optional",
	"pair",
	"set",
	"shared_ptr",
	"string",
	"tuple",
	"unique_ptr",
	"unordered_map",
	"unordered_multimap",
	"unordered_multiset",
	"unordered_set",
	"variant",
	"vector",
	"weak_ptr"
)

# 同じ名前の宣言の候補は，対象の型が未確定な場合だけ要確認として返す
OVERLOAD_HEADERS: dict[str, tuple[str, ...]] = {
	"begin": (
		"array",
		"deque",
		"forward_list",
		"iterator",
		"list",
		"map",
		"regex",
		"set",
		"span",
		"string",
		"string_view",
		"unordered_map",
		"unordered_set",
		"valarray",
		"vector"
	),
	"end": (
		"array",
		"deque",
		"forward_list",
		"iterator",
		"list",
		"map",
		"regex",
		"set",
		"span",
		"string",
		"string_view",
		"unordered_map",
		"unordered_set",
		"valarray",
		"vector"
	),
	"erase": ("deque", "forward_list", "list", "string", "vector"),
	"erase_if": ("deque", "forward_list", "list", "map", "set", "string", "unordered_map", "unordered_set", "vector"),
	"get": ("array", "tuple", "utility", "variant"),
	"hash":
	("bitset", "coroutine", "functional", "memory", "optional", "string", "string_view", "system_error", "variant", "vector"),
	"move": ("algorithm", "utility"),
	"swap": tuple(sorted({ TYPE_HEADERS[name] for name in SWAP_TYPES } | { "utility" }))
}

def standard_dependencies(code: str) -> list[HeaderUse]:
	"""実引数数と構文上の対象型から，標準のオーバーロードの宣言を照合する
	型が未確定な形は違反と決めず，要確認の候補を返す
	計算量：構文解析費用 P・ノード数 N・型名の総文字数 B に対して O(P+N+B)
	引数：
		code: コメントとリテラルを除いた論理テキスト
	戻値：
		使用毎の確定した直接依存と，未確定な候補
	"""
	found: list[HeaderUse] = []
	# 多義でない記号は，従来通り名前だけで宣言元が決まる
	for symbol in sorted(set(re.findall(r"\bstd\s*::\s*([A-Za-z_][A-Za-z_0-9]*)\b", code))):
		if symbol in NEED and symbol not in OVERLOAD_HEADERS:
			found.append(HeaderUse(symbol, NEED[symbol], (NEED[symbol],)))
	if not any(re.search(r"\bstd\s*::\s*" + symbol + r"\b", code) for symbol in OVERLOAD_HEADERS):
		# 多義な標準名を持たない宣言元の返戻
		return found
	roots: list[Node] = parse_dump(dump_ast(code, "cpp"), code)
	values: dict[Node, str] = {}
	standard_hashes: dict[Node, int] = {}
	covered: set[int] = set()
	aliases: dict[str, str] = {}
	scopes: list[dict[str, str|None]] = [{}]
	pending: list[tuple[Node, bool]] = [(root, False) for root in reversed(roots)]
	while pending:
		node: Node
		is_ready: bool
		node, is_ready = pending.pop()
		named: list[Node] = Node.get_named_children(node)
		if not is_ready:
			if node.type == "qualified_identifier" and re.fullmatch(r"(?:::\s*)?std\s*::\s*hash\s*<.*>", node.text, re.S):
				standard_hashes.update(
					(child, node.start + node.text.index("std")) for child in named if child.type == "template_type" and child.field == "name"
				)
			if node.type in ("compound_statement", "function_definition", "lambda_expression", "namespace_definition"):
				scopes.append({})
			pending.append((node, True))
			pending.extend((child, False) for child in reversed(named))
			continue
		value: str = aliases.get(node.text, "") if node.type in ("identifier", "type_identifier") else ""
		if node.type == "qualified_identifier":
			match: re.Match[str]|None = re.fullmatch(r"(?:::\s*)?std\s*::\s*([A-Za-z_][A-Za-z_0-9]*)(?:\s*<.*>)?", node.text, re.S)
			if match and match[1] in TYPE_HEADERS:
				value = match[1]
		elif node.type == "primitive_type":
			value = "primitive:" + node.text
		elif node.type in ("type_descriptor", "parenthesized_expression", "reference_declarator") and len(named) == 1:
			value = values[named[0]]
		elif node.type == "alias_declaration":
			name: Node|None = next((child for child in named if child.field == "name"), None)
			type_node: Node|None = next((child for child in named if child.field == "type"), None)
			if name is not None and type_node is not None:
				scopes[-1].setdefault(name.text, aliases.get(name.text))
				aliases[name.text] = values[type_node]
		elif node.type in ("declaration", "field_declaration", "parameter_declaration"):
			type_node = next((child for child in named if child.field == "type"), None)
			for child in named:
				if child.field != "declarator":
					continue
				declarator: Node = child
				is_indirect: bool = False
				is_function_type: bool = False
				while declarator.children:
					is_indirect |= declarator.type == "pointer_declarator"
					is_function_type |= declarator.type == "function_declarator"
					next_node: Node|None = next((part for part in declarator.children if part.field == "declarator"), None)
					if next_node is None and declarator.type in ("parenthesized_declarator", "reference_declarator"):
						parts: list[Node] = Node.get_named_children(declarator)
						next_node = parts[0] if len(parts) == 1 else None
					if next_node is None:
						break
					declarator = next_node
				if declarator.type == "identifier":
					scopes[-1].setdefault(declarator.text, aliases.get(declarator.text))
					aliases[declarator.text] = "pointer" if is_indirect else "" if is_function_type else values.get(type_node, "")
		elif node.type == "call_expression":
			function: Node|None = next((child for child in named if child.field == "function"), None)
			args: Node|None = next((child for child in named if child.field == "arguments"), None)
			operands: list[Node] = Node.get_named_children(args) if args is not None else []
			match = re.fullmatch(r"(?:::\s*)?std\s*::\s*(\w+)(?:\s*<.*>)?", function.text, re.S) if function is not None else None
			if match and match[1] in OVERLOAD_HEADERS:
				covered.add(function.start + function.text.index("std"))
				symbol: str = match[1]
				target: str = values[operands[0]] if operands else ""
				header: str|None = None
				if symbol in ("begin", "end") and len(operands) == 1:
					header = "iterator"
				elif symbol == "move":
					header = "utility" if len(operands) == 1 else "algorithm" if len(operands) in (3, 4) else None
					if len(operands) == 1:
						value = target
				elif symbol == "get" and target in ("array", "pair", "tuple", "variant") and len(operands) == 1:
					header = TYPE_HEADERS[target]
				elif symbol in ("erase", "erase_if") and len(operands) == 2 and target in TYPE_HEADERS:
					candidate: str = TYPE_HEADERS[target]
					header = candidate if candidate in OVERLOAD_HEADERS[symbol] else None
				elif symbol == "swap" and "<" not in function.text and len(operands) == 2 and target and values[operands[1]] == target:
					header = TYPE_HEADERS[target] if target in SWAP_TYPES else "utility"
				found.append(HeaderUse(symbol, header, OVERLOAD_HEADERS[symbol] if symbol in ("begin", "end") or header is None else (header,)))
		elif node in standard_hashes:
			covered.add(standard_hashes[node])
			args = next((child for child in named if child.field == "arguments"), None)
			operands = Node.get_named_children(args) if args is not None else []
			target = values[operands[0]] if len(operands) == 1 else ""
			header = TYPE_HEADERS.get(target) if target in (
				"basic_string",
				"basic_string_view",
				"bitset",
				"coroutine_handle",
				"error_code",
				"error_condition",
				"optional",
				"shared_ptr",
				"string",
				"string_view",
				"unique_ptr",
				"variant"
			) else None
			if len(operands) == 1 and re.fullmatch(
				r"(?:bool|char|char8_t|char16_t|char32_t|double|float|int|long|short|signed|unsigned|wchar_t)(?:\s+(?:int|long|unsigned))*",
				operands[0].text
			):
				header = "functional"
			if len(operands) == 1 and re.fullmatch(r"std\s*::\s*vector\s*<\s*bool\s*>", operands[0].text):
				header = "vector"
			found.append(HeaderUse("hash", header, (header,) if header is not None else OVERLOAD_HEADERS["hash"]))
		values[node] = value
		if node.type in ("compound_statement", "function_definition", "lambda_expression", "namespace_definition"):
			for name_text, previous in scopes.pop().items():
				if previous is None:
					aliases.pop(name_text, None)
				else:
					aliases[name_text] = previous
	# マクロ・関数ポインタ等，構文上の呼出へ対応しない多義名も未確定として残す
	for match in re.finditer(r"\bstd\s*::\s*(" + "|".join(OVERLOAD_HEADERS) + r")\b", code):
		if match.start() not in covered:
			found.append(HeaderUse(match[1], None, OVERLOAD_HEADERS[match[1]]))
	# 使用毎の確定・未確定な宣言元の返戻
	return found

def header_uses(code: str) -> dict[str, str]:
	"""確定した標準記号と必要なヘッダを返す
	同じ記号の別のオーバーロードの依存も，個別に保持する
	引数：
		code: コメントとリテラルを除いた論理テキスト
	戻値：
		確定した宣言元の対応（未確定の形は含めない）
	"""
	uses: dict[str, str] = {}
	seen: set[str] = includes_of(code)
	for use in standard_dependencies(code):
		if use.header is not None:
			key: str = use.symbol if use.symbol not in uses or uses[use.symbol] == use.header else use.symbol + ":" + use.header
			uses[key] = next((candidate for candidate in use.candidates if candidate in seen), use.header)
	# 確定した標準の直接依存の返戻
	return uses

def include_self_check() -> list[str]:
	"""実際の依存とコメント・リテラル内の偽の依存を区別する
	戻値：
		手書の依存数・欠落数と一致しないケースの一覧
	"""
	found: list[str] = []
	for text, expected_uses, expected_missing in (
		("#include <optional>\nstd::optional<int> value;", 1, 0),
		("std /* gap */ :: /* gap */ optional<int> value;", 1, 1),
		("#include <optional>\nstd \n :: \n optional<int> value;", 1, 0),
		("auto text = \"std :: optional\";", 0, 0),
		("// #include <optional>\nstd::optional<int> value;", 1, 1),
		("/* #include <optional> */\nstd::optional<int> value;", 1, 1),
		("auto text=R\"(\n#include <optional>\n)\";\nstd::optional<int> value;", 1, 1),
		("int value; #include <optional>\nstd::optional<int> item;", 1, 1),
		("/* prefix */ # include <optional>\nstd::optional<int> value;", 1, 0),
		("/\\\n/ std::optional<int> fake;\nint value;\n", 0, 0),
		("std::opt\\\nional<int> value;\n", 1, 1),
		("#inc\\\nlude <optional>\nstd::opt\\\nional<int> value;\n", 1, 0)
	):
		code: str = cpp_code(text)
		uses: dict[str, str] = header_uses(code)
		seen: set[str] = includes_of(code)
		if len(uses) != expected_uses or sum(header not in seen for header in uses.values()) != expected_missing:
			found.append(repr(text))
	# 宣言を持つ直接のヘッダだけで成功し，欠落した同じ依存は失敗する
	for declaration, expression, header in (
		("std::pair<int,int> P;", "std::get<0>(P)", "utility"),
		("std::array<int,3> A;", "std::get<0>(A)", "array"),
		("std::tuple<int> T;", "std::get<int>(T)", "tuple"),
		("std::variant<int> V;", "std::get<int>(V)", "variant"),
		("std::string S;", "std::erase(S, 'x')", "string"),
		("std::list<int> L;", "std::erase(L, 1)", "list"),
		("std::map<int,int> M;", "std::erase_if(M, Pred)", "map"),
		("std::array<int,3> A;", "std::swap(A,A)", "array"),
		("std::span<int> S;", "std::swap(S,S)", "utility"),
		("int X;", "std::swap(X,X)", "utility"),
		("std::pair<int,int> P;", "std::get<0>(std::move(P))", "utility"),
		("", "std::hash<std::vector<bool>>{}", "vector"),
		("", "std::move(A,B,C)", "algorithm"),
		("", "std::move(A)", "utility"),
		("", "std::hash<std::string>{}", "string"),
		("", "std::hash<int>{}", "functional"),
		("std::array<int,3> A;", "std::begin(A)", "iterator"),
		("std::array<int,3> A;", "std::end(A)", "iterator")
	):
		code = cpp_code(declaration + expression + ";")
		uses = header_uses(code)
		symbol: str = re.search(r"std::(\w+)", expression)[1]
		dependencies: list[HeaderUse] = [use for use in standard_dependencies(code) if use.symbol == symbol]
		seen = { header }
		if (
			uses.get(symbol) != header or not dependencies or
			any(use.header is None or not any(candidate in seen for candidate in use.candidates) for use in dependencies) or
			any(use.header is not None and any(candidate in includes_of(code) for candidate in use.candidates) for use in dependencies)
		):
			found.append("オーバーロードの直接依存が違う：" + expression)
	for text, symbol, header in (
		("std::array<int,3>* P; std::swap(P,P);", "swap", "utility"),
		("using A=std::array<int,3>; A V; ::std::get<0>(V);", "get", "array"),
		("std::array<int,3>& V=Other; std::swap(V,V);", "swap", "array"),
		("std::move(A); Other::hash<int> H;", "move", "utility")
	):
		uses = header_uses(text)
		if uses.get(symbol) != header or text.endswith("Other::hash<int> H;") and "hash" in uses:
			found.append("修飾・ポインタ・参照・型エイリアスの宣言元が違う：" + text)
	for header in "array", "iterator":
		for symbol in "begin", "end":
			code = "#include <" + header + ">\nstd::" + symbol + "(Unknown);"
			if header_uses(code).get(symbol) != header:
				found.append("標準で宣言された範囲アクセスの直接依存を拒否した：" + header)
	for text in (
		"std::get<0>(Unknown);",
		"std::erase(Unknown,1);",
		"std::swap(Unknown,Other);",
		"std::hash<Unknown> H;",
		"std::hash<std::string::value_type> H;",
		"std::array<int,3> A; std::swap<std::array<int,3>>(A,A);"
	):
		if not any(use.header is None for use in standard_dependencies(text)):
			found.append("未確定な宣言元を違反又は成功と決めた：" + text)
	uses_with_unknown: list[HeaderUse] = [
		use for use in standard_dependencies("std::array<int,3> A; std::get<0>(A); auto Fn=std::get<0>;") if use.symbol == "get"
	]
	if len(uses_with_unknown) != 2 or sum(use.header is None for use in uses_with_unknown) != 1:
		found.append("同じ記号の解決済の呼出で別の未確定な参照を隠した")
	# 照合結果の返戻
	return found

def main() -> int:
	"""過不足を報せ，不足が有れば非零で終える
	計算量：対象ファイル数 F・総文字数 L・構文解析費用 P・型名の総文字数 B に対して O(F log F+L+P+B)
	戻値：
		成功なら０，違反又は失敗が有れば非零の終了値
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
	"""
	# 起動した場所に依らず作業木の根から源を探す
	os.chdir(Path(__file__).resolve().parent.parent)
	lex_failures: list[str] = include_self_check()
	for failure in lex_failures:
		print(f"FAIL include lexer self-check: {failure}", file=sys.stderr)
	missing: int = len(lex_failures)
	total: int = 0
	review: int = 0
	paths: list[str] = glob.glob("src/**/*.cpp", recursive=True) + glob.glob("src/**/*.hpp", recursive=True)
	codes: dict[str, str] = { path: cpp_code(Path(path).read_text(encoding="utf-8")) for path in paths }
	# ソース毎の使用記号とインクルード見出の照合
	for path in sorted(paths):
		# 実装自身と対のヘッダで，既に読んだ論理テキストを共有する
		seen: set[str] = includes_of(codes[path]) | (includes_of(codes.get(path[:-4] + ".hpp", "")) if path.endswith(".cpp") else set())
		for use in standard_dependencies(codes[path]):
			if use.header is None:
				print(f"REVIEW include: {path} std::{use.symbol} declaration unresolved; candidates {use.candidates}", file=sys.stderr)
				review += 1
				continue
			symbol: str = use.symbol
			header: str = use.header
			total += 1
			if not any(candidate in seen for candidate in use.candidates):
				print(f"FAIL include: {path} uses std::{symbol} but does not include <{header}>", file=sys.stderr)
				missing += 1
	# 不足件数の報告
	print(f"includes: {total} uses checked, {missing} missing, {review} review")
	# 不足が有れば非零の返戻
	return 1 if missing else 0

if __name__ == "__main__":
	sys.exit(main())
