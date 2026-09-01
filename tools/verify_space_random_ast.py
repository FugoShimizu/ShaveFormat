#!/usr/bin/env python3
# AST の不変条件付のスペースランダム化回帰テスト
# 字句間の空白・改行・空行を変異させ，構文木の親子関係と字句値が元と一致したケースだけ採用する
# 前処理指令行・コメント・文字列・先頭インデントは変更しない
# 使い方：
# python3 tools/verify_space_random_ast.py
# python3 tools/verify_space_random_ast.py typescript tsx ruby
from __future__ import annotations
import ast
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
from verify_semantic_preservation import DUMPAST, FMT, ROOT, Node, parse_dump, has_error

# 空白変異試験の実行条件
MAX_CHARS = os.environ.get("MAX_CHARS", "64")
TARGET_OK = 20
MAX_ATTEMPTS = 2000
SEED = 20260423
COMMAND_TIMEOUT = 120
Fingerprint = tuple[str, str, str | None, tuple["Fingerprint", ...]]

CASES = [
	("c", ".c", "c_like", "int f(int a, int b) { int x = a + b; if(x > 10) return x; return x * 2; }\n"),
	(
		"cpp",
		".cpp",
		"c_like",
		"#include <vector>\nstruct A : B { int f(const std::vector<int> &v) { if(v.empty()) return 0; return v[0] + 1; } };\n"
	),
	(
		"csharp",
		".cs",
		"c_like",
		"using System; class C { static string F(object x) { if(x is int y) return y.ToString(); return (x as string) ?? \"\"; } }\n"
	),
	("java", ".java", "c_like", "class C { static int f(int a, int b) { if(a > b) return a + b; return a - b; } }\n"),
	("go", ".go", "c_like", "package p\nfunc f(a int, b int) []int { if a > b { return []int{a + b} }\nreturn []int{a - b} }\n"),
	("rust", ".rs", "c_like", "fn f(a: i32, b: i32) -> Option<i32> { if a > b { Some(a + b) } else { None } }\n"),
	("kotlin", ".kt", "c_like", "fun f(a: Int, b: Int): Int { return if(a > b) a + b else a - b }\n"),
	("ruby", ".rb", "hash_line", "def f(a, b)\n  x = a + b\n  if x > 10 then x else x * 2 end\nend\n"),
	("python", ".py", "hash_line", "def f(a: int, b: int) -> int:\n    x = a + b\n    return x if x > 10 else x * 2\n"),
	("javascript", ".js", "js_like", "function f(a, b) { const x = a + b; return x > 10 ? x : x * 2; }\n"),
	("typescript", ".ts", "js_like", "function f<T>(x: T | null, y: number): T | number { return (x as T) ?? y; }\n"),
	(
		"tsx",
		".tsx",
		"js_like",
		"export default function C(props: { x: string | null }): JSX.Element { return <div>{props.x ?? \"\"}</div>; }\n"
	),
	("json", ".json", "json", "{\"a\": [1, 2, 3], \"b\": {\"c\": true, \"d\": null}}\n"),
	("html", ".html", "html", "<div class=\"x\"><span data-x=\"1\"></span><br /></div>\n"),
	(
		"css",
		".css",
		"css",
		".a, .b { color: red; margin: 0 auto; } @media screen and (min-width: 10px) { .a { padding: 1px 2px; } }\n"
	),
	("swift", ".swift", "c_like", "func Sum(_ x: Int, _ y: Int) -> Int { return (x + y) * 2 }\n"),
	("php", ".php", "c_like", "<?php\nfunction Sum($x, $y) { return ($x + $y) * 2; }\n"),
	("scss", ".scss", "css", "$c: red; .a { color: $c; margin: 0 auto; }\n")
]

def line_start_index(text: str, pos: int) -> int:
	"""指定位置を含む行の先頭を得る
	引数：
		text: 原文
		pos: 位置
	戻値：
		行頭位置
	"""
	# 行頭位置を返戻
	return text.rfind("\n", 0, pos) + 1

def is_preproc_line(text: str, pos: int) -> bool:
	"""指定位置の行が前処理指令か判定する
	引数：
		text: 原文
		pos: 位置
	戻値：
		前処理指令行か
	"""
	cur = line_start_index(text, pos)
	while cur < len(text) and text[cur] in " \t":
		cur += 1
	# 前処理指令行判定を返戻
	return cur < len(text) and text[cur] == "#"

def is_in_leading_indent(text: str, pos: int) -> bool:
	"""指定位置が行頭インデント内か判定する
	引数：
		text: 原文
		pos: 位置
	戻値：
		行頭インデント内か
	"""
	cur = line_start_index(text, pos)
	while cur < pos and text[cur] in " \t":
		cur += 1
	# 行頭インデント内判定を返戻
	return cur == pos

def is_word(char: str) -> bool:
	"""英数字又は下線か判定する
	引数：
		char: 対象文字
	戻値：
		語の構成文字か
	"""
	# 単語文字判定を返戻
	return char.isalnum() or char == "_"

def mutate_run(text: str, i: int, rng: random.Random) -> tuple[str, int]:
	"""指定位置から続く空白と改行を変異させる
	引数：
		text: 原文
		i: 開始位置
		rng: 乱数生成器
	戻値：
		変異後の文字列と並びの終端位置
	"""
	j = i
	while j < len(text) and text[j] in " \t\n":
		j += 1
	PREV = text[i - 1] if i else ""
	FOLL = text[j] if j < len(text) else ""
	COUNT = rng.randint(1, 3) if PREV and FOLL and is_word(PREV) and is_word(FOLL) else rng.randint(0, 3)
	ORDINARY = text[i:j] if "\n" in text[i:j] else " " * COUNT
	# 変異させた並びと並びの終わりの位置を返戻
	return rng.choice([ORDINARY] * 12 + ["\n", "\n\n", "\n\t"]), j

def mutate_spaces(text: str, mode: str, rng: random.Random) -> tuple[str, int]:
	"""字句間の空白と改行を変異させる
	計算量：原文の長さ N に対し O(N)
	引数：
		text: 原文
		mode: 変異方法
		rng: 乱数生成器
	戻値：
		変異後の原文と変異数
	"""
	out: list[str] = []
	i = 0
	N = len(text)
	changes = 0
	state = "code"
	quote = ""
	while i < N:
		char = text[i]
		nxt = text[i + 1] if i + 1 < N else ""
		# 現在の字句状態に応じた変異
		match state:
			case "code":
				# 前処理行と行頭インデントの保持
				if (
					mode == "c_like" and is_preproc_line(text, i) or
					mode in { "hash_line", "c_like", "js_like", "css", "html", "json" } and char == " " and is_in_leading_indent(text, i)
				):
					out.append(char)
					i += 1
					continue
				# コメントと文字列への遷移
				if mode in { "c_like", "js_like" } and char == "/" and nxt == "/":
					state = "line_comment"
					out += [char, nxt]
					i += 2
					continue
				if mode in { "c_like", "js_like", "css" } and char == "/" and nxt == "*":
					state = "block_comment"
					out += [char, nxt]
					i += 2
					continue
				if mode == "hash_line" and char == "#":
					state = "line_comment"
					out.append(char)
					i += 1
					continue
				if mode == "html" and text.startswith("<!--", i):
					state = "html_comment"
					out += list("<!--")
					i += 4
					continue
				if char in ("'", "\""):
					quote = char
					state = "string"
					out.append(char)
					i += 1
					continue
				# 通常空白列の変異
				if char in " \t\n":
					replacement, j = mutate_run(text, i, rng)
					out.append(replacement)
					if replacement != text[i:j]:
						changes += 1
					i = j
					continue
				out.append(char)
				i += 1
				continue
			case "line_comment":
				# 行コメント終端でのコード状態復帰
				out.append(char)
				i += 1
				if char == "\n":
					state = "code"
				continue
			case "block_comment":
				out.append(char)
				i += 1
				if char == "*" and nxt == "/":
					out.append(nxt)
					i += 1
					state = "code"
				continue
			case "html_comment":
				out.append(char)
				i += 1
				if text.startswith("-->", i - 1):
					out += list(text[i:i + 2])
					i += 2
					state = "code"
				continue
			case "string":
				out.append(char)
				i += 1
				if char == "\\" and i < N:
					out.append(text[i])
					i += 1
					continue
				if char == quote:
					state = "code"
				continue
	# 変換後テキストと変更数を返戻
	return "".join(out), changes

def run(cmd: list[str], *, input_text: str | None = None) -> subprocess.CompletedProcess[str]:
	"""外部コマンドを実行する
	引数：
		cmd: 起動引数
		input_text: 標準入力
	戻値：
		実行結果
	"""
	# 実行結果を返戻
	return subprocess.run(cmd, input=input_text, capture_output=True, text=True, timeout=COMMAND_TIMEOUT)

def run_fmt(ext: str, content: str) -> str:
	"""標準入力で整形器を実行する
	引数：
		ext: 拡張子
		content: 原文
	戻値：
		整形結果
	送出例外：
		RuntimeError: 整形器が異常終了した場合
		OSError: 整形器を起動出来ない場合
	"""
	LANG = "typescriptreact" if ext == ".tsx" else ext.lstrip(".")
	completed = run([str(FMT), "--stdin", "--lang", LANG, "--chars", MAX_CHARS, "--no-lint", "--fail-on-skip"], input_text=content)
	if completed.returncode:
		raise RuntimeError(f"format failed {ext}: {completed.stderr}\n{completed.stdout}")
	# フォーマット結果を返戻
	return completed.stdout

def ast_types(ext: str, content: str) -> tuple[Fingerprint, ...]:
	"""全節点の親子関係と字句値を保持した指紋を得る
	計算量：外部解析を除き，ダンプ文字数 B と節点数 N に対し O(B + N)
	引数：
		ext: 拡張子
		content: 原文
	戻値：
		構文木の指紋
	送出例外：
		RuntimeError: dumpast が異常終了した場合
		ValueError: AST ダンプが不正な場合
		OSError: dumpast を起動出来ない場合
	"""
	completed = run([str(DUMPAST), "-", ext.lstrip(".")], input_text=content)
	if completed.returncode:
		raise RuntimeError(f"dumpast failed {ext}: {completed.stderr}")
	roots = parse_dump(completed.stdout)
	if has_error(roots):
		# 不正構文の不採用を返戻
		return ()
	def fingerprint(node: Node) -> Fingerprint:
		"""中間節点の原文配置だけを除いた節点情報を返す
		計算量：部分木の節点数 N に対し O(N)
		引数：
			node: 指紋を求める節点
		戻値：
			節点と子の指紋
		"""
		# 節点と子の指紋を返戻
		return node.kind, node.type, None if node.children else node.text, tuple(fingerprint(child) for child in node.children)
	# 全構文木の指紋の返戻
	return tuple(fingerprint(root) for root in roots)

def main() -> int:
	"""全言語のスペースランダム化回帰試験を実行する
	戻値：
		終了コード
	送出例外：
		RuntimeError: 基準入力が不正，又は外部解析・整形が失敗した場合
		ValueError: AST ダンプが不正な場合
		OSError: 外部処理の起動又は検証資料の保存が失敗した場合
	"""
	if not FMT.is_file():
		print(f"missing formatter: {FMT}", file=sys.stderr)
		# 異常終了コードを返戻
		return 1
	if not DUMPAST.is_file():
		print(f"missing dumpast: {DUMPAST}", file=sys.stderr)
		# 異常終了コードを返戻
		return 1
	filters = set(sys.argv[1:])
	if filters - { case[0] for case in CASES }:
		print("unknown language filter", file=sys.stderr)
		# 不明な言語選択の失敗返戻
		return 1
	rng = random.Random(SEED)
	is_all_ok = True
	for name, ext, mode, source in CASES:
		if filters and name not in filters:
			continue
		baseline = run_fmt(ext, source)
		baseline_ast = ast_types(ext, source)
		if not baseline_ast:
			raise RuntimeError(f"invalid baseline: {name}")
		ok_count = 0
		skipped = 0
		print(f"== {name} ==")
		for attempt in range(1, MAX_ATTEMPTS + 1):
			mutated, changes = mutate_spaces(source, mode, rng)
			if not changes:
				skipped += 1
				continue
			if name == "python":
				try:
					ast.parse(mutated)
				except SyntaxError as e:
					skipped += 1
					continue
			if ast_types(ext, mutated) != baseline_ast:
				skipped += 1
				continue
			if name in { "javascript", "typescript", "tsx" }:
				parser_script = r'''
const ts = require("./vscode-extension/node_modules/typescript");
const fs = require("node:fs");
const data = JSON.parse(fs.readFileSync(0, "utf8"));
const kind = data.ext === ".tsx" ? ts.ScriptKind.TSX : data.ext === ".js" ? ts.ScriptKind.JS : ts.ScriptKind.TS;
const tree = ts.createSourceFile("input" + data.ext, data.code, ts.ScriptTarget.Latest, true, kind);
process.exit(tree.parseDiagnostics.length ? 1 : 0);
'''
				checked = subprocess.run(
					["node", "-e", parser_script],
					input=json.dumps({ "ext": ext, "code": mutated }),
					text=True,
					capture_output=True,
					cwd=ROOT,
					timeout=COMMAND_TIMEOUT
				)
				if checked.stderr or checked.returncode not in (0, 1):
					raise RuntimeError(f"TypeScript parser failed: {checked.stderr}")
				if checked.returncode:
					skipped += 1
					continue
			out = run_fmt(ext, mutated)
			is_unchanged = out == baseline
			ok_count += 1
			status = "OK" if is_unchanged else "NG"
			print(f"accept={ok_count} attempt={attempt} changes={changes} skipped={skipped} {status}")
			if not is_unchanged:
				failure_dir = Path(tempfile.mkdtemp(prefix=f"shaveformat-layout-{name}-"))
				print(f"failure artifacts: {failure_dir}")
				(failure_dir / "expected").write_text(baseline, encoding="utf-8")
				(failure_dir / "got").write_text(out, encoding="utf-8")
				(failure_dir / "input").write_text(mutated, encoding="utf-8")
				is_all_ok = False
				break
			if ok_count >= TARGET_OK:
				break
		if ok_count < TARGET_OK:
			print(f"INSUFFICIENT_ACCEPTED ok={ok_count} skipped={skipped}")
			is_all_ok = False
		if not is_all_ok:
			break
	print("ALL_OK" if is_all_ok else "FAILED")
	# 終了コードを返戻
	return 0 if is_all_ok else 1

if __name__ == "__main__":
	sys.exit(main())
