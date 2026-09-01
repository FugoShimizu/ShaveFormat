#pragma once

#include <tree_sitter/api.h>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

// ノード種別集合クラス
class NodeKind {
public:

	// 整列済集合クラス
	class NodeTypeSet {
	private:

		std::vector<std::string_view> Slots; // 要素格納スロット列
		uint64_t FirstCharMask; // 各要素先頭文字（`& 0X3F` の６ビット）を立てた６４ビットマスク
		uint64_t SizeMask; // 各要素の長さ (1..63) を立てた６４ビットマスク，長さ不一致で即 false 判定
		static size_t HashOf(const std::string_view Type); // ハッシュ値算出関数

	public:

		bool Contains(const std::string_view Type) const; // 包含判定関数
		bool Contains(const TSNode Node) const; // 節点の型の包含判定関数

		NodeTypeSet(const std::initializer_list<std::string_view> Init); // コンストラクタ
	};

	static const NodeTypeSet AccessOrderMember; // Java/C#/PHP のアクセス修飾子順序検査対象成員
	static const NodeTypeSet AmbiguousBodyContainer; // 型の本体とは限らない本体
	static const NodeTypeSet AngleBracketList; // `<` `>` で囲まれる型引数／型パラメータリスト（内側スペース判定と平坦化での内部分割優先で共用）
	static const NodeTypeSet ArrowToken; // 矢印の字句 (-> / =>) — 前後に空白を置く
	static const NodeTypeSet AssignmentExpression; // 代入式系 (assignment / assignment_expression / augmented_assignment_expression) 言語横断
	static const NodeTypeSet AwaitTryCastExpression; // `await` / `try`／型キャストの式
	static const NodeTypeSet AttachBracket; // `[` を直接隣接付加するノード
	static const NodeTypeSet AttachColonOrDot; // `:` 又は `.` を直前トークンに直接隣接付加するノード
	static const NodeTypeSet AttachParen; // `(` を直接隣接付加する引数／仮引数リスト
	static const NodeTypeSet AttachPrefix; // 後続要素に付く属性・注釈・デコレータ
	static const NodeTypeSet AutoParameterHost; // auto を型名の代わりに書ける C++ の仮引数の親 (for_range_loop / parameter_declaration)
	static const NodeTypeSet BareDirective; // コロンを持たない行コメント形の指令（`//export` 等）
	static const NodeTypeSet BodyDeclaration; // 制御構文の本体で波括弧を保持する宣言
	static const NodeTypeSet BoolInitContext; // 真偽値型の初期値を持ち得る宣言・代入ノード (assignment / declaration / field_declaration)
	static const NodeTypeSet BraceAttach; // `{` を型名直後に隣接付加するリテラル
	static const NodeTypeSet BraceClause; // 閉じ波括弧に続く本体付節（指令の錨を本体へ降ろす）
	static const NodeTypeSet BraceNoSpace; // 空の本体を語へ密着させる Go の型定義ノード (interface_type / struct_type)
	static const NodeTypeSet BraceRemoveStmtExcluded; // 本体の波括弧を外さない文 (catch_clause / repeat_while_statement)
	static const NodeTypeSet BraceWrapStmtExcluded; // 本体へ波括弧を付けない文 (repeat_while_statement / try_expression)
	static const NodeTypeSet BracketOpaque; // 括弧計数から除外する不透明リテラル（正規表現等，字面では除算と区別出来ず構文木に依拠する）
	static const NodeTypeSet BracketType; // `[` 始まりの型ノード (array_type / dictionary_type / slice_type) — Structure NeedsGapBetween の隣接判定
	static const NodeTypeSet BreakKeyword; // 脱出の語 (break / break@) — 語で始まる脱出の文の種別
	static const NodeTypeSet BreakOpaque; // 内部を行分割候補にしないノード（Rust マクロのトークンツリー本体／Rb 隣接文字列連結／Rb rescue 修飾子）
	static const NodeTypeSet CCastInner; // C/C++ キャスト `(type)` 内側に出現し得る型部位（丸括弧削除抑止対象）
	static const NodeTypeSet CCastTypeNameInner; // C/C++ の二項式で括弧内を型名と看做せる単独子
	static const NodeTypeSet CDeclaratorWrapper; // C の宣言子を包む宣言子 (attributed_declarator / parenthesized_declarator / pointer_declarator) — 関数定義の宣言子の検査
	static const NodeTypeSet CFunctionOrFieldDeclarator; // C/C++ の関数宣言子経路及び欄のメソッド判定用宣言子
	static const NodeTypeSet CNameDeclaration; // 名前を宣言する C / C++ の宣言 (declaration / parameter_declaration) — 同じファイルで宣言した名前の収集
	static const NodeTypeSet CParenDeclarator; // C の丸括弧で始まり得る宣言子（型との空白判定）
	static const NodeTypeSet CSharpDeclPrefix; // C# の宣言で宣言子の並びの前に並ぶ修飾子・属性 (attribute_list / modifier) — 型の頭の一部として統合・分割する
	static const NodeTypeSet CStatementKeyword; // 文の始まりの C / C++ の語 (break / continue / do / for / goto / if / return / switch / while)
	static const NodeTypeSet CaseBodyOpener; // switch の節の本体を開く字句 (-> / :)
	static const NodeTypeSet CaseClause; // switch の節（言語横断）（case_statement / switch_section / when_entry 等）
	static const NodeTypeSet CastExpr; // キャスト式ノード (cast_expression / type_cast_expression) — Structure NeedsGapBetween の密着判定
	static const NodeTypeSet ClassBody; // クラス本体ノード（アクセス修飾子の順序検査対象）
	static const NodeTypeSet ClassBodyContainer; // クラス／構造体／インターフェース等のメンバを直接保持するコンテナ
	static const NodeTypeSet ClassLike; // クラス系コンテナノード（`class` / `struct` / `interface` / `enum` / `module` / `namespace` 等）全言語横断
	static const NodeTypeSet ClassLikeStrict; // 列挙・インターフェースを除くクラス／構造体宣言
	static const NodeTypeSet ClauseBody; // 最後の子に本体の文を持つ節 (colon_block / else_clause / else_if_clause) — 末尾のコメントを本体の最後の文の後へ置く
	static const NodeTypeSet CloserToken; // 閉じ字句（括弧・塊・要素の閉じと PHP の代替構文の終端語）— コメントの塊の末尾の判定
	static const NodeTypeSet CmpBoundaryNode; // 言語横断の比較境界検査対象
	static const NodeTypeSet ColonControl; // `:` 前スペース不要な制御構文（case/label/lambda 等）
	static const NodeTypeSet CombinatorSelector; // CSS/SCSS の結合子セレクタ（子孫／子／兄弟）— 子の pseudo は結合子前提で密着させない
	static const NodeTypeSet Comment; // コメントノード
	static const NodeTypeSet Compact; // 内部スペース・改行を入れない密集ノード（属性／単項演算子等）
	static const NodeTypeSet CompactRecurse; // 内部に分割可能子を持つ Compact 類似節点
	static const NodeTypeSet CondContextParent; // 条件式が直接置かれる制御構文親 — `Lint::CheckZeroComparisonCondition` の文脈判定
	static const NodeTypeSet CondParenWrap; // 条件式を包む括弧ノード (condition_clause / parenthesized_expression) — Lint の条件文脈剥がし
	static const NodeTypeSet ConditionFieldHost; // 条件を場 condition に持つ節点 (conditional_expression / for_statement / ternary_expression)
	static const NodeTypeSet ConditionalAlternative; // else の位置で自身が条件と本体を持つ分岐（else 節に包まれない else if の if・PHP の elseif 節）
	static const NodeTypeSet ConstructionReceiverHost; // 生成を最初の子に持つ後置の式（member_expression / member_call_expression 等） — 生成の空の実引数の保持
	static const NodeTypeSet ContinueKeyword; // 継続の語 (continue / continue@)
	static const NodeTypeSet Control; // 制御構文ノード（条件 `()` の括弧維持対象，括弧維持集合の構成要素）
	static const NodeTypeSet ControlKeyword; // 制御キーワード（catch/elif/else 等）
	static const NodeTypeSet CppBuiltinType; // C++ の組込型 (primitive_type / sized_type_specifier)
	static const NodeTypeSet CppConstLiteral; // C / C++ の `constexpr` 候補初期化子リテラル — `Lint::CheckConstexprCandidate`
	static const NodeTypeSet CppDeclTypeSpecifier; // C/C++ declaration の型部位 (primitive_type / type_identifier / qualified_identifier / sized_type_specifier)
	static const NodeTypeSet CppDeclarationName; // 宣言の先頭に置ける C++ の名前・型の節点 — 宣言にも読める式の判定
	static const NodeTypeSet CppNullCandidate; // 空ポインタの定数の候補 (identifier / null) — NULL マクロの検査
	static const NodeTypeSet CppScopeBoundary; // C++ の型引数の遡りを止める範囲 (compound_statement / translation_unit)
	static const NodeTypeSet CppUnnamableInitializer; // 型名を書けない C++ の初期化子 (lambda_expression / structured_binding_declarator) — auto の例外
	static const NodeTypeSet CsNameType; // 式としても読める C# の型名 (identifier / qualified_name)
	static const NodeTypeSet CsPostfixChain; // C# の後置式の連鎖
	static const NodeTypeSet CssNameFragmentHost; // 断片を密着させる CSS の名前 (property_name / tag_name)
	static const NodeTypeSet CssNamespacePrefix; // 名前空間の区切り `|` を持つ CSS の選択子の節点 (attribute_name / namespace_selector)
	static const NodeTypeSet CssNumericValue; // CSS/SCSS の数値値 (integer_value / float_value) — 単位 plain_value とのバイト隣接密着判定用
	static const NodeTypeSet CssSelectorAtomic; // CSS/SCSS の最小セレクタ系 (class_name / id_name / placeholder) — 内部空白挿入禁止
	static const NodeTypeSet CssSelectorHost; // 選択子を取る CSS の節点 (extend_statement / pseudo_class_selector / pseudo_element_selector) — 中の `#name` は色でない
	static const NodeTypeSet CssSelectorName; // CSS の選択子の名前 (class_name / id_selector) — 選択子の内側で密着
	static const NodeTypeSet DeclContainer; // 同型一括宣言を内包し得るコンテナ
	static const NodeTypeSet DeclDeclaratorChild; // 宣言子（変数名 + 任意の初期化子）ノード
	static const NodeTypeSet DeclInitDeclarator; // 初期化子を伴う宣言子 (init_declarator / variable_declarator)
	static const NodeTypeSet DeclLike; // 一括宣言可能な宣言ノード
	static const NodeTypeSet DeclTypeChild; // 宣言の型部位ノード
	static const NodeTypeSet DecltypeIdExpression; // 括弧の有無で decltype の推論する型が変わる式（名前・メンバアクセス）
	static const NodeTypeSet DefaultLabel; // 既定の節の字句 (default / default_keyword / else)
	static const NodeTypeSet DestructurePattern; // 分割代入パターン (array_pattern / object_pattern) — 単一名を持たない為ドキュメント／let 再代入解析でスキップ
	static const NodeTypeSet DoLoop; // 本体の後に条件を置く繰返 (do_statement / do_while_statement) — 本体が最初の名前付子
	static const NodeTypeSet DocWrapper; // ドキュメントコメントのアンカーを１階層包むラッパ (ambient_declaration / export_statement / template_declaration)
	static const NodeTypeSet ElseAny; // else 系 (else / else_clause) — else を持たない if の判定
	static const NodeTypeSet ElseIfChainParent; // `else-if` 連鎖の親候補（`else_clause` / `if_statement`）— `LineSplit::ApplyBraceWraps` の連鎖除外判定
	static const NodeTypeSet ElseKeyword; // if の後続の枝を始める字句（else / PHP の elseif）
	static const NodeTypeSet ElseLikeClause; // else 系の節 (else_clause / catch_clause / finally_clause)
	static const NodeTypeSet ExitKeyword; // 関数を抜ける語 (return / throw / throw_keyword) — 語で始まる脱出の文の終端判定
	static const NodeTypeSet ExpandContainer; // 展開形を組み立てる丸／波括弧コンテナ
	static const NodeTypeSet ExprStmtLiteral; // expression_statement 直下の丸括弧削除を抑止するリテラル系（数値／文字列／真偽値）
	static const NodeTypeSet FieldCallee; // 被呼出側の欄の参照 (field_expression / member_access_expression / navigation_expression) — 欄の関数の呼出の括弧の保持
	static const NodeTypeSet FlowExit; // 制御フロー脱出文（return/break/continue/goto/throw 系）
	static const NodeTypeSet ForEachHeader; // 反復対象を区切語の後に置く見出（対象の括弧除去用）
	static const NodeTypeSet FunctionBodyContainer; // 関数本体ノード (block / compound_statement / function_body / statement_block) 言語横断
	static const NodeTypeSet FunctionLikeAny; // 関数の範囲の境界（定義・関数式・ラムダ・局所関数）
	static const NodeTypeSet FunctionLikeDefinition; // 言語横断の関数・メソッド定義
	static const NodeTypeSet FunctionNameIdentifier; // 関数名として現れる識別子葉 (constant / identifier / simple_identifier) — Lint の関数名判定
	static const NodeTypeSet GeneratorStarHost; // JS/TS の前置 `*` を持つジェネレーター関数
	static const NodeTypeSet GoCaseClause; // Go の switch / select の節 (communication_case / default_case / expression_case / type_case)
	static const NodeTypeSet GoConversionType; // 括弧無で型の変換に置ける Go の型（type_identifier / slice_type / struct_type 等） — 変換の型を包む括弧の除去
	static const NodeTypeSet GoGroupedDecl; // Go の丸括弧付一括宣言（括弧内を改行）
	static const NodeTypeSet GoMemberList; // Go の構造体・インタフェースの要素の並び (field_declaration_list / interface_type)
	static const NodeTypeSet GoStatementBoundary; // 改行で文・宣言を区切れる Go の並び（block / source_file / var_spec_list 等）
	static const NodeTypeSet GoSwitch; // Go の switch 文 (expression_switch_statement / type_switch_statement)
	static const NodeTypeSet GoType; // Go の型宣言ノード
	static const NodeTypeSet GoTypeName; // 型名の形の Go の型 (type_identifier / qualified_type / generic_type) — 見出の複合リテラルを包む括弧の保持
	static const NodeTypeSet GreedyTailExpression; // 右端を貪欲に伸ばすか，呼出時だけ括弧を要する式
	static const NodeTypeSet GroupingParen; // グルーピング括弧ノード (parenthesized_expression / parenthesized_statements / tuple_expression) — 単一被演算子の括弧除去対象
	static const NodeTypeSet GroupingTypeParen; // 型を包む括弧の節点 (parenthesized_type / tuple_type) — 型を包む冗長な括弧の除去
	static const NodeTypeSet HashSplat; // Ruby `**hash` 系ノード (hash_splat_parameter / hash_splat_argument / hash_splat_pattern)
	static const NodeTypeSet HtmlBlockTag; // HTML の行を区切る要素の小文字のタグ名（既定の表示形式が block/list-item／表の部品）— 前後と内側の端の空白を描画しない
	static const NodeTypeSet HtmlDefinitionTag; // HTML の定義の一覧の項目の小文字のタグ名 (dd / dt) — 互いの開始タグで暗黙に閉じる
	static const NodeTypeSet HtmlElementWrap; // HTML 要素ラッパ (element / script_element / style_element)
	static const NodeTypeSet HtmlForeignTextTag; // SVG の文字・MathML の字句の小文字のタグ名 — 外来要素の内側で空白を描画する
	static const NodeTypeSet HtmlHiddenTag; // HTML の箱を作らない要素の小文字のタグ名（既定の表示形式が none）— 前後の空白の扱いは更に隣の子で決まる
	static const NodeTypeSet HtmlKnownTag; // tree-sitter-html が名前を種別の１バイトで持つ要素の小文字のタグ名 — 開いた要素の直列化の大きさの見積
	static const NodeTypeSet HtmlObsoleteVoidTag; // 構文解析器が空要素として読む廃止済の HTML の要素の小文字のタグ名（basefont / frame 等） — ネストの深さの数え方
	static const NodeTypeSet HtmlOptionalEndTag; // 閉じタグを省ける HTML の要素の小文字のタグ名（li / p / td 等） — 暗黙に閉じる要素
	static const NodeTypeSet HtmlParagraphCloser; // HTML の段落を暗黙に閉じる開始タグの小文字のタグ名（div / h1 / ol 等） — 構文解析器の段落の閉じ方
	static const NodeTypeSet HtmlPreformattedTag; // HTML の空白を描画する要素の小文字のタグ名 (listing / pre) — 内容の空白を保ち内側のタグだけを整える
	static const NodeTypeSet HtmlRawContentTag; // HTML で内容全体を文字として保つタグ
	static const NodeTypeSet HtmlRawTextElement; // HTML の埋込 raw_text を持つ要素 (script_element / style_element) — Formatter の部分解析対象
	static const NodeTypeSet HtmlRawTextTag; // 内容を生の本文として読む HTML の要素の小文字のタグ名 (script / style) — ネストの深さの数え方
	static const NodeTypeSet HtmlRubyTag; // HTML のルビの部品の小文字のタグ名 (rb / rp / rt) — 互いの開始タグで暗黙に閉じる
	static const NodeTypeSet HtmlTableCellTag; // HTML の表のセルの小文字のタグ名 (td / th) — 互いと行の開始タグで暗黙に閉じる
	static const NodeTypeSet HtmlTag; // HTML タグ系ノード
	static const NodeTypeSet HtmlVoidTag; // HTML の空要素の小文字のタグ名（br / img / input 等） — 内容と閉じタグを持たない
	static const NodeTypeSet IdentifierLeafLike; // 葉識別子相当（`identifier` / `simple_identifier`）— `Lint::CheckShadowedVariable` 等で識別子参照を言語横断判定
	static const NodeTypeSet IfNode; // if 系のノード
	static const NodeTypeSet ImportLike; // import / using / use 系（preproc_include は別構造種別の為，含めない）
	static const NodeTypeSet IndentBlock; // インデントブロック（IndentContainer ∪ RubyBlock ∪ JSX 要素の統合済集合）
	static const NodeTypeSet IndentContainer; // インデントを付与するコンテナノード
	static const NodeTypeSet InfixOp; // 二項演算子・代入・アロー関数等，両側スペースを入れる
	static const NodeTypeSet InitHeader; // 初期化の文を持つ見出
	static const NodeTypeSet IntLiteralLike; // 整数リテラル葉 — `Lint::CheckCmpBoundary` の境界値判定（`NumberLiteral` より狭い）
	static const NodeTypeSet IterationHeader; // 反復見出（対象前の `in`/`as` は演算子外）
	static const NodeTypeSet JavaEmptyArgumentHost; // 空の実引数を省ける Java の節点 (annotation / enum_constant) — 省ける括弧の除去
	static const NodeTypeSet JavaEscapeHost; // Java の Unicode エスケープを中身として包む字句 (block_comment / character_literal / line_comment / string_literal)
	static const NodeTypeSet JavaNonConstantExpression; // 定数式に為れない Java の式 — final 付与の定数判定
	static const NodeTypeSet JavaParameterScope; // Java の仮引数スコープを開く節点
	static const NodeTypeSet JavaStatementHost; // Java で switch を文位置に置く親
	static const NodeTypeSet JavaTypeBody; // Java の型の本体 (annotation_type_body / class_body / enum_body / interface_body)
	static const NodeTypeSet JsBindingName; // JS/TS の束縛の名前 (identifier / shorthand_property_identifier_pattern)
	static const NodeTypeSet JsDefaultDeclaration; // `export default` の後で宣言と読まれる JS/TS の式 (class / function_expression / generator_function)
	static const NodeTypeSet JsInRestoring; // JS/TS の `in` を二項演算子に戻す（for 文の初期化子の外に為る）式（arguments / 計算の鍵 / 欄 / 対 / 括弧 / 塊 / 添字 / 補間）
	static const NodeTypeSet JsJoiningModifier; // 改行の後の成員へ掛かる JS/TS のクラスの修飾語 (get / set / static) — 解析器が名前の欄と読む物を繋ぐ
	static const NodeTypeSet JsMemberTarget; // JS/TS のメンバ・添字の参照 (member_expression / subscript_expression) — 代入で変数を再束縛しない
	static const NodeTypeSet JsModuleStatement; // JS/TS のモジュールの印の文 (import_statement / export_statement)
	static const NodeTypeSet JsRegexPrefixWord; // 後の `/` を正規表現の始まりにする JS/TS の語（return / typeof 等） — コメントの並びの見送りの字句の読み
	static const NodeTypeSet JsSeparatingModifier; // 改行で成員を区切る JS/TS のクラスの修飾語 (abstract / accessor) — 解析器が後続の修飾語と読む物を区切る
	static const NodeTypeSet JsStatementBlockOrBlock; // JS/TS の statement_block + Java/Go/Rust 等の block — 単独コメント `{ }` のブロック親判定
	static const NodeTypeSet JsStatementSequence; // 文・成員を並べる JS/TS の節点 (program / statement_block / class_body / switch_case) — JSX から退避するコメントの置き場
	static const NodeTypeSet JsTsAsiTarget; // JS/TS で末尾 `;` 補完対象となる文ノード
	static const NodeTypeSet JsTsClassMemberAsi; // JS/TS クラス本体で末尾セミコロンを補う成員
	static const NodeTypeSet JsVarDeclKeyword; // JS/TS の変数宣言キーワードトークン (const / let / var) — DeclEdit の型接頭辞判定
	static const NodeTypeSet JsxContainer; // 子要素を保持する JSX コンテナ（jsx_element，断片 `<>` も同じ節点）HTML 要素は別 (HtmlElementWrap)
	static const NodeTypeSet JsxElement; // JSX / HTML 要素
	static const NodeTypeSet JsxHtmlTagOpenClose; // JSX/HTML の開閉タグ全種 (jsx_opening_element / jsx_closing_element / start_tag / end_tag) — JsxContentCount で内容から除外
	static const NodeTypeSet JsxOrHtmlCloseTag; // JSX/HTML の閉じタグ系（end_tag / jsx_closing_element，断片 `</>` も jsx_closing_element）
	static const NodeTypeSet JsxOrHtmlOpenLike; // JSX/HTML の開きタグ + 自己閉鎖 (jsx_opening_element / jsx_self_closing_element / start_tag / self_closing_tag)
	static const NodeTypeSet JsxOrHtmlOpenTag; // JSX/HTML の開きタグ系（jsx_opening_element / start_tag，断片 `<>` も jsx_opening_element）
	static const NodeTypeSet JsxOrHtmlTagAny; // JSX タグ全種 (jsx_opening_element / jsx_closing_element / jsx_self_closing_element) — jsx_text 越境境界のアンカー
	static const NodeTypeSet JsxSpan; // コメントの挿入の JSX の文脈の索引に載せる節点（`{…}`・タグ・要素）
	static const NodeTypeSet JsxTag; // JSX タグ
	static const NodeTypeSet JsxTextLike; // JSX の本文 (html_character_reference / jsx_text) — 連続する子文字列
	static const NodeTypeSet KeywordJump; // 語で始まる脱出の文 (control_transfer_statement / jump_expression)
	static const NodeTypeSet KeywordUnary; // 語や記号で前置する単項の式（キャスト／`co_await` / `delete`／PHP の `@`）— 演算子の優先順位は前置単項
	static const NodeTypeSet KotlinAccessor; // Kotlin のアクセサ (getter / setter) — 直前のプロパティの続きとして空行を置かない
	static const NodeTypeSet KotlinAnnotationLike; // Kotlin 注釈系親 (annotation / file_annotation / use_site_target) — 全トークン密着
	static const NodeTypeSet KotlinAssignableWrapper; // 代入先を包む Kotlin の節点 (directly_assignable_expression / parenthesized_expression)
	static const NodeTypeSet KotlinBraceBodyHost; // 本体の波括弧を直接の子に持つ Kotlin の構文 (anonymous_initializer / secondary_constructor / try_expression)
	static const NodeTypeSet KotlinBranchBody; // 枝を持つ Kotlin の式の枝の本体の包み (catch_block / control_structure_body) — 文の値が枝の値に為る
	static const NodeTypeSet KotlinClassMember; // Kotlin の型の本体の成員（宣言・コンパニオン・初期化子・副構築子・列挙子）— 解析器が `ERROR` に包んだ成員の判定
	static const NodeTypeSet KotlinFunctionHead; // Kotlin の関数の見出の字句 (fun / modifiers) — 型仮引数の `<` の前に空白
	static const NodeTypeSet KotlinImportLeaf; // Kotlin の `import` 表現の葉ノード（`identifier` / `import_header` / `import_list`）— `Lang::Kotlin` 限定文脈で語彙判別
	static const NodeTypeSet KotlinJointSuffix; // 改行を跨いで前の式へ繋がる Kotlin の接尾辞・参照・中置式・返戻値
	static const NodeTypeSet KotlinNegatableTest; // `!` を前置して否定する Kotlin の検査 (check_expression / range_test / type_test)
	static const NodeTypeSet KotlinNonExpression; // 式の位置に置けない Kotlin の文 (assignment / for_statement / while_statement)
	static const NodeTypeSet KotlinNotIsIn; // Kotlin `!is` `!in` 演算子の右側キーワード (in / is)
	static const NodeTypeSet KotlinNumericLiteralSuffix; // Kotlin の数値リテラル接尾子付 (long_literal / real_literal / unsigned_literal) — 数値部と接尾子の密着必須
	static const NodeTypeSet KotlinParameterHost; // Kotlin で既定値の `=` を持つ仮引数親
	static const NodeTypeSet KotlinReturnType; // Kotlin 関数の戻値型ノード (dynamic_type / function_type / nullable_type / parenthesized_type / user_type)
	static const NodeTypeSet KotlinStatementContext; // 改行が文を終える Kotlin の文脈 (class_body / control_structure_body / function_body / source_file / statements)
	static const NodeTypeSet KotlinTypeOperatorHost; // 型の被演算子を取る Kotlin の型 (nullable_type / receiver_type) — 型を包む括弧の保持
	static const NodeTypeSet KotlinValueBranch; // 枝の最後の文の値を結果にする Kotlin の式 (if_expression / when_expression)
	static const NodeTypeSet LabelNode; // 文に付くラベルと Kotlin の注釈
	static const NodeTypeSet LabeledStatement; // ラベル付の文 (labeled_statement / named_label_statement)
	static const NodeTypeSet LayoutCloserToken; // 行頭で親深度に揃える閉じトークン — `Layout::CollectTokenIndents` の `IsCloser` 判定
	static const NodeTypeSet LeadingTypeArguments; // 先頭の型引数を後ろの子へ密着させる節点 (method_invocation / type_assertion)
	static const NodeTypeSet JumpWithLabel; // ラベルを跳び先として持つ脱出の式（中のラベルは定義でなく参照）
	static const NodeTypeSet Leaf; // 葉ノード（中身を再構築せず原文を其のまま使う）
	static const NodeTypeSet Loop; // 全言語のループ構造ノード（for/while/do/range/foreach 系等を全列挙）
	static const NodeTypeSet LoopExitStatement; // 繰返を抜ける文 (break_statement / continue_statement)
	static const NodeTypeSet LoopForOrWhile; // for_statement + while_statement — TsSource MatchStep の同位置入替対応
	static const NodeTypeSet LoopOrSwitch; // break_statement の境界となる構造（Loop ∪ switch/Go expression-switch/type-switch/select）
	static const NodeTypeSet MultilineCommaContainer; // 実引数・仮引数と Go の複数行コンテナ
	static const NodeTypeSet MacroDefinition; // マクロの定義 (preproc_def / preproc_function_def)
	static const NodeTypeSet MacroLike; // マクロ呼出風ノード
	static const NodeTypeSet Member; // メンバアクセス連鎖（dot / scope の記法）
	static const NodeTypeSet MemberFieldRank; // C#/PHP のプロパティ・定数・イベント欄（アクセス順位２）
	static const NodeTypeSet MemberMethodRank; // 成員関数・メソッド・コンストラクター（アクセス順位３）
	static const NodeTypeSet MemberTypeRank; // 型別名とネスト型宣言（アクセス順位１）
	static const NodeTypeSet MisparsedTemplateHost; // 山括弧の誤解析で型引数リストへ化けた比較式
	static const NodeTypeSet MutatingExpression; // 変数を書き換える式 (assignment_expression / update_expression)
	static const NodeTypeSet NamedParameterKind; // 単一名を持つ仮引数ノード — `Lint::FindParameterEvent` の対象種別
	static const NodeTypeSet NamespaceLike; // C++ / TS の名前空間本体を改行する対象
	static const NodeTypeSet NewParenCall; // C# `new()` 形の丸括弧呼出文脈 (constructor_constraint / implicit_object_creation_expression)
	static const NodeTypeSet NoInnerSpaceEdit; // 内部スペース正規化スキップ
	static const NodeTypeSet NonVerbatimLeaf; // 内容が逐語でない葉（型・名前・要素の構造）— 行末空白の保護対象外
	static const NodeTypeSet NormColon; // `:` 周辺空白を `: ` 形に正規化するノード
	static const NodeTypeSet NormEq; // `=` 周辺空白を１つに正規化するノード
	static const NodeTypeSet NumberLiteral; // 数値リテラル系ノード
	static const NodeTypeSet ObjectLike; // オブジェクトリテラル系ノード（{} 内側スペース挿入対象）
	static const NodeTypeSet OpenPrefixExpression; // 語で始まり右の被演算子を式の終わり迄取る式（`yield` / `include` / `print`／ラムダ／`return` 等）— 演算子の優先順位は最も弱い
	static const NodeTypeSet OpenTailToken; // 宣言へ密着する記号及び Ruby で改行を式の継続と読ませる文末字句 (& / * / ** / .. / ... / :)
	static const NodeTypeSet OptionalMarker; // 直前トークンへ密着する省略可能性記号 (optional_chain / optional_type / quest) — Structure NeedsGapBetween
	static const NodeTypeSet OptionalParenHost; // 言語が省く事を許す括弧の並びを子に持つ節点（arrow_function / lambda_expression / call_suffix 等） — 省ける括弧の除去
	static const NodeTypeSet OptionalTypeMember; // `?` と型注釈を同行に保つ宣言
	static const NodeTypeSet ParamContainerLike; // 文書コメント解析対象の仮引数列
	static const NodeTypeSet ParameterContainer; // 関数パラメータコンテナ (formal_parameters / function_value_parameters / parameter_list / parameters) 言語横断
	static const NodeTypeSet ParenInnerFirstChildNoUnwrap; // 内側の最左の子孫が此の型なら括弧を剥がさない（Rb の多重代入の左辺・Py の条件式を値に取るセイウチ演算子）
	static const NodeTypeSet ParenInnerNoUnwrap; // 即時実行・コンマ式・貪欲式等，丸括弧を外せない式
	static const NodeTypeSet ParenKeeper; // 括弧維持対象ノード（直接一致・Control・Member の和集合）
	static const NodeTypeSet ParenLeadType; // 丸括弧で始まる型（前置語との空白判定）
	static const NodeTypeSet ParenOptionalHeader; // 見出の条件を括弧で包まない言語 (Go / Rust / Swift / Python / Ruby) の制御構文 — 条件を包む括弧は冗長
	static const NodeTypeSet ParenValueSlot; // 括弧が値の全体を包み後に字句が続かない値の位置（代入の右辺・実引数・戻値・要素・仮引数の既定値）
	static const NodeTypeSet PatternKind; // 変数束縛パターン (pattern / tuple_pattern) — Structure NeedsGapBetween で制御キーワード直後の括弧前スペース判定
	static const NodeTypeSet PhpAltCloser; // PHP の代替構文の終端語 (enddeclare / endfor / endforeach / endif / endswitch / endwhile)
	static const NodeTypeSet PhpAltHost; // PHP の代替構文の本体を開く `:` を直接の子に持つノード (colon_block / declare_statement / for_statement / switch_block)
	static const NodeTypeSet PhpAltLeadingColon; // 本体を開く `:` を先頭の字句に持つ PHP の代替構文の本体 (colon_block / switch_block)
	static const NodeTypeSet PhpCompleteToken; // 閉じタグの前で終端を要さない PHP の字句（`;` / 本体の `{` `}` / 見出の `:`）
	static const NodeTypeSet PhpIfBranch; // PHP の if の後続の枝の節 (else_clause / else_if_clause)
	static const NodeTypeSet PhpInlineHtml; // PHP の地の文ノード (text / text_interpolation) — 出力其の物の為，インデント・行末空白を含め逐語で保つ
	static const NodeTypeSet PhpNamespaceUse; // PHP の名前空間取込ノード (namespace_use_declaration / namespace_use_group) — `\` と群括弧を名前へ密着
	static const NodeTypeSet PhpOpenTagEnd; // PHP の開始タグで終わる子 (php_tag / text_interpolation) — 後続を開始タグと同じ行に置く判定
	static const NodeTypeSet PhpReturnType; // PHP 関数の戻値型ノード (intersection_type / named_type / optional_type / primitive_type / union_type)
	static const NodeTypeSet PhpSemicolonStatement; // 式等で終わり常に `;` で閉じる PHP の文 — 閉じタグの直前で `;` を省いたかを末尾の子で見る
	static const NodeTypeSet PhpUnsplittable; // 内側で改行出来ない PHP の節点 (cast_expression / visibility_modifier)
	static const NodeTypeSet PipeParamList; // `|` で囲まれる仮引数列 (block_parameters / closure_parameters) — Structure NeedsGapBetween の密着判定
	static const NodeTypeSet PlainVariable; // 型を構文から確定出来ない単純な変数 (identifier / variable_name)
	static const NodeTypeSet PointerDecl; // ポインタ宣言子
	static const NodeTypeSet PointerOrRefDeclarator; // C/C++ ポインタ／参照宣言子 (pointer_declarator / reference_declarator) — 戻値型のポインタ／参照判定
	static const NodeTypeSet PostfixOrUpdateExpression; // 後置単項演算子及び前置・後置共通の更新式
	static const NodeTypeSet PostfixCallHost; // 被呼出側を最初の子に持つ呼出 (call_expression / function_call_expression) — 被呼出側を包む括弧の保持
	static const NodeTypeSet PostfixReceiverChain; // 受け手（最初の子）を持つ後置の連鎖 — 受け手を包む括弧の除去で最初の子を辿る
	static const NodeTypeSet PostfixReceiverHost; // 受け手を最初の子に持つ後置の式（メンバ参照・呼出・添字）— 受け手を包む括弧の除去
	static const NodeTypeSet PostfixReceiverLeaf; // 括弧無で後置の式の受け手に置ける名前・字句で閉じるリテラル — 後置の連鎖の最初の子の終点
	static const NodeTypeSet Preproc; // C/C++/C# プリプロセッサノード（preproc_* を全列挙）
	static const NodeTypeSet PreprocBlock; // 条件分岐型プリプロセッサノード (#if / #ifdef / #ifndef / #elif / #elifdef / #else)
	static const NodeTypeSet PreprocConditionWord; // 条件を開く前処理指令の名前 (if / ifdef / ifndef)
	static const NodeTypeSet PreprocConditional; // プリプロセッサ条件の根 (preproc_if / preproc_ifdef) — elif/else を含まない根限定集合
	static const NodeTypeSet PreprocDirectiveClose; // プリプロセッサ指令の閉じ系 (#else / #endif) — 単独指令で常に空行区切
	static const NodeTypeSet PreprocDirectiveOpen; // プリプロセッサ指令の開き／分岐系 (#elif / #elifdef / #elifndef / #if / #ifdef / #ifndef) — 後続の条件式と連結対象
	static const NodeTypeSet PreprocFlat; // 行分割で平坦に再帰するプリプロセッサ節点
	static const NodeTypeSet PreprocNameQuery; // 条件で名前がマクロかを問う前処理の節点 (preproc_defined / preproc_elifdef / preproc_ifdef)
	static const NodeTypeSet PyDefLike; // Python の定義文 (function_definition / class_definition / decorated_definition) — 空行区切の構造種別判定で実行文と区別する
	static const NodeTypeSet PySplittableExpr; // Python の演算子式 (binary_operator / boolean_operator / comparison_operator) — Layout の演算子継続行インデント揃え用
	static const NodeTypeSet PythonAsClause; // 値を `as` で束縛する Python の節 (except_clause / with_item)
	static const NodeTypeSet PythonBareList; // Python の括弧の無い組 (expression_list / pattern_list) — 末尾のコンマの除去
	static const NodeTypeSet PythonBareTupleHost; // 組を括弧無で置ける Python の文（return_statement / yield / delete_statement 等） — 組の括弧の除去
	static const NodeTypeSet PythonConditionHost; // 条件を持つ Python の文 (elif_clause / if_statement / while_statement)
	static const NodeTypeSet PythonNumber; // Python の数値リテラル (float / integer) — `.` の前の括弧が必須
	static const NodeTypeSet PythonParenOnlyElement; // 括弧の無い組に置けない Python の要素 (named_expression / yield) — 組の括弧の保持
	static const NodeTypeSet PythonPostfix; // Python の後置の式 (attribute / call) — 最高位の優先順位
	static const NodeTypeSet PythonReturnValueHost; // 値を返す Python の文・式 (return_statement / yield) — 星付の要素の組の括弧の保持
	static const NodeTypeSet PythonRightValueHost; // 右辺を持つ Python の文 (assignment / augmented_assignment / for_statement)
	static const NodeTypeSet PythonTargetHost; // 対象と値の場を持つ Python の文・節 (assignment / augmented_assignment / for_statement / for_in_clause) — 組の括弧の除去
	static const NodeTypeSet PythonUnwrappedChild; // 包まない Python の子 (assignment / augmented_assignment / block)
	static const NodeTypeSet PythonValueRoot; // 括弧外で折り返す為に包む Python の値式の根（PythonWrappable と呼出・添字・単項・await）
	static const NodeTypeSet PythonValueStatement; // 値式を持つ Python の文（return_statement / expression_statement / yield 等）
	static const NodeTypeSet PythonWrapOpaque; // 包まない Python の範囲 (lambda_parameters / type)
	static const NodeTypeSet PythonWrappable; // 括弧外で折り返す為に包む Python の値式（演算・属性・ラムダ・並び等）
	static const NodeTypeSet RangeLike; // 範囲式・範囲パターン (range_expression / range_pattern) — Structure NeedsGapBetween の密着判定
	static const NodeTypeSet RangeOperator; // 範囲の演算子 (.. / ...)
	static const NodeTypeSet RangeOrUnaryExpression; // Kotlin の増減式及び Swift の範囲／単項演算子親
	static const NodeTypeSet ReturnOrDocFunctionLike; // 明示 return・文書コメント・return 直前コメント検査の関数様ノード
	static const NodeTypeSet ReturnOrThrow; // 関数の制御終端文（`return_statement` / `throw_statement`）— `Lint::TerminatesAlways` 判定
	static const NodeTypeSet ReturnStatement; // 値を返す文 (co_return_statement / return_statement) — 値の全体を包むマクロの括弧を外せる
	static const NodeTypeSet ReturnTuple; // 戻値が組型のノード
	static const NodeTypeSet ReturnValueHost; // return の返す値の参照の有無を決める関数（C++ の decltype(auto) / PHP の参照返し）
	static const NodeTypeSet ReturnValueParent; // 返す値を直接の子に持つ節点 (arrow_function / return_statement)
	static const NodeTypeSet RubyAnonymousForward; // 名前を略して転送出来る Ruby の実引数 (block_argument / hash_splat_argument / splat_argument)
	static const NodeTypeSet RubyArgumentExtent; // 命令形の呼出の実引数が左端から続く Ruby の式 (binary / call / conditional / element_reference / range)
	static const NodeTypeSet RubyArgumentKeyword; // 空白に依らず後続を実引数と読む Ruby の語 (break / next / return)
	static const NodeTypeSet RubyArgumentListHost; // 実引数の並びを子に持つ Ruby の節点 (break / call / next / return / yield)
	static const NodeTypeSet RubyArgumentSequence; // Ruby の要素の並び (argument_list / right_assignment_list) — 名前の後の実引数が残りの要素を取り込む
	static const NodeTypeSet RubyArgumentStart; // 名前の後に空白を置いて続けると実引数の始まりに為る Ruby の字句 (& * ** + - << [)
	static const NodeTypeSet RubyArrayLiteral; // Ruby `%w[...]` / `%i[...]` 配列リテラル親 (string_array / symbol_array)
	static const NodeTypeSet RubyBareList; // 括弧を省ける Ruby の並び (argument_list / method_parameters) — 文末の `;` を改行へ置き換える前に括弧で閉じる
	static const NodeTypeSet RubyBinding; // 修飾子化で束縛と参照の順序が変わる Ruby 式
	static const NodeTypeSet RubyBlock; // Ruby 制御ブロック（end 区切り）
	static const NodeTypeSet RubyBodyContainer; // 本体直前を改行し複数行化する Ruby コンテナ（case_match を除く）
	static const NodeTypeSet RubyBodyTailClause; // Ruby body_statement 末尾節 (rescue / ensure) — メソッド本体の暗黙 return 対象から除外
	static const NodeTypeSet RubyBodyTrigger; // Ruby 節本体の開始キーワード現在側 (else / elsif / then) — Structure GetStructuredText の IsBody 判定
	static const NodeTypeSet RubyBraceBlock; // １文なら１行に置く Ruby の波括弧ブロックと本体
	static const NodeTypeSet RubyBracketIndentBlock; // Ruby IndentBlock の括弧付限定対象 (argument_list / array / hash / method_parameters)
	static const NodeTypeSet RubyBranch; // Ruby の分岐 (case / if / unless) — 構造化が節の本体を１行に畳み得る
	static const NodeTypeSet RubyBranchClause; // Ruby の分岐の節 (else / elsif / in_clause / then / when) — 節の語と本体を子に持つ
	static const NodeTypeSet RubyChainedStringLike; // Ruby 連鎖文字列の分裂残骸種別 (heredoc_beginning / string) — 同型連続時は暗黙 return 付与の対象外
	static const NodeTypeSet RubyClassDefLike; // Ruby class 定義系 (class / singleton_class) — Structure GetStructuredText で `< 親` 継承記法のスペース調整対象
	static const NodeTypeSet RubyClassLike; // Ruby の `class` 系（`begin` / `class` / `module`）— tree-sitter Ruby の `class X end` の `ERROR` 回避対象
	static const NodeTypeSet RubyClauseWord; // Ruby で式を始められない節・終端語
	static const NodeTypeSet RubyCommandArgumentHost; // 命令形の呼出の実引数の読みを揃える Ruby の節点 (binary / break / call / element_reference / next / range / return / yield)
	static const NodeTypeSet RubyCondOrRescue; // Ruby 条件分岐終端節キーワード + 例外節 (elsif / rescue / when) — Structure GetStructuredText で本体の多行判定対象
	static const NodeTypeSet RubyCondTerminator; // Ruby 条件分岐終端節キーワード (elsif / when) — Structure 平坦化判定で本体改行注入対象
	static const NodeTypeSet RubyContinuation; // Ruby 継続行のインデント１段深化対象ノード（call/binary/conditional/assignment／各種 modifier/argument_list）
	static const NodeTypeSet RubyDelimitedLiteral; // Ruby の区切り文字付リテラル (delimited_symbol / regex / subshell) — 括弧の深さ計算で内部へ降りない
	static const NodeTypeSet RubyElseEnsure; // Ruby else / ensure 節キーワード (else / ensure) — Structure GetStructuredText で本体扱いの IsBody 判定
	static const NodeTypeSet RubyExpressionLink; // Ruby の式の接続を成す節点（`binary` / `call` / `element_reference` / `unary`）— `Structure::RubyExpressionShape` の構造化前後の照合
	static const NodeTypeSet RubyFlowExpr; // Ruby の脱出式 (break / next / redo / retry / return / yield) — 範囲式の始端に来ると `return` 前置が破綻する
	static const NodeTypeSet RubyFreshScope; // 外の局所変数を見ない新しいスコープを開く Ruby の節点 (class / method / module / program / singleton_class / singleton_method)
	static const NodeTypeSet RubyHeadedBody; // 見出子後に本体を包み，空本体のコメント錨を定める Ruby 構文
	static const NodeTypeSet RubyKeywordBlock; // キーワードで開き `end` で閉じる Ruby の式（if / while / case / begin 等）
	static const NodeTypeSet RubyLocalScope; // 局所変数のスコープを開く Ruby の節点（RubyFreshScope ∪ block / do_block / lambda，ブロックは外の局所変数も見る）
	static const NodeTypeSet RubyLoggerMethod; // Ruby ロガー系メソッド名 (debug / error / fatal / info / warn) — 受信子がロガー末尾の時に副作用判定
	static const NodeTypeSet RubyLowBinary; // 命令形の呼出の実引数の外に在る Ruby の語の二項演算子 (and / or)
	static const NodeTypeSet RubyLowUnary; // 代入より弱い Ruby の単項の語 (defined? / not)
	static const NodeTypeSet RubyMethodDef; // Ruby メソッド定義系 (method / singleton_method) — Structure GetStructuredText の本体配置分岐と Edit の暗黙 return 走査起点
	static const NodeTypeSet RubyModifierExpr; // Ruby の修飾子形の式（`x if c` 等）— 外側からの畳込と１行化の対象外
	static const NodeTypeSet RubyModifierForm; // 修飾子形式へ畳める Ruby の分岐・繰返 (if / unless / until / while) — Edit の修飾子形式変換
	static const NodeTypeSet RubyNumericLiteralSuffix; // Ruby rational/complex リテラル親 (complex / rational) — 数値部と接尾子 r/i/ri の密着必須
	static const NodeTypeSet RubyParameters; // Ruby の仮引数の並び (block_parameters / lambda_parameters / method_parameters) — 局所変数を導入する
	static const NodeTypeSet RubyPatternTest; // Ruby のパターン照合の式 (match_pattern / test_pattern) — 式の中で最も弱い
	static const NodeTypeSet RubyReturnSafe; // Ruby メソッド末尾へ return を安全に付けられる単純式
	static const NodeTypeSet RubySemicolonKeep; // `;` を改行へ置き換えない Ruby の親 (block_parameters / parenthesized_statements)
	static const NodeTypeSet RubySideEffectMethod; // Ruby 副作用専用メソッド名 (p / pp / print / puts) — 自動 return 付与対象から除外
	static const NodeTypeSet RubySpacedBinary; // 名前の後の空白の後に密着すると実引数・文字列・正規表現の始まりに為る Ruby の二項の演算子 (% & * ** + - / <<)
	static const NodeTypeSet RubyStatementHost; // Ruby の文が直接置かれるコンテナ（program / then / do / body_statement 等）— 値位置との判別
	static const NodeTypeSet RubyStatementSequence; // Ruby の文を並べる節点（parenthesized_statements / then / block_body 等）— 平坦化で文を `;` で区切る
	static const NodeTypeSet RubyTerminatorHost; // 文・見出を終える改行を直下に持つ Ruby の節点（then / do_block / if / def 等）— 括弧内でも直下の改行を畳まない
	static const NodeTypeSet RubyThenElse; // Ruby の条件分岐の本体の節 (else / then)
	static const NodeTypeSet RustAssignmentLike; // Rust 代入式系 (assignment_expression / compound_assignment_expr) — mut マーク検出対象
	static const NodeTypeSet RustBaseTraversal; // Rust の mut 削除候補から基底名を辿る節点
	static const NodeTypeSet RustBlockLike; // 文頭に置くと `}` で文を終える Rust の塊の式（block / unsafe_block / if_expression / loop_expression 等）
	static const NodeTypeSet RustLetBinding; // Rust の let の束縛 (let_condition / let_declaration)
	static const NodeTypeSet RustMacroTokens; // Rust のマクロが受け取る字句列 (macro_definition / token_tree) — 字句の削除対象外
	static const NodeTypeSet RustRefPattern; // Rust の参照・フィールドのパターン (field_pattern / ref_pattern)
	static const NodeTypeSet RustStatementHead; // 文頭の式を持つ Rust の節点 (block / expression_statement)
	static const NodeTypeSet RustTypeOperatorHost; // 型の被演算子を取る Rust の型（reference_type / pointer_type / bounded_type 等） — 型を包む括弧の保持
	static const NodeTypeSet RustValueJump; // 値を取り得る Rust の脱出の式 (break_expression / return_expression / yield_expression)
	static const NodeTypeSet ScopeBody; // 言語固有の範囲本体
	static const NodeTypeSet ScopeContainer; // 関数・メソッド・プログラムの本体スコープ
	static const NodeTypeSet ScssMixinRule; // ミックスインの定義と取込の SCSS の規則（`include_statement` / `mixin_statement`）— 空の括弧の除去
	static const NodeTypeSet ScssParameterHost; // 仮引数の並びを持つ SCSS の規則（`function_statement` / `mixin_statement`）— 空の並びの `MISSING` の許容
	static const NodeTypeSet SectionHeader; // セクション見出（case/default/label／アクセス修飾子等）
	static const NodeTypeSet SectionParent; // セクション親ノード（内側に SectionHeader を含むコンテナ）
	static const NodeTypeSet SeparatorToken; // 区切りの字句 (, / : / ;) — 中置演算子として空白で囲まない
	static const NodeTypeSet SequentialStmtContainer; // 逐次実行される文の直接コンテナ（`FunctionBodyContainer` ∪ `statements` の統合済集合）— `Lint::TerminatesAlways`
	static const NodeTypeSet SimpleTypeLeaf; // 単純型葉ノード — Lint の型注釈及び仮引数型の判定
	static const NodeTypeSet SingleIndented; // 単一インデントを必要とするノード（if/for/while 等の単文本体対象）
	static const NodeTypeSet SizeofParenArgument; // 括弧で始まり `sizeof` の実引数を包み直さない節点（誤読されたキャスト式 / 括弧式）
	static const NodeTypeSet Skip; // スペース正規化スキップノード（リテラル／コメント／特殊構文等）
	static const NodeTypeSet SkipButRecurseForLayout; // Skip 集合内でインデント走査時のみ子へ再帰する例外（pair/keyed_element／各種 parameter／添字の範囲）
	static const NodeTypeSet SkipGapEdit; // 子間ギャップ編集をスキップするノード
	static const NodeTypeSet SpaceKeyword; // スペース付与キーワード
	static const NodeTypeSet SpacelessBrace; // `{` 前後の空白を許容しないノード
	static const NodeTypeSet SplittableDeclParent; // 統合変数宣言を別宣言へ分割可能な statement-block 親
	static const NodeTypeSet StatementHost; // 文が直接置かれるコンテナ（block / compound_statement / statements 等）— 値位置との判別
	static const NodeTypeSet StatementList; // 文リストノード（Go/Kt 等の括弧付宣言グループや Ruby のメソッド本体）
	static const NodeTypeSet StatementsOnlyBreak; // 文列の前でだけ改行し，見出の子（パターン・仮引数）を同じ行に保つ節 (catch_block / switch_entry)
	static const NodeTypeSet StmtCtxParent; // 文文脈の親 (expression_statement / statements) — 式値を使わない純文の文文脈判定
	static const NodeTypeSet StringContentLeaf; // 文字列の内側テキスト子 (string_content / string_fragment) — 引用符正規化での `"` 含有検査対象
	static const NodeTypeSet StringDelimiter; // 文字列を開閉する引用符の字句（" / ''' / ` 等） — 閉じない文字列の検出
	static const NodeTypeSet StringLikeAll; // 言語横断の全文字列リテラル
	static const NodeTypeSet StringLikeInnerPreserve; // 行末空白除去時に内部バイト範囲を保護する文字列／ヒアドキュメント／コメント系ノード
	static const NodeTypeSet StringPrefix; // 文字列の開きの前の接頭辞（r / b / L / u8 / R / @ / $ 等） — 閉じない文字列の検出
	static const NodeTypeSet StringSeqContainer; // 行分割で平坦に再帰する文字列・記号列
	static const NodeTypeSet StripEq; // `=` 周辺空白を完全に剥がすノード
	static const NodeTypeSet SwiftBraceBodyHost; // 本体の波括弧を直接の子に持つ Swift の制御構文 (do / for / guard / if / repeat-while / while) — 本体の文を展開
	static const NodeTypeSet SwiftHeaderExpressionHost; // 見出の式を直接の子に持つ Swift の文・節 (if / guard / for / switch / while / where_clause) — 見出の式を包む括弧の保持
	static const NodeTypeSet SwiftHeaderLead; // Swift の見出の式の前の字句（if / while / , / = / in 等） — 見出のクロージャの括弧付
	static const NodeTypeSet SwiftHeaderStatement; // 見出の後に本体の波括弧が続く Swift の文 (if / guard / for / switch / while) — 見出のクロージャの括弧付
	static const NodeTypeSet SwiftKeywordNode; // 名前付の節点に為る Swift の語 (value_binding_pattern / binding_pattern_kind / where_keyword) — 後の括弧との間の空白
	static const NodeTypeSet SwiftPostfixCall; // Swift の呼出・添字の後置式 (call_expression / subscript_expression) — 任意連鎖の `?` を密着
	static const NodeTypeSet SwiftStrayBlock; // 見出の文の直後の同じ行の `{` で始まる Swift の塊（`ERROR` / `lambda_literal`）— 見出のクロージャの読違の検出
	static const NodeTypeSet SwiftTypeOperatorHost; // 型の被演算子を取る Swift の型（optional_type / metatype / protocol_composition_type 等） — 型を包む括弧の保持
	static const NodeTypeSet SwitchBody; // switch の本体 (compound_statement / switch_block / switch_body)
	static const NodeTypeSet SwitchGotoTarget; // C# の `goto case` / `goto default` の跳び先を表す字句
	static const NodeTypeSet SwitchCaseAncestor; // switch の節・文 (case_statement / switch_statement) — return のコメントを省ける
	static const NodeTypeSet SwitchStatementLike; // switch に類する文 (switch_statement / when_expression)
	static const NodeTypeSet SwitchOrWithExpression; // switch 式及び C# の with 式
	static const NodeTypeSet TemplateStringLike; // JS/TS テンプレート文字列系 (template_string / template_substitution) — Structure GetStructuredText で内部式の空白正規化対象
	static const NodeTypeSet Ternary; // 三項演算子系 + range-for 系（`?` `:` 周辺で特殊スペース処理対象）
	static const NodeTypeSet ThenDo; // `then` / `do` 等の制御本体トリガ
	static const NodeTypeSet Transparent; // 透過ノード（深さを増やさず子を其のまま展開）
	static const NodeTypeSet TransparentWrap; // AttachComments のアンカー展開対象（FunctionBodyContainer ∪ parenthesized_expression / control_structure_body / statements）
	static const NodeTypeSet TsAssertion; // TS の型の表明 (as_expression / satisfies_expression / type_assertion)
	static const NodeTypeSet TypeCombination; // PHP / TS の型結合ノード
	static const NodeTypeSet TsFunctionType; // 戻値の型の中で条件の型を再び許す TS の関数の型 (constructor_type / function_type)
	static const NodeTypeSet TsGreedyType; // 右端を貪欲に伸ばす TS の型 (conditional_type / constructor_type / function_type / infer_type)
	static const NodeTypeSet TsTypeBody; // TS の型本体 (interface_body / object_type)
	static const NodeTypeSet TsTypeOperatorHost; // 型の被演算子を取る TS の型・式（array_type / union_type / intersection_type 等） — 型を包む括弧の保持
	static const NodeTypeSet TupleLike; // １要素の組 `(x,)` で末尾コンマ必須となる組系 (tuple / tuple_expression / tuple_pattern / tuple_struct_pattern / tuple_type)
	static const NodeTypeSet TypeAnnotationParent; // 型注釈の親として改行後へ置く宣言
	static const NodeTypeSet TypeAnnotationSimpleType; // TS のコロン後で分割出来る単純型（複合型は内側を優先）
	static const NodeTypeSet TypeArgumentLookalikeSlot; // 比較式が型引数列に見え得る一覧要素の位置
	static const NodeTypeSet TypeCastRight; // 型を右に取る変換の式 (as_expression / satisfies_expression / type_cast_expression)
	static const NodeTypeSet TypeColonParent; // `:` 後で分割候補にする鍵値対親 (pair / pair_pattern / property_signature)
	static const NodeTypeSet TypeExpression; // 型表現ノード（戻値型・パラメータ型として現れる具体的な型；修飾子や注釈は含まない）言語横断
	static const NodeTypeSet TypeOperatorInner; // 演算子を持つ型（union_type / function_type / nullable_type / bounded_type 等） — 型の演算子の被演算子を包む括弧の保持
	static const NodeTypeSet TypedBinding; // 型注釈と初期値を直下に持ち，コロン後で割らない宣言
	static const NodeTypeSet UnaryPre; // 前置単項演算子
	static const NodeTypeSet UnaryPreOrUpdate; // 前置単項演算子 + 更新式（UnaryPre ∪ update_expression の統合済集合）— Structure の隣接スペース判定用
	static const NodeTypeSet UnaryPrefixExpression; // 前置の単項式 (prefix_unary_expression / unary_expression) — `x = !x` の検査
	static const NodeTypeSet UnaryPrefixToken; // キャスト風の括弧の後に来る前置単項の記号 (! / + / - / ~)
	static const NodeTypeSet Unformattable; // 整形対象外ノード（内部構造を保持し原文返戻）
	static const NodeTypeSet VariableDeclaration; // 変数宣言ノード（DeclLike + let_declaration/property_declaration 含む拡張版）言語横断
	static const NodeTypeSet VerbatimNode; // 子から組み直さず原文を写す節点（`ERROR` / `shebang_line`）

	NodeKind() = delete; // コンストラクタ（禁止）
};
