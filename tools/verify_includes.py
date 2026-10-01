#! /usr/bin/env python3
"""使う標準記号に対する include の過不足の検査

他環境 (libstdc++ / MSVC) では推移的な取込に頼れない為，使う記号の見出を自分で取り込んで居る事を字面で確かめる
"""

import glob
import os

from pathlib import Path

import re
import sys

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

def includes_of(path: str) -> set[str]:
	"""ファイルが取込む標準見出の集合を返す
	計算量：ファイルの文字数 L に対して O(L)
	"""
	try:
		# 見出の集合の返戻
		return set(re.findall(r"#include <([a-z_]+)>", open(path, encoding="utf-8").read()))
	except OSError:
		# 読めないファイルは取込無として扱う事の返戻
		return set()

def code_only(text: str) -> str:
	"""文字列・文字のリテラルとコメントを除いたコードの字面を返す（綴りの定義やコメントの記号名を使用と数えない）
	計算量：文字数 L に対して最悪 O(L²)
	"""
	# 文字列とコメントを除く走査の準備
	out: list[str] = []
	pos: int = 0
	size: int = len(text)
	while pos < size:
		char: str = text[pos]
		# 生文字列（`R"区切(` から `)区切"` 迄）の読飛
		is_raw_head: bool = char == "R" and (not pos or not (text[pos - 1].isalnum() or text[pos - 1] == "_"))
		raw: re.Match[str]|None
		if is_raw_head and (raw := re.match(r'R"([^()\\ ]*)\(', text[pos:pos + 20])):
			close: int = text.find(")" + raw.group(1) + "\"", pos + raw.end())
			pos = size if close < 0 else close + len(raw.group(1)) + 2
			continue
		# 文字列・文字のリテラルの読飛（逆斜線の逃避を含む）
		if char in "\"'":
			pos += 1
			while pos < size and text[pos] != char and text[pos] != "\n":
				pos += 2 if text[pos] == "\\" else 1
			pos += 1
			continue
		# 行コメントと囲みコメントの読飛
		if text.startswith("//", pos):
			close = text.find("\n", pos)
			pos = size if close < 0 else close
			continue
		if text.startswith("/*", pos):
			close = text.find("*/", pos + 2)
			pos = size if close < 0 else close + 2
			continue
		out.append(char)
		pos += 1
	# コードの字面の返戻
	return "".join(out)

def main() -> int:
	"""過不足を報せ，不足が有れば非零で終える
	計算量：対象ファイル数 F・総文字数 L に対して最悪 O(F log F+L²)
	"""
	# 起動した場所に依らず作業木の根から源を探す
	os.chdir(Path(__file__).resolve().parent.parent)
	missing: int = 0
	total: int = 0
	paths: list[str] = glob.glob("src/**/*.cpp", recursive=True) + glob.glob("src/**/*.hpp", recursive=True)
	# ソース毎の使用記号と取込見出の照合
	for path in sorted(paths):
		text: str = open(path, encoding="utf-8").read()
		body: str = code_only("\n".join(line for line in text.split("\n") if not line.startswith("#include")))
		# 見出は自分の取込だけで完結させる（他所から取込まれる順は呼出側に依る）
		# 実装は対の見出も取込元に数える（自分の宣言を含む事は避けれない）
		seen: set[str] = includes_of(path) | (includes_of(path[:-4] + ".hpp") if path.endswith(".cpp") else set())
		for symbol in sorted(set(re.findall(r"std::([a-z_]+)", body))):
			header: str|None = NEED.get(symbol)
			if header is None:
				continue
			total += 1
			if header not in seen:
				print(f"FAIL include: {path} uses std::{symbol} but does not include <{header}>", file=sys.stderr)
				missing += 1
	# 不足件数の報告
	print(f"includes: {total} uses checked, {missing} missing")
	# 不足が有れば非零の返戻
	return 1 if missing else 0

if __name__ == "__main__":
	sys.exit(main())
