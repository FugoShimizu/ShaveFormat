#! /usr/bin/env python3
"""表示から除くコードポイントの共通表の生成

実行：python3 tools/generate_hidden_display.py
生成物は手で編集しない
"""

from pathlib import Path

import sys

from tempfile import TemporaryDirectory

# 表示から落とす範囲（Default_Ignorable_Code_Point に C1 制御文字と行を分ける U+2028 / U+2029 を足した物）
# Unicode の範囲順（C0・DEL と不正なサロゲートの扱いは呼出側に残す）
HIDDEN_RANGES: tuple[tuple[int, int], ...] = (
	(0X80, 0X9F),
	(0XAD, 0XAD),
	(0X34F, 0X34F),
	(0X61C, 0X61C),
	(0X115F, 0X1160),
	(0X17B4, 0X17B5),
	(0X180B, 0X180F),
	(0X200B, 0X200F),
	(0X2028, 0X202E),
	(0X2060, 0X206F),
	(0X3164, 0X3164),
	(0XFE00, 0XFE0F),
	(0XFEFF, 0XFEFF),
	(0XFFA0, 0XFFA0),
	(0XFFF0, 0XFFF8),
	(0X1BCA0, 0X1BCA3),
	(0X1D173, 0X1D17A),
	(0XE0000, 0XE0FFF)
)

def escape_point(point: int) -> str:
	"""正規表現で使うコードポイントの表記

	計算量：O(1)
	引数：point：コードポイント
	戻値：基本多言語面又は補助面のエスケープ
	"""
	# コードポイントのエスケープの返戻
	return f"\\u{point:04X}" if point < 0X10000 else f"\\u{{{point:X}}}"

def generated_files() -> dict[str, str]:
	"""１つの範囲表から両言語の生成物を作る

	計算量：範囲数 R に対し O(R)
	戻値：作業木からの相対パスと生成する内容
	"""
	banner: str = "// tools/generate_hidden_display.py: python3 tools/generate_hidden_display.py で生成\n// 生成物は手で編集しない\n"
	rows: list[str] = [f"\t\t{{ 0X{first - 1:X}, 0X{last + 1:X} }}" for first, last in HIDDEN_RANGES]
	header: str = (
		banner + "#pragma once\n\nnamespace DisplaySafety {\n"
		"\tstruct HiddenRange {\n\t\tchar32_t Below;\n\t\tchar32_t Above;\n\t}; // 表示から除く範囲（両端を含まない前後のコードポイント）\n\n"
		"\tinline constexpr HiddenRange Hidden[] = {\n" + ",\n".join(rows) + "\n\t};\n}\n"
	)
	parts: list[str] = [escape_point(first) + ("-" + escape_point(last) if first != last else "") for first, last in HIDDEN_RANGES]
	pattern: str = "".join(parts).replace("\\", "\\\\")
	typescript: str = banner + f'export const HIDDEN_DISPLAY_RANGES: string =\n"{pattern}";\n'
	# 共通表の生成物の返戻
	return { "src/Util/HiddenDisplay.hpp": header, "vscode-extension/src/hiddenDisplay.ts": typescript }

def check_generated(root: Path) -> list[str]:
	"""既存の全体検査で生成物の一致を確かめる

	計算量：生成物の総長 L に対し O(L)
	引数：root：作業木の根
	戻値：一致しない生成物の説明
	送出例外：OSError：生成物の読込に失敗する場合
	"""
	# 定義元からの生成内容との不一致の返戻
	return [
		f"generated display ranges differ: {name}" for name, text in generated_files().items() if (root / name).read_text(
			encoding="utf-8"
		) != text
	]

def main() -> int:
	"""共通の範囲表を両言語へ生成する

	計算量：生成物の総長 L に対し O(L)
	戻値：正常終了は０
	送出例外：OSError：生成物の書込に失敗する場合
	"""
	root: Path = Path(__file__).resolve().parent.parent
	for name, text in generated_files().items():
		(root / name).write_text(text, encoding="utf-8", newline="\n")
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
	files: tuple[str, ...] = "src/Util/HiddenDisplay.hpp", "vscode-extension/src/hiddenDisplay.ts"
	expected: dict[str, bytes] = { name: (root / name).read_bytes() for name in files }
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
