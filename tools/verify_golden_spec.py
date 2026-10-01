#! /usr/bin/env python3
"""期待結果其の物を仕様の条文と照合する検査

`golden_test.sh` は `.in` を整形して `.out` と比べる為，`.out` 自体が条文に適うかは見ない
期待結果が実装の誤りを写して居ると検査は緑の儘に為る為，此処では**整形器を動かさず**
`.out` だけを読んで条文との食違いを探す
照合する条文は，空の本体（整形仕様 6.3）と行頭の閉じ括弧の後の閉じ括弧（整形仕様 6.3）の２つだけで，其の他の条文への適合は此の検査では確かめない
"""

import glob
import os

from pathlib import Path

import re
import sys

# 行を閉じる字句（整形仕様 6.3 の空の本体の検査で，見出の次に来たら本体が空）
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

def indent_of(line: str) -> int:
	"""行頭のタブ数を返す
	計算量：行の文字数 L に対して O(L)
	"""
	# タブ数の返戻
	return len(line) - len(line.lstrip("\t"))

def is_comment(line: str) -> bool:
	"""行の全体がコメントかを返す
	計算量：行の文字数 L に対して O(L)
	"""
	text: str = line.strip()
	if text.startswith(("#", "//")):
		# 行コメントの返戻
		return True
	if text.startswith("/*"):
		# 閉じで終わる囲みコメントだけを行コメントと同じに扱う事の返戻
		return text.endswith("*/") and text.index("*/") == len(text) - 2
	if text.startswith("*"):
		# 囲みコメントの継続行の返戻（コードの `*p = 1;` を除く）
		return not text.rstrip().endswith((")", ",", ";", "{", "}"))
	# HTML のコメントの返戻
	return text.startswith("<!--")

def check_empty_body(path: str, lines: list[str], width: int) -> list[str]:
	"""整形仕様 6.3 の「空の本体は畳む」への違反を返す
	畳んだ形が行幅（生成するインデントを数えない，行分割仕様 3.2）を超える物は，行幅に依る選択（整形仕様 6.4）で折れる為に除く
	計算量：行数 L・見出の語数 H に対して O(LH)
	"""
	# 見出と直後の閉じ字句の探索
	found: list[str] = []
	for index, line in enumerate(lines):
		head: str = line.strip()
		if not head or is_comment(line):
			continue
		if not (head.endswith(("do", "then", "{")) or any(head.startswith(h) for h in HEADS) and not head.endswith(("end", "}"))):
			continue
		# 空白とコメントだけを飛ばした次の行
		next_index: int = index + 1
		while next_index < len(lines) and (not lines[next_index].strip() or is_comment(lines[next_index])):
			next_index += 1
		if next_index >= len(lines):
			continue
		close: str = lines[next_index].strip()
		if indent_of(lines[next_index]) != indent_of(line) or close not in CLOSERS:
			continue
		# `;` が要る為に畳めない形と，畳んだ形が行幅を超える形は除く
		if any(pattern.match(head) for pattern in NO_FOLD) or width and len(head) + 1 + len(close) > width:
			continue
		found.append(f"{path}:{index + 1}: spec 6.3 empty body is not folded: {head} … {close}")
	# 違反の一覧の返戻
	return found

def check_closer_run(path: str, lines: list[str]) -> list[str]:
	"""整形仕様 6.3 の「展開した閉じ括弧の後続」への違反を返す（Go は対象外）
	計算量：行数 L に対して O(L)
	"""
	if "/go/" in path:
		# 自動セミコロン挿入を避ける言語の除外の返戻
		return []
	found: list[str] = []
	for index, line in enumerate(lines):
		text: str = line.strip()
		# 行頭が閉じ括弧で，後に別の閉じ括弧が続く形だけを見る
		if len(text) > 1 and text[0] in ")]}" and text[1] in ")]}":
			found.append(f"{path}:{index + 1}: spec 6.3 a closer follows a closer on the same line: {text}")
	# 違反の一覧の返戻
	return found

def main() -> int:
	"""期待結果を条文と照合し，違反が有れば非零で終える
	計算量：対象ファイル数 F・総行数 L に対して O(F log F+L)
	"""
	# 起動した場所に依らず作業木の根から検体を探す
	os.chdir(Path(__file__).resolve().parent.parent)
	# 期待出力の収集と条文違反の照合
	violations: list[str] = []
	total: int = 0
	# 言語の判定を経路の区切りに依らせない様に，Windows の `\` も `/` へ揃える
	for path in sorted(found.replace("\\", "/") for found in glob.glob("test_data/golden/*/*.out")):
		# 見送りの検体は原文返戻で，整形の規則を適用しない
		if glob.glob(path[:-4] + ".skip") or glob.glob(path[:-4] + ".partial"):
			continue
		lines: list[str] = open(path, encoding="utf-8").read().split("\n")
		total += 1
		# 検体毎の行幅（`.chars` が無ければ既定の `128`，`0` は無制限）
		width: int = int(open(path[:-4] + ".chars").read()) if glob.glob(path[:-4] + ".chars") else 128
		violations += check_empty_body(path, lines, width)
		violations += check_closer_run(path, lines)
	# 違反と検査件数の報告
	for violation in violations:
		print(f"FAIL golden spec: {violation}", file=sys.stderr)
	print(f"golden spec: {total} expected outputs checked against 2 rules (empty body, closer run), {len(violations)} violations")
	# 違反が有れば非零の返戻
	return 1 if violations else 0

if __name__ == "__main__":
	sys.exit(main())
