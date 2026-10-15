#pragma once

#include "GrammarProfile.hpp"
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

// 文法のノード名の表（整形仕様 4.3，文法の定義だけがノード名を役割・記法へ対応付け，入力側・検証側・中核は役割を読む）
struct GrammarProfile::Names {
	// 整列済集合クラス
	class NameSet {
	private:

		std::vector<std::string_view> Slots; // 要素格納スロット列
		uint64_t FirstCharMask; // 各要素先頭文字（`& 0X3F` の６ビット）を立てた６４ビットマスク
		uint64_t SizeMask; // 各要素の長さ (1..63) を立てた６４ビットマスク，長さ不一致で即 false 判定
		static size_t HashOf(const std::string_view Type); // ハッシュ値算出関数
		void Build(const std::vector<std::string_view> &Items); // 要素列からの構築関数

	public:

		bool HasName(const std::string_view Type) const; // 包含判定関数

		NameSet(const std::initializer_list<std::string_view> Initializer); // コンストラクタ
		NameSet(const NameSet &Base, const std::initializer_list<std::string_view> Extra); // 和集合のコンストラクタ
		NameSet(const NameSet &Base, const NameSet &Added, const std::initializer_list<std::string_view> Extra); // ２つの集合と追加名の和集合のコンストラクタ
	};

	static const NameSet AccessOrderMember; // 公開範囲を持つメンバの宣言
	static const NameSet AmbiguousBodyContainer; // 型の本体とは限らない本体
	static const NameSet AngleBracketList; // `<` `>` で囲まれる型引数／型パラメータリスト（内側スペース判定と平坦化での内部分割優先で共用）
	static const NameSet ArrowToken; // 矢印のトークン (-> / =>) — 前後に空白を置く
	static const NameSet AssignmentExpr; // 言語横断の単純代入と複合代入
	static const NameSet AwaitTryCastExpr; // `await` / `try` / `unsafe`／型キャストの式
	static const NameSet AttachBracket; // `[` を直接隣接付加するノード
	static const NameSet AttachColonOrDot; // `:` 又は `.` を直前トークンに直接隣接付加するノード
	static const NameSet AttachParen; // `(` を直接隣接付加する引数／仮引数リスト
	static const NameSet AttachPrefix; // 後続要素に付く属性・アノテーション・デコレータ
	static const NameSet BinaryExpr; // 言語横断の二項演算又は其の連鎖のノード
	static const NameSet BoolInitCtx; // 真偽値型の初期値を持ち得る宣言・フィールド・仮引数
	static const NameSet BraceAttach; // `{` を型名直後に隣接付加するリテラル
	static const NameSet BraceClause; // 閉波括弧に続く本体付節（間のコメントのアンカーを本体の最初の文へ降ろす）
	static const NameSet BraceNoSpace; // 空の本体を語へ密着させる Go の型定義ノード (interface_type / struct_type)
	static const NameSet BracketType; // `[` 始まりの型ノード (array_type / dictionary_type / slice_type)
	static const NameSet BreakColonHost; // `:` の後を分割候補にする親（三項・キー値の要素・見出の区切，型注釈・継承・ラベル等は含めない）
	static const NameSet BreakKeyword; // 脱出の語 (break / break@) — 語で始まる脱出の文の種別
	static const NameSet BreakOpaque; // 内部を行分割候補にしないノード（Rust マクロのトークンツリー本体／Ruby 隣接文字列連結／Ruby rescue 修飾子）
	static const NameSet CallNode; // 関数・メソッドの呼出（言語横断） — `ControlFlow::IsNoReturnCall`
	static const NameSet CatchClause; // 例外を捕捉する節（言語横断） — `ControlFlow::IsTryTerminating`
	static const NameSet CCastInner; // C/C++ キャスト `(type)` 内側に出現し得る型部位（丸括弧削除抑止対象）
	static const NameSet CCastTypeNameInner; // C/C++ の二項式で括弧内を型名と看做せる単独子
	static const NameSet CDeclSpecifier; // C / C++ の関数定義の宣言子の前に置く指定・属性 — `ControlFlow::IsNoReturnDeclared`
	// C の宣言子を包む宣言子 (attributed_declarator / parenthesized_declarator / pointer_declarator) — 関数定義の宣言子の検査
	static const NameSet CDeclaratorWrapper;
	static const NameSet CFunctionOrFieldDeclarator; // C/C++ の関数宣言子経路及びフィールドのメソッド判定用宣言子
	static const NameSet CNameDecl; // 名前を宣言する C / C++ の宣言・フィールド・関数定義
	static const NameSet ConditionalBranchClause; // if の後続の枝の内で自身の条件を持つ節 (elif_clause / else_if_clause) — 更に後続の枝を要する
	static const NameSet CParenDeclarator; // C の丸括弧で始まり得る宣言子（型との空白判定）
	static const NameSet CTypeAliasDecl; // 型の別名を宣言する C / C++ の宣言 (alias_declaration / type_definition) — 同じファイルで宣言した型名の収集
	static const NameSet CaseBodyOpener; // switch の節の本体を開くトークン（`->`・`:`・`=>`）
	static const NameSet CaseClause; // switch の節（言語横断）（case_statement / switch_section / when_entry / match_arm 等）
	static const NameSet CastLikeExpr; // キャスト式ノード (cast_expression / type_cast_expression)
	static const NameSet ClassBody; // クラス本体ノード（アクセス修飾子の順序検査対象）
	static const NameSet ClassBodyContainer; // クラス／構造体／インターフェース等のメンバを直接保持するコンテナ
	static const NameSet ClassLike; // クラス系コンテナノード（`class` / `struct` / `interface` / `enum` / `module` / `namespace` 等）全言語横断
	static const NameSet ClassLikeStrict; // 公開範囲を持つメンバの本体を所有する型
	static const NameSet ClauseBody; // 最後の子に本体の文を持つ節 (colon_block / else_clause / else_if_clause) — 末尾のコメントを本体の最後の文の後へ置く
	static const NameSet CloserToken; // 閉トークン（括弧・ブロック・要素の閉じと PHP の代替構文の終端語）— コメントのブロックの末尾の判定
	// 構文を閉じる語 (end / enddeclare / endfor / endforeach / endif / endswitch / endwhile)— 見出の行に同居すると本体を置く行が残らない
	static const NameSet CloserWord;
	static const NameSet ColonControl; // `:` 前スペース不要な制御構文（case/label/無名関数等）
	static const NameSet CombinatorSelector; // CSS/SCSS の結合子セレクタ（子孫／子／兄弟）— 子の疑似セレクタは結合子前提で密着させない
	static const NameSet Comment; // コメントノード
	static const NameSet Compact; // 内部スペース・改行を入れない密集ノード（属性／単項演算子等）
	static const NameSet CompactRecurse; // 内部に分割可能子を持つ Compact 類似ノード
	static const NameSet CondParenWrap; // 条件式を包む括弧ノード (condition_clause / parenthesized_expression) — Lint の条件文脈剥がし
	static const NameSet CondFieldHost; // 条件を場 cond に持つノード (conditional_expression / for_statement / ternary_expression)
	static const NameSet ConditionalAlternative; // else の位置で自身が条件と本体を持つ分岐（else 節に包まれない else if の if・PHP の elseif 節）
	static const NameSet ConstructionReceiverHost; // 生成を最初の子に持つ後置の式（member_expression / member_call_expression 等） — 生成の空の実引数の保持
	static const NameSet ContinueKeyword; // 継続の語 (continue / continue@)
	static const NameSet ContinuingKeyword; // 後に続くトークンを要し，後の改行で文を終えない JS/TS, Go の語（else / typeof / new 等）
	static const NameSet Control; // 制御構文ノード（条件 `()` の括弧維持対象，括弧維持集合の構成要素）
	static const NameSet ControlKeyword; // 制御キーワード（catch/elif/else 等）
	// C/C++ decl の型部位 (primitive_type / type_identifier / qualified_identifier / sized_type_specifier)
	static const NameSet CppDeclTypeSpecifier;
	static const NameSet CppScopeBoundary; // C++ の型引数の遡りを止める範囲 (compound_statement / translation_unit)
	// 型名を導入する C++ の指定子 (class_specifier / enum_specifier / struct_specifier / union_specifier) — 同じファイルで宣言した型名の収集
	static const NameSet CppTypeSpecifier;
	static const NameSet CsNameType; // 式としても読める C# の型名 (identifier / qualified_name)
	static const NameSet CsPostfixChain; // C# の後置式の連鎖
	static const NameSet CssNamespacePrefix; // 名前空間の区切 `|` を持つ CSS のセレクタのノード (attribute_name / namespace_selector)
	static const NameSet CssDelimitedList; // 内側の改行が意味を変えない値・実引数・仮引数の括弧
	static const NameSet CssNumericVal; // CSS/SCSS の数値値 (integer_value / float_value) — 単位 plain_value とのバイト隣接密着判定用
	static const NameSet CssSelectorAtomic; // CSS/SCSS の最小セレクタ系 (class_name / id_name / placeholder) — 内部空白挿入禁止
	// セレクタを取る CSS のノード (extend_statement / pseudo_class_selector / pseudo_element_selector) — 中の `#name` は色でない
	static const NameSet CssSelectorHost;
	static const NameSet CssSelectorName; // CSS のセレクタの名前 (class_name / id_selector) — セレクタの内側で密着
	static const NameSet CssSpacedValue; // 前の値から空白で区切る CSS・SCSS の値の要素
	static const NameSet DecodedLiteralPart; // 投影でデコードするリテラルの部品
	static const NameSet DeclInitDeclarator; // 初期化子を伴う宣言子 (init_declarator / variable_declarator)
	static const NameSet DeclLike; // 一括宣言可能な宣言ノード
	static const NameSet DeclTypeChild; // 宣言の型部位ノード
	static const NameSet DeclaratorHead; // 宣言の中で宣言子を始めるノード (pattern / variable_declarator)
	static const NameSet DecltypeIdExpr; // 括弧の有無で decltype の推論する型が変わる式（名前・メンバアクセス）
	static const NameSet DefaultCase; // 切替の既定の枝のノード
	static const NameSet DestructurePattern; // 分割代入パターン (array_pattern / object_pattern) — 単一名を持たない為ドキュメント／let 再代入解析でスキップ
	static const NameSet DocumentableDecl; // 既存のドキュメントを付けれる型・メンバ・最上位の宣言
	static const NameSet DocumentedDef; // ドキュメントを付ける名前付の関数・コンストラクタの定義（関数定義と Swift のコンストラクタ）
	static const NameSet DocHostSeq; // 文・メンバを並べ直前へドキュメントを置ける親（値の位置へ置いた定義の除外に使う）
	static const NameSet DefBody; // 定義の本体（文の並び・式の本体・Ruby の本文）— 見出の範囲の終わり
	// ドキュメントコメントのアンカーを１階層包むラッパ (ambient_declaration / decorated_definition / export_statement / template_declaration)
	static const NameSet DocWrapper;
	static const NameSet ElseAny; // else 系 (else / else_clause) — else を持たない if の判定
	static const NameSet ElseLikeClause; // else 系の節 (else_clause / catch_clause / finally_clause)
	static const NameSet ExitKeyword; // 関数を抜ける語 (return / throw / throw_keyword) — 語で始まる脱出の文の終端判定
	static const NameSet ExpandContainer; // 展開形を組み立てる丸／波括弧コンテナ
	static const NameSet ExprStmtLiteral; // expression_statement 直下の丸括弧削除を抑止するリテラル系（数値／文字列／真偽値）
	// 被呼出側のフィールドの参照 (field_expression / member_access_expression / navigation_expression) — フィールドの関数の呼出の括弧の保持
	static const NameSet FieldCallee;
	static const NameSet FinallyClause; // 例外の有無に依らず実行する後始末の節（言語横断）
	static const NameSet FlowExit; // 制御フロー脱出文（return/break/continue/goto/throw 系）
	static const NameSet FlowReturn; // 関数から戻る文・式（言語横断，Kotlin / Swift の脱出の文は先頭の語で別に判ずる）
	static const NameSet ForEachHeader; // 反復対象を区切語の後に置く見出（対象の括弧除去用）
	static const NameSet FunctionBodyContainer; // 関数本体ノード (block / compound_statement / function_body / statement_block) 言語横断
	static const NameSet FunctionLikeAny; // 関数の範囲の境界（定義・関数式・ラムダ・ローカル関数）
	static const NameSet FunctionLikeDef; // 言語横断の関数・メソッド定義
	static const NameSet FunctionNameIdentifier; // 関数名として現れる識別子葉 (constant / identifier / simple_identifier) — Lint の関数名判定
	static const NameSet GeneratorMarker; // 本体をジェネレータ・コルーチンにする式・文 — 通常関数の末尾の返戻の対象外
	static const NameSet GeneratorStarHost; // JS/TS の前置 `*` を持つジェネレーター関数
	static const NameSet GoConversionType; // 括弧無で型の変換に置ける Go の型（type_identifier / slice_type / struct_type 等） — 変換の型を包む括弧の除去
	static const NameSet GoDocumentedDecl; // godoc が直前のコメントをドキュメントとする Go の最上位の宣言
	static const NameSet GoFunctionDecl; // 関数のディレクティブを受ける Go の関数宣言 (function_declaration / method_declaration)
	static const NameSet GoGroupedDecl; // Go の丸括弧付一括宣言（括弧内を改行）
	static const NameSet GenericParamList; // 型の仮引数の並び
	static const NameSet GoMemberList; // Go の構造体・インタフェースの要素の並び (field_declaration_list / interface_type)
	static const NameSet GoSemicolonHost; // Go の省略可能なセミコロンを持つ文・メンバ・インポートのコンテナ
	static const NameSet GoSemicolonLiteral; // Go の自動の文終端の前で内部を１トークンとして読むリテラル
	static const NameSet GoSemicolonNamedToken; // Go の自動の文終端を挿入する名前付のトークン（共通の識別子・数値ロール以外）
	static const NameSet GoSemicolonToken; // Go の自動の文終端を挿入する名前無のトークン
	static const NameSet GoSwitch; // Go の switch 文 (expression_switch_statement / type_switch_statement)
	static const NameSet GoType; // Go の型宣言ノード
	static const NameSet GoTypeName; // 型名の形の Go の型 (type_identifier / qualified_type / generic_type) — 見出の複合リテラルを包む括弧の保持
	static const NameSet GreedyTailExpr; // 右端を貪欲に伸ばすか，呼出時だけ括弧を要する式
	// グルーピング括弧ノード (parenthesized_expression / parenthesized_statements / tuple_expression) — 単一被演算子の括弧除去対象
	static const NameSet GroupingParen;
	static const NameSet GroupingTypeParen; // 型を包む括弧のノード (parenthesized_type / tuple_type) — 型を包む冗長な括弧の除去
	static const NameSet HashSplat; // Ruby `**hash` 系ノード (hash_splat_argument / hash_splat_parameter)
	static const NameSet HtmlElementWrap; // HTML 要素ラッパ (element / script_element / style_element)
	static const NameSet HtmlKnownTag; // tree-sitter-html が名前を種別の１バイトで持つ要素の小文字のタグ名 — 開いた要素のシリアライズの大きさの見積
	static const NameSet HtmlRawTextElement; // HTML の埋込 raw_text を持つ要素 (script_element / style_element) — Formatter の部分解析対象
	static const NameSet HtmlTag; // HTML タグ系ノード
	// 葉識別子相当 (`identifier` / `simple_identifier`) — `LintAnalyzer::IsStrVarCandidate` 等で識別子参照を言語横断判定
	static const NameSet IdentifierLeafLike;
	static const NameSet IfNode; // if 系のノード
	static const NameSet IndentContainer; // インデントを付与するコンテナノード
	static const NameSet InfixOperator; // 二項演算子・代入・アロー関数等，両側スペースを入れる
	static const NameSet InitHeader; // 初期化の文を持つ見出
	static const NameSet IntLiteralLike; // 整数リテラル葉 — `LintAnalyzer::CheckCmpBoundary` の境界値判定（`NumLiteral` より狭い）
	static const NameSet IterationHeader; // 反復見出（対象前の `in`/`as` は演算子外）
	static const NameSet JavaEmptyArgHost; // 空の実引数を省ける Java のノード (annotation / enum_constant) — 省ける括弧の除去
	// Java の Unicode エスケープを中身として包むトークン (block_comment / character_literal / line_comment / string_literal)
	static const NameSet JavaEscapeHost;
	static const NameSet JavaExhaustiveCaseLabel; // 枝のラベルに置くと Java の切替へ網羅を要求する照合と null (null_literal / pattern)
	static const NameSet JavaParamScope; // Java の仮引数スコープを開くノード
	static const NameSet JavaStmtHost; // Java で switch を文位置に置く親
	static const NameSet JavaTypeBody; // Java の型の本体 (annotation_type_body / class_body / enum_body / interface_body)
	static const NameSet JsBindingName; // JS/TS の束縛の名前 (identifier / shorthand_property_identifier_pattern)
	static const NameSet JsInRestoring; // JS/TS の `in` を二項演算子に戻す（for 文の初期化子の外に為る）式（args / 計算のキー / フィールド / 対 / 括弧 / ブロック / 添字 / 補間）
	static const NameSet JsLetBindingHead; // `let` の後の改行でパーサーが分けた次の文の最初の名前 (identifier / statement_identifier) — 宣言へ連結
	// JS/TS の被演算子を先頭子に取る参照・呼出 (call_expression / member_expression / subscript_expression) — アロー関数は括弧無で此処へ来れない
	static const NameSet JsMemberHost;
	static const NameSet JsMemberTarget; // JS/TS のメンバ・添字の参照 (member_expression / subscript_expression) — 代入で変数を再束縛しない
	static const NameSet JsModuleStatement; // JS/TS のモジュールのマーカーの文 (import_statement / export_statement)
	static const NameSet JsRestrictedKeyword; // 後に改行を挟めない JS/TS の語 (async / break / continue / return / throw / yield) — 改行で読みが変わる
	static const NameSet JsStatementBlockOrBlock; // JS/TS の statement_block + Java/Go/Rust 等の block — 単独コメント `{ }` のブロック親判定
	// 文・メンバを並べる JS/TS のノード（program / statement_block / class_body / switch_case / interface_body 等）— JSX から退避するコメントの置場・跡に改行を残すコメントの親
	static const NameSet JsStatementSequence;
	static const NameSet JsxContainer; // 子要素を保持する JSX コンテナ（jsx_element，断片 `<>` も同じノード）HTML 要素は別 (HtmlElementWrap)
	static const NameSet JsxElement; // JSX / HTML 要素
	// JSX/HTML の開閉タグ全種 (jsx_opening_element / jsx_closing_element / start_tag / end_tag) — JsxContentCount で内容から除外
	static const NameSet JsxHtmlTagOpenClose;
	static const NameSet JsxTag; // JSX タグ
	static const NameSet JsxTextLike; // JSX の本文 (html_character_reference / jsx_text) — 連続する子文字列
	static const NameSet KeywordJump; // 語で始まる脱出の文 (control_transfer_statement / jump_expression)
	static const NameSet KeywordUnary; // 語や記号で前置する単項の式（キャスト／`co_await` / `delete`／PHP の `@`）— 演算子の優先順位は前置単項
	static const NameSet KotlinAccessor; // Kotlin のアクセサ (getter / setter) — 直前のプロパティの続きとして空行を置かない
	static const NameSet KotlinAssignableWrapper; // 代入先を包む Kotlin のノード (directly_assignable_expression / parenthesized_expression)
	// 本体の波括弧を直接の子に持つ Kotlin の構文 (anonymous_initializer / secondary_constructor / try_expression)
	static const NameSet KotlinBraceBodyHost;
	static const NameSet KotlinClassMember; // Kotlin の型の本体のメンバ（宣言・コンパニオン・初期化子・セカンダリコンストラクタ・列挙子）— パーサーが `ERROR` に包んだメンバの判定
	static const NameSet KotlinFunctionHead; // Kotlin の関数の見出のトークン (fun / modifiers) — 型仮引数の `<` の前に空白
	static const NameSet KotlinJointSuffix; // 改行を跨いで前の式へ繋がる Kotlin の接尾辞・参照・中置式・返戻値
	static const NameSet KotlinNegatableTest; // `!` を前置して否定する Kotlin の検査 (check_expression / range_test / type_test)
	static const NameSet KotlinNotIsIn; // Kotlin `!is` `!in` 演算子の右側キーワード (in / is)
	// Kotlin の数値リテラル接尾子付 (long_literal / real_literal / unsigned_literal) — 数値部と接尾子の密着必須
	static const NameSet KotlinNumericLiteralSuffix;
	// Kotlin 関数の戻値型ノード (dynamic_type / function_type / nullable_type / parenthesized_type / user_type)
	static const NameSet KotlinReturnType;
	// 改行が文を終える Kotlin の文脈 (class_body / control_structure_body / function_body / source_file / stmts)
	static const NameSet KotlinStmtCtx;
	static const NameSet KotlinTypeOperatorHost; // 型の被演算子を取る Kotlin の型 (nullable_type / receiver_type) — 型を包む括弧の保持
	static const NameSet LabeledStmt; // ラベル付の文 (labeled_statement / named_label_statement)
	static const NameSet LazyLogicalOperator; // 右辺を条件付で評価する論理演算子のトークン
	static const NameSet LeadingTypeArgs; // 先頭の型引数を後ろの子へ密着させるノード (method_invocation / type_assertion)
	static const NameSet JumpWithLabel; // ラベルを跳先として持つ脱出の式（中のラベルは定義でなく参照）
	static const NameSet Leaf; // 葉ノード（中身を再構築せず原文を其の儘使う）
	static const NameSet LiteralContent; // デコード対象と逐語保持するリテラルの本文
	static const NameSet Loop; // 全言語のループ構造ノード（for/while/do/range/foreach 系等を全列挙）
	static const NameSet LoopExitStmt; // 繰返を抜ける文（break_statement / continue_statement 等）
	static const NameSet LoopOrSwitch; // break_statement の境界に為る構造（Loop ∪ switch/Go expr-switch/type-switch/select）
	static const NameSet MultilineCommaContainer; // 実引数・仮引数と Go の複数行コンテナ
	static const NameSet MacroBody; // 行末迄のトークンを１つに呑むマクロの本体 (preproc_arg) — 本体の後の行コメントもトークンに含む
	static const NameSet MacroDef; // マクロの定義 (preproc_def / preproc_function_def)
	static const NameSet MacroLike; // マクロ呼出風ノード
	static const NameSet Member; // メンバアクセス連鎖（dot / scope の記法）
	static const NameSet MemberFieldRank; // C#/PHP のプロパティ・定数・イベントフィールド（アクセス順位２）
	static const NameSet MemberMethodRank; // メンバ関数・メソッド・コンストラクター（アクセス順位３）
	static const NameSet MemberTypeRank; // 型エイリアスとネスト型宣言（アクセス順位１）
	static const NameSet MisparsedTemplateHost; // 山括弧の誤解析で型引数リストへ化けた比較式
	static const NameSet MutatingExpr; // 変数を書き換える式 (assignment_expression / update_expression)
	static const NameSet NameDeclarator; // 名前と初期値・型を包む宣言子 (init_declarator / pattern / variable_declarator) — 宣言の型の探索
	static const NameSet NameToken; // 比較用の名前を投影する名前・ラベルのトークン
	static const NameSet NamedParamKind; // 共有の宣言子走査で仮引数を区別するノード
	static const NameSet NamedTypeParam; // 型の導入を持つ仮引数
	static const NameSet NamespaceLike; // 名前空間・モジュールの本体を改行する対象
	static const NameSet NewParenCall; // C# `new()` 形の丸括弧呼出文脈 (constructor_constraint / implicit_object_creation_expression)
	static const NameSet NonConstantExpr; // 定数式に為れない式
	static const NameSet NormColon; // `:` 周辺空白を `: ` 形に正規化するノード
	static const NameSet NumLiteral; // 数値リテラル系ノード
	static const NameSet ObjectLike; // オブジェクトリテラル系ノード（{} 内側スペース挿入対象）
	static const NameSet OpaqueLineText; // 行末迄を本文が飲むノードとパーサーが読めなかった領域 (ERROR / preproc_arg) — 行形式のコメントが再読込で本文へ紛れる
	static const NameSet OpenPrefixExpr; // 語で始まり右の被演算子を式の終わり迄取る式（`yield` / `include` / `print`／ラムダ／`return` 等）— 演算子の優先順位は最も弱い
	static const NameSet OpenTailToken; // 宣言へ密着する記号及び Ruby で改行を式の継続と読ませる文末トークン（`&`・`*`・`**`・`..`・`...`・`:`）
	static const NameSet OptionalMarker; // 直前トークンへ密着する省略可能性記号 (optional_chain / optional_type / quest)
	static const NameSet OptionalTypeMember; // `?` と型注釈を同行に保つ宣言
	static const NameSet OrdinaryFunction; // 通常関数（名前を持つ関数・メソッドの定義，コンストラクタ，デストラクタ，アクセサ） — 末尾の返戻の明示の対象
	static const NameSet ParamContainerLike; // ドキュメントコメント解析対象の仮引数列
	// 関数パラメータコンテナ (formal_parameters / function_value_parameters / parameter_list / params) 言語横断
	static const NameSet ParamContainer;
	static const NameSet ParenInnerFirstChildNoUnwrap; // 内側の最左の子孫が此の型なら括弧を剥がさない（Ruby の多重代入の左辺・Python の条件式を値に取るセイウチ演算子）
	static const NameSet ParenInnerNoUnwrap; // 即時実行・カンマ式・貪欲式等，丸括弧を外せない式
	static const NameSet StructuralParenBoundary; // 見出と本体の構文上の区切
	static const NameSet MeaningfulParenGroup; // 型・値・パターンの意味を定める括弧のグループ
	static const NameSet ReceiverParenHost; // 受手を包む括弧が後置の結合を保つ式
	static const NameSet ParenLeadType; // 丸括弧で始まる型（前置語との空白判定）
	static const NameSet ParenOptionalHeader; // 見出の条件を括弧で包まない言語 (Go / Rust / Swift / Python / Ruby) の制御構文 — 条件を包む括弧は冗長
	static const NameSet ParenValSlot; // 括弧が値の全体を包み後にトークンが続かない値の位置（代入の右辺・実引数・戻値・要素・仮引数の既定値）
	static const NameSet PatternKind; // 変数束縛パターン (pattern / tuple_pattern)
	static const NameSet PhpAltCloser; // PHP の代替構文の終端語 (enddeclare / endfor / endforeach / endif / endswitch / endwhile)
	// PHP の代替構文の本体を開く `:` を直接の子に持つノード (colon_block / declare_statement / for_statement / switch_block)
	static const NameSet PhpAltHost;
	static const NameSet PhpAltLeadingColon; // 本体を開く `:` を先頭のトークンに持つ PHP の代替構文の本体 (colon_block / switch_block)
	static const NameSet PhpIfBranch; // PHP の if の後続の枝の節 (else_clause / else_if_clause)
	static const NameSet PhpNamespaceUse; // PHP の名前空間インポートノード (namespace_use_declaration / namespace_use_group) — `\` とグループ括弧を名前へ密着
	static const NameSet PhpOpenTagEnd; // PHP の開始タグで終わる子 (php_tag / text_interpolation) — 後続を開始タグと同じ行に置く判定
	static const NameSet PhpSemicolonStmt; // 式等で終わり常に `;` で閉じる PHP の文 — 閉タグの直前で `;` を省いたかを末尾の子で見る
	static const NameSet PhpUnsplittable; // 内側で改行出来ない PHP のノード (cast_expression / visibility_modifier)
	static const NameSet PipeParamList; // `|` で囲まれる仮引数列 (block_parameters / closure_parameters)
	static const NameSet PtrDecl; // ポインタ宣言子
	static const NameSet PtrOrRefDeclarator; // C/C++ ポインタ／参照宣言子 (pointer_declarator / reference_declarator) — 戻値型のポインタ／参照判定
	static const NameSet PostfixOrUpdateExpr; // 後置単項演算子及び前置・後置共通の更新式
	static const NameSet PostfixCallHost; // 被呼出側を最初の子に持つ呼出 (call_expression / function_call_expression) — 被呼出側を包む括弧の保持
	static const NameSet PostfixReceiverChain; // 受手（最初の子）を持つ後置の連鎖 — 受手を包む括弧の除去で最初の子を辿る
	static const NameSet PostfixReceiverHost; // 受手を最初の子に持つ後置の式（メンバ参照・呼出・添字・非 null のアサーション）— 受手を包む括弧の除去
	static const NameSet PostfixReceiverLeaf; // 括弧無で後置の式の受手に置ける名前・トークンで閉じるリテラル — 後置の連鎖の最初の子の終点
	static const NameSet Preproc; // C/C++/C# プリプロセッサノード（preproc_* を全列挙）
	static const NameSet PreprocConditional; // プリプロセッサ条件の根 (preproc_if / preproc_ifdef) — elif/else を含まない根限定集合
	static const NameSet PreprocDirectiveClose; // プリプロセッサディレクティブの閉系 (#else / #endif) — 単独ディレクティブで常に空行区切
	// プリプロセッサディレクティブの開き／分岐系 (#elif / #elifdef / #elifndef / #if / #ifdef / #ifndef) — 後続の条件式と連結対象
	static const NameSet PreprocDirectiveOpen;
	static const NameSet PreprocNameQuery; // 条件で名前がマクロかを問う前処理のノード (preproc_defined / preproc_elifdef / preproc_ifdef)
	static const NameSet PythonContinuationExpr; // Python の括弧内で改行を継続出来る式
	static const NameSet PythonDocstringOwner; // 本体の最初の文字列をドキュメンテーション文字列とする Python の定義 (class_definition / function_definition)
	static const NameSet PythonNum; // Python の数値リテラル (float / integer) — `.` の前の括弧が必須
	static const NameSet PythonPlaceholder; // 中身の無い事を表す Python の文・式 (pass_statement / ellipsis)
	static const NameSet PythonPostfix; // Python の後置の式 (attribute / call) — 最高位の優先順位
	static const NameSet QualifiedMember; // 受手を修飾する Java / C# のフィールドの参照 (field_access / member_access_expression)
	static const NameSet RangeLike; // 範囲式・範囲パターン (range_expression / range_pattern)
	static const NameSet RangeOperator; // 範囲の演算子（`..`・`...`）
	static const NameSet RangeOrUnaryExpr; // Kotlin の増減式及び Swift の範囲／単項演算子親
	static const NameSet ResultlessRoutine; // 値を返さない構築・破棄・設定・監視の操作
	static const NameSet ReturnOrThrow; // 次の文へ進まずに関数を終える文・式（返戻・送出・処理の終了） — `ControlFlow::TerminationRuleOf`
	static const NameSet ReturnStmt; // 値を返す文 (co_return_statement / return_statement) — 値の全体を包むマクロの括弧を外せる
	static const NameSet ReturnTuple; // 戻値が組型のノード
	static const NameSet ReturnValHost; // return の返す値の参照の有無を決める関数（C++ の decltype(auto) / PHP の参照返）
	static const NameSet ReturnValParent; // 返す値を直接の子に持つノード (arrow_function / return_statement)
	static const NameSet RubyAnonymousForward; // 名前を略して転送出来る Ruby の実引数 (block_argument / hash_splat_argument / splat_argument)
	static const NameSet RubyArgExtent; // 命令形の呼出の実引数が左端から続く Ruby の式 (binary / call / conditional / element_reference / range)
	static const NameSet RubyArgKeyword; // 空白に依らず後続を実引数と読む Ruby の語 (break / next / return)
	static const NameSet RubyArgListHost; // 実引数の並びを子に持つ Ruby のノード (break / call / next / return / yield)
	static const NameSet RubyArgSeq; // Ruby の要素の並び (argument_list / right_assignment_list) — 名前の後の実引数が残りの要素を取り込む
	static const NameSet RubyArgStart; // 名前の後に空白を置いて続けると実引数の始まりに為る Ruby のトークン (& * ** + - << [)
	static const NameSet RubyArrayLiteral; // Ruby `%w[...]` / `%i[...]` 配列リテラル親 (string_array / symbol_array)
	static const NameSet RubyBinding; // 修飾子化で束縛と参照の順序が変わる Ruby 式
	static const NameSet RubyBlock; // Ruby 制御ブロック（end 区切）
	static const NameSet RubyBraceBlock; // １文なら１行に置く Ruby の波括弧ブロックと本体
	static const NameSet RubyClassDefLike; // Ruby class 定義系 (class / singleton_class)
	// 命令形の呼出の実引数の読みを揃える Ruby のノード (binary / break / call / element_reference / next / range / return / yield)
	static const NameSet RubyCommandArgHost;
	static const NameSet RubyCondHost; // `cond` のフィールドを持つ Ruby のノード (elsif / if / unless) — 空本体への ` then` 補完対象
	static const NameSet RubyDelimitedLiteral; // Ruby の区切文字付リテラル (delimited_symbol / regex / subshell) — 括弧の深さ計算で内部へ降りない
	static const NameSet RubyFlowExpr; // Ruby の脱出式 (break / next / redo / retry / return / yield) — 範囲式の始端に来ると `return` 前置が破綻する
	// 外のローカル変数を見ない新しいスコープを開く Ruby のノード (class / method / module / program / singleton_class / singleton_method)
	static const NameSet RubyFreshScope;
	static const NameSet RubyLocalScope; // ローカル変数のスコープを開く Ruby のノード（RubyFreshScope ∪ block / do_block / 無名関数，ブロックは外のローカル変数も見る）
	static const NameSet RubyLowBinary; // 命令形の呼出の実引数の外に在る Ruby の語の二項演算子 (and / or)
	static const NameSet RubyLowUnary; // 代入より弱い Ruby の単項の語 (`defined?` / not)
	static const NameSet RubyMethodDef; // Ruby メソッド定義系 (method / singleton_method)
	static const NameSet RubyNumericLiteralSuffix; // Ruby rational/complex リテラル親 (complex / rational) — 数値部と接尾子 r/i/ri の密着必須
	static const NameSet RubyParams; // Ruby の仮引数の並び (block_parameters / lambda_parameters / method_parameters) — ローカル変数を導入する
	static const NameSet RubyPatternTest; // Ruby のパターン照合の式 (match_pattern / test_pattern) — 式の中で最も弱い
	static const NameSet RubyStmtHost; // Ruby の文が直接置かれるコンテナ（program / then / do / body_statement 等）— 値位置との判別
	static const NameSet RubyStmtSeq; // Ruby の文を並べるノード（parenthesized_statements / then / block_body 等）— 平坦化で文を `;` で区切る
	static const NameSet WritableAssignment; // 代入先を書き換える式（可変性の検査対象）
	static const NameSet RustBaseTraversal; // Rust の mut 削除候補から基底名を辿るノード
	static const NameSet RustBlockLike; // 文頭に置くと `}` で文を終える Rust のブロックの式（block / unsafe_block / if_expression / loop_expression 等）
	static const NameSet RustBodyOnlyLoop; // 条件と本体が独立した一時値の範囲に為る Rust の繰返 (loop_expression / while_expression)
	static const NameSet RustBorrowingOperator; // 被演算子を参照で受ける Rust の比較演算子のトークン
	static const NameSet RustDeclStmt; // Rust のブロックの中の宣言の文（値を与えない）
	static const NameSet RustDestructurePattern; // 単一名を持たない Rust の分解パターン
	static const NameSet RustLetBinding; // Rust の let の束縛 (let_condition / let_declaration)
	static const NameSet RustLetCond; // 照合の対象を持つ Rust の条件 (let_chain / let_condition)
	static const NameSet RustParamNameWrapper; // Rust の仮引数名を包むパターン
	static const NameSet RustParamPattern; // 型を明示出来る Rust の仮引数のパターン
	static const NameSet RustPlaceOrPlainVal; // 場所を要する文脈に置いても破棄を伴う一時値を作らない Rust の式（場所の式とトークン）
	static const NameSet RustPlaceProjection; // 最初の子を場所として参照する Rust の式 (field_expression / index_expression)
	static const NameSet RustRefPattern; // Rust の参照・フィールドのパターン (field_pattern / ref_pattern)
	static const NameSet RustScrutineeHost; // 照合の対象を `val` 場に持つ Rust の式 (let_condition / match_expression)
	static const NameSet RustStmtHead; // 文頭の式を持つ Rust のノード (block / expression_statement)
	static const NameSet RustTypeOperatorHost; // 型の被演算子を取る Rust の型（reference_type / pointer_type / bounded_type 等） — 型を包む括弧の保持
	static const NameSet RustValJump; // 値を取り得る Rust の脱出の式 (break_expression / return_expression / yield_expression)
	static const NameSet ScopeBody; // 言語固有の範囲本体
	static const NameSet ScopeContainer; // 関数・メソッド・プログラムの本体スコープ
	static const NameSet ScssMixinRule; // ミックスインの定義とインクルードの SCSS の規則 (`include_statement` / `mixin_statement`) — 空の括弧の除去
	static const NameSet ScssModuleClause; // モジュールの別名・公開する名前・設定の節と，設定を開く語
	static const NameSet ScssModuleRule; // モジュールを読む SCSS の規則 (`forward_statement` / `use_statement`)
	static const NameSet ScssNameAtom; // モジュールの別名と名前空間付のミックスイン名
	static const NameSet ScssParamHost; // 仮引数の並びを持つ SCSS の規則 (`function_statement` / `mixin_statement`) — 空の並びの `MISSING` の許容
	static const NameSet SectionKeyword; // 手前のブロックに続く節を開く語 (catch / else / elseif / ensure / finally / rescue)
	static const NameSet SeparatorToken; // 区切のトークン（`,`・`:`・`;`）— 中置演算子として空白で囲まない
	static const NameSet SequentialStmtContainer; // 逐次実行される文の直接コンテナ（`FunctionBodyContainer` ∪ `stmts` の統合済集合）
	static const NameSet SingleBodyStmt; // 本体を１度だけ実行する文（言語横断） — 本体の終端で文の終端を判ずる
	static const NameSet SpaceSkip; // スペース正規化スキップノード（リテラル／コメント／特殊構文等）
	static const NameSet SpaceKeyword; // スペース付与キーワード
	static const NameSet SpacelessBrace; // `{` 前後の空白を許容しないノード
	// 逐次文のコンテナ（`SequentialStmtContainer` ∪ `control_structure_body` ∪ `statement_list`）
	static const NameSet StmtFlowContainer;
	static const NameSet StmtList; // 文リストノード（Go/Kotlin 等の括弧付宣言グループや Ruby のメソッド本体）
	static const NameSet StmtsOnlyBreak; // 文列の前でだけ改行し，見出の子（パターン・仮引数）を同じ行に保つ節 (catch_block / switch_entry)
	static const NameSet StmtCtxParent; // 文文脈の親 (expression_statement / stmts) — 式値を使わない純文の文文脈判定
	static const NameSet StrDelimiter; // 文字列を開閉する引用符のトークン（" / ''' / ` 等） — 閉じない文字列の検出
	static const NameSet StrInterpolation; // 文字列の中の埋込式 (`${...}` / `$x` / `#{...}` / `\(...)` / `{...}`) — コードの為にコメントを置ける
	static const NameSet StrLikeAll; // 言語横断の全文字列リテラル
	static const NameSet StrLikeInnerPreserve; // 行末空白除去時に内部バイト範囲を保護する文字列／ヒアドキュメント／コメント系ノード
	static const NameSet StrSeqContainer; // 行分割で平坦に再帰する文字列・記号列
	static const NameSet SwiftBraceBodyHost; // 本体の波括弧を直接の子に持つ Swift の制御構文 (do / for / guard / if / repeat-while / while) — 本体の文を展開
	// 見出の式を直接の子に持つ Swift の文・節 (if / guard / for / switch / while / where_clause) — 見出の式を包む括弧の保持
	static const NameSet SwiftHeaderExprHost;
	static const NameSet SwiftHeaderLead; // Swift の見出の式の前のトークン（`if`・`while`・`,`・`=`・`in` 等） — 見出のクロージャの括弧付
	static const NameSet SwiftHeaderStmt; // 見出の後に本体の波括弧が続く Swift の文 (if / guard / for / switch / while) — 見出のクロージャの括弧付
	// 名前付のノードに為る Swift の語 (value_binding_pattern / binding_pattern_kind / where_keyword) — 後の括弧との間の空白
	static const NameSet SwiftKeywordNode;
	static const NameSet SwiftLiteralText; // 中身をコードと読まない Swift の文字列・正規表現のリテラル（補間の中の式を除く） — 密着コメントの修復の除外
	static const NameSet SwiftPostfixCall; // Swift の呼出・添字の後置式 (call_expression / subscript_expression) — 任意連鎖の `?` を密着
	static const NameSet SwiftStrayBlock; // 見出の文の直後の同じ行の `{` で始まる Swift のブロック (`ERROR` / `lambda_literal`) — 見出のクロージャの読違の検出
	// 型の被演算子を取る Swift の型（optional_type / metatype / protocol_composition_type 等） — 型を包む括弧の保持
	static const NameSet SwiftTypeOperatorHost;
	static const NameSet SwitchBody; // switch の本体 (compound_statement / switch_block / switch_body)
	static const NameSet SwitchGotoTarget; // C# の `goto case` / `goto default` の跳先を表すトークン
	static const NameSet SwitchStmtLike; // switch に類する文（switch_statement / when_expression / match_expression 等）
	static const NameSet SwitchOrWithExpr; // switch 式及び C# の with 式
	static const NameSet ColonPairHost; // 三項演算子系 + range-for 系（`?` `:` 周辺で特殊スペース処理対象）
	static const NameSet ThenDo; // `then` / `do` 等の制御本体トリガ
	static const NameSet ThisRef; // 自身の参照 (this / this_expression)
	static const NameSet Transparent; // 透過ノード（深さを増やさず子を其の儘展開）
	// `SourceProjector::Project` のアンカーの展開対象（FunctionBodyContainer ∪ control_structure_body / stmts）
	static const NameSet TransparentWrap;
	static const NameSet TryStmt; // 例外処理の文・式（言語横断，Swift の do は別に判ずる）
	static const NameSet TypeCombination; // 型を合成する `|` / `&` を持つノード
	static const NameSet TsFunctionType; // 戻値の型の中で条件の型を再び許す TS の関数の型 (constructor_type / function_type)
	static const NameSet TsGreedyType; // 右端を貪欲に伸ばす TS の型 (conditional_type / constructor_type / function_type / infer_type)
	// １要素の組 `(x,)` で末尾カンマ必須に為る組系 (tuple / tuple_expression / tuple_pattern / tuple_struct_pattern / tuple_type)
	static const NameSet TupleLike;
	static const NameSet TypeArgLookalikeSlot; // 比較式が型引数列に見え得る一覧要素の位置
	static const NameSet TypeCastRight; // 型を右に取る変換の式 (as_expression / satisfies_expression / type_cast_expression)
	static const NameSet TypeColonParent; // `:` の右を値・型とするキー値対・型のフィールド (pair / pair_pattern / property_signature) — 優先順位の右端探索の停止
	static const NameSet TypeMemberBody; // 全文法の型メンバの本体（共有する本体は所有者で区別する）
	// 宣言の型の名前（primitive_type / predefined_type / integral_type / type_identifier / user_type 等） — 宣言の型の探索
	static const NameSet TypeNameNode;
	// 名前に型を付ける宣言・仮引数（decl / parameter / let_declaration / property_declaration 等） — 宣言の型の探索
	static const NameSet TypedDecl;
	// 演算子を持つ型（union_type / function_type / nullable_type / bounded_type 等） — 型の演算子の被演算子を包む括弧の保持
	static const NameSet TypePrefixKeyword; // 内側の前置型の語
	static const NameSet TypePrefixOperand; // 前置型の被演算子
	static const NameSet TypePrefixOperator; // 外側の前置型の語
	static const NameSet TypePrefixWeak; // 前置語より結合の弱い型
	static const NameSet TypePrefixWrapper; // 内側の前置型を包む構文
	static const NameSet TypeOperatorInner;
	static const NameSet UnaryPre; // 前置単項演算子
	static const NameSet UnaryPreOrUpdate; // 前置単項演算子 + 更新式（UnaryPre ∪ update_expression の統合済集合）— Canonicalizer の隣接スペース判定用
	static const NameSet UnaryPrefixExpr; // 前置の単項式 (prefix_unary_expression / unary_expression) — `x = !x` の検査
	static const NameSet UnaryPrefixToken; // キャスト風の括弧の後に来る前置単項の記号 (! / + / - / ~)
	static const NameSet Unformattable; // 整形対象外ノード（内部構造を保持し原文返戻）
	static const NameSet VarDecl; // 変数宣言ノード（DeclLike + let_declaration/property_declaration 含む拡張バージョン）言語横断
	static const NameSet VarDeclarator; // 名前を `name` 場に持つ Java / C# の変数の宣言 — `ControlFlow::DeclarationsIn`
	static const NameSet VerbatimNode; // 子から組み直さず原文をコピーするノード (`ERROR` / `shebang_line`)
	static const NameSet WhileLoop; // 先に条件を判ずる繰返 (while_expression / while_statement)
	static const NameSet &GetEmptyStmtContainers(const ::Lang Lang); // 無名の空文・空宣言を直接並べる文法毎のコンテナの取得関数
	static const NameSet &GetElementBodies(const ::Lang Lang); // 文法毎の要素列の本体の登録集合の取得関数
};
