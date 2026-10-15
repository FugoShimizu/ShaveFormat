#! /usr/bin/env python3
"""Unicode の英数字・小文字・表音文字の固定区間表の生成

既存の正規化表と同じ Unicode のバージョンを使う
実行：python3 tools/generate_unicode_alphanumeric.py
生成物は src/Profile/UnicodeAlphanumeric.hpp と UnicodePhonetic.hpp に置き，手で編集しない
"""

from generate_unicode_normalization import VERSION, array_text
from pathlib import Path
from tempfile import TemporaryDirectory

import unicodedata

def alphanumeric_kind(char: str) -> str:
	"""ラテン文字・互換形と Unicode の数字の種類の取得

	仮名・漢字等の文字は英字に含めず，数字の文字種は変えない
	計算量：固定した Unicode の分解長の上限に対し O(1)
	引数：
		char: 判定する１文字
	戻値：
		英字は L，数字は N，其の他は空の文字列
	"""
	category: str = unicodedata.category(char)[0]
	if category == "N":
		if (
			unicodedata.east_asian_width(char) == "W" and
			not any(part.isascii() and part.isalnum() for part in unicodedata.normalize("NFKD", char))
		):
			# 和文の漢数字の返戻
			return ""
		# 各文字種の数字の返戻
		return "N"
	if category != "L":
		# 英字以外の文字の返戻
		return ""
	# ラテン文字とラテン文字に分解する互換形の種類の返戻
	return "L" if any("LATIN" in unicodedata.name(part, "") for part in unicodedata.normalize("NFKD", char)) else ""

def is_phonetic_letter(char: str) -> bool:
	"""語を空白で区切る文字と其の結合文字の判定

	文字カテゴリと文字名で字母・音節・合字と其の結合文字を読み，和文の文字・全角形を除く
	ハングルは全角の幅属性を持つが，語を空白で区切る表音文字として含む
	計算量：Unicode の文字名の固定長に対し O(1)
	引数：
		char: 判定する１文字
	戻値：
		表音文字又は其の結合文字なら True
	"""
	category: str = unicodedata.category(char)
	name: str = unicodedata.name(char, "")
	width: str = unicodedata.east_asian_width(char)
	is_letter: bool = category in ("Ll", "Lt", "Lu") or category.startswith("M") or category.startswith("L") and (
		name.startswith("HANGUL ") or any(
			unicodedata.category(part) in ("Ll", "Lt", "Lu") or
			any(kind in unicodedata.name(part, "") for kind in ("LETTER", "LIGATURE", "SYLLAB")) for part in
			unicodedata.normalize("NFKD", char)
		)
	)
	# 字母・音節・合字の種類から，全角形・仮名・漢字を除いた判定の返戻
	return is_letter and width != "F" and (name.startswith("HANGUL ") or width != "W" and "KATAKANA" not in name)

def main(root: Path|None = None) -> int:
	"""英字・数字・小文字の連続区間の生成

	英数字の ASCII は呼出側の既存の判定を使い，英数字の表へ重ねない
	小文字の表は ASCII も含む全コードポイントから作る
	計算量：Unicode のコードポイント数 U に対して O(U)
	引数：root: 生成先の作業木の根，None なら此のスクリプトを持つ作業木
	戻値：正常終了は０
	送出例外：
		OSError：生成物の書込に失敗する場合
		RuntimeError：固定した Unicode のバージョンと一致しない場合
	"""
	if unicodedata.unidata_version != VERSION:
		raise RuntimeError(f"Unicode {VERSION} is required, got {unicodedata.unidata_version}")
	if root is None:
		root = Path(__file__).resolve().parent.parent
	lowercase_ranges: list[int] = []
	begin_lowercase: int = 0
	in_lowercase: bool = False
	ranges: list[int] = []
	begin: int = 0
	kind: str = ""
	for point in range(0X110001):
		is_lowercase: bool = point < 0X110000 and unicodedata.category(chr(point)) == "Ll"
		if is_lowercase != in_lowercase:
			if in_lowercase:
				lowercase_ranges.append(begin_lowercase << 21 | point - 1)
			begin_lowercase = point
			in_lowercase = is_lowercase
		if point < 0X80:
			continue
		current: str = alphanumeric_kind(chr(point)) if point < 0X110000 else ""
		if current != kind:
			if kind:
				ranges.append((1 << 63 if kind == "N" else 0) | begin << 21 | point - 1)
			begin = point
			kind = current
	header: str = (
		f"// Unicode {VERSION}（Python 3.13）: python3 tools/generate_unicode_alphanumeric.py で生成\n"
		"// 生成物は手で編集しない\n#pragma once\n\n#include <array>\n#include <cstdint>\n\n"
		"namespace Core::UnicodeAlphanumeric {\n"
	)
	header += array_text("Ranges", "uint64_t", ranges, "文字・数字の区間：最上位ビットは数字，残る上位ビットは先頭，下位２１ビットは末尾，末尾の昇順")
	header += "\n" + array_text("LowercaseRanges", "uint64_t", lowercase_ranges, "小文字の区間：上位ビットは先頭，下位２１ビットは末尾，末尾の昇順")
	header += "}\n"
	(root / "src/Profile/UnicodeAlphanumeric.hpp").write_text(header, encoding="utf-8", newline="\n")
	phonetic_ranges: list[int] = []
	begin = 0
	in_range: bool = False
	for point in range(0X80, 0X110001):
		is_phonetic: bool = point < 0X110000 and is_phonetic_letter(chr(point))
		if is_phonetic != in_range:
			if in_range:
				phonetic_ranges.append(begin << 21 | point - 1)
			begin = point
			in_range = is_phonetic
	phonetic_header: str = (
		f"// Unicode {VERSION}（Python 3.13）: python3 tools/generate_unicode_alphanumeric.py で生成\n"
		"// 生成物は手で編集しない\n#pragma once\n\n#include <array>\n#include <cstdint>\n\n"
		"namespace Core::UnicodePhonetic {\n"
	)
	phonetic_header += array_text("Ranges", "uint64_t", phonetic_ranges, "表音文字と結合文字の区間：上位ビットは先頭，下位２１ビットは末尾，末尾の昇順")
	phonetic_header += "}\n"
	(root / "src/Profile/UnicodePhonetic.hpp").write_text(phonetic_header, encoding="utf-8", newline="\n")
	print(f"Unicode {VERSION}: {len(ranges)} alphanumeric ranges, {len(phonetic_ranges)} phonetic ranges")
	# 正常終了の返戻
	return 0

def entry_self_check() -> list[str]:
	"""一時領域への生成の終了値・内容と，書込失敗の伝播を確認する
	計算量：生成費用 P と生成物の総バイト数 B に対して O(P+B)
	戻値：起動契約又は既存の生成物と違う結果
	送出例外：OSError：既存の生成物又は一時領域を読み書き出来ない場合
	"""
	original_file: str = __file__
	root: Path = Path(original_file).resolve().parent.parent
	found: list[str] = []
	files: tuple[str, ...] = "src/Profile/UnicodeAlphanumeric.hpp", "src/Profile/UnicodePhonetic.hpp"
	expected: dict[str, bytes] = { name: (root / name).read_bytes() for name in files }
	if unicodedata.unidata_version != VERSION:
		try:
			main()
		except RuntimeError:
			# 未対応のバージョンで書込前に拒否した結果の返戻
			return []
		# 未対応のバージョンを受理した不一致の返戻
		return ["未対応の Unicode のバージョンを受理した"]
	try:
		with TemporaryDirectory(prefix="shavefmt-generator-") as temp:
			temporary_root: Path = Path(temp)
			for name in files:
				(temporary_root / name).parent.mkdir(parents=True, exist_ok=True)
			globals()["__file__"] = str(temporary_root / "tools" / Path(original_file).name)
			generated_status: int = main()
			if (
				type(generated_status) is not int or generated_status or
				any((temporary_root / name).read_bytes() != expected[name] for name in files)
			):
				found.append("英数字・表音文字の生成の終了値又は内容が変わった")
			original_version: str = VERSION
			try:
				globals()["VERSION"] = "unsupported"
				try:
					main()
				except RuntimeError:
					pass
				else:
					found.append("未対応の Unicode のバージョンを受理した")
			finally:
				globals()["VERSION"] = original_version
			globals()["__file__"] = str(temporary_root / "missing" / "tools" / Path(original_file).name)
			try:
				main()
			except OSError:
				pass
			else:
				found.append("書込失敗を正常終了と数えた")
	finally:
		globals()["__file__"] = original_file
	# 起動契約の不一致の返戻
	return found

if __name__ == "__main__":
	raise SystemExit(main())
