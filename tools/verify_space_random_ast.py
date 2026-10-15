#! /usr/bin/env python3
# 構文木の不変条件付の空白変異の退行検査
# トークン間の空白・改行・空行を変異させ，構文木の親子関係とトークン値が元と一致したケースだけ採用する
# プリプロセッサディレクティブ行・コメント・文字列・先頭インデントは変更しない
# 使方：
# python3 tools/verify_space_random_ast.py # 全言語
# python3 tools/verify_space_random_ast.py typescript tsx ruby # 言語の指定
from __future__ import annotations

from cpp_lex import cpp_number_end
from verify_case_variants import match_rust_char_literal
from verify_semantic_preservation import DUMPAST, FMT, has_error, Node, parse_dump, ROOT

import ast
import json
import os

from pathlib import Path

import random
import subprocess
import sys
import tempfile

from typing import TypeAlias

# 空白変異テストの実行条件
MAX_CHARS: str = os.environ.get("MAX_CHARS", "64")
TARGET_OK: int = 20
MAX_ATTEMPTS: int = 2000
SEED: int = 20260423
COMMAND_TIMEOUT: int = 120
Fingerprint: TypeAlias = tuple[str, str, str | None, int]

# 共通の乱数ジェネレータを全テストケースで継続して使う為にテストケースの順を保つ
CASES: list[tuple[str, str, str, str]] = [
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
	("go", ".go", "go", "package p\nfunc f(a int, b int) []int { if a > b { return []int{a + b} }\nreturn []int{a - b} }\n"),
	("rust", ".rs", "rust", "fn f(a: i32, b: i32) -> Option<i32> { if a > b { Some(a + b) } else { None } }\n"),
	("kotlin", ".kt", "c_like", "fun f(a: Int, b: Int): Int { return if(a > b) a + b else a - b }\n"),
	("kotlin-ctor", ".kt", "c_like", "class A(val constructorName: String)\nclass B : A(\"x\")\n"),
	("ruby", ".rb", "hash_line", "def f(a, b)\n  x = a + b\n  if x > 10 then x else x * 2 end\nend\n"),
	("ruby-not", ".rb", "hash_line", "x = not (a and b)\ny = defined? (a)\nclass << obj\nend\n"),
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

def is_word(char: str) -> bool:
	"""英数字又は下線か判定する
	引数：
		char: 対象文字
	戻値：
		語の構成文字か
	"""
	# 単語文字判定の返戻
	return char.isalnum() or char == "_"

def mutate_run(rng: random.Random, text: str, i: int, is_newline_terminated: bool) -> tuple[str, int]:
	"""指定位置から続く空白と改行を変異させる
	計算量：指定位置以後の文字数 L に対して O(L)
	引数：
		rng: 乱数ジェネレータ
		text: 原文
		i: 開始位置
		is_newline_terminated: 改行で文が終わる言語か
	戻値：
		変異後の文字列と並びの終端位置
	"""
	# 連続空白の終端と両端トークンの取得
	j: int = i
	while j < len(text) and text[j] in " \t\n":
		j += 1
	prev: str = text[i - 1] if i else ""
	following: str = text[j] if j < len(text) else ""
	count: int = rng.randint(1, 3) if prev and following and is_word(prev) and is_word(following) else rng.randint(0, 3)
	# 自動のセミコロンを生むトークンの後へは元に無い改行を加えず，文の区切を変えない
	breaks: list[str] = ["\n", "\n\n", "\n\t"]
	if is_newline_terminated and "\n" not in text[i:j] and (is_word(prev) or prev in ")]}\"'`" or text[i - 2:i] in ("++", "--")):
		breaks = []
	# 乱数の添字に対応する候補の順を保った，変異後の文字列と終端位置の返戻
	return rng.choice([text[i:j] if "\n" in text[i:j] else " " * count] * 12 + breaks), j

def mutate_spaces(rng: random.Random, text: str, mode: str) -> tuple[str, int]:
	"""トークン間の空白と改行を変異させる
	計算量：原文の長さ N に対し O(N)
	引数：
		rng: 乱数ジェネレータ
		text: 原文
		mode: 変異方法
	戻値：
		変異後の原文と変異数
	"""
	# トークン状態と変異数の初期化
	output: list[str] = []
	i: int = 0
	n: int = len(text)
	changes: int = 0
	state: str = "code"
	quote: str = ""
	line_end: int = -1
	indent_end: int = 0
	is_preproc: bool = False
	while i < n:
		# 飛び越えた行も１度だけ読み，改行文字は其の前の行へ所属させる
		while i > line_end:
			line_start: int = line_end + 1
			line_end = text.find("\n", line_start)
			if line_end < 0:
				line_end = n
			indent_end = line_start
			while indent_end < line_end and text[indent_end] in " \t":
				indent_end += 1
			is_preproc = indent_end < line_end and text[indent_end] == "#"
		char: str = text[i]
		nxt: str = text[i + 1] if i + 1 < n else ""
		# 現在のトークン状態に応じた変異
		match state:
			case "code":
				# 前処理行と行頭インデントの保持
				if (
					mode == "c_like" and is_preproc or
					mode in { "c_like", "css", "go", "hash_line", "html", "js_like", "json", "rust" } and char == " " and i < indent_end
				):
					output.append(char)
					i += 1
					continue
				# 桁区切を引用符と読まず，数値を１度で通過する
				if mode in { "c_like", "go", "rust" } and (char.isdigit() or char == "." and nxt.isdigit()):
					end: int = cpp_number_end(text, i)
					output.append(text[i:end])
					i = end
					continue
				# コメントと文字列への遷移
				if mode in { "c_like", "go", "js_like", "rust" } and char == "/" and nxt == "/":
					state = "line_comment"
					output += [char, nxt]
					i += 2
					continue
				if mode in { "c_like", "css", "go", "js_like", "rust" } and char == "/" and nxt == "*":
					state = "block_comment"
					output += [char, nxt]
					i += 2
					continue
				if mode == "hash_line" and char == "#":
					state = "line_comment"
					output.append(char)
					i += 1
					continue
				if mode == "html" and text.startswith("<!--", i):
					state = "html_comment"
					output += list("<!--")
					i += 4
					continue
				# ライフタイム・ラベルの開始は引用状態へ移らず，後続のコメントを保護する
				if mode == "rust" and char == "'" and not match_rust_char_literal(text, i):
					output.append(char)
					i += 1
					continue
				if char in ("\"", "'"):
					quote = char
					state = "string"
					output.append(char)
					i += 1
					continue
				# 通常空白列の変異
				if char in " \t\n":
					replacement: str
					j: int
					replacement, j = mutate_run(rng, text, i, mode == "go")
					output.append(replacement)
					if replacement != text[i:j]:
						changes += 1
					i = j
					continue
				output.append(char)
				i += 1
				continue
			case "line_comment":
				# 行コメント終端でのコード状態復帰
				output.append(char)
				i += 1
				if char == "\n":
					state = "code"
				continue
			# ブロックコメント終端でのコード状態復帰
			case "block_comment":
				output.append(char)
				i += 1
				if char == "*" and nxt == "/":
					output.append(nxt)
					i += 1
					state = "code"
				continue
			# HTML コメント終端でのコード状態復帰
			case "html_comment":
				output.append(char)
				i += 1
				if text.startswith("-->", i - 1):
					output += list(text[i:i + 2])
					i += 2
					state = "code"
				continue
			# 文字列終端でのコード状態復帰
			case "string":
				output.append(char)
				i += 1
				if char == "\\" and i < n:
					output.append(text[i])
					i += 1
					continue
				if char == quote:
					state = "code"
				continue
	# 変換後テキストと変更数の返戻
	return "".join(output), changes

def space_mutation_self_check() -> list[str]:
	"""保護領域・行の切替と変異の結果を確かめる
	計算量：ケースの本文の総文字数 B に対して O(B)
	戻値：
		本文・変異数が契約と食い違うケース
	"""
	# 同じ本文と変異した本文の区別
	cases: list[tuple[str, str, str, int]] = [
		("c_like", "   x;", "   x;", 0),
		("c_like", "  #define X \"a b\"\n  x;", "  #define X \"a b\"\n  x;", 0),
		("c_like", "  /* a\n b */x;", "  /* a\n b */x;", 0),
		("c_like", "  \"a b\\\" c\";// d e\n", "  \"a b\\\" c\";// d e\n", 0),
		("hash_line", "  # a b\n  x;", "  # a b\n  x;", 0),
		("html", "  <!-- a\n b -->x;", "  <!-- a\n b -->x;", 0),
		("js_like", "  // a b\n  x;", "  // a b\n  x;", 0),
		("c_like", "  x y", "  x\ny", 1),
		("go", "  x y", "  x  y", 1),
		("rust", "<'a>// it's kept\n", "<'a>// it's kept\n", 0),
		("rust", "<'a>x y", "<'a>x\ny", 1),
		("rust", "b' ';// it's kept\n", "b' ';// it's kept\n", 0)
	]
	found: list[str] = []
	for mode, text, expected, count in cases:
		rng: random.Random = random.Random(0)
		if mutate_spaces(rng, text, mode) != (expected, count):
			found.append(f"{mode}: 保護領域・行の切替・変異が変わった")
	# 不一致の返戻
	return found

def run(command: list[str], *, input_text: str|None = None) -> subprocess.CompletedProcess[str]:
	"""外部コマンドを実行する
	計算量：入力の文字数 L・外部処理の実行時間 T に対して O(L+T)
	引数：
		command: 起動引数
		input_text: 標準入力
	戻値：
		実行結果
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		subprocess.TimeoutExpired: 呼び出した子プロセスが制限時間内に完了しない場合
	"""
	# 実行結果の返戻
	return subprocess.run(command, input=input_text, capture_output=True, text=True, timeout=COMMAND_TIMEOUT)

def run_fmt(content: str, ext: str) -> str:
	"""標準入力でフォーマッターを実行する
	計算量：原文の文字数 L・フォーマッターの実行時間 T に対して O(L+T)
	引数：
		content: 原文
		ext: 拡張子
	戻値：
		整形結果
	送出例外：
		RuntimeError: フォーマッターが異常終了した場合
		OSError: フォーマッターを起動出来ない場合
		subprocess.TimeoutExpired: 呼び出した子プロセスが制限時間内に完了しない場合
	"""
	# 指定した拡張子での整形と終了状態の確認
	completed: subprocess.CompletedProcess[str] = run(
		[
			str(FMT),
			"--stdin",
			"--lang",
			"typescriptreact" if ext == ".tsx" else ext.lstrip("."),
			"--chars",
			MAX_CHARS,
			"--no-lint",
			"--fail-on-skip"
		],
		input_text=content
	)
	if completed.returncode:
		raise RuntimeError(f"format failed {ext}: {completed.stderr}\n{completed.stdout}")
	# 整形結果の返戻
	return completed.stdout

def fingerprint(roots: list[Node]) -> tuple[Fingerprint, ...]:
	"""全ノードの親子関係と葉のテキストを先行順の指紋へ変える
	子の数を含め，ネストした組の比較で呼出スタックを使わない
	計算量：ノード数 N に対して O(N)
	引数：
		roots: 最上位のノードの並び
	戻値：
		構造と葉のテキストを保持した指紋
	"""
	# ノードの親子関係と葉のテキストの指紋化
	pending: list[Node] = list(reversed(roots))
	parts: list[tuple[str, str, str|None, int]] = []
	while pending:
		node: Node = pending.pop()
		parts.append((node.kind, node.type, None if node.children else node.text, len(node.children)))
		pending.extend(reversed(node.children))
	# 子の数を含めて構造を保持した先行順の指紋の返戻
	return tuple(parts)

def ast_types(content: str, ext: str) -> tuple[Fingerprint, ...]:
	"""全ノードの親子関係とトークン値を保持した指紋を得る
	計算量：外部解析を除き，ダンプ文字数 B とノード数 N に対し O(B + N)
	引数：
		content: 原文
		ext: 拡張子
	戻値：
		構文木の指紋
	送出例外：
		RuntimeError: dumpast が異常終了した場合
		ValueError: AST ダンプが不正な場合
		OSError: dumpast を起動出来ない場合
		json.JSONDecodeError: 読み込んだ内容が有効な JSON でない場合
		subprocess.TimeoutExpired: 呼び出した子プロセスが制限時間内に完了しない場合
	"""
	# 構文木の取得と解析エラーの除外
	completed: subprocess.CompletedProcess[str] = run([str(DUMPAST), "-", ext.lstrip(".")], input_text=content)
	if completed.returncode:
		raise RuntimeError(f"dumpast failed {ext}: {completed.stderr}")
	roots: list[Node] = parse_dump(completed.stdout)
	if has_error(roots):
		# 不正構文を採用しない事の返戻
		return ()
	# 構文木の指紋の返戻
	return fingerprint(roots)

def typescript_shape(content: str, ext: str) -> str:
	"""TypeScript の処理系で構文木の種別の並びを得る
	計算量：外部解析を除き，ノード数 N に対して O(N)
	引数：
		content: 原文
		ext: 拡張子
	戻値：
		先行順の種別と子の数の並び（構文エラーが在れば空）
	送出例外：
		RuntimeError: 処理系が異常終了した場合
		OSError: 処理系を起動出来ない場合
		subprocess.TimeoutExpired: 呼び出した子プロセスが制限時間内に完了しない場合
	"""
	# 構文エラーの判定と先行順の種別の書出
	parser_script: str = r"""
const ts = require("./vscode-extension/node_modules/typescript");
const fs = require("node:fs");
const data = JSON.parse(fs.readFileSync(0, "utf8"));
const kind = data.ext === ".tsx" ? ts.ScriptKind.TSX : data.ext === ".js" ? ts.ScriptKind.JS : ts.ScriptKind.TS;
const tree = ts.createSourceFile("input" + data.ext, data.code, ts.ScriptTarget.Latest, true, kind);
if(tree.parseDiagnostics.length) process.exit(1);
const parts = [];
const pending = [tree];
while(pending.length) {
	const node = pending.pop();
	const children = node.getChildren(tree);
	parts.push(node.kind + ":" + children.length);
	for(let i = children.length - 1; i >= 0; --i) pending.push(children[i]);
}
process.stdout.write(parts.join(" "));
"""
	checked: subprocess.CompletedProcess[str] = subprocess.run(
		["node", "-e", parser_script],
		input=json.dumps({ "code": content, "ext": ext }),
		text=True,
		capture_output=True,
		cwd=ROOT,
		timeout=COMMAND_TIMEOUT
	)
	if checked.stderr or checked.returncode not in (0, 1):
		raise RuntimeError(f"TypeScript parser failed: {checked.stderr}")
	# 構文エラーの無い時だけの種別の並びの返戻
	return "" if checked.returncode else checked.stdout

def main() -> int:
	"""全言語のスペースランダム化回帰テストを実行する
	計算量：言語数 F・各試行の最大回数 A・解析と整形の最大時間 T に対して O(FAT)
	戻値：
		終了コード
	送出例外：
		RuntimeError: 基準入力が不正，又は外部解析・整形が失敗した場合
		ValueError: AST ダンプが不正な場合
		OSError: 外部処理の起動又は検証データの保存が失敗した場合
		json.JSONDecodeError: 読み込んだ内容が有効な JSON でない場合
		subprocess.TimeoutExpired: 呼び出した子プロセスが制限時間内に完了しない場合
	"""
	# 必要な実行ファイルと対象指定の確認
	if not FMT.is_file():
		print(f"missing formatter: {FMT}", file=sys.stderr)
		# 異常終了コードの返戻
		return 1
	if not DUMPAST.is_file():
		print(f"missing dumpast: {DUMPAST}", file=sys.stderr)
		# 異常終了コードの返戻
		return 1
	filters: set[str] = set(sys.argv[1:])
	if filters - { case[0] for case in CASES }:
		print("unknown language filter", file=sys.stderr)
		# 不明な言語選択の失敗の返戻
		return 1
	for failure in space_mutation_self_check():
		print(f"FAIL space lexer self-check: {failure}", file=sys.stderr)
		# 数値・引用の読取の失敗の返戻
		return 1
	rng: random.Random = random.Random(SEED)
	is_all_ok: bool = True
	# テストケース毎の基準出力と構文木の取得
	for name, ext, mode, src in CASES:
		if filters and name not in filters:
			continue
		baseline: str = run_fmt(src, ext)
		baseline_ast: tuple[Fingerprint, ...] = ast_types(src, ext)
		baseline_shape: str = typescript_shape(src, ext) if name in { "javascript", "typescript", "tsx" } else ""
		if not baseline_ast or name in { "javascript", "typescript", "tsx" } and not baseline_shape:
			raise RuntimeError(f"invalid baseline: {name}")
		ok_count: int = 0
		skipped: int = 0
		print(f"== {name} ==")
		# 空白変異と有効な構文木の選別
		for attempt in range(1, MAX_ATTEMPTS + 1):
			mutated: str
			changes: int
			mutated, changes = mutate_spaces(rng, src, mode)
			if not changes:
				skipped += 1
				continue
			# Python の構文検査
			if name == "python":
				try:
					ast.parse(mutated)
				except SyntaxError:
					skipped += 1
					continue
			# 構文木が変わった変異と，自動のセミコロンで文の区切が変わった JavaScript 系の変異の除外
			if ast_types(mutated, ext) != baseline_ast or baseline_shape and typescript_shape(mutated, ext) != baseline_shape:
				skipped += 1
				continue
			# 採用した変異の整形結果と基準出力の比較
			output: str = run_fmt(mutated, ext)
			is_unchanged: bool = output == baseline
			ok_count += 1
			print(f'accept={ok_count} attempt={attempt} changes={changes} skipped={skipped} {"OK" if is_unchanged else "NG"}')
			# 差異の再現データの保存
			if not is_unchanged:
				failure_dir: Path = Path(tempfile.mkdtemp(prefix=f"shaveformat-layout-{name}-"))
				print(f"failure artifacts: {failure_dir}")
				(failure_dir / "expected").write_text(baseline, encoding="utf-8")
				(failure_dir / "got").write_text(output, encoding="utf-8")
				(failure_dir / "input").write_text(mutated, encoding="utf-8")
				is_all_ok = False
				break
			if ok_count >= TARGET_OK:
				break
		# 採用数の確認
		if ok_count < TARGET_OK:
			print(f"INSUFFICIENT_ACCEPTED ok={ok_count} skipped={skipped}")
			is_all_ok = False
		if not is_all_ok:
			break
	print("ALL_OK" if is_all_ok else "FAILED")
	# 終了コードの返戻
	return 0 if is_all_ok else 1

if __name__ == "__main__":
	sys.exit(main())
