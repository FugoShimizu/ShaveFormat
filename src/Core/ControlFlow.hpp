#pragma once

#include "SyntaxModel.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Core {
	enum class ReturnKind : uint8_t { Main, Never, Val, Void }; // 関数の戻値の区分 // 末尾で `0` を返す C / C++ のエントリーポイント // 通常復帰しない // 値を返す // 値を返さない
	enum class EndReachability : uint8_t { Reachable, Unknown, Unreachable }; // 関数の末尾の到達可能性 // 到達する // ジャンプ先の未解決，定数式の値又は既定の節の無い切替へ来る値の範囲に依り，実行時とコンパイル時の到達可能性を確かめれない // 到達しない
	enum class ReturnEvidence : uint8_t { Declared, Inferred, Unknown }; // 戻値の区分の根拠 // 型の宣言 // 本体の返戻文からの推論 // 根拠無

	// 関数の戻値の区分と其の根拠
	struct ReturnShape {
		ReturnKind Kind; // 戻値の区分
		ReturnEvidence Evidence; // 区分の根拠
		std::optional<bool> HasCallResult = std::nullopt; // 呼出が値を返すか（未確定は空，本体の返戻とは別に確定する）
	};

	// 通常関数の本体の要約
	struct FunctionEnd {
		NodeId Stmts; // 文を直下に並べるノード
		NodeId Last; // 最後の文
		ReturnKind Kind; // 戻値の区分
		ReturnEvidence Evidence = ReturnEvidence::Unknown; // 宣言済の戻値の根拠
	};

	// 関数内の goto と到達可能ラベルのインデックス
	struct GotoIdx {
		std::unordered_map<NodeId, NodeId> Targets; // goto 毎のジャンプ先（未解決は無）
		std::vector<NodeId> Entries; // 到達候補と為るラベル（現在の構文順）
		NodeId Body = NoNode; // インデックスを作った関数本体
		bool HasUnresolvedTarget = false; // 同じ制御フロー内でジャンプ先を確認出来ない goto が在るか
	};

	// 関数の制御の流れを構文だけで判定するクラス（整形仕様 5.2, 7.4, 8.4 で共有する）
	class ControlFlow {
	private:

		friend class SyntaxModel;
		friend class ReferenceProjection;
		friend struct SyntaxNormalizerAccess;

		struct LoopExitIdx; // 所有範囲毎の脱出インデックス

		using ChildEvaluation = std::pair<const KindDef *, EvaluationClass>; // 親の固定定義と取得済の子の評価時機
		using CalleeSummary = SyntaxModel::CalleeSummary; // モデルが所有する呼出先の構造
		using CallableIntroduction = Core::CallableIntroduction; // 共有する供給元の型
		using CallablePosition = Core::CallablePosition; // 共有する供給元の型
		using CallablePositions = Core::CallablePositions; // 共有する供給元の型
		using CallableResolution = Core::CallableResolution; // 共有する供給元の型
		using CallableIndex = Core::CallableIndex; // 共有する供給元の型
		using CallableNames = Core::CallableNames; // 共有する供給元の型
		using CallableMembers = Core::CallableMembers; // 共有する供給元の型
		using CallableImports = Core::CallableImports; // 共有する供給元の型
		using TerminationRule = EvaluationRule; // 投影済の評価契約の合成規則
		using CallableNameOwner = std::shared_ptr<const CallableIndex>; // 照会が保つ名前表の所有
		using LoopExitTable = std::unordered_map<NodeId, LoopExitIdx>; // 制御フローの所有者毎の脱出インデックス

		// 終端の控えと名前の表の型
		struct TerminationSummary {
			uint8_t Modes = 0; // 終端の成立する仮定
			uint8_t Effects = 0; // 返戻・送出・プロセス終了の可能な効果
			bool HasConditionalAccess = false; // 後続の呼出・添字を短絡し得る参照か
			bool HasUnknownEvaluation = false; // 評価時機を決める型・外部契約が不足するか
			bool IsAbrupt = false; // ジャンプを含め，此の文の直後へ通常完了しないか
			NodeId TransferSource = NoNode; // 元のジャンプを上書きし，外側へのジャンプを供給する後始末の本体
		};

		// １つの本体で再使用する終端の作業表
		struct TerminationResults {
			struct Entry {
				NodeId Node;
				NodeId Scope;
				TerminationSummary Summary;
			};

			std::vector<NodeId> Positions; // 最初の判定範囲の結果への直接参照
			std::vector<Entry> Entries; // 使用したノードだけの連続した結果
			std::unordered_map<uint64_t, TerminationSummary> Additional; // 同じノードを別の判定範囲で読む場合だけの結果
			const TerminationSummary *Find(const NodeId Node, const NodeId Scope) const; // ノードと判定範囲の結果の取得関数
			void Store(const NodeId Node, const NodeId Scope, const TerminationSummary Summary); // 判定結果の登録関数
			void Clear(const SyntaxModel &Syntax); // 使用した位置だけを空にして容量を保つ関数
		};

		// 脱出文と其のジャンプ先が届く最も外側の深さ
		struct ExitEvent {
			NodeId Node = NoNode; // 脱出文
			NodeId Dst = NoNode; // goto の投影済のジャンプ先
			NodeId Target = NoNode; // 脱出の解決済のジャンプ先
			uint32_t Depth = 0; // 脱出が届く最も外側のノードの深さ
			bool IsGoto = false; // goto に依る脱出か
			bool IsContinuation = false; // ジャンプ先の繰返を続けるか
		};

		// 対象部分木内の脱出を判定範囲毎に再利用するインデックス
		struct LoopExitIdx {
			// 同じ走査と照会で読むノード毎の事実
			struct NodeRecord {
				NodeId Node; // 事実の対象
				uint32_t Depth; // 根からの深さ
				std::optional<std::pair<NodeId, uint32_t>> Minimum; // 最初の判定範囲と其の最小脱出深さ
				size_t Event = std::numeric_limits<size_t>::max(); // 脱出事象の位置（無ければ最大値）
				NodeId Scope = NoNode; // 後始末の要約を持つ例外処理の最内の切替
				bool HasTry = false; // 後始末の要約を読む例外処理か
				std::optional<uint32_t> PlainMin = UINT32_MAX; // 後始末・未確定のジャンプを含まない部分木の最小脱出深さ
			};

			std::pmr::monotonic_buffer_resource Storage; // 此の所有範囲のインデックスと作業列の共有メモリ
			std::pmr::vector<NodeRecord> Nodes { &Storage }; // 訪問したノードだけの連続した事実
			std::pmr::unordered_map<NodeId, NodeId> SwitchParents { &Storage }; // 切替から其の外側の切替への参照
			std::pmr::vector<ExitEvent> Events { &Storage }; // 脱出文毎のジャンプ先
			std::pmr::unordered_map<uint64_t, uint32_t> AdditionalMin { &Storage }; // 同じノードを別の判定範囲で読む場合だけの最小脱出深さ
			std::pmr::vector<std::pair<NodeId, bool>> Pending { &Storage }; // 脱出要約の照会で再使用する後行の作業列
			size_t Generation = 0; // 再構築で以前の直接参照を区別する世代
			NodeId Root = NoNode; // インデックスの根
		};

		// 走査中の祖先に在る脱出可能な文
		// 無名の脱出と継続で数える祖先
		struct ExitPath {
			std::vector<NodeId> BreakTargets;
			std::vector<NodeId> ContinueTargets;
		};

		// 分岐文の枝
		struct BranchParts {
			NodeId Then = NoNode; // 真枝
			std::vector<NodeId> Alternatives; // 後続の枝の本体
			bool HasElse = false; // 条件を問わない最後の枝を持つか
		};

		// 例外処理文の節の本体
		struct TryParts {
			NodeId Body = NoNode; // 例外処理の本体
			NodeId Else = NoNode; // 例外の無かった時の節
			NodeId Finally = NoNode; // 後始末の節
			NodeId Operator = NoNode; // 文の本体を持たない試行の演算子
			std::vector<NodeId> Entries; // 捕捉範囲に含まれる入口の評価
			std::vector<NodeId> Handlers; // 捕捉の節
		};

		// 切替の節の本文
		struct CaseBody {
			std::vector<NodeId> Stmts; // 本文を開く記号より後の文（素通りの文を含む）
			bool IsOpened = false; // 本文を開く記号を持つか
			bool IsArrow = false; // 本文を開く記号が矢印か
		};

		// 切替の節と構造の確定状態
		struct SwitchParts {
			NodeId Body = NoNode;
			std::vector<NodeId> Cases;
			bool IsUnknown = false;
		};

		// １回の終端走査で共有する前提
		struct TerminationCtx {
			NodeId Function; // 終端を判定する関数
			bool DoesUseFunction = false; // 定数条件の判断が関数の可視名に依存し得るか
		};

		// 判定するノードと判定範囲と，其の子を積み終えたか
		struct TerminationFrame {
			NodeId Node;
			NodeId Scope;
			bool IsExpanded; // 子の評価を済ませた枠か
			TerminationRule Rule = TerminationRule::None;
			NodeId LoopBody = NoNode;
			uint8_t LoopModes = 0;
			uint8_t Effects = 0;
			NodeId OperandScope = NoNode;
			size_t OperandBegin = 0;
			size_t OperandEnd = 0; // 共有する評価対象の範囲
			NodeId InvocationBody = NoNode; // 子の展開と合成で共有する呼出時機の本体
		};

		// 連続する本体の照会で共有し，別の本体へ進む時に再使用する作業表
		struct TerminationMemo {
			NodeId Body = NoNode;
			NodeId Function = NoNode;
			bool DoesUseFunction = false;
			TerminationResults Known;
			std::vector<TerminationFrame> Work; // 本体間で再使用する子の展開の作業列
			std::vector<NodeId> Operands; // 作業枠が範囲で引く評価対象の列
		};

		// 親の子列から１度求める評価位置の条件
		struct EvaluationSelector {
			bool IsEnabled = true; // 兄弟・語・フィールドの条件が成立するか
			NodeId Selected = NoNode; // 語の直後又は名前付の位置で選ぶ子
		};

		static constexpr uint8_t ProcessExitEffect = 1; // 捕捉と後始末を実行せずプロセスを終える効果
		static constexpr uint8_t ReturnEffect = 2; // 関数から戻る効果
		static constexpr uint8_t SystemExitEffect = 4; // 標準の終了例外を送出する効果
		static constexpr uint8_t ThrowEffect = 8; // 型を確定出来ない例外を送出し得る効果
		static constexpr uint8_t TransferEffect = 16; // 関数以外へ制御を移す効果
		static constexpr uint8_t UnknownExitEffect = 32; // 送出かプロセス終了かを確定出来ない非復帰の契約
		static constexpr uint8_t UnknownCallEffect = 64; // 入力の束縛から呼出先を確定出来ない状態（効果との合成前に除く）
		static constexpr uint8_t CertainTermination = 1; // 仮定無で終端する位
		static constexpr uint8_t CoveredTermination = 2; // 切替の被覆を仮定して終端する位
		static constexpr uint8_t PessimisticTermination = 4; // 未確定の条件・呼出を終端と仮定する位
		static constexpr uint8_t AllTermination = 7; // 全ての判定で終端する位
		mutable std::set<std::string, std::less<>> TerminalMemberNames; // 標準終端名とメンバ代入の末尾名の保守的な集合
		mutable bool HasTerminalMemberNames = false; // 初回の全体収集を済ませたか
		mutable std::vector<NodeId> ChangedCallNames; // 次の照会前に代入名を追加する変更部分木
		mutable NodeId SimpleBody = NoNode; // 最後に単純な通常完了を照合した本体
		mutable std::optional<EndReachability> SimpleReach; // 単純な通常完了又は末尾の返戻（其の他は無）
		mutable std::vector<std::optional<TerminationSummary>> MacroArguments; // トークン列毎に共有する実引数の要約
		mutable std::unordered_map<NodeId, std::unordered_set<NodeId>> MacroBindingScopes; // 可視束縛の範囲毎に失効する実引数の位置
		mutable std::unordered_set<NodeId> BoundMacroArguments; // 入子のマクロの束縛に依存する要約の位置
		mutable std::vector<std::optional<bool>> GeneratorBodies; // 本体毎のジェネレータ・コルーチンのマーカーの有無
		mutable std::vector<std::optional<bool>> ValueReturns; // 本体毎の値付の返戻文の有無
		mutable std::vector<std::optional<bool>> NoReturnDeclarations; // 宣言毎の非復帰の指定の有無（宣言の子構造・テキストに依存）
		mutable std::vector<std::unique_ptr<GotoIdx>> GotoTargets; // 本体毎のジャンプ先（配列の再確保でも参照を保つ）
		mutable std::unordered_map<NodeId, BranchParts> BranchStructures; // 展開と合成で共有する枝
		mutable std::unordered_map<NodeId, TryParts> TryStructures; // 展開と合成で共有する例外処理の節
		mutable std::unordered_map<NodeId, SwitchParts> SwitchStructures; // 展開と合成で共有する切替の節
		mutable std::unordered_map<NodeId, CaseBody> CaseStructures; // 展開と合成で共有する節の本文
		mutable TerminationMemo Terminations; // 同じ本体・判定範囲の終端要約
		mutable std::vector<ChildEvaluation> ChildEvaluations; // 親の入力契約から求めた直接の子の評価時機
		mutable std::vector<std::optional<NodeId>> FlowOwners {}; // 変更部分木だけを失効する制御フローの所属
		mutable std::vector<std::pair<size_t, size_t>> LoopExitPositions {}; // ノード番号から所有者の世代と連続記録の位置への直接参照
		mutable size_t LoopExitGeneration = 0; // 脱出インデックスの最新の世代
		mutable LoopExitTable LoopExits {}; // 此の所有者が保持する関数毎の脱出インデックス
		mutable std::vector<std::pair<NodeId, bool>> CalleeWork; // 呼出先の要約の間で再使用する展開の作業列
		mutable std::vector<std::optional<std::vector<EvaluationSelector>>> EvaluationSelectors; // 親毎の評価位置の条件
		// 判定する構文と規則の評価の名前の控え
		const SyntaxModel &Syntax; // 判定する構文
		static bool IsKnownAttribute(const SyntaxModel &Syntax, NodeId Node); // 登録済の通常属性かの判定関数
		static uint64_t TerminationKey(const NodeId Node, const NodeId Scope); // 終端の判定結果のキーの生成関数
		// 標準型と終端の控えの共通操作
		static std::optional<Word> StandardResultType(const WordSet &Meanings); // 標準の戻値型の限定名の照合関数
		static TerminationSummary GetKnownSummary(const TerminationResults &Known, const NodeId Node, const NodeId Scope); // 判定済の終端と効果の取得関数
		static uint8_t GetKnownTermination(const TerminationResults &Known, const NodeId Node, const NodeId Scope); // 判定済の文の終端の取得関数
		static bool IsTransparentExpr(const SyntaxModel &Syntax, const NodeId Node); // 値を変えない単一の式の文又は括弧かの判定関数
		static NodeId GetUnwrappedExpr(const SyntaxModel &Syntax, NodeId Node); // 単一の式の文と透過的な括弧の中の値の取得関数
		NodeId GetExitLabel(const NodeId Node) const; // 脱出文のラベルの取得関数
		NodeId GetScopeOf(const NodeId Decl) const; // 宣言の有効範囲の取得関数
		NodeId GetFirstChildNode(const NodeId Node) const; // 先頭の子の取得関数
		std::pair<NodeId, bool> ResolveExitTarget(const ExitPath &Path, const NodeId Node, const bool IsContinuation) const; // 脱出文のジャンプ先の解決関数
		void CollectImportedPaths(const NodeId Import, const std::function<bool(NodeId)> &IsCustom, CallableNames &Names) const; // 明示的なインポートの末尾名又は別名の収集関数
		NodeId GetFlowOwner(const NodeId Node) const; // 制御の流れの所有者の取得関数
		bool IsArmSwitch(const NodeId Type) const; // 分岐を全て列挙する切替かの判定関数
		size_t GetLoopExitPosition(const LoopExitIdx &Idx, const NodeId Node) const; // 同じ所有者の訪問済の位置の取得関数（対象外は最大値）
		std::optional<uint32_t> GetLoopExitMinimum(const LoopExitIdx &Idx, const size_t Position, const NodeId Scope) const; // 部分木の判定範囲毎の最小脱出深さの取得関数
		void StoreLoopExitMinimum(LoopExitIdx &Idx, const size_t Position, const NodeId Scope, const uint32_t Depth) const; // 判定済の最小脱出深さの登録関数
		void BuildLoopExitIdx(const GotoIdx &Gotos, const NodeId Root, LoopExitIdx &Idx) const; // 脱出のインデックスの構築関数
		bool IsFallthroughStmt(const NodeId Node) const; // 次の節へ進む文かの判定関数
		bool IsIrrefutablePattern(const NodeId Pattern) const; // 全ての値に一致する照合かの判定関数
		bool IsNoReturnDeclared(const NodeId Node, const NodeId Name = NoNode) const; // 戻らない事を宣言した C / C++ の関数かの判定関数
		std::vector<NodeId> GetAssignedTargets(const NodeId Target, const bool ShouldCollectNames = false) const; // 分解した代入先のメンバ参照と指定時の名前の取得関数
		NodeId GetResultTypeOf(const NodeId Node) const; // 関数の戻値の型の取得関数
		void CollectBindingSource(const NodeId Scope, const NodeId Name, const NodeId Site, CallableNames &Names) const; // 中核の導入から呼出の供給元を登録する関数
		void CollectBindingChanges(const NodeId Scope, const NodeId Node, CallableNames &Names) const; // 中核の走査と同じ訪問から代入の供給元を登録する関数
		bool IsCallableBefore(const NodeId Left, const NodeId Right, bool &HasOrder) const; // 同じ照会の採番確認を共有する導入位置の比較関数
		CallableNameOwner GetCallableNamesOf(const NodeId Scope) const; // 呼出先を隠し得る直下の導入名の取得関数
		void ForgetMacroBindings(const NodeId Node) const; // 実引数の束縛範囲への登録の解除関数
		void InvalidateAliases(const NodeId Scope) const; // 変更した有効範囲に依存する供給値の保持の終了関数
		void InvalidateCallableScope(const NodeId Scope) const; // 導入表と其の結果を継承する名前の照会の失効関数
		bool HasCallableMember(const CalleeSummary &Summary) const; // 呼出結果への明示的な代入の候補の有無
		NodeId GetCallTarget(const NodeId Node) const; // 呼出先のフィールドから冗長括弧を除いて取得する関数
		bool CanSkipMemberResolution(const NodeId Function) const; // 終端先へ解決され得ないメンバ名かの判定関数
		std::optional<EndReachability> GetSimpleEndReach(const NodeId Body) const; // 単純な文と式の並びと末尾の返戻だけの到達可能性の取得関数
		CalleeSummary GetCalleeSummary(const NodeId Node) const; // 呼出先の先頭名と呼出の有無の共有取得関数
		ConstantTruth GetCondTruth(NodeId &Cond) const; // 条件のラッパーを１度辿り，確定したリテラルの真偽を取得する関数
		bool CanBeConstantCond(const NodeId Cond) const; // 定数式と評価され得る条件かの判定関数
		const BranchParts &GetBranchesOf(const NodeId Node) const; // 分岐文の枝の取得関数

		bool HasLoopExit(
			const GotoIdx &PrevGotos,
			const TerminationResults &Known,
			const NodeId Body,
			const NodeId Loop,
			const NodeId TransferScope = NoNode,
			const bool ShouldIncludeContinue = false
		) const; // 対象繰返からの脱出有無判定関数

		NodeId GetHandlerBody(const NodeId Handler) const; // 捕捉節又は捕捉式の実行部分の取得関数
		EvaluationClass GetChildEvaluation(const NodeId Node, const NodeId Child) const; // 子の評価時機の取得関数
		const TryParts &GetTryPartsOf(const NodeId Node) const; // 例外処理文の節の取得関数
		const SwitchParts &GetSwitchCases(const NodeId Node) const; // switch 文の節の取得関数
		const CaseBody &GetCaseBodyOf(const NodeId Case) const; // 切替の節の本文の取得関数
		bool IsDefaultCase(const NodeId Case) const; // 不一致の値を受ける切替の節かの判定関数
		bool IsExhaustivenessRequired(const NodeId Case) const; // 処理系が網羅を検査する照合の切替の節かの判定関数

		bool HasShadowingDecl(
			const NodeId Name,
			const bool IsGlobal,
			const bool IsQualified,
			const std::optional<Word> TypeMeaning = std::nullopt
		) const; // 標準の呼出・戻値型の名前を隠す可視宣言の判定関数

		uint8_t GetCondTermination(TerminationCtx &Ctx, const NodeId Cond) const; // 条件が反復を継続する仮定の取得関数
		NodeId GetFallthroughEntry(const NodeId Node, const NodeId Scope) const; // 次の枝へ進む語の属する切替の節の取得関数

		uint8_t SeqTermination(
			const GotoIdx &Gotos,
			const TerminationResults &Known,
			const std::span<const NodeId> Stmts,
			const NodeId Scope
		) const; // 文の並びの終端判定関数

		std::vector<NodeId> GetStmtsIn(const NodeId Container) const; // 文を直下に並べるノードの文の取得関数
		NodeId GetSingleBodyOf(const NodeId Node) const; // 本体を１度だけ実行する文の本体の取得関数
		std::vector<NodeId> GetPreprocStmts(const NodeId Node) const; // 前処理分岐の文の取得関数

		uint8_t BranchTermination(
			const GotoIdx &Gotos,
			const TerminationResults &Known,
			const NodeId Node,
			const NodeId TransferScope,
			bool &IsAbrupt
		) const; // 分岐文の全枝終端判定関数

		uint8_t SwitchTermination(
			const GotoIdx &Gotos,
			const TerminationResults &Known,
			const NodeId Node,
			const NodeId TransferScope
		) const; // switch 文の全分岐終端判定関数

		bool CanExcludeRole(const Role Desired) const; // 現在の役割インデックスから不在を確定出来るかの判定関数
		bool HasValReturn(const NodeId Body) const; // 同じ関数に属する値付の返戻文の有無の判定関数
		bool HasPtrOrRefReturn(const NodeId Function) const; // C / C++ のポインタ・参照を返す関数かの判定関数
		bool HasExprBody(const NodeId Node) const; // 式を本体とする関数かの判定関数
		bool IsPlainIntType(const NodeId Type) const; // マクロ置換の無い int 型の判定関数
		bool IsProgramEntry(const NodeId Node, const NodeId Type) const; // C / C++ のエントリーポイントかの判定関数

		ReturnShape GetBodyReturnShapeOf(
			const NodeId Node,
			const bool IsAsync,
			const bool IsNeverOnly = false,
			const NodeId Name = NoNode
		) const; // 本体の返戻区分の判定関数（非復帰だけの照会では其の他の区分を省ける）

		NodeId GetCallableGuard(const CallableIndex &Index, const CallableIntroduction &Binding) const; // 導入名の最内の条件付の実行範囲を共有する取得関数
		uint32_t GetCallHeadNameIdx(const NodeId Head, const std::string &HeadName) const; // 呼出とメンバ代入が共有する先頭名の比較番号の取得関数
		WordSet GetMacroMeanings(const NodeId Macro, bool *HasSameBinding = nullptr) const; // 現在の束縛に対応する投影済のマクロの役割の取得関数
		uint8_t GetCallEffects(const NodeId Node, bool &IsRegisteredMacro) const; // 戻らない事を言語・標準ライブラリが定める呼出かの判定関数
		std::optional<bool> MatchesException(const NodeId Handler, const NodeId Body, const uint8_t Effect) const; // 送出の効果と捕捉条件の一致の判定関数

		TerminationSummary SummarizeEntry(
			const TerminationResults &Known,
			const NodeId Node,
			const NodeId Scope,
			const std::span<const NodeId> Operands,
			const NodeId Excluded
		) const; // 共有契約に従う入口の必須評価の合成関数

		TerminationSummary TryTermination(const TerminationResults &Known, const NodeId Node, const NodeId TransferScope) const; // 例外処理文の終端判定関数
		bool IsPostCondLoop(const NodeId Node, const NodeId Body) const; // 本体を１度は実行する繰返かの判定関数
		uint8_t LoopTermination(TerminationCtx &Ctx, const NodeId Node, NodeId &Body) const; // 条件で終わらない繰返かの判定関数
		// 構文の部位の取得と終端の内部判定
		NodeId GetEvaluationCondition(const NodeId Node) const; // 投影した制御構文の条件又は対象の取得関数

		uint8_t NodeTermination(
			const GotoIdx &Gotos,
			const TerminationResults &Known,
			const NodeId Node,
			const NodeId Scope,
			const std::span<const NodeId> Operands,
			const TerminationRule Rule,
			const NodeId LoopBody,
			const uint8_t LoopModes,
			bool &IsAbrupt
		) const; // 判定済の子からの終端判定関数

		TerminationRule GetTerminationRuleOf(
			TerminationCtx &Ctx,
			const NodeId Node,
			const NodeId Scope,
			NodeId &LoopBody,
			uint8_t &LoopModes,
			uint8_t &Effects
		) const; // 終端の判定規則の取得関数

		NodeId GetInvocationBody(const NodeId Node) const; // 呼出時機の照会対象の本体の取得関数

		void TerminationOperands(
			const NodeId Node,
			const TerminationRule Rule,
			const NodeId LoopBody,
			std::vector<NodeId> &Output
		) const; // 終端の判定が読む子の収集関数

		TerminationSummary GetMacroArguments(const NodeId Node) const; // 未展開の実引数の確定出来る評価と制御移動の要約関数

		TerminationSummary SummarizeTermination(
			const GotoIdx &Gotos,
			const TerminationResults &Known,
			const NodeId Node,
			const NodeId Scope,
			const std::span<const NodeId> Operands,
			const NodeId OperandScope,
			const TerminationRule Rule,
			const NodeId LoopBody,
			const NodeId InvocationBody,
			const uint8_t LoopModes,
			const uint8_t Effects
		) const; // 子の終端と送出の効果の合成関数

		bool HasGeneratorIn(const NodeId Node) const; // 同じ関数に属するジェネレータのマーカーの有無の判定関数
		void InvalidateBodyFacts(const NodeId Node) const; // 対象と祖先のトークン・返戻・ジャンプ先の要約の失効関数
		void InvalidateCallees(const NodeId Node) const; // 対象と祖先の呼出先の構造の保持の終了関数
		void Invalidate(const NodeId Node, const bool IsStructure, const bool HasBindingChanges = true) const; // 書換の影響する制御の流れと名前の保持の終了関数
		void ClearTerminationStructures() const; // 抽出済の制御構造の保持の終了関数

		const TerminationResults &EvaluateTermination(
			const GotoIdx &Gotos,
			const std::span<const NodeId> Roots,
			const NodeId Scope,
			const NodeId Function,
			const bool ShouldStopAtTermination = false
		) const; // 終端の判定結果の構築関数

		bool IsOrdinaryFunction(const NodeId Type) const; // 通常関数のノードかの判定関数
		bool IsGeneratorLike(const NodeId Node, const NodeId Stmts) const; // ジェネレータ・コルーチンかの判定関数
		bool IsPlaceholderBody(const NodeId Stmts) const; // 中身を持たない事を表す本体かの判定関数
		bool HasUnexpandedBodySyntax(const NodeId Stmts) const; // 本体に未展開のマクロ・属性が在るかの判定関数
		bool CanCreateTailTemps(const NodeId Tail) const; // 末尾式が文の終わり迄生きる一時値を作るかの判定関数

	public:

		// 返戻と展開前の属性の共通判定
		static NodeId GetDocstring(const SyntaxModel &Syntax, const NodeId Node, std::unordered_map<NodeId, NodeId> &Values); // 本体の先頭の Python のドキュメンテーション文字列の取得関数
		static bool IsReturn(const SyntaxModel &Syntax, const NodeId Node); // 関数から戻る文・式かの判定関数
		static bool HasUnexpandedAttribute(const SyntaxModel &Syntax, NodeId Node, std::unordered_map<NodeId, bool> &Cache); // Rust の展開前の属性の範囲かの判定関数
		NodeId UnwrapCallTarget(NodeId Node) const; // 呼出先と別名の参照先を包む括弧の除去関数
		// 制御フローと返戻の取得・判定
		bool IsFlowBoundary(const NodeId Type) const; // 独立した制御フロー境界の判定関数
		bool HasGotoEntry(const GotoIdx &Gotos, const NodeId Node) const; // ノード内への goto 入口有無判定関数
		bool HasReturnInFlow(const NodeId Node) const; // 同じ制御フロー内の返戻文包含判定関数
		NodeId GetStmtsOf(const NodeId Node) const; // 関数の文の並びの取得関数
		ReturnShape GetReturnShapeOf(const NodeId Node) const; // 関数の戻値の区分の判定関数
		const GotoIdx &CollectGotoTargets(const NodeId Body) const; // 関数内 goto ジャンプ先収集関数
		NodeId GetTailExpr(const NodeId Last) const; // ブロックの値を与える末尾式の取得関数
		bool IsSingleImplicitReturnBody(const NodeId Function, const FunctionEnd &End) const; // 暗黙返戻の候補と為る単一式の本体かの判定関数
		std::vector<NodeId> GetReturningStmtsAfterTermination(const NodeId Container, const GotoIdx &Gotos) const; // 終端した後の返戻文を含む文の取得関数
		NodeId GetNextDeclarator(const NodeId Node) const; // C / C++ の次の宣言子の取得関数

		std::optional<FunctionEnd> ClassifyFunction(
			const NodeId Node,
			const std::optional<ReturnShape> &KnownShape = std::nullopt,
			const bool NeedsCompletion = false
		) const; // 通常関数の本体の要約関数（補完専用の照会は末尾の明示返戻を除く）

		EndReachability EndReach(const NodeId Function, const FunctionEnd &End) const; // 通常関数の末尾の到達可能性の判定関数
		bool CanExcludeResultBuilder(const NodeId Function) const; // 関数へのリザルトビルダーの適用を除外出来るかの判定関数
		bool CanMakeImplicitReturnExplicit(const NodeId Function, const FunctionEnd &End, const bool HasSingleImplicitResult) const; // 暗黙の単一式を明示返戻へ変えれるかの判定関数

		bool IsReturnCompletionUncertain(
			const NodeId Function,
			const FunctionEnd &End,
			const bool HasSingleImplicitResult,
			const bool CanMakeExplicit,
			std::unordered_map<NodeId, bool> &Cache
		) const; // 末尾の返戻の補完を構文から確かめれないかの判定関数

		ControlFlow(const SyntaxModel &Syntax); // コンストラクタ
	};
} // namespace Core
