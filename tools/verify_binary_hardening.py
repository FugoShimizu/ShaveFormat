#! /usr/bin/env python3
"""配布バイナリと静的に結合する実行時ライブラリの防御を独立に照合する"""

import argparse

from pathlib import Path

import re
import subprocess
import sys

def read_tool(arguments: list[str]) -> str:
	"""検査ツールの原本を取得する
	計算量：引数長 A，受信量 N と検査ツールの費用 Q に対し O(A + N + Q)
	引数：arguments: 実行するコマンド
	戻値：コマンドの標準出力
	送出例外：OSError / subprocess.SubprocessError: 起動又は実行の失敗
	"""
	# 検査自体の失敗を空の検査結果として受け取らない
	return subprocess.run(arguments, check=True, capture_output=True, text=True, timeout=30).stdout

def require_match(text: str, pattern: str, description: str) -> None:
	"""必須の宣言又は参照を照合する
	計算量：本検査の固定パターンと原本長 N に対し O(N)
	引数：text: 検査ツールの原本，pattern: 必須のパターン，description: 失敗時の説明
	送出例外：ValueError: 防御の証拠が無い場合
	"""
	if not re.search(pattern, text, re.IGNORECASE | re.MULTILINE):
		raise ValueError(description)
	# 照合の終了
	return

def verify_windows(headers: str, loadconfig: str, symbols: str) -> None:
	"""PE と実行時のオブジェクトの防御を照合する
	計算量：原本の総長 N に対し O(N)
	引数：headers: /headers の原本，loadconfig: /loadconfig の原本，symbols: 実行時の /symbols の原本
	送出例外：ValueError: ASLR・NX・CFG 又は実行時のスタック保護の証拠が無い場合
	"""
	require_match(headers, r"^[ \t]+Dynamic base\s*$", "ASLR is missing")
	require_match(headers, r"^[ \t]+NX compatible\s*$", "NX is missing")
	require_match(loadconfig, r"^[ \t]+CF Instrumented\s*$", "CFG instrumentation is missing")
	require_match(loadconfig, r"^[ \t]+FID table present\s*$", "CFG function table is missing")
	require_match(loadconfig, r"^[ \t]*0*[1-9a-f][0-9a-f]*\s+Security Cookie\s*$", "security cookie is missing")
	require_match(symbols, r"\bUNDEF\b[^\r\n]*\b__security_check_cookie\b", "runtime stack protection is missing")
	require_match(symbols, r"\.gfids\$y\b", "runtime CFG function identifiers are missing")
	# 照合の終了
	return

def verify_unix(binary: Path, runtime: Path, platform: str) -> None:
	"""ELF 又は Mach-O の実行制御と両側のスタック保護を照合する
	計算量：原本の総長 N と検査ツールの費用 Q に対し O(N + Q)
	引数：binary: 配布バイナリ，runtime: 結合する実行時の静的書庫，platform: 対象 OS
	送出例外：OSError / CalledProcessError / ValueError: 検査失敗又は防御の証拠が無い場合
	"""
	if platform == "linux":
		header: str = read_tool(["readelf", "-hW", str(binary)])
		segments: str = read_tool(["readelf", "-lW", str(binary)])
		dynamic: str = read_tool(["readelf", "-dW", str(binary)])
		require_match(header, r"Type:\s+DYN\b", "PIE is missing")
		require_match(segments, r"^[ \t]*GNU_RELRO\s", "RELRO is missing")
		require_match(dynamic, r"\bBIND_NOW\b|\bFLAGS[^\r\n]*\bNOW\b", "immediate binding is missing")
		stack: list[str] = re.findall(r"^[ \t]*GNU_STACK\s+(.+)$", segments, re.MULTILINE)
		if len(stack) != 1 or not re.search(r"\bRW\b", stack[0]):
			raise ValueError("non-executable stack is missing")
	else:
		header = read_tool(["otool", "-hv", str(binary)])
		require_match(header, r"\bPIE\b", "PIE is missing")
		if re.search(r"\bALLOW_STACK_EXECUTION\b", header):
			raise ValueError("executable stack is enabled")
	for artifact in binary, runtime:
		symbols: str = read_tool(["nm", "-u", str(artifact)])
		require_match(symbols, r"\b_+stack_chk_fail\b", f"stack protection is missing: {artifact}")
	# 照合の終了
	return

def main() -> int:
	"""配布対象の形式に対応する照合を実行する
	計算量：原本の総長 N と検査ツールの費用 Q に対し O(N + Q)
	戻値：照合成功時は０
	送出例外：OSError / CalledProcessError / ValueError: 読込又は照合の失敗
	"""
	parser: argparse.ArgumentParser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("platform", choices=["linux", "macos", "windows"])
	parser.add_argument("binary", type=Path)
	parser.add_argument("runtime", type=Path)
	parser.add_argument("--loadconfig", type=Path)
	args: argparse.Namespace = parser.parse_args()
	if args.platform == "windows":
		if not args.loadconfig:
			parser.error("Windows verification requires --loadconfig")
		verify_windows(
			args.binary.read_text(encoding="utf-8-sig"),
			args.loadconfig.read_text(encoding="utf-8-sig"),
			args.runtime.read_text(encoding="utf-8-sig")
		)
	else:
		verify_unix(args.binary, args.runtime, args.platform)
	print(f"{args.platform}: executable and tree-sitter runtime hardening verified")
	# 照合成功の返戻
	return 0

if __name__ == "__main__":
	sys.exit(main())
