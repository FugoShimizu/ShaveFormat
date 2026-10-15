#! /usr/bin/env python3
"""C / C++ の検証ツールが共有するトークンと非コード領域の読取"""

from __future__ import annotations

from bisect import bisect_left

import re

from typing import Iterable, Iterator, NamedTuple

# 生文字列の開始（長いエンコーディング接頭辞を先に照合する）
CPP_RAW_HEAD: re.Pattern[str] = re.compile(r'(?:u8|u|U|L)?R"([^()\\\s]{0,16})\(')
# 読取側が受理する水平空白付の継続行も，同じ原文対応へ含める
CPP_SPLICE: re.Pattern[str] = re.compile(r"\\[ \t]*(?:\r\n?|\n)")

class CppToken(NamedTuple):
	"""原文の範囲とトークンの分類"""

	kind: str
	start: int
	end: int
	text: str

def cpp_number_end(text: str, start: int) -> int:
	"""前処理数の終端を読み，桁区切を引用符と区別する
	計算量：トークンの文字数 L に対して O(L)
	引数：
		text: 読取対象の原文
		start: 数字又は数字が続く点の位置
	戻値：
		前処理数の直後の位置
	"""
	# 桁区切と指数の符号を含む前処理数の走査
	pos: int = start + 1
	while pos < len(text):
		char: str = text[pos]
		if (
			char.isalnum() or char in "_." or char == "'" and pos + 1 < len(text) and (text[pos + 1].isalnum() or text[pos + 1] == "_") or
			char in "+-" and text[pos - 1] in "eEpP"
		):
			pos += 1
		else:
			break
	# 前処理数の終端の返戻
	return pos

def logical_tokens(text: str, start: int = 0) -> Iterator[CppToken]:
	"""数・引用・コメントを区別し，論理テキストの範囲を１度ずつ返す
	計算量：原文の文字数 L に対して O(L)
	引数：
		text: 行連結済の論理テキスト
		start: 走査を始めるトークン境界
	戻値：
		論理テキスト順のトークン範囲の反復子
	"""
	# 非コード領域と，引用符を含む数値の走査
	pos: int = start
	while pos < len(text):
		start: int = pos
		char: str = text[pos]
		kind: str = "punct"
		if char.isspace():
			kind = "space"
			pos += 1
			while pos < len(text) and text[pos].isspace():
				pos += 1
		elif text.startswith("//", pos) or text.startswith("/*", pos):
			kind = "comment"
			is_line: bool = text[pos + 1] == "/"
			line_end: re.Match[str]|None = re.compile(r"[\r\n]").search(text, pos + 2) if is_line else None
			close: int = (line_end.start() if line_end else -1) if is_line else text.find("*/", pos + 2)
			pos = len(text) if close < 0 else close if is_line else close + 2
		elif char.isdigit() or char == "." and pos + 1 < len(text) and text[pos + 1].isdigit():
			kind = "number"
			pos = cpp_number_end(text, pos)
		elif raw := CPP_RAW_HEAD.match(text, pos):
			kind = "literal"
			ending: str = ")" + raw.group(1) + "\""
			close = text.find(ending, raw.end())
			pos = len(text) if close < 0 else close + len(ending)
		elif char in "\"'":
			kind = "literal"
			pos += 1
			while pos < len(text):
				current: str = text[pos]
				pos += 1
				if current == "\\" and pos < len(text):
					pos += 1
				elif current == char or current == "\n":
					break
		elif char.isalpha() or char == "_":
			kind = "identifier"
			pos += 1
			while pos < len(text) and (text[pos].isalnum() or text[pos] == "_"):
				pos += 1
		else:
			pos += 2 if text.startswith("##", pos) else 1
		yield CppToken(kind, start, pos, text[start:pos])
	# 終了
	return

class CppSource(NamedTuple):
	"""行連結後のテキストと，各文字の原文位置"""

	text: str
	offsets: list[int]

def cpp_source(text: str) -> CppSource:
	"""行連結を反映し，生文字列内だけ原文の連結を取り消す
	計算量：文字数 L と生文字列数 R に対して O(L+R log(L+1))
	引数：
		text: 原文
	戻値：
		論理テキストと終端を含む原文位置の対応
	"""
	chars: list[str] = []
	offsets: list[int] = []
	pos: int = 0
	while pos < len(text):
		if text[pos] == "\\" and (splice := CPP_SPLICE.match(text, pos)):
			pos = splice.end()
		else:
			chars.append(text[pos])
			offsets.append(pos)
			pos += 1
	offsets.append(len(text))
	logical: str = "".join(chars)
	parts: list[str] = []
	positions: list[int] = []
	start: int = 0
	tokens: Iterator[CppToken] = logical_tokens(logical)
	while (token := next(tokens, None)) is not None:
		if token.kind != "literal" or not (head := CPP_RAW_HEAD.match(logical, token.start)):
			continue
		# 生文字列の終端は，行連結前の本文で照合する
		ending: str = ")" + head.group(1) + "\""
		quote: int = logical.index("\"", token.start, head.end())
		raw_start: int = offsets[quote]
		close: int = text.find(ending, offsets[head.end() - 1] + 1)
		raw_end: int = len(text) if close < 0 else close + len(ending)
		parts.extend((logical[start:quote], text[raw_start:raw_end]))
		positions.extend(offsets[start:quote])
		positions.extend(range(raw_start, raw_end))
		start = bisect_left(offsets, raw_end)
		# 残りを生文字列の実際の終端から再開する
		tokens = logical_tokens(logical, start)
	parts.append(logical[start:])
	positions.extend(offsets[start:])
	# 照合結果の返戻
	return CppSource("".join(parts), positions)

def cpp_tokens(text: str) -> Iterator[CppToken]:
	"""論理テキストのトークンと原文の範囲を返す
	引数：
		text: 行連結前の原文
	戻値：
		論理テキストと原文の範囲を持つトークンの反復子
	"""
	source: CppSource = cpp_source(text)
	for token in logical_tokens(source.text):
		yield CppToken(token.kind, source.offsets[token.start], source.offsets[token.end - 1] + 1, token.text)

def cpp_code_tokens(tokens: Iterable[CppToken], should_keep_literals: bool = False) -> str:
	"""取得済のトークンから非コード領域を伏せ，位置と改行を保つ
	計算量：トークンの総文字数 L に対して O(L)
	引数：
		tokens: 行連結後の共有するトークン
		should_keep_literals: 型名の比較等の引用を読む為，リテラルの綴りを残すか
	戻値：
		非コード領域を空白へ置き換えたテキスト
	"""
	parts: list[str] = []
	for token in tokens:
		piece: str = token.text
		is_hidden: bool = token.kind == "comment" or token.kind == "literal" and not should_keep_literals
		parts.append(re.sub(r"[^\n]", " ", piece) if is_hidden else piece)
	# 同じトークン列の見方だけを変えたコードの返戻
	return "".join(parts)

def cpp_source_code(source: CppSource) -> str:
	"""既に連結した原文から，位置と改行を保ったコードを返す
	計算量：論理テキストの文字数 L に対して O(L)
	引数：
		source: 行連結と生文字列の原文保持を終えた読取結果
	戻値：
		非コード領域を空白へ置き換えたテキスト
	"""
	# 同じ読取結果のコードの返戻
	return cpp_code_tokens(logical_tokens(source.text))

def cpp_code(text: str) -> str:
	"""コメントとリテラルを除き，論理テキストの位置と改行を保つ
	計算量：文字数 L と生文字列数 R に対して O(L+R log(L+1))
	引数：
		text: 読取対象の原文
	戻値：
		行連結後の非コード領域を空白へ置き換えたテキスト
	"""
	# 共通のトークンの伏方へ渡したコードの返戻
	return cpp_source_code(cpp_source(text))

def cpp_lex_self_check() -> list[str]:
	"""桁区切の後のコードを残し，偽の宣言と依存を隠す正誤対を照合する
	計算量：テストケースの総文字数 L に対して O(L)
	戻値：
		手書の期待と違うケースの一覧
	"""
	# 数値の境界と非コード領域の独立した期待
	found: list[str] = []
	for text, expected in (
		("1'000; std::optional<int> x;", "1'000; std::optional<int> x;"),
		("0xAB'CD; 0b10'01; 1.2'3e+4;", "0xAB'CD; 0b10'01; 1.2'3e+4;"),
		("1'000; '\\''; std::optional<int> x;", "1'000;     ; std::optional<int> x;"),
		("'x' /* std::optional */", "                       "),
		("u8R\"tag(\nstd::optional /* ' */\n)tag\";", "        \n                     \n     ;"),
		("// std::optional\\\nstruct Missing {};\nint x;", "                                  \nint x;")
	):
		if cpp_code(text) != expected:
			found.append(repr(text))
	# 連結後のトークンと原文範囲を別々に照合する
	for text, expected in (
		(
			"/\\\n/ hidden\nint value;",
			[
				("comment", "// hidden", "/\\\n/ hidden"),
				("space", "\n", "\n"),
				("identifier", "int", "int"),
				("space", " ", " "),
				("identifier", "value", "value"),
				("punct", ";", ";")
			]
		),
		("opt\\\nional", [("identifier", "optional", "opt\\\nional")]),
		("inc\\\r\nlude", [("identifier", "include", "inc\\\r\nlude")]),
		("/\\\n* hidden *\\\n/", [("comment", "/* hidden */", "/\\\n* hidden *\\\n/")]),
		("\"a\\\nb\"", [("literal", "\"ab\"", "\"a\\\nb\"")]),
		("R\"(a\\\nb)\"", [("literal", "R\"(a\\\nb)\"", "R\"(a\\\nb)\"")]),
		("R\"(a)\\\n\"b)\"", [("literal", "R\"(a)\\\n\"b)\"", "R\"(a)\\\n\"b)\"")]),
		("R\\\n\"(a\\\nb)\"", [("literal", "R\"(a\\\nb)\"", "R\\\n\"(a\\\nb)\"")])
	):
		actual: list[tuple[str, str, str]] = [(token.kind, token.text, text[token.start:token.end]) for token in cpp_tokens(text)]
		if actual != expected:
			found.append(repr(text))
	# 読取の不一致の返戻
	return found
