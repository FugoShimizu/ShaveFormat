#! /usr/bin/env python3
"""製品とテスト専用の基準側の全テストケース・幅の変種を照合する（整形仕様 11.1）

終了コード・標準出力・警告を含む標準エラーの全行を照合する
作業予算の見送も比較の失敗とし，全ての組の終了結果を照合する
各側の実行時間と作業量は比較の合否と分離し，フォーマッターと同じ場所の `reference_comparison.tsv` へ記録する
使方：python3 tools/verify_reference_comparison.py [フォーマッター] [基準側] [失敗数の保存先]
"""

from __future__ import annotations

from verify_case_variants import WIDE, WIDTHS

import os

from pathlib import Path

import subprocess
import sys
import time

from typing import NamedTuple
from collections.abc import Callable, Iterator

import concurrent.futures

# 検査の経路と実行設定
ROOT: Path = Path(__file__).resolve().parent.parent
GOLDEN: Path = ROOT / "test_data/golden"
JOBS: int = min(max(int(os.environ.get("SHAVEFMT_JOBS", "2")), 1), 2)
ENVIRONMENT: dict[str, str] = dict(os.environ, SHAVEFMT_VERIFY_COMMENT_SEPARATION="1")

# 構文と行分割の作業予算の超過に依る見送の通知
BUDGET_SKIPS: tuple[bytes, ...] = (
	b"[skip] <stdin>: line wrapping exceeds formatting limit",
	b"[skip] <stdin>: structured text exceeds formatting limit"
)

class Comparison(NamedTuple):
	"""１入力・１幅の独立した比較結果"""

	label: str # テストケースと幅
	problems: list[str] # 不一致又は実行失敗
	product_seconds: float # 製品側の実行時間
	ref_seconds: float # 基準側の実行時間
	work: list[str] # 合否に使わない両側の作業量（製品・基準の順）

def execute(binary: str, src: bytes, lang: str, width: str) -> tuple[subprocess.CompletedProcess[bytes], float]:
	"""指定した側を観測付で実行し，終了結果と実行時間を返す
	計算量：入力・出力長 B と実行時間 T に対して O(B + T)
	引数：
		binary: フォーマッターの実行ファイル
		src: 入力のバイト列
		lang: 入力の言語
		width: 行幅
	戻値：
		終了結果と秒単位の実行時間
	送出例外：
		OSError: 子プロセスを起動出来ない場合
		subprocess.TimeoutExpired: 実行時間の上限を超える場合
	"""
	# 中核の観測付の子プロセスの起動
	started: float = time.perf_counter()
	result: subprocess.CompletedProcess[bytes] = subprocess.run(
		(binary, "--stdin", "--lang", lang, "--chars", width),
		input=src,
		capture_output=True,
		env=ENVIRONMENT,
		timeout=30
	)
	# 終了結果と実行時間の返戻
	return result, time.perf_counter() - started

def compare(product: str, reference: str, src: Path, width: str) -> Comparison:
	"""１入力・１幅を両側で順に実行し，実行の失敗と全ての契約上の出力を照合する
	計算量：入出力長 B と両側の実行時間 T に対して O(B + T)
	引数：
		product: 製品の実行ファイル
		reference: 基準側の実行ファイル
		src: 入力の経路
		width: 行幅
	戻値：
		失敗の説明と，合否に使わない両側の実行時間
	"""
	# １テストケース・１幅の両手順の照合の準備
	label: str = f"{src.relative_to(ROOT)}, width {width}"
	product_seconds: float = 0
	ref_seconds: float = 0
	try:
		text: bytes = src.read_bytes()
	except OSError as error:
		# 入力の準備の失敗の返戻
		return Comparison(label, [f"input failed: {error}"], 0, 0, [])
	results: list[subprocess.CompletedProcess[bytes]] = []
	problems: list[str] = []
	for side, binary in ("product", product), ("reference", reference):
		started: float = time.perf_counter()
		seconds: float
		try:
			result, seconds = execute(binary, text, src.parent.name, width)
			results.append(result)
		except subprocess.TimeoutExpired as error:
			seconds = time.perf_counter() - started
			problems.append(f"{side} timed out after {error.timeout:g}s")
		except (OSError, subprocess.SubprocessError) as error:
			seconds = time.perf_counter() - started
			problems.append(f"{side} execution failed: {error}")
		if side == "product":
			product_seconds = seconds
		else:
			ref_seconds = seconds
	if problems:
		# 側毎に区別した実行の失敗の返戻
		return Comparison(label, problems, product_seconds, ref_seconds, [])
	first, second = results
	work: list[str] = [
		" ".join(line[len(b"[work] "):].decode() for line in result.stderr.splitlines() if line.startswith(b"[work] ")) for result in
		(first, second)
	]
	problems = [
		f"{side} exceeded the work budget" for side, result in (("product", first), ("reference", second)) if any(
			line in BUDGET_SKIPS for line in result.stderr.splitlines()
		)
	]
	if first.returncode != second.returncode:
		problems.append(f"exit status differs: {first.returncode} / {second.returncode}")
	if first.stdout != second.stdout:
		problems.append("standard output differs")
	# 作業量の記録は警告・中核モデルの一致判定と分離する
	first_lines: list[bytes] = [line for line in first.stderr.splitlines() if not line.startswith(b"[work] ")]
	second_lines: list[bytes] = [line for line in second.stderr.splitlines() if not line.startswith(b"[work] ")]
	if first_lines != second_lines:
		pos: int = next(
			(idx for idx, pair in enumerate(zip(first_lines, second_lines)) if pair[0] != pair[1]),
			min(len(first_lines), len(second_lines))
		)
		problems.append(f"warnings or observations differ at line {pos + 1}")
	# 一致判定と時間記録の返戻
	return Comparison(label, problems, product_seconds, ref_seconds, work)

def comparison_self_check() -> list[str]:
	"""正常な一致と出力の差，作業予算に依る見送の欠落を反例で照合する
	計算量：固定数の結果の総バイト数 B に対して O(B)
	戻値：条文と違う判定の一覧
	"""
	original_execute: Callable[[str, bytes, str, str], tuple[subprocess.CompletedProcess[bytes], float]] = execute
	found: list[str] = []
	source: Path = GOLDEN / "c/return_explicit.in"
	try:
		for skip in BUDGET_SKIPS:
			for failing_side in 0, 1:
				for status, diagnostic in (
					(0, b""),
					(0, skip + b"\n"),
					(1, b"Error: injected failure\n"),
					(2, b"Error: injected usage failure\n"),
					(0, b"Error: injected failure\n"),
					(-4, b"")
				):
					results: list[subprocess.CompletedProcess[bytes]] = [subprocess.CompletedProcess([], 0, b"source", skip + b"\n")] * 2
					results[failing_side] = subprocess.CompletedProcess([], status, b"source", diagnostic)
					globals()["execute"] = lambda binary, src, lang, width: (results[int(binary)], 0.0)
					result: Comparison = compare("0", "1", source, "128")
					if not result.problems:
						found.append(f"作業予算の見送を検査の成功とした：{skip!r}, {failing_side}, {status}")
		for first, second, should_match in (
			(subprocess.CompletedProcess([], 0, b"source", b""), subprocess.CompletedProcess([], 0, b"source", b""), True),
			(
				subprocess.CompletedProcess([], 0, b"source", b"[work] structure 1\n"),
				subprocess.CompletedProcess([], 0, b"source", b"[work] structure 9\n"),
				True
			),
			(subprocess.CompletedProcess([], 0, b"source", b""), subprocess.CompletedProcess([], 0, b"BROKEN", b""), False),
			(subprocess.CompletedProcess([], 0, b"source", b""), subprocess.CompletedProcess([], 1, b"source", b""), False),
			(subprocess.CompletedProcess([], 0, b"source", b""), subprocess.CompletedProcess([], 0, b"source", b"[warn] changed\n"), False)
		):
			results = [first, second]
			globals()["execute"] = lambda binary, src, lang, width: (results[int(binary)], 0.0)
			if bool(compare("0", "1", source, "128").problems) == should_match:
				found.append("独立した終了結果の一致判定が違う")
		for failing_side in "0", "1":
			calls: list[str] = []
			def timeout_execute(binary: str, src: bytes, lang: str, width: str) -> tuple[subprocess.CompletedProcess[bytes], float]:
				"""指定した側だけを時間切れにし，両側の起動を記録する
				引数：binary: 側の識別子，src: 入力，lang: 言語，width: 幅
				戻値：成功した側の終了結果と時間
				送出例外：subprocess.TimeoutExpired: 選んだ側の実行が期限を超えた場合
				"""
				calls.append(binary)
				if binary == failing_side:
					raise subprocess.TimeoutExpired(binary, 30)
				# 成功した側の終了結果の返戻
				return subprocess.CompletedProcess([], 0, b"source", b""), 0.0
			globals()["execute"] = timeout_execute
			result = compare("0", "1", source, "128")
			side: str = "product" if failing_side == "0" else "reference"
			if result.problems != [f"{side} timed out after 30s"] or calls != ["0", "1"]:
				found.append(f"時間切れの側又は両側の起動を失った：{side}")
	finally:
		globals()["execute"] = original_execute
	# 不一致の返戻
	return found

def main() -> int:
	"""`test_data/golden` の全テストケースと共有の幅の変種を照合し，失敗数と時間を報告する
	計算量：テストケース数 F，幅数 W，総入出力長 B，総実行時間 T に対して O(FW + B + T)
	戻値：
		全比較の一致は０，不一致又は未実施は１
	送出例外：
		OSError: テストケース・幅・失敗数の記録を読み書き出来ない場合
		UnicodeDecodeError: 読み込んだファイルを指定の文字コードで解釈出来ない場合
	"""
	# 実行引数と検査の準備
	product: str = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "build/shavefmt")
	reference: str = sys.argv[2] if len(sys.argv) > 2 else str(Path(product).with_name("shavefmt_reference" + Path(product).suffix))
	cases: list[tuple[Path, str]] = []
	for src in sorted(GOLDEN.glob("*/*.in")):
		chars: Path = src.with_suffix(".chars")
		width: str = chars.read_text().strip() if chars.exists() else "128"
		cases.extend((src, variant) for variant in dict.fromkeys((width, WIDE, *WIDTHS)))
	self_failures: list[str] = comparison_self_check()
	failures: int = len(self_failures)
	for problem in self_failures:
		print(f"FAIL reference comparison self check: {problem}", file=sys.stderr)
	product_seconds: float = 0
	ref_seconds: float = 0
	records: list[str] = ["case\tproduct seconds\treference seconds\tproduct work\treference work"]
	# 各工程の中では両側を順に実行し，機械全体で本検査の実行を２本迄にする
	with concurrent.futures.ThreadPoolExecutor(max_workers=JOBS) as pool:
		results: Iterator[Comparison] = pool.map(
			compare,
			(product for _ in cases),
			(reference for _ in cases),
			(src for src, _ in cases),
			(width for _, width in cases)
		)
		for result in results:
			product_seconds += result.product_seconds
			ref_seconds += result.ref_seconds
			records.append(
				"\t".join((result.label, f"{result.product_seconds:.6f}", f"{result.ref_seconds:.6f}", *(result.work or ["", ""])))
			)
			if result.problems:
				failures += 1
				for problem in result.problems:
					print(f"FAIL reference comparison: {result.label}: {problem}", file=sys.stderr)
	Path(product).with_name("reference_comparison.tsv").write_text("\n".join(records) + "\n")
	if not cases:
		failures += 1
		print("FAIL reference comparison: no inputs selected", file=sys.stderr)
	print(f"reference comparison: {len(cases)} cases, {failures} failed, 0 excluded by the work budget")
	print(f"reference timing total: product={product_seconds:.6f}s reference={ref_seconds:.6f}s")
	if len(sys.argv) > 3:
		Path(sys.argv[3]).write_text(f"{failures}\n")
	# 検査の終了状態の返戻
	return int(bool(failures))

if __name__ == "__main__":
	sys.exit(main())
