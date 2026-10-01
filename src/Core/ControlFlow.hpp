#pragma once

#include "SyntaxModel.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Core {
	enum class ReturnKind : uint8_t { Main, Never, Value, Void }; // 関数の戻値の区分（`Main` は末尾で `0` を返す C / C++ の起動点）
	enum class EndReachability : uint8_t { Reachable, Unknown, Unreachable }; // 関数の末尾の到達可能性（`Unknown` は定数式の値又は既定の節の無い切替へ来る値の範囲に依り，実行時とコンパイル時の到達可能性を確かめれない）
	enum class ReturnEvidence : uint8_t { Declared, Inferred, Unknown }; // 戻値の区分の根拠（型の宣言，本体の返戻文からの推論，根拠無）

	// 関数の戻値の区分と其の根拠
	struct ReturnShape {
		ReturnKind Kind; // 戻値の区分
		ReturnEvidence Evidence; // 区分の根拠
	};

	// 通常関数の本体の要約
	struct FunctionEnd {
		NodeId Statements; // 文を直下に並べる節点
		NodeId Last; // 最後の文
		ReturnKind Kind; // 戻値の区分
	};

	// 関数内の goto と到達可能ラベルの索引
	struct GotoIndex {
		std::unordered_map<NodeId, NodeId> Targets; // goto 毎の飛先（未解決は無し）
		std::unordered_map<NodeId, std::pair<size_t, size_t>> Ranges; // 節点毎の構文順の半開範囲
		std::vector<size_t> Entries; // 到達候補となるラベルの構文順の位置
		NodeId Body = NoNode; // 索引を作った関数本体
	};

	// 関数の制御の流れを構文だけで判定するクラス（整形仕様 5.2, 7.4, 8.4 で共有する）
	class ControlFlow {
	private:

		using TerminationResults = std::unordered_map<uint64_t, bool>; // 節点と判定範囲の組毎の終端の判定結果
		using NameConstants = std::unordered_map<std::string, bool>; // 名前毎の，同じ範囲の同名の宣言に定数の宣言が有るか

		enum class TerminationRule : uint8_t {
			None,
			Branch,
			Expression,
			Fallthrough,
			InfiniteLoop,
			KeywordJump,
			Labeled,
			Loop,
			Preprocessor,
			SingleBody,
			Statements,
			Switch,
			Transfer,
			Try
		}; // 終端の判定規則（子を積む段と結果を合わせる段で共有する）

		// 名前の宣言（名前の字句・有効範囲・定数の宣言か）
		struct NameDeclaration {
			NodeId Name; // 名前の字句
			NodeId Scope; // 有効範囲の節点（宣言を含む最も内側の塊・繰返・捕捉の節・関数・型の本体）
			bool IsConstant; // 定数を宣言する語を名前より前に持つか
		};

		// 関数から見える名前の表（関数の中の宣言と，外側の範囲の直下の宣言）
		struct VisibleNameTable {
			std::unordered_map<std::string, std::vector<NameDeclaration>> Locals; // 名前毎の関数の中の宣言
			NameConstants Outer; // 名前毎の，名前を宣言する最も内側の外側の範囲の同名の宣言に定数の宣言が有るか
		};

		// 分岐文の枝
		struct BranchParts {
			NodeId Then = NoNode; // 真枝
			std::vector<NodeId> Alternatives; // 後続の枝の本体
			bool HasElse = false; // 条件を問わない最後の枝を持つか
		};

		// 例外処理文の節の本体
		struct TryParts {
			NodeId Body = NoNode;
			NodeId Else = NoNode;
			NodeId Finally = NoNode; // 本体・例外の無かった時の節・後始末の節
			std::vector<NodeId> Handlers; // 捕捉の節
		};

		// 切替の節の本文
		struct CaseBody {
			std::vector<NodeId> Statements; // 本文を開く記号より後の文（素通りの文を含む）
			bool IsOpened = false;
			bool IsArrow = false; // 本文を開く記号の有無と矢印か
		};

		// 脱出文と其の飛先が届く最も外側の深さ
		struct ExitEvent {
			NodeId Node = NoNode;
			NodeId Destination = NoNode;
			NodeId Target = NoNode; // 脱出文，goto の投影済の飛先，脱出の解決済の飛先
			uint32_t Depth = 0; // 脱出が届く最も外側の節点の深さ
			bool IsGoto = false;
			bool Continues = false; // goto に依る脱出か，飛先の繰返を続けるか
		};

		// 走査中の祖先に在る脱出可能な文
		struct ExitPath {
			std::vector<NodeId> BreakTargets;
			std::vector<NodeId> ContinueTargets; // 無名の脱出と継続で数える祖先
		};

		// 本体内の脱出を判定範囲毎に再利用する索引
		struct LoopExitIndex {
			std::unordered_map<NodeId, std::pair<size_t, size_t>> Ranges; // 訪れた節点毎の構文順の半開範囲
			std::unordered_map<NodeId, uint32_t> Depths; // 訪れた節点毎の根からの深さ
			std::vector<NodeId> Postorder; // 子から親への集計順
			std::vector<ExitEvent> Events; // 脱出文毎の飛先
			std::unordered_map<NodeId, std::unordered_map<NodeId, uint32_t>> MinimumByScope; // 判定範囲毎の部分木の最小脱出深さ
			NodeId Root = NoNode; // 索引の根
			bool HasGoto = false; // goto の飛先に依る判定範囲の違いの有無
		};

		const SyntaxModel &Syntax; // 判定する構文
		const VisibleNameTable *VisibleNames = nullptr; // 定数と為り得る条件を常に真と看做す関数から見える名前の表（到達可能性を悲観的に判定する複製だけが持つ）
		bool AssumesCaseCoverage = false; // 記述した全ての節が閉じる切替へ不一致の値が来ないと看做すか（到達可能性を確かめれない末尾を見分ける複製だけが立てる）
		mutable std::unordered_map<NodeId, NameConstants> MemberNames; // 型の本体・外側の塊毎の直下の宣言の名前の表（関数毎の判定で共有する）
		static uint64_t TerminationKey(const NodeId Node, const NodeId Scope); // 終端の判定結果の鍵の生成関数
		static bool KnownTermination(const TerminationResults &Known, const NodeId Node, const NodeId Scope); // 判定済の文の終端の取得関数
		NodeId FirstChildNode(const NodeId Node) const; // 先頭の子の取得関数
		NodeId ExitLabel(const NodeId Node) const; // 脱出文のラベルの取得関数
		std::pair<NodeId, bool> ResolveExitTarget(const NodeId Node, const bool Continues, const ExitPath &Path) const; // 脱出文の飛先の解決関数
		void BuildLoopExitIndex(const NodeId Root, const GotoIndex &Gotos, LoopExitIndex &Index) const; // 脱出の索引の構築関数

		bool HasLoopExit(
			const NodeId Body,
			const NodeId Loop,
			const GotoIndex &Gotos,
			LoopExitIndex &Index,
			const NodeId TransferScope = NoNode
		) const; // 対象ループからの脱出有無判定関数

		NodeId ScopeOf(const NodeId Declaration) const; // 宣言の有効範囲の取得関数
		std::vector<NameDeclaration> DeclarationsIn(const NodeId Scope, const bool IsMemberScope) const; // 範囲の名前の宣言の取得関数
		VisibleNameTable VisibleNamesOf(const NodeId Function) const; // 関数から見える名前の表の取得関数
		bool MayBeConstantCondition(const NodeId Cond) const; // 定数式と評価され得る条件かの判定関数
		bool IsAlwaysTrueCondition(NodeId Cond) const; // ループ条件の真値リテラル判定関数
		bool IsAssumedTrueCondition(const NodeId Cond) const; // 繰返を続ける条件と看做すかの判定関数
		bool IsInfiniteLoop(const NodeId Node, NodeId &Body) const; // 条件で終わらない繰返かの判定関数
		bool IsNoReturnCall(const NodeId Node) const; // 戻らない事を言語・標準ライブラリが定める呼出かの判定関数
		std::vector<NodeId> StatementsIn(const NodeId Container) const; // 文を直下に並べる節点の文の取得関数
		std::vector<NodeId> PreprocessorStatements(const NodeId Node) const; // 前処理分岐の文の取得関数
		BranchParts BranchesOf(const NodeId Node) const; // 分岐文の枝の取得関数
		TryParts TryPartsOf(const NodeId Node) const; // 例外処理文の節の取得関数
		std::pair<NodeId, std::vector<NodeId>> SwitchCases(const NodeId Node) const; // 切替文の節の取得関数
		bool IsFallthroughStatement(const NodeId Node) const; // 次の節へ進む文かの判定関数
		CaseBody CaseBodyOf(const NodeId Case) const; // 切替の節の本文の取得関数
		bool IsIrrefutablePattern(const NodeId Pattern) const; // 全ての値に一致する照合かの判定関数
		bool IsDefaultCase(const NodeId Case) const; // 不一致の値を受ける切替の節かの判定関数
		bool RequiresExhaustiveness(const NodeId Case) const; // 処理系が網羅を検査する照合の切替の節かの判定関数
		NodeId FallthroughEntry(const NodeId Node, const NodeId Scope) const; // 次の枝へ進む語の属する切替の節の取得関数
		TerminationRule TerminationRuleOf(const NodeId Node, const NodeId Scope) const; // 終端の判定規則の取得関数
		NodeId SingleBodyOf(const NodeId Node) const; // 本体を一度だけ実行する文の本体の取得関数
		void TerminationOperands(const NodeId Node, const NodeId Scope, std::vector<std::pair<NodeId, NodeId>> &Out) const; // 終端の判定が読む子の収集関数

		bool DoStatementsTerminate(
			const std::span<const NodeId> Statements,
			const GotoIndex &Gotos,
			const NodeId Scope,
			const TerminationResults &Known
		) const; // 文の並びの終端判定関数

		bool DoesSwitchTerminate(
			const NodeId Node,
			const GotoIndex &Gotos,
			const NodeId TransferScope,
			const TerminationResults &Known,
			LoopExitIndex &Index
		) const; // 切替文の全分岐終端判定関数

		bool DoesIfTerminate(
			const NodeId Node,
			const GotoIndex &Gotos,
			const NodeId TransferScope,
			const TerminationResults &Known,
			LoopExitIndex &Index
		) const; // 分岐文の全枝終端判定関数

		bool DoesTryTerminate(const NodeId Node, const NodeId TransferScope, const TerminationResults &Known) const; // 例外処理文の終端判定関数

		bool TerminatesWith(
			const NodeId Node,
			const GotoIndex &Gotos,
			const NodeId Scope,
			const TerminationResults &Known,
			LoopExitIndex &Index
		) const; // 判定済の子からの終端判定関数

		TerminationResults EvaluateTermination(const std::span<const NodeId> Roots, const GotoIndex &Gotos, const NodeId Scope) const; // 終端の判定結果の構築関数
		bool DoStatementsTerminate(const NodeId Container, const GotoIndex &Gotos) const; // 文を直下に並べる節点の終端判定関数
		bool IsOrdinaryFunction(const NodeId Type) const; // 通常関数の節点かの判定関数
		NodeId StatementsOf(const NodeId Node) const; // 通常関数の文の並びの取得関数
		bool HasFlowDescendant(const NodeId Node, const Role Desired) const; // 同じ関数に属する指定の役割の子孫の有無の判定関数
		bool IsGeneratorLike(const NodeId Node, const NodeId Statements) const; // 生成器・コルーチンかの判定関数
		bool IsPlaceholderBody(const NodeId Statements) const; // 中身を持たない事を表す本体かの判定関数
		bool IsNoReturnDeclared(const NodeId Node) const; // 戻らない事を宣言した C / C++ の関数かの判定関数
		bool IsProgramEntry(const NodeId Node) const; // C / C++ の起動点かの判定関数
		bool IsArmSwitch(const NodeId Type) const; // 分岐を全て列挙する切替かの判定関数
		bool HasValueReturn(const NodeId Body) const; // 同じ関数に属する値付の返戻文の有無の判定関数
		bool ReturnsPointerOrReference(const NodeId Function) const; // C / C++ のポインタ・参照を返す関数かの判定関数
		NodeId ResultTypeOf(const NodeId Node) const; // 関数の戻値の型の取得関数
		bool HasExpressionBody(const NodeId Node) const; // 式を本体とする関数かの判定関数
		bool IsSingleImplicitReturnBody(const NodeId Function, const FunctionEnd &End) const; // 暗黙返戻の候補と為る単一式の本体かの判定関数
		bool IsImplicitReturn(const NodeId Function, const FunctionEnd &End) const; // 単一式の本体が値を暗黙に返すかの判定関数
		bool ContainsUnexpandedBodySyntax(const NodeId Statements) const; // 本体に未展開のマクロ・属性が在るかの判定関数
		bool KeepsTailTemporaries(const NodeId Tail) const; // 末尾式が文の終わり迄生きる一時値を作るかの判定関数

	public:

		static bool IsReturn(const SyntaxModel &Syntax, const NodeId Node); // 関数から戻る文・式かの判定関数
		static bool HasUnexpandedAttribute(const SyntaxModel &Syntax, NodeId Node, std::unordered_map<NodeId, bool> &Cache); // Rust の展開前の属性の範囲かの判定関数
		NodeId NextDeclarator(const NodeId Node) const; // C / C++ の次の宣言子の取得関数
		bool IsFlowBoundary(const NodeId Type) const; // 独立した制御フロー境界の判定関数
		GotoIndex CollectGotoTargets(const NodeId Body) const; // 関数内 goto 飛先収集関数
		bool HasGotoEntry(const NodeId Node, const GotoIndex &Gotos) const; // 節点内への goto 入口有無判定関数
		bool HasReturnInFlow(const NodeId Node) const; // 同じ制御フロー内の返戻文包含判定関数
		std::vector<NodeId> ReturningStatementsAfterTermination(const NodeId Container, const GotoIndex &Gotos) const; // 終端した後の返戻文を含む文の取得関数
		ReturnShape ReturnShapeOf(const NodeId Node) const; // 関数の戻値の区分の判定関数
		std::optional<FunctionEnd> ClassifyFunction(const NodeId Node) const; // 通常関数の本体の要約関数
		bool CanMakeImplicitReturnExplicit(const NodeId Function, const FunctionEnd &End) const; // 暗黙の単一式を明示返戻へ変えれるかの判定関数
		NodeId TailExpression(const NodeId Last) const; // 塊の値を与える末尾式の取得関数
		EndReachability EndReach(const NodeId Function, const FunctionEnd &End) const; // 通常関数の末尾の到達可能性の判定関数
		bool IsReturnCompletionUncertain(const NodeId Function, const FunctionEnd &End, std::unordered_map<NodeId, bool> &Cache) const; // 末尾の返戻の補完を構文から確かめれないかの判定関数

		ControlFlow(const SyntaxModel &Syntax); // コンストラクタ
	};
} // namespace Core
