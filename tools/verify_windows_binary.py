#! /usr/bin/env python3
"""dumpbin の原本から Windows 8 の PE の宣言と静的な C / C++ の実行時を照合する"""

import argparse

from pathlib import Path

import re
import sys

def verify_binary(headers: str, dependencies: str) -> None:
	"""PE の両バージョンと実行時への依存を検査する
	引数：headers: /headers の出力，dependencies: /dependents の出力
	送出例外：ValueError: 下限の宣言又は静的結合を満たさない場合
	"""
	for field in "operating system", "subsystem":
		versions: list[str] = re.findall(r"^\s*(\d+\.\d+)\s+" + field + r" version\s*$", headers, re.MULTILINE | re.IGNORECASE)
		if len(versions) != 1 or tuple(map(int, versions[0].split("."))) != (6, 2):
			raise ValueError(f"expected Windows 8 (6.2) {field} version, got {versions}")
	if not re.search(r"\bKERNEL32\.dll\b", dependencies, re.IGNORECASE):
		raise ValueError("dumpbin dependency output is missing the Windows runtime")
	if re.search(
		r"\b(?:VCRUNTIME[^\s]*|MSVCP[^\s]*|MSVCR[^\s]*|ucrtbase|api-ms-win-crt[^\s]*)\.dll\b",
		dependencies,
		re.IGNORECASE
	):
		raise ValueError("C / C++ runtime is not statically linked")
	# 照合の終了
	return

def main() -> int:
	"""保存した dumpbin の両出力を照合する
	戻値：照合の成功時は０
	送出例外：OSError / ValueError: 読込又は検査の失敗
	"""
	parser: argparse.ArgumentParser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("headers", type=Path)
	parser.add_argument("dependencies", type=Path)
	args: argparse.Namespace = parser.parse_args()
	verify_binary(args.headers.read_text(encoding="utf-8-sig"), args.dependencies.read_text(encoding="utf-8-sig"))
	print("Windows minimum declared OS: 8 (6.2); C / C++ runtime: static")
	# 照合の成功の返戻
	return 0

if __name__ == "__main__":
	sys.exit(main())
