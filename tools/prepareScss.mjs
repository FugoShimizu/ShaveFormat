// 規則の順序を保ち，キーフレーム位置を小数とカンマ区切りの列へ拡張する
import * as fileSystem from "node:fs";
import path from "node:path";

const NUMBER_PATTERN = "[+-]?(?:[0-9]*\\.[0-9]+(?:[eE][+-]?[0-9]+)?|[0-9]+[eE][+-]?[0-9]+)";
const [source, output] = process.argv.slice(2);
const grammar = JSON.parse(fileSystem.readFileSync(source, "utf8"));
const selector = grammar.rules.keyframe_block.members[0];

grammar.rules.float_value.members[0].content = { type: "PATTERN", value: NUMBER_PATTERN };
selector.members.push({ type: "SYMBOL", name: "float_value" });

grammar.rules.keyframe_block.members[0] = {
	type: "SEQ",
	members: [selector, { type: "REPEAT", content: { type: "SEQ", members: [{ type: "STRING", value: "," }, selector] } }]
};

fileSystem.mkdirSync(output, { recursive: true });
fileSystem.writeFileSync(path.join(output, "grammar.json"), JSON.stringify(grammar, null, 2) + "\n");
