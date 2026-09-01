# Unnamed トークンスペース正規化規則

本書は Shave Format が tree-sitter AST の **unnamed トークン**（演算子・区切記号・括弧・キーワード等）の前後スペースを正規化する規則を，対象１６言語 (C / C++ / C# / Java / Go / Rust / Kotlin / Swift / PHP / JS / TS / Ruby / Python / JSON / HTML / CSS) 横断で示す．SCSS は独立 `Lang::ID` を持たない為 CSS パーサで処理する．

判定本体は [src/Pass/Structure.cpp](../src/Pass/Structure.cpp) の `StructurePass::NeedsGapBetween` に在り，親ノード種別の集合判定で分岐する．集合の定義は [src/Util/NodeKind.cpp](../src/Util/NodeKind.cpp) に在る．本書の表は主な判定を示し，先に成立した分岐を優先する．

## 凡例

| 表記 | 意味 | 例 |
|---|---|---|
| `_ op _` | 前後にスペース | `a + b` |
| `op _` | 後のみスペース | `a: b` |
| `_ op` | 前のみスペース | `a :b` |
| `op` | スペース無 | `a:b` |

---

## 1. 二項演算子 (`_ op _`)

`NodeKind::InfixOp` 集合の子として出現する unnamed 演算子トークン．`NeedsGapBetween` の `InfixOp` 分岐で前後にスペースを挿入する．`InfixOp` 集合は親ノード型のみを列挙し，個別トークンは集合に含まれない．親ノード型ベースで分岐し，トークンは親型で許容される範囲で前後スペース付与される．以下の表は「親ノード型（右）で出現する unnamed トークン例（左）」を示す．

| 出現する unnamed トークン例 | 親ノード型 |
|---|---|
| `+` `-` `*` `/` `%` `**` `//` | `binary_expression`, `additive_expression`, `multiplicative_expression`, `binary_operator` 等 |
| `&` `\|` `^` `<<` `>>` `>>>` | `binary_expression` |
| `&&` `\|\|` `and` `or` | `binary_expression`, `conjunction_expression`, `disjunction_expression`, `boolean_operator` 等 |
| `==` `!=` `===` `!==` `<=>` `=~` `!~` | `binary_expression`, `equality_expression`, `comparison_operator` 等 |
| `<` `>` `<=` `>=` | `binary_expression`, `comparison_expression`, `comparison_operator` 等 |
| `=` `+=` `-=` `*=` `/=` `%=` 他 | `assignment`, `assignment_expression`, `assignment_statement`, `augmented_assignment`, `augmented_assignment_expression`, `compound_assignment_expr`, `operator_assignment` 等 |
| `??` `?:` | `binary_expression`, `elvis_expression` |
| `in` `!in` `instanceof` `is` `!is` `as` `as?` | `binary_expression`, `as_expression`, `is_expression`, `is_pattern_expression` 等 |
| `?` `:` | `conditional`, `conditional_expression`, `ternary_expression`（→ § 3a 参照） |
| `=>` | `arrow_function`, `match_arm`, `switch_expression_arm` 等 |
| `->` | `lambda_expression`, `lambda_literal`, `function_type` 等 |
| `<-` | `send_statement`(Go) |
| `:=` | `short_var_declaration`, `range_clause`(Go)，`named_expression`(Py walrus) |

**例外:** `InfixOp` 子でも `:` `,` `;` 単独は `NeedsGapBetween` 内部で除外され，個別ルールに委ねる．

## 1b. 範囲演算子（密着）

`NodeKind::RangeLike` 又は Swift の `SwiftRangeOrUnary` に属する親の範囲演算子は密着させる．§ 1 の二項演算子 (`_ op _`) には属さない．

| トークン | 親ノード例 |
|---|---|
| `..` `...` `..=` `..<` | `range`, `range_expression`, `range_pattern`, `open_end_range_expression`, `open_start_range_expression` |

## 2. 単項演算子

### 2a. 前置単項 (`op x`)

`NodeKind::UnaryPreOrUpdate` 集合．現トークンが unnamed，次が named の場合，英字始まりの演算子又は同符号の `+ +`／`- -` の境界にスペースを挿入し，其れ以外は密着させる．

| トークン | 親ノード例 |
|---|---|
| `!` `-` `+` `~` `*`(deref) `&`(ref) | `unary_expression`, `pointer_expression`, `prefix_unary_expression`, `reference_expression` 等 |
| `++` `--` | `update_expression`（前置） |
| `not` | `not_operator`(Python) |

`typeof` `delete` `await` `void` は `NodeKind::SpaceKeyword` にも含まれる（§ 9b 参照）．

### 2b. 後置単項 (`x op`)

`NodeKind::UnaryPost` 集合．前を密着させる．

| トークン | 親ノード例 |
|---|---|
| `++` `--` | `postfix_expression`, `postfix_unary_expression`, `update_expression`, `inc_statement`, `dec_statement` |
| `!` | `non_null_expression`(TS) |
| `!!` | `postfix_unary_expression`(Kotlin) |

## 3. コロン `:` の分類

三項等は先行分岐で扱い，其の他は `NextIs({":"})` でコロン前の空白を判定する．何れにも該当しない場合は前後にスペースを挿入する（= `x : y` 形）．

### 3a. 前後スペース ` : `（`NodeKind::Ternary` 経由）

`Ternary` は三項演算子と range-for（C++ / Java の `for(x : c)` 系）を統合した集合．`Ternary` 親の `?` `:` は，PHP の `?:` の記号間を除き前後スペースを挿入する．

| 親ノード | 言語 | 例 |
|---|---|---|
| `conditional` | Ruby | `x ? a : b` |
| `conditional_expression` | C / C++ / C# / PHP | `x ? a : b` |
| `conditional_type` | TS | `A extends B ? C : D` |
| `ternary_expression` | Java / JS / TS | `x ? a : b` |
| `base_class_clause` | C++ | `class A : public B` |
| `base_list` | C# | `class A : B` |
| `enhanced_for_statement` | Java | `for(int x : arr)` |
| `field_initializer_list` | C++ | `Foo() : x(1)` |
| `for_range_loop` | C++ | `for(auto &v : arr)` |

### 3b. 後のみスペース `x: T`(`NodeKind::NormColon`)

`NormColon` 集合の親で `:` を含む箇所は NeedsGapBetween の `NextIs(":")` 分岐で前スペース無を返戻する．後続子との境界で別ルートにより後スペースが挿入される．

| 親ノード | 言語 | 例 |
|---|---|---|
| `pair` | JS / TS / Rb / Py | `a: 1` |
| `dict_pattern` | Py | `case {"a": x}` |
| `property_signature` | TS | `x: number` |
| `public_field_definition` | TS | `x: T = 1` |
| `type_annotation` | Swift / TS | `: number` |
| `typed_default_parameter` | Py | `def f(x: int = 1)` |
| `typed_parameter` | Py | `def f(x: int)` |
| `keyword_pattern` | Py | `case C(x=1)`（コロン無） |
| `keyword_parameter` | Ruby | `def f(name:, age: 0)` |
| `parameter` | Rust / Kotlin / Swift | `x: i32` |
| `class_parameter` | Kotlin | `val n: String` |
| `function_declaration` | Kotlin | `fun f(): Int` |
| `variable_declaration` | Kotlin | `val x: Int` |
| `field_initializer` | Rust | `S { a: 1 }` |
| `keyed_element` | Go | `"a": 1`(map / composite literal) |
| `declaration` | CSS | `property: value` |
| `feature_query` | CSS | `(prop: value)` |
| `media_feature` | CSS | `(prop: value)` |
| `dictionary_literal` | Swift | `["a": 1]` |
| `dictionary_type` | Swift | `[String: Int]` |
| `inheritance_constraint` | Swift | `T: Protocol` |
| `associatedtype_declaration` | Swift | `associatedtype X: Y` |
| `type_parameter` | Swift | `T: Protocol`（`NormColon` 経由で後のみスペース） |
| `value_argument` | Swift | `f(name: value)` |
| `attribute` | Swift | `@Published(initialValue: 0)` |
| `enum_type_parameters` | Swift | `case .x(value: Double)` |
| `lambda_parameter` | Swift | `{ (x: Int) in ... }` |
| `pattern` | Swift | `case .x(label: bindings)` |
| `trait_bounds` | Rust | `: Clone + Debug` |

Swift の継承コロンは `class_declaration` / `protocol_declaration` の直下に在り，既定の前後スペースを用いる (`class Dog : Animal`, `protocol P : AnyObject`)．`inheritance_specifier` は継承先だけを含む．

### 3c. Rust の型注釈 `x: T`

以下の親ノードでは `InfixOp` 又は `NormColon` に依りコロンの後にのみスペースを残す．`trait_bounds` は `NodeKind::NormColon` 集合経由（§ 3b 参照）．

| 親ノード | 言語 | 例 | 経路 |
|---|---|---|---|
| `field_declaration` | Rust | `pub x: i32` | `InfixOp` |
| `let_declaration` | Rust | `let x: i32 = 5` | `InfixOp` |
| `const_item` | Rust | `const MAX: usize = 100` | `NormColon` |
| `static_item` | Rust | `static G: &str = "x"` | `NormColon` |

### 3d. スペース無 `:`(`NodeKind::ColonControl` / `NodeKind::SectionHeader`)

ブロック開始 / case ラベル / Python の `:` 等．`ColonControl` 親又は `SectionHeader` 直後の `:` は前スペース無で密着させる．

| 親ノード | 言語 | 例 |
|---|---|---|
| `class_definition` | Py | `class A:` |
| `function_definition` | Py | `def f():` |
| `if_statement` | Py | `if x:` |
| `elif_clause` | Py | `elif x:` |
| `else_clause` | Py | `else:` |
| `for_statement` | Py | `for x in y:` |
| `while_statement` | Py | `while x:` |
| `match_statement` | Py | `match x:` |
| `case_clause` | Py | `case C(x):` |
| `with_statement` | Py | `with x:` |
| `try_statement` | Py | `try:` |
| `except_clause` | Py | `except E:` |
| `finally_clause` | Py | `finally:` |
| `lambda` | Py | `lambda x:` |
| `case_statement` | C / C++ | `case 1:` `default:` |
| `default_statement` | PHP | `default:` |
| `default_case` | Go | `default:` |
| `expression_case` | Go | `case 1:` |
| `type_case` | Go | `case int:` |
| `communication_case` | Go | `case ch <- v:` |
| `labeled_statement` | C / C++ / C# / Java / Go / JS / TS | `label:` |
| `switch_block_statement_group` | Java | `case 1:` |
| `switch_label` | Java | `case 1:` |
| `switch_case` | JS / TS | `case 1:` |
| `switch_default` | JS / TS | `default:` |
| `switch_section` | C# | `case 1: stmts;` |
| `switch_entry` | Swift | `case .x:` |
| `access_specifier` | C++ | `public:` `private:` `protected:` |

### 3e. CSS 固有

`feature_query` `media_feature` は § 3b の `NormColon` 経由で後のみスペース挿入．以下は CSS 固有の密着ケース．

| 親ノード | 例 | 処理 |
|---|---|---|
| `pseudo_class_selector` | `:hover` | `Skip`（密着） |
| `pseudo_element_selector` | `::before` | `Skip`（密着） |

## 4. セパレータ (`,` / `;`)

前は密着．後は次が `)` `]` `;` `,` 以外ならスペースを挿入する．コロン後では更に `}` の直前も密着させる．

| トークン | 親ノード例 |
|---|---|
| `,` | `arguments`, `argument_list`, `parameters`, `array`, `object` 等４０種以上 |
| `;` | `declaration`, `expression_statement`, `for_statement` 等 |

## 5. 括弧

### 5a. 開き括弧 (`(` `[`)

`CurIs({"(", "["})` の場合，後を密着させる．逆に Next が `(` / `[` の場合は文脈依存（次節参照）．

呼出名と実引数列（`NodeKind::AttachParen`）は密着させる．但し Ruby の括弧無の呼出で最初の実引数が括弧で始まる形（`p (1..6).to_a`：実引数列の最初の子が `(` の字句でない）は空白を保つ（密着させると括弧が実引数列に読まれ，結合が変わる）．

### 5b. 閉じ括弧 (`)` `]`)

`NextIs({")", "]"})` で前を密着させる．

### 5c. `{` / `}` の文脈依存

| 親ノード集合 | 処理 |
|---|---|
| `NodeKind::SpacelessBrace`(`jsx_expression`) | 密着 (`{expr}`) |
| `NodeKind::ObjectLike`（`object`, `hash`, `dictionary`, `interface_body`, `enum_body`, `initializer_list`, `literal_value` 等） | 内側にスペース `{ x }` |
| `NodeKind::IndentContainer`（`block`, `compound_statement`, `statement_block`, `class_body`, `function_body` 他） | 改行展開 (structured) |
| `NodeKind::BraceAttach`(`composite_literal`, `compound_literal_expression`) | 型名直後に密着 (Go `T{...}` / C `(T){...}`) |
| `NodeKind::BraceNoSpace`(Go `interface_type` / `struct_type`) | 開き `{` の前を密着 |
| `lambda_literal`(Kotlin) | 内側にスペース |

`{` と `}` が連続する空ブレース `{}` は常に密着．

## 6. メンバアクセス (`.` `->` `::` `?.` `&.`)

`.` `&.` `?.` のトークン前後は親型に依らず密着させる．名前付子のドット境界と任意連鎖も専用判定で密着させる．但し Python と JS / TS で小数点も指数も持たない十進整数の直後の `.` は小数点と読まれる為，空白で区切る（`6 .real` / `1 .toString()`）．

| トークン | 親ノード例 |
|---|---|
| `.` | `member_expression`, `field_expression`, `navigation_expression`, `scoped_identifier`, `directly_assignable_expression` 等 |
| `?.` | `member_expression`, `call_expression`, `navigation_suffix`, `optional_chain` |
| `::` | `scoped_identifier`, `qualified_identifier` |
| `->` `?->` | `field_expression`, `member_access_expression`, `nullsafe_member_access_expression` 等 |
| `.*` `->*` | C++ メンバポインタ（前後スペース） |
| `&.` | Ruby safe navigation |

`Member` は上表のメンバ参照・呼出と `jsx_attribute` 等を含む．JSX 属性 `<C foo="v" />` の `=` も密着させる．

**注意**：`->` `=>` `?->` は `Member` 親の場合のみ密着．其れ以外（例: `arrow_function`, `lambda_expression`）では前後スペース挿入．

## 7. 型パラメータ括弧 (`<` `>`)

`NodeKind::AngleBracketList` 集合．

| トークン | 親ノード | 処理 |
|---|---|---|
| `<` `>` | `template_argument_list`, `template_parameter_list`, `type_argument_list`, `type_arguments`, `type_parameter_list`, `type_parameters` | 内側密着 |
| `/>` | `jsx_self_closing_element`, `self_closing_tag` | HTML / JSX 固有 |
| `</` | `jsx_closing_element`, `end_tag` | HTML / JSX 固有 |

## 8. HTML / JSX タグ

`NodeKind::HtmlTag` 親 (`start_tag`, `end_tag`, `jsx_opening_element`, `jsx_closing_element`, `jsx_self_closing_element`, `self_closing_tag`) では，`<` `</` の後と `>` の前を密着させ，`/>` の前に１スペースを置く．

HTML のタグ境界は NeedsGapBetween 冒頭でも密着を判定する．`element` 本体のテキスト・要素間では原空白の有無を保持する．

## 9. キーワード

### 9a. 制御キーワード (`NodeKind::ControlKeyword`)— `(` 前密着

`if(x)` 形．C++ の `if constexpr(x)` も `(` の前にスペースを挿入しない．

`catch` `elif` `elsif` `except` `for` `foreach` `if` `switch` `when` `while`

### 9b. スペースキーワード (`NodeKind::SpaceKeyword`)— `(` 前スペース

`return (x)` 形．`(` `[` 前にスペースを挿入する．C# の `typeof(` と Ruby の `defined?(` は先行分岐で密着させる．

`as` `async` `await` `defined?` `delete` `extends` `from` `import` `in` `let` `new` `not` `print` `raise` `return` `throw` `typeof` `void` `yield`

### 9c. 其の他のキーワード（一般ルール）

`class` `struct` `enum` `interface` `fn` `func` `fun` `def` `const` `var` `val` `static` `pub` `private` `public` `impl` `trait` `use` `package` `module` `namespace` `break` `continue` `goto` `do` `else` `case` `default` `try` `finally` `where` `with` `is` `dyn` `unsafe` `suspend` `override` `abstract` `sealed` 他．英字終端 + 後続 named identifier の境界で空白挿入される一般ルートに委ねる．

## 10. 文字列 / リテラル囲み（密着）

文字列片は `Skip` 等で保持する．JS / TS の `${...}` 内は式を整形し，囲み記号へ密着させる．Ruby の補間式は保持するが，配列要素間・連結文字列間・ヒアドキュメント境界は別途整形する．

| トークン | 親ノード例 |
|---|---|
| `"` | `string_literal`, `string`, `quoted_attribute_value` |
| `'` | `lifetime`(Rust)，`char_literal`, `character_literal` |
| `` ` `` | `template_string` |
| `#{` `}` | `interpolation`(Ruby) |
| `${` `}` | `template_substitution`(JS / TS) |

## 11. プリプロセッサ（特殊処理）

指令テキストは `BuildPreprocText` で前後・行頭の空白と改行直前の半角空白を除き，指令境界へ改行を挿入する．行内の連続空白は１つへ畳み，指令名の前の空白（`#  define`）は詰める．文字列・文字リテラル・コメントと取込指令（`#include` / `#import` 等）の `<...>` の内側は内容として保つ．指令名は処理系の拡張（`#import` / `#include_next` / `#ident` 等）も受け付ける．条件付ブロックは AST から再構築する．

| トークン | 親ノード例 |
|---|---|
| `#define` `#if` `#ifdef` `#endif` 他 | `preproc_def`, `preproc_if`, `preproc_ifdef`, `preproc_function_def` 等 |

## 12. CSS / SCSS 固有

| トークン | 親ノード | 処理 |
|---|---|---|
| `.` | `class_selector` | 密着（`NodeKind::CssSelectorAtomic` の `class_name` 経由） |
| `#` | `id_selector`, `color_value` | 密着 |
| `:` | `pseudo_class_selector` | 密着（`Skip` 経由） |
| `::` | `pseudo_element_selector` | 密着 |
| `%` | SCSS `placeholder` | 密着（`%placeholder` を modulo 化させない） |
| `&` | SCSS `class_selector` の nesting / `nesting_selector` | 密着（`& __x` 化を防止） |
| `>` `+` `~` | `child_selector`, `adjacent_sibling_selector`, `sibling_selector` | 前後スペース（一般 InfixOp 経路ではなくセレクタ親の境界に依存） |
| `@media` `@import` 他 at-rule | at-rule 親 | 後スペース |

## 13. Rust 固有

| トークン | 親ノード | 処理 |
|---|---|---|
| `'` | `lifetime` | `Skip`（`'a` 密着） |
| `!` | `macro_invocation`, `macro_definition`(`NodeKind::MacroLike`) | 密着 (`println!`) |
| `dyn` | `dynamic_type` | スペース後 |
| `macro_rules!` | `macro_definition` | スペース後 |

Rust の `reference_expression` は `UnaryPreOrUpdate`，`reference_type` / `self_parameter` は `PointerDecl` 経由で参照記号を密着させる．`PointerDecl` 集合は C / C++ の pointer/reference declarator や Python `*args` / `**kwargs`，JS / TS の `...rest`，Ruby の splat / ブロック引数等の言語横断 splat / spread / reference を含む為，§ 14 を参照．

## 14. 其の他の言語固有の不可分トークン

実装の `NeedsGapBetween` 内で密着強制される代表ケース．

| 言語 | 親ノード / トークン | 例 |
|---|---|---|
| 言語横断 | `NodeKind::PointerDecl`（C/C++ pointer/reference declarator, Python splat / dict-splat, JS/TS rest / spread, Ruby splat / block_argument, Rust reference 等） | `int *p` `&self` `**kwargs` `...rest` `*args` |
| C / C++ | `attribute_declaration` の `[[` / `]]` | `[[nodiscard]]` |
| C++ | `binary_expression` 内のキャスト風 `(type)` + 単項 | `(counter_t)-1` |
| C# | `NewParenCall` の `new()` | `where T : new()` |
| Java | `spread_parameter` の `...` | `T...` |
| Kotlin | `check_expression` の `!` + `is`/`in`（`NodeKind::KotlinNotIsIn` = `{"in", "is"}` 集合で判定） | `!is`, `!in` |
| Kotlin | `KotlinNumericLiteralSuffix`(`long_literal` / `real_literal` / `unsigned_literal`) | `100L` `1.5F` `100UL` |
| Kotlin | `KotlinAnnotationLike`(`annotation` / `file_annotation` / `use_site_target`) | `@file:JvmName(...)`（`user_type` 同士を除き密着） |
| Kotlin | `jump_expression` の `return@label` | `return@outer` |
| Swift | `SwiftRangeOrUnary`(`open_end_range_expression`, `open_start_range_expression`, `postfix_expression`, `prefix_expression`, `range_expression`) | `0..<10` `...N` `x!` `-x` |
| Swift | `type_annotation` 直下の IUO `!` | `var x: Foo!` |
| PHP | `PhpTypeCombination`(`disjunctive_normal_form_type` / `intersection_type` / `type_list` / `union_type`) | `int\|false` `A&B` `(A&B)\|null` `catch(\A\|\B $e)` |
| PHP | `reference_assignment_expression` の `&` | `$ref = &$obj->cache` |
| PHP | `attribute_group` の `#[` | `#[Attribute(...)]` |
| PHP | `conditional_expression` の Elvis `?:` | `$s ?: 'default'` |
| PHP | `nullsafe_member_access_expression` / `nullsafe_member_call_expression` の `?->` | `$order?->pay(100)` |
| PHP | `namespace_name` / `qualified_name` / `variable_name`（`Leaf` で逐語保持） | `App\Service` `\App\Model\Item` `$name` |
| JS / TS | `GeneratorStarHost` の `*` | `function* gen()` |
| Ruby | `HashSplat`(`hash_splat_argument` / `hash_splat_parameter` / `hash_splat_pattern`) | `f(**h)` `def f(**opts)` |
| Ruby | `block_argument` の `&` | `arr.map(&:to_s)` |
| Ruby | `RubyArrayLiteral`(`string_array` / `symbol_array`) | `%w(a b c)` `%i(:a :b)` |
| Ruby | `defined?` キーワード | `defined?(a)` |
| Python | `type_alias_statement` の `type(x)` | `type(x).attr` |

---

## 関連

- [processing_spec.md](processing_spec.md) § 3.5.3 NeedsGapBetween — 本書の判定本体の上位仕様．
- [line_split_order.md](line_split_order.md) — 改行挿入位置の確定順とグループ化規則．
- [src/Pass/Structure.cpp](../src/Pass/Structure.cpp) — `StructurePass::NeedsGapBetween` 実装．
- [src/Util/NodeKind.cpp](../src/Util/NodeKind.cpp) — 親ノード集合の定義．
- [CODING_STANDARDS.md](../CODING_STANDARDS.md) — 整形が準拠するコーディング規約．

---

*本仕様書はコード実装と併せて保守する．実装が変更された場合は本書を必ず追従させる事．*
