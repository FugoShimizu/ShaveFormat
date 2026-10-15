#! /usr/bin/env python3
# 各配布環境で全言語の起動・再整形・原文保持を確認する
from __future__ import annotations

from tree_work import Work, evaluate

import ast
import json
import os

from pathlib import Path

import re
import shutil
import subprocess
import sys
import tempfile

from typing import Any

if os.name == "posix":
	import resource
	import signal

# 検査の経路と実行設定
ROOT: Path = Path(__file__).resolve().parent.parent
# 中核の出力を読取専用で観測する環境（整形結果を変えず，コメントを置かないテキストを標準エラーへ出す，整形仕様 10.5）
CORE_ENV: dict[str, str] = dict(os.environ, SHAVEFMT_VERIFY_COMMENT_SEPARATION="1")
COMMAND_TIMEOUT: int = 120

def run_process(*args: Any, timeout: float = COMMAND_TIMEOUT, **keyword_args: Any) -> subprocess.CompletedProcess[Any]:
	"""時間上限付で子プロセスを実行する
	計算量：子プロセスの実行時間 T に対して O(T)
	引数：
		args: subprocess.run へ渡す位置引数
		timeout: 子プロセスを待つ上限の秒数
		keyword_args: subprocess.run へ渡す名前付引数
	戻値：
		子プロセスの実行結果
	送出例外：
		subprocess.TimeoutExpired: 待機の上限時間を超えた場合
		subprocess.CalledProcessError: check を有効にし，子プロセスが失敗した場合
		OSError: 子プロセスを起動出来ない場合
	"""
	# 実行結果の返戻
	return subprocess.run(*args, timeout=timeout, **keyword_args)

def require(cond: object, msg: object = None) -> None:
	"""検査条件を確認する
	引数：
		cond: 成功条件
		msg: 失敗時の内容
	送出例外：
		AssertionError: 条件を満たさない場合
	"""
	# 検査条件の照合
	if not cond:
		raise AssertionError(msg)
	# 終了
	return

def core(command: list[str], src: bytes) -> bytes:
	"""整形した時の中核の出力（コメントを置かないテキスト）を返す
	計算量：入力のバイト数 B・フォーマッターの実行時間 T に対して O(B+T)
	引数：
		command: フォーマッターの起動引数
		src: 整形する原文（コードを含む物）
	戻値：
		中核の出力のテキスト
	送出例外：
		subprocess.CalledProcessError: フォーマッターが失敗の終了コードを返した場合
		subprocess.TimeoutExpired: 整形が所要時間の上限を超えた場合
		ValueError: 観測の行が１６進のテキストでない場合
		AssertionError: 中核の出力を観測出来ない場合
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
	"""
	# 整形時の中核出力の観測
	result: subprocess.CompletedProcess[bytes] = run_process(command, input=src, env=CORE_ENV, capture_output=True, check=True)
	lines: list[bytes] = [
		bytes.fromhex(line[len(b"[layout] "):].decode()) for line in result.stderr.split(b"\n") if line.startswith(b"[layout] ")
	]
	# 空の観測同士の一致で比較を通さない為の観測の有無の確認
	require(lines, (command, "core output was not observed", result.stderr))
	# 中核の出力の返戻
	return b"".join(line + b"\n" for line in lines)

def command_output(*args: Any, timeout: float = COMMAND_TIMEOUT, **keyword_args: Any) -> Any:
	"""時間上限付で子プロセスの標準出力を得る
	計算量：子プロセスの実行時間 T に対して O(T)
	引数：
		args: subprocess.check_output へ渡す位置引数
		timeout: 子プロセスを待つ上限の秒数
		keyword_args: subprocess.check_output へ渡す名前付引数
	戻値：
		子プロセスの標準出力（text 等の指定に依り bytes 又は str）
	送出例外：
		subprocess.TimeoutExpired: 待機の上限時間を超えた場合
		subprocess.CalledProcessError: 子プロセスが失敗した場合
		OSError: 子プロセスを起動出来ない場合
	"""
	# 標準出力の返戻
	return subprocess.check_output(*args, timeout=timeout, **keyword_args)

def python_tree(src: str|bytes) -> str:
	"""Python の構文木のテキストを返す（関数の末尾に補う値の無い `return` は処理を変えない為に除いて比べる）
	計算量：原文の文字数 L・構文木のノード数 N に対して O(L+N)
	引数：
		src: 原文
	戻値：
		構文木のテキスト
	送出例外：
		SyntaxError: 解析する入力が対象の言語バージョンの有効な構文でない場合
	"""
	# 構文木の生成と末尾の空返戻の除去
	tree: ast.Module = ast.parse(src)
	for node in ast.walk(tree):
		if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and len(node.body) > 1:
			if isinstance(node.body[-1], ast.Return) and node.body[-1].value is None:
				node.body.pop()
	# フィールド名と子の数を保持した先行順の指紋
	pending: list[object] = [tree]
	parts: list[tuple[object, ...]] = []
	while pending:
		item: object = pending.pop()
		if isinstance(item, ast.AST):
			fields: list[tuple[str, object]] = list(ast.iter_fields(item))
			parts.append(("node", type(item).__name__, tuple(name for name, _ in fields)))
			pending.extend(value for _, value in reversed(fields))
		elif isinstance(item, list):
			parts.append(("list", len(item)))
			pending.extend(reversed(item))
		else:
			parts.append(("value", type(item).__name__, item))
	# 深さに依らず比較出来る構文木の指紋の返戻
	return repr(parts)

def verify_python_wrapping(binary: Path) -> None:
	"""Python の折返でアノテーションの評価値と型引数を保持する事を検査する
	計算量：テストケース数 F・フォーマッターの最大実行時間 T に対して O(FT)
	引数：
		binary: フォーマッターの実行ファイル
	送出例外：
		AssertionError: 呼出先の検査で期待した契約との不一致を検出した場合
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		subprocess.CalledProcessError: 呼び出した子プロセスが非零の終了値を返す場合
		subprocess.TimeoutExpired: 呼び出した子プロセスが制限時間内に完了しない場合
		ValueError: 読み込んだ値が呼出先の受け入れる形式でない場合
	"""
	addition: str = " + ".join(f"value_{i}" for i in range(24))
	future_annotation: str = f"from __future__ import annotations\nx: {addition}\n"
	generic_tuple: str = (
		"class Meta(type):\n"
		"    def __getitem__(cls, item):\n"
		"        return item\n"
		"class Receiver(metaclass=Meta): pass\n"
		"x: Receiver[int,]\n"
		"print(type(__annotations__[\"x\"]).__name__)\n"
	)
	outputs: dict[tuple[str, str], bytes] = {}
	for src in future_annotation, generic_tuple:
		width: str = "20"
		command: list[str] = [str(binary), "--stdin", "--lang", "python", "--chars", width, "--no-lint", "--fail-on-skip"]
		outputs[src, width] = run_process(command, input=src.encode(), capture_output=True, check=True).stdout
	# アノテーションの評価値と型引数の保持確認
	width: str = "20"
	future_observer: bytes = b"\nprint(__annotations__)\n"
	require(
		run_process([sys.executable, "-I", "-"], input=future_annotation.encode() + future_observer, capture_output=True, check=True)
		.stdout == run_process(
			[sys.executable, "-I", "-"],
			input=outputs[future_annotation, width] + future_observer,
			capture_output=True,
			check=True
		).stdout,
		(width, "deferred annotation changed")
	)
	require(
		run_process([sys.executable, "-I", "-"], input=generic_tuple.encode(), capture_output=True, check=True).stdout ==
		run_process([sys.executable, "-I", "-"], input=outputs[generic_tuple, width], capture_output=True, check=True).stdout,
		(width, "annotation tuple argument changed")
	)
	print("python wrapping: 2 cases, native annotation values OK")
	# 終了
	return

def verify_python_annotations(binary: Path) -> None:
	"""型注釈と実体型を区別して括弧を保持する事を検査する
	計算量：テストケース数 F・フォーマッターと解釈器の最大実行時間 T に対して O(FT)
	引数：
		binary: フォーマッターの実行ファイル
	送出例外：
		AssertionError: 呼出先の検査で期待した契約との不一致を検出した場合
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		subprocess.CalledProcessError: 呼び出した子プロセスが非零の終了値を返す場合
		subprocess.TimeoutExpired: 呼び出した子プロセスが制限時間内に完了しない場合
		ValueError: 読み込んだ値が呼出先の受け入れる形式でない場合
	"""
	# 型注釈と実体型のテストケースの構築
	cases: list[tuple[str, str]] = [
		("value: bool = 1\n", "int 1 False False\n"),
		("value: bool = 0\n", "int 0 False False\n"),
		("value: bool = True\n", "bool True True False\n"),
		("value: bool = False\n", "bool False False True\n"),
		("value: \"bool\" = 1\n", "int 1 False False\n"),
		("from __future__ import annotations\nvalue: bool = 0\n", "int 0 False False\n"),
		("bool = int\nvalue: bool = True\n", "bool True True False\n"),
		("def f(value: bool = 1) -> bool:\n    return value\nvalue = f()\n", "int 1 False False\n"),
		("def f():\n    value: bool = 0\n    return value\nvalue = f()\n", "int 0 False False\n"),
		("class C:\n    item: bool = 1\nvalue = C.item\n", "int 1 False False\n"),
		("class C:\n    ((item)): bool = 1\nvalue = C.__annotations__\n", "dict {} False False\n"),
		("(value): bool = 1\n", "int 1 False False\n"),
		("value: list[bool] = [1, 0, True, False]\n", "list [1, 0, True, False] False False\n"),
		("value: tuple[bool, ...] = (1, 0, True, False)\n", "tuple (1, 0, True, False) False False\n")
	]
	observer: str = "\nprint(type(value).__name__, str(value), value is True, value is False)\n"
	# 原文と整形結果の実行結果の照合
	observed: dict[bytes, str] = {}
	for idx, (src, expected) in enumerate(cases):
		outputs: list[bytes] = [src.encode()]
		width: str = "20"
		command: list[str] = [str(binary), "--stdin", "--lang", "python", "--chars", width, "--no-lint", "--fail-on-skip"]
		first: bytes = run_process(command, input=src.encode(), capture_output=True, check=True).stdout
		outputs.append(first)
		for output in outputs:
			if output not in observed:
				completed: subprocess.CompletedProcess[bytes] = (
					run_process([sys.executable, "-I", "-"], input=output + observer.encode(), capture_output=True, check=True, timeout=10)
				)
				observed[output] = completed.stdout.decode().replace("\r\n", "\n")
			require(observed[output] == expected, (idx, "Python value or type changed", observed[output], expected))
	print(f"python annotations: {len(cases)} cases, narrow wrapping, native values and types OK")
	# 終了
	return

def verify_postfix_scaling(binary: Path) -> None:
	"""後置連鎖の括弧の正規化が，言語と受手の形に依らず線形の作業量に収まる事を検査する
	計算量：言語・構文の組数 F・最大ノード数 N・実行時間 T に対し O(F(N+T))
	引数：
		binary: フォーマッターの実行ファイル
	送出例外：
		AssertionError: 作業量が線形の余裕を超えるか，見送・出力の不一致が在る場合
		subprocess.CalledProcessError: フォーマッターが失敗した場合
		subprocess.TimeoutExpired: 時間上限を超えた場合
		OSError: 実行ファイルを起動出来ない場合
		ValueError: 観測した作業量を読めない場合
	"""
	wrappers: tuple[tuple[str, str, str], ...] = (
		("c", "void f(){r=", ";}\n"),
		("cpp", "void f(){r=", ";}\n"),
		("csharp", "class C{void F(){r=", ";}}\n"),
		("java", "class C{void f(){r=", ";}}\n"),
		("go", "package p\nfunc f(){r=", "}\n"),
		("rust", "fn f(){r=", ";}\n"),
		("kotlin", "fun f(){r=", "}\n"),
		("swift", "func f(){r=", "}\n"),
		("php", "<?php $r=", ";\n"),
		("javascript", "r=", ";\n"),
		("javascriptreact", "r=", ";\n"),
		("typescript", "r=", ";\n"),
		("typescriptreact", "r=", ";\n"),
		("ruby", "r=", "\n"),
		("python", "r=", "\n")
	)
	for lang, head, tail in wrappers:
		member: str = "->b" if lang == "php" else ".b"
		call: str = ".call()" if lang == "ruby" else ".apply()" if lang == "java" else "()"
		shapes: list[str] = [member, "[0]", call + member, member + "()"]
		if lang in ("javascript", "javascriptreact", "typescript", "typescriptreact"):
			shapes += ["?.b", "!.b"] if lang in ("typescript", "typescriptreact") else ["?.b"]
		contexts: list[tuple[str, str]] = [(head, tail)]
		if lang in ("javascript", "javascriptreact", "typescript", "typescriptreact"):
			contexts.append((head + "new (", ")()" + tail))
		for begin, end in contexts:
			context: str = "constructor" if begin != head else "receiver"
			for suffix in sorted(shapes):
				previous: int = 0
				for count in 128, 256, 512:
					name: str = "$a" if lang == "php" else "a"
					src: bytes = (begin + "(" * count + name + (")" + suffix) * count + end).encode()
					command: list[str] = [str(binary), "--stdin", "--lang", lang, "--chars", "0", "--no-lint", "--fail-on-skip"]
					result: subprocess.CompletedProcess[bytes] = run_process(command, input=src, capture_output=True, check=True, env=CORE_ENV)
					require(b"[skip]" not in result.stderr and b"[partial]" not in result.stderr, (lang, suffix, count, result.stderr))
					# マクロ抑止と短絡の必須括弧は残り得る為，作業量の検査では名前・操作の順を照合する
					flat: bytes = re.sub(rb"[\s()]", b"", (name + suffix * count).encode())
					require(flat in re.sub(rb"[\s()]", b"", result.stdout), (lang, suffix, count, "receiver operations changed"))
					match: re.Match[bytes]|None = re.search(rb"\[work\] structure (\d+)", result.stderr)
					require(match, (lang, suffix, count, "no structure observation"))
					work: int = int(match[1])
					if previous:
						require(work * 10 < previous * 26, (lang, suffix, count, previous, work))
					previous = work
					print(f"postfix work: {lang} {context} {suffix} {count} {work}")
	# 終了
	return

def verify_keyword_names(binary: Path) -> None:
	"""文脈で識別子に為る語の呼出と，真の待機・生成の式の実行結果を照合する
	計算量：入力数 F・言語数 L・幅数 W・各実行の時間 T に対し O(FLWT)
	引数：
		binary: フォーマッターの実行ファイル
	送出例外：
		AssertionError: 呼出の値又は構文を保持しない場合
		subprocess.CalledProcessError: 処理系又はフォーマッターが失敗した場合
		subprocess.TimeoutExpired: 実行の時間上限を超えた場合
		OSError: 実行ファイルを起動出来ない場合
	"""
	cases: list[str] = []
	for name in "await", "yield":
		for source in (
			"var NAME = x => x; console.log(NAME(1));",
			"function f() { var NAME = x => x; return NAME(1); } console.log(f());",
			"function f(NAME) { return NAME(1); } console.log(f(x => x));",
			"function NAME(x) { return x; } console.log(NAME(1));",
			"var NAME = x => x; function f() { return NAME(1); } console.log(f());",
			"var NAME = x => x; const f = x => x; console.log(f(NAME(1)));",
			"var NAME = x => x; if(NAME(1)) console.log(1);",
			"var NAME = x => ({value:x}); console.log(NAME(1).value);",
			"var NAME = x => [x]; console.log(NAME(1)[0]);",
			"var NAME = x => () => x; console.log(NAME(1)());",
			"async function f() { function g(NAME) { return NAME(1); } console.log(g(x => x)); } f();",
			"var NAME = x => x; console.log(NAME(/* value */1));",
			"var NAME = (...x) => x.length; console.log(NAME() + NAME(1,2) - 1);",
			"var NAME = x => x; console.log((NAME)(1));"
		):
			cases.append(source.replace("NAME", name))
	cases.extend(
		(
			"async function f() { return await (Promise.resolve(1)); } f().then(console.log);",
			"async function f() { return (await (0, Promise.resolve.bind(Promise))(1)).valueOf(); } f().then(console.log);",
			"function* f() { yield (1); } console.log(f().next().value);",
			"function* f() { function g(yield) { return yield(1); } yield g(x => x); } console.log(f().next().value);",
			"async function f() { function g() { var await = x => x; return await(1); } return await (g()); } f().then(console.log);"
		)
	)
	# 短絡の境界・構築の結合・受手の演算は，共有する括弧判定でも保持する
	cases.extend(
		(
			"let a = null, v; try { v = (a?.b).c; } catch(e) { v = 1; } console.log(v);",
			"let a = null, v; try { v = (a?.b)(); } catch(e) { v = 1; } console.log(v);",
			"let a = null, v; try { v = (a?.b)[0]; } catch(e) { v = 1; } console.log(v);",
			"const f = () => ({C: class {v = 1;}}); console.log(new (f().C)().v);",
			"const a = {C: class {v = 1;}}; console.log(new (a?.C)().v);",
			"class C {m() {return 1;}} console.log((new C).m());",
			"const a = 2, b = 1; console.log((a - b).valueOf());"
		)
	)
	contexts: list[tuple[str, str]] = [("commonjs", source) for source in cases]
	contexts.append(("module", "export {}; console.log(await Promise.resolve(1));"))
	contexts.append(("module", "console.log(await Promise.resolve(1));"))
	contexts.append(("module", "const f = x => x; console.log(await (0, f)(1));"))
	contexts.append(("commonjs", "var {f: await} = {f: x => x}; console.log(await(1));"))
	for idx, (goal, original) in enumerate(contexts):
		native: list[str] = ["node", "--input-type=" + goal, "-e"]
		require(run_process(native + [original], capture_output=True, check=True).stdout == b"1\n", idx)
		for lang in "javascript", "javascriptreact", "typescript", "typescriptreact":
			for width in "0", "20", "128":
				command: list[str] = [str(binary), "--stdin", "--lang", lang, "--chars", width, "--no-lint", "--fail-on-skip"]
				result: subprocess.CompletedProcess[str] = run_process(command, input=original, text=True, capture_output=True, check=True)
				require(not result.stderr, (idx, lang, width, result.stderr))
				require(run_process(native + [result.stdout], capture_output=True, check=True).stdout == b"1\n", (idx, lang, width))
	print(f"keyword names and postfix preservation: {len(contexts)} inputs, 4 routes, 3 widths, native values OK")
	# 終了
	return

def verify_restricted_line_breaks(binary: Path) -> None:
	"""改行で終わる文と，改行を禁じる送出・委譲を４文法で照合する
	計算量：入力数 F・言語数 L・幅数 W・各実行の時間 T に対し O(FLWT)
	引数：
		binary: フォーマッターの実行ファイル
	送出例外：
		AssertionError: 文の境界・実行値又は見送を保持しない場合
		subprocess.CalledProcessError: 処理系又はフォーマッターが失敗した場合
		subprocess.TimeoutExpired: 実行の時間上限を超えた場合
		OSError: 実行ファイルを起動出来ない場合
	"""
	# 期待する値は自動セミコロン挿入の規則から定める
	breaks: tuple[str, ...] = "\n", "\r", "\r\n", "\u2028", "\u2029", "/*\n*/", "/*\r*/", "// note\n"
	for separator in breaks:
		for value in "(1)", "[1]", "-1", "+1", "/a/.test('a')", "`text`":
			original: str = (
				"function f(){return" + separator + value + ";} function* g(){yield" + separator + value +
				";} console.log(f(), g().next().value);"
			)
			native: list[str] = ["node", "--input-type=commonjs", "-e"]
			require(run_process(native + [original], capture_output=True, check=True).stdout == b"undefined undefined\n")
			for lang in "javascript", "javascriptreact", "typescript", "typescriptreact":
				for width in "0", "20":
					command: list[str] = [str(binary), "--stdin", "--lang", lang, "--chars", width, "--no-lint", "--fail-on-skip"]
					result: subprocess.CompletedProcess[str] = run_process(command, input=original, text=True, capture_output=True, check=True)
					require(
						run_process(native + [result.stdout], capture_output=True, check=True).stdout == b"undefined undefined\n",
						(lang, width, separator, value, result.stdout)
					)
		for original in "function f(){throw" + separator + "(1);}", "function* g(){yield" + separator + "*[1];}":
			require(run_process(["node", "--check"], input=original, text=True, capture_output=True).returncode)
			for lang in "javascript", "javascriptreact", "typescript", "typescriptreact":
				rejected: subprocess.CompletedProcess[bytes] = (
					run_process([str(binary), "--stdin", "--lang", lang, "--no-lint"], input=original.encode(), capture_output=True)
				)
				require(
					not rejected.returncode and rejected.stdout == original.encode() and b"[skip]" in rejected.stderr,
					(lang, original, rejected)
				)
	# ラベルを次の行から取り込まず，通常の同じ行の値は保持する
	for original, expected in (
		("let n=0; outer:for(let i=0;i<2;i++){for(let j=0;j<2;j++){n++;break\nouter;}}console.log(n);", b"2\n"),
		("let n=0; outer:for(let i=0;i<2;i++){for(let j=0;j<2;j++){n++;continue\nouter;}}console.log(n);", b"4\n"),
		("function f(){return (1);}function* g(){yield (2);}console.log(f(),g().next().value);", b"1 2\n")
	):
		require(run_process(["node", "-e", original], capture_output=True, check=True).stdout == expected)
		for lang in "javascript", "javascriptreact", "typescript", "typescriptreact":
			result = run_process(
				[str(binary), "--stdin", "--lang", lang, "--no-lint", "--fail-on-skip"],
				input=original,
				text=True,
				capture_output=True,
				check=True
			)
			require(
				run_process(["node", "-e", result.stdout], capture_output=True, check=True).stdout == expected,
				(lang, original, result.stdout)
			)
	for lang in "typescript", "typescriptreact":
		original = "abstract\nclass C {}\n"
		result = run_process(
			[str(binary), "--stdin", "--lang", lang, "--no-lint", "--fail-on-skip"],
			input=original,
			text=True,
			capture_output=True,
			check=True
		)
		require(re.search(r"abstract;\s+class C", result.stdout), (lang, result.stdout))
	# クラスの改行はフィールドを作り，同じ行と static・get の改行は修飾を保つ
	for lang in "javascript", "typescript":
		for original, expected in (
			("class C{async\n[\"isAdmin\"](){return false;}}console.log(new C().isAdmin(),Object.keys(new C()));", b"false [ 'async' ]\n"),
			("class C{static async\n*g(){yield 1;}}console.log(new C().g().next().value,Object.keys(C));", b"1 [ 'async' ]\n"),
			("class C{async/*\r*/[\"x\"](){return 1;}}console.log(new C().x(),Object.keys(new C()));", b"1 [ 'async' ]\n"),
			("class C{static\n*g(){yield 1;}get\nx(){return 2;}}console.log(C.g().next().value,new C().x);", b"1 2\n"),
			("class C{async [\"x\"](){return 1;}}new C().x().then(console.log);", b"1\n"),
			("class C{async\n(){return 1;}}console.log(new C().async());", b"1\n")
		):
			require(run_process(["node", "-e", original], capture_output=True, check=True).stdout == expected)
			result = run_process(
				[str(binary), "--stdin", "--lang", lang, "--no-lint", "--fail-on-skip"],
				input=original,
				text=True,
				capture_output=True,
				check=True
			)
			require(
				run_process(["node", "-e", result.stdout], capture_output=True, check=True).stdout == expected,
				(lang, original, result)
			)
		for original in "const o={async\n*g(){}};", "const f=async\n(x)=>x;":
			require(run_process(["node", "--check"], input=original, text=True, capture_output=True).returncode)
			result = run_process([str(binary), "--stdin", "--lang", lang, "--no-lint"], input=original, text=True, capture_output=True)
			require(not result.returncode and result.stdout == original and "[skip]" in result.stderr, (lang, original, result))
	# TS の修飾語の後の改行もフィールドを作る
	original = "class C{readonly\n*g(){yield 1;}}console.log(new C().g().next().value,Object.keys(new C()));"
	result = run_process(
		[str(binary), "--stdin", "--lang", "typescript", "--no-lint", "--fail-on-skip"],
		input=original,
		text=True,
		capture_output=True,
		check=True
	)
	require(run_process(["node", "-e", result.stdout], capture_output=True, check=True).stdout == b"1 [ 'readonly' ]\n", result)
	print("restricted line breaks: native values, rejected syntax and modifier boundaries OK")
	# 終了
	return

def verify_javascript_comparisons(binary: Path) -> None:
	"""JS/TS の typeof 比較が結果を保つ事を検査する
	計算量：テストケース数 F・フォーマッターと解釈器の最大実行時間 T に対して O(FT)
	引数：
		binary: フォーマッターの実行ファイル
	送出例外：
		AssertionError: 呼出先の検査で期待した契約との不一致を検出した場合
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		subprocess.CalledProcessError: 呼び出した子プロセスが非零の終了値を返す場合
		subprocess.TimeoutExpired: 呼び出した子プロセスが制限時間内に完了しない場合
		ValueError: 読み込んだ値が呼出先の受け入れる形式でない場合
	"""
	# 比較演算のテストケースの構築
	cases: list[tuple[str, str]] = []
	for operator, result in ("==", "true"), ("!=", "false"):
		cases += [
			(f"const x = 1, y = 2; console.log({expr});", result) for expr in
			(f"typeof x {operator} \"number\"", f"\"number\" {operator} typeof x", f"typeof x {operator} typeof y")
		]
		cases += [
			(f"const x = 1, kind = {val}; console.log(typeof x {operator} kind);", result) for val in
			("new String(\"number\")", "{toString() { return \"number\"; }}")
		]
	# 各言語経路の整形結果の照合
	sources: list[str] = [src for src, _ in cases]
	expected: list[str] = [result for _, result in cases]
	labels: list[tuple[object, ...]] = [(idx, "original") for idx, _ in enumerate(cases)]
	for idx, (src, result) in enumerate(cases):
		# javascriptreact は javascript と同じ文法・言語の別名の為，文法の異なる３経路で見る
		for lang in "javascript", "typescript", "typescriptreact":
			width: str = "20"
			command: list[str] = [str(binary), "--stdin", "--lang", lang, "--chars", width, "--no-lint", "--fail-on-skip"]
			first: bytes = run_process(command, input=src.encode(), capture_output=True, check=True).stdout
			sources.append(first.decode())
			expected.append(result)
			labels.append((idx, lang, width, "normal"))
	# テストケース毎に独立した Node プロセスで比較結果を確認する
	# 処理系での比較結果の照合
	actual: dict[str, str] = {}
	for label, src, wanted in zip(labels, sources, expected):
		if src not in actual:
			completed: subprocess.CompletedProcess[bytes] = (
				run_process(["node", "-"], input=src.encode(), capture_output=True, check=True, timeout=10)
			)
			actual[src] = completed.stdout.decode().rstrip("\r\n")
		require(actual[src] == wanted, (label, "native comparison result changed", actual[src], wanted))
	print(f"javascript comparisons: {len(cases)} cases, 3 language routes, narrow wrapping, native results OK")
	# 終了
	return

def merge_items(items: list[object]) -> Work:
	"""子の値の評価を元の順に要求する
	計算量：子の数 N に対して O(N)
	引数：
		items: 子の値の並び
	戻値：
		評価した値の並び
	"""
	# 子の結果の収集
	result: list[object] = []
	for item in items:
		result.append((yield merge_text_work(item)))
	# 子の結果の返戻
	return result

def merge_text_work(val: object) -> Work:
	"""JSX の隣接する文字列の子を連結する
	計算量：値のノード数 N と文字列の総文字数 B に対して O(N+B)
	引数：
		val: JSX の実行結果
	戻値：
		文字列の子を連結した値
	"""
	if isinstance(val, list):
		# 各要素を連結した列の返戻
		return (yield from merge_items(val))
	if not isinstance(val, dict):
		# 要素でも列でもない値の返戻
		return val
	# JSX の文字列の子の結合
	merged: dict[object, object] = {}
	for key, item in val.items():
		if key != "children":
			merged[key] = (yield merge_text_work(item))
	if "children" in val:
		children: list[object] = []
		text_parts: list[str] = []
		for child in (yield merge_text_work(val["children"])):
			if isinstance(child, str):
				text_parts.append(child)
			else:
				if text_parts:
					children.append("".join(text_parts))
					text_parts.clear()
				children.append(child)
		if text_parts:
			children.append("".join(text_parts))
		merged["children"] = children
	# 文字列の子を連結した要素の返戻
	return merged

def merge_text(val: object) -> object:
	"""JSX の隣接する文字列の子を連結する
	計算量：値のノード数 N と文字列の総文字数 B に対して O(N+B)
	引数：
		val: JSX の実行結果
	戻値：
		文字列の子を連結した値
	"""
	# 隣接する文字列の子を連結した値の返戻
	return evaluate(merge_text_work(val))

def verify_jsx_comments(binary: Path) -> None:
	"""JSX のコメントと描画する子の並びを保持する事を検査する
	計算量：テストケース数 F・フォーマッターと解釈器の最大実行時間 T に対して O(FT)
	引数：
		binary: フォーマッターの実行ファイル
	送出例外：
		AssertionError: 呼出先の検査で期待した契約との不一致を検出した場合
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		json.JSONDecodeError: 読み込んだ内容が有効な JSON でない場合
		subprocess.CalledProcessError: 呼び出した子プロセスが非零の終了値を返す場合
		subprocess.TimeoutExpired: 呼び出した子プロセスが制限時間内に完了しない場合
		ValueError: 読み込んだ値が呼出先の受け入れる形式でない場合
	"""
	# 空の本文も含む隣接だけを結合し，要素の境界は保つ
	require(
		merge_text({ "children": ["a", "", { "children": ["b", "c"] }, "d", ""] }) ==
		{ "children": ["a", { "children": ["bc"] }, "d"] },
		"JSX の本文の結合又は要素の境界が違う"
	)
	require(merge_text({ "children": ["", ""] }) == { "children": [""] }, "JSX の空の本文が消えた")
	# JSX のコメントと子要素のテストケースの構築
	exprs: list[str] = [
		"<> a &amp; {} b </>",
		"<><a/>{/* jsxMarkerA */}<b/></>",
		"<Widget>\n a \n\n b \n</Widget>",
		"<Widget>    {}    </Widget>",
		"<Widget> {} </Widget>",
		"<Widget>&#32;&#32;{}  x  </Widget>",
		"<Widget>a\tb{}\t c</Widget>",
		"<Widget>a&#123;b&#125;{}c</Widget>",
		"<Widget>a&#13;b{}c</Widget>",
		"<Widget>a&amp;{/* jsxMarkerA */}b</Widget>",
		"<Widget>a&nbsp;b{}&#32;c</Widget>",
		"<Widget>a{/* jsxMarkerA */}b</Widget>",
		"<Widget>a{}b</Widget>",
		"<Widget> x　{}  y　</Widget>",
		"<a title=\"/* literal */\">{/* jsxMarkerA */}/* text */</a>",
		"<a title={<b>{/* jsxMarkerA */}</b>}>X</a>",
		"<a>A {/* jsxMarkerA */} B</a>",
		"<a>{/* jsxMarkerA\njsxMarkerB */}</a>",
		"<a>{/* jsxMarkerA */ /* jsxMarkerB */}</a>",
		"<a>{/* jsxMarkerA */}</a>",
		"<a>{// jsxMarkerA\n// jsxMarkerB\n}</a>",
		"<a>{// jsxMarkerA\n}</a>",
		"<a>{// jsxMarkerA */ tail\n}</a>",
		"<a>{flag && (/* jsxMarkerA */<b/>)}</a>",
		"<a>{}</a>",
		"<pre>a {} b</pre>",
		"[`A\nB`, <a>{/* jsxMarkerA */}</a>]"
	]
	# 各経路の整形とコメント保持の照合
	sources: list[str] = [f"const value = {expr};\n" for expr in exprs]
	comparisons: list[tuple[int, int, str, str]] = []
	for idx, src in enumerate(sources.copy()):
		for lang in "javascriptreact", "typescriptreact":
			width: str = "20"
			command: list[str] = [str(binary), "--stdin", "--lang", lang, "--chars", width, "--no-lint", "--fail-on-skip"]
			first: bytes = run_process(command, input=src.encode(), capture_output=True, check=True).stdout
			direct: bytes = core(command, src.encode())
			require(core(command, first) == direct)
			for marker in "jsxMarkerA", "jsxMarkerB":
				require(first.decode().count(marker) == src.count(marker), (idx, lang, width, "comment lost or duplicated", first))
				require(marker not in direct.decode(), (idx, lang, width, "comment removal failed", direct))
			for output in first, direct:
				comparisons.append((idx, len(sources), lang, width))
				sources.append(output.decode())
	# 拡張機能の既存の固定依存を使用し，JSX を実際の関数呼出へ変換して子の形も照合する
	runner: str = r"""
const ts = require(process.argv[1]);
const cp = require("node:child_process");
const sources = JSON.parse(require("node:fs").readFileSync(0, "utf8"));
const cache = new Map();
const prefix = "const Widget = \"Widget\", fragment = \"fragment\", flag = true; " +
	"function create(tag, props, ...children) { return { tag, props, children }; }\n";
const results = sources.map(source => {
	if(!cache.has(source)) {
		const tree = ts.createSourceFile("case.tsx", source, ts.ScriptTarget.Latest, true, ts.ScriptKind.TSX);
		if(tree.parseDiagnostics.length) throw new Error(JSON.stringify(tree.parseDiagnostics.map(d => d.messageText)));
		const compiled = ts.transpileModule(
			source,
			{
				compilerOptions: {
					target: ts.ScriptTarget.ES2020,
					jsx: ts.JsxEmit.React,
					jsxFactory: "create",
					jsxFragmentFactory: "fragment"
				}
			}
		).outputText;
		const execution = cp.execFileSync(
			process.execPath,
			["-"],
			{
				input: prefix + compiled + "\nconsole.log(JSON.stringify(value));",
				encoding: "utf8",
				timeout: 10000
			}
		);
		cache.set(source, execution.trim());
	}
	return cache.get(source);
});
process.stdout.write(JSON.stringify(results));
"""
	completed: subprocess.CompletedProcess[bytes] = run_process(
		["node", "-e", runner, str(ROOT / "vscode-extension/node_modules/typescript")],
		input=json.dumps(sources).encode(),
		capture_output=True,
		check=True,
		timeout=120
	)
	results: list[str] = json.loads(completed.stdout)
	require(len(results) == len(sources), "JSX native results missing")
	# 変換した JSX の実行結果の照合
	for original, output, lang, width in comparisons:
		require(
			merge_text(json.loads(results[original])) == merge_text(json.loads(results[output])),
			(original, lang, width, "JSX props or children changed", results[output])
		)
	print(f"jsx comments: {len(exprs)} cases, 2 language routes, narrow wrapping, native children OK")
	# 終了
	return

def verify_type_combinations(binary: Path) -> None:
	"""型の結合の正規化が直接の型と条件型の結果を保つ事を処理系で検査する
	計算量：テストケース数 F・フォーマッターの最大実行時間 T・型検査の実行時間 C に対して O(FT+C)
	引数：
		binary: フォーマッターの実行ファイル
	送出例外：
		subprocess.CalledProcessError: 整形又は型の同値性の検査が失敗した場合
		subprocess.TimeoutExpired: 子プロセスが所要時間の上限を超えた場合
		AssertionError: 型比較の実施件数が不足した場合
		OSError: フォーマッター又は Node.js を起動出来ない場合
		ValueError: 型検査の出力が JSON でない場合
		json.JSONDecodeError: 読み込んだ内容が有効な JSON でない場合
	"""
	# 再結合を許す合併と，簡約の時点を保つ交差を同じ比較経路へ通す
	sources: tuple[str, ...] = (
		"type X = string | (number | boolean);\n",
		"type X = {} | (string | \"foo\");\n",
		"type X = { a: 1 } | ({ b: 2 } | { c: 3 });\n",
		"type X = & (string & {});\n",
		"type X = string & ({} & {});\n",
		"type X = (string & {}) & {};\n",
		"type X = | (string | {});\n",
		"type X = | (string & {});\n",
		"type X = keyof (readonly string[]) | number;\n",
		"type X = keyof (readonly string[]) & string;\n",
		"type X = keyof (readonly string[]) extends string ? 1 : 2;\n",
		"type X = keyof (readonly string[]) | number | boolean;\n",
		"type X = boolean | keyof (readonly string[]) | number;\n",
		"type X = number | keyof (readonly string[]);\n",
		"type X = keyof (readonly [string, number]) | boolean;\n",
		"type X = (keyof (readonly string[]))[];\n",
		"type X = keyof readonly string[] | number;\n",
		"type X = (keyof readonly string[])[];\n"
	)
	comparisons: list[tuple[str, str, str, int]] = []
	for src in sources:
		for lang in "typescript", "typescriptreact":
			for width in 0, 32, 128:
				# Windows のテキストのパイプが改行を CRLF へ変えない様に，原文をバイト列で渡す
				result: subprocess.CompletedProcess[bytes] = run_process(
					[str(binary), "--stdin", "--lang", lang, "--chars", str(width), "--no-lint", "--fail-on-skip"],
					input=src.encode(),
					capture_output=True,
					check=True
				)
				require(not result.stderr, (src, lang, width, "type formatting was not complete", result.stderr))
				comparisons.append((src, result.stdout.decode(), lang, width))
	# 既存の固定依存の型チェッカーで，名前を分けた元と出力を同じ環境で比較する
	runner: str = r"""
const ts = require(process.argv[1]);
const comparisons = JSON.parse(require("node:fs").readFileSync(0, "utf8"));
const prelude = "type Equal<A, B> = (<T>() => T extends A ? 1 : 2) extends " +
	"(<T>() => T extends B ? 1 : 2) ? true : false;\n";
const source = prelude + comparisons.map(([original, formatted], index) =>
	original.replace(/\bX\b/, `Original${index}`) + formatted.replace(/\bX\b/, `Formatted${index}`) +
	`type Direct${index} = Equal<Original${index}, Formatted${index}>;\n` +
	`type Extracted${index} = Equal<Extract<Original${index} | "foo", "foo">, ` +
	`Extract<Formatted${index} | "foo", "foo">>;\n`
).join("");
let checked = 0;
for(const strict of [true, false]) {
	const file = "type-semantics.ts", options = { strict, noEmit: true, target: ts.ScriptTarget.ESNext };
	const host = ts.createCompilerHost(options), getSourceFile = host.getSourceFile.bind(host);
	host.getSourceFile = (name, version, ...rest) => name === file ?
		ts.createSourceFile(file, source, version, true) : getSourceFile(name, version, ...rest);
	const program = ts.createProgram([file], options, host), checker = program.getTypeChecker();
	const diagnostics = ts.getPreEmitDiagnostics(program).map(d => ts.flattenDiagnosticMessageText(d.messageText, "\n"));
	if(diagnostics.length) throw new Error(JSON.stringify(diagnostics));
	const types = new Map();
	for(const node of program.getSourceFile(file).statements) {
		if(ts.isTypeAliasDeclaration(node)) types.set(node.name.text, checker.typeToString(checker.getTypeFromTypeNode(node.type)));
	}
	for(let index = 0; index < comparisons.length; ++index) {
		if(types.get(`Direct${index}`) !== "true" || types.get(`Extracted${index}`) !== "true") {
			throw new Error(JSON.stringify({ comparison: comparisons[index], strict,
				direct: types.get(`Direct${index}`), extracted: types.get(`Extracted${index}`) }));
		}
		++checked;
	}
}
process.stdout.write(JSON.stringify({ checked, version: ts.version }));
"""
	completed: subprocess.CompletedProcess[str] = run_process(
		["node", "-e", runner, str(ROOT / "vscode-extension/node_modules/typescript")],
		input=json.dumps(comparisons),
		text=True,
		capture_output=True,
		check=True
	)
	checked: dict[str, Any] = json.loads(completed.stdout)
	require(checked["checked"] == len(comparisons) << 1, "type equivalence comparisons missing") # 厳密モードの有無の２通り
	print(f'type combinations: {checked["checked"]} comparisons, TypeScript {checked["version"]}, direct and Extract types equal')
	# 終了
	return

def verify_control_bodies(binary: Path) -> None:
	"""宣言本体・波括弧の書換とアンカーが元の配置・コメントに依存しない事を検査する
	計算量：テストケース数 F・フォーマッターと解釈器の最大実行時間 T に対して O(FT)
	引数：
		binary: フォーマッターの実行ファイル
	送出例外：
		subprocess.CalledProcessError: フォーマッター又は Node.js が失敗の終了コードを返した場合
		subprocess.TimeoutExpired: 子プロセスが所要時間の上限を超えた場合
		AssertionError: 中核の出力を観測出来ないか，出力・実行結果・冪等性が期待と異なる場合
		OSError: フォーマッター又は Node.js を起動出来ない場合
		ValueError: 中核の出力の観測又は Node.js の出力を読めない場合
		json.JSONDecodeError: 読み込んだ内容が有効な JSON でない場合
	"""
	# 制御節と本体のテストケースの構築
	clauses: tuple[str, ...] = (
		"do BODY while(false);",
		"for(;false;) BODY",
		"if(c) BODY",
		"if(c) if(c) BODY",
		"if(c) {} else BODY",
		"while(false) BODY"
	)
	# 波括弧の有無に依らない中核出力の照合
	sources: list[str] = []
	comparisons: list[tuple[int, int, str, str, str]] = []
	for lang in "javascript", "typescript":
		for clause in clauses:
			width: str = "20"
			command: list[str] = [str(binary), "--stdin", "--lang", lang, "--chars", width, "--no-lint", "--fail-on-skip"]
			outputs: list[bytes] = []
			# 波括弧の無い入力を比較基準にして後続の２形の中核出力を比べる為に此の順を保つ
			for body in "var x = 1;", "{ var x = 1; }", "{ /* marker */ var x = 1; }":
				original: bytes = ("function f(c) { " + clause.replace("BODY", body) + " return typeof x; }\n").encode()
				first: bytes = run_process(command, input=original, capture_output=True, check=True).stdout
				direct: bytes = core(command, original)
				require(core(command, first) == direct)
				require(re.search(rb"\{\s*var x = 1;\s*\}", direct), (lang, clause, width, direct))
				outputs.append(direct)
				comparisons.append((len(sources), len(sources) + 1, lang, clause, width))
				sources.extend((original.decode(), first.decode()))
			require(outputs[0] == outputs[1] == outputs[2], (lang, clause, width, "source layout changed output"))
		# 条件を割る見出は閉括弧の行へ本体を続け，其の行も行幅を超える時だけ本体を波括弧で次行へ送る
		for width, body, required in (
			("20", b"return 1234567890123;", rb"\)\s*\{\s*return 1234567890123;"),
			("20", b"return 1;", rb"\n\t\) return 1;"),
			("128", b"return 1;", rb"\) return 1;")
		):
			original: bytes = b"function f(c) { if(c && c && c && c && c && c) " + body + b" return 0; }\n"
			command: list[str] = [str(binary), "--stdin", "--lang", lang, "--chars", width, "--no-lint", "--fail-on-skip"]
			first: bytes = run_process(command, input=original, capture_output=True, check=True).stdout
			require(re.search(required, first), (lang, width, "whole control width ignored", first))
			comparisons.append((len(sources), len(sources) + 1, lang, "condition width", width))
			sources.extend((original.decode(), first.decode()))
		# タグ付文字列の枝に依る実行結果の照合
		for branch in "else if(!c)", "if(!c)":
			original: bytes = (
				("function tag(x) { return x; } function f(c) { if(c) return tag`x`; " + branch + " return tag`x`; return null; }\n")
				.encode()
			)
			width: str = "20"
			command: list[str] = [str(binary), "--stdin", "--lang", lang, "--chars", width, "--no-lint", "--fail-on-skip"]
			first: bytes = run_process(command, input=original, capture_output=True, check=True).stdout
			comparisons.append((len(sources), len(sources) + 1, lang, "template identity", width))
			sources.extend((original.decode(), first.decode()))
	# 制御本体の実行結果の照合
	script: str = r"""
const fs = require("node:fs"), vm = require("node:vm");
const sources = JSON.parse(fs.readFileSync(0, "utf8"));
process.stdout.write(JSON.stringify(sources.map(source =>
	vm.runInNewContext(source + "\nJSON.stringify([f(true), f(false), f(true) === f(false)])", {}, { timeout: 1000 })
)));
"""
	results: list[str] = (
		json.loads(run_process(["node", "-e", script], input=json.dumps(sources), text=True, capture_output=True, check=True).stdout)
	)
	require(len(results) == len(sources))
	for original, output, lang, clause, width in comparisons:
		require(results[original] == results[output], (lang, clause, width, "control body result changed"))
	print("control bodies: 6 clauses, 2 languages, narrow wrapping, native results and layout invariance OK")
	# 終了
	return

def verify_nested_decls(binary: Path) -> None:
	"""初期化式の中に在る宣言の分割を，検証を有効にした通常経路で確認する
	計算量：テストケース数 F・設定幅数 W・処理系の最大実行時間 T に対して O(FWT)
	引数：
		binary: フォーマッターの実行ファイル
	送出例外：
		subprocess.CalledProcessError: フォーマッター又は Node.js が失敗の終了コードを返した場合
		subprocess.TimeoutExpired: 子プロセスが所要時間の上限を超えた場合
		AssertionError: 見送が起きたか，冪等でないか，実行結果が期待と異なる場合
		OSError: フォーマッター又は Node.js を起動出来ない場合
	"""
	# 外側と内側の宣言が共に分割されるテストケースと，外側だけが単独宣言の対照テストケース
	src: str = """function Outer() {
const env = { painting: 1, stage: 2, viewer: { position: { clone: () => 3 }, quaternion: { clone: () => 4 } }, handlers: 5 };
const assembleOverviewImage = () => {
const { painting, stage, viewer, handlers } = env, storedOrigins = viewer.position.clone();
const storedRotations = viewer.quaternion.clone();
return [painting, stage, handlers, storedOrigins, storedRotations];
}, onSnapshotDone = assembleOverviewImage;
console.log(JSON.stringify(onSnapshotDone()));
return 0;
}
Outer();
"""
	control: str = src.replace("}, onSnapshotDone = assembleOverviewImage;", "};\nconst onSnapshotDone = assembleOverviewImage;")
	require(control != src, "the declaration control did not replace the outer declaration")
	# 期待する実行結果は入力の値と返戻順から定める（フォーマッターの出力を期待テキストにしない）
	expected: bytes = b"[1,2,5,3,4]\n"
	observed: dict[bytes, bytes] = {}
	for idx, original in enumerate((src.encode(), control.encode())):
		for lang in "javascript", "typescript", "typescriptreact":
			for width in "32", "0":
				command: list[str] = [str(binary), "--stdin", "--lang", lang, "--chars", width, "--no-lint", "--fail-on-skip"]
				first: subprocess.CompletedProcess[bytes] = run_process(command, input=original, capture_output=True, check=True)
				require(b"[skip]" not in first.stderr, (idx, lang, width, first.stderr))
				for code in original, first.stdout:
					if code not in observed:
						observed[code] = run_process(["node", "--input-type=module", "-"], input=code, capture_output=True, check=True).stdout
					require(observed[code] == expected, (idx, lang, width, "nested declaration result changed", observed[code]))
	print("nested declarations: 2 cases, 3 language routes, 2 widths, native values OK")
	# 終了
	return

def verify_input_review(binary: Path) -> None:
	"""入力・埋込・方言・通知が入口と静的解析の有無で変わらない事を検査する
	計算量：テストケース数 F・フォーマッターの最大実行時間 T・原文長 B に対して O(F(T+B))
	引数：
		binary: フォーマッターの実行ファイル
	送出例外：
		AssertionError: 出力・分類・通知又は原本保持が仕様と異なる場合
		OSError: 一時テストケースの作成又は実行に失敗した場合
		subprocess.TimeoutExpired: 実行が時間の上限を超えた場合
		subprocess.CalledProcessError: 呼び出した子プロセスが非零の終了値を返す場合
	"""
	# 期待する分類と本文は原文と整形仕様 2.1, 4.1, 4.4, 8.3, 10.4, 11.2 から定める
	cases: list[tuple[str, str, bytes, int, bytes]] = [
		("js", "partial", b"let  x = 1;\nlet y = ;\n", 3, b"formatting is partial"),
		("html", "partial.html", b"<script>let x = ;</script>\n<p>a</p>\n", 3, b"Embedded code"),
		("css", "line.css", b"a {\n\tcolor: red; // note\n\tbackground: blue;\n}\n", 4, b"parser could not read the source"),
		("html", "module.html", b"<script type=\"module\">let a = 1; a <!-- b</script>\n", 1, b""),
		("html", "classic.html", b"<script>let a = 1; a <!-- b</script>\n", 3, b"HTML-like comments are not supported"),
		(
			"html",
			"mime.html",
			b"<script type=\"text/javascript\">let a = 1; a <!-- b</script>\n",
			3,
			b"HTML-like comments are not supported"
		)
	]
	with tempfile.TemporaryDirectory(prefix="shaveformat-input-review-") as temp:
		dir: Path = Path(temp).resolve()
		for lang, name, src, classification, msg in cases:
			path: Path = dir / name
			path.write_bytes(src)
			for lint_args in [], ["--no-lint"]:
				stdin_result: subprocess.CompletedProcess[bytes] = (
					run_process([str(binary), "--stdin", "--lang", lang, *lint_args], input=src, env=CORE_ENV, capture_output=True)
				)
				require(not stdin_result.returncode, (name, stdin_result))
				if classification == 4:
					require(stdin_result.stdout == src and b"[skip]" in stdin_result.stderr, stdin_result)
					require(b"[layout]" not in stdin_result.stderr, stdin_result.stderr)
				else:
					require(
						re.findall(rb"^\[classification\] (\d+)$", stdin_result.stderr, re.M) == [str(classification).encode()],
						stdin_result.stderr
					)
				require(msg.lower() in stdin_result.stderr.lower(), (name, stdin_result.stderr))
				if name == "module.html":
					require(b"a < !--b;" in stdin_result.stdout and b"Embedded code was left as is" not in stdin_result.stderr, stdin_result)
				# 同じ入力の入口間の一致を比較する（出力から正解の本文を生成しない）
				stdin_warnings: list[bytes] = re.findall(rb"^\[warn\] .*?:(\d+:\d+: .*)$", stdin_result.stderr, re.M)
				for entry in [], ["-c"], ["--diff"]:
					result: subprocess.CompletedProcess[bytes] = (
						run_process([str(binary), "--lang", lang, *lint_args, *entry, str(path)], env=CORE_ENV, capture_output=True)
					)
					require(result.returncode in (0, 1) and msg.lower() in result.stderr.lower(), (name, entry, result))
					if classification == 4:
						require(b"[skip]" in result.stderr and b"[layout]" not in result.stderr, result)
					else:
						require(re.findall(rb"^\[classification\] (\d+)$", result.stderr, re.M) == [str(classification).encode()], result.stderr)
						require(re.findall(rb"^\[warn\] .*?:(\d+:\d+: .*)$", result.stderr, re.M) == stdin_warnings, (entry, result.stderr))
						if entry == ["-c"] and classification == 3:
							require(b"1 partial" in result.stdout, result.stdout)
					require(path.read_bytes() == src, "a read-only entry changed the input")
			# 書戻も同じ部分整形を返し，完全整形に分類し直さない
			if classification == 3:
				written: subprocess.CompletedProcess[bytes] = (
					run_process([str(binary), "--write", "--lang", lang, "--no-lint", str(path)], env=CORE_ENV, capture_output=True)
				)
				require(not written.returncode and b"[classification] 3" in written.stderr, written)
				require(path.read_bytes() == stdin_result.stdout, "write and stdin produced different partial text")
		# 明示した方言・読みの目標と既知の拡張子の矛盾は使用法の誤り（整形仕様 2.2）
		for lang, name, src in (
			("mjs", "conflict.cjs", b"a <!-- b\n"),
			("tsx", "conflict.ts", b"const x = <div>hi</div>;\n"),
			("json", "conflict.jsonc", b"{\n// c\n\"a\": 1,\n}\n"),
			("scss", "conflict.css", b"a { color: red; // note\n}\n")
		):
			path: Path = dir / name
			path.write_bytes(src)
			for entry in [], ["-c"], ["--diff"], ["-w"]:
				for lint_args in [], ["--no-lint"]:
					result: subprocess.CompletedProcess[bytes] = (
						run_process([str(binary), "--lang", lang, *entry, *lint_args, str(path)], capture_output=True)
					)
					require(result.returncode == 2 and not result.stdout and b"conflicts with the file extension" in result.stderr, result)
					require(path.read_bytes() == src, "a conflicting dialect changed the input")
		# コメントを許す設定名と，素の JSON への JSONC の明示は受理する
		for name, override in (
			(".babelrc", ""),
			(".eslintrc", ""),
			(".hintrc", ""),
			(".jsfmtrc", ""),
			(".jshintrc", ""),
			(".swcrc", ""),
			("data.geojson", "jsonc"),
			("data.json", "jsonc"),
			("data.jsonld", "jsonc"),
			("data.webmanifest", "jsonc")
		):
			path = dir / name
			src: bytes = b"{\"a\": 1}\n" if override else b"{\n// comment\n\"a\": 1\n}\n"
			path.write_bytes(src)
			for entry in [], ["-c"], ["--diff"], ["-w"]:
				path.write_bytes(src)
				args: list[str] = ["--lang", override] if override else []
				result = run_process([str(binary), *args, *entry, "--no-lint", str(path)], capture_output=True)
				require(result.returncode in (0, 1) and not result.stderr, result)
				require(b"1 skipped" not in result.stdout and b"1 failed" not in result.stdout, result)
				if entry != ["-w"]:
					require(path.read_bytes() == src, "JSON admission changed a read-only input")
		# 目標未指定の .js では明示した `Module` を使い，型無の埋込 `Script` と区別する
		path = dir / "explicit.js"
		path.write_bytes(b"a <!-- b\n")
		result = run_process([str(binary), "--lang", "mjs", "--diff", str(path)], env=CORE_ENV, capture_output=True)
		require(b"@@ -1,1 +1,1 @@\n-a <!-- b\n+a < !--b;\n" in result.stdout and b"[skip]" not in result.stderr, result)
		# import/export が無くても拡張子で確定した `Module` の最上位を不変化する（整形仕様 5.3）
		lexical_src: bytes = b"let answer = 1;\nuse(answer);\n"
		for goal, spelling in ("mjs", b"const answer = 1;"), ("cjs", b"let answer = 1;"), ("js", b"let answer = 1;"):
			goal_path: Path = dir / ("binding." + goal)
			goal_path.write_bytes(lexical_src)
			for lint_args in [], ["--no-lint"]:
				stdin_goal: subprocess.CompletedProcess[bytes] = (
					run_process([str(binary), "--stdin", "--lang", goal, *lint_args], input=lexical_src, capture_output=True)
				)
				require(not stdin_goal.returncode and stdin_goal.stdout.startswith(spelling + b"\n"), stdin_goal)
				file_goal: subprocess.CompletedProcess[bytes] = (
					run_process([str(binary), "--lang", goal, *lint_args, str(goal_path)], capture_output=True)
				)
				require(not file_goal.returncode and goal_path.read_bytes() == lexical_src, file_goal)
				# 書戻したコピーの本文は標準入力の結果と同じ（整形仕様 11.2 の入口間の同一結果）
				written_path: Path = dir / ("written." + goal)
				written_path.write_bytes(lexical_src)
				written_goal: subprocess.CompletedProcess[bytes] = (
					run_process([str(binary), "--lang", goal, "-w", *lint_args, str(written_path)], capture_output=True)
				)
				require(not written_goal.returncode and written_path.read_bytes() == stdin_goal.stdout, written_goal)
				written_path.unlink()
			goal_path.unlink()
		# 改行・BOM の通知は実際に書き換えた表現だけ（整形仕様 4.1, 10.4）
		normalizations: list[tuple[str, str, bytes, tuple[bytes, ...], tuple[bytes, ...]]] = [
			("json", "bom.json", b"\xef\xbb\xbf{ \"x\" : 1 }\r\n", (b"UTF-8 BOM", b"CRLF line endings"), ()),
			("json", "mixed.json", b"{\r\n\"x\": 1\n}\r\n", (b"mixes CRLF and LF",), ()),
			("c", "comment.c", b"int x; /* a\rb */\n", (), (b"CR-only",)),
			("js", "literal.js", b"const s = `a\rb`;\n", (), (b"CR-only",)),
			("js", "literal_crlf.js", b"const s = `a\r\nb`;\n", (), (b"line endings",)),
			("js", "lone.js", b"let x = 1;\rlet y = 2;\r", (b"CR-only",), ()),
			("js", "unicode.js", "let x = 1;\u2028let y = 2;\n".encode(), (b"Unicode line separators",), ()),
			("html", "embedded_crlf.html", b"<script>let x = 1;\r\n</script>\n", (b"mixes CRLF and LF",), ())
		]
		for lang, name, src, required, forbidden in normalizations:
			path = dir / name
			path.write_bytes(src)
			for lint_args in [], ["--no-lint"]:
				for entry in ["--stdin"], [], ["-c"], ["--diff"]:
					command: list[str] = [str(binary), "--lang", lang, *lint_args, *entry]
					if entry != ["--stdin"]:
						command.append(str(path))
					result = run_process(command, input=src if entry == ["--stdin"] else None, capture_output=True)
					require(result.returncode in (0, 1) and b"[skip]" not in result.stderr and b"Error:" not in result.stderr, result)
					for notice in required:
						require(result.stderr.count(notice) == 1, (name, notice, result.stderr))
					for notice in forbidden:
						require(notice not in result.stderr, (name, notice, result.stderr))
					if entry == ["--stdin"] and not required:
						require(b"\r" in result.stdout, ("a literal or comment lost CR", result))
				require(path.read_bytes() == src, "normalization notices changed a read-only input")
		# ファイルの入口でも宣言の値を指し，原文を保持する
		for name, src, position in (
			("encoding.rb", b"#!/usr/bin/env ruby\r\n# coding: cp932\r\nx=1\r\n", b"2:11"),
			("encoding.py", b"# coding: utf-7\nx=1\n", b"1:11")
		):
			path = dir / name
			path.write_bytes(src)
			for entry in [], ["-c"], ["--diff"], ["-w"]:
				result = run_process([str(binary), *entry, "--no-lint", str(path)], capture_output=True)
				label: bytes = str(path).encode() + b":" + position
				expected: bytes = (
					b"[skip] " + label + b": unsupported encoding\n[notice] " + label +
					b": correct or remove the encoding declaration before retrying\n"
				)
				require(not result.returncode and result.stderr == expected and path.read_bytes() == src, result)
	# 正規化の前に対象外のマーカーを読む場合も，改行記法に依る判定を変えない（整形仕様 2.2, 4.1）
	for lang, src, reason in (
		("h", b"int x;\r#import <Foundation/Foundation.h>\r", b"Objective-C header"),
		("ts", "\u2028<?xml version=\"1.0\"?>\n<root/>\n".encode(), b"XML document"),
		("ts", b"<?xml\xff", b"unsupported encoding")
	):
		result = run_process([str(binary), "--stdin", "--lang", lang], input=src, capture_output=True)
		require(not result.returncode and b"[skip]" in result.stderr and reason in result.stderr, result)
		require(result.stdout == src, "early rejection changed the input")
	# 宣言の値だけが対応外なら，原文の値の位置と宣言を直す対処を報告する
	for (
		lang,
		src,
		position
	) in ("ruby", b"#!/usr/bin/env ruby\r\n# coding: cp932\r\nx=1\r\n", b"2:11"), ("python", b"# coding: utf-7\nx=1\n", b"1:11"):
		for strict in [], ["--fail-on-skip"]:
			result = run_process([str(binary), "--stdin", "--lang", lang, "--no-lint", *strict], input=src, capture_output=True)
			label: bytes = b"<stdin>:" + position
			expected: bytes = (
				b"[skip] " + label + b": unsupported encoding\n[notice] " + label +
				b": correct or remove the encoding declaration before retrying\n"
			)
			require(result.returncode == bool(strict) and result.stdout == src and result.stderr == expected, result)
	for src in (
		b"const char *s = \"value\r@interface\";\n",
		b"const char *s = R\"tag(value\r@interface)tag\";\n",
		b"int x = 1'000;\r#import <Foundation/Foundation.h>\r"
	):
		result = run_process([str(binary), "--stdin", "--lang", "h"], input=src, capture_output=True)
		if src.startswith(b"int x"):
			require(b"Objective-C header" in result.stderr and result.stdout == src, result)
		else:
			require(b"Objective-C header" not in result.stderr, result.stderr)
	# 空行とコメントだけの行は深さ超過の連続範囲を切らず，最初のコードトークンだけを報告する（整形仕様 8.4）
	# 実装を持つメソッドで全ての型の本体を展開させ，生成するインデントの深さを入れ子の数と揃える
	for count, expected_count in (32, 0), (33, 1):
		src_text: str = "".join("\t" * depth + f"class A{depth} {{\n" for depth in range(count))
		src_text += "\t" * count + "int x = 1;\n\n"
		src_text += "\t" * count + "/** 写し */\n" + "\t" * count + "void y() {}\n"
		src_text += "".join("\t" * depth + "}\n" for depth in reversed(range(count)))
		result = run_process([str(binary), "--stdin", "--lang", "java"], input=src_text.encode(), capture_output=True)
		require(not result.returncode and b"[skip]" not in result.stderr, result)
		depth_warnings: list[tuple[bytes, bytes]] = (
			re.findall(rb"^\[warn\] <stdin>:(\d+):(\d+): Violation: Nesting depth", result.stderr, re.M)
		)
		require(len(depth_warnings) == expected_count, result.stderr)
		if depth_warnings:
			row: int = int(depth_warnings[0][0]) - 1
			col: int = int(depth_warnings[0][1]) - 1
			require(result.stdout.split(b"\n")[row][col:].startswith(b"int x"), (row, col, result.stdout))
	# 名前の無い仮引数をシグネチャ順で報告し，void と `...` を実引数と誤認しない（整形仕様 8.4）
	for lang in "c", "cpp":
		param_src: bytes = (
			b"/** result */\nint Foo(int, /* marker */ int Second) { return Second; }\n" +
			b"/** end */\nvoid End(void) {}\n/** variadic */\nvoid Variadic(int First, ...) {}\n"
		)
		result = run_process([str(binary), "--stdin", "--lang", lang], input=param_src, capture_output=True)
		require(not result.returncode and b"[skip]" not in result.stderr, result)
		require(result.stderr.count(b"missing a description for parameter #1") == 1, result.stderr)
		require(result.stderr.count(b"missing a description for parameter 'Second'") == 1, result.stderr)
		require(b"parameter #2" not in result.stderr and b"parameter #3" not in result.stderr, result.stderr)
	# Main は並列の整形結果を，パスの辞書順で定めた収集順で表示する（明示の順 z, a に依らない，整形仕様 9.4，10.5）
	with tempfile.TemporaryDirectory(prefix="shaveformat-observation-review-") as temp:
		dir = Path(temp).resolve()
		first: Path = dir / "z.c"
		second: Path = dir / "a.c"
		first.write_bytes(b"int z;\n")
		second.write_bytes(b"int a;\n")
		result = run_process([str(binary), "--no-lint", str(first), str(second)], env=CORE_ENV, capture_output=True)
		require(not result.returncode, result)
		lines: list[bytes] = [bytes.fromhex(line.decode()) for line in re.findall(rb"^\[layout\] (.*)$", result.stderr, re.M)]
		require(lines == [b"int a;", b"int z;"], result.stderr)
	# 終了
	return

def verify_literal_admission(binary: Path) -> None:
	"""閉じた補間無の本文の受理と，補間有・未閉じの本文の内部計数を検査する
	計算量：生成する本文の総長 B とフォーマッターの実行時間 T に対して O(B+T)
	引数：
		binary: フォーマッターの実行ファイル
	送出例外：
		AssertionError: 受理・失敗の分類又は本文の保持が仕様と異なる場合
		OSError: フォーマッターを起動出来ない場合
		subprocess.TimeoutExpired: 整形が時間上限を超えた場合
	"""
	# 各経路の旧内部計数で初めて 512 MiB を超える本文長（整形仕様 9.1）
	# 言語はプロジェクトの順，同じ言語の中は開きの文字コード順
	for language, opening, closing, size in (
		("go", b"package p\nvar x = `", b"`\n", 401300),
		("php", b"<?php\n$x = '\n", b"';\n", 401299),
		("php", b"<?php\n$x = <<<'TEXT'\n", b"\nTEXT;\n", 401295),
		("ruby", b"x = %q(", b")\n", 401301),
		("ruby", b"x = '\n", b"'\n", 401303),
		("ruby", b"x = <<'TEXT'\n", b"\nTEXT\n", 401301),
		("html", b"<div title=\"\n", b"\">x</div>\n", 401295)
	):
		body: bytes = b"-" * size
		src: bytes = opening + body + closing
		result: subprocess.CompletedProcess[bytes] = (
			run_process([str(binary), "--stdin", "--lang", language, "--no-lint"], input=src, capture_output=True)
		)
		# 閉じた補間無の本文を C の１領域として受理し，本文の値は保持する
		require(not result.returncode and not result.stderr, (language, size, result.returncode, result.stderr[:400]))
		require(body in result.stdout, (language, "literal payload changed"))
	# 補間有・未閉じの内部を C へ畳まず，T の見積超過で原文を保持する
	for (
		language,
		opening,
		closing,
		size
	) in ("go", b"package p\nvar x = `", b"", 401301), ("javascript", b"const x = `${value}", b"`;\n", 401297):
		src = opening + b"-" * size + closing
		result = run_process([str(binary), "--stdin", "--lang", language, "--no-lint"], input=src, capture_output=True)
		require(
			result.returncode == 1 and result.stdout == src and b"estimated working memory exceeds resource limit" in result.stderr,
			(language, size, result.returncode, result.stderr[:400])
		)
	# 終了
	return

def verify_path_encoding(binary: Path) -> None:
	"""Windows の処理系の名前で対象を判定し，必要な変換の失敗だけを一貫して報告する事を確認する
	計算量：固定件数のパス長 P と外部処理の時間 T に対し O(P + T)
	引数：
		binary: 検査するフォーマッター
	送出例外：
		AssertionError: 除外・通知・終了状態又は原文の保持が期待と異なる場合
		OSError: 検査する名前の作成又は実行ファイルの起動に失敗する場合
		subprocess.TimeoutExpired: 外部処理が時間上限を超えた場合
	"""
	# POSIX はワイド文字列からの変換を持たない為，此の検査は Windows だけで行う
	if os.name != "nt":
		# ワイド文字列の入口を持たない環境の返戻
		return
	with tempfile.TemporaryDirectory(prefix="shaveformat-path-encoding-") as directory:
		root: Path = Path(directory).resolve()
		good: Path = root / "good.cpp"
		good.write_bytes(b"int X;\n")
		(root / "bad\ud800.png").write_bytes(b"opaque")
		(root / "bad\ud800.min.js").write_bytes(b"let  x=1;\n")
		hidden: Path = root / ".\ud800"
		hidden.mkdir()
		(hidden / "hidden.cpp").write_bytes(b"int  X;\n")
		for mode in "--check", "--diff":
			result: subprocess.CompletedProcess[bytes] = run_process([str(binary), mode, "--no-lint", str(root)], capture_output=True)
			require(not result.returncode and not result.stderr and result.stdout.endswith(b", 2 path(s) excluded by name.\n"), result)
		invalid: Path = root / "bad\ud800.cpp"
		invalid.write_bytes(b"int  X;\n")
		label: bytes = str(invalid).replace("\ud800", "\\uD800").encode()
		expected: bytes = (
			b"Error: cannot read filesystem path: " + label + b"\n[notice] " + label +
			b": verify the path encoding and access permissions\n"
		)
		# 起動引数の変換の失敗は，従来の入口の通知と原本の保持にする
		result = run_process([str(binary), "--check", "--no-lint", str(invalid)], capture_output=True)
		require(result.returncode == 1 and result.stderr == expected and invalid.read_bytes() == b"int  X;\n", result)
		# 走査中の変換の失敗だけを読込失敗にし，兄弟の書戻を止めない
		for mode in "--check", "--write":
			good.write_bytes(b"int  X;\n")
			result = run_process([str(binary), mode, "--no-lint", str(root)], capture_output=True)
			lines: list[bytes] = result.stderr.splitlines()
			require(result.returncode == 1 and len(lines) == 2 and lines[0].startswith(b"Error reading: " + label + b": "), result)
			require(lines[1] == b"[notice] " + label + b": verify read access and retry after concurrent changes stop", result)
			summary: bytes = (
				b"Formatting check: 0 unchanged, 1 would change, 0 partial, 0 skipped (not checked), 0 failed, 0 warning(s), 2 path(s) excluded by name.\n" if
				mode == "--check" else
				b"Done. 1/1 file(s) formatted, 1 path(s) could not be read, 2 path(s) excluded by name.\n"
			)
			require(result.stdout.endswith(summary) and invalid.read_bytes() == b"int  X;\n", result)
			require(good.read_bytes() == (b"int  X;\n" if mode == "--check" else b"int X;\n"), result)
		invalid.unlink()
		# ワイド文字の枝名を相対パスから表示し，ファイル名の変換例外も要約を失わせない
		branch: Path = root / "branch\ud800"
		branch.mkdir()
		inside: Path = branch / "inside.cpp"
		inside.write_bytes(b"int  Y;\n")
		good.write_bytes(b"int  X;\n")
		result = run_process([str(binary), "--write", "--no-lint", "."], cwd=root, capture_output=True)
		label = ("." + os.sep + branch.name + os.sep + "inside.cpp").replace("\ud800", "\\uD800").encode()
		lines = result.stderr.splitlines()
		require(result.returncode == 1 and len(lines) == 2 and lines[0].startswith(b"Error reading: " + label + b": "), result)
		require(lines[1] == b"[notice] " + label + b": verify read access and retry after concurrent changes stop", result)
		require(
			result.stdout.endswith(b"Done. 1/1 file(s) formatted, 1 path(s) could not be read, 2 path(s) excluded by name.\n"),
			result
		)
		require(good.read_bytes() == b"int X;\n" and inside.read_bytes() == b"int  Y;\n", result)
		require(not list(root.glob(".shavefmt-*")), result)
	# 終了
	return

def verify_security_pm_review(binary: Path) -> None:
	"""作業域・差分・別名・診断・並列の回帰を仕様から検査する
	計算量：テストケース総長 B と実行時間 T に対して O(B+T)
	引数：
		binary: フォーマッターの実行ファイル
	送出例外：
		AssertionError: 分類・通知・内容が仕様と異なる場合
		OSError: 一時テストケースの作成又は実行に失敗した場合
		subprocess.TimeoutExpired: 実行が時間上限を超えた場合
		subprocess.CalledProcessError: 呼び出した子プロセスが非零の終了値を返す場合
	"""
	# 閉じた引用・専用区切を受理し，未確定の内部計数を保つ
	verify_literal_admission(binary)
	# メモリ不足では失敗して原文を返す（整形仕様 9.1，入力は 16 MiB 未満）
	dense: bytes = b"[" + b"1," * 300000 + b"1]\n"
	for lint_args in [], ["--no-lint"]:
		result: subprocess.CompletedProcess[bytes] = (
			run_process([str(binary), "--stdin", "--lang", "json", *lint_args], input=dense, capture_output=True)
		)
		require(result.returncode == 1 and result.stdout == dense and b"estimated working memory" in result.stderr, result.stderr)
	# 入力の取得中のサイズ超過も，受理の見送と同じ２行を出し原文を返す
	oversized_input: bytes = b" " * 0X1000001
	for entry_args, expected_exit in ([], 0), (["--fail-on-skip"], 1):
		result = run_process([str(binary), "--stdin", "--lang", "json", *entry_args], input=oversized_input, capture_output=True)
		require(result.returncode == expected_exit and result.stdout == oversized_input, result.stderr)
		require(
			result.stderr ==
			b"[skip] <stdin>: input exceeds size limit\n" + b"[notice] <stdin>: provide independently valid input within 16 MiB\n",
			result.stderr
		)
	del oversized_input
	# 静的解析の同一行の参照記号と，ドキュメント同期の多数の項目を線形に処理する
	for lang, src, lint_args in (
		("c", "// 規約 " + "§" * 80000 + "\nint x;\n", []),
		(
			"c",
			"/**\n * Summary\n" + "".join(" * @param a%d text\n" % idx for idx in range(8000)) + " */\nvoid f(" +
			", ".join("int a%d" % idx for idx in range(8000)) + ") {\n}\n",
			["--no-lint"]
		)
	):
		result = run_process(
			[str(binary), "--stdin", "--lang", lang, "--chars", "0", *lint_args],
			input=src.encode(),
			capture_output=True,
			timeout=10
		)
		require(not result.returncode and b"[skip]" not in result.stderr, result.stderr[:400])
	# パーサーの制限で出る ERROR を入力エラーと断定せず，未閉じの文字列は入力エラーと表示する
	for lang, src, msg in (
		("cpp", b"#if __has_include(<vector>)\nint a;\n#endif\n", b"parser recognition failed"),
		("js", b"const value = \"unterminated\n", b"input syntax error"),
		("json", b"1\n2\n", b"input syntax error"),
		("go", b"package p\n\vvar x int\n", b"input syntax error")
	):
		result = run_process([str(binary), "--stdin", "--lang", lang, "--no-lint"], input=src, capture_output=True)
		require(not result.returncode and msg in result.stderr.lower(), result.stderr)
		reason_lines: list[bytes] = [line for line in result.stderr.splitlines() if line.startswith((b"[skip]", b"[warn]"))]
		parse_notices: list[bytes] = [
			line for line in result.stderr.splitlines() if line.startswith(b"[notice]") and msg in line.lower()
		]
		require(len(parse_notices) == 1, "parse cause or remedy was duplicated")
		require(
			all(b"; correct" not in line and b"; verify" not in line and msg not in line.lower() for line in reason_lines),
			result.stderr
		)
		if b"[skip]" in result.stderr:
			require(any(line.endswith(b"parser could not read the source") for line in reason_lines), result.stderr)
		else:
			require(
				any(line.endswith(b"Parser could not read part of the source; formatting is partial") for line in reason_lines),
				result.stderr
			)
		require(re.search(rb"<stdin>:[0-9]+:[0-9]+:", result.stderr), result.stderr)
		if lang == "json" or lang == "go":
			require(b"<stdin>:2:1:" in result.stderr, "input syntax error lost its actual position")
	# 見送は原文の UTF-8 バイト列の位置を示し，除いた BOM の３バイトも列へ含める
	located_src: bytes = b"\xef\xbb\xbf1 2\r\n"
	located_result: subprocess.CompletedProcess[bytes] = (
		run_process([str(binary), "--stdin", "--lang", "json", "--no-lint"], input=located_src, capture_output=True)
	)
	require(located_result.stdout == located_src and b"<stdin>:1:6:" in located_result.stderr, located_result)
	# 固定本文と対処は別行とし，パスの無い通知にはパスフィールドを作らない（整形仕様 10.1, 10.4）
	result = run_process([str(binary), "--stdin", "--lang", "not-a-language"], input=b"", capture_output=True)
	require(result.returncode == 2, result)
	require(
		result.stderr == b"Error: --lang unsupported language ID 'not-a-language'\n" + b"[notice] run --help for the list\n",
		result.stderr
	)
	with tempfile.TemporaryDirectory(prefix="shaveformat-security-pm-") as temp:
		dir: Path = Path(temp).resolve()
		unsupported: Path = dir / "plain.unknown"
		unsupported.write_bytes(b"unchanged\n")
		result = run_process([str(binary), str(unsupported)], capture_output=True)
		expected_label: bytes = str(unsupported).encode()
		require(
			result.returncode == 1 and result.stderr ==
			b"Error: unsupported file type: " + expected_label + b"\n[notice] " + expected_label + b": use --lang ID to format it anyway\n",
			result.stderr
		)
		unsupported.unlink()
		# UTF-8 の名前と実体の同一性を，通常の書戻経路で確認する
		case_root: Path = dir / "CaseRoot"
		case_root.mkdir()
		unicode_path: Path = case_root / "値😀.c"
		unicode_path.write_bytes(b"int value=1;\n")
		selected_root: Path = dir / "caseroot"
		if not selected_root.exists():
			selected_root = case_root
		result = run_process([str(binary), "--write", "--no-lint", str(selected_root)], capture_output=True)
		require(not result.returncode and b"[skip]" not in result.stderr, result)
		require(unicode_path.read_bytes() == b"int value = 1;\n", "Unicode path or directory identity changed the result")
		unicode_path.unlink()
		case_root.rmdir()
		# 正規化形を同じ実体へ解決する環境では，テキストの違いでリンクと誤認しない
		normalized_root: Path = dir / "de\u0301"
		normalized_root.mkdir()
		normalized_file: Path = normalized_root / "value.c"
		normalized_file.write_bytes(b"int value=1;\n")
		selected_root = dir / "d\u00e9"
		if not selected_root.exists():
			selected_root = normalized_root
		result = run_process([str(binary), "--write", "--no-lint", str(selected_root)], capture_output=True)
		require(not result.returncode and b"[skip]" not in result.stderr, result)
		require(normalized_file.read_bytes() == b"int value = 1;\n", "normalization form changed directory admission")
		normalized_file.unlink()
		normalized_root.rmdir()
		# 診断設定はテキスト・分類・固定警告を変えない（整形仕様 10.5, 11.1）
		# 小さな名前だけでなく，観測のコピーが予算を使い切った修飾名も通す
		diagnostic_inputs: list[tuple[str, bytes]] = [
			("c", b"int value=1; /* comment */\n"),
			("cpp", b"namespace A {namespace Next=::A;void G(){}}\nvoid F(){A::" + b"Next::" * 512 + b"G();}\n")
		]
		diagnostic_environment: dict[str, str] = dict(os.environ)
		for var in "SHAVEFMT_WARNINGS", "SHAVEFMT_STRUCT_DIAG", "SHAVEFMT_VERIFY_COMMENT_SEPARATION":
			diagnostic_environment.pop(var, None)
		for diagnostic_lang, diagnostic_src in diagnostic_inputs:
			args: list[str] = [str(binary), "--stdin", "--lang", diagnostic_lang, "--chars", "0"]
			ordinary: subprocess.CompletedProcess[bytes] = (
				run_process(args, input=diagnostic_src, env=diagnostic_environment, capture_output=True)
			)
			require(not ordinary.returncode and b"[skip]" not in ordinary.stderr, ordinary.stderr)
			for var in "SHAVEFMT_WARNINGS", "SHAVEFMT_STRUCT_DIAG", "SHAVEFMT_VERIFY_COMMENT_SEPARATION":
				result = run_process(args, input=diagnostic_src, env=dict(diagnostic_environment, **{ var: "1" }), capture_output=True)
				require(
					result.returncode == ordinary.returncode and result.stdout == ordinary.stdout,
					"diagnostic settings changed formatting or classification"
				)
				require(
					[line for line in result.stderr.splitlines() if line.startswith((b"[warn]", b"[skip]", b"Error:"))] ==
					[line for line in ordinary.stderr.splitlines() if line.startswith((b"[warn]", b"[skip]", b"Error:"))],
					result.stderr
				)
		# 所有要素を移す整形でも，部分整形領域の警告と診断は同じ最終位置を使う
		for settings in {}, { "SHAVEFMT_WARNINGS": "1" }:
			src: bytes = b"<html><script>let value = ;\nlet other = 1;</script></html>\n"
			environment: dict[str, str] = dict(os.environ, **settings)
			result = run_process([str(binary), "--stdin", "--lang", "html", "--no-lint"], input=src, env=environment, capture_output=True)
			require(not result.returncode and b"Error:" not in result.stderr, result)
			warnings: list[bytes] = re.findall(
				rb"^\[warn\] (<stdin>:\d+:\d+): Embedded code: " + rb"Parser could not read part of the source; formatting is partial$",
				result.stderr,
				re.M
			)
			notices: list[bytes] = re.findall(rb"^\[notice\] (<stdin>:\d+:\d+):", result.stderr, re.M)
			owner: int = result.stdout.index(b"<script>")
			row: int = result.stdout[:owner].count(b"\n") + 1
			col: int = owner - result.stdout.rfind(b"\n", 0, owner)
			expected_point: bytes = b"<stdin>:%d:%d" % (row, col)
			require(warnings == notices == [expected_point], (warnings, notices, result.stderr))
		# 同じ密な入力はファイル・検査・差分でも失敗し，書戻をしない
		memory_path: Path = dir / "dense.json"
		for lint_args in [], ["--no-lint"]:
			for entry_args in ["--write"], ["--check"], ["--diff"]:
				memory_path.write_bytes(dense)
				result = run_process([str(binary), *entry_args, *lint_args, str(memory_path)], capture_output=True)
				require(result.returncode == 1 and b"estimated working memory" in result.stderr, result)
				require(memory_path.read_bytes() == dense, "memory rejection changed a file")
		memory_path.unlink()
		# 単独の見積超過は待機列へ入れず，同じ実行の通常入力を処理する（整形仕様 9.4）
		resource_batch: Path = dir / "resource-batch"
		resource_batch.mkdir()
		for idx in range(12):
			(resource_batch / ("small%d.json" % idx)).write_bytes(b"{}\n")
		(resource_batch / "dense.json").write_bytes(dense)
		result = run_process([str(binary), "--check", "--no-lint", str(resource_batch)], capture_output=True, timeout=10)
		require(result.returncode == 1 and b"estimated working memory" in result.stderr, result)
		require(
			b"12 unchanged, 0 would change, 0 partial, 0 skipped (not checked), 1 failed, 0 warning(s)" in result.stdout,
			"single-input rejection blocked or changed the parallel batch"
		)
		for item in resource_batch.iterdir():
			require(item.read_bytes() == (dense if item.name == "dense.json" else b"{}\n"), "check mode changed an admitted input")
			item.unlink()
		resource_batch.rmdir()
		# 出力・空行は仕様 6.5 の最上位宣言と式文の区分から手で定める
		path: Path = dir / "insert.cjs"
		path.write_bytes(b"const x = 1;\nconsole.log(x);\n")
		result = run_process([str(binary), "--diff", "--no-lint", str(path)], capture_output=True)
		require(not result.returncode and b"@@ -1,2 +1,3 @@\n const x = 1;\n+\n console.log(x);\n" in result.stdout, result)
		# BOM・改行の通知は静的解析と件数を分ける（整形仕様 8.4）
		path.write_bytes(b"\xef\xbb\xbf{\"x\": 1}\r\n")
		result = run_process([str(binary), "--lang", "json", "--stdin", "--no-lint"], input=path.read_bytes(), capture_output=True)
		require(result.stderr.count(b"[notice]") == 2 and b"[warn]" not in result.stderr, result.stderr)
		path.unlink()
		# 置場を失う空の case 本体の文中コメントは仕様 7.7 の脱落とし，診断を入力名に結び付ける
		drop_src: bytes = (ROOT / "test_data/golden/go/empty_statement_case.in").read_bytes()
		drop_expected: bytes = (ROOT / "test_data/golden/go/empty_statement_case.out").read_bytes()
		path = dir / "case.go"
		path.write_bytes(drop_src)
		diagnostic_environment: dict[str, str] = dict(os.environ, SHAVEFMT_WARNINGS="1")
		result = run_process([str(binary), "--write", "--no-lint", str(path)], env=diagnostic_environment, capture_output=True)
		require(not result.returncode and path.read_bytes() == drop_expected, result)
		require(b"[notice] " + str(path).encode() + b": comment dropped (" in result.stderr, result.stderr)
		stdin_result = run_process(
			[str(binary), "--stdin", "--lang", "go", "--no-lint"],
			input=drop_src,
			env=diagnostic_environment,
			capture_output=True
		)
		require(not stdin_result.returncode and stdin_result.stdout == drop_expected, stdin_result)
		require(b"[notice] <stdin>: comment dropped (" in stdin_result.stderr, stdin_result.stderr)
		path.unlink()
		# 同じ実体を２名で名指した場合とディレクトリの並列処理を同じ内容にする（整形仕様 9.4）
		shared: Path = dir / "a.c"
		alias: Path = dir / "b.c"
		shared.write_bytes(b"int  value ;\n" * 20000)
		os.link(shared, alias)
		result = run_process([str(binary), "--write", "--no-lint", str(dir)], capture_output=True)
		require(not result.returncode and b"Error writing" not in result.stderr, result)
		require(shared.read_bytes() == alias.read_bytes() == b"int value;\n" * 20000, "hard-link aliases had different results")
		require(
			b"hard link was broken" not in result.stderr and b"warning(s)" not in result.stdout,
			"fully rewritten aliases reported old contents or warnings"
		)
		shared.unlink()
		alias.unlink()
		# 別々の実体を並列で処理し，本文・警告の位置順を単独入口と比較する
		src: bytes = b"int  value ;\n"
		stdin_result: subprocess.CompletedProcess[bytes] = (
			run_process([str(binary), "--stdin", "--lang", "c"], input=src, capture_output=True)
		)
		require(not stdin_result.returncode and stdin_result.stdout == b"int value;\n", stdin_result)
		expected_warnings: list[bytes] = []
		for idx in range(30):
			path = dir / ("file%02d.c" % idx)
			path.write_bytes(src)
			expected_warnings.extend(
				line.replace(b"<stdin>", str(path).encode()) for line in stdin_result.stderr.splitlines() if line.startswith(b"[warn]")
			)
		result = run_process([str(binary), "--write", str(dir)], capture_output=True)
		require(not result.returncode, result)
		require([line for line in result.stderr.splitlines() if line.startswith(b"[warn]")] == expected_warnings, result.stderr)
		for path in dir.iterdir():
			require(path.read_bytes() == b"int value;\n", path)
			path.unlink()
		# 対象外のテンプレートを HTML や Python として走査しない（整形仕様 2.2）
		for suffix in ".ejs", ".jsp", ".asp", ".aspx", ".rhtml", ".volt", ".jshtm", ".rpy":
			path = dir / ("template" + suffix)
			path.write_bytes(b"label start:\n    \"hello\"\n")
			result = run_process([str(binary), "--check", str(path)], capture_output=True)
			require(result.returncode == 1 and b"unsupported file type" in result.stderr, result)
			path.unlink()
		# 上位のリンクを名指した場合も原因の成分と利用出来る実体のパスを通知する
		if os.name == "posix":
			path = dir / "target.c"
			path.write_bytes(b"int  value ;\n")
			link: Path = dir / "link"
			link.symlink_to(dir, target_is_directory=True)
			result = run_process([str(binary), "--write", str(link / "target.c")], capture_output=True)
			require(not result.returncode and b"link component" in result.stderr and b"use resolved path" in result.stderr, result)
			require(str(path).encode() in result.stderr and path.read_bytes() == b"int  value ;\n", result.stderr)
			link.unlink()
			path.unlink()
		if os.name == "nt":
			# junction も名指したパスのリンクとして通知し，実体を書き換えない
			path = dir / "target.c"
			path.write_bytes(b"int  value ;\n")
			junction: Path = dir / "junction"
			created: subprocess.CompletedProcess[bytes] = (
				run_process(["cmd", "/c", "mklink", "/J", str(junction), str(dir)], capture_output=True)
			)
			require(not created.returncode, created)
			try:
				result = run_process([str(binary), "--write", str(junction / "target.c")], capture_output=True)
				require(not result.returncode and b"link component" in result.stderr, result)
				require(path.read_bytes() == b"int  value ;\n", "junction target was written")
				# 元のパスの再解析点は `..` で隠れても拒否し，通常の書戻だけを１件にする
				hidden: str = str(junction / ".." / dir.name / "target.c")
				result = run_process([str(binary), "--write", "--fail-on-skip", str(path), hidden], capture_output=True)
				require(result.returncode == 1 and b"1 symbolic link(s) not followed" in result.stdout, result)
				require(result.stdout.startswith(f"Formatted: {path}\nDone. 1/1 file(s) formatted".encode()), result)
				require(path.read_bytes() == b"int value;\n" and b"link component" in result.stderr, result)
			finally:
				junction.rmdir()
			path.unlink()
		# ディレクトリの言語指定は無視する事を通知する
		path = dir / "kept.c"
		path.write_bytes(b"int value;\n")
		result = run_process([str(binary), "--check", "--lang", "json", str(dir)], capture_output=True)
		require(not result.returncode and b"--lang is ignored for directories" in result.stderr, result)
		path.unlink()
	# '=' の指定に実行可能な空白区切の対処を付ける
	result = run_process([str(binary), "--chars=100"], capture_output=True)
	require(result.returncode == 2 and b"--chars 100" in result.stderr, result)
	# 終了
	return

def verify_error_zone_loss(binary: Path) -> None:
	"""行幅を超えるエラー領域の保持と独立した失敗の優先順位を照合する
	計算量：テストケース数 F と外部処理の最大実行時間 T に対し O(FT)
	引数：
		binary: 検査するフォーマッター
	送出例外：
		AssertionError: 分類・理由又は原文の返戻が整形仕様の保持契約と異なる場合
		OSError: 参照するテストケースの読取又は子プロセスの起動が失敗する場合
		subprocess.TimeoutExpired: 整形が時間上限を超えた場合
	"""
	# 行幅を超えるエラー領域も継続の括弧で包まずテキストの儘保ち，外側を部分整形する（整形仕様 4.6, 8.2）
	chain: bytes = ("x = 1 if c else " * 8 + "0\n").encode()
	for src in chain, b"normal(1)\n" + chain + b"normal(2)\n", b"normal(1)\n# before\n" + chain + b"# after\nnormal(2)\n":
		result: subprocess.CompletedProcess[bytes] = (
			run_process([str(binary), "--stdin", "--lang", "python", "--no-lint"], input=src, capture_output=True)
		)
		require(not result.returncode and chain in result.stdout, result)
		require(b"formatting is partial" in result.stderr and b"[skip]" not in result.stderr, result)
	# ディレクティブの独立した保持違反は，エラー領域の喪失と同時でも失敗を優先
	directive_src: bytes = (ROOT / "test_data/golden/python/encoding_declaration.in").read_bytes() + chain
	directive: subprocess.CompletedProcess[bytes] = (
		run_process([str(binary), "--stdin", "--lang", "python", "--no-lint"], input=directive_src, capture_output=True)
	)
	require(directive.returncode == 1 and directive.stdout == directive_src, directive)
	require(b"formatting would change a directive comment" in directive.stderr and b"[skip]" not in directive.stderr, directive)
	# 同じ保護経路を通る既存の部分整形の行を伸ばしても，正常領域の構造を保つ
	for lang, src in (
		(
			"c",
			(ROOT / "test_data/golden/c/initializer_directive.in").read_bytes().replace(b"fa", b"function_" + b"a" * 150).replace(
				b"fb",
				b"function_" + b"b" * 150
			)
		),
		(
			"go",
			(ROOT / "test_data/golden/go/new_composite_partial.in").read_bytes().replace(
				b"1, 2",
				", ".join(str(idx) for idx in range(40)).encode()
			)
		)
	):
		result = run_process([str(binary), "--stdin", "--lang", lang, "--no-lint"], input=src, capture_output=True)
		require(not result.returncode and b"formatting is partial" in result.stderr and b"[skip]" not in result.stderr, result)
	# 終了
	return

def verify_empty_inputs(binary: Path) -> None:
	"""コードもコメントも無い入力を空の儘返す事を検査する（整形仕様 6.7）
	計算量：言語数 L・入力の種類数 K・フォーマッターの最大実行時間 T に対して O(LKT)
	引数：
		binary: フォーマッターの実行ファイル
	送出例外：
		AssertionError: 終了状態又は出力が空の結果の契約と異なる場合
		OSError: 子プロセスの起動に失敗した場合
		subprocess.TimeoutExpired: 実行が時間の上限を超えた場合
	"""
	# PHP はタグの外の空白を出力する本文として保持する為，空のバイト列だけを照合する
	# 改ページは HTML, CSS, SCSS の空白で，Go, JSON 等では不正な字である為，其の３言語だけで照合する
	for lang in "c", "php", "css":
		srcs: tuple[bytes, ...] = b"", b"\n", b" \t\r\n", b"\xEF\xBB\xBF", b"\xEF\xBB\xBF\r\n"
		if lang == "php":
			srcs = b"",
		elif lang in ("html", "css", "scss"):
			srcs += (b"\f",)
		for src in srcs:
			for lint_args in [], ["--no-lint"]:
				result: subprocess.CompletedProcess[bytes] = (
					run_process([str(binary), "--stdin", "--lang", lang, *lint_args], input=src, capture_output=True)
				)
				require(not result.returncode and not result.stdout, (lang, src, result))
	# 終了
	return

def verify_duplicate_paths(binary: Path, parent: Path) -> None:
	"""元のパスの表示・言語と，正規名毎の処理・通知・集計を検査する（整形仕様 9.4, 10.2, 11.1）
	計算量：テストケース数 F・子プロセスの最大時間 T・パス総長 P に対して O(FT+P)
	引数：
		binary: フォーマッターの実行ファイル
		parent: 既存の検査が所有する一時ディレクトリ
	送出例外：
		AssertionError: 元のパス・正規名・言語・件数が仕様と異なる場合
		OSError: テストケースの作成又は子プロセスの起動に失敗した場合
		subprocess.TimeoutExpired: 子プロセスが時間上限を超えた場合
	"""
	# 同じ検査の一時領域内に，走査と明示指定が重なる原本を用意
	with tempfile.TemporaryDirectory(prefix="duplicate-paths-", dir=parent) as temp:
		dir: Path = Path(temp).resolve()
		files: Path = dir / "files"
		files.mkdir()
		path: Path = files / "a.c"
		src: bytes = b"\xef\xbb\xbfint  value;\r\n"
		# 表示は引数順の最初の元のパス，書戻・各改行通知・集計は１件
		for args, label in (
			(["files/a.c", "files/a.c"], "files/a.c"),
			(["./files/a.c", "files/../files/a.c", "files/a.c"], "./files/a.c"),
			(["files/a.c", str(path)], "files/a.c"),
			([str(path), "files/a.c"], str(path)),
			(["files", "files/a.c"], os.path.join("files", "a.c")),
			(["./files", "files/a.c"], os.path.join("./files", "a.c"))
		):
			path.write_bytes(src)
			result: subprocess.CompletedProcess[bytes] = (
				run_process([str(binary), "--write", "--no-lint", *args], cwd=dir, capture_output=True)
			)
			require(not result.returncode and path.read_bytes() == b"int value;\n", (args, result))
			require(result.stdout == f"Formatted: {label}\nDone. 1/1 file(s) formatted.\n".encode(), (args, result))
			require(result.stderr.count(b"Input has a UTF-8 BOM") == 1, result)
			require(result.stderr.count(b"Input uses CRLF line endings") == 1, result)
			require(all(line.startswith(f"[notice] {label}: ".encode()) for line in result.stderr.splitlines()), result)
		# 表示の綴りでなく正規化済の絶対パスの順で報告
		last: Path = files / "z.c"
		for item in path, last:
			item.write_bytes(b"int  value;\n")
		result = run_process([str(binary), "--write", "--no-lint", str(last), "files/a.c"], cwd=dir, capture_output=True)
		require(not result.returncode and not result.stderr, result)
		require(result.stdout == f"Formatted: files/a.c\nFormatted: {last}\nDone. 2/2 file(s) formatted.\n".encode(), result)
		require(path.read_bytes() == last.read_bytes() == b"int value;\n", result)
		last.unlink()
		# 同じ実体の別名は２つのパスとして書き戻し，別名の分離の通知を重複へ取り違えない
		path.write_bytes(b"int  value;\n")
		alias: Path = files / "alias.c"
		os.link(path, alias)
		result = run_process([str(binary), "--write", "--no-lint", str(path), str(alias)], capture_output=True)
		require(not result.returncode and result.stdout.endswith(b"Done. 2/2 file(s) formatted.\n"), result)
		require(result.stdout.count(b"Formatted: ") == 2 and path.read_bytes() == alias.read_bytes() == b"int value;\n", result)
		require(not result.stderr, result)
		alias.unlink()
		# 大小文字と Unicode の同一視を実行先の名前解決で確かめ，別名の同一視と別ファイルの区別を共に検査
		case_dir: Path = dir / "CaseDir"
		case_dir.mkdir()
		for first_name, second_name in ("MiXeD-é.c", "mixed-É.c"), ("é.c", "e\u0301.c"):
			first: Path = case_dir / first_name
			second: Path = case_dir / second_name
			first.write_bytes(b"int  value;\n")
			is_same: bool = second.exists() and os.path.samefile(first, second)
			if not is_same:
				second.write_bytes(b"int  other;\n")
			result = run_process([str(binary), "--write", "--no-lint", str(second), str(first)], capture_output=True)
			require(not result.returncode and first.read_bytes() == b"int value;\n", result)
			if is_same:
				require(result.stdout == f"Formatted: {second}\nDone. 1/1 file(s) formatted.\n".encode(), result)
			else:
				require(second.read_bytes() == b"int other;\n" and result.stdout.endswith(b"Done. 2/2 file(s) formatted.\n"), result)
				second.unlink()
			first.unlink()
		# 親成分の大小違いも実際に同じディレクトリへ解決される場合だけ纏める
		first = case_dir / "a.c"
		first.write_bytes(b"int  value;\n")
		second = dir / "casedir" / "a.c"
		if second.exists() and os.path.samefile(first, second):
			result = run_process([str(binary), "--write", "--no-lint", str(second), str(first)], capture_output=True)
			require(not result.returncode and result.stdout == f"Formatted: {second}\nDone. 1/1 file(s) formatted.\n".encode(), result)
			require(first.read_bytes() == b"int value;\n", result)
		# 共有ヘッダを走査で先に収集しても，明示ファイルの C++ 指定の警告を１回保つ
		header_dir: Path = dir / "headers"
		header_dir.mkdir()
		header: Path = header_dir / "value.h"
		header.write_bytes(b"int Value = (int)Source;\n")
		for args, label in (
			(["headers", "headers/value.h"], os.path.join("headers", "value.h")),
			(["./headers/value.h", "headers"], "./headers/value.h")
		):
			result = run_process([str(binary), "--check", "--lang", "cpp", *args], cwd=dir, capture_output=True)
			require(not result.returncode, result)
			require(
				result.stdout ==
				b"Formatting check: 1 unchanged, 0 would change, 0 partial, 0 skipped (not checked), " + b"0 failed, 1 warning(s).\n",
				result
			)
			require(result.stderr.count(b"Replace the C-style cast with the appropriate C++ cast") == 1, result)
			require(f"[warn] {label}:".encode() in result.stderr and result.stderr.count(b"[warn]") == 1, result)
	# 終了
	return

def verify_argument_exit(binary: Path) -> None:
	"""使用法の拒否と表示優先を共通の終了処理で検査する
	計算量：引数の組数 A と各実行時間 T に対し O(AT)
	引数：
		binary: フォーマッターの実行ファイル
	送出例外：
		AssertionError: 終了状態又は入力保持の契約に反する場合
		OSError: パイプ又は子プロセスの作成に失敗する場合
		subprocess.TimeoutExpired: 拒否又は表示が時間上限を超える場合
	"""
	# 通常の診断は使用法エラー，書出失敗は入出力エラーとして区別する
	cases: list[list[str]] = [
		["--stdin", "--lang"],
		["--stdin", "-l"],
		["--stdin", "--chars"],
		["--stdin", "-m"],
		["--stdin", "--lang", "unsupported"],
		["--stdin", "--lang", ""],
		["--stdin", "--lang", "cpp", "--chars", "x"],
		["--stdin", "--chars", ""],
		["--stdin", "--chars", "-1"],
		["--stdin", "--chars", "9999999999999999999999999999999999"],
		["--unknown"],
		["--check", "--write"],
		["--stdin", "--check"],
		["--stdin", "--write"],
		["--stdin", "--diff"],
		["--stdin", "some.cpp"]
	]
	for args in cases:
		result: subprocess.CompletedProcess[bytes] = run_process([str(binary), *args], input=b"unprocessed input", capture_output=True)
		require(result.returncode == 2 and not result.stdout and result.stderr.startswith(b"Error:"), (args, result))
		if os.name == "posix":
			reader, writer = os.pipe()
			os.close(reader)
			try:
				failed: subprocess.CompletedProcess[bytes] = (
					run_process([str(binary), *args], input=b"unprocessed input", stdout=subprocess.PIPE, stderr=writer)
				)
				require(failed.returncode == 1 and not failed.stdout, (args, failed))
			finally:
				os.close(writer)
	# 指定した綴りと，値を取る既知のオプションだけへの対処の照合
	messages: list[tuple[list[str], str]] = [
		(["--wirte"], "Error: unknown option '--wirte'\n[notice] see --help\n"),
		(["--unknown=100"], "Error: unknown option '--unknown=100'\n[notice] see --help\n"),
		(["--write=1"], "Error: unknown option '--write=1'\n[notice] see --help\n"),
		(["--check", "--write"], "Error: --check and --write are mutually exclusive\n"),
		(["-c", "--write"], "Error: -c and --write are mutually exclusive\n"),
		(["--check", "-w"], "Error: --check and -w are mutually exclusive\n"),
		(["-c", "-w"], "Error: -c and -w are mutually exclusive\n")
	]
	# 分ければ受理される値だけに，入力した指定名と値を使う分割の案内を出す
	for options, values in (("--chars", "-m"), ("0", "000100", "100")), (("--lang", "-l"), ("CPP", "cpp", "ts")):
		for option in options:
			for value in values:
				separate: list[str] = (
					["--stdin", "--no-lint", "--lang", "json", option, value] if
					option in ("--chars", "-m") else
					["--stdin", "--no-lint", option, value]
				)
				result = run_process([str(binary), *separate], input=b"", capture_output=True)
				require(not result.returncode and not result.stdout and not result.stderr, (separate, result))
				messages.append(
					(
						[f"{option}={value}"],
						f"Error: {option} requires a separate value, got '{value}'\n" +
						f"[notice] use separate arguments: {option} {value}; see --help\n"
					)
				)
	# 分けても不正な値は，同じ値の原因を報告してから対応する対処へ案内する
	messages += [
		(
			["--chars=abc"],
			"Error: --chars requires a separate value, got 'abc'\n" + "Error: --chars requires a non-negative integer, got 'abc'\n"
		),
		(
			["--chars=9999999999999999999999999999999999"],
			"Error: --chars requires a separate value, got '9999999999999999999999999999999999'\n" +
			"Error: --chars is out of range, got '9999999999999999999999999999999999'\n"
		),
		(
			["--lang=foo"],
			"Error: --lang requires a separate value, got 'foo'\n" +
			"Error: --lang unsupported language ID 'foo'\n[notice] run --help for the list\n"
		)
	]
	# 短い連結形は値が正しい時だけ分割を案内し，既知の指定名は欠落した値として扱う
	for option, value in ("-m", "100"), ("-l", "cpp"):
		messages.append(
			(
				[option + value],
				f"Error: {option} requires a separate value, got '{value}'\n[notice] use separate arguments: {option} {value}; see --help\n"
			)
		)
	for option in "--chars", "--lang", "-l", "-m":
		messages.append(([option + "="], f"Error: {option} requires a value\n"))
		for following in "--", "--check", "--diff", "--lang", "--stdin", "-c", "-m":
			messages.append(([option, following], f"Error: {option} requires a value\n"))
	for option in "-list", "-mbad":
		messages.append(([option], f"Error: unknown option '{option}'\n[notice] see --help\n"))
	for option in "--chars", "-m":
		for value in "-5", "1e3", "9999999999999999999999999999999999", "bad", "１００":
			reason: str = "is out of range" if value == "9999999999999999999999999999999999" else "requires a non-negative integer"
			messages.append(([option, value], f"Error: {option} {reason}, got '{value}'\n"))
	for option in "--check", "--diff", "--write", "-c", "-w":
		messages.append((["--stdin", option], f"Error: --stdin cannot be combined with {option}\n"))
	for option in "--lang", "-l":
		messages.append(
			([option, "unsupported"], "Error: --lang unsupported language ID 'unsupported'\n[notice] run --help for the list\n")
		)
	for args, expected in messages:
		result = run_process([str(binary), *args], input=b"unprocessed input", capture_output=True)
		require(result.returncode == 2 and not result.stdout and result.stderr == expected.encode(), (args, result))
	# 不正な引数より表示を優先し，入力の処理へ進まない
	for option in "--help", "--version":
		for args in ["--unknown", option], [option, "--chars", "x"]:
			result = run_process([str(binary), *args], input=b"unprocessed input", capture_output=True)
			require(not result.returncode and result.stdout and not result.stderr, (args, result))
			if option == "--help":
				require(all(len(line) < 80 for line in result.stdout.splitlines()), result.stdout)
				require(b"usage error (unknown option, missing or invalid value," in result.stdout, result.stdout)
				require(b"a file or directory could not be read or processed," in result.stdout, result.stdout)
				require(
					b"Language of stdin or a named file" in result.stdout and b"(a known file extension must agree)" in result.stdout,
					result.stdout
				)
	# パス無の原因の新しい固定文は足さず，対応対象無の誤った流用を除いて使用法を保持する
	for args in [], ["--check"], ["--diff"], ["--no-lint"], ["-w"]:
		result = run_process([str(binary), *args], capture_output=True)
		require(
			result.returncode == 2 and not result.stdout and result.stderr.startswith(b"shavefmt ") and b"Usage:" in result.stderr,
			result
		)
		require(b"Error: no supported files found" not in result.stderr, result)
	# 名指したパスの不備でも，読取専用なら残りを処理し，書戻なら全ての原本を保護する
	with tempfile.TemporaryDirectory(prefix="shaveformat-path-rejection-") as temp:
		directory: Path = Path(temp).resolve()
		good: Path = directory / "good.c"
		unsupported: Path = directory / "plain.txt"
		missing: Path = directory / "missing.c"
		unsupported.write_bytes(b"unchanged\n")
		for rejected in unsupported, missing:
			label: bytes = str(rejected).encode()
			expected_error: bytes = (
				b"Error: unsupported file type: " + label + b"\n[notice] " + label + b": use --lang ID to format it anyway\n" if
				rejected == unsupported else
				b"Error: path not found: " + label + b"\n"
			)
			for entry in [], ["--check"], ["--diff"], ["--check", "--diff"], ["--write"]:
				good.write_bytes(b"int  value;\n")
				# 重複した正規パスは，処理と書戻の未処理数の何方でも１件に纏める
				result = run_process(
					[str(binary), *entry, "--no-lint", str(rejected), str(good), str(directory) + os.sep + "." + os.sep + "good.c"],
					capture_output=True
				)
				require(result.returncode == 1 and result.stderr == expected_error, (entry, rejected, result))
				if "--write" in entry:
					# 対応外の種類・存在しないパスは読込失敗でなく，名指しのパスの不備として数える
					require(
						result.stdout == b"Done. 0/1 file(s) formatted, 1 file(s) not written because a named path could not be used.\n",
						result
					)
				else:
					summary: bytes = (
						b"Formatting check: 0 unchanged, 1 would change, 0 partial, 0 skipped (not checked), 0 failed, 0 warning(s).\n" if
						"--check" in entry else
						b"Done. 1/1 file(s) would be formatted.\n"
					)
					require(result.stdout.endswith(summary), result)
					if "--diff" in entry:
						require(result.stdout.count(b"@@ -1,1 +1,1 @@\n-int  value;\n+int value;\n") == 1, result)
					else:
						require(result.stdout == b"Would format: " + str(good).encode() + b"\n" + summary, result)
				require(good.read_bytes() == b"int  value;\n" and unsupported.read_bytes() == b"unchanged\n", result)
				require(not list(directory.glob(".shavefmt-*")), result)
			# 対象の無い不備は，対応対象無の２つ目のエラーへ変えない
			result = run_process([str(binary), "--check", str(rejected)], capture_output=True)
			require(result.returncode == 1 and result.stderr == expected_error, result)
			require(
				result.stdout ==
				b"Formatting check: 0 unchanged, 0 would change, 0 partial, 0 skipped (not checked), 0 failed, 0 warning(s).\n",
				result
			)
	# 終了
	return

def verify_read_failures(binary: Path) -> None:
	"""読めない対象を結果に残し，読めない枝の兄弟も処理する事を照合する
	計算量：検査する入口数 K と各実行時間 T に対し O(K T)
	引数：
		binary: フォーマッターの実バイナリ
	送出例外：
		AssertionError: 読込の通知・継続・要約又は原本の保持に違反する場合
		OSError: 入力・権限又は子の操作に失敗する場合
		subprocess.TimeoutExpired: 処理が上限時間を超える場合
	"""
	# 管理者が権限を無視する POSIX の実行では，拒否を誘発したと扱わない終了
	if os.name == "posix" and not os.geteuid():
		# 此の権限条件で拒否を検査出来ない返戻
		return
	with tempfile.TemporaryDirectory(prefix="shaveformat-read-failure-") as temp:
		directory: Path = Path(temp).resolve()
		good: Path = directory / "a.c"
		bad: Path = directory / "b.c"
		bad.write_bytes(b"int  Y;\n")
		handle: object = None
		if os.name == "nt":
			import ctypes
			from ctypes import wintypes
			# 排他的な共有設定で，正規化と読込の何方からも同じ読込失敗を誘発する
			kernel: Any = ctypes.WinDLL("kernel32", use_last_error=True)
			kernel.CreateFileW.argtypes: list[Any] = [
				wintypes.LPCWSTR,
				wintypes.DWORD,
				wintypes.DWORD,
				wintypes.LPVOID,
				wintypes.DWORD,
				wintypes.DWORD,
				wintypes.HANDLE
			]
			kernel.CreateFileW.restype: Any = wintypes.HANDLE
			kernel.CloseHandle.argtypes: list[Any] = [wintypes.HANDLE]
			kernel.CloseHandle.restype: Any = wintypes.BOOL
			handle = kernel.CreateFileW(str(bad), 0X80000000, 0, None, 3, 0X80, None)
			require(handle != ctypes.c_void_p(-1).value, ctypes.get_last_error())
		else:
			bad.chmod(0)
		try:
			for paths in [str(directory)], [str(bad), str(good)]:
				for entry in [], ["--check"], ["--diff"], ["--write"]:
					good.write_bytes(b"int  X;\n")
					result: subprocess.CompletedProcess[bytes] = run_process([str(binary), *entry, "--no-lint", *paths], capture_output=True)
					lines: list[bytes] = result.stderr.splitlines()
					label: bytes = str(bad).encode()
					require(result.returncode == 1 and len(lines) == 2, result)
					require(lines[0].startswith(b"Error reading: " + label + b": ") and len(lines[0]) > len(label) + 17, result)
					require(lines[1] == b"[notice] " + label + b": verify read access and retry after concurrent changes stop", result)
					summary: bytes = (
						b"Formatting check: 0 unchanged, 1 would change, 0 partial, 0 skipped (not checked), 1 failed, 0 warning(s).\n" if
						"--check" in entry else
						b"Done. 1/2 file(s) " + (b"formatted" if "--write" in entry else b"would be formatted") +
						b", 1 file(s) could not be processed.\n"
					)
					require(result.stdout.endswith(summary), result)
					if "--diff" in entry:
						require(b"@@ -1,1 +1,1 @@\n-int  X;\n+int X;\n" in result.stdout, result)
					else:
						prefix: bytes = b"Formatted: " if "--write" in entry else b"Would format: "
						require(result.stdout == prefix + str(good).encode() + b"\n" + summary, result)
					require(good.read_bytes() == (b"int X;\n" if "--write" in entry else b"int  X;\n"), result)
					require(not list(directory.glob(".shavefmt-*")), result)
			# 名指し・走査・相対・絶対・点の成分を重ねても，読込失敗は１件に纏めて収集順に出す
			later: Path = directory / "c.c"
			later.write_bytes(b"int  Q;\n")
			good.write_bytes(b"int  X;\n")
			result = run_process(
				[str(binary), "--check", "--no-lint", "b.c", "./b.c", "../" + directory.name + "/b.c", str(bad), "."],
				cwd=directory,
				stdout=subprocess.PIPE,
				stderr=subprocess.STDOUT
			)
			rows: list[bytes] = result.stdout.splitlines()
			prefix: bytes = ("." + os.sep).encode()
			require(result.returncode == 1 and len(rows) == 5, result)
			require(rows[0] == b"Would format: " + prefix + b"a.c", result)
			require(
				rows[1].startswith(b"Error reading: b.c: ") and
				rows[2] == b"[notice] b.c: verify read access and retry after concurrent changes stop",
				result
			)
			require(rows[3] == b"Would format: " + prefix + b"c.c", result)
			require(
				rows[4] == b"Formatting check: 0 unchanged, 2 would change, 0 partial, 0 skipped (not checked), 1 failed, 0 warning(s).",
				result
			)
			later.unlink()
		finally:
			if os.name == "nt":
				require(kernel.CloseHandle(handle), ctypes.get_last_error())
			else:
				bad.chmod(0O600)
		require(bad.read_bytes() == b"int  Y;\n", bad)
		# 読めない枝の前後に兄弟を置き，列挙順の何方でも全て報告する
		if os.name == "posix":
			branch: Path = directory / "blocked"
			branch.mkdir()
			(branch / "inside.c").write_bytes(b"int  Z;\n")
			other_branch: Path = directory / "blocked2"
			other_branch.mkdir()
			branch.chmod(0)
			other_branch.chmod(0)
			try:
				for paths in [str(directory)], ["."]:
					for mode in "--check", "--write":
						good.write_bytes(b"int  X;\n")
						bad.write_bytes(b"int  Y;\n")
						result = run_process([str(binary), mode, "--no-lint", *paths], cwd=directory, capture_output=True)
						lines = result.stderr.splitlines()
						prefix = ("." + os.sep).encode() if paths == ["."] else (str(directory) + os.sep).encode()
						require(result.returncode == 1 and len(lines) == 4, result)
						labels: set[bytes] = { prefix + b"blocked", prefix + b"blocked2" }
						for offset in 0, 2:
							label = lines[offset][len(b"Error reading: "):].split(b": ", 1)[0]
							require(label in labels and lines[offset].startswith(b"Error reading: " + label + b": "), result)
							labels.remove(label)
							require(lines[offset + 1] == b"[notice] " + label + b": verify read access and retry after concurrent changes stop", result)
						summary = (
							b"Formatting check: 0 unchanged, 2 would change, 0 partial, 0 skipped (not checked), 0 failed, 0 warning(s).\n" if
							mode == "--check" else
							b"Done. 2/2 file(s) formatted, 2 path(s) could not be read.\n"
						)
						verb: bytes = b"Would format: " if mode == "--check" else b"Formatted: "
						require(result.stdout == verb + prefix + b"a.c\n" + verb + prefix + b"b.c\n" + summary, result)
						require(good.read_bytes() == (b"int  X;\n" if mode == "--check" else b"int X;\n"), result)
						require(bad.read_bytes() == (b"int  Y;\n" if mode == "--check" else b"int Y;\n"), result)
						require(not list(directory.glob(".shavefmt-*")), result)
				# 名指した状態の取得失敗も読込の理由と対処を出し，書戻の拒否は保つ
				for mode in "--check", "--write":
					good.write_bytes(b"int  X;\n")
					result = run_process([str(binary), mode, "--no-lint", "blocked/inside.c", "a.c"], cwd=directory, capture_output=True)
					lines = result.stderr.splitlines()
					label = b"blocked/inside.c"
					require(result.returncode == 1 and len(lines) == 2 and lines[0].startswith(b"Error reading: " + label + b": "), result)
					require(lines[1] == b"[notice] " + label + b": verify read access and retry after concurrent changes stop", result)
					expected = (
						b"Would format: a.c\nFormatting check: 0 unchanged, 1 would change, 0 partial, 0 skipped (not checked), 0 failed, 0 warning(s).\n" if
						mode == "--check" else
						b"Done. 0/1 file(s) formatted, 1 file(s) not written because a named path could not be used, 1 path(s) could not be read.\n"
					)
					require(result.stdout == expected and good.read_bytes() == b"int  X;\n", result)
					require(not list(directory.glob(".shavefmt-*")), result)
				# 名指したディレクトリだけが読めない場合も件数を保持する
				result = run_process([str(binary), "--write", "--no-lint", "blocked"], cwd=directory, capture_output=True)
				lines = result.stderr.splitlines()
				require(result.returncode == 1 and len(lines) == 2 and lines[0].startswith(b"Error reading: blocked: "), result)
				require(lines[1] == b"[notice] blocked: verify read access and retry after concurrent changes stop", result)
				require(result.stdout == b"Done. 0/0 file(s) formatted, 1 path(s) could not be read.\n", result)
			finally:
				branch.chmod(0O700)
				other_branch.chmod(0O700)
			(branch / "inside.c").unlink()
			branch.rmdir()
			other_branch.rmdir()
		# 検査対象無の除外の通知も，stderr の通知の接頭辞を保持する
		good.unlink()
		bad.unlink()
		excluded: Path = directory / "vendor"
		excluded.mkdir()
		(excluded / "hidden.c").write_bytes(b"int X;\n")
		expected: bytes = b"Error: no supported files found\n[notice] 1 path(s) excluded by name\n"
		if os.name == "posix":
			(directory / "link.c").symlink_to(excluded / "hidden.c")
			expected += b"[notice] 1 symbolic link(s) not followed\n"
		result = run_process([str(binary), "--check", str(directory)], capture_output=True)
		require(result.returncode == 1 and not result.stdout and result.stderr == expected, result)
	# 終了
	return

def unified_diff_cases() -> list[tuple[str, bytes, bytes, bytes]]:
	"""条文から手で定めた原文・整形本文・統一差分を返す
	計算量：固定件数に対して O(1)
	戻値：
		名前・原文・候補・見出しを除いた領域の列
	"""
	# 行番号・個数・文脈と末尾改行の境界の返戻
	return [
		("crlf.c", b"int  x;\r\n", b"int x;\n", b"@@ -1,1 +1,1 @@\n-int  x;\r\n+int x;\n"),
		(
			"delete.cjs",
			b"const x = 1;\n\n\nconsole.log(x);\n",
			b"const x = 1;\n\nconsole.log(x);\n",
			b"@@ -1,4 +1,3 @@\n const x = 1;\n \n-\n console.log(x);\n"
		),
		("empty.c", b"\n", b"", b"@@ -1,1 +0,0 @@\n-\n"),
		(
			"insert.cjs",
			b"const x = 1;\nconsole.log(x);\n",
			b"const x = 1;\n\nconsole.log(x);\n",
			b"@@ -1,2 +1,3 @@\n const x = 1;\n+\n console.log(x);\n"
		),
		(
			"regions.py",
			b"print(0)\nprint( 1 )\nprint(2)\nprint(3)\nprint(4)\nprint(5)\nprint(6)\nprint(7)\nprint(8)\nprint(9)\nprint(10)\nprint( 11 )\n",
			b"print(0)\nprint(1)\nprint(2)\nprint(3)\nprint(4)\nprint(5)\nprint(6)\nprint(7)\nprint(8)\nprint(9)\nprint(10)\nprint(11)\n",
			b"@@ -1,5 +1,5 @@\n print(0)\n-print( 1 )\n+print(1)\n print(2)\n print(3)\n print(4)\n"
			b"@@ -9,4 +9,4 @@\n print(8)\n print(9)\n print(10)\n-print( 11 )\n+print(11)\n"
		),
		("sample.c", b"int  x;\n", b"int x;\n", b"@@ -1,1 +1,1 @@\n-int  x;\n+int x;\n"),
		("sample.json", b"{\"x\":1}", b"{ \"x\": 1 }\n", b"@@ -1,1 +1,1 @@\n-{\"x\":1}\n\\ No newline at end of file\n+{ \"x\": 1 }\n")
	]

def verify_diff_application(cases: list[tuple[str, bytes, bytes, bytes]], diff: bytes) -> None:
	"""独立した適用処理で差分のバイト単位の意味を確認する
	計算量：本文のバイト数 B と外部処理の時間 T に対して O(B + T)
	引数：
		cases: 原文と期待する候補
		diff: CLI 又は手で定めた統一差分
	送出例外：
		AssertionError: 適用結果が期待と違う場合
		OSError: 一時ファイル又は外部コマンドへアクセス出来ない場合
		subprocess.TimeoutExpired: 外部コマンドが時間上限を超えた場合
	"""
	# 配布環境に在る独立した適用処理を使用（製品の処理を検査へ複製しない）
	# Windows の patch も改行を変換せずバイト通りに当てる
	for command in ["patch", "-p1", "--batch", "--binary"], ["git", "apply", "--whitespace=nowarn", "-"]:
		if not shutil.which(command[0]):
			print(f"SKIP unified diff application: {command[0]} is unavailable")
			continue
		with tempfile.TemporaryDirectory() as directory:
			dir: Path = Path(directory)
			for name, src, _, _ in cases:
				(dir / name).write_bytes(src)
			result: subprocess.CompletedProcess[bytes] = run_process(command, input=diff, cwd=dir, capture_output=True)
			require(not result.returncode, (command, result))
			for name, _, expected, _ in cases:
				require((dir / name).read_bytes() == expected, (command, name))
	# 終了
	return

def verify_unified_diff(binary: Path) -> None:
	"""差分の形式・非破壊・複数ファイルの適用を確認する
	計算量：固定件数の本文長 B と外部処理の時間 T に対して O(B + T)
	引数：
		binary: 検査する実行ファイル
	送出例外：
		AssertionError: 形式又は適用結果が期待と違う場合
		OSError: ファイル又は実行ファイルへアクセス出来ない場合
		subprocess.TimeoutExpired: 外部コマンドが時間上限を超えた場合
	"""
	# 名前順で出る各ファイルの見出しと領域を仕様から構成
	cases: list[tuple[str, bytes, bytes, bytes]] = sorted(unified_diff_cases())
	expected: bytes = b"".join(b"--- a/" + name.encode() + b"\n+++ b/" + name.encode() + b"\n" + hunk for name, _, _, hunk in cases)
	with tempfile.TemporaryDirectory() as directory:
		dir: Path = Path(directory)
		for name, src, _, _ in cases:
			(dir / name).write_bytes(src)
		result: subprocess.CompletedProcess[bytes] = run_process(
			[str(binary.resolve()), "--diff", "--no-lint", *[name for name, _, _, _ in reversed(cases)]],
			cwd=dir,
			capture_output=True
		)
		require(not result.returncode, result)
		require(result.stdout.startswith(expected) and result.stdout[len(expected):].startswith(b"Done."), result.stdout)
		for name, src, _, _ in cases:
			require((dir / name).read_bytes() == src, ("--diff changed the source", name))
		verify_diff_application(cases, result.stdout)
		# 無変更の対象には見出しと領域を出さない
		unchanged: Path = dir / "unchanged.c"
		unchanged.write_bytes(b"int x;\n")
		result = run_process([str(binary.resolve()), "--diff", "--no-lint", unchanged.name], cwd=dir, capture_output=True)
		require(not result.returncode and result.stdout.startswith(b"Done."), result)
		# 文脈の本文にも表示用の置換を適用し，タブと異体字セレクタを保持
		safe: Path = dir / "safe.cjs"
		safe.write_bytes("const s = \"\t❤\ufe0f\u200b\u202e\";\nconsole.log(s);\n".encode())
		result = run_process([str(binary.resolve()), "--diff", "--no-lint", safe.name], cwd=dir, capture_output=True)
		require(not result.returncode and "\"\t❤\ufe0f??\"".encode() in result.stdout, result)
		require("\u200b".encode() not in result.stdout and "\u202e".encode() not in result.stdout, result)
	# 終了
	return

def verify_diff_alias_resources(binary: Path) -> None:
	"""別名が離れて報告されても，記述子と退避先の不足を入力の失敗にしない事を確認する
	計算量：対象数 F・原文の総量 B・外部処理の時間 T に対し O(F + B + T)
	引数：
		binary: 検査する実行ファイル
	送出例外：
		AssertionError: 差分・集計・原文保持又は終了状態が仕様と異なる場合
		OSError: 一時ファイル又は実行ファイルへアクセス出来ない場合
		subprocess.TimeoutExpired: 外部処理が時間上限を超えた場合
	"""
	# 本文だけの golden では別名と記述子の上限を設定出来ない為，既存の CLI 検査へ追加する
	with tempfile.TemporaryDirectory(prefix="shaveformat-diff-alias-") as directory:
		dir: Path = Path(directory)
		for name in "a", "b":
			(dir / name).mkdir()
		# 別名の後半を全て離し，３００組が同時に再利用を待つ入力と，退避量が 16 MiB を超える入力
		for count, suffix, src, hunk in (
			(300, ".c", b"int  value ;\n", b"@@ -1,1 +1,1 @@\n-int  value ;\n+int value;\n"),
			(10, ".json", b"{ \"value\": \"" + b"a" * 0X100000 + b"\" }\n", b"")
		):
			paths: list[str] = []
			for idx in range(count):
				name: str = "f%03d%s" % (idx, suffix)
				shared: Path = dir / "a" / name
				alias: Path = dir / "b" / name
				shared.write_bytes(src)
				os.link(shared, alias)
				paths.extend(["a/" + name, "b/" + name])
			paths.sort()
			expected: bytes = (
				b"".join(b"--- a/" + path.encode() + b"\n+++ b/" + path.encode() + b"\n" + hunk for path in paths) if hunk else b""
			)
			expected += f"Done. {2 * count if hunk else 0}/{2 * count} file(s) would be formatted.\n".encode()
			command: list[str] = [str(binary.resolve()), "--diff", "--no-lint", "--chars", "0", "a", "b"]
			if os.name == "posix":
				# 上限は検査する子だけに適用し，親の記述子と其の他の実行を変えない
				command = [
					sys.executable,
					"-c",
					"import os, resource, sys; "
					"soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE); "
					"resource.setrlimit(resource.RLIMIT_NOFILE, (256 if soft == resource.RLIM_INFINITY else min(256, soft), hard)); "
					"os.execv(sys.argv[1], sys.argv[1:])",
					*command
				]
			result: subprocess.CompletedProcess[bytes] = run_process(command, cwd=dir, capture_output=True)
			require(not result.returncode and result.stdout == expected and not result.stderr, result)
			for path in paths:
				require((dir / path).read_bytes() == src, ("--diff changed an alias", path))
				(dir / path).unlink()
	# 終了
	return

def main() -> int:
	"""実行ファイルを指定して通常経路と原文保持を検査する
	計算量：テストケース数 F・外部処理の最大実行時間 T に対して O(FT)
	戻値：
		終了コード
	送出例外：
		AssertionError: 呼出先の検査で期待した契約との不一致を検出した場合
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		json.JSONDecodeError: 読み込んだ内容が有効な JSON でない場合
		subprocess.CalledProcessError: 呼び出した子プロセスが非零の終了値を返す場合
		subprocess.TimeoutExpired: 呼び出した子プロセスが制限時間内に完了しない場合
		SyntaxError: 解析する入力が対象の言語バージョンの有効な構文でない場合
		ValueError: 読み込んだ値が呼出先の受け入れる形式でない場合
	"""
	# 一時ディレクトリで起動する検査も同じ実行ファイルを指す様に，指定を絶対パスへ解決した個別検査の実行
	binary: Path = (Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/shavefmt").resolve()
	verify_unified_diff(binary)
	verify_diff_alias_resources(binary)
	verify_argument_exit(binary)
	verify_read_failures(binary)
	verify_input_review(binary)
	verify_path_encoding(binary)
	verify_security_pm_review(binary)
	verify_python_wrapping(binary)
	verify_error_zone_loss(binary)
	verify_python_annotations(binary)
	verify_postfix_scaling(binary)
	verify_keyword_names(binary)
	verify_restricted_line_breaks(binary)
	verify_javascript_comparisons(binary)
	verify_jsx_comments(binary)
	verify_type_combinations(binary)
	verify_control_bodies(binary)
	verify_nested_decls(binary)
	verify_empty_inputs(binary)
	# 標準入力の制御文字を EOF と誤認しない事の照合
	src: str = "const text = \"a\u001ab\"; console.log(text);\n"
	command: list[str] = [str(binary), "--stdin", "--lang", "javascript", "--chars", "20", "--no-lint", "--fail-on-skip"]
	first: bytes = run_process(command, input=src.encode(), capture_output=True, check=True).stdout
	require(b"a\x1ab" in first, "stdin was truncated at control-Z")
	# コメントの除去・閉括弧の同行化・HTML の終端処理で本文やトークンを壊さない
	preserved: list[tuple[str, str, str]] = [
		("cpp", "int F(int a,int b){return a+/*c*/++b;}\n", "a\\s*\\+\\s+\\+\\+b"),
		("javascript", "const s = `}\nelse text`; console.log(s);\n", "`}\\nelse text`"),
		("javascript", "const s = `}\nwhile(x);`; console.log(s);\n", "`}\\nwhile\\(x\\);`"),
		("javascript", "function F(){return/*\n*/1;}\n", "return;\\s+1;"),
		("javascript", "function F(){return/*\r*/1;}\n", "return;\\s+1;"),
		("html", "<img src=/assets/>\n", "src=/assets/>")
	]
	for lang, src, pattern in preserved:
		command: list[str] = [str(binary), "--stdin", "--lang", lang, "--no-lint", "--fail-on-skip"]
		first: bytes = core(command, src.encode())
		require(re.search(pattern, first.decode()), (lang, "content or token changed", first))
	# 深い制御構文と冗長なブロックも，上限内に処理を終え，異常終了しない（整形仕様 9.1, 9.2）
	# 同じ位置で終わるネストの本体の後の空行も，本体毎に読み直さない
	# エラー回復は入力の長さの２乗で膨らむ為，数 KB の閉じないコメントの記号の並びは期限で打ち切る
	for case_name, lang, src, reasons in (
		("unmatched comment closers", "c", "*/\n" * 4000, (b"parsing exceeds the time limit", b"parser could not read the source")),
		("deep unbraced controls", "c", "void f(int a) {\n" + "if(a) " * 3200 + "x();\n}\n", ()),
		("deep unbraced controls with blank tail", "c", "void f(int a) {\n" + "if (a) " * 4000 + "x();\n" + "\n" * 8000 + "}\n", ()),
		("deep JavaScript blocks", "javascript", "function f(a) {\n" + "if (a) { " * 4000 + "x();" + " }" * 4000 + "\n}\n", ()),
		("deep C blocks", "c", "void f(int a) {\nif (a) " + "{ " * 4000 + "x();" + " }" * 4000 + " else { y(); }\n}\n", ())
	):
		result: subprocess.CompletedProcess[bytes] = (
			run_process([str(binary), "--stdin", "--lang", lang, "--no-lint"], input=src.encode(), capture_output=True, timeout=30)
		)
		require(not result.returncode, (case_name, lang, result.returncode, result.stderr[:200]))
		if reasons:
			require(any(reason in result.stderr for reason in reasons), (case_name, lang, result.stderr[:200]))
	# 開いた要素の名前をパーサーが失い閉タグを読み違えた HTML だけをパーサーの制限の理由で見送る（整形仕様 9.3）
	# パーサーの手間が膨らむ其の他の深いネストは，解析１回毎の期限で打ち切るか上限の内に整形を終える（整形仕様 9.1）
	for src, is_skipped in (
		("<li><p>" * 5000 + "x", False),
		("<div a=/>" * 10000 + "</div>" * 10000, True),
		("<html>" + "<div>" * 5000 + "&amp;" * 50000 + "</html>", False),
		("<div>" + "<my-element>" * 92 + "<li><p>x</p></li></div>", False),
		("<my-element>" * 85 + "x" + "</my-element>" * 85, True),
		("<div>" * 1100 + "x" + "</div>" * 1100, True),
		("\r" + "<li><p>" * 5000 + "x", False)
	):
		result: subprocess.CompletedProcess[bytes] = (
			run_process([str(binary), "--stdin", "--lang", "html"], input=src.encode(), capture_output=True, timeout=60)
		)
		require(not result.returncode, (src[:20], result.stderr[:200]))
		require((b"nesting exceeds parser limit" in result.stderr) == is_skipped, (src[:20], result.stderr[:200]))
	# 過去に超線形時間を要した入力グループも上限内で終わる事の検証
	else_chain: str = (
		"func f(a: Int) {\n\tif a == 0 {\n\t\t// c\n\t}" + "".join(f" else if a == {i} {{\n\t\t// c\n\t}}" for i in range(1, 1000)) +
		"\n}\n"
	)
	for lang, src in (
		("cpp", "// " + "日[" * 20000 + "\nint x;\n"),
		("scss", "a {\n" * 200 + "@mixin m() {}\n" * 300 + "}\n" * 200),
		("swift", else_chain),
		("swift", "let y = " + "x!+" * 6000 + "1\n"),
		("swift", "let v = a" + "".join(" ?? b%d * c%d << d%d" % (idx, idx, idx) for idx in range(3000)) + "\n"),
		("typescript", "x = new a!." + ".".join("m%d" % idx for idx in range(6400)) + "();\n"),
		("typescript", "const x = new A!." + ".".join("m%d" % idx for idx in range(8000)) + ";\n"),
		("ruby", "x = a" + " + a" * 8000 + "\n"),
		("python", "x = 1 if c else " * 2000 + "0\n"),
		("javascript", ("a" + "(" * 4000 + " // c\n1" + ")" * 4000 + ";\n") * 5),
		("javascript", "\n".join("{ // note%d" % idx for idx in range(1000)) + "\nx;\n" + "}\n" * 1000),
		("css", ("a:not(" * 600 + "b /* c */" + ")" * 600 + " {}\n") * 20),
		("html", "<a x=\"1\" y=\"" * 3000),
		("html", "<!--" * 4000),
		("c", "".join("int f%d(int a) { return (A) / a; }\n" % idx for idx in range(4000))),
		("go", "package p\n\nfunc ExampleX() {\n\tx()\n" + "".join("\t// line %d\n" % idx for idx in range(20000)) + "}\n"),
		("cpp", "// " + "あ。" * 40000 + "\nint x;\n"),
		("python", "#x\n" * 40000 + "x = 1\n"),
		("c", "void f(int x) { switch(x) { case 1:\n" + "a();\n" * 10000 + "// c\n" * 10000 + "} }\n"),
		("kotlin", "fun f() {\n" + "a()\n" * 10000 + "// c\n" * 10000 + "}\n"),
		("go", "package p\n\nfunc f() {\n" + "a()\n" * 10000 + "// c\n" * 10000 + "}\n"),
		("javascript", "".join("x%d = /a*/ // c\n" % idx for idx in range(20000))),
		("javascript", "f(function () {\nx();\n" + "// eslint-disable-next-line\n" * 20000 + "}, y);\n"),
		("javascript", "function f() {\nx();\n" + ("// " + "c" * 400 + "\n") * 10000 + "}\n"),
		("javascript", "/*a*/ " * 32000 + "\nx();\n"),
		("c", "void f() {\nx();\n" + "// c\n" * 20000 + " " * 20000 + "y();\n}\n"),
		("c", "void f() {\nx();\n" + "/* NOLINTNEXTLINE */ " * 100000 + "\n}\n"),
		("c", "".join("#define T%d T%d\n" % (idx, idx - 1) for idx in range(4000, 0, -1)) + "#define T0 a + b\nint x = T4000;\n"),
		("cpp", "void g() { h(" + "f<N + 1>(" * 800 + "0" + ")" * 800 + "); }\n"),
		(
			"kotlin",
			"fun f(a: Int) {\n\tvar x = 0\n\tif (a == 0) { x = 0 } " +
			"".join("else if (a == %d) { x = %d } " % (idx, idx) for idx in range(1, 1600)) + "else { x = 1 }\n}\n"
		),
		(
			"kotlin",
			"fun f(a: Int): Int {\n\tval y = if (a == 0) 0 " + "".join("else if (a == %d) %d " % (idx, idx) for idx in range(1, 3200)) +
			"else 0\n\treturn y\n}\n"
		),
		("ruby", "x = [" + ", ".join(["if a then 1 else 2 end"] * 8000) + "]\n"),
		(
			"ruby",
			"def f(a)\n  if a == 0\n    0\n" + "".join("  elsif a == %d\n    %d\n" % (idx, idx) for idx in range(1, 8000)) + "  end\nend\n"
		),
		("ruby", "x = " + "if a then " * 800 + "1" + " else 2 end" * 800 + "\n"),
		("csharp", "class K {\n\tint " + ", ".join("A%d = F(/* c */ %d)" % (idx, idx) for idx in range(4000)) + ";\n}\n"),
		(
			"javascript",
			"var " + ", ".join("a%d = f(/* c */ %d, function() { /* d */ return 1; })" % (idx, idx) for idx in range(4000)) + ";\n"
		),
		("javascript", "x = " + "(( // c\n" * 8000 + "1" + "))" * 8000 + ";\n"),
		("c", "void f() {\n" + "while (a > 0) {\n" * 400 + "g();\n" + "}\n" * 400 + "}\n"),
		("kotlin", "fun f() {\n" + "if (a > 0) {\n" * 400 + "g()\n" + "}\n" * 400 + "}\n"),
		("c", "int x = " + "( /* c */ " * 4000 + "1" + ")" * 4000 + ";\n"),
		("python", "x = " + "(  # c\n" * 4000 + "1" + ")" * 4000 + "\n"),
		("c", "// " + "これはtestです" * 16000 + "\nint x;\n"),
		("c", "// " + "あa/b.c" * 16000 + "\nint x;\n"),
		(
			"c",
			"#define T a + b\n" + "".join("#define U%d T\n" % idx for idx in range(8000)) + "#define BIG (" +
			" ".join("U%d" % idx for idx in range(8000)) + ")\nint x = BIG;\n"
		)
	):
		# 時間だけを見ると異常終了も合格に為る為，終了状態も確かめる（意図して見送るテストケースは上の表で理由を照合して居る）
		result: subprocess.CompletedProcess[bytes] = (
			run_process([str(binary), "--stdin", "--lang", lang, "--no-lint"], input=src.encode(), capture_output=True, timeout=10)
		)
		require(not result.returncode, (lang, result.returncode, result.stderr[:200]))
		# パーサーが読めない連鎖はネストの上限内の為見送らず，８段の連鎖と同じくエラー領域を保つ部分整形とする
		if lang == "python" and src.startswith("x = 1 if c else "):
			require(
				result.stdout == src.encode() and b"formatting is partial" in result.stderr and b"[skip]" not in result.stderr,
				result.stderr[:200]
			)
		elif src.startswith("const x = new A!.") or src.startswith("x = a + a"):
			require(b"[skip]" not in result.stderr, (lang, result.stderr[:200]))
	# 静的解析自身も時間の上限の内に終わる（他の検査は `--no-lint` で整形の側だけを見る為，検査の走査の膨らみは此処でしか現れない）
	# ドキュメントコメントの並びを基準点から１つずつ遡ると，遡りが毎回根から降り直す為に並びの長さの２乗に膨らむ
	for lang, src in (
		("ruby", "# comment line\n" * 32000 + "def two\n\treturn 7\nend\n"),
		("ruby", "".join("# c\n" * 8 + "def f%d\n\treturn 1\nend\n" % idx for idx in range(4000))),
		("typescript", "function f(" + ",".join("p%d" % idx for idx in range(8192)) + "){ return 1; }\n")
	):
		# 時間だけを見ると，異常終了や即時の見送も合格に為る為，終了状態と検査の実施も確かめる
		# 仮引数の個別の先頭走査も検出し，折返の費用は既定の無効指定で除く
		args: list[str] = ["--chars", "0"] if lang == "typescript" else []
		result: subprocess.CompletedProcess[bytes] = run_process(
			[str(binary), "--stdin", "--lang", lang, "--fail-on-skip", *args],
			input=src.encode(),
			capture_output=True,
			timeout=10,
			check=True
		)
		require(b"[skip]" not in result.stderr, result.stderr)
		require(b"[warn]" in result.stderr, "the static checker did not run")
	# 貯留の返却漏れは出力も終了状態も変えない為，常駐量でしか気付けない（走査カーソルの漏れは入力の１０００倍に達した事が有る）
	# `RUSAGE_CHILDREN` は同じ処理が起こした全ての子の最大値の為，計測用の子を１つ起こして其の中で測る
	if os.name == "posix":
		src: str = "class C { int F(int x) { if (x > 0) { return x + 1; } return 0; } }\n" * 4000
		probe: str = (
			"import resource, subprocess, sys\n"
			"data = sys.stdin.buffer.read()\n"
			"subprocess.run(sys.argv[1:], input=data, capture_output=True, check=True, timeout=45)\n"
			"print(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)\n"
		)
		measured: subprocess.CompletedProcess[bytes] = run_process(
			[sys.executable, "-c", probe, str(binary), "--stdin", "--lang", "java", "--no-lint", "--fail-on-skip"],
			input=src.encode(),
			capture_output=True,
			timeout=60,
			check=True
		)
		# macOS はバイト，Linux はキビバイトで返す
		peak: int = int(measured.stdout) if sys.platform == "darwin" else int(measured.stdout) << 10
		# 期待構文と独立に解析した出力の構文を含む常駐量を検査し，使用を終えた解析木や作業領域の保持の退行も検出する
		# 入力に比例する分が線形で漏れ（入力の１０００倍）と区別出来る様，基礎量へ入力の７６８倍を足した上限で見る
		limit: int = 768 * len(src) + 0X1000000
		require(peak < limit, ("peak memory grew far beyond the input", peak, limit, len(src)))
		print(f"peak memory: {peak >> 20} MiB for {len(src) >> 10} KiB of input")
	# Swift の演算子へ密着したコメントは全ての開きを纏めて修復し，１度の差分解析の再利用領域を読取量へ数えない（整形仕様 9.3）
	glued: str = "".join("let v%d = 1\n" % idx for idx in range(20000)) + "let b = y" + " +/* c */1" * 40 + "\n"
	result: subprocess.CompletedProcess[bytes] = (
		run_process([str(binary), "--stdin", "--lang", "swift", "--no-lint"], input=glued.encode(), capture_output=True, timeout=30)
	)
	require(not result.returncode and b"[skip]" not in result.stderr and result.stdout.count(b"/* c */") == 40, result.stderr[:200])
	# 編集数６４・６５の境界で，未編集の大きいコメントを全文の再読取へ変えない（仕様 9.3）
	for count in 64, 65:
		src: str = (
			"func f(a: Int, b: Int) -> Int {\n" + "".join("\tlet v%d = a +/* c */ b\n" % idx for idx in range(count)) +
			"\treturn a\n}\n// " + "x" * 1200000 + "\n"
		)
		result: subprocess.CompletedProcess[bytes] = (
			run_process([str(binary), "--stdin", "--lang", "swift", "--no-lint"], input=src.encode(), capture_output=True, timeout=10)
		)
		require(
			not result.returncode and b"[skip]" not in result.stderr and result.stdout.count(b"/* c */") == count,
			(count, result.stderr[:200])
		)
	# パーサーの手間がコメントの並びの長さの２乗に膨らむ入力も，解析１回毎の期限で打ち切るか上限の内に整形を終える（整形仕様 9.1, 9.3）
	# Ruby・PHP のデータ境界の試行解析も同じ期限を持ち，未完了を通常の解析不一致へ落とさない
	for lang, src in (
		("kotlin", "fun f() {\nval a = foo\n" + "// c\n" * 16000 + ".bar()\n}\n"),
		("php", "<?php\n$a = $foo\n" + "// c\n" * 16000 + "->bar();\n__halt_compiler();\nDATA\n"),
		("javascript", "a = foo\n" + "// c\n" * 16000 + ".bar()\n"),
		("typescript", "a = foo\n" + "/**/ " * 16000 + "\n.bar()\n"),
		("ruby", "x = a\n" + "# c\n" * 64000 + ".b\n__END__\nDATA\n"),
		("python", "def f():\n" + "    # c\n" * 8000 + "    return 1\n")
	):
		result: subprocess.CompletedProcess[bytes] = (
			run_process([str(binary), "--stdin", "--lang", lang, "--no-lint"], input=src.encode(), capture_output=True, timeout=60)
		)
		require(
			not result.returncode and (b"[skip]" not in result.stderr or b"parsing exceeds the time limit" in result.stderr),
			(lang, result.stderr[:200])
		)
		if b"[skip]" in result.stderr:
			# 期限切は整形途中の候補を返さず原文を保持する（整形仕様 8.3, 9.1）
			require(result.stdout == src.encode(), (lang, "trial timeout changed the original"))
		elif lang in ("php", "ruby"):
			# 期限の内に境界を読めた場合はデータの全体と終端を保持する（整形仕様 4.4）
			require(result.stdout.endswith(b"\nDATA\n"), (lang, "trial parsing lost trailing data"))
	for lang, src in (
		("javascript", "x = 1; /*\n" + "// old line: doSomething(a, b, c);\n" * 4000 + "*/\ny = 2;\n"),
		("php", "<?php\n$x = 1; /*\n" + "# old line: doSomething($a);\n" * 4000 + "*/\n$y = 2;\n")
	):
		result: subprocess.CompletedProcess[bytes] = (
			run_process([str(binary), "--stdin", "--lang", lang, "--no-lint"], input=src.encode(), capture_output=True, timeout=10)
		)
		require(b"[skip]" not in result.stderr, (lang, result.stderr[:200]))
	result: subprocess.CompletedProcess[bytes] = run_process(
		[str(binary), "--stdin", "--lang", "swift", "--no-lint"],
		# 開きの修復に依り新たに行コメントに為る本文は，既存のトークンの木を再利用出来ず，1 MiB より多く読む
		input=("//* " + "n" * 0X100000 + "\nlet value = 1\n").encode(),
		capture_output=True,
		timeout=10
	)
	require(b"comments glued to operators exceed parser limit" in result.stderr, result.stderr[:200])
	# 不正入力の原文保持の確認
	guarded: bytes = b"\xef\xbb\xbf{\x00}\r\n"
	result: subprocess.CompletedProcess[bytes] = (
		run_process([str(binary), "--stdin", "--lang", "json", "--fail-on-skip"], input=guarded, capture_output=True)
	)
	require(result.returncode == 1 and result.stdout == guarded and b"[skip]" in result.stderr)
	# ファイル書戻経路の検査
	with tempfile.TemporaryDirectory(prefix="shaveformat-runtime-") as temp:
		# macOS の一時領域はシンボリックリンクの `/var` を経る為，明示指定でもリンクを辿らないフォーマッターへ実体のパスを渡す
		dir: Path = Path(temp).resolve()
		verify_duplicate_paths(binary.resolve(), dir)
		path: Path = dir / "sample.json"
		path.write_bytes(b"\xef\xbb\xbf{ \"x\" : 1 }\r\n")
		run_process([str(binary), "--write", str(path)], capture_output=True, check=True)
		result: bytes = path.read_bytes()
		# 成功した出力は BOM 無・LF とする（整形仕様 10.3）
		require(not result.startswith(b"\xef\xbb\xbf") and b"\r" not in result, result)
		require(list(dir.iterdir()) == [path], "temporary file was left behind")
		# 末尾区切文字はパスの表記に過ぎない為，通常ディレクトリをシンボリックリンクと誤認しない
		scan_dir: Path = dir / "scan"
		scan_dir.mkdir()
		(scan_dir / "formatted.c").write_bytes(b"int x;\n")
		scan_result: subprocess.CompletedProcess[bytes] = (
			run_process([str(binary), "--check", "--fail-on-skip", f"{scan_dir}{os.sep}"], capture_output=True)
		)
		require(not scan_result.returncode and b"symbolic link(s) not followed" not in scan_result.stderr, scan_result)
		(scan_dir / "formatted.c").unlink()
		scan_dir.rmdir()
		header: Path = dir / "ambiguous.h"
		header.write_bytes(b"template <typename T> void Set(T Value) { value_ = Value; }\n")
		header_result: subprocess.CompletedProcess[bytes] = run_process([str(binary), "--write", str(header)], capture_output=True)
		require(not header_result.returncode and b"parser could not read the source" not in header_result.stderr.lower())
		expected_header: (
			bytes
		) = b"/**\n *\n * @param Value\n */\ntemplate<typename T> void Set(T Value) {\n\tvalue_ = Value;\n\treturn;\n}\n"
		require(header.read_bytes() == expected_header, "C++ header detection did not preserve the complete definition")
		header.unlink()
		# 指定した文法の保護領域は入口の違いで見送らず，部分整形する（整形仕様 4.6, 8.3, 11.2）
		assumed: Path = dir / "assumed"
		assumed_src: bytes = b"function f(body: T) {\nreturn   body;\n}\n"
		assumed.write_bytes(assumed_src)
		stdin_assumed: subprocess.CompletedProcess[bytes] = (
			run_process([str(binary), "--stdin", "--lang", "javascript", "--no-lint"], input=assumed_src, capture_output=True)
		)
		assumed_result: subprocess.CompletedProcess[bytes] = (
			run_process([str(binary), "--lang", "javascript", "--no-lint", "--write", str(assumed)], capture_output=True)
		)
		require(not assumed_result.returncode and b"formatting is partial" in assumed_result.stderr, assumed_result)
		require(not stdin_assumed.returncode and b"formatting is partial" in stdin_assumed.stderr, stdin_assumed)
		require(assumed.read_bytes() == stdin_assumed.stdout, "assumed language changed the classification or output by entry")
		require(b"body: T" in assumed.read_bytes(), "partial formatting did not preserve the unrecognized syntax")
		assumed.unlink()
		# ハードリンク切断は書戻の通知とし，静的解析の警告へ数えない（仕様 8.4）
		original: bytes = b"{ \"x\" : 1 }\n"
		path.write_bytes(original)
		alias: Path = dir / "hard-link.json"
		os.link(path, alias)
		hard_link_result: subprocess.CompletedProcess[bytes] = (
			run_process([str(binary), "--write", str(path)], capture_output=True, check=True)
		)
		require(b"[notice] " in hard_link_result.stderr and b"hard link was broken" in hard_link_result.stderr, hard_link_result)
		require(hard_link_result.stdout.endswith(b"Done. 1/1 file(s) formatted.\n"), hard_link_result)
		require(alias.read_bytes() == original and path.read_bytes() != original, "hard link contents were not separated")
		alias.unlink()
		# 拡張機能は `.h` のドキュメントに `--lang h` を渡し，標準入力でもファイルの `.h` と同じく中身で C / C++ を判別させる
		for src, expected in (
			(b"template <typename T> void Set(T Value) { value_ = Value; }\n", expected_header),
			(b"int init(void);\n", b"int init(void);\n")
		):
			stdin_header: bytes = (
				run_process([str(binary), "--stdin", "--lang", "h", "--no-lint"], input=src, capture_output=True, check=True).stdout
			)
			require(stdin_header == expected, ("--lang h did not detect the header language like a .h file", stdin_header))
		# macOS の拡張属性と権限の確認
		if sys.platform == "darwin":
			import pwd
			original = b"{\"x\":1}\n"
			path.write_bytes(original)
			run_process(["xattr", "-w", "com.shaveformat.test", "preserved", str(path)], check=True)
			run_process([str(binary), "--write", str(path)], capture_output=True, check=True)
			require(command_output(["xattr", "-p", "com.shaveformat.test", str(path)]).strip() == b"preserved")
			path.write_bytes(original)
			run_process(["chmod", "+a", f"user:{pwd.getpwuid(os.getuid()).pw_name} allow read", str(path)], check=True)
			try:
				access: list[bytes] = command_output(["ls", "-le", str(path)]).splitlines()[1:]
				failed: subprocess.CompletedProcess[bytes] = run_process([str(binary), "--write", str(path)], capture_output=True)
				require(failed.returncode == 1 and path.read_bytes() == original)
				require(access == command_output(["ls", "-le", str(path)]).splitlines()[1:])
				require(list(dir.iterdir()) == [path], "temporary file was left behind")
			finally:
				run_process(["chmod", "-N", str(path)], check=True)
			run_process([str(binary), "--write", str(path)], capture_output=True, check=True)
		# 標準出力への書込失敗の確認
		if os.name == "posix":
			def limit_output() -> None:
				"""原本に触れず，子の標準出力だけを書込不能にする"""
				# 子の標準出力の書込上限の設定
				resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))
				signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
				# 終了
				return
			empty: Path = dir / "empty"
			empty.mkdir()
			for args in ["--check", str(path)], ["--help"], ["--version"], [str(empty)]:
				with (dir / "stdout").open("wb") as output:
					failed: subprocess.CompletedProcess[bytes] = (
						run_process([str(binary), *args], stdout=output, stderr=subprocess.PIPE, preexec_fn=limit_output)
					)
				require(failed.returncode == 1 and b"failed to write stdout" in failed.stderr, args)
			# 明示指定のパスもリンクを経れば辿らず（木の外の実体を書き換えない），名指した物を読まなかった事を `--fail-on-skip` で失敗に数える
			# シンボリックリンク経由の明示パスの拒否確認
			real: Path = dir / "real"
			real.mkdir()
			(real / "sample.c").write_bytes(b"int  x;\n")
			(real / "formatted.c").write_bytes(b"int x;\n")
			(dir / "link").symlink_to(real)
			linked: str = str(dir / "link" / "sample.c")
			for args, code in (
				(["--check", str(real / "formatted.c"), linked], 0),
				(["--check", "--fail-on-skip", str(real / "formatted.c"), linked], 1),
				(["--write", "--fail-on-skip", linked], 1)
			):
				result: subprocess.CompletedProcess[bytes] = run_process([str(binary), *args], capture_output=True)
				require(result.returncode == code and b"1 symbolic link(s) not followed" in result.stdout, (args, result))
			require((real / "sample.c").read_bytes() == b"int  x;\n", "file behind a symbolic link was rewritten")
			# リンクの後の `..` を畳む前に元のパスを拒否し，通常のパスだけを集計
			hidden_link: str = str(dir / "link" / ".." / "real" / "formatted.c")
			result = run_process([str(binary), "--check", "--fail-on-skip", str(real / "formatted.c"), hidden_link], capture_output=True)
			require(result.returncode == 1 and b"1 symbolic link(s) not followed" in result.stdout, result)
			require(b"Formatting check: 1 unchanged, 0 would change" in result.stdout, result)
			require(b"link component" in result.stderr, result)
			# 行先の無いリンクを名指したパスも，不在のパスでなく辿らないリンクとして数える
			(dir / "dangling.c").symlink_to(dir / "missing.c")
			for args, code in (
				(["--check", str(real / "formatted.c"), str(dir / "dangling.c")], 0),
				(["--check", "--fail-on-skip", str(real / "formatted.c"), str(dir / "dangling.c")], 1)
			):
				result: subprocess.CompletedProcess[bytes] = run_process([str(binary), *args], capture_output=True)
				require(
					result.returncode == code and b"1 symbolic link(s) not followed" in result.stdout and b"path not found" not in result.stderr,
					(args, result)
				)
	print("runtime: byte preservation OK")
	# 全検査を通過した事の返戻
	return 0

if __name__ == "__main__":
	sys.exit(main())
