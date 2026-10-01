#! /usr/bin/env python3
# 基準出力の構文検査
# 入力が処理系の構文検査を通る物に限り，期待出力も通る事を確かめる
# 基準出力の比較は tree-sitter の構文木に依る為，解析器の読違で壊れた出力（Ruby の `p(not -x)` 等）を期待値として固定し得る
# 処理系の無い言語 (Java / C# / Rust) と HTML は対象外とし，手元に無い処理系の言語は飛ばす
# Kotlin は処理系が有っても対象外とする：`kotlinc` に構文だけを見る選択肢が無く，未定義の名前等の意味の誤りと区別出来ない
# （区別には出力の文面で濾す事に為るが，文面は版で変わり，濾せなく為った時に誤りが無いと読めて検査が黙って効かなく為る）
# 使い方：python3 tools/verify_golden_syntax.py [言語 ...]
# 言語を指定すると其の言語だけを検査する
from __future__ import annotations

import json

from pathlib import Path

import shutil
import subprocess
import sys
import tempfile

ROOT: Path = Path(__file__).resolve().parent.parent
GOLDEN: Path = ROOT / "test_data/golden"
# 言語毎の処理系の起動（`{}` は検査するファイル）
# 拡張子は処理系が言語を決める為に付ける
C_CHECK: list[str] = ["clang", "-fsyntax-only", "-x", "c", "-std=c17", "{}"]
CPP_CHECK: list[str] = ["clang", "-fsyntax-only", "-x", "c++", "-std=c++20", "{}"]

COMMANDS: dict[str, tuple[str, list[list[str]]]] = {
	"c": (".c", [C_CHECK]),
	"cpp": (".cpp", [CPP_CHECK]),
	"h": (".h", [C_CHECK, CPP_CHECK]),
	"go": (".go", [["gofmt", "-e", "{}"]]),
	"swift": (".swift", [["swiftc", "-parse", "{}"]]),
	"php": (".php", [["php", "-l", "{}"]]),
	"ruby": (".rb", [["ruby", "-c", "{}"], ["ruby", "--parser=parse.y", "-c", "{}"]]),
	"css": (".css", [["sass", "--no-source-map", "{}"]]),
	"scss": (".scss", [["sass", "--no-source-map", "{}"]])
}

# TypeScript の構文解析器で読む言語と其の種別（JSX を持てる JavaScript は JSX として読む）
SCRIPT_KINDS: dict[str, str] = { "javascript": "JSX", "typescript": "TS", "typescriptreact": "TSX" }

RUNNER: str = r"""
const ts = require(process.argv[1]);
const cases = JSON.parse(require("node:fs").readFileSync(0, "utf8"));
const results = cases.map(([name, kind, source]) => {
	const tree = ts.createSourceFile(name, source, ts.ScriptTarget.Latest, true, ts.ScriptKind[kind]);
	return tree.parseDiagnostics.map(d => ts.flattenDiagnosticMessageText(d.messageText, " "));
});
process.stdout.write(JSON.stringify(results));
"""

def cases(language: str) -> list[tuple[Path, Path]]:
	"""見送の無い基準入力と期待出力の組を集める
	計算量：対象ファイル数 F に対して O(F log F)
	引数：
		language: 対象言語
	戻値：
		入力と期待出力の組
	"""
	# 見送の物は期待出力が入力と同じ為に除いた組の返戻
	return [
		(source, source.with_suffix(".out")) for source in sorted((GOLDEN / language).glob("*.in")) if source.with_suffix(".out")
		.exists() and not source.with_suffix(".skip").exists()
	]

def is_available(command: list[str]) -> bool:
	"""処理系が手元に在り，其の選択肢を受付けるか判定する
	計算量：処理系の起動時間 T に対して O(T)
	引数：
		command: 処理系の起動引数
	戻値：
		利用可能か
	"""
	if not shutil.which(command[0]):
		# 処理系が無い事の返戻
		return False
	if command[0] != "ruby":
		# ruby 以外は処理系が在れば使える事の返戻
		return True
	# 空の原文で選択肢を試した結果の返戻
	return not subprocess.run([*command[:-1], "-e", ""], capture_output=True, timeout=120).returncode

def command_errors(command: list[str], text: bytes, suffix: str, directory: Path) -> str|None:
	"""原文を一時ファイルへ書いて処理系で検査する
	計算量：原文のバイト数 B・処理系の起動時間 T に対して O(B+T)
	引数：
		command: 処理系の起動引数
		text: 原文
		suffix: 一時ファイルの拡張子
		directory: 一時ディレクトリ
	戻値：
		構文誤りの内容（誤りが無ければ None）
	"""
	path: Path = directory / ("case" + suffix)
	path.write_bytes(text)
	result: subprocess.CompletedProcess[bytes] = subprocess.run(
		[str(path) if part == "{}" else part for part in command],
		capture_output=True,
		timeout=120
	)
	# 誤りが無ければ None，有れば処理系の出力の返戻
	return None if not result.returncode else (result.stderr or result.stdout).decode(errors="replace").strip()

def verify_commands(languages: list[str], failures: list[str]) -> int:
	"""処理系を起動する言語の出力を検査する
	計算量：対象検体数 F・処理系の最大起動時間 T に対して O(FT)
	引数：
		languages: 対象言語
		failures: 失敗の追記先
	戻値：
		検査した数
	"""
	# 処理系の利用可否と検体の準備
	checked: int = 0
	with tempfile.TemporaryDirectory(prefix="shaveformat-golden-syntax-") as temporary:
		directory: Path = Path(temporary)
		for language in languages:
			suffix: str
			commands: list[list[str]]
			suffix, commands = COMMANDS[language]
			usable: list[list[str]] = [command for command in commands if is_available(command)]
			for source, expected in cases(language):
				for command in usable:
					# 入力が構文検査を通る物だけの期待出力の検査
					if command_errors(command, source.read_bytes(), suffix, directory) is not None:
						continue
					checked += 1
					if (errors := command_errors(command, expected.read_bytes(), suffix, directory)) is not None:
						failures.append(f"{expected.relative_to(ROOT)} ({command[0]} {' '.join(command[1:-1])}): {errors.splitlines()[0]}")
	# 検査した組の数の返戻
	return checked

def verify_scripts(languages: list[str], failures: list[str]) -> int:
	"""TypeScript の構文解析器で JavaScript / TypeScript の出力を検査する
	計算量：対象検体数 F・構文解析の実行時間 T に対して O(F+T)
	引数：
		languages: 対象言語
		failures: 失敗の追記先
	戻値：
		検査した数
	"""
	# 構文解析器への入力と期待出力の集約
	typescript: Path = ROOT / "vscode-extension/node_modules/typescript"
	if not typescript.exists() or not shutil.which("node"):
		# 構文解析器が無ければ検査しない事の返戻
		return 0
	pairs: list[tuple[str, Path, Path]] = [
		(SCRIPT_KINDS[language], source, expected) for language in languages for source, expected in cases(language)
	]
	payload: list[list[str]] = []
	for kind, source, expected in pairs:
		# 各組は入力と期待出力を並べる
		payload += [[source.name, kind, source.read_text()], [str(expected.relative_to(ROOT)), kind, expected.read_bytes().decode()]]
	results: list[list[str]] = json.loads(
		subprocess.run(
			["node", "-e", RUNNER, str(typescript)],
			input=json.dumps(payload),
			capture_output=True,
			encoding="utf-8",
			check=True,
			timeout=600
		).stdout
	)
	checked: int = 0
	for (kind, _, expected), source_errors, errors in zip(pairs, results[0::2], results[1::2]):
		# 入力が構文検査を通る物だけの期待出力の検査
		if source_errors:
			continue
		checked += 1
		if errors:
			failures.append(f"{expected.relative_to(ROOT)} (typescript {kind}): {errors[0]}")
	# 検査した数の返戻
	return checked

def verify_python(failures: list[str]) -> int:
	"""Python の出力を此の処理系で翻訳して検査する
	計算量：対象検体の総バイト数 B に対して O(B)
	引数：
		failures: 失敗の追記先
	戻値：
		検査した数
	"""
	# Python の入力と期待出力の構文検査
	checked: int = 0
	for source, expected in cases("python"):
		try:
			compile(source.read_bytes(), str(source), "exec")
		except SyntaxError:
			continue
		checked += 1
		try:
			compile(expected.read_bytes(), str(expected), "exec")
		except SyntaxError as e:
			failures.append(f"{expected.relative_to(ROOT)} (python): {e.msg} (line {e.lineno})")
	# 検査した数の返戻
	return checked

def verify_json(failures: list[str]) -> int:
	"""JSON の出力を厳密な構文で検査する
	計算量：対象検体の総バイト数 B に対して O(B)
	引数：
		failures: 失敗の追記先
	戻値：
		検査した数
	"""
	# JSON の入力と期待出力の構文検査
	checked: int = 0
	for source, expected in cases("json"):
		try:
			json.loads(source.read_bytes())
		except ValueError:
			continue
		checked += 1
		try:
			json.loads(expected.read_bytes())
		except ValueError as e:
			failures.append(f"{expected.relative_to(ROOT)} (json): {e}")
	# 検査した数の返戻
	return checked

def main() -> int:
	"""指定の言語（既定は全て）の基準出力を検査する
	計算量：対象検体数 F・各検査の最大時間 T に対して O(FT)
	戻値：
		終了コード
	"""
	# 対象言語の指定の確認
	known: list[str] = [*COMMANDS, *SCRIPT_KINDS, "python", "json"]
	languages: list[str] = sys.argv[1:] or known
	if unknown := [language for language in languages if language not in known]:
		print("unknown language:", *unknown, file=sys.stderr)
		# 綴り誤りの言語を検査０件の成功と読ませない事の返戻
		return 2
	failures: list[str] = []
	# 利用可能な処理系への検査の振分
	checked: int = verify_commands([language for language in languages if language in COMMANDS], failures)
	checked += verify_scripts([language for language in languages if language in SCRIPT_KINDS], failures)
	if "python" in languages:
		checked += verify_python(failures)
	if "json" in languages:
		checked += verify_json(failures)
	# 違反と検査数の報告
	for failure in failures:
		print("FAIL", failure)
	print(f"golden syntax: {checked} checks, {len(failures)} failed")
	# 失敗の有無の返戻（処理系が１つも無く何も検査しなかった場合も失敗とする）
	return 1 if failures or not checked else 0

if __name__ == "__main__":
	sys.exit(main())
