#include "SyntaxValidator.hpp"
#include "../Core/DocCommentSynchronizer.hpp"
#include "../Core/FormatIR.hpp"
#include "../Core/SourceProjector.hpp"
#include "../Core/SyntaxNormalizer.hpp"
#include "../Profile/GrammarProfile.hpp"
#include "CommentNormalizer.hpp"
#include "FormatLimitExceeded.hpp"
#include "InputNormalizer.hpp"
#include "ParseAdapter.hpp"
#include "SyntaxParser.hpp"
#include "TextEdit.hpp"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <limits>
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
 * 行の骨格の取得関数（整形仕様 8.1 の空行の不変性と，通常コメントへ変換した文書の移動先）
 * コメントの範囲を除いた論理行毎に，空行か，空白を除いたコードの字面かを並べる
 * コメントの内側の改行は行を分けず，コメントだけの行は除く（コメントの行は空行でもコードでもない）
 * 計算量：字面の長さ N とコメントの数 C に対し O(N + C)
 * @param Decoder 字面へ置いた解析用の印を行毎に本来の字句へ復号する解析結果（印を置いて居なければ無し）
 * @param Text 字面
 * @param Comments コメントの範囲（位置順，重ならない）
 * @param CodeStarts コードの論理行の始まりの位置の格納先（要らなければ無し）
 * @return 論理行毎の骨格（空行は空の字面）
 */
std::vector<SyntaxValidator::SkeletonLine> SyntaxValidator::LineSkeleton(
	const ParsedTree *const Decoder,
	const std::string_view Text,
	const std::span<const std::pair<uint32_t, uint32_t>> Comments,
	std::vector<size_t> *const CodeStarts
) {
	// 行骨格と走査位置の初期化
	std::vector<SkeletonLine> Lines;
	std::string Code;
	bool HasComment = false;
	size_t Next = 0, LineStart = 0;
	for(size_t Pos = 0;;) {
		// コメントの範囲の読飛（内側の改行は行を分けない）
		if(Next < Comments.size() && Pos == Comments[Next].first) {
			HasComment = true;
			Pos = std::max<size_t>(Pos, Comments[Next++].second);
			continue;
		}
		if(Pos >= Text.size() || Text[Pos] == '\n') {
			// 解析用の印の復号（印は行を占める指令と字数を変えない字に限る為，行毎に戻せる）
			if(Decoder && !Code.empty()) {
				Code = ParseAdapter::DecodeMarks(*Decoder, std::move(Code));
				std::erase_if(
					Code,
					[](const char Char) -> bool {
						// 空白の字かの返戻
						return Char == ' ' || Char == '\t' || Char == '\r';
					}
				);
			}
			// 論理行の確定（コメントだけの行は除き，末尾の改行の後の空の行は数えない）
			if(!Code.empty()) {
				Lines.push_back({ false, std::move(Code) });
				if(CodeStarts) CodeStarts->push_back(LineStart);
			} else if(!HasComment && Pos < Text.size()) Lines.push_back({ true, {} });
			Code.clear();
			HasComment = false;
			// 字面の終わりでの脱出
			if(Pos >= Text.size()) break;
			LineStart = ++Pos;
			continue;
		}
		// 空白を除いたコードの字の蓄積（印を復号する行は指令の語の区切りを保つ為に空白も残す）
		if(Decoder || Text[Pos] != ' ' && Text[Pos] != '\t' && Text[Pos] != '\r') Code += Text[Pos];
		++Pos;
	}
	// 行の骨格の返戻
	return Lines;
}

/**
 * 空行の不変性の判定関数（整形仕様 8.1）
 * `CodeLayout` の確定した空行の有無・数・前後のコード行との対応を，コメント復元と終端処理の後の字面が保つかを確かめる
 * @param Checked 検証の為に解析し直した最終字面
 * @param Comments 最終字面でコメントと読まれる範囲（位置順）
 * @param LaidOut `CodeLayout` が返した変更不能なコードの断片を改行で繋いだ基準の字面
 * @param Excluded 基準の字面の中で文法がコメントと読む区切記法と逐語区画に残したコメントの範囲（位置順）
 * @param OutputStarts 最終字面のコードの論理行の始まりの位置の格納先（通常コメントへ変換した文書の移動先の照合と共有する）
 * @param LaidOutStarts 基準の字面のコードの論理行の始まりの位置の格納先
 * @return 空行を追加・削除・移動・上書して居なければ true
 */
bool SyntaxValidator::KeepsBlankLines(
	const ParsedTree &Checked,
	const ByteRanges &Comments,
	const std::string_view LaidOut,
	const std::span<const std::pair<uint32_t, uint32_t>> Excluded,
	std::vector<size_t> &OutputStarts,
	std::vector<size_t> &LaidOutStarts
) {
	// 構文木の無い字面は照合の基準のコメントの範囲を持たない為の返戻
	if(!Checked.IsParsed()) return true;
	const size_t DataSize = Checked.size() - Checked.CodeText().size();
	// 逐語データが基準の字面より長い最終字面の返戻
	if(DataSize > LaidOut.size()) return false;
	// 基準と最終字面の行の骨格の一致の返戻
	return LineSkeleton(Checked.GetAnalysisMark() ? &Checked : nullptr, Checked.CodeText(), Comments, &OutputStarts) ==
	LineSkeleton(nullptr, LaidOut.substr(0, LaidOut.size() - DataSize), Excluded, &LaidOutStarts);
}

/**
 * 置く論理行の取得関数（整形仕様 7.3, 7.7, 8.1）
 * 通常コメントとして置く本文の論理行を，行末の空白を除き，内容の無い論理行は構造として保つ物だけを残して並べる（配置の段と同じ本文構造の判定に従う）
 * 計算量：本文の長さ N に対し O(N)
 * @param Text 期待コメントの出力記法の本文
 * @param KeptBlanks 論理行毎の構造として保つ空の論理行の印
 * @return 置く論理行
 */
std::vector<std::string> SyntaxValidator::PlacedLines(const std::string_view Text, const std::span<const uint8_t> KeptBlanks) {
	// 本文の論理行の切出
	std::vector<std::string> Lines;
	for(size_t Start = 0, Line = 0; Start <= Text.size(); ++Line) {
		const size_t End = std::min(Text.find('\n', Start), Text.size());
		std::string_view Row = Text.substr(Start, End - Start);
		Start = End + 1;
		while(!Row.empty() && (Row.back() == ' ' || Row.back() == '\t')) Row.remove_suffix(1);
		if(!Row.empty() || KeptBlanks[Line]) Lines.emplace_back(Row);
	}
	// 置く論理行の返戻
	return Lines;
}

/**
 * 通常コメントへ変換した文書の取得関数（整形仕様 7.6, 8.1）
 * 期待コメントの文書と認識した独立行の記録の内，確定した配置で説明対象（前置の属性を含む）が行の途中から始まる物を，配置の段が同じ位置で並べる記録の元の順に並べ，置く論理行と，塊の後の最初のコード行として認める断片を添える
 * 認める断片は，説明対象の行（置けない説明対象は前置の属性と同じ畳んだ行に在る）と，説明対象を含み行の最初の内容から始まる最も近い構文の行と，説明対象の行が閉じ括弧で始まる時の次のコード行（無ければ塊の後にコードが無い事を表す最大値）及び其の括弧で終わる構文の行とする
 * 計算量：節点数 V，記録の数 C，本文の長さの和 N，対象の深さの和 H に対し O(V + C log C + N + H)
 * @param Code `CodeLayout` が確定した変更不能なコードの出力
 * @param Outputs 期待コメントの記録毎の出力記法の記録
 * @param Structure 出力記法の記録の本文構造
 * @param Recognized 記録毎の文書の説明対象 (`DocCommentSynchronizer::DocumentTargets`)
 * @param Placed 記録毎の文書として置く説明対象 (`DocCommentSynchronizer::PlacedTargets`)
 * @return 原文順の変換した文書
 */
std::vector<SyntaxValidator::ConvertedDocument> SyntaxValidator::ConvertedDocuments(
	const LaidOutCode &Code,
	const std::span<const CommentRecord> Outputs,
	const CommentStructure &Structure,
	const std::span<const Core::NodeId> Recognized,
	const std::span<const Core::NodeId> Placed
) {
	// 配置資料と変換先の準備
	const AnchorPositionMap &Anchors = Code.GetAnchors();
	const Core::SyntaxModel &Syntax = *Anchors.Syntax;
	const std::span<const CodeFragment> Fragments = Code.GetFragments();
	std::vector<ConvertedDocument> Converted;
	// 変換した文書毎の記録の元の順と，閉じ括弧で始まる説明対象の行の其の括弧の終わりの位置毎の変換した文書
	std::vector<size_t> Originals;
	std::unordered_map<uint32_t, std::vector<size_t>> Closers;
	// 文書として置けない記録の移動先の収集
	for(size_t Index = 0; Index < Outputs.size(); ++Index) {
		if(Recognized[Index] == Core::NoNode || Placed[Index] != Core::NoNode) continue;
		const Core::NodeId Target = Recognized[Index];
		const uint32_t Line = Anchors.Nodes[Target].StartFragment;
		ConvertedDocument Document { PlacedLines(Outputs[Index].Text, Structure.GetKeptBlanks(Index)), { Line } };
		for(Core::NodeId Up = Syntax.ParentOf(Target); Up != Core::NoNode; Up = Syntax.ParentOf(Up)) {
			if(!Anchors.Nodes[Up].IsPlaced || !Core::DocCommentSynchronizer::StartsLine(Code, Up)) continue;
			Document.Destinations.push_back(Anchors.Nodes[Up].StartFragment);
			break;
		}
		// 説明対象の行が閉じ括弧で始まるか（配置の段と同じ字句片の形）
		const uint32_t Content = Fragments[Line].Start + static_cast<uint32_t>(Anchors.FragmentTabs[Line]);
		if(const CodePiece *const Piece = Code.PieceAt(Content); Piece && Piece->Shapes & CodePiece::StartsClosingBracket) {
			const uint32_t Next = Code.NextCodeFragment(Line);
			Document.Destinations.push_back(Next < Fragments.size() ? Next : std::numeric_limits<uint32_t>::max());
			Closers[Content + 1].push_back(Converted.size());
		}
		Converted.push_back(std::move(Document));
		Originals.push_back(Outputs[Index].OriginalOrder);
	}
	// 閉じ括弧で終わる構文の最も前の始まりの断片（其の括弧を開いた構文の行）
	if(!Closers.empty()) {
		std::unordered_map<uint32_t, uint32_t> Openings;
		for(const NodeSpan &Span : Anchors.Nodes) {
			if(!Span.IsPlaced || !Closers.contains(Span.End)) continue;
			const auto [Found, IsNew] = Openings.try_emplace(Span.End, Span.StartFragment);
			if(!IsNew) Found->second = std::min(Found->second, Span.StartFragment);
		}
		for(const auto &[End, Indices] : Closers) {
			if(const std::unordered_map<uint32_t, uint32_t>::const_iterator Found = Openings.find(End); Found != Openings.end()) {
				for(const size_t Index : Indices) Converted[Index].Destinations.push_back(Found->second);
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
	std::vector<ConvertedDocument> Sorted;
	Sorted.reserve(Converted.size());
	for(const size_t Index : Order) Sorted.push_back(std::move(Converted[Index]));
	// 原文順の変換した文書の返戻
	return Sorted;
}

/**
 * 独立行の通常コメントの続いた記録毎の本文の論理行の取得関数（整形仕様 8.1）
 * 文書と認識しない独立行の説明のコメントの置く論理行を，同じ群で続く記録毎に繋ぐ（変換した文書と同じ本文を複製と数えない為に使う）
 * 計算量：本文の長さの和 N に対し O(N)
 * @param Outputs 期待コメントの記録毎の出力記法の記録
 * @param Structure 出力記法の記録の本文構造
 * @param Recognized 記録毎の文書の説明対象
 * @return 続いた記録毎の本文の論理行
 */
std::vector<std::vector<std::string>> SyntaxValidator::OrdinaryBodies(
	const std::span<const CommentRecord> Outputs,
	const CommentStructure &Structure,
	const std::span<const Core::NodeId> Recognized
) {
	std::vector<std::vector<std::string>> Bodies;
	// 独立行の説明の通常コメントか
	const auto IsOrdinary = [&Outputs, &Recognized](const size_t Index) -> bool {
		// 文書と認識しない独立行の説明のコメントかの返戻
		return Recognized[Index] == Core::NoNode && Outputs[Index].Kind == CommentKind::OwnLine &&
		Outputs[Index].Role == CommentRole::Presentation;
	};
	for(size_t Index = 0; Index < Outputs.size(); ++Index) {
		if(!IsOrdinary(Index)) continue;
		std::vector<std::string> Lines = PlacedLines(Outputs[Index].Text, Structure.GetKeptBlanks(Index));
		// 同じ群で直前の記録に続く物は其の本文へ繋ぐ
		if(
			Index && IsOrdinary(Index - 1) && Outputs[Index].Format.Group != Core::NoCommentGroup &&
			Outputs[Index].Format.Group == Outputs[Index - 1].Format.Group
		) Bodies.back().insert(Bodies.back().end(), std::make_move_iterator(Lines.begin()), std::make_move_iterator(Lines.end()));
		else Bodies.push_back(std::move(Lines));
	}
	// 続いた記録毎の本文の論理行の返戻
	return Bodies;
}

/**
 * 最初の壊れたノードの位置の取得関数（部分整形の警告を誤りの在る行へ出す）
 * @param Src 走査対象のソース
 * @return 先行順で最初の壊れたノード (`ERROR` / `MISSING`) の開始位置（無ければ先頭）
 */
TSPoint SyntaxValidator::FirstBrokenPoint(const ParsedTree &Src) {
	TSPoint Point { 0, 0 };
	// 構文木を取得済の場合
	if(Src.IsParsed()) {
		HasDescendantWithAncestorsOf(
			Src.GetRoot(),
			[&Src, &Point](const TSNode Cur, const std::span<const TSNode> Ancestors) -> bool {
				// 壊れて居ない節点を読み進める事の返戻
				if(!SyntaxParser::IsBrokenNode(Src, Cur, Ancestors)) return false;
				// 最初の破損位置の記録
				Point = ts_node_start_point(Cur);
				// 最初の壊れたノードで打ち切る事の返戻
				return true;
			}
		);
	}
	// 位置の返戻
	return Point;
}

/**
 * コメントの中の波括弧の数の取得関数
 * 処理系の字句の規則で字面を数える（解析器が読めなかった範囲でも同じ規則で数える為）
 * 引用符の文字列と引用符の無い値の関数の引数はコメントを始めず，行コメントは其れを持つ文法だけで数える
 * @param Rules 文法の記法の定義（使用可能な記法）
 * @param Text 数える字面
 * @param UrlHead 引用符の無い引数を取る値の関数の開始記法
 * @return コメントの中の波括弧の数
 */
static size_t CountBracesInComments(
	const Core::NotationRules &Rules,
	const std::string_view Text,
	const std::string_view UrlHead
) {
	const std::string_view LineOpen = Core::CommentRepository::OpenerOf(CommentNotation::SlashLine);
	const std::string_view BlockOpen = Core::CommentRepository::OpenerOf(CommentNotation::Block);
	const std::string_view BlockClose = Core::CommentRepository::CloserOf(CommentNotation::Block);
	size_t Count = 0;
	// 字面の走査（行コメントは行末迄，囲みコメントは閉じ迄を中身とする）
	for(size_t Pos = 0; Pos < Text.size();) {
		const char Char = Text[Pos];
		const std::string_view Rest = Text.substr(Pos);
		// 引用符の文字列の読飛（逆斜線の逃避を含む）
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
		if(Rules.IsAvailable(CommentNotation::SlashLine) && Rest.starts_with(LineOpen)) {
			End = std::min(Text.find('\n', Pos), Text.size());
		} else if(Rules.IsAvailable(CommentNotation::Block) && Rest.starts_with(BlockOpen)) {
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
 * コメントの中の波括弧の数の保持の判定関数
 * tree-sitter-scss は宣言の値の中の `//` をコメントと読まず，続く行の選択子を同じ節点へ取り込む事が有る
 * 其の儘整形すると選択子と其の `{` がコメントの本文へ吸われ，様式が壊れる（処理系が `unmatched "}"` で拒む）
 * @param Rules 文法の記法の定義（使用可能な記法）
 * @param Source 整形前の入力
 * @param Output 整形結果
 * @param UrlHead 引用符の無い引数を取る値の関数の開始記法
 * @return コメントの中の波括弧が増えて居なければ true
 */
bool SyntaxValidator::KeepsBracesOutsideComments(
	const Core::NotationRules &Rules,
	const std::string_view Source,
	const std::string_view Output,
	const std::string_view UrlHead
) {
	// コメントの中の波括弧が増えて居ない事の返戻（増えたのはコードがコメントへ吸われた跡）
	return CountBracesInComments(Rules, Output, UrlHead) <= CountBracesInComments(Rules, Source, UrlHead);
}

/**
 * 検証の診断に使う節点名の取得関数
 * @param Syntax 対象の構文
 * @param Node 節点
 * @param Symbols 投影元の記号の並び
 * @return 元の節点名（生成節点は共通の役割の集合）
 */
std::string SyntaxValidator::KindComparison::Name(
	const Core::SyntaxModel &Syntax,
	const Core::NodeId Node,
	const std::span<const TSSymbol> Symbols
) const {
	const Core::KindDefinition *const Definition = Syntax.GetNode(Node).Kind.Definition;
	// 元の節点名の返戻
	if(
		Node < Symbols.size() && Symbols[Node] != std::numeric_limits<TSSymbol>::max() && &Profile.KindOf(Symbols[Node]) == Definition
	) return std::string(Profile.SymbolName(Symbols[Node]));
	// 共通の役割の集合の返戻
	return Definition ? Definition->Roles.to_string() : std::string();
}

/**
 * 共通の種別と入力側の記号対応の照合関数
 * @param Expected 期待構文
 * @param Actual 完成候補の構文
 * @param Before 期待する節点
 * @param After 完成候補の節点
 * @return 共通種別と，書換を受けて居ない節点の元の節点名が等しければ true
 */
bool SyntaxValidator::KindComparison::Same(
	const Core::SyntaxModel &Expected,
	const Core::SyntaxModel &Actual,
	const Core::NodeId Before,
	const Core::NodeId After
) const {
	const Core::KindDefinition *const Left = Expected.GetNode(Before).Kind.Definition;
	const Core::KindDefinition *const Right = Actual.GetNode(After).Kind.Definition;
	// 共通の種別が違う場合の拒否の返戻
	if(Left != Right) return false;
	// 原文の記号を持たない生成節点を共通種別で照合した結果の返戻
	if(Before >= ExpectedSymbols.size() || After >= ActualSymbols.size()) return true;
	const TSSymbol Original = ExpectedSymbols[Before], Output = ActualSymbols[After];
	// 原文の記号を持たない節点を共通種別で照合した結果の返戻
	if(Original == std::numeric_limits<TSSymbol>::max() || Output == std::numeric_limits<TSSymbol>::max()) return true;
	// 種別を書き換えた節点を共通種別で照合した結果の返戻
	if(&Profile.KindOf(Original) != Left || &Profile.KindOf(Output) != Right) return true;
	// 記号を共有した種別の内部でも元の節点名の違いを見逃さない照合の返戻
	return Profile.IsSameName(Original, Output);
}

/**
 * 独立したコードとして整形した埋込の中身の照合関数（整形仕様 4.4, 8.1）
 * 完成候補の中身が，所有区画の各行（空行と逐語の中から始まる行を除く）へ同じ深さの字下げだけを加えた字面かを確かめる
 * 中身其の物は埋込の言語の整形が独立した検証を済ませて居る為，埋込の境界で変わり得る字下げ以外の差を許さない
 * 計算量：中身の長さ N と逐語の中から始まる行の数 V に対し O(N log V)
 * @param Actual 完成候補の埋込の中身の字面（開始タグの後から終了タグの前迄）
 * @param Region 期待する所有区画
 * @return 字下げだけの違いなら true
 */
bool SyntaxValidator::MatchesEmbeddedContent(const std::string_view Actual, const Core::OwnedRegion &Region) {
	const std::string_view Text = Region.Text;
	// 空の所有区画はタグを同行へ置き，空の中身の儘保つ事の返戻
	if(Text.empty()) return Actual.empty();
	std::string_view Rest = Actual;
	// 開始タグの後の改行の無い中身の返戻
	if(!Rest.starts_with('\n')) return false;
	Rest.remove_prefix(1);
	// 最初の行の字下げを埋込の深さとする
	const size_t Depth = std::min(Rest.find_first_not_of('\t'), Rest.size());
	uint32_t Line = 0;
	for(size_t Cursor = 0; Cursor <= Text.size(); ++Line) {
		const size_t End = std::min(Text.find('\n', Cursor), Text.size());
		const std::string_view Row = Text.substr(Cursor, End - Cursor);
		// 空行と逐語の中から始まる行は字下げを持たない
		if(!Row.empty() && !std::ranges::binary_search(Region.VerbatimLines, Line)) {
			// 埋込の深さの字下げの無い行の返戻
			if(Rest.find_first_not_of('\t') < Depth) return false;
			Rest.remove_prefix(Depth);
		}
		// 行の字面と行の終わりの改行の無い中身の返戻
		if(!Rest.starts_with(Row) || Rest.size() == Row.size() || Rest[Row.size()] != '\n') return false;
		Rest.remove_prefix(Row.size() + 1);
		Cursor = End + 1;
	}
	// 終了タグの前の字下げだけが残るかの返戻
	return Rest.find_first_not_of('\t') == std::string_view::npos;
}

/**
 * 対応する節点の子の境界を比較する関数
 * @param Expected 書換後の期待構文
 * @param Actual 完成候補を再投影した構文
 * @param Kinds 診断用の記号対応
 * @param Before 期待する親
 * @param BeforeIndex 期待する子の前の位置
 * @param After 完成候補の親
 * @param AfterIndex 完成候補の子の前の位置
 * @param IsInMacro マクロの字句列の中か
 * @param ShouldDiagnose 不一致を診断するか
 * @return 保持境界と区切記法が一致すれば true
 */
bool SyntaxValidator::KeepsPreservedBoundary(
	const Core::SyntaxModel &Expected,
	const Core::SyntaxModel &Actual,
	const KindComparison &Kinds,
	const Core::NodeId Before,
	const size_t BeforeIndex,
	const Core::NodeId After,
	const size_t AfterIndex,
	const bool IsInMacro,
	const bool ShouldDiagnose
) {
	// 期待と完成候補の境界の分類
	const Core::SemanticBoundary *const Original =
	Expected.GetNode(Before).Facts.BoundaryBefore(static_cast<uint32_t>(BeforeIndex));
	const Core::SemanticBoundary *const Output = Actual.GetNode(After).Facts.BoundaryBefore(static_cast<uint32_t>(AfterIndex));
	const bool WasPreserved = Original && Original->Kind == Core::BoundaryKind::Preserved;
	const bool IsPreserved = Output && Output->Kind == Core::BoundaryKind::Preserved;
	const bool WasSeparated = Original && Original->Kind == Core::BoundaryKind::Separated;
	const bool IsSeparated = Output && Output->Kind == Core::BoundaryKind::Separated;
	// 通常の空白量は整形で変わるが，値を持つ文字列と Rust マクロの間隙は完全に保持する
	const auto SameSignificant = [](const std::string_view Left, const std::string_view Right) -> bool {
		size_t A = 0, B = 0;
		while(true) {
			while(A < Left.size() && std::isspace(static_cast<unsigned char>(Left[A]))) ++A;
			while(B < Right.size() && std::isspace(static_cast<unsigned char>(Right[B]))) ++B;
			// 双方の字面が尽きた時だけ一致する事の返戻
			if(A == Left.size() || B == Right.size()) return A == Left.size() && B == Right.size();
			// 空白以外の字が異なる場合の返戻
			if(Left[A++] != Right[B++]) return false;
		}
	};
	// 保持する間隙の字面と境界種別の照合
	const bool IsExact = WasPreserved && (
		IsInMacro || Original->Source == Core::PreservedSource::MacroTokens || Original->Source == Core::PreservedSource::LiteralContent
	);
	const bool SamePreservation = !WasPreserved || IsPreserved && Original->Source == Output->Source &&
	(IsExact ? Original->Gap == Output->Gap : SameSignificant(Original->Gap, Output->Gap));
	const bool Same = WasPreserved == IsPreserved && WasSeparated == IsSeparated && SamePreservation;
	// 不一致時の境界の診断
	if(ShouldDiagnose && !Same) {
		std::fprintf(
			stderr,
			"STRUCT-BOUNDARY %s %zu preserved=%d source=%d bytes=%zu | %s %zu preserved=%d source=%d bytes=%zu\n",
			Kinds.Name(Expected, Before, Kinds.ExpectedSymbols).c_str(),
			BeforeIndex,
			WasPreserved,
			WasPreserved ? static_cast<int>(Original->Source) : -1,
			WasPreserved ? Original->Gap.size() : 0,
			Kinds.Name(Actual, After, Kinds.ActualSymbols).c_str(),
			AfterIndex,
			IsPreserved,
			IsPreserved ? static_cast<int>(Output->Source) : -1,
			IsPreserved ? Output->Gap.size() : 0
		);
	}
	// 保持する間隙の種類と意味を持つ字面の一致の返戻
	return Same;
}

/**
 * 省略可能型を構成する基底の型と `?` の数の取得関数
 * tree-sitter は連続する `?` を一つの `??` 字句又は入れ子の optional_type として返す為，字句毎の長さを数える
 * @param Syntax 投影した構文
 * @param Node 省略可能型
 * @param Base 基底の型の格納先
 * @param Marks `?` の数の格納先
 * @return 全ての後置字句が `?` だけなら true
 */
bool SyntaxValidator::OptionalTypeParts(const Core::SyntaxModel &Syntax, Core::NodeId Node, Core::NodeId &Base, size_t &Marks) {
	Marks = 0;
	const std::string_view MarkerSpelling = Syntax.Rules().Spelling(Core::Word::OptionalityMarker);
	// 後置記号を１字で表さない記法を照合しない事の返戻
	if(MarkerSpelling.size() != 1) return false;
	while(Syntax.Is(Node, Core::Role::OptionalType)) {
		const std::span<const Core::NodeId> Children = Syntax.ChildrenOf(Node);
		// 型本体と後置記号に分けれない節点の拒否の返戻
		if(Children.size() < 2 || !Syntax.IsNamed(Children.front())) return false;
		for(size_t Index = 1; Index < Children.size(); ++Index) {
			const Core::NodeId Marker = Children[Index];
			const std::string_view Text = Syntax.GetNode(Marker).Lexeme;
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
 * 宣言子の部品の構造と字句の一致の判定関数
 * @param Expected 正規化後の期待構文
 * @param Actual 完成候補を再投影した構文
 * @param Kinds 共通種別と投影元の記号の対応
 * @param Before 期待する部品
 * @param After 完成候補の部品
 * @param RowCommas 此の出力宣言の共有部分へ選択した末尾カンマ（通常は無し）
 * @param AcceptContextualType 文脈に依る同値な型表現を許すか
 * @param UsedCommas 実際に照合した末尾カンマの記録先
 * @return 種別・場・子の順・字句が一致すれば true
 */
bool SyntaxValidator::MatchesDeclarationPiece(
	const Core::SyntaxModel &Expected,
	const Core::SyntaxModel &Actual,
	const KindComparison &Kinds,
	const Core::NodeId Before,
	const Core::NodeId After,
	const std::unordered_set<Core::NodeId> *const RowCommas,
	const bool AcceptContextualType,
	std::unordered_set<Core::NodeId> *const UsedCommas
) {
	// 宣言部品の対応節点の反復照合
	struct PieceMatch {
		Core::NodeId Before;
		Core::NodeId After;
		bool IsInMacro;
	};
	std::vector<PieceMatch> Stack { { Before, After, false } };
	while(!Stack.empty()) {
		const auto [Left, Right, WasInMacro] = Stack.back();
		Stack.pop_back();
		if(AcceptContextualType && MatchesContextualTypeRepresentation(Expected, Actual, Kinds, Left, Right, RowCommas, UsedCommas)) {
			continue;
		}
		const bool IsInMacro = WasInMacro || Expected.Is(Left, Core::Role::PreservedTokenSequence);
		const std::span<const Core::NodeId> Origin = Expected.ChildrenOf(Left), Result = Actual.ChildrenOf(Right);
		const bool HasSelectedComma = RowCommas && RowCommas->contains(Left);
		const bool HasInsertedComma = HasSelectedComma && Origin.size() > 1 && Result.size() == Origin.size() + 1 &&
		Actual.Is(Result[Result.size() - 2], Core::Role::CommaToken) && Kinds.Same(Expected, Actual, Origin.back(), Result.back());
		const bool IsChanged =
		!Kinds.Same(Expected, Actual, Left, Right) || Expected.GetNode(Left).Fields != Actual.GetNode(Right).Fields ||
		(HasSelectedComma ? !HasInsertedComma : Origin.size() != Result.size());
		// 部品の種別・役割・子の数が変わった時の拒否の返戻
		if(IsChanged) return false;
		if(HasInsertedComma && UsedCommas) UsedCommas->insert(Left);
		// 葉の字句が変わった時の拒否の返戻
		if(Origin.empty() && Expected.GetNode(Left).Lexeme != Actual.GetNode(Right).Lexeme) return false;
		// 分割した宣言子の中でも保持する字面が変わった時の拒否の返戻
		for(size_t Index = 0; Index <= Origin.size(); ++Index) if(
			!KeepsPreservedBoundary(
				Expected,
				Actual,
				Kinds,
				Left,
				Index,
				Right,
				Index == Origin.size() ? Result.size() : Index,
				IsInMacro,
				false
			)
		) return false;
		// 子部品の対応の次の照合への登録
		for(size_t Index = Origin.size(); Index--;) {
			Stack.push_back({ Origin[Index], Result[Index + (HasInsertedComma && Index == Origin.size() - 1)], IsInMacro });
		}
	}
	// 部品の全節点が一致した事の返戻
	return true;
}

/**
 * 同じ型を解析器が異なる節点へ写した場合の対応の判定関数
 * @param Expected 正規化後の期待構文
 * @param Actual 完成候補を再投影した構文
 * @param Kinds 共通種別と投影元の記号の対応
 * @param Before 期待する型
 * @param After 完成候補の型
 * @param RowCommas 此の出力宣言で選択した末尾カンマ
 * @param UsedCommas 実際に照合した末尾カンマの記録先
 * @return 省略可能性記号又はメタ型の基底と後置記号が同じなら true
 */
bool SyntaxValidator::MatchesContextualTypeRepresentation(
	const Core::SyntaxModel &Expected,
	const Core::SyntaxModel &Actual,
	const KindComparison &Kinds,
	const Core::NodeId Before,
	const Core::NodeId After,
	const std::unordered_set<Core::NodeId> *const RowCommas,
	std::unordered_set<Core::NodeId> *const UsedCommas
) {
	// 型注釈等の場を変えた節点を同値と扱わない事の返戻
	if(Expected.GetNode(Before).Fields != Actual.GetNode(After).Fields) return false;
	if(Expected.Is(Before, Core::Role::OptionalType) && Actual.Is(After, Core::Role::OptionalType)) {
		Core::NodeId Original = Core::NoNode, Result = Core::NoNode;
		size_t OriginalMarks = 0, ResultMarks = 0;
		// 記号の束ね方に依らず基底と `?` の数を照合した結果の返戻
		return OptionalTypeParts(Expected, Before, Original, OriginalMarks) && OptionalTypeParts(Actual, After, Result, ResultMarks) &&
		OriginalMarks == ResultMarks && MatchesDeclarationPiece(Expected, Actual, Kinds, Original, Result, RowCommas, true, UsedCommas);
	}
	// メタ型と限定型以外の異種節点の拒否の返戻
	if(!Expected.Is(Before, Core::Role::MetatypeNode) || !Actual.Is(After, Core::Role::UserType)) return false;
	const std::span<const Core::NodeId> Outer = Expected.ChildrenOf(Before), Qualified = Actual.ChildrenOf(After);
	const bool IsSameSuffix =
	Outer.size() == 3 && Expected.Is(Outer[0], Core::Role::UserType) && Expected.Is(Outer[1], Core::Role::DotToken) &&
	Expected.IsWord(Outer[2], Core::Word::MetatypeSuffix) && Qualified.size() == Expected.ChildCount(Outer[0]) + 2 &&
	Actual.Is(Qualified[Qualified.size() - 2], Core::Role::DotToken) && Actual.Is(Qualified.back(), Core::Role::TypeName) &&
	Actual.IsWord(Qualified.back(), Core::Word::MetatypeSuffix);
	// `.Protocol` 等の異なる後置型を混同しない拒否の返戻
	if(!IsSameSuffix) return false;
	const std::span<const Core::NodeId> Base = Expected.ChildrenOf(Outer[0]);
	// 限定型の基底を構成する節点の種別・場・字句が一致しない場合の拒否の返戻
	for(size_t Index = 0; Index < Base.size(); ++Index) {
		// 限定型の基底が一致しない場合の拒否の返戻
		if(!MatchesDeclarationPiece(Expected, Actual, Kinds, Base[Index], Qualified[Index], RowCommas, true, UsedCommas)) return false;
	}
	// 型の基底と `.Type` の後置記号が一致した事の返戻
	return true;
}

/**
 * 選択した宣言区分と完成候補の宣言子の対応の判定関数
 * 共有指定と宣言子毎の子の役割・順序・字句を，区分毎に照合する
 * @param Expected 正規化後の期待構文
 * @param Actual 完成候補を再投影した構文
 * @param Kinds 共通種別と投影元の記号の対応
 * @param Node 統合宣言
 * @param Rows 当該宣言から出力した宣言の並び
 * @param Partition 行分割が選んだ区分
 * @param RowCommas 出力宣言毎に共有部分へ選択した末尾カンマ
 * @param AcceptContextualType 文脈に依る同値な型表現を許すか
 * @return 宣言子の対応が一致すれば true
 */
bool SyntaxValidator::MatchesDeclarationPartition(
	const Core::SyntaxModel &Expected,
	const Core::SyntaxModel &Actual,
	const KindComparison &Kinds,
	const Core::NodeId Node,
	const std::span<const Core::NodeId> Rows,
	const Core::DeclarationPartition &Partition,
	const std::vector<std::unordered_set<Core::NodeId>> &RowCommas,
	const bool AcceptContextualType
) {
	// 分割元の宣言子と共有指定の取得
	const std::optional<Core::CommaDecl> Source = Expected.CommaDeclOf(Node);
	// 宣言子の無い区分の拒否の返戻
	if(!Source || Source->Lists.empty() || Partition.Rows.size() != Rows.size() || RowCommas.size() != Rows.size()) return false;
	const size_t Count = Source->Lists.front().Items.size();
	std::vector<Core::NodeId> SharedBefore;
	for(const Core::DeclPiece &Piece : Source->Pieces) if(Piece.List < 0) SharedBefore.push_back(Piece.Node);
	// 出力行毎の共有指定と宣言子の照合
	size_t End = 0;
	for(size_t Index = 0; Index < Rows.size(); ++Index) {
		std::unordered_set<Core::NodeId> UsedCommas;
		const Core::DeclaratorRange Range = Partition.Rows[Index];
		// 宣言子を飛ばす又は重ねる区分の拒否の返戻
		if(Range.Begin != End || Range.Begin >= Range.End || Range.End > Count) return false;
		const Core::NodeId Row = Rows[Index];
		const bool IsSameRole = Kinds.Same(Expected, Actual, Node, Row) && Expected.GetNode(Node).Fields == Actual.GetNode(Row).Fields;
		// 出力宣言の種別又は親の中の場が変わった区分の拒否の返戻
		if(!IsSameRole) return false;
		const std::optional<Core::CommaDecl> Output = Actual.CommaDeclOf(Row);
		// 共有指定と並びの数が変わった区分の拒否の返戻
		if(!Output || Source->Lists.size() != Output->Lists.size()) return false;
		std::vector<Core::NodeId> SharedAfter;
		for(const Core::DeclPiece &Piece : Output->Pieces) if(Piece.List < 0) SharedAfter.push_back(Piece.Node);
		// 共有する型・指定・終端の構造が変わった区分の拒否の返戻
		if(SharedBefore.size() != SharedAfter.size()) return false;
		for(size_t Shared = 0; Shared < SharedBefore.size(); ++Shared) {
			const bool IsMatchingShared = MatchesDeclarationPiece(
				Expected,
				Actual,
				Kinds,
				SharedBefore[Shared],
				SharedAfter[Shared],
				&RowCommas[Index],
				AcceptContextualType,
				&UsedCommas
			);
			// 宣言子間で繰り返した共有指定の子の相違の返戻
			if(!IsMatchingShared) return false;
		}
		// 選択した末尾カンマを出力で確認出来なかった場合の返戻
		if(UsedCommas.size() != RowCommas[Index].size()) return false;
		for(size_t List = 0; List < Source->Lists.size(); ++List) {
			const std::vector<std::vector<Core::NodeId>> &Before = Source->Lists[List].Items, &After = Output->Lists[List].Items;
			// 宣言子の数が選択結果と異なる区分の拒否の返戻
			if(Before.size() != Count || After.size() != Range.End - Range.Begin) return false;
			for(size_t Item = 0; Item < After.size(); ++Item) {
				const std::vector<Core::NodeId> &Origin = Before[Range.Begin + Item], &Result = After[Item];
				// 宣言子の構成が変わった区分の拒否の返戻
				if(Origin.size() != Result.size()) return false;
				for(size_t Piece = 0; Piece < Result.size(); ++Piece) {
					// 子の役割・順序・字句が変わった区分の拒否の返戻
					if(!MatchesDeclarationPiece(Expected, Actual, Kinds, Origin[Piece], Result[Piece], nullptr, AcceptContextualType, nullptr)) {
						// 宣言の構成が変わった場合の拒否の返戻
						return false;
					}
				}
			}
		}
		End = Range.End;
	}
	// 全宣言子を一度ずつ出力したかの返戻
	return End == Count;
}

/**
 * 期待構文と完成候補を再投影した構文の一致の判定関数
 * @param Actual 完成候補を入力と同じ補修の後に投影した構文
 * @param Language 入力時に確定した言語
 * @param Expectation 正規化後の期待構文と選択結果
 * @param Kinds 共通種別と投影元の記号の対応
 * @param ShouldDiagnose 差の位置を診断へ出すか
 * @return 対応表に従って構造が一致するなら true
 * @throws FormatLimitExceeded 選択した生成形式が期待構文に無い場合
 */
bool SyntaxValidator::KeepsStructure(
	const Core::SyntaxModel &Actual,
	const Lang Language,
	const Core::OutputExpectation &Expectation,
	const KindComparison &Kinds,
	const bool ShouldDiagnose
) {
	// 選択した表現の書換を受けた期待構文（書換の無い候補は共有する期待構文と其の儘比べ，全体を複写しない）
	std::optional<Core::SyntaxModel> Rewritten;
	const auto ExpectedView = [&Rewritten, &Expectation]() -> const Core::SyntaxModel & {
		// 書換えた写し又は共有する期待構文の返戻
		return Rewritten ? *Rewritten : *Expectation.NormalizedSyntax;
	};
	const auto Rewrite = [&Rewritten, &Expectation](const Core::RewritePlan &Plan) -> void {
		// 書換対象の生成と選択した表現の反映
		if(!Rewritten) Rewritten.emplace(*Expectation.NormalizedSyntax);
		Rewritten->Apply(Plan);
	};
	// 宣言区分と末尾カンマの対応準備
	std::unordered_set<Core::NodeId> CommaOwners;
	std::unordered_map<Core::NodeId, std::vector<std::unordered_set<Core::NodeId>>> RowCommas;
	for(const Core::DeclarationPartition &Partition : Expectation.DeclarationPartitions) {
		if(!RowCommas.emplace(Partition.Node, Partition.Rows.size()).second) {
			throw FormatLimitExceeded("a declaration has multiple partitions", FailureReason::FormatterDefect);
		}
	}
	// 選択した生成形式の期待構文への反映
	for(const Core::SelectedForm &Selected : Expectation.SelectedForms) {
		if(!Selected.IsBroken) continue;
		for(const Core::SelectedFormEffect &Effect : Selected.Effects) {
			const Core::SyntaxModel &Expected = ExpectedView();
			if(Effect.Operation == Core::FormOperation::TrailingComma) {
				const Core::NodeId Owner = Effect.Node;
				const std::span<const Core::NodeId> Children = Expected.ChildrenOf(Owner);
				const Core::CanonicalForm *const Form = Expected.FormOf(Owner, Core::FormOperation::TrailingComma);
				const bool IsValid = Form && Children.size() > 1 && !Expected.IsNamed(Children.back()) && Form->CanAddTrailingComma(
					Selected.IsBroken,
					Expected.IsNamed(Children[Children.size() - 2]),
					Expected.FirstChar(Children.back())
				);
				if(!IsValid) throw FormatLimitExceeded("a selected trailing comma has no syntax position", FailureReason::FormatterDefect);
				if(Effect.DeclarationOwner != Core::NoNode) {
					const std::unordered_map<Core::NodeId, std::vector<std::unordered_set<Core::NodeId>>>::iterator Found =
					RowCommas.find(Effect.DeclarationOwner);
					if(
						Found == RowCommas.end() || Effect.DeclarationRow >= Found->second.size() ||
						!Expected.Contains(Effect.DeclarationOwner, Owner) || !Found->second[Effect.DeclarationRow].insert(Owner).second
					) throw FormatLimitExceeded("a selected declaration comma has no output row", FailureReason::FormatterDefect);
					continue;
				}
				if(!CommaOwners.insert(Owner).second) {
					throw FormatLimitExceeded("a selected trailing comma has no syntax position", FailureReason::FormatterDefect);
				}
				Core::RewriteStep Step { Core::RewriteKind::Insert, Owner };
				Step.Index = static_cast<uint32_t>(Children.size() - 1);
				Step.Inserted = { std::string(Form->FixedLexeme) };
				Core::RewritePlan Plan;
				Plan.Steps.push_back(std::move(Step));
				Rewrite(Plan);
				continue;
			}
			if(Effect.Operation == Core::FormOperation::Continuation) {
				Core::RewritePlan Plan;
				Plan.Steps.push_back(Core::SyntaxNormalizer::ContinuationStep(Expected, Effect.Node));
				Rewrite(Plan);
				continue;
			}
			const Core::CanonicalForm *const Form = Expected.FormOf(Expected.ParentOf(Effect.Node), Effect.Operation);
			if(!Form) throw FormatLimitExceeded("a selected form has no syntax definition", FailureReason::FormatterDefect);
			Core::RewritePlan Plan;
			if(Effect.Operation == Core::FormOperation::BraceBody) {
				Plan.Steps.push_back(Core::SyntaxNormalizer::BraceBodyStep(Expected, Effect.Node, *Form));
			}
			if(!Plan.Steps.empty()) Rewrite(Plan);
		}
	}
	// 期待構文の宣言区分と診断表示の準備
	const Core::SyntaxModel &Expected = ExpectedView();
	std::unordered_map<Core::NodeId, const Core::DeclarationPartition *> Partitions;
	for(const Core::DeclarationPartition &Partition : Expectation.DeclarationPartitions) {
		Partitions.emplace(Partition.Node, &Partition);
	}
	const auto Describe =
	[&Kinds](const Core::SyntaxModel &Syntax, const Core::NodeId Node, const std::span<const TSSymbol> Symbols) -> std::string {
		std::string Description = Kinds.Name(Syntax, Node, Symbols);
		Description += "(";
		const std::span<const Core::NodeId> Children = Syntax.ChildrenOf(Node);
		const size_t Shown = std::min(Children.size(), size_t{ 8 });
		for(size_t Index = 0; Index < Shown; ++Index) {
			const Core::NodeId Child = Children[Index];
			Description += Syntax.IsNamed(Child) ? Kinds.Name(Syntax, Child, Symbols) : "'" + Kinds.Name(Syntax, Child, Symbols) + "'";
			Description += " ";
		}
		if(Shown < Children.size()) Description += "... +" + std::to_string(Children.size() - Shown);
		Description += ")";
		// 比較用の節点の記述の返戻
		return Description;
	};
	// 期待構文と完成候補の節点毎の照合
	struct NodeMatch {
		Core::NodeId Before;
		Core::NodeId After;
		bool IsInMacro;
	};
	std::vector<NodeMatch> Stack { { Expected.GetRoot(), Actual.GetRoot(), false } };
	while(!Stack.empty()) {
		const NodeMatch Frame = Stack.back();
		Stack.pop_back();
		const Core::NodeId Before = Frame.Before, After = Frame.After;
		const bool IsInMacro = Frame.IsInMacro || Expected.Is(Before, Core::Role::PreservedTokenSequence);
		const std::span<const Core::NodeId> ExpectedChildren = Expected.ChildrenOf(Before), ActualChildren = Actual.ChildrenOf(After);
		// 独立したコードとして整形した埋込の中身（期待構文は所有区画を置く節点，完成候補は中身の字面の葉）
		const Core::OwnedRegion *const Region = ActualChildren.empty() ? Expected.RegionOf(Before) : nullptr;
		const bool IsEmbeddedCode = Region != nullptr;
		bool Same = Kinds.Same(Expected, Actual, Before, After) && Expected.GetNode(Before).Fields == Actual.GetNode(After).Fields && (
			IsEmbeddedCode ?
			MatchesEmbeddedContent(Actual.GetNode(After).Lexeme, *Region) :
			!ExpectedChildren.empty() || !ActualChildren.empty() || Expected.GetNode(Before).Lexeme == Actual.GetNode(After).Lexeme
		);
		std::vector<NodeMatch> Matched;
		size_t Cursor = 0;
		// 子節点と分割した宣言区分の対応照合
		for(size_t Index = 0; Index < ExpectedChildren.size() && !IsEmbeddedCode; ++Index) {
			if(!Same) break;
			Same = KeepsPreservedBoundary(Expected, Actual, Kinds, Before, Index, After, Cursor, IsInMacro, ShouldDiagnose);
			if(!Same) break;
			const Core::NodeId Child = ExpectedChildren[Index];
			const std::unordered_map<Core::NodeId, const Core::DeclarationPartition *>::const_iterator Found = Partitions.find(Child);
			const bool HasRowComma = Found != Partitions.end() && std::ranges::any_of(
				RowCommas.at(Child),
				[](const std::unordered_set<Core::NodeId> &Owners) -> bool {
					// 此の出力宣言に選択した末尾カンマが在るかの返戻
					return !Owners.empty();
				}
			);
			if(Found != Partitions.end() && (Found->second->Rows.size() > 1 || HasRowComma)) {
				const size_t Count = Found->second->Rows.size();
				Same = Cursor + Count <= ActualChildren.size() && MatchesDeclarationPartition(
					Expected,
					Actual,
					Kinds,
					Child,
					std::span<const Core::NodeId>(ActualChildren.data() + Cursor, Count),
					*Found->second,
					RowCommas.at(Child),
					Kinds.Profile.GetReading().HasContextualTypeRepresentation
				);
				Cursor += Count;
				continue;
			}
			if(Cursor == ActualChildren.size()) {
				Same = false;
				break;
			}
			const Core::NodeId Output = ActualChildren[Cursor++];
			const bool IsContextualType = Kinds.Profile.GetReading().HasContextualTypeRepresentation &&
			MatchesContextualTypeRepresentation(Expected, Actual, Kinds, Child, Output, nullptr, nullptr);
			Same = IsContextualType || Kinds.Same(Expected, Actual, Child, Output);
			if(Same && !IsContextualType) Matched.push_back({ Child, Output, IsInMacro });
		}
		Same = Same && (
			IsEmbeddedCode || Cursor == ActualChildren.size() &&
			KeepsPreservedBoundary(Expected, Actual, Kinds, Before, ExpectedChildren.size(), After, Cursor, IsInMacro, ShouldDiagnose)
		);
		if(!Same) {
			if(ShouldDiagnose) {
				std::fprintf(
					stderr,
					"STRUCT-DIFF %d\n  exp %s fields=%llu\n  act %s fields=%llu\n",
					static_cast<int>(Language.Id),
					Describe(Expected, Before, Kinds.ExpectedSymbols).c_str(),
					static_cast<unsigned long long>(Expected.GetNode(Before).Fields),
					Describe(Actual, After, Kinds.ActualSymbols).c_str(),
					static_cast<unsigned long long>(Actual.GetNode(After).Fields)
				);
				// 差の位置と重なる選択だけを出し，正当な包装・区分の候補を探せる様にする
				constexpr size_t MaxDetails = 8, MaxScannedGroups = 16;
				size_t ShownForms = 0, FormIndex = 0;
				for(; FormIndex < Expectation.SelectedForms.size() && FormIndex < MaxScannedGroups && ShownForms < MaxDetails; ++FormIndex) {
					const Core::SelectedForm &Form = Expectation.SelectedForms[FormIndex];
					const std::vector<Core::NodeId>::const_iterator Last = Form.Nodes.begin() + std::min(Form.Nodes.size(), MaxDetails);
					const std::vector<Core::NodeId>::const_iterator Found = std::find_if(
						Form.Nodes.begin(),
						Last,
						[&Expected, Before](const Core::NodeId Owner) -> bool {
							// 差の位置と重なる群の対象かの返戻
							return Expected.Contains(Owner, Before) || Expected.Contains(Before, Owner);
						}
					);
					if(Found == Last) continue;
					std::fprintf(
						stderr,
						"  form %zu %s %s (owners=%zu)\n",
						FormIndex,
						Kinds.Name(Expected, *Found, Kinds.ExpectedSymbols).c_str(),
						Form.IsBroken ? "broken" : "flat",
						Form.Nodes.size()
					);
					++ShownForms;
				}
				if(FormIndex < Expectation.SelectedForms.size()) {
					std::fprintf(stderr, "  unexamined form groups: %zu\n", Expectation.SelectedForms.size() - FormIndex);
				}
				// 差の位置と重なる宣言区分の診断
				size_t ShownPartitions = 0, PartitionIndex = 0;
				for(
					;
					PartitionIndex < Expectation.DeclarationPartitions.size() && PartitionIndex < MaxScannedGroups && ShownPartitions < MaxDetails;
					++PartitionIndex
				) {
					const Core::DeclarationPartition &Partition = Expectation.DeclarationPartitions[PartitionIndex];
					if(!Expected.Contains(Partition.Node, Before) && !Expected.Contains(Before, Partition.Node)) continue;
					std::fprintf(stderr, "  partition %s", Kinds.Name(Expected, Partition.Node, Kinds.ExpectedSymbols).c_str());
					const size_t Shown = std::min(MaxDetails, Partition.Rows.size());
					for(size_t Row = 0; Row < Shown; ++Row) {
						const Core::DeclaratorRange Range = Partition.Rows[Row];
						std::fprintf(stderr, " [%zu,%zu)", Range.Begin, Range.End);
					}
					if(Shown < Partition.Rows.size()) std::fprintf(stderr, " ... +%zu rows", Partition.Rows.size() - Shown);
					std::fputc('\n', stderr);
					++ShownPartitions;
				}
				if(PartitionIndex < Expectation.DeclarationPartitions.size()) {
					std::fprintf(stderr, "  unexamined partitions: %zu\n", Expectation.DeclarationPartitions.size() - PartitionIndex);
				}
			}
			// 差が有る候補の返戻
			return false;
		}
		for(size_t Index = Matched.size(); Index--;) Stack.push_back(Matched[Index]);
	}
	// 全節点が一致した事の返戻
	return true;
}

/**
 * 文書の説明対象と本文の保持の判定関数（整形仕様 7.4, 8.1）
 * 期待コメントの文書として置く記録の説明対象が，完成候補でも文書の説明対象と為り，其の本文の論理行を通常コメントとの記法の区別と共に欠落・改変・順序違反無く持つかを比べる
 * 完成候補の文書に加わった論理行は，文書として置かない期待コメント（同じ記法の言語で文書に読まれる通常コメント）の論理行の出現を１度ずつ充てて説明出来る物だけを認め，文書の論理行の複製を拒む
 * 内容の無い論理行（埋込文書の開始・終端記号の行の残り等）は本文を持たない為，充てる対象としない
 * 期待に無い説明対象へ通常コメントと区別する記法の文書が増えた完成候補も拒む
 * 説明対象の識別は構文上の位置で一意に定まり，出力の順は構造の照合が保つ為，同期が並べ替えた説明対象の順に依らず対象毎に比べる
 * 同じ対象の文書の記録の分け方（行形式の文書の行毎の記録等）に依らない様に，対象毎に記録の順に論理行を繋いで比べる
 * 計算量：文書の数 D，記録の数 R と識別・本文の長さの和 L に対し O(D + R + L)
 * @param Expected 期待コメントの文書の説明対象と本文
 * @param Actual 完成候補の文書の説明対象と本文
 * @param Outputs 期待コメントの出力記法の記録
 * @param Placed 記録毎の文書として置く説明対象（置かない記録は無し）
 * @return 期待する説明対象を全て文書の説明対象として保ち，対象毎の期待の論理行を同じ記法の区別で原文の順に持ち，其の他の論理行を通常コメントの論理行で説明出来れば true
 */
bool SyntaxValidator::KeepsDocuments(
	const std::span<const ValidationContext::DocumentFact> Expected,
	const std::span<const ValidationContext::DocumentFact> Actual,
	const std::span<const CommentRecord> Outputs,
	const std::span<const Core::NodeId> Placed
) {
	// 説明対象毎の文書
	struct TargetDocument {
		std::vector<std::string_view> Lines; // 記録の順に繋いだ論理行
		std::vector<uint8_t> IsDedicated; // 論理行毎の通常コメントと区別する記法の文書の物か
	};
	// 説明対象毎の文書（期待と完成候補）
	std::unordered_map<std::string_view, TargetDocument> Required, Kept;
	// 説明対象の文書への記録の論理行と記法の区別の追加関数の定義
	const auto Append = [](const ValidationContext::DocumentFact &Fact, TargetDocument &Entry) -> void {
		Entry.Lines.insert(Entry.Lines.end(), Fact.Lines.begin(), Fact.Lines.end());
		Entry.IsDedicated.insert(Entry.IsDedicated.end(), Fact.Lines.size(), Fact.IsDedicated);
	};
	for(const ValidationContext::DocumentFact &Fact : Expected) Append(Fact, Required[Fact.Target]);
	for(const ValidationContext::DocumentFact &Fact : Actual) {
		if(Required.contains(Fact.Target)) Append(Fact, Kept[Fact.Target]);
		// 期待に無い説明対象へ通常コメントと区別する記法の文書が増えた事の返戻（同じ記法の言語の通常コメントは文書に読まれ得る）
		else if(Fact.IsDedicated) return false;
	}
	// 文書として置かない期待コメントの論理行と，文書に加わった論理行へ未だ充てて居ない出現の数（文書に加わった論理行が現れた時に初めて求める）
	std::vector<std::vector<std::string>> LooseLines;
	std::unordered_map<std::string_view, size_t> Unused;
	// 文書に加わった論理行へ通常コメントの論理行の出現を充てる関数の定義
	const auto TakeLoose = [&](const std::string_view Line) -> bool {
		// 最初の呼出での通常コメントの論理行の出現の集計
		if(LooseLines.empty()) {
			LooseLines.reserve(Outputs.size());
			for(size_t Index = 0; Index < Outputs.size(); ++Index) {
				LooseLines.push_back(
					Placed[Index] == Core::NoNode ? Core::DocCommentSynchronizer::LogicalLines(Outputs[Index]) : std::vector<std::string>()
				);
			}
			for(const std::vector<std::string> &Lines : LooseLines) for(const std::string &Loose : Lines) ++Unused[Loose];
		}
		const std::unordered_map<std::string_view, size_t>::iterator Spare = Unused.find(Line);
		// 充てる出現の無い論理行の返戻
		if(Spare == Unused.end() || !Spare->second) return false;
		--Spare->second;
		// 出現を充てた事の返戻
		return true;
	};
	for(const std::pair<const std::string_view, TargetDocument> &Target : Required) {
		const std::unordered_map<std::string_view, TargetDocument>::const_iterator Found = Kept.find(Target.first);
		// 説明対象の文書を失った事の返戻
		if(Found == Kept.end()) return false;
		// 期待の論理行を完成候補の同じ記法の区別の論理行に原文の順で探し，其の他の論理行へ文書として置かない期待コメントの論理行を充てる（同じ記法の通常コメントが文書の前に並ぶ言語では，所有構文の前へ置いた通常コメントも文書の本文に読まれる）
		const TargetDocument &Wanted = Target.second, &Held = Found->second;
		size_t Next = 0;
		for(size_t Index = 0; Index < Held.Lines.size(); ++Index) {
			if(
				Next < Wanted.Lines.size() && Held.Lines[Index] == Wanted.Lines[Next] && Held.IsDedicated[Index] == Wanted.IsDedicated[Next]
			) ++Next;
			// 通常コメントで説明出来ない本文の論理行（文書の論理行の複製・改変・記法の区別の変更）の返戻
			else if(!Held.Lines[Index].empty() && !TakeLoose(Held.Lines[Index])) return false;
		}
		// 本文の論理行の欠落・順序違反の返戻
		if(Next != Wanted.Lines.size()) return false;
	}
	// 説明対象と本文を保った事の返戻
	return true;
}

/**
 * 通常コメントへ変換した文書の本文と移動先の保持の判定関数（整形仕様 7.6, 8.1）
 * 変換した文書毎に，期待コメントの置く論理行が，認める断片の前の完成候補の塊の中に文書の記法でない記録として連続して現れる事を確かめる
 * 同じコード行の前（空行で分けた塊を含む）に置く文書は原文の順に現れる事を求め，塊の後の最初のコード行毎の照合済の位置から先だけを探す
 * 完成候補の論理行は正規化し直さずに比べ，正規化で消える違反を見逃さない
 * 同じ本文の完成候補の全ての塊での出現数が，変換した文書と期待コメントの通常コメント（続いた記録毎）の本文の中の出現数の和を越える物は複製とする
 * 出現は最初の論理行と移動先毎の位置の索引を共有して確かめる
 * 計算量：塊の論理行の数の和 L，期待本文の論理行の数の和 R，許した移動先の数 A，候補訪問数 M，本文の論理行の数の最大 B に対し O(L + R + A log L + M B)
 * @param Converted 原文順の変換した文書
 * @param Ordinary 期待コメントの独立行の通常コメントの論理行（続いた記録毎）
 * @param Runs 完成候補のコメントだけの行の塊
 * @return 変換した文書の本文を欠落・複製・順序違反・改変無く認める移動先に通常コメントとして保てば true
 */
bool SyntaxValidator::KeepsConvertedDocuments(
	const std::span<const ConvertedDocument> Converted,
	const std::span<const std::vector<std::string>> Ordinary,
	const std::span<const CommentRun> Runs
) {
	// 塊の論理行の字面と移動先毎の位置（塊と塊の中の論理行の順）
	using Position = std::pair<size_t, size_t>;
	using DestinationIndex = std::unordered_map<uint32_t, std::vector<Position>>;
	std::unordered_map<std::string_view, DestinationIndex> Positions;
	for(size_t Run = 0; Run < Runs.size(); ++Run) for(size_t Line = 0; Line < Runs[Run].Lines.size(); ++Line) {
		Positions[Runs[Run].Lines[Line]][Runs[Run].Next].emplace_back(Run, Line);
	}
	// 塊の位置から続く分が文書の記法でない論理行で，本文と一致するか
	const auto MatchesAt = [&Runs](const Position At, const std::vector<std::string> &Body) -> bool {
		const auto [Run, Start] = At;
		const std::span<const std::string> Lines = Runs[Run].Lines;
		// 塊を越える本文の返戻
		if(Start + Body.size() > Lines.size()) return false;
		// 文書の記法の論理行を含む事の返戻
		for(size_t Line = Start; Line < Start + Body.size(); ++Line) if(Runs[Run].Documents[Line]) return false;
		// 本文と同じ論理行の並びかの返戻
		return std::equal(Body.begin(), Body.end(), Lines.begin() + static_cast<std::ptrdiff_t>(Start));
	};
	// 塊の後の最初のコード行毎の照合済の位置の終わり（原文の順）
	std::unordered_map<uint32_t, Position> Cursors;
	for(const ConvertedDocument &Document : Converted) {
		// 内容の無い文書は通常コメントとして置かない（整形仕様 7.7）
		if(Document.Lines.empty()) continue;
		const std::unordered_map<std::string_view, DestinationIndex>::const_iterator BodyPositions =
		Positions.find(Document.Lines.front());
		// 本文の最初の論理行が出力に無い場合の返戻
		if(BodyPositions == Positions.end()) return false;
		std::optional<Position> FoundAt;
		uint32_t FoundNext = 0;
		for(const uint32_t Next : Document.Destinations) {
			const DestinationIndex::const_iterator Destination = BodyPositions->second.find(Next);
			if(Destination == BodyPositions->second.end()) continue;
			const std::unordered_map<uint32_t, Position>::iterator Cursor = Cursors.try_emplace(Next, 0, 0).first;
			const std::vector<Position> &Starts = Destination->second;
			for(
				std::vector<Position>::const_iterator At = std::lower_bound(Starts.begin(), Starts.end(), Cursor->second);
				At != Starts.end();
				++At
			) {
				if(FoundAt && *FoundAt <= *At) break;
				if(!MatchesAt(*At, Document.Lines)) continue;
				FoundAt = *At;
				FoundNext = Next;
				break;
			}
		}
		// 本文の欠落，認めない移動先，文書の記法の儘の配置，改変又は原文の順の違反の返戻
		if(!FoundAt) return false;
		Cursors.find(FoundNext)->second = { FoundAt->first, FoundAt->second + Document.Lines.size() };
	}
	// 期待の本文（変換した文書と通常コメント）の論理行の字面毎の位置
	std::vector<std::span<const std::string>> Bodies;
	for(const ConvertedDocument &Document : Converted) Bodies.emplace_back(Document.Lines);
	for(const std::vector<std::string> &Lines : Ordinary) Bodies.emplace_back(Lines);
	std::unordered_map<std::string_view, std::vector<Position>> Expected;
	for(size_t Body = 0; Body < Bodies.size(); ++Body) {
		for(size_t Line = 0; Line < Bodies[Body].size(); ++Line) Expected[Bodies[Body][Line]].emplace_back(Body, Line);
	}
	// 数え終えた本文
	std::unordered_set<std::string> Counted;
	for(const ConvertedDocument &Document : Converted) {
		if(Document.Lines.empty()) continue;
		std::string Key;
		for(const std::string &Line : Document.Lines) Key.append(Line).push_back('\n');
		// 同じ本文の２つ目以後の文書は数えない
		if(!Counted.insert(std::move(Key)).second) continue;
		size_t Occurrences = 0, Allowed = 0;
		if(
			const std::unordered_map<std::string_view, DestinationIndex>::const_iterator Found = Positions.find(Document.Lines.front());
			Found != Positions.end()
		) for(const std::pair<const uint32_t, std::vector<Position>> &Destination : Found->second) {
			for(const Position At : Destination.second) Occurrences += MatchesAt(At, Document.Lines);
		}
		// 変換した文書と通常コメントの本文の中の出現（他の文書の本文の一部に一致する物を含む）
		for(const auto &[Body, Start] : Expected[Document.Lines.front()]) {
			const std::span<const std::string> Lines = Bodies[Body];
			Allowed += Start + Document.Lines.size() <= Lines.size() &&
			std::equal(Document.Lines.begin(), Document.Lines.end(), Lines.begin() + static_cast<std::ptrdiff_t>(Start));
		}
		// 変換した文書と通常コメントの数を越えて現れる本文の複製の返戻
		if(Occurrences > Allowed) return false;
	}
	// 変換した文書の本文と移動先を保った事の返戻
	return true;
}

/**
 * 完成候補のコメントだけの行の塊の取得関数（整形仕様 7.6, 8.1）
 * 行の最初から始まり行の終わりで終わるコメントが，空行を挟まずに続く並びを塊とし，塊の論理行と塊の後の最初のコード行の断片を返す
 * コードの論理行は空行の照合と同じ行の分類 (`LineSkeleton`) で数え，基準の字面の同じ順のコードの論理行の断片へ対応させる
 * 計算量：字面の長さ N とコメントの数 C に対し O(N + C log N)
 * @param Text 補修前の最終字面のコード（逐語データを除く）
 * @param Comments 最終字面でコメントと読まれる範囲（位置順）
 * @param Output 範囲と同じ順の最終字面のコメントの記録
 * @param CodeStarts 最終字面のコードの論理行の始まりの位置（空行の照合の骨格と共有，位置順）
 * @param CodeLines 基準の字面のコードの論理行の始まりの断片の番号（位置順）
 * @param Reading 文法の読取規則
 * @return 位置順の塊（範囲と記録が対応しなければ空）
 */
std::vector<SyntaxValidator::CommentRun> SyntaxValidator::CommentRuns(
	const std::string_view Text,
	const ByteRanges &Comments,
	const Core::CommentRepository &Output,
	const std::span<const size_t> CodeStarts,
	const std::span<const uint32_t> CodeLines,
	const ReadingRules &Reading
) {
	const std::span<const CommentRecord> Records = Output.GetRecords();
	// 範囲と記録が対応しない字面の返戻
	if(Records.size() != Comments.size()) return {};
	// 行の中で位置の後が空白だけか（行末迄）
	const auto IsBlankAfter = [&Text, &Reading](const size_t Pos) -> bool {
		const uint32_t After = TextEdit::SkipSpRight(Text, static_cast<uint32_t>(Pos), Reading);
		// 行末か字面の終わりの返戻
		return After >= Text.size() || Text[After] == '\n';
	};
	std::vector<CommentRun> Runs;
	for(size_t Index = 0; Index < Comments.size(); ++Index) {
		const auto [Start, End] = Comments[Index];
		if(TextEdit::IsAfterCode(Text, Start, Reading) || !IsBlankAfter(End)) continue;
		// 前のコメントと改行１つだけを挟んで続くか
		const bool Joins = !Runs.empty() && Index && !TextEdit::IsAfterCode(Text, Comments[Index - 1].first, Reading) &&
		IsBlankAfter(Comments[Index - 1].second) &&
		std::ranges::count(Text.substr(Comments[Index - 1].second, Start - Comments[Index - 1].second), '\n') == 1;
		if(!Joins) Runs.push_back({ std::numeric_limits<uint32_t>::max(), {}, {} });
		const std::string_view Body = Records[Index].Text;
		// 通常コメントと区別する文書の記法の記録か（変換した文書を文書の儘置いた物を照合に使わない）
		const bool IsDocument =
		Core::DocCommentSynchronizer::IsDedicatedDocument(Core::CommentRepository::RulesOf(Records[Index]), Records[Index]);
		for(size_t Cursor = 0; Cursor <= Body.size();) {
			const size_t Break = std::min(Body.find('\n', Cursor), Body.size());
			Runs.back().Lines.emplace_back(Body.substr(Cursor, Break - Cursor));
			Runs.back().Documents.push_back(IsDocument);
			Cursor = Break + 1;
		}
		// 塊の後の最初のコード行の断片
		const size_t Ordinal = static_cast<size_t>(std::ranges::lower_bound(CodeStarts, End) - CodeStarts.begin());
		Runs.back().Next = Ordinal < CodeLines.size() ? CodeLines[Ordinal] : std::numeric_limits<uint32_t>::max();
	}
	// 位置順の塊の返戻
	return Runs;
}

/**
 * 構文誤りの周りの失った字面の数の取得関数
 * 整形が差し込んだコメント（文書化コメントの雛形等）の印は読み飛ばし，入力のコメントの印は同じ所に求める
 * 計算量：整形後の長さ N と節点の数 V，字面の長さの和 L，字面の印の数 M，印を除いた字で一致した所の数 K に対し O(N log N + V + L + K×M)
 * @param Src 整形後の解析済のソース
 * @param Zones 入力から集めた字面の並び (`SourceProjector::CollectErrorZones`)
 * @param OutputZones 完成候補の誤り区画の並び
 * @param Comments 完成候補のコメントの範囲
 * @return 整形後の空白を除きコメントを印とした字面に順に現れないか，其の出現数の減った字面の数
 */
size_t SyntaxValidator::CountLostErrorZones(
	const ParsedTree &Src,
	const std::vector<ValidationContext::ErrorZone> &Zones,
	const std::vector<ValidationContext::ErrorZone> &OutputZones,
	const ByteRanges &Comments
) {
	size_t Lost = 0;
	// 構文誤りの無かった入力と，木を持たず確かめ様の無い原稿の返戻（後段の解析済の判定へ委ねる）
	if(Zones.empty() || !Src.IsParsed()) return Lost;
	std::string Code;
	Core::SourceProjector::AppendCode(Src, 0, static_cast<uint32_t>(Src.CodeText().size()), Comments, Code);
	// 整形後の全体での出現数の一括計数
	std::vector<std::string_view> Texts;
	Texts.reserve(Zones.size());
	for(const ValidationContext::ErrorZone &Zone : Zones) Texts.push_back(Zone.Text);
	const std::vector<uint32_t> Counts = Core::SourceProjector::CountZones(Code, Texts);
	// 印を除いた字と，字毎の直前の印の有無（最後の要素は最後の字の後の印）
	std::string Plain;
	std::vector<uint8_t> Marked(1, 0);
	Plain.reserve(Code.size());
	Marked.reserve(Code.size() + 1);
	for(const char Char : Code) {
		if(!Char) Marked.back() = 1;
		else {
			Plain.push_back(Char);
			Marked.push_back(0);
		}
	}
	// 整形後の構文誤りの周りの字面（出現数の減った字面だけを確かめる為に遅延して集める）
	std::optional<std::unordered_set<std::string>> OutputTexts;
	std::string Needle;
	std::vector<size_t> Marks, Fail;
	size_t Pos = 0;
	for(size_t Idx = 0; Idx < Zones.size(); ++Idx) {
		const std::string &Text = Zones[Idx].Text;
		// 字面の印を除いた字と，印を求める字の位置（字の数と同じ位置は最後の字の後の印）
		Needle.clear();
		Marks.clear();
		for(const char Char : Text) {
			if(Char) Needle.push_back(Char);
			else Marks.push_back(Needle.size());
		}
		size_t End = std::string::npos;
		if(Needle.empty()) {
			// 印だけの字面の後の最初の印の探索
			for(size_t At = Pos; At < Marked.size() && End == std::string::npos; ++At) if(Marked[At]) End = At;
		} else {
			// 字面の接頭辞と接尾辞の最長一致（KMP 法の失敗の遷移）
			Fail.assign(Needle.size(), 0);
			for(size_t At = 1, Len = 0; At < Needle.size(); ++At) {
				while(Len && Needle[At] != Needle[Len]) Len = Fail[Len - 1];
				if(Needle[At] == Needle[Len]) ++Len;
				Fail[At] = Len;
			}
			// 印を除いた字で一致し，字面の印の所にも印が在る最初の位置の探索
			for(size_t At = Pos, Len = 0; At < Plain.size() && End == std::string::npos; ++At) {
				while(Len && Plain[At] != Needle[Len]) Len = Fail[Len - 1];
				if(Plain[At] == Needle[Len]) ++Len;
				if(Len < Needle.size()) continue;
				const size_t Start = At + 1 - Len;
				if(
					std::all_of(
						Marks.begin(),
						Marks.end(),
						[&Marked, Start](const size_t Mark) -> bool {
							// 同じ所に印が在るかの返戻
							return Marked[Start + Mark];
						}
					)
				) End = At + 1;
				else Len = Fail[Len - 1];
			}
		}
		// 字面を失った区画の計数（後の字面は同じ位置から探す）
		if(End == std::string::npos) {
			++Lost;
			continue;
		}
		if(Counts[Idx] < Zones[Idx].Count) {
			// 出現数が減った時は，構文誤りの所の字面が壊れて後の同じ字面に一致したのか，別の所の同じ字面が正当に変わったのかを，整形後にも同じ字面の構文誤りが残るかで見分ける
			if(!OutputTexts) {
				// 整形後の構文誤りの所の字面の集合（区画毎の線形走査に依る二乗化を防ぐ）
				OutputTexts.emplace();
				for(const ValidationContext::ErrorZone &Output : OutputZones) OutputTexts->insert(Output.Text);
			}
			// 構文誤りの所の字面を壊した区画の計数
			if(!OutputTexts->count(Text)) ++Lost;
		}
		Pos = End;
	}
	// 失った字面の数の返戻
	return Lost;
}

/**
 * 入力の区画に一致しない構文誤りの区画の数の取得関数
 * 失った区画の１つは整形後の区画の１つだけを説明し，其れを超える区画は正常区画に生じた構文誤りとする（整形仕様 8.3）
 * 計算量：整形後の長さ N と節点の数 V，字面の長さの和 L に対し O(N log N + V + L)
 * @param OutputZones 完成候補の誤り区画の並び
 * @param Zones 入力から集めた字面の並び (`SourceProjector::CollectErrorZones`)
 * @return 整形後の構文誤りの区画の内，入力の区画と字面が一致しない物の数
 */
size_t SyntaxValidator::CountUnmatchedErrorZones(
	const std::vector<ValidationContext::ErrorZone> &OutputZones,
	const std::vector<ValidationContext::ErrorZone> &Zones
) {
	// 入力の誤り区画の出現数の集計
	std::unordered_map<std::string_view, size_t> Remaining;
	for(const ValidationContext::ErrorZone &Zone : Zones) ++Remaining[Zone.Text];
	// 出力で一致しない誤り区画の計数
	size_t Unmatched = 0;
	for(const ValidationContext::ErrorZone &Output : OutputZones) {
		if(
			const std::unordered_map<std::string_view, size_t>::iterator Found = Remaining.find(Output.Text);
			Found != Remaining.end() && Found->second
		) --Found->second;
		else ++Unmatched;
	}
	// 一致しない区画の数の返戻
	return Unmatched;
}

/**
 * 最終字面の検証関数
 * 出力を新しい入力として入力と同じ字面の段の適応の後に一度だけ解析・投影し，入力に無かった構文の破壊が無いかを確かめる（整形仕様 8.1）
 * 補修後の解析結果で保持条件を照合し，其の投影を構造の照合と静的検査で共有する（整形仕様 3.4, 8.4）
 * 見送と失敗を同時に検出した場合は失敗を優先する為，全ての検査の後に失敗に当たる物から返す（整形仕様 8.3）
 * 計算量：字面の長さ N に対し解析１回分の O(N)
 * @param Profile 入力で確定した文法
 * @param Code `CodeLayout` が確定した変更不能なコードの出力（空行の照合基準で，照合の後に手放す）
 * @param Src 整形の済んだ最終字面
 * @param Expectation 中核が確定した期待構文と選択表現
 * @param Comments 同期・本文正規化済の期待コメント（文書とコメントの照合の比較元で，読むだけ）
 * @param Context 入力側から渡す資料（静的検査の要否を含む）
 * @return 検証の結果
 * @throws FormatLimitExceeded 解析用の印が最終字面に残る場合と，構造の照合中に内部の不変条件の違反を検出した場合（整形器の不具合）
 */
SyntaxValidator::ValidationResult SyntaxValidator::Validate(
	const GrammarProfile &Profile,
	std::shared_ptr<const LaidOutCode> Code,
	const std::string &Src,
	const Core::OutputExpectation &Expectation,
	const Core::CommentRepository &Comments,
	const ValidationContext &Context
) {
	const Lang Language = Profile.GetLanguage();
	// 最終字面を新しい入力として入力と同じ正規化・適応・受理判定で一度だけ解析し，整形中の木を持ち込まない（文法は入力で確定した定義）
	InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, Src);
	// 正規化の資源上限（試験解析の期限等）の見送の返戻
	if(Normalized.Reason.IsResourceLimit()) return { ValidationStatus::Skipped, std::nullopt, Normalized.Reason };
	if(Normalized.Reason != FailureReason::None || Normalized.IsFormChanged) {
		// 入力の表現として受理出来ない最終字面の返戻
		return { ValidationStatus::Failed, std::nullopt, FailureReason::SyntaxBroken };
	}
	// 解析用の印がコードへ残って居ない事の確認（実行停止後のデータは逐語区画として除く）
	const std::string_view CheckedCode(
		Normalized.Text.data(),
		Normalized.DataStart == std::string::npos ? Normalized.Text.size() : Normalized.DataStart
	);
	for(const std::string &Mark : Context.ProtectedRegions.AnalysisMarks) if(CheckedCode.find(Mark) != std::string_view::npos) {
		throw FormatLimitExceeded("analysis marks remain in the output", FailureReason::FormatterDefect);
	}
	ParseAdapter::ParseInput Adapted = ParseAdapter::Read(Profile, Normalized);
	if(Adapted.Decision != ParseDecision::Continue) {
		// 解析器の失敗の返戻
		if(Adapted.Decision == ParseDecision::Fail) return { ValidationStatus::Failed, std::nullopt, Adapted.Failure };
		// 読取の資源上限は見送とし，入力が読めた完成候補を読めなく為った其の他の理由は構文の破壊とする分類の返戻（整形仕様 3.2, 8.3）
		return Adapted.Failure.IsResourceLimit() ?
		ValidationResult{ ValidationStatus::Skipped, std::nullopt, Adapted.Failure } :
		ValidationResult{ ValidationStatus::Failed, std::nullopt, FailureReason::SyntaxBroken };
	}
	ParsedTree &ReadTree = *Adapted.Src;
	const std::string BeforeText(ReadTree);
	const std::string_view BeforeEdits = std::string_view(BeforeText).substr(0, ReadTree.CodeText().size());
	Core::DiagnosticLocationMap Locations;
	// 空行と変換した文書の照合の基準の字面（断片を改行で繋いだ物）と，其の中で文法がコメントと読む区切記法と逐語区画に残したコメントの範囲（位置順）
	std::string LaidOut;
	const std::span<const CodeFragment> Fragments = Code->GetFragments();
	for(size_t Index = 0; Index < Fragments.size(); ++Index) {
		if(Index) LaidOut += '\n';
		LaidOut += Fragments[Index].Text;
	}
	std::vector<std::pair<uint32_t, uint32_t>> Excluded(Code->GetSeparators().begin(), Code->GetSeparators().end());
	const std::vector<NodeSpan> &Spans = Code->GetAnchors().Nodes;
	for(const ValidationContext::KeptComment &Comment : Context.ProtectedRegions.KeptComments) {
		if(Comment.Node < Spans.size() && Spans[Comment.Node].IsPlaced) {
			Excluded.emplace_back(Spans[Comment.Node].Start + Comment.Start, Spans[Comment.Node].Start + Comment.End);
		}
	}
	// 最終字面の座標と空行は補修前の本文を基準に保つ
	const ByteRanges CommentRanges = ReadTree.CollectCommentRanges(ParseAdapter::OuterTextBounds(ReadTree).second);
	Adapted.Source = ParseInput::VerificationSource{ Normalized.Text, BeforeEdits, BeforeText, CommentRanges };
	// 空行の照合で読み飛ばす最終字面の範囲（コメントと，子の整形の検証が空行を保証する所有区画の中身，整形仕様 4.4, 8.1）
	ByteRanges Skipped = CommentRanges;
	if(ReadTree.GetReading().HasEmbeddedScripts) {
		const AnchorPositionMap &Anchors = Code->GetAnchors();
		std::vector<TSNode> RawTexts;
		WalkChildrenCursor(
			ReadTree.GetRoot(),
			[&](const TSNode Node) -> bool {
				// 逐語の本文でない節点の内側へ降りる事の返戻
				if(!ReadTree.Is(Node, Core::Role::RawTextNode)) return true;
				RawTexts.push_back(Node);
				// 逐語の本文の内側へ降りない事の返戻
				return false;
			}
		);
		std::vector<Core::NodeId> Embedded;
		for(Core::NodeId Node = 0; Node < Anchors.Syntax->NodeCount(); ++Node) {
			if(Anchors.Syntax->Is(Node, Core::Role::RawTextNode)) Embedded.push_back(Node);
		}
		// 埋込の数が同じ時だけ文書の順で対応させる（食い違う字面は除かずに照合し，空行の変化として検出する）
		if(RawTexts.size() == Embedded.size()) {
			for(size_t Index = 0; Index < Embedded.size(); ++Index) if(Anchors.Syntax->RegionOf(Embedded[Index])) {
				Skipped.emplace_back(ts_node_start_byte(RawTexts[Index]), ts_node_end_byte(RawTexts[Index]));
			}
			// 区画の前後の改行を含む断片の範囲（開始・終了タグを最終字面の側と同じく１行に繋ぐ）
			for(const OwnedRegionSpan &Span : Anchors.OwnedRegions) {
				const uint32_t Start = Fragments[Span.FirstFragment].Start;
				Excluded.emplace_back(
					Start ? Start - 1 : 0,
					Span.EndFragment < Fragments.size() ? Fragments[Span.EndFragment].Start : static_cast<uint32_t>(LaidOut.size())
				);
			}
			std::sort(Skipped.begin(), Skipped.end());
		}
	}
	std::sort(Excluded.begin(), Excluded.end());
	// 空行の照合の骨格のコードの論理行の始まり（最終字面と基準の字面，通常コメントへ変換した文書の移動先の照合と共有する）
	std::vector<size_t> OutputStarts, LaidOutStarts;
	const bool IsBlankLineChanged = !KeepsBlankLines(ReadTree, Skipped, LaidOut, Excluded, OutputStarts, LaidOutStarts);
	// 期待コメントの記録毎の文書の説明対象と，確定した配置で文書として置く説明対象（整形仕様 7.4, 7.6）
	const Core::SyntaxModel &ExpectedSyntax = *Code->GetAnchors().Syntax;
	const std::span<const CommentRecord> Records = Comments.GetRecords();
	const std::vector<Core::NodeId> Recognized = Core::DocCommentSynchronizer::DocumentTargets(ExpectedSyntax, Comments);
	const std::vector<Core::NodeId> Placed = Core::DocCommentSynchronizer::PlacedTargets(*Code, Records, Recognized);
	// 配置の段と同じ出力記法の記録
	std::vector<CommentRecord> Outputs;
	Outputs.reserve(Records.size());
	for(const CommentRecord &Record : Records) Outputs.push_back(Core::CommentRepository::OutputFormOf(Record));
	// 期待コメントの文書の説明対象と本文（出力記法の論理行，整形仕様 8.1）
	const std::vector<ValidationContext::DocumentFact> Documents =
	Core::SourceProjector::DescribeDocuments(ExpectedSyntax, Outputs, Placed);
	// 通常コメントへ変換した文書と通常コメントの本文，完成候補のコード行へ順に対応する断片（空行の照合と同じ行の分類，整形仕様 7.6, 8.1）
	std::vector<ConvertedDocument> Converted;
	std::vector<std::vector<std::string>> Ordinary;
	std::vector<uint32_t> CodeLines;
	if(Recognized != Placed) {
		// 出力記法の記録の本文構造
		const CommentStructure Structure = CommentNormalizer::Inspect(ReadOnlyCommentView(Outputs));
		Converted = ConvertedDocuments(*Code, Outputs, Structure, Recognized, Placed);
		Ordinary = OrdinaryBodies(Outputs, Structure, Recognized);
		// コードの論理行の始まりの断片（断片は改行で繋いだ為，前の改行の数）
		uint32_t Fragment = 0;
		size_t Counted = 0;
		for(const size_t Start : LaidOutStarts) {
			Fragment += static_cast<uint32_t>(
				std::count(LaidOut.begin() + static_cast<std::ptrdiff_t>(Counted), LaidOut.begin() + static_cast<std::ptrdiff_t>(Start), '\n')
			);
			Counted = Start;
			CodeLines.push_back(Fragment);
		}
	}
	// 照合し終えた配置は以後読まない為，出力の投影と重ならない様に記憶域を空ける
	Code.reset();
	// 補修前の破損位置と埋込区画の診断位置の収集
	if(ts_node_has_error(ReadTree.GetRoot())) {
		const TSPoint At = FirstBrokenPoint(ReadTree);
		Locations.FirstBroken = { At.row, At.column };
	}
	if(ReadTree.GetReading().HasEmbeddedScripts) {
		for(const ParseAdapter::EmbeddedContent &Embed : ParseAdapter::EmbeddedContents(ReadTree)) {
			const TSPoint Start = ts_node_start_point(Embed.Element);
			const TSPoint Content = ts_node_is_null(Embed.RawText) ? Start : ts_node_start_point(Embed.RawText);
			Locations.Embedded.emplace_back(Core::OutputPoint{ Start.row, Start.column }, Core::OutputPoint{ Content.row, Content.column });
		}
	}
	Locations.Text = BeforeText;
	// 入力と同じ補修を済ませ，検証する事実と投影を同じ木から得る
	if(!ParseAdapter::Repair(Adapted)) {
		// 独立して検出した空行の破壊を優先した失敗の返戻
		if(IsBlankLineChanged) return { ValidationStatus::Failed, std::nullopt, FailureReason::BlankLineChanged };
		// 補修の資源上限（整形仕様 9.3）は見送とし，入力が読めた連鎖を完成候補が読めなく為った事は構文の破壊とする分類の返戻（整形仕様 3.2）
		return Adapted.Decision == ParseDecision::Skip && Adapted.Failure.IsResourceLimit() ?
		ValidationResult{ ValidationStatus::Skipped, std::nullopt, Adapted.Failure } :
		ValidationResult{ ValidationStatus::Failed, std::nullopt, FailureReason::SyntaxBroken };
	}
	// 受理済の木の写しを受け取り，適応に使った木の記憶域を空ける
	ParsedTree Final = SyntaxParser::Parse(*Adapted.Profile, Adapted);
	Adapted.Src.reset();
	const bool IsRejected = Final.GetParseDecision() != ParseDecision::Continue;
	// インデントを失い解析器が空と読む塊（誤りを立てない為に上の検査を通る）
	const bool HasLostBody = Final.GetReading().HasIndentBlocks && SyntaxParser::HasEmptyBlock(Final);
	// 宣言の値の中の `//` を読めない CSS / SCSS で，出力の行コメントが次の行の波括弧を本文へ新たに吸ったか（整形仕様 8.1 の字句と値，入力に由来する物は入力の段で見送る）
	const bool IsBraceAbsorbed =
	Final.GetReading().AbsorbsRulesIntoValueComments && !Context.ProtectedRegions.InputText.empty() && !KeepsBracesOutsideComments(
		Final.GetRules().Notation,
		Context.ProtectedRegions.InputText,
		Final,
		Final.GetReading().UrlFunctionHead
	);
	// 入力と同じ補修の後の一度だけの投影と，期待構文との構造の照合（照合中に検出した内部の不変条件の違反は後で再送出する）
	bool IsStructureKept = false;
	std::exception_ptr StructureError;
	std::optional<Core::ProjectionResult> Projection;
	try {
		// 補修後に受理した木を投影する
		if(!IsRejected) {
			Projection.emplace(Core::SourceProjector::Project(Final, *Adapted.Profile, Adapted));
			// 最終構文は正規化・中間表現の構築をしない為，照合と静的検査の読取を其の予算へ数えない（整形仕様 9.2）
			Projection->Syntax.WorkAccount()->IsClosed = true;
			Locations.Nodes = std::move(Projection->Validation.InputDiagnostics.Positions);
			// 環境変数 `SHAVEFMT_STRUCT_DIAG` が在る時だけ期待構文との差を診断する（整形仕様 10.5，値の中身は問わない）
			if(Expectation.NormalizedSyntax) {
				IsStructureKept = KeepsStructure(
					Projection->Syntax,
					Language,
					Expectation,
					{ Context.SemanticChecks.Symbols, Projection->Validation.SemanticChecks.Symbols, Profile },
					std::getenv("SHAVEFMT_STRUCT_DIAG")
				);
			}
		} else if(std::getenv("SHAVEFMT_STRUCT_DIAG")) std::fputs("STRUCT-DIAG unavailable\n", stderr);
	} catch(...) {
		StructureError = std::current_exception();
	}
	if(StructureError) {
		// 照合より前に独立して検出した空行の破壊を，照合中の資源の上限等の送出より優先した失敗の返戻（整形仕様 8.3）
		if(IsBlankLineChanged) return { ValidationStatus::Failed, std::nullopt, FailureReason::BlankLineChanged };
		// 照合より前に検出した本体を失った塊の返戻
		if(HasLostBody) return { ValidationStatus::Failed, std::nullopt, FailureReason::EmptyBlock };
		// 照合より前に検出した波括弧の吸込の返戻
		if(IsBraceAbsorbed) return { ValidationStatus::Failed, std::nullopt, FailureReason::SyntaxBroken };
		// 照合中に検出した内部の不変条件の違反と資源の上限の再送出
		std::rethrow_exception(StructureError);
	}
	ValidationContext Candidate;
	if(Projection) Candidate = std::move(Projection->Validation);
	else {
		// 投影しない候補は，解析木から失敗と見送の分類資料を集める
		std::vector<TSNode> Nodes;
		WalkAst(
			Final.GetRoot(),
			[&Nodes](const TSNode Node) -> void {
				Nodes.push_back(Node);
			}
		);
		Candidate = Core::SourceProjector::CollectContext(Final, Profile, Adapted, Nodes);
		Candidate.ProtectedRegions.Directives =
		Core::SourceProjector::CollectDirectives(Final, Nodes, &Candidate.ProtectedRegions.DirectiveStarts);
	}
	// 入力に無かった構文誤りの有無（両側とも補修前後の少ない方の件数で比べる）
	const bool IsSyntaxBroken = ts_node_has_error(Final.GetRoot()) && (
		Context.InputDiagnostics.InputBrokenCount ?
		std::min(Adapted.PreparedBroken, Candidate.InputDiagnostics.BrokenCount) > Context.InputDiagnostics.InputBrokenCount :
		Candidate.InputDiagnostics.HasBrokenNode
	);
	const bool IsSemanticsChanged =
	!Context.SemanticChecks.Checks.empty() && Candidate.SemanticChecks.Checks != Context.SemanticChecks.Checks;
	const bool IsDirectiveChanged = Candidate.ProtectedRegions.Directives != Context.ProtectedRegions.Directives;
	// 直接付く文書の説明対象と本文，通常コメントへ変換した文書の本文と移動先の保持（期待コメントと比べる，整形仕様 7.6, 8.1）
	// 変換した文書の移動先はコードの論理行の順の対応に依る為，其の対応が崩れる空行の破壊が在れば照合せず空行の失敗を返す
	const bool IsDocumentChanged = Projection && (
		!KeepsDocuments(Documents, Candidate.ProtectedRegions.Documents, Outputs, Placed) ||
		!Converted.empty() && !IsBlankLineChanged && !KeepsConvertedDocuments(
			Converted,
			Ordinary,
			CommentRuns(BeforeEdits, CommentRanges, Projection->Comments, OutputStarts, CodeLines, Profile.GetReading())
		)
	);
	// 行末の各記号の前置区切りを，同じ最初の論理行を持つ期待コメントの記録のタブの数と照合する（並替・統合で記録の順と錨が変わっても記号毎に対応させる，整形仕様 7.6, 8.1）
	std::unordered_map<std::string_view, std::vector<size_t>> Recorded;
	for(size_t Index = 0; Index < Records.size(); ++Index) if(Records[Index].Kind == CommentKind::Trailing) {
		const std::string Marker = CommentNormalizer::StackMarkerOf(Outputs[Index].Notation);
		std::string_view Rest = Outputs[Index].Text;
		size_t Tabs = Records[Index].Format.LeadingTabCount;
		while(true) {
			const std::string_view Line = CommentNormalizer::FirstLogicalLine(Rest, Marker);
			Recorded[Line].push_back(Tabs);
			Rest.remove_prefix(Line.size());
			// 次の記号の前置区切り（本文の中の記号は本文の空白のタブの数，改行で分けた論理行は積む時のスペース１つ）
			const size_t GapEnd = std::min(Rest.find_first_not_of(" \t"), Rest.size());
			Tabs = static_cast<size_t>(std::ranges::count(Rest.substr(0, GapEnd), '\t'));
			Rest.remove_prefix(GapEnd);
			if(Rest.starts_with('\n')) {
				Rest.remove_prefix(1);
				Tabs = 0;
			} else if(!Marker.empty() && Rest.starts_with(Marker)) Rest.remove_prefix(Marker.size());
			else break;
			if(Rest.empty()) break;
		}
	}
	bool IsSeparatorChanged = false;
	for(const ValidationContext::LeadingSeparator &Separator : Candidate.InputDiagnostics.LeadingSeparators) {
		const std::unordered_map<std::string_view, std::vector<size_t>>::iterator Found = Recorded.find(Separator.Line);
		// 不正な区切りと，同じ本文の記録の何れのタブの数とも合わない区切りの検出
		IsSeparatorChanged = Separator.Tabs == SIZE_MAX ||
		Found != Recorded.end() && std::ranges::find(Found->second, Separator.Tabs) == Found->second.end();
		if(IsSeparatorChanged) break;
		if(Found != Recorded.end()) Found->second.erase(std::ranges::find(Found->second, Separator.Tabs));
	}
	// 指示コメント位置と誤り区画の照合
	Locations.DirectiveStarts = std::move(Candidate.ProtectedRegions.DirectiveStarts);
	for(uint32_t &Start : Locations.DirectiveStarts) Start = Final.OffsetBeforeEdits(Start);
	const size_t LostZones =
	CountLostErrorZones(Final, Context.ErrorZones, Candidate.ErrorZones, Candidate.InputDiagnostics.CommentRanges);
	const bool IsNormalRegionBroken =
	IsSyntaxBroken && (!LostZones || CountUnmatchedErrorZones(Candidate.ErrorZones, Context.ErrorZones) > LostZones);
	const bool IsObservationLost = !Context.ProtectedRegions.ObservedPositions.empty() &&
	Candidate.ProtectedRegions.ObservedPositions != Context.ProtectedRegions.ObservedPositions;
	// 正常区画に生じた新しい構文破壊の返戻（記録した誤り区画の保持違反に伴う物は後で見送とする，整形仕様 8.3）
	if(IsNormalRegionBroken) return { ValidationStatus::Failed, std::nullopt, FailureReason::SyntaxBroken };
	// 意味上の区別を保てて居ない事の返戻
	if(IsSemanticsChanged) return { ValidationStatus::Failed, std::nullopt, FailureReason::SemanticsChanged };
	// 指示コメントの保持契約に違反した候補を救済せず失敗とする事の返戻（整形仕様 8.3）
	if(IsDirectiveChanged) return { ValidationStatus::Failed, std::nullopt, FailureReason::DirectiveChanged };
	// 整形仕様 7.4 で認識した文書の説明対象を保てない候補を救済せず失敗とする事の返戻（整形仕様 7.7, 8.3）
	if(IsDocumentChanged) return { ValidationStatus::Failed, std::nullopt, FailureReason::DocumentChanged };
	if(IsSeparatorChanged) {
		// 行末コメントの前置区切りを記録と違えた配置の不具合の返戻
		return {
			ValidationStatus::Failed,
			std::nullopt,
			{ FailureReason::FormatterDefect, "a line-end comment separator differs from its record" }
		};
	}
	// 空行を追加・削除・移動又は上書した候補を失敗とする事の返戻（コメントの配置の不具合）
	if(IsBlankLineChanged) return { ValidationStatus::Failed, std::nullopt, FailureReason::BlankLineChanged };
	// 本体を失った塊を作った事の返戻
	if(HasLostBody) return { ValidationStatus::Failed, std::nullopt, FailureReason::EmptyBlock };
	// 出力のコメントが波括弧を新たに吸った構文破壊の返戻（整形仕様 8.3）
	if(IsBraceAbsorbed) return { ValidationStatus::Failed, std::nullopt, FailureReason::SyntaxBroken };
	// 受理拒否を解析不能区画の喪失で説明出来る候補の見送の返戻
	if(IsRejected && LostZones) return { ValidationStatus::Skipped, std::nullopt, FailureReason::ErrorZoneLost };
	// 其の他の受理拒否を正常区画を読めない構文破壊とする失敗の返戻
	if(IsRejected) return { ValidationStatus::Failed, std::nullopt, FailureReason::SyntaxBroken };
	// 選択した表現対応の下で再投影した構造を検証出来なかった候補の失敗の返戻
	if(!IsStructureKept) return { ValidationStatus::Failed, std::nullopt, FailureReason::StructureChanged };
	// 解析不能区画の保持を確認出来ない候補の拒否の返戻（整形仕様 8.3 の見送）
	if(LostZones) return { ValidationStatus::Skipped, std::nullopt, FailureReason::ErrorZoneLost };
	// 観測される値を保てて居ない事の返戻
	if(IsObservationLost) return { ValidationStatus::Skipped, std::nullopt, FailureReason::SourceObservationLost };
	// 静的検査の位置の行と桁（補修の前の字面の行頭の位置から求める）と最終字面のコメントの記録
	Locations.LineStarts.push_back(0);
	for(
		size_t Newline = Locations.Text.find('\n');
		Newline != std::string::npos;
		Newline = Locations.Text.find('\n', Newline + 1)
	) Locations.LineStarts.push_back(static_cast<uint32_t>(Newline + 1));
	for(Core::OutputRange &Range : Locations.Nodes) Range.Point = Locations.PointAt(Range.Start);
	for(const auto &[Start, End] : CommentRanges) Locations.Comments.push_back({ Start, End, Locations.PointAt(Start) });
	// 検証を通った候補の返戻
	return {
		ValidationStatus::Verified,
		Core::ValidatedOutput{ std::move(Projection->Syntax), std::move(Projection->Comments), std::move(Locations) },
		FailureReason::None
	};
}
