#pragma once

#include "FormatIR.hpp"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <unordered_map>
#include <vector>

namespace Core {
	// 行幅に依る選択を全て確定するクラス（整形仕様 3.1, 6.4，行分割仕様）
	class LineBreaker {
	private:

		friend struct LineBreakerComparison; // 基準手順との比較試験だけが非公開の切替を使う

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

		enum class GroupState : uint8_t { Pending, Broken, Flat }; // 群の状態（行分割仕様 3.2 の未評価・展開・平坦）

		struct EvaluationTrace {
			size_t Begin = 0; // 実際に参照した測定区間の先頭
			size_t End = 0; // 実際に参照した測定区間の末尾
		};

		struct EvaluationRecord {
			EvaluationTrace Trace; // 幅と残存境界を決めた要素の参照範囲
			size_t FirstVariable = 0; // 共有位置表の参照先群・条件付断片の開始番号
			size_t EndVariable = 0; // 共有位置表の参照先群・条件付断片の終了番号
			GroupState Result = GroupState::Pending; // 基準順での評価結果
			bool IsValid = false; // 再利用出来る結果が在るか
		};

		// 線形化した要素（測定区間の走査の単位）
		struct Atom {
			AtomKind Kind = AtomKind::Text; // 要素の種類
			GeneratedToken Generated = GeneratedToken::None; // 規則から生成した字句の種類
			bool HasNewline = false; // 改行を含む逐語か
			FormatRange Text; // 字面の範囲（選択可能な境界は平坦時の字面）
			uint32_t Width = 0; // 最初の行の文字数（選択可能な境界は平坦時の文字数）
			uint32_t TailWidth = 0; // 逐語の最後の行又は行端で選ぶ空白の表現の文字数
			FormatRange EdgeText; // 行端で選ぶ空白の表現の字面（中間表現の選択肢）
			uint32_t Group = NoGroup; // 選択可能な境界の所属する群
			uint32_t Condition = NoGroup; // 展開時だけ出す断片の従う群
			NodeId Node = NoNode; // 錨の節点（字句の葉と逐語区画は其の原子自身が錨を兼ね，空行の規則は前の宣言）
			uint32_t Detail = NoDetail; // 元の要素の属性の番号（句読点の所属・複写した共有部分の出現・空行の規則）
		};

		// 線形化の途中の状態
		struct Linearizer {
			std::vector<Atom> Atoms; // 線形化した要素
			std::vector<uint32_t> Enclosing; // 開いて居る群の構築時の番号
			std::unordered_map<uint32_t, uint32_t> Order; // 構築時の番号から後行順の番号への対応（複数の要素が共有する群は最後の要素の閉じの順）
			std::unordered_map<uint32_t, SelectedForm> Forms; // 構築時の群番号から全ての所属節点と構文操作への対応
			std::unordered_map<uint32_t, uint32_t> ExpansionDependencies; // 構築時の群番号で表す展開の依存先
			uint32_t Closed = 0; // 閉じた群の要素の数
		};

		// 幅で包装する本体と，其の直前の見出に属する分割群
		struct BraceChoice {
			uint32_t Body = NoGroup; // 波括弧を選ぶ本体の群
			std::vector<uint32_t> Headers; // 本体の直前に在る見出の群（後行順）
		};

		// 測定区間の境界の数の区間和（行分割仕様 3.2 の要約，巡毎に一度作り，群の評価毎に候補の間を走査しない）
		struct BoundaryCounter {
			std::vector<uint32_t> Tree; // 境界の数の Fenwick 木（添字は要素の位置と１の和）
			std::vector<uint8_t> IsCounted; // 要素を境界として数えて居るか
			uint32_t CountBefore(const size_t End) const; // 位置より前の境界の数の取得関数
			size_t Select(const uint32_t Rank) const; // 指定した順位の境界の位置の取得関数
			void Set(const size_t Position, const bool Counted); // 境界の採否の更新関数

			BoundaryCounter(const std::vector<Atom> &Atoms, const std::vector<uint8_t> &Assumed, const std::vector<GroupState> &States); // コンストラクタ（此の巡で見える境界を数える）
		};

		// 可視の字句の幅と残存境界の索引（行分割仕様 3.2 の区間の幅要約，群の評価毎に測定区間を走査しない）
		struct MeasureIndex {
			BoundaryCounter Boundaries; // 残存する境界
			std::vector<uint32_t> Tree;
			std::vector<uint32_t> Values; // 幅の Fenwick 木（添字は要素の位置と１の和）と各要素の現在の幅
			size_t WidthBefore(const size_t End) const; // 位置より前の可視字句の幅の取得関数
			void Set(const std::vector<Atom> &Atoms, const size_t Position, const bool Visible, const bool Boundary); // 要素の可視性と境界の採否の更新関数

			MeasureIndex(const std::vector<Atom> &Atoms, const std::vector<uint8_t> &Assumed, const std::vector<GroupState> &States); // コンストラクタ（可視の字句の幅と残存境界を数える）
		};

		// 行分割の作業単位の計上（整形仕様 9.2，初回評価と再評価の合計を入力の基準量から定めた上限で打ち切る）
		struct WorkMeter {
			size_t Used = 0; // 使った作業単位
			size_t Limit = 0; // 上限
			size_t Evaluations = 0; // 群の幅を実際に再評価した回数
			size_t LexemeRescanBytes = 0; // 不変字句を再走査したバイト数
			void Charge(const size_t Amount); // 作業単位の計上関数
		};

		const size_t MaxChars; // １行の最大文字数（構築時に一度だけ受け取り，０は無制限）
		static size_t FlatWidth(const FormatIR &IR, const FormatItemId Id, bool &HasBreak, WorkMeter &Work); // 平坦時の文字数の取得関数

		static FormatItemId SharedCopy(
			FormatIR &IR,
			const FormatItemId Id,
			std::unordered_map<uint32_t, uint32_t> &Groups,
			uint32_t &NextGroup
		); // 共有する部分の選択肢を保つ写しの取得関数

		static void MarkDeclarationOccurrence(FormatIR &IR, const FormatItemId Id, const NodeId Declaration, const uint32_t Row); // 複写した共有部分の出力上の出現の記録関数
		static void Linearize(const FormatIR &IR, const FormatItemId Id, const uint32_t Condition, Linearizer &State); // 中間表現の線形化関数
		static bool IsVisible(const Atom &Piece, const uint32_t Evaluated, const std::vector<uint8_t> &Assumed); // 断片を測定区間へ含めるかの判定関数
		static bool IsBoundary(const Atom &Piece, const uint32_t Evaluated, const std::vector<GroupState> &States); // 測定区間の境界かの判定関数

		static bool IsLineEdge(
			const std::vector<Atom> &Atoms,
			const size_t Position,
			const int Direction,
			const uint32_t Evaluated,
			const std::vector<GroupState> &States,
			const std::vector<uint8_t> &Assumed,
			EvaluationTrace *const Trace = nullptr
		); // 表示上の空白が行端へ接するかの判定関数

		void PartitionDeclaration(
			FormatIR &IR,
			const FormatItemId Id,
			std::vector<DeclarationPartition> &Partitions,
			uint32_t &NextGroup,
			WorkMeter &Work
		) const; // １つの統合宣言の区分関数

		void PartitionDeclarations(
			FormatIR &IR,
			const FormatItemId Id,
			std::vector<DeclarationPartition> &Partitions,
			uint32_t &NextGroup,
			WorkMeter &Work
		) const; // 統合宣言の区分関数

		bool CanFlatten(
			const std::vector<Atom> &Atoms,
			const std::vector<size_t> &Candidates,
			const std::span<const size_t> Conditionals,
			const uint32_t Evaluated,
			const std::vector<GroupState> &States,
			const std::vector<uint8_t> &Assumed,
			const BoundaryCounter *const Counter,
			MeasureIndex *const Index,
			WorkMeter &Work,
			EvaluationTrace *const Trace = nullptr
		) const; // 群の全候補を除けるかの評価関数

		void SelectBraceBody(
			const std::vector<Atom> &Atoms,
			const std::vector<std::vector<size_t>> &Candidates,
			const std::vector<std::vector<size_t>> &ConditionalsOf,
			const BraceChoice &Choice,
			std::vector<GroupState> &States,
			std::vector<uint8_t> &Conditions,
			MeasureIndex *const Index,
			WorkMeter &Work
		) const; // 制御本体と見出の表現を同じ条件付字句で評価する関数

		BrokenIR ResolveWithMode(
			FormatIR IR,
			const bool ShouldReuse,
			WorkMeter *const Measured,
			const size_t FingerprintMask = std::numeric_limits<size_t>::max()
		) const; // 比較試験と製品で共有する解決関数

	public:

		BrokenIR Resolve(FormatIR IR) const; // 選択の解決関数

		explicit LineBreaker(const size_t Chars); // コンストラクタ（行幅を受け取る唯一の口）
	};
} // namespace Core
