#! /usr/bin/env python3
"""実バイナリの標準入力の待機中に中断信号を送り，応答を検査する"""

import argparse

from contextlib import contextmanager, redirect_stderr, redirect_stdout

import errno
import io
import os
import pathlib
import select
import signal
import subprocess
import sys
import tempfile
import time

from typing import Iterator

# パイプの準備・信号応答・例外時の回収の期限
PREPARATION_SECONDS: float = 3
SIGNAL_SECONDS: float = 0.5
CLEANUP_SECONDS: float = 5

@contextmanager
def child_process(
	command: list[str],
	is_shell: bool = False,
	has_input: bool = False,
	inherited_fds: tuple[int, ...] = (),
	environment: dict[str, str]|None = None
) -> Iterator[subprocess.Popen[bytes]]:
	"""検査専用の子を生成し，全終了経路で子とパイプを回収する
	引数：
		command: 起動するコマンド
		is_shell: 専用グループも回収するか
		has_input: 標準入力のパイプを作るか
		inherited_fds: 検査入口だけへ渡す同期用の記述子
		environment: 子へ渡す環境，`None` は呼出元の環境
	戻値：所有する子プロセスを１度渡すイテレータ
	送出例外：OSError / subprocess.SubprocessError: 起動・回収又は呼出元の実行が失敗する場合
	"""
	process: subprocess.Popen[bytes] = subprocess.Popen(
		command,
		stdin=subprocess.PIPE if has_input else None,
		stdout=subprocess.PIPE,
		stderr=subprocess.PIPE,
		bufsize=0,
		start_new_session=is_shell,
		pass_fds=inherited_fds,
		env=environment,
		pipesize=8192 if sys.platform == "linux" else -1
	)
	try:
		yield process
	finally:
		try:
			if is_shell:
				try:
					os.killpg(process.pid, signal.SIGKILL)
				except ProcessLookupError:
					pass
			elif process.poll() is None:
				process.kill()
			process.wait(timeout=CLEANUP_SECONDS)
		finally:
			for stream in process.stdin, process.stdout, process.stderr:
				if stream is not None:
					stream.close()

@contextmanager
def write_process(
	command: list[str],
	is_shell: bool,
	is_synchronized: bool
) -> Iterator[tuple[subprocess.Popen[bytes], int, int]]:
	"""検査入口の公開通知と読込待機のパイプを，子と共に所有する
	計算量：O(1)
	引数：command: 起動するコマンド，is_shell: 専用グループも回収するか，is_synchronized: 検査入口の同期を使うか
	戻値：子・公開通知の読取記述子・読込待機の解除記述子，同期を使わない記述子は `-1`
	送出例外：OSError / subprocess.SubprocessError: パイプ又は子の準備・回収に失敗する場合
	"""
	if not is_synchronized:
		with child_process(command, is_shell) as process:
			yield process, -1, -1
		# 同期を使わない入口の終了
		return
	descriptors: list[int] = []
	try:
		notify, notify_child = os.pipe()
		descriptors.extend((notify, notify_child))
		gate_child, gate = os.pipe()
		descriptors.extend((gate_child, gate))
		environment: dict[str, str] = os.environ.copy()
		environment["SHAVEFMT_TEST_WRITE_GATE"] = str(gate_child)
		environment["SHAVEFMT_TEST_WRITE_NOTIFY"] = str(notify_child)
		with child_process(command, is_shell, inherited_fds=(gate_child, notify_child), environment=environment) as process:
			for descriptor in gate_child, notify_child:
				os.close(descriptor)
				descriptors.remove(descriptor)
			yield process, notify, gate
	finally:
		for descriptor in descriptors:
			os.close(descriptor)

def write_input(process: subprocess.Popen[bytes], data: bytes, deadline: float) -> None:
	"""入力を非ブロックで渡し，読まない子への書込も期限で打ち切る
	計算量：入力のバイト数 B に対して O(B)
	引数：process: 所有する子，data: 渡すバイト列，deadline: monotonic 時刻の期限
	送出例外：OSError: パイプが無い又は書込が失敗する場合，subprocess.TimeoutExpired: 書込の期限を超える場合
	"""
	if process.stdin is None:
		raise OSError(errno.EIO, "missing input pipe")
	descriptor: int = process.stdin.fileno()
	os.set_blocking(descriptor, False)
	position: int = 0
	while position < len(data):
		remaining: float = deadline - time.monotonic()
		if remaining <= 0.0 or not select.select([], [descriptor], [], max(0, remaining))[1]:
			raise subprocess.TimeoutExpired(process.args, PREPARATION_SECONDS)
		try:
			position += os.write(descriptor, data[position:position + 8192])
		except BlockingIOError:
			continue
	# 全入力を渡した処理の終了
	return

def read_diff(process: subprocess.Popen[bytes], deadline: float) -> bytes:
	"""完了した最初の差分を非ブロックで読み，未終端の出力も期限で打ち切る
	計算量：差分のバイト数 B ≤ ６４ KiB に対して O(B)
	引数：process: 所有する子，deadline: monotonic 時刻の期限
	戻値：最初の書換を含む標準出力の接頭辞
	送出例外：OSError: パイプ又は差分が無い場合，subprocess.TimeoutExpired: 準備の期限を超える場合
	"""
	if process.stdout is None:
		raise OSError(errno.EIO, "missing output pipe")
	descriptor: int = process.stdout.fileno()
	os.set_blocking(descriptor, False)
	prefix: bytearray = bytearray()
	marker: bytes = b"-int  X;\n+int X;\n"
	while len(prefix) < 65536:
		remaining: float = deadline - time.monotonic()
		if remaining <= 0.0 or not select.select([descriptor], [], [], max(0, remaining))[0]:
			raise subprocess.TimeoutExpired(process.args, PREPARATION_SECONDS)
		try:
			part: bytes = os.read(descriptor, 65536 - len(prefix))
		except BlockingIOError:
			continue
		if not part:
			raise OSError(errno.EIO, "output ended before the complete diff")
		prefix.extend(part)
		start: int = max(0, len(prefix) - len(part) - len(marker) + 1)
		if prefix.find(marker, start) > -1:
			# 完了した最初の差分の返戻
			return bytes(prefix)
	raise OSError(errno.EIO, "the first diff exceeded the test limit")

def check_signal(binary: pathlib.Path, interrupted: int) -> bool:
	"""準備を含む期限内で信号を送り，既定の終了動作を照合する
	引数：binary: 標準入力の待機を検査する実バイナリのパス，interrupted: 子へ送る POSIX 信号
	戻値：準備が３秒以内で，信号送信後０．５秒以内に其の信号で終了した場合は真，起動・実行の失敗は偽
	"""
	deadline: float = time.monotonic() + PREPARATION_SECONDS
	try:
		with child_process([str(binary), "--stdin", "--lang", "cpp"], has_input=True) as process:
			write_input(process, b"//" + b"x" * 0X40000 + b"\n", deadline)
			time.sleep(0.05)
			if time.monotonic() >= deadline:
				raise subprocess.TimeoutExpired(process.args, PREPARATION_SECONDS)
			started: float = time.monotonic()
			process.send_signal(interrupted)
			output, errors = process.communicate(timeout=SIGNAL_SECONDS)
			elapsed: float = time.monotonic() - started
			passed: bool = process.returncode == -interrupted and elapsed < SIGNAL_SECONDS
			print(f"signal {interrupted}: code={process.returncode}, elapsed={elapsed:.3f}s")
			if not passed:
				print(f"unexpected completion: stdout={len(output)} bytes, stderr={errors!r}", file=sys.stderr)
	except (OSError, subprocess.SubprocessError) as error:
		print(f"FAIL signal {interrupted}: {error}", file=sys.stderr)
		# 準備又は中断の失敗の返戻
		return False
	# 信号応答の検査結果の返戻
	return passed

def check_diff(binary: pathlib.Path) -> bool:
	"""後続の解析中に中断しても，期限内で完了済の差分が残る事を照合する
	引数：binary: 差分出力を検査する実バイナリのパス
	戻値：準備が３秒以内で，SIGTERM 後０．５秒以内に終了し，完了済の差分を保持した場合は真，実行の失敗は偽
	送出例外：OSError: 一時ファイルの作成又は削除が失敗する場合
	"""
	with tempfile.TemporaryDirectory(prefix="shavefmt-diff-signal-") as directory:
		root: pathlib.Path = pathlib.Path(directory).resolve()
		first: pathlib.Path = root / "a.cpp"
		second: pathlib.Path = root / "b.cpp"
		first.write_bytes(b"int  X;\n")
		second.write_text("".join(f"int V{index} = {index};\n" for index in range(20000)), encoding="utf-8")
		deadline: float = time.monotonic() + PREPARATION_SECONDS
		try:
			with child_process([str(binary), "--diff", "--no-lint", str(first), str(second)]) as process:
				prefix: bytes = read_diff(process, deadline)
				started: float = time.monotonic()
				process.send_signal(signal.SIGTERM)
				completion: tuple[bytes, bytes] = process.communicate(timeout=SIGNAL_SECONDS)
				output: bytes = prefix + completion[0]
				passed: bool = (
					process.returncode == -signal.SIGTERM and time.monotonic() - started < SIGNAL_SECONDS and b"--- a/" in output and
					b"a.cpp\n" in output and b"-int  X;\n+int X;\n" in output
				)
				print(f"diff before SIGTERM: retained={passed}, code={process.returncode}")
		except (OSError, subprocess.SubprocessError) as error:
			print(f"FAIL diff preparation or interruption: {error}", file=sys.stderr)
			# 準備又は中断の失敗の返戻
			return False
	# 完了済の差分の保持の検査結果の返戻
	return passed

def check_write(binary: pathlib.Path, interrupted: int, is_shell: bool = False, is_synchronized: bool = False) -> bool:
	"""公開済の書戻と後始末を保ち，中断の理由・要約・信号及び呼出元の終了を照合する
	計算量：原文の総長 B と処理時間 T に対し O(B + T)
	引数：
		binary: 検査する実バイナリ
		interrupted: 子又は子の専用グループへ送る信号
		is_shell: bash の後続のコマンドが実行されない事も検査するか
		is_synchronized: 検査入口で残りの読込を信号の送信迄待機させるか
	戻値：全ての保持・通知・終了の条件を満たせば真，起動・実行・回収の失敗は偽
	送出例外：
		OSError: 検査対象の作成又は一時ディレクトリの削除に失敗する場合
	"""
	with tempfile.TemporaryDirectory(prefix="shavefmt-write-signal-") as directory:
		root: pathlib.Path = pathlib.Path(directory).resolve()
		first: pathlib.Path = root / "a.cpp"
		# 原文の大きさで最初に割り当てられ，構文は小さい１件を用意する（１スレッドでも公開を先に確認する）
		first.write_bytes(b"int  X;\n" + b" " * 0X100000)
		original: bytes = "".join(f"int V{index} = {index};\n" for index in range(30000)).encode()
		others: list[pathlib.Path] = [root / f"b{index}.cpp" for index in range(3)]
		for path in others:
			path.write_bytes(original)
		command: list[str] = [str(binary), "-w", "--no-lint", str(root)]
		if is_shell:
			command = ["bash", "-c", "\"$1\" -w --no-lint \"$2\"; printf \"continued\\n\" > \"$2/continued\"", "_", str(binary), str(root)]
		deadline: float = time.monotonic() + 10
		try:
			with write_process(command, is_shell, is_synchronized) as (process, notify, gate):
				# 送信時刻を固定せず，１件の公開を確認してから解析中の残りを中断する
				if is_synchronized:
					if not select.select([notify], [], [], max(0, deadline - time.monotonic()))[0] or os.read(notify, 1) != b"p":
						raise OSError(errno.EIO, "publication notification was not received")
				else:
					while process.poll() is None and first.read_bytes() != b"int X;\n" and time.monotonic() < deadline:
						time.sleep(0.005)
				published: bool = first.read_bytes() == b"int X;\n"
				started: float = time.monotonic()
				if process.poll() is None:
					if is_shell:
						# 新しいセッションに作った此の検査のグループだけへ，端末と同じ SIGINT を送る
						os.killpg(process.pid, interrupted)
					else:
						process.send_signal(interrupted)
					if is_synchronized:
						os.write(gate, b"p" * len(others))
				output, errors = process.communicate(timeout=3)
				elapsed: float = time.monotonic() - started
				# 文型は既存の未完了・原文保持の物を使い，理由は処理系の中断理由から独立して定める
				reason: bytes = os.strerror(errno.ECANCELED).encode()
				expected_errors: bytes = b"".join(
					b"Error: formatting could not be completed (" + reason + b"), left unchanged: " + str(path).encode() + b"\n" for path in others
				)
				expected_summary: bytes = b"Done. 1/4 file(s) formatted, 3 file(s) could not be processed.\n"
				kept: bool = first.read_bytes() == b"int X;\n" and all(path.read_bytes() == original for path in others)
				clean: bool = not list(root.glob(".shavefmt-*"))
				stopped: bool = not (root / "continued").exists()
				passed: bool = (
					published and process.returncode == -interrupted and elapsed < 3 and kept and clean and stopped and
					output == b"Formatted: " + str(first).encode() + b"\n" + expected_summary and errors == expected_errors
				)
				print(
					f"write signal {interrupted}: shell={is_shell}, passed={passed}, kept={kept}, clean={clean}, stopped={stopped}, "
					f"code={process.returncode}, elapsed={elapsed:.3f}s"
				)
				if not passed:
					print(f"unexpected write completion: stdout={output!r}, stderr={errors!r}", file=sys.stderr)
		except (OSError, subprocess.SubprocessError) as error:
			print(f"FAIL write preparation or interruption: {error}", file=sys.stderr)
			# 準備又は中断の失敗の返戻
			return False
	# 書戻と中断の契約を照合した返戻
	return passed

def self_check() -> list[str]:
	"""読まない入力・未終端の差分・例外の後始末と正常な準備を実際の子で照合する
	戻値：期限・終了状態・パイプの回収と違う条件
	送出例外：OSError: 一時実行ファイルを作成又は削除出来ない場合
	"""
	found: list[str] = []
	previous: float = PREPARATION_SECONDS
	globals()["PREPARATION_SECONDS"] = 1.5
	try:
		with tempfile.TemporaryDirectory(prefix="shavefmt-interruption-check-") as directory:
			for name, code, is_diff, expected in (
				("no_read", "import time;time.sleep(5)", False, False),
				("partial_diff", "import sys,time;sys.stdout.write('-');sys.stdout.flush();time.sleep(5)", True, False),
				("read_input", "import os\nwhile os.read(0,8192): pass", False, True),
				(
					"complete_diff",
					"import sys,time;sys.stdout.write('--- a/a.cpp\\n+++ b/a.cpp\\n-int  X;\\n+int X;\\n');sys.stdout.flush();time.sleep(5)",
					True,
					True
				)
			):
				binary: pathlib.Path = pathlib.Path(directory) / name
				binary.write_text("#!" + sys.executable + "\n" + code + "\n", encoding="utf-8")
				binary.chmod(0O700)
				started: float = time.monotonic()
				diagnostics: io.StringIO = io.StringIO()
				with redirect_stdout(diagnostics), redirect_stderr(diagnostics):
					result: bool = check_diff(binary) if is_diff else check_signal(binary, signal.SIGTERM)
				if result != expected or time.monotonic() - started > 3:
					found.append(f"準備の期限又は正常な経路が違う：{name}")
		failure: OSError = BrokenPipeError(errno.EPIPE, "injected pipe failure")
		try:
			with child_process([sys.executable, "-c", "import time;time.sleep(5)"], has_input=True) as process:
				raise failure
		except (OSError, subprocess.SubprocessError):
			pass
		if (
			process.poll() is None or
			any(stream is not None and not stream.closed for stream in (process.stdin, process.stdout, process.stderr))
		):
			found.append(f"例外後に子又はパイプを残した：{type(failure).__name__}")
	finally:
		globals()["PREPARATION_SECONDS"] = previous
	# 有限な準備と後始末の照合結果の返戻
	return found

def check_ignored_write(binary: pathlib.Path, interrupted: int) -> bool:
	"""背景ジョブと同じ無視を継承した書戻が，信号を受けても完了する事を照合する
	計算量：原文の総長 B と処理時間 T に対し O(B + T)
	引数：
		binary: 検査する実バイナリ
		interrupted: 起動時に無視し，公開後に送る信号
	戻値：信号を実際に送り，両対象の書戻と要約が完了すれば真
	送出例外：
		OSError: 検査対象の操作・子の起動・信号送信に失敗する場合
		subprocess.TimeoutExpired: 子が３５秒以内に終了しない場合
	"""
	def prepare() -> None:
		"""継承する無視と，此の検査の子だけの終了上限を設定する"""
		signal.signal(interrupted, signal.SIG_IGN)
		signal.alarm(30)
		# 終了
		return
	with tempfile.TemporaryDirectory(prefix="shavefmt-ignored-signal-") as directory:
		root: pathlib.Path = pathlib.Path(directory).resolve()
		first: pathlib.Path = root / "a.cpp"
		second: pathlib.Path = root / "b.cpp"
		first.write_bytes(b"int  X;\n" + b" " * 0X100000)
		original: bytes = "".join(f"int  V{index} = {index};\n" for index in range(30000)).encode()
		second.write_bytes(original)
		with subprocess.Popen(
			[str(binary), "-w", "--no-lint", str(root)],
			stdout=subprocess.PIPE,
			stderr=subprocess.PIPE,
			preexec_fn=prepare
		) as process:
			# 処理器の登録後である事を，１件の公開を確認してから照合する
			deadline: float = time.monotonic() + 10
			while process.poll() is None and first.read_bytes() != b"int X;\n" and time.monotonic() < deadline:
				time.sleep(0.005)
			sent: bool = process.poll() is None and first.read_bytes() == b"int X;\n"
			if sent:
				process.send_signal(interrupted)
			output, errors = process.communicate(timeout=35)
			expected: bytes = (
				b"Formatted: " + str(first).encode() + b"\nFormatted: " + str(second).encode() + b"\nDone. 2/2 file(s) formatted.\n"
			)
			passed: bool = (
				sent and not process.returncode and output == expected and not errors and first.read_bytes() == b"int X;\n" and
				second.read_bytes() != original and not list(root.glob(".shavefmt-*"))
			)
			print(f"ignored write signal {interrupted}: sent={sent}, passed={passed}, code={process.returncode}")
			if not passed:
				print(f"unexpected ignored completion: stdout={output!r}, stderr={errors!r}", file=sys.stderr)
	# 継承した無視と公開・後始末・要約の検査結果の返戻
	return passed

def main() -> int:
	"""読取専用と書戻の入口を，SIGINT と SIGTERM の双方で検査する
	戻値：全検査が合格した場合は０，不合格が在る場合は１
	Windows は POSIX 信号の対象外で，検査を実行せず０を返す
	送出例外：
		SystemExit: 必須の `--binary` が無い等，引数が不正な場合
		OSError: 一時ファイルの操作・子の起動・パイプの入出力・信号送信が失敗する場合
	"""
	parser: argparse.ArgumentParser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--binary", type=pathlib.Path)
	parser.add_argument("--write-binary", type=pathlib.Path)
	parser.add_argument("--self-check", action="store_true")
	args: argparse.Namespace = parser.parse_args()
	if args.binary is None and not args.self_check:
		parser.error("--binary is required")
	if sys.platform == "win32":
		print("POSIX signal verification is not available on Windows")
		# POSIX 信号の対象でない環境の返戻
		return 0
	problems: list[str] = self_check()
	for problem in problems:
		print(f"FAIL interruption self check: {problem}", file=sys.stderr)
	if args.self_check or problems:
		# 自己検査の終了値の返戻
		return bool(problems)
	binary: pathlib.Path = args.binary.resolve()
	write_binary: pathlib.Path = args.write_binary.resolve() if args.write_binary is not None else binary
	passed: bool = True
	for interrupted in signal.SIGINT, signal.SIGTERM:
		if not check_signal(binary, interrupted):
			passed = False
	if not check_diff(binary):
		passed = False
	for interrupted in signal.SIGINT, signal.SIGTERM:
		if not check_write(write_binary, interrupted, is_synchronized=args.write_binary is not None):
			passed = False
	if not check_write(write_binary, signal.SIGINT, True, args.write_binary is not None):
		passed = False
	for interrupted in signal.SIGINT, signal.SIGTERM:
		if not check_ignored_write(binary, interrupted):
			passed = False
	# 全信号の検査結果の返戻
	return 0 if passed else 1

if __name__ == "__main__":
	sys.exit(main())
