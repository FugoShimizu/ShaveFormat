#! /usr/bin/env python3
"""バージョンを固定した外部原本を取得・照合し，目録の範囲だけから性能入力を作る

使方：python3 tools/build_10k.py <入力の保存先> --formatter <実行ファイル>
原本はリポジトリ外のキャッシュへ置き，欠落・変更・不受理では生成を完了しない
返った整形結果は入力・期待へ使わない
"""

from __future__ import annotations

import argparse
import ctypes

from ctypes import wintypes

import hashlib
import io
import json
import os

from pathlib import Path, PureWindowsPath

import re
import secrets
import stat
import subprocess
import sys

from tempfile import NamedTemporaryFile, TemporaryDirectory

import time

from typing import Any, Callable
from unittest.mock import patch
from urllib.error import HTTPError, URLError
from urllib.parse import SplitResult, urlsplit
from urllib.request import Request, urlopen

# Windows の標準モジュールだけを条件付きで取り込む
if os.name == "nt":
	import msvcrt

# 言語の報告順と測定する規模
LANGUAGES: tuple[str, ...] = (
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
	"ruby",
	"python",
	"json",
	"html",
	"css",
	"scss"
)

SIZES: tuple[int, ...] = 5000, 10000, 20000
TARGET_LINES: int = 10000
METRIC_VERSION: int = 3
SOURCES: Path = Path(__file__).resolve().parent / "perf_sources"
CACHE: Path = Path.home() / ".cache" / "ShaveFormat" / "perf-sources"
MAX_SOURCE_BYTES: int = 0X2000000 # 取得する原本１件の受信上限
DOWNLOAD_RETRY_DELAYS: tuple[int, ...] = 1, 2 # 再取得前の待機秒数，最後の失敗後は待機しない

# Windows の固定 API は利用者毎の処理を始める前に１度だけ定義する
if os.name == "nt":
	WINDOWS_SECURITY: ctypes.CDLL = ctypes.WinDLL("advapi32", use_last_error=True)
	WINDOWS_KERNEL: ctypes.CDLL = ctypes.WinDLL("kernel32", use_last_error=True)
	pointer: type[ctypes.c_void_p] = ctypes.c_void_p
	for name, result, args in (
		("EqualSid", wintypes.BOOL, (pointer, pointer)),
		("GetAce", wintypes.BOOL, (pointer, wintypes.DWORD, ctypes.POINTER(pointer))),
		(
			"GetSecurityInfo",
			wintypes.DWORD,
			(
				wintypes.HANDLE,
				wintypes.DWORD,
				wintypes.DWORD,
				ctypes.POINTER(pointer),
				pointer,
				ctypes.POINTER(pointer),
				pointer,
				ctypes.POINTER(pointer)
			)
		),
		(
			"GetTokenInformation",
			wintypes.BOOL,
			(wintypes.HANDLE, wintypes.DWORD, pointer, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD))
		),
		("IsWellKnownSid", wintypes.BOOL, (pointer, wintypes.DWORD)),
		("OpenProcessToken", wintypes.BOOL, (wintypes.HANDLE, wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE)))
	):
		function: Any = getattr(WINDOWS_SECURITY, name)
		function.restype, function.argtypes = result, args
	for name, result, args in (
		("CloseHandle", wintypes.BOOL, (wintypes.HANDLE,)),
		(
			"CreateFileW",
			wintypes.HANDLE,
			(wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, pointer, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
		),
		("GetCurrentProcess", wintypes.HANDLE, ()),
		("LocalFree", pointer, (pointer,))
	):
		function: Any = getattr(WINDOWS_KERNEL, name)
		function.restype, function.argtypes = result, args

def verify_windows_cache_access(fd: int) -> None:
	"""Windows の所有者と DACL を，開いたキャッシュのハンドルで確認する
	利用者本人・SYSTEM・管理者以外に許可した ACL は拒否する
	計算量：ACE 数 A に対して O(A)
	引数：fd: 検査するディレクトリ又は通常ファイル
	送出例外：OSError / ValueError: 所有者・権限を確認出来ない場合
	"""
	token: wintypes.HANDLE = wintypes.HANDLE()
	descriptor: ctypes.c_void_p = pointer()
	owner: ctypes.c_void_p = pointer()
	acl: ctypes.c_void_p = pointer()
	# TOKEN_QUERY で現在の利用者の SID を得る
	if not WINDOWS_SECURITY.OpenProcessToken(WINDOWS_KERNEL.GetCurrentProcess(), 0X8, ctypes.byref(token)):
		raise ctypes.WinError(ctypes.get_last_error())
	try:
		size: wintypes.DWORD = wintypes.DWORD()
		WINDOWS_SECURITY.GetTokenInformation(token, 1, None, 0, ctypes.byref(size)) # TokenUser
		if not size.value:
			raise ctypes.WinError(ctypes.get_last_error())
		user: Any = ctypes.create_string_buffer(size.value)
		if not WINDOWS_SECURITY.GetTokenInformation(token, 1, user, size, ctypes.byref(size)):
			raise ctypes.WinError(ctypes.get_last_error())
		user_sid: int = ctypes.cast(user, ctypes.POINTER(pointer))[0]
		# SE_FILE_OBJECT と OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION
		error: int = WINDOWS_SECURITY.GetSecurityInfo(
			msvcrt.get_osfhandle(fd),
			1,
			0X5,
			ctypes.byref(owner),
			None,
			ctypes.byref(acl),
			None,
			ctypes.byref(descriptor)
		)
		if error:
			raise ctypes.WinError(error)
		# 管理者として作成した場合の既定所有者も確認する
		if not owner or not (WINDOWS_SECURITY.EqualSid(owner, user_sid) or WINDOWS_SECURITY.IsWellKnownSid(owner, 26)) or not acl:
			raise ValueError("cache owner or DACL is not private; choose a new private --cache directory")
		# ACL の AceCount と ACCESS_ALLOWED_ACE の SidStart は Windows の定義に従う
		count: int = ctypes.c_ushort.from_address(acl.value + 4).value
		for idx in range(count):
			ace: ctypes.c_void_p = pointer()
			if not WINDOWS_SECURITY.GetAce(acl, idx, ctypes.byref(ace)):
				raise ctypes.WinError(ctypes.get_last_error())
			kind: int = ctypes.c_ubyte.from_address(ace.value).value
			if kind == 1: # ACCESS_DENIED_ACE_TYPE は許可を増やさない
				continue
			if kind:
				raise ValueError("cache has an unsupported access rule; choose a new private --cache directory")
			sid: ctypes.c_void_p = pointer(ace.value + 8)
			# Python が非公開のディレクトリへ付ける OWNER RIGHTS は，確認済の所有者だけに適用する
			if not (
				WINDOWS_SECURITY.EqualSid(sid, user_sid) or WINDOWS_SECURITY.IsWellKnownSid(sid, 22) or
				WINDOWS_SECURITY.IsWellKnownSid(sid, 26) or WINDOWS_SECURITY.IsWellKnownSid(sid, 71)
			):
				raise ValueError("cache permits another user; choose a new private --cache directory")
	finally:
		if descriptor:
			WINDOWS_KERNEL.LocalFree(descriptor)
		WINDOWS_KERNEL.CloseHandle(token)
	# 確認の終了
	return

def open_cache_entry(directory: int|None, path: Path, kind: int) -> int:
	"""リンクを辿らずに開き，実際の種別と所有者・権限を確認する
	計算量：POSIX は O(1)，Windows は ACE 数 A に対して O(A)
	引数：directory: POSIX の親のディレクトリ，path: 対象のパス，kind: 通常ファイル又はディレクトリの種別
	戻値：検査済のファイル記述子
	送出例外：OSError / ValueError: 開けない，又は安全なキャッシュでない場合
	"""
	if os.name == "nt":
		# GENERIC_READ | READ_CONTROL, OPEN_EXISTING, OPEN_REPARSE_POINT | BACKUP_SEMANTICS
		handle: int = WINDOWS_KERNEL.CreateFileW(str(path), 0X80020000, 3 if kind == stat.S_IFDIR else 7, None, 3, 0X02200000, None)
		if handle == ctypes.c_void_p(-1).value:
			raise ctypes.WinError(ctypes.get_last_error())
		try:
			fd: int = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
		except BaseException:
			WINDOWS_KERNEL.CloseHandle(handle)
			raise
	else:
		flags: int = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK
		if kind == stat.S_IFDIR:
			flags |= os.O_DIRECTORY
		fd: int = os.open(path.name if directory is not None else path, flags, dir_fd=directory)
	try:
		info: os.stat_result = os.fstat(fd)
		if stat.S_IFMT(info.st_mode) != kind or getattr(info, "st_file_attributes", 0) & 0X400:
			raise ValueError(f"{path}: cache entry has an unsafe type; choose a new private --cache directory")
		if os.name == "nt":
			verify_windows_cache_access(fd)
		elif info.st_uid != os.getuid() or info.st_mode & (0O077 if kind == stat.S_IFDIR else 0O022):
			raise ValueError(f"{path}: cache owner or permissions are unsafe; choose a new private --cache directory")
		if kind == stat.S_IFREG and info.st_size > MAX_SOURCE_BYTES:
			raise ValueError(f"{path}: cache exceeds source limit; remove the cache entry and retry")
		# 検査した記述子の返戻
		return fd
	except BaseException:
		os.close(fd)
		raise

def download_source(url: str) -> bytes:
	"""通信エラーと一時的な HTTP の失敗を有限回再試行し，原本の受信量を制限する
	計算量：受信するバイト数 B に対して O(B)，試行は最大３回
	引数：url: バージョンを固定した公開の取得先
	戻値：取得した原本のバイト列
	送出例外：ValueError: 取得又は HTTP の失敗
	"""
	for attempt in range(len(DOWNLOAD_RETRY_DELAYS) + 1):
		try:
			with urlopen(Request(url, headers={ "User-Agent": "ShaveFormat-performance" }), timeout=30) as response:
				if response.status != 200:
					raise ValueError(f"{url}: HTTP {response.status}; check network access and the pinned URL, then retry")
				data: bytes = response.read(MAX_SOURCE_BYTES + 1)
		except HTTPError as e:
			e.close()
			if e.code not in (408, 429, 500, 502, 503, 504) or attempt == len(DOWNLOAD_RETRY_DELAYS):
				raise ValueError(
					f"{url}: HTTP {e.code} after {attempt + 1} attempt(s); check network access and the pinned URL, then retry"
				) from e
		except (OSError, URLError) as e:
			if attempt == len(DOWNLOAD_RETRY_DELAYS):
				raise ValueError(f"{url}: download failed after {attempt + 1} attempt(s): {e}; check network access and retry") from e
		else:
			break
		time.sleep(DOWNLOAD_RETRY_DELAYS[attempt])
	# 取得に成功した原本の返戻
	return data

def fetch_source(source: dict, cache: Path) -> bytes:
	"""固定した原本の SHA-256 を確認し，一致するキャッシュだけを再利用する
	計算量：原本のバイト数 B に対して O(B)
	引数：source: URL と固定したダイジェスト，cache: リポジトリ外の保存先
	戻値：照合した原本のバイト列
	送出例外：OSError / ValueError: 取得・保存・HTTP 又は SHA-256 の照合の失敗
	"""
	url: str = source["url"]
	digest: str = source["sha256"]
	path: Path = cache / digest
	try:
		cache.mkdir(mode=0O700, parents=True, exist_ok=True)
		directory: int = open_cache_entry(None, cache, stat.S_IFDIR)
	except OSError as e:
		raise ValueError(f"{cache}: cache directory could not be opened safely ({e}); choose a new private --cache directory") from e
	try:
		try:
			fd: int = open_cache_entry(directory, path, stat.S_IFREG)
		except FileNotFoundError:
			is_cached: bool = False
			data: bytes = download_source(url)
		except OSError as e:
			raise ValueError(f"{path}: cache entry could not be opened safely ({e}); choose a new private --cache directory") from e
		else:
			is_cached = True
			with os.fdopen(fd, "rb", buffering=0) as cached:
				data = cached.read(MAX_SOURCE_BYTES + 1)
		# 空・過大・不一致の原本は保存せず，既存のキャッシュも毎回照合する
		actual: str = hashlib.sha256(data).hexdigest()
		if not data or len(data) > MAX_SOURCE_BYTES:
			raise ValueError(f"{url}: empty or oversized source; verify the pinned source and retry")
		if actual != digest:
			raise ValueError(
				f"{url}: SHA-256 mismatch: expected {digest}, received {actual}; remove {path} if present and verify the manifest before retrying"
			)
		if not is_cached:
			# 書込と置換にも検査済のディレクトリを使う
			if os.name == "nt":
				with NamedTemporaryFile(dir=cache, delete=False) as output:
					temporary: Path = Path(output.name)
					try:
						output.write(data)
						output.close()
						os.replace(temporary, path)
					finally:
						temporary.unlink(missing_ok=True)
			else:
				temporary_name: str = "." + secrets.token_hex(16)
				fd = os.open(temporary_name, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0O600, dir_fd=directory)
				try:
					with os.fdopen(fd, "wb") as output:
						output.write(data)
					os.replace(temporary_name, digest, src_dir_fd=directory, dst_dir_fd=directory)
				finally:
					try:
						os.unlink(temporary_name, dir_fd=directory)
					except FileNotFoundError:
						pass
		# 照合した原本だけを返す
		return data
	finally:
		os.close(directory)

def verify_sources(manifest: dict, cache: Path = CACHE) -> dict[str, list[bytes]]:
	"""全言語の外部 URL・バージョン・許諾・原本を照合する
	引数：manifest: 取得と切出の目録，cache: 原本の保存先
	戻値：言語毎の照合済の原本
	送出例外：OSError / ValueError / KeyError: 取得・目録又は原本の不備
	"""
	if tuple(manifest) != LANGUAGES:
		raise ValueError("performance manifest must declare every language in order")
	if cache.resolve().is_relative_to(SOURCES.parent.parent):
		raise ValueError("performance cache must be outside the repository; select --cache in a temporary directory")
	# 目録にローカルの絶対パスを混ぜない
	pending: list = [manifest]
	while pending:
		value: object = pending.pop()
		if isinstance(value, dict):
			pending.extend(value.keys())
			pending.extend(value.values())
		elif isinstance(value, list):
			pending.extend(value)
		elif isinstance(value, str) and not value.startswith("https://"):
			if Path(value).is_absolute() or bool(PureWindowsPath(value).drive):
				raise ValueError("performance manifest contains an absolute path; use pinned public URLs")
	originals: dict[str, list[bytes]] = {}
	for lang, item in manifest.items():
		if not item["sources"] or set(item["samples"]) != { str(size) for size in SIZES }:
			raise ValueError(f"{lang}: missing original or sample; repair the manifest")
		urls: set[str] = set()
		data: list[bytes] = []
		for source in item["sources"]:
			url: str = source["url"]
			parsed: SplitResult = urlsplit(url)
			version: str = source["version"]
			parts: list[str] = parsed.path.split("/")
			if (
				parsed.scheme != "https" or parsed.netloc != "raw.githubusercontent.com" or len(parts) < 5 or parts[3] != version or
				version in ("HEAD", "latest", "main", "master") or parsed.query or parsed.fragment
			):
				raise ValueError(f"{lang}: {url}: pinned raw GitHub URL required; repair the manifest")
			if [part.casefold() for part in parts[1:3]] == ["fugoshimizu", "shaveformat"] or url in urls:
				raise ValueError(f"{lang}: {url}: own or repeated source; choose distinct external originals")
			urls.add(url)
			license: dict = source["license"]
			prefix: str = f"https://raw.githubusercontent.com/{parts[1]}/{parts[2]}/{version}/"
			if not license["type"] or not license["url"].startswith(prefix) or not re.fullmatch("[0-9a-f]{64}", source["sha256"]):
				raise ValueError(f"{lang}: {url}: missing pinned license or SHA-256; repair the manifest")
			data.append(fetch_source(source, cache))
		originals[lang] = data
		# 各範囲の重なり・欠落と，全規模の行数を生成前に確認する
		used: set[int] = set()
		for size in SIZES:
			source_input(item, data, lang, size)
			used.update(piece["source"] for piece in item["samples"][str(size)])
		if used != set(range(len(data))):
			raise ValueError(f"{lang}: unused originals; remove unreferenced sources from the manifest")
	# 照合済の原本の返戻
	return originals

def source_input(item: dict, originals: list[bytes], lang: str, target: int) -> bytes:
	"""目録のバイト範囲を１度ずつ連結し，反復や原本の不足を許さない
	計算量：採用する範囲数 R と結果のバイト数 B に対して O(R + B)
	引数：item: 言語の切出の目録，originals: 照合済の原本，lang: 言語，target: 規模
	戻値：原本の範囲だけを使った入力
	送出例外：ValueError / KeyError: 範囲の欠落・重複又は行数の不足
	"""
	pieces: list[bytes] = []
	ends: dict[int, int] = {}
	for piece in item["samples"][str(target)]:
		idx: int = piece["source"]
		if type(idx) is not int or idx < 0 or idx >= len(originals) or not piece["ranges"]:
			raise ValueError(f"{lang}/{target}: missing original or ranges; repair the manifest")
		parts: list[bytes] = []
		for start, end in piece["ranges"]:
			if type(start) is not int or type(end) is not int or start < ends.get(idx, 0) or end < start + 1 or end > len(originals[idx]):
				raise ValueError(f"{lang}/{target}: overlapping or invalid range; repair the manifest")
			ends[idx] = end
			parts.append(originals[idx][start:end])
		pieces.append(b"".join(parts))
	data: bytes = b"\n".join(pieces)
	if data.count(b"\n") < target:
		raise ValueError(f"{lang}/{target}: insufficient distinct source lines; repair the manifest")
	# １度だけ合成した入力の返戻
	return data

def verify_inputs(
	manifest: dict,
	originals: dict[str, list[bytes]],
	inputs: Path,
	sizes: tuple[int, ...] = SIZES
) -> dict[tuple[str, int], bytes]:
	"""全ての測定入力を原本と照合し，欠落・変更を計測前に拒否する
	計算量：照合する入力の総バイト数 B に対して O(B)
	引数：manifest: 切出の目録，originals: 照合済の原本，inputs: 生成した入力の保存先，sizes: 照合する規模
	戻値：言語・規模毎の照合済の入力
	送出例外：OSError / ValueError / KeyError: 入力の欠落・変更又は目録の不備
	"""
	checked: dict[tuple[str, int], bytes] = {}
	for lang in LANGUAGES:
		for size in sizes:
			expected: bytes = source_input(manifest[lang], originals[lang], lang, size)
			data: bytes = (inputs / str(size) / f"{lang}.in").read_bytes()
			if data != expected or size == TARGET_LINES and (inputs / f"{lang}.in").read_bytes() != expected:
				raise ValueError(f"{lang}/{size}: stale performance input; rebuild with tools/build_10k.py and the same --cache")
			checked[lang, size] = data
	# 照合済の入力の返戻
	return checked

def materialize_inputs(manifest: dict, originals: dict[str, list[bytes]], output: Path) -> dict[tuple[str, int], bytes]:
	"""原本の範囲から計測用の入力を保存し，全件の受理は計数ツールで検査する
	計算量：全入力の総バイト数 B に対して O(B)
	引数：manifest: 切出の目録，originals: 照合済の原本，output: 入力の保存先
	戻値：言語・規模毎の生成した入力
	送出例外：OSError / ValueError / KeyError: 保存又は原本の範囲の不備
	"""
	checked: dict[tuple[str, int], bytes] = {}
	for lang in LANGUAGES:
		for size in SIZES:
			checked[lang, size] = source_input(manifest[lang], originals[lang], lang, size)
	# 全ての範囲の生成に成功してから保存する
	for (lang, size), data in checked.items():
		path: Path = output / str(size) / f"{lang}.in"
		path.parent.mkdir(parents=True, exist_ok=True)
		path.write_bytes(data)
		if size == TARGET_LINES:
			(output / f"{lang}.in").write_bytes(data)
	# 計数で受理を確認する入力の返戻
	return checked

def is_complete(binary: str, data: bytes, lang: str) -> bool:
	"""返った出力を転記せず，入力の完全な受理を確かめる
	引数：binary: フォーマッター，data: 原本から作った入力，lang: 言語
	戻値：見送・部分・失敗が無いか
	送出例外：OSError / subprocess.SubprocessError: 起動又は時間制限の失敗
	"""
	result: subprocess.CompletedProcess[bytes] = subprocess.run(
		(binary, "--stdin", "--lang", lang, "--no-lint", "--fail-on-skip"),
		input=data,
		stdout=subprocess.DEVNULL,
		stderr=subprocess.PIPE,
		timeout=300
	)
	if result.returncode or any(word in result.stderr for word in (b"[skip]", b"Error:", b"partial")):
		print(
			f"{lang}: formatter rejected source (exit {result.returncode}): {result.stderr.decode(errors='replace')}; update the formatter before measuring this input",
			file=sys.stderr
		)
		# 不受理の返戻
		return False
	# 完全な受理の返戻
	return True

def verify_input_hashes(manifest: dict, baseline: dict, originals: dict[str, list[bytes]]) -> None:
	"""生成する全入力と明示的に採取した基準を照合する
	引数：manifest: 切出の目録，baseline: 採取した基準，originals: 照合済の原本
	送出例外：ValueError / KeyError: 未確定の基準又は入力の変更
	"""
	if not baseline["isVerified"] or baseline.get("metricVersion") != METRIC_VERSION:
		raise ValueError("performance baseline pending; capture all languages with the corrected release probe")
	for lang in LANGUAGES:
		for size in SIZES:
			ref: dict|None = baseline["languages"][lang][str(size)]
			data: bytes = source_input(manifest[lang], originals[lang], lang, size)
			if ref is None or hashlib.sha256(data).hexdigest() != ref["inputSha256"]:
				raise ValueError(f"{lang}/{size}: input differs from baseline; capture the reviewed external corpus explicitly")
	print(f"performance inputs: all {len(LANGUAGES) * len(SIZES)} digests verified without a formatter")
	# 照合の終了
	return

def build_inputs(
	manifest: dict,
	originals: dict[str, list[bytes]],
	binary: str,
	sizes: list[int],
	languages: list[str],
	output: Path
) -> None:
	"""選んだ全入力の受理を確認してから保存し，失敗時に部分集合を渡さない
	引数：manifest: 切出の目録，originals: 照合済の原本，binary: 検査するフォーマッター，sizes: 規模，languages: 言語，output: 保存先
	送出例外：OSError / ValueError / subprocess.SubprocessError: 受理又は保存の失敗
	"""
	pending: dict[Path, bytes] = {}
	for size in sizes:
		for lang in languages:
			data: bytes = source_input(manifest[lang], originals[lang], lang, size)
			if not is_complete(binary, data, lang):
				raise ValueError(f"{lang}/{size}: generation failed; no new input set was saved")
			pending[output / str(size) / f"{lang}.in"] = data
			if size == TARGET_LINES:
				pending[output / f"{lang}.in"] = data
	for path, data in pending.items():
		path.parent.mkdir(parents=True, exist_ok=True)
		path.write_bytes(data)
		lines: int = data.count(b"\n")
		print(f"{path}: {lines} lines, {len(data)} bytes", flush=True)
	# 保存の終了
	return

def self_check() -> None:
	"""取得・キャッシュ・不一致・HTTP 失敗と反復の拒否を外部接続無しで検査する
	送出例外：AssertionError: 失敗の拒否又は再取得の抑止が契約と異なる場合
	"""
	source: dict = {
		"url": "https://raw.githubusercontent.com/twbs/bootstrap/v5.3.3/LICENSE",
		"sha256": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
	}
	with TemporaryDirectory(prefix="shavefmt-source-check-") as directory:
		cache: Path = Path(directory)
		response: io.BytesIO = io.BytesIO(b"abc")
		response.status: int = 200
		with patch(__name__ + ".urlopen", return_value=response) as request:
			assert fetch_source(source, cache) == b"abc"
			assert fetch_source(source, cache) == b"abc"
			assert request.call_count == 1
		(cache / source["sha256"]).write_bytes(b"bad")
		try:
			fetch_source(source, cache)
		except ValueError as e:
			assert "SHA-256 mismatch" in str(e) and source["url"] in str(e)
		else:
			raise AssertionError("corrupt cache was accepted")
		(cache / source["sha256"]).unlink()
		for failure, detail, count, delays in (
			(HTTPError(source["url"], 503, "unavailable", {}, None), "HTTP 503", 3, [1, 2]),
			(HTTPError(source["url"], 404, "not found", {}, None), "HTTP 404", 1, []),
			(URLError("temporary connection failure"), "download failed", 3, [1, 2])
		):
			with patch(__name__ + ".urlopen", side_effect=failure) as request, patch(__name__ + ".time.sleep") as wait:
				try:
					fetch_source(source, cache)
				except ValueError as e:
					assert detail in str(e) and source["url"] in str(e) and "retry" in str(e)
				else:
					raise AssertionError("download failure was accepted")
				assert request.call_count == count
				assert [call.args[0] for call in wait.call_args_list] == delays
		assert not (cache / source["sha256"]).exists()
		# 通信と HTTP の一時的な失敗後に，照合済の原本だけを保存する
		response = io.BytesIO(b"abc")
		response.status: int = 200
		with patch(
			__name__ + ".urlopen",
			side_effect=[URLError("temporary connection failure"), HTTPError(source["url"], 503, "unavailable", {}, None), response]
		) as request, patch(__name__ + ".time.sleep") as wait:
			assert fetch_source(source, cache) == b"abc"
			assert fetch_source(source, cache) == b"abc"
			assert request.call_count == 3
			assert [call.args[0] for call in wait.call_args_list] == [1, 2]
		(cache / source["sha256"]).unlink()
		response = io.BytesIO(b"bad")
		response.status: int = 200
		with patch(__name__ + ".urlopen", return_value=response) as request, patch(__name__ + ".time.sleep") as wait:
			try:
				fetch_source(source, cache)
			except ValueError as e:
				assert "SHA-256 mismatch" in str(e)
			else:
				raise AssertionError("changed download was accepted")
			assert request.call_count == 1 and not wait.called
		assert not (cache / source["sha256"]).exists()
		if os.name != "nt":
			# 別の利用者の所有物，共有の権限，リンク及び FIFO を読まない
			with patch(__name__ + ".os.getuid", return_value=os.getuid() + 1):
				try:
					fetch_source(source, cache)
				except ValueError as e:
					assert "owner or permissions" in str(e)
				else:
					raise AssertionError("foreign cache owner was accepted")
			cache.chmod(0O777)
			try:
				fetch_source(source, cache)
			except ValueError as e:
				assert "owner or permissions" in str(e)
			else:
				raise AssertionError("shared cache permissions were accepted")
			finally:
				cache.chmod(0O700)
			target: Path = cache / "original"
			target.write_bytes(b"abc")
			cached_path: Path = cache / source["sha256"]
			cached_path.symlink_to(target)
			try:
				fetch_source(source, cache)
			except ValueError as e:
				assert "could not be opened safely" in str(e)
			else:
				raise AssertionError("cache symlink was followed")
			cached_path.unlink()
			directory_link: Path = cache / "directory-link"
			directory_link.symlink_to(cache, target_is_directory=True)
			try:
				fetch_source(source, directory_link)
			except ValueError as e:
				assert "could not be opened safely" in str(e)
			else:
				raise AssertionError("cache directory symlink was followed")
			directory_link.unlink()
			os.mkfifo(cached_path)
			try:
				fetch_source(source, cache)
			except ValueError as e:
				assert "unsafe type" in str(e)
			else:
				raise AssertionError("cache FIFO was accepted")
			cached_path.unlink()
		# stat 後に増えた原本にも，読取の上限を適用する
		cached_path = cache / source["sha256"]
		cached_path.write_bytes(b"oversized")
		original_stat: Callable[[int], os.stat_result] = os.fstat
		def stale_size(fd: int) -> os.stat_result:
			"""通常ファイルの大きさだけを，増加前の値として返す"""
			info: os.stat_result = original_stat(fd)
			if not stat.S_ISREG(info.st_mode):
				# ディレクトリの情報の返戻
				return info
			values: list[int|float] = list(info)
			values[6] = 0
			# 増加前の大きさの返戻（Windows の属性等の名前だけのフィールドも引き継ぐ）
			return os.stat_result(
				values,
				{ name: getattr(info, name) for name in ("st_file_attributes", "st_reparse_tag") if hasattr(info, name) }
			)
		class LimitedRead(io.FileIO):
			"""読取上限を越えた要求を境界で拒否する"""

			def read(self: LimitedRead, size: int = -1) -> bytes:
				"""原本の読取量が上限を１バイトだけ越える事を確認する"""
				assert size == MAX_SOURCE_BYTES + 1
				# 制限した読取結果の返戻
				return super().read(size)
		with patch(__name__ + ".MAX_SOURCE_BYTES", 4), patch(
			__name__ + ".os.fstat",
			side_effect=stale_size
		), patch(__name__ + ".os.fdopen", side_effect=lambda fd, mode, buffering = -1: LimitedRead(fd, mode)):
			try:
				fetch_source(source, cache)
			except ValueError as e:
				assert "oversized source" in str(e)
			else:
				raise AssertionError("growing source exceeded the bounded read")
	# 同じ原本の範囲を反復する入力を拒否する
	item: dict = { "samples": { "2": [{ "source": 0, "ranges": [[0, 3], [0, 3]] }] } }
	try:
		source_input(item, [b"ab\n"], "c", 2)
	except ValueError as e:
		assert "overlapping" in str(e)
	else:
		raise AssertionError("repeated source range was accepted")
	print("performance sources: download, cache, integrity, HTTP failure and repetition checks passed")
	# 検査の終了
	return

def main() -> int:
	"""取得・照合又は入力の生成を実行し，失敗の原因と対処を表示する
	戻値：成功は０，取得・目録・受理・保存の失敗は１
	"""
	parser: argparse.ArgumentParser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("output", type=Path, nargs="?")
	parser.add_argument("--formatter", type=Path)
	parser.add_argument("--cache", type=Path, default=CACHE)
	parser.add_argument("--verify-sources", action="store_true")
	parser.add_argument("--verify-baseline", type=Path)
	parser.add_argument("--self-test", action="store_true")
	parser.add_argument("--sizes", nargs="+", type=int, choices=SIZES, default=list(SIZES))
	parser.add_argument("--languages", nargs="+", choices=LANGUAGES, default=list(LANGUAGES))
	args: argparse.Namespace = parser.parse_args()
	try:
		if args.self_test:
			self_check()
			# 自己検査の終了
			return 0
		manifest: dict = json.loads((SOURCES / "manifest.json").read_bytes())
		originals: dict[str, list[bytes]] = verify_sources(manifest, args.cache)
		if args.verify_baseline:
			verify_input_hashes(manifest, json.loads(args.verify_baseline.read_bytes()), originals)
		if args.verify_sources or args.verify_baseline:
			print("performance sources: all pinned external originals and ranges verified")
			# 原本の照合の終了
			return 0
		if args.output is None or args.formatter is None:
			parser.error("output and --formatter are required for generation")
		build_inputs(manifest, originals, str(args.formatter.resolve()), args.sizes, args.languages, args.output)
		# 生成の終了
		return 0
	except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as e:
		print(f"performance input error: {e}", file=sys.stderr)
		# 不完全な取得又は生成の失敗の返戻
		return 1

if __name__ == "__main__":
	sys.exit(main())
