#include "LineDiff.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
	using LineIdx = std::unordered_map<std::string_view, std::vector<size_t>>; // 候補の本文毎の行位置
	using CommonLines = std::vector<std::pair<size_t, size_t>>; // 最長共通部分列の原文・候補の行位置
	using Lines = std::vector<std::string_view>; // 本文を所有しない行列

	// 分割して比較する行範囲
	struct Range {
		size_t OldBegin;
		size_t OldEnd;
		size_t NewBegin;
		size_t NewEnd;
	};

	/**
	 * LF を末尾に残した行列の取得関数
	 * 計算量：本文長 B に対し O(B)，追加領域は行数 L に対し O(L)
	 * @param Text 本文
	 * @return 空の本文と空行と末尾改行の不在を区別する行列
	 * @throws std::bad_alloc 行列の領域を確保出来ない場合
	 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
	 */
	Lines Split(const std::string_view Text) {
		// 結果の格納先
		Lines Result;
		for(size_t Begin = 0; Begin < Text.size();) {
			const size_t Break = Text.find('\n', Begin), End = Break == std::string_view::npos ? Text.size() : Break + 1;
			Result.push_back(Text.substr(Begin, End - Begin));
			Begin = End;
		}
		// LF の有無を保つ行列の返戻
		return Result;
	}

	/**
	 * 疎な一致対からの最長共通部分列の取得関数
	 * 計算量：原文行数 L，一致対数 R，最長行長 S に対し平均 O(LS + R log(L + R + 1))，追加領域 O(L + R)
	 * @param Old 原文の行列
	 * @param Idx 候補の本文毎の位置のインデックス
	 * @param Piece 比較する範囲
	 * @return 疎な一致対の最長共通部分列（対が両範囲の行数の和を超す時は密な一致用の探索へ委ねる）
	 * @throws std::bad_alloc 一致対と最長列の領域を確保出来ない場合
	 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
	 */
	std::optional<CommonLines> SparseLcs(const Lines &Old, const LineIdx &Idx, const Range &Piece) {
		// 差分の一致範囲
		struct Matches {
			size_t OldLine;
			std::span<const size_t> NewLines;
		};
		std::vector<Matches> Rows;
		size_t Count = 0;
		const size_t Limit = Piece.OldEnd + Piece.NewEnd - Piece.OldBegin - Piece.NewBegin;
		for(size_t Row = Piece.OldBegin; Row < Piece.OldEnd; ++Row) {
			const LineIdx::const_iterator Found = Idx.find(Old[Row]);
			if(Found == Idx.end()) continue;
			const std::vector<size_t>::const_iterator First = std::lower_bound(Found->second.begin(), Found->second.end(), Piece.NewBegin);
			const std::vector<size_t>::const_iterator Last = std::lower_bound(First, Found->second.end(), Piece.NewEnd);
			Count += static_cast<size_t>(Last - First);
			// 密な一致対の探索への委譲を示す未確定結果の返戻
			if(Count > Limit) return std::nullopt;
			Rows.push_back({ Row, std::span<const size_t>(First, Last) });
		}
		// 差分の一致した行の組
		struct Match {
			size_t OldLine;
			size_t NewLine;
			size_t Prev; // 前の一致の Links の添字（SIZE_MAX は前の一致無）
		};
		std::vector<Match> Links;
		Links.reserve(Count);
		std::vector<size_t> Ends, LastLinks;
		for(const Matches &Row : Rows) for(size_t Pos = Row.NewLines.size(); Pos;) {
			const size_t NewLine = Row.NewLines[--Pos];
			const std::vector<size_t>::iterator End = std::lower_bound(Ends.begin(), Ends.end(), NewLine);
			const size_t Len = static_cast<size_t>(End - Ends.begin());
			if(End != Ends.end() && *End == NewLine) continue;
			Links.push_back({ Row.OldLine, NewLine, Len ? LastLinks[Len - 1] : SIZE_MAX });
			if(End == Ends.end()) {
				Ends.push_back(NewLine);
				LastLinks.push_back(Links.size() - 1);
			} else {
				*End = NewLine;
				LastLinks[Len] = Links.size() - 1;
			}
		}
		CommonLines Common(Ends.size());
		for(size_t Pos = Common.size(), Link = LastLinks.empty() ? SIZE_MAX : LastLinks.back(); Pos;) {
			const Match &Found = Links[Link];
			Common[--Pos] = { Found.OldLine, Found.NewLine };
			Link = Found.Prev;
		}
		// 原文順の最長共通部分列の返戻
		return Common;
	}

	/**
	 * Myers の最短編集路の中点の取得関数
	 * 計算量：両範囲の行数の和 L，残余探索量 R，最長行長 S に対し O(L + RS)，追加領域 O(L)
	 * @param Old 原文の行列
	 * @param New 候補の行列
	 * @param Piece 共通の接頭・接尾行を除いた非空範囲
	 * @param Remaining 差分表示全体で残る探索量（元の両本文のバイト数を初期値とする）
	 * @return 分割する原文・候補の行位置（共通行が無ければ終端）
	 * @throws std::bad_alloc 編集路の領域を確保出来ない場合
	 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
	 */
	std::pair<size_t, size_t> Bisect(const Lines &Old, const Lines &New, const Range &Piece, size_t &Remaining) {
		// 最短編集路を探索する両範囲の行数の取得
		const int64_t OldSize = static_cast<int64_t>(Piece.OldEnd - Piece.OldBegin);
		const int64_t NewSize = static_cast<int64_t>(Piece.NewEnd - Piece.NewBegin), Delta = OldSize - NewSize;
		const int64_t MaxDepth = OldSize + NewSize + 1 >> 1, Offset = MaxDepth + 1;
		const bool IsOdd = Delta % 2;
		std::vector<int64_t> Forward(static_cast<size_t>((MaxDepth << 1) + 3), -1), Backward = Forward;
		Forward[static_cast<size_t>(Offset + 1)] = Backward[static_cast<size_t>(Offset + 1)] = 0;
		int64_t ForwardLower = 0, ForwardUpper = 0, BackwardLower = 0, BackwardUpper = 0;
		for(int64_t Depth = 0; Depth <= MaxDepth; ++Depth) {
			for(int64_t Diagonal = ForwardLower - Depth; Diagonal <= Depth - ForwardUpper; Diagonal += 2) {
				// 探索量を使い切った領域の置換の返戻
				if(!Remaining) return { Piece.OldEnd, Piece.NewEnd };
				--Remaining;
				const size_t Idx = static_cast<size_t>(Offset + Diagonal);
				int64_t OldPos =
				Diagonal == -Depth || Diagonal != Depth && Forward[Idx - 1] < Forward[Idx + 1] ? Forward[Idx + 1] : Forward[Idx - 1] + 1;
				int64_t NewPos = OldPos - Diagonal;
				while(
					OldPos > -1 && NewPos > -1 && OldPos < OldSize && NewPos < NewSize &&
					Old[Piece.OldBegin + static_cast<size_t>(OldPos)] == New[Piece.NewBegin + static_cast<size_t>(NewPos)]
				) {
					// 探索量を使い切った領域の置換の返戻
					if(!Remaining) return { Piece.OldEnd, Piece.NewEnd };
					--Remaining;
					++OldPos;
					++NewPos;
				}
				Forward[Idx] = OldPos;
				const int64_t Reverse = Delta - Diagonal;
				if(OldPos > OldSize) ForwardUpper += 2;
				else if(NewPos > NewSize) ForwardLower += 2;
				else if(IsOdd && Reverse > -Depth && Reverse < Depth) {
					if(const int64_t Other = Backward[static_cast<size_t>(Offset + Reverse)]; Other > -1 && OldPos >= OldSize - Other) {
						// 前後の最短編集路が接した中点の返戻
						return { Piece.OldBegin + static_cast<size_t>(OldPos), Piece.NewBegin + static_cast<size_t>(NewPos) };
					}
				}
			}
			for(int64_t Diagonal = BackwardLower - Depth; Diagonal <= Depth - BackwardUpper; Diagonal += 2) {
				// 探索量を使い切った領域の置換の返戻
				if(!Remaining) return { Piece.OldEnd, Piece.NewEnd };
				--Remaining;
				const size_t Idx = static_cast<size_t>(Offset + Diagonal);
				int64_t OldPos =
				Diagonal == -Depth || Diagonal != Depth && Backward[Idx - 1] < Backward[Idx + 1] ? Backward[Idx + 1] : Backward[Idx - 1] + 1;
				int64_t NewPos = OldPos - Diagonal;
				while(
					OldPos > -1 && NewPos > -1 && OldPos < OldSize && NewPos < NewSize &&
					Old[Piece.OldEnd - static_cast<size_t>(OldPos) - 1] == New[Piece.NewEnd - static_cast<size_t>(NewPos) - 1]
				) {
					// 探索量を使い切った領域の置換の返戻
					if(!Remaining) return { Piece.OldEnd, Piece.NewEnd };
					--Remaining;
					++OldPos;
					++NewPos;
				}
				Backward[Idx] = OldPos;
				const int64_t Reverse = Delta - Diagonal;
				if(OldPos > OldSize) BackwardUpper += 2;
				else if(NewPos > NewSize) BackwardLower += 2;
				else if(!IsOdd && Reverse >= -Depth && Reverse <= Depth) {
					if(const int64_t Other = Forward[static_cast<size_t>(Offset + Reverse)]; Other > -1 && Other >= OldSize - OldPos) {
						// 後ろから辿った最短編集路との中点の返戻
						return { Piece.OldBegin + static_cast<size_t>(OldSize - OldPos), Piece.NewBegin + static_cast<size_t>(NewSize - NewPos) };
					}
				}
			}
		}
		// 共通行を持たない範囲の終端の返戻
		return { Piece.OldEnd, Piece.NewEnd };
	}
} // namespace

/**
 * 最長共通部分列に依る行単位の挿入・削除列の取得関数
 * 疎な一致の範囲は平均 O(B + L S + R log(L + R + 1))（一致対数 R，R は範囲の行数の和以下）
 * 共通行が無い範囲は行のインデックスを照会するだけで平均 O(B + L S)
 * 計算量：入力長 B，行数の和 L，最長行長 S に対し O(BS + LS log(L + 1))，追加領域 O(L)
 * @param Original 原文（改行を正規化しない）
 * @param Candidate 候補
 * @return 共通行を省いた編集列（本文のバイト数分の探索後は領域の置換を使う）（同じテキストへの一致は最初の位置を保ち，変更区間は削除を先に出す）
 * @throws std::bad_alloc 行列・編集路・結果の領域を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::vector<LineChange> CompareLines(const std::string_view Original, const std::string_view Candidate) {
	// 比較する原文と候補の行列の取得
	const Lines Old = Split(Original), New = Split(Candidate);
	LineIdx Idx;
	for(size_t Row = 0; Row < New.size(); ++Row) Idx[New[Row]].push_back(Row);
	std::vector<Range> Pending { { 0, Old.size(), 0, New.size() } };
	std::vector<LineChange> Result;
	size_t Remaining = Original.size() + Candidate.size();
	while(!Pending.empty()) {
		Range Piece = Pending.back();
		Pending.pop_back();
		while(Piece.OldBegin < Piece.OldEnd && Piece.NewBegin < Piece.NewEnd && Old[Piece.OldBegin] == New[Piece.NewBegin]) {
			++Piece.OldBegin;
			++Piece.NewBegin;
		}
		while(Piece.OldBegin < Piece.OldEnd && Piece.NewBegin < Piece.NewEnd && Old[Piece.OldEnd - 1] == New[Piece.NewEnd - 1]) {
			--Piece.OldEnd;
			--Piece.NewEnd;
		}
		if(Piece.OldBegin == Piece.OldEnd) for(size_t Pos = Piece.NewBegin; Pos < Piece.NewEnd; ++Pos) {
			Result.push_back({ true, Pos, New[Pos] });
		} else if(Piece.NewBegin == Piece.NewEnd) for(size_t Pos = Piece.OldBegin; Pos < Piece.OldEnd; ++Pos) {
			Result.push_back({ false, Pos, Old[Pos] });
		} else if(const std::optional<CommonLines> Common = SparseLcs(Old, Idx, Piece)) {
			size_t OldPos = Piece.OldBegin, NewPos = Piece.NewBegin;
			for(const std::pair<size_t, size_t> &Line : *Common) {
				for(; OldPos < Line.first; ++OldPos) Result.push_back({ false, OldPos, Old[OldPos] });
				for(; NewPos < Line.second; ++NewPos) Result.push_back({ true, NewPos, New[NewPos] });
				++OldPos;
				++NewPos;
			}
			for(; OldPos < Piece.OldEnd; ++OldPos) Result.push_back({ false, OldPos, Old[OldPos] });
			for(; NewPos < Piece.NewEnd; ++NewPos) Result.push_back({ true, NewPos, New[NewPos] });
		} else if(
			const std::pair<size_t, size_t> Middle = Bisect(Old, New, Piece, Remaining);
			Middle.first == Piece.OldBegin && Middle.second == Piece.NewBegin ||
			Middle.first == Piece.OldEnd && Middle.second == Piece.NewEnd
		) {
			for(size_t Pos = Piece.OldBegin; Pos < Piece.OldEnd; ++Pos) Result.push_back({ false, Pos, Old[Pos] });
			for(size_t Pos = Piece.NewBegin; Pos < Piece.NewEnd; ++Pos) Result.push_back({ true, Pos, New[Pos] });
		} else {
			Pending.push_back({ Middle.first, Piece.OldEnd, Middle.second, Piece.NewEnd });
			Pending.push_back({ Piece.OldBegin, Middle.first, Piece.NewBegin, Middle.second });
		}
	}
	// 共通行を省いた差分列の返戻
	return Result;
}
