#! /usr/bin/env python3
"""vendor の corpus を元の文法と拡張した文法で解析し，意図しない構文木の変更を検出する"""

import argparse
import json
import pathlib
import re
import subprocess
import sys
import tempfile

from collections.abc import Iterator
from unittest.mock import patch

# 許可は入力の識別子と変更前後の S 式を完全一致で限定する
# 新しい拡張が既存の corpus を直す場合だけ，仕様から定めた差を此処へ追加する
ALLOWED_CHANGES: dict[tuple[str, str, str], tuple[str, str]] = {}

# 先頭のクラスの誤読だけを訂正し，後続の木は全文を同一性の照合へ残す
ALLOWED_GOLDEN_PREFIXES: dict[tuple[str, str], tuple[str, str]] = {
	("typescript", "typescript/comment_declaration_member_anchors.in"): (
		"(program (class_declaration name: (type_identifier) body: (class_body " +
		"(public_field_definition (comment) name: (property_identifier) value: (number)) " +
		"(public_field_definition name: (property_identifier)) (comment) " +
		"(public_field_definition name: (property_identifier) value: (number)))) ",
		"(program (class_declaration name: (type_identifier) body: (class_body " +
		"(public_field_definition name: (property_identifier)) (comment) " +
		"(public_field_definition name: (property_identifier) value: (number)) " +
		"(public_field_definition name: (property_identifier)) (comment) " +
		"(public_field_definition name: (property_identifier) value: (number)))) "
	)
}

# 文法の拡張に依らない構文木の保存を検査する対象
GRAMMARS: dict[str, tuple[str, str]] = {
	"cpp": ("cpp/src", "prepareCpp.mjs"),
	"swift": ("swift/src", "prepareSwift.mjs"),
	"typescript": ("typescript/typescript/src", "prepareTypescript.mjs"),
	"tsx": ("typescript/tsx/src", "prepareTypescript.mjs"),
	"html": ("html/src", "prepareHtml.mjs"),
	"scss": ("scss/src", "prepareScss.mjs")
}

def corpus_cases(text: str) -> Iterator[tuple[str, str]]:
	"""corpus の見出と入力を取得し，不完全なテストケースを拒否する
	計算量：corpus の文字数 L に対して O(L)
	引数：
		text: corpus の全テキスト
	戻値：
		見出と入力の並び
	送出例外：
		ValueError: 見出又は期待の区切が欠ける場合
	"""
	# 見出と入力の対応の検査
	markers: list[re.Match[str]] = list(re.finditer(r"^={3,}[^\S\r\n]*\r?$", text, re.M))
	if not markers or len(markers) % 2:
		raise ValueError("corpus header is missing or incomplete")
	for idx in range(0, len(markers), 2):
		header: str = text[markers[idx].end():markers[idx + 1].start()].strip()
		end: int = markers[idx + 2].start() if idx + 2 < len(markers) else len(text)
		body: str = text[markers[idx + 1].end():end]
		separator: re.Match[str]|None = re.search(r"^-{3,}[^\S\r\n]*\r?$", body, re.M)
		if not header or separator is None or not body[separator.end():].strip():
			raise ValueError("corpus input or expectation is missing: " + header)
		yield header, body[:separator.start()].strip("\r\n") + "\n"

def sexpression(binary: pathlib.Path, source: str, grammar: str) -> str:
	"""指定した文法の S 式を，解析の実行結果から取得する
	計算量：パーサーの処理量 P と出力長 L に対して O(P+L)
	引数：
		binary: 元又は拡張した文法をリンクした dumpast
		source: corpus の入力
		grammar: 解析する文法
	戻値：
		パーサーが返した S 式
	送出例外：
		OSError: 子プロセスを起動出来ない場合
		RuntimeError: パーサーが失敗又は空の出力を返した場合
	"""
	# 独立したパーサーでの構文木の取得
	extension: str = "ts" if grammar == "typescript" else grammar
	result: subprocess.CompletedProcess[str] = subprocess.run(
		[str(binary), "--sexp", "-", extension],
		input=source,
		capture_output=True,
		text=True,
		encoding="utf-8",
		check=False
	)
	if result.returncode or result.stderr or not result.stdout.strip():
		raise RuntimeError(f"{binary}: parser failed ({result.returncode}): {result.stderr}")
	# 入力を加工しないパーサーの S 式の返戻
	return result.stdout.strip()

def is_allowed_change(key: tuple[str, str, str], before: str, after: str) -> bool:
	"""同一の木，又は許可表の完全に一致する変更かを照合する
	計算量：S 式の長さ L に対して O(L)
	引数：
		key: 文法・corpus の相対パス・見出
		before: 元の文法の S 式
		after: 拡張した文法の S 式
	戻値：
		保存又は意図した変更であれば真
	"""
	# 同一性又は限定した許可との一致の返戻
	return before == after or ALLOWED_CHANGES.get(key) == (before, after)

def is_allowed_golden_change(key: tuple[str, str], before: str, after: str) -> bool:
	"""宣言した先頭の木の訂正と，残りの全文の保存を照合する
	計算量：S 式の長さ L に対して O(L)
	引数：
		key: 文法・golden の相対パス
		before: 元の文法の S 式
		after: 拡張した文法の S 式
	戻値：
		保存又は限定した訂正であれば真
	"""
	if before == after:
		# 同一の木の返戻
		return True
	if key not in ALLOWED_GOLDEN_PREFIXES:
		# 宣言の無い変更の拒否
		return False
	old, new = ALLOWED_GOLDEN_PREFIXES[key]
	# 先頭の訂正と，後続の全文の同一性の返戻
	return before.startswith(old) and after == new + before[len(old):]

def verify_cpp_inputs(source_dir: pathlib.Path, original: pathlib.Path, extended: pathlib.Path) -> tuple[int, int]:
	"""クラスの実体化の役割・宣言子の必須範囲と，既存の正常な入力の木を照合する
	計算量：ファイル数 F・パスの最大長 D・入力の総長 L・解析量 P・S 式の総長 S に対して O(F D log(F+1)+L+P+S)
	引数：
		source_dir: golden の入力と期待を持つ作業ツリー
		original: 元の文法をリンクした dumpast
		extended: 拡張した文法をリンクした dumpast
	戻値：
		検査した入力数と不一致の件数
	送出例外：
		OSError: 入力又はパーサーを扱えない場合
		RuntimeError: パーサーが失敗した場合
		UnicodeDecodeError: golden を UTF-8 として読み込めない場合
	"""
	# 型指定子・名前・型引数の既存の役割から手で定めた構文木
	names: tuple[tuple[str, str], ...] = (
		(
			"A::B<C>",
			"(qualified_identifier scope: (namespace_identifier) name: (template_type name: (type_identifier) " +
			"arguments: (template_argument_list (type_descriptor type: (type_identifier)))))"
		),
		(
			"B<C>",
			"(template_type name: (type_identifier) arguments: (template_argument_list (type_descriptor type: (type_identifier))))"
		),
		(
			"B<C>::D<E>",
			"(qualified_identifier scope: (template_type name: (type_identifier) " +
			"arguments: (template_argument_list (type_descriptor type: (type_identifier)))) " +
			"name: (template_type name: (type_identifier) arguments: (template_argument_list (type_descriptor type: (type_identifier)))))"
		)
	)
	count: int = 0
	failures: int = 0
	for kind in "class", "struct", "union":
		for prefix in "", "extern ":
			for name, tree in names:
				source: str = f"{prefix}template {kind} {name};\n"
				expected: str = f"(translation_unit (template_instantiation type: ({kind}_specifier name: {tree})))"
				after: str = sexpression(extended, source, "cpp")
				count += 1
				if after != expected:
					failures += 1
					print("FAIL C++ class instantiation:", source.strip(), "\nexpected:", expected, "\nafter:", after)
	# クラスの指定子だけでない形では，宣言子の欠落を引き続き検出する
	for declaration in "class B<C> const", "class B<C> {}", "class [[attr]] B<C>", "const class B<C>", "enum E", "int", "void":
		for prefix in "", "extern ":
			source = f"{prefix}template {declaration};\n"
			after = sexpression(extended, source, "cpp")
			count += 1
			if not re.search(r"\((?:ERROR|MISSING)(?:\s|\))", after):
				failures += 1
				print("FAIL C++ missing declarator accepted:", source.strip(), "\nafter:", after)
	# 戻値型の無いコンストラクタと，型付きの関数・変数の枝の保存
	for source in (
		"extern template int Value<int>;\n",
		"extern template void F<int>();\n",
		"template A<int>::A(int);\n",
		"template class B<C> F();\n",
		"template class B<C> Value;\n",
		"template int Value<int>;\n",
		"template void F<int>();\n"
	):
		before: str = sexpression(original, source, "cpp")
		after = sexpression(extended, source, "cpp")
		count += 1
		if before != after or re.search(r"\((?:ERROR|MISSING)(?:\s|\))", after):
			failures += 1
			print("FAIL C++ existing instantiation changed:", source.strip(), "\nbefore:", before, "\nafter:", after)
	# corpus に無い形も，元の文法が正常に読める golden の全文で照合する
	paths: list[pathlib.Path] = (
		sorted(path for path in (source_dir / "test_data/golden/cpp").glob("*") if path.suffix in (".in", ".out"))
	)
	if not paths:
		raise RuntimeError("missing C++ golden inputs")
	for path in paths:
		source = path.read_text(encoding="utf-8")
		before = sexpression(original, source, "cpp")
		if re.search(r"\((?:ERROR|MISSING)(?:\s|\))", before):
			continue
		after = sexpression(extended, source, "cpp")
		count += 1
		if before != after:
			failures += 1
			print("FAIL C++ golden tree changed:", path, "\nbefore:", before, "\nafter:", after)
	# 受理範囲・役割・既存の木の照合結果の返戻
	return count, failures

def verify_typescript_inputs(source_dir: pathlib.Path, original: pathlib.Path, extended: pathlib.Path) -> tuple[int, int]:
	"""文脈依存の束縛名・文境界・メソッド名と，既存の正常な入力の木を照合する
	計算量：ファイル数 F・パスの最大長 D・入力の総長 L・解析量 P・S 式の総長 S に対して O(F D log(F+1)+L+P+S)
	引数：
		source_dir: golden の入力と期待を持つ作業ツリー
		original: 元の文法をリンクした dumpast
		extended: 拡張した文法をリンクした dumpast
	戻値：
		検査した入力数と不一致の件数
	送出例外：
		OSError: 入力又はパーサーを扱えない場合
		RuntimeError: パーサーが失敗，又は golden の入力が欠ける場合
		UnicodeDecodeError: golden を UTF-8 として読み込めない場合
	"""
	# 束縛位置だけに識別子を認め，真の待機の式は元の読取を保つ
	patterns: tuple[tuple[str, str], ...] = (
		("[...await]", "(array_pattern (rest_pattern (identifier)))"),
		("[await]", "(array_pattern (identifier))"),
		("{...await}", "(object_pattern (rest_pattern (identifier)))"),
		(
			"{alpha: await = 1}",
			"(object_pattern (pair_pattern key: (property_identifier) value: (assignment_pattern left: (identifier) right: (number))))"
		),
		("{alpha: await}", "(object_pattern (pair_pattern key: (property_identifier) value: (identifier)))")
	)
	count: int = 0
	failures: int = 0
	used: set[tuple[str, str]] = set()
	for grammar, directory in ("typescript", "typescript"), ("tsx", "typescriptreact"):
		for pattern, tree in patterns:
			source: str = f"var {pattern} = alpha;\n"
			expected: str = f"(program (variable_declaration (variable_declarator name: {tree} value: (identifier))))"
			after: str = sexpression(extended, source, grammar)
			count += 1
			if after != expected:
				failures += 1
				print("FAIL TypeScript binding name:", grammar, source.strip(), "\nexpected:", expected, "\nafter:", after)
		# メソッド名と仮引数の間の改行は，名前に依らずフィールドを作らない
		method: str = (
			"(program (class_declaration name: (type_identifier) body: (class_body " +
			"(method_definition name: (property_identifier) parameters: (formal_parameters) " +
			"body: (statement_block (return_statement (number)))))))"
		)
		for name in "abstract", "accessor", "alpha", "async", "get", "readonly", "set", "static":
			for gap in "", "\n":
				source = f"class Box{{{name}{gap}(){{return 1;}}}}\n"
				after = sexpression(extended, source, grammar)
				count += 1
				if after != method:
					failures += 1
					print("FAIL TypeScript method boundary:", grammar, source.strip(), "\nexpected:", method, "\nafter:", after)
		# 改行で終わるフィールド名と，後続のフィールドの独立した読み
		expected = (
			"(program (class_declaration name: (type_identifier) body: (class_body " +
			"(public_field_definition name: (property_identifier)) " +
			"(public_field_definition name: (property_identifier) value: (number)))))"
		)
		after = sexpression(extended, "class Box{accessor\nalpha=1}\n", grammar)
		count += 1
		if after != expected:
			failures += 1
			print("FAIL TypeScript field boundary:", grammar, "\nexpected:", expected, "\nafter:", after)
		# 修復で区切った名前の式文と，後続のクラスの宣言を別の構文へ読む
		expected = "(program (expression_statement (identifier)) (class_declaration name: (type_identifier) body: (class_body)))"
		after = sexpression(extended, "abstract;\nclass Box {}\n", grammar)
		count += 1
		if after != expected:
			failures += 1
			print("FAIL TypeScript declaration boundary:", grammar, "\nexpected:", expected, "\nafter:", after)
		# 明示した区切でメソッド名を失った入力は，拡張後も受理しない
		after = sexpression(extended, "class Box{alpha;\n(){}}\n", grammar)
		count += 1
		if not re.search(r"\((?:ERROR|MISSING)(?:\s|\))", after):
			failures += 1
			print("FAIL TypeScript missing method name accepted:", grammar, after)
		# 型・関数シグネチャ・計算した名前・真の待機の式の木を保持する
		for source in (
			"abstract class Box {}\n",
			"class Box{alpha\n[1](){return 1;}}\n",
			"class Box{async alpha(){return await alpha();}}\n",
			"class Box{alpha: number\nbeta(): void;}\n",
			"type Alpha = number\n(alpha);\n",
			"function alpha(): number\n(alpha);\n",
			"async function alpha(){return await (alpha());}\n"
		):
			before: str = sexpression(original, source, grammar)
			after = sexpression(extended, source, grammar)
			count += 1
			if before != after:
				failures += 1
				print("FAIL TypeScript existing tree changed:", grammar, source.strip(), "\nbefore:", before, "\nafter:", after)
		# 影響する２文法の golden の全文でも，元の正常な木の変更を検出する
		paths: list[pathlib.Path] = sorted((source_dir / "test_data/golden" / directory).glob("*.in"))
		if not paths:
			raise RuntimeError("missing TypeScript golden inputs: " + directory)
		for path in paths:
			for suffix in ".in", ".out":
				source = path.with_suffix(suffix).read_text(encoding="utf-8")
				before = sexpression(original, source, grammar)
				if re.search(r"\((?:ERROR|MISSING)(?:\s|\))", before):
					continue
				after = sexpression(extended, source, grammar)
				count += 1
				key: tuple[str, str] = grammar, path.with_suffix(suffix).relative_to(source_dir / "test_data/golden").as_posix()
				is_allowed: bool = is_allowed_golden_change(key, before, after)
				if is_allowed and before != after:
					used.add(key)
				if not is_allowed:
					failures += 1
					print("FAIL TypeScript golden tree changed:", path.with_suffix(suffix), "\nbefore:", before, "\nafter:", after)
	for key in ALLOWED_GOLDEN_PREFIXES.keys() - used:
		failures += 1
		print("FAIL unused TypeScript golden allowance:", key)
	# 再現と既存の木の照合結果の返戻
	return count, failures

def verify_shapes(source_dir: pathlib.Path, work_dir: pathlib.Path) -> int:
	"""各拡張が正しい文法を受け入れ，形の異なる文法を生成前に拒む事を確認する
	計算量：文法の総文字数 L と子プロセスの処理量 P に対して O(L+P)
	引数：
		source_dir: 元の文法と拡張のスクリプトを持つ作業ツリー
		work_dir: 専用の一時ディレクトリを作る場所
	戻値：
		照合した不一致の件数
	送出例外：
		OSError: 元の文法又は子プロセスを扱えない場合
		RuntimeError: 正常な文法を拒否，又は形の異なる文法を受理した場合
	"""
	# 文法の型・名前・位置の不一致の検査
	mutations: dict[str, tuple[tuple[str|int, ...], ...]] = {
		"cpp": (
			("class_specifier", "members", 0, "value"),
			("struct_specifier", "members", 1, "name"),
			("union_specifier", "type"),
			("template_instantiation", "value"),
			("template_instantiation", "content", "members", 0, "members", 0, "value"),
			("template_instantiation", "content", "members", 2, "members", 0, "name"),
			("template_instantiation", "content", "members", 3, "name"),
			("template_instantiation", "content", "members", 3, "content", "name"),
			("template_instantiation", "content", "members", 4, "value")
		),
		"swift": (("type_arguments", "type"), ("type_arguments", "content", "members", 0, "value")),
		"typescript": (
			("_reserved_identifier", "type"),
			("pattern", "content", "members", 0, "name"),
			("rest_pattern", "content", "members", 0, "value"),
			("rest_pattern", "content", "members", 1, "name"),
			("class_body", "members", 1, "content", "members", 3, "members", 0, "members", 3, "name")
		),
		"tsx": (
			("pattern", "value"),
			("rest_pattern", "value"),
			("rest_pattern", "content", "members", 1, "type"),
			("class_body", "members", 1, "content", "members", 3, "members", 1, "members", 0, "name")
		),
		"html": (("text", "type"),),
		"scss": (
			("keyframe_block", "members", 0, "type"),
			("keyframe_block", "members", 0, "members", 0, "name"),
			("attribute_selector", "members", 2, "content", "members", 0, "name"),
			("declaration", "members", 4, "type"),
			("last_declaration", "content", "members", 4, "members", 0, "name"),
			("child_selector", "content", "members", 0, "name"),
			("parameters", "members", 1, "members", 0, "name")
		)
	}
	count: int = 0
	with tempfile.TemporaryDirectory(prefix="grammar-shapes-", dir=work_dir) as temporary:
		base: pathlib.Path = pathlib.Path(temporary)
		for name, (relative, prepare) in GRAMMARS.items():
			original: pathlib.Path = source_dir / "vendor" / ("tree-sitter-" + relative) / "grammar.json"
			script: pathlib.Path = source_dir / "tools" / prepare
			for idx, route in enumerate(((),) + mutations[name]):
				grammar: dict = json.loads(original.read_text(encoding="utf-8"))
				if route:
					node: dict|list = grammar["rules"]
					for part in route[:-1]:
						node = node[part]
					node[route[-1]] = "unexpected_shape"
				src: pathlib.Path = base / f"{name}-{idx}.json"
				output: pathlib.Path = base / f"{name}-{idx}"
				src.write_text(json.dumps(grammar), encoding="utf-8")
				result: subprocess.CompletedProcess[str] = (
					subprocess.run(["node", str(script), str(src), str(output)], capture_output=True, text=True, encoding="utf-8", check=False)
				)
				if not route:
					if result.returncode or not (output / "grammar.json").is_file():
						raise RuntimeError(f"valid grammar rejected: {name}: {result.stderr}")
				elif not result.returncode or "Grammar shape mismatch" not in result.stderr or output.exists():
					raise RuntimeError(f"changed grammar shape accepted or written: {name}: {route}: {result.stderr}")
				else:
					count += 1
	# 正常な生成と，不一致の拒否を確認した件数の返戻
	return count

def self_check() -> list[str]:
	"""corpus の取零しと，許可表の無関係な変更への拡大を正誤対で検出する
	計算量：検査用の入力の総長 L に対して O(L)
	戻値：
		独立した期待と不一致に為った検査の説明
	"""
	# corpus の欠落と許可表の範囲の検査
	found: list[str] = []
	text: str = "===\nFirst\n===\na\n---\n(root)\n===\nSecond\n===\nb\n---\n(root)\n"
	if list(corpus_cases(text)) != [("First", "a\n"), ("Second", "b\n")]:
		found.append("corpus の連続するテストケースを取り落とした")
	for broken in "", "===\nFirst\n", "===\nFirst\n===\na\n", "===\nFirst\n===\na\n---\n":
		try:
			list(corpus_cases(broken))
			found.append("不完全な corpus を受理した")
		except ValueError:
			pass
	key: tuple[str, str, str] = "swift", "minimal.txt", "Intentional change"
	with patch.dict(ALLOWED_CHANGES, { key: ("(old)", "(new)") }, clear=True):
		for target, before, after, is_expected in (
			(key, "(same)", "(same)", True),
			(key, "(old)", "(new)", True),
			(key, "(other)", "(new)", False),
			(key, "(old)", "(other)", False),
			(("html", "minimal.txt", "Intentional change"), "(old)", "(new)", False)
		):
			if is_allowed_change(target, before, after) != is_expected:
				found.append("許可表の前後の木又は対象の一致を誤った")
	golden_key: tuple[str, str] = "typescript", "typescript/comment_declaration_member_anchors.in"
	old, new = ALLOWED_GOLDEN_PREFIXES[golden_key]
	for target, before, after, is_expected in (
		(golden_key, "(same)", "(same)", True),
		(golden_key, old + "(tail))", new + "(tail))", True),
		(golden_key, old + "(tail))", new + "(other))", False),
		(golden_key, "(other) (tail))", new + "(tail))", False),
		(golden_key, old + "(tail))", "(other) (tail))", False),
		(("tsx", golden_key[1]), old + "(tail))", new + "(tail))", False),
		((golden_key[0], "typescript/type_forms.in"), old + "(tail))", new + "(tail))", False)
	):
		if is_allowed_golden_change(target, before, after) != is_expected:
			found.append("golden の訂正の対象・先頭・後続の一致を誤った")
	# corpus と許可表の正誤対の結果の返戻
	return found

def main() -> int:
	"""拡張前後の全文法の corpus を比較し，不一致又は検査資産の欠落を失敗とする
	計算量：corpus の総長 L・解析量 P・S 式の総長 S に対して O(L+P+S)
	戻値：
		検査が成功すれば０，不一致が有れば１
	送出例外：
		OSError: 検査の入力又はパーサーを扱えない場合
		ValueError: corpus の構造が不正な場合
		RuntimeError: パーサー又は文法の形の検査が失敗した場合
	"""
	# 比較と形の検査の実行設定
	for problem in self_check():
		print("FAIL grammar extension checker:", problem)
		# チェッカーの正誤対の失敗の返戻
		return 1
	parser: argparse.ArgumentParser = argparse.ArgumentParser()
	parser.add_argument("--original", type=pathlib.Path)
	parser.add_argument("--extended", type=pathlib.Path)
	parser.add_argument("--shapes-only", action="store_true")
	parser.add_argument("--source-dir", type=pathlib.Path, required=True)
	parser.add_argument("--work-dir", type=pathlib.Path, required=True)
	args: argparse.Namespace = parser.parse_args()
	prepared: set[str] = { path.stem.removeprefix("prepare").lower() for path in (args.source_dir / "tools").glob("prepare*.mjs") }
	if prepared != { pathlib.Path(prepare).stem.removeprefix("prepare").lower() for _, prepare in GRAMMARS.values() }:
		raise RuntimeError("grammar extension coverage mismatch: " + ", ".join(sorted(prepared)))
	if args.shapes_only:
		shapes: int = verify_shapes(args.source_dir.resolve(), args.work_dir.resolve())
		print(f"grammar shapes: {shapes} rejected changes, 0 failed")
		# ホスト側での文法の形の検査の成功の返戻
		return 0
	if args.original is None or args.extended is None:
		parser.error("--original and --extended are required for corpus comparison")
	if args.original.resolve() == args.extended.resolve():
		raise ValueError("original and extended parsers must be separate executables")
	# vendor の corpus の前後比較
	count: int = 0
	failures: int = 0
	used: set[tuple[str, str, str]] = set()
	for grammar, (relative, _) in GRAMMARS.items():
		corpus: pathlib.Path = args.source_dir / "vendor" / ("tree-sitter-" + relative.split("/")[0]) / "test/corpus"
		files: list[pathlib.Path] = sorted(path for path in corpus.rglob("*") if path.is_file())
		if not files:
			raise RuntimeError("missing corpus: " + str(corpus))
		for path in files:
			for title, source in corpus_cases(path.read_text(encoding="utf-8")):
				key: tuple[str, str, str] = grammar, path.relative_to(corpus).as_posix(), title
				before: str = sexpression(args.original, source, grammar)
				after: str = sexpression(args.extended, source, grammar)
				count += 1
				if before != after and key in ALLOWED_CHANGES:
					used.add(key)
				if not is_allowed_change(key, before, after):
					failures += 1
					print("FAIL grammar extension:", key, "\nbefore:", before, "\nafter:", after)
	for key in ALLOWED_CHANGES.keys() - used:
		failures += 1
		print("FAIL unused grammar extension allowance:", key)
	cpp_count: int
	cpp_failures: int
	cpp_count, cpp_failures = verify_cpp_inputs(args.source_dir, args.original, args.extended)
	count += cpp_count
	failures += cpp_failures
	typescript_count: int
	typescript_failures: int
	typescript_count, typescript_failures = verify_typescript_inputs(args.source_dir, args.original, args.extended)
	count += typescript_count
	failures += typescript_failures
	print(f"grammar extensions: {count} inputs, {failures} failed")
	# 全比較の終了コードの返戻
	return 1 if failures else 0

if __name__ == "__main__":
	sys.exit(main())
