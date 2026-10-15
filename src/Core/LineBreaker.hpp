#pragma once

#include "FormatIR.hpp"
#include "NodeId.hpp"
#include "../Util/TextMetrics.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Core {
	class CoreObservation;

	// 行幅に依る選択を全て確定するクラス（整形仕様 3.1，6.4，行分割仕様）
	class LineBreaker {
	private:

		friend struct LineBreakerMeasurement; // テスト用の計数観測

		using BoundaryLists = std::pmr::vector<std::pmr::vector<size_t>>; // 同じ解決の共有メモリへ置くグループ毎の境界列
		// 不変トークンの行幅要約
		using LexemeSummary = TextMetrics::LineSummary;

		enum class AtomKind : uint8_t {
			Text,
			AnchorBegin,
			AnchorEnd,
			Blank,
			EdgeSpace,
			Hard,
			IndentBegin,
			IndentEnd,
			OwnedRegion,
			Separator,
			Soft,
			Verbatim
		}; // 線形化した要素の種類

		enum class GroupState : uint8_t { Pending, Broken, Flat }; // グループの状態（未評価・展開・平坦）

		// 宣言区分と親の判定が共有する平坦形
		// 先頭行・末尾行・最も幅の広い行の文字数
		struct FlatSummary {
			size_t Head = 0;
			size_t Tail = 0;
			size_t Max = 0;
			bool HasBreak = false; // 必須境界の有無
			bool HasNewline = false; // 幅要約に含む物理改行の有無
		};

		// 線形化した要素（測定区間の走査の単位）
		struct Atom {
			AtomKind Kind = AtomKind::Text; // 要素の種類
			GeneratedToken Generated = GeneratedToken::None; // 規則から生成したトークンの種類
			bool HasNewline : 1; // 逐語テキストに改行が在るか
			bool HasBlankNewline : 1; // 空行の比較で数える逐語の改行か
			bool HasCodeNewline : 1; // 逐語テキストの改行が展開の根拠に為るか
			bool IsActivatedWhenFlat : 1; // 条件付断片が平坦時だけ有効か
			// 先頭行と末尾行の内容の有無
			bool HasHeadContent : 1;
			bool HasTailContent : 1;
			bool HasVarWidth : 1; // 可変区間の幅インデックスに属するか
			FormatRange Text; // テキストの範囲（選択可能な境界は平坦時のテキスト）
			uint32_t WidthPos = 0; // 所属する測定区間の幅インデックス位置
			uint32_t Width = 0; // 最初の行の文字数（選択可能な境界は平坦時の文字数）
			uint32_t TailWidth = 0; // 逐語の最後の行の文字数（条件付の区切空白は０）
			uint32_t Group = NoGroup; // 選択可能な境界の所属するグループ
			uint32_t Cond = NoGroup; // 条件付断片の採否を決めるグループ
			NodeId Node = NoNode; // アンカーのノード（トークンの葉と逐語領域は其の原子自身がアンカーを兼ね，空行の規則は前の宣言）
			uint32_t Detail = NoDetail; // 元の要素の属性の番号（句読点の所属・コピーした共有部分の出現・空行の規則）

			Atom(); // コンストラクタ（未測定の原子のフラグを初期化する）
		};

		// 測定区間の境界の数の区間和（行分割仕様 3.2 の要約，１度構築し，グループの評価毎に候補の間を走査しない）
		struct BoundaryCounter {
			std::vector<uint32_t> Tree; // 境界の数のフェンウィック木（添字は要素の位置と１の和）
			std::vector<uint8_t> IsCounted; // 境界として数える要素は１，其の他は０
			uint32_t CountBefore(const size_t End) const; // 位置より前の境界の数の取得関数
			size_t Select(const uint32_t Rank) const; // 指定した順位の境界の位置の取得関数

			explicit BoundaryCounter(const size_t Count); // コンストラクタ（境界の格納領域）
		};

		// 行分割の作業単位の計上（整形仕様 9.2，評価の合計を入力の基準量から定めた上限で打ち切る）
		struct WorkMeter {
			// 継続の同じ測定区間へ属する候補範囲
			struct ContinuationRange {
				uint32_t Rank; // 除去する候補を除いた境界の順位
				size_t Begin; // 試行差分の始端
				size_t End; // 試行差分の排他的な終端
			};

			// 平坦形の共有要約を集める後行走査の作業枠
			struct FlatFrame {
				FormatItemId Id; // 測定する要素
				FormatRange Children {}; // 元の子列の範囲
				std::vector<FormatItemId> DeclParts {}; // 分割宣言の展開した部品列
				uint32_t Next = 0; // 次に読む子の位置
				FlatSummary Summary {}; // 完了した子から合成した要約
				bool HasEntered = false; // 要素の計数を開始したか
				bool IsReused = false; // 登録済の要約を引いたか
			};

			// 要約の表と共有メモリを１つの所有者で移し，観測結果へ渡した後も寿命を保つ
			struct SummaryCache {
				using LexemeBlock = std::array<size_t, 512>; // 開始位置から幅要約へ引く枠（０は未計数，番号は１始まり）

				// 宣言区分の確定後に手放せる平坦形の表（コンテナの番兵も其のメモリに在る為，表をメモリより先に破棄する様に１つの所有者へ纏める）
				struct FlatTable {
					std::pmr::monotonic_buffer_resource Storage; // 平坦形の表のメモリ
					std::pmr::unordered_map<FormatItemId, FlatSummary> Items { &Storage }; // 必要な平坦形の要約
				};

				std::pmr::monotonic_buffer_resource Storage; // 同じ開始位置で長さが異なるトークンの表のメモリ
				std::optional<FlatTable> Flat { std::in_place }; // 宣言区分の確定迄の平坦形の表
				std::vector<std::pair<uint32_t, LexemeSummary>> Lexemes; // 範囲の長さと幅要約（開始位置の枠と長さが異なる範囲の表から共有する）
				std::vector<std::unique_ptr<LexemeBlock>> LexemeStarts; // 初回に計数する開始位置の周囲だけ枠を確保する
				std::pmr::unordered_map<uint64_t, size_t> OtherLexemes { &Storage }; // 同じ開始位置で長さが異なる範囲の番号
			};

			size_t Used = 0; // 使った作業単位
			size_t Limit = 0; // 上限
			size_t GroupNodeVisits = 0; // 観測時のグループとノードの組の訪問数
			size_t DeclGroupVisits = 0; // 観測時の宣言のコピー用番号を求めるグループの訪問数
			size_t Evaluations = 0; // グループの幅を実際に評価した回数
			size_t LexemeBytes = 0; // トークン計数で読んだバイト数
			size_t SummaryCompositions = 0; // 区間要約の合成数
			size_t IdxUpdates = 0; // 採用差分のインデックス更新数
			size_t DecisionSummaries = 0; // 決定単位内の候補要約生成数
			size_t DependencyRegistrations = 0; // 中間表現から受け取った直接依存数
			size_t DependencyNotifications = 0; // 完了した直接依存の通知数
			size_t BoundaryEntries = 0; // グループへ登録した候補と条件付断片の位置の数
			size_t AtomPreparationVisits = 0; // 番号と幅の準備で訪れた原子数
			size_t PrefixEntries = 0; // 固定区間の累積値の構築数
			size_t BoundaryVisits = 0; // 固定区間の候補境界の訪問数
			size_t BoundaryUnlinks = 0; // 固定区間の境界の切離数
			// 同時に１つだけ行う試行の位置列（試行終了後に容量を再使用する）
			std::vector<size_t> TrialPositions;
			std::vector<size_t> TrialMerged;
			std::vector<size_t> TrialRemoved;
			std::vector<size_t> TrialAdded;
			// 行端の幅差分も，測定区間毎に内容を空にして容量を再使用する
			std::vector<size_t> WidthRemoved;
			std::vector<size_t> WidthAdded;
			// 決定単位の測定が終わる度に，同じ候補列の容量を再使用する
			std::vector<size_t> DecisionHeaders;
			std::vector<size_t> DecisionHeaderConditions;
			std::vector<size_t> DecisionCandidates;
			std::vector<size_t> DecisionConditions;
			std::vector<size_t> DecisionAdopted;
			// 連鎖の接頭部の試行も，同じ決定呼出の間で容量を再使用する
			std::vector<uint32_t> BracePrefixGroups;
			std::vector<size_t> BracePrefixCandidates;
			std::vector<size_t> BracePrefixConditions;
			std::vector<FlatFrame> FlatPending; // 未登録の平坦形の測定毎に，同じ作業列の容量を再使用する
			std::vector<size_t> ZeroSegments; // 幅０で使う必須境界の区間番号
			std::vector<size_t> TextBreaks; // 展開の根拠に為らない改行を含む逐語の原子位置
			// 継続単位毎に内容を空にし，同じ解決呼出の間は容量を再使用する
			std::vector<uint32_t> ContentGroups;
			std::vector<uint32_t> AnnotationContent;
			std::vector<uint32_t> EnclosedGroups;
			std::vector<uint32_t> InnerFlattened;
			std::vector<size_t> InnerCandidates;
			// 継続の型注釈側と本体側の試行でも，容量を共有して再使用する
			std::vector<uint32_t> ContinuationFlattened;
			std::vector<size_t> OutsideCandidates;
			std::vector<size_t> ContinuationRun;
			std::vector<size_t> ContinuationChanges;
			std::vector<ContinuationRange> ContinuationRanges;
			std::vector<size_t> GroupFinalizations; // グループ毎の確定回数
			std::unique_ptr<SummaryCache> Summaries; // 初回の必要時に作り，同じ解決呼出で所有する要約とメモリ
			void Charge(const size_t Amount); // 作業単位の計上関数
			LexemeSummary Count(const std::string_view Text); // トークンの行幅要約を直接計数する関数
			LexemeSummary Measure(const FormatIR &IR, const FormatRange Range); // 不変トークンの１度の計数関数
		};

		// 線形化の途中の状態
		struct Linearizer {
			std::pmr::unsynchronized_pool_resource Storage { std::pmr::pool_options{ 64, 4096 } }; // 小さな候補列は高々６４ブロック毎に共有し，４ KiB を超える列は直接確保・解放するメモリ
			std::vector<Atom> Atoms; // 線形化した要素
			bool ShouldRecordOrder = false; // 宣言の区分で後行順を作り直すか
			std::vector<uint32_t> Closed; // 線形化したグループの後行順
			std::vector<uint32_t> Enclosing; // 開いて居るグループの構築時の番号
			std::vector<std::pair<uint32_t, uint32_t>> Children; // 区分後の出現毎の直接の親子関係（構築時のグループ番号）
			GroupRanks Order; // 構築時の番号から後行順の番号への対応（複数の要素が共有するグループは最後の要素の閉じの順）
			std::vector<SelectedForm> Forms; // 決定順位から全ての所属ノードと構文操作への対応
		};

		// 可視のトークンの幅と残存境界のインデックス（行分割仕様 3.2 の区間の幅要約，グループの評価毎に測定区間を走査しない）
		struct MeasureIdx {
			using IndexChange = int64_t; // 同じ更新内の添字毎に集約する採用差分

			std::vector<uint32_t> Positions; // 可変の測定区間の原子位置（固定区間には幅のインデックスを持たない）
			BoundaryCounter Boundaries; // 残存する境界
			BoundaryCounter Content; // 行内で正の幅を持つ可視要素
			// 行端表現と行内表現の幅の差
			std::vector<int64_t> EdgeTree; // 木のノード毎の区間和
			std::vector<int64_t> EdgeVals; // 各要素の現在の幅の差
			// 幅の区間和と各要素の現在の幅
			std::vector<size_t> Tree; // フェンウィック木（添字は圧縮位置と１の和）
			std::vector<uint32_t> Vals;
			// ４列の採用差分と反映位置の再使用列
			std::array<std::vector<int64_t>, 4> Changes; // 幅・境界・内容・行端差の順の採用差分
			std::array<std::vector<uint8_t>, 4> HasChanged; // 総和が０でも変更を記録したか
			std::array<std::vector<size_t>, 4> ChangedIndices; // 各列で反映する添字だけを持つ再使用列
			size_t GetLocalBefore(const size_t End) const; // 原子位置に対応する圧縮位置の取得関数
			uint32_t GetContentBefore(const size_t End) const; // 原子位置より前の内容数の取得関数
			size_t GetContentAt(const uint32_t Rank) const; // 内容の順位から原子位置を引く関数
			size_t GetWidthBefore(const size_t End) const; // 位置より前の可視トークンの幅の取得関数
			int64_t GetEdgeBefore(const size_t End) const; // 位置より前の行端差の取得関数

			size_t ComputeTrialWidth(
				const std::vector<uint8_t> &Conditions,
				const std::vector<Atom> &Atoms,
				const size_t Begin,
				const size_t End,
				const bool HasLeftContent,
				const bool HasRightContent,
				const std::span<const size_t> Positions,
				const uint32_t Evaluated,
				const std::span<const uint32_t> Flattened,
				const uint32_t Expanded,
				WorkMeter &Work
			) const; // 行端差と候補差分を読取専用で合成する関数

			void AddChange(const size_t Idx, const size_t Column, const int64_t Amount); // 採用差分を木の添字へ元の訪問順で合算する関数

			void Apply(
				const std::vector<GroupState> &States,
				const std::vector<uint8_t> &Conditions,
				const std::vector<Atom> &Atoms,
				const std::vector<size_t> &Positions,
				WorkMeter &Work
			); // 採用差分の一括更新関数

			MeasureIdx(
				const std::vector<GroupState> &States,
				const std::vector<uint8_t> &Conditions,
				const std::span<const uint8_t> NeedsWidth,
				std::vector<Atom> &Atoms
			); // コンストラクタ（可視のトークンの幅と残存境界を数える）
		};

		// 未確定の括弧外を平坦とする読取専用の境界差分
		struct BoundaryProjection {
			const BoundaryCounter &Src; // 採用済の境界インデックス
			std::vector<size_t> Removed; // 同じ試行で除く境界位置（昇順）
			uint32_t CountBefore(const size_t End) const; // 差分を除いた境界数の取得関数
			size_t Select(const uint32_t Rank, WorkMeter &Work) const; // 差分を除いた順位の境界の取得関数

			BoundaryProjection(const BoundaryCounter &Src, const std::span<const size_t> Candidates, WorkMeter &Work); // １度共有する境界差分の構築関数
		};

		// 幅で包む本体と，其の直前の見出に属する分割グループ
		struct BraceChoice {
			uint32_t Body = NoGroup; // 波括弧を選ぶ本体のグループ
			std::vector<uint32_t> Headers; // 本体の直前に在る見出のグループ（後行順）
			std::vector<uint32_t> Prefix; // 採否を変更せず測定へ含める外側の連鎖と見出のグループ（昇順）
		};

		// テキストと境界位置が固定された範囲の累積値と残存境界
		struct FixedMeasure {
			std::vector<uint32_t> Positions; // 固定の測定区間と其の両端の原子位置
			std::vector<size_t> Prefix; // 平坦時の正確な累積文字数
			std::vector<uint32_t> Prev; // 残る左の境界（先頭と終端は番兵）
			std::vector<uint32_t> Next; // 残る右の境界

			bool CanFlatten(
				const std::vector<uint8_t> &Conditions,
				const std::vector<Atom> &Atoms,
				const std::span<const size_t> Candidates,
				const size_t MaxChars,
				WorkMeter &Work
			) const; // 累積値の差に依る候補の判定関数

			void Remove(const std::vector<Atom> &Atoms, const std::span<const size_t> Candidates); // 採用した境界列の切離関数

			FixedMeasure(const std::vector<GroupState> &States, const std::span<const uint8_t> IsFixed, std::vector<Atom> &Atoms); // 固定区間の構築
		};

		const size_t MaxChars; // １行の最大文字数（構築時に１度だけ受け取り，０は無制限）

		static bool IsVisible(
			const std::vector<uint8_t> &Conditions,
			const Atom &Piece,
			const uint32_t Evaluated,
			const std::span<const uint32_t> Flattened = std::span<const uint32_t>(),
			const uint32_t Expanded = NoGroup
		); // 断片を測定区間へ含めるかの判定関数

		static bool IsBoundary(
			const std::vector<GroupState> &States,
			const Atom &Piece,
			const uint32_t Evaluated,
			const std::span<const uint32_t> Flattened = std::span<const uint32_t>()
		); // 測定区間の境界かの判定関数

		static std::span<const size_t> GetTextBreaks(
			const std::vector<uint8_t> &Conditions,
			const std::vector<Atom> &Atoms,
			const size_t First,
			const size_t Last,
			const uint32_t Evaluated,
			const std::span<const uint32_t> Flattened,
			const uint32_t Expanded,
			size_t &Left,
			size_t &Right,
			WorkMeter &Work
		); // 候補内の物理改行と測定する外側の物理境界の取得関数

		static bool HasWidth(const Atom &Piece); // テキスト又は平坦境界の幅を持つかの判定関数
		static FlatSummary JoinFlat(const FlatSummary &Left, const FlatSummary &Right); // 平坦要約の定数時間の合成関数

		static bool IsLineEdge(
			const std::vector<GroupState> &States,
			const std::vector<uint8_t> &Conditions,
			const std::vector<Atom> &Atoms,
			const size_t Pos,
			const int Direction,
			const uint32_t Evaluated,
			WorkMeter &Work,
			const std::span<const uint32_t> Flattened = std::span<const uint32_t>(),
			const uint32_t Expanded = NoGroup
		); // 表示上の空白が行端へ接するかの判定関数

		static FlatSummary GetFlatForm(const FormatIR &IR, const FormatItemId Id, WorkMeter &Work); // 平坦形の行幅要約の取得関数

		static FormatItemId SharedCopy(
			FormatIR &IR,
			const FormatItemId Id,
			const NodeId Decl,
			const uint32_t Row,
			std::unordered_map<uint32_t, uint32_t> &Groups,
			uint32_t &NextGroup,
			WorkMeter &Work
		); // 共有する部分の選択肢を保つコピーの取得関数

		static void MarkDeclOccurrence(FormatIR &IR, const FormatItemId Id, const NodeId Decl, const uint32_t Row, WorkMeter &Work); // 複写した共有部分の出力上の出現の記録関数

		static void CopyDecisions(
			FormatIR &IR,
			const std::unordered_map<uint32_t, uint32_t> &Groups,
			std::vector<std::pair<uint32_t, uint32_t>> &Connections,
			WorkMeter &Work
		); // コピーしたグループだけの決定単位の追加関数

		static void Linearize(
			const FormatIR &IR,
			Linearizer &State,
			const FormatItemId Id,
			const uint32_t Cond,
			WorkMeter &Work,
			CoreObservation *const Observation = nullptr
		); // 中間表現の線形化関数

		static void ReorderDecisions(FormatIR &IR, Linearizer &State, WorkMeter &Work, CoreObservation *const Observation = nullptr); // 線形化で得た順位への直接参照の対応関数

		bool CanFlatten(
			const std::vector<GroupState> &States,
			const std::vector<uint8_t> &Conditions,
			const std::vector<Atom> &Atoms,
			const std::span<const size_t> Candidates,
			const std::span<const size_t> Conditionals,
			const uint32_t Evaluated,
			const MeasureIdx *const Idx,
			WorkMeter &Work,
			const std::span<const uint32_t> Flattened = std::span<const uint32_t>(),
			const std::span<const size_t> Changed = std::span<const size_t>(),
			const BoundaryProjection *const Projection = nullptr,
			const uint32_t Expanded = NoGroup
		) const; // グループの全候補を除けるかの評価関数

		void PartitionDecl(
			FormatIR &IR,
			const FormatItemId Id,
			std::vector<DeclPartition> &Partitions,
			uint32_t &NextGroup,
			WorkMeter &Work
		) const; // １つの統合宣言の区分関数

		bool HasAppliedBraceBody(
			std::vector<GroupState> &States,
			std::vector<uint8_t> &Conditions,
			const std::vector<Atom> &Atoms,
			const BoundaryLists &Candidates,
			const BoundaryLists &ConditionalsOf,
			const BraceChoice &Choice,
			const bool IsBodyForced,
			MeasureIdx *const Idx,
			WorkMeter &Work
		) const; // 制御本体と見出の表現を同じ条件付トークンで評価する関数

		bool CanFlattenContinuation(
			const std::vector<GroupState> &States,
			const std::vector<uint8_t> &Conditions,
			const std::vector<Atom> &Atoms,
			const BoundaryLists &Candidates,
			const BoundaryLists &ConditionalsOf,
			const uint32_t Group,
			const uint32_t Prefix,
			const std::vector<uint32_t> &ContentGroups,
			const MeasureIdx *const Idx,
			const std::span<const size_t> Changed,
			WorkMeter &Work,
			const uint32_t Expanded = NoGroup,
			const bool IsContainer = false
		) const; // 継続括弧を省ける全測定区間の判定関数

		BrokenIR ResolveMeasured(FormatIR IR, WorkMeter *const Measured, CoreObservation *const Observation = nullptr) const; // 製品の選択とテスト用の計数を共有する解決関数

	public:

		BrokenIR Resolve(FormatIR IR, CoreObservation *const Observation = nullptr) const; // 選択の解決関数

		explicit LineBreaker(const size_t Chars); // コンストラクタ（行幅上限の設定）
	};
} // namespace Core
