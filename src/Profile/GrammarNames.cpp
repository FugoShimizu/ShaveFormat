#include "GrammarNames.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

/**
 * ========== 集合操作 ==========
 * ハッシュ値算出関数
 * 計算量：型名の長さ L に対し O(L)
 * @param Type 対象文字列
 * @return ハッシュ値
 */
size_t GrammarProfile::Names::NameSet::HashOf(const std::string_view Type) {
	// FNV-1a ６４ビット：オフセット基底に各バイトを排他的論理和して素数を乗算
	uint64_t Hash = 14695981039346656037ULL;
	for(const unsigned char Char : Type) Hash = 1099511628211ULL * (Hash ^ Char);
	// 計算済ハッシュ値の返戻
	return static_cast<size_t>(Hash);
}

/**
 * 要素列からの構築関数
 * 各コンストラクタが同じ表を作る為，インデックスの作方を此処だけで定める
 * 計算量：要素数 N，型名の長さの和 L に対し平均 O(N + L)，衝突が続く場合は最悪 O(N² + L)
 * @param Items 収める要素の列（重複を含まない）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void GrammarProfile::Names::NameSet::Build(const std::vector<std::string_view> &Items) {
	// 各要素の先頭文字ビット＋長さビットを立てた６４ビット集合を事前計算（検索時の早期除外用）
	for(const std::string_view Item : Items) if(!Item.empty()) {
		FirstCharMask |= 1ULL << (static_cast<unsigned char>(Item[0]) & 0X3F);
		SizeMask |= 1ULL << std::min<size_t>(Item.size(), 63);
	}
	// ５要素以上は開番地表，４要素以下は要素列を線形探索する
	if(Items.size() > 4) {
		size_t Capacity = 8;
		while(Capacity < Items.size() << 1) Capacity <<= 1;
		Slots.assign(Capacity, std::string_view());
		for(const std::string_view Item : Items) {
			size_t Idx = HashOf(Item) & Capacity - 1;
			while(!Slots[Idx].empty()) Idx = Idx + 1 & Capacity - 1;
			Slots[Idx] = Item;
		}
	} else Slots = Items;
	// 終了
	return;
}

/**
 * 包含判定関数
 * 計算量：型名の長さ L，集合の要素数 N に対し平均 O(L)，衝突が続く場合は最悪 O(N L)
 * @param Type 判定対象
 * @return 含まれるなら true
 */
bool GrammarProfile::Names::NameSet::HasName(const std::string_view Type) const {
	// 寸法と先頭字の事前検査で含まれない場合の返戻（空文字列は寸法ビットで短絡し，先頭を読まない）
	if(
		const size_t Len = Type.size();
		Len > 63 || !(SizeMask >> Len & 0X1) || !(FirstCharMask >> (static_cast<unsigned char>(Type[0]) & 0X3F) & 0X1)
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
	// １〜４要素は分岐予測・ローカル性を活かす線形探索で，一致時に true の返戻
	for(const std::string_view Item : Slots) if(Item == Type) return true;
	// 何れの要素にも一致しない為 false の返戻
	return false;
}

/**
 * コンストラクタ
 * 計算量：要素数 N，型名の長さの和 L に対し平均 O(N + L)，衝突が続く場合は最悪 O(N² + L)
 * @param Initializer 収めるノードの型名
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
GrammarProfile::Names::NameSet::NameSet(const std::initializer_list<std::string_view> Initializer) : FirstCharMask(0),
SizeMask(0) {
	// 列挙した名前のインデックスの構築
	Build(std::vector<std::string_view>(Initializer));
	// 終了
	return;
}

/**
 * 和集合のコンストラクタ
 * 他の集合を土台とする集合を列挙で書くと土台の変更に追従せず乖離する為，計算で作る
 * 計算量：土台と追加分の要素数 N，型名の長さの和 L に対し平均 O(N + L)，衝突が続く場合は最悪 O(N² + N L)
 * @param Base 土台の集合
 * @param Extra 土台へ加えるノードの型名
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
GrammarProfile::Names::NameSet::NameSet(
	const NameSet &Base,
	const std::initializer_list<std::string_view> Extra
) : FirstCharMask(0), SizeMask(0) {
	// 土台の要素（５要素以上は空きを含む表）と追加分を集めた要素列
	std::vector<std::string_view> Items;
	Items.reserve(Base.Slots.size() + Extra.size());
	for(const std::string_view Item : Base.Slots) if(!Item.empty()) Items.push_back(Item);
	// 土台に在る要素は表へ２度入れない
	for(const std::string_view Item : Extra) if(!Base.HasName(Item)) Items.push_back(Item);
	Build(Items);
	// 終了
	return;
}

/**
 * ２つの集合と追加名を合成するコンストラクタ
 * 計算量：要素数 N，名前の長さの和 L に対し平均 O(N + L)，衝突が続く場合は最悪 O(N² + N L)
 * @param Base 土台の集合
 * @param Added 合成する集合
 * @param Extra 両集合へ加える名前
 * @throws std::bad_alloc 要素の表を確保出来ない場合
 * @throws std::length_error 要素の表の上限を超える場合
 */
GrammarProfile::Names::NameSet::NameSet(
	const NameSet &Base,
	const NameSet &Added,
	const std::initializer_list<std::string_view> Extra
) : FirstCharMask(0), SizeMask(0) {
	std::vector<std::string_view> Items;
	for(const std::string_view Item : Base.Slots) if(!Item.empty()) Items.push_back(Item);
	for(const std::string_view Item : Added.Slots) if(!Item.empty() && !Base.HasName(Item)) Items.push_back(Item);
	for(const std::string_view Item : Extra) if(!Base.HasName(Item) && !Added.HasName(Item)) Items.push_back(Item);
	Build(Items);
	// 終了
	return;
}

/**
 * ========== ノード型集合 ==========
 */
// アクセス修飾子又はアクセスセクションの公開範囲を持つメンバ
const GrammarProfile::Names::NameSet GrammarProfile::Names::AccessOrderMember = {
	"abstract_method_signature",
	"alias_declaration",
	"annotation_type_declaration",
	"class_declaration",
	"class_specifier",
	"compact_constructor_declaration",
	"companion_object",
	"const_declaration",
	"constant_declaration",
	"constructor_declaration",
	"conversion_operator_declaration",
	"delegate_declaration",
	"enum_declaration",
	"enum_specifier",
	"event_declaration",
	"event_field_declaration",
	"field_declaration",
	"friend_declaration",
	"function_declaration",
	"function_definition",
	"index_signature",
	"indexer_declaration",
	"init_declaration",
	"interface_declaration",
	"method_declaration",
	"method_definition",
	"method_signature",
	"object_declaration",
	"operator_declaration",
	"property_declaration",
	"public_field_definition",
	"record_declaration",
	"secondary_constructor",
	"struct_declaration",
	"struct_specifier",
	"subscript_declaration",
	"template_declaration",
	"type_definition",
	"typealias_declaration",
	"union_specifier",
	"using_declaration"
};

// 型の本体とは限らない本体
const GrammarProfile::Names::NameSet GrammarProfile::Names::AmbiguousBodyContainer =
{ "block_body", "body_statement", "declaration_list" };

// `<` `>` で囲まれる型／値リスト（`<` 直接連結＋内側スペース判定の共通集合）
const GrammarProfile::Names::NameSet GrammarProfile::Names::AngleBracketList = {
	"template_argument_list",
	"template_parameter_list",
	"type_argument_list",
	"type_arguments",
	"type_parameter_list",
	"type_parameters"
};

// 関数型・ラムダ・match の腕を成す言語横断の矢印トークン
const GrammarProfile::Names::NameSet GrammarProfile::Names::ArrowToken = { "->", "=>" };

// Kotlin と他言語を同じ変更文脈へ揃える代入式
const GrammarProfile::Names::NameSet GrammarProfile::Names::AssignmentExpr =
{ "assignment", "assignment_expression", "augmented_assignment", "augmented_assignment_expression" };

// await・try・unsafe・型キャストの式
const GrammarProfile::Names::NameSet GrammarProfile::Names::AwaitTryCastExpr =
{ "await_expression", "try_expression", "type_cast_expression", "unsafe_expression" };

// Java の寸法と C++ のラムダ捕獲で同じ隣接規則を使う `[` 直接連結ノード
const GrammarProfile::Names::NameSet GrammarProfile::Names::AttachBracket = { "dimensions", "lambda_capture_specifier" };

// `:`・`.` 又は型注釈の後置の指定を直前トークンに直接連結するノード
const GrammarProfile::Names::NameSet GrammarProfile::Names::AttachColonOrDot = {
	"adding_type_annotation",
	"asserts_annotation",
	"navigation_suffix",
	"omitting_type_annotation",
	"opting_type_annotation",
	"pseudo_class_selector",
	"pseudo_element_selector",
	"trait_bounds",
	"type_annotation",
	"type_predicate_annotation"
};

// `(` を直接連結する引数／パラメータリスト
const GrammarProfile::Names::NameSet GrammarProfile::Names::AttachParen = {
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

// 後続要素に付く属性・アノテーション・デコレータ
const GrammarProfile::Names::NameSet GrammarProfile::Names::AttachPrefix =
{ "annotation", "attribute_item", "attribute_list", "decorator", "marker_annotation", "modifiers" };

// 言語横断の二項演算又は其の連鎖のノード
const GrammarProfile::Names::NameSet GrammarProfile::Names::BinaryExpr = {
	"additive_expression",
	"binary",
	"binary_expression",
	"binary_operator",
	"bitwise_operation",
	"boolean_operator",
	"comparison_expression",
	"comparison_operator",
	"conjunction_expression",
	"disjunction_expression",
	"elvis_expression",
	"equality_expression",
	"infix_expression",
	"multiplicative_expression",
	"nil_coalescing_expression",
	"range_expression"
};

// 真偽値型の初期値を持ち得る宣言・フィールド・仮引数
const GrammarProfile::Names::NameSet GrammarProfile::Names::BoolInitCtx =
{ "declaration", "field_declaration", "optional_parameter_declaration" };

// `{` を型名直後に連結するリテラル
const GrammarProfile::Names::NameSet GrammarProfile::Names::BraceAttach =
{ "composite_literal", "compound_literal_expression" };

// 閉波括弧の後に続き本体のブロックを持つ節（else, catch, finally の節）— ブロックと節の間の独立したコメントのアンカーを節の本体の最初の文へ降ろす
const GrammarProfile::Names::NameSet GrammarProfile::Names::BraceClause =
{ "catch_block", "catch_clause", "else", "else_clause", "else_if_clause", "elsif", "finally_clause" };

// 空の本体を語へ密着させる型定義ノード（メンバを持つ本体は展開して空白で区切る）
// Go の `interface{}` / Go の `struct{}`
const GrammarProfile::Names::NameSet GrammarProfile::Names::BraceNoSpace = { "interface_type", "struct_type" };
// `[` 始まりの型ノード（Go / Rust / Swift の配列・スライス型と Swift の辞書型 — `[` 直前の隣接判定）
const GrammarProfile::Names::NameSet GrammarProfile::Names::BracketType = { "array_type", "dictionary_type", "slice_type" };

// `:` が三項の演算子，キー値の要素，範囲 for・assert の見出の区切の何れかである親（其の他の用途の `:` は分割候補にしない）
const GrammarProfile::Names::NameSet GrammarProfile::Names::BreakColonHost = {
	"argument", // C# / PHP f(name: val)
	"assert_statement", // Java の `assert x : msg`
	"conditional", // Ruby の `c ? a : b`
	"conditional_expression",
	"conditional_type", // TS の `A extends B ? C : D`
	"enhanced_for_statement",
	"field_initializer", // Rust P { x: val }
	"field_pattern", // Rust P { x: name }
	"for_range_loop",
	"keyed_element", // Go T{Key: val}
	"keyword_parameter", // Ruby def f(key: val)
	"keyword_pattern", // Ruby in {key: pattern}
	"lambda", // Python 無名関数 x: body
	"pair",
	"pair_pattern",
	"ternary_expression",
	"value_argument" // Swift f(label: val)
};

// Kotlin のラベル付 `break@` を含む脱出の語
const GrammarProfile::Names::NameSet GrammarProfile::Names::BreakKeyword = { "break", "break@" };
// 改行で意味が変わる隣接文字列と修飾形式の保護
const GrammarProfile::Names::NameSet GrammarProfile::Names::BreakOpaque = { "chained_string", "rescue_modifier" };

// 関数・メソッドの呼出（言語横断） — `ControlFlow::IsNoReturnCall`
const GrammarProfile::Names::NameSet GrammarProfile::Names::CallNode =
{ "call", "call_expression", "function_call_expression", "invocation_expression", "method_invocation" };

// 例外を捕捉する節（言語横断） — `ControlFlow::IsTryTerminating`
const GrammarProfile::Names::NameSet GrammarProfile::Names::CatchClause =
{ "catch_block", "catch_clause", "except_clause", "except_group_clause" };

// C/C++ キャスト `(type)` 内側に出現し得る型部位（括弧削除抑止対象，`(counter_t)-1` 等を二項式と誤判定する事の回避）
const GrammarProfile::Names::NameSet GrammarProfile::Names::CCastInner = {
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
const GrammarProfile::Names::NameSet GrammarProfile::Names::CCastTypeNameInner =
{ "identifier", "primitive_type", "sized_type_specifier", "type_descriptor", "type_identifier" };

// C / C++ の関数定義の宣言子の前に置く指定・属性 — `ControlFlow::IsNoReturnDeclared`
const GrammarProfile::Names::NameSet GrammarProfile::Names::CDeclSpecifier =
{ "attribute_declaration", "attribute_specifier", "ms_declspec_modifier", "storage_class_specifier", "type_qualifier" };

// C の宣言子を包む宣言子（関数定義の宣言子が関数の宣言子かを見る時に剥がす）
const GrammarProfile::Names::NameSet GrammarProfile::Names::CDeclaratorWrapper =
{ "attributed_declarator", "parenthesized_declarator", "pointer_declarator" };

// C/C++ の関数宣言子経路及びフィールドのメソッド判定用宣言子
const GrammarProfile::Names::NameSet GrammarProfile::Names::CFunctionOrFieldDeclarator =
{ "function_declarator", "init_declarator", "pointer_declarator", "reference_declarator" };

// 名前を宣言する C / C++ の宣言・フィールド・関数定義（宣言子を名前迄剥がして導入名を集める）
const GrammarProfile::Names::NameSet GrammarProfile::Names::CNameDecl =
{ "declaration", "field_declaration", "function_definition", "parameter_declaration" };

// if の後続の枝の内で自身の条件を持つ節 (elif_clause / else_if_clause) — 更に後続の枝を要する
const GrammarProfile::Names::NameSet GrammarProfile::Names::ConditionalBranchClause = { "elif_clause", "else_if_clause" };

// `(` で始まり得る C の宣言子（型との間を空白で区切，`int(*p)` を関数形式のキャストのテキストにしない）
const GrammarProfile::Names::NameSet GrammarProfile::Names::CParenDeclarator =
{ "array_declarator", "function_declarator", "init_declarator", "parenthesized_declarator" };

// 型の別名を宣言する C / C++ の宣言（同じファイルで宣言した型名を集める）
const GrammarProfile::Names::NameSet GrammarProfile::Names::CTypeAliasDecl = { "alias_declaration", "type_definition" };
// Java の矢印節と C 系のコロン節と Rust の照合の枝を同じ本体境界へ揃えるトークン
const GrammarProfile::Names::NameSet GrammarProfile::Names::CaseBodyOpener = { "->", ":", "=>" };

// switch の節（言語横断）
const GrammarProfile::Names::NameSet GrammarProfile::Names::CaseClause = {
	"case_clause",
	"case_statement",
	"communication_case",
	"default_case",
	"default_statement",
	"expression_case",
	"in_clause",
	"match_arm",
	"switch_block_statement_group",
	"switch_case",
	"switch_default",
	"switch_entry",
	"switch_rule",
	"switch_section",
	"type_case",
	"when",
	"when_entry"
};

// C 系の括弧キャストと Go の変換呼出を同じ密着判定へ渡すキャスト式
const GrammarProfile::Names::NameSet GrammarProfile::Names::CastLikeExpr = { "cast_expression", "type_cast_expression" };

// クラス本体ノード（アクセス修飾子順序検査対象）
const GrammarProfile::Names::NameSet GrammarProfile::Names::ClassBody =
{ "class_body", "declaration_list", "enum_body_declarations", "field_declaration_list" };

// クラス／構造体／インターフェース等のメンバを直接保持するコンテナ
const GrammarProfile::Names::NameSet GrammarProfile::Names::ClassBodyContainer = {
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
const GrammarProfile::Names::NameSet GrammarProfile::Names::ClassLike = {
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
	"interface_type",
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

// 公開範囲を持つメンバの本体を所有する型（匿名型・別形式の型も含む）
const GrammarProfile::Names::NameSet GrammarProfile::Names::ClassLikeStrict = {
	"abstract_class_declaration",
	"annotation_type_declaration",
	"anonymous_class",
	"class",
	"class_declaration",
	"class_specifier",
	"companion_object",
	"enum_declaration",
	"extension_declaration",
	"interface_declaration",
	"object_creation_expression",
	"object_declaration",
	"object_literal",
	"record_declaration",
	"struct_declaration",
	"struct_specifier",
	"trait_declaration",
	"union_specifier"
};

// 末尾コメントを最後の本体の後へ置く節
const GrammarProfile::Names::NameSet GrammarProfile::Names::ClauseBody = { "colon_block", "else_clause", "else_if_clause" };

// コメントをブロック末尾へ留める閉トークン（LayoutCloserToken と CloserWord の和に，前処理の条件を閉じる `#endif` を加えた物，何方かへの追加は此処にも反映）
// `CloserWord` は此の定義より後に在り，静的な初期化の順で空に為る為に計算で作れない
const GrammarProfile::Names::NameSet GrammarProfile::Names::CloserToken =
{ "#endif", ")", "/>", "</", ">", "]", "end", "enddeclare", "endfor", "endforeach", "endif", "endswitch", "endwhile", "}" };

// `:` 前スペース不要な制御構文（case／ラベル／ラムダ等）
const GrammarProfile::Names::NameSet GrammarProfile::Names::ColonControl = {
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
const GrammarProfile::Names::NameSet GrammarProfile::Names::CombinatorSelector =
{ "adjacent_sibling_selector", "child_selector", "descendant_selector", "sibling_selector" };

// コメントノード（JavaScript の `hash_bang_line` と Rust の `shebang` は標準の先頭記法 `#!` で，コメント経路で保持する）
const GrammarProfile::Names::NameSet GrammarProfile::Names::Comment = {
	"block_comment",
	"comment",
	"doc_comment",
	"hash_bang_line",
	"html_comment",
	"js_comment",
	"line_comment",
	"multiline_comment",
	"shebang"
};

// 内部スペース・改行を入れない密集型ノード（属性／単項演算子等）
const GrammarProfile::Names::NameSet GrammarProfile::Names::Compact = {
	"jsx_attribute",
	"keyword_argument",
	"pointer_expression",
	"prefix_unary_expression",
	"property_signature",
	"unary",
	"unary_expression"
};

// 密集したノードの内で，内部に折返の候補を持ち得るノード
const GrammarProfile::Names::NameSet GrammarProfile::Names::CompactRecurse = { "jsx_attribute", "property_signature" };
// 条件式を包む括弧ノード（Lint の条件文脈剥がし）
const GrammarProfile::Names::NameSet GrammarProfile::Names::CondParenWrap = { "condition_clause", "parenthesized_expression" };

// 条件を場 `cond` に持つノード（条件以外の子は条件の文脈でない）
const GrammarProfile::Names::NameSet GrammarProfile::Names::CondFieldHost =
{ "conditional_expression", "for_statement", "ternary_expression" };

// else の位置で自身が条件と本体を持つ分岐（else 節に包まれない else if の if と，PHP の elseif 節）
const GrammarProfile::Names::NameSet GrammarProfile::Names::ConditionalAlternative =
{ "else_if_clause", "if_expression", "if_statement" };

// 生成を最初の子に持つ後置の式（空の実引数を外すと後置の演算子が生成の対象の名前へ掛かる）
const GrammarProfile::Names::NameSet GrammarProfile::Names::ConstructionReceiverHost = {
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

// 継続の語（Kotlin はラベル付を `continue@` のトークンで持つ）
// Swift/Kotlin continue
// Kotlin continue@outer
const GrammarProfile::Names::NameSet GrammarProfile::Names::ContinueKeyword = { "continue", "continue@" };

// JS/TS, Go の継続語（後続トークンを要し，ブロックコメントの後の改行でも文を終えない語）
// 自動の `;` を補う語や，直後の改行で読みが変わる `return`・`let`・TS の `interface`・`type` 等は除く
const GrammarProfile::Names::NameSet GrammarProfile::Names::ContinuingKeyword = {
	"as",
	"case",
	"catch",
	"chan",
	"class",
	"const",
	"default",
	"defer",
	"delete",
	"do",
	"else",
	"export",
	"extends",
	"finally",
	"for",
	"from",
	"func",
	"function",
	"go",
	"goto",
	"if",
	"implements",
	"import",
	"in",
	"instanceof",
	"keyof",
	"map",
	"new",
	"of",
	"package",
	"range",
	"satisfies",
	"select",
	"struct",
	"switch",
	"try",
	"typeof",
	"var",
	"void",
	"while",
	"with"
};

// 制御構文ノード（条件 `()` の括弧維持対象，括弧保持集合の構成要素）
const GrammarProfile::Names::NameSet GrammarProfile::Names::Control = {
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
const GrammarProfile::Names::NameSet GrammarProfile::Names::ControlKeyword =
{ "catch", "elif", "elsif", "except", "for", "foreach", "if", "range", "switch", "when", "while" };

// C/C++ 宣言の型部位（後置 const 検出時の型認識対象）
const GrammarProfile::Names::NameSet GrammarProfile::Names::CppDeclTypeSpecifier =
{ "primitive_type", "qualified_identifier", "sized_type_specifier", "type_identifier" };

// C++ の型引数の遡りを止める範囲（文の本体とファイル）
const GrammarProfile::Names::NameSet GrammarProfile::Names::CppScopeBoundary = { "compound_statement", "translation_unit" };

// 型名を導入する C++ の指定子（同じファイルで宣言した型名を集める）
const GrammarProfile::Names::NameSet GrammarProfile::Names::CppTypeSpecifier =
{ "class_specifier", "enum_specifier", "struct_specifier", "union_specifier" };

// キャストにも式にも読める C# の単純名と限定名
const GrammarProfile::Names::NameSet GrammarProfile::Names::CsNameType = { "identifier", "qualified_name" };

// C# の後置式の連鎖（後置 `!` を含む）
const GrammarProfile::Names::NameSet GrammarProfile::Names::CsPostfixChain =
{ "element_access_expression", "invocation_expression", "member_access_expression", "postfix_unary_expression" };

// 内側の改行が値を変えない括弧の並び
const GrammarProfile::Names::NameSet GrammarProfile::Names::CssDelimitedList =
{ "arguments", "grid_value", "list_value", "parameters", "parenthesized_value" };

// 名前空間の区切 `|` を持つ CSS のセレクタのノード（`|` はセレクタのトークンの一部で空白を置けない）
const GrammarProfile::Names::NameSet GrammarProfile::Names::CssNamespacePrefix = { "attribute_name", "namespace_selector" };
// 整数と小数を直後の単位へ密着させる CSS/SCSS の数値値ノード
const GrammarProfile::Names::NameSet GrammarProfile::Names::CssNumericVal = { "float_value", "integer_value" };
// CSS/SCSS の分割不可セレクタ系（内部空白挿入禁止）
const GrammarProfile::Names::NameSet GrammarProfile::Names::CssSelectorAtomic = { "class_name", "id_name", "placeholder" };

// セレクタを取るノード（中の `#name` は色でなく ID セレクタで，大小を変えると別の要素を指す）
const GrammarProfile::Names::NameSet GrammarProfile::Names::CssSelectorHost =
{ "extend_statement", "pseudo_class_selector", "pseudo_element_selector" };

// CSS のセレクタの名前（`&` や `.` へ密着させる）
// CSS .a
// CSS #a
const GrammarProfile::Names::NameSet GrammarProfile::Names::CssSelectorName = { "class_name", "id_selector" };
// 前の値から空白で区切る CSS・SCSS の値の要素
const GrammarProfile::Names::NameSet GrammarProfile::Names::CssSpacedValue = { "grid_value" };
// 初期化子を伴う宣言子（初期化子の有無が混在する宣言の判別に用いる）
const GrammarProfile::Names::NameSet GrammarProfile::Names::DeclInitDeclarator = { "init_declarator", "variable_declarator" };

// 一括宣言可能な宣言ノード
const GrammarProfile::Names::NameSet GrammarProfile::Names::DeclLike = {
	"declaration",
	"field_declaration",
	"lexical_declaration",
	"local_declaration_statement",
	"local_variable_declaration",
	"variable_declaration"
};

// 宣言の型部位ノード
const GrammarProfile::Names::NameSet GrammarProfile::Names::DeclTypeChild = {
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

// 宣言の中で宣言子を始めるノード（Swift は名前の型式，其の他は宣言子）— 其の前の子を宣言子で共有する
const GrammarProfile::Names::NameSet GrammarProfile::Names::DeclaratorHead = { "pattern", "variable_declarator" };

// 括弧の有無で decltype の推論する型が変わる式（名前・メンバアクセスは括弧で包むと左辺値の参照に為る）
const GrammarProfile::Names::NameSet GrammarProfile::Names::DecltypeIdExpr =
{ "field_expression", "identifier", "qualified_identifier" };

// 切替の既定の枝のノード（語で既定を示す節は語で判定する）
const GrammarProfile::Names::NameSet GrammarProfile::Names::DefaultCase =
{ "default_case", "default_statement", "match_default_expression", "switch_default" };

// 分割代入パターン（単一名を持たない為，ドキュメント化引数名抽出／let 変異解析で除外）
const GrammarProfile::Names::NameSet GrammarProfile::Names::DestructurePattern = { "array_pattern", "object_pattern" };

// ドキュメントを付ける名前付の関数・コンストラクタ・デストラクタ・演算子の定義
const GrammarProfile::Names::NameSet GrammarProfile::Names::DocumentedDef = {
	"compact_constructor_declaration",
	"constructor_declaration",
	"conversion_operator_declaration",
	"deinit_declaration",
	"destructor_declaration",
	"function_declaration",
	"function_definition",
	"function_item",
	"generator_function_declaration",
	"init_declaration",
	"local_function_statement",
	"method",
	"method_declaration",
	"method_definition",
	"operator_declaration",
	"secondary_constructor",
	"singleton_method"
};

// 既存のドキュメントを付けれる型・メンバ・最上位の宣言（インポート・前処理のディレクティブ・ラッパーを含み，呼出・代入・名前だけのノードは配置の段が宣言として働く物に絞る）
const GrammarProfile::Names::NameSet GrammarProfile::Names::DocumentableDecl = {
	"abstract_class_declaration",
	"abstract_method_signature",
	"alias",
	"alias_declaration",
	"ambient_declaration",
	"annotation_type_declaration",
	"annotation_type_element_declaration",
	"associated_type",
	"associatedtype_declaration",
	"call_signature",
	"class",
	"class_declaration",
	"class_definition",
	"class_specifier",
	"compact_constructor_declaration",
	"companion_object",
	"concept_definition",
	"const_declaration",
	"const_item",
	"const_spec",
	"constant_declaration",
	"construct_signature",
	"constructor_declaration",
	"conversion_operator_declaration",
	"declaration",
	"declare_statement",
	"deinit_declaration",
	"delegate_declaration",
	"destructor_declaration",
	"enum_assignment",
	"enum_case",
	"enum_constant",
	"enum_declaration",
	"enum_entry",
	"enum_item",
	"enum_member_declaration",
	"enum_specifier",
	"enum_variant",
	"enumerator",
	"event_declaration",
	"event_field_declaration",
	"export_statement",
	"extern_alias_directive",
	"extern_crate_declaration",
	"field_declaration",
	"field_definition",
	"file_scoped_namespace_declaration",
	"friend_declaration",
	"function_declaration",
	"function_definition",
	"function_item",
	"function_signature",
	"function_signature_item",
	"generator_function_declaration",
	"impl_item",
	"import_declaration",
	"import_header",
	"import_list",
	"import_statement",
	"index_signature",
	"indexer_declaration",
	"init_declaration",
	"inner_attribute_item",
	"interface_declaration",
	"internal_module",
	"lexical_declaration",
	"linkage_specification",
	"local_function_statement",
	"macro_definition",
	"method",
	"method_declaration",
	"method_definition",
	"method_elem",
	"method_signature",
	"mod_item",
	"module",
	"module_declaration",
	"namespace_alias_definition",
	"namespace_declaration",
	"namespace_definition",
	"namespace_use_declaration",
	"object_declaration",
	"operator_declaration",
	"package_clause",
	"package_declaration",
	"package_header",
	"precedence_group_declaration",
	"preproc_call",
	"preproc_def",
	"preproc_function_def",
	"preproc_if",
	"preproc_ifdef",
	"preproc_include",
	"property_declaration",
	"property_signature",
	"protocol_declaration",
	"protocol_function_declaration",
	"protocol_property_declaration",
	"public_field_definition",
	"record_declaration",
	"secondary_constructor",
	"singleton_class",
	"singleton_method",
	"static_item",
	"struct_declaration",
	"struct_item",
	"struct_specifier",
	"subscript_declaration",
	"template_declaration",
	"trait_declaration",
	"trait_item",
	"type_alias",
	"type_alias_declaration",
	"type_declaration",
	"type_definition",
	"type_item",
	"type_spec",
	"typealias_declaration",
	"union_item",
	"union_specifier",
	"use_declaration",
	"using_declaration",
	"using_directive",
	"var_declaration",
	"var_spec",
	"variable_declaration"
};

// 文・メンバを並べ，其の直前へドキュメントを置ける親（値の位置へ置いた定義を除く為の集合，構造を確かめれない `ERROR` を含む）
const GrammarProfile::Names::NameSet GrammarProfile::Names::DocHostSeq = {
	"ERROR",
	"annotation_type_body",
	"block",
	"body_statement",
	"class_body",
	"compilation_unit",
	"compound_statement",
	"constructor_body",
	"control_structure_body",
	"declaration_list",
	"else",
	"elsif",
	"ensure",
	"enum_body",
	"enum_body_declarations",
	"enum_class_body",
	"enum_declaration_list",
	"field_declaration_list",
	"function_body",
	"if_modifier",
	"in_clause",
	"interface_body",
	"module",
	"object_type",
	"preproc_elif",
	"preproc_elifdef",
	"preproc_else",
	"preproc_if",
	"preproc_ifdef",
	"program",
	"protocol_body",
	"rescue",
	"rescue_modifier",
	"source_file",
	"statement_block",
	"statement_list",
	"statements",
	"switch_case",
	"switch_default",
	"switch_section",
	"then",
	"translation_unit",
	"unless_modifier",
	"until_modifier",
	"when",
	"while_modifier"
};

// ドキュメントコメントのアンカーを１階層包むラッパーノード（定義本体でなくラッパー側へ先行コメントが束縛される）
const GrammarProfile::Names::NameSet GrammarProfile::Names::DocWrapper = {
	"ambient_declaration",
	"decorated_definition",
	"export_statement",
	"friend_declaration",
	"global_statement",
	"linkage_specification",
	"template_declaration"
};

// 既定の後続枝（else 系）
// Ruby else
// C/C++/Rust/Python/JS/TS else { }
const GrammarProfile::Names::NameSet GrammarProfile::Names::ElseAny = { "else", "else_clause" };

// else 系の節
const GrammarProfile::Names::NameSet GrammarProfile::Names::ElseLikeClause =
{ "catch_clause", "else", "else_clause", "else_if_clause", "elsif", "finally_clause" };

// 関数を抜ける語（語で始まる脱出の文の終端判定）
const GrammarProfile::Names::NameSet GrammarProfile::Names::ExitKeyword = { "return", "throw", "throw_keyword" };

// BuildExpandedContainer 経路に乗る括弧系コンテナ（statement_block 子孫を持つ場合に子を改行展開する対象）
const GrammarProfile::Names::NameSet GrammarProfile::Names::ExpandContainer = {
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
const GrammarProfile::Names::NameSet GrammarProfile::Names::ExprStmtLiteral =
{ "char_literal", "false", "float_literal", "integer_literal", "number", "string_literal", "true" };

// 被呼出側のフィールドの参照（括弧で包むとフィールドの持つ関数の呼出，外すとメソッドの呼出に為る言語の括弧の保持）
const GrammarProfile::Names::NameSet GrammarProfile::Names::FieldCallee =
{ "field_expression", "member_access_expression", "navigation_expression" };

// 例外の有無に依らず実行する後始末の節（言語横断）
const GrammarProfile::Names::NameSet GrammarProfile::Names::FinallyClause = { "finally_block", "finally_clause" };

// 制御フロー脱出文（同一退出文を持つ if の候補検出にも使用）
const GrammarProfile::Names::NameSet GrammarProfile::Names::FlowExit =
{ "break_statement", "continue_statement", "goto_statement", "return_statement", "throw_statement" };

// 関数から戻る文・式（言語横断，Kotlin / Swift の脱出の文は先頭の語で別に判ずる）
const GrammarProfile::Names::NameSet GrammarProfile::Names::FlowReturn =
{ "co_return_statement", "return", "return_expression", "return_statement" };

// 反復の対象を区切 (`:` / `in` / `of`) の後に置く見出の文（見出の括弧は文のトークンで，対象を包む括弧は冗長）
const GrammarProfile::Names::NameSet GrammarProfile::Names::ForEachHeader =
{ "enhanced_for_statement", "for_in_statement", "for_range_loop", "foreach_statement" };

// 関数本体ノード（言語横断）
const GrammarProfile::Names::NameSet GrammarProfile::Names::FunctionBodyContainer =
{ "block", "compound_statement", "constructor_body", "function_body", "statement_block" };

// 定義の本体（文の並び・式の本体・Ruby の本文）— 見出の範囲は此の手前迄
const GrammarProfile::Names::NameSet GrammarProfile::Names::DefBody =
{ FunctionBodyContainer, { "arrow_expression_clause", "body_statement" } };

// 関数の範囲の境界（定義・関数式・ラムダ・ローカル関数，ネストの関数の中の `return` 等は外側の関数に属さない）
const GrammarProfile::Names::NameSet GrammarProfile::Names::FunctionLikeAny = {
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
const GrammarProfile::Names::NameSet GrammarProfile::Names::FunctionLikeDef = {
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
const GrammarProfile::Names::NameSet GrammarProfile::Names::FunctionNameIdentifier =
{ "constant", "identifier", "name", "simple_identifier" };

// 本体をジェネレータ・コルーチンにする式・文 — 通常関数の末尾の返戻の対象外
const GrammarProfile::Names::NameSet GrammarProfile::Names::GeneratorMarker =
{ "co_await_expression", "co_return_statement", "co_yield_statement", "yield", "yield_expression", "yield_statement" };

// JS/TS `*` 前置のジェネレータマーカーを持つ関数親
const GrammarProfile::Names::NameSet GrammarProfile::Names::GeneratorStarHost =
{ "generator_function", "generator_function_declaration", "method_definition" };

// 括弧無で型の変換に置ける Go の型（ポインタ・関数・チャネルの型は後の `(` と結合を争う）
const GrammarProfile::Names::NameSet GrammarProfile::Names::GoConversionType = {
	"array_type",
	"generic_type",
	"interface_type",
	"map_type",
	"qualified_type",
	"slice_type",
	"struct_type",
	"type_identifier"
};

// godoc が直前のコメントをドキュメントとする Go の最上位の宣言（パッケージのドキュメントを受けるパッケージ節を含む）
const GrammarProfile::Names::NameSet GrammarProfile::Names::GoDocumentedDecl =
{ "const_declaration", "function_declaration", "method_declaration", "package_clause", "type_declaration", "var_declaration" };

// 関数のディレクティブ（`//go:noinline` 等）と cgo の `//export` を受ける Go の関数宣言
const GrammarProfile::Names::NameSet GrammarProfile::Names::GoFunctionDecl = { "function_declaration", "method_declaration" };

// Canonicalizer で `(` 後・`)` 前を改行する Go の一括宣言
const GrammarProfile::Names::NameSet GrammarProfile::Names::GoGroupedDecl =
{ "const_declaration", "import_spec_list", "type_declaration", "var_spec_list" };

// Go の構造体・インタフェースの要素の並び（改行が区切を兼ねる）
const GrammarProfile::Names::NameSet GrammarProfile::Names::GoMemberList = { "field_declaration_list", "interface_type" };

// Go の多行展開時に末尾 `,` を補うコンテナ（gofmt 同様，自動セミコロン挿入に依る構文破壊を防止）
const GrammarProfile::Names::NameSet GrammarProfile::Names::MultilineCommaContainer =
{ "argument_list", "literal_value", "parameter_list", "type_arguments", "type_parameter_list" };

// Go の switch 文
const GrammarProfile::Names::NameSet GrammarProfile::Names::GoSwitch =
{ "expression_switch_statement", "type_switch_statement" };

// Go の型宣言ノード
const GrammarProfile::Names::NameSet GrammarProfile::Names::GoType = {
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
const GrammarProfile::Names::NameSet GrammarProfile::Names::GoTypeName =
{ "generic_type", "qualified_type", "type_identifier" };

// 右端を貪欲に伸ばす又は呼び出されて初めて括弧が要る式（後にトークンが続かない値の位置では括弧が冗長）
const GrammarProfile::Names::NameSet GrammarProfile::Names::GreedyTailExpr = {
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
const GrammarProfile::Names::NameSet GrammarProfile::Names::GroupingParen =
{ "parenthesized_expression", "parenthesized_statements", "tuple_expression" };

// 型を包む括弧のノード（TS / Kotlin / Go の parenthesized_type，Swift / Rust は要素１つの tuple_type が括弧に当たる）
const GrammarProfile::Names::NameSet GrammarProfile::Names::GroupingTypeParen = { "parenthesized_type", "tuple_type" };
// Ruby `**hash` 系ノード（`** hash` だと冪乗演算子と解釈され構文崩壊する為，識別子との密着が必須）
const GrammarProfile::Names::NameSet GrammarProfile::Names::HashSplat = { "hash_splat_argument", "hash_splat_parameter" };
// 通常要素と埋込 `script`･`style` を同じ開閉要素として包む
const GrammarProfile::Names::NameSet GrammarProfile::Names::HtmlElementWrap = { "element", "script_element", "style_element" };

// tree-sitter-html が１バイトの種別で保持する要素名
const GrammarProfile::Names::NameSet GrammarProfile::Names::HtmlKnownTag = {
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

// HTML の埋込 raw_text を持つ要素（Formatter の部分解析対象）
const GrammarProfile::Names::NameSet GrammarProfile::Names::HtmlRawTextElement = { "script_element", "style_element" };

// パーサーが内容を生の本文として読む HTML の要素の小文字のタグ名（内容の中のタグを数えない）
// HTML タグ系ノード
const GrammarProfile::Names::NameSet GrammarProfile::Names::HtmlTag =
{ "end_tag", "jsx_closing_element", "jsx_opening_element", "jsx_self_closing_element", "self_closing_tag", "start_tag" };

// 葉識別子相当（言語横断で名前参照ノード判定）名前の比較対象
// C/C++/Java/JS/TS 等
// Kotlin
const GrammarProfile::Names::NameSet GrammarProfile::Names::IdentifierLeafLike = { "identifier", "simple_identifier" };
// if 系のノード
const GrammarProfile::Names::NameSet GrammarProfile::Names::IfNode = { "if_expression", "if_statement" };

// インデントを付与するコンテナノード
const GrammarProfile::Names::NameSet GrammarProfile::Names::IndentContainer = {
	"accessor_list",
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
	"enumerator_list",
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
const GrammarProfile::Names::NameSet GrammarProfile::Names::InfixOperator = {
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

// 初期化の文を持つ見出（子の文の終わりの `;` を見出の区切と揃えて割る）
const GrammarProfile::Names::NameSet GrammarProfile::Names::InitHeader = { "condition_clause", "for_statement" };

// 整数リテラルの候補（数値の共通分類で全桁と型接尾辞を照合する）
const GrammarProfile::Names::NameSet GrammarProfile::Names::IntLiteralLike = {
	"decimal_integer_literal",
	"int_literal",
	"integer",
	"integer_literal",
	"long_literal",
	"number",
	"number_literal",
	"unsigned_literal"
};

// 反復対象を導く `in`, `as` 等を演算子から除く見出
const GrammarProfile::Names::NameSet GrammarProfile::Names::IterationHeader = {
	"for_expression",
	"for_in_clause",
	"for_in_statement",
	"for_statement",
	"foreach_statement",
	"from_clause",
	"in",
	"join_clause"
};

// 空の実引数を省ける Java のノード（アノテーションはマーカーのアノテーション，列挙の定数は引数の無いコンストラクタの呼出と同じ）
// Java @A()
// Java X()
const GrammarProfile::Names::NameSet GrammarProfile::Names::JavaEmptyArgHost = { "annotation", "enum_constant" };

// Java のトークン化前 Unicode エスケープを中身として包むトークン（`ParseAdapter::CanExpandJavaUnicodeEscapes` の展開要否判定）
const GrammarProfile::Names::NameSet GrammarProfile::Names::JavaEscapeHost =
// Java /* */
// Java 'a'
// Java
// // Java "s"
{ "block_comment", "character_literal", "line_comment", "string_literal" };

// 枝のラベルに置くと Java の切替へ網羅を要求する照合と null（処理系が網羅を検査し，全ての枝を抜ける切替の後は到達しない）
// Java case null
// Java case Circle c
const GrammarProfile::Names::NameSet GrammarProfile::Names::JavaExhaustiveCaseLabel = { "null_literal", "pattern" };

// 仮引数のスコープを開く Java のノード
const GrammarProfile::Names::NameSet GrammarProfile::Names::JavaParamScope =
{ "compact_constructor_declaration", "constructor_declaration", "lambda_expression", "method_declaration" };

// 文の位置に置いた Java の switch の親（tree-sitter-java は switch の文と式を同じ switch_expression で表す）
const GrammarProfile::Names::NameSet GrammarProfile::Names::JavaStmtHost = {
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
const GrammarProfile::Names::NameSet GrammarProfile::Names::JavaTypeBody =
{ "annotation_type_body", "class_body", "enum_body", "interface_body" };

// JS/TS の束縛の名前（分割代入の省略形を含む）
const GrammarProfile::Names::NameSet GrammarProfile::Names::JsBindingName =
{ "identifier", "shorthand_property_identifier_pattern" };

// JS/TS の `in` を for 初期化子外で二項演算子へ戻す式
const GrammarProfile::Names::NameSet GrammarProfile::Names::JsInRestoring = {
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

// `let` の後の改行を文の終わりと読んだパーサーが分けた次の文の最初の名前（束縛の名前で，TS の型注釈の付く名前はラベルと読む）
const GrammarProfile::Names::NameSet GrammarProfile::Names::JsLetBindingHead = { "identifier", "statement_identifier" };

// JS/TS のメンバ・添字の参照（代入しても受取側の変数は再束縛されない）
const GrammarProfile::Names::NameSet GrammarProfile::Names::JsMemberHost =
{ "call_expression", "member_expression", "subscript_expression" };

// JS/TS のメンバ・添字の参照 (member_expression / subscript_expression) — 代入で変数を再束縛しない
const GrammarProfile::Names::NameSet GrammarProfile::Names::JsMemberTarget = { "member_expression", "subscript_expression" };
// JS/TS のモジュールのマーカーの文（持たないファイルは古典スクリプトで，最上位の宣言を他のスクリプトと共有する）
const GrammarProfile::Names::NameSet GrammarProfile::Names::JsModuleStatement = { "export_statement", "import_statement" };

// 後に改行を挟めない JS/TS の語（改行を挟むと自動の `;` を補うか別の意味に読む，其の他の語の後の改行は読みを変えない）
const GrammarProfile::Names::NameSet GrammarProfile::Names::JsRestrictedKeyword =
{ "async", "break", "continue", "return", "throw", "yield" };

// JS/TS の statement_block + Java/Go/Rust 等の block（単独コメント `{ /* */ }` block 親判定）
const GrammarProfile::Names::NameSet GrammarProfile::Names::JsStatementBlockOrBlock = { "block", "statement_block" };

// 文・メンバを並べる JS/TS のノード（JSX の式の中から退避するコメントを其の子の文の前へ置き，改行が文・メンバを区切得る為に直下の改行を含むコメントの跡に改行を残す）
const GrammarProfile::Names::NameSet GrammarProfile::Names::JsStatementSequence =
{ "class_body", "interface_body", "object_type", "program", "statement_block", "switch_case", "switch_default" };

// JSX の内容境界の通常の要素とは別集合で判定
const GrammarProfile::Names::NameSet GrammarProfile::Names::JsxContainer = { "jsx_element" };

// JSX 要素
const GrammarProfile::Names::NameSet GrammarProfile::Names::JsxElement =
{ "element", "jsx_element", "jsx_self_closing_element" };

// JSX/HTML の開閉タグ全種（JsxContentCount で内容から除外する非内容子）
const GrammarProfile::Names::NameSet GrammarProfile::Names::JsxHtmlTagOpenClose =
{ "end_tag", "jsx_closing_element", "jsx_opening_element", "start_tag" };

// JSX タグ
// JS/TS <C>
// JS/TS <C/>
const GrammarProfile::Names::NameSet GrammarProfile::Names::JsxTag = { "jsx_opening_element", "jsx_self_closing_element" };
// JSX の本文（文字参照と本文は１つの子文字列として連なる）
// JSX &amp;
// JSX text
const GrammarProfile::Names::NameSet GrammarProfile::Names::JsxTextLike = { "html_character_reference", "jsx_text" };
// 語で始まる脱出の文（最初の子の語で break / continue / return / throw を見分ける）
const GrammarProfile::Names::NameSet GrammarProfile::Names::KeywordJump = { "control_transfer_statement", "jump_expression" };

// 語や記号で前置する単項の式（表の演算子を持たないか，別の演算子と同じテキストを持つ）
const GrammarProfile::Names::NameSet GrammarProfile::Names::KeywordUnary =
{ "cast_expression", "co_await_expression", "delete_expression", "error_suppression_expression" };

// Kotlin のアクセサ（直前のプロパティの一部で，パーサーは型の本体のメンバとして並べる）
// Kotlin val a get() = 1
// Kotlin var b = 0 private set
const GrammarProfile::Names::NameSet GrammarProfile::Names::KotlinAccessor = { "getter", "setter" };

// 代入先を包む Kotlin のノード（中の名前が代入先）
const GrammarProfile::Names::NameSet GrammarProfile::Names::KotlinAssignableWrapper =
// Kotlin x = 1 の x
// Kotlin (x) = 1
{ "directly_assignable_expression", "parenthesized_expression" };

// 本体の波括弧を包みのノードに入れず直接の子に持つ Kotlin の構文（本体の文だけを行毎に置き，見出と後続の節は同じ行に保つ）
const GrammarProfile::Names::NameSet GrammarProfile::Names::KotlinBraceBodyHost =
{ "anonymous_initializer", "secondary_constructor", "try_expression" };

// Kotlin の型の本体のメンバ（最後のメンバの後に同じ行で `}` が続くと，パーサーがメンバを `ERROR` に包む）
const GrammarProfile::Names::NameSet GrammarProfile::Names::KotlinClassMember = {
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

// Kotlin の関数の見出のトークン（後に続く型仮引数の `<` との間に空白を置く）
// Kotlin fun <T> f()
// Kotlin inline <T>
const GrammarProfile::Names::NameSet GrammarProfile::Names::KotlinFunctionHead = { "fun", "modifiers" };

// Kotlin で前後の行へ式を繋ぐ呼出と参照
const GrammarProfile::Names::NameSet GrammarProfile::Names::KotlinJointSuffix =
{ "call_suffix", "callable_reference", "infix_expression", "jump_expression", "navigation_suffix" };

// `!` を前置して否定する Kotlin の検査（`!is` / `!in` は１トークンで，空白を挟めない）
const GrammarProfile::Names::NameSet GrammarProfile::Names::KotlinNegatableTest =
{ "check_expression", "range_test", "type_test" };

// Kotlin `!is` `!in` 演算子の右側キーワード（否定する検査の中で `!` 直後の密着判定）
// Kotlin `!in` の `in`
// Kotlin `!is` の `is`
const GrammarProfile::Names::NameSet GrammarProfile::Names::KotlinNotIsIn = { "in", "is" };

// Kotlin の数値リテラル接尾子付種別（数値部と接尾子の密着必須，`100L` → `100 L` 化で構文解析失敗）
const GrammarProfile::Names::NameSet GrammarProfile::Names::KotlinNumericLiteralSuffix =
{ "long_literal", "real_literal", "unsigned_literal" };

// Kotlin 関数戻値型ノード（function_value_parameters 後の最初の名前付子で戻値型判定）
const GrammarProfile::Names::NameSet GrammarProfile::Names::KotlinReturnType =
{ "dynamic_type", "function_type", "nullable_type", "parenthesized_type", "user_type" };

// 改行が文を終える Kotlin の文脈（括弧の中と when の枝の条件では改行が意味を持たない）
const GrammarProfile::Names::NameSet GrammarProfile::Names::KotlinStmtCtx =
{ "class_body", "control_structure_body", "function_body", "source_file", "statements" };

// 型の被演算子を取る Kotlin の型（関数の型・省略可能の型を括弧無で置くと結合が変わる）
const GrammarProfile::Names::NameSet GrammarProfile::Names::KotlinTypeOperatorHost = { "nullable_type", "receiver_type" };
// ラベル付の文（goto のジャンプ先）
const GrammarProfile::Names::NameSet GrammarProfile::Names::LabeledStmt = { "labeled_statement", "named_label_statement" };
// 右辺を条件付で評価する論理演算子のトークン
const GrammarProfile::Names::NameSet GrammarProfile::Names::LazyLogicalOperator = { "&&", "||" };
// 先頭の型引数を後ろの子へ密着させるノード
const GrammarProfile::Names::NameSet GrammarProfile::Names::LeadingTypeArgs = { "method_invocation", "type_assertion" };

// 葉ノード（中身を再構築せず原文を其の儘使う）
const GrammarProfile::Names::NameSet GrammarProfile::Names::Leaf = {
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

// ラベルを跳先として持つ脱出の式（中に現れるラベルは定義でなく参照）
const GrammarProfile::Names::NameSet GrammarProfile::Names::JumpWithLabel =
{ "break_expression", "continue_expression", "jump_expression" };

// break, continue の境界に為る全言語の反復構文
const GrammarProfile::Names::NameSet GrammarProfile::Names::Loop = {
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

// 繰返を抜ける文（Rust は式）
const GrammarProfile::Names::NameSet GrammarProfile::Names::LoopExitStmt =
{ "break_expression", "break_statement", "continue_expression", "continue_statement" };

// `break_statement` の境界と為る反復・switch 系構文
const GrammarProfile::Names::NameSet GrammarProfile::Names::LoopOrSwitch = {
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

// 行末迄のトークンを１つに呑むマクロの本体
const GrammarProfile::Names::NameSet GrammarProfile::Names::MacroBody = { "preproc_arg" };
// マクロの定義（置換本体を持つプリプロセッサディレクティブ）
const GrammarProfile::Names::NameSet GrammarProfile::Names::MacroDef = { "preproc_def", "preproc_function_def" };

// マクロ呼出風ノード
const GrammarProfile::Names::NameSet GrammarProfile::Names::MacroLike =
{ "destructor_name", "macro_invocation", "marker_annotation", "operator_name", "try_operator" };

// メンバアクセスチェーン（ドット記法／スコープ解決記法）
const GrammarProfile::Names::NameSet GrammarProfile::Names::Member = {
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

// アクセス順序検査の順位２に当たる C# のプロパティ・イベントフィールド
const GrammarProfile::Names::NameSet GrammarProfile::Names::MemberFieldRank =
{ "const_declaration", "event_field_declaration", "property_declaration" };

// メンバ関数／メソッド／コンストラクタ（アクセス順序検査の順位 3）
const GrammarProfile::Names::NameSet GrammarProfile::Names::MemberMethodRank =
{ "constructor_declaration", "function_definition", "method_declaration", "method_definition" };

// 型エイリアス＋ネスト型宣言（アクセス順序検査の順位 1）
const GrammarProfile::Names::NameSet GrammarProfile::Names::MemberTypeRank = {
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
	"union_specifier",
	"using_declaration"
};

// 山括弧の誤解析で比較式が型引数リストへ化けたノード（C/C++ の `b < c && d >= e`，C# / TS の `a < b, c >> d`）
const GrammarProfile::Names::NameSet GrammarProfile::Names::MisparsedTemplateHost =
{ "generic_name", "instantiation_expression", "template_function", "template_type" };

// 変数を書き換える式
const GrammarProfile::Names::NameSet GrammarProfile::Names::MutatingExpr = { "assignment_expression", "update_expression" };

// 名前と初期値・型を包む宣言子（宣言の型を探す時に宣言へ遡る）
const GrammarProfile::Names::NameSet GrammarProfile::Names::NameDeclarator =
{ "init_declarator", "pattern", "variable_declarator" };

// 比較用の名前を投影する名前・ラベルのトークン
const GrammarProfile::Names::NameSet GrammarProfile::Names::NameToken = {
	"identifier",
	"label",
	"label_name",
	"name",
	"shorthand_property_identifier_pattern",
	"simple_identifier",
	"statement_identifier",
	"statement_label"
};

// 共有の宣言子走査で仮引数を区別するノード
const GrammarProfile::Names::NameSet GrammarProfile::Names::NamedParamKind =
// Java/JS
// Rust/Kotlin/Swift
// C/C++
// TS
{ "formal_parameter", "parameter", "parameter_declaration", "required_parameter" };

// 名前空間／内部モジュール（共有する本体を型のメンバ列と区別する）
const GrammarProfile::Names::NameSet GrammarProfile::Names::NamespaceLike =
{ "internal_module", "mod_item", "module", "namespace_declaration", "namespace_definition" };

// C# `new()` 形の括弧付呼出文脈（ジェネリック制約と目標型推論 new）
const GrammarProfile::Names::NameSet GrammarProfile::Names::NewParenCall =
{ "constructor_constraint", "implicit_object_creation_expression" };

// 定数式に為れない式（呼出・代入・増減・型検査・生成・参照・空値・型のリテラル等，未知の名前やノードは未証明として扱う）
const GrammarProfile::Names::NameSet GrammarProfile::Names::NonConstantExpr = {
	"anonymous_object_creation_expression",
	"array_access",
	"array_creation_expression",
	"assignment_expression",
	"await_expression",
	"class_literal",
	"conditional_access_expression",
	"element_access_expression",
	"instanceof_expression",
	"invocation_expression",
	"is_expression",
	"is_pattern_expression",
	"lambda_expression",
	"method_invocation",
	"method_reference",
	"null_literal",
	"object_creation_expression",
	"postfix_unary_expression",
	"switch_expression",
	"update_expression"
};

// `:` 周辺空白を `: ` 形に正規化するノード
const GrammarProfile::Names::NameSet GrammarProfile::Names::NormColon = {
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
	"field_pattern",
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
	"tuple_expression",
	"tuple_type_item",
	"type_annotation",
	"type_constraint",
	"type_parameter",
	"typed_default_parameter",
	"typed_parameter",
	"value_argument",
	"variable_declaration"
};

// 数値リテラル系ノード
const GrammarProfile::Names::NameSet GrammarProfile::Names::NumLiteral = {
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
const GrammarProfile::Names::NameSet GrammarProfile::Names::ObjectLike = {
	"dictionary",
	"dictionary_comprehension",
	"enum_body",
	"enum_member_declaration_list",
	"export_clause",
	"field_initializer_list",
	"hash",
	"initializer_expression",
	"initializer_list",
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

// 行末迄を本文が飲むノードとパーサーが読めなかった領域（行形式のコメントが再読込で本文へ紛れる）
const GrammarProfile::Names::NameSet GrammarProfile::Names::OpaqueLineText = { "ERROR", "preproc_arg" };

// 語で始まり右の被演算子を式の終わり迄取る式（括弧を外すと後続の演算子を被演算子へ取り込む）
const GrammarProfile::Names::NameSet GrammarProfile::Names::OpenPrefixExpr = {
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

// 宣言へ密着する記号及び Ruby で改行を式の継続と読ませる文末トークン
const GrammarProfile::Names::NameSet GrammarProfile::Names::OpenTailToken = { "&", "*", "**", "..", "...", ":" };
// 直前トークンへ密着する省略可能性記号（TS `?.` / `?` / Swift の省略可能型）
const GrammarProfile::Names::NameSet GrammarProfile::Names::OptionalMarker = { "optional_chain", "optional_type", "quest" };

// `?` を後ろで割ると宣言名と型注釈が別の構文へ化ける宣言ノード（`name?: T` の `?` は型注釈の一部で名前と不可分）
const GrammarProfile::Names::NameSet GrammarProfile::Names::OptionalTypeMember =
{ "abstract_method_signature", "method_signature", "optional_parameter", "property_signature", "public_field_definition" };

// 通常関数（名前を持つ関数・メソッドの定義，コンストラクタ，デストラクタ，アクセサ） — 末尾の返戻の明示の対象
const GrammarProfile::Names::NameSet GrammarProfile::Names::OrdinaryFunction = {
	"accessor_declaration",
	"computed_getter",
	"computed_setter",
	"constructor_declaration",
	"conversion_operator_declaration",
	"deinit_declaration",
	"destructor_declaration",
	"didset_clause",
	"function_declaration",
	"function_definition",
	"function_item",
	"getter",
	"init_declaration",
	"local_function_statement",
	"method_declaration",
	"method_definition",
	"operator_declaration",
	"secondary_constructor",
	"setter",
	"willset_clause"
};

// ドキュメント化引数列コンテナ（Ruby method_parameters を含むドキュメント化解析用，C/C++ は CFunctionOrFieldDeclarator 経由で別取得）
const GrammarProfile::Names::NameSet GrammarProfile::Names::ParamContainerLike =
{ "formal_parameters", "function_value_parameters", "method_parameters" }; // JS/TS

// Kotlin
// Ruby def f(x)
// 関数パラメータコンテナ（言語横断）
const GrammarProfile::Names::NameSet GrammarProfile::Names::ParamContainer =
{ "formal_parameters", "function_value_parameters", "parameter_list", "parameters" }; // JS/TS

// Kotlin
// C/C++/Rust
// Python
// 括弧除去で構文又は意味が変わる最左子孫
const GrammarProfile::Names::NameSet GrammarProfile::Names::ParenInnerFirstChildNoUnwrap =
{ "left_assignment_list", "named_expression" };

// 括弧を要する内側式は即時呼出等の構文と意味を保つ
const GrammarProfile::Names::NameSet GrammarProfile::Names::ParenInnerNoUnwrap = {
	"arrow_function",
	"comma_expression",
	"compound_statement", // GNU C/C++ の文式 `({ int y = 1; y; })` — 括弧を外すと初期化子の並びやブロックに為る
	"custom_operator", // Swift `(^^)` 演算子の参照 — 括弧を外すと演算子だけが残って構文が壊れる
	"element",
	"if_expression", // Kotlin/Rust `a + (if(c) 1 else 0) + b` — 括弧を外すと else 枝が右隣を飲込み値が変わる
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
	"rescue_modifier", // Ruby `(a rescue b)` 同上
	"sequence_expression",
	"unless_modifier", // Ruby `(a unless c)` 同上
	"until_modifier", // Ruby `(a until c)` 同上
	"when_expression", // Kotlin `a + (when { ... }) + b` — 同上
	"while_modifier", // Ruby `(a while c)` 同上
	"yield" // Python `[(yield)]` `f((yield))` 要素／引数位置の `yield`･`yield from` は括弧必須で剥がすと構文崩壊
};

// 見出と本体の構文上の区切
const GrammarProfile::Names::NameSet GrammarProfile::Names::StructuralParenBoundary = {
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
	"synchronized_statement", // Java synchronized(x) { } — 錠の式の括弧は構文の一部
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
	"with_statement" // JS with(x) — 非推奨
};

// 型・値・パターンの意味を定める括弧のグループ
const GrammarProfile::Names::NameSet GrammarProfile::Names::MeaningfulParenGroup = {
	"array_type",
	"cast_expression",
	"conditional_type",
	"decltype", // C++ decltype((x)) — 括弧の有無で型が変わる（`(x)` は左辺値の式で参照型 `int&`，`x` は宣言の型 `int`）
	"directly_assignable_expression",
	"jsx_attribute",
	"new_expression",
	"object_creation_expression", // PHP new (pick("B")) — 括弧の中は類名を求める式で，外すと `pick` 類の生成に為る
	"range_expression", // C# `arr[(a + b)..(c + d)]` — 括弧を外すと `..` の境界が解析不能に為る
	"sizeof_expression",
	"type_cast_expression"
};

// 受手を包む括弧が後置の結合を保つ式
const GrammarProfile::Names::NameSet GrammarProfile::Names::ReceiverParenHost = {
	"array_access",
	"call_expression",
	"class_constant_access_expression", // PHP `($a . $b)::C` — 類定数の参照は後置で受信側括弧を保持
	"conditional_access_expression", // C# `(a + b)?.M()` — 括弧を外すと `?.` が末尾の被演算子だけへ掛かる
	"element_access_expression",
	"element_binding_expression", // C# `(a + b)?[i]` — 同上
	"element_reference",
	"field_access", // Java (a - b).c — フィールド参照は後置で受信側の二項式を包む括弧を保持
	"field_expression",
	"function_call_expression",
	"index_expression",
	"indexing_expression",
	"instantiation_expression", // TS `(a ?? b)<T>` — 型引数の具体化は後置で，外すと末尾の被演算子だけへ掛かる
	"member_access_expression", // C# (a - b).M — メンバアクセスは括弧除去で受信側の二項式が再結合し意味変質
	"member_call_expression",
	"member_expression",
	"method_invocation", // Java (a - b).m() — メソッド呼出は後置で受信側括弧を保持
	"method_reference", // Java の `(c ? a : b)::m` — メソッド参照は後置で受信側括弧を保持
	"navigation_expression",
	"non_null_expression", // TS `(new a)!.c` — 非 null のアサーションは後置で受信側括弧を保持
	"nullsafe_member_access_expression", // PHP `($a - $b)?->y` — メンバアクセスは後置で受信側括弧を保持
	"nullsafe_member_call_expression", // PHP `($a - $b)?->f()` — 同上
	"qualified_identifier",
	"scoped_call_expression", // PHP `(A|B)::f()` — スコープ解決は後置で受信側括弧を保持
	"scoped_identifier",
	"scoped_property_access_expression", // PHP `(A|B)::$x` — 同上
	"selector_expression", // Go (a - b).M — セレクタは後置で受信側の二項式を包む括弧を保持
	"slice_expression", // Go (a + b)[1:2] — 切出は後置で受信側の二項式を包む括弧を保持
	"subscript",
	"subscript_expression",
	"type_assertion_expression" // Go (a + b).(T) — 型アサーションは後置で受信側の二項式を包む括弧を保持
};

// `(` で始まる型（修飾子・属性・生存期間等に続く `(` は呼出の括弧ではない）
const GrammarProfile::Names::NameSet GrammarProfile::Names::ParenLeadType =
{ "disjunctive_normal_form_type", "function_type", "parenthesized_type", "tuple_type", "unit_type" };

// 条件を括弧で包まない言語の制御構文
const GrammarProfile::Names::NameSet GrammarProfile::Names::ParenOptionalHeader = {
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
	"while_statement" // Swift/Python while (c) { } / while (c):
};

// 括弧が値の全体を包み，後に演算子や後置のトークンが続かない値の位置（代入の右辺・実引数・戻値・要素）
const GrammarProfile::Names::NameSet GrammarProfile::Names::ParenValSlot = {
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

// 変数を束縛するパターン
// Rust match 1 | _
// Rust (a, b)
const GrammarProfile::Names::NameSet GrammarProfile::Names::PatternKind = { "pattern", "tuple_pattern" };

// PHP の代替構文の終端語（本体の後で行頭に置く，足す時は CloserToken へも足す）
const GrammarProfile::Names::NameSet GrammarProfile::Names::PhpAltCloser =
{ "enddeclare", "endfor", "endforeach", "endif", "endswitch", "endwhile" };

// 構文を閉じる語（`PhpAltCloser` に Ruby, Lua 系の `end` を加えた物，見出の行に同居すると本体を置く行が残らない）
const GrammarProfile::Names::NameSet GrammarProfile::Names::CloserWord = { PhpAltCloser, { "end" } };

// PHP の代替構文の本体を開く `:` を直接の子に持つノード（`if` / `foreach` / `while` は `colon_block` に包む）
const GrammarProfile::Names::NameSet GrammarProfile::Names::PhpAltHost =
{ "colon_block", "declare_statement", "for_statement", "switch_block" };

// 本体を開く `:` を先頭のトークンに持つ PHP の代替構文の本体（`for` / `declare` は直下の `:` が本体を開く）
// PHP の `: stmts`
// PHP の `switch(...):`
const GrammarProfile::Names::NameSet GrammarProfile::Names::PhpAltLeadingColon = { "colon_block", "switch_block" };
// PHP の if の後続の枝の節（本体の後に並び，波括弧の無い本体の開いた if へ属し得る）
// PHP の名前空間インポートノード（`\` 区切とグループ括弧は名前の一部で，空白を挟むと名前解決が壊れる）
const GrammarProfile::Names::NameSet GrammarProfile::Names::PhpIfBranch = { "else_clause", "else_if_clause" };

const GrammarProfile::Names::NameSet GrammarProfile::Names::PhpNamespaceUse =
{ "namespace_use_declaration", "namespace_use_group" };

// PHP の開始タグで終わる子（地の文の後の開始タグ）
// PHP <?php
// PHP の `?> ... <?php`
const GrammarProfile::Names::NameSet GrammarProfile::Names::PhpOpenTagEnd = { "php_tag", "text_interpolation" };

// 式等で終わり常に `;` で閉じる PHP の文（閉タグが `;` の代わりに文を終え得，末尾の式が `}` で終わっても `;` を要する）
const GrammarProfile::Names::NameSet GrammarProfile::Names::PhpSemicolonStmt = {
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

// 型を合成する `|` / `&` を持つノード（合併・交差・複数の捕捉型・型の制約・Go の型集合）
const GrammarProfile::Names::NameSet GrammarProfile::Names::TypeCombination = {
	"catch_type",
	"disjunctive_normal_form_type",
	"intersection_type",
	"not_nullable_type",
	"protocol_composition_type",
	"type_bound",
	"type_elem",
	"type_list",
	"union_type"
};

// 内側で改行出来ない PHP のノード（括弧と語が１つのトークンに当たる）
const GrammarProfile::Names::NameSet GrammarProfile::Names::PhpUnsplittable = { "cast_expression", "visibility_modifier" };
// `|` で囲まれる仮引数列（Ruby ブロック／Rust クロージャ — `|` 両側の密着判定）
// Ruby |a, b|
// Rust |a, b|
const GrammarProfile::Names::NameSet GrammarProfile::Names::PipeParamList = { "block_parameters", "closure_parameters" };

// ポインタ宣言子
const GrammarProfile::Names::NameSet GrammarProfile::Names::PtrDecl = {
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
	"pointer_type_declarator", // C++ int S::*p（メンバへのポインタの宣言子をパーサーが限定名と読む）
	"reference_declarator",
	"reference_pattern",
	"reference_type",
	"rest_pattern",
	"rest_type",
	"self_parameter",
	"splat_argument",
	"splat_parameter",
	"splat_pattern", // Python case [1, *rest] / case {**rest}（マッチパターンのスター展開は密着）
	"spread_element",
	"spread_expression",
	"variadic_declarator",
	"variadic_parameter",
	"variadic_parameter_declaration" // Go xs ...int
};

// C/C++ ポインタ／参照宣言子（戻値型のポインタ／参照判定）
const GrammarProfile::Names::NameSet GrammarProfile::Names::PtrOrRefDeclarator =
{ "pointer_declarator", "reference_declarator" };

// 後置単項演算子及び前置・後置共通の更新式
const GrammarProfile::Names::NameSet GrammarProfile::Names::PostfixOrUpdateExpr = {
	"dec_statement",
	"inc_statement",
	"non_null_expression",
	"postfix_expression",
	"postfix_unary_expression",
	"update_expression" // JS/TS ++x / x++
};

// 被呼出側を最初の子に持つ呼出（被呼出側を包む括弧の保持の判定）
const GrammarProfile::Names::NameSet GrammarProfile::Names::PostfixCallHost = { "call_expression", "function_call_expression" };

// 受手（最初の子）を持つ後置の連鎖（受手を包む括弧の除去で，最初の子を辿って受手に置けるかを確かめる）
const GrammarProfile::Names::NameSet GrammarProfile::Names::PostfixReceiverChain = {
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

// 受手を最初の子に持つ後置の式（メンバ参照・呼出・添字，受手を包む括弧の除去の対象）
const GrammarProfile::Names::NameSet GrammarProfile::Names::PostfixReceiverHost = {
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
	"non_null_expression",
	"selector_expression",
	"subscript",
	"subscript_expression" // C/C++/JS/TS/PHP (a.b)[0]
};

// 括弧無で後置の式の受手に置ける名前・トークンで閉じるリテラル（後置の連鎖の最初の子の終点）
const GrammarProfile::Names::NameSet GrammarProfile::Names::PostfixReceiverLeaf = {
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
	"object", // JS/TS { a: 1 }（文頭はブロックと読まれる為に別に保持）
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
const GrammarProfile::Names::NameSet GrammarProfile::Names::Preproc = {
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

// 前処理条件の根（elif, else は除外）
const GrammarProfile::Names::NameSet GrammarProfile::Names::PreprocConditional = { "preproc_if", "preproc_ifdef" };
// プリプロセッサディレクティブの閉系（単独ディレクティブで常に空行区切）
// C/C++ #else
// C/C++ #endif
const GrammarProfile::Names::NameSet GrammarProfile::Names::PreprocDirectiveClose = { "#else", "#endif" };

// プリプロセッサディレクティブの開き／分岐系（後続の条件式と連結対象）
const GrammarProfile::Names::NameSet GrammarProfile::Names::PreprocDirectiveOpen =
{ "#elif", "#elifdef", "#elifndef", "#if", "#ifdef", "#ifndef" };

// 条件で名前がマクロかを問う前処理のノード (preproc_defined / preproc_elifdef / preproc_ifdef)
const GrammarProfile::Names::NameSet GrammarProfile::Names::PreprocNameQuery =
{ "preproc_defined", "preproc_elifdef", "preproc_ifdef" };

// Python の括弧内で改行を継続出来る式
const GrammarProfile::Names::NameSet GrammarProfile::Names::PythonContinuationExpr {
	"attribute",
	"await",
	"binary_operator",
	"boolean_operator",
	"call",
	"comparison_operator",
	"concatenated_string",
	"conditional_expression",
	"expression_list",
	"false",
	"float",
	"identifier",
	"integer",
	"lambda",
	"none",
	"not_operator",
	"pattern_list",
	"string",
	"subscript",
	"true",
	"tuple_expression",
	"type",
	"unary_operator"
};

// 本体の最初の文字列をドキュメンテーション文字列とする Python の定義 (class_definition / function_definition)
const GrammarProfile::Names::NameSet GrammarProfile::Names::PythonDocstringOwner =
{ "class_definition", "function_definition" };

// Python の数値リテラル (float / integer) — `.` の前の括弧が必須
// Python (1.5).real
// Python (1).bit_length
const GrammarProfile::Names::NameSet GrammarProfile::Names::PythonNum = { "float", "integer" };
// 中身の無い事を表す Python の文・式 (pass_statement / ellipsis)
const GrammarProfile::Names::NameSet GrammarProfile::Names::PythonPlaceholder = { "ellipsis", "pass_statement" };
// Python の後置の式 (attribute / call) — 最高位の優先順位
// Python (a + b).c
// Python (a + b)(c)
const GrammarProfile::Names::NameSet GrammarProfile::Names::PythonPostfix = { "attribute", "call" };
// 受手を修飾する Java / C# のフィールドの参照 (field_access / member_access_expression)
const GrammarProfile::Names::NameSet GrammarProfile::Names::QualifiedMember = { "field_access", "member_access_expression" };
// 範囲式・範囲パターン（Rust `0..n` — `..` 両側の密着判定）
const GrammarProfile::Names::NameSet GrammarProfile::Names::RangeLike = { "range", "range_expression", "range_pattern" };
// 範囲の演算子（終端の無い範囲の判定）
// Ruby/Kotlin/Rust `1..`
// Ruby/Swift `1...`
const GrammarProfile::Names::NameSet GrammarProfile::Names::RangeOperator = { "..", "..." };

// Kotlin の増減式及び Swift の範囲式／単項演算子親
const GrammarProfile::Names::NameSet GrammarProfile::Names::RangeOrUnaryExpr =
{ "open_end_range_expression", "open_start_range_expression", "postfix_expression", "prefix_expression", "range_expression" };

// 値を返さない構築・破棄・設定・監視の操作
const GrammarProfile::Names::NameSet GrammarProfile::Names::ResultlessRoutine = {
	"compact_constructor_declaration",
	"computed_setter",
	"constructor_declaration",
	"deinit_declaration",
	"destructor_declaration",
	"didset_clause",
	"init_declaration",
	"secondary_constructor",
	"setter",
	"willset_clause"
};

// 次の文へ進まずに関数を終える文・式（返戻・送出・処理の終了） — `ControlFlow::TerminationRuleOf`
const GrammarProfile::Names::NameSet GrammarProfile::Names::ReturnOrThrow = {
	"co_return_statement",
	"exit_statement",
	"raise_statement",
	"return",
	"return_expression",
	"return_statement",
	"throw_expression",
	"throw_statement"
};

// 値を返す文（値の全体を包む括弧は，マクロの展開後も値を変えない）
const GrammarProfile::Names::NameSet GrammarProfile::Names::ReturnStmt = { "co_return_statement", "return_statement" };

// 戻値が組型のノード
const GrammarProfile::Names::NameSet GrammarProfile::Names::ReturnTuple =
{ "func_literal", "function_declaration", "function_type", "method_declaration", "method_elem" };

// return の返す値の参照の有無を決める関数（C++ の decltype(auto) を返す関数・PHP の参照を返す関数）
const GrammarProfile::Names::NameSet GrammarProfile::Names::ReturnValHost =
{ "anonymous_function", "arrow_function", "function_definition", "lambda_expression", "method_declaration" };

// 返す値を直接の子に持つノード（PHP の参照を返す関数の値を包む括弧の保持）
const GrammarProfile::Names::NameSet GrammarProfile::Names::ReturnValParent = { "arrow_function", "return_statement" };

// 名前を略して転送出来る Ruby の実引数（`&` / `*` / `**` だけの形）
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyAnonymousForward =
{ "block_argument", "hash_splat_argument", "splat_argument" };

// 命令形の呼出の実引数が左端から続く Ruby の式（実引数は式の終わり迄続く）
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyArgExtent =
{ "binary", "call", "conditional", "element_reference", "range" };

// 空白に依らず後続を実引数と読む Ruby の語（`return + x` も `return(+x)`）
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyArgKeyword = { "break", "next", "return" };
// 実引数の並びを子に持つ Ruby のノード（並びの括弧の読みを揃える）
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyArgListHost = { "break", "call", "next", "return", "yield" };
// Ruby の要素の並び（名前の後の実引数が並びの残りの要素を取り込む）
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyArgSeq = { "argument_list", "right_assignment_list" };
// 名前の後に空白を置き，直後に空白を置かずに続けると実引数の始まりに為る Ruby のトークン
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyArgStart = { "&", "*", "**", "+", "-", "<<", "[" };
// Ruby `%w[...]` / `%i[...]` 配列リテラル親（内部の `%w(` `%i(` `)` は密着必須）
// Ruby %w[a b c]
// Ruby の `%i[:a :b]`
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyArrayLiteral = { "string_array", "symbol_array" };

// Ruby の名前束縛式は修飾子化で解析順が変わる為に保つ
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyBinding =
{ "assignment", "match_pattern", "operator_assignment", "regex", "test_pattern" };

// Ruby 制御ブロック（end 区切）
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyBlock =
{ "begin", "block", "case", "case_match", "class", "do_block", "for", "if", "lambda", "module", "unless", "until", "while" };

// １文なら１行に置く Ruby の波括弧ブロックと本体
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyBraceBlock = { "block", "block_body" };
// Ruby class 定義系（`< 親` 継承記法のスペース調整対象）
// Ruby class C < Base
// Ruby class << obj
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyClassDefLike = { "class", "singleton_class" };

// Ruby で括弧除去に依り実引数の読みが変わるノード
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyCommandArgHost =
{ "binary", "break", "call", "element_reference", "next", "range", "return", "yield" };

// `cond` のフィールドを持ち本体が空なら ` then` で節の境を示す Ruby のノード
// Ruby elsif c
// Ruby if c
// Ruby unless c
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyCondHost = { "elsif", "if", "unless" };
// Ruby の区切文字付リテラル（開きが `%s{` 等の複数トークンで閉じが１トークンの為，括弧の深さ計算で内部へ降りない）
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyDelimitedLiteral = { "delimited_symbol", "regex", "subshell" };

// Ruby の脱出式（値を持たず，範囲式の始端に置くと `return` の前置で意味が壊れる）
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyFlowExpr =
{ "break", "next", "redo", "retry", "return", "yield" };

// 外のローカル変数を見ない新しいスコープを開く Ruby のノード（修飾子形式へ畳む時のローカル変数の有無の判定で遡りを止める）
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyFreshScope =
{ "class", "method", "module", "program", "singleton_class", "singleton_method" };

// ローカル変数のスコープを開く Ruby のノード（ブロックは外のローカル変数も見るが，内側で導入したローカル変数は外から見えない）
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyLocalScope =
{ "block", "class", "do_block", "lambda", "method", "module", "program", "singleton_class", "singleton_method" };

// 命令形の呼出の実引数の外に在る Ruby の語の二項演算子（文の単位で結ぶ）
// Ruby foo +1 and x
// Ruby foo +1 or x
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyLowBinary = { "and", "or" };
// 代入より弱い Ruby の単項の語（括弧を外すと後続の演算子・三項を被演算子へ取り込む）
// Ruby `defined? x`
// Ruby not x
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyLowUnary = { "defined?", "not" };
// Ruby のメソッドの定義
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyMethodDef = { "method", "singleton_method" };
// Ruby の rational/complex リテラル親種別（数値部と接尾子 `r`/`i`/`ri` の密着必須，`3r` → `3 r` 化で SyntaxError）
// Ruby 3i / 1+2i
// Ruby 3r / 1.5r
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyNumericLiteralSuffix = { "complex", "rational" };

// Ruby の仮引数の並び（仮引数の名前はローカル変数を導入する）
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyParams =
{ "block_parameters", "lambda_parameters", "method_parameters" };

// Ruby のパターン照合の式（式の中で最も弱く，括弧を外すと代入等の左辺を取り込む）
// Ruby h => {a:}
// Ruby h in {a:}
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyPatternTest = { "match_pattern", "test_pattern" };

// Ruby の文を直接置くコンテナ（括弧内でも not の被演算子の不要なラッパーを外す）
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyStmtHost =
{ "block_body", "body_statement", "do", "else", "elsif", "ensure", "parenthesized_statements", "program", "then" };

// Ruby の文の並び（区切改行は子間に在る為，平坦化では実引数への誤読を防ぐ `;` で接続，条件と本体を持つ elsif は除外）
const GrammarProfile::Names::NameSet GrammarProfile::Names::RubyStmtSeq = {
	"begin_block",
	"block_body",
	"body_statement",
	"do",
	"else",
	"end_block",
	"ensure",
	"interpolation",
	"parenthesized_statements",
	"then"
};

// 代入先を書き換える式（可変性の検査対象）
const GrammarProfile::Names::NameSet GrammarProfile::Names::WritableAssignment =
{ "assignment_expression", "compound_assignment_expr" };

// Rust mut 削除候補の判定で式木から変更対象に為り得る基底識別子を辿る経路
const GrammarProfile::Names::NameSet GrammarProfile::Names::RustBaseTraversal = {
	"field_expression",
	"generic_function",
	"index_expression",
	"parenthesized_expression",
	"try_expression",
	"unary_expression"
}; // Rust x.y

// Rust f::<T>
// Rust x[i]
// Rust (x)
// Rust x?
// Rust !x / -x
// 文頭に置くと `}` で文を終える Rust のブロックの式（block / unsafe_block / if_expression / loop_expression 等）
const GrammarProfile::Names::NameSet GrammarProfile::Names::RustBlockLike = {
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

// 条件と本体が独立した一時値の範囲に為る Rust の繰返 (loop_expression / while_expression)
const GrammarProfile::Names::NameSet GrammarProfile::Names::RustBodyOnlyLoop = { "loop_expression", "while_expression" };
// 被演算子を参照で受ける Rust の比較演算子のトークン
const GrammarProfile::Names::NameSet GrammarProfile::Names::RustBorrowingOperator = { "!=", "<", "<=", "==", ">", ">=" };

// Rust のブロックの中の宣言の文（値を与えない）
const GrammarProfile::Names::NameSet GrammarProfile::Names::RustDeclStmt = {
	"associated_type",
	"attribute_item",
	"const_item",
	"empty_statement",
	"enum_item",
	"extern_crate_declaration",
	"foreign_mod_item",
	"function_item",
	"function_signature_item",
	"impl_item",
	"inner_attribute_item",
	"let_declaration",
	"macro_definition",
	"mod_item",
	"static_item",
	"struct_item",
	"trait_item",
	"type_item",
	"union_item",
	"use_declaration"
};

// Rust の let の束縛（パターンが名前を束縛する）
const GrammarProfile::Names::NameSet GrammarProfile::Names::RustLetBinding = { "let_condition", "let_declaration" };
// 照合の対象を持つ Rust の条件 (let_chain / let_condition)
const GrammarProfile::Names::NameSet GrammarProfile::Names::RustLetCond = { "let_chain", "let_condition" };

// 場所を要する文脈に置いても破棄を伴う一時値を作らない Rust の式（場所の式とトークン）
const GrammarProfile::Names::NameSet GrammarProfile::Names::RustPlaceOrPlainVal = {
	"boolean_literal",
	"char_literal",
	"field_expression",
	"float_literal",
	"index_expression",
	"integer_literal",
	"metavariable",
	"raw_string_literal",
	"reference_expression",
	"string_literal",
	"unit_expression"
};

const GrammarProfile::Names::NameSet GrammarProfile::Names::RustDestructurePattern =
{ "captured_pattern", "slice_pattern", "struct_pattern", "tuple_struct_pattern" };

const GrammarProfile::Names::NameSet GrammarProfile::Names::RustParamNameWrapper =
{ "mut_pattern", "ref_pattern", "reference_pattern" };

const GrammarProfile::Names::NameSet GrammarProfile::Names::RustParamPattern =
{ RustDestructurePattern, RustParamNameWrapper, { "_", "identifier", "parameter", "self_parameter", "tuple_pattern" } }; // 型を明示出来る仮引数のパターン

// 最初の子を場所として参照する Rust の式 (field_expression / index_expression)
const GrammarProfile::Names::NameSet GrammarProfile::Names::RustPlaceProjection = { "field_expression", "index_expression" };
// Rust の参照・フィールドのパターン（`ref mut` を探す）
const GrammarProfile::Names::NameSet GrammarProfile::Names::RustRefPattern = { "field_pattern", "ref_pattern" };
// 照合の対象を `val` 場に持つ Rust の式 (let_condition / match_expression)
const GrammarProfile::Names::NameSet GrammarProfile::Names::RustScrutineeHost = { "let_condition", "match_expression" };
// 文頭の式を持つ Rust のノード（式文と，末尾の式を持つブロック）
// Rust { a; b } の b
// Rust a;
const GrammarProfile::Names::NameSet GrammarProfile::Names::RustStmtHead = { "block", "expression_statement" };

// 型の被演算子を取る Rust の型（`+` で境界を並べた型を括弧無で置くと境界が外へ掛かる）
const GrammarProfile::Names::NameSet GrammarProfile::Names::RustTypeOperatorHost =
{ "abstract_type", "bounded_type", "dynamic_type", "pointer_type", "reference_type" };

// 値を取り得る Rust の脱出の式（値を省くと後続の式を値として取り込む）
const GrammarProfile::Names::NameSet GrammarProfile::Names::RustValJump =
{ "break_expression", "return_expression", "yield_expression" };

// 言語固有の範囲本体
const GrammarProfile::Names::NameSet GrammarProfile::Names::ScopeBody =
{ "block", "block_body", "rule_set", "statements", "switch_body" };

// 関数／メソッド／プログラム本体スコープのコンテナ
const GrammarProfile::Names::NameSet GrammarProfile::Names::ScopeContainer = {
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

// ミックスインの定義とインクルードの SCSS の規則（空の仮引数・実引数の括弧を省ける）
const GrammarProfile::Names::NameSet GrammarProfile::Names::ScssMixinRule = { "include_statement", "mixin_statement" };

// 別名・公開する名前・設定の節と，設定の括弧を開く語
const GrammarProfile::Names::NameSet GrammarProfile::Names::ScssModuleClause =
{ "module_alias", "module_configuration", "module_members", "with" };

// 別名・公開する名前・設定の節を伴う SCSS のモジュールの規則
const GrammarProfile::Names::NameSet GrammarProfile::Names::ScssModuleRule = { "forward_statement", "use_statement" };
// 修飾名の内部の記号を分割しない名前のノード
const GrammarProfile::Names::NameSet GrammarProfile::Names::ScssNameAtom = { "module_name", "qualified_mixin_name" };
// 仮引数の並びを持つ SCSS の規則（パーサーは空の並び `()` に空の仮引数を補って `MISSING` を立てる）
const GrammarProfile::Names::NameSet GrammarProfile::Names::ScssParamHost = { "function_statement", "mixin_statement" };

// 手前のブロックに続く節を開く語（節をノードで持たない Go の裸のブロック・Kotlin の本体のコンテナを，閉じの後の語で見分ける）
const GrammarProfile::Names::NameSet GrammarProfile::Names::SectionKeyword =
{ "catch", "else", "elseif", "ensure", "finally", "rescue" };

// 区切のトークン（中置演算子として空白で囲まない）
const GrammarProfile::Names::NameSet GrammarProfile::Names::SeparatorToken = { ",", ":", ";" }; // 共通 a, b

// 共通 a: b
// 共通 a; b
// 逐次実行される文の直接コンテナ（`FunctionBodyContainer` ∪ `stmts` の統合済集合）
const GrammarProfile::Names::NameSet GrammarProfile::Names::SequentialStmtContainer =
{ FunctionBodyContainer, { "statements" } };

// 逐次文のコンテナ（`SequentialStmtContainer` ∪ `control_structure_body` ∪ Go の `statement_list`）— `ControlFlow::TerminationRuleOf`
const GrammarProfile::Names::NameSet GrammarProfile::Names::StmtFlowContainer =
{ SequentialStmtContainer, { "control_structure_body", "statement_list" } };

// 本体を１度だけ実行する文（言語横断） — 本体の終端で文の終端を判ずる
const GrammarProfile::Names::NameSet GrammarProfile::Names::SingleBodyStmt = {
	"checked_statement",
	"fixed_statement",
	"lock_statement",
	"synchronized_statement",
	"unsafe_block",
	"unsafe_statement",
	"using_statement",
	"with_statement"
};

// スペース正規化を行わないノード（リテラル／コメント／特殊構文等）
const GrammarProfile::Names::NameSet GrammarProfile::Names::SpaceSkip = {
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
	"optional_parameter",
	"pair",
	"property_signature",
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
	"trait_bounds",
	"type_annotation",
	"typed_default_parameter",
	"typed_parameter",
	"user_defined_literal" // C++ "hello"s
};

// スペース付与キーワード
const GrammarProfile::Names::NameSet GrammarProfile::Names::SpaceKeyword = {
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
const GrammarProfile::Names::NameSet GrammarProfile::Names::SpacelessBrace = { "jsx_expression" }; // JS/TS {expr}

// 文リストノード（Go / Kotlin 等の括弧付宣言グループや Ruby のメソッド本体）
const GrammarProfile::Names::NameSet GrammarProfile::Names::StmtList = {
	"accessor_list",
	"body_statement",
	"colon_block",
	"const_declaration",
	"enum_body_declarations",
	"import_list", // Kotlin のインポートの並び
	"statement_list",
	"statements",
	"type_declaration",
	"var_declaration",
	"var_spec_list" // Go
};

// 文列の前でだけ改行し，見出の子（パターン・仮引数）を同じ行に保つ節
const GrammarProfile::Names::NameSet GrammarProfile::Names::StmtsOnlyBreak = { "catch_block", "switch_entry" };
// 式値を使わない純粋な文文脈の親
const GrammarProfile::Names::NameSet GrammarProfile::Names::StmtCtxParent = { "expression_statement", "statements" };
// 文字列を開閉する引用符のトークン（`ERROR` の直接の子に在れば，文字列が閉じずに後続を飲み込んだ事を示す）
const GrammarProfile::Names::NameSet GrammarProfile::Names::StrDelimiter = { "\"", "\"\"\"", "'", "'''", "`" };

// 文字列の中の埋込式（JS / TS の `${…}`・Ruby の `#{…}`・Python の f 文字列と C# の `{…}`・Kotlin の `${…}` と `$x`・Swift の `\(…)`）
const GrammarProfile::Names::NameSet GrammarProfile::Names::StrInterpolation =
{ "interpolated_expression", "interpolated_identifier", "interpolation", "raw_str_interpolation", "template_substitution" };

// Lint 等の言語横断判定に使う，実効的な文字列値の全種
const GrammarProfile::Names::NameSet GrammarProfile::Names::StrLikeAll = {
	"encapsed_string", // PHP の補間文字列
	"heredoc", // PHP のヒアドキュメント
	"interpolated_string_expression",
	"line_string_literal",
	"multi_line_string_literal", // Swift の複数行の文字列
	"raw_string_literal",
	"string",
	"string_literal",
	"string_value",
	"template_string"
}; // C# $"..."

// Kotlin "..."
// Rust r"..."
// Ruby/Python/JS/TS
// C/C++/Java
// CSS
// JS/TS `...`
// 行末空白除去時に内部バイト範囲を保護する文字列／ヒアドキュメント／コメント系ノード
const GrammarProfile::Names::NameSet GrammarProfile::Names::StrLikeInnerPreserve = {
	"block_comment",
	"comment",
	"heredoc_body",
	"interpolated_string_expression",
	"line_comment",
	"multi_line_string_literal",
	"multiline_comment",
	"raw_string_content",
	"string",
	"string_array", // Ruby %w[a b]（内部の改行が要素区切）
	"string_content",
	"symbol_array", // Ruby %i[a b]（内部の改行が要素区切）
	"template_string",
	"verbatim_string_literal" // C# @"..."
};

// 文字列・シンボルを並べるノード
const GrammarProfile::Names::NameSet GrammarProfile::Names::StrSeqContainer =
{ "concatenated_string", "string_array", "symbol_array" };

// 本体の波括弧を直接の子に持つ Swift の制御構文（本体の文を展開する）
const GrammarProfile::Names::NameSet GrammarProfile::Names::SwiftBraceBodyHost =
{ "do_statement", "for_statement", "guard_statement", "if_statement", "repeat_while_statement", "while_statement" };

// 見出の式を直接の子に持つ Swift の文・節（見出の式の全体を包む括弧は，本体と紛れるクロージャを持てば必須）
const GrammarProfile::Names::NameSet GrammarProfile::Names::SwiftHeaderExprHost =
{ "for_statement", "guard_statement", "if_statement", "switch_statement", "where_clause", "while_statement" };

// Swift の見出の式の前のトークン（条件・束縛の値・反復の対象・switch の対象の直前）
const GrammarProfile::Names::NameSet GrammarProfile::Names::SwiftHeaderLead =
{ ",", "=", "guard", "if", "in", "switch", "while" };

// 見出の後に本体の波括弧が続く Swift の文（見出に括弧無で置いたクロージャが本体と紛れる）
const GrammarProfile::Names::NameSet GrammarProfile::Names::SwiftHeaderStmt =
{ "for_statement", "guard_statement", "if_statement", "switch_statement", "while_statement" };

// 名前付のノードに為る Swift の語（後の括弧を呼出の実引数と紛れない様に空白で区切る）
const GrammarProfile::Names::NameSet GrammarProfile::Names::SwiftKeywordNode =
{ "binding_pattern_kind", "value_binding_pattern", "where_keyword" };

// 中身をコードと読まない Swift の文字列・正規表現のリテラル（補間の中の式を除く）
const GrammarProfile::Names::NameSet GrammarProfile::Names::SwiftLiteralText =
{ "line_string_literal", "multi_line_string_literal", "raw_string_literal", "regex_literal" };

// Swift の呼出・添字の後置式（任意連鎖の `?` を密着させる）
const GrammarProfile::Names::NameSet GrammarProfile::Names::SwiftPostfixCall = { "call_expression", "subscript_expression" };
// Swift の見出直後で本体と誤読された後置クロージャ
const GrammarProfile::Names::NameSet GrammarProfile::Names::SwiftStrayBlock = { "ERROR", "lambda_literal" };

// 型の被演算子を取る Swift の型（演算子を持つ型を括弧無で置くと結合が変わる）
const GrammarProfile::Names::NameSet GrammarProfile::Names::SwiftTypeOperatorHost =
{ "existential_type", "metatype", "opaque_type", "optional_type", "protocol_composition_type" };

// switch の本体（言語横断）
const GrammarProfile::Names::NameSet GrammarProfile::Names::SwitchBody =
{ "compound_statement", "switch_block", "switch_body" };

// C# の `goto case` / `goto default` の跳先（`switch` の転送で，素通りを禁じる言語仕様が要求する）
// C# goto case 1;
// C# goto default;
const GrammarProfile::Names::NameSet GrammarProfile::Names::SwitchGotoTarget = { "case", "default" };

// switch に類する文（全節の終端判定の対象）
const GrammarProfile::Names::NameSet GrammarProfile::Names::SwitchStmtLike = {
	"expression_switch_statement",
	"match_expression",
	"match_statement",
	"select_statement",
	"switch_statement",
	"type_switch_statement",
	"when_expression"
};

// switch 式及び C# の with 式
const GrammarProfile::Names::NameSet GrammarProfile::Names::SwitchOrWithExpr = { "switch_expression", "with_expression" };

// 三項演算子系＋範囲 for 系（`?` `:` 周辺で特殊スペース処理対象として一括判定する為，統合）
const GrammarProfile::Names::NameSet GrammarProfile::Names::ColonPairHost = {
	"base_class_clause",
	"base_list",
	"conditional",
	"conditional_expression",
	"conditional_type",
	"enhanced_for_statement",
	"field_initializer_list",
	"for_range_loop",
	"ternary_expression" // Java/JS/TS の `x ? a : b`
};

// `then` / `do` 等の制御本体開始キーワード
const GrammarProfile::Names::NameSet GrammarProfile::Names::ThenDo = { "do", "then" }; // Ruby do
// Ruby then
// 自身の参照 (this / this_expression)
const GrammarProfile::Names::NameSet GrammarProfile::Names::ThisRef = { "this", "this_expression" };

// 透過ノード（深さを増やさず子を其の儘展開）
const GrammarProfile::Names::NameSet GrammarProfile::Names::Transparent = {
	"block_body",
	"body_statement",
	"chained_string", // Ruby 'a' "b" 隣接文字列リテラル連結（Ｎ個の子 text を親インデントで揃える）
	"compilation_unit",
	"program",
	"source_file",
	"statement_list",
	"statements",
	"translation_unit" // C/C++ (file)
};

// `SourceProjector::Project` のアンカーを解包する透過ラッパー
// Kotlin と Swift の文リストを含む（`constructor_body` の漏れを防ぐ為，列挙でなく和集合で作る）
const GrammarProfile::Names::NameSet GrammarProfile::Names::TransparentWrap =
{ FunctionBodyContainer, { "control_structure_body", "statements" } };

// 例外処理の文・式（言語横断，Swift の do は別に判ずる）
const GrammarProfile::Names::NameSet GrammarProfile::Names::TryStmt =
{ "try_expression", "try_statement", "try_with_resources_statement" };

// 戻値型の中で条件型を再び許す TS の関数型
// TS new () => T
// TS () => T
const GrammarProfile::Names::NameSet GrammarProfile::Names::TsFunctionType = { "constructor_type", "function_type" };

// 右端を貪欲に伸ばす TS の型（条件の型の比べる型・アロー関数の戻値の型に括弧無で置くと後の `?` `=>` を取り込む）
const GrammarProfile::Names::NameSet GrammarProfile::Names::TsGreedyType =
{ "conditional_type", "constructor_type", "function_type", "infer_type" };

// １要素組 `(x,)` で末尾カンマ必須に為る組系（組リテラルと括弧付式の構文木区別）
const GrammarProfile::Names::NameSet GrammarProfile::Names::TupleLike =
{ "tuple", "tuple_expression", "tuple_pattern", "tuple_struct_pattern", "tuple_type" };

// 要素の `<` の比較が後の要素の `>` と対に為り，型引数の並びと読まれ得る一覧の要素の位置 (TS / C# / C++)
const GrammarProfile::Names::NameSet GrammarProfile::Names::TypeArgLookalikeSlot =
{ "argument", "argument_list", "arguments", "array", "initializer_expression", "initializer_list" };

// 型を右に取る変換の式（後ろの `<` 等を型の続きと読む）
const GrammarProfile::Names::NameSet GrammarProfile::Names::TypeCastRight =
{ "as_expression", "satisfies_expression", "type_cast_expression" };

// `:` の右を値・型とするキー値対・型のフィールド（優先順位の右端探索を止める）
const GrammarProfile::Names::NameSet GrammarProfile::Names::TypeColonParent = { "pair", "pair_pattern", "property_signature" };

/**
 * 無名の空文・空宣言を直接並べる文法毎のコンテナの取得関数
 * 計算量：文法の識別子による取得は O(1)
 * @param Lang 対象の文法
 * @return 宣言の終端と別に独立したセミコロンを置けるコンテナの集合
 * @throws std::bad_alloc 初回の集合のメモリを確保出来ない場合
 * @throws std::length_error 集合の最大長を超える場合
 */
const GrammarProfile::Names::NameSet &GrammarProfile::Names::GetEmptyStmtContainers(const ::Lang Lang) {
	// 言語識別子の順に，各文法が無名の空文を直接持つ場所を登録する
	static const NameSet Containers[] {
		{}, // C
		{ "field_declaration_list" }, // C++
		{}, // C#
		{
			"annotation_type_body",
			"block",
			"class_body",
			"constructor_body",
			"enum_body_declarations",
			"interface_body",
			"labeled_statement",
			"program",
			"switch_block_statement_group"
		}, // Java
		{}, // Go
		{}, // Rust
		{}, // Kotlin
		{}, // Swift
		{}, // PHP
		{ "class_body" }, // JavaScript
		{ "class_body" }, // TypeScript と TSX
		{}, // Ruby
		{}, // Python
		{}, // JSON
		{}, // HTML
		{}, // CSS と SCSS
		{} // 未確定の文法
	};
	// 文法毎の集合の返戻
	return Containers[Lang.Id];
}

/**
 * 文法毎の要素列の本体の登録集合の取得関数
 * 計算量：文法の識別子による取得は O(1)
 * @param Lang 対象の文法
 * @return カンマ区切のメンバ又は実装を持たないアクセサを並べる本体の集合
 * @throws std::bad_alloc 初回の登録集合のメモリを確保出来ない場合
 * @throws std::length_error 登録集合の最大長を超える場合
 */
const GrammarProfile::Names::NameSet &GrammarProfile::Names::GetElementBodies(const ::Lang Lang) {
	// 言語識別子の順に各文法の集合を登録し，同じノード名でも宣言列と要素列を区別する
	static const NameSet Bodies[] {
		{}, // C
		{}, // C++
		{ "accessor_list" }, // C#
		{}, // Java
		{}, // Go
		{ "field_declaration_list", "ordered_field_declaration_list" }, // Rust
		{}, // Kotlin
		{ "protocol_property_requirements" }, // Swift
		{}, // PHP
		{}, // JavaScript
		{ "interface_body", "object_type" }, // TypeScript と TSX
		{}, // Ruby
		{}, // Python
		{}, // JSON
		{}, // HTML
		{}, // CSS と SCSS
		{} // 未確定の文法
	};
	// 対象の文法の集合の返戻
	return Bodies[Lang.Id];
}

// 型のメンバを並べる本体（宣言列と要素列の何方も含む）
const GrammarProfile::Names::NameSet GrammarProfile::Names::TypeMemberBody = {
	"accessor_list",
	"annotation_type_body",
	"class_body",
	"declaration_list",
	"field_declaration_list",
	"interface_body",
	"interface_type",
	"object_type",
	"ordered_field_declaration_list",
	"protocol_body",
	"protocol_property_requirements"
};

// 宣言の型の名前（比較境界の整数性・値域の確認に使う）
const GrammarProfile::Names::NameSet GrammarProfile::Names::TypeNameNode = {
	"floating_point_type",
	"integral_type",
	"predefined_type",
	"primitive_type",
	"sized_type_specifier",
	"type_identifier",
	"user_type"
};

// 名前に型を付ける宣言・仮引数
const GrammarProfile::Names::NameSet GrammarProfile::Names::TypedDecl = {
	"declaration",
	"formal_parameter",
	"let_declaration",
	"local_variable_declaration",
	"parameter",
	"parameter_declaration",
	"property_declaration",
	"var_spec",
	"variable_declaration"
};

const GrammarProfile::Names::NameSet GrammarProfile::Names::TypePrefixKeyword = { "readonly" }; // 内側の前置型の語
const GrammarProfile::Names::NameSet GrammarProfile::Names::TypePrefixOperand = { "array_type", "tuple_type" }; // 前置型の被演算子
const GrammarProfile::Names::NameSet GrammarProfile::Names::TypePrefixOperator = { "keyof" }; // 外側の前置型の語

const GrammarProfile::Names::NameSet GrammarProfile::Names::TypePrefixWeak =
{ "conditional_type", "intersection_type", "union_type" }; // 前置語より結合の弱い型

const GrammarProfile::Names::NameSet GrammarProfile::Names::TypePrefixWrapper = { "readonly_type" }; // 内側の前置型を包む構文

// 演算子を持つ型（型の演算子の被演算子に括弧無で置くと結合が変わる）
const GrammarProfile::Names::NameSet GrammarProfile::Names::TypeOperatorInner = {
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

// 前置単項演算子
const GrammarProfile::Names::NameSet GrammarProfile::Names::UnaryPre = {
	"pointer_expression",
	"prefix_expression",
	"prefix_unary_expression",
	"reference_expression",
	"unary",
	"unary_expression",
	"unary_op_expression",
	"unary_operator" // Python -x
};

// 被演算子との隣接空白を判定する前置式と更新式
const GrammarProfile::Names::NameSet GrammarProfile::Names::UnaryPreOrUpdate = {
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
const GrammarProfile::Names::NameSet GrammarProfile::Names::UnaryPrefixExpr = {
	"not_operator",
	"prefix_expression",
	"prefix_unary_expression",
	"unary",
	"unary_expression",
	"unary_op_expression",
	"unary_operator"
};

// 前置単項の記号（キャスト風の括弧の後で密着させる）
const GrammarProfile::Names::NameSet GrammarProfile::Names::UnaryPrefixToken = { "!", "+", "-", "~" };

// 整形対象外ノード（内部構造を保持し原文の返戻）
const GrammarProfile::Names::NameSet GrammarProfile::Names::Unformattable =
{ "heredoc_beginning", "heredoc_body", "template_literal_type", "template_string", "template_substitution", "template_type" };

// 変数宣言ノード（DeclLike に let_declaration / property_declaration を含めた拡張バージョン）言語横断
const GrammarProfile::Names::NameSet GrammarProfile::Names::VarDecl = {
	"declaration",
	"let_declaration",
	"lexical_declaration",
	"local_variable_declaration",
	"property_declaration",
	"variable_declaration" // JS/TS の `var x = ...`
};

// 名前を `name` 場に持つ Java / C# の変数の宣言 — `ControlFlow::DeclarationsIn`
const GrammarProfile::Names::NameSet GrammarProfile::Names::VarDeclarator = {
	"catch_declaration",
	"catch_formal_parameter",
	"enhanced_for_statement",
	"formal_parameter",
	"parameter",
	"variable_declarator"
};

// 子から組み直さず原文をコピーするノード（`ERROR` は構造が信用出来ず，`shebang_line` は解釈系の経路を子に持たない）
const GrammarProfile::Names::NameSet GrammarProfile::Names::VerbatimNode = { "ERROR", "shebang_line" };
// 先に条件を判ずる繰返 (while_expression / while_statement)
const GrammarProfile::Names::NameSet GrammarProfile::Names::WhileLoop = { "while_expression", "while_statement" };

// Go の省略可能なセミコロンを持つ文・メンバ・インポートのコンテナ
const GrammarProfile::Names::NameSet GrammarProfile::Names::GoSemicolonHost {
	StmtList,
	GoMemberList,
	{ "import_spec_list", "source_file" }
};

// Go の自動の文終端の前で内部を１トークンとして読むリテラル
const GrammarProfile::Names::NameSet GrammarProfile::Names::GoSemicolonLiteral {
	"interpreted_string_literal",
	"raw_string_literal",
	"rune_literal"
};

// Go の自動の文終端を挿入する名前付のトークン（共通の識別子・数値ロール以外）
const GrammarProfile::Names::NameSet GrammarProfile::Names::GoSemicolonNamedToken {
	"blank_identifier",
	"false",
	"field_identifier",
	"imaginary_literal",
	"iota",
	"label_name",
	"nil",
	"package_identifier",
	"true",
	"type_identifier"
};

// Go の自動の文終端を挿入する名前無のトークン
const GrammarProfile::Names::NameSet GrammarProfile::Names::GoSemicolonToken {
	")",
	"++",
	"--",
	"]",
	"break",
	"continue",
	"fallthrough",
	"return",
	"}"
};

// 投影でデコードするリテラルの部品
const GrammarProfile::Names::NameSet GrammarProfile::Names::DecodedLiteralPart {
	"escape_sequence",
	"heredoc_end",
	"heredoc_start",
	"string_content",
	"string_fragment"
};

// デコード対象の集合を変えず，逐語保持する本文を加えたリテラルの内容
const GrammarProfile::Names::NameSet GrammarProfile::Names::LiteralContent {
	DecodedLiteralPart,
	{ "heredoc_content", "interpolated_string_text", "line_str_text", "multi_line_str_text", "raw_string_content", "string_value" }
};

// 型の導入を持つ仮引数（Python の同名の並びは使用側の文法で除く）
const GrammarProfile::Names::NameSet GrammarProfile::Names::NamedTypeParam { "type_parameter", "type_parameter_pack" };

// 型の仮引数の並び
const GrammarProfile::Names::NameSet GrammarProfile::Names::GenericParamList {
	"template_parameter_list",
	"type_parameter_list",
	"type_parameters"
};
