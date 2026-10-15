#include "CommentPlacer.hpp"
#include "CommentRepository.hpp"
#include "DocCommentReader.hpp"
#include "SignatureExtractor.hpp"
#include "SyntaxModel.hpp"
#include "SyntaxRole.hpp"
#include "../Util/CharClass.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include "../Util/LayoutTypes.hpp"
#include "../Util/TextEdit.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 行末迄を本文とするコメントかの判定関数
	 * 行コメントは１度置くと後続の挿入が種別に依らず其の本文へ吸われる為，並べて良いかの判定に用いる
	 * @param Text コメント本文
	 * @return `//` / `#` で始まるなら true
	 */
	bool CommentPlacer::IsLineCommentStart(const std::string_view Text) {
		// 行末迄を本文とするコメントかの返戻
		return Text.starts_with(CommentRepository::GetOpenerOf(CommentNotation::SlashLine)) ||
		Text.starts_with(CommentRepository::GetOpenerOf(CommentNotation::HashLine));
	}

	/**
	 * テキストの位置を含む断片の取得関数
	 * 計算量：断片数 F に対し O(log(F + 1))
	 * @param Code 変更不能なコードの出力
	 * @param Offset テキストの位置
	 * @return 位置を含む断片（行末の位置は其の行）
	 */
	uint32_t CommentPlacer::GetFragmentAt(const LaidOutCode &Code, const uint32_t Offset) {
		// 位置を含むコード断片の探索
		const std::span<const CodeFragment> Fragments = Code.GetFragments();
		const std::span<const CodeFragment>::iterator After = std::upper_bound(
			Fragments.begin(),
			Fragments.end(),
			Offset,
			[](const uint32_t Val, const CodeFragment &Fragment) -> bool {
				// 断片の始まりとの比較の返戻
				return Val < Fragment.Start;
			}
		);
		// 位置以前に始まる最後の断片の返戻
		return After == Fragments.begin() ? 0 : static_cast<uint32_t>(After - Fragments.begin() - 1);
	}

	/**
	 * テキストの位置の後に同じ行のコードが続くかの判定関数
	 * 計算量：断片数 F，トークン片数 P に対し O(log(F + 1) + log(P + 1))
	 * @param Code 変更不能なコードの出力
	 * @param Offset テキストの位置
	 * @return 位置から行末迄に空白以外の字が在れば true
	 */
	bool CommentPlacer::IsFollowedByCode(const LaidOutCode &Code, const uint32_t Offset) {
		// 後続のトークン片の取得
		const CodePiece *const Next = Code.GetPieceFrom(Offset);
		// 行の中の位置より後で終わるトークン片の有無の返戻（トークン片は区切の空白を含まない）
		return Next && Next->Start < Code.GetFragments()[GetFragmentAt(Code, Offset)].End;
	}

	/**
	 * 同じ行の位置以後の最初のコードの位置の取得関数
	 * 計算量：断片数 F，トークン片数 P に対し O(log(F + 1) + log(P + 1))
	 * @param Code 変更不能なコードの出力
	 * @param Offset テキストの位置
	 * @return 位置以後で区切の空白でない最初の位置（同じ行にコードが続かなければ行末）
	 */
	uint32_t CommentPlacer::GetContentFrom(const LaidOutCode &Code, const uint32_t Offset) {
		// 位置を含む行とトークン片の取得
		const CodeFragment &Line = Code.GetFragments()[GetFragmentAt(Code, Offset)];
		const CodePiece *const Next = Code.GetPieceFrom(Offset);
		// 同じ行のコードの位置か行末の返戻
		return Next && Next->Start < Line.End ? std::max(Offset, Next->Start) : std::max(Offset, Line.End);
	}

	/**
	 * トークン片の形の判定関数
	 * @param Piece トークン片（`nullptr` を許す）
	 * @param Shapes 求める形の何れか (`CodePiece::Shape`)
	 * @return トークン片が形の何れかを持てば true
	 */
	bool CommentPlacer::HasShape(const CodePiece *const Piece, const uint16_t Shapes) {
		// 形の有無の返戻
		return Piece && Piece->Shapes & Shapes;
	}

	/**
	 * テキストの位置の前に同じ行のコードが在るかの判定関数
	 * 計算量：断片数 F，トークン片数 P に対し O(log(F + 1) + log(P + 1))
	 * @param Code 変更不能なコードの出力
	 * @param Offset テキストの位置
	 * @return 行頭のタブの後から位置迄に空白以外の字が在れば true
	 */
	bool CommentPlacer::IsAfterCode(const LaidOutCode &Code, const uint32_t Offset) {
		// 位置を含む行と先行トークンの取得
		const CodeFragment &Line = Code.GetFragments()[GetFragmentAt(Code, Offset)];
		const CodePiece *const First = Code.GetPieceFrom(Line.Start);
		// 行の中の位置より前に始まるトークン片の有無の返戻（トークン片は行を跨がない）
		return First && First->Start < Offset && First->Start < Line.End;
	}

	/**
	 * テキストの位置の字を含むトークン片の取得関数
	 * 計算量：トークン片数 P に対し O(log(P + 1))
	 * @param Code 変更不能なコードの出力
	 * @param Offset テキストの位置
	 * @return 位置の字を含むトークン片（区切空白・インデント・改行の位置は `nullptr`）
	 */
	const CodePiece *CommentPlacer::GetPieceCovering(const LaidOutCode &Code, const uint32_t Offset) {
		// 位置を含むトークン片の取得
		const CodePiece *const Found = Code.GetPieceFrom(Offset);
		// 位置の字を含むトークン片の返戻
		return Found && Found->Start <= Offset ? Found : nullptr;
	}

	/**
	 * テキストの位置より前に始まる最後のトークン片の取得関数
	 * 計算量：トークン片数 P に対し O(log(P + 1))
	 * @param Code 変更不能なコードの出力
	 * @param Offset テキストの位置
	 * @return 位置より前に始まる最後のトークン片（無ければ `nullptr`）
	 */
	const CodePiece *CommentPlacer::GetPieceBefore(const LaidOutCode &Code, const uint32_t Offset) {
		// 位置より前のトークン片の探索
		const std::vector<CodePiece> &Pieces = Code.GetAnchors().Pieces;
		const std::vector<CodePiece>::const_iterator After = std::lower_bound(
			Pieces.begin(),
			Pieces.end(),
			Offset,
			[](const CodePiece &Piece, const uint32_t Pos) -> bool {
				// トークン片の始まりとの比較の返戻
				return Piece.Start < Pos;
			}
		);
		// 位置より前に始まる最後のトークン片の返戻
		return After == Pieces.begin() ? nullptr : &*(After - 1);
	}

	/**
	 * 置けないコメントの脱落の診断関数
	 * 通常コメントは種別・所属・コードの意味を保てる位置が無ければ除き，其の事実を診断用設定で確かめれる様にする（整形仕様 7.7, 10.5）
	 * 診断は理由とテキストの長さだけを出し，コメントの本文を出力しない
	 * 計算量：診断理由の長さ R，診断を格納する列の要素数 D，未合成の記録の合成費用 Q に対し O(R + D + Q + 1)
	 * @param Bytes 合成済のテキストの長さ
	 * @param Reason 置けない理由
	 * @param IsRequired 脱落を許さない記録か
	 * @param IsDirective ディレクティブコメントか
	 * @param Diagnostics 呼出側が表示する脱落の診断の格納先
	 * @param Record 診断する時だけ合成する記録（合成済なら nullptr）
	 * @throws FormatLimitExceeded 保護する記録を置けない場合
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	void CommentPlacer::ReportDrop(
		const size_t Bytes,
		const std::string_view Reason,
		const bool IsRequired,
		const bool IsDirective,
		std::vector<std::string> *const Diagnostics,
		const CommentRecord *const Record
	) {
		// 保持必須のコメントの配置失敗の通知
		if(IsRequired) throw FormatLimitExceeded(Reason, IsDirective ? FailureReason::DirectiveChanged : FailureReason::DocChanged);
		// 呼出側が診断の格納先を指定した時だけの脱落の診断
		if(Diagnostics) {
			Diagnostics->push_back(
				"comment dropped (" + std::string(Reason) + "): " +
				std::to_string(Record ? CommentRepository::Compose(*Record).size() : Bytes) + " bytes"
			);
		}
		// 終了
		return;
	}

	/**
	 * アンカーの後の文中コメントの置場の取得関数（整形仕様 7.6）
	 * 原文でコメントとアンカーの間に在った無名のトークンを同じ行の中で飛ばし，アンカーの後に同じ行のコードが続けばアンカーの直後へ置く
	 * 続かなければアンカーを終える区切・閉じのトークンの前，其れも無ければアンカーの直前へ置く
	 * 計算量：調べる子・兄弟の数 T，後続コードを調べるトークン数 K，断片数 F，トークン片数 P に対し O(T + (K + 1)(log(F + 1) + log(P + 1)))
	 * @param Code 変更不能なコードの出力
	 * @param Syntax 読取専用の期待構文
	 * @param Record コメントの記録
	 * @param HasInner アンカーの中のノードに紐付いた他のコメントが有るか
	 * @param InsideTokens アンカー毎に共有する配置済の無名トークンの位置
	 * @return 置場のテキストの位置（置けなければ `std::nullopt`）
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws FormatLimitExceeded 構文の照会・走査が作業予算を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 */
	std::optional<uint32_t> CommentPlacer::InlineAfter(
		const LaidOutCode &Code,
		const SyntaxModel &Syntax,
		const CommentRecord &Record,
		const bool HasInner,
		std::unordered_map<NodeId, std::vector<uint32_t>> &InsideTokens
	) {
		// アンカーの位置と文中コメントの配置候補の取得
		const std::vector<NodeSpan> &Spans = Code.GetAnchors().Nodes;
		const NodeId Anchor = Record.Anchor;
		const NodeSpan &Span = Spans[Anchor];
		// アンカーの中の無名のトークンの前（数が減った時は最後のトークンの前）
		if(Record.Placement.Side == CommentPlacement::Inside) {
			using TokenIter = std::unordered_map<NodeId, std::vector<uint32_t>>::iterator;
			const std::pair<TokenIter, bool> Added = InsideTokens.try_emplace(Anchor);
			const bool IsFirst = Added.second;
			std::vector<uint32_t> &Tokens = Added.first->second;
			if(IsFirst) for(const NodeId Child : Syntax.GetChildrenOf(Anchor)) if(!Syntax.IsNamed(Child) && Spans[Child].IsPlaced) {
				Tokens.push_back(Spans[Child].Start);
			}
			// トークンの前か，トークンが無い事の返戻
			return Tokens.empty() ?
			std::nullopt :
			std::optional<uint32_t>(Tokens[std::min<size_t>(Record.Placement.TokensBefore, Tokens.size() - 1)]);
		}
		// アンカーの後に同じ行で続く同じ親の無名のトークンを飛ばした後（閉トークンと行末の前で止める）
		if(Record.Placement.TokensBefore) {
			std::optional<uint32_t> RunEnd;
			uint16_t Taken = 0;
			for(
				NodeId Next = Syntax.GetNextSibling(Anchor);
				Next != NoNode && Taken < Record.Placement.TokensBefore;
				Next = Syntax.GetNextSibling(Next)
			) {
				if(!Spans[Next].IsPlaced) continue;
				if(
					Syntax.IsNamed(Next) || Syntax.Is(Next, Role::CloserToken) || Spans[Next].StartFragment != Span.EndFragment ||
					!IsFollowedByCode(Code, Spans[Next].End)
				) break;
				RunEnd = Spans[Next].End;
				++Taken;
			}
			// 飛ばしたトークンの後の返戻
			if(RunEnd) return RunEnd;
		}
		// アンカーの後に同じ行のコードが続く場合はアンカーの直後（空の括弧が続けば其の中）
		if(IsFollowedByCode(Code, Span.End)) {
			const uint32_t After = GetContentFrom(Code, Span.End);
			const CodePiece *const Next = Code.GetPieceAt(After);
			const bool IsEmptyParens = HasShape(Next, CodePiece::StartsEmptyParens) ||
			HasShape(Next, CodePiece::IsOpenParen) && HasShape(Code.GetPieceAt(Next->End), CodePiece::StartsCloseParen);
			// アンカーの直後か空の括弧の中の返戻
			return IsEmptyParens ? After + 1 : Span.End;
		}
		// アンカーの最後のトークン（文字列等のテキストは１つのトークン）
		NodeId Last = Anchor;
		while(true) {
			const uint32_t ChildCount = Syntax.GetChildCount(Last);
			if(!ChildCount || Syntax.Is(Last, Role::LiteralInterior)) break;
			NodeId Child = NoNode;
			for(uint32_t Idx = ChildCount; Idx-- && Child == NoNode;) {
				if(const NodeId Candidate = Syntax.GetChild(Last, Idx); Spans[Candidate].IsPlaced) Child = Candidate;
			}
			if(Child == NoNode) break;
			Last = Child;
		}
		// 最後のトークンが同じ行のコードの後のアンカーを終える区切・閉じのトークンか，アンカーの中に他のコメントが有れば其のトークンの前
		if(const NodeSpan &LastSpan = Spans[Last]; Last != Anchor && LastSpan.Start > Span.Start && IsAfterCode(Code, LastSpan.Start)) {
			// 其のトークンの前の返戻
			if(
				const bool IsTerminator = !Syntax.IsNamed(Last) && (
					Syntax.IsTokenRole(Last, Role::StmtTerminatorToken) || Syntax.IsTokenRole(Last, Role::CommaToken) ||
					Syntax.IsTokenRole(Last, Role::ClosingParenToken) || Syntax.IsTokenRole(Last, Role::ClosingSquareToken) ||
					Syntax.IsTokenRole(Last, Role::ClosingBraceToken)
				);
				HasInner || IsTerminator
			) return LastSpan.Start;
		}
		// アンカーの直前か，アンカーの中に他のコメントが有り順を保てない事の返戻
		return HasInner || Anchor == Syntax.GetRoot() ? std::nullopt : std::optional<uint32_t>(Span.Start);
	}

	/**
	 * テキストの位置の直前が空の波括弧かの判定関数
	 * 計算量：トークン片数 P に対し O(log(P + 1))
	 * @param Code 変更不能なコードの出力
	 * @param Offset テキストの位置
	 * @return 位置の前の２字が `{}` なら true
	 */
	bool CommentPlacer::IsAfterEmptyBraces(const LaidOutCode &Code, const uint32_t Offset) {
		// 位置の直前のトークン片の取得
		const CodePiece *const Last = Offset ? GetPieceCovering(Code, Offset - 1) : nullptr;
		// 位置で終わるトークン片の無い事の返戻
		if(!Last || Last->End != Offset) return false;
		const CodePiece *const Open = Last->Start ? GetPieceCovering(Code, Last->Start - 1) : nullptr;
		// 開波括弧のトークン片に密着した閉波括弧だけのトークン片の返戻
		return Last->End - Last->Start == 1 && Last->Shapes & CodePiece::StartsClosingBrace && Open && Open->End == Last->Start &&
		Open->Shapes & CodePiece::EndsOpeningBrace;
	}

	/**
	 * アンカーの直前に続く無名のトークンの並びの始まりの取得関数
	 * 原文でコメントとアンカーの間に在ったトークンの数だけ遡り，閉括弧・名前付のノードとアンカーを含まないノードのトークンで止める
	 * 計算量：辿る祖先・兄弟の数 T に対し O(T + 1)（出力に現れない兄弟も数える）
	 * @param Code 変更不能なコードの出力
	 * @param Syntax 読取専用の期待構文
	 * @param Anchor アンカーのノード
	 * @param Limit 遡るトークンの数の上限
	 * @return 遡ったトークンの始まり（遡れなければアンカーの始まり）
	 * @throws FormatLimitExceeded 構文の照会・走査が作業予算を超える場合
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 */
	uint32_t CommentPlacer::TokenRunStart(
		const LaidOutCode &Code,
		const SyntaxModel &Syntax,
		const NodeId Anchor,
		const uint16_t Limit
	) {
		// アンカーの位置と先行トークン数の取得
		const std::vector<NodeSpan> &Spans = Code.GetAnchors().Nodes;
		uint32_t Start = Spans[Anchor].Start;
		uint16_t Taken = 0;
		for(NodeId Cur = Anchor; Taken < Limit;) {
			NodeId Prev = Syntax.GetPrevSibling(Cur);
			while(Prev == NoNode) {
				const NodeId Parent = Syntax.GetParentOf(Cur);
				if(Parent == NoNode) break;
				Cur = Parent;
				Prev = Syntax.GetPrevSibling(Cur);
			}
			// 出力に現れないトークンの読飛
			while(Prev != NoNode && !Spans[Prev].IsPlaced && !Syntax.IsNamed(Prev)) Prev = Syntax.GetPrevSibling(Prev);
			if(
				Prev == NoNode || Syntax.IsNamed(Prev) || Syntax.GetChildCount(Prev) || Syntax.IsTokenRole(Prev, Role::ClosingParenToken) ||
				Syntax.IsTokenRole(Prev, Role::ClosingSquareToken) || Syntax.IsTokenRole(Prev, Role::ClosingBraceToken)
			) break;
			Start = Spans[Prev].Start;
			Cur = Prev;
			++Taken;
		}
		// 遡ったトークンの始まりの返戻
		return Start;
	}

	/**
	 * 文中コメントを置く行頭の後の文をパーサーが読めるかの判定関数（整形仕様 7.7）
	 * tree-sitter-swift は文を終えた改行の次の行の頭に在るブロックコメントの後の文を読めない為，Swift では其処へ置かない
	 * 前に同じ行のコードが在る所，演算子・閉トークンで前の式へ続く所，節の見出の前，文の並びの外，前のトークンが文を終えない所は読める
	 * 計算量：辿る祖先の数 H，断片数 F，トークン片数 P に対し O(H + log(F + 1) + log(P + 1))
	 * @param Code 変更不能なコードの出力
	 * @param Syntax 読取専用の期待構文
	 * @param Anchor アンカー
	 * @param At 置場のテキストの位置
	 * @return 置いても読めるなら true
	 * @throws FormatLimitExceeded 構文の照会・走査が作業予算を超える場合
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 */
	bool CommentPlacer::IsStmtHeadReadable(
		const LaidOutCode &Code,
		const SyntaxModel &Syntax,
		const NodeId Anchor,
		const uint32_t At
	) {
		// 改行で文を終えない言語と，行の途中と，演算子・閉トークンで前の式へ続く所の返戻
		if(
			!Syntax.GetRules().Notation.CanBlockCommentInterruptStmtHead || IsAfterCode(Code, At) ||
			HasShape(Code.GetPieceAt(At), CodePiece::StartsOperator | CodePiece::StartsClosingBracket)
		) return true;
		const std::vector<NodeSpan> &Spans = Code.GetAnchors().Nodes;
		// 位置から始まる祖先を辿った最初の位置より前から始まる祖先
		NodeId Up = Anchor;
		while(Up != NoNode && Spans[Up].IsPlaced && static_cast<uint64_t>(Spans[Up].Start) >= At) {
			// 節の見出で始まる所の返戻
			if(Syntax.Is(Up, Role::SectionHeader)) return true;
			Up = Syntax.GetParentOf(Up);
		}
		// 文の並びの外の返戻
		if(Up == NoNode || !Syntax.Is(Up, Role::StmtHost)) return true;
		const CodePiece *const Prev = GetPieceBefore(Code, At);
		// 前のトークンが無いか文を終えない（演算子・開括弧・区切の後で，後置の増減の後でない）事の返戻
		return !Prev || Prev->Shapes & CodePiece::EndsContinuation;
	}

	/**
	 * 独立・行末の通常コメントの規約の行形式のテキストの取得関数（整形仕様 7.6 の記法の選択）
	 * 一般の囲み，ドキュメントでない位置のドキュメントの記法，採用記法と異なる一般の行記法（PHP の `#`）の本文の論理行を採用する行記法で合成し，開始記号・終端記号だけの行を落とす
	 * 採用記法の行コメントと埋込ドキュメントの独立した記録も，整形仕様 7.3 で正規化した論理行を行コメントで保てない物は論理行毎に記法を選ぶ
	 * 行コメントで保てない論理行は同じ種別で合法な囲み（一般の囲み，無い文法は独立行の埋込ドキュメント）へ移し，合法な囲みの無い物は整形仕様 7.7 に委ねる
	 * 揃える記法で記号の一部と為る頭（前のメンバのドキュメントの頭）だけを密着させ，其の他の記号で始まる本文は区切空白の後へ置く
	 * 揃えた行が別の記法・処理系の登録形に為る論理行は，密着を外して避け，避けれなければ行コメントで保てない行とする
	 * 計算量：元記録と出力本文の長さの和 N，記法の登録語数 W に対し O((W + 1)(N + 1))
	 * @param Rules 投影済の記法
	 * @param Record コメントの記録
	 * @param Text 正規化した出力記法の本文
	 * @param Composed 配置候補の合成済のテキスト
	 * @param IsDoc ドキュメントか（ドキュメントの二重星の囲みは揃えない）
	 * @return 行形式のテキスト（既に行形式で保てる記録と行形式で本文を保てない１つの囲みの記録は空）
	 * @throws FormatLimitExceeded 記録が記法の定義を持たない場合（フォーマッターの不具合）
	 * @throws std::bad_alloc テキスト又は安全判定のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	std::string CommentPlacer::LineFormOf(
		const NotationRules &Rules,
		const CommentRecord &Record,
		const std::string_view Text,
		const std::string_view Composed,
		const bool IsDoc
	) {
		// 埋込ドキュメントの枠を１つに保ち，行記法へ変えない事の返戻
		if(
			Record.Notation == CommentNotation::RubyEmbeddedDocument &&
			CommentRepository::GetOutputNotationOf(Record) == CommentNotation::RubyEmbeddedDocument
		) return {};
		// 採用する行記法で置く記録の種類（グループを纏める記法と同じ定義）
		const CommentRepository::LinePlacement Placement = CommentRepository::GetLinePlacementOf(Record, IsDoc);
		// 揃えない記録の返戻
		if(Placement == CommentRepository::LinePlacement::None) return {};
		// 通常の独立・行末コメントの採用記法
		const CommentOutputForm *const Plain = Rules.GetAdoptedFormOf(CommentUse::Plain);
		const bool IsLineRecord = Placement == CommentRepository::LinePlacement::LineRecord;
		const bool IsAdoptedLine = IsLineRecord && Record.Notation == Plain->Notation;
		const std::string_view Input = IsAdoptedLine ? Composed : Text;
		// 内容の有る論理行の本文
		std::vector<std::string_view> Lines, Bodies;
		for(size_t Cursor = 0; Cursor <= Input.size();) {
			const size_t End = std::min(Input.find('\n', Cursor), Input.size());
			std::string_view Body = Input.substr(Cursor, End - Cursor);
			Cursor = End + 1;
			if(IsAdoptedLine) {
				Lines.push_back(Body);
				Body.remove_prefix(CommentRepository::GetOpenerOf(Plain->Notation).size());
				if(Body.starts_with(' ')) Body.remove_prefix(1);
			}
			while(!Body.empty() && (Body.back() == ' ' || Body.back() == '\t')) Body.remove_suffix(1);
			// 囲みの開始記号・終端記号だけの行の読飛
			if(!IsAdoptedLine && Body.empty()) continue;
			Bodies.push_back(Body);
		}
		// 本文を持たない囲みは行形式にすると本文を失う為に揃えない事の返戻
		if(Bodies.empty()) return {};
		// 論理行毎の行コメントのテキストと，行コメントで本文を保てるか
		std::vector<std::string> Generated;
		if(!IsAdoptedLine) Generated.reserve(Bodies.size());
		std::vector<uint8_t> IsLineSafe;
		for(size_t Idx = 0; Idx < Bodies.size(); ++Idx) {
			if(!IsAdoptedLine) {
				CommentRecord Part { std::string(Bodies[Idx]), Record.Placement };
				Part.Kind = Record.Kind;
				Part.Notation = Plain->Notation;
				Part.Format.Rules = Record.Format.Rules;
				// 揃える行記法で記号の一部と為る頭だけを密着させる
				Part.Format.ShouldAttachFirstLine = !CommentRepository::GetSymbolHeadOf(Part, Bodies[Idx]).empty();
				Generated.push_back(CommentRepository::Compose(Part));
				Lines.push_back(Generated.back());
			}
			IsLineSafe.push_back(
				CommentRepository::CanHoldInLineComment(Rules, Bodies[Idx]) &&
				CommentRepository::IsPlainLineText(Rules, Lines[Idx], Plain->Notation)
			);
		}
		const bool HasUnsafe = std::find(IsLineSafe.begin(), IsLineSafe.end(), 0) != IsLineSafe.end();
		// 全ての論理行を行コメントで保てる行記法の記録は其の儘とする事の返戻
		if(IsAdoptedLine && !HasUnsafe) return {};
		// 論理行毎に記法を選ぶ記録（一般の囲みの閉じた囲みの並びは次の整形でも論理行毎の別のコメントとして読む）
		const bool IsSeq = IsLineRecord || Placement == CommentRepository::LinePlacement::GeneralBlock &&
		(Record.Kind == CommentKind::Trailing || !Record.Format.ShouldSeparateLogicalLines);
		// 行コメントへ揃える最初の論理行（行末では行コメントの後のテキストが其の本文へ続く為，行コメントで保てる最後の連続だけを揃える）
		size_t First = 0;
		if(IsSeq && Record.Kind == CommentKind::Trailing) {
			First = Bodies.size();
			while(First && IsLineSafe[First - 1]) --First;
		}
		// 一括で揃える囲みに保てない行が有るか，揃える論理行が無い場合の空テキストの返戻（行記法の記録は全行を囲みへ移す）
		if(
			!IsSeq && HasUnsafe || First == Bodies.size() || IsSeq && !IsLineRecord && Record.Kind != CommentKind::Trailing &&
			std::find(IsLineSafe.begin(), IsLineSafe.end(), 1) == IsLineSafe.end()
		) return {};
		// 行コメントで保てない論理行を置く同じ種別で合法な囲み（一般の囲み，無い文法は独立行の埋込ドキュメント）
		const CommentNotation Enclosing =
		Rules.IsAvailable(CommentNotation::Block) ? CommentNotation::Block : CommentNotation::RubyEmbeddedDocument;
		// 合法な囲みを持たない保てない記録は其の記法の儘とする事の返戻（保持の違反は最終検証が拒否する，整形仕様 8.3）
		if(
			HasUnsafe &&
			(!Rules.IsAvailable(Enclosing) || !CommentRepository::IsBlockNotation(Enclosing) && Record.Kind != CommentKind::OwnLine)
		) return {};
		// 論理行毎の行記法と囲記法の合成
		std::string Output;
		for(size_t Idx = 0; Idx < Bodies.size();) {
			if(!Output.empty()) Output += '\n';
			// 行コメントで保てる論理行の行コメントへの揃え
			if(Idx >= First && IsLineSafe[Idx]) {
				Output += Lines[Idx++];
				continue;
			}
			// 行コメントで保てない論理行は論理行毎に閉じた囲みとする（投影は埋込ドキュメントも論理行毎の記録に分ける為，次の整形でも同じ形に為る）
			CommentRecord Part { std::string(Bodies[Idx]), Record.Placement };
			Part.Kind = Record.Kind;
			Part.Notation = Enclosing;
			Part.Format.Rules = Record.Format.Rules;
			// 移す囲みで記号の一部と為る頭だけを密着させる
			Part.Format.ShouldAttachFirstLine = !CommentRepository::GetSymbolHeadOf(Part, Bodies[Idx]).empty();
			++Idx;
			Output += CommentRepository::Compose(Part);
		}
		// 行形式のテキストの返戻
		return Output;
	}

	/**
	 * 行末へ積む形への変換関数（整形仕様 7.6）
	 * 文毎に分けた行末コメントの物理行が其々閉じたコメントなら，原文の順に空白を挟んで１行へ並べる
	 * 行コメントの後の物は其の本文へ続くが，文毎に記号を持つ為に文の区切を保つ
	 * 計算量：本文の長さ N に対し O(N)
	 * @param Text コメント本文（複数行を含み得る）
	 * @return １行へ積んだ本文（閉じない囲みの行や空の行を含み積めない場合は空）
	 * @throws std::bad_alloc 連結するコメント本文のメモリを確保出来ない場合
	 * @throws std::length_error 連結するコメント本文が格納先の上限を超える場合
	 */
	std::string CommentPlacer::StackLineEnd(const std::string_view Text) {
		// 行末へ積む本文の格納先
		std::string Result;
		const std::string_view Opener = CommentRepository::GetOpenerOf(CommentNotation::Block);
		const std::string_view Closer = CommentRepository::GetCloserOf(CommentNotation::Block);
		for(size_t Pos = 0; Pos <= Text.size();) {
			// 物理行の範囲
			const size_t End = std::min(Text.find('\n', Pos), Text.size());
			std::string_view Line = Text.substr(Pos, End - Pos);
			Pos = End + 1;
			TextEdit::TrimView(Line);
			// 其の行で閉じる囲みの並びと，最後の行コメントだけを積める
			for(std::string_view Rest = Line; !Rest.empty();) {
				// 行末迄を本文とする行コメントでの論理行の終端
				if(IsLineCommentStart(Rest)) break;
				// 囲みで始まらない行は積めない事の返戻
				if(!Rest.starts_with(Opener)) return {};
				// 同じ行の閉じ
				const size_t Close = Rest.find(Closer, Opener.size());
				// 行を越える囲みは積めない事の返戻
				if(Close == std::string_view::npos) return {};
				Rest.remove_prefix(Close + Closer.size());
				TextEdit::TrimView(Rest);
			}
			// 空の行は積めない事の返戻
			if(Line.empty()) return {};
			if(!Result.empty()) Result += ' ';
			Result += Line;
		}
		// 積んだ本文の返戻
		return Result;
	}

	/**
	 * 配置枠の決定関数（整形仕様 7.6）
	 * アンカーのノードの出力上の範囲と，原文での種類・所属から配置枠を決める
	 * 独立コメントは原文で後に続いたトークンの在る行（無ければ後続のコード行）の前へ，其の行の深さで置く
	 * 行末コメントはアンカーの終わる行の行末へ置く
	 * 文中コメントは同じ構文のトークンの境界へ置く
	 * 計算量：ドキュメントの説明対象・配置対象の取得の費用 D，ノード数 V，断片数 F，トークン片数 P，記録数 C，記録と合成したテキストの長さの和 N，記法の登録語数 W，地の文毎にマーカーを付ける断片数の和 B，記録毎の構文・断片の走査量の和 T，二分探索の回数 Q に対し平均 O(D + V log(V + 1) + V log(F + 1) + F + B + T + Q log(V + F + P + 1) + C log²(C + 1) + (W + 1)(N + C))
	 * @param Code 変更不能なコードの出力
	 * @param Syntax 読取専用の期待構文（アンカーの所属を引く）
	 * @param Comments アンカーのノード毎のコメント
	 * @param OpaqueRanges 行形式へ揃えない領域（同じノード訪問で収集する）
	 * @param Diagnostics 呼出側が表示する脱落の診断の格納先
	 * @param Work 検証時の役割・親の照会数の格納先（製品の配置では `nullptr`）
	 * @return 断片・配置枠の種類・位置・原文順で整列したコメント
	 * @throws FormatLimitExceeded アンカーのノードが出力に現れない場合，埋込の式の要素が無い場合，又は記録が記法の定義を持たない場合（フォーマッターの不具合）
	 * @throws FormatLimitExceeded 保護するドキュメント・ディレクティブコメントがコード境界を持てない場合（其々の保持違反，整形仕様 7.7，8.3）
	 * @throws FormatLimitExceeded 構文の照会・走査が作業予算を超える場合
	 * @throws std::bad_alloc 配置候補又は祖先のインデックスのメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 */
	std::vector<CommentPlacer::Candidate> CommentPlacer::CollectCandidates(
		const LaidOutCode &Code,
		const SyntaxModel &Syntax,
		const CommentRepository &Comments,
		std::vector<std::pair<uint32_t, uint32_t>> &OpaqueRanges,
		std::vector<std::string> *const Diagnostics,
		StructureWorkAccount *const Work
	) {
		// 製品と検証ツールで同じ役割・親の照会を使い，検証時だけ構文の照会数を数える
		const auto Is = [&](const NodeId Node, const Role Wanted) -> bool {
			// 役割の照会と返戻
			if(Work) Work->Charge(1);
			return Syntax.Is(Node, Wanted);
		};
		const auto ParentOf = [&](const NodeId Node) -> NodeId {
			// 親の照会と返戻
			if(Work) Work->Charge(1);
			return Syntax.GetParentOf(Node);
		};
		// アンカーと配置対象の記録の取得
		const AnchorPositionMap &Anchors = Code.GetAnchors();
		const std::span<const CodeFragment> Fragments = Code.GetFragments();
		const NotationRules &Rules = Syntax.GetRules().Notation;
		const std::span<const CommentRecord> Records = Comments.GetRecords();
		OpaqueRanges.clear();
		// 配置する記録の無い空の候補列の返戻
		if(Records.empty()) return {};
		// 行末・文中のコメントの有無（其の種類だけが読むインデックスは有る時だけ作る）
		bool HasTrailing = false, HasInline = false, HasOwnLine = false, HasEmptyOwnLine = false;
		for(const CommentRecord &Record : Records) {
			HasTrailing = HasTrailing || Record.Kind == CommentKind::Trailing;
			HasInline = HasInline || Record.Kind == CommentKind::Inline;
			HasOwnLine = HasOwnLine || Record.Kind == CommentKind::OwnLine;
			HasEmptyOwnLine = HasEmptyOwnLine || Record.Kind == CommentKind::OwnLine && Record.Placement.IsEmptyBody;
		}
		// 行末のコメントだけが読む，本文へ取り込まない為の開きの行毎の要素のインデックスと，コードの閉じの出力順の位置表（コメント毎に構文全体を走査しない）
		std::unordered_map<uint32_t, std::vector<NodeId>> MarkupByLine;
		std::vector<std::pair<uint32_t, uint32_t>> CodeBoundaries;
		bool HasMarkup = false, HasKeyword = false, HasTopLevel = false, HasLiteral = false;
		for(NodeId Id = 0; Id < Anchors.Nodes.size(); ++Id) {
			if(Anchors.Nodes[Id].IsPlaced && Is(Id, Role::OpaqueLineText)) {
				OpaqueRanges.emplace_back(Anchors.Nodes[Id].Start, Anchors.Nodes[Id].End);
			}
			HasMarkup = HasMarkup || Is(Id, Role::MarkupElement) || Is(Id, Role::MarkupOpen) || Is(Id, Role::SelfClosingTag) ||
			Is(Id, Role::MarkupExpr);
			HasKeyword = HasKeyword || HasEmptyOwnLine && (Is(Id, Role::KeywordBlock) || Is(Id, Role::KeywordSection));
			HasTopLevel = HasTopLevel || HasOwnLine && Is(Id, Role::AdjacentDocSeq);
			HasLiteral = HasLiteral || HasTrailing && IsLiteralAnchor(Syntax, Id, Work);
			if(HasTrailing) {
				const NodeSpan &Span = Anchors.Nodes[Id];
				if(!Span.IsPlaced) continue;
				if(Is(Id, Role::MarkupElement)) MarkupByLine[Span.StartFragment].push_back(Id);
				if(Is(Id, Role::CodeBoundary)) CodeBoundaries.emplace_back(Span.Start, Span.StartFragment);
			}
		}
		std::sort(CodeBoundaries.begin(), CodeBoundaries.end());
		// 記録毎に祖先を辿らない為に１度の走査で求めるノード毎の表（整形仕様 9.2）
		// 最も近い埋込の式・語のブロックか節（自身を含む），隣接するドキュメントを取り込む並び（Go の最上位）の中の最上位の祖先（自身を含む），最も近い埋込の要素（自身を含む）
		std::vector<NodeId> NearestMarkup(HasMarkup ? Syntax.GetNodeCount() : 0, NoNode), NearestElement(NearestMarkup.size(), NoNode);
		// 文・メンバ本体の手前の最外の要素と，其の範囲の最上位ノードを除いた最外の要素
		std::vector<NodeId> OuterMarkup(HasMarkup && HasTrailing ? Syntax.GetNodeCount() : 0, NoNode);
		std::vector<NodeId> StmtMarkup(OuterMarkup.size(), NoNode);
		std::vector<NodeId> NearestKeyword(HasKeyword ? Syntax.GetNodeCount() : 0, NoNode);
		std::vector<NodeId> TopLevel(HasTopLevel ? Syntax.GetNodeCount() : 0, NoNode);
		// 宣言の初期値の先頭（其の内側から行前へ移る通常コメントのドキュメントへの誤取込を防ぐ）
		std::vector<NodeId> InitialValues(TopLevel.size(), NoNode);
		std::vector<NodeId> OuterLiteral(HasLiteral ? Syntax.GetNodeCount() : 0, NoNode);
		if(HasMarkup || HasKeyword || HasTopLevel || HasLiteral) {
			for(std::vector<NodeId> Pending { Syntax.GetRoot() }; !Pending.empty();) {
				const NodeId Node = Pending.back();
				Pending.pop_back();
				const NodeId Parent = ParentOf(Node);
				if(HasMarkup) {
					NearestMarkup[Node] = Is(Node, Role::MarkupExpr) ? Node : Parent == NoNode ? NoNode : NearestMarkup[Parent];
					NearestElement[Node] = Is(Node, Role::MarkupElement) || Is(Node, Role::MarkupOpen) || Is(Node, Role::SelfClosingTag) ?
					Node :
					Parent == NoNode ? NoNode : NearestElement[Parent];
				}
				if(!OuterMarkup.empty()) {
					// 親の確定した要約を共有し，所有範囲の境界で外側の要素を除く
					const bool IsBoundary = Is(Node, Role::StmtHost) || Is(Node, Role::MemberBody);
					const bool IsElement = Is(Node, Role::MarkupElement) || Is(Node, Role::MarkupOpen);
					if(!IsBoundary) {
						OuterMarkup[Node] = Parent == NoNode ? NoNode : OuterMarkup[Parent];
						if(OuterMarkup[Node] == NoNode && IsElement) OuterMarkup[Node] = Node;
					}
					// 埋込の式の経路は所有範囲の最上位ノード自体を探索に含めない
					if(Parent != NoNode && !Is(Parent, Role::StmtHost) && !Is(Parent, Role::MemberBody)) {
						StmtMarkup[Node] = StmtMarkup[Parent];
						if(StmtMarkup[Node] == NoNode && IsElement) StmtMarkup[Node] = Node;
					}
				}
				if(HasKeyword) {
					NearestKeyword[Node] =
					Is(Node, Role::KeywordBlock) || Is(Node, Role::KeywordSection) ? Node : Parent == NoNode ? NoNode : NearestKeyword[Parent];
				}
				if(HasTopLevel) {
					TopLevel[Node] = Is(Parent, Role::AdjacentDocSeq) ? Node : Parent == NoNode ? NoNode : TopLevel[Parent];
					InitialValues[Node] = Parent == NoNode ? NoNode : InitialValues[Parent];
					if(
						InitialValues[Node] == NoNode && !Is(TopLevel[Node], Role::DocumentedDef) && Syntax.HasField(Node, ChildRole::Val) &&
						Is(Parent, Role::DocumentableDecl)
					) InitialValues[Node] = Node;
				}
				if(HasLiteral) {
					OuterLiteral[Node] =
					Parent != NoNode && OuterLiteral[Parent] != NoNode ? OuterLiteral[Parent] : IsLiteralAnchor(Syntax, Node, Work) ? Node : NoNode;
				}
				for(const NodeId Child : Syntax.GetChildrenOf(Node)) Pending.push_back(Child);
			}
		}
		// 親が無い要素の照会も，埋込の式が無い結果として返す
		const auto MarkupOf = [&NearestMarkup](const NodeId Node) -> NodeId {
			// 無効なノード番号を表の添字にしない照会の返戻
			return Node == NoNode ? NoNode : NearestMarkup[Node];
		};
		// 要素の本文・タグの外の行頭の取得（式の中と属性の中で同じ配置位置を使う）
		const auto ElementLine = [&](NodeId Markup) -> uint32_t {
			// 要素の役割が欠けた構文を，配置位置の範囲外参照にしない
			if(Markup == NoNode) throw FormatLimitExceeded("a markup expression has no containing element", FailureReason::FormatterDefect);
			// 要素の行頭迄のラッパーと断片の探索
			while(true) {
				NodeId Front = Markup;
				while(true) {
					const NodeId Parent = ParentOf(Front);
					if(Parent == NoNode || Anchors.Nodes[Parent].StartFragment != Anchors.Nodes[Markup].StartFragment) break;
					Front = Parent;
				}
				const NodeId Ctx = ParentOf(Front), Element = Ctx == NoNode ? NoNode : NearestElement[Ctx];
				// 埋込の式か要素の外のコードの行頭に達した場合の打切
				if(Element == NoNode || NearestMarkup[Ctx] != NoNode && MarkupOf(ParentOf(Element)) != NearestMarkup[Ctx]) break;
				Markup = Element;
			}
			// 要素の外に在る行頭の返戻
			return Anchors.Nodes[Markup].StartFragment;
		};
		// 文中コメントが共有するアンカーの無名トークンの位置
		std::unordered_map<NodeId, std::vector<uint32_t>> InsideTokens;
		// 記録毎のドキュメントの説明対象と，確定した配置でドキュメントとして置く物（ドキュメントと認識しない記録と置かない記録は無い，整形仕様 7.4，7.6）
		const std::span<const NodeId> Recognized = Comments.GetDocTargets(), Docs = Comments.GetPlacementTargets();
		// アンカーの中のノードに紐付いた他のコメントの有るアンカー（末尾の物と，アンカーの頭でない先行の物，文中のコメントだけが読む）
		std::vector<uint8_t> HasInner(HasInline ? Anchors.Nodes.size() : 0, 0);
		// コードの出力を持たない根に結び付いたコメントだけのファイルの物か（ドキュメントの独立した出力位置を用いる）
		const auto IsDocComment = [&](const CommentRecord &Record) -> bool {
			// 出力に現れない根のアンカーかの返戻
			return Record.Anchor == Syntax.GetRoot() && !Anchors.Nodes[Record.Anchor].IsPlaced;
		};
		// ノード毎の自身へ紐付いた記録の要約（先行でない物が有るか，先行の物のアンカーの最も後の始まり）
		std::vector<uint8_t> HasOwnAfter(HasInner.size(), 0);
		std::vector<int64_t> OwnBeforeStart(HasInner.size(), -1);
		for(const CommentRecord &Record : Records) {
			if(IsDocComment(Record)) continue;
			if(!Anchors.Nodes[Record.Anchor].IsPlaced) {
				throw FormatLimitExceeded("a comment anchor has no output position", FailureReason::FormatterDefect);
			}
			if(!HasInline || Record.Placement.IsRemovedCaseBody && Record.Kind == CommentKind::Inline) continue;
			if(Record.Placement.Side != CommentPlacement::Before) HasOwnAfter[Record.Anchor] = 1;
			else OwnBeforeStart[Record.Anchor] = std::max<int64_t>(OwnBeforeStart[Record.Anchor], Anchors.Nodes[Record.Anchor].Start);
		}
		// 子孫の要約の後順での集約と，先行でない物か，祖先より後から始まるアンカーの先行の物を子孫に持つ祖先へのマーカーの設定（記録毎に祖先を辿らない，整形仕様 9.2）
		std::vector<uint8_t> HasDescendantAfter(HasInner.size(), 0);
		std::vector<int64_t> DescendantBeforeStart(HasInner.size(), -1);
		if(HasInline) for(std::vector<std::pair<NodeId, bool>> Pending { { Syntax.GetRoot(), false } }; !Pending.empty();) {
			const std::pair<NodeId, bool> Step = Pending.back();
			const NodeId Node = Step.first;
			Pending.pop_back();
			if(!Step.second) {
				Pending.emplace_back(Node, true);
				for(const NodeId Child : Syntax.GetChildrenOf(Node)) Pending.emplace_back(Child, false);
				continue;
			}
			for(const NodeId Child : Syntax.GetChildrenOf(Node)) {
				HasDescendantAfter[Node] = HasDescendantAfter[Node] || HasOwnAfter[Child] || HasDescendantAfter[Child];
				DescendantBeforeStart[Node] = std::max({ DescendantBeforeStart[Node], OwnBeforeStart[Child], DescendantBeforeStart[Child] });
			}
			HasInner[Node] = HasDescendantAfter[Node] || DescendantBeforeStart[Node] > static_cast<int64_t>(Anchors.Nodes[Node].Start);
		}
		// 内容の無い説明のコメントかの判定
		const auto IsContentless = [&Records](const size_t Idx) -> bool {
			// 空の本文の説明のコメントかの返戻
			return Records[Idx].Role == CommentRole::Presentation && Records[Idx].Text.find_first_not_of(" \t\n") == std::string::npos;
		};
		std::vector<Candidate> Candidates;
		// 断片毎に其の終わりの改行が地の文の中に在るか（使う時に初めて作る）
		std::optional<std::vector<uint8_t>> BreaksInText;
		for(size_t RecordIdx = 0; RecordIdx < Records.size(); ++RecordIdx) {
			const CommentRecord &Record = Records[RecordIdx];
			// 所属確定で論理行から除いた空の縁の読飛
			if(Record.IsEmbeddedEdge) continue;
			const bool IsFileDoc = IsDocComment(Record);
			Candidate Found;
			Found.Record = &Record;
			Found.IsVerbatim = Record.Role == CommentRole::Directive;
			Found.IsRequired = Found.IsVerbatim || Recognized[RecordIdx] != NoNode && (Record.Kind == CommentKind::OwnLine || IsFileDoc);
			if(Record.Directive && Record.Directive->State == DirectiveState::Active) switch(Record.Directive->Definition) {
			case DirectiveDefinitionId::GoGenerate:
			case DirectiveDefinitionId::LinePos:
				Found.IsLineHeadRequired = true;
				break;
			}
			CommentRecord Output = CommentRepository::GetOutputFormOf(Record);
			if(IsFileDoc) {
				// 内容の無い通常コメントの読飛（整形仕様 7.7）
				if(IsContentless(RecordIdx)) continue;
				// ドキュメントの末尾（コードの断片の後）の独立した行
				Found.Text = CommentRepository::Compose(Output);
				if(Record.Kind != CommentKind::OwnLine) {
					ReportDrop(
						Found.Text.size(),
						"no code boundary for an inline or line-end comment",
						Found.IsRequired,
						Found.IsVerbatim,
						Diagnostics
					);
					continue;
				}
				Found.Where = CommentSlotKind::DocEnd;
				Found.Fragment = static_cast<uint32_t>(Fragments.size());
				Candidates.push_back(std::move(Found));
				continue;
			}
			const NodeSpan &Span = Anchors.Nodes[Record.Anchor];
			// 複数行のリテラルに属する行末の行先は，通常の配置と JSX の配置で同じ値を読む
			std::optional<uint32_t> LiteralEnd;
			if(
				const NodeId Literal = HasLiteral && Record.Kind == CommentKind::Trailing ? OuterLiteral[Record.Anchor] : NoNode;
				Literal != NoNode
			) {
				const NodeSpan &LiteralSpan = Anchors.Nodes[Literal];
				if(LiteralSpan.StartFragment < LiteralSpan.EndFragment) LiteralEnd = LiteralSpan.EndFragment;
			}
			// 空の case 本体で後続トークンを失った文中コメントの，種別を保った除去
			if(Record.Placement.IsRemovedCaseBody && Record.Kind == CommentKind::Inline) {
				ReportDrop(0, "no mid-line position in an empty case body", Found.IsRequired, Found.IsVerbatim, Diagnostics, &Output);
				continue;
			}
			// ドキュメントと認識する記録は記法と移譲済のアンカーの宣言への所属の双方から決め（整形仕様 7.4），説明対象の前に独立行を持てない物は通常コメントとして描く（整形仕様 7.6）
			const bool IsPlacedDoc = Docs[RecordIdx] != NoNode;
			Found.IsDoc = IsPlacedDoc;
			Output.Format.HasSummaryPlaceholder = Output.Format.HasSummaryPlaceholder && IsPlacedDoc;
			// 概要を記述する為の空の論理行以外の空のコメントの読飛
			if(IsContentless(RecordIdx) && (!Found.IsDoc || !Record.Format.HasSummaryPlaceholder)) continue;
			if(Record.Role == CommentRole::Presentation && !IsPlacedDoc && !CommentRepository::IsMemberDoc(Rules, Record)) {
				// ドキュメントでないドキュメントの囲みの記法は，行形式で保てない時に選ぶ同じ種別の囲みを一般の囲みとする（整形仕様 7.6）
				if(CommentRepository::IsBlockNotation(Output.Notation) && Rules.GetDocFormOf(Output.Notation)) {
					Output.Notation = CommentNotation::Block;
				}
			}
			Found.Text = CommentRepository::Compose(Output);
			const bool IsBefore = Record.Placement.Side == CommentPlacement::Before;
			// 空の式への文中の囲みの復帰と，其の他の種別を保った本文の外の配置枠への移動（整形仕様 7.7）
			NodeId Expr = HasMarkup ? NearestMarkup[Record.Anchor] : NoNode;
			// 埋込の式の祖先だけを辿り，アンカーの行を内側に含む最も近い物を選ぶ（要素の本文を跨いで外の式へ遡らず，其の本文の中の物は要素の経路で置く）
			if(Record.Kind == CommentKind::OwnLine) for(NodeId Up = Expr; Up != NoNode;) {
				if(Anchors.Nodes[Up].StartFragment < Span.StartFragment && Span.StartFragment < Anchors.Nodes[Up].EndFragment) {
					Expr = Up;
					break;
				}
				const NodeId Parent = ParentOf(Up), Element = Parent == NoNode ? NoNode : NearestElement[Parent];
				Up =
				Parent == NoNode || Element != NoNode && MarkupOf(ParentOf(Element)) == NearestMarkup[Parent] ? NoNode : NearestMarkup[Parent];
			}
			if(
				Expr != NoNode && Record.Role != CommentRole::Directive && !(
					Record.Kind == CommentKind::OwnLine && Anchors.Nodes[Expr].StartFragment < Span.StartFragment &&
					Span.StartFragment < Anchors.Nodes[Expr].EndFragment
				)
			) {
				const NodeSpan &ExprSpan = Anchors.Nodes[Expr];
				if(
					!Syntax.GetNamedChildCount(Expr) && Record.Kind == CommentKind::Inline && CommentRepository::IsBlockNotation(Record.Notation)
				) {
					// 空の式の中の文中の囲みの，種別と記法を保った配置（本文へ混ぜない JSX のコメントの形）
					Found.Text = CommentRepository::Compose(Record);
					Found.Where = CommentSlotKind::Inline;
					Found.Offset = ExprSpan.Start + 1;
					Found.Fragment = GetFragmentAt(Code, Found.Offset);
				} else if(Record.Kind == CommentKind::Inline && !IsLineCommentStart(Found.Text)) {
					Found.Where = CommentSlotKind::Inline;
					Found.Offset =
					IsBefore ? Span.Start : InlineAfter(Code, Syntax, Record, HasInner[Record.Anchor], InsideTokens).value_or(ExprSpan.End - 1);
					Found.Fragment = GetFragmentAt(Code, Found.Offset);
				} else if(Record.Kind == CommentKind::Trailing) {
					// 行末の物は所有範囲内の最外の要素の終わる行へ置き，要素が無ければ式の終わる行を使う
					const NodeId Markup = StmtMarkup[Expr] == NoNode ? Expr : StmtMarkup[Expr];
					Found.Where = CommentSlotKind::LineEnd;
					Found.Fragment = Anchors.Nodes[Markup].EndFragment;
					if(LiteralEnd) Found.Fragment = std::max(Found.Fragment, *LiteralEnd);
				} else {
					// 独立した物は式を含む要素の内，始まる行の頭（其の行で始まる最も外のノードの親の中）が要素の本文・タグの外に在る最も内の物の其の行の前へ置く（整形仕様 7.6, 7.7）
					Found.Where = CommentSlotKind::BeforeLine;
					Found.Fragment = ElementLine(NearestElement[Expr]);
					Found.Tabs = Anchors.FragmentTabs[Found.Fragment];
				}
				Candidates.push_back(std::move(Found));
				continue;
			}
			// 式のラッパーを祖先に持たないタグ内の独立コメントの，要素の本文・タグの外への配置
			if(
				HasMarkup && NearestElement[Record.Anchor] != NoNode && Record.Role != CommentRole::Directive &&
				(Expr == NoNode || NearestMarkup[NearestElement[Record.Anchor]] == Expr) &&
				(Record.Kind == CommentKind::OwnLine || Record.Kind == CommentKind::Inline && IsLineCommentStart(Found.Text))
			) {
				Found.Where = CommentSlotKind::BeforeLine;
				Found.Fragment = ElementLine(NearestElement[Record.Anchor]);
				Found.Tabs = Anchors.FragmentTabs[Found.Fragment];
				Candidates.push_back(std::move(Found));
				continue;
			}
			switch(Record.Kind) {
			case CommentKind::OwnLine:
				if(HasKeyword && NearestKeyword[Record.Anchor] != NoNode && Record.Placement.IsEmptyBody && !IsBefore) {
					// 語で閉じる構文の空の本体の物は，原文で其の後に続いたトークン（アンカーの中の後の節か閉じの語，無ければ構文の順でアンカーの後に続く物）の在る行の前へ置く（整形仕様 7.6）
					// アンカーの中は語を出さない本体 (`do` / `then`) の中も前順に探す
					NodeId Successor = NoNode;
					const std::span<const NodeId> Children = Syntax.GetChildrenOf(Record.Anchor);
					for(std::vector<NodeId> Pending(Children.rbegin(), Children.rend()); Successor == NoNode && !Pending.empty();) {
						const NodeId Node = Pending.back();
						Pending.pop_back();
						if(Is(Node, Role::TransparentSection)) {
							const std::span<const NodeId> Inner = Syntax.GetChildrenOf(Node);
							Pending.insert(Pending.end(), Inner.rbegin(), Inner.rend());
						} else if(Is(Node, Role::KeywordSection) || Is(Node, Role::CloserToken)) Successor = Node;
					}
					if(Successor == NoNode) Successor = Syntax.GetFollowingOf(Record.Anchor);
					Found.Where = CommentSlotKind::OwnLine;
					Found.Fragment = Span.EndFragment;
					if(Successor != NoNode && Anchors.Nodes[Successor].IsPlaced) {
						Found.Where = CommentSlotKind::BeforeLine;
						Found.Fragment = Anchors.Nodes[Successor].StartFragment;
					}
					break;
				}
				// 独立した行（アンカーが行内へ移っても行内コメントにしない）
				if(IsBefore && IsPlacedDoc) {
					// 外側の説明は宣言の直前，内側の説明は本体のアンカーの直前へ置く
					const NodeId Target =
					Rules.IsInnerDoc(Record.Notation) ? Record.Anchor : SignatureExtractor::GetLeadOf(Syntax, Docs[RecordIdx]);
					Found.Where = CommentSlotKind::BeforeLine;
					Found.Fragment = Anchors.Nodes[Target].StartFragment;
				} else if(IsBefore) {
					// 原文でコメントの後に在ったトークンの位置の行前への配置（畳んで空に為る本体の中の物はアンカーの頭でなく終わりの `{}` の位置）
					// 閉括弧・節の見出で始まる行も例外としない（整形仕様 7.6）
					const uint32_t Dst = Record.Placement.IsEmptyBody && IsAfterEmptyBraces(Code, Span.End) ?
					Span.End - 2 :
					Record.Placement.TokensBefore ? TokenRunStart(Code, Syntax, Record.Anchor, Record.Placement.TokensBefore) : Span.Start;
					Found.Where = CommentSlotKind::BeforeLine;
					Found.Fragment = GetFragmentAt(Code, Dst);
				} else {
					Found.Where = CommentSlotKind::OwnLine;
					Found.Fragment = Span.EndFragment;
					// 原文でコメントの後に在った閉じ・節（構文の順でアンカーの後に続く物）がアンカーの終わる行に在る物（`} else {` の `else`，畳んだ `f(1);` の `)`）は，行の後へ置くと後続の別の文の物に為る為，其の行の前へ置く（整形仕様 7.6）
					// 閉じでない無名のトークンは後置へ変えた構文の見出（`b if a` の `if`）で原文ではコメントより前に在った為に除く
					// 整形で加えた部分木（補った返戻文等）は原文に無い為に飛ばし，其の後の原文の境界の在る行の前へ置く
					// 後に原文の境界が無ければ，飛ばした部分木の後の独立した行とする（インデントで閉じる本体の末尾の物）
					NodeId Successor = Syntax.GetFollowingOf(Record.Anchor), Skipped = NoNode;
					for(; Successor != NoNode && Syntax.IsInsertedNode(Successor); Successor = Syntax.GetFollowingOf(Successor)) {
						Skipped = Successor;
					}
					if(
						Successor != NoNode && (Syntax.IsNamed(Successor) || Is(Successor, Role::CloserToken)) && Anchors.Nodes[Successor].IsPlaced &&
						(Skipped != NoNode || Anchors.Nodes[Successor].StartFragment == Span.EndFragment)
					) {
						Found.Where = CommentSlotKind::BeforeLine;
						Found.Fragment = Anchors.Nodes[Successor].StartFragment;
					} else if(Skipped != NoNode && Anchors.Nodes[Skipped].IsPlaced) Found.Fragment = Anchors.Nodes[Skipped].EndFragment;
				}
				break;
			case CommentKind::Trailing:
				Found.Where = CommentSlotKind::LineEnd;
				Found.Fragment = Span.EndFragment;
				if(IsBefore) {
					// 原文でアンカーとの間に在ったトークンを戻した位置の前の行末（其の位置が行頭なら其の前のコードの行，行の途中なら其の行）
					const uint32_t Run =
					Record.Placement.TokensBefore ? TokenRunStart(Code, Syntax, Record.Anchor, Record.Placement.TokensBefore) : Span.Start;
					Found.Fragment = GetFragmentAt(Code, Run);
					if(!IsAfterCode(Code, Run)) {
						uint32_t Prev = Found.Fragment;
						while(Prev && Fragments[Prev - 1].IsBlank) --Prev;
						if(!Prev) {
							ReportDrop(Found.Text.size(), "no line end with code", Found.IsRequired, Found.IsVerbatim, Diagnostics);
							continue;
						}
						Found.Fragment = Prev - 1;
					}
				}
				break;
			case CommentKind::Inline:
				// 同じ構文のトークンの境界（アンカーの前は原文でコメントとアンカーの間に在ったトークンの前）
				Found.Where = CommentSlotKind::Inline;
				if(
					const std::optional<uint32_t> At = IsBefore ?
					std::optional<uint32_t>(
						Record.Placement.TokensBefore ? TokenRunStart(Code, Syntax, Record.Anchor, Record.Placement.TokensBefore) : Span.Start
					) :
					InlineAfter(Code, Syntax, Record, HasInner[Record.Anchor], InsideTokens);
					At && !IsLineCommentStart(Found.Text) && IsStmtHeadReadable(Code, Syntax, Record.Anchor, *At)
				) {
					Found.Offset = *At;
					Found.Fragment = GetFragmentAt(Code, *At);
				} else {
					ReportDrop(Found.Text.size(), "no mid-line position", Found.IsRequired, Found.IsVerbatim, Diagnostics);
					continue;
				}
				break;
			}
			// 行末コメントの配置行の補正
			if(Found.Where == CommentSlotKind::LineEnd) {
				// 代入の右辺の配列の値の終わる行への移動
				if(
					const NodeId Parent = ParentOf(Record.Anchor), Val = Syntax.GetFieldChild(Parent, ChildRole::Right);
					Is(Parent, Role::AssignmentExpr) && Is(Val, Role::ArrayVal)
				) Found.Fragment = Anchors.Nodes[Val].EndFragment;
				// アンカーを含む要素の本文の外の行末への移動
				const NodeId Markup = OuterMarkup.empty() ? NoNode : OuterMarkup[Record.Anchor];
				if(Markup != NoNode) Found.Fragment = Anchors.Nodes[Markup].EndFragment;
				// アンカーの後に同じ行で開いた要素の終わる行への移動（本文へ取り込まない為）
				if(
					const std::unordered_map<uint32_t, std::vector<NodeId>>::const_iterator Elements = MarkupByLine.find(Span.EndFragment);
					Elements != MarkupByLine.end()
				) for(const NodeId Id : Elements->second) {
					if(const NodeSpan &Markup = Anchors.Nodes[Id]; Markup.Start >= Span.End && Markup.EndFragment > Found.Fragment) {
						Found.Fragment = Markup.EndFragment;
					}
				}
				// 補間を含む複数行のリテラルの外の行末への配置
				if(LiteralEnd) Found.Fragment = std::max(Found.Fragment, *LiteralEnd);
			}
			// コード境界と重なる行末コメントの文中への移動
			if(!CodeBoundaries.empty() && Record.Kind == CommentKind::Trailing) if(
				const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Boundary = std::lower_bound(
					CodeBoundaries.begin(),
					CodeBoundaries.end(),
					std::pair<uint32_t, uint32_t>{ IsBefore ? Span.Start : Span.End, 0 }
				);
				Boundary != CodeBoundaries.end() && Boundary->second == Found.Fragment
			) {
				Found.Where = CommentSlotKind::Inline;
				Found.Offset = Boundary->first;
			}
			// 逐語の中の改行で終わる断片の行末は差込禁止領域の為，逐語の終わる行末へ移す
			if(Found.Where == CommentSlotKind::LineEnd || Found.Where == CommentSlotKind::OwnLine) {
				while(Anchors.VerbatimBreaks[Found.Fragment] & AnchorPositionMap::EndsInVerbatim) ++Found.Fragment;
			}
			// 独立した行の物は後続のコードの行（閉括弧・節の見出を含む）をアンカーとして其の直前へ其の深さで置き，後続が無い物は行の後へ最上位の深さで置く（整形仕様 7.6）
			if(Found.Where == CommentSlotKind::OwnLine) {
				if(const uint32_t Next = Code.GetNextCodeFragment(Found.Fragment); Next < Fragments.size()) {
					Found.Where = CommentSlotKind::BeforeLine;
					Found.Fragment = Next;
				} else if(Code.GetFollowingTabs()) {
					Found.Where = CommentSlotKind::DocEnd;
					Found.Fragment = static_cast<uint32_t>(Fragments.size());
				}
			}
			// 逐語の中から始まる行の前は差込禁止領域の為，逐語の始まる行の前へ移す
			if(
				Found.Where == CommentSlotKind::BeforeLine && Found.Fragment &&
				Anchors.VerbatimBreaks[Found.Fragment - 1] & AnchorPositionMap::NextInVerbatim
			) {
				if(!BreaksInText) {
					BreaksInText.emplace(Fragments.size(), 0);
					// 地の文毎に其の中で終わる断片（出力の順に並ぶ断片の最初の物を二分探索で求める）のマーカー
					for(NodeId Id = 0; Id < Anchors.Nodes.size(); ++Id) {
						if(Anchors.Nodes[Id].IsPlaced && Is(Id, Role::InlineText)) {
							const NodeSpan &Text = Anchors.Nodes[Id];
							for(
								std::span<const CodeFragment>::iterator Fragment = std::ranges::lower_bound(Fragments, Text.Start, {}, &CodeFragment::End);
								Fragment != Fragments.end() && Fragment->End < Text.End;
								++Fragment
							) (*BreaksInText)[static_cast<size_t>(Fragment - Fragments.begin())] = 1;
						}
					}
				}
				// 遡る間に地の文を跨ぐか
				bool HasCrossedText = false;
				while(Found.Fragment && Anchors.VerbatimBreaks[Found.Fragment - 1] & AnchorPositionMap::NextInVerbatim) {
					HasCrossedText = HasCrossedText || (*BreaksInText)[Found.Fragment - 1];
					--Found.Fragment;
				}
				// 地の文（PHP の `?>`~`<?php`）の続きの行の前の物は，地の文へ差し込めず，遡ると所属する文の外へ出る為に除く（整形仕様 7.7）
				if(HasCrossedText) {
					ReportDrop(Found.Text.size(), "no position outside inline text", Found.IsRequired, Found.IsVerbatim, Diagnostics);
					continue;
				}
			}
			if(Found.Where == CommentSlotKind::BeforeLine) {
				Found.Tabs = Anchors.FragmentTabs[Found.Fragment];
				// Go の宣言又は初期値の先頭行へ移る通常コメントは，ドキュメント・cgo の前文へ取り込まれない様に空行の前に留め，空行が無ければ除く
				const NodeId Top = HasTopLevel ? TopLevel[Record.Anchor] : NoNode, Value = HasTopLevel ? InitialValues[Record.Anchor] : NoNode;
				const bool IsValueLine =
				Value != NoNode && Anchors.Nodes[Value].StartFragment == Found.Fragment && DocCommentReader::IsAtLineStart(Code, Value);
				if(
					const NodeId Decl =
					Top == NoNode || Anchors.Nodes[Top].StartFragment == Found.Fragment || IsValueLine ? Top : Syntax.GetNextNamedSibling(Top);
					Decl != NoNode && !(Decl == Top && (IsPlacedDoc || IsBefore && Value == NoNode)) && Anchors.Nodes[Decl].IsPlaced &&
					Syntax.GetNode(Decl).Facts.DoesAbsorbLeadingComment &&
					(Anchors.Nodes[Decl].StartFragment == Found.Fragment || Decl == Top && IsValueLine)
				) {
					uint32_t Prev = Found.Fragment;
					while(Prev && Fragments[Prev - 1].IsBlank) --Prev;
					if(Prev == Found.Fragment || !Prev) {
						ReportDrop(
							Found.Text.size(),
							"only a blank line keeps it out of the next Go declaration's doc",
							Found.IsRequired,
							Found.IsVerbatim,
							Diagnostics
						);
						continue;
					}
					Found.Where = CommentSlotKind::OwnLine;
					Found.Fragment = Prev - 1;
				}
			}
			// アンカーの後のコードも同行に為ったメンバの説明は，元のメンバ専用の記号で別のコードを説明しない
			Found.ShouldConvertMemberDoc = Found.Where == CommentSlotKind::LineEnd && CommentRepository::IsMemberDoc(Rules, Record) &&
			(Found.Fragment != Span.EndFragment || Span.End < Fragments[Found.Fragment].End);
			Candidates.push_back(std::move(Found));
		}
		// 断片・種類・位置・原文順への整列
		std::stable_sort(
			Candidates.begin(),
			Candidates.end(),
			[](const Candidate &Left, const Candidate &Right) -> bool {
				// 左右要素の比較の返戻
				return std::tie(Left.Fragment, Left.Where, Left.Offset, Left.Record->OriginalOrder) <
				std::tie(Right.Fragment, Right.Where, Right.Offset, Right.Record->OriginalOrder);
			}
		);
		// 配置枠を決めたコメントの返戻
		return Candidates;
	}

	/**
	 * 配置枠に応じた出力記法の本文の取得関数（整形仕様 7.6）
	 * 通常の独立・行末コメントの出力記法を規約の行形式へ揃え，行末コメントは文毎の記号を保って行末へ積む
	 * 文中とディレクティブとドキュメントの記法は其の儘とする
	 * 計算量：元記録と出力記法の本文と合成済のテキストの長さの和 N，記法の登録語数 W に対し O((W + 1)(N + 1))
	 * @param Rules 投影済の記法
	 * @param Found 置くコメント（元の記録，記号と本文を合成したテキスト，ドキュメントか）
	 * @param Where 配置枠の種類
	 * @param IsOpaque 行形式へ揃えると読み直せなく為る範囲へ置くか（プリプロセッサディレクティブの本文・読めなかった領域）
	 * @param Buf 揃えた本文の格納先（返戻の参照先に為り得る）
	 * @return 出力する本文
	 * @throws FormatLimitExceeded 行形式へ揃える記録が記法の定義を持たない場合（フォーマッターの不具合）
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	std::string_view CommentPlacer::RenderBody(
		const NotationRules &Rules,
		const Candidate &Found,
		const CommentSlotKind Where,
		const bool IsOpaque,
		std::string &Buf
	) {
		// 配置対象の記録と合成済の本文の取得
		const CommentRecord &Record = *Found.Record;
		const std::string_view Text = Found.Text;
		// 文中の枠の行コメント（PHP の閉タグの前へ置いた物）は，行末と同じく揃えて積む
		const bool IsInlineLineComment = Where == CommentSlotKind::Inline && IsLineCommentStart(Text);
		// 揃える対象でない記録の返戻
		if(Where == CommentSlotKind::Inline && !IsInlineLineComment || Record.Role == CommentRole::Directive) return Text;
		// 行形式へ揃えた本文（揃えた本文を行コメントとして読み直すと別の本文へ正規化される場合は，本文を保てない為に元の記法を保つ）
		std::string LineForm;
		if(!IsOpaque) {
			if(Found.ShouldConvertMemberDoc) {
				CommentRecord Ordinary = CommentRepository::GetOutputFormOf(Record);
				std::string Body;
				Body.reserve(Ordinary.Text.size());
				for(size_t Start = 0; Start <= Ordinary.Text.size();) {
					const size_t End = std::min(Ordinary.Text.find('\n', Start), Ordinary.Text.size());
					std::string_view Line = std::string_view(Ordinary.Text).substr(Start, End - Start);
					if(Line.starts_with(Rules.MemberDocHead)) {
						Line.remove_prefix(Rules.MemberDocHead.size());
						if(Line.starts_with(' ')) Line.remove_prefix(1);
					}
					if(Start) Body.push_back('\n');
					Body.append(Line);
					Start = End + 1;
				}
				Ordinary.Text = std::move(Body);
				Ordinary.Format.ShouldAttachFirstLine = false;
				Ordinary.Format.HasSummaryPlaceholder = false;
				const CommentOutputForm *const Plain = Rules.GetAdoptedFormOf(CommentUse::Plain);
				if(!Plain) throw FormatLimitExceeded("a converted member comment has no ordinary notation", FailureReason::FormatterDefect);
				Ordinary.Notation = CommentRepository::IsBlockNotation(Ordinary.Notation) ? CommentNotation::Block : Plain->Notation;
				const std::string Composed = CommentRepository::Compose(Ordinary);
				LineForm = LineFormOf(Rules, Ordinary, Ordinary.Text, Composed, false);
				if(LineForm.empty()) LineForm = Composed;
			} else LineForm = LineFormOf(Rules, Record, Record.Text, Found.Text, Found.IsDoc);
		}
		const std::string_view Chosen = LineForm.empty() ? Text : std::string_view(LineForm);
		if(Where != CommentSlotKind::LineEnd && !IsInlineLineComment || Chosen.find('\n') == std::string_view::npos) {
			Buf = std::move(LineForm);
			// 独立した行と１行の行末の記録の返戻
			return Buf.empty() ? Text : std::string_view(Buf);
		}
		// 文毎に分けた行末の記録は，行形式を優先して各文の記号の儘行末へ積む（積めない行を含む物は元の記法の儘，配置の判定へ委ねる）
		if(std::string Stacked = StackLineEnd(Chosen); !Stacked.empty()) {
			Buf = std::move(Stacked);
			// 行末へ積んだ描画用の本文の返戻
			return Buf;
		}
		// 通常コメントへ変換した物は，積めない場合もメンバ専用の記号へ戻さない
		if(Found.ShouldConvertMemberDoc && !LineForm.empty()) {
			Buf = std::move(LineForm);
			// メンバ専用の記号を除いた本文の返戻
			return Buf;
		}
		// 積めない記録の元の記法の返戻
		return Text;
	}

	/**
	 * コメント本文の描画関数
	 * 合成したコメントのテキストを，アンカーの深さのインデントを付けた物理行の並びへコピーする（処理系が読むコメントの続きの行は原文の儘）
	 * 計算量：本文の長さ N，物理行数 L，タブの段数 D に対し O(N + L(D + 1))
	 * @param TextView コメント本文
	 * @param Tabs 継続行（と `IsIndentFirst` の時の先頭行）へ付けるタブの段数
	 * @param IsIndentFirst 先頭行にもインデントを付けるか
	 * @param IsVerbatim 本文の続きの行を原文の儘描くか（処理系が読むコメントは，続きの行のインデントと `*` を揃えない）
	 * @param HasHeadFrames 行頭（桁０）の開始・終端の行条件を持つ埋込ドキュメントを含み得るか（其の開始・終端の行はインデントせず，間の本文の行は `*` を揃えない）
	 * @return 物理行の並び
	 * @throws FormatLimitExceeded インデントを付けた行で出力が再入力と同じ上限を超える場合
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	std::vector<std::string> CommentPlacer::RenderLines(
		const std::string_view TextView,
		const size_t Tabs,
		const bool IsIndentFirst,
		const bool IsVerbatim,
		const bool HasHeadFrames
	) {
		// 埋込ドキュメントの開始・終端記号の取得
		const std::string_view Opener = CommentRepository::GetOpenerOf(CommentNotation::RubyEmbeddedDocument);
		const std::string_view Closer = CommentRepository::GetCloserOf(CommentNotation::RubyEmbeddedDocument);
		std::vector<std::string> Lines;
		size_t Len = 0;
		// 埋込ドキュメントの開始の行の後で終端の行の前か
		bool IsInFrame = false;
		for(size_t Cursor = 0;;) {
			// 改行区切毎の本文の行
			const size_t NewlinePos = TextView.find('\n', Cursor);
			std::string_view Line =
			TextView.substr(Cursor, NewlinePos == std::string_view::npos ? std::string_view::npos : NewlinePos - Cursor);
			const size_t Emitted = Lines.size();
			// 埋込ドキュメントの開始・終端の行と本文の行
			const bool IsFrameLine = HasHeadFrames && Line == (IsInFrame ? Closer : Opener), IsFrameBody = IsInFrame && !IsFrameLine;
			if(IsFrameLine) IsInFrame ^= true;
			// 最初の行と，処理系が読むコメントでない物の続きの行のインデント（処理系が読むコメントの続きの行と，埋込ドキュメントの開始・終端の行は原文の儘）
			const bool IsIndented = !IsFrameLine && (Emitted ? IsFrameBody || !IsVerbatim : IsIndentFirst);
			// `*` で始まる継続行のドキュメントコメント位置への整合
			const size_t First = IsIndented ? Line.find_first_not_of(" \t") : std::string_view::npos;
			const bool IsStarLine =
			IsIndented && Emitted && !IsVerbatim && !IsFrameBody && First != std::string_view::npos && Line[First] == '*';
			if(IsStarLine) Line.remove_prefix(First);
			// 桁０の終端との区別に補ったタブは，配置の深さが同じ区別を保証する時だけ除く
			if(IsFrameBody && Tabs && Line.starts_with('\t') && CommentRepository::IsLineHeadWord(Line.substr(1), Closer)) {
				Line.remove_prefix(1);
			}
			// 個々の本文を実際の長さで制限し，配置後の全体の上限は合成で確認
			Len += Line.size() + static_cast<bool>(Emitted) + (IsIndented ? Tabs : 0) + IsStarLine;
			if(Len > MaxInputBytes) throw FormatLimitExceeded("output exceeds size limit");
			std::string &Output = Lines.emplace_back();
			if(IsIndented) Output.append(Tabs, '\t');
			if(IsStarLine) Output += ' ';
			Output.append(Line);
			// 最終行の描画後の終了
			if(NewlinePos == std::string_view::npos) break;
			// 次の本文行への走査位置更新
			Cursor = NewlinePos + 1;
		}
		// 物理行の並びの返戻
		return Lines;
	}

	/**
	 * テキストの位置の直前の字の種類の取得関数
	 * 計算量：トークン片数 P に対し O(log(P + 1))
	 * @param Code 変更不能なコードの出力
	 * @param Offset テキストの位置（行のインデントの後の位置以後）
	 * @param Content 行のインデントの後の位置
	 * @return 位置の直前の字の種類（区切の空白，インデントのタブ，開括弧，其の他のコード）
	 */
	CommentPlacer::Adjacent CommentPlacer::GetAdjacentBefore(
		const LaidOutCode &Code,
		const uint32_t Offset,
		const uint32_t Content
	) {
		// 位置の直前のトークン片の有無の判定
		if(const CodePiece *const Piece = Offset ? GetPieceCovering(Code, Offset - 1) : nullptr) {
			// トークン片の最後の字の種類の返戻
			return Piece->End == Offset && Piece->Shapes & CodePiece::EndsOpeningBracket ? Adjacent::OpenBracket : Adjacent::Code;
		}
		// 区切の空白かインデントのタブの返戻
		return Offset > Content ? Adjacent::Space : Adjacent::Tab;
	}

	/**
	 * 描画したコメントの最後の字の種類の取得関数
	 * @param Char コメントの物理行の最後の字
	 * @return 字の種類
	 */
	CommentPlacer::Adjacent CommentPlacer::GetAdjacentOf(const unsigned char Char) {
		// 字の種類の返戻
		return Char == ' ' ?
		Adjacent::Space :
		Char == '\t' ? Adjacent::Tab : IsOpenBracketChar(Char) ? Adjacent::OpenBracket : Adjacent::Code;
	}

	/**
	 * 行末へ置けない行末コメントかの判定関数（整形仕様 7.6, 7.7）
	 * １行の本文は行末へ積み，行コメントの後の物は其の本文へ続く（処理系の読むコメントの本文を変える積方はしない）
	 * 積めない複数行の本文は，行コメントの後なら後の行がコードに化け，２行目から別のコメントが始まる物は其のコメントが独立コメントに変わる
	 * 計算量：出力する本文の長さ N に対し O(N)
	 * @param Found 配置する記録と保護の条件
	 * @param Body 出力する本文
	 * @param IsAfterLineComment 同じ行で先に行コメントを開いたか
	 * @param IsLineCommentVerbatim 先に開いた行コメントが処理系の読むコメントか
	 * @param Diagnostics 呼出側が表示する脱落の診断の格納先
	 * @return 置けないなら true（脱落を診断する）
	 * @throws FormatLimitExceeded 保護する記録を置けない場合
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	bool CommentPlacer::IsUnplaceableLineEnd(
		const Candidate &Found,
		const std::string_view Body,
		const bool IsAfterLineComment,
		const bool IsLineCommentVerbatim,
		std::vector<std::string> *const Diagnostics
	) {
		// 本文の最初の改行位置の取得
		const size_t Newline = Body.find('\n');
		if(!IsAfterLineComment) {
			// 後続行の先頭空白の除去
			std::string_view Next = Newline == std::string_view::npos ? std::string_view() : Body.substr(Newline + 1);
			while(!Next.empty() && (Next.front() == ' ' || Next.front() == '\t')) Next.remove_prefix(1);
			// 次の行が新しいコメントを開かない事の返戻
			if(!IsLineCommentStart(Next) && !Next.starts_with(CommentRepository::GetOpenerOf(CommentNotation::Block))) return false;
		// 行コメントの後に１行の説明を置ける事の返戻
		} else if(!Found.IsVerbatim && !IsLineCommentVerbatim && Newline == std::string_view::npos) return false;
		// 脱落の診断（整形仕様 7.7, 10.5）
		ReportDrop(Found.Text.size(), "no line-end position", Found.IsRequired, Found.IsVerbatim, Diagnostics);
		// 置けない事の返戻
		return true;
	}

	/**
	 * 配置計画の構築関数
	 * 描画順の儘計画へ渡し，コメント行の列の条件を満たせないコメントは整形仕様 7.7 に従って扱う
	 * 計算量：描画済のコメント数 C，物理行の長さの和 N に対し O(C + N)
	 * @param Code 配置枠を与えた出力
	 * @param Items 描画済のコメント
	 * @param Diagnostics 呼出側が表示する脱落の診断の格納先
	 * @return コメントの配置計画
	 * @throws FormatLimitExceeded ドキュメント・ディレクティブコメントの物理行が記法の条件を満たせない場合（脱落させず，其々の保持違反の失敗とする，整形仕様 7.7，8.3）
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	CommentPlacementPlan CommentPlacer::BuildPlan(
		const LaidOutCode &Code,
		std::vector<RenderedComment> &&Items,
		std::vector<std::string> *const Diagnostics
	) {
		// コメント行の成立判定と配置計画への追加
		CommentPlacementPlan Plan(Code);
		for(RenderedComment &Item : Items) {
			std::optional<CommentBlock> Block =
			CommentBlock::Make(Code.GetAnchors().Syntax->GetRules().Notation, Code, std::move(Item.Lines), Item.Slot);
			if(!Block) {
				ReportDrop(Item.Text->size(), "comment lines do not satisfy the notation", Item.IsRequired, Item.IsDirective, Diagnostics);
				continue;
			}
			if(!Plan.IsInsertionSuccessful(Item.Slot, std::move(*Block))) {
				throw FormatLimitExceeded("comment insertion violates its slot contract", FailureReason::FormatterDefect);
			}
		}
		// 配置計画の返戻
		return Plan;
	}

	/**
	 * コメント配置関数（整形仕様 7.6）
	 * 確定したコードの配置枠へ，分離して置いたコメントを原文での種類の儘割り当てる
	 * コードのテキストと空行は読むだけで変えず，配置枠とコメント行の列の組だけを返す
	 * 計算量：出力記法の取得・本文構造の判定・配置枠の決定・本文と物理行の描画・配置計画の構築の費用の和 T，断片数 F，ノード数 V，候補数 C，トークン片数 P，配置枠数 S，候補のテキストと描画した行の長さの和 N に対し O(T + F + V log(V + 1) + C(log(V + 1) + log(P + 1) + log(S + 1)) + N)
	 * @param Code 変更不能なコードの出力
	 * @param Comments アンカーのノード毎のコメント
	 * @param Diagnostics 呼出側が表示する脱落の診断の格納先
	 * @return コメントの配置計画
	 * @throws FormatLimitExceeded 出力が再入力と同じ上限を超える場合は LimitExceeded
	 * @throws FormatLimitExceeded ドキュメントのコード境界・配置枠・物理行の条件が不成立なら `DocChanged`（整形仕様 7.7, 8.3）
	 * @throws FormatLimitExceeded ディレクティブのコード境界・配置枠・物理行の条件が不成立なら `DirectiveChanged`（脱落させず配置から伝える）
	 * @throws FormatLimitExceeded アンカーが出力に現れない場合，又は記録が記法の定義を持たない場合は FormatterDefect（取得先から伝える）
	 * @throws FormatLimitExceeded 構文の照会・走査が作業予算を超える場合
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 */
	CommentPlacementPlan CommentPlacer::PlaceLocal(
		const LaidOutCode &Code,
		const CommentRepository &Comments,
		std::vector<std::string> *const Diagnostics
	) {
		// コメントの無い入力の空の配置計画の返戻
		if(Comments.GetRecords().empty()) return CommentPlacementPlan(Code);
		const AnchorPositionMap &Anchors = Code.GetAnchors();
		const SyntaxModel &Syntax = *Anchors.Syntax;
		const NotationRules &Rules = Syntax.GetRules().Notation;
		const std::span<const CodeFragment> Fragments = Code.GetFragments();
		// 記録毎の出力記法での候補の収集（グループと順は入力の儘）
		std::vector<std::pair<uint32_t, uint32_t>> OpaqueRanges;
		const std::vector<Candidate> Candidates = CollectCandidates(Code, Syntax, Comments, OpaqueRanges, Diagnostics);
		// 行頭の開始・終端の行条件を持つ埋込ドキュメントを描き得るか
		const bool HasHeadFrames = Rules.IsAvailable(CommentNotation::RubyEmbeddedDocument);
		// 行形式へ揃えるとコメントとして読み直せなく為る範囲（行末迄を本文が飲むノードと読めなかった領域，位置の順に並べて重なりを統合する）
		std::sort(OpaqueRanges.begin(), OpaqueRanges.end());
		size_t Merged = 0;
		for(const std::pair<uint32_t, uint32_t> &Range : OpaqueRanges) {
			if(Merged && Range.first <= OpaqueRanges[Merged - 1].second) {
				OpaqueRanges[Merged - 1].second = std::max(OpaqueRanges[Merged - 1].second, Range.second);
			} else OpaqueRanges[Merged++] = Range;
		}
		OpaqueRanges.resize(Merged);
		// 置場が範囲の内か終わりに在り元の記法を保つか（計算量：範囲の数 R に対し O(log R)）
		const auto IsOpaqueAt = [&OpaqueRanges](const uint32_t Dst) -> bool {
			// 位置を含む逐語領域の探索
			const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator After = std::upper_bound(
				OpaqueRanges.begin(),
				OpaqueRanges.end(),
				Dst,
				[](const uint32_t Pos, const std::pair<uint32_t, uint32_t> &Range) -> bool {
					// 範囲の始まりとの比較の返戻
					return Pos < Range.first;
				}
			);
			// 置場以前に始まる最後の範囲の内か終わりに在るかの返戻
			return After != OpaqueRanges.begin() && Dst <= std::prev(After)->second;
		};
		std::vector<RenderedComment> Items;
		Items.reserve(Candidates.size());
		// 描画済のコメントの追加（配置枠を与えれない位置のコメントは整形仕様 7.7 に従って除く）
		const auto AddItem =
		[&](const Candidate &Found, const std::optional<CommentSlotId> Slot, std::vector<std::string> &&Lines) -> void {
			// 配置枠の有無の判定
			if(!Slot) {
				ReportDrop(Found.Text.size(), "no slot", Found.IsRequired, Found.IsVerbatim, Diagnostics);
				// 配置枠の無い通常コメントを脱落させた場合の終了
				return;
			}
			Items.push_back({ *Slot, std::move(Lines), &Found.Text, Found.IsRequired, Found.Record->Role == CommentRole::Directive });
			// 終了
			return;
		};
		// 行末へ置いたコメントが行コメントを開いた断片（後続の本文は其の行コメントへ続く）と，其れが処理系の読むコメントか
		uint32_t LineCommentFragment = std::numeric_limits<uint32_t>::max();
		bool IsLineCommentVerbatim = false;
		for(size_t Idx = 0; Idx < Candidates.size();) {
			if(Candidates[Idx].Where == CommentSlotKind::DocEnd) {
				// コードの断片の後のドキュメントの独立した行（所属する根の深さ）
				const Candidate &Found = Candidates[Idx++];
				std::string Buf;
				const std::string_view Body = RenderBody(Rules, Found, CommentSlotKind::OwnLine, false, Buf);
				AddItem(
					Found,
					Code.GetSlots().Find(CommentSlotKind::DocEnd, 0, 0),
					RenderLines(Body, Code.GetFollowingTabs().value_or(0), !Found.IsLineHeadRequired, Found.IsVerbatim, HasHeadFrames)
				);
				continue;
			}
			const uint32_t Fragment = Candidates[Idx].Fragment, LineStart = Fragments[Fragment].Start, LineEnd = Fragments[Fragment].End;
			const size_t Tabs = Anchors.FragmentTabs[Fragment];
			// 断片の中の同じ種類の候補の列挙
			const auto Next = [&](const CommentSlotKind Where) -> const Candidate * {
				// 同じ断片・種類の次の候補の返戻
				return Idx < Candidates.size() && Candidates[Idx].Fragment == Fragment && Candidates[Idx].Where == Where ?
				&Candidates[Idx++] :
				nullptr;
			};
			// 此の行の先行コメントをアンカーと同じ深さで本文より先に置く（空行は整形仕様 1.2 に従いコメントの内容で変えない）
			while(const Candidate *const Found = Next(CommentSlotKind::BeforeLine)) {
				std::string Buf;
				const std::string_view Body = RenderBody(Rules, *Found, CommentSlotKind::BeforeLine, IsOpaqueAt(LineStart), Buf);
				AddItem(
					*Found,
					Code.GetSlots().Find(CommentSlotKind::BeforeLine, Fragment, 0),
					RenderLines(Body, Found->Tabs, !Found->IsLineHeadRequired, Found->IsVerbatim, HasHeadFrames)
				);
			}
			// 行の中へ差し込む文中のコメント（前後の空白はトークンへ密着させる所を除いて１つ置く）
			uint32_t CopyFrom = LineStart + static_cast<uint32_t>(Tabs);
			bool IsInlineLineOpen = false;
			uint32_t InlineLinePos = 0;
			const uint32_t Content = LineStart + static_cast<uint32_t>(Tabs);
			Adjacent Prev = Tabs ? Adjacent::Tab : Adjacent::Newline;
			while(const Candidate *const Found = Next(CommentSlotKind::Inline)) {
				const uint32_t Dst = std::max(Found->Offset, CopyFrom);
				if(
					!Rules.LineCommentTerminator.empty() && IsInlineLineOpen && InlineLinePos == Dst &&
					(Found->Text.find(Rules.LineCommentTerminator) != std::string::npos || Found->Text.find('\n') != std::string::npos)
				) {
					ReportDrop(
						Found->Text.size(),
						"a preceding line comment would expose a PHP close tag",
						Found->IsRequired,
						Found->IsVerbatim,
						Diagnostics
					);
					continue;
				}
				if(Dst > CopyFrom) Prev = GetAdjacentBefore(Code, Dst, Content);
				std::string Buf;
				const std::string_view Body = RenderBody(Rules, *Found, CommentSlotKind::Inline, IsOpaqueAt(Dst), Buf);
				if(IsLineCommentStart(Body)) {
					IsInlineLineOpen = true;
					InlineLinePos = Dst;
				}
				std::vector<std::string> Lines = RenderLines(Body, Tabs, false, Found->IsVerbatim, HasHeadFrames);
				// 開括弧の直後を除く区切空白
				if(Prev == Adjacent::Code) Lines.front().insert(0, 1, ' ');
				// 後続のコードがコメントへ密着しない様に区切の空白の配置（閉括弧の前は元から空白が在った時だけ置く）
				if(Dst < LineEnd) {
					const CodePiece *const Following = Code.GetPieceFrom(Dst);
					const CodePiece *const Starting = Following && Following->Start == Dst ? Following : nullptr;
					if(
						Following && Following->Start <= Dst && !HasShape(Starting, CodePiece::StartsSeparator) &&
						(!HasShape(Starting, CodePiece::StartsClosingBracket) || GetAdjacentBefore(Code, Dst, Content) == Adjacent::Space) &&
						!Found->Record->Placement.IsGluedAfter
					) Lines.back() += ' ';
				}
				Prev = GetAdjacentOf(Lines.back().back());
				CopyFrom = Dst;
				AddItem(*Found, Code.GetSlots().Find(CommentSlotKind::Inline, Fragment, Dst), std::move(Lines));
			}
			// 行末の字（文中のコメントで終わる行は其のコメントの末尾）
			Adjacent Tail =
			CopyFrom == LineEnd ? Prev : LineEnd > LineStart ? GetAdjacentBefore(Code, LineEnd, Content) : Adjacent::Newline;
			// 此の行末に対応する行末と後の独立した行のコメント
			while(Idx < Candidates.size() && Candidates[Idx].Fragment == Fragment) {
				const Candidate &Found = Candidates[Idx++];
				std::string Buf;
				const std::string_view Body = RenderBody(Rules, Found, Found.Where, IsOpaqueAt(LineEnd), Buf);
				// アンカーの行の後の独立した行
				if(Found.Where == CommentSlotKind::OwnLine) {
					AddItem(
						Found,
						Code.GetSlots().Find(CommentSlotKind::OwnLine, Fragment, 0),
						RenderLines(Body, Found.Tabs, !Found.IsLineHeadRequired, Found.IsVerbatim, HasHeadFrames)
					);
					continue;
				}
				if(
					!Rules.LineCommentTerminator.empty() && LineCommentFragment == Fragment &&
					Found.Text.find(Rules.LineCommentTerminator) != std::string::npos
				) {
					ReportDrop(
						Found.Text.size(),
						"a preceding line comment would expose a PHP close tag",
						Found.IsRequired,
						Found.IsVerbatim,
						Diagnostics
					);
					continue;
				}
				if(IsUnplaceableLineEnd(Found, Body, LineCommentFragment == Fragment, IsLineCommentVerbatim, Diagnostics)) continue;
				std::vector<std::string> Lines = RenderLines(Body, Tabs, false, Found.IsVerbatim, HasHeadFrames);
				// 前置区切（記録したタブの数が正なら其の数のタブ，０なら半角スペース１つ，整形仕様 7.6）
				if(Tail != Adjacent::Space && Tail != Adjacent::Newline) {
					const size_t Leading = Found.Record->Format.LeadingTabCount;
					Lines.front().insert(0, Leading ? Leading : 1, Leading ? '\t' : ' ');
				}
				Tail = GetAdjacentOf(Lines.back().back());
				// 最後の行が行コメントを開いた断片の記録（積んだ後の物は先に開いた行コメントの本文へ続く）
				if(LineCommentFragment != Fragment && IsLineCommentStart(Body.substr(Body.rfind('\n') + 1))) {
					LineCommentFragment = Fragment;
					IsLineCommentVerbatim = Found.IsVerbatim;
				}
				AddItem(Found, Code.GetSlots().Find(CommentSlotKind::LineEnd, Fragment, 0), std::move(Lines));
			}
		}
		// 共通配置で描画したコメントの配置計画の返戻
		return BuildPlan(Code, std::move(Items), Diagnostics);
	}

	/**
	 * コメント配置関数（整形仕様 7.6）
	 * 親と子の所有記録を其々同じ規則で配置し，子の配置枠を親が確定した配置枠へコピーする
	 * 子の記録を親の通常コメントへ混ぜず，計画は配置枠とコメント行の列だけを持つ
	 * 計算量：親子の共通配置の費用の和 T，子の所有領域数 R，割当数 C，配置枠数 S，転写する行の長さの和 N に対し平均 O(T + R + C log(S + 1) + N)
	 * @param Code 子の断片と配置枠も具体化した変更不能なコード
	 * @param Comments 親の通常記録と子の独立した所有経路
	 * @param Diagnostics 呼出側が表示する脱落の診断の格納先
	 * @return 親の配置枠へのコメントの割当だけの計画
	 * @throws FormatLimitExceeded 子の所有記録・所有経路・対応する配置枠が無い場合，又は配置枠が別の出力に属する場合は FormatterDefect
	 * @throws FormatLimitExceeded 親又は子の共通配置が出力上限を超える場合は LimitExceeded（共通配置から伝える）
	 * @throws FormatLimitExceeded ドキュメントのコード境界・配置枠・物理行の条件に違反する場合は `DocChanged`（共通配置から伝える）
	 * @throws FormatLimitExceeded ディレクティブのコード境界・配置枠・物理行の条件に違反する場合は `DirectiveChanged`（共通配置から伝える）
	 * @throws FormatLimitExceeded アンカーの出力位置・記法の定義が不足する場合は FormatterDefect（共通配置から伝える）
	 * @throws FormatLimitExceeded 構文の照会・走査が作業予算を超える場合
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 */
	CommentPlacementPlan CommentPlacer::Place(
		const LaidOutCode &Code,
		const CommentRepository &Comments,
		std::vector<std::string> *const Diagnostics
	) {
		// 親の記録の配置と子の配置枠の統合
		CommentPlacementPlan Plan = PlaceLocal(Code, Comments, Diagnostics);
		for(const OwnedRegionSpan &Region : Code.GetAnchors().OwnedRegions) {
			const std::unordered_map<NodeId, std::shared_ptr<const CommentRepository>>::const_iterator Found =
			Comments.OwnedChildren.find(Region.Node);
			if(Found == Comments.OwnedChildren.end() || !Region.Placement) {
				throw FormatLimitExceeded("embedded comments have no ownership route", FailureReason::FormatterDefect);
			}
			const LaidOutCode &Child = *Region.Placement;
			CommentPlacementPlan ChildPlan = PlaceLocal(Child, *Found->second, Diagnostics);
			for(CommentInsertion &Insertion : ChildPlan.Insertions) {
				std::optional<CommentSlotId> Target;
				if(Insertion.Slot.Kind == CommentSlotKind::DocEnd) Target = Region.Terminal;
				else {
					const uint32_t Fragment = Region.FirstFragment + Insertion.Slot.Fragment;
					const uint32_t Offset = Insertion.Slot.Kind == CommentSlotKind::Inline ?
					Code.GetFragments()[Fragment].Start + Insertion.Slot.Offset - Child.GetFragments()[Insertion.Slot.Fragment].Start :
					0;
					Target = Code.GetSlots().Find(Insertion.Slot.Kind, Fragment, Offset);
				}
				if(!Target) throw FormatLimitExceeded("embedded slot mapping is incomplete", FailureReason::FormatterDefect);
				// 確認済の記法を親の配置枠へ移し，本文の所有を計画へ渡す
				if(
					!Insertion.Comment.IsRebindingSuccessful(Child.GetAnchors().Syntax->GetRules().Notation, Code, *Target) ||
					!Plan.IsInsertionSuccessful(*Target, std::move(Insertion.Comment))
				) throw FormatLimitExceeded("embedded slot belongs to another output", FailureReason::FormatterDefect);
			}
		}
		// コメント割当の計画の返戻
		return Plan;
	}
} // namespace Core
