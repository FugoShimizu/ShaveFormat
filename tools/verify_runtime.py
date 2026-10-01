#! /usr/bin/env python3
# 各配布環境で全言語の起動・再整形・原文保持を確認する
from __future__ import annotations

import ast
import json
import os

from pathlib import Path

import re
import subprocess
import sys
import tempfile

from typing import Any

if os.name == "posix":
	import resource
	import signal

ROOT: Path = Path(__file__).resolve().parent.parent
# 中核の出力を読取専用で観測する環境（整形結果を変えず，コメントを置かない字面を標準エラーへ出す，整形仕様 10.5）
CORE_ENV: dict[str, str] = dict(os.environ, SHAVEFMT_VERIFY_COMMENT_SEPARATION="1")
COMMAND_TIMEOUT: int = 120

def run_process(*args: Any, timeout: float = COMMAND_TIMEOUT, **kwargs: Any) -> subprocess.CompletedProcess[Any]:
	"""時間上限付で子処理を実行する
	計算量：子処理の実行時間 T に対して O(T)
	"""
	# 実行結果の返戻
	return subprocess.run(*args, timeout=timeout, **kwargs)

def core(command: list[str], source: bytes) -> bytes:
	"""整形した時の中核の出力（コメントを置かない字面）を返す
	計算量：入力のバイト数 B・整形器の実行時間 T に対して O(B+T)
	"""
	# 整形時の中核出力の観測
	result: subprocess.CompletedProcess[bytes] = run_process(command, input=source, env=CORE_ENV, capture_output=True, check=True)
	lines: list[bytes] = [
		bytes.fromhex(line[len(b"[layout] "):].decode()) for line in result.stderr.split(b"\n") if line.startswith(b"[layout] ")
	]
	# 中核の出力の返戻
	return b"".join(line + b"\n" for line in lines)

def command_output(*args: Any, timeout: float = COMMAND_TIMEOUT, **kwargs: Any) -> Any:
	"""時間上限付で子処理の標準出力を得る
	計算量：子処理の実行時間 T に対して O(T)
	"""
	# 標準出力の返戻
	return subprocess.check_output(*args, timeout=timeout, **kwargs)

CASES: dict[str, str] = {
	"c": "int F(int x) { return x+1; }\n",
	"cpp": "int F(int x) { return x+1; }\n",
	"csharp": "class C { int F(int x) { return x+1; } }\n",
	"java": "class C { int F(int x) { return x+1; } }\n",
	"go": "package p\nfunc F(x int) int { return x+1 }\n",
	"rust": "fn f(x: i32) -> i32 { x+1 }\n",
	"kotlin": "fun f(x: Int): Int { return x+1 }\n",
	"swift": "func F(_ x: Int) -> Int { return x+1 }\n",
	"php": "<?php\nfunction F($x) { return $x+1; }\n",
	"javascript": "const text = \"a\u001ab\"; console.log(text);\n",
	"javascriptreact": "const V = <span>value</span>;\n",
	"typescript": "const n: number = 1+2;\n",
	"typescriptreact": "const V = <span>value</span>;\n",
	"ruby": "def f(x); return x+1; end\n",
	"python": "def f(x: int) -> int:\n    return x+1\n",
	"json": "{\"n\": 1, \"s\": \"text\"}\n",
	"jsonc": "{\"n\": 1, /* note */ \"s\": \"text\"}\n",
	"html": "<svg/><p>text</p>\n",
	"css": ".a { color: red; margin: 0; }\n",
	"scss": "$c: red; .a { color: $c; }\n"
}

def python_tree(source: str|bytes, **options: Any) -> str:
	"""Python の構文木の字面を返す（関数の末尾に補う値の無い `return` は処理を変えない為に除いて比べる）
	計算量：原文の文字数 L・構文木の節点数 N に対して O(L+N)
	引数：
		source: 原文
		options: `ast.dump` への指定
	戻値：
		構文木の字面
	"""
	# 構文木の生成と末尾の空返戻の除去
	tree: ast.Module = ast.parse(source)
	for node in ast.walk(tree):
		if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and len(node.body) > 1:
			if isinstance(node.body[-1], ast.Return) and node.body[-1].value is None:
				node.body.pop()
	# 構文木の字面の返戻
	return ast.dump(tree, **options)

def require(condition: object, message: object = None) -> None:
	"""検査条件を確認する
	引数：
		condition: 成功条件
		message: 失敗時の内容
	送出例外：
		AssertionError: 条件を満たさない場合
	"""
	if not condition:
		raise AssertionError(message)
	# 終了
	return

def verify_python_wrapping(binary: Path) -> None:
	"""Python の折返で式・本体・文字列を保持し，各幅で収束する事を検査する
	計算量：検体数 F・整形器の最大実行時間 T に対して O(FT)
	引数：
		binary: 整形器の実行ファイル
	"""
	# 折返検体の原文と期待条件の構築
	items: str = ", ".join(f"value_{i}" for i in range(24))
	addition: str = " + ".join(f"value_{i}" for i in range(24))
	logic: str = " and ".join(f"flag_{i}" for i in range(24))
	values_source: str = f"def f():\n    values = [{items}]\n    return values\n"
	addition_source: str = f"total_slots = {addition}\n"
	wrapped_addition_source: str = f"total_slots = (\n    {addition}\n)\n"
	multiline_container: str = "value = [\n    \"\"\"A\n    B  \n\"\"\",\n    \"second\",\n]\n"
	future_annotation: str = f"from __future__ import annotations\nx: {addition}\n"
	inline_container: str = "value = [\"\"\"A\n    B  \n\"\"\", \"second\"]\n"
	commented_container: str = "value = [\n    \"\"\"A\n    B  \n\"\"\",\n    # marker\n    \"second\"  # tail\n]\n"
	cases: list[tuple[str, bool]] = [
		(values_source, True),
		(addition_source, True),
		(wrapped_addition_source, True),
		(f"if {logic}:\n    result = 1\n", True),
		("values = {" + ", ".join(f"\"key_{i}\": value_{i}" for i in range(20)) + "}\n", True),
		(f"def f({items}):\n    return call({items})\n", True),
		(f"def f():\n    return {items}\n", True),
		(f"result = {items}\n", True),
		(f"a = b = {addition}\n", True),
		(f"assert {logic}, message\n", True),
		(f"f = lambda x: {addition}\n", True),
		(f"x = {addition} if flag else alternate\n", True),
		("result = root" + "".join(f".property_{i}" for i in range(20)) + "\n", True),
		("value = [x * x for x in range(12345) if x % 2]\n", True),
		("value = sum(x * x for x in range(12345))\n", True),
		("value = f\"{(123456789012345678901234567890 + 1)=}\"\n", False),
		("value = \"abcdefghijklmnop\" \"qrstuvwxyz\"\n", True),
		("value = rb\"a\\b\" b\"c\"\n", True),
		("value = (\"\"\"A\n    B  \n\"\"\" \"C\")\n", True),
		(multiline_container, True),
		("if (value := len(\"abcdefghijklmnop\")) > 1:\n    result = value\n", True),
		("def f():\n\tif True:\n\t\treturn \"abcdefghijklmnop\" + \"qrstuvwxyz\"\n", True),
		(f"def f():\n    yield {addition}\n", True),
		(f"async def f():\n    return await call({items})\n", True),
		("def f(xs):\n    return 1, *xs\n", True),
		("value = data[1, 2, 3, 4, 5, 6, 7, 8]\n", True),
		("value = " + " // ".join(f"var_{i}" for i in range(15)) + "\n", True),
		("value = alpha is not beta and gamma not in delta\n", True),
		(future_annotation, True),
		("Result = F(A) + LongNameOne + LongNameTwo + G(B)\n", True),
		(f"def f():\n    raise {addition} from cause\n", True),
		(f"def f():\n    with {addition} as value:\n        pass\n", True),
		(items + "\n", True),
		("not " + " == ".join(f"value_{i}" for i in range(24)) + "\n", True),
		("root" + "".join(f".property_{i}" for i in range(20)) + "()\n", True),
		(inline_container, True),
		(commented_container, True),
		(f"match {addition}:\n    case _ if {logic}:\n        pass\n", True),
		("try:\n    pass\nexcept root" + "".join(f".error_{i}" for i in range(20)) + " as err:\n    pass\n", True)
	]
	# 注釈と継続括弧の検体の追加
	bare_annotation: str = f"x: {addition}\n"
	cases.append((bare_annotation, True))
	cases.append((f"x: {addition} = None\n", True))
	cases.append((f"x: {addition} = {addition}\n", True))
	cases.append((f"def f() -> {addition}:\n    pass\n", True))
	generic_tuple: str = (
		"class Meta(type):\n"
		"    def __getitem__(cls, item):\n"
		"        return item\n"
		"class Receiver(metaclass=Meta): pass\n"
		"x: Receiver[int,]\n"
		"print(type(__annotations__[\"x\"]).__name__)\n"
	)
	type_alias: str = "type Alias[T,] = list[T]\ntype PairAlias = Receiver[int, str,]\ntype TupleAlias = Receiver[int,]\n"
	cases.extend(((generic_tuple, True), (type_alias, True)))
	cases.append(("value = [\n    1, # first\n    \"\"\"A\nB\"\"\"\n]\n", True))
	golden: Path = ROOT / "test_data/golden/python/paren_continuation.out"
	ordered_source: str = golden.with_suffix(".in").read_text()
	unparsed_source: str = ast.unparse(ast.parse(ordered_source)) + "\n"
	cases.extend(((ordered_source, True), (unparsed_source, True)))
	all_width_sources: set[str] = {
		values_source,
		addition_source,
		wrapped_addition_source,
		multiline_container,
		inline_container,
		commented_container,
		ordered_source,
		unparsed_source
	}
	# 幅毎の出力と中核出力の記録
	outputs: dict[tuple[str, str], bytes] = {}
	stripped: dict[tuple[str, str], bytes] = {}
	# 幅の全組合せは改行・コメントの配置差と幅無制限の期待値に限る
	# 構文と冪等性と行幅の照合
	for index, (source, bounded) in enumerate(cases):
		original_tree: str = python_tree(source, include_attributes=False)
		for width in ("0", "128", "20") if source in all_width_sources else ("20",):
			command: list[str] = [str(binary), "--stdin", "--lang", "python", "--chars", width, "--no-lint", "--fail-on-skip"]
			first: bytes = run_process(command, input=source.encode(), capture_output=True, check=True).stdout
			compile(first, "<formatted>", "exec")
			require(python_tree(first, include_attributes=False) == original_tree, (index, width, "AST changed", first))
			require(
				run_process(command, input=first, capture_output=True, check=True).stdout == first,
				(index, width, "not a fixed point", first)
			)
			direct: bytes = core(command, source.encode())
			require(core(command, first) == direct)
			require(core(command, direct) == direct)
			require(python_tree(direct, include_attributes=False) == original_tree)
			if bounded and width != "0":
				lines: list[str] = direct.decode().splitlines()
				if source == future_annotation:
					lines = [line for line in lines if line != "from __future__ import annotations"]
				require(all(len(line.lstrip()) <= int(width) for line in lines), (index, width, direct))
			outputs[source, width] = first
			stripped[source, width] = direct
	# 原文の改行とコメントに依らない出力の照合
	for width in "0", "128", "20":
		require(
			outputs[addition_source, width] == outputs[wrapped_addition_source, width],
			(width, "original line breaks changed output")
		)
		require(
			outputs[multiline_container, width] == outputs[inline_container, width],
			(width, "multiline string container depends on line breaks")
		)
		require(
			stripped[inline_container, width] == stripped[commented_container, width],
			(width, "comments changed multiline string container")
		)
	# 注釈の評価値と型引数の保持確認
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
	compact_alias: bytes = re.sub(rb"\s+", b"", outputs[type_alias, width])
	require(b"Alias[T]" in compact_alias and b"Alias[T,]" not in compact_alias, (width, "type parameter trailing comma remained"))
	require(
		b"Receiver[int,str]" in compact_alias and b"Receiver[int,str,]" not in compact_alias,
		(width, "multi-argument trailing comma remained")
	)
	require(b"Receiver[int,]" in compact_alias, (width, "tuple type argument comma was removed"))
	require(b"x: (\n" in outputs[bare_annotation, "20"], "bare annotation was not wrapped")
	for width in "0", "128", "20":
		require(outputs[ordered_source, width] == outputs[unparsed_source, width], "continuation parentheses changed split order")
	require(outputs[ordered_source, "128"] == golden.read_bytes(), "post-order split differs from expected output")
	require(b"values = [value_0," in outputs[values_source, "0"], "unlimited width kept redundant line breaks")
	print(f"python wrapping: {len(cases)} cases, targeted widths, AST, syntax and fixed points OK")
	# 終了
	return

def verify_python_annotations(binary: Path) -> None:
	"""型注釈と実体型を区別して括弧を保持する事を検査する
	計算量：検体数 F・整形器と解釈器の最大実行時間 T に対して O(FT)
	引数：
		binary: 整形器の実行ファイル
	"""
	# 型注釈と実体型の検体の構築
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
		("value: tuple[bool, ...] = (1, 0, True, False)\n", "tuple (1, 0, True, False) False False\n"),
		("# marker\nvalue: bool = 1 # tail\n", "int 1 False False\n")
	]
	observer: str = "\nprint(type(value).__name__, str(value), value is True, value is False)\n"
	# 原文と整形結果の構文と実行結果の照合
	observed: dict[bytes, str] = {}
	stripped: dict[tuple[int, str], bytes] = {}
	for index, (source, expected) in enumerate(cases):
		original_tree: str = python_tree(source)
		outputs: list[bytes] = [source.encode()]
		width: str = "20"
		command: list[str] = [str(binary), "--stdin", "--lang", "python", "--chars", width, "--no-lint", "--fail-on-skip"]
		first: bytes = run_process(command, input=source.encode(), capture_output=True, check=True).stdout
		direct: bytes = core(command, source.encode())
		require(core(command, first) == direct)
		stripped[index, width] = direct
		outputs.extend((first, direct))
		for output in outputs:
			require(python_tree(output) == original_tree, (index, "annotation AST changed", output))
			if output not in observed:
				completed: subprocess.CompletedProcess[bytes] = run_process(
					[sys.executable, "-I", "-"],
					input=output + observer.encode(),
					capture_output=True,
					check=True,
					timeout=10
				)
				observed[output] = completed.stdout.decode().replace("\r\n", "\n")
			require(observed[output] == expected, (index, "Python value or type changed", observed[output], expected))
	width: str = "20"
	require(stripped[0, width] == stripped[len(cases) - 1, width], "comments changed annotation")
	print(f"python annotations: {len(cases)} cases, narrow wrapping, native values, types, AST OK")
	# 終了
	return

def verify_javascript_comparisons(binary: Path) -> None:
	"""JS/TS の typeof 比較が結果を保つ事を検査する
	計算量：検体数 F・整形器と解釈器の最大実行時間 T に対して O(FT)
	引数：
		binary: 整形器の実行ファイル
	"""
	# 比較演算の検体の構築
	cases: list[tuple[str, str, str]] = []
	for operator, result in ("==", "true"), ("!=", "false"):
		cases += [
			(
				f"const x = 1, y = 2; console.log({expression});",
				result,
				re.sub(r"\s+", "", expression.replace(operator, operator + "="))
			) for expression in (f"typeof x {operator} \"number\"", f"\"number\" {operator} typeof x", f"typeof x {operator} typeof y")
		]
		cases += [
			(f"const x = 1, kind = {value}; console.log(typeof x {operator} kind);", result, f"typeofx{operator}kind") for value in
			("new String(\"number\")", "{toString() { return \"number\"; }}")
		]
	# 同じ元入力へ付けたコメントの有無でも，コメント除去後のコードが一致する事を確認する
	cases.append((cases[0][0].replace("typeof x", "typeof /* marker */ x"), cases[0][1], cases[0][2]))
	# 各言語経路の整形と中核出力の照合
	sources: list[str] = [source for source, _, _ in cases]
	expected: list[str] = [result for _, result, _ in cases]
	labels: list[tuple[object, ...]] = [(index, "original") for index, _ in enumerate(cases)]
	stripped: dict[tuple[int, str, str], bytes] = {}
	for index, (source, result, required) in enumerate(cases):
		# javascriptreact は javascript と同じ文法・言語の別名の為，文法の異なる３経路で見る
		for lang in "javascript", "typescript", "typescriptreact":
			width: str = "20"
			command: list[str] = [str(binary), "--stdin", "--lang", lang, "--chars", width, "--no-lint", "--fail-on-skip"]
			first: bytes = run_process(command, input=source.encode(), capture_output=True, check=True).stdout
			direct: bytes = core(command, source.encode())
			require(core(command, first) == direct)
			for output in first, direct:
				code: str = output.decode().replace("/* marker */", "")
				require(required in re.sub(r"\s+", "", code), (index, lang, width, "comparison rule changed", output))
			stripped[index, lang, width] = direct
			sources.extend((first.decode(), direct.decode()))
			expected.extend((result, result))
			labels.extend(((index, lang, width, "normal"), (index, lang, width, "comment-free")))
	for lang in "javascript", "typescript", "typescriptreact":
		width: str = "20"
		require(stripped[0, lang, width] == stripped[len(cases) - 1, lang, width], "comments changed comparison")
	# 検体毎に独立した Node プロセスで比較結果を確認する
	# 処理系での比較結果の照合
	actual: dict[str, str] = {}
	for label, source, wanted in zip(labels, sources, expected):
		if source not in actual:
			completed: subprocess.CompletedProcess[bytes] = run_process(
				["node", "-"],
				input=source.encode(),
				capture_output=True,
				check=True,
				timeout=10
			)
			actual[source] = completed.stdout.decode().rstrip("\r\n")
		require(actual[source] == wanted, (label, "native comparison result changed", actual[source], wanted))
	print(f"javascript comparisons: {len(cases)} cases, 3 language routes, narrow wrapping, native results OK")
	# 終了
	return

def merge_text(value: object) -> object:
	"""JSX の隣接する文字列の子を連結する
	計算量：値の節点数 N と文字列の総文字数 B に対して O(N+B)
	引数：
		value: JSX の実行結果
	戻値：
		文字列の子を連結した値
	"""
	if isinstance(value, list):
		# 各要素を連結した列の返戻
		return [merge_text(item) for item in value]
	if not isinstance(value, dict):
		# 要素でも列でもない値の返戻
		return value
	# JSX の文字列の子の結合
	merged: dict[object, object] = { key: merge_text(item) for key, item in value.items() if key != "children" }
	if "children" in value:
		children: list[object] = []
		for child in merge_text(value["children"]):
			if isinstance(child, str) and children and isinstance(children[-1], str):
				children[-1] += child
			else:
				children.append(child)
		merged["children"] = children
	# 文字列の子を連結した要素の返戻
	return merged

def verify_jsx_comments(binary: Path) -> None:
	"""JSX のコメントと描画する子の並びを保持する事を検査する
	計算量：検体数 F・整形器と解釈器の最大実行時間 T に対して O(FT)
	引数：
		binary: 整形器の実行ファイル
	"""
	# JSX のコメントと子要素の検体の構築
	expressions: list[str] = [
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
	sources: list[str] = [f"const value = {expression};\n" for expression in expressions]
	comparisons: list[tuple[int, int, str, str]] = []
	for index, source in enumerate(sources.copy()):
		for lang in "javascriptreact", "typescriptreact":
			width: str = "20"
			command: list[str] = [str(binary), "--stdin", "--lang", lang, "--chars", width, "--no-lint", "--fail-on-skip"]
			first: bytes = run_process(command, input=source.encode(), capture_output=True, check=True).stdout
			direct: bytes = core(command, source.encode())
			require(core(command, first) == direct)
			for marker in "jsxMarkerA", "jsxMarkerB":
				require(first.decode().count(marker) == source.count(marker), (index, lang, width, "comment lost or duplicated", first))
				require(marker not in direct.decode(), (index, lang, width, "comment removal failed", direct))
			for output in first, direct:
				comparisons.append((index, len(sources), lang, width))
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
	print(f"jsx comments: {len(expressions)} cases, 2 language routes, narrow wrapping, native children OK")
	# 終了
	return

def verify_control_bodies(binary: Path) -> None:
	"""宣言本体と幅超過時の波括弧が元の配置・コメントに依存しない事を検査する
	計算量：検体数 F・整形器と解釈器の最大実行時間 T に対して O(FT)
	引数：
		binary: 整形器の実行ファイル
	"""
	# 制御節と本体の検体の構築
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
		# 条件を割る見出は閉じ括弧の行へ本体を続け，其の行も行幅を超える時だけ本体を波括弧で次行へ送る
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
		# タグ付文字列の枝による実行結果の照合
		for branch in "else if(!c)", "if(!c)":
			original: bytes = (
				("function tag(x) { return x; } function f(c) { if(c) return tag`x`; " + branch + " return tag`x`; return null; }\n").encode
			)()
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
	results: list[str] = json.loads(
		run_process(["node", "-e", script], input=json.dumps(sources), text=True, capture_output=True, check=True).stdout
	)
	require(len(results) == len(sources))
	for original, output, lang, clause, width in comparisons:
		require(results[original] == results[output], (lang, clause, width, "control body result changed"))
	print("control bodies: 6 clauses, 2 languages, narrow wrapping, native results and layout invariance OK")
	# 終了
	return

def main() -> int:
	"""実行ファイルを指定して通常経路と原文保持を検査する
	計算量：検体数 F・外部処理の最大実行時間 T に対して O(FT)
	戻値：
		終了コード
	"""
	# 個別検査の実行
	binary: Path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/shavefmt"
	verify_python_wrapping(binary)
	verify_python_annotations(binary)
	verify_javascript_comparisons(binary)
	verify_jsx_comments(binary)
	verify_control_bodies(binary)
	# 各言語の通常経路と二巡目の照合
	for lang, source in CASES.items():
		width: str = "20"
		command: list[str] = [str(binary), "--stdin", "--lang", lang, "--chars", width, "--no-lint", "--fail-on-skip"]
		original: bytes = source.encode()
		first: bytes = run_process(command, input=original, capture_output=True, check=True).stdout
		second: bytes = run_process(command, input=first, capture_output=True, check=True).stdout
		require(first and first == second, (lang, width, "not a fixed point"))
		require(b"\r\n" not in first, (lang, "stdout changed LF"))
		if lang == "javascript":
			require(b"a\x1ab" in first, "stdin was truncated at control-Z")
		direct: bytes = core(command, original)
		roundtrip: bytes = core(command, first)
		require(direct == roundtrip, (lang, width, "comments changed code"))
	# コメントの除去・閉じ括弧の同行化・HTML の終端処理で本文や字句を壊さない
	preserved: list[tuple[str, str, str]] = [
		("cpp", "int F(int a,int b){return a+/*c*/++b;}\n", "a\\s*\\+\\s+\\+\\+b"),
		("javascript", "const s = `}\nelse text`; console.log(s);\n", "`}\\nelse text`"),
		("javascript", "const s = `}\nwhile(x);`; console.log(s);\n", "`}\\nwhile\\(x\\);`"),
		("javascript", "function F(){return/*\n*/1;}\n", "return;\\s+1;"),
		("javascript", "function F(){return/*\r*/1;}\n", "return;\\s+1;"),
		("html", "<img src=/assets/>\n", "src=/assets/>")
	]
	for lang, source, pattern in preserved:
		command: list[str] = [str(binary), "--stdin", "--lang", lang, "--no-lint", "--fail-on-skip"]
		first: bytes = core(command, source.encode())
		require(re.search(pattern, first.decode()), (lang, "content or token changed", first))
		require(core(command, first) == first)
	# 行幅の超過で本体を包む制御構文の深いネストも，幅の区間和で測る為に巡の手間の上限の内で整形し，冗長な塊のネストは１巡で外す
	# 同じ位置で終わるネストの本体の後の空行も，本体毎に読み直さない
	# 誤り回復は入力の長さの二乗で膨らむ為，数 KB の閉じないコメントの記号の並びは期限で打ち切る
	for case_name, lang, source, reasons in (
		("unmatched comment closers", "c", "*/\n" * 4000, (b"parsing exceeds the time limit", b"parser could not read the source")),
		("deep unbraced controls", "c", "void f(int a) {\n" + "if(a) " * 3200 + "x();\n}\n", b""),
		("deep unbraced controls with blank tail", "c", "void f(int a) {\n" + "if (a) " * 4000 + "x();\n" + "\n" * 8000 + "}\n", b""),
		("deep JavaScript blocks", "javascript", "function f(a) {\n" + "if (a) { " * 4000 + "x();" + " }" * 4000 + "\n}\n", b""),
		("deep C blocks", "c", "void f(int a) {\nif (a) " + "{ " * 4000 + "x();" + " }" * 4000 + " else { y(); }\n}\n", b"")
	):
		result: subprocess.CompletedProcess[bytes] = run_process(
			[str(binary), "--stdin", "--lang", lang, "--no-lint"],
			input=source.encode(),
			capture_output=True,
			timeout=30
		)
		if isinstance(reasons, bytes):
			reasons = reasons,
		require(
			any(reason in result.stderr for reason in reasons) if reasons else b"[skip]" not in result.stderr,
			(case_name, lang, result.stderr[:200])
		)
	# 開いた要素の名前を解析器が失い閉じタグを読み違えた HTML だけを解析器の制限の理由で見送る（整形仕様 9.3）
	# 解析器の手間が膨らむ其の他の深いネストは，解析１回毎の期限で打ち切るか上限の内に整形を終える（整形仕様 9.1）
	for source, skipped in (
		("<li><p>" * 5000 + "x", False),
		("<div a=/>" * 10000 + "</div>" * 10000, True),
		("<html>" + "<div>" * 5000 + "&amp;" * 50000 + "</html>", False),
		("<div>" + "<my-element>" * 92 + "<li><p>x</p></li></div>", False),
		("<my-element>" * 85 + "x" + "</my-element>" * 85, True),
		("<div>" * 1100 + "x" + "</div>" * 1100, True),
		("\r" + "<li><p>" * 5000 + "x", False)
	):
		result: subprocess.CompletedProcess[bytes] = run_process(
			[str(binary), "--stdin", "--lang", "html"],
			input=source.encode(),
			capture_output=True,
			timeout=60
		)
		require(result.returncode == 0, (source[:20], result.stderr[:200]))
		require((b"nesting exceeds parser limit" in result.stderr) == skipped, (source[:20], result.stderr[:200]))
	# 過去に超線形時間を要した入力群も上限内で終わる事の検証
	else_chain: str = (
		"func f(a: Int) {\n\tif a == 0 {\n\t\t// c\n\t}" + "".join(f" else if a == {i} {{\n\t\t// c\n\t}}" for i in range(1, 1000)) +
		"\n}\n"
	)
	for lang, source in (
		("cpp", "// " + "日[" * 20000 + "\nint x;\n"),
		("scss", "a {\n" * 200 + "@mixin m() {}\n" * 300 + "}\n" * 200),
		("swift", else_chain),
		("swift", "let y = " + "x!+" * 6000 + "1\n"),
		("swift", "let v = a" + "".join(" ?? b%d * c%d << d%d" % (index, index, index) for index in range(3000)) + "\n"),
		("typescript", "x = new a!." + ".".join("m%d" % index for index in range(6400)) + "();\n"),
		("javascript", ("a" + "(" * 4000 + " // c\n1" + ")" * 4000 + ";\n") * 5),
		("javascript", "\n".join("{ // note%d" % index for index in range(1000)) + "\nx;\n" + "}\n" * 1000),
		("css", ("a:not(" * 600 + "b /* c */" + ")" * 600 + " {}\n") * 20),
		("html", "<a x=\"1\" y=\"" * 3000),
		("html", "<!--" * 4000),
		("c", "".join("int f%d(int a) { return (A) / a; }\n" % index for index in range(4000))),
		("go", "package p\n\nfunc ExampleX() {\n\tx()\n" + "".join("\t// line %d\n" % index for index in range(20000)) + "}\n"),
		("cpp", "// " + "あ。" * 40000 + "\nint x;\n"),
		("python", "#x\n" * 40000 + "x = 1\n"),
		("c", "void f(int x) { switch(x) { case 1:\n" + "a();\n" * 10000 + "// c\n" * 10000 + "} }\n"),
		("kotlin", "fun f() {\n" + "a()\n" * 10000 + "// c\n" * 10000 + "}\n"),
		("go", "package p\n\nfunc f() {\n" + "a()\n" * 10000 + "// c\n" * 10000 + "}\n"),
		("javascript", "".join("x%d = /a*/ // c\n" % index for index in range(20000))),
		("javascript", "f(function () {\nx();\n" + "// eslint-disable-next-line\n" * 20000 + "}, y);\n"),
		("javascript", "function f() {\nx();\n" + ("// " + "c" * 400 + "\n") * 10000 + "}\n"),
		("javascript", "/*a*/ " * 32000 + "\nx();\n"),
		("c", "void f() {\nx();\n" + "// c\n" * 20000 + " " * 20000 + "y();\n}\n"),
		("c", "void f() {\nx();\n" + "/* NOLINTNEXTLINE */ " * 100000 + "\n}\n"),
		("c", "".join("#define T%d T%d\n" % (index, index - 1) for index in range(4000, 0, -1)) + "#define T0 a + b\nint x = T4000;\n"),
		("cpp", "void g() { h(" + "f<N + 1>(" * 800 + "0" + ")" * 800 + "); }\n"),
		(
			"kotlin",
			"fun f(a: Int) {\n\tvar x = 0\n\tif (a == 0) { x = 0 } " +
			"".join("else if (a == %d) { x = %d } " % (index, index) for index in range(1, 1600)) + "else { x = 1 }\n}\n"
		),
		(
			"kotlin",
			"fun f(a: Int): Int {\n\tval y = if (a == 0) 0 " +
			"".join("else if (a == %d) %d " % (index, index) for index in range(1, 3200)) + "else 0\n\treturn y\n}\n"
		),
		("ruby", "x = [" + ", ".join(["if a then 1 else 2 end"] * 8000) + "]\n"),
		(
			"ruby",
			"def f(a)\n  if a == 0\n    0\n" + "".join("  elsif a == %d\n    %d\n" % (index, index) for index in range(1, 8000)) +
			"  end\nend\n"
		),
		("ruby", "x = " + "if a then " * 800 + "1" + " else 2 end" * 800 + "\n"),
		("csharp", "class K {\n\tint " + ", ".join("A%d = F(/* c */ %d)" % (index, index) for index in range(4000)) + ";\n}\n"),
		(
			"javascript",
			"var " + ", ".join("a%d = f(/* c */ %d, function() { /* d */ return 1; })" % (index, index) for index in range(4000)) + ";\n"
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
			"#define T a + b\n" + "".join("#define U%d T\n" % index for index in range(8000)) + "#define BIG (" +
			" ".join("U%d" % index for index in range(8000)) + ")\nint x = BIG;\n"
		)
	):
		# 時間だけを見ると異常終了も合格に為る為，終了状態も確かめる（意図して見送る検体は上の表で理由を照合して居る）
		result: subprocess.CompletedProcess[bytes] = run_process(
			[str(binary), "--stdin", "--lang", lang, "--no-lint"],
			input=source.encode(),
			capture_output=True,
			timeout=10
		)
		require(not result.returncode, (lang, result.returncode, result.stderr[:200]))
	# 静的検査自身も時間の上限の内に終わる（他の検査は `--no-lint` で整形の側だけを見る為，検査の走査の膨らみは此処でしか現れない）
	# ドキュメントコメントの並びを基準点から一つずつ遡ると，遡りが毎回根から降り直す為に並びの長さの二乗に膨らむ
	for lang, source in (
		("ruby", "# comment line\n" * 32000 + "def two\n\treturn 7\nend\n"),
		("ruby", "".join("# c\n" * 8 + "def f%d\n\treturn 1\nend\n" % index for index in range(4000)))
	):
		# 時間だけを見ると，異常終了や即時の見送りも合格に為る為，終了状態と検査の実施も確かめる
		result: subprocess.CompletedProcess[bytes] = run_process(
			[str(binary), "--stdin", "--lang", lang, "--fail-on-skip"],
			input=source.encode(),
			capture_output=True,
			timeout=10,
			check=True
		)
		require(b"[skip]" not in result.stderr, result.stderr)
		require(b"[warn]" in result.stderr, "the static checker did not run")
	# 貯留の返却漏れは出力も終了状態も変えない為，常駐量でしか気付けない（走査カーソルの漏れは入力の千倍に達した事が有る）
	# `RUSAGE_CHILDREN` は同じ処理が起こした全ての子の最大値の為，計測用の子を１つ起こして其の中で測る
	if os.name == "posix":
		source: str = "class C { int F(int x) { if (x > 0) { return x + 1; } return 0; } }\n" * 4000
		probe: str = (
			"import resource, subprocess, sys\n"
			"data = sys.stdin.buffer.read()\n"
			"subprocess.run(sys.argv[1:], input=data, capture_output=True, check=True, timeout=45)\n"
			"print(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)\n"
		)
		measured: subprocess.CompletedProcess[bytes] = run_process(
			[sys.executable, "-c", probe, str(binary), "--stdin", "--lang", "java", "--no-lint", "--fail-on-skip"],
			input=source.encode(),
			capture_output=True,
			timeout=60,
			check=True
		)
		# macOS はバイト，Linux はキビバイトで返す
		PEAK: int = int(measured.stdout) if sys.platform == "darwin" else int(measured.stdout) << 10
		# 検証は期待構文と出力の構文の節点表を同時に持つ為（整形仕様 8.1），健全な整形も入力の三百数十倍（arm64 の macOS は五百数十倍）に為る
		# 入力に比例する分が線形で漏れ（入力の千倍）と区別出来る様，基礎量へ入力の７６８倍を足した上限で見る
		LIMIT: int = len(source) * 768 + 0X1000000
		require(PEAK < LIMIT, ("peak memory grew far beyond the input", PEAK, LIMIT, len(source)))
		print(f"peak memory: {PEAK >> 20} MiB for {len(source) >> 10} KiB of input")
	# Swift の演算子へ密着したコメントは１つ毎に差分で読み直し，再利用した区画を読取量に数えない為，大きな原稿の末尾だけを直す入力は上限の内に整形する（整形仕様 9.3）
	glued: str = "".join("let v%d = 1\n" % index for index in range(20000)) + "let b = y" + " +/* c */1" * 40 + "\n"
	result: subprocess.CompletedProcess[bytes] = run_process(
		[str(binary), "--stdin", "--lang", "swift", "--no-lint"],
		input=glued.encode(),
		capture_output=True,
		timeout=30
	)
	require(
		result.returncode == 0 and b"[skip]" not in result.stderr and result.stdout.count(b"/* c */") == 40,
		result.stderr[:200]
	)
	# 解析器の手間がコメントの並びの長さの二乗に膨らむ入力も，解析１回毎の期限で打ち切るか上限の内に整形を終える（整形仕様 9.1, 9.3）
	for lang, source in (
		("python", "def f():\n" + "    # c\n" * 8000 + "    return 1\n"),
		("javascript", "a = foo\n" + "// c\n" * 16000 + ".bar()\n"),
		("typescript", "a = foo\n" + "/**/ " * 16000 + "\n.bar()\n"),
		("kotlin", "fun f() {\nval a = foo\n" + "// c\n" * 16000 + ".bar()\n}\n"),
		("php", "<?php\n$a = $foo\n" + "// c\n" * 16000 + "->bar();\n")
	):
		result: subprocess.CompletedProcess[bytes] = run_process(
			[str(binary), "--stdin", "--lang", lang, "--no-lint"],
			input=source.encode(),
			capture_output=True,
			timeout=60
		)
		require(
			result.returncode == 0 and (b"[skip]" not in result.stderr or b"parsing exceeds the time limit" in result.stderr),
			(lang, result.stderr[:200])
		)
	for lang, source in (
		("javascript", "x = 1; /*\n" + "// old line: doSomething(a, b, c);\n" * 4000 + "*/\ny = 2;\n"),
		("php", "<?php\n$x = 1; /*\n" + "# old line: doSomething($a);\n" * 4000 + "*/\n$y = 2;\n")
	):
		result: subprocess.CompletedProcess[bytes] = run_process(
			[str(binary), "--stdin", "--lang", lang, "--no-lint"],
			input=source.encode(),
			capture_output=True,
			timeout=10
		)
		require(b"[skip]" not in result.stderr, (lang, result.stderr[:200]))
	result: subprocess.CompletedProcess[bytes] = run_process(
		[str(binary), "--stdin", "--lang", "swift", "--no-lint"],
		input="".join("//* note %d\nlet v%d = %d\n" % (index, index, index) for index in range(1000)).encode(),
		capture_output=True,
		timeout=10
	)
	require(b"comments glued to operators exceed parser limit" in result.stderr, result.stderr[:200])
	# 不正入力の原文保持の確認
	guarded: bytes = b"\xef\xbb\xbf{\x00}\r\n"
	result: subprocess.CompletedProcess[bytes] = run_process(
		[str(binary), "--stdin", "--lang", "json", "--fail-on-skip"],
		input=guarded,
		capture_output=True
	)
	require(result.returncode == 1 and result.stdout == guarded and b"[skip]" in result.stderr)
	# ファイル書戻経路の検査
	with tempfile.TemporaryDirectory(prefix="shaveformat-runtime-") as temporary:
		# macOS の一時領域は記号リンクの `/var` を経る為，明示指定でもリンクを辿らない整形器へ実体の経路を渡す
		directory: Path = Path(temporary).resolve()
		path: Path = directory / "sample.json"
		path.write_bytes(b"\xef\xbb\xbf{ \"x\" : 1 }\r\n")
		run_process([str(binary), "--write", str(path)], capture_output=True, check=True)
		result: bytes = path.read_bytes()
		# 成功した出力は BOM 無・LF とする（整形仕様 10.3）
		require(not result.startswith(b"\xef\xbb\xbf") and b"\r" not in result, result)
		require(list(directory.iterdir()) == [path], "temporary file was left behind")
		# 末尾区切文字は経路の表記に過ぎない為，通常ディレクトリを記号リンクと誤認しない
		scan_directory: Path = directory / "scan"
		scan_directory.mkdir()
		(scan_directory / "formatted.c").write_bytes(b"int x;\n")
		scan_result: subprocess.CompletedProcess[bytes] = run_process(
			[str(binary), "--check", "--fail-on-skip", f"{scan_directory}{os.sep}"],
			capture_output=True
		)
		require(not scan_result.returncode and b"symbolic link(s) not followed" not in scan_result.stderr, scan_result)
		(scan_directory / "formatted.c").unlink()
		scan_directory.rmdir()
		header: Path = directory / "ambiguous.h"
		header.write_bytes(b"template <typename T> void Set(T Value) { value_ = Value; }\n")
		header_result: subprocess.CompletedProcess[bytes] = run_process([str(binary), "--write", str(header)], capture_output=True)
		require(not header_result.returncode and b"parser could not read the source" not in header_result.stderr.lower())
		expected_header: (
			bytes
		) = b"/**\n *\n * @param Value\n */\ntemplate<typename T> void Set(T Value) {\n\tvalue_ = Value;\n\treturn;\n}\n"
		require(header.read_bytes() == expected_header, "C++ header detection did not preserve the complete definition")
		header.unlink()
		# 拡張子から定まらず `--lang` で仮定した言語で読めない所の有る入力は，取違に依る内容の破壊を防ぐ為に書き換えず見送る（整形仕様 10.4）
		assumed: Path = directory / "assumed"
		assumed_source: bytes = b"function f(body: T) {\nreturn   body;\n}\n"
		assumed.write_bytes(assumed_source)
		assumed_result: subprocess.CompletedProcess[bytes] = run_process(
			[str(binary), "--lang", "javascript", "--write", str(assumed)],
			capture_output=True
		)
		require(not assumed_result.returncode and b"--lang does not match the file content" in assumed_result.stderr, assumed_result)
		require(assumed.read_bytes() == assumed_source, "a file read with an assumed language was rewritten")
		assumed.unlink()
		# ハードリンク切断時の警告と要約件数の検査
		original: bytes = b"{ \"x\" : 1 }\n"
		path.write_bytes(original)
		alias: Path = directory / "hard-link.json"
		os.link(path, alias)
		hard_link_result: subprocess.CompletedProcess[bytes] = run_process(
			[str(binary), "--write", str(path)],
			capture_output=True,
			check=True
		)
		require(b"hard link was broken" in hard_link_result.stderr, hard_link_result)
		require(hard_link_result.stdout.endswith(b"Done. 1/1 file(s) formatted, 1 warning(s).\n"), hard_link_result)
		require(alias.read_bytes() == original and path.read_bytes() != original, "hard link contents were not separated")
		alias.unlink()
		# 拡張機能は `.h` の文書に `--lang h` を渡し，標準入力でもファイルの `.h` と同じく中身で C / C++ を判別させる
		for source, expected in (
			(b"template <typename T> void Set(T Value) { value_ = Value; }\n", expected_header),
			(b"int init(void);\n", b"int init(void);\n")
		):
			stdin_header: bytes = (
				run_process([str(binary), "--stdin", "--lang", "h", "--no-lint"], input=source, capture_output=True, check=True).stdout
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
				require(list(directory.iterdir()) == [path], "temporary file was left behind")
			finally:
				run_process(["chmod", "-N", str(path)], check=True)
			run_process([str(binary), "--write", str(path)], capture_output=True, check=True)
		# 標準出力への書込失敗の確認
		if os.name == "posix":
			def limit_output() -> None:
				"""原本に触れず，子の標準出力だけを書込不能にする"""
				resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))
				signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
				# 終了
				return
			empty: Path = directory / "empty"
			empty.mkdir()
			for arguments in ["--check", str(path)], ["--help"], ["--version"], [str(empty)]:
				with (directory / "stdout").open("wb") as output:
					failed: subprocess.CompletedProcess[bytes] = run_process(
						[str(binary), *arguments],
						stdout=output,
						stderr=subprocess.PIPE,
						preexec_fn=limit_output
					)
				require(failed.returncode == 1 and b"failed to write stdout" in failed.stderr, arguments)
			# 明示指定の経路もリンクを経れば辿らず（木の外の実体を書き換えない），名指した物を読まなかった事を `--fail-on-skip` で失敗に数える
			# 記号リンク経由の明示経路の拒否確認
			real: Path = directory / "real"
			real.mkdir()
			(real / "sample.c").write_bytes(b"int  x;\n")
			(real / "formatted.c").write_bytes(b"int x;\n")
			(directory / "link").symlink_to(real)
			linked: str = str(directory / "link" / "sample.c")
			for arguments, code in (
				(["--check", str(real / "formatted.c"), linked], 0),
				(["--check", "--fail-on-skip", str(real / "formatted.c"), linked], 1),
				(["--write", "--fail-on-skip", linked], 1)
			):
				result: subprocess.CompletedProcess[bytes] = run_process([str(binary), *arguments], capture_output=True)
				require(result.returncode == code and b"1 symbolic link(s) not followed" in result.stdout, (arguments, result))
			require((real / "sample.c").read_bytes() == b"int  x;\n", "file behind a symbolic link was rewritten")
			# 行先の無いリンクを名指した経路も，不在の経路でなく辿らないリンクとして数える
			(directory / "dangling.c").symlink_to(directory / "missing.c")
			for arguments, code in (
				(["--check", str(real / "formatted.c"), str(directory / "dangling.c")], 0),
				(["--check", "--fail-on-skip", str(real / "formatted.c"), str(directory / "dangling.c")], 1)
			):
				result: subprocess.CompletedProcess[bytes] = run_process([str(binary), *arguments], capture_output=True)
				require(
					result.returncode == code and b"1 symbolic link(s) not followed" in result.stdout and b"path not found" not in result.stderr,
					(arguments, result)
				)
	print(f"runtime: {len(CASES)} languages, byte preservation OK")
	# 全検査を通過した事の返戻
	return 0

if __name__ == "__main__":
	sys.exit(main())
