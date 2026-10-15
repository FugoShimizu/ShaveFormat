import { assertGrammarShape } from "./grammarShape.mjs";
import * as fileSystem from "node:fs";
import path from "node:path";

// 文法の入力・出力のパスと既存の本文規則
const [src, output] = process.argv.slice(2);
const grammar = JSON.parse(fileSystem.readFileSync(src, "utf8"));

// 文法の更新で編集の前提が変わった場合は，生成前に失敗させる
assertGrammarShape(
	grammar,
	{ name: "html", rules: { text: { type: "PATTERN", value: "[^<>&\\s]([^<>&]*[^<>&\\s])?" } } },
	"grammar"
);

// 長く一致する文字参照を保ち，其れに為らない１文字の記号だけを本文へ加える
grammar.rules.text =
{ type: "TOKEN", content: { type: "CHOICE", members: [grammar.rules.text, { type: "PATTERN", value: "[&>]" }] } };

// 拡張した文法の出力
fileSystem.mkdirSync(output, { recursive: true });
fileSystem.writeFileSync(path.join(output, "grammar.json"), JSON.stringify(grammar, null, 2) + "\n");
