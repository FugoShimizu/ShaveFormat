#! /usr/bin/env python3
"""名前付の規則と同じ型名を持つ無名のトークンを，名前の確認無で型名比較して居る箇所を検出する

tree-sitter の文法では `decltype` (C++) や `type` (Python / TS) の様に，名前付のノードと無名のトークンが同じ型名を持つ事が有る
走査は無名のトークンも訪れる為，型名だけで見分けて子を引くと，子を持たないトークンから空ノードが返り異常終了する
型名を直に比べる形と，１度変数へ束ねてから比べる形の双方を見る
"""

from cpp_lex import CppSource, CppToken, cpp_code_tokens, cpp_source, logical_tokens
from tree_work import Work, evaluate
from verify_semantic_preservation import Node, dump_ast, has_error, parse_dump

import argparse

from bisect import bisect_right

import glob
import io
import os
import re
import sys

from typing import TypeAlias
from unittest.mock import patch

# 検査の経路と実行設定
ROOT: str = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 構築で使う全文法の独立した期待（PHP 単体の別文法を重複して数えない）
GRAMMARS: tuple[tuple[str, str], ...] = (
	("c", "c/src"),
	("cpp", "cpp/src"),
	("c_sharp", "c-sharp/src"),
	("java", "java/src"),
	("go", "go/src"),
	("rust", "rust/src"),
	("kotlin", "kotlin/src"),
	("swift", "swift/src"),
	("php", "php/php/src"),
	("javascript", "javascript/src"),
	("typescript", "typescript/typescript/src"),
	("tsx", "typescript/tsx/src"),
	("ruby", "ruby/src"),
	("python", "python/src"),
	("json", "json/src"),
	("html", "html/src"),
	("scss", "scss/src")
)

def parser_paths(build_dir: str) -> list[str]:
	"""構築が選んだ記号表を優先し，全１７文法の対応と取得を照合する
	計算量：パスの総文字数 L に対して O(L)
	引数：
		build_dir: 構築の生成先
	戻値：
		文法毎の parser.c のパス
	送出例外：
		SystemExit: 文法の不足・重複・余分な登録又は記号表の欠落が有る場合
		OSError: 文法の目録を読めない場合
	"""
	# 文法の登録と実ファイルの照合
	paths: dict[str, str] = {}
	manifest: str = os.path.join(build_dir, "grammar-parsers.tsv")
	if os.path.isfile(manifest):
		with open(manifest, encoding="utf-8") as src:
			for line in src:
				name: str
				path: str
				name, path = line.rstrip("\n").split("\t")
				if name in paths:
					raise SystemExit("文法の重複：" + name)
				paths[name] = path
	else:
		for name, subdir in GRAMMARS:
			generated: str = os.path.join(build_dir, "generated", name, "parser.c")
			grammar: str = subdir.split("/", 1)[0]
			script: str = os.path.join(ROOT, "tools", "prepare" + grammar[:1].upper() + grammar[1:] + ".mjs")
			paths[name] = (
				generated if
				os.path.isfile(generated) or os.path.isfile(script) else
				os.path.join(ROOT, "vendor/tree-sitter-" + subdir, "parser.c")
			)
	expected: set[str] = { name for name, _ in GRAMMARS }
	if set(paths) != expected:
		raise SystemExit(
			"文法の登録が一致しない：不足 " + ", ".join(sorted(expected - paths.keys())) + " ／ 余分 " + ", ".join(sorted(paths.keys() - expected))
		)
	for name, path in paths.items():
		if not os.path.isfile(path):
			raise SystemExit("文法の記号表が無い：" + name + "：" + path)
	# 全文法を確認した記号表の返戻
	return list(paths.values())

def grammar_self_check() -> list[str]:
	"""文法の不足・重複・余分な登録と，拡張した記号表の選択を正誤対で確かめる
	計算量：検査用のパスの総長 L に対して O(L)
	戻値：
		チェッカーが誤って受理又は拒否した入力の説明
	"""
	# 製品が使う全文法の登録の検査
	found: list[str] = []
	names: tuple[str, ...] = (
		"c",
		"cpp",
		"c_sharp",
		"java",
		"go",
		"rust",
		"kotlin",
		"swift",
		"php",
		"javascript",
		"typescript",
		"tsx",
		"ruby",
		"python",
		"json",
		"html",
		"scss"
	)
	manifest: str = "".join(name + "\tselected/" + name + "/parser.c\n" for name in names)
	for text, missing, is_valid in (
		(manifest, "", True),
		(manifest.replace("swift\tselected/swift/parser.c\n", ""), "", False),
		(manifest + "swift\tselected/swift/parser.c\n", "", False),
		(manifest + "php_only\tselected/php_only/parser.c\n", "", False),
		(manifest, "selected/swift/parser.c", False)
	):
		with patch("builtins.open", return_value=io.StringIO(text)), patch("os.path.isfile", side_effect=lambda path: path != missing):
			try:
				paths: list[str] = parser_paths("build")
				if not is_valid or paths != ["selected/" + name + "/parser.c" for name in names]:
					found.append("文法の登録又は選択したパスを誤って受理した")
			except SystemExit:
				if is_valid:
					found.append("全文法の正しい登録を拒否した")
	# 生成済の文法の在る入口では vendor より先に其の記号表を読む
	with patch("os.path.isfile", side_effect=lambda path: not path.endswith("grammar-parsers.tsv")):
		paths = parser_paths("build")
		for name in "cpp", "swift", "html", "scss":
			if os.path.join("build", "generated", name, "parser.c") not in paths:
				found.append("拡張した文法を選ばなかった：" + name)
	# 文法の選択と登録の正誤対の結果の返戻
	return found

def collision_names(build_dir: str) -> tuple[set[str], int]:
	"""文法毎に「名前付と無名の双方に現れる型名」を集める

	`node-types.json` は子の型として現れた無名のトークンしか載せず葉のトークンを取零す為，文法の記号表から採る
	計算量：文法の総バイト数 B に対し O(B)
	引数：
		build_dir: 構築の生成先
	戻値：
		衝突する型名と検査した文法数
	送出例外：
		SystemExit: 記号表の項目数と取得数が一致しない場合
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
	"""
	# 文法の記号表とメタデータの収集
	path: str
	src: io.TextIOWrapper
	key: str
	body: str
	names: set[str] = set()
	grammars: int = 0
	for path in parser_paths(build_dir):
		with open(path, encoding="utf-8", errors="replace") as src:
			text: str = src.read()
		symbols: re.Match[str]|None = re.search(r"ts_symbol_names\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
		metadata: re.Match[str]|None = re.search(r"ts_symbol_metadata\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
		if not symbols or not metadata:
			raise SystemExit("文法の記号表を読めない：" + path)
		entries: list[tuple[str, str]] = re.findall(r"\[(\w+)\]\s*=\s*\"((?:[^\"\\]|\\.)*)\"", symbols.group(1))
		# エスケープを含むトークンを取零すと後続の項目迄呑み込む為，記号の総数と読めた数の一致を確かめる
		if len(entries) != len(re.findall(r"^\s*\[\w+\]\s*=", symbols.group(1), re.M)):
			raise SystemExit("記号表を読み落とした：%s" % os.path.relpath(path, ROOT))
		# 名前付と無名の可視記号の集計
		table: dict[str, str] = dict(entries)
		named: set[str] = set()
		anonymous: set[str] = set()
		for key, body in re.findall(r"\[(\w+)\]\s*=\s*\{(.*?)\}", metadata.group(1), re.S):
			name: str|None = table.get(key)
			if name is None or ".visible = true" not in body:
				continue
			(named if ".named = true" in body else anonymous).add(name)
		names |= named & anonymous
		grammars += 1
	# 衝突する型名の集合と，記号表を読めた文法の数の返戻
	return names, grammars

def alias_comparisons(
	roots: list[Node],
	calls: dict[int, tuple[str, str]]|None = None,
	bindings: dict[Node, str]|None = None,
	scope_targets: dict[Node, set[str]]|None = None
) -> dict[int, tuple[str, str, str]]:
	"""構文上の宣言・書込・条件付の所属から型名比較の入力を投影する
	確定しない枝の代入は危険な由来を消さず，照合不能として残す
	計算量：ノード数 N と参照名の文字数 B に対して O(N+B)
	引数：
		roots: 子の役割と原文の範囲を持つ C++ の解析木
		calls: 解決した API と対象の格納先
		bindings: 宣言毎に区別した参照先の格納先
		scope_targets: スコープ内で宣言した参照先の格納先
	戻値：
		比較の原文位置から，危険な由来・比較する型名・演算子への対応
	送出例外：
		ValueError: 危険な型名の書込先又は API の呼出先の束縛を確定出来ない場合
	"""
	values: dict[Node, str] = {}
	targets: dict[Node, str] = {}
	apis: tuple[str, ...] = "ts_node_child", "ts_node_is_named", "ts_node_named_child", "ts_node_type"
	aliases: dict[int, str] = {}
	scopes: list[dict[int, str|None]] = [{}]
	functions: list[dict[int, str|None]] = []
	declared: list[dict[str, int]] = [{}]
	scope_nodes: list[Node|None] = [None]
	root_scopes: dict[int, Node|None] = {}
	root_slots: dict[int, int] = {}
	slots: dict[Node, int] = {}
	identities: dict[tuple[int, str, str], int] = {}
	def slot(parent: int, name: str, kind: str) -> int:
		"""修飾の鎖を複写せず，直属の参照へ識別子を与える
		計算量：直属の名前の文字数 B に対して O(B)
		引数：
			parent: 参照の直前の所属の識別子
			name: 直属の名前
			kind: 修飾の形式
		戻値：
			同じ所属と形式の名前で共有する識別子
		"""
		key: tuple[int, str, str] = parent, name, kind
		if key not in identities:
			identities[key] = len(identities) + 1
		# 直属の参照の識別子の返戻
		return identities[key]
	found: dict[int, tuple[str, str, str]] = {}
	node: Node
	child: Node
	is_ready: bool
	is_conditional: bool
	pending: list[tuple[Node, bool, bool]] = [(root, False, False) for root in reversed(roots)]
	while pending:
		node, is_ready, is_conditional = pending.pop()
		child: Node
		named: list[Node] = Node.get_named_children(node)
		if not is_ready:
			if node.type in ("function_definition", "lambda_expression"):
				functions.append({})
			if node.type in (
				"catch_clause",
				"class_specifier",
				"compound_statement",
				"for_range_loop",
				"for_statement",
				"function_definition",
				"if_statement",
				"lambda_expression",
				"struct_specifier",
				"switch_statement",
				"while_statement"
			):
				scopes.append({})
				declared.append({})
				scope_nodes.append(node)
			if node.type in ("declaration", "field_declaration", "parameter_declaration"):
				for child in named:
					if child.field != "declarator":
						continue
					declarator: Node|None = child
					while declarator.children and declarator.type not in ("field_identifier", "identifier"):
						next_declarator: Node|None = next((part for part in declarator.children if part.field == "declarator"), None)
						if next_declarator is None:
							break
						declarator = next_declarator
					declarations: list[Node] = (
						Node.get_named_children(declarator) if declarator.type == "structured_binding_declarator" else [declarator]
					)
					for binding in declarations:
						if binding.type not in ("field_identifier", "identifier"):
							continue
						name: int|str|None = slot(binding.start + 1, binding.text, "binding")
						declared[-1][binding.text] = name
						root_scopes[name] = scope_nodes[-1]
						if functions:
							functions[-1].setdefault(name, aliases.get(name))
						scopes[-1].setdefault(name, aliases.get(name))
						aliases[name] = "@" + binding.text if binding.text in apis else ""
			pending.append((node, True, is_conditional))
			for child in reversed(named):
				is_branch: bool = is_conditional or (
					node.type == "if_statement" and child.field in ("consequence", "alternative") or
					node.type in ("conditional_expression", "switch_statement", "try_statement") or
					node.type in ("do_statement", "for_range_loop", "for_statement", "while_statement") and child.field == "body"
				)
				pending.append((child, False, is_branch))
			continue
		value: str = ""
		target: str = "*"
		if node.type in ("field_identifier", "identifier", "namespace_identifier"):
			resolved: int|None = next((scope[node.text] for scope in reversed(declared) if node.text in scope), None)
			slots[node] = resolved or slot(0, node.text, "identifier")
			value = aliases.get(slots[node], "@" + node.text if node.text in apis else "")
			target = "@target:" + str(root_slots.get(slots[node], slots[node])) + ":" + str(slots[node])
		elif node.type in ("field_expression", "qualified_identifier"):
			if len(named) == 2 and named[0] in slots and not named[1].children:
				qualification: str = "".join(child.text for child in node.children if child.text in (".", "->", "::"))
				slots[node] = slot(slots[named[0]], named[1].text, node.type + qualification)
				root_scopes[slots[node]] = root_scopes.get(slots[named[0]])
				root_slots[slots[node]] = root_slots.get(slots[named[0]], slots[named[0]])
				value = aliases.get(slots[node], "")
			if len(named) == 1 and node.type == "qualified_identifier":
				value = "@" + named[0].text if node.text.startswith("::") and named[0].text in apis else values[named[0]]
				if named[0] in slots:
					slots[node] = slots[named[0]]
			if node in slots:
				target = "@target:" + str(root_slots.get(slots[node], slots[node])) + ":" + str(slots[node])
		elif node.type.endswith("_declarator") and node.type != "init_declarator":
			# 宣言のポインタ等のラッパーは，書込先の所属だけを内側と共有する
			declarator = next((child for child in named if child.field == "declarator"), None)
			if declarator in slots:
				slots[node] = slots[declarator]
		elif node.type == "call_expression":
			function: Node|None = next((child for child in named if child.field == "function"), None)
			args: Node|None = next((child for child in named if child.field == "arguments"), None)
			operands: list[Node] = Node.get_named_children(args) if args is not None else []
			callee: str = values.get(function, "")
			argument: str = targets[operands[0]] if operands else "*"
			if callee == "*":
				raise ValueError("照合不能：API の呼出先の束縛を確定出来ない")
			if callee.startswith("@") and callee[1:] in apis:
				if calls is not None:
					calls[node.start] = callee[1:], argument
				if callee == "@ts_node_type":
					value = argument if len(operands) == 1 else "*"
			elif function is not None and len(operands) == 1 and (
				function.text in ("std::move", "std::forward") or
				function.type == "template_function" and function.children[0].text in ("const_cast", "static_cast")
			):
				target = argument
				value = values[operands[0]]
			elif args is not None and any(values[child] for child in operands):
				# 未確認の呼出が危険な文字列を無害化したと決め付けない
				value = "*"
		elif node.type in ("parenthesized_expression", "initializer_list", "argument_list", "pointer_expression") and len(named) == 1:
			value = values[named[0]]
			target = targets[named[0]]
		elif node.type == "cast_expression":
			operand: Node|None = next((child for child in named if child.field == "value"), None)
			if operand is not None:
				target = targets[operand]
				value = values[operand]
		elif node.type in ("assignment_expression", "init_declarator"):
			left: Node|None = next((child for child in named if child.field in ("declarator", "left")), None)
			right: Node|None = next((child for child in named if child.field in ("right", "value")), None)
			if (
				left is not None and right is not None and
				(left.type in ("identifier", "field_expression", "qualified_identifier") or node.type == "init_declarator")
			):
				name = slots.get(left)
				if name is None:
					if values[right]:
						raise ValueError("照合不能：型名の書込先の所属を確定出来ない")
					values[node] = ""
					continue
				value = values[right]
				if functions:
					functions[-1].setdefault(name, aliases.get(name))
				previous: str|None = aliases.get(name, "")
				# 異なる枝の由来を安全な定義で上書きしたと決め付けない
				if is_conditional and previous != value:
					value = "*" if previous or value else ""
				aliases[name] = value
			elif any(values[child] for child in named):
				raise ValueError("照合不能：型名の書込先の所属を確定出来ない")
		elif node.type == "conditional_expression":
			alternatives: list[str] = [values[child] for child in named if child.field in ("consequence", "alternative")]
			value = alternatives[0] if alternatives and all(item == alternatives[0] for item in alternatives) else "*"
		elif node.type == "binary_expression" and len(named) == 2:
			operator: str = next((child.text for child in node.children if child.field == "operator"), "")
			left, right = named
			if left.type == "string_literal":
				left, right = right, left
			if operator in ("==", "!=") and right.type == "string_literal":
				name = right.text[1:-1]
				if values[left]:
					found[node.start] = values[left], name, operator
		elif node.type.endswith("_expression") and any(values[child] for child in named):
			# 明示した透過以外の演算では，危険な由来を消さない
			value = "*"
		values[node] = value
		targets[node] = target
		if bindings is not None:
			bindings[node] = target
		if scope_targets is not None and node in slots and (owner := root_scopes.get(slots[node])) is not None:
			scope_targets.setdefault(owner, set()).add("@target:" + str(root_slots.get(slots[node], slots[node])) + ":" + str(slots[node]))
		if node.type in (
			"catch_clause",
			"class_specifier",
			"compound_statement",
			"for_range_loop",
			"for_statement",
			"function_definition",
			"if_statement",
			"lambda_expression",
			"struct_specifier",
			"switch_statement",
			"while_statement"
		):
			declared.pop()
			scope_nodes.pop()
			for name, previous in scopes.pop().items():
				if previous is None:
					aliases.pop(name, None)
				else:
					aliases[name] = previous
		if node.type in ("function_definition", "lambda_expression"):
			for name, previous in functions.pop().items():
				if previous is None:
					aliases.pop(name, None)
				else:
					aliases[name] = previous
	# 比較の由来と位置の返戻
	return found

# 経路上で成立し得る型名比較と，全経路で保証された名前付の対象
GuardState: TypeAlias = tuple[frozenset[int], frozenset[str]]

def unsafe_comparisons(
	roots: list[Node],
	comparisons: dict[int, tuple[str, str, str]],
	names: set[str],
	calls: dict[int, tuple[str, str]]|None = None,
	work: list[int]|None = None
) -> set[int]:
	"""短絡・分岐・早期移動の到達経路から，名前付の保証無で子を取得する比較を集める
	未知の分岐は両経路を調べ，保証は合流で共通する物だけを保つ
	危険と確定した由来は次の照合と経路の集合から除き，直列の再走査を避ける
	計算量：事実の異なる文・式の訪問数 V と各経路の比較・保証数 F に対し O(V(F+1))
	引数：
		roots: 呼出と比較の対象を持つ解析木
		comparisons: 比較の位置と型名の由来
		names: 名前付と無名で衝突する型名
		calls: 共有する API と対象の対応
		work: 未判定の比較の照合数の格納先
	戻値：
		子の取得へ到達する危険な比較の位置
	送出例外：
		ValueError: goto の到達先の保証又は API の呼出先の束縛を確定出来ない場合
	"""
	if calls is None:
		calls = {}
	bindings: dict[Node, str] = {}
	scope_targets: dict[Node, set[str]] = {}
	alias_comparisons(roots, calls, bindings, scope_targets)
	pending: dict[str, set[int]] = {}
	for pos, (target, name, _) in comparisons.items():
		if name in names:
			pending.setdefault(target, set()).add(pos)
	root: Node
	unsafe: set[int] = set()
	if not any(value[1] in names for value in comparisons.values()):
		# 危険な型名比較の無い空の結果の返戻
		return unsafe
	def merge(left: GuardState|None, right: GuardState|None) -> GuardState|None:
		"""生存経路の比較を合併し，名前付の保証を共通部分へ絞る
		引数：left: 左経路の事実，right: 右経路の事実，終了した経路は `None`
		戻値：生存経路に依る比較の和と保証の共通部分，両経路が終了して居れば `None`
		"""
		if left is None:
			# 生存する右経路の返戻
			return right
		if right is None:
			# 生存する左経路の返戻
			return left
		# 両経路で共有する保証と比較の返戻
		return left[0] - unsafe | right[0] - unsafe, left[1] & right[1]
	def expression(state: GuardState|None, node: Node) -> Work:
		"""式の評価順と短絡に従い，真と偽の後の事実を返す
		引数：state: 式へ到達する比較と保証の事実，node: 評価する式のノード
		戻値：下位の評価を要求する作業，完了値は真・偽の各経路の事実の組
		送出例外：ValueError: 式に含む文の goto の到達先を確定出来ない場合
		"""
		if state is None:
			# 到達しない式の真偽の経路の返戻
			return None, None
		truth: GuardState|None
		falsehood: GuardState|None
		child: Node
		target_node: Node
		named: list[Node] = Node.get_named_children(node)
		if node.type in ("true", "false"):
			# 真偽値で確定した経路の返戻
			return (state, None) if node.type == "true" else (None, state)
		operator: str = next((child.text for child in node.children if child.field == "operator"), "")
		if node.type in ("condition_clause", "parenthesized_expression") and len(named) == 1:
			# 透過する条件の真偽の返戻
			return (yield expression(state, named[0]))
		if node.type == "unary_expression" and operator == "!" and len(named) == 1:
			truth, falsehood = yield expression(state, named[0])
			# 否定した真偽の経路の返戻
			return falsehood, truth
		if node.type == "binary_expression" and len(named) == 2 and operator in ("&&", "||"):
			truth, falsehood = yield expression(state, named[0])
			right_true: GuardState|None
			right_false: GuardState|None
			right_true, right_false = yield expression(truth if operator == "&&" else falsehood, named[1])
			if operator == "&&":
				# 論理積の真の経路と偽の合流の返戻
				return right_true, merge(falsehood, right_false)
			# 論理和の真の合流と偽の経路の返戻
			return merge(truth, right_true), right_false
		if node.type == "binary_expression" and len(named) == 2 and operator in ("==", "!="):
			literal: Node|None = next((child for child in named if child.type in ("true", "false")), None)
			if literal is not None:
				operand: Node = named[1] if named[0] is literal else named[0]
				truth, falsehood = yield expression(state, operand)
				# 真偽値との比較に従う経路の返戻
				return (truth, falsehood) if (literal.type == "true") == (operator == "==") else (falsehood, truth)
		if node.type == "conditional_expression":
			condition: Node = next(child for child in named if child.field == "condition")
			truth, falsehood = yield expression(state, condition)
			left: Node = next(child for child in named if child.field == "consequence")
			right: Node = next(child for child in named if child.field == "alternative")
			left_true: GuardState|None
			left_false: GuardState|None
			right_true: GuardState|None
			right_false: GuardState|None
			left_true, left_false = yield expression(truth, left)
			right_true, right_false = yield expression(falsehood, right)
			# 条件式の各結果へ合流した経路の返戻
			return merge(left_true, right_true), merge(left_false, right_false)
		if node.type in ("function_definition", "lambda_expression"):
			# 関数の別の呼出へ，外側で成立した保証を流用しない
			for child in named:
				if child.field == "body":
					yield statement((frozenset(), frozenset()), child)
			# 外側の式の経路の返戻
			return state, state
		for child in named:
			truth, falsehood = yield expression(state, child)
			state = merge(truth, falsehood)
		if state is None:
			# 評価が終了した式の経路の返戻
			return None, None
		if node.type == "call_expression":
			callee: str
			target: str
			callee, target = calls.get(node.start, ("", "*"))
			if callee in ("ts_node_child", "ts_node_named_child"):
				keys: tuple[str, ...] = () if target in state[1] else tuple(pending)
				for key in keys:
					candidates: set[int] = pending.get(key, set()) & state[0]
					if work is not None:
						work[0] += len(candidates)
					unsafe.update(candidates)
					if key in pending:
						pending[key].difference_update(candidates)
				state = state[0] - unsafe, state[1]
			if callee == "ts_node_is_named" and target != "*":
				# 条件で合流した生存経路の返戻
				return (state[0], state[1] | { target }), None if target in state[1] else (state[0], state[1] - { target })
		if node.start not in unsafe and node.start in comparisons and comparisons[node.start][1] in names:
			positive: GuardState = state[0] | { node.start }, state[1]
			# 型名との一致が成立する経路の返戻
			return (positive, state) if comparisons[node.start][2] == "==" else (state, positive)
		if node.type in ("assignment_expression", "declaration", "init_declarator", "update_expression"):
			# 書込と新しい宣言は対象の保証を失効させ，別の対象の保証は保つ
			targets: list[Node] = [child for child in named if child.field in ("declarator", "left")]
			if node.type == "update_expression":
				targets = named
			pending_targets: list[Node] = list(reversed(targets))
			while pending_targets:
				target_node: Node = pending_targets.pop()
				while target_node.type.endswith("_declarator"):
					declarator: Node|None = next((child for child in target_node.children if child.field == "declarator"), None)
					if declarator is None:
						break
					target_node = declarator
				if target_node.type == "structured_binding_declarator":
					pending_targets.extend(reversed(Node.get_named_children(target_node)))
					continue
				bound: str = bindings.get(target_node, "*")
				state = (
					state[0],
					frozenset(guard for guard in state[1] if not guard.startswith(bound.rsplit(":", 1)[0] + ":")) if
					target_node.type == "identifier" and bound != "*" else
					frozenset()
				)
		# 真偽を確定しない式の経路の返戻
		return state, state
	cache: dict[tuple[Node, GuardState|None], GuardState|None] = {}
	def statement(state: GuardState|None, node: Node) -> Work:
		"""同じ文と入口の事実の結果を共有し，反復の再照会で本体を再走査しない
		引数：state: 文へ到達する比較と保証の事実，node: 評価する文のノード
		戻値：下位の評価を要求する作業，完了値は文から復帰する経路の事実又は `None`
		送出例外：ValueError: 文中の goto の到達先を確定出来ない場合
		"""
		if state is not None:
			state = state[0] - unsafe, state[1]
		key: tuple[Node, GuardState|None] = node, state
		if key not in cache:
			cache[key] = yield statement_core(state, node)
		# 入口の事実が同じ文の結果の返戻
		return cache[key]
	def statement_core(state: GuardState|None, node: Node) -> Work:
		"""文の順と制御の所属に従って生存する経路を返す
		引数：state: 文へ到達する比較と保証の事実，node: 評価する文のノード
		戻値：下位の評価を要求する作業，完了値は生存する経路の事実又は `None`
		送出例外：ValueError: goto の到達先の名前付の保証を確定出来ない場合
		"""
		if state is None:
			# 到達しない文の経路の返戻
			return None
		truth: GuardState|None
		falsehood: GuardState|None
		child: Node
		named: list[Node] = Node.get_named_children(node)
		if node.type == "function_definition":
			for child in named:
				if child.field == "body":
					yield statement((frozenset(), frozenset()), child)
			# 関数外の元の経路の返戻
			return state
		if node.type == "if_statement":
			condition: Node = next(child for child in named if child.field == "condition")
			truth, falsehood = yield expression(state, condition)
			consequence: Node = next(child for child in named if child.field == "consequence")
			alternative: Node|None = next((child for child in named if child.field == "alternative"), None)
			# 条件で合流した生存経路の返戻
			return merge(
				(yield statement(truth, consequence)),
				(yield statement(falsehood, alternative)) if alternative is not None else falsehood
			)
		if node.type == "goto_statement":
			raise ValueError("照合不能：goto の到達先の名前付の保証を確定出来ない")
		if node.type in ("break_statement", "continue_statement", "return_statement", "throw_statement"):
			for child in named:
				yield expression(state, child)
			# 早期移動で終了した経路の返戻
			return None
		if node.type == "do_statement":
			body: Node = next(child for child in named if child.field == "body")
			condition: Node = next(child for child in named if child.field == "condition")
			entry: GuardState|None = yield statement((state[0], frozenset()), body)
			truth, falsehood = yield expression(entry, condition)
			# 後置の条件を初回の本体の保証へ使わず，次回の到達も調べる
			yield statement(truth, body)
			# 反復を終える経路の保守的な合流の返戻
			return merge(state, falsehood)
		if node.type in ("for_range_loop", "for_statement", "switch_statement", "while_statement"):
			# 反復・分岐の未確定な次回へ保証を持越さず，零回の経路も合流する
			entry: GuardState|None = state[0], frozenset()
			body: Node|None = next((child for child in named if child.field == "body"), None)
			for child in named:
				if child is body or child.field == "update":
					continue
				truth, falsehood = yield expression(entry, child)
				entry = truth if child.field == "condition" else merge(truth, falsehood)
			after_body: GuardState|None = (yield statement(entry, body)) if body is not None else entry
			for child in named:
				if child.field == "update":
					yield expression(merge(entry, after_body), child)
			# 零回と本体を通る経路の合流の返戻
			return merge(state, after_body)
		if node.type in ("case_statement", "try_statement"):
			# 異なる入口の保証を混ぜない
			for child in named:
				yield statement((state[0], frozenset()), child)
			# 入口の異なる分岐の未確定な保証の返戻
			return state[0], frozenset()
		if node.type in ("compound_statement", "else_clause", "translation_unit"):
			for child in named:
				state = yield statement(state, child)
			# スコープ内の束縛の保証を外側へ持ち越さない経路の返戻
			return None if state is None else (state[0], state[1] - scope_targets.get(node, set()))
		truth, falsehood = yield expression(state, node)
		# 式文の結果を捨てた後の経路の返戻
		return merge(truth, falsehood)
	for root in roots:
		evaluate(statement((frozenset(), frozenset()), root))
	# 名前付の保証が無い比較の返戻
	return unsafe

def risky_text(names: set[str], text: str, is_statement_fragment: bool = False) -> list[tuple[int, str, str]]:
	"""型名比較の別名を，宣言の有効範囲と代入順に追跡する
	コメント・文字列内の偽の比較は除き，型名の引用だけを比較の入力として読む
	計算量：事実毎の訪問数 V・文字数 L・比較数 C・経路毎の事実数 F に対して O(L+V(F+1)+C log(L+1))
	引数：
		names: 衝突する型名
		text: 調べる C++ ソース
		is_statement_fragment: 文の断片を検査用の関数本体へ包む場合は真
	戻値：
		原文の行番号・衝突する型名・比較の行
	送出例外：
		ValueError: 構文・不透明なマクロ・書込先の所属・API の束縛を確定出来ない場合
		RuntimeError: パーサーが失敗した場合
		OSError: パーサーを起動出来ない場合
	"""
	source: CppSource = cpp_source(text)
	tokens: list[CppToken] = list(logical_tokens(source.text))
	code: str = cpp_code_tokens(tokens)
	lines: list[str] = source.text.split("\n")
	starts: list[int] = [0, *(match.end() for match in re.finditer("\n", source.text))]
	physical: list[int] = [0, *(match.end() for match in re.finditer("\n", text))]
	# テキストの代入の推測ではなく，構文の宣言と書込の所属を読む
	prefix: str = "void VerifierFragment(){\n" if is_statement_fragment else ""
	parsed_source: str = prefix + source.text + ("\n}" if is_statement_fragment else "")
	roots: list[Node] = parse_dump(dump_ast(parsed_source, "cpp"), parsed_source)
	if has_error(roots):
		raise ValueError("型名比較の C++ 構文を照合不能")
	# 不透明なマクロで，構文の束縛を証明出来ない型名の参照を成功へ混ぜない
	pending_nodes: list[Node] = list(reversed(roots))
	while pending_nodes:
		node: Node = pending_nodes.pop()
		if node.type == "preproc_arg" and re.search(r"\bts_node_type\b", code[node.start - len(prefix):node.end - len(prefix)]):
			raise ValueError("照合不能：未展開マクロ内の型名の所属を確定出来ない")
		pending_nodes.extend(reversed(node.children))
	calls: dict[int, tuple[str, str]] = {}
	comparisons: dict[int, tuple[str, str, str]] = alias_comparisons(roots, calls)
	unsafe: set[int] = unsafe_comparisons(roots, comparisons, names, calls)
	excerpts: list[str] = [line.strip()[:100] for line in lines]
	found: list[tuple[int, str, str]] = []
	position: int
	for position in comparisons:
		if position not in unsafe:
			continue
		pos: int = position - len(prefix)
		row: int = bisect_right(physical, source.offsets[pos])
		idx: int = bisect_right(starts, pos) - 1
		found.append((row, comparisons[position][1], excerpts[idx]))
	# 危険な比較の返戻
	return found

def risky_lines(names: set[str]) -> list[tuple[str, int, str, str]]:
	"""衝突する型名で見分けたノードの子を，名前の確認無で引いて居る箇所を集める
	計算量：事実毎の総訪問数 V・文字数 L・比較数 C・経路毎の事実数 F に対して O(L+V(F+1)+C log(L+1))
	引数：
		names: 衝突する型名
	戻値：
		危険な比較の位置と内容
	送出例外：
		OSError: ソースを読めない場合
	"""
	path: str
	src: io.TextIOWrapper
	rows: list[tuple[str, int, str, str]] = []
	for path in (
		glob.glob(os.path.join(ROOT, "src/**/*.cpp"), recursive=True) + glob.glob(os.path.join(ROOT, "src/**/*.hpp"), recursive=True)
	):
		with open(path, encoding="utf-8") as src:
			rows.extend((os.path.relpath(path, ROOT), *row) for row in risky_text(names, src.read()))
	# 危険な比較の一覧の返戻
	return rows

def alias_self_check() -> list[str]:
	"""別名の上書・有効範囲と非コード領域の正誤対を照合する
	戻値：
		手書の危険な比較の件数と違う物の列
	"""
	found: list[str] = []
	for text, expected in (
		("auto Type = ts_node_type(Node); if(Type == \"type\") ts_node_named_child(Node, 0);", 1),
		("auto Type = ts_node_type(Node); Type = Other; if(Type == \"type\") ts_node_child(Node, 0);", 0),
		("auto Type = NamedTypeOf(Node); Type = ts_node_type(Node); if(Type == \"type\") ts_node_child(Node, 0);", 1),
		("auto Type = ts_node_type(Node); { auto Type = NamedTypeOf(Node); } if(Type == \"type\") ts_node_child(Node, 0);", 1),
		("void f(){ auto Type = ts_node_type(Node); } void g(){ if(Type == \"type\") ts_node_child(Node, 0); }", 0),
		("auto Type=ts_node_type(Node); { const char *Type; if(Type==\"type\") ts_node_child(Node,0); }", 0),
		("auto Type=ts_node_type(Node); Obj.Type=Other; if(Type==\"type\") ts_node_child(Node,0);", 1),
		("auto Type=ts_node_type(Node); if(flag) Type=Other; if(Type==\"type\") ts_node_child(Node,0);", 1),
		("auto Type=Other; if(flag) Type=ts_node_type(Node); if(Type==\"type\") ts_node_child(Node,0);", 1),
		("auto Type=Other; Type=NamedTypeOf(Node); if(Type==\"type\") ts_node_child(Node,0);", 0),
		("Obj.Type=ts_node_type(Node); if(Obj.Type==\"type\") ts_node_child(Node,0);", 1),
		("auto Type=ts_node_type(Node); if(auto Type=Other; flag) {} if(Type==\"type\") ts_node_child(Node,0);", 1),
		("const char *Type=ts_node_type(Node); if(Type==\"type\") ts_node_child(Node,0);", 1),
		("const char *Type=ts_node_type(Node); Type=Other; if(Type==\"type\") ts_node_child(Node,0);", 0),
		("auto Fn=ts_node_type; auto Type=Fn(Node); if(Type==\"type\") ts_node_child(Node,0);", 1),
		("auto Type=Wrap(ts_node_type(Node)); if(Type==\"type\") ts_node_child(Node,0);", 1),
		("auto Type=ts_node_type(Node); if(((const char *)Type)==\"type\") ts_node_child(Node,0);", 1),
		("/* if(ts_node_type(Node) == \"type\") ts_node_child(Node, 0); */", 0),
		("auto s = R\"(if(ts_node_type(Node) == \"type\") ts_node_child(Node, 0);)\";", 0),
		("if(ts_node_type /* gap */ (Node) == \"type\") ts_node_child(Node, 0);", 1)
	):
		if len(risky_text({ "type" }, text, not text.startswith("void "))) != expected:
			found.append(text)
	name: str = "type"
	getter: str = "ts_node_child"
	prefix: str = "std::string_view Type=ts_node_type(Node); "
	child: str = getter + "(Node,0);"
	# 真の経路・短絡・文の順・早期返戻と，左右の比較を同じ構文から読む
	for tail, expected in (
		("if(!ts_node_is_named(Node) && Type==\"NAME\") CHILD", 1),
		("if(ts_node_is_named(Node) || Type==\"NAME\") CHILD", 1),
		("ts_node_is_named(Node); if(Type==\"NAME\") CHILD", 1),
		("if(Type==\"NAME\") {CHILD ts_node_is_named(Node);}", 1),
		("if(ts_node_is_named(Node)){}else if(Type==\"NAME\") CHILD", 1),
		("if(ts_node_is_named(Node) && Type==\"NAME\") CHILD", 0),
		("if(!ts_node_is_named(Node)) return; if(Type==\"NAME\") CHILD", 0),
		("if(!ts_node_is_named(Other)) return; if(Type==\"NAME\") CHILD", 1),
		("if(!ts_node_is_named(Node)) return; Node=Other; if(Type==\"NAME\") CHILD", 1),
		("if(!ts_node_is_named(Node)) return; Type=ts_node_type(Node); if(Type==\"NAME\") CHILD", 0),
		("if(ts_node_is_named(Node) && Type==\"NAME\") {TSNode Node=Other; CHILD}", 1),
		("if(Type==\"NAME\") {if(!ts_node_is_named(Node)) return; CHILD}", 0),
		("if(false && Type==\"NAME\") CHILD", 0),
		("if(Type==\"NAME\") do {CHILD} while(ts_node_is_named(Node));", 1),
		("if(Type==\"NAME\") while(ts_node_is_named(Node)) {CHILD}", 0),
		("if(Type==\"NAME\") for(;ts_node_is_named(Node);Node=Other) {CHILD}", 0),
		("if(Type==\"NAME\") for(;flag;UPDATE) {}", 1),
		("if(ts_node_is_named(Node)==true && Type==\"NAME\") CHILD", 0),
		("if(false==ts_node_is_named(Node)) return; if(Type==\"NAME\") CHILD", 0),
		("if(!ts_node_is_named(Node)) return; if(!ts_node_is_named(Node) && Type==\"NAME\") CHILD", 0)
	):
		text: str = prefix + tail.replace("NAME", name).replace("CHILD", child).replace("UPDATE", child[:-1])
		if len(risky_text({ name }, text, True)) != expected:
			found.append(text)
	for comparison, tail in (
		("Type==\"type\"", child),
		("Type!=\"type\"", "return; " + child),
		("\"type\"==Type", child),
		("\"type\"!=Type", "return; " + child),
		("std::string_view(ts_node_type(Node))==\"type\"", child),
		("std::string_view(Fn(Node))==\"type\"", child)
	):
		text = "auto Fn=ts_node_type; " + prefix + f"if({comparison}) " + tail
		if len(risky_text({ name }, text, True)) != 1:
			found.append(text)
	for text, expected in (
		(
			"struct C { void f() { auto Type=ts_node_type(Node); if(ts_node_is_named(Node) && Type==\"type\") ts_node_child(Node,0); } };",
			0
		),
		(
			"struct C { void f() { auto Type=ts_node_type(Node); if(!ts_node_is_named(Node) && Type==\"type\") ts_node_child(Node,0); } };",
			1
		)
	):
		if len(risky_text({ "type" }, text)) != expected:
			found.append(text)
	for text in "void f(){*Target=ts_node_type(Node);}", "#define Raw(Node) ts_node_type(Node)\nvoid f(){}":
		try:
			risky_text({ "type" }, text)
		except ValueError:
			pass
		else:
			found.append("照合不能な型名の所属を成功にした")
	# 対象と呼出先の同じ透過・束縛の事実を，危険な取得と安全な保証の双方で使う
	prefix = f"auto Get={getter}; auto Type=ts_node_type(Node); "
	for target, callee in (
		("Node", getter),
		("Node", "::" + getter),
		("(Node)", "(" + getter + ")"),
		("((Node))", "(*" + getter + ")"),
		("Node", "(&" + getter + ")"),
		("std::move(Node)", "(*&" + getter + ")"),
		("static_cast<TSNode>(Node)", "Get")
	):
		text = prefix + f'if(Type=="{name}") {callee}({target},0);'
		if len(risky_text({ name }, text, True)) != 1:
			found.append(text)
	for target, guard in (
		("Node", "ts_node_is_named"),
		("(Node)", "::ts_node_is_named"),
		("((Node))", "(ts_node_is_named)"),
		("std::move(Node)", "(*&ts_node_is_named)"),
		("static_cast<TSNode>(Node)", "Named")
	):
		text = f'auto Named=ts_node_is_named; {prefix}if({guard}({target}) && Type=="{name}") ts_node_child(Node,0);'
		if risky_text({ name }, text, True):
			found.append(text)
	text = f'auto Type=::ts_node_type(Node); if(Type=="{name}") ts_node_child(Node,0);'
	if len(risky_text({ name }, text, True)) != 1:
		found.append("型名の取得を子の取得と別の所属とした")
	for text in (
		"auto Get=ts_node_child; Get=Other; auto Type=ts_node_type(Node); if(Type==\"type\") Get(Node,0);",
		"auto Named=ts_node_is_named; Named=Other; auto Type=ts_node_type(Node); if(Named(Node) && Type==\"type\") ts_node_child(Node,0);"
	):
		expected: int = 0 if text.startswith("auto Get") else 1
		if len(risky_text({ "type" }, text, True)) != expected:
			found.append(text)
	for guard in "Other::ts_node_is_named(Node)", "Holder.ts_node_is_named(Node)":
		text: str = "std::string_view Type=ts_node_type(Node); if(" + guard + " && Type==\"type\") ts_node_child(Node,0);"
		if len(risky_text({ "type" }, text, True)) != 1:
			found.append("別の関数の同名の末尾を名前付の保証とした")
	for api in "ts_node_child", "ts_node_type":
		text = f'void f(){{auto {api}=Other; auto Type=::ts_node_type(Node); if(Type=="type") ::ts_node_child(Node,0);}}'
		if len(risky_text({ "type" }, text, True)) != 1:
			found.append("ローカルの束縛でグローバル名前空間の API の呼出を隠した")
	text = "void f(){auto ts_node_is_named=Other; auto Type=ts_node_type(Node); if(::ts_node_is_named(Node) && Type==\"type\") ts_node_child(Node,0);}"
	if risky_text({ "type" }, text, True):
		found.append("グローバル名前空間の名前付の保証をローカルの束縛と混同した")
	# 遮蔽した対象の保証と，外側の正しい保証を区別する正誤対
	for (
		target,
		declaration
	) in ("Node", "TSNode Node=Other;"), ("Obj.Node", "Holder Obj=OtherObj;"), ("Ptr->Node", "Holder *Ptr=OtherPtr;"):
		prefix = "void f(TSNode Node,TSNode Other,Holder Obj,Holder OtherObj,Holder *Ptr,Holder *OtherPtr){"
		prefix += f"auto Type=ts_node_type({target});"
		inner: str = "{" + declaration + f"if(!ts_node_is_named({target}))return;" + "}"
		for guard, expected in ("", 1), (f"if(!ts_node_is_named({target}))return;", 0):
			text = prefix + f'if(Type=="{name}")' + "{" + guard + inner + f"{getter}({target},0);" + "}}"
			if len(risky_text({ name }, text)) != expected:
				found.append("遮蔽と外側の保証：" + text)
	# 同じ根でも，直接のメンバと operator-> が返す先のメンバの保証は混ぜない
	for guard, expected in ("Obj.Node", 1), ("Obj->Node", 0):
		text = (
			"void f(Proxy Obj){auto Type=ts_node_type(Obj->Node);if(ts_node_is_named(" + guard +
			") && Type==\"type\")ts_node_child(Obj->Node,0);}"
		)
		if len(risky_text({ "type" }, text)) != expected:
			found.append("メンバの参照記号の異なる保証：" + text)
	# 構造化束縛も，同じ名前の外側の対象とは別の宣言として読む
	for guard, expected in ("", 1), ("if(!ts_node_is_named(Node))return;", 0):
		text = (
			"void f(TSNode Node,TSNode Other){auto Type=ts_node_type(Node);if(Type==\"type\"){" + guard +
			"{auto [Node,Other2]=Pair;if(!ts_node_is_named(Node))return;}ts_node_child(Node,0);}}"
		)
		if len(risky_text({ "type" }, text)) != expected:
			found.append("構造化束縛と外側の保証：" + text)
	# 正誤対の不一致の返戻
	return found

def main() -> int:
	"""無名のトークンと型名が衝突する危険な比較を検査する
	計算量：記号表の総バイト数 B・訪問数 V・文字数 L・比較数 C・経路毎の事実数 F に対して O(B+L+V(F+1)+C log(L+1))
	戻値：
		終了コード
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		SystemExit: 呼出先の検査が継続出来ず終了を要求する場合
	"""
	# 文法数と危険な比較の確認
	problem: str
	path: str
	num: int
	name: str
	text: str
	for problem in grammar_self_check() + alias_self_check():
		print("FAIL node type alias:", problem)
		# 別名の自己検査の失敗の返戻
		return 1
	names: set[str]
	grammars: int
	args: argparse.ArgumentParser = argparse.ArgumentParser()
	args.add_argument("--build-dir", default=os.path.join(ROOT, "build"))
	names, grammars = collision_names(args.parse_args().build_dir)
	if grammars != len(GRAMMARS):
		print("文法の記号表の数が一致しない：%d / %d" % (grammars, len(GRAMMARS)))
		# 文法数の不一致の返戻
		return 1
	rows: list[tuple[str, int, str, str]] = risky_lines(names)
	print("文法 %d 個 ／ 衝突する型名 %d 語 ／ 名前の確認が無い型名比較 %d 件" % (grammars, len(names), len(rows)))
	for path, num, name, text in rows:
		print("  %s:%d [%s] %s" % (path, num, name, text))
	# 検出が有れば失敗とする終了コードの返戻（走査が無名のトークンを渡す為，子を引くと落ちる）
	return 1 if rows else 0

if __name__ == "__main__":
	sys.exit(main())
