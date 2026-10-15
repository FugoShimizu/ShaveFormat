#! /usr/bin/env python3
"""設計の不変条件の検査（整形仕様 3 の設計要件）

整形仕様 3 は「必要としないクラスへは型と依存関係の段階で渡さない」を設計の基本原則とする
此処では其の内，依存関係で機械的に確かめれる条件を取出の上で検査する

検査する条件：
1. 行幅 (`MaxChars`) を読むのは `LineBreaker` だけ（`Canonicalizer` を含む他の工程は行幅を受け取らない）
2. 中核のソース工程 (`SyntaxNormalizer` / `Canonicalizer` / `LineBreaker` / `CodeLayout`) がコメントの**本文**を読まない
3. `DocCommentSynchronizer` と `CommentNormalizer` と `CommentPlacer` は構文木を書き換えない（整形仕様 3 の権限の表）
4. コメントの配置計画は配置枠とコメント行の列だけを持ち，空行・コードの変更を表せない（整形仕様 3.2）
5. 変更不能なコードの出力は `CodeLayout` だけが，合成結果は `OutputDocument::Compose` だけが構築し，別の構築経路が無い
6. 空行を決める `CodeLayout` はコメントの記録を受け取らない
7. コメントの本文構造は `CommentNormalizer::Inspect` だけが読取専用の見方から判定し，変更不能な結果を返す（整形仕様 3.3）
見方は記法とドキュメント形式及び其の定義，本文，記録の順序と所属だけを持つ
同期・配置・検証・静的解析は其の結果を共有し，投影は間隙の共通判定 (`IsCommentGap`) と記法・アンカー・側の照合で所属を記録する
8. 所有領域は確定した子のコードだけを親の中核へ渡し，子のコメントと検証データは別々の型付経路で渡す
合成は変更不能なコードと配置計画の２入力だけ，シリアライズは其の断片と終端契約だけを扱う

此の検査は宣言・別名・明示した参照を照合する静的解析であり，C++ の完全な型解決・値の流れ・寿命・実行時の意味保存は証明しない
テキストの解析失敗は確認不能として報告し，検査結果と設計準拠の独立した監査を区別する
Compose の明示した自由関数・静的関数の呼出先は定義へ辿り，定義無・曖昧な呼出・未対応の見出を確認不能とする
入力のメンバ操作・標準ライブラリ・既知の型の構築呼出の内部は，此の狭い補助関数の追跡では証明しない

違反は零に成った為，`golden_test.sh` へ組み込んで退行の検出器として使う
条件を満たせなく為る変更を入れる時は，実装でなく整形仕様 3 の設計要件を先に見直す

使方：
python3 tools/verify_design_invariants.py
"""

from __future__ import annotations

from cpp_lex import CppSource, CppToken, cpp_code, cpp_lex_self_check, cpp_source, cpp_source_code, cpp_tokens
from bisect import bisect_left, bisect_right
from pathlib import Path

import re
import sys

from tempfile import TemporaryDirectory
from typing import Callable, TypeAlias

MethodContract: TypeAlias = tuple[str, tuple[tuple[str, ...], ...], tuple[str, ...], tuple[str, ...]] # 名前・仮引数型・後置修飾・返戻型
# 検査の経路と実行設定
ROOT: Path = Path(__file__).resolve().parent.parent
SRC: Path = ROOT / "src"
# 行幅を読んで良い実装ファイル
WIDTH_OWNER: str = "LineBreaker"

# コメントの経路に属し，本文を読んで良い実装ファイル
COMMENT_PATH: tuple[str, ...] = (
	"CommentNormalizer",
	"CommentPlacer",
	"CommentRepository",
	"DocCommentReader",
	"DocCommentSynchronizer",
	"SourceProjector"
)

# 中核のソース工程（コメントの本文を読んでは為らない）
CORE_PASSES: tuple[str, ...] = "Canonicalizer", "CodeLayout", "LineBreaker", "SyntaxNormalizer"
# 構文木を書き換えては為らない工程
READ_ONLY_PATH: tuple[str, ...] = "CommentNormalizer", "CommentPlacer", "DocCommentReader", "DocCommentSynchronizer"
# コメントの本文を読むテキスト
TEXT_READS: re.Pattern[str] = re.compile(r"(?:\.|->)\s*(?:Text|GetText)\b")
# 関数の定義の見出（`型 クラス::関数(` の形，名前空間の中のインデントを含む）
DEF: re.Pattern[str] = re.compile(r"^\s*[A-Za-z_][\w:<>,&*\s]*?\b([A-Z]\w+)\s*::\s*[A-Za-z_]\w*\s*\(")
# 名前空間を開く行
NAMESPACE: re.Pattern[str] = re.compile(r"^\s*namespace\b")

def sources() -> list[Path]:
	"""実装ファイルの経路を探して整列した一覧を返す
	計算量：走査した経路数 P・対象ファイル数 F に対して O(P+F log F)
	戻値：
		整列した実装ファイルの経路の一覧
	"""
	# 実装ファイルの一覧の返戻
	return sorted(list(SRC.rglob("*.cpp")) + list(SRC.rglob("*.hpp")))

def source_owners(source: CppSource, text: str, code: str) -> list[str]:
	"""論理テキストと位置表を共有し，原文の各行の所有者を返す
	計算量：原文の文字数 L に対して最悪 O(L²)
	引数：
		source: 原文から１度構築した位置表と論理テキスト
		text: 読取対象の原文
		code: 同じ論理テキストの非コード領域を伏せた物
	戻値：
		原文の行毎の所有者
	"""
	# 関数定義と波括弧の深さに依る所有クラスの追跡
	current: str = ""
	found: list[str] = []
	logical_pos: int = 0
	physical_pos: int = 0
	# 名前空間以外の波括弧の深さ（関数・クラスの本体の中の呼出を定義と見誤らない）と，開いた波括弧毎の名前空間か
	depth: int = 0
	scopes: list[bool] = []
	match: re.Match[str]|None
	for line in code.split("\n"):
		if not depth and (match := DEF.match(line)):
			current = match.group(1)
		logical_pos += len(line) + 1
		end: int = source.offsets[min(logical_pos, len(source.text))]
		count: int = text.count("\n", physical_pos, end) + (logical_pos > len(source.text))
		found.extend([current] * count)
		physical_pos = end
		is_namespace: bool = bool(NAMESPACE.match(line))
		for char in line:
			if char == "{":
				scopes.append(is_namespace)
				depth += not scopes[-1]
			elif char == "}" and scopes:
				depth -= not scopes.pop()
	# 行毎の所有クラスの一覧の返戻
	return found

def owners(text: str) -> list[str]:
	"""ソースの各行を所有するクラスを定義から判定して返す
	計算量：ソースの文字数 L に対して最悪 O(L²)
	引数：
		text: クラス・構造体の定義を含む C++ ソース
	戻値：
		ソースの各行に対応する所有型名の列
	"""
	source: CppSource = cpp_source(text)
	# 同じ読取結果から所有者を判定した列の返戻
	return source_owners(source, text, cpp_source_code(source))

def width_owner_violations() -> list[str]:
	"""行幅を LineBreaker の外で読む箇所を列挙する
	計算量：走査した経路数 P・対象ファイル数 F・総文字数 L に対して O(P+F log F+L)
	戻値：
		設計の不変条件への違反の一覧
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		UnicodeDecodeError: 読み込んだファイルを指定の文字コードで解釈出来ない場合
	"""
	# 行幅の所有者の違反の収集
	found: list[str] = []
	for path in sources():
		if path.stem.startswith(WIDTH_OWNER):
			continue
		for num, line in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
			# 行幅の受渡と構築だけは呼出側に許す（`Main` が `LineBreaker` を組み立てる）
			if "MaxChars" not in line or path.stem == "Main":
				continue
			found.append(f"{path.relative_to(ROOT)}:{num}: 行幅を {WIDTH_OWNER} の外で読んで居る")
	# 違反の一覧の返戻
	return found

def comment_text_violations() -> list[str]:
	"""中核のソース工程がコメントの本文を読む箇所を列挙する
	計算量：走査した経路数 P・対象ファイル数 F・総文字数 L に対して最悪 O(P+F log F+L²)
	戻値：
		設計の不変条件への違反の一覧
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		UnicodeDecodeError: 読み込んだファイルを指定の文字コードで解釈出来ない場合
	"""
	# 中核でのコメント本文の読取の違反の収集
	found: list[str] = []
	for path in sources():
		text: str = path.read_text(encoding="utf-8")
		# コメントの記録を触らないファイルは読様が無い
		if "CommentRecord" not in text:
			continue
		source: CppSource = cpp_source(text)
		code: str = cpp_source_code(source)
		line_starts: list[int] = [0, *(match.end() for match in re.finditer("\n", text))]
		ownership: list[str] = source_owners(source, text, code)
		seen: set[int] = set()
		for match in TEXT_READS.finditer(code):
			num: int = bisect_right(line_starts, source.offsets[match.start()]) - 1
			owner: str = ownership[num]
			if owner not in CORE_PASSES or owner in COMMENT_PATH or num in seen:
				continue
			seen.add(num)
			found.append(f"{path.relative_to(ROOT)}:{num + 1}: 中核の工程がコメントの本文を読んで居る")
	# 違反の一覧の返戻
	return found

def comment_path_mutation_violations() -> list[str]:
	"""コメントの経路が構文木を書き換えれる箇所を列挙する
	計算量：走査した経路数 P・対象ファイル数 F・総文字数 L に対して最悪 O(P+F log F+L²)
	戻値：
		設計の不変条件への違反の一覧
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		UnicodeDecodeError: 読み込んだファイルを指定の文字コードで解釈出来ない場合
	"""
	# 構文木を書き換えれる形（`const` の付かない参照）
	mutable: re.Pattern[str] = re.compile(r"(?<!const )SyntaxModel\s*&")
	found: list[str] = []
	for path in sources():
		text: str = path.read_text(encoding="utf-8")
		for num, (line, owner) in enumerate(zip(text.split("\n"), owners(text)), 1):
			# 宣言だけのヘッダはクラスの本体で属を決めれない為，ファイル名で補う
			if (
				(owner or path.stem) not in READ_ONLY_PATH or
				(not mutable.search(line) or line.lstrip().startswith("//") or line.lstrip().startswith("*"))
			):
				continue
			found.append(f"{path.relative_to(ROOT)}:{num}: コメントの経路が構文木を書き換えられる")
	# 違反の一覧の返戻
	return found

def closing_delimiter(code: str, start: int, opening: str, closing: str) -> int:
	"""対応する閉記号の位置を得る
	計算量：テキストの文字数 L に対して O(L)
	引数：
		code: コメントとリテラルを除いたテキスト
		start: 開きの位置
		opening: 開きの記号
		closing: 閉じの記号
	戻値：
		対応する閉じの位置
	送出例外：
		ValueError: 開き又は対応する閉じを確認出来ない場合
	"""
	# 指定した開きと閉じの対応の検査
	if start >= len(code) or code[start] != opening:
		raise ValueError("opening delimiter is missing")
	depth: int = 0
	for pos in range(start, len(code)):
		depth += code[pos] == opening
		depth -= code[pos] == closing
		if not depth:
			# 対応する閉じの返戻
			return pos
	raise ValueError("closing delimiter is missing")

def class_body(text: str, name: str) -> str:
	"""ヘッダで指定名のクラス・構造体の本体を返す
	見付からなければ空文字列を返す
	計算量：ヘッダの文字数 L に対して最悪 O(L²)
	引数：
		text: クラス・構造体を含むヘッダの全文
		name: 取得する型の名前
	戻値：
		指定した型の本体，見付からなければ空文字列
	送出例外：
		ValueError: 型の本体の波括弧が閉じない場合
	"""
	# 行連結後の本文とコードで位置を共有する
	text = cpp_source(text).text
	code: str = cpp_code(text)
	match: re.Match[str]|None = re.search(rf"^[ \t]*(?:class|struct)\s+{re.escape(name)}\b[^{{;]*\{{", code, re.M)
	if not match:
		# 定義が無い場合の空文字列の返戻
		return ""
	# ネストした型の閉じで打ち切らない本体のテキストの返戻
	return text[match.end():closing_delimiter(code, match.end() - 1, "{", "}")]

def cpp_decls(body: str) -> list[str]:
	"""型の直下の宣言を，入子の型と関数本体を除いて得る
	計算量：本文の文字数 L に対して O(L)，インデックスの再取得は O(1)
	引数：
		body: コメントとリテラルを除いた型の本文
	戻値：
		メンバ宣言と関数の見出の列
	送出例外：
		ValueError: 波括弧の対応を確認出来ない場合
	"""
	if isinstance(body, CppBody):
		# 共有する宣言の返戻
		return body.declarations
	# 宣言の領域の探索
	found: list[str] = []
	start: int = 0
	pos: int = 0
	while pos < len(body):
		if body[pos] == "{":
			end: int = closing_delimiter(body, pos, "{", "}")
			head: str = body[start:pos].strip()
			if re.search(r"\b(?:struct|class|enum)\b", head) or ")" in head:
				if ")" in head:
					found.append(head + ";")
				pos = end + 1
				start = pos
				continue
			pos = end
		elif body[pos] == ";":
			decl: str
			if (
				(decl := re.sub(r"\b(?:public|private|protected)\s*:", "", body[start:pos]).strip()) and
				not decl.startswith(("friend ", "typedef ", "using "))
			):
				found.append(decl)
			start = pos + 1
		pos += 1
	# メンバ宣言の返戻
	return found

def cpp_fields(body: str) -> list[tuple[str, str]]:
	"""メンバの名前と型のテキストを取り出す
	計算量：本文の文字数 L に対して O(L)，インデックスの再取得は O(1)
	引数：
		body: コメントとリテラルを除いた型の本文
	戻値：
		単純なデータメンバの名前と型の列（関数は含めない）
	送出例外：
		ValueError: 波括弧の対応を確認出来ない場合
	"""
	if isinstance(body, CppBody) and hasattr(body, "fields"):
		# 共有するフィールドの返戻
		return body.fields
	# メンバの型と名前の収集
	found: list[tuple[str, str]] = []
	for decl in cpp_decls(body):
		head: str = decl.split("=", 1)[0].split("{", 1)[0].strip()
		if "(" in head or re.search(r"\boperator\b", head):
			continue
		match: re.Match[str]|None
		if match := re.fullmatch(r"(.+?[\s*&])([A-Za-z_]\w*)(?:\s*\[[^]]*\])?", head, re.S):
			found.append((match.group(2), match.group(1).strip()))
	# 名前と型の返戻
	return found

def type_names(code: str) -> set[str]:
	"""型のテキストに在る識別子を得る（名前空間の別名も保守的に照合する）
	計算量：テキストの文字数 L に対して O(L)
	引数：
		code: 型又はメンバ宣言のテキスト
	戻値：
		識別子の集合
	"""
	# 型の識別子の返戻
	return set(re.findall(r"\b[A-Za-z_]\w*\b", code))

def cpp_args(code: str) -> list[str]:
	"""入子の外のカンマだけで引数又は仮引数を分ける
	計算量：テキストの文字数 L に対して O(L)
	引数：
		code: コメントとリテラルを除いた括弧の内容
	戻値：
		空でない各項目のテキスト
	送出例外：
		ValueError: 囲みの対応を確認出来ない場合
	"""
	# 実引数の区切の探索
	items: list[str] = []
	stack: list[str] = []
	pairs: dict[str, str] = { "(": ")", "<": ">", "[": "]", "{": "}" }
	start: int = 0
	for pos, char in enumerate(code):
		if char in pairs:
			stack.append(pairs[char])
		elif stack and char == stack[-1]:
			stack.pop()
		elif char == "," and not stack:
			items.append(code[start:pos].strip())
			start = pos + 1
	if stack:
		raise ValueError("argument nesting is not understood")
	tail: str
	if tail := code[start:].strip():
		items.append(tail)
	# 分割した項目の返戻
	return items

def decl_types(decl: str) -> set[str]:
	"""メンバ宣言から，メンバ名・仮引数名を除いた型の識別子を得る
	計算量：宣言の文字数 L に対して最悪 O(L²)
	引数：
		decl: コメントとリテラルを除いたメンバ宣言
	戻値：
		メンバ型，又は関数の戻値と仮引数型の識別子
	送出例外：
		ValueError: 関数の仮引数の囲みを確認出来ない場合
	"""
	# 宣言が参照する型の抽出
	opening: int = decl.find("(")
	if opening == -1:
		members: list[tuple[str, str]] = cpp_fields(decl + ";")
		# データメンバの型の返戻
		return set().union(*(type_names(val) for _, val in members))
	end: int = closing_delimiter(decl, opening, "(", ")")
	head: str = re.sub(r"\b(?:operator\b.*|[A-Za-z_]\w*)$", "", decl[:opening]).strip()
	found: set[str] = type_names(head)
	for arg in cpp_args(decl[opening + 1:end]):
		members = cpp_fields(arg + ";")
		found.update(type_names(members[0][1] if members else arg))
	if "->" in decl[end + 1:]:
		found.update(type_names(decl[end + 1:].split("->", 1)[1]))
	# 関数の型の返戻
	return found

def cpp_delimiters(code: str) -> dict[int, int]:
	"""コメントとリテラルを除いたコードの，同種の括弧の対応を１度の走査で求める
	計算量：文字数 L に対して O(L)
	引数：
		code: コメントとリテラルを除いたテキスト
	戻値：
		対応した開きの位置から閉じの位置への対応．未対応の開きは含まない
	"""
	pending: dict[str, list[int]] = { "(": [], "[": [], "{": [] }
	closers: dict[str, str] = { ")": "(", "]": "[", "}": "{" }
	pairs: dict[int, int] = {}
	for pos, char in enumerate(code):
		if char in pending:
			pending[char].append(pos)
		elif char in closers and pending[closers[char]]:
			pairs[pending[closers[char]].pop()] = pos
	# 囲みの対応の返戻
	return pairs

def cpp_type_idx(codes: dict[Path, str]) -> tuple[dict[str, list[tuple[Path, str]]], dict[str, str], dict[str, set[str]]]:
	"""補助型の名前を固定せず，宣言と別名から型の到達関係を控える
	同名の型・別名は統合して保守的に調べ，名前空間の同名異型を同一と証明しない
	計算量：総文字数 L・型数 V に対して O(L+V)
	引数：
		codes: 対象ファイルとコメント・リテラルを除いたテキスト
	戻値：
		型の定義位置と本文，別名の定義，型の到達関係
	送出例外：
		ValueError: 型の本体の終わりを確認出来ない場合
	"""
	# 型の本体と別名のインデックスの構築
	bodies: dict[str, list[tuple[Path, str]]] = {}
	aliases: dict[str, str] = {}
	bases: dict[str, set[str]] = {}
	for path, code in codes.items():
		pairs: dict[int, int] = cpp_delimiters(code)
		# 範囲は原文順に重ならない部分だけを各所有型へ渡す
		name: str
		owner: str
		limit: int
		cursor: int
		ranges: list[tuple[int, int]]
		stack: list[tuple[str, int, int, list[tuple[int, int]]]] = []
		for match in re.finditer(r"\b(?:struct|class)\s+(\w+)(?:\s+final)?(?:\s*:\s*[^;{}]+)?\s*\{", code):
			if re.search(r"\benum\s*$", code[max(0, match.start() - 16):match.start()]):
				continue
			while stack and stack[-1][1] < match.start():
				name, end, cursor, ranges = stack.pop()
				ranges.append((cursor, end))
				bodies.setdefault(name, []).append((path, CppBody(code, ranges)))
			end: int = pairs.get(match.end() - 1, -1)
			if end < 0:
				raise ValueError("型の本体の閉じが無い")
			if stack:
				owner, limit, cursor, ranges = stack[-1]
				ranges.append((cursor, match.end()))
				# 子の宣言の見出と閉じだけを親へ残す
				stack[-1] = owner, limit, end, ranges
			bases.setdefault(match.group(1), set()).update(type_names(match.group().partition(":")[2]))
			stack.append((match.group(1), end, match.end(), []))
		while stack:
			name, end, cursor, ranges = stack.pop()
			ranges.append((cursor, end))
			bodies.setdefault(name, []).append((path, CppBody(code, ranges)))
		for match in re.finditer(r"\busing\s+(\w+)\s*=\s*([^;]+);", code):
			aliases[match.group(1)] = aliases.get(match.group(1), "") + " " + match.group(2)
		for match in re.finditer(r"\btypedef\s+([^;{}]+?)\s+(\w+)\s*;", code):
			aliases[match.group(2)] = aliases.get(match.group(2), "") + " " + match.group(1)
	graph: dict[str, set[str]] = { name: type_names(val) for name, val in aliases.items() }
	for name, entries in bodies.items():
		edges: set[str] = graph.setdefault(name, set())
		edges.update(bases.get(name, set()))
		for _, body in entries:
			for decl in cpp_decls(body):
				edges.update(decl_types(decl))
	# 型のインデックスと到達関係の返戻
	return bodies, aliases, graph

def expanded_type(aliases: dict[str, str], code: str) -> str:
	"""型の別名を展開し，循環する別名は未展開のテキストとして残す
	計算量：参照した別名の定義と入力の総文字数 L に対して O(L)
	引数：
		aliases: 別名の定義
		code: 型のテキスト
	戻値：
		参照した別名を各々１回展開した型のテキスト
	"""
	# 同じ定義を展開して複写せず，参照の DAG を明示した作業列で辿る
	seen: set[str] = set()
	parts: list[str] = []
	text: str
	is_literal: bool
	pending: list[tuple[str, bool]] = [(code, False)]
	while pending:
		text, is_literal = pending.pop()
		if is_literal:
			parts.append(text)
			continue
		fragments: list[tuple[str, bool]] = []
		start: int = 0
		for match in re.finditer(r"\b[A-Za-z_]\w*\b", text):
			fragments.append((text[start:match.start()], True))
			name: str = match.group()
			if name in aliases and name not in seen:
				seen.add(name)
				fragments.append((aliases[name], False))
			else:
				# 循環と重複する参照は型名として残し，証明出来ない契約を許可しない
				fragments.append((name, True))
			start = match.end()
		fragments.append((text[start:], True))
		pending.extend(reversed(fragments))
	# 所属毎に展開した別名の返戻
	return "".join(parts)

def contract_tokens(text: str) -> tuple[str, ...]:
	"""契約の宣言と呼出を，コメント・空白・行の配置と区別して読む
	計算量：原文の文字数 L に対して O(L)
	引数：
		text: 契約の宣言又は呼出
	戻値：
		コメントと余白を除いたトークンの列
	"""
	# コメントと余白を除いた契約のトークンの返戻
	return tuple(token.text for token in cpp_tokens(text) if token.kind not in ("comment", "space"))

def contains_contract(text: str, expected: str) -> bool:
	"""原文の区切に依らず，構文上の連続するトークンを照合する
	計算量：原文の文字数 L・期待のトークン数 K に対して O(LK)
	引数：
		text: 照合するコード
		expected: 期待する宣言又は呼出の形
	戻値：
		構文上の連続するトークンが一致すれば真
	"""
	tokens: tuple[str, ...] = contract_tokens(text)
	wanted: tuple[str, ...] = contract_tokens(expected)
	# 構文上の連続する契約の一致の返戻
	return any(tokens[idx:idx + len(wanted)] == wanted for idx in range(len(tokens) - len(wanted) + 1))

def contract_methods(text: str) -> list[MethodContract]:
	"""直属の関数の型・引数・修飾を，仮引数名や空白の綴りから分ける
	計算量：直接の宣言の文字数 L に対して O(L)
	引数：
		text: コメントとリテラルを除いた直接の宣言
	戻値：
		名前・仮引数の型の列・後置修飾・返戻型の列
	送出例外：
		ValueError: 仮引数又は本体の囲みの対応を確認出来ない場合
	"""
	found: list[MethodContract] = []
	for decl in cpp_decls(text):
		opening: int = decl.find("(")
		if opening < 0:
			continue
		end: int = closing_delimiter(decl, opening, "(", ")")
		name: re.Match[str]|None = re.search(r"(operator\s*=|[A-Za-z_]\w*)\s*$", decl[:opening])
		if name is None:
			continue
		args: list[tuple[str, ...]] = []
		for arg in cpp_args(decl[opening + 1:end]):
			part: str = arg.partition("=")[0].strip()
			members: list[tuple[str, str]] = cpp_fields(part + ";")
			args.append(contract_tokens(members[0][1] if members else part))
		found.append(
			(re.sub(r"\s+", "", name.group()), tuple(args), contract_tokens(decl[end + 1:]), contract_tokens(decl[:name.start()]))
		)
	# 直接の関数の契約の一覧の返戻
	return found

def owner_self_check() -> list[str]:
	"""循環する別名と型・呼出の契約の正誤対を照合する
	計算量：テストケースの総文字数 L と追跡費用 P に対して O(L+P)
	戻値：
		手書の契約の期待と違うケースの一覧
	"""
	found: list[str] = []
	if expanded_type({ "A": "B", "B": "A" }, "A") != "A":
		found.append("循環する別名を展開し続けた")
	# 偶然の空白・仮引数名は契約の差とせず，`const` の除去を区別する
	for text, expected in (
		("static Result Inspect ( const View & Renamed );", ("const", "View", "&")),
		("static Result Inspect ( View & Renamed );", ("View", "&"))
	):
		methods: list[MethodContract] = contract_methods(text)
		if len(methods) != 1 or methods[0][1] != (expected,):
			found.append("関数の型の契約を仮引数名又は空白で誤読した")
	if (
		contains_contract("/* Shared :: Read ( */", "Shared::Read(") or
		not contains_contract("Shared /* gap */ :: Read (", "Shared::Read(")
	):
		found.append("コメントの本文と実際の共有する呼出を混同した")
	# 所有者の不一致の返戻
	return found

def fields(body: str) -> list[str]:
	"""クラス・構造体の本体が宣言するフィールドの名前を返す
	関数・型の宣言とコメントは除く
	計算量：本体の文字数 L に対して最悪 O(L²)
	引数：
		body: クラス・構造体の波括弧の内側のソース
	戻値：
		宣言されたフィールド名の列（関数・型の宣言とコメントを除く）
	"""
	# 行の畳方でなく，直属の宣言から名前を読む
	return [name for name, _ in cpp_fields(cpp_code(body))]

def placement_plan_violations() -> list[str]:
	"""配置計画が配置枠とコメント行の列の外のフィールドを持つ箇所を列挙する
	計算量：対象ヘッダの文字数 L に対して最悪 O(L²)
	戻値：
		設計の不変条件への違反の一覧
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		UnicodeDecodeError: 読み込んだファイルを指定の文字コードで解釈出来ない場合
	"""
	# 配置計画の許可フィールドと実際のフィールドの照合（Slot は記法を検査済の配置枠，NeedsLineEnd・HasLineHeadNotation は確認済の記法の終端条件だけ）
	path: Path = SRC / "Util/LayoutTypes.hpp"
	text: str = path.read_text(encoding="utf-8")
	allowed: dict[str, list[str]] = {
		"CommentBlock": ["Lines", "Slot", "NeedsLineEnd", "HasLineHeadNotation"],
		"CommentInsertion": ["Slot", "Comment"],
		"CommentPlacementPlan": ["Owner", "Insertions"]
	}
	found: list[str] = []
	for name, expected in allowed.items():
		actual: list[str]
		if (actual := fields(class_body(text, name))) != expected:
			found.append(f"{path.relative_to(ROOT)}: {name} のフィールドが {actual} で，配置枠とコメント行の列だけでない")
	# 違反の一覧の返戻
	return found

def cpp_helper_entries(code: str, scope: str = "") -> list[tuple[str, list[str], str|None, str]]:
	"""型・名前空間の直下の単純な関数宣言と定義を原文順に控える
	関数本体へは入らず，コンテナの範囲を共有する作業列で辿る
	計算量：文字数 L・返す修飾名と本文の総文字数 B に対して O(L+B)
	引数：
		code: コメント・リテラルを除いたテキスト
		scope: 所属の修飾名
	戻値：
		関数の修飾名・仮引数・本体（宣言だけの場合は None）・戻値の型の列
	送出例外：
		ValueError: 囲みの対応を確認出来ない場合
	"""
	code = re.sub(r"^[ \t]*#[^\n]*", "", code, flags=re.M)
	pairs: dict[int, int] = cpp_delimiters(code)
	found: list[tuple[str, list[str], str|None, str]] = []
	scope_parts: list[str] = [scope]
	pending: list[tuple[int, int, int]] = [(0, len(code), 1)]
	while pending:
		start: int
		limit: int
		depth: int
		start, limit, depth = pending.pop()
		del scope_parts[depth:]
		pos: int = start
		while pos < limit:
			if code[pos] not in "{;":
				pos += 1
				continue
			head: str = re.sub(r"\b(?:public|private|protected)\s*:", "", code[start:pos]).strip()
			end: int = pairs.get(pos, -1) if code[pos] == "{" else pos
			if end < 0 or end >= limit:
				raise ValueError("関数又はコンテナの閉じが無い")
			container: re.Match[str]|None = (
				re.fullmatch(r"(?:inline\s+)?namespace(?:\s+([\w:]+))?|(?:struct|class)\s+(\w+)(?:\s+final)?(?:\s*:[^{}]+)?", head)
			)
			if container and code[pos] == "{":
				name: str = container.group(1) or container.group(2) or ""
				pending.append((end + 1, limit, depth))
				scope_parts.append(name + ("::" if name else ""))
				pending.append((pos + 1, end, depth + 1))
				break
			match: re.Match[str]|None = re.match(r"^[\w:\s<>,*&]+?\b((?:\w+::)*\w+)\s*\(", head)
			if match:
				close: int = closing_delimiter(head, match.end() - 1, "(", ")")
				if re.fullmatch(r"\s*(?:const\s*)?(?:noexcept\s*)?(?:->\s*[\w:\s<>,*&]+)?", head[close + 1:]):
					name = match.group(1)
					args: list[str] = cpp_args(head[match.end():close])
					result: str = head[:match.start(1)] + head[close + 1:]
					found.append(
						("".join(scope_parts) + name, [] if args == ["void"] else args, code[pos + 1:end] if code[pos] == "{" else None, result)
					)
			pos = end + 1
			start = pos
	# 原文順の宣言の返戻
	return found

def construction_violations() -> list[str]:
	"""変更不能な出力と合成結果を定めた工程の外で構築出来る箇所を列挙する
	計算量：走査した経路数 P・対象ファイル数 F・総文字数 L に対して最悪 O(P+F log F+L²)
	戻値：
		設計の不変条件への違反の一覧
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		UnicodeDecodeError: 読み込んだファイルを指定の文字コードで解釈出来ない場合
	"""
	# 出力の構築の所有者の違反の収集
	path: Path = SRC / "Util/LayoutTypes.hpp"
	# コンストラクタと友の宣言の検査
	text: str = cpp_code(path.read_text(encoding="utf-8"))
	found: list[str] = []
	for name, owner in ("LaidOutCode", "friend class Core::CodeLayout;"), ("OutputDocument", ""):
		body: str = class_body(text, name)
		# コンストラクタは非公開の領域だけに置く
		sections: list[str] = re.split(r"\bpublic\s*:", body)
		if (
			not any(method == name for method, _, _, _ in contract_methods(sections[0])) or
			len(sections) > 1 and any(method == name for method, _, _, _ in contract_methods(sections[-1]))
		):
			found.append(f"{path.relative_to(ROOT)}: {name} のコンストラクタが非公開でない")
		friends: list[tuple[str, ...]] = [
			contract_tokens("friend class " + val + ";") for val in re.findall(r"\bfriend\s+class\s+([^;]+);", body)
		]
		if friends != ([contract_tokens(owner)] if owner else []):
			found.append(f"{path.relative_to(ROOT)}: {name} の友が定めた工程だけでない")
	# 合成結果を構築するのは `Compose` だけ（`OutputDocument(` の呼出を其の定義の中だけに許す）
	for src in sources():
		code: str = cpp_code(src.read_text(encoding="utf-8"))
		for head, _, function_body, _ in cpp_helper_entries(code):
			if function_body is None or head == "OutputDocument::Compose":
				continue
			if re.search(r"\bOutputDocument\s*\(", function_body):
				found.append(f"{src.relative_to(ROOT)}: OutputDocument を Compose の外で構築して居る")
	# 違反の一覧の返戻
	return found

def layout_comment_violations() -> list[str]:
	"""空行を決める CodeLayout がコメントの記録を受け取る箇所を列挙する
	計算量：走査した経路数 P・対象ファイル数 F・総文字数 L に対して O(P+F log F+L)
	戻値：
		設計の不変条件への違反の一覧
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		UnicodeDecodeError: 読み込んだファイルを指定の文字コードで解釈出来ない場合
	"""
	# コメントを配置構造へ渡す違反の収集
	found: list[str] = []
	for path in sources():
		if not path.stem.startswith("CodeLayout"):
			continue
		for num, line in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
			if re.search(r"\b(CommentRecord|CommentRepository|CommentPlacementPlan)\b", line) and not line.lstrip().startswith("//"):
				found.append(f"{path.relative_to(ROOT)}:{num}: CodeLayout がコメントの記録を扱って居る")
	# 違反の一覧の返戻
	return found

class CppBody(str):
	"""入子の型を分離した直属の範囲と，１度抽出した宣言のインデックス"""

	def __new__(cls: type[CppBody], source: str, ranges: list[tuple[int, int]]) -> CppBody:
		"""重ならない範囲だけをコピーし，宣言のインデックスを共有する
		計算量：直接の範囲の文字数 L に対して O(L)
		引数：
			cls: 構築する本体の型
			source: 範囲の所有者である原文
			ranges: 子の本体を除いた直接の範囲
		戻値：
			宣言とフィールドのインデックスを伴う本体
		送出例外：
			ValueError: 直接の宣言の囲みの対応を確認出来ない場合
		"""
		result: CppBody = super().__new__(cls, "".join(source[start:end] for start, end in ranges))
		result.ranges: tuple[tuple[int, int], ...] = tuple(ranges)
		result.declarations: list[str] = cpp_decls(str(result))
		result.fields: list[tuple[str, str]] = cpp_fields(result)
		# 直属の宣言のインデックスの返戻
		return result

def reachable_types(graph: dict[str, set[str]], start: str, stops: tuple[str, ...] = ()) -> set[str]:
	"""型からメンバ・公開の型・別名を辿って到達する名前を得る
	計算量：型数 V・到達関係の総数 E に対して O(V+E)
	引数：
		graph: 型の到達関係
		start: 調べる型
		stops: 到達だけを記録して其の先へ進まない型
	戻値：
		起点を含む到達する型名
	"""
	# 到達可能な型の収集
	found: set[str] = set()
	pending: list[str] = [start]
	while pending:
		name: str = pending.pop()
		if name in found:
			continue
		found.add(name)
		if name not in stops:
			pending.extend(graph.get(name, set()) - found)
	# 到達する型名の返戻
	return found

def cpp_predecessors(code: str) -> list[int]:
	"""各位置の直前のコードが終わる位置を共有する
	計算量：文字数 L に対して O(L)
	引数：
		code: コメントとリテラルを除いたテキスト
	戻値：
		各位置の直前の空白を除いた終端
	"""
	found: list[int] = []
	end: int = 0
	for pos, char in enumerate(code):
		found.append(end)
		if not char.isspace():
			end = pos + 1
	# 直前の非空白位置の返戻
	return found

def cpp_functions(code: str) -> list[tuple[str, list[str], str]]:
	"""修飾名を持つ関数定義の見出・仮引数・本体を得る
	計算量：文字数 L・関数数 F・返す本文量 B に対して O(L+F log L+B)
	引数：
		code: コメントとリテラルを除いたテキスト
	戻値：
		修飾名を含む見出，仮引数の列，本体の列
	送出例外：
		ValueError: 関数の囲みを確認出来ない場合
	"""
	# 区切の位置と括弧の対応を１度だけ収集する
	pairs: dict[int, int] = cpp_delimiters(code)
	boundaries: list[int] = [match.start() for match in re.finditer(r"[;{}]", code)]
	tail_pattern: re.Pattern[str] = re.compile(r"\s*(?:const\s*)?(?:noexcept\s*)?\{")
	found: list[tuple[str, list[str], str]] = []
	for match in re.finditer(r"\b[A-Za-z_]\w*\s*::\s*[A-Za-z_]\w*\s*\(", code):
		end: int = pairs.get(match.end() - 1, -1)
		if end < 0:
			raise ValueError("関数の仮引数の閉じが無い")
		tail: re.Match[str]|None = tail_pattern.match(code, end + 1)
		if not tail:
			continue
		opening: int = tail.end() - 1
		close: int = pairs.get(opening, -1)
		if close < 0:
			raise ValueError("関数本体の閉じが無い")
		boundary: int = bisect_left(boundaries, match.start())
		start: int = boundaries[boundary - 1] + 1 if boundary else 0
		head: str = re.sub(r"\s*::\s*", "::", code[start:match.end() - 1])
		found.append((head, cpp_args(code[match.end():end]), code[opening + 1:close]))
	# 関数定義の返戻
	return found

def is_immutable_code_type(code: str) -> bool:
	"""子のコード型の参照先に const が在るかを判定する
	計算量：展開済の型の文字数 L に対して O(L)
	引数：
		code: 別名を展開した型のテキスト
	戻値：
		LaidOutCode の前又は後に const が在るか
	"""
	# 変更不能なコード型かの判定の返戻
	return bool(re.search(r"\bconst\s+(?:\w+::)*LaidOutCode\b|\bLaidOutCode\s+const\b", code))

def owned_type_violations(bodies: dict[str, list[tuple[Path, str]]], aliases: dict[str, str], graph: dict[str, set[str]]) -> (
	list[str]
):
	"""所有領域から子の完成テキスト・コメント・検証データへ到達出来る型を検出する
	計算量：型数 V・到達関係 E・総文字数 L・別名数 A に対して最悪 O(V+E+(V+A+1)L²)
	引数：
		bodies: 型の定義位置と本文
		aliases: 別名の定義
		graph: 型の到達関係
	戻値：
		コード経路又は情報境界の違反の列
	送出例外：
		ValueError: 型の本文の波括弧の対応を確認出来ない場合
	"""
	# 所有する型の違反の収集
	found: list[str] = []
	if "OwnedRegion" not in bodies and "OwnedRegion" not in aliases:
		# 所有領域を確認出来なかった違反の返戻
		return ["OwnedRegion の定義が無く，情報境界を確認出来ない"]
	reached: set[str] = reachable_types(graph, "OwnedRegion")
	forbidden: set[str] = {
		"CommentBlock",
		"CommentPlacementPlan",
		"CommentRecord",
		"CommentRepository",
		"GrammarProfile",
		"OutputDocument",
		"TSNode",
		"ValidationContext"
	}
	for name in sorted(reached & forbidden):
		found.append(f"OwnedRegion から {name} へ型を介して到達出来る")
	outer: set[str] = reachable_types(graph, "OwnedRegion", ("LaidOutCode",))
	if "LaidOutCode" not in outer:
		found.append("OwnedRegion が確定した LaidOutCode の型付き経路を持たない")
	if outer & { "any", "char", "function", "string", "string_view" }:
		found.append("OwnedRegion のコード出力以外の経路がテキスト又は型を隠す任意の値を公開する")
	for name in sorted(outer - { "LaidOutCode" }):
		for path, body in bodies.get(name, []):
			for field, val in cpp_fields(body):
				expanded: str = expanded_type(aliases, val)
				if re.search(r"\bvoid\s*\*", expanded):
					found.append(f"{path.relative_to(ROOT)}: {name}.{field} が型を隠すポインタを持つ")
				if "LaidOutCode" in type_names(expanded) and not is_immutable_code_type(expanded):
					found.append(f"{path.relative_to(ROOT)}: {name}.{field} が変更不能な子のコードでない")
	# 型の違反の返戻
	return found

def cpp_globals(code: str) -> set[str]:
	"""関数・型の外に宣言した単純な状態の名前を得る
	計算量：テキストの文字数 L に対して O(L)
	引数：
		code: コメントとリテラルを除いたテキスト
	戻値：
		名前空間の直下も含む状態の名前（関数の間接参照とマクロは証明しない）
	送出例外：
		ValueError: 名前空間の囲みを確認出来ない場合
	"""
	# 名前空間の枠だけを除き，内部を１度の宣言走査へ渡す
	clean: str = re.sub(r"^[ \t]*#[^\n]*", "", code, flags=re.M)
	pairs: dict[int, int] = cpp_delimiters(clean)
	chars: list[str] = list(clean)
	for match in re.finditer(r"\bnamespace(?:\s+[\w:]+)?\s*\{", clean):
		end: int = pairs.get(match.end() - 1, -1)
		if end < 0:
			raise ValueError("namespace nesting is not understood")
		for pos in range(match.start(), match.end()):
			if chars[pos] != "\n":
				chars[pos] = " "
		chars[end] = " "
	# 関数外の状態の名前の返戻
	return {
		field for field, val in cpp_fields("".join(chars)) if not type_names(val) & { "class", "enum", "namespace", "struct" }
	}

def cpp_helper_calls(types: set[str], body: str) -> list[tuple[str, int]]:
	"""明示した自由関数又は静的関数の呼出と引数の数を得る
	入力のメンバ操作と標準ライブラリは此の狭い補助関数の追跡の対象外とする
	計算量：文字数 L・引数の抽出量 B に対して O(L+B)
	引数：
		types: 構築呼出を区別する既知の型
		body: 関数本体
	戻値：
		修飾名又は非修飾名と引数の数の列
	送出例外：
		ValueError: 呼出の引数を確認出来ない場合
	"""
	# 追跡する補助関数の呼出の収集
	found: list[tuple[str, int]] = []
	pairs: dict[int, int] = cpp_delimiters(body)
	previous: list[int] = cpp_predecessors(body)
	keywords: set[str] = { "alignof", "catch", "decltype", "for", "if", "noexcept", "sizeof", "switch", "while" }
	for match in re.finditer(r"(?<![\w:])((?:\w+\s*::\s*)*\w+)\s*\(", body):
		name: str = re.sub(r"\s*::\s*", "::", match.group(1))
		prefix: str = body[max(0, previous[match.start()] - 2):previous[match.start()]]
		if prefix.endswith(("->", ".")) or name.startswith("std::") or name in keywords or name.split("::")[-1] in types:
			continue
		end: int = pairs.get(match.end() - 1, -1)
		if end < 0:
			raise ValueError("呼出の仮引数の閉じが無い")
		found.append((name, len(cpp_args(body[match.end():end]))))
	# 追跡する呼出の返戻
	return found

def cpp_helper_signature(aliases: dict[str, str], types: set[str], args: list[str]) -> tuple[str, ...]:
	"""単純な仮引数の別名・const 記法・仮引数名の違いを照合用へ揃える
	計算量：テキストの文字数 L・別名数 A に対して最悪 O((A+1)L²)
	引数：
		aliases: 型の別名
		types: 既知の型
		args: 仮引数
	戻値：
		仮引数の順を保つ展開済の型の列
	送出例外：
		ValueError: 仮引数の波括弧の対応を確認出来ない場合
	"""
	# 補助関数の仮引数の型の照合
	found: list[str] = []
	types = types | { "bool", "char", "const", "double", "float", "int", "long", "short", "size_t", "void", "volatile" }
	for local_arg in args:
		arg: str = local_arg.split("=", 1)[0].strip()
		fields: list[tuple[str, str]] = cpp_fields(arg + ";")
		val: str = fields[0][1] if fields and fields[0][0] not in types and not fields[0][1].endswith("::") else arg
		val = expanded_type(aliases, val)
		val = re.sub(r"\b((?:\w+::)*\w+)\s+const\b", r"const \1", val)
		if not re.search(r"[<*&]", val):
			val = re.sub(r"\bconst\b", "", val)
		found.append(re.sub(r"\s+", "", val))
	# 仮引数の型の列の返戻
	return tuple(found)

def cpp_local_bindings(args: list[str], body: str) -> list[tuple[str, int, int]]:
	"""明示したローカル宣言と単純な for の束縛が有効なテキストの範囲を得る
	計算量：文字数 L・宣言の抽出量 B に対して O(L+B)
	引数：
		args: 仮引数
		body: 関数本体
	戻値：
		名前・有効範囲の先頭・末尾の列（末尾は含まない）
	送出例外：
		ValueError: 囲み又は for 本体の範囲を確認出来ない場合
	"""
	# ローカルの名前の導入と有効範囲の収集
	found: list[tuple[str, int, int]] = [(name, 0, len(body)) for arg in args for name, _ in cpp_fields(arg.split("=", 1)[0] + ";")]
	pairs: dict[int, int] = cpp_delimiters(body)
	# 同じ本体の全範囲を宣言毎に探さず，各位置の最内の終端を共有する
	scope_ends: list[int] = []
	pending: list[int] = []
	for pos, char in enumerate(body):
		if char == "}":
			if pending:
				pending.pop()
		scope_ends.append(pending[-1] if pending else len(body))
		if char == "{":
			if pos not in pairs:
				raise ValueError("有効範囲の閉じが無い")
			pending.append(pairs[pos])
	for match in re.finditer(r"(?:^|(?<=[;{}]))\s*([^;{}]+);", body):
		decl: str = match.group(1)
		for name, val in cpp_fields(decl + ";"):
			if type_names(val) & { "co_return", "co_yield", "delete", "else", "goto", "return", "throw" }:
				continue
			head: str = decl.split("=", 1)[0].strip()
			identifier: re.Match[str]|None = re.search(r"\b" + re.escape(name) + r"\b(?:\s*\[[^]]*\])?\s*$", head)
			if not identifier:
				continue
			start: int = match.start(1) + identifier.start()
			end: int = scope_ends[start]
			found.append((name, start, end))
	for match in re.finditer(r"\bfor\s*\(", body):
		close: int = pairs.get(match.end() - 1, -1)
		if close < 0:
			raise ValueError("for 見出の閉じが無い")
		header: str = body[match.end():close]
		separator: re.Match[str]|None = re.search(r";|(?<!:):(?!:)", header)
		if not separator:
			raise ValueError("for declaration is not understood")
		decl = header[:separator.start()]
		fields: list[tuple[str, str]] = cpp_fields(decl + ";")
		if len(fields) != 1:
			raise ValueError("for binding is not understood")
		declarator_name: str = fields[0][0]
		head = decl.split("=", 1)[0].strip()
		identifier = re.search(r"\b" + re.escape(declarator_name) + r"\b\s*$", head)
		if not identifier:
			raise ValueError("for declarator is not understood")
		pos: int = match.end() + identifier.start()
		start = close + 1
		while start < len(body) and body[start].isspace():
			start += 1
		if start == len(body) or re.compile(r"(?:if|for|while|switch|try|do)\b").match(body, start):
			raise ValueError("for statement scope is not understood")
		end = pairs.get(start, -1) if body[start] == "{" else body.find(";", start)
		if end == -1:
			raise ValueError("for body is not terminated")
		# 範囲 for の初期化式では外側の値を読み，本体だけで反復変数を束縛する
		found.append((declarator_name, pos, pos + len(declarator_name)))
		found.append((declarator_name, pos if separator.group() == ";" else start, end + 1))
	# ローカル束縛の有効範囲の返戻
	return found

def cpp_state_reads(external: set[str], args: list[str], body: str) -> set[str]:
	"""別の型の同名メンバとローカル束縛を，関数外の状態の参照から区別する
	計算量：文字数 L・束縛数 B に対して O(L+B log B)
	引数：
		external: 外側の状態名
		args: 仮引数
		body: 関数本体
	戻値：
		外側の状態を参照する識別子の集合
	送出例外：
		ValueError: ローカル束縛を確認出来ない場合
	"""
	# 現在の状態を読む名前の収集
	bindings: list[tuple[str, int, int]] = cpp_local_bindings(args, body)
	found: set[str] = set()
	previous: list[int] = cpp_predecessors(body)
	# 重なる束縛も個数で保持し，使用位置迄の導入と終了だけを処理する
	events: list[tuple[int, int, str]] = (
		sorted((position, delta, name) for name, first, last in bindings for position, delta in ((first, 1), (last, -1)))
	)
	active: dict[str, int] = {}
	delta: int
	cursor: int = 0
	for match in re.finditer(r"\b[A-Za-z_]\w*\b", body):
		while cursor < len(events) and events[cursor][0] <= match.start():
			_, delta, name = events[cursor]
			active[name] = active.get(name, 0) + delta
			cursor += 1
		name: str = match.group()
		if name not in external:
			continue
		prefix: str = body[max(0, previous[match.start()] - 2):previous[match.start()]]
		if prefix.endswith(("->", ".")) or not prefix.endswith("::") and active.get(name, 0):
			continue
		found.add(name)
	# 外側の状態への参照の返戻
	return found

def composition_helper_violations(
	codes: dict[Path, str],
	bodies: dict[str, list[tuple[Path, str]]],
	aliases: dict[str, str],
	graph: dict[str, set[str]]
) -> list[str]:
	"""Compose から明示した補助関数の定義へ辿り，間接の第３入力を検出する
	名前空間の探索・引数の型に依る多重定義の解決は証明せず，曖昧な呼出は確認不能とする
	計算量：総文字数 L・関数数 N・呼出数 E・別名数 A に対して最悪 O((N+E+1)(A+1)L³)
	引数：
		codes: テキスト
		bodies: 型の本文
		aliases: 型の別名
		graph: 型の到達関係
	戻値：
		状態又は禁止型への到達，及び呼出先を確認出来ない診断の列
	送出例外：
		ValueError: 宣言・呼出の囲みを確認出来ない場合
	"""
	# 合成に使う補助関数の境界の照合
	name: str
	path: Path
	args: list[str]
	body: str|None
	result: str
	route: tuple[str, ...]
	key: str
	target: Path
	params: list[str]
	text: str
	returned: str
	entries: dict[str, list[tuple[Path, list[str], str|None, str]]] = {}
	states: dict[Path, set[str]] = {}
	for path, code in codes.items():
		# コンパイル時定数は実行時の第３入力として数えない
		state: str = re.sub(r"\b(?:static\s+)?(?:inline\s+)?constexpr\s+[^;{}]+;", "", code)
		states[path] = cpp_globals(state)
		for name, args, body, result in cpp_helper_entries(code):
			entries.setdefault(name, []).append((path, args, body, result))
	header_states: set[str] = set().union(*(names for path, names in states.items() if path.suffix in (".h", ".hpp")))
	static_fields: set[str] = set()
	for items in bodies.values():
		for _, body in items:
			static_fields.update(
				name for name, val in cpp_fields(body) if "static" in type_names(val) and "constexpr" not in type_names(val)
			)
	found: list[str] = []
	pending: list[tuple[str, Path, list[str], str, str, tuple[str, ...]]] = [
		(name, path, args, body, result, (name,)) for name, items in entries.items() if name.endswith(
			"OutputDocument::Compose"
		) for path, args, body, result in items if body is not None
	]
	if not pending:
		# 起点の無い呼出追跡を成功としない確認不能の返戻
		return ["Compose の補助関数の追跡の起点となる定義を確認出来ない"]
	seen: set[tuple[str, Path]] = set()
	while pending:
		name, path, args, body, result, route = pending.pop()
		if (name, path) in seen:
			continue
		seen.add((name, path))
		state = re.sub(r"\bstatic\s+constexpr\s+[^;]*;", "", body)
		external: set[str] = states[path] | header_states | static_fields
		reached: set[str] = cpp_state_reads(external, args, state)
		label: str = " → ".join(route)
		if reached or re.search(r"\b(?:static|thread_local|extern)\b", state):
			found.append(f"{path.relative_to(ROOT)}: Compose の補助関数の経路 {label} が関数外又は静的な状態を参照: {sorted(reached)}")
		forbidden: set[str] = { "CommentRepository", "GrammarProfile", "TSNode", "ValidationContext" }
		dependencies: set[str] = type_names(expanded_type(aliases, result + " " + " ".join(args) + " " + body))
		reached_types: set[str] = set().union(*(reachable_types(graph, item) for item in dependencies))
		if reached_types & forbidden:
			found.append(f"{path.relative_to(ROOT)}: Compose の補助関数の経路 {label} が禁止した型へ到達出来る")
		for called, count in cpp_helper_calls(bodies.keys() | aliases.keys(), body):
			choices: list[str]
			if "::" in called:
				choices = [called] if called in entries else []
			else:
				local: str = name.rpartition("::")[0] + "::" + called
				choices = [local] if local in entries else [key for key in entries if key.split("::")[-1] == called]
			for key in choices:
				for target, params, _, returned in entries[key]:
					dependencies = type_names(expanded_type(aliases, returned + " " + " ".join(params)))
					if (reached_types := set().union(*(reachable_types(graph, item) for item in dependencies))) & forbidden:
						found.append(f"{target.relative_to(ROOT)}: Compose の補助関数の経路 {label} → {key} の宣言が禁止した型を持つ")
			defs: list[tuple[str, Path, list[str], str, str]] = [
				(key, target, params, text, returned) for key in choices for target, params, text, returned in entries[key] if text is not
				None and len(params) == count
			]
			signatures: set[tuple[str, ...]] = {
				cpp_helper_signature(aliases, bodies.keys() | aliases.keys(), params) for key in choices for _, params, _, _ in
				entries[key] if len(params) == count
			}
			if len(defs) != 1 or len(signatures) != 1:
				found.append(f"{path.relative_to(ROOT)}: Compose の補助関数の経路 {label} → {called} の定義を一意に確認出来ない")
				continue
			key, target, params, text, returned = defs[0]
			pending.append((key, target, params, text, returned, (*route, key)))
	# 間接経路の違反と確認不能の返戻
	return found

def composition_violations(
	codes: dict[Path, str],
	bodies: dict[str, list[tuple[Path, str]]],
	aliases: dict[str, str],
	graph: dict[str, set[str]]
) -> list[str]:
	"""合成を２入力へ限定し，保有状態・追加引数・観測用の切替に依る別経路を検出する
	計算量：総文字数 L・別名数 A・型数 V・到達関係 E に対して最悪 O((A+1)L³+V+E)
	引数：
		codes: 対象のテキスト
		bodies: 型の定義位置と本文
		aliases: 別名の定義
		graph: 型の到達関係
	戻値：
		合成の入力又は別経路の違反の列
	送出例外：
		ValueError: 型・関数・呼出の囲みの対応を確認出来ない場合
	"""
	# 合成の境界の違反の収集
	found: list[str] = []
	forbidden: set[str] = { "CommentRepository", "GrammarProfile", "TSNode", "ValidationContext" }
	for name in sorted(reachable_types(graph, "OutputDocument") & forbidden):
		found.append(f"OutputDocument から {name} へ型を介して到達出来る")
	decls: int = 0
	for path, body in bodies.get("OutputDocument", []):
		inputs: list[str] = []
		lens: list[str] = []
		for field, val in cpp_fields(body):
			expanded: str = expanded_type(aliases, val).strip()
			names: set[str] = type_names(expanded)
			inputs.extend(sorted(names & { "CommentPlacementPlan", "LaidOutCode" }))
			if "LaidOutCode" in names and not is_immutable_code_type(expanded):
				found.append(f"{path.relative_to(ROOT)}: OutputDocument.{field} が変更可能なコードを持つ")
			if not names & { "CommentPlacementPlan", "LaidOutCode" }:
				if re.fullmatch(r"(?:const\s+)?(?:std::)?size_t", expanded):
					lens.append(field)
				else:
					found.append(f"{path.relative_to(ROOT)}: OutputDocument.{field} が２入力以外の保有経路")
		if len(lens) > 1:
			found.append(f"{path.relative_to(ROOT)}: OutputDocument の追加の整数状態を合成長だけと確認出来ない")
		if sorted(inputs) != ["CommentPlacementPlan", "LaidOutCode"]:
			found.append(f"{path.relative_to(ROOT)}: OutputDocument がコードと配置計画を各々１経路で保持して居ない")
		for match in re.finditer(r"\bCompose\s*\(", body):
			end: int = closing_delimiter(body, match.end() - 1, "(", ")")
			args: list[str] = cpp_args(body[match.end():end])
			decls += 1
			names: list[set[str]] = [type_names(expanded_type(aliases, arg)) for arg in args]
			if len(args) != 2 or "LaidOutCode" not in names[0] or "CommentPlacementPlan" not in names[1]:
				found.append(f"{path.relative_to(ROOT)}: Compose がコードと配置計画だけの２入力でない")
			elif not is_immutable_code_type(expanded_type(aliases, args[0])):
				found.append(f"{path.relative_to(ROOT)}: Compose が変更可能なコードを受ける")
	if not decls:
		found.append("OutputDocument::Compose の宣言を確認出来ない")
	defs: int = 0
	for path, code in codes.items():
		for match in re.finditer(r"\bOutputDocument::Compose\s*\(", code):
			end: int = closing_delimiter(code, match.end() - 1, "(", ")")
			if len(cpp_args(code[match.end():end])) != 2:
				found.append(f"{path.relative_to(ROOT)}: Compose の定義又は呼出が２入力でない")
		for head, args, body in cpp_functions(code):
			if re.search(r"\bOutputDocument::\w+$", head.rstrip()):
				for call in re.finditer(r"(?<![\w:])Compose\s*\(", body):
					end = closing_delimiter(body, call.end() - 1, "(", ")")
					if len(cpp_args(body[call.end():end])) != 2:
						found.append(f"{path.relative_to(ROOT)}: 合成結果の関数内の Compose 呼出が２入力でない")
			if head.rstrip().endswith("OutputDocument::Compose"):
				defs += 1
				state: str = re.sub(r"\bstatic\s+constexpr\s+[^;]*;", "", body)
				if len(args) != 2 or re.search(r"\b(?:thread_local|static|extern)\b", state):
					found.append(f"{path.relative_to(ROOT)}: Compose が追加入力又は隠れた状態を持つ")
				if type_names(body) & cpp_globals(code):
					found.append(f"{path.relative_to(ROOT)}: Compose が関数外の状態を参照し，２入力だけと確認出来ない")
		if path.stem.startswith(("LayoutTypes", "OutputFinalizer")) and re.search(r"\bIncludes?OwnedComments\b", code):
			found.append(f"{path.relative_to(ROOT)}: 子のコメントを合成後に切り替える別経路が在る")
	if not defs:
		found.append("OutputDocument::Compose の定義を確認出来ない")
	found += composition_helper_violations(codes, bodies, aliases, graph)
	# 合成の違反の返戻
	return found

def serialization_violations(codes: dict[Path, str], aliases: dict[str, str]) -> list[str]:
	"""シリアライズに所有領域・構文探索・子のテキスト置換又は配置救済を持ち込む箇所を検出する
	計算量：総文字数 L・別名数 A に対して最悪 O((A+1)L²)
	引数：
		codes: コメントとリテラルを除いた対象のテキスト
		aliases: 別名の定義
	戻値：
		シリアライズの情報境界又は後付処理の違反の列
	送出例外：
		ValueError: 関数の囲みの対応を確認出来ない場合
	"""
	# シリアライズの境界の違反の収集
	found: list[str] = []
	defs: int = 0
	for path, code in codes.items():
		parts: list[str] = (
			[code] if
			path.stem.startswith("OutputFinalizer") else
			[body for head, _, body in cpp_functions(code) if head.rstrip().endswith("OutputFinalizer::Finalize")]
		)
		for part in parts:
			body: str = expanded_type(aliases, part)
			defs += "GetEnding" in body
			for match in re.finditer(
				r"\b(?:SyntaxModel|SyntaxNode|OwnedRegion|GetRegionOf|GetNode|GetNodeCount|GetChildrenOf)\b|"
				r"(?:\.|->)\s*Syntax\b|\.\s*(?:replace|erase|insert)\s*\(",
				body
			):
				found.append(f"{path.relative_to(ROOT)}: Finalizer が構文・子のテキスト又はコードの書換を扱う: {match.group().strip()}")
	if not defs:
		found.append("Finalizer が確定済の終端契約を読む経路を確認出来ない")
	# シリアライズの違反の返戻
	return found

def cpp_var_types(aliases: dict[str, str], args: list[str], body: str) -> dict[str, set[str]]:
	"""仮引数・明示型のローカル変数・範囲反復の変数の型を控える
	計算量：テキストの文字数 L・別名数 A に対して最悪 O((A+1)L²)
	引数：
		aliases: 別名の定義
		args: 仮引数
		body: テキスト
	戻値：
		変数名毎の型の識別子（auto の推論は行わない）
	送出例外：
		ValueError: 宣言の波括弧の対応を確認出来ない場合
	"""
	# 変数名から型へのインデックスの構築
	decls: list[str] = list(args)
	for match in re.finditer(r"(?:^|[;{}])\s*([^;{}]+?)\s*=[^;{}]*;", body):
		decls.append(match.group(1))
	for match in re.finditer(r"\bfor\s*\(\s*([^()]+?)(?<!:):(?!:)\s*[^()]+\)", body):
		decls.append(match.group(1))
	found: dict[str, set[str]] = {}
	for decl in decls:
		for name, val in cpp_fields(decl.strip() + ";"):
			found[name] = type_names(expanded_type(aliases, val))
	# 明示した型の返戻
	return found

def cpp_expr_types(
	bodies: dict[str, list[tuple[Path, str]]],
	aliases: dict[str, str],
	vars: dict[str, set[str]],
	expr: str
) -> set[str]:
	"""変数からデータメンバを辿る式の型を，宣言から求める
	計算量：式の段数 D・型数 V・テキストの総長 L・別名数 A に対して最悪 O(DV(A+1)L²)
	引数：
		bodies: 型本文
		aliases: 別名
		vars: 明示型
		expr: メンバ参照
	戻値：
		型の識別子（取得関数・auto・マクロに依る未確認の型は空集合）
	送出例外：
		ValueError: 型本文の波括弧の対応を確認出来ない場合
	"""
	# 式が参照する型の抽出
	names: list[str] = re.findall(r"\b[A-Za-z_]\w*\b", expr)
	if not names:
		# 式を確認出来ない場合の空集合の返戻
		return set()
	current: set[str] = vars.get(names[0], set())
	for field in names[1:]:
		following: set[str] = set()
		for name in current:
			for _, body in bodies.get(name, []):
				for member, val in cpp_fields(body):
					if member == field:
						following.update(type_names(expanded_type(aliases, val)))
		current = following
	# メンバ参照の型の返戻
	return current

def child_payload_roles(
	bodies: dict[str, list[tuple[Path, str]]],
	aliases: dict[str, str],
	vars: dict[str, set[str]],
	carriers: dict[str, dict[str, str]],
	payload: str
) -> dict[str, set[str]]:
	"""渡した子の値を，変数毎にコード・コメント・検証の型へ照合する
	全体の値渡も宣言で確認し，同じフィールド名を持つ別の型を代用しない
	計算量：式数 X・テキストの総長 L・型数 V・別名数 A に対して最悪 O(XV(A+1)L³)
	引数：
		bodies: 型本文
		aliases: 別名
		vars: 変数の型
		carriers: ３経路の宣言
		payload: 渡す式
	戻値：
		元の子の変数毎に明示した経路の集合
	送出例外：
		ValueError: 型本文の波括弧の対応を確認出来ない場合
	"""
	# 子の結果が持つ型の役割の収集
	roles: set[str] = { "CommentRepository", "LaidOutCode", "ValidationContext" }
	found: dict[str, set[str]] = {}
	for match in re.finditer(r"\b\w+(?:(?:\.|->)\s*\w+)+", payload):
		expr: str = match.group()
		owner: str = re.split(r"\s*(?:\.|->)\s*", expr)[0]
		found.setdefault(owner, set()).update(cpp_expr_types(bodies, aliases, vars, expr) & roles)
	for match in re.finditer(r"(?<![\w.:>])\b(\w+)\b(?!\s*(?:\.|->))", payload):
		owner = match.group(1)
		if vars.get(owner, set()) & carriers.keys():
			found.setdefault(owner, set()).update(roles)
	# 子の変数毎の経路の返戻
	return found

def child_route_violations(codes: dict[Path, str], bodies: dict[str, list[tuple[Path, str]]], aliases: dict[str, str]) -> (
	list[str]
):
	"""子の３経路の宣言と，登録先・配置元・検証元の型付参照を照合する
	補助型とフィールドの名前は宣言から取得し，投影側の private 入力経路も調べる
	完全な C++ の型解決・値の流れ・寿命は証明せず，明示型を確認出来ない経路は失敗とする
	計算量：総文字数 L・型数 V・別名数 A に対して最悪 O((V+A+1)L⁴)
	引数：
		codes: テキスト
		bodies: 型の位置と本文
		aliases: 別名
	戻値：
		違反又は確認出来ない経路の列
	送出例外：
		ValueError: 型・関数・呼出の囲みの対応を確認出来ない場合
	"""
	# 子の結果を渡す経路の違反の収集
	roles: set[str] = { "CommentRepository", "LaidOutCode", "ValidationContext" }
	carriers: dict[str, dict[str, str]] = {}
	for name, entries in bodies.items():
		for _, body in entries:
			routes: dict[str, str] = {}
			for field, val in cpp_fields(body):
				matched: set[str] = type_names(expanded_type(aliases, val)) & roles
				if len(matched) == 1:
					routes[next(iter(matched))] = field
			if len(routes) == 3 and len(set(routes.values())) == 3:
				carriers[name] = routes
	found: list[str] = []
	if not carriers:
		found.append("子のコード・コメント・検証データを別々に持つ型付き経路を確認出来ない")
	control: str = "\n".join(code for path, code in codes.items() if path.stem.startswith("Formatter"))
	if re.search(r"\b(?:GetNodeCount|GetRegionOf|GetNode|GetChildrenOf)\s*\(", control):
		found.append("Formatter がノード表を探索して子の所有経路の対応を復元して居る")
	producers: list[tuple[list[str], str]] = []
	consumers: dict[str, list[tuple[list[str], str]]] = { "CommentRepository": [], "ValidationContext": [] }
	for code in codes.values():
		for head, args, body in cpp_functions(code):
			# 入れ子のクラスの関数も其のクラスの経路として読む
			if re.search(r"\b(?:Formatter|SourceProjector)(?:::\w+)*::\w+$", head.rstrip()):
				producers.append((args, body))
			if re.search(r"\bCommentPlacer(?:::\w+)*::\w+$", head.rstrip()):
				consumers["CommentRepository"].append((args, body))
			if re.search(r"\bSyntaxValidator(?:::\w+)*::\w+$", head.rstrip()):
				consumers["ValidationContext"].append((args, body))
	access: str = r"\b\w+(?:(?:\.|->)\s*\w+)*"
	for role in "CommentRepository", "ValidationContext":
		members: set[str] = set()
		for _, body in bodies.get(role, []):
			for field, val in cpp_fields(body):
				names: set[str] = type_names(expanded_type(aliases, val))
				if role in names or names & carriers.keys():
					members.add(field)
		published: set[str] = set()
		consumed: set[str] = set()
		for args, body in producers:
			vars: dict[str, set[str]] = cpp_var_types(aliases, args, body)
			for match in re.finditer(r"(" + access + r")(?:\.|->)\s*(\w+)\s*\.\s*(?:emplace|push_back|insert)\s*\(", body):
				receiver: str = match.group(1)
				field: str = match.group(2)
				if field not in members or role not in cpp_expr_types(bodies, aliases, vars, receiver):
					continue
				end: int = closing_delimiter(body, match.end() - 1, "(", ")")
				vals: dict[str, set[str]] = child_payload_roles(bodies, aliases, vars, carriers, body[match.end():end])
				if role == "CommentRepository":
					if any(role in valueset for valueset in vals.values()):
						published.add(field)
				elif any(roles <= valueset for valueset in vals.values()):
					published.add(field)
		for args, body in consumers[role]:
			vars = cpp_var_types(aliases, args, body)
			if role == "CommentRepository":
				for match in re.finditer(r"(" + access + r")(?:\.|->)\s*(\w+)\s*\.\s*find\s*\(", body):
					receiver = match.group(1)
					field = match.group(2)
					lookup: re.Match[str]|None = re.search(r"\b(\w+)\s*=\s*$", body[:match.start()])
					if field not in members or not lookup or role not in cpp_expr_types(bodies, aliases, vars, receiver):
						continue
					iterator: str = lookup.group(1)
					if re.search(r"\b\w+\s*\([^;{}]*\*\s*" + re.escape(iterator) + r"\s*->\s*second\b", body):
						consumed.add(field)
			else:
				for loop in re.finditer(r"\bfor\s*\(\s*([^()]+?)(?<!:):(?!:)\s*(" + access + r")\s*\)", body):
					src: str = loop.group(2)
					parts: list[str] = re.split(r"\s*(?:\.|->)\s*", src)
					field = parts[-1]
					if field not in members or len(parts) < 2 or role not in cpp_expr_types(bodies, aliases, vars, ".".join(parts[:-1])):
						continue
					declared: list[tuple[str, str]] = cpp_fields(loop.group(1).strip() + ";")
					# 後の同名の反復変数の型を持ち込まず，此の本体の束縛と呼出だけを照合する
					start: int = loop.end()
					while start < len(body) and body[start].isspace():
						start += 1
					end = closing_delimiter(body, start, "{", "}") if body[start] == "{" else body.index(";", start)
					local: str = body[start:end + 1]
					for call in re.finditer(r"\bValidate\s*\(", local):
						end = closing_delimiter(local, call.end() - 1, "(", ")")
						local_vars: dict[str, set[str]] = dict(vars)
						local_vars.update(cpp_var_types(aliases, [loop.group(1)], local[:call.start()]))
						payload: str = local[call.end():end]
						# 同じ反復本体で明示した型のコピー・移動も，元の子の経路へ照合する
						prefix: str = local[:call.start()]
						for copied in re.finditer(
							r"\bValidationContext\s+(\w+)\s*=\s*(?:std::move\s*\(\s*\*\s*(" + access + r")\s*\)|\*\s*(" + access + r"))\s*;",
							prefix
						):
							name: str = copied.group(1)
							origin: str = copied.group(2) or copied.group(3)
							if (
								"ValidationContext" not in cpp_expr_types(bodies, aliases, local_vars, origin) or
								re.search(r"\b" + re.escape(name) + r"\s*=", prefix[copied.end():])
							):
								continue
							payload = re.sub(r"\b" + re.escape(name) + r"\b", "(" + origin + ")", payload)
						vals = child_payload_roles(bodies, aliases, local_vars, carriers, payload)
						if any(roles <= vals.get(name, set()) for name, _ in declared):
							consumed.add(field)
		if not published & consumed:
			found.append(f"子の {role} を親の専用経路へ登録し，配置又は検証で読む参照を確認出来ない")
	has_code_route: bool = False
	for path, code in codes.items():
		if path.parent == SRC / "Core":
			continue
		for head, args, body in cpp_functions(code):
			if "OwnedRegion" not in type_names(head):
				continue
			params: str = expanded_type(aliases, " ".join(args))
			if "LaidOutCode" in type_names(params) and not type_names(params) & { "CommentRepository", "ValidationContext", "string" }:
				for arg in args:
					for member, val in cpp_fields(arg + ";"):
						if "LaidOutCode" in type_names(expanded_type(aliases, val)) and re.search(r"\b" + re.escape(member) + r"\b", body):
							has_code_route = True
	if not has_code_route:
		found.append("入力側から所有領域へ確定した子のコードだけを渡す関数を確認出来ない")
	# 独立した所有経路の違反の返戻
	return found

def child_route_self_check() -> list[str]:
	"""子の経路の自己検査で，同名のローカル束縛と別の本体の呼出を区別する
	期待は整形仕様 3.1, 3.5 の３経路の遮断から定める
	計算量：テストケース数 F・１テストケースの最大照合費用 T に対して O(FT)
	戻値：
		手書の構造の正誤と判定が異なる物の列
	送出例外：
		ValueError: 型・関数・呼出の囲みの対応を確認出来ない場合
	"""
	# 最小の３経路と，後の本体で同じ名前を持つ別の型の宣言
	src: str = """
struct LaidOutCode {};
struct CommentRepository { std::map<int, CommentRepository *> OwnedChildren; };
struct Payload;
struct ValidationContext { std::vector<Payload> OwnedChildren; };
struct Payload { LaidOutCode *Code; CommentRepository *Comments; ValidationContext *Context; };
struct OtherChild { int Start; };
struct ProjectionResult { CommentRepository Comments; ValidationContext Validation; };
struct OwnedRegion { LaidOutCode *Code; };
OwnedRegion ParseAdapter::MakeRegion(LaidOutCode *Code) { return { Code }; }
void SourceProjector::Publish(Payload &Child, ProjectionResult &Result) {
	Result.Comments.OwnedChildren.emplace(0, Child.Comments);
	Result.Validation.OwnedChildren.push_back(Child);
}
void CommentPlacer::Place(const CommentRepository &Repository) {
	const std::map<int, CommentRepository *>::const_iterator Found = Repository.OwnedChildren.find(0);
	Use(*Found->second);
}
void SyntaxValidator::Validate(const ValidationContext &Context, std::vector<OtherChild> &Children) {
	for(const Payload &Child : Context.OwnedChildren) {
		Validate(Child.Code, *Child.Comments, *Child.Context);
	}
	for(OtherChild &Child : Children) Child.Start = 0;
}
"""
	# 同名の別の束縛の影響，経路の欠落及びループ外の無関係な呼出の照合
	found: list[str] = []
	if cpp_functions(cpp_code("void Owner /* gap */ :: f(){ g(); }")) != [("void Owner::f", [], " g(); ")]:
		found.append("修飾名の区切を跨いだ関数定義を読み落とした")
	if cpp_helper_calls(set(), cpp_code("N /* gap */ :: f(1);")) != [("N::f", 1)]:
		found.append("修飾名の区切を跨いだ補助関数を読み落とした")
	if cpp_globals("int A; namespace N { int B; namespace M { int C; } void f(){ int Local; } }") != { "A", "B", "C" }:
		found.append("名前空間の状態又は関数内のローカル束縛を取り違えた")
	if (
		cpp_helper_entries("namespace N { namespace M { void f(){} } void g(); }") !=
		[("N::M::f", [], "", "void "), ("N::g", [], None, "void ")]
	):
		found.append("名前空間の所属又は宣言順を取り違えた")
	if cpp_state_reads({ "State" }, [], "{ int State; use(State); } use(State);") != { "State" }:
		found.append("終了したローカル束縛を外側へ持ち越した")
	if cpp_state_reads({ "State" }, [], "{ int State; use(State); }"):
		found.append("ローカル束縛を外側の状態と取り違えた")
	for name, candidate, expected in (
		("後の同名の束縛", src, 0),
		(
			"所有する子の文脈のコピー",
			src.replace(
				"Validate(Child.Code, *Child.Comments, *Child.Context);",
				"ValidationContext Copy = *Child.Context; Validate(Child.Code, *Child.Comments, std::move(Copy));"
			),
			0
		),
		(
			"子に属さない文脈のコピー",
			src.replace(
				"Validate(Child.Code, *Child.Comments, *Child.Context);",
				"Payload Other; ValidationContext Copy = *Other.Context; Validate(Child.Code, *Child.Comments, std::move(Copy));"
			),
			1
		),
		(
			"コピー後の別の文脈への代入",
			src.replace(
				"Validate(Child.Code, *Child.Comments, *Child.Context);",
				"ValidationContext Copy = *Child.Context; Copy = Context; Validate(Child.Code, *Child.Comments, std::move(Copy));"
			),
			1
		),
		("検証データの登録の欠落", src.replace("Result.Validation.OwnedChildren.push_back(Child);", ""), 1),
		("子の検証の欠落", src.replace("Validate(Child.Code, *Child.Comments, *Child.Context);", ""), 1),
		(
			"別の本体の検証呼出",
			src.replace("Validate(Child.Code, *Child.Comments, *Child.Context);", "").replace(
				"for(OtherChild &Child : Children) Child.Start = 0;",
				"for(const Payload &Child : Other) { Validate(Child.Code, *Child.Comments, *Child.Context); }"
			),
			1
		)
	):
		codes: dict[Path, str] = { SRC / "Util/ChildRouteFixture.cpp": cpp_code(candidate) }
		bodies: dict[str, list[tuple[Path, str]]]
		aliases: dict[str, str]
		bodies, aliases = cpp_type_idx(codes)[:2]
		if len(child_route_violations(codes, bodies, aliases)) != expected:
			found.append(f"{name}：子の経路の判定が条文と違う")
	# 自己検査の食違の返戻
	return found

def owned_region_violations() -> list[str]:
	"""所有領域の型，２入力の合成，シリアライズ及び子の３経路を仕様の情報境界で検査する
	計算量：走査した経路数 P・ファイル数 F・総文字数 L・型数 V・別名数 A・到達関係 E に対して最悪 O(P+F log F+(V+A+1)L⁴+E)
	戻値：
		違反及び機械的に確認出来なかった条件の列
	送出例外：
		OSError: 対象ソースを読めない場合（検査を成功とせず終了する）
		UnicodeDecodeError: 読み込んだファイルを指定の文字コードで解釈出来ない場合
	"""
	# 実装のテキストを確認し，コメントに書いた設計の説明だけで成功にしない
	codes: dict[Path, str] = { path: cpp_code(path.read_text(encoding="utf-8")) for path in sources() }
	try:
		bodies: dict[str, list[tuple[Path, str]]]
		aliases: dict[str, str]
		graph: dict[str, set[str]]
		bodies, aliases, graph = cpp_type_idx(codes)
		found: list[str] = owned_type_violations(bodies, aliases, graph)
		found += composition_violations(codes, bodies, aliases, graph)
		found += serialization_violations(codes, aliases)
		found += child_route_violations(codes, bodies, aliases)
	except ValueError as e:
		# テキストを読めない検査を成功へ変えず，確認不能の違反の返戻
		return [f"所有領域の情報境界を確認出来ない: {e}"]
	# 情報境界の違反の返戻
	return found

def comment_structure_violations() -> list[str]:
	"""本文構造の判定の入口・見方・結果の形と，工程間の共有を外れる箇所を列挙する
	計算量：走査した経路数 P・対象ファイル数 F・総文字数 L に対して最悪 O(P+F log F+L²)
	戻値：
		設計の不変条件への違反の一覧
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		UnicodeDecodeError: 読み込んだファイルを指定の文字コードで解釈出来ない場合
	"""
	# 読取専用の見方のフィールドと公開関数の照合
	path: Path = SRC / "Util/CommentNormalizer.hpp"
	text: str = cpp_code(path.read_text(encoding="utf-8"))
	found: list[str] = []
	# 見方は判定する記録と選択したグループの位置を読取専用で持ち，記法とドキュメント形式の定義（コード例の書式）・本文・順序・所属だけを読取専用の関数で公開する
	view: str = class_body(text, "ReadOnlyCommentView")
	actual_fields: list[tuple[str, tuple[str, ...]]] = [(name, contract_tokens(value)) for name, value in cpp_fields(view)]
	expected_fields: list[tuple[str, tuple[str, ...]]] = [
		("Records", contract_tokens("std::span<const CommentRecord>")),
		("Selection", contract_tokens("std::span<const size_t>"))
	]
	if actual_fields != expected_fields:
		found.append(f"{path.relative_to(ROOT)}: ReadOnlyCommentView のフィールドが読取専用の記録の並びと選択位置だけでない")
	public_view: str = re.split(r"\bpublic\s*:", view)[-1]
	getters: list[tuple[str, bool]] = [
		(name, "const" in suffix) for name, _, suffix, _ in contract_methods(public_view) if name != "ReadOnlyCommentView"
	]
	expected: list[tuple[str, bool]] = [(name, True) for name in ("GetCodeExamples", "GetGroup", "GetOrder", "GetSize", "GetText")]
	if sorted(getters) != expected:
		found.append(f"{path.relative_to(ROOT)}: ReadOnlyCommentView の公開が記法・本文・順序・所属の読取だけでない")
	# 結果は判定だけが構築し，公開の関数は全て読取専用とする
	result: str = class_body(text, "CommentStructure")
	public: str = re.split(r"\bpublic\s*:", result)[-1]
	# 判定結果の構築権と読取専用性の照合
	friends: list[str] = re.findall(r"\bfriend\s+class\s+([\w:]+)\s*;", result)
	if friends != ["CommentNormalizer"]:
		found.append(f"{path.relative_to(ROOT)}: CommentStructure の友が判定の工程だけでない")
	methods: list[MethodContract] = contract_methods(public)
	operands: tuple[tuple[str, ...], tuple[str, ...]] = (
		contract_tokens("const CommentStructure &"),
		contract_tokens("CommentStructure &&")
	)
	constructors: list[tuple[tuple[str, ...], ...]] = [args for name, args, _, _ in methods if name == "CommentStructure"]
	private: str = re.split(r"\bpublic\s*:", result)[0]
	if (
		not any(name == "CommentStructure" and not args for name, args, _, _ in contract_methods(private)) or
		constructors != [(operand,) for operand in operands]
	):
		found.append(f"{path.relative_to(ROOT)}: CommentStructure の構築権が判定の工程に限定されて居ない")
	for operand in operands:
		if not any(name == "operator=" and args == (operand,) and "delete" in suffix for name, args, suffix, _ in methods):
			found.append(f"{path.relative_to(ROOT)}: CommentStructure の代入が削除されて居ない")
	for name, _, suffix, _ in methods:
		if name not in ("CommentStructure", "operator=") and "const" not in suffix:
			found.append(f"{path.relative_to(ROOT)}: CommentStructure の公開関数 `{name}` が読取専用でない")
	# 判定の入口は仮引数名でなく，見方・結果・修飾の型で照合する
	inspections: list[MethodContract] = [
		item for item in contract_methods(class_body(text, "CommentNormalizer")) if item[0] == "Inspect"
	]
	view_arg: tuple[str, ...] = contract_tokens("const ReadOnlyCommentView &")
	prior_arg: tuple[str, ...] = contract_tokens("const CommentStructure *")
	if (
		len(inspections) != 1 or inspections[0][1] not in ((view_arg,), (view_arg, prior_arg)) or
		inspections[0][3] != ("static", "CommentStructure")
	):
		found.append(f"{path.relative_to(ROOT)}: Inspect が読取専用の見方だけを受ける判定の入口でない")
	# 投影・同期・配置・検証・静的解析は同じ判定を共有する（投影は `Inspect` が読むグループの所属に要る間隙を同じ判定で照合する）
	# 配置は確定済の配置対象を読み，本文構造を使う検証・静的解析は共有した構造を読む
	# 各工程に依る共通判定の使用確認
	for stage, shared in (
		("SourceProjector", "CommentNormalizer::IsCommentGap("),
		("DocCommentSynchronizer", "CommentNormalizer::Inspect("),
		("CommentRepository", "GetOutputStructure("),
		("CommentPlacer", "GetPlacementTargets("),
		("SyntaxValidator", "GetStructure("),
		("LintAnalyzer", "GetOutputStructure(")
	):
		if not any(
			contains_contract(cpp_code(src.read_text(encoding="utf-8")), shared) for src in sources() if src.stem.startswith(stage)
		):
			name: str = shared.rstrip("(")
			found.append(f"{stage} が使用するコメントの判定を {name} で共有して居ない")
	# 静的解析は同期工程でなく読取専用のドキュメント判定へ依存する
	for path in sorted((SRC / "Lint").glob("*")):
		if path.suffix not in (".cpp", ".hpp"):
			continue
		text: str = path.read_text(encoding="utf-8")
		if re.search(r"DocCommentSynchronizer(?:::|\.hpp|\s*;)", text):
			found.append(f"{path.relative_to(ROOT)}: 静的解析がドキュメント同期の操作又は取込へ依存して居る")
	# 出力事実は記録が持ち，読む工程が評価・複写・更新を行わない
	repository_header: str = cpp_code((SRC / "Core/CommentRepository.hpp").read_text(encoding="utf-8"))
	repository_src: str = cpp_code((SRC / "Core/CommentRepository.cpp").read_text(encoding="utf-8"))
	if "mutable" in contract_tokens(repository_header) or "mutable" in contract_tokens(repository_src):
		found.append("CommentRepository が読取時に変更可能な状態を持って居る")
	for stage in "CommentPlacer", "SyntaxValidator":
		reader: str = "\n".join(src.read_text(encoding="utf-8") for src in sources() if src.stem.startswith(stage))
		if (
			contains_contract(cpp_code(reader), "DocCommentReader::GetDocTargets(") or
			contains_contract(cpp_code(reader), "DocCommentReader::PlacedTargets(")
		):
			found.append(f"{stage} が説明対象を再評価して居る")
	# 違反の一覧の返戻
	return found

def canonicalizer_spelling_violations() -> list[str]:
	"""正準化の共通工程で，投影済の役割を使わず語の綴りを判定する経路を列挙する
	生成する語の取得と，トークンの値・字の融合を保つ判定は対象に含めない
	計算量：対象の総文字数 L・ファイル数 F に対して O(L+F log F)
	戻値：
		違反する語の照合又は旧い綴り表の位置の一覧
	送出例外：
		OSError: 対象ソースを読めない場合
		UnicodeDecodeError: ソースを指定の文字コードで解釈出来ない場合
	"""
	found: list[str] = []
	pattern: re.Pattern[str] = re.compile(r"\b(?:IsWord|HasWordChild|IsSpelling)\s*\(|\b(?:TailOperatorWords|HeadBoundaryWords)\b")
	paths: list[Path] = list((SRC / "Core").glob("Canonicalizer*.cpp")) + list((SRC / "Core").glob("Canonicalizer*.hpp"))
	for path in sorted(paths):
		code: str = cpp_code(path.read_text(encoding="utf-8"))
		line: int = 1
		previous: int = 0
		for match in pattern.finditer(code):
			line += code.count("\n", previous, match.start())
			previous = match.start()
			found.append(f"{path.relative_to(ROOT)}:{line}: 語の綴りを再照合して居る")
	# 違反する位置の返戻
	return found

def await_operand_end(tokens: list[CppToken], position: int, pairs: dict[int, int]) -> int:
	"""単項の被演算子と後置の連鎖を辿り，別の二項演算の前で止める
	計算量：対象トークン数 N に対して O(N)
	引数：tokens: 有意トークン，position: 被演算子の先頭，pairs: 開閉の対応と確認したテンプレート実引数の対応の格納先
	戻値：被演算子の直後のトークン位置
	送出例外：ValueError: 被演算子又は其の開閉の対応が欠ける場合
	"""
	while position < len(tokens) and tokens[position].text in ("!", "&", "*", "+", "-", "co_await", "~"):
		position += 1
	if position >= len(tokens):
		raise ValueError("await operand is missing")
	if tokens[position].text == "[" and position in pairs:
		# ラムダの捕捉・見出・本体は１つの主式で，本文の文終端は外へ伝えない
		position = pairs[position] + 1
		while position < len(tokens) and tokens[position].text != "{":
			if tokens[position].text in ("(", "[") and position in pairs:
				position = pairs[position] + 1
			else:
				position += 1
		if position not in pairs:
			raise ValueError("await lambda body is missing")
		position = pairs[position] + 1
	elif tokens[position].text == "(":
		if position not in pairs:
			raise ValueError("await operand delimiter is missing")
		position = pairs[position] + 1
	else:
		position += 1
	while position < len(tokens):
		value: str = tokens[position].text
		if value in ("(", "["):
			if position not in pairs:
				raise ValueError("await postfix delimiter is missing")
			position = pairs[position] + 1
		elif value == "<":
			cursor: int = position + 1
			depth: int = 1
			while cursor < len(tokens) and depth:
				if tokens[cursor].text in (";", "}"):
					break
				if tokens[cursor].text in ("(", "[", "{") and cursor in pairs:
					cursor = pairs[cursor] + 1
					continue
				depth += (tokens[cursor].text == "<") - (tokens[cursor].text == ">")
				cursor += 1
			if depth or cursor >= len(tokens) or tokens[cursor].text not in ("(", ".", ":", "["):
				break
			pairs[position] = cursor - 1
			position = cursor
		elif value == ".":
			position += 1
			if position < len(tokens) and tokens[position].text == "template":
				position += 1
			position += 1
		elif position + 1 < len(tokens) and (value, tokens[position + 1].text) in (("-", ">"), (":", ":")):
			position += 2
			if position < len(tokens) and tokens[position].text == "template":
				position += 1
			position += 1
		else:
			break
	# 被演算子の範囲の返戻
	return min(position, len(tokens))

def conditional_await_offsets(code: str) -> list[int]:
	"""条件演算子・論理演算子と同じ式に置いた co_await の位置を返す
	GCC は其の式の co_await を選ばない側・短絡した側でも評価し，組立を重ねる
	制御文の条件の外の本文の co_await は，条件の演算子と別の式として扱う
	co_await の被演算子の実引数の中の演算子は，待機より先に評価される為に対象外とする
	計算量：テキストの文字数 L・トークン数 N・待機の被演算子の読取数 Q に対して O(L+N+Q log(N+1))
	引数：
		code: コメントとリテラルを除いたテキスト
	戻値：
		違反する co_await の位置の一覧
	送出例外：
		ValueError: 被演算子の本体又は閉括弧を確認出来ない場合
	"""
	found: list[int] = []
	# 原文位置を保ち，検査に使う演算子だけを共通の記号へ対応付ける
	alternatives: dict[str, str] = {
		"and": "&&",
		"and_eq": "&=",
		"bitand": "&",
		"bitor": "|",
		"compl": "~",
		"not": "!",
		"not_eq": "!=",
		"or": "||",
		"or_eq": "|=",
		"xor": "^",
		"xor_eq": "^="
	}
	tokens: list[CppToken] = [
		CppToken("punct", token.start, token.end, alternatives[token.text]) if
		token.kind == "identifier" and token.text in alternatives else
		token for token in cpp_tokens(code) if token.kind not in ("comment", "space")
	]
	pairs: dict[int, int] = {}
	stack: list[int] = []
	for idx, token in enumerate(tokens):
		if token.text in ("(", "[", "{"):
			stack.append(idx)
		elif token.text in (")", "]", "}") and stack:
			opening: int = stack.pop()
			if (tokens[opening].text, token.text) in (("(", ")"), ("[", "]"), ("{", "}")):
				pairs[opening] = idx
	closing: dict[int, int] = { end: start for start, end in pairs.items() }
	controls: set[int] = {
		idx for idx in pairs if tokens[idx].text == "(" and idx and (
			tokens[idx - 1].text in ("for", "if", "switch", "while") or
			idx > 1 and tokens[idx - 1].text == "constexpr" and tokens[idx - 2].text == "if"
		)
	}
	# ラムダ・requires の本体は，外側の式の主式の一部である
	expression_braces: set[int] = set()
	for opening in pairs:
		if tokens[opening].text != "{" or not opening:
			continue
		prior: int = opening - 1
		while prior and tokens[prior].text in ("constexpr", "mutable", "noexcept"):
			prior -= 1
		if tokens[prior].text == ")" and prior in closing:
			prior = closing[prior] - 1
		if prior > -1 and tokens[prior].text in ("]", "requires"):
			expression_braces.add(opening)
	# 前後の式境界を全トークンで１度求め，待機毎に文を走査し直さない
	left_edges: list[int] = [0] * (len(tokens) + 1)
	for idx in range(1, len(tokens) + 1):
		prior = idx - 1
		value: str = tokens[prior].text
		if value in (";", "{") or prior in controls:
			left_edges[idx] = idx
		elif prior in closing:
			opening = closing[prior]
			left_edges[idx] = idx if opening in controls or value == "}" and opening not in expression_braces else left_edges[opening]
		else:
			left_edges[idx] = left_edges[prior]
	right_edges: list[int] = [len(tokens)] * (len(tokens) + 1)
	for idx in reversed(range(len(tokens))):
		value = tokens[idx].text
		if value in (";", "}") or idx in closing and closing[idx] in controls:
			right_edges[idx] = idx
		elif idx in pairs:
			right_edges[idx] = right_edges[pairs[idx] + 1]
		else:
			right_edges[idx] = right_edges[idx + 1]
	# 演算子の位置を本体毎に持ち，入子の本体の演算子を外側の式へ含めない
	operators: dict[int, list[int]] = { -1: [] }
	owners: list[int] = []
	bodies: list[int] = [-1]
	for idx, token in enumerate(tokens):
		owners.append(bodies[-1])
		if token.text == "{":
			bodies.append(idx)
			operators[idx] = []
		elif token.text == "}" and len(bodies) > 1:
			bodies.pop()
		elif (
			token.text in ("&&", "?", "||") or idx + 1 < len(tokens) and
			(token.text == tokens[idx + 1].text and token.text in ("&", "|") and token.end == tokens[idx + 1].start)
		):
			operators[bodies[-1]].append(idx)
	for position, token in enumerate(tokens):
		if token.text != "co_await":
			continue
		left: int = left_edges[position]
		right: int = right_edges[position + 1]
		start: int = position + 1
		after: int = await_operand_end(tokens, start, pairs)
		# 待機する呼出の実引数・テンプレート実引数だけを外側の演算から除く
		excluded: dict[int, int] = {}
		idx: int = start
		while idx < after:
			if tokens[idx].text in ("(", "<") and idx in pairs and idx > start and (
				tokens[idx - 1].kind == "identifier" and
				tokens[idx - 1].text not in ("alignof", "co_await", "decltype", "noexcept", "sizeof") or
				tokens[idx - 1].text in (")", ">", "]", "}")
			):
				excluded[idx] = pairs[idx]
				idx = pairs[idx] + 1
			else:
				idx += 1
		positions: list[int] = operators[owners[position]]
		count: int = bisect_left(positions, right) - bisect_left(positions, left)
		for opening, end in excluded.items():
			count -= bisect_right(positions, end) - bisect_left(positions, opening)
		if count:
			found.append(token.start)
	# 違反する co_await の位置の返戻
	return found

def conditional_await_self_check() -> list[str]:
	"""条件式の中の co_await の判定を，手で定めた正誤の組で照合する
	計算量：固定の入力の総文字数 L に対して O(L)
	戻値：
		期待と異なる入力の一覧
	"""
	cases: tuple[tuple[str, int], ...] = (
		("Built Piece = IsNamed ? co_await Take(State, Child) : Leaf(Child);", 1),
		("Built Piece = Open ?\nco_await Sealed(State, Child) :\nco_await Take(State, Child);", 2),
		("Expand = Expand || Named(Child) && (co_await Get(State, Child))->HasBreak;", 1),
		("if(A && (co_await Get(State, Child))->HasBreak) return;", 1),
		("if(IsNamed) Piece = co_await Take(State, Child);\nelse Piece = Leaf(Child);", 0),
		("} else if(A && B) Piece = co_await Take(State, Child);", 0),
		("if(Cond(Id) && !Has(Id)) co_return co_await Flat(State, Id);", 0),
		("if(!Expand) Expand = (co_await Get(State, Child))->HasBreak;", 0),
		("if(const auto Shape = Get(Id); Shape && Can(*Shape)) co_return co_await Split(State, Id, *Shape);", 0),
		("co_return co_await Generic(State, Id, A || B, C ? D : E);", 0),
		("bool Result = co_await Get() && Check();", 1),
		("bool Result = co_await (Task) || Check();", 1),
		("bool Result = Ready && co_await Task;", 1),
		("bool Result = co_await Task || Ready;", 1),
		("bool Result = co_await (Ready ? First() : Second());", 1),
		("bool Result = co_await Get(Ready && Check());", 0),
		("bool Result = co_await Owner.Get(Ready ? Good() : Bad());", 0),
		("bool Result = co_await (Get(Ready || Check()));", 0),
		("bool Result = co_await (*Ptr)(Ready && Check());", 0),
		("bool Result = co_await Tasks[Choose(Ready ? Good() : Bad())];", 0),
		("bool Result = co_await Get<Task>(Ready && Check());", 0),
		("bool Result = co_await Owner.template Get<X<Y>>(Ready ? Good() : Bad());", 0),
		("bool Result = co_await Get<(Ready ? 1 : 2)>();", 0),
		("bool Result = co_await Get<Task>() && Check();", 1),
		("bool Result = co_await Task < Other && Check();", 1)
	)
	# 入子の本体と，制御見出の別々の式を区別する
	cases += (
		("if(auto X = co_await Get(); X && Ready) Use(X);", 0),
		("switch(auto X = co_await Get(); X && Ready) { default: break; }", 0),
		("for(auto X = co_await Get(); X && Ready; ++X) Use(X);", 0),
		("for(auto X = Ready && Check(); X; X = co_await Get()) Use(X);", 0),
		("for(auto X = co_await Get(); X; X = Ready && Check()) Use(X);", 0),
		("if(auto X = Ready && Check(); co_await Task) Use(X);", 0),
		("for(; co_await Task && Ready; ++X) Use(X);", 1),
		("for(; Ready; X = co_await Task || Check()) Use(X);", 1),
		("if(auto X = co_await Get() && Ready; X) Use(X);", 1),
		("bool X = co_await Get([]{ return Ready && Check(); });", 0),
		("bool X = Make([]{ return Ready && Check(); }) + co_await Task;", 0),
		("bool X = Ready && co_await Get([]{ return 1; });", 1),
		("auto F = []{ return co_await Task && Ready; };", 1),
		("bool X = Ready && []{ return co_await Task; }();", 0),
		("bool X = Ready && []{ return 1; }() + co_await Task;", 1),
		("bool X = Ready && [](int Y){ return Y; }(1) + co_await Task;", 1),
		("if constexpr(auto X = co_await Get(); X && Ready) Use(X);", 0)
	)
	# 被演算子と後続の演算及び括弧の有無を，指摘の順で照合する
	probes: list[tuple[str, int]] = list(cases)
	for operator in "and", "or":
		for expression in (
			"Ready OP co_await Task",
			"co_await Task OP Ready",
			"if(Ready OP co_await Task) Use();",
			"co_return Ready OP co_await Task"
		):
			probes.append((expression.replace("OP", operator) + ";", 1))
	for prefix in "not", "compl", "bitand":
		for argument in "Ready && Check()", "Ready || Check()", "Ready ? Good() : Bad()":
			probes.append(("bool X = co_await " + prefix + " Get(" + argument + ");", 0))
	for operand in "Task", "Owner.Task", "*Ptr", "Tasks[Idx]":
		for suffix in "&& Check()", "|| Check()", "? Good() : Bad()":
			for is_wrapped in False, True:
				awaited: str = "co_await " + operand
				if is_wrapped:
					awaited = "(" + awaited + ")"
				probes.append(("bool Result = " + awaited + " " + suffix + ";", 1))
	for operand in (
		"Get([]{ return 1; })",
		"Get([]{ int X = 1; return X; })",
		"Get([Value]{ return Value; })",
		"[]{ int X = 1; return Get(X); }()",
		"Get<Task<requires { typename T::value_type; }>>()"
	):
		for suffix in "&& Ready", "|| Ready", "? Good() : Bad()":
			probes.append(("bool X = co_await " + operand + " " + suffix + ";", 1))
		probes.append(("bool X = co_await " + operand + ";", 0))
	# 期待と異なる入力の返戻
	return [
		code for code, expected in probes if conditional_await_offsets(code) !=
		[match.start() for match in re.finditer("co_await", code)][:expected]
	]

def coroutine_condition_violations() -> list[str]:
	"""実装の全てのコルーチンで，条件演算子・論理演算子と同じ式に置いた co_await を列挙する
	計算量：対象ファイル数 F・総文字数 L・待機の被演算子の読取数 Q に対して O(F log F+L+Q log(L+1))
	戻値：
		違反の位置の一覧
	送出例外：
		OSError: 対象ソースを読めない場合
		UnicodeDecodeError: 読み込んだファイルを指定の文字コードで解釈出来ない場合
	"""
	found: list[str] = []
	for path in sources():
		code: str = cpp_code(path.read_text(encoding="utf-8"))
		try:
			offsets: list[int] = conditional_await_offsets(code)
		except ValueError as e:
			found.append(f"{path.relative_to(ROOT)}: 条件の範囲を確認出来ない: {e}")
			continue
		where: Path = path.relative_to(ROOT)
		found += [f"{where}:{code.count(chr(10), 0, offset) + 1}: 条件式の co_await（GCC は選ばない側も評価する）" for offset in offsets]
	# 違反の位置の返戻
	return found

def reference_fact_sources(root: Path) -> list[str]:
	"""基準側の分岐を持つソースと，別に翻訳する CMake の一覧を照合する
	計算量：総ソース長 L・ファイル数 F に対して O(L + F log F)
	引数：root: ソースと CMake の登録を持つ作業木
	戻値：未登録・不要な登録・重複又は確定不能な一覧の説明
	送出例外：OSError: ソース又は CMake の登録を読めない場合
		UnicodeDecodeError: ファイルを UTF-8 として読めない場合
	"""
	text: str = re.sub(r"(?m)#[^\n]*", "", (root / "CMakeLists.txt").read_text(encoding="utf-8"))
	lists: list[str] = re.findall(r"\bset\s*\(\s*SHAVEFMT_FACT_SOURCES\b([^)]*)\)", text)
	if len(lists) != 1:
		# 一覧を確定出来ない登録の拒否の返戻
		return ["SHAVEFMT_FACT_SOURCES の定義が１つでない"]
	registered: list[str] = [token.strip("\"") for token in re.split(r"[\s;]+", lists[0].strip()) if token]
	actual: set[str] = {
		path.relative_to(root).as_posix() for path in sorted((root / "src").rglob("*")) if path.suffix in (".cpp", ".hpp") and
		re.search(r"\bShaveFormatRefFacts\b", cpp_code(path.read_text(encoding="utf-8")))
	}
	found: list[str] = []
	if len(registered) != len(set(registered)):
		found.append("SHAVEFMT_FACT_SOURCES に同じ翻訳単位が重複して居る")
	found.extend("基準側の分岐の翻訳が未登録: " + path for path in sorted(actual - set(registered)))
	found.extend("基準側の分岐を持たない翻訳単位が登録されて居る: " + path for path in sorted(set(registered) - actual))
	# 両側の独立した読取の不一致の返戻
	return found

def reference_sources_self_check() -> list[str]:
	"""翻訳対象の欠落・不要な追加・重複と，コメント・文字列の対照を照合する
	計算量：固定したテストケースの総ソース長 L に対して O(L)
	戻値：手記述した正誤と異なる入力の一覧
	送出例外：OSError: 一時作業木又は登録・ソースを準備出来ない場合
	"""
	found: list[str] = []
	with TemporaryDirectory(prefix="shavefmt-reference-sources-") as directory:
		root: Path = Path(directory)
		(root / "src").mkdir()
		(root / "src/Fact.cpp").write_text("#if defined(ShaveFormatRefFacts)\nint x;\n#endif\n", encoding="utf-8")
		(root / "src/Common.cpp").write_text("// ShaveFormatRefFacts\nconst char *x=\"ShaveFormatRefFacts\";\n", encoding="utf-8")
		for declaration, should_match in (
			("set(SHAVEFMT_FACT_SOURCES src/Fact.cpp)", True),
			("set(SHAVEFMT_FACT_SOURCES \"src/Fact.cpp\")", True),
			("set(SHAVEFMT_FACT_SOURCES)", False),
			("set(SHAVEFMT_FACT_SOURCES src/Common.cpp)", False),
			("set(SHAVEFMT_FACT_SOURCES src/Fact.cpp src/Common.cpp)", False),
			("set(SHAVEFMT_FACT_SOURCES src/Fact.cpp src/Fact.cpp)", False),
			("# set(SHAVEFMT_FACT_SOURCES src/Fact.cpp)", False),
			("set(SHAVEFMT_FACT_SOURCES src/Fact.cpp)\nset(SHAVEFMT_FACT_SOURCES src/Fact.cpp)", False)
		):
			(root / "CMakeLists.txt").write_text(declaration, encoding="utf-8")
			if bool(reference_fact_sources(root)) == should_match:
				found.append("基準側の翻訳対象の判定が違う: " + declaration)
	# 読取と集合の照合の結果の返戻
	return found

def main() -> int:
	"""設計の不変条件を検査し，違反が有れば非零で終える
	計算量：各検査の実行時間の和 T に対して O(T)
	戻値：
		成功なら０，違反又は失敗が有れば非零の終了値
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		UnicodeDecodeError: 読み込んだファイルを指定の文字コードで解釈出来ない場合
	"""
	# 不変条件の検査と違反の集計
	checks: tuple[tuple[str, list[str]], ...] = (
		("C++ のトークンと所有者の読取", cpp_lex_self_check() + owner_self_check()),
		("子の経路の自己検査", child_route_self_check()),
		("基準側の翻訳対象の自己検査", reference_sources_self_check()),
		("基準側の翻訳対象", reference_fact_sources(ROOT)),
		("条件式の co_await の自己検査", conditional_await_self_check()),
		("条件式の co_await", coroutine_condition_violations()),
		("正準化の語の役割", canonicalizer_spelling_violations()),
		("行幅の所有", width_owner_violations()),
		("コメントの本文の遮断", comment_text_violations()),
		("コメントの経路の読取専用", comment_path_mutation_violations()),
		("配置計画の権限", placement_plan_violations()),
		("出力の構築経路", construction_violations()),
		("空行の決定とコメントの分離", layout_comment_violations()),
		("所有領域のコメントの分離", owned_region_violations()),
		("本文構造の共通判定", comment_structure_violations())
	)
	total: int = 0
	for name, failures in checks:
		total += len(failures)
		for failure in failures:
			print(f"FAIL design invariant [{name}]: {failure}", file=sys.stderr)
	print(f"design invariants: {len(checks)} checks, {total} violations")
	# 違反が有れば非零の返戻
	return 1 if total else 0

if __name__ == "__main__":
	sys.exit(main())
