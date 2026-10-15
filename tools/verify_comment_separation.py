#! /usr/bin/env python3
"""経路の分離・コメント非依存・空行の不変性の検査（整形仕様 1.2, 8.1, 10.5, 11.1）

フォーマッターは検査用の指定の時だけ，構文・中間表現・選択・アンカーの対応・配置の観測を標準エラーへ出す
`[layout]` のテキストはバイト毎の１６進で表す
此の観測は整形結果・検証・分類を変えない
コメントを変えた入力は此処で用意し，製品の同じ入口を通して中核の出力を比べる
各テストケースに就いて次を確かめる
中核工程へコメントノードが渡った不具合の失敗と，空行の不変性の違反が無い
通常コメントを除き，構文の属性を残した入力でも，同じ中核の出力に為る
独立した行コメントの本文を長くした入力でも中核の出力が変わらない
加えて，次の行に作用するディレクティブ・ドキュメント化のタグ・エンコーディング宣言・フォーマッター向のタグを普通の注記と入替えた雛形で，中核の出力が一致し，整形結果は其れへ空でないコメントの行を足しただけである事を確かめる

使方：
python3 tools/verify_comment_separation.py [フォーマッター]
"""

from __future__ import annotations

from verify_case_variants import JOBS, OPENERS

import itertools
import json
import os

from pathlib import Path

import re
import subprocess
import sys

from typing import NamedTuple

import concurrent.futures

from html.parser import HTMLParser

# 検査の経路と実行設定
ROOT: Path = Path(__file__).resolve().parent.parent
GOLDEN: Path = ROOT / "test_data/golden"
ENVIRONMENT: dict[str, str] = dict(os.environ, SHAVEFMT_VERIFY_COMMENT_SEPARATION="1")
LAYOUT_PREFIX: bytes = b"[layout] "
# 行コメントの記号（本文を長くする変異に使う，行コメントを持つ言語だけ）
LINE_MARKERS: dict[str, bytes] = { lang: openers[0].encode() for lang, openers in OPENERS.items() if openers[0] in ("#", "//") }
# HTML の子の JavaScript に使う行コメントの記号
LINE_MARKERS["html"] = b"//"
# 本文へ足す語（行幅を超える長さでコードの折返がコメントに依らない事も確かめる）
SUFFIX: bytes = b" variant" * 20

# 行幅を超える本体（コメントが折返・インデントを動かさない事を確かめる，短い本体では折返が起きず検査を擦り抜ける）
LONG_JS: str = (
	"const value = computeSomethingWithAVeryDescriptiveName(1111111, 2222222, 3333333, 4444444, "
	"5555555, 6666666, 7777777, 8888888, 999);"
)

LONG_C: str = (
	"int compute(int aaaaaaaa, int bbbbbbbb, int cccccccc, int dddddddd, int eeeeeeee, int ffffffff, " "int gggggggg) { return 0; }"
)

# 言語・本体の雛形（`{}` へコメントを差し込む）・差し込むコメントの並び
# 各注記列の先頭を基準の配置と比較する為に内側の順を保つ
CONTENT_GROUPS: tuple[tuple[str, str, tuple[str, ...]], ...] = (
	("c", "{}\n#include <stdio.h>\nint   x;\nint   y;\n", ("// note", "//go:build linux", "/* note */", "/** doc */")),
	("c", "{}\n" + LONG_C.replace("{", "{{").replace("}", "}}") + "\n", ("// note", "/* note */", "/** doc */")),
	("cpp", "{}\n#include <cstdio>\nint   x;\n", ("// note", "//go:build linux", "/** doc */")),
	("csharp", "{}\nclass C {{ int F() {{ return 1; }} }}\n", ("// note", "/// <summary>x</summary>")),
	("java", "{}\nclass C {{ int f() {{ return 1; }} }}\n", ("// note", "/** doc */")),
	("go", "{}\npackage p\n\nfunc f() int {{ return 1 }}\n", ("// note", "//go:build linux", "//go:embed x", "//go:generate cmd")),
	(
		"rust",
		"{}\nfn f() -> i32 {{ 1 }}\n",
		("// note", "/* note */", "/// note", "//! note", "/** note */", "/*! note */", "//// note", "/*** note */")
	),
	("swift", "{}\nfunc f() -> Int {{ return 1 }}\n", ("// note", "/// doc")),
	("php", "<?php\n{}\n$a = 1;\n$b = 2;\n", ("// note", "/* note */", "/** doc */", "# note")),
	("javascript", "{}\nconst a = 1;\nfunction f() {{ return a; }}\n", ("// note", "// @ts-ignore", "/*#__PURE__*/")),
	("javascript", "{}\n" + LONG_JS + "\n", ("// note", "/* note */", "/*#__PURE__*/")),
	(
		"typescript",
		"{}\nconst a = 1;\nfunction f() {{ return a; }}\n",
		("// note", "// @ts-ignore", "// @ts-expect-error", "/*#__PURE__*/", "/** @type {{number}} */")
	),
	("typescript", "{}\n" + LONG_JS + "\n", ("// note", "// @ts-expect-error")),
	("ruby", "{}\nx = 1\ny = 2\n", ("# note", "# frozen_string_literal: true", "# encoding: utf-8")),
	("python", "{}\nx = 1\ny = 2\n", ("# note", "# type: ignore", "# -*- coding: utf-8 -*-"))
)

# 埋込の対照入力と，入力の値から手で定めた JavaScript の結果（整形したテキストの期待値ではない）
class EmbeddedCase(NamedTuple):
	name: str
	plain: str
	commented: str
	marker: str
	expected: str|None # JavaScript の JSON 値（CSS と空の子は None）

# 同じ投影済の構文を持つ対だけを比較し，コメント内の改行で文境界の異なる物は別の対とする
EMBEDDED_GROUPS: tuple[EmbeddedCase, ...] = (
	EmbeddedCase("empty-script", "<script></script>", "<script>// 子注記\n</script>", "子注記", None),
	EmbeddedCase("empty-style", "<style></style>", "<style>/* 子注記 */</style>", "子注記", None),
	EmbeddedCase(
		"inline",
		"<script>globalThis.result=1+2;</script>",
		"<script>globalThis.result=1/* 文中 */+2;\t\t// 行末\n</script>",
		"文中",
		"3"
	),
	EmbeddedCase(
		"document",
		"<script>const o={value:7};globalThis.result=o.value;</script>",
		"<script>const o={\n/** 値の意味 */\nvalue:7};globalThis.result=o.value;</script>",
		"値の意味",
		"7"
	),
	EmbeddedCase(
		"verbatim-tab",
		"<div><script>const s=String.raw`A\n\tB  \n`;globalThis.result=Array.from(s,c=>c.codePointAt(0));</script></div>",
		"<div><script>// 逐語注記\nconst s=String.raw`A\n\tB  \n`;globalThis.result=Array.from(s,c=>c.codePointAt(0));</script></div>",
		"逐語注記",
		"[65,10,9,66,32,32,10]"
	),
	EmbeddedCase(
		"nested-depth-blank",
		"<div><section><script>function f(){return 3;}const n=3;globalThis.result=f()+n;</script></section></div>",
		"<div><section><script>function f(){return 3;\n// 閉境界\n}const n=3;"
		"globalThis.result=f()+n;\n// 子末尾\n</script></section></div>",
		"閉境界",
		"6"
	),
	EmbeddedCase(
		"wide-call",
		"<script>globalThis.result=Math.max(1111111,2222222,3333333,4444444,5555555,6666666);</script>",
		"<script>globalThis.result=Math.max(1111111,/* 幅注記 */2222222,3333333,4444444,5555555,6666666);</script>",
		"幅注記",
		"6666666"
	),
	EmbeddedCase(
		"return-newline",
		"<script>function f(){return;7;}globalThis.result=f()===undefined;</script>",
		"<script>function f(){return/* 改行注記\n*/7;}globalThis.result=f()===undefined;</script>",
		"改行注記",
		"true"
	),
	EmbeddedCase(
		"return-inline",
		"<script>function f(){return 7;}globalThis.result=f()===undefined;</script>",
		"<script>function f(){return/* 同行注記 */7;}globalThis.result=f()===undefined;</script>",
		"同行注記",
		"false"
	),
	EmbeddedCase(
		"strict-syntax",
		"<script>\"use strict\";globalThis.result=(function(){return this;})()===undefined;</script>",
		"<script>/* 構文注記 */\"use strict\";globalThis.result=(function(){return this;})()===undefined;</script>",
		"構文注記",
		"true"
	),
	EmbeddedCase(
		"same-line-close",
		"<script>let a=1;globalThis.result=a;</script>",
		"<script>let a=1;globalThis.result=a;// 同行注記</script>",
		"同行注記",
		"1"
	),
	EmbeddedCase(
		"between-statements",
		"<script>let a=1;globalThis.result=a;</script>",
		"<script>let a=1;\n// 文間注記\nglobalThis.result=a;</script>",
		"文間注記",
		"1"
	),
	EmbeddedCase("css-declaration", "<style>.a{color:red;}</style>", "<style>.a{/* 宣言前 */color:red;}</style>", "宣言前", None),
	EmbeddedCase("css-compound", "<style>.a.b{color:red;}</style>", "<style>.a/* 選択注記 */.b{color:red;}</style>", "選択注記", None),
	EmbeddedCase("css-descendant", "<style>.a .b{color:red;}</style>", "<style>.a /* 選択注記 */.b{color:red;}</style>", "選択注記", None)
)

# 完成した子を変更せず `Script` として実行する（先頭の削除・インデントの除去・文の追記を行わない）
EMBEDDED_NATIVE: str = r"""
const fs = require("node:fs"), vm = require("node:vm");
const sources = JSON.parse(fs.readFileSync(0, "utf8"));
const results = sources.map(source => {
	const context = vm.createContext(Object.create(null));
	try {
		new vm.Script(source).runInContext(context, { timeout: 1000 });
		// 観測した値の返戻
		return { value: JSON.stringify(context.result) };
	} catch(e) {
		// 構文又は実行の失敗の返戻
		return { error: String(e) };
	}
});
process.stdout.write(JSON.stringify(results));
"""

class EmbeddedParser(HTMLParser):
	"""テストケースの script / style の本文を，空白・文字参照を変更せず取得する
	テストケースは明示した閉タグを持ち，本文に終了タグの綴りや HTML のエスケープ状態を含まない
	"""

	def handle_data(self: EmbeddedParser, data: str) -> None:
		"""現在の子の本文を控える
		計算量：O(1)（テキストを複写せず断片を控える）
		引数：
			self: 本文の断片を保持するパーサー
			data: パーサーが読んだ本文の断片
		"""
		# 子の本文の断片の収集
		if self.tag:
			self.parts.append(data)
		# 終了
		return

	def handle_starttag(self: EmbeddedParser, tag: str, attrs: list[tuple[str, str|None]]) -> None:
		"""埋込の開始タグを控える
		計算量：O(1)
		引数：
			self: 埋込の開始を控えるパーサー
			tag: 開始タグの名前
			attrs: パーサーが読んだ属性（本文抽出には用いない）
		"""
		# 埋込の開始タグの登録
		if tag in ("script", "style"):
			self.tag: str = tag
			self.parts: list[str] = []
		# 終了
		return

	def handle_endtag(self: EmbeddedParser, tag: str) -> None:
		"""終了した子の本文を取得列へ加える
		計算量：子の本文の文字数 L に対して O(L)
		引数：
			self: 埋込の本文を確定するパーサー
			tag: 終了タグの名前
		"""
		# 閉じた埋込の本文の確定
		if tag == self.tag:
			self.children.append((tag, "".join(self.parts)))
			self.tag: str = ""
		# 終了
		return

	def __init__(self: EmbeddedParser) -> None:
		"""本文を保持するパーサーを初期化する
		計算量：O(1)
		引数：
			self: 初期化するパーサー
		"""
		# メンバの初期化
		super().__init__(convert_charrefs=False)
		self.children: list[tuple[str, str]] = []
		self.tag: str = ""
		self.parts: list[str] = []
		# 終了
		return

def embedded_child(src: bytes) -> tuple[str, str]:
	"""単一の所有領域を持つ HTML テストケースから子の種類と本文を得る
	計算量：入力の文字数 L に対して O(L)
	引数：
		src: 原入力又は完成した HTML
	戻値：
		所有するタグの名前と，加工して居ない本文
	送出例外：
		ValueError: 閉じない子，又は子の個数が１でない場合
	"""
	# 完成 HTML からの子の抽出
	parser: EmbeddedParser = EmbeddedParser()
	parser.feed(src.decode("utf-8"))
	parser.close()
	if parser.tag or len(parser.children) != 1:
		raise ValueError("the embedded child was lost, duplicated or left unclosed")
	# 子の返戻
	return parser.children[0]

def empty_child_problems(child: str, layout: str, marker: str, tag: str) -> list[str]:
	"""空の子へ通常注記だけを置き，配置枠の救済にコード改行を足して居ないか調べる
	計算量：子の文字数 L に対して O(L)
	引数：
		child: 完成 HTML 内の子の本文
		layout: 同じ子の CodeOnly の本文
		marker: 通常注記の本文に置いた識別語
		tag: 所有するタグの名前
	戻値：
		配置又は境界の違反の列
	"""
	# 空のコードと既存の独立行境界の確認（整形仕様 3.2, 7.6, 7.7）
	if layout.strip():
		# コードを持った違反の返戻
		return ["the empty child acquired code"]
	has_line_boundary: bool = "\n" in layout
	if marker not in child:
		# LF の有無だけでは合法な配置枠を確認出来ない為，通常注記の脱落だけでは違反としない
		# 脱落後のコード不変性の返戻
		return [] if child == layout else ["dropping the ordinary comment changed the empty child"]
	# 子自身の DocumentEnd 枠は在るが，親の行途中で其の前へ改行を足しては為らない
	if has_line_boundary:
		prefix: str = layout[:layout.rfind("\n") + 1]
		suffix: str = layout.rsplit("\n", 1)[-1]
		if not child.startswith(prefix) or not child.endswith(suffix):
			# 既存の境界が変わった違反の返戻
			return ["the empty child comment changed the existing tag boundaries"]
		child = child[len(prefix):len(child) - len(suffix) if suffix else len(child)]
	elif child.startswith(("\n", "\r")):
		# 配置救済の為に改行を足した違反の返戻
		return ["a newline was added to rescue the empty child comment"]
	lines: list[str] = child.splitlines()
	if any(not line.strip() for line in lines):
		# 空行を足した違反の返戻
		return ["the empty child comment introduced a blank line"]
	if child.count(marker) != 1 or len(lines) != 1:
		# 注記の個数又は論理行が変わった違反の返戻
		return ["the empty child comment was duplicated or split"]
	# 此の２テストケースでは，後続する所有要素の閉タグの深さは０
	# 同じ独立種別の合法な記法と，行を終える改行だけの確認
	pattern: str = r"// [^\r\n]+\n" if tag == "script" else r"/\* [^\r\n]+ \*/\n"
	# 違反の返戻
	return [] if re.fullmatch(pattern, child) else ["the empty child has no legal independent comment line"]

def embedded_shape_problems(case: EmbeddedCase, child: str, layout: str, width: str, has_comment: bool) -> list[str]:
	"""確定済の子の境界に対して記法・深さ・空行・幅の契約を照合する
	計算量：子の文字数 L に対して最悪 O(L²)（正規表現の照合を含む）
	引数：
		case: 入力から定めた受入条件
		child: 完成 HTML 内の子の本文
		layout: 同じ子の CodeOnly の本文
		width: フォーマッターへ指定した幅
		has_comment: テスト側で注記を加えた入力か
	戻値：
		記法・深さ・空行・幅の違反の列
	"""
	# 完成した子の構造の照合の準備
	problems: list[str] = []
	match case.name:
		case "empty-script" | "empty-style":
			if not has_comment:
				# 注記を持たない対照に行を追加しない事の返戻
				return [] if child == layout else ["the empty control changed during composition"]
			# 空の子の保持又は通常注記の合法な脱落の返戻
			return empty_child_problems(child, layout, case.marker, "script" if case.name == "empty-script" else "style")
		# ドキュメントは通常記法へ変換しても脱落を許さず，所有領域の外へ移さない（整形仕様 7.4, 7.6, 8.3）
		case "document" if has_comment:
			if child.count(case.marker) != 1:
				problems.append("the recognized document was lost or duplicated")
			if (
				any("const o" in line and "value" in line for line in layout.split("\n")) and
				not re.search(r"\n(?P<depth>\t*)// 値の意味[^\n]*\n(?P=depth)const o\b", child)
			):
				problems.append("the inline member document was not moved before its owning code line")
		# 文中種別と行末のタブ数（空白を一括削除して比べない）
		case "inline" if has_comment:
			if case.marker in child and not re.search(r"/\* " + case.marker + r"[^*]*\*/", child):
				problems.append("the inline comment changed kind")
			if not re.search(r"\t\t// 行末(?:\n|$)", child) or child.count("行末") != 1:
				problems.append("the trailing comment or its two separating tabs changed")
		# 原入力のネストと後続コード行から定まる絶対深さと，関数・宣言の構文境界に在る１行の空行
		# 関数の閉波括弧は深さ３，所有要素の閉タグは深さ２
		case "nested-depth-blank":
			if not re.search(r"\n\t{3}\}\n\n\t{3}const n\b", child):
				problems.append("the function/declaration blank boundary or its depth changed")
			if has_comment:
				if not re.search(r"\n\t{3}// 閉境界[^\n]*\n\t{3}\}", child) or child.count("閉境界") != 1:
					problems.append("the closing-boundary comment has the wrong depth or owner")
				if not re.search(r"\n\t{2}// 子末尾\n\t{2}$", child) or child.count("子末尾") != 1:
					problems.append("the child-end comment has the wrong depth or owner")
		# コメント無のコードで，狭い幅の折返と無制限を含む同行形を区別する
		case "wide-call":
			code_lines: list[str] = [line for line in layout.split("\n") if line.strip()]
			if (len(code_lines) > 1) != (width == "32"):
				problems.append("the call did not use the required width-dependent form")
		# 此の２テストケースのセレクタには文字列・エスケープ・囲みの入子が無い為，CSS のコメントトークンだけを読み飛ばす
		# CodeOnly 比較には適用しない（整形仕様 6.6, 11.1），ブラウザでの意味は未検証
		case "css-compound" | "css-descendant":
			expected: str = ".a.b" if case.name == "css-compound" else ".a .b"
			for text in child, layout:
				selector: str = re.sub(r"/\*.*?\*/", "", text.partition("{")[0], flags=re.DOTALL)
				if " ".join(selector.split()) != expected:
					problems.append("the compound/descendant selector boundary changed")
	# 違反の返戻
	return problems

def embedded_native_problems(cases: list[tuple[str, str, str]]) -> list[str]:
	"""入力の子と完成 HTML の子を実際の処理系で実行し，入力から定めた結果へ照合する
	計算量：テストケース数 F・文字数の和 L・処理系の実行時間 T に対して O(L+FT)
	引数：
		cases: 原入力と完成候補の順に並べた識別名・子の本文・期待する JSON 値
	戻値：
		処理系の失敗又は値の不一致の列
	"""
	# 処理系の失敗も検査失敗として報告し，未実施を成功へ読み替えない
	try:
		result: subprocess.CompletedProcess[str] = subprocess.run(
			["node", "-e", EMBEDDED_NATIVE],
			input=json.dumps([src for _, src, _ in cases]),
			text=True,
			capture_output=True,
			timeout=60,
			check=True
		)
		vals: list[dict[str, str]] = json.loads(result.stdout)
	except (OSError, subprocess.SubprocessError, ValueError) as e:
		# 診断を失わない失敗の返戻
		return [f'native JavaScript did not complete: {e}; stderr={getattr(e, "stderr", None)!r}']
	if not isinstance(vals, list) or len(vals) != len(cases) or any(not isinstance(val, dict) for val in vals):
		# 不完全な結果の違反の返戻
		return ["native JavaScript returned an incomplete result list"]
	# 手で定めた値の照合に依り，入力と完成候補の双方の意味を確認する
	problems: list[str] = [
		f"{label}: native result {actual!r}, expected {expected}" for (label, _, expected), actual in zip(cases, vals) if actual.get(
			"value"
		) != expected
	]
	for (label, _, _), original, changed in zip(cases[1::2], vals[0::2], vals[1::2]):
		if original.get("value") != changed.get("value"):
			problems.append(f"{label}: native result differs from the input child")
	# 違反の返戻
	return problems

def observations(errors: str) -> tuple[str, ...]:
	"""通知から中核の観測の全行を抜出す
	計算量：通知の長さ B に対して O(B)
	引数：
		errors: フォーマッターの通知
	戻値：
		構文・中間表現・選択・アンカー・配置を含む観測
	"""
	# 中核の観測の通知の抽出
	prefixes: tuple[str, ...] = (
		"[anchors]",
		"[broken]",
		"[embedded]",
		"[ending]",
		"[facts]",
		"[following]",
		"[form]",
		"[format-root]",
		"[format]",
		"[forms]",
		"[fragment]",
		"[kind]",
		"[laidout]",
		"[layout]",
		"[owned-end]",
		"[owned]",
		"[part]",
		"[partition]",
		"[piece]",
		"[placement]",
		"[selection]",
		"[separator]",
		"[syntax]"
	)
	# 中核の観測の返戻
	return tuple(line for line in errors.splitlines() if line.startswith(prefixes))

def observe(binary: str, text: bytes, lang: str, width: list[str]) -> tuple[int, bytes, bytes, str]:
	"""テストケースを整形し，終了コード・整形結果・中核の出力・標準エラーを返す
	計算量：入力・出力のバイト数 B・フォーマッターの実行時間 T に対して O(B+T)
	引数：
		binary: フォーマッターの経路
		text: テスト側で用意した入力
		lang: 入力の言語
		width: 行幅の指定を表す引数列
	戻値：
		終了コード・完成候補・CodeOnly の観測・標準エラー
	送出例外：
		OSError: フォーマッターを起動出来ない場合
		subprocess.TimeoutExpired: 整形が所要時間の上限を超えた場合
		ValueError: 中核の出力の観測の行が１６進のテキストでない場合
	"""
	# 中核出力を含む整形結果の取得
	command: list[str] = [binary, "--stdin", "--lang", lang, "--no-lint", *width]
	result: subprocess.CompletedProcess[bytes] = (
		subprocess.run(command, input=text, capture_output=True, timeout=300, env=ENVIRONMENT)
	)
	# 通知からの中核出力の抽出
	lines: list[bytes] = []
	is_embedded: bool = False
	for line in result.stderr.split(b"\n"):
		if line.startswith(b"[embedded] "):
			is_embedded = line.endswith(b" begin")
		elif not is_embedded and line.startswith(LAYOUT_PREFIX):
			lines.append(bytes.fromhex(line[len(LAYOUT_PREFIX):].decode()))
	# 観測の返戻
	return result.returncode, result.stdout, b"".join(line + b"\n" for line in lines), result.stderr.decode(errors="replace")

def line_changes(code: list[bytes], lines: list[bytes], marker: bytes) -> tuple[int, list[tuple[int, bytes|None]]]:
	"""既知のコード列へ加えた行と行末コメントを，原文順に１度ずつ対応させる
	計算量：両列の総バイト数 B に対して O(B)
	引数：
		code: コメントの無いコードの行列
		lines: コメントの配置後の行列
		marker: 許す行末コメントの記号（空なら行の追加だけ）
	戻値：
		未対応のコード行数と，追加・行末の変更の位置と元行（追加なら `None`）
	"""
	# コード行を消費する前進だけの照合
	pos: int = 0
	changes: list[tuple[int, bytes|None]] = []
	for idx, line in enumerate(lines):
		if pos < len(code) and line == code[pos]:
			pos += 1
		elif (
			marker and pos < len(code) and (original := code[pos].rstrip(b" \t")) and line.startswith(original) and
			line[len(original):].lstrip(b" \t").startswith(marker)
		):
			changes.append((idx, original))
			pos += 1
		else:
			changes.append((idx, None))
	# 対応の残りと追加・変更の返戻
	return len(code) - pos, changes

def lengthen_comments(output: bytes, layout: bytes, marker: bytes) -> bytes|None:
	"""コード列を変えずに，識別した行コメントの本文を長くした入力を返す
	計算量：入力・出力のバイト数 B に対して O(B)
	引数：
		output: 通常の出力
		layout: 同じ整形時に観測した `CodeOnly`
		marker: 行コメントの開始記号
	戻値：
		本文を長くした再入力，又は対象の無い場合は `None`
	"""
	# 逐語のコード行との一致を先に扱い，本文中のコメント記号を変異しない
	lines: list[bytes] = output.split(b"\n")
	changes: list[tuple[int, bytes|None]]
	_, changes = line_changes(layout.split(b"\n"), lines, marker)
	has_changed: bool = False
	for idx, original in changes:
		# 行末のバックスラッシュは次の行を繋ぐ為に本文を足さない
		if (original is not None or lines[idx].lstrip(b" \t").startswith(marker)) and not lines[idx].endswith(b"\\"):
			lines[idx] += SUFFIX
			has_changed = True
	# 変異した入力の返戻
	return b"\n".join(lines) if has_changed else None

def mutation_self_check() -> list[str]:
	"""逐語内容・行連結の保護と，行末・独立コメントの変異を正誤対で照合する
	計算量：正誤対の総バイト数 B に対して O(B)
	戻値：
		条文と入力から定めた期待と違うケースの一覧
	"""
	# 反復行・逐語行・挿入・行末の各経路の独立した期待
	found: list[str] = []
	for output, layout, expected in (
		(b"// note\nx();\nx();\n", b"x();\nx();\n", b"// note" + SUFFIX + b"\nx();\nx();\n"),
		(b"x(); // note\ny();\n", b"x();\ny();\n", b"x(); // note" + SUFFIX + b"\ny();\n"),
		(b"R\"(\n// literal\n)\";\n", b"R\"(\n// literal\n)\";\n", None),
		(b"// note\\\nx();\n", b"x();\n", None),
		(b"x();\n", b"x();\n", None),
		(b"// note\nx(); // tail\n", b"x();\n", b"// note" + SUFFIX + b"\nx(); // tail" + SUFFIX + b"\n")
	):
		if lengthen_comments(output, layout, b"//") != expected:
			found.append(repr(output))
	# コード列を消す不正候補と，追加だけの正常候補の区別
	for lines, should_preserve in ([b"x();", b"// note", b"x();"], True), ([b"x();", b"// note"], False):
		remaining: int
		changes: list[tuple[int, bytes|None]]
		remaining, changes = line_changes([b"x();", b"x();"], lines, b"")
		if (not remaining and all(lines[idx].strip() for idx, _ in changes)) != should_preserve:
			found.append(repr(lines))
	# 変異の食違の返戻
	return found

def inspect(binary: str, src: Path, lang: str) -> list[str]:
	"""１つのテストケースの違反の並びを返す
	計算量：入力・出力のバイト数 B・総実行時間 T に対して O(B+T)
	引数：
		binary: フォーマッターの経路
		src: 基準入力の経路
		lang: テストケースの言語
	戻値：
		観測・コメント非依存の違反の列
	送出例外：
		OSError: テストケース又は行幅の指定を読めないか，フォーマッターを起動出来ない場合
		subprocess.TimeoutExpired: 整形が所要時間の上限を超えた場合
		ValueError: 中核の出力の観測の行が１６進のテキストでない場合
		UnicodeDecodeError: 読み込んだファイルを指定の文字コードで解釈出来ない場合
	"""
	# テストケースの行幅と初回観測の取得
	chars: Path = src.with_suffix(".chars")
	width: list[str] = ["--chars", chars.read_text().strip()] if chars.exists() else []
	status: int
	output: bytes
	layout: bytes
	errors: str
	status, output, layout, errors = observe(binary, src.read_bytes(), lang, width)
	problems: list[str] = []
	# 中核へのコメント混入と見送の確認
	if "comment nodes reach the core passes" in errors:
		problems.append("comment nodes reach the core passes")
	if "formatting would change blank lines" in errors:
		problems.append("comment placement changes blank lines")
	if status:
		problems.append(f"the formatter exited with status {status}")
	if "[skip]" in errors:
		problems.append("unexpected skip")
	if problems:
		# 呼出側が期待する見送・失敗のテストケースを除いて居る為，此処へ到達した失敗を違反とする一覧の返戻
		return problems
	stripped_input: bytes = layout
	marker: bytes|None = LINE_MARKERS.get(lang)
	variant: bytes|None = lengthen_comments(output, layout, marker) if marker else None
	stripped_errors: str = errors
	# コメントを含まない出力は除く物が無く，其の再整形は基準テストケースの２巡目の検査と同じ為に省く
	if layout != output:
		stripped_status: int
		stripped_layout: bytes
		stripped_status, _, stripped_layout, stripped_errors = observe(binary, stripped_input, lang, width)
		if stripped_status or "[skip]" in stripped_errors:
			problems.append("the comment-free input was not formatted")
		# 出力から除いたテキストは行分割が選んだ表現を含み，原文と投影後の構文が異なる為，配置のテキストだけを照合する
		elif stripped_layout != layout:
			problems.append("removing the comments changes the code")
	# 独立コメントの本文変更に依る配置の照合
	if variant is not None:
		variant_status: int
		variant_layout: bytes
		variant_errors: str
		variant_status, _, variant_layout, variant_errors = observe(binary, variant, lang, width)
		if variant_status or "[skip]" in variant_errors:
			problems.append("the comment variant was not formatted")
		# 部分整形はパーサーが読めない領域の切目がコメントのトークンで変わり，投影後の構文が揃わない為，配置のテキストだけを照合する（整形仕様 11.1）
		elif (
			variant_layout != layout or
			"formatting is partial" not in errors and observations(variant_errors) != observations(stripped_errors)
		):
			problems.append("comment content changes the code")
	# 違反の返戻
	return problems

def content_problems(binary: str) -> tuple[int, int, list[str]]:
	"""内容だけが違うコメントを雛形へ差し込み，比べた数と見送の数と違反の並びを返す
	計算量：入力・出力の総文字数 B・総実行時間 T に対して O(B+T)
	引数：
		binary: フォーマッターの経路
	戻値：
		試みた入力数・見送数・違反の列
	送出例外：
		OSError: フォーマッターを起動出来ない場合
		subprocess.TimeoutExpired: 整形が所要時間の上限を超えた場合
		ValueError: 中核の出力の観測の行が１６進のテキストでない場合
	"""
	# 雛形とコメントの組合せの観測
	checked: int = 0
	skipped: int = 0
	problems: list[str] = []
	for lang, body, texts in CONTENT_GROUPS:
		base: list[str]|None = None
		base_observations: tuple[str, ...]|None = None
		for text in texts:
			checked += 1
			status: int
			output: bytes
			layout: bytes
			errors: str
			status, output, layout, errors = observe(binary, body.format(text).encode(), lang, [])
			# 見送は整形の拒否で，整形結果の変動ではない為に別に数える
			if "[skip]" in errors:
				skipped += 1
				continue
			if status:
				problems.append(f"{lang}: {text}: the formatter exited with status {status}")
				continue
			# 雛形は全てコードを持つ為，観測の欠けた空の配置同士の一致を認めない
			if not layout.strip():
				problems.append(f"{lang}: {text}: the core layout observation is missing or empty")
				continue
			code: list[str]
			normal: list[str]
			code, normal = layout.decode(errors="surrogateescape").split("\n"), output.decode(errors="surrogateescape").split("\n")
			base = base or code
			current_observations: tuple[str, ...] = observations(errors)
			if base_observations is None:
				base_observations = current_observations
			elif current_observations != base_observations:
				problems.append(f"{lang}: {text}: core observations differ from the first variant")
			# コメントの内容で配置が変わらない事（中核の出力はディレクティブコメントも含めて全てのコメントを置かない，整形仕様 10.5）
			if code != base:
				problems.append(f"{lang}: {text}: layout differs from the first variant")
			# 通常の出力は配置の行を変えず，空でないコメントの行だけを足す事
			remaining: int
			changes: list[tuple[int, bytes|None]]
			remaining, changes = line_changes([line.encode() for line in code], [line.encode() for line in normal], b"")
			if remaining or any(not normal[idx].strip() for idx, _ in changes):
				problems.append(f"{lang}: {text}: output does not preserve code lines with nonempty comment insertions")
	# 比べた数と見送の数と違反の返戻
	return checked, skipped, problems

def embedded_problems(binary: str) -> tuple[int, list[str]]:
	"""埋込の対照・通常注記・長い本文の親の中核と，完成した子の契約を照合する
	計算量：テストケース数 F・幅数 W・最大の入出力文字数 L・実行時間 T に対して最悪 O(FW(L²+T))
	引数：
		binary: フォーマッターの経路
	戻値：
		整形を試みた入力数と違反の列
	"""
	# 原入力を基に本文だけを長くし，出力を変異入力又は期待テキストに転用しない
	checked: int = 0
	problems: list[str] = []
	native_cases: list[tuple[str, str, str]] = []
	ext: str = "補足" * 80 # 注記の本文へ足す語（行幅を超える長さで，本文の切詰も検出する）
	for case in EMBEDDED_GROUPS:
		if case.commented.count(case.marker) != 1:
			problems.append(f"embedded {case.name}: the fixture has no unique comment marker")
			continue
		variants: tuple[str, ...] = case.plain, case.commented, case.commented.replace(case.marker, case.marker + ext, 1)
		for width in "128", "32", "0":
			base: bytes|None = None
			base_observations: tuple[str, ...]|None = None
			for variant, src in enumerate(variants):
				checked += 1
				label: str = f"embedded {case.name}, width {width}, variant {variant}"
				try:
					status: int
					output: bytes
					layout: bytes
					errors: str
					status, output, layout, errors = observe(binary, src.encode(), "html", ["--chars", width])
					current_observations: tuple[str, ...] = observations(errors)
					if base_observations is None:
						base_observations = current_observations
					elif current_observations != base_observations:
						problems.append(f"{label}: core observations differ from the first variant")
					# 無変更の原文返戻・見送・部分整形を，通常のテストケースの検証成功にしない
					if status or any(
						reason in errors for reason in (
							"[skip]",
							"Embedded code was left as is",
							"formatting is partial",
							"comment nodes reach the core passes",
							"formatting would change blank lines"
						)
					):
						problems.append(f"{label}: a variant was not completely formatted: status={status}; {errors}")
						continue
					# 空の子でも親にはタグが在り，観測無の空文字列同士の一致を認めない
					if not layout.strip():
						problems.append(f"{label}: the parent layout observation is missing or empty")
						continue
					if not variant:
						base = layout
					elif base is None or base != layout:
						problems.append(f"{label}: child comments changed the parent CodeOnly output or the control failed")
					original_tag: str
					original_child: str
					tag: str
					child: str
					layout_tag: str
					code_child: str
					original_tag, original_child = embedded_child(src.encode())
					tag, child = embedded_child(output)
					layout_tag, code_child = embedded_child(layout)
					if original_tag != tag or tag != layout_tag:
						problems.append(f"{label}: the owning element changed")
					# 此の非空テストケースは全てタグとコードを同行に詰めた入力で，原文返戻では必要な本体配置を満たさない
					if case.name not in ("empty-script", "empty-style") and output == src.encode():
						problems.append(f"{label}: the compact original was returned without formatting")
					if case.marker in layout.decode("utf-8"):
						problems.append(f"{label}: child comment text reached the parent CodeOnly output")
					if output.decode("utf-8").count(case.marker) != child.count(case.marker):
						problems.append(f"{label}: a child comment escaped its owning element")
					# 保持した注記の本文を切り詰めて幅検査を擦り抜けない事の確認
					body: str = case.marker + (ext if variant == 2 else "")
					if variant and case.marker in child and body not in child:
						problems.append(f"{label}: the retained comment body was truncated or changed")
					problems += [f"{label}: {problem}" for problem in embedded_shape_problems(case, child, code_child, width, bool(variant))]
					# 行末空白やタブを含む完成した子を其の儘実行し，入力と手で定めた値の双方へ照合する
					if case.expected is not None:
						native_cases.extend(((label + " input", original_child, case.expected), (label + " output", child, case.expected)))
				except (OSError, subprocess.SubprocessError, ValueError) as e:
					problems.append(f'{label}: the observation failed: {e}; stderr={getattr(e, "stderr", None)!r}')
	# 完成 HTML から取得した子の処理系での判定（CSS はトークン境界だけを確認する）
	if native_cases:
		problems += embedded_native_problems(native_cases)
	else:
		problems.append("embedded: no native JavaScript cases were prepared")
	print(f"embedded contracts: {checked} inputs, {len(native_cases)} native candidates; CSS native semantics not checked")
	# 入力数と違反の返戻
	return checked, problems

def main() -> int:
	"""テストケースの中核の出力を比べ，違反が有れば非零で終える
	計算量：テストケース数 F・総入出力文字数 B・実行時間 T に対して最悪 O(B²+FT+F log F)
	戻値：
		全検査に成功した場合は０，其の他は１
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		subprocess.TimeoutExpired: 呼び出した子プロセスが制限時間内に完了しない場合
		ValueError: 読み込んだ値が呼出先の受け入れる形式でない場合
	"""
	# 基準テストケースとフォーマッターの準備
	binary: str = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "build/shavefmt")
	sources: list[Path] = []
	for dir in sorted(path for path in GOLDEN.iterdir() if path.is_dir()):
		sources.extend(
			src for src in sorted(dir.glob("*.in")) if not src.with_suffix(".fail").exists() and not src.with_suffix(".skip").exists()
		)
	checked: int = len(sources)
	failures: list[str] = mutation_self_check()
	# テストケース毎の観測は互いに独立な為，同時に走らせる工程の数 (`SHAVEFMT_JOBS`) で分ける（結果はテストケースの順に集める）
	with concurrent.futures.ProcessPoolExecutor(max_workers=JOBS) as pool:
		for (
			src,
			problems
		) in zip(sources, pool.map(inspect, itertools.repeat(binary), sources, [src.parent.name for src in sources], chunksize=8)):
			failures += [f"{src.relative_to(ROOT)}: {problem}" for problem in problems]
	# テストケースの違反の報告
	for failure in failures:
		print(f"FAIL comment separation: {failure}", file=sys.stderr)
	print(f"comment separation: {checked} cases, {len(failures)} failed")
	# 内容差のテストケースの照合と報告
	contents: int
	skipped: int
	problems: list[str]
	contents, skipped, problems = content_problems(binary)
	embedded_count: int
	embedded_failures: list[str]
	embedded_count, embedded_failures = embedded_problems(binary)
	problems += embedded_failures
	contents += embedded_count
	for problem in problems:
		print(f"FAIL comment content: {problem}", file=sys.stderr)
	print(f"comment content: {contents} cases, {len(problems)} failed, {skipped} skipped")
	# 違反が有れば非零の返戻
	return 1 if failures or problems else 0

if __name__ == "__main__":
	sys.exit(main())
