#!/usr/bin/env python3
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

ROOT = Path(__file__).resolve().parent.parent
# コメントを書き戻さずに整形する環境（コメントの有無でコメント以外の整形結果が変わらない事の検査に用いる）
DROP_COMMENTS_ENV = dict(os.environ, SHAVEFMT_DROP_COMMENTS="1")
COMMAND_TIMEOUT = 120

def run_process(*args: Any, timeout: float = COMMAND_TIMEOUT, **kwargs: Any) -> subprocess.CompletedProcess[Any]:
	"""時間上限付で子処理を実行する"""
	# 実行結果を返戻
	return subprocess.run(*args, timeout=timeout, **kwargs)

def command_output(*args: Any, timeout: float = COMMAND_TIMEOUT, **kwargs: Any) -> Any:
	"""時間上限付で子処理の標準出力を得る"""
	# 標準出力を返戻
	return subprocess.check_output(*args, timeout=timeout, **kwargs)

CASES = {
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

def verify_cpp_declaration_parentheses(binary: Path) -> None:
	"""宣言への再解釈を防ぐ括弧と通常の冗長括弧除去を検査する
	引数：
		binary: 整形器の実行ファイル
	"""
	prefix = (
		"#include <istream>\n#include <iterator>\n#include <string>\n"
		"struct Thing { Thing(int) {} int operator[](int) const { return 1; } }; "
		"struct Holder { Holder(Thing) {} Thing Convert(Thing Value) { return Value; } }; "
		"Thing Convert(Thing Value) { return Value; }\n"
	)
	cases = [
		(
			"void F(std::istream &Input) { std::string Bytes("
			"(std::istreambuf_iterator<char>(Input)), std::istreambuf_iterator<char>()); }",
			"Bytes((std::istreambuf_iterator<char>(Input)),std::istreambuf_iterator<char>());",
			"Bytes((("
		),
		(
			"void F(std::istream &Input) { std::string Bytes("
			"(((/* marker */std::istreambuf_iterator<char>(Input)))), std::istreambuf_iterator<char>()); }",
			"Bytes((std::istreambuf_iterator<char>(Input)),std::istreambuf_iterator<char>());",
			"Bytes((("
		),
		("void F(int X) { (Thing(X)); }", "(Thing(X));", "((Thing(X)))"),
		("void F(int X) { (int(X)); }", "(int(X));", "((int(X)))"),
		("void F(int X) { (((Thing(X)))); }", "(Thing(X));", "((Thing(X)))"),
		("void F(int X, int Y) { (Thing(X) = Y); }", "(Thing(X)=Y);", "((Thing(X)=Y))"),
		("void F(int X) { (Thing(X)[1]); }", "(Thing(X)[1]);", "((Thing(X)[1]))"),
		("void F(int X) { Holder((Thing(X))); }", "Holder((Thing(X)));", "Holder(((Thing(X))))"),
		("void F(int X) { Holder(Convert((Thing(X)))); }", "Holder(Convert((Thing(X))));", "Convert(((Thing(X))))"),
		("void F() { int Value(((1))); }", "Value(1);", "Value(("),
		("void F(int X) { int Value(((X + 1))); }", "Value(X+1);", "Value(("),
		("void F(int X) { Convert(((X + 1))); }", "Convert(X+1);", "Convert(("),
		("Thing F(int X) { return Convert((Thing(X))); }", "returnConvert(Thing(X));", "Convert(("),
		("Thing F(int X) { Thing Value = Convert((Thing(X))); return Value; }", "Value=Convert(Thing(X));", "Convert(("),
		("void F(int X, Holder &Obj) { Obj.Convert((Thing(X))); }", "Obj.Convert(Thing(X));", "Convert((")
	]
	for source, required, forbidden in cases:
		for width in "0", "20", "128":
			command = [str(binary), "--stdin", "--lang", "cpp", "--chars", width, "--no-lint", "--fail-on-skip"]
			original = (prefix + source + "\n").encode()
			first = run_process(command, input=original, capture_output=True, check=True).stdout
			direct = run_process(command, input=original, env=DROP_COMMENTS_ENV, capture_output=True, check=True).stdout
			require(
				run_process(command, input=first, capture_output=True, check=True).stdout == first,
				(source, width, "not a fixed point")
			)
			require(
				run_process(command, input=direct, env=DROP_COMMENTS_ENV, capture_output=True, check=True).stdout == direct,
				(source, width, "comment-free path is not a fixed point")
			)
			roundtrip = run_process(command, input=first, env=DROP_COMMENTS_ENV, capture_output=True, check=True).stdout
			require(direct == roundtrip, (source, width, "comments changed code"))
			# 検体には文字列内のコメント記号が無い為，生成コメントを除いて両設定の初回を照合する
			for output in first, direct:
				code = re.sub(rb"/\*.*?\*/|//[^\n]*", b"", output, flags=re.S)
				compact = re.sub(rb"\s+", b"", code).decode()
				require(required in compact and forbidden not in compact, (source, width, "declaration parentheses changed", output))
	print(f"cpp declarations: {len(cases)} cases, 3 widths, normal and comment-free fixed points OK")
	# 終了
	return

def verify_python_wrapping(binary: Path) -> None:
	"""Python の折返で式・本体・文字列を保持し，各幅で収束する事を検査する
	引数：
		binary: 整形器の実行ファイル
	"""
	items = ", ".join(f"value_{i}" for i in range(24))
	addition = " + ".join(f"value_{i}" for i in range(24))
	logic = " and ".join(f"flag_{i}" for i in range(24))
	values_source = f"def f():\n    values = [{items}]\n    return values\n"
	addition_source = f"total_slots = {addition}\n"
	wrapped_addition_source = f"total_slots = (\n    {addition}\n)\n"
	multiline_container = "value = [\n    \"\"\"A\n    B  \n\"\"\",\n    \"second\",\n]\n"
	future_annotation = f"from __future__ import annotations\nx: {addition}\n"
	inline_container = "value = [\"\"\"A\n    B  \n\"\"\", \"second\"]\n"
	commented_container = "value = [\n    \"\"\"A\n    B  \n\"\"\",\n    # marker\n    \"second\"  # tail\n]\n"
	cases = [
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
	bare_annotation = f"x: {addition}\n"
	cases.append((bare_annotation, True))
	cases.append((f"x: {addition} = None\n", True))
	cases.append((f"x: {addition} = {addition}\n", True))
	cases.append((f"def f() -> {addition}:\n    pass\n", True))
	generic_tuple = (
		"class Meta(type):\n"
		"    def __getitem__(cls, item):\n"
		"        return item\n"
		"class Receiver(metaclass=Meta): pass\n"
		"x: Receiver[int,]\n"
		"print(type(__annotations__[\"x\"]).__name__)\n"
	)
	type_alias = "type Alias[T,] = list[T]\ntype PairAlias = Receiver[int, str,]\ntype TupleAlias = Receiver[int,]\n"
	cases.extend(((generic_tuple, True), (type_alias, True)))
	cases.append(("value = [\n    1, # first\n    \"\"\"A\nB\"\"\"\n]\n", True))
	golden = ROOT / "test_data/golden/python/rule_paren_continuation.out"
	ordered_source = golden.with_suffix(".in").read_text()
	unparsed_source = ast.unparse(ast.parse(ordered_source)) + "\n"
	cases.extend(((ordered_source, True), (unparsed_source, True)))
	all_width_sources = {
		values_source,
		addition_source,
		wrapped_addition_source,
		multiline_container,
		inline_container,
		commented_container,
		ordered_source,
		unparsed_source
	}
	outputs, stripped = {}, {}
	# 幅の全組合せは改行・コメントの配置差と幅無制限の期待値に限る
	for index, (source, bounded) in enumerate(cases):
		original_tree = ast.dump(ast.parse(source), include_attributes=False)
		for width in ("0", "20", "128") if source in all_width_sources else ("20",):
			command = [str(binary), "--stdin", "--lang", "python", "--chars", width, "--no-lint", "--fail-on-skip"]
			first = run_process(command, input=source.encode(), capture_output=True, check=True).stdout
			compile(first, "<formatted>", "exec")
			require(ast.dump(ast.parse(first), include_attributes=False) == original_tree, (index, width, "AST changed", first))
			require(
				run_process(command, input=first, capture_output=True, check=True).stdout == first,
				(index, width, "not a fixed point", first)
			)
			direct = run_process(command, input=source.encode(), env=DROP_COMMENTS_ENV, capture_output=True, check=True).stdout
			require(run_process(command, input=first, env=DROP_COMMENTS_ENV, capture_output=True, check=True).stdout == direct)
			require(run_process(command, input=direct, env=DROP_COMMENTS_ENV, capture_output=True, check=True).stdout == direct)
			require(ast.dump(ast.parse(direct), include_attributes=False) == original_tree)
			if bounded and width != "0":
				lines = direct.decode().splitlines()
				if source == future_annotation:
					lines = [line for line in lines if line != "from __future__ import annotations"]
				require(all(len(line.lstrip()) <= int(width) for line in lines), (index, width, direct))
			outputs[source, width] = first
			stripped[source, width] = direct
	for width in "0", "20", "128":
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
	width = "20"
	future_observer = b"\nprint(__annotations__)\n"
	future_source = future_annotation.encode() + future_observer
	require(
		run_process([sys.executable, "-I", "-"], input=future_source, capture_output=True, check=True).stdout == run_process(
			[sys.executable, "-I", "-"],
			input=outputs[future_annotation, width] + future_observer,
			capture_output=True,
			check=True
		).stdout,
		(width, "deferred annotation changed")
	)
	generic_source = generic_tuple.encode()
	require(
		run_process([sys.executable, "-I", "-"], input=generic_source, capture_output=True, check=True).stdout ==
		run_process([sys.executable, "-I", "-"], input=outputs[generic_tuple, width], capture_output=True, check=True).stdout,
		(width, "annotation tuple argument changed")
	)
	compact_alias = re.sub(rb"\s+", b"", outputs[type_alias, width])
	require(b"Alias[T]" in compact_alias and b"Alias[T,]" not in compact_alias, (width, "type parameter trailing comma remained"))
	require(
		b"Receiver[int,str]" in compact_alias and b"Receiver[int,str,]" not in compact_alias,
		(width, "multi-argument trailing comma remained")
	)
	require(b"Receiver[int,]" in compact_alias, (width, "tuple type argument comma was removed"))
	require(b"x: (\n" in outputs[bare_annotation, "20"], "bare annotation was not wrapped")
	for width in "0", "20", "128":
		require(outputs[ordered_source, width] == outputs[unparsed_source, width], "continuation parentheses changed split order")
	require(outputs[ordered_source, "128"] == golden.read_bytes(), "post-order split differs from expected output")
	require(b"values = [value_0," in outputs[values_source, "0"], "unlimited width kept redundant line breaks")
	print(f"python wrapping: {len(cases)} cases, targeted widths, AST, syntax and fixed points OK")
	# 終了
	return

def verify_python_annotations(binary: Path) -> None:
	"""型注釈と実体型を区別して括弧を保持する事を検査する
	引数：
		binary: 整形器の実行ファイル
	"""
	cases = [
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
	observer = "\nprint(type(value).__name__, str(value), value is True, value is False)\n"
	observed: dict[bytes, str] = {}
	stripped: dict[tuple[int, str], bytes] = {}
	for index, (source, expected) in enumerate(cases):
		original_tree = ast.dump(ast.parse(source))
		outputs = [source.encode()]
		width = "20"
		command = [str(binary), "--stdin", "--lang", "python", "--chars", width, "--no-lint", "--fail-on-skip"]
		first = run_process(command, input=source.encode(), capture_output=True, check=True).stdout
		direct = run_process(command, input=source.encode(), env=DROP_COMMENTS_ENV, capture_output=True, check=True).stdout
		require(run_process(command, input=first, env=DROP_COMMENTS_ENV, capture_output=True, check=True).stdout == direct)
		stripped[index, width] = direct
		outputs.extend((first, direct))
		for output in outputs:
			require(ast.dump(ast.parse(output)) == original_tree, (index, "annotation AST changed", output))
			if output not in observed:
				completed = run_process(
					[sys.executable, "-I", "-"],
					input=output + observer.encode(),
					capture_output=True,
					check=True,
					timeout=10
				)
				observed[output] = completed.stdout.decode().replace("\r\n", "\n")
			require(observed[output] == expected, (index, "Python value or type changed", observed[output], expected))
	width = "20"
	require(stripped[0, width] == stripped[len(cases) - 1, width], "comments changed annotation")
	print(f"python annotations: {len(cases)} cases, narrow wrapping, native values, types, AST OK")
	# 終了
	return

def verify_javascript_comparisons(binary: Path) -> None:
	"""JS/TS の typeof 比較が結果を保つ事を検査する
	引数：
		binary: 整形器の実行ファイル
	"""
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
	sources = [source for source, _, _ in cases]
	expected = [result for _, result, _ in cases]
	labels = [(index, "original") for index, _ in enumerate(cases)]
	stripped: dict[tuple[int, str, str], bytes] = {}
	for index, (source, result, required) in enumerate(cases):
		# javascriptreact は javascript と同じ文法・言語の別名の為，文法の異なる３経路で見る
		for lang in "javascript", "typescript", "typescriptreact":
			width = "20"
			command = [str(binary), "--stdin", "--lang", lang, "--chars", width, "--no-lint", "--fail-on-skip"]
			first = run_process(command, input=source.encode(), capture_output=True, check=True).stdout
			direct = run_process(command, input=source.encode(), env=DROP_COMMENTS_ENV, capture_output=True, check=True).stdout
			require(run_process(command, input=first, env=DROP_COMMENTS_ENV, capture_output=True, check=True).stdout == direct)
			for output in first, direct:
				code = output.decode().replace("/* marker */", "")
				require(required in re.sub(r"\s+", "", code), (index, lang, width, "comparison rule changed", output))
			stripped[index, lang, width] = direct
			sources.extend((first.decode(), direct.decode()))
			expected.extend((result, result))
			labels.extend(((index, lang, width, "normal"), (index, lang, width, "comment-free")))
	for lang in "javascript", "typescript", "typescriptreact":
		width = "20"
		require(stripped[0, lang, width] == stripped[len(cases) - 1, lang, width], "comments changed comparison")
	# 検体毎に独立した Node プロセスで比較結果を確認する
	actual: dict[str, str] = {}
	for label, source, wanted in zip(labels, sources, expected):
		if source not in actual:
			completed = run_process(["node", "-"], input=source.encode(), capture_output=True, check=True, timeout=10)
			actual[source] = completed.stdout.decode().rstrip("\r\n")
		require(actual[source] == wanted, (label, "native comparison result changed", actual[source], wanted))
	print(f"javascript comparisons: {len(cases)} cases, 3 language routes, narrow wrapping, native results OK")
	# 終了
	return

def merge_text(value: object) -> object:
	"""JSX の隣接する文字列の子を連結する
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
	引数：
		binary: 整形器の実行ファイル
	"""
	expressions = [
		"<a>{// jsxMarkerA\n}</a>",
		"<a>{/* jsxMarkerA */}</a>",
		"<a>{// jsxMarkerA\n// jsxMarkerB\n}</a>",
		"<a>{// jsxMarkerA */ tail\n}</a>",
		"<Widget>a{/* jsxMarkerA */}b</Widget>",
		"<Widget>a{}b</Widget>",
		"<pre>a {} b</pre>",
		"<><a/>{/* jsxMarkerA */}<b/></>",
		"<a title={<b>{/* jsxMarkerA */}</b>}>X</a>",
		"<a>{flag && (/* jsxMarkerA */<b/>)}</a>",
		"[`A\nB`, <a>{/* jsxMarkerA */}</a>]",
		"<a title=\"/* literal */\">{/* jsxMarkerA */}/* text */</a>",
		"<a>A {/* jsxMarkerA */} B</a>",
		"<a>{/* jsxMarkerA */ /* jsxMarkerB */}</a>",
		"<a>{/* jsxMarkerA\njsxMarkerB */}</a>",
		"<Widget>a&amp;{/* jsxMarkerA */}b</Widget>",
		"<a>{}</a>",
		"<Widget> {} </Widget>",
		"<Widget>a&nbsp;b{}&#32;c</Widget>",
		"<Widget>a\tb{}\t c</Widget>",
		"<Widget>a&#13;b{}c</Widget>",
		"<Widget>\n a \n\n b \n</Widget>",
		"<Widget>a&#123;b&#125;{}c</Widget>",
		"<Widget>&#32;&#32;{}  x  </Widget>",
		"<Widget> x　{}  y　</Widget>",
		"<Widget>    {}    </Widget>",
		"<> a &amp; {} b </>"
	]
	sources = [f"const value = {expression};\n" for expression in expressions]
	comparisons: list[tuple[int, int, str, str]] = []
	for index, source in enumerate(sources.copy()):
		for lang in "javascriptreact", "typescriptreact":
			width = "20"
			command = [str(binary), "--stdin", "--lang", lang, "--chars", width, "--no-lint", "--fail-on-skip"]
			first = run_process(command, input=source.encode(), capture_output=True, check=True).stdout
			direct = run_process(command, input=source.encode(), env=DROP_COMMENTS_ENV, capture_output=True, check=True).stdout
			require(run_process(command, input=first, env=DROP_COMMENTS_ENV, capture_output=True, check=True).stdout == direct)
			for marker in "jsxMarkerA", "jsxMarkerB":
				require(first.decode().count(marker) == source.count(marker), (index, lang, width, "comment lost or duplicated", first))
				require(marker not in direct.decode(), (index, lang, width, "comment removal failed", direct))
			for output in first, direct:
				comparisons.append((index, len(sources), lang, width))
				sources.append(output.decode())
	# 拡張機能の既存の固定依存を使用し，JSX を実際の関数呼出へ変換して子の形も照合する
	runner = r'''
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
'''
	completed = run_process(
		["node", "-e", runner, str(ROOT / "vscode-extension/node_modules/typescript")],
		input=json.dumps(sources).encode(),
		capture_output=True,
		check=True,
		timeout=120
	)
	results = json.loads(completed.stdout)
	require(len(results) == len(sources), "JSX native results missing")
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
	引数：
		binary: 整形器の実行ファイル
	"""
	clauses = (
		"if(c) BODY",
		"if(c) {} else BODY",
		"while(false) BODY",
		"for(;false;) BODY",
		"do BODY while(false);",
		"if(c) if(c) BODY"
	)
	sources: list[str] = []
	comparisons: list[tuple[int, int, str, str, str]] = []
	for lang in "javascript", "typescript":
		for clause in clauses:
			width = "20"
			command = [str(binary), "--stdin", "--lang", lang, "--chars", width, "--no-lint", "--fail-on-skip"]
			outputs: list[bytes] = []
			for body in "var x = 1;", "{ var x = 1; }", "{ /* marker */ var x = 1; }":
				original = ("function f(c) { " + clause.replace("BODY", body) + " return typeof x; }\n").encode()
				first = run_process(command, input=original, capture_output=True, check=True).stdout
				direct = run_process(command, input=original, env=DROP_COMMENTS_ENV, capture_output=True, check=True).stdout
				require(run_process(command, input=first, env=DROP_COMMENTS_ENV, capture_output=True, check=True).stdout == direct)
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
			original = b"function f(c) { if(c && c && c && c && c && c) " + body + b" return 0; }\n"
			command = [str(binary), "--stdin", "--lang", lang, "--chars", width, "--no-lint", "--fail-on-skip"]
			first = run_process(command, input=original, capture_output=True, check=True).stdout
			require(re.search(required, first), (lang, width, "whole control width ignored", first))
			comparisons.append((len(sources), len(sources) + 1, lang, "condition width", width))
			sources.extend((original.decode(), first.decode()))
		for branch in "else if(!c)", "if(!c)":
			original = (
				"function tag(x) { return x; } function f(c) { if(c) return tag`x`; " + branch + " return tag`x`; return null; }\n"
			).encode()
			width = "20"
			command = [str(binary), "--stdin", "--lang", lang, "--chars", width, "--no-lint", "--fail-on-skip"]
			first = run_process(command, input=original, capture_output=True, check=True).stdout
			comparisons.append((len(sources), len(sources) + 1, lang, "template identity", width))
			sources.extend((original.decode(), first.decode()))
	script = r"""
const fs = require("node:fs"), vm = require("node:vm");
const sources = JSON.parse(fs.readFileSync(0, "utf8"));
process.stdout.write(JSON.stringify(sources.map(source =>
    vm.runInNewContext(source + "\nJSON.stringify([f(true), f(false), f(true) === f(false)])", {}, { timeout: 1000 })
)));
"""
	results = json.loads(
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
	戻値：
		終了コード
	"""
	binary = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/shavefmt"
	verify_cpp_declaration_parentheses(binary)
	verify_python_wrapping(binary)
	verify_python_annotations(binary)
	verify_javascript_comparisons(binary)
	verify_jsx_comments(binary)
	verify_control_bodies(binary)
	for lang, source in CASES.items():
		width = "20"
		command = [str(binary), "--stdin", "--lang", lang, "--chars", width, "--no-lint", "--fail-on-skip"]
		original = source.encode()
		first = run_process(command, input=original, capture_output=True, check=True).stdout
		second = run_process(command, input=first, capture_output=True, check=True).stdout
		require(first and first == second, (lang, width, "not a fixed point"))
		require(b"\r\n" not in first, (lang, "stdout changed LF"))
		if lang == "javascript":
			require(b"a\x1ab" in first, "stdin was truncated at control-Z")
		direct = run_process(command, input=original, env=DROP_COMMENTS_ENV, capture_output=True, check=True).stdout
		roundtrip = run_process(command, input=first, env=DROP_COMMENTS_ENV, capture_output=True, check=True).stdout
		require(direct == roundtrip, (lang, width, "comments changed code"))
	# コメントの除去・閉じ括弧の同行化・HTML の終端処理で本文や字句を壊さない
	preserved = [
		("cpp", "int F(int a,int b){return a+/*c*/++b;}\n", "a\\s*\\+\\s+\\+\\+b"),
		("javascript", "const s = `}\nelse text`; console.log(s);\n", "`}\\nelse text`"),
		("javascript", "const s = `}\nwhile(x);`; console.log(s);\n", "`}\\nwhile\\(x\\);`"),
		("javascript", "function F(){return/*\n*/1;}\n", "return;\\s+1;"),
		("javascript", "function F(){return/*\r*/1;}\n", "return;\\s+1;"),
		("html", "<img src=/assets/>\n", "src=/assets/>")
	]
	for lang, source, pattern in preserved:
		command = [str(binary), "--stdin", "--lang", lang, "--no-lint", "--fail-on-skip"]
		first = run_process(command, input=source.encode(), env=DROP_COMMENTS_ENV, capture_output=True, check=True).stdout
		require(re.search(pattern, first.decode()), (lang, "content or token changed", first))
		require(run_process(command, input=first, env=DROP_COMMENTS_ENV, capture_output=True, check=True).stdout == first)
	# HTML の危険な深いネストだけを解析前に見送る事の検証
	# 行幅の超過で本体を包む制御構文の深いネストは，巡の手間の上限で見送り，冗長な塊のネストは１巡で外す
	# 同じ位置で終わるネストの本体の後の空行も，本体毎に読み直さない
	# 誤り回復は入力の長さの二乗で膨らむ為，数 KB の閉じないコメントの記号の並びは期限で打ち切る
	for case_name, lang, source, reasons in (
		("unmatched comment closers", "c", "*/\n" * 4000, (b"parsing exceeds the time limit", b"parser could not read the source")),
		("deep unbraced controls", "c", "void f(int a) {\n" + "if(a) " * 3200 + "x();\n}\n", b"line wrapping exceeds formatting limit"),
		(
			"deep unbraced controls with blank tail",
			"c",
			"void f(int a) {\n" + "if (a) " * 4000 + "x();\n" + "\n" * 8000 + "}\n",
			b"line wrapping exceeds formatting limit"
		),
		(
			"deep JavaScript blocks",
			"javascript",
			"function f(a) {\n" + "if (a) { " * 4000 + "x();" + " }" * 4000 + "\n}\n",
			b"line wrapping exceeds formatting limit"
		),
		("deep C blocks", "c", "void f(int a) {\nif (a) " + "{ " * 4000 + "x();" + " }" * 4000 + " else { y(); }\n}\n", b"")
	):
		result = run_process(
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
	for source, skipped in (
		("<li><p>" * 5000 + "x", True),
		("<div a=/>" * 10000 + "</div>" * 10000, True),
		("<html>" + "<div>" * 5000 + "&amp;" * 50000 + "</html>", True),
		("<div>" + "<my-element>" * 92 + "<li><p>x</p></li></div>", False),
		("<my-element>" * 85 + "x" + "</my-element>" * 85, True),
		("\r" + "<li><p>" * 5000 + "x", True)
	):
		result = run_process([str(binary), "--stdin", "--lang", "html"], input=source.encode(), capture_output=True, timeout=10)
		require((b"nesting exceeds parser limit" in result.stderr) == skipped, (source[:20], result.stderr))
	# 過去に超線形時間を要した入力群も上限内で終わる事の検証
	else_chain = (
		"func f(a: Int) {\n\tif a == 0 {\n\t\t// c\n\t}" + "".join(f" else if a == {i} {{\n\t\t// c\n\t}}" for i in range(1, 1000)) +
		"\n}\n"
	)
	for lang, source in (
		("cpp", "// " + "日[" * 20000 + "\nint x;\n"),
		("scss", "a {\n" * 200 + "@mixin m() {}\n" * 300 + "}\n" * 200),
		("swift", else_chain),
		("swift", "let y = " + "x!+" * 6000 + "1\n"),
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
		result = run_process(
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
		result = run_process(
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
		source = "class C { int F(int x) { if (x > 0) { return x + 1; } return 0; } }\n" * 4000
		probe = (
			"import resource, subprocess, sys\n"
			"data = sys.stdin.buffer.read()\n"
			"subprocess.run(sys.argv[1:], input=data, capture_output=True, check=True, timeout=45)\n"
			"print(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)\n"
		)
		measured = run_process(
			[sys.executable, "-c", probe, str(binary), "--stdin", "--lang", "java", "--no-lint", "--fail-on-skip"],
			input=source.encode(),
			capture_output=True,
			timeout=60,
			check=True
		)
		# macOS はバイト，Linux はキビバイトで返す
		PEAK = int(measured.stdout) if sys.platform == "darwin" else int(measured.stdout) << 10
		# 健全な整形は概ね入力の百数十倍に収まる為，走脈の領域の分の基礎量へ入力の２５６倍を足した上限で見る
		LIMIT = (len(source) << 8) + 0X1000000
		require(PEAK < LIMIT, ("peak memory grew far beyond the input", PEAK, LIMIT, len(source)))
		print(f"peak memory: {PEAK >> 20} MiB for {len(source) >> 10} KiB of input")
	# Swift の演算子へ密着したコメントは１つ毎に読み直す為，読み直す字の数の和が上限を超える入力は時間の上限の内に見送る
	glued = "".join("let v%d = 1\n" % index for index in range(20000)) + "let b = y" + " +/* c */1" * 40 + "\n"
	result = run_process(
		[str(binary), "--stdin", "--lang", "swift", "--no-lint"],
		input=glued.encode(),
		capture_output=True,
		timeout=30
	)
	require(b"comments glued to operators exceed parser limit" in result.stderr, result.stderr[:200])
	# 危険なコメント列を解析前に見送り，安全な境界例を通す事の検証
	for lang, source in (
		("python", "def f():\n" + "    # c\n" * 8000 + "    return 1\n"),
		("python", "x = 1\n" + "# c\n" * 16000 + "y = 2\n"),
		("python", "x = 1\n" + "\\\n" * 16000 + "y = 2\n"),
		("javascript", "a = foo\n" + "// c\n" * 16000 + ".bar()\n"),
		("typescript", "a = foo\n" + "/**/ " * 16000 + "\n.bar()\n"),
		("kotlin", "fun f() {\nval a = foo\n" + "// c\n" * 16000 + ".bar()\n}\n"),
		("php", "<?php\n$a = $foo\n" + "// c\n" * 16000 + "->bar();\n"),
		("javascript", "a = foo\n" + "\v// c\n" * 16000 + ".bar();\n"),
		("javascript", "a = foo" + " /**/" * 16000 + "\n.bar();\n"),
		("python", "\\\n" * 16000 + "x = 1\n"),
		("javascript", "a = foo\r" + "// c\r" * 16000 + ".bar();\r"),
		("javascript", "const tick = /`/;\na = foo\n" + "// c\n" * 16000 + ".bar();\n"),
		("javascript", "const e = <p>`</p>;\na = foo\n" + "// c\n" * 16000 + ".bar();\n"),
		("javascript", "const re = /\\/*/;\na = foo\n" + "// c\n" * 16000 + ".bar();\n"),
		("kotlin", "fun f() {\nval a = 1 /* start\n\"\"\" */\nval b = foo\n" + "// c\n" * 16000 + ".bar()\n}\n"),
		("javascript", "if (x) /[/*]/.test(s);\na = foo\n" + "// c\n" * 16000 + ".bar();\n"),
		("javascript", "const y = <p>a/*</p>;\na = foo\n" + "// c\n" * 16000 + ".bar();\n"),
		("kotlin", "val s = \"${\"/*\"}\"\nval a = foo\n" + "// c\n" * 16000 + ".bar()\n"),
		("php", "<?php\n$h = <<<EOT\n/*\nEOT;\n$a = foo\n" + "// c\n" * 16000 + "->bar();\n"),
		("php", "<p>/*</p><?php\n$a = foo\n" + "// c\n" * 16000 + "->bar();\n")
	):
		result = run_process(
			[str(binary), "--stdin", "--lang", lang, "--no-lint"],
			input=source.encode(),
			capture_output=True,
			timeout=10
		)
		require(b"comments exceed parser limit" in result.stderr, (lang, result.stderr[:200]))
	for lang, source in (
		("javascript", "x = 1; /*\n" + "// old line: doSomething(a, b, c);\n" * 4000 + "*/\ny = 2;\n"),
		("php", "<?php\n$x = 1; /*\n" + "# old line: doSomething($a);\n" * 4000 + "*/\n$y = 2;\n")
	):
		result = run_process(
			[str(binary), "--stdin", "--lang", lang, "--no-lint"],
			input=source.encode(),
			capture_output=True,
			timeout=10
		)
		require(b"[skip]" not in result.stderr, (lang, result.stderr[:200]))
	result = run_process(
		[str(binary), "--stdin", "--lang", "swift", "--no-lint"],
		input="".join("//* note %d\nlet v%d = %d\n" % (index, index, index) for index in range(1000)).encode(),
		capture_output=True,
		timeout=10
	)
	require(b"comments glued to operators exceed parser limit" in result.stderr, result.stderr[:200])
	guarded = b"\xef\xbb\xbf{\x00}\r\n"
	result = run_process([str(binary), "--stdin", "--lang", "json", "--fail-on-skip"], input=guarded, capture_output=True)
	require(result.returncode == 1 and result.stdout == guarded and b"[skip]" in result.stderr)
	with tempfile.TemporaryDirectory(prefix="shaveformat-runtime-") as temporary:
		# macOS の一時領域は記号リンクの `/var` を経る為，明示指定でもリンクを辿らない CLI へ実体の経路を渡す
		directory = Path(temporary).resolve()
		path = directory / "sample.json"
		path.write_bytes(b"\xef\xbb\xbf{ \"x\" : 1 }\r\n")
		run_process([str(binary), "--write", str(path)], capture_output=True, check=True)
		result = path.read_bytes()
		require(result.startswith(b"\xef\xbb\xbf") and result.endswith(b"\r\n") and b"\r\r\n" not in result)
		require(list(directory.iterdir()) == [path], "temporary file was left behind")
		# 末尾区切文字は経路の表記に過ぎない為，通常ディレクトリを記号リンクと誤認しない
		scan_directory = directory / "scan"
		scan_directory.mkdir()
		(scan_directory / "formatted.c").write_bytes(b"int x;\n")
		scan_result = run_process([str(binary), "--check", "--fail-on-skip", f"{scan_directory}{os.sep}"], capture_output=True)
		require(not scan_result.returncode and b"symbolic link(s) not followed" not in scan_result.stderr, scan_result)
		(scan_directory / "formatted.c").unlink()
		scan_directory.rmdir()
		header = directory / "ambiguous.h"
		header.write_bytes(b"template <typename T> void Set(T Value) { value_ = Value; }\n")
		header_result = run_process([str(binary), "--write", str(header)], capture_output=True)
		require(not header_result.returncode and b"parser could not read the source" not in header_result.stderr.lower())
		expected_header = b"/**\n *\n * @param Value\n */\ntemplate<typename T> void Set(T Value) {\n\tvalue_ = Value;\n}\n"
		require(header.read_bytes() == expected_header, "C++ header detection did not preserve the complete definition")
		header.unlink()
		# ハードリンク切断時の警告と要約件数の検査
		original = b'{ "x" : 1 }\n'
		path.write_bytes(original)
		alias = directory / "hard-link.json"
		os.link(path, alias)
		hard_link_result = run_process([str(binary), "--write", str(path)], capture_output=True, check=True)
		require(b"hard link was broken" in hard_link_result.stderr, hard_link_result)
		require(hard_link_result.stdout.endswith(b"Done. 1/1 file(s) formatted, 1 warning(s).\n"), hard_link_result)
		require(alias.read_bytes() == original and path.read_bytes() != original, "hard link contents were not separated")
		alias.unlink()
		# 拡張機能は `.h` の文書に `--lang h` を渡し，標準入力でもファイルの `.h` と同じく中身で C / C++ を判別させる
		for source, expected in (
			(b"template <typename T> void Set(T Value) { value_ = Value; }\n", expected_header),
			(b"int init(void);\n", b"int init(void);\n")
		):
			stdin_header = (
				run_process([str(binary), "--stdin", "--lang", "h", "--no-lint"], input=source, capture_output=True, check=True).stdout
			)
			require(stdin_header == expected, ("--lang h did not detect the header language like a .h file", stdin_header))
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
				access = command_output(["ls", "-le", str(path)]).splitlines()[1:]
				failed = run_process([str(binary), "--write", str(path)], capture_output=True)
				require(failed.returncode == 1 and path.read_bytes() == original)
				require(access == command_output(["ls", "-le", str(path)]).splitlines()[1:])
				require(list(directory.iterdir()) == [path], "temporary file was left behind")
			finally:
				run_process(["chmod", "-N", str(path)], check=True)
			run_process([str(binary), "--write", str(path)], capture_output=True, check=True)
		if os.name == "posix":
			def limit_output() -> None:
				"""原本に触れず，子の標準出力だけを書込不能にする"""
				resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))
				signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
				# 終了
				return
			empty = directory / "empty"
			empty.mkdir()
			for arguments in ["--check", str(path)], ["--help"], ["--version"], [str(empty)]:
				with (directory / "stdout").open("wb") as output:
					failed = run_process([str(binary), *arguments], stdout=output, stderr=subprocess.PIPE, preexec_fn=limit_output)
				require(failed.returncode == 1 and b"failed to write stdout" in failed.stderr, arguments)
			# 明示指定の経路もリンクを経れば辿らず（木の外の実体を書き換えない），名指した物を読まなかった事を `--fail-on-skip` で失敗に数える
			real = directory / "real"
			real.mkdir()
			(real / "sample.c").write_bytes(b"int  x;\n")
			(real / "formatted.c").write_bytes(b"int x;\n")
			(directory / "link").symlink_to(real)
			linked = str(directory / "link" / "sample.c")
			for arguments, code in (
				(["--check", str(real / "formatted.c"), linked], 0),
				(["--check", "--fail-on-skip", str(real / "formatted.c"), linked], 1),
				(["--write", "--fail-on-skip", linked], 1)
			):
				result = run_process([str(binary), *arguments], capture_output=True)
				require(result.returncode == code and b"1 symbolic link(s) not followed" in result.stdout, (arguments, result))
			require((real / "sample.c").read_bytes() == b"int  x;\n", "file behind a symbolic link was rewritten")
	print(f"runtime: {len(CASES)} languages, byte preservation OK")
	# 全検査を通過した事の返戻
	return 0

if __name__ == "__main__":
	sys.exit(main())
