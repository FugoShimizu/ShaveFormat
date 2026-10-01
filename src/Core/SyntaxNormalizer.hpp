#pragma once

#include "SyntaxModel.hpp"
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Core {
	// 行幅に依存しない構文の正規化クラス（整形仕様 3.1，第５章）
	class SyntaxNormalizer {
	private:

		// 変更した節点だけを前の状態に重ねる循環判定用の節点
		struct StateNode {
			const KindDefinition *Definition = nullptr; // 種別
			uint64_t Fields = 0; // 親の中の場
			std::string Lexeme {}; // 字句
			std::vector<NodeId> Children {}; // 子の並び
			SyntaxFacts Facts {}; // 境界・制約・生成形式の事実
			size_t Hash = 0; // 子を含む構造の要約
		};

		// 控えの節点の値の参照（控えた値か，節点表の変更前の控え又は現在の値を指す）
		struct StateView {
			const KindDefinition *Definition; // 種別
			uint64_t Fields; // 親の中の場
			std::string_view Lexeme; // 字句
			std::span<const NodeId> Children; // 子の並び
			const SyntaxFacts *Facts; // 境界・制約・生成形式の事実
			size_t Hash; // 子を含む構造の要約
		};

		// 前の状態の節点を共有する構文の控え（最初の状態は節点毎の要約だけを持ち，値は節点表の変更前の控えから引く）
		struct StateSnapshot {
			std::shared_ptr<const StateSnapshot> Previous {}; // 変更前の状態
			std::unordered_map<NodeId, StateNode> Changes {}; // 此の編集で変わった節点
			std::vector<size_t> Hashes {}; // 最初の状態の節点毎の要約
			NodeId Root = NoNode; // 根
			size_t Hash = 0; // 完全比較の候補を選ぶ要約
		};

		// 制御本体の波括弧の付与・除去の候補
		struct BraceCandidate {
			NodeId Body = NoNode; // 本体
			bool IsAdd = false; // 付与か（偽は除去）
			std::vector<NodeId> Layers {}; // 除く塊（外側から）
			const CanonicalForm *Form = nullptr; // 本体を持つ構文の波括弧の塊の生成形式
			bool DependsOnInner = true; // 内側の波括弧の増減で判断が変わり得るか（後続の枝の結付を保つ除去と付与）
		};

		// 値が使われる枝を持つ式と文の並びの文の控え（必要に為った時だけ集める）
		struct ValueUse {
			const SyntaxNormalizer &Normalizer; // 対象を選ぶ正規化
			const SyntaxModel &Syntax; // 構文モデル
			std::optional<std::unordered_set<NodeId>> Used {}; // 集めた節点
			bool Contains(const NodeId Node); // 値が使われるかの判定関数
		};

		// 波括弧の正規化の走査の状態
		struct BraceScan {
			const SyntaxModel &Syntax; // 構文モデル
			ValueUse Values; // 値が使われる枝を持つ式と文の並びの文
			std::vector<BraceCandidate> Candidates {}; // 候補
		};

		// 括弧の判定で共有する端と連鎖の控え
		struct ParenScanMemo {
			std::unordered_map<NodeId, NodeId> After {};
			std::unordered_map<NodeId, NodeId> BlockLead {};
			std::unordered_map<NodeId, NodeId> BlockTop {};
			std::unordered_map<NodeId, NodeId> FirstLeaf {};
			std::unordered_map<NodeId, NodeId> OperatorTop {};
			std::unordered_map<NodeId, NodeId> LastLeaf {};
			std::unordered_map<NodeId, bool> LeftConstraint {};
			std::unordered_map<NodeId, bool> RightJump {};
		};

		static constexpr int UnknownInnerPrec = std::numeric_limits<int>::max(); // 優先順位の分からない内側の式の番兵
		std::vector<NodeId> WorkNodes {}; // 現在の規則群が再評価する節点（初回は空で役割の索引から選ぶ）

		static std::shared_ptr<const StateSnapshot> StateOf(
			const SyntaxModel &Syntax,
			std::shared_ptr<const StateSnapshot> Previous = nullptr
		); // 編集差分を共有する構文の状態の取得関数

		static StateView StateViewOf(const StateSnapshot &State, const SyntaxModel &Syntax, const NodeId Id); // 控えの節点の値の取得関数

		static bool SameState(
			const SyntaxModel &Syntax,
			const StateSnapshot &Lhs,
			const StateSnapshot &Rhs,
			const bool RequireIdentity = false
		); // 構文状態の完全比較関数

		static std::vector<NodeId> InDocumentOrder(
			const SyntaxModel &Syntax,
			const std::vector<NodeId> &Found,
			const std::initializer_list<Role> Barriers
		); // 節点の構文順の整列関数

		static NodeId FirstLeaf(const SyntaxModel &Syntax, const NodeId Id); // 部分木の最初の字句の葉の取得関数
		static NodeId LastLexicalLeaf(const SyntaxModel &Syntax, const NodeId Id); // 部分木の最後の字句の葉の取得関数
		static size_t NumericSuffixStart(const SyntaxRules &Rules, const std::string_view Text); // 数値リテラルの型接尾辞の開始位置の取得関数
		static void CollectCssStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan); // CSS の色と宣言末尾の正規化の収集関数
		static void CollectQuoteStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan); // 文字列の二重引用符への正規化の収集関数
		static void CollectOptionalParenStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan); // 言語が省略を許す括弧の除去の収集関数

		static bool IsBuiltinScalarName(
			const SyntaxModel &Syntax,
			NodeId Operand,
			std::unordered_map<NodeId, std::unordered_map<std::string, bool>> &Memo
		); // 組込の算術型・ポインタの変数かの判定関数

		static void CollectIncrementStep(
			const SyntaxModel &Syntax,
			const NodeId Id,
			ValueUse &Values,
			std::unordered_map<NodeId, std::unordered_map<std::string, bool>> &Memo,
			RewritePlan &Plan
		); // 値を使わない位置の後置の増減の前置化の収集関数

		static void CollectLoopStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan); // 無条件の反復の正規化の収集関数
		static void CollectBoolLiteralStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan); // 真偽値型の初期値の真偽値の名前への正規化の収集関数
		static void CollectConstPositionStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan); // 型の後の const の型の前への移動の収集関数
		static GeneratedNode ArglessReturn(const SyntaxModel &Syntax); // 引数の無い返戻文の生成関数
		static bool LeadsWithAwaitName(const SyntaxModel &Syntax, NodeId Node); // JS/TS の await の式を `await` 名への呼出・添字と誤読した後置の連鎖かの判定関数
		static std::string GeneratedFixed(const SyntaxModel &Syntax, const FormOperation Operation, const FormPartRole Part); // 文法の生成形式に定めた固定字句の取得関数
		static int OpPrecedence(const SyntaxModel &Syntax, const NodeId OpNode, std::unordered_map<NodeId, int> &Memo); // 演算子の優先順位の取得関数
		static NodeId RightEdgeChild(const SyntaxModel &Syntax, const NodeId Node); // 右端の子の取得関数
		static bool IsPostfixAtom(const SyntaxModel &Syntax, NodeId Node); // 後置の連鎖の原子式の判定関数
		static bool KeepsPostfixReceiver(const SyntaxModel &Syntax, const NodeId Paren, const NodeId Host); // 後置の式の受け手を包む括弧の保持判定関数
		static NodeId PrefixCallArguments(const SyntaxModel &Syntax, const NodeId Node); // 前置の演算子の呼出と読んだ Swift の括弧の実引数の並びの取得関数
		static bool HasBodyLikeExpression(const SyntaxModel &Syntax, const NodeId Node); // 制御構文の見出で本体の塊と紛れる式を持つかの判定関数
		static bool OpensTypeArguments(const SyntaxModel &Syntax, NodeId Operand, std::unordered_map<NodeId, int> &Memo); // TS の型引数の閉じ `>` の直後に来る被演算子かの判定関数

		static int OppositePrecOf(
			const SyntaxModel &Syntax,
			const NodeId OpParent,
			const bool IsLeftOperand,
			std::unordered_map<NodeId, int> &Memo
		); // 被演算子の逆側で先に結合する演算子の優先順位の取得関数

		static bool OpensBracketPair(const SyntaxModel &Syntax, const NodeId Node); // 対の括弧を開く節点かの判定関数
		static bool IsKeywordTypeCast(const SyntaxModel &Syntax, const NodeId Cast); // 語の型への変換かの判定関数
		static bool ReadsAsDeclaration(const SyntaxModel &Syntax, const NodeId Paren, const NodeId Inner); // 外すと宣言と読まれ得る括弧かの判定関数

		static bool KeepsExpressionParen(
			const SyntaxModel &Syntax,
			const NodeId Paren,
			const NodeId Inner,
			const bool IsInBraceHeader,
			std::unordered_map<NodeId, int> &Memo,
			std::unordered_map<NodeId, bool> &AngleMemo,
			ParenScanMemo &ScanMemo
		); // 冗長な式の括弧を外すかの判定関数

		static NodeId TypeParenContent(const SyntaxModel &Syntax, const NodeId Node); // 型を包む括弧の中身の取得関数
		static uint32_t CollectTypeParenSteps(const SyntaxModel &Syntax, const NodeId Node, RewritePlan &Plan); // 型を包む冗長な括弧の除去の収集関数
		static bool IfWithElse(const SyntaxModel &Syntax, const NodeId Node); // else の枝を持つ分岐かの判定関数
		static bool EndsWithOpenIf(const SyntaxModel &Syntax, NodeId Node, const bool IsThroughBraces); // 後続の枝を受け得る開いた if で終わるかの判定関数
		static bool IsFollowedByElse(const SyntaxModel &Syntax, const NodeId Body); // 後に else が続く本体かの判定関数

		static void RemoveMergedDecl(
			const SyntaxModel &Syntax,
			const NodeId Node,
			const AnchorTarget End,
			const std::vector<NodeId> &Kept,
			const std::unordered_map<NodeId, NodeId> &Twins,
			RewriteStep &Removal,
			RewritePlan &Plan
		); // 纏めた宣言の除去の収集関数

		static bool IsDeclSplitScope(const SyntaxModel &Syntax, NodeId Container); // 宣言を分ける有効範囲かの判定関数

		std::vector<NodeId> Candidates(
			const SyntaxModel &Syntax,
			const std::initializer_list<Role> Roles,
			const std::initializer_list<Role> Barriers,
			const bool IsWhole = false
		) const; // 役割から選んだ構文順の対象の取得関数

		std::vector<NodeId> ParentsOfRole(
			const SyntaxModel &Syntax,
			const Role ChildRole,
			const std::initializer_list<Role> Barriers
		) const; // 役割を持つ子の親の構文順の取得関数

		void CollectIncrementSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const; // 値を使わない位置の後置の増減の前置化の一括収集関数
		void CollectNodeSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const; // 節点毎の正規化の収集関数
		void CollectTrailingCommaSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const; // 単なる区切りの末尾カンマの除去の収集関数
		void CollectSeparatorSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const; // 改行に替える文・成員の区切りの除去の収集関数
		void CollectTypeAssociationSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const; // 型の合併・交差の左結合への正規化の収集関数
		void CollectTerminatorSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const; // 文・成員の終端と型本体の区切りの収集関数
		void CollectExplicitReturnSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const; // 通常関数の末尾の返戻の明示の収集関数
		void CollectTailReturns(const SyntaxModel &Syntax, RewritePlan &Plan) const; // 末尾の値を返す関数の返戻の収集関数
		void CollectStatementForms(const SyntaxModel &Syntax, const std::span<const NodeId> Changed, RewritePlan &Plan) const; // 単一文本体の後置形式の収集関数
		void CollectParenSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const; // 冗長な括弧の除去の収集関数
		void CollectFusedPrefixSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const; // 前置の演算子と融合する被演算子の包装の収集関数
		std::unordered_set<NodeId> UsedBranchValues(const SyntaxModel &Syntax) const; // 値が使われる枝を持つ式と文の並びの文の収集関数

		void CollectBodyBraces(
			BraceScan &Scan,
			const NodeId Body,
			const bool IsElseBody,
			const NodeId ValueHost,
			const NodeId Owner
		) const; // 制御本体の波括弧の付与・除去の候補の収集関数

		void CollectBranchBraces(BraceScan &Scan, const NodeId Node, const NodeId Parent, const NodeId Next) const; // 枝の値を結果にする式の本体の波括弧の候補の収集関数
		void CollectBraceSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const; // 制御本体の波括弧の正規化の収集関数
		void CollectImmutableBindingSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const; // 不変指定の正規化の収集関数
		void CollectCommaDeclSteps(const SyntaxModel &Syntax, RewritePlan &Plan, const bool ShouldSplitBindings = false) const; // カンマで並べる宣言の統合・分割の収集関数
		void SetWorkNodes(const SyntaxModel &Syntax, const std::span<const NodeId> Changed, const bool StopAtFunction); // 変更に依存する対象の登録関数

	public:

		static RewriteStep BraceBodyStep(const SyntaxModel &Syntax, const NodeId Body, const CanonicalForm &Form); // 本体を波括弧で包む生成形式の書換関数
		static std::string NormalizeNumericLiteral(const SyntaxRules &Rules, const std::string_view Text, const bool ShouldPad); // 数値リテラルの正規化関数
		static bool FusesPrefixOperator(const SyntaxRules &Rules, const char Operator, const char Operand); // 前置の演算子と被演算子の先頭の記号の融合判定関数
		static bool IsValueUnusedContext(const SyntaxModel &Syntax, const NodeId Node, const NodeId Parent); // 値が使われない文の位置かの判定関数

		static std::optional<std::string> DoubleQuotedBody(
			const SyntaxRules &Rules,
			const std::string_view Body,
			const std::string_view Prefix,
			const bool IsJsxAttribute
		); // 単引用符の文字列の本文を二重引用符の本文へ再表現する関数

		static std::optional<std::string> TripleDoubleQuotedBody(
			const SyntaxRules &Rules,
			const std::string_view Body,
			const std::string_view Prefix
		); // Python の三重単引用符の本文を三重二重引用符の本文へ再表現する関数

		static RewriteStep ContinuationStep(const SyntaxModel &Syntax, const NodeId Id); // 継続括弧の選択に対応する期待構文の構築関数
		void CollectTupleSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const; // 組と照合の冗長な包装の収集関数
		RewriteTrace Normalize(SyntaxModel &Syntax); // 構文の正規化関数
	};
} // namespace Core
