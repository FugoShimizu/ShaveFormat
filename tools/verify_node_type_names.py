#!/usr/bin/env python3
"""名前付の規則と同じ型名を持つ無名の字句を，名前の確認無で型名比較して居る箇所を検出する

tree-sitter の文法では `decltype` (C++) や `type` (Python / TS) の様に，名前付の節点と無名の字句が同じ型名を持つ事が有る
走査は無名の字句も訪れる為，型名だけで見分けて子を引くと，子を持たない字句から空節点が返り異常終了する
型名を直に比べる形と，一度変数へ束ねてから比べる形の双方を見る
"""

import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# 文法の最少数（副モジュールの取得漏れや `tree-sitter generate` の未実行で検査が空振りするのを防ぐ）
GRAMMAR_FLOOR = 16
ALIAS = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*(?:=|\()\s*(NamedTypeOf|ts_node_type)\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)")

def collision_names() -> tuple[set[str], int]:
	"""文法毎に「名前付と無名の双方に現れる型名」を集める

	`node-types.json` は子の型として現れた無名の字句しか載せず葉の字句を取零す為，文法の記号表から採る
	計算量：文法の総バイト数 B に対し O(B)
	戻値：
		衝突する型名と検査した文法数
	送出例外：
		SystemExit: 記号表の項目数と取得数が一致しない場合
	"""
	names, grammars = set(), 0
	for path in glob.glob(os.path.join(ROOT, "vendor/tree-sitter-*/**/parser.c"), recursive=True):
		with open(path, encoding="utf-8", errors="replace") as source:
			text = source.read()
		symbols = re.search(r"ts_symbol_names\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
		metadata = re.search(r"ts_symbol_metadata\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
		if not symbols or not metadata:
			continue
		entries = re.findall(r"\[(\w+)\]\s*=\s*\"((?:[^\"\\]|\\.)*)\"", symbols.group(1))
		# 逃避を含む字句を取零すと後続の項目まで呑み込む為，記号の総数と読めた数の一致を確かめる
		if len(entries) != len(re.findall(r"^\s*\[\w+\]\s*=", symbols.group(1), re.M)):
			raise SystemExit("記号表を読み落とした：%s" % os.path.relpath(path, ROOT))
		table = dict(entries)
		named, anonymous = set(), set()
		for key, body in re.findall(r"\[(\w+)\]\s*=\s*\{(.*?)\}", metadata.group(1), re.S):
			name = table.get(key)
			if name is None or ".visible = true" not in body:
				continue
			(named if ".named = true" in body else anonymous).add(name)
		names |= named & anonymous
		grammars += 1
	# 衝突する型名の集合と，記号表を読めた文法の数の返戻
	return names, grammars

def risky_lines(names: set[str]) -> list[tuple[str, int, str, str]]:
	"""衝突する型名で見分けた節点の子を，名前の確認無で引いて居る箇所を集める

	無名の字句は子を持たない為，型名だけで見分けて子を引くと空節点が返る
	型名を見るだけで子を引かない箇所（Ruby の節の語の様に字句も含めて拾う設計）は危険でない
	計算量：原文の行数 L と衝突する型名の数 T に対し O(L * T)
	引数：
		names: 衝突する型名
	戻値：
		危険な比較の位置と内容
	"""
	rows: list[tuple[str, int, str, str]] = []
	# 型名を直に比べる形と，一度変数へ束ねてから比べる形（`std::string_view` で包む書き方も含む）
	patterns: list[tuple[str, re.Pattern[str], bool]] = []
	for name in sorted(names):
		quoted = re.escape(name)
		patterns.append((name, re.compile(r'ts_node_type\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\)?\s*[=!]=\s*"%s"' % quoted), True))
		patterns.append((name, re.compile(r'\b([A-Za-z_][A-Za-z0-9_]*)\s*\)?\s*[=!]=\s*"%s"' % quoted), False))
	for path in (
		glob.glob(os.path.join(ROOT, "src/**/*.cpp"), recursive=True) + glob.glob(os.path.join(ROOT, "src/**/*.hpp"), recursive=True)
	):
		with open(path, encoding="utf-8") as source:
			lines = source.read().split("\n")
		# 型名を束ねた変数から元の節点を引く表（名前を確かめた束ねは危険で無い印として空にする）
		alias: dict[str, str] = {}
		for index, line in enumerate(lines):
			alias.update({ target: "" if source == "NamedTypeOf" else node for target, source, node in ALIAS.findall(line) })
			if "ts_node_is_named" in line or "NamedTypeOf" in line:
				continue
			for name, pattern, is_direct in patterns:
				match = pattern.search(line)
				if not match:
					continue
				# 比べた型名の出所の節点を求める（直に `ts_node_type(X)` を比べた形は其の儘，束ねた形は表から引く）
				target = match.group(1) if is_direct else alias.get(match.group(1), "")
				if not target:
					continue
				# 型名が一致した枝の範囲を採る（不一致で抜ける見張りなら後続の行，波括弧が続けば枝の中，其れ以外は其の行）
				tail = line[match.end():]
				region = (
					"\n".join(lines[index + 1:index + 7]) if
					"!=" in match.group(0) and re.search(r"\b(?:return|continue|break|goto)\b", tail) else
					"\n".join(lines[index:index + 6]) if "{" in tail else "\n".join(lines[index:index + 2]) if tail.rstrip().endswith(")") else tail
				)
				# 一致した節点の子を其の枝で引いて居るかを見る（引かなければ空節点は生まれない）
				if (child := re.compile(r"ts_node_(?:named_)?child\(\s*%s\s*," % re.escape(target))).search(region):
					rows.append((os.path.relpath(path, ROOT), index + 1, name, line.strip()[:100]))
	# 危険な比較の一覧の返戻
	return rows

def main() -> int:
	"""無名の字句と型名が衝突する危険な比較を検査する
	戻値：
		終了コード
	"""
	names, grammars = collision_names()
	if grammars < GRAMMAR_FLOOR:
		print("文法の記号表を %d 個しか読めなかった（%d 個以上が要る．副モジュールの取得漏れ）" % (grammars, GRAMMAR_FLOOR))
		# 文法不足の失敗返戻
		return 1
	rows = risky_lines(names)
	print("文法 %d 個 ／ 衝突する型名 %d 語 ／ 名前の確認が無い型名比較 %d 件" % (grammars, len(names), len(rows)))
	for path, number, name, text in rows:
		print("  %s:%d [%s] %s" % (path, number, name, text))
	# 検出が有れば失敗として終える（走査が無名の字句を渡す為，子を引くと落ちる）
	return 1 if rows else 0

if __name__ == "__main__":
	sys.exit(main())
