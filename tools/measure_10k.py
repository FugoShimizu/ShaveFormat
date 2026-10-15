#! /usr/bin/env python3
"""全言語の１万行の整形時間を記録し，決定的な作業予算を照合する

使方：python3 tools/measure_10k.py <フォーマッター> <入力の保存先> <報告の保存先> --work-probe <計数ツール>
構築を行わない環境での実時間の記録だけは --timing-only を指定する
１秒の目標の未達は記録し，CI の合否には入力の完了と作業予算を使う
"""

from __future__ import annotations

from build_10k import CACHE, LANGUAGES, METRIC_VERSION, SIZES, SOURCES, TARGET_LINES, materialize_inputs, verify_inputs, verify_sources

import argparse

from contextlib import redirect_stdout

import hashlib
import io
import json
import os

from pathlib import Path

import platform
import shutil
import subprocess
import sys

from tempfile import TemporaryDirectory

import time

from unittest.mock import patch

# 時間の揺れを記録から除く測定回数
REPEATS: int = 3

# 計数ツールの出力順（既存の２工程，解析・修復・検証・配置・投影・静的解析）
PHASES: tuple[str, ...] = (
	"structure",
	"wrapping",
	"parseCalls",
	"parseBytes",
	"parseReadBytes",
	"repairCalls",
	"repairReadBytes",
	"validationCalls",
	"validationBytes",
	"placementTargets",
	"relationVisits",
	"castRepairVisits",
	"lintNodes",
	"lintComments"
)

def measure(binary: str, src: bytes, lang: str) -> float:
	"""通常の CLI の検証・静的解析を含む整形を測定する
	計算量：入出力長 B と整形時間 T に対して O(B + T)
	引数：
		binary: 配布するフォーマッター
		src: 固定原本から生成した入力
		lang: 対象言語
	戻値：成功した実行の最短の実時間（秒）
	送出例外：
		OSError / subprocess.SubprocessError: 実行又は時間制限に失敗する場合
		ValueError: 整形を完了しない場合
	"""
	# 読取専用な観測の時間を測定へ混ぜない為の実行環境
	env: dict[str, str] = os.environ.copy()
	for key in "SHAVEFMT_STRUCT_DIAG", "SHAVEFMT_VERIFY_COMMENT_SEPARATION", "SHAVEFMT_WARNINGS":
		env.pop(key, None)
	seconds: list[float] = []
	# 配布するバイナリでの独立した出力検証迄の実行
	for _ in range(REPEATS):
		started: float = time.perf_counter()
		result: subprocess.CompletedProcess[bytes] = (
			subprocess.run((binary, "--stdin", "--lang", lang, "--fail-on-skip"), input=src, capture_output=True, env=env, timeout=300)
		)
		seconds.append(time.perf_counter() - started)
		if (
			result.returncode or not result.stdout or
			any(word in result.stderr for word in (b"[skip]", b"Error:", b"formatting is partial"))
		):
			raise ValueError(f"{lang}: incomplete formatting (exit {result.returncode}): {result.stderr.decode(errors='replace')}")
	# 正常に完了した実行の最短時間の返戻
	return min(seconds)

def get_work(binary: str, src: Path, lang: str) -> dict[str, int]:
	"""各工程の決定的な計数を得て，既存の中核予算と観測の欠落を照合する
	引数：binary: 構築した計数ツール，src: 性能入力，lang: 言語
	戻値：工程毎の計数と中核の上限
	送出例外：OSError / subprocess.SubprocessError / ValueError: 実行又は計数の不備
	"""
	result: subprocess.CompletedProcess[bytes] = subprocess.run((binary, lang, str(src)), capture_output=True, timeout=300)
	if result.returncode:
		raise ValueError(f"{lang}: work probe failed: {result.stderr.decode(errors='replace')}")
	fields: list[int] = [int(field) for field in result.stdout.split()]
	if len(fields) != len(PHASES) + 3 or fields[0] != METRIC_VERSION or any(field < 0 for field in fields):
		raise ValueError(f"{lang}: missing or invalid stage accounting; rebuild the corrected probe")
	fields.pop(0)
	limit: int = fields.pop(2)
	base: int = fields.pop(2)
	work: dict[str, int] = dict(zip(PHASES, fields, strict=True))
	work["limit"] = limit
	work["base"] = base
	if not base:
		raise ValueError(f"{lang}: projected work base was not observed")
	if limit < max(524288, base << 5):
		raise ValueError(f"{lang}: work limit does not cover the projected work base")
	if not work["structure"] or not work["wrapping"] or limit < 524288 or max(work["structure"], work["wrapping"]) > limit:
		raise ValueError(f"{lang}: invalid core accounting or budget exceeded")
	if not all(
		work[phase] for phase in ("parseCalls", "parseBytes", "parseReadBytes", "validationCalls", "validationBytes", "lintNodes")
	):
		raise ValueError(f"{lang}: required stage was not observed")
	# 工程別の計数の返戻
	return work

def verify_counts(
	ref: dict,
	data: bytes,
	lang: str,
	size: int,
	work: dict[str, int],
	previous: dict[str, int]|None,
	previous_ref: dict|None
) -> None:
	"""取得済の計数から密度と倍々の伸長を照合する
	引数：ref: 固定した基準，data: 入力，lang: 言語，size: 規模，work: 実測計数，previous: 前の規模の実測計数，previous_ref: 前の規模の基準
	送出例外：ValueError: 計数の不備又は退行の検出
	"""
	base: int = work["base"]
	for phase in PHASES:
		used: int = work[phase]
		# 中核は既存の W，其の他は投入する固定入力のバイト数で密度を照合する
		denominator: int = base if phase in ("structure", "wrapping") else len(data)
		ref_denominator: int = ref["base"] if phase in ("structure", "wrapping") else ref["bytes"]
		if used * ref_denominator * 10 > ref[phase] * denominator * 11:
			raise ValueError(f"{lang}/{size}: {phase} density regressed by more than 10%")
		if previous and previous_ref and previous[phase]:
			# 規模毎の構文構成が異なる為，固定した同じ原本の工程別の基準の伸長と照合する
			current_scale: int = ref[phase]
			previous_scale: int = previous_ref[phase]
			if used * previous_scale * 10 > previous[phase] * current_scale * 11:
				raise ValueError(f"{lang}/{size}: {phase} work growth exceeds baseline growth by more than 10%")
	# 照合の終了
	return

def verify_regression(baseline: dict, binary: str, inputs: Path, lang: str) -> dict[str, int]:
	"""作業単位の密度と倍々の規模での伸長を照合する
	引数：baseline: 固定した基準，binary: 計数ツール，inputs: 入力の保存先，lang: 言語
	戻値：１万行の全工程の計数と中核の上限
	送出例外：OSError / subprocess.SubprocessError / ValueError: 計数・基準又は退行の検出
	"""
	if not baseline["isVerified"] or baseline.get("metricVersion") != METRIC_VERSION:
		raise ValueError("performance baseline is provisional; recapture all languages from the corrected frozen probe")
	previous: dict[str, int]|None = None
	previous_ref: dict|None = None
	measured: dict[str, int]|None = None
	# 各工程を別々に照合し，片方の退行を合計で隠さない
	for size in SIZES:
		src: Path = inputs / str(size) / f"{lang}.in"
		ref: dict|None = baseline["languages"][lang][str(size)]
		if ref is None:
			raise ValueError(f"{lang}/{size}: baseline pending; capture it explicitly with the corrected release probe")
		if hashlib.sha256(src.read_bytes()).hexdigest() != ref["inputSha256"]:
			raise ValueError(f"{lang}/{size}: input differs from the reviewed baseline corpus")
		work: dict[str, int] = get_work(binary, src, lang)
		data: bytes = src.read_bytes()
		verify_counts(ref, data, lang, size, work, previous, previous_ref)
		previous = work
		previous_ref = ref
		if size == TARGET_LINES:
			measured = work
	if measured is None:
		raise ValueError("missing 10k-line measurement")
	# １万行の計数の返戻
	return measured

def capture_baseline(binary: str, inputs: Path, output: Path, cache: Path = CACHE, generate: bool = False) -> None:
	"""明示した実行でだけ作業単位の実測基準を保存する
	引数：binary: 修正を構築済の計数ツール，inputs: 入力の保存先，output: 保存先，cache: 外部原本の保存先，generate: 原本から入力を生成するか
	送出例外：OSError / ValueError / subprocess.SubprocessError: 入力・計数・保存の失敗
	"""
	# 採取する入力の原本・許諾・出所を確認する
	manifest: dict = json.loads((SOURCES / "manifest.json").read_bytes())
	originals: dict[str, list[bytes]] = verify_sources(manifest, cache)
	checked: dict[tuple[str, int], bytes] = (
		materialize_inputs(manifest, originals, inputs) if generate else verify_inputs(manifest, originals, inputs)
	)
	# 実行中の親の構築でバイナリが差し替わっても同じ計数の実装を保つ
	with TemporaryDirectory(prefix="shavefmt-work-baseline-") as temp:
		frozen: Path = Path(temp) / ("verify_performance" + Path(binary).suffix)
		shutil.copy2(binary, frozen)
		digest: str = hashlib.sha256(frozen.read_bytes()).hexdigest()
		languages: dict = {}
		errors: list[str] = []
		# 固定した１つの計数ツールで全件を取得し終える迄，既存の基準を書き換えない
		for lang in LANGUAGES:
			values: dict = {}
			previous: dict[str, int]|None = None
			previous_ref: dict|None = None
			for size in SIZES:
				data: bytes = checked[lang, size]
				sample: Path = Path(temp) / f"{lang}-{size}.in"
				sample.write_bytes(data)
				try:
					work: dict[str, int] = get_work(str(frozen), sample, lang)
					ref: dict = {
						"base": work["base"],
						**{ phase: work[phase] for phase in PHASES },
						"inputSha256": hashlib.sha256(data).hexdigest(),
						"lines": data.count(b"\n"),
						"bytes": len(data)
					}
					verify_counts(ref, data, lang, size, work, previous, previous_ref)
					values[str(size)] = ref
					previous = work
					previous_ref = ref
					print(f"baseline {lang}/{size}: measured and checked", flush=True)
				except (OSError, ValueError, subprocess.SubprocessError) as e:
					errors.append(f"{lang}/{size}: {e}")
					previous = None
					previous_ref = None
			languages[lang] = values
		if errors:
			raise ValueError("baseline capture did not pass every input:\n" + "\n".join(errors))
	result: dict = {
		"source": "explicit capture of release work accounting",
		"metricVersion": METRIC_VERSION,
		"probeSha256": digest,
		"isVerified": True,
		"languages": languages
	}
	output.write_bytes((json.dumps(result, ensure_ascii=False, indent="\t") + "\n").encode("utf-8"))
	# 基準の採取の終了
	return

def self_check() -> None:
	"""計数の形式・基準量・伸長と採取失敗時の保存を外部接続無しで検査する
	送出例外：AssertionError: 計数又は基準の保存が契約と異なる場合
	"""
	# 列の対応と固定費で隠れて居る基準量の独立した期待
	raw: bytes = b"3 100 50 524288 1 2 10 10 0 0 1 5 0 2 0 1 0\n"
	with patch(__name__ + ".subprocess.run", return_value=subprocess.CompletedProcess((), 0, raw, b"")):
		work: dict[str, int] = get_work("unused", Path("unused"), "c")
	assert work == {
		"structure": 100,
		"wrapping": 50,
		"base": 1,
		"limit": 524288,
		"parseCalls": 2,
		"parseBytes": 10,
		"parseReadBytes": 10,
		"repairCalls": 0,
		"repairReadBytes": 0,
		"validationCalls": 1,
		"validationBytes": 5,
		"placementTargets": 0,
		"relationVisits": 2,
		"castRepairVisits": 0,
		"lintNodes": 1,
		"lintComments": 0
	}
	ref: dict = { **work, "bytes": 3 }
	# 同じ構文構成の基準量が３倍なら線形の３倍を許す
	linear: dict[str, int] = { **work, "structure": 300, "wrapping": 150, "base": 3 }
	verify_counts({ **linear, "bytes": 3 }, b"abc", "c", 10000, linear, work, ref)
	# 異なる構文構成の固定基準を其の儘再現した場合は密度の違いを退行としない
	varied: dict[str, int] = { **work, "structure": 200, "wrapping": 150, "base": 1 }
	verify_counts({ **varied, "bytes": 3 }, b"abc", "c", 10000, varied, work, ref)
	# 構文構成が同じ２倍の基準は手で定め，実測した２乗の作業を基準にしない
	linear_ref: dict = { **work, "structure": 200, "wrapping": 100, "base": 2, "bytes": 3 }
	for phase in "structure", "wrapping":
		for scale in 22, 23, 40:
			measured: dict[str, int] = { **linear_ref, phase: ref[phase] * scale // 10 }
			try:
				verify_counts(linear_ref, b"abc", "c", 10000, measured, work, ref)
			except ValueError as e:
				assert scale != 22, "work at the 10% boundary was rejected"
				assert "density regressed" in str(e)
			else:
				assert scale == 22, "quadratic work was accepted"
		# 前の規模だけ改善した場合も，密度とは独立して基準を超える伸長を拒む
		improved: dict[str, int] = { **work, phase: ref[phase] // 2 }
		try:
			verify_counts(linear_ref, b"abc", "c", 10000, linear_ref, improved, ref)
		except ValueError as e:
			assert "growth exceeds" in str(e)
		else:
			raise AssertionError("growth regression was accepted")
	# 未観測の工程を０で埋めず，古い形式を拒む
	with patch(__name__ + ".subprocess.run", return_value=subprocess.CompletedProcess((), 0, b"1 2 3\n", b"")):
		try:
			get_work("unused", Path("unused"), "c")
		except ValueError as e:
			assert "rebuild" in str(e)
		else:
			raise AssertionError("obsolete accounting was accepted")
	# 全件の失敗を記録し，保存済の基準は採取の失敗で上書しない
	with TemporaryDirectory(prefix="shavefmt-baseline-check-") as directory:
		root: Path = Path(directory)
		binary: Path = root / "probe"
		binary.write_bytes(b"unused probe")
		output: Path = root / "baseline.json"
		output.write_bytes(b"previous baseline")
		checked: dict[tuple[str, int], bytes] = { (lang, size): b"x\n" * size for lang in LANGUAGES for size in SIZES }
		with (
			patch(__name__ + ".verify_sources", return_value={}),
			patch(__name__ + ".verify_inputs", return_value=checked),
			patch(__name__ + ".get_work", side_effect=ValueError("synthetic rejection")) as probe,
			redirect_stdout(io.StringIO())
		):
			try:
				capture_baseline(str(binary), root / "inputs", output)
			except ValueError as e:
				assert "c/5000" in str(e) and "scss/20000" in str(e)
			else:
				raise AssertionError("failed capture was committed")
			assert probe.call_count == len(LANGUAGES) * len(SIZES)
		assert output.read_bytes() == b"previous baseline"
	print("performance baseline: protocol, base, growth and failed capture checks passed")
	# 検査の終了
	return

def main() -> int:
	"""全言語の性能検査を実行し，失敗も含めて結果を記録する
	戻値：全言語で完了すれば０，入力・計数・実行の失敗は１
	送出例外：OSError: 報告を書き込めない場合
	"""
	if sys.argv[1:] == ["--self-test"]:
		self_check()
		# 自己検査の終了
		return 0
	# 入力と計数の実行モードの確認
	parser: argparse.ArgumentParser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("formatter", type=Path)
	parser.add_argument("inputs", type=Path)
	parser.add_argument("report", type=Path)
	parser.add_argument("--capture-baseline", type=Path)
	parser.add_argument("--generate-inputs", action="store_true")
	parser.add_argument("--cache", type=Path, default=CACHE)
	parser.add_argument("--environment", default=platform.platform() + " / " + platform.machine())
	parser.add_argument("--pgo", choices=("enabled", "disabled", "unknown"), default="unknown")
	parser.add_argument("--baseline", type=Path, default=Path(__file__).with_name("performance_baseline.json"))
	mode: argparse._MutuallyExclusiveGroup = parser.add_mutually_exclusive_group(required=True)
	mode.add_argument("--work-probe", type=Path)
	mode.add_argument("--timing-only", action="store_true")
	args: argparse.Namespace = parser.parse_args()
	formatter: str = str(args.formatter.resolve())
	probe: str = str(args.work_probe.resolve()) if args.work_probe else ""
	if args.generate_inputs and not probe:
		parser.error("--generate-inputs requires --work-probe to check every generated input")
	# CI と同じ原本の取得・照合を行い，古い入力・欠落を計測前に拒否する
	try:
		if args.capture_baseline:
			if not probe:
				parser.error("--capture-baseline requires --work-probe")
			capture_baseline(probe, args.inputs, args.capture_baseline, args.cache, args.generate_inputs)
			# 明示指定した基準の採取の終了
			return 0
		manifest: dict = json.loads((SOURCES / "manifest.json").read_bytes())
		originals: dict[str, list[bytes]] = verify_sources(manifest, args.cache)
		checked: dict[tuple[str, int], bytes] = (
			materialize_inputs(manifest, originals, args.inputs) if
			args.generate_inputs else
			verify_inputs(manifest, originals, args.inputs, SIZES if probe else (TARGET_LINES,))
		)
	except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as e:
		print(f"performance input error: {e}; generation must finish before measurement", file=sys.stderr)
		# 不完全な入力を計測しない失敗の返戻
		return 1
	rows: list[str] = [
		f"Environment: {args.environment}. PGO: {args.pgo}.",
		"Wall-clock times are informational; shared runner load varies. The under-one-second target is recorded separately from work checks.",
		"",
		"| Language | Input lines | Best wall time (s) | Under 1 s | Structure | Wrapping | Core limit per stage | Result |",
		"|---|---:|---:|---|---:|---:|---:|---|"
	]
	stages: list[str] = ["| Language | " + " | ".join(PHASES[2:]) + " |", "|---|" + "---:|" * len(PHASES[2:])]
	baseline: dict = json.loads(args.baseline.read_bytes()) if probe else {}
	failed: int = 0
	# 言語毎の正常な整形と作業予算の照合
	for lang in LANGUAGES:
		try:
			data: bytes = checked[lang, TARGET_LINES]
			lines: int = data.count(b"\n")
			if lines < TARGET_LINES:
				raise ValueError(f"{lang}: fewer than {TARGET_LINES} lines")
			counts: str = "not measured | not measured | not checked"
			if probe:
				work: dict[str, int] = verify_regression(baseline, probe, args.inputs, lang)
				counts = f"{work['structure']} | {work['wrapping']} | {work['limit']}"
				stages.append("| " + lang + " | " + " | ".join(str(work[phase]) for phase in PHASES[2:]) + " |")
			seconds: float = measure(formatter, data, lang)
			verdict: str = "yes" if seconds < 1 else "no"
			kind: str = (
				"work checks passed; time target missed" if
				probe and not seconds < 1 else
				"work checks passed; time target met" if probe else "timing only"
			)
			rows.append(f"| {lang} | {lines} | {seconds:.6f} | {verdict} | {counts} | {kind} |")
		except (OSError, subprocess.SubprocessError, ValueError, KeyError) as e:
			failed += 1
			print(str(e), file=sys.stderr)
			rows.append(f"| {lang} | — | — | — | — | — | — | failed |")
		print(rows[-1], flush=True)
	# 合否と実時間を分離した結果の保存
	args.report.parent.mkdir(parents=True, exist_ok=True)
	args.report.write_bytes(
		("\n".join(rows + (["", "Deterministic stage counters (10k lines)", "", *stages] if probe else [])) + "\n").encode("utf-8")
	)
	# 実時間の目標の未達で CI の合否を変えない終了状態の返戻
	return int(bool(failed))

if __name__ == "__main__":
	sys.exit(main())
