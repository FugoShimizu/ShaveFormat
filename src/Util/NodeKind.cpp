#include "Util/NodeKind.hpp"
#include <algorithm>
#include <string_view>
#include <vector>

/** ========== 集合操作 ========== */
/**
 * ハッシュ値算出関数
 * @param Type 対象文字列
 * @return ハッシュ値
 */
size_t NodeKind::NodeTypeSet::HashOf(const std::string_view Type) {
	// FNV-1a ６４ビット：オフセット基底に各バイトを XOR して素数を乗算
	uint64_t Hash = 14695981039346656037ULL;
	for(const char Char : Type) Hash = 1099511628211ULL * (Hash ^ static_cast<unsigned char>(Char));
	// 計算済ハッシュ値の返戻
	return static_cast<size_t>(Hash);
}

/**
 * コンストラクタ
 * @param Init 候補要素
 */
NodeKind::NodeTypeSet::NodeTypeSet(const std::initializer_list<std::string_view> Init) : FirstCharMask(0), SizeMask(0) {
	// 各要素の先頭文字ビット＋長さビットを立てた６４ビット集合を事前計算（検索時の早期除外用）
	for(const std::string_view Item : Init) if(!Item.empty()) {
		FirstCharMask |= 1ULL << (static_cast<unsigned char>(Item[0]) & 0X3F);
		SizeMask |= 1ULL << std::min<size_t>(Item.size(), 63);
	}
	// ５要素以上は開番地表，４要素以下は Init を線形探索する
	if(Init.size() > 4) {
		size_t Capacity = 8;
		while(Capacity < Init.size() << 1) Capacity <<= 1;
		Slots.assign(Capacity, std::string_view());
		for(const std::string_view Item : Init) {
			size_t Idx = HashOf(Item) & Capacity - 1;
			while(!Slots[Idx].empty()) Idx = Idx + 1 & Capacity - 1;
			Slots[Idx] = Item;
		}
	} else Slots.assign(Init.begin(), Init.end());
	// 終了
	return;
}

/**
 * 包含判定関数
 * @param Type 判定対象
 * @return 含まれるなら true
 */
bool NodeKind::NodeTypeSet::Contains(const std::string_view Type) const {
	// 寸法と先頭字に依る事前検査
	if(
		const size_t Len = Type.size();
		Len > 63 || !(SizeMask >> Len & 0X1) || !(FirstCharMask >> (static_cast<unsigned char>(Type[0]) & 0X3F) & 0X1)
		// 空文字列は寸法ビットで短絡し，先頭を読まない
	) return false;
	// ５要素以上は空席到達で不在を確定する開番地表を使う
	if(Slots.size() > 4) {
		const size_t Mask = Slots.size() - 1;
		for(size_t Idx = HashOf(Type) & Mask;; Idx = Idx + 1 & Mask) {
			const std::string_view Slot = Slots[Idx];
			// 空席へ到達した為，不在の返戻
			if(Slot.empty()) return false;
			// 一致する要素が見付かった為，所属の返戻
			if(Slot == Type) return true;
		}
	}
	// １〜４要素は分岐予測・局所性を活かす線形探索で，一致時に true の返戻
	for(const std::string_view Item : Slots) if(Item == Type) return true;
	// 何れの要素にも一致しない為 false の返戻
	return false;
}

/**
 * 節点の型の包含判定関数
 * @param Node 判定対象の節点
 * @return 型が含まれるなら true
 */
bool NodeKind::NodeTypeSet::Contains(const TSNode Node) const {
	// 空節点は型を持たず，型の問合せが構文木を辿って落ちる為，含まないと答える事の返戻
	if(ts_node_is_null(Node)) return false;
	// 型の名前の包含判定の返戻
	return Contains(std::string_view(ts_node_type(Node)));
}

/** ========== 節点型集合 ========== */
// Java / C# / PHP のアクセス修飾子順序検査対象メンバ宣言（`Lint::CheckMemberAccessOrder` の `IsMember` 判定）
const NodeKind::NodeTypeSet NodeKind::AccessOrderMember =
{ "const_declaration", "constructor_declaration", "field_declaration", "method_declaration", "property_declaration" };

// 型の本体とは限らない本体
const NodeKind::NodeTypeSet NodeKind::AmbiguousBodyContainer = { "block_body", "body_statement", "declaration_list" };

// `<` `>` で囲まれる型／値リスト（`<` 直接連結＋内側スペース判定の共通集合）
const NodeKind::NodeTypeSet NodeKind::AngleBracketList = {
	"template_argument_list",
	"template_parameter_list",
	"type_argument_list",
	"type_arguments",
	"type_parameter_list",
	"type_parameters"
};

// 関数型・ラムダ・match の腕を成す言語横断の矢印字句
const NodeKind::NodeTypeSet NodeKind::ArrowToken = { "->", "=>" };

// Kotlin と他言語を同じ変更文脈へ揃える代入式
const NodeKind::NodeTypeSet NodeKind::AssignmentExpression =
{ "assignment", "assignment_expression", "augmented_assignment_expression" };

// await・try・型キャストの式
const NodeKind::NodeTypeSet NodeKind::AwaitTryCastExpression = { "await_expression", "try_expression", "type_cast_expression" };
// Java の寸法と C++ のラムダ捕獲で同じ隣接規則を使う `[` 直接連結ノード
const NodeKind::NodeTypeSet NodeKind::AttachBracket = { "dimensions", "lambda_capture_specifier" };

// `:` 又は `.` を直前トークンに直接連結するノード
const NodeKind::NodeTypeSet NodeKind::AttachColonOrDot =
{ "asserts_annotation", "navigation_suffix", "trait_bounds", "type_annotation", "type_predicate_annotation" };

// `(` を直接連結する引数／パラメータリスト
const NodeKind::NodeTypeSet NodeKind::AttachParen = {
	"argument_list",
	"arguments",
	"formal_parameters",
	"function_value_parameters",
	"lambda_declarator",
	"method_parameters",
	"parameter_list",
	"parameters",
	"parenthesized_expression",
	"primary_constructor",
	"tuple",
	"value_arguments"
};

// 後続要素に付く属性・注釈・デコレータ
const NodeKind::NodeTypeSet NodeKind::AttachPrefix =
{ "annotation", "attribute_item", "attribute_list", "decorator", "marker_annotation", "modifiers" };

// auto を型名の代わりに書く外ない C++ の親（ジェネリックラムダの仮引数・範囲 for）
const NodeKind::NodeTypeSet NodeKind::AutoParameterHost = { "for_range_loop", "parameter_declaration" };
// コロンを持たない行コメント形の指令（`//` 直後へ空白を入れると機能が壊れる）
const NodeKind::NodeTypeSet NodeKind::BareDirective = { "export", "extern", "line", "nolint", "sys", "sysnb" };

// 制御構文の本体で波括弧を保持する宣言（言語間で統一）
const NodeKind::NodeTypeSet NodeKind::BodyDeclaration = {
	"abstract_class_declaration",
	"alias_declaration",
	"class_declaration",
	"class_specifier",
	"declaration",
	"enum_declaration",
	"enum_specifier",
	"function_declaration",
	"function_definition",
	"function_static_declaration",
	"generator_function_declaration",
	"global_declaration",
	"interface_declaration",
	"lexical_declaration",
	"local_declaration_statement",
	"local_function_statement",
	"local_variable_declaration",
	"namespace_alias_definition",
	"property_declaration",
	"record_declaration",
	"static_assert_declaration",
	"struct_specifier",
	"trait_declaration",
	"type_alias_declaration",
	"type_definition",
	"union_specifier",
	"using_declaration",
	"variable_declaration"
};

// 真偽値型の初期値を持ち得る宣言・代入ノード（`Edit::MaybeCollectBoolLiteralEdit` の振分）
const NodeKind::NodeTypeSet NodeKind::BoolInitContext = { "assignment", "declaration", "field_declaration" };
// `{` を型名直後に連結するリテラル
const NodeKind::NodeTypeSet NodeKind::BraceAttach = { "composite_literal", "compound_literal_expression" };

// 閉じ波括弧の後に続き本体の塊を持つ節（else・catch・finally の節）次の行に作用する指令の錨を同じ行の本体の文へ降ろす
const NodeKind::NodeTypeSet NodeKind::BraceClause =
{ "catch_block", "catch_clause", "else", "else_clause", "else_if_clause", "elsif", "finally_clause" };

// 空の本体を語へ密着させる型定義ノード（成員を持つ本体は展開して空白で区切る）
// Go の `interface{}` / Go の `struct{}`
const NodeKind::NodeTypeSet NodeKind::BraceNoSpace = { "interface_type", "struct_type" };
// 本体の波括弧を外さない文（言語仕様が波括弧を要し，外すと再解析で復活して不動点へ到達しない）
const NodeKind::NodeTypeSet NodeKind::BraceRemoveStmtExcluded = { "catch_clause", "repeat_while_statement" };
// 本体へ波括弧を付けない文（Swift の `try <式>` は付けると再解析でラムダへ化け，`repeat { } while` はネストが増え続ける）
const NodeKind::NodeTypeSet NodeKind::BraceWrapStmtExcluded = { "repeat_while_statement", "try_expression" };

// 括弧の対応付を壊す不透明範囲は内部へ降りない
const NodeKind::NodeTypeSet NodeKind::BracketOpaque = {
	"char_literal",
	"character_literal",
	"interpolated_string_expression",
	"jsx_text",
	"multi_line_string_literal",
	"plain_value",
	"preproc_arg",
	"raw_string_literal",
	"regex",
	"regex_literal",
	"rune_literal",
	"string_literal",
	"text",
	"text_interpolation",
	"verbatim_string_literal"
};

// `[` 始まりの型ノード（Go / Rust / Swift の配列・スライス型と Swift の辞書型 — `[` 直前の隣接判定）
const NodeKind::NodeTypeSet NodeKind::BracketType = { "array_type", "dictionary_type", "slice_type" };
// Kotlin のラベル付 `break@` を含む脱出の語
const NodeKind::NodeTypeSet NodeKind::BreakKeyword = { "break", "break@" };
// 改行で意味が変わる字句列・隣接連結の保護（任意字句列の Rust の sql!{...}・macro_rules! の型も式評価対象外）
const NodeKind::NodeTypeSet NodeKind::BreakOpaque = { "chained_string", "rescue_modifier", "token_tree" };

// C/C++ キャスト `(type)` 内側に出現し得る型部位（括弧削除抑止対象，`(counter_t)-1` 等を二項式と誤判定する事の回避）
const NodeKind::NodeTypeSet NodeKind::CCastInner = {
	"abstract_pointer_declarator",
	"pointer_declarator",
	"primitive_type",
	"qualified_identifier",
	"sized_type_specifier",
	"splice_type_specifier",
	"template_type",
	"type_identifier"
};

// C/C++ binary_expression 被演算子の `(type)` 中身が型名相当と看做せる単独子（単項演算子前の括弧をキャストと推定する用）
const NodeKind::NodeTypeSet NodeKind::CCastTypeNameInner =
{ "identifier", "primitive_type", "sized_type_specifier", "type_descriptor", "type_identifier" };

// C の宣言子を包む宣言子（関数定義の宣言子が関数の宣言子かを見る時に剥がす）
const NodeKind::NodeTypeSet NodeKind::CDeclaratorWrapper =
{ "attributed_declarator", "parenthesized_declarator", "pointer_declarator" };

// C/C++ の関数宣言子経路及び欄のメソッド判定用宣言子
const NodeKind::NodeTypeSet NodeKind::CFunctionOrFieldDeclarator =
{ "function_declarator", "init_declarator", "pointer_declarator", "reference_declarator" };

// 名前を宣言する C / C++ の宣言（宣言子の子を名前迄剥がして，同じファイルで宣言した名前を集める）
const NodeKind::NodeTypeSet NodeKind::CNameDeclaration = { "declaration", "parameter_declaration" };

// `(` で始まり得る C の宣言子（型との間を空白で区切り，`int(*p)` を関数形式のキャストの字面にしない）
const NodeKind::NodeTypeSet NodeKind::CParenDeclarator =
{ "array_declarator", "function_declarator", "init_declarator", "parenthesized_declarator" };

// C# の宣言で宣言子の並び (variable_declaration) の前に並ぶ修飾子・属性（型の頭の一部）
const NodeKind::NodeTypeSet NodeKind::CSharpDeclPrefix = { "attribute_list", "modifier" };

// マクロの置換本体で制御文を始め得る C / C++ の語
const NodeKind::NodeTypeSet NodeKind::CStatementKeyword =
{ "break", "continue", "do", "for", "goto", "if", "return", "switch", "while" };

// Java の矢印節と C 系のコロン節を同じ本体境界へ揃える字句
const NodeKind::NodeTypeSet NodeKind::CaseBodyOpener = { "->", ":" };

// switch の節（言語横断）
const NodeKind::NodeTypeSet NodeKind::CaseClause = {
	"case_statement",
	"default_statement",
	"switch_block_statement_group",
	"switch_entry",
	"switch_rule",
	"switch_section",
	"when_entry"
};

// C 系の括弧キャストと Go の変換呼出を同じ密着判定へ渡すキャスト式
const NodeKind::NodeTypeSet NodeKind::CastExpr = { "cast_expression", "type_cast_expression" };

// クラス本体ノード（アクセス修飾子順序検査対象）
const NodeKind::NodeTypeSet NodeKind::ClassBody =
{ "class_body", "declaration_list", "enum_body_declarations", "field_declaration_list" };

// クラス／構造体／インターフェース等のメンバを直接保持するコンテナ
const NodeKind::NodeTypeSet NodeKind::ClassBodyContainer = {
	"annotation_type_body",
	"body_statement",
	"class_body",
	"declaration_list",
	"enum_body",
	"enum_body_declarations",
	"enum_class_body",
	"field_declaration_list",
	"interface_body",
	"protocol_body"
};

// クラス系コンテナノード（`class` / `struct` / `interface` / `enum` / `module` / `namespace` 等）全言語横断
const NodeKind::NodeTypeSet NodeKind::ClassLike = {
	"class",
	"class_declaration",
	"class_definition",
	"class_specifier",
	"companion_object",
	"enum_declaration",
	"enum_item",
	"extension_declaration",
	"impl_item",
	"interface_declaration",
	"mod_item",
	"module",
	"namespace_declaration",
	"namespace_definition",
	"object_declaration",
	"protocol_declaration",
	"singleton_class",
	"struct_declaration",
	"struct_item",
	"struct_specifier",
	"trait_item",
	"union_specifier"
};

// アクセス指定順を検査する class・struct
const NodeKind::NodeTypeSet NodeKind::ClassLikeStrict = { "class_declaration", "class_specifier", "struct_specifier" };
// 末尾コメントを最後の本体の後へ置く節
const NodeKind::NodeTypeSet NodeKind::ClauseBody = { "colon_block", "else_clause", "else_if_clause" };

// コメントを塊末尾へ留める閉じ字句（LayoutCloserToken と PhpAltCloser の和，何方かへの追加は此処にも反映）
const NodeKind::NodeTypeSet NodeKind::CloserToken =
{ ")", "/>", "</", ">", "]", "end", "enddeclare", "endfor", "endforeach", "endif", "endswitch", "endwhile", "}" };

// CmpBoundary Lint 対象（全言語横断の比較式／中置式ノード集合）
const NodeKind::NodeTypeSet NodeKind::CmpBoundaryNode =
{ "binary", "binary_expression", "comparison_expression", "comparison_operator", "infix_expression" };

// `:` 前スペース不要な制御構文（case／ラベル／ラムダ等）
const NodeKind::NodeTypeSet NodeKind::ColonControl = {
	"access_specifier",
	"case_clause",
	"case_statement",
	"class_definition",
	"communication_case",
	"default_case",
	"default_statement",
	"elif_clause",
	"else_clause",
	"except_clause",
	"expression_case",
	"finally_clause",
	"for_statement",
	"function_definition",
	"if_statement",
	"labeled_statement",
	"lambda",
	"match_statement",
	"switch_block_statement_group",
	"switch_case",
	"switch_default",
	"switch_entry",
	"switch_label",
	"switch_section",
	"try_statement",
	"type_case",
	"while_statement",
	"with_statement"
};

// CSS / SCSS の子孫・子・隣接兄弟・一般兄弟の結合子（子の `:where` 等も別要素の為，先頭 `:` を密着させず空白保持）
const NodeKind::NodeTypeSet NodeKind::CombinatorSelector =
{ "adjacent_sibling_selector", "child_selector", "descendant_selector", "sibling_selector" };

// コメントノード
const NodeKind::NodeTypeSet NodeKind::Comment =
{ "block_comment", "comment", "doc_comment", "html_comment", "js_comment", "line_comment", "multiline_comment" };

// 内部スペース・改行を入れない密集型ノード（属性／単項演算子等）
const NodeKind::NodeTypeSet NodeKind::Compact = {
	"default_parameter",
	"jsx_attribute",
	"keyword_argument",
	"pointer_expression",
	"prefix_unary_expression",
	"property_signature",
	"public_field_definition",
	"unary",
	"unary_expression"
};

// `Compact` に含まれるが内部に分割対象を持ち得る為，`LineSplit::IsCompactNode` で `false` 返戻させて再帰経路に乗せる
const NodeKind::NodeTypeSet NodeKind::CompactRecurse = { "jsx_attribute", "property_signature" };

// `Lint::CheckZeroComparisonCondition` 用の条件親（制御構文に加え `{x && <C/>}` の JSX 式コンテナも真偽文脈）
const NodeKind::NodeTypeSet NodeKind::CondContextParent = {
	"conditional_expression",
	"do_statement",
	"for_statement",
	"if_statement",
	"jsx_expression",
	"ternary_expression",
	"while_statement"
};

// 条件式を包む括弧ノード（Lint の条件文脈剥がし）
const NodeKind::NodeTypeSet NodeKind::CondParenWrap = { "condition_clause", "parenthesized_expression" };
// 条件を場 `condition` に持つ節点（条件以外の子は条件の文脈でない）
const NodeKind::NodeTypeSet NodeKind::ConditionFieldHost = { "conditional_expression", "for_statement", "ternary_expression" };
// else の位置で自身が条件と本体を持つ分岐（else 節に包まれない else if の if と，PHP の elseif 節）
const NodeKind::NodeTypeSet NodeKind::ConditionalAlternative = { "else_if_clause", "if_expression", "if_statement" };

// 生成を最初の子に持つ後置の式（空の実引数を外すと後置の演算子が生成の対象の名前へ掛かる）
const NodeKind::NodeTypeSet NodeKind::ConstructionReceiverHost = {
	"call_expression",
	"class_constant_access_expression",
	"function_call_expression",
	"member_access_expression",
	"member_call_expression",
	"member_expression",
	"new_expression",
	"non_null_expression",
	"nullsafe_member_access_expression",
	"nullsafe_member_call_expression",
	"scoped_call_expression",
	"scoped_property_access_expression",
	"subscript_expression"
};

// 継続の語（Kotlin はラベル付を `continue@` の字句で持つ）
// Swift/Kt continue // Kt continue@outer
const NodeKind::NodeTypeSet NodeKind::ContinueKeyword = { "continue", "continue@" };

// 制御構文ノード（条件 `()` の括弧維持対象，括弧保持集合の構成要素）
const NodeKind::NodeTypeSet NodeKind::Control = {
	"begin",
	"case",
	"case_match",
	"catch_block",
	"catch_clause",
	"do",
	"do_block",
	"do_statement",
	"do_while_statement",
	"else",
	"else_clause",
	"else_if_clause",
	"enhanced_for_statement",
	"except_clause",
	"expression_switch_statement",
	"finally_block",
	"finally_clause",
	"for",
	"for_expression",
	"for_in_statement",
	"for_range_loop",
	"for_statement",
	"foreach_statement",
	"guard_statement",
	"if",
	"if_expression",
	"if_statement",
	"loop_expression",
	"match_expression",
	"match_statement",
	"repeat_while_statement",
	"seh_except_clause",
	"seh_finally_clause",
	"seh_try_statement",
	"select_statement",
	"switch_expression",
	"switch_statement",
	"try_block",
	"try_expression",
	"try_statement",
	"try_with_resources_statement",
	"type_switch_statement",
	"unless",
	"until",
	"when_expression",
	"while",
	"while_expression",
	"while_statement",
	"with_statement"
};

// 制御キーワード
const NodeKind::NodeTypeSet NodeKind::ControlKeyword =
{ "catch", "elif", "elsif", "except", "for", "foreach", "if", "range", "switch", "when", "while" };

// C++ の組込型
const NodeKind::NodeTypeSet NodeKind::CppBuiltinType = { "primitive_type", "sized_type_specifier" };
// C / C++ の `constexpr` 候補判定で初期化子に許すリテラル（`Lint::CheckConstexprCandidate`）
const NodeKind::NodeTypeSet NodeKind::CppConstLiteral = { "char_literal", "false", "number_literal", "string_literal", "true" };

// C/C++ 宣言の型部位（後置 const 検出時の型認識対象）
const NodeKind::NodeTypeSet NodeKind::CppDeclTypeSpecifier =
{ "primitive_type", "qualified_identifier", "sized_type_specifier", "type_identifier" };

// 宣言の先頭に置ける C++ の名前・型の節点（型と関数の名前解決を仮定せず，宣言に使える構文で判定する）
const NodeKind::NodeTypeSet NodeKind::CppDeclarationName = {
	"decltype",
	"dependent_name",
	"identifier",
	"placeholder_type_specifier",
	"primitive_type",
	"qualified_identifier",
	"template_function",
	"template_type",
	"type_identifier"
};

// 構文木で異なる節点型を持つ `NULL` と `nullptr` の候補
const NodeKind::NodeTypeSet NodeKind::CppNullCandidate = { "identifier", "null" };
// C++ の型引数の遡りを止める範囲（文の本体とファイル）
const NodeKind::NodeTypeSet NodeKind::CppScopeBoundary = { "compound_statement", "translation_unit" };
// 型名を書けない C++ の初期化子（auto の検査の例外）
const NodeKind::NodeTypeSet NodeKind::CppUnnamableInitializer = { "lambda_expression", "structured_binding_declarator" };
// キャストにも式にも読める C# の単純名と限定名
const NodeKind::NodeTypeSet NodeKind::CsNameType = { "identifier", "qualified_name" };

// C# の後置式の連鎖（後置 `!` を含む）
const NodeKind::NodeTypeSet NodeKind::CsPostfixChain =
{ "element_access_expression", "invocation_expression", "member_access_expression", "postfix_unary_expression" };

// 断片と補間 `#{...}` を密着させる CSS/SCSS の名前
const NodeKind::NodeTypeSet NodeKind::CssNameFragmentHost = { "property_name", "tag_name" };
// 名前空間の区切り `|` を持つ CSS の選択子の節点（`|` は選択子の字句の一部で空白を置けない）
const NodeKind::NodeTypeSet NodeKind::CssNamespacePrefix = { "attribute_name", "namespace_selector" };
// 整数と小数を直後の単位へ密着させる CSS/SCSS の数値値ノード
const NodeKind::NodeTypeSet NodeKind::CssNumericValue = { "float_value", "integer_value" };
// CSS/SCSS の分割不可セレクタ系（内部空白挿入禁止）
const NodeKind::NodeTypeSet NodeKind::CssSelectorAtomic = { "class_name", "id_name", "placeholder" };

// 選択子を取る節点（中の `#name` は色でなく ID 選択子で，大小を変えると別の要素を指す）
const NodeKind::NodeTypeSet NodeKind::CssSelectorHost =
{ "extend_statement", "pseudo_class_selector", "pseudo_element_selector" };

// CSS の選択子の名前（`&` や `.` へ密着させる）
// CSS .a // CSS #a
const NodeKind::NodeTypeSet NodeKind::CssSelectorName = { "class_name", "id_selector" };

// 同型一括宣言を内包し得るコンテナ
const NodeKind::NodeTypeSet NodeKind::DeclContainer = {
	"annotation_type_body",
	"arrow_function",
	"block",
	"case",
	"catch_clause",
	"class_body",
	"class_declaration",
	"class_specifier",
	"compound_statement",
	"constructor_body",
	"declaration_list",
	"do_statement",
	"else_clause",
	"enum_body",
	"field_declaration_list",
	"finally_clause",
	"for_statement",
	"function_body",
	"function_declaration",
	"function_definition",
	"function_item",
	"if_statement",
	"interface_body",
	"lambda_expression",
	"linkage_specification",
	"method_declaration",
	"method_definition",
	"namespace_definition",
	"program",
	"source_file",
	"statement_block",
	"struct_specifier",
	"switch_body",
	"translation_unit",
	"try_statement",
	"while_statement"
};

// 宣言子（変数名＋任意の初期化子）ノード
const NodeKind::NodeTypeSet NodeKind::DeclDeclaratorChild = {
	"abstract_array_declarator",
	"abstract_pointer_declarator",
	"abstract_reference_declarator",
	"array_declarator",
	"field_identifier",
	"identifier",
	"init_declarator",
	"operator_cast",
	"pointer_declarator",
	"reference_declarator",
	"structured_binding_declarator",
	"variable_declarator"
};

// 初期化子を伴う宣言子（初期化子の有無が混在する宣言の判別に用いる）
const NodeKind::NodeTypeSet NodeKind::DeclInitDeclarator = { "init_declarator", "variable_declarator" };

// 一括宣言可能な宣言ノード
const NodeKind::NodeTypeSet NodeKind::DeclLike = {
	"declaration",
	"field_declaration",
	"lexical_declaration",
	"local_declaration_statement",
	"local_variable_declaration",
	"variable_declaration"
};

// 宣言の型部位ノード
const NodeKind::NodeTypeSet NodeKind::DeclTypeChild = {
	"array_type",
	"attribute_declaration",
	"attribute_list",
	"attribute_specifier",
	"auto",
	"boolean_type",
	"class_specifier",
	"decltype",
	"dependent_type",
	"enum_specifier",
	"floating_point_type",
	"function_pointer_type",
	"generic_name",
	"generic_type",
	"implicit_type",
	"integral_type",
	"modifiers",
	"ms_declspec_modifier",
	"nullable_type",
	"placeholder_type_specifier",
	"pointer_type",
	"predefined_type",
	"primitive_type",
	"qualified_identifier",
	"qualified_name",
	"ref_qualifier",
	"ref_type",
	"scoped_type_identifier",
	"sized_type_specifier",
	"storage_class_specifier",
	"struct_specifier",
	"template_type",
	"tuple_type",
	"type_identifier",
	"type_qualifier",
	"void_type"
};

// 括弧の有無で decltype の推論する型が変わる式（名前・メンバアクセスは括弧で包むと左辺値の参照に為る）
const NodeKind::NodeTypeSet NodeKind::DecltypeIdExpression = { "field_expression", "identifier", "qualified_identifier" };
// 既定の節の字句
const NodeKind::NodeTypeSet NodeKind::DefaultLabel = { "default", "default_keyword", "else" };
// 分割代入パターン（単一名を持たない為，文書化引数名抽出／let 変異解析で除外）
const NodeKind::NodeTypeSet NodeKind::DestructurePattern = { "array_pattern", "object_pattern" };
// 本体の後に条件を置く繰返（本体は最初の名前付子，最後の名前付子は条件）
const NodeKind::NodeTypeSet NodeKind::DoLoop = { "do_statement", "do_while_statement" };
// 文書化コメントのアンカーを１階層包む包装ノード（定義本体でなく包装側へ先行コメントが束縛される）
const NodeKind::NodeTypeSet NodeKind::DocWrapper = { "ambient_declaration", "export_statement", "template_declaration" };
// TsSource の行末コメント錨を親へ遡る時の上書除外（else 系）
// Ruby else // C/C++/Rust/Py/JS/TS else { }
const NodeKind::NodeTypeSet NodeKind::ElseAny = { "else", "else_clause" };
// `LineSplit::ApplyBraceWraps` で連鎖から除外する `else-if` の親候補
const NodeKind::NodeTypeSet NodeKind::ElseIfChainParent = { "else_clause", "if_statement" };
// if の後続の枝を始める字句
// 共通 else // PHP elseif
const NodeKind::NodeTypeSet NodeKind::ElseKeyword = { "else", "elseif" };

// else 系の節
const NodeKind::NodeTypeSet NodeKind::ElseLikeClause =
{ "catch_clause", "else", "else_clause", "else_if_clause", "elsif", "finally_clause" };

// 関数を抜ける語（語で始まる脱出の文の終端判定）
const NodeKind::NodeTypeSet NodeKind::ExitKeyword = { "return", "throw", "throw_keyword" };

// BuildExpandedContainer 経路に乗る括弧系コンテナ（statement_block 子孫を持つ場合に子を改行展開する対象）
const NodeKind::NodeTypeSet NodeKind::ExpandContainer = {
	"argument_list",
	"arguments",
	"condition_clause",
	"formal_parameters",
	"function_value_parameters",
	"jsx_expression",
	"method_parameters",
	"parameter_list",
	"parameters",
	"parenthesized_expression",
	"primary_constructor",
	"tuple_expression",
	"value_arguments"
};

// expression_statement 直下で外側括弧を保持するリテラル系（数値／文字列／論理値）
const NodeKind::NodeTypeSet NodeKind::ExprStmtLiteral =
{ "char_literal", "false", "float_literal", "integer_literal", "number", "string_literal", "true" };

// 被呼出側の欄の参照（括弧で包むと欄の持つ関数の呼出，外すとメソッドの呼出に為る言語の括弧の保持）
const NodeKind::NodeTypeSet NodeKind::FieldCallee = { "field_expression", "member_access_expression", "navigation_expression" };

// 制御フロー脱出文（同一退出文を持つ if の候補検出にも使用）
const NodeKind::NodeTypeSet NodeKind::FlowExit =
{ "break_statement", "continue_statement", "goto_statement", "return_statement", "throw_statement" };

// 反復の対象を区切 (`:` / `in` / `of`) の後に置く見出の文（見出の括弧は文の字句で，対象を包む括弧は冗長）
const NodeKind::NodeTypeSet NodeKind::ForEachHeader =
{ "enhanced_for_statement", "for_in_statement", "for_range_loop", "foreach_statement" };

// 関数本体ノード（言語横断）
const NodeKind::NodeTypeSet NodeKind::FunctionBodyContainer =
{ "block", "compound_statement", "constructor_body", "function_body", "statement_block" };

// 関数の範囲の境界（定義・関数式・ラムダ・局所関数，ネストの関数の中の `return` 等は外側の関数に属さない）
const NodeKind::NodeTypeSet NodeKind::FunctionLikeAny = {
	"anonymous_function",
	"anonymous_method_expression",
	"arrow_function",
	"closure_expression",
	"constructor_declaration",
	"func_literal",
	"function_declaration",
	"function_definition",
	"function_expression",
	"function_item",
	"generator_function",
	"generator_function_declaration",
	"init_declaration",
	"lambda",
	"lambda_expression",
	"local_function_statement",
	"method",
	"method_declaration",
	"method_definition",
	"secondary_constructor",
	"singleton_method"
};

// 関数定義／メソッド定義ノード型（言語横断，コンストラクタ含む）
const NodeKind::NodeTypeSet NodeKind::FunctionLikeDefinition = {
	"constructor_declaration",
	"function_declaration",
	"function_definition",
	"function_item",
	"method",
	"method_declaration",
	"method_definition",
	"secondary_constructor",
	"singleton_method"
};

// 関数名として現れる識別子葉（Ruby の定数メソッド名を含む — Lint の関数名判定）
const NodeKind::NodeTypeSet NodeKind::FunctionNameIdentifier = { "constant", "identifier", "name", "simple_identifier" };

// JS/TS `*` 前置のジェネレータ印を持つ関数親
const NodeKind::NodeTypeSet NodeKind::GeneratorStarHost =
{ "generator_function", "generator_function_declaration", "method_definition" };

// Go の switch / select の節
const NodeKind::NodeTypeSet NodeKind::GoCaseClause = { "communication_case", "default_case", "expression_case", "type_case" };

// 括弧無で型の変換に置ける Go の型（ポインタ・関数・チャネルの型は後の `(` と結合を争う）
const NodeKind::NodeTypeSet NodeKind::GoConversionType = {
	"array_type",
	"generic_type",
	"interface_type",
	"map_type",
	"qualified_type",
	"slice_type",
	"struct_type",
	"type_identifier"
};

// Structure で `(` 後・`)` 前を改行する Go の一括宣言
const NodeKind::NodeTypeSet NodeKind::GoGroupedDecl =
{ "const_declaration", "import_spec_list", "type_declaration", "var_spec_list" };

// Go の構造体・インタフェースの要素の並び（改行が区切りを兼ねる）
const NodeKind::NodeTypeSet NodeKind::GoMemberList = { "field_declaration_list", "interface_type" };

// Go の多行展開時に末尾 `,` を補うコンテナ（gofmt 同様，自動セミコロン挿入に依る構文破壊を防止）
const NodeKind::NodeTypeSet NodeKind::MultilineCommaContainer =
{ "argument_list", "literal_value", "parameter_list", "type_arguments", "type_parameter_list" };

// 改行で文・宣言を区切れる Go の並び（改行を跨ぐ継続の検査の対象外）
const NodeKind::NodeTypeSet NodeKind::GoStatementBoundary = {
	"block",
	"const_declaration",
	"field_declaration_list",
	"interface_type",
	"source_file",
	"statement_list",
	"type_declaration",
	"var_declaration",
	"var_spec_list"
};

// Go の switch 文
const NodeKind::NodeTypeSet NodeKind::GoSwitch = { "expression_switch_statement", "type_switch_statement" };

// Go の型宣言ノード
const NodeKind::NodeTypeSet NodeKind::GoType = {
	"array_type",
	"func_literal",
	"function_type",
	"implicit_length_array_type",
	"interface_type",
	"map_type",
	"method_declaration",
	"slice_type",
	"struct_type"
};

// 型名の形の Go の型（複合リテラルの型に置くと，制御構文の見出で其の `{` が本体の始まりと紛れる）
const NodeKind::NodeTypeSet NodeKind::GoTypeName = { "generic_type", "qualified_type", "type_identifier" };

// 右端を貪欲に伸ばす又は呼び出されて初めて括弧が要る式（後に字句が続かない値の位置では括弧が冗長）
const NodeKind::NodeTypeSet NodeKind::GreedyTailExpression = {
	"arrow_function",
	"if_expression",
	"lambda",
	"lambda_expression",
	"lambda_literal",
	"match_expression",
	"reflect_expression",
	"when_expression"
};

// グルーピング括弧ノード（単一被演算子の括弧除去対象）
const NodeKind::NodeTypeSet NodeKind::GroupingParen =
{ "parenthesized_expression", "parenthesized_statements", "tuple_expression" };

// 型を包む括弧の節点（TS / Kotlin / Go の parenthesized_type，Swift / Rust は要素１つの tuple_type が括弧に当たる）
const NodeKind::NodeTypeSet NodeKind::GroupingTypeParen = { "parenthesized_type", "tuple_type" };
// Ruby `**hash` 系ノード（`** hash` だと冪乗演算子と解釈され構文崩壊する為，識別子との密着が必須）
const NodeKind::NodeTypeSet NodeKind::HashSplat = { "hash_splat_argument", "hash_splat_parameter" };

// 端の空白を描画しない HTML の行区切要素名
const NodeKind::NodeTypeSet NodeKind::HtmlBlockTag = {
	"address",
	"article",
	"aside",
	"blockquote",
	"body",
	"caption",
	"center",
	"col",
	"colgroup",
	"dd",
	"details",
	"dir",
	"div",
	"dl",
	"dt",
	"fieldset",
	"figcaption",
	"figure",
	"footer",
	"form",
	"frame",
	"frameset",
	"h1",
	"h2",
	"h3",
	"h4",
	"h5",
	"h6",
	"head",
	"header",
	"hgroup",
	"hr",
	"html",
	"legend",
	"li",
	"listing",
	"main",
	"menu",
	"nav",
	"ol",
	"optgroup",
	"option",
	"p",
	"plaintext",
	"pre",
	"search",
	"section",
	"summary",
	"table",
	"tbody",
	"td",
	"tfoot",
	"th",
	"thead",
	"tr",
	"ul",
	"xmp"
};

// HTML の定義の一覧の項目の小文字のタグ名（構文解析器は互いの開始タグで暗黙に閉じる）
const NodeKind::NodeTypeSet NodeKind::HtmlDefinitionTag = { "dd", "dt" };
// 通常要素と埋込 script・style を同じ開閉要素として包む
const NodeKind::NodeTypeSet NodeKind::HtmlElementWrap = { "element", "script_element", "style_element" };
// SVG の文字・MathML の字句の小文字のタグ名（外来要素の内側でも空白を描画する）
const NodeKind::NodeTypeSet NodeKind::HtmlForeignTextTag = { "mi", "mn", "mo", "ms", "mtext", "text", "textpath", "tspan" };

// HTML の箱を作らない小文字要素名（隣と併合される空白の扱いは更に隣で決定，開いた時だけ箱を作る dialog も空白保持側で処理）
const NodeKind::NodeTypeSet NodeKind::HtmlHiddenTag =
{ "area", "base", "basefont", "datalist", "dialog", "link", "meta", "noembed", "noframes", "param", "rp", "template", "title" };

// tree-sitter-html が１バイトの種別で保持する要素名
const NodeKind::NodeTypeSet NodeKind::HtmlKnownTag = {
	"a",
	"abbr",
	"address",
	"area",
	"article",
	"aside",
	"audio",
	"b",
	"base",
	"basefont",
	"bdi",
	"bdo",
	"bgsound",
	"blockquote",
	"body",
	"br",
	"button",
	"canvas",
	"caption",
	"cite",
	"code",
	"col",
	"colgroup",
	"command",
	"data",
	"datalist",
	"dd",
	"del",
	"details",
	"dfn",
	"dialog",
	"div",
	"dl",
	"dt",
	"em",
	"embed",
	"fieldset",
	"figcaption",
	"figure",
	"footer",
	"form",
	"frame",
	"h1",
	"h2",
	"h3",
	"h4",
	"h5",
	"h6",
	"head",
	"header",
	"hgroup",
	"hr",
	"html",
	"i",
	"iframe",
	"image",
	"img",
	"input",
	"ins",
	"isindex",
	"kbd",
	"keygen",
	"label",
	"legend",
	"li",
	"link",
	"main",
	"map",
	"mark",
	"math",
	"menu",
	"menuitem",
	"meta",
	"meter",
	"nav",
	"nextid",
	"noscript",
	"object",
	"ol",
	"optgroup",
	"option",
	"output",
	"p",
	"param",
	"picture",
	"pre",
	"progress",
	"q",
	"rb",
	"rp",
	"rt",
	"rtc",
	"ruby",
	"s",
	"samp",
	"script",
	"section",
	"select",
	"slot",
	"small",
	"source",
	"span",
	"strong",
	"style",
	"sub",
	"summary",
	"sup",
	"svg",
	"table",
	"tbody",
	"td",
	"template",
	"textarea",
	"tfoot",
	"th",
	"thead",
	"time",
	"title",
	"tr",
	"track",
	"u",
	"ul",
	"var",
	"video",
	"wbr"
};

// 構文解析器が空要素として読む廃止済の HTML の要素の小文字のタグ名（tree-sitter-html の空要素の中で HtmlVoidTag に無い物）
const NodeKind::NodeTypeSet NodeKind::HtmlObsoleteVoidTag =
{ "basefont", "bgsound", "command", "frame", "image", "isindex", "keygen", "menuitem", "nextid" };

// 閉じタグを省ける HTML の要素の小文字のタグ名（構文解析器は後続の兄弟の開始タグで暗黙に閉じる）
const NodeKind::NodeTypeSet NodeKind::HtmlOptionalEndTag =
{ "colgroup", "dd", "dt", "li", "optgroup", "p", "rb", "rp", "rt", "td", "th", "tr" };

// 構文解析器が HTML の段落を暗黙に閉じる開始タグの小文字のタグ名（tree-sitter-html の TAG_TYPES_NOT_ALLOWED_IN_PARAGRAPHS）
const NodeKind::NodeTypeSet NodeKind::HtmlParagraphCloser = {
	"address",
	"article",
	"aside",
	"blockquote",
	"details",
	"div",
	"dl",
	"fieldset",
	"figcaption",
	"figure",
	"footer",
	"form",
	"h1",
	"h2",
	"h3",
	"h4",
	"h5",
	"h6",
	"header",
	"hr",
	"main",
	"nav",
	"ol",
	"p",
	"pre",
	"section"
};

// HTML の空白を描画する要素の小文字のタグ名（内容は要素・コメントとして読む）
const NodeKind::NodeTypeSet NodeKind::HtmlPreformattedTag = { "listing", "pre" };

// HTML の逐語内容要素は空白を含む内容を保護する
const NodeKind::NodeTypeSet NodeKind::HtmlRawContentTag =
{ "iframe", "noembed", "noframes", "plaintext", "textarea", "title", "xmp" };

// HTML の埋込 raw_text を持つ要素（Formatter の部分解析対象）
const NodeKind::NodeTypeSet NodeKind::HtmlRawTextElement = { "script_element", "style_element" };
// 構文解析器が内容を生の本文として読む HTML の要素の小文字のタグ名（内容の中のタグを数えない）
// HTML <script> // HTML <style>
const NodeKind::NodeTypeSet NodeKind::HtmlRawTextTag = { "script", "style" };
// HTML のルビの部品の小文字のタグ名（構文解析器は互いの開始タグで暗黙に閉じる）
const NodeKind::NodeTypeSet NodeKind::HtmlRubyTag = { "rb", "rp", "rt" };
// HTML の表のセルの小文字のタグ名（構文解析器は互いと行の開始タグで暗黙に閉じる）
const NodeKind::NodeTypeSet NodeKind::HtmlTableCellTag = { "td", "th" };

// HTML タグ系ノード
const NodeKind::NodeTypeSet NodeKind::HtmlTag =
{ "end_tag", "jsx_closing_element", "jsx_opening_element", "jsx_self_closing_element", "self_closing_tag", "start_tag" };

// HTML の空要素の小文字のタグ名（内容と閉じタグを持たない）
const NodeKind::NodeTypeSet NodeKind::HtmlVoidTag =
{ "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr" };

// 葉識別子相当（言語横断で名前参照ノード判定）`Lint::CheckShadowedVariable` で識別子名比較対象
// C/C++/Java/JS/TS 等 // Kt
const NodeKind::NodeTypeSet NodeKind::IdentifierLeafLike = { "identifier", "simple_identifier" };
// if 系のノード
const NodeKind::NodeTypeSet NodeKind::IfNode = { "if_expression", "if_statement" };

// インポート系 (import / using / use)（preproc_include は別構造種別の為，含めない）
const NodeKind::NodeTypeSet NodeKind::ImportLike = {
	"future_import_statement",
	"import_declaration",
	"import_from_statement",
	"import_statement",
	"use_declaration",
	"use_statement",
	"using_directive"
};

// インデントブロック（IndentContainer ∪ RubyBlock ∪ JSX 要素の和集合）
const NodeKind::NodeTypeSet NodeKind::IndentBlock = {
	"annotation_type_body",
	"argument_list",
	"array",
	"begin",
	"block",
	"block_body",
	"body_statement",
	"case",
	"case_match",
	"class",
	"class_body",
	"colon_block",
	"compound_statement",
	"computed_property",
	"constructor_body",
	"control_structure_body",
	"declaration_list",
	"dictionary",
	"dictionary_comprehension",
	"do_block",
	"enum_body",
	"enum_class_body",
	"expression_switch_statement",
	"field_declaration_list",
	"for",
	"function_body",
	"generator_expression",
	"hash",
	"if",
	"interface_body",
	"jsx_element",
	"jsx_opening_element",
	"jsx_self_closing_element",
	"keyframe_block_list",
	"lambda",
	"list",
	"list_comprehension",
	"list_pattern",
	"match_block",
	"method",
	"method_parameters",
	"module",
	"parameters",
	"parenthesized_expression",
	"parenthesized_statements",
	"program",
	"protocol_body",
	"select_statement",
	"set",
	"set_comprehension",
	"singleton_class",
	"singleton_method",
	"source_file",
	"statement_block",
	"statements",
	"string_array",
	"switch_block",
	"switch_body",
	"symbol_array",
	"translation_unit",
	"tuple",
	"tuple_pattern",
	"type_parameter",
	"type_switch_statement",
	"unless",
	"until",
	"when_expression",
	"while",
	"willset_didset_block",
	"with_clause"
};

// インデントを付与するコンテナノード
const NodeKind::NodeTypeSet NodeKind::IndentContainer = {
	"annotation_type_body",
	"block",
	"block_body",
	"body_statement",
	"catch_block",
	"class",
	"class_body",
	"colon_block",
	"compound_statement",
	"computed_property",
	"constructor_body",
	"control_structure_body",
	"declaration_list",
	"do_block",
	"enum_body",
	"enum_class_body",
	"enum_declaration_list",
	"expression_switch_statement",
	"field_declaration_list",
	"finally_block",
	"function_body",
	"interface_body",
	"interface_type",
	"keyframe_block_list",
	"match_block",
	"method",
	"module",
	"program",
	"protocol_body",
	"select_statement",
	"singleton_class",
	"singleton_method",
	"source_file",
	"statement_block",
	"statements",
	"switch_block",
	"switch_body",
	"translation_unit",
	"type_switch_statement",
	"when_expression",
	"willset_didset_block"
};

// 二項演算子・代入・アロー関数等，両側スペースを入れる
const NodeKind::NodeTypeSet NodeKind::InfixOp = {
	"additive_expression",
	"alias_declaration",
	"alternative_pattern",
	"arrow_function",
	"as_expression",
	"assignment",
	"assignment_expression",
	"assignment_statement",
	"augmented_assignment",
	"augmented_assignment_expression",
	"binary",
	"binary_expression",
	"binary_operator",
	"bitwise_operation",
	"boolean_operator",
	"comparison_expression",
	"comparison_operator",
	"compound_assignment_expr",
	"conditional",
	"conditional_expression",
	"conjunction_expression",
	"const_spec",
	"declaration",
	"disjunction_expression",
	"elvis_expression",
	"enum_member_declaration",
	"equality_expression",
	"field_declaration",
	"field_definition",
	"function_type",
	"infix_expression",
	"init_declarator",
	"intersection_type",
	"is_expression",
	"is_pattern_expression",
	"lambda_expression",
	"lambda_literal",
	"let_condition",
	"let_declaration",
	"match_arm",
	"multiplicative_expression",
	"named_expression",
	"namespace_alias_definition",
	"nil_coalescing_expression",
	"operator_assignment",
	"or_pattern",
	"property_declaration",
	"range_clause",
	"send_statement",
	"short_var_declaration",
	"switch_expression_arm",
	"ternary_expression",
	"type_alias_declaration",
	"type_alias_statement",
	"type_cast_expression",
	"type_item",
	"typealias_declaration",
	"union_pattern",
	"union_type",
	"using_directive",
	"var_spec",
	"variable_declarator",
	"variadic_parameter_declaration"
};

// 初期化の文を持つ見出（子の文の終わりの `;` を見出の区切りと揃えて割る）
const NodeKind::NodeTypeSet NodeKind::InitHeader = { "condition_clause", "for_statement" };

// 整数リテラル葉（`Lint::CheckCmpBoundary` の境界値判定 — 浮動小数を含む `NumberLiteral` より狭い完全一致集合）
const NodeKind::NodeTypeSet NodeKind::IntLiteralLike =
{ "decimal_integer_literal", "int_literal", "integer", "integer_literal", "number", "number_literal" };

// 反復対象を導く `in`・`as` 等を演算子から除く見出
const NodeKind::NodeTypeSet NodeKind::IterationHeader = {
	"for_expression",
	"for_in_clause",
	"for_in_statement",
	"for_statement",
	"foreach_statement",
	"from_clause",
	"in",
	"join_clause"
};

// 空の実引数を省ける Java の節点（注釈は印の注釈，列挙の定数は引数の無い構築子の呼出と同じ）
// Java @A() // Java X()
const NodeKind::NodeTypeSet NodeKind::JavaEmptyArgumentHost = { "annotation", "enum_constant" };

// Java の字句化前 Unicode エスケープを中身として包む字句（`Formatter::ExpandJavaUnicodeEscapes` の展開要否判定）
const NodeKind::NodeTypeSet NodeKind::JavaEscapeHost =
// Java /* */ // Java 'a' // Java // // Java "s"
{ "block_comment", "character_literal", "line_comment", "string_literal" };

// 定数式の構成要素に為れない Java の式（未知の名前や節点は未証明として扱う）
const NodeKind::NodeTypeSet NodeKind::JavaNonConstantExpression = {
	"array_access",
	"array_creation_expression",
	"assignment_expression",
	"class_literal",
	"instanceof_expression",
	"lambda_expression",
	"method_invocation",
	"null_literal",
	"object_creation_expression",
	"switch_expression",
	"update_expression"
};

// 仮引数のスコープを開く Java の節点
const NodeKind::NodeTypeSet NodeKind::JavaParameterScope =
{ "compact_constructor_declaration", "constructor_declaration", "lambda_expression", "method_declaration" };

// 文の位置に置いた Java の switch の親（tree-sitter-java は switch の文と式を同じ switch_expression で表す）
const NodeKind::NodeTypeSet NodeKind::JavaStatementHost = {
	"block",
	"constructor_body",
	"do_statement",
	"enhanced_for_statement",
	"for_statement",
	"if_statement",
	"labeled_statement",
	"switch_block_statement_group",
	"while_statement"
};

// Java の型の本体（仮引数のスコープを引き継がない）
const NodeKind::NodeTypeSet NodeKind::JavaTypeBody = { "annotation_type_body", "class_body", "enum_body", "interface_body" };
// JS/TS の束縛の名前（分割代入の省略形を含む）
const NodeKind::NodeTypeSet NodeKind::JsBindingName = { "identifier", "shorthand_property_identifier_pattern" };
// `export default` の後で宣言と読まれる JS/TS の式（関数・クラスの宣言の形で，文の終わりの `;` を持たない）
const NodeKind::NodeTypeSet NodeKind::JsDefaultDeclaration = { "class", "function_expression", "generator_function" };

// JS/TS の `in` を for 初期化子外で二項演算子へ戻す式
const NodeKind::NodeTypeSet NodeKind::JsInRestoring = {
	"arguments",
	"computed_property_name",
	"field_definition",
	"pair",
	"parenthesized_expression",
	"public_field_definition",
	"statement_block",
	"subscript_expression",
	"template_substitution"
};

// 改行の後の成員へ掛かる JS/TS のクラスの修飾語（解析器は修飾語だけの行を其の名前の欄と読む為，後続の成員へ繋ぐ）
const NodeKind::NodeTypeSet NodeKind::JsJoiningModifier = { "get", "set", "static" };
// JS/TS のメンバ・添字の参照（代入しても受取側の変数は再束縛されない）
const NodeKind::NodeTypeSet NodeKind::JsMemberTarget = { "member_expression", "subscript_expression" };
// JS/TS のモジュールの印の文（持たないファイルは古典スクリプトで，最上位の宣言を他のスクリプトと共有する）
const NodeKind::NodeTypeSet NodeKind::JsModuleStatement = { "export_statement", "import_statement" };

// 後の `/` を正規表現の始まりにする JS/TS の語（式の始まりに為る語で，前の値の除算と読まない）
const NodeKind::NodeTypeSet NodeKind::JsRegexPrefixWord =
{ "await", "case", "delete", "do", "else", "in", "instanceof", "new", "of", "return", "throw", "typeof", "void", "yield" };

// 改行で成員を区切る JS/TS のクラスの修飾語（言語は修飾語の後の改行で其の名前の欄と後続の成員に分けるが，解析器が繋ぐ）
const NodeKind::NodeTypeSet NodeKind::JsSeparatingModifier = { "abstract", "accessor" };
// JS/TS の statement_block + Java/Go/Rust 等の block（単独コメント `{ /* */ }` block 親判定）
const NodeKind::NodeTypeSet NodeKind::JsStatementBlockOrBlock = { "block", "statement_block" };

// 文・成員を並べる JS/TS の節点（JSX の式の中から退避するコメントを，其の子の文の前へ置く）
const NodeKind::NodeTypeSet NodeKind::JsStatementSequence =
{ "class_body", "program", "statement_block", "switch_case", "switch_default" };

// JS/TS で末尾 `;` 補完対象となる文ノード（ASI 補完）
const NodeKind::NodeTypeSet NodeKind::JsTsAsiTarget = {
	"break_statement",
	"continue_statement",
	"debugger_statement",
	"do_statement",
	"export_statement",
	"expression_statement",
	"function_signature",
	"import_statement",
	"lexical_declaration",
	"return_statement",
	"throw_statement",
	"type_alias_declaration",
	"variable_declaration"
};

// JS/TS のクラスの本体で末尾 `;` 補完対象となる成員（`;` は成員の外の字句として本体に並ぶ）
const NodeKind::NodeTypeSet NodeKind::JsTsClassMemberAsi =
{ "abstract_method_signature", "field_definition", "index_signature", "method_signature", "public_field_definition" };

// JS/TS の変数宣言キーワードトークン（DeclEdit で宣言先頭子の型接頭辞判定に用いる）
const NodeKind::NodeTypeSet NodeKind::JsVarDeclKeyword = { "const", "let", "var" };
// JSX の内容境界を通常の要素とは別集合で判定する
const NodeKind::NodeTypeSet NodeKind::JsxContainer = { "jsx_element" };
// JSX 要素
const NodeKind::NodeTypeSet NodeKind::JsxElement = { "element", "jsx_element", "jsx_self_closing_element" };

// JSX/HTML の開閉タグ全種（JsxContentCount で内容から除外する非内容子）
const NodeKind::NodeTypeSet NodeKind::JsxHtmlTagOpenClose =
{ "end_tag", "jsx_closing_element", "jsx_opening_element", "start_tag" };

// JSX / HTML の閉じタグ系（`BracketPairs` で `</`〜`>` ペア化対象）
const NodeKind::NodeTypeSet NodeKind::JsxOrHtmlCloseTag = { "end_tag", "jsx_closing_element" };

// JSX/HTML の開きタグ＋自己閉じタグ（BracketPairs で `<`/`</` を `>`/`/>` にペア化する集合）
const NodeKind::NodeTypeSet NodeKind::JsxOrHtmlOpenLike =
{ "jsx_opening_element", "jsx_self_closing_element", "self_closing_tag", "start_tag" };

// JSX/HTML の開きタグ系（BracketPairs で `<`〜`>` ペア化対象）
// JS/TS <C> / <> // HTML <tag>
const NodeKind::NodeTypeSet NodeKind::JsxOrHtmlOpenTag = { "jsx_opening_element", "start_tag" };

// JSX タグ全種（jsx_text 越境境界アンカー）
const NodeKind::NodeTypeSet NodeKind::JsxOrHtmlTagAny =
// JS/TS </C> // JS/TS <C> // JS/TS <C/>
{ "jsx_closing_element", "jsx_opening_element", "jsx_self_closing_element" };

// コメントの挿入の JSX の文脈の索引に載せる節点（本文の文脈を決める要素と，式の文脈を決める `{…}`・タグ）
const NodeKind::NodeTypeSet NodeKind::JsxSpan =
{ "jsx_closing_element", "jsx_element", "jsx_expression", "jsx_opening_element", "jsx_self_closing_element" };

// JSX タグ
// JS/TS <C> // JS/TS <C/>
const NodeKind::NodeTypeSet NodeKind::JsxTag = { "jsx_opening_element", "jsx_self_closing_element" };
// JSX の本文（文字参照と本文は１つの子文字列として連なる）
// JSX &amp; // JSX text
const NodeKind::NodeTypeSet NodeKind::JsxTextLike = { "html_character_reference", "jsx_text" };
// 語で始まる脱出の文（最初の子の語で break / continue / return / throw を見分ける）
const NodeKind::NodeTypeSet NodeKind::KeywordJump = { "control_transfer_statement", "jump_expression" };

// 語や記号で前置する単項の式（表の演算子を持たないか，別の演算子と同じ字面を持つ）
const NodeKind::NodeTypeSet NodeKind::KeywordUnary =
{ "cast_expression", "co_await_expression", "delete_expression", "error_suppression_expression" };

// Kotlin のアクセサ（直前のプロパティの一部で，解析器は型の本体の成員として並べる）
// Kt val a get() = 1 // Kt var b = 0 private set
const NodeKind::NodeTypeSet NodeKind::KotlinAccessor = { "getter", "setter" };
// Kotlin の注釈親（use_site_target の `get`・`:` 等を全て密着）
const NodeKind::NodeTypeSet NodeKind::KotlinAnnotationLike = { "annotation", "file_annotation", "use_site_target" };

// 代入先を包む Kotlin の節点（中の名前が代入先）
const NodeKind::NodeTypeSet NodeKind::KotlinAssignableWrapper =
// Kt x = 1 の x // Kt (x) = 1
{ "directly_assignable_expression", "parenthesized_expression" };

// 本体の波括弧を包みの節点に入れず直接の子に持つ Kotlin の構文（本体の文だけを行毎に置き，見出と後続の節は同じ行に保つ）
const NodeKind::NodeTypeSet NodeKind::KotlinBraceBodyHost =
{ "anonymous_initializer", "secondary_constructor", "try_expression" };

// 枝を持つ Kotlin の式 (if / when / try) の枝の本体の包み（最後の文の値が枝の値に為り得る）
const NodeKind::NodeTypeSet NodeKind::KotlinBranchBody = { "catch_block", "control_structure_body" };

// Kotlin の型の本体の成員（最後の成員の後に同じ行で `}` が続くと，解析器が成員を `ERROR` に包む）
const NodeKind::NodeTypeSet NodeKind::KotlinClassMember = {
	"anonymous_initializer",
	"class_declaration",
	"companion_object",
	"enum_entry",
	"function_declaration",
	"getter",
	"object_declaration",
	"property_declaration",
	"secondary_constructor",
	"setter",
	"type_alias"
};

// Kotlin の関数の見出の字句（後に続く型仮引数の `<` との間に空白を置く）
// Kt fun <T> f() // Kt inline <T>
const NodeKind::NodeTypeSet NodeKind::KotlinFunctionHead = { "fun", "modifiers" };
// `Lang::Kotlin` の文脈内で語彙判別する `import` の葉
const NodeKind::NodeTypeSet NodeKind::KotlinImportLeaf = { "identifier", "import_header", "import_list" };

// Kotlin で前後の行へ式を繋ぐ呼出と参照
const NodeKind::NodeTypeSet NodeKind::KotlinJointSuffix =
{ "call_suffix", "callable_reference", "infix_expression", "jump_expression", "navigation_suffix" };

// `!` を前置して否定する Kotlin の検査（`!is` / `!in` は１字句で，空白を挟めない）
const NodeKind::NodeTypeSet NodeKind::KotlinNegatableTest = { "check_expression", "range_test", "type_test" };
// 式の位置に置けない Kotlin の文（値を使う if 式・when の枝の本体の波括弧を外すと，式の位置に文が来て誤りに為る）
const NodeKind::NodeTypeSet NodeKind::KotlinNonExpression = { "assignment", "for_statement", "while_statement" };
// Kotlin `!is` `!in` 演算子の右側キーワード（否定する検査の中で `!` 直後の密着判定）
// Kt `!in` の `in` // Kt `!is` の `is`
const NodeKind::NodeTypeSet NodeKind::KotlinNotIsIn = { "in", "is" };
// Kotlin の数値リテラル接尾子付種別（数値部と接尾子の密着必須，`100L` → `100 L` 化で構文解析失敗）
const NodeKind::NodeTypeSet NodeKind::KotlinNumericLiteralSuffix = { "long_literal", "real_literal", "unsigned_literal" };
// 仮引数の既定値の `=` を直接の子に持つ Kotlin の節点（関数の仮引数の並び・主構築子の仮引数）
const NodeKind::NodeTypeSet NodeKind::KotlinParameterHost = { "class_parameter", "function_value_parameters" };

// Kotlin 関数戻値型ノード（function_value_parameters 後の最初の名前付子で戻値型判定）
const NodeKind::NodeTypeSet NodeKind::KotlinReturnType =
{ "dynamic_type", "function_type", "nullable_type", "parenthesized_type", "user_type" };

// 改行が文を終える Kotlin の文脈（括弧の中と when の枝の条件では改行が意味を持たない）
const NodeKind::NodeTypeSet NodeKind::KotlinStatementContext =
{ "class_body", "control_structure_body", "function_body", "source_file", "statements" };

// 型の被演算子を取る Kotlin の型（関数の型・省略可能の型を括弧無で置くと結合が変わる）
const NodeKind::NodeTypeSet NodeKind::KotlinTypeOperatorHost = { "nullable_type", "receiver_type" };
// 枝の最後の文の値を結果にする Kotlin の式（値として使う時）
const NodeKind::NodeTypeSet NodeKind::KotlinValueBranch = { "if_expression", "when_expression" };
// 文に付くラベルと Kotlin の注釈
const NodeKind::NodeTypeSet NodeKind::LabelNode = { "annotation", "label", "statement_label" };
// ラベル付の文（goto の飛先）
const NodeKind::NodeTypeSet NodeKind::LabeledStatement = { "labeled_statement", "named_label_statement" };
// 行頭で親深度に揃える閉じトークン（`Layout::CollectTokenIndents` の `IsCloser` 判定，足す時は `CloserToken` へも足す）
const NodeKind::NodeTypeSet NodeKind::LayoutCloserToken = { ")", "/>", "</", ">", "]", "end", "}" };
// 先頭の型引数を後ろの子へ密着させる節点
const NodeKind::NodeTypeSet NodeKind::LeadingTypeArguments = { "method_invocation", "type_assertion" };

// 葉ノード（中身を再構築せず原文を其のまま使う）
const NodeKind::NodeTypeSet NodeKind::Leaf = {
	"attribute_value",
	"bare_string",
	"bare_symbol",
	"char_literal",
	"character_literal",
	"delimited_symbol",
	"encapsed_string",
	"heredoc",
	"heredoc_beginning",
	"heredoc_body",
	"interpolated_string_expression",
	"interpreted_string_literal",
	"jsx_closing_element",
	"line_string_literal",
	"macro_definition",
	"multi_line_string_literal",
	"multiline_comment",
	"namespace_name",
	"nested_type_identifier",
	"nowdoc",
	"qualified_name",
	"raw_string_literal",
	"raw_text",
	"regex",
	"regex_literal",
	"relative_name",
	"shell_command_expression",
	"string",
	"string_fragment",
	"string_literal",
	"string_value",
	"subshell",
	"template_argument_list",
	"template_parameter_list",
	"template_string",
	"text_interpolation",
	"type_argument_list",
	"type_arguments",
	"type_identifier",
	"type_parameter_list",
	"type_parameters",
	"uninterpreted",
	"user_defined_literal",
	"variable_name",
	"verbatim_string_literal"
};

// ラベルを跳び先として持つ脱出の式（中に現れるラベルは定義でなく参照）
const NodeKind::NodeTypeSet NodeKind::JumpWithLabel = { "break_expression", "continue_expression", "jump_expression" };

// break・continue の境界となる全言語の反復構文
const NodeKind::NodeTypeSet NodeKind::Loop = {
	"do_statement",
	"do_while_statement",
	"enhanced_for_statement",
	"for_expression",
	"for_in_statement",
	"for_range_loop",
	"for_statement",
	"foreach_statement",
	"loop_expression",
	"repeat_while_statement",
	"while_expression",
	"while_statement"
};

// 繰返を抜ける文
const NodeKind::NodeTypeSet NodeKind::LoopExitStatement = { "break_statement", "continue_statement" };
// `TsSource::MatchStep` の同位置入替対象（`for_statement` / `while_statement`）
const NodeKind::NodeTypeSet NodeKind::LoopForOrWhile = { "for_statement", "while_statement" };

// break_statement の境界となる反復・switch 系構文
const NodeKind::NodeTypeSet NodeKind::LoopOrSwitch = {
	"do_statement",
	"do_while_statement",
	"enhanced_for_statement",
	"expression_switch_statement",
	"for_expression",
	"for_in_statement",
	"for_range_loop",
	"for_statement",
	"foreach_statement",
	"loop_expression",
	"repeat_while_statement",
	"select_statement",
	"switch_statement",
	"type_switch_statement",
	"while_expression",
	"while_statement"
};

// マクロの定義（置換本体を持つ前処理指令）
const NodeKind::NodeTypeSet NodeKind::MacroDefinition = { "preproc_def", "preproc_function_def" };

// マクロ呼出風ノード
const NodeKind::NodeTypeSet NodeKind::MacroLike =
{ "destructor_name", "macro_definition", "macro_invocation", "marker_annotation", "operator_name", "try_operator" };

// メンバアクセスチェーン（ドット記法／スコープ解決記法）
const NodeKind::NodeTypeSet NodeKind::Member = {
	"directly_assignable_expression",
	"field_expression",
	"jsx_attribute",
	"member_access_expression",
	"member_call_expression",
	"member_expression",
	"navigation_expression",
	"nullsafe_member_access_expression",
	"nullsafe_member_call_expression",
	"qualified_identifier",
	"scoped_call_expression",
	"scoped_identifier",
	"scoped_property_access_expression"
};

// アクセス順序検査の順位２に当たる C# のプロパティ・イベント欄
const NodeKind::NodeTypeSet NodeKind::MemberFieldRank =
{ "const_declaration", "event_field_declaration", "property_declaration" };

// メンバ関数／メソッド／コンストラクタ（アクセス順序検査の順位 3）
const NodeKind::NodeTypeSet NodeKind::MemberMethodRank =
{ "constructor_declaration", "function_definition", "method_declaration", "method_definition" };

// 型別名＋ネスト型宣言（アクセス順序検査の順位 1）
const NodeKind::NodeTypeSet NodeKind::MemberTypeRank = {
	"alias_declaration",
	"class_declaration",
	"class_specifier",
	"enum_declaration",
	"enum_specifier",
	"interface_declaration",
	"record_declaration",
	"struct_declaration",
	"struct_specifier",
	"type_definition",
	"using_declaration"
};

// 山括弧の誤解析で比較式が型引数リストへ化けたノード（C/C++ の `b < c && d >= e`，C# / TS の `a < b, c >> d`）
const NodeKind::NodeTypeSet NodeKind::MisparsedTemplateHost =
{ "generic_name", "instantiation_expression", "template_function", "template_type" };

// 変数を書き換える式
const NodeKind::NodeTypeSet NodeKind::MutatingExpression = { "assignment_expression", "update_expression" };

// 単一名を持つ仮引数ノード（`Lint::FindParameterEvent` の対象種別）
const NodeKind::NodeTypeSet NodeKind::NamedParameterKind =
// Java/JS // Rust/Kt/Swift // C/C++ // TS
{ "formal_parameter", "parameter", "parameter_declaration", "required_parameter" };

// C++ / TS の名前空間／内部モジュール（`Structure::GetStructuredText` で本体の改行＋スペース整形対象）
const NodeKind::NodeTypeSet NodeKind::NamespaceLike = { "internal_module", "module", "namespace_definition" };
// C# `new()` 形の括弧付呼出文脈（ジェネリック制約と目標型推論 new）
const NodeKind::NodeTypeSet NodeKind::NewParenCall = { "constructor_constraint", "implicit_object_creation_expression" };

// 内部スペース正規化スキップ
const NodeKind::NodeTypeSet NodeKind::NoInnerSpaceEdit = {
	"attribute_value",
	"bare_string",
	"bare_symbol",
	"delimited_symbol",
	"format_specifier",
	"heredoc_body",
	"heredoc_content",
	"interpolated_string_expression",
	"interpolation",
	"label",
	"lifetime",
	"macro_definition",
	"namespace_name",
	"pseudo_class_selector",
	"pseudo_element_selector",
	"quoted_attribute_value",
	"regex",
	"regex_pattern",
	"relative_name",
	"slice",
	"string",
	"string_content",
	"string_fragment",
	"string_value",
	"subshell",
	"template_literal_type",
	"text",
	"variable_name"
};

// 走査を止める葉でも型・名前・要素構造を表す物は逐語保護対象外（コードの行末空白を除去）
const NodeKind::NodeTypeSet NodeKind::NonVerbatimLeaf = {
	"jsx_closing_element",
	"macro_definition",
	"namespace_name",
	"nested_type_identifier",
	"qualified_name",
	"relative_name",
	"template_argument_list"
};

// `:` 周辺空白を `: ` 形に正規化するノード
const NodeKind::NodeTypeSet NodeKind::NormColon = {
	"anonymous_function",
	"argument",
	"arrow_function",
	"associatedtype_declaration",
	"attribute",
	"catch_block",
	"class_parameter",
	"const_item",
	"constrained_type",
	"declaration",
	"dict_pattern",
	"dictionary_literal",
	"dictionary_type",
	"enum_declaration",
	"enum_type_parameters",
	"feature_query",
	"field_initializer",
	"function_declaration",
	"index_signature",
	"inheritance_constraint",
	"keyed_element",
	"keyword_parameter",
	"keyword_pattern",
	"lambda_parameter",
	"method_declaration",
	"named_label_statement",
	"pair",
	"pair_pattern",
	"parameter",
	"pattern",
	"property_signature",
	"public_field_definition",
	"static_item",
	"subpattern",
	"trait_bounds",
	"type_annotation",
	"type_parameter",
	"typed_default_parameter",
	"typed_parameter",
	"value_argument",
	"variable_declaration"
};

// `=` 周辺空白を１つに正規化するノード
const NodeKind::NodeTypeSet NodeKind::NormEq = {
	"optional_parameter",
	"optional_parameter_declaration",
	"public_field_definition",
	"required_parameter",
	"typed_default_parameter"
};

// 数値リテラル系ノード
const NodeKind::NodeTypeSet NodeKind::NumberLiteral = {
	"binary_integer_literal",
	"decimal_floating_point_literal",
	"decimal_integer_literal",
	"float",
	"float_literal",
	"hex_floating_point_literal",
	"hex_integer_literal",
	"hex_literal",
	"int_literal",
	"integer",
	"integer_literal",
	"number",
	"number_literal",
	"octal_integer_literal",
	"real_literal"
};

// オブジェクトリテラル系ノード（{} 内側スペース挿入対象）
const NodeKind::NodeTypeSet NodeKind::ObjectLike = {
	"accessor_list",
	"dictionary",
	"dictionary_comprehension",
	"enum_body",
	"enum_member_declaration_list",
	"export_clause",
	"field_initializer_list",
	"hash",
	"initializer_expression",
	"initializer_list",
	"interface_body",
	"literal_value",
	"named_imports",
	"object",
	"object_literal",
	"object_pattern",
	"object_type",
	"set",
	"set_comprehension",
	"struct_pattern",
	"switch_expression"
};

// 語で始まり右の被演算子を式の終わり迄取る式（括弧を外すと後続の演算子を被演算子へ取り込む）
const NodeKind::NodeTypeSet NodeKind::OpenPrefixExpression = {
	"arrow_function",
	"break",
	"break_expression",
	"closure_expression",
	"include_expression",
	"include_once_expression",
	"jump_expression",
	"lambda",
	"lambda_expression",
	"named_expression",
	"print_intrinsic",
	"require_expression",
	"require_once_expression",
	"return",
	"return_expression",
	"throw_expression",
	"yield",
	"yield_expression"
};

// 宣言へ密着する記号及び Ruby で改行を式の継続と読ませる文末字句
const NodeKind::NodeTypeSet NodeKind::OpenTailToken = { "&", "*", "**", "..", "...", ":" };
// 直前トークンへ密着する省略可能性記号（TS `?.` / `?` / Swift の省略可能型）
const NodeKind::NodeTypeSet NodeKind::OptionalMarker = { "optional_chain", "optional_type", "quest" };

// 言語が省く事を許す括弧の並び（名前だけのラムダの仮引数・空の仮引数・実引数）を子に持つ節点
const NodeKind::NodeTypeSet NodeKind::OptionalParenHost = {
	"annotation",
	"arrow_function",
	"attribute",
	"call",
	"call_expression",
	"call_suffix",
	"class_declaration",
	"class_definition",
	"enum_constant",
	"enum_entry",
	"exit_statement",
	"function_call_expression",
	"include_statement",
	"lambda",
	"lambda_expression",
	"lambda_function_type",
	"method",
	"mixin_statement",
	"new_expression",
	"object_creation_expression",
	"singleton_method",
	"yield" // Rb yield()
};

// `?` を後ろで割ると宣言名と型注釈が別の構文へ化ける宣言ノード（`name?: T` の `?` は型注釈の一部で名前と不可分）
const NodeKind::NodeTypeSet NodeKind::OptionalTypeMember =
{ "abstract_method_signature", "method_signature", "optional_parameter", "property_signature", "public_field_definition" };

// 文書化引数列コンテナ（Ruby method_parameters を含む文書化解析用，C/C++ は CFunctionOrFieldDeclarator 経由で別取得）
const NodeKind::NodeTypeSet NodeKind::ParamContainerLike =
{ "formal_parameters", "function_value_parameters", "method_parameters" }; // JS/TS // Kt // Rb def f(x)

// 関数パラメータコンテナ（言語横断）
const NodeKind::NodeTypeSet NodeKind::ParameterContainer =
{ "formal_parameters", "function_value_parameters", "parameter_list", "parameters" }; // JS/TS // Kt // C/C++/Rust // Py

// 括弧除去で構文又は意味が変わる最左子孫
const NodeKind::NodeTypeSet NodeKind::ParenInnerFirstChildNoUnwrap = { "left_assignment_list", "named_expression" };

// 括弧を要する内側式は即時呼出等の構文と意味を保つ
const NodeKind::NodeTypeSet NodeKind::ParenInnerNoUnwrap = {
	"arrow_function",
	"comma_expression",
	"compound_statement", // GNU C/C++ の文式 `({ int y = 1; y; })` — 括弧を外すと初期化子の並びや塊に為る
	"custom_operator", // Swift `(^^)` 演算子の参照 — 括弧を外すと演算子だけが残って構文が壊れる
	"element",
	"if_expression", // Kt/Rust `a + (if(c) 1 else 0) + b` — 括弧を外すと else 枝が右隣を飲込み値が変わる
	"if_modifier",
	"jsx_element",
	"jsx_self_closing_element",
	"lambda",
	"lambda_expression",
	"lambda_literal",
	"list_splat",
	"list_splat_pattern",
	"match_expression", // Rust `a + (match c { ... }) + b` — 末尾を貪欲に伸ばす式で if_expression と同型
	"named_expression",
	"reflect_expression", // C++26 `r == (^^int) && t` — 被演算子の型が後続の `&&` `<` 等を取り込む
	"rescue_modifier", // Rb `(a rescue b)` 同上
	"sequence_expression",
	"unless_modifier", // Rb `(a unless c)` 同上
	"until_modifier", // Rb `(a until c)` 同上
	"when_expression", // Kt `a + (when { ... }) + b` — 同上
	"while_modifier", // Rb `(a while c)` 同上
	"yield" // Py `[(yield)]` `f((yield))` 要素／引数位置の yield・yield from は括弧必須で剥がすと構文崩壊
};

// 括弧維持対象ノード（直接一致・Control・Member の和集合）
const NodeKind::NodeTypeSet NodeKind::ParenKeeper = {
	"array_access",
	"array_type",
	"begin",
	"call_expression",
	"case",
	"case_match",
	"cast_expression",
	"catch_block",
	"catch_clause",
	"class_constant_access_expression", // PHP `($a . $b)::C` — 類定数の参照は後置で受信側括弧を保持
	"conditional_access_expression", // C# `(a + b)?.M()` — 括弧を外すと `?.` が末尾の被演算子だけへ掛かる
	"conditional_type",
	"decltype", // C++ decltype((x)) — 括弧の有無で型が変わる（`(x)` は左辺値の式で参照型 `int&`，`x` は宣言の型 `int`）
	"directly_assignable_expression",
	"do",
	"do_block",
	"do_statement",
	"do_while_statement",
	"element_access_expression",
	"element_binding_expression", // C# `(a + b)?[i]` — 同上
	"element_reference",
	"else",
	"else_clause",
	"else_if_clause",
	"enhanced_for_statement",
	"except_clause",
	"expression_switch_statement",
	"field_access", // Java (a - b).c — フィールド参照は後置で受信側の二項式を包む括弧を保持
	"field_expression",
	"finally_block",
	"finally_clause",
	"for",
	"for_expression",
	"for_in_statement",
	"for_range_loop",
	"foreach_statement",
	"function_call_expression",
	"guard_statement",
	"if",
	"if_expression",
	"if_statement",
	"index_expression",
	"indexing_expression",
	"instantiation_expression", // TS `(a ?? b)<T>` — 型引数の具体化は後置で，外すと末尾の被演算子だけへ掛かる
	"jsx_attribute",
	"loop_expression",
	"match_expression",
	"match_statement",
	"member_access_expression", // C# (a - b).M — メンバアクセスは括弧除去で受信側の二項式が再結合し意味変質
	"member_call_expression",
	"member_expression",
	"method_invocation", // Java (a - b).m() — メソッド呼出は後置で受信側括弧を保持
	"method_reference", // Java (c ? a : b)::m — メソッド参照は後置で受信側括弧を保持
	"navigation_expression",
	"new_expression",
	"nullsafe_member_access_expression", // PHP `($a - $b)?->y` — メンバアクセスは後置で受信側括弧を保持
	"nullsafe_member_call_expression", // PHP `($a - $b)?->f()` — 同上
	"object_creation_expression", // PHP new (pick("B")) — 括弧の中は類名を求める式で，外すと `pick` 類の生成に為る
	"qualified_identifier",
	"range_expression", // C# `arr[(a + b)..(c + d)]` — 括弧を外すと `..` の境界が解析不能に為る
	"repeat_while_statement",
	"scoped_call_expression", // PHP `(A|B)::f()` — スコープ解決は後置で受信側括弧を保持
	"scoped_identifier",
	"scoped_property_access_expression", // PHP `(A|B)::$x` — 同上
	"seh_except_clause",
	"seh_finally_clause",
	"seh_try_statement",
	"select_statement",
	"selector_expression", // Go (a - b).M — セレクタは後置で受信側の二項式を包む括弧を保持
	"sizeof_expression",
	"slice_expression", // Go (a + b)[1:2] — 切出は後置で受信側の二項式を包む括弧を保持
	"subscript",
	"subscript_expression",
	"switch_expression",
	"switch_statement",
	"synchronized_statement", // Java synchronized(x) { } — 錠の式の括弧は構文の一部
	"try_block",
	"try_expression",
	"try_statement",
	"try_with_resources_statement",
	"type_assertion_expression", // Go (a + b).(T) — 型表明は後置で受信側の二項式を包む括弧を保持
	"type_cast_expression",
	"type_switch_statement",
	"unless",
	"until",
	"when_expression",
	"while",
	"while_expression",
	"while_statement",
	"with_statement" // JS with(x) — 非推奨
};

// `(` で始まる型（修飾子・属性・生存期間等に続く `(` は呼出の括弧ではない）
const NodeKind::NodeTypeSet NodeKind::ParenLeadType =
{ "disjunctive_normal_form_type", "function_type", "parenthesized_type", "tuple_type", "unit_type" };

// 条件を括弧で包まない言語の制御構文
const NodeKind::NodeTypeSet NodeKind::ParenOptionalHeader = {
	"case",
	"expression_switch_statement",
	"for_expression",
	"guard_statement",
	"if",
	"if_expression",
	"if_statement",
	"match_expression",
	"match_statement",
	"repeat_while_statement",
	"switch_statement",
	"unless",
	"until",
	"while",
	"while_expression",
	"while_statement" // Swift/Py while (c) { } / while (c):
};

// 括弧が値の全体を包み，後に演算子や後置の字句が続かない値の位置（代入の右辺・実引数・戻値・要素）
const NodeKind::NodeTypeSet NodeKind::ParenValueSlot = {
	"argument",
	"argument_list",
	"arguments",
	"array",
	"array_element_initializer",
	"array_expression",
	"assignment",
	"assignment_expression",
	"assignment_pattern",
	"class_parameter",
	"co_return_statement",
	"default_parameter",
	"export_statement",
	"function_value_parameters",
	"init_declarator",
	"initializer_list",
	"jsx_expression",
	"jump_expression",
	"keyword_argument",
	"let_declaration",
	"list",
	"object_assignment_pattern",
	"optional_parameter",
	"optional_parameter_declaration",
	"pair",
	"property_declaration",
	"required_parameter",
	"return_expression",
	"return_statement",
	"simple_parameter",
	"splice_specifier",
	"ternary_expression",
	"throw_statement",
	"typed_default_parameter",
	"value_argument",
	"variable_declarator",
	"yield_expression" // JS/TS yield (x)
};

// `Structure::NeedsGapBetween` で制御語直後の括弧前空白を判定する変数束縛パターン
const NodeKind::NodeTypeSet NodeKind::PatternKind = { "pattern", "tuple_pattern" }; // Rust match 1 | _ // Rust (a, b)
// PHP の代替構文の終端語（本体の後で行頭に置く，足す時は CloserToken へも足す）
const NodeKind::NodeTypeSet NodeKind::PhpAltCloser = { "enddeclare", "endfor", "endforeach", "endif", "endswitch", "endwhile" };
// PHP の代替構文の本体を開く `:` を直接の子に持つノード（`if` / `foreach` / `while` は `colon_block` に包む）
const NodeKind::NodeTypeSet NodeKind::PhpAltHost = { "colon_block", "declare_statement", "for_statement", "switch_block" };
// 本体を開く `:` を先頭の字句に持つ PHP の代替構文の本体（`for` / `declare` は直下の `:` が本体を開く）
const NodeKind::NodeTypeSet NodeKind::PhpAltLeadingColon = { "colon_block", "switch_block" }; // PHP : stmts // PHP switch(...):
// 閉じタグの前で終端を要さない PHP の字句（文を終えた `;`，本体の `{` `}`，代替構文の見出・case の `:`）
const NodeKind::NodeTypeSet NodeKind::PhpCompleteToken = { ":", ";", "{", "}" };
// PHP の if の後続の枝の節（本体の後に並び，波括弧の無い本体の開いた if へ属し得る）
const NodeKind::NodeTypeSet NodeKind::PhpIfBranch = { "else_clause", "else_if_clause" }; // PHP else x(); // PHP elseif($a) x();
// PHP の地の文（開始タグの外の，出力其の物の本文）を示すノード
const NodeKind::NodeTypeSet NodeKind::PhpInlineHtml = { "text", "text_interpolation" };
// PHP の名前空間取込ノード（`\` 区切と群括弧は名前の一部で，空白を挟むと名前解決が壊れる）
const NodeKind::NodeTypeSet NodeKind::PhpNamespaceUse = { "namespace_use_declaration", "namespace_use_group" };
// PHP の開始タグで終わる子（地の文の後の開始タグ）
const NodeKind::NodeTypeSet NodeKind::PhpOpenTagEnd = { "php_tag", "text_interpolation" }; // PHP <?php // PHP ?> ... <?php

// PHP 関数戻値型ノード（formal_parameters 後の最初の名前付子で戻値型判定）
const NodeKind::NodeTypeSet NodeKind::PhpReturnType =
{ "intersection_type", "named_type", "optional_type", "primitive_type", "union_type" };

// 式等で終わり常に `;` で閉じる PHP の文（閉じタグが `;` の代わりに文を終え得，末尾の式が `}` で終わっても `;` を要する）
const NodeKind::NodeTypeSet NodeKind::PhpSemicolonStatement = {
	"break_statement",
	"const_declaration",
	"continue_statement",
	"do_statement",
	"echo_statement",
	"exit_statement",
	"expression_statement",
	"function_static_declaration",
	"global_declaration",
	"goto_statement",
	"namespace_use_declaration",
	"return_statement",
	"unset_statement" // PHP unset($x) ?>
};

// PHP / TypeScript の型結合ノード
const NodeKind::NodeTypeSet NodeKind::TypeCombination =
{ "disjunctive_normal_form_type", "intersection_type", "type_list", "union_type" };

// 内側で改行出来ない PHP の節点（括弧と語が１つの字句に当たる）
const NodeKind::NodeTypeSet NodeKind::PhpUnsplittable = { "cast_expression", "visibility_modifier" };
// `|` で囲まれる仮引数列（Ruby ブロック／Rust クロージャ — `|` 両側の密着判定）
const NodeKind::NodeTypeSet NodeKind::PipeParamList = { "block_parameters", "closure_parameters" }; // Rb |a, b| // Rust |a, b|
// 型を構文から確定出来ない単純な変数（PHP は `$x` を variable_name で持つ）
const NodeKind::NodeTypeSet NodeKind::PlainVariable = { "identifier", "variable_name" }; // C 系 x // PHP $x

// ポインタ宣言子
const NodeKind::NodeTypeSet NodeKind::PointerDecl = {
	"abstract_pointer_declarator",
	"abstract_reference_declarator",
	"block_parameter",
	"by_ref",
	"dictionary_splat",
	"dictionary_splat_pattern",
	"list_splat",
	"list_splat_pattern",
	"pointer_declarator",
	"pointer_type",
	"pointer_type_declarator", // C++ int S::*p（メンバへのポインタの宣言子を解析器が限定名と読む）
	"reference_declarator",
	"reference_pattern",
	"reference_type",
	"rest_pattern",
	"rest_type",
	"self_parameter",
	"splat_argument",
	"splat_parameter",
	"splat_pattern", // Py case [1, *rest] / case {**rest}（マッチパターンのスター展開は密着）
	"spread_element",
	"spread_expression",
	"variadic_declarator",
	"variadic_parameter",
	"variadic_parameter_declaration" // Go xs ...int
};

// C/C++ ポインタ／参照宣言子（戻値型のポインタ／参照判定）
const NodeKind::NodeTypeSet NodeKind::PointerOrRefDeclarator = { "pointer_declarator", "reference_declarator" };

// 後置単項演算子及び前置・後置共通の更新式
const NodeKind::NodeTypeSet NodeKind::PostfixOrUpdateExpression = {
	"dec_statement",
	"inc_statement",
	"non_null_expression",
	"postfix_expression",
	"postfix_unary_expression",
	"update_expression" // JS/TS ++x / x++
};

// 被呼出側を最初の子に持つ呼出（被呼出側を包む括弧の保持の判定）
const NodeKind::NodeTypeSet NodeKind::PostfixCallHost = { "call_expression", "function_call_expression" };

// 受け手（最初の子）を持つ後置の連鎖（受け手を包む括弧の除去で，最初の子を辿って受け手に置けるかを確かめる）
const NodeKind::NodeTypeSet NodeKind::PostfixReceiverChain = {
	"array_access",
	"attribute",
	"call",
	"call_expression",
	"element_access_expression",
	"element_reference",
	"field_access",
	"field_expression",
	"function_call_expression",
	"index_expression",
	"indexing_expression",
	"invocation_expression",
	"member_access_expression",
	"member_call_expression",
	"member_expression",
	"method_invocation",
	"navigation_expression",
	"non_null_expression",
	"selector_expression",
	"slice_expression",
	"subscript",
	"subscript_expression",
	"type_assertion_expression" // Go a.(T)
};

// 受け手を最初の子に持つ後置の式（メンバ参照・呼出・添字，受け手を包む括弧の除去の対象）
const NodeKind::NodeTypeSet NodeKind::PostfixReceiverHost = {
	"array_access",
	"call_expression",
	"element_access_expression",
	"element_reference",
	"field_access",
	"field_expression",
	"function_call_expression",
	"index_expression",
	"indexing_expression",
	"member_access_expression",
	"member_call_expression",
	"member_expression",
	"method_invocation",
	"navigation_expression",
	"selector_expression",
	"subscript",
	"subscript_expression" // C/C++/JS/TS/PHP (a.b)[0]
};

// 括弧無で後置の式の受け手に置ける名前・字句で閉じるリテラル（後置の連鎖の最初の子の終点）
const NodeKind::NodeTypeSet NodeKind::PostfixReceiverLeaf = {
	"array",
	"array_literal",
	"class_literal",
	"composite_literal",
	"dictionary",
	"encapsed_string",
	"identifier",
	"interpreted_string_literal",
	"line_string_literal",
	"list",
	"meta_property",
	"name",
	"object", // JS/TS { a: 1 }（文頭は塊と読まれる為に別に保持）
	"parenthesized_expression",
	"qualified_identifier",
	"raw_string_literal",
	"regex",
	"scoped_identifier",
	"self",
	"self_expression",
	"simple_identifier",
	"string",
	"string_literal",
	"struct_expression",
	"template_string",
	"this",
	"this_expression",
	"tuple",
	"variable_name" // PHP $a
};

// C/C++/C# プリプロセッサノード（preproc_ で始まる名称を全列挙）
const NodeKind::NodeTypeSet NodeKind::Preproc = {
	"preproc_arg",
	"preproc_call",
	"preproc_def",
	"preproc_define",
	"preproc_defined",
	"preproc_directive",
	"preproc_elif",
	"preproc_elifdef",
	"preproc_else",
	"preproc_endregion",
	"preproc_error",
	"preproc_function_def",
	"preproc_if",
	"preproc_if_in_attribute_list",
	"preproc_ifdef",
	"preproc_include",
	"preproc_line",
	"preproc_nullable",
	"preproc_params",
	"preproc_pragma",
	"preproc_region",
	"preproc_undef",
	"preproc_warning" // C#
};

// 条件分岐型プリプロセッサノード
const NodeKind::NodeTypeSet NodeKind::PreprocBlock =
{ "preproc_elif", "preproc_elifdef", "preproc_else", "preproc_if", "preproc_ifdef" };

// 条件を開く前処理指令の名前（`#` の後の語）
const NodeKind::NodeTypeSet NodeKind::PreprocConditionWord = { "if", "ifdef", "ifndef" };
// 前処理条件の根（elif・else は除外）
const NodeKind::NodeTypeSet NodeKind::PreprocConditional = { "preproc_if", "preproc_ifdef" };
// プリプロセッサ指令の閉じ系（単独指令で常に空行区切り）
const NodeKind::NodeTypeSet NodeKind::PreprocDirectiveClose = { "#else", "#endif" }; // C/C++ #else // C/C++ #endif
// プリプロセッサ指令の開き／分岐系（後続の条件式と連結対象）
const NodeKind::NodeTypeSet NodeKind::PreprocDirectiveOpen = { "#elif", "#elifdef", "#elifndef", "#if", "#ifdef", "#ifndef" };

// `LineSplit::CollectBreaks` の `IsTerminal` を拡張する前処理の平坦再帰対象
const NodeKind::NodeTypeSet NodeKind::PreprocFlat = {
	"preproc_call",
	"preproc_def",
	"preproc_define",
	"preproc_endregion",
	"preproc_error",
	"preproc_function_def",
	"preproc_include",
	"preproc_line",
	"preproc_nullable",
	"preproc_pragma",
	"preproc_region",
	"preproc_undef",
	"preproc_warning"
}; // C/C++ #pragma 等 // C# の各指令は 1 物理行必須で継続行を持たない為，全種を平坦扱いにする

const NodeKind::NodeTypeSet NodeKind::PreprocNameQuery = { "preproc_defined", "preproc_elifdef", "preproc_ifdef" };
const NodeKind::NodeTypeSet NodeKind::PyDefLike = { "class_definition", "decorated_definition", "function_definition" };
const NodeKind::NodeTypeSet NodeKind::PySplittableExpr = { "binary_operator", "boolean_operator", "comparison_operator" };
const NodeKind::NodeTypeSet NodeKind::PythonAsClause = { "except_clause", "with_item" }; // Py except E as e // Py with f() as x
const NodeKind::NodeTypeSet NodeKind::PythonBareList = { "expression_list", "pattern_list" }; // Py x = 1, 2, // Py a, b, = c

const NodeKind::NodeTypeSet NodeKind::PythonBareTupleHost =
{ "block", "delete_statement", "module", "return_statement", "yield" };

const NodeKind::NodeTypeSet NodeKind::PythonConditionHost = { "elif_clause", "if_statement", "while_statement" };
const NodeKind::NodeTypeSet NodeKind::PythonNumber = { "float", "integer" }; // Py (1.5).real // Py (1).bit_length
const NodeKind::NodeTypeSet NodeKind::PythonParenOnlyElement = { "named_expression", "yield" };
const NodeKind::NodeTypeSet NodeKind::PythonPostfix = { "attribute", "call" }; // Py (a + b).c // Py (a + b)(c)
const NodeKind::NodeTypeSet NodeKind::PythonReturnValueHost = { "return_statement", "yield" };
const NodeKind::NodeTypeSet NodeKind::PythonRightValueHost = { "assignment", "augmented_assignment", "for_statement" };

const NodeKind::NodeTypeSet NodeKind::PythonTargetHost =
{ "assignment", "augmented_assignment", "for_in_clause", "for_statement" };

const NodeKind::NodeTypeSet NodeKind::PythonUnwrappedChild = { "assignment", "augmented_assignment", "block" };

const NodeKind::NodeTypeSet NodeKind::PythonValueRoot = {
	"attribute",
	"await",
	"binary_operator",
	"boolean_operator",
	"call",
	"comparison_operator",
	"concatenated_string",
	"conditional_expression",
	"expression_list",
	"generic_type",
	"lambda",
	"not_operator",
	"subscript",
	"tuple_expression",
	"unary_operator"
};

// 値式を子に持つ Python の文（子の値式を包む）
const NodeKind::NodeTypeSet NodeKind::PythonValueStatement = {
	"assert_statement",
	"delete_statement",
	"expression_statement",
	"if_clause",
	"match_statement",
	"raise_statement",
	"return_statement",
	"yield" // Py yield v
};

// 包まない Python の範囲（ラムダの仮引数と型注釈は別に扱う）
const NodeKind::NodeTypeSet NodeKind::PythonWrapOpaque = { "lambda_parameters", "type" }; // Py lambda a, b: x // Py x: int

// 括弧外で折り返す為に包む Python の値式（呼出・配列の既存括弧は其の内側で分割する）
const NodeKind::NodeTypeSet NodeKind::PythonWrappable = {
	"attribute",
	"binary_operator",
	"boolean_operator",
	"comparison_operator",
	"concatenated_string",
	"conditional_expression",
	"expression_list",
	"generic_type",
	"lambda",
	"pattern_list",
	"tuple_expression"
};

// 範囲式・範囲パターン（Rust `0..n` — `..` 両側の密着判定）
const NodeKind::NodeTypeSet NodeKind::RangeLike = { "range", "range_expression", "range_pattern" };
// 範囲の演算子（終端の無い範囲の判定）
const NodeKind::NodeTypeSet NodeKind::RangeOperator = { "..", "..." }; // Rb/Kt/Rust 1.. // Rb/Swift 1...

// Kotlin の増減式及び Swift の範囲式／単項演算子親
const NodeKind::NodeTypeSet NodeKind::RangeOrUnaryExpression =
{ "open_end_range_expression", "open_start_range_expression", "postfix_expression", "prefix_expression", "range_expression" };

// 明示 return・文書コメント・return 直前コメント検査の関数様ノード
const NodeKind::NodeTypeSet NodeKind::ReturnOrDocFunctionLike =
{ "function_declaration", "function_definition", "lambda_expression", "method_declaration", "method_definition" };

// `Lint::TerminatesAlways` が判定する関数の制御終端文
const NodeKind::NodeTypeSet NodeKind::ReturnOrThrow = { "return_statement", "throw_statement" }; // 共通 return // 共通 throw
// 値を返す文（値の全体を包む括弧は，マクロの展開後も値を変えない）
const NodeKind::NodeTypeSet NodeKind::ReturnStatement = { "co_return_statement", "return_statement" };

// 戻値が組型のノード
const NodeKind::NodeTypeSet NodeKind::ReturnTuple =
{ "func_literal", "function_declaration", "function_type", "method_declaration", "method_elem" };

// return の返す値の参照の有無を決める関数（C++ の decltype(auto) を返す関数・PHP の参照を返す関数）
const NodeKind::NodeTypeSet NodeKind::ReturnValueHost =
{ "anonymous_function", "arrow_function", "function_definition", "lambda_expression", "method_declaration" };

// 返す値を直接の子に持つ節点（PHP の参照を返す関数の値を包む括弧の保持）
const NodeKind::NodeTypeSet NodeKind::ReturnValueParent = { "arrow_function", "return_statement" };
// 名前を略して転送出来る Ruby の実引数（`&` / `*` / `**` だけの形）
const NodeKind::NodeTypeSet NodeKind::RubyAnonymousForward = { "block_argument", "hash_splat_argument", "splat_argument" };
// 命令形の呼出の実引数が左端から続く Ruby の式（実引数は式の終わり迄続く）
const NodeKind::NodeTypeSet NodeKind::RubyArgumentExtent = { "binary", "call", "conditional", "element_reference", "range" };
// 空白に依らず後続を実引数と読む Ruby の語（`return + x` も `return(+x)`）
const NodeKind::NodeTypeSet NodeKind::RubyArgumentKeyword = { "break", "next", "return" };
// 実引数の並びを子に持つ Ruby の節点（並びの括弧の読みを揃える）
const NodeKind::NodeTypeSet NodeKind::RubyArgumentListHost = { "break", "call", "next", "return", "yield" };
// Ruby の要素の並び（名前の後の実引数が並びの残りの要素を取り込む）
const NodeKind::NodeTypeSet NodeKind::RubyArgumentSequence = { "argument_list", "right_assignment_list" };
// 名前の後に空白を置き，直後に空白を置かずに続けると実引数の始まりに為る Ruby の字句
const NodeKind::NodeTypeSet NodeKind::RubyArgumentStart = { "&", "*", "**", "+", "-", "<<", "[" };
// Ruby `%w[...]` / `%i[...]` 配列リテラル親（内部の `%w(` `%i(` `)` は密着必須）
const NodeKind::NodeTypeSet NodeKind::RubyArrayLiteral = { "string_array", "symbol_array" }; // Rb %w[a b c] // Rb %i[:a :b]
// 括弧を省ける Ruby の並び（`Edit::CollectRubySemicolonEdits` が文末の開いた字句を括弧で閉じる）
const NodeKind::NodeTypeSet NodeKind::RubyBareList = { "argument_list", "method_parameters" }; // Rb foo a, b // Rb def f a, b

// Ruby の名前束縛式は修飾子化で解析順が変わる為に保つ
const NodeKind::NodeTypeSet NodeKind::RubyBinding =
{ "assignment", "match_pattern", "operator_assignment", "regex", "test_pattern" };

// Ruby 制御ブロック（end 区切り）
const NodeKind::NodeTypeSet NodeKind::RubyBlock =
{ "begin", "block", "case", "case_match", "class", "do_block", "for", "if", "lambda", "module", "unless", "until", "while" };

// Ruby の本体直前を強制改行するコンテナ
const NodeKind::NodeTypeSet NodeKind::RubyBodyContainer = { "begin", "block", "class", "do_block", "module" };
// Ruby の body_statement 直下で兄弟となる rescue / ensure（def...rescue...ensure...end の暗黙 return 対象外）
const NodeKind::NodeTypeSet NodeKind::RubyBodyTailClause = { "ensure", "rescue" }; // Rb ensure 節 // Rb rescue 節
// `Structure::GetStructuredText` の本体構造化対象となる Ruby の `then` / `else` / `elsif`（`ensure` は自身でなく親型で判定）
const NodeKind::NodeTypeSet NodeKind::RubyBodyTrigger = { "else", "elsif", "then" }; // Rb else // Rb elsif cond // Rb then
// １文なら１行に置く Ruby の波括弧ブロックと本体
const NodeKind::NodeTypeSet NodeKind::RubyBraceBlock = { "block", "block_body" };
// Ruby IndentBlock の括弧付限定対象（括弧無の `render json: x` は IndentBlock 化禁止）
const NodeKind::NodeTypeSet NodeKind::RubyBracketIndentBlock = { "argument_list", "array", "hash", "method_parameters" };
// Ruby の分岐（構造化が節の本体を１行に畳み得る）
const NodeKind::NodeTypeSet NodeKind::RubyBranch = { "case", "if", "unless" };
// Ruby の分岐の節（節の語と本体を子に持つ）
const NodeKind::NodeTypeSet NodeKind::RubyBranchClause = { "else", "elsif", "in_clause", "then", "when" };
// Ruby 連鎖文字列の分裂残骸種別（行継続除去で chained_string が分裂した残骸の可能性 — 同型連続時は暗黙 return 付与の対象外）
const NodeKind::NodeTypeSet NodeKind::RubyChainedStringLike = { "heredoc_beginning", "string" }; // Rb <<~TAG // Rb "..."
// Ruby class 定義系（`< 親` 継承記法のスペース調整対象）
const NodeKind::NodeTypeSet NodeKind::RubyClassDefLike = { "class", "singleton_class" }; // Rb class C < Base // Rb class << obj
// Ruby の `class` 系（tree-sitter Ruby の `class X end` の `ERROR` 回避対象 — 本体直前／末尾に必ず改行を入れる）
const NodeKind::NodeTypeSet NodeKind::RubyClassLike = { "begin", "class", "module" }; // Rb begin // Rb class // Rb module

// 式を始めれない Ruby の節・閉じの語（行末の範囲の後で次の行の式が続かない印）
const NodeKind::NodeTypeSet NodeKind::RubyClauseWord =
{ "do", "else", "elsif", "end", "ensure", "in", "rescue", "then", "when" };

// Ruby で括弧除去に依り実引数の読みが変わる節点
const NodeKind::NodeTypeSet NodeKind::RubyCommandArgumentHost =
{ "binary", "break", "call", "element_reference", "next", "range", "return", "yield" };

// Ruby 条件分岐終端節キーワード + 例外節（when / elsif / rescue 何れも本体多行判定に同一基準を適用）
const NodeKind::NodeTypeSet NodeKind::RubyCondOrRescue = { "elsif", "in_clause", "rescue", "when" };
// Structure の平坦化時に本体改行を補う Ruby の条件分岐終端節
const NodeKind::NodeTypeSet NodeKind::RubyCondTerminator = { "elsif", "in_clause", "when" }; // Rb elsif cond // Rb case の when

// 子が開始行と別行なら継続字下げ＋１の Ruby 節点（`puts a,\n b` の括弧無 argument_list は IndentBlock=false の不足分を補完）
const NodeKind::NodeTypeSet NodeKind::RubyContinuation = {
	"argument_list",
	"assignment",
	"binary",
	"call",
	"conditional",
	"element_reference", // Rb arr[idx]（`[` で分割された時に idx を +１段インデント）
	"exceptions", // Rb rescue X, Y, Z（例外クラス列が複数行に渡る時に rescue 行から +１段インデント）
	"if_modifier",
	"operator_assignment", // Rb x ||= expr / x += expr 等（右辺が改行後に出現する場合に左辺行から +１段インデント）
	"pair", // Rb key: value（値が改行後に出現する場合にキー行から +１段インデント）
	"rescue_modifier",
	"unless_modifier",
	"until_modifier",
	"while_modifier" // Rb expr while cond
};

// Ruby の区切り文字付リテラル（開きが `%s{` 等の複数字句で閉じが１字句の為，括弧の深さ計算で内部へ降りない）
const NodeKind::NodeTypeSet NodeKind::RubyDelimitedLiteral = { "delimited_symbol", "regex", "subshell" };
// Ruby else / ensure 節キーワード（Structure GetStructuredText で親型側の本体扱い IsBody 判定対象）
const NodeKind::NodeTypeSet NodeKind::RubyElseEnsure = { "else", "ensure" }; // Rb else 節 // Rb ensure 節
// Ruby の式の接続を成す節点（`Structure::RubyExpressionShape` が構造化の前後で型と親型の並びを照合する）
const NodeKind::NodeTypeSet NodeKind::RubyExpressionLink = { "binary", "call", "element_reference", "unary" };
// Ruby の脱出式（値を持たず，範囲式の始端に置くと `return` の前置で意味が壊れる）
const NodeKind::NodeTypeSet NodeKind::RubyFlowExpr = { "break", "next", "redo", "retry", "return", "yield" };

// 外の局所変数を見ない新しいスコープを開く Ruby の節点（修飾子形式へ畳む時の局所変数の有無の判定で遡りを止める）
const NodeKind::NodeTypeSet NodeKind::RubyFreshScope =
{ "class", "method", "module", "program", "singleton_class", "singleton_method" };

// 見出の子の後に本体を包む子を持つ Ruby 構文
const NodeKind::NodeTypeSet NodeKind::RubyHeadedBody = {
	"block",
	"class",
	"do_block",
	"elsif",
	"for",
	"if",
	"in_clause",
	"method",
	"module",
	"rescue",
	"singleton_class",
	"singleton_method",
	"unless",
	"until",
	"when",
	"while" // Rb while c ... end
};

// Ruby のキーワードで開き `end` で閉じる式と，既に修飾子形の式（`return` の前置や修飾子への畳込で構造が壊れる）
const NodeKind::NodeTypeSet NodeKind::RubyKeywordBlock = {
	"begin",
	"case",
	"case_match",
	"for",
	"if",
	"if_modifier", // Rb `x if c`（既に修飾子形で，更に畳むと修飾子が二重に掛かる）
	"unless",
	"unless_modifier",
	"until",
	"until_modifier",
	"while",
	"while_modifier" // Rb `x while c`
};

// 局所変数のスコープを開く Ruby の節点（ブロックは外の局所変数も見るが，内側で導入した局所変数は外から見えない）
const NodeKind::NodeTypeSet NodeKind::RubyLocalScope =
{ "block", "class", "do_block", "lambda", "method", "module", "program", "singleton_class", "singleton_method" };

// Ruby のロガー呼出を自動 return の対象から除くメソッド名
const NodeKind::NodeTypeSet NodeKind::RubyLoggerMethod = { "debug", "error", "fatal", "info", "warn" };
// 命令形の呼出の実引数の外に在る Ruby の語の二項演算子（文の単位で結ぶ）
const NodeKind::NodeTypeSet NodeKind::RubyLowBinary = { "and", "or" }; // Rb foo +1 and x // Rb foo +1 or x
// 代入より弱い Ruby の単項の語（括弧を外すと後続の演算子・三項を被演算子へ取り込む）
const NodeKind::NodeTypeSet NodeKind::RubyLowUnary = { "defined?", "not" }; // Rb defined? x // Rb not x
// `Structure::GetStructuredText` の本体配置・見出判定に使う Ruby メソッドの親型
const NodeKind::NodeTypeSet NodeKind::RubyMethodDef = { "method", "singleton_method" };

// Ruby の修飾子形の式（`x if c` 等）更に外側から畳むと条件が二つ並び読み解けなく為る
const NodeKind::NodeTypeSet NodeKind::RubyModifierExpr =
{ "if_modifier", "unless_modifier", "until_modifier", "while_modifier" };

// 修飾子形式（`<式> if <条件>`）へ畳める Ruby の分岐・繰返
const NodeKind::NodeTypeSet NodeKind::RubyModifierForm = { "if", "unless", "until", "while" };
// Ruby の rational/complex リテラル親種別（数値部と接尾子 `r`/`i`/`ri` の密着必須，`3r` → `3 r` 化で SyntaxError）
const NodeKind::NodeTypeSet NodeKind::RubyNumericLiteralSuffix = { "complex", "rational" }; // Rb 3i / 1+2i // Rb 3r / 1.5r
// Ruby の仮引数の並び（仮引数の名前は局所変数を導入する）
const NodeKind::NodeTypeSet NodeKind::RubyParameters = { "block_parameters", "lambda_parameters", "method_parameters" };
// Ruby のパターン照合の式（式の中で最も弱く，括弧を外すと代入等の左辺を取り込む）
const NodeKind::NodeTypeSet NodeKind::RubyPatternTest = { "match_pattern", "test_pattern" }; // Rb h => {a:} // Rb h in {a:}

// Ruby メソッド本体最終式に `return` 自動付与可能な単純式型（制御フロー式／既存 return 経路／代入は安全に包めない為，除外）
const NodeKind::NodeTypeSet NodeKind::RubyReturnSafe = {
	"array",
	"binary",
	"call",
	"class_variable",
	"complex",
	"conditional",
	"constant",
	"element_reference",
	"false",
	"float",
	"global_variable",
	"hash",
	"heredoc_beginning",
	"identifier",
	"instance_variable",
	"integer",
	"nil",
	"parenthesized_statements",
	"range",
	"rational",
	"regex",
	"scope_resolution",
	"self",
	"simple_symbol",
	"string",
	"string_array",
	"symbol_array",
	"true",
	"unary" // Rb -x / !flag
};

// `;` を改行へ置き換えない Ruby の親（置き換えると意味が変わる）
const NodeKind::NodeTypeSet NodeKind::RubySemicolonKeep = { "block_parameters", "parenthesized_statements" };
// Ruby 副作用専用メソッド名（標準出力／検査用）戻値が `nil` 等で利用されないと推定可能で，自動 return 付与対象から除外
const NodeKind::NodeTypeSet NodeKind::RubySideEffectMethod = { "p", "pp", "print", "puts" };
// Ruby で実引数と誤読される為に空白を要する二項演算子
const NodeKind::NodeTypeSet NodeKind::RubySpacedBinary = { "%", "&", "*", "**", "+", "-", "/", "<<" };

// Ruby の文を直置するコンテナ（値位置の分岐を修飾子化して，偽の時に代入迄省かれる意味変更を防止）
const NodeKind::NodeTypeSet NodeKind::RubyStatementHost =
{ "block_body", "body_statement", "do", "else", "elsif", "ensure", "program", "then" };

// Ruby の文の並び（区切り改行は子間に在る為，平坦化では実引数への誤読を防ぐ `;` で接続，条件と本体を持つ elsif は除外）
const NodeKind::NodeTypeSet NodeKind::RubyStatementSequence =
{ "begin_block", "block_body", "body_statement", "do", "else", "end_block", "ensure", "parenthesized_statements", "then" };

// 文・見出の終端改行を直下の字句に持つ Ruby 節点（括弧内も畳込不可，子の内側の改行は式継続）
const NodeKind::NodeTypeSet NodeKind::RubyTerminatorHost = {
	"begin",
	"begin_block",
	"block",
	"block_body",
	"body_statement",
	"case",
	"case_match",
	"class",
	"do",
	"do_block",
	"else",
	"elsif",
	"end_block",
	"ensure",
	"if",
	"in_clause",
	"method",
	"module",
	"parenthesized_statements",
	"rescue",
	"singleton_class",
	"singleton_method",
	"then",
	"unless",
	"when" // Rb case x when p ... 節
};

// Ruby の条件分岐の本体の節
const NodeKind::NodeTypeSet NodeKind::RubyThenElse = { "else", "then" }; // Rb else // Rb then
// Rust 代入式系（mut マーク検出対象）
const NodeKind::NodeTypeSet NodeKind::RustAssignmentLike = { "assignment_expression", "compound_assignment_expr" };

// Rust mut 削除候補の判定で式木から変更対象になり得る基底識別子を辿る経路
const NodeKind::NodeTypeSet NodeKind::RustBaseTraversal = {
	"field_expression",
	"generic_function",
	"index_expression",
	"parenthesized_expression",
	"try_expression",
	"unary_expression"
}; // Rust x.y // Rust f::<T> // Rust x[i] // Rust (x) // Rust x? // Rust !x / -x

const NodeKind::NodeTypeSet NodeKind::RustBlockLike = {
	"async_block",
	"block",
	"const_block",
	"for_expression",
	"gen_block",
	"if_expression",
	"loop_expression",
	"match_expression",
	"try_block",
	"unsafe_block",
	"while_expression" // Rust while c { }
};

// Rust の let の束縛（パターンが名前を束縛する）
const NodeKind::NodeTypeSet NodeKind::RustLetBinding = { "let_condition", "let_declaration" };
// Rust のマクロが受け取る字句列（マクロの規則が字句の並びを照合する為，末尾コンマ１つでも照合結果が変わり得る）
const NodeKind::NodeTypeSet NodeKind::RustMacroTokens = { "macro_definition", "token_tree" };
// Rust の参照・フィールドのパターン（`ref mut` を探す）
const NodeKind::NodeTypeSet NodeKind::RustRefPattern = { "field_pattern", "ref_pattern" };
// 文頭の式を持つ Rust の節点（式文と，末尾の式を持つ塊）
const NodeKind::NodeTypeSet NodeKind::RustStatementHead = { "block", "expression_statement" }; // Rust { a; b } の b // Rust a;

// 型の被演算子を取る Rust の型（`+` で境界を並べた型を括弧無で置くと境界が外へ掛かる）
const NodeKind::NodeTypeSet NodeKind::RustTypeOperatorHost =
{ "abstract_type", "bounded_type", "dynamic_type", "pointer_type", "reference_type" };

// 値を取り得る Rust の脱出の式（値を省くと後続の式を値として取り込む）
const NodeKind::NodeTypeSet NodeKind::RustValueJump = { "break_expression", "return_expression", "yield_expression" };
// 言語固有の範囲本体
const NodeKind::NodeTypeSet NodeKind::ScopeBody = { "block", "block_body", "rule_set", "statements", "switch_body" };

// 関数／メソッド／プログラム本体スコープのコンテナ
const NodeKind::NodeTypeSet NodeKind::ScopeContainer = {
	"block",
	"compound_statement",
	"constructor_body",
	"function_body",
	"module",
	"program",
	"source_file",
	"statement_block",
	"statements",
	"translation_unit" // C/C++ ファイル全体
};

// ミックスインの定義と取込の SCSS の規則（空の仮引数・実引数の括弧を省ける）
const NodeKind::NodeTypeSet NodeKind::ScssMixinRule = { "include_statement", "mixin_statement" };
// 仮引数の並びを持つ SCSS の規則（解析器は空の並び `()` に空の仮引数を補って `MISSING` を立てる）
const NodeKind::NodeTypeSet NodeKind::ScssParameterHost = { "function_statement", "mixin_statement" };

// セクション見出（case/default／ラベル／アクセス修飾子等）
const NodeKind::NodeTypeSet NodeKind::SectionHeader = {
	"access_specifier",
	"case_statement",
	"communication_case",
	"default_case",
	"default_statement", // PHP default:（C/C++ は default も case_statement で表現される）
	"else",
	"elsif",
	"ensure",
	"expression_case",
	"in_clause",
	"labeled_statement",
	"named_label_statement",
	"rescue",
	"statement_label",
	"switch_block_statement_group",
	"switch_case",
	"switch_default",
	"switch_entry",
	"switch_label",
	"switch_section",
	"type_case",
	"when" // Rb when 1
};

// セクション親ノード（内側に SectionHeader を含むコンテナ）
const NodeKind::NodeTypeSet NodeKind::SectionParent = {
	"begin",
	"case",
	"case_match",
	"class_body",
	"compound_statement",
	"expression_switch_statement",
	"field_declaration_list",
	"for",
	"if",
	"select_statement",
	"switch_block",
	"switch_body",
	"switch_statement", // Swift { case .x: }（TS は switch_body が包装ノードの為，子は section_header にならず無効）
	"type_switch_statement",
	"unless",
	"until",
	"while" // Rb while x end
};

// 区切りの字句（中置演算子として空白で囲まない）
const NodeKind::NodeTypeSet NodeKind::SeparatorToken = { ",", ":", ";" }; // 共通 a, b // 共通 a: b // 共通 a; b

// 逐次実行される文の直接コンテナ（`FunctionBodyContainer` ∪ `statements` の統合済集合 — `Lint::TerminatesAlways`）
const NodeKind::NodeTypeSet NodeKind::SequentialStmtContainer =
{ "block", "compound_statement", "constructor_body", "function_body", "statement_block", "statements" };

// 単純型葉ノード（Lint の型注釈及び仮引数型の判定）
const NodeKind::NodeTypeSet NodeKind::SimpleTypeLeaf =
{ "integral_type", "predefined_type", "primitive_type", "type_identifier" };

// 単一インデントを必要とするノード（if / for / while 等の単文本体対象）
const NodeKind::NodeTypeSet NodeKind::SingleIndented = {
	"catch_clause",
	"do_statement",
	"do_while_statement",
	"else_clause",
	"else_if_clause",
	"enhanced_for_statement",
	"fixed_statement",
	"for_in_statement",
	"for_range_loop",
	"for_statement",
	"foreach_statement",
	"if_expression",
	"if_statement",
	"labeled_statement",
	"lock_statement",
	"using_statement",
	"while_statement"
};

// sizeof の実引数として包み直さない括弧始まりの節点（`sizeof(int)+1` のキャスト誤読を `sizeof((int)+1)` に変えない）
const NodeKind::NodeTypeSet NodeKind::SizeofParenArgument = { "cast_expression", "parenthesized_expression" };

// スペース正規化を行わないノード（リテラル／コメント／特殊構文等）
const NodeKind::NodeTypeSet NodeKind::Skip = {
	"attribute_value",
	"block_comment",
	"char_literal",
	"character_literal",
	"comment",
	"default_parameter",
	"format_specifier",
	"heredoc_body",
	"heredoc_content",
	"interpolated_string_expression",
	"interpolation",
	"interpreted_string_literal",
	"keyed_element",
	"keyword_argument",
	"keyword_pattern",
	"label",
	"lifetime",
	"line_comment",
	"macro_definition",
	"optional_parameter",
	"pair",
	"property_signature",
	"pseudo_class_selector",
	"pseudo_element_selector",
	"public_field_definition",
	"quoted_attribute_value",
	"raw_string_literal",
	"regex",
	"regex_pattern",
	"required_parameter",
	"script_element",
	"slice",
	"string",
	"string_fragment",
	"string_literal",
	"string_value",
	"style_element",
	"template_string",
	"text",
	"token_tree",
	"token_tree_pattern",
	"trait_bounds",
	"type_annotation",
	"typed_default_parameter",
	"typed_parameter",
	"user_defined_literal" // C++ "hello"s
};

// Skip 集合内でインデント走査時のみ子へ再帰する例外（pair/keyed_element／各種パラメータ／添字の範囲）
const NodeKind::NodeTypeSet NodeKind::SkipButRecurseForLayout = {
	"default_parameter",
	"keyed_element",
	"keyword_argument",
	"keyword_pattern",
	"optional_parameter",
	"pair",
	"required_parameter",
	"slice",
	"typed_default_parameter",
	"typed_parameter" // Py def f(x: int)
};

// 子間の隙間編集を行わないノード
const NodeKind::NodeTypeSet NodeKind::SkipGapEdit = {
	"document",
	"element",
	"hash_splat_argument",
	"interpolated_string_expression",
	"script_element",
	"splat_argument",
	"style_element",
	"template_string",
	"template_substitution" // JS/TS ${expr}
};

// スペース付与キーワード
const NodeKind::NodeTypeSet NodeKind::SpaceKeyword = {
	"as",
	"assert",
	"async",
	"await",
	"case",
	"co_await",
	"co_return",
	"co_yield",
	"defined?",
	"del",
	"delete",
	"extends",
	"from",
	"import",
	"in",
	"let",
	"new",
	"not",
	"of",
	"print",
	"raise",
	"return",
	"throw",
	"typeof",
	"void",
	"yield"
};

// `{` 前後の空白を許容しないノード
const NodeKind::NodeTypeSet NodeKind::SpacelessBrace = { "jsx_expression" }; // JS/TS {expr}

// 文ブロック直下で統合変数宣言を別宣言へ分割しても構文上安全な親（兄弟文追加が常に有効）
const NodeKind::NodeTypeSet NodeKind::SplittableDeclParent = {
	"block",
	"compound_statement",
	"constructor_body",
	"declaration_list", // C/C++/C#/Rust（波括弧の無い `extern "C" int a, b;` は分けた宣言子が C の結合を失う為に除く）
	"namespace_definition",
	"program",
	"statement_block",
	"translation_unit" // C/C++ ファイル全体
};

// 文が直接置かれるコンテナ（此処に在る式は文で，値として使われて居ない）
const NodeKind::NodeTypeSet NodeKind::StatementHost = {
	"block",
	"block_body",
	"body_statement",
	"compound_statement",
	"control_structure_body",
	"program",
	"source_file",
	"statement_block",
	"statement_list",
	"statements",
	"translation_unit" // C/C++ ファイル全体
};

// 文リストノード（Go / Kt 等の括弧付宣言グループや Ruby のメソッド本体）
const NodeKind::NodeTypeSet NodeKind::StatementList = {
	"body_statement",
	"colon_block",
	"const_declaration",
	"enum_body_declarations",
	"import_spec_list",
	"statement_list",
	"statements",
	"type_declaration",
	"var_declaration",
	"var_spec_list" // Go
};

// 文列の前でだけ改行し，見出の子（パターン・仮引数）を同じ行に保つ節
const NodeKind::NodeTypeSet NodeKind::StatementsOnlyBreak = { "catch_block", "switch_entry" };
// 式値を使わない純粋な文文脈の親
const NodeKind::NodeTypeSet NodeKind::StmtCtxParent = { "expression_statement", "statements" };
// 文字列の内側テキスト子（引用符正規化での `"` 含有判定対象）
const NodeKind::NodeTypeSet NodeKind::StringContentLeaf = { "string_content", "string_fragment" };
// 文字列を開閉する引用符の字句（`ERROR` の直接の子に在れば，文字列が閉じずに後続を飲み込んだ事を示す）
const NodeKind::NodeTypeSet NodeKind::StringDelimiter = { "\"", "\"\"\"", "'", "'''", "`" };

// Lint 等の言語横断判定に使う，実効的な文字列値の全種
const NodeKind::NodeTypeSet NodeKind::StringLikeAll = {
	"interpolated_string_expression",
	"line_string_literal",
	"raw_string_literal",
	"string",
	"string_literal",
	"string_value",
	"template_string"
}; // C# $"..." // Kt "..." // Rust r"..." // Rb/Py/JS/TS // C/C++/Java // CSS // JS/TS `...`

const NodeKind::NodeTypeSet NodeKind::StringLikeInnerPreserve = {
	"block_comment",
	"comment",
	"heredoc_body",
	"interpolated_string_expression",
	"line_comment",
	"multi_line_string_literal",
	"multiline_comment",
	"raw_string_content",
	"string",
	"string_array", // Rb %w[a b]（内部の改行が要素区切り）
	"string_content",
	"symbol_array", // Rb %i[a b]（内部の改行が要素区切り）
	"template_string",
	"verbatim_string_literal" // C# @"..."
};

// 文字列の開きの前の接頭辞（閉じない文字列の検出で読み飛ばす）
const NodeKind::NodeTypeSet NodeKind::StringPrefix =
{ "$", "$$", "$@", "@", "@$", "L", "LR", "R", "U", "UR", "b", "br", "c", "cr", "r", "rb", "u", "u8", "u8R", "uR" };

// `LineSplit::CollectBreaks` の `IsTerminal` を拡張する文字列・シンボル列のコンテナ
const NodeKind::NodeTypeSet NodeKind::StringSeqContainer = { "concatenated_string", "string_array", "symbol_array" };

// `=` 周辺を密着する節点（PHP の declare_directive は代入でなく指令の実引数の為，PSR-12 の `declare(strict_types=1);` に統一）
const NodeKind::NodeTypeSet NodeKind::StripEq =
{ "attribute", "declare_directive", "default_parameter", "keyword_argument", "keyword_pattern", "setter" };

// 本体の波括弧を直接の子に持つ Swift の制御構文（本体の文を展開する）
const NodeKind::NodeTypeSet NodeKind::SwiftBraceBodyHost =
{ "do_statement", "for_statement", "guard_statement", "if_statement", "repeat_while_statement", "while_statement" };

// 見出の式を直接の子に持つ Swift の文・節（見出の式の全体を包む括弧は，本体と紛れるクロージャを持てば必須）
const NodeKind::NodeTypeSet NodeKind::SwiftHeaderExpressionHost =
{ "for_statement", "guard_statement", "if_statement", "switch_statement", "where_clause", "while_statement" };

// Swift の見出の式の前の字句（条件・束縛の値・反復の対象・switch の対象の直前）
const NodeKind::NodeTypeSet NodeKind::SwiftHeaderLead = { ",", "=", "guard", "if", "in", "switch", "while" };

// 見出の後に本体の波括弧が続く Swift の文（見出に括弧無で置いたクロージャが本体と紛れる）
const NodeKind::NodeTypeSet NodeKind::SwiftHeaderStatement =
{ "for_statement", "guard_statement", "if_statement", "switch_statement", "while_statement" };

// 名前付の節点に為る Swift の語（後の括弧を呼出の実引数と紛れない様に空白で区切る）
const NodeKind::NodeTypeSet NodeKind::SwiftKeywordNode = { "binding_pattern_kind", "value_binding_pattern", "where_keyword" };
// Swift の呼出・添字の後置式（任意連鎖の `?` を密着させる）
const NodeKind::NodeTypeSet NodeKind::SwiftPostfixCall = { "call_expression", "subscript_expression" };
// Swift の見出直後で本体と誤読された後置クロージャ
const NodeKind::NodeTypeSet NodeKind::SwiftStrayBlock = { "ERROR", "lambda_literal" };

// 型の被演算子を取る Swift の型（演算子を持つ型を括弧無で置くと結合が変わる）
const NodeKind::NodeTypeSet NodeKind::SwiftTypeOperatorHost =
{ "existential_type", "metatype", "opaque_type", "optional_type", "protocol_composition_type" };

// switch の本体（言語横断）
const NodeKind::NodeTypeSet NodeKind::SwitchBody = { "compound_statement", "switch_block", "switch_body" };
// C# の `goto case` / `goto default` の跳び先（`switch` の転送で，素通りを禁じる言語仕様が要求する）
const NodeKind::NodeTypeSet NodeKind::SwitchGotoTarget = { "case", "default" }; // C# goto case 1; // C# goto default;
// switch の節・文（中の return はコメントを省ける）
const NodeKind::NodeTypeSet NodeKind::SwitchCaseAncestor = { "case_statement", "switch_statement" };
// switch に類する文（全節の終端判定の対象）
const NodeKind::NodeTypeSet NodeKind::SwitchStatementLike = { "switch_statement", "when_expression" };
// switch 式及び C# の with 式
const NodeKind::NodeTypeSet NodeKind::SwitchOrWithExpression = { "switch_expression", "with_expression" };
// `Structure::GetStructuredText` で内部式の空白を再構築する JS / TS テンプレート文字列（`Unformattable` の例外）
const NodeKind::NodeTypeSet NodeKind::TemplateStringLike = { "template_string", "template_substitution" };

// 三項演算子系＋範囲 for 系（`?` `:` 周辺で特殊スペース処理対象として一括判定する為，統合）
const NodeKind::NodeTypeSet NodeKind::Ternary = {
	"base_class_clause",
	"base_list",
	"conditional",
	"conditional_expression",
	"conditional_type",
	"enhanced_for_statement",
	"field_initializer_list",
	"for_range_loop",
	"ternary_expression" // Java/JS/TS x ? a : b
};

// `then` / `do` 等の制御本体開始キーワード
const NodeKind::NodeTypeSet NodeKind::ThenDo = { "do", "then" }; // Rb do // Rb then

// 透過ノード（深さを増やさず子を其のまま展開）
const NodeKind::NodeTypeSet NodeKind::Transparent = {
	"block_body",
	"body_statement",
	"chained_string", // Rb 'a' "b" 隣接文字列リテラル連結（Ｎ個の子 string を親インデントで揃える）
	"compilation_unit",
	"program",
	"source_file",
	"statement_list",
	"statements",
	"translation_unit" // C/C++ (file)
};

// AttachComments の錨を解包する透過包装
const NodeKind::NodeTypeSet NodeKind::TransparentWrap = {
	"block",
	"compound_statement",
	"control_structure_body",
	"function_body",
	"parenthesized_expression",
	"statement_block",
	"statements" // Kt/Swift（文リスト）
};

// TS の型の表明（代入先・増減の被演算子に置くと括弧が要る）
const NodeKind::NodeTypeSet NodeKind::TsAssertion = { "as_expression", "satisfies_expression", "type_assertion" };
// 戻値型の中で条件型を再び許す TS の関数型
const NodeKind::NodeTypeSet NodeKind::TsFunctionType = { "constructor_type", "function_type" }; // TS new () => T // TS () => T
// 右端を貪欲に伸ばす TS の型（条件の型の比べる型・矢印関数の戻値の型に括弧無で置くと後の `?` `=>` を取り込む）
const NodeKind::NodeTypeSet NodeKind::TsGreedyType = { "conditional_type", "constructor_type", "function_type", "infer_type" };
// TS の型本体
const NodeKind::NodeTypeSet NodeKind::TsTypeBody = { "interface_body", "object_type" }; // Java/TS // TS { x: number }

// 型の被演算子を取る TS の型・式（演算子を持つ型を括弧無で置くと結合が変わる）
const NodeKind::NodeTypeSet NodeKind::TsTypeOperatorHost = {
	"array_type",
	"as_expression",
	"index_type_query",
	"intersection_type",
	"lookup_type",
	"optional_type",
	"readonly_type",
	"rest_type",
	"satisfies_expression",
	"union_type" // TS (() => void) | null
};

// １要素組 `(x,)` で末尾カンマ必須となる組系（組リテラルと括弧付式の構文木区別）
const NodeKind::NodeTypeSet NodeKind::TupleLike =
{ "tuple", "tuple_expression", "tuple_pattern", "tuple_struct_pattern", "tuple_type" };

// type_annotation の親として改行後配置対象となる親
const NodeKind::NodeTypeSet NodeKind::TypeAnnotationParent =
{ "optional_parameter", "property_signature", "required_parameter", "typed_default_parameter", "variable_declarator" };

// TS 単純型（`:` 後分割を許可，複合型 union / intersection / function は内側分割優先で含めない）
const NodeKind::NodeTypeSet NodeKind::TypeAnnotationSimpleType =
{ "array_type", "generic_type", "nested_type_identifier", "predefined_type", "type_identifier" };

// 要素の `<` の比較が後の要素の `>` と対に為り，型引数の並びと読まれ得る一覧の要素の位置 (TS / C# / C++)
const NodeKind::NodeTypeSet NodeKind::TypeArgumentLookalikeSlot =
{ "argument", "argument_list", "arguments", "array", "initializer_expression", "initializer_list" };

// 型を右に取る変換の式（後ろの `<` 等を型の続きと読む）
const NodeKind::NodeTypeSet NodeKind::TypeCastRight = { "as_expression", "satisfies_expression", "type_cast_expression" };
// `:` 後で分割候補にする鍵値対親
const NodeKind::NodeTypeSet NodeKind::TypeColonParent = { "pair", "pair_pattern", "property_signature" };

// 型表現ノード（戻値型・パラメータ型として現れる具体的な型，修飾子／属性／補助子は含まない）言語横断
const NodeKind::NodeTypeSet NodeKind::TypeExpression = {
	"array_type",
	"auto",
	"boolean_type",
	"decltype",
	"floating_point_type",
	"generic_type",
	"integral_type",
	"placeholder_type_specifier",
	"predefined_type",
	"primitive_type",
	"qualified_identifier",
	"sized_type_specifier",
	"template_type",
	"type_identifier",
	"user_type",
	"void_type" // Java void
};

// 演算子を持つ型（型の演算子の被演算子に括弧無で置くと結合が変わる）
const NodeKind::NodeTypeSet NodeKind::TypeOperatorInner = {
	"asserts",
	"bounded_type",
	"channel_type",
	"conditional_type",
	"constructor_type",
	"existential_type",
	"function_type",
	"index_type_query",
	"infer_type",
	"intersection_type",
	"nullable_type",
	"opaque_type",
	"optional_type",
	"protocol_composition_type",
	"readonly_type",
	"rest_type",
	"type_predicate",
	"union_type" // TS A | B
};

// 型注釈と初期化子を直接持ち，仮引数の並びで分割する宣言
const NodeKind::NodeTypeSet NodeKind::TypedBinding =
{ "class_parameter", "const_item", "function_declaration", "let_declaration", "static_item" };

// 前置単項演算子
const NodeKind::NodeTypeSet NodeKind::UnaryPre = {
	"pointer_expression",
	"prefix_expression",
	"prefix_unary_expression",
	"reference_expression",
	"unary",
	"unary_expression",
	"unary_op_expression",
	"unary_operator" // Py -x
};

// 被演算子との隣接空白を判定する前置式と更新式
const NodeKind::NodeTypeSet NodeKind::UnaryPreOrUpdate = {
	"negated_type",
	"not_operator",
	"pointer_expression",
	"prefix_expression",
	"prefix_unary_expression",
	"reference_expression",
	"reflect_expression", // C++26 ^^x（P2996 リフレクション演算子 — 前置で被演算子に密着）
	"unary",
	"unary_expression",
	"unary_op_expression",
	"unary_operator",
	"update_expression" // JS/TS ++x / x++
};

// 前置の単項式（C# は prefix_unary_expression で持つ）
const NodeKind::NodeTypeSet NodeKind::UnaryPrefixExpression = { "prefix_unary_expression", "unary_expression" };
// 前置単項の記号（キャスト風の括弧の後で密着させる）
const NodeKind::NodeTypeSet NodeKind::UnaryPrefixToken = { "!", "+", "-", "~" };

// 整形対象外ノード（内部構造を保持し原文の返戻）
const NodeKind::NodeTypeSet NodeKind::Unformattable = {
	"directive", // Swift `#if` 等の条件コンパイル指示子 — 行末改行が意味を持ち再構築で改行が失われると構文崩壊
	"heredoc_beginning",
	"heredoc_body",
	"macro_definition",
	"primary_constructor", // Kt class Foo(x: Int) — `constructor` キーワードが子に出ない
	"pure_virtual_clause", // C++ `= 0` — `0` が無名で子に出ず再構築で欠落する
	"template_literal_type",
	"template_string",
	"template_substitution",
	"template_type",
	"throws_clause", // Swift throws(E) — 解析器が `throws` を字句として持たず，子から組み直すとキーワードが消える
	"type_alias_statement" // Py 3.12 `type X = ...` — `type(x)` 関数呼出も誤判定する事があり括弧削除で構文崩壊
};

// 変数宣言ノード（DeclLike に let_declaration / property_declaration を含めた拡張版）言語横断
const NodeKind::NodeTypeSet NodeKind::VariableDeclaration = {
	"declaration",
	"let_declaration",
	"lexical_declaration",
	"local_variable_declaration",
	"property_declaration",
	"variable_declaration" // JS/TS var x = ...
};

// 子から組み直さず原文を写す節点（`ERROR` は構造が信用出来ず，`shebang_line` は解釈系の経路を子に持たない）
const NodeKind::NodeTypeSet NodeKind::VerbatimNode = { "ERROR", "shebang_line" };
