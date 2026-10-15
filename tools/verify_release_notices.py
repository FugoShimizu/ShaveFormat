#! /usr/bin/env python3
"""CLI と VSIX に第三者の通知が在る事と，同じ対象の本文が一致する事を確かめる"""

from generate_third_party_notices import GRAMMARS

import argparse

from pathlib import Path

import re
import sys
import tarfile
import zipfile

def verify_notice(data: bytes, archive: Path) -> None:
	"""必要な全ての部品の通知の欠落を検出する
	引数：data: 通知の本文，archive: 対象の書庫
	送出例外：ValueError: 通知の部品又は許諾の本文が欠ける場合
	"""
	for name in (
		"tree-sitter runtime",
		"tree-sitter runtime: ICU (lib/src/unicode)",
		*("tree-sitter-" + grammar for grammar in GRAMMARS),
		"Unicode 15.1.0 Character Database (normalization and alphanumeric tables)"
	):
		if name.encode() + b"\n" + b"=" * 72 not in data:
			raise ValueError(f"{archive}: missing notice for {name}")
	icu: bytes = (
		b"tree-sitter runtime: ICU (lib/src/unicode)\n" + b"=" * 72 + b"\nCOPYRIGHT AND PERMISSION NOTICE (ICU 58 and later)"
	)
	if b"Permission is hereby granted" not in data or b"UNICODE LICENSE V3" not in data or icu not in data:
		raise ValueError(f"{archive}: missing license text")
	if "linux-" in archive.name and (b"GCC runtime: COPYING3" not in data or b"GCC runtime: COPYING.RUNTIME" not in data):
		raise ValueError(f"{archive}: missing statically linked GCC runtime notices")
	# 照合の終了
	return

def main() -> int:
	"""展開せずに配布物の通知を読む
	戻値：全ての照合に成功すれば０
	送出例外：OSError / ValueError / tarfile.TarError / zipfile.BadZipFile: 書庫又は通知の不備
	"""
	parser: argparse.ArgumentParser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("cli", type=Path)
	parser.add_argument("vsix", type=Path)
	args: argparse.Namespace = parser.parse_args()
	notices: dict[str, bytes] = {}
	cnt: int = 0
	# 形式毎の名前を確認し，１つだけの通知を読む
	for path in sorted((*args.cli.glob("*.tar.gz"), *args.cli.glob("*.zip"), *args.vsix.glob("*.vsix"))):
		member: str = "extension/THIRD_PARTY_NOTICES" if path.suffix == ".vsix" else "THIRD_PARTY_NOTICES"
		if path.name.endswith(".tar.gz"):
			with tarfile.open(path) as archive:
				if sum(info.name == member and info.isfile() for info in archive.getmembers()) != 1:
					raise ValueError(f"{path}: missing or duplicate notices")
				licenses: list[tarfile.TarInfo] = [info for info in archive.getmembers() if info.name == "LICENSE" and info.isfile()]
				if len(licenses) != 1:
					raise ValueError(f"{path}: missing or duplicate project license")
				license_stream: tarfile.ExFileObject|None = archive.extractfile(licenses[0])
				if license_stream is None:
					raise ValueError(f"{path}: unreadable project license")
				with license_stream:
					if not license_stream.read().strip():
						raise ValueError(f"{path}: empty project license")
				stream: tarfile.ExFileObject|None = archive.extractfile(member)
				if stream is None:
					raise ValueError(f"{path}: unreadable notices")
				with stream:
					data: bytes = stream.read()
		else:
			with zipfile.ZipFile(path) as archive:
				if archive.namelist().count(member) != 1:
					raise ValueError(f"{path}: missing or duplicate notices")
				data = archive.read(member)
				if path.suffix != ".vsix" and (archive.namelist().count("LICENSE") != 1 or not archive.read("LICENSE").strip()):
					raise ValueError(f"{path}: missing, duplicate or empty project license")
		verify_notice(data, path)
		match: re.Match[str]|None = re.search(r"(?:darwin|linux|win32)-(?:arm64|x64)", path.name)
		if not match:
			raise ValueError(f"{path}: unrecognized target")
		target: str = match.group().replace("win32-arm64", "win32-x64")
		if target in notices and notices[target] != data:
			raise ValueError(f"{path}: CLI and VSIX notices differ")
		notices[target] = data
		cnt += 1
	if not cnt:
		raise ValueError("no packaged notices to verify")
	print(f"third-party notices: {cnt} archives verified")
	# 照合の成功の返戻
	return 0

if __name__ == "__main__":
	sys.exit(main())
