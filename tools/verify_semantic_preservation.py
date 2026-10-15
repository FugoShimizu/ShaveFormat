#! /usr/bin/env python3
# 整形前後の意味等価の検証
# Shave Format の整形前後で，空白等を除いた構文木の意味構造を比較する
# 構造・演算子・内容の比較で変質を検出するが，型解決や実行時の等価性を証明する物ではない
# 括弧除去に依る構造の変化は頂点ノード型の差として検出する
# 正しい冗長括弧除去（`(a + b)` → `a + b`）は両者から括弧を剥がせば一致し，誤検出しない
# 使方：
# python3 tools/verify_semantic_preservation.py # 独立した正誤対・全 golden の照合
# python3 tools/verify_semantic_preservation.py ruby cpp # 言語フィルタ
from __future__ import annotations

from cpp_lex import CppSource, CppToken, cpp_source, cpp_tokens, logical_tokens
from tree_work import Work, evaluate

import ast

from bisect import bisect_left, bisect_right
from collections import Counter, deque
from dataclasses import dataclass
from functools import lru_cache
from html import unescape

import json
import os

from pathlib import Path

import re

from shutil import which

import subprocess
import sys

from tempfile import TemporaryDirectory
from typing import Callable, Iterator, TypeAlias
from unittest.mock import patch

# 属性を値ごと読み，名前の一部分や別の属性の引用値を `type` と見做さない
ATTRIBUTE: re.Pattern[str] = re.compile(r"""([^\s/=<>]+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))?""")

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

def embedded_language(name: str, attributes: str) -> str|None:
	"""完全な属性名と MIME の値から，単独と同じ規則で読む子の言語を選ぶ
	計算量：属性の文字数 L に対して O(L)
	引数：
		name: 埋込要素の名前
		attributes: 開タグの属性の原文
	戻値：
		単独で解析する言語．未対応の形式なら `None`
	"""
	attrs: list[re.Match[str]] = list(ATTRIBUTE.finditer(attributes))
	kind: str = unescape(
		next(
			(next((value for value in attr.groups()[1:] if value is not None), "") for attr in attrs if attr.group(1).lower() == "type"),
			""
		)
	).strip().lower()
	if name == "script" and (not kind or kind == "module" or kind.split(";", 1)[0].strip() in SCRIPT_TYPES):
		# 単独の JavaScript と同じ子の言語の返戻
		return "javascript"
	if name == "script" and kind in ("application/json", "application/ld+json", "importmap", "speculationrules"):
		# JSON の独立した子の言語の返戻
		return "json"
	if name == "style" and kind in ("", "text/css"):
		# 単独の CSS と同じ子の言語の返戻
		return "css"
	# 単独の子の言語を選べない事の返戻
	return None

# 検査対象の経路と実行ファイル
ROOT: Path = Path(__file__).resolve().parent.parent
# Windows の実行ファイルは拡張子を持つ
EXE: str = ".exe" if sys.platform == "win32" else ""
FMT: Path = ROOT / f"build/shavefmt{EXE}" if (ROOT / f"build/shavefmt{EXE}").is_file() else ROOT / f"bin/shavefmt{EXE}"
DUMPAST: Path = ROOT / f"build/dumpast{EXE}" if (ROOT / f"build/dumpast{EXE}").is_file() else ROOT / f"bin/dumpast{EXE}"
COMMAND_TIMEOUT: int = 120

# キー →（`shavefmt` の `--lang` 識別子，`dumpast` の拡張子）
# tsx のみ ID と拡張子が相違する
LANGS: dict[str, tuple[str, str]] = {
	"c": ("c", "c"),
	"cpp": ("cpp", "cpp"),
	"csharp": ("csharp", "cs"),
	"java": ("java", "java"),
	"go": ("go", "go"),
	"rust": ("rust", "rs"),
	"kotlin": ("kotlin", "kt"),
	"swift": ("swift", "swift"),
	"php": ("php", "php"),
	"javascript": ("javascript", "js"),
	"typescript": ("typescript", "ts"),
	"tsx": ("typescriptreact", "tsx"),
	"ruby": ("ruby", "rb"),
	"python": ("python", "py"),
	"html": ("html", "html"),
	"css": ("css", "css"),
	"scss": ("scss", "scss")
}

# 条文から定めた意味比較の正誤対（真は保存，偽は拒否）
SELF_CHECK_PAIRS: list[tuple[str, str, str, str, bool]] = [
	("cpp", "返戻型の括弧の保護", "decltype(auto) f(int &x){return x;}", "decltype(auto) f(int &x){return (x);}", False),
	("ruby", "末尾以外の返戻の保持", "def f; return 1; return 2; end", "def f; 1; 2; end", False),
	("php", "地の文の空白変更", "<?php f(); ?>Read   Return", "<?php f(); ?>Read Return", False),
	("ruby", "終端データの大小文字", "puts 1\n__END__\nRead_10", "puts 1\n__END__\nread10", False),
	("python", "デバッグ式の表記", "f\"{ x + y = }\"", "f\"{x+y=}\"", False),
	("html", "空白保持本文", "<pre>Read   Return</pre>", "<pre>Read Return</pre>", False),
	("html", "コメント本文", "<!-- Read   Return -->", "<!-- Read Return -->", False),
	("html", "空要素に続く本文の欠落", "<p>alpha<br/>beta</p>", "<p>alpha<br></p>", False),
	("html", "外来内容の閉じの喪失", "<svg><path/><circle/></svg>", "<svg><path><circle/></svg>", False),
	(
		"html",
		"HTML へ戻る名前空間の境界",
		"<svg><desc><br/></desc></svg><math><mi><br/></mi></math>",
		"<svg><desc><br></desc></svg><math><mi><br></mi></math>",
		True
	),
	("scss", "改頁で終わる行コメント後の値の変更", ".alpha{// note\fwidth:1px;}", ".alpha{// note\nwidth:2px;}", False),
	("scss", "補間の式の変更", ".alpha{content:'x #{$count + 1} y';}", ".alpha{content:\"x #{$count + 2} y\";}", False),
	("scss", "補間の有効化", ".alpha{content:'\\#{$count}';}", ".alpha{content:\"#{$count}\";}", False),
	(
		"scss",
		"URL の内部の引用本文の変更",
		".alpha{background:url(<svg a=\"b\"></svg>);}",
		".alpha{background:url(<svg a=\"c\"></svg>);}",
		False
	),
	(
		"scss",
		"URL の後の値の変更",
		".alpha{background:url(<svg a=\"b\"></svg>);color:red;}",
		".alpha{background:url(<svg a=\"b\"></svg>);color:blue;}",
		False
	),
	("scss", "空でない実引数の喪失", "@include ns.alpha(1px);", "@include ns.alpha;", False),
	("css", "URL 引数の綴り", ".x{background:url(0xAB)}", ".x{background:url(0xab)}", False),
	(
		"cpp",
		"一方の枝だけの取消",
		"#define S(x) #x\n#if X\n#undef S\n#endif\nint f(){return S(a+b);}",
		"#define S(x) #x\n#if X\n#undef S\n#endif\nint f(){return S(a + b);}",
		False
	),
	(
		"cpp",
		"入子の枝の取消",
		"#define S(x) #x\n#if X\n#if Y\n#undef S\n#else\n#undef S\n#endif\n#else\n#undef S\n#endif\nint f(){return S(a+b);}",
		"#define S(x) #x\n#if X\n#if Y\n#undef S\n#else\n#undef S\n#endif\n#else\n#undef S\n#endif\nint f(){return S(a + b);}",
		True
	),
	(
		"cpp",
		"取消後の文字列化の再定義",
		"#define S(x) #x\n#undef S\n#define S(x) #x\nconst char *s=S(a+b);\n",
		"#define S(x) #x\n#undef S\n#define S(x) #x\nconst char *s=S(a + b);\n",
		False
	),
	("rust", "マクロの数値の区切", "fn f()-> &'static str { stringify!(1_0) }", "fn f()-> &'static str { stringify!(10) }", False),
	("c", "繰返更新の所属", "void f(){for(;0;){g();break;}}", "void f(){for(;;0){g();break;}}", False),
	("c", "繰返条件の所属", "void f(){for(;0;){g();break;}}", "void f(){for(0;;){g();break;}}", False),
	(
		"cpp",
		"可変実引数の区切空白",
		"#define S(...) #__VA_ARGS__\nconst char *s=S(x, y);\n",
		"#define S(...) #__VA_ARGS__\nconst char *s=S(x,y);\n",
		False
	),
	("c", "文字列化する空白", "#define S(x) #x\nconst char *s=S(x + y);\n", "#define S(x) #x\nconst char *s=S(x+y);\n", False),
	("cpp", "文字列化する第２引数", "#define S(a,b) a #b\nint n=S(x, (y));\n", "#define S(a,b) a #b\nint n=S(x, y);\n", False),
	(
		"cpp",
		"文字列化を転送する呼出",
		"#define S(x) #x\n#define Q(x) S(x)\nconst char *s=Q((x));\n",
		"#define S(x) #x\n#define Q(x) S(x)\nconst char *s=Q(x);\n",
		False
	),
	(
		"cpp",
		"標準のアサーションの表記",
		"#include <cassert>\nvoid f(int x){assert((x));}\n",
		"#include <cassert>\nvoid f(int x){assert(x);}\n",
		False
	),
	("c", "宣言の本体", "void f(){if(x){int y=2;}}", "void f(){if(x)int y=2;}", False),
	("cpp", "decltype の参照型", "int x; decltype((x)) y=x;", "int x; decltype(x) y=x;", False),
	("cpp", "推論返戻の参照型", "int x; decltype(auto) f(){return (x);}", "int x; decltype(auto) f(){return x;}", False),
	("csharp", "式内の束縛", "class C{void F(){if(x){G(out var y);}}}", "class C{void F(){if(x)G(out var y);}}", False),
	("go", "ビルド条件の改変", "//go:build linux\n\npackage p\n", "//go:build windows\n\npackage p\n", False),
	("go", "ビルド条件の欠落", "//go:build linux\n\npackage p\n", "package p\n", False),
	("go", "指示形の有効化", "// go:build linux\n\npackage p\n", "//go:build linux\n\npackage p\n", False),
	(
		"go",
		"埋込パターンの改変",
		"package p\nimport _ \"embed\"\n//go:embed a.txt\nvar value string\n",
		"package p\nimport _ \"embed\"\n//go:embed b.txt\nvar value string\n",
		False
	),
	(
		"go",
		"同名の別所有者への移動",
		"package p\nfunc a(){\n//go:noinline\nvar x int\n}\nfunc b(){var x int}\n",
		"package p\nfunc a(){var x int}\nfunc b(){\n//go:noinline\nvar x int\n}\n",
		False
	),
	("go", "生成指示の桁", "package p\n//go:generate echo x\n", "package p\n\t//go:generate echo x\n", False),
	("go", "前文の変更", "package p\n/*int x;*/\nimport \"C\"\n", "package p\n/*int y;*/\nimport \"C\"\n", False),
	("go", "出力注記の変更", "package p\nfunc Example(){\n// Output: a\n}\n", "package p\nfunc Example(){\n// Output: b\n}\n", False),
	("rust", "初期化子の末尾値の破棄", "fn f(){let value={1;};}", "fn f(){let value={1};}", False),
	("rust", "クロージャの末尾値の破棄", "fn f(){let value=||{1;};}", "fn f(){let value=||{1};}", False),
	("rust", "マクロのセミコロン", "fn f(){m!(a;);}", "fn f(){m!(a);}", False),
	("rust", "マクロの空の括弧", "fn f(){m!(());}", "fn f(){m!();}", False),
	("rust", "マクロの照合側の区切", "macro_rules! m {(a;) => {x();};}", "macro_rules! m {(a) => {x();};}", False),
	("rust", "マクロの照合側の繰返", "macro_rules! m {($($a:ident;)*) => {};}", "macro_rules! m {($($a:ident)*) => {};}", False),
	(
		"rust",
		"マクロの展開側の繰返",
		"macro_rules! m {($($a:ident),*) => {$($a();)*};}",
		"macro_rules! m {($($a:ident),*) => {$($a())*};}",
		False
	),
	("rust", "マクロの末尾トークン", "fn f(){m!(a,);}", "fn f(){m!(a);}", False),
	("rust", "ドキュメントの説明の変更", "/// a\nfn a() {}\n", "/// b\nfn a() {}\n", False),
	(
		"javascript",
		"文字列ディレクティブの有効化",
		"function f(){(\"use strict\"); return this;}",
		"function f(){\"use strict\"; return this;}",
		False
	),
	("javascript", "省略可能チェーンの短絡範囲", "const obj=null; const value=(obj?.m)();", "const obj=null; const value=obj?.m();", False),
	("javascript", "空要素の削除", "function f(){return [1,,2];}", "function f(){return [1,2];}", False),
	("javascript", "空要素の追加", "function f(){return [1,2];}", "function f(){return [1,,2];}", False),
	("javascript", "空要素の移動", "const a=[1,,2];", "const a=[1,2,,];", False),
	("javascript", "末尾の空要素", "const a=[1,,];", "const a=[1,];", False),
	("javascript", "ローカル束縛の公開", "function f(){{let y=2;}return typeof y;}", "function f(){let y=2;return typeof y;}", False),
	("javascript", "後続節の所属", "if(x){if(y)g();}else h();", "if(x)if(y)g();else h();", False),
	("javascript", "空の本体の保持", "if(x){}", "if(x);", False),
	("javascript", "先頭記法の変更", "#!/usr/bin/node\nconst x=1;\n", "#!/usr/bin/env node\nconst x=1;\n", False),
	("typescript", "非空アサーション越しの短絡範囲", "const value=((obj?.m)!)();", "const value=obj?.m!();", False),
	("typescript", "分解の空要素", "const [x,,y]=a;", "const [x,y]=a;", False),
	("typescript", "参照指示の変更", "/// <reference path=\"a.ts\" />\nlet x=1;\n", "/// <reference path=\"b.ts\" />\nlet x=1;\n", False),
	("ruby", "仮引数の分解", "each { |a,| use(a) }", "each { |a| use(a) }", False),
	("ruby", "文字列凍結の変更", "# frozen_string_literal: true\nx=\"a\"\n", "# frozen_string_literal: false\nx=\"a\"\n", False),
	("python", "アノテーションの記録", "x=1\n(x): int = 2\n", "x=1\nx: int = 2\n", False),
	("python", "エンコーディングの変更", "# coding: utf-8\nx=1\n", "# coding: latin-1\nx=1\n", False),
	("python", "エンコーディングの有効化", "\n\n# coding: utf-8\nx=1\n", "# coding: utf-8\nx=1\n", False),
	("html", "属性の本文の空白", "<p title=\"Read   Return\">word</p>", "<p title=\"Read Return\">word</p>", False),
	("css", "エスケープした URL の本文", ".x{background:u\\72l(0xAB)}", ".x{background:u\\72l(0xab)}", False),
	("rust", "マクロの区切の除去", "fn f(){m!(a b);}", "fn f(){m!(ab);}", False),
	("rust", "マクロの結合する記号", "fn f(){m!(a::b);}", "fn f(){m!(a: :b);}", False),
	("rust", "マクロのレイアウト非依存", "m!{a + 1, (b,)}", "m!{\n a  +\n 1,\n(b,)\n}", True),
	("rust", "マクロの記号の密着", "m!(+*);", "m!(+ *);", False),
	("rust", "マクロのコメントの区切", "m!(+/* note */*);", "m!(+ *);", True),
	("rust", "未展開マクロの間隙の圧縮", "fn f(){stringify!(a  + b);}", "fn f(){stringify!(a + b);}", True),
	("php", "停止後のデータの保持", "<?php __halt_compiler();  DATA\n", "<?php __halt_compiler(); DATA\n", False),
	("php", "停止前の構文破壊", "<?php $x=1;__halt_compiler();DATA", "<?php $x=;__halt_compiler();DATA", False),
	("php", "文字列内の偽の停止", "<?php $x=\"__halt_compiler();\";$y=1;", "<?php $x=\"__halt_compiler();\";$y=;", False),
	("html", "埋込の式の変更", "<script>let x=(a+b);</script>", "<script>let x=a - b;</script>", False),
	("html", "埋込の文字列の本文", "<script>let x=\"a  b\";</script>", "<script>let x=\"a b\";</script>", False),
	("html", "埋込のディレクティブ", "<script>\"use strict\";f();</script>", "<script>\"use asm\";f();</script>", False),
	("html", "未対応の埋込の保持", "<script type=\"x-custom\">a  b</script>", "<script type=\"x-custom\">a b</script>", False),
	("html", "CSS の子孫結合子", "<style>.a .b{color:red}</style>", "<style>.a.b{color:red}</style>", False),
	("css", "カスタム値のトークンの区切", ".a{--x:1 +2;}", ".a{--x:1+2;}", False),
	("css", "カスタム値の分離コメント", ".a{--x:a/**/b;}", ".a{--x:ab;}", False),
	("css", "空白を値に持つカスタムプロパティ", ".x{--x:  ;}", ".x{--x: ;}", False)
]

# 型の取得では，裸の名前・メンバの式と括弧付の式を区別する
SELF_CHECK_PAIRS.extend(
	[
		("cpp", "推論初期値の括弧の除去：大域", "int x; decltype(auto) y=(x);", "int x; decltype(auto) y=x;", False),
		(
			"cpp",
			"推論初期値の括弧の除去：メンバ",
			"struct S{int x;}; S a; decltype(auto) y=(a.x);",
			"struct S{int x;}; S a; decltype(auto) y=a.x;",
			False
		),
		(
			"cpp",
			"推論初期値の括弧の除去：ポインタのメンバ",
			"struct S{int x;}; S a; S*p=&a; decltype(auto) y=(p->x);",
			"struct S{int x;}; S a; S*p=&a; decltype(auto) y=p->x;",
			False
		)
	]
)

# 真偽値の初期化は，初期化の共通の形だけを真偽値リテラルへ変え，型と被演算子の違う初期値を変えない
SELF_CHECK_PAIRS.extend(
	[
		(
			"cpp",
			"真偽値の初期化の共通の形",
			"bool a(0);bool b{1};bool c={0};bool d[2]={0,1};bool (e)=1;bool f(bool x=(1),bool=0);",
			"bool a(false);bool b{true};bool c={false};bool d[2]={false,true};bool (e)=true;bool f(bool x=true,bool=false);",
			True
		),
		("cpp", "配列ポインタの初期値", "bool *a[2]={0,0};", "bool *a[2]={false,false};", False),
		("cpp", "初期化式の被演算子", "bool a{0+1};", "bool a{false+true};", False),
		(
			"c",
			"真偽値配列の指示付初期化",
			"#include <stdbool.h>\n_Bool a[2]={[0]=1,[1]={0}};",
			"#include <stdbool.h>\n_Bool a[2]={[0]=true,[1]={false}};",
			True
		),
		("c", "指示先のインデックスの真偽値化", "#include <stdbool.h>\n_Bool a[2]={[0]=1};", "#include <stdbool.h>\n_Bool a[2]={[true]=true};", False)
	]
)

SELF_CHECK_PAIRS.extend(
	[
		(
			"cpp",
			"推論初期値のテンプレート名",
			"template<class T>void x(){} decltype(auto)y=(x<int>);",
			"template<class T>void x(){} decltype(auto)y=x<int>;",
			False
		)
	]
)

def is_comment(node_type: str) -> bool:
	"""コメントノードか判定する
	計算量：型名の文字数 L に対して O(L)

	引数：
		node_type: ノード型

	戻値：
		コメントノードなら True
	"""
	# コメント判定の返戻
	return "comment" in node_type

# 通常関数の末尾の補完は許し，非末尾への挿入と返戻値の変更は拒否する
SELF_CHECK_PAIRS.extend(
	[
		("cpp", "非末尾への返戻の挿入", "void f(){g();g();}", "void f(){g();return;g();}", False),
		("cpp", "返戻値の変更", "void f(){return 1;}", "void f(){return 2;}", False)
	]
)

SELF_CHECK_PAIRS.extend(
	[
		("cpp", "到達不能な返戻の除去", "int f(){return 1;return;}", "int f(){return 1;}", True),
		("cpp", "main の終了値の変更", "int main(){g();}", "int main(){g();return 1;}", False),
		("cpp", "空の本体への返戻", "void f(){}", "void f(){return;}", False),
		("cpp", "全枝が脱出した後への返戻", "void f(){if(x)return;else throw 1;}", "void f(){if(x)return;else throw 1;return;}", False),
		("cpp", "到達可能な返戻の除去", "void f(){g();return;}", "void f(){g();}", False),
		("java", "定数変数化する final", "class A{void f(){int x=1;h(x);}}", "class A{void f(){final int x=1;h(x);}}", False),
		("rust", "借用に要る mut", "fn f(){let mut x=1;g(&mut x);}", "fn f(){let x=1;g(&mut x);}", False),
		(
			"javascript",
			"eval 内の束縛の const",
			"function f(){let x=1;eval('x=2');g(x);}",
			"function f(){const x=1;eval('x=2');g(x);}",
			False
		),
		("ruby", "begin の後判定への変更", "while x\nbegin;g();end\nend", "begin;g();end while x", False),
		("ruby", "条件の束縛の変更", "if x=g()\nh(x)\nend", "h(x) if x=g()", False)
	]
)

# 指定子の所属と初期化の順序を保ち，宣言子の欠落・評価順の変更は拒否する
SELF_CHECK_PAIRS.extend(
	[
		("cpp", "初期化の値の変更", "int f(){int a=1,b=2;return a+b;}", "int f(){int a=9,b=2;return a+b;}", False),
		("cpp", "宣言子の欠落", "int f(){int a=1;int b=2;return a+b;}", "int f(){int a=1;return a+b;}", False),
		("cpp", "初期化の評価順の変更", "int f(){int a=f(),b=g();return a+b;}", "int f(){int b=g(),a=f();return a+b;}", False)
	]
)

SELF_CHECK_PAIRS.extend(
	[
		("typescript", "インポートの別名の変更", "import {B as C,A} from \"x\";", "import {A,B as D} from \"x\";", False),
		(
			"typescript",
			"インポート文の評価順の変更",
			"import {A} from \"x\";import {B} from \"y\";",
			"import {B} from \"y\";import {A} from \"x\";",
			False
		)
	]
)

# 交差型の簡約の境界を保持し，合併の結合と冗長なラッパーを区別する
SELF_CHECK_PAIRS.extend(
	[
		("typescript", "交差型の右グループ", "type X = string & (string & {});", "type X = string & string & {};", False),
		("typescript", "弱い型演算の括弧", "type X = (\"foo\" | string) & {};", "type X = \"foo\" | string & {};", False),
		("typescript", "条件型の合併", "type X = (string extends string ? 1 : 2) | 3;", "type X = string extends string ? 1 : 2 | 3;", False),
		("typescript", "関数型の合併", "type X = (() => string) | number;", "type X = () => string | number;", False),
		("typescript", "配列型の合併", "type X = (string | number)[];", "type X = string | number[];", False),
		("typescript", "型の要素の欠落", "type X = string & number & {};", "type X = string & {};", False),
		("typescript", "型の要素の並替", "type X = string | number | {};", "type X = number | string | {};", False)
	]
)

# 宣言型の保存は利用先の有無と独立に確認し，未知の推論を許可へ読み替えない
SELF_CHECK_PAIRS.extend(
	[
		("typescript", "未参照の数値", "function f(){let x=1;}", "function f(){const x=1;}", False),
		("tsx", "BigInt の推論", "function f(){let x=1n;return x;}", "function f(){const x=1n;return x;}", False)
	]
)

# 増減の値が制御の枝から何処へ渡るかを，独立した正誤対で確認する
SELF_CHECK_PAIRS.extend(
	[
		(
			"kotlin",
			"増減の値の使用：if の初期化子",
			"fun f(c: Boolean, xs: List<Int>) {var y: Int = 0; val z = if(c) y++ else 0}",
			"fun f(c: Boolean, xs: List<Int>) {var y: Int = 0; val z = if(c) ++y else 0}",
			False
		),
		(
			"kotlin",
			"増減の値の使用：ラムダの末尾値",
			"fun f(c: Boolean, xs: List<Int>) {var y: Int = 0; val action = {y++}}",
			"fun f(c: Boolean, xs: List<Int>) {var y: Int = 0; val action = {++y}}",
			False
		),
		(
			"kotlin",
			"増減の値の使用：明示の返戻の枝の値",
			"fun f(c: Boolean, xs: List<Int>) {var y: Int = 0; fun nested(): Int {return if(c) y++ else 0}}",
			"fun f(c: Boolean, xs: List<Int>) {var y: Int = 0; fun nested(): Int {return if(c) ++y else 0}}",
			False
		)
	]
)

SELF_CHECK_PAIRS.extend(
	[
		("kotlin", "未使用の増減対象の変更", "fun f(){if(c) y++}", "fun f(){if(c) ++z}", False),
		("kotlin", "未使用の増減方向の変更", "fun f(){if(c) y++}", "fun f(){if(c) --y}", False)
	]
)

class Node:
	"""dumpast の罫線の木を解析した生ノード（型・名前付 区分・テキスト・子）"""

	# 属性の宣言・初期化の区分に対応する為，フィールドの順を保持
	__slots__: tuple[str, ...] = "kind", "type", "_text", "children", "source", "start", "end", "field", "comment_text"
	kind: str
	type: str
	children: list[Node]

	@property
	def text(self: Node) -> str:
		"""共有する原文の範囲から必要な本文だけを読む
		引数：self: 原文上の範囲又は手記述の本文を持つノード
		戻値：原文上の開始・終端の間のテキスト，原文が無い場合は保存した本文
		"""
		# 共有する原文又は手書の本文の返戻
		return self._text if self.source is None else self.source[self.start:self.end]

	@staticmethod
	def get_named_children(node: Node) -> list[Node]:
		"""コメントを除いた名前付の子の一覧を返す
		計算量：直下の子の数 N に対して O(N)

		引数：
			node: 対象ノード

		戻値：
			コメント以外の名前付子
		"""
		# 名前付の子の一覧の返戻
		return [child for child in node.children if child.kind == "N" and not is_comment(child.type)]

	@staticmethod
	def has_comma(node: Node) -> bool:
		"""直下にカンマのトークンを持つか判定する
		計算量：直下の子の数 N に対して O(N)

		引数：
			node: 対象ノード

		戻値：
			カンマを持てば True
		"""
		# カンマのトークンの有無の返戻
		return any(child.kind == "U" and child.text.strip() == "," for child in node.children)

	@staticmethod
	def is_empty_list(node: Node, cache: dict[Node, bool]|None = None) -> bool:
		"""区切以外の無名トークンも含め，仮引数・実引数の内容が空か判定する
		計算量：未計数の部分木のノード数 N・トークンの本文長 B に対して O(N+B)，再取得は O(1)
		引数：
			node: 対象ノード
			cache: 同じ正規化で共有する計数結果．未指定なら此の呼出内だけで使う
		戻値：
			空の並びなら真
		"""
		# 子から親へ結果を控え，非空の末端を毎回再探索しない
		if cache is None:
			cache = {}
		item: Node
		contents: list[Node]|None
		pending: list[tuple[Node, list[Node]|None]] = [(node, None)]
		while pending:
			item, contents = pending.pop()
			if item in cache:
				continue
			if contents is not None:
				cache[item] = all(cache[child] for child in contents)
				continue
			if item.type not in EMPTY_LISTS or not item.children:
				cache[item] = False
				continue
			contents = [
				child for child in item.children if not is_comment(child.type) and
				not (child.kind == "U" and child.text.strip() in ("", "(", ")"))
			]
			pending.append((item, contents))
			pending.extend((child, None) for child in reversed(contents))
		# 此の呼出の対象の結果の返戻
		return cache[node]

	@staticmethod
	def has_unscoped_binding(node: Node) -> bool:
		"""単一文が其のラッパーの有効範囲へ名前を導入するかを返す
		内側で束縛を完結する本体・関数・ループ見出は外へ移さない
		計算量：対象部分木のノード数 N に対して O(N)
		引数：
			node: ラッパー内の単一文又は其の子
		戻値：
			ラッパーに依存する束縛を導入するなら真
		"""
		# 所属の要約を子から親へ１度投影する
		return TreeFacts([node]).binding[node]

	@text.setter
	def text(self: Node, value: str) -> None:
		"""手書の木又は従来のダンプの本文を保持する
		引数：self: 本文を設定するノード，value: 原文の範囲に代わって保持するテキスト
		"""
		self._text: str = value
		self.source: str|None = None
		self.comment_text: str|None = None
		# 本文の設定の終了
		return

	def __init__(self: Node, text: str, kind: str, type_: str) -> None:
		"""生ノードを種別 (N/U)・型・テキストで初期化する

		引数：
			self: 初期化する生ノード
			text: ノードの原文
			kind: 名前付 区分 (N/U)
			type_: ノード型
		"""
		# ノードの属性の初期化
		self.source: str|None = None
		self.field: str = ""
		self.start: int = -1
		self.end: int = -1
		self.kind: str = kind
		self.type: str = type_
		self.text: str = text
		self.children: list[Node] = []
		# 終了
		return

# 行から「マーカの前の接頭辞」「N/U 区分」「型」「テキスト」を抽出する正規表現
_LINE: re.Pattern[str] = re.compile(r"^((?:\t|│\t)*)(?:├─|└─)\[([NU]):")

class AstDump(str):
	"""位置だけのダンプと，共有する原文の組"""

	def __new__(cls: type[AstDump], text: str, source: str) -> AstDump:
		"""既存の文字列の入口を保って原文を渡す
		引数：
			cls: 構築するダンプの型
			text: 位置を出したダンプの文字列
			source: 範囲の所有者である原文
		戻値：
			原文を伴うダンプの文字列
		"""
		result: AstDump = super().__new__(cls, text)
		result.source: str = source
		# 原文を伴う位置のダンプの返戻
		return result

def parse_spans(src: str, dump_text: str) -> list[Node]:
	"""バイト範囲を１度文字位置へ投影して，共有する原文の木を作る
	計算量：原文のバイト数 B とノード数 N に対して O(B+N)
	引数：
		src: ノードが共有する原文
		dump_text: 階層・種別・範囲・子の役割を持つ JSON の行
	戻値：
		原文を共有する最上位のノード
	送出例外：
		ValueError: 範囲又は階層が不正な場合
	"""
	positions: dict[int, int] = { 0: 0 }
	byte: int = 0
	for idx, char in enumerate(src):
		byte += len(char.encode("utf-8"))
		positions[byte] = idx + 1
	roots: list[Node] = []
	stack: list[Node] = []
	depth: int
	kind: str
	name: str
	begin: int
	end: int
	field: str
	for line in dump_text.splitlines():
		depth, kind, name, begin, end, field = json.loads(line)
		if (
			not isinstance(depth, int) or depth < 0 or not depth < len(stack) + 1 or kind not in ("N", "U") or not isinstance(name, str) or
			not isinstance(field, str) or begin not in positions or end not in positions or end < begin
		):
			raise ValueError("invalid AST span")
		del stack[depth:]
		node: Node = Node("", kind, name)
		node.source, node.start, node.end = src, positions[begin], positions[end]
		node.field: str = field
		if stack:
			parent: Node = stack[-1]
			if node.start < parent.start or parent.end < node.end or parent.children and parent.children[-1].end > node.start:
				raise ValueError("AST span is outside its owner")
			parent.children.append(node)
		else:
			roots.append(node)
		stack.append(node)
	if len(roots) == 1:
		roots[0].start, roots[0].end = 0, len(src)
	# 原文を共有する根の列の返戻
	return roots

def parse_dump(dump_text: str, src: str|None = None) -> list[Node]:
	"""dumpast の全出力を解析し最上位の木グループを返す

	計算量：O(n)（n は入力文字数）

	引数：
		dump_text: dumpast の出力
		src: 物理位置の比較用の原文（解析木が含めない先頭空白も保つ）

	戻値：
		最上位ノードの一覧

	送出例外：
		ValueError: 出力の行又はノード値が不正な場合
		json.JSONDecodeError: 読み込んだ内容が有効な JSON でない場合
	"""
	if src is None:
		src = getattr(dump_text, "source", None)
	if dump_text.startswith("["):
		if src is None:
			raise ValueError("AST spans require their source")
		# 保護データと埋込の所属を投影した木の返戻
		return prepare_roots(parse_spans(src, dump_text), src)
	# 解析木のテキストの読取の準備
	roots: list[Node] = []
	stack: list[tuple[int, Node]] = []
	# 構文木の各行からのノードの復元
	for line in dump_text.split("\n"):
		if not line.strip():
			continue
		matched: re.Match[str]|None = _LINE.match(line)
		if not matched:
			raise ValueError(f"invalid AST dump line: {line!r}")
		depth: int = matched.group(1).count("\t")
		tail: str = line[matched.end():]
		node_type: object
		end: int
		node_type, end = json.JSONDecoder().raw_decode(tail)
		if tail[end:end + 2] != "] ":
			raise ValueError(f"invalid AST node type: {line!r}")
		text: object = json.loads(tail[end + 2:])
		if not isinstance(node_type, str) or not isinstance(text, str):
			raise ValueError("AST node type and text must be strings")
		# 親子関係と根の構築
		node: Node = Node(text, matched.group(2), node_type)
		while stack and stack[-1][0] >= depth:
			stack.pop()
		if stack:
			stack[-1][1].children.append(node)
		else:
			roots.append(node)
		stack.append((depth, node))
	# 根の範囲から省かれた先頭空白を位置の比較へ戻す
	if src is not None and len(roots) == 1:
		roots[0].text: str = src
	# 木グループの返戻
	return roots

@lru_cache(maxsize=256)
def dump_ast(code: str, ext: str) -> str:
	"""dumpast で完全な構文木の文字列を得る
	計算量：原文の文字数 L・パーサーの実行時間 T に対して O(L+T)

	引数：
		code: 解析するソース
		ext: 入力言語の拡張子

	戻値：
		dumpast の標準出力

	送出例外：
		RuntimeError: dumpast が失敗した場合
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		subprocess.TimeoutExpired: 呼び出した子プロセスが制限時間内に完了しない場合
	"""
	# パーサーに依る構文木の取得
	completed: subprocess.CompletedProcess[str] = (
		subprocess.run([str(DUMPAST), "--spans", "-", ext], input=code, capture_output=True, text=True, timeout=COMMAND_TIMEOUT)
	)
	if completed.returncode:
		raise RuntimeError(f"dumpast failed ext={ext}: {completed.stderr}")
	# 構文木の文字列の返戻
	return AstDump(completed.stdout, code)

def prepare_roots(roots: list[Node], src: str) -> list[Node]:
	"""停止ディレクティブのデータと，解析出来る埋込の子を所属へ投影する
	計算量：親子の解析費用 P・ノード数 N・本文の文字数 L に対して O(P+N+L)
	引数：
		roots: 原文の範囲を持つ解析木
		src: 親の原文
	戻値：
		保護データと埋込の子の所属を分けた同じ根の列
	送出例外：
		RuntimeError: 埋込のパーサーが失敗した場合
		OSError: パーサーを起動出来ない場合
		ValueError: 子の位置のダンプが不正な場合
		subprocess.TimeoutExpired: 子の解析が制限時間内に完了しない場合
	"""
	if len(roots) == 1 and roots[0].type == "program" and any(child.type == "php_tag" for child in roots[0].children):
		root: Node = roots[0]
		for idx, statement in enumerate(root.children):
			if statement.type != "expression_statement":
				continue
			calls: list[Node] = Node.get_named_children(statement)
			if len(calls) != 1 or calls[0].type != "function_call_expression":
				continue
			parts: list[Node] = Node.get_named_children(calls[0])
			if (
				len(parts) != 2 or parts[0].type != "name" or parts[0].text.lower() != "__halt_compiler" or parts[1].type != "arguments" or
				Node.get_named_children(parts[1]) or not any(child.text == ";" for child in statement.children)
			):
				continue
			tail: Node = Node("", "N", "protected_php_tail")
			tail.source, tail.start, tail.end = src, statement.end, len(src)
			# エラー一般を免除せず，停止位置より後だけを未解析データとして分ける
			root.children[idx + 1:] = [tail]
			break
	pending: list[Node] = list(reversed(roots))
	while pending:
		node: Node = pending.pop()
		if node.type in ("script_element", "style_element"):
			start: Node|None = next((child for child in node.children if child.type == "start_tag"), None)
			if start is not None:
				name: str = "script" if node.type == "script_element" else "style"
				lang: str|None = embedded_language(name, start.text[len(name) + 1:-1])
				for child in node.children:
					if child.type == "raw_text" and lang is not None:
						body: str = child.text
						prefix: int = len(body) - len(body.lstrip(" \t\r\n"))
						has_bom: bool = body[prefix:prefix + 1] == "\ufeff"
						if has_bom:
							body = body[:prefix] + " " + body[prefix + 1:]
						child.children: list[Node] = parse_dump(dump_ast(body, LANGS.get(lang, (lang, lang))[1]), body)
						if lang in ("css", "scss"):
							child.children: list[Node] = css_comparison_roots(lang, child.children)
						if lang == "javascript" and unescape(
							next(
								(
									next((value for value in attr.groups()[1:] if value is not None), "") for attr in
									ATTRIBUTE.finditer(start.text[len(name) + 1:-1]) if attr.group(1).lower() == "type"
								),
								""
							)
						).strip().lower() == "module":
							child.children[0].type: str = "module"
						child.type: str = "embedded:" + lang
			continue
		pending.extend(reversed(node.children))
	# 原文を共有する根の列の返戻
	return roots

def is_paren_wrapper(node_type: str) -> bool:
	"""括弧ラッパノード（parenthesized_expression / _statements 等）か判定する
	計算量：型名の文字数 L に対して O(L)

	引数：
		node_type: ノード型

	戻値：
		括弧ラッパなら True
	"""
	# 括弧ラッパ判定の返戻
	return "parenthe" in node_type

def source_leaves(roots: list[Node]) -> list[tuple[Node, int, int]]:
	"""コメントを分解せず，原文に対応する葉と範囲を返す
	計算量：ノード数 N・原文の文字数 L に対してO(N+L)
	引数：
		roots: 原文を持つ解析木
	戻値：
		原文順の葉と開始・終端の位置
	送出例外：
		RuntimeError: ノードの本文を所有範囲に対応付けれない場合
	"""
	found: list[tuple[Node, int, int]] = []
	pending: list[Node] = list(reversed(roots))
	while pending:
		node: Node = pending.pop()
		if is_comment(node.type) or node.type.startswith("embedded:") or not node.children:
			if node.start < 0:
				raise RuntimeError("AST source positions are required")
			found.append((node, node.start, node.end))
		else:
			pending.extend(reversed(node.children))
	# 先行順の葉は原文順なので，再照合も整列も行わない
	return found

# 正規形ノード＝（型，葉のテキスト又は `None`，子のタプル）
NormNode: TypeAlias = tuple[str, str | None, tuple["NormNode", ...]]
# 意味に無関係な構造ラッパ（整形が付与・除去・結合する）
# 型を消し名前付子のみ昇格して透過する
# 内側の式（binary 等）は透過対象外なので演算子・結合は保持され，破壊は中身に残って捕捉される
# 波括弧は所属と束縛を確認した単一文本体だけを透過
# return 引数・メソッド呼出引数のラッパ（暗黙 return の `return E` ↔ `E` を同一視）
# 文末 `;` の有無を吸収
# 空文（孤立した `;`）の削除を吸収
# do / end のループ本体ラッパ（`while c do end` の do 補完を吸収，本体があれば昇格）
TRANSPARENT_WRAPPERS: set[str] = { "argument_list", "do", "empty_statement", "expression_statement" }

# 省略可能な波括弧を持つ言語と制御本体の所属
OPTIONAL_BODY_LANGS: set[str] = {
	"c",
	"cpp",
	"csharp",
	"java",
	"php",
	"javascript",
	"javascriptreact",
	"typescript",
	"typescriptreact",
	"tsx"
}

BODY_BLOCKS: set[str] = { "block", "compound_statement", "statement_block" }

CONTROL_BODIES: set[str] = {
	"do_statement",
	"else_clause",
	"else_if_clause",
	"for_each_statement",
	"for_in_statement",
	"for_range_loop",
	"for_statement",
	"foreach_statement",
	"if_statement",
	"while_statement"
}

# 其の内側で束縛を完結する構文
BINDING_SCOPES: set[str] = (
	BODY_BLOCKS |
	{ "anonymous_function", "anonymous_method_expression", "arrow_function", "function_expression", "lambda", "lambda_expression" }
)

LOOP_SCOPES: set[str] = { "for_each_statement", "for_in_statement", "for_statement", "foreach_statement" }

# 式の中に名前を導入する構文（文の宣言は種別名でも識別）
EXPR_BINDINGS: set[str] = {
	"declaration_expression",
	"declaration_pattern",
	"single_variable_designation",
	"type_definition",
	"var_pattern",
	"variable_designation"
}

# 空要素を持つ配列と分解の並び
ARRAY_LANGS: set[str] = { "javascript", "javascriptreact", "typescript", "typescriptreact", "tsx" }

# 本体の先頭に指示文列を持つ JavaScript の関数
DIRECTIVE_FUNCTIONS: set[str] = {
	"arrow_function",
	"function_declaration",
	"function_expression",
	"generator_function",
	"generator_function_declaration",
	"method_definition"
}

ARRAY_LISTS: set[str] = { "array", "array_pattern" }
# Python の組の種別（括弧の有無と位置で種別が変わり，`return (a, b)` と `return a, b` は同じ組）
PYTHON_TUPLES: set[str] = { "expression_list", "pattern_list", "tuple", "tuple_expression", "tuple_pattern" }
# 仮引数の並び（名前だけの仮引数１つは括弧を省ける：`(x) => x` と `x => x`）
PARAM_LISTS: set[str] = { "formal_parameters", "inferred_parameters", "parameter_list" }
# 仮引数１つを包むノード（型・既定値の無い名前だけなら名前と同じ）
PARAM_WRAPPERS: set[str] = { "parameter", "required_parameter" }

# 仮引数・実引数の列と其のラッパー（値の空の組・単位 `()` は含めない）
EMPTY_LISTS: set[str] = {
	"annotation_argument_list",
	"argument_list",
	"arguments",
	"attribute_argument_list",
	"formal_parameters",
	"lambda_declarator",
	"lambda_parameters",
	"method_parameters",
	"parameter_list",
	"parameters",
	"primary_constructor",
	"value_arguments"
}

# 空列を同じ呼出・生成・省略可能な宣言へ対応付ける所有構文
EMPTY_LIST_OWNERS: dict[str, set[str]] = {
	"annotation_argument_list": { "annotation" },
	"argument_list": {
		"call",
		"call_expression",
		"class_definition",
		"enum_constant",
		"invocation_expression",
		"method_invocation",
		"object_creation_expression",
		"yield"
	},
	"arguments": {
		"anonymous_class",
		"attribute",
		"call_expression",
		"function_call_expression",
		"member_call_expression",
		"new_expression",
		"nullsafe_member_call_expression",
		"object_creation_expression",
		"scoped_call_expression"
	},
	"attribute_argument_list": { "attribute" },
	"lambda_declarator": { "lambda_expression" },
	"lambda_parameters": { "lambda" },
	"method_parameters": { "method", "singleton_method" },
	"primary_constructor": { "class_declaration" },
	"value_arguments": { "call_suffix", "constructor_delegation_call", "constructor_invocation", "enum_entry" }
}

# 数値リテラルらしいノード型のヒント（文字列・シンボルは除外）
NUMERIC_HINTS: tuple[str, ...] = "bin", "decimal", "float", "hex", "imaginary", "int", "number", "oct", "rational", "scientific"

def is_numeric_literal(node_type: str) -> bool:
	"""数値リテラルノードか判定する（表記揺正規化の対象）
	計算量：型名の文字数 L に対して O(L)

	引数：
		node_type: ノード型

	戻値：
		数値リテラルなら True
	"""
	# 数値の種別名の照合
	lowered: str = node_type.lower()
	if "string" in lowered or "char" in lowered or "symbol" in lowered:
		# 値を保持すべき文字列系を対象外とする事の返戻
		return False
	# 数値ヒントに該当するかの返戻
	return any(hint in lowered for hint in NUMERIC_HINTS)

# 内部の空白其の物が値（意味）を持つ葉（文字列・ヒアドキュメントの本文等）
# 厳密にテキスト比較する
CONTENT_HINTS: tuple[str, ...] = "char", "content", "heredoc", "raw", "regex", "string"

def is_content_leaf(node_type: str) -> bool:
	"""空白も値の一部と為る内容の葉（文字列・ヒアドキュメントの本文等）か判定する
	計算量：型名の文字数 L に対して O(L)

	引数：
		node_type: ノード型

	戻値：
		内容の葉なら True
	"""
	# 本文の葉の種別名の照合
	lowered: str = node_type.lower()
	# 内容の葉に当たるかの返戻
	return any(hint in lowered for hint in CONTENT_HINTS)

def is_content_body(node_type: str) -> bool:
	"""文字列・ヒアドキュメントの本文（区切記号を除く実内容）ノードか判定する
	計算量：型名の文字数 L に対して O(L)

	引数：
		node_type: ノード型

	戻値：
		内容本文ノードなら True
	"""
	# 本文のコンテナの種別名の照合
	lowered: str = node_type.lower()
	# 本文ノード（`string_content` / `heredoc_content` / `string_fragment` 等，区切記号は除外）に該当するかの返戻
	return "content" in lowered or "fragment" in lowered

# 通常の式として読み替えない未展開マクロのトークン列とパターン
RUST_TOKEN_TREES: tuple[str, ...] = "token_repetition", "token_repetition_pattern", "token_tree", "token_tree_pattern"

class TreeFacts:
	"""直属の子・括弧の中身・束縛・返戻位置を１回の走査で共有する"""

	def __init__(self: TreeFacts, roots: list[Node]) -> None:
		"""子から親へ意味の要約を投影する
		計算量：ノード数 N に対して O(N)
		引数：
			self: 構築する意味の要約
			roots: 要約を共有する木の根
		"""
		self.named: dict[Node, list[Node]] = {}
		self.commas: set[Node] = set()
		self.inner: dict[Node, Node|None] = {}
		self.binding: dict[Node, bool] = {}
		self.optional: dict[Node, bool] = {}
		self.decl_auto: dict[Node, bool] = {}
		self.decl_initial: set[Node] = set()
		self.ruby_tail: dict[Node, Node|None] = {}
		self.ruby_scopes: dict[Node, Node] = {}
		self.ruby_parents: dict[Node, Node|None] = {}
		self.ruby_bindings: dict[Node, dict[str, int]] = {}
		self.ruby_work: int = 0
		self.go_true: dict[Node, bool]|None = None
		self.go_work: int = 0
		self.update_names: dict[Node, frozenset[Node]] = {}
		self.macro_events: dict[str, list[tuple[int, bool]]]|None = None
		self.macro_parens: set[Node]|None = None
		self.constant_true: set[Node]|None = None
		self.directive_strings: set[Node] = set()
		node: Node
		is_ready: bool
		pending: list[tuple[Node, bool]] = [(root, False) for root in reversed(roots)]
		while pending:
			node, is_ready = pending.pop()
			if not is_ready:
				self.named[node] = Node.get_named_children(node)
				pending.append((node, True))
				pending.extend((child, False) for child in reversed(node.children))
				continue
			named: list[Node] = self.named[node]
			if Node.has_comma(node):
				self.commas.add(node)
			self.inner[node] = (
				(self.inner[named[0]] if len(named) == 1 and node not in self.commas else None) if
				is_paren_wrapper(node.type) or node.type in PYTHON_TUPLES else
				node
			)
			self.binding[node] = (
				False if node.type in BINDING_SCOPES else self.binding[named[-1]] if node.type in LOOP_SCOPES and named else True if
				node.type in EXPR_BINDINGS or node.type == "declaration" or node.type.endswith("_declaration") or
				node.type == "instanceof_expression" and len(named) > 2 else
				any(self.binding[child] for child in named)
			)
			self.optional[node] = bool(
				node.type in ("call_expression", "member_expression", "non_null_expression", "subscript_expression") and
				(any(child.type in ("?.", "optional_chain") for child in node.children) or named and self.optional[named[0]])
			)
			self.decl_auto[node] = bool(
				node.type == "decltype" and any(child.type == "auto" for child in node.children) or
				any(self.decl_auto[child] for child in node.children if child.type not in ("compound_statement", "parameter_list"))
			)
			if any(child.field == "type" and child.type == "placeholder_type_specifier" and self.decl_auto[child] for child in named):
				for child in named:
					values: list[Node] = self.named[child] if child.type == "init_declarator" else [child]
					for value in values:
						if value.field in ("default_value", "value"):
							if value.type == "argument_list" and len(self.named[value]) == 1:
								self.decl_initial.add(self.named[value][0])
							else:
								self.decl_initial.add(value)
			tails: list[Node] = [child for child in named if node.type != "body_statement" or child.type not in ("ensure", "rescue")]
			self.ruby_tail[node] = tails[-1] if tails else None
		# 指示文列は直属の文字列だけの文から読み，式の値と所属を混同しない
		for node, named in self.named.items():
			containers: list[Node] = (
				[node] if node.type == "program" else [child for child in named if child.field == "body" and child.type == "statement_block"] if
				node.type in DIRECTIVE_FUNCTIONS else
				[]
			)
			for container in containers:
				for statement in self.named[container]:
					if is_comment(statement.type) or statement.type == "hash_bang_line":
						continue
					expressions: list[Node] = self.named[statement]
					if statement.type != "expression_statement" or len(expressions) != 1 or expressions[0].type != "string":
						break
					self.directive_strings.add(expressions[0])
		# 所属と意味の要約の構築の終了
		return

class Ancestors:
	"""コピーせず共有する祖先の親リンク"""

	__slots__: tuple[str, ...] = (
		"depth",
		"directive_boundary",
		"empty_lists",
		"facts",
		"html_namespace",
		"is_decltype_owner",
		"is_directive_ready",
		"is_in_macro_tokens",
		"named",
		"node",
		"outer",
		"transparent_outer",
		"transparent_receiver"
	)

	def __len__(self: Ancestors) -> int:
		"""保持する祖先の数を返す
		引数：self: 祖先の親リンク
		戻値：
			祖先の個数
		"""
		# 照合結果の返戻
		return self.depth

	def __getitem__(self: Ancestors, index: int) -> Node:
		"""負の添字で指定した近い祖先を返す
		計算量：添字の絶対値 K に対して O(K)
		引数：
			self: 祖先の親リンク
			index: 直近を `-1` とする負の添字
		戻値：
			指定した祖先
		送出例外：
			IndexError: 添字が保持する範囲外の場合
			RuntimeError: 祖先の親リンク又はノードが宣言した深さと矛盾する場合
		"""
		if not -self.depth <= index < 0:
			raise IndexError(index)
		current: Ancestors = self
		for _ in range(-index - 1):
			if current.outer is None:
				raise RuntimeError("ancestor link ended before its declared depth")
			current = current.outer
		if current.node is None:
			raise RuntimeError("ancestor link has no node at its declared depth")
		# 照合結果の返戻
		return current.node

	def __reversed__(self: Ancestors) -> Iterator[Node]:
		"""直近の親から外側へ祖先を辿る
		引数：self: 祖先の親リンク
		戻値：
			親から根へ辿る反復子
		"""
		current: Ancestors = self
		while current.node is not None:
			yield current.node
			if current.outer is None:
				break
			current = current.outer

	def get_facts(self: Ancestors) -> TreeFacts:
		"""外側の根に属する要約を共有し，明示した祖先の入口でも１度だけ作る
		計算量：未投影の木のノード数 N・祖先の深さ D に対して O(N+D)，再取得は O(1)
		引数：
			self: 現在の祖先の所属
		戻値：
			所属の木と祖先の間で共有する意味の要約
		送出例外：
			ValueError: 木を持たない空の所属から要約を求めた場合
		"""
		if self.facts is None:
			contexts: list[Ancestors] = []
			outer: Ancestors|None = self
			while outer is not None and outer.node is not None:
				contexts.append(outer)
				outer = outer.outer
			if not contexts:
				raise ValueError("AST facts require their owner")
			facts: TreeFacts = TreeFacts([contexts[-1].node])
			is_decltype_owner: bool = False
			for context in reversed(contexts):
				context.facts: TreeFacts|None = facts
				if context.node.type in ("function_definition", "lambda_expression"):
					is_decltype_owner = facts.decl_auto[context.node]
				context.is_decltype_owner: bool = is_decltype_owner
		# 所属に共有する意味の要約の返戻
		return self.facts

	def get_directive_boundary(self: Ancestors) -> Node|None:
		"""コンテナの最初の非ディレクティブ文を１度取得して共有する
		計算量：初回は子の数 N に対して O(N)，再取得は O(1)
		引数：self: ディレクティブの境界を共有するコンテナの所属
		戻値：
			文字列の括弧の除去でディレクティブに変わり得る最初の文
		"""
		if not self.is_directive_ready:
			self.is_directive_ready: bool = True
			statements: list[Node] = (
				self.named if self.named is not None else Node.get_named_children(self.node) if self.node is not None else []
			)
			for statement in statements:
				if statement.type == "hash_bang_line":
					continue
				expressions: list[Node] = Node.get_named_children(statement)
				if statement.type != "expression_statement" or len(expressions) != 1 or expressions[0].type != "string":
					self.directive_boundary: Node|None = statement
					break
		# 同じコンテナの子の間で共有する境界の返戻
		return self.directive_boundary

	def __init__(self: Ancestors, outer: Ancestors|None = None, node: Node|None = None, named: list[Node]|None = None) -> None:
		"""直近の祖先と外側のリンクを保持する
		引数：
			self: 構築する祖先の親リンク
			outer: 更に外側のリンク．最外では `None`
			node: 直近の祖先．空のリンクでは `None`
			named: 呼出元が取得済の名前付子．未取得なら `None`
		"""
		self.facts: TreeFacts|None = outer.facts if outer is not None else None
		self.is_decltype_owner: bool = bool(outer and outer.is_decltype_owner)
		if node is not None and node.type in ("function_definition", "lambda_expression"):
			self.is_decltype_owner: bool = bool(self.facts and self.facts.decl_auto[node])
		self.transparent_outer: Ancestors|None = self
		self.transparent_receiver: Node|None = node
		if node is not None and outer is not None and (is_paren_wrapper(node.type) or node.type == "non_null_expression"):
			self.transparent_outer: Ancestors|None = outer.transparent_outer
			self.transparent_receiver: Node|None = outer.transparent_receiver if outer.transparent_outer is not outer else node
		self.empty_lists: dict[Node, bool] = outer.empty_lists if outer is not None else {}
		self.node: Node|None = node
		self.outer: Ancestors|None = outer
		self.depth: int = (len(outer) if outer else 0) + bool(node)
		self.named: list[Node]|None = named
		self.is_directive_ready: bool = False
		self.directive_boundary: Node|None = None
		self.is_in_macro_tokens: bool = bool(outer and outer.is_in_macro_tokens or node and node.type in RUST_TOKEN_TREES)
		self.html_namespace: str = outer.html_namespace if outer is not None else "html"
		if node is not None and node.type == "element" and node.children:
			start: Node = node.children[0]
			tag: str = next((child.text.lower() for child in start.children if child.type == "tag_name"), "")
			if self.html_namespace == "math_text":
				self.html_namespace: str = "math" if tag in ("malignmark", "mglyph") else "html"
			if self.html_namespace == "html" and tag in ("math", "svg"):
				self.html_namespace: str = tag
			if self.html_namespace == "svg" and tag in ("desc", "foreignobject", "title"):
				self.html_namespace: str = "html"
			elif self.html_namespace == "math" and tag in ("mi", "mn", "mo", "ms", "mtext"):
				self.html_namespace: str = "math_text"
			elif self.html_namespace == "math" and tag == "annotation-xml":
				for attribute in ATTRIBUTE.finditer(start.text[len(tag) + 1:-1]):
					if (
						attribute.group(1).lower() == "encoding" and
						unescape(next((value for value in attribute.groups()[1:] if value is not None), "")).lower() in
						("application/xhtml+xml", "text/html")
					):
						self.html_namespace: str = "html"
						break
		# 終了
		return

def preprocessor_paths(node: Node, conditional: bool, inactive: bool) -> list[tuple[Node, bool, bool]]:
	"""整数定数の条件だけを確定し，枝毎の前処理の状態を子へ渡す
	計算量：直接の子数 N と条件の文字数 B に対して O(N+B)
	引数：node: 前処理の枝又は通常の構文，conditional: 外側の未確定条件，inactive: 外側の不成立の枝
	戻値：子と，未確定・不成立の状態の列
	"""
	if node.type not in ("preproc_elif", "preproc_elifdef", "preproc_if", "preproc_ifdef"):
		# 通常の構文と else は，外側の状態を其の儘引き継ぐ
		return [(child, conditional, inactive) for child in node.children]
	condition: Node|None = next((child for child in node.children if child.field == "condition"), None)
	value: str = condition.text.strip() if condition is not None else ""
	known: bool = bool(re.fullmatch(r"(?:0[xX][0-9a-fA-F]+|0[0-7]*|[1-9][0-9]*)", value))
	selected: bool = known and any(char != "0" for char in (value[2:] if value.startswith(("0X", "0x")) else value))
	# 確定した枝だけを有効とし，未知の条件は何方の枝も未確定として渡す
	return [
		(
			child,
			conditional or not known,
			inactive or known and (selected if child.type in ("preproc_elif", "preproc_elifdef", "preproc_else") else not selected)
		) for child in node.children
	]

def macro_paren_nodes(ancestors: Ancestors, lang: str) -> set[Node]:
	"""原文の前処理・束縛・実引数の構造から，括弧の端を意味比較へ残すノードを投影する
	重なるラッパーの外層だけを比較し，実引数の文字列化は独立した観測検査へ任せる
	計算量：ノード数 N・履歴数 M・本文長 B・束縛の照会深さ D・別名の総照会数 Q に対して O(B+ND+(N+Q+M)log(M+1))
	引数：
		ancestors: 同じ原文の木と意味の要約を持つ，空でない文脈
		lang: C 系の言語キー
	戻値：非マクロの事実が無い単独名，又はマクロの結合・型引数との区別を保護する最外のラッパー
	送出例外：
		ValueError: 原文の木を持たない所属を渡した場合
		IndexError: 空の文脈を渡した場合
		RuntimeError: 祖先の親リンクが宣言した深さと矛盾する場合
	"""
	facts: TreeFacts = ancestors.get_facts()
	root: Node = ancestors[-len(ancestors)]
	definitions: dict[str, list[tuple[int, str, bool|None]]] = {}
	includes: list[int] = []
	parents: dict[Node, Node|None] = {}
	scopes: dict[Node, Node] = {}
	outer_scopes: dict[Node, Node|None] = { root: None }
	bindings: dict[Node, dict[str, int]] = { root: {} }
	scalar_bindings: dict[Node, set[str]] = { root: set() }
	ordered: list[Node] = []
	pending: list[tuple[Node, Node|None, Node, bool, bool]] = [(root, None, root, False, False)]
	while pending:
		part, parent, scope, conditional, inactive = pending.pop()
		parents[part] = parent
		if part is not root and part.type in (
			"compound_statement",
			"field_declaration_list",
			"for_range_loop",
			"for_statement",
			"function_definition",
			"lambda_expression",
			"namespace_definition",
			"template_declaration"
		):
			outer_scopes[part] = scope
			bindings[part] = {}
			scalar_bindings[part] = set()
			scope = part
		scopes[part] = scope
		ordered.append(part)
		if not inactive and part.type == "preproc_include":
			includes.append(part.start)
		if not inactive and part.type in ("preproc_call", "preproc_def", "preproc_function_def"):
			match: re.Match[str]|None = re.match(r"#\s*(?:define|undef)\s+([A-Za-z_]\w*)", cpp_directive_source(cpp_source(part.text).text))
			if match is not None:
				body: str = ""
				if not conditional and part.type == "preproc_def":
					value: Node|None = next((item for item in part.children if item.type == "preproc_arg"), None)
					if value is not None:
						replacement: list[CppToken] = [item for item in cpp_tokens(value.text) if item.kind not in ("comment", "space")]
						if len(replacement) == 1 and replacement[0].kind in ("char", "identifier", "number", "string"):
							body = replacement[0].text
				definitions.setdefault(match[1], []).append((part.start, body, None if conditional else part.type == "preproc_function_def"))
		if not inactive and not conditional and part.type in (
			"alias_declaration",
			"declaration",
			"enumerator",
			"field_declaration",
			"function_definition",
			"optional_parameter_declaration",
			"parameter_declaration",
			"type_definition"
		):
			for child in part.children:
				if child.field not in ("declarator", "name"):
					continue
				decl: Node|None = child
				while decl is not None and decl.type not in ("field_identifier", "identifier", "type_identifier"):
					decl = next(
						(item for item in decl.children if item.field == "declarator" or item.kind == "N" and is_paren_wrapper(decl.type)),
						None
					)
				if decl is not None:
					owner: Node = outer_scopes[scope] or scope if part.type == "function_definition" else scope
					bindings[owner].setdefault(decl.text, decl.start)
					if child is decl and any(item.field == "type" and item.type == "primitive_type" for item in part.children):
						scalar_bindings[owner].add(decl.text)
		pending.extend(
			(child, part, scope, branch, dead) for child, branch, dead in reversed(preprocessor_paths(part, conditional, inactive))
		)
	includes.sort()
	for events in definitions.values():
		events.sort()
	# 名前毎の履歴を既存の共有履歴へ１度投影する
	active_macros(ancestors, root, set())
	possible: dict[Node, bool] = {}
	macro_calls: dict[Node, bool] = {}
	nonmacro: dict[Node, bool] = {}
	scalar_prefix: dict[Node, bool] = {}
	atomic: dict[str, tuple[int, int, bool, bool]] = {}
	for part in ordered:
		if part.type not in ("field_identifier", "identifier", "qualified_identifier", "type_identifier"):
			continue
		name: str = part.text
		scope: Node|None = scopes[part]
		is_bound: bool = False
		is_scalar: bool = False
		while scope is not None:
			position: int|None = bindings[scope].get(name)
			if position is not None and position <= part.start:
				is_bound = True
				is_scalar = name in scalar_bindings[scope]
				break
			scope = outer_scopes[scope]
		declared_macro: bool = bool(active_macros(ancestors, part, { name }))
		macro: bool = declared_macro
		scalar_prefix[part] = is_scalar and not declared_macro
		include_position: int = bisect_right(includes, part.start)
		last_include: int = includes[include_position - 1] if include_position else -1
		if include_position:
			# 未読ヘッダの後は，当該名の確定した取消が在る時だけ未定義とする
			events: list[tuple[int, bool]] = facts.macro_events.get(name, []) if facts.macro_events is not None else []
			position = bisect_right(events, (part.start, True)) - 1
			macro |= position < 0 or events[position][0] < last_include or events[position][1]
		# 定義済の別名が最終的に１つのリテラルへ展開する時は，式の結合を変えない
		alias: str = name
		seen: set[str] = set()
		path: list[tuple[str, int]] = []
		is_atomic: bool = False
		is_macro_call: bool = False
		until: int = includes[include_position] if include_position < len(includes) else root.end + 1
		while macro and alias not in seen:
			seen.add(alias)
			cached: tuple[int, int, bool, bool]|None = atomic.get(alias)
			if cached is not None and cached[0] <= part.start < cached[1]:
				until = min(until, cached[1])
				is_atomic = cached[2]
				is_macro_call = cached[3]
				break
			candidates: list[tuple[int, str, bool|None]] = definitions.get(alias, [])
			selected: int = bisect_right(candidates, (part.start, chr(0X10FFFF))) - 1
			next_event: int = candidates[selected + 1][0] if selected + 1 < len(candidates) else root.end + 1
			path.append((alias, next_event))
			if selected < 0:
				is_macro_call = last_include > -1
				break
			definition: int
			replacement_text: str
			function_like: bool|None
			definition, replacement_text, function_like = candidates[selected]
			if definition < last_include or function_like is not False:
				is_macro_call = True
				break
			if not replacement_text:
				break
			if not re.fullmatch(r"[A-Za-z_]\w*", replacement_text):
				is_atomic = True
				break
			alias = replacement_text
		else:
			if macro:
				is_macro_call = True
				cycle_start: int = next(idx for idx, (item, _) in enumerate(path) if item == alias)
				until = min(until, min(next_event for _, next_event in path[cycle_start:]))
		# 別名の依存先の次の変更迄だけ共有し，無関係な定義で再走査しない
		for alias, next_event in reversed(path):
			until = min(until, next_event)
			atomic[alias] = part.start, until, is_atomic, is_macro_call
		possible[part] = macro and not is_atomic
		macro_calls[part] = is_macro_call
		nonmacro[part] = is_bound and not macro
	# 式全体の展開と，呼出先の末尾のトークンの状態を別々に集約
	for part in reversed(ordered):
		possible[part] = possible.get(part, False) or any(possible[child] for child in part.children)
		if part.type == "unary_expression" and len(facts.named[part]) == 1:
			scalar_prefix[part] = bool(
				any(child.field == "operator" and child.text == "+" for child in part.children) and
				scalar_prefix.get(facts.named[part][0], False)
			)
		last: Node|None = next((child for child in reversed(part.children) if not is_comment(child.type)), None)
		macro_calls[part] = macro_calls[last] if last is not None else macro_calls.get(part, False)
	found: set[Node] = set()
	for part in ordered:
		if part.type != "parenthesized_expression" or len(facts.named[part]) != 1:
			continue
		parent: Node|None = parents[part]
		if parent is None or is_paren_wrapper(parent.type):
			continue
		inner: Node|None = facts.inner[part]
		if (
			inner is None or parent.type == "unary_expression" and inner.type == "unary_expression" and scalar_prefix.get(inner, False) and
			any(child.field == "operator" and child.text == "+" for child in parent.children)
		):
			# 宣言したスカラーの前置の正符号は，密着を避けるラッパーの有無で評価の木を変えない
			continue
		if inner.type == "identifier" and not nonmacro.get(inner, False) or possible[inner] and not (
			parent.type in ("co_return_statement", "expression_statement", "return_statement") or
			parent.type == "for_statement" and part.field == "update" or parent.type == "init_declarator" and part.field == "value"
		):
			# 値全体の返戻・捨てる文・反復の更新では，括弧を越える結合先が無い
			found.add(part)
		if parent.type == "argument_list":
			call: Node|None = parents[parent]
			if call is not None and call.type == "call_expression" and facts.named[call]:
				target: Node = facts.named[call][0]
				if macro_calls[target]:
					found.add(part)
	if lang in ("cpp", "h"):
		found.update(comparison_paren_nodes(facts, parents, ordered))
	# 原文の束縛・前処理から確定した保護対象の返戻
	return found

def comparison_paren_nodes(facts: TreeFacts, parents: dict[Node, Node|None], ordered: list[Node]) -> set[Node]:
	"""実引数の比較が，後続の閉じと型引数の並びへ結合し得る括弧を求める
	必須の丸括弧を持つ呼出の中の比較は外の閉じへ数えず，実引数列毎に右から１度だけ読む
	計算量：ノード数 N に対して O(N)
	引数：
		facts: 原文の子とラッパーの共有要約
		parents: 原文の直接の親
		ordered: 原文順のノード
	戻値：型引数の開きへ結合し得る比較の最外のラッパー
	"""
	found: set[Node] = set()
	for args in ordered:
		call: Node|None = parents[args]
		if args.type != "argument_list" or call is None or call.type != "call_expression":
			continue
		has_close: bool = False
		for part in reversed(facts.named[args]):
			inner: Node|None = facts.inner[part]
			if inner is None or inner.type != "binary_expression":
				continue
			operator: Node|None = next((child for child in inner.children if child.field == "operator"), None)
			if operator is None:
				continue
			if operator.text in (">", ">>"):
				has_close = True
			elif has_close and operator.text == "<" and part.type == "parenthesized_expression":
				left: Node|None = next((child for child in inner.children if child.field == "left"), None)
				while left is not None and left.children:
					left = next((child for child in reversed(left.children) if not is_comment(child.type)), None)
				if left is not None and left.type in ("field_identifier", "identifier", "type_identifier"):
					found.add(part)
	# 同じ実引数列の開きの候補だけを残し，通常の比較の閉じを意味のマーカーへ数えない結果の返戻
	return found

def has_semantic_parens(ancestors: Ancestors, node: Node, lang: str) -> bool:
	"""マクロ・型・ディレクティブ・短絡の意味を変える最外のラッパーかを返す
	重なったラッパーの数でなく，裸の名前と括弧付の式の区別を保持する
	計算量：初回のマクロ投影の費用 P・参照する構文のノード数 N・祖先の深さ D に対して O(P+N+D)
	引数：
		ancestors: 直近の祖先から外側へ辿るリンク
		node: 単一要素を包むノード
		lang: 言語キー
	戻値：
		意味の比較へラッパーのマーカーを残すなら真
	送出例外：RuntimeError: ディレクティブを持つコンテナの親リンクが欠ける場合
	"""
	if not ancestors:
		# 外側の構文が無い事の返戻
		return False
	facts: TreeFacts = ancestors.get_facts()
	parent: Node = ancestors[-1]
	if lang in ("c", "cpp", "h"):
		if facts.macro_parens is None:
			facts.macro_parens: set[Node]|None = macro_paren_nodes(ancestors, lang)
		if node in facts.macro_parens:
			# マクロの展開・呼出抑止・型引数との区別を保持したラッパーの返戻
			return True
	if lang in ("typescript", "tsx") and node.type == "parenthesized_type":
		inner: Node|None = facts.inner[node]
		# 同種の交差型も，明示したグループ毎の簡約で型が変わり得る
		# 重なる括弧の外層だけを残し，型全体又は原子型のラッパーとは区別する
		return parent.type == "intersection_type" and inner is not None and inner.type == parent.type
	postfix: tuple[str, ...] = "call_expression", "member_expression", "subscript_expression"
	if lang == "cpp":
		if parent.type not in ("decltype", "return_statement") and node not in facts.decl_initial:
			# 型の取得規則へ作用しない文脈では内側を走査しない
			return False
	elif lang == "python":
		is_target: bool = parent.type == "assignment" and facts.named[parent][0] is node
		if not is_target or not any(child.type == ":" for child in parent.children):
			# アノテーション付代入の対象以外では内側を走査しない
			return False
	elif lang in ARRAY_LANGS:
		# `!` は実行時に受手を変えない為，其の外側の後置演算迄を見る
		receiver: Node = node
		outer: Ancestors|None = ancestors
		if parent.type == "non_null_expression":
			receiver = ancestors.transparent_receiver
			outer = ancestors.transparent_outer
			if not outer:
				# 意味の区別を追加しない事の返戻
				return False
			parent = outer[-1]
		if parent.type != "expression_statement" and (
			parent.type not in postfix or facts.named[parent][0] is not receiver or
			any(child.type in ("?.", "optional_chain") for child in parent.children)
		):
			# 文又は短絡を継続する受手以外では内側を走査しない
			return False
	else:
		# 意味を持つラッパーを比較する言語の対象外である事の返戻
		return False
	inner: Node|None = facts.inner[node]
	if inner is None:
		# 意味の区別を追加しない事の返戻
		return False
	if lang == "python":
		# アノテーション付代入の裸の名前との区別の返戻
		return inner.type == "identifier"
	if lang in ARRAY_LANGS:
		if parent.type == "expression_statement":
			if inner.type != "string" or len(ancestors) < 2:
				# 文字列だけの文でない事の返戻
				return False
			container: Node = ancestors[-2]
			if (
				container.type != "program" and
				not (container.type == "statement_block" and len(ancestors) > 2 and ancestors[-3].type in DIRECTIVE_FUNCTIONS)
			):
				# ファイル又は関数の本体でない事の返戻
				return False
			if ancestors.outer is None:
				raise RuntimeError("directive container has no ancestor link")
			# 子一覧と境界を共有するコンテナのリンクから，ディレクティブに為り得る位置かの返戻
			return ancestors.outer.get_directive_boundary() is parent
		# 実引数・添字の中へ入らず，受手の連続に在る省略可能チェーンだけを調べる
		# 受手の連続に属する短絡の要約だけを読む
		return facts.optional[inner]
	if inner.type not in ("field_expression", "identifier", "qualified_identifier", "template_function"):
		# 括弧の有無で型の取得規則が変わらない式の返戻
		return False
	if parent.type == "decltype" or node in facts.decl_initial:
		# 型の取得と初期値の値・参照の区別の返戻
		return True
	# 最内の関数の見出だけから投影した返戻型を読む
	return ancestors.is_decltype_owner

def normalize_children(nodes: list[Node], lang: str, ancestors: Ancestors) -> Work:
	"""子の要求を原文の順に作り，正規形の並びを連結する
	計算量：子の数 N と結果のノード数 R に対して O(N+R)
	引数：
		nodes: 対象の子の並び
		lang: 言語キー
		ancestors: 外側から並べた祖先ノード
	戻値：
		子の正規形の並び
	"""
	# 子の結果の収集
	result: list[NormNode] = []
	for child in nodes:
		result.extend((yield normalize_work(child, lang, ancestors)))
	# 子の正規形の返戻
	return result

def control_header_parts(node: Node) -> tuple[tuple[str, ...], list[list[Node]], list[Node], list[Node]]|None:
	"""繰返と Go の選択の見出を，空の位置も保持して役割毎に分ける
	見出の直下又は見出の文・宣言を終えるセミコロンだけを区切りとする
	計算量：直下の子と其の直下の子の総数 N に対して O(N)
	引数：
		node: 制御文又は見出のノード
	戻値：
		役割・部分・見出の前・見出の後の並び，対象外の見出なら `None`
	"""
	# 評価順に意味が在る為，初期化・条件・更新の順を保つ
	roles: tuple[str, ...] = "initializer", "condition", "update"
	if node.type == "for_clause":
		start: int = 0
		end: int = len(node.children)
	elif node.type == "for_statement":
		start = next((idx + 1 for idx, child in enumerate(node.children) if child.kind == "U" and child.text == "("), -1)
		end = next((idx for idx, child in enumerate(node.children) if child.kind == "U" and child.text == ")"), -1)
		if start < 0 or end < start:
			# ３つの位置を括弧で囲まない見出の返戻
			return None
	elif node.type == "expression_switch_statement":
		start = 1
		end = next((idx for idx, child in enumerate(node.children) if child.kind == "U" and child.text == "{"), -1)
		if end < start:
			# 選択本体の境界を持たない形の返戻
			return None
		# 選択対象の省略も役割として保持し，初期化へ移した式と区別する
		roles = "initializer", "selector"
	else:
		# 同期する見出以外の返戻
		return None
	parts: list[list[Node]] = [[]]
	for child in node.children[start:end]:
		if child.kind == "U" and child.text == ";":
			parts.append([])
		else:
			parts[-1].append(child)
			if child.kind == "N" and any(part.kind == "U" and part.text == ";" for part in child.children):
				parts.append([])
	if node.type == "expression_switch_statement" and len(parts) == 1:
		parts.insert(0, [])
	if len(parts) != len(roles):
		# 所定の位置の数と一致しない見出の返戻
		return None
	# 空の位置を含む，意味上の境界を保持した見出の返戻
	return roles, parts, node.children[:start], node.children[end:]

def active_macros(ancestors: Ancestors, node: Node, names: set[str]) -> set[str]:
	"""原文の位置で有効又は条件付のマクロ名を，共有する履歴から求める
	計算量：初回は木のノード数 N と履歴数 M に対して O(N+M log M)，照会は名前数 K に対して O(K log M)
	引数：ancestors: 原文の文脈，node: 照会位置，names: 照会する名前
	戻値：置換の可能性を除外出来ない名前
	"""
	facts: TreeFacts = ancestors.get_facts()
	if facts.macro_events is None:
		facts.macro_events: dict[str, list[tuple[int, bool]]] = {}
		pending: list[tuple[Node, bool, bool]] = [(ancestors[-len(ancestors)], False, False)]
		while pending:
			part, is_conditional, inactive = pending.pop()
			if not inactive and part.type in ("preproc_call", "preproc_def", "preproc_function_def"):
				match: re.Match[str]|None = re.match(r"#\s*(define|undef)\s+([A-Za-z_]\w*)", cpp_directive_source(cpp_source(part.text).text))
				if match is not None:
					facts.macro_events.setdefault(match[2], []).append((part.start, match[1] == "define" or is_conditional))
			pending.extend(preprocessor_paths(part, is_conditional, inactive))
		for events in facts.macro_events.values():
			events.sort()
	active: set[str] = set()
	for name in names:
		events: list[tuple[int, bool]] = facts.macro_events.get(name, [])
		position: int = bisect_right(events, (node.start, True)) - 1
		if position > -1 and events[position][1]:
			active.add(name)
	# 条件分岐の中の取消を確定としない履歴の返戻
	return active

def boolean_initializer(lang: str, ancestors: Ancestors, node: Node) -> str:
	"""組込の真偽値の直接の初期値だけを，利用可能な真偽値リテラルへ対応付ける
	計算量：直属の宣言・初期化子列の子数 C・祖先深度 D・先行ソース長 B と前処理の履歴照会費用 M に対して O(C+D+B+M)
	引数：lang: C 系の言語，ancestors: 初期化の文脈，node: 数値のノード
	戻値：対応するリテラル．型・名前の契約が未確定なら空
	"""
	if node.text not in ("0", "1") or not ancestors:
		# 真偽値の直接の初期化でない結果の返戻
		return ""
	# 初期化の値だけを外へ辿り，演算・呼出・指示先のインデックスを透過しない
	value: Node = node
	parents: Ancestors = ancestors
	lists: list[Node] = []
	while parents.node is not None:
		parent: Node = parents.node
		if parent.type in ("initializer_list", "parenthesized_expression"):
			if parent.type == "initializer_list":
				lists.append(parent)
		elif parent.type == "argument_list":
			if parents.outer is None or parents.outer.node is None or parents.outer.node.type != "init_declarator":
				# 通常の呼出の実引数の除外の返戻
				return ""
			lists.append(parent)
		elif parent.type == "initializer_pair" and value.field == "value":
			pass
		else:
			break
		value = parent
		if parents.outer is None:
			# 宣言の外へ達した結果の返戻
			return ""
		parents = parents.outer
	if parents.node is None:
		# 初期化先の宣言へ達しない結果の返戻
		return ""
	parent = parents.node
	declaration: Node|None = None
	declarator: Node|None = None
	if parent.type == "init_declarator" and value.field == "value" and parents.outer is not None:
		declaration = parents.outer.node
		declarator = next((child for child in parent.children if child.field == "declarator"), None)
	elif parent.type in ("field_declaration", "optional_parameter_declaration") and value.field == "default_value":
		declaration = parent
		for child in parent.children:
			if child is value:
				break
			if child.field == "declarator":
				declarator = child
	is_array: bool = False
	while declarator is not None and declarator.type in ("array_declarator", "parenthesized_declarator"):
		is_array |= declarator.type == "array_declarator"
		declarator = next(iter(Node.get_named_children(declarator)), None)
	if (
		declaration is None or
		declarator is not None and declarator.type not in ("field_identifier", "identifier", "qualified_identifier") or
		declarator is None and parent.type != "optional_parameter_declaration" or
		is_array and parent.type == "optional_parameter_declaration" or
		not is_array and any(len(Node.get_named_children(part)) != 1 for part in lists)
	):
		# 実効型が真偽値と確定する初期化でない結果の返戻
		return ""
	type_name: str = next((child.text for child in declaration.children if child.field == "type"), "")
	if type_name not in ("_Bool", "bool"):
		# 真偽値型でない結果の返戻
		return ""
	literal: str = "true" if node.text == "1" else "false"
	if lang == "c":
		root: Node = ancestors[-len(ancestors)]
		prefix: str = cpp_source(root.text[:node.start]).text
		known: dict[str, str] = {}
		included: bool = False
		depth: int = 0
		for line in prefix.splitlines():
			match: re.Match[str]|None = re.match(r"\s*#\s*(\w+)\s*(.*)", line)
			if match is None:
				continue
			command, body = match.groups()
			if command in ("if", "ifdef", "ifndef"):
				depth += 1
			elif command == "endif":
				depth = max(0, depth - 1)
			elif command == "include" and body.strip() == "<stdbool.h>" and not depth and not included:
				known.update({ "bool": "_Bool", "false": "0", "true": "1" })
				included = True
			elif command in ("include", "include_next", "import") and body.strip() != "<stdbool.h>":
				known.clear()
			elif command in ("define", "undef"):
				parts: list[str] = body.split(None, 1)
				if parts:
					known[parts[0]] = parts[1].strip() if command == "define" and not depth and len(parts) == 2 else ""
		if known.get(literal) != node.text or type_name == "bool" and known.get("bool") != "_Bool":
			# 標準の値を証明出来ない結果の返戻
			return ""
	elif active_macros(ancestors, node, { type_name, literal }):
		# マクロの置換を持つ対象の見送の返戻
		return ""
	# 値・直接の宣言型・リテラルの可用性が一致した結果の返戻
	return literal

def builtin_update_references(function: Node) -> frozenset[Node]:
	"""宣言の有効位置とスコープを辿り，組込型へ解決する増減の参照を集める
	計算量：関数のノード数 N と共有範囲の名前数 S に対して O(NS)
	引数：function: 名前付関数
	戻値：組込の増減を選ぶ対象の参照
	"""
	found: set[Node] = set()
	pending: list[tuple[Node, dict[str, bool], bool|None]] = [(function, {}, None)]
	while pending:
		part, scope, declared = pending.pop()
		if part is not function and part.type in ("function_definition", "lambda_expression"):
			continue
		if part.type in (
			"catch_clause",
			"compound_statement",
			"for_range_loop",
			"for_statement",
			"if_statement",
			"switch_statement",
			"while_statement"
		):
			scope: dict[str, bool] = dict(scope)
		if declared is not None:
			name: Node|None = part
			is_builtin: bool = declared or part.type == "pointer_declarator"
			while name is not None and name.type != "identifier":
				if name.type == "structured_binding_declarator":
					for item in name.children:
						if item.type == "identifier":
							scope[item.text] = False
					break
				name = next(
					(
						child for child in name.children if child.field == "declarator" or
						child.kind == "N" and (is_paren_wrapper(name.type) or name.type == "reference_declarator")
					),
					None
				)
			if name is not None and name.type == "identifier":
				scope[name.text] = is_builtin
		if part.type == "using_declaration" and not any(child.type in ("enum", "namespace") for child in part.children):
			for imported in Node.get_named_children(part):
				if imported.type == "qualified_identifier":
					name = next((child for child in imported.children if child.field == "name"), None)
					if name is not None and name.type == "identifier":
						scope[name.text] = False
		if part.type == "update_expression":
			for name in Node.get_named_children(part):
				while is_paren_wrapper(name.type) and len(Node.get_named_children(name)) == 1:
					name = Node.get_named_children(name)[0]
				if name.type == "identifier" and scope.get(name.text, False):
					found.add(name)
		is_declaration: bool = part.type in ("declaration", "for_range_loop", "optional_parameter_declaration", "parameter_declaration")
		is_builtin = any(child.type in ("primitive_type", "sized_type_specifier") for child in part.children)
		pending.extend(
			(child, scope, is_builtin if is_declaration and child.field == "declarator" else None) for child in reversed(part.children)
		)
	# 入子の束縛を外側へ漏らさない参照の返戻
	return frozenset(found)

def kotlin_discards_value(ancestors: Ancestors, node: Node) -> bool:
	"""枝と末尾式の値を外側へ辿り，値の破棄を確認する
	計算量：祖先の深さ D と其の直下の子の総数 N に対して O(D+N)
	引数：ancestors: 外側の構文，node: 値の使用を調べる式
	戻値：値を捨てる文脈へ達した場合は真
	"""
	outer: Node = node
	for parent in reversed(ancestors):
		if parent.type == "statements":
			statements: list[Node] = Node.get_named_children(parent)
			if statements and outer is not statements[-1]:
				# 末尾以外の文の値は本体の値に為らない
				return True
		elif parent.type == "function_body":
			# 式本体の返戻値を，波括弧付の本体の末尾文と区別する
			return any(child.type == "{" for child in parent.children)
		elif parent.type in ("do_while_statement", "for_statement", "while_statement"):
			# 条件と反復対象の値は使い，本体の値だけを捨てる
			return outer.type == "control_structure_body"
		elif not (
			is_paren_wrapper(parent.type) or parent.type == "control_structure_body" or
			parent.type in ("if_expression", "when_entry") and outer.type == "control_structure_body" or
			parent.type == "when_expression" and outer.type == "when_entry"
		):
			# 初期化子・実引数・返戻・ラムダ等では，値の破棄を推測しない
			return False
		outer = parent
	# 値を捨てる文脈が見付からない結果の返戻
	return False

def discarded_update(lang: str, ancestors: Ancestors, node: Node) -> bool:
	"""増減の値を捨てる文脈と，前後の形式が同じ更新を行う対象を確認する
	計算量：対象の所属する関数のノード数 N に対して O(N)
	引数：
		lang: 対象言語
		ancestors: 外側の構文と共有事実
		node: 増減のノード
	戻値：前置・後置の選択だけを比較から除ける場合は真
	"""
	if (
		node.type not in
		("postfix_expression", "postfix_unary_expression", "prefix_expression", "prefix_unary_expression", "update_expression") or
		not ancestors
	):
		# 更新の等価性を確定出来ない結果の返戻
		return False
	if lang == "kotlin":
		# 同じ増減演算の値を捨てる場合だけ，前後の形式を揃える
		return any(child.type in ("++", "--") for child in node.children) and kotlin_discards_value(ancestors, node)
	outer: Node = node
	parent: Node|None = None
	for candidate in reversed(ancestors):
		if not is_paren_wrapper(candidate.type):
			parent = candidate
			break
		outer = candidate
	if (
		parent is None or
		not (parent.type == "expression_statement" or parent.type == "for_statement" and outer.field in ("increment", "update"))
	):
		# 更新の等価性を確定出来ない結果の返戻
		return False
	if lang in ("csharp", "java", "javascript", "php", "typescript", "tsx"):
		# 多重定義の前置・後置を別々に選ばない言語の返戻
		return True
	if lang not in ("c", "cpp"):
		# 更新の等価性を確定出来ない結果の返戻
		return False
	named: list[Node] = Node.get_named_children(node)
	if len(named) != 1:
		# 更新の等価性を確定出来ない結果の返戻
		return False
	facts: TreeFacts = ancestors.get_facts()
	macro_names: set[str] = (
		set(re.findall(r"[A-Za-z_]\w*", node.text)) |
		{ "char", "char16_t", "char32_t", "double", "float", "int", "long", "short", "signed", "unsigned", "wchar_t" }
	)
	if active_macros(ancestors, node, macro_names):
		# 対象名又は組込型の置換を，宣言型だけで打ち消さない
		return False
	if lang == "c":
		# 組込の更新の返戻
		return True
	while len(named) == 1 and is_paren_wrapper(named[0].type):
		named = Node.get_named_children(named[0])
	if len(named) != 1 or named[0].type != "identifier":
		# 更新の等価性を確定出来ない結果の返戻
		return False
	function: Node|None = (
		next((part for part in reversed(ancestors) if part.type in ("function_definition", "lambda_expression")), None)
	)
	if function is None or function.type == "lambda_expression":
		# 更新の等価性を確定出来ない結果の返戻
		return False
	if function not in facts.update_names:
		facts.update_names[function] = builtin_update_references(function)
	# 関数毎に１度確定した対象の返戻
	return named[0] in facts.update_names[function]

def jsx_text_value(text: str) -> str:
	"""JSX の本文の行境界と文字参照を，表示される文字列へ対応付ける
	計算量：本文長 N に対して O(N)
	引数：text: 式又は要素の境界で区切った本文
	戻値：行端の ASCII 空白だけを除いた本文
	"""
	lines: list[str] = re.split(r"\r\n|[\r\n]", text)
	parts: list[str] = []
	line: str
	for idx, line in enumerate(lines):
		if idx:
			line = line.lstrip(" \t")
		if idx + 1 < len(lines):
			line = line.rstrip(" \t")
		if line:
			parts.append(line)
	# 文字参照の空白を行端のインデントへ読み替えない値の返戻
	return unescape(" ".join(parts))

def jsx_children(lang: str, ancestors: Ancestors, node: Node) -> Work:
	"""JSX の式・要素・本文を分け，子の値と個数を保持する
	計算量：直下の本文長 L と子の正規化費用 C に対して O(L+C)
	引数：
		lang: 対象言語
		ancestors: 外側の意味の事実
		node: 開閉要素を持つ JSX
	戻値：子の評価順と要素境界を保持した正規形
	"""
	children: list[NormNode] = []
	position: int = node.children[0].end
	opening: Node = node.children[0]
	children.extend((yield normalize_work(opening, lang, ancestors)))
	for child in node.children[1:]:
		if child.type in ("html_character_reference", "jsx_text"):
			continue
		if text := jsx_text_value(node.source[position:child.start]):
			children.append(("jsx_text_value", text, ()))
		position = child.end
		children.extend((yield normalize_work(child, lang, ancestors)))
	# 開閉タグも保持する JSX の返戻
	return [(node.type, None, tuple(children))]

def quoted_value(lang: str, node: Node) -> str|None:
	"""通常の引用文字列を，区切とエスケープから値へデコードする
	補間・正規表現・未知のエスケープは此の経路で解釈しない
	計算量：本文長 N に対して O(N)
	引数：
		lang: 対象言語
		node: 文字列候補のノード
	戻値：型の区分を含む値．対象外又は未確認なら None
	"""
	kinds: tuple[str, ...] = "encapsed_string", "interpreted_string_literal", "string", "string_literal"
	if node.type not in kinds:
		# 文字列でないノードの返戻
		return None
	text: str = node.text
	if lang == "python":
		try:
			value: object = ast.literal_eval(text)
		except (SyntaxError, ValueError):
			# 補間や未対応の構文の返戻
			return None
		# bytes と文字列を区別した値の返戻
		return repr(value) if isinstance(value, (str, bytes)) else None
	if (
		lang not in ("c", "cpp", "go", "javascript", "php", "ruby", "typescript", "tsx") or
		any(child.type in ("interpolation", "string_interpolation", "variable_name") for child in node.children) or
		lang == "php" and node.type == "encapsed_string" and any(child.type == "{" for child in node.children)
	):
		# 未登録の言語又は評価する式を持つ文字列の返戻
		return None
	prefix: str = ""
	if lang in ("c", "cpp"):
		match: re.Match[str]|None = re.match(r'(u8|u|U|L)?(".*")$', text, re.S)
		if match is None:
			# 通常の C 文字列でない場合の返戻
			return None
		prefix, text = match[1] or "", match[2]
	elif lang == "php" and text.startswith(("b'", "b\"", "B'", "B\"")):
		text = text[1:]
	if len(text) < 2 or text[0] not in "\"'" or text[-1] != text[0]:
		# 完結した引用文字列でない場合の返戻
		return None
	quote: str = text[0]
	body: str = text[1:-1]
	decoded: list[str] = []
	pos: int = 0
	while pos < len(body):
		char: str = body[pos]
		pos += 1
		if char != "\\":
			decoded.append(char)
			continue
		if pos == len(body):
			# 終端して居ないエスケープの返戻
			return None
		char = body[pos]
		pos += 1
		if quote == "'" and lang in ("php", "ruby"):
			decoded.append(char if char in "\\'" else "\\" + char)
		elif char in "\\\"" or char == "'" and lang != "php" or lang == "php" and char == "$" or lang == "ruby" and char == "#":
			decoded.append(char)
		elif char in "abefnrtv":
			if char in "ae" and lang not in ("c", "cpp", "ruby", "php") or lang == "php" and char in "ab":
				# 言語が値を定めないエスケープの返戻
				return None
			decoded.append(dict(zip("abefnrtv", "\a\b\x1b\f\n\r\t\v"))[char])
		elif char == "\n" and lang not in ("go", "php"):
			continue
		elif char == "\r" and pos < len(body) and body[pos] == "\n" and lang not in ("go", "php"):
			pos += 1
		elif (
			(char == "x" or char == "u" and lang != "php") and (width := 2 if char == "x" else 4) and
			re.fullmatch(r"[0-9a-fA-F]+", body[pos:pos + width]) and len(body[pos:pos + width]) == width
		):
			if lang in ("c", "cpp") and char == "x" and pos + width < len(body) and body[pos + width] in "0123456789abcdefABCDEF":
				# 桁を切り捨てず，未対応の長いエスケープの返戻
				return None
			codepoint: int = int(body[pos:pos + width], 16)
			if codepoint > 127 and lang in ("c", "cpp", "go", "php", "ruby"):
				# バイトと文字の対応を，実行時のエンコーディング無しで決めない
				# 言語が値を定めないエスケープの返戻
				return None
			decoded.append(chr(codepoint))
			pos += width
		else:
			# 不明な規則を値の一致へ読み替えない結果の返戻
			return None
	# 接頭辞に依る型を保つ値の返戻
	return prefix + repr("".join(decoded))

def go_true_bound(ancestors: Ancestors, node: Node) -> bool:
	"""導入の位置と有効範囲を１度収集し，Go の真の組込名の遮蔽を調べる
	計算量：初回はノード数 N に対して O(N)，以後の照会は O(1)
	引数：ancestors: 条件を囲む構文，node: 条件のノード
	戻値：組込名を遮蔽する導入が見える場合は真
	"""
	if not ancestors:
		# 宣言を持たない単独のリテラルの返戻
		return False
	facts: TreeFacts = ancestors.get_facts()
	if facts.go_true is None:
		facts.go_true: dict[Node, bool] = {}
		bindings: set[Node] = set()
		targets: set[Node] = set()
		for part in facts.named:
			facts.go_work += 1
			if part.type in ("range_clause", "receive_statement", "short_var_declaration", "type_switch_statement"):
				if not any(child.type == ":=" for child in part.children):
					continue
				for child in part.children:
					if child.field in ("alias", "left"):
						targets.update(name for name in child.children if name.type in ("identifier", "true") and name.text == "true")
						if any(name in targets for name in child.children):
							bindings.add(part)
			elif part.type in (
				"const_spec",
				"function_declaration",
				"import_spec",
				"parameter_declaration",
				"type_alias",
				"type_parameter_declaration",
				"type_spec",
				"var_spec",
				"variadic_parameter_declaration"
			):
				for child in part.children:
					if child.field == "name" and child.text == "true":
						targets.add(child)
						bindings.add(part)
		root: Node = ancestors[-len(ancestors)]
		header: list[Node] = list(root.children)
		global_true: bool = False
		while header:
			part: Node = header.pop()
			facts.go_work += 1
			global_true |= part in bindings
			if part.type in ("const_declaration", "import_declaration", "import_spec_list", "type_declaration", "var_declaration"):
				header.extend(part.children)
		pending: list[tuple[Node, list[bool], bool]] = [(root, [global_true], False)]
		while pending:
			entry: tuple[Node, list[bool], bool] = pending.pop()
			part: Node = entry[0]
			scope: list[bool] = entry[1]
			is_leave: bool = entry[2]
			facts.go_work += 1
			if is_leave:
				# 初期化子・反復対象・受信を評価した後だけ，後続へ導入を渡す
				scope[0] = True
				continue
			if part.type in (
				"block",
				"communication_case",
				"default_case",
				"expression_case",
				"expression_switch_statement",
				"for_statement",
				"func_literal",
				"function_declaration",
				"if_statement",
				"method_declaration",
				"type_case",
				"type_switch_statement"
			):
				scope = [scope[0]]
			if part.type == "true":
				facts.go_true[part] = scope[0] or part in targets
			if (
				part in bindings and
				part.type in ("const_spec", "range_clause", "receive_statement", "short_var_declaration", "type_alias", "type_spec", "var_spec")
			):
				pending.append((part, scope, True))
			parameters: bool = False
			if part.type in ("func_literal", "function_declaration", "method_declaration"):
				parameters = any(
					parameter in bindings for child in part.children if child.type in ("parameter_list", "type_parameter_list") for parameter in
					child.children
				)
			for child in reversed(part.children):
				child_scope: list[bool] = scope
				if (
					parameters and child.field == "body" or
					part.type == "type_switch_statement" and part in bindings and child.type in ("default_case", "type_case")
				):
					child_scope = [True]
				pending.append((child, child_scope, False))
	# 使用位置へ投影した遮蔽の返戻
	return facts.go_true.get(node, False)

def c_constant_true(lang: str, ancestors: Ancestors, node: Node) -> bool:
	"""使用位置の前処理状態から，真の名前が定数へ解決する事を確認する
	計算量：初回はノード数 N・ソース長 B に対して O(N+B)，以後の照会は O(1)
	引数：lang: C 系の言語，ancestors: 原文の文脈，node: 真の名前の参照
	戻値：副作用の無い定数真と確認出来る場合は真
	"""
	facts: TreeFacts = ancestors.get_facts()
	if facts.constant_true is None:
		facts.constant_true: set[Node] = set()
		root: Node = ancestors[-len(ancestors)] if ancestors else node
		# リテラルとコメントの内部を前処理行として読まない
		mask: str = "".join(
			re.sub(r"[^\r\n]", " " if token.kind == "comment" else "x", token.text) if
			token.kind in ("comment", "literal") else
			token.text for token in cpp_phase_tokens(root.source or root.text)
		)
		directives: Iterator[re.Match[str]] = iter(re.finditer(r"(?m)^[ \t]*#[ \t]*(\w+)[ \t]*([^\n]*)", mask))
		directive: re.Match[str]|None = next(directives, None)
		# C17 と共有ヘッダでは，名前の綴りだけをリテラルと看做さない
		state: tuple[bool|None, bool|None] = True if lang == "cpp" else None, False
		branches: list[tuple[tuple[bool|None, bool|None], tuple[bool|None, bool|None]|None, bool]] = []
		entry: tuple[bool|None, bool|None]
		joined: tuple[bool|None, bool|None]|None
		has_else: bool
		for part in facts.named:
			if part.type not in ("identifier", "true") or part.text != "true":
				continue
			while directive is not None and directive.start() < part.start:
				command, body = directive.groups()
				if command in ("if", "ifdef", "ifndef"):
					branches.append((state, None, False))
				elif command in ("elif", "else") and branches:
					entry, joined, _ = branches[-1]
					joined = (
						state if joined is None else (joined[0] if joined[0] == state[0] else None, joined[1] if joined[1] == state[1] else None)
					)
					branches[-1] = entry, joined, command == "else"
					state = entry
				elif command == "endif" and branches:
					entry, joined, has_else = branches.pop()
					if joined is not None:
						state = state[0] if state[0] == joined[0] else None, state[1] if state[1] == joined[1] else None
					if not has_else:
						state = state[0] if state[0] == entry[0] else None, state[1] if state[1] == entry[1] else None
				elif command in ("import", "include", "include_next"):
					if command == "include" and body.strip() == "<stdbool.h>":
						state = True if state[1] is False else state[0], True
					else:
						state = None, None
				elif command in ("define", "undef"):
					definition: re.Match[str]|None = re.match(r"true\b(.*)", body)
					if definition is not None:
						state = lang == "cpp" if command == "undef" else definition[1].strip() == "1", state[1]
				directive = next(directives, None)
			if state[0] is True:
				facts.constant_true.add(part)
	# 使用位置で確定した定数の返戻
	return node in facts.constant_true

def loop_form(lang: str, node: NormNode) -> NormNode:
	"""副作用の無い定数真の条件だけを，初期化・更新の無い反復へ揃える
	計算量：見出の子数 N に対して O(N)
	引数：
		lang: 対象言語
		node: 正規化した反復又は其の他のノード
	戻値：本体・脱出先・else 節を保持した反復
	"""
	kind: str
	text: str|None
	children: tuple[NormNode, ...]
	kind, text, children = node
	if lang == "rust" and kind in ("loop_expression", "while_expression"):
		prefix: tuple[NormNode, ...] = children[:2] if children and children[0][0] == "label" else ()
		body: tuple[NormNode, ...] = children[len(prefix):]
		if (
			body and body[0][0] == "loop" or
			len(body) > 2 and body[0][0] == "while" and body[1] == ("boolean_literal", None, (("true", "true", ()),))
		):
			# ラベルと本体を残し，組込の真の条件だけを省く
			return "infinite_loop", None, prefix + body[-1:]
	if lang == "ruby" and kind in ("while", "while_modifier"):
		position: int = 1 if kind == "while" else 2
		if len(children) > position and children[position] == ("integer", "1", ()):
			children = children[:position] + (("true", "true", ()),) + children[position + 1:]
		# 後置形との照合の為，反復の種類と本体を保持する
		return kind, text, children
	if kind in ("for_statement", "for_expression") and len(children) > 3:
		if children[1:4] == (("initializer", None, ()), ("condition", None, ()), ("update", None, ())):
			# 見出が空の反復の返戻
			return "infinite_loop", None, children[4:]
	if kind in ("while", "while_statement", "while_expression") and len(children) > 1:
		condition: NormNode = children[1]
		while condition[0] in ("boolean_literal", "condition_clause") and len(condition[2]) == 1:
			condition = condition[2][0]
		if (
			condition[0] in ("boolean", "true") and condition[1] in ("True", "true") or
			lang not in ("csharp", "java", "go", "rust", "kotlin", "swift") and condition[0] in ("integer", "number", "number_literal") and
			condition[1] == "1"
		):
			# 定数の真偽だけを省いた反復の返戻
			return "infinite_loop", None, tuple(child for child in children[2:] if child[0] != ":")
	if (
		lang == "go" and kind == "for_statement" and len(children) == 3 and children[1] in
		(("true", "true", ()), ("for_clause", None, (("initializer", None, ()), ("condition", None, ()), ("update", None, ()))))
	):
		# 明示した真の条件も，専用の無条件反復と同じ本体を持つ
		return "infinite_loop", None, children[2:]
	if lang == "go" and kind == "for_statement" and len(children) == 2 and children[1][0] == "block":
		# Go の専用の無条件反復の返戻
		return "infinite_loop", None, children[1:]
	# 其の他の見出と評価を保持するノードの返戻
	return node

def numeric_spelling(text: str) -> str:
	"""値・接尾辞・基数を保持し，小数点に隣接する省略済の０だけを補う
	計算量：テキスト長 N に対して O(N)
	引数：text: 数値トークンの綴り
	戻値：字形と小数点の０を揃えた綴り
	"""
	value: str = text.lower().replace("_", "")
	value = re.sub(r"^([+-]?(?:0x)?)\.", r"\g<1>0.", value)
	pattern: str = r"\.(?=(?:p[+-]?[0-9]+)?[dfl]*$)" if "0x" in value else r"\.(?=(?:e[+-]?[0-9]+)?[flmd]*$)"
	# 値と型の識別を残した綴りの返戻
	return re.sub(pattern, ".0", value)

def ruby_capture_names(node: Node) -> set[str]:
	"""静的な正規表現の捕捉を，エスケープ・文字クラス・コメント・拡張モードから区別する
	計算量：正規表現の長さ L に対して O(L)
	引数：node: 左辺の正規表現リテラル
	戻値：ローカル変数を導入する名前付捕捉の名前
	"""
	names: set[str] = set()
	if len(node.children) < 2 or any(child.type == "interpolation" for child in node.children):
		# 動的な正規表現はローカル変数を導入しない
		return names
	text: str = node.text[node.children[0].end - node.start:node.children[-1].start - node.start]
	modes: list[bool] = ["x" in node.children[-1].text[1:]]
	options: re.Pattern[str] = re.compile(r"\(\?([imx]*)(?:-([imx]+))?([:)])")
	capture: re.Pattern[str] = re.compile(r"\(\?(?:<([^\W\d]\w*)>|'([^\W\d]\w*)')")
	position: int = 0
	depth: int = 0
	while position < len(text):
		char: str = text[position]
		if char == "\\":
			position += 2
			continue
		if char == "[":
			depth += 1
			position += 2 if text.startswith("[^", position) else 1
			if text.startswith("]", position):
				position += 1
			continue
		if depth:
			if char == "]":
				depth -= 1
			position += 1
			continue
		if char == "#" and modes[-1]:
			newline: int = text.find("\n", position)
			position = len(text) if newline < 0 else newline + 1
			continue
		if text.startswith("(?#", position):
			position += 3
			while position < len(text) and text[position] != ")":
				position += 2 if text[position] == "\\" else 1
			position += 1
			continue
		if char == "(":
			mode: re.Match[str]|None = options.match(text, position)
			if mode is not None:
				extended: bool = (modes[-1] or "x" in mode[1]) and "x" not in (mode[2] or "")
				if mode[3] == ":":
					modes.append(extended)
				else:
					modes[-1] = extended
				position = mode.end()
				continue
			modes.append(modes[-1])
			match: re.Match[str]|None = capture.match(text, position)
			if match is not None:
				names.add(match[1] or match[2])
				position = match.end()
				continue
		elif char == ")" and len(modes) > 1:
			modes.pop()
		position += 1
	# 捕捉の構文として読んだ名前の返戻
	return names

def ruby_binding_before(ancestors: Ancestors, node: Node, name: str) -> bool:
	"""有効範囲と初出位置を１度記録し，対象より前の導入を照会する
	計算量：初回は木のノード数 N・正規表現長 B に対して O(N+B)，照会はブロックの深さ D に対して O(D)
	引数：ancestors: 対象の祖先，node: 原文上の対象，name: 照会する名前
	戻値：対象の有効範囲で先行する導入が在れば真
	"""
	facts: TreeFacts = ancestors.get_facts()
	if not facts.ruby_scopes:
		root: Node = ancestors[-len(ancestors)] if ancestors else node
		pending: list[tuple[Node, Node|None, bool]] = [(root, None, False)]
		while pending:
			part, scope, is_binding = pending.pop()
			facts.ruby_work += 1
			outer_scope: Node|None = scope
			if (
				scope is None or
				part.type in ("block", "class", "do_block", "lambda", "method", "module", "singleton_class", "singleton_method")
			):
				facts.ruby_parents[part] = None if part.type in ("class", "method", "module", "singleton_class", "singleton_method") else scope
				scope = part
				facts.ruby_bindings[scope] = {}
			facts.ruby_scopes[part] = scope
			bindings: dict[str, int] = facts.ruby_bindings[scope]
			if is_binding and part.type == "identifier":
				bindings.setdefault(part.text, part.start)
			if is_binding and part.type == "keyword_pattern" and not any(child.field == "value" for child in part.children):
				for child in part.children:
					if child.type == "hash_key_symbol":
						bindings.setdefault(child.text, child.start)
			if part.type == "binary" and any(child.type == "=~" for child in part.children):
				left: Node|None = next((child for child in part.children if child.field == "left"), None)
				if left is not None and left.type == "regex":
					facts.ruby_work += len(left.text)
					for capture_name in ruby_capture_names(left):
						bindings.setdefault(capture_name, part.start)
			for child in reversed(part.children):
				# 対象のパターンだけを辿り，受手・添字・既定値の参照を導入へ混ぜない
				binding: bool = (
					part.type in ("assignment", "operator_assignment") and child.field == "left" or
					part.type in ("for", "in_clause", "match_pattern", "test_pattern") and child.field == "pattern" or
					part.type in ("block_parameters", "exception_variable", "lambda_parameters", "method_parameters") or is_binding and (
						part.type in (
							"array_pattern",
							"as_pattern",
							"destructured_left_assignment",
							"destructured_parameter",
							"find_pattern",
							"hash_pattern",
							"left_assignment_list",
							"rest_assignment"
						) or part.type == "keyword_pattern" and child.field == "value" or
						part.type in ("block_parameter", "hash_splat_parameter", "keyword_parameter", "optional_parameter", "splat_parameter") and
						child.field == "name"
					)
				)
				# クラスの継承元と特異定義の受手は，定義の本体でなく外側で評価する
				child_scope: Node|None = (
					outer_scope if
					part.type in ("class", "singleton_class", "singleton_method") and child.field in ("object", "superclass", "value") else
					scope
				)
				pending.append((child, child_scope, binding))
	scope: Node|None = facts.ruby_scopes[node]
	while scope is not None:
		facts.ruby_work += 1
		if facts.ruby_bindings[scope].get(name, node.start) < node.start:
			# 初出位置を確認した導入の返戻
			return True
		scope = facts.ruby_parents[scope]
	# 対象より前の導入が無い結果の返戻
	return False

def ruby_modifier_equivalent(ancestors: Ancestors, node: Node) -> bool:
	"""後置への移動で条件内の名前の解釈が変わらない単一文を確認する
	計算量：条件と本体のノード数 N・共通名数 K・ブロックの深さ D に対して O(N+KD)，導入の初回記録を除く
	引数：ancestors: 外側の有効範囲，node: 前置の条件又は反復
	戻値：導入順と反復の開始を保持する場合は真
	"""
	condition: Node|None = next((child for child in node.children if child.field == "condition"), None)
	body: Node|None = next((child for child in node.children if child.field in ("body", "consequence")), None)
	if condition is None or body is None:
		# 条件又は本体の対応が無い結果の返戻
		return False
	introduced: set[str] = set()
	references: set[str] = set()
	pending: list[tuple[Node, bool]] = [(condition, True), (body, False)]
	while pending:
		part, is_condition = pending.pop()
		if part.type == "begin" or is_condition and "assignment" in part.type:
			# 評価順又は変数の導入順を変える候補の拒否
			return False
		if is_condition and part.type == "identifier":
			references.add(part.text)
		if not is_condition and "assignment" in part.type:
			for target in part.children:
				if target.field == "left":
					work: list[Node] = [target]
					while work:
						name: Node = work.pop()
						if name.type == "identifier":
							introduced.add(name.text)
						work.extend(name.children)
		if not is_condition and part.type == "regex":
			introduced.update(first or second for first, second in re.findall(r"\(\?<([^>]+)>|\(\?'([^']+)'", part.text))
		pending.extend((child, is_condition) for child in part.children)
	# 未導入の条件名を本体側の導入でローカル参照へ変えない
	return all(ruby_binding_before(ancestors, node, name) for name in introduced & references)

def csharp_parameter_name(ancestors: Ancestors, name: str) -> bool:
	"""最も内側のメソッドの仮引数として導入した値の名前を確認する
	計算量：祖先数 D と仮引数数 P に対して O(D+P)
	引数：ancestors: 対象の文脈，name: 括弧内の名前
	戻値：型でなく値の導入が確認出来る場合は真
	"""
	for parent in reversed(ancestors):
		if parent.type in ("lambda_expression", "anonymous_method_expression", "local_function_statement"):
			# 内側の関数の束縛を未確認とする結果の返戻
			return False
		if parent.type in ("constructor_declaration", "method_declaration"):
			# 直近の名前付関数の仮引数との照合結果の返戻
			return any(
				child.text == name and child.field == "name" for part in parent.children if part.type == "parameter_list" for parameter in
				part.children for child in parameter.children
			)
	# 仮引数への解決が無い結果の返戻
	return False

# Swift の標準演算子の優先順位
# 未登録の演算子の結合は読み替えない
SWIFT_PRECEDENCE: dict[str, int] = {
	"!=": 30,
	"%": 80,
	"&": 80,
	"&*": 80,
	"&+": 70,
	"&-": 70,
	"&<<": 90,
	"&>>": 90,
	"&&": 20,
	"*": 80,
	"+": 70,
	"-": 70,
	"...": 50,
	"..<": 50,
	"/": 80,
	"<": 30,
	"<<": 90,
	"<=": 30,
	"==": 30,
	">": 30,
	">=": 30,
	">>": 90,
	"??": 40,
	"^": 70,
	"|": 70,
	"||": 10
}

SWIFT_BINARY: set[str] = {
	"additive_expression",
	"bitwise_operation",
	"comparison_expression",
	"conjunction_expression",
	"disjunction_expression",
	"equality_expression",
	"infix_expression",
	"multiplicative_expression",
	"nil_coalescing_expression",
	"range_expression"
}

def swift_binary_work(ancestors: Ancestors, node: Node) -> Work:
	"""明示した括弧を越えず，標準演算子の列を言語の優先順位で結合する
	計算量：演算列のノード数 N と各項の正規化費用 P に対して O(N+P)
	引数：ancestors: 外側の構文，node: 式の根
	戻値：演算と評価順を保持した正規形
	"""
	sequence: list[tuple[Node, Ancestors]] = []
	pending: list[tuple[Node, Ancestors]] = [(node, ancestors)]
	while pending:
		part, parents = pending.pop()
		children: list[Node] = [child for child in part.children if not is_comment(child.type)]
		if part.type in SWIFT_BINARY and len(children) == 3 and children[1].text in SWIFT_PRECEDENCE:
			inner: Ancestors = Ancestors(parents, part)
			pending.extend((child, inner) for child in reversed(children))
		else:
			sequence.append((part, parents))
	values: list[NormNode] = []
	operators: list[str] = []
	for idx, (part, parents) in enumerate(sequence):
		if idx % 2:
			operator: str = part.text
			while operators and (
				SWIFT_PRECEDENCE[operators[-1]] > SWIFT_PRECEDENCE[operator] or
				operators[-1] != "??" and SWIFT_PRECEDENCE[operators[-1]] == SWIFT_PRECEDENCE[operator]
			):
				right: NormNode = values.pop()
				left: NormNode = values.pop()
				values.append(("swift_binary", operators.pop(), (left, right)))
			operators.append(operator)
		else:
			parts: list[NormNode] = yield normalize_work(part, "swift", parents)
			values.append(parts[0] if len(parts) == 1 else ("swift_operand", None, tuple(parts)))
	while operators:
		right = values.pop()
		left = values.pop()
		values.append(("swift_binary", operators.pop(), (left, right)))
	# 括弧付の項を保持した結合結果の返戻
	return values

def css_identifier(text: str) -> str:
	"""CSS の識別子のエスケープを解き，関数名の所属を比較する
	計算量：識別子の文字数 L に対して O(L)
	引数：
		text: 識別子の原文
	戻値：
		エスケープを解いた識別子
	"""
	# １６進の最大桁数と，其の直後の区切空白を１回で消費する
	parts: list[str] = []
	start: int = 0
	for match in re.finditer(r"\\(?:([0-9A-Fa-f]{1,6})(?:\r\n|[ \t\r\n\f])?|([^\r\n\f]))", text):
		parts.append(text[start:match.start()])
		if match.group(1):
			value: int = int(match.group(1), 16)
			parts.append(chr(value) if bool(value) and value < 0X110000 and not 0XD7FF < value < 0XE000 else "\ufffd")
		else:
			parts.append(match.group(2))
		start = match.end()
	parts.append(text[start:])
	# 解いた識別子の返戻
	return "".join(parts)

def content_body_text(text: str, node_type: str) -> str:
	"""内容本文ノードのテキストを意味比較用に正規化する（言語仕様上本文に含まれない部分を除く）
	計算量：本文の文字数 L に対して O(L)

	引数：
		text: 内容本文の原文
		node_type: ノード型

	戻値：
		意味比較用の本文
	"""
	if node_type == "multiline_string_fragment":
		# Java テキストブロックの開始行の空白は本文から除く
		# tree-sitter は此の行末空白を断片の先頭（最初の改行より前）に含める為，整形での行末空白除去を意味破壊と誤検出する
		# 先頭の水平空白だけを除いた本文の返戻（最初の改行以降の実本文は厳密比較を維持し，真の本文変質は捕捉）
		return re.sub(r"^[ \t]+", "", text)
	# 其れ以外の逐語の儘の返戻
	return text

def content_signature(roots: list[Node]) -> tuple[str, ...]:
	"""文字列・ヒアドキュメントの本文等の内容の葉のテキスト列（出現順）を返す

	計算量：O(n + b)（n は木グループのノード数，b は本文の総文字数）

	引数：
		roots: 最上位ノードの一覧

	戻値：
		出現順の内容本文
	"""
	# 内容本文の葉の先行順の収集
	texts: list[str] = []
	pending: list[Node] = list(reversed(roots))
	while pending:
		node: Node = pending.pop()
		if is_comment(node.type):
			continue
		if node.children:
			pending.extend(reversed(node.children))
		elif is_content_body(node.type):
			texts.append(content_body_text(node.text, node.type))
	# 内容の葉のテキスト列の返戻
	return tuple(texts)

def directive_boundary_token(node: Node, lang: str) -> bool:
	"""許可変換で省略される修飾と区切を，適用境界の番号へ数えない
	計算量：トークン長 N に対して O(N)
	引数：
		node: 境界を進める候補
		lang: 対象言語
	戻値：意味の境界を進めるトークンなら真
	"""
	# 変換其の物の可否は，独立した構造照合で判定する
	return bool(node.text.strip()) and not is_comment(node.type) and not (
		lang == "rust" and node.type == "mutable_specifier" or lang == "ruby" and node.kind == "U" and node.type in ("end", "then")
	)

def directive_signature(roots: list[Node], lang: str, retained_comments: set[Node]|None = None) -> tuple[tuple, ...]:
	"""登録ディレクティブコメントの本文・構文境界・位置の成立条件を独立に収集する
	通常コメントの本文は意味比較に含めず，無効な登録形も比較して有効化を検出する
	構文の葉の順序で境界を識別し，同じ綴りの宣言を同一視しない
	計算量：ノード数 N・原文の文字数 L に対して O((N+L) log(N+L+1))
	引数：
		roots: 原文を持つ解析木
		lang: テストケースの言語
		retained_comments: 保持記録として認識したコメントの格納先
	戻値：
		原文順のディレクティブの保持条件
	送出例外：
		RuntimeError: 解析木を原文へ対応付けれない場合
	"""
	text: str = "".join(root.text for root in roots)
	leaves: list[tuple[Node, int, int]] = source_leaves(roots)
	facts: DirectiveFacts = DirectiveFacts(roots, leaves, lang) if leaves else None
	lines: list[int] = [0, *(match.end() for match in re.finditer("\n", text))]
	blank_starts: list[int] = []
	blank_ends: list[int] = []
	for blank in re.finditer(r"(?=(\n[ \t]*\n))", text):
		blank_starts.append(blank.start())
		blank_ends.append(blank.start() + len(blank[1]))
	first: str = text[:lines[1] if len(lines) > 1 else len(text)].lstrip(" \t\f")
	found: list[tuple] = []
	boundary: int = 0
	example_comments: set[int] = set()
	parent: Node
	pending: list[Node] = list(roots)
	while pending:
		parent = pending.pop()
		if parent.type in ("function_declaration", "method_declaration"):
			name: str = next((child.text for child in parent.children if child.field == "name"), "")
			parameters: list[Node] = [child for child in parent.children if child.field in ("parameters", "result")]
			body: Node|None = next((child for child in parent.children if child.field == "body"), None)
			is_example: bool = bool(
				parent.type == "function_declaration" and name.startswith("Example") and (len(name) == 7 or not name[7].islower()) and
				all(part.type == "parameter_list" and not Node.get_named_children(part) for part in parameters) and body is not None
			)
			if is_example:
				comments: list[Node] = [node for node, _, _ in source_leaves([body]) if is_comment(node.type)]
				group: list[Node] = []
				for comment in comments:
					gap: str = text[group[-1].end:comment.start] if group else ""
					if gap.strip() or gap.count("\n") > 1:
						group.clear()
					group.append(comment)
				if group and re.match(r"(?i)(?://|/\*)\s*(?:unordered )?output:", group[0].text):
					# 公式ツールチェーンが読む，本体内の最後のコメントグループだけを保護する
					example_comments.update(id(comment) for comment in group)
		pending.extend(parent.children)
	is_output: bool = False
	first_code: int = next(
		(start for node, start, _ in leaves if not is_comment(node.type) and node.type not in ("hash_bang_line", "shebang")),
		len(text)
	)
	next_code: list[int] = [len(text)] * len(leaves)
	following: int = len(text)
	for idx in range(len(leaves) - 1, -1, -1):
		next_code[idx] = following
		if not is_comment(leaves[idx][0].type):
			following = leaves[idx][1]
	for idx, (node, start, end) in enumerate(leaves):
		raw: str = node.text.rstrip("\r\n")
		if not is_comment(node.type) and node.type not in ("hash_bang_line", "shebang"):
			is_output = False
			# 許されるラッパー・区切・返戻補完は境界の番号を進めない（意味差は構造照合で拒む）
			if directive_boundary_token(node, lang) and raw not in ("(", ")", ",", ";", "return", "{", "}"):
				boundary += 1
			continue
		line: int = bisect_right(lines, start) - 1
		is_standalone: bool = not text[lines[line]:start].strip()
		is_header: bool = start < first_code
		following = next_code[idx]
		blank_idx: int = bisect_left(blank_starts, end)
		has_blank: bool = blank_idx < len(blank_ends) and blank_ends[blank_idx] <= following
		line_gap: int = bisect_right(lines, following) - bisect_right(lines, end)
		is_output = (
			lang == "go" and id(node) in example_comments and (is_output or bool(re.match(r"(?i)(?://|/\*)\s*(?:unordered )?output:", raw)))
		)
		# 適用対象・所有範囲と，登録定義の成立に使う位置・宣言の事実を比較する
		owner: tuple = facts.owners[node] if facts is not None else ()
		target: tuple = facts.following[idx] if facts is not None else ()
		record_count: int = len(found)
		match: re.Match[str]|None = GO_DIRECTIVE.match(raw)
		if match and (lang == "go" or lang in ("c", "cpp") and match[1] in ("go:build", "line")):
			name: str = match[1]
			tail: str = raw[match.end():]
			body: str = tail[1:] if name == "line" and tail.startswith(" ") else tail.lstrip(" \t")
			state: tuple = is_standalone,
			if name in ("go:build", "go:debug"):
				state += (is_header,)
			elif name in ("go:generate", "line"):
				state += (start == lines[line],)
			else:
				# 後続宣言との空行の有無は適用条件，通常コメントの追加は対象を変えない
				state += (has_blank,)
			if name in (
				"export",
				"extern",
				"go:embed",
				"go:noescape",
				"go:noinline",
				"go:norace",
				"go:nosplit",
				"go:uintptrescapes",
				"go:wasmexport",
				"go:wasmimport",
				"line"
			):
				state += (owner, target, line_gap if name == "line" else line_gap == 1)
			if name in ("go:embed", "export"):
				state += (tuple(sorted(facts.imports)) if facts is not None else (),)
			if name == "go:debug":
				state += (facts.package if facts is not None else "",)
			found.append((name, body, boundary, state))
		elif lang in ("c", "cpp", "go") and raw.startswith("/*line "):
			found.append(("block-line", raw[7:-2], boundary, (owner, target)))
		elif lang in ("rust", "javascript", "typescript", "tsx") and raw.startswith("#!") and not raw.startswith("#!["):
			found.append(("shebang", "#!" + raw[2:].lstrip(" \t"), boundary, not start))
		elif lang in ("typescript", "tsx") and re.match(r"///\s*<(?:reference|amd-module|amd-dependency)(?=[\s/>])", raw):
			found.append(("triple-slash", "///" + raw[3:].lstrip(" \t\u3000"), boundary, is_header and is_standalone))
		elif lang in ("ruby", "python") and raw.startswith("#"):
			pattern: str = (
				r"coding[:=][ \t]*([-\w.]+)" if lang == "python" else r"(?i)(?<![\w])(?:coding|encoding)[ \t]*[:=][ \t]*([-\w.]+)"
			)
			if encoding := re.search(pattern, raw):
				is_active: bool = (
					is_standalone and
					(not line or line == 1 and (not first.strip() or first.startswith("#") if lang == "python" else first.startswith("#!")))
				)
				found.append(("encoding", encoding[1].lower(), 0, is_active))
			if lang == "ruby":
				magic_text: str = raw[1:].strip()
				if "-*-" in magic_text:
					magic_text = magic_text.split("-*-", 2)[1]
				for magic in re.finditer(r"(?:^|;)\s*(frozen_string_literal|shareable_constant_value|warn_indent)\s*:\s*([^;\s]+)", magic_text):
					name = magic[1]
					allowed_values: tuple[str, ...] = (
						("experimental_copy", "experimental_everything", "literal", "none") if name == "shareable_constant_value" else ("false", "true")
					)
					if magic[2] not in allowed_values:
						continue
					is_active = is_header if name == "frozen_string_literal" else is_standalone if name == "shareable_constant_value" else True
					found.append(
						(
							name,
							magic[2],
							boundary if name == "shareable_constant_value" else 0,
							(is_active, owner) if name == "shareable_constant_value" else is_active
						)
					)
		elif is_output:
			found.append(("example-output", raw, boundary, (owner, target)))
		elif lang == "go" and re.compile(r'import\s+(?:\(\s*)?"C"').match(text, following):
			# cgo の前文は普通のコメント記号でも内容がコンパイルへ渡る
			found.append(("cgo-preamble", raw, boundary, (not has_blank, owner, target)))
		if retained_comments is not None and record_count < len(found):
			retained_comments.add(node)
	# 認識したディレクティブと成立条件の返戻
	return tuple(found)

def cpp_token_role(text: str) -> str:
	"""前処理の代替記号を役割へ対応付け，原文の綴りとは分ける
	引数：text: 前処理トークンの綴り
	戻値：同じ働きを持つ基本記号
	"""
	# 綴りを観測する文字列化へは此の対応を適用しない
	return { "%:": "#", "%:%:": "##" }.get(text, text)

def cpp_phase_tokens(text: str) -> Iterator[CppToken]:
	"""論理行を読み，include のヘッダ名の内部をコメントへ読み替えない
	計算量：ソース長 N に対して O(N)
	引数：text: 行継続を結合したソース
	戻値：指示のヘッダ名を独立させたトークン列
	"""
	pos: int = 0
	is_line_start: bool = True
	is_command: bool = False
	expects_header: bool = False
	while pos < len(text):
		token: CppToken = next(logical_tokens(text, pos))
		if token.kind == "punct" and text.startswith("%:", pos):
			end: int = pos + (4 if text.startswith("%:%:", pos) else 2)
			token = CppToken("punct", pos, end, text[pos:end])
		if expects_header and token.text == "<":
			end: int = text.find(">", pos + 1)
			if -1 < end and not any(char in "\r\n" for char in text[pos:end]):
				token = CppToken("header", pos, end + 1, text[pos:end + 1])
		pos = token.end
		if token.kind == "space":
			if "\n" in token.text or "\r" in token.text:
				is_line_start = True
				is_command = False
				expects_header = False
		elif token.kind != "comment":
			expects_header = is_command and token.text in ("import", "include", "include_next")
			is_command = is_line_start and cpp_token_role(token.text) == "#"
			is_line_start = False
		yield token
	# 論理行の読取の完了
	return

def cpp_comment_source(text: str) -> str:
	"""行継続を結合し，引用以外のコメントを空白へ置き換える
	計算量：原文長 N に対して O(N)
	引数：text: 独立した入力又は候補
	戻値：コメントを除いた翻訳段階のテキスト
	"""
	logical: str = cpp_source(text).text
	# 行コメントの終端の改行は別トークンとして保持する
	return "".join(
		" " if
		token.kind == "comment" else
		token.text if token.kind == "literal" else token.text.replace("\r\n", "\n").replace("\r", "\n") for token in
		cpp_phase_tokens(logical)
	)

def cpp_token_signature(roots: list[Node]) -> tuple[tuple[str, str], ...]:
	"""C / C++ の前処理トークンを，ディレクティブの論理行も含めて比較する
	演算子の最長一致，引用の接頭辞・接尾辞及び関数形式マクロの密着を保持する
	計算量：ソース長 N に対して O(N)
	引数：roots: 原文を保持する木
	戻値：コメント以外のトークンと，意味を持つ境界
	"""
	text: str = cpp_comment_source("".join(root.text for root in roots))
	tokens: list[CppToken] = list(cpp_phase_tokens(text))
	result: list[tuple[str, str]] = []
	is_directive: bool = False
	is_line_start: bool = True
	pos: int = 0
	while pos < len(tokens):
		token: CppToken = tokens[pos]
		pos += 1
		if token.kind == "comment":
			continue
		if token.kind == "space":
			if "\n" in token.text or "\r" in token.text:
				if is_directive:
					result.append(("directive_end", ""))
				is_directive = False
				is_line_start = True
			continue
		if is_line_start and cpp_token_role(token.text) == "#":
			is_directive = True
			result.append(("directive_start", ""))
		is_line_start = False
		value: str = token.text
		if token.kind == "punct":
			end: int = token.end
			while pos < len(tokens) and tokens[pos].kind == "punct" and tokens[pos].start == end:
				value += tokens[pos].text
				end = tokens[pos].end
				pos += 1
			# 長い演算子を先に採り，空白に依る分離を保持する
			for mark in re.findall(
				r"%:%:|<=>|>>=|<<=|->\*|<:|:>|<%|%>|%:|\.\.\.|##|::|\.\*|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||[+*/%&|^!-]=|.",
				value
			):
				result.append(("punct", mark))
		else:
			result.append((token.kind, value))
		if pos < len(tokens) and tokens[pos].start == token.end:
			if token.kind == "literal" and tokens[pos].kind == "identifier" or token.kind == "identifier" and tokens[pos].kind == "literal":
				result.append(("literal_attachment", ""))
			if (
				is_directive and token.kind == "identifier" and tokens[pos].text == "(" and
				any(item == ("identifier", "define") for item in result[-3:])
			):
				result.append(("function_macro", ""))
	if is_directive:
		result.append(("directive_end", ""))
	# 論理行と最長一致を保持したトークン列の返戻
	return tuple(result)

def cpp_directive_source(text: str) -> str:
	"""代替記号の指示の開始だけを，同じ長さの解析用表記へ対応付ける
	計算量：原文長 L に対して O(L)
	引数：text: 論理行のソース
	戻値：引用と置換本文を保持した解析用ソース
	"""
	if "%:" not in text:
		# 対応を要しない原文の返戻
		return text
	chars: list[str] = list(text)
	is_start: bool = True
	for token in cpp_phase_tokens(text):
		if token.kind == "space":
			if "\n" in token.text or "\r" in token.text:
				is_start = True
		elif token.kind != "comment":
			if is_start and token.text == "%:":
				chars[token.start:token.end] = "# "
			is_start = False
	# 位置を保った指示の表記の返戻
	return "".join(chars)

def css_empty_values(roots: list[Node]) -> list[Node]:
	"""閉波括弧の直前の空のカスタム値を，偽の欠落から空の値へ対応付ける
	計算量：ノード数 N と空白・コメントの長さ B に対して O(N+B)
	引数：roots: CSS の解析木
	戻値：実際のエラーを保持し，空の値だけを対応付けた木
	"""
	pending: list[Node] = list(roots)
	while pending:
		node: Node = pending.pop()
		if node.type == "block":
			for declaration, closing in zip(node.children, node.children[1:]):
				if declaration.type != "declaration" or closing.type != "}":
					continue
				parts: list[Node] = [child for child in declaration.children if not is_comment(child.type)]
				if (
					len(parts) == 3 and parts[0].type == "property_name" and css_identifier(parts[0].text).startswith("--") and
					len(css_identifier(parts[0].text)) > 2 and parts[1].type == ":" and parts[2].type == "MISSING" and
					parts[1].end <= parts[2].start <= closing.start and parts[2].start == parts[2].end and node.source is not None and
					re.fullmatch(r"(?:[ \t\r\n\f]|/\*[\s\S]*?\*/)*", node.source[parts[1].end:closing.start])
				):
					# セミコロンの在る空の値と同じ構造にし，値の空白は原文で照合する
					parts[2].type: str = "integer_value"
		pending.extend(node.children)
	# 対応付済の木の返戻
	return roots

def scss_interpolation_end(text: str, start: int) -> int|None:
	"""引用とコメントの中の閉じを除き，補間の対応する閉じの後を返す
	計算量：補間の長さ B に対して O(B)
	引数：text: 原文，start: 開きの位置
	戻値：閉じの後の位置．閉じを欠く場合は None
	"""
	depth: int = 1
	pos: int = start + 2
	while pos < len(text):
		if text[pos] == "\\":
			pos += 2
		elif text[pos] in "\"'":
			quote: str = text[pos]
			pos += 1
			while pos < len(text) and text[pos] != quote:
				pos += 2 if text[pos] == "\\" else 1
			pos += 1
		elif text.startswith("/*", pos):
			end: int = text.find("*/", pos + 2)
			if end < 0:
				# 閉じを欠くコメントの返戻
				return None
			pos = end + 2
		elif text.startswith("//", pos):
			end = text.find("\n", pos + 2)
			if end < 0:
				# コメントに閉じを取り込まれた補間の返戻
				return None
			pos = end
		else:
			if text[pos] == "{":
				depth += 1
			elif text[pos] == "}":
				depth -= 1
				if not depth:
					# 対応する閉じの後の位置の返戻
					return pos + 1
			pos += 1
	# 閉じを欠く補間の返戻
	return None

def css_value_source(
	lang: str,
	text: str,
	should_mask_expressions: bool = False,
	comments: list[str]|None = None
) -> tuple[str, dict[int, tuple[int, str, str]]]:
	"""引用符と URL の本文を独立に読み，外側の構文を解析する為のソースを返す
	計算量：ソース長 B に対して O(B)
	引数：
		lang: CSS 又は SCSS
		text: 原文
		should_mask_expressions: 行配置の照合で，補間の式も本文として覆うか（式の意味を比較する場合は偽）
		comments: 同じ走査で取得するコメント記録の格納先．不要なら None
	戻値：本文をマスクしたソースと，原文の開始・終端・種別・値
	"""
	masked: list[str] = list(text)
	values: dict[int, tuple[int, str, str]] = {}
	function: re.Pattern[str] = re.compile(r"(?:[-\w]|\\(?:[0-9A-Fa-f]{1,6}[ \t\r\n\f]?|[^\r\n\f]))+\(", re.ASCII)
	interpolation: re.Pattern[str] = re.compile(r"#\{\$[-A-Za-z_][-A-Za-z_0-9]*\}")
	line_end: re.Pattern[str] = re.compile(r"[\r\n\f]")
	pos: int = 0
	while pos < len(text):
		call: re.Match[str]|None = (
			function.match(text, pos) if not pos or not (text[pos - 1].isalnum() or text[pos - 1] in "_-.\\") else None
		)
		if call is not None and css_identifier(call.group()[:-1]).lower() == "url":
			start: int = pos
			body_start: int = call.end()
			end: int = body_start
			while end < len(text) and text[end].isspace():
				end += 1
			if end < len(text) and text[end] in "\"'":
				pos = end
				continue
			body: list[str] = []
			part: int = body_start
			while end < len(text) and text[end] != ")":
				if lang == "scss" and text.startswith("#{", end):
					next_end: int|None = scss_interpolation_end(text, end)
					if next_end is None:
						# 閉じを欠く本文を保護せず，残りを繰り返して走査しない事の返戻
						return "".join(masked), values
					body.extend((text[part:end], "x"))
					part = end = next_end
				else:
					end += 2 if text[end] == "\\" else 1
			checked: str = "".join(body) + text[part:end] if body else text[body_start:end]
			if end < len(text) and text[end] == ")" and (lang == "scss" or re.fullmatch(r'(?:\\[^\r\n\f]|[^\s"\'()\\])*', checked.strip())):
				values[start] = end + 1, "css_url", text[body_start:end]
				masked[body_start:end] = "x" * (end - body_start)
				pos = end + 1
				continue
		interpolation_end: int|None = None
		if lang == "scss" and text.startswith("#{", pos):
			if should_mask_expressions:
				interpolation_end = scss_interpolation_end(text, pos)
				if interpolation_end is None:
					# 閉じを欠く補間を保護せず，残りを繰り返して走査しない事の返戻
					return "".join(masked), values
			elif match := interpolation.match(text, pos):
				interpolation_end = match.end()
		if interpolation_end is not None:
			end = interpolation_end
			values[pos] = end, "scss_interpolation", text[pos:end]
			masked[pos:end] = "x" * (end - pos)
			pos = end
			continue
		if text.startswith("/*", pos):
			end: int = text.find("*/", pos + 2)
			end = len(text) if end < 0 else end + 2
			if comments is not None:
				comments.append(text[pos:end])
			pos = end
			continue
		if lang == "scss" and text.startswith("//", pos):
			closing: re.Match[str]|None = line_end.search(text, pos + 2)
			end = closing.start() if closing is not None else len(text)
			if comments is not None:
				comments.append(text[pos:end])
			pos = end
			continue
		if text[pos] not in "\"'":
			if text[pos] == "\f":
				masked[pos] = "\n"
			pos += 1
			continue
		start: int = pos
		quote: str = text[pos]
		pos += 1
		part = pos
		string_parts: list[tuple[str, str]] = []
		while pos < len(text) and text[pos] != quote:
			if text[pos] == "\\":
				pos += 3 if text.startswith("\r\n", pos + 1) else 2
			elif lang == "scss" and text.startswith("#{", pos):
				interpolation_end = scss_interpolation_end(text, pos)
				if interpolation_end is None:
					break
				string_parts.extend(
					(("text", css_identifier(re.sub(r"\\(?:\r\n|[\r\n\f])", "", text[part:pos]))), ("expression", text[pos:interpolation_end]))
				)
				part = pos = interpolation_end
			elif text[pos] in "\r\n\f":
				break
			else:
				pos += 1
		if pos >= len(text) or text[pos] != quote:
			continue
		pos += 1
		kind: str = "css_string"
		if string_parts:
			string_parts.append(("text", css_identifier(re.sub(r"\\(?:\r\n|[\r\n\f])", "", text[part:pos - 1]))))
			value: str = json.dumps(string_parts, ensure_ascii=False)
			kind: str = "scss_string"
		else:
			body: str = text[start + 1:pos - 1]
			value: str = css_identifier(re.sub(r"\\(?:\r\n|[\r\n\f])", "", body))
		values[start] = pos, kind, value
		masked[start:pos] = "\"" + "x" * (pos - start - 2) + "\""
	# 保護本文の値と位置の返戻
	return "".join(masked), values

def css_syntax_source(lang: str, roots: list[Node], text: str) -> str:
	"""特殊な序数の記法とマップの区切を，生のパーサーが読める同値な表記へ揃える
	引用本文・URL・値の内側の空白を変更せず，マップの全要素のキー値を確認する
	計算量：ソース長 B・ノード数 N・囲みの深さ D に対して O(BD+N)
	引数：lang: CSS 又は SCSS，roots: 独立した木，text: 原文
	戻値：序数とマップの区切の空白だけを詰めたソース
	"""
	removed: set[int] = set()
	pending: list[Node] = list(roots)
	ordinal_names: set[str] = { "nth-child", "nth-last-child", "nth-last-of-type", "nth-of-type" }
	ordinal: re.Pattern[str] = re.compile(r"\s*\(([+-]?[0-9]*[nN])([ \t\r\n\f]*)([+-])([ \t\r\n\f]*)([0-9]+)\s*\)")
	while pending:
		node: Node = pending.pop()
		if node.type in ("pseudo_class_selector", "pseudo_element_selector"):
			name: Node|None = next((child for child in node.children if child.type == "class_name"), None)
			if name is not None and css_identifier(name.text).lower() in ordinal_names:
				match: re.Match[str]|None = ordinal.match(text, name.end)
				if match is not None:
					for group in 2, 4:
						removed.update(range(match.start(group), match.end(group)))
		pending.extend(node.children)
	if lang == "scss":
		masked, _ = css_value_source(lang, text)
		pattern: re.Pattern[str] = (
			re.compile(r'/\*[\s\S]*?(?:\*/|$)|//[^\r\n]*|"(?:\\[\s\S]|[^"\\])*(?:"|$)|\'(?:\\[\s\S]|[^\'\\])*(?:\'|$)|\\[\s\S]|.')
		)
		stack: list[tuple[str, int, list[int], list[int]]] = []
		for token in pattern.finditer(masked):
			char: str = token.group()
			pos: int = token.start()
			if char in ("(", "[", "{"):
				stack.append((char, pos, [], []))
			elif char in (")", "]", "}") and stack:
				opening, begin, commas, colons = stack.pop()
				if (opening, char) not in (("(", ")"), ("[", "]"), ("{", "}")):
					continue
				previous: str = masked[begin - 1] if begin else ""
				if opening != "(" or previous and (previous.isalnum() or previous in "_-\\."):
					continue
				begins: list[int] = [begin + 1, *(comma + 1 for comma in commas)]
				ends: list[int] = [*commas, pos]
				items: list[tuple[int, int]] = [(first, last) for first, last in zip(begins, ends) if masked[first:last].strip()]
				if len(items) != len(colons):
					continue
				pairs: list[tuple[int, int, int]] = []
				for (first, last), colon in zip(items, colons):
					if not first <= colon < last or not masked[first:colon].strip() or not masked[colon + 1:last].strip():
						pairs = []
						break
					pairs.append((first, colon, last))
				for first, colon, last in pairs:
					for start, end in (first, colon), (colon + 1, last):
						while start < end and masked[start].isspace():
							removed.add(start)
							start += 1
						while end > start and masked[end - 1].isspace():
							end -= 1
							removed.add(end)
			elif stack and char == ",":
				stack[-1][2].append(pos)
			elif stack and char == ":":
				stack[-1][3].append(pos)
	# 保護する本文を原文の儘に残したソースの返戻
	return "".join(char for pos, char in enumerate(text) if pos not in removed)

def css_url_body(node: Node) -> str|None:
	"""通常のトークン列へ分解しない，引用符の無い URL の引数を返す
	計算量：直下の子の数 C・関数名と引数の文字数 B に対して O(C+B)
	引数：
		node: 関数呼出のノード
	戻値：
		保護する引数全体．URL 又は引用符の無い引数でなければ `None`
	"""
	if node.type != "call_expression":
		# URL の形でない事の返戻
		return None
	named: list[Node] = Node.get_named_children(node)
	if len(named) != 2 or css_identifier(named[0].text.rsplit(":", 1)[-1]).strip().lower() != "url":
		# URL の関数でない事の返戻
		return None
	body: str = named[1].text if named[1].type == "css_url" else named[1].text[1:-1]
	# 引用形は通常の文字列の保護へ渡す
	return None if body.lstrip().startswith(("\"", "'")) else body.strip(" \t\r\n\f")

def css_comparison_roots(lang: str, roots: list[Node]) -> list[Node]:
	"""文字列と非引用 URL を独立に読み，本文の誤読を外側の構造へ伝えない
	計算量：ソース長 B・ノード数 N・囲みの深さ D と再解析費用 P に対して O(BD+N+P)
	引数：lang: CSS 又は SCSS，roots: 独立した原文の木
	戻値：本文を値として保持し，同値なソースを再解析した木
	"""
	text: str = "".join(root.text for root in roots)
	logical: str = css_syntax_source(lang, roots, text)
	if logical != text:
		text = logical
		roots = parse_dump(dump_ast(text, LANGS[lang][1]), text)
	logical, values = css_value_source(lang, text)
	masked: list[str] = list(logical)
	has_empty_include: bool = False
	if lang == "scss":
		pending_includes: list[Node] = list(roots)
		while pending_includes:
			node: Node = pending_includes.pop()
			if node.type in ("include_statement", "mixin_statement"):
				for child in node.children:
					if child.type in ("ERROR", "arguments", "parameters") and re.fullmatch(r"\([ \t\r\n\f]*\)", child.text):
						masked[child.start:child.end] = " " * (child.end - child.start)
						has_empty_include = True
			pending_includes.extend(node.children)
	if not values and not has_empty_include and logical == text:
		# 文字列と空の include の引数を持たない原文の返戻
		return css_empty_values(scss_argument_roots(text, roots) if lang == "scss" else roots)
	logical: str = "".join(masked)
	result: list[Node] = parse_dump(dump_ast(logical, LANGS[lang][1]), logical)
	if lang == "scss":
		result = scss_argument_roots(logical, result)
	pending: list[Node] = list(result)
	while pending:
		node: Node = pending.pop()
		node.source: str|None = text
		if (
			node.type in ("call_expression", "plain_value", "string_value") and node.start in values and node.end == values[node.start][0]
		):
			_, node.type, node.text = values[node.start]
			node.children: list[Node] = []
		else:
			if css_url_body(node) is not None:
				head, arguments = Node.get_named_children(node)
				start: int = head.start + head.text.rfind(":") + 1
				while start < head.end and text[start].isspace():
					start += 1
				if start in values and node.end == values[start][0]:
					_, arguments.type, arguments.text = values[start]
					arguments.children: list[Node] = []
			pending.extend(child for child in node.children if child.type != "css_url")
	# デコード値と外側の構造を保持した木の返戻
	return css_empty_values(result)

def java_unicode_source(text: str, origins: list[int]|None = None) -> str:
	"""Java の字句解析に先行する Unicode エスケープを，適格な位置だけで解く
	計算量：ソース長 N に対して O(N)
	引数：text: 翻訳前のソース，origins: 翻訳後の各文字の原文位置と終端の格納先
	戻値：論理ソース．単独のサロゲートはパーサーへ渡せる元の綴りを保つ
	"""
	parts: list[str] = []
	pos: int = 0
	slashes: int = 0
	pattern: re.Pattern[str] = re.compile(r"\\u+([0-9A-Fa-f]{4})")
	while pos < len(text):
		match: re.Match[str]|None = pattern.match(text, pos) if text[pos] == "\\" and not slashes % 2 else None
		if match is not None:
			value: int = int(match[1], 16)
			decoded: str = "\\u" + f"{value:04X}" if 0XD7FF < value < 0XE000 else chr(value)
			parts.append(decoded)
			if origins is not None:
				origins.extend([pos] * len(decoded))
			pos = match.end()
			slashes = 0
		else:
			parts.append(text[pos])
			if origins is not None:
				origins.append(pos)
			slashes = slashes + 1 if text[pos] == "\\" else 0
			pos += 1
	if origins is not None:
		origins.append(len(text))
	# 翻訳段階を１度だけ適用したソースの返戻
	return "".join(parts)

def java_document_body(raw: str) -> str:
	"""物理行の装飾と，エスケープが作る行の装飾をコード例と区別して読む
	計算量：本文の文字数 B に対して O(B)
	引数：raw: 元の物理行を保持したドキュメントコメント
	戻値：コードの内容を保持し，装飾だけを除いた翻訳後の本文
	"""
	from verify_case_variants import comment_enclosures
	physical: str = doc_body("java", raw)
	origins: list[int] = []
	body: str = java_unicode_source(physical, origins)
	_, protected, _ = comment_enclosures(body)
	pattern: str = r"<(/?)(code|pre)(?=[\s/>])(?:[^>\"']|\"[^\"]*\"|'[^']*')*>"
	stack: list[str] = []
	begin: int = 0
	for match in re.finditer(pattern, body, re.I):
		if match[1]:
			if stack and stack[-1] == match[2].lower():
				stack.pop()
				if not stack:
					protected[begin:match.end()] = b"\1" * (match.end() - begin)
		elif not match[0].endswith("/>"):
			if not stack:
				begin = match.start()
			stack.append(match[2].lower())
	if stack:
		protected[begin:] = b"\1" * (len(body) - begin)
	position: int = 0
	inline: re.Pattern[str] = re.compile(r"\{@[A-Za-z]\w*\b")
	while match := inline.search(body, position):
		begin = match.start()
		end: int = doc_bracket_end(body, begin)
		protected[begin:end] = b"\1" * (end - begin)
		position = end
	parts: list[str] = []
	position: int = 0
	for line in body.splitlines(keepends=True):
		mark: re.Match[str]|None = re.match(r"[ \t]*\* ?", line)
		if (
			position and physical[origins[position - 1]] == "\\" and mark is not None and not any(protected[position:position + mark.end()])
		):
			parts.append(line[mark.end():])
		else:
			parts.append(line)
		position += len(line)
	# 翻訳後のコードへ物理行の装飾の除去を再適用しない本文の返戻
	return "".join(parts)

def ruby_comparison_roots(roots: list[Node]) -> list[Node]:
	"""Ruby の行頭ディレクティブの厳密な終端を，コメント本文から区別する
	計算量：ソース長 B・ノード数 N・誤読した終端候補 K と解析費用 P に対して O(K(B+N+P))
	引数：roots: 独立した原文の木
	戻値：原文の範囲を保持し，偽の終端を通常の本文として読んだ木
	"""
	text: str = "".join(root.text for root in roots)
	masked: list[str] = list(text)
	result: list[Node] = roots
	while True:
		changed: bool = False
		pending: list[Node] = list(result)
		while pending:
			node: Node = pending.pop()
			if node.type == "uninterpreted" and node.start > 6:
				start: int = node.start - 7
				line_end: int = text.find("\n", node.start)
				line: str = text[node.start:line_end if line_end > -1 else len(text)]
				if line not in ("", "\r") or start and text[start - 1] != "\n":
					masked[start] = "a"
					changed = True
			elif is_comment(node.type) and node.text.startswith("=begin"):
				end: re.Match[str]|None = re.search(r"(?m)^=end(?=[ \t\r\n\f\v]|$)", text[node.start + 6:])
				if end is not None:
					limit: int = node.start + 6 + end.start()
					for false_end in re.finditer(r"=end", text[node.start:limit]):
						position: int = node.start + false_end.end() - 4
						if masked[position] == "=":
							masked[position:position + 4] = "xxxx"
							changed = True
			pending.extend(node.children)
		if not changed:
			break
		logical: str = "".join(masked)
		result = parse_dump(dump_ast(logical, "rb"), logical)
	pending = list(result)
	while pending:
		node = pending.pop()
		node.source: str|None = text
		pending.extend(node.children)
	# マーカーの本文も原文へ戻した木の返戻
	return result

def has_error(roots: list[Node]) -> bool:
	"""木グループ内に ERROR ノードが存在するか判定する

	計算量：O(n)（n は木グループのノード数）

	引数：
		roots: 最上位ノードの一覧

	戻値：
		ERROR 又は MISSING が有れば True
	"""
	# 木グループのエラーノードの走査
	pending: list[Node] = list(reversed(roots))
	while pending:
		node: Node = pending.pop()
		if node.type in ("ERROR", "MISSING"):
			# エラーを検出した事の返戻
			return True
		pending.extend(reversed(node.children))
	# エラーが無い事の返戻
	return False

def c_preprocessor_roots(lang: str, text: str, roots: list[Node]) -> list[Node]:
	"""前処理行の引用内のコメント記号を隠し，解析後に元の本文を戻す
	計算量：本文長 B・ノード数 N と必要時の再解析費用 P に対して O(B+N+P)
	引数：lang: C 系の言語，text: 論理行の原文，roots: 原文の木
	戻値：前処理トークンの綴りと範囲を保持した木
	"""
	if not has_error(roots):
		# 前処理行を読めた木の返戻
		return roots
	chars: list[str] = list(text)
	is_directive: bool = False
	is_start: bool = True
	for token in logical_tokens(text):
		if token.kind == "space":
			if "\n" in token.text:
				is_start, is_directive = True, False
			continue
		if is_start and token.text == "#":
			is_directive = True
		if is_directive and token.kind in ("comment", "literal"):
			chars[token.start:token.end] = re.sub(r"[^\r\n]", " " if token.kind == "comment" else "x", token.text)
		if token.kind != "comment":
			is_start = False
	logical: str = "".join(chars)
	if logical == text:
		# 解析に必要な引用の置換が無い木の返戻
		return roots
	result: list[Node] = parse_dump(dump_ast(logical, "cpp" if lang == "h" else lang), logical)
	pending: list[Node] = list(result)
	while pending:
		node: Node = pending.pop()
		node.source: str|None = text
		pending.extend(node.children)
	# 内容の比較には置換前の引用・コメントを使う木の返戻
	return result

def cpp_template_roots(roots: list[Node]) -> list[Node]:
	"""記録された名前空間のクラステンプレートの型引数の閉じを分離する
	計算量：ノード数 N・本文長 B と必要時の再解析費用 P に対して O(N+B+P)
	引数：roots: C++ の独立した解析木
	戻値：既知の型だけから成る宣言の，閉記号を分けた木
	"""
	types: set[tuple[str, ...]] = set()
	templates: set[tuple[str, ...]] = set()
	pending: list[tuple[Node, tuple[str, ...], bool]] = [(root, (), False) for root in roots]
	while pending:
		node, namespace, is_template = pending.pop()
		if node.type in ("function_definition", "lambda_expression"):
			continue
		if node.type == "namespace_definition":
			name: Node|None = next((child for child in node.children if child.field == "name"), None)
			if name is None:
				continue
			namespace: tuple[str, ...] = *namespace, name.text
		if node.type in ("class_specifier", "struct_specifier"):
			name = next((child for child in node.children if child.field == "name"), None)
			if name is not None and name.type == "type_identifier":
				types.add((*namespace, name.text))
				if is_template:
					templates.add((*namespace, name.text))
			continue
		pending.extend((child, namespace, is_template or node.type == "template_declaration") for child in node.children)
	text: str = "".join(root.text for root in roots)
	tokens: list[CppToken] = [token for token in logical_tokens(text) if token.kind not in ("space", "comment")]
	marks: set[int] = set()
	def read_type(position: int, depth: int) -> int:
		"""既知の修飾型と，型だけの実引数列の直後を求める
		引数：position: 有意トークン列の読始，depth: 型引数の入子の深さ
		戻値：確認した型の直後の位置，未知の型・欠落した区切又は６４段超の入子なら `-1`
		"""
		if depth > 64:
			# 過剰な入子を未確認とする結果の返戻
			return -1
		if position + 1 < len(tokens) and tokens[position].text == tokens[position + 1].text == ":":
			position += 2
		name: list[str] = []
		while position < len(tokens) and tokens[position].kind == "identifier":
			name.append(tokens[position].text)
			position += 1
			if position + 1 >= len(tokens) or tokens[position].text != ":" or tokens[position + 1].text != ":":
				break
			position += 2
		if tuple(name) not in types:
			# 既知の型へ解決出来ない結果の返戻
			return -1
		if position < len(tokens) and tokens[position].text == "<" and tuple(name) in templates:
			while True:
				position = read_type(position + 1, depth + 1)
				if position < 0 or position >= len(tokens):
					# 型引数を読めない結果の返戻
					return -1
				if tokens[position].text != ",":
					break
			if tokens[position].text != ">":
				# 閉記号の欠落を示す結果の返戻
				return -1
			position += 1
		# 型の直後の位置の返戻
		return position
	for idx, token in enumerate(tokens):
		# 値の比較やシフトの途中から型として読み始めない
		if idx and tokens[idx - 1].text not in ("{", ";", "}"):
			continue
		end: int = read_type(idx, 0)
		if end < 0 or end + 1 >= len(tokens) or tokens[end].kind != "identifier" or tokens[end + 1].text != ";":
			continue
		for pos in range(idx + 1, end):
			if tokens[pos - 1].text == tokens[pos].text == ">" and tokens[pos - 1].end == tokens[pos].start:
				marks.add(tokens[pos].start)
	if not marks:
		# 閉記号の分離が不要な木の返戻
		return roots
	parts: list[str] = []
	previous: int = 0
	for position in sorted(marks):
		parts.extend((text[previous:position], " "))
		previous = position
	parts.append(text[previous:])
	logical: str = "".join(parts)
	# 既知のテンプレートの型引数だけを，文法上同じ区切で読む木の返戻
	return parse_dump(dump_ast(logical, "cpp"), logical)

def cpp_bitfield_roots(roots: list[Node]) -> list[Node]:
	"""ビットフィールドの幅と初期化子を分け，旧パーサーの誤読から独立に照合する
	計算量：ソース長 B・ノード数 N と２回の解析費用 P に対して O(B+N+P)
	引数：roots: C++ の独立した解析木
	戻値：型・宣言名・幅・初期化を保持した木．境界を確定出来なければ元の木
	"""
	text: str = "".join(root.text for root in roots)
	bodies: set[int] = set()
	names: set[int] = set()
	unnamed: set[int] = set()
	pending: list[Node] = list(roots)
	while pending:
		node: Node = pending.pop()
		if node.type == "field_declaration_list":
			bodies.add(node.start)
		if node.type == "field_identifier" and node.field == "declarator":
			names.add(node.end)
		if node.type == "field_declaration" and any(child.type == "MISSING" and child.field == "declarator" for child in node.children):
			unnamed.update(child.start for child in node.children if child.type == "bitfield_clause")
		pending.extend(node.children)
	if not bodies or not names and not unnamed:
		# ビットフィールドの候補が無い木では，トークン列を追加で構築しない
		return roots
	tokens: list[CppToken] = [token for token in cpp_tokens(text) if token.kind not in ("comment", "space")]
	pairs: dict[int, int] = {}
	stack: list[int] = []
	for idx, token in enumerate(tokens):
		if token.text in ("(", "[", "{"):
			stack.append(idx)
		elif token.text in (")", "]", "}") and stack:
			opening: int = stack.pop()
			if (tokens[opening].text, token.text) in (("(", ")"), ("[", "]"), ("{", "}")):
				pairs[opening] = idx
	clauses: dict[int, tuple[int, int, int, int, str]] = {}
	for opening, closing in pairs.items():
		if tokens[opening].start not in bodies:
			continue
		pos: int = opening + 1
		while pos < closing:
			if tokens[pos].text != ":" or not pos or tokens[pos - 1].end not in names and tokens[pos].start not in unnamed:
				pos = pairs.get(pos, pos) + 1
				continue
			colon: int = pos
			name_end: int = tokens[pos].end if tokens[pos].start in unnamed else tokens[pos - 1].end
			pos += 1
			width_begin: int = pos
			width_end: int = -1
			initializer: int = -1
			has_conditional: bool = False
			while pos < closing and tokens[pos].text not in (",", ";"):
				mark: str = tokens[pos].text
				if mark == "?":
					has_conditional = True
				if width_end < 0 and (mark == "{" or mark == "=" and not has_conditional):
					width_end = pos
					initializer = pos + (mark == "=")
				pos = pairs.get(pos, pos) + 1
			if pos >= closing or pos == width_begin:
				# 不完全な宣言の境界を正常なビットフィールドとして修復しない
				return roots
			if width_end < 0:
				width_end = pos
			if tokens[colon].start in unnamed and initializer > -1:
				# 無名のビットフィールドへ初期化子を付けた不正な入力を修復しない
				return roots
			clauses[name_end] = (
				tokens[colon].start,
				tokens[width_begin].start,
				tokens[width_end - 1].end,
				tokens[pos - 1].end,
				text[tokens[initializer].start:tokens[pos - 1].end] if initializer > -1 else ""
			)
			pos += 1
	if not clauses:
		# 幅の境界を独立に得れない場合は，生の木の保持検査を続ける
		return roots
	masked: list[str] = list(text)
	expressions: list[str] = []
	for begin, width_begin, width_end, end, initializer in clauses.values():
		masked[begin:end] = re.sub(r"[^\r\n]", " ", text[begin:end])
		if begin in unnamed:
			masked[begin] = "a"
		expressions.extend(("int alpha=(" + text[width_begin:width_end] + ");", "int beta=" + (initializer or "0") + ";"))
	logical: str = "".join(masked)
	result: list[Node] = parse_dump(dump_ast(logical, "cpp"), logical)
	probe: str = "void alpha(){" + "".join(expressions) + "}"
	parsed: list[Node] = parse_dump(dump_ast(probe, "cpp"), probe)
	if has_error(result) or has_error(parsed):
		# 適応で別の構文のエラーを隠さず，未確認の入力は元の検査へ渡す
		return roots
	values: list[Node] = []
	pending = list(reversed(parsed))
	while pending:
		node = pending.pop()
		if node.field == "value":
			values.append(node)
		else:
			pending.extend(reversed(node.children))
	if len(values) != len(clauses) << 1:
		# 幅・初期化子の欠落又は追加を，解析の成功だけで受理しない
		return roots
	positions: dict[int, tuple[Node, Node]] = {
		position: (values[idx << 1], values[(idx << 1) + 1]) for idx, position in enumerate(clauses)
	}
	attached: int = 0
	width: Node
	initializer_node: Node
	begin: int
	end: int
	initializer: str
	part: Node
	start: int
	pending = list(result)
	while pending:
		node = pending.pop()
		node.source: str|None = text
		if node.type == "field_identifier" and node.end in positions:
			attached += 1
			width, initializer_node = positions[node.end]
			begin, width_begin, width_end, end, initializer = clauses[node.end]
			for part, start in (width, width_begin - 1), (initializer_node, end - len(initializer)):
				delta: int = start - part.start
				inner: list[Node] = [part]
				while inner:
					child: Node = inner.pop()
					child.text: str = child.text
					child.start += delta
					child.end += delta
					child.source: str|None = None
					inner.extend(child.children)
			name: Node = Node("" if begin in unnamed else node.text, node.kind, "unnamed_bitfield" if begin in unnamed else node.type)
			name.start, name.end, name.source = node.start, node.end, None if begin in unnamed else text
			node.type: str = "bitfield_declarator"
			node.children: list[Node] = [name, width]
			if initializer:
				node.children.append(initializer_node)
			node.end: int = end
			continue
		pending.extend(node.children)
	if attached != len(clauses):
		# 全ての幅・初期化子の所属を確認出来ない適応は採用しない
		return roots
	# 幅と初期化子の構造を，宣言子の所有範囲に保持した木の返戻
	return result

def c_binding_roots(lang: str, roots: list[Node]) -> list[Node]:
	"""既知の型・値を文頭の曖昧な構文へ渡し，宣言子を独立に読み直す
	計算量：ノード数 N・スコープ深さ D と必要な宣言の再解析費用 P に対して O(ND+P)
	引数：lang: C 系の言語，roots: 同じ原文から求めた木
	戻値：名前の所属と原文の範囲を保持した木
	"""
	if lang in ("cpp", "h"):
		roots = cpp_bitfield_roots(cpp_template_roots(roots))
	text: str = "".join(root.text for root in roots)
	macro_names: set[str] = set(re.findall(r"(?m)^\s*#\s*(?:define|undef)\s+([A-Za-z_]\w*)", text))
	changed: bool = False
	pending: list[tuple[Node, dict[str, bool]]] = [(root, {}) for root in reversed(roots)]
	while pending:
		node, scope = pending.pop()
		if node.type in (
			"compound_statement",
			"field_declaration_list",
			"namespace_definition",
			"function_definition",
			"for_statement",
			"for_range_loop",
			"lambda_expression",
			"template_declaration"
		):
			scope: dict[str, bool] = dict(scope)
		if node.type == "template_declaration":
			parameters = [child for child in node.children if child.type == "template_parameter_list"]
			while parameters:
				part = parameters.pop()
				if part.type in ("type_parameter_declaration", "optional_type_parameter_declaration", "variadic_type_parameter_declaration"):
					for child in part.children:
						if child.type == "type_identifier":
							scope[child.text] = True
				else:
					parameters.extend(part.children)
		if node.type == "function_definition":
			parameters: list[Node] = [child for child in node.children if child.field == "declarator"]
			while parameters:
				part: Node = parameters.pop()
				if part.type in ("parameter_declaration", "optional_parameter_declaration"):
					decl: Node|None = next((child for child in part.children if child.field == "declarator"), None)
					while decl is not None and decl.type != "identifier":
						decl = next(
							(child for child in decl.children if child.field == "declarator" or child.kind == "N" and is_paren_wrapper(decl.type)),
							None
						)
					if decl is not None:
						scope[decl.text] = False
				else:
					parameters.extend(part.children)
		if node.type in ("using_declaration", "using_directive"):
			scope.clear()
		if node.type in ("type_definition", "alias_declaration"):
			for child in node.children:
				if child.field in ("declarator", "name") and child.type == "type_identifier":
					scope[child.text] = True
		if node.type in ("declaration", "expression_statement"):
			match: re.Match[str]|None = re.match(r"([A-Za-z_]\w*)\s*(.*)", node.text, re.S)
			if match is not None:
				name, rest = match.groups()
				is_builtin: bool = (
					name in ("char", "double", "float", "int", "long", "short", "signed", "unsigned", "void") or
					name == "bool" and lang in ("cpp", "h") or name == "_Bool" and lang == "c"
				)
				role: bool|None = True if is_builtin else scope.get(name)
				if name in macro_names:
					role = None
				if role is False and node.type == "declaration" and rest.startswith(("*", "&")):
					prefix: str = "void __scope(void){"
					logical = prefix + "(" + name + ")" + node.text[len(name):] + "}"
					parsed = parse_dump(dump_ast(logical, "cpp" if lang == "cpp" else "c"), logical)
					function: Node|None = next((child for child in parsed[0].children if child.type == "function_definition"), None)
					body: Node|None = next((child for child in function.children if child.field == "body"), None) if function is not None else None
					items = Node.get_named_children(body) if body is not None else []
					if len(items) == 1 and items[0].type == "expression_statement" and not has_error(parsed):
						work = [items[0]]
						while work:
							part = work.pop()
							start, end = part.start - len(prefix), part.end - len(prefix)
							part.start: int = node.start + max(0, start - (2 if start > len(name) + 1 else 1))
							part.end: int = node.start + max(0, end - (2 if end > len(name) + 1 else 1))
							part.source: str|None = text
							work.extend(part.children)
						node.type: str = "expression_statement"
						node.children: list[Node] = items[0].children
						changed = True
				if (
					role is True and rest.startswith(("(", "*")) and (rest.startswith("(") or node.type == "expression_statement") and
					"#" not in node.text
				):
					logical: str = "int" + node.text[len(name):]
					if lang == "cpp":
						logical = re.sub(r"^int(\s*\(\s*)&(?!&)", r"int\1*", logical)
					parsed: list[Node] = parse_dump(dump_ast(logical, "c"), logical)
					items: list[Node] = Node.get_named_children(parsed[0]) if len(parsed) == 1 else []
					if len(items) == 1 and items[0].type == "declaration" and not has_error(parsed):
						declared: Node = items[0]
						work: list[Node] = [declared]
						while work:
							part = work.pop()
							start, end = part.start, part.end
							part.start: int = node.start + (start + len(name) - 3 if start > 2 else start)
							part.end: int = node.start + (end + len(name) - 3 if end > 2 else end)
							part.source: str|None = text
							if lang == "cpp" and part.type == "*" and part.text == "&":
								part.type: str = "&"
							if lang == "cpp" and part.type == "pointer_declarator" and part.text.lstrip().startswith("&"):
								part.type: str = "reference_declarator"
							if not start and part.type == "primitive_type":
								part.type: str = "primitive_type" if is_builtin else "type_identifier"
							work.extend(part.children)
						node.type: str = "declaration"
						node.children: list[Node] = declared.children
						changed = True
		if node.type == "declaration":
			for child in node.children:
				if child.field != "declarator":
					continue
				decl = child
				while decl is not None and decl.type != "identifier":
					decl = next(
						(part for part in decl.children if part.field == "declarator" or part.kind == "N" and is_paren_wrapper(decl.type)),
						None
					)
				if decl is not None:
					scope[decl.text] = False
		for child in reversed(node.children):
			child_scope: dict[str, bool] = scope
			if node.type == "for_range_loop" and child.field == "body":
				child_scope = dict(scope)
				work = [part for part in node.children if part.field == "declarator"]
				while work:
					part = work.pop()
					if part.type == "identifier":
						child_scope[part.text] = False
					work.extend(part.children)
			pending.append((child, child_scope))
	# 再投影した事を呼出元へ伝え，正規形と共有事実も作り直す
	return list(roots) if changed else roots

def comparison_roots(lang: str, roots: list[Node]) -> list[Node]:
	"""C / C++ の翻訳段階の行連結を独立した比較にも適用する
	計算量：ソース長 N と必要時の再解析費用 P に対して O(N+P)
	引数：
		lang: 対象言語
		roots: 物理ソースから得た木
	戻値：同じ入力の論理行から得た木．変更が無ければ元の木
	送出例外：
		OSError: パーサーを起動出来ない場合
		RuntimeError: パーサーが失敗した場合
		ValueError: パーサーの位置表が不正な場合
		json.JSONDecodeError: パーサーの JSON が不正な場合
		subprocess.TimeoutExpired: パーサーが制限時間内に終了しない場合
	"""
	if lang == "ruby":
		# Ruby のコメントとデータの境界を適応した木の返戻
		return ruby_comparison_roots(roots)
	if lang == "java":
		text: str = "".join(root.text for root in roots)
		if "\\u" not in text and "\r" not in text:
			# 展開又は改行の変換を必要としない木の返戻
			return roots
		origins: list[int] = []
		logical: str = java_unicode_source(text, origins)
		if "\r" in logical:
			positions: list[int] = [
				idx for idx, char in enumerate(logical) if char != "\r" or idx + 1 == len(logical) or logical[idx + 1] != "\n"
			]
			logical = "".join("\n" if logical[idx] == "\r" else logical[idx] for idx in positions)
			origins = [*(origins[idx] for idx in positions), len(text)]
		if logical == text:
			# 翻訳段階で境界と本文が変わらない入力の返戻
			return roots
		result: list[Node] = parse_dump(dump_ast(logical, "java"), logical)
		pending: list[Node] = list(result)
		while pending:
			node: Node = pending.pop()
			if is_comment(node.type) and node.text.startswith("/**") and not node.text.startswith("/***"):
				raw: str = "/**" + text[origins[node.start + 3]:origins[node.end - 2]] + "*/"
				# 物理行の装飾を先に読み，エスケープが作るコード内の改行と区別する
				node.comment_text: str|None = java_document_body(raw)
			pending.extend(node.children)
		# Java の字句解析に先行する変換を適用した木の返戻
		return result
	if lang in ("css", "scss"):
		# CSS の文字列と URL の境界を適応した木の返戻
		return css_comparison_roots(lang, roots)
	if lang not in ("c", "cpp", "h"):
		# 翻訳段階の異なる言語の返戻
		return roots
	text: str = "".join(root.text for root in roots)
	if "\\" not in text and "\r" not in text and "#" not in text and "%:" not in text:
		# 行連結も物理改行の変更も無い木の返戻
		return c_binding_roots(lang, roots)
	logical: str = cpp_directive_source(cpp_comment_source(text))
	# 空のディレクティブは処理を持たない
	# 文字列中の # は対象にしない
	mask: str = "".join(
		re.sub(r"[^\r\n]", " " if token.kind == "comment" else "x", token.text) if
		token.kind in ("comment", "literal") else
		token.text for token in cpp_phase_tokens(logical)
	)
	chars: list[str] = list(logical)
	for match in re.finditer(r"(?m)^[ \t]*#[ \t]*(?=\r?$)", mask):
		chars[mask.index("#", match.start(), match.end())] = " "
	logical = "".join(chars)
	# 物理ソースと異なる場合だけ独立に読み直す木の返戻
	return c_binding_roots(
		lang,
		c_preprocessor_roots(
			lang,
			logical,
			parse_dump(dump_ast(logical, "cpp" if lang == "h" else lang), logical) if logical != text else roots
		)
	)

def cpp_semantic_parens(text: str) -> tuple[tuple[str, ...], ...]:
	"""マクロの結合と型の取得・推論を保護する括弧の中身を構文から収集する
	変種のアンカー検査でも，意味比較と同じ所属・型の判定を用いる
	計算量：解析・束縛の修復費用 P・ノード数 N・保護する式の総文字数 B に対して O(P+N+B)
	引数：
		text: C++ のソース
	戻値：
		意味を持つ最外の括弧毎の，有意トークンの並び
	送出例外：
		OSError: パーサーの起動が失敗する場合
		RuntimeError: パーサーが失敗するか，意味を持つ括弧の中身を確認出来ない場合
		ValueError: パーサーの出力が不正な場合
		json.JSONDecodeError: ノード値が不正な場合
		subprocess.TimeoutExpired: パーサーが時間の上限を越える場合
	"""
	# 式と誤読された宣言子を，意味比較と同じ入力の束縛から確定する
	roots: list[Node] = comparison_roots("cpp", parse_dump(dump_ast(text, "cpp"), text))
	facts: TreeFacts = TreeFacts(roots)
	outer: Ancestors = Ancestors()
	outer.facts: TreeFacts|None = facts
	node: Node
	ancestors: Ancestors
	pending: list[tuple[Node, Ancestors]] = [(root, outer) for root in reversed(roots)]
	found: list[tuple[str, ...]] = []
	while pending:
		node, ancestors = pending.pop()
		if is_paren_wrapper(node.type) and has_semantic_parens(ancestors, node, "cpp"):
			inner: Node|None = facts.inner[node]
			if inner is None:
				raise RuntimeError("semantic parentheses have no inner expression")
			found.append(tuple(token.text for token in cpp_tokens(inner.text) if token.kind not in ("comment", "space")))
		context: Ancestors = Ancestors(ancestors, node, facts.named[node])
		pending.extend((child, context) for child in reversed(node.children))
	# 余分な重なりと空白に依存しない保護の記録の返戻
	return tuple(found)

def html_void_element(ancestors: Ancestors, node: Node) -> bool:
	"""HTML の名前空間の空要素かを確認する
	計算量：開タグの子数 C とタグ名の長さ B に対して O(C+B)
	引数：
		ancestors: 名前空間の境界を共有する祖先の文脈
		node: 開タグ又は其れを持つ要素
	戻値：HTML の空要素なら真，外来内容なら偽
	"""
	start: Node = node.children[0] if node.type == "element" and node.children else node
	tag: str = next((child.text.lower() for child in start.children if child.type == "tag_name"), "")
	# 公式の空要素を，SVG・MathML の同名のタグと区別した結果の返戻
	return (
		ancestors.html_namespace in ("html", "math_text") and
		tag in ("area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr")
	)

def normalize_work(node: Node, lang: str = "", ancestors: Ancestors = Ancestors()) -> Work:
	"""子の結果を要求しながらノードを意味正規形へ落とす

	交差型は二項構造と簡約の境界を保ち，合併型の連続部分は最外で１度だけ収集する
	計算量：O(n + b)（n は対象部分木のノード数，b は結果の文字数．型演算の子の収集の総数は O(n)）

	引数：
		node: 正規化するノード
		lang: 言語キー
		ancestors: 外側から並べた祖先ノード

	戻値：
		透過要素を除いた正規形ノードの一覧
	"""
	# 制御見出の役割を，ラッパーと文終端の透過より先に確定する
	node_type: str = node.type
	if (
		not node.children and node_type in ("module", "program", "source_file", "stylesheet", "translation_unit") and
		not node.text.strip()
	):
		# 空の構文の正規形の返戻
		return [(node_type, None, ())]
	if lang in ("typescript", "tsx") and node_type == "union_type":
		# 合併だけは同じ種類の並びを１度収集し，交差・関数・条件・配列型等を越えない
		parts: list[NormNode] = []
		pending: list[tuple[Node, Ancestors]] = [(node, ancestors)]
		while pending:
			part, parents = pending.pop()
			facts: TreeFacts = parents.get_facts()
			if part.type == "union_type":
				inner_parents: Ancestors = Ancestors(parents, part)
				pending.extend((child, inner_parents) for child in reversed(part.children))
			elif part.type == "parenthesized_type" and (inner := facts.inner[part]) is not None and inner.type == "union_type":
				pending.append((facts.named[part][0], Ancestors(parents, part)))
			else:
				parts.extend((yield normalize_work(part, lang, parents)))
		if parts and parts[0][0] == "|":
			# 型の先頭の区切は項を増やさず，項間の演算子は保持する
			parts.pop(0)
		if len(parts) == 1:
			# 単独の型の返戻
			return parts
		# 項と演算子の順を保持した合併型の返戻
		return [(node_type, None, tuple(parts))]
	if lang == "swift" and node_type in SWIFT_BINARY:
		children: list[Node] = [child for child in node.children if not is_comment(child.type)]
		if len(children) == 3 and children[1].text in SWIFT_PRECEDENCE:
			# 標準の優先順位で組み立てた式の返戻
			return (yield swift_binary_work(ancestors, node))
	if lang in ("c", "cpp") and node_type == "number_literal":
		if literal := boolean_initializer(lang, ancestors, node):
			# 型と値を確認した真偽値の返戻
			return [(literal, literal, ())]
	if lang == "ruby" and node_type == "binary" and len(node.children) == 3:
		left, operator, right = node.children
		if (
			left.type == "identifier" and operator.type in ("+", "-") and left.end < operator.start and operator.end == right.start and
			not ruby_binding_before(ancestors, node, left.text)
		):
			# 未導入の名前と密着した符号付の引数は，二項演算でなくコマンド呼出である
			values: list[NormNode] = (yield normalize_work(right, lang, Ancestors(ancestors, node)))
			if len(values) == 1:
				# 符号を引数へ保持したコマンド呼出の返戻
				return [("call", None, (("identifier", left.text, ()), ("unary", None, ((operator.type, operator.text, ()), values[0]))))]
	if lang == "ruby" and node_type == "call" and len(node.children) == 2:
		left, arguments = node.children
		parts: list[Node] = Node.get_named_children(arguments)
		if left.type == "identifier" and left.field == "method" and arguments.type == "argument_list" and len(parts) == 1:
			argument: Node = parts[0]
			if argument.type == "unary" and len(argument.children) == 2:
				operator, right = argument.children
				if (
					operator.type in ("+", "-") and arguments.start == operator.start and left.end < operator.start and
					operator.end == right.start and ruby_binding_before(ancestors, node, left.text)
				):
					values = (yield normalize_work(right, lang, Ancestors(Ancestors(Ancestors(ancestors, node), arguments), argument)))
					if len(values) == 1:
						# 先行するローカル変数と密着符号を，実処理系と同じ二項演算へ投影する
						return [("binary", None, (("identifier", left.text, ()), (operator.type, operator.text, ()), values[0]))]
	if lang == "php":
		if node_type == "function_call_expression":
			parts: list[Node] = Node.get_named_children(node)
			if (
				parts and parts[0].type == "name" and parts[0].text.lower() in ("die", "exit") and
				all(Node.is_empty_list(part, ancestors.empty_lists) for part in parts[1:])
			):
				# 引数の無い終了構文の返戻
				return [("empty_exit", parts[0].text.lower(), ())]
		if node_type == "name" and node.text.lower() in ("die", "exit") and ancestors and ancestors[-1].type == "expression_statement":
			# 丸括弧を省いた終了構文の返戻
			return [("empty_exit", node.text.lower(), ())]
	if lang == "go" and node_type == "true" and go_true_bound(ancestors, node):
		# 組込リテラルと，入力で同じ名前を導入した値を区別する
		return [("identifier", node.text, ())]
	if lang in ("c", "cpp", "h") and node_type in ("identifier", "true") and node.text == "true":
		# マクロの状態を確認した定数だけを，反復の条件除去へ渡す
		return [("true" if c_constant_true(lang, ancestors, node) else "identifier", node.text, ())]
	if node_type in ("protected_error", "protected_php_tail", "raw_text", "uninterpreted"):
		# 保護する未解析データの返戻
		return [(node_type, node.text, ())]
	if node_type in ("hash_bang_line", "shebang"):
		# 開始位置と実行対象はディレクティブの照合で保ち，必須でない区切空白だけを揃える
		return [(node_type, "#!" + node.text[2:].lstrip(" \t"), ())]
	if (
		lang == "csharp" and is_paren_wrapper(node_type) and any(child.type in ("base", "this") for child in node.children) and
		not Node.get_named_children(node)
	):
		# 受手のキーワードは無名トークンとして記録される
		return (yield from normalize_children(node.children, lang, Ancestors(ancestors, node)))
	if lang == "csharp" and node_type == "implicit_parameter":
		# 単一の型注記の無いラムダ仮引数を，括弧の内外で同じ名前として比較する
		return [("identifier", node.text, ())]
	if lang == "csharp" and node_type == "cast_expression":
		cast_parts: list[Node] = Node.get_named_children(node)
		if (
			len(cast_parts) == 2 and cast_parts[0].type == "identifier" and cast_parts[1].type == "prefix_unary_expression" and
			cast_parts[1].children[0].type in ("&", "*", "+", "-", "^") and
			(cast_parts[1].children[0].type in ("+", "-") or csharp_parameter_name(ancestors, cast_parts[0].text))
		):
			# 名前だけの括弧に続く単項の正負は，型変換でなく二項演算として読む
			parts = yield from normalize_children([cast_parts[0], *cast_parts[1].children], lang, Ancestors(ancestors, node))
			# 値の括弧と正負の演算を保持した結果の返戻
			return [("binary_expression", None, tuple(parts))]
	if lang == "csharp" and node_type == "cast_expression":
		parts: list[Node] = Node.get_named_children(node)
		if (
			len(parts) == 2 and parts[0].type == "array_type" and parts[1].type == "prefix_unary_expression" and
			parts[1].children[0].type in ("&", "*", "+", "-", "^")
		):
			type_parts: list[Node] = Node.get_named_children(parts[0])
			if (
				len(type_parts) == 2 and type_parts[0].type == "identifier" and type_parts[1].type == "array_rank_specifier" and
				Node.get_named_children(type_parts[1])
			):
				left: list[NormNode] = yield normalize_work(type_parts[0], lang, Ancestors(ancestors, parts[0]))
				indices: (
					list[NormNode]
				) = yield from normalize_children(Node.get_named_children(type_parts[1]), lang, Ancestors(ancestors, type_parts[1]))
				right: list[NormNode] = yield from normalize_children(parts[1].children, lang, Ancestors(ancestors, parts[1]))
				index: NormNode = (
					"bracketed_argument_list",
					None,
					(("[", "[", ()), *(("argument", None, (value,)) for value in indices), ("]", "]", ()))
				)
				# 添字の値と二項演算を保持した式の返戻
				return [("binary_expression", None, (("element_access_expression", None, (*left, index)), *right))]
	if lang == "csharp" and node_type == "declaration_expression" and ancestors and ancestors[-1].type == "argument":
		parts = Node.get_named_children(node)
		if (
			len(parts) == 2 and parts[0].type == "pointer_type" and
			not any(child.type in ("in", "out", "ref") for child in ancestors[-1].children)
		):
			values: list[NormNode] = yield from normalize_children([*parts[0].children, parts[1]], lang, Ancestors(ancestors, node))
			# 実引数中の積を保持した式の返戻
			return [("binary_expression", None, tuple(values))]
	if (
		lang == "java" and node_type == "annotation" and
		all(child.type != "annotation_argument_list" or not Node.get_named_children(child) for child in node.children)
	):
		# 空の引数だけを省けるアノテーションの返戻
		parts = yield from normalize_children(node.children, lang, Ancestors(ancestors, node))
		# 空の引数を省いたアノテーションの返戻
		return [("marker_annotation", None, tuple(parts))]
	if (
		lang == "kotlin" and node_type == "constructor_invocation" and ancestors and
		ancestors[-1].type in ("annotation", "unescaped_annotation")
	):
		parts = yield from normalize_children(node.children, lang, Ancestors(ancestors, node))
		if len(parts) == 1 and parts[0][0] == "user_type":
			# 空の実引数だけを除いたアノテーションの返戻
			return parts
	if lang == "swift" and node_type == "metatype":
		# 括弧付のメタタイプの鎖を１度辿り，型の接頭部を祖先毎に複写しない
		tails: list[str] = []
		part: Node = node
		parents: Ancestors = ancestors
		while True:
			children: list[Node] = [child for child in part.children if not is_comment(child.type)]
			if part.type == "metatype" and len(children) == 3 and children[1].type == "." and children[2].type in ("Protocol", "Type"):
				tails.append(children[2].type)
				parents = Ancestors(parents, part)
				part = children[0]
			elif tails and part.type == "tuple_type" and not Node.has_comma(part) and len(names := Node.get_named_children(part)) == 1:
				inner: list[Node] = Node.get_named_children(names[0]) if names[0].type == "tuple_type_item" else names
				if len(inner) != 1:
					break
				parents = Ancestors(parents, part)
				part = inner[0]
			else:
				break
		if tails:
			parts: list[NormNode] = yield normalize_work(part, lang, parents)
			if len(parts) == 1 and parts[0][0] == "user_type":
				values: list[NormNode] = list(parts[0][2])
				for mark in reversed(tails):
					values.extend(((".", ".", ()), ("type_identifier", mark, ())))
				# 型の名前とメタタイプの末尾を同順で保持した列の返戻
				return [("user_type", None, tuple(values))]
			for mark in reversed(tails):
				parts = [(node_type, None, (*parts, (".", ".", ()), (mark, mark, ())))]
			# 名前以外の基底型の構造を保持したメタタイプの返戻
			return parts
		# 不完全なメタタイプの子を通常の比較へ渡す
		parts = yield from normalize_children(node.children, lang, Ancestors(ancestors, node))
		# 読み取った構造を保持したメタタイプの返戻
		return [(node_type, None, tuple(parts))]
	if lang == "ruby" and node_type == "heredoc_body":
		# 遅延して現れる本文は，開始順の逐語照合で１度だけ比べる
		return []
	if lang in ("css", "scss") and node_type == "class_selector" and node.source is not None:
		dot: int = next((idx for idx, child in enumerate(node.children) if child.type == "."), -1)
		gap: str = node.source[node.children[0].end:node.children[dot].start] if dot != -1 and bool(dot) else ""
		if "/*" in gap and (lang == "scss" or re.search(r"[ \t\r\n\f]", re.sub(r"/\*[\s\S]*?\*/", "", gap))):
			prefix: tuple[NormNode, ...] = tuple((yield normalize_work(node.children[0], lang, Ancestors(ancestors, node))))
			suffix: tuple[NormNode, ...] = tuple((yield from normalize_children(node.children[dot:], lang, Ancestors(ancestors, node))))
			# コメント付の子孫セレクタの返戻
			return [("descendant_selector", None, (*prefix, ("class_selector", None, suffix)))]
	if lang == "scss" and node_type == "binary_expression":
		parts: list[Node] = [child for child in node.children if not is_comment(child.type)]
		if len(parts) == 3 and not parts[0].text and parts[1].text in ("+", "-") and parts[2].type in ("float_value", "integer_value"):
			# 符号と単位を保持した数値の返戻
			return [("css_number", numeric_spelling(parts[1].text + parts[2].text), ())]
	if lang in ("css", "scss"):
		if node_type == "declaration":
			property_name: Node|None = next((child for child in node.children if child.type == "property_name"), None)
			if property_name is not None and property_name.text.startswith("--"):
				body: str = node.text[node.text.find(":") + 1:].removesuffix(";")
				if not node.text.endswith(";") and node.source is not None:
					body += re.match(r"[ \t\r\n\f]*", node.source[node.end:])[0]
				if not body.strip():
					# 空白だけの値は，空の値と区別して保持する
					return [("custom_property_empty", property_name.text + (" " if body else ""), ())]
		if node_type in ("integer_value", "float_value"):
			# 単位だけが子に記録される数値も，本体の値を保持する
			spelling: str = re.sub(r"(?:/\*[\s\S]*?\*/)+", "/**/", node.text)
			number: re.Match[str]|None = re.fullmatch(r"([+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?)(.*)", spelling)
			# 数値と単位を保持した正規形の返戻
			return [("css_number", numeric_spelling(number.group(1)) + number.group(2) if number else spelling, ())]
		if node_type in ("identifier", "plain_value"):
			# コメントの有無に依るパーサーの種別の違いを吸収する
			return [("css_identifier", css_identifier(node.text), ())]
		if node_type == "color_value":
			# セレクタの ID と異なる色の桁の表記の返戻
			return [(node_type, node.text.upper(), ())]
	if node_type.startswith("embedded:"):
		inner_lang: str = node_type.partition(":")[2]
		inner_roots: list[Node] = comparison_roots(inner_lang, node.children)
		# 子のディレクティブと内容も，単独入口が比べる正規形へ含める
		value = json.dumps(
			(
				forest_sexpr(inner_roots, inner_lang),
				directive_signature(node.children, inner_lang),
				content_signature(inner_roots),
				node.text.lstrip(" \t\r\n").startswith("\ufeff")
			),
			ensure_ascii=False
		)
		# 単独入口と同じ子の比較結果の返戻
		return [(node_type, value, ())]
	if lang == "rust" and (ancestors.is_in_macro_tokens or node_type in RUST_TOKEN_TREES):
		if is_comment(node_type):
			# コメントをトークンの綴りに含めない空の列の返戻
			return []
		# 入子のトークン列・パターンにも保護を伝え，通常の数値・括弧・区切の正規化を適用しない
		macro_children: list[NormNode] = (yield from normalize_children(node.children, lang, Ancestors(ancestors, node)))
		# 保護したトークンの構造と綴りの返戻
		return [(node_type, None if macro_children else node.text, tuple(macro_children))]
	if lang in ("javascript", "typescript", "tsx") and node_type == "class_body":
		items: list[NormNode] = []
		children: list[Node] = [child for child in node.children if not is_comment(child.type)]
		pos: int = 0
		while pos < len(children):
			child: Node = children[pos]
			pos += 1
			parts: list[Node] = [part for part in child.children if not is_comment(part.type)]
			if (
				child.type == "public_field_definition" and len(parts) == 1 and parts[0].type == "property_identifier" and
				parts[0].text in ("get", "set", "static") and pos < len(children)
			):
				following: Node = children[pos]
				gap: str = node.source[child.end:following.start] if node.source is not None else ";"
				is_modifier: bool = (
					following.type == "method_definition" or
					parts[0].text == "static" and following.type in ("public_field_definition", "static_block")
				)
				if is_modifier and not any(char == ";" for token in cpp_tokens(gap) if token.kind != "comment" for char in token.text):
					values: list[NormNode] = (yield normalize_work(following, lang, Ancestors(ancestors, node)))
					if len(values) == 1:
						mark: str = parts[0].text
						items.append((values[0][0], values[0][1], ((mark, mark, ()),) + values[0][2]))
						pos += 1
						continue
			items.extend((yield normalize_work(child, lang, Ancestors(ancestors, node))))
		# 改行を跨ぐ修飾子と，明示の文終端を区別したメンバ列の返戻
		return [(node_type, None, tuple(items))]
	if (
		lang in ("javascript", "typescript", "tsx") and node_type == "jsx_element" and node.source is not None and
		len(node.children) > 1
	):
		# 本文と式の境界を先に確定し，コメントの移動から本文の値を推定しない
		return (yield jsx_children(lang, Ancestors(ancestors, node), node))
	if node_type == "string" and ancestors and ancestors[-1].type == "jsx_attribute":
		# JSX 属性のバックスラッシュはエスケープでなく本文
		return [("string_value", repr(unescape(node.text[1:-1])), ())]
	if (
		lang in ("javascript", "typescript", "tsx") and ancestors and
		ancestors[-1].type in ("field_definition", "method_definition", "pair", "public_field_definition")
	):
		parent: Node = ancestors[-1]
		name: Node|None = (
			next((child for child in parent.children if child.type in ("computed_property_name", "property_identifier", "string")), None)
		)
		if node is name and node_type in ("property_identifier", "string"):
			value: str|None = repr(node.text) if node_type == "property_identifier" else quoted_value(lang, node)
			if value is not None:
				# 評価する計算名と異なる，固定のプロパティ名の返戻
				return [("property_key", value, ())]
	if (value := quoted_value(lang, node)) is not None:
		if lang in ARRAY_LANGS and node in ancestors.get_facts().directive_strings:
			# 値が同じでも，エスケープ又は行継続を持つ記法は厳格モードを有効にしない
			return [("directive_string", repr((value, node.text[1:-1] == "use strict")), ())]
		# 引用符の選択と，エスケープ断片の分割を値として照合する
		return [("string_value", value, ())]
	if (header := control_header_parts(node)) is not None:
		roles: tuple[str, ...]
		parts: list[list[Node]]
		prefix: list[Node]
		suffix: list[Node]
		roles, parts, prefix, suffix = header
		header_ancestors: Ancestors = Ancestors(ancestors, node)
		children: list[NormNode] = (yield from normalize_children(prefix, lang, header_ancestors))
		# 見出の評価順と空の役割を保つ
		for role, part in zip(roles, parts):
			children.append((role, None, tuple((yield from normalize_children(part, lang, header_ancestors)))))
		children.extend((yield from normalize_children(suffix, lang, header_ancestors)))
		# 省略可能な文終端を透過しても各位置の役割は保った正規形の返戻
		return [loop_form(lang, (node_type, None, tuple(children)))]
	# 文の所属を親へ残し，継続記号・任意の区切・通常コメントを除く
	if (
		lang == "python" and node_type == "line_continuation" or lang == "ruby" and node.kind == "U" and node_type == "then" or
		is_comment(node_type) or node.kind == "U" and (
			(stripped := node.text.strip()) == "" or (not ancestors or ancestors[-1].type not in RUST_TOKEN_TREES) and (
				stripped in ("(", ")", ";") or stripped == "," and
				(not ancestors or not (ancestors[-1].type == "block_parameters" and len(Node.get_named_children(ancestors[-1])) == 1))
			)
		)
	):
		# 意味に無関係なコメント・空白・区切・括弧を除いた空の並びの返戻
		return []
	if lang in ARRAY_LANGS and node_type in ARRAY_LISTS:
		# 要素を伴わないカンマ毎の空要素の保持
		items: list[NormNode] = []
		has_element: bool = False
		for child in node.children:
			if child.kind == "N" and not is_comment(child.type):
				items.extend((yield normalize_work(child, lang, Ancestors(ancestors, node))))
				has_element = True
			elif child.kind == "U" and child.text == ",":
				if not has_element:
					items.append(("array_hole", None, ()))
				has_element = False
		# 空要素の位置と数を保持した配列の返戻
		return [(node_type, None, tuple(items))]
	# 空の実引数と冗長な括弧の除去
	named: list[Node] = ancestors.facts.named[node] if ancestors.facts is not None else Node.get_named_children(node)
	if (
		len(named) == 1 and (is_paren_wrapper(node_type) or lang == "python" and node_type in PYTHON_TUPLES) and
		has_semantic_parens(ancestors, node, lang)
	):
		# 意味の区別を１重だけ残して内側の冗長なラッパーを除いた正規形の返戻
		return [("semantic_parens", None, tuple((yield normalize_work(named[0], lang, Ancestors(ancestors, node)))))]
	if lang == "cpp" and node_type == "parameter_list" and len(named) == 1 and named[0].text.strip() == "void":
		if (
			ancestors and ancestors[-1].type in ("abstract_function_declarator", "function_declarator") and
			all(child.kind == "N" or child.text.strip() in ("", "(", ")") for child in node.children)
		):
			# void が単独の関数の仮引数列は，空列の存在を保持して対応付ける
			return [(node_type, None, ())]
	elif (
		node.kind == "N" and ancestors and ancestors[-1].type in EMPTY_LIST_OWNERS.get(node_type, ()) and
		Node.is_empty_list(node, ancestors.empty_lists)
	):
		# 空の内容を同値に扱える所有構文だけで，列の表現を透過する
		return []
	if (
		len(named) == 1 and
		(is_paren_wrapper(node_type) or lang == "swift" and node_type == "tuple_expression" and not Node.has_comma(node))
	):
		# 所属を保持して単一式の冗長括弧を剥がした中身の返戻
		return (yield normalize_work(named[0], lang, Ancestors(ancestors, node)))
	if (
		node_type == "tuple_type" and len(named) == 1 and not Node.has_comma(node) and
		len((inner := Node.get_named_children(named[0]) if named[0].type == "tuple_type_item" else [named[0]])) == 1
	):
		# 要素１つの組の型を型を包む括弧と見た中身の返戻
		return (yield normalize_work(inner[0], lang, ancestors))
	if lang == "swift" and node_type == "optional_type" and len(named) == 1:
		# パーサーが `??` を１つのトークンに読む為，ネストの省略可能の型をマーカーの数へ畳む（`(Int?)?` は `Int??`）
		marks: int = sum(child.text.count("?") for child in node.children if child.kind == "U")
		base: list[NormNode] = (yield normalize_work(named[0], lang, Ancestors(ancestors, node)))
		if len(base) == 1 and base[0][0].startswith("optional_type:"):
			marks += int(base[0][0].split(":")[1])
			base = list(base[0][2])
		# マーカーの数を型名に持つ正規形の返戻
		return [(f"optional_type:{marks}", None, tuple(base))]
	if (
		node_type in PARAM_LISTS and len(named) == 1 and
		len((inner := Node.get_named_children(named[0]) if named[0].type in PARAM_WRAPPERS else [named[0]])) == 1 and
		inner[0].type == "identifier"
	):
		# 名前だけの仮引数１つの並びを名前と同じと見た中身の返戻
		return (yield normalize_work(inner[0], lang, ancestors))
	if lang == "python" and node_type in PYTHON_TUPLES:
		if len(named) == 1 and not Node.has_comma(node):
			# カンマの無い要素１つの組を名前を包む括弧と見た中身の返戻
			return (yield normalize_work(named[0], lang, ancestors))
		# 括弧の有無で変わる組の種別を揃えた正規形の返戻
		return [("tuple", None, tuple((yield from normalize_children(named, lang, Ancestors(ancestors, node)))))]
	if lang == "python" and node_type == "subscript" and len(named) > 1 and (len(named) > 2 or Node.has_comma(node)):
		# 添字に並べた要素 (`d[a, b]`) は組の添字 (`d[(a, b)]`) と同じ
		indices: tuple[NormNode, ...] = tuple((yield from normalize_children(named[1:], lang, Ancestors(ancestors, node))))
		receiver: tuple[NormNode, ...] = tuple((yield normalize_work(named[0], lang, Ancestors(ancestors, node))))
		# 組を添字に持つ正規形の返戻
		return [(node_type, None, receiver + (("[", "[", ()), ("tuple", None, indices), ("]", "]", ())))]
	if lang == "cpp" and node_type == "user_defined_literal" and named and named[0].type == "number_literal":
		literal: Node = named[0]
		if literal.text.startswith(("+", "-")):
			parts: tuple[NormNode, ...] = tuple((yield from normalize_children(named, lang, Ancestors(ancestors, node))))
			signed: NormNode = parts[0]
			# 接尾辞より外側で評価する符号の返戻
			return [("unary_expression", None, (signed[2][0], ("user_defined_literal", None, (signed[2][1], *parts[1:]))))]
	# 全ブロックの末尾値の破棄と，明示の返戻の違いを保持する
	if lang == "rust" and node_type == "block":
		body_children: list[NormNode] = []
		body_ancestors: Ancestors = Ancestors(ancestors, node)
		for child in node.children:
			value: Node = child
			value_ancestors: Ancestors = body_ancestors
			if named and child is named[-1]:
				if child.type == "expression_statement" and len(values := Node.get_named_children(child)) == 1:
					value = values[0]
					value_ancestors = Ancestors(body_ancestors, child)
				is_return: bool = value.type == "return_expression"
				is_jump: bool = is_return or value.type in ("break_expression", "continue_expression")
				if child.type == "expression_statement" and not is_jump:
					is_terminated: bool = any(part.kind == "U" and part.text == ";" for part in child.children)
					if is_terminated:
						# 値を捨てる末尾文を，値を返す末尾式と区別する
						body_children.append(("discarded_tail", None, ()))
			body_children.extend((yield normalize_work(value, lang, value_ancestors)))
		# 値の破棄と明示の返戻を保持した本体の返戻
		return [(node_type, None, tuple(body_children))]
	# 単一式本体と返戻の正規化
	if (
		lang == "kotlin" and node_type == "control_structure_body" and len(bodies := Node.get_named_children(node)) == 1 and
		bodies[0].type == "statements" and len(vals := Node.get_named_children(bodies[0])) == 1
	):
		head: Node = vals[0]
		while head.type == "prefix_expression" and head.children and head.children[0].type in ("annotation", "label"):
			head = head.children[-1]
		if not head.type.endswith("_declaration") and not head.text.lstrip().startswith("{"):
			# 単一式本体のラッパーだけを吸収したノードの返戻（宣言のスコープとラムダの実行境界は保持する）
			return [(node_type, None, tuple((yield normalize_work(vals[0], lang, Ancestors(ancestors, node)))))]
	if lang == "ruby" and node_type == "return" and ancestors:
		parent: Node = ancestors[-1]
		is_method_tail: bool = (
			parent.type == "body_statement" and len(ancestors) > 1 and ancestors[-2].type in ("method", "singleton_method")
		)
		is_rescue_tail: bool = (
			parent.type == "then" and len(ancestors) > 3 and ancestors[-2].type == "rescue" and ancestors[-3].type == "body_statement" and
			ancestors[-4].type in ("method", "singleton_method")
		)
		named = [child for child in node.children if child.kind == "N"]
		if (
			(is_method_tail or is_rescue_tail) and ancestors.facts is not None and ancestors.facts.ruby_tail[parent] is node and
			len(named) == 1 and (arg := named[0]).type == "argument_list" and
			len(vals := [child for child in arg.children if child.kind == "N"]) == 1
		):
			# メソッド末尾の単一返戻値の正規形の返戻
			return (yield normalize_work(vals[0], lang, ancestors))
	if (
		node_type in TRANSPARENT_WRAPPERS or
		lang in OPTIONAL_BODY_LANGS and node_type in BODY_BLOCKS and ancestors and ancestors[-1].type in CONTROL_BODIES and
		len(named) == 1 and ancestors.facts is not None and
		(not ancestors.facts.binding[named[0]] or lang == "csharp" and named[0].type == "if_statement")
	):
		# 意味に無関係なラッパーと，束縛を導入しない単一文本体を透過した子列の返戻
		return (
			yield from normalize_children([child for child in node.children if child.kind == "N"], lang, Ancestors(ancestors, node))
		)
	if lang in ("c", "cpp") and node_type == "number_literal" and re.fullmatch(r"[+-](?:[0-9]|\.).*", node.text):
		# 符号は演算として残し，符号と数値の間の空白を値の差にしない
		mark: str = node.text[0]
		# 符号の演算を保持した数値の返戻
		return [("unary_expression", None, ((mark, mark, ()), ("number_literal", numeric_spelling(node.text[1:]), ())))]
	if lang in ("c", "cpp", "h") and node_type == "preproc_arg":
		# 引用・演算子・トークン境界を保持し，通常コメントだけを除く
		return [(node_type, repr(cpp_token_signature([Node(node.text, "N", "translation_unit")])), ())]
	# 中間ノードと内容葉の正規形の構築
	out_children: list[NormNode] = (yield from normalize_children(node.children, lang, Ancestors(ancestors, node, named)))
	if lang in ("typescript", "tsx") and node_type == "intersection_type" and out_children and out_children[0][0] == "&":
		# 交差型も先頭の区切だけを除き，項のグループを保持する
		out_children.pop(0)
		if len(out_children) == 1:
			# １項の交差は同じ型の返戻
			return out_children
	if (
		lang == "csharp" and node_type == "binary_expression" and len(out_children) == 3 and
		node.children[-1].type == "cast_expression" and out_children[-1][0] == "binary_expression"
	):
		parts: tuple[NormNode, ...] = out_children[-1][2]
		priority: dict[str, int] = { "%": 3, "&": 0, "*": 3, "+": 2, "-": 2, "/": 3 }
		if (
			len(parts) == 3 and out_children[1][0] in priority and parts[1][0] in priority and
			priority[out_children[1][0]] >= priority[parts[1][0]]
		):
			# 左結合の優先順位を保った式の返戻
			return [("binary_expression", None, (("binary_expression", None, (*out_children[:2], parts[0])), *parts[1:]))]
	if (
		lang == "csharp" and node_type == "prefix_unary_expression" and len(out_children) == 2 and
		node.children[-1].type == "cast_expression" and out_children[-1][0] == "binary_expression"
	):
		parts: tuple[NormNode, ...] = out_children[-1][2]
		if len(parts) == 3:
			# 単項演算の対象を保った式の返戻
			return [("binary_expression", None, (("prefix_unary_expression", None, (out_children[0], parts[0])), *parts[1:]))]
	if lang == "swift" and node_type == "call_expression" and len(out_children) == 2 and out_children[1][0] == "call_suffix":
		arguments: tuple[NormNode, ...] = out_children[1][2]
		if len(arguments) == 1 and arguments[0][0] == "value_arguments" and len(arguments[0][2]) == 1:
			argument: NormNode = arguments[0][2][0]
			if argument[0] == "value_argument" and len(argument[2]) == 1:
				callee: NormNode = out_children[0]
				operator: NormNode = callee[2][-1] if callee[0] == "swift_binary" else callee
				if not operator[2] and operator[1] in ("!", "+", "-", "~"):
					prefix: NormNode = "prefix_expression", None, (("bang" if operator[1] == "!" else operator[1], operator[1], ()), argument[2][0])
					# 引数の括弧を持つ前置演算の返戻
					return [(callee[0], callee[1], (callee[2][0], prefix))] if callee[0] == "swift_binary" else [prefix]
	if lang == "kotlin" and node_type == "function_body":
		out_children = [part for child in out_children for part in (child[2] if child[0] == "statements" else (child,))]
	if lang == "ruby" and node_type == "right_assignment_list" and out_children and out_children[0][0] == "call":
		first: Node|None = next((child for child in node.children if child.kind == "N"), None)
		if first is not None and first.type == "binary" and out_children[0][2] and out_children[0][2][-1][0] == "unary":
			# 密着した符号から始まるコマンドの引数列を，１つの呼出へ戻す
			return [("call", None, (*out_children[0][2], *out_children[1:]))]
	if lang == "ruby" and node_type in ("if", "unless", "until", "while") and ruby_modifier_equivalent(ancestors, node):
		if len(out_children) == 4 and out_children[2][0] == "then" and len(out_children[2][2]) == 1:
			# 条件と導入の有効範囲を保つ後置条件の返戻
			return [(node_type + "_modifier", None, (out_children[2][2][0], out_children[0], out_children[1]))]
		if len(out_children) == 3 and node_type in ("until", "while"):
			# 評価順を保つ後置反復の返戻
			return [loop_form(lang, (node_type + "_modifier", None, (out_children[2], out_children[0], out_children[1])))]
	if (
		lang in ("kotlin", "swift") and node_type == "call_expression" and out_children and out_children[0][0] == "call_expression" and
		(len(out_children[0][2]) == 1 or out_children[0][2][1:] == (("call_suffix", None, ()),)) and
		not any(part.type == "value_arguments" for child in node.children[1:] for part in [child, *child.children]) and
		any(part.type in ("annotated_lambda", "lambda_literal") for child in node.children[1:] for part in [child, *child.children])
	):
		# クロージャ側に別の実引数が無い時だけ，同じ呼出の空の実引数を省く
		out_children[0] = out_children[0][2][0]
	if lang == "html" and node_type in ("self_closing_tag", "start_tag"):
		if html_void_element(ancestors, node):
			# HTML の空要素だけは閉じの斜線が構文上の子を持たない
			return [("start_tag", None, tuple((">", ">", ()) if child[0] == "/>" else child for child in out_children))]
	if lang == "html" and node_type == "element" and out_children and html_void_element(ancestors, node):
		# パーサーが空要素の子へ取り込んだ後続本文を，同じ階層の兄弟へ戻す
		return [("html_void", None, out_children[0][2]), *out_children[1:]]
	if node_type in ("statement_list", "statements", "then") and not out_children:
		# コメントと任意の区切だけの並びは空の本体と同じ
		return []
	if discarded_update(lang, ancestors, node):
		operators: list[NormNode] = [child for child in out_children if child[0] in ("++", "--")]
		operands: list[NormNode] = [child for child in out_children if child[0] not in ("++", "--")]
		if len(operators) == len(operands) == 1:
			# 評価対象と増減の向きを保持した未使用の更新値の返戻
			return [("discarded_update", None, tuple(operators + operands))]
	if (
		lang in ("typescript", "tsx") and node_type == "instantiation_expression" and len(out_children) == 2 and
		node.children[0].type == "unary_expression"
	):
		operand: NormNode = out_children[0]
		if len(operand[2]) == 2:
			# 型の実引数は単項演算の結果でなく，演算対象の参照へ結び付く
			return [("unary_expression", None, (operand[2][0], ("instantiation_expression", None, (operand[2][1], out_children[1]))))]
	if lang in ("javascript", "typescript", "tsx") and node_type == "binary_expression" and len(out_children) == 3:
		left, operator, right = out_children
		is_left_type: bool = left[0] == "unary_expression" and bool(left[2]) and left[2][0][0] == "typeof"
		is_right_type: bool = right[0] == "unary_expression" and bool(right[2]) and right[2][0][0] == "typeof"
		if (
			(is_left_type and (is_right_type or right[0] == "string_value") or is_right_type and left[0] == "string_value") and
			operator[0] in ("==", "!=")
		):
			mark: str = operator[0] + "="
			out_children[1] = mark, mark, ()
	# 中間ノードはテキスト不要
	# 数値表記は正規化し，内容葉と記号は厳密に保持する
	text: str|None = (
		None if
		out_children or node.children else
		numeric_spelling(node.text) if is_numeric_literal(node_type) or node_type == "real_literal" else node.text if
		is_content_leaf(node_type) else
		re.sub(r"\s+", " ", node.text).strip() if any(char.isalpha() for char in node.text) else node.text
	)
	# 正規形ノードの返戻
	return [loop_form(lang, (node_type, text, tuple(out_children)))]

def normalize(node: Node, lang: str = "", ancestors: Ancestors = Ancestors()) -> list[NormNode]:
	"""ノードを意味正規形へ落とす（付随情報は除去，括弧・構造ラッパは中身を昇格）

	交差型は二項構造と簡約の境界を保ち，合併型の連続部分は最外で１度だけ収集する
	計算量：O(n + b)（n は対象部分木のノード数，b は結果の文字数．型演算の子の収集の総数は O(n)）

	引数：
		node: 正規化するノード
		lang: 言語キー
		ancestors: 外側から並べた祖先ノード

	戻値：
		透過要素を除いた正規形ノードの一覧
	"""
	# 呼出毎に控えを分け，後の編集済の木へ以前の結果を持ち越さない
	if not ancestors:
		ancestors = Ancestors()
		ancestors.facts: TreeFacts|None = TreeFacts([node])
	elif ancestors.facts is None:
		ancestors.get_facts()
	# 明示した作業列による正規化の結果の返戻
	return evaluate(normalize_work(node, lang, ancestors))

def sexpr(norm: NormNode) -> str:
	"""正規形ノードを正準 S 式文字列へ変換する

	計算量：正規形のノード数 N と出力文字数 B に対して O(N+B)

	引数：
		norm: 正規形ノード

	戻値：
		正準 S 式
	"""
	# 子と閉記号を先行順で連結する断片の収集
	pending: list[NormNode|str] = [norm]
	parts: list[str] = []
	while pending:
		item: NormNode|str = pending.pop()
		if isinstance(item, str):
			parts.append(item)
			continue
		node_type: str
		text: str|None
		children: tuple[NormNode, ...]
		node_type, text, children = item
		if not children:
			parts.append(f"({node_type} {text!r})")
			continue
		parts.append(f"({node_type} ")
		pending.append(")")
		for idx in range(len(children) - 1, -1, -1):
			pending.append(children[idx])
			if idx:
				pending.append(" ")
	# 全断片を連結した正準形の返戻
	return "".join(parts)

def cpp_argument_ranges(text: str, work: list[int]) -> dict[int, list[tuple[int, int]]]:
	"""全ての丸括弧の直接のカンマ境界を１度収集する
	計算量：ソース長 L と生文字列数 R に対して O(L+R log(L+1))，本文は複写しない
	引数：
		text: 原文
		work: 読取文字数・完成した括弧対数・観測本文長の格納先
	戻値：開括弧位置毎の実引数範囲
	"""
	ranges: dict[int, list[tuple[int, int]]] = {}
	frames: list[tuple[int, int, list[tuple[int, int]]]] = []
	work[0] += len(text)
	for token in cpp_tokens(text):
		if token.kind != "punct":
			continue
		if token.text == "(":
			frames.append((token.start, token.end, []))
		elif frames and token.text in (",", ")"):
			opening, start, args = frames.pop()
			args.append((start, token.start))
			if token.text == ",":
				frames.append((opening, token.end, args))
			else:
				ranges[opening] = args
				work[1] += 1
	# 原文位置だけを持つ境界の返戻
	return ranges

def stringized_text(text: str) -> str:
	"""文字列化で観測するトークンと，内部の空白の有無を返す
	計算量：実引数の文字数 L に対して O(L)
	引数：
		text: 文字列化する実引数の原文
	戻値：
		端の空白を除き，連続する空白とコメントを１空白にしたテキスト
	"""
	# 翻訳段階の行連結と文字列化時の空白の集約
	parts: list[str] = []
	has_space: bool = False
	for token in cpp_tokens(text):
		if token.kind in ("comment", "space"):
			has_space = bool(parts)
			continue
		if has_space:
			parts.append(" ")
		parts.append(token.text)
		has_space = False
	# 文字列化で観測するテキストの返戻
	return "".join(parts)

MacroDefinition: TypeAlias = tuple[tuple[str, ...], str] | None
MacroState: TypeAlias = dict[str, frozenset[MacroDefinition]]

def initial_macro(name: str) -> frozenset[MacroDefinition]:
	"""標準のアサーションだけに既知の文字列化契約を与える
	引数：
		name: マクロ名
	戻値：
		初期の定義候補．登録の無い名前は `None`
	"""
	# 照合結果の返戻
	return frozenset({ (("value",), "#value") if name == "assert" else None })

class MacroCache:
	"""不変の置換トークンと，参照した定義だけに依存する観測結果を共有する"""

	def get_tokens(self: MacroCache, body: str) -> tuple[str, ...]:
		"""同じ置換本文を１度だけトークン化する
		計算量：未読の本文の長さ L に対して O(L)，共有済なら辞書照会だけ
		引数：
			self: ソース毎の共有結果
			body: 置換本文
		戻値：
			変更不能な有意トークン列
		"""
		if body not in self.tokens:
			self.tokens[body] = tuple(
				cpp_token_role(token.text) for token in cpp_phase_tokens(cpp_source(body).text) if token.kind not in ("comment", "space")
			)
			self.work[0] += 1
		# 共有した置換トークンの返戻
		return self.tokens[body]

	def invalidate(self: MacroCache, name: str) -> None:
		"""変更する名前から直接の依存辺だけを辿り，使用側へ失効を伝播する
		計算量：失効する状態数 S・直接の依存辺 E に対して O(S+E+1)
		引数：
			self: ソース毎の共有結果
			name: 定義の候補が変わる名前
		"""
		pending: list[str] = [name]
		seen: set[str] = set()
		invalid: set[tuple[str, int]] = set()
		while pending:
			dependency: str = pending.pop()
			if dependency in seen:
				continue
			seen.add(dependency)
			for key in self.users.get(dependency, ()):
				if key not in invalid:
					invalid.add(key)
					pending.append(key[0])
		for key in invalid:
			for dependency in self.dependencies.pop(key):
				users: set[tuple[str, int]] = self.users[dependency]
				users.remove(key)
				if not users:
					del self.users[dependency]
			for child in { (target, len(bindings)) for target, bindings in self.forwardings.pop(key) }:
				parents: set[tuple[str, int]] = self.parents[child]
				parents.discard(key)
				if not parents:
					del self.parents[child]
			del self.slots[key]
			self.work[2] += 1
		# 終了
		return

	def lookup(self: MacroCache, macros: MacroState, name: str, count: int) -> set[tuple[int, ...]]:
		"""直接の転送辺と解決済の下位結果を共有し，循環も増えた観測だけを伝播する
		計算量：新しい転送状態 S・直接の辺 E・観測の合成費用 P に対して O(S+E+P)
		引数：
			self: ソース毎の共有結果
			macros: 使用位置の定義候補
			name: 呼出名
			count: 実引数の数
		戻値：
			文字列化する実引数の番号列の集合（呼出側は変更しない）
		"""
		key: tuple[str, int] = name, count
		if key in self.slots:
			# 有効な下位の解決を再走査しない結果の返戻
			return self.slots[key]
		callee: str
		arity: int
		pending: list[tuple[str, int]] = [key]
		created: list[tuple[str, int]] = []
		while pending:
			current: tuple[str, int] = pending.pop()
			if current in self.slots:
				continue
			callee, arity = current
			forwardings: list[tuple[str, list[set[int]]]] = []
			dependencies: set[str] = { callee }
			self.slots[current] = observed_macro_slots(macros, callee, arity, self, dependencies, forwardings)
			self.forwardings[current] = forwardings
			created.append(current)
			for target, bindings in forwardings:
				child: tuple[str, int] = target, len(bindings)
				dependencies.add(target)
				self.parents.setdefault(child, set()).add(current)
				pending.append(child)
			self.dependencies[current] = dependencies
			for dependency in dependencies:
				self.users.setdefault(dependency, set()).add(current)
		# 各辺の束縛を合成し，変わった下位結果だけを直接の使用側へ通知する
		queue: deque[tuple[str, int]] = deque(reversed(created))
		queued: set[tuple[str, int]] = set(created)
		while queue:
			current = queue.popleft()
			queued.remove(current)
			found: set[tuple[int, ...]] = self.slots[current]
			previous: int = len(found)
			for target, bindings in self.forwardings[current]:
				for slots in tuple(self.slots[target, len(bindings)]):
					observed: set[int] = set().union(*(bindings[idx] for idx in slots if idx < len(bindings)))
					if observed:
						found.add(tuple(sorted(observed)))
			if len(found) != previous:
				for parent in self.parents.get(current, ()):
					if parent not in queued:
						queue.append(parent)
						queued.add(parent)
		# 使用位置で有効な観測結果の返戻
		return self.slots[key]

	def __init__(self: MacroCache) -> None:
		"""１つのソースのマクロの照会・依存・作業計数を初期化する
		引数：
			self: 初期化する共有結果
		"""
		self.tokens: dict[str, tuple[str, ...]] = {}
		self.slots: dict[tuple[str, int], set[tuple[int, ...]]] = {}
		self.dependencies: dict[tuple[str, int], set[str]] = {}
		self.forwardings: dict[tuple[str, int], list[tuple[str, list[set[int]]]]] = {}
		self.parents: dict[tuple[str, int], set[tuple[str, int]]] = {}
		self.users: dict[str, set[tuple[str, int]]] = {}
		self.work: list[int] = [0, 0, 0]
		# 終了
		return

def observed_macro_slots(
	macros: MacroState,
	name: str,
	count: int,
	cache: MacroCache|None = None,
	dependencies: set[str]|None = None,
	forwardings: list[tuple[str, list[set[int]]]]|None = None
) -> set[tuple[int, ...]]:
	"""使用位置で有効な定義と転送先から，文字列化する実引数の番号を得る
	計算量：転送状態数 S・置換本体の総文字数 L・実引数数 A に対して最悪 O(S(L+A+1)(A+1) log(A+2))
	引数：
		macros: 使用位置のマクロ定義（取消は `None`）
		name: 呼出名
		count: 呼出の実引数数
		cache: 共有するトークン列と作業計数
		dependencies: 実際に照会した名前の格納先
		forwardings: 直接の転送先の格納先．指定時は下位を此処で解決しない
	戻値：
		直接又は転送先で文字列化する実引数の番号列の集合
	"""
	# マクロの別名と仮引数の転送を明示した作業列で辿る
	pending: list[tuple[str, list[set[int]]]] = [(name, [{ idx } for idx in range(count)])]
	seen: set[tuple[str, tuple[tuple[int, ...], ...]]] = set()
	found: set[tuple[int, ...]] = set()
	while pending:
		callee: str
		bindings: list[set[int]]
		callee, bindings = pending.pop()
		key: tuple[str, tuple[tuple[int, ...], ...]] = callee, tuple(tuple(sorted(binding)) for binding in bindings)
		if key in seen:
			continue
		seen.add(key)
		if dependencies is not None:
			dependencies.add(callee)
		if cache is not None:
			cache.work[1] += 1
		for definition in macros.get(callee, initial_macro(callee)):
			if definition is None:
				continue
			params: tuple[str, ...]
			body: str
			params, body = definition
			tokens: tuple[str, ...] = (
				cache.get_tokens(body) if
				cache is not None else
				tuple(cpp_token_role(token.text) for token in cpp_phase_tokens(cpp_source(body).text) if token.kind not in ("comment", "space"))
			)
			if not params and len(tokens) == 1 and tokens[0].isidentifier():
				(pending if forwardings is None else forwardings).append((tokens[0], bindings))
				continue
			mapped: dict[str, set[int]] = { param: binding for param, binding in zip(params, bindings) }
			if params and params[-1] == "__VA_ARGS__":
				mapped[params[-1]] = set().union(*bindings[len(params) - 1:])
			# ネストした呼出を閉じる時にだけ，直接の実引数から親へ束縛を渡す
			frames: list[tuple[str, list[set[int]]]] = []
			for idx, piece in enumerate(tokens):
				if piece == "#" and idx + 1 < len(tokens) and (observed := mapped.get(tokens[idx + 1])):
					found.add(tuple(sorted(observed)))
				if piece == "(":
					previous: str = tokens[idx - 1] if idx else ""
					frames.append((previous if previous.isidentifier() and previous not in mapped else "", [set()]))
				elif piece == ")" and frames:
					forward_name: str
					forwarded: list[set[int]]
					forward_name, forwarded = frames.pop()
					if forward_name:
						(pending if forwardings is None else forwardings).append((forward_name, forwarded))
					if frames:
						frames[-1][1][-1].update(set().union(*forwarded))
				elif piece == "," and frames:
					frames[-1][1].append(set())
				elif frames and piece in mapped:
					frames[-1][1][-1].update(mapped[piece])
	# 文字列化する実引数の番号の返戻
	return found

class MacroBranch:
	"""条件枝で変更した名前だけを控え，開始時の状態と枝の結果を合流する"""

	def assign(self: MacroBranch, macros: MacroState, name: str, value: frozenset[MacroDefinition]) -> None:
		"""同じ枝の最初の変更だけを控え，使用位置の定義を置き換える
		引数：
			self: 現在の分岐の作業
			macros: 使用位置の定義
			name: 変更する名前
			value: 新しい定義候補
		"""
		if name not in self.undo:
			self.undo[name] = macros.get(name)
			self.base.setdefault(name, macros.get(name, initial_macro(name)))
		macros[name] = value
		# 終了
		return

	def finish_arm(self: MacroBranch, macros: MacroState) -> None:
		"""変更した名前の結果だけを収集し，次の枝の開始状態を復元する
		引数：
			self: 現在の分岐の作業
			macros: 使用位置の定義
		"""
		for name, previous in self.undo.items():
			self.results.setdefault(name, set()).update(macros[name])
			self.writes[name] = self.writes.get(name, 0) + 1
			if previous is None:
				del macros[name]
			else:
				macros[name] = previous
		self.undo.clear()
		# 終了
		return

	def __init__(self: MacroBranch, count: int) -> None:
		"""暗黙の不成立の枝を含む枝数と，変更する名前の作業列を初期化する
		引数：
			self: 初期化する分岐の作業
			count: 合流する枝数
		"""
		self.count: int = count
		self.base: MacroState = {}
		self.undo: dict[str, frozenset[MacroDefinition]|None] = {}
		self.results: dict[str, set[MacroDefinition]] = {}
		self.writes: dict[str, int] = {}
		# 終了
		return

def source_observation_signature(
	roots: list[Node],
	lang: str,
	work: list[int]|None = None,
	argument_work: list[int]|None = None,
	observed_ranges: list[tuple[int, int]]|None = None
) -> tuple[str, ...]:
	"""C / C++ の文字列化マクロと標準のアサーションの観測値を収集する
	通常の式の正規化から独立して括弧と意味の在る空白を比較する
	無条件の定義・取消は置き換え，未確定の条件分岐の後では枝毎の定義候補を合流する
	計算量：原文長 L・ノード数 N・分岐費用 S・解析と解決の費用 P・観測本文長 B に対して O(L+N+S+P+B)
	引数：
		roots: 原文を持つ構文木の根
		lang: 言語キー
		work: トークン化・転送状態の照会・結果の失効の回数の格納先
		argument_work: 境界走査の文字数・括弧対数・観測本文長の格納先
		observed_ranges: 文字列化される実引数の原文範囲の格納先
	戻値：
		有効な定義に束縛された実引数の文字列化結果
	"""
	# 定義・取消・呼出を原文順に処理する走査
	if lang not in ("c", "cpp", "h"):
		# 観測対象の無い言語の空の列の返戻
		return ()
	text: str = "".join(root.text for root in roots)
	logical: str = text
	source_offsets: list[int]|None = None
	if "\\" in text:
		joined: CppSource = cpp_source(text)
		logical = joined.text
		if observed_ranges is not None:
			source_offsets = joined.offsets
	prepared: str = cpp_directive_source(logical)
	if prepared != text or any(root.source is None for root in roots):
		roots = parse_spans(logical, str(dump_ast(prepared, "cpp" if lang == "h" else lang)))
		text = logical
	ranges: dict[int, dict[int, list[tuple[int, int]]]] = {}
	argument_counts: list[int] = [0, 0, 0]
	macros: MacroState = {}
	found: list[str] = []
	cache: MacroCache = MacroCache()
	# 枝の控えは変更した名前だけを持ち，状態の全列を複写しない
	frames: list[MacroBranch] = []
	pending: list[Node|tuple[str, MacroBranch]] = list(reversed(roots))
	while pending:
		node: Node|tuple[str, MacroBranch] = pending.pop()
		if isinstance(node, tuple):
			action: str
			frame: MacroBranch
			action, frame = node
			if action == "arm":
				for name, previous in frame.undo.items():
					if macros[name] != (initial_macro(name) if previous is None else previous):
						cache.invalidate(name)
				frame.finish_arm(macros)
			else:
				frames.pop()
				for name, definitions in frame.results.items():
					if frame.writes[name] < frame.count:
						definitions.update(frame.base[name])
					value: frozenset[MacroDefinition] = frozenset(definitions)
					if macros.get(name, initial_macro(name)) != value:
						cache.invalidate(name)
					if frames:
						frames[-1].assign(macros, name, value)
					else:
						macros[name] = value
			continue
		if node.type in ("preproc_if", "preproc_ifdef"):
			arms: list[list[Node]] = []
			arm: Node|None = node
			has_else: bool = False
			while arm is not None:
				body: list[Node] = []
				next_arm: Node|None = None
				has_else = arm.type == "preproc_else"
				for child in arm.children:
					if child.type in ("preproc_elif", "preproc_elifdef", "preproc_else"):
						next_arm = child
					else:
						body.append(child)
				arms.append(body)
				arm = next_arm
			# 原文順に枝を観測し，暗黙の不成立の枝では開始時の状態を保つ
			frame = MacroBranch(len(arms) + (not has_else))
			frames.append(frame)
			pending.append(("merge", frame))
			for body in reversed(arms):
				pending.append(("arm", frame))
				pending.extend(reversed(body))
			continue
		named: list[Node] = Node.get_named_children(node)
		if node.type in ("preproc_def", "preproc_function_def") and named:
			params: list[str] = []
			body: str = ""
			for child in named[1:]:
				if child.type == "preproc_params":
					params = [cpp_source(param.text).text for param in Node.get_named_children(child)]
					if any(token.text == "..." for token in child.children):
						params.append("__VA_ARGS__")
				elif child.type == "preproc_arg":
					body = child.text
			name: str = cpp_source(named[0].text).text
			value = frozenset({ (tuple(params), body) })
			if macros.get(name, initial_macro(name)) != value:
				cache.invalidate(name)
			if frames:
				frames[-1].assign(macros, name, value)
			else:
				macros[name] = value
			continue
		if node.type == "preproc_call":
			words: list[str] = [token.text for token in cpp_tokens(node.text) if token.kind == "identifier"]
			if len(words) > 1 and words[0] == "undef":
				if macros.get(words[1], initial_macro(words[1])) != frozenset({ None }):
					cache.invalidate(words[1])
				if frames:
					frames[-1].assign(macros, words[1], frozenset({ None }))
				else:
					macros[words[1]] = frozenset({ None })
			continue
		if node.type == "call_expression" and len(named) == 2 and named[0].type == "identifier" and named[1].type == "argument_list":
			name = cpp_source(named[0].text).text
			if any(definition is not None for definition in macros.get(name, initial_macro(name))):
				source: str = named[1].source if named[1].source is not None else text
				if id(source) not in ranges:
					ranges[id(source)] = cpp_argument_ranges(source, argument_counts)
				args: list[tuple[int, int]] = ranges[id(source)].get(named[1].start, [])
				for slots in sorted(cache.lookup(macros, name, len(args))):
					if observed_ranges is not None:
						for idx in slots:
							begin: int
							end: int
							begin, end = args[idx]
							observed_ranges.append((begin, end) if source_offsets is None else (source_offsets[begin], source_offsets[end]))
					argument_counts[2] += sum(args[idx][1] - args[idx][0] for idx in slots)
					found.append(stringized_text(",".join(source[args[idx][0]:args[idx][1]] for idx in slots)))
		pending.extend(reversed(node.children))
	if work is not None:
		work.extend(cache.work)
	if argument_work is not None:
		argument_work.extend(argument_counts)
	# 原文順の観測値の返戻
	return tuple(found)

def token_gaps(nodes: list[Node], is_rust: bool = False) -> tuple[tuple[bool, bool], ...]:
	"""コメント本文を固定せず，直属のトークン間の区切の有無を比較する
	計算量：トークン数 N と間隙の総文字数 B に対して O(N+B)
	引数：
		nodes: 同じ原文を共有するトークンの列
		is_rust: 記号同士の密着だけを比較するか
	戻値：
		連続する非コメントのトークン間の，空白とコメントの所属
		Rust は隣接する記号同士の密着だけを比較し，其れ以外は有無を返す
	送出例外：
		RuntimeError: トークンの位置又は原文の所属を確定出来ない場合
	"""
	found: list[tuple[bool, bool]] = []
	previous: Node|None = None
	previous_token: Node|None = None
	has_space: bool = False
	has_comment: bool = False
	has_token: bool = False
	for node in nodes:
		if node.start < 0:
			raise RuntimeError("token gaps require AST source positions")
		if previous is not None:
			if node.source is None or node.source is not previous.source:
				raise RuntimeError("token gaps have different source owners")
			gap: str = node.source[previous.end:node.start]
			has_space |= bool(gap)
		if is_comment(node.type):
			has_comment = True
		else:
			if has_token:
				if is_rust and previous_token is not None:
					# 区切・リテラル・名前との空白は意味を持つ密着に含めず，コメントは記号同士を区切る
					punctuation: str = "!#$%&'*+,-./:;<=>?@^|~"
					is_pair: bool = (
						previous_token.kind == node.kind == "U" and bool(previous_token.text) and bool(node.text) and
						all(char in punctuation for char in previous_token.text + node.text)
					)
					found.append((is_pair and not (has_space or has_comment), False))
				else:
					found.append((has_space, has_comment))
			previous_token = node
			has_space, has_comment = False, False
			has_token = True
		previous = node
	# コメント本文と分離したトークン間隙の返戻
	return tuple(found)

def pre_text_signature(node: Node) -> tuple[str, ...]:
	"""保持要素のタグ間の原文を，パーサーが省いた空白も含めて収集する
	計算量：部分木のノード数 N と本文長 B に対して O(N+B)
	引数：node: pre 要素
	戻値：タグの境界毎のテキスト．タグ内の空白とコメント本文は含めない
	"""
	pieces: list[str] = []
	text: list[str] = []
	position: int = node.start
	pending: list[Node] = list(reversed(node.children))
	while pending:
		part: Node = pending.pop()
		if part.type in ("comment", "end_tag", "self_closing_tag", "start_tag"):
			text.append(node.source[position:part.start])
			position = part.end
			if part.type != "comment":
				pieces.append("".join(text))
				text.clear()
		else:
			pending.extend(reversed(part.children))
	text.append(node.source[position:node.end])
	pieces.append("".join(text))
	# 表示位置毎の逐語テキストの返戻
	return tuple(pieces)

def protected_observation_signature(roots: list[Node], lang: str) -> tuple[str, ...]:
	"""通常の式の正規化から独立して，表示・出力される原文の保護範囲を比較する
	計算量：ノード数 N・収集する本文の総文字数 B に対して O(N+B)
	引数：
		roots: 原文を持つ構文木の根
		lang: 言語キー
	戻値：
		原文順の保護本文
	送出例外：RuntimeError: CSS のカスタムプロパティの値の範囲又はトークンの原文の所有者を確認出来ない場合
	"""
	found: list[str] = []
	url_values: dict[int, tuple[int, str, str]] = {}
	url_ends: set[int] = set()
	if lang in ("css", "scss"):
		_, values = css_value_source(lang, "".join(root.text for root in roots))
		url_values = { start: value for start, value in values.items() if value[1] == "css_url" }
		url_ends = { value[0] for value in url_values.values() }
	pending: list[tuple[Node, bool]] = [(root, False) for root in reversed(roots)]
	while pending:
		node, is_pre = pending.pop()
		if (
			lang == "php" and node.type in ("protected_php_tail", "text") or lang == "ruby" and node.type == "uninterpreted" or
			lang == "html" and node.type in ("attribute_value", "comment", "doctype")
		):
			found.append(node.text)
			continue
		if lang == "html" and node.type == "element" and node.children:
			start: Node = node.children[0]
			tags: list[Node] = [child for child in start.children if child.type == "tag_name"]
			if not is_pre and tags and tags[0].text.lower() == "pre":
				found.extend(pre_text_signature(node))
			is_pre |= bool(tags and tags[0].text.lower() == "pre")
			if tags and tags[0].text.lower() in ("iframe", "noembed", "noframes", "plaintext", "textarea", "title", "xmp"):
				end: Node = node.children[-1]
				limit: int = len(node.text) - (len(end.text) if end.type == "end_tag" else 0)
				found.append(node.text[len(start.text):limit])
				pending.extend((child, is_pre) for child in reversed(start.children))
				continue
		if lang == "rust" and node.type in RUST_TOKEN_TREES:
			found.append(json.dumps(token_gaps(node.children, True)))
		if node.type.startswith("embedded:"):
			# 子は其の言語の単独の正規形が保護し，親の位置では再照合しない
			continue
		if lang == "python" and node.type == "interpolation" and any(child.type == "=" for child in node.children):
			pos: int = 0
			for child in node.children:
				pos = child.end - node.start
				if child.type == "=":
					found.append(node.text[:re.compile(r"\s*").match(node.text, pos).end()])
					break
		if lang in ("css", "scss") and (body := css_url_body(node)) is not None:
			if node.end not in url_ends:
				# エスケープされた関数名も，木が確定した URL として保持する
				found.append(body)
		if lang in ("css", "scss") and node.type == "declaration" and node.children:
			if node.children[0].type == "property_name" and css_identifier(node.children[0].text).startswith("--"):
				colon: int = next((child.end for child in node.children if child.type == ":"), -1)
				end = node.end - node.text.endswith(";")
				if colon < 0 or node.source is None:
					raise RuntimeError("custom property requires its value range")
				value: str = node.source[colon:end]
				if not value and (space := re.match(r"[ \t\r\n\f]+(?=[;}])", node.source[end:])):
					value = space.group()
				if not value.strip():
					found.append(value)
				else:
					leaves: list[Node] = [leaf for leaf, begin, limit in source_leaves([node]) if colon <= begin and limit <= end]
					found.append(
						json.dumps((tuple(leaf.text for leaf in leaves if not is_comment(leaf.type)), token_gaps(leaves)), ensure_ascii=False)
					)
		pending.extend((child, is_pre) for child in reversed(node.children))
	# 生の木が URL を誤読しても，本文の観測を欠落させない
	return (*found, *(value[2].strip(" \t\r\n\f") for value in url_values.values()))

def forest_sexpr(roots: list[Node], lang: str = "") -> str:
	"""木グループ全体を正準 S 式へ変換する

	計算量：ノード数 N・内容文字数 B・マクロの解決費用 P に対して最悪 O(N²+B+P)

	引数：
		roots: 最上位ノードの一覧
		lang: 言語キー

	戻値：
		木グループの正準 S 式
	"""
	# 保護する観測値の収集
	signature: tuple[str, ...] = source_observation_signature(roots, lang) + protected_observation_signature(roots, lang)
	# 構造と，意味の在るソース観測値を合わせた正準形の返戻
	return (
		" ".join(sexpr(norm) for root in roots for norm in normalize(root, lang)) +
		(json.dumps(signature, ensure_ascii=False) if signature else "")
	)

def literal_ranges(text: str, ext: str, code_start: int = 0) -> tuple[dict[int, int], dict[int, int]]:
	"""解析木が読むリテラルの断片と，Ruby の最外のリテラルの開始・終端を返す
	パーサーの本文を原文へ対応付けれない場合は RuntimeError を送出する
	計算量：解析・木の復元費用 P・ノード数 N・原文の文字数 L に対してO(P+N+L)
	引数：
		text: コメントとコードを区分する原文
		ext: パーサーに渡す言語の拡張子
		code_start: 言語の本文の開始位置（埋込の開始タグの直後等）
	戻値：
		逐語の断片と最外のリテラルの，開始位置から直後の位置へのインデックス
	送出例外：
		OSError: パーサーの起動が失敗する場合
		RuntimeError: パーサーが失敗する，又は解析木を原文へ対応付けれない場合
		ValueError: パーサーの出力が不正な場合
		json.JSONDecodeError: パーサーのノード値が有効な JSON でない場合
		subprocess.TimeoutExpired: パーサーが制限時間内に完了しない場合
	"""
	# 原文の範囲を共有する解析木の対応付
	found: dict[int, int] = {}
	if ext in ("css", "scss"):
		_, values = css_value_source(ext, text[code_start:])
		found = { code_start + start: code_start + value[0] for start, value in values.items() if value[1] == "css_url" }
	enclosures: dict[int, int] = {}
	pending: list[tuple[list[Node], bool]] = [(parse_dump(dump_ast(text[code_start:], ext)), False)]
	while pending:
		children: list[Node]
		is_enclosed: bool
		children, is_enclosed = pending.pop()
		for node in children:
			if node.start == node.end:
				continue
			pos: int = code_start + node.start
			if node.start < 0:
				raise RuntimeError(f"AST text does not match its source: {node.type}")
			end: int = code_start + node.end
			if node.type.startswith("embedded:"):
				found[pos] = end
			elif ext in ("css", "scss") and css_url_body(node) is not None:
				head: Node = Node.get_named_children(node)[0]
				start: int = head.start + head.text.rfind(":") + 1
				while start < head.end and text[code_start + start].isspace():
					start += 1
				# 誤読したマップのキーはコードに残し，URL の開始から本文を保護する
				found[code_start + start] = end
			elif (
				node.type in ("multi_line_string_literal", "raw_string_literal", "regex", "regex_literal") or
				node.type == "string_literal" and ext in ("java", "kt") or
				node.type in ("interpolated_string_expression", "template_string", "verbatim_string_literal") or
				ext == "scss" and node.type == "plain_value" and not node.children or
				node.type in ("delimited_symbol", "string", "string_array", "subshell", "symbol_array") and
				(ext == "rb" or node.text.startswith("%"))
			):
				# 補間のコードと逐語の断片の分離
				if ext == "rb" and not is_enclosed:
					enclosures[pos] = end
				fragment_start: int = pos
				hole_start: int|None = None
				hole_idx: int = 0
				for child_idx, child in enumerate(node.children):
					child_pos: int = code_start + child.start
					child_end: int = code_start + child.end
					# Kotlin の補間はコードとコメントを同じ兄弟列に持つ
					if child.type == "interpolation_expression_start":
						if fragment_start < child_pos:
							found[fragment_start] = child_pos
						hole_start, hole_idx = child_pos, child_idx
					elif child.type == "interpolation_expression_end" and hole_start is not None:
						pending.append((node.children[hole_idx:child_idx + 1], True))
						fragment_start, hole_start = child_end, None
					elif hole_start is None and child.type in ("interpolated_expression", "interpolation", "template_substitution"):
						if fragment_start < child_pos:
							found[fragment_start] = child_pos
						pending.append(([child], True))
						fragment_start = child_end
				if fragment_start < end:
					found[fragment_start] = end
			else:
				pending.append((node.children, is_enclosed))
	# リテラル区間のインデックスの返戻
	return found, enclosures

# 整形仕様 4.5 に登録された Go の定型名（未知の //go:名は通常コメント）
GO_DIRECTIVE: re.Pattern[str] = re.compile(
	r"//(go:(?:build|debug|embed|generate)(?=[ \t\r\n]|$)|(?:go:(?:linkname|noescape|noinline|norace|nosplit|uintptrescapes|wasmexport|wasmimport)|export|extern|line)(?=[ \r\n]|$))"
)

class DirectiveFacts:
	"""登録形の所有範囲と後続対象を，位置の空白と独立した構文の事実として記録する"""

	def __init__(self: DirectiveFacts, roots: list[Node], leaves: list[tuple[Node, int, int]], lang: str) -> None:
		"""許可済のラッパーを透過し，同じ綴りの別の対象を構文順で識別する
		計算量：ノード数 N・葉の数 T に対して O(N log(T+1)+T)
		引数：
			self: 初期化する登録形の事実
			roots: 原文を持つ構文木
			leaves: 原文順の葉（空でない物）
			lang: 言語キー
		"""
		self.owners: dict[Node, tuple] = {}
		self.targets: dict[Node, tuple] = {}
		self.imports: set[str] = set()
		self.package: str = ""
		starts: list[int] = []
		positions: list[int] = []
		boundary: int = 0
		for node, start, _ in leaves:
			starts.append(start)
			positions.append(boundary)
			if directive_boundary_token(node, lang) and node.text not in ("(", ")", ",", ";", "return", "{", "}"):
				boundary += 1
		facts: TreeFacts = TreeFacts(roots)
		scope: int = 0
		wrappers: set[Node] = set()
		parent: Node|None
		pending: list[tuple[Node, Node|None, tuple, tuple]] = [(root, None, (), ()) for root in reversed(roots)]
		while pending:
			node, parent, owner, target = pending.pop()
			position_idx: int = bisect_left(starts, node.start)
			position: int = positions[position_idx] if position_idx < len(positions) else boundary
			if (
				node.type in BODY_BLOCKS or node.type in ("declaration_list", "class_body", "namespace_definition") or
				lang == "ruby" and node.type in ("class", "method", "module", "singleton_method")
			):
				named: list[Node] = facts.named[node]
				is_wrapper: bool = (
					lang in OPTIONAL_BODY_LANGS and node.type in BODY_BLOCKS and parent is not None and parent.type in CONTROL_BODIES and
					len(named) == 1 and named[0].type != "empty_statement" and not facts.binding[named[0]]
				)
				if is_wrapper:
					wrappers.update(child for child in node.children if child.kind == "U" and child.text in ("{", "}"))
				else:
					owner: tuple[str, int, int] = node.type, position, scope
					scope += 1
			if node.type.endswith(("_declaration", "_definition", "_item")) or node.type == "var_spec":
				target: tuple[str, int, bool] = node.type, position, any(child.type in BODY_BLOCKS for child in node.children)
			if node.type == "import_spec":
				self.imports.update(child.text.strip("\"") for child in node.children if child.type == "interpreted_string_literal")
			if node.type == "package_clause":
				self.package: str = "".join(child.text for child in node.children if child.type == "package_identifier")
			self.owners[node] = owner
			self.targets[node] = target
			if not is_comment(node.type):
				pending.extend((child, node, owner, target) for child in reversed(node.children))
		# 空の隣接した並びも別の対象に為る
		# 許可済の本体ラッパーの開閉だけを透過する
		target_positions: list[int] = []
		boundary = 0
		for node, _, _ in leaves:
			target_positions.append(boundary)
			if directive_boundary_token(node, lang) and node not in wrappers and node.text not in ("(", ")", ",", ";", "return"):
				boundary += 1
		self.following: list[tuple] = [()] * len(leaves)
		next_target: tuple = "end", boundary
		for idx in range(len(leaves) - 1, -1, -1):
			node: Node = leaves[idx][0]
			self.following[idx] = next_target
			if directive_boundary_token(node, lang) and node not in wrappers and node.text not in ("(", ")", ",", ";", "return"):
				next_target = node.type, target_positions[idx], self.targets[node], self.owners[node]
		# 終了
		return

# 検証ツールが仕様から定めるドキュメントの項目と説明対象
DocItem: TypeAlias = tuple[str, str, tuple[str, ...]]

DOC_DEFINITIONS: frozenset[str] = frozenset(
	(
		"class_declaration",
		"class_definition",
		"class_specifier",
		"constructor_declaration",
		"declaration",
		"enum_declaration",
		"field_declaration",
		"function_declaration",
		"function_definition",
		"function_item",
		"interface_declaration",
		"lexical_declaration",
		"method",
		"method_declaration",
		"method_definition",
		"method_elem",
		"module",
		"property_declaration",
		"singleton_method",
		"struct_declaration",
		"struct_specifier",
		"type_alias_declaration",
		"type_declaration",
		"variable_declaration"
	)
)

# 型・メンバ・最上位の宣言を，チェッカー自身の認識表で対応付ける
DOC_DEFINITION_EXTRAS: dict[str, frozenset[str]] = {
	"c": frozenset(("enum_specifier", "enumerator", "preproc_include", "type_definition")),
	"cpp": frozenset(
		(
			"alias_declaration",
			"enum_specifier",
			"enumerator",
			"namespace_definition",
			"preproc_include",
			"type_definition",
			"using_declaration"
		)
	),
	"csharp": frozenset(
		("delegate_declaration", "enum_member_declaration", "namespace_declaration", "record_declaration", "using_directive")
	),
	"java":
	frozenset(("annotation_type_declaration", "enum_constant", "import_declaration", "package_declaration", "record_declaration")),
	"go": frozenset(("const_declaration", "import_declaration", "var_declaration")),
	"rust": frozenset(
		(
			"const_item",
			"enum_item",
			"enum_variant",
			"field_declaration",
			"impl_item",
			"mod_item",
			"source_file",
			"struct_item",
			"trait_item",
			"type_item",
			"union_item",
			"use_declaration"
		)
	),
	"kotlin": frozenset(("import_header", "object_declaration", "type_alias")),
	"swift":
	frozenset(("associatedtype_declaration", "enum_entry", "import_declaration", "protocol_declaration", "typealias_declaration")),
	"php": frozenset(("const_declaration", "enum_case", "namespace_use_declaration", "trait_declaration")),
	"javascript": frozenset(("import_statement",)),
	"typescript": frozenset(("import_statement",)),
	"tsx": frozenset(("import_statement",)),
	"ruby": frozenset(("class",))
}

DOC_BODIES: frozenset[str] = frozenset(
	(
		"annotation_type_body",
		"block",
		"body_statement",
		"class_body",
		"compound_statement",
		"declaration_list",
		"enum_body",
		"enum_class_body",
		"enum_declaration_list",
		"enum_member_declaration_list",
		"enumerator_list",
		"field_declaration_list",
		"function_body",
		"interface_body",
		"interface_type",
		"object_type",
		"protocol_body",
		"statement_block"
	)
)

DOC_FRAMES: frozenset[str] = frozenset(
	(
		"declaration",
		"do_statement",
		"expression_statement",
		"field_declaration",
		"if_statement",
		"lexical_declaration",
		"macro_definition",
		"macro_invocation",
		"property_declaration",
		"return_statement",
		"throw_statement",
		"variable_declaration"
	)
)

DOC_CONTAINERS: frozenset[str] = frozenset(("initializer_list", "literal_value", "object", "object_literal"))
# 属性・値・逐語領域として既に比較する経路
DOC_OTHER_PATHS: frozenset[str] = frozenset(("python", "json", "jsonc", "html", "css", "scss"))

class DocIndex:
	"""原文の範囲・直接の所有者・配置枠を，入力の世代毎に共有する
	所有者の対応は親の番号とローカルの見出の組を共有し，全祖先を展開しない
	計算量：ノードとコメント候補の総数 N・原文量 B に対し構築は O(N log(N+1)+B)．共有する観測の文字数 H を含む保持量は O(N+B+H)
	"""

	def header(self: DocIndex, node: Node) -> str:
		"""本体を除いたローカルの見出を１度だけ正規化する
		計算量：見出の正規化費用 H に対して O(H)，再利用は O(1)
		引数：
			self: 同じ世代のドキュメント観測
			node: 説明対象の定義
		戻値：
			本体とコメントに依らない見出
		"""
		if node not in self.headers:
			header: Node = Node("", node.kind, node.type)
			header.children: list[Node] = [] if node.type == "source_file" else [
				child for child in node.children if child.field != "body" and child.type not in DOC_BODIES and
				not (not child.children and child.text in ("end", "}"))
			]
			self.headers[node] = " ".join(sexpr(norm) for norm in normalize(header, self.lang))
		# 共有するローカルの見出の返戻
		return self.headers[node]

	def identify(self: DocIndex) -> None:
		"""直接の親と見出から，入力と候補に共通の所有者番号を割り当てる
		計算量：定義数 D・見出の正規化費用の総和 H に対して O(D+H)
		引数：
			self: 同じ世代のドキュメント観測
		"""
		if self.owner_ids or not self.comments:
			# 既存の番号又はコメント無しの木の早期返戻
			return
		counts: Counter[tuple[int, str]] = Counter()
		for node in self.definitions:
			parent: int = self.owner_ids.get(self.owners[node], 0)
			identity: tuple[int, str] = parent, self.header(node)
			counts[identity] += 1
			occurrence: int = counts[identity]
			self.work["owner_links"] += 1
			self.occurrences[node] = occurrence
			key: tuple[int, str, int] = parent, identity[1], occurrence
			self.owner_ids[node] = self.identities.setdefault(key, len(self.identities) + 1)
		# 所有者の番号の構築完了の返戻
		return

	def following(self: DocIndex, end: int) -> Node|None:
		"""コメントの後続宣言を位置のインデックスから返す
		計算量：宣言数 D に対して O(log(D+1))
		引数：
			self: 同じ世代のドキュメント観測
			end: コメントの終端
		戻値：
			後続する最初の宣言．無ければ None
		"""
		self.work["definition_queries"] += 1
		idx: int = bisect_left(self.starts, end)
		# 位置に対応する後続宣言の返戻
		return self.ordered_definitions[idx] if idx < len(self.starts) else None

	def __init__(self: DocIndex, lang: str, roots: list[Node], identities: dict[tuple[int, str, int], int]|None = None) -> None:
		"""１度の木の走査と位置のインデックスから，コメントの所属を求める
		計算量：ノードとコメント候補の総数 N・原文量 B に対して O(N log(N+1)+B)
		引数：
			self: 初期化するドキュメント観測
			lang: 入力の言語
			roots: 原文範囲を持つ解析木
			identities: 入力と候補で共有する所有者の対応表
		"""
		self.work: Counter[str] = Counter()
		self.lang: str = lang
		self.roots: list[Node] = roots
		self.identities: dict[tuple[int, str, int], int] = identities if identities is not None else {}
		self.definitions: list[Node] = []
		self.comments: list[Node] = []
		self.decorators: list[Node] = []
		self.frames: list[Node] = []
		self.opaque_frames: set[Node] = set()
		self.owners: dict[Node, Node|None] = {}
		self.frame_of: dict[Node, Node|None] = {}
		self.container_of: dict[Node, Node|None] = {}
		self.template_of: dict[Node, Node|None] = {}
		self.body_of: dict[Node, Node|None] = {}
		self.headers: dict[Node, str] = {}
		self.parts: dict[Node, tuple[list[Node], list[int]]] = {}
		self.targets: dict[str, Node] = {}
		self.owner_ids: dict[Node, int] = {}
		self.occurrences: dict[Node, int] = {}
		self.records: tuple[dict[str, list[DocItem]], set[str]]|None = None
		self.ordinary: Counter[str]|None = None
		self.merged_body_comments: set[Node] = set()
		self.members: dict[bool, tuple[dict[str, list[tuple[str, ...]]], dict[str, list[tuple[str, ...]]]]] = {}
		kinds: frozenset[str] = DOC_DEFINITIONS | DOC_DEFINITION_EXTRAS.get(lang, frozenset())
		pending: list[tuple[Node, Node|None, Node|None, Node|None, Node|None]] = [
			(root, None, None, None, None) for root in reversed(roots)
		]
		node: Node
		owner: Node|None
		frame: Node|None
		container: Node|None
		template: Node|None
		while pending:
			node, owner, frame, container, template = pending.pop()
			self.work["nodes"] += 1
			self.owners[node] = owner
			self.frame_of[node] = frame
			self.container_of[node] = container
			self.template_of[node] = template
			if node.kind == "N" and node.type in kinds:
				self.definitions.append(node)
				self.body_of[node] = (
					node if
					node.type == "source_file" else
					next((child for child in node.children if child.field == "body" or child.type in DOC_BODIES), None)
				)
				if self.body_of[node] is None:
					# 型指定等のラッパーを介する本体も，別の宣言へ降りずに取得する
					body_pending: list[Node] = list(reversed(node.children))
					while body_pending:
						body_candidate: Node = body_pending.pop()
						if body_candidate.type in DOC_BODIES:
							self.body_of[node] = body_candidate
							break
						if body_candidate.type not in kinds and not is_comment(body_candidate.type):
							body_pending.extend(reversed(body_candidate.children))
				owner = node
			if node.type in DOC_FRAMES and (node.type != "macro_invocation" or frame is None or frame.type != "expression_statement"):
				self.frames.append(node)
				frame = node
			if lang == "rust" and node.type in RUST_TOKEN_TREES and frame is not None:
				self.opaque_frames.add(frame)
			if node.type in DOC_CONTAINERS:
				container = node
			if node.type == "template_declaration":
				template = node
			if node.type == "decorator":
				self.decorators.append(node)
			if is_comment(node.type):
				self.comments.append(node)
			else:
				pending.extend((child, owner, frame, container, template) for child in reversed(node.children))
		self.source: str = roots[0].source or roots[0].text if roots else ""
		# 葉の範囲と既存コメントを並べ，未投影の囲みだけを位置で収集する
		ranges: list[tuple[int, int]] = sorted((node.start, node.end) for node, _, _ in source_leaves(roots) if node.end > node.start)
		starts: list[int] = [begin for begin, _ in ranges]
		ends: list[int] = []
		for _, end in ranges:
			ends.append(max(end, ends[-1] if ends else 0))
		block_begin: int = 0
		while True:
			self.work["block_searches"] += 1
			block_start: int = self.source.find("/*", block_begin)
			if block_start < 0:
				break
			self.work["block_searches"] += 1
			block_end: int = self.source.find("*/", block_start + 2)
			if block_end < 0:
				break
			# 閉記号の無い開始列でも，同じ接尾部を検索し直さない
			block_begin = block_end + 2
			self.work["range_queries"] += 1
			idx: int = bisect_right(starts, block_start) - 1
			if -1 < idx and block_start < ends[idx]:
				continue
			comment: Node = Node("", "N", "comment")
			comment.source, comment.start, comment.end = self.source, block_start, block_begin
			self.comments.append(comment)
		self.comments.sort(key=lambda node: node.start)
		self.ordered_definitions: list[Node] = sorted(self.definitions, key=lambda node: (node.start, -node.end))
		self.starts: list[int] = [node.start for node in self.ordered_definitions]
		self.containing: dict[Node, Node|None] = {}
		active: list[Node] = []
		pos: int = 0
		for comment in self.comments:
			while pos < len(self.ordered_definitions) and self.starts[pos] < comment.start:
				node = self.ordered_definitions[pos]
				while active and active[-1].end < node.start + 1:
					active.pop()
				active.append(node)
				pos += 1
			while active and active[-1].end < comment.end:
				active.pop()
			self.containing[comment] = active[-1] if active else None
		masked: list[str] = list(self.source)
		for comment in self.comments:
			masked[comment.start:comment.end] = ["\n" if char == "\n" else " " for char in self.source[comment.start:comment.end]]
		self.code: str = "".join(masked)
		self.code_positions: list[int] = [idx for idx, char in enumerate(self.code) if not char.isspace()]
		self.line_positions: list[int] = [idx for idx, char in enumerate(self.code) if char not in " \t\r\n\ufeff"]
		self.newlines: list[int] = [idx for idx, char in enumerate(self.source) if char == "\n"]
		self.blanks: list[tuple[int, int]] = [(match.start(1), match.end(1)) for match in re.finditer(r"(?=(\n[ \t]*\n))", self.source)]
		self.blank_starts: list[int] = [begin for begin, _ in self.blanks]
		self.ordered_frames: list[Node] = sorted(self.frames, key=lambda node: (node.start, -node.end))
		self.frame_starts: list[int] = [node.start for node in self.ordered_frames]
		self.finished_frames: list[Node] = sorted(self.frames, key=lambda node: (node.end, node.start))
		self.frame_ends: list[int] = [node.end for node in self.finished_frames]
		self.decorator_starts: list[int] = [node.start for node in self.decorators]
		# 共有する観測の構築完了の返戻
		return

def doc_name(text: str) -> str:
	"""引用・接頭辞・文字参照を除き，説明対象の比較名を返す
	計算量：名前の文字数 B に対して O(B)
	引数：
		text: ドキュメント又はシグネチャの名前
	戻値：
		比較用の名前
	"""
	text = text.strip().strip("`")
	text = re.sub(r"^(?:r#|@|\\uFEFF)", "", text)
	text = re.sub(
		r"\\u(?:\{([0-9a-fA-F]+)\}|([0-9a-fA-F]{4}))|\\U([0-9a-fA-F]{8})",
		lambda match: chr(int(next(value for value in match.groups() if value), 16)),
		text
	)
	# 綴りの装飾を除いた比較名の返戻
	return text

def doc_bracket_end(text: str, start: int) -> int:
	"""引用・エスケープ・入子を保って項目の型又は既定値の終端を返す
	計算量：対象の範囲の文字数 B に対して O(B)
	引数：
		text: 項目の本文
		start: 開記号の位置
	戻値：
		閉じの直後．閉じなければ本文の終端
	"""
	stack: list[str] = []
	quote: str = ""
	is_escaped: bool = False
	pairs: dict[str, str] = { "(": ")", "[": "]", "{": "}" }
	for pos in range(start, len(text)):
		char: str = text[pos]
		if is_escaped:
			is_escaped = False
		elif char == "\\":
			is_escaped = True
		elif quote:
			if char == quote:
				quote = ""
		elif char in "\"'`":
			quote = char
		elif char in pairs:
			stack.append(pairs[char])
		elif stack and char == stack[-1]:
			stack.pop()
			if not stack:
				# 項目の囲みの終端の返戻
				return pos + 1
	# 未完の囲みの終端の返戻
	return len(text)

def doc_sexpr_end(text: str, start: int) -> int:
	"""正規形の引用値と記号の型名を区別して，１つのノードの終端を返す
	計算量：正規形の文字数 B に対して O(B)
	引数：
		text: 見出の正規形
		start: ノードの開始位置
	戻値：
		ノードの閉じの直後
	"""
	depth: int = 0
	pos: int = start
	while pos < len(text):
		char: str = text[pos]
		if char == "(":
			depth += 1
			pos += 1
			if pos < len(text) and text[pos] in "()":
				pos += 1
			else:
				while pos < len(text) and not text[pos].isspace() and text[pos] != ")":
					pos += 1
			continue
		if char in "\"'" and pos and text[pos - 1].isspace():
			quote: str = char
			pos += 1
			while pos < len(text):
				if text[pos] == "\\":
					pos += 2
				elif text[pos] == quote:
					pos += 1
					break
				else:
					pos += 1
			continue
		if char == ")":
			depth -= 1
			if not depth:
				# 原文範囲に対応する正規形の終端の返戻
				return pos + 1
		pos += 1
	# 不完の正規形の終端の返戻
	return len(text)

def doc_description(text: str) -> tuple[str, ...]:
	"""仕様の本文正規化だけを適用し，説明の出現順とコードを保つ
	計算量：本文の文字数 B に対して O(B)
	引数：
		text: 記法とタグの見出を除いた説明
	戻値：
		論理行の説明キー
	"""
	from verify_case_variants import comment_character_kind, comment_enclosures, logical_lines
	from unicodedata import category
	found: list[str] = []
	ordinary: list[str] = []
	is_code: bool = False
	fence: str = ""
	def flush() -> None:
		"""通常の説明だけを文境界と文字種の規則で照合形へ移す
		計算量：未処理の本文量 B に対して O(B)
		戻値：
			なし
		"""
		for line in logical_lines("\n".join(ordinary)):
			_, protected, _ = comment_enclosures(line)
			parts: list[str] = []
			for pos, char in enumerate(line):
				if protected[pos]:
					parts.append(char)
					continue
				if char.isspace():
					left_pos: int = pos - 1
					while -1 < left_pos and category(line[left_pos]).startswith("M"):
						left_pos -= 1
					left: str = line[left_pos] if -1 < left_pos else ""
					right: str = line[pos + 1] if pos + 1 < len(line) else ""
					if left and right and not (
						comment_character_kind(left)[0] or comment_character_kind(right)[0] or left in ",，、:：;；([{（［｛" or right in ",，、:：;；)]}）］｝"
					):
						parts.append(" ")
				else:
					parts.append(char.translate(str.maketrans("，、：；（）［］｛｝＜＞／－＋＝＆", ",,:;()[]{}<>/-+=&")))
			found.append("".join(parts))
		ordinary.clear()
		# 未処理の説明の反映の完了
		return
	for line in text.splitlines():
		stripped: str = line.strip()
		mark: re.Match[str]|None = re.match(r"(`{3,}|~{3,})", stripped)
		if mark or re.match(r"[@\\]code\b", stripped):
			flush()
			if mark:
				fence = "" if fence and stripped.startswith(fence) else mark[1]
			else:
				is_code = True
			found.append(stripped)
		elif is_code or fence:
			# コード例は文正規化へ渡さず，空行も含めて保持する
			found.append(stripped)
		else:
			ordinary.append(line)
		if re.match(r"[@\\]endcode\b", stripped):
			is_code = False
	flush()
	# 空白の文字種と記号の表記だけを揃え，語・読点・コードを保った説明の返戻
	return tuple(found)

def doc_items(lang: str, body: str) -> list[DocItem]:
	"""記法から独立した説明項目を，境界・引用・コード例を保って読む
	計算量：本文の文字数 B に対して O(B)
	引数：
		lang: 同義タグと採用形式を決める言語
		body: １つのドキュメントの本文
	戻値：
		原文順の説明項目
	"""
	# XML は標準のパーサーで引用内の終端と文字参照を１度読む
	if lang == "csharp" and re.search(r"</?(?:summary|param|typeparam|returns)\b", body):
		from xml.etree import ElementTree
		try:
			root: ElementTree.Element = ElementTree.fromstring("<doc>" + body + "</doc>")
		except ElementTree.ParseError:
			# 読取不能な構造を説明無へ落とさない項目の返戻
			return [("xml-unparsed", "", (body.strip(),))]
		def xml_description(element: ElementTree.Element) -> tuple[str, ...]:
			"""XML の内容・参照先・コード例を構造毎に独立して読む
			計算量：要素の文字数 B と子孫数 N に対して O(B+N)
			引数：
				element: 開始・終了と属性を解析済の説明要素
			戻値：
				参照の属性とコードを保持した説明の列
			"""
			if element.tag in ("code", "pre", "programlisting"):
				# コードの内部の句点・空白・空行を本文正規化へ渡さない内容の返戻
				return tuple(line.strip() for line in "".join(element.itertext()).splitlines() if line.strip())
			parts: list[str] = list(doc_description(element.text or ""))
			for child in element:
				parts.append("\0xml:" + child.tag + repr(sorted(child.attrib.items())))
				parts.extend(xml_description(child))
				parts.append("\0/xml:" + child.tag)
				parts.extend(doc_description(child.tail or ""))
			# 本文の正規化と XML の構造を分けた説明の返戻
			return tuple(parts)
		items: list[DocItem] = []
		if root.text and root.text.strip():
			items.append(("summary", "", doc_description(root.text)))
		for element in root:
			kind: str = { "param": "param", "returns": "return", "summary": "summary", "typeparam": "typeparam" }.get(
				element.tag,
				"xml:" + element.tag
			)
			name: str = doc_name(element.attrib.get("name", ""))
			# 内部の参照・コードの構造と属性も説明の一部として保持する
			description: tuple[str, ...] = xml_description(element)
			if kind.startswith("xml:"):
				description = ElementTree.tostring(element, encoding="unicode").removesuffix(element.tail or ""),
			if description:
				attributes: tuple[tuple[str, str], ...] = tuple(sorted((key, value) for key, value in element.attrib.items() if key != "name"))
				items.append((kind, name + ("\0" + repr(attributes) if attributes else ""), description))
			if element.tail and element.tail.strip():
				items.append(("summary", "", doc_description(element.tail)))
		# XML の説明項目の返戻
		return items
	items = []
	kind = "summary"
	name = ""
	lines: list[str] = []
	fence: str = ""
	is_code: bool = False
	is_in_parameters: bool = False
	parameter_indent: int = 0
	overload: int = 0
	# 本文へ統合した通常コメントのタグ状の開始のエスケープ（JSDoc の `@`，Swift の箇条書の `-`）
	marker: str = { "javascript": "@", "swift": "-", "tsx": "@", "typescript": "@" }.get(lang, "")
	for line in body.splitlines():
		text: str = line.strip()
		mark: re.Match[str]|None = re.match(r"(`{3,}|~{3,})", text)
		if mark:
			fence = "" if fence and text.startswith(fence) else mark[1]
		if re.match(r"[@\\]code\b", text) or "{@code" in text:
			is_code = True
		header: re.Match[str]|None = None if fence or is_code else re.match(r"[@\\]([\w]+)(?![\w-])\s*(.*)", text)
		if not text and lang != "rust":
			is_in_parameters = False
		if lang == "rust" and not fence and not is_code:
			heading: re.Match[str]|None = re.fullmatch(r"#\s+(Arguments|Errors|Panics|Returns)\s*", text)
			parameter: re.Match[str]|None = (
				re.match(r"[*-]\s+(?:`([^`]+)`|([^\s:]+))\s*(?::|-)?\s*(.*)", text) if is_in_parameters else None
			)
			if heading or parameter:
				if lines:
					items.append((kind, name, doc_description("\n".join(lines))))
				if heading:
					kind = { "Arguments": "paramlist", "Errors": "heading:Errors", "Panics": "heading:Panics", "Returns": "return" }[heading[1]]
					name, lines = "", []
					is_in_parameters = heading[1] == "Arguments"
				else:
					kind, name, lines = "param", doc_name(parameter[1] or parameter[2]), [parameter[3]]
				continue
		if lang == "swift" and not fence and not is_code:
			if re.fullmatch(r"-\s*Parameters:\s*", text):
				is_in_parameters = True
				parameter_indent = -1
				continue
			swift: re.Match[str]|None = re.match(r"-\s*(?:Parameter\s+([^:]+)|Returns|Throws):\s*(.*)", text)
			if is_in_parameters:
				indent: int = len(line) - len(line.lstrip())
				if parameter_indent < 0:
					parameter_indent = indent
				is_in_parameters = indent >= parameter_indent
			if is_in_parameters:
				swift = re.match(r"-\s*([^:]+):\s*(.*)", text)
			if swift:
				if lines:
					items.append((kind, name, doc_description("\n".join(lines))))
				kind = "param" if swift[1] else "return" if "Returns:" in text else "throws"
				name = doc_name(swift[1] or "")
				lines = [swift[2]]
				continue
		if header:
			if lines or kind.startswith("overload:"):
				items.append((kind, name, doc_description("\n".join(lines))))
			kind, tail = header[1], header[2]
			annotation: str = ""
			if kind == "overload":
				overload += 1
			if kind == "desc" and lang in ("javascript", "typescript", "tsx"):
				kind = "description"
			if kind in ("brief", "details") or kind == "short" and lang in ("c", "cpp", "h"):
				kind = "summary"
			if (
				kind == "returns" or
				kind in ("arg", "argument") and lang in ("javascript", "typescript", "tsx", "javascriptreact", "typescriptreact")
			):
				kind = "return" if kind == "returns" else "param"
			name = ""
			if lang == "php" and kind == "param":
				php_type: re.Match[str]|None = re.match(r"^(\S+)\s+(?=[&.]*\$)", tail)
				if php_type:
					annotation = php_type[1]
					tail: str = tail[php_type.end():]
			if tail.startswith("{"):
				end: int = doc_bracket_end(tail, 0)
				annotation = "".join(doc_description(tail[:end]))
				tail = tail[end:].lstrip()
			if kind in ("param", "template", "tparam", "typeparam"):
				kind = "typeparam" if kind != "param" else "param"
				if tail.startswith("[in") and re.match(r"\[(?:in|out|in,out)\]", tail):
					tail = tail[tail.index("]") + 1:].lstrip()
				if tail.startswith("["):
					end: int = doc_bracket_end(tail, 0)
					optional: list[str] = tail[1:end - 1].split("=", 1)
					name = doc_name(optional[0])
					if len(optional) == 2:
						annotation += "\0default:" + optional[1]
					tail = tail[end:]
				else:
					match: re.Match[str]|None = re.match(r"(`[^`]+`|<[^>]+>|[^\s:]+)\s*:?(.*)", tail)
					if match:
						name, tail = doc_name(match[1]), match[2]
						if name.startswith("<") and name.endswith(">"):
							kind, name = "typeparam", name[1:-1]
			if annotation and tail.strip(" -\t"):
				name += "\0" + annotation
			if overload:
				kind = f"overload:{overload}" + (":" + kind if kind != "overload" else "")
			lines = [tail.lstrip(" -\t")]
		else:
			# タグ状の開始のエスケープを，タグへ読替えず本文として照合する
			lines.append(re.sub(r"^(\s*)\\(?=" + re.escape(marker) + ")", r"\1", line) if marker and not fence and not is_code else line)
		if re.match(r"[@\\]endcode\b", text) or is_code and "}" in text and "{@code" not in text:
			is_code = False
	if lines:
		items.append((kind, name, doc_description("\n".join(lines))))
	# 説明の無い補完骨格を内容として数えない項目の返戻
	result: list[DocItem] = []
	for kind, name, description in items:
		if kind == "summary":
			if lang in ("c", "cpp", "h"):
				description = tuple(re.sub(r"^\\([@\\])", r"\1", part) for part in description)
			elif lang == "java":
				description = tuple(part.replace("&#64;", "@").replace("&#92;", "\\") for part in description)
		if description or kind.startswith("overload:"):
			result.append((kind, name, description))
	# タグの解釈を終えた概要のリテラルだけを，エスケープ前の説明へ対応付ける
	return result

def doc_body(lang: str, raw: str) -> str:
	"""記法と継続行の装飾だけを除き，本文のコードの記号を保持する
	計算量：記録の文字数 B に対して O(B)
	引数：
		lang: 採用する記法の言語
		raw: １つのコメント記録
	戻値：
		記法枠を除いた本文
	"""
	if raw.startswith("=begin"):
		# 埋込ドキュメントの本文の返戻
		return re.sub(r"^=begin ?|\n=end ?", "\n", raw).strip("\n")
	if raw.startswith("/*"):
		opening: str = r"^/\*" if lang in ("go", "css", "scss") or re.match(r"^/\*{3,}", raw) else r"^/\*+"
		raw = re.sub(opening + r"|\*/$", "", raw)
		if lang not in ("javascript", "typescript", "tsx", "java") and raw.startswith("!"):
			raw = raw[1:]
		physical: list[str] = raw.splitlines()
		inner: list[str] = [line for line in physical[1:] if line.strip()]
		if inner and all(re.match(r"^\s*\*", line) for line in inner):
			raw = "\n".join(
				re.sub(r"^\s*\* ?", "", line) if idx or not line.startswith("**") else line for idx, line in enumerate(physical)
			)
	else:
		raw = re.sub(r"^(?://[!/]?|#+) ?" if not raw.startswith("////") else r"^// ?", "", raw)
	# 原文順に行末へ積んだ論理行を再び分離する
	from verify_case_variants import comment_enclosures
	_, protected, _ = comment_enclosures(raw)
	for match in reversed(list(re.finditer(r"[ \t]+//(?:[ \t]+|$)", raw))):
		if not any(protected[match.start():match.end()]):
			raw = raw[:match.start()] + "\n" + raw[match.end():]
	# 装飾だけを除いた本文の返戻
	return raw

def node_doc_body(lang: str, node: Node) -> str:
	"""物理行から読んだ装飾と，翻訳後の本文を重ねて除去せずに返す
	計算量：初回の本文量 B に対して O(B)，投影済の本文の取得は O(1)
	引数：lang: 採用する記法の言語，node: 独立に解析したコメント
	戻値：記法枠と物理行の装飾を除いた本文
	"""
	# Java のエスケープが作るコード内の行にも，装飾の除去を再適用しない
	return node.comment_text if node.comment_text is not None else doc_body(lang, node.text)

def doc_records(
	lang: str,
	roots: list[Node],
	should_include_ordinary: bool = False,
	documented_keys: set[str]|None = None,
	index: DocIndex|None = None,
	should_merge_empty_body: bool = False,
	should_read_converted_items: bool = False
) -> dict[str, list[DocItem]]:
	"""記法と構文上の所属から，同期する説明の独立した観測を返す
	計算量：インデックスと位置照会の費用 I・原文量 B・見出の正規化と間隙照合の費用 H・定義数 D に対して O(I+B+H+D log(B+1))
	引数：
		lang: 入力の言語
		roots: 原文範囲を持つ解析木
		should_include_ordinary: 通常形式へ変換した候補も読むか
		documented_keys: ドキュメントとして認識した説明対象の格納先
		index: 同じ入力で共有する範囲と所有者の観測
		should_merge_empty_body: 入力側で空の本体の独立コメントを説明へ統合するか
		should_read_converted_items: 候補の通常形式へ移したドキュメント項目を読むか
	戻値：
		説明対象の見出の正規形毎の原文順の項目
	"""
	index = index if index is not None else DocIndex(lang, roots)
	comments: list[Node] = index.comments
	parents: dict[Node, Node|None] = index.owners
	source: str = index.source
	code: str = index.code
	index.identify()
	retained_comments: set[Node] = set()
	directive_signature(roots, lang, retained_comments)
	groups: list[tuple[Node, list[str], bool, bool|None, bool]] = []
	variants: dict[tuple[Node, Node], Node] = {}
	documented: set[Node] = set()
	empty_bodies: set[Node] = set()
	if should_merge_empty_body:
		for definition, body_node in index.body_of.items():
			if definition.type not in TRANSFORM_FUNCTIONS.get(lang, set()) or body_node is None:
				continue
			begin: int = bisect_left(index.code_positions, body_node.start)
			end: int = bisect_left(index.code_positions, body_node.end)
			if end == begin + 2 and all(code[index.code_positions[begin + offset]] == brace for offset, brace in enumerate("{}")):
				empty_bodies.add(body_node)
	previous_end: int = 0
	for comment in comments:
		# 未展開のトークン列の説明は宣言の所属へ変換せず，配置枠の本文比較へ渡す
		if index.frame_of.get(comment) in index.opaque_frames:
			continue
		raw: str = comment.text
		if not raw:
			continue
		target: Node|None = index.following(comment.end)
		owner: Node|None = index.containing[comment]
		owner_body: Node|None = index.body_of.get(owner)
		is_inner: bool = lang == "rust" and raw.startswith(("//!", "/*!"))
		if is_inner:
			target = owner if owner is not None else roots[0] if roots else None
		elif owner_body is not None and comment.end <= owner_body.start:
			# 本体の手前の見出の説明は，後に在るメンバでなく其の定義へ統合する
			target = owner
		is_empty_body: bool = (
			not is_inner and owner_body in empty_bodies and owner_body.start < comment.start and comment.end < owner_body.end
		)
		if is_empty_body:
			# 閉境界が定義の行へ移る本体だけを，同じ定義の説明の末尾へ対応付ける
			target = owner
		container: Node|None = index.container_of.get(comment)
		if container is not None:
			is_in_header: bool = owner_body is not None and comment.end < owner_body.start + 1
			decorator_idx: int = bisect_right(index.decorator_starts, comment.start) - 1
			decorator: Node|None = index.decorators[decorator_idx] if -1 < decorator_idx else None
			is_in_decorator: bool = (
				decorator is not None and comment.end < decorator.end + 1 and target is not None and decorator.end < target.start + 1
			)
			if not (is_in_header or is_in_decorator) and (target is None or target.end > container.end):
				continue
		if owner is not None and (target is None or target.end > owner.end):
			target = owner
		if target is None:
			continue
		if not raw.startswith(("=begin",)):
			line_idx: int = bisect_left(index.newlines, comment.start) - 1
			line_start: int = index.newlines[line_idx] + 1 if -1 < line_idx else 0
			line_idx = bisect_left(index.newlines, comment.end)
			line_end: int = index.newlines[line_idx] if line_idx < len(index.newlines) else -1
			code_idx: int = bisect_left(index.line_positions, line_start)
			if is_empty_body:
				line_limit: int = len(code) if line_end < 0 else line_end
				if code_idx < len(index.line_positions) and index.line_positions[code_idx] < line_limit:
					# 文中・行末のコメントは独立した説明へ移さない
					continue
			if (
				code_idx < len(index.line_positions) and index.line_positions[code_idx] < comment.start and
				(not raw.startswith(("/**", "/*!")) or -1 < line_end and target.start > line_end)
			):
				continue
		body_node: Node|None = index.body_of.get(target)
		if owner is target and body_node is not None and comment.start >= body_node.start and not is_empty_body and not is_inner:
			continue
		if owner is not target and comment.source is not None:
			decorator_idx = bisect_right(index.decorator_starts, comment.start) - 1
			decorator = index.decorators[decorator_idx] if -1 < decorator_idx else None
			if (
				decorator is not None and
				not (comment.start < decorator.end < target.start + 1 and not code[decorator.end:target.start].strip())
			):
				decorator = None
			gap_begin: int = decorator.end if decorator is not None else comment.end
			gap_idx: int = bisect_left(index.code_positions, gap_begin)
			gap: str = (
				code[gap_begin:target.start] if gap_idx < len(index.code_positions) and index.code_positions[gap_idx] < target.start else ""
			)
			gap = gap.lstrip("\ufeff")
			gap = re.sub(r"template\s*<[^;{}]+>|(?:public|private|protected|static|export|final|async)\s+", "", gap)
			gap = re.sub(r"@[\w.]+(?:\([^()]*\))?\s*", "", gap)
			if gap.strip():
				continue
		body: str = node_doc_body(lang, comment)
		# 保持記録は説明へ混ぜず，行グループと同期する区間の境界にする
		if comment in retained_comments or raw.startswith("#!") and not bisect_left(index.newlines, comment.start):
			retained_body: str = "!" + raw[2:].lstrip(" \t") if raw.startswith("#!") else body
			groups.append((target, [retained_body], False, None, is_inner))
			previous_end = comment.end
			continue
		if lang == "go" and not raw.startswith("/*"):
			blank_idx: int = bisect_left(index.blank_starts, comment.end)
			if blank_idx < len(index.blanks) and index.blanks[blank_idx][1] < target.start + 1:
				continue
		if target not in index.parts:
			decl_parts: list[Node] = [child for child in target.children if child.type in ("init_declarator", "variable_declarator")]
			index.parts[target] = decl_parts, [child.start for child in decl_parts]
		decl_parts, part_starts = index.parts[target]
		part_idx: int = bisect_right(part_starts, comment.start) - 1
		part: Node|None = decl_parts[part_idx] if -1 < part_idx and comment.start < decl_parts[part_idx].end else None
		if part is None and owner is target:
			part_idx = bisect_left(part_starts, comment.end)
			part = decl_parts[part_idx] if part_idx < len(decl_parts) else None
		if part is not None:
			variant_key: tuple[Node, Node] = target, part
			if variant_key not in variants:
				variant: Node = Node("", target.kind, target.type)
				variant.start, variant.end = target.start, target.end
				variant.children: list[Node] = [
					child for child in target.children if child.type not in ("init_declarator", "variable_declarator") or child is part
				]
				parents[variant] = parents[target]
				index.occurrences[variant] = index.occurrences[target]
				index.template_of[variant] = index.template_of[target]
				variants[variant_key] = variant
			target = variants[variant_key]
		is_doc: bool = not is_empty_body and (raw.startswith(("/**", "/*!", "///", "//!", "=begin")) or lang in ("go", "ruby"))
		if is_doc or is_empty_body:
			documented.add(target)
		if not body.strip():
			continue
		if is_empty_body:
			index.merged_body_comments.add(comment)
		is_line: bool = not raw.startswith(("/*", "=begin"))
		if (
			groups and groups[-1][0] is target and groups[-1][2:] == (True, is_doc, is_inner) and is_line and
			not code[previous_end:comment.start].strip() and not re.search(r"\n[ \t]*\n", source[previous_end:comment.start])
		):
			groups[-1][1].append(body)
		else:
			groups.append((target, [body], is_line, is_doc, is_inner))
		previous_end = comment.end
	found: dict[tuple[str, int], list[DocItem]] = {}
	prefix: dict[tuple[str, int], list[DocItem]] = {}
	suffix: dict[tuple[str, int], list[DocItem]] = {}
	intervals: Counter[str] = Counter()
	boundaries: dict[tuple[str, int], DocItem] = {}
	key_order: dict[str, int] = {}
	for target, bodies, _, is_doc, is_inner in groups:
		if not should_include_ordinary and target not in documented:
			continue
		key: str = index.header(target) + ("|inner" if is_inner else "")
		key = key.replace(" (parameter_list (parameter_declaration (primitive_type 'void')))", "")
		parent_id: int = index.owner_ids.get(parents[target], 0)
		template_node: Node|None = index.template_of.get(target)
		template: str = " ".join(
			sexpr(norm) for child in template_node.children if child.type == "template_parameter_list" for norm in normalize(child, lang)
		) if template_node is not None else ""
		key = str(parent_id) + "#" + str(index.occurrences[target]) + "|" + template + key
		if documented_keys is not None and target in documented:
			documented_keys.add(key)
		index.targets[key] = target
		key_order.setdefault(key, len(key_order))
		if is_doc is None:
			intervals[key] += 1
			boundaries[key, intervals[key]] = "\0boundary", "", doc_description("\n".join(bodies))
			continue
		interval: tuple[str, int] = key, intervals[key]
		items: list[DocItem] = (
			doc_items(lang, "\n".join(bodies)) if
			is_doc or lang == "csharp" or should_read_converted_items and any(re.match(r"[ \t]*[@\\]", body) for body in bodies) else
			[("summary", "", doc_description("\n".join(bodies)))]
		)
		if is_doc:
			found.setdefault(interval, []).extend(items)
		elif interval in found:
			suffix.setdefault(interval, []).extend(items)
		else:
			prefix.setdefault(interval, []).extend(items)
	result: dict[str, list[DocItem]] = {}
	for key in key_order:
		items = []
		for ordinal in range(intervals[key] + 1):
			interval = key, ordinal
			if interval in boundaries:
				items.append(boundaries[interval])
			items.extend((*prefix.get(interval, []), *found.get(interval, []), *suffix.get(interval, [])))
		result[key] = items
	# 概要への統合の位置と保持記録の境界を確定した項目の返戻
	return result

def doc_intervals(items: list[DocItem]) -> list[list[DocItem]]:
	"""保持記録で説明項目を分け，本文と項目順の比較範囲を共有する
	計算量：項目数 N に対して O(N)
	引数：items: 原文順の説明項目と保持記録
	戻値：境界を先頭に保持した区間の並び
	"""
	result: list[list[DocItem]] = [[]]
	for item in items:
		if item[0] == "\0boundary":
			result.append([])
		result[-1].append(item)
	# 空の区間も省かず，境界の出現数と順を保持する
	return result

def doc_target_header(target: str) -> str:
	"""所有者の区切と，シグネチャの値の中の記号を区別する
	計算量：見出と所有者の文字数 B に対して O(B)
	引数：
		target: 説明対象の正規形
	戻値：
		説明対象其の物の見出
	"""
	begin: int = 0
	pos: int = 0
	while pos < len(target):
		if target[pos] == "(":
			pos = doc_sexpr_end(target, pos)
			continue
		if target[pos] == "|":
			begin = pos + 1
		pos += 1
	# 構造の外の最後の境界以降の見出の返戻
	return target[begin:]

def doc_param_position(target: str, name: str, kind: str) -> int:
	"""シグネチャの比較名の並びから，説明項目の位置を返す
	計算量：見出の文字数 B に対して O(B)
	引数：
		target: 説明対象の見出の正規形
		name: 項目の比較名
		kind: 値又は型の仮引数の区分
	戻値：
		対応する名前の位置．未知の項目は最後
	"""
	import ast
	original_len: int = len(target)
	name = name.split("\0", 1)[0]
	header: str = doc_target_header(target)
	offset: int = len(target) - len(header)
	target = header
	start: int = min(
		(
			match.start() for match in re.finditer(
				r"\((?:formal_parameters|function_value_parameters|method_parameters|parameter_list|parameters|parameter)\b",
				target
			)
		),
		default=0
	)
	if kind == "param":
		target = target[start:]
		types: str = "identifier|field_identifier|simple_identifier|name"
	elif kind == "typeparam":
		groups: list[str] = []
		for match in re.finditer(r"\((?:type_parameters|type_parameter_list|template_parameter_list)\b", target):
			groups.append(target[match.start():doc_sexpr_end(target, match.start())])
		target = " ".join(groups)
		start = 0
		types = "identifier|simple_identifier|type_identifier|name"
	else:
		# 戻値には名前の位置を使わない返戻
		return original_len
	for match in re.finditer(r"\((?:" + types + r") ('(?:[^'\\]|\\.)*')\)", target):
		if doc_name(ast.literal_eval(match[1])).lstrip("$&.") in [part.split(".", 1)[0] for part in name.lstrip("$&.").split(",")]:
			# 同じ束縛の位置の返戻
			return offset + start + match.start()
	# 不存在の項目の末尾位置の返戻
	return original_len

def doc_declarators(target: str) -> tuple[str, tuple[str, ...]]:
	"""宣言の共通見出と，表現の分割・統合に対応する宣言子を返す
	計算量：見出の文字数 B に対して O(B)
	引数：
		target: 説明対象の正規形
	戻値：
		共通の見出と原文順の宣言子
	"""
	if "(var_spec " in target:
		match: re.Match[str]|None = re.search(r"\(var_spec ((?:\(identifier '(?:[^'\\]|\\.)*'\)\s*)+)", target)
		if match is not None:
			bindings: tuple[str, ...] = tuple(item[0] for item in re.finditer(r"\(identifier '(?:[^'\\]|\\.)*'\)", match[1]))
			# 宣言の共通部分と束縛の列の返戻
			return target[:match.start(1)] + target[match.end(1):], bindings
	parts: list[str] = []
	common: list[str] = []
	begin: int = 0
	for match in re.finditer(r"\((?:init_declarator|variable_declarator)\b", target):
		if match.start() < begin:
			continue
		end: int = doc_sexpr_end(target, match.start())
		common.append(target[begin:match.start()].rstrip())
		parts.append(target[match.start():end])
		begin = end
	common.append(target[begin:])
	# 宣言子以外の所属・型・修飾子を保った対応の返戻
	return "".join(common), tuple(parts)

def doc_member_frames(
	lang: str,
	roots: list[Node],
	should_include_ordinary: bool = False,
	index: DocIndex|None = None
) -> tuple[dict[str, list[tuple[str, ...]]], dict[str, list[tuple[str, ...]]]]:
	"""通常形式へ変換する値のメンバの説明と，配置先の独立した本文を返す
	計算量：インデックスと位置照会の費用 I・本文量 B・見出と配置枠の正規化費用 H に対して O(I+B+H)
	引数：
		lang: 比較する言語
		roots: 原文範囲を共有する解析木
		should_include_ordinary: 通常形式へ変換した候補のメンバの所属も読むか
		index: 同じ入力で共有する範囲と所有者の観測
	戻値：
		構文上の配置枠毎の入力ドキュメントと全ての独立した候補本文
	"""
	index = index if index is not None else DocIndex(lang, roots)
	index.identify()
	keys: dict[Node, str] = {}
	counts: Counter[tuple[int, str]] = Counter()
	# コメントの無い木では配置枠を正規化せず，深い式の全文の不要な複写を避ける
	used: set[Node] = set()
	assigned: list[tuple[Node, Node, Node|None]] = []
	member_lists: dict[Node, tuple[list[Node], list[int]]] = {}
	for comment in index.comments:
		frame: Node|None = index.frame_of.get(comment)
		container: Node|None = index.container_of.get(comment)
		if frame is None:
			previous_idx: int = bisect_right(index.frame_ends, comment.start) - 1
			previous: Node|None = index.finished_frames[previous_idx] if previous_idx > -1 else None
			if previous in index.opaque_frames:
				code_idx: int = bisect_left(index.code_positions, previous.end)
				if (
					bisect_left(index.newlines, previous.end) == bisect_left(index.newlines, comment.start) and
					(code_idx == len(index.code_positions) or index.code_positions[code_idx] > comment.start - 1)
				):
					frame = previous
		idx: int = bisect_left(index.frame_starts, comment.end)
		following: Node|None = index.ordered_frames[idx] if idx < len(index.frame_starts) else None
		if frame is not None and following is not None and following.end <= frame.end and container is None:
			frame = following
		if frame is None:
			frame = following
			decl_idx: int = bisect_left(index.starts, comment.end)
			if frame is not None and decl_idx < len(index.starts) and index.starts[decl_idx] < frame.start:
				continue
		if frame is None:
			continue
		member: Node|None = None
		if container is not None:
			if container not in member_lists:
				children: list[Node] = [child for child in container.children if child.kind == "N" and not is_comment(child.type)]
				member_lists[container] = children, [child.start for child in children]
			children, starts = member_lists[container]
			idx = bisect_left(starts, comment.end)
			member = children[idx] if idx < len(children) else None
		used.add(frame)
		assigned.append((comment, frame, member))
	for frame in index.frames if used else ():
		# 同じ見出の別の出現を区別する為，コメント無しの配置枠も数える
		parent_id: int = index.owner_ids.get(index.owners[frame], 0)
		header: str = " ".join(sexpr(norm) for norm in normalize(frame, lang))
		identity: tuple[int, str] = parent_id, header
		counts[identity] += 1
		keys[frame] = str(parent_id) + "|" + header + "#" + str(counts[identity])
	docs: dict[str, list[tuple[str, ...]]] = {}
	all_comments: dict[str, list[tuple[str, ...]]] = {}
	for comment, frame, member in assigned:
		body: tuple[str, ...] = doc_description(node_doc_body(lang, comment))
		all_comments.setdefault(keys[frame], []).append(body)
		if frame in index.opaque_frames:
			if should_include_ordinary or comment.text.startswith(("/**", "/*!", "///", "//!")):
				docs.setdefault(keys[frame], []).append(("\0target:", *body))
			continue
		if (
			(should_include_ordinary or comment.text.startswith(("/**", "/*!", "///"))) and member is not None and
			member.type in ("field_initializer", "initializer_pair", "pair")
		):
			identity_text: str = "\0target:" + " ".join(sexpr(norm) for norm in normalize(member, lang))
			docs.setdefault(keys[frame], []).append((identity_text, *body))
	# 値のメンバと通常形式の配置枠の独立した読取の返戻
	return docs, all_comments

def is_doc_member_preserved(
	lang: str,
	before: list[Node],
	after: list[Node],
	input_index: DocIndex,
	candidate_index: DocIndex
) -> bool:
	"""直接の記法を失っても，値のメンバの説明を同じ所有構文で照合する
	計算量：インデックス構築・正規化の費用 I・本文量 B・宣言子との文字照合費用 R に対して O(I+B+R)
	引数：
		lang: 比較する言語
		before: 入力の木
		after: 候補の木
		input_index: 入力側の共有する観測
		candidate_index: 候補側の独立した観測
	戻値：
		説明の内容・順序・出現数と配置枠が保たれて居るなら真
	"""
	if False not in input_index.members:
		input_index.members[False] = doc_member_frames(lang, before, False, input_index)
	if True not in candidate_index.members:
		candidate_index.members[True] = doc_member_frames(lang, after, True, candidate_index)
	expected: dict[str, list[tuple[str, ...]]] = { key: list(items) for key, items in input_index.members[False][0].items() }
	originals: dict[str, list[tuple[str, ...]]] = { key: list(items) for key, items in input_index.members[False][1].items() }
	actual, candidates = candidate_index.members[True]
	# 宣言の分割・統合でも，説明する値を含む宣言子との対応を保つ
	candidate_parts_index: dict[tuple[str, str], str] = {}
	for key in candidates:
		common, parts = doc_declarators(key)
		for part in parts:
			candidate_parts_index[common, part] = key
	for key in list(expected):
		if key in candidates:
			continue
		common, parts = doc_declarators(key)
		members: list[str] = [document[0][len("\0target:"):] for document in expected[key]]
		placements: set[str] = {
			candidate_parts_index[common, part] for part in parts if (common, part) in candidate_parts_index and
			any(member in part for member in members)
		}
		if len(placements) == 1:
			placement: str = next(iter(placements))
			expected.setdefault(placement, []).extend(expected.pop(key))
			originals.setdefault(placement, []).extend(originals.pop(key))
	for key, documents in expected.items():
		protected: set[str] = { part for document in documents for part in document if not part.startswith("\0target:") }
		left: tuple[str, ...] = tuple(part for body in originals[key] for part in body if part in protected)
		right: tuple[str, ...] = tuple(part for body in candidates.get(key, []) for part in body if part in protected)
		original_parts: tuple[str, ...] = tuple(part for body in originals[key] for part in body)
		candidate_parts: tuple[str, ...] = tuple(part for body in candidates.get(key, []) for part in body)
		# 不透明な配置枠では，元の通常コメントも通常形式で現れる本文の根拠とする
		available: list[tuple[str, ...]] = (
			[("\0target:", *body) for body in originals[key]] if
			any(document and document[0] == "\0target:" for document in documents) else
			documents
		)
		if (
			left != right or bool(Counter(candidate_parts) - Counter(original_parts)) or
			bool(Counter(actual.get(key, [])) - Counter(available))
		):
			# 所有構文を失った説明又は欠落・複製の拒否の返戻
			return False
	for key, documents in actual.items():
		if key not in expected:
			original_parts = tuple(part for body in originals.get(key, []) for part in body)
			new_parts: tuple[str, ...] = tuple(part for document in documents for part in document if not part.startswith("\0target:"))
			if bool(Counter(new_parts) - Counter(original_parts)):
				# 説明の新しい出現を入力から導けない場合の拒否の返戻
				return False
	# 通常形式を含む保持条件の一致の返戻
	return True

def css_comment_signature(lang: str, roots: list[Node]) -> tuple[tuple[bool, str], ...]:
	"""値の中のコメント状の記号を除き，CSS・SCSS の説明本文を原文順に読む
	計算量：原文の長さ B とコメント本文の正規化費用 P に対して O(B+P)
	引数：
		lang: コメント記法を解釈する言語
		roots: コメントを独立に取得する原文の木
	戻値：本文の論理行と，配信物へ残す記法の指定
	"""
	comments: list[str] = []
	css_value_source(lang, "".join(root.text for root in roots), comments=comments)
	result: list[tuple[bool, str]] = []
	for raw in comments:
		parts: tuple[str, ...] = doc_description(doc_body(lang, raw))
		is_loud: bool = raw.startswith("/*!")
		result.extend((is_loud, part) for part in parts)
		if is_loud and not parts:
			result.append((True, ""))
	# 本文の改変・欠落・複製・順序の違いを検出する記録の返戻
	return tuple(result)

def documentation_preserved(lang: str, before: list[Node], after: list[Node], input_index: DocIndex|None = None) -> bool:
	"""入力側の説明の期待と候補の独立した読取結果を照合する
	候補の項目の再同期は行わず，説明対象・内容・順序・出現数を比較する
	計算量：インデックス構築・正規化費用 I・本文量 B・宣言対応の照合費用 R・仮引数数 P・見出量 H に対して O(I+B+R+PH+P log P)
	引数：
		lang: 比較する言語
		before: 入力の木
		after: 候補の木
		input_index: 行幅間で共有する入力側の観測
	戻値：
		ドキュメントの保持契約を満たすなら真
	"""
	if lang in ("css", "scss"):
		# ドキュメントの同期を持たない言語でも，通常コメントの本文を照合する
		return css_comment_signature(lang, before) == css_comment_signature(lang, after)
	if lang in DOC_OTHER_PATHS:
		# 値又はディレクティブとして既に比較する形式の返戻
		return True
	input_index = input_index if input_index is not None else DocIndex(lang, before)
	candidate_index: DocIndex = DocIndex(lang, after, input_index.identities)
	if input_index.records is None:
		input_keys: set[str] = set()
		input_index.records: tuple[dict[str, list[DocItem]], set[str]] = (
			doc_records(lang, before, True, input_keys, input_index, True),
			input_keys
		)
	original, input_keys = input_index.records
	candidate_keys: set[str] = set()
	converted: dict[str, list[DocItem]] = (
		doc_records(lang, after, True, candidate_keys, candidate_index, should_read_converted_items=True)
	)
	expected: dict[str, list[DocItem]] = { key: list(items) for key, items in original.items() if key in input_keys }
	for key, items in original.items():
		if input_index.targets[key].type in TRANSFORM_FUNCTIONS.get(lang, set()) and any(item[0] != "\0boundary" for item in items):
			# 定義へ統合する通常の説明も，入力側の保持すべき内容として確定する
			expected.setdefault(key, list(items))
	actual: dict[str, list[DocItem]] = { key: list(items) for key, items in converted.items() if key in candidate_keys }
	original: dict[str, list[DocItem]] = { key: list(items) for key, items in original.items() }
	# 通常の独立コメントへ変換された宣言の分割・統合だけを対応付ける
	# 同行に為ったメンバの説明だけを，通常形式の所有構文へ対応付ける
	candidate_owners: dict[int|None, str] = {
		candidate_index.owner_ids.get(node): key for key, node in candidate_index.targets.items()
	}
	collapsed: dict[str, str] = {}
	for target in expected:
		if target in converted:
			continue
		node: Node = input_index.targets[target]
		parent: Node|None = input_index.owners[node]
		parent_id: int|None = input_index.owner_ids.get(parent)
		candidate: str|None = target.removesuffix("|inner") if target.endswith("|inner") else candidate_owners.get(parent_id)
		if candidate not in converted or target.endswith("|inner") and candidate in candidate_keys:
			candidate = None
		if candidate is not None:
			body: Node|None = candidate_index.body_of.get(candidate_index.targets[candidate])
			if body is not None and "\n" not in candidate_index.code[body.start:body.end]:
				collapsed[target] = candidate
	for mapping in expected, original:
		for target, candidate in collapsed.items():
			if target in mapping:
				mapping.setdefault(candidate, []).extend(mapping.pop(target))
	declarators: dict[tuple[str, str], list[tuple[str, frozenset[str]]]] = {}
	for candidate in converted:
		common, parts = doc_declarators(candidate)
		if parts:
			part_set: frozenset[str] = frozenset(parts)
			for part in parts:
				declarators.setdefault((common, part), []).append((candidate, part_set))
	remapped: dict[str, str] = {}
	for target in original:
		if target in converted:
			continue
		common, parts = doc_declarators(target)
		if parts:
			part_set = frozenset(parts)
			candidate = next((key for key, other_parts in declarators.get((common, parts[0]), []) if part_set <= other_parts), None)
			if candidate is not None:
				remapped[target] = candidate
	for mapping in expected, original:
		for target, candidate in remapped.items():
			if target in mapping:
				mapping.setdefault(candidate, []).extend(mapping.pop(target))
	if input_index.ordinary is None:
		input_index.ordinary: Counter[str] = Counter()
		for node, _, _ in source_leaves(before):
			if (
				is_comment(node.type) and node not in input_index.merged_body_comments and
				not node.text.startswith(("/**", "/*!", "///", "//!", "=begin"))
			):
				input_index.ordinary.update(doc_description(node_doc_body(lang, node)))
	ordinary: Counter[str] = input_index.ordinary.copy()
	# パーサーが零幅の記録を出した場合も，独立に読んだ通常形式の本文を数える
	unrecorded: list[str] = [
		part for target, items in original.items() if target not in expected for item in items for part in item[2]
	]
	for part, count in Counter(unrecorded).items():
		ordinary[part] = max(count, ordinary[part])
	# 所属を確定した説明は其の対象で消費し，未対応の本文と混ぜない
	ordinary.subtract(unrecorded)
	ordinary = +ordinary
	# 説明の無い自動生成の概要と骨格は既存の内容を改変しない
	for target in set(expected) | set(actual):
		left: list[DocItem] = list(original.get(target, expected.get(target, [])))
		if target not in expected:
			items: list[DocItem] = actual.get(target, [])
			if [item for item in left if item[0] == "\0boundary"] != [item for item in items if item[0] == "\0boundary"]:
				# 自動生成の説明でも既存の保持記録の境界を変えない
				return False
			parts: list[str] = [part for item in items if item[0] != "\0boundary" for part in item[2]]
			available: Counter[str] = Counter(part for item in left if item[0] != "\0boundary" for part in item[2])
			default_match: re.Match[str]|None = re.search(
				r"\(" + ("field_identifier" if "(method_declaration " in target else "identifier") + r" '([^']+)'\)",
				doc_target_header(target)
			)
			default: list[str] = [default_match[1]] if default_match else []
			for part in parts:
				if lang == "go" and part in default:
					default.remove(part)
					continue
				if available[part]:
					available[part] -= 1
					continue
				if not ordinary[part]:
					# 入力に無い説明を生成した候補の拒否の返戻
					return False
				ordinary[part] -= 1
			continue
		right: list[DocItem] = list(converted.get(target, []) if target in expected else actual.get(target, []))
		for items in left, right:
			for idx, (kind, name, description) in enumerate(items):
				if (
					kind == "param" and doc_param_position(target, name, "typeparam") < len(target) and
					doc_param_position(target, name, "param") == len(target)
				):
					items[idx] = "typeparam", name, description
		left_intervals: list[list[DocItem]] = doc_intervals(left)
		right_intervals: list[list[DocItem]] = doc_intervals(right)
		if len(left_intervals) != len(right_intervals):
			# 保持記録の境界の追加・欠落の拒否
			return False
		for left, right in zip(left_intervals, right_intervals):
			# シグネチャの順に入力側だけを同期する
			# 候補の項目順は読み取った儘にする
			inner: list[DocItem] = [item for item in left if item[0].startswith("overload:")]
			left = [item for item in left if not item[0].startswith("overload:")]
			params: list[DocItem] = [item for item in left if item[0] in ("param", "typeparam", "return")]
			params.sort(
				key=lambda item: ({ "typeparam": 0, "param": 1, "return": 2 }[item[0]], doc_param_position(target, item[1], item[0]))
			)
			insertion: int = next((idx for idx, item in enumerate(left) if item[0] in ("param", "typeparam", "return")), len(left))
			left = [
				*[item for item in left[:insertion] if item[0] not in ("param", "typeparam", "return")],
				*params,
				*[item for item in left[insertion:] if item[0] not in ("param", "typeparam", "return")]
			]
			left.extend(inner)
			left = [
				("summary", "", tuple(part for item in left if item[0] == "summary" for part in item[2])),
				*[item for item in left if item[0] != "summary"]
			]
			right = [
				("summary", "", tuple(part for item in right if item[0] == "summary" for part in item[2])),
				*[item for item in right if item[0] != "summary"]
			]
			if lang == "go" and left[0][2] != right[0][2]:
				name_match: re.Match[str]|None = re.search(
					r"\(" + ("field_identifier" if "(method_declaration " in target else "identifier") + r" '([^']+)'\)",
					doc_target_header(target)
				)
				if name_match:
					for parts in (*left[0][2], name_match[1]), (name_match[1], *left[0][2]):
						if right[0][2] == parts:
							right[0] = left[0]
							break
			if left != right:
				# 既存の説明項目の破壊の返戻
				return False
	# 値のメンバ等から通常の独立コメントへ移した物も，所有構文毎に照合する
	return is_doc_member_preserved(lang, before, after, input_index, candidate_index)

# 束縛の開始と関数型の契約を保つ独立した正誤対
SELF_CHECK_PAIRS.extend(
	[("c", "引数未指定への変更：void use(int (*p)(void));", "void use(int (*p)(void));", "void use(int (*p)());", False)]
)

# 許可変換の独立した構文表（候補の差を除去せず，入力の保存条件を照合する）
# 型・書込・関数の境界から手で定めた許可変換の正誤対
SELF_CHECK_PAIRS.extend(
	[
		(
			"javascript",
			"反復の既存束縛への書込：in",
			"function f(){let x=1;for(x in {a:1}){}return x;}",
			"function f(){const x=1;for(x in {a:1}){}return x;}",
			False
		)
	]
)

SELF_CHECK_PAIRS.extend([("kotlin", "束縛の更新：++x", "fun f(){var x=1;++x;println(x)}", "fun f(){val x=1;++x;println(x)}", False)])

TRANSFORM_FUNCTIONS: dict[str, set[str]] = {
	"c": { "function_definition" },
	"cpp": { "function_definition" },
	"csharp": { "accessor_declaration", "constructor_declaration", "destructor_declaration", "method_declaration" },
	"java": { "constructor_declaration", "method_declaration" },
	"go": { "function_declaration", "method_declaration" },
	"rust": { "function_item" },
	"kotlin": { "function_declaration", "secondary_constructor", "setter" },
	"swift": { "computed_setter", "deinit_declaration", "function_declaration", "init_declaration" },
	"php": { "function_definition", "method_declaration" },
	"javascript": { "function_declaration", "method_definition" },
	"typescript": { "function_declaration", "method_definition" },
	"tsx": { "function_declaration", "method_definition" },
	"ruby": { "method", "singleton_method" },
	"python": { "function_definition" }
}

TRANSFORM_BODIES: set[str] = {
	"block",
	"body_statement",
	"compound_statement",
	"constructor_body",
	"function_body",
	"statement_block"
}

TRANSFORM_SEQUENCES: set[str] = TRANSFORM_BODIES | { "else", "statement_list", "statements", "then" }
TRANSFORM_TYPE_BODIES: frozenset[str] = frozenset(("class_body", "object_literal"))

TRANSFORM_RETURNS: set[str] = {
	"control_transfer_statement",
	"jump_expression",
	"return",
	"return_expression",
	"return_statement"
}

TRANSFORM_DECLARATORS: dict[str, set[str]] = {
	"declaration": { "array_declarator", "identifier", "init_declarator", "pointer_declarator", "reference_declarator" },
	"field_declaration": {
		"array_declarator",
		"bitfield_declarator",
		"field_identifier",
		"pointer_declarator",
		"reference_declarator",
		"variable_declarator"
	},
	"function_static_declaration": { "static_variable_declaration" },
	"let_declaration": { "identifier" },
	"lexical_declaration": { "variable_declarator" },
	"local_variable_declaration": { "variable_declarator" },
	"variable_declaration": { "variable_declarator" }
}

def transform_nodes(nodes: tuple[NormNode, ...]) -> Iterator[NormNode]:
	"""正規形のノードを明示した作業列で辿る
	計算量：ノード数 N に対して O(N)
	引数：nodes: 読取対象の並び
	戻値：原文順のノード
	"""
	pending: list[NormNode] = list(reversed(nodes))
	while pending:
		node: NormNode = pending.pop()
		yield node
		pending.extend(reversed(node[2]))
	# 終了
	return

def has_sequential_binding_change(names: list[NormNode], values: tuple[NormNode, ...]) -> bool:
	"""宣言全体の後から始まる束縛を，初期化子の間へ移せるか調べる
	計算量：名前数 N・初期化子の総ノード数 V に対して O(N+V)
	引数：names: 宣言順の名前，values: 対応する初期化子
	戻値：先行する束縛名の参照又は未確認の束縛が在れば真
	"""
	introduced: set[str] = set()
	for name, value in zip(names, values):
		if name[2] or not name[1] or any(not part[2] and part[1] in introduced for part in transform_nodes((value,))):
			# 未確認の名前又は束縛先を変え得る初期化子の拒否
			return True
		if name[1] != "_":
			introduced.add(name[1])
	# 先行する宣言の新しい束縛を参照しない事の返戻
	return False

def declaration_parts(node: NormNode) -> list[NormNode]:
	"""共有指定と宣言子毎の型・初期化を保ち，宣言の選択表現を照合単位へ分ける
	計算量：直接の子数 N・初期化子の総ノード数 V・指定子の総テキスト長 B に対して最悪 O(N²+NB+V)
	引数：node: 宣言又は其の他の構文
	戻値：評価順を保つ宣言子の列．対象外の構文は元の１項目
	"""
	kind: str
	text: str|None
	children: tuple[NormNode, ...]
	kind, text, children = node
	if kind == "global_statement" and len(children) == 1 and children[0][0] == "local_declaration_statement":
		# 最上位の文の所属を保つ宣言子列の返戻
		return [(kind, text, (part,)) for part in declaration_parts(children[0])]
	if (
		kind == "export_statement" and children and children[-1][0] in ("lexical_declaration", "variable_declaration") or
		kind in ("field_declaration", "local_declaration_statement") and children and children[-1][0] == "variable_declaration"
	):
		# export 又は C# の文の所属を保ち，内側の宣言子だけを分ける
		return [(kind, text, (*children[:-1], part)) for part in declaration_parts(children[-1])]
	if (
		kind in ("const_declaration", "var_declaration") and len(children) == 2 and children[1][0] in ("const_spec", "var_spec") and
		not any(part[1] == "iota" for part in transform_nodes(children))
	):
		spec: tuple[NormNode, ...] = children[1][2]
		equals: int = next((idx for idx, child in enumerate(spec) if child[0] == "="), len(spec))
		names: list[NormNode] = [child for child in spec[:equals] if child[0] == "identifier"]
		types: tuple[NormNode, ...] = tuple(child for child in spec[:equals] if child[0] != "identifier")
		values: tuple[NormNode, ...] = spec[equals + 1:]
		if len(values) == 1 and values[0][0] == "expression_list":
			values = values[0][2]
		if names and (not values or len(names) == len(values)) and not has_sequential_binding_change(names, values):
			# 多値の呼出を宣言子毎の評価へ読み替えない，１対１の初期化の返戻
			return [
				(kind, text, (children[0], (children[1][0], None, (name, *types, *((spec[equals], values[idx]) if values else ()))))) for idx,
				name in enumerate(names)
			]
	if kind == "short_var_declaration" and len(children) == 3 and children[0][0] == children[2][0] == "expression_list":
		if len(children[0][2]) == len(children[2][2]) and not has_sequential_binding_change(list(children[0][2]), children[2][2]):
			# 多値を返す単一の呼出を分割せず，明示した同数の値だけを対応付ける
			return [
				(kind, text, (("expression_list", None, (name,)), children[1], ("expression_list", None, (value,)))) for name, value in
				zip(children[0][2], children[2][2])
			]
	if kind == "property_declaration":
		pattern_types: set[str] = { "pattern", "variable_declaration" }
		starts: list[int] = [idx for idx, child in enumerate(children) if child[0] in pattern_types]
		if starts:
			prefix: tuple[NormNode, ...] = children[:starts[0]]
			parts: list[tuple[NormNode, ...]] = [children[start:end] for start, end in zip(starts, [*starts[1:], len(children)])]
			shared_type: tuple[NormNode, ...] = ()
			for idx in range(len(parts) - 1, -1, -1):
				part: tuple[NormNode, ...] = parts[idx]
				if part[0][0] != "pattern" or any(child[0] == "=" for child in part):
					shared_type = ()
				elif len(part) == 2 and part[1][0] == "type_annotation":
					shared_type = part[1:]
				elif len(part) == 1:
					parts[idx] += shared_type
			# 初期化の無い Swift の連続した名前に限り，末尾の型注記を共有する
			return [(kind, text, prefix + part) for part in parts]
	if kind in TRANSFORM_DECLARATORS:
		starts = [idx for idx, child in enumerate(children) if child[0] in TRANSFORM_DECLARATORS[kind]]
		if starts:
			prefix = children[:starts[0]]
			if kind in ("declaration", "local_variable_declaration", "variable_declaration"):
				# 基底型の指定子の位置だけを比較する
				# ポインタ側の修飾は宣言子内に残す
				qualifiers: list[NormNode] = [child for child in prefix if child[0] == "type_qualifier"]
				prefix = tuple(sorted(qualifiers, key=sexpr)) + tuple(child for child in prefix if child[0] != "type_qualifier")
			# 宣言子と初期化の順序を保持した列の返戻
			return [(kind, text, prefix + children[start:end]) for start, end in zip(starts, [*starts[1:], len(children)])]
	# 対象外の構文の返戻
	return [node]

def transform_binding(node: NormNode) -> str:
	"""単純な宣言子の束縛名を，型名や初期化子から区別する
	計算量：宣言子のノード数 N に対して O(N)
	引数：node: 宣言子毎に分けた宣言
	戻値：単純な束縛の名前．分解等の未確認の形は空
	"""
	children: tuple[NormNode, ...] = node[2]
	for child in children:
		if child[0] == "=":
			break
		if child[0] in {
			"identifier",
			"init_declarator",
			"pattern",
			"static_variable_declaration",
			"var_spec",
			"variable_declaration",
			"variable_declarator"
		}:
			binding: NormNode = child
			while binding[0] != "identifier" and binding[0] != "simple_identifier" and binding[2]:
				binding = binding[2][0]
			if binding[0] in ("identifier", "simple_identifier", "name"):
				# 宣言子の束縛名の返戻
				return binding[1] or ""
	# 未確認の束縛の返戻
	return ""

class TransformIndex:
	"""関数の包含範囲と，捕捉・未確定の効果の照会を共有する"""

	def count(self: TransformIndex, function: NormNode, positions: list[int]) -> int:
		"""関数の全範囲に属する出現数を返す
		計算量：出現数 M に対して O(log(M+1))
		引数：self: 共有する位置表，function: 対象の関数，positions: 原文順の出現位置
		戻値：対象範囲の出現数
		"""
		begin, end = self.spans[id(function)]
		# 半開区間の出現数の返戻
		return bisect_left(positions, end) - bisect_left(positions, begin)

	def __init__(self: TransformIndex, roots: tuple[NormNode, ...]) -> None:
		"""全構造を１度辿り，文字列を関数毎に複製しない位置表を作る
		計算量：ノード数 N・名前の総文字数 B に対して O(N+B)
		引数：self: 構築する位置表，roots: 入力の正規形
		"""
		self.spans: dict[int, tuple[int, int]] = {}
		self.mentions: dict[str, list[int]] = {}
		self.attributes: list[int] = []
		self.dynamic: list[int] = []
		self.macros: list[int] = []
		self.visits: int = 0
		pending: list[tuple[NormNode, int]] = [(node, -1) for node in reversed(roots)]
		while pending:
			node, begin = pending.pop()
			if -1 < begin:
				self.spans[id(node)] = begin, self.visits
				continue
			begin: int = self.visits
			self.visits += 1
			kind, text, children = node
			if kind in ("identifier", "simple_identifier"):
				self.mentions.setdefault(text or "", []).append(begin)
				if text == "eval" or "\\" in (text or ""):
					self.dynamic.append(begin)
			if "attribute" in kind:
				self.attributes.append(begin)
			if "macro" in kind:
				self.macros.append(begin)
			pending.append((node, begin))
			pending.extend((child, -1) for child in reversed(children))
		# 終了
		return

def transform_context(
	lang: str,
	index: TransformIndex,
	function: NormNode,
	shadowed_macros: frozenset[str]
) -> tuple[str, frozenset[str]]:
	"""入力の通常関数の戻値区分と，不変指定を許す単純な束縛を調べる
	計算量：直属のノード数 N・名前数 K・全文のノード数 T・テキスト長 B・初期化子の確認量 I に対して O(N+B+I+K log(T+1))
	引数：
		lang: 言語キー
		index: 全関数で共有する出現位置
		function: 入力側の名前付関数
		shadowed_macros: 定義又は取込に依り標準の導入と確定出来ない名前
	戻値：返戻補完の区分と，遮蔽・更新・捕捉の無い確認済の束縛
	"""
	head: list[NormNode] = [child for child in function[2] if child[0] not in TRANSFORM_BODIES]
	tokens: list[str] = [node[1] or "" for node in transform_nodes(tuple(head)) if not node[2]]
	mode: str = "typed"
	if lang == "ruby":
		mode = "value"
	elif lang in ("javascript", "typescript", "tsx", "python", "php"):
		annotations: list[NormNode] = [node for node in head if node[0] in ("return_type", "type_annotation", "type")]
		annotation_words: list[str] = [node[1] or "" for node in transform_nodes(tuple(annotations)) if not node[2] and node[0] != ":"]
		mode = "void" if not annotations or annotation_words == ["None" if lang == "python" else "void"] else "typed"
	elif lang in ("c", "cpp", "csharp", "java"):
		if (
			any(child[1] == "void" and child[0] in ("predefined_type", "primitive_type", "void_type") for child in head) or
			"constructor" in function[0] or "destructor" in function[0] or lang == "csharp" and function[0] == "accessor_declaration" and
			any(token in ("add", "init", "remove", "set") for token in tokens) or lang == "cpp" and len(head) == 1
		):
			mode = "void"
		if lang == "cpp":
			for part in transform_nodes(tuple(head)):
				if (
					part[0] == "trailing_return_type" and
					part[2] == (("->", "->", ()), ("type_descriptor", None, (("primitive_type", "void", ()),)))
				):
					mode = "void"
	elif lang == "go":
		mode = "typed" if any(node[0] in ("type_identifier", "pointer_type", "slice_type", "result") for node in head) else "void"
	elif lang == "rust":
		mode = "typed" if "->" in tokens and "()" not in tokens else "void"
	elif lang in ("kotlin", "swift"):
		mode = (
			"value" if
			lang == "swift" and "->" in tokens and "Void" not in tokens else
			"typed" if lang == "kotlin" and any(child[0] == "user_type" for child in head) and "Unit" not in tokens else "void"
		)
	if "async" in tokens:
		if lang in ("typescript", "tsx") and annotation_words == ["Promise", "<", "void", ">"] or lang == "csharp" and any(
			child[0] == "identifier" and child[1] in ("Task", "ValueTask") or
			child[0] == "qualified_name" and [part[1] for part in transform_nodes((child,)) if not part[2]] in (
				["System", ".", "Threading", ".", "Tasks", ".", "Task"],
				["System", ".", "Threading", ".", "Tasks", ".", "ValueTask"]
			) for child in head
		):
			mode = "void"
	counts: Counter[str] = Counter()
	unsafe: set[str] = set()
	mentions: Counter[str] = Counter()
	parameters: set[str] = set()
	initializers: dict[str, NormNode] = {}
	blocked: bool = bool(index.count(function, index.dynamic))
	ordered: list[NormNode] = []
	# 書込・借用・捕捉の文脈を子へ渡し，同じメンバの子孫を繰り返し探索しない
	pending: list[tuple[NormNode, bool, bool]] = [(function, False, False)]
	while pending:
		node, is_unsafe, is_nested = pending.pop()
		index.visits += 1
		kind: str = node[0]
		children: tuple[NormNode, ...] = node[2]
		if node is not function and kind in TRANSFORM_FUNCTIONS.get(lang, set()):
			# 独立して文脈を調べる関数の捕捉は，共有する名前の位置表で照会する
			continue
		is_nested |= node is not function and (
			"lambda" in kind or "closure" in kind or
			kind in ("anonymous_function", "arrow_function", "function_expression", "generator_function", "generator_function_declaration")
		)
		ordered.append(node)
		if not is_nested:
			if (
				kind in ("co_await_expression", "co_return_statement", "co_yield_statement", "yield", "yield_expression", "yield_statement") or
				"generator" in kind
			):
				blocked = True
			if "macro" in kind or lang in ("rust", "swift") and "attribute" in kind:
				# 標準の出力マクロと診断属性だけを，独立した固定の契約で認める
				name: str = next((part[1] or "" for part in transform_nodes(children) if part[0] == "identifier"), "")
				if lang != "rust" or "macro" in kind and (kind != "macro_invocation" or name in shadowed_macros) or name not in (
					("eprint", "eprintln", "print", "println") if
					"macro" in kind else
					("allow", "cold", "deny", "deprecated", "doc", "forbid", "inline", "must_use", "warn")
				):
					blocked = True
		if kind in TRANSFORM_DECLARATORS or kind in ("property_declaration", "var_declaration"):
			for part in declaration_parts(node):
				name = transform_binding(part)
				if name:
					counts[name] += 1
					if lang == "java":
						declarator: NormNode = next((child for child in part[2] if child[0] == "variable_declarator"), part)
						if any(child[0] == "=" for child in declarator[2]):
							initializers[name] = declarator[2][-1]
		if kind in ("identifier", "simple_identifier"):
			mentions[node[1] or ""] += 1
			if is_unsafe or is_nested:
				unsafe.add(node[1] or "")
			if node[1] == "eval" or "\\" in (node[1] or ""):
				blocked = True
		is_update: bool = (
			"update" in kind or kind in ("postfix_expression", "prefix_expression") and any(child[0] in ("++", "--") for child in children)
		)
		# マクロの実引数は，展開後の可変借用を否定出来ない
		is_borrow: bool = kind in ("borrow_expression", "reference_expression", "macro_invocation")
		is_receiver: bool = lang == "rust" and kind == "call_expression" and bool(children) and children[0][0] == "field_expression"
		is_parameter: bool = kind in ("formal_parameter", "parameter", "parameter_declaration", "simple_parameter", "typed_parameter")
		if lang == "java" and is_parameter:
			parameters.update(child[1] or "" for child in children if child[0] == "identifier")
		iteration_target: int = (
			next((idx - 1 for idx, child in enumerate(children) if child[0] in ("in", "of")), -1) if kind == "for_in_statement" else -1
		)
		for idx, child in enumerate(children):
			is_target: bool = "assignment" in kind and not idx or idx == iteration_target
			pending.append((child, is_unsafe or is_target or is_update or is_borrow or is_parameter or is_receiver and not idx, is_nested))
	# 直属で数えた名前との差は，入子の本体に在る同名の参照又は束縛である
	for name in counts:
		if index.count(function, index.mentions.get(name, [])) > mentions[name]:
			unsafe.add(name)
	if blocked or any(token in ("Never", "_Noreturn", "noreturn") for token in tokens):
		if lang == "rust" and not index.count(function, index.attributes):
			begin, end = index.spans[id(function)]
			macro_idx: int = bisect_left(index.macros, end) - 1
			last_macro: int = index.macros[macro_idx] if -1 < macro_idx and begin <= index.macros[macro_idx] else end
			late: set[str] = {
				transform_binding(part) for part in ordered if part[0] == "let_declaration" and index.spans[id(part)][1] - 1 > last_macro
			}
			# マクロより後に導入した束縛だけを確定した結果の返戻
			return "blocked", frozenset(name for name in late if counts[name] == 1 and name not in unsafe)
		# 未確認の制御・捕捉・外部効果へ補完を広げない区分の返戻
		return "blocked", frozenset()
	# 同じ綴りの別束縛が無く，更新先でもない対象の返戻
	safe: set[str] = { name for name, count in counts.items() if count == 1 and name not in unsafe }
	if lang == "java":
		# 仮引数・配列要素は定数式でなく，参照の無いローカル名は定数化の観測先を持たない
		safe.update(
			"final:" + name for name, value in initializers.items() if index.count(function, index.mentions.get(name, [])) == 1 or
			any(part[0] == "array_access" or part[0] == "identifier" and part[1] in parameters for part in transform_nodes((value,)))
		)
	# 戻値の区分と不変指定の保存条件の返戻
	return mode, frozenset(safe)

def transform_return(node: NormNode) -> tuple[NormNode, ...]|None:
	"""返戻文の値を，通常の式及びラベル付の脱出から区別する
	計算量：直接の子数 N に対して O(N)
	引数：node: 返戻候補の構文
	戻値：返戻値の列．返戻以外は None
	"""
	if node[0] in TRANSFORM_RETURNS and node[2] and node[2][0][0] == "return":
		# 脱出先を修飾しない返戻の値の返戻
		return node[2][1:]
	# 返戻以外の返戻
	return None

def transform_terminal(node: NormNode) -> bool:
	"""入力に明示した無条件の脱出又は全枝の脱出を判定する
	計算量：部分木のノード数 N に対して O(N)
	引数：node: 入力側の末尾文
	戻値：入力側で後続へ達しない事が構文から確定する場合は真
	"""
	facts: dict[int, bool] = {}
	for part in reversed(list(transform_nodes((node,)))):
		children: list[NormNode] = [child for child in part[2] if child[0] not in ("{", "}", "else", ":")]
		terminal: bool = (
			transform_return(part) is not None or part[0] in ("raise_statement", "throw", "throw_expression", "throw_statement")
		)
		if part[0] in TRANSFORM_SEQUENCES | { "else_clause" } and children:
			terminal = facts[id(children[-1])]
		elif part[0] == "if_statement" and len(children) > 1 and children[-1][0] == "else_clause":
			terminal = facts[id(children[-2])] and facts[id(children[-1])]
		facts[id(part)] = terminal
	# 入力の脱出事実の返戻
	return facts[id(node)]

def binding_names(node: NormNode) -> tuple[NormNode, ...]:
	"""束縛パターンの導入名だけを，キー・型・既定値から区別して列挙する
	計算量：パターンのノード数 N に対して O(N)
	引数：node: 宣言子又は束縛パターン
	戻値：原文順の導入名
	"""
	found: list[NormNode] = []
	pending: list[NormNode] = [node]
	while pending:
		part: NormNode = pending.pop()
		kind, _, children = part
		if kind in ("identifier", "shorthand_property_identifier_pattern", "simple_identifier"):
			found.append(part)
		elif kind in ("assignment_pattern", "variable_declarator") and children:
			pending.append(children[0])
		elif kind == "pair_pattern" and children:
			pending.append(children[-1])
		elif kind in (
			"array_pattern",
			"captured_pattern",
			"multi_variable_declaration",
			"mut_pattern",
			"object_pattern",
			"pattern",
			"ref_pattern",
			"rest_pattern",
			"tuple_pattern",
			"variable_declaration"
		):
			pending.extend(reversed(children))
		elif kind in ("let_declaration", "lexical_declaration", "property_declaration"):
			end: int = next((idx for idx, child in enumerate(children) if child[0] in ("=", "by")), len(children))
			pending.extend(reversed(children[:end]))
	# キーと初期化の参照を含めない名前の返戻
	return tuple(found)

def immutable_bindings(lang: str, roots: tuple[NormNode, ...]) -> set[int]:
	"""宣言と更新先をスコープで対応付け，不変指定を許す導入名を選ぶ
	計算量：ノード数 N・最大スコープ深さ D に対して O(ND)
	引数：lang: 言語キー，roots: 入力の正規形
	戻値：書込・未知の捕捉を持たない導入名の識別番号
	"""
	scopes: list[dict[str, int]] = [{}]
	parents: list[int] = [-1]
	owners: dict[int, int] = {}
	bindings: set[int] = set()
	names: set[int] = set()
	pending: list[tuple[NormNode, int, bool]] = [(node, 0, False) for node in reversed(roots)]
	while pending:
		node, scope, is_local = pending.pop()
		kind, _, children = node
		if kind in TRANSFORM_FUNCTIONS.get(lang, set()) or kind in ("function_body", "lambda_literal"):
			is_local: bool = True
		elif kind in TRANSFORM_TYPE_BODIES:
			is_local = False
		if lang == "kotlin" and kind in TRANSFORM_TYPE_BODIES or kind in (
			"arrow_function",
			"block",
			"closure_expression",
			"for_in_statement",
			"for_statement",
			"function_body",
			"function_declaration",
			"function_expression",
			"function_item",
			"lambda_expression",
			"lambda_literal",
			"method_definition",
			"statement_block"
		):
			parents.append(scope)
			scope: int = len(scopes)
			scopes.append({})
		owners[id(node)] = scope
		introduced: tuple[NormNode, ...] = ()
		if kind in ("let_declaration", "lexical_declaration", "property_declaration"):
			introduced = binding_names(node)
		elif kind == "for_in_statement":
			position: int = next((idx for idx, child in enumerate(children) if child[0] in ("const", "let")), -1)
			if position > -1 and position + 1 < len(children):
				introduced = binding_names(children[position + 1])
		elif kind in ("formal_parameters", "function_value_parameters", "lambda_parameters", "parameter", "parameters"):
			introduced = tuple(part for part in transform_nodes(children) if part[0] in ("identifier", "simple_identifier"))
		for name in introduced:
			key: str = name[1] or ""
			scopes[scope][key] = id(name) if key not in scopes[scope] else -1
			names.add(id(name))
			if kind in ("for_in_statement", "let_declaration", "lexical_declaration") or kind == "property_declaration" and is_local:
				bindings.add(id(name))
		pending.extend((child, scope, is_local) for child in reversed(children))
	unsafe: set[int] = set()
	pending_refs: list[tuple[NormNode, bool]] = [(node, False) for node in reversed(roots)]
	while pending_refs:
		node, is_write = pending_refs.pop()
		kind, _, children = node
		if kind in ("identifier", "shorthand_property_identifier_pattern", "simple_identifier") and id(node) not in names:
			scope = owners[id(node)]
			while scope > -1 and node[1] not in scopes[scope]:
				scope = parents[scope]
			if scope > -1 and is_write:
				unsafe.add(scopes[scope][node[1]])
		if kind in ("identifier", "simple_identifier") and (node[1] == "eval" or "\\" in (node[1] or "")):
			unsafe.update(bindings)
		is_update: bool = (
			"update" in kind or kind in ("postfix_expression", "prefix_expression") and any(child[0] in ("++", "--") for child in children)
		)
		iteration: int = (
			next((idx - 1 for idx, child in enumerate(children) if child[0] in ("in", "of")), -1) if kind == "for_in_statement" else -1
		)
		for idx, child in enumerate(children):
			is_target: bool = "assignment" in kind and not idx or idx == iteration
			if lang == "rust":
				is_target |= kind == "reference_expression" and any(part[0] == "mutable_specifier" for part in children)
				is_target |= kind == "call_expression" and not idx
				is_target |= kind == "let_declaration" and any(
					part[0] == "ref_pattern" and any(inner[0] == "mutable_specifier" for inner in transform_nodes((part,))) for part in
					transform_nodes(children)
				)
				is_target |= kind == "macro_invocation"
			if (
				lang in ("javascript", "kotlin") and
				child[0] in ("indexing_expression", "member_expression", "navigation_expression", "subscript_expression")
			):
				is_target = False
			pending_refs.append(
				(
					child,
					(is_write or is_update or is_target) and not (lang == "rust" and kind == "index_expression" and idx) and not (
						lang in ("javascript", "kotlin") and
						child[0] in ("indexing_expression", "member_expression", "navigation_expression", "subscript_expression")
					)
				)
			)
	# 曖昧な重複導入も除いた，確認済の名前の返戻
	return bindings.intersection(name for scope in scopes for name in scope.values()) - unsafe

def type_script_constants(roots: tuple[NormNode, ...], reasons: dict[int, str]|None = None) -> set[int]:
	"""スコープで参照を解決し，型の観測を変えない宣言子だけを選ぶ
	計算量：ノード数 N・最大スコープ深さ D に対して O(ND)
	引数：roots: 入力側の正規形の木，reasons: 宣言子毎の非変換の理由の格納先
	戻値：型を保持し，書込の無い宣言子の識別番号
	"""
	scopes: list[dict[str, int]] = [{}]
	parents: list[int] = [-1]
	capture_scopes: set[int] = set()
	owners: dict[int, int] = {}
	declarations: dict[int, NormNode] = {}
	names: set[int] = set()
	unconfirmed: set[int] = set()
	pending: list[tuple[NormNode, int]] = [(node, 0) for node in reversed(roots)]
	while pending:
		node, scope = pending.pop()
		kind, _, children = node
		is_capture_scope: bool = kind in (
			"arrow_function",
			"class",
			"class_declaration",
			"function_declaration",
			"function_expression",
			"generator_function",
			"generator_function_declaration",
			"method_definition"
		)
		if is_capture_scope or kind in ("constructor_type", "function_type", "statement_block"):
			parents.append(scope)
			scope: int = len(scopes)
			scopes.append({})
			if is_capture_scope:
				capture_scopes.add(scope)
		owners[id(node)] = scope
		if kind == "variable_declarator" and children:
			bindings: tuple[NormNode, ...] = binding_names(node)
			for binding in bindings:
				name: str = binding[1] or ""
				scopes[scope][name] = id(node) if name not in scopes[scope] else -1
				names.add(id(binding))
			declarations[id(node)] = node
			annotation: NormNode|None = next((child for child in children if child[0] == "type_annotation"), None)
			is_stable: bool = False
			if children[0][0] == "identifier":
				is_stable = annotation is not None or children[-1][0] in ("array", "arrow_function", "function_expression", "object", "regex")
			elif annotation and annotation[2]:
				shape: tuple[str, str, str] = children[0][0], annotation[2][-1][0], children[-1][0]
				is_stable = shape in (("array_pattern", "array_type", "array"), ("object_pattern", "object_type", "object")) and not any(
					part[0] in
					("array_pattern", "assignment_pattern", "computed_property_name", "object_assignment_pattern", "object_pattern") for part in
					transform_nodes(children[0][2])
				)
			if not bindings or not is_stable or not any(child[0] == "=" for child in children):
				unconfirmed.add(id(node))
				if reasons is not None:
					reasons[id(node)] = "初期化・宣言型又は分割した型の相関を確認出来ない"
		elif kind in ("optional_parameter", "required_parameter"):
			for child in children:
				if child[0] == "=":
					break
				if child[0] == "identifier":
					scopes[scope][child[1] or ""] = -1
					names.add(id(child))
		pending.extend((child, scope) for child in reversed(children))
	unsafe: set[int] = set()
	pending_refs: list[tuple[NormNode, bool]] = [(node, False) for node in reversed(roots)]
	while pending_refs:
		node, is_write = pending_refs.pop()
		kind, _, children = node
		if kind in ("identifier", "shorthand_property_identifier", "shorthand_property_identifier_pattern") and id(node) not in names:
			scope = owners[id(node)]
			is_captured: bool = False
			while scope > -1 and node[1] not in scopes[scope]:
				# 関数・クラスの境界を越える絞込は処理系のバージョンに依る為，許可しない
				is_captured |= scope in capture_scopes
				scope = parents[scope]
			if scope > -1:
				binding: int = scopes[scope][node[1]]
				if is_write or is_captured:
					unsafe.add(binding)
					if reasons is not None:
						reasons[binding] = "捕捉先での型の絞込が未確認" if is_captured else "再代入又は増減が在る"
		if kind == "identifier" and (node[1] == "eval" or "\\" in (node[1] or "")):
			unsafe.update(declarations)
			if reasons is not None:
				reasons.update((key, "eval 又は識別子の解釈が未確認") for key in declarations)
		is_update: bool = kind in ("discarded_update", "update_expression")
		iteration: int = (
			next((idx - 1 for idx, child in enumerate(children) if child[0] in ("in", "of")), -1) if kind == "for_in_statement" else -1
		)
		for idx, child in enumerate(children):
			pending_refs.append((child, is_write or is_update or "assignment" in kind and not idx or idx == iteration))
	# 導入名と参照を同じ有効範囲で対応付けた結果の返戻
	confirmed: set[int] = {
		key for key, declaration in declarations.items() if all(
			scopes[owners[key]].get(binding[1] or "") == key for binding in binding_names(declaration)
		)
	}
	if reasons is not None:
		reasons.update((key, "導入名の解決が未確認") for key in declarations.keys() - confirmed)
	# 全ての導入名の解決と保存条件を満たす宣言子の返戻
	return confirmed - unsafe - unconfirmed

def rust_tail_value(node: NormNode) -> bool:
	"""一時値の借用・射影を含まない末尾値の形を確認する
	計算量：式のノード数 N に対して O(N)
	引数：node: 返戻へ移す末尾式
	戻値：値の直接移動又は既存の場所からの読取を確認出来れば真
	"""
	pending: list[tuple[NormNode, bool]] = [(node, True)]
	while pending:
		part, allows_call = pending.pop()
		kind, _, children = part
		if kind == "call_expression":
			if not children:
				# 未確認の保存条件の拒否
				return False
			callee: NormNode = children[0]
			if (
				callee[0] not in ("field_expression", "identifier", "scoped_identifier") or not allows_call and callee[0] != "field_expression"
			):
				# 未確認の保存条件の拒否
				return False
			pending.extend((child, False) for child in children)
		elif kind == "closure_expression":
			if not allows_call:
				# 未確認の保存条件の拒否
				return False
		elif kind in ("field_expression", "index_expression"):
			base: NormNode = children[0]
			if base[0] not in ("field_expression", "identifier", "index_expression", "self"):
				# 未確認の保存条件の拒否
				return False
			pending.extend((child, False) for child in children)
		elif kind in ("binary_expression", "unary_expression"):
			pending.extend((child, allows_call and any(part[1] in ("&&", "||") for part in children)) for child in children)
		elif kind in (
			"arguments",
			"block",
			"break_expression",
			"else_clause",
			"if_expression",
			"infinite_loop",
			"loop_expression",
			"parenthesized_expression",
			"scoped_identifier",
			"type_cast_expression"
		):
			pending.extend((child, allows_call or kind == "if_expression") for child in children)
		elif children or kind not in {
			"!",
			"!=",
			"%",
			"&",
			"&&",
			"(",
			")",
			"*",
			"+",
			",",
			"-",
			".",
			"/",
			"::",
			"<",
			"<<",
			"<=",
			"==",
			">",
			">=",
			">>",
			"[",
			"]",
			"^",
			"as",
			"boolean_literal",
			"break",
			"char_literal",
			"else",
			"field_identifier",
			"float_literal",
			"identifier",
			"if",
			"integer_literal",
			"loop",
			"primitive_type",
			"self",
			"{",
			"|",
			"||",
			"}"
		}:
			# 未確認の保存条件の拒否
			return False
	# 借用又は破棄順の未確認の変化が無い値の返戻
	return True

def rust_known_effects(node: NormNode, shadowed: frozenset[str]) -> bool:
	"""クロージャを含む関数の未展開効果を，独立した標準登録で確認する
	計算量：対象のノード数 N に対して O(N)
	引数：node: 関数又は属性，shadowed: 標準の導入を確認出来ない名前
	戻値：返戻補完を妨げる未確認の効果が無ければ真
	"""
	pending: list[NormNode] = [node]
	while pending:
		part: NormNode = pending.pop()
		kind, _, children = part
		if part is not node and kind in ("function_item", "macro_definition"):
			continue
		if kind == "attribute":
			if not children or children[0][1] not in ("allow", "cold", "deny", "deprecated", "doc", "forbid", "inline", "must_use", "warn"):
				# 未確認の保存条件の拒否
				return False
			continue
		if kind == "macro_invocation":
			name: str = children[0][1] or "" if children else ""
			if name not in ("eprint", "eprintln", "print", "println") or name in shadowed:
				# 未確認の保存条件の拒否
				return False
			for argument in transform_nodes(children[2:]):
				if argument[0] in ("#", "!", "return") or argument[1] == "return":
					# 未確認の保存条件の拒否
					return False
			continue
		pending.extend(children)
	# 確認済の効果の返戻
	return True

def rust_return_functions(roots: tuple[NormNode, ...], shadowed: frozenset[str]) -> set[int]:
	"""包含項目と直前の属性を保持して返戻補完可能な関数を集める
	計算量：ノード数 N・入子の関数の深さ D に対して O(ND)
	引数：roots: 入力の正規形，shadowed: 標準マクロを遮蔽する名前
	戻値：未確認の属性・マクロが無い関数の識別子
	"""
	confirmed: set[int] = set()
	pending: list[tuple[tuple[NormNode, ...], bool]] = [(roots, True)]
	while pending:
		sequence, inherited = pending.pop()
		attributes: bool = inherited
		for part in sequence:
			if part[0] == "attribute_item":
				attributes &= rust_known_effects(part, shadowed)
				continue
			if part[0] == "inner_attribute_item":
				inherited &= rust_known_effects(part, shadowed)
				attributes &= inherited
				continue
			if (
				part[0] == "function_item" and attributes and not any(child[0] == "never_type" for child in part[2]) and
				rust_known_effects(part, shadowed)
			):
				confirmed.add(id(part))
			pending.append((part[2], attributes))
			attributes = inherited
	# 属性の適用範囲を保持した関数の返戻
	return confirmed

def allowed_structure(lang: str, before: list[Node], after: list[Node]) -> bool:
	"""候補の構文差を保持し，独立した入力の事実で許可変換の保存条件を照合する
	計算量：正規形のノード数 N・テキスト長 B に対して最悪 O(N²+NB+P)（P はマクロ等の保護する観測の照合費用）
	引数：
		lang: 言語キー
		before: 入力の木
		after: 候補の木
	戻値：許可変換だけで対応する場合は真．未確認の変換又は保護違反は偽
	"""
	if (
		source_observation_signature(before, lang) != source_observation_signature(after, lang) or
		protected_observation_signature(before, lang) != protected_observation_signature(after, lang)
	):
		# 許可変換で，文字列化・保持本文等のソース観測を消さない
		return False
	if lang == "swift":
		pending_nodes: list[Node] = [*before, *after]
		while pending_nodes:
			node: Node = pending_nodes.pop()
			if (
				node.type == "operator_declaration" and any(child.type == "infix" for child in node.children) or
				node.type == "prefix_expression" and any(child.type in SWIFT_BINARY for child in node.children)
			):
				# 独自の演算子，又は前置演算が中置式へ伸びた木だけでは結合を確定出来ない
				return False
			pending_nodes.extend(node.children)
	left: tuple[NormNode, ...] = tuple(part for root in before for part in normalize(root, lang))
	right: tuple[NormNode, ...] = tuple(part for root in after for part in normalize(root, lang))
	if " ".join(sexpr(part) for part in left) == " ".join(sexpr(part) for part in right):
		# 既に正規化した構造の完全一致を，許可変換の再照合より先に確定する
		return True
	confirmed_embedded: set[tuple[NormNode, NormNode]] = set()
	if lang == "html":
		original_children: list[Node] = [node for node, _, _ in source_leaves(before) if node.type.startswith("embedded:")]
		candidate_children: list[Node] = [node for node, _, _ in source_leaves(after) if node.type.startswith("embedded:")]
		if len(original_children) == len(candidate_children):
			for original, candidate in zip(original_children, candidate_children):
				if (
					original.type != candidate.type or
					original.text.lstrip(" \t\r\n\f").startswith("\ufeff") != candidate.text.lstrip(" \t\r\n\f").startswith("\ufeff")
				):
					continue
				child_lang: str = original.type.split(":", 1)[1]
				if not classify(child_lang, original.children, candidate.children):
					confirmed_embedded.add((normalize(original, lang)[0], normalize(candidate, lang)[0]))
	type_constants: set[int] = type_script_constants(left) if lang in ("typescript", "tsx") else set()
	immutable: set[int] = immutable_bindings(lang, left) if lang in ("javascript", "kotlin", "rust") else set()
	is_module: bool = lang in ("javascript", "typescript", "tsx") and (
		any(root[0] == "module" for root in left) or
		any(child[0] in ("export_statement", "import_statement") for root in left for child in root[2])
	)
	# 標準マクロの暗黙の導入を，利用者の定義・取込で上書した名前を保護する
	shadowed_macros: frozenset[str] = frozenset(
		part[1] or "" for node in transform_nodes(left) if node[0] in ("macro_definition", "use_declaration") for part in
		transform_nodes(node[2]) if part[0] == "identifier"
	) if lang == "rust" else frozenset()
	rust_functions: set[int] = rust_return_functions(left, shadowed_macros) if lang == "rust" else set()
	context_index: TransformIndex = TransformIndex(left)
	pending: list[tuple[tuple[NormNode, ...], tuple[NormNode, ...], str, frozenset[str], bool]] = [
		(left, right, "", frozenset(), False)
	]
	mode: str
	safe: frozenset[str]
	tail: bool
	while pending:
		left, right, mode, safe, tail = pending.pop()
		# 隣接した宣言子の評価順は，統合・分離の何方でも同じ並びとして照合する
		if (
			mode in ("", "scope") and
			sum(node[0] in TRANSFORM_DECLARATORS or node[0] in ("property_declaration", "var_declaration") for node in left) >
			sum(node[0] in TRANSFORM_DECLARATORS or node[0] in ("property_declaration", "var_declaration") for node in right)
		):
			# 保存条件を満たさない候補の拒否
			return False
		left = tuple(part for node in left for part in declaration_parts(node))
		right = tuple(part for node in right for part in declaration_parts(node))
		end: int = len(left) - int(bool(left) and left[-1][0] in ("}", "end"))
		other_end: int = len(right) - int(bool(right) and right[-1][0] in ("}", "end"))
		if tail and lang == "swift" and mode == "value" and len([node for node in left if node[0] not in ("{", "}", "statements")]) > 1:
			tail = False
		if tail and end and other_end:
			if (
				mode == "void" and transform_return(right[other_end - 1]) == () or
				mode == "entry" and transform_return(right[other_end - 1]) == (("number_literal", "0", ()),)
			) and transform_return(left[end - 1]) is None:
				# 入力の関数末尾へ到達する，値を変えない返戻の補完だけを照合する
				if mode != "entry" and all(
					part[0] in ("{", "pass_statement", "ellipsis") or
					not position and part[0] == "string_value" and not (part[1] or "").startswith("b") for position, part in enumerate(left[:end])
				) or transform_terminal(left[end - 1]):
					# 保存条件を満たさない候補の拒否
					return False
				right = right[:other_end - 1] + right[other_end:]
				# Rust の捨てた末尾式の値は，補った空の返戻でも捨てる
				if lang == "rust" and end > 1 and left[end - 2][0] == "discarded_tail":
					left = left[:end - 2] + left[end - 1:]
			elif transform_return(left[end - 1]) == () and end > 1 and transform_terminal(left[end - 2]):
				# 入力側で先行する無条件の脱出に達した後の末尾返戻だけを除く
				left = left[:end - 1] + left[end:]
		if len(left) != len(right):
			# 保存条件を満たさない候補の拒否
			return False
		end = len(left) - int(bool(left) and left[-1][0] in ("}", "end"))
		for idx, (first, second) in enumerate(zip(left, right)):
			is_tail: bool = tail and idx == end - 1
			if is_tail and mode == "value" and lang in ("rust", "ruby", "swift") and transform_return(first) is None:
				values: tuple[NormNode, ...]|None = transform_return(second)
				if values is not None and len(values) == 1 and (lang != "rust" or rust_tail_value(first)):
					pending.append(((first,), values, "", safe, False))
					continue
			if lang == "ruby" and first[0] in ("if", "unless", "while", "until") and second[0] == first[0] + "_modifier":
				parts: tuple[NormNode, ...] = first[2]
				values = second[2]
				if len(values) != 3:
					# 保存条件を満たさない候補の拒否
					return False
				if len(parts) == 4 and parts[2][0] == "then" and len(parts[2][2]) == 1:
					body: NormNode = parts[2][2][0]
				elif len(parts) == 3 and first[0] in ("while", "until"):
					body = parts[2]
				else:
					# 保存条件を満たさない候補の拒否
					return False
				if any("assignment" in item[0] or item[0] in ("begin", "regex") for item in transform_nodes((parts[1], body))):
					# 保存条件を満たさない候補の拒否
					return False
				pending.append(((parts[1], body), (values[2], values[0]), mode, safe, False))
				continue
			if first[0].startswith("embedded:") and (first, second) in confirmed_embedded:
				continue
			if lang == "rust" and mode not in ("", "blocked", "scope") and first[0] == "mut_pattern" and first[2] and (
				id(first[2][-1]) in immutable or
				first[2][-1][0] == "captured_pattern" and first[2][-1][2] and id(first[2][-1][2][0]) in immutable
			):
				parts: tuple[NormNode, ...] = tuple(child for child in first[2] if child[0] != "mutable_specifier")
				if len(parts) == 1:
					pending.append((parts, (second,), mode, safe, tail))
					continue
			if first[:2] != second[:2]:
				# 保存条件を満たさない候補の拒否
				return False
			children: tuple[NormNode, ...] = first[2]
			other: tuple[NormNode, ...] = second[2]
			if lang == "javascript" and not mode and (
				first[0] == "statement_block" or
				first[0] == "program" and any(child[0] in ("export_statement", "import_statement") for child in children)
			):
				_, safe = transform_context(lang, context_index, first, shadowed_macros)
			if first[0] in TRANSFORM_FUNCTIONS.get(lang, set()):
				function_mode: str
				function_safe: frozenset[str]
				function_mode, function_safe = transform_context(lang, context_index, first, shadowed_macros)
				if lang == "rust":
					function_mode = "blocked"
					if id(first) in rust_functions:
						function_mode = (
							"value" if any(child[0] == "->" for child in children) and not any(child[0] == "unit_type" for child in children) else "void"
						)
				if (
					mode == "" and function_mode != "blocked" and lang in ("c", "cpp") and first[0] == "function_definition" and
					any(child[0] == "function_declarator" and child[2] and child[2][0] == ("identifier", "main", ()) for child in children)
				):
					function_mode = "entry"
				if len(children) != len(other):
					# 保存条件を満たさない候補の拒否
					return False
				for child, candidate in zip(children, other):
					pending.append(
						(
							(child,),
							(candidate,),
							function_mode,
							function_safe,
							child[0] in TRANSFORM_BODIES or first[0] in ("computed_setter", "secondary_constructor") and child[0] == "statements"
						)
					)
				continue
			if first[0] == "named_imports" and lang in ("javascript", "typescript", "tsx"):
				# 文全体を動かさず，指定子の別名と型指定を含む独立した順序で照合する
				children = tuple(sorted(children, key=lambda child: (sexpr(child).lower(), sexpr(child))))
				other = tuple(sorted(other, key=lambda child: (sexpr(child).lower(), sexpr(child))))
			if (
				lang not in ("kotlin", "typescript", "tsx") and transform_binding(first) in safe or
				lang == "java" and "final:" + transform_binding(first) in safe or
				(lang == "kotlin" or mode not in ("", "scope", "blocked") or is_module and mode == "") and binding_names(first) and
				all(id(name) in immutable for name in binding_names(first)) or
				lang in ("typescript", "tsx") and (mode not in ("", "scope", "blocked") or is_module and not mode) and
				any(id(child) in type_constants for child in children)
			):
				if (
					lang == "java" and first[0] == "local_variable_declaration" and
					not any(part[0] == "final" for child in children if child[0] == "modifiers" for part in child[2]) and other and
					other[0][0] == "modifiers" and other[0][2] == (("final", "final", ()),)
				):
					# 呼出・生成の初期化は定数変数に為らず，型・到達可能性を変えない
					if "final:" + transform_binding(first) in safe or any(
						child[0] in (
							"array_creation_expression",
							"lambda_expression",
							"method_invocation",
							"object_creation_expression",
							"switch_expression"
						) for child in transform_nodes(children)
					) or any(child[0] == "array_type" or child[0] == "type_identifier" and child[1] not in ("String", "var") for child in children):
						other = other[1:]
				elif lang == "rust" and first[0] == "let_declaration" and not any(child[0] == "mutable_specifier" for child in other):
					children = tuple(child for child in children if child[0] != "mutable_specifier")
				elif (
					lang in ("kotlin", "javascript", "typescript", "tsx") and
					any(child[0] == "=" or any(part[0] == "=" for part in child[2]) for child in children)
				):
					if children and other and (
						lang == "kotlin" and children[0] == ("binding_pattern_kind", None, (("var", "var", ()),)) and
						other[0] == ("binding_pattern_kind", None, (("val", "val", ()),)) or
						lang != "kotlin" and children[0] == ("let", "let", ()) and other[0] == ("const", "const", ())
					):
						children = (other[0], *children[1:])
			if lang == "javascript" and first[0] == "for_in_statement":
				position: int = next((pos for pos, child in enumerate(children) if child[0] == "let"), -1)
				if position > -1 and len(other) == len(children) and other[position][0] == "const":
					names: tuple[NormNode, ...] = binding_names(children[position + 1])
					if names and all(id(name) in immutable for name in names):
						children = children[:position] + (other[position],) + children[position + 1:]
			child_tail: bool = tail and first[0] in TRANSFORM_SEQUENCES
			if lang == "ruby" and is_tail and first[0] == "begin":
				# ensure は値の説明対象でもメソッドからの返戻対象でもない
				if len(children) != len(other):
					# 保存条件を満たさない候補の拒否
					return False
				for child, candidate in zip(children, other):
					if child[0] in ("begin", "else", "end", "ensure", "rescue"):
						pending.append(((child,), (candidate,), mode, safe, child[0] in ("else", "rescue")))
				ordinary: tuple[NormNode, ...] = (
					tuple(child for child in children if child[0] not in ("begin", "else", "end", "ensure", "rescue"))
				)
				candidate_ordinary: tuple[NormNode, ...] = (
					tuple(child for child in other if child[0] not in ("begin", "else", "end", "ensure", "rescue"))
				)
				pending.append((ordinary, candidate_ordinary, mode, safe, True))
				continue
			pending.append(
				(
					children,
					other,
					"scope" if first[0] in ("class_declaration", "struct_specifier") else mode,
					safe,
					child_tail or lang == "ruby" and first[0] in ("else", "rescue") and tail
				)
			)
	# 全ての差が保存条件を満たした結果の返戻
	return True

def heredoc_signature(roots: list[Node]) -> tuple[str, ...]:
	"""遅延して読む Ruby のヒアドキュメントの本文と終端を開始順に保持する
	計算量：ノード数 N と本文長 B に対して O(N+B)
	引数：roots: 読違の修復後の独立した木
	戻値：本文と終端の逐語列
	"""
	found: list[str] = []
	pending: list[Node] = list(reversed(roots))
	while pending:
		node: Node = pending.pop()
		if node.type == "heredoc_body":
			found.append(
				json.dumps(
					[(child.type, forest_sexpr([child], "ruby") if child.type == "interpolation" else child.text) for child in node.children],
					ensure_ascii=False
				)
			)
		else:
			pending.extend(reversed(node.children))
	# 開始順と対応した本文の返戻
	return tuple(found)

def cpp_directive_observation(roots: list[Node]) -> tuple[tuple[tuple[str, str], ...], tuple[str, ...], tuple[str, ...]]:
	"""翻訳段階の指示の境界・本文と，指示内のコメントの説明を独立に観測する
	計算量：ソース長 N に対して O(N)
	引数：roots: 翻訳前の木
	戻値：空の指示以外のトークン列，全コメントの説明，指示内のコメントの説明
	"""
	result: list[tuple[str, str]] = []
	line: list[tuple[str, str]] = []
	is_directive: bool = False
	for item in cpp_token_signature(roots):
		if item[0] == "directive_start":
			is_directive = True
			line = []
		elif item[0] == "directive_end":
			if len(line) > 1:
				result.extend(line)
				result.append(item)
			is_directive = False
		elif is_directive:
			line.append(item)
	comments: list[str] = []
	directive_comments: list[str] = []
	is_directive = False
	is_line_start: bool = True
	for token in cpp_phase_tokens(cpp_source("".join(root.text for root in roots)).text):
		if token.kind == "comment":
			parts: tuple[str, ...] = doc_description(doc_body("cpp", token.text))
			comments.extend(parts)
			if is_directive:
				directive_comments.extend(parts)
		elif token.kind == "space":
			if "\n" in token.text or "\r" in token.text:
				is_directive = False
				is_line_start = True
		else:
			if is_line_start and cpp_token_role(token.text) == "#":
				is_directive = True
			is_line_start = False
	# 空の指示の配置差でも説明の脱落を許さない観測の返戻
	return tuple(result), tuple(comments), tuple(directive_comments)

def kotlin_statement_source(text: str) -> str:
	"""改行で終わる呼出・返戻と，閉波括弧の境界をパーサーへ明示する
	計算量：原文長 N に対して O(N)
	引数：text: 独立した入力又は候補
	戻値：文字列・コメントの本文を保持した解析用のソース
	"""
	pattern: (
		str
	) = r'''"""[\s\S]*?"""|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|`[^`]*`|//[^\r\n]*|/\*[\s\S]*?\*/|\s+|\w+|\?\.|::|\+\+|--|->|.'''
	tokens: list[re.Match[str]] = list(re.finditer(pattern, text))
	if any(part[0].startswith("/*") and "/*" in part[0][2:] or part[0].startswith("\"") and "${" in part[0] for part in tokens):
		# 未解釈の入子と補間を保つ原文の返戻
		return text
	code: list[re.Match[str]] = [part for part in tokens if not part[0].isspace() and not part[0].startswith(("//", "/*"))]
	edits: list[tuple[int, int, str]] = []
	depth: list[str] = []
	closes_control: bool = False
	for idx, token in enumerate(code):
		value: str = token[0]
		if value == "}":
			edits.append((token.start(), token.start(), "\n"))
		if idx:
			previous: re.Match[str] = code[idx - 1]
			gap: str = text[previous.end():token.start()]
			is_line: bool = "\n" in gap or "\r" in gap
			is_complete: bool = not closes_control and (previous[0] in (")", "]", "}", "++", "--", "return") or previous[0].isalnum())
			is_start: bool = value in ("(", "::", "val", "var")
			if is_line and is_complete and (not depth or depth[-1] == "{") and (is_start or previous[0] == "return"):
				edits.append((previous.end(), previous.end(), ";"))
				if value == "(" and idx + 2 < len(code) and code[idx + 1][0].isidentifier() and code[idx + 2][0] == ")":
					edits.append((token.start(), token.end(), ""))
					edits.append((code[idx + 2].start(), code[idx + 2].end(), ""))
		closes_control = False
		if value in ("(", "[", "{"):
			depth.append("control" if value == "(" and idx and code[idx - 1][0] in ("catch", "for", "if", "when", "while") else value)
		elif value in (")", "]", "}") and depth:
			closes_control = depth.pop() == "control"
	chunks: list[str] = []
	position: int = 0
	for start, end, addition in sorted(set(edits)):
		chunks.extend((text[position:start], addition))
		position = end
	chunks.append(text[position:])
	# トークンの順序と明示した文境界の返戻
	return "".join(chunks)

def php_statement_source(roots: list[Node], text: str) -> str:
	"""トークンの文法順から else の所属を求め，埋込の単一文を囲む
	計算量：トークン数 N・入子の深さ D に対して O(ND)
	引数：roots: 原文の木，text: 原文
	戻値：分岐と反復の所属を明示した同じ文列
	"""
	tokens: list[Node] = [node for node, _, _ in source_leaves(roots) if not is_comment(node.type)]
	pairs: dict[int, int] = {}
	stack: list[int] = []
	for idx, token in enumerate(tokens):
		if token.type in ("(", "[", "{"):
			stack.append(idx)
		elif token.type in (")", "]", "}") and stack:
			start: int = stack.pop()
			if (tokens[start].type, token.type) in (("(", ")"), ("[", "]"), ("{", "}")):
				pairs[start] = idx
	edits: set[tuple[int, int]] = set()
	def statement(position: int, depth: int) -> int:
		"""最も内側の未対応の if へ else を結び，文の直後を返す
		引数：position: 文の先頭のトークン位置，depth: 文の入子の深さ
		戻値：確認した文の直後の位置，欠落・対象外の記法・言語の境界又は１００段超の入子なら `-1`
		"""
		if position >= len(tokens) or depth > 100:
			# 範囲外又は深さの未確認の返戻
			return -1
		kind: str = tokens[position].type
		if kind == "{":
			# 波括弧の対応する直後の位置の返戻
			return pairs.get(position, -2) + 1
		if kind in ("if", "elseif", "while", "for", "foreach", "declare"):
			opening: int = position + 1
			if opening not in pairs or tokens[opening].type != "(":
				# 条件の対応を確定出来ない結果の返戻
				return -1
			body: int = pairs[opening] + 1
			if body >= len(tokens) or tokens[body].type == ":":
				# 対象外の選択記法又は欠落した本体の返戻
				return -1
			end: int = statement(body, depth + 1)
			if end < 0:
				# 未確認の本体の返戻
				return -1
			if tokens[body].type != "{":
				edits.add((tokens[body].start, tokens[end - 1].end))
			if kind in ("if", "elseif") and end < len(tokens):
				if tokens[end].type == "elseif":
					# 後続する条件節の末尾の返戻
					return statement(end, depth + 1)
				if tokens[end].type == "else":
					body = end + 1
					end = statement(body, depth + 1)
					if end != -1 and tokens[body].type != "{":
						edits.add((tokens[body].start, tokens[end - 1].end))
			# 本体と後続節の末尾の返戻
			return end
		if kind == "do":
			end = statement(position + 1, depth + 1)
			if end < 0 or end + 1 not in pairs or tokens[end].type != "while":
				# 後置条件の対応が無い結果の返戻
				return -1
			end = pairs[end + 1] + 1
			# 後置条件と終端を含む末尾の返戻
			return end + 1 if end < len(tokens) and tokens[end].type == ";" else -1
		while position < len(tokens):
			if tokens[position].type in ("php_end_tag", "text", "}", "ERROR"):
				# 言語の境界を越えない未確認の返戻
				return -1
			if tokens[position].type == ";":
				# 終端記号の直後の返戻
				return position + 1
			position = pairs.get(position, position) + 1
		# 終端を確定出来ない結果の返戻
		return -1
	for idx, token in enumerate(tokens):
		if token.type in ("if", "elseif", "while", "for", "foreach", "declare"):
			statement(idx, 0)
	chunks: list[str] = []
	position: int = 0
	openings: Counter[int] = Counter(start for start, _ in edits)
	closings: Counter[int] = Counter(end for _, end in edits)
	for start in sorted(openings.keys() | closings.keys()):
		chunks.extend((text[position:start], "}" * closings[start], "{" * openings[start]))
		position = start
	chunks.append(text[position:])
	# 文と脱出先の同じ選択表現の返戻
	return "".join(chunks)

def repair_comparison_source(lang: str, roots: list[Node]) -> list[Node]:
	"""区切に敏感な構文を，同じ入力の合法な選択表現で独立に読み直す
	計算量：ノード数 N・原文長 B と必要時の再解析費用 P に対して O(N+B+P)
	引数：
		lang: 対象言語
		roots: 原文を持つ木
	戻値：区切を明示して読んだ木．対象が無ければ元の木
	送出例外：RuntimeError: 独立した再解析が失敗した場合
	"""
	if lang not in ("csharp", "php", "javascript", "typescript", "tsx", "ruby", "kotlin"):
		# 選択表現の読替を持たない言語の返戻
		return roots
	text: str = "".join(root.text for root in roots)
	if lang == "kotlin":
		logical: str = kotlin_statement_source(text)
		# 文の区切を明示した解析木の返戻
		return parse_dump(dump_ast(logical, "kt"), logical) if logical != text else roots
	if lang == "csharp":
		# 文字列・文字・ブロックコメントの本文を保持し，コード上の行境界だけを適応する
		parts: list[str] = re.findall(
			r'@"(?:[^"]|"")*"|"(?:[^"\\]|\\.)*"|\'(?:[^\'\\]|\\.)*\'|//[^\r\n\u0085\u2028\u2029]*|/\*[\s\S]*?\*/|[\u0085\u2028\u2029]|[^"\'/\u0085\u2028\u2029]+|.',
			text
		)
		logical: str = "".join("\n" if part in ("\u0085", "\u2028", "\u2029") else part for part in parts)
		# パーサーの行境界と実際の字句解析の行境界を揃えた木の返戻
		return parse_dump(dump_ast(logical, "cs"), logical) if logical != text else roots
	if lang == "php":
		logical = php_statement_source(roots, text)
		if logical != text:
			roots = parse_dump(dump_ast(logical, "php"), logical)
			text = logical
	edits: list[tuple[int, int, str]] = []
	prefix_readonly: set[int] = set()
	if lang in ("typescript", "tsx"):
		# エラーの子にも残る型の範囲から，前置型の強い被演算子だけを囲む
		operands: dict[int, int] = {}
		pending_types: list[Node] = list(reversed(roots))
		while pending_types:
			part: Node = pending_types.pop()
			if part.type in ("array_type", "tuple_type"):
				operands[part.start] = max(part.end, operands.get(part.start, 0))
			pending_types.extend(reversed(part.children))
		leaves: list[Node] = [part for part, _, _ in source_leaves(roots) if not is_comment(part.type)]
		for keyword, readonly, operand in zip(leaves, leaves[1:], leaves[2:]):
			if keyword.type == "keyof" and readonly.type == "readonly" and operand.start in operands:
				prefix_readonly.add(readonly.start)
				edits.extend(((readonly.start, readonly.start, "("), (operands[operand.start], operands[operand.start], ")")))
	async_owners: dict[int, bool|None] = {}
	if lang in ("javascript", "typescript", "tsx"):
		frames: list[tuple[Node, bool|None]] = [(root, None) for root in reversed(roots)]
		while frames:
			part, is_async = frames.pop()
			if part.type in (
				"arrow_function",
				"function_declaration",
				"function_expression",
				"generator_function",
				"generator_function_declaration",
				"method_definition"
			):
				is_async: bool|None = any(child.type == "async" for child in part.children)
			async_owners[id(part)] = is_async
			if part.type == "await_expression" and is_async is False and len(part.children) > 1:
				keyword, value = part.children[0], part.children[-1]
				if re.search(r"[\r\n\u2028\u2029]", text[keyword.end:value.start]):
					# 通常関数の識別子 await は，次行の呼出を待機する演算子でない
					edits.append((keyword.end, keyword.end, ";"))
			frames.extend((child, is_async) for child in reversed(part.children))
	pending: list[Node] = list(reversed(roots))
	while pending:
		node: Node = pending.pop()
		if lang == "php" and node.type == "php_end_tag":
			prefix: str = text[:node.start].rstrip()
			if prefix and prefix[-1] not in ";{}:":
				# 閉タグが暗黙に終える文を，パーサーにも明示する
				edits.append((node.start, node.start, ";"))
		if lang in ("javascript", "typescript", "tsx"):
			if node.type in ("return_statement", "yield_expression"):
				parts: list[Node] = [child for child in node.children if not is_comment(child.type)]
				if len(parts) > 1 and parts[0].type in ("return", "yield"):
					keyword, value = parts[0], parts[1]
					if value.type != ";" and re.search(r"[\r\n\u2028\u2029]", text[keyword.end:value.start]):
						# 制限付の予約語の後の改行は，後続の式を同じ値へ取り込まない
						edits.append((keyword.end, keyword.end, ";"))
			if node.type == "method_definition":
				parts = [child for child in node.children if not is_comment(child.type)]
				for keyword, following in zip(parts, parts[1:]):
					if keyword.type == "async" and re.search(r"[\r\n\u2028\u2029]", text[keyword.end:following.start]):
						# async メソッドの見出は改行を挟めず，先行するフィールドに為る
						edits.append((keyword.end, keyword.end, ";"))
			if (
				lang != "javascript" and node.type in ("call_expression", "member_expression", "subscript_expression") and
				node.text.startswith("new ")
			):
				callee: Node = node.children[0] if node.type == "call_expression" and node.children else node
				arguments: Node|None = (
					next((child for child in node.children if child.type == "arguments"), None) if node.type == "call_expression" else None
				)
				while callee.type in ("member_expression", "non_null_expression", "subscript_expression") and callee.children:
					callee = callee.children[0]
				if callee.type == "new_expression" and not any(child.type == "arguments" for child in callee.children):
					constructor: Node|None = next((child for child in callee.children if child.field == "constructor"), None)
					if constructor is not None:
						# new の直後から続く非 null・メンバ・添字は，１つのコンストラクタの参照である
						limit: int = arguments.start if arguments is not None else node.end
						edits.extend(((constructor.start, constructor.start, "("), (limit, limit, ")")))
						continue
			if node.type in ("call_expression", "subscript_expression") and node.children and node.children[0].type == "arrow_function":
				arrow: Node = node.children[0]
				body: Node|None = next((child for child in arrow.children if child.type == "statement_block"), None)
				following: Node|None = next((child for child in node.children[1:] if not is_comment(child.type)), None)
				if body is not None and following is not None and re.search(r"[\r\n\u2028\u2029]", text[body.end:following.start]):
					# 波括弧のアロー本体の直後は，括弧で囲まない限り呼出・添字の受手でない
					edits.append((body.end, body.end, ";"))
			if node.type == "class_body":
				members: list[Node] = [child for child in node.children if child.kind == "N" and not is_comment(child.type)]
				for member, following in zip(members, members[1:]):
					if (
						member.type == "field_definition" and member.text == "static" and following.type in ("field_definition", "method_definition")
					):
						gap: str = text[member.end:following.start]
						if not re.sub(r"/\*[\s\S]*?\*/|//[^\r\n]*", " ", gap).strip():
							edits.append((member.end, following.start, " "))
			updates: list[Node] = (
				[child for child in node.children if not is_comment(child.type)] if node.type == "update_expression" else []
			)
			if len(updates) == 2:
				operand, operator = updates
				if operator.type in ("++", "--") and re.search(r"[\r\n\u2028\u2029]", text[operand.end:operator.start]):
					# 後置の増減演算子の前に改行は置けず，次の前置式の開始に為る
					edits.append((operand.end, operand.end, ";"))
			statements: list[Node] = [child for child in node.children if child.kind == "N" and not is_comment(child.type)]
			for first, following in zip(statements, statements[1:]):
				first_children: list[Node] = [child for child in first.children if not is_comment(child.type)]
				if first.type != "expression_statement" or len(first_children) != 1:
					continue
				keyword: Node = first_children[0]
				gap: str = text[keyword.end:following.start]
				if keyword.type != "identifier" or re.sub(r"/\*[\s\S]*?\*/|//[^\r\n]*", " ", gap).strip():
					continue
				value: Node|None = following.children[0] if following.type == "expression_statement" and following.children else None
				while value is not None and value.type == "sequence_expression" and value.children:
					value = value.children[0]
				is_declaration: bool = keyword.text == "let" and (
					value is not None and value.type == "assignment_expression" and bool(value.children) and
					value.children[0].type == "identifier" or
					following.type == "statement_block" and re.match(r"\s*=", text[following.end:]) is not None
				)
				is_await: bool = keyword.text == "await" and async_owners.get(id(keyword)) is True
				is_export: bool = (
					keyword.text == "export" and
					following.type in ("class_declaration", "function_declaration", "lexical_declaration", "statement_block")
				)
				if is_declaration or is_export or is_await:
					# 宣言の予約語と次の構文の間は，自動セミコロンの境界でない
					edits.append((keyword.end, following.start, " "))
		if lang in ("typescript", "tsx"):
			for child in node.children:
				if is_comment(child.type):
					continue
				if child.type == "readonly_type" and not has_error([child]):
					operand: Node|None = next((part for part in child.children if part.kind == "N" and not is_comment(part.type)), None)
					while operand is not None and operand.type in ("conditional_type", "intersection_type", "union_type"):
						operand = next((part for part in operand.children if part.kind == "N" and not is_comment(part.type)), None)
					if operand is not None and child.start not in prefix_readonly and operand.end < child.end:
						# 前置型の被演算子へ弱い演算子を吸わせず，明示した括弧と別のエラーを保持する
						edits.extend(((child.start, child.start, "("), (operand.end, operand.end, ")")))
		if lang in ("typescript", "tsx") and node.type == "public_field_definition" and node.children:
			first: Node = node.children[0]
			following: Node|None = next((child for child in node.children[1:] if not is_comment(child.type)), None)
			if first.type == "accessor" and following is not None and re.search(r"[\r\n\u2028\u2029]", text[first.end:following.start]):
				# 改行で終わるメンバ名を自動アクセサの修飾子に読まない
				edits.append((first.start, first.end, "\"accessor\";"))
		if lang == "ruby" and node.type in ("binary", "element_reference", "range"):
			head: Node = node
			while head.type in ("binary", "element_reference", "range") and head.children:
				head = head.children[0]
			if head.type == "return" and head.children and head.children[0].type == "return":
				# 返戻の予約語は式の演算対象でなく，残る式全体を値として取る
				edits.extend(((head.children[0].end, head.children[0].end, " ("), (node.end, node.end, ")")))
				continue
		if lang == "ruby" and node.type in ("method", "singleton_method"):
			array: re.Match[str]|None = (
				re.fullmatch(r"def[ \t]+[A-Za-z_]\w*[!?]?[ \t]*=[ \t]*([A-Za-z_]\w*[!?]?)([ \t]+)(\[[\s\S]*\])", node.text)
			)
			if array is not None:
				edits.append((node.start + array.start(2), node.start + array.end(2), "("))
				edits.append((node.end, node.end, ")"))
			# 仮引数や代入の無い式本体に限り，呼出のヒアドキュメントを明示する
			match: re.Match[str]|None = (
				re.fullmatch(r"def[ \t]+[A-Za-z_]\w*[!?]?[ \t]*=[ \t]*([A-Za-z_]\w*[!?]?)([ \t]+)(<<[-~]?[A-Za-z_]\w*)", node.text)
			)
			if match is not None:
				name: str = re.sub(r"^<<[-~]?", "", match.group(3))
				if re.search(r"(?m)^[ \t]*" + re.escape(name) + r"\r?$", text[node.end:]):
					edits.append((node.start + match.start(2), node.start + match.end(2), "("))
					edits.append((node.end, node.end, ")"))
		if lang == "ruby" and node.type == "range" and node.children and node.children[-1].type in ("..", "..."):
			gap_match: re.Match[str]|None = re.match(r"(?:[ \t\r\n]|\#[^\r\n]*(?:\r?\n|$))*", text[node.end:])
			if gap_match is not None and "\n" in gap_match[0]:
				following_text: str = text[node.end + gap_match.end():]
				if following_text and following_text[0] not in ";)}]":
					# 範囲演算子の直後の改行は，終端の式へ継続する
					edits.append((node.end, node.end + gap_match.end(), " "))
		pending.extend(reversed(node.children))
	if not edits:
		# 原文の解析結果の返戻
		return roots
	pieces: list[str] = []
	pos: int = 0
	for begin, end, replacement in sorted(edits):
		pieces.extend((text[pos:begin], replacement))
		pos = end
	pieces.append(text[pos:])
	logical: str = "".join(pieces)
	# フォーマッターを経由しない独立した読取の返戻
	return parse_dump(dump_ast(logical, LANGS[lang][1]), logical)

def scss_argument_roots(text: str, roots: list[Node]) -> list[Node]:
	"""閉丸括弧前の空白で誤読した実引数列を，原文の位置へ対応付ける
	計算量：ソース長 B・ノード数 N と高々２回の再解析費用 P に対して O(B+N+P)
	引数：text: 解析用のソース，roots: 同じソースの独立した木
	戻値：完全な実引数列だけを修復し，残るエラーの原文を保持した木
	"""
	if not has_error(roots):
		# エラーの無い木の返戻
		return roots
	closers: set[int] = set()
	pending: list[Node] = list(roots)
	while pending:
		node: Node = pending.pop()
		if node.type == ")" and node.text == ")":
			closers.add(node.start)
		pending.extend(node.children)
	if not closers:
		# 実在する閉丸括弧を持たない木の返戻
		return roots
	# 引用・コメント・エスケープを先に読み，行コメントの終端改行も保持する
	pattern: re.Pattern[str] = re.compile(
		r"\\[\s\S]|/\*[\s\S]*?(?:\*/|$)|//[^\r\n]*(?:\r\n|[\r\n]|$)[ \t\r\n\f]*"
		r"|\"(?:\\[\s\S]|[^\"\\])*(?:\"|$)|'(?:\\[\s\S]|[^'\\])*(?:'|$)|[ \t\r\n\f]+"
	)
	gaps: dict[int, int] = {
		match.end(): match.start() for match in pattern.finditer(text) if match.end() in closers and match.group().isspace()
	}
	if not gaps:
		# 修復する空白を持たない木の返戻
		return roots
	# 試読と，試読で確認出来た空白だけの再読を順に行う
	for _ in range(2):
		positions: list[int] = []
		begin: int = 0
		for end, start in gaps.items():
			positions.extend(range(begin, start))
			begin = end
		positions.extend(range(begin, len(text)))
		logical: str = "".join(text[pos] for pos in positions)
		result: list[Node] = parse_dump(dump_ast(logical, "scss"), logical)
		confirmed: set[int] = set()
		work: list[Node] = list(result)
		while work:
			node = work.pop()
			if node.type in ("ERROR", "MISSING"):
				continue
			if node.type == "include_statement":
				for child in node.children:
					if child.type == "arguments" and not has_error([child]) and child.children:
						closing: Node = child.children[-1]
						if closing.type == ")" and closing.text == ")":
							confirmed.add(positions[closing.start])
			work.extend(node.children)
		if not set(gaps).issubset(confirmed):
			gaps = { end: start for end, start in gaps.items() if end in confirmed }
			if not gaps:
				# 完全な実引数列を確認出来ない木の返戻
				return roots
			continue
		# 半開区間の両端を別々に戻し，保護本文に含まれる空白を失わない
		work = list(result)
		while work:
			node = work.pop()
			start: int = node.start
			end: int = node.end
			node.start: int = positions[start] if start < len(positions) else len(text)
			node.end: int = positions[end - 1] + 1 if start < end else node.start
			node.source: str|None = text
			work.extend(node.children)
		for root in result:
			root.start: int = 0
			root.end: int = len(text)
		# 原文の位置へ戻した木の返戻
		return result
	# 対応を確定出来ない場合は元の保護領域を維持する
	return roots

def json_structure(roots: list[Node]) -> str|None:
	"""JSON のコメントと許容する末尾カンマだけを除き，値とメンバの順を比較する
	計算量：ソース長 N に対して O(N)
	引数：roots: 原文を保持する木
	戻値：重複キーも保持した構造．其の他の構文エラーは None
	"""
	text: str = "".join(root.text for root in roots)
	tokens: list[str] = re.findall(r'"(?:[^"\\]|\\.)*"|//[^\r\n]*|/\*[\s\S]*?\*/|[ \t\r\n]+|[{}\[\],:]|[^"{}\[\],:\s/]+|.', text)
	tokens = [token for token in tokens if not all(char in " \t\r\n" for char in token) and not token.startswith(("//", "/*"))]
	if any(token in ("-Infinity", "Infinity", "NaN") for token in tokens):
		# JSON の数値でない名前を受理しない結果の返戻
		return None
	# 文字列の内側を触らず，最後の区切だけを除く
	logical: str = " ".join(
		token for idx, token in enumerate(tokens) if token != "," or idx + 1 >= len(tokens) or tokens[idx + 1] not in ("}", "]")
	)
	try:
		value: object = json.loads(
			logical,
			object_pairs_hook=lambda pairs: ("object", pairs),
			parse_int=lambda number: ("number", numeric_spelling(number)),
			parse_float=lambda number: ("number", numeric_spelling(number))
		)
	except (ValueError, RecursionError):
		# 未対応のエラーを比較成功へ読み替えない結果の返戻
		return None
	# キーの順・重複と値の型を保持した構造の返戻
	return repr(value)

def python_structure(roots: list[Node]) -> str|None:
	"""Python の実際の優先順位・文字列・型位置を標準パーサーで独立に照合する
	計算量：ソース長 N に対して O(N)
	引数：roots: 入力又は候補の原文を持つ根
	戻値：位置情報を含まない構造．解析不能なら None
	"""
	try:
		parsed: ast.AST = ast.parse("".join(root.text for root in roots))
	except (SyntaxError, ValueError):
		# 未対応の構文を比較成功へ読み替えない結果の返戻
		return None
	for node in ast.walk(parsed):
		if (
			isinstance(node, ast.While) and isinstance(node.test, ast.Constant) and type(node.test.value) is int and node.test.value == 1
		):
			node.test: ast.expr = ast.Constant(value=True)
		if isinstance(node, ast.Delete):
			targets: list[ast.expr] = []
			pending_targets: list[ast.expr] = list(reversed(node.targets))
			while pending_targets:
				target: ast.expr = pending_targets.pop()
				if isinstance(target, ast.Tuple):
					pending_targets.extend(reversed(target.elts))
				else:
					targets.append(target)
			node.targets: list[ast.expr] = targets
	for function in ast.walk(parsed):
		if (
			not isinstance(function, (ast.FunctionDef, ast.AsyncFunctionDef)) or len(function.body) < 2 or
			not isinstance(function.body[-1], ast.Return) or function.body[-1].value is not None
		):
			continue
		body: list[ast.stmt] = function.body[:-1]
		if all(
			isinstance(node, ast.Pass) or isinstance(node, ast.Expr) and isinstance(node.value, ast.Constant) and
			(node.value.value is Ellipsis or not position and isinstance(node.value.value, str)) for position, node in enumerate(body)
		):
			continue
		pending: list[ast.AST] = list(body)
		has_value: bool = False
		while pending:
			node: ast.AST = pending.pop()
			if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda, ast.ClassDef)):
				continue
			has_value |= isinstance(node, (ast.Yield, ast.YieldFrom)) or isinstance(node, ast.Return) and node.value is not None
			pending.extend(ast.iter_child_nodes(node))
		is_void: bool = function.returns is None or isinstance(function.returns, ast.Constant) and function.returns.value is None
		if is_void and not has_value or isinstance(body[-1], (ast.Return, ast.Raise)):
			function.body: list[ast.stmt] = body
	# 値・演算・文の順序を含む構造の返戻
	return ast.dump(parsed, include_attributes=False)

def swift_type_names(roots: list[Node]) -> tuple[str, ...]:
	"""処理系の構文ダンプが省く型の綴りも，出現順を含めて照合する
	計算量：本文長 N に対して O(N)
	引数：roots: 原文の構文木
	戻値：通常コメント以外の名前の列
	"""
	# １６進の浮動小数を整数より先に読み，数値の後続のメンバ・範囲端を残す
	number_prefix: str = (
		r"^(?:0[xX][0-9a-fA-F_]+(?:\.[0-9a-fA-F_]+)?[pP][+-]?[0-9_]+|"
		r"0[xX][0-9a-fA-F_]+|0[bB][01_]+|0[oO][0-7_]+|"
		r"[0-9][0-9_]*(?:\.[0-9_]+)?(?:[eE][+-]?[0-9_]+)?)"
	)
	# 数値の接頭部を除き，通常コメント以外の名前の列の返戻
	return tuple(
		word for token in cpp_tokens("".join(root.text for root in roots)) if token.kind != "comment" for word in
		re.findall(r"[^\W\d]\w*", re.sub(number_prefix, "", token.text) if token.kind == "number" else token.text) if word not in
		("let", "return", "var")
	)

def auxiliary_address(match: re.Match[str]) -> str:
	"""実行アドレスを本文と区別出来る内部マーカーへ変える
	引数：match: 属性又は参照のアドレス
	戻値：属性の区切とアドレスのマーカー
	"""
	address: str = match[2].lower().lstrip("0")
	# ヌルと非ヌルの差を保持し，番号は構造の正規化後に確定する
	return match[1] + "\0" + (address or "null") + "\0"

def auxiliary_dump_metadata(dump: str) -> str:
	"""補助ダンプの位置を除き，実行アドレスを同一性を保って正規化する
	本文の値と判別子は位置・アドレスに似た綴りでも保持する
	計算量：ダンプ長 N に対して O(N)
	引数：dump: 処理系の標準出力
	戻値：名前・値・判別子・参照の関係を保持したダンプ
	"""
	parts: list[str] = []
	end: int = 0
	# 引用属性と範囲を先に読み，空白を含む１つの値を分割しない
	for match in re.finditer(
		r"(?:[A-Za-z_]\w*=)?(?:\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*')|(?:[A-Za-z_]\w*=)?<[^<>\n]*@\s+0[xX][0-9a-fA-F]+>|\b(?:range|source_range)=\[[^\]]*\]|[^\s()]+",
		dump
	):
		parts.append(dump[end:match.start()])
		token: str = match[0]
		content: str = token[1:-1] if token.startswith(("\"", "'")) else token
		key: str = content.partition("=")[0]
		is_literal: bool = key in ("text", "value") or token.startswith(("\"", "'")) and not re.match(r"[A-Za-z_]\w*=", content)
		if not is_literal:
			if re.fullmatch(r"(?:range|source_range)=\[[^\]]*\]|(?:byte_offset|column|line|source_offset)=[0-9]+", token):
				token = ""
			else:
				# 引用符の有無に依らず，参照の座標だけを除いて名前を残す
				token = re.sub(r"@(?:<[^<>\s]+>|[^\s\"'()@]+):[0-9]+:[0-9]+(?=$|[\"'\],;])", "", token)
				if re.fullmatch(r"location=(?:<[^<>\s]+>|[^\s\"'()]+):[0-9]+:[0-9]+", token):
					token = ""
				if key != "discriminator":
					token = re.sub(r"(=\s*[\"']?|@\s+)0[xX]([0-9a-fA-F]+)(?![\w.])", auxiliary_address, token)
		parts.append(token)
		end = match.end()
	parts.append(dump[end:])
	# 空白と構造の区切を変えず，メタデータを正規化したテキストの返戻
	return "".join(parts)

def swift_dump_structure(dump: str) -> tuple|None:
	"""Swift の構文ダンプの構造を読み，エラーノードを拒否する
	計算量：ダンプ長 N に対して O(N)
	引数：dump: 処理系の標準出力
	戻値：正規形．不完全なダンプ又は構文エラーなら None
	"""
	frames: list[tuple[list[str], list[tuple]]] = []
	roots: list[tuple] = []
	for token in re.findall(r'"(?:\\.|[^"\\])*"|[^\s()"]+|[()]', dump):
		if token == "(":
			frames.append(([], []))
		elif token == ")":
			if not frames:
				# 対応しない閉記号を持つダンプの拒否
				return None
			head, children = frames.pop()
			if not head:
				# 種類を持たないダンプの拒否
				return None
			node: tuple = tuple(head), tuple(children)
			if frames:
				frames[-1][1].append(node)
			else:
				roots.append(node)
		elif frames:
			frames[-1][0].append(token)
	if frames or len(roots) != 1:
		# 未完結又は複数の原文を持つダンプの拒否
		return None
	values: dict[int, tuple] = {}
	# 入力が宣言した中置演算子へ，標準の優先順位を推定しない
	custom_operators: set[str] = set()
	for heading in re.findall(r"\(infix_operator_decl\b[^()]*", dump):
		custom_operators.update(re.findall(r'"([^"\\]+)"', heading))
	pending: list[tuple[tuple, bool]] = [(roots[0], False)]
	while pending:
		node, ready = pending.pop()
		head, children = node
		if not ready:
			pending.append((node, True))
			pending.extend((child, False) for child in reversed(children))
			continue
		kind: str = head[0]
		prefix: str = ""
		if "=" in kind and kind.split("=", 1)[0] in ("original_init", "processed_init", "result", "type_repr", "where"):
			prefix, kind = kind.split("=", 1)
		if kind.startswith("error_") or kind.endswith("_error"):
			# 成功の終了値でも回復用のエラーノードは根拠にしない
			return None
		parts: list[tuple] = [values[id(child)] for child in children]
		heading: tuple[str, ...] = (
			kind,
			*(atom for atom in head[1:] if atom not in ("implicit", "single_expression", "trailing_semi"))
		)
		if kind in ("float_literal_expr", "integer_literal_expr"):
			# 数値の字形だけを揃え，リテラルの種別と正確な綴りの値を残す
			heading = tuple(
				json.dumps(numeric_spelling(json.loads(atom))) if
				idx and heading[idx - 1] == "value=" and atom.startswith("\"") else
				atom for idx, atom in enumerate(heading)
			)
		if kind == "source_file":
			heading = kind,
		if (
			kind == "sequence_expr" and len(parts) > 2 and parts[1] == parts[2] and
			parts[1][0][0] in ("coerce_expr", "conditional_checked_cast_expr", "forced_checked_cast_expr", "is_subtype_expr")
		):
			parts = [(("written_cast",), tuple(parts[:3])), *parts[3:]]
			if len(parts) == 1:
				value = parts[0]
				values[id(node)] = ((prefix,), (value,)) if prefix else value
				continue
		if kind == "sequence_expr" and len(parts) > 2 and len(parts) % 2:
			operators: list[str] = [
				part[0][part[0].index("name=") + 1].strip("\"") if
				part[0][0] == "unresolved_decl_ref_expr" and "name=" in part[0] else
				"" for part in parts[1::2]
			]
			if all(operator in SWIFT_PRECEDENCE and operator not in custom_operators for operator in operators):
				# 明示したグループを項として残し，未結合の標準演算列だけを結合する
				operands: list[tuple] = [parts[0]]
				pending_operators: list[str] = []
				for operator, operand in zip((*operators, ""), (*parts[2::2], ())):
					while pending_operators and (
						not operator or SWIFT_PRECEDENCE[pending_operators[-1]] > SWIFT_PRECEDENCE[operator] or
						operator != "??" and SWIFT_PRECEDENCE[pending_operators[-1]] == SWIFT_PRECEDENCE[operator]
					):
						right: tuple = operands.pop()
						left: tuple = operands.pop()
						operands.append(((*heading, "operator=" + pending_operators.pop()), (left, right)))
					if operator:
						pending_operators.append(operator)
						operands.append(operand)
				value: tuple = operands[0]
				values[id(node)] = ((prefix,), (value,)) if prefix else value
				continue
		if len(parts) == 1 and (
			kind in ("paren_expr", "unresolved_pattern_expr") or kind == "type_tuple" and len(heading) == 1 or
			kind == "pattern_expr" and parts[0][0][0].startswith("pattern_")
		):
			value: tuple = parts[0]
		else:
			if kind == "source_file":
				parts = [
					entry for part in parts for entry in
					(part[1][0][1] if part[0][0] == "top_level_code_decl" and len(part[1]) == 1 and part[1][0][0][0] == "brace_stmt" else (part,))
				]
			if kind in ("brace_stmt", "source_file"):
				declarations: tuple = tuple(part for part in parts if part[0][0] == "var_decl")
				parts = [
					entry for part in parts if part[0][0] != "var_decl" for entry in (part[1] if part[0][0] == "pattern_binding_decl" else (part,))
				]
				if declarations:
					parts.append((("declared_variables",), declarations))
			if kind in ("constructor_decl", "func_decl") and all(
				part[1] == ((("type_tuple",), ()),) or part[1] == ((("type_unqualified_ident", "id=", "\"Void\"", "unbound"), ()),) for part in
				parts if part[0] == ("result",)
			) and not any("?" in atom or "!" in atom for atom in heading):
				for idx, part in enumerate(parts):
					if part[0][0] == "brace_stmt":
						end: int = len(part[1]) - (bool(part[1]) and part[1][-1][0][0] == "declared_variables")
						if end > 1 and part[1][end - 1] == (("return_stmt",), ()):
							parts[idx] = part[0], part[1][:end - 1] + part[1][end:]
			if kind == "func_decl" and any(child[0][0].startswith("result=") for child in children):
				for idx, part in enumerate(parts):
					if part[0][0] == "brace_stmt" and len(part[1]) == 1 and part[1][0][0][0] == "return_stmt" and len(part[1][0][1]) == 1:
						parts[idx] = part[0], part[1][0][1]
			value = heading, tuple(parts)
		if prefix:
			value = (prefix,), (value,)
		values[id(node)] = value
	# 型・演算・評価順・束縛・文の所属を保持した処理系の構造の返戻
	return values[id(roots[0])]

@dataclass(frozen=True)
class AuxiliaryParser:
	"""外部の補助解析の起動・ダンプ読取・独立した観測の登録"""

	command: tuple[str, ...]
	normalize: Callable[[str], tuple|None]
	observation: Callable[[list[Node]], tuple]
	verified_versions: tuple[str, ...]

AUXILIARY_PARSERS: dict[str, AuxiliaryParser] = {
	"swift": AuxiliaryParser(("swift", "-frontend", "-dump-parse", "-"), swift_dump_structure, swift_type_names, ("6.1.2", "6.2.4"))
}

UNCONFIRMED_AUXILIARY_VERSION: str = "未確認（補助の処理系の版）"
UNCONFIRMED_AUXILIARY: str = "未確認（補助の処理系が入力を読めない）"

@lru_cache(maxsize=None)
def auxiliary_command(parser: AuxiliaryParser) -> tuple[str, ...]:
	"""PATH で選んだ処理系を１実行内で固定する
	引数：parser: 補助解析の登録
	戻値：実際の起動コマンド．未発見なら空
	"""
	compiler: str|None = which(parser.command[0])
	# 選択済の起動コマンドの返戻
	return (compiler, *parser.command[1:]) if compiler else ()

def auxiliary_run(command: list[str], text: str|None) -> subprocess.CompletedProcess[str]:
	"""補助の処理系を期限付で起動し，期限切れを処理系の拒否として返す
	引数：command: 起動コマンド，text: 標準入力
	戻値：標準出力・標準エラー・終了値
	"""
	try:
		# 起動結果の返戻
		return subprocess.run(command, input=text, text=True, capture_output=True, timeout=30)
	except subprocess.TimeoutExpired:
		# 応答の無い処理系を保存又は破壊の根拠にしない
		return subprocess.CompletedProcess(command, 124, "", "timed out after 30 seconds")

@lru_cache(maxsize=None)
def auxiliary_version(parser: AuxiliaryParser) -> subprocess.CompletedProcess[str]|None:
	"""選択した処理系のバージョンの実測を共有する
	引数：parser: 補助解析の登録
	戻値：バージョンの出力と終了値．未発見なら None
	"""
	command: tuple[str, ...] = auxiliary_command(parser)
	# 起動した処理系のバージョンの返戻
	return auxiliary_run([command[0], "--version"], None) if command else None

@lru_cache(maxsize=None)
def auxiliary_version_supported(parser: AuxiliaryParser) -> bool:
	"""ダンプの書式を確認したバージョンだけを比較の根拠にする
	引数：parser: 確認済のバージョンを持つ登録
	戻値：バージョンの制限が無いか確認済なら真．処理系の未発見は入力の未受理へ渡す
	"""
	result: subprocess.CompletedProcess[str]|None = auxiliary_version(parser)
	if not parser.verified_versions or result is None:
		# 未発見の処理系はダンプを返さない
		return True
	versions: list[str] = re.findall(r"\bversion\s+(\d+(?:\.\d+)+)(?![\w.])", result.stdout + result.stderr, re.I)
	# バージョンが不明な時も推測せず，確認済の登録とだけ照合する
	return not result.returncode and bool(versions) and versions[-1] in parser.verified_versions

@lru_cache(maxsize=None)
def auxiliary_identity(parser: AuxiliaryParser) -> str:
	"""選択方法・実行パス・バージョンを記録する
	引数：parser: 補助解析の登録
	戻値：実行環境の説明
	"""
	command: tuple[str, ...] = auxiliary_command(parser)
	if not command:
		# 未発見の記録の返戻
		return f"PATH {parser.command[0]}: 未発見; version=未取得"
	result: subprocess.CompletedProcess[str] = auxiliary_version(parser)
	# 選択方法と実測したバージョンの返戻
	return (
		f"PATH {parser.command[0]} -> {command[0]!r}; version={(result.stdout + result.stderr).strip()!r}; "
		f"version_exit={result.returncode}; verified_versions={parser.verified_versions!r}"
	)

@lru_cache(maxsize=64)
def auxiliary_parse(parser: AuxiliaryParser, text: str) -> subprocess.CompletedProcess[str]|None:
	"""同じ登録と原文の起動結果を共有する
	引数：parser: 補助解析の登録，text: 原文
	戻値：標準出力・標準エラー・終了値．未発見なら None
	"""
	command: tuple[str, ...] = auxiliary_command(parser)
	# 診断を含む起動結果の返戻
	return auxiliary_run(list(command), text) if command else None

def auxiliary_references(structure: tuple|None) -> tuple|None:
	"""比較に残る構造の出現順でアドレスへ番号を付ける
	構造の正規化で除いたノードを初出順へ数えない
	計算量：構造と本文の総要素数 N に対して O(N)
	引数：structure: 位置を除き，アドレスをマーカーにした正規形
	戻値：参照の同一性を保持した正規形
	"""
	if structure is None:
		# 解析出来ないダンプの拒否を保持する
		return None
	addresses: dict[str, int] = {}
	frames: list[list] = [[]]
	pending: list[tuple|str|None] = [structure]
	while pending:
		node: tuple|str|None = pending.pop()
		if node is None:
			value: tuple = tuple(frames.pop())
			frames[-1].append(value)
		elif isinstance(node, str):
			frames[-1].append(re.sub(r"\0[0-9a-f]+\0", lambda match: f"\0address_{addresses.setdefault(match[0], len(addresses))}\0", node))
		else:
			frames.append([])
			pending.append(None)
			pending.extend(reversed(node))
	# 元のタプルの構造と本文の順序を保持した結果の返戻
	return frames[0][0]

@lru_cache(maxsize=64)
def auxiliary_structure(parser: AuxiliaryParser, text: str) -> tuple|None:
	"""入力を受理した処理系の完全な構造だけを返す
	引数：parser: 補助解析の登録，text: 原文
	戻値：正規形．処理系の拒否又はエラーダンプなら None
	"""
	result: subprocess.CompletedProcess[str]|None = auxiliary_parse(parser, text)
	if result is None or result.returncode:
		# 未起動又は処理系の拒否を保持する
		return None
	# 受理した入力の正規形の返戻
	return auxiliary_references(parser.normalize(auxiliary_dump_metadata(result.stdout)))

def auxiliary_diagnostic(parser: AuxiliaryParser, text: str) -> str:
	"""未確認又は不一致の補助解析の状態を説明する
	引数：parser: 補助解析の登録，text: 原文
	戻値：終了値と標準エラーの先頭
	"""
	result: subprocess.CompletedProcess[str]|None = auxiliary_parse(parser, text)
	# 解析の状態と診断の返戻
	return f"exit={result.returncode}, stderr={result.stderr[:1600]!r}" if result is not None else "exit=未起動, stderr=''"

def auxiliary_comparison(parser: AuxiliaryParser, before: list[Node], after: list[Node]) -> str:
	"""入力の受理を先に確認し，候補の拒否・構造差と未確認を区別する
	引数：parser: 補助解析の登録，before: 入力，after: 候補
	戻値：保存は空，不一致又は未確認は其の分類
	"""
	if parser.observation(before) != parser.observation(after):
		# 独立した観測の不一致の返戻
		return "構造差"
	if not auxiliary_version_supported(parser):
		# 構造の相違を解釈出来ないバージョンのダンプを保存又は破壊の根拠にしない
		return UNCONFIRMED_AUXILIARY_VERSION + " — " + auxiliary_identity(parser)
	source: str = "".join(root.text for root in before)
	original: tuple|None = auxiliary_structure(parser, source)
	if original is None:
		# 入力の受理を確定出来ない理由の返戻
		return UNCONFIRMED_AUXILIARY + " — " + auxiliary_identity(parser) + "; " + auxiliary_diagnostic(parser, source)
	candidate: tuple|None = auxiliary_structure(parser, "".join(root.text for root in after))
	# 候補の拒否も含む照合結果の返戻
	return "" if candidate is not None and original == candidate else "構造差"

def classify(lang: str, before_roots: list[Node], after_roots: list[Node], input_index: DocIndex|None = None) -> str:
	"""整形前後の木グループを比較し破壊種別（"" は保存）を判定する
	計算量：木グループのノード数 N・内容の総文字数 B・マクロ等の保護する観測の照合費用 P に対して最悪 O(N²+NB+P)

	引数：
		lang: 言語キー
		before_roots: 整形前の木グループ
		after_roots: 整形後の木グループ
		input_index: 行幅間で共有する入力側の観測

	戻値：
		破壊種別又は補助解析の未確認と理由（保存時は空文字列）
	"""
	if lang == "ruby":
		before_roots = ruby_comparison_roots(before_roots)
		after_roots = ruby_comparison_roots(after_roots)
		input_index = None
	if (
		source_observation_signature(before_roots, lang) != source_observation_signature(after_roots, lang) or
		protected_observation_signature(before_roots, lang) != protected_observation_signature(after_roots, lang)
	):
		# 物理位置と元のテキストの観測を，論理行への適応より先に照合する
		return "構造差"
	if lang in ("c", "cpp", "h"):
		input_directives, input_comments, input_inner = cpp_directive_observation(before_roots)
		output_directives, output_comments, output_inner = cpp_directive_observation(after_roots)
		if input_directives != output_directives:
			# 生の解析が条件の終端を誤読しても，指示の本文を次の行へ越境させない
			return "構造差"
		for (
			required,
			original,
			available
		) in (input_inner, input_comments, output_comments), (output_inner, output_comments, input_comments):
			counts: Counter[str] = Counter(original)
			candidate_counts: Counter[str] = Counter(available)
			remaining: Iterator[str] = iter(available)
			if (
				any(counts[part] > candidate_counts[part] for part in required) or
				not all(any(part == candidate for candidate in remaining) for part in required)
			):
				# 指示の前へ移した説明を許可し，本文の脱落・追加・順序の変更は拒む
				return "ドキュメント破壊"
	documentation_before: list[Node] = before_roots
	documentation_after: list[Node] = after_roots
	adapted: list[Node] = repair_comparison_source(lang, comparison_roots(lang, before_roots))
	if adapted is not before_roots:
		before_roots = adapted
		input_index = None
	after_roots = repair_comparison_source(lang, comparison_roots(lang, after_roots))
	if lang == "java":
		# Java の説明対象とコメント境界は，トークン化前の Unicode 展開で確定する
		documentation_before, documentation_after = before_roots, after_roots
	if lang == "ruby" and heredoc_signature(before_roots) != heredoc_signature(after_roots):
		# 通常の文列へ読み替えないヒアドキュメントの内容差の返戻
		return "内容破壊"
	has_native_match: bool = lang in ("c", "cpp", "h") and cpp_token_signature(before_roots) == cpp_token_signature(after_roots)
	if lang in ("json", "jsonc") and (native := json_structure(before_roots)) is not None:
		has_native_match = native == json_structure(after_roots)
	if lang == "python" and (native := python_structure(before_roots)) is not None:
		candidate_structure: str|None = python_structure(after_roots)
		if candidate_structure is not None and native != candidate_structure:
			# 実際の優先順位・真偽値化の回数を，誤読した木の一致で打ち消さない
			return "構造差"
		has_native_match = native == candidate_structure
	auxiliary_problem: str = ""
	if parser := AUXILIARY_PARSERS.get(lang):
		# 通常の照合で確定しない構文だけを補助解析へ渡す
		has_native_match = not has_error(after_roots) and allowed_structure(lang, before_roots, after_roots)
		if not has_native_match:
			auxiliary_problem = auxiliary_comparison(parser, before_roots, after_roots)
			if auxiliary_problem == "構造差":
				# 受理済の入力に対する候補の不一致の返戻
				return auxiliary_problem
			has_native_match = not auxiliary_problem
	if not has_native_match and not auxiliary_problem and has_error(after_roots):
		if not has_error(before_roots):
			# 完成候補で初めて生じたエラーを，保護領域へ読み替えない
			return "構文破壊"
		problem, reason = partial_structure(lang, before_roots, after_roots)
		if problem or reason:
			# 保護領域又は外側の対応を確認出来ない結果の返戻
			return problem or "構文破壊"
		has_native_match = True
	if lang in ("c", "cpp", "h"):
		# コメントの内容と所属は翻訳前の両方の木から独立に照合する
		directive_before: list[Node] = documentation_before
		directive_after: list[Node] = documentation_after
	else:
		directive_before, directive_after = before_roots, after_roots
	if directive_signature(directive_before, lang) != directive_signature(directive_after, lang):
		# 本文・適用境界・有効性の変更の返戻
		return "ディレクティブ破壊"
	if not has_native_match and not auxiliary_problem and not allowed_structure(lang, before_roots, after_roots):
		if content_signature(before_roots) != content_signature(after_roots):
			# 構造とデコード値で同値を確認出来ない本文差の返戻
			return "内容破壊"
		# 保存条件を満たす許可変換に対応しない構造差の返戻
		return "構造差"
	if not documentation_preserved(lang, documentation_before, documentation_after, input_index):
		# 説明対象・構造・本文・出現順の破壊の返戻
		return "ドキュメント破壊"
	# 独立した保持条件も満たした上で保存又は未確認を返す
	return auxiliary_problem

def node_location(node: Node|None) -> str:
	"""構造差のノードを種類・原文位置・本文で表示する
	計算量：原文長 N に対して O(N)
	引数：node: 入力又は候補の対応ノード
	戻値：１行の診断用テキスト
	"""
	if node is None:
		# 処理結果の返戻
		return "<ノード無>"
	source: str = node.source or node.text
	line: int = source.count("\n", 0, node.start) + 1
	column: int = node.start - source.rfind("\n", 0, node.start)
	# 処理結果の返戻
	return f"{node.type}@{line}:{column}[{node.start}:{node.end}] {node.text[:200]!r}"

def native_difference(before: tuple|None, after: tuple|None) -> str:
	"""補助処理系の正規形で最初に異なるノードの見出を表示する
	計算量：正規形のノード数 N に対して O(N)
	引数：before: 入力の正規形，after: 候補の正規形
	戻値：種類と構文情報の対．一致又は解析不可の場合は其の状態
	"""
	if before is None or after is None:
		# 解析失敗を構造の一致と区別した状態の返戻
		return "解析不可"
	pending: list[tuple[tuple|None, tuple|None]] = [(before, after)]
	while pending:
		first, second = pending.pop()
		if first is None or second is None or first[0] != second[0]:
			# 最初に異なる種類又は構文情報の返戻
			return repr(first[0] if first else None)[:800] + " / " + repr(second[0] if second else None)[:800]
		left: tuple = first[1]
		right: tuple = second[1]
		for idx in range(max(len(left), len(right)) - 1, -1, -1):
			pending.append((left[idx] if idx < len(left) else None, right[idx] if idx < len(right) else None))
	# 全ノードの一致の返戻
	return "一致"

def structure_difference(lang: str, before: list[Node], after: list[Node]) -> str:
	"""生の木の先頭差と，補助処理系が照合を確定出来ない理由を表示する
	計算量：木のノード数 N・原文長 B・処理系の費用 P に対して O(N+B+P)
	引数：lang: 言語，before: 入力の木，after: 候補の木
	戻値：ノードの対と補助解析の診断
	送出例外：OSError: 起動に失敗した場合
	"""
	pending: list[tuple[Node|None, Node|None]] = [
		(before[idx] if idx < len(before) else None, after[idx] if idx < len(after) else None) for idx in
		range(max(len(before), len(after)) - 1, -1, -1)
	]
	detail: str = "生の木に差が無い（許可変換又はソース観測の条件を確認出来ない）"
	while pending:
		first, second = pending.pop()
		if first is None or second is None or first.type != second.type or not first.children and first.text != second.text:
			detail = "生の木の先頭差: 入力 " + node_location(first) + " / 候補 " + node_location(second)
			break
		left: list[Node] = [part for part in first.children if not is_comment(part.type)]
		right: list[Node] = [part for part in second.children if not is_comment(part.type)]
		for idx in range(max(len(left), len(right)) - 1, -1, -1):
			pending.append((left[idx] if idx < len(left) else None, right[idx] if idx < len(right) else None))
	if parser := AUXILIARY_PARSERS.get(lang):
		detail += "; " + auxiliary_identity(parser)
		for label, roots in ("入力", before), ("候補", after):
			detail += f"; {label}: " + auxiliary_diagnostic(parser, "".join(root.text for root in roots))
		left_native: tuple|None = auxiliary_structure(parser, "".join(root.text for root in before))
		right_native: tuple|None = auxiliary_structure(parser, "".join(root.text for root in after))
		detail += "; native先頭差=" + native_difference(left_native, right_native)
	# 処理結果の返戻
	return detail

def type_script_difference(lang: str, before: list[Node], after: list[Node]) -> str:
	"""不一致の候補で変更された不変指定を，宣言子の順と非変換の理由で説明する
	計算量：ノード数 N・最大スコープ深さ D に対して O(ND)
	引数：lang: 言語キー，before: 入力の木，after: 候補の木
	戻値：変更された宣言子の詳細．対象外なら空
	"""
	if lang not in ("typescript", "tsx"):
		# 型の保存条件の対象外の返戻
		return ""
	left: tuple[NormNode, ...] = tuple(part for root in before for part in normalize(root, lang))
	right: tuple[NormNode, ...] = tuple(part for root in after for part in normalize(root, lang))
	reasons: dict[int, str] = {}
	confirmed: set[int] = type_script_constants(left, reasons)
	declarations: list[list[tuple[str, NormNode]]] = []
	for roots in left, right:
		items: list[tuple[str, NormNode]] = []
		for node in transform_nodes(roots):
			if node[0] != "lexical_declaration":
				continue
			keyword: str = next((child[0] for child in node[2] if child[0] in ("const", "let")), "")
			items.extend((keyword, child) for child in node[2] if child[0] == "variable_declarator")
		declarations.append(items)
	if len(declarations[0]) != len(declarations[1]):
		# 宣言子の欠落又は追加の詳細の返戻
		return f"宣言子数 {len(declarations[0])} → {len(declarations[1])}"
	details: list[str] = []
	for idx, ((keyword, node), (candidate, other)) in enumerate(zip(*declarations), 1):
		if keyword == candidate:
			continue
		name: str = ", ".join(binding[1] or "?" for binding in binding_names(node))
		reason: str = reasons.get(id(node), "型の条件は確認済，他の構造・保存条件の不一致" if id(node) in confirmed else "保存条件が未確認")
		details.append(f"宣言子 {idx} [{name}] {keyword} → {candidate}: {reason}")
	# 位置の対応を意味の受理条件として使わない診断の返戻
	return "；".join(details)

def dump_input_check() -> list[str]:
	"""全出力形式で，空の正常入力を受理し，読込・出力の失敗を拒否する
	計算量：固定数の小入力に対するパーサーの起動費用 P に対し O(P)
	戻値：
		入口の成功・失敗の契約と違うケースの一覧
	送出例外：
		OSError: 一時入力の作成又はパーサーの起動が失敗する場合
		subprocess.TimeoutExpired: パーサーが制限時間内に完了しない場合
	"""
	found: list[str] = []
	mode: tuple[str, ...]
	command: list[str]
	stdin: bytes|None
	temp: str
	content: bytes
	with TemporaryDirectory(prefix="shavefmt-dump-input-") as temp:
		base: Path = Path(temp)
		directory: Path = base / "directory.cpp"
		directory.mkdir()
		for mode in (), ("--types-only",), ("--spans",):
			for content in b"", b"int x;":
				path: Path = base / "source.cpp"
				path.write_bytes(content)
				for command, stdin in ([str(path)], None), (["-", "cpp"], content):
					result: subprocess.CompletedProcess[bytes] = (
						subprocess.run([str(DUMPAST), *mode, *command], input=stdin, capture_output=True, timeout=COMMAND_TIMEOUT)
					)
					if result.returncode or result.stderr or not result.stdout:
						found.append(f"正常な入力を拒否：{mode} {command} {content!r}")
					elif content and b"declaration" not in result.stdout:
						found.append(f"正常な入力の宣言が無い：{mode} {command}")
			result: subprocess.CompletedProcess[bytes] = (
				subprocess.run([str(DUMPAST), *mode, str(directory)], capture_output=True, timeout=COMMAND_TIMEOUT)
			)
			if not result.returncode or not result.stderr or result.stdout:
				found.append(f"ディレクトリの読込失敗を受理：{mode}")
			if os.name == "posix":
				descriptor: int = os.open(directory, os.O_RDONLY)
				try:
					result = subprocess.run([str(DUMPAST), *mode, "-", "cpp"], stdin=descriptor, capture_output=True, timeout=COMMAND_TIMEOUT)
				finally:
					os.close(descriptor)
				if not result.returncode or not result.stderr or result.stdout:
					found.append(f"標準入力の読込失敗を受理：{mode}")
				reader: int
				writer: int
				reader, writer = os.pipe()
				os.close(reader)
				try:
					result = subprocess.run(
						[str(DUMPAST), *mode, "-", "cpp"],
						input=b"int x;",
						stdout=writer,
						stderr=subprocess.PIPE,
						timeout=COMMAND_TIMEOUT
					)
				finally:
					os.close(writer)
				if not result.returncode:
					found.append(f"出力先の失敗を成功にした：{mode}")
	# 入力契約の不一致の返戻
	return found

def dump_traversal_check() -> list[str]:
	"""深い構文木の型一覧と，通常出力の先行順を独立した木の形と照合する
	計算量：ネストの深さ D とパーサーの実行費用 P に対して O(D+P)
	戻値：
		出力の失敗又は走査順の食違の一覧
	送出例外：
		OSError: パーサーの起動が失敗する場合
		RuntimeError: 通常出力のパーサーが失敗する場合
		ValueError: パーサーの出力が不正な場合
		json.JSONDecodeError: パーサーのノード値が有効な JSON でない場合
		subprocess.TimeoutExpired: パーサーが制限時間内に完了しない場合
	"""
	# 呼出スタックに依存しない走査の深さと，括弧の先行順の期待
	depth: int = 32768
	source: str = "x=" + "(" * depth + "1" + ")" * depth + ";\n"
	expected: list[str] = ["program", "expression_statement", "assignment_expression", "identifier", "="]
	expected += ["parenthesized_expression", "("] * depth + ["number"] + [")"] * depth + [";"]
	result: subprocess.CompletedProcess[str] = subprocess.run(
		[str(DUMPAST), "--types-only", "-", "js"],
		input=source,
		capture_output=True,
		text=True,
		timeout=COMMAND_TIMEOUT
	)
	found: list[str] = []
	if result.returncode or result.stderr or result.stdout.splitlines() != expected:
		found.append(f"深い AST の型一覧が不正：終了値 {result.returncode}")
	# 通常出力の罫線は，括弧と演算を持つ代入の親子関係を表す
	shallow: str = "x=(1)+2;\n"
	roots: list[Node] = parse_dump(dump_ast(shallow, "js"), shallow)
	pending: list[tuple[Node, int]] = [(root, 0) for root in reversed(roots)]
	actual: list[tuple[str, int]] = []
	while pending:
		node: Node
		level: int
		node, level = pending.pop()
		actual.append((node.type, level))
		pending.extend((child, level + 1) for child in reversed(node.children))
	if actual != [
		("program", 0),
		("expression_statement", 1),
		("assignment_expression", 2),
		("identifier", 3),
		("=", 3),
		("binary_expression", 3),
		("parenthesized_expression", 4),
		("(", 5),
		("number", 5),
		(")", 5),
		("+", 4),
		("number", 4),
		(";", 2)
	]:
		found.append("通常の AST 出力の先行順・親子関係が不正")
	# 既定の罫線出力も残し，位置の出力への移行で別の利用経路を失わない
	ordinary: subprocess.CompletedProcess[str] = (
		subprocess.run([str(DUMPAST), "-", "js"], input=shallow, capture_output=True, text=True, timeout=COMMAND_TIMEOUT)
	)
	if ordinary.returncode or ordinary.stderr:
		found.append("既定の AST 出力が失敗した")
	else:
		ordinary_roots: list[Node] = parse_dump(ordinary.stdout, shallow)
		plain: list[tuple[str, int, int]] = [(node.type, begin, end) for node, begin, end in source_leaves(roots)]
		ordinary_types: list[str] = []
		pending_nodes: list[Node] = list(reversed(ordinary_roots))
		while pending_nodes:
			node = pending_nodes.pop()
			ordinary_types.append(node.type)
			pending_nodes.extend(reversed(node.children))
		if (
			ordinary_types != [name for name, _ in actual] or plain !=
			[("identifier", 0, 1), ("=", 1, 2), ("(", 2, 3), ("number", 3, 4), (")", 4, 5), ("+", 5, 6), ("number", 6, 7), (";", 7, 8)]
		):
			found.append("既定の走査順又は独立した原文の範囲と違う")
	# 走査の食違の返戻
	return found

def deep_walk_check() -> list[str]:
	"""深い正当な木と末端の反例で，全ての木の走査の順序と拒否を確かめる
	期待は木の構造と，保持するトークン・束縛から手で定める
	計算量：深さ D と各検査の費用 P に対して O(D+P)
	戻値：
		走査の順序又は深さへの依存が違う検査の一覧
	送出例外：
		OSError: パーサーの起動が失敗する場合
		RuntimeError: パーサーの起動又は原文の対応付が失敗する場合
		ValueError: パーサーの出力が不正な場合
		json.JSONDecodeError: パーサーの出力をデコード出来ない場合
		subprocess.TimeoutExpired: パーサーが制限時間内に完了しない場合
	"""
	# 共通の生ノードを使う検査の読取
	from verify_space_random_ast import fingerprint
	from verify_design_invariants import cpp_helper_entries
	from verify_golden_spec import syntax_lines
	from verify_runtime import merge_text, python_tree
	# 呼出スタックの上限を超える木の構築
	depth: int = 2048
	root: Node = Node("", "N", "wrapper")
	current: Node = root
	for _ in range(depth):
		child: Node = Node("", "N", "wrapper")
		current.children: list[Node] = [child]
		current = child
	leaf: Node = Node("value", "N", "string_content")
	current.children: list[Node] = [leaf]
	found: list[str] = []
	# 親リンクの欠落を，最適化で除かれる `assert` だけに任せない
	for has_node in False, True:
		ancestors: Ancestors = Ancestors(None, leaf) if has_node else Ancestors()
		ancestors.depth: int = 2 if has_node else 1
		try:
			ancestors[-ancestors.depth]
		except RuntimeError:
			pass
		else:
			found.append("宣言した深さと矛盾する祖先のリンクを受理した")
	valid: Ancestors = Ancestors(Ancestors(None, root), leaf)
	if valid[-1] is not leaf or valid[-2] is not root:
		found.append("正当な祖先の順序が変わった")
	# Unicode のバイト位置と，同じ本文の別々の出現を独立した位置で識別する
	span_source: str = "字 x x"
	span_dump: str = "\n".join(
		(
			"[0,\"N\",\"program\",0,7,\"\"]",
			"[1,\"N\",\"identifier\",0,3,\"\"]",
			"[1,\"N\",\"identifier\",4,5,\"\"]",
			"[1,\"N\",\"identifier\",6,7,\"\"]"
		)
	)
	span_roots: list[Node] = parse_spans(span_source, span_dump)
	if [(start, end) for _, start, end in source_leaves(span_roots)] != [(0, 1), (2, 3), (4, 5)]:
		found.append("Unicode 又は同じ本文の範囲が違う")
	try:
		parse_spans("x", "[0,\"N\",\"program\",0,1,\"\"]\n[1,\"N\",\"identifier\",0,2,\"\"]")
	except ValueError:
		pass
	else:
		found.append("所有範囲外のダンプを受理した")
	expected: str = "(wrapper " * (depth + 1) + "(string_content 'value')" + ")" * (depth + 1)
	if has_error([root]) or Node.has_unscoped_binding(root) or forest_sexpr([root]) != expected:
		found.append("深い正常な木のエラー・束縛・正規形が違う")
	if content_signature([root]) != ("value",) or len(fingerprint([root])) != depth + 2:
		found.append("深い正常な木の内容・指紋が違う")
	leaf.type: str = "ERROR"
	if not has_error([root]):
		found.append("深い末端のエラーを見逃した")
	leaf.type: str = "lexical_declaration"
	if not Node.has_unscoped_binding(root):
		found.append("深い末端の束縛を見逃した")
	# 内側で閉じる束縛を外側の本体へ持ち出さない
	binding_leaf: Node = Node("", "N", "lexical_declaration")
	binding_scope: Node = Node("", "N", "statement_block")
	binding_scope.children: list[Node] = [binding_leaf]
	binding_parent: Node = Node("", "N", "if_statement")
	binding_parent.children: list[Node] = [binding_scope]
	binding_facts: TreeFacts = TreeFacts([binding_parent])
	if not binding_facts.binding[binding_leaf] or binding_facts.binding[binding_scope] or binding_facts.binding[binding_parent]:
		found.append("有効範囲に従う束縛の投影が違う")
	# 空の並びの全ての層を調べ，末端の値を見逃さない事の照合
	current = root
	while current.children:
		current.type: str = "argument_list"
		current = current.children[0]
	current.kind, current.type, current.text = "U", ")", ")"
	if not Node.is_empty_list(root):
		found.append("深い空の実引数の並びを拒んだ")
	current.kind, current.type, current.text = "N", "identifier", "value"
	if Node.is_empty_list(root):
		found.append("空でない実引数の並びを透過した")
	# 深い単項式の正準形と，末端の値の違いを独立した期待で検査する
	for count in 500, 1000, 2000, 4000:
		leaf: Node = Node("value", "N", "identifier")
		unary: Node = leaf
		for _ in range(count):
			parent: Node = Node("", "N", "unary_expression")
			parent.children: list[Node] = [Node("!", "U", "!"), unary]
			unary = parent
		expected: str = "(unary_expression (! '!') " * count + "(identifier 'value')" + ")" * count
		if forest_sexpr([unary], "javascript") != expected:
			found.append(f"深い単項式の正準形が違う：{count}")
		leaf.text: str = "other"
		if forest_sexpr([unary], "javascript") == expected:
			found.append(f"深い単項式の値の変更を見逃した：{count}")
	# 非空の末端を持つ重なった並びも，同じ計数結果を共有する
	leaf = Node("value", "N", "identifier")
	current = leaf
	for _ in range(depth):
		parent = Node("", "N", "argument_list")
		parent.children: list[Node] = [current]
		current = parent
	if forest_sexpr([current]) != "(identifier 'value')":
		found.append("重なった並びの非空の末端を失った")
	leaf.text: str = "other"
	if forest_sexpr([current]) != "(identifier 'other')":
		found.append("編集前の並びの計数結果を持ち越した")
	# 実際のダンプと，葉だけを変えた反例の意味比較
	source: str = "x=" + "(" * 512 + "1" + ")" * 512 + ";\n"
	candidate: str = "x=2;\n"
	parsed: list[Node] = parse_dump(dump_ast(source, "js"), source)
	changed: list[Node] = parse_dump(dump_ast(candidate, "js"), candidate)
	if has_error(parsed) or forest_sexpr(parsed, "javascript") == forest_sexpr(changed, "javascript"):
		found.append("深い実際の構文の値の変更を見逃した")
	# 名前空間・JSX の値・Python の式・埋込領域の既存の走査の照合
	code: str = "namespace N{" * depth + "void f();" + "}" * depth
	if cpp_helper_entries(code) != [("N::" * depth + "f", [], None, "void ")]:
		found.append("深い名前空間の宣言が違う")
	value: object = { "tag": "p", "props": { "id": 1 }, "children": ["first", "second"] }
	for _ in range(depth):
		value = [value]
	merged: object = merge_text(value)
	for _ in range(depth):
		merged = merged[0]
	if merged != { "tag": "p", "props": { "id": 1 }, "children": ["firstsecond"] }:
		found.append("深い JSX の子の連結が違う")
	if python_tree("x=" + "+".join(["1"] * 512)) == python_tree("x=2"):
		found.append("深い Python の式の変更を見逃した")
	if syntax_lines("html/embedded.out", ["<script>const s=\"text\";</script>"]).lines != ["<script>const s=\"____\";</script>"]:
		found.append("埋込の逐語本文を構文と誤認した")
	# 深い木の正誤対の結果の返戻
	return found

SELF_CHECK_PAIRS.extend([("c", "指示の論理行 #", "#define X 1\n", "#define X\n1\n", False)])

def protected_error_tree(lang: str, roots: list[Node]) -> tuple[list[Node], tuple[tuple[str, str, str, str], ...]]:
	"""最外のエラー本文と隣接トークンを控え，外側の独立した木を複写する
	計算量：ノード数 N と記録する本文長 B に対して O(N+B)
	引数：lang: 対象言語，roots: 入力又は候補の独立した解析木
	戻値：保護ノードを持つ木と，本文・種類・前後のトークンの原文順の記録
	"""
	copied: list[Node] = []
	leaves: list[Node] = []
	zones: list[int] = []
	pending: list[tuple[Node, list[Node]]] = [(root, copied) for root in reversed(roots)]
	while pending:
		node, output = pending.pop()
		is_error: bool = node.type in ("ERROR", "MISSING")
		protected: str = node.text
		if is_error and lang in ("c", "cpp", "h") and "　" in protected:
			tokens: list[CppToken] = list(cpp_tokens(protected))
			if all(token.kind in ("space", "comment") for token in tokens):
				# エラーに巻き込まれた通常コメントの区切空白だけを除き，改行と不正な文字は保つ
				protected = "".join("<comment>" if token.kind == "comment" else re.sub(r"[ \t]", "", token.text) for token in tokens)
		copy: Node = Node(
			protected if is_error else node.text if node.source is None else "",
			node.kind,
			"protected_error" if is_error else node.type
		)
		(
			copy.source,
			copy.start,
			copy.end,
			copy.field
		) = node.source if not is_error or protected == node.text else None, node.start, node.end, node.field
		output.append(copy)
		if is_error or not node.children:
			if is_error:
				zones.append(len(leaves))
			if is_error or not is_comment(node.type):
				leaves.append(copy if is_error else node)
		else:
			pending.extend((child, copy.children) for child in reversed(node.children))
	facts: list[tuple[str, str, str, str]] = []
	for pos in zones:
		node: Node = leaves[pos]
		facts.append((node.type, node.text, leaves[pos - 1].text if pos else "", leaves[pos + 1].text if pos + 1 < len(leaves) else ""))
	# 本文と境界を独立に求めた構造の返戻
	return copied, tuple(facts)

def linkage_body_roots(lang: str, roots: list[Node]) -> list[Node]:
	"""同一トークン列を別途確認するリンケージの囲みを，部分整形の本文検査から分離する
	計算量：原文長 B・ノード数 N と解析費用 P に対して O(B+N+P)
	引数：lang: C 系の言語，roots: 独立した原文の木
	戻値：リンケージの囲みだけを除き，未知の宣言等のエラーを残した木
	"""
	text: str = "".join(root.text for root in roots)
	tokens: list[CppToken] = []
	is_start: bool = True
	is_directive: bool = False
	for token in cpp_tokens(text):
		if token.kind == "space":
			if "\n" in token.text:
				is_start, is_directive = True, False
			continue
		if token.kind == "comment":
			continue
		if is_start and cpp_token_role(token.text) == "#":
			is_directive = True
		is_start = False
		if not is_directive:
			tokens.append(token)
	pairs: dict[int, int] = {}
	stack: list[int] = []
	for idx, token in enumerate(tokens):
		if token.text == "{":
			stack.append(idx)
		elif token.text == "}" and stack:
			pairs[stack.pop()] = idx
	masked: list[str] = list(text)
	changed: bool = False
	for idx, token in enumerate(tokens):
		if token.text != "extern":
			continue
		end: int = idx + 1
		while end < len(tokens) and tokens[end].kind == "literal":
			end += 1
		if end == idx + 1 or end not in pairs:
			continue
		for part in (*tokens[idx:end + 1], tokens[pairs[end]]):
			masked[part.start:part.end] = re.sub(r"[^\r\n]", " ", part.text)
		changed = True
	if not changed:
		# 対を成すリンケージの囲みが無い入力の返戻
		return roots
	logical: str = "".join(masked)
	result: list[Node] = parse_dump(dump_ast(logical, "cpp" if lang == "h" else lang), logical)
	pending: list[Node] = list(result)
	while pending:
		node: Node = pending.pop()
		node.source: str|None = text
		pending.extend(node.children)
	# 元の範囲と本文を持つ，未知のトークンを保護する木の返戻
	return result

def partial_structure(lang: str, before: list[Node], after: list[Node]) -> tuple[str, str]:
	"""保護領域の保持と其の外側の変換を，完成候補の独立した記録と比較する
	計算量：ノード数 N・本文長 B・通常の比較費用 P に対して O(N+B+P)
	引数：
		lang: 対象言語
		before: 入力の木
		after: 完成候補の木
	戻値：保持違反と，対応を確定出来ない場合の理由
	"""
	before = repair_comparison_source(lang, comparison_roots(lang, before))
	after = repair_comparison_source(lang, comparison_roots(lang, after))
	if lang in ("c", "cpp", "h") and cpp_token_signature(before) == cpp_token_signature(after):
		# 条件・リンケージ・其の所属を含む全トークンの一致が前提である
		before = linkage_body_roots(lang, before)
		after = linkage_body_roots(lang, after)
	left, original = protected_error_tree(lang, before)
	right, candidate = protected_error_tree(lang, after)
	if original != candidate:
		if "".join(root.text for root in before) == "".join(root.text for root in after):
			# 原文が同じで解析範囲だけが異なる場合は未確認として返す
			return "", "原文は同じだが，パーサーの領域分割の対応が未確定"
		if tuple(fact[1] for fact in original) == tuple(fact[1] for fact in candidate):
			# 同じ本文に接するトークンの変更は保持違反として返す
			return "保護境界破壊", ""
		# 本文の変更・欠落・順序違反の返戻
		return "保護本文破壊", ""
	if not documentation_preserved(lang, before, after):
		# 保護領域の外のドキュメントの欠落の返戻
		return "ドキュメント破壊", ""
	if directive_signature(before, lang) != directive_signature(after, lang):
		# ディレクティブの本文・適用境界の差の返戻
		return "ディレクティブ破壊", ""
	if not allowed_structure(lang, left, right):
		if lang in ("css", "scss") and protected_observation_signature(before, lang) != protected_observation_signature(after, lang):
			# 保護内容の差を，外側の空白の選択表現として受理しない
			return "内容破壊", ""
		# 未登録の読違又は変換を，対象外の領域へ吸収しない
		return "構造差", ""
	# 保護本文・境界と外側の構造の一致の返戻
	return "", ""

def golden_semantic_check(keys: list[str]) -> tuple[int, list[str], list[str]]:
	"""手で定めた全ての golden の候補に，構造・値・保護内容の保持契約を照合する
	入力と期待を別々に解析し，期待ファイルを書き換えない
	計算量：対象数 F・最大解析比較費用 P に対して O(FP)
	引数：
		keys: 対象の言語の一覧．全言語なら JSON 等の非ドキュメント言語も含む
	戻値：
		確認した候補数，保持違反，全体の構造照合の対象外の理由
	送出例外：
		OSError: ファイルの読取又はパーサーの起動が失敗する場合
		RuntimeError: パーサーが失敗する場合
		UnicodeDecodeError: 異なるバイト列の候補が UTF-8 でない場合
		ValueError: パーサーの位置表が不正な場合
		json.JSONDecodeError: パーサーの JSON が不正な場合
		subprocess.TimeoutExpired: パーサーが制限時間内に終了しない場合
	"""
	found: list[str] = []
	excluded: list[str] = []
	count: int = 0
	for path in sorted((ROOT / "test_data/golden").glob("*/*.in")):
		lang: str = { "javascriptreact": "javascript", "typescriptreact": "tsx" }.get(path.parent.name, path.parent.name)
		if set(keys) != set(LANGS) and lang not in keys:
			continue
		if not path.with_suffix(".out").is_file() and not path.with_suffix(".fail").is_file():
			count += 1
			found.append(str(path.relative_to(ROOT)) + ": 期待ファイルが無い")
			continue
		source: bytes = path.read_bytes()
		candidate: bytes = (
			path.with_suffix(".out").read_bytes() if
			path.with_suffix(".out").is_file() else
			source if path.with_suffix(".fail").is_file() else b""
		)
		count += 1
		label: str = str(path.relative_to(ROOT))
		marker: str = next((suffix for suffix in (".fail", ".skip", ".partial") if path.with_suffix(suffix).is_file()), "")
		if marker in (".fail", ".skip"):
			if source != candidate:
				found.append(label + ": 見送・失敗の期待が原文を変更して居る")
			excluded.append(label + ": 原文保持の " + marker + " — " + path.with_suffix(marker).read_text().strip())
			continue
		if source == candidate:
			if marker == ".partial":
				print("[成功] 部分整形の原文全体のバイト一致: " + label)
			continue
		ext: str = LANGS.get(lang, (lang, "json" if lang == "jsonc" else "cpp" if lang == "h" else lang))[1]
		text: str = source.decode("utf-16" if source.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig")
		output: str = candidate.decode("utf-16" if candidate.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig")
		before: list[Node] = parse_dump(dump_ast(text, ext), text)
		after: list[Node] = parse_dump(dump_ast(output, ext), output)
		if marker == ".partial":
			problem, reason = partial_structure(lang, before, after)
			if problem:
				found.append(label + ": " + problem)
			elif reason:
				excluded.append(label + ": 部分整形の未確認 — " + reason)
			else:
				print("[成功] 部分整形の本文・順序・境界・外側の構造: " + label)
		elif problem := classify(lang, before, after):
			if problem.startswith((UNCONFIRMED_AUXILIARY, UNCONFIRMED_AUXILIARY_VERSION)):
				excluded.append(label + ": " + problem)
				continue
			detail: str = type_script_difference(lang, before, after)
			if problem == "構造差":
				detail += ("; " if detail else "") + structure_difference(lang, before, after)
			found.append(label + ": " + problem + (" — " + detail if detail else ""))
	# 候補数と独立した保持違反及び対象外の理由の返戻
	return count, found, excluded

def macro_cache_check() -> list[str]:
	"""無関係な定義・取消・条件枝を挟む転送列の観測と線形の作業量を照合する
	戻値：
		条文から定めた観測と作業量の不一致
	送出例外：
		OSError: パーサーの起動が失敗する場合
		RuntimeError: パーサーが失敗する場合
		ValueError: パーサーの出力が不正な場合
	"""
	found: list[str] = []
	previous: int = 0
	for count in 16, 32, 64, 128:
		parts: list[str] = ["#define M0(x) #x\n"]
		parts.extend(f"#define M{idx}(x) M{idx - 1}(x)\n" for idx in range(1, count))
		for idx in range(count):
			parts.append(
				f"#define UNUSED{idx} 1\n#undef UNUSED{idx}\n#if A\n#define OTHER{idx} 2\n"
				f"#else\n#define OTHER{idx} 3\n#endif\nconst char*s{idx}=M{count - 1}(a + b);\n"
			)
		text: str = "".join(parts)
		roots: list[Node] = parse_dump(dump_ast(text, "cpp"), text)
		work: list[int] = []
		observed: tuple[str, ...] = source_observation_signature(roots, "cpp", work)
		if has_error(roots) or observed != ("a + b",) * count or work != [count, count, 0]:
			found.append(f"無関係なマクロ変更で観測又は作業量が変わる：{count}, {work}")
		if previous and sum(work) > 3 * previous:
			found.append(f"マクロの作業量が倍増時に３倍を超える：{count}")
		previous = sum(work)
	# 循環で仮引数の束縛が入れ替わる場合も，増えた観測だけを合流する
	macros: MacroState = { "A": frozenset({ (("x", "y"), "#x B(y,x)") }), "B": frozenset({ (("x", "y"), "A(x,y)") }) }
	cache: MacroCache = MacroCache()
	if cache.lookup(macros, "A", 2) != { (0,), (1,) } or cache.lookup(macros, "B", 2) != { (0,), (1,) }:
		found.append("循環する転送の異なる束縛の観測を失った")
	cache.invalidate("A")
	macros["A"] = frozenset({ None })
	if cache.lookup(macros, "B", 2):
		found.append("循環する転送の取消を反映しなかった")
	# 不成立の名前も依存先に含み，転送先の定義・取消・再定義と条件枝の合流を反映する
	text = (
		"#define FORWARD(x) TARGET(x)\nconst char*a=FORWARD(one);\n#define TARGET(x) #x\n"
		"const char*b=FORWARD(two);\n#undef TARGET\nconst char*c=FORWARD(three);\n"
		"#if A\n#define TARGET(x) #x\n#else\n#define TARGET(x) x\n#endif\nconst char*d=FORWARD(four);\n"
	)
	roots = parse_dump(dump_ast(text, "cpp"), text)
	if has_error(roots) or source_observation_signature(roots, "cpp") != ("two", "four"):
		found.append("転送先の定義・取消又は条件枝の合流を失った")
	# 観測しない呼出も内側へ進み，実引数の本文を祖先毎に読まない
	for lang in "c", "cpp", "h":
		for definition in "int f(int x);\n", "#define f(x) (x)\n":
			for count in 64, 128, 256, 512:
				text = definition + "int g(){return " + "f(" * count + "0" + ")" * count + ";}\n"
				roots = parse_dump(dump_ast(text, "cpp" if lang == "h" else lang), text)
				argument_work: list[int] = []
				observed = source_observation_signature(roots, lang, argument_work=argument_work)
				if observed or argument_work[0] > len(text) or argument_work[1] > len(text) or argument_work[2]:
					found.append(f"観測しない入子の再走査又は本文の複写：{lang}, {count}, {argument_work}")
	# 外側が非マクロでも内側の文字列化と，テンプレート内の前処理カンマを保持する
	for text, expected in (
		("#define S(x) #x\nint x=f(S(a+b));\n", ("a+b",)),
		("\n  #define S(x) #x\nconst char*s=S(a+b);\n", ("a+b",)),
		("%:define S(x) %:x\nconst char*s=S(%:);\n", ("%:",)),
		("%\\\n:define S(x) %:x\nconst char*s=S(a\\\n+b);\n", ("a+b",)),
		("#define S(...) #__VA_ARGS__\nconst char*s=S(f<A,B>(),g(1,2));\n", ("f<A,B>(),g(1,2)",)),
		("#define S(x) #x\nconst char*s=S(S(a+b));\n", ("S(a+b)", "a+b"))
	):
		roots = parse_dump(dump_ast(text, "cpp"), text)
		if source_observation_signature(roots, "cpp") != expected:
			found.append("入子又は前処理の実引数境界の観測を失った")
	# 観測と作業量の不一致の返戻
	return found

def documentation_self_check(keys: list[str]) -> list[str]:
	"""仕様から書いた候補で，説明・対象・参照・コードの破壊と許可された同期を照合する
	計算量：正誤対数 F・最大の解析比較費用 P に対して O(FP)
	引数：
		keys: 対象の言語の一覧
	戻値：
		仕様と食い違った正誤対
	送出例外：
		OSError: パーサーを起動出来ない場合
		RuntimeError: パーサーが失敗する場合
		ValueError: パーサーの位置表が不正な場合
		json.JSONDecodeError: パーサーの JSON が不正な場合
		subprocess.TimeoutExpired: パーサーが制限時間内に終了しない場合
	"""
	found: list[str] = []
	pairs: list[tuple[str, str, str, str, bool]] = [
		(
			"rust",
			"未展開のトークン列の説明の配置",
			"macro_rules! alpha { () => {}; }\nmacro_rules! beta { () => {\n//! Note\nstruct Box;\n}; }\n",
			"macro_rules! alpha { () => {}; }\n// Note\nmacro_rules! beta { () => { struct Box; }; }\n",
			True
		),
		(
			"rust",
			"未展開のトークン列の説明の欠落",
			"macro_rules! alpha { () => {}; }\nmacro_rules! beta { () => {\n//! Note\nstruct Box;\n}; }\n",
			"macro_rules! alpha { () => {}; }\nmacro_rules! beta { () => { struct Box; }; }\n",
			False
		),
		(
			"rust",
			"マクロ呼出のラッパーと行末の説明の配置",
			"m!(\n/// Note\nalpha);\nm!{alpha; // Tail\n}\n",
			"// Note\nm!(alpha);\nm!{alpha; } // Tail\n",
			True
		),
		("rust", "外側の記法の変換", "/// Summary\nfn f() {}", "/**\n * Summary\n */\nfn f() {}", True),
		("rust", "内側の記法の変換", "//! Summary\nstruct S;", "/*!\n * Summary\n */\nstruct S;", True),
		("rust", "説明の欠落", "/// Summary\nfn f() {}", "fn f() {}", False),
		("rust", "説明の複製", "/// Summary\nfn f() {}", "/** Summary\n * Summary\n */\nfn f() {}", False),
		("rust", "内側から外側への変更", "mod m {\n//! Summary\nfn f() {}\n}", "/** Summary */\nmod m {\nfn f() {}\n}", False),
		("rust", "空の本体の内側の行の配置", "mod m {\n//! Summary\n}\n", "// Summary\nmod m {}\n", True),
		("rust", "空の本体の内側の囲みの配置", "mod m {\n/*! Summary */\n}\n", "// Summary\nmod m {}\n", True),
		("rust", "空の本体の内側の説明の欠落", "mod m {\n//! Summary\n}\n", "mod m {}\n", False),
		("rust", "空の本体の別の定義への直付", "mod m {\n//! Summary\n}\n", "/** Summary */\nmod m {}\n", False),
		(
			"rust",
			"根の内側の記法の変換",
			"//! Summary\n#![deny(missing_docs)]\nstruct S;",
			"/*! Summary */\n#![deny(missing_docs)]\nstruct S;",
			True
		),
		("rust", "見出の中の説明の統合", "struct S\n/** Summary */\n{ value:i32 }", "/** Summary */\nstruct S { value:i32 }", True),
		("rust", "見出の中の説明の欠落", "struct S\n/** Summary */\n{ value:i32 }", "struct S { value:i32 }", False),
		("rust", "列挙子の通常コメントへの配置", "enum E {\n/// Red\nRed, Blue\n}", "// Red\nenum E { Red, Blue }", True),
		("rust", "列挙子の説明の欠落", "enum E {\n/// Red\nRed, Blue\n}", "enum E { Red, Blue }", False),
		(
			"rust",
			"不足項目の骨格",
			"/// Summary\nfn f(x:i32)->i32{x}",
			"/** Summary\n * # Arguments\n * * `x`\n * # Returns\n */\nfn f(x:i32)->i32{x}",
			True
		),
		(
			"cpp",
			"説明の改変",
			"/** Summary\n * @param x Value\n * @return Result\n */\nint f(int x){return x;}",
			"/** Changed\n * @param x Value\n * @return Result\n */\nint f(int x){return x;}",
			False
		),
		(
			"cpp",
			"説明の複製",
			"/** Summary\n * @param x Value\n * @return Result\n */\nint f(int x){return x;}",
			"/** Summary\n * @param x Value\n * @return Result\n */\n/** Summary\n * @param x Value\n * @return Result\n */\nint f(int x){return x;}",
			False
		),
		(
			"csharp",
			"説明の改変",
			"class A {\n/// <summary>Summary</summary>\n/// <param name=\"x\">Value</param>\n/// <returns>Result</returns>\nint f(int x){return x;}\n}",
			"class A {\n/// <summary>Changed</summary>\n/// <param name=\"x\">Value</param>\n/// <returns>Result</returns>\nint f(int x){return x;}\n}",
			False
		),
		("go", "説明の削除", "package p\n// Summary\nfunc f(x int) int {return x}", "package p\nfunc f(x int) int {return x}", False),
		(
			"swift",
			"説明の改変",
			"/** Summary\n * - Parameter x: Value\n * - Returns: Result\n */\nfunc f(x: Int) -> Int {return x}",
			"/** Changed\n * - Parameter x: Value\n * - Returns: Result\n */\nfunc f(x: Int) -> Int {return x}",
			False
		),
		(
			"php",
			"説明の改変",
			"<?php\n/** Summary\n * @param int $x Value\n * @return int Result\n */\nfunction f($x){return $x;}",
			"<?php\n/** Changed\n * @param int $x Value\n * @return int Result\n */\nfunction f($x){return $x;}",
			False
		),
		(
			"ruby",
			"説明の改変",
			"# Summary\n# @param x Value\n# @return Result\ndef f(x); return x; end",
			"# Changed\n# @param x Value\n# @return Result\ndef f(x); return x; end",
			False
		),
		(
			"cpp",
			"登録形を跨ぐ仮引数の区間越の移動",
			"/** First\n * @param a A value\n * @param b B value\n * @return Result\n */\n//line 1 file.cpp\n/** Second\n * @param c C value\n */\nint f(int a,int b,int c){return a+b+c;}",
			"/** First\n * @param a A value\n * @param b B value\n * @param c C value\n * @return Result\n */\n//line 1 file.cpp\n/** Second\n */\nint f(int a,int b,int c){return a+b+c;}",
			False
		),
		(
			"cpp",
			"登録形を跨ぐ戻値の区間越の移動",
			"/** First\n * @param a A value\n * @param b B value\n * @return Result\n */\n//line 1 file.cpp\n/** Second\n * @param c C value\n */\nint f(int a,int b,int c){return a+b+c;}",
			"/** First\n * @param a A value\n * @param b B value\n */\n//line 1 file.cpp\n/** Second\n * @param c C value\n * @return Result\n */\nint f(int a,int b,int c){return a+b+c;}",
			False
		),
		(
			"cpp",
			"登録形を跨ぐ候補の項目順の破壊",
			"/** First\n * @param a A value\n * @param b B value\n * @return Result\n */\n//line 1 file.cpp\n/** Second\n * @param c C value\n */\nint f(int a,int b,int c){return a+b+c;}",
			"/** First\n * @param b B value\n * @param a A value\n * @return Result\n */\n//line 1 file.cpp\n/** Second\n * @param c C value\n */\nint f(int a,int b,int c){return a+b+c;}",
			False
		),
		(
			"cpp",
			"登録形を跨ぐ保持記録の削除",
			"/** First\n * @param a A value\n * @param b B value\n * @return Result\n */\n//line 1 file.cpp\n/** Second\n * @param c C value\n */\nint f(int a,int b,int c){return a+b+c;}",
			"/** First\n * @param a A value\n * @param b B value\n * @return Result\n */\n/** Second\n * @param c C value\n */\nint f(int a,int b,int c){return a+b+c;}",
			False
		),
		(
			"cpp",
			"登録形を跨ぐ保持記録の追加",
			"/** First\n * @param a A value\n * @param b B value\n * @return Result\n */\n//line 1 file.cpp\n/** Second\n * @param c C value\n */\nint f(int a,int b,int c){return a+b+c;}",
			"/** First\n * @param a A value\n * @param b B value\n * @return Result\n */\n//line 1 file.cpp\n//line 1 file.cpp\n/** Second\n * @param c C value\n */\nint f(int a,int b,int c){return a+b+c;}",
			False
		),
		(
			"cpp",
			"登録形を跨ぐ説明の欠落",
			"/** First\n * @param a A value\n * @param b B value\n * @return Result\n */\n//line 1 file.cpp\n/** Second\n * @param c C value\n */\nint f(int a,int b,int c){return a+b+c;}",
			"/** First\n * @param a A value\n * @param b B value\n * @return Result\n */\n//line 1 file.cpp\n/** Second\n * @param c \n */\nint f(int a,int b,int c){return a+b+c;}",
			False
		),
		(
			"cpp",
			"統合した本文の変更",
			"/** First\n * @param a A value\n * @param b B value\n * @return Result\n */\n/** ========== Y ========== */\n/** Second\n * @param c C value\n */\nint f(int a,int b,int c){return a+b+c;}",
			"/** First\n * ========== Z ==========\n * Second\n * @param a A value\n * @param b B value\n * @param c C value\n * @return Result\n */\nint f(int a,int b,int c){return a+b+c;}",
			False
		),
		(
			"cpp",
			"統合した本文の欠落",
			"/** First\n * @param a A value\n * @param b B value\n * @return Result\n */\n/** ========== Y ========== */\n/** Second\n * @param c C value\n */\nint f(int a,int b,int c){return a+b+c;}",
			"/** First\n * Second\n * @param a A value\n * @param b B value\n * @param c C value\n * @return Result\n */\nint f(int a,int b,int c){return a+b+c;}",
			False
		),
		(
			"cpp",
			"統合した本文の重複",
			"/** First\n * @param a A value\n * @param b B value\n * @return Result\n */\n/** ========== Y ========== */\n/** Second\n * @param c C value\n */\nint f(int a,int b,int c){return a+b+c;}",
			"/** First\n * ========== Y Y ==========\n * Second\n * @param a A value\n * @param b B value\n * @param c C value\n * @return Result\n */\nint f(int a,int b,int c){return a+b+c;}",
			False
		),
		(
			"cpp",
			"説明順の変更",
			"/** First\n * @param a A value\n * @param b B value\n * @return Result\n */\n/** ========== Y ========== */\n/** Second\n * @param c C value\n */\nint f(int a,int b,int c){return a+b+c;}",
			"/** Second\n * ========== Y ==========\n * First\n * @param a A value\n * @param b B value\n * @param c C value\n * @return Result\n */\nint f(int a,int b,int c){return a+b+c;}",
			False
		),
		(
			"cpp",
			"仮引数の対応の変更",
			"/** First\n * @param a A value\n * @param b B value\n * @return Result\n */\n/** ========== Y ========== */\n/** Second\n * @param c C value\n */\nint f(int a,int b,int c){return a+b+c;}",
			"/** First\n * ========== Y ==========\n * Second\n * @param b A value\n * @param a B value\n * @param c C value\n * @return Result\n */\nint f(int a,int b,int c){return a+b+c;}",
			False
		),
		("go", "インポートの説明の改変", "package p\n// S\nimport \"fmt\"\n", "package p\n// Changed\nimport \"fmt\"\n", False),
		("cpp", "値メンバの説明の削除", "struct X {int a,b;};\nX x={/** S */.a=1,.b=2};", "struct X {int a,b;};\nX x={.a=1,.b=2};", False),
		(
			"cpp",
			"値メンバの説明の移動",
			"struct X {int a,b;};\nX x={/** S */.a=1,.b=2};",
			"struct X {int a,b;};\nX x={.a=1,/** S */.b=2};",
			False
		),
		(
			"java",
			"所有型とメンバの説明の欠落",
			"/** Owner */\nclass C {\n/** Value */\nint value;\n}",
			"/** Owner */\nclass C { int value; }",
			False
		),
		(
			"java",
			"所有型とメンバの説明の複製",
			"/** Owner */\nclass C {\n/** Value */\nint value;\n}",
			"/** Owner */\n// Value\n// Value\nclass C { int value; }",
			False
		),
		(
			"java",
			"所有型とメンバの説明の順序",
			"/** Owner */\nclass C {\n/** Value */\nint value;\n}",
			"// Value\n/** Owner */\nclass C { int value; }",
			False
		),
		("cpp", "既存の概要と本体コメントの順序", "/** Summary */\nvoid f(){\n// First\n}", "/** First\nSummary */\nvoid f(){}", False),
		(
			"cpp",
			"タグの対象",
			"/** Summary\n * @param x Value\n * @return Result\n * @see g\n * @code\n * x + 1\n * @endcode\n */\nint f(int x){return x;}\nint h(int x){return x;}",
			"/** Summary\n * @param y Value\n * @return Result\n * @see g\n * @code\n * x + 1\n * @endcode\n */\nint f(int x){return x;}\nint h(int x){return x;}",
			False
		),
		(
			"cpp",
			"参照",
			"/** Summary\n * @param x Value\n * @return Result\n * @see g\n * @code\n * x + 1\n * @endcode\n */\nint f(int x){return x;}\nint h(int x){return x;}",
			"/** Summary\n * @param x Value\n * @return Result\n * @see h\n * @code\n * x + 1\n * @endcode\n */\nint f(int x){return x;}\nint h(int x){return x;}",
			False
		),
		(
			"cpp",
			"コード例",
			"/** Summary\n * @param x Value\n * @return Result\n * @see g\n * @code\n * x + 1\n * @endcode\n */\nint f(int x){return x;}\nint h(int x){return x;}",
			"/** Summary\n * @param x Value\n * @return Result\n * @see g\n * @code\n * x + 2\n * @endcode\n */\nint f(int x){return x;}\nint h(int x){return x;}",
			False
		),
		(
			"cpp",
			"別の定義",
			"/** Summary\n * @param x Value\n * @return Result\n * @see g\n * @code\n * x + 1\n * @endcode\n */\nint f(int x){return x;}\nint h(int x){return x;}",
			"int f(int x){return x;}\n/** Summary\n * @param x Value\n * @return Result\n * @see g\n * @code\n * x + 1\n * @endcode\n */\nint h(int x){return x;}",
			False
		),
		(
			"cpp",
			"同名の別の所有型",
			"/** Summary\n * @param x Value\n * @return Result\n * @see g\n * @code\n * x + 1\n * @endcode\n */\nint f(int x){return x;}\nint h(int x){return x;}",
			"class A {/** Summary\n * @param x Value\n * @return Result\n * @see g\n * @code\n * x + 1\n * @endcode\n */\nint f(int x){return x;}\nint h(int x){return x;}}; class B {int f(int x){return x;}};",
			False
		)
	]
	for lang, label, source, candidate, should_preserve in pairs:
		if lang not in keys:
			continue
		before: list[Node] = parse_dump(dump_ast(source, LANGS[lang][1]), source)
		after: list[Node] = parse_dump(dump_ast(candidate, LANGS[lang][1]), candidate)
		if has_error(before) or has_error(after) or documentation_preserved(lang, before, after) != should_preserve:
			found.append(lang + " " + label)
	# 語の区切・読点・型・既定値・XML 参照・コードは削除又は改変を許さない
	for lang, source, candidate, should_preserve in (
		("cpp", "Value is", "Valueis", False),
		("cpp", "a,b", "ab", False),
		("cpp", "a,b", "a, b", True),
		("cpp", "説明（値）", "説明 (値)", True),
		("cpp", "@code\nx + 1.\n\nx + 2\n@endcode", "@code\nx + 1\n\nx + 2\n@endcode", False),
		("cpp", "@code\nx + 1\n\nx + 2\n@endcode", "@code\nx + 1\nx + 2\n@endcode", False),
		("cpp", "```\nx + 1\n```", "```\nx+1\n```", False),
		("javascript", "@param [xs=[1,2]] Values", "@param [xs=[1,3]] Values", False),
		("php", "@param int $x Value", "@param string $x Value", False),
		("csharp", "<summary><see cref=\"A\" /></summary>", "<summary><see cref=\"B\" /></summary>", False),
		("csharp", "<summary><code>x + 1.</code></summary>", "<summary><code>x + 1</code></summary>", False),
		(
			"csharp",
			"<summary>S</summary><param name=\"x\" data=\"/>\">Value</param>",
			"<summary>S</summary><param name=\"x\" data=\"/>\"></param>",
			False
		),
		(
			"csharp",
			"<summary>S</summary><param name=\"&#x78;\">Value</param>",
			"<summary>S</summary><param name=\"x\">Value</param>",
			True
		),
		(
			"csharp",
			"<summary>S</summary><param name=\"x\" data=\"/>\">Value</param>",
			"<summary>S</summary><param name='x' data='/> '>Value</param>",
			False
		)
	):
		if (doc_items(lang, source) == doc_items(lang, candidate)) != should_preserve:
			found.append(lang + " 読取の正誤対：" + candidate)
	# 全宣言・全祖先の反復を，時間に依存しない実際の作業数で検出する
	if "cpp" in keys:
		for work_size in 16, 32, 64, 128:
			work_source: str = "".join(f"/** S{num} */\nint f{num}(int x){{return x;}}\n" for num in range(work_size))
			work_roots: list[Node] = parse_dump(dump_ast(work_source, "cpp"), work_source)
			work_index: DocIndex = DocIndex("cpp", work_roots)
			work_keys: set[str] = set()
			work_records: dict[str, list[DocItem]] = doc_records("cpp", work_roots, True, work_keys, work_index)
			if (
				len(work_records) != work_size or work_index.work["range_queries"] != work_size or
				work_index.work["definition_queries"] != work_size or work_index.work["owner_links"] != work_size
			):
				found.append("平坦なドキュメント列の作業数又は保持数：" + str(work_size))
	if "javascript" in keys:
		for depth in 16, 32, 64, 128:
			deep_source: str = "".join(f"function f{num}(){{" for num in range(depth)) + "}" * depth
			deep_roots: list[Node] = parse_dump(dump_ast(deep_source, "js"), deep_source)
			deep_index: DocIndex = DocIndex("javascript", deep_roots)
			if len(deep_index.owners) != deep_index.work["nodes"]:
				found.append("コメント無しの木の所有者・配置枠の作業：" + str(depth))
			linked_source: str = "/** S */\n" + deep_source
			linked_roots: list[Node] = parse_dump(dump_ast(linked_source, "js"), linked_source)
			linked_index: DocIndex = DocIndex("javascript", linked_roots)
			if len(doc_records("javascript", linked_roots, index=linked_index)) != 1 or linked_index.work["owner_links"] != depth:
				found.append("直接の親だけを共有する所有者：" + str(depth))
			quoted_source: str = "const x=\"" + "/*a" * depth + "\";"
			quoted_roots: list[Node] = parse_dump(dump_ast(quoted_source, "js"), quoted_source)
			quoted_index: DocIndex = DocIndex("javascript", quoted_roots)
			if has_error(quoted_roots) or quoted_index.comments or quoted_index.work["block_searches"] != 2:
				found.append("文字列内の閉じの無いコメント記号の反復探索：" + str(depth))
		# 幅間の再利用でも，１つ前の候補の観測を入力へ取り込まない
		cache_source: str = "/** Summary */\nfunction f(){}"
		cache_input: list[Node] = parse_dump(dump_ast(cache_source, "js"), cache_source)
		cache_index: DocIndex = DocIndex("javascript", cache_input)
		for cache_candidate, should_preserve_cache in (
			(cache_source.replace("Summary", "Summary."), True),
			(cache_source.replace("Summary", "Changed"), False),
			(cache_source, True)
		):
			cache_after: list[Node] = parse_dump(dump_ast(cache_candidate, "js"), cache_candidate)
			if documentation_preserved("javascript", cache_input, cache_after, cache_index) != should_preserve_cache:
				found.append("幅間で共有する入力のドキュメント観測")
		if cache_index.work["definition_queries"] != 1:
			found.append("幅毎に入力側のドキュメントを読み直した")
	# 独立した反例と正常例の食違の返戻
	return found

SELF_CHECK_PAIRS.extend(
	[
		("go", "文字と UTF8 のバイト", "package p\nvar x=\"\\xC3\\xA9\"", "package p\nvar x=\"Ã©\"", False),
		("php", "文字列の改行のバックスラッシュ", "<?php $x=\"a\\\nb\";", "<?php $x=\"ab\";", False),
		("typescript", "static メンバの区切", "class C{static;\nx=1}", "class C{static x=1;}", False)
	]
)

def json_self_check() -> list[str]:
	"""コメントと末尾区切の選択表現を，値・重複キー・配列要素の変更から区別する
	計算量：固定数の小入力の解析比較費用 P に対して O(P)
	戻値：独立した期待との食違
	"""
	pairs: list[tuple[str, str, bool]] = [
		("{\"a\":1,/* c */}", "{\"a\":1}", True),
		("[1,2,]", "[1,2]", True),
		("{\"a\":1,\"a\":2}", "{\"a\":2}", False),
		("{\"a\":1,\"b\":2}", "{\"b\":2,\"a\":1}", False),
		("[1,2]", "[1,3]", False),
		("[1,,2]", "[1,2]", False),
		("[\"/*c*/\"]", "[\"\"]", False),
		("[\"a\",]", "[\"a\",2]", False)
	]
	found: list[str] = []
	for src, output, should_match in pairs:
		before: list[Node] = parse_dump(dump_ast(src, "json"), src)
		after: list[Node] = parse_dump(dump_ast(output, "json"), output)
		result: str = classify("json", before, after)
		if (not result) != should_match:
			found.append("JSON の値・順・区切の照合: " + src)
	# 独立した正例・反例の食違の返戻
	return found

def ruby_binding_self_check() -> list[str]:
	"""平坦な並びの導入照会が接頭部を再走査しない事を作業数で確認する
	計算量：最大テストケースのノード数 N に対して O(N)
	戻値：導入の作業数又はスコープの対応が不正な条件
	送出例外：RuntimeError: 構文解析が失敗した場合
	"""
	found: list[str] = []
	previous: int = 0
	for count in 64, 128, 256, 512:
		source: str = "def f\nx=1\n" + "x +1\n" * count + "end\n"
		root: Node = parse_dump(dump_ast(source, "rb"), source)[0]
		ancestors: Ancestors = Ancestors()
		facts: TreeFacts = TreeFacts([root])
		ancestors.facts: TreeFacts|None = facts
		evaluate(normalize_work(root, "ruby", ancestors))
		work: int = facts.ruby_work
		if not work or work > len(facts.named) + count or previous and work > previous << 1:
			found.append(f"Ruby の導入照会の作業数: N={count}, work={work}")
		previous = work
	# 処理結果の返戻
	return found

def partial_self_check() -> list[str]:
	"""保護領域と外側の構造の独立した正例・反例を照合する
	計算量：固定数の入力の解析費用 P に対して O(P)
	戻値：本文・順序・境界・外側の照合が期待と異なる項目
	"""
	pairs: list[tuple[str, str, str, bool]] = [
		(
			"c",
			"#ifdef __cplusplus\nextern \"C\"{\n#endif\nint MODE alpha(int count);\n#ifdef __cplusplus\n}\n#endif\n",
			"#ifdef __cplusplus\nextern \"C\" {\n#endif\nint MODE alpha(int count);\n#ifdef __cplusplus\n}\n#endif\n",
			True
		),
		(
			"c",
			"#ifdef __cplusplus\nextern \"C\"{\n#endif\nint MODE alpha(int count);\n#ifdef __cplusplus\n}\n#endif\n",
			"#ifdef __cplusplus\nextern \"C++\"{\n#endif\nint MODE alpha(int count);\n#ifdef __cplusplus\n}\n#endif\n",
			False
		),
		(
			"c",
			"#ifdef __cplusplus\nextern \"C\"{\n#endif\nint MODE alpha(int count);\n#ifdef __cplusplus\n}\n#endif\n",
			"#ifdef __cplusplus\nextern \"C\"{\n#endif\nint OTHER alpha(int count);\n#ifdef __cplusplus\n}\n#endif\n",
			False
		),
		("python", "x=1\nbad=???\ny=2\n", "x = 1\nbad=???\ny = 2\n", True),
		("python", "x=1\nbad=???\ny=2\n", "x = 3\nbad=???\ny = 2\n", False),
		("python", "x=1\nbad=???\ny=2\n", "x=1\nbad=!!!\ny=2\n", False),
		("python", "x=1\na=???\ny=2\nb=!!!\nz=3\n", "x=1\na=!!!\ny=2\nb=???\nz=3\n", False),
		("python", "x=1\nbad=???\ny=2\n", "x=1\ny=2\nbad=???\n", False),
		("python", "x=1\nbad=???\ny=2\n", "x=1\nbad=???\ny=!!!\n", False),
		("c", "void f(){g();　// c\n　// d\n}", "void f(){g();　 // c\n　 // d\n}", True),
		("c", "void f(){g();　// c\n　// d\n}", "void f(){g();　 // c\n // d\n}", False),
		("c", "void f(){g();　// c\nh();}", "void f(){g();h();　 // c\n}", False),
		("css", ".a /* x */.b{x:y}", "/* x */ .a .b{x:y;}", True),
		("css", ".a /* x */.b{x:y}", ".a.b{x:y;}", False),
		("css", ".a/**/.b{x:y}", ".a .b{x:y;}", False),
		("css", ".a/*\n*/.b{x:y}", ".a.b{x:y;}", True),
		("css", ".a{--x:}", ".a{--x:;}", True),
		("css", ".a{--x: }", ".a{--x: ;}", True),
		("css", ".a{--x:/**/}", ".a{--x:/**/;}", True),
		("css", ".a{--x:}", ".a{--x: ;}", False),
		("css", ".a{--x: }", ".a{--x:;}", False),
		("css", ".a{--x:}", ".a{--x:1;}", False),
		("css", ".a{color:}", ".a{color:;}", False),
		("css", ".a{--x:;broken:???}", ".a{--x:;broken:!!!}", False),
		("css", ".a{--x:} .b{broken:???}", ".a{--x:;} .b{broken:???}", True),
		("scss", "@include f(a\n); .a{x:???;}", "@include f(a); .a{x:???;}", True),
		("scss", "@include f(a\n); .a{x:???;}", "@include f(a); .a{x:!!!;}", False),
		("scss", "@include f($x: \n);", "@include f($y: \n);", False),
		("scss", "@include f($x: \n);", "@include f($x: 1);", False),
		("scss", "@include f(a\n", "@include f(b\n", False),
		("scss", "@include f(a\n); @include g($x: \n);", "@include f(a); @include g($x: \n);", True),
		("scss", "@include f(a\n); @include g($x: \n);", "@include f(a); @include g($y: \n);", False),
		("scss", "@include f(a // hello\n);", "@include f(a // hello);", False),
		("scss", "@include f(a // hello\n);", "@include f(a // changed\n);", False),
		("scss", ".alpha{color:??? // hello\n;}", ".alpha{color:??? // changed\n;}", False),
		("scss", "@include f(\"a\n);", "@include f(\"b\n);", False),
		("scss", "@include f(a\n); .a{x:???;}", ".a{x:???;} @include f(a);", False),
		("scss", ".a{--x:#{$v};}", ".a{--x: #{$v};}", True),
		("scss", ".a{--x:#{$v};}", ".a{--x:#{$w};}", False)
	]
	for language in "typescript", "tsx":
		pairs.extend(
			[
				(language, "type T = keyof readonly A[]; type X = ???;", "type T = keyof readonly A[]; type X = !!!;", False),
				(language, "type T = keyof readonly ;", "type T = keyof readonly A[];", False)
			]
		)
	# 既存の部分整形と同じ正常領域・保護領域を持つ独立した正誤対
	original: str = "def f():\n    x = 1\n    y = = 2\n    return x\n\nprint(f())\n"
	formatted: str = "def f():\n\tx = 1\n\ty = = 2\n\treturn x\n\nprint(f())\n"
	pairs.extend(
		[
			("python", original, formatted, True),
			("python", original, formatted.replace("x = 1", "x = 3"), False),
			("python", original, formatted.replace("y = = 2", "y = = 3"), False)
		]
	)
	found: list[str] = []
	for lang, source, candidate, should_match in pairs:
		before: list[Node] = parse_dump(dump_ast(source, LANGS.get(lang, (lang, lang))[1]), source)
		after: list[Node] = parse_dump(dump_ast(candidate, LANGS.get(lang, (lang, lang))[1]), candidate)
		problem: str
		reason: str
		problem, reason = partial_structure(lang, before, after)
		if (not problem and not reason) != should_match or not should_match and not problem:
			found.append("部分整形の保存条件: " + repr(source))
	text: str = "x=???\n"
	before = parse_dump(dump_ast(text, "py"), text)
	after = parse_dump(dump_ast(text, "py"), text)
	pending: list[Node] = list(after)
	while pending:
		node: Node = pending.pop()
		if node.type == "ERROR":
			node.start: int = node.start + 1
			break
		pending.extend(node.children)
	problem, reason = partial_structure("python", before, after)
	if problem or not reason:
		found.append("同じ原文の解析範囲の差を保持違反と区別出来ない")
	# 未確認と保持違反を区別した照合結果の返戻
	return found

def auxiliary_self_check(excluded: list[str]|None = None) -> list[str]:
	"""処理系の受理範囲と，未確認・不一致の判定を手書の正誤対で照合する
	計算量：正誤対数 F・解析比較費用 P に対して O(FP)
	引数：excluded: 補助解析を確認出来ない正誤対の記録先
	戻値：判定・診断の食違
	"""
	found: list[str] = []
	parser: AuxiliaryParser = AUXILIARY_PARSERS["swift"]
	source: str = "if xs.contains { $0 > 1 } { print(1) }"
	candidate: str = "if (xs.contains { $0 > 1 }) { print(1) }"
	before: list[Node] = parse_dump(dump_ast(source, "swift"), source)
	after: list[Node] = parse_dump(dump_ast(candidate, "swift"), candidate)
	valid: str = "(source_file \"<stdin>\" (integer_literal_expr value=\"1\"))"
	changed: str = "(source_file \"<stdin>\" (integer_literal_expr value=\"2\"))"
	invalid: str = "(source_file \"<stdin>\" (processed_init=error_expr))"
	cases: list[tuple[int, str, int, str, str]] = [
		(1, valid, 0, valid, UNCONFIRMED_AUXILIARY),
		(0, invalid, 0, valid, UNCONFIRMED_AUXILIARY),
		(0, "", 0, valid, UNCONFIRMED_AUXILIARY),
		(0, "(source_file", 0, valid, UNCONFIRMED_AUXILIARY),
		(0, valid, 1, valid, "構造差"),
		(0, valid, 0, invalid, "構造差"),
		(0, valid, 0, changed, "構造差"),
		(0, valid, 0, valid, "")
	]
	for banner, exit_code, expected in (
		("Apple Swift version 5.10 (swiftlang-5.10.0.13)", 0, False),
		("Apple Swift version 6.1.2", 0, True),
		("swift-driver version: 1.127.15 Apple Swift version 6.2.4", 0, True),
		("Apple Swift version 6.3.0", 0, False),
		("unknown", 0, False),
		("Apple Swift version 6.2.4", 1, False)
	):
		auxiliary_version_supported.cache_clear()
		with patch(__name__ + ".auxiliary_version", return_value=subprocess.CompletedProcess([], exit_code, banner, "")):
			if auxiliary_version_supported(parser) != expected:
				found.append("補助解析の版の確認範囲: " + banner)
	auxiliary_version_supported.cache_clear()
	with patch(__name__ + ".auxiliary_version_supported", return_value=False), patch(
		__name__ + ".auxiliary_identity",
		return_value="PATH swift -> /probe; version='5.10'"
	), patch(__name__ + ".auxiliary_parse") as unsupported_parse:
		result: str = classify("swift", before, after)
		if not result.startswith(UNCONFIRMED_AUXILIARY_VERSION) or "5.10" not in result or unsupported_parse.called:
			found.append("未確認の版でダンプを比較した")
	# CI の参照名・判別子を残し，引用の有無と位置属性の各書式を照合する
	for first, second, expected in (
		("discriminator=0.$0@<stdin>:10:16", "discriminator=0.$0@<stdin>:26:12", ""),
		("discriminator=4.$0@<stdin>:72:18", "discriminator=4.$0@<stdin>:131:16", ""),
		("decl=\"main.f@<stdin>:1:2\"", "decl=\"main.f@<stdin>:3:4\"", ""),
		("decl=main.f@/tmp/input.src:1:2", "decl=main.f@/tmp/input.src:3:4", ""),
		("range=[<stdin>:1:2 - line:3:4]", "range=[<stdin>:5:6 - line:7:8]", ""),
		("source_range=[12:34]", "source_range=[56:78]", ""),
		("location=<stdin>:1:2", "location=<stdin>:3:4", ""),
		("byte_offset=12 column=2 line=1 source_offset=12", "byte_offset=34 column=4 line=3 source_offset=34", ""),
		("discriminator=0.$0@<stdin>:10:16", "discriminator=0.$1@<stdin>:26:12", "構造差"),
		("discriminator=0.$0@<stdin>:10:16", "discriminator=1.$0@<stdin>:26:12", "構造差"),
		("decl=\"main.f@<stdin>:1:2\"", "decl=\"main.g@<stdin>:3:4\"", "構造差"),
		("value=\"$0@<stdin>:1:2\"", "value=\"$0@<stdin>:3:4\"", "構造差"),
		("value=\"range=[<stdin>:1:2 - line:3:4]\"", "value=\"range=[<stdin>:5:6 - line:7:8]\"", "構造差"),
		("text=\"byte_offset=12 line=1\"", "text=\"byte_offset=34 line=3\"", "構造差")
	):
		head: str = "(source_file \"<stdin>\" (declref_expr type='<null>' decl=main. .top-level code.explicit closure "
		input_dump: str = head + first + " function_ref=unapplied))"
		output_dump: str = head + second + " function_ref=unapplied))"
		cases.append((0, input_dump, 0, output_dump, expected))
	cases.append((0, valid, 0, valid.replace("integer_literal_expr", "float_literal_expr"), "構造差"))
	# アドレスの綴りでなく，同じダンプ内の参照の関係と実際の値を照合する
	for first, second, expected in (
		("'anonname=0x14b8431d8'", "'anonname=0x13c832a10'", ""),
		("owner=\"0x1234\"", "owner=\"0x5678\"", ""),
		("owner=0x1234 ref=0x1234", "owner=0x5678 ref=0x5678", ""),
		("owner=0x1234 ref=0x5678", "owner=0x9876 ref=0xABCD", ""),
		("owner=0x00AB ref=0Xab", "owner=0x1234 ref=0x1234", ""),
		("decl_context=0x1234 ref=<anonymous @ 0x1234>", "decl_context=0x5678 ref=<anonymous @ 0x5678>", ""),
		("decl=\"<anonymous @ 0x1234>\" ref=0x1234", "decl=\"<anonymous @ 0x5678>\" ref=0x5678", ""),
		("owner=0x1234 ref=0x1234", "owner=0x5678 ref=0x9876", "構造差"),
		("owner=0x1234 ref=0x5678", "owner=0x9876 ref=0x9876", "構造差"),
		("decl_context=0x1234 ref=<anonymous @ 0x1234>", "decl_context=0x5678 ref=<anonymous @ 0x9876>", "構造差"),
		("owner=0x0", "owner=0x1234", "構造差"),
		("'anonname=0x1234'", "'othername=0x5678'", "構造差"),
		("owner=0x1234 discriminator=1", "owner=0x5678 discriminator=2", "構造差"),
		("discriminator=0x1", "discriminator=0x2", "構造差"),
		("value=0x1234", "value=0x5678", "構造差"),
		("text=\"0x1234\"", "text=\"0x5678\"", "構造差"),
		("value=\"owner=0x1234\"", "value=\"owner=0x5678\"", "構造差"),
		("value=\"<anonymous @ 0x1234>\"", "value=\"<anonymous @ 0x5678>\"", "構造差"),
		("\"0x1234\"", "\"0x5678\"", "構造差"),
		("value=0x1p2", "value=0x1p3", "構造差")
	):
		cases.append((0, f"(source_file (parameter {first}))", 0, f"(source_file (parameter {second}))", expected))
	# 除去される括弧と並替前の宣言が，残る参照の番号をずらさない
	cases.append(
		(
			0,
			"(source_file (paren_expr owner=0x1234 (parameter owner=0x5678 ref=0x5678)))",
			0,
			"(source_file (parameter owner=0x9876 ref=0x9876))",
			""
		)
	)
	cases.append(
		(
			0,
			"(source_file (var_decl owner=0x1234) (parameter owner=0x5678 ref=0x1234))",
			0,
			"(source_file (parameter owner=0x9876 ref=0xABCD) (var_decl owner=0xABCD))",
			""
		)
	)
	cases.append((0, "(source_file (parameter value=\"address_0\"))", 0, "(source_file (parameter value=\"address_1\"))", "構造差"))
	for input_exit, input_dump, output_exit, output_dump, expected in cases:
		results: list[subprocess.CompletedProcess[str]] = [
			subprocess.CompletedProcess([], input_exit, input_dump, "input diagnostic"),
			subprocess.CompletedProcess([], output_exit, output_dump, "output diagnostic")
		]
		auxiliary_structure.cache_clear()
		with patch(
			__name__ + ".auxiliary_parse",
			side_effect=lambda registration, text: results[0] if text == source else results[1]
		) as parse_call, patch(
			__name__ + ".auxiliary_identity",
			return_value="PATH probe -> /probe; version='1'"
		), patch(__name__ + ".auxiliary_version_supported", return_value=True):
			result: str = classify("swift", before, after)
		if result.split(" — ", 1)[0] != expected:
			found.append(f"補助解析の分類: {input_exit}, {input_dump!r}, {output_exit}, {output_dump!r}: {result}")
		if expected == UNCONFIRMED_AUXILIARY and (
			not all(word in result for word in ("PATH", "version=", f"exit={input_exit}", "input diagnostic")) or
			any(call.args[1] != source for call in parse_call.call_args_list)
		):
			found.append("補助解析の未確認に環境と診断が無い")
	# 言語名によらない入力の未受理と，候補を読まない事の確認
	auxiliary_structure.cache_clear()
	with patch(__name__ + ".auxiliary_version_supported", return_value=True), patch(
		__name__ + ".auxiliary_parse",
		return_value=None
	), patch(__name__ + ".auxiliary_identity", return_value="PATH probe: 未発見"):
		if not auxiliary_comparison(parser, before, after).startswith(UNCONFIRMED_AUXILIARY):
			found.append("補助処理系の未発見が未確認でない")
	auxiliary_structure.cache_clear()
	# 文法のエラー領域を無視せず，公式の構文と独立した本文照合で確定する
	for source, candidate, expected in (
		(
			"func alpha(_ flag:Bool){if flag{print(1)} /* Boundary */ ;print(2)}",
			"func alpha(_ flag:Bool){if flag{print(1)}\n// Boundary\nprint(2)\nreturn}",
			True
		),
		(
			"func alpha(_ flag:Bool){if flag{print(1)} /* Boundary */ ;print(2)}",
			"func alpha(_ flag:Bool){if flag{print(1);print(2)}\n// Boundary\nreturn}",
			False
		)
	):
		before = parse_dump(dump_ast(source, "swift"), source)
		after = parse_dump(dump_ast(candidate, "swift"), candidate)
		result = classify("swift", before, after)
		if result.startswith((UNCONFIRMED_AUXILIARY, UNCONFIRMED_AUXILIARY_VERSION)) and excluded is not None:
			excluded.append("swift 文境界の正誤対: " + result)
		elif (not result) != expected:
			found.append("公式の構文で確定したコメント付の文境界の照合")
	# 正誤対の食違の返戻
	return found

def transform_context_self_check() -> list[str]:
	"""入子の本体を辿り直さず，合法な返戻補完と末端の値の破壊を区別する
	計算量：最大ノード数 N・解析比較の費用 P に対して O(N+P)
	戻値：文脈の結果又は線形の訪問数を満たさない条件
	"""
	found: list[str] = []
	previous: int = 0
	for size in 16, 32, 64, 128:
		head: str = "".join(f"function f{idx}(){{" for idx in range(size))
		source: str = head + "ping(1);" + "}" * size
		candidate: str = head + "ping(1);" + "return;}" * size
		before: list[Node] = parse_dump(dump_ast(source, "js"), source)
		after: list[Node] = parse_dump(dump_ast(candidate, "js"), candidate)
		roots: tuple[NormNode, ...] = tuple(part for root in before for part in normalize(root, "javascript"))
		index: TransformIndex = TransformIndex(roots)
		functions: list[NormNode] = [node for node in transform_nodes(roots) if node[0] == "function_declaration"]
		if any(transform_context("javascript", index, function, frozenset()) != ("void", frozenset()) for function in functions):
			found.append(f"入子の通常関数の文脈: {size}")
		if index.visits > (size << 4) + 16 or previous and index.visits > previous << 1:
			found.append(f"入子の関数の訪問数が線形でない: {size}, {index.visits}")
		previous = index.visits
		changed: str = candidate.replace("ping(1)", "ping(2)")
		broken: list[Node] = parse_dump(dump_ast(changed, "js"), changed)
		if not allowed_structure("javascript", before, after) or allowed_structure("javascript", before, broken):
			found.append(f"入子の関数の返戻補完又は値の保持: {size}")
	# 文脈と独立した本体の照合結果の返戻
	return found

def range_update_self_check() -> list[str]:
	"""範囲 for の遮蔽を含む増減を，組込と多重定義の副作用から照合する
	計算量：固定数の入力の総長 B と解析比較の費用 P に対して O(B + P)
	戻値：型・スコープ・増減の選択に依る意味の差を見落した条件
	送出例外：
		OSError: パーサーを起動出来ない場合
		RuntimeError: パーサーの実行が失敗する場合
		ValueError: パーサーの出力が不正な場合
		subprocess.TimeoutExpired: パーサーの実行時間の上限を超える場合
	"""
	found: list[str] = []
	head: str = (
		"int Pre=0,Post=0;struct Counter{"
		"void operator++()const{++Pre;}void operator++(int)const{++Post;}"
		"void operator--()const{--Pre;}void operator--(int)const{--Post;}"
		"operator bool()const{return true;}};namespace Ns{Counter x;}"
	)
	cases: list[tuple[str, str, str, bool]] = []
	for declaration in "Counter &x", "auto &[x]", "auto &x":
		if "[x]" in declaration:
			collection: str = "struct Row{Counter x;};Row xs[1];"
		else:
			collection = "Counter xs[1];"
		for initializer in ("", "int seed=0;") if declaration == "Counter &x" else ("",):
			for operator in ("++", "--") if declaration == "Counter &x" else ("++",):
				prefix: str = head + "void F(){int x=0;" + collection + "for(" + initializer + declaration + ":xs)"
				cases.append((f"{declaration}, {initializer!r}, {operator}", prefix + "x" + operator + ";}", prefix + operator + "x;}", False))
	for body, should_preserve in (
		("int xs[1]={0};for(int &x:xs)x++;", True),
		("int x=0;int xs[1]={0};for(int &x:xs)x++;", True),
		("Counter xs[1];for(Counter &x:xs)x++;", False),
		("int x=0;Counter value;{Counter &x=value;x++;}", False),
		("int x=0;{using Ns::x;x++;}", False),
		("int x=0;{using namespace Ns;x++;}", True),
		("int x=0;for(Counter x;true;){x++;break;}", False),
		("int x=0;if(Counter x=Counter())x++;", False),
		("int x=0;while(Counter x=Counter()){x++;break;}", False),
		("int x=0;try{throw Counter();}catch(Counter &x){x++;}", False),
		("int x=0;Counter xs[1];for(Counter &x:xs)x++;x++;", True)
	):
		source: str = head + "void F(){" + body + "}"
		before, _, after = source.rpartition("x++;")
		cases.append((body, source, before + "++x;" + after, should_preserve))
	for label, source, candidate, should_preserve in cases:
		before_roots: list[Node] = parse_dump(dump_ast(source, "cpp"), source)
		after_roots: list[Node] = parse_dump(dump_ast(candidate, "cpp"), candidate)
		if has_error(before_roots) or has_error(after_roots):
			found.append(f"範囲 for の自己検査の構文が不正：{label}")
		elif (not classify("cpp", before_roots, after_roots)) != should_preserve:
			found.append(f"増減の型又はスコープの判定が違う：{label}")
	# 独立した副作用とスコープの照合結果の返戻
	return found

def directive_string_self_check(keys: list[str]) -> list[str]:
	"""指示文の所属と厳格モードの有効性を，文字列の値から独立に照合する
	計算量：正誤対の数 F・１対の解析比較費用 P に対して O(FP)
	引数：keys: 検査する言語キー
	戻値：指示文の有効性又は通常の文字列の同値性を誤判定した条件
	送出例外：
		OSError: パーサーを起動出来ない場合
		RuntimeError: パーサーの実行が失敗する場合
		ValueError: パーサーの出力が不正な場合
		subprocess.TimeoutExpired: パーサーが制限時間内に完了しない場合
	"""
	found: list[str] = []
	contexts: tuple[tuple[str, str], ...] = ("", "probe=1;"), ("function f(){", "probe=1;}")
	spellings: tuple[str, ...] = "\"use\\x20strict\"", "\"use\\u0020strict\"", "\"use str\\x69ct\""
	for key in "javascript", "html":
		if key not in keys:
			continue
		cases: list[tuple[str, str, bool]] = []
		for head, tail in contexts:
			for spelling in spellings if not head else spellings[:1]:
				before: str = head + spelling + ";" + tail
				after: str = head + "\"use strict\";" + tail
				cases.extend(((before, after, False), (after, before, False)))
		for before, after in (
			("\"use strict\";", "'use strict';"),
			("\"use\\x20strict\";", "\"use\\u0020strict\";"),
			("const x=\"use\\x20strict\";", "const x=\"use strict\";"),
			("function f(){return \"use\\x20strict\";}", "function f(){return \"use strict\";}"),
			("0;\"use\\x20strict\";", "0;\"use strict\";"),
			("{\"use\\x20strict\";}", "{\"use strict\";}")
		):
			cases.append((before, after, True))
		for before, after in (
			("(\"use strict\");", "\"use strict\";"),
			("(\"other\");\"use strict\";", "\"other\";\"use strict\";"),
			("function f(){(\"other\");\"use strict\";}", "function f(){\"other\";\"use strict\";}")
		):
			cases.append((before, after, False))
		for before, after, should_preserve in cases:
			if key == "html":
				before, after = "<script>" + before + "</script>", "<script>" + after + "</script>"
			before_roots: list[Node] = parse_dump(dump_ast(before, LANGS[key][1]), before)
			after_roots: list[Node] = parse_dump(dump_ast(after, LANGS[key][1]), after)
			if has_error(before_roots) or has_error(after_roots):
				found.append(f"{key}: 指示文の自己検査の構文が不正：{before!r}")
			elif (not classify(key, before_roots, after_roots)) != should_preserve:
				found.append(f"{key}: 指示文の所属又は有効性の判定が違う：{before!r} / {after!r}")
	# 指示文と通常の文字列を区別した照合結果の返戻
	return found

def self_check(keys: list[str], excluded: list[str]|None = None) -> list[str]:
	"""手書の正誤対で意味比較ツールの誤検出と見逃を返す
	フォーマッターの出力を期待の根拠にしない
	計算量：正誤対の数 F・１対の最大解析比較費用 P に対して O(FP)
	引数：
		keys: 検査する言語キーの一覧
		excluded: 補助解析の未確認の格納先
	戻値：
		条文から定めた判定と食い違う正誤対の一覧
	送出例外：
		OSError: パーサーの起動が失敗する場合
		RuntimeError: パーサーが失敗する場合
		ValueError: パーサーの出力が不正な場合
		json.JSONDecodeError: パーサーのノード値が有効な JSON でない場合
		subprocess.TimeoutExpired: パーサーが制限時間内に完了しない場合
	"""
	# 許可した変換と禁止した意味差の独立した照合
	found: list[str] = dump_traversal_check() + deep_walk_check() if "javascript" in keys else []
	found.extend(documentation_self_check(keys))
	found.extend(auxiliary_self_check(excluded))
	found.extend(directive_string_self_check(keys))
	if "javascript" in keys:
		found.extend(transform_context_self_check())
	if "ruby" in keys:
		found.extend(ruby_binding_self_check())
	if "python" in keys:
		found.extend(partial_self_check())
	if set(keys) == set(LANGS):
		found.extend(json_self_check())
	if "cpp" in keys:
		found.extend(range_update_self_check())
	if "cpp" in keys or "c" in keys:
		found.extend(macro_cache_check())
		found.extend(dump_input_check())
	for key, label, src, candidate, should_preserve in SELF_CHECK_PAIRS:
		if key not in keys and not (key == "h" and ("c" in keys or "cpp" in keys)):
			continue
		before: list[Node] = parse_dump(dump_ast(src, "cpp" if key == "h" else LANGS[key][1]), src)
		after: list[Node] = parse_dump(dump_ast(candidate, "cpp" if key == "h" else LANGS[key][1]), candidate)
		if has_error(repair_comparison_source(key, comparison_roots(key, before))):
			found.append(f"{key} {label}: 自己検査の入力が不正")
		else:
			result: str = classify(key, before, after)
			if result.startswith((UNCONFIRMED_AUXILIARY, UNCONFIRMED_AUXILIARY_VERSION)) and excluded is not None:
				excluded.append(f"{key} {label}: {result}")
				continue
			if (not result) != should_preserve:
				found.append(f"{key} {label}: 意味保存の判定が条文と違う")
	# 自己検査の食違の返戻
	return found

SELF_CHECK_PAIRS.extend(
	[
		("kotlin", "分解した束縛の一部の再代入", "fun f(){var (a,b)=g();b=2;h(a,b)}", "fun f(){val (a,b)=g();b=2;h(a,b)}", False),
		("php", "終了値の削除", "<?php die(1);", "<?php die;", False),
		("javascript", "通常関数の await の演算化", "var await=1;function f(){await\ng();}", "var await=1;function f(){await g();}", False),
		("javascript", "通常関数の識別子の文終端", "function alpha(){await\nbeta();}", "function alpha(){await;beta();}", True),
		(
			"javascript",
			"更新する反復束縛の const",
			"function f(xs){for(let [a,b] of xs){b=1;g(a,b);}}",
			"function f(xs){for(const [a,b] of xs){b=1;g(a,b);}}",
			False
		),
		("javascript", "静的メンバの対象の変更", "class A{static\nx=1;}", "class A{static; x=1;}", False)
	]
)

SELF_CHECK_PAIRS.extend(
	(lang, *pair) for lang in ("javascript", "typescript", "tsx") for pair in (
		("返戻の改行境界", "const alpha=()=>{return\n(1+0);};", "const alpha=()=>{return;1+0;};", True),
		("返戻値への改行境界の取込", "const alpha=()=>{return\n(1+0);};", "const alpha=()=>{return 1+0;};", False),
		("生成の改行境界", "const alpha=function*(){yield\n(2+0);};", "const alpha=function*(){yield;2+0;};", True),
		("生成値への改行境界の取込", "const alpha=function*(){yield\n(2+0);};", "const alpha=function*(){yield 2+0;};", False),
		("非同期の改行継続", "const alpha=async()=>{await\nbeta();};", "const alpha=async()=>{await beta();};", True),
		("非同期の継続を分断", "const alpha=async()=>{await\nbeta();};", "const alpha=async()=>{await;beta();};", False),
		("非同期フィールドとメソッド", "class Box{async\n[\"alpha\"](){return 1;}}", "class Box{\"async\";[\"alpha\"](){return 1;}}", True),
		("非同期フィールドの修飾子化", "class Box{async\n[\"alpha\"](){return 1;}}", "class Box{async [\"alpha\"](){return 1;}}", False),
		("静的フィールドと生成メソッド", "class Box{static async\n*alpha(){yield 1;}}", "class Box{static \"async\";*alpha(){yield 1;}}", True),
		(
			"通常コメントのタグ状の説明の統合",
			"// @param count Note\nfunction alpha(count){}",
			"/**\\@param count Note*/\nfunction alpha(count){}",
			True
		),
		("タグ状の説明の内容変更", "// @param count Note\nfunction alpha(count){}", "/**\\@param count Changed*/\nfunction alpha(count){}", False),
		(
			"タグ状の説明の複製",
			"// @param count Note\nfunction alpha(count){}",
			"/**\\@param count Note\n * \\@param count Note*/\nfunction alpha(count){}",
			False
		),
		(
			"タグ状の説明の所属変更",
			"// @param count Note\nfunction alpha(count){}\nfunction beta(count){}",
			"function alpha(count){}\n/**\\@param count Note*/\nfunction beta(count){}",
			False
		),
		("説明タグの同義形式", "/**@desc Note*/\nfunction alpha(){}", "/**@description Note*/\nfunction alpha(){}", True),
		("説明タグの内容変更", "/**@desc Note*/\nfunction alpha(){}", "/**@description Changed*/\nfunction alpha(){}", False),
		(
			"コード例のエスケープ保持",
			"/**Note\n * ```\n * \\@param count\n * ```\n */\nfunction alpha(){}",
			"/**Note\n * ```\n * @param count\n * ```\n */\nfunction alpha(){}",
			False
		)
	)
)

SELF_CHECK_PAIRS.extend(
	(lang, *pair) for lang in ("typescript", "tsx") for pair in (
		("合併型の先頭区切", "type Alpha=|Box|Other;", "type Alpha=Box|Other;", True),
		("交差型の先頭区切", "type Alpha=&Box&Other;", "type Alpha=Box&Other;", True),
		("１項の合併型の先頭区切", "type Alpha=|Box;", "type Alpha=Box;", True),
		("１項の交差型の先頭区切", "type Alpha=&Box;", "type Alpha=Box;", True),
		("合併型の項の欠落", "type Alpha=|Box|Other;", "type Alpha=Box;", False),
		("交差型のグループ変更", "type Alpha=&Box&(Other&Third);", "type Alpha=Box&Other&Third;", False),
		("前置型の配列のラッパー", "type Alpha=keyof (readonly Box[])|Other;", "type Alpha=keyof readonly Box[]|Other;", True),
		("前置型のタプルのラッパー", "type Alpha=keyof (readonly [Box,Other])|Third;", "type Alpha=keyof readonly [Box,Other]|Third;", True),
		("前置型のジェネリックのラッパー", "type Alpha=keyof (readonly Box<T>[])&Other;", "type Alpha=keyof readonly Box<T>[]&Other;", True),
		("前置型の配列の所属変更", "type Alpha=(keyof readonly Box[])[];", "type Alpha=keyof readonly Box[][];", False),
		("前置型の合併の所属変更", "type Alpha=keyof (readonly Box[]|Other);", "type Alpha=keyof readonly Box[]|Other;", False),
		("前置型のタプルの項の欠落", "type Alpha=keyof readonly [Box,Other];", "type Alpha=keyof readonly [Box];", False),
		(
			"前置型と明示型の不変束縛",
			"export {};let count:string=\"a\";type Alpha=keyof (readonly Box[])|Other;",
			"export {};const count:string=\"a\";type Alpha=keyof readonly Box[]|Other;",
			True
		),
		(
			"前置型と推論型の変化",
			"export {};let count=1;type Alpha=keyof (readonly Box[])|Other;",
			"export {};const count=1;type Alpha=keyof readonly Box[]|Other;",
			False
		),
		("前置型と本当のエラー本文", "type Alpha=keyof readonly Box[];const count=1;", "type Alpha=keyof (readonly Box[]);const count=+;", False)
	)
)

SELF_CHECK_PAIRS.extend(
	[
		("cpp", "コルーチンへの通常返戻", "Task f(){co_await g();}", "Task f(){co_await g();return;}", False),
		("kotlin", "sequence の終端", "fun f()=sequence{yield(1)}", "fun f()=sequence{yield(1);return}", False)
	]
)

SELF_CHECK_PAIRS.extend(
	[
		("kotlin", "改行後の式を返戻値にしない", "fun f(){return\n(x)}", "fun f(){return x}", False),
		("kotlin", "通常の呼出の実引数を残す", "fun f(){g(1)}", "fun f(){g(2)}", False),
		("kotlin", "アノテーションの有値の実引数", "@A(1) class C", "@A class C", False),
		("swift", "二重の通常呼出の保持", "let x=f()()", "let x=f()", False)
	]
)

SELF_CHECK_PAIRS.extend(
	[
		("rust", "後方の不明なマクロへの束縛", "fn f(){let mut x=1;opaque!(x);}", "fn f(){let x=1;opaque!(x);}", False),
		("swift", "末尾クロージャの所属の変更", "let a=f(g()){x in x}", "let a=f(g(){x in x})", False),
		("scss", "カスタム値の補間対象の変更", ".a{--x:#{$v};}", ".a{--x:#{$w};}", False)
	]
)

# 初期化の型と翻訳段階の境界を，独立した正誤対で確認する
SELF_CHECK_PAIRS.extend(
	[
		(
			"typescript",
			"分割した一部の更新",
			"function f(){let {a,b}:{a:number,b:number}={a:1,b:2};b=3;use(a,b);}",
			"function f(){const {a,b}:{a:number,b:number}={a:1,b:2};b=3;use(a,b);}",
			False
		),
		(
			"typescript",
			"分割した一部の捕捉",
			"function f(){let {a,b}:{a:number,b:number}={a:1,b:2};use(()=>b);}",
			"function f(){const {a,b}:{a:number,b:number}={a:1,b:2};use(()=>b);}",
			False
		),
		(
			"typescript",
			"分割した代入先",
			"function f(){let {a,b}:{a:number,b:number}={a:1,b:2};({b}=other);use(a,b);}",
			"function f(){const {a,b}:{a:number,b:number}={a:1,b:2};({b}=other);use(a,b);}",
			False
		),
		(
			"typescript",
			"分割した型の相関",
			"function f(){let {kind,value}:({kind:\"n\",value:number}|{kind:\"s\",value:string})=read();use(kind,value);}",
			"function f(){const {kind,value}:({kind:\"n\",value:number}|{kind:\"s\",value:string})=read();use(kind,value);}",
			False
		),
		(
			"typescript",
			"分割の入子",
			"function f(){let [{a}]:{a:number}[]=[{a:1}];use(a);}",
			"function f(){const [{a}]:{a:number}[]=[{a:1}];use(a);}",
			False
		),
		(
			"typescript",
			"分割の既定値",
			"function f(){let {a=1}:{a?:number}={};use(a);}",
			"function f(){const {a=1}:{a?:number}={};use(a);}",
			False
		),
		(
			"typescript",
			"導入名の重複",
			"function f(){let {a,b}:{a:number,b:number}={a:1,b:2};let a:number=3;use(a,b);}",
			"function f(){const {a,b}:{a:number,b:number}={a:1,b:2};let a:number=3;use(a,b);}",
			False
		),
		("typescript", "関数自体の捕捉", "function f(){let x=()=>1;use(()=>x);}", "function f(){const x=()=>1;use(()=>x);}", False),
		("c", "複数行コメントの脱落", "#if A && /* Check.\n*/ B\nint x;\n#endif\n", "#if A && B\nint x;\n#endif\n", False)
	]
)

SELF_CHECK_PAIRS.extend([("cpp", "別の同文のコメントで脱落を補わない", "// Same\n#define X 1 /* Same */\n", "// Same\n#define X 1\n", False)])

SELF_CHECK_PAIRS.extend(
	[
		(
			"rust",
			"返戻条件 一時値の借用",
			"fn f()->usize{(&String::from(\"x\")).len()}",
			"fn f()->usize{return (&String::from(\"x\")).len();}",
			False
		),
		(
			"rust",
			"返戻条件 ローカルの破棄順",
			"fn f()->i32{let guard=Guard::new();make().value}",
			"fn f()->i32{let guard=Guard::new();return make().value;}",
			False
		),
		("rust", "返戻条件 一時値の比較", "fn f()->bool{String::from(\"a\")==\"a\"}", "fn f()->bool{return String::from(\"a\")==\"a\";}", False),
		("rust", "返戻条件 末尾の未知マクロ", "fn f()->i32{opaque!()}", "fn f()->i32{return opaque!();}", False),
		("rust", "返戻条件 先行する未知マクロ", "fn f(x:i32)->i32{opaque!();x}", "fn f(x:i32)->i32{opaque!();return x;}", False),
		("rust", "返戻条件 関数の未知属性", "#[rewrite]\nfn f(x:i32)->i32{x}", "#[rewrite]\nfn f(x:i32)->i32{return x;}", False),
		(
			"rust",
			"返戻条件 印字実引数の未知マクロ",
			"fn f()->i32{println!(\"{}\",opaque!());1}",
			"fn f()->i32{println!(\"{}\",opaque!());return 1;}",
			False
		),
		(
			"rust",
			"返戻条件 印字実引数の返戻",
			"fn f()->i32{println!(\"{}\",{return 1;});2}",
			"fn f()->i32{println!(\"{}\",{return 1;});return 2;}",
			False
		),
		(
			"rust",
			"返戻条件 標準マクロの遮蔽",
			"macro_rules! println{()=>{return}} fn f()->i32{println!();1}",
			"macro_rules! println{()=>{return}} fn f()->i32{println!();return 1;}",
			False
		),
		("rust", "返戻条件 添字の変更", "fn f(x:[i32;2])->i32{x[0]}", "fn f(x:[i32;2])->i32{return x[1];}", False),
		("rust", "返戻条件 添字の受手の変更", "fn f(x:[i32;1],y:[i32;1])->i32{x[0]}", "fn f(x:[i32;1],y:[i32;1])->i32{return y[0];}", False),
		("rust", "返戻条件 一時値の添字", "fn f()->i32{make()[0]}", "fn f()->i32{return make()[0];}", False),
		("rust", "返戻条件 添字内の未確認の呼出", "fn f(x:[i32;1])->i32{x[index()]}", "fn f(x:[i32;1])->i32{return x[index()];}", False),
		("rust", "返戻条件 添字前の未知マクロ", "fn f(x:[i32;1])->i32{opaque!();x[0]}", "fn f(x:[i32;1])->i32{opaque!();return x[0];}", False),
		(
			"rust",
			"返戻条件 独立した関数の未知マクロ",
			"fn f(x:i32)->i32{fn inner(){opaque!();}x}",
			"fn f(x:i32)->i32{fn inner(){opaque!();}return x;}",
			True
		)
	]
)

SELF_CHECK_PAIRS.extend(
	[
		(
			"rust",
			"マクロの定義は展開でない",
			"fn f(x:i32)->i32{macro_rules! m{()=>{opaque!()}}x}",
			"fn f(x:i32)->i32{macro_rules! m{()=>{opaque!()}}return x;}",
			True
		),
		("rust", "非復帰型へ補完しない", "fn f()->!{loop{}}", "fn f()->!{return loop{};}", False)
	]
)

SELF_CHECK_PAIRS.extend(
	[
		("kotlin", "式本体の添字の変更", "fun f(x:IntArray):Int=(x)[0]", "fun f(x:IntArray):Int=x[1]", False),
		("swift", "暗黙の返戻の添字の変更", "func f(_ x:[Int])->Int{x[0]}", "func f(_ x:[Int])->Int{return x[1]}", False),
		("ruby", "暗黙の返戻の添字の変更", "def f(x);x[0];end", "def f(x);return x[1];end", False)
	]
)

# 導入の構文だけを変え，同じ演算を明示メソッド呼出へ変える不正候補を照合する
SELF_CHECK_PAIRS.extend(
	[
		(
			"ruby",
			"符号と導入：ハッシュパターン +",
			"def a(x);100;end\ncase {a:2}\nin {a:}\nx=a +1\np x\nend\n",
			"def a(x);100;end\ncase {a:2}\nin {a:}\nx=a(+1)\np x\nend\n",
			False
		)
	]
)

SELF_CHECK_PAIRS.extend(
	[
		(
			"ruby",
			"符号付呼出の範囲：受手のフィールド +",
			"def a(x);100;end\nobj=Object.new\ndef obj.a=(x);end\nobj.a=2\nx=a +1\np x\n",
			"def a(x);100;end\nobj=Object.new\ndef obj.a=(x);end\nobj.a=2\nx=a(+1)\np x\n",
			True
		)
	]
)

SELF_CHECK_PAIRS.extend(
	[
		(
			"go",
			"反復の束縛：型 switch true",
			"package p\nfunc f()int{x:=0;var value any=false;switch true:=value.(type){case bool:_=true;for true{x++;break}};return x}\n",
			"package p\nfunc f()int{x:=0;var value any=false;switch true:=value.(type){case bool:_=true;for{x++;break}};return x}\n",
			False
		)
	]
)

SELF_CHECK_PAIRS.extend(
	[
		(
			"kotlin",
			"ローカルクラス",
			"fun Probe(){class C{var X:Int=1};val P=C::X;P.set(C(),2)}",
			"fun Probe(){class C{val X:Int=1};val P=C::X;P.set(C(),2)}",
			False
		),
		(
			"kotlin",
			"匿名オブジェクト",
			"fun Probe(){val C=object{var X:Int=1};val P=C::X;P.set(2)}",
			"fun Probe(){val C=object{val X:Int=1};val P=C::X;P.set(2)}",
			False
		),
		("kotlin", "同じ呼出の空の実引数の省略", "fun Probe(){Make() {Work()}}", "fun Probe(){Make {Work()}}", True),
		(
			"kotlin",
			"getter 内のローカル束縛",
			"fun f(){class C{val X:Int get(){var x=1;return x}}}",
			"fun f(){class C{val X:Int get(){val x=1;return x}}}",
			True
		),
		("kotlin", "通常呼出の保持", "fun Probe(){Make()() {Work()}}", "fun Probe(){Make() {Work()}}", False),
		("swift", "通常呼出の保持", "let Result=Make()() {Work()}", "let Result=Make() {Work()}", False)
	]
)

SELF_CHECK_PAIRS.extend(
	[
		("c", "void と未指定の区別", "int F(void);", "int F();", False),
		("cpp", "自由関数の宣言", "int F(...);", "int F();", False),
		("cpp", "自由関数の定義", "int F(...){return 0;}", "int F(){return 0;}", False),
		("cpp", "メンバ関数の宣言", "struct C{int F(...);};", "struct C{int F();};", False),
		("cpp", "メンバ関数の定義", "struct C{int F(...){return 0;}};", "struct C{int F(){return 0;}};", False),
		("cpp", "コンストラクタの宣言", "struct C{C(...);};", "struct C{C();};", False),
		("cpp", "コンストラクタの定義", "struct C{C(...) {}};", "struct C{C() {}};", False),
		("cpp", "関数ポインタの型", "using F=int(*)(...);", "using F=int(*)();", False),
		("cpp", "関数参照の型", "using F=int(&)(...);", "using F=int(&)();", False),
		("cpp", "関数型の typedef", "typedef int F(...);", "typedef int F();", False),
		("cpp", "無修飾のラムダ", "auto F=[](...) {};", "auto F=[]() {};", False),
		("cpp", "修飾付のラムダ", "auto F=[](...) mutable noexcept {};", "auto F=[]() mutable noexcept {};", False),
		("cpp", "合法なラムダの空列の省略", "auto F=[]() {};", "auto F=[] {};", True),
		("cpp", "void と空列の対応", "int F(void);", "int F();", True),
		(
			"csharp",
			"無値の匿名メソッド",
			"delegate void D(int X); class C { void F(){ D d = delegate {}; } }",
			"delegate void D(int X); class C { void F(){ D d = delegate() {}; } }",
			False
		),
		(
			"csharp",
			"有値の匿名メソッド",
			"delegate int D(int X); class C { void F(){ D d = delegate {return 1;}; } }",
			"delegate int D(int X); class C { void F(){ D d = delegate() {return 1;}; } }",
			False
		)
	]
)

# 処理系の未結合の式と任意の区切を，意味を変える対から区別する
SELF_CHECK_PAIRS.extend(
	[
		("swift", "前置演算の単独値の括弧", "let count = -(alpha)", "let count = -alpha", True),
		("swift", "比較と論理演算の冗長括弧", "let count = (alpha <= beta) && flag", "let count = alpha <= beta && flag", True),
		("swift", "シフトとビット演算の冗長括弧", "let count = (alpha << beta) & gamma", "let count = alpha << beta & gamma", True),
		("swift", "乗算の項の冗長括弧", "let count = alpha - (maker(beta) * 0.5)", "let count = alpha - maker(beta) * 0.5", True),
		("swift", "乗算の項の型変換", "let count = (alpha as! Int) * 2", "let count = alpha as! Int * 2", True),
		("swift", "右結合の冗長括弧", "let count = alpha ?? (beta ?? gamma)", "let count = alpha ?? beta ?? gamma", True),
		("swift", "数値の字形と前置演算の結合", "let count = 0xff - (-alpha + beta)", "let count = 0xFF - (-alpha + beta)", True),
		(
			"swift",
			"大きな整数の値の変更",
			"let count = 9223372036854775808 - (-alpha + beta)",
			"let count = 9223372036854775809 - (-alpha + beta)",
			False
		),
		(
			"swift",
			"独自の演算子の結合の変更",
			"infix operator <+> : MultiplicationPrecedence\nlet count = alpha <+> (beta * gamma)",
			"infix operator <+> : MultiplicationPrecedence\nlet count = alpha <+> beta * gamma",
			False
		),
		("swift", "前置演算の対象の変更", "let count = -(alpha + beta)", "let count = -alpha + beta", False),
		("swift", "乗算と加算の結合の変更", "let count = (alpha + beta) * gamma", "let count = alpha + beta * gamma", False),
		("swift", "減算の結合の変更", "let count = alpha - (beta - gamma)", "let count = alpha - beta - gamma", False),
		("swift", "シフトの対象の変更", "let count = alpha << (beta & gamma)", "let count = alpha << beta & gamma", False),
		("swift", "右結合の対象の変更", "let count = (alpha ?? beta) ?? gamma", "let count = alpha ?? beta ?? gamma", False),
		("swift", "後置の受手の変更", "let count = (alpha + beta).value", "let count = alpha + beta.value", False),
		("swift", "明示の Void の末尾返戻", "func alpha()->Void{while flag{break}}", "func alpha()->Void{while flag{break};return}", True),
		("swift", "空のタプル型の末尾返戻", "func alpha()->(){print(1)}", "func alpha()->(){print(1);return}", True),
		("swift", "内部の脱出の追加", "func alpha()->Void{print(1);print(2)}", "func alpha()->Void{print(1);return;print(2)}", False),
		("swift", "返戻値の変更", "func alpha()->Int{1}", "func alpha()->Int{return 2}", False),
		("swift", "クロージャ内の脱出の追加", "func alpha(){run{print(1)}}", "func alpha(){run{print(1);return}}", False),
		(
			"swift",
			"概要の箇条書記号のエスケープ",
			"// - Parameter count: Read\nfunc alpha(_ count:Int){}",
			"/** \\- Parameter count: Read */\nfunc alpha(_ count:Int){}",
			True
		),
		(
			"swift",
			"概要のタグへの誤変換",
			"// - Parameter count: Read\nfunc alpha(_ count:Int){}",
			"/** - Parameter count: Read */\nfunc alpha(_ count:Int){}",
			False
		),
		(
			"swift",
			"概要の本文の変更",
			"// - Parameter count: Read\nfunc alpha(_ count:Int){}",
			"/** \\- Parameter count: Write */\nfunc alpha(_ count:Int){}",
			False
		),
		(
			"swift",
			"コード例のエスケープの変更",
			"/// ~~~swift\n/// \\-alpha\n/// ~~~\nfunc alpha(){}",
			"/**\n * ~~~swift\n * -alpha\n * ~~~\n */\nfunc alpha(){}",
			False
		)
	]
)

# 取消済の名前・不成立の枝の定義と補間の折返を，意味を変える対から区別する
SELF_CHECK_PAIRS.extend(
	[
		(
			"c",
			"取消済の値名と不成立の定義",
			"int alpha(int count){\n#undef count\n#if 0\n#define count 2\n#endif\nreturn (count);}",
			"int alpha(int count){\n#undef count\n#if 0\n#define count 2\n#endif\nreturn count;}",
			True
		),
		(
			"cpp",
			"確定した else の取消",
			"#define count 1\n#if 0\n#define count 2\n#else\n#undef count\n#endif\nint alpha(int count){return (count);}",
			"#define count 1\n#if 0\n#define count 2\n#else\n#undef count\n#endif\nint alpha(int count){return count;}",
			True
		),
		(
			"cpp",
			"未確定の枝の定義の保護",
			"int alpha(int count){\n#if ALPHA\n#define count 2\n#endif\nreturn (count);}",
			"int alpha(int count){\n#if ALPHA\n#define count 2\n#endif\nreturn count;}",
			False
		),
		(
			"cpp",
			"前置の正符号の分離",
			"#include <cstddef>\nint alpha(char count){return + +count;}",
			"#include <cstddef>\nint alpha(char count){return +(+count);}",
			True
		),
		("cpp", "前置の正符号と更新の区別", "int alpha(char count){return + +count;}", "int alpha(char count){return ++count;}", False),
		("cpp", "幅と初期化子の分離", "struct Box{int alpha:(1)=0,beta:(1+1){0};};", "struct Box{int alpha:1=0;int beta:1+1{0};};", True),
		("cpp", "アクセス指定と無名の幅", "class Box{public:int alpha:(1)=0;int :2;};", "class Box{public:int alpha:1=0;int :2;};", True),
		("cpp", "幅の変更の拒否", "struct Box{int alpha:(1)=0;};", "struct Box{int alpha:2=0;};", False),
		("cpp", "初期化子の変更の拒否", "struct Box{int alpha:(1)=0;};", "struct Box{int alpha:1=1;};", False),
		("cpp", "宣言子の欠落の拒否", "struct Box{int alpha:(1)=0,beta:(1)=0;};", "struct Box{int alpha:1=0;};", False),
		("cpp", "概要の同義タグ", "/** @short Alpha */\nvoid alpha(){}", "/** Alpha */\nvoid alpha(){}", True),
		("cpp", "概要の変更の拒否", "/** @short Alpha */\nvoid alpha(){}", "/** Beta */\nvoid alpha(){}", False),
		(
			"cpp",
			"通常の説明のタグ状文字列",
			"// @param count Alpha\nvoid alpha(int count){}",
			"/** \\@param count Alpha */\nvoid alpha(int count){}",
			True
		),
		("cpp", "概要のリテラルの欠落", "// @param count Alpha\nvoid alpha(int count){}", "/** */\nvoid alpha(int count){}", False),
		(
			"csharp",
			"内容の無いコード行",
			"class Box{/** <summary><code>\n\n</code></summary> */void Alpha(){}}",
			"class Box{/** <summary><code>\n</code></summary> */void Alpha(){}}",
			True
		),
		(
			"csharp",
			"コード内容の変更の拒否",
			"class Box{/** <summary><code>alpha</code></summary> */void Alpha(){}}",
			"class Box{/** <summary><code>beta</code></summary> */void Alpha(){}}",
			False
		),
		(
			"java",
			"通常の説明の文字参照",
			"class Box{\n// @param count Alpha\nvoid alpha(int count){}}",
			"class Box{\n/** &#64;param count Alpha */\nvoid alpha(int count){}}",
			True
		),
		(
			"java",
			"文字参照の内容の欠落",
			"class Box{\n// @param count Alpha\nvoid alpha(int count){}}",
			"class Box{\n/** */\nvoid alpha(int count){}}",
			False
		),
		(
			"java",
			"翻訳で作る行と物理行の装飾",
			"class Box{\n/** Summary\\u000a * <pre>alpha.\\u000a * beta.</pre> */\nvoid alpha(){}}",
			"class Box{\n/**\n * Summary\n * <pre>alpha.\\u000a * beta.</pre>\n */\nvoid alpha(){}}",
			True
		),
		(
			"java",
			"翻訳で作るコード行の記号の保持",
			"class Box{\n/** <pre>alpha.\\u000a * beta.</pre> */\nvoid alpha(){}}",
			"class Box{\n/** <pre>alpha.\\u000a beta.</pre> */\nvoid alpha(){}}",
			False
		),
		(
			"go",
			"小文字の接尾辞の通常コメント",
			"package p\nfunc Examplealpha(){return\n// Output: Alpha. Beta.\n}\n",
			"package p\nfunc Examplealpha(){return\n// Output: Alpha\n// Beta\n}\n",
			True
		),
		(
			"go",
			"最後のグループ以外の通常コメント",
			"package p\nfunc Example(){\n// Output: Alpha. Beta.\nreturn\n// Gamma\n}\n",
			"package p\nfunc Example(){\n// Output: Alpha\n// Beta\nreturn\n// Gamma\n}\n",
			True
		),
		(
			"go",
			"出力注記の空行の変更",
			"package p\nfunc Example(){\n// Output: Alpha\n\n}\n",
			"package p\nfunc Example(){\n// Output: Alpha\n}\n",
			True
		),
		(
			"go",
			"有効な出力注記の内容の保護",
			"package p\nfunc Example(){\n// Output: Alpha. Beta.\n}\n",
			"package p\nfunc Example(){\n// Output: Alpha\n// Beta\n}\n",
			False
		),
		("php", "補間の式の折返", "<?php echo \"p{$o->f(alpha,beta)}q\";", "<?php echo \"p{$o->f(\nalpha,\nbeta\n)\n}q\";", True),
		("php", "補間の本文の空白の保護", "<?php echo \"p{$o->f(alpha,beta)}q\";", "<?php echo \"p {$o->f(alpha,beta)}q\";", False),
		("php", "補間の実引数の順序の保護", "<?php echo \"p{$o->f(alpha,beta)}q\";", "<?php echo \"p{$o->f(beta,alpha)}q\";", False),
		("ruby", "ヒアドキュメントの補間の折返", "<<TEXT\np#{alpha+beta}q\nTEXT\n", "<<TEXT\np#{\nalpha+beta\n}q\nTEXT\n", True),
		("ruby", "ヒアドキュメントの本文の空白の保護", "<<TEXT\np#{alpha+beta}q\nTEXT\n", "<<TEXT\np #{alpha+beta}q\nTEXT\n", False),
		("ruby", "ヒアドキュメントの補間の値の保護", "<<TEXT\np#{alpha+beta}q\nTEXT\n", "<<TEXT\np#{alpha-beta}q\nTEXT\n", False)
	]
)

def main() -> int:
	"""独立した正誤対・golden を検証し結果を返す
	計算量：対象テストケース数 F・１テストケースの最大検査時間 T に対して O(FT)

	戻値：
		検証成功なら０，失敗なら１
	送出例外：
		OSError: 参照するファイルの読み書き又は子プロセスの起動が失敗する場合
		RuntimeError: 呼出先の整形又は検証が失敗する場合
		ValueError: 読み込んだ値が呼出先の受け入れる形式でない場合
		json.JSONDecodeError: 読み込んだ内容が有効な JSON でない場合
		subprocess.TimeoutExpired: 呼び出した子プロセスが制限時間内に完了しない場合
	"""
	# 検査環境と対象言語の確認
	if not DUMPAST.is_file():
		print(f"missing binary: {DUMPAST}", file=sys.stderr)
		# バイナリ欠如の異常終了の返戻
		return 1
	filters: set[str] = set(sys.argv[1:])
	if unknown := filters - LANGS.keys():
		print(f"invalid arguments: {sorted(unknown)}", file=sys.stderr)
		# 不正な引数の失敗の返戻
		return 1
	keys: list[str] = [key for key in LANGS if not filters or key in filters]
	for key, parser in AUXILIARY_PARSERS.items():
		if key in keys:
			print("AUXILIARY " + key + ": " + auxiliary_identity(parser))
	# 比較ツールの誤検出・見逃しの確認
	comparison_excluded: list[str] = []
	comparison_failures: list[str] = self_check(keys, comparison_excluded)
	for reason in comparison_excluded:
		print("EXCLUDED semantic self check: " + reason)
	for problem in comparison_failures:
		print(f"FAIL semantic self check: {problem}", file=sys.stderr)
	# 全ての既存の候補で構造・内容・ドキュメントの保存を独立に照合する
	golden_count: int
	golden_failures: list[str]
	golden_excluded: list[str]
	golden_count, golden_failures, golden_excluded = golden_semantic_check(keys)
	for reason in golden_excluded:
		print("EXCLUDED golden structure: " + reason)
	for problem in golden_failures:
		print("FAIL golden semantic: " + problem, file=sys.stderr)
	print("\n=== 検証結果 ===")
	print(f"自己検査の正誤対の対象外 {len(comparison_excluded)} 件")
	print(f"golden {golden_count} 件: {len(golden_failures)} 件の不一致，全体の構造照合の対象外 {len(golden_excluded)} 件")
	print("FAILED" if comparison_failures or golden_failures else "ALL_OK")
	# 失敗が無い場合の `0` の返戻
	return 1 if comparison_failures or golden_failures else 0

if __name__ == "__main__":
	sys.exit(main())
