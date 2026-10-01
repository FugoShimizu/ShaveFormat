#include "OutputFinalizer.hpp"
#include "../Core/SyntaxModel.hpp"
#include "FormatLimitExceeded.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/** ========== 出力確定 ========== */
/**
 * 出力確定関数（整形仕様 3.3, 6.7）
 * 合成で出力の順に並べたコメント行の列を，コードの断片と其の順に直列化して終端契約を満たす
 * 独立したコメント行を終える改行は出すが，空の物理行を生む改行は出さず，空行の断片を変更しない
 * 計算量：節点数 V，空のコードを持つ所有区画の数 R，子を含む字面の長さ N，割当の数 C に対し O(V + R log R + N + C)
 * @param Document コードとコメントの合成結果
 * @param VerbatimLines 逐語の中から始まる行の番号（０始まりの昇順，整形仕様 4.4 の所有区画を親の深さへ置く時に字下げを付けない行）の格納先（不要なら nullptr）
 * @return 最終字面（逐語の末尾データは其の終端を保ち，其の他は終端契約の改行に従う）
 * @throws FormatLimitExceeded コメントだけを持つ所有区画の開始タグを置いて居ない場合，子を含む出力が大きさの上限を超える場合
 */
std::string OutputFinalizer::Finalize(const OutputDocument &Document, std::vector<uint32_t> *const VerbatimLines) {
	// コード断片とコメント割当の取得及び出力領域の確保
	const LaidOutCode &Code = Document.GetCode();
	const std::span<const CodeFragment> Fragments = Code.GetFragments();
	const std::span<const CommentInsertion> Insertions = Document.GetComments().GetInsertions();
	const AnchorPositionMap &Anchors = Code.GetAnchors();
	const std::span<const OwnedRegionSpan> Owned =
	Document.IncludesOwnedComments() ? std::span<const OwnedRegionSpan>(Anchors.OwnedRegions) : std::span<const OwnedRegionSpan>();
	// コード行の無い子のコメントを開始タグの後へ差し込む位置
	struct DeferredRegion {
		const Core::OwnedRegion *Region; // コメントだけを持つ所有区画
		uint32_t Fragment; // 開始タグの終わる断片
		uint32_t Offset; // 開始タグの終わりの字面の位置
		uint32_t Depth; // 区画の行の深さ
	};
	std::vector<DeferredRegion> Deferred;
	if(Document.IncludesOwnedComments() && Anchors.Syntax->HasRegions()) {
		for(Core::NodeId Node = 0; Node < Anchors.Syntax->NodeCount(); ++Node) {
			const Core::OwnedRegion *const Region = Anchors.Syntax->RegionOf(Node);
			if(!Region || !Region->CodeText.empty() || Region->Text.empty()) continue;
			const Core::NodeId Parent = Anchors.Syntax->ParentOf(Node);
			Core::NodeId Opening = Core::NoNode;
			if(Parent != Core::NoNode) for(const Core::NodeId Child : Anchors.Syntax->ChildrenOf(Parent)) {
				if(Anchors.Syntax->Is(Child, Core::Role::MarkupOpen)) Opening = Child;
			}
			if(Opening == Core::NoNode || !Anchors.Nodes[Opening].IsPlaced) {
				throw FormatLimitExceeded("embedded comment has no opening boundary", FailureReason::FormatterDefect);
			}
			const NodeSpan &Span = Anchors.Nodes[Opening];
			Deferred.push_back({ Region, Span.EndFragment, Span.End, Anchors.FragmentTabs[Span.EndFragment] + 1 });
		}
	}
	std::stable_sort(
		Deferred.begin(),
		Deferred.end(),
		[](const DeferredRegion &Left, const DeferredRegion &Right) -> bool {
			return Left.Fragment != Right.Fragment ? Left.Fragment < Right.Fragment : Left.Offset < Right.Offset;
		}
	);
	std::string Out;
	Out.reserve(Document.GetSize() + 1);
	// 次に出す割当
	size_t Next = 0;
	// 次に出す割当が断片の配置枠の種類かの判定
	const auto IsNextAt = [&Insertions, &Next](const uint32_t Fragment, const CommentSlotKind Kind) -> bool {
		// 割当を出し終えた場合の返戻
		if(Next >= Insertions.size()) return false;
		const CommentSlotId &Slot = Insertions[Next].Slot;
		// 断片と種類の一致の返戻
		return Slot.Fragment == Fragment && Slot.Kind == Kind;
	};
	// 次の割当のコメント行の列の追記（行の間だけに改行を置く）
	const auto AppendNext = [&Out, &Insertions, &Next]() -> void {
		bool IsFirst = true;
		for(const std::string &Line : Insertions[Next++].Comment.GetLines()) {
			if(!IsFirst) Out += '\n';
			Out += Line;
			IsFirst = false;
		}
	};
	// 逐語の中から始まる行の番号と，其の数え終えた字面の位置
	const std::vector<uint8_t> &VerbatimBreaks = Code.GetAnchors().VerbatimBreaks;
	if(VerbatimLines) VerbatimLines->clear();
	uint32_t Line = 0;
	size_t Counted = 0, NextOwned = 0, NextDeferred = 0;
	// 子の最終字面の行を，親の所属深さで出す
	const auto AppendOwned = [&Out](const Core::OwnedRegion &Region, const uint32_t Depth) -> void {
		const std::string_view Child = Region.Text;
		uint32_t RowNumber = 0;
		for(size_t Cursor = 0; Cursor < Child.size(); ++RowNumber) {
			const size_t End = std::min(Child.find('\n', Cursor), Child.size());
			const std::string_view Row(Child.data() + Cursor, End - Cursor);
			if(RowNumber) Out += '\n';
			if(!Row.empty() && !std::ranges::binary_search(Region.VerbatimLines, RowNumber)) Out.append(Depth, '\t');
			Out.append(Row);
			Cursor = End + 1;
		}
		if(Out.size() > MaxInputBytes) throw FormatLimitExceeded("output exceeds size limit");
	};
	for(uint32_t Index = 0; Index < Fragments.size(); ++Index) {
		const CodeFragment &Fragment = Fragments[Index];
		// 断片の区切りの改行
		if(Index) Out += '\n';
		// 行の前の独立したコメント（行を終える改行を添える）
		while(IsNextAt(Index, CommentSlotKind::BeforeLine)) {
			AppendNext();
			Out += '\n';
		}
		if(NextOwned < Owned.size() && Owned[NextOwned].FirstFragment == Index) {
			const OwnedRegionSpan &Span = Owned[NextOwned++];
			const Core::OwnedRegion &Region = *Anchors.Syntax->RegionOf(Span.Node);
			uint32_t Depth = Anchors.FragmentTabs[Index];
			for(uint32_t Row = Index; Row < Span.EndFragment; ++Row) if(!Fragments[Row].IsBlank) {
				Depth = Anchors.FragmentTabs[Row];
				break;
			}
			AppendOwned(Region, Depth);
			Index = Span.EndFragment - 1;
			while(IsNextAt(Index, CommentSlotKind::LineEnd)) AppendNext();
			while(IsNextAt(Index, CommentSlotKind::OwnLine)) {
				Out += '\n';
				AppendNext();
			}
			continue;
		}
		// 前の断片が逐語の中の改行で終わる行（生成部分と逐語部分の区別，整形仕様 3.2）
		if(VerbatimLines && Index && Index <= VerbatimBreaks.size() && VerbatimBreaks[Index - 1] & AnchorPositionMap::NextInVerbatim) {
			Line += static_cast<uint32_t>(std::count(Out.begin() + static_cast<std::ptrdiff_t>(Counted), Out.end(), '\n'));
			Counted = Out.size();
			VerbatimLines->push_back(Line);
		}
		// 文中のコメントを位置順に差し込んだコード行
		uint32_t Copied = Fragment.Start;
		while(IsNextAt(Index, CommentSlotKind::Inline) || NextDeferred < Deferred.size() && Deferred[NextDeferred].Fragment == Index) {
			const bool IsDeferred = NextDeferred < Deferred.size() && Deferred[NextDeferred].Fragment == Index &&
			(!IsNextAt(Index, CommentSlotKind::Inline) || Deferred[NextDeferred].Offset <= Insertions[Next].Slot.Offset);
			const uint32_t Offset = IsDeferred ? Deferred[NextDeferred].Offset : Insertions[Next].Slot.Offset;
			Out.append(Fragment.Text, Copied - Fragment.Start, Offset - Copied);
			Copied = Offset;
			if(IsDeferred) {
				const DeferredRegion &Region = Deferred[NextDeferred++];
				Out += '\n';
				AppendOwned(*Region.Region, Region.Depth);
				if(Offset < Fragment.End) {
					Out += '\n';
					Out.append(Region.Depth - 1, '\t');
				}
			} else AppendNext();
		}
		Out.append(Fragment.Text, Copied - Fragment.Start, Fragment.End - Copied);
		// 行末のコメントと後の独立した行のコメント
		while(IsNextAt(Index, CommentSlotKind::LineEnd)) AppendNext();
		while(IsNextAt(Index, CommentSlotKind::OwnLine)) {
			Out += '\n';
			AppendNext();
		}
	}
	// 文書の末尾の独立した行のコメント
	while(Next < Insertions.size()) {
		if(!Out.empty()) Out += '\n';
		AppendNext();
	}
	// コードが空でもコメントの行を終える改行を出す終端契約（整形仕様 6.7）
	if(!Out.empty() && (Code.GetEnding().EndsWithNewline || Fragments.empty())) Out += '\n';
	// 最終字面の返戻
	return Out;
}
