#! /usr/bin/env python3
"""配布物に静的に含む第三者のライセンス原文を１つの通知へ纏める

使方：python tools/generate_third_party_notices.py <リポジトリ> <tree-sitter のソース> <保存先> [--gcc-licenses <ディレクトリ>]
原本の欠落は失敗とし，通知を黙って省略しない
"""

import argparse

from pathlib import Path

import sys

# 静的に結合する文法の言語順（TypeScript の２文法は同じ原本を使う）
GRAMMARS: tuple[str, ...] = (
	"c",
	"cpp",
	"c-sharp",
	"java",
	"go",
	"rust",
	"kotlin",
	"swift",
	"php",
	"javascript",
	"typescript",
	"ruby",
	"python",
	"json",
	"html",
	"scss"
)

def notice(name: str, path: Path) -> bytes:
	"""原本を変更せず，部品名だけを前置した通知を作る
	引数：name: 部品名，path: ライセンス原文
	戻値：見出とライセンスの原本
	送出例外：OSError / ValueError: 原本が読めない，又は空の場合
	"""
	data: bytes = path.read_bytes()
	if not data.strip():
		raise ValueError(f"empty third-party license: {path}")
	# 原本の通知の返戻
	return name.encode("utf-8") + b"\n" + b"=" * 72 + b"\n" + data + b"\n\n"

def main() -> int:
	"""実行時・全ての文法・Unicode の通知を保存する
	戻値：保存の成功時は０
	送出例外：OSError / ValueError: 原本の読込又は保存の失敗
	"""
	parser: argparse.ArgumentParser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("repository", type=Path)
	parser.add_argument("runtime", type=Path)
	parser.add_argument("output", type=Path)
	parser.add_argument("--gcc-licenses", type=Path)
	args: argparse.Namespace = parser.parse_args()
	# 実際に構築へ使う実行時・文法の原本の取込
	parts: list[bytes] = [
		notice("tree-sitter runtime", args.runtime / "LICENSE"),
		notice("tree-sitter runtime: ICU (lib/src/unicode)", args.runtime / "lib/src/unicode/LICENSE")
	]
	for grammar in GRAMMARS:
		parts.append(notice("tree-sitter-" + grammar, args.repository / "vendor" / ("tree-sitter-" + grammar) / "LICENSE"))
	parts.append(
		notice(
			"Unicode 15.1.0 Character Database (normalization and alphanumeric tables)",
			Path(__file__).with_name("licenses") / "unicode.LICENSE"
		)
	)
	# Linux で静的に結合する GCC の実行時の許諾と例外
	if args.gcc_licenses:
		for name in "COPYING3", "COPYING.RUNTIME":
			parts.append(notice("GCC runtime: " + name, args.gcc_licenses / name))
	args.output.parent.mkdir(parents=True, exist_ok=True)
	args.output.write_bytes(b"".join(parts))
	# 保存の成功の返戻
	return 0

if __name__ == "__main__":
	sys.exit(main())
