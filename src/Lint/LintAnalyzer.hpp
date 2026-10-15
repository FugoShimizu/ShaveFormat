#pragma once

#include "OutputFacts.hpp"
#include "../Core/CommentRepository.hpp"
#include "../Core/SyntaxModel.hpp"
#include "../Core/ValidatedOutput.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

enum class LintRule : uint8_t {
	NestingDepth, // ネストの深さ
	AccessOrder, // アクセス修飾子の順序
	ExplicitAccess, // 既定のアクセス修飾子の明記
	MemberOrder, // クラスメンバの宣言順
	JumpUsage, // goto・ラベル付脱出
	ImmediateReturnVariable, // 即時返戻のローカル変数
	InferredType, // 型推論の使用
	AnyType, // 型としての `any`
	MissingReturn, // 明示終端の不足
	UnreachableReturn, // 到達不能な末尾の返戻
	DeclarationType, // 宣言の型の不在
	ParameterType, // 仮引数の型の不在
	ReturnType, // 戻値型の不在
	MissingDocumentation, // ドキュメントの不在
	DocumentationSummary, // ドキュメントの概要の不足
	DocumentationParameter, // ドキュメントの仮引数の説明の不足
	DocumentationReturn, // ドキュメントの戻値の説明の不足
	DocumentationNotation, // ドキュメントの記法
	DocumentationItemForm, // ドキュメントの説明項目の形式
	DocumentationName, // ドキュメントの書出の宣言名
	ConstantMacro, // 定数マクロ
	UsingNamespace, // `using namespace`
	MissingReturnComment, // 返戻コメントの不在
	ZeroComparison, // ０比較
	JsxNonzeroCondition, // JSX の非０判定
	BooleanNegation, // 真偽値の反転
	NullishComparison, // `null` / `undefined` の比較の縮約
	MergeIf, // 同一本体の if の統合
	ComparisonBoundary, // 比較境界の調整
	UnitAssignment, // `+= 1` / `-= 1`
	UnusedPostfix, // 後置増減の未使用結果
	CStyleCast, // C 形式のキャスト
	Other // 一覧外の警告と通知
}; // 静的解析の警告の識別子（整形仕様 8.4 の警告一覧の行順）

using LintSortVal = std::variant<std::string, int64_t>; // 表記前の置換値（整形仕様 8.4）
using LintSortVals = std::vector<LintSortVal>; // 本文のフィールドの記載順の置換値

// LintAnalyzer が検出した規約違反１件を表す構造体
struct LintWarning {
	uint32_t Row; // 違反位置の行
	uint32_t Column; // 違反位置の列
	std::string Msg; // 警告文（引数名等を含む動的メッセージにも対応する為 string_view でなく text で所有）
	LintRule Rule = LintRule::Other; // 警告一覧の識別子（同じ位置の警告を一覧の行順に並べる）
	uint64_t Target = UINT64_MAX; // 対象の最終構文の先行順
	LintSortVals Vals = LintSortVals(); // 表示前の文字列・整数の置換値
	bool operator<(const LintWarning &Other) const; // 表示前の警告順序の比較演算子
};

// 検証済の最終投影を走査し，規約違反を返す読取専用の検査クラス
class LintAnalyzer {
private:

	friend struct LintFactsVerification; // 既存の診断検査で共有参照と属性の先行順を照合する型

	using NodeId = Core::NodeId;
	using NamedLabels = std::unordered_map<std::string_view, NodeId>; // 名前毎のラベルのノード
	using LabelTargetIdx = std::unordered_map<NodeId, NodeId>; // ラベルノード毎の対象ループインデックス（空ノードは対象無）

	// コメントの本文の行頭と出力行内の一致位置
	struct CommentLine {
		size_t Begin; // 本文内の行頭
		std::optional<size_t> Match; // 最終テキストの行内位置（未照会なら無，不一致なら npos）
	};

	// ドキュメントの完全性の実際の走査・複写・位置照合の観測
	struct DocumentationWork {
		size_t Bindings = 0; // 説明不足の位置の為に取得した仮引数の束縛数
		size_t Bytes = 0; // 論理行の本文を所有列へコピーしたバイト数
		size_t Lines = 0; // 追加した論理行数
		size_t Points = 0; // 出力位置を照合した論理行数
		size_t RuleCopies = 0; // 入力形式の差異で複写した記法の定義数
	};

	// メンバ分類が読む見出の共有要約
	struct MemberHeader {
		bool IsTypeMember = false; // 型への所属を指定する修飾子が在るか
		bool HasProperty = false; // プロパティを指定する修飾子が在るか
		bool HasAccess = false; // 明示した可視性を持つか
		Core::AccessMeaning AccessMeaning; // 投影済の可視性の意味
		int AccessRank = -1; // 修飾子の集合から確定した可視性の順位
		NodeId Specifier = NoNode; // 最初のアクセス修飾子
	};

	// 数値である根拠と，型又は演算から負の値を取り得る事の要約
	struct NumericFact {
		bool IsNumeric = false; // 数値の型又は演算を確認したか
		bool CanBeNegative = false; // 負の値が型又は代入から可能か
		bool IsUnsigned = false; // 負の値を持たない型か
		bool IsBlocked = false; // 明示型を数値と確定出来ないか
		bool HasExplicitType = false; // 型指定から数値の範囲を確認したか
		std::optional<uint64_t> LiteralMagnitude; // 整数リテラルの絶対値（確定出来ない場合は無）
	};

	static constexpr NodeId NoNode = Core::NoNode;
	// 最終テキストと其の投影・コメント・検証データへの読取専用の参照
	mutable std::unordered_map<NodeId, std::optional<size_t>> StmtLineCounts; // 関数毎の必須コード行数（３行で打切）
	mutable std::unordered_map<NodeId, NodeId> FunctionStmts; // 関数毎の文の並び（検査中の構文は変更不能で，無い結果も再利用する）
	mutable std::unordered_map<const char *, std::vector<CommentLine>> CommentLines; // 使用する本文毎の行と位置の共有データ
	const Core::SyntaxModel &Syntax;
	const Core::CommentRepository &Repository;
	const StaticLint::OutputFacts &Facts;
	const Core::ControlFlow &Flow; // 共通の収集と個別検査が読む同じ木の事実
	const Core::SyntaxRules &Rules;
	const std::string &Src;
	DocumentationWork *DocWork = nullptr; // 検査ツールが指定した観測先（通常の静的解析では無）
	bool HasNumericBindings = false; // 此の完成候補の数値の要約を収集済か
	std::unordered_map<NodeId, NumericFact> NumericBindings; // 導入名毎の型・初期値・代入の要約
	std::unordered_map<NodeId, NumericFact> NumericExpressions; // 同じ完成構文の式の符号の要約
	std::unordered_map<NodeId, MemberHeader> MemberHeaders; // ノード毎の見出の修飾子と可視性
	std::unordered_map<NodeId, NodeId> LabelBodies; // 重なるラベルの最も内の文
	std::vector<size_t> BindingRefs; // 未評価の式を含めた導入以外の参照数
	std::unordered_map<NodeId, std::pair<bool, bool>> JsxPaths; // 描画への許容経路と論理積の左辺の構文の共有データ
	std::unordered_set<NodeId> MergedIfs; // 統合グループの先頭から報告済の内側の分岐
	std::unordered_map<NodeId, NamedLabels> GotoLabels; // 関数毎のラベルのインデックス
	std::unordered_map<const char *, const StaticLint::CommentFacts *> CommentTexts; // 最終テキストの開始位置から本文の記録へのインデックス

	template<class Function> static void VisitCmpPairs(const Core::SyntaxModel &Syntax, const NodeId Node, const Function &Inspect); // 比較の隣接する項と演算子の走査関数

	static StaticLint::OutputFacts ReadFacts(const Core::ValidatedOutput &Output); // 検証済の出力の検査データの取得関数

	/**
	 * 祖先の経路を伴う子孫の走査関数
	 * 計算量：訪れるノード数 N と呼出先の総費用 F に対し O(N + F)
	 * @param Root 根
	 * @param Visit 子孫の処理（偽は部分木を除外）
	 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
	 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
	 */
	template<class Function> void WalkChildrenCursor(const NodeId Root, Function &&Visit) const {
		// 根の無い場合の終了
		if(Root == NoNode) return;
		std::vector<NodeId> Path { Root }, Next { 0 };
		while(!Path.empty()) {
			if(Next.back() == Syntax.GetChildCount(Path.back())) {
				Path.pop_back();
				Next.pop_back();
				continue;
			}
			const NodeId Id = Syntax.GetChild(Path.back(), Next.back()++);
			bool IsDescend;
			if constexpr(std::is_invocable_r_v<bool, Function, NodeId, std::span<const NodeId>>) {
				IsDescend = Visit(Id, std::span<const NodeId>(Path));
			} else IsDescend = Visit(Id);
			if(IsDescend) {
				Path.push_back(Id);
				Next.push_back(0);
			}
		}
		// 終了
		return;
	}

	NodeId UnwrapGroup(NodeId Node) const; // 括弧又は単独の式列のラッパーの内側の取得関数
	NodeId GetFunctionStmtsOf(const NodeId Function) const; // 変更不能な関数の文の並びの取得関数
	uint32_t GetLineStartOf(const uint32_t Pos) const; // 共通の行頭表から位置を含む行の先頭を取得する関数
	uint32_t GetBeginOf(const NodeId Id) const; // ノードの開始位置の取得関数
	const StaticLint::CommentFacts &GetCommentInfo(const std::string_view Text) const; // コメントの投影データの取得関数
	uint32_t GetEndOf(const NodeId Id) const; // ノードの終端位置の取得関数
	bool IsBindingName(const NodeId Name) const; // 導入名を表せる葉の判定関数
	NodeId GetTokenChildOf(const NodeId Parent, const std::string_view Spelling) const; // 親の直下に在る名前無トークンの取得関数
	StaticLint::OutputPoint GetPointOf(const NodeId Id) const; // ノードの出力位置の取得関数
	bool HasError(const NodeId Id) const; // 投影した構文の破損の判定関数
	NodeId GetFunctionNameOf(const NodeId Function) const; // 定義の名前のノードの取得関数
	NodeId GetTrailingReturnTypeOf(NodeId Declarator) const; // 所属する宣言子の後置戻値型の型部分の取得関数
	bool HasExplicitTypeOf(const NodeId Node) const; // 仮引数又は束縛の型指定の有無の判定関数
	bool HasRequiredAuto(const NodeId Node) const; // 後置戻値型を導く先頭の auto かの判定関数
	NodeId GetExplicitReturnTypeOf(const NodeId Function) const; // 関数の明示された戻値型のノードの取得関数
	std::optional<size_t> CountStmtLines(const NodeId Function) const; // 本体の文の始まる行の数の取得関数
	StaticLint::OutputPoint GetPointInComment(const size_t Idx, const std::string_view Body, const size_t Offset) const; // コメント本文の位置を最終テキストへコピーする関数
	size_t AppendSrcLogicalLines(const CommentRecord &Record, std::vector<std::string> &Lines) const; // 装飾を除いた論理行を追加し，最初の本文位置を返す関数
	NodeId GetOutermostGroup(NodeId Id) const; // 単一式の最外の括弧の取得関数
	bool HasUnusedVal(const NodeId Node) const; // ラッパーを除いた式の結果を使用しない文脈の判定関数
	bool HasInitializedBinding(const NodeId Name, const NodeId Stmt) const; // 直前の文が束縛の初期値を持つかの判定関数
	bool HasUnnameableInitializer(const NodeId Node) const; // 推論する型が匿名型又は構造化束縛であるかの判定関数
	bool HasConditionBinding(const NodeId Node) const; // 条件の初期化文又は導入名を持つ分岐かの判定関数
	Core::RoleSet GetModifierRoles(Core::NodeId Node) const; // 修飾のラッパーとトークンの投影済の役割の取得関数
	bool IsJsxRenderingCond(NodeId Node); // 括弧と論理積だけを通る JSX の左辺の比較の判定関数
	std::string_view GetNodeView(const NodeId Node); // 投影済のトークン又はノードのソース範囲の参照取得関数
	bool IsIntegerLiteral(NodeId Node); // 整数リテラル又は其の単項符号の判定関数
	std::optional<uint64_t> IntLiteralVal(const std::string_view Text, std::string_view &Suffix); // 整数リテラルの値の取得関数
	bool IsNumericExponentReadSuccessful(const std::string_view Text, size_t &Pos, int64_t &Exponent); // 数値の指数の符号・全桁と桁区切の読取関数
	bool IsNumericVal(const NodeId Node, const uint64_t Val); // 指定された値の数値リテラルの判定関数
	bool IsNumericText(const std::string_view Text); // 単一の数値リテラルのテキストの判定関数
	NumericFact GetNumericFact(const NodeId Node); // 数値の根拠と負の値の可能性の取得関数
	void CollectNumericBindings(); // 初期値と書込から数値の符号を共有する関数
	int GetAccessRank(const Core::AccessMeaning &Access); // アクセス修飾子の集合に対応する順位の取得関数
	const MemberHeader &GetMemberHeader(const NodeId Node); // メンバの見出の修飾子と可視性の取得関数
	NodeId GetLabelBody(NodeId Node); // 重なるラベルを除いた文の取得関数
	void CollectBindingRefs(); // 共有の束縛事実に対応する参照数の収集関数
	std::string_view FindUnnamedToken(const NodeId Node, const std::string_view Option1, const std::string_view Option2); // 名前無子トークンからの候補一致トークン検索関数
	std::vector<std::string_view> LeadingCommentTexts(const NodeId Anchor, const std::span<const uint32_t> DirectiveStarts); // アンカーの手前に続く独立したコメントの本文の収集関数

	void Push(
		const StaticLint::OutputPoint Point,
		const LintRule Rule,
		const std::string_view Msg,
		std::vector<LintWarning> &Output,
		const LintSortVals &Vals = LintSortVals(),
		const Core::NodeId Target = Core::NoNode
	); // 出力位置の警告の追加関数

	void Push(
		const NodeId Node,
		const LintRule Rule,
		const std::string_view Msg,
		std::vector<LintWarning> &Output,
		const LintSortVals &Vals = LintSortVals()
	); // 違反１件追加関数

	void CheckParamType(const NodeId Node, const uint32_t Ordinal, std::vector<LintWarning> &Output); // 型を明示出来る仮引数の検査関数
	void CheckNestingDepth(std::vector<LintWarning> &Output); // ネストの深さの検査関数
	void CheckMemberDeclOrder(const NodeId Node, std::vector<LintWarning> &Output); // クラスメンバ宣言順違反検出関数
	void CheckMissingExplicitReturn(const NodeId Node, std::vector<LintWarning> &Output); // 関数本体終端と到達不能 return 検証関数
	void CheckDeclType(const NodeId Node, std::vector<LintWarning> &Output); // 型を明示出来る宣言子の検査関数
	void CheckParamTypes(const NodeId Params, std::vector<LintWarning> &Output); // 仮引数列を１度列挙する型の検査関数
	void CheckAccessSpecifierOrder(const NodeId Node, std::vector<LintWarning> &Output); // アクセス修飾子順序違反検出関数
	void CheckNumericJump(const NodeId Node, std::vector<LintWarning> &Output); // 段数付の脱出の検査関数
	void CheckGotoOrLabel(const NodeId Node, LabelTargetIdx &Labels, std::vector<LintWarning> &Output); // goto／ラベル付 break, continue 検出関数
	void CheckTypeInferenceKeyword(const NodeId Node, std::vector<LintWarning> &Output); // auto / var / any 使用検出関数
	void CheckReturnTypeHint(const NodeId Node, std::vector<LintWarning> &Output); // 関数戻値型の不在検出関数
	void CheckMacroConstant(const NodeId Node, std::vector<LintWarning> &Output); // #define 定数検出関数
	void CheckUsingNamespace(const NodeId Node, std::vector<LintWarning> &Output); // using namespace 検出関数

	void CheckReturnCommentPresence(
		const NodeId Node,
		const std::vector<std::pair<uint32_t, uint32_t>> &Comments,
		std::vector<LintWarning> &Output
	); // return コメント不在検出関数

	void CheckNullishCmpCandidate(const NodeId Node, std::vector<LintWarning> &Output); // ヌリッシュ比較縮約候補検出関数
	void CheckUnsafeIfMergeCandidate(const NodeId Node, std::vector<LintWarning> &Output); // 波括弧言語の if 統合候補検出関数
	void CheckBoolFlipPattern(const NodeId Node, std::vector<LintWarning> &Output); // 真偽値の排他的論理和の代入を勧める反転代入の検出関数
	void CheckDocCommentPresence(const NodeId Node, std::vector<LintWarning> &Output); // 関数ドキュメントコメント不在検出関数
	void CheckDocCommentForm(const size_t Idx, std::vector<LintWarning> &Output); // 規約の採用形式でないドキュメントの検出関数（整形仕様 8.4 のドキュメントの形式）

	void CheckZeroCmpPair(
		const NodeId Node,
		const NodeId Left,
		const NodeId Right,
		const NodeId Token,
		std::vector<LintWarning> &Output
	); // 隣接する２項の０比較候補の検出関数

	void CheckBoundaryCmpPair(const NodeId Left, const NodeId Right, const NodeId Token, std::vector<LintWarning> &Output); // 隣接する２項の整数リテラル境界比較検出関数
	void CheckUselessLocalVar(const NodeId Node, std::vector<LintWarning> &Output); // 無意味なローカル変数検出関数
	void CheckZeroCmp(const NodeId Node, std::vector<LintWarning> &Output); // 数値の０比較候補の検出関数
	void CheckCStyleCast(const NodeId Node, std::vector<LintWarning> &Output); // C 形式キャスト検出関数

	void CheckDocCommentCompleteness(
		const NodeId Node,
		const std::span<const std::string_view> Texts,
		std::vector<LintWarning> &Output
	); // ドキュメントコメントの項目と説明の不足の検出関数

	void CheckCmpBoundary(const NodeId Node, std::vector<LintWarning> &Output); // 整数リテラル境界比較検出関数（全言語）
	void CheckCompoundAssignIncrement(const NodeId Node, std::vector<LintWarning> &Output); // １の増減代入の検出関数
	void CheckUnusedPostfixIncrement(const NodeId Node, std::vector<LintWarning> &Output); // 結果未使用の後置増減候補検出関数
	std::vector<LintWarning> Run(); // 警告の取得関数

	LintAnalyzer(
		const Core::SyntaxModel &Syntax,
		const std::string &Src,
		const Core::CommentRepository &Comments,
		const StaticLint::OutputFacts &Facts
	); // 最終投影の読取専用の検査のコンストラクタ

public:

	static std::vector<LintWarning> Analyze(Core::ValidatedOutput &Output); // 専用の事実を確定して検証済の出力の規約違反を求める関数
};
