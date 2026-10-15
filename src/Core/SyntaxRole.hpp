#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

enum class CommentNotation : uint8_t;

namespace Core {
	enum class DocDescription : uint8_t { Failure, Inheritance, NestedSig, Other, Param, ParamList, Return, Summary, TypeParam }; // 形式共通の説明項目
	enum class DocItemForm : uint8_t { Heading, HeadingList, List, Prose, Tag, XmlElement }; // ドキュメントの項目の外形
	enum class DocStylePolicy : uint8_t { Standard, NameSummary, SrcOwned }; // ドキュメントの所有と概要の扱い

	// ドキュメント形式毎の読取語と生成語（記法の定義が span で持つ為，古い libc++ でも完全な型として此処で定義する）
	struct DocVocabularyEntry {
		DocDescription Item; // 共通の説明項目
		DocItemForm Form; // 項目の外形
		std::string_view Canonical; // 生成時の規約の綴り
		std::string_view Prefix; // 名前を持つ項目の開始テキスト
		std::string_view Suffix; // 名前を持つ項目の終端テキスト
		std::array<std::string_view, 28> Aliases; // 読取時に同義とする綴り（綴りの数≦ 28）
		std::string_view Skeleton = {}; // 名前を持たない項目の生成テキスト
		std::array<std::string_view, 5> Qualifiers {}; // タグの後の修飾語（修飾語の数≦ 5）
		std::string_view Attribute = {}; // 名前を取る要素の属性名
		DocStylePolicy Policy = DocStylePolicy::Standard; // 概要とソース所有の扱い
	};

	enum class Role : uint16_t {
		AliasDecl, // 別名宣言
		AnnotationParts, // アノテーションを成す子の並び
		ArrowFunction, // 単一の名前の仮引数を括弧無で書ける関数
		Assignment, // 代入の式
		Await, // 待機の式
		BareTuple, // 囲みを持たない組の並び
		Block, // 文を波括弧で包み得るブロック
		BodyOwner, // 単一の文の本体を持つ制御構文（繰返・ラベル付の文・リソースの文・else 節等）
		BodyWrapper, // 波括弧の有無に依らず制御本体を包むノード
		BranchBody, // 枝の値を結果にし得る本体
		CaseArm, // 値を結果にする分岐の枝
		Clause, // 同じ深さの行頭へ置く後続の節
		CodeBoundary, // 地の文を挟むコードの境界
		CodeOpener, // 埋込コードを開くトークン
		CollapsibleText, // 行を跨ぐ空白を畳む地の文
		CommaDecl, // 宣言子又は名前をカンマで併記出来る宣言
		CommaExpr, // カンマで並べた値の式
		CondSection, // 条件と本体の境に語を要する節
		Construction, // new に依る構築式
		ContinuationExpr, // 改行時に継続用のラッパーを選べる式
		CreationInputs, // 関数・クロージャの作成時に評価する初期値の並び
		CreationVal, // 作成時に外側の範囲で評価する初期値を持つ項目
		Decl, // 本体に置くと有効範囲の変わる宣言
		DeclSeq, // 分離・統合出来る宣言を文・メンバ・最上位の列として並べるコンテナ
		DeclaratorBinary, // 宣言の読みを演算子の役割で判ずる二項式
		DeclaratorCallee, // 型名としても読める呼出先
		DeclaratorContainer, // 宣言の候補を並べる又は包む構造
		DeclaratorFirstChild, // 先頭の子を宣言の候補として読む構造
		DeclaratorInvalid, // 宣言の実引数として読めない構造
		DeclaratorItem, // 併記出来る宣言子
		DeclaratorOperator, // 宣言の間接参照・参照又は型引数の区切にも為る演算子
		DeducedType, // 宣言子毎の初期化子から型又は型引数を推論し得る型（C++ の `auto` と型引数を省いたジェネリック型の名前）
		DefaultParam, // `=` を伴う仮引数の既定値
		DelimitedBody, // 見出の区切と終端の語で囲む本体
		DoLoop, // 後判定の繰返
		EagerEvaluation, // 名前付の子を此の式の評価時に必ず評価する構文
		ElseClause, // else 節
		ElseIfClause, // 自身の条件を持つ後続の枝の節
		ElseKeyword, // else のトークン
		EnumSpecifier, // 列挙型の指定
		EqualsSpaced, // 初期化・既定値の `=` を空白で区切る構文
		EqualsTight, // 名前付引数等の `=` を密着させる構文
		Error, // パーサーのエラー領域
		EvaluationCondition, // 制御構文の入口で評価する条件又は対象のラッパー
		EvaluationEntry, // 親の評価開始時に処理する入力の節
		ExceptionSection, // 例外の見出と本体を改行で分ける節
		ExpandedBody, // 文を持つ本体を必ず展開するコンテナ
		Export, // 公開する宣言又は値
		ExprStmt, // 式の文
		ExprTerminator, // 式を終える区切のトークン（閉括弧・カンマ・セミコロン）
		FinalSection, // 常に展開する後始末の節
		If, // 条件分岐
		IndentedBody, // 改行とインデントで境界を表す本体
		InlineText, // 地の文
		Instantiation, // 型引数を付けた式
		Iteration, // in 又は of で反復する構文
		KeywordBlock, // 語で開始し終端の語で閉じる本体
		KeywordSection, // 語で始まる後続の節
		Label, // ラベル・アノテーション
		Labeled, // ラベル付の文
		LastValLambda, // 最後の文の値を結果にするラムダ
		Loop, // 繰返
		MarkupAttribute, // タグの属性
		MarkupClose, // 内容を終えるタグ
		MarkupContainer, // 要素と本文を並べるドキュメント構造
		MarkupElement, // 開閉タグで内容を囲む要素
		MarkupExpr, // 地の文の中の式のラッパー
		MarkupOpen, // 内容を始めるタグ
		MarkupText, // 表示する本文
		MemberBody, // 終端記号を持つメンバの本体
		NameParts, // 名前を成す子の並び
		NamedImports, // 名前付のインポートの並び
		Namespace, // 名前空間の宣言
		NamespaceAliasDef, // 名前空間の別名の定義
		NamespaceDef, // 本体を持つ名前空間の定義
		NonNullAssertion, // null でない事のアサーション
		OpenBrace, // `{` のトークン
		OuterText, // 本体の外で逐語に保つ地の文（最初のトークンの前の空白と閉タグの後の地の文）
		PatternBranch, // 対象式の後で改行を要する照合
		PostfixReceiverHost, // 受手に呼出又は添字を付ける式
		PrefixOperation, // 前置の演算
		Preproc, // プリプロセッサディレクティブ
		PreprocBranch, // 宣言等を子として持つ条件分岐のプリプロセッサディレクティブ
		PreprocLine, // １物理行の内部を折らないプリプロセッサディレクティブ
		PreservedTokenSeq, // 展開・照合に用いるトークン列（意味を持つトークン・区切を保持する）
		RequiredParam, // 型注釈を持ち得る必須の仮引数
		ReturnVal, // 明示的な返戻で包める値
		SectionHeader, // 本体より１段浅く置く case・ラベル・アクセス修飾子等の節見出
		StmtHost, // 文を直接並べるノード
		StmtOnly, // 値を持たない文
		StmtSeq, // 本体の中の文の並び
		TailValFunction, // 本体末尾の値を暗黙に返す通常関数
		TerminatedMember, // 本体の兄弟として終端記号を補うメンバ
		TerminatedStmt, // 終端のセミコロンを要する文
		TrailingCommaHost, // 展開時に末尾のカンマを要する並び
		TransparentSection, // 同行時の境界語を親が出す本体
		TypeAlias, // 型の別名の定義
		TypeApplication, // 型引数を適用する型
		TypeArgList, // 型の実引数の並び
		TypeAssertion, // 型のアサーション
		TypeDef, // 型の定義
		TypeMember, // 両区切を許す型のメンバ
		TypeMemberBody, // 型のメンバを並べる本体（共有する本体は所有者で区別する）
		TypeParamList, // 型の仮引数の並び
		TypeSyntax, // 式とは異なる文法で読む型のラッパー
		TypeofCmp, // typeof の比較を厳密化出来る二項式
		UsingKeyword, // リソースの宣言の語
		ValBranching, // 枝の値を結果にする式
		ValTry, // 値を結果にする try 式
		// 正規化の規則・制御の流れ・シグネチャ・コメント
		AdjacentDocSeq, // 直後の宣言が先行コメントをドキュメントへ取り込む並び
		AlternativeBodyCloser, // 代替の本体記法の終端語
		AmbientDecl, // 外部の実体を記述する宣言
		AnnotatedLambda, // アノテーションを伴うラムダ
		Annotation, // 宣言へ付けるアノテーション
		AnonymousClass, // 無名のクラス
		AppliedGenericType, // 引数を適用したジェネリック型
		ArgReturn, // 実引数の並びを返す返戻
		ArgSeq, // 実引数を並べるコンテナ
		ArmSwitch, // 枝を全列挙して終端を判定する切替
		ArrayDeclarator, // 配列の宣言子
		ArrayVal, // 配列の値
		AssertsAnnotation, // 真である事をアサーションする戻値アノテーション
		AssignmentExpr, // 代入の値を表す式
		AttachPrefix, // 宣言に先行してドキュメントのアンカーと為るアノテーション
		Attribute, // 属性の参照又は指定
		AttributeArgList, // 属性の実引数の並び
		AttributedTailFunction, // 属性の展開が本体を書き換え得る末尾値の関数
		BareArgNotation, // 括弧を省ける実引数・仮引数と終端の無い値の記法
		BareFallthrough, // 独立した語で次の枝へ進む
		BareListTrailingComma, // 囲みを持たない並びの余分な末尾カンマを除く
		Binary, // Ruby の二項式の解析形（累乗・命令形の読取）
		BinaryExpr, // 言語横断の二項演算又は其の連鎖
		BinaryOperation, // 左右の場又は３部品を持つ二項式の解析形（投影・正規化の修復）
		BodyTempScopeLoop, // 一時値の有効範囲を本体に限る反復
		BoolInitializerDecl, // 整数の初期値を真偽値名へ揃える型付宣言
		BoolLiteral, // 真偽値のトークンのラッパー
		Boolean, // 真偽値
		BorrowingOperator, // 借用又は間接参照の演算子
		BreakKeyword, // 反復から脱出する語
		BreakTarget, // 無名の脱出先に為る構文
		BuiltinArithmeticType, // 予約語で型が確定する組込の算術型
		CallArgs, // 呼出の実引数の並び
		CallExpr, // 呼出式
		CallNode, // 関数又はメソッドの呼出
		CallSuffix, // 呼出を付加する接尾部
		CaseBodyOpener, // 枝の本体を開く区切
		CaseClause, // 切替の枝の節
		CaseExprArm, // 文の case と区別して終端を合成する切替の式の枝
		CaseLabelJump, // 切替の枝をジャンプ先として指定出来る移動
		CatchClause, // 例外を捕捉する節
		CatchableBody, // 捕捉の節を後置する本体
		Class, // クラスの本体を伴う構文
		ClassBody, // クラスのメンバを持つ本体
		ClassDecl, // クラスの宣言
		ClassMembers, // クラスのメンバの並び
		ClauseOrCondLoop, // 条件又は３節を選べる反復
		CloserToken, // 囲みを閉じるトークン
		ColorVal, // 色の値
		Comment, // コメント
		CommunicationCase, // 通信で選ぶ切替の枝
		CompoundAssignment, // 演算を伴う代入
		CompoundBody, // 複数の文を囲む本体
		CompoundFunctionBody, // 文列の本体を波括弧のブロックに限る通常関数
		CondParenWrap, // 条件を包む括弧又は条件のコンテナ
		ConditionalBranchClause, // 条件を持つ分岐の節
		ConditionalVal, // 条件に依って選ぶ値
		ConstantName, // 定数の名前
		ConstructionReceiverHost, // 構築式の受手の親
		ConstructorInvocation, // コンストラクタの呼出
		ContinueExitsSwitch, // 継続で外へ抜ける切替
		ContinueKeyword, // 反復を継続する語
		ContractAttribute, // 宣言へ適用する名前・修飾・引数を持つ属性
		ContractAttributeGroup, // 宣言へ適用する属性の並び
		ContractAttributeList, // 宣言へ適用する実引数形式の属性の並び
		CountedLoop, // 初期化・条件・更新を持つ反復文
		DeclList, // 宣言の並び
		DeclSpecifier, // 宣言の性質を指定する構文
		DeclTypeChild, // 宣言の型を示す子
		DeclTypeSpecifier, // 宣言の型を指定する構文
		DeclaratorParams, // 宣言の仮引数の並び
		DeclaratorSig, // ネストの宣言子でシグネチャを定める定義
		DefBody, // 定義の本体
		DefaultCase, // 既定の枝
		DestructorDecl, // 破棄子の宣言
		DestructurePattern, // 値を分解して束縛するパターン
		DirectParamSig, // 仮引数を直下へ並べる定義
		DirectSwitchArms, // 切替の枝を直下に並べる
		DiscardedBranchValHost, // 枝の値を使わない文の親
		DocHostSeq, // ドキュメントと定義を順に並べるコンテナ
		DocStr, // 本体の先頭でドキュメントに為り得る文字列
		DocWrapper, // ドキュメント化対象を包む構文
		DocumentableDecl, // 既存のドキュメントを付けれる型・メンバ・最上位の宣言
		DocumentedDef, // ドキュメントを同期する定義
		ElementRef, // 要素の参照
		Ellipsis, // 省略を表す語
		Else, // 条件に一致しない時の枝
		ElseAny, // 既定の後続枝
		EmptySemicolonSeq, // 空文を区切として除ける文の並び
		EmptyStmt, // 空の文
		EmptyStmtContainer, // 独立した無名の空文・空宣言を直接並べるコンテナ
		EnumBodyDecls, // 列挙本体の宣言の並び
		EnumClassBody, // 列挙クラスの本体
		EnumEntry, // 列挙の項目
		ExitKeyword, // 実行を終端する語
		ExitStmt, // 実行を終了する文
		ExplicitKeywordBody, // 開始語を明示する本体
		ExplicitResultFunction, // 末尾へ明示返戻を補える通常関数
		ExprCase, // 式で選ぶ切替の枝
		ExprList, // 式の並び
		FallthroughName, // 次の枝へ進む語の候補
		FallthroughStmt, // 次の枝へ進む文
		FieldName, // フィールドの名前
		FinallyClause, // 後始末の節
		FirstAssignmentBinding, // 初回の単純代入がローカル束縛を導入する形
		FlowBoundary, // 外側の制御移動から独立する境界
		FlowReturn, // 制御を呼出元へ返す構文
		ForClause, // ３節の反復の見出
		ForRangeLoop, // 範囲を走査する反復
		ForeachStmt, // 要素を走査する反復文
		FunctionBindingScope, // 単純代入のローカル束縛を関数全体で共有する範囲
		FunctionBody, // 関数の本体
		FunctionBodyContainer, // 関数の文を直接持つコンテナ
		FunctionCallExpr, // 関数を対象とする呼出式
		FunctionDeclarator, // 関数の宣言子
		FunctionDef, // 関数の定義
		FunctionExpr, // 関数を値とする式
		FunctionValParams, // 関数の値の仮引数の並び
		GeneratorMarker, // ジェネレータを指定する記号
		GenericName, // 型引数を伴う名前
		GenericParam, // ジェネリック型の仮引数
		GenericParams, // ジェネリック型の仮引数の並び
		GotoStmt, // 名前を指定する移動文
		GroupingParen, // 式を纏める括弧
		HeredocBeginning, // 複数行の文字列の開始部
		HoistedDocBody, // 先頭の定義のドキュメントを外へ巻き上げる本体
		Identifier, // 識別子
		ImplicitResultBody, // 末尾値を暗黙に返す本体
		ImplicitReturnCandidate, // リザルトビルダーの適用を調べる単一式の通常関数
		ImplicitReturnCandidateExpr, // 暗黙返戻の単一式と為り得る式
		ImplicitReturnConformance, // リザルトビルダーを推論し得る適合・継承の節
		ImplicitReturnExpr, // 暗黙返戻を明示出来る単一の値の式
		ImplicitReturnExtToken, // 外部の適合を持ち得る拡張の語
		ImplicitReturnNominal, // メンバの暗黙返戻を調べる型の宣言
		InferredParams, // 型を推論する仮引数の並び
		InitDeclarator, // 初期化子を伴う宣言子
		InitializerExpr, // 初期化の式
		InlineDefBody, // 独立した本体コンテナを要しないドキュメント化対象
		InnerFunctionStmts, // 本体の内側で関数の文を並べるコンテナ
		IntVal, // 整数の値
		InvocationBody, // 呼出へ渡し，実行時機が呼出先に依存する本体
		IterationVal, // 反復の値
		JumpLabel, // 移動先のラベル
		KeywordJump, // 語で指定する制御移動
		KeywordWhile, // 条件判定の反復の語
		LabeledBlockExit, // 本体自身のラベルへ脱出出来る文の並び
		LabeledStmt, // ラベル付の文
		LabeledValExit, // 値とラベルを分けて指定する脱出
		LambdaExpr, // 式としてのラムダ
		LambdaFunctionType, // ラムダの関数型
		LambdaFunctionTypeParams, // ラムダの関数型の仮引数
		LambdaLiteral, // ラムダの値
		LambdaParam, // ラムダの仮引数
		LambdaParams, // ラムダの仮引数の並び
		LazyLogicalOperator, // 短絡評価する論理演算子
		LeadingConstDecl, // 型の前へ const を移せる宣言
		LeadingStrDocOwner, // 先頭の文字列をドキュメントとする定義
		LexicalBlock, // ローカルの有効範囲を持つブロック
		LinkageSpec, // 結合方式の指定
		LoopElse, // 脱出せず終了した時の後続枝を持つ反復
		LoopExitStmt, // 反復から脱出する文
		LoopExpr, // 無条件の反復式
		MacroCall, // マクロの呼出
		MatchBlock, // 照合の枝を並べるブロック
		MayDeclarePlaceholderFunction, // 省略の本体だけで型を宣言出来る関数
		MethodDecl, // メソッドの宣言
		MethodDef, // メソッドの定義
		MethodInvocation, // メソッドの呼出
		MethodParams, // メソッドの仮引数の並び
		MixinParams, // ミックスインの仮引数
		MixinRule, // ミックスインの宣言又は呼出
		MixinStmt, // ミックスインの呼出文
		Modifier, // 宣言の修飾子
		Module, // モジュールの本体
		NamedAccessor, // 名前で読出と其の他を分ける操作
		NamedLabelStmt, // 名前付のラベルの文
		NonConstantExpr, // 定数式に為れない式
		NonValDecl, // 値を返さない宣言文
		NonnegativeNumericType, // 負の値を持たない組込数値型の綴り
		NormalizedNum, // 数値の字形を揃える値
		NormalizedQuote, // 引用符を二重引用符へ揃える文字列
		NumLiteral, // 数値の値を表す構文
		NumericToken, // 数値を表すトークン
		ObjectCreationExpr, // オブジェクトの構築式
		ObjectVal, // オブジェクトの値
		ObservableQualifier, // 読取を観測可能にする限定子
		OmittableCondLoop, // 省いた条件で無条件に続く３節の反復
		OpaqueLineText, // 行を分割せず保持する本文
		OperatorCast, // 型変換の演算子
		OptionalAnnotationArgs, // 空のアノテーション引数を省ける親
		OptionalConstructionArgs, // 空の構築・アノテーション・終了の実引数を省ける構文
		OptionalEmptySuperclass, // 空の基底型の並びを省ける定義
		OptionalLambdaDeclarator, // 空の仮引数だけの宣言子を省けるラムダ
		OptionalLoopTerminator, // 本体を持たない反復又は代替構文の終端を補う
		OptionalSemicolonHost, // 文・メンバのセミコロンを改行へ替えれる並び
		OptionalVoidParam, // 引数の無い事を表す型を省ける並び
		OverloadableIncrement, // 多重定義を区別して増減を揃える式
		Param, // 仮引数
		ParamContainerLike, // 仮引数を含むコンテナ
		ParamDecl, // 型を含む仮引数の宣言
		ParamModifierList, // 後続の仮引数を指定する修飾の並び
		ParamName, // 仮引数のシグネチャへ取り出す名前
		ParamNameWrapper, // 単一の仮引数名又は分解パターンを包む構文
		ParenthesizedDeclarator, // 括弧で囲む宣言子
		ParenthesizedLoopCond, // 括弧の条件を持つ反復
		PatternCond, // パターンへの束縛を条件とする構文
		PatternList, // パターンの並び
		PatternMembership, // 所属を調べるパターン
		PatternParam, // 束縛のパターンで名前を定める仮引数
		PlaceOrPlainVal, // 格納場所又は一時値を延長しない値
		PlaceProjection, // 格納場所の部分を参照する式
		PlaceholderBody, // 説明文字列や省略の文だけを空本体とする並び
		PlaceholderStmt, // 空の実装を表す省略の文
		PlaceholderTypeSpecifier, // 推論を要求する型指定子
		PlainStmtSeq, // 値を返さない文の並び
		PostCondLoop, // 後判定で本体を１度は実行する反復
		PostParamResultType, // 仮引数の後へ置く戻値型
		PostfixExpr, // 後置の演算式
		PostfixLoop, // 本体の後へ条件を置き，本体の形で前判定と後判定を分ける繰返
		PostfixUnaryExpr, // 後置の単項演算式
		PrefixableIncrement, // 使わない結果の増減を前置へ揃えれる式
		PreprocElse, // 前処理の既定の枝
		PrimaryConstructor, // プライマリコンストラクタ
		PrimitiveType, // 基本型
		PtrDeclarator, // ポインターの宣言子
		PtrOrRefDeclarator, // ポインター又は参照の宣言子
		PtrTypeDeclarator, // ポインター型の宣言子
		QualifiedIdentifier, // 修飾付の識別子
		QualifiedMember, // 受手又は修飾を伴うメンバ
		QuotedStr, // 引用符で囲む文字列
		Range, // 範囲
		RangeClause, // 範囲反復の見出
		ReaderRoutine, // 値を返す読出の操作
		ReceiverCall, // 受手を伴い得る呼出
		RefDeclarator, // 参照を導入する宣言子
		RefExpr, // 参照を得る式
		Regex, // 正規表現
		ResourceBody, // 開始・後始末が暗黙に送出し得るリソースの管理
		ResourceList, // 記述順に開始し，逆順に後始末するリソースの並び
		ResultlessRoutine, // 値を返さない構築・破棄・設定・監視の操作
		ReturnKeyword, // 返戻の語
		ReturnOrThrow, // 返戻又は例外送出
		ScopeBody, // 独立した有効範囲の本体
		ScrutineeHost, // 照合対象の値を持つ構文
		SecondaryConstructor, // 副次のコンストラクタ
		SectionKeyword, // 節を始める語
		SelectorHost, // セレクタを持つ構文
		SelfVal, // 自身を表す値
		SeparateParamParts, // 仮引数と既定値等を別の子に持つ定義
		SiblingLabeledBranch, // 先行する兄弟のラベルから脱出出来る条件分岐
		SimpleName, // 修飾を持たない名前
		SingleBodyStmt, // 単一の本体を持つ文
		SingleExprResultFunction, // 単一式の本体が暗黙に値を返す関数
		SizedDeclarator, // 定数式で幅を指定する宣言子
		SizedTypeSpecifier, // 幅を指定する型指定子
		StarGeneratorFunction, // 星の指定でジェネレータに為る関数
		StmtFlowContainer, // 制御の流れを順に調べる文のコンテナ
		StmtLabel, // 文へ付けるラベル
		StructuredBindingDeclarator, // 名前を組として導入する宣言子
		StyleLexeme, // 色・宣言末尾・空のミックスイン引数を正規化する構文
		Subscript, // 添字の指定
		SuppressingBody, // 本体の送出を後始末で抑止し得るリソースの管理
		SwitchBody, // 切替の枝を持つ本体
		SwitchEntry, // 切替の枝の項目
		SwitchLabel, // 切替の枝のラベル
		SwitchStmt, // 値に依る switch 文
		TerminatingMacroCall, // 終端が定まる標準マクロを呼べる構文
		ThisRef, // 現在の実体への参照
		TrailingReturnType, // 宣言子の後に置く戻値型
		TranslationUnit, // 翻訳単位
		TransparentLabelPrefix, // ラベルと本体の間で制御移動に影響しないアノテーション
		TrueVal, // 真の値
		TryStmt, // 例外を扱う文
		TupleExpr, // 組を表す式
		TupleLike, // 組の値又は組を分解するパターン
		TupleTypeComma, // 添字・型の組をカンマで区別する
		TypeCase, // 型で選ぶ切替の枝
		TypeModifiers, // 型の修飾子の並び
		TypeName, // 型の名前
		TypeQualifier, // 型の限定子
		TypedDecl, // 型又は性質を伴う宣言
		UnaryOperation, // 単項の演算式
		UnaryVal, // 単項の値
		UnconditionalLoopKeyword, // 常真の条件を専用の反復語へ揃える構文
		UnescapedAttribute, // 引用符の変換でエスケープを加えない属性
		ValArgs, // 値の実引数の並び
		ValParens, // 値の式を囲む括弧
		ValStmtSeq, // 末尾値を持ち得る文の並び
		ValSwitchRule, // 切替式の結果を与える枝
		VarDecl, // 変数の宣言
		VarDeclarator, // 変数を導入する宣言子
		VarDeclaratorItem, // 変数の宣言子の項目
		WhileExpr, // 条件判定の反復式
		WhileLoop, // 条件を調べ続ける反復
		YieldVal, // ジェネレータが送り出す値
		// 中間表現の構築と配置
		AdjacencySensitiveInfix, // 隣接空白で演算子の読みが変わる中置式
		AlternateBodyLayout, // コロンと終端語で本体を囲む構文
		AmbiguousBodyContainer, // 型の本体とも文の本体とも為り得るコンテナ
		AngleBracketList, // 山括弧で囲む型又は値の並び
		AnnotatedDefaultParam, // 型注釈と既定値を持つ仮引数
		AnnotatedParam, // 型注釈を持つ仮引数
		AnnotationConstructor, // アノテーションとして用いるコンストラクタの呼出
		AnnotationUnionHost, // 型注釈として合併演算を含み得る構文
		AnnotationUnionOperator, // 型注釈内の合併を表す演算子
		ArgSpread, // 値を展開して渡す実引数
		ArrayLiteral, // 配列を値として作る構文
		ArrowToken, // 関数型又は枝の向きを表す矢印のトークン
		AttachBracket, // 先行する構文へ角括弧を密着させるノード
		AttachColonOrDot, // 先行する名前へコロン又は点を密着させる構文
		AttachParen, // 先行する構文へ丸括弧を密着させる並び
		AttachedAccessor, // 直前の性質へ付属する読出又は書込の操作
		AttachedArrowLambda, // 矢印に仮引数を密着させるラムダ
		AttachedEmptyStmt, // 直前の構文へ付く空の文
		AttachedHashSplat, // 辞書の値を展開する記号と対象
		AttachedKeywordBrackets, // 先頭の語と専用の角括弧を密着させる式
		AttachedLabel, // 後続のコロンへ密着するラベル
		AttachedLowUnaryWord, // 結合の弱い単項の語と被演算子
		AttachedMagnitudeSuffix, // 数値へ大きさの接尾子を密着させる値
		AttachedNamespaceImport, // 名前空間の区切を密着させるインポート
		AttachedNegationTest, // 否定記号を検査の語へ密着させる式
		AttachedNumericUnit, // 数値と単位の接尾子を密着させる値
		AttachedPercent, // 数値へ密着する百分率の記号
		AttachedRangeOrUnary, // 範囲又は単項の演算子を被演算子へ密着させる式
		AttachedRefModifier, // 後続の対象へ密着する参照の修飾
		AttachedSelector, // 記号と名前を密着させるセレクタ
		AttachedSelectorNamespace, // 名前空間の区切を密着させるセレクタ
		AttachedTypeQuery, // 型又は寸法を問う語と括弧を密着させる構文
		AttributeDecl, // 宣言へ付ける属性の指定
		AttributeSelector, // 属性の値又は有無で要素を選ぶセレクタ
		AwaitCallHead, // 待機の語としても名前としても読める呼出の先頭
		BareCallArgs, // 括弧を省いて渡せる実引数の並び
		BaseRef, // 基底の実体を参照する式
		BinaryOperator, // Python の算術・ビット二項式の解析形（型結合の文脈を継承）
		BlockArg, // 呼出へ渡すブロックの実引数
		BraceAttach, // 型名へ波括弧を直結する複合値
		BraceNoSpace, // 空の波括弧を型名へ密着させる型
		BracedConditionalVal, // 波括弧の枝を持ち値を返す条件式
		BracketType, // 角括弧で始まる配列等の型
		BracketedType, // 角括弧の内側に型を指定する構文
		BreakColonHost, // 演算・キー値・見出の区切としてコロンを持つ構文
		BreakOpaque, // 内部の改行で意味が変わる不可分のトークン列
		BreakableDelimiterInterior, // セレクタ以外の文脈で，平坦時は密着し，内側を改行出来る括弧の並び
		CaptureInitializer, // 初期値を持つ捕捉の指定
		CaptureList, // 外側の変数を捕獲する指定の並び
		CastLikeExpr, // 型への変換を表す式
		CastLookalikeOperator, // 型変換と同じテキストへ誤読し得る二項式
		CastTypeAtom, // 括弧内で型名に相当する単独のノード
		ChannelType, // 値の送受信路を表す型
		ClassBodyContainer, // 型のメンバを直接並べる本体
		ClassLike, // 型又は名前空間を定義するコンテナ
		ClassSelector, // クラス名で要素を選ぶセレクタ
		CloserWord, // 語で開いた構文を閉じる終端語
		ClosureBlock, // クロージャとして渡すブロック
		ClosureBody, // クロージャの文を収める本体
		ClosureStmts, // クロージャの中に置く文の並び
		CodeOpeningTag, // 地の文からコードへ切り替える開始タグ
		CodeTextBoundary, // 地の文と埋込コードを区切るノード
		ColonBody, // コロンの後に文を並べる本体
		ColonBodyEnd, // コロンで開いた本体を閉じる語
		ColonBodyHost, // コロンを本体の開始に使う構文
		ColonControl, // コロンで見出又は節を区切る制御構文
		ColonPairHost, // 条件・継承・キー値等をコロンで区切る構文
		CombinatorSelector, // 要素間の関係を指定するセレクタ
		Compact, // 内部の空白と改行を抑える構文
		CompactInterpolationOperator, // 埋込式内で演算子を密着させる式
		CompactRecurse, // 密着する構文の内で折返候補を持つノード
		CompoundDeclarator, // 丸括弧で始まり得る複合宣言子
		ConditionalOperator, // 条件で値を選ぶ演算式
		ConstantCondHost, // 常真又は常偽の条件を持ち得る分岐
		ContentTagBoundary, // 内容の始め又は終りを示すタグ
		ContinuationHeader, // 折返時に式の継続を示す見出
		Control, // 条件又は本体を持つ制御構文
		ControlKeyword, // 制御構文を始める語
		DeclDecorator, // 後続の宣言へ付ける修飾
		DeconstructionType, // 分解代入で型を推論する指定子
		DeferredLiteralBody, // 開始記号から離れた位置に在る文字列の本体
		DeferredLiteralEnd, // 離れた位置の文字列本体を閉じる終端名
		DeferredLiteralStart, // 離れた位置の文字列本体を始める記号
		DelimitedMemberSeq, // 改行を区切としてメンバを並べるコンテナ
		DelimitedStmtSeq, // 文の間を改行又は区切で分ける並び
		DependentBranchLayout, // 後続の枝との位置関係で本体を配置する分岐
		DimensionNum, // 単位を後置する数値
		DirectBraceLayout, // 波括弧を直下に持つ本体の構文
		DirectControlBraceLayout, // 波括弧を直下に持つ制御構文
		DirectiveBlankBoundary, // 前後を空行で区切る条件付のプリプロセッサディレクティブ
		ElementText, // 要素内に表示する本文
		ElseLikeClause, // 分岐又は例外処理に続く節
		EmbeddedOpeningBoundary, // 地の文の後で埋込コードを開く境界
		EmptyCondBody, // 本体が空なら見出と終端語を繋ぐ条件分岐
		EmptyLoopBody, // 本体を省いても成立する反復
		EndDelimitedMethod, // 終端の語で閉じるメソッド定義
		EnsureSection, // 常に実行する後始末の節
		EnumeratorSeq, // 列挙項目を並べるコンテナ
		ExceptionClause, // 例外を捕捉する節
		ExclamationMark, // 否定又は値の存在をアサーションする記号
		ExpandContainer, // 本体を含む時に子を改行展開する括弧のコンテナ
		ExpandedTypeParams, // 改行展開する型仮引数の並び
		ExportDecl, // 外部へ公開する宣言又は値
		ExprTag, // 属性を持ち得る開始又は自己完結のタグ
		FieldList, // 型のフィールドを並べる本体
		FunctionType, // 仮引数と戻値で表す関数型
		GeneratorStarHost, // 星の記号でジェネレータを指定する関数
		GenericArgs, // ジェネリック化した構文へ渡す型引数の並び
		GenericFunction, // 型引数を伴う関数の構文
		GenericFunctionHead, // 型仮引数に先行する関数の見出
		GenericLookalikeOperator, // 型引数の閉記号に似る比較演算子を持つ式
		GroupedDecl, // 括弧で複数の宣言項目を束ねる宣言
		GroupedImport, // 括弧の内にインポート対象を纏める宣言
		GuardStmt, // 不成立時の本体を持つ条件文
		HardLineDirective, // 後続の構文と物理行を分けるディレクティブ
		IdSelector, // 識別名で要素を選ぶセレクタ
		ImportItem, // インポート対象の１項目
		ImportItems, // インポート対象を並べるコンテナ
		IndentContainer, // 子へインデントを付ける本体又はコンテナ
		Indexer, // 添字に依るメンバ参照を定義する宣言
		IndexerParams, // 添字の仮引数を囲む並び
		InferredArrayConstruction, // 要素から配列型を推論する構築式
		InfixOperator, // 左右を空白で区切る中置の演算又は結合
		InheritedClass, // 継承元を指定し得るクラスの定義
		InitHeader, // 初期化文を見出に含む条件又は反復
		InlineBraceBody, // 単一の文を波括弧の内へ同行で置ける本体
		IntMemberBoundary, // メンバ参照の点と区切を要する整数値
		InterpolatedLiteral, // 埋込式を含む文字列の値
		Interpolation, // 他の構文へ値を埋め込む部分
		InterpolationAlignment, // 埋込値の揃位置を指定する節
		IterationSection, // 反復の本体を始める節
		KeyPath, // メンバへの経路を値として表す式
		KeywordControlLayout, // 語で始め終える制御構文の本体
		KeywordExpandedContainer, // 文のブロックを含む時に語の本体を展開するコンテナ
		KeywordValAssignment, // 語で閉じる本体を値に代入する式
		LabeledPrefix, // 本体に先行するラベルを持つ構文
		LeadingColonBody, // 先頭のコロンで本体を開く節
		LeadingTypeArgs, // 先頭の型引数を後続の要素へ連結する構文
		LiteralContent, // リテラルの値・記法を構成するトークン
		LiteralText, // 地の文を表す本文
		MacroLike, // 名前と後続の要素を不可分に繋ぐ構文
		MarkupSyntax, // タグと本文で表すドキュメントの構文
		Member, // 点又は修飾記号でメンバを参照する式
		MisparsedTemplateHost, // 比較式を型引数として誤読し得る構文
		NamedInfix, // 名前で表す中置演算を持つ式
		NamedKeyword, // 名前付ノードとして現れる構文上の語
		NamespaceLike, // 名前空間又は内部モジュールの定義
		NegatableTestWord, // 否定記号を前置出来る検査の語
		NestingSelector, // 外側のセレクタを参照する記号
		NewParenCall, // 構築の語の後に実引数の括弧を置く構文
		NoDelimiterBreak, // 区切のトークンの後で折返さない構文
		NonKeywordClass, // 終端の語で閉じないクラス構文
		NormColon, // コロンの後を空白で区切る構文
		NumericUnitSuffix, // 数値へ接尾子を密着させる値
		ObjectLike, // 波括弧の内側に項目を置く値又は本体
		OpenRangeOperand, // 終端を省ける範囲式の被演算子
		OpenTailToken, // 後続の構文へ密着する記号
		OperatorFunctionDecl, // 演算子を名前として定義する関数宣言
		OperatorName, // 演算子を関数名として指定する構文
		OptionalChain, // 値の欠如を許してメンバ参照を連ねる構文
		OptionalInvocation, // 呼出又は添字へ任意連鎖の記号を密着させる式
		OptionalMarker, // 省略可能性を直前の構文へ付ける記号
		OptionalType, // 値の欠如を許す型
		OptionalTypeMember, // 名前に省略可能の記号を付けるメンバ又は仮引数
		PackExpansion, // 展開の記号 `...` を名前の前又は式・型の後へ付ける展開（C++ のパック展開）
		ParamContainer, // 関数の仮引数を収めるコンテナ
		ParenLeadType, // 丸括弧で始まる型の構文
		PatternKind, // 値を分解又は束縛するパターン
		PatternSection, // パターンとの一致を調べる節
		PipeParamList, // 縦線で囲む仮引数の並び
		PostfixAssertionMark, // 値の存在を後置でアサーションする記号
		PostfixOrUpdateExpr, // 後置演算又は値の増減を表す式
		PostfixPtrType, // 型名の後にポインター記号を付ける型
		PostfixReceiverChain, // 先頭の受手に後置操作を連ねる式
		PrefixLabel, // 後続の構文へ先行するラベル
		PreprocConditional, // 前処理の条件分岐を始めるディレクティブ
		PreprocDirectiveClose, // 前処理の後続枝又は終端を示すトークン
		PreprocDirectiveOpen, // 前処理の条件又は後続枝を始めるトークン
		PtrDecl, // ポインター・参照・展開の記号を伴う構文
		PtrType, // 参照先の型を指定するポインター型
		RangeAssignment, // 終端を省いた範囲を代入し得る式
		RangeLike, // 範囲を表す式又はパターン
		RangeOperator, // 範囲の始端と終端を結ぶ演算子
		RecoverableTemplate, // 型引数と比較式の誤読を復元し得る構文
		RefAssignment, // 参照先へ値を代入する式
		RenderedChildren, // 表示する子の要素を持つコンテナ
		RenderedText, // 要素の内側に表示する本文又は文字参照
		RescueSection, // 例外を受ける節
		ReturnTuple, // 戻値を組の型で表す関数又は型
		SelectorName, // セレクタの記号へ密着する名前
		SelfRef, // 現在の実体を指す式
		SeparatorToken, // 要素を区切るトークン
		SignedNumericType, // 負の値を持ち得る組込数値型の綴り
		SignedOperandHost, // 符号を伴い得る被演算子を持つ単項式
		SizeofExpr, // 大きさを求める演算（C++ の `sizeof...` は記号を語と括弧へ密着させる）
		SpaceKeyword, // 後続の構文と空白で区切る語
		SpaceSkip, // 内部の空白を原文通りに保つ構文
		SpacedAttributeTag, // 属性同士を空白で区切る開始タグ
		SpacedControlBody, // 見出から空白で分ける制御本体
		SpacedFunctionHead, // 関数の見出と後続の括弧を空白で分ける宣言
		SpacedValue, // 前の値から空白で区切る値の要素
		SpacelessBrace, // 波括弧の内側へ空白を挟めない式
		SpreadParam, // 残りの引数を受け取る仮引数
		StmtList, // 文を順に収めるコンテナ
		StmtsOnlyBreak, // 見出を保ち文の並びだけを改行する節
		StrInterpolation, // 文字列へ埋め込む式又は名前
		StrSeqContainer, // 区切無で文字列又はシンボルを並べるコンテナ
		SwitchBodyLayout, // 枝の文をインデントして並べる切替の本体
		SwitchStmtLike, // 複数の枝を選ぶ switch 文
		SwitchVal, // 複数の枝から値を選ぶ切替式
		TagElement, // 開始又は自己完結のタグを子に持つ要素
		TagSyntax, // 内容の境界を山括弧のタグで表す構文
		TemplateInterpolation, // 雛形の文字列へ埋め込む式
		ThenSection, // 条件の成立側の本体を始める節
		TightAnonymousForward, // 名前を省いた引数転送の記号
		TightHeaderEquals, // 見出の等号を前後へ密着させる構文
		TightParamEquals, // 仮引数の既定値の等号を密着させるノード
		Transparent, // インデントの深さを変えず子を展開するコンテナ
		TypeAnnotation, // 対象の型を注記する構文
		TypeCombination, // 合併又は交差の記号で型を組み合わせる構文
		TypeSlot, // 型の構文を収めるノード
		TypedLambda, // 型又は修飾を仮引数の前に置けるラムダ
		TypedSwitchLayout, // 値又は型で枝を選ぶ switch 文
		UnaryPreOrUpdate, // 被演算子の前置演算又は値の更新
		UnaryPrefixToken, // 前置の単項演算を表す記号
		UnbrokenBaseRef, // 内部の改行を許さない基底の参照
		UnbrokenExceptionBinding, // 捕捉節の見出から分離出来ない例外の束縛
		UnbrokenImport, // 内部の改行を許さないインポートの宣言
		UnbrokenOptionalType, // 省略可能の記号から型を分離出来ない構文
		UnbrokenParenSyntax, // 括弧又は語の内部で改行出来ない構文
		UnbrokenStrSeq, // 隣接する文字列を分離出来ない並び
		UnionType, // 複数の型から選べる合併型
		UnitText, // 単位を表す文字列
		ValArg, // 値を渡す実引数
		ValArm, // 値を選んで結果にする分岐の枝
		ValJump, // 制御移動又は値の受渡を行う式
		VariadicTypeParam, // 可変長の型仮引数
		WordArray, // 語又はシンボルを並べて作る配列
		WordArrayClose, // 語の配列を閉じる記号
		WordArrayOpen, // 語の配列を開く記号
		// 静的解析
		AccessModifier, // 公開範囲を定める修飾子
		AccessOrderMember, // 公開範囲の順序を照合するメンバ
		AccessOrderedBody, // アクセス範囲をメンバから読む本体
		AccessOrderedClass, // アクセス範囲の順序を調べる型
		AccessSection, // 公開範囲の領域の見出
		AddressOperation, // アドレスを取得する演算
		AlternativeClause, // 分岐の不成立側の節
		AssignableWrapper, // 代入先として読める式のラッパー
		AssignmentOperation, // 単純代入又は複合代入の式
		BoolFlipInspection, // 真偽値の反転候補を含み得る代入
		BoolFlipName, // 真偽値の自己代入で照合する単純名
		CastInspection, // 型付キャストへの置換候補
		ConcatenatedLiteral, // 文字列の暗黙の連結
		CondFieldHost, // 条件を子の役割で指定する構文
		CondInitializer, // 条件に先行する初期化の文
		CondMergeInspection, // 短絡条件を統合出来る分岐
		Conditional, // 条件と成立時の本体を持つ分岐
		ConditionalAlternative, // else の位置で自身が条件と本体を持つ分岐（else 節に包まれない else if の if ･ PHP の elseif 節）
		ControlTransfer, // 制御を移す文
		DeclInitDeclarator, // 初期化子を伴う宣言子
		DeclLike, // 一括宣言出来る宣言
		DeclWithType, // 名前に型を付ける宣言又は仮引数
		DeclaratorWrapper, // 仮引数又はメンバの宣言子のラッパー
		EmbeddedExpr, // 地の文へ埋め込む式
		EnumerationBody, // 列挙型の本体
		FieldDef, // メンバの型と宣言子
		FlowExit, // 返戻・脱出・跳躍・送出に依る制御の移動
		FunctionNameIdentifier, // 関数名として使う単純な識別子
		FunctionScope, // 関数の範囲の境界（定義・関数式・ラムダ・ローカル関数）
		GotoInspection, // 多重脱出の用途を調べる跳躍
		IdentifierLeafLike, // 名前のテキストだけを持つ識別子
		ImplicitPrivateClass, // 既定の公開範囲が private の型
		ImplicitPublicMembers, // メンバの既定の公開範囲が public の型
		IncrementInspection, // 数値の１加減算候補を含み得る代入
		InstanceMethodDef, // メソッドの定義
		IntLiteralCandidate, // 整数リテラルの候補
		JumpExpr, // 脱出又は送出の式
		JumpWithLabel, // ラベルを跳先として持つ脱出の式（中のラベルは定義でなく参照）
		LabelInspection, // 多重脱出の用途を調べるラベル
		LabeledConstruct, // 跳躍先のラベルを持つ文
		LabeledLoop, // ラベルで脱出先を指定出来る繰返
		LiteralInterior, // 内部のテキストを保つ文字列又はコメント
		MacroConstantInspection, // 定数マクロの検査対象
		MemberBindingModifier, // 解決先からメンバの所属・プロパティを確認する修飾
		MemberConstructor, // 生成・初期化を担うメンバ
		MemberDeclCall, // 型の関係又は実体の状態を宣言出来る呼出
		MemberDestructor, // 破棄・終了を担うメンバ
		MemberDestructorName, // メンバ順序の破棄子名
		MemberDocumentableCall, // 確認済の標準呼出に依るドキュメント化対象
		MemberEmbeddedField, // 名前を付けず型を埋め込めるフィールド
		MemberFieldRank, // フィールド・プロパティ・定数・イベントの宣言（メンバ順位２）
		MemberInstanceState, // 確認済の標準呼出が宣言する実体毎の状態
		MemberMethodRank, // メンバ関数・メソッド・コンストラクター（アクセス順位３）
		MemberModifier, // メンバの修飾又はアノテーション
		MemberOrderBody, // メンバの宣言順を照合する並び
		MemberOwner, // 型又は型へ操作を付ける宣言
		MemberPrivateName, // 名前の構文がメンバ自身の私有の可視性を定める
		MemberProperty, // 確認済のデコレータが宣言するプロパティ
		MemberReceiver, // 実体を受ける仮引数
		MemberRelation, // 型との関係・構成を定める宣言
		MemberStaticOwner, // 静的なアノテーションの効果を確認出来る単体の所有者
		MemberStorageSpecifier, // 静的なメンバの判定に使うメモリ指定
		MemberTypeFields, // フィールドが暗黙に型へ属する本体の所有者
		MemberTypeOperation, // 型に属する操作
		MemberTypeRank, // 型エイリアスとネスト型宣言（アクセス順位１）
		MemberTypeState, // 型に属する定数又は状態
		MemberVisibilitySection, // 後続のメンバの公開範囲を切り替える記法
		MemberWrapper, // アノテーション付定義又は式文の包み
		ModifierSeq, // 修飾子の並び
		MutatingExpr, // 変数の値を書き換える式
		NameDeclarator, // 名前と初期値又は型を包む宣言子
		NamedFunctionDef, // 言語横断の関数・メソッド定義
		NamedParamKind, // 単一名を持つ仮引数
		NestedBreakTarget, // 外側の脱出先との区別を要する構文
		OptionalDeclType, // 明示出来る型を省いた宣言子の検査対象
		OptionalParamType, // 明示出来る型を省いた仮引数の検査対象
		OrdinaryFunctionDef, // 通常関数（名前を持つ関数・メソッドの定義，コンストラクタ，デストラクタ，アクセサ）
		PostfixInspection, // 後置増減の候補を含み得る更新式
		PrefixUnary, // 前置単項演算子
		RepeatedControl, // 条件又は列挙に依り本体を繰り返す構文
		RescueExpr, // 後置の例外回復式
		ReturnHintContext, // 戻値型明示の可否を所属と見出から確定するメソッド
		ReturnHintRequired, // 戻値の型注釈を要する定義
		ReturnStmt, // 明示的な返戻文
		ReturnValList, // 返戻値を並べるコンテナ
		ScopeContainer, // 関数・メソッド・プログラムの本体スコープ
		ScopeImport, // 他の範囲の名前を取り込む宣言（using の宣言・ディレクティブ）
		SingletonMethod, // 単一の受手のメソッドの定義
		StrLikeAll, // 言語横断の全文字列リテラル
		StrictEqualityInspection, // null と undefined の論理比較を含み得る式
		SwitchGotoTarget, // 値に依る分岐の選択肢への跳躍先
		ThrowKeyword, // 送出を示す語
		ThrowStmt, // 値の送出文
		ThrowVal, // 値の送出式
		ThrowsDecl, // 送出する例外の宣言
		TypeHintBindingGroup, // 個別の型指定を持つ分解の束縛の並び
		TypeHintExternalBinding, // 同じ範囲では型注釈を書けない外側の束縛の宣言
		TypeHintParamContainer, // 型の明示を検査するクロージャの仮引数列
		TypeInferenceInspection, // 明示型の代替を調べる型
		TypeNameNode, // 宣言に型を指定する名前
		UnaryPrefixExpr, // 被演算子の前に演算子を置く単項式
		UncertainLocalBindings, // 動的な導入又は外部の束縛を含む構文
		UnusedValHost, // 式の結果を捨てる文の位置
		UserDefinedOperator, // 利用者定義の演算子名
		UserType, // 利用者定義の型
		VarDeclStmt, // 変数の名前と初期値を導入する宣言
		ZeroLiteral, // 純粋仮想関数の０指定に使う数値リテラル
		/**
		 * ========== 括弧・波括弧・宣言の正規化 ==========
		 */
		AbstractArray, // 名前を持たない配列の宣言子
		AbstractFunction, // 名前を持たない関数の宣言子
		AbstractPtr, // 名前を持たないポインタの宣言子
		AlgebraicTypeParen, // 合併・交差・条件型の結合を指定する型の括弧
		AliasPattern, // 名前を束縛する照合
		AlternativePattern, // 選択肢を持つ照合
		Arg, // 実引数のラッパー
		AtomicConstruction, // 後置の受手の原子に為る構築
		AtomicNum, // 後置の受手に置ける数値
		AttributeSensitiveNum, // Python の数値リテラル (float / integer)
		AwaitNameReceiver, // 待機の語を名前と読んだ可能性の有る後置の受手
		AwaitTryCastExpr, // `await` / `try` / `unsafe`／型キャストの式
		AwaitVal, // 単項の待機値
		BindingCond, // 条件の中の束縛
		BindingMarker, // 束縛の語のラッパー
		BindingName, // JS/TS の束縛の名前
		BlockEndingParen, // ブロックでの文終端と脱出値の境界を保つ括弧
		BlockExprStmtHost, // 文頭の式を持つ Rust のノード
		BodyAmbiguousControl, // 波括弧の本体と見出の値を区別する制御構文
		BodyAmbiguousHeader, // 本体に似た式を見出で保護する構文
		BorrowExpr, // 値を借用する式
		BorrowedTypeOperandHost, // 型の被演算子を取る Rust の型
		BorrowingMatch, // 照合のパターンが対象を借用し得る式
		CallAttributeOrSubscript, // Python の後置の式 (attribute / call)
		CallableType, // 戻値の型の中で条件の型を再び許す TS の関数の型
		CalleeNeedsGrouping, // 括弧を外すと呼出の意味が変わる受手
		CapturedPattern, // 照合の対象も名前へ捕捉するパターン
		CasePattern, // 分岐の照合のラッパー
		CastAmbiguousParen, // 型変換と式の区別を保つ括弧
		CastBeforeUnary, // 後続の単項と読みを争う型変換
		CastExpr, // 型を左側に置く変換
		CastTypePart, // C/C++ キャスト `(type)` 内側に出現し得る型部位（丸括弧削除抑止対象）
		CatchTypeAlternatives, // 捕捉する型の選択肢
		ChannelReceive, // チャネルの受信を取り得る前置式
		ClosureExpr, // 捕捉を伴う無名関数の式
		CmpInfix, // テキストから比較の優先順位を取得する中置式
		ColonSensitiveInterpolation, // コロンを区切として読む補間
		CommandArgParen, // 括弧無の実引数と語の単項の結付を保つ括弧
		CommandCall, // 括弧無の実引数を取り得る呼出
		CommandStmtHost, // Ruby の文が直接置かれるコンテナ— 値位置との判別
		CompactConstructor, // 仮引数を宣言から受け継ぐコンストラクタ
		ComposedTypeCast, // 合成型を右側に取る型変換
		ComposedTypeOperandHost, // 型の被演算子を取る Swift の型
		CompositeTypeName, // 型名の形の Go の型
		ComprehensionFilter, // 内包表記の条件
		ComprehensionIteration, // 内包表記の反復対象
		ComprehensionScope, // 内包表記の導入が本文と後続の節を遮蔽する範囲
		ConditionalAccessChain, // C# の後置式の連鎖
		ConditionalAccessExpr, // 条件付のメンバ参照
		ConditionalAccessParen, // 任意の連鎖の境界を保つ括弧
		ConditionalExpr, // 条件と値の選択
		ConditionalTailOnly, // 最後の枝だけに同格の条件式を置ける条件式
		ConditionalType, // 条件に依って選ぶ型
		ConstructionReceiverParen, // 構築と呼出の受手の区別を保つ括弧
		ConversionTypeParen, // 式の変換とチャネルの向きを保つ型の括弧
		DeclaratorHead, // 宣言の中で宣言子を始めるノード
		DeducedTypeKeyword, // 型を推論する語
		DeducedValCategoryName, // 括弧で推論する値の種類が変わる名前
		DeleteStmt, // 削除の対象を並べる文
		DelimitedIterationTarget, // 開区切の後に反復対象を置く見出
		ExprStmtLiteral, // expression_statement 直下の丸括弧削除を抑止するリテラル系（数値／文字列／真偽値）
		FieldExpr, // フィールドを参照する式
		FinalizableLocal, // 不変の修飾子を追加出来るローカル宣言
		FoldExpr, // 演算子で並びを畳込む式
		ForEachHeader, // 反復対象を区切語の後に置く見出（対象の括弧除去用）
		ForInStmt, // 束縛と反復対象を持つ反復
		GeneratorExpr, // Python の括弧で囲む生成式
		GlobalStmt, // 最上位の文のラッパー
		GreedyTailExpr, // 右端を貪欲に伸ばすか，呼出時だけ括弧を要する式
		GreedyType, // 右端を貪欲に伸ばす TS の型
		GroupingTypeParen, // 型を包む括弧のノード
		HeaderBodyLikeVal, // 見出で本体の開きと紛れる値
		ImportAlias, // インポートの別名を包むノード
		ImportAliasOnly, // 別名を指定する場合だけ単独の名前を導入するインポート
		ImportAllNamesMarker, // １つの名前でなく範囲の全ての名前を取り込む語
		ImportKindMarker, // 取り込む名前の種別を指定する語
		ImportPathGroup, // 同じ供給元の下にインポート項目を並べるコンテナ
		ImportPathItem, // 供給元と別名を持つインポートの項目
		ImportRequiresKind, // 種別の指定が在る場合だけ単独の名前を導入するインポート
		ImportSelfName, // グループの修飾名自身を導入するインポートの語
		ImportStmt, // 外部の定義を取り込む文
		ImportWildcard, // 全ての名前を指定するインポート
		ImportedMemberBinding, // 指定した供給元から名前の場の各項目を導入するインポート
		ImportedModuleBinding, // 名前の場の各モジュールの先頭名を導入するインポート
		ImportedPathBinding, // 修飾名の末尾又は別名を導入するインポート
		InferredType, // 制約から推論する型
		InnerExpansionAttribute, // 所属する本体を変換し得る属性
		IntersectionType, // 交差の型
		InvocationExpr, // 名前又はメンバの呼出
		IterationHeader, // 反復見出（対象前の `in`/`as` は演算子外）
		KeywordCastOperand, // 語の型への変換の値の囲みを省ける構文
		KeywordExprPrec, // 待機・後置・条件・否定の構文に依る優先順位
		KeywordUnary, // 語や記号で前置する単項の式— 演算子の優先順位は前置単項
		LeftAbsorbingTypeCast, // 括弧無では手前の二項式全体を値として取り込む型変換
		LogicalNegation, // 比較より弱く結合する否定
		LowKeywordPrec, // 語の論理演算と同じ最弱順位を持つ構文
		LowPrecPatternTest, // Ruby のパターン照合の式
		LowPrecUnaryKeyword, // 代入より弱い Ruby の単項の語（定義を問う `defined?`・否定の `not`）
		MemberExpr, // 受手のメンバを取得する式
		MemberMutationTarget, // JS/TS のメンバ・添字の参照
		MembershipExprBoundary, // JS/TS の `in` を二項演算子に戻す（for 文の初期化子の外に為る）式（args / 計算のキー / フィールド / 対 / 括弧 / ブロック / 添字 / 補間）
		MutablePattern, // 可変として束縛する照合
		MutablePatternBinding, // 可変の指定を除去出来るパターンの束縛
		MutableSpecifier, // 可変の指定
		MutationBaseTraversal, // Rust の mut 削除候補から基底名を辿るノード
		NamedCompositeBody, // 型名を伴うと本体の開きと紛れる複合値
		NamedInfixCall, // 固定の優先順位を持つ名前の中置式
		NamedVal, // 名前を束縛して値を得る式
		NavigationSuffix, // メンバへの航行の接尾辞
		NestedLoopBodyBraces, // ネストの反復本体の波括弧を要する後判定の反復
		NonNullableType, // 非 null の型の合成
		NormalFormType, // 選言標準形の型
		NormalizedBindingName, // 正規化に依って同一名の照合が変わり得る束縛名
		NullableTypeOperandHost, // 型の被演算子を取る Kotlin の型
		OpenPrefixExpr, // 語で始まり右の被演算子を式の終わり迄取る式（`yield` / `include` / `print`／ラムダ／`return` 等）— 演算子の優先順位は最も弱い
		OperatorBoundaryParen, // 単項・範囲・待機の記号の境界を保つ括弧
		OperatorCall, // 演算子を呼出側に持ち得る呼出
		OptionalCondParen, // 条件の囲みを省ける見出
		OptionalNavigationMark, // 任意の航行の接尾辞
		OptionalValJump, // 値を取り得る Rust の脱出の式
		OuterExpansionAttribute, // 後続の宣言を変換し得る属性
		ParamScope, // Java の仮引数スコープを開くノード
		ParamScopeBoundary, // Java の型の本体
		ParenInnerFirstChildNoUnwrap, // 内側の最左の子孫が此の型なら括弧を剥がさない（Ruby の多重代入の左辺・Python の条件式を値に取るセイウチ演算子）
		ParenInnerNoUnwrap, // 即時実行・カンマ式・貪欲式等，丸括弧を外せない式
		ParenValSlot, // 括弧が値の全体を包み後にトークンが続かない値の位置（代入の右辺・実引数・戻値・要素・仮引数の既定値）
		Pattern, // 値を照合するパターン
		PostfixAwait, // 後置の待機
		PostfixCallHost, // 被呼出側を最初の子に持つ呼出
		PostfixExprHost, // 式を左端の受手とする呼出・添字・メンバ参照
		PostfixReceiverLeaf, // 括弧無で後置の式の受手に置ける名前・トークンで閉じるリテラル
		PostfixTry, // 後置の試行
		PostfixValOperation, // 左端の式を受手とする値の演算
		PrefixAbsorbsInfix, // 中置の被演算子から結合を取得する前置式
		PrefixAbsorbsTypeCast, // 後続の型アサーションを被演算子へ取り込む前置式
		PrefixAwait, // 前置の待機
		PrefixExpr, // 前置の演算子を持つ式
		ProtocolComposition, // 適合条件を合成した型
		RangeExpr, // 両端又は片端を指定する範囲
		RangeOrUnaryExpr, // Kotlin の増減式及び Swift の範囲／単項演算子親
		RangeWithoutEnd, // 終点の無い範囲
		RangeWithoutStart, // 始点の無い範囲
		ReceiverMutationCall, // 受手の基底の束縛を変更し得る呼出
		RecordDecl, // メンバの仮引数を持つ記録型の宣言
		RefBindingPattern, // Rust の参照・フィールドのパターン
		RefModifier, // 参照として扱う指定
		RefPattern, // 参照を束縛する照合
		RefValParen, // 型・条件・参照返戻の読みを保つ括弧
		RemovablePatternParen, // 単一の照合を囲む括弧を省略出来る位置
		RemovableTypeParen, // 単一型を包む時に省略可能な型の括弧
		ReopenableTypeCast, // 逆側の同格演算子迄型を伸ばし得る変換
		ReplaceableBindingKeyword, // 束縛の語を不変の語へ置換出来る宣言
		RestParamPart, // 仮引数の残余又は可変長の指定
		RestrictedExprOperand, // 条件・数値の属性・待機の被演算子に制約の有る構文
		ReturnValHost, // return の返す値の参照の有無を決める関数（C++ の decltype (auto) / PHP の参照返）
		ReturnValParent, // 返す値を直接の子に持つノード
		SeqExpansion, // 並びの展開
		StmtEndingBlock, // 文頭に置くと `}` で文を終える Rust のブロックの式
		SwitchOrWithExpr, // switch 式及び C# の with 式
		SwitchWithPrec, // 呼出・値分岐・複製の優先順位を持つ式
		TernaryExpr, // 条件と２つの枝を持つ式
		TrailingTypeCast, // 型を右側に置く変換
		TransparentTupleParen, // 単一の式を包む透過可能な組の括弧
		TupleParamFunctionType, // 組の括弧で仮引数を表す関数型
		TuplePattern, // 組の要素を照合するパターン
		TupleTypeItem, // 組の型の要素
		TupleVal, // 組の値
		TypeArgAmbiguousParen, // 比較と型引数の区別を保つ括弧
		TypeArgLookalikeSlot, // 比較式が型引数列に見え得る一覧要素の位置
		TypeBounds, // 型の制約の並び
		TypeCastRight, // 型を右に取る変換の式
		TypeColonParent, // `:` の右を値・型とするキー値対・型のフィールド
		TypeConversion, // 型を指定した式の変換
		TypeElements, // 型集合の要素
		TypeList, // 型の並び
		TypeOperatorInner, // 演算子を持つ型
		TypePackExpansion, // 型の並びの展開
		TypeSwitch, // 型で分岐する文
		UnambiguousConversionType, // 括弧無で型の変換に置ける Go の型
		UnaryReceiverCast, // 前置単項を受手に取れる型変換
		UnparenthesizedBindingValSlot, // Python 3.8 から代入式を括弧無で置ける要素・実引数
		UnwrappedSwitchSubject, // 対象の括弧を省ける値分岐
		UpdateExpr, // 前置又は後置の増減
		ValBindingPattern, // 値を束縛する照合
		ValCategoryType, // 式の値の種類を調べる型
		WrappedTypeItem, // 型の要素の包みを持つ型の括弧
		WritableAssignment, // Rust 代入式系
		// 入力側の読取（整形仕様 4.3，文法の定義がノード名から与え，入力側と検証側が読む）
		AdjacencyRecordingHost, // 子の間の原文の隣接が読みを決めるノード
		AdjacencyRecordingOperand, // 前の子との原文の隣接が読みを決める子
		AmpersandToken, // 参照又は論理積のトークン
		AngleCloseCandidate, // 型引数の閉じと紛れる字を含むトークン
		AngleCloseToken, // 閉じの山括弧で始まるトークン
		AngleConflictToken, // 型引数と比較の読分けに関わる山括弧を含むトークン
		ArgListHost, // 実引数の並びを子に持つノード
		ArgStartToken, // 名前の後に空白を置いて続けると実引数の始まりに為るトークン
		ArrayRank, // 配列型の次元指定
		ArrayTypeSyntax, // 配列型の構文
		ArrowFunctionNode, // 矢印で本体を指定する関数
		AssignmentToken, // 等号のトークン
		AsteriskToken, // 乗算又は間接参照のアスタリスクのトークン
		AtToken, // アノテーション又は参照を始めるトークン
		AttributeOpenToken, // 属性を開く番号記号と角括弧のトークン
		AwaitKeyword, // 待機の演算子として読む語
		BackslashToken, // バックスラッシュのトークン
		BareTypeDecl, // 裸の名前を型として導入する宣言
		BaseReceiverKeyword, // 基底の実体を指す受手の語
		BinaryPrecExpr, // Swift の演算子の優先順位を修復する二項式の解析形
		BindingBaseClause, // 所属を解決するクラスの基底列
		BindingExpr, // 名前を束縛する式
		BlockCloseKeyword, // 語で始めた本体を閉じるトークン
		BlockCommentNode, // ブロックコメントのノード
		BlockParamList, // ブロックの仮引数の並び
		BodyDelimiterLead, // 見出の値の内側を区切る先頭記号
		BodyEndingClause, // 最後の子に本体の文を持つ節
		BodyTriggerWord, // 制御の本体を始める語
		BracedHeaderStmt, // 見出の後に本体の波括弧が続く文
		BracedStmtBlock, // 波括弧で文を並べるブロック
		CallableRef, // 呼出可能な対象への参照
		CaseKeyword, // 枝の条件を始める語
		CastContinuationToken, // 型変換の後へ続く単項記号のトークン
		CastKeyword, // 型変換を表す語
		CastOperandLead, // 型変換の被演算子と紛れる先頭記号
		CastUpdateFollowerLead, // 更新式の後で型変換と紛れる先頭記号
		ChangeByOneToken, // 値を１つ増減するトークン
		ChannelReceiveToken, // 通信路から値を受ける演算子のトークン
		ClassMember, // 型の本体のメンバ
		ClauseHeading, // 節を始める見出（言語を問わない，コメントの所属の判定に使う）
		ClosedValTerminator, // 値の位置を閉じる複合区切
		ClosingBraceLead, // 閉波括弧で始まるトークン
		ClosingBraceToken, // 閉波括弧のトークン
		ClosingBracketLead, // 閉括弧で始まるトークン
		ClosingParenLead, // 閉丸括弧で始まるトークン
		ClosingParenTail, // 丸括弧を閉じる記号で終わるトークン
		ClosingParenToken, // 丸括弧を閉じるトークン
		ClosingSquareLead, // 角括弧を閉じる記号で始まるトークン
		ClosingSquareToken, // 角括弧を閉じるトークン
		ClosingTagOpenToken, // 閉タグを始めるトークン
		CoalescingToken, // 欠損値を選ぶ二重疑問符のトークン
		ColonLead, // 節又はフィールドの区切で始まるトークン
		ColonToken, // 節又はフィールドを区切るコロンのトークン
		CommaLead, // 並びの区切で始まるトークン
		CommaTail, // 並びの区切で終わるトークン
		CommaToken, // カンマのトークン
		CommandArgExtent, // 命令形の呼出の実引数が左端から続く式
		CommandArgHost, // 命令形の呼出の実引数の読みを揃えるノード
		CommentNode, // コメントのノード
		CompositeLiteral, // 複合値のリテラル
		CondBodyDelimiterToken, // 条件と本体を分けるトークン
		ConditionalOperatorExpr, // 真側を区切の記号で閉じる条件式
		ConstantConditionKeyword, // 定数式の条件を指定する語
		ConstantDeclNode, // 定数の宣言ノード
		ConstantSpec, // 定数の宣言項目
		ConstructionKeyword, // 実体の生成を始める語
		ContentElement, // 開始タグと終了タグで内容を囲む要素
		ContinuingWord, // 後に続くトークンを要し，後の改行で文を終えない語
		DebugInterpolation, // 等号を付けると式のテキストも表示する補間
		DeclExpr, // 宣言を値として扱う式
		DefinitionKeyword, // 名前付の定義を始める語
		DelimitedList, // 括弧で囲んだ実引数・仮引数・要素の並び
		DelimitedLiteral, // 区切文字付のリテラル
		DocTypeDecl, // 文書型の宣言
		DocumentedTopDecl, // 直前のコメントをドキュメントとする最上位の宣言
		DotToken, // 点のトークン
		DoubleColonToken, // 二重コロンのトークン
		DoubleSquareCloseToken, // 二重角括弧を閉じるトークン
		DoubleSquareOpenToken, // 二重角括弧を開くトークン
		ElementSeq, // 名前の後の実引数が残りの要素を取り込む要素の並び
		EmbeddedCodeElement, // 別の言語のコードを中身に持つ要素
		EmbeddedScriptElement, // 埋込の実行コードを持つ要素
		EmptyParensLead, // 空の丸括弧で始まるトークン
		EnumerationItem, // 列挙の項目
		ErroneousEndTag, // 誤読された終了タグ
		EscapeLiteralHost, // トークン化前のエスケープを内側に持つリテラル
		ExclamationToken, // 感嘆符のトークン
		ExhaustiveCaseLabel, // 切替の枝のラベルに置くと処理系が切替へ網羅を要求する照合と値
		ExhaustiveSwitch, // 不一致で後続へ進まず，送出又は待機する場合も含む網羅の切替
		ExponentToken, // 冪を表す演算子のトークン
		ExprContinuationLead, // 前の式へ続く演算子・参照の記号で始まるトークン
		ExprContinuationTail, // 後の式へ続き文を終えない記号で終わるトークン（後置の増減を除く）
		ExprTypeName, // 式としても読める型名
		FallthroughKeyword, // 次の枝へ進む語
		FatArrowToken, // 右向きの太い矢印のトークン
		FollowingBodyClause, // 閉じの波括弧に続く本体付の節
		FunctionDeclNode, // 関数の宣言ノード
		FunctionKeyword, // 関数の見出を始める語
		FunctionLiteral, // 関数の値
		GreaterThanToken, // 大なり記号のトークン
		HashToken, // 番号記号のトークン
		HeaderExprLead, // 見出の式の前のトークン
		HeredocAmbiguousShift, // 複数行文字列の開始と紛れるシフトのトークン
		HeredocBody, // 複数行文字列の本体
		IfFollowingBranch, // if の後続の枝の節
		IfStmtNode, // 条件分岐の文ノード
		ImportDeclNode, // インポートの宣言ノード
		ImportLeaf, // インポートの表現の葉
		IncludeStmtNode, // インクルード文のノード
		InstantiationContinuationLead, // 具体化した式に続くと解釈を変える先頭記号
		IntLiteralVal, // 整数の値のノード
		InterpolationCloseToken, // 文字列の直接の子として埋込式を閉じるトークン
		InterpolationFormat, // 補間の書式指定
		InterpolationFormatSeparator, // 補間の書式区切と紛れるトークン
		InterpolationOpaqueLead, // 補間内で区切を隠す値の先頭記号
		InterpolationOpenToken, // 埋込式を始めるトークン
		IsolatedScope, // 外のローカル変数を見ない有効範囲を開くノード
		IterationBodyDelimiterToken, // 反復と本体を分けるトークン
		IterationKeyword, // 反復の対象を導入する語
		JointPunctuationToken, // 隣接する記号との密着がトークン列の意味に為る記号
		LabelName, // ラベルの名前
		LabeledReceiverKeyword, // ラベル付の受手を始めるトークン
		LaggingGrammarError, // 文法の追随遅れで正当な構文にも立つエラー領域
		LeadingBreakToken, // トークンの前で折り返す閉じ又は参照の記号
		LeadingClauseToken, // 行頭で節の境界を表すトークン
		LeftAssignmentList, // 代入の左辺の並び
		LessThanToken, // 小なり記号のトークン
		LineCommentNode, // 行コメントのノード
		LineContinuation, // 次の行へ文を続ける行継続
		LineJoiningSuffix, // 改行を跨いで前の式へ繋がる接尾辞・参照・中置式・返戻値
		LineTerminatedCtx, // 改行が文を終える文脈
		LocalParams, // ローカル変数を導入する仮引数の並び
		LocalScope, // ローカル変数の有効範囲を開くノード
		LowPrecBinaryKeyword, // 命令形の呼出の実引数の外に在る語の二項演算子
		MacroBody, // 行末迄のトークンを１つに呑むマクロの本体
		MacroDef, // 置換本体を持つマクロの定義
		MacroGuardedParens, // マクロの展開で意味が変わり得る値の括弧
		MacroNameQuery, // 名前がマクロかを問う前処理の条件
		MacroRuleDef, // 規則でトークン列を書き換えるマクロの定義
		MacroRulesToken, // マクロ規則を始めるトークン
		MarkupDeclOpenToken, // ドキュメント宣言を始めるトークン
		MarkupElementNode, // 開始タグと中身を持つ要素
		MeaningfulParenGroup, // 型・値・パターンの意味を定める括弧のグループ
		MemberAccessArrow, // メンバ参照の矢印のトークン
		MemberAccessLead, // メンバ参照の記号で始まるトークン
		MemberAccessTail, // メンバ参照の記号で終わるトークン
		MetatypeNode, // 型自身を指す型
		ModuleRule, // モジュールを読む規則
		ModuleStmt, // モジュールのマーカーの文
		NameDecl, // 宣言子を名前迄剥がして名前を得る宣言
		NameToken, // 比較用の名前を投影する名前・ラベルのトークン
		NamespaceName, // 名前空間の名前（宣言と修飾の名前）
		NestingDeclarator, // 宣言子を包む宣言子
		NonNullAssertionToken, // 値の非空をアサーションするトークン
		NonNullExpr, // 値の非 null アサーションの式
		ObjectLiteralNode, // オブジェクトの値の構文
		OpaqueLiteral, // 中身をコードと読まない文字列・正規表現のリテラル
		OpenRangeToken, // 終端を省ける範囲の２点のトークン
		OpeningAngleLead, // 山括弧を開く記号で始まるトークン
		OpeningBraceLead, // 波括弧を開く記号で始まるトークン
		OpeningBraceTail, // ブロックを開く波括弧で終わるトークン
		OpeningBraceToken, // 本体又はコンテナを開く波括弧のトークン
		OpeningBracketLead, // 囲みを開く記号で始まるトークン
		OpeningBracketTail, // 開括弧で終わるトークン
		OpeningBracketToken, // 開角括弧のトークン
		OpeningParenLead, // 丸括弧を開く記号で始まるトークン
		OpeningParenToken, // 開丸括弧のトークン
		OpeningSquareLead, // 角括弧を開く記号で始まるトークン
		OperandHeadedAccess, // 被演算子を先頭の子に取る参照・呼出
		OperatorPrecBoundary, // 演算子の優先順位が結付を区切る式
		OptionalAccessToken, // 省略可能なメンバ参照のトークン
		OptionalArrowToken, // 省略可能な右向きの矢印のトークン
		ParamRule, // 仮引数の並びを持つ規則
		PhpEndTag, // PHP の終了タグ
		PipeLead, // 選択又は論理和の記号で始まるトークン
		PipeToken, // 論理和又は選択を表す縦線のトークン
		PlaceholderNode, // 空フィールドを表すノード
		PlainValWord, // 値の中の裸の語
		PostfixBindingExpr, // 後置の演算が被演算子へ結び付く式
		PostfixBindingOperand, // 後置の演算を右端へ結び直す被演算子の式
		PotentialExprDecl, // 式としても読める型付の宣言
		PragmaTargetFunction, // 関数へのディレクティブを受ける関数の宣言
		PrefixUnaryOperation, // 前置の単項演算式
		PreprocCall, // 前処理の呼出ディレクティブ
		PreprocFunctionDef, // 関数形式の前処理定義
		PreprocInclude, // プリプロセッサのインクルードディレクティブ
		PreprocNode, // 前処理のディレクティブと其の部品
		ProductAmbiguousPtrType, // 乗算と読み違え得るポインタ型
		PropertyNameNode, // 性質の名前のノード
		PtrMemberAccess, // メンバへのポインタを辿る演算子
		QualifiedNameNode, // 修飾を伴う名前のノード
		QuestionLead, // 疑問又は選択の記号で始まるトークン
		QuestionToken, // 疑問符のトークン
		QuoteToken, // 文字列を開閉する引用符のトークン（接頭辞付の物を含む）
		RawTextNode, // 逐語に保つ本文のノード
		ReassigningShortDecl, // 既存の束縛へ代入し得る短い宣言
		ReceiveStmt, // 通信を受け取る文
		ReceiverParenHost, // 受手を包む括弧が後置の結合を保つ式
		RegexLiteralNode, // 正規表現のリテラルノード
		RestrictedLineWord, // 後に改行を挟めない語
		ReturningStmt, // 値を返す文
		RightArrowToken, // 右向きの細い矢印のトークン
		SafeAccessToken, // 安全なメンバ参照のトークン
		ScopeQualifierToken, // 有効範囲を指定する二重コロンのトークン
		ScopedIdentifier, // 有効範囲を指定した識別子
		SelectorList, // セレクタの並び
		SelectorRule, // セレクタを持つ規則
		SelfClosingTag, // 自己終端するタグ
		SelfClosingTagToken, // 要素を自身で閉じるタグのトークン
		SelfReceiverKeyword, // 現在の実体を指す受手の語
		SeparatorLead, // 並び又は文の区切で始まるトークン
		SequentialStmts, // 逐次実行する文を直に並べるノード
		ShortAssignmentToken, // 短い代入のトークン
		ShortVarDecl, // 短い記法の変数宣言
		SlashToken, // 斜線のトークン
		SpacelessTypeName, // トークンの間に空白を置かない型の名前
		SpacingSensitiveInfix, // 前後の空白で前置・後置・中置が変わる演算子の中置の式
		SpacingSensitiveRange, // 前後の空白で読みが変わる範囲の式
		SplitBindingHead, // 束縛の語の後の改行でパーサーが分けた次の文の最初の名前
		SpreadToken, // 値の展開を表す３点のトークン
		SrcCopiedNode, // 子から組み直さず原文をコピーするノード
		SrcFileNode, // 原文ファイルの根ノード
		SrcLeaf, // 中身を組み直さず原文のテキストを其の儘使うノード
		StartTag, // 開始タグ
		StmtOrMemberSeq, // 文・メンバを並べるノード
		StmtTerminatorLead, // 文を終える区切で始まるトークン
		StmtTerminatorTail, // 文を終える区切で終わるトークン
		StmtTerminatorToken, // 文を終えるセミコロンのトークン
		StrLiteralNode, // 文字列リテラルのノード
		StrayBlock, // 見出の文の直後の同じ行の `{` で始まるブロックと読み違えたノード
		StructExpr, // 構造体の値の式
		StructuralParenBoundary, // 見出と本体の構文上の区切
		SubscriptExprNode, // 添字に依る参照の式
		SuperRef, // 親の実体への参照
		TagNameNode, // タグの名前のノード
		TemplateInstantiationType, // 型引数を伴う型の実体化
		TemplateScanBoundary, // 型引数の読違の遡りを止める範囲
		TemplateTypeParam, // 名前を導入する型・テンプレートの仮引数
		TemplateValParam, // 名前を導入する非型のテンプレート仮引数
		TildeToken, // 波線のトークン
		TrailingBreakToken, // トークンの後で折り返す開き又は区切の記号
		TrailingDataSection, // 解釈しない末尾のデータ領域
		TrailingOperatorToken, // 先頭以外で後続を折り返す演算記号
		TransparentWrapper, // アンカーを中身へ移す透過のラッパー
		TypeAliasStmt, // 型の別名を宣言する文
		TypeArgOpeningLead, // 型引数の開始と紛れる被演算子の先頭記号
		TypeDeclarationKeyword, // 型の宣言を始める語
		TypeParamScope, // 型引数を導入する宣言のラッパー
		UnaryPrefixLead, // 前置単項の記号で始まるトークン
		UnfieldedOperator, // 中央の演算子に文法のフィールドを持たない３部の構文
		UnicodeEscapeHost, // トークン化前の Unicode のエスケープを中身に持つトークン
		UnsupportedComment, // 記法の登録に無いコメント
		ValDiscardingAssignment, // 値を使わない文の形の代入
		ValTakingJump, // 空白に依らず後続を値と読む脱出の語
		ValueOutputTag, // 式の値を出力するコードの開始タグ
		VarDeclNode, // 変数の宣言ノード
		VarSpec, // 変数の宣言項目
		VarSpecList, // 変数の宣言項目の並び
		VariadicParamDecl, // 可変個の仮引数の宣言
		VerbatimQuotedStr, // テキストを保つ引用文字列
		VerbatimSubtree, // 内部の構造を保ち原文のテキストを返すノード
		WhenCond, // 条件選択の条件
		WhereClauseNode, // 制約を表す節
		WildcardToken, // 照合で全ての値に一致する読捨のトークン
		// トークンから投影する型・修飾・演算の事実
		ExplicitVirtualModifier, // 上書又は継承制限を明示する仮想指定
		ImmutableTypeQualifier, // 読取専用の型修飾
		InstanceStateInvalidModifier, // 非静的フィールドでは許されない指定
		MemberClassKeyword, // ネストしたクラスの宣言を始める語
		MemberEnumKeyword, // ネストした列挙の宣言を始める語
		MemberStructKeyword, // ネストした構造体の宣言を始める語
		ModifierConstantEvaluation, // コンパイル時評価を要求する明示指定
		ModifierConstantExpression, // 定数式に利用出来る明示指定
		ModifierConstantInitialization, // 静的初期化を要求する明示指定
		ModifierInline, // インラインの明示指定
		ModifierMutable, // 定数の実体内でも変更出来る明示指定
		ModifierStatic, // 型に属する状態・操作の明示指定
		ModifierThreadLocal, // スレッド毎の状態を持つ明示指定
		ModifierVirtual, // 仮想メンバの明示指定
		NullishNull, // 値の欠如を示すヌリッシュ値
		NullishUndefined, // 未定義を示すヌリッシュ値
		StaticLifetimeModifier, // 静的寿命又はスレッドの寿命を持つ指定
		VolatileTypeQualifier, // 揮発する型修飾
		XorOperator, // 排他的論理和の演算子
		Count // 役割の数（番兵）
	}; // 構文の役割（整形仕様 3.2，4.3，文法のノード名に依らない共通の種別と性質で，投影側が文法の記号毎に付ける）

	using RoleSet = std::bitset<static_cast<size_t>(Role::Count)>; // 役割の集合

	// 投影で確定したアクセス修飾子の集合・長さ・登録形
	struct AccessMeaning {
		uint64_t Mask = 0; // 文法内で共有する修飾の同一性（未知の語は全ビットを立てる）
		size_t Length = 0; // 語の長さと語間の区切の合計（入力の空白を含めず，重複する語も数える）
		int ExactRank = -1; // 語の順も登録形に一致する順位（未確認なら負の１）
	};

	enum class NumericLiteralKind : uint8_t { Other, FloatingPoint, ImaginaryFloatingPoint, ImaginaryInteger, Integer }; // 数値トークンの分類 // 未登録の接尾辞・未確定の値（既定値） // 浮動小数 // 浮動小数の係数の虚数 // 整数の係数の虚数 // 整数

	enum class ChildRole : uint8_t {
		None,
		Alias,
		Alternative,
		ArgField,
		Args,
		BlockField,
		Body,
		Communication,
		Cond,
		Consequence,
		Declarator,
		DirectiveField,
		EvaluationSubject,
		Exceptions,
		Function,
		FunctionField,
		Guard,
		IfTrue,
		Increment,
		Initializer,
		Left,
		Length,
		Macro,
		MethodField,
		Name,
		Object,
		ObjectField,
		Operand,
		Operator,
		OperatorField,
		Other,
		Params,
		ParamsField,
		Path,
		Pattern,
		Placement,
		Qualifier,
		ReceiverField,
		Result,
		ReturnType,
		Right,
		ScopeField, // 文法の修飾名のスコープの場
		SharedQualifier,
		SizeField, // 文法の配列の寸法の場
		Superclass,
		Target,
		Type,
		Update,
		Val,
		Var,
		Count
	}; // 親の中で子が担う役割（投影時に文法の場を此の役割へ対応付ける）

	static_assert(static_cast<uint8_t>(ChildRole::Count) < 65); // 子の役割は１語の集合に収める

	enum class FormOperation : uint8_t {
		ArglessReturn, // 値の無い返戻を組み立てる操作
		BraceBody, // 制御本体を波括弧のブロックで包む操作（部品はブロック・`{`･`}`・ブロックの中の文の並びの順で，文の並びを持たないブロックは３個）
		Continuation, // 改行を跨ぐ式を合法にする括弧を展開時だけ補う操作
		ExplicitTailReturn, // 末尾値を返戻の語と実引数の並びで包む操作
		GeneratedLexemes, // ビルダーと正規化処理が生成する固定トークンを定める操作
		LeftAssociateBinary, // 同値な二項連鎖を項の順序を保って左結合へ組み直す操作
		MemberSeparator, // 平坦なメンバ列の境界へ文区切を置く操作
		OmitEmptyArgs, // 空の実引数を除き，文法上の結果種別へ組み直す操作
		PostfixCond, // 単一文と条件の結付を保って後置形式へ組み直す操作
		ReturnWithZero, // 零を返す返戻を組み立てる操作
		TrailingComma, // 展開した並びの閉じの直前へ区切を置く操作
		UnconditionalLoop, // 真の条件を持つ反復を文法の節と場へ組み立てる操作
		UnwrapSeq // 文脈上許される要素列のラッパーを外す操作
	}; // 生成形式の共通操作（整形仕様 3.2）

	struct KindDef;
	struct CanonicalForm;

	// 文法の定義が所有する変更不能な生成形式の並び（ノード毎に持つ為，記号毎の定義の並びへの参照だけを持つ）
	class CanonicalFormList {
	private:

		const std::vector<CanonicalForm> *Defs = nullptr; // 記号毎の生成形式の定義（無ければ空）

	public:

		std::span<const CanonicalForm> GetView() const; // 生成形式の並びの取得関数
		std::span<const CanonicalForm>::iterator begin() const; // 生成形式の並びの先頭の取得関数
		std::span<const CanonicalForm>::iterator end() const; // 生成形式の並びの終端の取得関数

		explicit CanonicalFormList(const std::vector<CanonicalForm> &Forms); // コンストラクタ（記号毎の定義を参照）

		CanonicalFormList() = default; // コンストラクタ（空）
	};

	enum class FormPartRole : uint8_t {
		Args,
		Block,
		BlockClose,
		BlockOpen,
		Body,
		CallClose,
		CallOpen,
		Close,
		Cond,
		IntVal,
		Keyword,
		LastElement,
		Left,
		Open,
		Result,
		Right,
		Separator,
		Stmts,
		Target,
		Terminator,
		TrueVal,
		Val,
		ValDelimiter
	}; // 生成形式の部品が書換後の構文で担う役割

	enum class Word : uint8_t {
		AbsoluteNamePrefix, // ローカルの探索を行わない絶対修飾の接頭辞
		AlternateBranch, // 条件分岐の代替枝を始める語
		AsyncModifier, // 非同期の関数を示す語
		AsyncVoidType, // 値を返さない非同期の関数の戻値の型
		AwaitName, // 待機の演算子として読む名前
		AwaitNameEscape, // 待機の語を識別子として読ませるエスケープ
		BareCall, // 空の実引数の括弧を省ける呼出の名前
		BareCapture, // 引数の末尾で受手を捕捉する記号
		BaseRef, // 基底の型のメンバを指す語
		BlockEnd, // 語で始めたブロックを閉じる語
		Boolean, // 真偽値の組込の型
		Case, // 切替の枝を始める語
		Cast, // 型の変換の語
		CatchAllException, // 全ての送出を捕捉するトークン
		CatchBinding, // 捕捉した値を束縛する語
		CatchFilter, // 捕捉を条件で絞る語
		CatchHead, // 捕捉の節を始める語
		CatchQualifier, // 捕捉の見出で型の前に置く修飾の語
		Channel, // 通信路の型を始める語
		ClassHead, // クラスを宣言する語
		CondThen, // 条件と本体を分ける語
		ConstQualifier, // 不変の型修飾の語
		ConstantCond, // 定数式の条件を示す語
		ConstantDecl, // 定数の宣言に用いる語
		ConstexprQualifier, // 定数式の修飾の語
		Construct, // 生成の語
		ConstructionMethod, // 実体の生成を指示する語又は操作の名前
		Default, // 既定の枝を始める語
		DeferredInit, // 初期化を後へ延ばす修飾の語
		Define, // 定義を始める語
		Delegation, // ジェネレータの委譲を示す語
		DiscardName, // 束縛を捨てる名前
		DocAttributeName, // ドキュメントを表す属性の名前
		DynamicScope, // 名前の解決を実行時に変え得る呼出の名前
		DynamicTopType, // 動的な最上位の型名
		EagerMacroToken, // 未展開の実引数で評価を選択・遅延しない区切と演算子
		EntryFunction, // 処理系がエントリーポイントとする関数の名前
		EntryResultType, // エントリーポイントの戻値の int 型を構成する指定子
		EnumImport, // 列挙子を所属へ導入する語
		ErrorResultType, // 失敗を戻値として表す型名
		ExportName, // 後の宣言・指定子を公開する語として読む名前
		ExprBody, // 関数の式の本体を導くトークン
		ExternalBinding, // ローカルな導入でない既定の束縛の名前
		Fallthrough, // 次の枝へ進む語
		False, // 偽の値の名前
		FloatingType, // 浮動小数点の型名
		ForcedTry, // 送出を異常終了へ変える試行の語
		FunctionHead, // 仮引数又は受手の括弧を直後に置く関数の語
		FunctionScopedBinding, // 関数の有効範囲の束縛を宣言する語
		Getter, // 値を読む操作の名前
		ImmutableBinding, // 不変の束縛を宣言する語
		ImplicitRaise, // 受手の無い送出の呼出の名前
		ImportedTarget, // 明示のインポートによる解決を要する標準対象名
		In, // 所属又は反復の対象を示す語
		InferredType, // 型を推論する型名
		InlineNamespace, // 名前空間のメンバを外側へも導入する修飾の語
		InlineStyleAttribute, // 要素の表示規則を指定する属性の名前
		IntTruth, // 整数で表す真の値
		IntZero, // 整数で表す偽の値
		IterationDo, // 反復の条件と本体を分ける語
		IterationOf, // 反復の対象の値を示す語
		LabeledReceiver, // ラベル付の受手の参照を始めるトークン
		LabeledReturn, // ラベルで返戻先を示す語
		LineContinuation, // 括弧の外で改行を越えて文を続ける記号（パーサーがトークンにせず空白と読む）
		ListSeparator, // 値の並びを区切る記号
		LogicalWord, // 語で綴る論理演算子
		LongIntType, // 幅が文法で変わる長整数型の名前
		Loop, // 無条件の反復の語
		MacroConcatenation, // マクロの置換トークンを結合する演算子
		MacroStringizing, // マクロの実引数を文字列化する演算子
		MetadataElement, // ドキュメントの付帯情報を収める要素の名前
		MetatypeSuffix, // 型自身を表す後置の名前
		NamespaceAttribute, // 名前空間を指定する属性の名前
		Negation, // 語で綴る否定の演算子
		NeverType, // 戻らない事を示す戻値の型
		NoReturnAttributeName, // 非復帰の属性の名前
		NoReturnAttributeNamespace, // 非復帰の属性を定める名前空間
		NoReturnSpecifier, // 戻らない事を示す宣言の指定子
		NonMacroName, // マクロでないと判定する標準の識別子名
		NullLiteral, // 再束縛出来ない，値の欠如を表すリテラル
		NullMacro, // 空ポインターを表す旧来のマクロの名前
		NullishVal, // 値の無い状態を示す名前
		ObjectHead, // 無名の物を作る宣言の語
		OptionalTry, // 送出を欠如値へ変える試行の語
		OptionalityMarker, // 値の欠如を許す型へ付ける記号
		OrdinaryAttribute, // 返戻補完を変えない標準の属性
		OrdinaryMacro, // 返戻補完に必要な効果を登録した通常の標準マクロ
		OutputArg, // 出力先を受け取る実引数の指定子
		PanicMacro, // 呼出側のエラーで中断するマクロの名前
		PreprocAlternateBranch, // 前処理の条件に対する既定の枝のディレクティブ
		PreprocDefined, // 前処理でマクロの定義の有無を問う演算子
		PreprocDefinedBranch, // 定義されたマクロを条件とするディレクティブ
		PreprocUndefinedBranch, // 未定義のマクロを条件とするディレクティブ
		PreserveSpaceAttribute, // 内部空白を保持する属性の名前
		PrivateAccess, // 非公開のメンバを指定する語
		ProcessExitCall, // 捕捉・後始末を実行せずプロセスを終了する標準呼出
		ProtectedAccess, // 継承先へ公開するメンバを指定する語
		PublicAccess, // 公開するメンバを指定する語
		QualifiedResultType, // 標準の供給元を含む無値又は戻らない型の限定名
		ReassignableBinding, // 再代入出来る束縛を宣言する語
		RefBinding, // 参照で束縛する語
		RegexDelimiter, // 正規表現を開く区切記号
		Return, // 返戻の語
		ScopedEnumModifier, // 列挙子を列挙の内部へ限定する修飾の語
		Self, // 自身を指す語
		SignedByteType, // 符号の有無が文法で変わる８ビット整数型の名前
		StandardTerminalDecl, // 同じ関数の再宣言で登録を継ぐ標準名
		StandardTerminalNoArgs, // 仮引数を持たない標準の終端関数
		StmtDeclHead, // 式の文の先頭で宣言として読まれる語
		StmtEnd, // 文を明示して終える区切記号
		StringizingAssertion, // 引数をテキストの儘保つ標準のアサーションマクロの名前
		SystemExitCall, // 標準の終了例外を送出する呼出
		SystemExitExcludedType, // 終了例外を捕捉しない標準の例外型
		SystemExitHandlerType, // 終了例外を捕捉する標準の例外型
		TerminatingCall, // 戻らない事を言語・標準ライブラリが定める呼出の名前（修飾を含む綴り）
		TerminatingImport, // 既知の終端呼出を供給する明示的なインポート先
		TerminatingMacro, // 戻らない事を言語・標準ライブラリが定めるマクロの名前（修飾を含む綴り）
		ThrowHead, // 送出の式を始める語
		ThrowsAnnotation, // 送出を宣言するアノテーションの名前
		ThrowsMarker, // シグネチャに送出を宣言する語
		True, // 真の値の名前
		TypeDecl, // 型の宣言を始める語
		TypeQuery, // 値の型を問う演算子の語
		TypeTest, // 型を検査する演算子の語
		UsingNamespace, // 名前空間を取り込む宣言を示す予約語
		ValOutputTag, // 式の値を出力する埋込のコードの開始タグ
		VariadicMarker, // 可変長の仮引数を示す記号
		VoidType, // 値を返さない事を示す型
		While, // 条件判定の反復の語
		YieldName, // ジェネレータの値を送り出す語
		YieldNameEscape, // 送出の語を識別子として読ませるエスケープ
		Count
	};

	using WordSet = std::bitset<static_cast<size_t>(Word::Count)>; // 投影又は生成形式で確定した語の意味

	// 生成形式の１部品の固定の定義
	struct FormPart {
		FormPartRole Role = FormPartRole::Body; // 生成後の子の役割
		const KindDef *Def = nullptr; // 生成する種別（既存の子は無い）
		std::string_view FixedLexeme {}; // 子を持たない生成トークンのテキスト
		FormPartRole Parent = FormPartRole::Result; // 生成後の直属の親の役割
		ChildRole Field = ChildRole::None; // 生成後の親に於ける場
		bool IsNamed = false; // 生成するノードが名前付か
		uint32_t BlankKey = 0; // 採用する生成形式の空行比較の値
		CanonicalFormList Forms {}; // 生成するノードが持つ変更不能な生成形式
		WordSet Meanings {}; // 生成形式が直接与える語の意味
		const KindDef *TokenDef = nullptr; // 生成形式が直接与えるトークンの役割
	};

	enum class FormCond : uint8_t { BinaryChainWithOptionalLeft, BrokenListWithLastElement, EmptyArgs, UnbracedBody }; // 生成形式を選べる構文上の状態（左項省略形の二項連鎖は右項だけと同値）

	// 生成形式（整形仕様 3.2，共通操作と部品の役割・種別・固定トークンの定義で，原文・言語別の整形関数を持たない）
	struct CanonicalForm {
		FormOperation Operation = FormOperation::BraceBody; // 共通操作
		std::vector<FormPart> Parts {}; // 生成又は移す子と其の構文上の役割
		std::string_view FixedLexeme {}; // 共通操作が生成する固定トークン（該当する形式だけ）
		RoleSet ApplicableRoles {}; // 形式を使える所有者の構文上の役割
		FormCond Cond = FormCond::UnbracedBody; // 形式を選べる構文上の状態
		std::string_view ClosingLexemes {}; // 展開した並びを閉じるトークン（該当する形式だけ）
		const FormPart *GetPartOf(const FormPartRole Role) const; // 役割に対応する部品の取得関数
		bool CanApplyTo(const RoleSet &Roles) const; // 所有者の役割に適用出来るかの判定関数
		bool CanWrapBody(const bool IsBraced) const; // 本体をラッパー出来るかの判定関数
		bool CanAddTrailingComma(const bool IsBroken, const bool HasLastElement, const unsigned char Closer) const; // 末尾のカンマを生成出来るかの判定関数
		bool CanOmitEmptyArgs(const bool IsEmpty) const; // 空の実引数を省けるかの判定関数
	};

	// 使用可能な記法（整形仕様 3.2，入力の記法の識別と出力するコメント行の列のトークンの規則に用いる）
	struct AvailableNotation {
		CommentNotation Notation; // 記法
		bool IsTabSeparated; // 名前迄を開始記号に含むディレクティブ記法の名前の後の区切にタブも読むか
	};

	// 同義のドキュメントタグの対応（整形仕様 7.4，入力のタグを採用するタグへ揃える）
	struct DocTagSynonym {
		std::string_view From; // 入力のタグ
		std::string_view To; // 揃えるタグ
	};

	enum class OmittedResultNotation : uint8_t { Inferred, Int, Unit }; // 型を省いた関数の戻値（本体から推論する，整数を返す，単位型を返す）
	enum class InferredTypeNotation : uint8_t { None, DynamicTop, Implicit, Named, Placeholder }; // 静的解析が照合する推論型又は動的な最上位型の記法
	enum class DocStyle : uint8_t { AtTag, Callout, Docstring, MarkdownHeading, Prose, XmlTag }; // ドキュメントの項目の記法

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
		XmlDoc,
		Yard
	}; // 本文の解釈を決めるドキュメントの形式（整形仕様 3.2，囲記号だけで識別しない）

	enum class CommentUse : uint8_t { Plain, Doc }; // 出力記法の用途（整形仕様 7.6） // 通常の独立・行末コメント // ドキュメント

	// ドキュメントの入力記法と説明する側
	struct DocumentNotation {
		CommentNotation Notation; // 入力記法
		DocumentFormId Form; // 本文の形式
		bool IsInner = false; // 囲む本体を説明するか
	};

	// 用途毎の採用記法（整形仕様 3.2，出力ドキュメント形式は採用記法の入力のドキュメント形式と同じ）
	struct CommentOutputForm {
		CommentUse Use; // 用途
		CommentNotation Notation; // 採用記法
		bool IsInner = false; // 囲む本体のドキュメントか
	};

	// 共通工程が照合・生成する語の意味（整形仕様 3.1，3.5，綴りは GrammarProfile が言語毎に定め，最初の綴りを生成に用いる）
	// 行コメントの登録形の判定条件（整形仕様 4.5 の登録定義から記法・本文・位置の判定に要る条件だけを投影し，採用記法の成立条件として共有する変更不能な値，整形仕様 3.2）
	// 位置は本文の中の位置（行頭の組と囲みのマーカーの中の組）を持ち，ファイルの中の位置（何行目か，最初のトークンの前か）は，検証が新たな登録形を位置に依らず拒む（整形仕様 8.1）為に持たない
	struct RegisteredLineForms {
		std::string_view EncodingDeclWord = {}; // 注記の中から探すエンコーディング宣言の登録語（小文字）
		std::string_view EncodingDeclSeparators = {}; // エンコーディング宣言の登録語と名前の区切
		std::string_view EncodingNameChars = {}; // 英数字の他にエンコーディングの名前へ使える字（空は区切の後に空白でない字が続けば読む）
		bool IsEncodingWordCaseless = false; // エンコーディング宣言の登録語の大小文字を問わない
		bool CanAllowEncodingSeparatorSpace = false; // エンコーディング宣言の登録語と区切の間の空白を認める
		std::vector<std::string_view> EncodingMagicCommentNames {}; // エンコーディングを指定する実行時設定の組の名前（小文字）
		std::unordered_map<std::string_view, std::vector<std::string_view>> MagicCommentValues {}; // 実行時設定の名前と有効な値（小文字）
		std::string_view MagicCommentMarker = {}; // 実行時設定の組を並べる囲みのマーカー
		std::string_view MagicCommentSeparators = {}; // 実行時設定の名前と値の区切
		std::string_view MagicCommentDelimiters = {}; // 囲みの中の組の区切
		std::string_view MagicCommentQuotes = {}; // 名前を終える引用符（最初の字は値を囲む引用符）
		std::string_view MagicCommentNameJoiners = {}; // 実行時設定の名前の中で同じと読む連結の字
	};

	// 用途毎の採用記法と其の成立条件（整形仕様 3.2）
	struct CommentOutputFormList {
		std::vector<CommentOutputForm> Forms {}; // 用途毎の採用記法（無い用途は入力の記法の儘）
		RegisteredLineForms Conditions {}; // 採用記法の成立条件（揃えた行コメントが処理系の登録形に為らない事）
	};

	// コメントの中のコード例の書式（整形仕様 7.3，7.4，インデント・囲み・ドキュメントの行内タグ・ディレクティブ・アノテーションの引数・要素で示すコード例）
	// 言語のドキュメント形式が示すコード例の記法を合わせた物を，ドキュメント・通常コメントの全ての記法へ一律に当てる（記法の変換やドキュメントへの統合で同じ本文の読みを変えない，整形仕様 3.3）
	struct CodeExampleForms {
		bool ShouldMarkCodeByIndent = false; // 引用・箇条のマーカーの後でインデントした行を段落の直後でもコード例とする（偽は空行の後の４桁以上のインデントだけ，本文は行頭にインデントを持たない）
		bool HasFencedCode = false; // 同じ字の３字以上の連続（``` 又は ~~~）で開閉する囲みのコード例を持つ
		std::vector<std::string_view> InlineTags {}; // 対応する閉波括弧迄の中身をテキストの儘読むドキュメントの行内タグの開始（Javadoc の `{@code` 等）
		std::string_view TagMarks = {}; // ドキュメントのタグ・ディレクティブを始める字（タグの行はコードスパンの段落を区切る）
		std::vector<std::string_view> BlockCommands {}; // 対応する終了ディレクティブ迄をテキストの儘読むディレクティブの名前（Doxygen の `code` 等）
		std::string_view BlockCommandExcludedPrefix = {}; // 開始記号を末尾に含む別の行内記法の接頭辞（其の内部をブロックの開始・終端コマンドへ読み替えない）
		std::string_view EndCommandPrefix = {}; // 終了ディレクティブの名前の前置
		unsigned char AnnotationMark = '\0'; // 引数の括弧の中をテキストの儘読むアノテーションを始める字
		std::vector<std::string_view> CodeElements {}; // 内容をテキストの儘読むドキュメントの要素の名前（HTML ･ XML の `<code>` 等）
		std::vector<std::pair<std::string_view, std::string_view>> MarkupSections {}; // 要素の対応付で読み飛ばす領域の開始と終端（CDATA・コメント）
		std::vector<std::pair<std::string_view, std::string_view>> MathDelimiters {}; // 数式の内容を保持する開閉の記法（Doxygen の `\f$` 等）
	};

	// 説明の先頭をタグ又は軽量記法として読まない為のエスケープ
	struct DocHeadEscape {
		char From; // 説明の先頭の字
		std::string_view To; // 同じ字を説明として表す綴り
		bool ShouldRepeat = false; // 同じ字の連続も全て置換するか
	};

	// コメントの記法の定義（整形仕様 3.2，使用可能な記法，入力のドキュメント形式，用途毎の採用記法と其の成立条件，ドキュメントの語彙を持つ変更不能な値）
	struct NotationRules {
		std::vector<AvailableNotation> AvailableNotations {}; // 使用可能な記法
		bool IsBlockCommentNested = false; // ブロックコメントの中の `/*` がネストの囲みを開くか
		std::string_view LineCommentTerminator = {}; // 行コメントの本文を終えるテキスト
		std::vector<std::string_view> LineCommentBreaks {}; // 行コメントの本文を行の途中で終える LF 以外の改行のテキスト
		std::vector<std::string_view> LineSplices {}; // 行末に置くと次の行を行コメントへ繋ぐテキスト
		bool ShouldDecodeUnicodeEscapes = false; // トークン化前に Unicode のエスケープを展開する（エスケープで書いた改行も行を終える）
		bool CanBlockCommentInterruptStmtHead = false; // 行頭のブロックコメントが後続文の読取を妨げる
		bool HasBracketedDocType = false; // ドキュメントの型を角括弧で囲む
		bool HasBareDocType = false; // ドキュメントの型を裸の語で前置する
		std::string_view DocParamSigil = {}; // ドキュメントの仮引数名を始めるマーカー
		std::vector<std::string_view> DocParamMarks {}; // ドキュメントの仮引数名の前に置く参照渡・可変長のマーカー（文法が並べる順）
		bool ShouldExcludeDirectiveLines = false; // ドキュメントの本文からディレクティブの形の行（記号の直後の名前と `:`）を除く
		CodeExampleForms CodeExamples {}; // コメントの中のコード例の書式
		bool HasOptionalFailureType = false; // 失敗条件のタグの型名を省ける
		std::vector<DocumentNotation> DocumentForms {}; // ドキュメントに使う記法と其の入力のドキュメント形式
		std::string_view MemberDocHead = {}; // 前のメンバのドキュメントを表す本文の頭（Doxygen の `///<` 等）
		CommentOutputFormList OutputForms {}; // 用途毎の採用記法と其の成立条件
		std::string_view DocAttributeOpen = {}; // ドキュメントを属性で表す時の開始記号
		std::vector<DocTagSynonym> DocTagSynonyms {}; // 同義のドキュメントタグ
		DocStyle Doc = DocStyle::AtTag; // ドキュメントの項目の記法
		std::span<const DocVocabularyEntry> Vocabulary {}; // 採用する形式の説明項目・読取語・生成記法
		std::span<const DocVocabularyEntry> TagVocabulary {}; // タグの読取と不足項目の生成に使う既定の語彙
		std::vector<std::pair<std::string_view, std::string_view>> DocReferenceAttributes {}; // 説明を持つ参照として読む要素と属性の対応
		std::string_view DocTagTerminators = {}; // タグ名を終える字（空なら英数字・下線以外）
		std::string_view DocParamNameTerminators = {}; // 空白以外で仮引数名を終える字
		std::string_view SummaryTag = {}; // 概要の正規タグ
		std::string_view AlternateSummaryTag = {}; // 統一せず読む概要の別のタグ
		std::string_view TypeParamTag = {}; // 型の仮引数の正規タグ
		std::vector<std::pair<char, std::string_view>> DocTextEscapes {}; // 説明全体へ適用する文字参照
		std::vector<DocHeadEscape> DocHeadEscapes {}; // 保持範囲外の説明の先頭へ適用するエスケープ
		std::string_view FailureTag = {}; // 失敗条件の既定のタグ
		bool ShouldReviewMarkdownLine = false; // 行形式のドキュメントの意味を確認して囲みへ移す
		bool HasMarkupComments = false; // 山括弧のコメント記号を使う
		std::string_view MarkupCommentOpen; // 山括弧のコメントの開始記号（構文規則の定義を参照）
		std::string_view MarkupCommentClose; // 山括弧のコメントの終了記号（構文規則の定義を参照）
		bool IsAvailable(const CommentNotation Notation) const; // 使用可能な記法かの判定関数
		std::optional<DocumentFormId> GetDocFormOf(const CommentNotation Notation) const; // ドキュメントの記法のドキュメント形式の取得関数
		bool IsInnerDoc(const CommentNotation Notation) const; // 囲む本体のドキュメント記法かの判定関数
		const CommentOutputForm *GetAdoptedFormOf(const CommentUse Use, const bool IsInner = false) const; // 用途と説明する側の採用記法の取得関数
	};

	enum class ConstantTruth : uint8_t { Unknown, False, NumericFalse, NumericTrue, True }; // 入力で確定したリテラルの真偽 // 未確定 // 偽 // 数値の偽 // 数値の真 // 真

	// 文法の構文・意味の固定の定義（整形仕様 3.2 の構文事実が共有する変更不能な値，言語名を持たず，投影側が文法毎に１度定める）
	struct SyntaxRules {
		// 正規表現の名前付の捕捉の記法
		struct RegexCapture {
			std::string_view Open; // 名前の前の開き
			std::string_view Close; // 名前の後の閉じ
		};

		// 中核の工程と静的解析が照合する演算子の綴り
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
			std::string_view MemberAccess; // メンバの参照
		};

		// 正規化・制御の流れ・シグネチャ・コメントの固定の動作条件
		std::vector<std::string_view> CodeOpeningSuffixes {}; // 同じ行でコードを開く境界の末尾
		NotationRules Notation {}; // コメントの記法の定義（コメントの経路へは此れだけを渡す）
		std::string_view DocForm = {}; // 静的解析で示す採用済のドキュメント記法
		bool HasLabelJump = false; // 名前への任意の制御移動を持つ
		bool IsLabelJumpScoped = false; // ラベルのジャンプ先を最内の有効範囲で選ぶ
		bool HasNumericExitDepth = false; // 脱出先を正の段数で指定する
		bool DoesContinueCountSwitch = false; // 継続の段数に切替も数える
		bool IsExitLabelBodyChild = false; // 脱出先のラベルを本体の子に持つ
		bool IsExitLabelPrevSibling = false; // 脱出先のラベルを先行する兄弟に持つ
		bool CanCaseNestInStmt = false; // 切替の枝を文の内側にも置ける
		bool DoesCaseOpenWithStmtEnd = false; // 枝の区切を文の終わりの記号でも開く
		bool CanCaseFallThrough = false; // 終端しない枝が次の枝へ進む
		bool CanColonCaseFallThrough = false; // コロンで開く枝が次の枝へ進む
		bool CanEmptyCaseFallThrough = false; // 本体の無い枝だけが次の枝へ進む
		OmittedResultNotation OmittedResult = OmittedResultNotation::Inferred; // 型を省いた関数の戻値
		bool DoesDeferredCallReturnVal = false; // 非同期・ジェネレータの呼出が，本体の返戻とは別の値を返す
		bool IsTailExprVal = false; // ブロックの末尾の式がブロックの値を与える
		bool HasStaticCatchReachability = false; // 有効な捕捉節の到達可能性を実行時の送出と独立に合成する
		bool DoesReachabilityUseConstantCond = false; // 定数式の値に依って到達可能性が変わる
		std::string_view BasePrefixLetters = {}; // 小文字に保つ基数の接頭辞の字
		unsigned char DigitSeparator = '\0'; // 数値の桁を区切る字
		std::array<std::string_view, 3> ExitDepthBasePrefixes {}; // 脱出段数の１６進・２進・８進の接頭辞の字
		std::string_view ImaginaryLiteralSuffixes {}; // 虚数リテラルの単文字の接尾辞
		std::vector<std::string_view> IntegerLiteralSuffixes {}; // 整数リテラルの型接尾辞
		std::string_view FloatingLiteralSuffixes {}; // 浮動小数リテラルの単文字の型接尾辞
		std::vector<std::string_view> NumericTypeSuffixes {}; // 符号と幅を含む数値の型接尾辞（長い物から照合する）
		std::string_view QuotePrefixes = {}; // 再表現出来る引用文字列の接頭辞
		std::string_view RawQuotePrefixes = {}; // エスケープを解かない引用文字列の接頭辞
		std::string_view FormatQuotePrefixes = {}; // 補間を持つ引用文字列の接頭辞
		std::string_view NonDocQuotePrefixes = {}; // ドキュメンテーション文字列に為らない引用文字列の接頭辞
		bool IsSingleQuoteBackslashLiteral = false; // 単引用符内の通常のバックスラッシュを字として読む
		bool DoesInterpolateDollar = false; // 二重引用符内でドル記号から補間する
		std::string_view HashInterpolationHeads = {}; // 二重引用符内で井桁記号の直後に置いて補間を始める字
		std::vector<RegexCapture> RegexNamedCaptures {}; // 照合で同名のローカル変数を束縛する正規表現の名前付の捕捉の記法
		bool HasTripleQuote = false; // 三重引用符を持つ
		bool IsEveryIntCondTrue = false; // 整数条件の値に依らず常真である文法か
		bool DoesBoolNameNeedDecl = false; // 真偽値の名前を宣言後にだけ使える
		bool ShouldPadDecPoint = false; // 小数点の前後の零を補う
		bool HasSpacedDimensionSigns = false; // 単位を後置出来る数値の符号を空白で離しても同じ単項式として読む
		bool HasTrailingCommaEmptyVal = false; // 末尾のカンマが空の値を表す
		bool DoesSingletonCommaDetermineTuple = false; // 単一要素の末尾カンマが組の型を決める
		bool CanLineContinueOnlyInBrackets = false; // 括弧の外の改行が文を終え，括弧の内側だけで行が続く
		bool ShouldProtectBrokenDeclBoundaries = false; // 誤った宣言のトークン境界を保護するか
		bool HasAngleListSyntax = false; // 山括弧を型の並びとして読む文法か
		bool CanCallFormsDeclare = false; // 名前が型かを構文から判別出来ず，型の名前の呼出の形が宣言子とも読める
		FormPart FalseDef {}; // 偽の値の生成定義
		FormPart ExprStmtDef {}; // 式の文の生成定義
		FormPart ReturnExprDef {}; // 返戻の式の生成定義
		FormPart ParenthesizedStmtsDef {}; // 括弧で囲む文の並びの生成定義
		FormPart TrueDef {}; // 真の値の生成定義
		FormPart LoopExprDef {}; // 無条件の反復式の生成定義
		FormPart ImplicitParamDef {}; // 暗黙の仮引数の生成定義
		FormPart DoubleQuotedDef {}; // 二重引用符への変更で種別も変わる文字列の生成定義
		FormPart PrefixUnaryDef {}; // 前置の単項式の生成定義
		FormPart PrefixDef {}; // 前置の式の生成定義
		std::unordered_map<Role, std::vector<std::string_view>> LexemeRoleSpellings {}; // 固定の綴りだけで確定するトークンの役割
		std::unordered_map<std::string_view, FormPart> TokenDefs {}; // 生成するトークンのテキスト毎の種別と構文事実の定義
		std::array<const KindDef *, 256> SingleByteTokens {}; // 同じ固定定義を１バイトの綴りで直接引くインデックス
		std::array<std::vector<std::string_view>, static_cast<size_t>(Word::Count)> Words {}; // 語の意味毎の綴り（最初の綴りを生成に用いる）
		std::array<size_t, static_cast<size_t>(Word::Count)> WordMaxLengths {}; // 語の意味毎の最長の綴り
		std::bitset<static_cast<size_t>(Word::Count)> CaseInsensitiveWords {}; // 大文字小文字を区別せず照合する語の意味
		std::vector<CanonicalForm> GeneratedForms {}; // 文法に共通の生成形式
		// 中間表現の構築と配置の固定の動作条件
		bool CanContinueWithBrace = false; // 次の行の波括弧が前の式へ続く記法
		bool CanContinueWithCall = false; // 次の行の呼出・添字・参照が前の式へ続く記法
		bool CanOmitTrailingSemicolon = false; // 末尾のセミコロンを省く記法
		bool IsWhitespaceSemantic = false; // 空白の追加がセレクタ・値の意味を変える記法
		std::string_view LexicalSeparator = {}; // 空白を入れずにトークンを分ける固定の記法
		bool HasDeferredLiteralBodies = false; // 開始行より後に本文を置くトークンを持つ記法
		bool HasLineDirectives = false; // プリプロセッサディレクティブが物理行を占める記法
		bool HasAutomaticLineTerminator = false; // 行末の名前・閉括弧で文区切を補う記法
		bool CanAttachTopLevelSemicolon = false; // 独立した最上位のセミコロンを前の式へ接続する記法
		bool HasHeadMemberArrow = false; // メンバ参照の矢印を次行の先頭へ置く記法
		bool HasNumericUnitAdjacency = false; // 数値と単位の境界で原文の隣接を保つ記法
		bool HasBareControlHeader = false; // 制御語の後に必須の条件括弧を持たない記法
		// 静的解析の固定の動作条件
		bool HasDetailedMemberOrder = false; // メンバの詳細な分類順位を使う
		std::string_view MemberStaticWord = {}; // 静的メンバのメモリ指定
		std::string_view MemberVirtualWord = {}; // 仮想メンバの指定
		InferredTypeNotation InferredType = InferredTypeNotation::None; // 推論型又は動的な最上位型の記法
		std::string_view DefaultIntType; // 接尾辞の無い整数リテラルの型
		uint32_t NestingLimit = 8; // インデントの上限
		bool HasLeadingZeroOctal = false; // ０始まりの８進表記
		bool ShouldOrderMembers = false; // メンバを役割・生成・破棄の順に並べる
		bool HasBoundMemberCalls = false; // 宣言呼出の解決結果を投影するか
		bool HasBoundMemberModifiers = false; // 修飾の解決結果を静的解析の入口で投影するか
		std::unordered_map<std::string_view, std::string_view> ImplicitBindingTargets {}; // 暗黙に導入される名前から標準の対象への対応
		std::vector<std::string_view> MemberTypeModifiers; // 型に属するメンバを示す修飾語
		std::vector<std::string_view> MemberConstructorNames; // 言語が定める生成のメンバ名
		std::vector<std::string_view> MemberDestructorNames; // 言語が定める破棄のメンバ名
		bool HasAssociatedFunctions = false; // 受手の無いメンバ関数が型に属するか
		std::vector<std::string_view> AccessOrder {}; // 公開範囲の全順位
		std::vector<AccessMeaning> AccessMeanings {}; // 登録順の可視性の意味と長さ
		AccessMeaning ImplicitAccessMeaning {}; // 修飾を省いたメンバの可視性
		AccessMeaning PublicAccessMeaning {}; // 公開するメンバの可視性
		AccessMeaning PrivateAccessMeaning {}; // 明示する語を持たない非公開メンバの可視性
		std::string_view ImplicitAccess = {}; // 修飾子を省いたメンバの公開範囲
		bool ShouldRequireExplicitPrivate = false; // 既定の私有領域を明記する
		bool ShouldRequireExplicitInternal = false; // 既定の内部公開を明記する
		bool HasAccessSections = false; // 独立のアクセスセクションを持つ
		bool HasModifierGroups = false; // 修飾語を１つの並びへ纏める
		bool HasReturnLabels = false; // ラベルが関数値の返戻先を兼ねる
		bool HasReturnComments = true; // 返戻の前に説明を付ける
		bool HasStrictEquality = false; // 型変換を伴わない等価演算子を持つ
		bool HasBoundaryCmp = false; // 比較境界の調整の候補を検査する
		bool HasZeroCmp = false; // 数値の０比較の候補を検査する
		bool HasPositiveZeroCmp = false; // 正値と零との関係比較を候補に含める
		bool HasZeroInequality = false; // 零との不等値比較を候補に含める
		bool ShouldPreferBoolXorAssignment = false; // 組込真偽値の反転に排他的論理和代入を用いる
		bool ShouldPreferUnitUpdate = false; // 数値の１加減算に増減演算を用いる
		bool ShouldPreferPrefixUpdate = false; // 結果未使用の増減演算に前置を用いる
		OperatorNotation Operators {}; // 中核の工程と静的解析で照合する演算記号
		std::string_view MarkupCommentOpen = {}; // ドキュメントのコメントを開く記号
		std::string_view MarkupCommentClose = {}; // ドキュメントのコメントを閉じる記号
		/**
		 * ========== 括弧・波括弧・宣言の正規化 ==========
		 */
		std::unordered_set<std::string_view> AssignmentOperators {}; // 代入演算子のテキスト
		std::unordered_map<std::string_view, int> OperatorPrec = {}; // 演算子のテキスト毎の優先順位（０は不明）
		std::unordered_set<std::string_view> CmpOperators = {}; // 比較演算子のテキストの集合
		std::unordered_set<std::string_view> FusedPrefixPairs = {}; // 前置演算子と値の先頭が融合する２文字の組
		std::unordered_set<std::string_view> FusedRangePairs = {}; // 範囲の区切同士が融合する２文字の組
		std::string_view OperatorFusionChars = {}; // 隣接すると演算子のトークンへ融合する字
		std::string_view AssignmentFusionBefore = {}; // 代入の左側で演算子へ融合する字
		std::string_view AssignmentFusionAfter = {}; // 代入の右側で演算子へ融合する字
		std::string_view AttachedOperatorPrefixChars = {}; // 密着時に後置演算子へ読める先頭の字
		std::string_view OpenRangeClosers = ")]},;"; // 終端の無い範囲を閉じる区切
		std::unordered_set<int> RequiredValParenPrecs = {}; // 値の位置で括弧を要する優先順位
		std::string_view SignsOverPower = {}; // 累乗を外側から取れる単項の記号
		int PowerPrec = -1; // 累乗の優先順位（無ければ負）
		int AssignmentPrec = 1; // 代入の優先順位
		int ConditionalPrec = 2; // 条件演算の優先順位
		std::unordered_set<std::string_view> TypeContinuationTokens = {}; // 変換先の型へ続く演算子
		std::unordered_set<int> NonAssocPrecs = {}; // 同格の括弧を保持する優先順位
		std::unordered_set<int> RightAssocPrecs = {}; // 右結合の優先順位
		std::vector<std::pair<int, int>> ProtectedPrecPairs = {}; // 読みを保つ為に括弧を残す親と子の優先順位の組
		FormPart PrefixExprDef {}; // 前置の演算の生成定義
		std::string_view StandaloneDataMarker = {}; // 最上位の単独の名前の式をデータ開始へ変えない括弧を要する綴り
		FormPart ExprParenDef {}; // 式の括弧の生成定義
		bool HasBackwardBindingTypeAnnotation = false; // 初期値の無い束縛が後続の型注釈を共有するか
		bool HasReassignableLexicalKeyword = false; // 変更の無いブロックの有効範囲の束縛を不変の宣言へ揃える記法か
		bool ShouldPreserveBindingTypes = false; // 不変指定に依る推論型・絞込・型の観測への影響を確認するか
		bool ShouldNormalizeIdentifierNfkc = false; // 識別子の比較名へだけ NFKC を適用するか
		bool HasFunctionLocalBindings = false; // ローカルの導入が関数全体の名前の束縛を決める
		bool HasBindingIdentity = false; // 共通の束縛の導入・参照・書込を解決するか
		bool HasOrderedBindings = false; // ローカルの名前が宣言の後から見えるか
		bool HasBindingInInitializer = false; // 導入したローカル名が初期値の中から見えるか
		bool HasBareBindingParams = false; // 型・既定値を持たない名前だけの仮引数も導入するか
		bool HasOrderedBindingShadow = false; // 同じブロックの後の宣言が先のローカル名を隠すか
		bool HasMemberBindingReceiver = false; // 型の本体が未確定なメンバ名も供給し得るか
		bool HasLocalBindingPriority = false; // ローカル名を暗黙の受手のメンバより先に解決するか
		std::string_view ImplicitBindingParam {}; // 型の確認を要する暗黙のクロージャ仮引数名
		std::string_view ImplicitFunctionBinding {}; // 通常の関数だけが導入する暗黙の束縛名
		bool AreFunctionNamesCaseInsensitive = false; // 関数の名前の ASCII の大小を区別しないか
		bool HasNamespaceFunctionImports = false; // 名前空間毎に関数・型・定数のインポートを分けるか
		std::string_view ImportPathSeparator = "::"; // インポートグループの供給元を繋ぐ区切
		std::unordered_map<std::string_view, std::string_view> StandardImportPrefixes {}; // 標準の供給元毎に導入する登録名の接頭辞
		std::string_view BindingNameQuote {}; // 識別子の名前を包む固定の区切（投影済の名前の更新用）
		bool CanMutateBase = false; // フィールド・添字・分割代入の変更が基底の束縛に及ぶか
		bool ShouldRemoveMutableMarker = false; // 不変化で可変の指定を除去する記法か
		bool ShouldPreserveConstantStatus = false; // 不変化に依る定数変数化を避ける必要が有るか
		FormPart ModifierListDef {}; // 修飾子の並びの生成定義
		static bool IsFusionLetter(const unsigned char Char); // 語の演算子へ融合する英字かの判定関数
		static bool IsFusionAlnum(const unsigned char Char); // 名前へ融合する英数字かの判定関数
		static bool IsFusionNameStart(const unsigned char Char); // 演算子名へ融合する英字又は下線かの判定関数
		static bool CanFuseDimensionTokens(const unsigned char Last, const std::string_view After); // 隣接するトークンが次元値等へ融合するかの判定関数
		const RoleSet *GetTokenRoles(const std::string_view Text) const; // 登録済トークン全体の役割集合の取得関数
		bool HasTokenRole(const std::string_view Text, const Role Wanted) const; // 登録済トークン全体の綴りの役割の判定関数
		std::string_view GetSpelling(const Word Meaning) const; // 語の意味の生成する綴りの取得関数
		bool CanFuseOperatorChar(const unsigned char Char) const; // 字が演算子の融合に関わるかの判定関数
		bool CanFuseAssignment(const unsigned char Before, const unsigned char After) const; // 代入のトークンと隣接する字が融合するかの判定関数
		bool CanFuseRangePair(const unsigned char Before, const unsigned char After) const; // 範囲の区切同士が融合するかの判定関数
	};

	enum class BindingSyntax : uint8_t {
		None, // 名前を導入しない普通の構造
		Block, // ブロックの名前の有効範囲
		Catch, // 捕捉の仮引数の有効範囲
		ClassBody, // メンバの名前の有効範囲
		ClassDecl, // 外側へ名前を導入する型
		ClassExpr, // 自分の中だけへ名前を導入する型式
		Closure, // クロージャの仮引数と捕捉の境界
		ConstructorParam, // 初期化名又はメンバも導入するコンストラクタの仮引数
		Decl, // １つ以上の束縛を導入する宣言
		Declarator, // 名前と初期化子を持つ宣言子
		DefaultPattern, // 左のパターンと右の既定値
		DynamicScope, // 未確定な名前を供給する範囲
		Function, // 名前を外側へ導入しない関数
		FunctionDecl, // 外側へ名前を導入する関数
		Ignore, // 型等の値の束縛を読まない領域
		Import, // 外から名前を導入する宣言
		ImportSpecifier, // 読込名とローカルの別名の指定
		Initializer, // 型の初期化時だけ評価する領域
		Loop, // 反復の束縛の有効範囲
		Member, // フィールド又は添字を介した参照
		MemberFunction, // 名を外側で評価し，仮引数と本体へ範囲を導入するメンバ関数
		MemberPart, // フィールドの名前を値の束縛として読まない領域
		ModuleDecl, // 型の名前空間へ名を導入する独立した項目の範囲
		Name, // 名前の導入又は利用
		NamePattern, // 名前と型を持つパターン
		NamedExpr, // 自分の中だけへ名前を導入する関数式
		NamedMember, // 計算しないフィールド名を持つ参照又は呼出
		PairPattern, // キーと導入する値のパターン
		Param, // 型又は既定値を持つ仮引数
		Params, // 仮引数の並び
		Pattern, // 分割するパターンの並び
		RestPattern, // 残余又は単一のラッパーのパターン
		TypeName, // 型のトークン（値の導入の場では名前として解決する）
		TypeParam, // 所属する定義の頭部へ型の名前を導入する仮引数
		Update, // 増減を含み得る式
		VarBlock, // 関数の宣言語もブロックの中へ隔離する有効範囲
		Write // 代入する式
	}; // 名前の導入・利用と有効範囲の固定の構造定義（制御フローの境界とは独立）

	enum class BindingNamespace : uint8_t { Val, Type }; // 名前の導入と探索を分ける名前空間 // 値の名前空間 // 型・モジュールの名前空間
	enum class BindingParamSrc : uint8_t { Direct, EnclosingType }; // 仮引数の固定の供給契約 // 自身の仮引数 // 所有する型の仮引数からの暗黙の導入
	enum class BindingConstructorForm : uint8_t { None, Nominal, PositionalFields }; // 型が値のコンストラクタを供給する構造の契約 // コンストラクタを導入しない形 // 本体無又は位置付のフィールドで値のコンストラクタを導入する型 // 位置付のフィールドの本体

	// 名前空間・仮引数の供給・パターン・コンストラクタと定数参照の変更不能な定義
	struct BindingDef {
		BindingNamespace Namespace = BindingNamespace::Val; // 宣言する名前空間
		BindingParamSrc Params = BindingParamSrc::Direct; // 仮引数を供給する固定の契約
		ChildRole ValueInput = ChildRole::Val; // 宣言又は別名の供給値を持つ場
		ChildRole PatternRef = ChildRole::None; // 新しい名前を導入せず既存の値を参照するパターン頭部の場
		BindingConstructorForm Constructor = BindingConstructorForm::None; // 値のコンストラクタを導入する構造の契約
		bool IsStaticItem = false; // ローカル値の捕捉でない項目の導入か
		bool ShouldRequireImmutableConstantSrc = false; // 定数式で参照する為に既存の不変指定と宣言初期値を必要とする導入か

		/**
		 * 固定の束縛契約の一致の判定関数
		 * 計算量：O(1)
		 * @param Other 比較する固定の契約
		 * @return 名前空間・仮引数・パターン・コンストラクタ・捕捉と定数参照の契約が一致すれば true
		 */
		constexpr bool operator==(const BindingDef &Other) const = default;
	};

	enum class EvaluationRule : uint8_t {
		None,
		Branch,
		Choice,
		Container,
		Eager,
		Expr,
		Fallthrough,
		InfiniteLoop,
		KeywordJump,
		Labeled,
		Loop,
		Macro,
		Preproc,
		ShortCircuit,
		SingleBody,
		Stmts,
		Switch,
		Transfer,
		Try
	}; // 評価位置を組み合わせる共通の制御演算

	enum class EvaluationClass : uint8_t { Conditional, Deferred, Invocation, Required, Unevaluated, Unknown }; // 評価時機 // 条件付 // 遅延 // 呼出回数の未確定な本体の照会 // 必須 // 非評価 // 事実不足で補完不可

	struct EvaluationInput {
		ChildRole Field = ChildRole::None; // 選ぶフィールド（無指定なら名前付の位置）
		uint32_t NamedIndex = 0; // フィールドを持たない子の位置
		EvaluationClass Class = EvaluationClass::Required; // 選んだ子の評価時機
		ChildRole WhenPresent = ChildRole::None; // 此のフィールドが在る場合だけ適用する条件
		std::optional<Role> ChildKind = std::nullopt; // フィールドに代る子の役割
		std::string_view WhenToken {}; // 親の直接のトークンを条件にする場合の綴り
		std::string_view AfterToken {}; // 此の直接のトークンの直後の名前付子を選ぶ綴り
		std::optional<Role> UnlessKind = std::nullopt; // 此の役割を持つ子には適用しない条件
		std::optional<Role> WhenSiblingKind = std::nullopt; // 此の役割を持つ直接の兄弟が必要な条件

		constexpr bool operator==(const EvaluationInput &Other) const = default; // 固定の評価位置の一致の判定関数
	}; // 既定の評価時機と異なる子の位置

	struct EvaluationDef {
		EvaluationRule Rule = EvaluationRule::None; // 経路の合成規則
		EvaluationClass Children = EvaluationClass::Conditional; // 子の既定の評価時機
		EvaluationClass Invocation = EvaluationClass::Deferred; // 包含関数の呼出時に読む見出の評価時機
		ChildRole FirstInput = ChildRole::None; // 記述順より先に評価する入力のフィールド
		ChildRole CatchInput = ChildRole::None; // 式自身に捕捉先を持つ場合のフィールド
		ChildRole ConditionField = ChildRole::Cond; // 制御構文の条件又は対象のフィールド
		std::optional<uint32_t> ConditionIndex = std::nullopt; // フィールドの無い条件の名前付の位置
		std::string_view UnevaluatedHead {}; // 呼出の形で表す非評価演算の語
		std::string_view UnknownHead {}; // 構文だけでは評価を確定出来ない型照会の語
		bool HasDeferredArguments = false; // 型契約が無ければ実引数の評価時機を確定出来ないか
		bool DoesEvaluateChildInputs = false; // 直接の呼出自身を遅延し，其の入力だけを評価するか
		bool HasEntry = false; // 本体の選択前に必須の入力を評価するか
		bool IsContainer = false; // 自身の送出を持たない構文のラッパーか
		bool IsOrdered = false; // 必須の入力を記述順に評価するか
		bool MaySkipInputs = false; // 要素数等が未確定で入力を１度も評価しない場合が在るか
		bool IsRequiredFirst = false; // 既定値の列をアノテーションより先に評価するか
		ChildRole ThrowBeforeField = ChildRole::None; // 此の入力の直前に確保等が送出し得る場
		std::vector<std::string_view> ConditionalOperators {}; // 先頭だけを必須評価にする演算子
		std::vector<EvaluationInput> Inputs {}; // フィールド又は名前付の位置毎の評価時機

		bool operator==(const EvaluationDef &Other) const = default; // 評価契約の一致の判定関数
	}; // 記号毎に共有する子の評価契約

	struct TypeBindingDef {
		uint8_t Precedence = 0; // 型演算の結合の強さ（０は未定義）
		uint8_t OperandPrecedence = 0; // 被演算子に必要な最小の結合の強さ
		bool HasOperand = false; // 型の被演算子を持つか
		bool IsIndex = false; // 添字の内側は型全体を受理するか
		bool IsOpenTail = false; // 型の末尾を後続の式から隔てる必要が在るか
		bool PreservesGrouping = false; // 同格でも型の簡約時点を保持するか
		bool RequiresCallableGroup = false; // 関数型を直接の項として置けないか

		bool operator==(const TypeBindingDef &Other) const = default; // 型演算の契約の一致の判定関数
	}; // 括弧の内外で比較する型演算の固定契約

	// 共通のノード種別の定義（整形仕様 3.2，構文上の役割を共有する変更不能な値）
	struct KindDef {
		RoleSet Roles {}; // 役割
		std::string DocIdentity; // 同じ役割・束縛の表示を持つ種類の共通番号（文法の寿命で固定）
		std::vector<Role> RoleList {}; // 種別が持つ役割だけの昇順の列
		bool IsNamed = false; // 構文か（偽はトークン）
		bool HasCallableIntroductionRole = false; // 呼出名の導入を調べる役割を含むか
		TypeBindingDef TypeBinding {}; // 型の優先順位と被演算子の契約
		BindingSyntax Binding = BindingSyntax::None; // 束縛を読む共通の構造
		EvaluationDef Evaluation {}; // 子の評価時機の固定契約
		BindingDef Bindings {}; // 投影で共有する名前の導入・利用の固定契約
		void InitRoleList(); // 固定定義の役割列と呼出名の導入の候補の構築関数
	};
} // namespace Core
