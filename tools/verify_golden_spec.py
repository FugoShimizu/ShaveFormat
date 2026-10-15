#! /usr/bin/env python3
"""期待結果其の物を仕様の条文と照合する検査

`golden_test.sh` は `.in` を整形して `.out` と比べる為，`.out` 自体が条文に適うかは見ない
期待結果が実装のエラーをコピーして居ると検査は緑の儘に為る為，此処では**フォーマッターを動かさず** `.out` だけを読んで条文との食違を探す
照合する規則は，空の本体，行頭の閉括弧の後の閉括弧，非空の文本体の必須改行の３つで，其の他の条文への適合は此の検査では確かめない
加えて，表示から除く範囲と英数字・表音文字の生成物と定義元の一致を確かめる
"""

from generate_hidden_display import check_generated, entry_self_check as hidden_entry_self_check
from generate_unicode_alphanumeric import entry_self_check as alphanumeric_entry_self_check
from generate_unicode_normalization import entry_self_check as unicode_entry_self_check
from tree_work import Work, evaluate
from verify_case_variants import embedded

from verify_semantic_preservation import (
	Node,
	css_value_source,
	dump_ast,
	is_comment,
	is_content_leaf,
	parse_dump,
	source_leaves,
	source_observation_signature
)

from bisect import bisect_left, bisect_right

import glob
import os

from pathlib import Path

import re
import sys

from typing import NamedTuple

import unicodedata

# 波括弧以外の空の本体の閉じ（波括弧は解析済の対応で検査する整形仕様 6.3）
CLOSERS: tuple[str, ...] = ")", ");", "]", "],", "end", "}", "})", "});", "};"

# 本体を開く見出の語
HEADS: tuple[str, ...] = (
	"begin",
	"class ",
	"def ",
	"do",
	"fn ",
	"for ",
	"func ",
	"function ",
	"if ",
	"module ",
	"switch ",
	"try",
	"unless ",
	"until ",
	"while "
)

# `;` が要る為に畳まない Ruby の形（整形仕様 6.3）
NO_FOLD: tuple[re.Pattern[str], ...] = (
	re.compile(r"^begin\s+rescue\b"),
	re.compile(r"^class\s*<<"),
	re.compile(r"^class\s+\w+\s*<"),
	re.compile(r"^def\s+[^(]*$")
)

class SyntaxLines(NamedTuple):
	"""逐語本文を伏せた行・コメント継続行・分割可能な閉じ・空の開閉の対応・本体の必須改行"""

	lines: list[str]
	continuations: set[int]
	closer_splits: set[tuple[int, int]]|None
	empty_braces: set[tuple[int, int, int]]
	inline_bodies: set[tuple[int, int]]

# 末尾カンマを挿入しても要素の所属を変えない Go の並び
GO_COMMA_GROUPS: set[str] = { "argument_list", "literal_value", "parameter_list", "type_arguments", "type_parameter_list" }

# 改行で文・宣言の終端を補っても所属を変えない Go の本体と宣言の並び
GO_STATEMENT_GROUPS: set[str] = {
	"block",
	"const_declaration",
	"expression_switch_statement",
	"field_declaration_list",
	"import_spec_list",
	"interface_type",
	"select_statement",
	"type_declaration",
	"type_switch_statement",
	"var_spec_list"
}

# 文・宣言の本体を所有する構文（式の囲みは此の集合の補集合とする）
BODY_OWNERS: set[str] = {
	"abstract_class_declaration",
	"accessor_declaration",
	"annotation_type_declaration",
	"at_rule",
	"catch_block",
	"catch_clause",
	"class",
	"class_declaration",
	"class_definition",
	"class_specifier",
	"class_static_block",
	"companion_object",
	"computed_getter",
	"computed_modify",
	"computed_property",
	"computed_setter",
	"constructor_declaration",
	"constructor_definition",
	"declaration_list",
	"destructor_declaration",
	"do_statement",
	"each_statement",
	"else_clause",
	"enum_constant",
	"enum_declaration",
	"expression_switch_statement",
	"extension_declaration",
	"finally_block",
	"finally_clause",
	"for_expression",
	"for_in_statement",
	"for_statement",
	"foreach_statement",
	"function_declaration",
	"function_definition",
	"function_item",
	"function_statement",
	"guard_statement",
	"if_expression",
	"if_statement",
	"impl_item",
	"include_statement",
	"init_declaration",
	"interface_declaration",
	"interface_type",
	"keyframe_block",
	"keyframes_statement",
	"labeled_statement",
	"lock_statement",
	"loop_expression",
	"match_arm",
	"match_expression",
	"media_statement",
	"method_declaration",
	"method_definition",
	"mixin_statement",
	"namespace_declaration",
	"namespace_definition",
	"object_creation_expression",
	"object_declaration",
	"protocol_declaration",
	"record_declaration",
	"repeat_while_statement",
	"rule_set",
	"secondary_constructor",
	"select_statement",
	"static_initializer",
	"struct_declaration",
	"struct_item",
	"struct_specifier",
	"struct_type",
	"supports_statement",
	"switch_statement",
	"synchronized_statement",
	"trait_item",
	"try_expression",
	"try_statement",
	"try_with_resources_statement",
	"type_switch_statement",
	"union_item",
	"union_specifier",
	"using_statement",
	"when_entry",
	"when_expression",
	"while_expression",
	"while_statement"
}

# 所有者から本体へ降りる構文上のラッパー
BODY_CONTAINERS: set[str] = {
	"accessor_list",
	"block",
	"class_body",
	"compound_statement",
	"constructor_body",
	"control_structure_body",
	"declaration_list",
	"enum_body",
	"enum_class_body",
	"enum_declaration_list",
	"field_declaration_list",
	"function_body",
	"interface_body",
	"keyframe_block_list",
	"match_block",
	"protocol_body",
	"statement_block",
	"switch_block",
	"switch_body"
}

# 列挙子だけなら要素の並びとし，宣言又は文の本体を含む場合だけ展開する
ENUM_BODIES: set[str] = { "enum_body", "enum_class_body", "enum_declaration_list" }

# 文・宣言でなく要素を並べる為，文の本体を子孫に持たなければ上限内で畳める本体（拡張子毎，フィールド宣言を並べる型の本体を除く）
ELEMENT_BODIES: dict[str, set[str]] = {
	"cs": { "accessor_list" },
	"rs": { "field_declaration_list", "ordered_field_declaration_list" },
	"swift": { "protocol_property_requirements" },
	"ts": { "interface_body" },
	"tsx": { "interface_body" }
}

# 文を並べる本体
STATEMENT_BODIES: set[str] = {
	"block",
	"compound_statement",
	"constructor_body",
	"control_structure_body",
	"function_body",
	"statement_block",
	"statements"
}

def body_break_positions(roots: list[Node], line_starts: list[int], ext: str) -> set[tuple[int, int]]:
	"""非空の文・宣言本体で必須改行を欠くコードの行と列を返す
	計算量：ノード数 N・行数 L に対して O(N log L)
	引数：
		roots: 原文の範囲を持つ解析木
		line_starts: 原文の行頭の位置
		ext: 解析した言語の拡張子
	戻値：
		開括弧直後・文間・閉括弧直前の必須改行を欠く位置
	"""
	found: set[tuple[int, int]] = set()
	pending: list[tuple[Node, str]] = [(node, "") for node in roots]
	nodes: list[tuple[Node, str]] = []
	while pending:
		node: Node
		owner: str
		node, owner = pending.pop()
		nodes.append((node, owner))
		# 埋込言語の位置は呼出側で親へ移す
		pending.extend((child, node.type) for child in node.children if child.source is node.source)
	spans: dict[Node, tuple[int, int, str, str]] = {}
	has_statement: set[Node] = set()
	has_declaration: set[Node] = set()
	for node, _ in reversed(nodes):
		if node.type in STATEMENT_BODIES or any(child in has_statement for child in node.children):
			has_statement.add(node)
		if (
			node.type.endswith("_declaration") and node.type != "enum_declaration" or
			any(child in has_declaration for child in node.children)
		):
			has_declaration.add(node)
		if is_comment(node.type):
			continue
		children: list[tuple[int, int, str, str]] = [spans[child] for child in node.children if child in spans]
		if children:
			spans[node] = children[0][0], children[-1][1], children[0][2], children[-1][3]
		elif not node.children and node.start < node.end:
			spans[node] = node.start, node.end, node.type, node.type
	elements: set[str] = ELEMENT_BODIES.get(ext, set())
	for node, owner in nodes:
		if (
			not (node.type in BODY_OWNERS or node.type in BODY_CONTAINERS and owner in BODY_OWNERS) or
			node.type in elements and node not in has_statement or
			node.type in ENUM_BODIES and node not in has_statement and node not in has_declaration
		):
			continue
		braces: list[tuple[int, bool, bool, str]] = []
		children: list[Node] = list(reversed(node.children))
		while children:
			child: Node = children.pop()
			if child.type in ("enum_body_declarations", "statement_list", "statements"):
				children.extend(reversed(child.children))
				continue
			if child not in spans:
				continue
			start: int
			end: int
			first_kind: str
			last_kind: str
			start, end, first_kind, last_kind = spans[child]
			if child.kind == "U" and child.type == "{":
				braces.append((end, False, True, last_kind))
			elif braces:
				previous: int
				has_content: bool
				needs_break: bool
				previous_kind: str
				previous, has_content, needs_break, previous_kind = braces[-1]
				is_closer: bool = child.kind == "U" and child.type == "}"
				# ラベルは後続文の見出，空文のセミコロンは直前の式の終端として扱う
				# PHP の地の文へ接する境界は表示値の保持を優先する
				if (
					needs_break and first_kind != "php_end_tag" and
					(child.kind == "N" and not (child.type == "empty_statement" and previous_kind == "}") or is_closer and has_content)
				):
					line: int = bisect_right(line_starts, start) - 1
					if line == bisect_right(line_starts, previous - 1) - 1:
						found.add((line, start - line_starts[line]))
				if is_closer:
					braces.pop()
				else:
					braces[-1] = end, has_content or child.kind == "N", child.type != "label" and last_kind != "php_tag", last_kind
	# 必須改行を欠く境界の返戻
	return found

def layout_boundaries(roots: list[Node]) -> tuple[set[Node], set[int]]:
	"""Go の分割可能な閉じと，本体でない JSX の式の開きを返す
	添字や式を囲む括弧は，カンマの挿入で構文が変わる為に含めない
	計算量：ノード数 N に対して O(N)
	引数：
		roots: 解析木の最上位の並び
	戻値：
		分割出来る閉括弧と，空本体の判定から除く JSX の式の開始位置
	"""
	found: set[Node] = set()
	expressions: set[int] = set()
	pending: list[Node] = list(roots)
	while pending:
		node: Node = pending.pop()
		if node.type in GO_COMMA_GROUPS or node.type in GO_STATEMENT_GROUPS:
			found.update(child for child in node.children if child.kind == "U" and child.text in (")", "]", "}"))
		if node.type == "jsx_expression":
			expressions.update(child.start for child in node.children if child.kind == "U" and child.text == "{")
		pending.extend(node.children)
	# 並びの所属で確定した境界の返戻
	return found, expressions

def header_damage(roots: list[Node]) -> int:
	"""共有ヘッダの解析で，エラーと成立しない関数宣言子を数える
	計算量：ノード数 N に対して O(N)
	引数：roots: 独立に解析した木
	戻値：文法上の破損の数
	"""
	damage: int = 0
	pending: list[Node] = list(roots)
	while pending:
		node: Node = pending.pop()
		if node.type in ("ERROR", "MISSING"):
			damage += 1
		elif node.type == "function_definition":
			# C の関数定義には仮引数部が必要で，型名と識別子だけでは成立しない
			declarators: list[Node] = [child for child in node.children if child.field == "declarator"]
			has_parameters: bool = False
			while declarators:
				declarator: Node = declarators.pop()
				has_parameters |= declarator.type == "function_declarator"
				declarators.extend(declarator.children)
			if not has_parameters:
				damage += 1
		pending.extend(node.children)
	# 破損数の返戻
	return damage

def spec_roots(ext: str, text: str) -> list[Node]:
	"""CSS 系の本文を独立に保護し，共有ヘッダは破損が減る場合だけ C++ を選ぶ
	計算量：解析費用 P とノード数 N に対して O(P+N)
	引数：ext: 文法の拡張子，text: 原文
	戻値：仕様から独立に選んだ文法の木
	"""
	if ext in ("css", "scss"):
		text, _ = css_value_source(ext, text, True)
	roots: list[Node] = parse_dump(dump_ast(text, "c" if ext == "h" else ext), text)
	if ext == "h" and (damage := header_damage(roots)):
		candidate: list[Node] = parse_dump(dump_ast(text, "cpp"), text)
		if header_damage(candidate) < damage:
			roots = candidate
	# 選んだ木の返戻
	return roots

def syntax_lines_work(path: str, lines: list[str]) -> Work:
	"""逐語本文とコメントを構文の行頭と見誤らない照合用の行を返す
	補間内の式は葉を辿り，逐語の断片だけを覆う
	コメント内の改行はグループの展開による改行と区別する
	引数：
		path: 言語を持つテストケースの経路
		lines: 原文の行
	戻値：
		逐語本文を伏せた行・コメント継続行・分割可能な閉じ・空の開閉の対応・本体の必須改行
	送出例外：
		RuntimeError: 解析又は原文の対応付が失敗する場合
		OSError: パーサーを起動出来ない場合
		ValueError: パーサーの出力を読めない場合
		subprocess.TimeoutExpired: パーサーが制限時間内に終わらない場合
	"""
	lang: str = path.replace("\\", "/").split("/")[-2]
	ext: str = {
		"csharp": "cs",
		"rust": "rs",
		"kotlin": "kt",
		"javascript": "js",
		"javascriptreact": "jsx",
		"typescript": "ts",
		"typescriptreact": "tsx",
		"ruby": "rb",
		"python": "py",
		"jsonc": "json"
	}.get(lang, lang)
	text: str = "\n".join(lines)
	chars: list[str] = list(text)
	line_starts: list[int] = [0, *(match.end() for match in re.finditer("\n", text))]
	continuations: set[int] = set()
	roots: list[Node] = spec_roots(ext, text)
	splittable_closers: set[Node]
	expressions: set[int]
	splittable_closers, expressions = layout_boundaries(roots)
	inline_bodies: set[tuple[int, int]] = body_break_positions(roots, line_starts, ext)
	closer_splits: set[tuple[int, int]]|None = set()
	observed_ranges: list[tuple[int, int]] = []
	source_observation_signature(roots, lang, observed_ranges=observed_ranges)
	observed: list[int] = [0] * (len(text) + 1)
	for begin, end in observed_ranges:
		observed[begin] += 1
		observed[end] -= 1
	for idx in range(1, len(observed)):
		observed[idx] += observed[idx - 1]
	previous_end: int = -1
	# 開閉の対応と空の内容は解析済のトークンから求め，閉じの接尾とは分ける
	empty_braces: set[tuple[int, int, int]] = set()
	braces: list[tuple[int, bool]] = []
	for node, start, end in source_leaves(roots):
		if not is_comment(node.type):
			if node.kind == "U" and node.text == "}" and braces:
				opening: int
				has_content: bool
				opening, has_content = braces.pop()
				if not has_content and opening not in expressions:
					first_line: int = bisect_right(line_starts, opening) - 1
					empty_braces.add((first_line, opening - line_starts[first_line], bisect_right(line_starts, start) - 1))
			else:
				if braces:
					braces[-1] = braces[-1][0], True
				if node.kind == "U" and node.text == "{":
					braces.append((start, False))
		# 文字列化で観測する密着は分割せず，其の他の閉じには構造規則を適用する
		if (
			node in splittable_closers if
			lang == "go" else
			node.text and node.text[0] in ")]}" and not (start == previous_end and observed[start] and start and observed[start - 1])
		):
			line: int = bisect_right(line_starts, start) - 1
			closer_splits.add((line, start - line_starts[line]))
		if not is_comment(node.type):
			previous_end = end
		if len(node.text) > 1 and all(char in ")]}" for char in node.text):
			chars[start + 1:end] = " " * (end - start - 1)
		is_comment_leaf: bool = is_comment(node.type)
		if is_comment_leaf:
			continuations.update(range(bisect_right(line_starts, start), bisect_left(line_starts, end)))
		if (
			is_comment_leaf or is_content_leaf(node.type) or
			node.type in ("attribute_value", "line_str_text", "multi_line_str_text", "text")
		):
			for idx in range(start, end):
				if not chars[idx].isspace() and unicodedata.category(chars[idx]) != "Cc":
					chars[idx] = " " if is_comment_leaf else "_"
	if lang == "html":
		for section in embedded(text):
			if section.should_inspect:
				body: list[str] = text[section.content_start:section.content_end].split("\n")
				child: SyntaxLines = (yield syntax_lines_work(f"{section.lang}/embedded.out", body))
				chars[section.content_start:section.content_end] = "\n".join(child.lines)
				first_line: int = bisect_right(line_starts, section.content_start) - 1
				continuations.update(first_line + line for line in child.continuations)
				first_column: int = section.content_start - line_starts[first_line]
				inline_bodies.update((first_line + line, column + (first_column if not line else 0)) for line, column in child.inline_bodies)
				empty_braces.update(
					(first_line + opening, column + (first_column if not opening else 0), first_line + closing) for opening, column, closing in
					child.empty_braces
				)
	# 行境界を保った照合用のテキストの返戻
	return SyntaxLines("".join(chars).split("\n"), continuations, closer_splits, empty_braces, inline_bodies)

def syntax_lines(path: str, lines: list[str]) -> SyntaxLines:
	"""逐語本文とコメントを構文の行頭と見誤らない照合用の行を返す
	補間内の式は葉を辿り，逐語の断片だけを覆う
	コメント内の改行はグループの展開による改行と区別する
	引数：
		path: 言語を持つテストケースの経路
		lines: 原文の行
	戻値：
		逐語本文を伏せた行・コメント継続行・分割可能な閉じ・空の開閉の対応・本体の必須改行
	送出例外：
		RuntimeError: 解析又は原文の対応付が失敗する場合
		OSError: パーサーを起動出来ない場合
		ValueError: パーサーの出力を読めない場合
		subprocess.TimeoutExpired: パーサーが制限時間内に終わらない場合
	"""
	# 行境界を保持した照合結果の返戻
	return evaluate(syntax_lines_work(path, lines))

def indent_of(line: str) -> int:
	"""行頭のタブ数を返す
	計算量：行の文字数 L に対して O(L)
	引数：
		line: 検査する１行のテキスト
	戻値：
		行頭のタブの数
	"""
	# タブ数の返戻
	return len(line) - len(line.lstrip("\t"))

def is_comment_line(line: str) -> bool:
	"""行の全体がコメントかを返す
	計算量：行の文字数 L に対して O(L)
	引数：
		line: 検査する１行のテキスト
	戻値：
		行の全体がコメントなら真
	"""
	# コメントのテキストの判定
	text: str = line.strip()
	if text.startswith(("#", "//")):
		# 行コメントの返戻
		return True
	if text.startswith("/*"):
		# 閉じで終わるブロックコメントだけを行コメントと同じに扱う事の返戻
		return text.endswith("*/") and text.index("*/") == len(text) - 2
	if text.startswith("*"):
		# ブロックコメントの継続行の返戻（コードの `*p = 1;` を除く）
		return not text.rstrip().endswith((")", ",", ";", "{", "}"))
	# HTML のコメントの返戻
	return text.startswith("<!--")

def check_empty_body(path: str, lines: list[str], width: int, code: SyntaxLines|None = None) -> list[str]:
	"""整形仕様 6.3 の「空の本体は畳む」への違反を返す
	波括弧以外の空の本体で，畳んだ形が行幅（生成するインデントを数えない，行分割仕様 3.2）を超える物は，行幅に依る選択（整形仕様 6.4）で折れる為に除く（空の本体の `{}` は幅に依らず展開しない）
	計算量：解析・逐語照合費用 P・行数 L・見出の語数 H に対して O(P+LH)
	引数：
		path: テストケース又は実装ファイルの経路
		lines: 検査する行の並び
		width: 整形の行幅
		code: 共有する照合用の行（省略時は解析する）
	戻値：
		空の本体を畳む条文への違反の一覧
	"""
	# 逐語領域を除いた見出と直後の閉トークンの探索
	parsed: SyntaxLines = syntax_lines(path, lines) if code is None else code
	lines = parsed.lines
	found: list[str] = []
	for idx, line in enumerate(lines):
		head: str = line.strip()
		if (
			not head or is_comment_line(line) or
			not (head.endswith(("do", "then", "{")) or any(head.startswith(h) for h in HEADS) and not head.endswith(("end", "}")))
		):
			continue
		# 空白とコメントだけを飛ばした次の行
		next_idx: int = idx + 1
		while next_idx < len(lines) and (not lines[next_idx].strip() or is_comment_line(lines[next_idx])):
			next_idx += 1
		if next_idx >= len(lines):
			continue
		close: str = lines[next_idx].strip()
		# `;` が要る為に畳めない形と，畳んだ形が行幅を超える波括弧以外の形は除く
		is_empty_braces: bool = (idx, len(line.rstrip()) - 1, next_idx) in parsed.empty_braces
		if not is_empty_braces and (
			head.endswith("{") or indent_of(lines[next_idx]) != indent_of(line) or close not in CLOSERS or
			any(pattern.match(head) for pattern in NO_FOLD) or
			width and sum(unicodedata.category(char) != "Cc" for char in head + close) + 1 > width
		):
			continue
		found.append(f"{path}:{idx + 1}: spec 6.3 empty body is not folded: {head} … {close}")
	# 違反の一覧の返戻
	return found

def check_closer_run(path: str, lines: list[str], code: SyntaxLines|None = None) -> list[str]:
	"""整形仕様 6.3 の「展開した閉括弧の後続」への違反を返す
	計算量：解析・逐語照合費用 P・行数 L に対して O(P+L)
	引数：
		path: テストケース又は実装ファイルの経路
		lines: 検査する行の並び
		code: 共有する照合用の行（省略時は解析する）
	戻値：
		展開した閉括弧の後続の条文への違反の一覧
	"""
	parsed: SyntaxLines = syntax_lines(path, lines) if code is None else code
	found: list[str] = []
	for idx, line in enumerate(parsed.lines):
		if idx in parsed.continuations:
			continue
		text: str = line.strip()
		# コメント・逐語内容を除いた行で，区切空白を跨いで閉トークンの後続を調べる
		if not text or text[0] not in ")]}":
			continue
		column: int = len(line) - len(line.lstrip())
		is_followed: bool = False
		is_splittable: bool = parsed.closer_splits is None
		for pos in range(1, len(text)):
			char: str = text[pos]
			if char.isspace():
				continue
			if char not in ")]}":
				break
			is_followed = True
			if parsed.closer_splits is None or (idx, column + pos) in parsed.closer_splits:
				is_splittable = True
				break
		if is_followed and is_splittable:
			found.append(f"{path}:{idx + 1}: spec 6.3 a closer follows a closer on the same line: {text}")
	# 違反の一覧の返戻
	return found

# 逐語領域と構文の境界から手で定めた正誤対（整形仕様 4.4，6.3）
SELF_CHECK_CASES: list[tuple[str, str, int, int, int]] = [
	("cpp", "#define S(x) #x\nconst char *text = S(({\nint a;\n})\n);\n", 0, 0, 20),
	("cpp", "#define S(x) #x\nconst char *text = S(({\nint a;\n} )\n);\n", 0, 1, 20),
	("cpp", "const char *text = f(({\nint a;\n}));\n", 0, 1, 20),
	("cpp", "void f(){g(h(\nx\n));}\n", 0, 1, 128),
	("cpp", "void f(){g(h(\nx\n)\n);}\n", 0, 0, 128),
	("cpp", "void f(){g(h(\nx\n) /* note */ );}\n", 0, 1, 128),
	("cpp", "void f(){g(h(\nx\n) + (x));}\n", 0, 0, 128),
	("go", "package p\nfunc h() {\n\tf(g(\n\t\taaaaaaaaaaaaa,\n\t) )\n}\n", 0, 1, 128),
	("cpp", "const char *s = u8R\"tag(\n))\n)tag\";\n", 0, 0, 128),
	("cpp", "void f(){ g(h(/* a\n * b */ )); }\n", 0, 0, 128),
	("cpp", "void f() {\n}\n", 1, 0, 10),
	("go", "package p\nfunc h() {\n\tf(g(\n\t\taaaaaaaaaaaaa,\n\t))\n}\n", 0, 1, 128),
	("go", "package p\nfunc h() {\n\tf(g(\n\t\taaaaaaaaaaaaa,\n\t),\n\t)\n}\n", 0, 0, 128),
	("go", "package p\nvar x = ([]int{\n1,\n})\n", 0, 0, 128),
	("go", "package p\nvar x = a[f(\n1,\n)]\n", 0, 0, 128),
	("go", "package p\nvar x = [][]int{{\n1,\n}}\n", 0, 1, 128),
	("go", "package p\nvar x = [][]int{{\n1,\n},\n}\n", 0, 0, 128),
	("go", "package p\nfunc h() {\n\tf((\n\t\t[]int{1,\n\t}))\n}\n", 0, 1, 128),
	("go", "package p\nfunc h() {\n\tf((\n\t\t[]int{1,\n\t}),\n\t)\n}\n", 0, 0, 128),
	("go", "package p\nfunc h(x func(\na int,\n)) {}\n", 0, 1, 128),
	("go", "package p\nfunc h(x func(\na int,\n),\n) {}\n", 0, 0, 128),
	("go", "package p\nfunc F[T interface {\nM()\n}]() {}\n", 0, 1, 128),
	("go", "package p\nfunc F[T interface {\nM()\n},\n]() {}\n", 0, 0, 128),
	("go", "package p\nfunc h(){F[struct{\nx int\n}](x)}\n", 0, 1, 128),
	("go", "package p\nfunc h(){F[struct{\nx int\n},\n](x)}\n", 0, 0, 128),
	("go", "package p\nfunc h(){if x {\nf()\n}}\n", 0, 1, 128),
	("go", "package p\nfunc h(){if x {\nf()\n}\n}\n", 0, 0, 128),
	("go", "package p\nvar (\nx=f(\n1,\n))\n", 0, 1, 128),
	("go", "package p\nvar (\nx=f(\n1,\n)\n)\n", 0, 0, 128),
	("go", "package p\nconst (\nx=len(\n\"abc\",\n))\n", 0, 1, 128),
	("go", "package p\nconst (\nx=len(\n\"abc\",\n)\n)\n", 0, 0, 128),
	("go", "package p\ntype (\nT struct {\nx int\n})\n", 0, 1, 128),
	("go", "package p\ntype (\nT struct {\nx int\n}\n)\n", 0, 0, 128),
	("go", "package p\ntype T struct {\nx struct {\ny int\n}}\n", 0, 1, 128),
	("go", "package p\ntype T struct {\nx struct {\ny int\n}\n}\n", 0, 0, 128),
	("go", "package p\ntype T interface {\nF(\nx int,\n)}\n", 0, 1, 128),
	("go", "package p\ntype T interface {\nF(\nx int,\n)\n}\n", 0, 0, 128),
	("rust", "#[derive(Debug /* d\n * e */ )]\nstruct S;\n", 0, 0, 128),
	("swift", "let s = \"\"\"\n))\n\"\"\"\n", 0, 0, 128),
	("javascript", "f(g(\nx\n));\n", 0, 1, 128),
	("javascript", "if(x){\n}\n", 1, 0, 128),
	("javascript", "f(g(\nx\n/* a\n * b */\n));\n", 0, 1, 128),
	("javascript", "f(g(\nx\n/* a */ ));\n", 0, 1, 128),
	("ruby", "def f()\nend\n", 0, 0, 10),
	("ruby", "def f()\nend\n", 1, 0, 11),
	("python", "text = \"\"\"\n))\nif x {\n}\n\"\"\"\n", 0, 0, 128),
	("html", "<script>\nf(g(/* a\n * b */ ));\n</script>\n", 0, 0, 128),
	("csharp", "value = $$\"\"\"\np{{\n\ta + b\n}}q\n\"\"\";\n", 0, 0, 128),
	("csharp", "value = $$\"\"\"\np{{new object[] {\n\ta\n}}}q\n\"\"\";\n", 0, 1, 128)
]

# 接尾の種類に依らず空の開閉を検出し，畳んだ対照は検出しない（整形仕様 6.3）
SELF_CHECK_CASES.extend(
	[
		("c", "void f(void) {\n} void g(void) {}\n", 1, 0, 128),
		("c", "void f(void) {} void g(void) {}\n", 0, 0, 128),
		("cpp", "struct X {\n} x;\n", 1, 0, 128),
		("cpp", "struct X {} x;\n", 0, 0, 128),
		("cpp", "auto x = [] {\n}();\n", 1, 0, 128),
		("cpp", "auto x = [] {}();\n", 0, 0, 128),
		("csharp", "class C { object[] x = new object[] {\n\tnew C {\n\t},\n}; }\n", 1, 0, 128),
		("csharp", "class C { object[] x = new object[] {\n\tnew C {},\n}; }\n", 0, 0, 128),
		("java", "class C { Object[] x = {\n\tnew Object() {\n\t},\n}; }\n", 1, 0, 128),
		("java", "class C { Object[] x = {\n\tnew Object() {},\n}; }\n", 0, 0, 128),
		("go", "package p\nvar x = []struct{\n}{\n\t{\n\t},\n}\n", 2, 0, 128),
		("go", "package p\nvar x = []struct{}{\n\t{},\n}\n", 0, 0, 128),
		("rust", "struct S; fn f(){let x = [S {\n}, S {}];}\n", 1, 0, 128),
		("rust", "struct S; fn f(){let x = [S {}, S {}];}\n", 0, 0, 128),
		("kotlin", "val x = listOf({\n}, { 1 })\n", 1, 0, 128),
		("kotlin", "val x = listOf({}, { 1 })\n", 0, 0, 128),
		("swift", "let x: [()->Void] = [{\n}, { print(1) }]\n", 1, 0, 128),
		("swift", "let x: [()->Void] = [{}, { print(1) }]\n", 0, 0, 128),
		("php", "<?php $x = [function() {\n}, function(){return 1;}];\n", 1, 0, 128),
		("php", "<?php $x = [function() {}, function(){return 1;}];\n", 0, 0, 128),
		("javascript", "const x = [\n\t{\n\t},\n\t{\n\t}\n];\n", 2, 0, 128),
		("javascript", "const x = [\n\t{},\n\t{}\n];\n", 0, 0, 128),
		("python", "x = [\n\t{\n\t},\n\t{\n\t}\n]\n", 2, 0, 128),
		("python", "x = [\n\t{},\n\t{}\n]\n", 0, 0, 128),
		("json", "[\n\t{\n\t},\n\t{\n\t}\n]\n", 2, 0, 128),
		("json", "[\n\t{},\n\t{}\n]\n", 0, 0, 128),
		("javascript", "try {\n} catch(e) {}\n", 1, 0, 128),
		("javascript", "try {} catch(e) {}\n", 0, 0, 128),
		("javascript", "do {\n} while(x);\n", 1, 0, 128),
		("javascript", "do {} while(x);\n", 0, 0, 128),
		("html", "<script>const x = {\n};</script>\n", 1, 0, 128),
		("html", "<script>const x = {};</script>\n", 0, 0, 128)
	]
)

# 制御文字の幅は０，通常の逐語テキストの幅は１として空の本体を照合する
for control in "\t", "\x85":
	for header in "if \"{}\" then", "def f(x=\"{}\")":
		SELF_CHECK_CASES.extend(
			[
				("ruby", header.format(control * 32) + "\nend\n", 1, 0, 20),
				("ruby", header.format(control * 32) + " end\n", 0, 0, 20),
				("ruby", header.format("x" * 32) + "\nend\n", 0, 0, 20)
			]
		)

# JSX のコメントを収める埋込式は本体でなく，コメントの継続行を畳む対象でない（整形仕様 4.4）
SELF_CHECK_CASES.append(("typescriptreact", "const x = <p>{/* first\n * second */}</p>;\n", 0, 0, 128))

def check_inline_body(path: str, lines: list[str], code: SyntaxLines|None = None) -> list[str]:
	"""非空の文・宣言本体の必須改行への違反を返す
	計算量：解析費用 P・検出数 B に対して O(P+B log B)
	引数：
		path: テストケース又は実装ファイルの経路
		lines: 検査する行の並び
		code: 共有する照合用の行（省略時は解析する）
	戻値：
		本体の展開への違反の一覧
	"""
	parsed: SyntaxLines = syntax_lines(path, lines) if code is None else code
	# 必須改行を欠く境界の一覧の返戻
	return [
		f"{path}:{line + 1}:{column + 1}: required body newline is missing: {lines[line].strip()}" for line, column in
		sorted(parsed.inline_bodies)
	]

# 文・宣言の本体と式の囲みを区別する手書の正誤対
INLINE_BODY_CASES: list[tuple[str, str, int]] = [
	("c", "void f() {\nif(x) return;\n}\n", 0),
	("c", "void f() { ;\n}\n", 1),
	("c", "void f() {\ng(); ;\n}\n", 1),
	("cpp", "auto f = [] { g(); };\n", 0),
	("cpp", "enum E { A, B }; int a[] = {1, 2};\n", 0),
	("cpp", "S(({ int a; a; }));\n", 0),
	("java", "class C { void f() {\ng();\n} }\n", 2),
	("java", "class C { int x; }\n", 2),
	("go", "package p\ntype T struct { x int; y int }\n", 3),
	("go", "package p\ntype I interface { M() }\n", 2),
	("rust", "struct P { x: i32, y: i32 }\n", 0),
	("rust", "struct P(i32, i32);\n", 0),
	("rust", "trait T { fn f(); }\n", 2),
	("go", "package p\nfunc f() {\nfor { /* note */ }\n}\n", 0),
	("go", "package p\nfunc f() {\nif x { g() } else { h() }\n}\n", 4),
	("go", "package p\nvar f = func() { g() }\n", 0),
	("rust", "fn f() {\nlet f = || { g(); };\nm!{if x {g();}};\n}\n", 0),
	("kotlin", "fun f() {\ntry { a() } catch(e: E) { b() } finally { c() }\n}\n", 6),
	("kotlin", "val x = items.map { it * 2 }\n", 0),
	("swift", "let x = items.map { $0 * 2 }\n", 0),
	("php", "<?php __halt_compiler(); if($x){y();}\n", 0),
	("javascript", "function f() { /* note */ }\n", 0),
	("javascript", "const x = { value: 1 }; const f = () => { g(); };\n", 0),
	("typescript", "const x: { value: number } = { value: 1 };\n", 0),
	("typescriptreact", "const x = <p>{value}</p>; const y = `${value}`;\n", 0),
	("scss", ".x {\n\tmargin: 0 -#{$size}px;\n\tsrc: URL(//cdn/#{$img}/x.woff2);\n}\n", 0),
	("scss", ".x {\n\tmargin: 0 -#{f(1)}px;\n}\n", 0)
]

INLINE_BODY_CASES.extend(
	[
		("h", "class Point { public: int x; int y; };", 4),
		("csharp", "class C {\nint X { get; set; }\n}\n", 0),
		("csharp", "struct S { int x; }\n", 2),
		("java", "record R(int x) { static int y; }\n", 2),
		("kotlin", "class C { val x = 1 }\n", 2),
		("rust", "union U { a: i32 }\n", 0),
		("swift", "struct S { var x: Int }\n", 2),
		("swift", "protocol P { func f() }\n", 2),
		("typescript", "abstract class A { x = 1; }\n", 2),
		("swift", "protocol P {\nvar x: Int { get set }\n}\n", 0),
		("php", "<?php class C { public $x; }\n", 2),
		("javascript", "class C { x = 1; }\n", 2),
		("typescript", "interface I { a: number, b: string }\n", 0),
		("h", "class Point { void f() { g(); } };", 4),
		("kotlin", "fun f() {\nouter@ while(true) {\ng()\n}\n}\n", 0),
		("kotlin", "fun f() { outer@ while(true) {\ng()\n}\n}\n", 1),
		("php", "<?php function f() {\n?><p><?= $x; ?></p><?php return;\n}\n", 0),
		("php", "<?php function f() {\n?><p><?= $x; ?></p><?php return; }\n", 1),
		("rust", "fn f() {\nmatch x {\n_ => {}\n};\n}\n", 0),
		("rust", "fn f() {\nmatch x {\n_ => {}\n}; g();\n}\n", 1)
	]
)

# 登録漏れの本体で，異なる２つの内容の境界を独立に畳む
BODY_ROLE_CASES: list[tuple[str, str, str, str, str]] = [
	("java", "class C {\nObject x = new Object() {", "int x;", "int y;", "};\n}"),
	("java", "enum E {", "A;", "int x;", "}"),
	("java", "enum E {\nA {", "int x;", "int y;", "}\n}"),
	("kotlin", "enum class E {", "A;", "fun f() {}", "}"),
	("swift", "enum E {", "case a", "func f() {}", "}"),
	("php", "<?php enum E {", "case A;", "public const X = 1;", "}"),
	("javascript", "const C = class {", "x = 1;", "y = 2;", "};"),
	("javascript", "class C {\nstatic {", "let x = 1;", "let y = 2;", "}\n}"),
	("css", "@font-face {", "font-family: X;", "src: url(x);", "}"),
	("css", "@page {", "margin: 1px;", "padding: 2px;", "}"),
	("css", "@property --x {", "syntax: \"<color>\";", "inherits: false;", "}"),
	("css", "@counter-style x {", "system: cyclic;", "symbols: \"x\";", "}"),
	("css", "@layer x {", "a {\ncolor: red;\n}", "b {\ncolor: blue;\n}", "}"),
	("css", "@supports(display: grid) {", "a {\ncolor: red;\n}", "b {\ncolor: blue;\n}", "}"),
	("css", "@keyframes x {", "from {\ncolor: red;\n}", "to {\ncolor: blue;\n}", "}"),
	("css", "@keyframes x {\nfrom {", "color: red;", "opacity: 1;", "}\n}"),
	("scss", "@mixin X($x) {", "color: red;", "width: $x;", "}"),
	("scss", "@function X($x) {", "$y: $x;", "@return $y;", "}"),
	("scss", "@include X {", "color: red;", "opacity: 1;", "}"),
	("scss", "@each $x in (a, b) {", "color: red;", "width: $x;", "}")
]

# 本体の種類毎に開側・文間・閉側だけを畳んだ独立した反例
BODY_BREAK_CASES: list[tuple[str, str, str, str]] = [
	("c", "void f() {", "g();", "}"),
	("cpp", "namespace N {", "int x;", "}"),
	("cpp", "void f() {\ntry {", "a();", "} catch(...) {}\n}"),
	("cpp", "void f() {\ntry {} catch(...) {", "b();", "}\n}"),
	("csharp", "class C {\nint P {\nget {", "return 1;", "}\n}\n}"),
	("java", "class C {\nvoid f() {", "g();", "}\n}"),
	("go", "package p\nfunc f() {\nfor {", "g();", "}\n}"),
	("rust", "fn f() {\nloop {", "g();", "}\n}"),
	("kotlin", "fun f() {\ntry {} finally {", "g();", "}\n}"),
	("swift", "func f() {\nguard x else {", "return;", "}\n}"),
	("php", "<?php function f() {", "g();", "}"),
	("javascript", "function f() {", "g();", "}"),
	("typescript", "class C {\nf() {", "g();", "}\n}"),
	("html", "<script>function f() {", "g();", "}</script>"),
	("html", "<style>a {", "color: red;", "}</style>"),
	("css", "a {", "color: red;", "}")
]

def self_check() -> list[str]:
	"""逐語の内容と実際の展開違反を区別出来るかを返す
	戻値：
		条文と食い違う検査結果
	"""
	found: list[str] = hidden_entry_self_check() + alphanumeric_entry_self_check() + unicode_entry_self_check()
	for lang, text, empty_count, closer_count, width in SELF_CHECK_CASES:
		path: str = f"test_data/golden/{lang}/self_check.out"
		lines: list[str] = text.split("\n")
		code: SyntaxLines = syntax_lines(path, lines)
		if len(check_empty_body(path, lines, width, code)) != empty_count or len(check_closer_run(path, lines, code)) != closer_count:
			found.append(f"{lang}: 逐語領域と構文の展開の判定が条文と違う")
	for lang, text, count in INLINE_BODY_CASES:
		path: str = f"test_data/golden/{lang}/self_check.out"
		if len(check_inline_body(path, text.split("\n"))) != count:
			found.append(f"{lang}: 本体の必須改行の判定が条文と違う: {text!r}")
	for lang, head, statement, tail in BODY_BREAK_CASES:
		for opening, middle, closing, count in ("\n", "\n", "\n", 0), (" ", "\n", "\n", 1), ("\n", " ", "\n", 1), ("\n", "\n", " ", 1):
			text: str = head + opening + statement + middle + statement + closing + tail
			if len(check_inline_body(f"{lang}/boundary.out", text.split("\n"))) != count:
				found.append(f"{lang}: 本体の必須改行の判定が条文と違う: {text!r}")
	for lang, head, first, second, tail in BODY_ROLE_CASES:
		for opening, middle, closing, count in ("\n", "\n", "\n", 0), (" ", "\n", "\n", 1), ("\n", " ", "\n", 1), ("\n", "\n", " ", 1):
			text = head + opening + first + middle + second + closing + tail
			if len(check_inline_body(f"{lang}/role.out", text.split("\n"))) != count:
				found.append(f"{lang}: 本体の所有者又はラッパーの判定が違う: {text!r}")
	for lang, text in (
		("java", "enum E { A, B; }"),
		("kotlin", "enum class E { A, B }"),
		("swift", "enum E { case a, b }"),
		("php", "<?php enum E {case A; case B;}"),
		("javascript", "const C = class {}; class D {\nstatic {}\n}"),
		("css", "@font-face {} @keyframes x {}"),
		("scss", "@mixin X($x) {} @include X {}")
	):
		if check_inline_body(f"{lang}/element.out", text.split("\n")):
			found.append(f"{lang}: 列挙子だけ又は空の本体の同行形を拒否した: {text!r}")
	# 不一致の返戻
	return found

def main() -> int:
	"""期待結果を条文と照合し，違反が有れば非零で終える
	計算量：対象ファイル数 F・総行数 L・解析と逐語照合の総費用 P に対して O(F log F+L+P)
	戻値：
		成功なら０，違反又は失敗が有れば非零の終了値
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
	"""
	# 起動した場所に依らず作業木の根からテストケースを探す
	os.chdir(Path(__file__).resolve().parent.parent)
	# 期待出力の収集と条文違反の照合
	violations: list[str] = self_check()
	# 表示の共通表の生成物と定義元の一致
	violations += check_generated(Path.cwd())
	total: int = 0
	# 言語の判定を経路の区切に依らせない様に，Windows の `\` も `/` へ揃える
	for path in sorted(found.replace("\\", "/") for found in glob.glob("test_data/golden/*/*.out")):
		# 見送のテストケースは原文返戻で，整形の規則を適用しない
		if glob.glob(path[:-4] + ".skip") or glob.glob(path[:-4] + ".partial"):
			continue
		lines: list[str] = Path(path).read_text(encoding="utf-8").split("\n")
		total += 1
		# テストケース毎の行幅（`.chars` が無ければ既定の `128`，`0` は無制限）
		width: int = int(Path(path[:-4] + ".chars").read_text()) if glob.glob(path[:-4] + ".chars") else 128
		code: SyntaxLines = syntax_lines(path, lines)
		violations += check_empty_body(path, lines, width, code)
		violations += check_closer_run(path, lines, code)
		violations += check_inline_body(path, lines, code)
	# 違反と検査件数の報告
	for violation in violations:
		print(f"FAIL golden spec: {violation}", file=sys.stderr)
	print(
		f"golden spec: {total} expected outputs checked against 3 rules (empty body, closer run, body newline), "
		f"{len(violations)} violations"
	)
	# 違反が有れば非零の返戻
	return 1 if violations else 0

if __name__ == "__main__":
	sys.exit(main())
