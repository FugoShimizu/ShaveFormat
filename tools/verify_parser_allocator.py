#! /usr/bin/env python3
"""仕様の原文保持と一時名の後始末を，確保失敗の注入で照合する"""

import argparse
import pathlib
import subprocess
import sys
import tempfile

from unittest.mock import patch

# 原文保持とリソース不足の通知の，条文から手記述した期待
FAILURE_REASON: bytes = b"Error: formatting could not be completed (out of memory or execution resources), left unchanged: "

FAILURE_REMEDY: bytes = (
	b"retry after releasing memory or execution resources; " b"report persistent failures with the formatter version\n"
)

WRITE_SUMMARY: bytes = (
	b"Formatting check: 0 unchanged, 1 would change, 0 partial, 0 skipped (not checked), 1 failed, "
	b"0 warning(s), 3 path(s) excluded by name, 4 symbolic link(s) not followed.\n"
)

def run_case(
	binary: pathlib.Path,
	original: bytes,
	mode: str,
	countdown: int,
	language: str,
	target: pathlib.Path|None = None
) -> bool:
	"""子プロセスの終了状態・原文・通知を独立した期待と照合する
	引数：
		binary: 失敗を注入する検査用バイナリ
		original: 標準入力へ渡し，保持を照合する全バイト
		mode: 通常実行又は失敗を注入する工程の識別子
		countdown: 失敗迄に許す確保の回数
		language: 入力の言語キー
		target: 書戻の検査対象のパス，標準入力だけの検査では `None`
	戻値：確保失敗を実際に注入出来た場合は真，通常に完了した場合は偽
	送出例外：
		RuntimeError: 終了値・通知・原文・書戻の結果又は一時名の後始末が期待と違う場合
		OSError: 検査用バイナリの起動又は書戻したファイルの読取に失敗する場合
		subprocess.TimeoutExpired: 子プロセスが３０秒以内に終了しない場合
	"""
	args: list[str] = [str(binary), mode, str(countdown), language]
	if target is not None:
		args.append(str(target))
	result: subprocess.CompletedProcess[bytes] = subprocess.run(args, input=original, capture_output=True, check=False, timeout=30)
	if target is not None:
		expected_summary: bytes = (
			WRITE_SUMMARY if
			mode == "write-before" else
			b"Done. 1/2 file(s) formatted, 1 file(s) could not be processed, 2 path(s) could not be read, "
			b"3 path(s) excluded by name, 4 symbolic link(s) not followed.\n"
		)
		if result.returncode != 1 or result.stdout != expected_summary:
			raise RuntimeError(f"write failure changed its exit status or summary: {result!r}")
		if target.read_bytes() != b"int X;\n":
			raise RuntimeError("write failure changed the publication result")
		if pathlib.Path(str(target) + ".parsing.cpp").read_bytes() != b"int   Y;":
			raise RuntimeError("allocation failure changed the source being parsed")
		if any(target.parent.glob(".shavefmt-*")):
			raise RuntimeError("write failure left a temporary name")
	else:
		if result.returncode not in (0, 1) or result.stdout != original:
			raise RuntimeError(f"parser allocation changed the exit status or original bytes: {result!r}")
	if result.returncode == 1:
		label: bytes = (str(target) + ".parsing.cpp").encode("utf-8") if target is not None else b"<stdin>"
		expected: bytes = FAILURE_REASON + label + b"\n[notice] " + label + b": " + FAILURE_REMEDY
		if result.stderr != expected:
			raise RuntimeError(f"resource failure changed its fixed notice, path or remedy: {result!r}")
	else:
		if result.stderr:
			raise RuntimeError(f"successful parsing emitted a failure notice: {result!r}")
	# 確保失敗を実際に注入出来たかの返戻
	return result.returncode == 1

def self_check() -> list[str]:
	"""正常・原文保持・書戻と其の反例で，検査自身の欠落を検出する
	計算量：固定した結果と原文の総バイト数 B に対して O(B)
	戻値：手記述した期待と違う判定の一覧
	送出例外：OSError: 書戻先又は一時名の反例を準備出来ない場合
	"""
	found: list[str] = []
	binary: pathlib.Path = pathlib.Path("allocation-probe")
	notice: bytes = FAILURE_REASON + b"<stdin>\n[notice] <stdin>: " + FAILURE_REMEDY
	for original in [b"\xef\xbb\xbfint X;\r\n"]:
		for status, output, diagnostic, expected in (
			(0, original, b"", False),
			(1, original, notice, True),
			(2, original, b"", None),
			(-11, original, b"", None),
			(0, b"BROKEN", b"", None),
			(1, b"BROKEN", notice, None),
			(1, original + b"x", notice, None),
			(1, original, b"", None),
			(1, original, notice.splitlines(keepends=True)[0], None),
			(1, original, notice.splitlines(keepends=True)[1], None),
			(1, original, notice + b"extra\n", None),
			(0, original, notice, None)
		):
			result: subprocess.CompletedProcess[bytes] = subprocess.CompletedProcess([], status, output, diagnostic)
			with patch("subprocess.run", return_value=result):
				try:
					actual: bool = run_case(binary, original, "parser", 0, "cpp")
				except RuntimeError:
					if expected is not None:
						found.append(f"valid allocation result was rejected: {original!r}, {status}")
				else:
					if expected is None or actual != expected:
						found.append(f"allocation result was not checked: {original!r}, {status}, {diagnostic!r}")
	with tempfile.TemporaryDirectory(prefix="shavefmt-allocation-self-check-") as directory:
		target: pathlib.Path = pathlib.Path(directory).resolve() / "source.cpp"
		parsing: pathlib.Path = pathlib.Path(str(target) + ".parsing.cpp")
		parsing.write_bytes(b"int   Y;")
		notice = FAILURE_REASON + str(parsing).encode("utf-8") + b"\n[notice] " + str(parsing).encode("utf-8") + b": " + FAILURE_REMEDY
		for status, output, content, parsed, has_temporary, is_valid in (
			(1, WRITE_SUMMARY, b"int X;\n", b"int   Y;", False, True),
			(0, WRITE_SUMMARY, b"int X;\n", b"int   Y;", False, False),
			(2, WRITE_SUMMARY, b"int X;\n", b"int   Y;", False, False),
			(1, b"BROKEN", b"int X;\n", b"int   Y;", False, False),
			(1, WRITE_SUMMARY, b"int   X;", b"int   Y;", False, False),
			(1, WRITE_SUMMARY, b"int X;\n", b"int   Y;", True, False),
			(1, WRITE_SUMMARY, None, b"int   Y;", False, False),
			(1, WRITE_SUMMARY, b"int X;\n", b"int Y;\n", False, False),
			(1, WRITE_SUMMARY, b"int X;\n", None, False, False)
		):
			if parsed is None:
				if parsing.exists():
					parsing.unlink()
			else:
				parsing.write_bytes(parsed)
			if content is not None:
				target.write_bytes(content)
			elif target.exists():
				target.unlink()
			temporary: pathlib.Path = target.parent / ".shavefmt-probe"
			if has_temporary:
				temporary.write_bytes(b"")
			elif temporary.exists():
				temporary.unlink()
			result = subprocess.CompletedProcess([], status, output, notice)
			with patch("subprocess.run", return_value=result):
				try:
					actual = run_case(binary, b"", "write-before", 0, "cpp", target)
				except (OSError, RuntimeError):
					if is_valid:
						found.append("valid publication result was rejected")
				else:
					if not is_valid or not actual:
						found.append(f"publication result was not checked: {status}, {content!r}, {has_temporary}")
	# 独立した反例の判定の返戻
	return found

def main() -> int:
	"""既存の golden 入力と，公開前後の書戻を順に検査する
	戻値：全ての対象言語・工程・原文保持・書戻の条件が通過した場合は０
	送出例外：
		SystemExit: 必須の `--binary` が無い等，コマンドラインの引数が不正な場合
		RuntimeError: テストケースが欠けるか，確保失敗の注入又は保持の検査が失敗する場合
		OSError: 入力・一時ファイルの操作又は検査用バイナリの起動が失敗する場合
		subprocess.TimeoutExpired: 子プロセスが３０秒以内に終了しない場合
	"""
	parser: argparse.ArgumentParser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--binary", type=pathlib.Path, required=True)
	args: argparse.Namespace = parser.parse_args()
	if failures := self_check():
		raise RuntimeError("; ".join(failures))
	root: pathlib.Path = pathlib.Path(__file__).resolve().parents[1]
	binary: pathlib.Path = args.binary.resolve()
	languages: tuple[str, ...] = (
		"c",
		"cpp",
		"csharp",
		"java",
		"go",
		"rust",
		"kotlin",
		"swift",
		"php",
		"javascript",
		"typescript",
		"typescriptreact",
		"ruby",
		"python",
		"json",
		"html",
		"css",
		"scss"
	)
	cases: int = 0
	for language in languages:
		fixtures: list[pathlib.Path] = sorted((root / "test_data" / "golden" / language).glob("*.in"))
		if not fixtures:
			raise RuntimeError(f"missing allocation fixture: {language}")
		original: bytes = fixtures[0].read_bytes()
		if run_case(binary, original, "normal", 0, language):
			raise RuntimeError(f"normal parsing injected an allocation failure: {language}")
		cases += 1
		for mode in "parser", "parse":
			failed: int = 0
			for countdown in range(48):
				failed += run_case(binary, original, mode, countdown, language)
				cases += 1
			if not failed:
				raise RuntimeError(f"allocation failure was not injected: {mode}, {language}")
	# 失敗時だけ BOM・CRLF・末尾改行無も原文の全バイトで保持する
	for original in b"", b"\xef\xbb\xbfint X;\r\n", b"int X;":
		if not run_case(binary, original, "parser", 0, "cpp"):
			raise RuntimeError(f"byte preservation did not inject an allocation failure: {original!r}")
		cases += 1
	with tempfile.TemporaryDirectory(prefix="shavefmt-parser-allocation-") as directory:
		target: pathlib.Path = pathlib.Path(directory).resolve() / "source.cpp"
		for mode in "cancel-before", "cancel-after", "cancel-nested":
			target.write_bytes(b"int   X;")
			result: subprocess.CompletedProcess[bytes] = (
				subprocess.run([str(binary), mode, "0", "cpp", str(target)], input=b"", capture_output=True, check=False, timeout=30)
			)
			if result.returncode or result.stdout or result.stderr:
				raise RuntimeError(f"canceled publication changed its completion contract: {result!r}")
			if target.read_bytes() != b"int X;\n":
				raise RuntimeError("canceled publication changed the published content")
			if any(target.parent.glob(".shavefmt-*")):
				raise RuntimeError("canceled publication left a temporary name")
			cases += 1
		for mode in "write-before", "write-after":
			target.write_bytes(b"int   X;")
			pathlib.Path(str(target) + ".parsing.cpp").write_bytes(b"int   Y;")
			if not run_case(binary, b"", mode, 0, "cpp", target):
				raise RuntimeError(f"publication did not inject an allocation failure: {mode}")
			cases += 1
	print(f"parser allocation: {cases} cases passed")
	# 全条件の検査成功の返戻
	return 0

if __name__ == "__main__":
	sys.exit(main())
