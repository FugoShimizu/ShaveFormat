#! /usr/bin/env python3
"""子プロセスの実時間を制限し，時間切れを終了コード１２４で報告する"""

import argparse
import contextlib
import io
import math

from pathlib import Path

import subprocess
import sys

from tempfile import TemporaryDirectory

def run(command: list[str], seconds: float) -> int:
	"""入出力を継承して子を実行し，期限を超えた子を回収する
	引数：command: 実行ファイルと実引数，seconds: 正の有限な制限秒数
	戻値：子の終了値，信号終了は１２８＋信号番号，時間切れは１２４
	送出例外：OSError: 子プロセスを起動出来ない場合
	"""
	try:
		result: subprocess.CompletedProcess[bytes] = subprocess.run(command, timeout=seconds, check=False)
	except subprocess.TimeoutExpired:
		print(f"timed out after {seconds:g}s: {command[0]}", file=sys.stderr)
		# 時間切れの終了値の返戻
		return 124
	# 子の終了値の返戻
	return result.returncode if result.returncode > -1 else 128 - result.returncode

def golden_runner_self_check() -> list[str]:
	"""golden の初回・再整形の時間切れと，後続の正常なケースへの継続を照合する
	戻値：時間切れの理由・失敗数・継続と違う条件（POSIX のスクリプトを実行出来ない環境は対象外）
	送出例外：OSError / subprocess.SubprocessError / ValueError: 一時入力の操作・起動・終了又は検査関数の読取が失敗する場合
	"""
	if sys.platform == "win32":
		# POSIX のスクリプトの対象外の結果の返戻
		return []
	root: Path = Path(__file__).resolve().parent.parent
	source: str = (root / "tools/golden_test.sh").read_text()
	functions: str = source[source.index("check_case() {"):source.index('if ! TMP_ROOT="')]
	with TemporaryDirectory(prefix="shavefmt-golden-timeout-") as directory:
		temporary: Path = Path(directory)
		fixtures: Path = temporary / "cpp"
		fixtures.mkdir()
		binary: Path = temporary / "formatter"
		binary.write_text(
			"#!/bin/bash\ntext=$(cat)\ncase \"$text\" in\n"
			"'int Hang;'|'int Second;') exec sleep 30;;\n"
			"'int  Second;') printf 'int Second;\\n';;\n"
			"*) printf 'int X;\\n';;\nesac\n"
		)
		binary.chmod(0O700)
		paths: list[str] = []
		for (
			name,
			before,
			after
		) in ("first", "int Hang;\n", "int Hang;\n"), ("second", "int  Second;\n", "int Second;\n"), ("normal", "int  X;\n", "int X;\n"):
			path: Path = fixtures / (name + ".in")
			path.write_text(before)
			path.with_suffix(".out").write_text(after)
			paths.append(str(path))
		script: str = (
			functions + "FMT=\"$1\"; ROOT=\"$2\"; TMP_ROOT=\"$3\"; CASE_TIMEOUT=3; shift 3;\n"
			"for input in \"$@\"; do if check_case \"$input\"; then echo passed; else echo failed; fi; done\n"
		)
		result: subprocess.CompletedProcess[str] = subprocess.run(
			["bash", "-c", script, "_", str(binary), str(root), str(temporary), *paths],
			capture_output=True,
			text=True,
			timeout=60
		)
		if (
			result.returncode or result.stdout != "failed\nfailed\npassed\n" or "FAIL cpp/first: timed out" not in result.stderr or
			"FAIL cpp/second (idempotence): timed out" not in result.stderr
		):
			# 時間切れの理由又は継続の欠落の返戻
			return ["golden の時間切れの検出又は後続のケースへの継続が違う"]
	# 正常な期限と継続の結果の返戻
	return []

def self_check() -> list[str]:
	"""通常の終了値と時間切れを，有限な子プロセスで照合する
	戻値：独立した終了値の期待と違う条件
	送出例外：OSError / subprocess.SubprocessError / ValueError: 一時入力の操作・起動・終了又は検査関数の読取が失敗する場合
	"""
	found: list[str] = golden_runner_self_check()
	for source, expected in ("pass", 0), ("import sys;sys.exit(7)", 7), ("import time;time.sleep(5)", 124):
		errors: io.StringIO = io.StringIO()
		with contextlib.redirect_stderr(errors):
			status: int = run([sys.executable, "-c", source], 0.2 if expected == 124 else 5)
		if status != expected or ("timed out" in errors.getvalue()) != (expected == 124):
			found.append(f"子プロセスの終了値又は時間切れの理由が違う：{expected}")
	# 子の実行の照合結果の返戻
	return found

def main() -> int:
	"""コマンドと制限時間を受け取り，実行の終了値を返す
	戻値：子の終了値，時間切れは１２４，起動失敗は１２７，自己検査の失敗は１
	送出例外：
		SystemExit: 制限時間又はコマンドが不正な場合
		OSError / subprocess.SubprocessError / ValueError: 自己検査の一時入力・起動・終了又は検査関数の読取が失敗する場合
	"""
	parser: argparse.ArgumentParser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--seconds", type=float, default=30)
	parser.add_argument("--self-check", action="store_true")
	parser.add_argument("command", nargs=argparse.REMAINDER)
	args: argparse.Namespace = parser.parse_args()
	if not math.isfinite(args.seconds) or args.seconds <= 0.0:
		parser.error("--seconds must be finite and positive")
	if args.self_check:
		problems: list[str] = self_check()
		for problem in problems:
			print(f"FAIL limited process: {problem}", file=sys.stderr)
		# 自己検査の終了値の返戻
		return bool(problems)
	command: list[str] = args.command[1:] if args.command[:1] == ["--"] else args.command
	if not command:
		parser.error("a command is required")
	try:
		# 実行の終了値の返戻
		return run(command, args.seconds)
	except OSError as error:
		print(f"could not execute {command[0]}: {error}", file=sys.stderr)
		# 起動の失敗の終了値の返戻
		return 127

if __name__ == "__main__":
	sys.exit(main())
