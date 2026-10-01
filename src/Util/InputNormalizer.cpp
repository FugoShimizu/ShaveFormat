#include "InputNormalizer.hpp"
#include "CharClass.hpp"
#include "../Core/CommentRepository.hpp"
#include "FileIO.hpp"
#include "../Profile/GrammarProfile.hpp"
#include "ParseAdapter.hpp"
#include "TextEdit.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * Unicode の区切りを改行と読む入力に其の区切りが在るかの判定関数
 * NEL・行区切り・段落区切りは正規化で LF に為る為，見送る時に原文へ戻せる様に控える要否の判定にも用いる
 * @param Profile 対象の文法
 * @param Source 原文
 * @return 文法が改行と読む Unicode の区切りを原文が含めば true
 */
bool InputNormalizer::HasUnicodeSeparator(const GrammarProfile &Profile, const std::string_view Source) {
	const std::vector<std::string_view> &Separators = Profile.GetReading().UnicodeLineSeparators;
	// 改行と読む区切りの有無の返戻
	return std::any_of(
		Separators.begin(),
		Separators.end(),
		[&Source](const std::string_view Separator) -> bool {
			// 改行に読む区切りを含むかの返戻
			return Source.find(Separator) != std::string_view::npos;
		}
	);
}

/**
 * CRLF の正規化関数
 * リテラルと文書の逐語内容の内側の CRLF は値其の物の為に保ち，其れ以外の CRLF を LF へ畳む
 * 末尾の改行の並びは，PHP の地の文・Ruby の `__END__` 以降のデータの中身に為り得る為，混在する入力では原文の儘保つ（整形仕様 4.4）
 * @param Source 正規化対象の原文（CRLF を含む，結果で上書きされる）
 * @param Literals 改行を値其の物として保つ範囲（原文順）
 * @param KeepsTailRun 末尾の改行の並びを原文の儘保つか（改行様式が混在する入力）
 * @return 逐語区画の外の CRLF を畳んだ場合 true
 */
bool InputNormalizer::FoldCrLfOutside(
	std::string &Source,
	const std::vector<std::pair<uint32_t, uint32_t>> &Literals,
	const bool KeepsTailRun
) {
	// 正規化先と変更状態の準備
	std::string Normalized;
	Normalized.reserve(Source.size());
	size_t LiteralIdx = 0;
	bool IsFolded = false;
	// 原文の儘保つ末尾の改行の並びの始まり
	const size_t TailRun = KeepsTailRun ? Source.find_last_not_of("\r\n") + 1 : Source.size();
	// 原文バイトの走査
	for(size_t Idx = 0; Idx < Source.size(); ++Idx) {
		while(LiteralIdx < Literals.size() && Literals[LiteralIdx].second <= Idx) ++LiteralIdx;
		// リテラルと保つ末尾の外の CRLF だけを LF へ畳む
		if(
			Source[Idx] == '\r' && Idx + 1 < Source.size() && Source[Idx + 1] == '\n' && Idx < TailRun &&
			!(LiteralIdx < Literals.size() && Literals[LiteralIdx].first <= Idx)
		) {
			IsFolded = true;
			continue;
		}
		Normalized += Source[Idx];
	}
	// 正規化結果への置換
	Source = std::move(Normalized);
	// リテラルの外の CRLF を畳んだか（書戻で改行様式が変わるか）の返戻
	return IsFolded;
}

/**
 * 単独の CR を行の終わりとして読む記法かの判定関数（整形仕様 4.1）
 * @param Profile 対象の文法
 * @return 単独の CR を LF へ揃える記法なら true
 */
bool InputNormalizer::ReadsLoneCrAsNewline(const GrammarProfile &Profile) {
	// 文法が定める改行の読みの返戻
	return Profile.GetReading().ReadsLoneCrAsNewline;
}

/**
 * 改行コードの LF 基準の正規化関数
 * 改行様式が混在する入力は，逐語区画の内側の改行と末尾の改行の並びを原文の儘保ち，其れ以外の CRLF を LF へ揃える（書戻は LF の儘）
 * 末尾の改行の数は揃えない（中身に為る末尾は投影が保ち，其の他は出力の終端契約が揃える）
 * @param Profile 対象の文法
 * @param Source 正規化対象の原文（結果で上書きされる）
 * @param HasUnicodeSeparator 原文が文法で改行と読む Unicode の区切りを含むか
 * @param Reason 改行を値として保つ範囲を確定出来ず正規化しなかった理由の格納先
 * @param Trial 同じ字面を解析済なら再利用する試験解析の結果
 * @return 末尾の改行の数以外の改行の姿（CRLF・単独の CR・文法が読む Unicode の区切り）を変えた場合 true
 */
bool InputNormalizer::NormalizeNewlines(
	const GrammarProfile &Profile,
	std::string &Source,
	const bool HasUnicodeSeparator,
	FailureReason &Reason,
	std::optional<ParsedTree> &Trial
) {
	// 改行表現を変えた印
	bool IsFormChanged = false;
	// 単独 CR を改行と読む言語だけ LF 化し，空白・地の文・リテラルの CR は保持
	const bool IsLoneCrNewline = ReadsLoneCrAsNewline(Profile);
	bool IsLoneCr = false;
	for(
		size_t Pos = IsLoneCrNewline ? Source.find('\r') : std::string::npos;
		Pos != std::string::npos && !IsLoneCr;
		Pos = Source.find('\r', Pos + 1) // 単独 CR の検出
	) IsLoneCr = Pos + 1 >= Source.size() || Source[Pos + 1] != '\n';
	// 個別の改行列を含む場合
	if(IsLoneCr || HasUnicodeSeparator) {
		// 保護範囲列（確定出来なければリテラルの中を書き換えない様に正規化しない）
		const std::optional<std::vector<std::pair<uint32_t, uint32_t>>> Protected =
		ParseAdapter::LiteralRanges(Profile, Source, HasUnicodeSeparator, Reason, Trial);
		// 保護範囲を確定出来ない事の返戻
		if(!Protected) return false;
		const std::vector<std::pair<uint32_t, uint32_t>> &Literals = *Protected;
		const std::vector<std::string_view> &Separators = Profile.GetReading().UnicodeLineSeparators;
		// 個別改行を LF 化する出力
		std::string Normalized;
		Normalized.reserve(Source.size());
		size_t LiteralIdx = 0;
		// 原文バイトの走査
		for(size_t Idx = 0; Idx < Source.size(); ++Idx) {
			while(LiteralIdx < Literals.size() && Literals[LiteralIdx].second <= Idx) ++LiteralIdx;
			// 保護範囲内の場合
			if(LiteralIdx < Literals.size() && Literals[LiteralIdx].first <= Idx) {
				Normalized += Source[Idx];
				continue;
			}
			// 単独 CR の LF 化
			if(IsLoneCr && Source[Idx] == '\r' && (Idx + 1 >= Source.size() || Source[Idx + 1] != '\n')) {
				Normalized += '\n';
				IsFormChanged = true;
				continue;
			}
			if(HasUnicodeSeparator) if(
				const std::vector<std::string_view>::const_iterator Separator = std::find_if(
					Separators.begin(),
					Separators.end(),
					[&Source, Idx](const std::string_view Candidate) -> bool {
						// 此処から改行に読む区切りが始まるかの返戻
						return !Source.compare(Idx, Candidate.size(), Candidate);
					}
				); Separator != Separators.end()
			) {
				// 文法が読む Unicode の区切りの LF 化
				Normalized += '\n';
				Idx += Separator->size() - 1;
				IsFormChanged = true;
				continue;
			}
			Normalized += Source[Idx];
		}
		Source = std::move(Normalized);
	}
	// リテラル外の CRLF の正規化
	if(Source.find("\r\n") != std::string::npos) {
		// 混在する入力は末尾の改行の並びを原文の儘保つ（CRLF で統一された入力は末尾も LF へ揃える）
		const bool IsMixed = FileIO::IsNewlineMixed(Source);
		// 値を構成するリテラルの内側の CRLF を保つ為の保護範囲（整形仕様 4.1, 10.3）
		const std::optional<std::vector<std::pair<uint32_t, uint32_t>>> Literals =
		ParseAdapter::LiteralRanges(Profile, Source, false, Reason, Trial);
		// 保護範囲を確定出来ない事の返戻
		if(!Literals) return false;
		// CRLF を畳んだかも改行の姿の変化に含めた結果の返戻（統一された入力の LF 化も整形差分に含める，整形仕様 10.3）
		return FoldCrLfOutside(Source, *Literals, IsMixed) || IsFormChanged;
	}
	// 改行の姿を変えたかの返戻（末尾の改行の数は出力の終端契約が揃え，中身の改行は投影が保つ）
	return IsFormChanged;
}

/**
 * 共有拡張子の対象外の方言の宣言の判定関数
 * @param Rules 入力の読取規則
 * @param Source 整形対象のソース（末尾改行正規化済）
 * @return 対象外の方言の印が行頭に現れたら true
 */
bool InputNormalizer::LooksForeignDialect(const ReadingRules &Rules, const std::string &Source) {
	// 囲みコメントを除いた行頭で，別語との部分一致を避け `#import`・`@interface` 系を検出
	const std::string_view BlockOpen = Core::CommentRepository::OpenerOf(CommentNotation::Block);
	const std::string_view BlockClose = Core::CommentRepository::CloserOf(CommentNotation::Block);
	const std::string_view LineOpen = Core::CommentRepository::OpenerOf(CommentNotation::SlashLine);
	bool IsInBlockComment = false;
	// 原文の行走査
	for(size_t LineStart = 0; LineStart < Source.size();) {
		const size_t LineEnd = Source.find('\n', LineStart);
		const std::string_view Line(Source.data() + LineStart, (LineEnd == std::string::npos ? Source.size() : LineEnd) - LineStart);
		// 前処理指令も `@interface` も行頭インデントが許される為，先頭の空白を読み飛ばしてから照合する
		size_t Head = 0;
		while(Head < Line.size() && (Line[Head] == ' ' || Line[Head] == '\t')) ++Head;
		if(const std::string_view Code = Line.substr(Head); !IsInBlockComment) {
			for(const std::string_view Marker : Rules.ForeignDialectHeads) if(Code.starts_with(Marker)) {
				// Objective-C の印を発見した事の返戻
				if(const size_t After = Marker.size(); After >= Code.size() || !IsIdentifierChar(Code[After])) return true;
			}
		}
		// ブロックコメントの開閉を数え，次行へ状態を持ち越す（行コメントは其の行の残りを覆うだけで持ち越さない）
		for(size_t Pos = 0; Pos + 1 < Line.size(); ++Pos) {
			if(IsInBlockComment && Line.substr(Pos).starts_with(BlockClose)) {
				IsInBlockComment = false;
				++Pos;
			} else if(!IsInBlockComment && Line.substr(Pos).starts_with(BlockOpen)) {
				IsInBlockComment = true;
				++Pos;
			} else if(!IsInBlockComment && Line.substr(Pos).starts_with(LineOpen)) break;
		}
		if(LineEnd == std::string::npos) break;
		LineStart = LineEnd + 1;
	}
	// 印が無い事の返戻
	return false;
}

/**
 * 共有拡張子の対象外の文書の判定関数
 * @param Rules 入力の読取規則
 * @param Source 整形対象のソース
 * @return 対象外の文書の印で始まれば true
 */
bool InputNormalizer::LooksMarkupDocument(const ReadingRules &Rules, const std::string &Source) {
	// 最初の非空白位置
	const size_t Head = Source.find_first_not_of(" \t\r\n");
	// 宣言・文書型で始まるかの返戻
	return Head != std::string::npos && std::any_of(
		Rules.MarkupDocumentHeads.begin(),
		Rules.MarkupDocumentHeads.end(),
		[&Source, Head](const std::string_view Marker) -> bool {
			// 文書の印との先頭一致の返戻
			return !Source.compare(Head, Marker.size(), Marker);
		}
	);
}

/**
 * 先頭の UTF-8 の BOM の数の取得関数
 * @param Source 原文
 * @return 先頭に続く BOM (EF BB BF) の数（リテラル・コメントの中の物は数えない）
 */
size_t InputNormalizer::CountBom(const std::string_view Source) {
	size_t Count = 0;
	while(Source.substr(3 * Count).starts_with("\xEF\xBB\xBF")) ++Count;
	// BOM の数の返戻
	return Count;
}

/**
 * 印の無い多バイト符号化の周期的な零バイトの判定関数
 * @param Source 実行停止後の逐語データを除いた原文
 * @return UTF-16 又は UTF-32 の ASCII 部分に相当する零バイトの並びなら true
 */
static bool LooksWideEncoding(const std::string_view Source) {
	const size_t Size = std::min(Source.size(), size_t{ 32 });
	// 先頭で零バイトの周期を持つ広い符号化の１文字以上を確かめる
	for(const size_t Width : { size_t{ 4 }, size_t{ 2 } }) {
		if(Source.size() % Width || Size < Width) continue;
		for(size_t TextByte = 0; TextByte < Width; ++TextByte) {
			bool Matches = true;
			for(size_t Pos = 0; Pos + Width <= Size; Pos += Width) {
				for(size_t Part = 0; Part < Width; ++Part) if(Source[Pos + Part] == '\0' != (Part != TextByte)) {
					Matches = false;
					break;
				}
				if(!Matches) break;
			}
			// 周期的な零バイトを検出した事の返戻
			if(Matches) return true;
		}
	}
	// 多バイト符号化の印が無い事の返戻
	return false;
}

/**
 * 入力の表現の受理の判定と正規化関数（整形仕様 3.4 の Normalize，4.1）
 * 符号化・BOM・改行様式を受理の条件と照らし，正規化した字面を新しい値として返す（原文は書き換えない）
 * 入力指定に依る方言の判定は，選択状態を受け取ってから別に行う
 * @param Profile 対象の文法
 * @param RawSource 整形対象の原文
 * @return 正規化の結果（受理しない入力は見送理由を持つ）
 */
InputNormalizer::NormalizedInput InputNormalizer::Normalize(const GrammarProfile &Profile, const std::string &RawSource) {
	NormalizedInput Input;
	// 文字として読めない原本の見送（UTF-16 の印と，其れに前が重ならない UTF-32 ビッグエンディアンの印）
	// 空文字は一律に拒まず，文法が受理するかで判る（整形仕様 4.1）
	if(
		RawSource.starts_with("\xFF\xFE") || RawSource.starts_with("\xFE\xFF") ||
		RawSource.starts_with(std::string_view("\0\0\xFE\xFF", 4))
	) {
		Input.Reason = FailureReason::UnsupportedEncoding;
		// 対応外符号化の見送の返戻
		return Input;
	}
	// 先頭の UTF-8 の BOM を除いた字面（成功した出力は入口に依らず BOM 無の UTF-8，整形仕様 4.1, 10.3）
	Input.BomCount = CountBom(RawSource);
	Input.Text.assign(RawSource, 3 * Input.BomCount);
	// 実行停止後のデータを正規化と符号化検査の対象から外す（試験解析は BOM を除いた字面で行い，後の解析で再利用する）
	const size_t DataStart = ParseAdapter::HaltedDataStart(Profile, Input.Text, Input.Trial);
	std::string Data;
	if(DataStart != std::string::npos) {
		Data.assign(Input.Text, DataStart);
		Input.Text.resize(DataStart);
	}
	// 正規化対象の符号化の検査
	if(LooksWideEncoding(Input.Text) || !TextEdit::IsValidUtf8(Input.Text)) {
		Input.Reason = FailureReason::UnsupportedEncoding;
		// UTF-8 として読めない原本の見送の返戻
		return Input;
	}
	// 改行の正規化（改行を値として保つ範囲を確定出来なければ正規化せずに見送る）
	const bool IsNewlineChanged =
	NormalizeNewlines(Profile, Input.Text, HasUnicodeSeparator(Profile, Input.Text), Input.Reason, Input.Trial);
	Input.IsFormChanged = IsNewlineChanged || Input.BomCount;
	// 改行を正規化出来ない入力の見送の返戻
	if(Input.Reason != FailureReason::None) return Input;
	// 実行停止後のデータの復帰
	if(DataStart != std::string::npos) Input.DataStart = Input.Text.size();
	Input.Text += Data;
	// 中身で判別する共有拡張子の別の方言の宣言と，拡張子を共有する文書の見送
	if(
		const ReadingRules &Rules = Profile.GetReading();
		Rules.HasAmbiguousHeaderDialect && LooksForeignDialect(Rules, Input.Text)
	) Input.Reason = FailureReason::HeaderDialect;
	else if(Rules.MayContainMarkupDocument && LooksMarkupDocument(Rules, Input.Text)) Input.Reason = FailureReason::MarkupDocument;
	// 正規化の結果の返戻
	return Input;
}
