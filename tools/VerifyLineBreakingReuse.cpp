#include "Core/Canonicalizer.hpp"
#include "Core/CodeLayout.hpp"
#include "Core/CoreObservation.hpp"
#include "Core/FormatIR.hpp"
#include "Core/LineBreaker.hpp"
#include "Core/SourceProjector.hpp"
#include "Core/SyntaxModel.hpp"
#include "Core/SyntaxNormalizer.hpp"
#include "Core/SyntaxRole.hpp"
#include "Profile/GrammarProfile.hpp"
#include "Util/FormatLimitExceeded.hpp"
#include "Util/InputNormalizer.hpp"
#include "Util/Lang.hpp"
#include "Util/LayoutTypes.hpp"
#include "Util/OutputFinalizer.hpp"
#include "Util/ParseAdapter.hpp"
#include "Util/ParsedTree.hpp"
#include "Util/SyntaxParser.hpp"
#include "Util/TextMetrics.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Core {
	// 製品の手順を変えず計数だけを観測するテスト用の入口
	struct LineBreakerMeasurement {
		using Meter = LineBreaker::WorkMeter;

		// 解決した中間表現と製品の計数の観測結果
		struct Result {
			BrokenIR Output;
			Meter Counts;
		};

		/**
		 * 棄却する候補の判定が共有インデックスを変更しない事の検査関数
		 * 計算量：固定要素数に対し O(1)
		 * @return 上限２の棄却でインデックスを保ち，同じ作業列を使う逆の採用で元へ戻れば true
		 * @throws FormatLimitExceeded 平坦化の評価が手で定めた作業予算を超える場合
		 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
		 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
		 */
		static bool IsTrialReadOnly() {
			// 棄却する候補の判定が共有インデックスを変更しない事の検査
			std::vector<LineBreaker::Atom> Atoms(3);
			for(LineBreaker::Atom &Piece : Atoms) Piece.Width = 1;
			Atoms[1].Kind = LineBreaker::AtomKind::Soft;
			Atoms[1].Group = 0;
			const std::vector<LineBreaker::GroupState> States(1, LineBreaker::GroupState::Pending);
			const std::vector<uint8_t> Conditions(1, 0);
			LineBreaker::MeasureIdx Idx(States, Conditions, std::vector<uint8_t>(Atoms.size(), 1), Atoms);
			const std::vector<size_t> WidthTree = Idx.Tree;
			const std::vector<int64_t> EdgeTree = Idx.EdgeTree, EdgeVals = Idx.EdgeVals;
			const std::vector<uint32_t> Content = Idx.Content.Tree;
			const std::vector<uint8_t> ContentFlags = Idx.Content.IsCounted;
			const std::vector<uint32_t> Widths = Idx.Vals, Boundaries = Idx.Boundaries.Tree;
			const std::vector<uint8_t> BoundaryFlags = Idx.Boundaries.IsCounted;
			const LineBreaker Breaker(2);
			constexpr size_t TrialWork = 16; // 評価１・境界４・幅要約１１の作業単位
			Meter Counts;
			Counts.Limit = TrialWork;
			const bool IsFlat =
			Breaker.CanFlatten(States, Conditions, Atoms, std::vector<size_t>{ 1 }, std::span<const size_t>(), 0, &Idx, Counts);
			// 同じ境界を含む非空の差分は併合し，重複を除いて同じ棄却を得る
			const std::vector<size_t> Changed { 1 };
			Meter DeltaCounts;
			DeltaCounts.Limit = TrialWork + 3; // 元の候補の複写１と併合する２位置の生成を加える
			const bool IsDeltaFlat = Breaker.CanFlatten(
				States,
				Conditions,
				Atoms,
				std::vector<size_t>{ 1 },
				std::span<const size_t>(),
				0,
				&Idx,
				DeltaCounts,
				std::span<const uint32_t>(),
				Changed
			);
			// 両経路の棄却・作業量と全インデックスの一致の控え
			const bool IsUnchanged =
			!IsFlat && !IsDeltaFlat && Counts.Used == TrialWork && DeltaCounts.Used == TrialWork + 3 && !Counts.IdxUpdates &&
			!Counts.TrialPositions.capacity() && !Counts.TrialMerged.capacity() && DeltaCounts.TrialPositions.size() == 1 &&
			DeltaCounts.TrialMerged.size() == 1 && !DeltaCounts.IdxUpdates && Idx.Tree == WidthTree && Idx.Vals == Widths &&
			Idx.Boundaries.Tree == Boundaries && Idx.Content.Tree == Content && Idx.Content.IsCounted == ContentFlags &&
			Idx.EdgeTree == EdgeTree && Idx.EdgeVals == EdgeVals && Idx.Boundaries.IsCounted == BoundaryFlags;
			// 同じ１位置の幅・内容・境界を除き，逆の採用で元へ戻して作業列の容量を照合する
			Meter Updates;
			Updates.Limit = 12; // 各採用は葉１・経路２・反映３の６単位
			Atoms[1].Width = 0;
			Idx.Apply(std::vector<LineBreaker::GroupState>{ LineBreaker::GroupState::Flat }, Conditions, Atoms, Changed, Updates);
			const LineBreaker::MeasureIdx::IndexChange *const WidthBuffer = Idx.Changes[0].data();
			const LineBreaker::MeasureIdx::IndexChange *const BoundaryBuffer = Idx.Changes[1].data();
			const LineBreaker::MeasureIdx::IndexChange *const ContentBuffer = Idx.Changes[2].data();
			Atoms[1].Width = 1;
			Idx.Apply(States, Conditions, Atoms, Changed, Updates);
			const LineBreaker::BoundaryCounter &ContentIdx = Idx.Content; // 更新後の内容の共有参照
			// 試行の不変性と，両採用で同じ作業列を使い全インデックスが元へ戻る事の返戻
			return IsUnchanged && Updates.Used == 12 && Updates.IdxUpdates == 2 && Updates.SummaryCompositions == 2 && WidthBuffer &&
			BoundaryBuffer && ContentBuffer && WidthBuffer == Idx.Changes[0].data() && BoundaryBuffer == Idx.Changes[1].data() &&
			ContentBuffer == Idx.Changes[2].data() && !Idx.Changes[3].capacity() && Idx.Tree == WidthTree && Idx.Vals == Widths &&
			ContentIdx.Tree == Content && ContentIdx.IsCounted == ContentFlags && Idx.EdgeTree == EdgeTree && Idx.EdgeVals == EdgeVals &&
			Idx.Boundaries.Tree == Boundaries && Idx.Boundaries.IsCounted == BoundaryFlags;
		}

		/**
		 * ４列の更新を登録順で反映した結果と，直接構築したインデックスの照合関数
		 * 計算量：固定８原子に対し O(1)
		 * @return 幅・境界・内容・行端差と作業計数が一致し，逆の更新でも元へ戻れば true
		 * @throws FormatLimitExceeded インデックス更新が手記述した作業予算を超える場合
		 * @throws std::bad_alloc 原子又はインデックスのメモリを確保出来ない場合
		 * @throws std::length_error 作業列が保持領域の最大長を超える場合
		 */
		static bool AreIndexUpdatesIndependent() {
			// 重なる更新経路を持つ幅・境界・内容・行端差の準備
			std::vector<LineBreaker::Atom> Atoms(8);
			const std::vector<uint8_t> Conditions(1, 0), NeedsIndex(Atoms.size(), 1);
			const std::vector<LineBreaker::GroupState> Pending(1, LineBreaker::GroupState::Pending), Flat(1, LineBreaker::GroupState::Flat);
			std::vector<size_t> Positions;
			for(size_t Pos = 0; Pos < Atoms.size(); ++Pos) {
				Atoms[Pos].Kind = Pos % 2 ? LineBreaker::AtomKind::Soft : LineBreaker::AtomKind::EdgeSpace;
				Atoms[Pos].Group = 0;
				Atoms[Pos].Width = 1;
				Atoms[Pos].TailWidth = 2;
				Positions.push_back(Pos);
			}
			LineBreaker::MeasureIdx Idx(Pending, Conditions, NeedsIndex, Atoms);
			Meter Counts;
			Counts.Limit = 128; // １回毎に葉８・境界経路８・幅経路２０・４列の反映２８
			for(const bool IsRestore : { false, true }) {
				for(LineBreaker::Atom &Piece : Atoms) {
					Piece.Width = IsRestore ? 1 : 0;
					Piece.TailWidth = IsRestore ? 2 : 0;
				}
				const std::vector<LineBreaker::GroupState> &States = IsRestore ? Pending : Flat;
				Idx.Apply(States, Conditions, Atoms, Positions, Counts);
				const LineBreaker::MeasureIdx Direct(States, Conditions, NeedsIndex, Atoms);
				if(
					Idx.Tree != Direct.Tree || Idx.Vals != Direct.Vals || Idx.Boundaries.Tree != Direct.Boundaries.Tree ||
					Idx.Boundaries.IsCounted != Direct.Boundaries.IsCounted || Idx.Content.Tree != Direct.Content.Tree ||
					Idx.Content.IsCounted != Direct.Content.IsCounted || Idx.EdgeTree != Direct.EdgeTree || Idx.EdgeVals != Direct.EdgeVals
				// 直接構築と異なるセルを検出した事の返戻
				) return false;
			}
			// 独立したセルの全値と，変更回数・作業量の一致の返戻
			return Counts.Used == 128 && Counts.IdxUpdates == 16 && Counts.SummaryCompositions == 32;
		}

		/**
		 * 改行と可視のコードポイントが１度の本文走査で測られる事の検査関数
		 * 計算量：各行の反復文字数 B に対し O(B)
		 * @param ByteCount 各行の反復可視文字数
		 * @return 本文長の予算だけで，制御文字・非制御文字・終端改行を含む幅要約を得れれば true
		 * @throws FormatLimitExceeded トークン計数が作業予算を超える場合
		 * @throws std::bad_alloc トークンのメモリを確保出来ない場合
		 * @throws std::length_error トークンが保持領域の最大長を超える場合
		 */
		static bool HasSinglePassLexemeMeasurement(const size_t ByteCount) {
			// 先頭行のタブと C1 制御文字，次行の非制御文字，終端の空行の幅の照合
			const std::string Text = std::string(ByteCount, 'x') + "\t\xC2\x85\n" + std::string(ByteCount, 'y') + "C\xC2\xA0" "D\n";
			Meter Counts;
			Counts.Limit = Text.size();
			const TextMetrics::LineSummary Summary = Counts.Count(Text), Direct = TextMetrics::MeasureLines(Text);
			// １度の本文の計上と行幅要約の一致の返戻
			return Counts.Used == Text.size() && Counts.LexemeBytes == Text.size() && Summary.Head == ByteCount && !Summary.Tail &&
			Summary.Max == ByteCount + 3 && Summary.HasNewline && Direct.Head == ByteCount && !Direct.Tail && Direct.Max == ByteCount + 3 &&
			Direct.HasNewline;
		}

		/**
		 * 未評価の境界と，評価中の境界を除く行端照会の線形性の検査関数
		 * 計算量：内容を持たない原子数 N に対し O(N)
		 * @param AtomCount 境界の前に置く内容を持たない原子数
		 * @return 未評価では行端，評価中では内容を得て，各照会が N + 1 単位なら true
		 * @throws FormatLimitExceeded 隣接原子の走査が作業予算を超える場合
		 * @throws std::bad_alloc 原子列又は選択列を確保出来ない場合
		 * @throws std::length_error 原子列又は選択列の要素数上限を超える場合
		 */
		static bool HasLinearLineEdgeQueries(const size_t AtomCount) {
			// 零幅のアンカー列の後に未評価の境界と可視の内容を置く
			std::vector<LineBreaker::Atom> Atoms(AtomCount + 3);
			Atoms.front().Kind = LineBreaker::AtomKind::EdgeSpace;
			for(size_t Pos = 1; Pos <= AtomCount; ++Pos) Atoms[Pos].Kind = LineBreaker::AtomKind::AnchorBegin;
			Atoms[AtomCount + 1].Kind = LineBreaker::AtomKind::Soft;
			Atoms[AtomCount + 1].Group = 0;
			Atoms[AtomCount + 1].HasHeadContent = true;
			Atoms.back().HasHeadContent = true;
			const std::vector<LineBreaker::GroupState> States { LineBreaker::GroupState::Pending };
			const std::vector<uint8_t> Conditions;
			Meter PendingWork, EvaluatedWork;
			PendingWork.Limit = EvaluatedWork.Limit = AtomCount + 1;
			// 同じ照会でも，評価対象の境界だけを除いた結果と区別する
			const bool IsPendingEdge = LineBreaker::IsLineEdge(States, Conditions, Atoms, 0, 1, NoGroup, PendingWork);
			const bool IsEvaluatedEdge = LineBreaker::IsLineEdge(States, Conditions, Atoms, 0, 1, 0, EvaluatedWork);
			// 片方向の１度の走査の計数と，仮境界の保持の返戻
			return IsPendingEdge && !IsEvaluatedEdge && PendingWork.Used == AtomCount + 1 && EvaluatedWork.Used == AtomCount + 1;
		}

		/**
		 * 見出の候補列を読む作業が予算へ計上される事の検査関数
		 * 計算量：見出の候補数 N に対し O(N)
		 * @param CandidateCount 見出の候補数
		 * @return 候補数未満の予算でリソース上限の見送に為れば true
		 * @throws std::bad_alloc 要素又は候補列のメモリを確保出来ない場合
		 * @throws std::length_error 要素又は候補列が保持領域の最大長を超える場合
		 * @throws std::out_of_range 呼出先が候補又はグループの範囲外を参照する場合
		 */
		static bool IsBracePreparationLimited(const size_t CandidateCount) {
			// 見出の候補と同じ必須境界区間の準備
			std::vector<LineBreaker::Atom> Atoms(CandidateCount);
			std::pmr::monotonic_buffer_resource Storage;
			LineBreaker::BoundaryLists Candidates(2, &Storage), Conditionals(2, &Storage);
			for(size_t Pos = 0; Pos < CandidateCount; ++Pos) {
				Atoms[Pos].Kind = LineBreaker::AtomKind::Soft;
				Atoms[Pos].Group = 1;
				Candidates[1].push_back(Pos);
			}
			std::vector<LineBreaker::GroupState> States(2, LineBreaker::GroupState::Pending);
			std::vector<uint8_t> Conditions(2, 0);
			Meter Counts;
			Counts.Limit = CandidateCount - 1;
			Counts.ZeroSegments.assign(CandidateCount, 0);
			// 本体のラッパーを必須とし，幅０でも見出の候補を読む作業を照合
			try {
				LineBreaker(0).HasAppliedBraceBody(
					States,
					Conditions,
					Atoms,
					Candidates,
					Conditionals,
					LineBreaker::BraceChoice{ 0, { 1 }, {} },
					true,
					nullptr,
					Counts
				);
			} catch(const FormatLimitExceeded &E) {
				// リソース上限の見送だけを検出した事の返戻
				return E.Kind == FailureReason::LimitExceeded && E.Reason == "line wrapping exceeds formatting limit";
			}
			// 候補列の読取を計上せず終了した事の返戻
			return false;
		}

		/**
		 * 幅０の宣言区分が不変条件の検査だけを計上する事の照合関数
		 * 計算量：宣言子数 N に対し O(N)
		 * @param IR 宣言の中間表現
		 * @param Count 宣言子数と同じ検査の予算
		 * @return 幅を測らず検査だけの予算で１つの区分を記録すれば true
		 * @throws FormatLimitExceeded 中間表現が不整合又は検査の予算を超える場合
		 * @throws std::bad_alloc 区分のメモリを確保出来ない場合
		 * @throws std::length_error 区分が保持領域の最大長を超える場合
		 * @throws std::out_of_range 中間表現の参照範囲が共有領域に無い場合
		 */
		static bool IsUnlimitedPartition(FormatIR &IR, const size_t Count) {
			// 幅０の宣言子の検査と出力区分の記録
			std::vector<DeclPartition> Partitions;
			uint32_t NextGroup = 0;
			Meter Counts;
			Counts.Limit = Count;
			LineBreaker(0).PartitionDecl(IR, IR.Root, Partitions, NextGroup, Counts);
			// 未使用の幅測定と追加の走査が無い１つの出力区分の返戻
			return Counts.Used == Count && !Counts.LexemeBytes && !Counts.Summaries && !NextGroup && Partitions.size() == 1 &&
			Partitions[0].Node == IR.Items[IR.Root].Node && Partitions[0].Rows.size() == 1 && !Partitions[0].Rows[0].Begin &&
			Partitions[0].Rows[0].End == Count;
		}

		/**
		 * 共有グループのノード収集が初出順を保ち線形の予算で終わる事の照合関数
		 * 計算量：グループ毎のノード数 N に対し平均 O(N)
		 * @param Count 異なるノードの数
		 * @return 同じノードを別のグループにも記録し，同じ組の再出現だけを除けば true
		 * @throws FormatLimitExceeded 線形化が手記述の作業予算を超える場合
		 * @throws std::bad_alloc 中間表現・作業列又は観測用の集合を確保出来ない場合
		 * @throws std::length_error 中間表現又は作業列が保持領域の最大長を超える場合
		 * @throws std::out_of_range 観測結果に対応するグループが無い場合
		 */
		static bool HasLinearGroupNodeCollection(const uint32_t Count) {
			// ２つのグループへ同じノードを２度ずつ逆順で記録する中間表現の構築
			FormatIR IR;
			std::vector<FormatItemId> Parts;
			std::vector<NodeId> Expected;
			for(uint32_t Node = Count; Node--;) Expected.push_back(Node);
			for(uint32_t Group = 0; Group < 2; ++Group) for(uint32_t Pass = 0; Pass < 2; ++Pass) for(const NodeId Node : Expected) {
				FormatItem Item;
				Item.Kind = FormatItemKind::Group;
				Item.Group = Group;
				Item.Node = Node;
				Parts.push_back(IR.Add(Item));
			}
			IR.Root = IR.Parent(FormatItemKind::Sequence, Parts);
			LineBreaker::Linearizer State;
			Meter Counts;
			// 各出現の親と子の訪問２，根の終了１の予算（観測の照会は含めない）
			Counts.Limit = (static_cast<size_t>(Count) << 3) + 1;
			Counts.GroupFinalizations.resize(2, 0);
			CoreObservation Observation;
			LineBreaker::Linearize(IR, State, IR.Root, NoGroup, Counts, &Observation);
			Observation.CompleteSelections(State.Forms);
			LineBreaker::Linearizer Plain;
			Meter PlainCounts;
			PlainCounts.Limit = Counts.Limit;
			PlainCounts.GroupFinalizations.resize(2, 0);
			LineBreaker::Linearize(IR, Plain, IR.Root, NoGroup, PlainCounts);
			// １単位足りない場合も，観測の有無で同じ境界に達して見送る
			for(const bool IsObserved : { false, true }) {
				LineBreaker::Linearizer Limited;
				Meter LimitedCounts;
				LimitedCounts.Limit = Counts.Limit - 1;
				LimitedCounts.GroupFinalizations.resize(2, 0);
				CoreObservation LimitedObservation;
				try {
					LineBreaker::Linearize(IR, Limited, IR.Root, NoGroup, LimitedCounts, IsObserved ? &LimitedObservation : nullptr);
					// 予算超過を検出しなかった事の返戻
					return false;
				} catch(const FormatLimitExceeded &E) {
					// 上限以外の分類又は異なる作業量で終わった事の返戻
					if(E.Kind != FailureReason::LimitExceeded || LimitedCounts.Used != LimitedCounts.Limit) return false;
				}
			}
			// 原文の初出順と，全ての出現を１度読む計数の返戻
			return Counts.Used == Counts.Limit && PlainCounts.Used == Counts.Used &&
			Counts.GroupNodeVisits == static_cast<size_t>(Count) << 2 && State.Forms.at(0).Nodes == Expected &&
			State.Forms.at(1).Nodes == Expected;
		}

		/**
		 * 構築済の決定単位を製品の手順で評価する関数
		 * 計算量：原子数 N と参照する候補差分の累計 K に対し O(N + K log(N + K + 1))
		 * @param IR 幅非依存の中間表現
		 * @param MaxChars 最大文字数
		 * @return 解決結果と計数
		 * @throws FormatLimitExceeded 中間表現が不整合又は作業予算を超える場合
		 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
		 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
		 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
		 */
		static Result Run(FormatIR IR, const size_t MaxChars) {
			// 構築済の決定単位を製品の手順で評価
			const LineBreaker Breaker(MaxChars);
			Result Checked;
			Checked.Output = Breaker.ResolveMeasured(std::move(IR), &Checked.Counts);
			// 製品の評価結果と計数の返戻
			return Checked;
		}
	};
}

/**
 * 展開時だけ出すトークンの追加関数
 * 計算量：テキストのバイト数 B に対し O(B)
 * @param IR 中間表現
 * @param Group 従うグループの番号
 * @param Text 展開時のテキスト
 * @return 条件付要素の番号
 * @throws FormatLimitExceeded 中間表現の構築又は評価が作業予算を超える場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
 */
static Core::FormatItemId AddConditional(Core::FormatIR &IR, const uint32_t Group, const std::string_view Text) {
	// 条件付要素とグループの関連付
	const std::vector<Core::FormatItemId> Children { IR.Leaf(Text, Core::FormatItemKind::Token) };
	const Core::FormatItemId Id = IR.Parent(Core::FormatItemKind::IfBroken, Children);
	IR.Items[Id].Group = Group;
	// 条件付要素の番号の返戻
	return Id;
}

/**
 * 子を持つグループの追加関数
 * 計算量：子の数 N に対し O(N)
 * @param IR 中間表現
 * @param Group グループの番号
 * @param Children 子の並び
 * @return グループの要素番号
 * @throws FormatLimitExceeded 中間表現の構築又は評価が作業予算を超える場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 */
static Core::FormatItemId AddGroup(Core::FormatIR &IR, const uint32_t Group, const std::vector<Core::FormatItemId> &Children) {
	// グループと識別番号の設定
	const Core::FormatItemId Id = IR.Parent(Core::FormatItemKind::Group, Children);
	IR.Items[Id].Group = Group;
	// グループの要素番号の返戻
	return Id;
}

/**
 * namespace Core
 * テスト条件の検査関数
 * 計算量：O(1)
 * @param IsSatisfied 成功条件
 * @param Msg 失敗箇所
 * @throws std::runtime_error 条件を満たさない場合
 */
static void Check(const bool IsSatisfied, const char *const Msg) {
	// テスト条件の確認
	if(!IsSatisfied) throw std::runtime_error(Msg);
	// 終了
	return;
}

namespace Core {
	// 配置の本文参照の合成数を観測する入口
	struct CodeLayoutAccess {
		/**
		 * 製品と同じ配置で本文の参照数を取得する関数
		 * 計算量：配置の総費用 Q に対し O(Q)
		 * @param IR 解決済の中間表現
		 * @param Work 非空の参照数と連続範囲の数の格納先
		 * @return 確定した配置
		 * @throws FormatLimitExceeded 出力が上限を超える場合
		 * @throws std::bad_alloc 出力又は作業列を確保出来ない場合
		 * @throws std::length_error 出力又は作業列が保持領域の最大長を超える場合
		 * @throws std::out_of_range 構文又は本文の参照先が範囲外の場合
		 */
		static LaidOutCode Materialize(const BrokenIR &IR, std::pair<size_t, size_t> &Work) {
			// 本文の計数を伴う配置の返戻
			return CodeLayout::MaterializeWithWork(IR, &Work);
		}
	};

	// 既存の文脈準備と同時に作った後送の要約を観測する入口
	struct CanonicalizerAccess {
		/**
		 * 後送の要約と共通走査の作業量の照合関数
		 * 計算量：文脈の準備で訪問するノード・子数 N に対し O(N)
		 * @param Syntax 読取専用の構文
		 * @param Count 部分木の開始と本文の期待数
		 * @param HasPattern 照合の節を持つかの期待
		 * @return 文脈の準備で計上した作業量
		 * @throws FormatLimitExceeded 文脈の準備が作業予算を超える場合
		 * @throws std::runtime_error 共通走査が開始・本文又は照合の節を要約して居ない場合
		 * @throws std::bad_alloc 要約又は作業列を確保出来ない場合
		 * @throws std::length_error 要約又は作業列の件数が上限を超える場合
		 * @throws std::out_of_range 要約の参照先が表に無い場合
		 */
		static size_t GetHeredocWork(const SyntaxModel &Syntax, const uint32_t Count, const bool HasPattern) {
			// 専用の走査を呼ばず，文脈準備だけで後送の３値が揃う事の照合
			FormatIR IR;
			Canonicalizer::Builder State { Syntax, IR };
			const size_t Before = Syntax.GetWorkAccount()->Used;
			Canonicalizer::PrepareContexts(State);
			const Canonicalizer::HeredocSummary &Summary = State.Heredocs.at(Syntax.GetRoot());
			Check(Summary.Begins == Count && Summary.Bodies == Count, "context traversal did not summarize deferred literals");
			Check(Summary.HasPattern == HasPattern, "context traversal lost pattern branches");
			// 封止中は候補を読まない為，子数に比例する０の並びも確保しない
			State.NoBreakDepth = 1;
			const size_t BeforeSealed = Syntax.GetWorkAccount()->Used;
			for(NodeId Id = 0; Id < Syntax.GetNodeCount(); ++Id) {
				const std::pmr::vector<uint8_t> Candidates = Canonicalizer::BreakBoundaries(State, Id);
				Check(Candidates.empty() && !Candidates.capacity(), "sealed syntax allocated unused break candidates");
			}
			Check(Syntax.GetWorkAccount()->Used == BeforeSealed, "sealed syntax scanned unused break boundaries");
			// 共通走査の作業量の返戻
			return Syntax.GetWorkAccount()->Used - Before;
		}
	};
}

/**
 * 正規の入力経路から構築予算を測る構文を投影する関数
 * 計算量：入力のテキスト長 B と解析・投影で訪問するトークン・ノードの延数 N に対し O(B + N)
 * @param Lang 入力言語
 * @param Src 入力
 * @return 入力と解析木を持たない構文表
 * @throws FormatLimitExceeded 解析又は投影の予算を超える場合
 * @throws std::runtime_error 入力が受理されない場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::invalid_argument 指定された言語を呼出先が扱えない場合
 * @throws std::length_error 入力側又は投影先のテキスト・要素の数が保持領域の最大長を超える場合
 * @throws std::out_of_range 入力側又は投影先のノード・共有範囲が参照先の表又はテキストに無い場合
 */
static Core::SyntaxModel ProjectSrc(const ::Lang Lang, const std::string &Src) {
	// 正規の入力経路を使う，構築予算の測定対象の構文の投影
	const GrammarProfile &Profile = GrammarProfile::Resolve(Lang);
	InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, Src);
	Check(Normalized.Reason == FailureReason::None, "the scaling input was rejected before parsing");
	ParseInput Input = ParseAdapter::Adapt(Profile, Normalized);
	Check(Input.Decision == ParseDecision::Continue, "the scaling input was rejected during adaptation");
	const ParsedTree Parsed = SyntaxParser::Parse(*Input.Profile, Input);
	Check(Parsed.GetParseDecision() == ParseDecision::Continue, "the scaling input was rejected during parsing");
	Core::ProjectionResult Projected = Core::SourceProjector::Project(Parsed, *Input.Profile, Input);
	// 独立した構文表の返戻
	return std::move(Projected.Syntax);
}

/**
 * Python の正規の入力経路から構文を投影する関数
 * 計算量：入力のテキスト長 B と解析・投影で訪問するトークン・ノードの延数 N に対し O(B + N)
 * @param Src 入力
 * @return 投影した構文
 * @throws FormatLimitExceeded 入力又は投影が上限を超える場合
 * @throws std::runtime_error 入力を受理出来ない場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::invalid_argument 指定された言語を呼出先が扱えない場合
 * @throws std::length_error 入力側又は投影先のテキスト・要素の数が保持領域の最大長を超える場合
 * @throws std::out_of_range 入力側又は投影先のノード・共有範囲が参照先の表又はテキストに無い場合
 */
static Core::SyntaxModel ProjectPython(const std::string &Src) {
	// Python の構文の返戻
	return ProjectSrc(Lang::Get(Lang::Python), Src);
}

/**
 * 解決済の要素から行分割のテキストを得る関数
 * 計算量：解決済の要素数 N と出力バイト数 B に対し O(N + B)
 * @param IR 解決済の中間表現
 * @return 出力のテキスト
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 * @throws std::out_of_range 解決済のテキストの範囲が共有領域に無い場合
 */
static std::string Render(const Core::BrokenIR &IR) {
	// 解決済要素からのテキストの連結
	std::string Text;
	for(const Core::ResolvedItem &Item : IR.Items) switch(Item.Kind) {
	case Core::ResolvedKind::Newline:
		Text.push_back('\n');
		break;
	case Core::ResolvedKind::Text:
	case Core::ResolvedKind::Separator:
	case Core::ResolvedKind::Verbatim:
		Text.append(IR.GetTextOf(Item));
		break;
	}
	// テキストの返戻
	return Text;
}

/**
 * 継続括弧と代入の接頭部を持つテストケースの構築関数
 * 計算量：テキストのバイト数 B に対し O(B)
 * @param HasVerbatim 複数行の文字列と後続の組を使うか
 * @return ラッパーと直前の境界が同じ単位に属する中間表現
 * @throws FormatLimitExceeded 中間表現の構築が作業予算を超える場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
 */
static Core::FormatIR MakeContinuationFixture(const bool HasVerbatim) {
	// 継続括弧と代入の接頭部を持つテストケースの構築
	Core::FormatIR Fixture;
	Core::FormatItemId Content;
	if(HasVerbatim) {
		const std::vector<Core::FormatItemId> TupleParts {
			Fixture.Leaf("(", Core::FormatItemKind::Token),
			Fixture.Leaf("", Core::FormatItemKind::SoftBreak),
			Fixture.Leaf("a,", Core::FormatItemKind::Token),
			Fixture.Leaf(" ", Core::FormatItemKind::SoftBreak),
			Fixture.Leaf("b", Core::FormatItemKind::Token),
			Fixture.Leaf("", Core::FormatItemKind::SoftBreak),
			Fixture.Leaf(")", Core::FormatItemKind::Token)
		};
		Fixture.Items[TupleParts.front()].Generated = Core::GeneratedToken::CallOpen;
		Fixture.Items[TupleParts.back()].Generated = Core::GeneratedToken::CallClose;
		const Core::FormatItemId Tuple = AddGroup(Fixture, 0, TupleParts);
		const Core::FormatItemId Literal = Fixture.Leaf("\"\"\"\n" + std::string(120, 'x') + "\"\"\"", Core::FormatItemKind::Verbatim);
		Fixture.EditDetail(Literal).DoesExcludeCodeNewlines = true;
		const std::vector<Core::FormatItemId> ContentParts {
			Literal,
			Fixture.Leaf(" %", Core::FormatItemKind::Token),
			Fixture.Leaf(" ", Core::FormatItemKind::SoftBreak),
			Tuple
		};
		Content = AddGroup(Fixture, 1, ContentParts);
	} else {
		const std::vector<Core::FormatItemId> ContentParts {
			Fixture.Leaf("aaaa +", Core::FormatItemKind::Token),
			Fixture.Leaf(" ", Core::FormatItemKind::SoftBreak),
			Fixture.Leaf("bbbb", Core::FormatItemKind::Token)
		};
		Content = AddGroup(Fixture, 1, ContentParts);
	}
	const std::vector<Core::FormatItemId> WrapperParts {
		AddConditional(Fixture, 2, "("),
		Fixture.Leaf("", Core::FormatItemKind::SoftBreak),
		Content,
		Fixture.Leaf("", Core::FormatItemKind::SoftBreak),
		AddConditional(Fixture, 2, ")")
	};
	const Core::FormatItemId Wrapper = AddGroup(Fixture, 2, WrapperParts);
	Fixture.Items[Wrapper].Node = 1;
	Fixture.Items[Wrapper].Operation = Core::FormOperation::Continuation;
	const std::vector<Core::FormatItemId> AssignmentParts {
		Fixture.Leaf(HasVerbatim ? "x =" : "body: str =", Core::FormatItemKind::Token),
		Fixture.Leaf(" ", Core::FormatItemKind::SoftBreak),
		Wrapper
	};
	Fixture.Root = AddGroup(Fixture, 3, AssignmentParts);
	Fixture.BuildDecisions();
	// 構築した継続括弧のテストケースの返戻
	return Fixture;
}

/**
 * 深いコンテナの幅０の判定の作業量を照合する関数
 * 計算量：最大のグループ数 N と実入力の解析・投影・正規化・構築の費用 Q に対し O(N + Q)
 * @throws FormatLimitExceeded 構築又は評価が作業予算を超える場合
 * @throws std::runtime_error 平坦形・必須境界・作業量の線形性が一致しない場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
 */
static void VerifyZeroWidthScaling() {
	// 深いコンテナの幅０の判定の作業量を照合
	for(const bool HasMandatory : { false, true }) {
		size_t Prev = 0;
		for(const uint32_t Count : std::initializer_list<uint32_t>{ 512, 1024 }) {
			Core::FormatIR Fixture;
			Core::FormatItemId Content = Fixture.Leaf("2", Core::FormatItemKind::Token);
			if(HasMandatory) {
				const std::vector<Core::FormatItemId> Parts {
					Content,
					Fixture.Leaf("", Core::FormatItemKind::HardBreak),
					Fixture.Leaf("3", Core::FormatItemKind::Token)
				};
				Content = Fixture.Parent(Core::FormatItemKind::Sequence, Parts);
			}
			for(uint32_t Group = 0; Group < Count; ++Group) {
				const std::vector<Core::FormatItemId> Parts {
					Fixture.Leaf("[1,", Core::FormatItemKind::Token),
					Fixture.Leaf(" ", Core::FormatItemKind::SoftBreak),
					Content,
					Fixture.Leaf("", Core::FormatItemKind::SoftBreak),
					Fixture.Leaf("]", Core::FormatItemKind::Token)
				};
				Content = AddGroup(Fixture, Group, Parts);
			}
			Fixture.Root = Content;
			Fixture.BuildDecisions();
			const Core::LineBreakerMeasurement::Result Checked = Core::LineBreakerMeasurement::Run(std::move(Fixture), 0);
			Check(!Prev || Checked.Counts.Used < (Prev << 1) + 64, "zero-width work grew faster than linearly");
			Prev = Checked.Counts.Used;
			Check(!Checked.Counts.DeclGroupVisits, "zero width prepared unused declaration copy groups");
			Check(
				!Checked.Counts.LexemeBytes && !Checked.Counts.PrefixEntries && !Checked.Counts.IdxUpdates,
				"zero width measured text or built a width index"
			);
			Check(Checked.Counts.GroupFinalizations == std::vector<size_t>(Count, 1), "a zero-width group was not finalized once");
			for(const Core::SelectedForm &Form : Checked.Output.Expectation.SelectedForms) {
				Check(Form.IsBroken == HasMandatory, "zero width discarded a mandatory boundary or kept an optional one");
			}
		}
	}
	// 実際の JSON の入力経路でも，深さを倍増して正規化・中間表現の構築・幅０の評価の作業量を照合する
	size_t PrevStructure = 0, PrevWrapping = 0, PrevBuild = 0;
	for(uint32_t Count = 256; Count < 4097; Count <<= 1) {
		std::string Src, Expected;
		for(uint32_t Depth = 0; Depth < Count; ++Depth) {
			Src.append("{\"a\":");
			Expected.append("{ \"a\": ");
		}
		Src.push_back('1');
		Src.append(Count, '}');
		Expected.push_back('1');
		for(uint32_t Depth = 0; Depth < Count; ++Depth) Expected.append(" }");
		Expected.push_back('\n');
		Core::SyntaxModel Projected = ProjectSrc(Lang::Get(Lang::JSON), Src);
		Core::SyntaxNormalizer().Normalize(Projected);
		const size_t BeforeBuild = Projected.GetWorkAccount()->Used;
		Core::FormatIR IR = Core::Canonicalizer::Build(std::make_shared<Core::SyntaxModel>(std::move(Projected)));
		const size_t BuildWork = IR.NormalizedSyntax->GetWorkAccount()->Used - BeforeBuild;
		// 深さを倍増しても，正準化の控えの参照と組立が線形に収まる事を個別に照合する
		Check(BuildWork && (!PrevBuild || BuildWork <= (PrevBuild << 1) + 64), "deep JSON repeated canonical construction work");
		PrevBuild = BuildWork;
		std::fprintf(stdout, "canonical JSON n=%u work=%zu\n", Count, BuildWork);
		const size_t StructureWork = IR.NormalizedSyntax->GetWorkAccount()->Used;
		Check(!PrevStructure || StructureWork <= (PrevStructure << 1) + 64, "deep JSON repeated normalization or construction work");
		PrevStructure = StructureWork;
		IR.CloseWork();
		const Core::LineBreakerMeasurement::Result Checked = Core::LineBreakerMeasurement::Run(std::move(IR), 0);
		Check(!PrevWrapping || Checked.Counts.Used <= (PrevWrapping << 1) + 64, "deep JSON repeated zero-width evaluation work");
		PrevWrapping = Checked.Counts.Used;
		Check(
			!Checked.Counts.LexemeBytes && !Checked.Counts.PrefixEntries && !Checked.Counts.IdxUpdates,
			"deep JSON measured widths or built a width index at zero width"
		);
		const std::shared_ptr<const LaidOutCode> Code = std::make_shared<LaidOutCode>(Core::CodeLayout::Materialize(Checked.Output));
		size_t Stored = 0;
		for(const CodeFragment &Row : Code->GetFragments()) Stored += Row.Text.size();
		Check(Stored + 1 == Expected.size(), "deep JSON layout stored more than its flat text");
		Check(
			OutputFinalizer::Finalize(OutputDocument::Compose(Code, CommentPlacementPlan(*Code))) == Expected,
			"deep JSON changed the independently specified flat output"
		);
	}
	// 波括弧が必須の本体を単一文でも展開し，２文以上の本体の必須改行を保つ
	struct ControlFixture {
		::Lang::ID Lang;
		const char *Header;
		const char *Body;
		const char *Inline;
	}; // 条文が禁じる波括弧付の単一の制御本体の同行形
	const ControlFixture ControlInputs[] {
		{ Lang::Go, "package p\nfunc f(){\n", "if x { g() } else { h() }\nif y { g(); h() }\n", "if x { g() } else { h() }" },
		{ Lang::Rust, "fn f(){\n", "if x { g(); } else { h(); }\nif y { g(); h(); }\n", "if x { g(); } else { h(); }" },
		{ Lang::Swift, "func f(){\n", "if x { g() } else { h() }\nif y { g(); h() }\n", "if x { g() } else { h() }" }
	};
	for(const ControlFixture &Input : ControlInputs) {
		size_t PrevControlBuild = 0;
		for(const size_t ControlCount : { 16U, 32U, 64U }) {
			std::string ControlSrc = Input.Header;
			for(size_t Idx = 0; Idx < ControlCount; ++Idx) ControlSrc += Input.Body;
			ControlSrc += "}\n";
			Core::SyntaxModel ControlSyntax = ProjectSrc(Lang::Get(Input.Lang), ControlSrc);
			Core::SyntaxNormalizer().Normalize(ControlSyntax);
			const size_t ControlStart = ControlSyntax.GetWorkAccount()->Used;
			Core::FormatIR ControlIR = Core::Canonicalizer::Build(std::make_shared<Core::SyntaxModel>(std::move(ControlSyntax)));
			const size_t ControlWork = ControlIR.NormalizedSyntax->GetWorkAccount()->Used - ControlStart;
			Check(
				ControlWork && (!PrevControlBuild || ControlWork <= 3 * PrevControlBuild),
				"single control bodies repeated their traversal"
			);
			PrevControlBuild = ControlWork;
			ControlIR.CloseWork();
			const Core::LineBreakerMeasurement::Result ControlOutput = Core::LineBreakerMeasurement::Run(std::move(ControlIR), 0);
			const std::string ControlText = Render(ControlOutput.Output);
			size_t ElseCount = 0, ElsePos = 0;
			while((ElsePos = ControlText.find("} else {\n", ElsePos)) != std::string::npos) {
				++ElseCount;
				ElsePos += std::string_view("} else {\n").size();
			}
			Check(ControlText.find(Input.Inline) == std::string::npos, "a braced single control body kept its inline form");
			Check(ElseCount == ControlCount, "a braced single control body lost its expanded else");
			Check(ControlText.find("if y {\n") != std::string::npos, "two statement control body lost its mandatory newline");
			std::fprintf(
				stdout,
				"single control bodies lang=%u n=%zu build=%zu\n",
				static_cast<unsigned>(Input.Lang),
				ControlCount,
				ControlWork
			);
		}
	}
	// 後送の有無・記法・照合の節を変え，兄弟数の倍増時も既存の文脈準備と構築の費用を共有する
	struct DeferredFixture {
		const char *Src;
		const char *Body;
		uint32_t Begins;
		bool HasPattern;
		const char *Head = nullptr; // 逐語内容として１度ずつ残す本文（無ければ検査しない）
	}; // 条文から定めた，封止行で保持する語と文区切
	const DeferredFixture DeferredInputs[] {
		{ "if c then <<~C else 1 end\n  body\nC\n", "  body\nC", 1, false, "if c then <<~C else 1 end" },
		{ "unless c then <<~C else 1 end\n  body\nC\n", "  body\nC", 1, false, "unless c then <<~C else 1 end" },
		{ "case c when 1 then <<~C end\n  body\nC\n", "  body\nC", 1, false, "case c when 1 then <<~C end" },
		{ "if c then 1 elsif d then <<~C end\n  body\nC\n", "  body\nC", 1, false, "if c then 1 elsif d then <<~C end" },
		{ "foo(if c then <<~C else 1 end)\n  body\nC\n", "  body\nC", 1, false, "foo(if c then <<~C else 1 end)" },
		{ "x = <<~A; def m; 1; end\n  body\nA\n", "  body\nA", 1, false, "; def m;" },
		{ "x = <<~A; begin; y; rescue; z; end\n  body\nA\n", "  body\nA", 1, false, "begin; y; rescue; z; end" },
		{ "x = foo(a,b)\n", nullptr, 0, false },
		{ "x = <<X\nbody\nX\n", "body\nX", 1, false },
		{ "x = <<-X\n  dash body\n  X\n", "  dash body\n  X", 1, false },
		{ "x = <<~X\n  squiggly body\nX\n", "  squiggly body\nX", 1, false },
		{ "x = <<X\n#{value}\nX\n", "#{value}\nX", 1, false },
		{ "foo(<<X, <<Y)\none\nX\ntwo\nY\n", "one\nX\ntwo\nY", 2, false },
		{ "c = case y\nin 1 then <<~C end\n  one\nC\n", "  one\nC", 1, true }
	};
	for(const DeferredFixture &Input : DeferredInputs) {
		size_t PrevContexts = 0, PrevCanonical = 0, PrevZeroWidth = 0, PrevBoundedWidth = 0;
		for(const uint32_t Count : { 32U, 64U }) {
			std::string Src;
			for(uint32_t Idx = 0; Idx < Count; ++Idx) Src += Input.Src;
			Core::SyntaxModel Syntax = ProjectSrc(Lang::Get(Lang::Ruby), Src);
			Core::SyntaxNormalizer().Normalize(Syntax);
			const size_t ContextWork = Core::CanonicalizerAccess::GetHeredocWork(Syntax, Input.Begins * Count, Input.HasPattern);
			Check(!PrevContexts || ContextWork <= (PrevContexts << 1) + 64, "deferred context summaries exceeded linear growth");
			PrevContexts = ContextWork;
			const size_t Before = Syntax.GetWorkAccount()->Used;
			Core::FormatIR IR = Core::Canonicalizer::Build(std::make_shared<Core::SyntaxModel>(std::move(Syntax)));
			const size_t Work = IR.NormalizedSyntax->GetWorkAccount()->Used - Before;
			Check(!PrevCanonical || Work <= (PrevCanonical << 1) + 64, "deferred canonical construction exceeded linear growth");
			PrevCanonical = Work;
			std::fprintf(
				stdout,
				"deferred summaries n=%u starts=%u contexts=%zu build=%zu\n",
				Count,
				Input.Begins * Count,
				ContextWork,
				Work
			);
			IR.CloseWork();
			for(const size_t Width : { 0U, 128U }) {
				const Core::LineBreakerMeasurement::Result Checked = Core::LineBreakerMeasurement::Run(IR, Width);
				size_t &PrevWrapping = Width ? PrevBoundedWidth : PrevZeroWidth;
				Check(!PrevWrapping || Checked.Counts.Used <= (PrevWrapping << 1) + 64, "deferred wrapping work exceeded linear growth");
				PrevWrapping = Checked.Counts.Used;
				if(!Input.Body) continue;
				// 開始数と独立して，本文の綴り・対応する終端・複数の本文の出現順を照合する
				const std::string Text = Render(Checked.Output);
				size_t Found = 0, Pos = 0;
				const std::string_view Body = Input.Body;
				while((Pos = Text.find(Body, Pos)) != std::string::npos) {
					++Found;
					Pos += Body.size();
				}
				Check(Found == Count, "deferred body text or emission order changed");
				if(Input.Head) {
					// 本文の保持だけでは見逃す，then・節の語・文区切の欠落を照合する
					Found = 0;
					Pos = 0;
					const std::string_view Head = Input.Head;
					while((Pos = Text.find(Head, Pos)) != std::string::npos) {
						++Found;
						Pos += Head.size();
					}
					Check(Found == Count, "sealed header lost a keyword or statement separator");
				}
			}
		}
	}
	// 未展開マクロは括弧のグループを入力の大きさに比例する作業量で構築し，幅０と狭い幅の両方で評価する
	size_t PrevCanonical = 0, PrevZeroWidth = 0, PrevBoundedWidth = 0;
	for(const uint32_t Count : { 128U, 256U, 512U }) {
		std::string Src = "fn f(){m!{";
		for(uint32_t Idx = 0; Idx < Count; ++Idx) Src += "(a + 1),";
		Src += "};}\n";
		Core::SyntaxModel Syntax = ProjectSrc(Lang::Get(Lang::Rust), Src);
		Core::SyntaxNormalizer().Normalize(Syntax);
		const size_t Before = Syntax.GetWorkAccount()->Used;
		Core::FormatIR IR = Core::Canonicalizer::Build(std::make_shared<Core::SyntaxModel>(std::move(Syntax)));
		const size_t Work = IR.NormalizedSyntax->GetWorkAccount()->Used - Before;
		Check(!PrevCanonical || Work <= (PrevCanonical << 1) + 64, "macro token groups exceeded linear construction work");
		PrevCanonical = Work;
		IR.CloseWork();
		for(const size_t Width : { 0U, 20U }) {
			const Core::LineBreakerMeasurement::Result Checked = Core::LineBreakerMeasurement::Run(IR, Width);
			size_t &PrevWrapping = Width ? PrevBoundedWidth : PrevZeroWidth;
			Check(!PrevWrapping || Checked.Counts.Used <= (PrevWrapping << 1) + 64, "macro token groups exceeded linear wrapping work");
			PrevWrapping = Checked.Counts.Used;
			const std::string Text = Render(Checked.Output);
			Check(static_cast<size_t>(std::count(Text.begin(), Text.end(), ',')) == Count, "macro token groups lost a trailing comma");
			Check(Width || Text.find("m!{ (a + 1), (a + 1),") != std::string::npos, "zero width expanded macro token groups");
			std::fprintf(stdout, "macro groups n=%u width=%zu build=%zu wrap=%zu\n", Count, Width, Work, Checked.Counts.Used);
		}
	}
	// 終了
	return;
}

/**
 * 深い行の配置が生成したタブを本文へコピーしない事の照合関数
 * 計算量：ネスト数 N と最終出力長 M に対し O(N + M)
 * @throws FormatLimitExceeded 中間表現又は行分割が作業予算を超える場合
 * @throws std::runtime_error 作業量・保持する本文長・位置又は最終出力が一致しない場合
 * @throws std::bad_alloc 中間表現・配置・期待のテキストを確保出来ない場合
 * @throws std::length_error 中間表現又は出力が保持領域の最大長を超える場合
 * @throws std::out_of_range 中間表現又は配置の参照位置が範囲外の場合
 */
static void VerifyIndentScaling() {
	// 独立した構文の所有者を使う，必須改行を持つ深い呼出の配置
	const std::shared_ptr<const Core::SyntaxModel> Syntax = std::make_shared<Core::SyntaxModel>(ProjectPython("x\n"));
	for(const size_t MaxChars : { size_t(0), size_t(128) }) {
		size_t PrevWork = 0, PrevStored = 0;
		for(uint32_t Count = 256; Count < 4096; Count <<= 1) {
			Core::FormatIR Fixture;
			Fixture.NormalizedSyntax = Syntax;
			Core::FormatItemId Content = Fixture.Leaf("x", Core::FormatItemKind::Token);
			for(uint32_t Group = 0; Group < Count; ++Group) {
				const std::vector<Core::FormatItemId> Inner { Fixture.Leaf("", Core::FormatItemKind::HardBreak), Content };
				const std::vector<Core::FormatItemId> Parts {
					Fixture.Leaf("f(", Core::FormatItemKind::Token),
					Fixture.Parent(Core::FormatItemKind::IndentScope, Inner),
					Fixture.Leaf("", Core::FormatItemKind::HardBreak),
					Fixture.Leaf(")", Core::FormatItemKind::Token)
				};
				Content = AddGroup(Fixture, Group, Parts);
			}
			Fixture.Root = Content;
			Fixture.BuildDecisions();
			Fixture.CloseWork();
			const Core::LineBreakerMeasurement::Result Checked = Core::LineBreakerMeasurement::Run(std::move(Fixture), MaxChars);
			Check(!PrevWork || Checked.Counts.Used <= (PrevWork << 1) + 64, "indent fixture repeated line-breaking work");
			PrevWork = Checked.Counts.Used;
			Check(!Checked.Counts.DeclGroupVisits, "layout without declarations prepared declaration copy groups");
			const std::shared_ptr<const LaidOutCode> Code = std::make_shared<LaidOutCode>(Core::CodeLayout::Materialize(Checked.Output));
			Check(
				Code->GetAnchors().FragmentTabs.size() == Code->GetFragments().size() &&
				Code->GetAnchors().VerbatimBreaks.size() == Code->GetFragments().size(),
				"layout lost a row of deferred anchor metadata"
			);
			size_t Stored = 0;
			uint32_t RowIdx = 0;
			for(const CodeFragment &Row : Code->GetFragments()) {
				const uint32_t Depth = RowIdx <= Count ? RowIdx : 2 * Count - RowIdx;
				Check(Row.PrefixTabs == Depth && Code->GetAnchors().FragmentTabs[RowIdx] == Depth, "layout changed a row's depth");
				Check(!Code->GetAnchors().VerbatimBreaks[RowIdx++], "generated lines acquired a literal boundary");
				Stored += Row.Text.size();
				Check(Row.Storage == Code->GetFragments().front().Storage, "layout allocated a separate owner per generated line");
				Check(Row.End - Row.Start == Row.PrefixTabs + Row.Text.size(), "deferred indent lost a fragment position");
			}
			Check(Stored == 3 * static_cast<size_t>(Count) + 1, "layout copied generated indentation into text storage");
			Check(!PrevStored || Stored <= (PrevStored << 1) + 1, "layout text storage grew faster than linearly");
			PrevStored = Stored;
			// 開きの行・最深の値・逆順の閉じの行に，構造の深さのタブを手で対応付ける
			std::string Expected;
			for(uint32_t Depth = 0; Depth < Count; ++Depth) Expected.append(Depth, '\t').append("f(\n");
			Expected.append(Count, '\t').append("x\n");
			for(uint32_t Depth = Count; Depth--;) Expected.append(Depth, '\t').append(")\n");
			const OutputDocument Doc = OutputDocument::Compose(Code, CommentPlacementPlan(*Code));
			Check(Doc.GetSize() + 1 == Expected.size(), "the layout end lost the composed output length");
			Check(OutputFinalizer::Finalize(Doc) == Expected, "deferred indentation changed the serialized output");
		}
	}
	// リテラルのタブは生成したインデントと区別し，逐語の次行へ深さを加えない
	Core::FormatIR Literal;
	Literal.NormalizedSyntax = Syntax;
	const std::vector<Core::FormatItemId> Inner {
		Literal.Leaf("", Core::FormatItemKind::HardBreak),
		Literal.Leaf("\tx\n\tz", Core::FormatItemKind::Verbatim)
	};
	const std::vector<Core::FormatItemId> Parts {
		Literal.Leaf("f(", Core::FormatItemKind::Token),
		Literal.Parent(Core::FormatItemKind::IndentScope, Inner),
		Literal.Leaf("", Core::FormatItemKind::HardBreak),
		Literal.Leaf(")", Core::FormatItemKind::Token)
	};
	Literal.Root = Literal.Parent(Core::FormatItemKind::Sequence, Parts);
	Literal.BuildDecisions();
	Literal.CloseWork();
	const std::shared_ptr<const LaidOutCode> Code =
	std::make_shared<LaidOutCode>(Core::CodeLayout::Materialize(Core::LineBreaker(128).Resolve(std::move(Literal))));
	Check(
		OutputFinalizer::Finalize(OutputDocument::Compose(Code, CommentPlacementPlan(*Code))) == "f(\n\t\tx\n\tz\n)\n",
		"deferred indentation changed literal tabs or a verbatim continuation"
	);
	// 終了
	return;
}

/**
 * 配置枠毎のシリアライズ長を手記述の長さで照合する関数
 * 計算量：固定の入力の解析・投影・構築・配置の費用 Q に対し O(Q)
 * @throws FormatLimitExceeded 投影・構築・配置・合成が上限を超える場合
 * @throws std::runtime_error 配置枠・記法の契約又は合成の長さが一致しない場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::invalid_argument 指定された言語を呼出先が扱えない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
 */
static void VerifyComposedLen() {
	// 配置枠毎のシリアライズ長を手記述の長さで照合
	const std::shared_ptr<const Core::SyntaxModel> Syntax =
	std::make_shared<Core::SyntaxModel>(ProjectSrc(Lang::Get(Lang::Cpp), "int value = 1;\n"));
	Core::FormatIR Built = Core::Canonicalizer::Build(Syntax);
	Built.CloseWork();
	const std::shared_ptr<const LaidOutCode> Code =
	std::make_shared<LaidOutCode>(Core::CodeLayout::Materialize(Core::LineBreaker(128).Resolve(std::move(Built))));
	Check(Code->GetFragments().size() == 1 && Code->GetFragments()[0].Text == "int value = 1;", "size fixture changed");
	Core::FormatIR OtherBuilt = Core::Canonicalizer::Build(Syntax);
	OtherBuilt.CloseWork();
	const std::shared_ptr<const LaidOutCode> OtherCode =
	std::make_shared<LaidOutCode>(Core::CodeLayout::Materialize(Core::LineBreaker(128).Resolve(std::move(OtherBuilt))));
	// 別の所有者の計画とコードの合成の拒否
	bool HasRejectedOwner = false;
	try {
		OutputDocument::Compose(Code, CommentPlacementPlan(*OtherCode));
	} catch(const FormatLimitExceeded &E) {
		HasRejectedOwner = E.Kind == FailureReason::FormatterDefect;
	}
	Check(HasRejectedOwner, "composition accepted a plan from another output");
	for(
		const CommentSlotKind Kind :
		{
			CommentSlotKind::BeforeLine,
			CommentSlotKind::Inline,
			CommentSlotKind::LineEnd,
			CommentSlotKind::OwnLine,
			CommentSlotKind::DocEnd
		}
	) {
		const uint32_t Fragment = Kind == CommentSlotKind::DocEnd ? 1 : 0;
		std::optional<CommentSlotId> Slot;
		if(Kind == CommentSlotKind::Inline) for(uint32_t Offset = 0; Offset < 15 && !Slot; ++Offset) {
			Slot = Code->GetSlots().Find(Kind, Fragment, Offset);
		} else Slot = Code->GetSlots().Find(Kind, Fragment, 0);
		Check(Slot.has_value(), "size fixture lacks a slot");
		std::optional<CommentBlock> Comment = CommentBlock::Make(Syntax->GetRules().Notation, *Code, { "/*a", "b*/" }, *Slot);
		Check(Comment.has_value(), "size fixture comment was rejected");
		CommentPlacementPlan Plan(*Code);
		// 所有者と配置枠の不一致の拒否（拒否時はブロックの所有を渡さない）
		CommentPlacementPlan OtherPlan(*OtherCode);
		Check(!OtherPlan.IsInsertionSuccessful(*Slot, std::move(*Comment)), "insertion accepted a foreign owner");
		const std::optional<CommentSlotId> Changed =
		Code->GetSlots().Find(Kind == CommentSlotKind::BeforeLine ? CommentSlotKind::OwnLine : CommentSlotKind::BeforeLine, 0, 0);
		Check(Changed.has_value(), "size fixture lacks a different valid slot");
		Check(!Plan.IsInsertionSuccessful(*Changed, std::move(*Comment)), "insertion accepted a different slot");
		CommentSlotId Foreign = *Slot;
		Foreign.Owner = OtherCode->GetIdentity();
		Check(
			!CommentBlock::Make(Syntax->GetRules().Notation, *Code, { "/* note */" }, Foreign),
			"construction accepted a foreign slot"
		);
		Check(Plan.IsInsertionSuccessful(*Slot, std::move(*Comment)), "size fixture plan was rejected");
		Check(!Plan.IsInsertionSuccessful(*Slot, std::move(*Comment)), "insertion accepted a moved comment block");
		const OutputDocument Doc = OutputDocument::Compose(Code, std::move(Plan));
		bool HasExtraNewline = false;
		switch(Kind) {
		case CommentSlotKind::BeforeLine:
		case CommentSlotKind::OwnLine:
		case CommentSlotKind::DocEnd:
			HasExtraNewline = true;
			break;
		case CommentSlotKind::Inline:
		case CommentSlotKind::LineEnd:
			break;
		}
		const size_t Expected = HasExtraNewline + 21;
		Check(Doc.GetSize() == Expected, "composed size differs from slot serialization");
		Check(OutputFinalizer::Finalize(Doc).size() == Expected + 1, "serialized size differs from composition");
	}
	// コメントの前後の空の割当と，末尾だけの割当のシリアライズの照合
	Core::FormatIR Rows;
	Rows.NormalizedSyntax = Syntax;
	const std::vector<Core::FormatItemId> Parts {
		Rows.Leaf("a();", Core::FormatItemKind::Token),
		Rows.Leaf("", Core::FormatItemKind::HardBreak),
		Rows.Leaf("b();", Core::FormatItemKind::Token),
		Rows.Leaf("", Core::FormatItemKind::HardBreak),
		Rows.Leaf("c();", Core::FormatItemKind::Token)
	};
	Rows.Root = Rows.Parent(Core::FormatItemKind::Sequence, Parts);
	Rows.BuildDecisions();
	Rows.CloseWork();
	const std::shared_ptr<const LaidOutCode> RowsCode =
	std::make_shared<LaidOutCode>(Core::CodeLayout::Materialize(Core::LineBreaker(128).Resolve(std::move(Rows))));
	for(const CommentSlotKind Kind : { CommentSlotKind::DocEnd, CommentSlotKind::LineEnd }) {
		const uint32_t Fragment = Kind == CommentSlotKind::DocEnd ? 3 : 1;
		const std::optional<CommentSlotId> Slot = RowsCode->GetSlots().Find(Kind, Fragment, 0);
		Check(Slot.has_value(), "a sparse comment fixture lacks a slot");
		const std::vector<std::string> Lines { Kind == CommentSlotKind::DocEnd ? "/* note */" : " /* note */" };
		std::optional<CommentBlock> Comment = CommentBlock::Make(Syntax->GetRules().Notation, *RowsCode, Lines, *Slot);
		Check(Comment.has_value(), "a sparse comment fixture was rejected");
		CommentPlacementPlan Plan(*RowsCode);
		Check(Plan.IsInsertionSuccessful(*Slot, std::move(*Comment)), "a sparse comment insertion was rejected");
		const std::string_view Expected =
		Kind == CommentSlotKind::DocEnd ? "a();\nb();\nc();\n/* note */\n" : "a();\nb(); /* note */\nc();\n";
		const OutputDocument Doc = OutputDocument::Compose(RowsCode, std::move(Plan));
		Check(Doc.GetSize() + 1 == Expected.size(), "a sparse comment changed the composed output length");
		Check(OutputFinalizer::Finalize(Doc) == Expected, "a row without an insertion lost code or a later comment");
	}
	// 記法の成立・終端・ネストとトークン化前のデコードの照合
	struct NotationFixture {
		::Lang::ID Lang;
		CommentSlotKind Kind;
		std::vector<std::string> Lines;
		bool IsAccepted;
	};
	const NotationFixture Inputs[] = {
		{ Lang::Cpp, CommentSlotKind::BeforeLine, { "/* note", "tail */ // end" }, true },
		{ Lang::Cpp, CommentSlotKind::BeforeLine, { "/* note", "tail" }, false },
		{ Lang::Cpp, CommentSlotKind::BeforeLine, { "// note\nint extra;" }, false },
		{ Lang::Cpp, CommentSlotKind::BeforeLine, { "\t" }, false },
		{ Lang::Cpp, CommentSlotKind::BeforeLine, { "/* note", "tail */ int extra;" }, false },
		{ Lang::Cpp, CommentSlotKind::BeforeLine, { "/* outer /*", "inner */ tail */" }, false },
		{ Lang::Cpp, CommentSlotKind::BeforeLine, { "// note\\", "// tail" }, false },
		{ Lang::Cpp, CommentSlotKind::Inline, { "// note" }, false },
		{ Lang::Java, CommentSlotKind::BeforeLine, { "\\u002f\\u002a note", "tail \\u002a\\u002f" }, true },
		{ Lang::Java, CommentSlotKind::BeforeLine, { "// note\\u000a int extra;" }, false },
		{ Lang::Kotlin, CommentSlotKind::BeforeLine, { "/* outer /*", "inner */ tail */" }, true },
		{ Lang::Ruby, CommentSlotKind::BeforeLine, { "=begin", "note", "=end" }, true },
		{ Lang::Ruby, CommentSlotKind::BeforeLine, { " =begin", "note", "=end" }, false },
		{ Lang::Ruby, CommentSlotKind::BeforeLine, { "=begin", "note", "=ending" }, false },
		{ Lang::Ruby, CommentSlotKind::LineEnd, { "=begin", "note", "=end" }, false }
	};
	for(const NotationFixture &Input : Inputs) {
		std::optional<CommentSlotId> Slot;
		if(Input.Kind == CommentSlotKind::Inline) for(uint32_t Offset = 0; Offset < 15 && !Slot; ++Offset) {
			Slot = Code->GetSlots().Find(Input.Kind, 0, Offset);
		} else Slot = Code->GetSlots().Find(Input.Kind, 0, 0);
		Check(Slot.has_value(), "notation fixture lacks a slot");
		const std::optional<CommentBlock> Comment =
		CommentBlock::Make(GrammarProfile::Resolve(Lang::Get(Input.Lang)).GetRules().Notation, *Code, Input.Lines, *Slot);
		Check(Comment.has_value() == Input.IsAccepted, "comment notation acceptance differs from its lexical contract");
	}
	// 終了
	return;
}

/**
 * 空の出力と末尾データの正確な終端長を照合する関数
 * 計算量：固定の入力の解析・投影・構築・配置・合成の費用 Q に対し O(Q)
 * @throws FormatLimitExceeded 投影・構築・配置・合成が上限を超える場合
 * @throws std::runtime_error 空の出力又は末尾データに終端改行を補った場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::invalid_argument 指定された言語を呼出先が扱えない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
 */
static void VerifyEndingLen() {
	// 空の出力と末尾データの正確な終端長を照合
	// 空の出力と末尾データの終端長の比較条件
	struct Fixture {
		::Lang::ID Lang;
		std::string_view Src;
		size_t Bytes;
	};
	const Fixture Inputs[] = { { Lang::Cpp, "", 0 }, { Lang::Ruby, "__END__\nraw", 11 }, { Lang::Ruby, "__END__\nraw\n", 12 } };
	for(const Fixture &Input : Inputs) {
		const std::shared_ptr<const Core::SyntaxModel> Syntax =
		std::make_shared<Core::SyntaxModel>(ProjectSrc(Lang::Get(Input.Lang), std::string(Input.Src)));
		Core::FormatIR Built = Core::Canonicalizer::Build(Syntax);
		Built.CloseWork();
		const std::shared_ptr<const LaidOutCode> Code =
		std::make_shared<LaidOutCode>(Core::CodeLayout::Materialize(Core::LineBreaker(128).Resolve(std::move(Built))));
		const OutputDocument Doc = OutputDocument::CodeOnly(Code);
		Check(Doc.GetSize() == Input.Bytes, "the ending fixture has an incorrect composed length");
		Check(!Code->GetEnding().DoesEndWithNewline, "an empty or data-only output acquired a generated ending");
		Check(OutputFinalizer::Finalize(Doc) == Input.Src, "serialization changed the ending contract");
	}
	// 終了
	return;
}

/**
 * 終端込みの配置長が出力上限の前後で正しく受理される事の検査関数
 * 計算量：参照するテキストの出力バイト数 B と，固定の入力の投影費用 Q に対し O(B + Q)
 * @throws FormatLimitExceeded 入力の投影が作業予算を超える場合
 * @throws std::runtime_error 上限内の配置を拒み，又は上限超過の配置を受理した場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::invalid_argument 指定された言語を呼出先が扱えない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 * @throws std::out_of_range ノード又は共有範囲が参照先の表又はテキストに無い場合
 */
static void VerifyLayoutLimit() {
	// 同じ短いテキストの参照による，上限の直前・一致・直後の配置の照合
	const std::shared_ptr<const Core::SyntaxModel> Syntax =
	std::make_shared<Core::SyntaxModel>(ProjectSrc(Lang::Get(Lang::Cpp), ""));
	for(const size_t ByteCount : { MaxInputBytes - 1, MaxInputBytes, MaxInputBytes + 1 }) {
		Core::BrokenIR Fixture;
		Fixture.Expectation.NormalizedSyntax = Syntax;
		Fixture.Texts.assign(1024, 'x');
		for(size_t Remaining = ByteCount; Remaining;) {
			Core::ResolvedItem Piece;
			Piece.Text.Size = static_cast<uint32_t>(Remaining < Fixture.Texts.size() ? Remaining : Fixture.Texts.size());
			Fixture.Items.push_back(Piece);
			Remaining -= Piece.Text.Size;
		}
		bool HasExceededLimit = false;
		try {
			const LaidOutCode Code = Core::CodeLayout::Materialize(Fixture);
			Check(Code.GetFragments().size() == 1, "the size fixture changed its physical line count");
			Check(Code.GetFragments()[0].Text.size() == ByteCount, "the size fixture lost a shared text occurrence");
			Check(Code.GetEnding().DoesEndWithNewline, "the size fixture lost its required ending");
		} catch(const FormatLimitExceeded &E) {
			Check(
				E.Kind == FailureReason::LimitExceeded && E.Reason == "output exceeds size limit",
				"layout length failed for a reason other than its output limit"
			);
			HasExceededLimit = true;
		}
		Check(HasExceededLimit == ByteCount > MaxInputBytes - 1, "layout did not include its ending in the output limit");
	}
	// 終了
	return;
}

/**
 * 固定・可変・分割不能な区間の幅インデックスを照合する関数
 * 計算量：固定の原子数とテキスト長 N に対し O(N)
 * @throws FormatLimitExceeded 構築又は選択が作業予算を超える場合
 * @throws std::runtime_error 不要な区間の幅を計数又は重複してインデックス化する場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
 */
static void VerifyWidthRegions() {
	// 固定・可変・分割不能な区間の幅インデックスを照合
	Core::FormatIR Fixture;
	const std::vector<Core::FormatItemId> Fixed {
		Fixture.Leaf("a", Core::FormatItemKind::Token),
		Fixture.Leaf(" ", Core::FormatItemKind::SoftBreak),
		Fixture.Leaf("b", Core::FormatItemKind::Token)
	};
	const std::vector<Core::FormatItemId> Open { Fixture.Leaf("(", Core::FormatItemKind::Token) };
	const Core::FormatItemId ConditionalOpen = Fixture.Parent(Core::FormatItemKind::IfBroken, Open);
	Fixture.Items[ConditionalOpen].Group = 1;
	const std::vector<Core::FormatItemId> Close { Fixture.Leaf(")", Core::FormatItemKind::Token) };
	const Core::FormatItemId ConditionalClose = Fixture.Parent(Core::FormatItemKind::IfBroken, Close);
	Fixture.Items[ConditionalClose].Group = 1;
	const std::vector<Core::FormatItemId> Var {
		ConditionalOpen,
		Fixture.Leaf("x", Core::FormatItemKind::Token),
		Fixture.Leaf(" ", Core::FormatItemKind::SoftBreak),
		Fixture.Leaf("y", Core::FormatItemKind::Token),
		ConditionalClose
	};
	const std::string Literal(256, 'q');
	const std::vector<Core::FormatItemId> Rows {
		AddGroup(Fixture, 0, Fixed),
		Fixture.Leaf("", Core::FormatItemKind::HardBreak),
		AddGroup(Fixture, 1, Var),
		Fixture.Leaf("", Core::FormatItemKind::HardBreak),
		Fixture.Leaf(Literal, Core::FormatItemKind::Token)
	};
	Fixture.Root = Fixture.Parent(Core::FormatItemKind::Sequence, Rows);
	Fixture.BuildDecisions();
	const Core::LineBreakerMeasurement::Result Checked = Core::LineBreakerMeasurement::Run(Fixture, 3);
	Check(Render(Checked.Output) == "a b\nx y\n" + Literal, "disjoint width regions changed output");
	Check(Checked.Counts.PrefixEntries == 4, "a variable or unused region acquired a fixed prefix");
	Check(Checked.Counts.LexemeBytes == 8, "an unused region was measured");
	Check(Checked.Counts.GroupFinalizations == std::vector<size_t>(2, 1), "a region was not finalized once");
	// 終了
	return;
}

/**
 * 共有部分の必須改行の後の行幅を宣言区分へ反映する事の照合関数
 * 計算量：固定要素数に対し O(1)
 * @throws FormatLimitExceeded 中間表現の構築又は評価が作業予算を超える場合
 * @throws std::runtime_error 幅の境界又は共有部分の複写が一致しない場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
 */
static void VerifySharedDeclLines() {
	// 共有部分の必須改行の後の行幅を宣言区分へ反映する事の照合
	Core::FormatIR Fixture;
	const std::vector<Core::FormatItemId> Empty;
	const Core::FormatItemId Val = Fixture.Parent(Core::FormatItemKind::Sequence, Empty);
	std::vector<Core::FormatItemId> Parts {
		Fixture.Leaf("x\nlong", Core::FormatItemKind::Verbatim),
		Core::NoItem,
		Fixture.Leaf(";", Core::FormatItemKind::Token),
		Fixture.Leaf(", ", Core::FormatItemKind::Token),
		Fixture.Leaf("", Core::FormatItemKind::HardBreak)
	};
	for(const std::string_view Name : { "a", "b" }) {
		const std::vector<Core::FormatItemId> Unit { Fixture.Leaf(Name, Core::FormatItemKind::Token), Val };
		Parts.push_back(Fixture.Parent(Core::FormatItemKind::Sequence, Unit));
	}
	Fixture.Root = Fixture.Parent(Core::FormatItemKind::DeclarationSplit, Parts);
	Fixture.Items[Fixture.Root].Node = 1;
	Fixture.BuildDecisions();
	// 幅０でも不変条件を検査し，幅用の要約と空走査を作らない事の照合
	Check(Core::LineBreakerMeasurement::IsUnlimitedPartition(Fixture, 2), "unlimited declarations performed unused width work");
	for(size_t Invalid = 0; Invalid < 4; ++Invalid) {
		Core::FormatIR Broken = Fixture;
		if(!Invalid) Broken.Items[Broken.Root].Node = Core::NoNode;
		else if(Invalid == 1) Broken.Items[Broken.Root].Children.Size = 5; // 共有部分だけで宣言子が無い場合
		else Broken.Items[Parts[Invalid + 3]].Children.Size = 1; // 第１又は第２の宣言子の組が不完全な場合
		bool IsRejected = false;
		try {
			Core::LineBreakerMeasurement::IsUnlimitedPartition(Broken, 2);
		} catch(const FormatLimitExceeded &E) {
			IsRejected = E.Kind == FailureReason::FormatterDefect && E.Reason == "the format IR has an invalid declaration split";
		}
		Check(IsRejected, "an unlimited declaration bypassed its IR invariant checks");
	}
	for(const size_t Width : std::initializer_list<size_t>{ 7, 9, 0 }) {
		const Core::LineBreakerMeasurement::Result Checked = Core::LineBreakerMeasurement::Run(Fixture, Width);
		const bool IsSplit = Width == 7;
		Check(
			Checked.Counts.FlatPending.empty() && (Width ? Checked.Counts.FlatPending.capacity() : !Checked.Counts.FlatPending.capacity()),
			"flat measurement did not retain its completed scratch buffer or allocated it for unlimited width"
		);
		Check(
			Render(Checked.Output) == (IsSplit ? "x\nlonga;\nx\nlongb;" : "x\nlonga, b;"),
			"a shared mandatory line was measured as only its first line"
		);
		Check(
			Checked.Output.Expectation.DeclarationPartitions[0].Rows.size() == (IsSplit ? 2 : 1),
			"a shared mandatory line chose the wrong declaration partition"
		);
	}
	// 終了
	return;
}

/**
 * 共有する分割グループだけを宣言毎へコピーする事の照合関数
 * 計算量：固定要素数に対し O(1)
 * @throws FormatLimitExceeded 構築又は評価が作業予算を超える場合
 * @throws std::runtime_error コピーしたグループ・区分・出力又は確定回数が一致しない場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
 */
static void VerifyCopiedDeclGroups() {
	// 共有する分割グループだけを宣言毎へコピーする事の照合
	Core::FormatIR Fixture;
	const std::vector<Core::FormatItemId> HeadParts {
		Fixture.Leaf("x", Core::FormatItemKind::Token),
		Fixture.Leaf(" ", Core::FormatItemKind::SoftBreak),
		Fixture.Leaf("long", Core::FormatItemKind::Token)
	};
	std::vector<Core::FormatItemId> Parts {
		AddGroup(Fixture, 0, HeadParts),
		Core::NoItem,
		Fixture.Leaf(";", Core::FormatItemKind::Token),
		Fixture.Leaf(", ", Core::FormatItemKind::Token),
		Fixture.Leaf("", Core::FormatItemKind::HardBreak)
	};
	for(const std::string_view Name : { "a", "b" }) {
		const std::vector<Core::FormatItemId> Unit { Fixture.Leaf(Name, Core::FormatItemKind::Token), Core::NoItem };
		Parts.push_back(Fixture.Parent(Core::FormatItemKind::Sequence, Unit));
	}
	Fixture.Root = Fixture.Parent(Core::FormatItemKind::DeclarationSplit, Parts);
	Fixture.Items[Fixture.Root].Node = 1;
	Fixture.BuildDecisions();
	const Core::LineBreakerMeasurement::Result Split = Core::LineBreakerMeasurement::Run(Fixture, 7);
	Check(Split.Counts.DeclGroupVisits == 1, "a split declaration did not prepare its existing group once");
	Check(Render(Split.Output) == "x\nlonga;\nx\nlongb;", "copied declaration groups lost their independent choices");
	Check(Split.Counts.GroupFinalizations == std::vector<size_t>(2, 1), "copied groups were not finalized once per output row");
	const Core::LineBreakerMeasurement::Result Unlimited = Core::LineBreakerMeasurement::Run(Fixture, 0);
	Check(!Unlimited.Counts.DeclGroupVisits, "an unlimited declaration scanned unused copy groups");
	Check(
		Render(Unlimited.Output) == "x longa, b;" && Unlimited.Counts.GroupFinalizations == std::vector<size_t>(1, 1),
		"an unchanged declaration duplicated its shared group"
	);
	// 終了
	return;
}

/**
 * 相互依存する単位の共有部分だけを複写する宣言の照合関数
 * 計算量：固定要素数に対し O(1)
 * @throws FormatLimitExceeded 構築又は評価が作業予算を超える場合
 * @throws std::runtime_error 外部の採否・直接通知又はメンバの確定回数が一致しない場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
 */
static void VerifyPartialDeclUnit() {
	// 相互依存する単位の共有部分だけを複写する宣言の照合
	Core::FormatIR Fixture;
	const std::vector<Core::FormatItemId> HeadParts {
		AddConditional(Fixture, 0, "("),
		Fixture.Leaf("x", Core::FormatItemKind::Token),
		Fixture.Leaf(" ", Core::FormatItemKind::SoftBreak),
		Fixture.Leaf("long", Core::FormatItemKind::Token),
		AddConditional(Fixture, 0, ")")
	};
	const Core::FormatItemId Head = AddGroup(Fixture, 1, HeadParts);
	Fixture.Items[Head].Node = 1;
	Fixture.Items[Head].Operation = Core::FormOperation::BraceBody;
	std::vector<Core::FormatItemId> Parts {
		Head,
		Core::NoItem,
		Fixture.Leaf(";", Core::FormatItemKind::Token),
		Fixture.Leaf(", ", Core::FormatItemKind::Token),
		Fixture.Leaf("", Core::FormatItemKind::HardBreak)
	};
	for(const std::string_view Name : { "a", "b" }) {
		const std::vector<Core::FormatItemId> Unit { Fixture.Leaf(Name, Core::FormatItemKind::Token), Core::NoItem };
		Parts.push_back(Fixture.Parent(Core::FormatItemKind::Sequence, Unit));
	}
	const Core::FormatItemId Decl = Fixture.Parent(Core::FormatItemKind::DeclarationSplit, Parts);
	Fixture.Items[Decl].Node = 2;
	const std::vector<Core::FormatItemId> RootParts {
		Decl,
		Fixture.Leaf("", Core::FormatItemKind::SoftBreak),
		Fixture.Leaf("!", Core::FormatItemKind::Token)
	};
	Fixture.Root = AddGroup(Fixture, 0, RootParts);
	Fixture.BuildDecisions();
	const Core::LineBreakerMeasurement::Result Split = Core::LineBreakerMeasurement::Run(Fixture, 7);
	Check(Render(Split.Output) == "x\nlonga;\nx\nlongb;!", "a partial unit copy lost the external condition");
	Check(Split.Counts.GroupFinalizations == std::vector<size_t>(3, 1), "a partial unit copy finalized an original member twice");
	Check(
		Split.Counts.DependencyRegistrations == 1 && Split.Counts.DependencyNotifications == 1,
		"a partial unit copy lost its direct external dependency"
	);
	// 終了
	return;
}

/**
 * 見出の順位へ早まる本体と其の子及び外側の確定順の照合関数
 * 計算量：固定要素数に対し O(1)
 * @throws FormatLimitExceeded 構築又は評価が作業予算を超える場合
 * @throws std::runtime_error 直接依存・手記述の出力又は１度の確定が一致しない場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
 */
static void VerifyBraceDescendant() {
	// 見出と本体の間へ位置する呼出のグループの構築
	Core::FormatIR Fixture;
	const std::vector<Core::FormatItemId> HeaderParts {
		Fixture.Leaf("if(", Core::FormatItemKind::Token),
		Fixture.Leaf("", Core::FormatItemKind::SoftBreak),
		Fixture.Leaf("c)", Core::FormatItemKind::Token)
	};
	const std::vector<Core::FormatItemId> CallParts {
		Fixture.Leaf("f(", Core::FormatItemKind::Token),
		Fixture.Leaf("", Core::FormatItemKind::SoftBreak),
		Fixture.Leaf("a", Core::FormatItemKind::Token),
		Fixture.Leaf("", Core::FormatItemKind::SoftBreak),
		Fixture.Leaf(");", Core::FormatItemKind::Token)
	};
	const std::vector<Core::FormatItemId> BodyParts {
		AddConditional(Fixture, 1, "{"),
		Fixture.Leaf(" ", Core::FormatItemKind::SoftBreak),
		AddGroup(Fixture, 2, CallParts),
		Fixture.Leaf("", Core::FormatItemKind::SoftBreak),
		AddConditional(Fixture, 1, "}")
	};
	const Core::FormatItemId Body = AddGroup(Fixture, 1, BodyParts);
	Fixture.Items[Body].Node = 1;
	Fixture.Items[Body].Operation = Core::FormOperation::BraceBody;
	Fixture.EditDetail(Body).BodyParent = 0;
	const Core::FormatItemId Control =
	Fixture.Parent(Core::FormatItemKind::AnchorMark, std::vector<Core::FormatItemId>{ AddGroup(Fixture, 0, HeaderParts), Body });
	Fixture.Items[Control].Node = 0;
	Fixture.Root = AddGroup(Fixture, 3, std::vector<Core::FormatItemId>{ Control });
	Fixture.BuildDecisions();
	// 順位を早めた本体の待機と，外側の通常の親への待機の引継の照合
	for(const size_t Width : std::initializer_list<size_t>{ 0, 6, 9, 11, 128 }) {
		const Core::LineBreakerMeasurement::Result Checked = Core::LineBreakerMeasurement::Run(Fixture, Width);
		Check(
			Render(Checked.Output) == (Width && Width < 11 ? "if(c){\nf(a);\n}" : "if(c) f(a);"),
			"a brace unit evaluated its body before the ordinary child"
		);
		Check(
			Checked.Counts.DependencyRegistrations == 2 && Checked.Counts.DependencyNotifications == 2,
			"a brace descendant did not receive its two direct notifications"
		);
		Check(Checked.Counts.GroupFinalizations == std::vector<size_t>(4, 1), "a brace descendant was finalized twice");
	}
	// 終了
	return;
}

/**
 * 継続括弧・メンバ・コンテナの候補順位と確定回数の照合関数
 * 計算量：固定要素数に対し O(1)
 * @throws FormatLimitExceeded 中間表現の構築又は評価が作業予算を超える場合
 * @throws std::runtime_error 決定単位・テキスト又は確定回数が一致しない場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::invalid_argument 入力の言語を呼出先が扱えない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
 */
static void VerifyContinuationUnits() {
	// 接頭部を含む平坦形と逐語の末尾行の照合
	const Core::FormatIR Assignment = MakeContinuationFixture(false);
	for(const size_t Width : std::initializer_list<size_t>{ 22, 23, 24, 0 }) {
		const Core::LineBreakerMeasurement::Result Checked = Core::LineBreakerMeasurement::Run(Assignment, Width);
		Check(
			Render(Checked.Output) == (Width == 22 ? "body: str = (\naaaa + bbbb\n)" : "body: str = aaaa + bbbb"),
			"continuation omitted its prefix from the flat width"
		);
		Check(Checked.Counts.GroupFinalizations == std::vector<size_t>(3, 1), "continuation finalized a member twice");
	}
	// 未確定の % 後の境界を残して６文字の組を確定し，其の後に１３２文字の接続を測る
	const Core::FormatIR Verbatim = MakeContinuationFixture(true);
	for(const size_t Width : std::initializer_list<size_t>{ 128, 131, 132, 133, 0 }) {
		const Core::LineBreakerMeasurement::Result Checked = Core::LineBreakerMeasurement::Run(Verbatim, Width);
		const std::string Expected = Width && Width < 132 ?
		"x = (\n\"\"\"\n" + std::string(120, 'x') + "\"\"\" %\n(a, b)\n)" :
		"x = \"\"\"\n" + std::string(120, 'x') + "\"\"\" % (a, b)";
		Check(Render(Checked.Output) == Expected, "verbatim tail selected the wrong percent or tuple boundary");
		Check(Checked.Counts.GroupFinalizations == std::vector<size_t>(4, 1), "verbatim finalized a member twice");
	}
	// 受手・コンテナ・継続括弧の順位と，内包表記の既存の継続範囲の照合
	struct PythonFixture {
		std::string Src;
		std::string Expected;
		size_t Width;
	};
	for(
		const PythonFixture &Test :
		{
			PythonFixture{ "item2: typing.Receiver[int, str]\n", "item2: (\n\ttyping.Receiver[\n\t\tint,\n\t\tstr\n\t]\n)\n", 20 },
			PythonFixture{
				"result = registry.lookup(configuration_table[compute_primary_index(c, \"alpha_component\"), "
				"compute_secondary_index(c, \"beta_component\")])\n",
				"result = registry.lookup(\n\tconfiguration_table[compute_primary_index(c, \"alpha_component\"), "
				"compute_secondary_index(c, \"beta_component\")]\n)\n",
				128
			},
			PythonFixture{
				"cases = ((\"swift\", \"let v = a\" + \"\".join(\" ?? b%d * c%d << d%d\" % (index, index, index) "
				"for index in range(3000)) + \"\\n\"), (\"swift\", \"let v = a\" + \"\".join(\" ?? b%d * c%d << d%d\" % "
				"(index, index, index) for index in range(3000)) + \"\\nxxxxxxxxxxxxxxx\"))\n",
				"cases = (\n\t(\"swift\", \"let v = a\" + \"\".join(\" ?? b%d * c%d << d%d\" % (index, index, index) "
				"for index in range(3000)) + \"\\n\"),\n\t(\"swift\", \"let v = a\" + \"\".join(\" ?? b%d * c%d << d%d\" % "
				"(index, index, index) for index in range(3000)) + \"\\nxxxxxxxxxxxxxxx\")\n)\n",
				128
			},
			PythonFixture{ "value = aaaaaaaaaaaaaaaa.bbbbbbbbbbbbbbbb\n", "value = (\n\taaaaaaaaaaaaaaaa\n\t.bbbbbbbbbbbbbbbb\n)\n", 20 },
			PythonFixture{ "value = aaaaaaaaaaaaaaaa.bbbbbbbbbbbbbbbb\n", "value = aaaaaaaaaaaaaaaa.bbbbbbbbbbbbbbbb\n", 0 }
		}
	) {
		Core::SyntaxModel Syntax = ProjectPython(Test.Src);
		Core::SyntaxNormalizer().Normalize(Syntax);
		Core::FormatIR IR = Core::Canonicalizer::Build(std::make_shared<Core::SyntaxModel>(std::move(Syntax)));
		IR.CloseWork();
		const Core::LineBreakerMeasurement::Result Result = Core::LineBreakerMeasurement::Run(std::move(IR), Test.Width);
		Check(
			Result.Counts.GroupFinalizations == std::vector<size_t>(Result.Counts.GroupFinalizations.size(), 1),
			"a Python continuation member was not finalized once"
		);
		const std::shared_ptr<const LaidOutCode> Code = std::make_shared<LaidOutCode>(Core::CodeLayout::Materialize(Result.Output));
		Check(
			OutputFinalizer::Finalize(OutputDocument::Compose(Code, CommentPlacementPlan(*Code))) == Test.Expected,
			"Python continuation candidate order differs"
		);
	}
	// 終了
	return;
}

/**
 * 本体と見出の決定単位を３つの合法形で照合する関数
 * 計算量：固定要素数に対し O(1)
 * @param MaxChars 幅の上限
 * @param Expected 合法形の優先順位から導いたテキスト
 * @param Summaries 手順内で必要に為る候補要約の数
 * @param Updates 採用時に値が変わるインデックス葉の数
 * @throws FormatLimitExceeded 中間表現の構築又は評価が作業予算を超える場合
 * @throws std::runtime_error テキスト・選択状態又は手記述の計数が一致しない場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
 */
static void VerifyBraceUnit(
	const size_t MaxChars,
	const std::string_view Expected,
	const size_t Summaries,
	const size_t Updates
) {
	// 本体と見出の決定単位を３つの合法形で照合
	Core::FormatIR Fixture;
	const std::vector<Core::FormatItemId> HeaderParts {
		Fixture.Leaf("if(", Core::FormatItemKind::Token),
		Fixture.Leaf("", Core::FormatItemKind::SoftBreak),
		Fixture.Leaf("c)", Core::FormatItemKind::Token)
	};
	const Core::FormatItemId Header = AddGroup(Fixture, 0, HeaderParts);
	const std::vector<Core::FormatItemId> BodyParts {
		AddConditional(Fixture, 1, "{"),
		Fixture.Leaf("", Core::FormatItemKind::SoftBreak),
		Fixture.Leaf("b;", Core::FormatItemKind::Token),
		Fixture.Leaf("", Core::FormatItemKind::SoftBreak),
		AddConditional(Fixture, 1, "}")
	};
	const Core::FormatItemId Body = AddGroup(Fixture, 1, BodyParts);
	Fixture.Items[Body].Node = 1;
	Fixture.Items[Body].Operation = Core::FormOperation::BraceBody;
	Fixture.EditDetail(Body).BodyParent = 0;
	Fixture.Root = Fixture.Parent(Core::FormatItemKind::AnchorMark, std::vector<Core::FormatItemId>{ Header, Body });
	Fixture.Items[Fixture.Root].Node = 0;
	Fixture.BuildDecisions();
	const Core::LineBreakerMeasurement::Result Checked = Core::LineBreakerMeasurement::Run(Fixture, MaxChars);
	Check(Render(Checked.Output) == Expected, "brace unit chose the wrong legal form");
	Check(Checked.Counts.GroupFinalizations == std::vector<size_t>({ 1, 1 }), "brace unit finalized a member twice");
	Check(Checked.Counts.DecisionSummaries == Summaries, "brace unit evaluated a lower priority summary");
	Check(Checked.Counts.IdxUpdates == Updates, "brace unit changed its index while trying a form");
	Check(Checked.Counts.LexemeBytes == 9, "brace unit recounted or copied text");
	// 終了
	return;
}

/**
 * 固定区間と棄却した試行の計数を，手記述の期待で照合する関数
 * 計算量：固定グループ数 N に対し O(N)
 * @param Count 独立した固定グループの数
 * @param MaxChars 幅（３は採用，２は棄却，０は無制限）
 * @throws FormatLimitExceeded 中間表現の構築又は評価が作業予算を超える場合
 * @throws std::runtime_error テキスト・選択状態又は手記述の計数が一致しない場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
 */
static void VerifyFixedCounts(const size_t Count, const size_t MaxChars) {
	// 各グループは３テキスト断片と１境界，グループの間は必須改行
	Core::FormatIR Fixture;
	std::vector<Core::FormatItemId> Rows;
	for(uint32_t Group = 0; Group < Count; ++Group) {
		if(Group) Rows.push_back(Fixture.Leaf("", Core::FormatItemKind::HardBreak));
		const std::vector<Core::FormatItemId> Pieces {
			Fixture.Leaf("a", Core::FormatItemKind::Token),
			Fixture.Leaf(" ", Core::FormatItemKind::SoftBreak),
			Fixture.Leaf("b", Core::FormatItemKind::Token)
		};
		Rows.push_back(AddGroup(Fixture, Group, Pieces));
	}
	Fixture.Root = Fixture.Parent(Core::FormatItemKind::Sequence, Rows);
	Fixture.BuildDecisions();
	const Core::LineBreakerMeasurement::Result Checked = Core::LineBreakerMeasurement::Run(Fixture, MaxChars);
	const Core::LineBreakerMeasurement::Meter &Counts = Checked.Counts;
	Check(Counts.LexemeBytes == (MaxChars ? 3 * Count : 0), "fixed lexemes were recounted");
	Check(Counts.BoundaryEntries == Count, "fixed boundary registration repeated or omitted a position");
	Check(
		MaxChars ? Counts.Summaries && Counts.Summaries->Lexemes.size() == 3 * Count : !Counts.Summaries,
		"measurement cache was not shared or was created for width zero"
	);
	Check(!MaxChars || Counts.Summaries->OtherLexemes.empty(), "distinct text starts allocated hashed summaries");
	Check(Counts.AtomPreparationVisits == (MaxChars ? 2 : 1) * ((Count << 2) - 1), "atom preparation repeated a full pass");
	Check(
		!Counts.ContentGroups.capacity() && !Counts.AnnotationContent.capacity() && !Counts.EnclosedGroups.capacity() &&
		!Counts.InnerCandidates.capacity() && !Counts.InnerFlattened.capacity() && !Counts.ContinuationFlattened.capacity() &&
		!Counts.OutsideCandidates.capacity() && !Counts.ContinuationRun.capacity() && !Counts.ContinuationChanges.capacity() &&
		!Counts.ContinuationRanges.capacity(),
		"ordinary groups allocated continuation buffers"
	);
	Check(Counts.PrefixEntries == (MaxChars ? (Count << 2) - 1 : 0), "fixed prefix construction differs");
	Check(Counts.BoundaryVisits == (MaxChars ? Count : 0), "fixed boundary visits differ");
	Check(Counts.BoundaryUnlinks == (MaxChars == 3 ? Count : 0), "fixed boundary unlink count differs");
	Check(!Counts.IdxUpdates, "fixed trials updated an index or copied text");
	Check(!Counts.SummaryCompositions, "fixed widths were reaggregated");
	Check(Counts.Evaluations == Count, "a fixed group was evaluated more than once");
	Check(
		!Counts.TrialPositions.capacity() && !Counts.TrialMerged.capacity() && !Counts.TrialRemoved.capacity() &&
		!Counts.TrialAdded.capacity(),
		"fixed groups allocated unused trial buffers"
	);
	Check(Counts.DecisionSummaries == (MaxChars ? Count : 0), "fixed decision summary count differs");
	Check(!Counts.DependencyRegistrations && !Counts.DependencyNotifications, "fixed groups acquired dependencies");
	Check(Counts.GroupFinalizations == std::vector<size_t>(Count, 1), "a fixed group was not finalized once");
	for(const Core::SelectedForm &Form : Checked.Output.Expectation.SelectedForms) {
		Check(Form.IsBroken == (MaxChars == 2), "fixed width boundary differs");
	}
	// 終了
	return;
}

/**
 * 同じテキストを持つ葉の共有と，必須境界で棄却するグループの計数の照合関数
 * 計算量：葉の参照数 N と期待・出力の総バイト数 B に対し平均 O(N + B)
 * @throws FormatLimitExceeded 中間表現の構築又は評価が作業予算を超える場合
 * @throws std::runtime_error テキスト・選択状態又は手記述の計数が一致しない場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
 */
static void VerifyLexemeSharing() {
	// 同じテキストを持つ葉の共有と，必須境界で棄却するグループの計数の照合
	Core::FormatIR Shared;
	const Core::FormatItemId Token = Shared.Leaf("ab", Core::FormatItemKind::Token);
	const Core::FormatItemId Separator = Shared.Leaf(" ", Core::FormatItemKind::SoftBreak);
	const Core::FormatItemId Prefix = Shared.LeafRange({ Shared.Items[Token].Text.Begin, 1 }, Core::FormatItemKind::Token);
	const std::vector<Core::FormatItemId> Pieces { Token, Separator, Prefix, Separator, Token };
	Shared.Root = AddGroup(Shared, 0, Pieces);
	Shared.BuildDecisions();
	Core::LineBreakerMeasurement::Result Checked = Core::LineBreakerMeasurement::Run(Shared, 7);
	Check(Render(Checked.Output) == "ab a ab", "shared lexemes changed their output");
	Check(Checked.Counts.LexemeBytes == 4 && Checked.Counts.PrefixEntries == 5, "shared lexeme was recounted");
	Check(
		Checked.Counts.Summaries && Checked.Counts.Summaries->OtherLexemes.size() == 1,
		"only differing lengths at the same start should require a hashed summary"
	);
	// 解決済の観測結果を２度移しても，要約の所有者と長さの異なる３つの範囲の表を保つ
	const Core::LineBreakerMeasurement::Meter::SummaryCache *const Owner = Checked.Counts.Summaries.get();
	Core::LineBreakerMeasurement::Meter Moved;
	Moved.Summaries = std::make_unique<Core::LineBreakerMeasurement::Meter::SummaryCache>();
	Moved = std::move(Checked.Counts);
	Checked.Counts = std::move(Moved);
	Check(
		Owner && Checked.Counts.Summaries.get() == Owner && Owner->Lexemes.size() == 3,
		"moving measurements invalidated summaries"
	);
	Core::FormatIR Mandatory;
	const std::vector<Core::FormatItemId> MandatoryParts {
		Mandatory.Leaf("a", Core::FormatItemKind::Token),
		Mandatory.Leaf(" ", Core::FormatItemKind::SoftBreak),
		Mandatory.Leaf("", Core::FormatItemKind::HardBreak),
		Mandatory.Leaf("b", Core::FormatItemKind::Token),
		Mandatory.Leaf(" ", Core::FormatItemKind::SoftBreak),
		Mandatory.Leaf("c", Core::FormatItemKind::Token)
	};
	Mandatory.Root = AddGroup(Mandatory, 0, MandatoryParts);
	Mandatory.BuildDecisions();
	const Core::LineBreakerMeasurement::Result Rejected = Core::LineBreakerMeasurement::Run(Mandatory, 8);
	Check(Rejected.Output.Expectation.SelectedForms[0].IsBroken, "mandatory boundary was removed");
	Check(!Rejected.Counts.LexemeBytes && !Rejected.Counts.DecisionSummaries, "mandatory rejection measured an unused candidate");
	Check(!Rejected.Counts.IdxUpdates && !Rejected.Counts.BoundaryUnlinks, "mandatory rejection changed its retained boundaries");
	// 長い文字列を同じ幅要約で読む多参照の検査（行分割仕様 3.2，幅０では 3.3 に従い測らない）
	const std::string Text = "\"" + std::string(256, 'x') + "\"";
	for(const size_t MaxChars : { size_t(0), size_t(128) }) {
		size_t PrevWork = 0;
		for(uint32_t Count = 256; Count < 4096; Count <<= 1) {
			Core::FormatIR Repeated;
			const Core::FormatItemId Literal = Repeated.Leaf(Text, Core::FormatItemKind::Token);
			const Core::FormatItemId Separator = Repeated.Leaf(" ", Core::FormatItemKind::SoftBreak);
			std::vector<Core::FormatItemId> Refs;
			std::string Expected;
			for(uint32_t Idx = 0; Idx < Count; ++Idx) {
				if(Idx) {
					Refs.push_back(Separator);
					Expected.push_back(MaxChars ? '\n' : ' ');
				}
				Refs.push_back(Literal);
				Expected += Text;
			}
			Repeated.Root = AddGroup(Repeated, 0, Refs);
			Repeated.BuildDecisions();
			const Core::LineBreakerMeasurement::Result Expanded = Core::LineBreakerMeasurement::Run(std::move(Repeated), MaxChars);
			Check(Render(Expanded.Output) == Expected, "shared long literals changed their text or boundaries");
			Check(
				Expanded.Output.Expectation.SelectedForms[0].IsBroken == static_cast<bool>(MaxChars),
				"literal group chose the wrong form"
			);
			Check(Expanded.Counts.LexemeBytes == (MaxChars ? Text.size() + 1 : 0), "shared long literal or separator was recounted");
			if(MaxChars) {
				Check(
					Expanded.Counts.Summaries && Expanded.Counts.Summaries->Lexemes.size() == 2,
					"shared literal cache grew with the number of references"
				);
				Check(
					Expanded.Counts.Summaries->OtherLexemes.empty() && Expanded.Counts.Summaries->LexemeStarts.size() == 1,
					"repeated literal references allocated new lookup storage"
				);
			}
			Check(!PrevWork || Expanded.Counts.Used <= (PrevWork << 1) + 64, "shared literal references exceeded linear work growth");
			PrevWork = Expanded.Counts.Used;
		}
	}
	// 連続範囲・省いた本文・同じ範囲の再使用を倍増し，本文のコピー回数と行境界を手期待で照合する
	const std::shared_ptr<const Core::SyntaxModel> TextSyntax =
	std::make_shared<const Core::SyntaxModel>(ProjectSrc(Lang::Get(Lang::JSON), "0"));
	for(const unsigned TextMode : { 0U, 1U, 2U }) for(const size_t PartCount : { 16U, 32U, 64U }) {
		Core::BrokenIR TextIR;
		TextIR.Expectation.NormalizedSyntax = TextSyntax;
		std::string TextExpected;
		if(TextMode == 2) TextIR.Texts = "ab";
		for(size_t Part = 0; Part < PartCount; ++Part) {
			const uint32_t Offset = TextMode == 2 ? 0 : static_cast<uint32_t>(TextIR.Texts.size());
			if(TextMode != 2) TextIR.Texts += TextMode == 1 ? "a#b" : "ab";
			Core::ResolvedItem LeftPiece, RightPiece, LineEnd;
			LeftPiece.Text = { Offset, 1 };
			RightPiece.Text = { Offset + (TextMode == 1 ? 2U : 1U), 1 };
			LineEnd.Kind = Core::ResolvedKind::Newline;
			TextIR.Items.insert(TextIR.Items.end(), { LeftPiece, RightPiece, LineEnd });
			TextExpected += "ab\n";
		}
		std::pair<size_t, size_t> TextWork;
		const std::shared_ptr<const LaidOutCode> TextCode =
		std::make_shared<const LaidOutCode>(Core::CodeLayoutAccess::Materialize(TextIR, TextWork));
		Check(TextWork.first == PartCount << 1, "layout revisited a nonempty source range");
		Check(
			TextWork.second == (!TextMode ? 1 : TextMode == 1 ? PartCount + 1 : PartCount),
			"layout copied adjacent ranges separately or joined an omitted or repeated range"
		);
		Check(
			OutputFinalizer::Finalize(OutputDocument::Compose(TextCode, CommentPlacementPlan(*TextCode))) == TextExpected,
			"layout range composition changed text or line boundaries"
		);
	}
	// 終了
	return;
}

/**
 * 条件付断片の棄却試行がインデックスを変更しない事の照合関数
 * 計算量：固定要素数に対し O(1)
 * @throws FormatLimitExceeded 中間表現の構築又は評価が作業予算を超える場合
 * @throws std::runtime_error テキスト・選択状態又は手記述の計数が一致しない場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
 */
static void VerifyTrialCounts() {
	// 条件付断片の棄却試行がインデックスを変更しない事の照合
	Check(Core::LineBreakerMeasurement::IsTrialReadOnly(), "a rejected candidate modified a shared index");
	Check(
		Core::LineBreakerMeasurement::AreIndexUpdatesIndependent(),
		"registered-order index updates changed values or work counts"
	);
	// 空の条件付トークンは固定経路の対象外だが，採用差分にはテキストの変更が無い
	Core::FormatIR Fixture;
	const std::vector<Core::FormatItemId> Pieces {
		Fixture.Leaf("a", Core::FormatItemKind::Token),
		Fixture.Leaf(" ", Core::FormatItemKind::SoftBreak),
		Fixture.Leaf("b", Core::FormatItemKind::Token),
		AddConditional(Fixture, 0, "")
	};
	Fixture.Root = AddGroup(Fixture, 0, Pieces);
	Fixture.BuildDecisions();
	const Core::LineBreakerMeasurement::Result Rejected = Core::LineBreakerMeasurement::Run(Fixture, 2);
	Check(Render(Rejected.Output) == "a\nb", "rejected trial chose a flat form");
	Check(Rejected.Counts.LexemeBytes == 3, "trial lexeme counting differs");
	Check(!Rejected.Counts.IdxUpdates, "broken selection changed an unchanged index");
	Check(Rejected.Counts.SummaryCompositions == 1 && Rejected.Counts.DecisionSummaries == 1, "trial summary generation differs");
	Check(Rejected.Counts.GroupFinalizations == std::vector<size_t>{ 1 }, "trial finalized a group more than once");
	const Core::LineBreakerMeasurement::Result Accepted = Core::LineBreakerMeasurement::Run(Fixture, 3);
	Check(Render(Accepted.Output) == "a b", "accepted trial chose a broken form");
	Check(Accepted.Counts.IdxUpdates == 1, "accepted trial did not update its boundary once");
	Check(Accepted.Counts.DecisionSummaries == 1 && Accepted.Counts.SummaryCompositions == 1, "accepted trial was remeasured");
	// 終了
	return;
}

/**
 * 生成形式の直接依存の登録と１度の完了通知の照合関数
 * 計算量：固定要素数に対し O(1)
 * @throws FormatLimitExceeded 中間表現の構築又は評価が作業予算を超える場合
 * @throws std::runtime_error テキスト・選択状態又は手記述の計数が一致しない場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
 */
static void VerifyDependencyCounts() {
	// 生成形式の直接依存の登録と１度の完了通知の照合
	Core::FormatIR Fixture;
	const std::vector<Core::FormatItemId> Left {
		Fixture.Leaf("a", Core::FormatItemKind::Token),
		Fixture.Leaf(" ", Core::FormatItemKind::SoftBreak),
		Fixture.Leaf("b", Core::FormatItemKind::Token)
	};
	const Core::FormatItemId First = AddGroup(Fixture, 0, Left);
	const std::vector<Core::FormatItemId> Right {
		Fixture.Leaf("c", Core::FormatItemKind::Token),
		Fixture.Leaf(" ", Core::FormatItemKind::SoftBreak),
		Fixture.Leaf("d", Core::FormatItemKind::Token),
		AddConditional(Fixture, 1, "!")
	};
	const std::vector<Core::FormatItemId> Children { First, AddGroup(Fixture, 1, Right) };
	Fixture.Root = Fixture.Parent(Core::FormatItemKind::Sequence, Children);
	Fixture.BuildDecisions();
	const Core::LineBreakerMeasurement::Result Checked = Core::LineBreakerMeasurement::Run(Fixture, 4);
	Check(Render(Checked.Output) == "a bc\nd!", "dependency order differs");
	Check(
		!Checked.Counts.DependencyRegistrations && !Checked.Counts.DependencyNotifications,
		"direct dependency was not notified once"
	);
	Check(Checked.Counts.GroupFinalizations == std::vector<size_t>({ 1, 1 }), "dependency order finalized a group twice");
	Check(Checked.Counts.LexemeBytes == 7 && Checked.Counts.DecisionSummaries == 2, "dependency summary count differs");
	Check(
		Checked.Counts.SummaryCompositions == 3 && Checked.Counts.IdxUpdates == 2,
		"dependency selection did not update its boundary and conditional token once"
	);
	// 終了
	return;
}

/**
 * 条件付の依存が無い親子グループの直接評価の照合関数
 * 計算量：固定要素数に対し O(1)
 * @throws FormatLimitExceeded 構築又は評価が作業予算を超える場合
 * @throws std::runtime_error 依存表・通知・確定回数又はテキストが一致しない場合
 * @throws std::bad_alloc 入力又は中間表現のメモリを確保出来ない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
 */
static void VerifyIndependentParent() {
	// 条件付の依存が無い親子グループの直接評価の照合
	Core::FormatIR Fixture;
	const std::vector<Core::FormatItemId> InnerParts {
		Fixture.Leaf("a", Core::FormatItemKind::Token),
		Fixture.Leaf(" ", Core::FormatItemKind::SoftBreak),
		Fixture.Leaf("b", Core::FormatItemKind::Token)
	};
	const std::vector<Core::FormatItemId> OuterParts {
		Fixture.Leaf("p", Core::FormatItemKind::Token),
		Fixture.Leaf(" ", Core::FormatItemKind::SoftBreak),
		AddGroup(Fixture, 0, InnerParts),
		Fixture.Leaf(" ", Core::FormatItemKind::SoftBreak),
		Fixture.Leaf("q", Core::FormatItemKind::Token)
	};
	Fixture.Root = AddGroup(Fixture, 1, OuterParts);
	Fixture.BuildDecisions();
	const Core::LineBreakerMeasurement::Result Checked = Core::LineBreakerMeasurement::Run(Fixture, 0);
	Check(Render(Checked.Output) == "p a b q", "independent nesting changed the ordinary flat form");
	Check(
		!Checked.Counts.DependencyRegistrations && !Checked.Counts.DependencyNotifications,
		"ordinary nesting registered or notified a dependency"
	);
	Check(Checked.Counts.GroupFinalizations == std::vector<size_t>(2, 1), "an independent group was finalized twice");
	// 終了
	return;
}

/**
 * 後続グループへ依存する子と其の親の確定順を照合する関数
 * 計算量：固定要素数に対し O(1)
 * @throws FormatLimitExceeded 構築又は評価が作業予算を超える場合
 * @throws std::runtime_error 直接依存又は手記述の出力が一致しない場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
 */
static void VerifyDelayedParent() {
	// 後続グループへ依存する子と其の親の確定順を照合
	Core::FormatIR Fixture;
	const std::vector<Core::FormatItemId> ChildParts {
		Fixture.Leaf("aaaa", Core::FormatItemKind::Token),
		Fixture.Leaf(" ", Core::FormatItemKind::SoftBreak),
		Fixture.Leaf("b", Core::FormatItemKind::Token)
	};
	const Core::FormatItemId Child = AddGroup(Fixture, 0, ChildParts);
	const std::vector<Core::FormatItemId> ParentParts {
		Fixture.Leaf("P", Core::FormatItemKind::Token),
		Fixture.Leaf(" ", Core::FormatItemKind::SoftBreak),
		Child
	};
	const Core::FormatItemId Parent = AddGroup(Fixture, 1, ParentParts);
	const std::vector<Core::FormatItemId> ProviderParts {
		Fixture.Leaf("c", Core::FormatItemKind::Token),
		Fixture.Leaf(" ", Core::FormatItemKind::SoftBreak),
		Fixture.Leaf("d", Core::FormatItemKind::Token)
	};
	const Core::FormatItemId Provider = AddGroup(Fixture, 2, ProviderParts);
	const std::vector<Core::FormatItemId> Root { Parent, AddConditional(Fixture, 2, "!"), Provider };
	Fixture.Root = Fixture.Parent(Core::FormatItemKind::Sequence, Root);
	Fixture.BuildDecisions();
	const Core::LineBreakerMeasurement::Result Checked = Core::LineBreakerMeasurement::Run(Fixture, 5);
	Check(Render(Checked.Output) == "P\naaaa\nbc d", "a parent was evaluated before its delayed child");
	Check(
		Checked.Counts.DependencyRegistrations == 2 && Checked.Counts.DependencyNotifications == 2,
		"a delayed parent did not receive its two direct notifications"
	);
	Check(Checked.Counts.GroupFinalizations == std::vector<size_t>(3, 1), "a delayed group was finalized twice");
	// 終了
	return;
}

/**
 * 祖先の条件付断片と子の間の親子依存を同じ単位へ束ねる事の照合関数
 * 計算量：固定要素数に対し O(1)
 * @throws FormatLimitExceeded 構築又は評価が作業予算を超える場合
 * @throws std::runtime_error 中間の所属・出力又は確定回数が一致しない場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
 */
static void VerifyAncestorDependencyPath() {
	// 祖先の条件付断片と子の間の親子依存を同じ単位へ束ねる事の照合
	Core::FormatIR Fixture;
	const std::vector<Core::FormatItemId> InnerParts {
		Fixture.Leaf("a", Core::FormatItemKind::Token),
		Fixture.Leaf(" ", Core::FormatItemKind::SoftBreak),
		Fixture.Leaf("b", Core::FormatItemKind::Token)
	};
	const Core::FormatItemId Inner = AddGroup(Fixture, 0, InnerParts);
	const std::vector<Core::FormatItemId> MiddleParts { Fixture.Leaf("", Core::FormatItemKind::SoftBreak), Inner };
	const Core::FormatItemId Middle = AddGroup(Fixture, 1, MiddleParts);
	const std::vector<Core::FormatItemId> OuterParts {
		Fixture.Leaf("", Core::FormatItemKind::SoftBreak),
		Middle,
		AddConditional(Fixture, 2, "!"),
		Fixture.Leaf("", Core::FormatItemKind::SoftBreak)
	};
	Fixture.Root = AddGroup(Fixture, 2, OuterParts);
	Fixture.BuildDecisions();
	for(const size_t MaxChars : std::initializer_list<size_t>{ 0, 3 }) {
		const Core::LineBreakerMeasurement::Result Checked = Core::LineBreakerMeasurement::Run(Fixture, MaxChars);
		Check(Render(Checked.Output) == "a b", "an ancestor unit changed its flat form");
		Check(Checked.Counts.GroupFinalizations == std::vector<size_t>(3, 1), "an ancestor member was not finalized once");
	}
	// 終了
	return;
}

/**
 * 幅の境界と幅要約の計数テストの実行関数
 * 計算量：全テストケースの要素・候補差分数の総和 Q と期待・出力の総バイト数 B に対し平均 O(Q + B)
 * @return 検査の成否に応じた終了状態
 */
int main() {
	// 幅の境界と幅要約のテストケースの照合
	try {
		for(uint32_t CandidateCount = 256; CandidateCount < 4096; CandidateCount <<= 1) {
			Check(
				Core::LineBreakerMeasurement::HasSinglePassLexemeMeasurement(CandidateCount),
				"single-pass lexeme measurement changed its widths or work accounting"
			);
			Check(
				Core::LineBreakerMeasurement::HasLinearGroupNodeCollection(CandidateCount),
				"shared group node collection changed its order or exceeded linear work"
			);
			Check(Core::LineBreakerMeasurement::IsBracePreparationLimited(CandidateCount), "brace preparation was not charged");
			Check(
				Core::LineBreakerMeasurement::HasLinearLineEdgeQueries(CandidateCount),
				"line edge queries changed pending boundaries or exceeded linear work"
			);
		}
		VerifyZeroWidthScaling();
		VerifyIndentScaling();
		VerifyComposedLen();
		VerifyEndingLen();
		VerifyLayoutLimit();
		VerifyWidthRegions();
		VerifySharedDeclLines();
		VerifyCopiedDeclGroups();
		VerifyPartialDeclUnit();
		VerifyBraceDescendant();
		VerifyContinuationUnits();
		VerifyBraceUnit(8, "if(c)b;", 1, 3);
		VerifyBraceUnit(6, "if(c){\nb;\n}", 2, 3);
		VerifyBraceUnit(5, "if(\nc)b;", 4, 2);
		VerifyFixedCounts(1, 2);
		VerifyFixedCounts(1, 3);
		for(const size_t FixedCount : { 32U, 64U, 128U }) {
			VerifyFixedCounts(FixedCount, 3);
			VerifyFixedCounts(FixedCount, 0);
		}
		VerifyLexemeSharing();
		VerifyTrialCounts();
		VerifyDependencyCounts();
		VerifyIndependentParent();
		VerifyDelayedParent();
		VerifyAncestorDependencyPath();
		std::puts("counts: lexemes/summaries/updates/finalizations/dependencies");
	} catch(const std::exception &E) {
		std::fprintf(stderr, "line breaking reuse: %s\n", E.what());
		// 照合の中の例外で失敗とする事の返戻
		return 1;
	}
	// 全ての照合を通った事の返戻
	return 0;
}
