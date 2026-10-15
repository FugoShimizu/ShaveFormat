#include "LineBreaker.hpp"
#include "CoreObservation.hpp"
#include "FormatIR.hpp"
#include "NodeId.hpp"
#include "SyntaxModel.hpp"
#include "SyntaxRole.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include "../Util/ParserAllocator.hpp"
#include "../Util/TextMetrics.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
	#if defined(ShaveFormatRefFacts)

	constexpr bool ShouldReuseFlatSummaries = false; // 平坦形の要約を要素間で共有するか（テスト専用の基準側の構築は要素毎に求め直す）
	thread_local std::string_view RefText; // テスト専用の直接測定で読む現在のテキスト（基準側の構築だけが持つ）
	thread_local bool HasRefWidths = false; // テスト専用の直接測定で幅を求めるか（行幅上限が０なら不要）

	#else

	constexpr bool ShouldReuseFlatSummaries = true; // 平坦形の要約を要素間で共有するか

	#endif
} // namespace

namespace Core {
	/**
	 * 未測定の原子の初期化関数
	 * 計算量：O(1)
	 */
	LineBreaker::Atom::Atom() : HasNewline(false),
	HasBlankNewline(false),
	HasCodeNewline(false),
	IsActivatedWhenFlat(false),
	HasHeadContent(false),
	HasTailContent(false),
	HasVarWidth(false) {}

	/**
	 * 平坦形の行幅要約を合成する関数
	 * 計算量：O(1)
	 * @param Left 前の要約
	 * @param Right 後の要約
	 * @return 必須境界と各行の幅を保った要約
	 */
	LineBreaker::FlatSummary LineBreaker::JoinFlat(const FlatSummary &Left, const FlatSummary &Right) {
		// 平坦形の行幅要約を合成
		FlatSummary Result;
		Result.Head = Left.HasNewline ? Left.Head : Left.Head + Right.Head;
		Result.Tail = Right.HasNewline ? Right.Tail : Left.Tail + Right.Tail;
		Result.Max = std::max({ Left.Max, Right.Max, Left.Tail + Right.Head });
		Result.HasBreak = Left.HasBreak || Right.HasBreak;
		Result.HasNewline = Left.HasNewline || Right.HasNewline;
		// 合成した要約の返戻
		return Result;
	}

	/**
	 * 断片を測定区間へ含めるかの判定関数（行分割仕様 3.4）
	 * 評価中又は同時に試すグループの断片は平坦時の採否を用いる
	 * 展開形の試行差分に属する断片は展開時の採否を用い，其の他は採用済の選択を読む
	 * 計算量：試行に含めるグループ数 G に対し O(log(G + 1) + 1)
	 * @param Conditions 採用済の条件付トークンの選択
	 * @param Piece 線形化した要素
	 * @param Evaluated 評価中のグループ
	 * @param Flattened 同じ平坦形の試行へ含めるグループ（昇順）
	 * @param Expanded 展開形の条件付断片を読むグループ（平坦形の指定を優先し，NoGroup は変更無）
	 * @return 測定区間へ含めるなら true
	 */
	bool LineBreaker::IsVisible(
		const std::vector<uint8_t> &Conditions,
		const Atom &Piece,
		const uint32_t Evaluated,
		const std::span<const uint32_t> Flattened,
		const uint32_t Expanded
	) {
		// 採用済の選択へ試行差分を合成した可視性の返戻
		return Piece.Cond == NoGroup || Piece.IsActivatedWhenFlat != (
			Piece.Cond != Evaluated && (Piece.Cond == Expanded || Conditions[Piece.Cond]) &&
			!std::binary_search(Flattened.begin(), Flattened.end(), Piece.Cond)
		);
	}

	/**
	 * 測定区間の境界かの判定関数（行分割仕様 3.2）
	 * 必須改行・空行・所有領域・コードの改行を含む逐語テキストは展開を要求する境界とする
	 * 試行対象以外のグループの選択可能な境界は，平坦と確定する迄，測定区間の境界とする
	 * 計算量：試行に含めるグループ数 G に対し O(log(G + 1) + 1)
	 * @param States グループの状態
	 * @param Piece 線形化した要素
	 * @param Evaluated 評価中のグループ
	 * @param Flattened 同じ平坦形の試行へ含めるグループ（昇順）
	 * @return 境界なら true
	 */
	bool LineBreaker::IsBoundary(
		const std::vector<GroupState> &States,
		const Atom &Piece,
		const uint32_t Evaluated,
		const std::span<const uint32_t> Flattened
	) {
		// 測定区間の境界の判定
		switch(Piece.Kind) {
		case AtomKind::Hard:
		case AtomKind::Blank:
		case AtomKind::OwnedRegion:
			// 必須改行と空行と行を跨ぐ所有領域の返戻
			return true;
		case AtomKind::Verbatim:
			// コードの改行を含む逐語の返戻
			return Piece.HasCodeNewline;
		case AtomKind::Soft:
			// 未評価又は展開と決まった他のグループの境界の返戻
			return Piece.Group != NoGroup && Piece.Group != Evaluated && States[Piece.Group] != GroupState::Flat &&
			!std::binary_search(Flattened.begin(), Flattened.end(), Piece.Group);
		default:
			// 測定区間の境界でない事の返戻
			return false;
		}
	}

	/**
	 * 展開を要求しない物理改行から測定区間を分ける関数
	 * コードの境界は変更せず，候補に接する物理行の端と候補内の物理改行だけを返す
	 * 計算量：記録した物理改行数 B と端を求める際の訪問数 K に対し O(log(B + 1) + K)
	 * @param Conditions 採用済の条件付トークンの選択
	 * @param Atoms 線形化した要素
	 * @param First 最初の候補
	 * @param Last 最後の候補
	 * @param Evaluated 評価中のグループ
	 * @param Flattened 同時に平坦形を測るグループ
	 * @param Expanded 展開形の条件付断片を読むグループ
	 * @param Left 左の残存境界（無い場合は原子数）
	 * @param Right 右の残存境界（無い場合は原子数）
	 * @param Work 物理改行の記録と訪問の計数先
	 * @return 候補内の物理改行の位置列（可視性は呼出側で照合する）
	 * @throws FormatLimitExceeded 境界の訪問が作業予算を超える場合
	 */
	std::span<const size_t> LineBreaker::GetTextBreaks(
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
	) {
		// 必要な物理改行を持たない場合の返戻
		if(Work.TextBreaks.empty()) return std::span<const size_t>();
		const std::vector<size_t>::const_iterator Begin = std::lower_bound(Work.TextBreaks.begin(), Work.TextBreaks.end(), First);
		const std::vector<size_t>::const_iterator End = std::upper_bound(Begin, Work.TextBreaks.cend(), Last);
		for(std::vector<size_t>::const_iterator At = Begin; At != Work.TextBreaks.begin();) {
			const size_t Pos = *--At;
			Work.Charge(1);
			if(Left != Atoms.size() && Pos <= Left) break;
			if(IsVisible(Conditions, Atoms[Pos], Evaluated, Flattened, Expanded)) {
				Left = Pos;
				break;
			}
		}
		for(std::vector<size_t>::const_iterator At = End; At != Work.TextBreaks.end(); ++At) {
			const size_t Pos = *At;
			Work.Charge(1);
			if(Pos >= Right) break;
			if(IsVisible(Conditions, Atoms[Pos], Evaluated, Flattened, Expanded)) {
				Right = Pos;
				break;
			}
		}
		// 候補間の物理改行の範囲の返戻
		return std::span<const size_t>(Work.TextBreaks).subspan(
			static_cast<size_t>(Begin - Work.TextBreaks.cbegin()),
			static_cast<size_t>(End - Begin)
		);
	}

	/**
	 * テキスト又は平坦境界の幅を持つかを判定する関数
	 * @param Piece 線形化した要素
	 * @return 幅を持つ要素なら true
	 */
	bool LineBreaker::HasWidth(const Atom &Piece) {
		// テキスト又は平坦境界の幅を持つかを判定
		switch(Piece.Kind) {
		case AtomKind::Text:
		case AtomKind::Separator:
		case AtomKind::Soft:
		case AtomKind::Verbatim:
		case AtomKind::EdgeSpace:
			// テキスト又は平坦境界の返戻
			return true;
		default:
			// 幅を持たない要素の返戻
			return false;
		}
	}

	/**
	 * 行分割の作業単位の計上関数（整形仕様 9.2）
	 * @param Amount 計上する作業単位
	 * @throws FormatLimitExceeded 評価の合計が上限を超える場合（規定したリソース上限の見送）
	 * @throws ProcessingInterrupted 書戻以外の処理の中断を要求済の場合
	 */
	void LineBreaker::WorkMeter::Charge(const size_t Amount) {
		ParserAllocator::CheckInterruption();
		// 上限超過の見送
		if(Amount > Limit - Used) throw FormatLimitExceeded("line wrapping exceeds formatting limit");
		Used += Amount;
		// 終了
		return;
	}

	/**
	 * トークンの行幅要約を直接計数する関数
	 * 計算量：UTF-8 バイト数 B に対し O(B)
	 * @param Text UTF-8 のトークンのテキスト
	 * @return 先頭行・末尾行・最大行の幅と改行の有無
	 * @throws FormatLimitExceeded トークン計数が作業予算を超える場合
	 */
	LineBreaker::LexemeSummary LineBreaker::WorkMeter::Count(const std::string_view Text) {
		// トークンの行幅要約の直接計数
		Charge(Text.size());
		LexemeBytes += Text.size();
		// トークンの直接測定の返戻
		return TextMetrics::MeasureLines(Text);
	}

	/**
	 * 不変トークンを必要に為った時に１度計数する関数
	 * 計算量：未計数の UTF-8 バイト数 B に対し償却 O(B + 1)，開始位置の枠の初回の準備は共有テキスト長 T に対し O(T)，既存の要約の取得は期待 O(1)
	 * @param IR 共有テキストの所有者
	 * @param Range 計数する共有範囲
	 * @return 改行の有無と各行の幅
	 * @throws FormatLimitExceeded トークン計数が作業予算を超える場合
	 * @throws std::out_of_range 共有範囲の開始位置がテキストの終端を超える場合
	 * @throws std::bad_alloc トークンの要約の共有表のメモリを確保出来ない場合
	 * @throws std::length_error トークンの要約の共有表が保持領域の最大長を超える場合
	 */
	LineBreaker::LexemeSummary LineBreaker::WorkMeter::Measure(const FormatIR &IR, const FormatRange Range) {
		// 必要に為った不変トークンの１度の計数
		#if defined(ShaveFormatRefFacts)
		// テスト専用のトークンの直接測定の返戻
		return Count(std::string_view(IR.Texts).substr(Range.Begin, Range.Size));
		#else
		if(!Summaries) Summaries = std::make_unique<SummaryCache>();
		constexpr size_t BlockSize = std::tuple_size_v<SummaryCache::LexemeBlock>; // 開始位置の枠を一括確保する単位
		if(Summaries->LexemeStarts.size() <= Range.Begin / BlockSize) Summaries->LexemeStarts.resize(IR.Texts.size() / BlockSize + 1);
		std::unique_ptr<SummaryCache::LexemeBlock> &Block = Summaries->LexemeStarts.at(Range.Begin / BlockSize);
		if(!Block) Block = std::make_unique<SummaryCache::LexemeBlock>();
		size_t &First = (*Block)[Range.Begin % BlockSize];
		if(!First) {
			Summaries->Lexemes.emplace_back(Range.Size, Count(std::string_view(IR.Texts).substr(Range.Begin, Range.Size)));
			First = Summaries->Lexemes.size();
		} else if(Summaries->Lexemes[First - 1].first == Range.Size) Charge(1);
		else {
			// 開始位置を共有しても長さが異なる範囲は，別の要約として１度だけ計数する
			const std::pair<std::pmr::unordered_map<uint64_t, size_t>::iterator, bool> Entry =
			Summaries->OtherLexemes.try_emplace(static_cast<uint64_t>(Range.Begin) << 32 | Range.Size);
			if(Entry.second) {
				Entry.first->second = Summaries->Lexemes.size();
				Summaries->Lexemes.emplace_back(Range.Size, Count(std::string_view(IR.Texts).substr(Range.Begin, Range.Size)));
			} else Charge(1);
			// 長さが異なる範囲の要約の返戻
			return Summaries->Lexemes[Entry.first->second].second;
		}
		// 開始位置から直接取得した要約の返戻
		return Summaries->Lexemes[First - 1].second;
		#endif
	}

	/**
	 * 位置より前の境界の数の取得関数
	 * 計算量：要素数 N に対し O(log N)
	 * @param End 数える範囲の終わり（此の位置を含まない）
	 * @return 先頭から位置の手前迄の境界の数
	 */
	uint32_t LineBreaker::BoundaryCounter::CountBefore(const size_t End) const {
		// 位置より前の境界の数の取得
		uint32_t Count = 0;
		for(size_t Idx = End; Idx; Idx -= Idx & ~Idx + 1) Count += Tree[Idx];
		// 境界の数の返戻
		return Count;
	}

	/**
	 * 指定した順位の境界の位置の取得関数
	 * 計算量：要素数 N に対し O(log N)
	 * @param Rank 先頭を０とする境界の順位
	 * @return 境界の位置
	 */
	size_t LineBreaker::BoundaryCounter::Select(const uint32_t Rank) const {
		// 指定した順位の境界の位置の取得
		size_t Pos = 0, Step = 1;
		while(Step <= Tree.size() >> 1) Step <<= 1;
		uint32_t Remaining = Rank + 1;
		for(; Step; Step >>= 1) if(Pos + Step < Tree.size() && Tree[Pos + Step] < Remaining) {
			Remaining -= Tree[Pos + Step];
			Pos += Step;
		}
		// 境界の位置の返戻
		return Pos;
	}

	/**
	 * 境界の格納領域を確保する関数
	 * 計算量：要素数 N に対し O(N)
	 * @param Count 線形化した要素数
	 * @throws std::bad_alloc 境界の個数と位置の配列のメモリを確保出来ない場合
	 * @throws std::length_error 配列の要素数が格納先の上限を超える場合
	 */
	LineBreaker::BoundaryCounter::BoundaryCounter(const size_t Count) : Tree(Count + 1, 0), IsCounted(Count, 0) {}

	/**
	 * 平坦試行の差分を除いた境界数の取得関数
	 * 計算量：要素数 N と差分数 K に対し O(log(N + K + 1))
	 * @param End 数える範囲の終わり（此の位置を含まない）
	 * @return 採用済の境界数から試行の差分を除いた数
	 */
	uint32_t LineBreaker::BoundaryProjection::CountBefore(const size_t End) const {
		// 平坦試行の差分を除いた境界数の取得
		const size_t Difference = static_cast<size_t>(std::lower_bound(Removed.begin(), Removed.end(), End) - Removed.begin());
		// 試行で残る境界数の返戻
		return Src.CountBefore(End) - static_cast<uint32_t>(Difference);
	}

	/**
	 * 平坦試行の差分を除いた順位の境界の取得関数
	 * 計算量：要素数 N と差分数 K に対し O(log(N + 1) * log(N + K + 1))
	 * @param Rank 先頭を０とする試行の境界順位
	 * @param Work インデックスと差分の要約の参照を計上する先
	 * @return 指定した順位に在る残存境界の位置
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 */
	size_t LineBreaker::BoundaryProjection::Select(const uint32_t Rank, WorkMeter &Work) const {
		// 平坦試行の差分を除いた順位の境界の取得
		size_t Begin = 0, End = Src.IsCounted.size();
		while(Begin < End) {
			const size_t Middle = std::midpoint(Begin, End);
			Work.Charge(2);
			if(CountBefore(Middle + 1) > Rank) End = Middle;
			else Begin = Middle + 1;
		}
		// 試行の境界位置の返戻
		return Begin;
	}

	/**
	 * 平坦試行で除く境界を１度対応付ける構築関数
	 * 計算量：候補数 K に対し O(K)
	 * @param Src 採用済の境界インデックス（外側の候補は此の差分を使う間未確定の儘）
	 * @param Candidates 平坦と試す括弧外の候補（昇順，重複無）
	 * @param Work 候補の検査を計上する先
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	LineBreaker::BoundaryProjection::BoundaryProjection(
		const BoundaryCounter &Src,
		const std::span<const size_t> Candidates,
		WorkMeter &Work
	) : Src(Src) {
		// 平坦試行で除く境界の対応付け
		for(const size_t Candidate : Candidates) {
			Work.Charge(1);
			if(Src.IsCounted[Candidate]) Removed.push_back(Candidate);
		}
		// 終了
		return;
	}

	/**
	 * 可視のトークンの幅と残存境界のインデックスの構築関数
	 * 計算量：要素数 N に対し O(N)
	 * @param States グループの状態
	 * @param Conditions 採用済の条件付トークンの選択
	 * @param NeedsWidth 幅インデックスが必要な原子位置
	 * @param Atoms 線形化した要素
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	LineBreaker::MeasureIdx::MeasureIdx(
		const std::vector<GroupState> &States,
		const std::vector<uint8_t> &Conditions,
		const std::span<const uint8_t> NeedsWidth,
		std::vector<Atom> &Atoms
	) : Boundaries(Atoms.size()), Content(0) {
		// 必須境界の位置は全区間で共有し，幅・行端差・内容は必要な可変区間だけに持つ
		if(Atoms.size() > UINT32_MAX) throw std::length_error("width index exceeds capacity");
		for(size_t Pos = 0; Pos < Atoms.size(); ++Pos) {
			if(NeedsWidth[Pos]) Positions.push_back(static_cast<uint32_t>(Pos));
			const Atom &Piece = Atoms[Pos];
			Boundaries.IsCounted[Pos] = IsVisible(Conditions, Piece, NoGroup) && IsBoundary(States, Piece, NoGroup);
			Boundaries.Tree[Pos + 1] += Boundaries.IsCounted[Pos];
			if(const size_t Parent = Pos + (Pos + 1 & ~Pos) + 1; Parent < Boundaries.Tree.size()) {
				Boundaries.Tree[Parent] += Boundaries.Tree[Pos + 1];
			}
		}
		Content = BoundaryCounter(Positions.size());
		Tree.assign(Positions.size() + 1, 0);
		Vals.assign(Positions.size(), 0);
		EdgeTree.assign(Positions.size() + 1, 0);
		EdgeVals.assign(Positions.size(), 0);
		for(size_t Local = 0; Local < Positions.size(); ++Local) {
			Atom &Piece = Atoms[Positions[Local]];
			Piece.WidthPos = static_cast<uint32_t>(Local);
			Piece.HasVarWidth = true;
			const bool IsPieceVisible = IsVisible(Conditions, Piece, NoGroup);
			Vals[Local] = IsPieceVisible && HasWidth(Piece) ? Piece.Width : 0;
			Content.IsCounted[Local] = static_cast<bool>(Vals[Local]);
			EdgeVals[Local] = IsPieceVisible && Piece.Kind == AtomKind::EdgeSpace ? static_cast<int64_t>(Piece.TailWidth) - Piece.Width : 0;
			Tree[Local + 1] += Vals[Local];
			Content.Tree[Local + 1] += Content.IsCounted[Local];
			EdgeTree[Local + 1] += EdgeVals[Local];
			if(const size_t Parent = Local + (Local + 1 & ~Local) + 1; Parent < Tree.size()) {
				Tree[Parent] += Tree[Local + 1];
				Content.Tree[Parent] += Content.Tree[Local + 1];
				EdgeTree[Parent] += EdgeTree[Local + 1];
			}
		}
		// 終了
		return;
	}

	/**
	 * 採用差分を木の添字へ元の訪問順で合算する関数
	 * 計算量：初回の列の確保は原子数 N に対し O(N)，其の後の加算は償却 O(1)
	 * @param Idx 木の添字
	 * @param Column 幅・境界・内容・行端差の順の列番号
	 * @param Amount 此の添字へ加える符号付の差分
	 * @throws std::bad_alloc 差分又は添字の列のメモリを確保出来ない場合
	 * @throws std::length_error 差分又は添字の列が要素数上限を超える場合
	 */
	void LineBreaker::MeasureIdx::AddChange(const size_t Idx, const size_t Column, const int64_t Amount) {
		// 初回に要る列だけを確保し，同じ添字への加算は元の訪問順で纏める
		std::vector<int64_t> &Buffer = Changes[Column];
		std::vector<uint8_t> &Flags = HasChanged[Column];
		if(Buffer.empty()) {
			const size_t Count = Column == 1 ? Boundaries.Tree.size() : Tree.size();
			Buffer.resize(Count);
			Flags.resize(Count);
		}
		if(!Flags[Idx]) {
			Flags[Idx] = 1;
			ChangedIndices[Column].push_back(Idx);
		}
		Buffer[Idx] += Amount;
		// 終了
		return;
	}

	/**
	 * 採用差分のインデックスを纏めて更新する関数
	 * 計算量：原子数 N と差分数 K，変更する木の添字数 U に対し O(N + K log(N + 1) + U)
	 * @param States 確定したグループの選択
	 * @param Conditions 確定した条件付トークン
	 * @param Atoms 線形化した要素
	 * @param Positions 採用に依って変わり得る位置（昇順，重複無）
	 * @param Work 実際に変更したインデックス葉と合成の計数先
	 * @throws FormatLimitExceeded インデックス更新が作業予算を超える場合
	 * @throws std::bad_alloc ノード毎の差分を格納するメモリを確保出来ない場合
	 */
	void LineBreaker::MeasureIdx::Apply(
		const std::vector<GroupState> &States,
		const std::vector<uint8_t> &Conditions,
		const std::vector<Atom> &Atoms,
		const std::vector<size_t> &Positions,
		WorkMeter &Work
	) {
		// 前回に触れた添字だけを空にし，列と差分の容量を保つ
		for(size_t Column = 0; Column < Changes.size(); ++Column) {
			for(const size_t Idx : ChangedIndices[Column]) {
				Changes[Column][Idx] = 0;
				HasChanged[Column][Idx] = 0;
			}
			ChangedIndices[Column].clear();
		}
		for(const size_t Pos : Positions) {
			const Atom &Piece = Atoms[Pos];
			const bool IsPieceVisible = IsVisible(Conditions, Piece, NoGroup);
			const bool IsPieceBoundary = IsPieceVisible && IsBoundary(States, Piece, NoGroup);
			const int64_t BoundaryChange = static_cast<int64_t>(IsPieceBoundary) - Boundaries.IsCounted[Pos];
			const size_t Local = Piece.WidthPos;
			const bool IsIndexed = Piece.HasVarWidth;
			const uint32_t Width = IsIndexed && IsPieceVisible && HasWidth(Piece) ? Piece.Width : 0;
			const int64_t WidthChange = IsIndexed ? static_cast<int64_t>(Width) - Vals[Local] : 0;
			const int64_t ContentChange = IsIndexed ? static_cast<int64_t>(static_cast<bool>(Width)) - Content.IsCounted[Local] : 0;
			const int64_t EdgeVal =
			IsIndexed && IsPieceVisible && Piece.Kind == AtomKind::EdgeSpace ? static_cast<int64_t>(Piece.TailWidth) - Piece.Width : 0;
			const int64_t EdgeChange = IsIndexed ? EdgeVal - EdgeVals[Local] : 0;
			Work.Charge(1);
			if(!WidthChange && !BoundaryChange && !ContentChange && !EdgeChange) continue;
			++Work.IdxUpdates;
			Boundaries.IsCounted[Pos] = IsPieceBoundary;
			if(BoundaryChange) for(size_t Idx = Pos + 1; Idx < Boundaries.Tree.size(); Idx += Idx & ~Idx + 1) {
				Work.Charge(1);
				AddChange(Idx, 1, BoundaryChange);
			}
			if(WidthChange || ContentChange || EdgeChange) {
				Vals[Local] = Width;
				Content.IsCounted[Local] = static_cast<bool>(Width);
				EdgeVals[Local] = EdgeVal;
				for(size_t Idx = Local + 1; Idx < Tree.size(); Idx += Idx & ~Idx + 1) {
					Work.Charge(1);
					if(WidthChange) AddChange(Idx, 0, WidthChange);
					if(ContentChange) AddChange(Idx, 2, ContentChange);
					if(EdgeChange) AddChange(Idx, 3, EdgeChange);
				}
			}
		}
		// 各セルの差分は集計済で互いに独立して居る為，初回登録の順で１度だけ反映する
		for(const size_t Idx : ChangedIndices[0]) {
			Work.Charge(1);
			const int64_t Change = Changes[0][Idx];
			if(!Change) continue;
			if(Change < 0) Tree[Idx] -= static_cast<size_t>(-Change);
			else Tree[Idx] += static_cast<size_t>(Change);
			++Work.SummaryCompositions;
		}
		for(const size_t Idx : ChangedIndices[1]) {
			Work.Charge(1);
			const int64_t Change = Changes[1][Idx];
			if(!Change) continue;
			if(Change < 0) Boundaries.Tree[Idx] -= static_cast<uint32_t>(-Change);
			else Boundaries.Tree[Idx] += static_cast<uint32_t>(Change);
		}
		for(const size_t Idx : ChangedIndices[2]) {
			Work.Charge(1);
			const int64_t Change = Changes[2][Idx];
			if(Change < 0) Content.Tree[Idx] -= static_cast<uint32_t>(-Change);
			else Content.Tree[Idx] += static_cast<uint32_t>(Change);
		}
		for(const size_t Idx : ChangedIndices[3]) {
			Work.Charge(1);
			const int64_t Change = Changes[3][Idx];
			EdgeTree[Idx] += Change;
			if(Change) ++Work.SummaryCompositions;
		}
		// 終了
		return;
	}

	/**
	 * 原子位置に対応する圧縮位置の取得関数
	 * 計算量：インデックス対象の原子数 N に対し O(log(N + 1))
	 * @param End 原子位置
	 * @return 此の位置より前のインデックス要素数
	 */
	size_t LineBreaker::MeasureIdx::GetLocalBefore(const size_t End) const {
		// 圧縮位置の返戻
		return static_cast<size_t>(std::lower_bound(Positions.begin(), Positions.end(), End) - Positions.begin());
	}

	/**
	 * 原子位置より前の内容数の取得関数
	 * 計算量：インデックス対象の原子数 N に対し O(log(N + 1))
	 * @param End 原子位置
	 * @return 正の幅を持つ内容数
	 */
	uint32_t LineBreaker::MeasureIdx::GetContentBefore(const size_t End) const {
		// 内容数の返戻
		return Content.CountBefore(GetLocalBefore(End));
	}

	/**
	 * 内容の順位から原子位置を引く関数
	 * 計算量：インデックス対象の原子数 N に対し O(log(N + 1))
	 * @param Rank 内容の零始まりの順位
	 * @return 元の原子位置
	 */
	size_t LineBreaker::MeasureIdx::GetContentAt(const uint32_t Rank) const {
		// 原子位置の返戻
		return Positions[Content.Select(Rank)];
	}

	/**
	 * 位置より前の可視トークンの幅の取得関数
	 * 計算量：要素数 N に対し O(log N)
	 * @param End 測る範囲の終わり
	 * @return 先頭から位置の手前迄の幅
	 */
	size_t LineBreaker::MeasureIdx::GetWidthBefore(const size_t End) const {
		// 位置より前の可視トークンの幅の取得
		size_t Width = 0;
		for(size_t At = GetLocalBefore(End); At; At -= At & ~At + 1) Width += Tree[At];
		// 幅の返戻
		return Width;
	}

	/**
	 * 位置より前の行端表現の幅の差の取得関数
	 * 計算量：要素数 N に対し O(log N)
	 * @param End 取得する範囲の終端
	 * @return 行端表現と行内表現の幅の差の総和
	 */
	int64_t LineBreaker::MeasureIdx::GetEdgeBefore(const size_t End) const {
		// 位置より前の行端表現の幅の差の取得
		int64_t Difference = 0;
		for(size_t Pos = GetLocalBefore(End); Pos; Pos -= Pos & ~Pos + 1) Difference += EdgeTree[Pos];
		// 行端差の返戻
		return Difference;
	}

	/**
	 * 既存区間の幅と行端差から候補幅を読取専用で合成する関数
	 * 計算量：要素数 N と候補差分数 K に対し O((K + 1) log(N + K + 1))
	 * @param Conditions 採用済の条件付トークンの選択
	 * @param Atoms テキストと行端表現
	 * @param Begin 測定区間の始端
	 * @param End 測定区間の終端
	 * @param HasLeftContent 左の逐語境界に同じ行の内容が残るか
	 * @param HasRightContent 右の逐語境界に同じ行の内容が残るか
	 * @param Positions 候補差分の位置（昇順，重複無）
	 * @param Evaluated 平坦形を測るグループ
	 * @param Flattened 同時に平坦形を測るグループ
	 * @param Expanded 展開形の条件付断片を読むグループ（平坦形の指定を優先し，NoGroup は変更無）
	 * @param Work 区間要約と候補差分の計数先
	 * @return 行端の空白を行端用のテキストへ置き換えた幅
	 * @throws FormatLimitExceeded 要約合成が作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	size_t LineBreaker::MeasureIdx::ComputeTrialWidth(
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
	) const {
		// 既存区間の幅と行端差からの候補幅の合成
		size_t Width = GetWidthBefore(End) - GetWidthBefore(Begin);
		Work.Charge(2);
		++Work.DecisionSummaries;
		++Work.SummaryCompositions;
		std::vector<size_t> &Removed = Work.WidthRemoved, &Added = Work.WidthAdded;
		Removed.clear();
		Added.clear();
		for(const size_t Pos : Positions) if(Begin <= Pos && Pos < End) {
			Work.Charge(1);
			const Atom &Piece = Atoms[Pos];
			if(!Piece.HasVarWidth) continue;
			const size_t Local = Piece.WidthPos;
			const size_t Val = HasWidth(Piece) && IsVisible(Conditions, Piece, Evaluated, Flattened, Expanded) ? Piece.Width : 0;
			if(Val != Vals[Local]) {
				Width -= Vals[Local];
				Width += Val;
				++Work.SummaryCompositions;
			}
			if(Content.IsCounted[Local] && !Val) Removed.push_back(Pos);
			else if(!Content.IsCounted[Local] && Val) Added.push_back(Pos);
		}
		const uint32_t Before = GetContentBefore(Begin), After = GetContentBefore(End);
		Work.Charge(2);
		size_t First = End, Last = End;
		for(uint32_t Rank = Before; Rank < After; ++Rank) {
			const size_t Pos = GetContentAt(Rank);
			Work.Charge(1);
			if(!std::binary_search(Removed.begin(), Removed.end(), Pos)) {
				First = Pos;
				break;
			}
		}
		for(uint32_t Rank = After; Rank > Before;) {
			const size_t Pos = GetContentAt(--Rank);
			Work.Charge(1);
			if(!std::binary_search(Removed.begin(), Removed.end(), Pos)) {
				Last = Pos;
				break;
			}
		}
		for(const size_t Pos : Added) {
			First = std::min(First, Pos);
			Last = Last == End ? Pos : std::max(Last, Pos);
		}
		// 測定区間の行端表現の幅の差分の合成
		const auto EdgeSum = [&](const size_t Start, const size_t Finish) -> int64_t {
			int64_t Difference = GetEdgeBefore(Finish) - GetEdgeBefore(Start);
			Work.Charge(2);
			for(const size_t Pos : Positions) if(Start <= Pos && Pos < Finish) {
				Work.Charge(1);
				const Atom &Piece = Atoms[Pos];
				const int64_t Val = Piece.Kind == AtomKind::EdgeSpace && IsVisible(Conditions, Piece, Evaluated, Flattened, Expanded) ?
				static_cast<int64_t>(Piece.TailWidth) - Piece.Width :
				0;
				if(Piece.HasVarWidth) Difference += Val - EdgeVals[Piece.WidthPos];
			}
			// 候補差分を含む行端差の返戻
			return Difference;
		};
		int64_t Difference = 0;
		if(First != End) {
			if(!HasLeftContent) Difference += EdgeSum(Begin, First + 1);
			if(!HasRightContent) Difference += EdgeSum(Last, End);
			if(!HasLeftContent && !HasRightContent && First == Last) Difference -= EdgeSum(First, First + 1);
		} else if(!HasLeftContent || !HasRightContent) Difference = EdgeSum(Begin, End);
		if(Difference < 0) Width -= static_cast<size_t>(-Difference);
		else Width += static_cast<size_t>(Difference);
		// 行端差を合成した幅の返戻
		return Width;
	}

	/**
	 * 固定区間の累積値と残存境界を構築する関数
	 * 計算量：原子数 N に対し O(N)
	 * @param States グループの未評価状態
	 * @param IsFixed 幅を参照する固定区間
	 * @param Atoms 線形化した固定要素
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	LineBreaker::FixedMeasure::FixedMeasure(
		const std::vector<GroupState> &States,
		const std::span<const uint8_t> IsFixed,
		std::vector<Atom> &Atoms
	) {
		// 固定区間の累積値と残存境界の構築
		if(Atoms.size() > UINT32_MAX - 2) throw std::length_error("fixed width index exceeds capacity");
		for(size_t Pos = 0; Pos < Atoms.size(); ++Pos) {
			if(
				const Atom &Piece = Atoms[Pos];
				IsFixed[Pos] || Pos + 1 < Atoms.size() && IsFixed[Pos + 1] && Piece.Cond == NoGroup && Piece.Kind != AtomKind::Soft &&
				IsBoundary(States, Piece, NoGroup)
			) Positions.push_back(static_cast<uint32_t>(Pos));
		}
		Prefix.assign(Positions.size() + 1, 0);
		Prev.assign(Positions.size() + 2, 0);
		Next.assign(Positions.size() + 2, static_cast<uint32_t>(Positions.size() + 1));
		size_t Last = 0;
		for(size_t Local = 0; Local < Positions.size(); ++Local) {
			Atom &Piece = Atoms[Positions[Local]];
			Piece.WidthPos = static_cast<uint32_t>(Local);
			Prefix[Local + 1] = Prefix[Local] + (HasWidth(Piece) ? Piece.Width : 0);
			if(Piece.Cond == NoGroup && IsBoundary(States, Piece, NoGroup)) {
				Prev[Local + 1] = static_cast<uint32_t>(Last);
				Next[Last] = static_cast<uint32_t>(Local + 1);
				Last = Local + 1;
			}
		}
		Next[Last] = static_cast<uint32_t>(Positions.size() + 1);
		Prev[Positions.size() + 1] = static_cast<uint32_t>(Last);
		// 終了
		return;
	}

	/**
	 * 固定区間の全候補を読取専用で判定する関数
	 * 計算量：候補数 K と物理改行数 B に対し O(K + B + log(B + 1))
	 * @param Conditions 採用済の条件付トークンの選択
	 * @param Atoms 固定テキストを持つ原子
	 * @param Candidates 構文順の候補境界
	 * @param MaxChars 行幅上限 (>0)
	 * @param Work 境界の訪問と候補要約の計数先
	 * @return 境界が連続し，結合後の幅が上限内なら true
	 * @throws FormatLimitExceeded 固定区間の判定が作業予算を超える場合
	 */
	bool LineBreaker::FixedMeasure::CanFlatten(
		const std::vector<uint8_t> &Conditions,
		const std::vector<Atom> &Atoms,
		const std::span<const size_t> Candidates,
		const size_t MaxChars,
		WorkMeter &Work
	) const {
		// 候補の無いグループの返戻
		if(Candidates.empty()) return true;
		Work.Charge(1);
		++Work.BoundaryVisits;
		for(size_t Idx = 1; Idx < Candidates.size(); ++Idx) {
			Work.Charge(1);
			++Work.BoundaryVisits;
			// 他の残存境界を挟む事の返戻
			if(Next[Atoms[Candidates[Idx - 1]].WidthPos + 1] != Atoms[Candidates[Idx]].WidthPos + 1) return false;
		}
		Work.Charge(2);
		++Work.DecisionSummaries;
		const size_t Before = Prev[Atoms[Candidates.front()].WidthPos + 1], After = Next[Atoms[Candidates.back()].WidthPos + 1];
		const size_t LeftBoundary = Before ? Positions[Before - 1] : Atoms.size();
		const size_t RightBoundary = After <= Positions.size() ? Positions[After - 1] : Atoms.size();
		size_t Left = LeftBoundary, Right = RightBoundary;
		const std::span<const size_t> TextBreaks = GetTextBreaks(
			Conditions,
			Atoms,
			Candidates.front(),
			Candidates.back(),
			NoGroup,
			std::span<const uint32_t>(),
			NoGroup,
			Left,
			Right,
			Work
		);
		// 不変の物理行の要約を候補の境界で接続する
		const auto Fits = [&](const size_t Begin, const size_t End) -> bool {
			// 外側の境界は可変インデックスとも共有する為，固定表の位置を使う
			const size_t Start = Begin == Atoms.size() ? 0 : Begin == LeftBoundary ? Before : Atoms[Begin].WidthPos + 1;
			const size_t Finish = End == Atoms.size() ? Positions.size() : End == RightBoundary ? After - 1 : Atoms[End].WidthPos;
			size_t Width = Prefix[Finish] - Prefix[Start];
			if(Begin != Atoms.size() && Atoms[Begin].Kind == AtomKind::Verbatim) Width += Atoms[Begin].TailWidth;
			if(End != Atoms.size() && Atoms[End].Kind == AtomKind::Verbatim) Width += Atoms[End].Width;
			// 接続した物理行が上限内である事の返戻
			return Width <= MaxChars;
		};
		for(const size_t Pos : TextBreaks) {
			Work.Charge(3);
			++Work.DecisionSummaries;
			// 接続した物理行が上限を超える場合の返戻
			if(!Fits(Left, Pos)) return false;
			Left = Pos;
		}
		// 最後の物理行が上限内に収まる事の返戻
		return Fits(Left, Right);
	}

	/**
	 * 採用した固定境界列を１度切り離す関数
	 * 計算量：O(1)
	 * @param Atoms 圧縮位置を持つ原子
	 * @param Candidates 連続する事を判定済の候補境界
	 */
	void LineBreaker::FixedMeasure::Remove(const std::vector<Atom> &Atoms, const std::span<const size_t> Candidates) {
		// 候補の無いグループの終了
		if(Candidates.empty()) return;
		const size_t Left = Prev[Atoms[Candidates.front()].WidthPos + 1], Right = Next[Atoms[Candidates.back()].WidthPos + 1];
		Next[Left] = Right;
		Prev[Right] = Left;
		// 終了
		return;
	}

	/**
	 * 表示上の空白が行端へ接するかの判定関数
	 * 計算量：要素数 N と試行に含めるグループ数 G に対し最悪 O(N * (log(G + 1) + 1))
	 * @param States グループの選択
	 * @param Conditions 条件付断片の選択
	 * @param Atoms 線形化した要素
	 * @param Pos 空白の位置
	 * @param Direction 探索方向（前は -1，後は 1）
	 * @param Evaluated 平坦化を試すグループ（出力時は無）
	 * @param Work 隣接要素の走査の計数先
	 * @param Flattened 境界と条件付断片を平坦形として扱うグループ（昇順）
	 * @param Expanded 展開形の条件付断片を読むグループ（平坦形の指定を優先し，NoGroup は変更無）
	 * @return 指定方向に同じ行の内容が無ければ `true`
	 * @throws FormatLimitExceeded 隣接走査又はテスト専用のトークン計数が作業予算を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	bool LineBreaker::IsLineEdge(
		const std::vector<GroupState> &States,
		const std::vector<uint8_t> &Conditions,
		const std::vector<Atom> &Atoms,
		const size_t Pos,
		const int Direction,
		const uint32_t Evaluated,
		WorkMeter &Work,
		const std::span<const uint32_t> Flattened,
		const uint32_t Expanded
	) {
		// 表示上の空白と行端の接触の判定
		for(
			std::ptrdiff_t Idx = static_cast<std::ptrdiff_t>(Pos) + Direction;
			Idx > -1 && Idx < static_cast<std::ptrdiff_t>(Atoms.size());
			Idx += Direction
		) {
			Work.Charge(1);
			const Atom &Neighbor = Atoms[static_cast<size_t>(Idx)];
			if(!IsVisible(Conditions, Neighbor, Evaluated, Flattened, Expanded)) continue;
			if(Neighbor.HasNewline || IsBoundary(States, Neighbor, Evaluated, Flattened)) {
				#if defined(ShaveFormatRefFacts)
				if(Neighbor.Kind == AtomKind::Verbatim && HasRefWidths) {
					const LexemeSummary Summary = Work.Count(RefText.substr(Neighbor.Text.Begin, Neighbor.Text.Size));
					// 逐語の端が空の行である事の返戻
					return !(Direction < 0 ? Summary.Tail : Summary.Head);
				}
				#endif
				// 行の端の返戻
				return Neighbor.Kind != AtomKind::Verbatim || !(Direction < 0 ? Neighbor.HasTailContent : Neighbor.HasHeadContent);
			}
			// 同じ行の内容の確認
			if(HasWidth(Neighbor)) {
				#if defined(ShaveFormatRefFacts)
				const bool HasContent =
				HasRefWidths ? Work.Count(RefText.substr(Neighbor.Text.Begin, Neighbor.Text.Size)).Head : Neighbor.HasHeadContent;
				#else
				const bool HasContent = Neighbor.HasHeadContent;
				#endif
				// 同じ行の内容へ接する事の返戻
				if(HasContent) return false;
			}
		}
		// 出力の端の返戻
		return true;
	}

	/**
	 * 宣言区分の平坦形を子の要約から１度生成する関数
	 * 計算量：新しく必要に為る要素数 N とテキストのバイト数 B に対し O(N + B)
	 * @param IR 中間表現
	 * @param Id 平坦形を参照する要素
	 * @param Work 共有要約と作業量
	 * @return 必須境界を含む各行の幅の要約
	 * @throws FormatLimitExceeded 要約生成が作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	LineBreaker::FlatSummary LineBreaker::GetFlatForm(const FormatIR &IR, const FormatItemId Id, WorkMeter &Work) {
		// 存在しない共有部分の要約の返戻
		if(Id == NoItem) return {};
		Work.Charge(1);
		if(!Work.Summaries) Work.Summaries = std::make_unique<WorkMeter::SummaryCache>();
		if(
			const std::pmr::unordered_map<FormatItemId, FlatSummary>::const_iterator Found =
			ShouldReuseFlatSummaries ? Work.Summaries->Flat->Items.find(Id) : Work.Summaries->Flat->Items.end();
			Found != Work.Summaries->Flat->Items.end()
		// 走査の作業列を作らず，登録済の平坦形の要約を返す
		) return Found->second;
		// 未登録の根だけが走査の作業枠を必要とし，同じ解決の中で容量を再使用する
		std::vector<WorkMeter::FlatFrame> &Pending = Work.FlatPending;
		Pending.clear();
		Pending.push_back({ Id });
		FlatSummary Result;
		while(!Pending.empty()) {
			WorkMeter::FlatFrame &Cur = Pending.back();
			if(!Cur.HasEntered) {
				Cur.HasEntered = true;
				if(Pending.size() > 1) Work.Charge(1);
				if(
					const std::pmr::unordered_map<FormatItemId, FlatSummary>::const_iterator Found =
					ShouldReuseFlatSummaries && Pending.size() > 1 ? Work.Summaries->Flat->Items.find(Cur.Id) : Work.Summaries->Flat->Items.end();
					Found != Work.Summaries->Flat->Items.end()
				) {
					Cur.Summary = Found->second;
					Cur.IsReused = true;
				} else switch(const FormatItem &Item = IR.Items[Cur.Id]; Item.Kind) {
				case FormatItemKind::Token:
				case FormatItemKind::Space:
				case FormatItemKind::LexicalSeparator:
				case FormatItemKind::SoftBreak:
				case FormatItemKind::Verbatim:
					{
						const LexemeSummary Summary = Work.Measure(IR, Item.Text);
						Cur.Summary = {
							Summary.Head,
							Summary.Tail,
							Summary.Max,
							Summary.HasNewline && !IR.GetDetailOf(Item.Detail).DoesExcludeTextNewlines &&
							!IR.GetDetailOf(Item.Detail).DoesExcludeCodeNewlines,
							Summary.HasNewline
						};
					}
					break;
				case FormatItemKind::HardBreak:
				case FormatItemKind::BlankLine:
				case FormatItemKind::OwnedRegion:
					Cur.Summary.HasBreak = true;
					Cur.Summary.HasNewline = true;
					break;
				case FormatItemKind::IfBroken:
					if(Item.IsActivatedWhenFlat) Cur.Children = Item.Children;
					break;
				case FormatItemKind::DeclarationSplit:
					Cur.DeclParts = IR.GetDeclChildrenOf(Item);
					Work.Charge((Item.Children.Size << 1) - 7);
					break;
				default:
					Cur.Children = Item.Children;
					break;
				}
			}
			if(const size_t ChildCount = Cur.DeclParts.empty() ? Cur.Children.Size : Cur.DeclParts.size(); Cur.Next < ChildCount) {
				Pending.push_back({ Cur.DeclParts.empty() ? IR.Links[Cur.Children.Begin + Cur.Next++] : Cur.DeclParts[Cur.Next++] });
				continue;
			}
			Result = Cur.Summary;
			// 新しく求めた要約だけを登録する（引いた要約は登録済）
			if(ShouldReuseFlatSummaries && !Cur.IsReused) Work.Summaries->Flat->Items.emplace(Cur.Id, Result);
			Pending.pop_back();
			if(!Pending.empty()) {
				Work.Charge(1);
				Pending.back().Summary = JoinFlat(Pending.back().Summary, Result);
				++Work.SummaryCompositions;
			}
		}
		// 共有した平坦形の要約の返戻
		return Result;
	}

	/**
	 * 共有する部分のコピーの取得関数
	 * 区分した２つ目以降の宣言で繰り返す部分のアンカーを外し，グループには新しい番号を振って折返の選択肢を保つ
	 * 計算量：コピーする要素数 N に対し O(N)
	 * @param IR 中間表現（コピーを追加する）
	 * @param Id 最初の宣言の共有する部分
	 * @param Decl 元の統合宣言
	 * @param Row 出力宣言の番号
	 * @param Groups 元のグループ番号から此の出力宣言のグループ番号への対応
	 * @param NextGroup 新しいグループ番号の割当先
	 * @param Work コピーの作業単位
	 * @return アンカーを重複させず選択肢を保つコピー
	 * @throws FormatLimitExceeded グループの番号を割り当てれない場合，又はコピー元の展開依存先が先行しない場合，又は作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	FormatItemId LineBreaker::SharedCopy(
		FormatIR &IR,
		const FormatItemId Id,
		const NodeId Decl,
		const uint32_t Row,
		std::unordered_map<uint32_t, uint32_t> &Groups,
		uint32_t &NextGroup,
		WorkMeter &Work
	) {
		// 共有部分を後行でコピーする作業枠の準備
		struct Frame {
			FormatItemId Id;
			FormatItem Copy = {};
			std::vector<FormatItemId> Children = {};
			uint32_t Next = 0;
			bool HasEntered = false;
		};
		std::vector<Frame> Pending { { Id } };
		FormatItemId Result = NoItem;
		// 子のコピーと，グループ番号・属性の参照先の更新
		while(!Pending.empty()) {
			Frame &Cur = Pending.back();
			if(!Cur.HasEntered) {
				Cur.HasEntered = true;
				Work.Charge(1);
				Cur.Copy = IR.Items[Cur.Id];
				// コピー先のグループ番号への変換と，アンカーの除去
				FormatItem &Copy = Cur.Copy;
				switch(Copy.Kind) {
				case FormatItemKind::Group:
					{
						if(const std::unordered_map<uint32_t, uint32_t>::const_iterator Found = Groups.find(Copy.Group); Found == Groups.end()) {
							if(NextGroup == NoGroup) throw FormatLimitExceeded("the format IR has too many groups", FailureReason::FormatterDefect);
							Groups.emplace(Copy.Group, NextGroup);
							Copy.Group = NextGroup++;
						} else Copy.Group = Found->second;
					}
					break;
				case FormatItemKind::AnchorMark:
					Copy.Kind = FormatItemKind::Sequence;
					Copy.Node = NoNode;
					break;
				case FormatItemKind::IfBroken:
					if(const std::unordered_map<uint32_t, uint32_t>::const_iterator Found = Groups.find(Copy.Group); Found != Groups.end()) {
						Copy.Group = Found->second;
					}
					break;
				default:
					// コピーした文の終端と区切は規則から生成した同じ種類のトークンとし，出力のアンカーは元の要素だけが持つ
					if(Copy.Kind == FormatItemKind::Token && Copy.Node != NoNode && Copy.Generated == GeneratedToken::None) {
						if(IR.NormalizedSyntax->IsTokenRole(Copy.Node, Role::StmtTerminatorToken)) Copy.Generated = GeneratedToken::StmtTerminator;
						else if(IR.NormalizedSyntax->IsTokenRole(Copy.Node, Role::CommaToken)) Copy.Generated = GeneratedToken::ListSeparator;
					}
					Copy.Node = NoNode;
					break;
				}
				// コピーの追加で子の並びの領域が動く為，番号で読む
				Cur.Children.reserve(Copy.Children.Size);
			}
			if(Cur.Next < Cur.Copy.Children.Size) {
				Pending.push_back({ IR.Links[Cur.Copy.Children.Begin + Cur.Next++] });
				continue;
			}
			Cur.Copy.Children = IR.Link(Cur.Children);
			// 属性はコピー毎に持ち，出現の記録が元の要素へ及ばない様にする
			const uint32_t Detail = Cur.Copy.Detail;
			Cur.Copy.Detail = NoDetail;
			Work.Charge(1);
			Result = IR.Add(Cur.Copy);
			if(Detail != NoDetail) {
				const FormatItemDetail Original = IR.GetDetailOf(Detail);
				IR.EditDetail(Result) = Original;
				if(Original.ExpansionDependency != NoGroup) {
					const std::unordered_map<uint32_t, uint32_t>::const_iterator Found = Groups.find(Original.ExpansionDependency);
					if(Found == Groups.end()) {
						throw FormatLimitExceeded("a copied expansion dependency has no preceding group", FailureReason::FormatterDefect);
					}
					IR.EditDetail(Result).ExpansionDependency = Found->second;
				}
			}
			FormatItemDetail &Occurrence = IR.EditDetail(Result);
			Occurrence.DeclOwner = Decl;
			Occurrence.DeclRow = Row;
			Pending.pop_back();
			if(!Pending.empty()) Pending.back().Children.push_back(Result);
		}
		// コピーの返戻
		return Result;
	}

	/**
	 * コピーした共有部分が属する出力宣言の記録関数
	 * 共有部分の各要素へ，元の統合宣言と分割後の宣言番号を記録する
	 * 計算量：辿る要素数 N に対し O(N)
	 * @param IR 中間表現
	 * @param Id 共有部分の要素
	 * @param Decl 元の統合宣言
	 * @param Row 出力宣言の番号
	 * @param Work 出現の記録の作業単位
	 * @throws FormatLimitExceeded 記録の作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	void LineBreaker::MarkDeclOccurrence(
		FormatIR &IR,
		const FormatItemId Id,
		const NodeId Decl,
		const uint32_t Row,
		WorkMeter &Work
	) {
		// コピーした共有部分が属する出力宣言の記録
		struct Frame {
			FormatItemId Id;
			uint32_t Next = 0;
			bool HasEntered = false;
		};
		std::vector<Frame> Pending { { Id } };
		while(!Pending.empty()) {
			Frame &Cur = Pending.back();
			if(!Cur.HasEntered) {
				Cur.HasEntered = true;
				Work.Charge(1);
				FormatItemDetail &Detail = IR.EditDetail(Cur.Id);
				Detail.DeclOwner = Decl;
				Detail.DeclRow = Row;
			}
			const FormatRange Children = IR.Items[Cur.Id].Children;
			if(Cur.Next == Children.Size) {
				Pending.pop_back();
				continue;
			}
			Pending.push_back({ IR.Links[Children.Begin + Cur.Next++] });
		}
		// 終了
		return;
	}

	/**
	 * コピーしたグループの決定単位だけを追加する関数
	 * 計算量：コピーしたグループ数 G と其の直接参照数 D に対し O(G + D)
	 * @param IR 中間表現
	 * @param Groups 元のグループとコピーしたグループの対応
	 * @param Connections 元の単位へ追加する直接依存と通知先（受け手・供給元）
	 * @param Work 行分割の作業予算
	 * @throws FormatLimitExceeded 追加の作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void LineBreaker::CopyDecisions(
		FormatIR &IR,
		const std::unordered_map<uint32_t, uint32_t> &Groups,
		std::vector<std::pair<uint32_t, uint32_t>> &Connections,
		WorkMeter &Work
	) {
		// コピーしたグループの決定単位だけを追加
		std::unordered_map<uint32_t, uint32_t> Copies;
		for(const std::pair<const uint32_t, uint32_t> &Entry : Groups) {
			Work.Charge(1);
			const uint32_t Original = IR.DecisionOf.Get(Entry.first), Copy = static_cast<uint32_t>(IR.Decisions.size());
			Copies.emplace(Original, Copy);
			const FormatDecision &Src = IR.Decisions[Original];
			Work.Charge(Src.Dependencies.size() + Src.Successors.size() + 1);
			FormatDecision Unit;
			Unit.Group = Entry.second;
			Unit.Rank = Copy;
			Unit.UnitOwner = Src.UnitOwner;
			Unit.ExpansionDependency = Src.ExpansionDependency;
			Unit.Prefix = Src.Prefix;
			Unit.Annotation = Src.Annotation;
			Unit.Procedure = Src.Procedure;
			Unit.IsContinuationContent = Src.IsContinuationContent;
			Unit.IsContinuationList = Src.IsContinuationList;
			Unit.IsContinuationMember = Src.IsContinuationMember;
			Unit.IsContinuationType = Src.IsContinuationType;
			Unit.IsChainBody = Src.IsChainBody;
			Unit.Dependencies = Src.Dependencies;
			Unit.Successors = Src.Successors;
			IR.DecisionOf.Set(Entry.second, Copy);
			IR.Decisions.push_back(std::move(Unit));
		}
		// コピーしたグループ又は元のグループの順位の照会
		const auto Remap = [&](const uint32_t Rank) -> uint32_t {
			const std::unordered_map<uint32_t, uint32_t>::const_iterator Found = Copies.find(Rank);
			// 此の出現にコピーしたグループ又は外側の直接参照の返戻
			return Found == Copies.end() ? Rank : Found->second;
		};
		for(const std::pair<const uint32_t, uint32_t> &Entry : Copies) {
			Work.Charge(1);
			FormatDecision &Unit = IR.Decisions[Entry.second];
			Unit.UnitOwner = Remap(Unit.UnitOwner);
			IR.Decisions[Unit.UnitOwner].Members.push_back(Unit.Rank);
			if(Unit.ExpansionDependency != NoGroup) Unit.ExpansionDependency = Remap(Unit.ExpansionDependency);
			if(Unit.Prefix != NoGroup) Unit.Prefix = Remap(Unit.Prefix);
			if(Unit.Annotation != NoGroup) {
				// 元の単位から選択を受け取る外側のアノテーションと，コピーしたアノテーションの対応付け
				const std::unordered_map<uint32_t, uint32_t>::const_iterator Annotation = Copies.find(Unit.Annotation);
				Unit.Annotation = Annotation == Copies.end() ? NoGroup : Annotation->second;
			}
			for(uint32_t &Provider : Unit.Dependencies) {
				Work.Charge(1);
				if(!Copies.contains(Provider)) Connections.emplace_back(Unit.Rank, Provider);
				Provider = Remap(Provider);
			}
			for(uint32_t &Receiver : Unit.Successors) {
				Work.Charge(1);
				if(!Copies.contains(Receiver)) Connections.emplace_back(Receiver, Unit.Rank);
				Receiver = Remap(Receiver);
			}
			// 元の単位の一部だけをコピーした場合は，外側の採否を元の単位から受け取る
			if(Unit.UnitOwner == Unit.Rank) {
				bool HasExternalMember = false;
				for(const uint32_t Member : IR.Decisions[Entry.first].Members) {
					Work.Charge(1);
					HasExternalMember = HasExternalMember || !Copies.contains(Member);
				}
				if(HasExternalMember) {
					Unit.Dependencies.push_back(Entry.first);
					Connections.emplace_back(Unit.Rank, Entry.first);
				}
			}
		}
		// 終了
		return;
	}

	/**
	 * グループの全候補を除けるかの評価関数（行分割仕様 3.3）
	 * 全候補を読取上で除き，候補が同じ測定区間へ収まって幅が上限以下なら除ける
	 * 共有の境界差分 P が有れば，各境界位置の取得に O(log(N + 1) * log(N + P + 1)) を要する
	 * インデックスが無ければ O(N * (log(G + 1) + 1))，行端空白の隣接確認を含む場合は最悪 O(N² * (log(G + 1) + 1))
	 * 計算量：要素数 N，候補・条件付断片の数 K と同時に測るグループ数 G に対しインデックスが有れば O((K + 1) log N + K log(G + 1))
	 * @param States グループの状態
	 * @param Conditions 採用済の条件付トークンの選択
	 * @param Atoms 線形化した要素
	 * @param Candidates 評価するグループの選択可能な境界の位置
	 * @param Conditionals 平坦時の採否を試す条件付断片の位置
	 * @param Evaluated 評価するグループ
	 * @param Idx 可視トークン・行端差・境界のインデックス（幅０とテスト専用の直接測定では無）
	 * @param Work 行分割の作業単位（評価と走査した要素を計上する）
	 * @param Flattened 境界と条件付断片を平坦形として扱うグループ（昇順）
	 * @param Changed 外側の試行で境界又は表示を変えた断片
	 * @param Projection 括弧外の未確定の候補を平坦として読む共有差分（不要なら nullptr）
	 * @param Expanded 展開形の条件付断片を読むグループ（平坦形の指定を優先し，NoGroup は変更無）
	 * @return グループの折返を除けるなら true
	 * @throws FormatLimitExceeded 作業単位が上限を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	bool LineBreaker::CanFlatten(
		const std::vector<GroupState> &States,
		const std::vector<uint8_t> &Conditions,
		const std::vector<Atom> &Atoms,
		const std::span<const size_t> Candidates,
		const std::span<const size_t> Conditionals,
		const uint32_t Evaluated,
		const MeasureIdx *const Idx,
		WorkMeter &Work,
		const std::span<const uint32_t> Flattened,
		const std::span<const size_t> Changed,
		const BoundaryProjection *const Projection,
		const uint32_t Expanded
	) const {
		// グループの１回の評価を１作業単位とする（整形仕様 9.2）
		Work.Charge(1);
		++Work.Evaluations;
		// 候補の無いグループの返戻
		if(Candidates.empty()) return true;
		const size_t First = Candidates.front(), Last = Candidates.back();
		// 候補差分と既存のインデックスの読取専用の合成
		if(Idx) {
			// 通常の境界だけの試行は，生成済の候補列を其の儘読む
			std::vector<size_t> &Positions = Work.TrialPositions, &Combined = Work.TrialMerged;
			const bool HasDelta = !Conditionals.empty() || !Changed.empty();
			if(HasDelta) {
				// 既知の２列の必要量を１度予約し，後の試行で少しずつ増える時も償却した伸長を保つ
				const auto Merge =
				[](const std::span<const size_t> Left, const std::span<const size_t> Right, std::vector<size_t> &Output) -> void {
					if(Left.size() > Output.max_size() || Right.size() > Output.max_size() - Left.size()) {
						throw std::length_error("too many trial positions");
					}
					const size_t Count = Left.size() + Right.size();
					if(Count > Output.capacity()) {
						const size_t Capacity = Output.capacity(), Max = Output.max_size();
						Output.reserve(std::max(Count, Capacity > (Max >> 1) ? Max : Capacity << 1));
					}
					std::merge(Left.begin(), Left.end(), Right.begin(), Right.end(), std::back_inserter(Output));
					// 終了
					return;
				};
				Positions.clear();
				Work.Charge(Candidates.size() + Conditionals.size());
				Merge(Candidates, Conditionals, Positions);
				if(!Changed.empty()) {
					Combined.clear();
					Work.Charge(Positions.size() + Changed.size());
					Merge(Positions, Changed, Combined);
				}
				std::vector<size_t> &Merged = Changed.empty() ? Positions : Combined;
				Merged.erase(std::unique(Merged.begin(), Merged.end()), Merged.end());
			}
			const std::span<const size_t> Merged = !HasDelta ? Candidates : Changed.empty() ? Positions : Combined;
			const BoundaryCounter &Boundaries = Idx->Boundaries;
			// ローカル差分を除いた境界数の照会
			const auto CountBefore = [&](const size_t End) -> uint32_t {
				Work.Charge(Projection ? 2 : 1);
				// 試行の共有差分を含む境界数の返戻
				return Projection ? Projection->CountBefore(End) : Boundaries.CountBefore(End);
			};
			// ローカル差分を除いた順位の境界の照会
			const auto Select = [&](const uint32_t Rank) -> size_t {
				// 試行の共有差分を含む境界位置の返戻
				return Projection ? Projection->Select(Rank, Work) : Boundaries.Select(Rank);
			};
			std::vector<size_t> &Removed = Work.TrialRemoved, &Added = Work.TrialAdded;
			Removed.clear();
			Added.clear();
			for(const size_t Pos : Merged) {
				const Atom &Piece = Atoms[Pos];
				if(
					const bool IsTrialBoundary =
					IsBoundary(States, Piece, Evaluated, Flattened) && IsVisible(Conditions, Piece, Evaluated, Flattened, Expanded);
					Boundaries.IsCounted[Pos] && !IsTrialBoundary
				) Removed.push_back(Pos);
				else if(!Boundaries.IsCounted[Pos] && IsTrialBoundary) Added.push_back(Pos);
			}
			Work.Charge(Merged.size());
			const size_t RemovedInner = static_cast<size_t>(
				std::upper_bound(Removed.begin(), Removed.end(), Last) - std::lower_bound(Removed.begin(), Removed.end(), First)
			);
			const size_t AddedInner =
			static_cast<size_t>(std::upper_bound(Added.begin(), Added.end(), Last) - std::lower_bound(Added.begin(), Added.end(), First));
			const uint32_t BeforeFirst = CountBefore(First), AfterLast = CountBefore(Last + 1);
			// 他の残存境界を挟む事の返戻
			if(AfterLast + AddedInner - BeforeFirst - RemovedInner) return false;
			// 採用済のインデックスとローカル差分からの左右の残存境界の取得
			size_t Left = Atoms.size(), Right = Atoms.size();
			uint32_t Rank = BeforeFirst;
			while(Rank) {
				const size_t Pos = Select(--Rank);
				Work.Charge(1);
				if(!std::binary_search(Removed.begin(), Removed.end(), Pos)) {
					Left = Pos;
					break;
				}
			}
			Rank = AfterLast;
			const uint32_t Total = CountBefore(Atoms.size());
			while(Rank < Total) {
				const size_t Pos = Select(Rank++);
				Work.Charge(1);
				if(!std::binary_search(Removed.begin(), Removed.end(), Pos)) {
					Right = Pos;
					break;
				}
			}
			for(const size_t Pos : Added) {
				if(Pos < First && (Left == Atoms.size() || Left < Pos)) Left = Pos;
				if(Pos > Last && Pos < Right) Right = Pos;
			}
			const std::span<const size_t> TextBreaks =
			GetTextBreaks(Conditions, Atoms, First, Last, Evaluated, Flattened, Expanded, Left, Right, Work);
			// 同じ試行差分を物理行毎の既存要約へ合成する
			const auto Fits = [&](const size_t Begin, const size_t End) -> bool {
				const bool HasLeftContent = Begin != Atoms.size() && Atoms[Begin].Kind == AtomKind::Verbatim && Atoms[Begin].HasTailContent;
				const bool HasRightContent = End != Atoms.size() && Atoms[End].Kind == AtomKind::Verbatim && Atoms[End].HasHeadContent;
				const size_t Start = Begin == Atoms.size() ? 0 : Begin + 1;
				const std::span<const size_t>::iterator From = std::lower_bound(Merged.begin(), Merged.end(), Start);
				const std::span<const size_t>::iterator To = std::lower_bound(From, Merged.end(), End);
				size_t Width = Idx->ComputeTrialWidth(
					Conditions,
					Atoms,
					Start,
					End,
					HasLeftContent,
					HasRightContent,
					Merged.subspan(static_cast<size_t>(From - Merged.begin()), static_cast<size_t>(To - From)),
					Evaluated,
					Flattened,
					Expanded,
					Work
				);
				if(Begin != Atoms.size() && Atoms[Begin].Kind == AtomKind::Verbatim) Width += Atoms[Begin].TailWidth;
				if(End != Atoms.size() && Atoms[End].Kind == AtomKind::Verbatim) Width += Atoms[End].Width;
				// 接続した物理行が上限内である事の返戻
				return Width <= MaxChars;
			};
			for(const size_t Pos : TextBreaks) {
				Work.Charge(1);
				if(!IsVisible(Conditions, Atoms[Pos], Evaluated, Flattened, Expanded)) continue;
				// 接続した物理行が上限を超える場合の返戻
				if(!Fits(Left, Pos)) return false;
				Left = Pos;
			}
			// 全候補を除いた各物理行が上限内に収まる事の返戻
			return Fits(Left, Right);
		}
		// 幅０では最初と最後の候補が同じ必須境界区間に在るかだけを読む
		if(!MaxChars) {
			Work.Charge(1);
			// 合法な平坦形の返戻
			return Work.ZeroSegments[First] == Work.ZeroSegments[Last];
		}
		// テスト専用の直接測定に於ける候補間の確認
		for(size_t Idx = First + 1; Idx < Last; ++Idx) {
			Work.Charge(1);
			// 候補が複数の測定区間へ分かれる事の返戻
			if(
				const Atom &Piece = Atoms[Idx];
				IsVisible(Conditions, Piece, Evaluated, Flattened, Expanded) && IsBoundary(States, Piece, Evaluated, Flattened)
			) return false;
		}
		#if !defined(ShaveFormatRefFacts)
		// 幅０の合法な平坦形の返戻
		return true;
		#else
		size_t Width = 0, Begin = First;
		++Work.DecisionSummaries;
		// 候補へ接する左の物理行の始端と，逐語の末尾行の幅
		while(Begin) {
			Work.Charge(1);
			const Atom &Piece = Atoms[--Begin];
			if(!IsVisible(Conditions, Piece, Evaluated, Flattened, Expanded)) continue;
			if(Piece.HasNewline || IsBoundary(States, Piece, Evaluated, Flattened)) {
				if(Piece.Kind == AtomKind::Verbatim) Width = Work.Count(RefText.substr(Piece.Text.Begin, Piece.Text.Size)).Tail;
				++Begin;
				break;
			}
		}
		// 候補差分を反映した物理行の直接測定
		for(size_t Pos = Begin; Pos < Atoms.size(); ++Pos) {
			Work.Charge(1);
			const Atom &Piece = Atoms[Pos];
			if(!IsVisible(Conditions, Piece, Evaluated, Flattened, Expanded)) continue;
			const bool IsPhysicalBoundary = Piece.HasNewline || IsBoundary(States, Piece, Evaluated, Flattened);
			if(Pos > Last && IsPhysicalBoundary) {
				if(Piece.Kind == AtomKind::Verbatim) Width += Work.Count(RefText.substr(Piece.Text.Begin, Piece.Text.Size)).Head;
				// 右の物理境界迄の行幅の返戻
				return Width <= MaxChars;
			}
			if(
				!HasWidth(Piece) || Piece.Kind == AtomKind::EdgeSpace && (
					IsLineEdge(States, Conditions, Atoms, Pos, -1, Evaluated, Work, Flattened, Expanded) ||
					IsLineEdge(States, Conditions, Atoms, Pos, 1, Evaluated, Work, Flattened, Expanded)
				)
			) continue;
			const LexemeSummary Summary = Work.Count(RefText.substr(Piece.Text.Begin, Piece.Text.Size));
			Width += Summary.Head;
			// 接続した物理行が上限を超える場合の返戻
			if(Width > MaxChars) return false;
			if(Piece.HasNewline) Width = Summary.Tail;
		}
		// 最後の物理行が上限内である事の返戻
		return Width <= MaxChars;
		#endif
	}

	/**
	 * １つの統合宣言の区分関数（行分割仕様 3.1 の２，4.1）
	 * 宣言子の順を保ち，共有する部分と各宣言子を平坦形で測って行幅内へ収まる最長の連続範囲を１つの宣言に纏める
	 * 必須改行又は空行を含む宣言子は其の構造を保って他の宣言子と纏めず，行幅０では分けない
	 * 計算量：当該宣言の要素数 N と測るテキストの長さ T と出力区分数 R に対し O(R * (N + T))
	 * @param IR 中間表現（区分した並びへ書き換える）
	 * @param Id 区分する要素
	 * @param Partitions 確定した出力区分の格納先
	 * @param NextGroup 新しくコピーした共有部分のグループ番号の割当先
	 * @param Work 行分割の作業単位
	 * @throws FormatLimitExceeded 宣言子又は元のノードを持たない場合（中間表現の不具合），又は作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void LineBreaker::PartitionDecl(
		FormatIR &IR,
		const FormatItemId Id,
		std::vector<DeclPartition> &Partitions,
		uint32_t &NextGroup,
		WorkMeter &Work
	) const {
		// 統合宣言以外の要素の終了
		if(IR.Items[Id].Kind != FormatItemKind::DeclarationSplit) return;
		const NodeId Node = IR.Items[Id].Node;
		const std::span<const FormatItemId> Links = IR.GetChildrenOf(IR.Items[Id]);
		if(Node == NoNode || Links.size() < 6) {
			throw FormatLimitExceeded("the format IR has an invalid declaration split", FailureReason::FormatterDefect);
		}
		const size_t Count = Links.size() - 5; // 共有部分５個を除いた宣言子の数
		DeclPartition Partition;
		Partition.Node = Node;
		// 幅０で必要な宣言子の不変条件の検査と，分割しない区分の記録
		if(!MaxChars) {
			for(const FormatItemId Part : Links.subspan(5)) {
				Work.Charge(1);
				if(IR.GetChildrenOf(IR.Items[Part]).size() != 2) {
					throw FormatLimitExceeded("the format IR has an invalid declaration split", FailureReason::FormatterDefect);
				}
			}
			Partition.Rows.push_back({ 0, Count });
			Partitions.push_back(std::move(Partition));
			// 幅に依る分割を行わない宣言の終了
			return;
		}
		// 中間表現の追加で参照が失効しない様に，幅判定で使う部品列だけを保持
		const std::vector<FormatItemId> Parts(Links.begin(), Links.end());
		// 宣言子毎の名前と初期化子
		std::vector<FormatItemId> Names(Count), Vals(Count);
		for(size_t Idx = 0; Idx < Count; ++Idx) {
			Work.Charge(1);
			const std::span<const FormatItemId> Unit = IR.GetChildrenOf(IR.Items[Parts[Idx + 5]]); // 共有部分５個の後の名前・初期化子の組
			if(Unit.size() != 2) {
				throw FormatLimitExceeded("the format IR has an invalid declaration split", FailureReason::FormatterDefect);
			}
			Names[Idx] = Unit[0];
			Vals[Idx] = Unit[1];
		}
		// 必須境界の後を含む，共有指定と宣言子の全ての物理行の幅の取得
		const FlatSummary Head = GetFlatForm(IR, Parts[0], Work);
		const FlatSummary Middle = GetFlatForm(IR, Parts[1], Work);
		const FlatSummary Tail = GetFlatForm(IR, Parts[2], Work);
		const bool HasVals =
		Vals[0] != NoItem && (IR.Items[Vals[0]].Children.Size || IR.Items[Vals[0]].Kind != FormatItemKind::Sequence);
		std::vector<FlatSummary> NameForms(Count), ValForms(Count);
		for(size_t Idx = 0; Idx < Count; ++Idx) {
			Work.Charge(1);
			NameForms[Idx] = GetFlatForm(IR, Names[Idx], Work);
			ValForms[Idx] = GetFlatForm(IR, Vals[Idx], Work);
		}
		const FormatItemId Separator = Parts[3], Break = Parts[4];
		const FlatSummary SeparatorForm = Count > 1 ? GetFlatForm(IR, Separator, Work) : FlatSummary{};
		// 要約の定数時間の合成に依る，同じ宣言へ収まる最長の連続範囲の選択
		for(size_t Begin = 0, End = 0; Begin < Count; Begin = End) {
			FlatSummary NameForm = NameForms[Begin], ValForm = ValForms[Begin];
			for(
				End = Begin + 1;
				End < Count && !NameForm.HasBreak && !ValForm.HasBreak && !NameForms[End].HasBreak && !ValForms[End].HasBreak &&
				!SeparatorForm.HasBreak;
				++End
			) {
				Work.Charge(HasVals ? 8 : 7); // 名前の２回・値の１〜２回・全体の４回の要約の合成
				const FlatSummary NextName = JoinFlat(JoinFlat(NameForm, SeparatorForm), NameForms[End]);
				const FlatSummary NextVal = JoinFlat(HasVals ? JoinFlat(ValForm, SeparatorForm) : ValForm, ValForms[End]);
				if(
					const FlatSummary Next = JoinFlat(JoinFlat(JoinFlat(JoinFlat(Head, NextName), Middle), NextVal), Tail);
					Next.Max > MaxChars
				) break;
				NameForm = NextName;
				ValForm = NextVal;
			}
			Partition.Rows.push_back({ Begin, End });
			if(Partition.Rows.size() >= NoGroup) {
				throw FormatLimitExceeded("the format IR has too many declaration rows", FailureReason::FormatterDefect);
			}
		}
		if(Partition.Rows.size() == 1) {
			Partitions.push_back(std::move(Partition));
			// 分割を要しない宣言の終了
			return;
		}
		std::vector<FormatItemId> Rows;
		std::vector<std::pair<uint32_t, uint32_t>> Connections;
		const size_t OriginalDecisions = IR.Decisions.size();
		for(const DeclaratorRange &Range : Partition.Rows) {
			const size_t Begin = Range.Begin, End = Range.End;
			const uint32_t Row = static_cast<uint32_t>(&Range - Partition.Rows.data());
			std::unordered_map<uint32_t, uint32_t> CopiedGroups;
			// 宣言毎の共有部分のコピーの追加
			const auto AppendShared = [&](const FormatItemId Src) -> void {
				// 存在しない共有部分の終了
				if(Src == NoItem) return;
				const FormatItemId Shared = Begin ? SharedCopy(IR, Src, Node, Row, CopiedGroups, NextGroup, Work) : Src;
				if(!Begin) MarkDeclOccurrence(IR, Shared, Node, Row, Work);
				Rows.push_back(Shared);
			};
			if(Begin) Rows.push_back(Break);
			AppendShared(Parts[0]);
			for(size_t Idx = Begin; Idx < End; ++Idx) {
				if(Idx > Begin) Rows.push_back(Separator);
				if(Names[Idx] != NoItem) Rows.push_back(Names[Idx]);
			}
			AppendShared(Parts[1]);
			for(size_t Idx = Begin; Idx < End; ++Idx) {
				if(Idx > Begin && HasVals) Rows.push_back(Separator);
				if(Vals[Idx] != NoItem) {
					Work.Charge(1);
					FormatItem Val = IR.Items[Vals[Idx]];
					// 共有する代入グループの出現だけを宣言毎に分け，右辺の部分木はコピーしない
					if(
						const std::unordered_map<uint32_t, uint32_t>::const_iterator Found = CopiedGroups.find(Val.Group);
						Val.Kind == FormatItemKind::Group && Found != CopiedGroups.end()
					) {
						Val.Group = Found->second;
						Rows.push_back(IR.Add(Val));
					} else Rows.push_back(Vals[Idx]);
				}
			}
			AppendShared(Parts[2]);
			CopyDecisions(IR, CopiedGroups, Connections, Work);
		}
		// 全てのコピーが元の対応を読んだ後に，元の単位が持つ片側だけを接続する
		for(const std::pair<uint32_t, uint32_t> &Connection : Connections) {
			Work.Charge(1);
			if(Connection.first < OriginalDecisions) IR.Decisions[Connection.first].Dependencies.push_back(Connection.second);
			if(Connection.second < OriginalDecisions) IR.Decisions[Connection.second].Successors.push_back(Connection.first);
		}
		FormatItem Replaced;
		Replaced.Children = IR.Link(Rows);
		Work.Charge(1);
		IR.Items[Id] = Replaced;
		Partitions.push_back(std::move(Partition));
		// 終了
		return;
	}

	/**
	 * 中間表現の線形化関数
	 * 構築済の後行順を読み，選択可能な境界を最も内のグループへ所属させる（行分割仕様 2.1, 3.1）
	 * 計算量：辿る要素数 N と逐語のバイト数 B に対し平均 O(N + B)
	 * @param IR 中間表現
	 * @param State 線形化の途中の状態
	 * @param Id 線形化する要素
	 * @param Cond 条件付断片の採否を決めるグループ（構築時の番号，無ければ `NoGroup`）
	 * @param Work 要素の訪問と原子の生成の計数先
	 * @param Observation 製品の予算を変えない読取専用な観測
	 * @throws FormatLimitExceeded 条件付断片のネスト，未区分の宣言，又はノードの無い構文操作が有る場合（中間表現の不具合），又は作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void LineBreaker::Linearize(
		const FormatIR &IR,
		Linearizer &State,
		const FormatItemId Id,
		const uint32_t Cond,
		WorkMeter &Work,
		CoreObservation *const Observation
	) {
		// 決定順位を直接使い，決定を持たない観測用の入力は観測枠の番号を使う
		State.Forms.resize(IR.Decisions.empty() ? Work.GroupFinalizations.size() : IR.Decisions.size());
		// 要素のネストを巡る作業枠の準備
		struct Frame {
			FormatItemId Id;
			uint32_t Cond;
			uint32_t Next = 0;
			bool HasEntered = false;
			bool IsActivatedWhenFlat = false;
		};
		std::vector<Frame> Pending { { Id, Cond } };
		std::vector<std::vector<FormatItemId>> DeclParts; // 宣言分割の中に居る時だけ出力順の子列を保持する
		std::pmr::unordered_set<uint64_t> GroupLinks(&State.Storage); // 区分で変わった直接の親子関係だけを同じ線形化で記録する
		while(!Pending.empty()) {
			Work.Charge(1);
			Frame &Cur = Pending.back();
			const FormatItem &Item = IR.Items[Cur.Id];
			// 要素と共有テキスト範囲の追加
			const auto Push = [&State, &IR, &Item, &Cur, &Work](const AtomKind Kind, const NodeId Node) -> void {
				Work.Charge(1);
				State.Atoms.emplace_back();
				Atom &Piece = State.Atoms.back();
				Piece.Kind = Kind;
				Piece.Generated = Item.Generated;
				Piece.Text = Item.Text;
				Piece.Cond = Cur.Cond;
				Piece.IsActivatedWhenFlat = Cur.IsActivatedWhenFlat;
				Piece.Node = Node;
				Piece.Detail = Item.Detail;
				Piece.HasHeadContent = !IR.GetTextOf(Item).empty();
				Piece.HasTailContent = Piece.HasHeadContent;
				if(Kind == AtomKind::Verbatim) {
					Piece.HasNewline = Item.HasNewline;
					Piece.HasBlankNewline = Item.HasNewline && !IR.GetDetailOf(Item.Detail).DoesExcludeTextNewlines;
					Piece.HasCodeNewline = Piece.HasBlankNewline && !IR.GetDetailOf(Item.Detail).DoesExcludeCodeNewlines;
					Piece.HasHeadContent = Item.HasHeadContent;
					Piece.HasTailContent = Item.HasTailContent;
				}
				if(Kind == AtomKind::Soft && !State.Enclosing.empty()) Piece.Group = State.Enclosing.back();
			};
			if(!Cur.HasEntered) {
				Cur.HasEntered = true;
				// 要素の種類に応じた原子とグループの開始の記録
				switch(Item.Kind) {
				case FormatItemKind::Sequence:
					break;
				case FormatItemKind::Token:
					Push(AtomKind::Text, Item.Node);
					break;
				case FormatItemKind::Space:
					// 条件付の区切空白は，閉じが更に続く行端では空の表現を選ぶ
					Push(Cur.Cond == NoGroup ? AtomKind::Text : AtomKind::EdgeSpace, Item.Node);
					break;
				case FormatItemKind::LexicalSeparator:
					Push(AtomKind::Separator, NoNode);
					break;
				case FormatItemKind::Verbatim:
					Push(AtomKind::Verbatim, Item.Node);
					break;
				case FormatItemKind::OwnedRegion:
					Push(AtomKind::OwnedRegion, Item.Node);
					break;
				case FormatItemKind::SoftBreak:
					Push(AtomKind::Soft, NoNode);
					break;
				case FormatItemKind::HardBreak:
					Push(AtomKind::Hard, NoNode);
					break;
				case FormatItemKind::BlankLine:
					Push(AtomKind::Blank, IR.GetDetailOf(Item.Detail).Before);
					break;
				case FormatItemKind::Group:
					if(State.ShouldRecordOrder && !State.Enclosing.empty()) {
						Work.Charge(1);
						const uint32_t Parent = State.Enclosing.back();
						if(Parent != Item.Group && GroupLinks.insert(static_cast<uint64_t>(Parent) << 32 | Item.Group).second) {
							State.Children.emplace_back(Parent, Item.Group);
						}
					}
					State.Enclosing.push_back(Item.Group);
					break;
				case FormatItemKind::IfBroken:
					if(Cur.Cond != NoGroup) {
						throw FormatLimitExceeded("conditional fragments are nested in the format IR", FailureReason::FormatterDefect);
					}
					break;
				case FormatItemKind::IndentScope:
					Push(AtomKind::IndentBegin, NoNode);
					break;
				case FormatItemKind::AnchorMark:
					Push(AtomKind::AnchorBegin, Item.Node);
					break;
				case FormatItemKind::DeclarationSplit:
					DeclParts.push_back(IR.GetDeclChildrenOf(Item));
					Work.Charge((Item.Children.Size << 1) - 7);
					break;
				}
			}
			// 子の条件付断片の巡回
			switch(Item.Kind) {
			case FormatItemKind::DeclarationSplit:
				if(Cur.Next < DeclParts.back().size()) {
					Pending.push_back({ DeclParts.back()[Cur.Next++], Cur.Cond });
					continue;
				}
				DeclParts.pop_back();
				break;
			case FormatItemKind::Sequence:
			case FormatItemKind::Group:
			case FormatItemKind::IfBroken:
			case FormatItemKind::IndentScope:
			case FormatItemKind::AnchorMark:
				if(Cur.Next < Item.Children.Size) {
					Frame Child { IR.Links[Item.Children.Begin + Cur.Next++], Item.Kind == FormatItemKind::IfBroken ? Item.Group : Cur.Cond };
					Child.IsActivatedWhenFlat = Item.Kind == FormatItemKind::IfBroken ? Item.IsActivatedWhenFlat : Cur.IsActivatedWhenFlat;
					Pending.push_back(std::move(Child));
					continue;
				}
				break;
			}
			// グループと範囲の終了の記録
			switch(Item.Kind) {
			case FormatItemKind::Group:
				State.Enclosing.pop_back();
				if(State.ShouldRecordOrder) {
					State.Order.Set(Item.Group, static_cast<uint32_t>(State.Closed.size()));
					State.Closed.push_back(Item.Group);
				}
				if(Item.Operation && Item.Node == NoNode) {
					throw FormatLimitExceeded("a selected form has no syntax node", FailureReason::FormatterDefect);
				}
				{
					SelectedForm &Form = State.Forms[IR.Decisions.empty() ? Item.Group : IR.DecisionOf.Get(Item.Group)];
					if(Item.Node != NoNode) {
						if(!Work.GroupFinalizations.empty()) ++Work.GroupNodeVisits;
						if(Observation) Observation->RecordSelection(Item.Node, IR.Decisions.empty() ? Item.Group : IR.DecisionOf.Get(Item.Group));
					}
					if(Item.Operation) {
						const FormatItemDetail &Detail = IR.GetDetailOf(Item.Detail);
						Form.Effects.push_back({ Item.Node, *Item.Operation, Detail.DeclOwner, Detail.DeclRow });
					}
				}
				break;
			case FormatItemKind::IndentScope:
				Push(AtomKind::IndentEnd, NoNode);
				break;
			case FormatItemKind::AnchorMark:
				Push(AtomKind::AnchorEnd, Item.Node);
				break;
			}
			Pending.pop_back();
		}
		// 終了
		return;
	}

	/**
	 * 直接参照を出力上の後行順へ対応付ける関数
	 * 計算量：グループ数 G と直接参照数 D に対し O(G + D)
	 * @param IR 構築済の決定単位
	 * @param State 線形化で記録したグループの閉じの順
	 * @param Work 行分割の作業予算
	 * @param Observation 製品の予算を変えない読取専用な観測
	 * @throws FormatLimitExceeded 対応付けの作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void LineBreaker::ReorderDecisions(FormatIR &IR, Linearizer &State, WorkMeter &Work, CoreObservation *const Observation) {
		// 直接参照と出力上の後行順の対応付け
		std::vector<FormatDecision> Ordered;
		Ordered.reserve(IR.Decisions.size());
		std::vector<SelectedForm> OrderedForms;
		OrderedForms.reserve(IR.Decisions.size());
		std::vector<uint32_t> Ranks(IR.Decisions.size(), NoGroup);
		for(uint32_t Pos = 0; Pos < State.Closed.size(); ++Pos) {
			Work.Charge(1);
			const uint32_t Group = State.Closed[Pos];
			if(State.Order.Get(Group) != Pos) continue;
			const uint32_t Original = IR.DecisionOf.Get(Group);
			Ranks[Original] = static_cast<uint32_t>(Ordered.size());
			Ordered.push_back(std::move(IR.Decisions[Original]));
			OrderedForms.push_back(std::move(State.Forms[Original]));
		}
		if(Observation) Observation->ReorderSelections(Ranks);
		for(FormatDecision &Unit : Ordered) {
			Work.Charge(1);
			Unit.Rank = Ranks[Unit.Rank];
			Unit.UnitOwner = Ranks[Unit.UnitOwner];
			Unit.UnitRank = Unit.Rank;
			if(Unit.ExpansionDependency != NoGroup) Unit.ExpansionDependency = Ranks[Unit.ExpansionDependency];
			if(Unit.Prefix != NoGroup) Unit.Prefix = Ranks[Unit.Prefix];
			if(Unit.Annotation != NoGroup) Unit.Annotation = Ranks[Unit.Annotation];
			Unit.Members.clear();
			Unit.Children.clear();
			for(std::vector<uint32_t> *const Refs : { &Unit.Dependencies, &Unit.Successors }) for(uint32_t &Ref : *Refs) {
				Work.Charge(1);
				Ref = Ranks[Ref];
			}
			State.Order.Set(Unit.Group, Unit.Rank);
		}
		// 区分後の出現が持つ直接の親子関係を，同じ線形化で得た順に対応付ける
		for(const std::pair<uint32_t, uint32_t> &Link : State.Children) {
			Work.Charge(1);
			Ordered[State.Order.Get(Link.first)].Children.push_back(State.Order.Get(Link.second));
		}
		for(const FormatDecision &Member : Ordered) {
			Work.Charge(1);
			FormatDecision &Owner = Ordered[Member.UnitOwner];
			Owner.Members.push_back(Member.Rank);
			Owner.UnitRank = std::min(Owner.UnitRank, Member.Rank);
		}
		IR.Decisions = std::move(Ordered);
		State.Forms = std::move(OrderedForms);
		// 終了
		return;
	}

	/**
	 * 波括弧を省略出来る制御本体の同行形・見出の後のラッパー形・見出を折った同行形を此の順で選ぶ関数
	 * 本体と見出の採否を同じ単位で確定し，成立した最優先の合法形より後の候補を測らない
	 * インデックスが無いテスト専用の幅測定は，行端の隣接走査を含め，原子数 N と試行グループ数 G に対し１候補で最悪 O(N² * (log(G + 1) + 1))
	 * 計算量：要素数 N，見出と連鎖のグループ数 H と参照する候補・条件付断片の累計 K に対しインデックスが有れば O(K log K + (H + K) log N)
	 * @param States グループの状態（本体と見出を同時に確定する）
	 * @param Conditions 採用する条件付断片
	 * @param Atoms 線形化した要素
	 * @param Candidates グループ毎の選択可能な境界
	 * @param ConditionalsOf グループ毎の条件付断片
	 * @param Choice 本体と見出のグループ
	 * @param IsBodyForced 先行するグループの展開が本体のラッパーを必要とするか
	 * @param Idx 可視トークンの幅と境界のインデックス
	 * @param Work 行分割の作業単位
	 * @return 見出のグループを個別に確定したなら `true`
	 * @throws FormatLimitExceeded 作業単位が上限を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	bool LineBreaker::HasAppliedBraceBody(
		std::vector<GroupState> &States,
		std::vector<uint8_t> &Conditions,
		const std::vector<Atom> &Atoms,
		const BoundaryLists &Candidates,
		const BoundaryLists &ConditionalsOf,
		const BraceChoice &Choice,
		const bool IsBodyForced,
		MeasureIdx *const Idx,
		WorkMeter &Work
	) const {
		// 本体と見出を同時に平坦形で測る為の候補の統合
		std::vector<size_t> &HeaderCandidates = Work.DecisionHeaders, &HeaderConditionals = Work.DecisionHeaderConditions;
		HeaderCandidates.clear();
		HeaderConditionals.clear();
		for(const uint32_t Header : Choice.Headers) {
			Work.Charge(Candidates[Header].size() + ConditionalsOf[Header].size() + 1);
			HeaderCandidates.insert(HeaderCandidates.end(), Candidates[Header].begin(), Candidates[Header].end());
			HeaderConditionals.insert(HeaderConditionals.end(), ConditionalsOf[Header].begin(), ConditionalsOf[Header].end());
		}
		std::sort(HeaderCandidates.begin(), HeaderCandidates.end());
		std::sort(HeaderConditionals.begin(), HeaderConditionals.end());
		// 連鎖全体の見出が収まる時だけ，外側の未確定の境界を測定上で除く
		std::vector<uint32_t> &PrefixGroups = Work.BracePrefixGroups;
		std::span<const uint32_t> Flattened = Choice.Headers;
		bool HasChainPrefix = false;
		if(!Choice.Prefix.empty() && !Candidates[Choice.Body].empty()) {
			PrefixGroups = Choice.Headers;
			PrefixGroups.insert(PrefixGroups.end(), Choice.Prefix.begin(), Choice.Prefix.end());
			std::sort(PrefixGroups.begin(), PrefixGroups.end());
			Flattened = PrefixGroups;
			std::vector<size_t> &PrefixCandidates = Work.BracePrefixCandidates, &PrefixConditionals = Work.BracePrefixConditions;
			PrefixCandidates = HeaderCandidates;
			PrefixConditionals = HeaderConditionals;
			const size_t BodyStart = Candidates[Choice.Body].front();
			for(const uint32_t Prefix : Choice.Prefix) {
				const std::pmr::vector<size_t>::const_iterator CandidateEnd =
				std::lower_bound(Candidates[Prefix].begin(), Candidates[Prefix].end(), BodyStart);
				const std::pmr::vector<size_t>::const_iterator ConditionalEnd =
				std::lower_bound(ConditionalsOf[Prefix].begin(), ConditionalsOf[Prefix].end(), BodyStart);
				Work.Charge(Candidates[Prefix].size() + ConditionalsOf[Prefix].size() + 1);
				PrefixCandidates.insert(PrefixCandidates.end(), Candidates[Prefix].begin(), CandidateEnd);
				PrefixConditionals.insert(PrefixConditionals.end(), ConditionalsOf[Prefix].begin(), ConditionalEnd);
			}
			std::sort(PrefixCandidates.begin(), PrefixCandidates.end());
			std::sort(PrefixConditionals.begin(), PrefixConditionals.end());
			HasChainPrefix = CanFlatten(
				States,
				Conditions,
				Atoms,
				PrefixCandidates,
				PrefixConditionals,
				NoGroup,
				Idx,
				Work,
				Flattened,
				ConditionalsOf[Choice.Body],
				nullptr,
				Choice.Body
			);
			if(HasChainPrefix) {
				HeaderCandidates.swap(PrefixCandidates);
				HeaderConditionals.swap(PrefixConditionals);
			} else Flattened = Choice.Headers;
		}
		bool IsFlat = false;
		if(!IsBodyForced) {
			Work.Charge(
				HeaderCandidates.size() + HeaderConditionals.size() + Candidates[Choice.Body].size() + ConditionalsOf[Choice.Body].size()
			);
			std::vector<size_t> &Combined = Work.DecisionCandidates, &CombinedConditionals = Work.DecisionConditions;
			Combined.assign(HeaderCandidates.begin(), HeaderCandidates.end());
			CombinedConditionals.assign(HeaderConditionals.begin(), HeaderConditionals.end());
			Combined.insert(Combined.end(), Candidates[Choice.Body].begin(), Candidates[Choice.Body].end());
			CombinedConditionals.insert(CombinedConditionals.end(), ConditionalsOf[Choice.Body].begin(), ConditionalsOf[Choice.Body].end());
			std::sort(Combined.begin(), Combined.end());
			std::sort(CombinedConditionals.begin(), CombinedConditionals.end());
			IsFlat = CanFlatten(States, Conditions, Atoms, Combined, CombinedConditionals, Choice.Body, Idx, Work, Flattened);
		}
		bool IsHeaderFlat = IsFlat;
		if(!IsFlat) {
			// 波括弧を含む見出の測定と本体を次行へ送る形の優先
			IsHeaderFlat = HasChainPrefix || CanFlatten(
				States,
				Conditions,
				Atoms,
				HeaderCandidates,
				HeaderConditionals,
				NoGroup,
				Idx,
				Work,
				Flattened,
				ConditionalsOf[Choice.Body],
				nullptr,
				Choice.Body
			);
			if(!IsHeaderFlat && !IsBodyForced) {
				// 見出の残存境界に続く本体の同行形を測る
				IsFlat = CanFlatten(States, Conditions, Atoms, Candidates[Choice.Body], ConditionalsOf[Choice.Body], Choice.Body, Idx, Work);
			}
		}
		States[Choice.Body] = IsFlat ? GroupState::Flat : GroupState::Broken;
		Conditions[Choice.Body] = !IsFlat;
		if(IsHeaderFlat) for(const uint32_t Header : Choice.Headers) {
			States[Header] = GroupState::Flat;
			Conditions[Header] = 0;
		} else {
			// 選んだ本体と先行した見出の差分の読取及び，結合形を棄却した見出の１度の確定
			std::vector<size_t> &Adopted = Work.DecisionAdopted;
			Adopted.clear();
			if(Idx) {
				Work.Charge(ConditionalsOf[Choice.Body].size() + Candidates[Choice.Body].size());
				Adopted.assign(ConditionalsOf[Choice.Body].begin(), ConditionalsOf[Choice.Body].end());
				Adopted.insert(Adopted.end(), Candidates[Choice.Body].begin(), Candidates[Choice.Body].end());
				std::sort(Adopted.begin(), Adopted.end());
				Idx->Apply(States, Conditions, Atoms, Adopted, Work);
			}
			for(const uint32_t Header : Choice.Headers) {
				Work.Charge(1);
				States[Header] = CanFlatten(States, Conditions, Atoms, Candidates[Header], ConditionalsOf[Header], Header, Idx, Work) ?
				GroupState::Flat :
				GroupState::Broken;
				Conditions[Header] = States[Header] == GroupState::Broken;
				if(Idx) {
					Work.Charge(Candidates[Header].size() + ConditionalsOf[Header].size());
					Adopted.assign(Candidates[Header].begin(), Candidates[Header].end());
					Adopted.insert(Adopted.end(), ConditionalsOf[Header].begin(), ConditionalsOf[Header].end());
					std::sort(Adopted.begin(), Adopted.end());
					Idx->Apply(States, Conditions, Atoms, Adopted, Work);
				}
			}
		}
		// 見出のグループを個別に確定したかの返戻
		return !IsHeaderFlat;
	}

	/**
	 * 継続括弧を省ける全測定区間の判定関数
	 * 内側の確定した境界を保ち，ラッパーと括弧外の候補を同じ平坦形で測る
	 * インデックスが無い直接測定は区間数 R，テキスト長 T に対し最悪 O(R * (T + N² * (log(G + 1) + 1)))
	 * 計算量：原子数 N，メンバ数 G と候補差分数 K に対しインデックスが有れば O(K log K + K log(N + 1) + K log(G + 1))
	 * @param States グループの確定状態
	 * @param Conditions 採用済の条件付トークンの選択
	 * @param Atoms 線形化した要素
	 * @param Candidates グループ毎の選択可能な境界
	 * @param ConditionalsOf グループ毎の条件付断片
	 * @param Group ラッパーを選ぶグループ
	 * @param Prefix ラッパーの外側の接頭部のグループ
	 * @param ContentGroups ラッパーと同時に平坦形を試す括弧外のグループ
	 * @param Idx 確定済の幅と境界のインデックス
	 * @param Changed 同じ試行で表示を変える後続の条件付トークン
	 * @param Work 候補差分と区間要約の計数先
	 * @param Expanded 展開形の条件付断片を読むグループ（平坦形の指定を優先し，NoGroup は変更無）
	 * @param IsContainer 入口・要素区切・出口を同じ測定区間へ保つか
	 * @return 全ての測定区間がラッパーを省く形で収まれば true
	 * @throws FormatLimitExceeded 評価の作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	bool LineBreaker::CanFlattenContinuation(
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
		const uint32_t Expanded,
		const bool IsContainer
	) const {
		// 継続の平坦候補の統合
		Work.Charge(ContentGroups.size());
		std::vector<uint32_t> &Flattened = Work.ContinuationFlattened;
		Flattened.assign(ContentGroups.begin(), ContentGroups.end());
		if(Prefix != NoGroup) Flattened.push_back(Prefix);
		std::sort(Flattened.begin(), Flattened.end());
		// ラッパー自身と括弧外のメンバの候補の統合
		Work.Charge(Candidates[Group].size());
		std::vector<size_t> &OutsideCandidates = Work.OutsideCandidates;
		OutsideCandidates.assign(Candidates[Group].begin(), Candidates[Group].end());
		for(const uint32_t Member : ContentGroups) {
			Work.Charge(Candidates[Member].size() + 1);
			OutsideCandidates.insert(OutsideCandidates.end(), Candidates[Member].begin(), Candidates[Member].end());
		}
		std::sort(OutsideCandidates.begin(), OutsideCandidates.end());
		OutsideCandidates.erase(std::unique(OutsideCandidates.begin(), OutsideCandidates.end()), OutsideCandidates.end());
		bool IsFlat = true;
		std::vector<size_t> &Run = Work.ContinuationRun;
		Run.clear();
		if(Idx) {
			Work.Charge(Candidates[Group].size() + ConditionalsOf[Group].size() + OutsideCandidates.size() + Changed.size());
			std::vector<size_t> &TrialChanges = Work.ContinuationChanges;
			TrialChanges.assign(Candidates[Group].begin(), Candidates[Group].end());
			TrialChanges.insert(TrialChanges.end(), ConditionalsOf[Group].begin(), ConditionalsOf[Group].end());
			TrialChanges.insert(TrialChanges.end(), OutsideCandidates.begin(), OutsideCandidates.end());
			TrialChanges.insert(TrialChanges.end(), Changed.begin(), Changed.end());
			for(const uint32_t Member : ContentGroups) {
				Work.Charge(ConditionalsOf[Member].size() + 1);
				TrialChanges.insert(TrialChanges.end(), ConditionalsOf[Member].begin(), ConditionalsOf[Member].end());
			}
			if(Prefix != NoGroup) {
				Work.Charge(Candidates[Prefix].size() + ConditionalsOf[Prefix].size());
				TrialChanges.insert(TrialChanges.end(), Candidates[Prefix].begin(), Candidates[Prefix].end());
				TrialChanges.insert(TrialChanges.end(), ConditionalsOf[Prefix].begin(), ConditionalsOf[Prefix].end());
			}
			std::sort(TrialChanges.begin(), TrialChanges.end());
			TrialChanges.erase(std::unique(TrialChanges.begin(), TrialChanges.end()), TrialChanges.end());
			// 要素区切を持つコンテナは，内側の残存境界を跨いだ全平坦形を採用しない
			if(IsContainer) {
				// コンテナの全候補を同じ区間で除けるかの返戻
				return CanFlatten(
					States,
					Conditions,
					Atoms,
					OutsideCandidates,
					std::span<const size_t>(),
					Group,
					Idx,
					Work,
					Flattened,
					TrialChanges,
					nullptr,
					Expanded
				);
			}
			// 継続の同じ測定区間へ属する候補範囲
			std::vector<WorkMeter::ContinuationRange> &Ranges = Work.ContinuationRanges;
			Ranges.clear();
			uint32_t Removed = 0;
			// 除去する境界を除いた順位と各差分の対応付け
			for(size_t Pos = 0; Pos < TrialChanges.size(); ++Pos) {
				Work.Charge(1);
				const size_t ChangedPos = TrialChanges[Pos];
				if(const uint32_t Rank = Idx->Boundaries.CountBefore(ChangedPos) - Removed; Ranges.empty() || Ranges.back().Rank != Rank) {
					Ranges.push_back({ Rank, Pos, Pos + 1 });
				} else Ranges.back().End = Pos + 1;
				if(Idx->Boundaries.IsCounted[ChangedPos]) ++Removed;
			}
			size_t NextOutside = 0;
			for(const WorkMeter::ContinuationRange &Range : Ranges) {
				Work.Charge(1);
				const size_t End = TrialChanges[Range.End - 1] + 1;
				while(NextOutside < OutsideCandidates.size() && OutsideCandidates[NextOutside] < End) {
					Work.Charge(1);
					Run.push_back(OutsideCandidates[NextOutside++]);
				}
				if(Run.empty()) continue;
				IsFlat = CanFlatten(
					States,
					Conditions,
					Atoms,
					Run,
					std::span<const size_t>(),
					Group,
					Idx,
					Work,
					Flattened,
					std::span<const size_t>(TrialChanges).subspan(Range.Begin, Range.End - Range.Begin),
					nullptr,
					Expanded
				);
				Run.clear();
				if(!IsFlat) break;
			}
		} else {
			if(IsContainer) {
				// 直接測定でコンテナの全候補を同じ区間で除けるかの返戻
				return CanFlatten(
					States,
					Conditions,
					Atoms,
					OutsideCandidates,
					ConditionalsOf[Group],
					Group,
					nullptr,
					Work,
					Flattened,
					Changed,
					nullptr,
					Expanded
				);
			}
			for(const size_t Candidate : OutsideCandidates) {
				Work.Charge(1);
				bool IsNewRun = false;
				if(!Run.empty()) {
					if(!MaxChars) IsNewRun = Work.ZeroSegments[Run.back()] != Work.ZeroSegments[Candidate];
					else for(size_t Between = Run.back() + 1; Between < Candidate; ++Between) {
						Work.Charge(1);
						IsNewRun = IsNewRun ||
						IsVisible(Conditions, Atoms[Between], Group, Flattened, Expanded) && IsBoundary(States, Atoms[Between], Group, Flattened);
					}
				}
				if(IsNewRun) {
					IsFlat = CanFlatten(
						States,
						Conditions,
						Atoms,
						Run,
						std::span<const size_t>(),
						Group,
						nullptr,
						Work,
						Flattened,
						Changed,
						nullptr,
						Expanded
					);
					Run.clear();
					if(!IsFlat) break;
				}
				Run.push_back(Candidate);
			}
			if(IsFlat && !Run.empty()) {
				IsFlat = CanFlatten(
					States,
					Conditions,
					Atoms,
					Run,
					std::span<const size_t>(),
					Group,
					nullptr,
					Work,
					Flattened,
					Changed,
					nullptr,
					Expanded
				);
			}
		}
		// 継続括弧を省く合法形の判定の返戻
		return IsFlat;
	}

	/**
	 * 選択の解決関数（整形仕様 6.4，行分割仕様第３章）
	 * 統合宣言を先に区分し，条件付表現を其の幅を参照するグループより先に確定して後行順に１度ずつ選択する
	 * インデックスが無い場合は，測定区間の要素の訪問と試行グループの探索を繰り返す
	 * 行端空白の隣接確認は，１区間に対し最悪 O(N² log(G + 2))
	 * 計算量：要素数 N，グループ数 G と順序の構築・測定で参照する要素の累計 K に対しインデックスが有れば O(N + G log G + K log(K + N + G + 1))
	 * @param IR 幅非依存の中間表現（統合宣言を区分した形へ書き換えるコピー）
	 * @param Measured 実際の作業量の記録先（不要なら nullptr）
	 * @param Observation 製品の予算を変えない読取専用な観測
	 * @return 全ての選択を解決した中間表現
	 * @throws FormatLimitExceeded 中間表現のグループ・宣言区分・構文操作が不整合の場合（フォーマッターの不具合），作業単位が上限を超える場合（リソース上限の見送）
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	BrokenIR LineBreaker::ResolveMeasured(FormatIR IR, WorkMeter *const Measured, CoreObservation *const Observation) const {
		// 評価の作業単位（上限は正規化と同じ入力の基準量から定めた値）
		WorkMeter Work;
		Work.Limit = IR.NormalizedSyntax ? IR.NormalizedSyntax->GetWorkLimit() : std::numeric_limits<size_t>::max();
		// 区分後にコピーする共有部分へ，既存のグループと重ならない番号を割り当てる
		// 幅０又は分割宣言の無い入力では，コピー用の番号を使わない
		uint32_t NextGroup = 0;
		if(MaxChars && !IR.Decls.empty()) for(const FormatDecision &Unit : IR.Decisions) {
			Work.Charge(1);
			if(Measured) ++Work.DeclGroupVisits;
			NextGroup = std::max(NextGroup, Unit.Group + 1);
		}
		std::vector<DeclPartition> Partitions;
		bool IsPartitioned = false;
		for(const FormatItemId Decl : IR.Decls) {
			Work.Charge(1);
			PartitionDecl(IR, Decl, Partitions, NextGroup, Work);
			IsPartitioned = IsPartitioned || Partitions.back().Rows.size() > 1;
		}
		// 宣言の幅要約は区分の確定後には読まず，トークンの幅要約だけを後の評価へ残す
		if(!Measured) {
			// 表を其のメモリより先に破棄してからメモリを返す
			if(Work.Summaries) Work.Summaries->Flat.reset();
			std::vector<WorkMeter::FlatFrame>().swap(Work.FlatPending);
		}
		#if defined(ShaveFormatRefFacts)
		RefText = IR.Texts;
		HasRefWidths = MaxChars;
		#endif
		if(Measured) Work.GroupFinalizations.resize(IR.Decisions.size(), 0);
		Linearizer State;
		State.ShouldRecordOrder = IsPartitioned;
		if(IsPartitioned) State.Order.Reset(IR.DecisionOf.GetCapacity());
		// 宣言の区分が出現を変えない時は構築済の原子数を使い，其の他は従来の上限を予約する
		if(IR.LinearizedCount && !IsPartitioned) State.Atoms.reserve(IR.LinearizedCount);
		else {
			if(IR.Items.size() > State.Atoms.max_size() >> 1) throw std::length_error("too many format atoms");
			State.Atoms.reserve(IR.Items.size() << 1);
		}
		Linearize(IR, State, IR.Root, NoGroup, Work, Observation);
		if(IsPartitioned) ReorderDecisions(IR, State, Work, Observation);
		else State.Order = std::move(IR.DecisionOf);
		// 線形化した要素と子の並びは以後使わない為，評価の間のメモリを空ける
		IR.Items = std::vector<FormatItem>();
		IR.Links = std::vector<FormatItemId>();
		std::vector<Atom> &Atoms = State.Atoms;
		const uint32_t GroupCount = static_cast<uint32_t>(State.Order.GetSize());
		// グループの番号を後行順へ振り直す為のグループ毎の候補と，従う断片の位置
		BoundaryLists Candidates(GroupCount, &State.Storage), ConditionalsOf(GroupCount, &State.Storage);
		std::vector<GroupState> States(GroupCount, GroupState::Pending);
		#if !defined(ShaveFormatRefFacts)
		std::vector<size_t> SegmentOf;
		std::vector<uint8_t> IsVarSegment;
		#endif
		if(!MaxChars) Work.ZeroSegments.resize(Atoms.size());
		#if !defined(ShaveFormatRefFacts)
		if(MaxChars && GroupCount) {
			SegmentOf.resize(Atoms.size());
			IsVarSegment.push_back(0);
		}
		#endif
		#if defined(ShaveFormatRefFacts)
		const bool ShouldRecordSegments = !MaxChars;
		#else
		const bool ShouldRecordSegments = !MaxChars || GroupCount;
		#endif
		size_t Segment = 0;
		Work.AtomPreparationVisits += Atoms.size();
		// 番号の対応は此の走査中に変わらず，無条件の原子を挟む同じ所属の照会も直前の結果を使う
		std::array<uint32_t, 2> PreviousGroups { NoGroup, NoGroup }, PreviousRanks {};
		for(size_t Idx = 0; Idx < Atoms.size(); ++Idx) {
			Work.Charge(1);
			Atom &Piece = Atoms[Idx];
			if(MaxChars && GroupCount && Piece.HasNewline && !Piece.HasCodeNewline) Work.TextBreaks.push_back(Idx);
			const std::array<uint32_t *, 2> Refs { &Piece.Group, &Piece.Cond };
			for(size_t Slot = 0; Slot < Refs.size(); ++Slot) {
				uint32_t *const Ref = Refs[Slot];
				if(*Ref == NoGroup) continue;
				if(*Ref != PreviousGroups[Slot]) {
					const std::optional<uint32_t> Found = State.Order.Find(*Ref);
					if(!Found) throw FormatLimitExceeded("the format IR refers to an unknown group", FailureReason::FormatterDefect);
					PreviousGroups[Slot] = *Ref;
					PreviousRanks[Slot] = *Found;
				}
				*Ref = PreviousRanks[Slot];
			}
			if(Piece.Kind == AtomKind::Soft && Piece.Group != NoGroup) {
				Candidates[Piece.Group].push_back(Idx);
				++Work.BoundaryEntries;
			}
			if(Piece.Cond != NoGroup) {
				ConditionalsOf[Piece.Cond].push_back(Idx);
				++Work.BoundaryEntries;
			}
			// 番号の振直しと同じ走査で，必須境界と可変表現の所属を記録する
			if(!MaxChars) Work.ZeroSegments[Idx] = Segment;
			#if !defined(ShaveFormatRefFacts)
			if(MaxChars && GroupCount) {
				SegmentOf[Idx] = Segment;
				if(Piece.Cond != NoGroup || Piece.Kind == AtomKind::EdgeSpace) IsVarSegment[Segment] = 1;
			}
			#endif
			if(ShouldRecordSegments && Piece.Cond == NoGroup && Piece.Kind != AtomKind::Soft && IsBoundary(States, Piece, NoGroup)) {
				++Segment;
				#if !defined(ShaveFormatRefFacts)
				if(MaxChars && GroupCount) IsVarSegment.push_back(0);
				#endif
			}
		}
		std::vector<uint8_t> HasBrokenHeader(GroupCount, 0);
		#if !defined(ShaveFormatRefFacts)
		std::vector<uint8_t> NeedsIndexedWidth, IsFixedPos;
		bool HasVarUnit = false;
		// 必須境界を跨ぐグループには幅を生成せず，幅を参照する範囲の不変トークンだけを１度計数する
		if(MaxChars && GroupCount) {
			NeedsIndexedWidth.assign(Atoms.size(), 0);
			IsFixedPos.assign(Atoms.size(), 0);
			std::vector<uint8_t> NeedsWidth(IsVarSegment.size(), 0);
			for(const std::pmr::vector<size_t> &Positions : Candidates) {
				Work.Charge(1);
				if(!Positions.empty() && SegmentOf[Positions.front()] == SegmentOf[Positions.back()]) {
					NeedsWidth[SegmentOf[Positions.front()]] = 1;
				}
			}
			for(const FormatDecision &Unit : IR.Decisions) {
				Work.Charge(1);
				if(Unit.Members.size() < 2) continue;
				HasVarUnit = true;
				for(const uint32_t Member : Unit.Members) for(const size_t Pos : Candidates[Member]) {
					Work.Charge(1);
					IsVarSegment[SegmentOf[Pos]] = 1;
				}
			}
			Work.AtomPreparationVisits += Atoms.size();
			for(size_t Pos = 0; Pos < Atoms.size(); ++Pos) {
				Work.Charge(1);
				Atom &Piece = Atoms[Pos];
				const bool IsWidthNeeded = NeedsWidth[SegmentOf[Pos]] ||
				Piece.Kind == AtomKind::Verbatim && Piece.Cond == NoGroup && Piece.HasCodeNewline && NeedsWidth[SegmentOf[Pos] + 1];
				IsFixedPos[Pos] = !IsVarSegment[SegmentOf[Pos]] && IsWidthNeeded;
				NeedsIndexedWidth[Pos] = IsVarSegment[SegmentOf[Pos]] && IsWidthNeeded;
				if(IsWidthNeeded && Piece.Text.Size) {
					const LexemeSummary Summary = Work.Measure(IR, Piece.Text);
					Piece.Width = Summary.Head;
					Piece.TailWidth = Summary.Tail;
					Piece.HasHeadContent = Summary.Head;
					Piece.HasTailContent = Summary.Tail;
				}
			}
		}
		#endif
		// 未評価の境界を保つインデックスの構築と，確定した条件付トークンの差分の適用
		std::vector<uint8_t> Conditions(GroupCount, 0);
		std::optional<MeasureIdx> Idx;
		std::optional<FixedMeasure> Fixed;
		std::vector<uint8_t> IsFixedGroup(GroupCount, 0);
		#if defined(ShaveFormatRefFacts)
		// テスト専用の構築では現在の原子列から直接測る
		#else
		if(MaxChars && GroupCount) {
			bool HasVarGroup = HasVarUnit, HasFixedGroup = false;
			for(uint32_t Group = 0; Group < GroupCount; ++Group) {
				Work.Charge(Candidates[Group].size() + 1);
				bool IsFixed = ConditionalsOf[Group].empty() &&
				(Candidates[Group].empty() || SegmentOf[Candidates[Group].front()] == SegmentOf[Candidates[Group].back()]);
				for(const size_t Pos : Candidates[Group]) if(!IsFixedPos[Pos]) IsFixed = false;
				IsFixedGroup[Group] = IsFixed && !Candidates[Group].empty();
				HasFixedGroup = HasFixedGroup || IsFixed && !Candidates[Group].empty();
				HasVarGroup = HasVarGroup || !IsFixed;
			}
			if(HasFixedGroup) {
				Work.Charge(Atoms.size());
				Fixed.emplace(States, IsFixedPos, Atoms);
				Work.Charge(Fixed->Positions.size());
				Work.PrefixEntries = Fixed->Positions.size();
			}
			if(HasVarGroup) {
				Work.Charge(Atoms.size());
				Idx.emplace(States, Conditions, NeedsIndexedWidth, Atoms);
				Work.Charge(Idx->Positions.size());
			}
		}
		#endif
		// 幅インデックスへ取り込んだ所属と準備のフラグは，評価の間には保持しない
		#if !defined(ShaveFormatRefFacts)
		std::vector<size_t>().swap(SegmentOf);
		std::vector<uint8_t>().swap(IsVarSegment);
		std::vector<uint8_t>().swap(NeedsIndexedWidth);
		std::vector<uint8_t>().swap(IsFixedPos);
		#endif
		State.Order = GroupRanks();
		IR.DecisionOf = GroupRanks();
		std::vector<uint32_t>().swap(State.Closed);
		std::vector<uint32_t>().swap(State.Enclosing);
		// 直接依存を持つ範囲だけ，未確定数と基準順位の待機列を持つ
		std::vector<size_t> Unresolved;
		std::vector<uint32_t> Ready;
		// 決定単位の基準順位に従う待機列の比較
		const auto IsLater = [&IR](const uint32_t Left, const uint32_t Right) -> bool {
			// 基準順位の小さい単位を先にする順序の返戻
			return IR.Decisions[Left].UnitRank > IR.Decisions[Right].UnitRank;
		};
		for(const FormatDecision &Unit : IR.Decisions) {
			if(Unit.UnitOwner != Unit.Rank) continue;
			if(!Unit.Dependencies.empty()) {
				// 密な決定番号を直接引き，依存の無い入力には作業列を確保しない
				if(Unresolved.empty()) Unresolved.resize(GroupCount);
				Unresolved.at(Unit.Rank) = Unit.Dependencies.size();
			}
			Work.DependencyRegistrations += Unit.Dependencies.size();
			Work.Charge(Unit.Dependencies.size());
			if(Unit.Dependencies.empty() && !Unit.Successors.empty()) {
				Ready.push_back(Unit.Rank);
				std::push_heap(Ready.begin(), Ready.end(), IsLater);
			}
		}
		// 選択済のグループの候補と条件付トークンのインデックスへの反映
		std::vector<size_t> UpdatePositions; // 確定毎に容量を再使用する採用差分の位置列
		// 継続単位の分類と仮の平坦形の列は，単位の終了後に容量を再使用する
		std::vector<uint32_t> &ContentGroups = Work.ContentGroups, &AnnotationContent = Work.AnnotationContent;
		std::vector<uint32_t> &EnclosedGroups = Work.EnclosedGroups, &InnerFlattened = Work.InnerFlattened;
		std::vector<size_t> &InnerCandidates = Work.InnerCandidates;
		const auto Update = [&](const std::span<const uint32_t> Groups) -> void {
			std::vector<size_t> &Positions = UpdatePositions;
			Positions.clear();
			for(const uint32_t Group : Groups) {
				Conditions[Group] = States[Group] == GroupState::Broken;
				if(IsFixedGroup[Group]) {
					if(States[Group] == GroupState::Flat && !Candidates[Group].empty()) {
						Fixed->Remove(Atoms, Candidates[Group]);
						Work.BoundaryUnlinks += Candidates[Group].size();
						Work.Charge(Candidates[Group].size());
					}
					continue;
				}
				if(Idx) {
					Work.Charge(Candidates[Group].size() + ConditionalsOf[Group].size());
					Positions.insert(Positions.end(), Candidates[Group].begin(), Candidates[Group].end());
					Positions.insert(Positions.end(), ConditionalsOf[Group].begin(), ConditionalsOf[Group].end());
				}
			}
			if(Idx) {
				std::sort(Positions.begin(), Positions.end());
				Positions.erase(std::unique(Positions.begin(), Positions.end()), Positions.end());
				Idx->Apply(States, Conditions, Atoms, Positions, Work);
			}
			// 終了
			return;
		};
		uint32_t Independent = 0, Finalized = 0;
		while(Independent < GroupCount || !Ready.empty()) {
			while(Independent < GroupCount) {
				if(
					const FormatDecision &Entry = IR.Decisions[Independent], &Unit = IR.Decisions[Entry.UnitOwner];
					Entry.Rank == Unit.UnitRank && Unit.Dependencies.empty() && Unit.Successors.empty()
				) break;
				++Independent;
			}
			uint32_t Group = NoGroup;
			if(!Ready.empty() && (Independent == GroupCount || IR.Decisions[Ready.front()].UnitRank < Independent)) {
				std::pop_heap(Ready.begin(), Ready.end(), IsLater);
				Group = Ready.back();
				Ready.pop_back();
			} else if(Independent < GroupCount) Group = IR.Decisions[Independent++].UnitOwner;
			else break;
			Work.Charge(1);
			const FormatDecision &Unit = IR.Decisions[Group];
			bool HasAppliedMembers = false, HasBrokenChildHeader = false;
			for(const uint32_t Member : Unit.Members) for(const uint32_t Child : IR.Decisions[Member].Children) {
				Work.Charge(1);
				HasBrokenChildHeader = HasBrokenChildHeader || HasBrokenHeader[Child];
			}
			const bool IsBodyForced = Unit.IsChainBody && HasBrokenChildHeader ||
			Unit.ExpansionDependency != NoGroup && States[Unit.ExpansionDependency] == GroupState::Broken;
			bool HasEvaluatedProcedure = false;
			switch(Unit.Procedure) {
			case DecisionProcedure::BraceBody:
				if(Unit.Members.size() > 1) {
					BraceChoice Choice;
					Choice.Body = Group;
					for(const uint32_t Member : Unit.Members) if(Member != Group) Choice.Headers.push_back(Member);
					// 最内の本体だけが，直接の親を辿って連鎖の見出を参照する
					if(MaxChars && !Unit.IsChainBody) {
						for(uint32_t Prefix = Unit.Prefix; Prefix != NoGroup; Prefix = IR.Decisions[Prefix].Prefix) {
							Work.Charge(1);
							const FormatDecision &Outer = IR.Decisions[Prefix];
							if(
								!Outer.IsChainBody || Outer.UnitOwner != Prefix || States[Prefix] != GroupState::Pending ||
								Outer.ExpansionDependency != NoGroup && States[Outer.ExpansionDependency] == GroupState::Broken
							) break;
							Work.Charge(Outer.Members.size());
							Choice.Prefix.insert(Choice.Prefix.end(), Outer.Members.begin(), Outer.Members.end());
						}
					}
					std::sort(Choice.Prefix.begin(), Choice.Prefix.end());
					Choice.Prefix.erase(std::unique(Choice.Prefix.begin(), Choice.Prefix.end()), Choice.Prefix.end());
					HasAppliedMembers =
					HasAppliedBraceBody(States, Conditions, Atoms, Candidates, ConditionalsOf, Choice, IsBodyForced, Idx ? &*Idx : nullptr, Work);
					HasEvaluatedProcedure = true;
				}
				break;
			case DecisionProcedure::Continuation:
				{
					// 括弧外の候補と途中のコンテナの分類及びコンテナ確定後のラッパーの選択
					ContentGroups.clear();
					AnnotationContent.clear();
					EnclosedGroups.clear();
					InnerFlattened.clear();
					for(const uint32_t Member : Unit.Members) {
						Work.Charge(1);
						if(Member == Group || Member == Unit.Annotation) continue;
						if(!IR.Decisions[Member].IsContinuationContent || IR.Decisions[Member].IsContinuationMember) {
							EnclosedGroups.push_back(Member);
							if(IR.Decisions[Member].IsContinuationMember && (Unit.Annotation == NoGroup || Unit.Annotation < Member)) {
								InnerFlattened.push_back(Member);
							}
						} else if(Unit.Annotation != NoGroup && Member < Unit.Annotation) AnnotationContent.push_back(Member);
						else ContentGroups.push_back(Member);
					}
					bool HasMemberBreak = false, HasAnnotationMemberBreak = false;
					if(!EnclosedGroups.empty()) {
						// 外側の未確定の演算・要素区切は残し，接頭部を含む継続括弧無の形の成立を先に確かめる
						const bool IsInnerUnwrapped = !Unit.IsContinuationList && !Unit.IsContinuationType && CanFlattenContinuation(
							States,
							Conditions,
							Atoms,
							Candidates,
							ConditionalsOf,
							Group,
							Unit.Prefix,
							InnerFlattened,
							Idx ? &*Idx : nullptr,
							std::span<const size_t>(),
							Work
						);
						InnerFlattened.clear();
						InnerCandidates.clear();
						if(IsInnerUnwrapped) {
							InnerFlattened.push_back(Group);
							if(Idx) {
								Work.Charge(Candidates[Group].size());
								InnerCandidates.assign(Candidates[Group].begin(), Candidates[Group].end());
							}
							if(Unit.Prefix != NoGroup) {
								Work.Charge(1);
								InnerFlattened.push_back(Unit.Prefix);
								if(Idx) {
									Work.Charge(Candidates[Unit.Prefix].size());
									InnerCandidates.insert(InnerCandidates.end(), Candidates[Unit.Prefix].begin(), Candidates[Unit.Prefix].end());
								}
							}
						}
						std::sort(InnerFlattened.begin(), InnerFlattened.end());
						std::sort(InnerCandidates.begin(), InnerCandidates.end());
						InnerCandidates.erase(std::unique(InnerCandidates.begin(), InnerCandidates.end()), InnerCandidates.end());
						std::optional<BoundaryProjection> Projection;
						if(Idx && !InnerCandidates.empty()) Projection.emplace(Idx->Boundaries, InnerCandidates, Work);
						for(const uint32_t Member : EnclosedGroups) {
							// 演算・要素区切の仮の境界と継続記号を反映した，コンテナの１度の確定
							const bool IsTypeContent = Unit.IsContinuationType || Unit.Annotation != NoGroup && Member < Unit.Annotation;
							States[Member] = CanFlatten(
								States,
								Conditions,
								Atoms,
								Candidates[Member],
								ConditionalsOf[Member],
								Member,
								Idx ? &*Idx : nullptr,
								Work,
								IsTypeContent ? std::span<const uint32_t>() : std::span<const uint32_t>(InnerFlattened),
								IsTypeContent ? std::span<const size_t>() : std::span<const size_t>(ConditionalsOf[Group]),
								!IsTypeContent && Projection ? &*Projection : nullptr,
								IsTypeContent || IsInnerUnwrapped ? NoGroup : Group
							) ? GroupState::Flat : GroupState::Broken;
							if(IR.Decisions[Member].IsContinuationMember && States[Member] == GroupState::Broken) {
								if(Unit.Annotation != NoGroup && Member < Unit.Annotation) HasAnnotationMemberBreak = true;
								else HasMemberBreak = true;
							}
							Update(std::span<const uint32_t>(&Member, 1));
						}
					}
					if(Unit.Annotation != NoGroup) {
						// 後続の開き括弧を含めた型注釈のラッパーの確定
						const uint32_t Annotation = Unit.Annotation;
						const bool IsAnnotationFlat = !HasAnnotationMemberBreak && CanFlattenContinuation(
							States,
							Conditions,
							Atoms,
							Candidates,
							ConditionalsOf,
							Annotation,
							IR.Decisions[Annotation].Prefix,
							AnnotationContent,
							Idx ? &*Idx : nullptr,
							ConditionalsOf[Group],
							Work,
							Group
						);
						States[Annotation] = IsAnnotationFlat ? GroupState::Flat : GroupState::Broken;
						if(IsAnnotationFlat) {
							for(const uint32_t Member : AnnotationContent) States[Member] = GroupState::Flat;
							AnnotationContent.push_back(Annotation);
							Update(AnnotationContent);
						} else {
							Update(std::span<const uint32_t>(&Annotation, 1));
							for(const uint32_t Member : AnnotationContent) {
								States[Member] = CanFlatten(
									States,
									Conditions,
									Atoms,
									Candidates[Member],
									ConditionalsOf[Member],
									Member,
									Idx ? &*Idx : nullptr,
									Work,
									std::span<const uint32_t>(),
									ConditionalsOf[Group],
									nullptr,
									Group
								) ? GroupState::Flat : GroupState::Broken;
								Update(std::span<const uint32_t>(&Member, 1));
							}
						}
					}
					// 確定済の境界を用いた継続括弧の選択
					bool IsFlat = !HasMemberBreak && CanFlattenContinuation(
						States,
						Conditions,
						Atoms,
						Candidates,
						ConditionalsOf,
						Group,
						Unit.Prefix,
						ContentGroups,
						Idx ? &*Idx : nullptr,
						std::span<const size_t>(),
						Work,
						NoGroup,
						Unit.IsContinuationList
					);
					// 内部を折れず，独立した行でも幅を超える内容には継続括弧を採用しない
					if(!IsFlat && MaxChars && ContentGroups.empty() && Candidates[Group].size() == 2) {
						FlatSummary Content;
						bool HasBoundary = false;
						for(size_t Pos = Candidates[Group].front() + 1; Pos < Candidates[Group].back(); ++Pos) {
							Work.Charge(1);
							const Atom &Piece = Atoms[Pos];
							if(!IsVisible(Conditions, Piece, NoGroup)) continue;
							if(IsBoundary(States, Piece, NoGroup)) {
								HasBoundary = true;
								break;
							}
							if(HasWidth(Piece)) {
								const LexemeSummary Summary = Work.Measure(IR, Piece.Text);
								Content = JoinFlat(Content, { Summary.Head, Summary.Tail, Summary.Max, false, Summary.HasNewline });
							}
						}
						if(!HasBoundary && Content.Max > MaxChars) IsFlat = true;
					}
					States[Group] = IsFlat ? GroupState::Flat : GroupState::Broken;
					Conditions[Group] = !IsFlat;
					if(IsFlat) {
						for(const uint32_t Member : ContentGroups) {
							States[Member] = GroupState::Flat;
							Conditions[Member] = 0;
						}
						ContentGroups.push_back(Group);
						Update(ContentGroups);
					} else {
						// 採用したラッパーのインデックスへの反映と，内容の各グループの後行順での１度の確定
						Update(std::span<const uint32_t>(&Group, 1));
						for(const uint32_t Member : ContentGroups) {
							States[Member] =
							CanFlatten(States, Conditions, Atoms, Candidates[Member], ConditionalsOf[Member], Member, Idx ? &*Idx : nullptr, Work) ?
							GroupState::Flat :
							GroupState::Broken;
							Update(std::span<const uint32_t>(&Member, 1));
						}
					}
					HasAppliedMembers = true;
					HasEvaluatedProcedure = true;
				}
				break;
			}
			if(!HasEvaluatedProcedure) {
				if(Unit.Members.size() > 1) {
					// 相互依存するラッパーの全平坦形の先行した測定と，棄却時だけのラッパーの採用
					std::vector<size_t> &Combined = Work.DecisionCandidates, &CombinedConditionals = Work.DecisionConditions;
					Combined.clear();
					CombinedConditionals.clear();
					for(const uint32_t Member : Unit.Members) {
						Work.Charge(Candidates[Member].size() + ConditionalsOf[Member].size() + 1);
						Combined.insert(Combined.end(), Candidates[Member].begin(), Candidates[Member].end());
						CombinedConditionals.insert(CombinedConditionals.end(), ConditionalsOf[Member].begin(), ConditionalsOf[Member].end());
					}
					std::sort(Combined.begin(), Combined.end());
					std::sort(CombinedConditionals.begin(), CombinedConditionals.end());
					const bool IsFlat =
					CanFlatten(States, Conditions, Atoms, Combined, CombinedConditionals, Group, Idx ? &*Idx : nullptr, Work, Unit.Members);
					States[Group] = IsFlat ? GroupState::Flat : GroupState::Broken;
					if(IsFlat) for(const uint32_t Member : Unit.Members) States[Member] = GroupState::Flat;
					else {
						Update(std::span<const uint32_t>(&Group, 1));
						for(const uint32_t Member : Unit.Members) if(Member != Group) {
							States[Member] =
							CanFlatten(States, Conditions, Atoms, Candidates[Member], ConditionalsOf[Member], Member, Idx ? &*Idx : nullptr, Work) ?
							GroupState::Flat :
							GroupState::Broken;
							Update(std::span<const uint32_t>(&Member, 1));
						}
						HasAppliedMembers = true;
					}
				} else if(IsBodyForced) States[Group] = GroupState::Broken;
				else if(IsFixedGroup[Group]) {
					Work.Charge(1);
					++Work.Evaluations;
					States[Group] = Fixed->CanFlatten(Conditions, Atoms, Candidates[Group], MaxChars, Work) ? GroupState::Flat : GroupState::Broken;
				} else {
					States[Group] =
					CanFlatten(States, Conditions, Atoms, Candidates[Group], ConditionalsOf[Group], Group, Idx ? &*Idx : nullptr, Work) ?
					GroupState::Flat :
					GroupState::Broken;
				}
			}
			bool HasOwnBrokenHeader = false;
			if(Unit.Procedure == DecisionProcedure::BraceBody) for(const uint32_t Member : Unit.Members) {
				HasOwnBrokenHeader = HasOwnBrokenHeader || Member != Group && States[Member] == GroupState::Broken;
			}
			const bool HasHeader =
			HasOwnBrokenHeader || (Unit.Procedure != DecisionProcedure::BraceBody || Unit.IsChainBody) && HasBrokenChildHeader;
			for(const uint32_t Member : Unit.Members) HasBrokenHeader[Member] = HasHeader;
			if(!HasAppliedMembers) Update(Unit.Members);
			for(const uint32_t Member : Unit.Members) {
				if(Measured) ++Work.GroupFinalizations[Member];
				++Finalized;
			}
			for(const uint32_t Successor : IR.Decisions[Group].Successors) {
				const uint32_t Rank = Successor;
				Work.Charge(1);
				++Work.DependencyNotifications;
				if(!--Unresolved.at(Rank)) {
					Ready.push_back(Rank);
					std::push_heap(Ready.begin(), Ready.end(), IsLater);
				}
			}
		}
		// 未解決の依存の不変条件違反に依る終了
		if(Finalized != GroupCount) {
			throw FormatLimitExceeded("the format IR has unresolved decision dependencies", FailureReason::FormatterDefect);
		}
		// 評価を終えた表を，解決済の要素を確保する前に解放する
		Idx.reset();
		Fixed.reset();
		BoundaryLists(&State.Storage).swap(Candidates);
		BoundaryLists(&State.Storage).swap(ConditionalsOf);
		State.Storage.release();
		std::vector<FormatDecision>().swap(IR.Decisions);
		std::vector<FormatItemId>().swap(IR.Decls);
		std::vector<uint8_t>().swap(HasBrokenHeader);
		std::vector<uint8_t>().swap(IsFixedGroup);
		std::vector<size_t>().swap(Unresolved);
		std::vector<uint32_t>().swap(Ready);
		std::vector<size_t>().swap(UpdatePositions);
		// 観測を要求する呼出には幅要約も渡し，通常の出力では最後の読取後に解放する
		if(!Measured) {
			Work.Summaries.reset();
			std::vector<size_t>().swap(Work.ZeroSegments);
			std::vector<size_t>().swap(Work.TextBreaks);
		}
		// 選択したグループと宣言の区分の期待結果の確定
		BrokenIR Result;
		Result.Items.reserve(Atoms.size());
		Result.Expectation.NormalizedSyntax = IR.NormalizedSyntax;
		Result.Expectation.DeclarationPartitions = std::move(Partitions);
		Result.Expectation.SelectedForms = std::move(State.Forms);
		for(uint32_t Group = 0; Group < GroupCount; ++Group) {
			Result.Expectation.SelectedForms[Group].IsBroken = States[Group] == GroupState::Broken;
		}
		// 解決済の要素の追加
		std::vector<std::pair<NodeId, size_t>> OpenDecls;
		size_t Line = 0, LastContentLine = 0;
		// 解決済の断片の出力と，空行判定用の複数行の宣言の記録
		const auto Emit =
		[&](const ResolvedKind Kind, const FormatRange Text, const NodeId Node, const bool HasNewline = false) -> void {
			Result.Items.emplace_back();
			ResolvedItem &Item = Result.Items.back();
			Item.Kind = Kind;
			Item.Text = Text;
			Item.Node = Node;
			switch(Kind) {
			case ResolvedKind::AnchorBegin:
				if(IR.BlankRefs.contains(Node)) OpenDecls.emplace_back(Node, Line);
				break;
			case ResolvedKind::AnchorEnd:
				if(!OpenDecls.empty() && OpenDecls.back().first == Node) {
					if(LastContentLine > OpenDecls.back().second) Result.Multiline.insert(Node);
					OpenDecls.pop_back();
				}
				break;
			case ResolvedKind::BlankLine:
				++Result.LineBreakCount;
				break;
			case ResolvedKind::Newline:
				++Result.LineBreakCount;
				++Line;
				break;
			case ResolvedKind::OwnedRegion:
				LastContentLine = Line++;
				break;
			case ResolvedKind::Verbatim:
				++Result.TextItemCount;
				if(HasNewline) {
					++Line;
					if(IR.BlankRefs.contains(Node)) Result.Multiline.insert(Node);
				}
				LastContentLine = Line;
				break;
			case ResolvedKind::Text:
			case ResolvedKind::Separator:
				++Result.TextItemCount;
				LastContentLine = Line;
				break;
			}
			// 終了
			return;
		};
		// 確定した境界とトークンの出力
		for(const Atom &Piece : Atoms) {
			Work.Charge(1);
			// 選ばなかった形に従う断片の読飛
			if(!IsVisible(Conditions, Piece, NoGroup)) continue;
			const FormatItemDetail &Detail = IR.GetDetailOf(Piece.Detail);
			if(
				Detail.PunctuationOwner != NoNode &&
				(Piece.Generated == GeneratedToken::ListSeparator || Piece.Generated == GeneratedToken::StmtTerminator)
			) {
				const FormOperation Operation =
				Piece.Generated == GeneratedToken::ListSeparator ? FormOperation::TrailingComma : FormOperation::MemberSeparator;
				const SelectedFormEffect Effect { Detail.PunctuationOwner, Operation, Detail.DeclOwner, Detail.DeclRow };
				if(Piece.Cond == NoGroup) {
					// 幅で選ぶグループを持たないコンテナも，採用した句読点を同じ形式一覧へ載せる
					SelectedForm Form;
					Form.IsBroken = Piece.Generated == GeneratedToken::ListSeparator;
					Form.Nodes.push_back(Detail.PunctuationOwner);
					Form.Effects.push_back(Effect);
					Result.Expectation.SelectedForms.push_back(std::move(Form));
				} else Result.Expectation.SelectedForms[Piece.Cond].Effects.push_back(Effect);
			}
			switch(Piece.Kind) {
			case AtomKind::EdgeSpace:
				{
					const size_t Pos = static_cast<size_t>(&Piece - Atoms.data());
					if(
						const bool IsAtEdge =
						IsLineEdge(States, Conditions, Atoms, Pos, -1, NoGroup, Work) || IsLineEdge(States, Conditions, Atoms, Pos, 1, NoGroup, Work);
						!IsAtEdge && Piece.Text.Size
					) Emit(ResolvedKind::Text, Piece.Text, Piece.Node);
				}
				break;
			case AtomKind::Text:
				if(Piece.Text.Size) {
					Emit(ResolvedKind::Text, Piece.Text, Piece.Node);
					Result.Items.back().Generated = Piece.Generated;
				}
				break;
			case AtomKind::Separator:
				Emit(ResolvedKind::Separator, Piece.Text, NoNode);
				break;
			case AtomKind::Soft:
				if(Piece.Group != NoGroup && States[Piece.Group] == GroupState::Broken) Emit(ResolvedKind::Newline, {}, NoNode);
				else if(Piece.Text.Size) Emit(ResolvedKind::Text, Piece.Text, NoNode);
				break;
			case AtomKind::Hard:
				Emit(ResolvedKind::Newline, {}, NoNode);
				break;
			case AtomKind::Blank:
				Emit(ResolvedKind::BlankLine, {}, Piece.Node);
				Result.Items.back().Before = Piece.Node;
				Result.Items.back().After = Detail.After;
				Result.Items.back().IsForced = Detail.IsForced;
				break;
			case AtomKind::Verbatim:
				Emit(ResolvedKind::Verbatim, Piece.Text, Piece.Node, Piece.HasBlankNewline);
				break;
			case AtomKind::OwnedRegion:
				Emit(ResolvedKind::OwnedRegion, {}, Piece.Node);
				break;
			case AtomKind::IndentBegin:
				Emit(ResolvedKind::IndentBegin, {}, NoNode);
				break;
			case AtomKind::IndentEnd:
				Emit(ResolvedKind::IndentEnd, {}, NoNode);
				break;
			case AtomKind::AnchorBegin:
				Emit(ResolvedKind::AnchorBegin, {}, Piece.Node);
				break;
			case AtomKind::AnchorEnd:
				Emit(ResolvedKind::AnchorEnd, {}, Piece.Node);
				break;
			}
		}
		Result.Texts = std::move(IR.Texts);
		#if defined(ShaveFormatRefFacts)
		RefText = std::string_view();
		#endif
		if(Observation) Observation->CompleteSelections(Result.Expectation.SelectedForms);
		if(Measured) *Measured = std::move(Work);
		// 解決済の中間表現の返戻
		return Result;
	}

	/**
	 * 選択の解決関数
	 * 計算量：要素数 N，グループ数 G と順序の構築・測定で参照する要素の累計 K に対し O(N + G log G + K log(K + N + G + 1))
	 * @param IR 幅非依存の中間表現
	 * @param Observation 製品の予算を変えない読取専用な観測
	 * @return 全ての選択を解決した中間表現
	 * @throws FormatLimitExceeded 中間表現の不整合又は作業上限超過の場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	BrokenIR LineBreaker::Resolve(FormatIR IR, CoreObservation *const Observation) const {
		// 幅要約を再利用した結果の返戻
		return ResolveMeasured(std::move(IR), nullptr, Observation);
	}

	/**
	 * コンストラクタ（行幅上限の設定）
	 * @param Chars １行の最大文字数（０は無制限）
	 */
	LineBreaker::LineBreaker(const size_t Chars) : MaxChars(Chars) {}
} // namespace Core
