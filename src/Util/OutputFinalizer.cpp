#include "OutputFinalizer.hpp"
#include "LayoutTypes.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

/**
 * ========== 出力確定 ==========
 * 出力確定関数（整形仕様 3.3, 6.7）
 * 合成で出力の順に並べたコメント行の列を，コードの断片と其の順にシリアライズして終端契約を満たす
 * 独立したコメント行を終える改行は出すが，空の物理行を生む改行は出さず，空行の断片を変更しない
 * 計算量：断片の数 F，テキストの長さ N，割当の数 C に対し O(F + N + C)
 * @param Doc コードとコメントの合成結果
 * @return 最終テキスト（逐語の末尾データは其の終端を保ち，其の他は終端契約の改行に従う）
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 */
std::string OutputFinalizer::Finalize(const OutputDocument &Doc) {
	// コード断片とコメント割当の取得
	const LaidOutCode &Code = Doc.GetCode();
	const std::span<const CodeFragment> Fragments = Code.GetFragments();
	const std::span<const CommentInsertion> Insertions = Doc.GetComments().GetInsertions();
	// 合成時に受理済の正確な長さと終端契約から確保量を取得
	const size_t Size = Doc.GetSize();
	const bool HasEnding = Size && (Code.GetEnding().DoesEndWithNewline || Fragments.empty());
	std::string Output;
	Output.reserve(Size + HasEnding);
	// 次に出す割当
	size_t Next = 0;
	// 次に出す割当が断片の配置枠の種類かの判定
	const auto IsNextAt = [&Insertions, &Next](const uint32_t Fragment, const CommentSlotKind Kind) -> bool {
		// 割当を出し終えた場合の返戻
		if(Next >= Insertions.size()) return false;
		const CommentSlotId &Slot = Insertions[Next].GetSlot();
		// 断片と種類の一致の返戻
		return Slot.Fragment == Fragment && Slot.Kind == Kind;
	};
	// 次の割当のコメント行の列の追記（行の間だけに改行を置く）
	const auto AppendNext = [&Output, &Insertions, &Next]() -> void {
		bool IsFirst = true;
		for(const std::string &Line : Insertions[Next++].GetComment().GetLines()) {
			if(!IsFirst) Output += '\n';
			Output += Line;
			IsFirst = false;
		}
		// 終了
		return;
	};
	for(uint32_t Idx = 0; Idx < Fragments.size(); ++Idx) {
		const CodeFragment &Fragment = Fragments[Idx];
		// 断片の区切の改行
		if(Idx) Output += '\n';
		// 此の行の割当が無ければ，種類毎に同じ配置先を照合せずコード行だけを出す
		if(Next == Insertions.size() || Insertions[Next].GetSlot().Fragment != Idx) {
			Fragment.AppendTo(0, Fragment.End - Fragment.Start, Output);
			continue;
		}
		// 行の前の独立したコメント（行を終える改行を添える）
		while(IsNextAt(Idx, CommentSlotKind::BeforeLine)) {
			AppendNext();
			Output += '\n';
		}
		// 文中のコメントを位置順に差し込んだコード行
		uint32_t Copied = Fragment.Start;
		while(IsNextAt(Idx, CommentSlotKind::Inline)) {
			const uint32_t Offset = Insertions[Next].GetSlot().Offset;
			Fragment.AppendTo(Copied - Fragment.Start, Offset - Copied, Output);
			Copied = Offset;
			AppendNext();
		}
		Fragment.AppendTo(Copied - Fragment.Start, Fragment.End - Copied, Output);
		// 行末のコメントと後の独立した行のコメント
		while(IsNextAt(Idx, CommentSlotKind::LineEnd)) AppendNext();
		while(IsNextAt(Idx, CommentSlotKind::OwnLine)) {
			Output += '\n';
			AppendNext();
		}
	}
	// ドキュメントの末尾の独立した行のコメント
	while(Next < Insertions.size()) {
		if(!Output.empty()) Output += '\n';
		AppendNext();
	}
	// コードが空でもコメントの行を終える改行を出す終端契約（整形仕様 6.7）
	if(HasEnding) Output += '\n';
	// 最終テキストの返戻
	return Output;
}
