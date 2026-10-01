#! /usr/bin/env python3
# 整形前後の意味等価の検証
# Shave Format の整形前後で，空白等を除いた構文木の意味構造を比較する
# 構造・演算子・内容の比較で変質を検出するが，型解決や実行時の等価性を証明する物ではない
# 括弧除去に依る構造の変化は頂点ノード型の差として検出する
# 正しい冗長括弧除去（`(a + b)` → `a + b`）は両者から括弧を剥がせば一致し，誤検出しない
# 使い方：
# python3 tools/verify_semantic_preservation.py # 内蔵合成ケース（報告クラス網羅）
# python3 tools/verify_semantic_preservation.py ruby cpp # 言語フィルタ
from __future__ import annotations

import json

from pathlib import Path

import re
import subprocess
import sys

# 検査対象の経路と実行ファイル
ROOT: Path = Path(__file__).resolve().parent.parent
# Windows の実行ファイルは拡張子を持つ
EXE: str = ".exe" if sys.platform == "win32" else ""
FMT: Path = ROOT / f"build/shavefmt{EXE}"
DUMPAST: Path = ROOT / f"build/dumpast{EXE}"
# 無制限・既定幅・行分割を伴う狭幅で意味保存を検査する
WIDTHS: list[str] = ["0", "128", "20"]
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
	"python": ("python", "py")
}

# 報告クラス（括弧付二項式への後置メソッド／添字／メンバ／単項前置／ネスト）を各言語で網羅する合成ケース
# 各検体を単一 return 式へ分け，括弧除去だけを検証する
CASES: list[tuple[str, str, str]] = [
	(
		"c",
		"括弧優先順位・添字・単項",
		"int a(int x, int y) { return (x + y) * 2; }\n"
		"int b(int *p, int x, int y) { return p[(x + y) % 8]; }\n"
		"int d(int x, int y) { return (x + y) > 0 ? -(x - y) : ~(x | y); }\n"
	),
	(
		"cpp",
		"括弧後置メンバ・ネスト",
		"struct P { int x; int g() const; };\n"
		"int e(P a, P b) { return ((a.x + b.x) * a.g()) - b.g(); }\n"
		"int h(int x, int y) { return (x + y) > 0 ? -(x - y) : ~(x | y); }\n"
	),
	(
		"csharp",
		"括弧後置メソッド・ビット",
		"class C {\n"
		"\tint A(int x, int y) { return (x + y) * 2; }\n"
		"\tstring B(int x, int y) { return (x + y).ToString(); }\n"
		"\tint D(int x, int y) { return (x + y) > 0 ? -(x - y) : (x & y); }\n"
		"}\n"
	),
	(
		"java",
		"括弧後置メソッド・キャスト・三項",
		"class C {\n"
		"\tint a(int x, int y) { return (x + y) * 2; }\n"
		"\tint b(int[] p, int x, int y) { return p[(x + y) % p.length]; }\n"
		"\tString c(int x, int y) { return ((Integer) (x + y)).toString(); }\n"
		"\tint d(int x, int y) { return (x + y) > 0 ? -(x - y) : (x ^ y); }\n"
		"}\n"
	),
	(
		"go",
		"括弧優先順位・単項",
		"package p\n"
		"func a(x int, y int) int { return (x + y) * 2 }\n"
		"func b(x int, y int) int { return -(x - y) + (x | y) }\n"
		"func d(p []int, x int, y int) int { return p[(x + y) % 8] }\n"
	),
	(
		"rust",
		"括弧優先順位・メソッド・単項",
		"fn a(x: i32, y: i32) -> i32 { return (x + y) * 2; }\n"
		"fn b(x: i32, y: i32) -> u32 { return (x + y).pow(2); }\n"
		"fn d(x: i32, y: i32) -> i32 { return -(x - y) + (x | y); }\n"
	),
	(
		"kotlin",
		"括弧後置メソッド・三項",
		"fun a(x: Int, y: Int): Int = (x + y) * 2\n"
		"fun b(x: Int, y: Int): String = (x + y).toString()\n"
		"fun d(x: Int, y: Int): Int = if((x + y) > 0) -(x - y) else (x or y)\n"
	),
	("swift", "括弧優先順位・返戻", "func Sum(_ x: Int, _ y: Int) -> Int { return (x + y) * 2 }\n"),
	("php", "括弧優先順位・返戻", "<?php\nfunction Sum($x, $y) { return ($x + $y) * 2; }\n"),
	(
		"javascript",
		"括弧後置メソッド・三項",
		"function a(x, y) { return (x + y) * 2; }\n"
		"function b(x, y) { return (x + y).toString(); }\n"
		"function d(x, y) { return (x + y) > 0 ? -(x - y) : (x | y); }\n"
	),
	(
		"typescript",
		"括弧後置メソッド・型注釈",
		"function a(x: number, y: number): number { return (x + y) * 2; }\n"
		"function b(x: number, y: number): string { return (x + y).toFixed(2); }\n"
		"function d(x: number, y: number): number { return (x + y) > 0 ? -(x - y) : (x | y); }\n"
	),
	("tsx", "括弧後置メソッド・JSX", "const b = (x: number, y: number) => <span title={(x + y).toString()}>{(x - y).toString()}</span>;\n"),
	(
		"ruby",
		"報告ケース（括弧後置メソッド・float）",
		"def a(x, y); return (x + y) * 2; end\n"
		"def b(price); return (price.to_i * 1.1).floor; end\n"
		"def c(x, y); return (x.to_i + y.to_i).to_s; end\n"
		"def d(x, y); return (x - y).abs; end\n"
	),
	(
		"python",
		"括弧後置メソッド・条件式",
		"def a(x, y): return (x + y) * 2\n"
		"def b(x, y): return (x + y).bit_length()\n"
		"def d(x, y): return -(x - y) if (x + y) > 0 else (x | y)\n"
	)
]

# 冗長な括弧の除去（組・型・名前だけの仮引数・空の仮引数と実引数）が構造を変えない事を見る合成ケース
CASES.extend(
	[
		("cpp", "空の仮引数のラムダ", "int k() { return [](){ return 1; }(); }\n"),
		("go", "型の括弧・型の変換", "package p\nvar w *(chan int)\nfunc c(x any) any { return (interface{})(x) }\n"),
		("rust", "型の変換の格", "fn c(x: u8) -> i32 { return (x as i32) + 1; }\n"),
		("kotlin", "空の実引数・主の構築子", "class A()\nfun m(xs: List<Int>) = xs.map() { it }\n"),
		("swift", "型の括弧・クロージャの仮引数", "func T(_ x: (Int)?) -> (Int?)? { return x }\nlet g: (Int, Int) -> Int = { (x, y) in x + y }\n"),
		("javascript", "名前だけの仮引数・空の実引数", "const f = (x) => x;\nconst g = new Map();\n"),
		("typescript", "型の括弧・名前だけの仮引数", "type A = (string);\nconst f = (x) => x;\nconst g = new Map();\n"),
		("ruby", "空の仮引数・実引数", "def e(); return 1; end\ndef g(a); return a.b(); end\n"),
		(
			"python",
			"組・添字・末尾カンマ",
			"def t(a, b): return (a, b)\n"
			"def u(d, a, b): return d[(a, b)]\n"
			"def v(x): return (x, x,)\n"
			"def w(xs):\n\tfor (i) in xs:\n\t\tpass\n\treturn (xs[0], xs[1])[0]\n"
		)
	]
)

class Node:
	"""dumpast の罫線ツリーをパースした生ノード（型・named 区分・テキスト・子）"""

	__slots__ = "kind", "type", "text", "children"
	kind: str
	type: str
	text: str
	children: list[Node]

	def __init__(self, kind: str, type_: str, text: str) -> None:
		"""生ノードを種別 (N/U)・型・テキストで初期化する

		引数：
		    kind: named 区分 (N/U)
		    type_: ノード型
		    text: ノードの原文"""
		# 節点の属性の初期化
		self.kind = kind
		self.type = type_
		self.text = text
		self.children = []
		# 終了
		return

# 行から「マーカの前の接頭辞」「N/U 区分」「型」「テキスト」を抽出する正規表現
_LINE: re.Pattern[str] = re.compile(r"^((?:\t|│\t)*)(?:├─|└─)\[([NU]):")

def parse_dump(dump_text: str) -> list[Node]:
	"""dumpast の全出力を解析し最上位の木群を返す

	計算量：O(n)（n は入力文字数）

	引数：
	    dump_text: dumpast の出力

	戻値：
	    最上位ノードの一覧

	送出例外：
	    ValueError: 出力の行又はノード値が不正な場合"""
	roots: list[Node] = []
	stack: list[tuple[int, Node]] = []
	# 構文木の各行からの節点の復元
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
		node: Node = Node(matched.group(2), node_type, text)
		while stack and stack[-1][0] >= depth:
			stack.pop()
		if stack:
			stack[-1][1].children.append(node)
		else:
			roots.append(node)
		stack.append((depth, node))
	# 木群の返戻
	return roots

def is_paren_wrapper(node_type: str) -> bool:
	"""括弧ラッパノード（parenthesized_expression / _statements 等）か判定する
	計算量：型名の文字数 L に対して O(L)

	引数：
	    node_type: ノード型

	戻値：
	    括弧ラッパなら True"""
	# 括弧ラッパ判定の返戻
	return "parenthe" in node_type

def is_comment(node_type: str) -> bool:
	"""コメントノードか判定する
	計算量：型名の文字数 L に対して O(L)

	引数：
	    node_type: ノード型

	戻値：
	    コメントノードなら True"""
	# コメント判定の返戻
	return "comment" in node_type

# 正規形ノード＝（型，葉のテキスト又は `None`，子のタプル）
NormNode = tuple[str, str | None, tuple["NormNode", ...]]

# 意味に無関係な構造ラッパ（整形が付与・除去・結合する）
# 型を消し名前付子のみ昇格して透過する
# 内側の式（binary 等）は透過対象外なので演算子・結合は保持され，破壊は中身に残って捕捉される
# 波括弧の付与／除去で増減するブロック
# return 引数・メソッド呼出引数のラッパ（暗黙 return の `return E` ↔ `E` を同一視）
# 文末 `;` の有無を吸収
# 空文（孤立した `;`）の削除を吸収
# do / end のループ本体ラッパ（`while c do end` の do 補完を吸収，本体があれば昇格）
TRANSPARENT_WRAPPERS: set[str] = {
	"argument_list",
	"block",
	"compound_statement",
	"do",
	"empty_statement",
	"expression_statement",
	"statement_block"
}

# Python の組の種別（括弧の有無と位置で種別が変わり，`return (a, b)` と `return a, b` は同じ組）
PYTHON_TUPLES: set[str] = { "expression_list", "pattern_list", "tuple", "tuple_pattern" }
# 仮引数の並び（名前だけの仮引数１つは括弧を省ける：`(x) => x` と `x => x`）
PARAMETER_LISTS: set[str] = { "formal_parameters", "inferred_parameters", "parameter_list" }
# 仮引数１つを包む節点（型・既定値の無い名前だけなら名前と同じ）
PARAMETER_WRAPPERS: set[str] = { "parameter", "required_parameter" }

# 言語が省く事を許す空の仮引数・実引数の並び（値の空の組・単位 `()` は含めない）
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

# 数値リテラルらしいノード型のヒント（文字列・シンボルは除外）
NUMERIC_HINTS: tuple[str, ...] = "bin", "decimal", "float", "hex", "imaginary", "int", "number", "oct", "rational", "scientific"

def is_numeric_literal(node_type: str) -> bool:
	"""数値リテラルノードか判定する（表記揺れ正規化の対象）
	計算量：型名の文字数 L に対して O(L)

	引数：
	    node_type: ノード型

	戻値：
	    数値リテラルなら True"""
	LOWERED: str = node_type.lower()
	if "string" in LOWERED or "char" in LOWERED or "symbol" in LOWERED:
		# 値を保持すべき文字列系を対象外とする事の返戻
		return False
	# 数値ヒントに該当するかの返戻
	return any(hint in LOWERED for hint in NUMERIC_HINTS)

# 内部の空白其の物が値（意味）を持つ葉（文字列・ヒアドキュメントの本文等）
# 厳密にテキスト比較する
CONTENT_HINTS: tuple[str, ...] = "char", "content", "heredoc", "raw", "regex", "string"

def is_content_leaf(node_type: str) -> bool:
	"""空白も値の一部と為る内容の葉（文字列・ヒアドキュメントの本文等）か判定する
	計算量：型名の文字数 L に対して O(L)

	引数：
	    node_type: ノード型

	戻値：
	    内容の葉なら True"""
	LOWERED: str = node_type.lower()
	# 内容の葉に当たるかの返戻
	return any(hint in LOWERED for hint in CONTENT_HINTS)

def is_content_body(node_type: str) -> bool:
	"""文字列・ヒアドキュメントの本文（区切り記号を除く実内容）ノードか判定する
	計算量：型名の文字数 L に対して O(L)

	引数：
	    node_type: ノード型

	戻値：
	    内容本文ノードなら True"""
	LOWERED: str = node_type.lower()
	# 本文ノード（string_content / heredoc_content / string_fragment 等，デリミタは除外）に該当するかの返戻
	return "content" in LOWERED or "fragment" in LOWERED

def named_children(node: Node) -> list[Node]:
	"""コメントを除いた名前付の子の一覧を返す
	計算量：直下の子の数 N に対して O(N)

	引数：
	    node: 対象ノード

	戻値：
	    コメント以外の名前付子"""
	# 名前付の子の一覧の返戻
	return [child for child in node.children if child.kind == "N" and not is_comment(child.type)]

def has_comma(node: Node) -> bool:
	"""直下にコンマの字句を持つか判定する
	計算量：直下の子の数 N に対して O(N)

	引数：
	    node: 対象ノード

	戻値：
	    コンマを持てば True"""
	# コンマの字句の有無の返戻
	return any(child.kind == "U" and child.text.strip() == "," for child in node.children)

def is_empty_list(node: Node) -> bool:
	"""中身の無い仮引数・実引数の並び（空の並びだけを包む宣言子を含む）か判定する

	計算量：O(n)（n は対象部分木のノード数）

	引数：
	    node: 対象ノード

	戻値：
	    空の並びなら True"""
	# 並びの種別で，名前付の子も空の並びだけかの返戻（`f(())` の `()` は値の空の組の為に並びでない）
	return node.type in EMPTY_LISTS and bool(node.children) and all(is_empty_list(child) for child in named_children(node))

def normalize(node: Node, lang: str = "", ancestors: tuple[Node, ...] = ()) -> list[NormNode]:
	"""ノードを意味正規形へ落とす（付随情報は除去，括弧・構造ラッパは中身を昇格）

	計算量：最悪 O(n²)（n は対象部分木のノード数）

	引数：
	    node: 正規化するノード
	    lang: 言語キー
	    ancestors: 外側から並べた祖先ノード

	戻値：
	    透過要素を除いた正規形ノードの一覧"""
	# コメントと意味に無関係な字句の除外
	NODE_TYPE: str = node.type
	if is_comment(NODE_TYPE):
		# 意味に無関係なコメントを除いた空の並びの返戻
		return []
	if node.kind == "U" and ((STRIPPED := node.text.strip()) == "" or STRIPPED in ("(", ")", ",", ";")):
		# 空白と文の区切と括弧の字句（ネストは構文木が表す）を除いた空の並びの返戻（英字の演算子と制御字句は保持する）
		return []
	# 空の実引数と冗長な括弧の除去
	named: list[Node] = named_children(node)
	if node.kind == "N" and is_empty_list(node):
		# 空の括弧の並び（言語が省く事を許す空の仮引数・実引数）を除いた空の並びの返戻
		return []
	if (
		len(named) == 1 and (is_paren_wrapper(NODE_TYPE) or lang == "swift" and NODE_TYPE == "tuple_expression" and not has_comma(node))
	):
		# 単一式を包む冗長括弧と Swift の要素１つの組を剥がした中身の返戻
		return normalize(named[0], lang, ancestors)
	if (
		NODE_TYPE == "tuple_type" and len(named) == 1 and not has_comma(node) and
		len((inner := named_children(named[0]) if named[0].type == "tuple_type_item" else [named[0]])) == 1
	):
		# 要素１つの組の型を型を包む括弧と見た中身の返戻
		return normalize(inner[0], lang, ancestors)
	if lang == "swift" and NODE_TYPE == "optional_type" and len(named) == 1:
		# 解析器が `??` を１つの字句に読む為，ネストの省略可能の型を印の数へ畳む（`(Int?)?` は `Int??`）
		marks: int = sum(child.text.count("?") for child in node.children if child.kind == "U")
		base: list[NormNode] = normalize(named[0], lang, ancestors + (node,))
		if len(base) == 1 and base[0][0].startswith("optional_type:"):
			marks += int(base[0][0].split(":")[1])
			base = list(base[0][2])
		# 印の数を型名に持つ正規形の返戻
		return [(f"optional_type:{marks}", None, tuple(base))]
	if (
		NODE_TYPE in PARAMETER_LISTS and len(named) == 1 and
		len((inner := named_children(named[0]) if named[0].type in PARAMETER_WRAPPERS else [named[0]])) == 1 and
		inner[0].type == "identifier"
	):
		# 名前だけの仮引数１つの並びを名前と同じと見た中身の返戻
		return normalize(inner[0], lang, ancestors)
	if lang == "python" and NODE_TYPE in PYTHON_TUPLES:
		if len(named) == 1 and not has_comma(node):
			# コンマの無い要素１つの組を名前を包む括弧と見た中身の返戻
			return normalize(named[0], lang, ancestors)
		# 括弧の有無で変わる組の種別を揃えた正規形の返戻
		return [("tuple", None, tuple(norm for child in named for norm in normalize(child, lang, ancestors + (node,))))]
	if lang == "python" and NODE_TYPE == "subscript" and len(named) > 1 and (len(named) > 2 or has_comma(node)):
		# 添字に並べた要素 (`d[a, b]`) は組の添字 (`d[(a, b)]`) と同じ
		indices: tuple[NormNode, ...] = tuple(norm for child in named[1:] for norm in normalize(child, lang, ancestors + (node,)))
		# 組を添字に持つ正規形の返戻
		return [
			(
				NODE_TYPE,
				None,
				tuple(normalize(named[0], lang, ancestors + (node,))) + (("[", "[", ()), ("tuple", None, indices), ("]", "]", ()))
			)
		]
	# 単一式本体と返戻の正規化
	if (
		lang == "kotlin" and NODE_TYPE == "control_structure_body" and len((bodies := named_children(node))) == 1 and
		bodies[0].type == "statements" and len((values := named_children(bodies[0]))) == 1
	):
		head: Node = values[0]
		while head.type == "prefix_expression" and head.children and head.children[0].type in ("annotation", "label"):
			head = head.children[-1]
		if not head.type.endswith("_declaration") and not head.text.lstrip().startswith("{"):
			# 単一式本体の包装だけを吸収した節点の返戻（宣言のスコープとラムダの実行境界は保持する）
			return [(NODE_TYPE, None, tuple(normalize(values[0], lang, ancestors + (node,))))]
	if lang == "ruby" and NODE_TYPE == "return" and ancestors:
		parent: Node = ancestors[-1]
		siblings: list[Node] = named_children(parent)
		IS_METHOD_TAIL: bool = (
			parent.type == "body_statement" and len(ancestors) > 1 and ancestors[-2].type in ("method", "singleton_method")
		)
		IS_RESCUE_TAIL: bool = (
			parent.type == "then" and len(ancestors) > 3 and ancestors[-2].type == "rescue" and ancestors[-3].type == "body_statement" and
			ancestors[-4].type in ("method", "singleton_method")
		)
		if IS_METHOD_TAIL:
			siblings = [child for child in siblings if child.type not in ("ensure", "rescue")]
		named = [child for child in node.children if child.kind == "N"]
		if (
			(IS_METHOD_TAIL or IS_RESCUE_TAIL) and siblings and siblings[-1] is node and len(named) == 1 and
			(argument := named[0]).type == "argument_list" and
			len((values := [child for child in argument.children if child.kind == "N"])) == 1
		):
			# メソッド末尾の単一返戻値の正規形の返戻
			return normalize(values[0], lang, ancestors)
	if NODE_TYPE in TRANSPARENT_WRAPPERS:
		# 構造ラッパは型を消し名前付子のみ昇格（return / 波括弧 / `;` 等の無名トークンは破棄）
		lifted: list[NormNode] = [
			norm for child in node.children if child.kind == "N" for norm in normalize(child, lang, ancestors + (node,))
		]
		# 透過後の子列の返戻
		return lifted
	# 中間節点と内容葉の正規形の構築
	out_children: list[NormNode] = [norm for child in node.children for norm in normalize(child, lang, ancestors + (node,))]
	if (
		lang in ("kotlin", "swift") and NODE_TYPE == "call_expression" and out_children and out_children[0][0] == "call_expression" and
		len(out_children[0][2]) == 1
	):
		# Swift / Kotlin の後置のクロージャの前の空の実引数を外した呼出（`xs.map() { }` の `xs.map()`）は被呼出側と同じ（他の言語の `f()()` は別の呼出）
		out_children[0] = out_children[0][2][0]
	# 中間ノードはテキスト不要
	# 数値表記は正規化し，内容葉と記号は厳密に保持する
	TEXT: str|None = (
		None if out_children else node.text.lower().replace("_", "") if is_numeric_literal(NODE_TYPE) else node.text if
		is_content_leaf(NODE_TYPE) else
		re.sub(r"\s+", " ", node.text).strip() if any(char.isalpha() for char in node.text) else node.text
	)
	# 正規形ノードの返戻
	return [(NODE_TYPE, TEXT, tuple(out_children))]

def sexpr(norm: NormNode) -> str:
	"""正規形ノードを正準 S 式文字列へ変換する

	計算量：最悪 O(n²)（n は正規形のノード数）

	引数：
	    norm: 正規形ノード

	戻値：
	    正準 S 式"""
	node_type: str
	text: str|None
	children: tuple[NormNode, ...]
	node_type, text, children = norm
	if not children:
		# 葉の型とテキストの返戻
		return f"({node_type} {text!r})"
	# 中間ノードの型と子の S 式の返戻
	return f"({node_type} " + " ".join(sexpr(child) for child in children) + ")"

def forest_sexpr(roots: list[Node], lang: str = "") -> str:
	"""木群全体を正準 S 式へ変換する

	計算量：最悪 O(n²)（n は木群のノード数）

	引数：
	    roots: 最上位ノードの一覧
	    lang: 言語キー

	戻値：
	    木群の正準 S 式"""
	# 木群の S 式の返戻
	return " ".join(sexpr(norm) for root in roots for norm in normalize(root, lang))

def has_error(roots: list[Node]) -> bool:
	"""木群内に ERROR ノードが存在するか判定する

	計算量：O(n)（n は木群のノード数）

	引数：
	    roots: 最上位ノードの一覧

	戻値：
	    ERROR 又は MISSING が有れば True"""
	def has_error_below(node: Node) -> bool:
		"""ノード以下に ERROR ノードが有るか再帰判定する

		計算量：O(n)（n は対象部分木のノード数）

		引数：
		    node: 対象ノード

		戻値：
		    ERROR 又は MISSING が有れば True"""
		if node.type in ("ERROR", "MISSING"):
			# ERROR を検出した事の返戻
			return True
		# 子に ERROR が有るかの返戻
		return any(has_error_below(child) for child in node.children)
	# 木群に ERROR が有るかの返戻
	return any(has_error_below(root) for root in roots)

def content_body_text(node_type: str, text: str) -> str:
	"""内容本文ノードのテキストを意味比較用に正規化する（言語仕様上本文に含まれない部分を除く）
	計算量：本文の文字数 L に対して O(L)

	引数：
	    node_type: ノード型
	    text: 内容本文の原文

	戻値：
	    意味比較用の本文"""
	if node_type == "multiline_string_fragment":
		# Java テキストブロックの開始行の空白は本文から除く
		# tree-sitter は此の行末空白を fragment 先頭（最初の改行より前）に含める為，整形での行末空白除去を意味破壊と誤検出する
		# 先頭の水平空白だけを除いた本文の返戻（最初の改行以降の実本文は厳密比較を維持し，真の本文変質は捕捉）
		return re.sub(r"^[ \t]+", "", text)
	# 其れ以外の逐語の儘の返戻
	return text

def content_signature(roots: list[Node]) -> tuple[str, ...]:
	"""文字列・ヒアドキュメントの本文等の内容の葉のテキスト列（出現順）を返す

	計算量：O(n + b)（n は木群のノード数，b は本文の総文字数）

	引数：
	    roots: 最上位ノードの一覧

	戻値：
	    出現順の内容本文"""
	# 内容本文の葉の再帰収集
	texts: list[str] = []
	def walk(node: Node) -> None:
		"""内容の葉のテキストを再帰で集める

		計算量：O(n + b)（n は対象部分木のノード数，b は本文の総文字数）

		引数：
		    node: 対象ノード"""
		if is_comment(node.type):
			# 収集対象外のコメントの場合の終了
			return
		if not node.children:
			if is_content_body(node.type):
				texts.append(content_body_text(node.type, node.text))
			# 葉を集めた後の終了
			return
		for child in node.children:
			walk(child)
		# 終了
		return
	for root in roots:
		walk(root)
	# 内容の葉のテキスト列の返戻
	return tuple(texts)

def dump_ast(ext: str, code: str) -> str:
	"""dumpast で完全な構文木の文字列を得る
	計算量：原文の文字数 L・解析器の実行時間 T に対して O(L+T)

	引数：
	    ext: 入力言語の拡張子
	    code: 解析するソース

	戻値：
	    dumpast の標準出力

	送出例外：
	    RuntimeError: dumpast が失敗した場合"""
	# 解析器による構文木の取得
	completed: subprocess.CompletedProcess[str] = subprocess.run(
		[str(DUMPAST), "-", ext],
		input=code,
		capture_output=True,
		text=True,
		timeout=COMMAND_TIMEOUT
	)
	if completed.returncode:
		raise RuntimeError(f"dumpast failed ext={ext}: {completed.stderr}")
	# 構文木の文字列の返戻
	return completed.stdout

def format_code(lang: str, width: str, code: str) -> str:
	"""shavefmt で指定幅の整形結果を得る
	計算量：原文の文字数 L・整形器の実行時間 T に対して O(L+T)

	引数：
	    lang: shavefmt の言語識別子
	    width: 最大表示幅
	    code: 整形するソース

	戻値：
	    整形済ソース

	送出例外：
	    RuntimeError: 整形の失敗又は見送の場合"""
	# 整形器による指定幅の出力の取得
	completed: subprocess.CompletedProcess[str] = subprocess.run(
		[str(FMT), "--stdin", "--lang", lang, "--chars", width, "--no-lint"],
		input=code,
		capture_output=True,
		text=True,
		timeout=COMMAND_TIMEOUT
	)
	if completed.returncode:
		raise RuntimeError(f"format failed lang={lang} width={width}: {completed.stderr}")
	if "[skip]" in completed.stderr:
		raise RuntimeError(f"skipped lang={lang} width={width}: {completed.stderr.strip()}")
	# 整形結果の返戻
	return completed.stdout

class Failure:
	"""意味破壊・構文破壊の検出結果"""

	__slots__ = "kind", "key", "label", "width", "code"
	kind: str
	key: str
	label: str
	width: str
	code: str

	def __init__(self, kind: str, key: str, label: str, width: str, code: str) -> None:
		"""検出結果を破壊種別・言語・ラベル・幅・整形出力で初期化する

		引数：
		    kind: 破壊種別
		    key: 言語キー
		    label: 検体名
		    width: 整形幅
		    code: 整形結果又は失敗内容"""
		self.kind = kind
		self.key = key
		self.label = label
		self.width = width
		self.code = code
		# 終了
		return

def classify(before_roots: list[Node], before: str, after_roots: list[Node], lang: str = "") -> str:
	"""整形前後の木群を比較し破壊種別（"" は保存）を判定する
	計算量：木群の節点数 N・内容の総文字数 B に対して最悪 O(N²+B)

	引数：
	    before_roots: 整形前の木群
	    before: 整形前の正準 S 式
	    after_roots: 整形後の木群
	    lang: 言語キー

	戻値：
	    破壊種別（保存時は空文字列）"""
	if has_error(after_roots):
		# 整形後の構文エラー（最重大）の返戻
		return "構文破壊"
	if content_signature(before_roots) != content_signature(after_roots):
		# 文字列・ヒアドキュメントの本文の変質（高確度の意味破壊）の返戻
		return "内容破壊"
	if before != forest_sexpr(after_roots, lang):
		# 構造の変化（仕様に沿う変換と真の結合移動が混在・要個別確認）の返戻
		return "構造差"
	# 保存の返戻
	return ""

def check_case(key: str, label: str, code: str) -> list[Failure]:
	"""１ケースを全幅で整形し意味保存・構文非破壊を検証する

	計算量：O(w × p)（w は検査幅数，p は１回の整形・解析費用）

	引数：
	    key: 言語キー
	    label: 検体名
	    code: 検体のソース

	戻値：
	    検出した失敗の一覧"""
	# 各幅の整形と意味保存の照合
	lang: str
	ext: str
	lang, ext = LANGS[key]
	before_roots: list[Node] = parse_dump(dump_ast(ext, code))
	if has_error(before_roots):
		# 合成検体の不正入力を検査成功と数えない失敗の返戻
		return [Failure("入力不正", key, label, "input", code)]
	before: str = forest_sexpr(before_roots, key)
	failures: list[Failure] = []
	for width in WIDTHS:
		try:
			out: str = format_code(lang, width, code)
		except RuntimeError as e:
			failures.append(Failure("整形未完了", key, label, width, str(e)))
			continue
		after_roots: list[Node] = parse_dump(dump_ast(ext, out))
		if (kind := classify(before_roots, before, after_roots, key)):
			failures.append(Failure(kind, key, label, width, out))
	# 検出結果の返戻
	return failures

def report_failures(failures: list[Failure]) -> None:
	"""検出した失敗を詳細表示する
	計算量：失敗の件数 F・表示する総文字数 B に対して O(F+B)

	引数：
	    failures: 表示する失敗"""
	# 検出した失敗の詳細表示
	for fail in failures:
		print(f"\n!!!! {fail.kind} [{fail.key}] {fail.label} (chars={fail.width})")
		print("---- 整形出力 ----")
		print(fail.code.rstrip("\n"))
	# 終了
	return

def main() -> int:
	"""合成ケースを検証し結果を返す
	計算量：対象検体数 F・１検体の最大検査時間 T に対して O(FT)

	戻値：
	    検証成功なら０，失敗なら１"""
	# 検査環境と対象言語の確認
	if not FMT.is_file() or not DUMPAST.is_file():
		print(f"missing binary: {FMT} / {DUMPAST}", file=sys.stderr)
		# バイナリ欠如の異常終了の返戻
		return 1
	filters: set[str] = set(sys.argv[1:])
	if (unknown := filters - LANGS.keys()):
		print(f"invalid arguments: {sorted(unknown)}", file=sys.stderr)
		# 不正な引数の失敗の返戻
		return 1
	keys: list[str] = [key for key in LANGS if not filters or key in filters]
	if (missing := set(keys) - { key for key, _, _ in CASES }):
		print(f"missing synthetic cases: {sorted(missing)}", file=sys.stderr)
		# 検体不足の失敗の返戻
		return 1
	case_failures: list[Failure] = []
	case_count: int = 0
	# 各検体の照合と結果の集計
	for key, label, code in CASES:
		if key not in keys:
			continue
		case_count += 1
		fails: list[Failure] = check_case(key, label, code)
		status: str = "成功" if not fails else "失敗"
		print(f"[{status}] {key}: {label}")
		case_failures.extend(fails)
	# 失敗の詳細表示
	report_failures(case_failures)
	print("\n=== 検証結果 ===")
	CASE_RESULT: str = "検査通過" if not case_failures else str(len(case_failures)) + " 件の失敗"
	print(f"合成 {case_count} ケース: {CASE_RESULT}")
	print("FAILED" if case_failures else "ALL_OK")
	# 失敗が無い場合の `0` の返戻
	return 1 if case_failures else 0

if __name__ == "__main__":
	sys.exit(main())
