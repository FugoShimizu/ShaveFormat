#! /usr/bin/env python3
"""経路の分離・コメント非依存・空行の不変性の検査（整形仕様 1.2, 8.1, 10.5, 11.1）

整形器は検査用の指定の時だけ，中核の出力（コメントを置かない配置の字面）を `[layout]` の行（バイト毎の１６進）として標準エラーへ出す
此の観測は整形結果・検証・分類を変えない
コメントを変えた入力は此処で用意し，製品の同じ入口を通して中核の出力を比べる
各検体に就いて次を確かめる
中核工程へコメント節点が渡った不具合の失敗と，空行の不変性の違反が無い
中核の出力（全てのコメントを除いた入力）を入力にしても，同じ中核の出力に為る
独立した行コメントの本文を長くした入力でも中核の出力が変わらない
加えて，次の行に作用する指令・文書化の札・符号化宣言・整形器向の札を普通の注記と入替えた雛形で，中核の出力が一致し，整形結果は其れへ空でないコメントの行を足しただけである事を確かめる

使い方：
python3 tools/verify_comment_separation.py [整形器]
"""

from __future__ import annotations

from verify_case_variants import JOBS, OPENERS

import concurrent.futures
import difflib
import itertools
import os

from pathlib import Path

import subprocess
import sys

ROOT: Path = Path(__file__).resolve().parent.parent
GOLDEN: Path = ROOT / "test_data/golden"
ENVIRONMENT: dict[str, str] = dict(os.environ, SHAVEFMT_VERIFY_COMMENT_SEPARATION="1")
LAYOUT_PREFIX: bytes = b"[layout] "

# 行コメントの記号（本文を長くする変異に使う，行コメントを持つ言語だけ）
LINE_MARKERS: dict[str, bytes] = {
	language: openers[0].encode() for language, openers in OPENERS.items() if openers[0] in ("//", "#")
}

LINE_MARKERS["html"] = b"//"
# 本文へ足す語（行幅を超える長さでコードの折返がコメントに依らない事も確かめる）
SUFFIX: bytes = b" variant" * 20

# 行幅を超える本体（コメントが折返・インデントを動かさない事を確かめる，短い本体では折返が起きず検査を擦り抜ける）
LONG_JS: (
	str
) = "const value = computeSomethingWithAVeryDescriptiveName(1111111, 2222222, 3333333, 4444444, 5555555, 6666666, 7777777, 8888888, 999);"

LONG_C: (
	str
) = "int compute(int aaaaaaaa, int bbbbbbbb, int cccccccc, int dddddddd, int eeeeeeee, int ffffffff, int gggggggg) { return 0; }"

# 言語・本体の雛形（`{}` へコメントを差し込む）・差し込むコメントの並び
# 各注記列の先頭を基準の配置と比較する為に内側の順を保つ
CONTENT_GROUPS: tuple[tuple[str, str, tuple[str, ...]], ...] = (
	(
		"c",
		"{}\n#include <stdio.h>\nint   x;\nint   y;\n",
		("// note", "//go:build linux", "// +build linux", "//nolint:all", "//line 5 \"a.c\"", "/* note */", "/** doc */")
	),
	("c", "{}\n" + LONG_C.replace("{", "{{").replace("}", "}}") + "\n", ("// note", "/* note */", "//nolint:all", "/** doc */")),
	("cpp", "{}\n#include <cstdio>\nint   x;\n", ("// note", "//go:build linux", "// +build linux", "/** doc */")),
	("csharp", "{}\nclass C {{ int F() {{ return 1; }} }}\n", ("// note", "/// <summary>x</summary>", "// ReSharper disable All")),
	("java", "{}\nclass C {{ int f() {{ return 1; }} }}\n", ("// note", "/** doc */", "// CHECKSTYLE:OFF")),
	(
		"go",
		"{}\npackage p\n\nfunc f() int {{ return 1 }}\n",
		("// note", "//go:build linux", "// +build linux", "//go:embed x", "//go:generate cmd", "//nolint:errcheck")
	),
	("rust", "{}\nfn f() -> i32 {{ 1 }}\n", ("// note", "/// doc", "//! doc", "// rustfmt::skip")),
	("swift", "{}\nfunc f() -> Int {{ return 1 }}\n", ("// note", "/// doc", "// swiftlint:disable all")),
	("php", "<?php\n{}\n$a = 1;\n$b = 2;\n", ("// note", "/* note */", "/** doc */", "# note")),
	(
		"javascript",
		"{}\nconst a = 1;\nfunction f() {{ return a; }}\n",
		("// note", "// @ts-ignore", "/*#__PURE__*/", "/* webpackChunkName: \"x\" */")
	),
	(
		"javascript",
		"{}\n" + LONG_JS + "\n",
		("// note", "// eslint-disable-next-line no-magic-numbers", "/* note */", "/*#__PURE__*/", "// @ts-ignore")
	),
	(
		"typescript",
		"{}\nconst a = 1;\nfunction f() {{ return a; }}\n",
		("// note", "// @ts-ignore", "// @ts-expect-error", "/*#__PURE__*/", "/** @type {{number}} */", "// eslint-disable")
	),
	("typescript", "{}\n" + LONG_JS + "\n", ("// note", "// eslint-disable-next-line no-magic-numbers", "// @ts-expect-error")),
	("ruby", "{}\nx = 1\ny = 2\n", ("# note", "# frozen_string_literal: true", "# rubocop:disable all", "# encoding: utf-8")),
	("python", "{}\nx = 1\ny = 2\n", ("# note", "# type: ignore", "# noqa", "# -*- coding: utf-8 -*-", "# fmt: off"))
)

# 親の中核を比較する埋込の対（行末と独立したコメント，異なる所有区画）
EMBEDDED_GROUPS: tuple[tuple[bytes, bytes], ...] = (
	(b"<script>let a=1;\nconsole.log(a);</script>", "<script>let a=1; // 説明\nconsole.log(a);</script>".encode()),
	(b"<script>let a=1;\nconsole.log(a);</script>", b"<script>let a=1;\n// note\nconsole.log(a);</script>"),
	(b"<script></script>", b"<script>// note</script>"),
	(b"<style>.a{color:red;}</style>", b"<style>.a{/* note */color:red;}</style>")
)

def observe(binary: str, language: str, text: bytes, width: list[str]) -> tuple[int, bytes, bytes, str]:
	"""検体を整形し，終了コード・整形結果・中核の出力・標準エラーを返す
	計算量：入力のバイト数 B・整形器の実行時間 T に対して O(B+T)
	"""
	# 中核出力を含む整形結果の取得
	command: list[str] = [binary, "--stdin", "--lang", language, "--no-lint", *width]
	result: subprocess.CompletedProcess[bytes] = subprocess.run(
		command,
		input=text,
		capture_output=True,
		timeout=300,
		env=ENVIRONMENT
	)
	# 通知からの中核出力の抽出
	lines: list[bytes] = [
		bytes.fromhex(line[len(LAYOUT_PREFIX):].decode()) for line in result.stderr.split(b"\n") if line.startswith(LAYOUT_PREFIX)
	]
	# 観測の返戻
	return result.returncode, result.stdout, b"".join(line + b"\n" for line in lines), result.stderr.decode(errors="replace")

def lengthen_comments(output: bytes, layout: bytes, marker: bytes) -> bytes|None:
	"""整形結果の独立した行コメントの本文を長くした入力を返す（該当する行が無ければ空）
	計算量：出力行数 L に対して最悪 O(L²)
	"""
	# 整形出力と中核出力の行の対応付け
	lines: list[bytes] = output.split(b"\n")
	code: list[bytes] = layout.split(b"\n")
	changed: bool = False
	for tag, first, last, low, high in difflib.SequenceMatcher(None, code, lines, autojunk=False).get_opcodes():
		if tag == "replace" and last - first == high - low:
			for offset in range(high - low):
				original: bytes = code[first + offset].rstrip(b" \t")
				current: bytes = lines[low + offset]
				if current.startswith(original) and current[len(original):].lstrip(b" \t").startswith(marker) and not current.endswith(b"\\"):
					lines[low + offset] += SUFFIX
					changed = True
		if tag != "insert":
			continue
		for index in range(low, high):
			# 行末の逆斜線は次の行を繋ぐ為に本文を足さない
			if lines[index].lstrip(b" \t").startswith(marker) and not lines[index].endswith(b"\\"):
				lines[index] += SUFFIX
				changed = True
	# 変異した入力の返戻
	return b"\n".join(lines) if changed else None

def inspect(binary: str, language: str, source: Path) -> list[str]:
	"""１つの検体の違反の並びを返す
	計算量：検体のバイト数 B・整形器の総実行時間 T に対して O(B+T)
	"""
	# 検体の行幅と初回観測の取得
	chars: Path = source.with_suffix(".chars")
	width: list[str] = ["--chars", chars.read_text().strip()] if chars.exists() else []
	status: int
	output: bytes
	layout: bytes
	errors: str
	status, output, layout, errors = observe(binary, language, source.read_bytes(), width)
	problems: list[str] = []
	# 中核へのコメント混入と見送の確認
	if "comment nodes reach the core passes" in errors:
		problems.append("comment nodes reach the core passes")
	if "formatting would change blank lines" in errors:
		problems.append("comment placement changes blank lines")
	if status:
		problems.append(f"the formatter exited with status {status}")
	if "[skip]" in errors:
		problems.append("unexpected skip")
	if problems:
		# 呼出側が期待する見送・失敗の検体を除いて居る為，此処へ到達した失敗を違反とする一覧の返戻
		return problems
	# コメントを含まない出力は除く物が無く，其の再整形は基準検体の二巡目の検査と同じ為に省く
	if layout != output:
		stripped_status: int
		stripped_layout: bytes
		stripped_errors: str
		stripped_status, _, stripped_layout, stripped_errors = observe(binary, language, layout, width)
		if stripped_status or "[skip]" in stripped_errors:
			problems.append("the comment-free input was not formatted")
		elif stripped_layout != layout:
			problems.append("removing the comments changes the code")
	# 独立コメントの本文変更による配置の照合
	marker: bytes|None
	variant: bytes|None
	if (marker := LINE_MARKERS.get(language)) and (variant := lengthen_comments(output, layout, marker)) is not None:
		variant_status: int
		variant_layout: bytes
		variant_errors: str
		variant_status, _, variant_layout, variant_errors = observe(binary, language, variant, width)
		if variant_status or "[skip]" in variant_errors:
			problems.append("the comment variant was not formatted")
		elif variant_layout != layout:
			problems.append("comment content changes the code")
	# 違反の返戻
	return problems

def content_problems(binary: str) -> tuple[int, int, list[str]]:
	"""内容だけが違うコメントを雛形へ差し込み，比べた数と見送の数と違反の並びを返す
	計算量：雛形の総文字数 B・整形器の総実行時間 T に対して O(B+T)
	"""
	# 雛形とコメントの組合せの観測
	checked: int = 0
	skipped: int = 0
	problems: list[str] = []
	for language, body, texts in CONTENT_GROUPS:
		base: list[str]|None = None
		for text in texts:
			checked += 1
			status: int
			output: bytes
			layout: bytes
			errors: str
			status, output, layout, errors = observe(binary, language, body.format(text).encode(), [])
			# 見送は整形の拒否で，整形結果の変動では無い為に別に数える
			if "[skip]" in errors:
				skipped += 1
				continue
			if status:
				problems.append(f"{language}: {text}: the formatter exited with status {status}")
				continue
			code: list[str]
			normal: list[str]
			code, normal = layout.decode(errors="surrogateescape").split("\n"), output.decode(errors="surrogateescape").split("\n")
			base = base or code
			# コメントの内容で配置が変わらない事（中核の出力は指示コメントも含めて全てのコメントを置かない，整形仕様 10.5）
			if code != base:
				problems.append(f"{language}: {text}: layout differs from the first variant")
			# 通常の出力は配置の行を変えず，空でないコメントの行だけを足す事
			for tag, first, last, low, high in difflib.SequenceMatcher(None, code, normal, autojunk=False).get_opcodes():
				if tag != "equal" and (tag != "insert" or any(not line.strip() for line in normal[low:high])):
					problems.append(f"{language}: {text}: output {tag}: {code[first:last]!r} -> {normal[low:high]!r}")
	# 比べた数と見送の数と違反の返戻
	return checked, skipped, problems

def embedded_problems(binary: str) -> list[str]:
	"""埋込のコメントだけを変えた対の親の中核出力を照合する
	計算量：検体数 F・整形器の最大実行時間 T に対して O(FT)
	"""
	# 子の行末・独立・文中のコメントを親の観測へ混ぜない事の照合
	problems: list[str] = []
	for index, (plain, commented) in enumerate(EMBEDDED_GROUPS, 1):
		first_status, _, first_layout, first_errors = observe(binary, "html", plain, [])
		second_status, _, second_layout, second_errors = observe(binary, "html", commented, [])
		if first_status or second_status or "[skip]" in first_errors or "[skip]" in second_errors:
			problems.append(f"embedded {index}: a variant was not formatted")
		elif first_layout != second_layout:
			problems.append(f"embedded {index}: child comments change the parent layout")
	# 違反の返戻
	return problems

def main() -> int:
	"""検体の中核の出力を比べ，違反が有れば非零で終える
	計算量：検体数 F・整形器の最大実行時間 T に対して O(FT+F log F)
	"""
	# 基準検体と整形器の準備
	binary: str = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "build/shavefmt")
	sources: list[Path] = [
		source for directory in sorted(path for path in GOLDEN.iterdir() if path.is_dir()) for source in
		sorted(directory.glob("*.in")) if not source.with_suffix(".fail").exists() and not source.with_suffix(".skip").exists()
	]
	checked: int = len(sources)
	failures: list[str] = []
	# 検体毎の観測は互いに独立な為，同時に走らせる工程の数 (`SHAVEFMT_JOBS`) で分ける（結果は検体の順に集める）
	with concurrent.futures.ProcessPoolExecutor(max_workers=JOBS) as pool:
		for source, problems in zip(
			sources,
			pool.map(inspect, itertools.repeat(binary), [source.parent.name for source in sources], sources, chunksize=8)
		):
			failures += [f"{source.relative_to(ROOT)}: {problem}" for problem in problems]
	# 検体の違反の報告
	for failure in failures:
		print(f"FAIL comment separation: {failure}", file=sys.stderr)
	print(f"comment separation: {checked} cases, {len(failures)} failed")
	# 内容差の検体の照合と報告
	contents: int
	skipped: int
	problems: list[str]
	contents, skipped, problems = content_problems(binary)
	problems += embedded_problems(binary)
	contents += len(EMBEDDED_GROUPS)
	for problem in problems:
		print(f"FAIL comment content: {problem}", file=sys.stderr)
	print(f"comment content: {contents} cases, {len(problems)} failed, {skipped} skipped")
	# 違反が有れば非零の返戻
	return 1 if failures or problems else 0

if __name__ == "__main__":
	sys.exit(main())
