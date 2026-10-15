// 既存の構文木を保ち，CSS・SCSS の不足する構文を文法へ追加する
import { assertGrammarShape } from "./grammarShape.mjs";
import * as fileSystem from "node:fs";
import path from "node:path";

// キーフレームの位置として読む指数付の数値の記法
const NUM_PATTERN = "[+-]?(?:[0-9]*\\.[0-9]+(?:[eE][+-]?[0-9]+)?|[0-9]+[eE][+-]?[0-9]+)";
// 文法の入力・出力の経路と編集するセレクタ
const [src, output] = process.argv.slice(2);
const grammar = JSON.parse(fileSystem.readFileSync(src, "utf8"));

// 文法の更新で位置・型・参照先が変わった場合は，編集を始める前に失敗させる
assertGrammarShape(
	grammar,
	{
		name: "scss",
		rules: {
			float_value: { type: "SEQ", members: [{ type: "TOKEN", content: { type: "SEQ" } }, { type: "CHOICE" }] },
			keyframe_block: {
				type: "SEQ",
				members: [
					{
						type: "CHOICE",
						members: [{ type: "SYMBOL", name: "from" }, { type: "SYMBOL", name: "to" }, { type: "SYMBOL", name: "integer_value" }]
					},
					{ type: "SYMBOL", name: "block" }
				]
			},
			identifier: { type: "PATTERN" },
			attribute_selector: {
				type: "SEQ",
				members: [
					{ type: "CHOICE" },
					{ type: "STRING", value: "[" },
					{
						type: "ALIAS",
						named: true,
						value: "attribute_name",
						content: { type: "CHOICE", members: [{ type: "SYMBOL", name: "identifier" }, { type: "SYMBOL", name: "namespace_selector" }] }
					},
					{ type: "CHOICE" },
					{ type: "STRING", value: "]" }
				]
			},
			declaration: {
				type: "SEQ",
				members: [
					{ type: "ALIAS", value: "property_name" },
					{ type: "STRING", value: ":" },
					{ type: "SYMBOL", name: "_value" },
					{ type: "REPEAT" },
					{ type: "CHOICE", members: [{ type: "SYMBOL", name: "important" }, { type: "BLANK" }] },
					{ type: "STRING", value: ";" }
				]
			},
			last_declaration: {
				type: "PREC",
				content: {
					type: "SEQ",
					members: [
						{ type: "ALIAS", value: "property_name" },
						{ type: "STRING", value: ":" },
						{ type: "SYMBOL", name: "_value" },
						{ type: "REPEAT" },
						{ type: "CHOICE", members: [{ type: "SYMBOL", name: "important" }, { type: "BLANK" }] }
					]
				}
			},
			forward_statement: {
				type: "SEQ",
				members: [{ type: "STRING", value: "@forward" }, { type: "SYMBOL", name: "_value" }, { type: "STRING", value: ";" }]
			},
			use_statement: {
				type: "SEQ",
				members: [{ type: "STRING", value: "@use" }, { type: "SYMBOL", name: "_value" }, { type: "STRING", value: ";" }]
			},
			include_statement: {
				type: "SEQ",
				members: [{ type: "STRING", value: "@include" }, { type: "SYMBOL", name: "identifier" }, { type: "CHOICE" }, { type: "CHOICE" }]
			},
			selectors: {
				type: "SEQ",
				members: [
					{ type: "SYMBOL", name: "_selector" },
					{ type: "REPEAT", content: { type: "SEQ", members: [{ type: "STRING", value: "," }, { type: "SYMBOL", name: "_selector" }] } }
				]
			},
			pseudo_class_selector: {
				type: "SEQ",
				members: [
					{ type: "CHOICE" },
					{ type: "ALIAS", value: ":" },
					{
						type: "ALIAS",
						value: "class_name",
						content:
						{ type: "CHOICE", members: [{ type: "SYMBOL", name: "identifier" }, { type: "SYMBOL", name: "_concatenated_identifier" }] }
					},
					{
						type: "CHOICE",
						members: [{ type: "ALIAS", value: "arguments", content: { type: "SYMBOL", name: "pseudo_class_arguments" } }, { type: "BLANK" }]
					}
				]
			},
			_selector: {
				type: "CHOICE",
				members: [
					{ type: "CHOICE" },
					{ type: "ALIAS", value: "tag_name", content: { type: "SYMBOL", name: "_concatenated_identifier" } },
					{ type: "SYMBOL", name: "placeholder" }
				]
			},
			at_rule: {
				type: "SEQ",
				members: [
					{ type: "SYMBOL", name: "at_keyword" },
					{
						type: "CHOICE",
						members: [
							{
								type: "SEQ",
								members: [
									{ type: "SYMBOL", name: "_query" },
									{ type: "REPEAT", content: { type: "SEQ", members: [{ type: "STRING", value: "," }, { type: "SYMBOL", name: "_query" }] } }
								]
							},
							{ type: "BLANK" }
						]
					},
					{ type: "CHOICE" }
				]
			},
			parameter: { type: "SEQ", members: [{ type: "SYMBOL", name: "variable" }, { type: "CHOICE" }] },
			_value: { type: "CHOICE", members: [{ type: "PREC" }, { type: "PREC" }, { type: "SYMBOL", name: "variable" }] }
		}
	},
	"grammar"
);

for(const [name, operator] of [["adjacent_sibling_selector", "+"], ["child_selector", ">"], ["sibling_selector", "~"]]) {
	assertGrammarShape(
		grammar.rules[name],
		{
			type: "PREC_LEFT",
			content: {
				type: "SEQ",
				members: [{ type: "SYMBOL", name: "_selector" }, { type: "STRING", value: operator }, { type: "SYMBOL", name: "_selector" }]
			}
		},
		`grammar.rules.${name}`
	);
}

// 括弧の追加位置と，空の形へ置き換える閉括弧の位置を照合する
for(const name of ["_include_arguments", "arguments", "list_value", "parameters", "pseudo_class_arguments"]) {
	const open = name === "list_value" || name === "parameters" ?
	{ type: "STRING", value: "(" } :
	{ type: "IMMEDIATE_TOKEN", content: { type: "STRING", value: "(" } };
	const close = name === "_include_arguments" ?
	{ type: "IMMEDIATE_TOKEN", content: { type: "STRING", value: ")" } } :
	{ type: "STRING", value: ")" };
	assertGrammarShape(grammar.rules[name], { type: "SEQ", members: [open, {}, close] }, `grammar.rules.${name}`);
}

for(const name of ["_include_arguments", "parameters"]) {
	const argument = name === "parameters" ?
	{ type: "SYMBOL", name: "parameter" } :
	{ type: "ALIAS", value: "argument", content: { type: "SYMBOL", name: "_include_argument" } };
	assertGrammarShape(
		grammar.rules[name].members[1],
		{
			type: "SEQ",
			members: [argument, { type: "REPEAT", content: { type: "SEQ", members: [{ type: "STRING", value: "," }, argument] } }]
		},
		`grammar.rules.${name}.members.1`
	);
	assertGrammarShape(
		grammar.rules[name].members[2],
		name === "parameters" ? { type: "STRING", value: ")" } : { type: "IMMEDIATE_TOKEN", content: { type: "STRING", value: ")" } },
		`grammar.rules.${name}.members.2`
	);
}

const pseudoValue = {
	type: "CHOICE",
	members: [{ type: "SYMBOL", name: "_selector" }, { type: "REPEAT1", content: { type: "SYMBOL", name: "_value" } }]
};

assertGrammarShape(
	grammar.rules.pseudo_class_arguments.members[1],
	{
		type: "CHOICE",
		members: [
			{
				type: "SEQ",
				members: [pseudoValue, { type: "REPEAT", content: { type: "SEQ", members: [{ type: "STRING", value: "," }, pseudoValue] } }]
			},
			{ type: "BLANK" }
		]
	},
	"grammar.rules.pseudo_class_arguments.members.1"
);

assertGrammarShape(
	grammar.rules.list_value.members[1],
	{
		type: "SEQ",
		members: [
			{ type: "SYMBOL", name: "_value" },
			{ type: "REPEAT1", content: { type: "SEQ", members: [{ type: "STRING", value: "," }, { type: "SYMBOL", name: "_value" }] } }
		]
	},
	"grammar.rules.list_value.members.1"
);

const selector = grammar.rules.keyframe_block.members[0];

// 小数の位置とカンマで並べる位置の文法への拡張
grammar.rules.float_value.members[0].content = { type: "PATTERN", value: NUM_PATTERN };
selector.members.push({ type: "SYMBOL", name: "float_value" });

grammar.rules.keyframe_block.members[0] = {
	type: "SEQ",
	members: [selector, { type: "REPEAT", content: { type: "SEQ", members: [{ type: "STRING", value: "," }, selector] } }]
};

// 属性名の位置だけ値より優先し，引用符内の空白を属性値へ残す
grammar.rules._attribute_identifier = { type: "TOKEN", content: { type: "PREC", value: 1, content: grammar.rules.identifier } };
grammar.rules.attribute_selector.members[2].content.members[0] = { type: "SYMBOL", name: "_attribute_identifier" };

grammar.rules._attribute_namespace = {
	type: "PREC_LEFT",
	value: 1,
	content: {
		type: "SEQ",
		members: [
			{ type: "ALIAS", content: { type: "SYMBOL", name: "_attribute_identifier" }, named: true, value: "tag_name" },
			{ type: "STRING", value: "|" },
			{ type: "ALIAS", content: { type: "SYMBOL", name: "identifier" }, named: true, value: "tag_name" }
		]
	}
};

grammar.rules.attribute_selector.members[2].content.members.push({ type: "SYMBOL", name: "_attribute_namespace" });

// 変数の末尾の指定は値と区別し，両指定の順と併用を保持する
grammar.rules.variable_flag =
{ type: "CHOICE", members: [{ type: "STRING", value: "!default" }, { type: "STRING", value: "!global" }] };

for(const name of ["declaration", "last_declaration"]) {
	const declaration = name === "declaration" ? grammar.rules[name] : grammar.rules[name].content;
	declaration.members[4].members.unshift({ type: "REPEAT1", content: { type: "SYMBOL", name: "variable_flag" } });
}

// モジュールの別名・再公開する名前・設定は独立した節として読む
grammar.rules.module_alias =
{ type: "SEQ", members: [{ type: "STRING", value: "as" }, { type: "SYMBOL", name: "module_name" }] };

grammar.rules.module_name = { type: "PATTERN", value: "(?:--|-?[a-zA-Z_])[a-zA-Z0-9-_]*\\*?|\\*" };

grammar.rules.module_members = {
	type: "SEQ",
	members: [
		{ type: "CHOICE", members: [{ type: "STRING", value: "hide" }, { type: "STRING", value: "show" }] },
		{ type: "CHOICE", members: [{ type: "SYMBOL", name: "identifier" }, { type: "SYMBOL", name: "variable" }] },
		{
			type: "REPEAT",
			content: {
				type: "SEQ",
				members: [
					{ type: "STRING", value: "," },
					{ type: "CHOICE", members: [{ type: "SYMBOL", name: "identifier" }, { type: "SYMBOL", name: "variable" }] }
				]
			}
		}
	]
};

grammar.rules.module_configuration =
{ type: "SEQ", members: [{ type: "STRING", value: "with" }, { type: "SYMBOL", name: "parameters" }] };

for(const name of ["forward_statement", "use_statement"]) {
	const clauses = name === "forward_statement" ?
	["module_alias", "module_members", "module_configuration"] :
	["module_alias", "module_configuration"];
	grammar.rules[name].members.splice(
		2,
		0,
		...clauses.map(clause => ({ type: "CHOICE", members: [{ type: "SYMBOL", name: clause }, { type: "BLANK" }] }))
	);
}

// include の修飾名は１トークンにし，既に読める変数・関数の形は保つ
grammar.rules.qualified_mixin_name = { type: "PATTERN", value: "(--|-?[a-zA-Z_])[a-zA-Z0-9-_]*\\.[a-zA-Z_][a-zA-Z0-9-_]*" };

grammar.rules.include_statement.members[1] =
{ type: "CHOICE", members: [grammar.rules.include_statement.members[1], { type: "SYMBOL", name: "qualified_mixin_name" }] };

// 相対セレクタは規則と擬似クラスの入口に限り，既存の結合子ノードへ読む
grammar.rules._relative_selector = { type: "CHOICE", members: [] };

for(const name of ["adjacent_sibling_selector", "child_selector", "sibling_selector"]) {
	const relativeName = "_relative_" + name;
	grammar.rules[relativeName] = { type: "SEQ", members: grammar.rules[name].content.members.slice(1) };
	grammar.rules._relative_selector.members.push(
		{ type: "ALIAS", content: { type: "SYMBOL", name: relativeName }, named: true, value: name }
	);
}

const selectorChoice =
{ type: "CHOICE", members: [{ type: "SYMBOL", name: "_selector" }, { type: "SYMBOL", name: "_relative_selector" }] };

grammar.rules.selectors.members[0] = selectorChoice;
grammar.rules.selectors.members[1].content.members[1] = selectorChoice;
// 相対セレクタの引数は has に限定し，An+B 等の値の引数の読みを保つ
grammar.rules._has_arguments = structuredClone(grammar.rules.pseudo_class_arguments);

const hasElements = grammar.rules._has_arguments.members[1].members[0];

hasElements.members[0].members[0] = selectorChoice;
hasElements.members[1].content.members[1].members[0] = selectorChoice;
grammar.rules._has_selector = structuredClone(grammar.rules.pseudo_class_selector);
grammar.rules._has_selector.members[2].content = { type: "STRING", value: "has" };
grammar.rules._has_selector.members[3].members[0].content = { type: "SYMBOL", name: "_has_arguments" };

grammar.rules._selector.members.push(
	{ type: "ALIAS", content: { type: "SYMBOL", name: "_has_selector" }, named: true, value: "pseudo_class_selector" }
);

// extend の対象として，既存のプレースホルダセレクタも受理する
grammar.rules.extend_statement.members[1].members.push({ type: "SYMBOL", name: "placeholder" });

// 未知のアット規則の関数を条件として読み，既存の条件の優先順位を保つ
grammar.rules._at_query =
{ type: "CHOICE", members: [{ type: "SYMBOL", name: "_query" }, { type: "SYMBOL", name: "call_expression" }] };

grammar.rules.at_rule.members[1].members[0].members[0] = { type: "SYMBOL", name: "_at_query" };
grammar.rules.at_rule.members[1].members[0].members[1].content.members[1] = { type: "SYMBOL", name: "_at_query" };

// 括弧の並びの末尾カンマを受理し，単一要素のリストも値と区別する
for(const name of ["_include_arguments", "arguments", "list_value", "parameters"]) {
	grammar.rules[name].members.splice(2, 0, { type: "CHOICE", members: [{ type: "STRING", value: "," }, { type: "BLANK" }] });
}

grammar.rules.list_value.members[1].members[1].type = "REPEAT";
grammar.rules.list_value = { type: "PREC", value: -2, content: grammar.rules.list_value };
// 空のリストを幅０の数値へ読み替えず，同じリストのノードへ読む
grammar.rules._empty_list_value = { type: "SEQ", members: [{ type: "STRING", value: "(" }, { type: "STRING", value: ")" }] };

grammar.rules._value.members.push(
	{ type: "ALIAS", content: { type: "SYMBOL", name: "_empty_list_value" }, named: true, value: "list_value" }
);

// 空の実引数・仮引数と，閉括弧前の空白は欠落した値として読まない
for(const name of ["_include_arguments", "parameters"]) {
	const members = grammar.rules[name].members;
	members[1] = { type: "CHOICE", members: [members[1], { type: "BLANK" }] };
	members[members.length - 1] = { type: "STRING", value: ")" };
}

// 空白で並べる値を，仮引数の既定値と include の実引数でも受理する
grammar.rules.parameter.members[1].members[0].members[1].content =
{ type: "REPEAT1", content: { type: "SYMBOL", name: "_value" } };

grammar.rules._include_argument.members[1].content = { type: "REPEAT1", content: { type: "SYMBOL", name: "_value" } };
// 再公開の設定だけに許される default の指定は，通常の仮引数へ広げない
grammar.rules._module_parameter = structuredClone(grammar.rules.parameter);

grammar.rules._module_parameter.members.push(
	{
		type: "CHOICE",
		members:
		[{ type: "ALIAS", content: { type: "STRING", value: "!default" }, named: true, value: "variable_flag" }, { type: "BLANK" }]
	}
);

grammar.rules._module_parameters = structuredClone(grammar.rules.parameters);

const moduleParams = grammar.rules._module_parameters.members[1].members[0];
const moduleParam = { type: "ALIAS", content: { type: "SYMBOL", name: "_module_parameter" }, named: true, value: "parameter" };

moduleParams.members[0] = moduleParam;
moduleParams.members[1].content.members[1] = moduleParam;
grammar.rules._forward_configuration = structuredClone(grammar.rules.module_configuration);

grammar.rules._forward_configuration.members[1] =
{ type: "ALIAS", content: { type: "SYMBOL", name: "_module_parameters" }, named: true, value: "parameters" };

grammar.rules.forward_statement.members[4].members[0] =
{ type: "ALIAS", content: { type: "SYMBOL", name: "_forward_configuration" }, named: true, value: "module_configuration" };

// 名前のエスケープは，１６進の終端の空白も同じトークンとして読む
const NAME_ESCAPE = "\\\\(?:[0-9a-fA-F]{1,6}(?:\\r\\n|[ \\t\\n\\r\\f])?|[^0-9a-fA-F\\n\\r\\f])";

grammar.rules.identifier.value = `(?:--|-?(?:[a-zA-Z_]|${NAME_ESCAPE}))(?:[a-zA-Z0-9_-]|${NAME_ESCAPE})*`;

// 否定の被演算子は値として読み，条件・返戻・実引数にも同じ式を使う
grammar.rules.unary_expression = {
	type: "PREC_RIGHT",
	value: 1,
	content: { type: "SEQ", members: [{ type: "STRING", value: "not" }, { type: "SYMBOL", name: "_value" }] }
};

grammar.rules._value.members.push({ type: "SYMBOL", name: "unary_expression" });

// 論理演算は既存の二項式へ読み，通常の二項式より低い優先順位を持たせる
for(const [operator, precedence] of [["and", -3], ["or", -4]]) {
	const name = "_logical_" + operator;
	grammar.rules[name] = {
		type: "PREC_LEFT",
		value: precedence,
		content: {
			type: "SEQ",
			members: [{ type: "SYMBOL", name: "_value" }, { type: "STRING", value: operator }, { type: "SYMBOL", name: "_value" }]
		}
	};
	grammar.rules._value.members.push(
		{ type: "ALIAS", content: { type: "SYMBOL", name }, named: true, value: "binary_expression" }
	);
}

// 関数の名前付実引数だけを argument とし，既存の位置実引数の木は保つ
grammar.rules._keyword_argument = {
	type: "SEQ",
	members: [
		{ type: "FIELD", name: "name", content: { type: "SYMBOL", name: "variable" } },
		{ type: "STRING", value: ":" },
		{ type: "FIELD", name: "value", content: { type: "REPEAT1", content: { type: "SYMBOL", name: "_value" } } }
	]
};

const functionArgs = grammar.rules.arguments.members[1].members[0];

const keywordArgument =
{ type: "ALIAS", content: { type: "SYMBOL", name: "_keyword_argument" }, named: true, value: "argument" };

functionArgs.members[0] = { type: "CHOICE", members: [functionArgs.members[0], keywordArgument] };

functionArgs.members[1].content.members[1] =
{ type: "CHOICE", members: [functionArgs.members[1].content.members[1], keywordArgument] };

// 可変長の仮引数と展開する実引数は，既存の仮引数・実引数の役割へ読む
grammar.rules.parameter = {
	type: "CHOICE",
	members:
	[grammar.rules.parameter, { type: "SEQ", members: [{ type: "SYMBOL", name: "variable" }, { type: "STRING", value: "..." }] }]
};

grammar.rules._spread_argument = {
	type: "SEQ",
	members: [{ type: "FIELD", name: "value", content: { type: "SYMBOL", name: "_value" } }, { type: "STRING", value: "..." }]
};

const spreadArgument = { type: "ALIAS", content: { type: "SYMBOL", name: "_spread_argument" }, named: true, value: "argument" };

functionArgs.members[0].members.push(spreadArgument);
functionArgs.members[1].content.members[1].members.push(spreadArgument);

grammar.rules._include_argument =
{ type: "CHOICE", members: [grammar.rules._include_argument, grammar.rules._spread_argument] };

// 拡張した文法の出力
fileSystem.mkdirSync(output, { recursive: true });
fileSystem.writeFileSync(path.join(output, "grammar.json"), JSON.stringify(grammar, null, 2) + "\n");
