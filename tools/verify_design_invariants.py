#! /usr/bin/env python3
"""設計の不変条件の検査（整形仕様 3 の設計要件）

整形仕様 3 は「必要としないクラスへは型と依存関係の段階で渡さない」を設計の基本原則とする
此処では其の内，依存関係で機械的に確かめれる条件を取出の上で検査する

検査する条件：
1. 行幅 (`MaxChars`) を読むのは `LineBreaker` だけ（`Canonicalizer` を含む他の工程は行幅を受け取らない）
2. 中核のソース工程 (`SyntaxNormalizer` / `Canonicalizer` / `LineBreaker` / `CodeLayout`) がコメントの**本文**を読まない
3. `DocCommentSynchronizer` と `CommentNormalizer` と `CommentPlacer` は構文木を書き換えない（整形仕様 3 の権限の表）
4. コメントの配置計画は配置枠とコメント行の列だけを持ち，空行・コードの変更を表せない（整形仕様 3.2）
5. 変更不能なコードの出力は `CodeLayout` だけが，合成結果は `OutputDocument::Compose` だけが構築し，別の構築経路が無い
6. 空行を決める `CodeLayout` はコメントの記録を受け取らない
7. コメントの本文構造は `CommentNormalizer::Inspect` だけが読取専用の見方から判定し，変更不能な結果を返す（整形仕様 3.3）
見方は記法と文書形式及び其の定義，本文，記録の順序と所属だけを持つ
同期・配置・検証・静的検査は其の結果を共有し，投影は同じ群の判定 (`CanJoinComments`) で所属を記録する
8. 所有区画は子のコメントを除いたコード行と最終字面を別に持ち，親の中核はコード行だけを読む

違反は零に成った為，`golden_test.sh` へ組み込んで退行の検出器として使う
条件を満たせなく為る変更を入れる時は，実装でなく整形仕様 3 の設計要件を先に見直す

使い方：
python3 tools/verify_design_invariants.py
"""

from __future__ import annotations

from pathlib import Path

import re
import sys

ROOT: Path = Path(__file__).resolve().parent.parent
SRC: Path = ROOT / "src"
# 行幅を読んで良い実装ファイル
WIDTH_OWNER: str = "LineBreaker"

# コメントの経路に属し，本文を読んで良い実装ファイル
COMMENT_PATH: tuple[str, ...] = (
	"CommentNormalizer",
	"CommentPlacer",
	"CommentRepository",
	"DocCommentSynchronizer",
	"SourceProjector"
)

# 中核のソース工程（コメントの本文を読んではならない）
CORE_PASSES: tuple[str, ...] = "Canonicalizer", "CodeLayout", "LineBreaker", "SyntaxNormalizer"
# 構文木を書き換えてはならない工程
READ_ONLY_PATH: tuple[str, ...] = "CommentNormalizer", "CommentPlacer", "DocCommentSynchronizer"
# コメントの本文を読む字面
TEXT_READS: re.Pattern[str] = re.compile(r"\.Text\b|->Text\b")
# 関数の定義の見出（`型 クラス::関数(` の形，名前空間の中の字下げを含む）
DEFINITION: re.Pattern[str] = re.compile(r"^\s*[A-Za-z_][\w:<>,&*\s]*?\b([A-Z]\w+)::[A-Za-z_]\w*\s*\(")
# 名前空間を開く行
NAMESPACE: re.Pattern[str] = re.compile(r"^\s*namespace\b")
# 文字列・文字の字面（波括弧の深さを数える時に除く）
LITERAL: re.Pattern[str] = re.compile(r"\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'")

def sources() -> list[Path]:
	"""実装ファイルの経路を探して整列した一覧を返す
	計算量：走査した経路数 P・対象ファイル数 F に対して O(P+F log F)
	"""
	# 実装ファイルの一覧の返戻
	return sorted(list(SRC.rglob("*.cpp")) + list(SRC.rglob("*.hpp")))

def owners(text: str) -> list[str]:
	"""ソースの各行を所有するクラスを定義から判定して返す
	計算量：ソースの文字数 L に対して最悪 O(L²)
	"""
	# 関数定義と波括弧の深さによる所有クラスの追跡
	current: str = ""
	found: list[str] = []
	# 名前空間以外の波括弧の深さ（関数・クラスの本体の中の呼出を定義と見誤らない）と，開いた波括弧毎の名前空間か
	depth: int = 0
	scopes: list[bool] = []
	match: re.Match[str]|None
	for line in text.split("\n"):
		if not depth and (match := DEFINITION.match(line)):
			current = match.group(1)
		found.append(current)
		code: str = LITERAL.sub("", line.split("//")[0])
		for char in code:
			if char == "{":
				scopes.append(bool(NAMESPACE.match(code)))
				depth += not scopes[-1]
			elif char == "}" and scopes:
				depth -= not scopes.pop()
	# 行毎の所有クラスの一覧の返戻
	return found

def width_owner_violations() -> list[str]:
	"""行幅を LineBreaker の外で読む箇所を列挙する
	計算量：走査した経路数 P・対象ファイル数 F・総文字数 L に対して O(P+F log F+L)
	"""
	found: list[str] = []
	for path in sources():
		if path.stem.startswith(WIDTH_OWNER):
			continue
		for number, line in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
			# 行幅の受渡と構築だけは呼出側に許す（`Main` が `LineBreaker` を組み立てる）
			if "MaxChars" not in line or path.stem == "Main":
				continue
			found.append(f"{path.relative_to(ROOT)}:{number}: 行幅を {WIDTH_OWNER} の外で読んで居る")
	# 違反の一覧の返戻
	return found

def comment_text_violations() -> list[str]:
	"""中核のソース工程がコメントの本文を読む箇所を列挙する
	計算量：走査した経路数 P・対象ファイル数 F・総文字数 L に対して最悪 O(P+F log F+L²)
	"""
	found: list[str] = []
	for path in sources():
		text: str = path.read_text(encoding="utf-8")
		# コメントの記録を触らないファイルは読み様が無い
		if "CommentRecord" not in text:
			continue
		for number, (line, owner) in enumerate(zip(text.split("\n"), owners(text)), 1):
			# 属するクラスで判定し，コメントの経路の名前のファイルへ中核の関数を置いても素通りさせない
			if owner not in CORE_PASSES or owner in COMMENT_PATH:
				continue
			if not TEXT_READS.search(line) or line.lstrip().startswith("//"):
				continue
			found.append(f"{path.relative_to(ROOT)}:{number}: 中核の工程がコメントの本文を読んで居る")
	# 違反の一覧の返戻
	return found

def comment_path_mutation_violations() -> list[str]:
	"""コメントの経路が構文木を書き換えれる箇所を列挙する
	計算量：走査した経路数 P・対象ファイル数 F・総文字数 L に対して最悪 O(P+F log F+L²)
	"""
	# 構文木を書き換えれる形（`const` の付かない参照）
	mutable: re.Pattern[str] = re.compile(r"(?<!const )SyntaxModel\s*&")
	found: list[str] = []
	for path in sources():
		text: str = path.read_text(encoding="utf-8")
		for number, (line, owner) in enumerate(zip(text.split("\n"), owners(text)), 1):
			# 宣言だけのヘッダはクラスの本体で属を決めれない為，ファイル名で補う
			if (owner or path.stem) not in READ_ONLY_PATH:
				continue
			if not mutable.search(line) or line.lstrip().startswith("//") or line.lstrip().startswith("*"):
				continue
			found.append(f"{path.relative_to(ROOT)}:{number}: コメントの経路が構文木を書き換えられる")
	# 違反の一覧の返戻
	return found

def class_body(text: str, name: str) -> str:
	"""ヘッダで指定名のクラス・構造体の本体を返す
	見付からなければ空文字列を返す
	計算量：ヘッダの文字数 L に対して最悪 O(L²)
	"""
	match: re.Match[str]|None = re.search(rf"^[ \t]*(?:class|struct) {name}\b[^{{;]*\{{(.*?)^[ \t]*\}};", text, re.M | re.S)
	# 本体の字面の返戻
	return match.group(1) if match else ""

def fields(body: str) -> list[str]:
	"""クラス・構造体の本体が宣言する欄の名前を返す
	関数・型の宣言と注釈は除く
	計算量：本体の文字数 L に対して最悪 O(L²)
	"""
	found: list[str] = []
	match: re.Match[str]|None
	for line in body.split("\n"):
		code: str = line.split("//")[0].strip()
		# 欄でない行（関数・入れ子の型・区切り）の読飛
		if not code.endswith(";") or "(" in code or code.startswith(("friend", "using", "enum", "static")):
			continue
		if match := re.search(r"(\w+)\s*(?:=[^;]*|\{[^;]*\})?;$", code):
			found.append(match.group(1))
	# 欄の名前の一覧の返戻
	return found

def placement_plan_violations() -> list[str]:
	"""配置計画が配置枠とコメント行の列の外の欄を持つ箇所を列挙する
	計算量：対象ヘッダの文字数 L に対して最悪 O(L²)
	"""
	# 配置計画の許可欄と実際の欄の照合
	path: Path = SRC / "Util/LayoutTypes.hpp"
	text: str = path.read_text(encoding="utf-8")
	allowed: dict[str, list[str]] = {
		"CommentBlock": ["Lines"],
		"CommentInsertion": ["Slot", "Comment"],
		"CommentPlacementPlan": ["Owner", "Insertions"]
	}
	found: list[str] = []
	for name, expected in allowed.items():
		actual: list[str] = fields(class_body(text, name))
		if actual != expected:
			found.append(f"{path.relative_to(ROOT)}: {name} の欄が {actual} で，配置枠とコメント行の列だけでない")
	# 違反の一覧の返戻
	return found

def construction_violations() -> list[str]:
	"""変更不能な出力と合成結果を定めた工程の外で構築出来る箇所を列挙する
	計算量：走査した経路数 P・対象ファイル数 F・総文字数 L に対して最悪 O(P+F log F+L²)
	"""
	path: Path = SRC / "Util/LayoutTypes.hpp"
	# 構築子と友の宣言の検査
	text: str = path.read_text(encoding="utf-8")
	found: list[str] = []
	for name, owner in ("LaidOutCode", "friend class Core::CodeLayout;"), ("OutputDocument", ""):
		body: str = class_body(text, name)
		# 構築子は非公開の区画だけに置く
		if (
			not re.search(rf"^\s*(?:explicit )?{name}\(", body.split("public:")[0], re.M) or
			re.search(rf"^\s*(?:explicit )?{name}\(", body.split("public:")[-1], re.M)
		):
			found.append(f"{path.relative_to(ROOT)}: {name} の構築子が非公開でない")
		# 友の宣言は定めた工程だけ
		friends: list[str] = re.findall(r"^\s*friend class [\w:]+;", body, re.M)
		if [line.strip() for line in friends] != ([owner] if owner else []):
			found.append(f"{path.relative_to(ROOT)}: {name} の友が {friends} で，定めた工程だけでない")
	# 合成結果を構築するのは `Compose` だけ（`OutputDocument(` の呼出を其の定義の中だけに許す）
	for source in sources():
		lines: list[str] = source.read_text(encoding="utf-8").split("\n")
		for number, (line, owner) in enumerate(zip(lines, owners("\n".join(lines))), 1):
			if re.search(r"\bOutputDocument\(std::move", line) and owner != "OutputDocument":
				found.append(f"{source.relative_to(ROOT)}:{number}: OutputDocument を Compose の外で構築して居る")
	# 違反の一覧の返戻
	return found

def layout_comment_violations() -> list[str]:
	"""空行を決める CodeLayout がコメントの記録を受け取る箇所を列挙する
	計算量：走査した経路数 P・対象ファイル数 F・総文字数 L に対して O(P+F log F+L)
	"""
	found: list[str] = []
	for path in sources():
		if not path.stem.startswith("CodeLayout"):
			continue
		for number, line in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
			if (
				re.search(r"\b(CommentRecord|CommentRepository|GetAttachments|CommentPlacementPlan)\b", line) and
				not line.lstrip().startswith("//")
			):
				found.append(f"{path.relative_to(ROOT)}:{number}: CodeLayout がコメントの記録を扱って居る")
	# 違反の一覧の返戻
	return found

def owned_region_violations() -> list[str]:
	"""所有区画のコメントが文字列を経て親の中核へ入る経路を列挙する
	計算量：対象ファイルの総文字数 L に対して最悪 O(L²)
	"""
	# 所有区画の欄と，親の中核及び合成の入口の照合
	model: Path = SRC / "Core/SyntaxModel.hpp"
	layout: Path = SRC / "Core/CodeLayout.cpp"
	formatter: Path = SRC / "Formatter.cpp"
	finalizer: Path = SRC / "Util/OutputFinalizer.cpp"
	actual: list[str] = fields(class_body(model.read_text(encoding="utf-8"), "OwnedRegion"))
	expected: list[str] = ["CodeText", "CodeVerbatimLines", "Text", "VerbatimLines"]
	found: list[str] = []
	if actual != expected:
		found.append(f"{model.relative_to(ROOT)}: OwnedRegion の欄が {actual} で，コード行と子の最終字面に分かれて居ない")
	code: str = layout.read_text(encoding="utf-8")
	if "Region.CodeText" not in code or "Region.CodeVerbatimLines" not in code:
		found.append(f"{layout.relative_to(ROOT)}: 所有区画のコード行だけを配置して居ない")
	for path in SRC / "Core/SourceProjector.cpp", SRC / "Core/Canonicalizer.cpp", SRC / "Core/LineBreaker.cpp", layout:
		for number, line in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
			if re.search(r"\bRegion(?:\.|->)(?:Text|VerbatimLines)\b", line) and not line.lstrip().startswith("//"):
				found.append(f"{path.relative_to(ROOT)}:{number}: 親の中核が子の合成結果を読んで居る")
	control: str = formatter.read_text(encoding="utf-8")
	if (
		not re.search(r"Embedded->CodeText\s*=\s*OutputFinalizer::Finalize\(\s*OutputDocument::CodeOnly\(Code\)", control) or
		"std::move(Inner.CodeText)" not in control or "OutputFinalizer::Finalize(OutputDocument::CodeOnly(Code))" not in control
	):
		found.append(f"{formatter.relative_to(ROOT)}: 子のコメントを除いたコード行を渡して居ない")
	adapter: str = (SRC / "Util/ParseAdapter.cpp").read_text(encoding="utf-8")
	if "std::move(CodeText), std::move(CodeVerbatimLines), std::move(Output)" not in adapter:
		found.append("src/Util/ParseAdapter.cpp: 所有区画へコード行と子の最終字面を別に渡して居ない")
	assembly: str = finalizer.read_text(encoding="utf-8")
	if "Region.Text" not in assembly or "Document.IncludesOwnedComments()" not in assembly:
		found.append(f"{finalizer.relative_to(ROOT)}: 子の最終字面を出力の組立で合成して居ない")
	code_only: str = (SRC / "Util/LayoutTypes.cpp").read_text(encoding="utf-8")
	if "Document.IncludeOwnedComments = false;" not in code_only:
		found.append("src/Util/LayoutTypes.cpp: 中核の観測へ子のコメントを差し込める")
	# 違反の返戻
	return found

def comment_structure_violations() -> list[str]:
	"""本文構造の判定の入口・見方・結果の形と，工程間の共有を外れる箇所を列挙する
	計算量：走査した経路数 P・対象ファイル数 F・総文字数 L に対して最悪 O(P+F log F+L²)
	"""
	# 読取専用の見方の欄と公開関数の照合
	path: Path = SRC / "Util/CommentNormalizer.hpp"
	text: str = path.read_text(encoding="utf-8")
	found: list[str] = []
	# 見方は判定する記録と文書の位置で確定した所属を読取専用で持ち，記法と文書形式の定義（コード例の書式）・本文・順序・所属だけを読取専用の関数で公開する
	view: str = class_body(text, "ReadOnlyCommentView")
	if (
		fields(view) != ["Records", "Membership"] or "std::span<const CommentRecord> Records;" not in view or
		"std::span<const Core::CommentGroupId> Membership;" not in view
	):
		found.append(f"{path.relative_to(ROOT)}: ReadOnlyCommentView の欄が読取専用の記録の並びと所属だけでない")
	getters: list[str] = [
		match.group(1) + (match.group(2) or "") for match in
		re.finditer(r"^\s*[^/\n]*?\b(\w+)\(.*?\)( const)?;", view.split("public:")[-1], re.M) if match.group(1) != "ReadOnlyCommentView"
	]
	expected: list[str] = ["GetSize const", "GetCodeExamples const", "GetText const", "GetOrder const", "GetGroup const"]
	if getters != expected:
		found.append(f"{path.relative_to(ROOT)}: ReadOnlyCommentView の公開が {getters} で，記法と文書形式の定義・本文・順序・所属の読取だけでない")
	# 結果は判定だけが構築し，公開の関数は全て読取専用とする
	result: str = class_body(text, "CommentStructure")
	public: str = result.split("public:")[-1]
	# 判定結果の構築権と読取専用性の照合
	if [line.strip() for line in re.findall(r"^\s*friend class [\w:]+;", result, re.M)] != ["friend class CommentNormalizer;"]:
		found.append(f"{path.relative_to(ROOT)}: CommentStructure の友が判定の工程だけでない")
	if (
		not re.search(r"^\s*CommentStructure\(\)", result.split("public:")[0], re.M) or
		re.findall(r"^\s*CommentStructure\((.*?)\)", public, re.M) != ["const CommentStructure &", "CommentStructure &&"]
	):
		found.append(f"{path.relative_to(ROOT)}: CommentStructure の構築子が非公開でない（公開は写しと移動だけ）")
	for operand in "const CommentStructure &", "CommentStructure &&":
		if f"CommentStructure &operator=({operand}) = delete;" not in public:
			found.append(f"{path.relative_to(ROOT)}: CommentStructure の代入演算子 `{operand}` が削除されて居らず，判定結果を書き換えられる")
	for declaration in re.findall(r"^\s*[^/\n]*\);", public, re.M):
		if not declaration.rstrip().endswith(") const;"):
			found.append(f"{path.relative_to(ROOT)}: CommentStructure の公開関数 `{declaration.strip()}` が読取専用でない")
	# 判定の入口は読取専用の見方だけを受ける（完成候補や記録の器を受けない）
	if "static CommentStructure Inspect(const ReadOnlyCommentView &View);" not in text:
		found.append(f"{path.relative_to(ROOT)}: Inspect が読取専用の見方だけを受ける判定の入口でない")
	# 投影・同期・配置・検証・静的検査は同じ判定を共有する（投影は `Inspect` が読む群の所属を同じ群の判定で記録する）
	# 各工程による共通判定の使用確認
	for stage, shared in (
		("SourceProjector", "CommentNormalizer::CanJoinComments("),
		("DocCommentSynchronizer", "CommentNormalizer::Inspect("),
		("CommentPlacer", "CommentNormalizer::Inspect("),
		("SyntaxValidator", "CommentNormalizer::Inspect("),
		("LintAnalyzer", "CommentNormalizer::Inspect(")
	):
		if not any(shared in source.read_text(encoding="utf-8") for source in sources() if source.stem.startswith(stage)):
			found.append(f"{stage} が本文構造の判定を {shared.rstrip('(')} で共有して居ない")
	# 違反の一覧の返戻
	return found

def main() -> int:
	"""設計の不変条件を検査し，違反が有れば非零で終える
	計算量：各検査の実行時間の和 T に対して O(T)
	"""
	# 不変条件の検査と違反の集計
	checks: tuple[tuple[str, list[str]], ...] = (
		("行幅の所有", width_owner_violations()),
		("コメントの本文の遮断", comment_text_violations()),
		("コメントの経路の読取専用", comment_path_mutation_violations()),
		("配置計画の権限", placement_plan_violations()),
		("出力の構築経路", construction_violations()),
		("空行の決定とコメントの分離", layout_comment_violations()),
		("所有区画のコメントの分離", owned_region_violations()),
		("本文構造の共通判定", comment_structure_violations())
	)
	total: int = 0
	for name, failures in checks:
		total += len(failures)
		for failure in failures:
			print(f"FAIL design invariant [{name}]: {failure}", file=sys.stderr)
	print(f"design invariants: {len(checks)} checks, {total} violations")
	# 違反が有れば非零の返戻
	return 1 if total else 0

if __name__ == "__main__":
	sys.exit(main())
