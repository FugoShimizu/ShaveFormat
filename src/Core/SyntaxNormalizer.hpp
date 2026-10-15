#pragma once

#include "SyntaxGeneration.hpp"
#include "SyntaxModel.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace Core {
	// 行幅に依存しない構文の正規化クラス（整形仕様 3.1，第５章）
	// 固定の規則順位で１度ずつ評価し，役割インデックスを適用と共に更新する
	class SyntaxNormalizer {
	private:

		friend struct SyntaxNormalizerAccess;

		using NodeCollectEntry = std::function<void(const SyntaxNormalizer &, const SyntaxModel &, NodeId, RewritePlan &)>; // ノードの規則の入口
		using RootCollectEntry = std::function<void(const SyntaxNormalizer &, const SyntaxModel &, RewritePlan &)>; // 根の規則の入口
		using CollectEntry = std::variant<NodeCollectEntry, RootCollectEntry>; // 所有範囲毎の収集関数
		using StmtNames = std::unordered_set<std::string>; // 後置形式の判定で使う名前の集合
		// 親の判定と接続を共有する正規化障壁
		using NormalizationBarrier = NormalizationFacts::NormalizationBarrier; // モデルが所有する事実の型
		using AngleFacts = std::unordered_map<NodeId, NormalizationFacts::AngleFact>; // ノード毎の山括弧の控え
		// 括弧の判定で共有する端と連鎖の控え
		using ParenScanMemo = NormalizationFacts::ParenScanMemo; // モデルが所有する事実の型
		using ParenCtx = NormalizationFacts::ParenCtx; // モデルが所有する事実の型
		using Generation = SyntaxGeneration; // 共通の構文生成部品

		enum class ParenAncestorKind : uint8_t {
			BareStatement, // 文の並び・語の本体又は根で止まる所属
			CasePattern,
			CountedLoop,
			Following,
			GroupingParen,
			OppositeLeft,
			OppositeRight,
			PostfixReceiver,
			PostfixUnary,
			PtrType,
			ReturnHost,
			StatementLead,
			TypeRightBoundary,
			TypeRightCallable,
			Count
		}; // 括弧の判定で共有する祖先の停止条件（末尾は表の要素数）

		// 束縛の導入又は終了の事象
		struct BindingEvent {
			size_t Event = 0; // 此の名前の導入又は範囲の終了の変化
			size_t Version = SIZE_MAX; // 此の時点の同じ名前の可視束縛（無も記録する）
		};

		// 時点毎の可視束縛の連鎖
		struct BoundVersion {
			std::string Name {}; // 其の時点で導入した名前
			size_t Prev = SIZE_MAX; // 同じ名前の直前の束縛
			size_t Depth = 0; // 有効な束縛の積上数
		};

		// 候補へ入る直前の束縛の時点
		struct BindingPoint {
			size_t Event = 0; // 候補に入る直前の名前インデックスの変化
			size_t ScopeStart = 0; // 同じ関数の束縛の開始位置
		};

		// 制御本体の波括弧の付与・除去の候補
		struct BraceCandidate {
			NodeId Body = NoNode; // 本体
			bool IsAdd = false; // 付与か（偽は除去）
			std::vector<NodeId> Layers {}; // 除くブロック（外側から）
			const CanonicalForm *Form = nullptr; // 本体を持つ構文の波括弧のブロックの生成形式
		};

		// 有効範囲の未確定な名前とマクロの要約
		struct UncertainScope {
			size_t End = 0; // 部分木の最後のドキュメント位置
			size_t LastMacro = 0; // 部分木の最後のマクロのドキュメント位置
			bool HasUnicode = false; // 部分木に非 ASCII 名を含むか
		};

		// 正規化の規則と収集の入口
		struct RuleDescriptor {
			std::string_view Name {}; // 規則の固定の識別名
			std::vector<Role> TargetRoles {}; // 直接の対象を選ぶ役割
			std::vector<Role> ChildRoles {}; // 所有者を対象にする子の役割
			CollectEntry Collect {}; // 対象の１度の評価の入口
			bool CanBatchReturnCompletion = false; // 末尾式を変えない返戻の追加を一括収集出来るか
			bool IsInnerFirst = false; // 内側の最終形を確定してから外側を評価するか
		};

		// 候補と祖先だけを繋ぐ構文順の作業枠
		struct DocOrderPath {
			NodeId First = NoNode; // 最初の子経路
			NodeId Next = NoNode; // 次の兄弟経路
			bool IsKnown = false; // 今回の整列で登録済か
			bool IsSelected = false; // 候補自身か
		};

		// 整列毎に登録済の枠だけを消して再使用する作業領域
		struct DocOrderScratch {
			std::vector<DocOrderPath> Paths {}; // ノード番号で引く経路
			std::vector<NodeId> Visited {}; // 今回登録した枠
			std::vector<NodeId> Missing {}; // 未登録の祖先と候補経路の走査を再使用する列
		};

		// 文の位置毎の可視束縛のインデックス
		struct StmtBindingIdx {
			std::unordered_map<std::string, std::vector<BindingEvent>> Names {}; // 名前毎の可視束縛の変化
			std::vector<BoundVersion> Versions {}; // 導入と巻戻を共有する不変の鎖
			std::unordered_map<NodeId, BindingPoint> Points {}; // 各見出の直前の束縛
		};

		// 増減判定で現在可視な型と束縛の積上位置
		struct ScalarBinding {
			bool IsBuiltin; // 組込の算術型又はポインタ型と確定して居るか
			size_t Depth; // 可視束縛の積上位置
		};

		// 条件の後置化で共有する文の要約
		struct StmtFacts {
			std::shared_ptr<StmtNames> Reads {}; // 部分木が読む名前
			std::shared_ptr<StmtNames> Writes {}; // 部分木が束縛する名前
			bool IsCondSafe = true; // 後置条件に置ける構文か
			bool IsBindingSafe = true; // 束縛名を確定出来るか
		};

		// 波括弧の正規化の走査の状態
		struct BraceScan {
			const SyntaxModel &Syntax; // 構文モデル
			std::vector<BraceCandidate> Candidates {}; // 候補
			std::unordered_map<NodeId, bool> Forms {}; // 内側から確定する本体の波括弧
		};

		// 有効範囲毎の未確定な名前のインデックス
		struct UncertainNameFacts {
			std::unordered_map<NodeId, UncertainScope> Scopes {}; // 有効範囲から引く共有要約
		};

		// 名前の解決・型の観測と最上位の束縛の方針
		struct LexicalPolicy {
			std::unordered_set<NodeId> TypeObservedBindings {}; // 型照会・捕捉・クラス本体から外側の束縛への参照で型を観測する導入名
			std::unordered_map<uint32_t, std::unordered_set<NodeId>> UnresolvedTypeNames {}; // 未解決の型の観測の名前から可視の祖先への対応
			bool IsDynamic = false; // 動的な名前解決を含むか
			bool IsModule = false; // 最上位で語彙的な束縛を持てるか
			bool ShouldPreserveBindingTypes = false; // 型の不変も確認してから不変指定するか
		};

		// 不変指定を判定する束縛の候補
		struct ImmutableCandidate {
			NodeId Decl = NoNode; // 不変指定の候補の宣言
			NodeId Marker = NoNode; // 可変のマーカー又は修飾子の並び
			NodeId Scope = NoNode; // 有効範囲
			bool IsAttributed = false; // 展開前の属性の範囲か
			std::vector<NodeId> BindingNames {}; // 束縛の同一性を持つ導入名
			std::vector<NodeId> ConstantBindings {}; // 定数式の初期値を持つ導入名
		};

		// 祖先の停止位置と，其の直下の子
		struct ParenAncestorBoundary {
			NodeId Child = NoNode; // 停止した祖先の直下の子
			NodeId Parent = NoNode; // 停止した祖先（無ければ根の外側）
			bool IsKnown = false; // 停止位置を取得済か
		};

		// 外したラッパーの中身と変更範囲の走査済のフラグ
		struct ParenReplacement {
			NodeId Content = NoNode; // ラッパーを置き換える中身
			bool IsInvalidated = false; // 今回の変更範囲を走査済か
		};

		// 同じ括弧の規則内で共有する祖先の経路
		struct ParenAncestorMemo {
			const SyntaxModel *Owner = nullptr; // 此の要約を所有するモデル
			std::vector<std::vector<ParenAncestorBoundary>> Tables {}; // 停止条件毎のノード番号で引く表
			std::vector<ParenReplacement> Replacements {}; // 外したラッパーから中身への接続
			std::vector<NodeId> Path {}; // 未取得の経路の再使用の作業列
			std::vector<NodeId> AliasPath {}; // 外したラッパーの接続の再使用の作業列
			std::vector<NodeId> Invalidated {}; // 今回失効させた範囲
			std::vector<NodeId> Pending {}; // 変更範囲の走査の再使用の作業列
		};

		static constexpr int UnknownInnerPrec = std::numeric_limits<int>::max(); // 優先順位の分からない内側の式の番兵
		mutable std::vector<uint8_t> MacroBindings; // 同じ規則内で共有する部分木のマクロ束縛（０は未判定，１は無，２は有）
		mutable std::vector<NodeId> MacroBindingNodes; // 規則の終了後に失効する判定済の位置（表全体を消去しない）
		mutable std::vector<std::pair<NodeId, bool>> MacroBindingWork; // 部分木の判定間で再使用する後行の作業列
		mutable bool HasMacroFacts = true; // 此の正規化にマクロ束縛の事実が在るか（単独の規則では有無を仮定しない）
		mutable std::unordered_map<NodeId, NodeId> DocstringValues; // 同じ引用符の規則内で共有する説明文字列の先頭の部品
		mutable ParenAncestorMemo ParenAncestors; // 同じ括弧の規則内で再使用する祖先の要約
		mutable DocOrderScratch DocOrder; // 此の正規化のインスタンスが再使用する整列の作業領域
		std::vector<std::pair<std::string_view, size_t>> *RuleWork = nullptr; // 検査で指定した規則毎の作業量の格納先
		static const std::vector<RuleDescriptor> &GetRules(); // 固定の共通規則表の取得関数
		static bool HasLeadingAwaitName(const SyntaxModel &Syntax, NodeId Node); // `await` 式を同名への呼出・添字と誤読した連鎖かの判定関数

		static std::optional<std::string> TripleDoubleQuotedBody(
			const SyntaxRules &Rules,
			const std::string_view Body,
			const std::string_view Prefix
		); // Python の三重単引用符の本文を三重二重引用符の本文へ再表現する関数

		static std::optional<std::string> DoubleQuotedBody(
			const SyntaxRules &Rules,
			const std::string_view Body,
			const std::string_view Prefix,
			const bool IsJsxAttribute
		); // 単引用符の文字列の本文を二重引用符の本文へ再表現する関数

		static const CanonicalForm *GetGeneratedFormOf(const SyntaxModel &Syntax, const FormOperation Operation); // 文法に共通の生成形式の取得関数

		static GeneratedNode GeneratedPart(
			const SyntaxModel &Syntax,
			const CanonicalForm &Form,
			const FormPartRole Role,
			const NodeId Body,
			const bool HasTrueName
		); // 生成形式の部品からノードを組み立てる関数

		static size_t GetNumericSuffixStart(const SyntaxRules &Rules, const std::string_view Text); // 数値リテラルの型接尾辞の開始位置の取得関数
		static bool CanShareParenLayers(const SyntaxModel &Syntax, const NodeId Outer, const NodeId Inner); // 同じ保持判定で一括除去する括弧の層かの判定関数
		static bool IsPostfixAtom(const SyntaxModel &Syntax, NodeId Node); // 後置の連鎖の原子式の判定関数
		static NodeId GetPrefixCallArgs(const SyntaxModel &Syntax, const NodeId Node); // 前置の演算子の呼出と読んだ Swift の括弧の実引数の並びの取得関数
		static NodeId GetTypeParenContent(const SyntaxModel &Syntax, const NodeId Node); // 型を包む括弧の中身の取得関数
		static NodeId GetRightEdgeChild(const SyntaxModel &Syntax, const NodeId Node); // 右端の子の取得関数
		static NormalizationFacts::TypeEdgeFact GetTypeEdge(const SyntaxModel &Syntax, const NodeId Node, ParenScanMemo &Memo); // 型の右端の共有要約の取得関数
		static bool HasElseBranch(const SyntaxModel &Syntax, const NodeId Node); // `else` の枝を持つ分岐かの判定関数
		static bool HasClauseLineBreak(const SyntaxModel &Syntax, NodeId Node); // 枝又は終端の前で改行を要する制御文かの判定関数
		static void CollectCssStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan); // CSS の色と宣言末尾の正規化の収集関数
		static GeneratedNode ArglessReturn(const SyntaxModel &Syntax); // 引数の無い返戻文の生成関数
		static bool ShouldWrapArg(const SyntaxModel &Syntax, const NodeId Node, const bool HasDelimiter); // 実引数の先頭の読みを保つ括弧が必要かの判定関数
		static bool IsControlValArg(const SyntaxModel &Syntax, const NodeId Node); // グループとして実引数へ置く制御値かの判定関数
		static NodeId GetControlValParenOwner(const SyntaxModel &Syntax, const NodeId Paren, const NodeId Inner); // 制御式の括弧を値に取る語の取得関数
		static GeneratedNode FormNode(const CanonicalForm &Form, const FormPartRole Part); // 生成形式の部品からノードを作る関数
		static bool HasPrefixOperatorFusion(const SyntaxRules &Rules, const unsigned char Operator, const unsigned char Operand); // 前置の演算子と被演算子の先頭の記号の融合判定関数
		static std::string NormalizeNumericLiteral(const SyntaxRules &Rules, const std::string_view Text, const bool ShouldPad); // 数値リテラルの正規化関数
		static bool IsBoundBefore(const SyntaxModel &Syntax, const StmtBindingIdx &Idx, const NodeId Target, const std::string &Name); // 同じ範囲で既に束縛済かの判定関数
		static bool IsParsedAsDecl(const SyntaxModel &Syntax, const NodeId Paren, const NodeId Inner, ParenScanMemo &ScanMemo); // 外すと宣言と読まれ得る括弧かの判定関数
		static bool IsKeywordTypeCast(const SyntaxModel &Syntax, const NodeId Cast); // 語の型への変換かの判定関数
		static bool HasOpeningBracketPair(const SyntaxModel &Syntax, const NodeId Node); // 対の括弧を開くノードかの判定関数
		static bool HasBodyLikeExpr(const SyntaxModel &Syntax, const NodeId Node); // 制御構文の見出で本体のブロックと紛れる式を持つかの判定関数
		static bool IsFollowedByElse(const SyntaxModel &Syntax, const NodeId Body); // 後に `else` が続く本体かの判定関数

		static bool HasOpenIfSuffix(
			const SyntaxModel &Syntax,
			NodeId Node,
			const bool IsThroughBraces,
			const std::unordered_map<NodeId, bool> &BraceForms
		); // 後続の枝を受け得る開いた `if` で終わるかの判定関数

		static void RemoveMergedDecl(
			const SyntaxModel &Syntax,
			const NodeId Node,
			const AnchorTarget End,
			const std::vector<NodeId> &Kept,
			const std::unordered_map<NodeId, AnchorTransfer> &SharedAnchors,
			RewriteStep &Removal,
			RewritePlan &Plan
		); // 纏めた宣言の除去の収集関数

		const NormalizationBarrier &GetNormalizationBarrierOf(const SyntaxModel &Syntax, const NodeId Id) const; // 親から継承する障壁の取得関数
		void CollectTrailingCommaNode(const SyntaxModel &Syntax, const NodeId Target, RewritePlan &Plan) const; // 単なる区切の末尾カンマの除去の収集関数
		bool HasMacroBindingIn(const SyntaxModel &Syntax, const NodeId Id) const; // 部分木に展開未確定のマクロ束縛が在るかの判定関数
		bool HasProtectedNormalizationAncestor(const SyntaxModel &Syntax, const NodeId Id) const; // 保護祖先の判定関数
		void CollectLoopStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const; // 無条件の反復の正規化の収集関数
		void CollectTerminatorNode(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const; // 文・メンバの終端と型本体の区切の収集関数
		void CollectStyleNodeStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const; // スタイルのトークン・区切の収集関数
		void CollectQuoteStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const; // 文字列の二重引用符への正規化の収集関数
		void CollectQuotedNodeStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const; // 引用文字列１ノードの収集関数
		void CollectBareNotationNode(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const; // 裸の引数のラッパーの収集関数
		void CollectBareNotationBatch(const SyntaxModel &Syntax, RewritePlan &Plan) const; // 所属を保つ裸の引数の書換の一括収集関数
		void CollectTypeQueryNode(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const; // 型照会の比較の収集関数
		void CollectImportOrderNode(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const; // 名前付のインポート順の収集関数
		void CollectBoolLiteralStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const; // 真偽値型の初期値の真偽値の名前への正規化の収集関数
		void CollectConstPosStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const; // 型の後の `const` の型の前への移動の収集関数
		void CollectOptionalParenStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const; // 言語が省略を許す括弧の除去の収集関数
		bool HasErrorNormalizationAncestor(const SyntaxModel &Syntax, const NodeId Id) const; // 対象を含むエラー障壁の判定関数
		void CollectExplicitReturnNode(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const; // 通常関数の末尾の返戻の明示の収集関数
		void CollectTailReturnNode(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const; // 末尾の値を返す関数の返戻の収集関数
		void CollectLoopTerminatorStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const; // 反復末尾１ノードの収集関数
		void CollectFusedPrefixNodeStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const; // 前置の演算子１ノードのラッパーの収集関数
		void CollectSeparatorNode(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const; // 改行に替える文・メンバの区切の除去の収集関数
		void CollectTypeAssociationNodeStep(const SyntaxModel &Syntax, const NodeId Node, RewritePlan &Plan) const; // 型の同値な連鎖１ノードの収集関数
		void CollectTupleNode(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const; // 組と照合の冗長なラッパーの収集関数
		std::vector<NodeId> SortInDocOrder(const SyntaxModel &Syntax, const std::vector<NodeId> &Found) const; // ノードの構文順の整列関数
		std::vector<NodeId> GetCandidates(const SyntaxModel &Syntax, const std::initializer_list<Role> Roles) const; // 役割から選んだ構文順の対象の取得関数
		void CollectNumStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const; // 数値１ノードの収集関数
		void CollectNumBatch(const SyntaxModel &Syntax, RewritePlan &Plan) const; // 独立した数値トークンの正規化の一括収集関数

		void CollectIncrementStep(
			const SyntaxModel &Syntax,
			const NodeId Id,
			const std::unordered_map<std::string_view, ScalarBinding> &Scalars,
			const size_t ScopeStart,
			RewritePlan &Plan
		) const; // 値を使わない位置の後置の増減の前置化の収集関数

		void CollectIncrementBatch(const SyntaxModel &Syntax, RewritePlan &Plan) const; // 値を使わない位置の後置の増減の前置化の一括収集関数

		void CollectStmtNode(
			const SyntaxModel &Syntax,
			const NodeId Id,
			const StmtBindingIdx &Bindings,
			const std::unordered_map<NodeId, NodeId> &PostfixStmts,
			const std::unordered_map<NodeId, StmtFacts> &Summaries,
			RewritePlan &Plan
		) const; // 単一文本体の後置形式の収集関数

		StmtBindingIdx BuildStmtBindingIdx(const SyntaxModel &Syntax, const NodeId Scope, RewritePlan *const Plan = nullptr) const; // 同じアルゴリズムで現在のインデックスを構築する関数
		void CollectStmtBatch(const SyntaxModel &Syntax, RewritePlan &Plan) const; // 独立した後置形式の共通評価関数
		int GetOperatorPrec(const SyntaxModel &Syntax, const NodeId OperatorNode, std::unordered_map<NodeId, int> &Memo) const; // 演算子の優先順位の取得関数
		bool HasAngleConflict(const SyntaxModel &Syntax, const NodeId Root, AngleFacts &Memo) const; // 型引数と紛れる山括弧を持つ式かの判定関数
		void PrepareParenAncestors(const SyntaxModel &Syntax) const; // 括弧の規則と同じ寿命の要約を準備する関数
		NodeId GetParenReplacement(const SyntaxModel &Syntax, NodeId Node) const; // 外したラッパーの現在の中身の取得関数

		bool IsParenAncestorBoundary(
			const SyntaxModel &Syntax,
			const NodeId Child,
			const NodeId Parent,
			const ParenAncestorKind Kind
		) const; // 祖先の停止条件の判定関数

		ParenAncestorBoundary GetParenAncestorOf(const SyntaxModel &Syntax, const NodeId Node, const ParenAncestorKind Kind) const; // 同じ停止条件の祖先の要約の取得関数
		void UpdateParenAncestors(const SyntaxModel &Syntax, const RewritePlan &Plan, const size_t FirstStep) const; // 括弧の書換の影響範囲の要約を更新する関数
		int GetOppositePrecOf(const SyntaxModel &Syntax, const NodeId OperatorParent, const bool IsLeftOperand) const; // 被演算子の逆側で先に結合する演算子の優先順位の取得関数
		bool HasTypeArgOpener(const SyntaxModel &Syntax, NodeId Operand, std::unordered_map<NodeId, int> &Memo) const; // TS の型引数の閉じ `>` の直後に来る被演算子かの判定関数
		bool ShouldPreservePostfixReceiver(const SyntaxModel &Syntax, const NodeId Paren, const NodeId Host) const; // 後置の式の受手を包む括弧の保持判定関数
		void CollectTypeParenSteps(const SyntaxModel &Syntax, const NodeId Node, RewritePlan &Plan) const; // 型を包む冗長な括弧の除去の収集関数

		bool ShouldPreserveExprParen(
			const SyntaxModel &Syntax,
			const NodeId Paren,
			const NodeId Inner,
			const NodeId ControlOwner,
			std::unordered_map<NodeId, int> &Memo,
			AngleFacts &AngleMemo,
			ParenScanMemo &ScanMemo
		) const; // 式の括弧を保つ必要が在るかの判定関数

		void CollectParenNode(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const; // 冗長な括弧の除去の収集関数

		void CollectBodyBraces(
			BraceScan &Scan,
			const NodeId Body,
			const NodeId ValHost,
			const NodeId Owner,
			const bool IsElseBody
		) const; // 制御本体の波括弧の付与・除去の候補の収集関数

		void CollectBranchBraces(BraceScan &Scan, const NodeId Node, const NodeId Parent, const NodeId Next) const; // 枝の値を結果にする式の本体の波括弧の候補の収集関数
		void CollectBraceCandidates(BraceScan &Scan, const NodeId Target) const; // １所有者の波括弧の意味候補の収集関数
		void CollectBraceBatch(const SyntaxModel &Syntax, RewritePlan &Plan) const; // 制御本体の波括弧の正規化の収集関数
		UncertainNameFacts GetUncertainNames(const SyntaxModel &Syntax) const; // 未確定な展開と名前の事実の取得関数
		std::unordered_set<NodeId> CollectImmutableBindings(const SyntaxModel &Syntax, RewritePlan &Plan) const; // 宣言子毎の不変指定の判定の収集関数
		LexicalPolicy GetLexicalPolicy(const SyntaxModel &Syntax) const; // ドキュメントの語彙的な束縛の前提の取得関数
		bool CanPreserveBindingType(const SyntaxModel &Syntax, const LexicalPolicy &Policy, const NodeId Decl) const; // 不変指定で束縛の型を保てるかの判定関数
		bool IsTypeObservedBinding(const SyntaxModel &Syntax, const LexicalPolicy &Policy, const NodeId Name) const; // 型を観測される導入名かの判定関数

		void CollectIndivisibleLexicalBindings(
			const SyntaxModel &Syntax,
			const LexicalPolicy &Policy,
			const bool CanChangeBinding,
			RewritePlan &Plan
		) const; // 分割出来ない位置の語彙的な束縛の不変指定の収集関数

		void CollectDeclBatch(const SyntaxModel &Syntax, RewritePlan &Plan) const; // 不変指定と宣言グループの最終形の収集関数
		std::vector<NodeId> GetRuleTargets(const SyntaxModel &Syntax, const RuleDescriptor &Rule) const; // 役割インデックスからの構文順の対象の取得関数

		void RunRule(
			SyntaxModel &Syntax,
			const RuleDescriptor &Rule,
			const bool HasFollowingRule,
			RewriteTrace &Trace,
			const bool HasMacroBindings = true
		) const; // 規則適用

	public:

		RewriteTrace Normalize(SyntaxModel &Syntax); // 構文の正規化関数
	};
} // namespace Core
