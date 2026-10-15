import { assertGrammarShape } from "./grammarShape.mjs";
import * as fileSystem from "node:fs";
import path from "node:path";

// 文法の入力・出力のパスと元の明示的実体化の規則
const [src, output] = process.argv.slice(2);
const grammar = JSON.parse(fileSystem.readFileSync(src, "utf8"));

// 文法の更新で編集の前提が変わった場合は，生成前に失敗させる
assertGrammarShape(
	grammar,
	{
		name: "cpp",
		rules: {
			template_instantiation: {
				type: "PREC",
				value: 1,
				content: {
					type: "SEQ",
					members: [
						{ type: "CHOICE", members: [{ type: "STRING", value: "extern" }, { type: "BLANK" }] },
						{ type: "STRING", value: "template" },
						{ type: "CHOICE", members: [{ type: "SYMBOL", name: "_declaration_specifiers" }, { type: "BLANK" }] },
						{ type: "FIELD", name: "declarator", content: { type: "SYMBOL", name: "_declarator" } },
						{ type: "STRING", value: ";" }
					]
				}
			}
		}
	},
	"grammar"
);

// 本体や属性を持つクラス定義へ受理範囲を広げず，既存の指定子ノードと名前の役割へ読む
const specifiers = [];

for(const [name, keyword] of [["class_specifier", "class"], ["struct_specifier", "struct"], ["union_specifier", "union"]]) {
	assertGrammarShape(
		grammar.rules[name],
		{ type: "SEQ", members: [{ type: "STRING", value: keyword }, { type: "SYMBOL", name: "_class_declaration" }] },
		`grammar.rules.${name}`
	);
}

for(const name of ["class_specifier", "struct_specifier", "union_specifier"]) {
	const ruleName = "_explicit_" + name;
	grammar.rules[ruleName] = {
		type: "SEQ",
		members: [grammar.rules[name].members[0], { type: "FIELD", name: "name", content: { type: "SYMBOL", name: "_class_name" } }]
	};
	specifiers.push({ type: "ALIAS", content: { type: "SYMBOL", name: ruleName }, named: true, value: name });
}

// 宣言子の無い枝は型の指定子だけとし，関数・変数の元の枝は其の儘残す
const members = grammar.rules.template_instantiation.content.members;

members.splice(
	2,
	2,
	{
		type: "CHOICE",
		members: [
			{ type: "SEQ", members: members.slice(2, 4) },
			{ type: "FIELD", name: "type", content: { type: "CHOICE", members: specifiers } }
		]
	}
);

// 拡張した文法の出力
fileSystem.mkdirSync(output, { recursive: true });
fileSystem.writeFileSync(path.join(output, "grammar.json"), JSON.stringify(grammar, null, 2) + "\n");
