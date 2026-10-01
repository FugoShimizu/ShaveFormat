#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

enum class CommentNotation : uint8_t;

namespace Core {
	enum class Role : uint16_t {
		AliasDeclaration, // 別名宣言
		AnnotationParts, // 注釈を成す子の並び
		ArrowFunction, // 単一の名前の仮引数を括弧無で書ける関数
		Assignment, // 代入の式
		Await, // 待機の式
		BareTuple, // 囲みを持たない組の並び
		Block, // 文を波括弧で包み得る塊
		BodyOwner, // 単一の文の本体を持つ制御構文（繰返・ラベル付の文・資源の文・else 節等）
		BodyWrapper, // 波括弧の有無に依らず制御本体を包む節点
		BranchBody, // 枝の値を結果にし得る本体
		CaseArm, // 値を結果にする分岐の枝
		Clause, // 同じ深さの行頭へ置く後続の節
		CodeBoundary, // 地の文を挟むコードの境界
		CodeOpener, // 埋込コードを開く字句
		CollapsibleText, // 行を跨ぐ空白を畳む地の文
		CommaDeclaration, // 宣言子又は名前をカンマで併記出来る宣言
		ConditionSection, // 条件と本体の境に語を要する節
		Construction, // new に依る構築式
		ContinuationExpression, // 改行時に継続用の包装を選べる式
		Declaration, // 本体に置くと有効範囲の変わる宣言
		DeclaratorItem, // 併記出来る宣言子
		DeducedType, // 宣言子毎の初期化子から型又は型引数を推論し得る型（C++ の `auto` と型引数を省いた総称型の名前）
		DefaultParameter, // `=` を伴う仮引数の既定値
		DelimitedBody, // 見出の区切りと終端の語で囲む本体
		DoLoop, // 後判定の繰返
		ElseClause, // else 節
		ElseIfClause, // 自身の条件を持つ後続の枝の節
		ElseKeyword, // else の字句
		EnumSpecifier, // 列挙型の指定
		EqualsSpaced, // 初期化・既定値の `=` を空白で区切る構文
		EqualsTight, // 名前付引数等の `=` を密着させる構文
		Error, // 解析器の誤り区画
		ExceptionSection, // 例外の見出と本体を改行で分ける節
		ExpandedBody, // 文を持つ本体を必ず展開する容器
		Export, // 公開する宣言又は値
		ExpressionStatement, // 式の文
		ExpressionTerminator, // 式を終える区切りの字句（閉じ括弧・カンマ・セミコロン）
		FinalSection, // 常に展開する後始末の節
		If, // 条件分岐
		IndentedBody, // 改行と字下げで境界を表す本体
		InlineText, // 地の文
		Instantiation, // 型引数を付けた式
		Iteration, // in 又は of で反復する構文
		KeywordBlock, // 語で開始し終端の語で閉じる本体
		KeywordSection, // 語で始まる後続の節
		Label, // ラベル・注釈
		Labeled, // ラベル付の文
		LastValueLambda, // 最後の文の値を結果にするラムダ
		Loop, // 繰返
		MarkupAttribute, // タグの属性
		MarkupClose, // 内容を終えるタグ
		MarkupContainer, // 要素と本文を並べる文書構造
		MarkupElement, // 開閉タグで内容を囲む要素
		MarkupExpression, // 地の文の中の式の包装
		MarkupOpen, // 内容を始めるタグ
		MarkupText, // 表示する本文
		MemberBody, // 終端記号を持つ成員の本体
		NameParts, // 名前を成す子の並び
		NamedImports, // 名前付の取込の並び
		Namespace, // 名前空間の宣言
		NamespaceAliasDefinition, // 名前空間の別名の定義
		NamespaceDefinition, // 本体を持つ名前空間の定義
		NonNullAssertion, // null でない事の表明
		OpenBrace, // `{` の字句
		OuterText, // 本体の外で逐語に保つ地の文（最初の字句の前の空白と閉じタグの後の地の文）
		PatternBranch, // 対象式の後で改行を要する照合
		PostfixReceiverHost, // 受け手に呼出又は添字を付ける式
		PrefixOperation, // 前置の演算
		Preprocessor, // 前処理指令
		PreprocessorBranch, // 宣言等を子として持つ条件分岐の前処理指令
		PreprocessorLine, // １物理行の内部を折らない前処理指令
		PreservedTokenSequence, // 展開・照合に用いる字句列（内部の字句と間隙を保持する）
		RequiredParameter, // 型注釈を持ち得る必須の仮引数
		ReturnValue, // 明示的な返戻で包める値
		SectionHeader, // 本体より１段浅く置く case・ラベル・アクセス修飾子等の節見出
		StatementHost, // 文を直接並べる節点
		StatementOnly, // 値を持たない文
		StatementSequence, // 本体の中の文の並び
		TailValueFunction, // 本体末尾の値を暗黙に返す通常関数
		TerminatedMember, // 本体の兄弟として終端記号を補う成員
		TerminatedStatement, // 終端のセミコロンを要する文
		TrailingCommaHost, // 展開時に末尾のカンマを要する並び
		TransparentSection, // 同行時の境界語を親が出す本体
		TypeAlias, // 型の別名の定義
		TypeApplication, // 型引数を適用する型
		TypeArgumentList, // 型の実引数の並び
		TypeAssertion, // 型の表明
		TypeDefinition, // 型の定義
		TypeMember, // 両区切りを許す型の成員
		TypeMemberBody, // セミコロンとカンマの何方も使える型の本体
		TypeParameterList, // 型の仮引数の並び
		TypeSyntax, // 式とは異なる文法で読む型の包装
		TypeofComparison, // typeof の比較を厳密化出来る二項式
		UsingKeyword, // 資源の宣言の語
		ValueBranching, // 枝の値を結果にする式
		ValueTry, // 値を結果にする try 式
		// 正規化の規則・制御の流れ・署名・コメント
		AdjacentDocumentSequence, // 直後の宣言が先行コメントを文書へ取り込む並び
		AlternativeBodyCloser, // 代替の本体記法の終端語
		AmbientDeclaration, // 外部の実体を記述する宣言
		AnnotatedLambda, // 注釈を伴うラムダ
		Annotation, // 宣言へ付ける注釈
		AnonymousClass, // 無名のクラス
		AppliedGenericType, // 引数を適用した総称型
		ArgumentReturn, // 実引数の並びを返す返戻
		ArgumentSequence, // 実引数を並べる容器
		ArmSwitch, // 枝を全列挙して終端を判定する切替
		ArrayDeclarator, // 配列の宣言子
		ArrayValue, // 配列の値
		AssertsAnnotation, // 真である事を表明する戻値注釈
		AssignmentExpression, // 代入の値を表す式
		AttachPrefix, // 宣言に先行して文書の錨と為る注釈
		Attribute, // 属性の参照又は指定
		AttributeArgumentList, // 属性の実引数の並び
		AttributedTailFunction, // 属性の展開が本体を書き換え得る末尾値の関数
		BareArgumentNotation, // 括弧を省ける実引数・仮引数と終端の無い値の記法
		BareFallthrough, // 独立した語で次の枝へ進む
		BareListTrailingComma, // 囲みを持たない並びの余分な末尾カンマを除く
		Binary, // 二項の値
		BinaryOperation, // 二項の演算式
		BodyTemporaryScopeLoop, // 一時値の有効範囲を本体に限る反復
		Boolean, // 真偽値
		BooleanInitializerDeclaration, // 整数の初期値を真偽値名へ揃える型付宣言
		BooleanLiteral, // 真偽値の字句の包装
		BorrowingOperator, // 借用又は間接参照の演算子
		BreakKeyword, // 反復から脱出する語
		BreakTarget, // 無名の脱出先に為る構文
		CallArguments, // 呼出の実引数の並び
		CallExpression, // 呼出式
		CallNode, // 関数又はメソッドの呼出
		CallSuffix, // 呼出を付加する接尾部
		CapitalBooleanLoop, // 先頭大文字の真偽値を条件に用いる反復
		CaseBodyOpener, // 枝の本体を開く区切
		CaseClause, // 切替の枝の節
		CaseLabelJump, // 切替の枝を飛先として指定出来る移動
		CatchClause, // 例外を捕捉する節
		CatchableBody, // 捕捉の節を後置する本体
		Class, // クラスの本体を伴う構文
		ClassBody, // クラスの成員を持つ本体
		ClassDeclaration, // クラスの宣言
		ClassMembers, // クラスの成員の並び
		ClauseOrConditionLoop, // 条件又は三節を選べる反復
		CloserToken, // 囲みを閉じる字句
		ColorValue, // 色の値
		Comment, // コメント
		CommunicationCase, // 通信で選ぶ切替の枝
		CompoundAssignment, // 演算を伴う代入
		CompoundBody, // 複数の文を囲む本体
		CompoundFunctionBody, // 文列の本体を波括弧の塊に限る通常関数
		CondParenWrap, // 条件を包む括弧又は条件の容器
		ConditionalBranchClause, // 条件を持つ分岐の節
		ConditionalValue, // 条件に依って選ぶ値
		ConstantName, // 定数の名前
		ConstructionReceiverHost, // 構築式の受け手の親
		ConstructorInvocation, // 構築子の呼出
		ContinueExitsSwitch, // 継続で外へ抜ける切替
		ContinueKeyword, // 反復を継続する語
		CountedLoop, // 初期化・条件・更新を持つ反復文
		DeclTypeChild, // 宣言の型を示す子
		DeclarationList, // 宣言の並び
		DeclarationSpecifier, // 宣言の性質を指定する構文
		DeclarationTypeSpecifier, // 宣言の型を指定する構文
		DeclaratorParameters, // 宣言の仮引数の並び
		DeclaratorSignature, // 入れ子の宣言子で署名を定める定義
		DefaultCase, // 既定の枝
		DefinitionBody, // 定義の本体
		DestructorDeclaration, // 破棄子の宣言
		DestructurePattern, // 値を分解して束縛する模式
		DirectParameterSignature, // 仮引数を直下へ並べる定義
		DirectSwitchArms, // 切替の枝を直下に並べる
		DiscardedBranchValueHost, // 枝の値を使わない文の親
		DocHostSequence, // 文書と定義を順に並べる容器
		DocWrapper, // 文書化対象を包む構文
		DocumentString, // 本体の先頭で文書に為り得る文字列
		DocumentableDeclaration, // 既存の文書を付けれる型・成員・最上位の宣言
		DocumentedDefinition, // 文書を同期する定義
		ElementReference, // 要素の参照
		Ellipsis, // 省略を表す語
		Else, // 条件に一致しない時の枝
		ElseAny, // 既定の後続枝
		EmptySemicolonSequence, // 空文を区切りとして除ける文の並び
		EmptyStatement, // 空の文
		EnumBodyDeclarations, // 列挙本体の宣言の並び
		EnumClassBody, // 列挙クラスの本体
		EnumEntry, // 列挙の項目
		ExitKeyword, // 実行を終端する語
		ExitStatement, // 実行を終了する文
		ExplicitKeywordBody, // 開始語を明示する本体
		ExplicitResultFunction, // 末尾へ明示返戻を補える通常関数
		ExpressionCase, // 式で選ぶ切替の枝
		ExpressionList, // 式の並び
		FallthroughName, // 次の枝へ進む語の候補
		FallthroughStatement, // 次の枝へ進む文
		FieldName, // 欄の名前
		FinallyClause, // 後始末の節
		FlowBoundary, // 外側の制御移動から独立する境界
		FlowReturn, // 制御を呼出元へ返す構文
		ForClause, // 三節の反復の見出
		ForRangeLoop, // 範囲を走査する反復
		ForeachStatement, // 要素を走査する反復文
		FunctionBody, // 関数の本体
		FunctionBodyContainer, // 関数の文を直接持つ容器
		FunctionCallExpression, // 関数を対象とする呼出式
		FunctionDeclarator, // 関数の宣言子
		FunctionDefinition, // 関数の定義
		FunctionExpression, // 関数を値とする式
		FunctionValueParameters, // 関数の値の仮引数の並び
		GeneratorMarker, // 生成器を指定する記号
		GenericName, // 型引数を伴う名前
		GenericParameter, // 総称型の仮引数
		GenericParameters, // 総称型の仮引数の並び
		GotoStatement, // 名前を指定する移動文
		GroupingParen, // 式をまとめる括弧
		HeredocBeginning, // 複数行の文字列の開始部
		HoistedDocumentBody, // 先頭の定義の文書を外へ巻き上げる本体
		Identifier, // 識別子
		ImplicitResultBody, // 末尾値を暗黙に返す本体
		ImplicitReturnCandidate, // 結果構築子の適用を調べる単一式の通常関数
		ImplicitReturnCandidateExpression, // 暗黙返戻の単一式と為り得る式
		ImplicitReturnConformance, // 結果構築子を推論し得る適合・継承の節
		ImplicitReturnExpression, // 暗黙返戻を明示出来る単一の値の式
		ImplicitReturnExtensionToken, // 外部の適合を持ち得る拡張の語
		ImplicitReturnNominal, // 成員の暗黙返戻を調べる型の宣言
		InferredParameters, // 型を推論する仮引数の並び
		InitDeclarator, // 初期化子を伴う宣言子
		InitializerExpression, // 初期化の式
		InlineDefinitionBody, // 独立した本体容器を要しない文書化対象
		InnerFunctionStatements, // 本体の内側で関数の文を並べる容器
		IntegerValue, // 整数の値
		IterationValue, // 反復の値
		JumpLabel, // 移動先のラベル
		KeywordJump, // 語で指定する制御移動
		KeywordWhile, // 条件判定の反復の語
		LabeledBlockExit, // 本体自身のラベルへ脱出出来る文の並び
		LabeledStatement, // ラベル付の文
		LabeledValueExit, // 値とラベルを分けて指定する脱出
		LambdaExpression, // 式としてのラムダ
		LambdaFunctionType, // ラムダの関数型
		LambdaFunctionTypeParameters, // ラムダの関数型の仮引数
		LambdaLiteral, // ラムダの値
		LambdaParameter, // ラムダの仮引数
		LambdaParameters, // ラムダの仮引数の並び
		LazyLogicalOperator, // 短絡評価する論理演算子
		LeadingConstDeclaration, // 型の前へ const を移せる宣言
		LeadingStringDocumentOwner, // 先頭の文字列を文書とする定義
		LexicalBlock, // 局所の有効範囲を持つ塊
		LinkageSpecification, // 結合方式の指定
		LoopElse, // 脱出せず終了した時の後続枝を持つ反復
		LoopExitStatement, // 反復から脱出する文
		LoopExpression, // 無条件の反復式
		MacroCall, // マクロの呼出
		MatchBlock, // 照合の枝を並べる塊
		MayDeclarePlaceholderFunction, // 省略の本体だけで型を宣言出来る関数
		MethodDeclaration, // メソッドの宣言
		MethodDefinition, // メソッドの定義
		MethodInvocation, // メソッドの呼出
		MethodParameters, // メソッドの仮引数の並び
		MixinParameters, // ミックスインの仮引数
		MixinRule, // ミックスインの宣言又は呼出
		MixinStatement, // ミックスインの呼出文
		Modifier, // 宣言の修飾子
		Module, // モジュールの本体
		NamedAccessor, // 名前で読出と其の他を分ける操作
		NamedLabelStatement, // 名前付のラベルの文
		NonConstantExpression, // 定数式に為れない式
		NonValueDeclaration, // 値を返さない宣言文
		NormalizedNumber, // 数値の字形を揃える値
		NormalizedQuote, // 引用符を二重引用符へ揃える文字列
		NumberLiteral, // 数値の値を表す構文
		NumericToken, // 数値を表す字句
		ObjectCreationExpression, // オブジェクトの構築式
		ObjectValue, // オブジェクトの値
		OmittableConditionLoop, // 省いた条件で無条件に続く三節の反復
		OpaqueLineText, // 行を分割せず保持する本文
		OperatorCast, // 型変換の演算子
		OptionalAnnotationArguments, // 空の注釈引数を省ける親
		OptionalConstructionArguments, // 空の構築・注釈・終了の実引数を省ける構文
		OptionalEmptySuperclass, // 空の基底型の並びを省ける定義
		OptionalLambdaDeclarator, // 空の仮引数だけの宣言子を省けるラムダ
		OptionalLoopTerminator, // 本体を持たない反復又は代替構文の終端を補う
		OptionalSemicolonHost, // 文・成員のセミコロンを改行へ替えれる並び
		OptionalVoidParameter, // 引数の無い事を表す型を省ける並び
		OverloadableIncrement, // 多重定義を区別して増減を揃える式
		ParamContainerLike, // 仮引数を含む容器
		Parameter, // 仮引数
		ParameterDeclaration, // 型を含む仮引数の宣言
		ParameterName, // 仮引数の署名へ取り出す名前
		ParenthesizedDeclarator, // 括弧で囲む宣言子
		ParenthesizedLoopCondition, // 括弧の条件を持つ反復
		PatternCondition, // 模式への束縛を条件とする構文
		PatternList, // 模式の並び
		PatternMembership, // 所属を調べる模式
		PatternParameter, // 束縛の模式で名前を定める仮引数
		PlaceOrPlainValue, // 格納場所又は一時値を延長しない値
		PlaceProjection, // 格納場所の部分を参照する式
		PlaceholderBody, // 説明文字列や省略の文だけを空本体とする並び
		PlaceholderStatement, // 空の実装を表す省略の文
		PlaceholderTypeSpecifier, // 推論を要求する型指定子
		PlainStatementSequence, // 値を返さない文の並び
		PointerDeclarator, // ポインターの宣言子
		PointerOrRefDeclarator, // ポインター又は参照の宣言子
		PointerTypeDeclarator, // ポインター型の宣言子
		PostConditionLoop, // 後判定で本体を一度は実行する反復
		PostParameterResultType, // 仮引数の後へ置く戻値型
		PostfixExpression, // 後置の演算式
		PostfixUnaryExpression, // 後置の単項演算式
		PrefixableIncrement, // 使わない結果の増減を前置へ揃えれる式
		PreprocElse, // 前処理の既定の枝
		PrimaryConstructor, // 主構築子
		PrimitiveType, // 基本型
		QualifiedIdentifier, // 修飾付の識別子
		QualifiedMember, // 受け手又は修飾を伴う成員
		QuotedString, // 引用符で囲む文字列
		Range, // 範囲
		RangeClause, // 範囲反復の見出
		ReaderRoutine, // 値を返す読出の操作
		ReceiverCall, // 受け手を伴い得る呼出
		ReferenceExpression, // 参照を得る式
		Regex, // 正規表現
		ResultlessRoutine, // 値を返さない構築・破棄・設定・監視の操作
		ReturnKeyword, // 返戻の語
		ReturnOrThrow, // 返戻又は例外送出
		ScopeBody, // 独立した有効範囲の本体
		ScrutineeHost, // 照合対象の値を持つ構文
		SecondaryConstructor, // 副次の構築子
		SectionKeyword, // 節を始める語
		SelectorHost, // 選択子を持つ構文
		SelfValue, // 自身を表す値
		SeparateParameterParts, // 仮引数と既定値等を別の子に持つ定義
		SiblingLabeledBranch, // 先行する兄弟のラベルから脱出出来る条件分岐
		SimpleName, // 修飾を持たない名前
		SingleBodyStatement, // 単一の本体を持つ文
		SingleExpressionResultFunction, // 単一式の本体が暗黙に値を返す関数
		SizedTypeSpecifier, // 幅を指定する型指定子
		StarGeneratorFunction, // 星の指定で生成器に為る関数
		StatementFlowContainer, // 制御の流れを順に調べる文の容器
		StatementLabel, // 文へ付けるラベル
		StyleLexeme, // 色・宣言末尾・空のミックスイン引数を正規化する構文
		Subscript, // 添字の指定
		SwitchBody, // 切替の枝を持つ本体
		SwitchEntry, // 切替の枝の項目
		SwitchLabel, // 切替の枝のラベル
		SwitchStatement, // 値に依る切替文
		TerminatingMacroCall, // 終端が定まる標準マクロを呼べる構文
		ThisReference, // 現在の実体への参照
		TrailingReturnType, // 宣言子の後に置く戻値型
		TranslationUnit, // 翻訳単位
		TransparentLabelPrefix, // ラベルと本体の間で制御移動に影響しない注釈
		TrueValue, // 真の値
		TryStatement, // 例外を扱う文
		TupleExpression, // 組を表す式
		TupleLike, // 組の値又は組を分解する模式
		TupleTypeComma, // 添字・型の組をカンマで区別する
		TypeCase, // 型で選ぶ切替の枝
		TypeModifiers, // 型の修飾子の並び
		TypeName, // 型の名前
		TypeQualifier, // 型の限定子
		ObservableQualifier, // 読取を観測可能にする限定子
		TypedDeclaration, // 型又は性質を伴う宣言
		UnaryOperation, // 単項の演算式
		UnaryValue, // 単項の値
		UnconditionalLoopKeyword, // 常真の条件を専用の反復語へ揃える構文
		UnescapedAttribute, // 引用符の変換で逃避を加えない属性
		ValueArguments, // 値の実引数の並び
		ValueParentheses, // 値の式を囲む括弧
		ValueStatementSequence, // 末尾値を持ち得る文の並び
		ValueSwitchRule, // 切替式の結果を与える枝
		VariableDeclaration, // 変数の宣言
		VariableDeclarator, // 変数を導入する宣言子
		VariableDeclaratorItem, // 変数の宣言子の項目
		WhileExpression, // 条件判定の反復式
		WhileLoop, // 条件を調べ続ける反復
		YieldValue, // 生成器が送り出す値
		// 中間表現の構築と配置
		AdjacencySensitiveInfix, // 隣接空白で演算子の読みが変わる中置式
		AlternateBodyLayout, // コロンと終端語で本体を囲む構文
		AmbiguousBodyContainer, // 型の本体とも文の本体とも為り得る容器
		AngleBracketList, // 山括弧で囲む型又は値の並び
		AnnotatedDefaultParameter, // 型注釈と既定値を持つ仮引数
		AnnotatedParameter, // 型注釈を持つ仮引数
		AnnotationConstructor, // 注釈として用いる構築子の呼出
		AnnotationUnionHost, // 型注釈として合併演算を含み得る構文
		AnnotationUnionOperator, // 型注釈内の合併を表す演算子
		ArgumentSpread, // 値を展開して渡す実引数
		ArrayLiteral, // 配列を値として作る構文
		ArrowToken, // 関数型又は枝の向きを表す矢印の字句
		AttachBracket, // 先行する構文へ角括弧を密着させる節点
		AttachColonOrDot, // 先行する名前へコロン又は点を密着させる構文
		AttachParen, // 先行する構文へ丸括弧を密着させる並び
		AttachedAccessor, // 直前の性質へ付属する読出又は書込の操作
		AttachedArrowLambda, // 矢印に仮引数を密着させるラムダ
		AttachedEmptyStatement, // 直前の構文へ付く空の文
		AttachedHashSplat, // 辞書の値を展開する記号と対象
		AttachedLabel, // 後続のコロンへ密着するラベル
		AttachedLowUnaryWord, // 結合の弱い単項の語と被演算子
		AttachedMagnitudeSuffix, // 数値へ大きさの接尾子を密着させる値
		AttachedNamespaceImport, // 名前空間の区切りを密着させる取込
		AttachedNegationTest, // 否定記号を検査の語へ密着させる式
		AttachedNumericUnit, // 数値と単位の接尾子を密着させる値
		AttachedPercent, // 数値へ密着する百分率の記号
		AttachedRangeOrUnary, // 範囲又は単項の演算子を被演算子へ密着させる式
		AttachedReferenceModifier, // 後続の対象へ密着する参照の修飾
		AttachedSelector, // 記号と名前を密着させる選択子
		AttachedSelectorNamespace, // 名前空間の区切りを密着させる選択子
		AttachedTypeQuery, // 型又は寸法を問う語と括弧を密着させる構文
		AttributeDeclaration, // 宣言へ付ける属性の指定
		AttributeSelector, // 属性の値又は有無で要素を選ぶ選択子
		AwaitCallHead, // 待機の語としても名前としても読める呼出の先頭
		BareCallArguments, // 括弧を省いて渡せる実引数の並び
		BaseReference, // 基底の実体を参照する式
		BinaryOperator, // 二項の演算子を持つ節点
		BlockArgument, // 呼出へ渡す塊の実引数
		BraceAttach, // 型名へ波括弧を直結する複合値
		BraceNoSpace, // 空の波括弧を型名へ密着させる型
		BracedConditionalValue, // 波括弧の枝を持ち値を返す条件式
		BracketType, // 角括弧で始まる配列等の型
		BracketedType, // 角括弧の内側に型を指定する構文
		BreakColonHost, // 演算・鍵値・見出の区切りとしてコロンを持つ構文
		BreakOpaque, // 内部の改行で意味が変わる不可分の字句列
		CaptureList, // 外側の変数を捕獲する指定の並び
		CastExpr, // 型への変換を表す式
		CastLookalikeOperator, // 型変換と同じ字面へ誤読し得る二項式
		CastTypeAtom, // 括弧内で型名に相当する単独の節点
		ChannelType, // 値の送受信路を表す型
		ClassBodyContainer, // 型の成員を直接並べる本体
		ClassLike, // 型又は名前空間を定義する容器
		ClassSelector, // クラス名で要素を選ぶ選択子
		CloserWord, // 語で開いた構文を閉じる終端語
		ClosureBlock, // 閉包として渡す塊
		ClosureBody, // 閉包の文を収める本体
		ClosureStatements, // 閉包の中に置く文の並び
		CodeOpeningTag, // 地の文からコードへ切り替える開始タグ
		CodeTextBoundary, // 地の文と埋込コードを区切る節点
		ColonBody, // コロンの後に文を並べる本体
		ColonBodyEnd, // コロンで開いた本体を閉じる語
		ColonBodyHost, // コロンを本体の開始に使う構文
		ColonControl, // コロンで見出又は節を区切る制御構文
		ColonPairHost, // 条件・継承・鍵値等をコロンで区切る構文
		CombinatorSelector, // 要素間の関係を指定する選択子
		Compact, // 内部の空白と改行を抑える構文
		CompactInterpolationOperator, // 埋込式内で演算子を密着させる式
		CompactRecurse, // 密着する構文の内で折返候補を持つ節点
		CompoundDeclarator, // 丸括弧で始まり得る複合宣言子
		ConditionalOperator, // 条件で値を選ぶ演算式
		ConstantConditionHost, // 常真又は常偽の条件を持ち得る分岐
		ContentTagBoundary, // 内容の始め又は終りを示すタグ
		ContinuationHeader, // 折返時に式の継続を示す見出
		Control, // 条件又は本体を持つ制御構文
		ControlKeyword, // 制御構文を始める語
		DeclarationDecorator, // 後続の宣言へ付ける修飾
		DeconstructionType, // 分解代入で型を推論する指定子
		DeferredLiteralBody, // 開始記号から離れた位置に在る文字列の本体
		DeferredLiteralStart, // 離れた位置の文字列本体を始める記号
		DelimitedMemberSequence, // 改行を区切りとして成員を並べる容器
		DelimitedStatementSequence, // 文の間を改行又は区切りで分ける並び
		DependentBranchLayout, // 後続の枝との位置関係で本体を配置する分岐
		DimensionNumber, // 単位を後置する数値
		DirectBraceLayout, // 波括弧を直下に持つ本体の構文
		DirectControlBraceLayout, // 波括弧を直下に持つ制御構文
		DirectiveBlankBoundary, // 前後を空行で区切る条件付の前処理指令
		ElementText, // 要素内に表示する本文
		ElseLikeClause, // 分岐又は例外処理に続く節
		EmbeddedOpeningBoundary, // 地の文の後で埋込コードを開く境界
		EmptyConditionBody, // 本体が空なら見出と終端語を繋ぐ条件分岐
		EmptyLoopBody, // 本体を省いても成立する反復
		EndDelimitedMethod, // 終端の語で閉じるメソッド定義
		EnsureSection, // 常に実行する後始末の節
		EnumeratorSequence, // 列挙項目を並べる容器
		ExceptionClause, // 例外を捕捉する節
		ExclamationMark, // 否定又は値の存在を表明する記号
		ExpandContainer, // 本体を含む時に子を改行展開する括弧の容器
		ExpandedTypeParameters, // 改行展開する型仮引数の並び
		ExportDeclaration, // 外部へ公開する宣言又は値
		ExpressionTag, // 属性を持ち得る開始又は自己完結のタグ
		FieldList, // 型の欄を並べる本体
		FunctionType, // 仮引数と戻値で表す関数型
		GeneratorStarHost, // 星の記号で生成器を指定する関数
		GenericArguments, // 総称化した構文へ渡す型引数の並び
		GenericFunction, // 型引数を伴う関数の構文
		GenericFunctionHead, // 型仮引数に先行する関数の見出
		GenericLookalikeOperator, // 型引数の閉じ記号に似る比較演算子を持つ式
		GroupedDeclaration, // 括弧で複数の宣言項目を束ねる宣言
		GroupedImport, // 括弧の内に取込対象をまとめる宣言
		GuardStatement, // 不成立時の本体を持つ条件文
		HardLineDirective, // 後続の構文と物理行を分ける指令
		IdSelector, // 識別名で要素を選ぶ選択子
		ImportItem, // 取込対象の一項目
		ImportItems, // 取込対象を並べる容器
		IndentContainer, // 子へ字下げを付ける本体又は容器
		Indexer, // 添字に依る成員参照を定義する宣言
		IndexerParameters, // 添字の仮引数を囲む並び
		InferredArrayConstruction, // 要素から配列型を推論する構築式
		InfixOp, // 左右を空白で区切る中置の演算又は結合
		InheritedClass, // 継承元を指定し得るクラスの定義
		InitHeader, // 初期化文を見出に含む条件又は反復
		InlineBraceBody, // 単一の文を波括弧の内へ同行で置ける本体
		IntegerMemberBoundary, // 成員参照の点と区切りを要する整数値
		InterpolatedLiteral, // 埋込式を含む文字列の値
		Interpolation, // 他の構文へ値を埋め込む部分
		InterpolationAlignment, // 埋込値の揃え位置を指定する節
		IterationSection, // 反復の本体を始める節
		KeyPath, // 成員への経路を値として表す式
		KeywordControlLayout, // 語で始め終える制御構文の本体
		KeywordExpandedContainer, // 文の塊を含む時に語の本体を展開する容器
		KeywordValueAssignment, // 語で閉じる本体を値に代入する式
		LabeledPrefix, // 本体に先行するラベルを持つ構文
		LeadingColonBody, // 先頭のコロンで本体を開く節
		LeadingTypeArguments, // 先頭の型引数を後続の要素へ連結する構文
		LiteralText, // 地の文を表す本文
		MacroLike, // 名前と後続の要素を不可分に繋ぐ構文
		MarkupSyntax, // タグと本文で表す文書の構文
		Member, // 点又は修飾記号で成員を参照する式
		MisparsedTemplateHost, // 比較式を型引数として誤読し得る構文
		NamedInfix, // 名前で表す中置演算を持つ式
		NamedKeyword, // 名前付節点として現れる構文上の語
		NamespaceLike, // 名前空間又は内部モジュールの定義
		NegatableTestWord, // 否定記号を前置出来る検査の語
		NestingSelector, // 外側の選択子を参照する記号
		NewParenCall, // 構築の語の後に実引数の括弧を置く構文
		NoDelimiterBreak, // 区切りの字句の後で折返さない構文
		NonKeywordClass, // 終端の語で閉じないクラス構文
		NormColon, // コロンの後を空白で区切る構文
		NumericUnitSuffix, // 数値へ接尾子を密着させる値
		ObjectLike, // 波括弧の内側に項目を置く値又は本体
		OpenRangeOperand, // 終端を省ける範囲式の被演算子
		OpenTailToken, // 後続の構文へ密着する記号
		OperatorFunctionDeclaration, // 演算子を名前として定義する関数宣言
		OperatorName, // 演算子を関数名として指定する構文
		OptionalChain, // 値の欠如を許して成員参照を連ねる構文
		OptionalInvocation, // 呼出又は添字へ任意連鎖の記号を密着させる式
		OptionalMarker, // 省略可能性を直前の構文へ付ける記号
		OptionalType, // 値の欠如を許す型
		OptionalTypeMember, // 名前に省略可能の記号を付ける成員又は仮引数
		PackExpansion, // 展開の記号 `...` を名前の前又は式・型の後へ付ける展開（C++ のパック展開）
		ParameterContainer, // 関数の仮引数を収める容器
		ParenLeadType, // 丸括弧で始まる型の構文
		PatternKind, // 値を分解又は束縛する模式
		PatternSection, // 模式との一致を調べる節
		PipeParamList, // 縦線で囲む仮引数の並び
		PointerDecl, // ポインター・参照・展開の記号を伴う構文
		PointerType, // 参照先の型を指定するポインター型
		PostfixAssertionMark, // 値の存在を後置で表明する記号
		PostfixOrUpdateExpression, // 後置演算又は値の増減を表す式
		PostfixPointerType, // 型名の後にポインター記号を付ける型
		PostfixReceiverChain, // 先頭の受け手に後置操作を連ねる式
		PrefixLabel, // 後続の構文へ先行するラベル
		PreprocConditional, // 前処理の条件分岐を始める指令
		PreprocDirectiveClose, // 前処理の後続枝又は終端を示す字句
		PreprocDirectiveOpen, // 前処理の条件又は後続枝を始める字句
		RangeAssignment, // 終端を省いた範囲を代入し得る式
		RangeLike, // 範囲を表す式又は模式
		RangeOperator, // 範囲の始端と終端を結ぶ演算子
		RecoverableTemplate, // 型引数と比較式の誤読を復元し得る構文
		ReferenceAssignment, // 参照先へ値を代入する式
		RenderedChildren, // 表示する子の要素を持つ容器
		RenderedText, // 要素の内側に表示する本文又は文字参照
		RescueSection, // 例外を受ける節
		ReturnTuple, // 戻値を組の型で表す関数又は型
		SelectorName, // 選択子の記号へ密着する名前
		SelfReference, // 現在の実体を指す式
		SeparatorToken, // 要素を区切る字句
		SignedOperandHost, // 符号を伴い得る被演算子を持つ単項式
		SizeofExpression, // 大きさを求める演算（C++ の `sizeof...` は記号を語と括弧へ密着させる）
		SpaceKeyword, // 後続の構文と空白で区切る語
		SpaceSkip, // 内部の空白を原文通りに保つ構文
		SpacedAttributeTag, // 属性同士を空白で区切る開始タグ
		SpacedControlBody, // 見出から空白で分ける制御本体
		SpacedFunctionHead, // 関数の見出と後続の括弧を空白で分ける宣言
		SpacelessBrace, // 波括弧の内側へ空白を挟めない式
		SpreadParameter, // 残りの引数を受け取る仮引数
		StatementList, // 文を順に収める容器
		StatementsOnlyBreak, // 見出を保ち文の並びだけを改行する節
		StringInterpolation, // 文字列へ埋め込む式又は名前
		StringSeqContainer, // 区切り無で文字列又はシンボルを並べる容器
		SwitchBodyLayout, // 枝の文を字下げして並べる切替の本体
		SwitchStatementLike, // 複数の枝を選ぶ切替文
		SwitchValue, // 複数の枝から値を選ぶ切替式
		TagElement, // 開始又は自己完結のタグを子に持つ要素
		TagSyntax, // 内容の境界を山括弧のタグで表す構文
		TemplateInterpolation, // 雛形の文字列へ埋め込む式
		ThenSection, // 条件の成立側の本体を始める節
		TightAnonymousForward, // 名前を省いた引数転送の記号
		TightHeaderEquals, // 見出の等号を前後へ密着させる構文
		TightParameterEquals, // 仮引数の既定値の等号を密着させる節点
		Transparent, // 字下げの深さを変えず子を展開する容器
		TypeAnnotation, // 対象の型を注記する構文
		TypeCombination, // 合併又は交差の記号で型を組み合わせる構文
		TypeSlot, // 型の構文を収める節点
		TypedLambda, // 型又は修飾を仮引数の前に置けるラムダ
		TypedSwitchLayout, // 値又は型で枝を選ぶ切替文
		UnaryPreOrUpdate, // 被演算子の前置演算又は値の更新
		UnaryPrefixToken, // 前置の単項演算を表す記号
		UnbrokenBaseReference, // 内部の改行を許さない基底の参照
		UnbrokenExceptionBinding, // 捕捉節の見出から分離出来ない例外の束縛
		UnbrokenImport, // 内部の改行を許さない取込の宣言
		UnbrokenOptionalType, // 省略可能の記号から型を分離出来ない構文
		UnbrokenParenSyntax, // 括弧又は語の内部で改行出来ない構文
		UnbrokenStringSequence, // 隣接する文字列を分離出来ない並び
		UnionType, // 複数の型から選べる合併型
		UnitText, // 単位を表す文字列
		ValueArgument, // 値を渡す実引数
		ValueArm, // 値を選んで結果にする分岐の枝
		ValueJump, // 制御移動又は値の受渡を行う式
		VariadicTypeParameter, // 可変長の型仮引数
		WordArray, // 語又はシンボルを並べて作る配列
		WordArrayClose, // 語の配列を閉じる記号
		WordArrayOpen, // 語の配列を開く記号
		// 静的検査
		AccessModifier, // 公開範囲を定める修飾子
		AccessOrderMember, // 公開範囲の順序を照合する成員
		AccessOrderedBody, // アクセス範囲を成員から読む本体
		AccessOrderedClass, // アクセス範囲の順序を調べる型
		AccessSection, // 公開範囲の区画の見出
		ImplicitPrivateClass, // 既定の公開範囲が private の型
		OptionalDeclarationType, // 明示出来る型を省いた宣言子の検査対象
		OptionalParameterType, // 明示出来る型を省いた仮引数の検査対象
		AddressOperation, // アドレスを取得する演算
		AlternativeClause, // 分岐の不成立側の節
		AssignableWrapper, // 代入先として読める式の包装
		AssignmentOperation, // 単純代入又は複合代入の式
		AutoParameterHost, // 仮引数の型を推論で省略出来る位置
		BoolFlipInspection, // 真偽値の反転候補を含み得る代入
		BoundaryComparison, // 言語横断の比較境界検査対象
		BuiltinNumericType, // 定数式に使える組込の数値型
		CastInspection, // 型付キャストへの置換候補
		ConcatenatedLiteral, // 文字列の暗黙の連結
		ConditionFieldHost, // 条件を子の役割で指定する構文
		ConditionInitializer, // 条件に先行する初期化の文
		ConditionMergeInspection, // 短絡条件を統合出来る分岐
		Conditional, // 条件と成立時の本体を持つ分岐
		ConditionalAlternative, // else の位置で自身が条件と本体を持つ分岐（else 節に包まれない else if の if・PHP の elseif 節）
		ConstantLiteral, // 定数式に使えるリテラル
		ConstexprInspection, // 定数式の宣言への置換候補
		ControlTransfer, // 制御を移す文
		CountExpression, // 長さ又は要素数を返す式の形
		DeclInitDeclarator, // 初期化子を伴う宣言子
		DeclLike, // 一括宣言出来る宣言
		DeclarationWithType, // 名前に型を付ける宣言又は仮引数
		DeclaratorWrapper, // 仮引数又は成員の宣言子の包装
		EmbeddedExpression, // 地の文へ埋め込む式
		EnumerationBody, // 列挙型の本体
		ErrorResult, // エラー値を返す型を持つ定義
		ExceptionBody, // 捕捉の節を持ち得る本体
		ExceptionContinuation, // 外側へ失敗を伝える捕捉・後始末の節
		ExceptionHandler, // 送出を受け止める節
		ExceptionTypes, // 捕捉する例外の型の並び
		FailureAnnotation, // 失敗を宣言する注釈列
		FieldDefinition, // 成員の型と宣言子
		FlowExit, // 返戻・脱出・跳躍・送出に依る制御の移動
		FunctionNameIdentifier, // 関数名として使う単純な識別子
		FunctionScope, // 関数の範囲の境界（定義・関数式・ラムダ・局所関数）
		GotoInspection, // 多重脱出の用途を調べる跳躍
		IdentifierLeafLike, // 名前の字面だけを持つ識別子
		ImplicitRaiseName, // 受け手無の例外送出の名前
		IncrementInspection, // 数値の１加減算候補を含み得る代入
		InstanceMethodDefinition, // メソッドの定義
		IntegerLiteralCandidate, // 整数リテラル葉
		JumpExpression, // 脱出又は送出の式
		JumpWithLabel, // ラベルを跳び先として持つ脱出の式（中のラベルは定義でなく参照）
		LabelInspection, // 多重脱出の用途を調べるラベル
		LabeledConstruct, // 跳躍先のラベルを持つ文
		LabeledLoop, // ラベルで脱出先を指定出来る繰返
		LiteralInterior, // 内部の字面を保つ文字列又はコメント
		MacroConstantInspection, // 定数マクロの検査対象
		MemberFieldRank, // 欄・プロパティ・定数・イベントの宣言（成員順位２）
		MemberDestructorName, // 成員順序の破棄子名
		MemberFriendRank, // 型との関係を定める友達宣言
		MemberMethodRank, // 成員関数・メソッド・コンストラクター（アクセス順位３）
		MemberStorageSpecifier, // 静的な成員の判定に使う記憶域指定
		MemberTypeRank, // 型別名とネスト型宣言（アクセス順位１）
		ModifierSequence, // 修飾子の並び
		MutableArgumentList, // 参照渡で書換の可能性を持つ実引数列
		MutatingExpression, // 変数の値を書き換える式
		NameDeclarator, // 名前と初期値又は型を包む宣言子
		NamedFunctionDefinition, // 言語横断の関数・メソッド定義
		NamedParameterKind, // 単一名を持つ仮引数
		NestedBreakTarget, // 外側の脱出先との区別を要する構文
		NullPointerInspection, // ヌルポインタの専用記法への置換候補
		NumericLiteral, // 数値を表すリテラル
		OrdinaryFunctionDefinition, // 通常関数（名前を持つ関数・メソッドの定義，コンストラクタ，デストラクタ，アクセサ）
		PlainVariable, // 型を構文から確定出来ない単純な変数
		PostfixInspection, // 後置増減の候補を含み得る更新式
		PrefixUnary, // 前置単項演算子
		RepeatedControl, // 条件又は列挙に依り本体を繰り返す構文
		RescueExpression, // 後置の例外回復式
		ReturnHintRequired, // 戻値の型注釈を要する定義
		ReturnStatement, // 明示的な返戻文
		ScopeContainer, // 関数・メソッド・プログラムの本体スコープ
		ScopeImport, // 他の範囲の名前を取り込む宣言（using の宣言・指令）
		SimpleTypeLeaf, // 名前だけで指定する単純型
		SingletonMethod, // 単一の受け手のメソッドの定義
		StrictEqualityInspection, // 数値の厳密等価を調べる演算
		StringLikeAll, // 言語横断の全文字列リテラル
		SwitchGotoTarget, // 値による分岐の選択肢への跳躍先
		ThrowKeyword, // 送出を示す語
		ThrowStatement, // 値の送出文
		ThrowValue, // 値の送出式
		ThrowsDeclaration, // 送出する例外の宣言
		TypeInferenceInspection, // 明示型の代替を調べる型
		TypeNameNode, // 宣言に型を指定する名前
		UnaryPrefixExpression, // 被演算子の前に演算子を置く単項式
		UnnameableInitializer, // 型名を書けない初期化子
		UnusedValueHost, // 式の結果を捨てる文の位置
		UserDefinedOperator, // 利用者定義の演算子名
		UserType, // 利用者定義の型
		VariableDeclarationStatement, // 変数の名前と初期値を導入する宣言
		ZeroComparisonInspection, // ０との比較の候補を含み得る構文
		ZeroConditionHost, // 暗黙の真偽変換を使える条件の所有者
		ZeroLiteral, // ０比較の対象に為る数値リテラル
		/** ========== 括弧・波括弧・宣言の正規化 ========== */
		AbstractArray, // 名前を持たない配列の宣言子
		AbstractFunction, // 名前を持たない関数の宣言子
		AbstractPointer, // 名前を持たないポインタの宣言子
		AlgebraicTypeOperandHost, // 型の被演算子を取る TS の型・式
		AlgebraicTypeParen, // 合併・交差・条件型の結合を指定する型の括弧
		AliasPattern, // 名前を束縛する照合
		AlternativePattern, // 選択肢を持つ照合
		Argument, // 実引数の包装
		AtomicConstruction, // 後置の受け手の原子に為る構築
		AtomicNumber, // 後置の受け手に置ける数値
		AttributeSensitiveNumber, // Python の数値リテラル (float / integer)
		AwaitNameReceiver, // 待機の語を名前と読んだ可能性の有る後置の受け手
		AwaitTryCastExpression, // `await` / `try`／型キャストの式
		AwaitValue, // 単項の待機値
		BindingCondition, // 条件の中の束縛
		BindingMarker, // 束縛の語の包装
		BindingName, // JS/TS の束縛の名前
		BlockEndingParen, // 塊での文終端と脱出値の境界を保つ括弧
		BlockExpressionStatementHost, // 文頭の式を持つ Rust の節点
		BodyAmbiguousControl, // 波括弧の本体と見出の値を区別する制御構文
		BodyAmbiguousHeader, // 本体に似た式を見出で保護する構文
		BorrowExpression, // 値を借用する式
		BorrowedTypeOperandHost, // 型の被演算子を取る Rust の型
		BorrowingMatch, // 照合の模式が対象を借用し得る式
		CallAttributeOrSubscript, // Python の後置の式 (attribute / call)
		CallableType, // 戻値の型の中で条件の型を再び許す TS の関数の型
		CalleeNeedsGrouping, // 括弧を外すと呼出の意味が変わる受け手
		CapturedPattern, // 照合の対象も名前へ捕捉する模式
		CasePattern, // 分岐の照合の包装
		CastAmbiguousParen, // 型変換と式の区別を保つ括弧
		CastBeforeUnary, // 後続の単項と読みを争う型変換
		CastExpression, // 型を左側に置く変換
		CastTypePart, // C/C++ キャスト `(type)` 内側に出現し得る型部位（丸括弧削除抑止対象）
		CatchTypeAlternatives, // 捕捉する型の選択肢
		ChannelReceive, // チャネルの受信を取り得る前置式
		ClosureExpression, // 捕捉を伴う無名関数の式
		ColonSensitiveInterpolation, // コロンを区切りとして読む補間
		CommandArgumentParen, // 括弧無の実引数と語の単項の結付を保つ括弧
		CommandCall, // 括弧無の実引数を取り得る呼出
		CommandStatementHost, // Ruby の文が直接置かれる容器— 値位置との判別
		CompactConstructor, // 仮引数を宣言から受け継ぐ構築子
		ComparisonInfix, // 字面から比較の優先順位を取得する中置式
		ComposedTypeCast, // 合成型を右側に取る型変換
		ComposedTypeOperandHost, // 型の被演算子を取る Swift の型
		CompositeTypeName, // 型名の形の Go の型
		ComprehensionFilter, // 内包表記の条件
		ComprehensionIteration, // 内包表記の反復対象
		ConditionalAccessChain, // C# の後置式の連鎖
		ConditionalAccessExpression, // 条件付の成員参照
		ConditionalAccessParen, // 任意の連鎖の境界を保つ括弧
		ConditionalExpression, // 条件と値の選択
		ConditionalTailOnly, // 最後の枝だけに同格の条件式を置ける条件式
		ConditionalType, // 条件に依って選ぶ型
		ConstructionReceiverParen, // 構築と呼出の受け手の区別を保つ括弧
		ConversionTypeParen, // 式の変換とチャネルの向きを保つ型の括弧
		DeclaratorHead, // 宣言の中で宣言子を始める節点
		DeducedTypeKeyword, // 型を推論する語
		DeducedValueCategoryName, // 括弧で推論する値の種類が変わる名前
		DeleteStatement, // 削除の対象を並べる文
		DelimitedIterationTarget, // 開き区切りの後に反復対象を置く見出
		ExprStmtLiteral, // expression_statement 直下の丸括弧削除を抑止するリテラル系（数値／文字列／真偽値）
		FieldExpression, // 欄を参照する式
		FinalizableLocal, // 不変の修飾子を追加出来る局所宣言
		FoldExpression, // 演算子で並びを畳込む式
		ForEachHeader, // 反復対象を区切語の後に置く見出（対象の括弧除去用）
		ForInStatement, // 束縛と反復対象を持つ反復
		GlobalStatement, // 最上位の文の包装
		GreedyTailExpression, // 右端を貪欲に伸ばすか，呼出時だけ括弧を要する式
		GreedyType, // 右端を貪欲に伸ばす TS の型
		GroupingTypeParen, // 型を包む括弧の節点
		HeaderBodyLikeValue, // 見出で本体の開きと紛れる値
		ImportStatement, // 外部の定義を取込む文
		InferredType, // 制約から推論する型
		InnerExpansionAttribute, // 所属する本体を変換し得る属性
		IntersectionType, // 交差の型
		InvocationExpression, // 名前又は成員の呼出
		IterationHeader, // 反復見出（対象前の `in`/`as` は演算子外）
		KeywordCastOperand, // 語の型への変換の値の囲みを省ける構文
		KeywordExpressionPrecedence, // 待機・後置・条件・否定の構文による優先順位
		KeywordUnary, // 語や記号で前置する単項の式— 演算子の優先順位は前置単項
		LogicalNegation, // 比較より弱く結合する否定
		LowKeywordPrecedence, // 語の論理演算と同じ最弱順位を持つ構文
		LowPrecedencePatternTest, // Ruby のパターン照合の式
		LowPrecedenceUnaryKeyword, // 代入より弱い Ruby の単項の語 (defined? / not)
		MemberExpression, // 受け手の成員を取得する式
		MemberMutationTarget, // JS/TS のメンバ・添字の参照
		MembershipExpressionBoundary, // JS/TS の `in` を二項演算子に戻す（for 文の初期化子の外に為る）式（arguments / 計算の鍵 / 欄 / 対 / 括弧 / 塊 / 添字 / 補間）
		MutablePattern, // 可変として束縛する照合
		MutablePatternBinding, // 可変の指定を除去出来る模式の束縛
		MutableSpecifier, // 可変の指定
		MutationBaseTraversal, // Rust の mut 削除候補から基底名を辿る節点
		NamedCompositeBody, // 型名を伴うと本体の開きと紛れる複合値
		NamedInfixCall, // 固定の優先順位を持つ名前の中置式
		NamedValue, // 名前を束縛して値を得る式
		NavigationSuffix, // 成員への航行の接尾辞
		NestedLoopBodyBraces, // 入れ子の反復本体の波括弧を要する後判定の反復
		NonNullableType, // 非 null の型の合成
		NormalFormType, // 選言標準形の型
		NormalizedBindingName, // 正規化に依って同一名の照合が変わり得る束縛名
		NullableTypeOperandHost, // 型の被演算子を取る Kotlin の型
		OpenPrefixExpression, // 語で始まり右の被演算子を式の終わり迄取る式（`yield` / `include` / `print`／ラムダ／`return` 等）— 演算子の優先順位は最も弱い
		OperatorBoundaryParen, // 単項・範囲・待機の記号の境界を保つ括弧
		OperatorCall, // 演算子を呼出側に持ち得る呼出
		OptionalConditionParen, // 条件の囲みを省ける見出
		OptionalNavigationMark, // 任意の航行の接尾辞
		OptionalValueJump, // 値を取り得る Rust の脱出の式
		OuterExpansionAttribute, // 後続の宣言を変換し得る属性
		ParameterScope, // Java の仮引数スコープを開く節点
		ParameterScopeBoundary, // Java の型の本体
		ParenInnerFirstChildNoUnwrap, // 内側の最左の子孫が此の型なら括弧を剥がさない（Ruby の多重代入の左辺・Python の条件式を値に取るセイウチ演算子）
		ParenInnerNoUnwrap, // 即時実行・コンマ式・貪欲式等，丸括弧を外せない式
		ParenValueSlot, // 括弧が値の全体を包み後に字句が続かない値の位置（代入の右辺・実引数・戻値・要素・仮引数の既定値）
		Pattern, // 値を照合する模式
		PostfixAwait, // 後置の待機
		PostfixCallHost, // 被呼出側を最初の子に持つ呼出
		PostfixExpressionHost, // 式を左端の受け手とする呼出・添字・成員参照
		PostfixReceiverLeaf, // 括弧無で後置の式の受け手に置ける名前・字句で閉じるリテラル
		PostfixTry, // 後置の試行
		PostfixValueOperation, // 左端の式を受け手とする値の演算
		PrefixAbsorbsInfix, // 中置の被演算子から結合を取得する前置式
		PrefixAwait, // 前置の待機
		PrefixExpression, // 前置の演算子を持つ式
		ProtocolComposition, // 適合条件を合成した型
		RangeExpression, // 両端又は片端を指定する範囲
		RangeOrUnaryExpression, // Kotlin の増減式及び Swift の範囲／単項演算子親
		RangeWithoutEnd, // 終点の無い範囲
		RangeWithoutStart, // 始点の無い範囲
		ReceiverMutationCall, // 受け手の基底の束縛を変更し得る呼出
		RecordDeclaration, // 成員の仮引数を持つ記録型の宣言
		ReferenceBindingPattern, // Rust の参照・フィールドのパターン
		ReferenceModifier, // 参照として扱う指定
		ReferencePattern, // 参照を束縛する照合
		ReferenceValueParen, // 型・条件・参照返戻の読みを保つ括弧
		RemovablePatternParen, // 単一の照合を囲む括弧を省略出来る位置
		RemovableTypeParen, // 単一型を包む時に省略可能な型の括弧
		ReopenableTypeCast, // 逆側の同格演算子迄型を伸ばし得る変換
		ReplaceableBindingKeyword, // 束縛の語を不変の語へ置換出来る宣言
		RestrictedExpressionOperand, // 条件・数値の属性・待機の被演算子に制約の有る構文
		ReturnValueHost, // return の返す値の参照の有無を決める関数（C++ の decltype(auto) / PHP の参照返し）
		ReturnValueParent, // 返す値を直接の子に持つ節点
		SequenceExpansion, // 並びの展開
		StatementEndingBlock, // 文頭に置くと `}` で文を終える Rust の塊の式
		SwitchOrWithExpression, // switch 式及び C# の with 式
		SwitchWithPrecedence, // 呼出・値分岐・複製の優先順位を持つ式
		TernaryExpression, // 条件と二つの枝を持つ式
		TrailingTypeCast, // 型を右側に置く変換
		TransparentTupleParen, // 単一の式を包む透過可能な組の括弧
		TupleParameterFunctionType, // 組の括弧で仮引数を表す関数型
		TuplePattern, // 組の要素を照合する模式
		TupleTypeItem, // 組の型の要素
		TupleValue, // 組の値
		TypeArgumentAmbiguousParen, // 比較と型引数の区別を保つ括弧
		TypeArgumentLookalikeSlot, // 比較式が型引数列に見え得る一覧要素の位置
		TypeBounds, // 型の制約の並び
		TypeCastRight, // 型を右に取る変換の式
		TypeColonParent, // `:` の右を値・型とする鍵値対・型の欄
		TypeConversion, // 型を指定した式の変換
		TypeElements, // 型集合の要素
		TypeList, // 型の並び
		TypeOperatorInner, // 演算子を持つ型
		TypePackExpansion, // 型の並びの展開
		TypeSwitch, // 型で分岐する文
		UnambiguousConversionType, // 括弧無で型の変換に置ける Go の型
		UnaryReceiverCast, // 前置単項を受け手に取れる型変換
		UnwrappedSwitchSubject, // 対象の括弧を省ける値分岐
		UpdateExpression, // 前置又は後置の増減
		ValueBindingPattern, // 値を束縛する照合
		ValueCategoryType, // 式の値の種類を調べる型
		WrappedTypeItem, // 型の要素の包みを持つ型の括弧
		WritableAssignment, // Rust 代入式系
		// 入力側の読取（整形仕様 4.3，文法の定義が節点名から与え，入力側と検証側が読む）
		AdjacencyRecordingHost, // 子の間の原文の隣接が読みを決める節点
		AdjacencyRecordingOperand, // 前の子との原文の隣接が読みを決める子
		AmpersandToken, // 参照又は論理積の字句
		AngleCloseCandidate, // 型引数の閉じと紛れる字を含む字句
		AngleCloseToken, // 閉じの山括弧で始まる字句
		AngleConflictToken, // 型引数と比較の読分けに関わる山括弧を含む字句
		ArgumentListHost, // 実引数の並びを子に持つ節点
		ArgumentStartToken, // 名前の後に空白を置いて続けると実引数の始まりに為る字句
		ArrayRank, // 配列型の次元指定
		ArrayTypeSyntax, // 配列型の構文
		ArrowFunctionNode, // 矢印で本体を指定する関数
		AssignmentToken, // 等号の字句
		AsteriskToken, // 乗算又は間接参照の星印の字句
		AtToken, // 注釈又は参照を始める字句
		AttributeOpenToken, // 属性を開く番号記号と角括弧の字句
		BackslashToken, // 逆斜線の字句
		BinaryPrecedenceExpression, // 優先順位に従って結び付く二項式
		BindingExpression, // 名前を束縛する式
		BlockCommentNode, // 囲みコメントの節点
		BlockParameterList, // 塊の仮引数の並び
		BodyDelimiterLead, // 見出の値の内側を区切る先頭記号
		BodyEndingClause, // 最後の子に本体の文を持つ節
		BodyTriggerWord, // 制御の本体を始める語
		BracedHeaderStatement, // 見出の後に本体の波括弧が続く文
		BracedStatementBlock, // 波括弧で文を並べる塊
		CallableReference, // 呼出可能な対象への参照
		CastContinuationToken, // 型変換の後へ続く単項記号の字句
		CastOperandLead, // 型変換の被演算子と紛れる先頭記号
		CastUpdateFollowerLead, // 更新式の後で型変換と紛れる先頭記号
		ChangeByOneToken, // 値を一つ増減する字句
		ChannelReceiveToken, // 通信路から値を受ける演算子の字句
		ClassMember, // 型の本体の成員
		ClauseHeading, // 節を始める見出（言語を問わない，コメントの所属の判定に使う）
		ClosedValueTerminator, // 値の位置を閉じる複合区切り
		ClosingBraceLead, // 閉じ波括弧で始まる字句
		ClosingBraceToken, // 閉じ波括弧の字句
		ClosingBracketLead, // 閉じ括弧で始まる字句
		ClosingParenLead, // 閉じ丸括弧で始まる字句
		ClosingParenTail, // 丸括弧を閉じる記号で終わる字句
		ClosingParenToken, // 丸括弧を閉じる字句
		ClosingSquareLead, // 角括弧を閉じる記号で始まる字句
		ClosingSquareToken, // 角括弧を閉じる字句
		ClosingTagOpenToken, // 閉じタグを始める字句
		CoalescingToken, // 欠損値を選ぶ二重疑問符の字句
		ColonLead, // 節又は欄の区切りで始まる字句
		ColonToken, // 節又は欄を区切るコロンの字句
		CommaLead, // 並びの区切りで始まる字句
		CommaTail, // 並びの区切りで終わる字句
		CommaToken, // カンマの字句
		CommandArgumentExtent, // 命令形の呼出の実引数が左端から続く式
		CommandArgumentHost, // 命令形の呼出の実引数の読みを揃える節点
		CommentKeepingHost, // コメントを分離せず原文の儘残す節点
		CommentNode, // コメントの節点
		CompositeLiteral, // 複合値のリテラル
		ConditionBodyDelimiterToken, // 条件と本体を分ける字句
		ConditionalOperatorExpression, // 真側を区切りの記号で閉じる条件式
		ConstantDeclarationNode, // 定数の宣言節点
		ConstantSpecification, // 定数の宣言項目
		ContentElement, // 開始タグと終了タグで内容を囲む要素
		ContinuingWord, // 後に続く字句を要し，後の改行で文を終えない語
		DeclarationExpression, // 宣言を値として扱う式
		DelimitedList, // 括弧で囲んだ実引数・仮引数・要素の並び
		DelimitedLiteral, // 区切り文字付のリテラル
		DocumentTypeDeclaration, // 文書型の宣言
		DocumentedTopDeclaration, // 直前のコメントを文書とする最上位の宣言
		DotToken, // 点の字句
		DoubleColonToken, // 二重コロンの字句
		DoubleSquareCloseToken, // 二重角括弧を閉じる字句
		DoubleSquareOpenToken, // 二重角括弧を開く字句
		ElementSequence, // 名前の後の実引数が残りの要素を取り込む要素の並び
		EmbeddedCodeElement, // 別の言語のコードを中身に持つ要素
		EmbeddedScriptElement, // 埋込の実行コードを持つ要素
		EmptyParenthesesLead, // 空の丸括弧で始まる字句
		EnumerationItem, // 列挙の項目
		ErroneousEndTag, // 誤読された終了タグ
		EscapeLiteralHost, // 字句化前の逃避を内側に持つリテラル
		ExclamationToken, // 感嘆符の字句
		ExhaustiveCaseLabel, // 切替の枝のラベルに置くと処理系が切替へ網羅を要求する照合と値
		ExhaustiveSwitch, // 網羅を処理系が保証するか不一致で送出する切替
		ExponentToken, // 冪を表す演算子の字句
		ExpressionContinuationLead, // 前の式へ続く演算子・参照の記号で始まる字句
		ExpressionContinuationTail, // 後の式へ続き文を終えない記号で終わる字句（後置の増減を除く）
		ExpressionTypeName, // 式としても読める型名
		FatArrowToken, // 右向きの太い矢印の字句
		FollowingBodyClause, // 閉じの波括弧に続く本体付の節
		FunctionDeclarationNode, // 関数の宣言節点
		FunctionLiteral, // 関数の値
		GreaterThanToken, // 大なり記号の字句
		HashToken, // 番号記号の字句
		HeaderExpressionLead, // 見出の式の前の字句
		HeredocAmbiguousShift, // 複数行文字列の開始と紛れるシフトの字句
		HeredocBody, // 複数行文字列の本体
		IfFollowingBranch, // if の後続の枝の節
		IfStatementNode, // 条件分岐の文節点
		ImportDeclarationNode, // 取込の宣言節点
		ImportLeaf, // 取込の表現の葉
		IncludeStatementNode, // 取込の文節点
		InstantiationContinuationLead, // 具体化した式に続くと解釈を変える先頭記号
		IntegerLiteralValue, // 整数の値の節点
		InterpolationFormat, // 補間の書式指定
		InterpolationFormatSeparator, // 補間の書式区切りと紛れる字句
		InterpolationOpaqueLead, // 補間内で区切りを隠す値の先頭記号
		InterpolationOpenToken, // 埋込式を始める字句
		IsolatedScope, // 外の局所変数を見ない有効範囲を開く節点
		IterationBodyDelimiterToken, // 反復と本体を分ける字句
		LabelName, // ラベルの名前
		LaggingGrammarError, // 文法の追随遅れで正当な構文にも立つ誤り区画
		LeadingBreakToken, // 字句の前で折り返す閉じ又は参照の記号
		LeftAssignmentList, // 代入の左辺の並び
		LessThanToken, // 小なり記号の字句
		LineCommentNode, // 行コメントの節点
		LineContinuation, // 次の行へ文を続ける行継続
		LineJoiningSuffix, // 改行を跨いで前の式へ繋がる接尾辞・参照・中置式・返戻値
		LineTerminatedContext, // 改行が文を終える文脈
		LocalParameters, // 局所変数を導入する仮引数の並び
		LocalScope, // 局所変数の有効範囲を開く節点
		LowPrecedenceBinaryKeyword, // 命令形の呼出の実引数の外に在る語の二項演算子
		MacroBody, // 行末迄の字句を１つに呑むマクロの本体
		MacroDefinition, // 置換本体を持つマクロの定義
		MacroGuardedParentheses, // マクロの展開で意味が変わり得る値の括弧
		MacroNameQuery, // 名前がマクロかを問う前処理の条件
		MacroRuleDefinition, // 規則で字句列を書き換えるマクロの定義
		MacroRulesToken, // マクロ規則を始める字句
		MarkupDeclarationOpenToken, // 文書宣言を始める字句
		MarkupElementNode, // 開始タグと中身を持つ要素
		MeaningfulParenGroup, // 型・値・模式の意味を定める括弧の群
		MemberAccessArrow, // 成員参照の矢印の字句
		MemberAccessLead, // 成員参照の記号で始まる字句
		MemberAccessTail, // 成員参照の記号で終わる字句
		MetatypeNode, // 型自身を指す型
		ModuleRule, // モジュールを読む規則
		ModuleStatement, // モジュールの印の文
		NameDeclaration, // 宣言子を名前迄剥がして名前を得る宣言
		BareTypeDeclaration, // 裸の名前を型として導入する宣言
		PotentialExpressionDeclaration, // 式としても読める型付の宣言
		NameToken, // 比較用の名前を投影する名前・ラベルの字句
		NamespaceName, // 名前空間の名前（宣言と修飾の名前）
		NestingDeclarator, // 宣言子を包む宣言子
		NonNullAssertionToken, // 値の非空を表明する字句
		NonNullExpression, // 値の非 null 表明の式
		ObjectLiteralNode, // 物体の値の構文
		OpaqueLiteral, // 中身をコードと読まない文字列・正規表現のリテラル
		OpenRangeToken, // 終端を省ける範囲の二点の字句
		OpeningAngleLead, // 山括弧を開く記号で始まる字句
		OpeningBraceLead, // 波括弧を開く記号で始まる字句
		OpeningBraceTail, // 塊を開く波括弧で終わる字句
		OpeningBraceToken, // 本体又は容器を開く波括弧の字句
		OpeningBracketLead, // 囲みを開く記号で始まる字句
		OpeningBracketTail, // 開き括弧で終わる字句
		OpeningBracketToken, // 開き角括弧の字句
		OpeningParenLead, // 丸括弧を開く記号で始まる字句
		OpeningParenToken, // 開き丸括弧の字句
		OpeningSquareLead, // 角括弧を開く記号で始まる字句
		OperandHeadedAccess, // 被演算子を先頭の子に取る参照・呼出
		OperatorPrecedenceBoundary, // 演算子の優先順位が結付を区切る式
		OptionalAccessToken, // 省略可能な成員参照の字句
		OptionalArrowToken, // 省略可能な右向きの矢印の字句
		ParameterRule, // 仮引数の並びを持つ規則
		PhpEndTag, // PHP の終了タグ
		PipeLead, // 選択又は論理和の記号で始まる字句
		PipeToken, // 論理和又は選択を表す縦線の字句
		PlaceholderNode, // 空欄を表す節点
		PlainValueWord, // 値の中の裸の語
		PointerMemberAccess, // 成員へのポインタを辿る演算子
		PostfixBindingExpression, // 後置の演算が被演算子へ結び付く式
		PostfixBindingOperand, // 後置の演算を右端へ結び直す被演算子の式
		PragmaTargetFunction, // 関数への指示子を受ける関数の宣言
		PrefixUnaryOperation, // 前置の単項演算式
		PreprocessorCall, // 前処理の呼出指令
		PreprocessorFunctionDefinition, // 関数形式の前処理定義
		PreprocessorInclude, // 前処理の取込指令
		PreprocessorNode, // 前処理の指令と其の部品
		ProductAmbiguousPointerType, // 乗算と読み違え得るポインタ型
		PropertyNameNode, // 性質の名前の節点
		QualifiedNameNode, // 修飾を伴う名前の節点
		QuestionLead, // 疑問又は選択の記号で始まる字句
		QuestionToken, // 疑問符の字句
		QuoteToken, // 文字列を開閉する引用符の字句（接頭辞付の物を含む）
		RawTextNode, // 逐語に保つ本文の節点
		ReassigningShortDeclaration, // 既存の束縛へ代入し得る短い宣言
		ReceiveStatement, // 通信を受け取る文
		ReceiverParenHost, // 受け手を包む括弧が後置の結合を保つ式
		RegexLiteralNode, // 正規表現のリテラル節点
		RestrictedLineWord, // 後に改行を挟めない語
		ReturningStatement, // 値を返す文
		RightArrowToken, // 右向きの細い矢印の字句
		SafeAccessToken, // 安全な成員参照の字句
		ScopedIdentifier, // 有効範囲を指定した識別子
		SelectorList, // 選択子の並び
		SelectorRule, // 選択子を持つ規則
		SelfClosingTag, // 自己終端するタグ
		SelfClosingTagToken, // 要素を自身で閉じるタグの字句
		SeparatorLead, // 並び又は文の区切りで始まる字句
		SequentialStatements, // 逐次実行する文を直に並べる節点
		ShortAssignmentToken, // 短い代入の字句
		ShortVariableDeclaration, // 短い記法の変数宣言
		SlashToken, // 斜線の字句
		SourceCopiedNode, // 子から組み直さず原文を写す節点
		SourceFileNode, // 原文ファイルの根節点
		SourceLeaf, // 中身を組み直さず原文の字面を其の儘使う節点
		SpacelessTypeName, // 字句の間に空白を置かない型の名前
		SpacingSensitiveInfix, // 前後の空白で前置・後置・中置が変わる演算子の中置の式
		SpacingSensitiveRange, // 前後の空白で読みが変わる範囲の式
		SplitBindingHead, // 束縛の語の後の改行で解析器が分けた次の文の最初の名前
		SpreadToken, // 値の展開を表す三点の字句
		StartTag, // 開始タグ
		StatementOrMemberSequence, // 文・成員を並べる節点
		StatementTerminatorLead, // 文を終える区切りで始まる字句
		StatementTerminatorTail, // 文を終える区切りで終わる字句
		StatementTerminatorToken, // 文を終えるセミコロンの字句
		StrayBlock, // 見出の文の直後の同じ行の `{` で始まる塊と読み違えた節点
		StringLiteralNode, // 文字列リテラルの節点
		StructExpression, // 構造体の値の式
		StructuralParenBoundary, // 見出と本体の構文上の区切り
		SubscriptExpressionNode, // 添字に依る参照の式
		SuperReference, // 親の実体への参照
		TagNameNode, // タグの名前の節点
		TemplateInstantiationType, // 型引数を伴う型の実体化
		TemplateScanBoundary, // 型引数の読違の遡りを止める範囲
		TildeToken, // 波線の字句
		TrailingBreakToken, // 字句の後で折り返す開き又は区切りの記号
		TrailingDataSection, // 解釈しない末尾のデータ区画
		TrailingOperatorToken, // 先頭以外で後続を折り返す演算記号
		TransparentWrapper, // 錨を中身へ移す透過の包装
		TypeAliasStatement, // 型の別名を宣言する文
		TypeArgumentOpeningLead, // 型引数の開始と紛れる被演算子の先頭記号
		TypeParameterScope, // 型引数を導入する宣言の包装
		UnaryPrefixLead, // 前置単項の記号で始まる字句
		UnicodeEscapeHost, // 字句化前の Unicode の逃避を中身に持つ字句
		UnsupportedComment, // 記法の登録に無いコメント
		ValueDiscardingAssignment, // 値を使わない文の形の代入
		ValueTakingJump, // 空白に依らず後続を値と読む脱出の語
		VariableDeclarationNode, // 変数の宣言節点
		VariableSpecification, // 変数の宣言項目
		VariableSpecificationList, // 変数の宣言項目の並び
		VariadicParameterDeclaration, // 可変個の仮引数の宣言
		VerbatimQuotedString, // 字面を保つ引用文字列
		VerbatimSubtree, // 内部の構造を保ち原文の字面を返す節点
		WhenCondition, // 条件選択の条件
		WhereClauseNode, // 制約を表す節
		WildcardToken, // 照合で全ての値に一致する読捨の字句
		Count // 役割の数（番兵）
	}; // 構文の役割（整形仕様 3.2, 4.3，文法の節点名に依らない共通の種別と性質で，投影側が文法の記号毎に付ける）

	using RoleSet = std::bitset<static_cast<size_t>(Role::Count)>; // 役割の集合

	enum class ChildRole : uint8_t {
		None,
		Alias,
		Alternative,
		ArgumentField,
		Arguments,
		BlockField,
		Body,
		Communication,
		Condition,
		Consequence,
		Declarator,
		DirectiveField,
		Exceptions,
		Function,
		FunctionField,
		Guard,
		IfTrue,
		Increment,
		Left,
		Macro,
		MethodField,
		Name,
		Object,
		ObjectField,
		Operand,
		Operator,
		OperatorField,
		Other,
		Parameters,
		ParametersField,
		Path,
		Pattern,
		ReceiverField,
		Result,
		ReturnType,
		Right,
		Superclass,
		Target,
		Type,
		Update,
		Value,
		Variable,
		Count
	}; // 親の中で子が担う役割（投影時に文法の場を此の役割へ対応付ける）

	static_assert(static_cast<uint8_t>(ChildRole::Count) < 65); // 子の役割は１語の集合に収める

	enum class FormOperation : uint8_t {
		ArglessReturn, // 値の無い返戻を組み立てる操作
		BraceBody, // 制御本体を波括弧の塊で包む操作（部品は塊・`{`・`}`・塊の中の文の並びの順で，文の並びを持たない塊は３個）
		Continuation, // 改行を跨ぐ式を合法にする括弧を展開時だけ補う操作
		ExplicitTailReturn, // 末尾値を返戻の語と実引数の並びで包む操作
		GeneratedLexemes, // 組立器と正規化器が生成する固定字句を定める操作
		OmitEmptyArguments, // 空の実引数を除き，文法上の結果種別へ組み直す操作
		PostfixCondition, // 単一文と条件の結付を保って後置形式へ組み直す操作
		ReturnWithZero, // 零を返す返戻を組み立てる操作
		TrailingComma, // 展開した並びの閉じの直前へ区切りを置く操作
		UnconditionalLoop // 真の条件を持つ反復を文法の節と場へ組み立てる操作
	}; // 生成形式の共通操作（整形仕様 3.2）

	struct KindDefinition;

	enum class FormPartRole : uint8_t {
		Arguments,
		Block,
		BlockClose,
		BlockOpen,
		Body,
		CallClose,
		CallOpen,
		Close,
		Condition,
		IntegerValue,
		Keyword,
		LastElement,
		Open,
		Result,
		Separator,
		Statements,
		Terminator,
		TrueValue,
		ValueDelimiter
	}; // 生成形式の部品が書換後の構文で担う役割

	struct FormPart {
		FormPartRole Role = FormPartRole::Body; // 生成後の子の役割
		const KindDefinition *Definition = nullptr; // 生成する種別（既存の子は無し）
		std::string_view FixedLexeme {}; // 子を持たない生成字句の字面
		FormPartRole Parent = FormPartRole::Result; // 生成後の直属の親の役割
		ChildRole Field = ChildRole::None; // 生成後の親に於ける場
		bool IsNamed = false; // 生成する節点が名前付か
	};

	enum class FormCondition : uint8_t { BrokenListWithLastElement, EmptyArguments, UnbracedBody }; // 生成形式を選べる構文上の状態

	// 生成形式（整形仕様 3.2，共通操作と部品の役割・種別・固定字句の定義で，原文・言語別の整形関数を持たない）
	struct CanonicalForm {
		FormOperation Operation = FormOperation::BraceBody; // 共通操作
		std::vector<FormPart> Parts {}; // 生成又は移す子と其の構文上の役割
		std::string_view FixedLexeme {}; // 共通操作が生成する固定字句（該当する形式だけ）
		RoleSet ApplicableRoles {}; // 形式を使える所有者の構文上の役割
		FormCondition Condition = FormCondition::UnbracedBody; // 形式を選べる構文上の状態
		std::string_view ClosingLexemes {}; // 展開した並びを閉じる字句（該当する形式だけ）
		const FormPart *PartOf(const FormPartRole Role) const; // 役割に対応する部品の取得関数
		bool AppliesTo(const RoleSet &Roles) const; // 所有者の役割に適用出来るかの判定関数
		bool CanWrapBody(const bool IsBraced) const; // 本体を包装出来るかの判定関数
		bool CanAddTrailingComma(const bool IsBroken, const bool HasLastElement, const char Closer) const; // 末尾のカンマを生成出来るかの判定関数
		bool CanOmitEmptyArguments(const bool IsEmpty) const; // 空の実引数を省けるかの判定関数
	};

	// 文法の定義が所有する変更不能な生成形式の並び（節点毎に持つ為，記号毎の定義の並びへの参照だけを持つ）
	class CanonicalFormList {
	private:

		const std::vector<CanonicalForm> *Definitions = nullptr; // 記号毎の生成形式の定義（無ければ空）

	public:

		std::span<const CanonicalForm> View() const; // 生成形式の並びの取得関数
		std::span<const CanonicalForm>::iterator begin() const; // 生成形式の並びの先頭の取得関数
		std::span<const CanonicalForm>::iterator end() const; // 生成形式の並びの終端の取得関数

		explicit CanonicalFormList(const std::vector<CanonicalForm> &Forms); // コンストラクタ（記号毎の定義を参照）

		CanonicalFormList() = default; // コンストラクタ（空）
	};

	// 使用可能な記法（整形仕様 3.2，入力の記法の識別と出力するコメント行の列の字句の規則に用いる）
	struct AvailableNotation {
		CommentNotation Notation; // 記法
		bool IsTabSeparated; // 名前迄を開始記号に含む指示記法の名前の後の区切りにタブも読むか
	};

	// 同義の文書札の対応（整形仕様 7.4，入力の札を採用する札へ揃える）
	struct DocumentTagSynonym {
		std::string_view From; // 入力の札
		std::string_view To; // 揃える札
	};

	enum class OmittedResultNotation : uint8_t { Inferred, Integer, Unit }; // 型を省いた関数の戻値（本体から推論する，整数を返す，単位型を返す）
	enum class NameCase : uint8_t { Unrestricted, LowerInitial, NoUppercase, UpperInitial }; // 静的検査が求める名前の大小文字の条件
	enum class InferredTypeNotation : uint8_t { None, DynamicTop, Implicit, Named, Placeholder }; // 静的検査が照合する推論型又は動的な最上位型の記法
	enum class DocumentationStyle : uint8_t { AtTag, Callout, Docstring, MarkdownHeading, Prose, XmlTag }; // 文書の項目の記法

	enum class DocumentFormId : uint8_t {
		Doxygen,
		Godoc,
		JSDoc,
		Javadoc,
		JavadocMarkdown,
		KDoc,
		PHPDoc,
		Rustdoc,
		SwiftMarkup,
		XmlDocumentation,
		Yard
	}; // 本文の解釈を決める文書の形式（整形仕様 3.2，囲み記号だけで識別しない）

	enum class CommentUse : uint8_t { Plain, Document, Separator }; // 出力記法を選ぶコメントの用途（整形仕様 7.6，`Plain` は通常の独立・行末コメント，`Document` は文書，`Separator` は区切コメント）

	// 用途毎の採用記法（整形仕様 3.2，出力文書形式は採用記法の入力の文書形式と同じ）
	struct CommentOutputForm {
		CommentUse Use; // 用途
		CommentNotation Notation; // 採用記法
		std::string_view GluedHead; // 開始記号へ密着させる本文の頭（開始記号の綴りを持たない記法を記号の連続で表す）
	};

	enum class Word : uint8_t {
		AlternateBranch, // 条件分岐の代替枝を始める語
		AsyncModifier, // 非同期の関数を示す語
		AsyncVoidType, // 値を返さない非同期の関数の戻値の型
		AwaitName, // 待機の演算子として読む名前
		BareCall, // 空の実引数の括弧を省ける呼出の名前
		BareCapture, // 引数の末尾で受け手を捕捉する記号
		BaseReference, // 基底の型の成員を指す語
		BlockEnd, // 語で始めた塊を閉じる語
		Boolean, // 真偽値の組込の型
		Case, // 切替の枝を始める語
		Cast, // 型の変換の語
		CatchBinding, // 捕捉した値を束縛する語
		CatchFilter, // 捕捉を条件で絞る語
		CatchHead, // 捕捉の節を始める語
		CatchQualifier, // 捕捉の見出で型の前に置く修飾の語
		Channel, // 通信路の型を始める語
		ClassHead, // クラスを宣言する語
		ConditionThen, // 条件と本体を分ける語
		ConstQualifier, // 不変の型修飾の語
		ConstantCondition, // 定数式の条件を示す語
		ConstantDeclaration, // 定数の宣言に用いる語
		ConstexprQualifier, // 定数式の修飾の語
		Construct, // 生成の語
		ConstructionMethod, // 実体の生成を指示する語又は操作の名前
		Default, // 既定の枝を始める語
		DeferredInitialization, // 初期化を後へ延ばす修飾の語
		Define, // 定義を始める語
		Delegation, // 生成器の委譲を示す語
		DiscardName, // 束縛を捨てる名前
		DocumentAttributeName, // 文書を表す属性の名前
		DocumentableCall, // 文書を付けれる宣言として働く呼出の名前（属性の定義）
		DynamicScope, // 名前の解決を実行時に変え得る呼出の名前
		DynamicTopType, // 動的な最上位の型名
		EntryFunction, // 処理系が起動点とする関数の名前
		EntryResultType, // 起動点の関数の戻値の型
		ErrorResultType, // 失敗を戻値として表す型名
		ExpressionBody, // 関数の式の本体を導く字句
		Fallthrough, // 次の枝へ進む語
		False, // 偽の値の名前
		FloatingType, // 浮動小数点の型名
		FunctionHead, // 仮引数又は受け手の括弧を直後に置く関数の語
		FunctionScopedBinding, // 関数の有効範囲の束縛を宣言する語
		Getter, // 値を読む操作の名前
		ImmutableBinding, // 不変の束縛を宣言する語
		ImplicitRaise, // 受け手の無い送出の呼出の名前
		In, // 所属又は反復の対象を示す語
		InferredType, // 型を推論する型名
		InlineStyleAttribute, // 要素の表示規則を指定する属性の名前
		IntegerAlias, // 標準の整数の型の別名
		IntegerTruth, // 整数で表す真の値
		IntegerZero, // 整数で表す偽の値
		IterationDo, // 反復の条件と本体を分ける語
		IterationOf, // 反復の対象の値を示す語
		LabeledReceiver, // ラベル付の受け手の参照を始める字句
		LabeledReturn, // ラベルで返戻先を示す語
		LineContinuation, // 括弧の外で改行を越えて文を続ける記号（解析器が字句にせず空白と読む）
		ListSeparator, // 値の並びを区切る記号
		LogicalWord, // 語で綴る論理演算子
		LongIntegerType, // 幅が文法で変わる長整数型の名前
		Loop, // 無条件の反復の語
		MetadataElement, // 文書の付帯情報を収める要素の名前
		MetatypeSuffix, // 型自身を表す後置の名前
		NamespaceAttribute, // 名前空間を指定する属性の名前
		Negation, // 語で綴る否定の演算子
		NeverType, // 戻らない事を示す戻値の型
		NoReturnSpecifier, // 戻らない事を示す宣言の指定子
		NullMacro, // 空ポインターを表す旧来のマクロの名前
		NullishValue, // 値の無い状態を示す名前
		ObjectHead, // 無名の物を作る宣言の語
		OptionalityMarker, // 値の欠如を許す型へ付ける記号
		OutputArgument, // 出力先を受け取る実引数の指定子
		PanicMacro, // 呼出側の誤りで中断するマクロの名前
		PreserveSpaceAttribute, // 内部空白を保持する属性の名前
		PrivateAccess, // 非公開の成員を指定する語
		ProtectedAccess, // 継承先へ公開する成員を指定する語
		PublicAccess, // 公開する成員を指定する語
		ReassignableBinding, // 再代入出来る束縛を宣言する語
		ReferenceBinding, // 参照で束縛する語
		RegexDelimiter, // 正規表現を開く区切記号
		Return, // 返戻の語
		Self, // 自身を指す語
		SideEffectMethod, // 値の返戻を補わない副作用のメソッドの名前
		SignedByteType, // 符号の有無が文法で変わる８ビット整数型の名前
		StandardNamespace, // 標準の名前空間の修飾
		StatementDeclarationHead, // 式の文の先頭で宣言として読まれる語
		StatementEnd, // 文を明示して終える区切記号
		StringType, // 文字列の型名
		StringizingAssertion, // 引数を字面の儘保つ標準の表明マクロの名前
		TerminatingCall, // 戻らない事を言語・標準ライブラリが定める呼出の名前（修飾を含む綴り）
		TerminatingMacro, // 戻らない事を言語・標準ライブラリが定めるマクロの名前（修飾を含む綴り）
		ThrowHead, // 送出の式を始める語
		ThrowsAnnotation, // 送出を宣言する注釈の名前
		ThrowsMarker, // 署名に送出を宣言する語
		True, // 真の値の名前
		TypeDeclaration, // 型の宣言を始める語
		TypeQuery, // 値の型を問う演算子の語
		TypeTest, // 型を検査する演算子の語
		UniversalException, // 全ての例外を受ける基底の型名
		UsingNamespace, // 名前空間を取込む宣言の先頭
		ValueOutputTag, // 式の値を出力する埋込のコードの開始タグ
		VariadicMarker, // 可変長の仮引数を示す記号
		VoidType, // 値を返さない事を示す型
		While, // 条件判定の反復の語
		Count
	}; // 共通工程が照合・生成する語の意味（整形仕様 3.1, 3.5，綴りは GrammarProfile が言語毎に定め，最初の綴りを生成に用いる）

	// 行コメントの登録形の判定条件（整形仕様 4.5 の登録定義から記法・本文・位置の判定に要る条件だけを投影し，採用記法の成立条件として共有する変更不能な値，整形仕様 3.2）
	// 位置は本文の中の位置（行頭の組と囲みの印の中の組）を持ち，ファイルの中の位置（何行目か，最初の字句の前か）は，検証が新たな登録形を位置に依らず拒む（整形仕様 8.1）為に持たない
	struct RegisteredLineForms {
		std::string_view EncodingDeclarationWord = {}; // 注記の中から探す符号化宣言の登録語（小文字）
		std::string_view EncodingDeclarationSeparators = {}; // 符号化宣言の登録語と名前の区切り
		std::string_view EncodingNameChars = {}; // 英数字の他に符号化の名前へ使える字（空は区切りの後に空白でない字が続けば読む）
		bool IsEncodingWordCaseless = false; // 符号化宣言の登録語の大小文字を問わない
		bool AllowsEncodingSeparatorSpace = false; // 符号化宣言の登録語と区切りの間の空白を認める
		std::vector<std::string_view> EncodingMagicCommentNames {}; // 符号化を指定する実行時設定の組の名前（小文字）
		std::vector<std::string_view> MagicCommentNames {}; // 実行時設定の登録語（小文字，連結の字は `MagicCommentNameJoiners` の最後の字で表す）
		std::string_view MagicCommentMarker = {}; // 実行時設定の組を並べる囲みの印
		std::string_view MagicCommentSeparators = {}; // 実行時設定の名前と値の区切り
		std::string_view MagicCommentDelimiters = {}; // 囲みの中の組の区切り
		std::string_view MagicCommentQuotes = {}; // 名前を終える引用符（最初の字は値を囲む引用符）
		std::string_view MagicCommentNameJoiners = {}; // 実行時設定の名前の中で同じと読む連結の字
	};

	// 用途毎の採用記法と其の成立条件（整形仕様 3.2）
	struct CommentOutputFormList {
		std::vector<CommentOutputForm> Forms {}; // 用途毎の採用記法（無い用途は入力の記法の儘）
		RegisteredLineForms Conditions {}; // 採用記法の成立条件（揃えた行コメントが処理系の登録形に為らない事）
	};

	// コメントの中のコード例の書式（整形仕様 7.3, 7.4，字下げ・囲み・文書の行内タグ・指令・注釈の引数・要素で示すコード例）
	// 言語の文書形式が示すコード例の記法を合わせた物を，文書・通常コメントの全ての記法へ一律に当てる（記法の変換や文書への統合で同じ本文の読みを変えない，整形仕様 3.3）
	struct CodeExampleForms {
		bool MarksCodeByIndent = false; // 引用・箇条の印の後で字下げした行を段落の直後でもコード例とする（偽は空行の後の４桁以上の字下げだけ，本文は行頭に字下げを持たない）
		bool HasFencedCode = false; // 同じ字の３字以上の連続（``` 又は ~~~）で開閉する囲みのコード例を持つ
		std::vector<std::string_view> InlineTags {}; // 対応する閉じ波括弧迄の中身を字面の儘読む文書の行内タグの開始（Javadoc の `{@code` 等）
		std::string_view TagMarks = {}; // 文書の札・指令を始める字（札の行はコードスパンの段落を区切る）
		std::vector<std::string_view> BlockCommands {}; // 対応する終了指令迄を字面の儘読む指令の名前（Doxygen の `code` 等）
		std::string_view EndCommandPrefix = {}; // 終了指令の名前の前置き
		char AnnotationMark = '\0'; // 引数の括弧の中を字面の儘読む注釈を始める字
		std::vector<std::string_view> CodeElements {}; // 内容を字面の儘読む文書の要素の名前（HTML・XML の `<code>` 等）
		std::vector<std::pair<std::string_view, std::string_view>> MarkupSections {}; // 要素の対応付けで読み飛ばす区画の開始と終端（CDATA・注釈）
	};

	// コメントの記法の定義（整形仕様 3.2，使用可能な記法，入力の文書形式，用途毎の採用記法と其の成立条件，文書の語彙を持つ変更不能な値）
	struct NotationRules {
		std::vector<AvailableNotation> AvailableNotations {}; // 使用可能な記法
		bool IsBlockCommentNested = false; // 囲みコメントの中の `/*` が入れ子の囲みを開くか
		std::string_view LineCommentTerminator = {}; // 行コメントの本文を終える字面
		std::vector<std::string_view> LineCommentBreaks {}; // 行コメントの本文を行の途中で終える LF 以外の改行の字面
		std::vector<std::string_view> LineSplices {}; // 行末に置くと次の行を行コメントへ繋ぐ字面
		bool DecodesUnicodeEscapes = false; // 字句化前に Unicode の逃避を展開する（逃避で書いた改行も行を終える）
		bool BlockCommentInterruptsStatementHead = false; // 行頭の囲みコメントが後続文の読取を妨げる
		bool HasBracketedDocumentType = false; // 文書の型を角括弧で囲む
		bool HasBareDocumentType = false; // 文書の型を裸の語で前置する
		std::string_view DocumentParameterSigil = {}; // 文書の仮引数名を始める印
		std::vector<std::string_view> DocumentParameterMarks {}; // 文書の仮引数名の前に置く参照渡・可変長の印（文法が並べる順）
		bool ExcludesDirectiveLines = false; // 文書の本文から指示子の形の行（記号の直後の名前と `:`）を除く
		CodeExampleForms CodeExamples {}; // コメントの中のコード例の書式
		bool HasOptionalFailureType = false; // 失敗条件の札の型名を省ける
		std::vector<std::pair<CommentNotation, DocumentFormId>> DocumentForms {}; // 文書に使う記法と其の入力の文書形式
		std::string_view MemberDocumentHead = {}; // 前の成員の文書を表す本文の頭（Doxygen の `///<` 等）
		CommentOutputFormList OutputForms {}; // 用途毎の採用記法と其の成立条件
		bool DocumentCommentIsAttribute = false; // 文書記法が処理系の属性に為る
		std::string_view DocumentAttributeOpen = {}; // 文書を属性で表す時の開始記号
		bool InlineCommentPrecludesDocument = false; // 直前の文中コメントが文書行の挿入を妨げる
		std::vector<DocumentTagSynonym> DocumentTagSynonyms {}; // 同義の文書札
		DocumentationStyle Documentation = DocumentationStyle::AtTag; // 文書の項目の記法
		std::string_view FailureTag = "@throws"; // 失敗条件の既定の札
		bool ReviewMarkdownLine = false; // 行形式の文書の意味を確認して囲みへ移す
		bool HasMarkupComments = false; // 山括弧のコメント記号を使う
		bool IsAvailable(const CommentNotation Notation) const; // 使用可能な記法かの判定関数
		std::optional<DocumentFormId> DocumentFormOf(const CommentNotation Notation) const; // 文書の記法の文書形式の取得関数
		const CommentOutputForm *AdoptedFormOf(const CommentUse Use) const; // 用途の採用記法の取得関数
	};

	// 文法の構文・意味の固定の定義（整形仕様 3.2 の構文事実が共有する変更不能な値，言語名を持たず，投影側が文法毎に一度定める）
	struct SyntaxRules {
		// 数値の境界検査で認識する整数型の名前
		struct IntegerTypeName {
			std::string_view Name; // 型名
			uint32_t Bits; // 最小のビット幅
			bool IsSigned; // 符号の有無
		};

		// 中核の工程と静的検査が照合する演算子の綴り
		struct OperatorNotation {
			std::string_view Assignment; // 単純な代入
			std::string_view Equal; // 等価比較
			std::string_view NotEqual; // 非等価比較
			std::string_view StrictEqual; // 型変換の無い等価比較
			std::string_view StrictNotEqual; // 型変換の無い非等価比較
			std::string_view LogicalAnd; // 短絡する論理積
			std::string_view LogicalOr; // 短絡する論理和
			std::string_view LogicalNot; // 論理否定
			std::string_view Plus; // 加算又は単項の正号
			std::string_view Minus; // 減算又は単項の負号
			std::string_view AddAssign; // 加算を伴う代入
			std::string_view SubtractAssign; // 減算を伴う代入
			std::string_view Increment; // １を加える演算
			std::string_view Decrement; // １を引く演算
			std::string_view GreaterEqual; // 以上の比較
			std::string_view LessEqual; // 以下の比較
			std::string_view Greater; // 大小の比較で大きい側
			std::string_view Less; // 大小の比較で小さい側
			std::string_view MemberAccess; // 成員の参照
		};

		// 正規表現の名前付の捕捉の記法
		struct RegexCapture {
			std::string_view Open; // 名前の前の開き
			std::string_view Close; // 名前の後の閉じ
		};

		// 正規化の規則・制御の流れ・署名・コメント
		std::vector<std::string_view> CodeOpeningSuffixes {}; // 同じ行でコードを開く境界の末尾
		NotationRules Notation {}; // コメントの記法の定義（コメントの経路へは此れだけを渡す）
		std::string_view DocumentationForm = {}; // 静的検査で示す採用済の文書記法
		bool HasLabelJump = false; // 名前への任意の制御移動を持つ
		bool LabelJumpIsScoped = false; // ラベルの飛先を最内の有効範囲で選ぶ
		bool ExitHasNumericDepth = false; // 脱出先を正の段数で指定する
		bool ContinueCountsSwitch = false; // 継続の段数に切替も数える
		bool ExitLabelIsBodyChild = false; // 脱出先のラベルを本体の子に持つ
		bool ExitLabelIsPreviousSibling = false; // 脱出先のラベルを先行する兄弟に持つ
		bool CaseMayNestInStatement = false; // 切替の枝を文の内側にも置ける
		bool CaseOpensWithStatementEnd = false; // 枝の区切を文の終わりの記号でも開く
		bool CaseFallsThrough = false; // 終端しない枝が次の枝へ進む
		bool ColonCaseFallsThrough = false; // コロンで開く枝が次の枝へ進む
		bool EmptyCaseFallsThrough = false; // 本体の無い枝だけが次の枝へ進む
		OmittedResultNotation OmittedResult = OmittedResultNotation::Inferred; // 型を省いた関数の戻値
		bool TailExpressionIsValue = false; // 塊の末尾の式が塊の値を与える
		bool ReachabilityUsesConstantCondition = false; // 定数式の値に依って到達可能性が変わる
		std::string_view BasePrefixLetters = {}; // 小文字に保つ基数の接頭辞の字
		char DigitSeparator = '\0'; // 数値の桁を区切る字
		std::array<std::string_view, 3> ExitDepthBasePrefixes {}; // 脱出段数の１６進・２進・８進の接頭辞の字
		std::vector<std::string_view> NumericTypeSuffixes {}; // 符号と幅を含む数値の型接尾辞（長い物から照合する）
		std::string_view BigIntegerSuffix = {}; // 任意精度整数を示す数値リテラルの接尾辞
		std::string_view QuotePrefixes = {}; // 再表現出来る引用文字列の接頭辞
		std::string_view RawQuotePrefixes = {}; // 逃避を解かない引用文字列の接頭辞
		std::string_view FormatQuotePrefixes = {}; // 補間を持つ引用文字列の接頭辞
		std::string_view NonDocumentQuotePrefixes = {}; // 文書文字列に為らない引用文字列の接頭辞
		bool SingleQuoteBackslashIsLiteral = false; // 単引用符内の通常の逆斜線を字として読む
		bool InterpolatesDollar = false; // 二重引用符内でドル記号から補間する
		std::string_view HashInterpolationHeads = {}; // 二重引用符内で井桁記号の直後に置いて補間を始める字
		std::vector<RegexCapture> RegexNamedCaptures {}; // 照合で同名の局所変数を束縛する正規表現の名前付の捕捉の記法
		bool HasTripleQuote = false; // 三重引用符を持つ
		bool BooleanNameNeedsDeclaration = false; // 真偽値の名前を宣言後にだけ使える
		bool PadDecimalPoint = false; // 小数点の前後の零を補う
		bool TrailingCommaHasEmptyValue = false; // 末尾のカンマが空の値を表す
		bool SingletonCommaDeterminesTuple = false; // 単一要素の末尾カンマが組の型を決める
		bool LineContinuesOnlyInBrackets = false; // 括弧の外の改行が文を終え，括弧の内側だけで行が続く
		bool SplitBindingsBeforeConst = false; // 不変指定の前に宣言子毎へ分ける
		bool NormalizeTypeAssociation = false; // 型の合併と交差を左結合へ揃える
		bool NormalizeTupleWrapping = false; // 組の包装を固定点迄揃える
		bool CallFormsMayDeclare = false; // 名前が型かを構文から判別出来ず，型の名前の呼出の形が宣言子とも読める
		const KindDefinition *FalseDefinition = nullptr; // 偽の値の生成定義
		const KindDefinition *ExpressionStatementDefinition = nullptr; // 式の文の生成定義
		const KindDefinition *ReturnExpressionDefinition = nullptr; // 返戻の式の生成定義
		const KindDefinition *ParenthesizedStatementsDefinition = nullptr; // 括弧で囲む文の並びの生成定義
		const KindDefinition *TrueDefinition = nullptr; // 真の値の生成定義
		const KindDefinition *LoopExpressionDefinition = nullptr; // 無条件の反復式の生成定義
		const KindDefinition *ImplicitParameterDefinition = nullptr; // 暗黙の仮引数の生成定義
		const KindDefinition *DoubleQuotedDefinition = nullptr; // 二重引用符への変更で種別も変わる文字列の生成定義
		const KindDefinition *PrefixUnaryDefinition = nullptr; // 前置の単項式の生成定義
		const KindDefinition *PrefixDefinition = nullptr; // 前置の式の生成定義
		std::unordered_map<std::string_view, const KindDefinition *> TokenDefinitions {}; // 生成する字句の字面毎の種別の定義
		std::array<std::vector<std::string_view>, static_cast<size_t>(Word::Count)> Words {}; // 語の意味毎の綴り（最初の綴りを生成に用いる）
		std::bitset<static_cast<size_t>(Word::Count)> CaseInsensitiveWords {}; // 大文字小文字を区別せず照合する語の意味
		std::vector<CanonicalForm> GeneratedForms {}; // 文法に共通の生成形式
		// 中間表現の構築と配置
		bool CanContinueWithBrace = false; // 次の行の波括弧が前の式へ続く記法
		bool CanContinueWithCall = false; // 次の行の呼出・添字・参照が前の式へ続く記法
		bool CanOmitTrailingSemicolon = false; // 末尾のセミコロンを省く記法
		bool IsWhitespaceSemantic = false; // 空白の追加が選択子・値の意味を変える記法
		std::string_view LexicalSeparator = {}; // 空白を入れずに字句を分ける固定の記法
		bool CanBreakAfterAssignment = true; // 代入の記号の直後で式を継続出来る記法
		bool HasDeferredLiteralBodies = false; // 開始行より後に本文を置く字句を持つ記法
		bool HasLineDirectives = false; // 前処理指令が物理行を占める記法
		bool HasAutomaticLineTerminator = false; // 行末の名前・閉じ括弧で文区切りを補う記法
		bool CanAttachTopLevelSemicolon = false; // 独立した最上位のセミコロンを前の式へ接続する記法
		bool HasHeadMemberArrow = false; // 成員参照の矢印を次行の先頭へ置く記法
		bool HasNumericUnitAdjacency = false; // 数値と単位の境界で原文の隣接を保つ記法
		bool HasSpacedEmptyValue = false; // 空の宣言値のコロンと終端を空白で区切る記法
		bool HasBareControlHeader = false; // 制御語の後に必須の条件括弧を持たない記法
		std::string_view LineEdgeSpaceOpen = {}; // 行端の本文の空白を値の式で表す開き
		std::string_view LineEdgeSpaceClose = {}; // 行端の本文の空白を値の式で表す閉じ
		std::string_view LineEdgeTab = {}; // 行端の本文のタブを表す字面
		std::vector<std::string_view> TailOperatorWords {}; // 行末へ置く語の演算子
		std::vector<std::string_view> HeadBoundaryWords {}; // 行頭へ置く節の語
		// 静的検査
		NameCase FunctionCase = NameCase::Unrestricted; // 関数名の大小文字の条件
		bool FunctionCaseByFirst = false; // 名前の先頭の大小で公開範囲を定める
		std::vector<std::string_view> BooleanTypeNames {}; // 真偽値を返す型の名前
		std::vector<std::string_view> BooleanQueryPrefixes {}; // 真偽を問う関数名の先頭
		std::string_view BooleanQuerySuffix = {}; // 真偽を問う関数名の末尾
		std::vector<std::string_view> GetterPrefixes {}; // 取得を表す関数名の先頭
		std::vector<std::string_view> SetterPrefixes {}; // 設定を表す関数名の先頭
		std::string_view SetterNameSuffix = {}; // 言語が設定メソッド名へ要求する末尾
		std::string_view DestructiveMethodSuffix = {}; // 受け手を破壊的に変更するメソッドの末尾
		std::string_view InstanceStatePrefix = {}; // 受け手の状態を表す束縛の先頭
		std::vector<std::string_view> DocumentationConstraintHeadings {}; // 利用上の制約を説明する文書の見出
		std::vector<std::string_view> DocumentationComplexityHeadings {}; // 計算量を説明する文書の見出
		bool TypeAnnotationObserved = false; // 型注釈の評価又は観測が有る
		bool HasReceiverParameter = false; // 型の成員の最初の仮引数で受け手を受ける
		bool DetailedMemberOrder = false; // 成員の詳細な分類順位を使う
		std::string_view MemberStaticWord = {}; // 静的成員の記憶域指定
		std::string_view MemberVirtualWord = {}; // 仮想成員の指定
		InferredTypeNotation InferredType = InferredTypeNotation::None; // 推論型又は動的な最上位型の記法
		std::vector<std::string_view> FixedFunctionNames; // 外部契約で固定する名前
		std::string_view FixedFunctionPrefix; // 外部契約で固定する名前の接頭辞
		std::vector<std::string_view> FixedNameModifiers; // 名前を外部契約へ結び付ける修飾語
		std::vector<std::string_view> CountNames; // 長さ又は要素数を返す名前
		std::vector<IntegerTypeName> IntegerTypeNames; // 数値の境界検査で認識する整数型
		std::vector<std::string_view> FloatingTypeNames; // 数値の境界検査で認識する浮動小数点型
		std::vector<std::string_view> CharacterTypeNames; // 数値の境界検査で認識する文字型
		ChildRole CountNameRole = ChildRole::Function; // 要素数の名前の子の役割
		std::string_view DefaultIntegerType; // 接尾辞の無い整数リテラルの型
		uint32_t LongBits = 32; // long の最小のビット幅
		uint32_t NestingLimit = 8; // 字下げの上限
		bool ByteIsSigned = false; // byte の符号の有無
		bool HasLeadingZeroOctal = false; // ０始まりの８進表記
		bool RequiresKnownInteger = false; // 型を宣言から確定出来ない比較では要素数だけを扱う
		bool OrderMembers = false; // 成員を型・変数・関数の順に並べる
		std::vector<std::string_view> AccessOrder {}; // 公開範囲の全順位
		std::string_view ImplicitAccess = {}; // 修飾子を省いた成員の公開範囲
		bool RequireExplicitPrivate = false; // 既定の私有区画を明記する
		bool RequireExplicitInternal = false; // 既定の内部公開を明記する
		bool HasAccessSections = false; // 独立のアクセス区画を持つ
		bool HasModifierGroups = false; // 修飾語を一つの並びへ纏める
		bool HasReturnLabels = false; // ラベルが関数値の返戻先を兼ねる
		bool HasReturnComments = true; // 返戻の前に説明を付ける
		bool HasStrictEquality = false; // 型変換を伴わない等価演算子を持つ
		bool AllowsNumericTruthiness = false; // 数値の０比較を真偽判定で表せる
		bool PrefersBooleanXorAssignment = false; // 組込真偽値の反転に排他的論理和代入を用いる
		bool PrefersUnitUpdate = false; // 数値の１加減算に増減演算を用いる
		bool PrefersPrefixUpdate = false; // 結果未使用の増減演算に前置を用いる
		OperatorNotation Operators {}; // 中核の工程と静的検査で照合する演算記号
		bool CatchAllBindings = false; // 型の無い捕捉の束縛が全てを受ける
		bool CatchBindingPattern = false; // let・var の単独の束縛が全てを受ける
		bool CatchExceptionList = false; // 捕捉する型を例外の並びで指定する
		bool CatchFilter = false; // when に依る捕捉条件を持つ
		std::string_view MarkupCommentOpen = {}; // 文書のコメントを開く記号
		std::string_view MarkupCommentClose = {}; // 文書のコメントを閉じる記号
		/** ========== 括弧・波括弧・宣言の正規化 ========== */
		std::unordered_set<std::string_view> AssignmentOperators {}; // 代入演算子の字面
		std::unordered_map<std::string_view, int> OperatorPrecedence = {}; // 演算子の字面毎の優先順位（０は不明）
		std::unordered_set<std::string_view> ComparisonOperators = {}; // 比較演算子の字面の集合
		std::unordered_set<std::string_view> FusedPrefixPairs = {}; // 前置演算子と値の先頭が融合する２文字の組
		std::unordered_set<std::string_view> FusedRangePairs = {}; // 範囲の区切り同士が融合する２文字の組
		std::string_view OperatorFusionChars = {}; // 隣接すると演算子の字句へ融合する字
		std::string_view AssignmentFusionBefore = {}; // 代入の左側で演算子へ融合する字
		std::string_view AssignmentFusionAfter = {}; // 代入の右側で演算子へ融合する字
		std::string_view AttachedOperatorPrefixChars = {}; // 密着時に後置演算子へ読める先頭の字
		std::string_view MisparsedComparisonLeadChars = {}; // 読違えた比較の断片に続く字の候補
		std::string_view OpenRangeClosers = ")]},;"; // 終端の無い範囲を閉じる区切り
		std::unordered_set<int> RequiredValueParenPrecedences = {}; // 値の位置で括弧を要する優先順位
		std::string_view SignsOverPower = {}; // 累乗を外側から取れる単項の記号
		int PowerPrecedence = -1; // 累乗の優先順位（無ければ負）
		int AssignmentPrecedence = 1; // 代入の優先順位
		int ConditionalPrecedence = 2; // 条件演算の優先順位
		std::unordered_set<std::string_view> TypeContinuationTokens = {}; // 変換先の型へ続く演算子
		std::unordered_set<int> NonAssociativePrecedences = {}; // 同格の括弧を保持する優先順位
		std::unordered_set<int> RightAssociativePrecedences = {}; // 右結合の優先順位
		std::unordered_set<int> ProtectedPrecedencePairs = {}; // 読みを保つ為に括弧を残す親と子の優先順位の組（親×３２＋子）
		const KindDefinition *PatternListDefinition = nullptr; // 囲み無の照合の並びの生成定義
		const KindDefinition *TupleExpressionDefinition = nullptr; // 囲み無の組の式の生成定義
		const KindDefinition *ExpressionListDefinition = nullptr; // 囲み無の値の並びの生成定義
		const KindDefinition *PrefixExpressionDefinition = nullptr; // 前置の演算の生成定義
		const KindDefinition *ExpressionParenDefinition = nullptr; // 式の括弧の生成定義
		bool FreezesLexicalBindings = false; // 変更の無い塊の有効範囲の束縛を不変の宣言へ揃える記法か
		bool CanMutateBase = false; // 欄・添字・分割代入の変更が基底の束縛に及ぶか
		bool ShouldRemoveMutableMarker = false; // 不変化で可変の指定を除去する記法か
		bool ShouldPreserveConstantStatus = false; // 不変化による定数変数化を避ける必要が有るか
		const KindDefinition *ModifierListDefinition = nullptr; // 修飾子の並びの生成定義
		bool IsSpelling(const std::string_view Text, const Word Meaning) const; // 字面が語の意味の綴りかの判定関数
		std::string_view Spelling(const Word Meaning) const; // 語の意味の生成する綴りの取得関数
		bool FusesOperatorChar(const char Char) const; // 字が演算子の融合に関わるかの判定関数
		bool FusesAssignment(const char Before, const char After) const; // 代入の字句と隣接する字が融合するかの判定関数
		bool FusesRangePair(const char Before, const char After) const; // 範囲の区切り同士が融合するかの判定関数
		static bool IsFusionLetter(const char Char); // 語の演算子へ融合する英字かの判定関数
		static bool IsFusionAlnum(const char Char); // 名前へ融合する英数字かの判定関数
		static bool IsFusionNameStart(const char Char); // 演算子名へ融合する英字又は下線かの判定関数
		static bool FusesDimensionTokens(const char Before, const std::string_view After); // 隣接する字句が次元値等へ融合するかの判定関数
	};

	// 共通の節点種別の定義（整形仕様 3.2，役割と空行比較の値が等しい記号で共有する変更不能な値）
	struct KindDefinition {
		uint32_t BlankKey = 0; // 公開節点型の同異だけを表す空行比較の値
		RoleSet Roles {}; // 役割
		bool IsNamed = false; // 構文か（偽は字句）
	};
} // namespace Core
