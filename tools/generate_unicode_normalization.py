#! /usr/bin/env python3
"""Python 3.13 の識別子用の Unicode 15.1.0 正規化表の生成

実行：python3 tools/generate_unicode_normalization.py
生成物は src/Profile/UnicodeNormalization.hpp に置き，手で編集しない
"""

from pathlib import Path

import sys

from tempfile import TemporaryDirectory

import unicodedata

VERSION: str = "15.1.0" # Python 3.13 が使う Unicode のバージョン

def array_text(name: str, type_name: str, values: list[int], description: str) -> str:
	"""コードポイント順又は組のキー順の固定表の出力

	計算量：要素数 N に対し O(N)
	引数：
		name：表の名前
		type_name：要素の整数型
		values：順序済の整数列
		description：表と値の配置の説明
	戻値：C++ の配列定義
	"""
	rows: list[str] = []
	for value in values:
		rows.append(f"\t\t0X{value:X}")
	# 固定表の定義の返戻
	return (
		f"\tinline constexpr std::array<{type_name}, {len(values)}> {name} {{\n" + ",\n".join(rows) + f"\n\t}}; // {description}\n"
	)

def main() -> int:
	"""固定した Unicode の表の生成

	互換分解を再帰的に展開し，合成除外を確認した正準合成だけを登録する
	計算量：Unicode のコードポイント数 U と展開後の要素数 D に対し O(U + D)
	戻値：正常終了は０
	送出例外：
		OSError：生成物の書込又はサイズの取得に失敗する場合
		RuntimeError：Unicode のバージョン又は展開した表の範囲が対応外の場合
	"""
	if unicodedata.unidata_version != VERSION:
		raise RuntimeError(f"Unicode {VERSION} is required, got {unicodedata.unidata_version}")
	decompositions: list[int] = []
	points: list[int] = []
	classes: list[int] = []
	compositions: list[int] = []
	for point in range(0X110000):
		char: str = chr(point)
		combining: int = unicodedata.combining(char)
		if combining:
			classes.append(point << 8 | combining)
		# Hangul の分解と合成は実装側で算術的に行う（音節の分解を返す Python のバージョンでも表へ入れない）
		if 0XABFF < point < 0XD7A4:
			continue
		expanded: str = unicodedata.normalize("NFKD", char)
		if expanded != char:
			if len(expanded) > 255 or len(points) > 0XFFFFFF:
				raise RuntimeError("decomposition table exceeds its packed range")
			decompositions.append(point << 32 | len(points) << 8 | len(expanded))
			points.extend(map(ord, expanded))
		mapping: str = unicodedata.decomposition(char)
		if mapping and not mapping.startswith("<"):
			pair: list[int] = [int(word, 16) for word in mapping.split()]
			if len(pair) == 2 and unicodedata.normalize("NFC", "".join(map(chr, pair))) == char:
				compositions.append(pair[0] << 42 | pair[1] << 21 | point)
	compositions.sort() # 合成の第１・第２コードポイントをキーとする照会順
	header: str = (
		"// Unicode 15.1.0（Python 3.13）: python3 tools/generate_unicode_normalization.py で生成\n"
		"// 生成物は手で編集しない\n#pragma once\n\n#include <array>\n#include <cstdint>\n\n"
		"namespace Core::UnicodeNormalization {\n"
	)
	header += array_text("Decompositions", "uint64_t", decompositions, "互換分解：上位３２ビットはコードポイント，下位３２ビットは開始添字２４ビットと長さ８ビット") + "\n"
	header += array_text("Points", "char32_t", points, "完全に展開した互換分解のコードポイント") + "\n"
	header += array_text("CombiningClasses", "uint32_t", classes, "正準結合クラス：上位２４ビットはコードポイント，下位８ビットはクラス") + "\n"
	header += array_text("Compositions", "uint64_t", compositions, "正準合成：第１・第２・結果のコードポイントを各２１ビットで格納")
	header += "}\n"
	output: Path = Path(__file__).resolve().parent.parent / "src/Profile/UnicodeNormalization.hpp"
	# 改行は環境に依らず LF（Windows の既定の CRLF へ変えない）
	output.write_text(header, encoding="utf-8", newline="\n")
	print(
		f"Unicode {VERSION}: {len(decompositions)} decompositions, {len(points)} points, "
		f"{len(classes)} classes, {len(compositions)} compositions"
	)
	print(
		f"table bytes: {8 * len(decompositions) + 4 * len(points) + 4 * len(classes) + 8 * len(compositions)}; "
		f"header bytes: {output.stat().st_size}"
	)
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
	files: tuple[str, ...] = "src/Profile/UnicodeNormalization.hpp",
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
				found.append("生成の終了値又は内容が変わった")
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
	sys.exit(main())
