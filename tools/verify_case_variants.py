#! /usr/bin/env python3
"""検体の変種での性質の検査（整形仕様 1.3, 1.5, 7.6, 10.4）

`golden_test.sh` は検体毎に定めた一つの幅と原文の儘の入力でしか整形しない
此処では整形を拒む検体 (`.fail` / `.skip`) を除く各検体を分割の無い幅と複数の狭い幅で整形し，次を確かめる
何れの幅でも整形を拒まれず見送られない（正常な入力の整形の拒否は整形仕様 1.5 に反する）
二巡目が一巡目と一致する（冪等性は行幅に依らず成り立つ，整形仕様 1.3）
各行幅の整形結果と期待する出力 (`.out`) で，独立コメントが後続の文を錨とする規則を満たす（整形仕様 7.6）
深さは後続のコード行（閉じ括弧・節の見出を含む）の深さに一致し，後続が無ければ最上位（埋込の区画は其の区画の最上位）の深さに在り，タブで字下げする
原文でコメントの後に在った字句を追い越さず，原文でコメントの前に在った字句を持つ後の行より前へ遡らない
行頭へ置けない行（JSX の要素の中・Rust のマクロの字句列の中）が錨の行なら，其の区画の始まる行の前を許す（整形仕様 7.6, 7.7）
文書は定義の文書の同期（整形仕様 7.4）に従う為に錨の検査から除く
加えて各行の末尾へ空白とタブを足した入力でも，終了コードが契約（整形仕様 10.4 の `0` / `1` / `2`）に収まる事を確かめる
逐語の区画の空白だけの行は行頭の空白の数が行末を越える形に為り，位置の計算を符号無で行うと反転する

使い方：
python3 tools/verify_case_variants.py [整形器]
"""

from __future__ import annotations

import concurrent.futures
import difflib
import itertools
import os

from pathlib import Path

import re
import subprocess
import sys

from typing import NamedTuple

ROOT: Path = Path(__file__).resolve().parent.parent
GOLDEN: Path = ROOT / "test_data/golden"
# 同時に走らせる検査の工程の数（`tools/golden_test.sh` と同じ `SHAVEFMT_JOBS`，既定は２）
JOBS: int = max(1, int(os.environ.get("SHAVEFMT_JOBS", "2")))
# 何も分割されない幅
WIDE: str = "1000000"
# 掃引する狭い行幅（検体毎の幅での冪等性は `golden_test.sh` が確かめる）
WIDTHS: tuple[str, ...] = "12", "40"

# 言語毎の独立コメントの開始の字面（行コメントの記号を先に置き，前処理指令・属性・私有名等の `#` をコメントと見做さない）
# `verify_comment_separation.py` も此の表から行コメントの記号を引く
OPENERS: dict[str, tuple[str, ...]] = {
	"c": ("//", "/*"),
	"cpp": ("//", "/*"),
	"h": ("//", "/*"),
	"csharp": ("//", "/*"),
	"java": ("//", "/*"),
	"go": ("//", "/*"),
	"rust": ("//", "/*"),
	"kotlin": ("//", "/*"),
	"swift": ("//", "/*"),
	"php": ("//", "/*", "# "),
	"javascript": ("//", "/*"),
	"javascriptreact": ("//", "/*"),
	"typescript": ("//", "/*"),
	"typescriptreact": ("//", "/*"),
	"ruby": ("#", "=begin"),
	"python": ("#",),
	"json": ("//", "/*"),
	"jsonc": ("//", "/*"),
	"html": ("<!--",),
	"css": ("/*",),
	"scss": ("//", "/*")
}

# HTML の埋込の区画の要素と中身の言語
EMBEDDED: dict[str, str] = { "script": "javascript", "style": "css" }
# 埋込の区画の開始の要素（`type` が JavaScript でない物は中身を JavaScript として読まない）
EMBEDDED_TAG: re.Pattern[str] = re.compile(r"<(script|style)\b([^>]*)>", re.I)

FOREIGN_TYPE: re.Pattern[str] = re.compile(
	r"\btype\s*=\s*(?![\"']?(?:(?:text|application)/(?:java|ecma)script|module)\b)",
	re.I
)

# 囲みの記法の開始と終端（終端迄の継続行はコードの行でない）
BLOCKS: dict[str, str] = { "/*": "*/", "<!--": "-->", "=begin": "=end" }
# 三重の引用符の文字列を持つ言語（中の記号をコメントと見做さない）
TRIPLE_QUOTE_LANGUAGES: tuple[str, ...] = "python",
# Unicode の空白 (Zs) を字句の間の空白と読む言語と，其の字
UNICODE_SPACE_LANGUAGES: tuple[str, ...] = "csharp", "javascript", "javascriptreact", "typescript", "typescriptreact"
UNICODE_SPACES: str = "\u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u202f\u205f\u3000"
# 字句の前に Unicode の逃避を読む言語（Java 言語仕様 3.3）
UNICODE_ESCAPE_LANGUAGES: tuple[str, ...] = "java",
UNICODE_ESCAPE: re.Pattern[str] = re.compile(r"\\u+([0-9A-Fa-f]{4})")
# ヒアドキュメント（開始の行の次の行から終端の語の行迄が本文）を持つ言語
HEREDOC_LANGUAGES: tuple[str, ...] = "ruby",
HEREDOC_START: re.Pattern[str] = re.compile(r"<<[~-]?(['\"`]?)([A-Za-z_]\w*)\1")
# 行頭（桁０）を必須とする記法（字下げしない為に深さを比べない，終端も行頭の物だけ）
COLUMN_ZERO: tuple[str, ...] = "=begin",
# 要素・文の区切り（正規化で増減する為に錨の照合に用いない）
SEPARATORS: tuple[str, ...] = ",", ";"
# 文書の開始の字面（括弧の中の物は定義の見出の文書として定義の文書へ同期する為に錨の検査から除く，整形仕様 7.4）
DOCUMENTS: tuple[str, ...] = "/*!", "/**", "//!", "///"
# 文書を通常のコメントと同じ記法で書く言語の，文書を付ける定義の語（括弧の中の見出のコメントは其の定義の文書へ同期する，整形仕様 7.4）
LINE_DOCUMENT_HEADS: dict[str, tuple[str, ...]] = { "go": ("const", "func", "type", "var"), "ruby": ("class", "def", "module") }
# 引用符の無い `url()` の引数を一つの字句とする言語（中の `//`・`/*` はコメントでない，字句間空白仕様 5.3）
URL_LANGUAGES: tuple[str, ...] = "css", "scss"
# JSX を書ける言語
MARKUP_LANGUAGES: tuple[str, ...] = "javascriptreact", "typescriptreact"
# JSX の要素を始める `<` の前に置ける語（其の他の語・閉じ括弧の後の `<` は比較か型引数）
MARKUP_LEADS: tuple[str, ...] = "await", "case", "default", "return", "yield"
# JSX の要素の中の文脈（行頭へ独立コメントを置けない）
MARKUP_INSIDE: tuple[str, ...] = "children", "close", "self", "tag"
# 識別子・数値の字句
WORD: re.Pattern[str] = re.compile(r"[\w$@]+")
# 候補を拒んだ理由の内，整形器の不具合を示す物（整形仕様 8.1 の構文・意味・構造・空行の検証失敗）
DEFECTS: tuple[str, ...] = "break syntax", "change blank lines", "change code structure", "change the meaning of the code"
# 契約上の終了コード
ALLOWED: tuple[int, ...] = 0, 1, 2
# 各行の末尾へ足す字（空白とタブの双方を１回の変異で通す）
SUFFIX: bytes = b" \t"

class Token(NamedTuple):
	"""字句（コードの字句又はコメント）"""

	text: str # 字面
	line: int # 始まる行（０から）
	comment: bool # コメントか
	standalone: bool # 前後に同じ行のコードの無いコメントか
	indent: str # 始まる行の行頭から字句迄の字面

class Region(NamedTuple):
	"""字句解析済の検査区画"""

	tokens: list[Token] # 区画の字句
	base: int # 後続コードの無い時の深さ
	language: str # 区画内の言語
	tag_line: int # 埋込の開始タグの行（其れ以外は -1）

class EmbeddedRegion(NamedTuple):
	"""HTML 内の埋込区画と本文の範囲"""

	language: str # 本文の言語
	body: str # 開始タグの行頭から閉じタグの手前迄
	first_line: int # 本文の開始行
	base: int # 区画の最上位の深さ
	content_start: int # 開始タグの直後
	content_end: int # 閉じタグの直前
	inspect: bool # 本文を埋込言語として検査するか

def is_defect(result: subprocess.CompletedProcess[bytes]) -> bool:
	"""整形器の終了状態と通知から不具合に依る拒否かを返す
	計算量：通知の文字数 B に対して O(B)
	"""
	# 終了状態と不具合通知の照合
	message: str = result.stderr.decode("utf-8", errors="replace")
	# 不具合の判定の返戻
	return (
		result.returncode < 0 or bool(result.returncode) and (
			"formatter defect" in message or "could not be completed" in message or any(f"would {reason}," in message for reason in DEFECTS)
		)
	)

def format_source(binary: str, language: str, text: bytes, width: str) -> bytes:
	"""指定の整形器と言語と行幅で検体を整形したバイト列を返す
	整形の拒否又は見送では通知を伴う RuntimeError を送出する
	整形器の実行が時間切れに為れば TimeoutExpired を送出する
	計算量：整形器の実行時間 T と通知の長さ B に対して O(T+B)
	"""
	# 整形器の起動と見送の確認
	result: subprocess.CompletedProcess[bytes] = subprocess.run(
		(binary, "--stdin", "--lang", language, "--no-lint", "--chars", width),
		input=text,
		capture_output=True,
		timeout=300
	)
	message: str = result.stderr.decode("utf-8", errors="replace").strip()
	# 見送は終了コード０で原文を返す為，通知で区別する
	if result.returncode or "[skip]" in message:
		raise RuntimeError(message)
	# 整形結果の返戻
	return result.stdout

def line_end(text: str, index: int) -> int:
	"""字面の指定位置以後で最初の改行の位置を返す
	改行が無ければ字面の長さを返す
	計算量：指定位置以後の文字数 L に対して O(L)
	"""
	end: int = text.find("\n", index)
	# 行の終わりの返戻
	return len(text) if end < 0 else end

def decode_unicode_escapes(text: str) -> str:
	"""文字・文字列のリテラルの外の ASCII の Unicode の逃避を其の字へ読み替えた字面を返す（Java 言語仕様 3.3，整形器も字句の外の物を読み替える）
	計算量：文字数 L に対して O(L)
	"""
	pieces: list[str] = []
	# 引用符の中の逃避を除く走査
	index: int = 0
	backslashes: int = 0
	while index < len(text):
		if text[index] in "\"'":
			end: int = index + 1
			while end < len(text) and text[end] != text[index] and text[end] != "\n":
				end += 2 if text[end] == "\\" else 1
			pieces.append(text[index:end + 1])
			index = end + 1
			backslashes = 0
			continue
		escape: re.Match[str]|None = UNICODE_ESCAPE.match(text, index)
		# 前に奇数個の逆斜線が続く逆斜線は逃避を始めない（`\\u000a` は字の儘）
		if escape and not backslashes % 2 and int(escape.group(1), 16) < 0X80:
			pieces.append(chr(int(escape.group(1), 16)))
			index = escape.end()
			backslashes = 0
			continue
		pieces.append(text[index])
		backslashes = backslashes + 1 if text[index] == "\\" else 0
		index += 1
	# 読み替えた字面の返戻
	return "".join(pieces)

def lex(text: str, language: str, first_line: int = 0) -> list[Token]:
	"""指定言語のコメント開始記号に従い，字面をコードの字句とコメントへ分ける
	文字列・三重引用符・C# の逐語文字列・ヒアドキュメント・引用符の無い `url()` の中の記号はコメントと見做さない
	行番号は指定の開始行を基準とする
	計算量：字面の文字数 L に対して最悪 O(L²)
	"""
	openers: tuple[str, ...] = OPENERS.get(language, ())
	# 言語毎の記法と字句走査の準備
	if language in UNICODE_ESCAPE_LANGUAGES:
		text = decode_unicode_escapes(text)
	found: list[Token] = []
	# 開始の行の終わりで本文を読み飛ばすヒアドキュメントの終端の語
	heredocs: list[str] = []
	index: int = 0
	line: int = first_line
	line_start: int = 0
	while index < len(text):
		char: str = text[index]
		# 空白とヒアドキュメント本文の読飛
		if char == "\n":
			index, line, line_start = index + 1, line + 1, index + 1
			# ヒアドキュメントの本文の行（終端の語だけの行迄）の読飛
			while heredocs and index < len(text):
				end: int = line_end(text, index)
				if text[index:end].strip() == heredocs[0]:
					heredocs.pop(0)
				index, line, line_start = end + 1, line + 1, end + 1
			continue
		if char in " \t\r\ufeff" or language in UNICODE_SPACE_LANGUAGES and char in UNICODE_SPACES:
			index += 1
			continue
		indent: str = text[line_start:index]
		if language in HEREDOC_LANGUAGES and (heredoc := HEREDOC_START.match(text, index)):
			heredocs.append(heredoc.group(2))
			found.append(Token(heredoc.group(0), line, False, False, indent))
			index = heredoc.end()
			continue
		# 引用形の字面の保護
		if language in TRIPLE_QUOTE_LANGUAGES and text.startswith(("\"\"\"", "'''"), index):
			close: int = text.find(text[index:index + 3], index + 3)
			end = len(text) if close < 0 else close + 3
			body: str = text[index:end]
			found.append(Token(body, line, False, False, indent))
			line, line_start = line + body.count("\n"), index + body.rfind("\n") + 1 if "\n" in body else line_start
			index = end
			continue
		if language == "csharp" and text.startswith(("$@\"", "@$\"", "@\""), index):
			prefix: int = 3 if text.startswith(("$@\"", "@$\""), index) else 2
			end: int = index + prefix
			while end < len(text):
				if text.startswith("\"\"", end):
					end += 2
				elif text[end] == "\"":
					end += 1
					break
				else:
					end += 1
			body: str = text[index:end]
			found.append(Token(body, line, False, False, indent))
			if "\n" in body:
				line, line_start = line + body.count("\n"), index + body.rfind("\n") + 1
			index = end
			continue
		if (
			language in URL_LANGUAGES and text[index:index + 4].lower() == "url(" and text[index + 4:index + 5] not in ("\"", "'") and
			(close := text.find(")", index)) >= 0
		):
			found.append(Token(text[index:close + 1], line, False, False, indent))
			index = close + 1
			continue
		# コメント開始記号と本文の抽出
		opener: str = next((opener for opener in openers if text.startswith(opener, index)), "")
		# ヒアドキュメント・正規表現・区切りを持つリテラルの中の埋込式の `#{` はコメントでない
		if language in HEREDOC_LANGUAGES and text.startswith("#{", index):
			opener = ""
		# 行頭を必須とする記法は行頭の物だけ
		if opener in COLUMN_ZERO and indent:
			opener = ""
		if opener:
			if opener in COLUMN_ZERO:
				closer: re.Match[str]|None = re.compile(rf"^{re.escape(BLOCKS[opener])}\b", re.M).search(text, index)
				end: int = line_end(text, closer.start()) if closer else len(text)
			elif opener in BLOCKS:
				end = text.find(BLOCKS[opener], index + len(opener))
				end = len(text) if end < 0 else end + len(BLOCKS[opener])
			else:
				end = line_end(text, index)
			body: str = text[index:end]
			found.append(Token(body, line, True, not indent.strip() and not text[end:line_end(text, end)].strip(), indent))
		elif char in "\"'`":
			# 閉じの無い引用符（Rust の生存期間等）は記号の字句とする
			end = index + 1
			while end < len(text) and text[end] != char and (text[end] != "\n" or char == "`"):
				end += 2 if text[end] == "\\" else 1
			end = end + 1 if end < len(text) and text[end] == char else index + 1
			body: str = text[index:end]
			found.append(Token(body, line, False, False, indent))
		else:
			word: re.Match[str]|None = WORD.match(text, index)
			end = word.end() if word else index + 1
			body: str = text[index:end]
			found.append(Token(body, line, False, False, indent))
		if "\n" in body:
			line, line_start = line + body.count("\n"), index + body.rfind("\n") + 1
		index = end
	# コメントだけの行（同じ行にコードの字句の無い行）の物は独立（`/* a */ // b` の両方，整形仕様 7.1）
	code_lines: set[int] = {
		line for token in found if not token.comment for line in range(token.line, token.line + token.text.count("\n") + 1)
	}
	# 字句の並びの返戻
	return [
		(
			token._replace(standalone=token.line not in code_lines and token.line + token.text.count("\n") not in code_lines) if
			token.comment else
			token
		) for token in found
	]

def embedded(text: str) -> list[EmbeddedRegion]:
	"""HTML の埋込区画毎の言語・字面・行・深さ・本文の範囲を返す
	計算量：文字数 L・埋込の区画数 E に対して O(EL)
	"""
	# 埋込要素と閉じ要素の対応付け
	found: list[EmbeddedRegion] = []
	previous_end: int = 0
	for tag in EMBEDDED_TAG.finditer(text):
		if tag.start() < previous_end:
			continue
		name: str = tag.group(1).lower()
		close: re.Match[str]|None = re.search(rf"</{re.escape(name)}\s*>", text[tag.end():], re.I)
		content_end: int = len(text) if close is None else tag.end() + close.start()
		previous_end = len(text) if close is None else tag.end() + close.end()
		inspect: bool = name != "script" or not FOREIGN_TYPE.search(tag.group(2))
		# 開始の要素の在る行の頭から読み，同じ行の中身を独立した行の物と見誤らない
		head: int = text.rfind("\n", 0, tag.start()) + 1
		found.append(
			EmbeddedRegion(
				EMBEDDED[name],
				text[head:content_end],
				text.count("\n", 0, head),
				len(text[head:tag.start()]) - len(text[head:tag.start()].lstrip("\t")) + 1,
				tag.end(),
				content_end,
				inspect
			)
		)
	# 埋込の区画の返戻
	return found

def regions(text: str, language: str) -> list[Region]:
	"""指定言語で検査する区画毎の字句と最上位の深さと区画の言語を返す
	HTML では本体と埋込の JavaScript・CSS を別の区画とする
	計算量：文字数 L・埋込の区画数 E に対して最悪 O((E+1)L²)
	"""
	if language != "html":
		# 全体を一つの区画とする事の返戻
		return [Region(lex(text, language), 0, language, -1)]
	# 埋込区画を除いた HTML 本体の構築
	sections: list[EmbeddedRegion] = embedded(text)
	parts: list[str] = []
	start: int = 0
	for section in sections:
		parts.append(text[start:section.content_start])
		parts.append(re.sub(r"[^\n]", " ", text[section.content_start:section.content_end]))
		start = section.content_end
	parts.append(text[start:])
	# 埋込の本文を字句と見做さず，HTML 本体の行番号と字下げを保持する
	markup: list[Token] = lex("".join(parts), "html")
	# HTML の本体と各埋込の区画の返戻
	return [
		Region(markup, 0, "html", -1),
		*[
			Region(
				lex(section.body, section.language, section.first_line),
				section.base,
				section.language,
				section.first_line
			) for section in sections if section.inspect
		]
	]

def depth_mismatches(text: str, parsed: list[Region]) -> list[str]:
	"""指定言語の独立コメントの深さと字下げの違反を行順に返す
	深さは後続のコード行，無ければ区画の最上位と比較する
	計算量：文字数 L・埋込の区画数 E・字句数 N に対して最悪 O((E+1)L²+N log N)
	"""
	lines: list[str] = text.split("\n")
	# 行と違反の組
	found: list[tuple[int, str]] = []
	# 後続コード行の深さの追跡
	for tokens, base, _, _ in parsed:
		# 後続のコード行の深さ（末尾から遡って求める）
		following: int = base
		for token in reversed(tokens):
			if not token.comment:
				following = len(lines[token.line]) - len(lines[token.line].lstrip("\t"))
			elif token.standalone and not token.indent.strip() and token.indent.strip("\t"):
				found.append((token.line, f"{token.line + 1} 行目の {token.text[:40]}：タブでなく字下げした"))
			elif (
				token.standalone and not token.indent.strip() and len(token.indent) != following and not token.text.startswith(COLUMN_ZERO)
			):
				found.append((token.line, f"{token.line + 1} 行目の {token.text[:40]}：深さ {len(token.indent)}，後続のコード行の深さ {following}"))
			elif token.standalone and not token.indent.strip() and not token.text.startswith(COLUMN_ZERO):
				# 継続行が全て `*` で始まる囲みは，其の行を開始の行と同じ深さに半角スペース１つを置いて揃える（記号の無い本文の行を持つ物は逐語の儘）
				rests: list[str] = token.text.split("\n")[1:]
				for offset, rest in enumerate(rests if all(rest.lstrip(" \t").startswith("*") for rest in rests) else [], 1):
					if rest[:len(rest) - len(rest.lstrip(" \t"))] != token.indent + " ":
						found.append((token.line + offset, f"{token.line + offset + 1} 行目の {rest.strip()[:40]}：囲みの継続行の深さが開始の行と違う"))
	# 食違いの一覧（行の順）の返戻
	return [problem for _, problem in sorted(found)]

def blocked_lines(tokens: list[Token], language: str) -> dict[int, int]:
	"""行頭へ独立コメントを置けない出力の行と，其の行を含む区画の始まる行の対応を返す（整形仕様 7.6, 7.7）
	区画は Rust のマクロの字句列（逐語）と，コードの文脈で始まる JSX の要素（札と本文）とし，区画の始まる行自身は置ける
	計算量：コードの字句数 N に対して O(N)
	"""
	found: dict[int, int] = {}
	# Rust のマクロ区画の走査
	if language == "rust":
		depth: int = 0
		start: int = -1
		for index, token in enumerate(tokens):
			if depth and token.line != tokens[index - 1].line:
				found.setdefault(token.line, start)
			if depth:
				depth += (token.text in ("(", "[", "{")) - (token.text in (")", "]", "}"))
			elif token.text in ("(", "[", "{") and index >= 2 and tokens[index - 1].text == "!" and WORD.fullmatch(tokens[index - 2].text):
				depth, start = 1, tokens[index - 2].line
	# JSX の要素区画の走査
	elif language in MARKUP_LANGUAGES:
		# 文脈の積（コードの "code"・"brace"・"container"，札の中の "tag"・"self"，本文の "children"，閉じの札の "close"）と，コードの文脈で始めた要素の始まる行の積
		contexts: list[str] = ["code"]
		starts: list[int] = []
		for index, token in enumerate(tokens):
			if (not index or token.line != tokens[index - 1].line) and contexts[-1] in MARKUP_INSIDE and starts:
				found.setdefault(token.line, starts[-1])
			following: str = tokens[index + 1].text if index + 1 < len(tokens) else ""
			previous: str = tokens[index - 1].text if index else ""
			if contexts[-1] in MARKUP_INSIDE:
				if contexts[-1] == "children" and token.text == "<":
					contexts.append("close" if following == "/" else "tag")
				elif contexts[-1] in ("children", "tag") and token.text == "{":
					contexts.append("brace" if contexts[-1] == "tag" else "container")
				elif contexts[-1] == "tag" and token.text == "/" and following == ">":
					contexts[-1] = "self"
				elif contexts[-1] == "tag" and token.text == ">":
					contexts[-1] = "children"
				elif contexts[-1] in ("close", "self") and token.text == ">":
					# 閉じの札は其の要素の本文も閉じ，コードの文脈へ戻れば区画を終える
					if contexts.pop() == "close":
						contexts.pop()
					if contexts[-1] not in MARKUP_INSIDE:
						starts.pop()
			elif (
				token.text == "<" and (WORD.fullmatch(following) or following == ">") and
				(not (WORD.fullmatch(previous) or previous in (")", "]")) or previous in MARKUP_LEADS)
			):
				starts.append(token.line)
				contexts.append("tag")
			elif token.text == "{":
				contexts.append("brace")
			elif token.text == "}" and len(contexts) > 1:
				contexts.pop()
	# 置けない行と区画の始まる行の対応の返戻
	return found

def separator_mismatches(text: str, parsed: list[Region]) -> list[str]:
	"""指定言語の行末コメントの前置区切りの違反を行順に返す（整形仕様 7.6）
	行末コメントは同じ行の前にコード又はコメントが在り，同じ行にコードが続かない物とし，前置区切りはタブだけ又は半角スペース１つとする
	行末へ積んだ記号の前の区切りも同じ規則で検査し，字面を保つ区画（Rust のマクロの字句列・JSX）の物は除く
	計算量：文字数 L・埋込の区画数 E・字句数 N に対して最悪 O((E+1)L²+N log N)
	"""
	# 行と違反の組
	found: list[tuple[int, str]] = []
	for tokens, _, inner, tag_line in parsed:
		if inner == "html":
			continue
		blocked: dict[int, int] = blocked_lines(tokens, inner)
		# 後に続くコードの字句の始まる行（末尾から遡って求め，無ければ最終行の後）
		following: int = text.count("\n") + 1
		for token in reversed(tokens):
			if not token.comment:
				following = token.line
				continue
			if (
				token.standalone or token.line in blocked or token.line == tag_line or not token.indent.strip() or
				following <= token.line + token.text.count("\n")
			):
				continue
			separator: str = token.indent[len(token.indent.rstrip(" \t" + (UNICODE_SPACES if inner in UNICODE_SPACE_LANGUAGES else ""))):]
			# 行末へ積んだ記号の前の区切りも同じ規則（行コメントの本文の中の，空白の後の記号）
			opener: str = next(opener for opener in OPENERS[inner] if token.text.startswith(opener))
			stacked: list[str] = (
				re.findall(rf"([ \t\u3000]+){re.escape(opener)} ", token.text[len(opener):]) if opener not in BLOCKS else []
			)
			for gap in [separator, *stacked]:
				if gap != " " and (not gap or gap.strip("\t")):
					found.append((token.line, f"{token.line + 1} 行目の {token.text[:40]}：前置区切り {gap!r}"))
	# 違反の一覧（行の順）の返戻
	return [problem for _, problem in sorted(found)]

def trailing_separators(text: str, parsed: list[Region]) -> list[tuple[tuple[int, str], str, str]]:
	"""指定言語の行末へ積んだ記号毎の錨の字句・本文・前置区切りを返す
	行末コメントは同じ行の前にコード又はコメントが在り，同じ行にコードが続かない物とし，字面を保つ区画の物と HTML の埋込の区画の開始の行の物は除く
	計算量：文字数 L・埋込の区画数 E・字句数 N に対して最悪 O((E+1)L²+N log N)
	"""
	found: list[tuple[int, int, int, str, str]] = []
	for tokens, _, inner, tag_line in parsed:
		if inner == "html":
			continue
		blocked: dict[int, int] = blocked_lines(tokens, inner)
		# 後に続くコードの字句の始まる行（末尾から遡って求め，無ければ最終行の後）
		following: int = text.count("\n") + 1
		for token in reversed(tokens):
			if not token.comment:
				following = token.line
				continue
			if (
				token.standalone or token.line in blocked or token.line == tag_line or not token.indent.strip() or
				following <= token.line + token.text.count("\n")
			):
				continue
			opener: str = next(opener for opener in OPENERS[inner] if token.text.startswith(opener))
			parts: list[str] = (
				re.split(rf"([ \t\u3000]+){re.escape(opener)} ", token.text[len(opener):]) if
				opener not in BLOCKS else
				[token.text[len(opener):]]
			)
			gap: str = token.indent[len(token.indent.rstrip(" \t" + (UNICODE_SPACES if inner in UNICODE_SPACE_LANGUAGES else ""))):]
			for index in range(0, len(parts), 2):
				body: str = parts[index].split("\n", 1)[0]
				found.append((token.line, len(token.indent), len(found), body, gap))
				if index + 1 < len(parts):
					gap = parts[index + 1]
	# 本文と前置区切りの組の一覧（原文の順）の返戻
	return [(body, gap) for _, _, _, body, gap in sorted(found)]

def separator_record_mismatches(source: str, before: list[Region], output: str, after: list[Region]) -> list[str]:
	"""出力の行末コメントの前置区切りを，同じ本文の原文の記録の何れかのタブの数と照合する（整形仕様 7.1, 7.6）
	並替・統合で記録の順が変わっても記号毎に対応させ，原文に同じ本文の物が無い物は除き，原文の記録から分けた追加の論理行はタブ０個と照合する
	計算量：文字数 L・埋込の区画数 E・字句数 N に対して最悪 O((E+1)L²+N log N)
	"""
	# 本文毎の原文の記録のタブの数
	recorded: dict[str, list[int]] = {}
	for body, gap in trailing_separators(source, before):
		# 元の１記録を文毎に分けた追加の記号はタブ０個の記録として扱う
		pieces: list[str] = [part for part in re.split(r"[。．]|\.(?=\s|$)", body) if part.strip()] or [body]
		for index, part in enumerate(pieces):
			recorded.setdefault(re.sub(r"[\s/*#<>!=。．.、，,-]+", "", part), []).append(gap.count("\t") if index == 0 else 0)
	found: list[str] = []
	for body, gap in trailing_separators(output, after):
		key: str = re.sub(r"[\s/*#<>!=。．.、，,-]+", "", body)
		if key not in recorded:
			continue
		tabs: int|None = next((tabs for tabs in recorded[key] if gap == ("\t" * tabs if tabs else " ")), None)
		if tabs is None:
			found.append(f"{key[:40]}：前置区切り {gap!r}，原文のタブの数 {recorded[key]}")
		else:
			recorded[key].remove(tabs)
	# 食違いの一覧の返戻
	return found

def sentences(body: str) -> set[str]:
	"""コメント本文の全体と行・文毎の本文を照合用の集合で返す
	記号・空白・句読点は除く
	計算量：本文の文字数 B に対して O(B)
	"""
	# 本文全体と空でない行・文の集合の返戻
	return { re.sub(r"[\s/*#<>!=。．.、，,-]+", "", piece) for piece in (body, *re.split(r"[。．\n]|\.\s", body)) } - { "" }

def count(tokens: list[Token], text: str) -> int:
	"""字句の並びで指定の字面と一致する物の数を返す
	計算量：字句数 N に対して O(N)
	"""
	# 同じ字面の字句の数の返戻
	return sum(1 for token in tokens if token.text == text)

def anchor_mismatches(before: list[Region], after: list[Region]) -> list[str]:
	"""解析済の区画の独立コメントの錨に反する物を列挙する
	計算量：区画数 E と各区画の照合費用 A に対して O(EA)
	"""
	# HTML 本体のコメントはソース側に保持する為，埋込の区画だけ錨の字句を照合する
	return [
		problem for source_region, output_region in zip(before, after) if source_region.language != "html" for problem in
		anchor_region_mismatches(source_region.tokens, output_region.tokens, output_region.language)
	]

def anchor_region_mismatches(before: list[Token], after: list[Token], language: str) -> list[str]:
	"""原文と出力の独立コメントの錨に反する物を列挙する
	指定のコメント開始記号が無ければ空の一覧を返す
	字句の対応は右側の字句を先に SequenceMatcher で照合して右へ寄せ，補った同じ字面の字句を原文の後続字句と取り違えない
	後続字句の追越と，錨の行（原文で後に続いた字句の在る行）より前への遡行を検査する
	錨の行が行頭へ置けない区画の中なら其の区画の始まる行の前を許し，括弧の中の文書は定義の文書の同期に従う為に除く
	計算量：コード字句数 N・コメント数 C・本文の最大文字数 B に対し最悪 O(N²+C²(N²+B))
	"""
	if language not in OPENERS:
		# 記法の無い言語の返戻
		return []
	# 原文と出力のコード字句の対応付け
	source_code: list[Token] = [token for token in before if not token.comment]
	output_code: list[Token] = [token for token in after if not token.comment]
	# 原文の独立コメント毎の照合用の本文と，後に続くコードの字句の位置と，丸括弧・角括弧の中か
	comments: list[tuple[set[str], int, bool]] = []
	# 原文の独立でないコメント（行末・文中）の照合用の本文（種別の変化を検査する）
	attached: list[set[str]] = []
	position: int = 0
	depth: int = 0
	for token in before:
		if not token.comment:
			position += 1
			depth += (token.text in ("(", "[")) - (token.text in (")", "]"))
		elif token.standalone:
			comments.append((sentences(token.text), position, depth > 0))
		else:
			attached.append(sentences(token.text))
	# コメントを除く字句の並びの対応（整形で補い又は除いた字句は対応しない）
	matcher: difflib.SequenceMatcher = difflib.SequenceMatcher(
		None,
		[token.text for token in reversed(source_code)],
		[token.text for token in reversed(output_code)],
		autojunk=False
	)
	mapping: dict[int, int] = {
		len(source_code) - first - offset - 1: len(output_code) - second - offset - 1 for first, second, size in
		matcher.get_matching_blocks() for offset in range(size)
	}
	inverse: dict[int, int] = { value: key for key, value in mapping.items() }
	# 対応の右寄せ（最長の一致区間を優先する照合は，後続の同じ字面の原文の字句の手前へ補った字句を原文の字句と対応させる為，次の対応迄の未対応の同じ字面の字句へ移す）
	limit: int = len(output_code)
	for source_index in sorted(mapping, reverse=True):
		target: int = mapping[source_index]
		moved: int = next(
			(
				index for index in range(limit - 1, target, -1) if index not in inverse and output_code[index].text == output_code[target].text
			),
			target
		)
		if moved != target:
			del inverse[target]
			mapping[source_index], inverse[moved] = moved, source_index
		limit = mapping[source_index]
	blocked: dict[int, int] = blocked_lines(output_code, language)
	found: list[str] = []
	position = 0
	# 出力コメントの錨と種別の照合
	for token in after:
		if not token.comment:
			position += 1
			continue
		body: set[str] = sentences(token.text)
		# 同じ本文の記録の内，錨が出力の位置と合う物が在れば良い（文毎に分けた物・文書へ統合した物は其の一部の本文で照合する）
		starts: list[tuple[int, bool]] = [
			(start, inside) for pieces, start, inside in comments if token.standalone and body & pieces and body <= pieces
		]
		# 原文で独立した物だけに同じ本文が在る，独立でない物（種別の変化，整形仕様 7.7）
		is_standalone_source: bool = any(body & pieces and body <= pieces for pieces, _, _ in comments)
		is_attached_source: bool = any(body & pieces and body <= pieces for pieces in attached)
		if not token.standalone and is_standalone_source and not is_attached_source:
			found.append(f"{token.line + 1} 行目の {token.text[:40]}：原文で独立した物が行末・文中へ変わった")
			continue
		problems: list[str] = []
		for start, inside in starts:
			# 括弧の中（定義の見出の仮引数・装飾子・属性の実引数等）から定義の文書へ同期した物
			if (
				inside and (
					token.text.startswith(DOCUMENTS) or
					position < len(output_code) and output_code[position].text in LINE_DOCUMENT_HEADS.get(language, ())
				)
			):
				break
			# 同じ字面の補った字句を錨と誤認しない為，字面でなく対応済の後続字句だけで早期に判定する
			if mapping.get(start) == position:
				break
			# 原文で後に続いた字句の内，区切りを除いて出力に残る最初の物
			following: int = start
			while following < len(source_code) and (following not in mapping or source_code[following].text in SEPARATORS):
				following += 1
			line: int = output_code[position].line if position < len(output_code) else -1
			# 錨の行（原文で後に続いた字句の在る行）
			anchor_line: int = output_code[mapping[following]].line if following in mapping else line
			# 錨の行が置けない区画の中で，其の区画の始まる行の前に在る物
			if blocked.get(anchor_line) == line:
				break
			# 字面の数でも前後の入替が確かめれる物だけを数え，並替えで交差した対応を違反と見誤らない
			overtaken: list[int] = [
				index for index in range(position) if inverse.get(index, -1) >= following and output_code[index].text not in SEPARATORS and
				count(output_code[:position], output_code[index].text) > count(source_code[:start], output_code[index].text)
			]
			preceded: list[int] = [
				index for index in range(position, len(output_code)) if 0 <= inverse.get(index, len(source_code)) < start and
				output_code[index].line != anchor_line and output_code[index].text not in SEPARATORS and
				count(output_code[position:], output_code[index].text) > count(source_code[start:], output_code[index].text)
			]
			# 原文で後に続いた最初の字句其の物を追い越した物（手前の字句の除去で字面の数の比較が打ち消されても見逃さない）
			# 次の字句も続けて対応する物だけとし，並替え（後置の条件への変換等）で別の箇所の同じ字句へ対応した物を見誤らない
			if (
				following in mapping and mapping.get(following + 1) == mapping[following] + 1 and mapping[following] < position and
				output_code[mapping[following]].line != line
			):
				overtaken.append(mapping[following])
			# 補った字句（原文に対応の無い字句，補った返戻文等）の前へ置いた物（補った字句は錨に為らない）
			inserted: list[int] = [
				index for index in range(position, mapping[following] if following in mapping else len(output_code)) if index not in inverse and
				output_code[index].text not in SEPARATORS and (following not in mapping or output_code[index].line < anchor_line)
			]
			if not overtaken and not preceded and not inserted:
				break
			if inserted and not overtaken and not preceded:
				problems.append(f"補った {output_code[inserted[0]].text!r}（{output_code[inserted[0]].line + 1} 行目）の前へ置いた")
				continue
			problems.append(
				f"原文で後に在った {output_code[overtaken[-1]].text!r}（{output_code[overtaken[-1]].line + 1} 行目）を追い越した" if
				overtaken else
				f"原文で前に在った {output_code[preceded[0]].text!r}（{output_code[preceded[0]].line + 1} 行目）より前へ遡った"
			)
		else:
			if problems:
				found.append(f"{token.line + 1} 行目の {token.text[:40]}：{problems[0]}")
	# 食違いの一覧の返戻
	return found

def exit_code(binary: str, language: str, text: bytes) -> int:
	"""指定の整形器と言語で各行末へ空白とタブを足した検体の終了コードを返す
	時間切れは -1，整形器の不具合は -2 を返す
	計算量：入力のバイト数 B と整形器の実行時間 T に対して O(B+T)
	"""
	# 行末空白を付けた変種での終了状態の確認
	mutated: bytes = b"\n".join(line + SUFFIX if line else line for line in text.split(b"\n"))
	try:
		result: subprocess.CompletedProcess[bytes] = subprocess.run(
			(binary, "--stdin", "--lang", language, "--no-lint"),
			input=mutated,
			capture_output=True,
			timeout=120
		)
	except subprocess.TimeoutExpired:
		# 時間の上限を越えた場合は契約外として扱う事の返戻
		return -1
	# 整形器の不具合を契約外とした終了コードの返戻
	return -2 if is_defect(result) else result.returncode

def inspect(binary: str, language: str, source: Path) -> tuple[int, list[str]]:
	"""指定の整形器と言語で検体の変種を調べ，比べた幅の数と違反を返す
	計算量：整形器の実行時間の和 T と各検査の実行時間の和 V に対して O(T+V)
	"""
	# 原文と期待出力の注記配置の照合
	text: bytes = source.read_bytes()
	original: str = text.decode(errors="replace")
	before: list[Region] = regions(original, language)
	checked: int = 0
	expected: str = source.with_suffix(".out").read_bytes().decode(errors="replace")
	expected_regions: list[Region] = regions(expected, language)
	problems: list[str] = [
		f"期待する出力の{problem}" for problem in
		depth_mismatches(expected, expected_regions) + separator_mismatches(expected, expected_regions) +
		separator_record_mismatches(original, before, expected, expected_regions) + anchor_mismatches(before, expected_regions)
	]
	# 各行幅での冪等性と注記配置の照合
	for width in (WIDE, *WIDTHS):
		try:
			first: bytes = format_source(binary, language, text, width)
			checked += 1
			if format_source(binary, language, first, width) != first:
				problems.append(f"幅 {width} で二巡目が一巡目と異なる")
		except RuntimeError as error:
			problems.append(f"幅 {width} で整形を拒否：{error}")
			continue
		output: str = first.decode(errors="replace")
		output_regions: list[Region] = regions(output, language)
		problems += [
			f"幅 {width} の{problem}" for problem in depth_mismatches(output, output_regions) + separator_mismatches(output, output_regions) +
			separator_record_mismatches(original, before, output, output_regions) + anchor_mismatches(before, output_regions)
		]
	code: int
	if (code := exit_code(binary, language, text)) not in ALLOWED:
		problems.append(f"各行の末尾へ空白とタブを足すと終了コード {code}（-1 は時間切れ，-2 は整形器の不具合）")
	# 比べた幅の数と違反の返戻
	return checked, problems

def main() -> int:
	"""検体の変種を整形して突合し，違反が有れば非零で終える
	計算量：検体数 F・１検体当たりの整形と検査の最大時間 T に対して O(FT+F log F)
	"""
	# 基準検体と整形器の準備
	binary: str = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "build/shavefmt")
	# 整形を拒む事を期待する検体は変種でも拒否が期待と為る為に除く
	sources: list[Path] = [
		source for directory in sorted(path for path in GOLDEN.iterdir() if path.is_dir()) for source in
		sorted(directory.glob("*.in")) if not source.with_suffix(".fail").exists() and not source.with_suffix(".skip").exists()
	]
	checked: int = 0
	failures: list[str] = []
	# 検体毎の整形と検査は互いに独立な為，同時に走らせる工程の数 (`SHAVEFMT_JOBS`) で分ける（結果は検体の順に集める）
	with concurrent.futures.ProcessPoolExecutor(max_workers=JOBS) as pool:
		for source, (widths, problems) in zip(
			sources,
			pool.map(inspect, itertools.repeat(binary), [source.parent.name for source in sources], sources, chunksize=8)
		):
			checked += widths
			failures += [f"{source.relative_to(ROOT)}: {problem}" for problem in problems]
	# 検体の違反の報告
	for failure in failures:
		print(f"FAIL case variants: {failure}", file=sys.stderr)
	print(f"case variants: {checked} cases, {len(failures)} failed")
	# 違反が有れば非零の返戻
	return 1 if failures else 0

if __name__ == "__main__":
	sys.exit(main())
