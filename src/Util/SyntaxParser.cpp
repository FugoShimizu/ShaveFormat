#include "SyntaxParser.hpp"
#include "FormatLimitExceeded.hpp"
#include "ParseAdapter.hpp"
#include "ParsedTree.hpp"
#include "ParserAllocator.hpp"

#if defined(SHAVEFMT_PERFORMANCE_PROBE)

#include "PerformanceObservation.hpp"

#endif

#include "TextEdit.hpp"
#include "../Profile/GrammarProfile.hpp"
#include <tree_sitter/api.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/**
 * パーサー解放関数
 * 計算量：O(1)
 * @param Target 解放するパーサー
 */
void SyntaxParser::Deleter::operator()(TSParser *const Target) const {
	// 所有するパーサーの解放
	ts_parser_delete(Target);
	// 終了
	return;
}

/**
 * コンストラクタ（文法を設定しないパーサーの構築）
 * 計算量：O(1)
 */
SyntaxParser::SyntaxParser() {
	// パーサーと外部スキャナーの生成前の確保器登録
	ParserAllocator::Install();
	Handle.reset(ts_parser_new());
	// 終了
	return;
}

/**
 * 文法の設定関数
 * 計算量：O(1)
 * @param Grammar 設定する文法
 * @return 実行時ライブラリと互換で設定出来れば true
 */
bool SyntaxParser::CanSetGrammar(const TSLanguage *const Grammar) {
	// 文法を設定出来たかの返戻
	return Handle && ts_parser_set_language(Handle.get(), Grammar);
}

/**
 * パーサーへ文面を渡す読出関数
 * 計算量：O(1)
 * @param Payload 読ませる文面
 * @param ByteIdx 読み始める位置
 * @param Pos 読み始める行と桁（位置で決まる読出をしない為に使わない）
 * @param BytesRead 返した長さの格納先
 * @return 読み始めた位置からの残り全体（末尾では長さ０）
 */
static const char *ReadWholeSrc(
	void *const Payload,
	const uint32_t ByteIdx,
	[[maybe_unused]] const TSPoint Pos,
	uint32_t *const BytesRead
) {
	// 解析対象の文面
	const std::string_view &Text = *static_cast<const std::string_view *>(Payload);
	if(ByteIdx >= Text.size()) {
		// 末尾での読出量
		*BytesRead = 0;
		// 末尾に達し読む物が無い事の返戻
		return "";
	}
	// 返す残余の長さ
	*BytesRead = static_cast<uint32_t>(Text.size() - ByteIdx);
	#if defined(SHAVEFMT_PERFORMANCE_PROBE)
	PerformanceObservation::Parsing.ReadBytes += *BytesRead;
	#endif
	// 残り全体の返戻
	return Text.data() + ByteIdx;
}

// 読取量を数える入力（パーサーへ渡したバイト数を累計する）
struct CountedSrc {
	std::string_view Text; // 纏めた挿入後の本文
	ParserReadBudget &Budget; // 未使用の実読取予算と打切の合図
};

/**
 * 残余予算内の連続領域の取得関数
 * 計算量：O(1)，本文を複写しない
 * @param Src 読み出す既存の連続範囲
 * @param Pos 読み始める位置
 * @return 最大６４バイト且つ残余予算内の範囲（EOF 又は予算打切なら空）
 */
std::string_view ParserReadBudget::Take(const std::string_view Src, const size_t Pos) {
	// EOF には追加の読出が要らない事の返戻
	if(Pos >= Src.size()) return std::string_view();
	if(!Remaining) {
		IsCancelled = true;
		// 更なる読出に依る予算打切の返戻
		return std::string_view();
	}
	const size_t Len = std::min({ size_t{ 64 }, Src.size() - Pos, Remaining });
	Remaining -= Len;
	// 複写しない連続範囲の返戻
	return Src.substr(Pos, Len);
}

/**
 * 実読取予算内に限るパーサーの入力読出関数（整形仕様 9.3）
 * 読み始めた位置から上限迄を渡し，渡したバイト数を残余から引く（差分解析で再利用した領域は読まれない為に数えない）
 * 計算量：O(1)
 * @param Payload 読取量を数える入力
 * @param ByteIdx 挿入後の本文で読み始める位置
 * @param Pos 読み始める行と列（位置だけで読む為に使わない）
 * @param BytesRead 返した長さの格納先
 * @return 読み始めた位置からの上限迄の本文（末尾では長さ０）
 */
static const char *ReadCountedChunk(
	void *const Payload,
	const uint32_t ByteIdx,
	[[maybe_unused]] const TSPoint Pos,
	uint32_t *const BytesRead
) {
	// 読取予算を持つソースへの参照
	CountedSrc &Src = *static_cast<CountedSrc *>(Payload);
	const std::string_view Chunk = Src.Budget.Take(Src.Text, ByteIdx);
	*BytesRead = static_cast<uint32_t>(Chunk.size());
	#if defined(SHAVEFMT_PERFORMANCE_PROBE)
	PerformanceObservation::Parsing.ReadBytes += *BytesRead;
	PerformanceObservation::Parsing.RepairReadBytes += *BytesRead;
	#endif
	// 複写せず，予算内の連続した本文領域の返戻
	return Chunk.empty() ? "" : Chunk.data();
}

// 解析 API が受け取る期限と修復の打切の合図
struct ParseProgress {
	const std::chrono::steady_clock::time_point &Until; // 此の解析の期限
	const ParserReadBudget *Budget; // 修復の実読取予算
};

/**
 * 期限又は実読取予算に依る打切の判定関数
 * 計算量：O(1)
 * @param State パーサーの進捗と今回の期限
 * @return 期限又は予算を使い切れば true
 */
static bool HasReachedParseDeadline(TSParseState *const State) {
	// 解析の進行状態の取得
	const ParseProgress &Progress = *static_cast<const ParseProgress *>(State->payload);
	// 解析の中断の返戻
	return ParserAllocator::IsStopping() || Progress.Budget && Progress.Budget->IsCancelled ||
	std::chrono::steady_clock::now() > Progress.Until;
}

/**
 * 許容破損の判定関数（パーサーの未対応に由来し，書掛ではない `ERROR` / `MISSING`）
 * 計算量：ノード数 N と節のテキスト・引数の前置空白の長 B，宣言・空引数・空値の判定の費用 Q に対し初回 O(N + B + Q)
 * @param Src 判定対象を含むソース
 * @param Node 判定対象のノード
 * @param Ancestors Node の祖先の並び（走査の道筋から渡す，末尾が親）
 * @return パーサーの追随遅れに由来するなら true
 * @throws std::bad_alloc 木の世代毎の親と許容エラーの表を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
bool SyntaxParser::IsToleratedBreak(const ParsedTree &Src, const TSNode Node, const std::span<const TSNode> Ancestors) {
	// CSS の空のカスタムプロパティ値と逐語の URL 内部の偽の欠落を許容する事の返戻
	if(ParseAdapter::IsEmptyCssCustomVal(Src, Node) || Src.IsToleratedCssUrlVal(Node)) return true;
	// 型を持たないノードは許容対象でない事の返戻
	if(ts_node_is_null(Node)) return false;
	// 親の構文で確かめたパーサーの追随遅れの許容
	if(!Ancestors.empty()) {
		// 直近の祖先ノード
		const TSNode Parent = Ancestors.back();
		// セレクタ・確認済のモジュールの節・空引数・仮置の名前の追随遅れだけを許容する事の返戻
		if(
			Src.Is(Node, Core::Role::NestingSelector) && (
				Src.Is(Parent, Core::Role::CombinatorSelector) || Src.Is(Parent, Core::Role::BinaryOperation) && HasChildOf(
					Parent,
					[&Src](const TSNode Child) -> bool {
						// 確認済のモジュールの節を表すエラーかの返戻
						return Src.Is(Child, Core::Role::Error) && Src.IsToleratedModuleError(Child);
					}
				)
			) || ts_node_is_missing(Node) && Src.Is(Parent, Core::Role::Param) && !Src.GetLen(Parent) && Ancestors.size() > 2 &&
			Src.Is(Ancestors[Ancestors.size() - 3], Core::Role::ParamRule) || ParseAdapter::IsEmptyArgError(Src, Node, Parent) ||
			Src.Is(Node, Core::Role::Identifier) && Src.Is(Parent, Core::Role::PlaceholderNode)
		) return true;
	}
	// 構文エラーの外と SCSS の文法の外の返戻
	if(!Src.Is(Node, Core::Role::LaggingGrammarError)) return false;
	// １度の走査で継いだモジュールの節への所属の返戻
	return Src.IsToleratedModuleError(Node);
}

/**
 * 期限と取消を設定した構文解析関数
 * 計算量：パーサーの費用 Q に対し O(Q)
 * @param Parser パーサー
 * @param Old 差分解析の基準（全体解析では無）
 * @param Input 入力の読出口
 * @param Until 今回の解析期限
 * @param Budget 修復の予算（通常の解析では無）
 * @return 解析木（中断又は失敗では無）
 * @throws ProcessingInterrupted 書戻以外の処理の中断を要求済の場合
 */
TSTree *SyntaxParser::ParseUntil(
	TSParser *const Parser,
	const TSTree *const Old,
	const TSInput Input,
	const std::chrono::steady_clock::time_point &Until,
	const ParserReadBudget *const Budget
) {
	ParseProgress Progress { Until, Budget };
	const TSParseOptions Options { &Progress, HasReachedParseDeadline };
	// 期限と予算を監視した解析結果の取得
	TSTree *const Tree = ts_parser_parse_with_options(Parser, Old, Input, Options);
	if(ParserAllocator::IsStopping()) {
		ts_tree_delete(Tree);
		ParserAllocator::CheckInterruption();
	}
	// 呼出側へ木の所有権を渡す返戻
	return Tree;
}

/**
 * アロー関数を被演算子に置いたエラーの有無の判定関数
 * アロー関数は括弧で包まない限り参照・添字・呼出の被演算子に為れず，行の終わりを挟まない `() => {}[0]` は言語処理系が拒む（JavaScript 等）
 * パーサーはエラーを立てずに受理する為，入力のエラーとして扱う（整形仕様 4.6, 8.2）
 * 計算量：ノード数 N と検査した本文の総長 L に対し O(N + L)
 * @param Src 走査対象のソース
 * @param IssueOffset 最初に検出した原文内の解析用バイト位置の格納先
 * @return 行の終わりを挟まないアロー関数の被演算子が在れば true
 * @throws std::bad_alloc 木の走査又はトークンの検査の領域を確保出来ない場合
 * @throws std::length_error 親子表又はトークンの列の要素数が上限を超える場合
 */
bool SyntaxParser::HasArrowOperandMisuse(const ParsedTree &Src, std::optional<uint32_t> &IssueOffset) {
	// 同じ世代の親子表の訪問で確定した結果の取得
	Src.RecordNodeRelations();
	const std::optional<uint32_t> Pos = Src.Syntax.Relations->ArrowOperand;
	if(Pos) IssueOffset = Pos;
	// 受理しない構文の有無の返戻
	return Pos.has_value();
}

/**
 * 行継続の直後のコメントの有無の判定関数
 * パーサーは行継続の後のコメントをエラーを立てずに受理するが，処理系は継続する行へコメントを置けない（Python 等）
 * 整形すると継続のバックスラッシュを失い別の構文エラーへ書き換わる為，入力のエラーとして扱う（整形仕様 4.6, 8.2）
 * 計算量：ノード数 N と検査した本文の総長 L に対し O(N + L)
 * @param Src 走査対象のソース
 * @param IssueOffset 最初に検出した原文内の解析用バイト位置の格納先
 * @return 行継続の直後にコメントが在れば true
 * @throws std::bad_alloc 木の走査又はトークンの検査の領域を確保出来ない場合
 * @throws std::length_error 親子表又はトークンの列の要素数が上限を超える場合
 */
bool SyntaxParser::HasContinuationComment(const ParsedTree &Src, std::optional<uint32_t> &IssueOffset) {
	// 同じ世代の親子表の訪問で確定した結果の取得
	Src.RecordNodeRelations();
	const std::optional<uint32_t> Pos = Src.Syntax.Relations->ContinuationComment;
	if(Pos) IssueOffset = Pos;
	// 受理しない構文の有無の返戻
	return Pos.has_value();
}

/**
 * 論理行の改行とインデントの成立条件の判定関数
 * 処理系は括弧の外のバックスラッシュで続けない改行で論理行を終える為，式又は見出の途中の改行を拒むが，パーサーは演算子の後等の改行を読み飛ばして受理する（Python 等）
 * パーサーはエラーを立てずに受理する為，入力のエラーとして扱い，完成候補でも同じ判定で構文の破壊を検出する（整形仕様 4.6, 8.1）
 * インデントは主列とタブを１字とする代替列で照合し，コメントだけの行・リテラル・継続行の深さを本体の所属へ数えない
 * 論理行の終わりと為れるのは，文のコンテナの中の文の間，見出のコロンと本体の間，本体又は本体で終わる節と後続の節の間，修飾子と定義の間だけとする
 * 計算量：ノード数 N，トークン間の文字数 B と共通祖先の探索で訪ねる祖先数 A に対し平均 O(N + B + A)
 * @param Src 走査対象のソース
 * @param IssueOffset 最初に検出した原文内の解析用バイト位置の格納先
 * @return 論理行の途中の改行又は本体の所属が成立しないインデントが在れば true
 * @throws std::bad_alloc 木の走査又はトークンの検査の領域を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool SyntaxParser::HasStmtLineBreak(const ParsedTree &Src, std::optional<uint32_t> &IssueOffset) {
	// 対象テキストの取得
	const std::string &Text = Src;
	// 同じ世代の記録番号で，各親の次の子を直接参照する
	Src.RecordNodeRelations();
	std::vector<uint32_t> ChildCursors;
	const auto ChildAt = [&Src, &ChildCursors](const TSNode Parent, const uint32_t Pos) -> TSNode {
		const ParsedChild *const Record = Src.FindRecordedNode(Parent);
		// 此の世代に属さない親の空の子の返戻
		if(!Record) return TSNode{};
		const std::span<const ParsedChild> Children = Src.GetChildrenAt(Record->Relation);
		if(ChildCursors.empty()) ChildCursors.resize(Src.Syntax.Relations->Parents.Records.size(), 0);
		uint32_t &Next = ChildCursors[Record->Relation];
		while(Next < Children.size() && Children[Next].End <= Pos) ++Next;
		// 位置を含む子又は空のノードの返戻
		return Next < Children.size() && Children[Next].GetStart() <= Pos ? Children[Next].Node : TSNode{};
	};
	// 括弧の深さ，直前のトークンと，論理行の途中の改行の有無
	uint32_t Depth = 0;
	TSNode Prev {};
	const ReadingRules &Reading = Src.GetReading();
	const std::string_view Continuation = Src.GetRules().GetSpelling(Core::Word::LineContinuation);
	std::vector<std::pair<size_t, size_t>> Indents { { 0, 0 } };
	size_t Column = 0, AlternateColumn = 0, ContinuationColumn = 0, GapWork = 0;
	uint32_t GapStart = 0, LineEnd = 0;
	bool IsLineStart = true, HasBreak = false;
	const auto CanIndentAfter = [&Src](const TSNode Node) -> bool {
		// 見出のコロン以外では本体のインデントを始めない事の返戻
		if(ts_node_is_null(Node) || !Src.Is(Node, Core::Role::ColonToken)) return false;
		TSNode Body = Src.GetNextSibling(Node);
		while(Src.Is(Body, Core::Role::Comment) || Src.Is(Body, Core::Role::LineContinuation) || Src.Is(Body, Core::Role::Error)) {
			Body = Src.GetNextSibling(Body);
		}
		// 宣言又は制御の見出に属する本体が後続する事の返戻
		return !ts_node_is_null(Body) && Src.Is(Body, Core::Role::IndentedBody);
	};
	for(const size_t Slot : Src.Syntax.Relations->AdmissionLeaves) {
		++Src.Syntax.Relations->AdmissionLeafWork;
		const TSNode Node = Src.Syntax.Relations->Children[Slot].Node;
		// 見付けた後と，幅の無い補完は検査しない
		if(HasBreak || !Src.GetLen(Node)) continue;
		const uint32_t Start = Src.GetStart(Node);
		// トークンの内部を読まず，隙間を原文順に１度だけ読む
		for(uint32_t At = GapStart; At < Start; ++At) {
			++GapWork;
			// 空白として呑まれた登録済の継続も，コメント・リテラルの外の隙間でだけ読む
			if(
				!Continuation.empty() && At + Continuation.size() < Start &&
				std::string_view(Text).substr(At, Continuation.size()) == Continuation && Text[At + Continuation.size()] == '\n'
			) {
				if(IsLineStart && !ContinuationColumn) ContinuationColumn = Column;
				GapWork += Continuation.size();
				At += static_cast<uint32_t>(Continuation.size());
				continue;
			}
			const char Char = Text[At];
			if(Char == '\n') {
				IsLineStart = true;
				Column = AlternateColumn = ContinuationColumn = 0;
				LineEnd = At;
			} else if(IsLineStart && Reading.HasIndentBlocks) {
				if(Char == ' ') {
					++Column;
					++AlternateColumn;
				} else if(Char == '\t') {
					Column += Reading.SourceIndentTabWidth - Column % Reading.SourceIndentTabWidth;
					++AlternateColumn;
				} else if(Reading.IndentResetChars.find(Char) != std::string_view::npos) Column = AlternateColumn = 0;
			}
		}
		GapStart = Src.GetEnd(Node);
		// 継続の改行を論理行の終わりへ数えず，行頭の最初の非零の深さを保つ
		if(Src.Is(Node, Core::Role::LineContinuation)) {
			if(IsLineStart && !ContinuationColumn) ContinuationColumn = Column;
			continue;
		}
		// コメントだけの行は深さの列へ加えない
		if(Src.Is(Node, Core::Role::Comment)) continue;
		if(IsLineStart && !Depth && Reading.HasIndentBlocks) {
			const std::pair<size_t, size_t> Current = ContinuationColumn ?
			std::pair<size_t, size_t>{ ContinuationColumn, ContinuationColumn } :
			std::pair<size_t, size_t>{ Column, AlternateColumn };
			++GapWork;
			if(Current.first > Indents.back().first) {
				HasBreak = Current.second <= Indents.back().second || !CanIndentAfter(Prev);
				if(!HasBreak) Indents.push_back(Current);
			} else {
				while(Current.first < Indents.back().first && Indents.size() > 1) {
					Indents.pop_back();
					++GapWork;
				}
				HasBreak = Current != Indents.back();
			}
			if(HasBreak) IssueOffset = Start;
		}
		if(HasBreak) continue;
		if(!ts_node_is_null(Prev) && !Depth && IsLineStart) {
			const uint32_t From = Src.GetEnd(Prev), To = Start;
			TSNode Common {};
			for(TSNode Owner = Src.GetParentOf(Node); !ts_node_is_null(Owner); Owner = Src.GetParentOf(Owner)) {
				if(Src.GetStart(Owner) < From && To < Src.GetEnd(Owner)) {
					Common = Owner;
					break;
				}
			}
			// エラーを含む範囲はエラー領域の判定へ委ね，文のコンテナの中の文の間は論理行の終わりとする
			if(!ts_node_is_null(Common) && !ts_node_has_error(Common) && !Src.Is(Common, Core::Role::IndentContainer)) {
				const TSNode Left = ChildAt(Common, From - 1), Right = ChildAt(Common, To);
				const uint32_t LeftCount = ts_node_child_count(Left);
				// 本体又は本体で終わる節の後か
				const bool IsAfterBody = Src.Is(Left, Core::Role::IndentContainer) ||
				LeftCount && Src.Is(ts_node_child(Left, LeftCount - 1), Core::Role::IndentContainer);
				HasBreak = !(
					Src.Is(Left, Core::Role::ColonToken) && Src.Is(Right, Core::Role::IndentContainer) || IsAfterBody ||
					Src.Is(Left, Core::Role::AttachPrefix)
				);
				if(HasBreak) IssueOffset = LineEnd;
			}
		}
		if(
			Src.Is(Node, Core::Role::OpeningParenToken) || Src.Is(Node, Core::Role::OpeningBracketToken) ||
			Src.Is(Node, Core::Role::OpeningBraceToken)
		) ++Depth;
		else if(
			Depth && (
				Src.Is(Node, Core::Role::ClosingParenToken) || Src.Is(Node, Core::Role::ClosingSquareToken) ||
				Src.Is(Node, Core::Role::ClosingBraceToken)
			)
		) --Depth;
		IsLineStart = false;
		Prev = Node;
	}
	Src.Syntax.Relations->IndentationWork += GapWork;
	// 論理行又はインデントの成立条件に反する入力の有無の返戻
	return HasBreak;
}

/**
 * 自動の文終端が構文の内部へ入る入力エラーの判定関数
 * 計算量：同じ世代の親子表の準備費用 R に対し初回 O(R)，準備済なら O(1)
 * @param Src 解析済のテキストと登録した文終端の定義
 * @param IssueOffset 最初の不正な改行の位置の格納先
 * @return 完結した文・宣言・メンバの境界でない位置へ文終端が入る場合は true
 * @throws std::bad_alloc 境界の作業列を確保出来ない場合
 * @throws std::length_error 作業列の要素数上限を超える場合
 */
bool SyntaxParser::HasImplicitTerminatorMisuse(const ParsedTree &Src, std::optional<uint32_t> &IssueOffset) {
	// 共通走査で記録した最初の不正な文終端の位置
	Src.RecordNodeRelations();
	const std::optional<uint32_t> Pos = Src.Syntax.Relations->RejectedTerminator;
	if(Pos) IssueOffset = Pos;
	// 不正な文終端の有無の返戻
	return Pos.has_value();
}

/**
 * 処理系が拒む値の数とトークンの有無の判定関数
 * 計算量：同じ世代の親子表の準備費用 R に対し初回 O(R)，準備済なら O(1)
 * @param Src 解析済の本文
 * @param IssueOffset 最初に検出した原文内の解析用バイト位置の格納先
 * @return 読取規則が拒む最上位の並び又はリテラルが在れば true
 * @throws std::bad_alloc 木の走査又はトークンの検査の領域を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool SyntaxParser::HasRejectedVal(const ParsedTree &Src, std::optional<uint32_t> &IssueOffset) {
	// 値・コメント・親子関係を同じ世代の１度の走査で取得する
	Src.RecordNodeRelations();
	const ParsedTree::NodeRelations &Relations = *Src.Syntax.Relations;
	// 最上位の値の数は，リテラルの検査より先に判定する
	const std::optional<uint32_t> Pos = Relations.ExtraRootVal ? Relations.ExtraRootVal : Relations.RejectedVal;
	if(Pos) IssueOffset = Pos;
	// 処理系が拒む値の有無の返戻
	return Pos.has_value();
}

/**
 * 処理系が拒むトークンの間の空白の字の有無の判定関数
 * パーサーは垂直タブ・改ページを全ての文法で空白と読むが，処理系は言語毎に拒む（Go の改ページ，Python の垂直タブ等）
 * 整形するとトークンの間の空白を生成し直して処理系が受理するテキストへ書き換わる為，入力のエラーとして扱う（整形仕様 4.6, 8.1）
 * 計算量：ノード数 N と本文の長さ L に対し O(N + L)
 * @param Src 走査対象のソース
 * @param IssueOffset 最初に検出した原文内の解析用バイト位置の格納先
 * @return 処理系が空白と読まない垂直タブ又は改ページがトークンの間に在れば true
 * @throws std::bad_alloc 木の走査又はトークンの検査の領域を確保出来ない場合
 * @throws std::length_error 親子表又はトークンの列の要素数が上限を超える場合
 */
bool SyntaxParser::HasRejectedSpace(const ParsedTree &Src, std::optional<uint32_t> &IssueOffset) {
	// 同じ世代の親子表の訪問で確定した結果の取得
	Src.RecordNodeRelations();
	const std::optional<uint32_t> Pos = Src.Syntax.Relations->RejectedSpace;
	if(Pos) IssueOffset = Pos;
	// 受理しない構文の有無の返戻
	return Pos.has_value();
}

/**
 * 破損又は名前の無いトークンへ入った NUL の有無の判定関数
 * NUL の最小の所有範囲が破損又は文法名の無い自動終端である場合だけ入力エラーとし，受理した文字列・コメントの内容を拒否しない
 * 計算量：本文長 L，訪問ノード数 N，NUL 数 Z，木の深さ H に対し O(L + N log(Z + 1))，追加領域 O(Z + H)
 * @param Src 解析済の本文
 * @param IssueOffset 最初に検出した NUL の解析用バイト位置の格納先
 * @return 破損又は名前の無いトークンへ読まれた NUL が在れば true
 * @throws std::bad_alloc 木の走査の作業枠を確保出来ない場合
 * @throws std::length_error 木の走査の作業列の要素数上限を超える場合
 */
bool SyntaxParser::HasRejectedNul(const ParsedTree &Src, std::optional<uint32_t> &IssueOffset) {
	const std::string_view Text = Src.GetCodeText();
	const size_t First = Text.find('\0');
	// NUL を含まない本文の返戻
	if(First == std::string_view::npos) return false;
	// エラーの無い自動終端も含め，NUL を持つ部分木だけを辿る為の位置列
	std::vector<uint32_t> NulPositions;
	for(size_t Pos = First; Pos != std::string_view::npos; Pos = Text.find('\0', Pos + 1)) {
		NulPositions.push_back(static_cast<uint32_t>(Pos));
	}
	std::optional<uint32_t> Rejected;
	// エラーノードの子に属さない範囲から最初の NUL を記録する関数
	// 計算量：NUL 数 Z に対し O(log(Z + 1))
	// @param Start 範囲の開始バイト
	// @param End 範囲の終端バイト
	const auto ReadGap = [&](const uint32_t Start, const uint32_t End) -> void {
		// 空又は重なった範囲の終了
		if(End <= Start) return;
		// 本文を再走査せず，受理した子の範囲を除いた位置列から照合する
		const std::vector<uint32_t>::const_iterator Position = std::ranges::lower_bound(NulPositions, Start);
		if(Position != NulPositions.end() && *Position < End && (!Rejected || *Position < *Rejected)) Rejected = *Position;
		// 終了
		return;
	};
	// エラーノードの未受理範囲を調べ，子の走査を要するかを返す関数
	// 計算量：直接の子数 C，NUL 数 Z に対し O((C + 1) log(Z + 1))
	// @param Node 判定対象のノード
	// @return 確定した位置より前の NUL を含む子を辿る場合は true
	const auto ShouldDescend = [&](const TSNode Node) -> bool {
		// NUL を含まない部分木と確定した位置以降の読飛し
		const uint32_t Begin = Src.GetStart(Node), End = Src.GetEnd(Node);
		const std::vector<uint32_t>::const_iterator Position = std::ranges::lower_bound(NulPositions, Begin);
		// NUL を持つ未確定の部分木以外を除く事の返戻
		if(Position == NulPositions.end() || *Position >= End || Rejected && Begin >= *Rejected) return false;
		// 文法の名前も本文の保持契約も無い自動終端へ取り込まれた NUL は，正常な文字列・コメントと区別する
		if(!ts_node_is_named(Node) && !ts_node_child_count(Node) && !*ts_node_type(Node)) {
			ReadGap(Begin, End);
			// 保持の条件が無いトークンを記録した後の返戻
			return false;
		}
		// 通常のノードに属する NUL の保持条件を子で調べる事の返戻
		if(!ts_node_has_error(Node) || !Src.Is(Node, Core::Role::Error)) return true;
		uint32_t Start = Src.GetStart(Node);
		ForEachChild(
			Node,
			[&](const TSNode Child) -> void {
				// 直接子の前の範囲の検査と子の範囲の除外
				ReadGap(Start, Src.GetStart(Child));
				Start = std::max(Start, Src.GetEnd(Child));
				// 終了
				return;
			}
		);
		// 最後の直接子の後の範囲の検査
		ReadGap(Start, Src.GetEnd(Node));
		// より前の NUL を含み得る子を辿る事の返戻
		return true;
	};
	// 根自身と，降下を要する子ノードの検査
	if(ShouldDescend(Src.GetRoot())) WalkChildrenCursor(Src.GetRoot(), ShouldDescend);
	if(Rejected) IssueOffset = Rejected;
	// パーサーが受理しなかった NUL の有無の返戻
	return Rejected.has_value();
}

/**
 * 構文破損ノードの判定関数（`ERROR` / `MISSING`，但し文字列リテラルの内側は除く）
 * 計算量：子・祖先と照合テキストの総量 N に対し O(N)
 * @param Src 判定対象を含むソース
 * @param Node 判定対象のノード
 * @param Ancestors Node の祖先の並び（走査の道筋から渡す，末尾が親，根では空）
 * @return 構文の破損と見做すなら true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool SyntaxParser::IsBrokenNode(const ParsedTree &Src, const TSNode Node, const std::span<const TSNode> Ancestors) {
	// 欠落ノードか
	const bool IsMissing = ts_node_is_missing(Node);
	// `ERROR` でも `MISSING` でもないノードとパーサーの追随遅れを破損としない事の返戻
	if(!IsMissing && !Src.Is(Node, Core::Role::Error) || IsToleratedBreak(Src, Node, Ancestors)) return false;
	// 文字列直下の `ERROR` だけ許容し，`MISSING` 又は文字列外を破損とする事の返戻
	return IsMissing || Ancestors.empty() || !Src.Is(Ancestors.back(), Core::Role::StrLikeAll);
}

/**
 * JSONC の末尾カンマを表す破損からカンマを得る関数
 * 計算量：ノード数 N と直接の子数 C に対し初回 O(N + C)，以後 O(C)
 * @param Src 判定対象を含むソース
 * @param Node 判定対象の `ERROR` 又は `MISSING`
 * @return 配列又はオブジェクトの最後の要素に続くカンマのトークン（該当しなければ空）
 * @throws std::bad_alloc 直接親の表を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
TSNode SyntaxParser::JsonTrailingCommaToken(const ParsedTree &Src, const TSNode Node) {
	// 欠落として補われたカンマか
	const bool IsMissing = ts_node_is_missing(Node);
	if(
		!IsMissing && (!Src.Is(Node, Core::Role::Error) || !Src.GetProfile().IsSpelling(Src.GetView(Node), Core::Word::ListSeparator))
	// JSONC の末尾カンマに対応して居ないパーサーが立てる形でない事の返戻
	) return TSNode{};
	// 破損を含む親ノード
	const TSNode Parent = Src.GetParentOf(Node);
	if(ts_node_is_null(Parent) || !Src.Is(Parent, Core::Role::ArrayLiteral) && !Src.Is(Parent, Core::Role::ObjectVal)) {
		// 親の無い破損又は JSON の配列とオブジェクト以外に現れたカンマを許容しない事の返戻
		return TSNode{};
	}
	// 対象通過済と後続値有無のフラグ
	bool IsAfter = false, HasValAfter = false;
	// 直前の値と其れに続くカンマ
	TSNode LastVal {}, Comma {};
	ForEachChild(
		Parent,
		[&](const TSNode Child) -> void {
			// 対象破損へ到達した場合
			if(ts_node_eq(Child, Node)) {
				// 対象破損通過済の記録
				IsAfter = true;
				// 対象破損へ到達した場合の終了
				return;
			}
			// 対象破損より後の子の場合
			if(IsAfter) {
				if(ts_node_is_named(Child) && !Src.Is(Child, Core::Role::Comment)) HasValAfter = true;
				// 対象破損より後の子の場合の終了
				return;
			}
			if(const bool IsNamed = ts_node_is_named(Child); IsNamed && !Src.Is(Child, Core::Role::Comment)) {
				// 対象破損より前の最後の値
				LastVal = Child;
				// 新しい値に対応するカンマの初期化
				Comma = {};
			} else if(!IsNamed && !ts_node_is_null(LastVal) && Src.Is(Child, Core::Role::CommaToken)) Comma = Child;
		}
	);
	if(!IsMissing && !ts_node_is_null(LastVal) && ts_node_child_count(Node) == 1) Comma = ts_node_child(Node, 0);
	// 空の列の `[,]` / `{,}` は末尾カンマでなく構文エラーで，後続に値が在るカンマも区切のエラーとして許容しない事の返戻
	return IsAfter && !ts_node_is_null(LastVal) && !HasValAfter ? Comma : TSNode{};
}

/**
 * 本体を失ったインデントのブロックの有無の判定関数
 * tree-sitter-python はインデントの無い本体（`def f():` の次の行のインデント無の文）を空のブロックと読み，エラーを立てない（Python は拒む）
 * 計算量：ノード数 N に対し O(N)
 * @param Src 解析済のソース（インデントでブロックを成す言語）
 * @return 最初の幅０のブロックの位置（無ければ空）
 * @throws std::bad_alloc 親子表のメモリを確保出来ない場合
 * @throws std::length_error 親子表の要素数が上限を超える場合
 */
std::optional<uint32_t> SyntaxParser::EmptyBlockAt(const ParsedTree &Src) {
	// 未解析ソースの空の判定の返戻
	if(!Src.IsParsed()) return std::nullopt;
	Src.RecordNodeRelations();
	// 親子表の同じ訪問で確定した欠落本体の位置の返戻
	return Src.Syntax.Relations->EmptyBlock;
}

/**
 * 境界を確定出来ないエラー領域の有無の判定関数（整形仕様 4.6）
 * エラーの回復がドキュメントを組み直す文法では全ての領域を，其の他の文法でもドキュメントの中に閉じない括弧が在れば領域を信頼しない
 * インデント又は語でブロックを閉じる文法では，エラーを含む最も大きい１行の構文が其の行の全体を占め，領域が語のトークンを含まない時だけブロックの境界を確定出来る
 * 其の他の文法は波括弧等の記号でブロックを閉じる為，領域毎の境界を保護の照合（整形仕様 8.3）に委ねる
 * 計算量：ノード数 N，エラー領域数 E，境界の空白の総長 B に対し O(N + E log(N + 1) + B)
 * @param Src 解析済のソース
 * @return 境界を確定出来ないエラー領域が在れば true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool SyntaxParser::HasUntrustedErrorZone(const ParsedTree &Src) {
	const ReadingRules &Rules = Src.GetReading();
	// エラーの回復がドキュメントを組み直す文法の返戻
	if(Rules.CanRecoveryRestructureDoc) return true;
	// 同じ世代の親子表の訪問で数えた，根を除く括弧の開閉数の差
	Src.RecordNodeRelations();
	const int Depth = Src.Syntax.Relations->OpenDepth;
	// 閉じない括弧の在るドキュメントの返戻
	if(Depth > 0) return true;
	// 記号でブロックを閉じる文法の返戻
	if(!Rules.HasIndentBlocks && !Rules.HasKeywordClosedBlocks) return false;
	const std::string &Text = Src;
	// 破損ノードの論理行の範囲
	struct ErrorLine {
		TSNode Error;
		TSNode Line;
	};
	std::vector<ErrorLine> Errors;
	std::vector<uint32_t> Words;
	std::unordered_map<uint32_t, uint32_t> CommentEnds;
	// 同じ論理行の判定
	const auto IsSingleLine = [](const TSNode Node) -> bool {
		// 改行を含まない範囲の判定結果の返戻
		return ts_node_start_point(Node).row == ts_node_end_point(Node).row;
	};
	const TSNode Root = Src.GetRoot();
	std::vector<TSNode> LineOwners { IsSingleLine(Root) ? Root : TSNode{} };
	WalkChildrenCursor(
		Root,
		[&](const TSNode Node, const std::span<const TSNode> Ancestors) -> bool {
			LineOwners.resize(Ancestors.size());
			const TSNode Parent = Ancestors.back(), Above = LineOwners.back();
			const TSNode Owner = !IsSingleLine(Parent) ? TSNode{} : ts_node_is_null(Above) ? Parent : Above;
			LineOwners.push_back(Owner);
			if(Src.Is(Node, Core::Role::Error)) Errors.push_back({ Node, IsSingleLine(Node) && !ts_node_is_null(Owner) ? Owner : Node });
			if(Src.Is(Node, Core::Role::Comment)) CommentEnds.emplace(Src.GetStart(Node), Src.GetEnd(Node));
			if(!ts_node_is_named(Node) && !ts_node_child_count(Node)) {
				if(const std::string_view Word = Src.GetView(Node); !Word.empty() && std::isalpha(static_cast<unsigned char>(Word.front()))) {
					Words.push_back(Src.GetStart(Node));
				}
			}
			// 子を辿る事の返戻
			return true;
		}
	);
	// エラー領域自身が根である場合は祖先を持たない
	if(Src.Is(Root, Core::Role::Error)) Errors.insert(Errors.begin(), { Root, Root });
	for(const ErrorLine &Error : Errors) {
		const uint32_t Lead = TextEdit::SkipSpaceLeft(Text, Rules, Src.GetStart(Error.Line));
		uint32_t Tail = TextEdit::SkipSpaceRight(Text, Rules, Src.GetEnd(Error.Line));
		// 行末のコメントは行の終わりへ含める
		if(
			const std::unordered_map<uint32_t, uint32_t>::const_iterator Comment = CommentEnds.find(Tail);
			Comment != CommentEnds.end()
		) Tail = TextEdit::SkipSpaceRight(Text, Rules, Comment->second);
		const bool IsWholeLine = IsSingleLine(Error.Line) && (!Lead || Text[Lead - 1] == '\n') &&
		(Tail == Text.size() || Text[Tail] == '\n' || Text[Tail] == '\r');
		// 行の全体を占めない領域又はブロックを開閉し得る語のトークンを含む領域の返戻
		if(
			const std::vector<uint32_t>::const_iterator Word = std::lower_bound(Words.begin(), Words.end(), Src.GetStart(Error.Error));
			!IsWholeLine || Word != Words.end() && *Word < Src.GetEnd(Error.Error)
		) return true;
	}
	// 全領域の境界を確定出来た事の返戻
	return false;
}

/**
 * 期限付の構文解析関数
 * 計算量：原文長 B とパーサーの実作業量 Q に対し O(B + Q)
 * @param Old 増分解析の土台（全体解析では nullptr）
 * @param Text 解析する文面
 * @param IsExpired 期限の超過の格納先（超過した時だけ真を書く）
 * @param Budget 残余予算と即時打切の合図（通常の解析では nullptr）
 * @return 解析した構文木（期限の超過と失敗では nullptr）
 * @throws std::bad_alloc 文字列の区切又は検査のメモリの確保に失敗した場合
 */
TSTree *SyntaxParser::Parse(
	const TSTree *const Old,
	const std::string_view Text,
	bool &IsExpired,
	ParserReadBudget *const Budget
) const {
	// 今回の解析期限
	#if defined(SHAVEFMT_PERFORMANCE_PROBE)
	++PerformanceObservation::Parsing.Calls;
	PerformanceObservation::Parsing.InputBytes += Text.size();
	PerformanceObservation::Parsing.RepairCalls += static_cast<bool>(Old);
	#endif
	const std::chrono::steady_clock::time_point Until =
	std::chrono::steady_clock::now() + std::chrono::microseconds(DeadlineBaseMicros + static_cast<int64_t>(Text.size()));
	// 通常の解析では読取予算を使わず，修復の差分解析だけ残余を減らす
	ParserReadBudget Unlimited { 0 };
	CountedSrc Counted { Text, Budget ? *Budget : Unlimited };
	std::string_view WholeText = Text;
	TSInput Input {};
	Input.payload = Budget ? static_cast<void *>(&Counted) : &WholeText;
	Input.read = Budget ? ReadCountedChunk : ReadWholeSrc;
	Input.encoding = TSInputEncodingUTF8;
	TSTree *Tree = ParseUntil(Handle.get(), Old, Input, Until, Budget);
	if(Budget && Budget->IsCancelled) {
		ts_tree_delete(Tree);
		Tree = nullptr;
	}
	// 次の解析前に於ける打切後のパーサー状態の初期化
	if(!Tree) {
		// 中断した解析状態の初期化
		ts_parser_reset(Handle.get());
		// 期限超過の通知
		if(!(Budget && Budget->IsCancelled) && std::chrono::steady_clock::now() > Until) IsExpired = true;
	}
	// 解析した構文木の返戻
	return Tree;
}

/**
 * 解析結果の受理の判定関数（整形仕様 3.4 の Parse，4.6）
 * パーサーが読めないソース，処理系が拒む構文，本文・インデントを保てない `ERROR`・`MISSING` を受理せず，其の他のエラー領域は部分整形へ回す
 * 計算量：ソースの長さ L，引用符の候補数 Q，祖先数 D，構文解析・保護範囲とエラーノードの判定の費用 P に対し O(Q L + D + P)
 * @param Profile 入力で確定した文法
 * @param Input 適応を済ませた入力（解析結果の所有権を引き取る）
 * @return 適応時の解析結果と受理の判定
 * @throws std::bad_alloc 文字列の区切又は検査のメモリの確保に失敗した場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
ParsedTree SyntaxParser::Parse(const GrammarProfile &Profile, ParseInput &Input) {
	// 適応段階で止めた入力の受理区分の引継
	ParsedTree Src(std::move(*Input.Src));
	Input.Src.reset();
	if(Input.Decision != ParseDecision::Continue) {
		const ParseDecision Decision = Input.Decision == ParseDecision::Skip ? ParseDecision::Skip : ParseDecision::Fail;
		Src.SetParseDecision(Decision, Input.Failure, false);
		// 適応が既に止めた結果の返戻
		return Src;
	}
	const ReadingRules &Rules = Profile.GetReading();
	// 遅延した本文の終端を，エラーフラグに依らず実在する原文と対応させる
	if(Rules.CanAbsorbVerbatimOnError) {
		std::vector<bool> EmptyNames;
		size_t Ends = 0;
		const bool HasMissingEnd = HasDescendantOf(
			Src.GetRoot(),
			[&](const TSNode Node) -> bool {
				if(Src.Is(Node, Core::Role::HeredocBeginning)) {
					const std::string_view Header = Src.GetView(Node);
					const size_t Quote = Header.find_first_of("'\"`");
					EmptyNames.push_back(Quote != std::string_view::npos && Quote + 2 == Header.size() && Header[Quote] == Header.back());
				}
				// 終端以外は開始と本体の対応を記録して次へ進む返戻
				if(!Src.Is(Node, Core::Role::DeferredLiteralEnd)) return false;
				const bool IsEmptyName = Ends < EmptyNames.size() && EmptyNames[Ends];
				++Ends;
				const uint32_t Start = Src.GetStart(Node);
				// 空の引用名は実在する空行だけを終端とし，EOF に補われた終端とは区別する
				if(
					Src.GetEnd(Node) != Start || IsEmptyName && Start < Src.size() &&
					(Src[Start] == '\n' || Src[Start] == '\r' && Start + 1 < Src.size() && Src[Start + 1] == '\n')
				) return false;
				Src.IssueOffset = Start;
				// 原文の終端を持たない本文を見付けた事の返戻
				return true;
			}
		);
		if(HasMissingEnd || Ends < EmptyNames.size()) {
			Src.Issue = ParseIssue::InputSyntax;
			Src.SetParseDecision(ParseDecision::Skip, FailureReason::InputSyntaxError, true);
			// 実在する終端を持たない逐語本文を原文で保持する返戻
			return Src;
		}
	}
	// パーサーが回復して受理した定義も，登録した宣言子の成立条件で確かめる
	if(Rules.ShouldRequireFunctionDeclarator && CountMalformedCDefs(Src)) {
		const ParsedTree::NodeRelations &Relations = *Src.Syntax.Relations;
		const bool IsInputError = Relations.MalformedFunctionDef.has_value();
		Src.IssueOffset = IsInputError ? Relations.MalformedFunctionDef : Relations.UncertainFunctionDef;
		Src.Issue = IsInputError ? ParseIssue::InputSyntax : ParseIssue::UnrecognizedSyntax;
		Src.SetParseDecision(
			ParseDecision::Skip,
			IsInputError ? FailureReason::InputSyntaxError : FailureReason::UnrecognizedSyntax,
			IsInputError
		);
		// 関数としての成立を確認出来ない定義を原文で保持する返戻
		return Src;
	}
	// パーサーが読めないテキストと処理系が拒む構文の検査
	if(
		HasRejectedNul(Src, Src.IssueOffset) || Rules.CanAcceptArrowOperand && HasArrowOperandMisuse(Src, Src.IssueOffset) ||
		Rules.CanAcceptContinuationComment && HasContinuationComment(Src, Src.IssueOffset) ||
		Rules.CanAcceptStmtLineBreak && HasStmtLineBreak(Src, Src.IssueOffset) ||
		!Rules.AutomaticTerminators.empty() && HasImplicitTerminatorMisuse(Src, Src.IssueOffset) ||
		(Rules.HasSingleRootVal || Rules.HasStrictStrEscapes || Rules.ShouldHaveFractionDigits) &&
		HasRejectedVal(Src, Src.IssueOffset) ||
		(Rules.CanAcceptVerticalTabSpace || Rules.CanAcceptFormFeedSpace) && HasRejectedSpace(Src, Src.IssueOffset)
	) {
		Src.Issue = ParseIssue::InputSyntax;
		Src.SetParseDecision(ParseDecision::Skip, FailureReason::InputSyntaxError, false);
		// パーサーが NUL で読めないソースと，パーサーは受理するが処理系が拒むソースの返戻
		return Src;
	}
	// 入力破損の種類別のフラグ
	bool HasInputMissing = false, HasJsxError = false, HasOpenStr = false, HasInputError = false;
	// 開きから読んだ文字列の閉じの位置（パーサーが閉じの引用符を独立したエラーノードに分けた時に開きと読まない）
	std::unordered_set<size_t> Closers;
	// エラーノードで得た開引用符から，エスケープを区別して同じ行の閉じを探す
	const auto IsUnclosed = [&Src, &Closers, &Rules](const uint32_t Start, const std::string_view Quote) -> bool {
		// 一致する通常引用の改行とエスケープの事実
		const std::vector<LiteralReading>::const_iterator Literal = std::ranges::find_if(
			Rules.Literals,
			[Quote](const LiteralReading &Candidate) -> bool {
				// 同じ引用の開きかの返戻
				return Candidate.Open == Quote && Candidate.Delimiter == LiteralDelimiter::Fixed;
			}
		);
		// 登録の無いエラーノードでは従来のバックスラッシュのエスケープを使う
		const std::string_view Escape = Literal == Rules.Literals.end() ? std::string_view("\\") : Literal->Escape;
		// 対象範囲の走査
		for(size_t Pos = Start + Quote.size(); Pos < Src.size(); ++Pos) {
			// 行内の引用が閉じない事の返戻
			if(Src[Pos] == '\n' && (Literal == Rules.Literals.end() || !Literal->CanCrossLine)) return true;
			if(!Escape.empty() && Src[Pos] == Escape.front()) ++Pos;
			else if(std::string_view(Src).substr(Pos).starts_with(Quote)) {
				Closers.insert(Pos);
				// エスケープされて居ない閉引用符に達した事の返戻
				return false;
			}
		}
		// ソースの末尾迄閉じない事の返戻
		return true;
	};
	// 接頭辞後の未閉判定（生・三重・逐語文字列は残り全体で閉じを探索）
	// 行を跨げる文字列の引用符の長さ（其の引用符で始まらなければ０）
	const auto MultilineQuoteLen = [&Rules](const std::string_view Text) -> size_t {
		// 行を跨げる引用符の照合
		for(const LiteralReading &Literal : Rules.Literals) {
			if(
				Literal.CanCrossLine && Literal.Open.size() > 1 &&
				(Literal.Delimiter == LiteralDelimiter::Fixed || Literal.Delimiter == LiteralDelimiter::Repeated) &&
				Text.starts_with(Literal.Open)
			// 一致した引用符の長さの返戻
			) return Literal.Open.size();
		}
		// 行を跨げる引用符で始まらない事の返戻
		return 0;
	};
	// 閉じて居ない開きの判定
	const auto IsUnclosedOpening = [&Src, &Closers, &IsUnclosed, &MultilineQuoteLen, &Rules](const uint32_t Start) -> bool {
		// 先に読んだ文字列の閉じの引用符の返戻
		if(Closers.contains(Start)) return false;
		// 文字列接頭辞に対応する閉引用符の探索
		const std::string &Whole = Src;
		const std::string_view Text = std::string_view(Whole).substr(Start, 0X20);
		size_t Prefix = 0;
		for(size_t Len = std::min<size_t>(Text.size(), 3); Len; --Len) {
			if(std::ranges::find(Rules.StrPrefixes, Text.substr(0, Len)) != Rules.StrPrefixes.end()) {
				// 一致した接頭辞長の記録
				Prefix = Len;
				break;
			}
		}
		const std::string_view Rest = Text.substr(Prefix), Head = Text.substr(0, Prefix);
		// 接頭辞が文字列の形の一覧に在るか
		const auto IsHeadIn = [&Head](const std::vector<std::string_view> &Heads) -> bool {
			// 一覧の接頭辞かの返戻
			return std::ranges::find(Heads, Head) != Heads.end();
		};
		// 接頭辞の後にトークンが無ければ文字列の開きではない事の返戻
		if(Rest.empty()) return false;
		std::string Close;
		size_t OpenLen = 1;
		if(Rest.front() == '#' && (!Prefix || IsHeadIn(Rules.HashQuotePrefixes))) {
			const size_t Hashes = Rest.find_first_not_of('#');
			// `#` の並びの後が引用符でなければ文字列の開きではない事の返戻
			if(Hashes == std::string_view::npos || Rest[Hashes] != '"') return false;
			const size_t Quotes = std::max<size_t>(MultilineQuoteLen(Rest.substr(Hashes)), 1);
			Close.assign(Quotes, '"').append(Hashes, '#');
			OpenLen = Hashes + Quotes;
		// 引用符無の返戻
		} else if(Rest.front() != '"' && Rest.front() != '\'' && Rest.front() != '`') return false;
		else if(IsHeadIn(Rules.RawStrPrefixes) && Rest.front() == '"') {
			const size_t Paren = Rest.find('(');
			// 区切の `(` が無ければ生文字列の開きではない事の返戻
			if(Paren == std::string_view::npos) return false;
			Close.assign(1, ')').append(Rest.substr(1, Paren - 1)).push_back('"');
			OpenLen = Paren + 1;
		} else if(const size_t Len = MultilineQuoteLen(Rest)) {
			const std::vector<LiteralReading>::const_iterator Literal = std::ranges::find_if(
				Rules.Literals,
				[Rest](const LiteralReading &Candidate) -> bool {
					// 複数引用符の開きかの返戻
					return Rest.starts_with(Candidate.Open) && Candidate.CanCrossLine;
				}
			);
			switch(Literal->Delimiter) {
			case LiteralDelimiter::Repeated:
				OpenLen = std::min(Rest.find_first_not_of(Rest.front()), Rest.size());
				break;
			default:
				OpenLen = Len;
				break;
			}
			Close.assign(OpenLen, Rest.front());
		} else if(IsHeadIn(Rules.VerbatimStrPrefixes)) Close = "\"";
		// 通常の引用符が閉じて居ないかの返戻
		else return IsUnclosed(Start + static_cast<uint32_t>(Prefix), Rest.substr(0, 1));
		const size_t Found = Whole.find(Close, Start + Prefix + OpenLen);
		if(Found != std::string::npos) Closers.insert(Found);
		// 行を跨げる文字列の閉じの区切がファイルの残りに無いかの返戻
		return Found == std::string::npos;
	};
	// JSX は最外コンテナを１度だけ調べ，`ERROR` 毎の祖先探索の２乗化を回避
	uint32_t JsxCheckedEnd = 0;
	// 既存 `ERROR` は部分整形で許容し新規分だけ破壊判定（O(1) のフラグで健全なら全木走査を省略）
	if(Src.IsParsed() && ts_node_has_error(Src.GetRoot())) {
		HasDescendantWithAncestorsOf(
			Src.GetRoot(),
			[&](const TSNode Cur, const std::span<const TSNode> Ancestors) -> bool {
				// 型を持たないノードの内側は調べない事の返戻
				if(ts_node_is_null(Cur)) return false;
				if(Src.GetStart(Cur) >= JsxCheckedEnd && ts_node_has_error(Cur) && Src.Is(Cur, Core::Role::RenderedChildren)) {
					// 検査済 JSX コンテナ範囲の更新
					JsxCheckedEnd = Src.GetEnd(Cur);
					HasJsxError = HasDescendantWithAncestorsOf(
						Cur,
						[&Src](const TSNode Inner, const std::span<const TSNode> InnerAncestors) -> bool {
							// 破損した `ERROR` かの返戻
							return Src.Is(Inner, Core::Role::Error) && IsBrokenNode(Src, Inner, InnerAncestors);
						}
					);
				}
				// 破損の判定は `IsBrokenNode` に集約する（文字列リテラル直下の `ERROR` は中身のテキストで構文は壊れて居ない）
				const bool IsTreeBroken = IsBrokenNode(Src, Cur, Ancestors) &&
				!(Rules.CanTolerateTrailingCommaError && !ts_node_is_null(JsonTrailingCommaToken(Src, Cur)));
				if(IsTreeBroken && Src.Is(Cur, Core::Role::Error)) {
					HasOpenStr = IsUnclosedOpening(Src.GetStart(Cur));
					if(!HasOpenStr) {
						ForEachChild(
							Cur,
							[&](const TSNode Child) -> bool {
								// C++ の `R"` 等の接頭辞付開始トークンを含む引用符候補
								if(Src.Is(Child, Core::Role::QuoteToken)) HasOpenStr = IsUnclosedOpening(Src.GetStart(Child));
								// 閉じない文字列を見付ける迄次の子へ進む事の返戻
								return !HasOpenStr;
							}
						);
					}
				}
				// 未対応構文でも出る `ERROR` と区別し，閉括弧等の不足を表す `MISSING` を記録
				if(ts_node_is_missing(Cur) && IsTreeBroken) HasInputMissing = true;
				if(IsTreeBroken) {
					HasInputError = true;
					if(!Src.IssueOffset) Src.IssueOffset = Src.GetStart(Cur);
					Src.Issue = HasOpenStr ? ParseIssue::InputSyntax : ParseIssue::UnrecognizedSyntax;
				}
				// 整形しない事が確定したら早期に打切の返戻（HasDescendantOf は true 返戻で打切に為る）
				return HasInputMissing && HasInputError || HasOpenStr || HasJsxError;
			},
			true
		);
	}
	// CSS / SCSS の宣言値の後の行コメントをパーサーがトークンへ呑み，行末をエラー領域の外へ置いた場合の見送（整形仕様 4.6, 10.4）
	// 行末空白を整えると後続の波括弧がコメント内へ入り，境界を確定出来ない
	std::vector<std::pair<uint32_t, uint32_t>> ErrorRanges;
	if(Rules.CanAbsorbRulesIntoValComments && HasInputError) {
		if(Src.Is(Src.GetRoot(), Core::Role::Error)) ErrorRanges.emplace_back(Src.GetStart(Src.GetRoot()), Src.GetEnd(Src.GetRoot()));
		else {
			WalkChildrenCursor(
				Src.GetRoot(),
				[&](const TSNode Node) -> bool {
					// エラー領域でないノードの子を辿る事の返戻
					if(!Src.Is(Node, Core::Role::Error)) return true;
					ErrorRanges.emplace_back(Src.GetStart(Node), Src.GetEnd(Node));
					// 内側のエラー領域を重ねて記録しない事の返戻
					return false;
				}
			);
		}
	}
	// 逐語領域内の位置の判定
	if(
		const auto IsProtectedAt = [&ErrorRanges](const uint32_t Pos) -> bool {
			// 位置を含み得るエラー領域の探索
			const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator After = std::upper_bound(
				ErrorRanges.begin(),
				ErrorRanges.end(),
				Pos,
				[](const uint32_t Val, const std::pair<uint32_t, uint32_t> &Range) -> bool {
					// 領域の始まりとの比較の返戻
					return Val < Range.first;
				}
			);
			// 外側のエラー領域の中に在る位置かの返戻
			return After != ErrorRanges.begin() && Pos < std::prev(After)->second;
		}; Rules.CanAbsorbRulesIntoValComments && HasInputError && HasDescendantOf(
			Src.GetRoot(),
			[&Src, &IsProtectedAt](const TSNode Node) -> bool {
				// 宣言値への所属を確定出来ないエラーは，行コメントの呑込と断定しない
				if(!Src.Is(Node, Core::Role::Error)) return false;
				const TSNode Declaration = Src.GetParentOf(Node);
				// 宣言の直接子でないエラーの返戻
				if(!Src.Is(Declaration, Core::Role::TypedDecl)) return false;
				const TSNode Separator = Src.GetChild(Declaration, 1);
				// 宣言値の始まりより前のエラーの返戻
				if(!Src.Is(Separator, Core::Role::ColonToken) || Src.GetStart(Node) < Src.GetEnd(Separator)) return false;
				// 直前の区切トークンの追跡準備
				TSNode Prev {};
				// 隣り合う２つの `/` のトークンを子に持ち，其の行の終わりがエラー領域の外に在るエラー領域かの返戻
				return HasChildOf(
					Node,
					[&Src, &Prev, &IsProtectedAt](const TSNode Child) -> bool {
						// 隣り合う２つの斜線トークンの接続の判定
						const bool IsJoined =
						!ts_node_is_null(Prev) && !ts_node_is_missing(Prev) && !ts_node_is_missing(Child) && Src.Is(Prev, Core::Role::SlashToken) &&
						Src.Is(Child, Core::Role::SlashToken) && Src.GetView(Prev) == "/" && Src.GetView(Child) == "/" &&
						Src.GetEnd(Prev) == Src.GetStart(Child);
						Prev = Child;
						const size_t LineEnd = IsJoined ? Src.GetCodeText().find('\n', Src.GetEnd(Child)) : std::string_view::npos;
						// 行コメントの開きのトークンの並びで，行の終わりを整え得るかの返戻
						return LineEnd != std::string_view::npos && !IsProtectedAt(static_cast<uint32_t>(LineEnd));
					}
				);
			}
		)
	) {
		Src.SetParseDecision(ParseDecision::Skip, FailureReason::BraceAbsorbed, HasInputError);
		// 境界を確定出来ない入力の見送の返戻
		return Src;
	}
	if(Rules.HasIndentBlocks) {
		const std::optional<uint32_t> EmptyAt = EmptyBlockAt(Src);
		Src.HasMissingBody = EmptyAt.has_value();
		if(EmptyAt) {
			Src.Issue = ParseIssue::InputSyntax;
			Src.IssueOffset = EmptyAt;
		}
	}
	// 本文・インデントを保てない破損だけ見送，パーサー未対応の正当な構文は許容
	if(
		HasInputMissing || HasJsxError || HasOpenStr || Src.HasMissingBody || HasInputError && (
			HasUntrustedErrorZone(Src) || Rules.CanMisreadCustomProperties && (
				Src.Is(Src.GetRoot(), Core::Role::Error) || HasDescendantOf(
					Src.GetRoot(),
					[&Src, &Rules](const TSNode Node) -> bool {
						// URL の引数列でエラーと兄弟に分かれたコメントの誤読を照合する
						if(Src.IsCssUrlArgs(Node) && ts_node_has_error(Node)) {
							// 直接子でコメントの誤読を見付けたかの返戻
							return HasChildOf(
								Node,
								[&Src](const TSNode Child) -> bool {
									// URL の本文をコメントと読んだ子かの返戻
									return Src.Is(Child, Core::Role::Comment);
								}
							);
						}
						// URL の呼出自体をエラーと読んだ範囲の照合
						if(Src.Is(Node, Core::Role::Error)) {
							// CSS 関数名の照合テキスト
							const std::string_view Url = Rules.UrlFunctionHead;
							// 関数名を値と誤読した url の直前３字からの探索
							const uint32_t From = Src.GetStart(Node) - std::min<uint32_t>(Src.GetStart(Node), 3);
							// url 誤読の探索範囲
							const std::string_view Text(Src.data() + From, Src.GetEnd(Node) - From);
							// 大小を問わず url 内のコメント誤読を検出した結果の返戻（コメント無なら逐語保持可能）
							return std::search(
								Text.begin(),
								Text.end(),
								Url.begin(),
								Url.end(),
								[](const unsigned char Actual, const unsigned char Expected) -> bool {
									// 英字の大小を揃えた字の一致の返戻
									return std::tolower(Actual) == Expected;
								}
							) != Text.end() && HasDescendantOf(
								Node,
								[&Src](const TSNode Inner) -> bool {
									// コメントかの返戻
									return Src.Is(Inner, Core::Role::Comment);
								}
							);
						}
						// 要素の足りない規則は誤読の形でない事の返戻
						if(!Src.Is(Node, Core::Role::SelectorRule) || ts_node_named_child_count(Node) < 3) return false;
						const std::string_view Selector = Src.GetView(ts_node_named_child(Node, 0));
						const TSNode Colon = ts_node_named_child(Node, 1);
						// `--x` だけのセレクタ・`:` の `ERROR`・値の波括弧が並ぶ規則（カスタムプロパティの誤読）かの返戻
						return Selector.starts_with(Rules.CustomPropertyHead) && Selector.find_first_of(" \t\n") == std::string_view::npos &&
						Selector.find(Rules.PropertySeparator) == std::string_view::npos && Src.Is(Colon, Core::Role::Error) &&
						Src.GetView(Colon) == Rules.PropertySeparator;
					}
				)
			) || Rules.CanMisreadEscapedRegexEnd && HasDescendantOf(
				Src.GetRoot(),
				[&Src](const TSNode Node) -> bool {
					// 正規表現でない事の返戻
					if(!Src.Is(Node, Core::Role::RegexLiteralNode)) return false;
					const std::string_view Text = Src.GetView(Node);
					// `/` で終わらない（空の範囲を含む）正規表現でない事の返戻
					if(Text.size() < 2 || Text.back() != '/') return false;
					// 正規表現末尾のエスケープ記号数
					size_t Escapes = 0;
					for(size_t At = Text.size() - 1; At && Text[At - 1] == '\\'; --At) ++Escapes;
					// 終わりの `/` が逃がされた（前に奇数個の `\` が続く）かの返戻
					return Escapes & 1;
				}
			) || Rules.CanDropTokensOnRecovery && HasDescendantOf(
				Src.GetRoot(),
				[&Src](const TSNode Node) -> bool {
					// Kotlin が回復時に落としたトークンを子の間隙から検出し，else 脱落等に因る文の誤結合を防止
					uint32_t PrevEnd = std::numeric_limits<uint32_t>::max();
					// 子の間に落としたトークンが在るかの返戻
					return HasChildOf(
						Node,
						[&](const TSNode Child) -> bool {
							// 現在の子の開始と直前の終端
							const uint32_t Start = Src.GetStart(Child), Prev = std::exchange(PrevEnd, Src.GetEnd(Child));
							// 前の子との間に落としたトークンが在るかの返戻
							return Prev < Start &&
							std::string_view(Src.data() + Prev, Start - Prev).find_first_not_of(" \t\r\n;") != std::string_view::npos;
						}
					);
				}
			) || Rules.CanAbsorbVerbatimOnError && (
				!Rules.VerbatimStart.empty() && Src.GetCodeText().find(Rules.VerbatimStart) != std::string_view::npos ||
				HasDescendantWithAncestorsOf(
					Src.GetRoot(),
					[&Src](const TSNode Node, const std::span<const TSNode> Ancestors) -> bool {
						// 開始とも読めるシフトは，直接の親のエラーに在る既存の条件を保つ
						if(Src.Is(Node, Core::Role::HeredocAmbiguousShift)) return !Ancestors.empty() && Src.Is(Ancestors.back(), Core::Role::Error);
						// 逐語本文の境界に関わらないノードの返戻
						if(!Src.Is(Node, Core::Role::HeredocBeginning) && !Src.Is(Node, Core::Role::HeredocBody)) return false;
						// 境界がエラー領域へ取り込まれたかの返戻（外側の閉じた本文は拒否しない）
						return std::ranges::any_of(
							Ancestors,
							[&Src](const TSNode Ancestor) -> bool {
								// 境界を取り込んだエラーかの返戻
								return Src.Is(Ancestor, Core::Role::Error);
							}
						);
					}
				) || HasChildOf(
					Src.GetRoot(),
					[&Src](const TSNode Child) -> bool {
						// 地の文の子かの返戻
						return Src.Is(Child, Core::Role::InlineText);
					}
				)
			)
		)
	) {
		const FailureReason Reason =
		HasOpenStr || Src.HasMissingBody ? FailureReason::InputSyntaxError : FailureReason::UnrecognizedSyntax;
		Src.SetParseDecision(ParseDecision::Skip, Reason, HasInputError);
		// 構造が信用出来ない為，整形未実施の返戻
		return Src;
	}
	Src.SetParseDecision(ParseDecision::Continue, FailureReason::None, HasInputError);
	// 整形続行を呼出側に通知の返戻
	return Src;
}

/**
 * 壊れたノード (`ERROR` / `MISSING`) の計数関数
 * 計算量：部分木のノード数 N と破損の照会費用 Q に対し O(N + Q)
 * @param Src 走査対象を含むソース
 * @param Node 走査対象のルートノード
 * @param FirstPoint 最初の破損位置の格納先（破損が無ければ先頭，無ければ記録しない）
 * @return 壊れたノードの個数
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
uint32_t SyntaxParser::CountBrokenNodes(const ParsedTree &Src, const TSNode Node, TSPoint *const FirstPoint) {
	// 破損ノードの計数値
	uint32_t Count = 0;
	if(FirstPoint) *FirstPoint = TSPoint{ 0, 0 };
	HasDescendantWithAncestorsOf(
		Node,
		[&Src, &Count, FirstPoint](const TSNode Cur, const std::span<const TSNode> Ancestors) -> bool {
			// 破損ノードの計数
			if(IsBrokenNode(Src, Cur, Ancestors)) {
				if(!Count && FirstPoint) *FirstPoint = ts_node_start_point(Cur);
				++Count;
			}
			// 全ての破損候補を数える迄走査を続ける事の返戻
			return false;
		},
		true
	);
	// 壊れたノードの個数の返戻
	return Count;
}

/**
 * 宣言子のラッパーを透過して関数の成立を確認する関数
 * 計算量：宣言子の深さ D とフィールドの初回の収集費用 Q に対し O(D + Q)
 * @param Src 解析済の木
 * @param Node 本体を持つ定義
 * @param Declarator 最内の宣言子の格納先
 * @param Work 訪問した宣言子の作業量の格納先
 * @return 関数宣言子又は変換演算子の関数形が成立する場合は真
 * @throws std::bad_alloc 直接子又はフィールドの記録を確保出来ない場合
 * @throws std::length_error 記録の要素数上限を超える場合
 */
bool SyntaxParser::ReadFunctionDeclarator(const ParsedTree &Src, const TSNode Node, TSNode &Declarator, size_t &Work) {
	Declarator = Src.GetFieldChild(Node, Core::ChildRole::Declarator);
	bool IsConversion = false;
	while(
		!ts_node_is_null(Declarator) && !Src.Is(Declarator, Core::Role::FunctionDeclarator) &&
		!Src.Is(Declarator, Core::Role::AbstractFunction) && (
			Src.Is(Declarator, Core::Role::NestingDeclarator) || Src.Is(Declarator, Core::Role::PtrOrRefDeclarator) ||
			Src.Is(Declarator, Core::Role::DeclaratorWrapper) || Src.Is(Declarator, Core::Role::OperatorCast) ||
			Src.Is(Declarator, Core::Role::QualifiedIdentifier)
		)
	) {
		++Work;
		IsConversion |= Src.Is(Declarator, Core::Role::OperatorCast);
		TSNode Inner = Src.GetFieldChild(Declarator, Core::ChildRole::Declarator);
		if(ts_node_is_null(Inner)) Inner = Src.GetFieldChild(Declarator, Core::ChildRole::Name);
		if(ts_node_is_null(Inner)) {
			const uint32_t Count = ts_node_named_child_count(Declarator);
			const bool IsLast = Src.Is(Declarator, Core::Role::OperatorCast) || Src.Is(Declarator, Core::Role::QualifiedIdentifier);
			Inner = ts_node_named_child(Declarator, IsLast && Count ? Count - 1 : 0);
		}
		Declarator = Inner;
	}
	// 成立した関数の宣言子の返戻
	if(
		!ts_node_is_null(Declarator) &&
		(Src.Is(Declarator, Core::Role::FunctionDeclarator) || IsConversion && Src.Is(Declarator, Core::Role::AbstractFunction))
	) return true;
	// 関数の宣言子を確認出来ない事の返戻
	return false;
}

/**
 * 文法が要求する関数宣言子を確認出来ない定義の共有計数関数
 * tree-sitter-c は C++ の `class A { ... };` を型 `class`・宣言子 `A` の関数定義と読み，破損を立てない
 * 定義と宣言子の成立を同じ世代の記録へ保存し，入力・完成候補・共有ヘッダの選択で使う
 * 計算量：初回は親子表の収集費用 Q，候補と宣言子の走査量 D，マクロの照会費用 M に対し O(Q + D + M)，以後 O(1)
 * @param Src 確定した C / C++ の文法で解析したソース
 * @return 宣言子が関数の宣言子でない関数定義の個数
 * @throws FormatLimitExceeded 解析の期限又は累積作業量の範囲を超える場合
 * @throws std::runtime_error マクロの宣言子を確かめるパーサーを初期化出来ない場合
 * @throws std::bad_alloc 候補・マクロの記録又は解析のメモリを確保出来ない場合
 * @throws std::length_error 記録又は本文の要素数上限を超える場合
 */
uint32_t SyntaxParser::CountMalformedCDefs(const ParsedTree &Src) {
	// 同じ木の直接子と前処理の状態を共有する
	Src.RecordNodeRelations();
	ParsedTree::NodeRelations &Relations = *Src.Syntax.Relations;
	// 同じ世代で収集済の件数の返戻
	if(Relations.HasFunctionDefFacts) return Relations.MalformedFunctionDefs;
	const size_t Before = Relations.AdmissionWork;
	if(Src.GetReading().HasPreprocMacros) Src.CollectMacroStates();
	std::unordered_map<const ParsedTree::MacroBody *, bool> MacroDeclarators;
	for(const uint32_t Candidate : Relations.FunctionDefCandidates) {
		++Relations.AdmissionWork;
		const TSNode Node = Relations.Children[Relations.Parents.Records[Candidate].Slot].Node;
		TSNode Declarator {};
		if(ReadFunctionDeclarator(Src, Node, Declarator, Relations.AdmissionWork)) continue;
		bool IsUncertain = false, HasMacroDeclarator = false;
		if(Src.GetReading().HasPreprocMacros && !ts_node_is_null(Declarator)) {
			const std::string_view Name = Src.GetView(Declarator);
			const uint32_t Use = Src.GetStart(Declarator);
			const ParsedTree::MacroDef Def = Src.GetMacroDef(Name, Use);
			IsUncertain = Def.State == ParsedTree::MacroState::Unknown;
			if(Def.State == ParsedTree::MacroState::Defined && !Def.IsFunctionLike) {
				ParsedTree::MacroDef Target = Def;
				std::unordered_set<std::string_view> Seen;
				while(Target.State == ParsedTree::MacroState::Defined && !Target.IsFunctionLike && !Target.Alias.empty()) {
					++Relations.AdmissionWork;
					if(!Seen.insert(Target.Alias).second) break;
					Target = Src.GetMacroDef(Target.Alias, Use);
				}
				IsUncertain = Target.State == ParsedTree::MacroState::Unknown;
				if(Target.State == ParsedTree::MacroState::Defined && !Target.IsFunctionLike && Target.Body) {
					const auto [Found, IsNew] = MacroDeclarators.try_emplace(Target.Body.get(), false);
					if(IsNew) {
						// 仮引数の無いマクロの記録済トークンを，関数宣言子の文法で１度だけ確かめる
						// 戻値の型を要求する文法でも宣言子を読める，登録済の型で試行する（入力の戻値型は変えない）
						std::string Header(Src.GetRules().GetSpelling(Core::Word::VoidType));
						Header += ' ';
						const std::string_view Tokens = Target.Body->Signature;
						Relations.AdmissionWork += Tokens.size();
						for(size_t Pos = 1; Pos < Tokens.size();) {
							const size_t End = Tokens.find('\0', Pos);
							if(End == std::string_view::npos) break;
							Header.append(Tokens.substr(Pos, End - Pos)).push_back(' ');
							Pos = End + 1;
						}
						Header += "{}";
						const ParsedTree Trial(Src.GetProfile(), std::move(Header));
						if(!Trial.IsParsed()) {
							if(Trial.HasExpiredParse()) throw FormatLimitExceeded("parsing exceeds the time limit");
							throw std::runtime_error("a declarator probe could not initialize its parser");
						}
						{
							const TSNode Root = Trial.GetRoot();
							Relations.AdmissionWork += ts_node_descendant_count(Root);
							if(!ts_node_has_error(Root) && ts_node_named_child_count(Root) == 1) {
								const TSNode Definition = ts_node_named_child(Root, 0);
								TSNode Inner {};
								Found->second =
								Trial.Is(Definition, Core::Role::FunctionDef) && ReadFunctionDeclarator(Trial, Definition, Inner, Relations.AdmissionWork);
							}
						}
					}
					HasMacroDeclarator = Found->second;
				}
			}
		}
		if(HasMacroDeclarator) continue;
		++Relations.MalformedFunctionDefs;
		std::optional<uint32_t> &Position = IsUncertain ? Relations.UncertainFunctionDef : Relations.MalformedFunctionDef;
		if(!Position) Position = Src.GetStart(Node);
	}
	Src.NameWork.Charge(Relations.AdmissionWork - Before);
	Relations.HasFunctionDefFacts = true;
	// 関数宣言子を確認出来ない定義の件数の返戻
	return Relations.MalformedFunctionDefs;
}
