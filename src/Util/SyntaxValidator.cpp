#include "SyntaxValidator.hpp"
#include "CommentNormalizer.hpp"
#include "FormatLimitExceeded.hpp"
#include "InputNormalizer.hpp"
#include "ParseAdapter.hpp"
#include "SyntaxParser.hpp"
#include "TextEdit.hpp"
#include "WorkingMemory.hpp"
#include "../Core/DocCommentReader.hpp"
#include "../Core/FormatIR.hpp"
#include "../Core/SourceProjector.hpp"
#include "../Core/SyntaxGeneration.hpp"
#include "../Profile/GrammarProfile.hpp"
#include <tree_sitter/api.h>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <memory_resource>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/**
 * 検証分類の取得関数
 * @return 検証の分類
 */
SyntaxValidator::ValidationStatus SyntaxValidator::ValidationResult::GetStatus() const {
	// 分類の返戻
	return Status;
}

/**
 * 静的解析用の事実を補う検証済の候補の取得関数
 * @return 成功の場合だけ存在する候補への書換可能な参照
 */
std::optional<Core::ValidatedOutput> &SyntaxValidator::ValidationResult::GetOutput() {
	// 候補の返戻
	return Output;
}

/**
 * 見送又は失敗の理由の取得関数
 * @return 成功の場合は無，其の他は必須の理由
 */
FailureReason SyntaxValidator::ValidationResult::GetReason() const {
	// 理由の返戻
	return Reason;
}

/**
 * 静的解析用の事実を補う検証済の子の取得関数
 * @return 成功の場合だけ保持する子の列の書換可能なビュー
 */
std::span<SyntaxValidator::ValidatedChild> SyntaxValidator::ValidationResult::GetChildren() {
	// 子の列の返戻
	return Children;
}

/**
 * 検証工程だけの結果の構築口
 * @param Status 検証の分類
 * @param Output 成功の場合だけ保持する候補
 * @param Reason 成功の場合は無，其の他は必須の理由
 * @param Children 成功した候補の子
 * @throws FormatLimitExceeded 分類と保持値が矛盾する場合
 */
SyntaxValidator::ValidationResult::ValidationResult(
	const ValidationStatus Status,
	std::optional<Core::ValidatedOutput> Output,
	const FailureReason Reason,
	std::vector<ValidatedChild> Children
) : Status(Status), Output(std::move(Output)), Reason(Reason), Children(std::move(Children)) {
	const bool IsVerified = Status == ValidationStatus::Verified;
	if(
		IsVerified != this->Output.has_value() || IsVerified != (Reason == FailureReason::None) ||
		!IsVerified && !this->Children.empty()
	) throw FormatLimitExceeded("invalid validation result", FailureReason::FormatterDefect);
	// 終了
	return;
}

/**
 * 行の骨格の取得関数（整形仕様 8.1 の空行の不変性と，通常コメントへ変換したドキュメントの移動先）
 * コメントの範囲を除いた論理行毎に，空行か，空白を除いたコードのテキストかを並べる
 * コメントの内側の改行は行を分けず，コメントだけの行は除く（コメントの行は空行でもコードでもない）
 * 準備・修復前の本文を読み，行毎のテキストを空白の除去と同じ走査で共通の領域へ蓄積する
 * 計算量：テキストの長さ N，コメント数 C に対し O(N + C)
 * @param Text テキスト
 * @param Comments コメントの範囲（位置順，重ならない）
 * @param Code 空白を除いた全てのコード行のテキストの格納先
 * @param CodeStarts コードの論理行の始まりの位置の格納先（要らなければ無い）
 * @return 論理行毎の骨格（空行は空のテキスト）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::vector<SyntaxValidator::SkeletonLine> SyntaxValidator::LineSkeleton(
	const std::string_view Text,
	const std::span<const std::pair<uint32_t, uint32_t>> Comments,
	std::string &Code,
	std::vector<size_t> *const CodeStarts
) {
	// 行骨格と走査位置の初期化
	std::vector<SkeletonLine> Lines;
	Code.clear();
	bool HasComment = false;
	size_t Next = 0, LineStart = 0, CodeStart = 0;
	for(size_t Pos = 0;;) {
		// コメントの範囲の読飛（内側の改行は行を分けない）
		if(Next < Comments.size() && Pos == Comments[Next].first) {
			HasComment = true;
			Pos = std::max<size_t>(Pos, Comments[Next++].second);
			continue;
		}
		if(Pos >= Text.size() || Text[Pos] == '\n') {
			// 論理行の確定（コメントだけの行は除き，末尾の改行の後の空の行は数えない）
			if(Code.size() != CodeStart) {
				Lines.push_back({ false, CodeStart, Code.size() - CodeStart });
				if(CodeStarts) CodeStarts->push_back(LineStart);
			} else if(!HasComment && Pos < Text.size()) Lines.push_back({ true, CodeStart, 0 });
			CodeStart = Code.size();
			HasComment = false;
			// テキストの終わりでの脱出
			if(Pos >= Text.size()) break;
			LineStart = ++Pos;
			continue;
		}
		// 空白を除いたコードの字の蓄積
		if(Text[Pos] != ' ' && Text[Pos] != '\t' && Text[Pos] != '\r') Code += Text[Pos];
		++Pos;
	}
	// 行の骨格の返戻
	return Lines;
}

/**
 * 置く論理行の取得関数（整形仕様 7.3, 7.7, 8.1）
 * 通常コメントとして置く本文の論理行を，行末の空白を除き，内容の無い論理行を除いて並べる
 * 計算量：本文の長さ N に対し O(N)
 * @param Text 期待コメントの出力記法の本文
 * @param StackMarker 行末へ積んだ論理行の開始記号（積まない記法では空）
 * @param Protected 本文のコード例の保護標識（未指定なら保護無）
 * @return 置く論理行
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::vector<std::string> SyntaxValidator::PlacedLines(
	const std::string_view Text,
	const std::string_view StackMarker,
	const std::span<const unsigned char> Protected
) {
	// 本文の論理行の切出
	std::vector<std::string> Lines;
	for(size_t Start = 0; Start <= Text.size();) {
		const size_t End = std::min(Text.find('\n', Start), Text.size());
		size_t Offset = Start;
		std::string_view Rest = Text.substr(Start, End - Start);
		while(true) {
			std::string_view Row = Rest;
			if(!StackMarker.empty()) {
				Row = CommentNormalizer::GetFirstLogicalLine(
					Rest,
					StackMarker,
					Protected.empty() ? Protected : Protected.subspan(Offset, Rest.size())
				);
			}
			while(!Row.empty() && (Row.back() == ' ' || Row.back() == '\t')) Row.remove_suffix(1);
			if(!Row.empty()) Lines.emplace_back(Row);
			if(StackMarker.empty()) break;
			// 読取済の本文の後の区切空白と記号だけを消費する
			size_t Next = Row.size();
			while(Next < Rest.size() && !Rest.substr(Next).starts_with(StackMarker)) ++Next;
			if(Next == Rest.size()) break;
			Next += StackMarker.size();
			Rest.remove_prefix(Next);
			Offset += Next;
		}
		Start = End + 1;
	}
	// 置く論理行の返戻
	return Lines;
}

/**
 * コメントの中の波括弧の数の取得関数
 * 処理系のトークンの規則でテキストを数える（パーサーが読めなかった範囲でも同じ規則で数える為）
 * 引用符の文字列と引用符の無い値の関数の引数はコメントを始めず，行コメントは其れを持つ文法だけで数える
 * 計算量：本文のテキスト長 L，区切記号の長さの和 M に対し O(L M)
 * @param Rules 文法の記法の定義（使用可能な記法）
 * @param Text 数えるテキスト
 * @param UrlHead 引用符の無い引数を取る値の関数の開始記法
 * @return コメントの中の波括弧の数
 */
size_t SyntaxValidator::CountBracesInComments(
	const Core::NotationRules &Rules,
	const std::string_view Text,
	const std::string_view UrlHead
) {
	// ブロックコメントの開きの記法の取得
	const std::string_view BlockOpen = Core::CommentRepository::GetOpenerOf(CommentNotation::Block);
	const std::string_view BlockClose = Core::CommentRepository::GetCloserOf(CommentNotation::Block);
	size_t Count = 0;
	// テキストの走査（行コメントは行末迄，ブロックコメントは閉じ迄を中身とする）
	for(size_t Pos = 0; Pos < Text.size();) {
		const unsigned char Char = Text[Pos];
		const std::string_view Rest = Text.substr(Pos);
		// 引用符の文字列の読飛（バックスラッシュのエスケープを含む）
		if(Char == '"' || Char == '\'') {
			for(++Pos; Pos < Text.size() && Text[Pos] != Char && Text[Pos] != '\n'; ++Pos) if(Text[Pos] == '\\') ++Pos;
			++Pos;
			continue;
		}
		// 引用符の無い値の関数の引数の読飛
		if(!UrlHead.empty() && Rest.starts_with(UrlHead)) {
			Pos = std::min(Text.find(')', Pos), Text.size());
			continue;
		}
		// コメントの中身の終わり（コメントでなければ Pos の儘）
		size_t End = Pos;
		if(
			Rules.IsAvailable(CommentNotation::SlashLine) &&
			Rest.starts_with(Core::CommentRepository::GetOpenerOf(CommentNotation::SlashLine))
		) End = std::min(Text.find('\n', Pos), Text.size());
		else if(Rules.IsAvailable(CommentNotation::Block) && Rest.starts_with(BlockOpen)) {
			const size_t Close = Text.find(BlockClose, Pos + BlockOpen.size());
			End = Close == std::string_view::npos ? Text.size() : Close + BlockClose.size();
		}
		if(End == Pos) {
			++Pos;
			continue;
		}
		// コメントの中身の波括弧の計数
		for(size_t At = Pos; At < End; ++At) if(Text[At] == '{' || Text[At] == '}') ++Count;
		Pos = End;
	}
	// コメントの中の波括弧の数の返戻
	return Count;
}

/**
 * 省略可能型を構成する基底の型と `?` の数の取得関数
 * tree-sitter は連続する `?` を１つの `??` トークン又はネストの optional_type として返す為，トークン毎の長さを数える
 * 計算量：型のラッパーの深さ D，直接の子数 C，テキスト長の和 L に対し O(D + C + L)
 * @param Syntax 投影した構文
 * @param Node 省略可能型
 * @param Base 基底の型の格納先
 * @param Marks `?` の数の格納先
 * @return 全ての後置トークンが `?` だけなら true
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
bool SyntaxValidator::HasOptionalTypeParts(
	const Core::SyntaxModel &Syntax,
	Core::NodeId Node,
	Core::NodeId &Base,
	size_t &Marks
) {
	// 省略可能型を構成する基底の型と `?` の数の取得
	Marks = 0;
	const std::string_view MarkerSpelling = Syntax.GetRules().GetSpelling(Core::Word::OptionalityMarker);
	// 後置記号を１字で表さない記法を照合しない事の返戻
	if(MarkerSpelling.size() != 1) return false;
	while(Syntax.Is(Node, Core::Role::OptionalType)) {
		const std::span<const Core::NodeId> Children = Syntax.GetChildrenOf(Node);
		// 型本体と後置記号に分けれないノードの拒否の返戻
		if(Children.size() < 2 || !Syntax.IsNamed(Children.front())) return false;
		for(size_t Idx = 1; Idx < Children.size(); ++Idx) {
			const Core::NodeId Marker = Children[Idx];
			const std::string_view Text = Syntax.GetLexemeOf(Marker);
			if(Syntax.IsNamed(Marker) || Text.empty() || Text.find_first_not_of(MarkerSpelling.front()) != std::string_view::npos) {
				// `!` や名前付の子を省略可能性記号として数えない事の返戻
				return false;
			}
			Marks += Text.size();
		}
		Node = Children.front();
	}
	Base = Node;
	// 少なくとも１つの省略可能性記号を読めた事の返戻
	return Marks;
}

/**
 * 重なる除外範囲の統合関数
 * 所有領域の全体と内部の区切記法が重なる時も，各範囲を１度で読み飛ばせる並びへ纏める
 * 計算量：範囲の数 R に対し O(R log R)
 * @param Ranges 半開区間の除外範囲（統合した位置順の区間で上書する）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void SyntaxValidator::MergeRanges(ByteRanges &Ranges) {
	// 重なる除外範囲の統合
	std::sort(Ranges.begin(), Ranges.end());
	size_t Written = 0;
	for(const auto &[Start, End] : Ranges) {
		if(Start == End) continue;
		if(Written && Start <= Ranges[Written - 1].second) Ranges[Written - 1].second = std::max(Ranges[Written - 1].second, End);
		else Ranges[Written++] = { Start, End };
	}
	Ranges.resize(Written);
	// 終了
	return;
}

/**
 * 空行とコード行の対応の照合関数（整形仕様 8.1）
 * 空行の有無・数とコード行のテキストを別に照合する
 * エラー領域に接する行の本文差は保持照合へ渡し，空行差に数えない
 * 正常行のテキスト差は，構造が一致する時だけ空行と構文境界の対応の変化として扱う
 * 計算量：本文長 B と領域・断片・アンカーの総数 N に対し O(B + N)
 * @param Checked 検証の為に解析し直した最終テキスト
 * @param Text 準備・修復前の最終テキスト
 * @param Comments 最終テキストでコメントと読まれる範囲（位置順）
 * @param LaidOutput `CodeLayout` が返した変更不能なコードの断片を改行で繋いだ基準のテキスト
 * @param Excluded 基準のテキストの中で文法がコメントと読む区切記法と逐語領域に残したコメントの範囲（位置順）
 * @param Errors 基準のテキストの保護されたエラー本体の範囲（位置順，重ならない）
 * @param OutputStarts 最終テキストのコードの論理行の始まりの位置の格納先（通常コメントへ変換したドキュメントの移動先の照合と共有する）
 * @param LaidOutputStarts 基準のテキストのコードの論理行の始まりの位置の格納先
 * @return 空行の形と正常行のテキストの一致
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
SyntaxValidator::LineComparison SyntaxValidator::CompareLayout(
	const ParsedTree &Checked,
	const std::string_view Text,
	const ByteRanges &Comments,
	const std::string_view LaidOutput,
	const std::span<const std::pair<uint32_t, uint32_t>> Excluded,
	const ByteRanges &Errors,
	std::vector<size_t> &OutputStarts,
	std::vector<size_t> &LaidOutputStarts
) {
	// 構文木の無いテキストは照合の基準のコメントの範囲を持たない為の返戻
	if(!Checked.IsParsed()) return {};
	const size_t CodeEnd = Checked.OffsetInInput(Checked.GetCodeText().size()), DataSize = Text.size() - CodeEnd;
	// 逐語データが基準のテキストより長い最終テキストの返戻
	if(DataSize > LaidOutput.size()) return { false, false };
	std::string ActualCode, ExpectedCode;
	const std::vector<SkeletonLine> Actual = LineSkeleton(Text.substr(0, CodeEnd), Comments, ActualCode, &OutputStarts),
	Expected = LineSkeleton(LaidOutput.substr(0, LaidOutput.size() - DataSize), Excluded, ExpectedCode, &LaidOutputStarts);
	// 論理行の数が変わった候補の返戻
	if(Actual.size() != Expected.size()) return { false, false };
	LineComparison Result;
	size_t Row = 0, Error = 0;
	for(size_t Idx = 0; Idx < Expected.size(); ++Idx) {
		const bool IsSame =
		Actual[Idx].IsBlank == Expected[Idx].IsBlank && std::string_view(ActualCode).substr(Actual[Idx].Start, Actual[Idx].Length) ==
		std::string_view(ExpectedCode).substr(Expected[Idx].Start, Expected[Idx].Length);
		Result.HasSameBlanks &= Actual[Idx].IsBlank == Expected[Idx].IsBlank;
		if(Expected[Idx].IsBlank) continue;
		const size_t Start = LaidOutputStarts[Row++];
		const size_t End = Row < LaidOutputStarts.size() ? LaidOutputStarts[Row] : LaidOutput.size() - DataSize;
		while(Error < Errors.size() && Errors[Error].second <= Start) ++Error;
		if((Error == Errors.size() || End <= Errors[Error].first) && !IsSame) Result.HasSameNormalCode = false;
	}
	// 空行の形と行の対応の独立した結果の返戻
	return Result;
}

/**
 * 通常コメントへ変換したドキュメントの取得関数（整形仕様 7.6, 8.1）
 * 期待コメントのドキュメントと認識した独立行の記録の内，確定した配置で説明対象（前置の属性を含む）が行の途中から始まる物を，配置の段が同じ位置で並べる記録の元の順に並べ，置く論理行と，ブロックの後の最初のコード行として認める断片を添える
 * 認める断片は，説明対象の行，其れを含み行の最初の内容から始まる最も近い構文の行，対象が閉括弧で始まる時の次のコード行及び其の括弧で終わる構文の行とする
 * 置けない説明対象は前置属性と同じ畳んだ行を用い，後続コードが無い場合は最大値を用いる
 * 計算量：ノード数 V，記録の数 C，本文の長さの和 N，対象の深さの和 H に対し O(V + C log C + N + H)
 * @param Code `CodeLayout` が確定した変更不能なコードの出力
 * @param Outputs 期待コメントの記録毎の出力記法の記録
 * @param Recognized 記録毎のドキュメントの説明対象 (`DocCommentReader::DocTargets`)
 * @param Placed 記録毎のドキュメントとして置く説明対象 (`DocCommentReader::PlacedTargets`)
 * @return 原文順の変換したドキュメント
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::vector<SyntaxValidator::ConvertedDoc> SyntaxValidator::ConvertedDocs(
	const LaidOutCode &Code,
	const std::span<const CommentRecord> Outputs,
	const std::span<const Core::NodeId> Recognized,
	const std::span<const Core::NodeId> Placed
) {
	// 配置データと変換先の準備
	const AnchorPositionMap &Anchors = Code.GetAnchors();
	const Core::SyntaxModel &Syntax = *Anchors.Syntax;
	const std::span<const CodeFragment> Fragments = Code.GetFragments();
	std::vector<ConvertedDoc> Converted;
	// 変換したドキュメント毎の記録の元の順と，閉括弧で始まる説明対象の行の其の括弧の終わりの位置毎の変換したドキュメント
	std::vector<size_t> Originals;
	std::unordered_map<uint32_t, std::vector<size_t>> Closers;
	// ドキュメントとして置けない記録の移動先の収集
	for(size_t Idx = 0; Idx < Outputs.size(); ++Idx) {
		if(Recognized[Idx] == Core::NoNode || Placed[Idx] != Core::NoNode) continue;
		const Core::NodeId Target = Recognized[Idx];
		const uint32_t Line = Anchors.Nodes[Target].StartFragment;
		ConvertedDoc Doc { PlacedLines(Outputs[Idx].Text), { Line } };
		for(Core::NodeId Up = Syntax.GetParentOf(Target); Up != Core::NoNode; Up = Syntax.GetParentOf(Up)) {
			if(!Anchors.Nodes[Up].IsPlaced || !Core::DocCommentReader::IsAtLineStart(Code, Up)) continue;
			Doc.Destinations.push_back(Anchors.Nodes[Up].StartFragment);
			break;
		}
		// 説明対象の行が閉括弧で始まるか（配置の段と同じトークン片の形）
		const uint32_t Content = Fragments[Line].Start + Anchors.FragmentTabs[Line];
		if(const CodePiece *const Piece = Code.GetPieceAt(Content); Piece && Piece->Shapes & CodePiece::StartsClosingBracket) {
			const uint32_t Next = Code.GetNextCodeFragment(Line);
			Doc.Destinations.push_back(Next < Fragments.size() ? Next : std::numeric_limits<uint32_t>::max());
			Closers[Content + 1].push_back(Converted.size());
		}
		Converted.push_back(std::move(Doc));
		Originals.push_back(Outputs[Idx].OriginalOrder);
	}
	// 閉括弧で終わる構文の最も前の始まりの断片（其の括弧を開いた構文の行）
	if(!Closers.empty()) {
		std::unordered_map<uint32_t, uint32_t> Openings;
		for(const NodeSpan &Span : Anchors.Nodes) {
			if(!Span.IsPlaced || !Closers.contains(Span.End)) continue;
			const auto [Found, IsNew] = Openings.try_emplace(Span.End, Span.StartFragment);
			if(!IsNew) Found->second = std::min(Found->second, Span.StartFragment);
		}
		for(const auto &[End, Indices] : Closers) {
			if(const std::unordered_map<uint32_t, uint32_t>::const_iterator Found = Openings.find(End); Found != Openings.end()) {
				for(const size_t Idx : Indices) Converted[Idx].Destinations.push_back(Found->second);
			}
		}
	}
	// 配置の段が同じ位置で並べる原文の順（記録の元の順）
	std::vector<size_t> Order(Converted.size());
	std::iota(Order.begin(), Order.end(), size_t{ 0 });
	std::stable_sort(
		Order.begin(),
		Order.end(),
		[&Originals](const size_t Lhs, const size_t Rhs) -> bool {
			// 元の順の比較の返戻
			return Originals[Lhs] < Originals[Rhs];
		}
	);
	std::vector<ConvertedDoc> Sorted;
	Sorted.reserve(Converted.size());
	for(const size_t Idx : Order) Sorted.push_back(std::move(Converted[Idx]));
	// 原文順の変換したドキュメントの返戻
	return Sorted;
}

/**
 * 独立行の通常コメントの続いた記録毎の本文の論理行の取得関数（整形仕様 8.1）
 * ドキュメントと認識しない独立行の説明のコメントの置く論理行を，同じグループで続く記録毎に繋ぐ（変換したドキュメントと同じ本文を複製と数えない為に使う）
 * 計算量：本文の長さの和 N に対し O(N)
 * @param Outputs 期待コメントの記録毎の出力記法の記録
 * @param Recognized 記録毎のドキュメントの説明対象
 * @return 続いた記録毎の本文の論理行
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::vector<std::vector<std::string>> SyntaxValidator::OrdinaryBodies(
	const std::span<const CommentRecord> Outputs,
	const std::span<const Core::NodeId> Recognized
) {
	// 通常コメント本文の比較データの収集準備
	std::vector<std::vector<std::string>> Bodies;
	// 独立行の説明の通常コメントか
	const auto IsOrdinary = [&Outputs, &Recognized](const size_t Idx) -> bool {
		// ドキュメントと認識しない独立行の説明のコメントかの返戻
		return Recognized[Idx] == Core::NoNode && Outputs[Idx].Kind == CommentKind::OwnLine &&
		Outputs[Idx].Role == CommentRole::Presentation;
	};
	for(size_t Idx = 0; Idx < Outputs.size(); ++Idx) {
		if(!IsOrdinary(Idx)) continue;
		std::vector<std::string> Lines = PlacedLines(Outputs[Idx].Text);
		// 同じグループで直前の記録に続く物は其の本文へ連結
		if(
			Idx && IsOrdinary(Idx - 1) && Outputs[Idx].Format.Group != Core::NoCommentGroup &&
			Outputs[Idx].Format.Group == Outputs[Idx - 1].Format.Group
		) Bodies.back().insert(Bodies.back().end(), std::make_move_iterator(Lines.begin()), std::make_move_iterator(Lines.end()));
		else Bodies.push_back(std::move(Lines));
	}
	// 続いた記録毎の本文の論理行の返戻
	return Bodies;
}

/**
 * 前処理の見出のコメントの本文・種別・順序・出現数の取得関数
 * 条件分岐の本体と見出を区別し，コードに残したコメントも独立に数える
 * 種別毎の論理行を比べ，記法に依る記録の分割と，独立コメントを見出の前へ移す配置を許す
 * 計算量：ノード数 N，記録数 C，本文長 L，未共有の本文構造の判定費用 Q に対し O(N + C + L + Q)
 * @param Syntax 期待又は完成候補の構文
 * @param Comments 期待の正規化済の記録，又は完成候補から独立に読んだ記録
 * @param Kept 同じ側の逐語トークンに残したコメントの範囲
 * @return 前処理の見出のコメントの保持事実
 * @throws std::bad_alloc 保持事実又は所属表のメモリを確保出来ない場合
 * @throws std::length_error 作業列又は本文が上限を超える場合
 * @throws std::out_of_range アンカー又は逐語コメントの範囲が不正の場合
 */
SyntaxValidator::DirectiveCommentFacts SyntaxValidator::GetDirectiveCommentFacts(
	const Core::SyntaxModel &Syntax,
	const Core::CommentRepository &Comments,
	const std::span<const ValidationContext::KeptComment> Kept
) {
	// 前処理の見出への所属と，逐語コメントを持つノードの表
	std::vector<uint8_t> IsHeader(Syntax.GetNodeCount(), 0), HasKept(Syntax.GetNodeCount(), 0);
	std::vector<Core::NodeId> Pending { Syntax.GetRoot() };
	while(!Pending.empty()) {
		const Core::NodeId Id = Pending.back();
		Pending.pop_back();
		const bool IsBranch = Syntax.Is(Id, Core::Role::PreprocBranch);
		// 条件分岐の本体を除き，見出のフィールドと開始・終端の語を照合する
		IsHeader[Id] =
		!IsBranch && (IsHeader[Id] || Syntax.Is(Id, Core::Role::PreprocNode) || Syntax.Is(Id, Core::Role::PreprocDirectiveClose));
		for(const Core::NodeId Child : Syntax.GetNode(Id).Children) {
			IsHeader[Child] = IsBranch ?
			Syntax.HasField(Child, Core::ChildRole::Cond) || Syntax.HasField(Child, Core::ChildRole::Name) || !Syntax.GetNode(Child).Idx :
			IsHeader[Id];
			Pending.push_back(Child);
		}
	}
	DirectiveCommentFacts Facts;
	Facts.Lines.resize(static_cast<size_t>(CommentKind::Trailing) + 1);
	for(const ValidationContext::KeptComment &Comment : Kept) {
		HasKept[Comment.Node] = 1;
		if(IsHeader[Comment.Node]) {
			Facts.Kept.emplace_back(Syntax.GetLexemeOf(Comment.Node).substr(Comment.Start, Comment.End - Comment.Start));
		}
	}
	const std::span<const CommentRecord> Records = Comments.GetRecords();
	const std::span<const Core::NodeId> Docs = Comments.GetDocTargets();
	std::optional<CommentStructure> Inspected;
	const CommentStructure *Structure = Comments.GetStructure();
	for(size_t Idx = 0; Idx < Records.size(); ++Idx) {
		const CommentRecord &Record = Records[Idx];
		// ドキュメント・ディレクティブコメントは各専用の保持照合で扱い，逐語コメントの診断用の記録は重ねて数えない
		if(
			Record.Anchor == Core::NoNode || !IsHeader[Record.Anchor] || HasKept[Record.Anchor] ||
			Record.Role != CommentRole::Presentation || Docs[Idx] != Core::NoNode
		) continue;
		// 配置済の本文構造は共有し，候補は必要な場合だけ独立に１度判定する
		if(!Structure) {
			Inspected.emplace(CommentNormalizer::Inspect(ReadOnlyCommentView(Records)));
			Structure = &*Inspected;
		}
		std::string Marker;
		if(Record.Kind == CommentKind::Trailing) {
			Marker = CommentNormalizer::GetStackMarkerOf(Core::CommentRepository::GetOutputNotationOf(Record));
		}
		std::vector<std::string> Lines = PlacedLines(Record.Text, Marker, Structure->GetCodeMask(Idx));
		std::vector<std::string> &KindLines = Facts.Lines[static_cast<size_t>(Record.Kind)];
		KindLines.insert(KindLines.end(), std::make_move_iterator(Lines.begin()), std::make_move_iterator(Lines.end()));
	}
	// １度の走査で収集した保持事実の返戻
	return Facts;
}

/**
 * コメントの中の波括弧の数の保持の判定関数
 * tree-sitter-scss は宣言の値の中の `//` をコメントと読まず，続く行のセレクタを同じノードへ取り込む事が有る
 * 其の儘整形するとセレクタと其の `{` がコメントの本文へ吸われ，様式が壊れる（処理系が `unmatched "}"` で拒む）
 * 計算量：２つのテキストの総長 B と記法数 F に対し O(B F)
 * @param Rules 文法の記法の定義（使用可能な記法）
 * @param Src 整形前の入力
 * @param Output 整形結果
 * @param UrlHead 引用符の無い引数を取る値の関数の開始記法
 * @return コメントの中の波括弧が増えて居なければ true
 */
bool SyntaxValidator::HasNoExtraCommentBraces(
	const Core::NotationRules &Rules,
	const std::string_view Src,
	const std::string_view Output,
	const std::string_view UrlHead
) {
	// コメントの中の波括弧が増えて居ない事の返戻（増えたのはコードがコメントへ吸われた跡）
	return CountBracesInComments(Rules, Output, UrlHead) <= CountBracesInComments(Rules, Src, UrlHead);
}

/**
 * 検証の診断に使うノード名の取得関数
 * 計算量：元のノード名長 L 又は固定の役割数 R に対し期待値 O(L + R)
 * @param Syntax 対象の構文
 * @param Node ノード
 * @param Symbols 投影元の記号の並び
 * @return 元のノード名（生成ノードは共通の役割の集合）
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
std::string SyntaxValidator::KindCmp::GetName(
	const Core::SyntaxModel &Syntax,
	const Core::NodeId Node,
	const std::span<const TSSymbol> Symbols
) const {
	// 対象のノードの種別定義の取得
	const Core::KindDef *const Def = Syntax.GetValidationNode(Node).Kind.Def;
	if(
		Node < Symbols.size() && Symbols[Node] != std::numeric_limits<TSSymbol>::max() &&
		&Profile.GetKindOf(Symbols[Node]) == Profile.GetBaseKindOf(Def)
	// 元のノード名の返戻
	) return std::string(Profile.GetSymbolName(Symbols[Node]));
	// 共通の役割の集合の返戻
	return Def ? Def->Roles.to_string() : std::string();
}

/**
 * 共通の種別と入力側の記号対応の照合関数
 * 計算量：ノードの役割数 R と照合するノード名長 L に対し期待値 O(R + L)
 * @param Expected 期待構文
 * @param Actual 完成候補の構文
 * @param Before 期待するノード
 * @param After 完成候補のノード
 * @return 共通種別と，書換を受けて居ないノードの元のノード名が等しければ true
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
bool SyntaxValidator::KindCmp::IsSame(
	const Core::SyntaxModel &Expected,
	const Core::SyntaxModel &Actual,
	const Core::NodeId Before,
	const Core::NodeId After
) const {
	// 比較する原文の種別定義の取得
	const Core::KindDef *const Left = Expected.GetValidationNode(Before).Kind.Def;
	const Core::KindDef *const Right = Actual.GetNode(After).Kind.Def;
	// 共通の種別が違う場合の拒否の返戻
	if(Left != Right) return false;
	// 原文の記号を持たない生成ノードを共通種別で照合した結果の返戻
	if(Before >= ExpectedSymbols.size() || After >= ActualSymbols.size()) return true;
	const TSSymbol Original = ExpectedSymbols[Before], Output = ActualSymbols[After];
	// 同じ記号は固定定義を重ねて照会せず，生成又は種別を書き換えたノードも共通種別で照合する
	if(
		Original == Output || Original == std::numeric_limits<TSSymbol>::max() || Output == std::numeric_limits<TSSymbol>::max() ||
		&Profile.GetKindOf(Original) != Profile.GetBaseKindOf(Left) || &Profile.GetKindOf(Output) != Profile.GetBaseKindOf(Right)
	) return true;
	// 記号を共有した種別の内部でも元のノード名の違いを見逃さない照合の返戻
	return Profile.IsSameName(Original, Output);
}

/**
 * 構造照合が読む境界かの判定関数
 * @param Boundary 子の前の境界
 * @return 保持する間隙又は表示上の分離なら true
 */
bool SyntaxValidator::HasSignificantBoundary(const Core::SemanticBoundary &Boundary) {
	// 構造照合が読む境界かの返戻
	return Boundary.Kind == Core::BoundaryKind::Preserved || Boundary.Kind == Core::BoundaryKind::Separated ||
	Boundary.Kind == Core::BoundaryKind::TokenGlued || Boundary.Kind == Core::BoundaryKind::TokenSpaced;
}

/**
 * 対応するノードの子の境界を比較する関数
 * 計算量：前後の間隙のテキスト長の和 L，境界の取得・診断生成の費用 Q に対し O(L + Q)
 * @param Expected 書換後の期待構文
 * @param Actual 完成候補を再投影した構文
 * @param Kinds 診断用の記号対応
 * @param Before 期待する親
 * @param BeforeIdx 期待する子の前の位置
 * @param After 完成候補の親
 * @param AfterIdx 完成候補の子の前の位置
 * @param Original 期待する位置の境界（無ければ nullptr）
 * @param Output 完成候補の位置の境界（無ければ nullptr）
 * @param ShouldDiagnose 不一致を診断するか
 * @return 保持境界と区切記法が一致すれば true
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
bool SyntaxValidator::HasSamePreservedBoundary(
	const Core::SyntaxModel &Expected,
	const Core::SyntaxModel &Actual,
	const KindCmp &Kinds,
	const Core::NodeId Before,
	const size_t BeforeIdx,
	const Core::NodeId After,
	const size_t AfterIdx,
	const Core::SemanticBoundary *const Original,
	const Core::SemanticBoundary *const Output,
	const bool ShouldDiagnose
) {
	// 期待と完成候補の境界の分類
	const bool HasOriginalPreservation = Original && Original->Kind == Core::BoundaryKind::Preserved;
	const bool IsPreserved = Output && Output->Kind == Core::BoundaryKind::Preserved;
	// 通常の空白量は整形で変わるが，値の持つ文字列の間隙は完全に保持
	const auto IsSameSignificant = [](const std::string_view Left, const std::string_view Right) -> bool {
		// 原文と候補の保存境界の突合の準備
		size_t LeftIdx = 0, RightIdx = 0;
		while(true) {
			while(LeftIdx < Left.size() && std::isspace(static_cast<unsigned char>(Left[LeftIdx]))) ++LeftIdx;
			while(RightIdx < Right.size() && std::isspace(static_cast<unsigned char>(Right[RightIdx]))) ++RightIdx;
			// 双方のテキストが尽きた時だけ一致する事の返戻
			if(LeftIdx == Left.size() || RightIdx == Right.size()) return LeftIdx == Left.size() && RightIdx == Right.size();
			// 空白以外の字が異なる場合の返戻
			if(Left[LeftIdx++] != Right[RightIdx++]) return false;
		}
	};
	// 保持する間隙のテキストと境界種別の照合
	const bool IsExact = HasOriginalPreservation && (
		Original->Src == Core::PreservedSrc::MacroTokens || Original->Src == Core::PreservedSrc::LiteralContent ||
		Original->Src == Core::PreservedSrc::RenderedLineEdges
	);
	const bool IsSamePreservation = !HasOriginalPreservation || IsPreserved && Original->Src == Output->Src &&
	(IsExact ? Original->Gap == Output->Gap : IsSameSignificant(Original->Gap, Output->Gap));
	const auto TokenBoundary = [](const Core::SemanticBoundary *const Boundary) -> Core::BoundaryKind {
		// 意味を持つトークン境界だけの返戻
		return Boundary && (Boundary->Kind == Core::BoundaryKind::TokenGlued || Boundary->Kind == Core::BoundaryKind::TokenSpaced) ?
		Boundary->Kind :
		Core::BoundaryKind::Spaced;
	};
	const bool IsSame = TokenBoundary(Original) == TokenBoundary(Output) && HasOriginalPreservation == IsPreserved &&
	(Original && Original->Kind == Core::BoundaryKind::Separated) == (Output && Output->Kind == Core::BoundaryKind::Separated) &&
	IsSamePreservation;
	// 不一致時の境界の診断
	if(ShouldDiagnose && !IsSame) {
		std::fprintf(
			stderr,
			"STRUCT-BOUNDARY %s %zu preserved=%d source=%d bytes=%zu | %s %zu preserved=%d source=%d bytes=%zu\n",
			Kinds.GetName(Expected, Before, Kinds.ExpectedSymbols).c_str(),
			BeforeIdx,
			HasOriginalPreservation,
			HasOriginalPreservation ? static_cast<int>(Original->Src) : -1,
			HasOriginalPreservation ? Original->Gap.size() : 0,
			Kinds.GetName(Actual, After, Kinds.ActualSymbols).c_str(),
			AfterIdx,
			IsPreserved,
			IsPreserved ? static_cast<int>(Output->Src) : -1,
			IsPreserved ? Output->Gap.size() : 0
		);
	}
	// 保持する間隙の種類と意味を持つテキストの一致の返戻
	return IsSame;
}

/**
 * 文脈に依る型表現の対応を共通の照合へ登録する関数
 * 計算量：基底型の子数 C，省略可能な型部品の判定・照合結果の登録の費用 Q に対し O(C + Q)
 * @param Expected 正規化後の期待構文
 * @param Actual 完成候補を再投影した構文
 * @param Frame 対応する型と宣言の末尾カンマの照合状態
 * @param Matched 基底の照合の登録先（種別・場・トークンは共通の作業列で確認する）
 * @return 許可する型表現の対応を登録したなら true
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
bool SyntaxValidator::HasMatchingContextualTypeRepresentation(
	const Core::SyntaxModel &Expected,
	const Core::SyntaxModel &Actual,
	const NodeMatch &Frame,
	std::vector<NodeMatch> &Matched
) {
	// 原文と候補の文脈依存の型のノードの取得
	const Core::NodeId Before = Frame.Before, After = Frame.After;
	// 型注釈等の場を変えたノードを同値と扱わない事の返戻
	if(Frame.Fields.value_or(Expected.GetValidationNode(Before).Fields) != Actual.GetNode(After).Fields) return false;
	// 宣言の部品の対応の登録
	const auto Register = [&Frame, &Matched](const Core::NodeId Left, const Core::NodeId Right) -> void {
		// 比較対象を移す照合枠の複写
		NodeMatch Base = Frame;
		Base.Before = Left;
		Base.After = Right;
		Base.Fields.reset();
		Base.HasSelectedForm = false;
		Matched.push_back(std::move(Base));
		// 終了
		return;
	};
	if(Expected.Is(Before, Core::Role::OptionalType) && Actual.Is(After, Core::Role::OptionalType)) {
		Core::NodeId Original = Core::NoNode, Result = Core::NoNode;
		// 基底を読めない，又は `?` の数が異なる型の拒否の返戻
		if(
			size_t OriginalMarks = 0, ResultMarks = 0;
			!HasOptionalTypeParts(Expected, Before, Original, OriginalMarks) || !HasOptionalTypeParts(Actual, After, Result, ResultMarks) ||
			OriginalMarks != ResultMarks
		) return false;
		Register(Original, Result);
		// 省略可能性記号が同じ基底を登録した事の返戻
		return true;
	}
	// メタ型と限定型以外の異種ノードの拒否の返戻
	if(!Expected.Is(Before, Core::Role::MetatypeNode) || !Actual.Is(After, Core::Role::UserType)) return false;
	const std::span<const Core::NodeId> Outer = Expected.GetChildrenOf(Before), Qualified = Actual.GetChildrenOf(After);
	// 異なる後置型を混同しない拒否の返戻
	if(
		const bool IsSameSuffix =
		Outer.size() == 3 && Expected.Is(Outer[0], Core::Role::UserType) && Expected.Is(Outer[1], Core::Role::DotToken) &&
		Expected.IsWord(Outer[2], Core::Word::MetatypeSuffix) && Qualified.size() == Expected.GetChildCount(Outer[0]) + 2 &&
		Actual.Is(Qualified[Qualified.size() - 2], Core::Role::DotToken) && Actual.Is(Qualified.back(), Core::Role::TypeName) &&
		Actual.IsWord(Qualified.back(), Core::Word::MetatypeSuffix);
		!IsSameSuffix
	) return false;
	const std::span<const Core::NodeId> Base = Expected.GetChildrenOf(Outer[0]);
	for(size_t Idx = 0; Idx < Base.size(); ++Idx) Register(Base[Idx], Qualified[Idx]);
	// 型の基底と `.Type` の後置記号を対応付けた事の返戻
	return true;
}

/**
 * 選択した宣言区分と完成候補の宣言子の対応の判定関数
 * 区分の被覆と共有指定を確認し，全ての部品を通常の子と同じ作業列へ登録する
 * 計算量：宣言の部品数 P，共有部品数 S，宣言名の数 I，種類の照合・共有領域の登録の費用 Q に対し O(P + S + I + Q)
 * @param Expected 正規化後の期待構文
 * @param Actual 完成候補を再投影した構文
 * @param Kinds 共通種別と投影元の記号の対応
 * @param Node 統合宣言
 * @param Rows 当該宣言から出力した宣言の並び
 * @param Partition 行分割が選んだ区分
 * @param RowCommas 出力宣言毎に共有部分へ選択した末尾カンマ
 * @param IsInMacro 保存するトークン列の内側か
 * @param Matched 共有指定と宣言子の部品の照合の登録先
 * @return 区分の被覆と宣言子の対応を登録出来れば true
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
bool SyntaxValidator::HasMatchingDeclPartition(
	const Core::SyntaxModel &Expected,
	const Core::SyntaxModel &Actual,
	const KindCmp &Kinds,
	const Core::NodeId Node,
	const std::span<const Core::NodeId> Rows,
	const Core::DeclPartition &Partition,
	const std::vector<std::unordered_set<Core::NodeId>> &RowCommas,
	const bool IsInMacro,
	std::vector<NodeMatch> &Matched
) {
	// 分割元の宣言子と共有指定の取得
	const std::optional<Core::CommaDecl> Src = Expected.GetCommaDeclOf(Node);
	// 宣言子の無い区分の拒否の返戻
	if(!Src || Src->Lists.empty() || Partition.Rows.size() != Rows.size() || RowCommas.size() != Rows.size()) return false;
	const size_t Count = Src->Lists.front().Items.size();
	std::vector<Core::NodeId> SharedBefore, SharedAfter;
	for(const Core::DeclPiece &Piece : Src->Pieces) if(Piece.List < 0) SharedBefore.push_back(Piece.Node);
	// 出力行毎の共有指定と宣言子の照合
	size_t End = 0;
	for(size_t Idx = 0; Idx < Rows.size(); ++Idx) {
		const std::shared_ptr<std::unordered_set<Core::NodeId>> UsedCommas = std::make_shared<std::unordered_set<Core::NodeId>>();
		const Core::DeclaratorRange Range = Partition.Rows[Idx];
		// 宣言子を飛ばす又は重ねる区分の拒否の返戻
		if(Range.Begin != End || Range.Begin >= Range.End || Range.End > Count) return false;
		const Core::NodeId Row = Rows[Idx];
		// 出力宣言の種別又は親の中の場が変わった区分の拒否の返戻
		if(
			const bool IsSameRole =
			Kinds.IsSame(Expected, Actual, Node, Row) && Expected.GetValidationNode(Node).Fields == Actual.GetNode(Row).Fields;
			!IsSameRole
		) return false;
		const std::optional<Core::CommaDecl> Output = Actual.GetCommaDeclOf(Row);
		// 共有指定と並びの数が変わった区分の拒否の返戻
		if(!Output || Src->Lists.size() != Output->Lists.size()) return false;
		SharedAfter.clear();
		for(const Core::DeclPiece &Piece : Output->Pieces) if(Piece.List < 0) SharedAfter.push_back(Piece.Node);
		// 共有する型・指定・終端の構造が変わった区分の拒否の返戻
		if(SharedBefore.size() != SharedAfter.size()) return false;
		for(size_t Shared = 0; Shared < SharedBefore.size(); ++Shared) {
			Matched.push_back({ SharedBefore[Shared], SharedAfter[Shared], IsInMacro, &RowCommas[Idx], UsedCommas });
		}
		// 子孫の照合を終えた後で，此の行に選択した末尾カンマの全て確認
		Matched.push_back({ Node, Row, IsInMacro, &RowCommas[Idx], UsedCommas, true });
		for(size_t List = 0; List < Src->Lists.size(); ++List) {
			const std::vector<std::vector<Core::NodeId>> &Before = Src->Lists[List].Items, &After = Output->Lists[List].Items;
			// 宣言子の数が選択結果と異なる区分の拒否の返戻
			if(Before.size() != Count || After.size() != Range.End - Range.Begin) return false;
			for(size_t Item = 0; Item < After.size(); ++Item) {
				const std::vector<Core::NodeId> &Origin = Before[Range.Begin + Item], &Result = After[Item];
				// 宣言子の構成が変わった区分の拒否の返戻
				if(Origin.size() != Result.size()) return false;
				for(size_t Piece = 0; Piece < Result.size(); ++Piece) Matched.push_back({ Origin[Piece], Result[Piece], IsInMacro });
			}
		}
		End = Range.End;
	}
	// 全宣言子を１度ずつ出力したかの返戻
	return End == Count;
}

/**
 * 期待構文と完成候補を再投影した構文の一致の判定関数
 * 計算量：構文木のノード数 N，生成形式と宣言分割の部品数 F，各ノードの対応判定・意味構造の比較・診断生成の費用 Q に対し O(N + F + Q)
 * @param Actual 完成候補を入力と同じ修復の後に投影した構文
 * @param Expectation 正規化後の期待構文と選択結果
 * @param Kinds 共通種別と投影元の記号の対応
 * @param ShouldDiagnose 差の位置を診断へ出すか
 * @param ExpectedErrors 入力の領域の本体と祖先の分類
 * @param ActualErrors 完成候補の対応する範囲の本体と祖先の分類
 * @param TypeMatch 文脈に依る型表現の照合関数
 * @param PartitionMatch 宣言区分の照合関数
 * @param Work 対応数と作業領域の増加の観測先（無ければ観測しない）
 * @return 正常な部分木が対応表に従って一致するなら true
 * @throws FormatLimitExceeded 選択した生成形式が期待構文に無い場合又は作業予算を超えた場合
 * @throws std::bad_alloc 選択部品・照合の作業列又は診断のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
template<class Match, class Cmp, class TypeFunction, class PartitionFunction> bool SyntaxValidator::HasMatchingStructure(
	const Core::SyntaxModel &Actual,
	const Core::OutputExpectation &Expectation,
	const Cmp &Kinds,
	const bool ShouldDiagnose,
	const std::span<const ErrorZonePart> ExpectedErrors,
	const std::span<const ErrorZonePart> ActualErrors,
	const TypeFunction TypeMatch,
	const PartitionFunction PartitionMatch,
	MatchWork *const Work
) {
	// 共有する期待構文と，選択した生成部品だけの対応表
	const Core::SyntaxModel &Expected = *Expectation.NormalizedSyntax;
	std::unordered_map<Core::NodeId, const Core::GeneratedNode *> SelectedNodes;
	std::deque<Core::GeneratedNode> GeneratedNodes;
	// 宣言区分と採用した句読点の対応準備
	std::unordered_set<Core::NodeId> CommaOwners, MemberSeparators;
	bool HasMemberSeparator = false;
	using CommaOwnersByRow = std::vector<std::unordered_set<Core::NodeId>>;
	struct DeclMatch {
		const Core::DeclPartition *Partition; // 選択した宣言区分
		CommaOwnersByRow RowCommas; // 此の区分の出力行毎に選択した末尾カンマ
		CommaOwnersByRow RowSeparators; // 此の区分の出力行毎に選択したメンバ区切の直後の子
	};
	using DeclMatches = std::unordered_map<Core::NodeId, DeclMatch>;
	DeclMatches Partitions;
	if(!Expectation.DeclarationPartitions.empty()) Partitions.reserve(Expectation.DeclarationPartitions.size());
	for(const Core::DeclPartition &Partition : Expectation.DeclarationPartitions) {
		if(
			!Partitions.emplace(
				Partition.Node,
				DeclMatch{ &Partition, CommaOwnersByRow(Partition.Rows.size()), CommaOwnersByRow(Partition.Rows.size()) }
			).second
		) throw FormatLimitExceeded("a declaration has multiple partitions", FailureReason::FormatterDefect);
	}
	// 選択した生成形式の部品と挿入位置の対応
	for(const Core::SelectedForm &Selected : Expectation.SelectedForms) {
		for(const Core::SelectedFormEffect &Effect : Selected.Effects) {
			if(!Selected.IsBroken && Effect.Operation != Core::FormOperation::MemberSeparator) continue;
			Core::RewriteStep Step;
			switch(Effect.Operation) {
			case Core::FormOperation::TrailingComma:
				{
					const Core::NodeId Owner = Effect.Node;
					const std::span<const Core::NodeId> Children = Expected.GetChildrenOf(Owner);
					const Core::CanonicalForm *const Form = Expected.GetFormOf(Owner, Core::FormOperation::TrailingComma);
					if(
						const bool IsValid = Form && Children.size() > 1 && !Expected.IsNamed(Children.back()) && Form->CanAddTrailingComma(
							Selected.IsBroken,
							Expected.IsNamed(Children[Children.size() - 2]),
							Expected.GetFirstChar(Children.back())
						);
						!IsValid
					) throw FormatLimitExceeded("a selected trailing comma has no syntax position", FailureReason::FormatterDefect);
					if(Effect.DeclOwner != Core::NoNode) {
						if(
							const typename DeclMatches::iterator Found = Partitions.find(Effect.DeclOwner);
							Found == Partitions.end() || Effect.DeclRow >= Found->second.RowCommas.size() ||
							!Expected.HasNodeWithin(Effect.DeclOwner, Owner) || !Found->second.RowCommas[Effect.DeclRow].insert(Owner).second
						) throw FormatLimitExceeded("a selected declaration comma has no output row", FailureReason::FormatterDefect);
						continue;
					}
					if(!CommaOwners.insert(Owner).second) {
						throw FormatLimitExceeded("a selected trailing comma has no syntax position", FailureReason::FormatterDefect);
					}
					continue;
				}
			case Core::FormOperation::MemberSeparator:
				{
					const Core::NodeId Child = Effect.Node;
					if(Selected.IsBroken || Child >= Expected.GetNodeCount()) {
						throw FormatLimitExceeded("a selected member separator has no syntax position", FailureReason::FormatterDefect);
					}
					const Core::NodeId Owner = Expected.GetParentOf(Child);
					const size_t Idx = Expected.GetValidationSiblingIndex(Child);
					if(
						Owner == Core::NoNode || !(Expected.Is(Owner, Core::Role::TypeMemberBody) || Expected.Is(Owner, Core::Role::ImportItems)) ||
						!Expected.Is(Owner, Core::Role::DelimitedMemberSeq) || !Expected.Is(Owner, Core::Role::OptionalSemicolonHost) || !Idx
					) throw FormatLimitExceeded("a selected member separator has no syntax position", FailureReason::FormatterDefect);
					const std::span<const Core::NodeId> Children = Expected.GetChildrenOf(Owner);
					if(
						Idx >= Children.size() || Children[Idx] != Child || !Expected.IsNamed(Children[Idx - 1]) || !(
							Expected.IsNamed(Child) ||
							Expected.Is(Children[Idx - 1], Core::Role::ClassMember) && Expected.IsTokenRole(Child, Core::Role::ClosingBraceToken)
						)
					) throw FormatLimitExceeded("a selected member separator has no syntax position", FailureReason::FormatterDefect);
					HasMemberSeparator = true;
					if(Effect.DeclOwner != Core::NoNode) {
						const typename DeclMatches::iterator Found = Partitions.find(Effect.DeclOwner);
						if(
							Found == Partitions.end() || Effect.DeclRow >= Found->second.RowSeparators.size() ||
							!Expected.HasNodeWithin(Effect.DeclOwner, Child) || !Found->second.RowSeparators[Effect.DeclRow].insert(Child).second
						) throw FormatLimitExceeded("a selected declaration separator has no output row", FailureReason::FormatterDefect);
					} else if(!MemberSeparators.insert(Child).second) {
						throw FormatLimitExceeded("a selected member separator has multiple selections", FailureReason::FormatterDefect);
					}
					continue;
				}
			case Core::FormOperation::Continuation:
			case Core::FormOperation::BraceBody:
				if(const Core::GeneratedNode *const Prepared = Expected.GetPreparedValidationGeneration(Effect.Node, Effect.Operation)) {
					SelectedNodes.emplace(Effect.Node, Prepared);
					continue;
				}
				Step.Inserted = Expected.GetValidationGeneration(Effect.Node, Effect.Operation);
				break;
			default:
				continue;
			}
			// ラッパーも組直も，外側の場を保ち既存の子孫を参照する
			Step.Inserted.Fields = Expected.GetValidationNode(Effect.Node).Fields;
			if(!SelectedNodes.contains(Effect.Node)) {
				GeneratedNodes.push_back(std::move(Step.Inserted));
				SelectedNodes.emplace(Effect.Node, &GeneratedNodes.back());
			}
		}
	}
	// 選択の位置と重複を検査した後，通常の一対一照合で足りる区分を除く
	std::erase_if(
		Partitions,
		[](const typename DeclMatches::value_type &Entry) -> bool {
			// 宣言分割も選択した末尾カンマも無い区分かの返戻
			return Entry.second.Partition->Rows.size() < 2 && std::ranges::all_of(
				Entry.second.RowCommas,
				[](const std::unordered_set<Core::NodeId> &Owners) -> bool {
					// 此の出力行に選択した末尾カンマが無いかの返戻
					return Owners.empty();
				}
			) && std::ranges::all_of(
				Entry.second.RowSeparators,
				[](const std::unordered_set<Core::NodeId> &Children) -> bool {
					// 此の出力行に選択したメンバ区切が無いかの返戻
					return Children.empty();
				}
			);
		}
	);
	const std::string MemberSeparator = HasMemberSeparator ?
	Core::SyntaxGeneration::GetGeneratedFixed(Expected, Core::FormOperation::GeneratedLexemes, Core::FormPartRole::Terminator) :
	std::string();
	const auto MatchPartition = [&](
		const DeclMatch &Selection,
		const Core::NodeId Node,
		const std::span<const Core::NodeId> Rows,
		const bool IsInMacro,
		std::vector<Match> &Matches
	) -> bool {
		// 共有指定の対応に，採用した出力行のメンバ区切を直接参照で渡す
		const size_t Start = Matches.size();
		if(!PartitionMatch(Expected, Actual, Kinds, Node, Rows, *Selection.Partition, Selection.RowCommas, IsInMacro, Matches)) {
			// 共有指定又は宣言子の対応が無い事の返戻
			return false;
		}
		for(size_t Idx = Start; Idx < Matches.size(); ++Idx) if(Matches[Idx].RowCommas) {
			const size_t Row = static_cast<size_t>(Matches[Idx].RowCommas - Selection.RowCommas.data());
			Matches[Idx].RowSeparators = &Selection.RowSeparators[Row];
		}
		// 共有指定と宣言子の対応を登録した事の返戻
		return true;
	};
	// 比較対象の診断テキストの構築
	const auto Describe =
	[&Kinds](const Core::SyntaxModel &Syntax, const Core::NodeId Node, const std::span<const TSSymbol> Symbols) -> std::string {
		// 比較対象の種別の診断テキストの構築
		std::string Description = Kinds.GetName(Syntax, Node, Symbols);
		Description += "(";
		const std::span<const Core::NodeId> Children = Syntax.GetChildrenOf(Node);
		const size_t Shown = std::min(Children.size(), size_t{ 8 });
		for(size_t Idx = 0; Idx < Shown; ++Idx) {
			const Core::NodeId Child = Children[Idx];
			Description +=
			Syntax.IsNamed(Child) ? Kinds.GetName(Syntax, Child, Symbols) : "'" + Kinds.GetName(Syntax, Child, Symbols) + "'";
			Description += " ";
		}
		if(Shown < Children.size()) Description += "... +" + std::to_string(Children.size() - Shown);
		Description += ")";
		// 比較用のノードの記述の返戻
		return Description;
	};
	// 期待構文と完成候補のノード毎の照合
	struct ScratchObserver {
		const size_t Capacity; // 此の対応を作る前の容量
		const std::vector<Match> &Matched; // 対応を作る間だけ借りる共通の作業領域
		MatchWork *Work; // 検査だけが渡す観測先

		/**
		 * 対応の完了又は枝の終了時の容量の観測関数
		 * 計算量：O(1)
		 */
		~ScratchObserver() {
			// 観測先を持たない通常の照合の終了
			if(!Work) return;
			if(Matched.capacity() > Capacity) ++Work->Growth;
			Work->Peak = std::max(Work->Peak, Matched.capacity());
			// 終了
			return;
		}
	};
	std::vector<Match> Stack, Matched;
	if(ExpectedErrors.empty()) Stack.push_back({ Expected.GetRoot(), Actual.GetRoot(), false });
	else {
		// 喪失した領域の祖先だけの展開（正常な兄弟の部分木を省略しない）
		std::vector<Match> Pending { { Expected.GetRoot(), Core::NoNode, false } }, Frontier;
		while(!Pending.empty()) {
			Match Frame = std::move(Pending.back());
			Pending.pop_back();
			if(!Frame.Generated && !Frame.HasSelectedForm) {
				if(
					const std::unordered_map<Core::NodeId, const Core::GeneratedNode *>::const_iterator Selected = SelectedNodes.find(Frame.Before);
					Selected != SelectedNodes.end()
				) Frame.Generated = Selected->second;
			}
			if(Frame.Generated) {
				// 選択したラッパーの正常な部品と，生成したトークンの記録
				if(ExpectedErrors[Frame.Before] == ErrorZonePart::Normal || Frame.Generated->Children.empty()) {
					Frontier.push_back(std::move(Frame));
					continue;
				}
				for(size_t Idx = Frame.Generated->Children.size(); Idx--;) {
					const Core::GeneratedNode &Child = Frame.Generated->Children[Idx];
					Match Next { Child.Existing == Core::NoNode ? Frame.Before : Child.Existing, Core::NoNode, Frame.IsInMacro };
					Next.Generated = Child.Existing == Core::NoNode ? &Child : nullptr;
					Next.Fields = Child.Fields;
					Next.HasSelectedForm = Child.Existing == Frame.Before;
					Pending.push_back(std::move(Next));
				}
				continue;
			}
			switch(ExpectedErrors[Frame.Before]) {
			case ErrorZonePart::Interior:
				continue;
			case ErrorZonePart::Normal:
				Frontier.push_back(std::move(Frame));
				continue;
			case ErrorZonePart::Ancestor:
				break;
			}
			Frame.IsInMacro = Frame.IsInMacro || Expected.Is(Frame.Before, Core::Role::PreservedTokenSeq);
			const std::span<const Core::NodeId> Children = Expected.GetChildrenOf(Frame.Before);
			for(size_t Idx = Children.size(); Idx--;) Pending.push_back({ Children[Idx], Core::NoNode, Frame.IsInMacro });
		}
		// 完成候補では出力範囲から分類した祖先だけを展開
		std::vector<Core::NodeId> ActualPending { Actual.GetRoot() }, ActualFrontier;
		while(!ActualPending.empty()) {
			const Core::NodeId Node = ActualPending.back();
			ActualPending.pop_back();
			switch(ActualErrors[Node]) {
			case ErrorZonePart::Interior:
				continue;
			case ErrorZonePart::Normal:
				ActualFrontier.push_back(Node);
				continue;
			case ErrorZonePart::Ancestor:
				break;
			}
			const std::span<const Core::NodeId> Children = Actual.GetChildrenOf(Node);
			for(size_t Idx = Children.size(); Idx--;) ActualPending.push_back(Children[Idx]);
		}
		// 正常な宣言区分も通常の照合と同じ対応へ渡す
		size_t Cursor = 0;
		for(Match &Frame : Frontier) {
			const typename DeclMatches::const_iterator Partition = Partitions.find(Frame.Before);
			if(!Frame.Generated && Partition != Partitions.end()) {
				const size_t Count = Partition->second.Partition->Rows.size();
				// 対応しない正常な宣言区分が残る事の返戻
				if(
					Cursor + Count > ActualFrontier.size() || !MatchPartition(
						Partition->second,
						Frame.Before,
						std::span<const Core::NodeId>(ActualFrontier.data() + Cursor, Count),
						Frame.IsInMacro,
						Matched
					)
				) return false;
				Cursor += Count;
				continue;
			}
			// 対応しない正常な部分木が残る事の返戻
			if(Cursor == ActualFrontier.size()) return false;
			Frame.After = ActualFrontier[Cursor++];
			// 展開した祖先の子の場は祖先の差であり，正常な部分木の内部の場は従来通り比較
			Frame.Fields = Actual.GetNode(Frame.After).Fields;
			Matched.push_back(std::move(Frame));
		}
		// 期待に無い正常な部分木が残る事の返戻
		if(Cursor != ActualFrontier.size()) return false;
		for(size_t Idx = Matched.size(); Idx--;) Stack.push_back(std::move(Matched[Idx]));
	}
	while(!Stack.empty()) {
		if(Work) ++Work->Pairs;
		Match Frame = std::move(Stack.back());
		Stack.pop_back();
		if(Frame.IsRowEnd) {
			// 共有指定の子孫で確認出来なかった選択の拒否の返戻
			if(Frame.UsedCommas->size() != Frame.RowCommas->size() + (Frame.RowSeparators ? Frame.RowSeparators->size() : 0)) return false;
			continue;
		}
		// 選択した部品の根へ入り，既存の部分木へ戻る時も同じ照合の作業列を使う
		if(!Frame.Generated && !Frame.HasSelectedForm) {
			if(
				const std::unordered_map<Core::NodeId, const Core::GeneratedNode *>::const_iterator Selected = SelectedNodes.find(Frame.Before);
				Selected != SelectedNodes.end()
			) Frame.Generated = Selected->second;
		}
		if(Frame.Generated) {
			const Core::GeneratedNode &Generated = *Frame.Generated;
			const Core::SyntaxNode &Output = Actual.GetNode(Frame.After);
			if(
				const std::string_view Lexeme = !Generated.Lexeme.empty() ? std::string_view(Generated.Lexeme) : Generated.Src != Core::NoNode ?
				std::string_view(Expected.GetValidationNode(Generated.Src).Lexeme) :
				std::string_view(Generated.Kind);
				Expected.GetDefFor(Generated).Def != Output.Kind.Def || Frame.Fields.value_or(Generated.Fields) != Output.Fields ||
				Generated.Children.size() != Output.Children.size() || Generated.Children.empty() && Lexeme != Output.Lexeme ||
				std::ranges::any_of(Output.Facts.Boundaries.GetView(), HasSignificantBoundary)
			// 生成した種類・テキスト・境界又は子の構成が一致しない事の返戻
			) return false;
			for(size_t Idx = Generated.Children.size(); Idx--;) {
				const Core::GeneratedNode &Child = Generated.Children[Idx];
				Match Next {
					Child.Existing == Core::NoNode ? Frame.Before : Child.Existing,
					Output.Children[Idx],
					Frame.IsInMacro,
					Frame.RowCommas,
					Frame.UsedCommas
				};
				Next.Generated = Child.Existing == Core::NoNode ? &Child : nullptr;
				Next.Fields = Child.Fields;
				Next.HasSelectedForm = Child.Existing == Frame.Before;
				Next.RowSeparators = Frame.RowSeparators;
				Stack.push_back(std::move(Next));
			}
			continue;
		}
		const Core::NodeId Before = Frame.Before, After = Frame.After;
		const bool IsInMacro = Frame.IsInMacro || Expected.Is(Before, Core::Role::PreservedTokenSeq);
		const Core::ValidationNode Original = Expected.GetValidationNode(Before);
		const Core::SyntaxNode &Output = Actual.GetNode(After);
		const std::span<const Core::NodeId> ExpectedChildren = Original.Children, ActualChildren = Output.Children;
		const ScratchObserver Observation { Matched.capacity(), Matched, Work };
		Matched.clear();
		if(Kinds.Profile.GetReading().HasContextualTypeRepresentation && TypeMatch(Expected, Actual, Frame, Matched)) {
			for(size_t Idx = Matched.size(); Idx--;) Stack.push_back(std::move(Matched[Idx]));
			continue;
		}
		// 一対一の子は全境界の照合後に直接積み，中間の照合枠を作らない
		const bool HasDirectMatches = Partitions.empty() && ExpectedChildren.size() == ActualChildren.size();
		const bool HasSelectedComma = CommaOwners.contains(Before) || Frame.RowCommas && Frame.RowCommas->contains(Before);
		const bool HasInsertedComma =
		HasSelectedComma && ExpectedChildren.size() > 1 && ActualChildren.size() == ExpectedChildren.size() + 1 &&
		Actual.Is(ActualChildren[ActualChildren.size() - 2], Core::Role::CommaToken) &&
		Kinds.IsSame(Expected, Actual, ExpectedChildren.back(), ActualChildren.back());
		// 独立したコードとして整形した埋込の中身（期待構文は所有領域を置くノード，完成候補は中身のテキストの葉）
		const bool IsEmbeddedCode = ActualChildren.empty() && Expected.HasValidationRegion(Before);
		bool IsSame = (!HasSelectedComma || HasInsertedComma) && Kinds.IsSame(Expected, Actual, Before, After) &&
		Frame.Fields.value_or(Original.Fields) == Output.Fields && (
			IsEmbeddedCode ?
			Actual.Is(After, Core::Role::RawTextNode) :
			!ExpectedChildren.empty() || !ActualChildren.empty() || Original.Lexeme == Output.Lexeme
		);
		if(IsSame && HasInsertedComma && Frame.UsedCommas) Frame.UsedCommas->insert(Before);
		size_t Cursor = 0, BeforeBoundary = 0, AfterBoundary = 0;
		const std::span<const Core::SemanticBoundary> BeforeBoundaries = Expected.GetValidationBoundaries(Before),
		AfterBoundaries = Output.Facts.Boundaries.GetView();
		const bool HasBoundaries = !BeforeBoundaries.empty() || !AfterBoundaries.empty();
		// 子の対応は前進する為，境界も同じ順で１度だけ読む
		const auto ReadBoundary = [Work](const std::span<const Core::SemanticBoundary> Boundaries, const size_t Idx, size_t &Position)
		-> const Core::SemanticBoundary * {
			while(Position < Boundaries.size() && Boundaries[Position].Before < Idx) {
				if(Work) ++Work->BoundaryAdvances;
				++Position;
			}
			// 此の子の前に在る境界だけの返戻
			return Position < Boundaries.size() && Boundaries[Position].Before == Idx ? &Boundaries[Position] : nullptr;
		};
		// 子ノードと分割した宣言区分の対応照合
		for(size_t Idx = 0; Idx < ExpectedChildren.size() && !IsEmbeddedCode; ++Idx) {
			if(!IsSame) break;
			const Core::NodeId Child = ExpectedChildren[Idx];
			if(
				Expected.IsValidationRegionEmpty(Child) &&
				(Cursor == ActualChildren.size() || !Actual.Is(ActualChildren[Cursor], Core::Role::RawTextNode))
			) continue;
			// 採用したメンバ境界に在る生成記号だけを消費し，其の後の子と意味境界は通常通り照合する
			if(Frame.RowSeparators ? Frame.RowSeparators->contains(Child) : MemberSeparators.contains(Child)) {
				if(
					Cursor == ActualChildren.size() || Actual.IsNamed(ActualChildren[Cursor]) ||
					!Actual.IsTokenRole(ActualChildren[Cursor], Core::Role::StmtTerminatorToken) ||
					Actual.GetNode(ActualChildren[Cursor]).Lexeme != MemberSeparator
				) {
					IsSame = false;
					break;
				}
				++Cursor;
				if(Frame.RowSeparators && Frame.UsedCommas) Frame.UsedCommas->insert(Child);
			}
			// 両側に境界が無い場合は，無と無の同じ比較を子の位置毎に繰り返さない
			IsSame = !HasBoundaries || HasSamePreservedBoundary(
				Expected,
				Actual,
				Kinds,
				Before,
				Idx,
				After,
				Cursor,
				ReadBoundary(BeforeBoundaries, Idx, BeforeBoundary),
				ReadBoundary(AfterBoundaries, Cursor, AfterBoundary),
				ShouldDiagnose
			);
			if(!IsSame) break;
			if(HasInsertedComma && Idx + 1 == ExpectedChildren.size()) ++Cursor;
			const typename DeclMatches::const_iterator Found = HasDirectMatches ? Partitions.end() : Partitions.find(Child);
			if(Found != Partitions.end()) {
				const size_t Count = Found->second.Partition->Rows.size();
				IsSame = Cursor + Count <= ActualChildren.size() &&
				MatchPartition(Found->second, Child, std::span<const Core::NodeId>(ActualChildren.data() + Cursor, Count), IsInMacro, Matched);
				Cursor += Count;
				continue;
			}
			if(Cursor == ActualChildren.size()) {
				IsSame = false;
				break;
			}
			if(!HasDirectMatches) {
				Match Next { Child, ActualChildren[Cursor], IsInMacro, Frame.RowCommas, Frame.UsedCommas };
				Next.RowSeparators = Frame.RowSeparators;
				Matched.push_back(std::move(Next));
			}
			++Cursor;
		}
		IsSame = IsSame && (
			IsEmbeddedCode || Cursor == ActualChildren.size() && (
				!HasBoundaries || HasSamePreservedBoundary(
					Expected,
					Actual,
					Kinds,
					Before,
					ExpectedChildren.size(),
					After,
					Cursor,
					ReadBoundary(BeforeBoundaries, ExpectedChildren.size(), BeforeBoundary),
					ReadBoundary(AfterBoundaries, Cursor, AfterBoundary),
					ShouldDiagnose
				)
			)
		);
		if(!IsSame) {
			if(ShouldDiagnose) {
				std::fprintf(
					stderr,
					"STRUCT-DIFF %d\n  exp %s fields=%llu\n  act %s fields=%llu\n",
					static_cast<int>(Kinds.Profile.GetLang().Id),
					Describe(Expected, Before, Kinds.ExpectedSymbols).c_str(),
					static_cast<unsigned long long>(Expected.GetValidationNode(Before).Fields),
					Describe(Actual, After, Kinds.ActualSymbols).c_str(),
					static_cast<unsigned long long>(Actual.GetNode(After).Fields)
				);
				// 差の位置と重なる選択だけを出し，正当なラッパー・区分の候補を探せる様にする
				constexpr size_t MaxDetails = 8, MaxScannedGroups = 16;
				size_t ShownForms = 0, FormIdx = 0;
				for(; FormIdx < Expectation.SelectedForms.size() && FormIdx < MaxScannedGroups && ShownForms < MaxDetails; ++FormIdx) {
					const Core::SelectedForm &Form = Expectation.SelectedForms[FormIdx];
					const std::vector<Core::NodeId>::const_iterator Last = Form.Nodes.begin() + std::min(Form.Nodes.size(), MaxDetails);
					const std::vector<Core::NodeId>::const_iterator Found = std::find_if(
						Form.Nodes.begin(),
						Last,
						[&Expected, Before](const Core::NodeId Owner) -> bool {
							// 差の位置と重なるグループの対象かの返戻
							return Expected.HasNodeWithin(Owner, Before) || Expected.HasNodeWithin(Before, Owner);
						}
					);
					if(Found == Last) continue;
					std::fprintf(
						stderr,
						"  form %zu %s %s (owners=%zu)\n",
						FormIdx,
						Kinds.GetName(Expected, *Found, Kinds.ExpectedSymbols).c_str(),
						Form.IsBroken ? "broken" : "flat",
						Form.Nodes.size()
					);
					++ShownForms;
				}
				if(FormIdx < Expectation.SelectedForms.size()) {
					std::fprintf(stderr, "  unexamined form groups: %zu\n", Expectation.SelectedForms.size() - FormIdx);
				}
				// 差の位置と重なる宣言区分の診断
				size_t ShownPartitions = 0, PartitionIdx = 0;
				for(
					;
					PartitionIdx < Expectation.DeclarationPartitions.size() && PartitionIdx < MaxScannedGroups && ShownPartitions < MaxDetails;
					++PartitionIdx
				) {
					const Core::DeclPartition &Partition = Expectation.DeclarationPartitions[PartitionIdx];
					if(!Expected.HasNodeWithin(Partition.Node, Before) && !Expected.HasNodeWithin(Before, Partition.Node)) continue;
					std::fprintf(stderr, "  partition %s", Kinds.GetName(Expected, Partition.Node, Kinds.ExpectedSymbols).c_str());
					const size_t Shown = std::min(MaxDetails, Partition.Rows.size());
					for(size_t Row = 0; Row < Shown; ++Row) {
						const Core::DeclaratorRange Range = Partition.Rows[Row];
						std::fprintf(stderr, " [%zu,%zu)", Range.Begin, Range.End);
					}
					if(Shown < Partition.Rows.size()) std::fprintf(stderr, " ... +%zu rows", Partition.Rows.size() - Shown);
					std::fputc('\n', stderr);
					++ShownPartitions;
				}
				if(PartitionIdx < Expectation.DeclarationPartitions.size()) {
					std::fprintf(stderr, "  unexamined partitions: %zu\n", Expectation.DeclarationPartitions.size() - PartitionIdx);
				}
			}
			// 差が有る候補の返戻
			return false;
		}
		if(HasDirectMatches) for(size_t Idx = ExpectedChildren.size(); Idx--;) {
			Match Next { ExpectedChildren[Idx], ActualChildren[Idx], IsInMacro, Frame.RowCommas, Frame.UsedCommas };
			Next.RowSeparators = Frame.RowSeparators;
			Stack.push_back(std::move(Next));
		} else for(size_t Idx = Matched.size(); Idx--;) Stack.push_back(std::move(Matched[Idx]));
	}
	// 全ノードが一致した事の返戻
	return true;
}

/**
 * コード位置・テキスト・コメントのマーカーのインデックス化関数
 * 喪失の判定と正常領域の構造比較は，同じ空白・除外範囲の除去と位置の対応を使う
 * 計算量：テキストの長さ N と除外範囲の数 R に対し O(N + R)
 * @param Text テキスト
 * @param Excluded 位置順の重ならない除外範囲
 * @return 元の位置毎のコード位置，空白を除いたテキスト及び除外範囲のマーカー
 * @throws std::bad_alloc インデックスのメモリを確保出来ない場合
 * @throws std::length_error 表又はテキストの要素数上限を超える場合
 */
SyntaxValidator::CodeIndex SyntaxValidator::IndexCode(
	const std::string_view Text,
	const std::span<const std::pair<uint32_t, uint32_t>> Excluded
) {
	// 各位置迄のコードの字の計数と，コメントの位置の記録
	CodeIndex Index;
	Index.Positions.resize(Text.size() + 1, 0);
	Index.Text.reserve(Text.size());
	Index.Marks.push_back(0);
	size_t Range = 0;
	for(size_t Idx = 0; Idx < Text.size(); ++Idx) {
		while(Range < Excluded.size() && Excluded[Range].second <= Idx) ++Range;
		if(Range < Excluded.size() && Excluded[Range].first <= Idx) Index.Marks.back() = 1;
		else if(!std::isspace(static_cast<unsigned char>(Text[Idx]))) {
			Index.Text.push_back(Text[Idx]);
			Index.Marks.push_back(0);
		}
		Index.Positions[Idx + 1] = static_cast<uint32_t>(Index.Text.size());
	}
	// コードのインデックスの返戻
	return Index;
}

/**
 * 所有ノードの出力範囲に依るエラー領域の位置の対応付関数
 * 領域の識別と順序を保ち，同じテキストの別の出現へ対応をずらさない
 * 計算量：テキストの長さ N，除外範囲の数 R，所有ノードの数 E に対し O(N + R + E)
 * @param ExpectedOwners 配置が記録した保護領域毎の所有ノード
 * @param Zones 領域毎の所有ノードとエラー本文の範囲差
 * @param Spans 所有ノードのコードの出力範囲
 * @param LaidOutput コメントを置く前のコード
 * @param Excluded コードの出力の除外範囲
 * @param Candidate 完成候補の準備前のデコード済のテキスト
 * @param Comments 完成候補の準備前の除外範囲
 * @return 両側のコード位置と領域毎の保持境界
 * @throws std::bad_alloc 位置・範囲の表を確保出来ない場合
 * @throws std::length_error 表の要素数上限を超える場合
 * @throws std::out_of_range 所有ノードの出力範囲が表に無い場合
 */
SyntaxValidator::ErrorZoneLayout SyntaxValidator::MapErrorZones(
	const std::span<const std::vector<Core::NodeId>> ExpectedOwners,
	const std::span<const ValidationContext::ErrorZone> Zones,
	const std::span<const NodeSpan> Spans,
	const std::string_view LaidOutput,
	const ByteRanges &Excluded,
	const std::string_view Candidate,
	const ByteRanges &Comments
) {
	// 配置と候補の共通のコード位置のインデックス
	ErrorZoneLayout Layout { IndexCode(LaidOutput, Excluded), IndexCode(Candidate, Comments), {} };
	for(size_t Idx = 0; Idx < ExpectedOwners.size(); ++Idx) {
		const std::vector<Core::NodeId> &Owners = ExpectedOwners[Idx];
		uint32_t Start = UINT32_MAX, End = 0;
		for(const Core::NodeId Owner : Owners) if(Owner < Spans.size() && Spans[Owner].IsPlaced) {
			Start = std::min(Start, Layout.Before.Positions.at(Spans[Owner].Start));
			End = std::max(End, Layout.Before.Positions.at(Spans[Owner].End));
		}
		// 所有範囲の無い領域も識別を残し，喪失の判定へ渡す
		if(
			Start == UINT32_MAX || Idx >= Zones.size() || Zones[Idx].OwnerPrefix > End - Start ||
			Zones[Idx].OwnerSuffix > End - Start - Zones[Idx].OwnerPrefix
		) Layout.Ranges.emplace_back(UINT32_MAX, UINT32_MAX);
		else {
			// 逐語ノードの中のエラー本文へ絞ってから，保持する前後の境界を含める
			Start += Zones[Idx].OwnerPrefix;
			End -= Zones[Idx].OwnerSuffix;
			Layout.Ranges.emplace_back(Start ? Start - 1 : 0, End < Layout.Before.Text.size() ? End + 1 : End);
		}
	}
	// 喪失判定と遮蔽に共有する位置の返戻
	return Layout;
}

/**
 * 同じ出力位置に本文と前後の境界を保てないエラー領域の識別子の取得関数
 * コメントの追加は許し，入力から在るコメントのマーカーは同じコード位置に求める
 * 領域毎に定まった範囲だけを比べ，後の同じテキストを代わりに使わない
 * 計算量：領域数 Z と領域のテキストの長さの和 L に対し O(Z + L)（本文のインデックスは共有する）
 * @param Zones 入力の領域の識別順のテキスト
 * @param Layout 所有ノードから求めた同じ識別順の出力位置と候補のインデックス
 * @return 本文・順序・前後の境界を失った領域の入力の並びの位置
 * @throws FormatLimitExceeded 領域の識別と所有範囲の数が一致しない場合
 * @throws std::bad_alloc 喪失した領域の表を確保出来ない場合
 * @throws std::length_error 表の要素数上限を超える場合
 */
std::vector<size_t> SyntaxValidator::LostErrorZones(
	const std::vector<ValidationContext::ErrorZone> &Zones,
	const ErrorZoneLayout &Layout
) {
	if(Zones.size() != Layout.Ranges.size()) {
		throw FormatLimitExceeded("error-zone identities differ from their output ranges", FailureReason::FormatterDefect);
	}
	std::vector<size_t> Lost;
	uint32_t PreviousEnd = 0;
	for(size_t Idx = 0; Idx < Zones.size(); ++Idx) {
		const auto [Start, End] = Layout.Ranges[Idx];
		bool IsKept = Start != UINT32_MAX && Start >= PreviousEnd && Start <= End && End <= Layout.After.Text.size();
		if(IsKept) {
			size_t Pos = Start;
			for(const unsigned char Char : Zones[Idx].Text) {
				if(!Char) IsKept = Layout.After.Marks[Pos];
				else if(Pos < End && Char == static_cast<unsigned char>(Layout.After.Text[Pos])) ++Pos;
				else IsKept = false;
				if(!IsKept) break;
			}
			IsKept = IsKept && Pos == End;
		}
		if(!IsKept) Lost.push_back(Idx);
		// 内容の喪失でも出力位置は消費し，次の領域の識別を前へずらさない
		if(Start != UINT32_MAX) PreviousEnd = std::max(PreviousEnd, End);
	}
	// 失った領域の識別子の返戻
	return Lost;
}

/**
 * 期待構文と再投影した構文の一致の判定関数
 * 計算量：照合するノード数 V と選択部品数 F に対し O(V + F)
 * @param Actual 完成候補を入力と同じ修復の後に投影した構文
 * @param Expectation 正規化後の期待構文と選択結果
 * @param Kinds 共通種別と投影元の記号の対応
 * @param ShouldDiagnose 差の位置を診断へ出すか
 * @param Work 対応数と作業領域の増加の観測先（無ければ観測しない）
 * @return 対応表に従って構造が一致するなら true
 * @throws FormatLimitExceeded 選択した生成形式が期待構文に無い場合又は作業予算を超えた場合
 * @throws std::bad_alloc 選択部品・照合の作業列又は診断のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
bool SyntaxValidator::HasSameStructure(
	const Core::SyntaxModel &Actual,
	const Core::OutputExpectation &Expectation,
	const KindCmp &Kinds,
	const bool ShouldDiagnose,
	MatchWork *const Work
) {
	return HasMatchingStructure<NodeMatch>(
		Actual,
		Expectation,
		Kinds,
		ShouldDiagnose,
		{},
		{},
		HasMatchingContextualTypeRepresentation,
		HasMatchingDeclPartition,
		Work
	);
}

/**
 * ドキュメントの説明対象と本文の保持の判定関数（整形仕様 7.4, 8.1）
 * 期待コメントのドキュメントとして置く記録の説明対象が，完成候補でもドキュメントの説明対象と為り，其の本文の論理行を通常コメントとの記法の区別と共に欠落・改変・順序違反無く持つかを比べる
 * 完成候補のドキュメントに加わった論理行は，ドキュメントとして置かない期待コメント（同じ記法の言語でドキュメントに読まれる通常コメント）の論理行の出現を１度ずつ充てて説明出来る物だけを認め，ドキュメントの論理行の複製を拒む
 * 内容の無い論理行（埋込ドキュメントの開始・終端記号の行の残り等）は本文を持たない為，充てる対象としない
 * 期待に無い説明対象へ通常コメントと区別する記法のドキュメントが増えた完成候補も拒む
 * 説明対象の識別は構文上の位置で一意に定まり，出力の順は構造の照合が保つ為，同期が並べ替えた説明対象の順に依らず対象毎に比べる
 * 同じ対象のドキュメントの記録の分方（行形式のドキュメントの行毎の記録等）に依らない様に，対象毎に記録の順に論理行を繋いで比べる
 * 計算量：ドキュメントの数 D，記録の数 R と識別・本文の長さの和 L に対し O(D + R + L)
 * @param Expected 期待コメントのドキュメントの説明対象と本文
 * @param Actual 完成候補のドキュメントの説明対象と本文
 * @param Outputs 期待コメントの出力記法の記録
 * @param Placed 記録毎のドキュメントとして置く説明対象（置かない記録は無い）
 * @param Work 記録と完成候補の論理行の訪問数（不要なら nullptr）
 * @param TargetQueries 説明対象のハッシュ表の照会数（不要なら nullptr）
 * @return 期待する説明対象を全てドキュメントの説明対象として保ち，対象毎の期待の論理行を同じ記法の区別で原文の順に持ち，其の他の論理行を通常コメントの論理行で説明出来れば true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool SyntaxValidator::HasSameDocs(
	const std::span<const ValidationContext::DocFact> Expected,
	const std::span<const ValidationContext::DocFact> Actual,
	const std::span<const CommentRecord> Outputs,
	const std::span<const Core::NodeId> Placed,
	size_t *const Work,
	size_t *const TargetQueries
) {
	// 各側で独立に得た記録の参照を保持し，論理行は照合する時だけ読む
	struct TargetDoc {
		std::pmr::vector<const ValidationContext::DocFact *> Expected; // 期待の記録の参照（原文順）
		std::pmr::vector<const ValidationContext::DocFact *> Actual; // 完成候補の記録の参照（原文順）

		/**
		 * 照合の共有メモリを列へ渡す生成関数
		 * 計算量：O(1)
		 * @param Storage 照合の終わり迄有効な共有メモリ
		 */
		explicit TargetDoc(std::pmr::memory_resource *const Storage) : Expected(Storage), Actual(Storage) {}
	};
	std::pmr::monotonic_buffer_resource Storage;
	// 別々に投影した接頭部を，親の対応と此の宣言だけの表示で照合する
	struct TargetKey {
		size_t Scope; // 共有接頭部の番号
		std::string_view Target; // 対象の表示
		bool IsInner = false; // 囲む本体の説明か

		bool operator==(const TargetKey &Other) const = default; // 対象と説明する側の等価比較演算子
	};
	struct TargetHash {
		/**
		 * 接頭部と対象の表示のハッシュの取得関数
		 * 計算量：表示の長さ L に対し O(L)
		 * @param Key 接頭部と対象の表示
		 * @return 内容照合で衝突を区別するハッシュ
		 */
		size_t operator()(const TargetKey &Key) const noexcept {
			return std::hash<size_t>()(Key.Scope) ^ std::hash<std::string_view>()(Key.Target) ^ std::hash<bool>()(Key.IsInner);
		}
	};
	std::pmr::unordered_map<TargetKey, size_t, TargetHash> Scopes(&Storage);
	using ScopeMap = std::pmr::unordered_map<const ValidationContext::DocStorage *, std::pmr::vector<size_t>>;
	ScopeMap ScopeIds(&Storage);
	const auto ScopeOf = [&](const ValidationContext::DocFact &Fact) -> size_t {
		// 所有する宣言が無い対象の共通の根の返戻
		if(!Fact.Scope) return 0;
		// 接頭部の所有者を持たない不正な記録の返戻
		if(!Fact.Owner) return SIZE_MAX;
		const auto [Found, Added] = ScopeIds.try_emplace(Fact.Owner.get());
		std::pmr::vector<size_t> &Ids = Found->second;
		if(Added) {
			Ids.push_back(0);
			for(size_t Idx = 1; Idx < Fact.Owner->Scopes.size(); ++Idx) {
				const ValidationContext::DocScope &Scope = Fact.Owner->Scopes[Idx];
				// 共有接頭部は親から子へ記録した表だけを受理する
				if(Scope.Parent >= Idx) return SIZE_MAX;
				Ids.push_back(Scopes.try_emplace(TargetKey{ Ids[Scope.Parent], Scope.Label }, Scopes.size() + 1).first->second);
			}
		}
		// 此の照合の中だけで共有する接頭部の番号の返戻
		return Fact.Scope < Ids.size() ? Ids[Fact.Scope] : SIZE_MAX;
	};
	using DocIndex = std::pmr::unordered_map<TargetKey, TargetDoc, TargetHash>;
	DocIndex Required(&Storage);
	size_t PreviousScope = 0;
	bool IsPreviousInner = false;
	std::string_view PreviousTarget;
	TargetDoc *Previous = nullptr;
	for(const ValidationContext::DocFact &Fact : Expected) {
		if(Work) ++*Work;
		const size_t Scope = ScopeOf(Fact);
		// 不正な接頭部を持つ記録は照合に失敗する
		if(Scope == SIZE_MAX) return false;
		if(
			!Previous || PreviousScope != Scope || PreviousTarget.data() != Fact.Target.data() ||
			PreviousTarget.size() != Fact.Target.size() || IsPreviousInner != Fact.IsInner
		) {
			if(TargetQueries) ++*TargetQueries;
			Previous = &Required.try_emplace(TargetKey{ Scope, Fact.Target, Fact.IsInner }, &Storage).first->second;
			PreviousTarget = Fact.Target;
			PreviousScope = Scope;
			IsPreviousInner = Fact.IsInner;
		}
		Previous->Expected.push_back(&Fact);
	}
	// 完成候補の説明対象は，完成候補自身の表示の範囲の中だけで照会を共有する
	Previous = nullptr;
	for(const ValidationContext::DocFact &Fact : Actual) {
		if(Work) ++*Work;
		const size_t Scope = ScopeOf(Fact);
		// 不正な接頭部を持つ記録は照合に失敗する
		if(Scope == SIZE_MAX) return false;
		if(
			!Previous || PreviousScope != Scope || PreviousTarget.data() != Fact.Target.data() ||
			PreviousTarget.size() != Fact.Target.size() || IsPreviousInner != Fact.IsInner
		) {
			if(TargetQueries) ++*TargetQueries;
			const DocIndex::iterator Found = Required.find(TargetKey{ Scope, Fact.Target, Fact.IsInner });
			if(Found == Required.end()) {
				// 期待に無い対象へ専用の記法のドキュメントが増えた候補は認めない
				if(Fact.IsDedicated) return false;
				continue;
			}
			Previous = &Found->second;
			PreviousTarget = Fact.Target;
			PreviousScope = Scope;
			IsPreviousInner = Fact.IsInner;
		}
		Previous->Actual.push_back(&Fact);
	}
	// 通常コメントの論理行は，出力記録の本文内の範囲で保持する
	std::vector<std::pair<size_t, size_t>> LooseRanges;
	bool HasLooseLines = false;
	std::pmr::unordered_map<std::string_view, size_t> Unused(&Storage);
	const auto CanTakeLoose = [&](const std::string_view Line) -> bool {
		if(!HasLooseLines) {
			HasLooseLines = true;
			for(size_t Idx = 0; Idx < Outputs.size(); ++Idx) {
				if(Placed[Idx] != Core::NoNode) continue;
				LooseRanges.clear();
				Core::DocCommentReader::AppendLogicalLineRanges(Outputs[Idx], LooseRanges);
				const std::string_view Body = Core::CommentRepository::GetOutputTextOf(Outputs[Idx]);
				for(const std::pair<size_t, size_t> &Loose : LooseRanges) ++Unused[Body.substr(Loose.first, Loose.second)];
			}
		}
		const std::pmr::unordered_map<std::string_view, size_t>::iterator Spare = Unused.find(Line);
		// 充てる出現が無い事の返戻
		if(Spare == Unused.end() || !Spare->second) return false;
		--Spare->second;
		// 出現を充てた事の返戻
		return true;
	};
	for(const DocIndex::value_type &Target : Required) {
		const TargetDoc &Wanted = Target.second;
		// 論理行が空の記録も，説明対象の存在を保つ事を求める
		if(Wanted.Actual.empty()) return false;
		size_t NextDoc = 0, NextLine = 0;
		for(const ValidationContext::DocFact *const Fact : Wanted.Actual) for(const std::pair<size_t, size_t> &Line : Fact->Lines) {
			if(Work) ++*Work;
			while(NextDoc < Wanted.Expected.size() && NextLine == Wanted.Expected[NextDoc]->Lines.size()) {
				++NextDoc;
				NextLine = 0;
			}
			const std::string_view Text = Fact->Text.substr(Line.first, Line.second);
			if(NextDoc < Wanted.Expected.size()) {
				const ValidationContext::DocFact &ExpectedFact = *Wanted.Expected[NextDoc];
				const std::pair<size_t, size_t> &ExpectedLine = ExpectedFact.Lines[NextLine];
				if(
					Text == ExpectedFact.Text.substr(ExpectedLine.first, ExpectedLine.second) && Fact->IsDedicated == ExpectedFact.IsDedicated &&
					Fact->IsInner == ExpectedFact.IsInner
				) {
					++NextLine;
					continue;
				}
			}
			// 期待の次の論理行と違う行は，通常コメントの出現で説明出来る物だけを認める
			if(!Text.empty() && !CanTakeLoose(Text)) return false;
		}
		while(NextDoc < Wanted.Expected.size() && NextLine == Wanted.Expected[NextDoc]->Lines.size()) {
			++NextDoc;
			NextLine = 0;
		}
		// 期待の論理行が残った事の返戻
		if(NextDoc != Wanted.Expected.size()) return false;
	}
	// 説明対象と本文を保った事の返戻
	return true;
}

/**
 * 通常コメントへ変換したドキュメントの本文と移動先の保持の判定関数（整形仕様 7.6, 8.1）
 * 変換したドキュメント毎に，期待コメントの置く論理行が，認める断片の前の完成候補のブロックの中にドキュメントの記法でない記録として連続して現れる事を確かめる
 * 同じコード行の前（空行で分けたブロックを含む）に置くドキュメントは原文の順に現れる事を求め，ブロックの後の最初のコード行毎の照合済の位置から先だけを探す
 * 完成候補の論理行は正規化し直さずに比べ，正規化で消える違反を見逃さない
 * 同じ本文の完成候補の全てのブロックでの出現数が，変換したドキュメントと期待コメントの通常コメント（続いた記録毎）の本文の中の出現数の和を越える物は複製とする
 * 配置は最初の論理行と移動先毎の位置のインデックスを共有し，出現数は論理行の識別子と本文の接頭辞・接尾辞を共有して数える
 * 出現数は接尾辞の集計で重なる一致も数え，配置候補の照合は移動先毎の原文順を保つ
 * 計算量：論理行数 L，期待行数 R，移動先数 A，配置候補訪問数 M，最大本文行数 B に対し，テキストの照合費用を除き平均 O(L + R + A log(L + 1) + M B)
 * @param Converted 原文順の変換したドキュメント
 * @param Ordinary 期待コメントの独立行の通常コメントの論理行（続いた記録毎）
 * @param Runs 完成候補のコメントだけの行のブロック
 * @return 変換したドキュメントの本文を欠落・複製・順序違反・改変無く認める移動先に通常コメントとして保てば true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool SyntaxValidator::HasMatchingConvertedDocs(
	const std::span<const ConvertedDoc> Converted,
	const std::span<const std::vector<std::string>> Ordinary,
	const std::span<const CommentRun> Runs
) {
	// ブロックの論理行のテキストと移動先毎の位置（ブロックとブロックの中の論理行の順）
	using Pos = std::pair<size_t, size_t>;
	using DstIdx = std::unordered_map<uint32_t, std::vector<Pos>>;
	std::unordered_map<std::string_view, DstIdx> Positions;
	for(size_t Run = 0; Run < Runs.size(); ++Run) for(size_t Line = 0; Line < Runs[Run].Lines.size(); ++Line) {
		Positions[Runs[Run].Lines[Line]][Runs[Run].Next].emplace_back(Run, Line);
	}
	// ブロックの位置から続く分がドキュメントの記法でない論理行で，本文と一致するか
	const auto DoesMatchAt = [&Runs](const Pos At, const std::vector<std::string> &Body) -> bool {
		// 照合するコメント列と始まりの取得
		const auto [Run, Start] = At;
		const std::span<const std::string> Lines = Runs[Run].Lines;
		// ブロックを越える本文の返戻
		if(Start + Body.size() > Lines.size()) return false;
		// ドキュメントの記法の論理行を含む事の返戻
		for(size_t Line = Start; Line < Start + Body.size(); ++Line) if(Runs[Run].Docs[Line]) return false;
		// 本文と同じ論理行の並びかの返戻
		return std::equal(Body.begin(), Body.end(), Lines.begin() + static_cast<std::ptrdiff_t>(Start));
	};
	// ブロックの後の最初のコード行毎の照合済の位置の終わり（原文の順）
	std::unordered_map<uint32_t, Pos> Cursors;
	for(const ConvertedDoc &Doc : Converted) {
		// 内容の無いドキュメントは通常コメントとして置かない（整形仕様 7.7）
		if(Doc.Lines.empty()) continue;
		const std::unordered_map<std::string_view, DstIdx>::const_iterator BodyPositions = Positions.find(Doc.Lines.front());
		// 本文の最初の論理行が出力に無い場合の返戻
		if(BodyPositions == Positions.end()) return false;
		std::optional<Pos> FoundAt;
		uint32_t FoundNext = 0;
		for(const uint32_t Next : Doc.Destinations) {
			const DstIdx::const_iterator Dst = BodyPositions->second.find(Next);
			if(Dst == BodyPositions->second.end()) continue;
			const std::vector<Pos> &Starts = Dst->second;
			for(
				std::vector<Pos>::const_iterator At =
				std::lower_bound(Starts.begin(), Starts.end(), Cursors.try_emplace(Next, 0, 0).first->second);
				At != Starts.end();
				++At
			) {
				if(FoundAt && *FoundAt <= *At) break;
				if(!DoesMatchAt(*At, Doc.Lines)) continue;
				FoundAt = *At;
				FoundNext = Next;
				break;
			}
		}
		// 本文の欠落，認めない移動先，ドキュメントの記法の儘の配置，改変又は原文の順の違反の返戻
		if(!FoundAt) return false;
		Cursors.find(FoundNext)->second = { FoundAt->first, FoundAt->second + Doc.Lines.size() };
	}
	// 本文の共通接頭辞と接尾辞の対応（出現数を本文毎の位置の総当りで求めない）
	struct Prefix {
		std::unordered_map<size_t, size_t> Next; // 論理行の識別子から続く接頭辞
		size_t Fallback = 0; // 次に照合出来る最長の接尾辞
		size_t Actual = 0; // 完成候補の出現数
		size_t Expected = 0; // 期待本文の出現数
		bool IsBody = false; // 変換したドキュメントの本文全体か
	};
	std::unordered_map<std::string_view, size_t> Symbols;
	std::vector<Prefix> Prefixes(1);
	for(const ConvertedDoc &Doc : Converted) {
		size_t State = 0;
		for(const std::string &Line : Doc.Lines) {
			const size_t Symbol = Symbols.try_emplace(Line, Symbols.size()).first->second;
			const auto [Next, IsNew] = Prefixes[State].Next.try_emplace(Symbol, Prefixes.size());
			State = Next->second;
			if(IsNew) Prefixes.emplace_back();
		}
		if(State) Prefixes[State].IsBody = true;
	}
	// 接尾辞へ戻りながら次の論理行を読む（１つの本文の走査全体では進んだ長さ以上に戻らない）
	const auto Advance = [&Prefixes](size_t State, const size_t Symbol) -> size_t {
		while(true) {
			const std::unordered_map<size_t, size_t>::const_iterator Next = Prefixes[State].Next.find(Symbol);
			// 同じ論理行で続く接頭辞の返戻
			if(Next != Prefixes[State].Next.end()) return Next->second;
			// 一致する接頭辞が無い事の返戻
			if(!State) return 0;
			State = Prefixes[State].Fallback;
		}
	};
	// 幅優先で接尾辞の対応を確定する（同じ深さの順序は照合結果へ影響しない）
	std::vector<size_t> Order;
	Order.reserve(Prefixes.size());
	Order.push_back(0);
	for(size_t Idx = 0; Idx < Order.size(); ++Idx) {
		const size_t State = Order[Idx];
		for(const auto &[Symbol, Next] : Prefixes[State].Next) {
			Prefixes[Next].Fallback = State ? Advance(Prefixes[State].Fallback, Symbol) : 0;
			Order.push_back(Next);
		}
	}
	// ブロックの中の全ての本文を１度に数える（ドキュメント記法とブロックの切目を跨がない）
	const auto Count =
	[&](const std::span<const std::string> Lines, const std::span<const uint8_t> Docs, const bool IsActual) -> void {
		size_t State = 0;
		for(size_t Line = 0; Line < Lines.size(); ++Line) {
			if(!Docs.empty() && Docs[Line]) {
				State = 0;
				continue;
			}
			const std::unordered_map<std::string_view, size_t>::const_iterator Symbol = Symbols.find(Lines[Line]);
			State = Symbol == Symbols.end() ? 0 : Advance(State, Symbol->second);
			if(IsActual) ++Prefixes[State].Actual;
			else ++Prefixes[State].Expected;
		}
		// 終了
		return;
	};
	for(const CommentRun &Run : Runs) Count(Run.Lines, Run.Docs, true);
	for(const ConvertedDoc &Doc : Converted) Count(Doc.Lines, std::span<const uint8_t>(), false);
	for(const std::vector<std::string> &Lines : Ordinary) Count(Lines, std::span<const uint8_t>(), false);
	// 重なる部分列も接尾辞の出現へ１度ずつ加算する（全ての一致位置の列挙を行わない）
	for(size_t Idx = Order.size(); Idx > 1;) {
		const Prefix &Entry = Prefixes[Order[--Idx]];
		Prefix &Fallback = Prefixes[Entry.Fallback];
		Fallback.Actual += Entry.Actual;
		Fallback.Expected += Entry.Expected;
		// 期待の本文中の出現数を越える複製の返戻
		if(Entry.IsBody && Entry.Actual > Entry.Expected) return false;
	}
	// 変換したドキュメントの本文と移動先を保った事の返戻
	return true;
}

/**
 * 完成候補のコメントだけの行のブロックの取得関数（整形仕様 7.6, 8.1）
 * 行の最初から始まり行の終わりで終わるコメントが，空行を挟まずに続く並びをブロックとし，ブロックの論理行とブロックの後の最初のコード行の断片を返す
 * コードの論理行は空行の照合と同じ行の分類 (`LineSkeleton`) で数え，基準のテキストの同じ順のコードの論理行の断片へ対応させる
 * 計算量：テキストの長さ N とコメントの数 C に対し O(N + C log N)
 * @param Reading 文法の読取規則
 * @param Output 範囲と同じ順の最終テキストのコメントの記録
 * @param Text 修復前の最終テキストのコード（逐語データを除く）
 * @param Comments 最終テキストでコメントと読まれる範囲（位置順）
 * @param CodeStarts 最終テキストのコードの論理行の始まりの位置（空行の照合の骨格と共有，位置順）
 * @param CodeLines 基準のテキストのコードの論理行の始まりの断片の番号（位置順）
 * @return 位置順のブロック（範囲と記録が対応しなければ空）
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::vector<SyntaxValidator::CommentRun> SyntaxValidator::CommentRuns(
	const ReadingRules &Reading,
	const Core::CommentRepository &Output,
	const std::string_view Text,
	const ByteRanges &Comments,
	const std::span<const size_t> CodeStarts,
	const std::span<const uint32_t> CodeLines
) {
	// 比較するコメント記録の取得
	const std::span<const CommentRecord> Records = Output.GetRecords();
	// 範囲と記録が対応しないテキストの返戻
	if(Records.size() != Comments.size()) return {};
	// 行の中で位置の後が空白だけか（行末迄）
	const auto IsBlankAfter = [&Text, &Reading](const size_t Pos) -> bool {
		// 領域の後の空白を除いた位置の取得
		const uint32_t After = TextEdit::SkipSpaceRight(Text, Reading, static_cast<uint32_t>(Pos));
		// 行末かテキストの終わりの返戻
		return After >= Text.size() || Text[After] == '\n';
	};
	std::vector<CommentRun> Runs;
	for(size_t Idx = 0; Idx < Comments.size(); ++Idx) {
		const auto [Start, End] = Comments[Idx];
		if(TextEdit::IsAfterCode(Text, Reading, Start) || !IsBlankAfter(End)) continue;
		// 前のコメントと改行１つだけを挟んで続くか
		if(
			const bool ShouldJoin = !Runs.empty() && Idx && !TextEdit::IsAfterCode(Text, Reading, Comments[Idx - 1].first) &&
			IsBlankAfter(Comments[Idx - 1].second) &&
			std::ranges::count(Text.substr(Comments[Idx - 1].second, Start - Comments[Idx - 1].second), '\n') == 1;
			!ShouldJoin
		) Runs.push_back({ std::numeric_limits<uint32_t>::max(), {}, {} });
		const std::string_view Body = Records[Idx].Text;
		// 通常コメントと区別するドキュメントの記法の記録か（変換したドキュメントをドキュメントの儘置いた物を照合に使わない）
		const bool IsDoc = Core::DocCommentReader::IsDedicatedDoc(Core::CommentRepository::GetRulesOf(Records[Idx]), Records[Idx]);
		for(size_t Cursor = 0; Cursor <= Body.size();) {
			const size_t Break = std::min(Body.find('\n', Cursor), Body.size());
			Runs.back().Lines.emplace_back(Body.substr(Cursor, Break - Cursor));
			Runs.back().Docs.push_back(IsDoc);
			Cursor = Break + 1;
		}
		// ブロックの後の最初のコード行の断片
		const size_t Ordinal = static_cast<size_t>(std::ranges::lower_bound(CodeStarts, End) - CodeStarts.begin());
		Runs.back().Next = Ordinal < CodeLines.size() ? CodeLines[Ordinal] : std::numeric_limits<uint32_t>::max();
	}
	// 位置順のブロックの返戻
	return Runs;
}

/**
 * 同じ出力位置に残らないエラー領域の数の取得関数
 * 計算量：領域数 Z とテキストの長さの和 L に対し O(Z + L)
 * @param Zones 入力の領域の識別順のテキスト
 * @param Layout 領域毎の出力位置と候補のインデックス
 * @return 保持出来ない領域の数
 * @throws FormatLimitExceeded 領域の識別と所有範囲の数が一致しない場合
 * @throws std::bad_alloc 喪失した領域の表を確保出来ない場合
 * @throws std::length_error 表の要素数上限を超える場合
 */
size_t SyntaxValidator::CountLostErrorZones(
	const std::vector<ValidationContext::ErrorZone> &Zones,
	const ErrorZoneLayout &Layout
) {
	return LostErrorZones(Zones, Layout).size();
}

/**
 * 入力の領域に一致しない構文エラーの領域の数の取得関数
 * 失った領域の１つは整形後の領域の１つだけを説明し，其れを超える領域は正常領域に生じた構文エラーとする（整形仕様 8.3）
 * 計算量：整形後の長さ N とノードの数 V，テキストの長さの和 L に対し O(N log N + V + L)
 * @param OutputZones 完成候補のエラー領域の並び
 * @param Zones 入力から集めたテキストの並び (`SourceProjector::CollectErrorZones`)
 * @return 整形後の構文エラーの領域の内，入力の領域とテキストが一致しない物の数
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
size_t SyntaxValidator::CountUnmatchedErrorZones(
	const std::vector<ValidationContext::ErrorZone> &OutputZones,
	const std::vector<ValidationContext::ErrorZone> &Zones
) {
	// 入力のエラー領域の出現数の集計
	std::unordered_map<std::string_view, size_t> Remaining;
	for(const ValidationContext::ErrorZone &Zone : Zones) ++Remaining[Zone.Text];
	// 出力で一致しないエラー領域の計数
	size_t Unmatched = 0;
	for(const ValidationContext::ErrorZone &Output : OutputZones) {
		if(
			const std::unordered_map<std::string_view, size_t>::iterator Found = Remaining.find(Output.Text);
			Found != Remaining.end() && Found->second
		) --Found->second;
		else ++Unmatched;
	}
	// 一致しない領域の数の返戻
	return Unmatched;
}

/**
 * 喪失した保護領域の本体と祖先の分類関数
 * 入力の所有ノードと其の出力範囲だけを使い，言語・ノードの種別で保護違反を救済しない
 * 計算量：ノードの数 V，喪失した領域数 E に対し O(V log(E + 1) + E log(E + 1))
 * @param Expected 正規化後の期待構文
 * @param Actual 完成候補の再投影した構文
 * @param Lost 喪失した領域の入力の並びの位置
 * @param Spans 所有ノードのコードの出力範囲
 * @param Layout 喪失の判定と共有する領域毎の位置と両側のインデックス
 * @param Locations 完成候補のノードの準備・修復前の位置
 * @return 入力と完成候補の領域の本体・祖先の分類
 * @throws std::bad_alloc 位置・範囲又は分類の表を確保出来ない場合
 * @throws std::length_error 表の要素数上限を超える場合
 * @throws std::out_of_range 領域の所有又は位置の対応が表に無い場合
 */
SyntaxValidator::ErrorZoneMasks SyntaxValidator::ClassifyErrorZones(
	const Core::SyntaxModel &Expected,
	const Core::SyntaxModel &Actual,
	const std::span<const size_t> Lost,
	const std::span<const NodeSpan> Spans,
	const ErrorZoneLayout &Layout,
	const std::span<const Core::OutputRange> Locations
) {
	// 喪失した領域の所有ノードと出力範囲の対応
	ErrorZoneMasks Masks {
		std::vector<ErrorZonePart>(Expected.GetNodeCount(), ErrorZonePart::Normal),
		std::vector<ErrorZonePart>(Actual.GetNodeCount(), ErrorZonePart::Normal)
	};
	const std::vector<uint32_t> &Before = Layout.Before.Positions, &After = Layout.After.Positions;
	ByteRanges Ranges;
	for(const size_t Zone : Lost) {
		const auto [Start, End] = Layout.Ranges.at(Zone);
		if(Start != UINT32_MAX) Ranges.emplace_back(Start, End);
	}
	// エラー本体と隣接境界の，重ならないコードの出力範囲への統合
	std::sort(Ranges.begin(), Ranges.end());
	size_t Written = 0;
	for(const std::pair<uint32_t, uint32_t> &Range : Ranges) {
		if(Written && Range.first <= Ranges[Written - 1].second) {
			Ranges[Written - 1].second = std::max(Ranges[Written - 1].second, Range.second);
		} else Ranges[Written++] = Range;
	}
	Ranges.resize(Written);
	// 両側を同じ保持境界で分類し，外側の独立した正常部分木は全体の構造を比較する
	const auto Classify = [&Ranges](const uint32_t Start, const uint32_t End) -> ErrorZonePart {
		const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Next =
		std::upper_bound(Ranges.begin(), Ranges.end(), std::pair<uint32_t, uint32_t>(Start, UINT32_MAX));
		// 保持領域の中だけに在る事の返戻
		if(Next != Ranges.begin() && Start < End && End <= std::prev(Next)->second) return ErrorZonePart::Interior;
		if(Next != Ranges.begin() && Start < std::prev(Next)->second || Next != Ranges.end() && Next->first < End) {
			// 保持領域と外側の両方を含む事の返戻
			return ErrorZonePart::Ancestor;
		}
		// 保護領域から独立した範囲の返戻
		return ErrorZonePart::Normal;
	};
	std::vector<std::pair<uint32_t, uint32_t>> ExpectedRanges(Expected.GetNodeCount()), ActualRanges(Actual.GetNodeCount());
	for(Core::NodeId Node = 0; Node < Expected.GetNodeCount(); ++Node) if(Node < Spans.size() && Spans[Node].IsPlaced) {
		const uint32_t Start = Before.at(Spans[Node].Start), End = Before.at(Spans[Node].End);
		ExpectedRanges[Node] = { Start, End };
		Masks.Expected[Node] = Classify(Start, End);
	}
	for(Core::NodeId Node = 0; Node < Actual.GetNodeCount(); ++Node) {
		const uint32_t Start = After.at(Locations[Node].Start), End = After.at(Locations[Node].End);
		ActualRanges[Node] = { Start, End };
		Masks.Actual[Node] = Classify(Start, End);
	}
	// 再解析でエラーの祖先の結付が変わる場合も，正常な部分木の切目だけを共有して比較の粒度を揃える
	const auto CollectFrontier =
	[](const Core::SyntaxModel &Syntax, const std::span<const ErrorZonePart> Parts) -> std::vector<Core::NodeId> {
		std::vector<Core::NodeId> Pending { Syntax.GetRoot() }, Frontier;
		while(!Pending.empty()) {
			const Core::NodeId Node = Pending.back();
			Pending.pop_back();
			if(Parts[Node] == ErrorZonePart::Interior) continue;
			if(Parts[Node] == ErrorZonePart::Normal) {
				Frontier.push_back(Node);
				continue;
			}
			const std::span<const Core::NodeId> Children = Syntax.GetChildrenOf(Node);
			Pending.insert(Pending.end(), Children.begin(), Children.end());
		}
		// 先頭の部分木を末尾から取れる並びの返戻
		return Frontier;
	};
	std::vector<Core::NodeId> ExpectedFrontier = CollectFrontier(Expected, Masks.Expected),
	ActualFrontier = CollectFrontier(Actual, Masks.Actual);
	const auto HasExpanded =
	[](const Core::SyntaxModel &Syntax, const std::span<ErrorZonePart> Parts, std::vector<Core::NodeId> &Frontier) -> bool {
		const Core::NodeId Node = Frontier.back();
		const std::span<const Core::NodeId> Children = Syntax.GetChildrenOf(Node);
		// トークンの分割は出来ない事の返戻（後の構造比較で不一致を検出する）
		if(Children.empty()) return false;
		Parts[Node] = ErrorZonePart::Ancestor;
		Frontier.pop_back();
		for(size_t Idx = Children.size(); Idx--;) Frontier.push_back(Children[Idx]);
		// １段の展開を行った事の返戻
		return true;
	};
	while(!ExpectedFrontier.empty() && !ActualFrontier.empty()) {
		const auto [ExpectedStart, ExpectedEnd] = ExpectedRanges[ExpectedFrontier.back()];
		const auto [ActualStart, ActualEnd] = ActualRanges[ActualFrontier.back()];
		if(ExpectedStart == ActualStart && ExpectedEnd == ActualEnd) {
			ExpectedFrontier.pop_back();
			ActualFrontier.pop_back();
			continue;
		}
		// 重ならない正常領域又は位置の無いノードは，通常の構造比較へ其の儘渡す
		if(ExpectedEnd <= ActualStart || ActualEnd <= ExpectedStart) break;
		bool IsExpanded = false;
		if(ExpectedStart < ActualStart || ActualEnd < ExpectedEnd) IsExpanded = HasExpanded(Expected, Masks.Expected, ExpectedFrontier);
		if(ActualStart < ExpectedStart || ExpectedEnd < ActualEnd) {
			IsExpanded = HasExpanded(Actual, Masks.Actual, ActualFrontier) || IsExpanded;
		}
		if(!IsExpanded) break;
	}
	// 喪失した領域の本体と祖先の分類の返戻
	return Masks;
}

/**
 * 最終テキストの共通の検証関数
 * 出力を新しい入力として入力と同じテキストの段の適応の後に１度だけ解析・投影し，入力に無かった構文の破壊が無いかを確かめる（整形仕様 8.1）
 * 修復後の解析結果で保持条件を照合し，其の投影を構造の照合と静的解析で共有する（整形仕様 3.4, 8.4）
 * 見送と失敗を同時に検出した場合は失敗を優先する為，全ての検査の後に失敗に当たる物から返す（整形仕様 8.3）
 * 計算量：親子の解析・投影・保持照合の計算量の和（解析は各所有領域で１回，除外範囲数 R の統合は O(R log R)）
 * @param Profile 入力で確定した文法
 * @param Ctx 入力側の意味・保護領域と子の独立検証の所有経路
 * @param Code 空行の照合基準（必要な本文・位置を抽出した後に手放す）
 * @param Src 整形の済んだ最終テキスト
 * @param Comments 同期・本文正規化済の期待コメント
 * @param Expectation 中核が確定した期待構文と選択表現
 * @param ShouldDiagnose 検証結果を変えずに構造の差を診断する指定
 * @return 検証の結果
 * @throws FormatLimitExceeded 解析用のマーカーが最終テキストに残る場合と，構造の照合中に内部の不変条件の違反を検出した場合（フォーマッターの不具合）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
SyntaxValidator::ValidationResult SyntaxValidator::ValidationResult::ValidateImpl(
	const GrammarProfile &Profile,
	ValidationContext &Ctx,
	std::shared_ptr<const LaidOutCode> Code,
	const std::string &Src,
	const Core::CommentRepository &Comments,
	Core::OutputExpectation &Expectation,
	const bool ShouldDiagnose
) {
	// 最終テキストを新しい入力として入力と同じ正規化・適応・受理判定で１度だけ解析し，整形中の木を持ち込まない（文法は入力で確定した定義）
	// 完成候補の大きさはコメント合成で受理済（整形仕様 9.1）
	// 完成候補の見積作業域超過の失敗の返戻
	if(
		const size_t Estimate = EstimateWorkingBytes(Ctx.EntryProfile ? *Ctx.EntryProfile : Profile, Src);
		Estimate > MaxWorkingBytes
	) return { ValidationStatus::Failed, std::nullopt, FailureReason::WorkingMemoryExceeded };
	std::string LaidOutput;
	ByteRanges Excluded, ErrorRanges;
	std::vector<NodeSpan> Spans;
	std::vector<std::vector<Core::NodeId>> ErrorOwners;
	const std::span<const CommentRecord> Records = Comments.GetRecords(), Outputs = Records;
	const std::span<const Core::NodeId> Recognized = Comments.GetDocTargets(), Placed = Comments.GetPlacementTargets();
	DirectiveCommentFacts DirectiveComments;
	std::vector<ValidationContext::DocFact> Docs;
	std::vector<ConvertedDoc> Converted;
	std::vector<std::vector<std::string>> Ordinary;
	std::vector<uint32_t> CodeLines;
	const bool HasConvertedDocs = !std::ranges::equal(Recognized, Placed);
	const auto PrepareExpectation = [&]() -> void {
		// 空行と変換したドキュメントの照合の基準のテキスト（断片を改行で繋いだ物）と，其の中で文法がコメントと読む区切記法と逐語領域に残したコメントの範囲（位置順）
		const std::span<const CodeFragment> Fragments = Code->GetFragments();
		for(size_t Idx = 0; Idx < Fragments.size(); ++Idx) {
			if(Idx) LaidOutput += '\n';
			Fragments[Idx].AppendTo(0, Fragments[Idx].End - Fragments[Idx].Start, LaidOutput);
		}
		Excluded.assign(Code->GetSeparators().begin(), Code->GetSeparators().end());
		if(!Ctx.ErrorZones.empty()) {
			Spans = Code->GetAnchors().Nodes;
			ErrorOwners = Code->GetAnchors().Syntax->GetProtectedErrorOwners();
		}
		const std::vector<NodeSpan> &LayoutSpans = Code->GetAnchors().Nodes;
		for(const ValidationContext::KeptComment &Comment : Ctx.ProtectedRegions.KeptComments) {
			if(Comment.Node < LayoutSpans.size() && LayoutSpans[Comment.Node].IsPlaced) {
				Excluded.emplace_back(LayoutSpans[Comment.Node].Start + Comment.Start, LayoutSpans[Comment.Node].Start + Comment.End);
			}
		}
		// 親の空行照合は子が別に照合した領域だけを除く（子の断片の配置境界も含む）
		if(Profile.GetReading().HasEmbeddedScripts) for(const OwnedRegionSpan &Span : Code->GetAnchors().OwnedRegions) {
			if(Span.FirstFragment == Span.EndFragment) continue;
			const uint32_t Start = Fragments[Span.FirstFragment].Start;
			Excluded.emplace_back(
				Start ? Start - 1 : 0,
				Span.EndFragment < Fragments.size() ? Fragments[Span.EndFragment].Start : static_cast<uint32_t>(LaidOutput.size())
			);
		}
		// 保護されたエラー本体の出力範囲（テキストの差と空行の差を混同しない為の行の分類）
		for(const std::vector<Core::NodeId> &Owners : Code->GetAnchors().Syntax->GetProtectedErrorOwners()) {
			for(const Core::NodeId Owner : Owners) {
				if(LayoutSpans[Owner].IsPlaced) ErrorRanges.emplace_back(LayoutSpans[Owner].Start, LayoutSpans[Owner].End);
			}
		}
		// 前処理の見出の通常コメントと逐語トークン内のコメントの期待（完成候補の記録を持ち込まない）
		DirectiveComments = Profile.GetReading().HasPreprocMacros ?
		GetDirectiveCommentFacts(*Code->GetAnchors().Syntax, Comments, Ctx.ProtectedRegions.KeptComments) :
		DirectiveCommentFacts{};
		// 期待コメントの記録と確定した配置に対する，出力記法のドキュメントの説明対象と本文（整形仕様 7.4, 7.6, 8.1）
		Docs = Core::SourceProjector::DescribeDocs(*Code->GetAnchors().Syntax, Outputs, Placed);
		// 通常コメントへ変換したドキュメントと通常コメントの本文，完成候補のコード行へ順に対応する断片（空行の照合と同じ行の分類，整形仕様 7.6，8.1）
		if(HasConvertedDocs) {
			// 出力記法の記録の本文構造
			Converted = ConvertedDocs(*Code, Outputs, Recognized, Placed);
			Ordinary = OrdinaryBodies(Outputs, Recognized);
			// 通常記法の保持済ドキュメントも，完成候補の通常記法の本文の出現数に含まれる
			for(const ValidationContext::DocFact &Doc : Docs) if(!Doc.IsDedicated) {
				std::vector<std::string> Body;
				Body.reserve(Doc.Lines.size());
				for(const std::pair<size_t, size_t> &Line : Doc.Lines) Body.emplace_back(Doc.Text.substr(Line.first, Line.second));
				Ordinary.push_back(std::move(Body));
			}
		}
		// 所有する期待を縮約し，選択した宣言と生成部品は元のモデルで確定する
		if(Expectation.NormalizedSyntax) {
			std::vector<Core::NodeId> Declarations;
			for(const Core::DeclPartition &Partition : Expectation.DeclarationPartitions) if(Partition.Rows.size() > 1) {
				Declarations.push_back(Partition.Node);
			}
			std::vector<std::pair<Core::NodeId, Core::FormOperation>> Generations;
			for(const Core::SelectedForm &Selected : Expectation.SelectedForms) {
				for(const Core::SelectedFormEffect &Effect : Selected.Effects) {
					if(
						Selected.IsBroken && (
							Effect.Operation == Core::FormOperation::BraceBody || Effect.Operation == Core::FormOperation::Continuation ||
							Effect.Operation == Core::FormOperation::TrailingComma
						) || !Selected.IsBroken && Effect.Operation == Core::FormOperation::MemberSeparator
					) Generations.push_back({ Effect.Node, Effect.Operation });
					if(
						Effect.DeclOwner != Core::NoNode && (
							Selected.IsBroken && Effect.Operation == Core::FormOperation::TrailingComma ||
							!Selected.IsBroken && Effect.Operation == Core::FormOperation::MemberSeparator
						)
					) Declarations.push_back(Effect.DeclOwner);
				}
			}
			Expectation.NormalizedSyntax = std::make_shared<const Core::SyntaxModel>(
				Core::SyntaxModel::CompactForValidation(*Expectation.NormalizedSyntax, Declarations, Generations, HasSignificantBoundary)
			);
		}
		// 本文・位置・説明対象を所有する期待へ渡し終え，配置と元の構文を解析前に手放す
		Code.reset();
		// 配置の読取と所有期待の準備の終了
		return;
	};
	PrepareExpectation();
	InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, Src, !Ctx.IsEmbedded, true);
	// 正規化のリソース上限（試行解析の期限等）の見送の返戻
	if(Normalized.Reason.IsResourceLimit()) return { ValidationStatus::Skipped, std::nullopt, Normalized.Reason };
	if(Normalized.Reason != FailureReason::None || Normalized.IsFormChanged) {
		// 入力の表現として受理出来ない最終テキストの返戻
		return { ValidationStatus::Failed, std::nullopt, FailureReason::SyntaxBroken };
	}
	// 解析用のマーカーがコードへ残って居ない事の確認（実行停止後のデータは逐語領域として除く）
	const std::string_view CheckedCode(
		Normalized.Text.data(),
		Normalized.DataStart == std::string::npos ? Normalized.Text.size() : Normalized.DataStart
	);
	for(const std::string &Mark : Ctx.ProtectedRegions.AnalysisMarks) if(CheckedCode.find(Mark) != std::string_view::npos) {
		throw FormatLimitExceeded("analysis marks remain in the output", FailureReason::FormatterDefect);
	}
	ParseInput Adapted = ParseAdapter::Read(Profile, Normalized);
	if(Adapted.Decision != ParseDecision::Continue) {
		// パーサーの失敗の返戻
		if(Adapted.Decision == ParseDecision::Fail) return { ValidationStatus::Failed, std::nullopt, Adapted.Failure };
		// 読取のリソース上限は見送とし，入力が読めた完成候補を読めなく為った其の他の理由は構文の破壊とする分類（整形仕様 3.2, 8.3）の返戻
		return Adapted.Failure.IsResourceLimit() ?
		ValidationResult{ ValidationStatus::Skipped, std::nullopt, Adapted.Failure } :
		ValidationResult{ ValidationStatus::Failed, std::nullopt, FailureReason::SyntaxBroken };
	}
	const ParsedTree &ReadTree = *Adapted.Src;
	const bool HasPreparedText = Adapted.HasPreparedText;
	std::vector<uint32_t> PreparedLineStarts;
	if(HasPreparedText) {
		PreparedLineStarts.push_back(0);
		for(size_t Pos = 0; Pos < ReadTree.size(); ++Pos) if(ReadTree[Pos] == '\n') {
			PreparedLineStarts.push_back(static_cast<uint32_t>(Pos + 1));
		}
	}
	const std::string_view BeforeText = Normalized.Text;
	const std::string_view BeforeEdits = BeforeText.substr(0, ReadTree.OffsetInInput(ReadTree.GetCodeText().size()));
	Core::DiagnosticLocationMap Locations;
	// 最終テキストの座標と空行は，準備・修復前の本文を基準に保つ
	ByteRanges CommentRanges = ReadTree.CollectInputCommentRanges(ParseAdapter::GetOuterTextBounds(ReadTree).second);
	Adapted.Origin = ParseInput::VerificationSrc{ Normalized.Text };
	// 子の完成候補を子の期待構文・空行・意味・保護領域で独立して再検証する
	std::vector<ValidatedChild> Children;
	FailureReason ChildFailure = FailureReason::None, ChildSkip = FailureReason::None;
	ByteRanges Skipped = CommentRanges;
	if(ReadTree.GetReading().HasEmbeddedScripts) {
		const std::vector<ParseAdapter::EmbeddedContent> ActualChildren = ParseAdapter::EmbeddedContents(ReadTree);
		for(const ParseAdapter::EmbeddedContent &Embed : ActualChildren) {
			const TSPoint Start = ts_node_start_point(Embed.Element);
			Locations.Embedded.push_back({ Start.row, Start.column });
		}
		for(ValidationContext::ChildExpectation &Child : Ctx.OwnedChildren) {
			if(Child.Ordinal >= ActualChildren.size()) {
				ChildFailure = FailureReason::StructureChanged;
				continue;
			}
			if(!ParseAdapter::HasPreservedEmbeddedEnd(ReadTree, ActualChildren[Child.Ordinal])) ChildFailure = FailureReason::SyntaxBroken;
			const TSNode Raw = ActualChildren[Child.Ordinal].RawText;
			uint32_t Start = ts_node_is_null(Raw) ? ts_node_end_byte(ActualChildren[Child.Ordinal].Element) : ts_node_start_byte(Raw);
			std::string_view Content = ts_node_is_null(Raw) ? std::string_view() : ReadTree.GetView(Raw);
			if(!ts_node_is_null(Raw)) {
				Skipped.emplace_back(ReadTree.OffsetInInput(ts_node_start_byte(Raw)), ReadTree.OffsetInInput(ts_node_end_byte(Raw)));
			}
			// 親が配置したタグ間の境界改行だけを除き，子のコード・コメントの実テキストは其の儘検証する
			if(Content.starts_with('\n')) {
				Content.remove_prefix(1);
				++Start;
				if(
					const size_t Last = Content.rfind('\n');
					Last != std::string_view::npos && Content.substr(Last + 1).find_first_not_of('\t') == std::string_view::npos
				) Content = Content.substr(0, Last);
			}
			const std::string Candidate(Content);
			ValidationResult Checked = [&Child, &Candidate, ShouldDiagnose]() -> ValidationResult {
				const SyntaxValidator ChildValidator(ShouldDiagnose);
				// 適応側の所有経路は検証前に手放され，子の期待と文脈は此の経路だけが所有する
				Core::OutputExpectation ExpectedChild = std::move(*Child.Syntax);
				ValidationContext ChildContext = std::move(*Child.Ctx);
				Child.Syntax.reset();
				Child.Ctx.reset();
				// 所有経路を移した子の独立した検証の返戻
				return ChildValidator.Validate(
					*Child.Profile,
					std::move(Child.Code),
					Candidate,
					std::move(ExpectedChild),
					*Child.Comments,
					std::move(ChildContext)
				);
			}();
			Child.Comments.reset();
			switch(Checked.Status) {
			case ValidationStatus::Failed:
				ChildFailure = Checked.Reason;
				break;
			case ValidationStatus::Skipped:
				ChildSkip = Checked.Reason;
				break;
			default:
				Children.push_back({ Child.Ordinal, Start, std::move(*Checked.Output) });
				break;
			}
		}
	}
	MergeRanges(ErrorRanges);
	MergeRanges(Skipped);
	MergeRanges(Excluded);
	std::optional<ErrorZoneLayout> ErrorLayout;
	if(!Ctx.ErrorZones.empty()) {
		ErrorLayout.emplace(MapErrorZones(ErrorOwners, Ctx.ErrorZones, Spans, LaidOutput, Excluded, Normalized.Text, Skipped));
	}
	std::vector<std::vector<Core::NodeId>>().swap(ErrorOwners);
	// 空行の照合の骨格のコードの論理行の始まり（最終テキストと基準のテキスト，通常コメントへ変換したドキュメントの移動先の照合と共有する）
	std::vector<size_t> OutputStarts, LaidOutputStarts;
	const LineComparison Lines =
	CompareLayout(ReadTree, BeforeText, Skipped, LaidOutput, Excluded, ErrorRanges, OutputStarts, LaidOutputStarts);
	const bool IsBlankLineChanged = !Lines.HasSameBlanks;
	if(HasConvertedDocs) {
		// コードの論理行の始まりの断片（断片は改行で繋いだ為，前の改行の数）
		uint32_t Fragment = 0;
		size_t Counted = 0;
		for(const size_t Start : LaidOutputStarts) {
			Fragment += static_cast<uint32_t>(
				std::count(
					LaidOutput.begin() + static_cast<std::ptrdiff_t>(Counted),
					LaidOutput.begin() + static_cast<std::ptrdiff_t>(Start),
					'\n'
				)
			);
			Counted = Start;
			CodeLines.push_back(Fragment);
		}
	}
	// 空行と移動先の照合を終えた基準テキストと範囲は，独立投影へ持ち込まない
	std::string().swap(LaidOutput);
	ByteRanges().swap(Excluded);
	ByteRanges().swap(ErrorRanges);
	std::vector<size_t>().swap(LaidOutputStarts);
	if(Converted.empty()) std::vector<size_t>().swap(OutputStarts);
	// 修復前の破損位置と埋込領域の診断位置の収集
	if(ts_node_has_error(ReadTree.GetRoot())) {
		const TSPoint At = Adapted.PreparedFirstBroken;
		Locations.FirstBroken = { At.row, At.column };
	}
	// 入力と同じ修復を済ませ，検証する事実と投影を同じ木から得る
	if(!ParseAdapter::CanRepair(Adapted)) {
		// 独立して検出した空行の破壊を優先した失敗の返戻
		if(IsBlankLineChanged) return { ValidationStatus::Failed, std::nullopt, FailureReason::BlankLineChanged };
		// 修復のリソース上限（整形仕様 9.3）は見送とし，入力が読めた連鎖を完成候補が読めなく為った事は構文の破壊とする分類（整形仕様 3.2）の返戻
		return Adapted.Decision == ParseDecision::Skip && Adapted.Failure.IsResourceLimit() ?
		ValidationResult{ ValidationStatus::Skipped, std::nullopt, Adapted.Failure } :
		ValidationResult{ ValidationStatus::Failed, std::nullopt, FailureReason::SyntaxBroken };
	}
	ParsedTree Final = SyntaxParser::Parse(*Adapted.Profile, Adapted);
	const bool IsRejected = Final.GetParseDecision() != ParseDecision::Continue, HasLostBody = Final.HasEmptyBlock();
	// 宣言の値の中の `//` を読めない CSS / SCSS で，出力の行コメントが次の行の波括弧を本文へ新たに吸ったか（整形仕様 8.1 のトークンと値，入力に由来する物は入力の段で見送る）
	const bool IsBraceAbsorbed = Final.GetReading().CanAbsorbRulesIntoValComments && !Ctx.ProtectedRegions.InputText.empty() &&
	!HasNoExtraCommentBraces(Final.GetRules().Notation, Ctx.ProtectedRegions.InputText, Final, Final.GetReading().UrlFunctionHead);
	// 入力と同じ修復の後の１度だけの投影と，期待構文との正常領域の構造の照合（照合中に検出した内部の不変条件の違反は後で再送出する）
	bool IsNormalStructureKept = false, HasSyntaxError = false;
	std::vector<size_t> Lost;
	std::exception_ptr StructureError;
	std::optional<Core::ProjectionResult> Projection;
	try {
		// 受理拒否された部分整形の候補も投影し，失ったエラー領域の外の構造を独立して照合する
		if(!IsRejected || !Ctx.ErrorZones.empty()) {
			Projection.emplace(Core::SourceProjector::Project(Final, *Adapted.Profile, Adapted, ErrorLayout.has_value()));
			HasSyntaxError = ts_node_has_error(Final.GetRoot());
			// 投影は独立したモデルへ事実を渡し終えた為，照合中は本文と位置の対応だけを保持する
			Final.ReleaseSyntax();
			// 最終構文は正規化・中間表現の構築をしない為，照合と静的解析の読取を其の予算へ数えない（整形仕様 9.2）
			Projection->Syntax.CloseWork();
			Locations.Nodes = std::move(Projection->Validation.InputDiagnostics.Positions);
			if(ErrorLayout) Lost = LostErrorZones(Ctx.ErrorZones, *ErrorLayout);
			// 環境変数 `SHAVEFMT_STRUCT_DIAG` が有る時だけ期待構文との差を診断する（整形仕様 10.5，値の中身は問わない）
			if(Expectation.NormalizedSyntax) {
				const KindCmp Kinds { Ctx.SemanticChecks.Symbols, Projection->Validation.SemanticChecks.Symbols, Profile };
				if(Lost.empty()) IsNormalStructureKept = HasSameStructure(Projection->Syntax, Expectation, Kinds, ShouldDiagnose);
				else {
					const ErrorZoneMasks Masks =
					ClassifyErrorZones(*Expectation.NormalizedSyntax, Projection->Syntax, Lost, Spans, *ErrorLayout, Locations.Nodes);
					IsNormalStructureKept = HasMatchingStructure<NodeMatch>(
						Projection->Syntax,
						Expectation,
						Kinds,
						ShouldDiagnose,
						Masks.Expected,
						Masks.Actual,
						HasMatchingContextualTypeRepresentation,
						HasMatchingDeclPartition
					);
				}
			}
		} else if(ShouldDiagnose) std::fputs("STRUCT-DIAG unavailable\n", stderr);
	} catch(...) {
		StructureError = std::current_exception();
	}
	// 入力側の構造は照合を終え，後の意味・コメントの照合と静的解析は独立した候補だけを読む
	Expectation.NormalizedSyntax.reset();
	std::vector<NodeSpan>().swap(Spans);
	if(StructureError) {
		// 照合より前に独立して検出した空行の破壊を，照合中のリソースの上限等の送出より優先した失敗（整形仕様 8.3）の返戻
		if(IsBlankLineChanged) return { ValidationStatus::Failed, std::nullopt, FailureReason::BlankLineChanged };
		// 照合より前に検出した本体を失ったブロックの返戻
		if(HasLostBody) return { ValidationStatus::Failed, std::nullopt, FailureReason::EmptyBlock };
		// 照合より前に検出した波括弧の吸込の返戻
		if(IsBraceAbsorbed) return { ValidationStatus::Failed, std::nullopt, FailureReason::SyntaxBroken };
		// 照合中に検出した内部の不変条件の違反とリソースの上限の再送出
		std::rethrow_exception(StructureError);
	}
	// 記号の対応は構造の照合で読み終え，失敗分類と静的解析は其の列を使わない
	if(Projection) std::vector<TSSymbol>().swap(Projection->Validation.SemanticChecks.Symbols);
	ValidationContext Candidate;
	if(Projection) Candidate = std::move(Projection->Validation);
	else {
		// 投影しない候補は，解析木から失敗と見送の分類データを集める
		std::vector<TSNode> Nodes;
		WalkAst(
			Final.GetRoot(),
			[&Nodes](const TSNode Node) -> void {
				Nodes.push_back(Node);
				return;
			}
		);
		Candidate = Core::SourceProjector::CollectCtx(Final, Profile, Adapted, Nodes);
		if(ErrorLayout) Lost = LostErrorZones(Ctx.ErrorZones, *ErrorLayout);
		Candidate.ProtectedRegions.Directives = Core::SourceProjector::CollectDirectives(Final);
		HasSyntaxError = ts_node_has_error(Final.GetRoot());
	}
	// 入力に無かった構文エラーの有無（両側とも修復前後の少ない方の件数で比べる）
	const bool IsSyntaxBroken = HasSyntaxError && (
		Ctx.InputDiagnostics.InputBrokenCount ?
		std::min(Adapted.PreparedBroken, Candidate.InputDiagnostics.BrokenCount) > Ctx.InputDiagnostics.InputBrokenCount :
		static_cast<bool>(Candidate.InputDiagnostics.BrokenCount)
	);
	const bool IsDirectiveChanged = Candidate.ProtectedRegions.Directives != Ctx.ProtectedRegions.Directives;
	const bool IsDirectiveCommentChanged = Projection && Profile.GetReading().HasPreprocMacros && DirectiveComments !=
	GetDirectiveCommentFacts(Projection->Syntax, Projection->Comments, Candidate.ProtectedRegions.KeptComments);
	// 直接付くドキュメントの説明対象と本文，通常コメントへ変換したドキュメントの本文と移動先の保持（期待コメントと比べる，整形仕様 7.6，8.1）
	// 変換したドキュメントは空行の形と正常行の対応が一致すれば照合する（エラー本文の変化で独立したドキュメントの破壊を隠さない）
	const bool IsDocChanged = Projection && (
		!HasSameDocs(Docs, Candidate.ProtectedRegions.Docs, Outputs, Placed) ||
		!Converted.empty() && !IsBlankLineChanged && Lines.HasSameNormalCode && !HasMatchingConvertedDocs(
			Converted,
			Ordinary,
			CommentRuns(
				Profile.GetReading(),
				Projection->Comments,
				BeforeEdits,
				Candidate.InputDiagnostics.CommentRecords,
				OutputStarts,
				CodeLines
			)
		)
	);
	// 行末の各記号の前置区切を，同じ最初の論理行を持つ期待コメントの記録のタブの数と照合する（並替・統合で記録の順とアンカーが変わっても記号毎に対応させる，整形仕様 7.6, 8.1）
	using SeparatorCounts = std::unordered_map<size_t, size_t>; // 同じ本文を持つ区切のタブ数毎の残数
	std::unordered_map<std::string_view, SeparatorCounts> Recorded;
	for(size_t Idx = 0; Idx < Records.size(); ++Idx) if(Records[Idx].Kind == CommentKind::Trailing) {
		const std::string Marker = CommentNormalizer::GetStackMarkerOf(Core::CommentRepository::GetOutputNotationOf(Outputs[Idx]));
		std::string_view Rest = Outputs[Idx].Text;
		size_t Tabs = Records[Idx].Format.LeadingTabCount;
		do {
			const std::string_view Line = CommentNormalizer::GetFirstLogicalLine(Rest, Marker);
			++Recorded[Line][Tabs];
			Rest.remove_prefix(Line.size());
			// 次の記号の前置区切（本文の中の記号は本文の空白のタブの数，改行で分けた論理行は積む時のスペース１つ）
			const size_t GapEnd = std::min(Rest.find_first_not_of(" \t"), Rest.size());
			Tabs = static_cast<size_t>(std::ranges::count(Rest.substr(0, GapEnd), '\t'));
			Rest.remove_prefix(GapEnd);
			if(Rest.starts_with('\n')) {
				Rest.remove_prefix(1);
				Tabs = 0;
			} else if(!Marker.empty() && Rest.starts_with(Marker)) Rest.remove_prefix(Marker.size());
			else break;
		} while(!Rest.empty());
	}
	bool IsSeparatorChanged = false;
	for(const ValidationContext::LeadingSeparator &Separator : Candidate.InputDiagnostics.LeadingSeparators) {
		const std::unordered_map<std::string_view, SeparatorCounts>::iterator Found = Recorded.find(Separator.Line);
		// 不正な区切の検出
		IsSeparatorChanged = Separator.Tabs == SIZE_MAX;
		if(!IsSeparatorChanged && Found != Recorded.end()) {
			const SeparatorCounts::iterator Count = Found->second.find(Separator.Tabs);
			IsSeparatorChanged = Count == Found->second.end() || !Count->second;
			if(!IsSeparatorChanged) --Count->second;
		}
		if(IsSeparatorChanged) break;
	}
	// ディレクティブコメント位置とエラー領域の照合
	Locations.DirectiveStarts = std::move(Candidate.ProtectedRegions.DirectiveStarts);
	for(uint32_t &Start : Locations.DirectiveStarts) Start = Final.OffsetInInput(Start);
	const size_t LostZones = Lost.size();
	Code.reset();
	const bool IsNormalRegionBroken =
	IsSyntaxBroken && (!LostZones || CountUnmatchedErrorZones(Candidate.ErrorZones, Ctx.ErrorZones) > LostZones);
	// 正常領域に生じた新しい構文破壊（記録したエラー領域の保持違反に伴う物は後で見送とする，整形仕様 8.3）の返戻
	if(IsNormalRegionBroken) return { ValidationStatus::Failed, std::nullopt, FailureReason::SyntaxBroken };
	if(Candidate.SemanticChecks.Checks != Ctx.SemanticChecks.Checks) {
		// 意味上の区別を保てて居ない事の返戻
		return { ValidationStatus::Failed, std::nullopt, FailureReason::SemanticsChanged };
	}
	// ディレクティブコメントの保持契約に違反した候補を救済せず失敗とする事（整形仕様 8.3）の返戻
	if(IsDirectiveChanged) return { ValidationStatus::Failed, std::nullopt, FailureReason::DirectiveChanged };
	// 整形仕様 7.4 で認識したドキュメントの説明対象を保てない候補を救済せず失敗とする事（整形仕様 7.7, 8.3）の返戻
	if(IsDocChanged) return { ValidationStatus::Failed, std::nullopt, FailureReason::DocChanged };
	// 安全な配置を持つ前処理のコメントの欠落・改変・複製・順序又は種別の変更の失敗の返戻
	if(IsDirectiveCommentChanged) {
		// 前処理のコメントの保持違反の返戻
		return {
			ValidationStatus::Failed,
			std::nullopt,
			{ FailureReason::FormatterDefect, "comments in a preprocessor directive differ from their records" }
		};
	}
	if(IsSeparatorChanged) {
		// 行末コメントの前置区切を記録と違えた配置の不具合の返戻
		return {
			ValidationStatus::Failed,
			std::nullopt,
			{ FailureReason::FormatterDefect, "a line-end comment separator differs from its record" }
		};
	}
	// 空行を追加・削除・移動又は上書した候補を失敗とする事（コメントの配置の不具合）の返戻
	if(IsBlankLineChanged) return { ValidationStatus::Failed, std::nullopt, FailureReason::BlankLineChanged };
	// 本体を失ったブロックを作った事の返戻
	if(HasLostBody) return { ValidationStatus::Failed, std::nullopt, FailureReason::EmptyBlock };
	// 出力のコメントが波括弧を新たに吸った構文破壊（整形仕様 8.3）の返戻
	if(IsBraceAbsorbed) return { ValidationStatus::Failed, std::nullopt, FailureReason::SyntaxBroken };
	// 子の保護領域又は意味の保持違反（見送より失敗を優先する）の返戻
	if(ChildFailure != FailureReason::None) return { ValidationStatus::Failed, std::nullopt, ChildFailure };
	// 其の他の受理拒否を正常領域を読めない構文破壊とする失敗の返戻
	if(IsRejected && !LostZones) return { ValidationStatus::Failed, std::nullopt, FailureReason::SyntaxBroken };
	// 喪失した領域と独立した正常領域の構造差を持つ候補の失敗の返戻
	if(!IsNormalStructureKept) return { ValidationStatus::Failed, std::nullopt, FailureReason::StructureChanged };
	// 正常な構造を保った儘，コード行を別の構文境界へ割り振った候補の失敗の返戻
	if(!Lines.HasSameNormalCode) return { ValidationStatus::Failed, std::nullopt, FailureReason::BlankLineChanged };
	// 解析不能領域の保持を確認出来ない候補の拒否（整形仕様 8.3 の見送）の返戻
	if(LostZones) return { ValidationStatus::Skipped, std::nullopt, FailureReason::ErrorZoneLost };
	// 子の独立検証のリソース上限又はエラー領域の保持不能の返戻
	if(ChildSkip != FailureReason::None) return { ValidationStatus::Skipped, std::nullopt, ChildSkip };
	Locations.Text = std::move(Normalized.Text);
	// 静的解析の位置の行と桁（修復の前のテキストの行頭の位置から求める）と最終テキストのコメントの記録
	Locations.LineStarts = std::move(Normalized).TakeLineStarts();
	// ノード範囲は部分整形の照合から引き継ぎ，行と桁・コメント位置は静的解析の初回に求める
	if(HasPreparedText) {
		// 準備前の診断座標への復元
		const auto RestorePoint = [&](Core::OutputPoint &Point) -> void {
			Point = Locations.GetPointAt(Final.OffsetBeforePreparation(PreparedLineStarts[Point.Row] + Point.Column));
			// 終了
			return;
		};
		RestorePoint(Locations.FirstBroken);
		for(Core::OutputPoint &Element : Locations.Embedded) RestorePoint(Element);
		for(ValidatedChild &Child : Children) Child.Start = Final.OffsetBeforePreparation(Child.Start);
	}
	// 検証を通った候補の返戻
	return {
		ValidationStatus::Verified,
		Core::ValidatedOutput{
			std::move(Projection->Syntax),
			std::move(Projection->Comments),
			std::move(Locations),
			std::move(Projection->PrepareLint)
		},
		FailureReason::None,
		std::move(Children)
	};
}

/**
 * 所有する期待を解析前に縮約する検証関数
 * 計算量：期待の縮約と共通の独立検証の処理量に対し線形
 * @param Profile 入力で確定した文法
 * @param Code 空行の照合基準
 * @param Src 整形の済んだ最終テキスト
 * @param Expectation 以後の工程が読まない入力側の所有期待
 * @param Comments 同期・本文正規化済の期待コメント
 * @param Ctx 入力側の意味・保護領域と子の独立検証の所有経路
 * @return 検証の結果（完成候補は独立して解析・投影する）
 * @throws FormatLimitExceeded 独立検証が内部の不変条件の違反を検出した場合
 * @throws std::bad_alloc 期待の縮約又は共通の独立検証を確保出来ない場合
 * @throws std::invalid_argument 共通の独立検証の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 共通の独立検証の入出力又は同期が失敗した場合
 */
SyntaxValidator::ValidationResult SyntaxValidator::Validate(
	const GrammarProfile &Profile,
	std::shared_ptr<const LaidOutCode> Code,
	const std::string &Src,
	Core::OutputExpectation &&Expectation,
	const Core::CommentRepository &Comments,
	ValidationContext Ctx
) const {
	return ValidationResult::ValidateImpl(Profile, Ctx, std::move(Code), Src, Comments, Expectation, ShouldDiagnoseStructure);
} // 所有する期待を縮約した検証結果の返戻

/**
 * 構造の差の診断指定を受け取るコンストラクタ
 * @param Diagnose 検証結果を変えずに補助診断を出す指定
 */
SyntaxValidator::SyntaxValidator(const bool Diagnose) : ShouldDiagnoseStructure(Diagnose) {}
