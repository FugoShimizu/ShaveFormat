#! /usr/bin/env python3
# 基準出力の構文検査
# 入力が処理系の構文検査を通る物に限り，期待出力も通る事を確かめる
# 基準出力の比較は tree-sitter の構文木に依る為，パーサーの読違で壊れた出力（Ruby の `p(not -x)` 等）を期待値として固定し得る
# 処理系の無い言語 (Java / C# / Rust) と HTML は対象外とし，手元に無い処理系の言語は飛ばす
# Kotlin は処理系が有っても対象外とする：`kotlinc` に構文だけを見る選択肢が無く，未定義の名前等の意味のエラーと区別出来ない
# （区別には出力の文面で濾す事に為るが，文面はバージョンで変わり，濾せなく為った時にエラーが無いと読めて検査が黙って効かなく為る）
# 使方：`python3 tools/verify_golden_syntax.py [言語 ...]`
# 言語を指定すると其の言語だけを検査する
from __future__ import annotations

import hashlib
import json
import os

from pathlib import Path

import shutil
import subprocess
import sys
import tempfile

from collections.abc import Iterator

import concurrent.futures

# 検査の経路と実行設定
ROOT: Path = Path(__file__).resolve().parent.parent
GOLDEN: Path = ROOT / "test_data/golden"
# 同時に走らせる処理系の数（`tools/golden_test.sh` と同じ `SHAVEFMT_JOBS`，既定・上限は２）
JOBS: int = min(max(int(os.environ.get("SHAVEFMT_JOBS", "2")), 1), 2)
# 言語毎の処理系の起動（`{}` は検査するファイル）
# 拡張子は処理系が言語を決める為に付ける
C_CHECK: list[str] = ["clang", "-fsyntax-only", "-x", "c", "-std=c17", "{}"]
CPP_CHECK: list[str] = ["clang", "-fsyntax-only", "-x", "c++", "-std=c++20", "{}"]

# 言語毎のテストケースの拡張子と構文の照合に使う処理系
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

# TypeScript のパーサーで読む言語と其の種別（JSX を持てる JavaScript は JSX として読む）
SCRIPT_KINDS: dict[str, str] = { "javascript": "JSX", "javascriptreact": "JSX", "typescript": "TS", "typescriptreact": "TSX" }

# テストケース毎の TypeScript の構文診断を返す子プロセス
RUNNER: str = r"""
const ts = require(process.argv[1]);
const cases = JSON.parse(require("node:fs").readFileSync(0, "utf8"));
const results = cases.map(([name, kind, source]) => {
	const tree = ts.createSourceFile(name, source, ts.ScriptTarget.Latest, true, ts.ScriptKind[kind]);
	return tree.parseDiagnostics.map(d => ts.flattenDiagnosticMessageText(d.messageText, " "));
});
process.stdout.write(JSON.stringify(results));
"""

def cases(lang: str) -> list[tuple[Path, Path]]:
	"""見送の無い基準入力と期待出力の組を集める
	計算量：対象ファイル数 F に対して O(F log F)
	引数：
		lang: 対象言語
	戻値：
		入力と期待出力の組
	"""
	# 見送の物は期待出力が入力と同じ為に除いた組の返戻
	return [
		(src, src.with_suffix(".out")) for src in sorted((GOLDEN / lang).glob("*.in")) if src.with_suffix(".out").exists() and
		not src.with_suffix(".skip").exists()
	]

def is_available(command: list[str]) -> bool:
	"""処理系が手元に在り，其の選択肢を受付けるか判定する
	計算量：処理系の起動時間 T に対して O(T)
	引数：
		command: 処理系の起動引数
	戻値：
		利用可能か
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		subprocess.TimeoutExpired: 呼び出した子プロセスが制限時間内に完了しない場合
	"""
	if not shutil.which(command[0]):
		# 処理系が無い事の返戻
		return False
	if command[0] != "ruby":
		# ruby 以外は処理系が在れば使える事の返戻
		return True
	# 空の原文で選択肢を試した結果の返戻
	return not subprocess.run([*command[:-1], "-e", ""], capture_output=True, timeout=120).returncode

def command_identity(command: list[str]) -> str:
	"""処理系の実体とバージョンを識別する文字列を求める（処理系を更新した後に古いキャッシュを使わない為）
	計算量：処理系の起動時間 T に対して O(T)
	引数：
		command: 処理系の起動引数
	戻値：
		実体のパス・大きさ・更新日時・バージョンの表示と起動引数を繋げた文字列
	送出例外：
		OSError: 処理系の情報の取得又は起動が失敗する場合
		subprocess.TimeoutExpired: 呼び出した子プロセスが制限時間内に完了しない場合
	"""
	path: str = os.path.realpath(shutil.which(command[0]) or command[0])
	status: os.stat_result = os.stat(path)
	version: subprocess.CompletedProcess[bytes] = subprocess.run([command[0], "--version"], capture_output=True, timeout=120)
	shown: str = (version.stdout + version.stderr).decode(errors="replace")
	# 実体・バージョン・起動引数を繋げた識別の返戻
	return "\0".join([path, str(status.st_size), str(status.st_mtime_ns), shown, *command])

def cache_key(identity: str, suffix: str, text: bytes) -> str:
	"""処理系と検査するテキストの組のキャッシュのキーを求める
	計算量：テキストのバイト数 B に対して O(B)
	引数：
		identity: 処理系の識別
		suffix: 一時ファイルの拡張子
		text: 検査するテキスト
	戻値：
		SHA-256 の16進表記
	"""
	# 識別・拡張子・テキストの組の要約の返戻
	return hashlib.sha256(identity.encode() + b"\0" + suffix.encode() + b"\0" + text).hexdigest()

def command_errors(path: Path, command: list[str], text: bytes) -> str|None:
	"""原文を一時ファイルへ書いて処理系で検査する
	計算量：原文のバイト数 B・処理系の起動時間 T に対して O(B+T)
	引数：
		path: 一時ファイル（処理系が言語を決める拡張子を付け，同時に走らせる検査毎に別の名前にする）
		command: 処理系の起動引数
		text: 原文
	戻値：
		構文エラーの内容（エラーが無ければ None）
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		subprocess.TimeoutExpired: 呼び出した子プロセスが制限時間内に完了しない場合
	"""
	# 構文を照合するテストケースの書込
	path.write_bytes(text)
	result: subprocess.CompletedProcess[bytes] = (
		subprocess.run([str(path) if part == "{}" else part for part in command], capture_output=True, timeout=120)
	)
	# エラーが無ければ None，有れば処理系の出力の返戻
	return None if not result.returncode else (result.stderr or result.stdout).decode(errors="replace").strip()

def verify_commands(langs: list[str], failures: list[str]) -> int:
	"""処理系を起動する言語の出力を検査する（同じ実行内で同じ処理系とテキストの結果だけを共有する）
	計算量：対象テストケース数 F・テキストの総バイト数 B・異なるテキストの数 U・処理系の最大起動時間 T に対して O(F+B+UT)
	引数：
		langs: 対象言語
		failures: 失敗の追記先
	戻値：
		検査した数
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		subprocess.TimeoutExpired: 呼び出した子プロセスが制限時間内に完了しない場合
	"""
	# 処理系の利用可否とテストケース毎の入力・期待出力のキー
	tasks: list[tuple[Path, list[str], str, str]] = []
	texts: dict[str, tuple[list[str], str, bytes]] = {}
	for lang in langs:
		suffix: str
		commands: list[list[str]]
		suffix, commands = COMMANDS[lang]
		usable: list[tuple[list[str], str]] = [(command, command_identity(command)) for command in commands if is_available(command)]
		for src, expected in cases(lang):
			src_text: bytes = src.read_bytes()
			expected_text: bytes = expected.read_bytes()
			for command, identity in usable:
				src_key: str = cache_key(identity, suffix, src_text)
				expected_key: str = cache_key(identity, suffix, expected_text)
				texts.setdefault(src_key, (command, suffix, src_text))
				texts.setdefault(expected_key, (command, suffix, expected_text))
				tasks.append((expected, command, src_key, expected_key))
	# SDK・標準ヘッダ・取込先の依存を再確認する為，前回の処理系の結果は読まない
	results: dict[str, str|None] = {}
	pending: list[str] = list(texts)
	with tempfile.TemporaryDirectory(prefix="shaveformat-golden-syntax-") as temp:
		paths: list[Path] = [Path(temp) / f"case{index}{texts[key][1]}" for index, key in enumerate(pending)]
		with concurrent.futures.ThreadPoolExecutor(max_workers=JOBS) as pool:
			found: Iterator[str|None] = (
				pool.map(command_errors, paths, [texts[key][0] for key in pending], [texts[key][2] for key in pending])
			)
			results.update(zip(pending, found))
	checked: int = 0
	for expected, command, src_key, expected_key in tasks:
		# 入力が構文検査を通る物だけの期待出力の検査
		if results[src_key] is not None:
			continue
		checked += 1
		if (errors := results[expected_key]) is not None:
			failures.append(f'{expected.relative_to(ROOT)} ({command[0]} {" ".join(command[1:-1])}): {errors.splitlines()[0]}')
	# 検査した組の数の返戻
	return checked

def verify_scripts(langs: list[str], failures: list[str]) -> int:
	"""TypeScript のパーサーで JavaScript / TypeScript の出力を検査する
	計算量：対象テストケース数 F・構文解析の実行時間 T に対して O(F+T)
	引数：
		langs: 対象言語
		failures: 失敗の追記先
	戻値：
		検査した数
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		UnicodeDecodeError: 読み込んだファイルを指定の文字コードで解釈出来ない場合
		json.JSONDecodeError: 読み込んだ内容が有効な JSON でない場合
		subprocess.TimeoutExpired: 呼び出した子プロセスが制限時間内に完了しない場合
	"""
	# パーサーへの入力と期待出力の集約
	typescript: Path = ROOT / "vscode-extension/node_modules/typescript"
	if not typescript.exists() or not shutil.which("node"):
		# パーサーが無ければ検査しない事の返戻
		return 0
	pairs: list[tuple[str, Path, Path]] = [(SCRIPT_KINDS[lang], src, expected) for lang in langs for src, expected in cases(lang)]
	payload: list[list[str]] = []
	for kind, src, expected in pairs:
		# 各組は入力と期待出力を並べる
		payload += [[src.name, kind, src.read_text()], [str(expected.relative_to(ROOT)), kind, expected.read_bytes().decode()]]
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
	for (kind, _, expected), src_errors, errors in zip(pairs, results[0::2], results[1::2]):
		# 入力が構文検査を通る物だけの期待出力の検査
		if src_errors:
			continue
		checked += 1
		if errors:
			failures.append(f"{expected.relative_to(ROOT)} (typescript {kind}): {errors[0]}")
	# 検査した数の返戻
	return checked

def verify_python(failures: list[str]) -> int:
	"""Python の出力を此の処理系で翻訳して検査する
	計算量：対象テストケースの総バイト数 B に対して O(B)
	引数：
		failures: 失敗の追記先
	戻値：
		検査した数
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
	"""
	# Python の入力と期待出力の構文検査
	checked: int = 0
	for src, expected in cases("python"):
		try:
			compile(src.read_bytes(), str(src), "exec")
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
	計算量：対象テストケースの総バイト数 B に対して O(B)
	引数：
		failures: 失敗の追記先
	戻値：
		検査した数
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
	"""
	# JSON の入力と期待出力の構文検査
	checked: int = 0
	for src, expected in cases("json"):
		try:
			json.loads(src.read_bytes())
		except ValueError:
			continue
		checked += 1
		try:
			json.loads(expected.read_bytes())
		except ValueError as e:
			failures.append(f"{expected.relative_to(ROOT)} (json): {e}")
	# 検査した数の返戻
	return checked

def known_languages() -> list[str]:
	"""利用可能な検査の対象を固定の言語順で返す
	戻値：各検査が対応する言語キーの新しい一覧
	"""
	return [
		"c",
		"cpp",
		"h",
		"go",
		"swift",
		"php",
		"javascript",
		"javascriptreact",
		"typescript",
		"typescriptreact",
		"ruby",
		"python",
		"json",
		"css",
		"scss"
	]

def main() -> int:
	"""指定の言語（既定は全て）の基準出力を検査する
	計算量：対象テストケース数 F・各検査の最大時間 T に対して O(FT)
	戻値：
		終了コード
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		UnicodeDecodeError: 読み込んだファイルを指定の文字コードで解釈出来ない場合
		json.JSONDecodeError: 読み込んだ内容が有効な JSON でない場合
		subprocess.TimeoutExpired: 呼び出した子プロセスが制限時間内に完了しない場合
	"""
	# 対象言語の指定の確認
	known: list[str] = known_languages()
	langs: list[str] = sys.argv[1:] or known
	if unknown := [lang for lang in langs if lang not in known]:
		print("unknown language:", *unknown, file=sys.stderr)
		# 綴エラーの言語を検査０件の成功と読ませない事の返戻
		return 2
	failures: list[str] = []
	# 利用可能な処理系への検査の振分
	checked: int = verify_commands([lang for lang in langs if lang in COMMANDS], failures)
	checked += verify_scripts([lang for lang in langs if lang in SCRIPT_KINDS], failures)
	if "python" in langs:
		checked += verify_python(failures)
	if "json" in langs:
		checked += verify_json(failures)
	# 違反と検査数の報告
	for failure in failures:
		print("FAIL", failure)
	print(f"golden syntax: {checked} checks, {len(failures)} failed")
	# 失敗の有無の返戻（処理系が１つも無く何も検査しなかった場合も失敗とする）
	return 1 if failures or not checked else 0

if __name__ == "__main__":
	sys.exit(main())
