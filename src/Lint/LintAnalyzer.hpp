#pragma once

#include "../Core/CommentRepository.hpp"
#include "../Core/SyntaxModel.hpp"
#include "../Core/ValidatedOutput.hpp"
#include "OutputFacts.hpp"
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class CommentStructure;

enum class LintRule : uint8_t {
	NestingDepth, // ネストの深さ
	AccessOrder, // アクセス修飾子の順序
	ExplicitAccess, // 既定のアクセス修飾子の明記
	MemberOrder, // クラス成員の宣言順
	JumpUsage, // goto・ラベル付脱出
	ImmediateReturnVariable, // 即時返戻の局所変数
	InferredType, // 型推論の使用
	AnyType, // 型としての `any`
	MissingReturn, // 明示終端の不足
	ReturnReachability, // 明示終端の要否の未確定
	UnreachableReturn, // 到達不能な末尾の返戻
	FunctionNameStyle, // 関数名の形
	FunctionNameCharacters, // 関数名の文字種
	FunctionNameRole, // 関数名の役割
	DeclarationType, // 宣言の型の不在
	ParameterType, // 仮引数の型の不在
	ReturnType, // 戻値型の不在
	MissingDocumentation, // 文書の不在
	DocumentationSummary, // 文書の概要の不足
	DocumentationParameter, // 文書の仮引数の説明の不足
	DocumentationReturn, // 文書の戻値の説明の不足
	DocumentationConstraint, // 文書の制約の説明の不足
	DocumentationFailure, // 文書の失敗条件の説明の不足
	DocumentationComplexity, // 文書の計算量の説明の不足
	DocumentationNotation, // 文書の記法
	DocumentationItemForm, // 文書の説明項目の形式
	DocumentationName, // 文書の書出の宣言名
	ConstexprCandidate, // `constexpr` の候補
	LocalConst, // 局所 `const` の候補
	FloatSuffix, // 浮動小数の接尾辞
	ConstantMacro, // 定数マクロ
	Nullptr, // `NULL` の使用
	UsingNamespace, // `using namespace`
	CommentSentences, // コメントの文区切
	CommentPeriod, // コメントの句点
	StandardsReference, // 規約番号への言及
	RedundantUnary, // 冗長な二重符号
	MissingReturnComment, // 返戻コメントの不在
	ReturnCommentForm, // 返戻コメントの形式
	ZeroComparison, // ０比較
	JsxNonzeroCondition, // JSX の非０判定
	BooleanNegation, // 真偽値の反転
	NumericComparison, // 数値の厳密比較
	NullishComparison, // `null` / `undefined` の比較の縮約
	MergeIf, // 同一本体の if の統合
	ComparisonBoundary, // 比較境界の調整
	UnitAssignment, // `+= 1` / `-= 1`
	UnusedPostfix, // 後置増減の未使用結果
	CStyleCast, // C 形式のキャスト
	Other // 一覧外の警告と通知
}; // 静的検査の警告の識別子（整形仕様 8.4 の警告一覧の行順）

// LintAnalyzer が検出した規約違反１件を表す構造体
struct LintWarning {
	uint32_t Row; // 違反位置の行
	uint32_t Column; // 違反位置の列
	std::string Message; // 警告文（引数名等を含む動的メッセージにも対応する為 string_view でなく string で所有）
	LintRule Rule = LintRule::Other; // 警告一覧の識別子（同じ位置の警告を一覧の行順に並べる）
};

// 検証済の最終投影を走査し，規約違反を返す読取専用の検査クラス
class LintAnalyzer {
private:

	using NodeId = Core::NodeId;
	using Role = Core::Role;
	using LabelTargetIndex = std::unordered_map<NodeId, NodeId>; // ラベル節点毎の対象ループ索引（空節点は対象無）
	using CondContextIndex = std::unordered_map<NodeId, bool>; // 真偽値として評価する節点索引（値は JSX 式内か）

	using StringEventIndex =
	std::unordered_map<NodeId, std::unordered_map<std::string_view, std::vector<std::pair<uint32_t, bool>>>>; // スコープ毎の文字列宣言・代入事象を位置順に持つ索引

	// 型の数値の区分（比較境界の整数性と値域の確認に使う）
	struct NumericType {
		bool IsKnown = false; // 型を数値の区分へ引けたか
		bool IsFloat = false; // 浮動小数点型か
		bool IsSigned = true; // 符号付の整数型か
		bool IsCharacter = false; // 文字型か（文字コードの境界は意味の有る値）
		uint32_t Bits = 0; // 整数型の幅（環境で変わる物は最も狭い幅）
	};

	// 呼出側へ伝わる失敗（文書の失敗条件の項目と照合する物）
	struct FailureFlow {
		bool IsThrown = false; // 捕捉されずに呼出側へ伝わる送出か，署名が宣言した送出
		bool IsUnsure = false; // 例外処理の本体の中に在り，捕捉されるかを構文から決めれない送出
		bool IsPanic = false; // Rust の中断のマクロ
		bool IsError = false; // Rust の `Result` の戻値
	};

	static constexpr NodeId NoNode = Core::NoNode;
	const Core::SyntaxModel &Syntax;
	const Core::CommentRepository &Repository;
	const StaticLint::OutputFacts &Facts;
	const Core::SyntaxRules &Rules;
	const std::string &Source;
	std::unordered_map<const char *, const StaticLint::CommentFacts *> CommentTexts; // 最終字面の開始位置から本文の記録への索引

	static void Push(
		const StaticLint::OutputPoint Point,
		const LintRule Rule,
		const std::string_view Message,
		std::vector<LintWarning> &Out
	); // 出力位置の警告の追加関数

	static StaticLint::OutputFacts ReadFacts(const Core::ValidatedOutput &Output); // 検証済の出力の検査資料の取得関数
	size_t CountStatementLines(const std::string &Source, const NodeId Function, const size_t Limit) const; // 本体の文の始まる行の数の取得関数
	uint32_t BeginOf(const NodeId Id) const;
	uint32_t EndOf(const NodeId Id) const;
	StaticLint::OutputPoint PointOf(const NodeId Id) const;
	NodeId FunctionNameOf(const NodeId Function) const;
	NodeId ExplicitReturnTypeOf(const NodeId Function) const;
	NodeId ParameterNameOf(const NodeId Function, const std::string_view Name) const;
	NodeId TokenChildOf(const NodeId Parent, const std::string_view Spelling) const;
	StaticLint::OutputPoint PointInComment(const size_t Index, const std::string_view Body, const size_t Offset) const;
	bool HasError(const NodeId Id) const;
	NodeId OutermostGroup(NodeId Id) const;
	bool IsValueUnusedContext(const NodeId Id, const NodeId Parent) const;
	NodeId DescendantAt(NodeId Root, const uint32_t Begin, const uint32_t End) const;
	const StaticLint::CommentFacts &CommentInfo(const std::string_view Text) const;
	std::vector<std::string> SourceLogicalLines(const std::string_view Text) const;

	/**
	 * 祖先の経路を伴う子孫の走査関数
	 * @param Root 根
	 * @param Visit 子孫の処理（偽は部分木を除外）
	 */
	template<class Function> void WalkChildrenCursor(const NodeId Root, Function &&Visit) const {
		// 根の無い場合の終了
		if(Root == NoNode) return;
		std::vector<NodeId> Path { Root };
		std::vector<uint32_t> Next { 0 };
		while(!Path.empty()) {
			if(Next.back() == Syntax.ChildCount(Path.back())) {
				Path.pop_back();
				Next.pop_back();
				continue;
			}
			const NodeId Id = Syntax.Child(Path.back(), Next.back()++);
			bool Descend;
			if constexpr(std::is_invocable_r_v<bool, Function, NodeId, std::span<const NodeId>>) {
				Descend = Visit(Id, std::span<const NodeId>(Path));
			} else Descend = Visit(Id);
			if(Descend) {
				Path.push_back(Id);
				Next.push_back(0);
			}
		}
		// 終了
		return;
	}

	std::string_view NodeView(const std::string &Source, const NodeId Node); // ノードのソース上テキスト参照取得関数
	std::string_view UnwrapAssignableName(const std::string &Source, NodeId Lhs); // 代入可能式の包み剥離識別子名取得関数
	void Push(const NodeId Node, const LintRule Rule, const std::string_view Message, std::vector<LintWarning> &Out); // 違反１件追加関数
	void CheckAccessSpecifierOrder(const std::string &Source, const NodeId Node, std::vector<LintWarning> &Out); // アクセス修飾子順序違反検出関数
	void CheckGotoOrLabel(const std::string &Source, const NodeId Node, LabelTargetIndex &Labels, std::vector<LintWarning> &Out); // goto／ラベル付 break・continue 検出関数
	void CheckUselessLocalVariable(const std::string &Source, const NodeId Node, std::vector<LintWarning> &Out); // 無意味な局所変数検出関数
	bool IsAutoExceptionContext(const NodeId AutoNode); // 型推論キーワードの例外文脈判定関数
	void CheckTypeInferenceKeyword(const NodeId Node, std::vector<LintWarning> &Out); // auto / var / any 使用検出関数

	std::string_view FindUnnamedToken(
		const std::string &Source,
		const NodeId Node,
		const std::string_view Option1,
		const std::string_view Option2
	); // 名前無子トークンからの候補一致トークン検索関数

	void CheckMissingExplicitReturn(const NodeId Node, std::vector<LintWarning> &Out); // 関数本体終端と到達不能 return 検証関数
	bool HasMarkupDescendant(const NodeId Node); // 部分木の JSX 要素包含判定関数
	void CheckFunctionNamingConvention(const std::string &Source, const NodeId Node, std::vector<LintWarning> &Out); // 関数命名規則違反検出関数
	void CheckReturnTypeHint(const NodeId Node, std::vector<LintWarning> &Out); // 関数戻値型の不在検出関数
	void CheckDeclarationType(const NodeId Node, std::vector<LintWarning> &Out); // 型を明示出来る宣言子の検査関数
	void CheckParameterType(const NodeId Node, std::vector<LintWarning> &Out); // 型を明示出来る仮引数の検査関数

	std::vector<std::string_view> LeadingCommentTexts(
		const std::string &Source,
		const NodeId Anchor,
		const std::vector<uint32_t> &DirectiveStarts
	); // 錨の手前に続く独立したコメントの本文の収集関数

	bool IsDocumentationLiteral(const std::string &Source, const NodeId Statement); // Python の文書文字列に為る文かの判定関数

	void CheckDocCommentPresence(
		const std::string &Source,
		const NodeId Node,
		const std::span<const std::string_view> Leading,
		std::vector<LintWarning> &Out
	); // 関数ドキュメントコメント不在検出関数

	bool CatchesThrow(const std::string &Source, const NodeId Handler, const std::string_view Thrown); // 捕捉の節が送出を確かに受け止めるかの判定関数
	FailureFlow FailureFlowOf(const std::string &Source, const NodeId Node); // 呼出側へ伝わる失敗の判定関数（整形仕様 8.4 の文書の不足で文書の失敗条件の項目と照合する）

	void CheckDocCommentCompleteness(
		const std::string &Src,
		const NodeId Node,
		const std::span<const std::string_view> Texts,
		std::vector<LintWarning> &Out
	); // ドキュメントコメントの項目と説明の不足の検出関数

	void CheckMemberDeclarationOrder(const NodeId Node, std::vector<LintWarning> &Out); // クラスメンバ宣言順違反検出関数
	void CheckConstexprCandidate(const NodeId Node, std::vector<LintWarning> &Out); // constexpr 化候補検出関数

	void CheckConstBindingCandidate(
		const std::string &Source,
		const NodeId Node,
		const std::unordered_set<std::string> &MutatedNames,
		std::vector<LintWarning> &Out
	); // C/C++ const 候補検出関数

	bool IsFloatLiteral(const std::string_view Text); // リテラル文字列の浮動小数判定関数
	void CheckFloatSuffixCandidate(const std::string &Source, const NodeId Node, std::vector<LintWarning> &Out); // C/C++ 浮動小数接尾辞候補検出関数
	void CheckMacroConstant(const NodeId Node, std::vector<LintWarning> &Out); // #define 定数検出関数
	void CheckUsingNamespace(const std::string &Source, const NodeId Node, std::vector<LintWarning> &Out); // using namespace 検出関数

	void CheckReturnCommentPresence(
		const std::string &Source,
		const NodeId Node,
		const std::vector<std::pair<uint32_t, uint32_t>> &Comments,
		std::vector<LintWarning> &Out
	); // return コメント不在検出関数

	void MarkCondContext(const std::string &Source, const NodeId Host, CondContextIndex &Index); // 真偽値として評価される位置の印付関数

	void CheckZeroComparisonCondition(
		const std::string &Source,
		const NodeId Node,
		const CondContextIndex &Conditions,
		std::vector<LintWarning> &Out
	); // 制御構文条件の整数ゼロ比較候補の検出関数

	void CheckNumberStrictEquality(
		const std::string &Source,
		const NodeId Node,
		const CondContextIndex &Conditions,
		std::vector<LintWarning> &Out
	); // 数値比較の厳密等価検出関数

	void CheckNullishComparisonCandidate(const std::string &Source, const NodeId Node, std::vector<LintWarning> &Out); // ヌリッシュ比較縮約候補検出関数
	void CheckCStyleCast(const NodeId Node, std::vector<LintWarning> &Out); // C 形式キャスト検出関数
	void CheckNullMacroCandidate(const NodeId Node, std::vector<LintWarning> &Out); // 空ポインターの旧来のマクロの検出関数
	void CheckUnsafeIfMergeCandidate(const std::string &Source, const NodeId Node, std::vector<LintWarning> &Out); // 波括弧言語の if 統合候補検出関数
	void CheckStandardsReference(const size_t Index, const CommentStructure &Structure, std::vector<LintWarning> &Out); // 規約番号参照の検出関数
	void CheckDocCommentForm(const std::string &Src, const size_t Index, std::vector<LintWarning> &Out); // 規約の採用形式でない文書の検出関数（整形仕様 8.4 の文書の形式）

	void CheckCommentSentences(
		const size_t Index,
		const CommentStructure &Structure,
		const std::vector<uint32_t> &DirectiveStarts,
		std::vector<LintWarning> &Out
	); // コメントの文区切の検査関数

	void CheckBoolFlipPattern(const std::string &Source, const NodeId Node, std::vector<LintWarning> &Out); // 真偽値の排他的論理和の代入を勧める反転代入の検出関数
	std::optional<uint64_t> IntegerLiteralValue(const std::string_view Text, std::string_view &Suffix); // 整数リテラルの値の取得関数
	NumericType NumericTypeOf(const std::string_view Text); // 型の字面の数値の区分の取得関数
	std::string_view DeclaredTypeText(const std::string &Source, const NodeId Use); // 局所の名前の宣言の型の字面の取得関数
	void CheckCmpBoundary(const std::string &Source, const NodeId Node, std::vector<LintWarning> &Out); // 整数リテラル境界比較検出関数（全言語）

	bool LooksLikeStringVariable(
		const std::string &Source,
		const NodeId AssignNode,
		const std::string_view Name,
		StringEventIndex &Index
	); // 変数の文字列文脈推定判定関数

	void CheckCompoundAssignIncrement(
		const std::string &Source,
		const NodeId Node,
		StringEventIndex &Index,
		std::vector<LintWarning> &Out
	); // １の増減を増減演算子で書く言語の増分代入の検出関数

	void CheckUnusedPostfixIncrement(const std::string &Source, const NodeId Node, std::vector<LintWarning> &Out); // 結果未使用の後置増減候補検出関数
	void CheckRedundantDoubleSign(const std::string &Source, const NodeId Node, std::vector<LintWarning> &Out); // 冗長な二重符号 (`-(-x)` / `+(+x)`) 検出関数
	void CheckNestingDepth(const std::string &Source, const NodeId Root, std::vector<LintWarning> &Out); // ネストの深さの検査関数
	std::vector<LintWarning> Run(); // 警告の取得関数

	LintAnalyzer(
		const Core::SyntaxModel &Syntax,
		const std::string &Source,
		const Core::CommentRepository &Comments,
		const StaticLint::OutputFacts &Facts
	); // 最終投影の読取専用の検査の構築子

public:

	static std::vector<LintWarning> Analyze(const Core::ValidatedOutput &Output); // 検証済の出力の規約検査関数
};
