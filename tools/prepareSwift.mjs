// 型引数・空のタプル・型本体の診断・属性の型制約を既存のノードで読む
import { assertGrammarShape } from "./grammarShape.mjs";
import * as fileSystem from "node:fs";
import path from "node:path";

// 文法の入力・出力の経路
const [src, output] = process.argv.slice(2);
const grammar = JSON.parse(fileSystem.readFileSync(src, "utf8"));

// 文法の更新で編集の前提が変わった場合は，生成前に失敗させる
assertGrammarShape(
	grammar,
	{
		name: "swift",
		rules: {
			type_arguments: {
				type: "PREC_LEFT",
				content: { type: "SEQ", members: [{ type: "STRING", value: "<" }, { type: "SEQ" }, { type: "STRING", value: ">" }] }
			}
		}
	},
	"grammar"
);

assertGrammarShape(
	grammar.rules.tuple_expression,
	{
		type: "PREC_RIGHT",
		content: { type: "SEQ", members: [{ type: "STRING", value: "(" }, { type: "SEQ" }, { type: "STRING", value: ")" }] }
	},
	"grammar.rules.tuple_expression"
);

assertGrammarShape(grammar.rules._type_level_declaration, { type: "CHOICE" }, "grammar.rules._type_level_declaration");

assertGrammarShape(
	grammar.rules._protocol_member_declaration,
	{ type: "CHOICE" },
	"grammar.rules._protocol_member_declaration"
);

assertGrammarShape(grammar.rules._attribute_argument, { type: "CHOICE" }, "grammar.rules._attribute_argument");
assertGrammarShape(grammar.rules.type_constraints, { type: "PREC_RIGHT" }, "grammar.rules.type_constraints");
// 型引数の既存規則への動的な優先順位の付与
grammar.rules.type_arguments = { type: "PREC_DYNAMIC", value: 1, content: grammar.rules.type_arguments };

// 要素数０のタプルも，値のタプルとして同じ括弧のノードへ含める
const tupleMembers = grammar.rules.tuple_expression.content.members;

tupleMembers[1] = { type: "CHOICE", members: [tupleMembers[1], { type: "BLANK" }] };
// コンパイラの診断は，型の種類を問わずメンバの位置でも受ける
grammar.rules._type_level_declaration.members.push({ type: "SYMBOL", name: "diagnostic" });
grammar.rules._protocol_member_declaration.members.push({ type: "SYMBOL", name: "diagnostic" });
// 属性の名前に依らず，既存の where の型制約を属性の実引数にも認める
grammar.rules._attribute_argument.members.push({ type: "SYMBOL", name: "type_constraints" });
// 拡張した文法の出力
fileSystem.mkdirSync(output, { recursive: true });
fileSystem.writeFileSync(path.join(output, "grammar.json"), JSON.stringify(grammar, null, 2) + "\n");

// 同じ入口で外部スキャナーの境界の判定も拡張する（文法の形だけを検査する入力にはスキャナーが無い）
const scannerSrc = path.join(path.dirname(src), "scanner.c");

if(fileSystem.existsSync(scannerSrc)) {
	const scanner = fileSystem.readFileSync(scannerSrc, "utf8");
	// 行コメントの２個目のスラッシュを通常の演算子へ戻す分岐が，文境界の探索を遮って居る
	const original = "} else if (any_comment == CONTINUE_PARSING_SLASH_CONSUMED) {";
	const replacement = "} else if (any_comment == CONTINUE_PARSING_SLASH_CONSUMED && lexer->lookahead != '/') {";
	const originalBlock = "if (!has_seen_single_comment) {";
	const replacementBlock = "if (!has_seen_single_comment && !semi_is_valid) {";
	const originalBoundary = "if (saw_operator) {\n            // The operator we saw should suppress the newline";
	const replacementBoundary =
	"if (saw_operator || lexer->lookahead == ')' || lexer->lookahead == ']' || lexer->lookahead == '}') {\n            // The operator or closing delimiter should suppress the newline";
	// 上流の処理が変われば，未確認のスキャナーを生成せず失敗させる
	if(scanner.split(original).length != 2) throw new Error("Swift scanner shape mismatch at comment lookahead");
	if(scanner.split(originalBlock).length != 2) throw new Error("Swift scanner shape mismatch at block comment boundary");
	if(scanner.split(originalBoundary).length != 2) throw new Error("Swift scanner shape mismatch at comment boundary");
	// 行コメントは既存の次の分岐で読み切り，コメントの後の演算子と文境界の判定へ進める
	fileSystem.writeFileSync(
		path.join(output, "scanner.c"),
		scanner.replace(original, replacement).replace(originalBlock, replacementBlock).replace(originalBoundary, replacementBoundary)
	);
}
