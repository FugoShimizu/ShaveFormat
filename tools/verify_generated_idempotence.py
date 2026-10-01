#! /usr/bin/env python3
"""組み立てた入力での冪等性（整形仕様 1.3）と内容の保全（整形仕様 7.3）の検査

基準検体は人が書いた物しか見ない為，検体に無い形の非冪等と内容の消失は素通りする
此処では本文の型と包み方を掛け合わせて入力を組み立て，整形を二度掛けて一致を確かめる
冪等でも内容が消える事は有る為，本文を失った囲みコメントと，消えた先頭の記号も併せて見る
複数の論理行の構造（段落・引用・箇条・コード例・空の論理行）は記法を変えて包み，記法の変換を経ても読みが変わらずコード例の字面が残るかを見る（整形仕様 3.3, 7.3）
見送り（原文返戻）も，見送りの理由が無い検体の見送りと同じく失敗として扱う
"""

import itertools
import subprocess
import sys

from verify_case_variants import depth_mismatches, regions

# 文末の記号を含む本文（連続と単独の双方，記号だけの行を作らないかを見る）
SENTENCES: tuple[str, ...] = (
	"a? and b! stay",
	"。",
	"。あ",
	"あ。 ？い",
	"あ。。い",
	"あ。い",
	"あ！い",
	"あ！！",
	"あ！？い",
	"あ？",
	"あ？い",
	"！！！あ",
	"（あ？）い",
	"？",
	"？あ"
)

# 言語毎の，空の本体と其の包み方（`{0}` へコメントを差し込む，json はコメントを書けない為に対象外）
# 各言語の雛形の番号を FULL_SHAPES が参照する為に内側の順を保つ
SHAPES: dict[str, tuple[str, ...]] = {
	"c": (
		"void f(void) {{\n\t// {0}\n\tint x = 1;\n}}\n",
		"void g(void) {{\n\t// {0}\n}}\n",
		"int h(void) {{\n\tif(\n\t\t1\n\t\t// {0}\n\t) return 1;\n\treturn 0;\n}}\n",
		"void i(void) {{\n\tint x = 1; // {0}\n\tg(x);\n}}\n",
		"void j(void) {{\n\tint x = 1; /* {0} */\n\tg(x);\n}}\n"
	),
	"cpp":
	("void f() {{\n\t// {0}\n\tint x = 1;\n}}\n", "struct S {{\n\t// {0}\n}};\n", "/* {0} */\nint g() {{\n\treturn 1;\n}}\n"),
	"csharp": ("class K {{\n\tvoid F() {{\n\t\t// {0}\n\t\tint x = 1;\n\t}}\n}}\n",),
	"java": ("class K {{\n\tvoid f() {{\n\t\t// {0}\n\t\tint x = 1;\n\t}}\n}}\n",),
	"go": ("package p\n\nfunc f() {{\n\t// {0}\n\tx := 1\n\t_ = x\n}}\n", "package p\n\nfunc g() {{\n\t// {0}\n}}\n"),
	"rust": ("fn f() {{\n\t// {0}\n\tlet x = 1;\n}}\n", "fn g() {{\n\t// {0}\n}}\n"),
	"kotlin": ("fun f() {{\n\t// {0}\n\tval x = 1\n}}\n", "fun g() {{\n\t// {0}\n}}\n"),
	"swift": ("func f() {{\n\t// {0}\n\tlet x = 1\n}}\n", "func g() {{\n\t// {0}\n}}\n"),
	"php": ("<?php\nfunction f() {{\n\t// {0}\n\t$x = 1;\n}}\n",),
	"javascript": (
		"function f() {{\n\t// {0}\n\tlet x = 1;\n}}\n",
		"function g() {{\n\t// {0}\n}}\n",
		"if (a) {{\n\t// {0}\n}} else {{\n\tb();\n}}\n",
		"function i() {{\n\tlet x = 1; // {0}\n\treturn x;\n}}\n"
	),
	"javascriptreact": (
		"const f = () => (\n\t<div>\n\t\t{{/* {0} */}}\n\t\t<p>x</p>\n\t</div>\n);\n",
		"function g() {{\n\t// {0}\n\treturn null;\n}}\n"
	),
	"typescript": ("function f(): void {{\n\t// {0}\n\tconst x = 1;\n}}\n", "interface I {{\n\t// {0}\n}}\n"),
	"typescriptreact": ("const f = (): JSX.Element => (\n\t<div>\n\t\t{{/* {0} */}}\n\t\t<p>x</p>\n\t</div>\n);\n",),
	"ruby": (
		"def f\n\t# {0}\n\tx = 1\nend\n",
		"def g(a)\n\t# {0}\nend\n",
		"R = (def m(a)\n\t# {0}\nend)\n",
		"begin\n\t# {0}\nend\n",
		"class A\n\t# {0}\nend\n",
		"def i\n\tx = 1 # {0}\n\tx\nend\n"
	),
	"python": ("def f():\n\t# {0}\n\tx = 1\n", "class A:\n\t# {0}\n\tpass\n", "def i():\n\tx = 1 # {0}\n\treturn x\n"),
	"jsonc": ("{{\n\t// {0}\n\t\"a\": 1\n}}\n", "{{\n\t\"a\": 1 // {0}\n}}\n"),
	"html": ("<div>\n\t<!-- {0} -->\n\t<p>x</p>\n</div>\n",),
	"css": ("a {{\n\t/* {0} */\n\tcolor: red;\n}}\n", "/* {0} */\na {{\n\tcolor: red;\n}}\n"),
	"scss": ("a {{\n\t// {0}\n\tcolor: red;\n}}\n", ".b {{\n\t// {0}\n}}\n")
}

# 全ての本文を通す形（文の区切の規則は言語に依らない為，記法の異なる代表の形だけで全ての本文を確かめる）
FULL_SHAPES: set[tuple[str, int]] = {
	("c", 0),
	("c", 3),
	("c", 4),
	("javascriptreact", 0),
	("ruby", 0),
	("python", 2),
	("html", 0),
	("css", 0)
}

# 其の他の形へ通す本文（配置は本文の論理行の数と頭の記号だけで変わる為，其の形の異なる物に限る）
SAMPLE_SENTENCES: tuple[str, ...] = "a? and b! stay", "。あ", "あ。い"
# 本文を失った囲みコメントの形（囲みは空に為らない為，現れたら内容が消えて居る）
# 行コメントの `//` `#` は文書化の空行として正当な為に入れない
EMPTY_COMMENTS: tuple[str, ...] = "/* */", "/**/", "<!-- -->", "<!---->"
# 文末の記号（手前に文が無ければ文末ではない為，除かれも分けられもしない）
SENTENCE_MARKS: tuple[str, ...] = "。", "！", "．", "？"
# 掛け合わせる行幅（狭い幅は行分割を通し，別の配置経路を踏む）
WIDTHS: tuple[int, ...] = 0, 40

# 複数の論理行の構造と，出力に字面の儘残るべきコード例（説明文として読む物は無し）と，囲み・要素でコード例を示す書式だけのコード例か
# 記号の後の字下げは区切空白と為り説明文として読む（整形仕様 7.6），引用の記号の後の字下げは本文の途中の為にコード例として読む
STRUCTURES: tuple[tuple[tuple[str, ...], str|None, bool], ...] = (
	(("`x` is used", "", "    a  =  b"), None, False),
	(("\"q\" is used", "", "    a  =  b"), None, False),
	(("> Para", "", ">     a  =  b"), "a  =  b", False),
	(("Para", "", "    a  =  b", "", "    c  =  d"), None, False),
	(("- item", "", "      a  =  b"), None, False),
	(("```", "a  =  b", "", "c  =  d", "```"), "c  =  d", True),
	(("<pre>", "a  =  b", "", "c  =  d", "</pre>"), "c  =  d", True),
	(("    First. Second",), None, False)
)

# 言語毎の構造の包み方（`{0}` へ包んだコメントを差し込む）と，使う記法（行コメントの記号，囲みの有無，埋込文書の有無）と，囲み・要素でコード例を示すか（godoc は字下げだけ）と，文書でない位置の文書の行記法
STRUCTURE_SHAPES: dict[str, tuple[str, str, bool, bool, bool, str]] = {
	"c": ("int x;\n{0}\nint y;\n", "//", True, False, True, ""),
	"cpp": ("void f() {{\n\tg();\n{0}\n\th();\n}}\n", "//", True, False, True, "///"),
	"go": ("package p\n\n{0}\nvar x = 1\n", "//", True, False, False, ""),
	"javascript": ("function f() {{\n\tg();\n{0}\n\th();\n}}\n", "//", True, False, True, ""),
	"ruby": ("x = 1\n{0}\ny = 2\n", "#", False, True, True, "#:"),
	"python": ("x = 1\n{0}\ny = 2\n", "#", False, False, True, ""),
	"css": ("a {{\n\tcolor: red;\n}}\n{0}\nb {{\n\tcolor: blue;\n}}\n", "", True, False, True, "")
}

def run(binary: str, language: str, width: int, text: str) -> tuple[str, bool, int]:
	"""整形を一度掛け，結果と見送りの有無と終了コードを返す
	計算量：入力の文字数 L・整形器の実行時間 T に対して O(L+T)
	"""
	# 整形器の起動
	command: tuple[str, ...] = binary, "--stdin", "--lang", language, "--chars", str(width), "--no-lint"
	result: subprocess.CompletedProcess[str] = subprocess.run(command, input=text, capture_output=True, text=True)
	# 整形結果と，見送りの有無と，終了状態の返戻
	return result.stdout, "[skip]" in result.stderr, result.returncode

def leading_marks(sentence: str) -> str:
	"""本文の手前に在る文末の記号の並びを返す
	計算量：本文の文字数 L に対して O(L)
	"""
	# 先頭の文末記号の走査
	end: int = 0
	while sentence[end:end + 1] in SENTENCE_MARKS:
		end += 1
	# 先頭の記号の並びの返戻
	return sentence[:end]

def degradation(sentence: str, text: str) -> str:
	"""内容を失った出力の理由を返す（正常なら空文字列）
	計算量：出力の文字数 L に対して O(L)
	"""
	# 空に為った囲みコメントの検出
	for form in EMPTY_COMMENTS:
		if form in text:
			# 本文を失った囲みコメントの返戻
			return f"empty comment {form}"
	# 手前に文を持たない記号は文末ではない為，除かれず其の儘残る（整形仕様 7.3）
	if (marks := leading_marks(sentence)) and marks not in text:
		# 消えた先頭の記号の返戻
		return f"leading marks {marks} were removed"
	# 内容を失って居ない事の返戻
	return ""

def wrappings(lines: tuple[str, ...], marker: str, has_block: bool, has_embedded: bool, document_marker: str) -> list[str]:
	"""構造を包んだコメントの字面を記法毎に返す
	行コメント・囲み・埋込文書を扱い，文書でない位置の文書記法も含める
	計算量：論理行の総文字数 L に対して O(L)
	"""
	# 行コメントと囲みコメントの生成
	found: list[str] = []
	if marker:
		found.append("\n".join(f"{marker} {line}" if line else marker for line in lines))
	if has_block:
		found.append("/*\n" + "\n".join(f" * {line}" if line else " *" for line in lines) + "\n */")
		found.append("/*\n" + "\n".join(f"\t{line}" if line else "" for line in lines) + "\n*/")
		found.append("/**\n" + "\n".join(f" * {line}" if line else " *" for line in lines) + "\n */")
	# 文書記法の生成
	if document_marker:
		found.append("\n".join(f"{document_marker} {line}" if line else document_marker for line in lines))
		# 字で始まる本文を記号へ密着させた形（別の記法・登録形に為らない限り密着を保って行記法へ揃える）
		# 字下げの行は記号の後の空白が全て区切空白と為り，前の成員の文書の頭 `<` を密着させると別の文書に為る為，何れも区切空白を残す
		found.append(
			"\n".join(f"{document_marker}{line}" if line[:1] not in " <" else f"{document_marker} {line}".rstrip() for line in lines)
		)
	# 埋込文書と連結した段落の生成
	if has_embedded:
		found.append("=begin\n" + "\n".join(lines) + "\n=end")
	# 行コメントへ揃える記法の前に行コメントの段落が続く形（出力で行コメントの群と合わさる）
	if marker:
		found += [f"{marker} Para\n{comment}" for comment in found[1:]]
	# 記法毎の字面の返戻
	return found

def check_structures(binary: str) -> tuple[int, int]:
	"""複数の論理行の構造を記法を変えて包み，冪等性とコード例の字面の保持を確かめる
	計算量：生成検体数 F・整形器の最大実行時間 T に対して O(FT)
	"""
	# 構造と記法と行幅の組合せの検査
	total: int = 0
	failed: int = 0
	for language, (shape, marker, has_block, has_embedded, has_markup_code, document_marker) in STRUCTURE_SHAPES.items():
		for (lines, code, needs_markup), width in itertools.product(STRUCTURES, WIDTHS):
			# 囲み・要素を持たない書式では其の構造を説明文として読む
			code: str|None = code if has_markup_code or not needs_markup else None
			for comment in wrappings(lines, marker, has_block, has_embedded, document_marker):
				total += 1
				first: str
				is_skipped: bool
				status: int
				# 初回整形と見送の確認
				first, is_skipped, status = run(binary, language, width, shape.format(comment))
				if status or is_skipped:
					print(f"FAIL generated structure: {language} w={width} skipped or non-zero:\n{comment}", file=sys.stderr)
					failed += 1
					continue
				if problems := depth_mismatches(first, regions(first, language)):
					print(f"FAIL generated structure (depth): {language} w={width}: {problems[0]}\n{comment}", file=sys.stderr)
					failed += 1
					continue
				# コード例の本文の保持確認
				if code and code not in first:
					print(f"FAIL generated structure (content): {language} w={width} lost `{code}`:\n{comment}", file=sys.stderr)
					failed += 1
					continue
				# 再整形での冪等性の確認
				second: str
				second, is_skipped, status = run(binary, language, width, first)
				if status or is_skipped or first != second:
					print(f"FAIL generated structure (idempotence): {language} w={width}:\n{comment}", file=sys.stderr)
					for line in (first or "").split("\n"):
						print(f"  1st| {line}", file=sys.stderr)
					for line in (second or "").split("\n"):
						print(f"  2nd| {line}", file=sys.stderr)
					failed += 1
	# 検体の数と失敗の数の返戻
	return total, failed

def main() -> int:
	"""組み立てた入力の冪等性と退化した出力を確かめ，違反が有れば非零で終える
	計算量：生成検体数 F・整形器の最大実行時間 T に対して O(FT)
	"""
	# 整形器と構造検体の準備
	binary: str = sys.argv[1] if len(sys.argv) > 1 else "build/shavefmt"
	total: int
	failed: int
	total, failed = check_structures(binary)
	# 文末記号と幅の組合せの検査
	for language, shapes in SHAPES.items():
		for index, shape in enumerate(shapes):
			sentences: tuple[str, ...] = SENTENCES if (language, index) in FULL_SHAPES else SAMPLE_SENTENCES
			for sentence, width in itertools.product(sentences, WIDTHS):
				total += 1
				first: str
				is_skipped: bool
				status: int
				# 初回整形と見送の確認
				first, is_skipped, status = run(binary, language, width, shape.format(sentence))
				if status or is_skipped:
					print(f"FAIL generated: {language} w={width} skipped or non-zero: {sentence}", file=sys.stderr)
					failed += 1
					continue
				if problems := depth_mismatches(first, regions(first, language)):
					print(f"FAIL generated (depth): {language} w={width} {sentence}: {problems[0]}", file=sys.stderr)
					failed += 1
					continue
				# コメントが内容を失って居ないかの確認（冪等では捕まらない軸）
				if reason := degradation(sentence, first):
					print(f"FAIL generated (content): {language} w={width} {sentence}: {reason}", file=sys.stderr)
					failed += 1
					continue
				# 再整形での冪等性の確認
				second: str
				second, is_skipped, status = run(binary, language, width, first)
				if status or is_skipped or first != second:
					print(f"FAIL generated (idempotence): {language} w={width} {sentence}", file=sys.stderr)
					for line in (first or "").split("\n"):
						print(f"  1st| {line}", file=sys.stderr)
					for line in (second or "").split("\n"):
						print(f"  2nd| {line}", file=sys.stderr)
					failed += 1
	print(f"generated idempotence: {total} cases, {failed} failed")
	# 違反が有れば非零の返戻
	return 1 if failed else 0

if __name__ == "__main__":
	sys.exit(main())
