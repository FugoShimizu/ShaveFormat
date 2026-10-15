#!/usr/bin/env bash
# ドキュメントが引用する型とメンバの宣言の検査
set -u;
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)";
python3 -B - "${ROOT}" "$@" <<'PYTHON'
from pathlib import Path

import re
import sys

sys.path.insert(0, str(Path(sys.argv[1]) / "tools"))

from verify_design_invariants import cpp_code, cpp_decls, cpp_fields, cpp_type_idx

def declared_symbols(codes: dict[Path, str]) -> tuple[set[str], set[str]]:
	"""コメントとリテラルを除いたソースの型・直接のメンバ宣言を得る
	計算量：総文字数 L・型数 V に対して O(L+V)
	引数：
		codes: ファイル毎のコード
	戻値：
		宣言した型名と所有型で修飾したメンバ名の集合
	送出例外：
		ValueError: 宣言の囲みを確認出来ない場合
	"""
	# 同名の型の曖昧さを残した宣言のインデックス
	bodies: dict[str, list[tuple[Path, str]]]
	aliases: dict[str, str]
	bodies, aliases, _ = cpp_type_idx(codes)
	types: set[str] = set(bodies) | set(aliases)
	members: set[str] = set()
	for code in codes.values():
		types.update(re.findall(r"\benum\s+(?:class\s+|struct\s+)?([A-Za-z_]\w*)\b", code))
	for owner, entries in bodies.items():
		if len(entries) != 1:
			continue
		body: str = entries[0][1]
		members.update(f"{owner}::{name}" for name, _ in cpp_fields(body))
		# 本体の呼出でなく，直接の関数宣言の見出だけでの照合
		for decl in cpp_decls(body):
			opening: int = -1
			depth: int = 0
			for pos, char in enumerate(decl):
				if char in "={":
					break
				depth += (char == "<") - (char == ">")
				if char == "(" and not depth:
					opening = pos
					break
			if opening < 0:
				continue
			match: re.Match[str]|None = re.search(r"\b([A-Za-z_]\w*)\s*$", decl[:opening])
			if match:
				members.add(f"{owner}::{match.group(1)}")
	# 宣言に基づく名前の返戻
	return types, members

def self_check() -> list[str]:
	"""宣言の有無を手書きの正誤対で照合する
	計算量：正誤対の数 F・最大解析費用 P に対して O(FP)
	戻値：
		欠落の見逃し又は宣言の誤拒否の一覧
	送出例外：
		ValueError: 宣言の囲みを確認出来ない場合
	"""
	# コメント・リテラル・呼出と，実際の宣言の区別
	found: list[str] = []
	for text, has_type, has_member in (
		("struct Actual {};", False, False),
		("/\\\n/ struct Missing { int Value; };", False, False),
		("struct Mis\\\nsing { int Val\\\nue; };", True, True),
		("/* struct Missing { int Value; }; */", False, False),
		("struct Missing {}; // Missing::Value", True, False),
		("struct Missing {}; const char *s = \"Missing::Value\";", True, False),
		("struct Missing {}; void f(){Missing::Value();}", True, False),
		("struct Missing { int x = Value(); };", True, False),
		("struct Missing { int x{Other::Value()}; };", True, False),
		("struct Missing { std::function<void()> x; };", True, False),
		("struct Missing { void f(){Other::Value();} };", True, False),
		("struct Missing { int Value; };", True, True),
		("struct Missing { struct Nested { int Value; }; };", True, False),
		("struct Missing { struct Nested { int Child; }; int Value; };", True, True),
		("constexpr int A=1'000;\nstruct Missing { int Value; };\nconstexpr int B=2'000;", True, True),
		("constexpr int A=1000;\nstruct Missing { int Value; };\nconstexpr int B=2000;", True, True),
		("constexpr int A=1'000; const char *s=\"struct Missing { int Value; };\";", False, False),
		("struct Missing { static void Value(); };", True, True),
		("struct Missing { static void Value() {} };", True, True)
	):
		types: set[str]
		members: set[str]
		types, members = declared_symbols({ Path("case.cpp"): cpp_code(text) })
		if ("Missing" in types) != has_type or ("Missing::Value" in members) != has_member:
			found.append(text)
	# 正誤対の食違の返戻
	return found

def main(root: Path) -> int:
	"""仕様とドキュメントの引用を実際の宣言と照合する
	計算量：総文字数 L・型数 V・引用数 Q に対して O(L+V+Q)
	引数：
		root: 作業ツリーの経路
	戻値：
		欠落又は自己検査の失敗が有れば１，其れ以外は０
	送出例外：
		OSError: ファイルを読めない場合
		UnicodeDecodeError: ソースを指定の文字コードで読めない場合
		ValueError: 宣言の囲みを確認出来ない場合
	"""
	# 実際の宣言と欠落の反例の照合
	problems: list[str] = self_check()
	if problems or "--self-check" in sys.argv[2:]:
		for problem in problems:
			print(f"FAIL doc symbol self-check: {problem}", file=sys.stderr)
		print(f"doc symbols self-check: {len(problems)} failed")
		# 自己検査の終了値の返戻
		return int(bool(problems))
	paths: list[Path] = sorted((*root.joinpath("src").rglob("*.cpp"), *root.joinpath("src").rglob("*.hpp")))
	codes: dict[Path, str] = { path: cpp_code(path.read_text(encoding="utf-8")) for path in paths }
	types: set[str]
	members: set[str]
	types, members = declared_symbols(codes)
	# 責務の表とクラス図が定める型の契約
	spec: str = root.joinpath("docs/formatter_spec.md").read_text(encoding="utf-8")
	section: str = spec.split("## 3.1 ", 1)[1].split("## 3.2 ", 1)[0]
	cited_types: set[str] = set(re.findall(r"^\| `([A-Za-z]\w*)` \|", section, re.M))
	for diagram in re.findall(r"^```mermaid\n(.*?)^```", spec, re.M | re.S):
		cited_types.update(re.findall(r"^\s*class ([A-Za-z]\w*)", diagram, re.M))
	for name in sorted(cited_types - types):
		problems.append(f"{name} は仕様上の型だが実装に宣言が無い")
	# 修飾名の引用と直接のメンバ宣言の対応
	documents: list[Path] = sorted(root.joinpath("docs").rglob("*")) + [root / "README.md"]
	cited_members: set[str] = set()
	for path in documents:
		if path.is_file():
			cited_members.update(re.findall(r"`([A-Za-z]\w*::[A-Za-z_]+)`", path.read_text(encoding="utf-8")))
	cited_members = { name for name in cited_members if not name.startswith("std::") }
	for name in sorted(cited_members - members):
		problems.append(f"{name} はドキュメントの引用だが所有型にメンバ宣言が無い")
	# 改名前の型を引用する残存コードとドキュメントの検査
	gone: str = "CommentAttach|DocSig|Edit|Layout|LayoutComment|LineSplit|Lint|ParseRepair|Postprocess|Preprocess"
	gone += "|Structure|SyntaxCheck|TsSource"
	for path in paths + [path for path in documents if path.is_file()]:
		for line, text in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
			if re.search(rf"\b({gone})::[A-Za-z_]+", text):
				problems.append(f"{path}:{line}: {text} は改名で消えた型名を引用して居る")
	for problem in problems:
		print(f"FAIL doc symbol: {problem}", file=sys.stderr)
	print(f"doc symbols: {len(cited_types) + len(cited_members)} cited, {len(problems)} missing")
	# 宣言の欠落に依る終了値の返戻
	return int(bool(problems))

sys.exit(main(Path(sys.argv[1])))
PYTHON
