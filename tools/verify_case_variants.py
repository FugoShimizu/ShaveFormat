#! /usr/bin/env python3
"""テストケースの変種での性質の検査（整形仕様 1.3, 1.5, 7.6, 10.4）

`golden_test.sh` はテストケース毎に定めた１つの幅と原文の儘の入力でしか整形しない
此処では整形を拒むテストケース (`.fail` / `.skip`) を除く各テストケースを分割の無い幅と複数の狭い幅で整形し，次を確かめる
何れの幅でも整形を拒まれず見送られない（正常な入力の整形の拒否は整形仕様 1.5 に反する）
２巡目が１巡目と一致する（冪等性は行幅に依らず成り立つ，整形仕様 1.3）
各行幅の整形結果と期待する出力 (`.out`) で，独立コメントが後続の文をアンカーとする規則を満たす（整形仕様 7.6）
深さは後続のコード行（閉括弧・節の見出を含む）の深さに一致し，後続が無ければ最上位（埋込の領域は親の閉タグのコード行）の深さに在り，タブでインデントする
原文でコメントの後に在ったトークンを追い越さず，原文でコメントの前に在ったトークンを持つ後の行より前へ遡らない
行頭へ置けない行（JSX の要素の中・Rust の複数行リテラルの継続行）がアンカーの行なら，其の領域の始まる行の前を許す（整形仕様 7.6, 7.7）
ドキュメントは統合・項目同期（整形仕様 7.4）と，同じ定義の見出への対応に従ってアンカーを検査する
加えて各行の末尾へ空白とタブを足した入力でも，終了コードが契約（整形仕様 10.4 の `0` / `1` / `2`）に収まる事を確かめる
逐語の領域の空白だけの行は行頭の空白の数が行末を越える形に為り，位置の計算を符号無で行うと反転する

使方：
python3 tools/verify_case_variants.py [フォーマッター]
"""

from __future__ import annotations

from cpp_lex import cpp_number_end, CPP_RAW_HEAD, cpp_source
from verify_semantic_preservation import cpp_semantic_parens, doc_records, DocIndex, dump_ast, LANGS, literal_ranges, Node, parse_dump

import ast

from bisect import bisect_left, bisect_right
from collections import Counter, deque, OrderedDict

import heapq

from html import unescape

import itertools
import os

from pathlib import Path

import re
import subprocess
import sys

from typing import Callable, NamedTuple

import unicodedata
import concurrent.futures

# 検査の経路と実行設定
ROOT: Path = Path(__file__).resolve().parent.parent
GOLDEN: Path = ROOT / "test_data/golden"
# 同時に走らせる検査の工程の数（`tools/golden_test.sh` と同じ `SHAVEFMT_JOBS`，既定は２）
JOBS: int = max(1, int(os.environ.get("SHAVEFMT_JOBS", "2")))
# 何も分割されない幅
WIDE: str = "1000000"
# 掃引する狭い行幅（テストケース毎の幅での冪等性は `golden_test.sh` が確かめる）
WIDTHS: tuple[str, ...] = "12", "40"

# 言語毎の独立コメントの開始のテキスト（行コメントの記号を先に置き，プリプロセッサディレクティブ・属性・私有名等の `#` をコメントと見做さない）
# `verify_comment_separation.py` も此の表から行コメントの記号を引く
OPENERS: dict[str, tuple[str, ...]] = {
	"c": ("//", "/*"),
	"cpp": ("//", "/*"),
	"h": ("//", "/*"),
	"csharp": ("//", "/*"),
	"java": ("//", "/*"),
	"go": ("//", "/*"),
	"rust": ("//", "/*"),
	"kotlin": ("//", "/*"),
	"swift": ("//", "/*"),
	"php": ("//", "/*", "# "),
	"javascript": ("//", "/*"),
	"javascriptreact": ("//", "/*"),
	"typescript": ("//", "/*"),
	"typescriptreact": ("//", "/*"),
	"ruby": ("#", "=begin"),
	"python": ("#",),
	"json": ("//", "/*"),
	"jsonc": ("//", "/*"),
	"html": ("<!--",),
	"css": ("/*",),
	"scss": ("//", "/*")
}

# HTML の埋込の領域の要素と中身の言語
EMBEDDED: dict[str, str] = { "script": "javascript", "style": "css" }
# 引用属性内の > を開始タグの終端としない
EMBEDDED_TAG: re.Pattern[str] = re.compile(r"""<(script|style)(?=[\s/>])((?:[^>'"]|'[^']*'|"[^"]*")*)>""", re.I)
# 属性を値ごと読み，名前の一部分や別の属性の引用値を type と見做さない
ATTRIBUTE: re.Pattern[str] = re.compile(r"""([^\s/=<>]+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))?""")
EMBEDDED_CLOSE: dict[str, re.Pattern[str]] = { name: re.compile(rf"</{name}\s*>", re.I) for name in EMBEDDED }

SCRIPT_TYPES: set[str] = {
	"application/ecmascript",
	"application/javascript",
	"application/x-ecmascript",
	"application/x-javascript",
	"text/ecmascript",
	"text/javascript",
	"text/javascript1.0",
	"text/javascript1.1",
	"text/javascript1.2",
	"text/javascript1.3",
	"text/javascript1.4",
	"text/javascript1.5",
	"text/jscript",
	"text/livescript",
	"text/x-ecmascript",
	"text/x-javascript"
}

# 囲みの記法の開始と終端（終端迄の継続行はコードの行でない）
BLOCKS: dict[str, str] = { "/*": "*/", "<!--": "-->", "=begin": "=end" }
# ネストのブロックコメントを持つ言語と開始・終端のトークン
NESTED_COMMENT_LANGS: tuple[str, ...] = "rust", "kotlin", "swift"
COMMENT_DELIMITERS: re.Pattern[str] = re.compile(r"/\*|\*/")
# 三重の引用符の文字列を持つ言語（中の記号をコメントと見做さない）
TRIPLE_QUOTE_LANGS: tuple[str, ...] = "python",
# Unicode の空白 (Zs) をトークンの間の空白と読む言語と，其の字
UNICODE_SPACE_LANGS: tuple[str, ...] = "csharp", "javascript", "javascriptreact", "typescript", "typescriptreact"
UNICODE_SPACES: str = "\u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u202f\u205f\u3000"
# トークンの前に Unicode のエスケープを読む言語（Java 言語仕様 3.3）
UNICODE_ESCAPE_LANGS: tuple[str, ...] = "java",
UNICODE_ESCAPE: re.Pattern[str] = re.compile(r"\\u+([0-9A-Fa-f]{4})")
# ヒアドキュメント（開始の行の次の行から終端の語の行迄が本文）を持つ言語
HEREDOC_LANGS: tuple[str, ...] = "ruby",
HEREDOC_START: re.Pattern[str] = re.compile(r"<<[~-]?(['\"`]?)([A-Za-z_]\w*)\1")
# 行頭（桁０）を必須とする記法（インデントしない為に深さを比べない，終端も行頭の物だけ）
COLUMN_ZERO: tuple[str, ...] = "=begin",
# 行頭（桁０）で有効に為る言語毎の登録ディレクティブ（桁０の物は深さの照合から除く，整形仕様 4.5，7.6）
LINE_HEAD_DIRECTIVES: dict[str, tuple[str, ...]] = { "go": ("//go:generate", "//line ") }
# 要素・文の区切（正規化で増減する為にアンカーの照合に用いない）
SEPARATORS: tuple[str, ...] = ",", ";"
# ドキュメントの開始のテキスト（括弧の中の物は定義の見出のドキュメントとして定義のドキュメントへ同期する為にアンカーの検査から除く，整形仕様 7.4）
DOCUMENTS: tuple[str, ...] = "/*!", "/**", "//!", "///"

# 引用属性を含む XML タグと，同期で補う空のタグ・Markdown の見出と項目の骨格（引用内の記号・改行はタグの境界でない）
DOCUMENT_MARKUP: re.Pattern[str] = re.compile(
	r"""</?[A-Za-z](?:[^<>"']|"[^"]*"|'[^']*')*>|(?m:^[^\S\n]*(?:@(?:param(?:[^\S\n]+<[^>\n]+>|[^\S\n]+\w+)|return|returns)|\#+[^\S\n]+(?:Arguments|Returns)|[*-][^\S\n]+`[^`\n]+`)[^\S\n]*$)"""
)

# ドキュメントを通常のコメントと同じ記法で書く言語の，ドキュメントを付ける定義の語（括弧の中の見出のコメントは其の定義のドキュメントへ同期する，整形仕様 7.4）
LINE_DOCUMENT_HEADS: dict[str, tuple[str, ...]] = { "go": ("const", "func", "type", "var"), "ruby": ("class", "def", "module") }
# 引用符の無い `url()` の引数を１つのトークンとする言語（中の `//`･`/*` はコメントでない，トークン間空白仕様 5.3）
URL_LANGS: tuple[str, ...] = "css", "scss"
# JSX を書ける言語
MARKUP_LANGS: tuple[str, ...] = "javascript", "javascriptreact", "typescriptreact"
# JSX の要素を始める `<` の前に置ける語（其の他の語・閉括弧の後の `<` は比較か型引数）
MARKUP_LEADS: tuple[str, ...] = "await", "case", "default", "return", "yield"
# JSX の要素の中の文脈（行頭へ独立コメントを置けない）
MARKUP_INSIDE: tuple[str, ...] = "children", "close", "self", "tag"
# 識別子・数値のトークン
WORD: re.Pattern[str] = re.compile(r"[\w$@]+")
# 行頭の空白（本文を複写せず，改行を越えない）
LINE_INDENT: re.Pattern[str] = re.compile(r"[^\S\n]*")
TAB_INDENT: re.Pattern[str] = re.compile(r"\t*")

# 正規表現又は区切付リテラルを読むパーサーの拡張子
REGEX_EXTS: dict[str, str] = {
	"csharp": "cs",
	"java": "java",
	"rust": "rs",
	"kotlin": "kt",
	"swift": "swift",
	"javascript": "js",
	"javascriptreact": "jsx",
	"typescript": "ts",
	"typescriptreact": "tsx",
	"ruby": "rb",
	"css": "css",
	"scss": "scss"
}

# Rust の生文字列の開き
RAW_STRING_HEAD: re.Pattern[str] = re.compile(r'(?:b|c)?r#*"')

# Rust の文字リテラルは１文字又は規定のエスケープだけを囲み，ライフタイム・ラベルを取り込まない
RUST_CHAR_LITERAL: re.Pattern[str] = (
	re.compile(r"'([^'\\\n\r\t]|\\(?:[\"'\\nrt0]|x[0-7][0-9A-Fa-f]|u\{(?:[0-9A-Fa-f]_*){1,6}\}))'")
)

# バイト文字は非 ASCII の字と Unicode エスケープを含まず，１６進の全てのバイトを許す
RUST_BYTE_CHAR_LITERAL: re.Pattern[str] = re.compile(r"'([^'\\\n\r\t\x80-\U0010FFFF]|\\(?:[\"'\\nrt0]|x[0-9A-Fa-f]{2}))'")
# Swift の拡張正規表現の開き
RAW_REGEX_HEAD: re.Pattern[str] = re.compile(r"#+/")

# 候補を拒んだ理由の内，検証失敗の全理由（整形仕様 10.4）
DEFECTS: tuple[str, ...] = (
	"break syntax",
	"change a directive comment",
	"change a documentation comment",
	"change blank lines",
	"change code structure",
	"change the meaning of the code",
	"remove a required block body"
)

# 契約上の終了コード
ALLOWED: tuple[int, ...] = 0, 1, 2
# 各行の末尾へ足す字（空白とタブの双方を１回の変異で通す）
SUFFIX: bytes = b" \t"

# 条文から定めたコメント分類・深さ・区切の正誤対
# 各組は名前・言語・テキスト・実際のコメント本文・深さの違反数・区切の違反数
SELF_CHECK_CASES: list[tuple[str, str, str, tuple[str, ...], int, int]] = [
	("連結後のコメント", "cpp", "/\\\n/ note\nx();\n", ("// note",), 0, 0),
	("生文字列内の連結取消", "cpp", "const char *s=R\"(a)\\\n\" // hidden\nb)\";\n// real\nx();\n", ("// real",), 0, 0),
	("C++ の桁区切の後のコメント", "cpp", "int n=1'000; // note\n", ("// note",), 0, 0),
	("C++ の数値と文字リテラル", "cpp", "int n=0xAB'CD; char c='\"'; // note\n", ("// note",), 0, 0),
	("前置のバイト順記号", "c", "\ufeffg(); // real\n", ("// real",), 0, 0),
	("C++ の入れ子でない囲み", "cpp", "void f(){\n\t/* outer /* inner */\n\tx();\n}\n", ("/* outer /* inner */",), 0, 0),
	("生文字列の注記記号", "cpp", "const char *s = R\"tag(\n\t// literal\n)tag\";\n// real\nx();\n", ("// real",), 0, 0),
	("生文字列の外の深さ違反", "cpp", "const char *s = u8R\"tag(\n\t// literal\n)tag\";\n\t// real\nx();\n", ("// real",), 1, 0),
	("Java のテキストブロック", "java", "class A { String s = \"\"\"\n\t// literal\n\"\"\"; }\n", (), 0, 0),
	("rust の入れ子コメントの深さ1", "rust", "fn f() {\n/*! outer /* inner */ tail */\n\tx();\n}\n", ("/*! outer /* inner */ tail */",), 1, 0),
	(
		"rust の入れ子コメントの深さ0",
		"rust",
		"fn f() {\n\t/*! outer /* inner */ tail */\n\tx();\n}\n",
		("/*! outer /* inner */ tail */",),
		0,
		0
	),
	("Rust の生文字列", "rust", "const S: &str = r#\"\n\t// literal\n\"#;\n", (), 0, 0),
	(
		"kotlin の入れ子コメントの深さ1",
		"kotlin",
		"fun f() {\n/*! outer /* inner */ tail */\n\tx();\n}\n",
		("/*! outer /* inner */ tail */",),
		1,
		0
	),
	(
		"kotlin の入れ子コメントの深さ0",
		"kotlin",
		"fun f() {\n\t/*! outer /* inner */ tail */\n\tx();\n}\n",
		("/*! outer /* inner */ tail */",),
		0,
		0
	),
	("Kotlin の複数行文字列", "kotlin", "val s = \"\"\"\n\t// literal\n\"\"\"\n", (), 0, 0),
	("Kotlin の補間内の実コメント", "kotlin", "val s = \"\"\"\n${f(\n// real\n1)}\n\"\"\"\n", ("// real",), 0, 0),
	(
		"swift の入れ子コメントの深さ1",
		"swift",
		"func f() {\n/*! outer /* inner */ tail */\n\tx();\n}\n",
		("/*! outer /* inner */ tail */",),
		1,
		0
	),
	(
		"swift の入れ子コメントの深さ0",
		"swift",
		"func f() {\n\t/*! outer /* inner */ tail */\n\tx();\n}\n",
		("/*! outer /* inner */ tail */",),
		0,
		0
	),
	("拡張の正規表現", "swift", "let r=#/[//]/# // real\n", ("// real",), 0, 0),
	("Swift の複数行文字列", "swift", "let s = \"\"\"\n\t// literal\n\"\"\"\n", (), 0, 0),
	("Swift の補間内の実コメント", "swift", "let s = \"\"\"\n\\(f(\n// real\n1))\n\"\"\"\n", ("// real",), 0, 0),
	("閉じ波括弧の深さ", "javascript", "if(x){\n\tg();\n// close\n}\n", ("// close",), 0, 0),
	("閉じ波括弧より深い物", "javascript", "if(x){\n\tg();\n\t// close\n}\n", ("// close",), 1, 0),
	("正規表現の文字集合", "javascript", "const r=/[//]/; // real\n", ("// real",), 0, 0),
	("正規表現の後の区切違反", "javascript", "const r=/[//]/;  // real\n", ("// real",), 0, 1),
	("除算の後の区切", "javascript", "value / divisor; // real\n", ("// real",), 0, 0),
	("除算の後の区切違反", "javascript", "value / divisor;  // real\n", ("// real",), 0, 1),
	("文頭の正規表現", "javascript", "if(x) /[//]/.test(y); // real\n", ("// real",), 0, 0),
	("式本体の後の除算", "javascript", "const r=function(){} / divisor; // real\n", ("// real",), 0, 0),
	("独立した複数のコメント", "javascript", "\t/* first */ // second\n\tg();\n", ("/* first */", "// second"), 0, 0),
	("囲みの後の行末コメント", "javascript", "g(); /* first */ // second\n", ("/* first */", "// second"), 0, 0),
	("行末へ積むタブの区切", "javascript", "g();\t// first\t// second\n", ("// first\t// second",), 0, 0),
	("Unicode の前置区切", "javascript", "g();\u00a0// real\n", ("// real",), 0, 1),
	("JSX の正規表現", "javascriptreact", "const r=<p>{/[//]/.test(x)}</p>; // real\n", ("// real",), 0, 0),
	("正規表現のエスケープとフラグ", "typescript", "const r=/[\\/]/g; // real\n", ("// real",), 0, 0),
	("後置増減の後の除算", "typescript", "value++ / divisor; // real\n", ("// real",), 0, 0),
	("TSX の正規表現", "typescriptreact", "const r=<p>{/[//]/.test(x)}</p>; // real\n", ("// real",), 0, 0),
	("正規表現内の井桁", "ruby", "r=/#/ # real\n", ("# real",), 0, 0),
	("正規表現の後の区切違反", "ruby", "r=/#/  # real\n", ("# real",), 0, 1),
	("通常の除算", "ruby", "r=a / b / c # real\n", ("# real",), 0, 0),
	("正規表現の実引数", "ruby", "puts /#/ # real\n", ("# real",), 0, 0),
	("区切付文字列", "ruby", "r=%q{#} # real\n", ("# real",), 0, 0),
	("正規表現の補間内のコメント", "ruby", "r=%r{#{x # real\n}}\n", ("# real",), 0, 0),
	("文字列の補間内のコメント", "ruby", "s=\"head\n#{f(1,\n# real\n2)}\"\n", ("# real",), 0, 0),
	("補間後の逐語の本文", "ruby", "c=`head#{x}\n# data\ntail`\n# real\nf()\n", ("# real",), 0, 0),
	("区切付正規表現", "ruby", "r=%r{[#]} # real\n", ("# real",), 0, 0),
	("行頭の垂直タブ", "python", "\v# c\n", ("# c",), 0, 0),
	("末尾の閉じタグの深さ０", "html", "<script>\n\tf();\n// tail\n</script>\n", ("// tail",), 0, 0),
	("末尾の閉じタグより深い物", "html", "<script>\n\tf();\n\t// tail\n</script>\n", ("// tail",), 1, 0),
	("ネストの閉じタグの深さ１", "html", "<div>\n\t<style>\n\t\tp{}\n\t/* tail */\n\t</style>\n</div>\n", ("/* tail */",), 0, 0),
	("ネストの閉じタグより深い物", "html", "<div>\n\t<style>\n\t\tp{}\n\t\t/* tail */\n\t</style>\n</div>\n", ("/* tail */",), 1, 0),
	("末尾より前のコードの深さ", "html", "<script>\n\t// head\n\tf();\n</script>\n", ("// head",), 0, 0),
	("末尾でないコメントの深さ", "html", "<script>\n// head\n\tf();\n</script>\n", ("// head",), 1, 0),
	("埋込の正規表現", "html", "<script>\n\tconst r=/[//]/; // real\n</script>\n", ("// real",), 0, 0),
	("完全な属性名", "html", "<script data-type=\"application/json\">\n\t// c\nx();\n</script>\n", ("// c",), 1, 0),
	("属性の等号の空白", "html", "<script type = \"text/javascript\">\n\t// c\nx();\n</script>\n", ("// c",), 1, 0),
	("空の型は既定", "html", "<script type=\"\">\n// c\nx();\n</script>\n", ("// c",), 0, 0),
	("型の部分一致を拒む", "html", "<script type=\"text/javascript-extra\">\n// c\nx();\n</script>\n", (), 0, 0),
	("別属性の値に型名", "html", "<script title='type=\"application/json\">'>\n// c\nx();\n</script>\n", ("// c",), 0, 0),
	("型の文字参照", "html", "<script type=\"text&#47;javascript\">\n// c\nx();\n</script>\n", ("// c",), 0, 0),
	("未対応の様式の型", "html", "<style type=\"text/less\">\n\t/* c */\np{}\n</style>\n", (), 0, 0),
	("Java の展開で開く引用符", "java", "String s=\\u0022// literal\\u0022; // real\n", ("// real",), 0, 0),
	("Java の偶数個のバックスラッシュ", "java", r'String s="\\u0022// literal"; // real' + "\n", ("// real",), 0, 0),
	("Java のサロゲート対のリテラル", "java", "String s=\"\\uD83D\\uDE00// literal\"; // real\n", ("// real",), 0, 0),
	("埋込のタグだけが先行する独立コメント", "html", "<script>// note\nf();\n</script>\n", ("// note",), 0, 0),
	("CRLF の開始タグと正規表現", "html", "<script\r\n>/[//]/.test(x); // note\n</script>\n", ("// note",), 0, 0),
	("CRLF の開始タグと不正な区切", "html", "<script\r\n>/[//]/.test(x);// note\n</script>\n", ("// note",), 0, 1),
	("SCSS の値とコメントの区別", "scss", ".a{color: //Z1 red;\nbackground: blue; // Z2\n}\n", ("// Z2",), 0, 0)
]

# 条文から手で定めたアンカーと前置区切の移動の正誤対（整形仕様 7.1, 7.6, 7.7）
# 各組は名前・言語・原文・候補・アンカーの違反数・前置区切の記録の違反数
TRANSFER_CHECK_CASES: list[tuple[str, str, str, str, int, int]] = [
	(
		"空の兄弟の終端とラッパーの除去及び返戻の補完",
		"javascript",
		"function f(){if(a){x();}try{g();}catch{\n// catch\n}\n{\n// bare\n}const o={};}\n",
		"function f(){\n\tif(a)x();\n\ttry{\n\t\tg();\n\t// catch\n\t}catch{}\n\t// bare\n\t{}\n\tconst o={};\n\treturn;\n}\n",
		0,
		0
	),
	(
		"同名の定義毎の宣言の統合と分割",
		"cpp",
		"// one\nvoid F(){const int A=1;const int B=2;}\n// two\nvoid F(){const int A=1,B=2;}\n",
		"/** one */\nvoid F(){\n\tconst int A=1,B=2;\n\treturn;\n}\n/** two */\nvoid F(){\n\tconst int A=1;\n\tconst int B=2;\n\treturn;\n}\n",
		0,
		0
	),
	(
		"宣言の統合と分割で別の定義へ移した説明",
		"cpp",
		"// one\nvoid F(){const int A=1;const int B=2;}\n// two\nvoid F(){const int A=1,B=2;}\n",
		"/** two */\nvoid F(){\n\tconst int A=1,B=2;\n\treturn;\n}\n/** one */\nvoid F(){\n\tconst int A=1;\n\tconst int B=2;\n\treturn;\n}\n",
		2,
		0
	),
	(
		"除いた本体の終端の後続節と閉じの同行",
		"javascript",
		"if(a){for(;;){x();y();}\n// note\n}else{z();w();}\n",
		"if(a) for(;;){\n\tx();\n\ty();\n// note\n} else {\n\tz();\n\tw();\n}\n",
		0,
		0
	),
	(
		"終端の前の文へ移したコメント",
		"javascript",
		"if(a){for(;;){x();y();}\n// note\n}else{z();w();}\n",
		"if(a) for(;;){\n\tx();\n\t// note\n\ty();\n} else {\n\tz();\n\tw();\n}\n",
		1,
		0
	),
	(
		"終端の後続節を越えて移したコメント",
		"javascript",
		"if(a){for(;;){x();y();}\n// note\n}else{z();w();}\n",
		"if(a) for(;;){\n\tx();\n\ty();\n} else {\n\t// note\n\tz();\n\tw();\n}\n",
		1,
		0
	),
	("複数行ブロックの行末への変換", "c", "x();\t/* One\n Two */\ny();\n", "x();\t// One // Two\ny();\n", 0, 0),
	("ブロックの追加行へのタブの複製", "c", "x();\t/* One\n Two */\ny();\n", "x();\t// One\t// Two\ny();\n", 0, 1),
	("本文末尾のバックスラッシュ", "c", "x();\t/* C:\\\n Next */\ny();\n", "x();\t/* C:\\ */ // Next\ny();\n", 0, 0),
	("コメントに吸われる継続行", "c", "x();\t// note \\  \ny();\nz();\n", "x();\t// note y();\nz();\n", 0, 0),
	("継続行の原文のタブの消失", "c", "x();\t// note \\  \ny();\nz();\n", "x(); // note y();\nz();\n", 0, 1),
	("プリプロセッサの後のコメント", "c", "#/* Continued.\n*/\\\n\nx();\n", "# // Continued\nx();\n", 0, 0),
	("単独 CR の行境界", "c", "x();\t// One\ry();\n", "x();\t// One\ny();\n", 0, 0),
	("Unicode の行境界", "csharp", "x();\t// One\u2028y();\n", "x();\t// One\ny();\n", 0, 0),
	("Unicode の行境界のタブの消失", "javascript", "x();\t// One\u2029y();\n", "x(); // One\ny();\n", 0, 1),
	("PHP の閉じタグの境界", "php", "<?php x();\t// One. Next.\n?>text\n", "<?php x();\t// One // Next ?>text\n", 0, 0),
	("PHP の閉じタグの前のタブの消失", "php", "<?php x();\t// One. Next.\n?>text\n", "<?php x(); // One // Next ?>text\n", 0, 1),
	("埋込の開始行の記録", "html", "<script>x();\t// One\ny();</script>\n", "<script>\n\tx();\t// One\n\ty();\n</script>\n", 0, 0),
	("埋込の開始行のタブの消失", "html", "<script>x();\t// One\ny();</script>\n", "<script>\n\tx(); // One\n\ty();\n</script>\n", 0, 1),
	("テンプレートの行末の移動", "javascript", "const s=`a${x\t// One\n}b`;\n", "const s=`a${x}b`;\t// One\n", 0, 0),
	("テンプレートの移動でのタブの消失", "javascript", "const s=`a${x\t// One\n}b`;\n", "const s=`a${x}b`; // One\n", 0, 1),
	("逐語補間の行末の移動", "csharp", "var s=$@\"{x\t// One\n}b\";\n", "var s=$@\"{x}b\";\t// One\n", 0, 0),
	("逐語補間の移動でのタブの消失", "csharp", "var s=$@\"{x\t// One\n}b\";\n", "var s=$@\"{x}b\"; // One\n", 0, 1),
	("Kotlin の補間の行末の移動", "kotlin", "val s=\"\"\"a${x\t// One\n}b\"\"\"\n", "val s=\"\"\"a${x}b\"\"\"\t// One\n", 0, 0),
	("Kotlin の補間のタブの消失", "kotlin", "val s=\"\"\"a${x\t// One\n}b\"\"\"\n", "val s=\"\"\"a${x}b\"\"\" // One\n", 0, 1),
	("見出と本体の間の説明の統合", "c", "int f(int x)\n// note\n{return x;}\n", "/** note */\nint f(int x) {return x;}\n", 0, 0),
	(
		"別の定義へ移した見出の説明",
		"c",
		"int f(int x)\n// note\n{return x;}\nint g(){return 0;}\n",
		"int f(int x){return x;}\n/** note */\nint g(){return 0;}\n",
		1,
		0
	),
	("前置デコレータを越えた所属", "typescript", "class A{@dec\n/** note */\nm(){}}\n", "class A{\n/** note */\n@dec\nm(){}}\n", 0, 0),
	(
		"タグ骨格を説明の出現に数えない",
		"typescript",
		"/** first */\nfunction f(){}\n/** second\n * @return\n */\nfunction g(){}\n",
		"/** first\n * @return\n */\nfunction f(){}\n/** second\n * @return\n */\nfunction g(){}\n",
		0,
		0
	),
	(
		"XML の説明項目の並替",
		"csharp",
		"/// <returns>result</returns><summary>note</summary>\nint F(){return 1;}\n",
		"/**\n * <summary>note</summary>\n * <returns>result</returns>\n */\nint F(){return 1;}\n",
		0,
		0
	),
	(
		"XML の引用属性の継続と概要への統合",
		"csharp",
		"class Earlier {\n/// <summary>First</summary>\nvoid E() {}\n}\nclass A {\n// Before\n/// <summary data=\"a\n/// >b\">First</summary>\n/// <summary data='/>'>Second</summary>\n// After\nvoid F() {}\n}\n",
		"class Earlier {\n/** <summary>First</summary> */\nvoid E() {}\n}\nclass A {\n/**\n * <summary data=\"a\n * >b\">Before\n * First</summary>\n * <summary data='/>'>Second\n * After</summary>\n */\nvoid F() {}\n}\n",
		0,
		0
	),
	(
		"XML の継続属性を持つ説明の再使用",
		"csharp",
		"/// <summary data=\"a\n/// >b\">First</summary>\nvoid F() {}\n",
		"/**\n * <summary data=\"a\n * >b\">First\n * First</summary>\n */\nvoid F() {}\n",
		1,
		0
	),
	(
		"XML の引用属性を持つ別の定義への説明の移動",
		"csharp",
		"/// <summary data=\"/>\">First</summary>\nvoid F() {}\n/// <summary data='/>'>Second</summary>\nvoid G() {}\n",
		"/** <summary data=\"/>\">Second</summary> */\nvoid F() {}\n/** <summary data='/>'>First</summary> */\nvoid G() {}\n",
		2,
		0
	),
	(
		"XML タグの外の記録毎の文境界",
		"cpp",
		"// (Read.\n// Return. Close.) Next.\nint A;\n/// (Read.\n/// Return. Close.) Next.\nint B;\n",
		"// (Read\n// Return Close) Next\nint A;\n/**\n * (Read\n * Return Close) Next\n */\nint B;\n",
		0,
		0
	),
	("タグに似たコード片の出現", "cpp", "/** `<T>` */\nf();\n/** `<U>` */\ng();\n", "/** `<T>` */\nf();\n/** `<U>` */\ng();\n", 0, 0),
	("タグに似たコード片の出現の再使用", "cpp", "/** `<T>` */\nf();\n/** `<U>` */\ng();\n", "/** `<T>` */\nf();\n/** `<T>` */\ng();\n", 1, 0),
	("フェンスの物理行毎の出現", "javascript", "/*\n * ```\n * note\n */\nf();\n", "// ```\n// note\nf();\n", 0, 0),
	("フェンス本文の出現の再使用", "javascript", "/*\n * ```\n * note\n */\nf();\n", "// ```\n// note\n// note\nf();\n", 1, 0),
	("同じアンカーの別の出現", "cpp", "// note\n// note\nf();\n", "// note\n// note\nf();\n", 0, 0),
	("同じアンカーの出現の超過", "cpp", "// note\n// note\nf();\n", "// note\n// note\n// note\nf();\n", 1, 0),
	("１記録の同じ文の各出現", "cpp", "// One. One. One.\nf();\n", "// One\n// One\n// One\nf();\n", 0, 0),
	("１記録の同じ文の超過", "cpp", "// One. One. One.\nf();\n", "// One\n// One\n// One\n// One\nf();\n", 1, 0),
	("異なる本文の出現とアンカー", "cpp", "// One\nf();\n// Two\ng();\n", "// One\nf();\n// Two\ng();\n", 0, 0),
	("別の本文の出現を流用", "cpp", "// One\nf();\n// Two\ng();\n", "// One\nf();\n// One\ng();\n", 1, 0),
	("独立と行末の同じ本文", "cpp", "// note\nf(); // note\n", "// note\nf(); // note\n", 0, 0),
	("独立だけの本文の行末化", "cpp", "// note\nf();\n", "f(); // note\n", 1, 0),
	("同じ本文の別アンカー", "javascript", "a();\n// note\nb();\n// note\nc();\n", "a();\n// note\nb();\n// note\nc();\n", 0, 0),
	("同じ出現の前方への再使用", "javascript", "a();\n// note\nb();\n// note\nc();\n", "a();\n// note\n// note\nb();\nc();\n", 1, 0),
	("同じ出現の後方への再使用", "javascript", "a();\n// note\nb();\n// note\nc();\n", "a();\nb();\n// note\n// note\nc();\n", 1, 0),
	("文の分割の各出現", "javascript", "// One. Two.\na();\n", "// One\n// Two\na();\n", 0, 0),
	("同じ文の分割の各出現", "javascript", "// One. One.\na();\n", "// One\n// One\na();\n", 0, 0),
	("ドキュメントへの統合", "javascript", "// One\n// Two\nfunction f(){}\n", "/**\n * One\n * Two\n */\nfunction f(){}\n", 0, 0),
	("疑問の文末のタブ", "javascript", "x();\t// First? Second\n", "x();\t// First? // Second\n", 0, 0),
	("疑問の文末のタブの消失", "javascript", "x();\t// First? Second\n", "x(); // First? // Second\n", 0, 1),
	("感嘆の文末のタブ", "javascript", "x();\t// First! Second\n", "x();\t// First! // Second\n", 0, 0),
	("感嘆の追加行へのタブの複製", "javascript", "x();\t// First! Second\n", "x();\t// First!\t// Second\n", 0, 1),
	("囲んだコードの文末記号", "javascript", "x();\t// `x?! y` End\n", "x();\t// `x?! y` End\n", 0, 0),
	("引用を閉じる文末", "javascript", "x();\t// \"First?\" Second\n", "x();\t// \"First?\" Second\n", 0, 0),
	("括弧内の文の非分割", "javascript", "x();\t// (First? Second)\n", "x();\t// (First? Second)\n", 0, 0),
	("囲みの外の追加行のタブ", "javascript", "x();\t// (First? Second). Next\n", "x();\t// (First? Second)\t// Next\n", 0, 1),
	("引用内の文の非分割", "javascript", "x();\t// \"First? Second\"\n", "x();\t// \"First? Second\"\n", 0, 0),
	(
		"行内タグの中の文の非分割",
		"java",
		"/** {@link A First. (Second. Third.)} */\nclass B {}\n",
		"/** {@link A First (Second Third)} */\nclass B {}\n",
		0,
		0
	),
	("括弧内の記号の例", "javascript", "x();\t// (?) End\n", "x();\t// (?) End\n", 0, 0),
	("連続する文末記号", "javascript", "x();\t// First? ! Second\n", "x();\t// First? ! // Second\n", 0, 0),
	("列挙番号の点", "javascript", "x();\t// 1. First\n", "x();\t// 1. First\n", 0, 0),
	("照合出来ない行末の本文", "javascript", "x();\t// First\n", "x(); // Other\n", 0, 1),
	(
		"同じ本文を持つ２つの JSX の行末記録",
		"typescriptreact",
		"const a=<A>\n{x&&( // show\n<B/>\n)}</A>;\nconst b=<A>{x&&( // show\n<B/>\n)}</A>;\n",
		"const a=<A>{x&&(<B/>)}</A>; // show\nconst b=<A>{x&&(<B/>)}</A>; // show\n",
		0,
		0
	),
	(
		"JSX の原文のタブの保持違反",
		"typescriptreact",
		"const a=<A>\n{x&&(\t// show\n<B/>\n)}</A>;\n",
		"const a=<A>{x&&(<B/>)}</A>; // show\n",
		0,
		1
	),
	("同行へ畳んだ実引数", "ruby", "r=foo(1,\n# c\n2)\n", "# c\nr=foo(1,2)\n", 0, 0),
	("実引数のアンカーの追越", "ruby", "r=foo(1,\n# c\n2)\nf()\n", "r=foo(1,2)\n# c\nf()\n", 1, 0),
	("正規表現の補間の同行化", "ruby", "r=/a#{foo(1,\n# c\n2)}/\n", "# c\nr=/a#{foo(1,2)}/\n", 0, 0),
	("記号の補間の同行化", "ruby", "s=:\"a#{foo(1,\n# c\n2)}\"\n", "# c\ns=:\"a#{foo(1,2)}\"\n", 0, 0),
	("複数行リテラルの外の独立行", "ruby", "s=\"head\n#{foo(1,\n# c\n2)}\"\n", "# c\ns=\"head\n#{foo(1,2)}\"\n", 0, 0),
	("複数行リテラルより前の別の文の追越", "ruby", "g()\ns=\"head\n#{foo(1,\n# c\n2)}\"\n", "# c\ng()\ns=\"head\n#{foo(1,2)}\"\n", 1, 0),
	(
		"消した括弧のアンカーの行",
		"ruby",
		"def v(o,p)\n(o*p)..\n# note\n((o+1)*p-1)\nend\n",
		"def v(o,p)\n\treturn o *\n\t# note\n\tp..(o + 1) *\n\tp - 1\nend\n",
		0,
		0
	),
	(
		"消した括弧のアンカーより前の行",
		"ruby",
		"def v(o,p)\n(o*p)..\n# note\n((o+1)*p-1)\nend\n",
		"def v(o,p)\n\t# note\n\treturn o *\n\tp..(o + 1) *\n\tp - 1\nend\n",
		1,
		0
	),
	(
		"Java の展開で閉じた引用符",
		"java",
		"class A{String x=\"\\u0022; // boundary\\u000aint y;\n}\n",
		"class A {\n\tString x = \"\\u0022; // boundary\n\tint y;\n}\n",
		0,
		0
	),
	(
		"Java の展開後の前置タブの増加",
		"java",
		"class A{String x=\"\\u0022; // boundary\\u000aint y;\n}\n",
		"class A {\n\tString x = \"\\u0022;\t// boundary\n\tint y;\n}\n",
		0,
		1
	),
	("独立コメントの無い反復", "javascript", "x();\n" * 32, "x();\n" * 32, 0, 0),
	(
		"Java の展開した復帰の前置タブの増加",
		"java",
		"class A{String x=\"\\u0022; // boundary\\u000dint y;\n}\n",
		"class A {\n\tString x = \"\\u0022;\t// boundary\n\tint y;\n}\n",
		0,
		1
	)
]

# 括弧の除去・前置化・返戻の補完は，原文の後続の所属を保つ
TRANSFER_CHECK_CASES.extend(
	[
		("前置化と返戻の補完", "cpp", "void F(int &x){\n// note\n(x)++;\n}\n", "void F(int &x){\n// note\n++x;\nreturn;\n}\n", 0, 0),
		("前置化で後続を追越", "cpp", "void F(int &x){\n// note\n(x)++;\n}\n", "void F(int &x){\n++x;\n// note\nreturn;\n}\n", 1, 0),
		("宣言の統合と返戻の補完", "cpp", "void F(){int a=1;\n// note\nint b=2;}\n", "void F(){int a=1,\n// note\nb=2;return;}\n", 0, 0),
		(
			"同名のローカル型の宣言の分割",
			"java",
			"class X{void F(){class A{int c=1,\n// first\nd=2;}}\nvoid G(){class A{int c=1,\n// second\nd=2;}}}\n",
			"class X{void F(){class A{int c=1;\n// first\nint d=2;}}\nvoid G(){class A{int c=1;\n// second\nint d=2;}}}\n",
			0,
			0
		),
		("値を包む返戻の前への移譲", "rust", "fn f()->i32{\n// note\nx+y\n}\n", "fn f()->i32{\n// note\nreturn (\nx+y\n);\n}\n", 0, 0),
		("値を包む返戻の中へ移した先行コメント", "rust", "fn f()->i32{\n// note\nx+y\n}\n", "fn f()->i32{\nreturn (\n// note\nx+y\n);\n}\n", 1, 0),
		(
			"引用符と束縛と返戻の補完",
			"javascript",
			"function f(){\n// note\nlet s='x';\n}\n",
			"function f(){\n// note\nconst s=\"x\";\nreturn;\n}\n",
			0,
			0
		),
		(
			"同じ条件の後置化と記号列の折返",
			"ruby",
			"def f\nunless check(%i[a b])\nfirst\nend\n# note\nunless ready(%i[c d])\nsecond\nend\nend\n",
			"def f\nfirst unless check(%i[a b])\n# note\nsecond unless ready(%i[\nc\nd\n])\nend\n",
			0,
			0
		),
		("引用符と返戻の補完", "ruby", "def f\n# note\n'value'\nend\n", "def f\n# note\nreturn \"value\"\nend\n", 0, 0),
		(
			"条件の後置化と返戻の前への移譲",
			"ruby",
			"def f\n# note\nunless ready\nvalue\nend\nend\n",
			"def f\n# note\nreturn (\nvalue unless ready\n)\nend\n",
			0,
			0
		),
		(
			"後置化した条件の前のコメントを返戻の中へ移すエラー",
			"ruby",
			"def f\n# note\nunless ready\nvalue\nend\nend\n",
			"def f\nreturn (\n# note\nvalue unless ready\n)\nend\n",
			1,
			0
		),
		(
			"後置化した本体の前のコメントの保持",
			"ruby",
			"def f\nunless ready\n# note\nvalue\nend\nend\n",
			"def f\nreturn (\n# note\nvalue unless ready\n)\nend\n",
			0,
			0
		),
		(
			"後置化した本体の前のコメントを返戻の外へ移すエラー",
			"ruby",
			"def f\nunless ready\n# note\nvalue\nend\nend\n",
			"def f\n# note\nreturn (\nvalue unless ready\n)\nend\n",
			1,
			0
		),
		("返戻で包んだ既存の括弧の内側の保持", "ruby", "def f\n(\n# note\nvalue\n)\nend\n", "def f\nreturn (\n# note\nvalue\n)\nend\n", 0, 0),
		("返戻で包んだ既存の括弧の内側を外へ移すエラー", "ruby", "def f\n(\n# note\nvalue\n)\nend\n", "def f\n# note\nreturn (\nvalue\n)\nend\n", 1, 0)
	]
)

class Token(NamedTuple):
	"""トークン（コードのトークン又はコメント）"""

	text: str # 分類用にエスケープを読替済のテキスト
	src_text: str # 物理行・位置の照合に使う原文のテキスト
	line: int # 始まる行（０から）
	is_comment: bool # コメントか
	is_standalone: bool # 前後に同じ行のコードの無いコメントか
	indent: str # 始まる行の行頭のインデント（行毎に共有）
	col: int # 始まる行の行頭からトークン迄の桁数
	separator: str # トークンの直前の空白
	literal_line: int # 最外の Ruby リテラルの始まる行（外側は -1）
	logical_line: int # エスケープの展開後の開始行
	logical_end_line: int # エスケープの展開後の終端行

	@property
	def has_preceding_text(self: Token) -> bool:
		"""同じ行の前に空白以外のテキストを持つかを返す
		計算量：O(1)
		引数：self: 行頭と開始位置を持つトークン
		戻値：
		空白以外のテキストが先行するなら真
		"""
		# 行頭の空白以外の先行テキストの判定の返戻
		return self.col > len(self.indent)

class Region(NamedTuple):
	"""字句解析済の検査領域"""

	tokens: list[Token] # 領域のトークン
	base: int # 後続コードの無い時の深さ（埋込は親の閉タグのコード行）
	lang: str # 領域内の言語
	tag_line: int # 埋込の開始タグの行（其れ以外は -1）
	semantic_parens: tuple[tuple[str, ...], ...] = () # 型の取得・推論に作用する括弧の記録

class EmbeddedRegion(NamedTuple):
	"""HTML 内の埋込領域と本文の範囲"""

	lang: str # 本文の言語
	body: str # 開始タグの行頭（前の領域の後）から閉タグの手前迄
	first_line: int # 本文の開始行
	base: int # 後続する閉タグのコード行の深さ（タグが無ければ所有要素の深さ）
	content_start: int # 開始タグの直後
	content_end: int # 閉タグの直前
	should_inspect: bool # 本文を埋込言語として検査するか

# 改定した整形仕様 7.3 と 11.2 の文区切の例（表の対象順で照合する）
LOGICAL_LINE_CASES: tuple[tuple[str, tuple[str, ...]], ...] = (
	("café's result. Next.", ("café's result", "Next")),
	("'café's result. Return.' Next.", ("'café's result Return' Next",)),
	("Ａ'Ｂ result. Next.", ("Ａ'Ｂ result", "Next")),
	("𝔄'𝔅 result. Next.", ("𝔄'𝔅 result", "Next")),
	("１２. First. Next.", ("１２. First", "Next")),
	("Ａ. First. Next.", ("Ａ. First", "Next")),
	("漢. First. Next.", ("漢", "First", "Next")),
	("読む。 Return.", ("読む", "Return")),
	("Read. Return.", ("Read", "Return")),
	("Read　. Next.", ("Read", "Next")),
	("Read.　Next.", ("Read", "Next")),
	("Read?　Next!", ("Read?", "Next!")),
	("(Read.　Return.)", ("(Read Return)",)),
	("\"Read.　Return.\"", ("\"Read Return\"",)),
	("'Read.　Return.'", ("'Read Return'",)),
	("Read　.", ("Read",)),
	("`Read.　Return.`", ("`Read.　Return.`",)),
	("Read.\u00a0Next.\u2009Finish.", ("Read", "Next", "Finish")),
	("Read (once). Return.", ("Read (once)", "Return")),
	("Dr. Smith reads.", ("Dr", "Smith reads")),
	("処理する.結果を返す.", ("処理する.結果を返す",)),
	("1. Read. Return.", ("1. Read", "Return")),
	("A. Read. Return.", ("A. Read", "Return")),
	("(Read. Return.)", ("(Read Return)",)),
	("(Read. Return.). Next.", ("(Read Return)", "Next")),
	("(Read. \"Return? Next!\" Done.) End.", ("(Read \"Return? Next!\" Done) End",)),
	("don't stop. Next.", ("don't stop", "Next")),
	("users' choices. Next.", ("users' choices", "Next")),
	("'users' choices. Next.", ("'users' choices", "Next")),
	(r'"Read\". Return." Next.', (r'"Read\" Return" Next',)),
	(r'"Read\\". Next.', (r'"Read\\"', "Next")),
	("('a.'b.'x.'c)", ("('a 'b.'x 'c)",)),
	("Read? Return! Next.", ("Read?", "Return!", "Next")),
	("Read?Return!Next.", ("Read?Return!Next",)),
	("読む？返す！次。", ("読む？", "返す！", "次")),
	("Read?! Next.", ("Read?!", "Next")),
	("First? ! Second.", ("First? !", "Second")),
	("？！読む。", ("？！読む",)),
	("(Read? Return!) Next.", ("(Read? Return!) Next",)),
	("\"Read. Return.\" Next.", ("\"Read Return\" Next",)),
	("Read. \"Quote.\" Done.", ("Read", "\"Quote\" Done")),
	("{@link A First. (Second. Third.)}", ("{@link A First (Second Third)}",)),
	("`f(x).v?!` Read. Next.", ("`f(x).v?!` Read", "Next")),
	("`0.25 + 1.0` equals `1.25`.", ("`0.25 + 1.0` equals `1.25`",)),
	("``x`?! . y`` Done.", ("``x`?! . y`` Done",)),
	("```\nx. y?!\n```\nDone.", ("```", "x. y?!", "```", "Done")),
	("処理.. 続行.", ("処理", "続行")),
	("処理.。", ("処理",)),
	("処理..結果.", ("処理..結果",)),
	("Read． Return。", ("Read", "Return")),
	(r'(\."Read. Return.")', (r'(\."Read Return")',)),
	("[Read. {Return? Next!}] End.", ("[Read {Return? Next!}] End",)),
	("「読む。『返す？次！』終る。」続行。", ("「読む『返す？次！』終る」続行",)),
	("〔Read. ＜Return. Next.＞ Done.〕 End.", ("〔Read＜Return Next＞Done〕End",)),
	(r"`x\`. Next.", (r"`x\`", "Next")),
	("... operator", ("operator",)),
	("Read ...", ("Read",)),
	("(... see)", ("(see)",)),
	("Read . Next.", ("Read", "Next")),
	(" ( Read .  Return. ) ", ("(Read Return)",)),
	("\" Read.  Return. \"", ("\"Read Return\"",)),
	("`Read .  Next.`  Done.", ("`Read .  Next.` Done",)),
	("(処理.. 続行.)", ("(処理続行)",)),
	("{@link A 処理.. 続行.}", ("{@link A 処理続行}",)),
	("処理 続行", ("処理続行",)),
	("Read.. 続行", ("Read", "続行")),
	("(Read.. 続行)", ("(Read 続行)",)),
	("Read 続行", ("Read 続行",)),
	("Read ， Next ： Done ； End", ("Read，Next：Done；End",)),
	("Read （ Next ） Done", ("Read（Next）Done",)),
	("Read ・ Next × Done ÷ End", ("Read・Next×Done÷End",)),
	("Read , Next : Done ; End", ("Read, Next: Done; End",)),
	("Read < Next > Done", ("Read <Next> Done",)),
	("Read ( once ) , ( Next )", ("Read (once),(Next)",)),
	("第１章 基本方針", ("第１章 基本方針",)),
	("処理 → 続行 ↔ 戻る — 終る", ("処理 → 続行 ↔ 戻る — 終る",)),
	("α 処理 Я 続行 한 終る", ("α 処理 Я 続行 한 終る",)),
	("50% 処理 30° 続行 ¥ 終る", ("50% 処理 30° 続行 ¥ 終る",)),
	("Ａ 処理", ("Ａ処理",)),
	("処理 `続行  戻る` 終る", ("処理 `続行  戻る` 終る",)),
	(r"（\。“読む。返す。”）", (r"（\ “読む返す”）",)),
	("Read ※ Next", ("Read ※ Next",)),
	("Next.\n", ("Next",))
)

def is_defect(result: subprocess.CompletedProcess[bytes]) -> bool:
	"""フォーマッターの終了状態と通知から不具合に依る拒否かを返す
	計算量：通知の文字数 B に対して O(B)
	引数：
		result: フォーマッターの終了状態と通知
	戻値：
		不具合に依る拒否なら真
	"""
	# 終了状態と不具合通知の照合
	msg: str = result.stderr.decode("utf-8", errors="replace")
	# 不具合の判定の返戻
	return (
		result.returncode < 0 or bool(result.returncode) and
		("formatter defect" in msg or "could not be completed" in msg or any(f"would {reason}," in msg for reason in DEFECTS))
	)

def format_src(binary: str, text: bytes, lang: str, width: str) -> bytes:
	"""指定のフォーマッターと言語と行幅でテストケースを整形したバイト列を返す
	整形の拒否又は見送では通知を伴う RuntimeError を送出する
	フォーマッターの実行が時間切に為れば TimeoutExpired を送出する
	計算量：フォーマッターの実行時間 T と通知の長さ B に対して O(T+B)
	引数：
		binary: フォーマッターの実行ファイル
		text: 照合又は解析するテキスト
		lang: テストケースの言語
		width: 整形の行幅
	戻値：
		指定した行幅での整形結果のバイト列
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		subprocess.TimeoutExpired: 呼び出した子プロセスが制限時間内に完了しない場合
		RuntimeError: 呼出先の整形又は検証が失敗する場合
	"""
	# フォーマッターの起動と見送の確認
	result: subprocess.CompletedProcess[bytes] = subprocess.run(
		(binary, "--stdin", "--lang", lang, "--no-lint", "--chars", width),
		input=text,
		capture_output=True,
		timeout=300
	)
	msg: str = result.stderr.decode("utf-8", errors="replace").strip()
	# 見送は終了コード０で原文を返す為，通知で区別する
	if result.returncode or "[skip]" in msg:
		raise RuntimeError(msg)
	# 整形結果の返戻
	return result.stdout

def line_end(text: str, idx: int) -> int:
	"""テキストの指定位置以後で最初の改行の位置を返す
	改行が無ければテキストの長さを返す
	計算量：指定位置以後の文字数 L に対して O(L)
	引数：
		text: 照合又は解析するテキスト
		idx: テキストの開始位置
	戻値：
		最初の改行の位置（無ければテキストの長さ）
	"""
	# 後続の改行の探索
	end: int = text.find("\n", idx)
	# 行の終わりの返戻
	return len(text) if end < 0 else end

def decode_unicode_escapes(text: str) -> tuple[str, list[int]]:
	"""トークン化前に Unicode のエスケープを展開し，引用符・コメントの境界を読むテキストを返す
	計算量：文字数 L に対して O(L)
	引数：
		text: 照合又は解析するテキスト
	戻値：
		エスケープを展開したテキストと，各境界の原文での位置
	"""
	# エスケープをデコードするテキストの組立
	pieces: list[str] = []
	positions: list[int] = [0]
	# 引用符の内外を区別しないトークン化前の展開
	idx: int = 0
	backslashes: int = 0
	while idx < len(text):
		escape: re.Match[str]|None
		# 前に奇数個のバックスラッシュが続くバックスラッシュはエスケープを始めない（`\\u000a` は字の儘）
		if (escape := UNICODE_ESCAPE.match(text, idx)) and not backslashes % 2:
			value: int = int(escape.group(1), 16)
			end: int = escape.end()
			low: re.Match[str]|None
			# UTF-16 のサロゲート対は１文字とし，未対応のサロゲートは原文を保つ
			if 0XD7FF < value < 0XDC00 and (low := UNICODE_ESCAPE.match(text, end)):
				low_value: int = int(low.group(1), 16)
				if 0XDBFF < low_value < 0XE000:
					value = 0X10000 + (value - 0XD800 << 10) + low_value - 0XDC00
					end = low.end()
			if 0XD7FF < value < 0XE000:
				pieces.append(text[idx:end])
				positions.extend(range(idx + 1, end + 1))
			else:
				pieces.append("\n" if value == 0XD else chr(value))
				positions.append(end)
			idx = end
			backslashes = 0
			continue
		pieces.append("\n" if text[idx] == "\r" else text[idx])
		backslashes = backslashes + 1 if text[idx] == "\\" else 0
		idx += 1
		positions.append(idx)
	# 読み替えたテキストと物理位置の返戻
	return "".join(pieces), positions

def match_rust_char_literal(text: str, start: int) -> re.Match[str]|None:
	"""１文字又は規定のエスケープで完結する Rust の文字・バイト文字の記法を返す
	計算量：エスケープの文字数 L に対して O(L+1)
	引数：
		text: 読取対象のテキスト
		start: 単引用符の位置（0 ≤ start < len(text)）
	戻値：
		完結する文字リテラルの一致，ライフタイム・ラベル等なら None
	送出例外：
		MemoryError: 文字リテラルの一致の確保が失敗する場合
	"""
	# `b` 接頭辞を持つバイト文字は，文字の ASCII エスケープと異なる範囲で照合する
	pattern: re.Pattern[str] = RUST_BYTE_CHAR_LITERAL if start and text[start - 1] == "b" else RUST_CHAR_LITERAL
	# 閉じの単引用符を含む１文字の一致の返戻
	return pattern.match(text, start)

def quote_ends(text: str, quote: str) -> dict[int, int]:
	"""各引用符から読む終端を後方走査で対応付ける
	未閉なら開始直後へ戻る既存の回復を保持する
	計算量：文字数 L に対して O(L)
	引数：text: 読取対象の本文，quote: 引用符
	戻値：各開始位置から終端直後への対応
	"""
	found: dict[int, int] = {}
	next_end: int = 0
	escaped_end: int = 0
	for idx in range(len(text) - 1, -1, -1):
		char: str = text[idx]
		end: int = next_end
		if char == quote:
			found[idx] = next_end or idx + 1
			end = idx + 1
		elif char == "\\":
			end = escaped_end
		elif char == "\n" and quote != "`":
			end = 0
		next_end, escaped_end = end, next_end
	# 全開始位置の終端の返戻
	return found

def lex(text: str, lang: str, first_line: int = 0, code_start: int = 0) -> list[Token]:
	"""指定言語のコメント開始記号に従い，テキストをコードのトークンとコメントへ分ける
	文字列・正規表現・ヒアドキュメント・引用符の無い `url()` の中の記号はコメントと見做さない
	行番号は指定の開始行を基準とする
	計算量：テキストの文字数 L・リテラルインデックスの構築費用 P に対して O(L+P)
	引数：
		text: 照合又は解析するテキスト
		lang: テストケースの言語
		first_line: 領域の開始行
		code_start: 埋込本文の開始位置（其れより前のタグはリテラルの解析対象・コードのトークンでない）
	戻値：
		コードとコメントのトークンの並び
	送出例外：
		OSError: パーサーの起動が失敗する場合
		RuntimeError: リテラルの解析又は原文との対応付が失敗する場合
		ValueError: パーサーの出力が不正な場合
		json.JSONDecodeError: パーサーのノード値が有効な JSON でない場合
		subprocess.TimeoutExpired: パーサーが制限時間内に完了しない場合
	"""
	# 言語毎の記法とトークン走査の準備
	openers: tuple[str, ...] = OPENERS.get(lang, ())
	spaces: str = " \t" + (UNICODE_SPACES if lang in UNICODE_SPACE_LANGS else "")
	src: str = text
	positions: list[int]|range = range(len(text) + 1)
	if lang in UNICODE_ESCAPE_LANGS:
		text, positions = decode_unicode_escapes(text)
	elif lang in ("c", "cpp", "h"):
		text, positions = cpp_source(text)
	# 読取前の改行と C / C++ の行連結の対応
	boundaries: str = r"\r\n|\n" if lang in ("go", "rust", "ruby") else r"\r\n?|\n"
	if lang == "csharp":
		boundaries += r"|[\x85\u2028\u2029]"
	elif lang in UNICODE_SPACE_LANGS:
		boundaries += r"|[\u2028\u2029]"
	parts: list[str] = []
	mapped: list[int] = []
	start: int = 0
	for boundary in re.finditer(boundaries, text):
		parts.append(text[start:boundary.start()])
		mapped.extend(positions[start:boundary.start()])
		parts.append("\n")
		mapped.append(positions[boundary.start()])
		start = boundary.end()
	parts.append(text[start:])
	mapped.extend(positions[start:])
	text, positions = "".join(parts), mapped
	code_start = bisect_right(positions, code_start - 1)
	found: list[Token] = []
	code_lines: set[int] = set()
	physical_cursor: int = 0
	logical_cursor: int = 0
	logical_line: int = first_line
	# 開始行に続く本文を読み飛ばすヒアドキュメントの終端の語
	heredocs: deque[str] = deque()
	idx: int = 0
	line: int = first_line
	line_start: int = 0
	gap_start: int = 0
	indent: str = ""
	has_line_token: bool = False
	# リテラルに為り得る境界で１度だけ構築する解析インデックス
	protected: dict[int, int] = {}
	enclosures: dict[int, int] = {}
	literal_end: int = 0
	literal_line: int = -1
	has_literal_idx: bool = False
	quotes: dict[str, dict[int, int]] = {}
	while idx < len(text):
		# 行・桁・インデントはエスケープの展開で増えた改行を数えず，物理の原文から得る
		logical_line += text.count("\n", logical_cursor, idx)
		logical_cursor = idx
		pos: int = positions[idx]
		line += src.count("\n", physical_cursor, pos)
		newline: int = src.rfind("\n", physical_cursor, pos)
		if newline > -1:
			line_start = newline + 1
			has_line_token = False
		physical_cursor = pos
		char: str = text[idx]
		# 空白とヒアドキュメント本文の読飛
		if char == "\n" and idx not in protected:
			idx += 1
			while heredocs and idx < len(text):
				end: int = line_end(text, idx)
				if text[idx:end].strip() == heredocs[0]:
					heredocs.popleft()
				idx = min(end + 1, len(text))
			gap_start = idx
			continue
		if (char in spaces or char in "\r\ufeff") and idx not in protected:
			if char not in spaces:
				gap_start = idx + 1
			idx += 1
			continue
		# コメントや引用形の内部を越えた実際の区切記号での解析
		if not has_literal_idx and lang in REGEX_EXTS and idx > code_start - 1 and (
			char == "/" and not text.startswith(("/*", "//", "/="), idx) or
			lang == "ruby" and (char in "%\"'`" or text.startswith((":\"", ":'"), idx)) or
			lang == "swift" and (text.startswith("\"\"\"", idx) or char == "#" and RAW_REGEX_HEAD.match(text, idx)) or
			lang in ("csharp", "java", "kotlin") and char in "\"$@" or char == "`" or lang == "rust" and RAW_STRING_HEAD.match(text, idx) or
			lang == "scss" and text.startswith("//", idx) or lang in URL_LANGS and char in "uU\\"
		):
			protected, enclosures = literal_ranges(text, REGEX_EXTS[lang], code_start)
			has_literal_idx = True
		# 最外のリテラルの始まる行の共有（補間内の独立コメントの安全な行への移動に使用）
		if idx in enclosures:
			literal_end, literal_line = enclosures[idx], line
		elif idx > literal_end - 1:
			literal_line = -1
		# 行毎に共有するインデントと，トークンの直前の区切の取得
		if not has_line_token:
			indent = LINE_INDENT.match(src, line_start).group()
			has_line_token = True
		separator: str = src[positions[gap_start]:pos]
		col: int = pos - line_start
		is_comment: bool = False
		# 引用形・区切付リテラルの保護
		if lang in ("c", "cpp", "h") and (char.isdigit() or char == "." and text[idx + 1:idx + 2].isdigit()):
			end = cpp_number_end(text, idx)
		elif lang in ("cpp", "h") and (raw := CPP_RAW_HEAD.match(text, idx)):
			raw_end: str = ")" + raw.group(1) + "\""
			close: int = text.find(raw_end, raw.end())
			end = len(text) if close < 0 else close + len(raw_end)
		elif lang in HEREDOC_LANGS and (heredoc := HEREDOC_START.match(text, idx)):
			heredocs.append(heredoc.group(2))
			end = heredoc.end()
		elif idx in protected:
			end = protected[idx]
		elif lang in TRIPLE_QUOTE_LANGS and text.startswith(("\"\"\"", "'''"), idx):
			close = text.find(text[idx:idx + 3], idx + 3)
			end = len(text) if close < 0 else close + 3
		elif lang == "csharp" and text.startswith(("$@\"", "@\"", "@$\""), idx):
			end = idx + (3 if text.startswith(("$@\"", "@$\""), idx) else 2)
			while end < len(text):
				if text.startswith("\"\"", end):
					end += 2
				elif text[end] == "\"":
					end += 1
					break
				else:
					end += 1
		elif lang == "php" and text.startswith("?>", idx):
			end = idx + 2
		else:
			# コメント開始記号と本文の抽出
			opener: str = next((opener for opener in openers if text.startswith(opener, idx)), "")
			if lang in HEREDOC_LANGS and text.startswith("#{", idx) or opener in COLUMN_ZERO and col:
				opener = ""
			if opener:
				is_comment = True
				if opener in COLUMN_ZERO:
					closer: re.Match[str]|None = re.compile(rf"^{re.escape(BLOCKS[opener])}\b", re.M).search(text, idx)
					end = line_end(text, closer.start()) if closer else len(text)
				elif opener == "/*" and lang in NESTED_COMMENT_LANGS:
					depth: int = 1
					end = len(text)
					for delimiter in COMMENT_DELIMITERS.finditer(text, idx + len(opener)):
						depth += 1 if delimiter.group() == "/*" else -1
						if not depth:
							end = delimiter.end()
							break
				elif opener in BLOCKS:
					close = text.find(BLOCKS[opener], idx + len(opener))
					end = len(text) if close < 0 else close + len(BLOCKS[opener])
				else:
					end = line_end(text, idx)
					if lang == "php" and (close := text.find("?>", idx, end)) > -1:
						end = close
			elif lang == "rust" and char == "'":
				# 次の単引用符迄探さず，文字リテラルの完結する記法だけを１つに読む
				literal: re.Match[str]|None = match_rust_char_literal(text, idx)
				end = literal.end() if literal else idx + 1
			elif char in "\"'`":
				# 同じ引用符の終端を１度だけ求め，未閉の引用符は記号として回復する
				if char not in quotes:
					quotes[char] = quote_ends(text, char)
				end = quotes[char][idx]
			else:
				word: re.Match[str]|None = WORD.match(text, idx)
				end = word.end() if word else idx + 1
		# トークンの共通の記録と行・式境界の更新
		end = min(end, len(text))
		# 埋込の開始タグは子のコードのトークンに含めない
		if is_comment or idx > code_start - 1:
			body: str = text[idx:end]
			end_line: int = logical_line + body.count("\n")
			found.append(
				Token(body, src[pos:positions[end]], line, is_comment, False, indent, col, separator, literal_line, logical_line, end_line)
			)
			if not is_comment:
				code_lines.update(range(logical_line, end_line + 1))
		idx, gap_start = end, end
	# 同じ行の複数の囲みも含め，コメントだけの論理行を独立としたトークン列の返戻
	return [
		token._replace(is_standalone=token.logical_line not in code_lines and token.logical_end_line not in code_lines) if
		token.is_comment else
		token for token in found
	]

def embedded(text: str) -> list[EmbeddedRegion]:
	"""HTML の埋込領域毎の言語・テキスト・行・深さ・本文の範囲を返す
	計算量：文字数 L・埋込の領域数 E に対して O(L+E log L)
	引数：
		text: 照合又は解析するテキスト
	戻値：
		埋込領域毎の言語・本文・位置の一覧
	"""
	# 埋込要素と閉じ要素の対応付
	found: list[EmbeddedRegion] = []
	line_starts: list[int] = [0, *(match.end() for match in re.finditer("\n", text))]
	line_tabs: list[int] = [TAB_INDENT.match(text, start).end() - start for start in line_starts]
	prev_end: int = 0
	for tag in EMBEDDED_TAG.finditer(text):
		if tag.start() < prev_end:
			continue
		name: str = tag.group(1).lower()
		close: re.Match[str]|None = EMBEDDED_CLOSE[name].search(text, tag.end())
		content_end: int = len(text) if close is None else close.start()
		attrs: list[re.Match[str]] = list(ATTRIBUTE.finditer(tag.group(2)))
		kind: str = unescape(
			next((next((val for val in attr.groups()[1:] if val is not None), "") for attr in attrs if attr.group(1).lower() == "type"), "")
		).strip().lower()
		should_inspect: bool = (
			not kind or (kind == "module" or kind.split(";", 1)[0].strip() in SCRIPT_TYPES if name == "script" else kind == "text/css")
		)
		close_line: int = bisect_right(line_starts, content_end) - 1
		# 開始の要素の在る行の頭から読み，同じ行の中身を独立した行の物と見誤らない
		first_line: int = bisect_right(line_starts, tag.start()) - 1
		head: int = line_starts[first_line]
		body_start: int = max(head, prev_end)
		found.append(
			EmbeddedRegion(
				EMBEDDED[name],
				text[body_start:content_end],
				first_line,
				line_tabs[close_line if close else first_line],
				tag.end(),
				content_end,
				should_inspect
			)
		)
		prev_end = len(text) if close is None else close.end()
	# 埋込の領域の返戻
	return found

def regions(text: str, lang: str) -> list[Region]:
	"""指定言語で検査する領域毎のトークンと最上位の深さと領域の言語を返す
	HTML では本体と埋込の JavaScript･CSS を別の領域とする
	計算量：文字数 L・埋込の領域数 E・リテラルインデックスの構築費用 P に対してO((E+1)L+P)
	引数：
		text: 照合又は解析するテキスト
		lang: テストケースの言語
	戻値：
		トークン・最上位の深さ・言語を持つ領域の一覧
	送出例外：
		OSError: パーサーの起動が失敗する場合
		RuntimeError: リテラルの解析又は原文との対応付が失敗する場合
		ValueError: パーサーの出力が不正な場合
		json.JSONDecodeError: パーサーのノード値が有効な JSON でない場合
		subprocess.TimeoutExpired: パーサーが制限時間内に完了しない場合
	"""
	if lang != "html":
		# 全体を１つの領域とする事の返戻
		tokens: list[Token] = lex(text, lang)
		parens: tuple[tuple[str, ...], ...] = (
			cpp_semantic_parens(text) if
			lang in ("cpp", "h") and any(token.text == "decltype" for token in tokens if not token.is_comment) else
			()
		)
		# 本文のトークンと型に作用する括弧の記録の返戻
		return [Region(tokens, 0, lang, -1, parens)]
	# 埋込領域を除いた HTML 本体の構築
	sections: list[EmbeddedRegion] = embedded(text)
	parts: list[str] = []
	start: int = 0
	for section in sections:
		parts.append(text[start:section.content_start])
		parts.append(re.sub(r"[^\n]", " ", text[section.content_start:section.content_end]))
		start = section.content_end
	parts.append(text[start:])
	# 埋込の本文をトークンと見做さず，HTML 本体の行番号とインデントを保持する
	markup: list[Token] = lex("".join(parts), "html")
	# HTML の本体と各埋込の領域の返戻
	return [
		Region(markup, 0, "html", -1),
		*[
			Region(
				lex(section.body, section.lang, section.first_line, section.content_start - section.content_end + len(section.body)),
				section.base,
				section.lang,
				section.first_line
			) for section in sections if section.should_inspect
		]
	]

def depth_mismatches(parsed: list[Region], text: str) -> list[str]:
	"""指定言語の独立コメントの深さとインデントの違反を行順に返す
	深さは後続のコード行，無ければ領域の終端に対応する深さと比較する
	計算量：文字数 L・トークン数 N に対して O(L+N log N)
	引数：
		parsed: 解析済の領域の一覧
		text: 照合又は解析するテキスト
	戻値：
		独立コメントの深さとインデントの違反の一覧
	"""
	# 行の深さの照合の準備
	lines: list[str] = text.split("\n")
	depths: list[int] = [len(line) - len(line.lstrip("\t")) for line in lines]
	# 行と違反の組
	found: list[tuple[int, str]] = []
	# 後続コード行の深さの追跡
	for tokens, base, lang, _, _ in parsed:
		# 後続のコード行の深さ（末尾から遡って求める）
		following: int = base
		for token in reversed(tokens):
			if not token.is_comment:
				following = depths[token.line]
				continue
			if not token.is_standalone or token.has_preceding_text:
				continue
			if token.indent.strip("\t"):
				found.append((token.line, f"{token.line + 1} 行目の {token.text[:40]}：タブでなくインデントした"))
			elif (
				len(token.indent) != following and not token.text.startswith(COLUMN_ZERO) and
				not (not token.indent and token.text.startswith(LINE_HEAD_DIRECTIVES.get(lang, ())))
			):
				found.append((token.line, f"{token.line + 1} 行目の {token.text[:40]}：深さ {len(token.indent)}，後続のコード行の深さ {following}"))
			elif not token.text.startswith(COLUMN_ZERO):
				# 継続行が全て `*` で始まる囲みは，其の行を開始の行と同じ深さに半角スペース１つを置いて揃える（記号の無い本文の行を持つ物は逐語の儘）
				rests: list[str] = token.src_text.split("\n")[1:]
				for offset, rest in enumerate(rests if all(rest.lstrip(" \t").startswith("*") for rest in rests) else [], 1):
					if rest[:len(rest) - len(rest.lstrip(" \t"))] != token.indent + " ":
						found.append((token.line + offset, f"{token.line + offset + 1} 行目の {rest.strip()[:40]}：囲みの継続行の深さが開始の行と違う"))
	# 食違の一覧（行の順）の返戻
	return [problem for _, problem in sorted(found)]

def blocked_lines(tokens: list[Token], lang: str) -> dict[int, int]:
	"""行頭へ独立コメントを置けない出力の行と，其の行を含む領域の始まる行の対応を返す（整形仕様 7.6, 7.7）
	領域は Ruby のリテラル，Rust の複数行リテラルと，コードの文脈で始まる JSX の要素とし，領域の始まる行自身は置ける
	計算量：コードのトークン数 N・テキストの文字数 L に対して O(N+L)
	引数：
		tokens: コードのトークンの並び
		lang: テストケースの言語
	戻値：
		独立コメントを置けない行と領域の開始行の対応
	"""
	# 独立コメントを置けない行の収集
	found: dict[int, int] = {}
	# 字句解析と共有した Ruby の最外のリテラルの境界
	if lang == "ruby":
		for token in tokens:
			if token.literal_line < 0:
				continue
			for line in range(max(token.line, token.literal_line + 1), token.line + token.src_text.count("\n") + 1):
				found.setdefault(line, token.literal_line)
	# Rust では複数行リテラルの内部だけを保護し，マクロのコード行は通常のアンカーとして扱う
	elif lang == "rust":
		for token in tokens:
			if token.is_comment:
				continue
			for line in range(token.line + 1, token.line + token.src_text.count("\n") + 1):
				found.setdefault(line, token.line)
	# JSX の要素領域の走査
	elif lang in MARKUP_LANGS:
		# 文脈の積（コードの "code"・"brace"・"container"，タグの中の "tag"・"self"，本文の "children"，閉じのタグの "close"）と，コードの文脈で始めた要素の始まる行の積
		contexts: list[str] = ["code"]
		starts: list[int] = []
		# タグ名に続く型引数の括弧は，開始タグの終端と区別する（関数型のアローの > も閉じにしない）
		type_ends: list[str] = []
		type_brackets: dict[str, str] = { "(": ")", "<": ">", "[": "]", "{": "}" }
		for idx, token in enumerate(tokens):
			if (not idx or token.line != tokens[idx - 1].line) and contexts[-1] in MARKUP_INSIDE and starts:
				found.setdefault(token.line, starts[-1])
			following: str = tokens[idx + 1].text if idx + 1 < len(tokens) else ""
			prev: str = tokens[idx - 1].text if idx else ""
			if contexts[-1] in MARKUP_INSIDE:
				if contexts[-1] == "tag" and (type_ends or token.text == "<"):
					if token.text in type_brackets:
						type_ends.append(type_brackets[token.text])
					elif token.text == type_ends[-1] and not (token.text == ">" and prev == "="):
						type_ends.pop()
				elif contexts[-1] == "children" and token.text == "<":
					contexts.append("close" if following == "/" else "tag")
				elif contexts[-1] in ("children", "tag") and token.text == "{":
					contexts.append("brace" if contexts[-1] == "tag" else "container")
				elif contexts[-1] == "tag" and token.text == "/" and following == ">":
					contexts[-1] = "self"
				elif contexts[-1] == "tag" and token.text == ">":
					contexts[-1] = "children"
				elif contexts[-1] in ("close", "self") and token.text == ">":
					# 閉じのタグは其の要素の本文も閉じ，コードの文脈へ戻れば領域を終える
					if contexts.pop() == "close":
						contexts.pop()
					if contexts[-1] not in MARKUP_INSIDE:
						starts.pop()
			elif (
				token.text == "<" and (WORD.fullmatch(following) or following == ">") and
				(not (WORD.fullmatch(prev) or prev in (")", "]")) or prev in MARKUP_LEADS)
			):
				starts.append(token.line)
				contexts.append("tag")
			elif token.text == "{":
				contexts.append("brace")
			elif token.text == "}" and len(contexts) > 1:
				contexts.pop()
	# 置けない行と領域の始まる行の対応の返戻
	return found

def separator_mismatches(parsed: list[Region]) -> list[str]:
	"""指定言語の行末コメントの前置区切の違反を行順に返す（整形仕様 7.6）
	行末コメントは同じ行の前にコード又はコメントが在り，同じ行にコードが続かない物とし，前置区切はタブだけ又は半角スペース１つとする
	行末へ積んだ記号の前の区切も同じ規則で検査し，テキストを保つ領域（JSX）の物は除く
	計算量：領域数 E・トークン数 N・全トークンの本文量 B に対して O(E+N+B log(B+1))
	引数：
		parsed: 解析済の領域の一覧
	戻値：
		行末コメントの前置区切の違反の一覧
	"""
	# 行と違反の組
	found: list[tuple[int, str]] = []
	for tokens, _, inner, tag_line, _ in parsed:
		if inner == "html":
			continue
		blocked: dict[int, int] = blocked_lines(tokens, inner)
		# 後に続くコードのトークンの始まる行（末尾から遡って求め，無ければ最終行の後）
		following: int = tokens[-1].logical_end_line + 1 if tokens else 0
		for token in reversed(tokens):
			if not token.is_comment:
				following = token.logical_line + (inner == "php" and token.text == "?>")
				continue
			if (
				token.is_standalone or token.line in blocked or token.line == tag_line or not token.has_preceding_text or
				following <= token.logical_end_line
			):
				continue
			separator: str = token.separator
			# 行末へ積んだ記号の前の区切も同じ規則（行コメントの本文の中の，空白の後の記号）
			opener: str = next(opener for opener in OPENERS[inner] if token.text.startswith(opener))
			stacked: list[str] = (
				re.findall(rf"([ \t\u3000]+){re.escape(opener)} ", token.text[len(opener):]) if opener not in BLOCKS else []
			)
			for gap in [separator, *stacked]:
				if gap != " " and (not gap or gap.strip("\t")):
					found.append((token.line, f"{token.line + 1} 行目の {token.text[:40]}：前置区切り {gap!r}"))
	# 違反の一覧（行の順）の返戻
	return [problem for _, problem in sorted(found)]

def comment_body(text: str) -> str:
	"""記号と継続行の装飾を除いたコメント本文を返す
	計算量：テキストの文字数 B に対して O(B)
	引数：
		text: コメントのテキスト又は本文
	戻値：
		正規化の対象の本文
	"""
	# ドキュメントの開始記号は１つの記法として剥がし，本文の `!` と区別する（整形仕様 3.2）
	opening: str = next((opening for opening in DOCUMENTS if text.startswith(opening)), "")
	if text.startswith(("/*", "<!--")):
		closing: str = "-->" if text.startswith("<!--") else "*/"
		opening = opening or ("<!--" if text.startswith("<!--") else "/*")
		if text.endswith(closing):
			text = text[:-len(closing)]
		text = text[len(opening):]
		text = re.sub(r"(?m)^[ \t]*\* ?", "", text)
	elif opening:
		text = text[len(opening):]
	else:
		text = re.sub(r"^(?://+|#+) ?", "", text)
	# 本文の返戻
	return text

def trailing_separators(parsed: list[Region], should_include_blocked: bool = False) -> list[tuple[str, str, tuple[int, int]]]:
	"""指定言語の行末へ積んだ記号毎の本文・前置区切・コード境界を返す
	行末コメントは同じ行の前にコード又はコメントが在り，同じ行にコードが続かない物とする
	原文では移動前の記録も収集し，出力ではテキストを保つ領域の物を除く
	計算量：領域数 E・トークン数 N・全トークンの本文量 B に対して O(E+N+B log(B+1))
	引数：
		parsed: 解析済の領域の一覧
		should_include_blocked: 移動前の原文の記録として差込禁止領域の物も収集するか
	戻値：
		行末の記号毎の本文・前置区切・領域とコードの境界の一覧
	"""
	# 行末コメントの区切の収集
	found: list[tuple[int, int, int, str, str, tuple[int, int]]] = []
	for region_idx, (tokens, _, inner, _, _) in enumerate(parsed):
		if inner == "html":
			continue
		blocked: dict[int, int] = blocked_lines(tokens, inner)
		# 後に続くコードのトークンの始まる行（末尾から遡って求め，無ければ最終行の後）
		code_pos: int = sum(not token.is_comment for token in tokens)
		following: int = tokens[-1].logical_end_line + 1 if tokens else 0
		for token in reversed(tokens):
			if not token.is_comment:
				code_pos -= 1
				following = token.logical_line + (inner == "php" and token.text == "?>")
				continue
			if (
				token.is_standalone or not should_include_blocked and token.line in blocked or not token.has_preceding_text or
				following <= token.logical_end_line
			):
				continue
			opener: str = next(opener for opener in OPENERS[inner] if token.text.startswith(opener))
			# 最初の記録の開始記号は，行・囲み共に文キーで記法を識別する迄保つ
			parts: list[str] = re.split(rf"([ \t\u3000]+){re.escape(opener)} ", token.text) if opener not in BLOCKS else [token.text]
			gap: str = token.separator
			for part, preceding_gap in zip(parts[0::2], [gap, *parts[1::2]]):
				body: str = part if should_include_blocked else part.split("\n", 1)[0]
				found.append((token.line, token.col, len(found), body, preceding_gap, (region_idx, code_pos)))
	# 本文と前置区切の組の一覧（原文の順）の返戻
	return [(body, gap, anchor) for _, _, _, body, gap, anchor in sorted(found)]

# 説明文の括弧・引用の対（開き，閉じ）
# 製品の `SentenceBrackets` と同じ組を仕様から定める（製品から読まない）
SENTENCE_BRACKETS: dict[str, str] = {
	"(": ")",
	"[": "]",
	"{": "}",
	"‘": "’",
	"“": "”",
	"〈": "〉",
	"《": "》",
	"「": "」",
	"『": "』",
	"【": "】",
	"〔": "〕",
	"〖": "〗",
	"〘": "〙",
	"〚": "〛",
	"（": "）",
	"＜": "＞",
	"［": "］",
	"｛": "｝"
}

def comment_alnum_kind(char: str) -> str:
	"""説明文の英数字の種類の取得

	英字はラテン文字と其の互換形とし，仮名・漢字は含めない
	製品の表を参照せず，Unicode の数字・文字名・互換分解から判定する
	計算量：固定した Unicode の分解長の上限に対し O(1)
	引数：
		char: 判定する１文字（境界の外側は空の文字列）
	戻値：
		英字は L，数字は N，其の他は空の文字列
	"""
	if not char:
		# 境界の外側の返戻
		return ""
	if unicodedata.category(char).startswith("N"):
		if (
			unicodedata.east_asian_width(char) == "W" and
			not any(part.isascii() and part.isalnum() for part in unicodedata.normalize("NFKD", char))
		):
			# 英数字の互換形でない和文の漢数字の返戻
			return ""
		# 数字の種類の返戻
		return "N"
	# ラテン文字・互換形の英字の種類の返戻
	return (
		"L" if char.isalpha() and any("LATIN" in unicodedata.name(letter, "") for letter in unicodedata.normalize("NFKD", char)) else ""
	)

def comment_enclosures(body: str) -> tuple[bytearray, bytearray, bytearray]:
	"""本文の各位置に，囲みの内部・コードの保護・引用の読みを対応付ける
	引用のビットは引用符・エスケープ・アポストロフィ・開きの順で，句点除去後の読みに使う
	計算量：本文の文字数 B に対して O(B)
	引数：
		body: コメントの本文
	戻値：
		囲みの内部，コードの保護，引用の読みの各位置の配列
	"""
	# 囲み・コード片・引用の元の読みの確定（引用のビットは引用符・エスケープ・アポストロフィ・開きの順）
	inside: bytearray = bytearray(len(body))
	protected: bytearray = bytearray(len(body))
	quotes: bytearray = bytearray(len(body))
	pending: list[str] = []
	backticks: int = 0
	slashes: int = 0
	pos: int = 0
	while pos < len(body):
		char: str = body[pos]
		inside[pos] = bool(pending)
		is_escaped: bool = bool(slashes % 2)
		if char == "`" and (backticks or not is_escaped):
			end: int = pos + 1
			while end < len(body) and body[end] == "`":
				end += 1
			protected[pos:end] = b"\1" * (end - pos)
			if not backticks:
				quotes[pos] = 1 | 8
				backticks = end - pos
			elif backticks == end - pos:
				backticks = 0
			slashes = 0
			pos = end
			continue
		if backticks:
			protected[pos] = 1
		else:
			is_quote: bool = char in "\"'`‘’“”「」『』"
			if is_quote:
				quotes[pos] = 1 | (2 if is_escaped else 0)
			is_apostrophe: bool = (
				char == "'" and bool(pos) and pos + 1 < len(body) and bool(comment_alnum_kind(body[pos - 1])) and
				bool(comment_alnum_kind(body[pos + 1]))
			)
			if is_apostrophe:
				quotes[pos] |= 4
			elif not is_quote or not is_escaped:
				if pending and char == pending[-1]:
					pending.pop()
				elif char in SENTENCE_BRACKETS or char in "\"'" and not (char == "'" and pos and comment_alnum_kind(body[pos - 1])):
					pending.append(SENTENCE_BRACKETS.get(char, char))
					if is_quote:
						quotes[pos] |= 8
		slashes = slashes + 1 if char == "\\" else 0
		pos += 1
	# 各位置の保持条件の返戻
	return inside, protected, quotes

def comment_character_kind(char: str) -> tuple[bool, bool]:
	"""全角文字と，全角の区切・括弧・連結記号かを返す
	文字の対応表は追加せず，Unicode の文字種・名前・互換形から判定する
	計算量：O(1)
	引数：
		char: 判定する文字
	戻値：
		全角文字か，隣接空白を除く全角記号かの組
	"""
	# Unicode の属性による表音文字・単位・通貨と全角形の区別
	category: str = unicodedata.category(char)
	name: str = unicodedata.name(char, "")
	width: str = unicodedata.east_asian_width(char)
	is_letter: bool = category in ("Ll", "Lt", "Lu") or category[0] == "M" or char.isalpha() and (
		name.startswith("HANGUL ") or any(
			unicodedata.category(part) in ("Ll", "Lt", "Lu") or
			any(kind in unicodedata.name(part, "") for kind in ("LETTER", "LIGATURE", "SYLLAB")) for part in
			unicodedata.normalize("NFKD", char)
		)
	)
	is_phonetic: bool = is_letter and (name.startswith("HANGUL ") or width != "W" and "KATAKANA" not in name)
	is_unit: bool = name == "DEGREE SIGN" or name == "DOUBLE PRIME" or name == "PER MILLE SIGN" or name == "PRIME"
	is_wide: bool = width == "F" or not (char.isascii() or char.isspace() or category in ("Cf", "Sc") or is_phonetic or is_unit)
	# 括弧・引用・ハイフン・演算子と区切子の分類（参照記号等の独立した記号へ禁止を広げない）
	is_mark: bool = is_wide and (
		category in ("Pd", "Pe", "Pf", "Pi", "Ps", "Sm") or name.endswith("COLON") or name.endswith("COMMA") or
		name.endswith("FULL STOP") or name.endswith("MIDDLE DOT") or unicodedata.normalize("NFKC", char) in "!&*/?"
	)
	# 幅と隣接空白の禁止条件の返戻
	return is_wide, is_mark

def is_comment_space_removable(left: str, right: str) -> bool:
	"""全角文字同士の間や記号に接する，不要な本文の空白かを返す
	コードと引用の保持条件は呼出側で先に確かめる
	計算量：O(1)
	引数：
		left: 空白の前の文字
		right: 空白の後の文字
	戻値：
		空白を除く隣接なら真
	"""
	# 矢印とダッシュの前後は可読性の為に空白を許す
	if left == "—" or left == "→" or left == "↔" or right == "—" or right == "→" or right == "↔":
		# 規約の例外である事の返戻
		return False
	is_wide_left: bool
	is_mark_left: bool
	is_wide_left, is_mark_left = comment_character_kind(left)
	is_wide_right: bool
	is_mark_right: bool
	is_wide_right, is_mark_right = comment_character_kind(right)
	# 半角の括弧の外側でも，隣が括弧又は区切子なら空白を置かない
	is_left_boundary: bool = left in ",:;<>" or unicodedata.category(left) in ("Pe", "Ps")
	is_right_boundary: bool = right in ",:;<>" or unicodedata.category(right) in ("Pe", "Ps")
	# 残す半角の点は語や数値の一部なので区切子として扱わない
	return (
		right in ",:;" or left == "<" or right == ">" or is_wide_left and is_wide_right or is_mark_left or is_mark_right or
		is_left_boundary and is_right_boundary
	)

def logical_lines(body: str) -> list[str]:
	"""囲みを保持し，句点を除いたコメント本文の論理行を返す
	囲みの対応を先に確定し，句点は後ろから１度だけ読み，除去後の隣接を次の判定へ渡す（整形仕様 7.3）
	引用の読みを変える除去位置は空白で置き換え，其れ以外は全角文字同士の間と括弧の内側の余白を除く
	計算量：本文の文字数 B に対して O(B)
	引数：
		body: コメントの本文
	戻値：
		元の順の論理行の一覧
	"""
	# 同じ囲みの事実を文区切・句点除去・引用の隣接の保持に使用する
	closers: str = "\"'" + "".join(SENTENCE_BRACKETS.values())
	inside: bytearray
	protected: bytearray
	quotes: bytearray
	inside, protected, quotes = comment_enclosures(body)
	# 本文頭の列挙番号の点は句点として除かない
	enumerators: set[int] = set()
	for match in re.finditer(r"(?m)^[ \t]*", body):
		pos: int = match.end()
		kind: str = comment_alnum_kind(body[pos:pos + 1])
		if kind == "N":
			while pos < len(body) and comment_alnum_kind(body[pos]) == "N":
				pos += 1
		elif kind == "L":
			pos += 1
		if kind and body[pos:pos + 1] == ".":
			enumerators.add(pos)
	parts: list[str] = list(body)
	boundaries: bytearray = bytearray(len(body))
	next_chars: list[str] = [""] * len(body)
	tail: str = ""
	tail_pos: int = -1
	# 閉じ：対応表の閉じと，開きでない引用符（開く引用符は閉じに数えない）
	is_closer: Callable[[], bool] = lambda: tail in closers and not (tail in "\"'" and quotes[tail_pos] & 8)
	for pos in range(len(body) - 1, -1, -1):
		char: str = body[pos]
		next_chars[pos] = tail
		if not protected[pos]:
			if char in "。．" or char == "." and pos not in enumerators and (not tail or tail.isspace() or is_closer()):
				parts[pos] = ""
				boundaries[pos] = not inside[pos]
				if boundaries[pos]:
					tail = "\n"
					tail_pos = -1
				continue
			if not inside[pos] and (char in "！？" or char in "!?" and (not tail or tail.isspace() or is_closer())):
				boundaries[pos] = 1
		tail = char
		tail_pos = pos
	# 句点の除去で引用の開閉・アポストロフィ・エスケープが変わる隣接だけを空白で隔てる
	kept_spaces: bytearray = bytearray(len(body))
	previous: str = ""
	slashes: int = 0
	for pos, part in enumerate(parts):
		if quotes[pos]:
			next_char: str = next_chars[pos]
			has_word_before: bool = bool(comment_alnum_kind(previous))
			has_word_after: bool = bool(comment_alnum_kind(next_char))
			has_changed: bool = (
				bool(quotes[pos] & 2) != bool(slashes % 2) or body[pos] == "'" and
				(bool(quotes[pos] & 4) != (has_word_before and has_word_after) or bool(quotes[pos] & 8) and has_word_before)
			)
			if has_changed:
				if pos and not parts[pos - 1]:
					parts[pos - 1] = " "
					kept_spaces[pos - 1] = 1
					slashes = 0
				elif pos + 1 < len(parts) and not parts[pos + 1]:
					parts[pos + 1] = " "
					kept_spaces[pos + 1] = 1
		if part:
			previous = part
			slashes = slashes + 1 if part == "\\" else 0
	# 連続する文末記号を同じ行へ残し，記号だけの追加行は作らない
	found: list[str] = []
	line: list[str] = []
	has_text: bool = False
	should_separate: bool = False
	has_space: bool = False
	is_space_kept: bool = False
	is_after_open: bool = False
	is_last_protected: bool = False
	chapter: int = 0 # 「第」，数字列，章・節の終端の順で，例外の空白を識別する
	slashes = 0
	for pos, part in enumerate(parts):
		if (
			part == "\n" or
			should_separate and part and not part.isspace() and (part not in "!?。！．？" + closers or part in "\"'" and quotes[pos] & 8)
		):
			if text := "".join(line).strip():
				found.append(text)
			line.clear()
			has_text = False
			should_separate = False
			has_space = False
			is_space_kept = False
			is_after_open = False
			is_last_protected = False
			chapter = 0
			slashes = 0
		# コードと引用の保持用スペースを残し，全角文字同士の間と括弧の内側等の余白を除く
		if part and part != "\n":
			if part.isspace() and not protected[pos]:
				has_space = True
				is_space_kept = is_space_kept or bool(kept_spaces[pos])
			else:
				# 再読時も，空白の除去で引用の読みが変わる隣接は１スペースで隔てる
				if has_space and quotes[pos] and line:
					has_word_before = bool(comment_alnum_kind(line[-1]))
					has_word_after = bool(comment_alnum_kind(next_chars[pos]))
					is_apostrophe: bool = has_word_before and has_word_after
					is_space_kept = is_space_kept or (
						bool(quotes[pos] & 2) != bool(slashes % 2) or
						part == "'" and (bool(quotes[pos] & 4) != is_apostrophe or bool(quotes[pos] & 8) and has_word_before)
					)
				is_closing: bool = part in closers and not protected[pos] and not quotes[pos] & (2 | 4 | 8)
				if has_space and (
					is_space_kept or line and not is_after_open and not is_closing and
					(is_last_protected or protected[pos] or chapter == 3 or not is_comment_space_removable(line[-1], part))
				):
					line.append(" ")
					slashes = 0
				has_space = False
				is_space_kept = False
				line.append(part)
				is_last_protected = bool(protected[pos])
				# 章番号を前から１度読む（成長する行の後方再探索は行わない）
				if part == "第":
					chapter = 1
				elif chapter in (1, 2) and comment_alnum_kind(part) == "N":
					chapter = 2
				else:
					chapter = 3 if chapter == 2 and part in "章節" else 0
				slashes = slashes + 1 if part == "\\" else 0
				is_after_open = bool(not protected[pos] and (part in SENTENCE_BRACKETS and not quotes[pos] & 2 or quotes[pos] & 8))
			if not part.isspace() and part not in "!?。！．？":
				has_text = True
		if boundaries[pos] and has_text:
			should_separate = True
	if text := "".join(line).strip():
		found.append(text)
	# 非空の論理行の返戻
	return found

def sentence_keys(body: str, lang: str = "") -> list[str]:
	"""論理行毎に空白とコメントの記号を除いた照合キーを返す
	計算量：本文の文字数 B に対して O(B)
	引数：
		body: コメントの本文
		lang: ドキュメント形式を決める言語
	戻値：
		重複した出現も保持したキーの並び
	"""
	# 記法の装飾と同期で補う空のタグを本文と取り違えない識別
	is_document: bool = body.startswith(DOCUMENTS)
	body = comment_body(body)
	if is_document:
		# 同義タグと項目の記法だけを揃え，説明・型・名前・コード例は保持する
		lines: list[str] = body.split("\n")
		_, protected, _ = comment_enclosures(body)
		offset: int = 0
		is_in_parameters: bool = False
		for idx, line in enumerate(lines):
			first: int = offset + len(line) - len(line.lstrip())
			is_protected: bool = first < offset + len(line) and bool(protected[first])
			offset += len(line) + 1
			if is_protected:
				continue
			if lang in ("javascript", "javascriptreact", "typescript", "typescriptreact"):
				lines[idx] = re.sub(
					r"^(\s*)@(arg|argument|returns)\b",
					lambda match: match[1] + ("@return" if match[2] == "returns" else "@param"),
					line
				)
			elif lang == "swift":
				if re.fullmatch(r"\s*-\s*Parameters:\s*", line):
					is_in_parameters = True
					lines[idx] = ""
				elif is_in_parameters:
					if re.match(r"\s*-\s+[^:]+:", line) and not re.match(r"\s*-\s+Parameter\s", line):
						lines[idx] = re.sub(r"^(\s*-\s+)", r"\1Parameter ", line)
					elif line.strip() and not line[:1].isspace():
						is_in_parameters = False
		body = "\n".join(lines)
		# バッククォートのコード例を保持し，其の外のタグと空の骨格だけを分離する
		protected: bytearray
		_, protected, _ = comment_enclosures(body)
		code: memoryview = memoryview(protected)
		body = DOCUMENT_MARKUP.sub(lambda match: match.group() if code[match.start()] else "\n", body)
	# フェンス内でも物理行毎の出現を保持する照合キーの返戻
	parts: list[str] = logical_lines(body)
	_, protected, _ = comment_enclosures("\n".join(parts))
	found: list[str] = []
	pos: int = 0
	for part in parts:
		key: str = (
			"".join(char for idx, char in enumerate(part) if protected[pos + idx] or not (char.isspace() or char in "/*#<>=、，,-"))
		)
		if key:
			found.append(key)
		pos += len(part) + 1
	# 論理行毎のキーの返戻
	return found

def token_sentence_keys(tokens: list[Token], lang: str) -> list[list[str]]:
	"""行形式のドキュメントを跨ぐ XML タグを分離し，トークン毎の本文キーを返す
	引用属性の読取だけを継続行と共有し，タグの外の本文は元の記録毎に照合する
	計算量：トークン数 N・本文の総文字数 B に対して O(N+B)
	引数：
		tokens: 領域のトークンの並び
		lang: ドキュメント形式を決める言語
	戻値：
		トークンと同じ長さのキーの並び
	"""
	found: list[list[str]] = [[] for _ in tokens]
	idx: int = 0
	while idx < len(tokens):
		token: Token = tokens[idx]
		if not token.is_comment:
			idx += 1
			continue
		end: int = idx + 1
		if token.is_standalone and token.text.startswith(("//!", "///")):
			while (
				end < len(tokens) and tokens[end].is_standalone and tokens[end].text.startswith(token.text[:3]) and
				tokens[end].logical_line == tokens[end - 1].logical_end_line + 1
			):
				end += 1
		if end == idx + 1 or not any("<" in part.text for part in tokens[idx:end]):
			for part_idx in range(idx, end):
				found[part_idx] = sentence_keys(tokens[part_idx].text, lang)
		else:
			parts: list[str] = [comment_body(part.text) for part in tokens[idx:end]]
			body: str = "\n".join(parts)
			_, protected, _ = comment_enclosures(body)
			# 複数行のタグ内の文字だけを除き，記録と論理行の境界を動かさない
			body = DOCUMENT_MARKUP.sub(
				lambda match: "\n" * match.group().count("\n") if "\n" in match.group() and not protected[match.start()] else match.group(),
				body
			)
			lines: list[str] = body.split("\n")
			line: int = 0
			for part_idx, part in enumerate(parts):
				count: int = part.count("\n") + 1
				found[idx + part_idx] = sentence_keys(token.text[:3] + "\n".join(lines[line:line + count]), lang)
				line += count
		idx = end
	# 記録毎の本文キーの返戻
	return found

def has_same_code(before: list[Region], after: list[Region]) -> bool:
	"""対応する領域のコードのトークン列が一致するかを返す
	計算量：領域数 E・トークン数 N・テキスト量 B に対して O(E+N+B)
	引数：
		before: 原文の領域
		after: 出力の領域
	戻値：
		コードの並びが一致するなら真
	"""
	# 原文と出力のコードの一致の返戻
	return len(before) == len(after) and all(
		src.lang == output.lang and [token.text for token in src.tokens if not token.is_comment] ==
		[token.text for token in output.tokens if not token.is_comment] for src, output in zip(before, after)
	)

def separator_record_mismatches(before: list[Region], after: list[Region]) -> list[str]:
	"""出力の行末コメントの前置区切を，同じ本文の原文の記録の何れかのタブの数と照合する（整形仕様 7.1, 7.6）
	並替・統合後も各出現を１度だけ対応させ，原文の記録から分けた追加の論理行はタブ０個と照合する
	計算量：領域数 E・トークン数 N・全トークンの本文量 B に対して O(E+N+B)
	引数：
		before: 入力の解析済の領域又はトークンの並び
		after: 出力の解析済の領域又はトークンの並び
	戻値：
		原文と出力の前置区切の違反の一覧
	"""
	# 本文毎の原文の記録のタブの数
	recorded: dict[str, Counter[int]] = {}
	source_records: list[tuple[str, str, tuple[int, int]]] = trailing_separators(before, True)
	output_records: list[tuple[str, str, tuple[int, int]]] = trailing_separators(after)
	can_match_order: bool|None = None
	for body, gap, _ in source_records:
		# 元の１記録を文毎に分けた追加の記号はタブ０個の記録として扱う
		pieces: list[str] = sentence_keys(body)
		for idx, part in enumerate(pieces):
			recorded.setdefault(part, Counter())[gap.count("\t") if not idx else 0] += 1
	found: list[str] = []
	for record_idx, (body, gap, anchor) in enumerate(output_records):
		for piece_idx, key in enumerate(sentence_keys(body)):
			if key not in recorded:
				if can_match_order is None:
					can_match_order = len(source_records) == len(output_records) and has_same_code(before, after)
				# コードと出現順が同じなら，本文に依存せず記録の前置区切を照合する
				if can_match_order and source_records[record_idx][2] == anchor:
					expected_tabs: int = source_records[record_idx][1].count("\t") if not piece_idx else 0
					if gap != ("\t" * expected_tabs if expected_tabs else " "):
						found.append(f"{key[:40]}：前置区切り {gap!r}，対応する原文のタブの数 {expected_tabs}")
				# 本文も記録の対応も識別出来ない場合だけの検査の見送
				continue
			tabs: int = gap.count("\t")
			if gap != ("\t" * tabs if tabs else " ") or not recorded[key][tabs]:
				found.append(f"{key[:40]}：前置区切り {gap!r}，原文のタブの数と残数 {dict(recorded[key])}")
			else:
				recorded[key][tabs] -= 1
	# 食違の一覧の返戻
	return found

def documentation_start(tokens: list[Token], pos: int) -> int:
	"""定義の見出と前置デコレータから統合先の開始位置を返す
	計算量：見出のトークン数 H に対して O(H)
	引数：
		tokens: コメントを除くコードのトークン列
		pos: 原文の後続トークンの位置
	戻値：
		ドキュメントの統合先の開始位置
	"""
	# 本体の開きの直前の見出の識別
	start: int = pos
	if pos < len(tokens) and tokens[pos].text == "{" and pos and tokens[pos - 1].text == ")":
		depth: int = 0
		while start:
			start -= 1
			depth += (tokens[start].text == ")") - (tokens[start].text == "(")
			if not depth:
				break
		if not start or tokens[start - 1].text in ("catch", "for", "if", "switch", "while", "with"):
			# 定義の見出でない場合の元の位置の返戻
			return pos
		while start and tokens[start - 1].text not in (";", "{", "}"):
			start -= 1
	# 定義の前置デコレータとの同一所属
	while start and tokens[start - 1].text.startswith("@"):
		start -= 1
	# 見出の開始の返戻
	return start

def subsequence_mapping(before: list[str], after: list[str]) -> dict[int, int]|None:
	"""一方が他方の部分列なら，原文順を保った右端の対応を１度の走査で返す
	計算量：双方のトークン数の和 N に対して O(N)
	引数：
		before: 対応元のトークンの綴り
		after: 対応先のトークンの綴り
	戻値：
		右側を優先した位置の対応．部分列でない場合は None
	"""
	found: dict[int, int] = {}
	pos: int = len(after) - 1
	for idx in range(len(before) - 1, -1, -1):
		while pos > -1 and before[idx] != after[pos]:
			pos -= 1
		if pos < 0:
			# 部分列でない事の返戻
			return None
		found[idx] = pos
		pos -= 1
	# 右端の対応の返戻
	return found

def quoted_key(text: str, lang: str) -> str:
	"""引用符だけが異なる通常の文字列を値で対応付け，補間・専用リテラルは保つ
	計算量：テキストの文字数 B に対して O(B)
	引数：
		text: トークンの綴り
		lang: 言語キー
	戻値：
		安全にデコードした値のキー又は変更しない綴り
	"""
	lang = { "javascriptreact": "javascript", "typescriptreact": "typescript" }.get(lang, lang)
	if (
		lang not in ("php", "javascript", "typescript", "ruby", "python") or len(text) < 2 or text[0] not in "\"'" or
		text[-1] != text[0] or text.startswith(("'''", "\"\"\""))
	):
		# 専用の記法の返戻
		return text
	if lang == "python":
		try:
			value: object = ast.literal_eval(text)
		except (SyntaxError, ValueError):
			# デコード出来ない綴りの返戻
			return text
		# 通常の文字列だけの値の返戻
		return "\0string:" + value if isinstance(value, str) else text
	parts: list[str] = []
	escapes: dict[str, str] = { "b": "\b", "f": "\f", "n": "\n", "r": "\r", "t": "\t", "v": "\v" }
	if lang == "php":
		del escapes["b"]
	if lang in ("php", "ruby"):
		escapes["e"] = "\x1b"
	if lang == "ruby":
		escapes.update({ "a": "\a", "s": " " })
	idx: int = 1
	while idx < len(text) - 1:
		piece: str = text[idx]
		if piece == "\\":
			idx += 1
			if idx >= len(text) - 1:
				# 不正なエスケープの綴りの返戻
				return text
			piece = text[idx]
			if piece in (text[0], "\\") or lang in ("javascript", "typescript") and piece in "\"'":
				parts.append(piece)
			elif lang in ("php", "ruby") and text[0] == "'":
				parts.append("\\")
				continue
			elif piece in escapes:
				parts.append(escapes[piece])
			elif piece == "$" and lang == "php" or piece == "#" and lang == "ruby":
				parts.append(piece)
			elif lang in ("javascript", "typescript") and piece in "xu":
				start: int = idx + 1
				end: int = start + (2 if piece == "x" else 4)
				is_braced: bool = piece == "u" and text[start:start + 1] == "{"
				if is_braced:
					start += 1
					end = text.find("}", start)
				if end < start + 1 or end > len(text) - 1 or is_braced and end - start > 6:
					# 未対応のエスケープの返戻
					return text
				try:
					value = int(text[start:end], 16)
				except ValueError:
					# 不正な数値エスケープの返戻
					return text
				if value > 0X10FFFF:
					# 範囲外のコードポイントの返戻
					return text
				parts.append(chr(value))
				idx = end if is_braced else end - 1
			elif lang in ("javascript", "typescript") and piece == "0" and not text[idx + 1:idx + 2].isdigit():
				parts.append("\0")
			elif lang in ("javascript", "typescript") and piece in "\r\n":
				if piece == "\r" and text[idx + 1:idx + 2] == "\n":
					idx += 1
			elif lang in ("javascript", "typescript") and not piece.isdigit():
				parts.append(piece)
			elif lang == "php" and piece not in "xu" and not piece.isdigit():
				parts.extend(("\\", piece))
			else:
				# 値を確定出来ない記法の返戻
				return text
		else:
			if text[0] == "\"" and (
				lang == "ruby" and piece == "#" and text[idx + 1:idx + 2] in ("{", "@", "$") or lang == "php" and piece == "$" and
				(text[idx + 1:idx + 2].isalpha() or text[idx + 1:idx + 2] in ("_", "{") or text[idx + 1:idx + 2] and ord(text[idx + 1]) > 127)
			):
				# 補間を値の文字列へ読み替えない事の返戻
				return text
			parts.append(piece)
		idx += 1
	value = "".join(parts)
	if lang in ("javascript", "typescript"):
		value = value.encode("utf-16-le", errors="surrogatepass").hex()
	# 引用符に依存しない通常の値のキーの返戻
	return "\0string:" + value

def definition_fences(texts: list[str]) -> dict[tuple[str, ...], list[tuple[int, ...]]]:
	"""C 系の関数の見出と本体の閉じを，同じ見出の出現順で区分する
	文の終端と括弧の対から見出を読み，宣言の分割・統合で変わる本体をキーへ含めない
	計算量：トークン数 N・見出の綴りの文字数 B に対して O(N+B)
	引数：
		texts: コメントを除くコードのトークンのキー
	戻値：
		見出毎の，見出の全位置と本体の閉じの位置
	"""
	# 括弧の対と，見出を開始出来る文の境界の収集
	stack: list[int] = []
	pairs: dict[int, int] = {}
	starts: list[int] = []
	start: int = 0
	for idx, text in enumerate(texts):
		starts.append(start)
		if text in ("(", "{"):
			stack.append(idx)
		elif text in (")", "}") and stack and texts[stack[-1]] == ("(" if text == ")" else "{"):
			opening: int = stack.pop()
			pairs[opening], pairs[idx] = idx, opening
		if text in (";", "{", "}"):
			start = idx + 1
	found: dict[tuple[str, ...], list[tuple[int, ...]]] = {}
	# 制御の見出と，名前を持つ関数の定義の区別
	for idx, text in enumerate(texts):
		if text != "{" or not idx or texts[idx - 1] != ")" or idx not in pairs:
			continue
		opening = pairs.get(idx - 1, 0)
		if (
			not opening or not WORD.fullmatch(texts[opening - 1]) or texts[opening - 1] in ("catch", "for", "if", "switch", "while", "with")
		):
			continue
		start = starts[opening - 1]
		key: tuple[str, ...] = tuple(texts[start:idx])
		found.setdefault(key, []).append((*range(start, idx + 1), pairs[idx]))
	# 本体の変換に依らない所属の返戻
	return found

def map_brace_pairs(before: list[str], after: list[str], mapping: dict[int, int]) -> None:
	"""対応済の内容を同じ対で囲む波括弧だけを追加する
	除いた外側のラッパーの閉じを，残る内側の本体の閉じへ片側だけ対応させない
	計算量：双方のトークン数の和 N に対して O(N)
	引数：
		before: 原文のトークンのキー
		after: 出力のトークンのキー
		mapping: 内容の対応と，追加する対の格納先
	"""
	# 補完したトークンを除き，対の内部の最初と最後の共通の内容を１度集める
	inverse: dict[int, int] = { target: start for start, target in mapping.items() }
	groups: list[dict[tuple[bool, int, int], list[tuple[int, int]]]] = []
	for texts, links in (before, mapping), (after, inverse):
		previous: list[int] = []
		following: list[int] = [len(after)] * (len(texts) + 1)
		last: int = -1
		for idx, text in enumerate(texts):
			previous.append(last)
			if idx in links and text not in ("{", "}"):
				last = mapping[idx] if texts is before else idx
		for idx in range(len(texts) - 1, -1, -1):
			if idx in links and texts[idx] not in ("{", "}"):
				following[idx] = mapping[idx] if texts is before else idx
			else:
				following[idx] = following[idx + 1]
		stack: list[int] = []
		pairs: dict[tuple[bool, int, int], list[tuple[int, int]]] = {}
		for idx, text in enumerate(texts):
			if text == "{":
				stack.append(idx)
			elif text == "}" and stack:
				opening: int = stack.pop()
				first: int = following[opening + 1]
				last = previous[idx]
				# 内容が無い対は，直前と直後の対応済の境界で所属を区別する
				key: tuple[bool, int, int] = (False, first, last) if first <= last else (True, previous[opening], following[idx + 1])
				pairs.setdefault(key, []).append((opening, idx))
		groups.append(pairs)
	# 同じ内容の入子を推測せず，空の兄弟は同じ数の出現順で対応付ける
	for key, pairs in groups[0].items():
		targets: list[tuple[int, int]] = groups[1].get(key, [])
		if (
			len(pairs) != len(targets) or len(pairs) != 1 and
			(not key[0] or not all(left[1] < right[0] for items in (pairs, targets) for left, right in zip(items, items[1:])))
		):
			continue
		for pair, target_pair in zip(pairs, targets):
			for start, target in zip(pair, target_pair):
				mapping.setdefault(start, target)
	# 終了
	return

def token_mapping(before: list[str], after: list[str], lang: str = "") -> dict[int, int]|None:
	"""定義の見出と一意の連続トークンで区分し，各区間を右から１度だけ対応付ける
	所属の無い反復の並替は推測せず，照合不能とする
	計算量：トークン数 N・綴りの文字数 B に対して平均 O(N+B)
	引数：
		before: 原文のトークンのキー
		after: 出力のトークンのキー
		lang: 言語キー
	戻値：
		原文順を保つ位置の対応．所属を決めれない混在は None
	"""
	if before == after:
		# 同じ位置の対応の返戻
		return { idx: idx for idx in range(len(before)) }
	insertions: dict[int, int]|None = subsequence_mapping(before, after)
	if insertions is not None:
		# 追加だけの対応の返戻
		return insertions
	deletions: dict[int, int]|None = subsequence_mapping(after, before)
	if deletions is not None:
		# 除去だけの対応の返戻
		return { src: output for output, src in deletions.items() }
	left_counts: Counter[str] = Counter(before)
	right_counts: Counter[str] = Counter(after)
	is_permuted: bool = False
	if left_counts == right_counts:
		kept: list[list[str]] = []
		for texts in before, after:
			words: list[str] = []
			idx: int = 0
			while idx < len(texts):
				if idx + 1 < len(texts) and texts[idx] in ("+", "-") and texts[idx + 1] == texts[idx]:
					idx += 2
					continue
				if texts[idx] not in ("(", ")", ";", "[", "]", "const", "{", "}"):
					words.append(texts[idx])
				idx += 1
			kept.append(words)
		is_permuted = kept[0] != kept[1]
	owners: list[dict[tuple[str, str], list[int]]] = []
	for texts in before, after:
		heads: dict[tuple[str, str], list[int]] = {}
		for idx in range(len(texts) - 1):
			if texts[idx] in ("class", "def", "enum", "fn", "fun", "func", "function", "import", "interface", "struct"):
				heads.setdefault((texts[idx], texts[idx + 1]), []).append(idx)
		owners.append(heads)
	owned: dict[int, int] = {}
	for key, starts in owners[0].items():
		ends: list[int] = owners[1].get(key, [])
		if len(starts) == len(ends):
			for start, end in zip(starts, ends):
				owned[start], owned[start + 1] = end, end + 1
	if lang in ("c", "cpp", "h"):
		left_fences: dict[tuple[str, ...], list[tuple[int, ...]]] = definition_fences(before)
		right_fences: dict[tuple[str, ...], list[tuple[int, ...]]] = definition_fences(after)
		for key, heads in left_fences.items():
			targets: list[tuple[int, ...]] = right_fences.get(key, [])
			if len(heads) == len(targets):
				for head, target in zip(heads, targets):
					owned.update(zip(head, target))
	if not owned and (is_permuted or not any(count == 1 and right_counts[text] == 1 for text, count in left_counts.items())):
		# 独立した所属を持たない反復の混在の返戻
		return None
	next_owner: list[int] = [len(after)] * (len(before) + 3)
	limit: int = len(after)
	for idx in range(len(before) - 1, -1, -1):
		if idx in owned:
			limit = owned[idx]
		next_owner[idx] = limit
	positions: dict[tuple[str, ...], int] = {}
	for idx in range(len(after) - 2):
		key: tuple[str, ...] = tuple(after[idx:idx + 3])
		positions[key] = idx if key not in positions else -1
	counts: Counter[tuple[str, ...]] = Counter(tuple(before[idx:idx + 3]) for idx in range(len(before) - 2))
	boundaries: list[tuple[int, int]] = [(-1, -1)]
	for idx in range(len(before)):
		if idx in owned:
			point: int = owned[idx]
			if idx > boundaries[-1][0] and point > boundaries[-1][1]:
				boundaries.append((idx, point))
			continue
		if idx > len(before) - 3:
			continue
		key = tuple(before[idx:idx + 3])
		point = positions.get(key, -1)
		if (
			counts[key] == 1 and point > -1 and point + 2 < next_owner[idx + 3] and idx > boundaries[-1][0] and
			point > boundaries[-1][1] and not any(idx + offset in owned for offset in (1, 2))
		):
			# 波括弧は連続した綴りが一意でも，開閉の対を確認してから対応させる
			boundaries.extend((idx + offset, point + offset) for offset in range(3) if before[idx + offset] not in ("{", "}"))
	boundaries.append((len(before), len(after)))
	imports: list[bool] = []
	is_import: bool = False
	for text in before:
		if text == "import" and lang in ("javascript", "javascriptreact", "typescript", "typescriptreact"):
			is_import = True
		elif text in (";", "from"):
			is_import = False
		imports.append(is_import)
	found: dict[int, int] = {}
	for (left, start), (right, end) in zip(boundaries, boundaries[1:]):
		words: list[list[str]] = []
		for texts, begin, stop in (before, left + 1, right), (after, start + 1, end):
			kept_words: list[str] = []
			idx = begin
			while idx < stop:
				if idx + 1 < stop and texts[idx] in ("+", "-") and texts[idx + 1] == texts[idx]:
					idx += 2
					continue
				if texts[idx] not in ("(", ")", ",", ";", "[", "]", "const", "final", "let", "mut", "return", "val", "var", "{", "}"):
					kept_words.append(texts[idx])
				idx += 1
			words.append(kept_words)
		if words[0] != words[1] and Counter(words[0]) == Counter(words[1]) and not imports[left + 1]:
			# 同じ所属内でも意味を持つトークンの並替を推測しない事の返戻
			return None
		if left > -1:
			found[left] = start
		queues: dict[str, list[int]] = {}
		for idx in range(start + 1, end):
			# 本体の波括弧は，内外の対を崩す片側だけの貪欲な対応へ使わない
			if after[idx] not in ("{", "}"):
				queues.setdefault(after[idx], []).append(idx)
		limit = end
		for idx in range(right - 1, left, -1):
			queue: list[int] = queues.get(before[idx], [])
			while queue and queue[-1] >= limit:
				queue.pop()
			if queue:
				point = queue.pop()
				found[idx] = point
				limit = point
	map_brace_pairs(before, after, found)
	# 境界の間で確定した対応の返戻
	return found

def modifier_headers(tokens: list[Token]) -> dict[tuple[str, ...], list[tuple[bool, int, int]]]:
	"""Ruby の先行条件と後置条件を，条件の綴りと出現順で対応付ける
	計算量：トークン数 N・綴りの文字数 B に対して O(N+B)
	引数：
		tokens: コメントを除くコードのトークン列
	戻値：
		条件毎の先行形の有無と範囲の出現列
	"""
	found: dict[tuple[str, ...], list[tuple[bool, int, int]]] = {}
	idx: int = 0
	while idx < len(tokens):
		token: Token = tokens[idx]
		if token.text not in ("if", "unless", "until", "while"):
			idx += 1
			continue
		is_head: bool = not idx or tokens[idx - 1].logical_end_line < token.logical_line or tokens[idx - 1].text in (";", "do", "then")
		start: int = idx
		idx += 1
		depth: int = 0
		line: int = token.logical_end_line
		while idx < len(tokens):
			part: Token = tokens[idx]
			if not depth and (part.logical_line > line or part.text in (";", "do", "then")):
				break
			depth += (part.text in ("(", "[", "{")) - (part.text in (")", "]", "}"))
			line = part.logical_end_line
			idx += 1
		values: list[str] = []
		for part in tokens[start:idx]:
			text: str = part.text
			if text.startswith(("%i[", "%w[")) and "\\" not in text:
				text = text[:3] + " ".join(text[3:-1].split()) + text[-1]
			values.append(text)
		found.setdefault(tuple(values), []).append((is_head, start, idx))
	# 同じ条件に属する範囲の返戻
	return found

def anchor_limits(
	source: list[Token],
	output: list[Token],
	mapping: dict[int, int],
	following: list[int],
	modifier_heads: dict[int, int],
	wrappers: dict[int, int]
) -> list[tuple[int, int]]:
	"""トークンの出現数と原文順から，全アンカーの許される位置の範囲を共有する
	計算量：トークン数 N に対して O(N log(N+2))
	引数：
		source: 原文のコードのトークン
		output: 出力のコードのトークン
		mapping: 原文順を保つ位置の対応
		following: 各位置から続く対応済の非区切トークン
		modifier_heads: 後置化した条件の位置と原文の構文の始端
		wrappers: 返戻で包んだ原文の始端と出力の先行境界の格納先
	戻値：
		各原文位置の追越・遡行・補完に依る下限と上限（両端を含む）
	"""
	# 補完・除去の無い列は，行を跨ぐ直前の非区切トークンだけで範囲が決まる
	if len(source) == len(output) and all(mapping.get(idx) == idx for idx in range(len(source))):
		previous: list[int] = []
		last: int = -1
		line_start: int = 0
		line: int = -1
		prior_line: int = -1
		last_line_start: int = 0
		for idx, token in enumerate(output):
			if token.line != line:
				line_start = idx
				prior_line = last
				line = token.line
			previous.append(prior_line + 1)
			if token.text not in SEPARATORS:
				last = idx
				last_line_start = line_start
		# 同じコード列の許可範囲の収集
		limits: list[tuple[int, int]] = []
		for next_idx in following:
			limits.append((previous[next_idx], next_idx) if next_idx < len(output) else (last_line_start, len(output)))
		# 許可範囲の返戻
		return limits
	positions: dict[str, list[int]] = {}
	mapped: dict[str, list[tuple[int, int]]] = {}
	line_starts: dict[int, int] = {}
	line_ends: dict[int, int] = {}
	unmatched: list[int] = []
	inverse: dict[int, int] = { target: start for start, target in mapping.items() }
	parentheses: list[tuple[int, int]] = []
	for idx, token in enumerate(output):
		positions.setdefault(token.text, []).append(idx)
		line_starts.setdefault(token.line, idx)
		line_ends[token.line] = idx + 1
		if idx not in inverse and token.text not in SEPARATORS:
			unmatched.append(idx)
		# 対応する値を包む返戻の先行境界と，元から括弧の内側に在る境界を区別する
		if token.text == "(":
			parentheses.append((idx, len(source)))
		if parentheses:
			opening: int
			first: int
			opening, first = parentheses[-1]
			parentheses[-1] = opening, min(first, inverse.get(idx, len(source)), modifier_heads.get(idx, len(source)))
		if token.text == ")" and parentheses:
			opening, first = parentheses.pop()
			if parentheses:
				parent: int
				parent_first: int
				parent, parent_first = parentheses[-1]
				parentheses[-1] = parent, min(parent_first, first)
			if first < len(source) and opening and output[opening - 1].text == "return" and opening - 1 not in inverse:
				wrappers[first] = line_starts[output[opening - 1].line]
	for start in sorted(mapping):
		if source[start].text not in SEPARATORS:
			mapped.setdefault(source[start].text, []).append((start, mapping[start]))
	totals: Counter[str] = Counter(token.text for token in source)
	prefix: Counter[str] = Counter()
	cursors: dict[str, int] = { text: 0 for text in mapped }
	visible: dict[str, int] = {}
	versions: Counter[str] = Counter()
	upper: list[tuple[int, str, int]] = []
	lower: list[tuple[int, str, int]] = []
	ending: list[tuple[int, str, int]] = []
	# 名前毎に変わる候補だけを更新し，失効した候補は先頭に来た時に除く
	def refresh(text: str) -> None:
		"""同じテキストの計数と，前後の対応が決める位置を更新する
		引数：
			text: 更新するトークンの綴り
		"""
		if text not in mapped:
			# 対応の無い名前を除く返戻
			return
		versions[text] += 1
		version: int = versions[text]
		occurrences: list[int] = positions[text]
		links: list[tuple[int, int]] = mapped[text]
		count: int = prefix[text]
		cursor: int = cursors[text]
		bound: int = (
			max(links[cursor][1], occurrences[count]) + 1 if cursor < len(links) and count < len(occurrences) else len(output) + 1
		)
		heapq.heappush(upper, (bound, text, version))
		suffix_idx: int = len(occurrences) - totals[text] + count - 1
		excess: int = occurrences[suffix_idx] if suffix_idx > -1 else -1
		heapq.heappush(lower, (-min(visible.get(text, -1), excess), text, version))
		heapq.heappush(ending, (-min(line_starts[output[links[-1][1]].line] - 1, excess), text, version))
		# 終了
		return
	for text in mapped:
		refresh(text)
	ordered: list[tuple[int, str]] = (
		sorted((target, source[start].text) for start, target in mapping.items() if source[start].text not in SEPARATORS)
	)
	visible_cursor: int = 0
	unmatched_lines: list[int] = [output[idx].line for idx in unmatched]
	limits: list[tuple[int, int]] = []
	for start in range(len(source) + 1):
		next_idx: int = following[start]
		if next_idx in mapping:
			line: int = output[mapping[next_idx]].line
			while visible_cursor < len(ordered) and output[ordered[visible_cursor][0]].line < line:
				target: int
				text: str
				target, text = ordered[visible_cursor]
				visible[text] = target
				refresh(text)
				visible_cursor += 1
			left_heap: list[tuple[int, str, int]] = lower
			unmatched_cursor: int = bisect_left(unmatched_lines, line)
			inserted: int = unmatched[unmatched_cursor - 1] if unmatched_cursor else -1
		else:
			left_heap = ending
			inserted = unmatched[-1] if unmatched else -1
		for queue in upper, left_heap:
			while queue and queue[0][2] != versions[queue[0][1]]:
				heapq.heappop(queue)
		left: int = max(-left_heap[0][0] if left_heap else -1, inserted) + 1
		right: int = upper[0][0] - 1 if upper else len(output)
		if next_idx in mapping and mapping.get(next_idx + 1) == mapping[next_idx] + 1:
			right = min(right, line_ends[output[mapping[next_idx]].line] - 1)
		limits.append((wrappers[start], wrappers[start]) if start in wrappers else (left, right))
		if start < len(source):
			text = source[start].text
			prefix[text] += 1
			if text in cursors and start in mapping:
				cursors[text] += 1
			refresh(text)
	# 原文位置毎の許可範囲の返戻
	return limits

class AnchorCandidates:
	"""位置が単調に進む出力へ，許される範囲の原文の出現だけを渡す"""

	def add(self: AnchorCandidates, key: str, anchor: tuple[int, bool], first: int, last: int) -> None:
		"""両端を含む許可範囲を登録する
		引数：
			self: 候補のインデックス
			key: 本文の照合キー
			anchor: 原文のアンカーと囲内かの組
			first: 許される最初の位置
			last: 許される最後の位置
		"""
		if first < last + 1:
			self.events.append((first, 1, key, anchor))
			self.events.append((last + 1, -1, key, anchor))
		# 終了
		return

	def advance(self: AnchorCandidates, pos: int) -> None:
		"""出力が進んだ範囲の開始・終了だけを反映する
		引数：
			self: 候補のインデックス
			pos: 出力のコード位置
		"""
		delta: int
		key: str
		anchor: tuple[int, bool]
		while self.cursor < len(self.events) and self.events[self.cursor][0] <= pos:
			_, delta, key, anchor = self.events[self.cursor]
			self.active[key, anchor] += delta
			if delta == 1:
				heapq.heappush(self.queues.setdefault(key, []), anchor)
			self.cursor += 1
		# 終了
		return

	def first(self: AnchorCandidates, key: str, remaining: OrderedDict[tuple[int, bool], int]) -> tuple[int, bool]|None:
		"""許可範囲の中の未消費の最初の出現を返す
		引数：
			self: 候補のインデックス
			key: 本文の照合キー
			remaining: 原文順の出現と残数
		戻値：
		対応する最初の出現．無ければ None
		"""
		queue: list[tuple[int, bool]] = self.queues.get(key, [])
		while queue and (not self.active[key, queue[0]] or not remaining.get(queue[0], 0)):
			heapq.heappop(queue)
		# 最初の未消費の出現の返戻
		return queue[0] if queue else None

	def __init__(self: AnchorCandidates) -> None:
		"""範囲の開始・終了と，本文毎の未消費の候補を初期化する
		引数：
			self: 初期化する候補のインデックス
		"""
		self.events: list[tuple[int, int, str, tuple[int, bool]]] = []
		self.active: Counter[tuple[str, tuple[int, bool]]] = Counter()
		self.queues: dict[str, list[tuple[int, bool]]] = {}
		self.cursor: int = 0
		# 終了
		return

def root_document_keys(tokens: list[Token], lang: str) -> Counter[str]:
	"""内側の記法と構文上の所属で，ファイルを説明するドキュメントの本文キーを返す
	マクロの未展開の列も，独立した構文木が確定する所属で扱う
	計算量：原文量 B・構文解析費用 P・ドキュメントの照会費用 D に対し O(B+P+D)
	引数：
		tokens: 原文のトークン
		lang: 構文解析と言語の記法の指定
	戻値：
		ファイルの内側ドキュメントへ同期する本文の出現数
	"""
	found: Counter[str] = Counter()
	if lang not in LANGS or not any(token.is_comment and token.text.startswith(("//!", "/*!")) for token in tokens):
		# 内側ドキュメントの候補を持たない入力の空の結果の返戻
		return found
	parts: list[str] = []
	line: int = 0
	for token in tokens:
		parts.append("\n" * max(0, token.line - line) + token.separator + token.src_text)
		line = token.line + token.src_text.count("\n")
	source: str = "".join(parts)
	roots: list[Node] = parse_dump(dump_ast(source, LANGS[lang][1]), source)
	index: DocIndex = DocIndex(lang, roots)
	for target, items in doc_records(lang, roots, index=index).items():
		if not target.endswith("|inner") or index.owners.get(index.targets[target]) is not None:
			continue
		for _, _, description in items:
			for line in description:
				found.update(sentence_keys(line, lang))
	# 独立した所属で確定した本文の返戻
	return found

def anchor_region_mismatches(before: list[Token], after: list[Token], lang: str) -> list[str]:
	"""原文と出力の独立コメントのアンカーに反する物を列挙する
	指定のコメント開始記号が無ければ空の一覧を返す
	トークンの追加又は除去だけなら部分列を１度で照合し，混在する列は定義と一意の連続トークンから対応を決める
	後続トークンの追越と，アンカーの行（原文で後に続いたトークンの在る行）より前への遡行を検査する
	アンカーの行が行頭へ置けない領域の中なら其の領域の始まる行の前を許し，括弧の中のドキュメントは定義のドキュメントの同期に従う為に除く
	本文キーのインデックスは原文順に持ち，対応済の出現だけを除いて別の出力へ再使用しない
	計算量：本文の総文字数 B・コード対応費用 T・コード数 N・コメント数 C に対して O(B+T+(N+C) log(N+C+2))
	引数：
		before: 入力の解析済の領域又はトークンの並び
		after: 出力の解析済の領域又はトークンの並び
		lang: テストケースの言語
	戻値：
		指定言語の独立コメントのアンカーの違反の一覧
	"""
	if lang not in OPENERS or not any(token.is_standalone for token in before):
		# 記法の無い言語と，原文に独立コメントが無い場合の空の一覧の返戻
		return []
	# 原文と出力のコードトークンの対応付
	code: list[list[Token]] = []
	for tokens in before, after:
		parts: list[Token] = []
		is_tail: bool = False
		for token in tokens:
			if token.is_comment:
				continue
			if lang == "php" and token.text.lower() == "__halt_compiler":
				is_tail = True
			key: str = token.text if is_tail else quoted_key(token.text, lang)
			parts.append(token if key == token.text else token._replace(text=key))
		code.append(parts)
	src_code: list[Token]
	output_code: list[Token]
	src_code, output_code = code
	# キー毎に原文順のアンカーと出現の残数を保持し，消費した先頭を次の走査で辿らない
	# 同じキー・アンカー・括弧内の条件を持つ出現は判定が等しい為，残数だけを合算する
	remaining: dict[str, OrderedDict[tuple[int, bool], int]] = {}
	empty: OrderedDict[tuple[int, bool], int] = OrderedDict()
	standalone_keys: set[str] = set()
	# 原文の独立でないコメント（行末・文中）の照合用の本文（種別の変化を検査する）
	attached_keys: set[str] = set()
	attached_docs: Counter[tuple[int, bool, bool]] = Counter()
	root_keys: Counter[str] = root_document_keys(before, lang)
	root_starts: dict[str, set[tuple[int, bool]]] = {}
	pos: int = 0
	depth: int = 0
	for token, keys in zip(before, token_sentence_keys(before, lang)):
		if not token.is_comment:
			pos += 1
			depth += (token.text in ("(", "[")) - (token.text in (")", "]"))
		elif token.is_standalone:
			# `depth` は不対応の閉じで負に為り得る為，正の値だけを囲みの内部とする
			anchor: tuple[int, bool] = pos, depth > 0
			standalone_keys.update(keys)
			if token.text.startswith(("//!", "/*!")):
				for key in keys:
					if root_keys[key]:
						root_keys[key] -= 1
						root_starts.setdefault(key, set()).add(anchor)
			for key in keys:
				if key not in remaining:
					remaining[key] = OrderedDict()
				occurrences: OrderedDict[tuple[int, bool], int] = remaining[key]
				occurrences[anchor] = occurrences.get(anchor, 0) + 1
		else:
			attached_keys.update(keys)
			if token.text.startswith(DOCUMENTS):
				attached_docs[pos, token.has_preceding_text, pos < len(src_code) and src_code[pos].logical_line == token.logical_end_line] += 1
	# コメントを除くトークンの並びの対応（整形で補い又は除いたトークンは対応しない）
	src_texts: list[str] = [token.text for token in src_code]
	output_texts: list[str] = [token.text for token in output_code]
	omitted: list[set[int]] = [set(), set()]
	modifier_heads: dict[int, int] = {}
	if lang == "ruby":
		left_headers: dict[tuple[str, ...], list[tuple[bool, int, int]]] = modifier_headers(src_code)
		right_headers: dict[tuple[str, ...], list[tuple[bool, int, int]]] = modifier_headers(output_code)
		for key, headers in left_headers.items():
			targets: list[tuple[bool, int, int]] = right_headers.get(key, [])
			if len(headers) != len(targets):
				continue
			for (is_head, start, end), (is_target_head, target_start, target_end) in zip(headers, targets):
				if is_head and not is_target_head:
					omitted[0].update(range(start, end))
					omitted[1].update(range(target_start, target_end))
					modifier_heads[target_start] = start
	indices: list[list[int]] = [[idx for idx in range(len(parts)) if idx not in skipped] for parts, skipped in zip(code, omitted)]
	mapping: dict[int, int]|None = (
		token_mapping([src_texts[idx] for idx in indices[0]], [output_texts[idx] for idx in indices[1]], lang)
	)
	if mapping is None:
		# 独立した所属から対応を確定出来ない混在の返戻
		return ["コードの追加と除去の所属を決めれず，コメントのアンカーを照合不能"]
	mapping = { indices[0][start]: indices[1][end] for start, end in mapping.items() }
	inverse: dict[int, int] = { val: key for key, val in mapping.items() }
	# 後続の同じ綴りを補った場合は，次の対応迄の未対応の出現へ右寄せし，原文の後続と区別する
	output_positions: dict[str, list[int]] = {}
	for idx, text in enumerate(output_texts):
		output_positions.setdefault(text, []).append(idx)
	limit: int = len(output_code)
	for src_idx in sorted(mapping, reverse=True):
		target: int = mapping[src_idx]
		positions: list[int] = output_positions[output_texts[target]]
		candidate: int = bisect_left(positions, limit) - 1
		moved: int = positions[candidate] if candidate > -1 and positions[candidate] > target else target
		if moved != target:
			del inverse[target]
			mapping[src_idx], inverse[moved] = moved, src_idx
		limit = mapping[src_idx]
	# 同期で本文が変わる行内のドキュメントは，両側のコード境界と配置種別で各出現を対応付ける
	doc_boundaries: Counter[tuple[int, int, bool, bool]] = Counter()
	for (start, has_prefix, has_suffix), count in attached_docs.items():
		left: int|None = mapping.get(start - 1) if start else -1
		right: int|None = mapping.get(start) if start < len(src_code) else len(output_code)
		if left is not None and right is not None:
			doc_boundaries[left, right, has_prefix, has_suffix] += count
	# 原文の後続トークンをアンカー毎に探さない
	following_tokens: list[int] = [len(src_code)] * (len(src_code) + 1)
	for idx in range(len(src_code) - 1, -1, -1):
		following_tokens[idx] = idx if idx in mapping and src_code[idx].text not in SEPARATORS else following_tokens[idx + 1]
	blocked: dict[int, int] = blocked_lines(output_code, lang)
	document_heads: tuple[str, ...] = LINE_DOCUMENT_HEADS.get(lang, ())
	wrappers: dict[int, int] = {}
	limits: list[tuple[int, int]] = anchor_limits(src_code, output_code, mapping, following_tokens, modifier_heads, wrappers)
	ordinary: AnchorCandidates = AnchorCandidates()
	documents: AnchorCandidates = AnchorCandidates()
	root_documents: AnchorCandidates = AnchorCandidates()
	inside: dict[str, deque[tuple[int, bool]]] = {}
	ending: dict[str, deque[tuple[int, bool]]] = {}
	line_ranges: dict[int, list[int]] = {}
	for idx, token in enumerate(output_code):
		line_ranges.setdefault(token.line, [idx, idx])[1] = idx
	doc_starts: dict[int, int] = {}
	for key, starts in remaining.items():
		for anchor in starts:
			start: int
			is_inside: bool
			start, is_inside = anchor
			ordinary.add(key, anchor, *limits[start])
			if anchor in root_starts.get(key, ()):
				root_documents.add(key, anchor, 0, 0)
			if start in mapping and start not in wrappers:
				ordinary.add(key, anchor, mapping[start], mapping[start])
			next_idx: int = following_tokens[start]
			if next_idx in mapping:
				line: int = output_code[mapping[next_idx]].line
				if blocked.get(line) in line_ranges:
					ordinary.add(key, anchor, *line_ranges[blocked[line]])
			else:
				ending.setdefault(key, deque()).append(anchor)
			if is_inside:
				inside.setdefault(key, deque()).append(anchor)
			else:
				if start not in doc_starts:
					doc_starts[start] = documentation_start(src_code, start)
				if doc_starts[start] in mapping:
					point: int = mapping[doc_starts[start]]
					documents.add(key, anchor, point, point)
	ordinary.events.sort()
	documents.events.sort()
	root_documents.events.sort()
	found: list[str] = []
	pos = 0
	for token, keys in zip(after, token_sentence_keys(after, lang)):
		if not token.is_comment:
			pos += 1
			continue
		ordinary.advance(pos)
		documents.advance(pos)
		root_documents.advance(pos)
		if not token.is_standalone and token.text.startswith(DOCUMENTS):
			boundary: tuple[int, int, bool, bool] = (
				pos - 1,
				pos,
				token.has_preceding_text,
				pos < len(output_code) and output_code[pos].logical_line == token.logical_end_line
			)
			if doc_boundaries[boundary]:
				doc_boundaries[boundary] -= 1
				continue
		for key in keys:
			is_standalone_src: bool = key in standalone_keys
			if not token.is_standalone:
				if is_standalone_src and key not in attached_keys:
					found.append(f"{token.line + 1} 行目の {token.text[:40]}：原文で独立した物が行末・文中へ変わった")
				continue
			starts: OrderedDict[tuple[int, bool], int] = remaining.get(key, empty)
			candidates: list[tuple[int, bool]] = []
			if (anchor := ordinary.first(key, starts)) is not None:
				candidates.append(anchor)
			is_document: bool = token.text.startswith(DOCUMENTS)
			root_anchor: tuple[int, bool]|None = root_documents.first(key, starts) if token.text.startswith(("//!", "/*!")) else None
			document_anchor: tuple[int, bool]|None = documents.first(key, starts) if is_document else None
			for anchor in root_anchor, document_anchor:
				if anchor is not None:
					candidates.append(anchor)
			for queues, is_allowed in (
				(inside, is_document or pos < len(output_code) and output_code[pos].text in document_heads),
				(ending, pos < len(output_code) and blocked.get(output_code[pos].line) == output_code[pos].line)
			):
				if not is_allowed or key not in queues:
					continue
				queue: deque[tuple[int, bool]] = queues[key]
				while queue and not starts.get(queue[0], 0):
					queue.popleft()
				if queue:
					candidates.append(queue[0])
			if not candidates:
				if starts:
					found.append(f"{token.line + 1} 行目の {token.text[:40]}：原文のアンカーの許可範囲へ置かれて居ない")
				elif is_standalone_src:
					found.append(f"{token.line + 1} 行目の {token.text[:40]}：原文の同じ出現を再使用した")
				continue
			matched: tuple[int, bool] = min(candidates)
			starts[matched] -= 1
			if not starts[matched]:
				del starts[matched]
	# 食違の一覧の返戻
	return found

def anchor_mismatches(before: list[Region], after: list[Region]) -> list[str]:
	"""解析済の領域の独立コメントのアンカーに反する物を列挙する
	計算量：領域数 E と各領域の照合費用 A に対して O(EA)
	引数：
		before: 入力の解析済の領域又はトークンの並び
		after: 出力の解析済の領域又はトークンの並び
	戻値：
		領域毎の独立コメントのアンカーの違反の一覧
	"""
	# 型の取得に作用する括弧を，アンカーの対応付で省略可能な区切にしない
	protected: list[str] = [
		"型の取得・推論に作用する括弧が変わった" for source, output in zip(before, after) if source.semantic_parens != output.semantic_parens
	]
	# HTML 本体のコメントはソース側に保持する為，埋込の領域だけアンカーのトークンを照合する
	return protected + [
		problem for src_region, output_region in zip(before, after) if src_region.lang != "html" for problem in
		anchor_region_mismatches(src_region.tokens, output_region.tokens, output_region.lang)
	]

def exit_code(binary: str, text: bytes, lang: str) -> int:
	"""指定のフォーマッターと言語で各行末へ空白とタブを足したテストケースの終了コードを返す
	時間切は -1，フォーマッターの不具合は -2 を返す
	計算量：入力のバイト数 B とフォーマッターの実行時間 T に対して O(B+T)
	引数：
		binary: フォーマッターの実行ファイル
		text: 照合又は解析するテキスト
		lang: テストケースの言語
	戻値：
		終了値（時間切は -1，不具合に依る拒否は -2）
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
	"""
	# 行末空白を付けた変種での終了状態の確認
	mutated: bytes = b"\n".join(line + SUFFIX if line else line for line in text.split(b"\n"))
	try:
		result: subprocess.CompletedProcess[bytes] = (
			subprocess.run((binary, "--stdin", "--lang", lang, "--no-lint"), input=mutated, capture_output=True, timeout=120)
		)
	except subprocess.TimeoutExpired:
		# 時間の上限を越えた場合は契約外として扱う事の返戻
		return -1
	# フォーマッターの不具合を契約外とした終了コードの返戻
	return -2 if is_defect(result) else result.returncode

def comment_blank_variants(parsed: list[Region], text: str) -> list[bytes]:
	"""コメント内の空の論理行を除いた入力と，重ねた入力を返す
	リテラル・地の文・コメント外の空行と，同じ行のコードは変更しない
	計算量：入力長 N・トークンの総長 T に対して O(N+T)
	引数：
		parsed: 元の入力から得たコメントの範囲
		text: 元の入力
	戻値：
		空の論理行を持つ時は２つの変種，其の他は空の列
	"""
	# 独立コメント内の物理行だけを変更対象とする
	lines: list[str] = text.splitlines(keepends=True)
	blanks: set[int] = set()
	for region in parsed:
		if region.lang == "html":
			continue
		for token in region.tokens:
			if not token.is_comment or not token.is_standalone or token.has_preceding_text:
				continue
			rows: list[str] = token.src_text.splitlines()
			for idx, row in enumerate(rows):
				# 囲みの開始・終端の行を含めず，本文を持たない行だけを選ぶ
				if re.fullmatch(r"[ \t]*(?://[/!]?|#)[ \t\r]*", row) or idx and idx + 1 < len(rows) and re.fullmatch(r"[ \t]*\*?[ \t\r]*", row):
					blanks.add(token.line + idx)
	# 既存の空の論理行の削除・重複だけを行い，入力の内容から変種を作る
	# 空の論理行を持たない時の空の列の返戻
	if not blanks:
		# 空の列の返戻
		return []
	# 空の論理行を除いた入力と重ねた入力の返戻
	return [
		"".join(line for idx, line in enumerate(lines) if idx not in blanks).encode(),
		"".join(line * (2 if idx in blanks else 1) for idx, line in enumerate(lines)).encode()
	]

def inspect(binary: str, src: Path, lang: str) -> tuple[int, list[str]]:
	"""指定のフォーマッターと言語でテストケースの変種を調べ，比べた幅の数と違反を返す
	計算量：フォーマッターの実行時間の和 T と各検査の実行時間の和 V に対して O(T+V)
	引数：
		binary: フォーマッターの実行ファイル
		src: 入力のテキスト
		lang: テストケースの言語
	戻値：
		比較した幅の数と違反の一覧
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		subprocess.TimeoutExpired: 呼び出した子プロセスが制限時間内に完了しない場合
		RuntimeError: リテラルの解析又は原文との対応付が失敗する場合
		ValueError: パーサーの出力が不正な場合
		json.JSONDecodeError: パーサーのノード値が有効な JSON でない場合
	"""
	# 原文と期待出力の注記配置の照合
	text: bytes = src.read_bytes()
	original: str = text.decode(errors="replace")
	before: list[Region] = regions(original, lang)
	checked: int = 0
	expected: str = src.with_suffix(".out").read_bytes().decode(errors="replace")
	expected_regions: list[Region] = regions(expected, lang)
	problems: list[str] = [
		f"期待する出力の{problem}" for problem in depth_mismatches(expected_regions, expected) + separator_mismatches(expected_regions) +
		separator_record_mismatches(before, expected_regions) + anchor_mismatches(before, expected_regions)
	]
	# 既存のドキュメントの入力を使い，空の論理行の有無への非依存も照合する
	variants: list[bytes] = (
		comment_blank_variants(before, original) if
		src.stem.startswith("documentation") or src.stem == "comment_code_paragraphs" else
		[]
	)
	# 各行幅での冪等性と注記配置の照合
	for width in (WIDE, *WIDTHS):
		try:
			first: bytes = format_src(binary, text, lang, width)
			checked += 1
			for variant in variants:
				if format_src(binary, variant, lang, width) != first:
					problems.append(f"幅 {width} でコメント内の空の論理行の有無に依って出力が異なる")
			if format_src(binary, first, lang, width) != first:
				problems.append(f"幅 {width} で二巡目が一巡目と異なる")
		except RuntimeError as e:
			problems.append(f"幅 {width} で整形を拒否：{e}")
			continue
		output: str = first.decode(errors="replace")
		output_regions: list[Region] = regions(output, lang)
		problems += [
			f"幅 {width} の{problem}" for problem in depth_mismatches(output_regions, output) + separator_mismatches(output_regions) +
			separator_record_mismatches(before, output_regions) + anchor_mismatches(before, output_regions)
		]
	code: int
	if (code := exit_code(binary, text, lang)) not in ALLOWED:
		problems.append(f"各行の末尾へ空白とタブを足すと終了コード {code}（-1 は時間切れ，-2 はフォーマッターの不具合）")
	# 比べた幅の数と違反の返戻
	return checked, problems

def self_check() -> list[str]:
	"""手書の正誤対でコメント検査の誤検出と見逃を返す
	フォーマッターの出力でなく，条文から定めた分類・深さ・区切と照合する
	計算量：正誤対の数 F・１対の最大検査費用 P に対して O(FP)
	戻値：
		分類・深さ・区切が期待と食い違う正誤対の一覧
	送出例外：
		OSError: 仕様の読込又はパーサーの起動が失敗する場合
		UnicodeDecodeError: 仕様を指定の文字コードで読めない場合
		StopIteration: 通知の契約の行が仕様に無い場合
		RuntimeError: リテラルの解析又は原文との対応付が失敗する場合
		ValueError: パーサーの出力が不正な場合
		json.JSONDecodeError: パーサーのノード値が有効な JSON でない場合
		subprocess.TimeoutExpired: パーサーが制限時間内に完了しない場合
	"""
	found: list[str] = []
	# 変種の期待も手で定め，コメント外の空行と値を保つ事を確かめる
	for language, source, expected in (
		("cpp", "/** A\n *\n * B\n */\nint x;\n", [b"/** A\n * B\n */\nint x;\n", b"/** A\n *\n *\n * B\n */\nint x;\n"]),
		(
			"go",
			"package p\n\n// A\n//\n// B\ntype T int\n",
			[b"package p\n\n// A\n// B\ntype T int\n", b"package p\n\n// A\n//\n//\n// B\ntype T int\n"]
		),
		("rust", "//! A\n//!\n//! B\nstruct T;\n", [b"//! A\n//! B\nstruct T;\n", b"//! A\n//!\n//!\n//! B\nstruct T;\n"]),
		(
			"ruby",
			"=begin\nA\n\nB\n=end\nclass C end\n",
			[b"=begin\nA\nB\n=end\nclass C end\n", b"=begin\nA\n\n\nB\n=end\nclass C end\n"]
		),
		("python", "value = \"\"\"A\n\nB\"\"\"\n", []),
		("html", "<!-- A\n\nB -->\n<p>C</p>\n", [])
	):
		if comment_blank_variants(regions(source, language), source) != expected:
			found.append(f"空の論理行の変種が手期待と異なる：{language}")
	# 宣言子を式と誤認せず，型の取得と未確定のマクロの式の括弧は保つ
	for source, output, is_allowed in (
		("void f(){int (first), (second);}", "void f(){int first;int second;}", True),
		(
			"using T=int;void f(){T *p,(value),(array)[2],(*fp)(int);}",
			"using T=int;void f(){T *p;T value;T array[2];T (*fp)(int);}",
			True
		),
		("int value;decltype((value)) ref=value;", "int value;decltype(value) ref=value;", False),
		("int value;decltype(auto) f(){return (value);}", "int value;decltype(auto) f(){return value;}", False),
		("void f(){RAW_CALL((1+2));}", "void f(){RAW_CALL(1+2);}", True),
		("void f(){g((a<b),(a<b),(c>d));}", "void f(){g((a<b),(a<b),c>d);}", True),
		("void f(){G((a<b),(a<b),(c>d));}", "void f(){G((a<b),(a<b),c>d);}", True),
		("void f(){G((a<b),(a<b),(c>d));}", "void f(){G(a<b,a<b,c>d);}", False),
		("void f(){g((a<b),h(c>d));}", "void f(){g(a<b,h(c>d));}", True),
		("void f(){g((1<b),(c>d));}", "void f(){g(1<b,c>d);}", True),
		("void f(){g((h()<b),(c>d));}", "void f(){g(h()<b,c>d);}", True),
		("#define lower(x) x\nvoid f(){lower((1+2));}", "#define lower(x) x\nvoid f(){lower(1+2);}", False),
		(
			"#define lower(x) x\n#define UPPER lower\nvoid f(){UPPER((1+2));}",
			"#define lower(x) x\n#define UPPER lower\nvoid f(){UPPER(1+2);}",
			False
		),
		(
			"#define lower(x) x\n#define UPPER lower\n#undef lower\nvoid f(){UPPER((1+2));}",
			"#define lower(x) x\n#define UPPER lower\n#undef lower\nvoid f(){UPPER(1+2);}",
			True
		),
		("#define CALL Handler\nvoid f(){CALL((1+2));}", "#define CALL Handler\nvoid f(){CALL(1+2);}", True),
		("#include \"unread.h\"\nvoid f(){lower((1));}", "#include \"unread.h\"\nvoid f(){lower(1);}", False),
		(
			"#include \"unread.h\"\n#undef lower\nvoid f(){lower((1));}",
			"#include \"unread.h\"\n#undef lower\nvoid f(){lower(1);}",
			True
		),
		("#define lower(x) x\nvoid f(){(lower)((1+2));}", "#define lower(x) x\nvoid f(){(lower)(1+2);}", True),
		(
			"#include \"unread.h\"\n#undef CALL\nvoid f(){Api::CALL((1+2));}",
			"#include \"unread.h\"\n#undef CALL\nvoid f(){Api::CALL(1+2);}",
			True
		),
		("#define VALUE a+b\nint f(){return 2*(VALUE);}", "#define VALUE a+b\nint f(){return 2*VALUE;}", False),
		("using T=int;void f(int T){T*(value);}", "using T=int;void f(int T){T*value;}", False)
	):
		if (cpp_semantic_parens(source) == cpp_semantic_parens(output)) != is_allowed:
			found.append("宣言子と意味を持つ式の括弧を混同した：" + source)
	# ライフタイム・ラベルの単引用符で，後続のコメントと前置区切の記録を隠さない
	source: str = "// café's result. Next.\nint First;\nint Second;\n"
	correct: str = "// café's result\n// Next\nint First;\nint Second;\n"
	wrong: str = "// café's result\nint First;\n// Next\nint Second;\n"
	before: list[Region] = regions(source, "cpp")
	if anchor_mismatches(before, regions(correct, "cpp")) or separator_record_mismatches(before, regions(correct, "cpp")):
		found.append("英数字のアポストロフィから分けた文の正しいアンカーを拒んだ")
	if not anchor_mismatches(before, regions(wrong, "cpp")):
		found.append("英数字のアポストロフィから分けた文の別のアンカーへの移動を見逃した")
	for source in (
		"fn f<'a>() { // it's kept\n}\n",
		"fn f<'a: 'b, 'b>() { // it's kept\n}\n",
		"fn f(x: &'static i32) { // it's kept\n}\n",
		"fn f(x: &'_ i32) { // it's kept\n}\n",
		"fn f() { 'outer: loop { // it's kept\nbreak 'outer; } }\n"
	):
		before: list[Region] = regions(source.replace(" //", "   //"), "rust")
		after: list[Region] = regions(source, "rust")
		wrong: list[Region] = regions(source.replace(" //", "\t\t //"), "rust")
		if (
			[token.text for region in before for token in region.tokens if token.is_comment] != ["// it's kept"] or
			separator_mismatches(after) or separator_record_mismatches(before, after) or len(separator_mismatches(wrong)) != 1 or
			len(separator_record_mismatches(before, wrong)) != 1
		):
			found.append(f"Rust: ライフタイム・ラベル後のコメント又は前置区切を読違えた：{source!r}")
	# マクロ内も後続のトークンの行へ置き，外へ移したコメントを許さない
	source: str = "m!(\n\t// value\n\tx\n);\n"
	output: str = "// value\nm!(\n\tx\n);\n"
	if blocked_lines(lex(source, "rust"), "rust") or not anchor_mismatches(regions(source, "rust"), regions(output, "rust")):
		found.append("Rust のマクロ内のアンカーを逐語領域として見逃した")
	# 内側ドキュメントは確定した説明対象にだけ同期する
	for source, output, should_accept in (
		("macro_rules! m { () => {\n//! crate\nx\n}; }\n", "/*! crate */\nmacro_rules! m { () => { x }; }\n", True),
		("mod m {\n//! module\nfn f() {}\n}\n", "/*! module */\nmod m {\nfn f() {}\n}\n", False)
	):
		if bool(anchor_mismatches(regions(source, "rust"), regions(output, "rust"))) == should_accept:
			found.append("内側ドキュメントの説明対象をアンカーに反映出来なかった")
	# 同じ行の複数のライフタイム間も，コメントだけを記録する
	source: str = "struct S<'a /* keep */, 'b> { x: &'a i32, y: &'b i32 }\n"
	if [token.text for token in lex(source, "rust") if token.is_comment] != ["/* keep */"]:
		found.append("Rust: ライフタイム仮引数間のブロックコメントを読違えた")
	# 文字・バイト文字と其のエスケープは，ライフタイムの扱いでも分割しない
	for literal in "'/'", "'\\\"'", "'\\''", "'\\\\'", "'\\n'", "'\\u{1_f600}'", "'\\x2f'", "'a'", "'語'":
		for prefix in ("", "b") if literal != "'語'" and "u{" not in literal else ("",):
			source = "fn f() { let x = " + prefix + literal + "; // it's kept\n}\n"
			tokens: list[Token] = lex(source, "rust")
			if literal not in [token.text for token in tokens] or [token.text for token in tokens if token.is_comment] != ["// it's kept"]:
				found.append(f"Rust: 文字リテラルと後続のコメントを読違えた：{literal!r}")
	literal: str = "'\\x80'"
	if match_rust_char_literal(literal, 0) is not None or match_rust_char_literal("b" + literal, 1) is None:
		found.append(f"Rust: 文字とバイト文字の１６進エスケープの範囲を混同した：{literal!r}")
	for literal in "'語'", "'\\u{2f}'":
		if match_rust_char_literal("b" + literal, 1) is not None:
			found.append(f"Rust: バイト文字へ Unicode の字又はエスケープを取り込んだ：{literal!r}")
	# 条文の文区切と句点除去の期待を，初回の変換及び再読取で照合する
	for lang in "css", "scss":
		for text in r".x{background:url(a//b)}", r".x{background:u\72l(a//b)}", r"$m:(x:url(a//b));", r"$m:(x:u\72l(a//b));":
			if any(token.is_comment for region in regions(text, lang) for token in region.tokens):
				found.append("引用符の無い URL の本文をコメントと誤認した")
	# SCSS の補間を持つ URL も，斜線と補間の内部の括弧を本文として読む
	for text in r".x{src:URL(//cdn/#{$img}/x.woff2);}", r".x{src:url(//cdn/#{f(1)}/x.woff2);}":
		if any(token.is_comment for region in regions(text, "scss") for token in region.tokens):
			found.append("補間を持つ URL の本文をコメントと誤認した")
	for body, expected in LOGICAL_LINE_CASES:
		if tuple(logical_lines(body)) != expected or tuple(logical_lines("\n".join(expected))) != expected:
			found.append(f"文区切・句点除去が条文と違う：{body!r}")
	# 記法・配置に依らず同じ論理行を識別し，ブロックコメントの終端を囲みの閉じと混同しない
	for body, expected in (
		("// Read. Return.", ("Read", "Return")),
		("// `f(a,b)`", ("`f(a,b)`",)),
		("// `f(ab)`", ("`f(ab)`",)),
		("// `a b`", ("`a b`",)),
		("// `ab`", ("`ab`",)),
		("/**\n * ```\n * f(a, b)\n * ```\n */", ("```", "f(a, b)", "```")),
		("// (処理.. 続行.)", ("(処理続行)",)),
		("/** {@link A 処理.. 続行.} */", ("{@linkA処理続行}",)),
		("// Read.. 続行", ("Read", "続行")),
		("//!doxygenStays", ("doxygenStays",)),
		("///doxygenStays", ("doxygenStays",)),
		("/*!doxygenStays*/", ("doxygenStays",)),
		("// !doxygenStays", ("!doxygenStays",)),
		("/*! !doxygenStays */", ("!doxygenStays",)),
		("/* Read. Return. */", ("Read", "Return")),
		("/** Read. Return. */", ("Read", "Return")),
		("<!-- Read. Return. -->", ("Read", "Return")),
		("/* Read.Next */", ("Read.Next",)),
		("/**\n * ```\n * x.y!\n *\n * ```\n */", ("```", "x.y!", "```")),
		("/** <summary data=\"/>\">First</summary> */", ("First",)),
		("/** <summary data='/>'>Second</summary> */", ("Second",)),
		("/**\n * <summary data=\"a\n * >b\">First</summary>\n */", ("First",)),
		("/**\n * <summary data='a\n * >b'>Second</summary>\n */", ("Second",)),
		("/** <summary data=\"`x` <y>\">First</summary> */", ("First",)),
		("/** `<summary data=\"/>\">` */", ("`<summary data=\"/>\">`",)),
		("/** <summary data=\"/ >\" /> */", ())
	):
		if tuple(sentence_keys(body)) != expected:
			found.append(f"記法の異なる本文の論理行が条文と違う：{body!r}")
	# 通常の値と補間・専用記法を区別する
	for lang, left, right, is_equal in (
		("ruby", "'value'", "\"value\"", True),
		("ruby", "'#{x}'", "\"#{x}\"", False),
		("ruby", "'#@x'", "\"#@x\"", False),
		("ruby", "'#$x'", "\"#$x\"", False),
		("php", "'$x'", "\"$x\"", False),
		("javascript", "'a'", "\"b\"", False),
		("cpp", "'a'", "\"a\"", False),
		("rust", "r#\"a\"#", "\"a\"", False)
	):
		if (quoted_key(left, lang) == quoted_key(right, lang)) != is_equal:
			found.append(f"{lang}: 値又は補間の異なる引用符を同じキーにした")
	# 所属の無い反復の混在は，最長一致の推測をせず照合不能とする
	for count in 1, 16, 1024:
		for prefix, suffix in ([], []), (["begin"], ["end"]):
			if token_mapping(prefix + ["a", "b"] * count + suffix, prefix + ["b", "a"] * count + suffix) is not None:
				found.append("所属の無い反復の混在を推測で対応付けた")
	for count in 16, 1024:
		left: list[str] = ["function", "f", "(", ")", "{"] + ["a", "(", ")", ";", "b", "(", ")", ";"] * count + ["}"]
		right: list[str] = ["function", "f", "(", ")", "{"] + ["b", "(", ")", ";", "a", "(", ")", ";"] * count + ["return", ";", "}"]
		if token_mapping(left, right, "javascript") is not None:
			found.append("同じ所属内の反復の並替を返戻の補完と誤認した")
	# 右側の同じトークンを補った時も，元の後続と補完した物を区別する
	for before, after, expected in (
		(["x", ";"], ["x", ";", "x", ";"], { 0: 2, 1: 3 }),
		(["x", ";"], ["return", "x", ";"], { 0: 1, 1: 2 }),
		(["x", ";"], ["x"], None)
	):
		if subsequence_mapping(before, after) != expected:
			found.append("補完だけのトークン列の原文順又は右端の対応が違う")
	for output, expected in ("x();\t// same\ny(); // same\n", 0), ("x();\t// same\ny();\t// same\n", 1):
		before = regions("x();\t// same\ny(); // same\n", "c")
		if len(separator_record_mismatches(before, regions(output, "c"))) != expected:
			found.append("同じ本文の前置区切の出現を再使用した")
	for source, output, expected in (
		("// same\nx();\n", "return;\n// same\nx();\n", 0),
		("// same\nx();\n", "// same\nreturn;\nx();\n", 1),
		("return;\n// same\nx();\n", "// same\nx();\n", 0),
		("// same\nx();\n// same\ny();\n", "// same\n// same\nx();\ny();\n", 1)
	):
		if len(anchor_mismatches(regions(source, "c"), regions(output, "c"))) != expected:
			found.append("アンカーの許可範囲又は原文順の消費が違う")
	# 型引数付の要素を閉じた後は別の要素の開始行を使い，前の宣言への移動を許さない
	for markup_type in "T", "Array<(x: T) => {value: U}>":
		prefix: str = f"const first = <A<{markup_type}> p={{1}} />;\n"
		for markup_source, markup_output in (
			("<B p={[1,\n// note\n2]} />", "<B\n\tp={[1, 2]}\n/>"),
			("<B p={f(1,\n// note\n2)} />", "<B\n\tp={f(1, 2)}\n/>"),
			("<B p={{a: 1,\n// note\nb: 2}} />", "<B\n\tp={{a: 1, b: 2}}\n/>"),
			("<B p={<C p={1\n// note\n} />} />", "<B\n\tp={<C p={1} />}\n/>"),
			("<><B p={1\n// note\n} /></>", "<>\n\t<B p={1} />\n</>")
		):
			source: str = prefix + "const second = " + markup_source + ";\n"
			output: str = prefix + "const second =\n// note\n" + markup_output + ";\n"
			wrong: str = "// note\n" + prefix + "const second =\n" + markup_output + ";\n"
			if anchor_mismatches(regions(source, "typescriptreact"), regions(output, "typescriptreact")):
				found.append("型引数付の JSX の後の要素の前への移動を拒んだ")
			if not anchor_mismatches(regions(source, "typescriptreact"), regions(wrong, "typescriptreact")):
				found.append("型引数付の JSX の前の宣言への移動を見逃した")
		# 属性の式の内部に安全なコード行が在れば，其の行をアンカーとし，要素の前へ遡らない
		for inner_source, inner_output in (
			("[1,\n// note\n2]", "[\n\t\t1,\n\t\t// note\n\t\t2\n\t]"),
			("f(1,\n// note\n2)", "f(\n\t\t1,\n\t\t// note\n\t\t2\n\t)"),
			("{a:1,\n// note\nb:2}", "{\n\t\ta:1,\n\t\t// note\n\t\tb:2\n\t}")
		):
			source = prefix + "const second = <B p={" + inner_source + "} />;\n"
			output = prefix + "const second =\n<B\n\tp={\n\t" + inner_output + "\n\t}\n/>;\n"
			wrong = output.replace("const second =\n", "const second =\n// note\n").replace("\t\t// note\n", "")
			if anchor_mismatches(regions(source, "typescriptreact"), regions(output, "typescriptreact")):
				found.append("属性の式の内部の安全なコード行のアンカーを拒んだ")
			if not anchor_mismatches(regions(source, "typescriptreact"), regions(wrong, "typescriptreact")):
				found.append("属性の式の内部の安全な行から要素の前へ移したコメントを見逃した")
		# 要素を閉じた後の通常コードは差込禁止領域でなく，後続のコード行へ置く
		for markup_lang in MARKUP_LANGS:
			code_prefix: str = prefix if markup_lang == "typescriptreact" else "const first = <A p={1} />;\n"
			source = code_prefix + "g();\n// note\nh();\n"
			if anchor_mismatches(regions(source, markup_lang), regions(source, markup_lang)):
				found.append("型引数付の JSX の後の通常コードのアンカーを拒んだ")
			if not anchor_mismatches(regions(source, markup_lang), regions("// note\n" + code_prefix + "g();\nh();\n", markup_lang)):
				found.append("閉じた JSX の前へ移した通常コードのコメントを見逃した")
	# 同期した本文を他の独立コメントの出現と混同せず，行内の種別と両側の境界を保つ
	prefix: str = "/** @return 値 */\nint other(){return 0;}\n"
	for label, source, output, is_allowed in (
		("行頭の文中ドキュメント", "/** @returns 値 */ int f(){return 1;}", "/** @return 値 */ int f(){return 1;}", True),
		("見出内のドキュメント", "int /** @returns 値 */ f(){return 1;}", "int /** @return 値 */ f(){return 1;}", True),
		("行末のドキュメント", "int f() /** @returns 値 */\n{return 1;}", "int f() /** @return 値 */\n{return 1;}", True),
		("複数行の文中ドキュメント", "/**\n * @returns 値\n */ int f(){return 1;}", "/**\n * @return 値\n */ int f(){return 1;}", True),
		(
			"同じ境界の複数の出現",
			"int /** @returns 値 */ /** @returns 値 */ f(){return 1;}",
			"int /** @return 値 */ /** @return 値 */ f(){return 1;}",
			True
		),
		("独立から文中への移動", "/** @return 値 */\nint f(){return 1;}", "/** @return 値 */ int f(){return 1;}", False),
		("独立から行末への移動", "/** @return 値 */\nint f(){return 1;}", "int f(){return 1;} /** @return 値 */", False),
		("別の境界への移動", "/** @returns 値 */ int f(){return 1;}", "int /** @return 値 */ f(){return 1;}", False),
		("同じ境界で行頭を変更", "int\n/** @returns 値 */ f(){return 1;}", "int /** @return 値 */ f(){return 1;}", False),
		("同じ境界で後続コード行を変更", "int /** @returns 値 */ f(){return 1;}", "int /** @return 値 */\nf(){return 1;}", False),
		("通常コメントを同期扱いにしない", "/* @returns 値 */ int f(){return 1;}", "/** @return 値 */ int f(){return 1;}", False),
		("同じ境界の出現を再使用", "int /** @returns 値 */ f(){return 1;}", "int /** @return 値 */ /** @return 値 */ f(){return 1;}", False),
		(
			"別の宣言への移動",
			"/** @returns 値 */ int f(){return 1;}\nint g(){return 2;}",
			"int f(){return 1;}\n/** @return 値 */ int g(){return 2;}",
			False
		)
	):
		problems: list[str] = anchor_mismatches(regions(prefix + source, "cpp"), regions(prefix + output, "cpp"))
		if bool(problems) == is_allowed:
			found.append("同期した文中ドキュメントの配置：" + label)
	# 記法の正規化で異なる項目を混同せず，重複した説明は消費済として検出する
	for lang, source, output in (
		(
			"javascript",
			"/** Summary\n * @arg {{x: number}} o Options\n */\nfunction f(o){}\n",
			"/** Summary\n * @param {{x: number}} o Options\n */\nfunction f(o){}\n"
		),
		(
			"javascript",
			"/** Summary\n * @returns {{x: number}}\n */\nfunction f(){}\n",
			"/** Summary\n * @return {{x: number}}\n */\nfunction f(){}\n"
		),
		(
			"swift",
			"/** Summary\n * - Parameters:\n *   - 数: Value\n */\nfunc f(数: Int){}\n",
			"/** Summary\n * - Parameter 数: Value\n */\nfunc f(数: Int){}\n"
		)
	):
		if anchor_mismatches(regions(source, lang), regions(output, lang)):
			found.append("ドキュメントの同義タグ又は項目記法を拒否した")
		duplicated: str = output.replace("Summary", "Summary\n * Summary", 1)
		if not anchor_mismatches(regions(source, lang), regions(duplicated, lang)):
			found.append("正規化したドキュメントの重複を見逃した")
	# 別の定義の見出と同じ綴りの補完骨格は，其の定義の説明の出現を消費しない
	before: str = "/// Left\nfn f()->i32{1}\n/// Right\n/// # Returns\n/// Value\nfn g()->i32{2}\n"
	after: str = (
		"/** Left\n * # Returns\n */\nfn f()->i32{return 1;}\n/** Right\n * # Returns\n * Value\n */\nfn g()->i32{return 2;}\n"
	)
	if anchor_mismatches(regions(before, "rust"), regions(after, "rust")):
		found.append("補完した Markdown の見出を別の定義の説明の出現とした")
	wrong: str = after.replace("Left", "Value", 1).replace(" * Value", " * Left", 1)
	if not anchor_mismatches(regions(before, "rust"), regions(wrong, "rust")):
		found.append("Markdown の骨格と共に別の定義へ移した説明を見逃した")
	# 同じ説明の別のブロックに同義の綴りが混在しても，原文の各出現を１度だけ使う
	for lang, first, other, definitions in (
		("javascript", "@param {{x: number}} o Options", "@arg {{x: number}} o Options", ("function f(o){}", "function g(o){}")),
		("javascript", "@return {{x: number}}", "@returns {{x: number}}", ("function f(){}", "function g(){}")),
		("swift", "- Parameter 数: Value", "- Parameters:\n *   - 数: Value", ("func f(数: Int){}", "func g(数: Int){}"))
	):
		head: str = "/** Summary\n * "
		tail: str = "\n */\n"
		source: str = head + first + tail + definitions[0] + "\n" + head + other + tail + definitions[1] + "\n"
		output: str = head + first + tail + definitions[0] + "\n" + head + first + tail + definitions[1] + "\n"
		if anchor_mismatches(regions(source, lang), regions(output, lang)):
			found.append("同義の項目の別の原文の出現を再使用とした")
		if not anchor_mismatches(regions(source, lang), regions(output + head + first + tail, lang)):
			found.append("別のブロックとして重ねたドキュメントを見逃した")
	for lang, body, other in (
		("cpp", "/** @arg o Options */", "/** @param o Options */"),
		("javascript", "/** `@arg o Options` */", "/** `@param o Options` */")
	):
		if sentence_keys(body, lang) == sentence_keys(other, lang):
			found.append("別の形式又はコード例を同義タグとして統一した")
	# 同じ本文でもドキュメントの原文の出現を１回ずつ消費し，行末の収集でも記法を剥がす
	for lang in "c", "cpp", "rust":
		src: str = "///doxygenStays\n//!doxygenStays\n"
		output: str = "/** doxygenStays */\n/** doxygenStays */\n"
		before: list[Region] = regions(src, lang)
		after: list[Region] = regions(output, lang)
		if anchor_mismatches(before, after) or len(anchor_mismatches(before, regions(output + "/** doxygenStays */\n", lang))) != 1:
			found.append(f"{lang}: ドキュメントの記法と出現の消費が条文と違う")
		if (
			[sentence_keys(body) for body, _, _ in trailing_separators(regions("x();\t/*!//!doxygenStays*/\ny();\n", lang), True)] !=
			[["!doxygenStays"]]
		):
			found.append(f"{lang}: 行末の囲みの本文を二度剥がした")
		for notation in "//!", "/*!":
			src = "x();\t" + notation + "doxygenStays" + (" */" if notation == "/*!" else "") + "\ny();\n"
			before = regions(src, lang)
			after = regions("x();\t/** doxygenStays */\ny();\n", lang)
			wrong: list[Region] = regions("x();\t\t/** doxygenStays */\ny();\n", lang)
			if separator_record_mismatches(before, after) or len(separator_record_mismatches(before, wrong)) != 1:
				found.append(f"{lang} {notation}: 行末のドキュメントの記法と前置区切の対応が条文と違う")
	# 照合キーでも囲み・非句点・保護したコード・保持する文末記号の差を消さない
	for before, after in (
		("// config.json", "// configjson"),
		("// `x.y!`", "// `xy`"),
		("// `f(a,b)`", "// `f(ab)`"),
		("// `a b`", "// `ab`"),
		("// Read!", "// Read"),
		("// (Read Return)", "// (Read) // (Return)")
	):
		if sentence_keys(before) == sentence_keys(after):
			found.append(f"保持対象の異なる本文を同じキーにした：{before!r}")
	# コメントの実体と，後続行・前置区切の独立した照合
	for label, lang, text, expected_comments, expected_depth, expected_separator in SELF_CHECK_CASES:
		parsed: list[Region] = regions(text, lang)
		comments: tuple[str, ...] = tuple(token.text for region in parsed for token in region.tokens if token.is_comment)
		if (
			comments != expected_comments or len(depth_mismatches(parsed, text)) != expected_depth or
			len(separator_mismatches(parsed)) != expected_separator
		):
			found.append(f"{lang} {label}: コメントの分類・深さ・区切が条文と違う")
	# 同行化と差込禁止領域からの移動で，アンカー及び原文の記録を失わない事の照合
	for label, lang, src, output, anchor_errors, record_errors in TRANSFER_CHECK_CASES:
		before: list[Region] = regions(src, lang)
		after: list[Region] = regions(output, lang)
		if len(anchor_mismatches(before, after)) != anchor_errors or len(separator_record_mismatches(before, after)) != record_errors:
			found.append(f"{lang} {label}: コメントのアンカー・前置区切の記録が条文と違う")
	# 通知の契約に在る全理由を，検査側の一覧の欠落に依存せず照合する
	spec: str = (ROOT / "docs/formatter_spec.md").read_text(encoding="utf-8")
	contract: str = next(line for line in spec.splitlines() if line.startswith("| `Error: formatting would <reason>,"))
	reasons: set[str] = set(re.findall(r"`([^`]+)`", contract)[1:])
	if reasons != set(DEFECTS):
		found.append("検証失敗の理由の一覧が通知の契約と違う")
	# 規定した検証失敗の通知と，許可する終了状態の独立した照合
	for reason in sorted(reasons):
		message: bytes = f"Error: formatting would {reason}, left unchanged: <stdin>\n".encode()
		if not is_defect(subprocess.CompletedProcess([], 1, b"x();\n", message)):
			found.append(f"検証失敗の理由を許可した：{reason}")
	for status, message in (
		(0, b""),
		(1, b"Error: --lang unsupported language ID 'unknown'\n"),
		(2, b""),
		(0, b"[warn] sample:1:1: Embedded code was left as is: formatting would break syntax\n")
	):
		if is_defect(subprocess.CompletedProcess([], status, b"x();\n", message)):
			found.append(f"検証失敗でない終了状態を拒んだ：{status}")
	# 自己検査の食違の返戻
	return found

def main() -> int:
	"""テストケースの変種を整形して突合し，違反が有れば非零で終える
	計算量：テストケース数 F・１テストケース当たりの整形と検査の最大時間 T に対して O(FT+F log F)
	戻値：
		成功なら０，違反又は失敗が有れば非零の終了値
	送出例外：
		OSError: 入力・仕様の読取又はパーサー・検査工程の起動が失敗する場合
		UnicodeDecodeError: 仕様を指定の文字コードで読めない場合
		StopIteration: 通知の契約の行が仕様に無い場合
		RuntimeError: リテラルの解析又は原文との対応付が失敗する場合
		ValueError: パーサーの出力又は検査の設定値が不正な場合
		json.JSONDecodeError: パーサーのノード値が有効な JSON でない場合
		subprocess.TimeoutExpired: パーサーが制限時間内に完了しない場合
	"""
	# 基準テストケースとフォーマッターの準備
	binary: str = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "build/shavefmt")
	# 整形を拒む事を期待するテストケースは変種でも拒否が期待と為る為に除く
	sources: list[Path] = [
		src for dir in sorted(path for path in GOLDEN.iterdir() if path.is_dir()) for src in sorted(dir.glob("*.in")) if not src
		.with_suffix(".fail").exists() and not src.with_suffix(".skip").exists()
	]
	checked: int = 0
	failures: list[str] = self_check()
	# テストケース毎の整形と検査は互いに独立な為，同時に走らせる工程の数 (`SHAVEFMT_JOBS`) で分ける（結果はテストケースの順に集める）
	with concurrent.futures.ProcessPoolExecutor(max_workers=JOBS) as pool:
		for (
			src,
			(widths, problems)
		) in zip(sources, pool.map(inspect, itertools.repeat(binary), sources, [src.parent.name for src in sources], chunksize=8)):
			checked += widths
			failures += [f"{src.relative_to(ROOT)}: {problem}" for problem in problems]
	# テストケースの違反の報告
	for failure in failures:
		print(f"FAIL case variants: {failure}", file=sys.stderr)
	print(f"case variants: {checked} cases, {len(failures)} failed")
	# 違反が有れば非零の返戻
	return 1 if failures else 0

if __name__ == "__main__":
	sys.exit(main())
