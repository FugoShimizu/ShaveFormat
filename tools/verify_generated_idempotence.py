#! /usr/bin/env python3
"""組み立てた入力での冪等性（整形仕様 1.3）と内容の保全（整形仕様 7.3）の検査

基準テストケースは人が書いた物しか見ない為，テストケースに無い形の非冪等と内容の消失は素通りする
此処では本文の型と包方を掛け合わせて入力を組み立て，整形を２度掛けて一致を確かめる
冪等でも内容が消える事は有る為，本文を失ったブロックコメントと，消えた先頭の記号も併せて見る
複数の論理行の構造（段落・引用・箇条・コード例・空の論理行）は記法を変えて包み，記法の変換を経ても読みが変わらずコード例のテキストが残るかを見る（整形仕様 3.3, 7.3）
見送（原文返戻）も，見送の理由が無いテストケースの見送と同じく失敗として扱う
"""

from verify_case_variants import Region, depth_mismatches, logical_lines, regions

import itertools
import re
import subprocess
import sys

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
# 形の添字を `FULL_SHAPES`・`LEADING_SHAPES`・`MERGED_SHAPES` で参照する為，内側の順を保持
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
	"javascriptreact": (
		"const f = () => (\n\t<div>\n\t\t{{/* {0} */}}\n\t\t<p>x</p>\n\t</div>\n);\n",
		"function g() {{\n\t// {0}\n\treturn null;\n}}\n"
	),
	"ruby": (
		"def f\n\t# {0}\n\tx = 1\nend\n",
		"def g(a)\n\t# {0}\nend\n",
		"R = (def m(a)\n\t# {0}\nend)\n",
		"begin\n\t# {0}\nend\n",
		"class A\n\t# {0}\nend\n",
		"def i\n\tx = 1 # {0}\n\tx\nend\n"
	),
	"python": ("def f():\n\t# {0}\n\tx = 1\n", "class A:\n\t# {0}\n\tpass\n", "def i():\n\tx = 1 # {0}\n\treturn x\n"),
	"html": ("<div>\n\t<!-- {0} -->\n\t<p>x</p>\n</div>\n",),
	"css": ("a {{\n\t/* {0} */\n\tcolor: red;\n}}\n", "/* {0} */\na {{\n\tcolor: red;\n}}\n"),
	"scss": ("a {{\n\t// {0}\n\tcolor: red;\n}}\n", ".b {{\n\t// {0}\n}}\n")
}

# 独立した宣言又は規則として本文違いを同じ入力へ纏める形
MERGED_SHAPES: dict[str, tuple[int, ...]] = {
	"c": (0, 1, 2, 3, 4),
	"cpp": (0, 1, 2),
	"javascriptreact": (1,),
	"ruby": (0, 1, 4, 5),
	"python": (0, 1, 2),
	"css": (0, 1),
	"scss": (0, 1)
}

# 空の本体の終端は見出と同行に為る為，独立コメントは其の行の前へ置かれる
# 前置本文は直前の宣言名から今回の宣言名迄を照合する
LEADING_SHAPES: set[tuple[str, int]] = { ("c", 1), ("cpp", 1), ("cpp", 2), ("ruby", 1), ("ruby", 4), ("css", 1), ("scss", 1) }

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
# 本文を失ったブロックコメントの形（囲みは空に為らない為，現れたら内容が消えて居る）
# 行コメントの `//` `#` はドキュメント化の空行として正当な為に入れない
EMPTY_COMMENTS: tuple[str, ...] = "/* */", "/**/", "<!-- -->", "<!---->"
# 本文頭でも保持する疑問・感嘆の記号（句点は整形仕様 7.3 に従って除く）
SENTENCE_MARKS: tuple[str, ...] = "!", "?", "！", "？"
# 掛け合わせる行幅（狭い幅は行分割を通し，別の配置経路を踏む）
WIDTHS: tuple[int, ...] = 0, 40

# 複数の論理行の構造と，出力にテキストの儘残るべきコード例（説明文として読む物は無）と，囲み・要素でコード例を示す書式だけのコード例か
# 記号の後のインデントは区切空白と為り説明文として読む（整形仕様 7.6），引用等の軽量記法は構造化せず説明文として読む（整形仕様 7.3）
STRUCTURES: tuple[tuple[tuple[str, ...], str|None, bool], ...] = (
	(("Para", "", "    a  =  b", "", "    c  =  d"), None, False),
	(("```", "a  =  b", "", "c  =  d", "```"), "c  =  d", True),
	(("<pre>", "a  =  b", "", "c  =  d", "</pre>"), "c  =  d", True)
)

# 言語毎の構造の包み方（`{0}` へ包んだコメントを差し込む）と，使う記法（行コメントの記号，囲みの有無，埋込ドキュメントの有無）と，囲み・要素でコード例を示すか（godoc はインデントだけ）と，ドキュメントでない位置のドキュメントの行記法
STRUCTURE_SHAPES: dict[str, tuple[str, str, bool, bool, bool, str]] = {
	"c": ("int x;\n{0}\nint y;\n", "//", True, False, True, ""),
	"cpp": ("void f() {{\n\tg();\n{0}\n\th();\n}}\n", "//", True, False, True, "///"),
	"go": ("package p\n\n{0}\nvar x = 1\n", "//", True, False, False, ""),
	"ruby": ("x = 1\n{0}\ny = 2\n", "#", False, True, True, "#:"),
	"python": ("x = 1\n{0}\ny = 2\n", "#", False, False, True, ""),
	"css": ("a {{\n\tcolor: red;\n}}\n{0}\nb {{\n\tcolor: blue;\n}}\n", "", True, False, True, "")
}

def run(binary: str, text: str, lang: str, width: int) -> tuple[str, bool, int]:
	"""整形を１度掛け，結果と見送の有無と終了コードを返す
	計算量：入力の文字数 L・フォーマッターの実行時間 T に対して O(L+T)
	引数：
		binary: フォーマッターの実行ファイル
		text: 照合又は解析するテキスト
		lang: テストケースの言語
		width: 整形の行幅
	戻値：
		整形結果・見送の有無・終了値（時間切れは１２４）
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
	"""
	# フォーマッターの起動
	command: tuple[str, ...] = binary, "--stdin", "--lang", lang, "--chars", str(width), "--no-lint"
	try:
		result: subprocess.CompletedProcess[str] = subprocess.run(command, input=text, capture_output=True, text=True, timeout=30)
	except subprocess.TimeoutExpired:
		print(f"FAIL generated: {lang} w={width}: timed out after 30s", file=sys.stderr)
		# 時間切れに為った整形の結果の返戻
		return "", False, 124
	# 整形結果と，見送の有無と，終了状態の返戻
	return result.stdout, "[skip]" in result.stderr, result.returncode

def leading_marks(sentence: str) -> str:
	"""本文の手前に在る文末の記号の並びを返す
	計算量：本文の文字数 L に対して O(L)
	引数：
		sentence: 元の論理行の本文
	戻値：
		本文の手前に在る文末記号の並び
	"""
	# 句点除去後の先頭で，保持する疑問・感嘆の記号だけを識別する
	lines: list[str] = logical_lines(sentence)
	sentence = lines[0] if lines else ""
	end: int = 0
	while sentence[end:end + 1] in SENTENCE_MARKS:
		end += 1
	# 先頭の記号の並びの返戻
	return sentence[:end]

def degradation(text: str, sentence: str) -> str:
	"""内容を失った出力の理由を返す（正常なら空文字列）
	計算量：出力の文字数 L に対して O(L)
	引数：
		text: 照合又は解析するテキスト
		sentence: 元の論理行の本文
	戻値：
		退化の理由（正常なら空文字列）
	"""
	# 空に為ったブロックコメントの検出
	for form in EMPTY_COMMENTS:
		if form in text:
			# 本文を失ったブロックコメントの返戻
			return f"empty comment {form}"
	# 手前に文を持たない疑問・感嘆の記号も，其の儘残る（整形仕様 7.3）
	if (marks := leading_marks(sentence)) and marks not in text:
		# 消えた先頭の記号の返戻
		return f"leading marks {marks} were removed"
	# 内容を失って居ない事の返戻
	return ""

def grouped_input(sentences: tuple[str, ...], lang: str, shape: str) -> tuple[str, tuple[str, ...]]:
	"""本文違いを独立した名前の宣言へ入れ，同一幅で検査する入力を返す
	計算量：全テストケースの総文字数 L に対して O(L)
	引数：
		sentences: 個別に保持を照合する本文
		lang: テストケースの言語
		shape: 独立宣言の雛形
	戻値：
		連結した入力と，本文の所在を区切る一意な宣言名
	送出例外：
		ValueError: 雛形に独立した宣言名が無い場合
	"""
	# 宣言名だけを変え，本文・ローカル名・式の内容を保持する
	parts: list[str] = []
	names: list[str] = []
	for idx, sentence in enumerate(sentences):
		text: str = shape.format(sentence)
		pattern: str = (
			r"(?m)^\.?([ab])\s*\{" if
			lang in ("css", "scss") else
			r"\b(?:void|int|struct|class|func|fn|fun|function|interface|def)\s+([A-Za-z_]\w*)"
		)
		match: re.Match[str]|None = re.search(pattern, text)
		if not match:
			raise ValueError(f"independent declaration not found: {lang}: {shape}")
		name: str = match[1] + str(idx)
		names.append(name)
		text = text[:match.start(1)] + name + text[match.end(1):]
		# package と開始記号はファイルの先頭に１回だけ置く
		if idx and lang == "go":
			text = text.removeprefix("package p\n\n")
		if idx and lang == "php":
			text = text.removeprefix("<?php\n")
		parts.append(text)
	# 全ての独立宣言と対応する名前の返戻
	return "\n".join(parts), tuple(names)

def grouped_degradation(
	parsed: list[Region],
	text: str,
	sentences: tuple[str, ...],
	names: tuple[str, ...],
	is_leading: bool
) -> str:
	"""他の宣言の本文で消失を隠さず，各宣言の本文を個別に照合する
	計算量：本文数 N・出力文字数 L に対して O(NL)
	引数：
		parsed: 深さ検査と共有する字句解析済の領域
		text: 整形後の全文
		sentences: 宣言毎の元の本文
		names: 入力順の一意な宣言名
		is_leading: 本文が宣言名より前に在るか
	戻値：
		宣言の欠落・重複・順序変更又は本文の退化の理由，正常なら空文字列
	"""
	# ドキュメントに補われる宣言名を数えず，コードの識別子の位置だけを照合する
	starts: list[int] = [0]
	for line in text.splitlines(keepends=True):
		starts.append(starts[-1] + len(line))
	locations: dict[str, list[tuple[int, int]]] = { name: [] for name in names }
	for region in parsed:
		for token in region.tokens:
			if not token.is_comment and token.text in locations:
				start: int = starts[token.line] + token.col
				locations[token.text].append((start, start + len(token.text)))
	# 各宣言が１回ずつ同じ順で残る事の確認
	positions: list[tuple[int, int]] = []
	for name in names:
		matches: list[tuple[int, int]] = locations[name]
		if len(matches) != 1 or positions and matches[0][0] <= positions[-1][0]:
			# 宣言の対応を失った理由の返戻
			return f"declaration {name} was lost, duplicated or reordered"
		positions.append(matches[0])
	# 前置本文は前の宣言名との間，本体内の本文は次の宣言名との間に限定する
	for idx, sentence in enumerate(sentences):
		start: int = (positions[idx - 1][1] if idx else 0) if is_leading else positions[idx][1]
		end: int = positions[idx][0] if is_leading else positions[idx + 1][0] if idx + 1 < len(names) else len(text)
		if reason := degradation(text[start:end], sentence):
			# 当該宣言の本文の退化理由の返戻
			return f"{names[idx]}: {reason}"
	# 個別の照合で退化が無い事の返戻
	return ""

def wrappings(lines: tuple[str, ...], marker: str, has_block: bool, has_embedded: bool, document_marker: str) -> list[str]:
	"""構造を包んだコメントのテキストを記法毎に返す
	行コメント・囲み・埋込ドキュメントを扱い，ドキュメントでない位置のドキュメント記法も含める
	計算量：論理行の総文字数 L に対して O(L)
	引数：
		lines: 検査する行の並び
		marker: コメントの開始記号
		has_block: ブロックコメントを使えるか
		has_embedded: 埋込ドキュメントを使えるか
		document_marker: ドキュメントの開始記号
	戻値：
		指定のコメント記法で包んだテキストの一覧
	"""
	# 行コメントとブロックコメントの生成
	found: list[str] = []
	if marker:
		found.append("\n".join(f"{marker} {line}" if line else marker for line in lines))
	if has_block:
		found.append("/*\n" + "\n".join(f" * {line}" if line else " *" for line in lines) + "\n */")
		found.append("/**\n" + "\n".join(f" * {line}" if line else " *" for line in lines) + "\n */")
	# ドキュメント記法の生成
	if document_marker:
		found.append("\n".join(f"{document_marker} {line}" if line else document_marker for line in lines))
		# 字で始まる本文を記号へ密着させた形（別の記法・登録形に為らない限り密着を保って行記法へ揃える）
		# インデントの行は記号の後の空白が全て区切空白と為り，前のメンバのドキュメントの頭 `<` を密着させると別のドキュメントに為る為，何れも区切空白を残す
		found.append(
			"\n".join(f"{document_marker}{line}" if line[:1] not in " <" else f"{document_marker} {line}".rstrip() for line in lines)
		)
	# 埋込ドキュメントと連結した段落の生成
	if has_embedded:
		found.append("=begin\n" + "\n".join(lines) + "\n=end")
	# 行コメントへ揃える記法の前に行コメントの段落が続く形（出力で行コメントのグループと合わさる）
	if marker:
		found += [f"{marker} Para\n{comment}" for comment in found[1:]]
	# 記法毎のテキストの返戻
	return found

def check_structures(binary: str) -> tuple[int, int]:
	"""複数の論理行の構造を記法を変えて包み，冪等性とコード例のテキストの保持を確かめる
	計算量：生成テストケース数 F・フォーマッターの最大実行時間 T に対して O(FT)
	引数：
		binary: フォーマッターの実行ファイル
	戻値：
		検査した数と失敗した数
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
	"""
	# 構造と記法と行幅の組合せの検査
	total: int = 0
	failed: int = 0
	for lang, (shape, marker, has_block, has_embedded, has_markup_code, document_marker) in STRUCTURE_SHAPES.items():
		for (lines, code, should_use_markup), width in itertools.product(STRUCTURES, WIDTHS):
			# 囲み・要素を持たない書式では其の構造を説明文として読む
			code: str|None = code if has_markup_code or not should_use_markup else None
			for comment in wrappings(lines, marker, has_block, has_embedded, document_marker):
				total += 1
				first: str
				is_skipped: bool
				status: int
				# 初回整形と見送の確認
				first, is_skipped, status = run(binary, shape.format(comment), lang, width)
				if status or is_skipped:
					print(f"FAIL generated structure: {lang} w={width} skipped or non-zero:\n{comment}", file=sys.stderr)
					failed += 1
					continue
				if problems := depth_mismatches(regions(first, lang), first):
					print(f"FAIL generated structure (depth): {lang} w={width}: {problems[0]}\n{comment}", file=sys.stderr)
					failed += 1
					continue
				# コード例の本文の保持確認
				if code and code not in first:
					print(f"FAIL generated structure (content): {lang} w={width} lost `{code}`:\n{comment}", file=sys.stderr)
					failed += 1
					continue
				# 再整形での冪等性の確認
				second: str
				second, is_skipped, status = run(binary, first, lang, width)
				if status or is_skipped or first != second:
					print(f"FAIL generated structure (idempotence): {lang} w={width}:\n{comment}", file=sys.stderr)
					for line in (first or "").split("\n"):
						print(f"  1st| {line}", file=sys.stderr)
					for line in (second or "").split("\n"):
						print(f"  2nd| {line}", file=sys.stderr)
					failed += 1
	# テストケースの数と失敗の数の返戻
	return total, failed

def self_check() -> list[str]:
	"""句点の除去を許し，疑問・感嘆の消失を拒む正誤対を照合する
	計算量：正誤対の総文字数 B に対して O(B)
	戻値：
		条文と食い違う期待の一覧
	"""
	# 本文頭の句点の除去と，句点除去後にも残る記号の保持（整形仕様 7.3）
	found: list[str] = []
	for sentence, text, has_expected_degradation in (
		("。読む", "// 読む", False),
		("．読む", "// 読む", False),
		("。？読む", "// ？読む", False),
		("。？読む", "// 読む", True),
		("？！読む", "// ？！読む", False),
		("？！読む", "// 読む", True),
		("?! Read", "// ?! Read", False),
		("?! Read", "// Read", True)
	):
		if bool(degradation(text, sentence)) != has_expected_degradation:
			found.append(f"本文頭の記号の保持が条文と違う：{sentence!r}")
	# 正誤対で見付かった食違の返戻
	return found

def main() -> int:
	"""組み立てた入力の冪等性と退化した出力を確かめ，違反が有れば非零で終える
	計算量：生成テストケース数 F・フォーマッターの最大実行時間 T に対して O(FT)
	戻値：
		成功なら０，違反又は失敗が有れば非零の終了値
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		ValueError: 統合する雛形に独立した宣言名が無い場合
	"""
	# フォーマッターの結果に依存しない，条文からの期待の照合
	if problems := self_check():
		for problem in problems:
			print(f"FAIL self_check: {problem}", file=sys.stderr)
		# 条文との食違を示す終了値の返戻
		return 1
	# フォーマッターと構造テストケースの準備
	binary: str = sys.argv[1] if len(sys.argv) > 1 else "build/shavefmt"
	total: int
	failed: int
	total, failed = check_structures(binary)
	# 文末記号と幅の組合せの検査
	for lang, shapes in SHAPES.items():
		for idx, shape in enumerate(shapes):
			sentences: tuple[str, ...] = SENTENCES if (lang, idx) in FULL_SHAPES else SAMPLE_SENTENCES
			is_merged: bool = idx in MERGED_SHAPES.get(lang, ())
			is_leading: bool = (lang, idx) in LEADING_SHAPES
			groups: tuple[tuple[str, ...], ...] = (sentences,) if is_merged else tuple((item,) for item in sentences)
			for group, width in itertools.product(groups, WIDTHS):
				sentence: str = " / ".join(group)
				names: tuple[str, ...] = ()
				text: str
				if is_merged:
					text, names = grouped_input(group, lang, shape)
				else:
					text = shape.format(group[0])
				total += 1
				first: str
				is_skipped: bool
				status: int
				# 初回整形と見送の確認
				first, is_skipped, status = run(binary, text, lang, width)
				if status or is_skipped:
					print(f"FAIL generated: {lang} w={width} skipped or non-zero: {sentence}", file=sys.stderr)
					failed += 1
					continue
				parsed: list[Region] = regions(first, lang)
				if problems := depth_mismatches(parsed, first):
					print(f"FAIL generated (depth): {lang} w={width} {sentence}: {problems[0]}", file=sys.stderr)
					failed += 1
					continue
				# コメントが内容を失って居ないかの確認（冪等では捕まらない軸）
				reason: str = grouped_degradation(parsed, first, group, names, is_leading) if is_merged else degradation(first, group[0])
				if reason:
					print(f"FAIL generated (content): {lang} w={width} {sentence}: {reason}", file=sys.stderr)
					failed += 1
					continue
				# 再整形での冪等性の確認
				second: str
				second, is_skipped, status = run(binary, first, lang, width)
				if status or is_skipped or first != second:
					print(f"FAIL generated (idempotence): {lang} w={width} {sentence}", file=sys.stderr)
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
