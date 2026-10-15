import { assertGrammarShape } from "./grammarShape.mjs";
import * as fileSystem from "node:fs";
import path from "node:path";

// 文法とスキャナーの入力・生成先
const [src, output] = process.argv.slice(2);
const grammar = JSON.parse(fileSystem.readFileSync(src, "utf8"));

// 文法の更新で編集の前提が変われば，生成前に拒否する
assertGrammarShape(grammar.rules._reserved_identifier, { type: "CHOICE" }, "grammar.rules._reserved_identifier");

assertGrammarShape(
	grammar.rules.pattern,
	{
		type: "PREC_DYNAMIC",
		value: -1,
		content: { type: "CHOICE", members: [{ type: "SYMBOL", name: "_lhs_expression" }, { type: "SYMBOL", name: "rest_pattern" }] }
	},
	"grammar.rules.pattern"
);

assertGrammarShape(
	grammar.rules.rest_pattern,
	{
		type: "PREC_RIGHT",
		value: 0,
		content: { type: "SEQ", members: [{ type: "STRING", value: "..." }, { type: "SYMBOL", name: "_lhs_expression" }] }
	},
	"grammar.rules.rest_pattern"
);

assertGrammarShape(
	grammar.rules.class_body,
	{
		type: "SEQ",
		members: [
			{ type: "STRING", value: "{" },
			{
				type: "REPEAT",
				content: {
					type: "CHOICE",
					members: [
						{ type: "SEQ" },
						{ type: "SEQ" },
						{ type: "SYMBOL", name: "class_static_block" },
						{ type: "SEQ" },
						{ type: "STRING", value: ";" }
					]
				}
			},
			{ type: "STRING", value: "}" }
		]
	},
	"grammar.rules.class_body"
);

const members = grammar.rules.class_body.members[1].content.members;

assertGrammarShape(
	members[3],
	{
		type: "SEQ",
		members: [
			{
				type: "CHOICE",
				members: [
					{ type: "SYMBOL", name: "abstract_method_signature" },
					{ type: "SYMBOL", name: "index_signature" },
					{ type: "SYMBOL", name: "method_signature" },
					{ type: "SYMBOL", name: "public_field_definition" }
				]
			},
			{ type: "CHOICE", members: [{ type: "SYMBOL", name: "_semicolon" }, { type: "STRING", value: "," }] }
		]
	},
	"grammar.rules.class_body.members.1.content.members.3"
);

assertGrammarShape(
	grammar.externals,
	[
		{ type: "SYMBOL", name: "_automatic_semicolon" },
		{ type: "SYMBOL", name: "_template_chars" },
		{ type: "SYMBOL", name: "_ternary_qmark" },
		{ type: "SYMBOL", name: "html_comment" },
		{ type: "STRING", value: "||" },
		{ type: "SYMBOL", name: "escape_sequence" },
		{ type: "SYMBOL", name: "regex_pattern" },
		{ type: "SYMBOL", name: "jsx_text" },
		{ type: "SYMBOL", name: "_function_signature_automatic_semicolon" },
		{ type: "SYMBOL", name: "__error_recovery" }
	],
	"grammar.externals"
);

// 識別子の枝も残し，演算子・修飾語の読みは既存の文脈の判定へ渡す
grammar.rules._reserved_identifier.members.push({ type: "STRING", value: "abstract" }, { type: "STRING", value: "accessor" });

const awaitName = { type: "ALIAS", content: { type: "STRING", value: "await" }, named: true, value: "identifier" };

grammar.rules.pattern.content.members.push(awaitName);

// 残余の束縛も，通常の左辺の読みを残して同じ識別子を受け入れる
grammar.rules.rest_pattern.content.members[1] =
{ type: "CHOICE", members: [grammar.rules.rest_pattern.content.members[1], awaitName] };

// メソッドの名前から仮引数への継続だけを，型やシグネチャの文境界と分ける
members[3].members[0].members.pop();

members.push(
	{
		type: "SEQ",
		members: [
			{ type: "SYMBOL", name: "public_field_definition" },
			{
				type: "CHOICE",
				members:
				[{ type: "SYMBOL", name: "_class_field_automatic_semicolon" }, { type: "STRING", value: "," }, { type: "STRING", value: ";" }]
			}
		]
	}
);

// 元の外部トークンのインデックスを保持して末尾へ追加する
grammar.externals.push({ type: "SYMBOL", name: "_class_field_automatic_semicolon" });

// フィールド専用の文境界で曖昧性が消える為，既存の競合の指定を除く
const classConflict = grammar.conflicts.findIndex(
	conflict => conflict.length == 2 && conflict[0] == "class_static_block" && conflict[1] == "_property_name"
);

if(classConflict < 0) throw new Error("Grammar shape mismatch at class member conflict");

grammar.conflicts.splice(classConflict, 1);

// 共用ヘッダを生成先へ取り込み，コピーしたスキャナーの相対パスに依存させない
const scannerSrc = path.join(path.dirname(src), "scanner.c");
let scanner;

if(fileSystem.existsSync(scannerSrc)) {
	const wrapper = fileSystem.readFileSync(scannerSrc, "utf8");
	const include = "#include \"../../common/scanner.h\"";
	const header = fileSystem.readFileSync(path.resolve(path.dirname(src), "../../common/scanner.h"), "utf8");
	const edits = [
		["    ERROR_RECOVERY,", "    ERROR_RECOVERY,\n    CLASS_FIELD_AUTOMATIC_SEMICOLON,"],
		[
			"    lexer->result_symbol = AUTOMATIC_SEMICOLON;",
			"    lexer->result_symbol = valid_symbols[CLASS_FIELD_AUTOMATIC_SEMICOLON] ? CLASS_FIELD_AUTOMATIC_SEMICOLON : AUTOMATIC_SEMICOLON;"
		],
		[
			"            if (valid_symbols[LOGICAL_OR]) {",
			"            if (valid_symbols[LOGICAL_OR] || (lexer->lookahead == '(' && valid_symbols[CLASS_FIELD_AUTOMATIC_SEMICOLON])) {"
		],
		[
			"        if (valid_symbols[AUTOMATIC_SEMICOLON]) {",
			"        if (valid_symbols[AUTOMATIC_SEMICOLON] || valid_symbols[CLASS_FIELD_AUTOMATIC_SEMICOLON]) {"
		],
		[
			"    if (valid_symbols[AUTOMATIC_SEMICOLON] || valid_symbols[FUNCTION_SIGNATURE_AUTOMATIC_SEMICOLON]) {",
			"    if (valid_symbols[AUTOMATIC_SEMICOLON] || valid_symbols[FUNCTION_SIGNATURE_AUTOMATIC_SEMICOLON] || valid_symbols[CLASS_FIELD_AUTOMATIC_SEMICOLON]) {"
		]
	];
	// 上流の分岐が変われば，未確認のスキャナーを出力しない
	if(wrapper.split(include).length != 2) throw new Error("Grammar shape mismatch at scanner include");
	for(const [before] of edits) if(header.split(before).length != 2) {
		throw new Error("Grammar shape mismatch at scanner boundary: " + before);
	}
	let extended = header;
	for(const [before, after] of edits) extended = extended.replace(before, after);
	scanner = wrapper.replace(include, () => extended.trimEnd());
}

// 文法と外部スキャナーを同じ生成先へ出力する
fileSystem.mkdirSync(output, { recursive: true });
fileSystem.writeFileSync(path.join(output, "grammar.json"), JSON.stringify(grammar, null, 2) + "\n");

if(scanner !== undefined) fileSystem.writeFileSync(path.join(output, "scanner.c"), scanner);
