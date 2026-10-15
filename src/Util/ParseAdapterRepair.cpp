#include "ParseAdapter.hpp"
#include "CharClass.hpp"
#include "FormatLimitExceeded.hpp"
#include "ParsedTree.hpp"
#include "SyntaxParser.hpp"
#include "TextEdit.hpp"
#include "../Core/CommentRepository.hpp"
#include "../Profile/GrammarProfile.hpp"
#include "../Profile/ReadingRules.hpp"
#include <tree_sitter/api.h>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

/**
 * 修復前の名前付ノードの領域の収集関数
 * 計算量：ノード数 V と未収集の親子表の準備費用 Q に対し O(V + Q)
 * @param Src 修復前の解析済ソース
 * @return 隙間では親を使い，幅０のノードを除いた原文順の領域
 * @throws std::bad_alloc 領域又は走査の道筋を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
ParseAdapter::RepairNodeRanges ParseAdapter::CollectRepairNodeRanges(const ParsedTree &Src) {
	// 同じ世代の子列を辿り，隙間へ戻る時の名前付の祖先を保持する
	struct Frame {
		uint32_t End; // 此の本体の終了位置
		TSNode Named; // 隙間を所有する名前付の祖先
		std::span<const ParsedChild> Children; // 記録済の直接子
		size_t Next = 0; // 次に訪ねる子の位置
	};
	const TSNode Root = Src.GetRoot();
	std::vector<Frame> Frames { { Src.GetEnd(Root), Root, Src.GetChildrenOf(Root) } };
	RepairNodeRanges Ranges { { 0, Root } };
	// 走査位置と祖先の事実の記録
	const auto Record = [&Ranges](const uint32_t Pos, const TSNode Named) -> void {
		// 対象の状態の更新
		if(Ranges.back().first == Pos) Ranges.back().second = Named;
		else if(!ts_node_eq(Ranges.back().second, Named)) Ranges.emplace_back(Pos, Named);
		// 終了
		return;
	};
	while(!Frames.empty()) {
		Frame &Parent = Frames.back();
		if(Parent.Next == Parent.Children.size()) {
			const uint32_t End = Parent.End;
			Frames.pop_back();
			if(!Frames.empty()) Record(End, Frames.back().Named);
			continue;
		}
		const ParsedChild &Child = Parent.Children[Parent.Next++];
		// 幅０のノードはバイトを所有せず，子孫へも降りない
		if(Child.GetStart() == Child.End) continue;
		const TSNode Named = Child.IsNamed ? Child.Node : Parent.Named;
		Record(Child.GetStart(), Named);
		Frames.push_back({ Child.End, Named, Src.GetChildrenAt(Child.Relation) });
	}
	// 原文順の領域の返戻
	return Ranges;
}

/**
 * 修復前の位置に属する最も内側の名前付ノードの取得関数
 * 計算量：ノード数 V，領域数 R に対し初回 O(V)，以後 O(log(R + 1))
 * @param Src 修復前の解析済ソース
 * @param Pos 修復前の原文のバイト位置
 * @param Ranges 初回の照会で収集する原文順の領域
 * @return 位置を含む最も内側の名前付ノード
 * @throws std::bad_alloc 初回の領域収集のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
TSNode ParseAdapter::RepairNodeAt(const ParsedTree &Src, const uint32_t Pos, std::optional<RepairNodeRanges> &Ranges) {
	// 修復前の位置に属する最も内側の名前付ノードの取得
	if(!Ranges) Ranges = CollectRepairNodeRanges(Src);
	const RepairNodeRanges::const_iterator After = std::upper_bound(
		Ranges->begin(),
		Ranges->end(),
		Pos,
		[](const uint32_t Offset, const RepairNodeRange &Range) -> bool {
			// 位置が領域より前かの返戻
			return Offset < Range.first;
		}
	);
	// 位置を含む名前付ノードの返戻
	return std::prev(After)->second;
}

/**
 * Ruby の終端の無い範囲の次の行の結合の収集関数
 * 範囲演算子の後の改行をパーサーが文末と読む為，隙間を解析用の空白へ置き換え，１つの範囲へ読ませる
 * 間の通常コメントは退避し，解析用の空白を投影で本来のトークン間へデコードする
 * 計算量：後続の隙間の長さ L，コメント数 C，領域数 R，初回の領域収集の費用 Q に対し O(L + C log(R + 1) + Q)
 * @param Src ソースコード
 * @param Node 範囲の候補
 * @param Ranges 初回の位置照会で収集する原文順の名前付ノードの領域
 * @param Folds 解析用の空白への置換の格納先
 * @param Restorations 空白の保護を解く原文の位置とテキストの格納先
 * @throws std::bad_alloc 編集又は退避するコメントの確保に失敗した場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void ParseAdapter::CollectRubyOpenRange(
	ParsedTree &Src,
	const TSNode Node,
	std::optional<RepairNodeRanges> &Ranges,
	std::vector<TextEdit> &Folds,
	std::vector<LexemeRestoration> &Restorations
) {
	// 範囲の後で節を開く語の取得
	const std::vector<std::string_view> &ClauseWords = Src.GetReading().ClauseWords;
	// 範囲でないノードの場合の終了
	if(!Src.Is(Node, Core::Role::Range)) return;
	// 式末尾の演算子ノード
	const TSNode Operator = ts_node_child(Node, ts_node_child_count(Node) - 1);
	// 末尾が範囲の演算子でない場合の終了
	if(ts_node_is_named(Operator) || !Src.Is(Operator, Core::Role::RangeOperator)) return;
	const uint32_t After = TextEdit::SkipSpaceRight(Src, Src.GetReading(), Src.GetEnd(Operator));
	// 演算子の後が改行でない場合の終了
	if(After >= Src.size() || Src[After] != '\n') return;
	// 次の実引数開始候補
	uint32_t Next = After;
	// 間に在る独立コメントの本文（畳むと行の残りが其の本文へ入る為，パーサーへ渡さずに退避する）
	std::vector<std::string_view> Between;
	// 空白と独立コメントを越えて実引数先頭へ移動
	while(Next < Src.size()) {
		// 空白と改行の読飛
		if(Src[Next] == '\n' || Src[Next] == ' ' || Src[Next] == '\t') {
			++Next;
			continue;
		}
		// `#` 以外に達した事の脱出
		if(Src[Next] != '#') break;
		// 其の位置で始まるノード（ヒアドキュメントの本文の `#` をコメントと取違えない）
		// コメントでない事の脱出
		if(const TSNode At = RepairNodeAt(Src, Next, Ranges); ts_node_is_null(At) || !Src.Is(At, Core::Role::Comment)) break;
		// 其の行の終わり
		const uint32_t LineEnd = static_cast<uint32_t>(std::min(Src.find('\n', Next), Src.size()));
		Between.push_back(std::string_view(Src.data() + Next, LineEnd - Next));
		Next = LineEnd;
	}
	// 次の実引数が無いか閉括弧・区切の場合の終了
	if(Next >= Src.size() || std::string_view(")]};").find(Src[Next]) != std::string_view::npos) return;
	uint32_t WordEnd = Next;
	// 実引数先頭語の終端迄走査
	while(WordEnd < Src.size() && IsIdentifierChar(Src[WordEnd])) ++WordEnd;
	// 実引数の先頭語が節の語の場合の終了
	if(std::ranges::find(ClauseWords, std::string_view(Src.data() + Next, WordEnd - Next)) != ClauseWords.end()) return;
	// コメントを畳んだ空白へ取り込まず，パーサーへ１つの範囲を伝える
	const uint32_t Start = Src.GetEnd(Operator);
	TextEdit::Push(Start, Next, " ", Folds);
	// 次が `.` の時だけデコード後も空白を残し，範囲の演算子とのトークンの結合を避ける
	if(Src[Next] != '.') Restorations.push_back({ Start, Next, "", false });
	// 独立コメントの畳んだ位置への退避（種別を変えず，置けるかと配置はコメント経路が決める，整形仕様 7.6）
	for(const std::string_view Text : Between) {
		Src.EscapedComments.push_back({ Src.GetEnd(Operator), std::string(Text), CommentKind::OwnLine });
	}
	// 終了
	return;
}

/**
 * Kotlin の改行を跨ぐ継目の区切編集の収集関数
 * Kotlin で文境界を跨ぐ誤結合を防ぐセミコロン補完関数
 * 計算量：先頭の子の列の長さ D，直接の子数 C，前の空白長 L，修復区間の登録費用の費用 Q に対し O(D + C + L + Q)
 * @param Src 整形対象ソース
 * @param Node 呼出の接尾辞・参照・返戻のノード（祖先の括弧又は when の条件で文境界を禁止して居ない物）
 * @param Edits 編集列追加先
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void ParseAdapter::CollectKotlinJointEdit(const ParsedTree &Src, const TSNode Node, std::vector<TextEdit> &Edits) {
	// 折返を補う継目の取得
	uint32_t Joint = Src.GetStart(Node);
	TSNode Lead {};
	// 呼出可能参照でない接尾の場合の終了
	if(Src.Is(Node, Core::Role::NavigationSuffix) && !Src.Is(ts_node_child(Node, 0), Core::Role::DoubleColonToken)) return;
	// 呼出可能参照の場合
	else if(Src.Is(Node, Core::Role::CallableRef)) {
		// 受手の無い参照と直前式の非接続
		TSNode Colons {};
		bool IsReceiver = false;
		ForEachChild(
			Node,
			// 呼出可能参照の各子を走査
			[&](const TSNode Child) -> bool {
				// `::` トークン又は名前付受手の検出
				if(!ts_node_is_named(Child) && Src.Is(Child, Core::Role::DoubleColonToken)) {
					Colons = Child;
					// `::` の発見で打ち切る事の返戻
					return false;
				}
				// 名前付の受手通過を記録
				IsReceiver = IsReceiver || ts_node_is_named(Child);
				// 次の子へ進む事の返戻
				return true;
			}
		);
		// 受手の無い参照の終了
		if(!IsReceiver || ts_node_is_null(Colons)) return;
		// 継目を `::` の開始へ更新
		Joint = Src.GetStart(Colons);
	// ジャンプ式の場合
	} else if(Src.Is(Node, Core::Role::JumpExpr)) {
		const std::string_view Keyword = Src.GetView(ts_node_child(Node, 0));
		const TSNode Val = ts_node_named_child(Node, ts_node_named_child_count(Node) - 1);
		// 値を返さない脱出の場合の終了
		if(
			Keyword != Src.GetRules().GetSpelling(Core::Word::Return) && Keyword != Src.GetRules().GetSpelling(Core::Word::LabeledReturn) ||
			ts_node_is_null(Val) || Src.Is(Val, Core::Role::JumpLabel)
		) return;
		// 継目を返戻値の開始へ更新
		Joint = Src.GetStart(Val);
		// 返戻値を先頭式候補へ設定
		Lead = Val;
	// 中置式の場合
	} else if(Src.Is(Node, Core::Role::NamedInfix)) {
		// Kotlin の改行後の単独呼出の文分離（裸の右辺は元から文でない為に除外）
		const TSNode Name = ts_node_named_child(Node, 1), Right = ts_node_named_child(Node, 2);
		if(ts_node_is_null(Name) || ts_node_is_null(Right) || Src[Src.GetStart(Right)] != '(' && Src[Src.GetStart(Right)] != '{') {
			// 呼出の形でない物の終了
			return;
		}
		// 継目を中置演算子名へ更新
		Joint = Src.GetStart(Name);
	} else {
		const TSNode Args = ts_node_named_child(Node, 0);
		// 単一のトークンを持つ単一の実引数でない場合の終了
		if(
			ts_node_is_null(Args) || !Src.Is(Args, Core::Role::ValArgs) || ts_node_named_child_count(Args) != 1 ||
			ts_node_child_count(ts_node_named_child(Args, 0)) != 1
		) return;
		// 実引数を先頭式候補へ設定
		Lead = Args;
	}
	// 継目の空白遡及に依る文末改行の確認
	bool HasNewline = false;
	// 継目前の空白を後退走査
	for(; Joint && (Src[Joint - 1] == ' ' || Src[Joint - 1] == '\t' || Src[Joint - 1] == '\n'); --Joint) {
		// 改行の発見を記録
		if(Src[Joint - 1] == '\n') HasNewline = true;
	}
	// 改行を含まない場合の終了
	if(!HasNewline) return;
	// 誤って繋がった文の区切の挿入
	TextEdit::Push(Joint, Joint, std::string(Src.GetRules().GetSpelling(Core::Word::StmtEnd)), Edits);
	// tree-sitter-kotlin が読めない文頭の冗長括弧の除去
	// 先頭子へ移動
	while(!ts_node_is_null(Lead) && ts_node_child_count(Lead) && !Src.Is(ts_node_child(Lead, 0), Core::Role::OpeningParenToken)) {
		Lead = ts_node_child(Lead, 0);
	}
	// 先頭が単一の子を持たない場合の終了
	if(ts_node_is_null(Lead) || ts_node_named_child_count(Lead) != 1) return;
	// 括弧省略候補の実引数名
	TSNode Name = ts_node_named_child(Lead, 0);
	// 値引数ラッパーを剥離
	if(Src.Is(Name, Core::Role::ValArg)) Name = ts_node_named_child(Name, 0);
	// 単純識別子の単独実引数の場合
	if(!ts_node_is_null(Name) && Src.Is(Name, Core::Role::SimpleName)) {
		const TSNode Open = ts_node_child(Lead, 0), Close = ts_node_child(Lead, ts_node_child_count(Lead) - 1);
		TextEdit::Push(Src.GetStart(Open), Src.GetEnd(Open), "", Edits);
		TextEdit::Push(Src.GetStart(Close), Src.GetEnd(Close), "", Edits);
	}
	// 終了
	return;
}

/**
 * Swift の演算子に密着したコメントの編集の収集関数
 * 計算量：ソース長 N，訪ねるノード数 V，候補数 K に対し O(N + V + K log(V + 1))
 * @param Src 修復前の解析済ソース
 * @param OperatorChars Swift の演算子を成す字
 * @return コメントの開きを区切る空白の挿入列
 * @throws std::bad_alloc 領域・祖先の事実又は編集を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::vector<TextEdit> ParseAdapter::CollectSwiftCommentEdits(const ParsedTree &Src, const std::string_view OperatorChars) {
	// 修復前のテキストから全ての密着位置を１度だけ集め，本文の中の候補を読み飛ばす
	const TSNode Root = Src.GetRoot();
	const std::string_view Text = Src.GetCodeText();
	// コメントの開きの候補を原文順に集め，候補の無い部分木の文脈を作らない
	std::vector<uint32_t> Candidates;
	for(size_t Pos = 0; Pos + 1 < Text.size(); ++Pos) if(Text[Pos] == '/' && (Text[Pos + 1] == '/' || Text[Pos + 1] == '*')) {
		Candidates.push_back(static_cast<uint32_t>(Pos));
	}
	// 本文にコメントの開きが無い場合の返戻
	if(Candidates.empty()) return {};
	// 補間は祖先の事実を切り，其の内側だけのリテラル・コメント・演算子を原文順の領域へ記録する
	struct CommentCtx {
		size_t Literal;
		bool IsParsedComment;
		std::optional<uint32_t> OperatorStart;
	};
	// 補間内の不透明なトークンの文脈
	struct LiteralCtx {
		size_t Parent;
		uint32_t Start;
		std::optional<bool> IsInStr;
	};
	// Swift の密着コメントのトークン文脈を収集する走査枠
	struct Frame {
		TSNode Node;
		CommentCtx Ctx;
	};
	std::vector<LiteralCtx> Literals = { { 0, 0, false } };
	// ノードを包むトークンの文脈の取得
	const auto CtxOf = [&Src, &Literals](CommentCtx Ctx, const TSNode Node) -> CommentCtx {
		// 補間自身と其の外側の事実を除いた領域の返戻
		if(Src.Is(Node, Core::Role::StrInterpolation)) return {};
		if(Src.Is(Node, Core::Role::OpaqueLiteral)) {
			Literals.push_back({ Ctx.Literal, Src.GetStart(Node), std::nullopt });
			Ctx.Literal = Literals.size() - 1;
		}
		Ctx.IsParsedComment = Ctx.IsParsedComment || Src.Is(Node, Core::Role::Comment);
		// 内側から遡る元の判定が最後に選ぶ，補間より内側の最も外の演算子の保持
		if(!Ctx.OperatorStart && Src.Is(Node, Core::Role::UserDefinedOperator)) Ctx.OperatorStart = Src.GetStart(Node);
		// 祖先と対象自身の事実の返戻
		return Ctx;
	};
	const CommentCtx RootCtx = CtxOf({}, Root);
	std::vector<Frame> Frames = { { Root, RootCtx } };
	using CtxRange = std::pair<uint32_t, CommentCtx>;
	std::vector<CtxRange> CtxRanges = { { 0, RootCtx } };
	// 文脈が変わる位置だけを記録し，同じ状態の直接子と祖先の復帰で列を伸ばさない
	const auto RecordCtx = [&CtxRanges](const CommentCtx &Ctx, const uint32_t Pos) -> void {
		const CtxRange &Prev = CtxRanges.back();
		if(Prev.first == Pos) CtxRanges.back().second = Ctx;
		else if(
			Pos < Prev.first || Prev.second.Literal != Ctx.Literal || Prev.second.IsParsedComment != Ctx.IsParsedComment ||
			Prev.second.OperatorStart != Ctx.OperatorStart
		) CtxRanges.emplace_back(Pos, Ctx);
		// 終了
		return;
	};
	size_t NextCandidate = 0;
	WalkRecordedChildren(
		Src,
		Root,
		[&](const TSNode Node, const std::span<const TSNode> Ancestors) -> bool {
			while(Frames.size() > Ancestors.size()) {
				const uint32_t End = Src.GetEnd(Frames.back().Node);
				Frames.pop_back();
				RecordCtx(Frames.back().Ctx, End);
			}
			// 候補を含まない部分木は，候補位置の文脈を変えない
			const uint32_t Start = Src.GetStart(Node), End = Src.GetEnd(Node);
			// カーソルは原文順の為，候補列も後退せずに１度だけ進める
			while(NextCandidate < Candidates.size() && Candidates[NextCandidate] < Start) ++NextCandidate;
			// 候補位置を所有しない部分木を辿らない事の返戻
			if(NextCandidate == Candidates.size() || Candidates[NextCandidate] >= End) return false;
			const CommentCtx Ctx = CtxOf(Frames.back().Ctx, Node);
			Frames.push_back({ Node, Ctx });
			RecordCtx(Ctx, Start);
			// 子の領域も記録する事の返戻
			return true;
		}
	);
	while(Frames.size() > 1) {
		const uint32_t End = Src.GetEnd(Frames.back().Node);
		Frames.pop_back();
		RecordCtx(Frames.back().Ctx, End);
	}
	std::vector<TextEdit> Inserted;
	std::vector<std::pair<uint32_t, uint32_t>> ConsumedComments;
	std::vector<size_t> PendingLiterals;
	for(size_t Next = 0, Pos = 0; Next < Candidates.size();) {
		// 既読のコメントの内側に在る候補は，元の本文走査と同じく飛ばす
		if(Candidates[Next] < Pos) {
			++Next;
			continue;
		}
		Pos = Candidates[Next++];
		const std::vector<CtxRange>::const_iterator AfterCtx = std::upper_bound(
			CtxRanges.begin(),
			CtxRanges.end(),
			static_cast<uint32_t>(Pos),
			[](const uint32_t Offset, const CtxRange &Range) -> bool {
				// 候補が領域より前かの返戻
				return Offset < Range.first;
			}
		);
		const CommentCtx &Ctx = std::prev(AfterCtx)->second;
		// リテラルの開始位置を越えた後では，其の位置を含む既読コメントの有無は変わらない
		// 各リテラルを１度だけ判定し，外側の判定と共に内側へ伝える
		size_t Literal = Ctx.Literal;
		PendingLiterals.clear();
		while(!Literals[Literal].IsInStr) {
			PendingLiterals.push_back(Literal);
			Literal = Literals[Literal].Parent;
		}
		bool IsInStr = *Literals[Literal].IsInStr;
		for(std::vector<size_t>::reverse_iterator Pending = PendingLiterals.rbegin(); Pending != PendingLiterals.rend(); ++Pending) {
			LiteralCtx &Cur = Literals[*Pending];
			const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator After = std::upper_bound(
				ConsumedComments.begin(),
				ConsumedComments.end(),
				Cur.Start,
				[](const uint32_t Offset, const std::pair<uint32_t, uint32_t> &Range) -> bool {
					// リテラルがコメントより前に始まるかの返戻
					return Offset < Range.first;
				}
			);
			IsInStr = IsInStr || After == ConsumedComments.begin() || Cur.Start >= std::prev(After)->second;
			Cur.IsInStr = IsInStr;
		}
		const bool IsParsedComment = Ctx.IsParsedComment, HasOperator = Ctx.OperatorStart.has_value();
		const uint32_t OperatorStart = Ctx.OperatorStart.value_or(static_cast<uint32_t>(Pos));
		// 文字列内のテキストはコメントを開かない
		if(IsInStr) {
			++Pos;
			continue;
		}
		const uint32_t CommentStart = static_cast<uint32_t>(Pos);
		const bool IsLine = Text[Pos + 1] == '/';
		if(!IsParsedComment) {
			if(
				HasOperator && OperatorStart < Pos || Pos && Text[Pos - 1] != '/' && OperatorChars.find(Text[Pos - 1]) != std::string_view::npos
			) Inserted.push_back({ static_cast<uint32_t>(Pos), static_cast<uint32_t>(Pos), " " });
			if(IsLine && HasOperator) {
				// コメントの開きの連続した斜線を演算子と読む形も同じ編集へ纏める
				size_t SlashEnd = Pos + 2;
				while(SlashEnd < Text.size() && Text[SlashEnd] == '/') ++SlashEnd;
				if(SlashEnd < Text.size() && Text[SlashEnd] == '*') {
					Inserted.push_back({ static_cast<uint32_t>(SlashEnd), static_cast<uint32_t>(SlashEnd), " " });
				}
			}
		}
		if(IsLine) {
			Pos = std::min(Text.find('\n', Pos + 2), Text.size());
			ConsumedComments.emplace_back(CommentStart, static_cast<uint32_t>(Pos));
			continue;
		}
		// Swift のネストしたブロックコメントの，修復前の本文から終端迄読飛
		size_t Depth = 1;
		Pos += 2;
		while(Depth && Pos + 1 < Text.size()) {
			if(Text[Pos] == '/' && Text[Pos + 1] == '*') {
				++Depth;
				Pos += 2;
			} else if(Text[Pos] == '*' && Text[Pos + 1] == '/') {
				--Depth;
				Pos += 2;
			} else ++Pos;
		}
		ConsumedComments.emplace_back(CommentStart, static_cast<uint32_t>(Pos));
	}
	// １度の走査で集めた編集の返戻
	return Inserted;
}

/**
 * Ruby の曖昧な実引数の読みの統一関数
 * 其れに，デコードデータ G の整列 O(G log(G + 1)) と編集１回分を加える
 * Ruby の空白で実引数に為る式を構文木へ反映する
 * tree-sitter-ruby は其の一部を二項式・添字・範囲・受信子と読み，整形で空白を揃えると読みが変わる為，他の編集より前に読みを Ruby へ揃える
 * 必須の括弧は実引数列全体へ移し，内側の整理を後段へ委ねる
 * 計算量：ノード数 N，最大の祖先数 D，名前と隙間の総長 L，位置照会数 K に対し平均 O(N D + L + K log(N + 1))
 * @param Src ソースコード
 * @throws FormatLimitExceeded 修復後の解析が期限を超えた場合
 * @throws std::length_error 修復後のテキストがパーサーの位置の範囲を超えた場合
 * @throws std::bad_alloc ローカルの宣言・編集又は本文の境界の確保に失敗した場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void ParseAdapter::RepairRubyCommandArgs(ParsedTree &Src) {
	// 文字参照を含む空白の判定
	const auto IsBlank = [&Src](const uint32_t LocalPos) -> bool {
		// 指定位置が空白かの返戻
		return LocalPos < Src.size() && (Src[LocalPos] == ' ' || Src[LocalPos] == '\t' || Src[LocalPos] == '\n');
	};
	// 実引数の修復の文脈
	struct ArgCtx {
		TSNode Top;
		TSNode List;
		TSNode Scope;
	};
	std::optional<RepairNodeRanges> Ranges;
	std::vector<ArgCtx> Contexts = { { Src.GetRoot(), {}, {} } };
	ArgCtx Cur {};
	// 実引数の終端の取得
	const auto ArgEnd = [&Src, &Cur]() -> uint32_t {
		if(!ts_node_is_null(Cur.List) && Src.Is(Cur.List, Core::Role::ElementSeq)) {
			// 実引数列の最終引数終端の返戻
			return Src.GetEnd(ts_node_named_child(Cur.List, ts_node_named_child_count(Cur.List) - 1));
		}
		// 単独引数の終端の返戻
		return Src.GetEnd(Cur.Top);
	};
	// 修復する実引数の括弧の記録
	const auto Wrap = [](const uint32_t From, const uint32_t To, const uint32_t End, std::vector<TextEdit> &Into) -> void {
		// 実引数前への開括弧挿入
		TextEdit::Push(From, To, "(", Into);
		TextEdit::Push(End, End, ")", Into);
	};
	// 誤読された逐語本文を除き，補間コードと通常コードの実引数の境界を求める
	using LocalNames = std::unordered_map<std::string_view, uint32_t>;
	std::unordered_map<const void *, LocalNames> Locals;
	std::unordered_map<const void *, TSNode> OuterScopes;
	// ローカルの名前の導入の判定
	const auto IsLocalDefined = [&](const std::string_view Name, const uint32_t Start) -> bool {
		for(TSNode Owner = Cur.Scope; !ts_node_is_null(Owner); Owner = OuterScopes.at(Owner.id)) {
			if(const std::unordered_map<const void *, LocalNames>::const_iterator Scope = Locals.find(Owner.id); Scope != Locals.end()) {
				if(const LocalNames::const_iterator Found = Scope->second.find(Name); Found != Scope->second.end() && Found->second <= Start) {
					// 見える範囲で先に導入した名前かの返戻
					return true;
				}
			}
			// 外側を見ない範囲の探索の終了
			if(Src.Is(Owner, Core::Role::IsolatedScope)) break;
		}
		// 先に導入したローカル変数でない事の返戻
		return false;
	};
	std::vector<TextEdit> Edits;
	std::vector<LexemeRestoration> RangeRestorations;
	std::vector<std::pair<uint32_t, uint32_t>> MisreadBodies;
	// ヒアドキュメントの本文範囲の記録
	const auto RecordHeredocBody = [&](const TSNode Operator) -> void {
		// 前進する走査位置
		size_t Pos = Src.GetEnd(Operator);
		const bool DoesAllowIndent = Pos < Src.size() && (Src[Pos] == '-' || Src[Pos] == '~');
		if(DoesAllowIndent) ++Pos;
		const unsigned char Quote = Pos < Src.size() && (Src[Pos] == '\'' || Src[Pos] == '"' || Src[Pos] == '`') ? Src[Pos++] : '\0';
		const size_t NameStart = Pos;
		if(Quote) while(Pos < Src.size() && Src[Pos] != Quote && Src[Pos] != '\n') ++Pos;
		else while(Pos < Src.size() && IsIdentifierChar(Src[Pos])) ++Pos;
		const std::string_view Name = std::string_view(Src).substr(NameStart, Pos - NameStart);
		// ヒアドキュメントの開始を持たないシフト式の終了
		if(!Quote && Name.empty() || Quote && (Pos == Src.size() || Src[Pos] != Quote)) return;
		const size_t HeaderEnd = Src.find('\n', Pos);
		// 本文の無い開始の終了
		if(HeaderEnd == std::string::npos) return;
		const size_t BodyStart = std::max(HeaderEnd + 1, MisreadBodies.empty() ? size_t{ 0 } : size_t{ MisreadBodies.back().second });
		for(size_t LineStart = BodyStart; LineStart < Src.size();) {
			const size_t LineEnd = std::min(Src.find('\n', LineStart), Src.size());
			size_t Lead = LineStart;
			if(DoesAllowIndent) while(Lead < LineEnd && (Src[Lead] == ' ' || Src[Lead] == '\t')) ++Lead;
			if(std::string_view(Src).substr(Lead, LineEnd - Lead) == Name) {
				MisreadBodies.emplace_back(static_cast<uint32_t>(BodyStart), static_cast<uint32_t>(LineEnd));
				// 原文から本文の終わりを確定した後の終了
				return;
			}
			LineStart = LineEnd + 1;
		}
		// 閉じの無い本文は後段の入力受理判定へ委ねる
		// 終了
		return;
	};
	// 修復対象の部品の収集
	const auto Collect = [&](const TSNode Node) -> void {
		// 命令の実引数を持つノードでない場合の終了
		if(!Src.Is(Node, Core::Role::CommandArgHost)) return;
		// 括弧付実引数を持つ候補の場合
		if(Src.Is(Node, Core::Role::ArgListHost)) {
			const bool IsReceiverCall = Src.Is(Node, Core::Role::ReceiverCall);
			const TSNode Args = IsReceiverCall ? Src.GetFieldChild(Node, Core::ChildRole::Args) : ts_node_named_child(Node, 0);
			// 実引数を持たない場合の終了
			if(ts_node_is_null(Args) || !Src.Is(Args, Core::Role::ArgSeq) || !ts_node_named_child_count(Args)) return;
			// 実引数列の子数と範囲
			const uint32_t Count = ts_node_named_child_count(Args), Begin = Src.GetStart(Args), End = Src.GetEnd(Args);
			const TSNode First = ts_node_named_child(Args, 0);
			// 明示括弧を持つ実引数列の場合
			if(Src.GetStart(First) != Begin) {
				// return の単一の実引数でない物と改行付の括弧を保つ場合（此の後で最初の実引数の括弧を外し，曖昧な後続は共に包む）の終了
				if(!Src.Is(Node, Core::Role::ValTakingJump) || Count != 1 || std::memchr(Src.data() + Begin, '\n', End - Begin)) return;
				// 括弧除去後も先頭が曖昧でない場合
				if(!Src.HasAmbiguousRubyLead(First)) {
					TextEdit::Push(Begin, Begin + 1, " ", Edits);
					TextEdit::Push(End - 1, End, "", Edits);
				// 外側の呼出範囲を括弧で保護
				} else if(const TSNode Top = Cur.Top; !ts_node_eq(Top, Node)) Wrap(Begin, Begin, Src.GetEnd(Top), Edits);
				// 明示括弧を持つ実引数の修復の終了
				return;
			}
			const TSNode Block = IsReceiverCall ? Src.GetFieldChild(Node, Core::ChildRole::BlockField) : TSNode{};
			// 受手無の呼出名が先に導入したローカル変数かの判定（括弧で始まる実引数の場合だけ探索する）
			const auto IsLocalHead = [&]() -> bool {
				const TSNode Method = IsReceiverCall ? Src.GetFieldChild(Node, Core::ChildRole::MethodField) : TSNode{};
				// ローカル変数の呼出名かの返戻
				return !ts_node_is_null(Method) && ts_node_is_null(Src.GetFieldChild(Node, Core::ChildRole::ReceiverField)) &&
				Src.Is(Method, Core::Role::Identifier) && IsLocalDefined(Src.GetView(Method), Src.GetStart(Node));
			};
			// 親の子の１回走査に依る前兄弟の取得（根からの引直の２乗費用を回避）
			TSNode Name {}, Prev {};
			ForEachChild(
				Node,
				// 実引数直前の呼出名を探索
				[&](const TSNode Child) -> bool {
					// 実引数到達時の直前子記録と現在子への更新
					if(ts_node_eq(Child, Args)) Name = Prev;
					Prev = Child;
					// 実引数の並びへ届いたら走査を打ち切る事の返戻
					return !ts_node_eq(Child, Args);
				}
			);
			// 直前の子が無い場合に，空ノードの終端０からソースを潰さない為の終了
			if(ts_node_is_null(Name)) return;
			if(
				// 裸の実引数列を括弧で保護
				Src[Begin] == '(' && (Src.HasAmbiguousRubyLead(First) || IsLocalHead()) ||
				!ts_node_is_null(Block) && Src[Src.GetStart(Block)] == '{' &&
				Src.Is(ts_node_named_child(Args, Count - 1), Core::Role::GroupingParen)
			) Wrap(Src.GetEnd(Name), Begin, End, Edits);
			// return 前置でのラベル誤認防止用空白の挿入
			else if(Src[Begin] == ':' && Src.GetEnd(Name) == Begin) TextEdit::Push(Begin, Begin, " ", Edits);
			// 裸の実引数の修復の終了
			return;
		}
		const TSNode Head = ts_node_named_child(Node, 0);
		const bool IsBinary = Src.Is(Node, Core::Role::Binary);
		TSNode Next {};
		// 二項式以外の後続子を探索
		if(!IsBinary && !ts_node_is_null(Head)) {
			// 先頭通過済のマーカー
			bool IsAfterHead = false;
			ForEachChild(
				Node,
				// 各子の先頭相対位置を確認
				[&](const TSNode Child) -> void {
					// 先頭通過状態と直後の子の記録
					if(IsAfterHead && ts_node_is_null(Next)) Next = Child;
					if(ts_node_eq(Child, Head)) IsAfterHead = true;
				}
			);
		}
		const TSNode Operator = IsBinary ? Src.GetFieldChild(Node, Core::ChildRole::OperatorField) : Next;
		// 先頭か演算子が無い場合の終了
		if(ts_node_is_null(Head) || ts_node_is_null(Operator)) return;
		// 先頭型と演算子のテキスト
		const std::string_view OperatorText = Src.GetView(Operator);
		// 実引数開始キーワードの場合
		if(Src.Is(Head, Core::Role::ValTakingJump)) {
			// 実引数を解析済の場合（此の後で return 後の実引数を修復し，+・範囲・数値始まりは括弧化し，他は符号を密着）の終了
			if(ts_node_named_child_count(Head)) return;
			if(
				// 曖昧なキーワード引数を括弧で保護
				const TSNode Right = IsBinary ? Src.GetFieldChild(Node, Core::ChildRole::Right) : TSNode{};
				OperatorText == Src.GetRules().Operators.Plus || Src.Is(Node, Core::Role::Range) ||
				OperatorText == Src.GetRules().Operators.Minus && !Src.CanAttachRubySign(Right)
			) Wrap(Src.GetEnd(Head), Src.GetStart(Operator), ArgEnd(), Edits);
			// Ruby 実引数開始演算子の場合
			else if(Src.Is(Operator, Core::Role::ArgStartToken)) {
				// 呼出名と演算子の間を単一空白化
				TextEdit::Push(Src.GetEnd(Head), Src.GetStart(Operator), " ", Edits);
				// 演算子と右辺を密着
				if(!ts_node_is_null(Right)) TextEdit::Push(Src.GetEnd(Operator), Src.GetStart(Right), "", Edits);
			}
			// キーワード直後の実引数の修復の終了
			return;
		}
		// 名前の後に空白が有り演算子の直後に空白が無い形（添字の `[` は前の空白だけで実引数に為る）でない場合の終了
		// 実引数を取れない呼出先の場合（実引数を取るのはローカル変数でない名前か，実引数の無いメソッドの呼出 (`foo.bar +1`)・`yield`・`super`）の終了
		if(
			!Src.Is(Operator, Core::Role::ArgStartToken) || !IsBlank(Src.GetStart(Operator) - 1) ||
			IsBinary && IsBlank(Src.GetEnd(Operator)) || [&]() -> bool {
				// 先頭の名前がローカル値でなく呼出を導くかの判定
				const bool IsMethod = Src.Is(Head, Core::Role::Identifier) ?
				!IsLocalDefined(Src.GetView(Head), Src.GetStart(Node)) :
				Src.Is(Head, Core::Role::ReceiverCall) ?
				ts_node_is_null(Src.GetFieldChild(Head, Core::ChildRole::Args)) &&
				ts_node_is_null(Src.GetFieldChild(Head, Core::ChildRole::BlockField)) :
				Src.Is(Head, Core::Role::SuperRef) || Src.Is(Head, Core::Role::YieldVal) && !ts_node_named_child_count(Head);
				return !IsMethod;
			}()
		) return;
		// シフト以外の曖昧な実引数を括弧で保護
		if(!Src.Is(Operator, Core::Role::HeredocAmbiguousShift)) Wrap(Src.GetEnd(Head), Src.GetStart(Operator), ArgEnd(), Edits);
		// ヒアドキュメントの開始だけを括弧で保護
		else {
			RecordHeredocBody(Operator);
			const uint32_t End = static_cast<uint32_t>(std::min(Src.find('\n', Src.GetEnd(Operator)), size_t{ Src.GetEnd(Cur.Top) }));
			Wrap(Src.GetEnd(Head), Src.GetStart(Operator), End, Edits);
		}
	};
	const auto CanDescendAfterRecording = [&](const TSNode Node, const TSNode Parent, const size_t Depth) -> bool {
		Contexts.resize(Depth);
		const ArgCtx &Above = Contexts.back();
		const bool IsArgExtent = Src.Is(Parent, Core::Role::CommandArgExtent) && ts_node_eq(ts_node_named_child(Parent, 0), Node) &&
		!Src.Is(Src.GetFieldChild(Parent, Core::ChildRole::OperatorField), Core::Role::LowPrecBinaryKeyword);
		Cur = {
			IsArgExtent ? Above.Top : Node,
			IsArgExtent ? Above.List : Parent,
			Src.Is(Parent, Core::Role::LocalScope) ? Parent : Above.Scope
		};
		if(Src.Is(Parent, Core::Role::LocalScope)) OuterScopes.try_emplace(Parent.id, Above.Scope);
		Contexts.push_back(Cur);
		// 誤読された逐語本文の式と宣言を取り込まない事の返戻
		if(
			const uint32_t Start = Src.GetStart(Node);
			!MisreadBodies.empty() && MisreadBodies.back().first <= Start && Start < MisreadBodies.back().second
		) return false;
		const TSNode Scope = Cur.Scope;
		// 名前の導入位置の記録関数
		const auto Record = [&](const TSNode Identifier) -> void {
			// 名前を持たない導入と有効範囲の外の終了
			if(ts_node_is_null(Scope) || ts_node_is_null(Identifier) || !Src.Is(Identifier, Core::Role::Identifier)) return;
			Locals[Scope.id].try_emplace(Src.GetView(Identifier), Src.GetEnd(Identifier));
			// 終了
			return;
		};
		if(Src.Is(Node, Core::Role::LocalParams)) {
			ForEachNamedChild(
				Node,
				[&](const TSNode Param) -> void {
					Record(Src.Is(Param, Core::Role::Identifier) ? Param : ts_node_named_child(Param, 0));
					// 終了
					return;
				}
			);
		} else if(
			const TSNode Left = Src.Is(Node, Core::Role::BindingExpr) ? Src.GetFieldChild(Node, Core::ChildRole::Left) : TSNode{};
			!ts_node_is_null(Left)
		) {
			if(Src.Is(Left, Core::Role::LeftAssignmentList)) ForEachNamedChild(Left, Record);
			else Record(Left);
		}
		CollectRubyOpenRange(Src, Node, Ranges, Edits, RangeRestorations);
		Collect(Node);
		// 正しく読まれた本文の補間内のコードも辿る事の返戻
		return true;
	};
	// 記録済の世代だけ子列を共有し，修復で捨てる未記録の世代へ表を追加しない
	if(Src.Syntax.HasParents) {
		struct Frame {
			TSNode Node; // 直接の親
			std::span<const ParsedChild> Children; // 未訪問の直接子
		};
		std::vector<Frame> Pending { { Src.GetRoot(), Src.GetChildrenAt(0) } };
		while(!Pending.empty()) {
			Frame &Parent = Pending.back();
			if(Parent.Children.empty()) {
				Pending.pop_back();
				continue;
			}
			const ParsedChild &Record = Parent.Children.front();
			Parent.Children = Parent.Children.subspan(1);
			if(!CanDescendAfterRecording(Record.Node, Parent.Node, Pending.size())) continue;
			const std::span<const ParsedChild> Children = Src.GetChildrenAt(Record.Relation);
			if(!Children.empty()) Pending.push_back({ Record.Node, Children });
		}
	} else {
		WalkChildrenCursor(
			Src.GetRoot(),
			[&](const TSNode Node, const std::span<const TSNode> Ancestors) -> bool {
				// 従来と同じ親と深さで修復を判定する事の返戻
				return CanDescendAfterRecording(Node, Ancestors.back(), Ancestors.size());
			}
		);
	}
	TextEdit::Apply(Src, Edits);
	if(!RangeRestorations.empty()) {
		// 左の式に在る範囲も含め，受理済の編集の位置と１度だけ対応付ける
		std::ranges::sort(RangeRestorations, std::less<uint32_t>(), &LexemeRestoration::Start);
		// Ruby のデコードデータは此処で初めて記録し，編集後の位置の順に追記する
		size_t Idx = 0;
		for(const AppliedEdit &Edit : Src.EditBatches.back()) {
			while(Idx < RangeRestorations.size() && RangeRestorations[Idx].Start < Edit.Start) ++Idx;
			if(Idx == RangeRestorations.size()) break;
			if(LexemeRestoration &Restoration = RangeRestorations[Idx]; Restoration.Start == Edit.Start && Restoration.End == Edit.End) {
				Src.LexemeRestorations.push_back(
					{ Edit.NewStart, Edit.NewEnd, std::move(Restoration.Original), Restoration.HasKindRestoration }
				);
				++Idx;
			}
		}
	}
	// 終了
	return;
}

/**
 * JS/TS のアロー関数を被演算子と読んだエラーの区切関数
 * アロー関数は括弧で包まない限り参照・添字・呼出の被演算子に為れず，`() => {}[0]` は言語処理系が拒む
 * パーサーは此れをエラーを立てずに１つの式と読む為，文の区切を補って読み直させる（整形仕様 4.2）
 * 計算量：ノード数 N に対し O(N)
 * @param Src ソースコード
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void ParseAdapter::RepairJavaScriptArrowChains(ParsedTree &Src) {
	// 区切の編集列
	std::vector<TextEdit> Edits;
	WalkAst(
		Src.GetRoot(),
		// ノード毎の被演算子の確認
		[&Src, &Edits](const TSNode Node) -> void {
			// 被演算子を先頭子に取らないノードを読み飛ばす場合の終了
			if(!Src.Is(Node, Core::Role::OperandHeadedAccess)) return;
			// 先頭子（被演算子）
			const TSNode Object = ts_node_child(Node, 0);
			// アロー関数でない被演算子を読み飛ばす場合の終了
			if(ts_node_is_null(Object) || !Src.Is(Object, Core::Role::ArrowFunctionNode)) return;
			// 被演算子の次のトークン
			const TSNode Next = ts_node_child(Node, 1);
			// 次のトークンの無い物を読み飛ばす場合の終了
			if(ts_node_is_null(Next)) return;
			const uint32_t From = Src.GetEnd(Object), To = Src.GetStart(Next);
			// 改行を挟まない物を読み飛ばす場合（自動セミコロン挿入が効かず，原文自体が言語処理系の拒む構文エラー）の終了
			if(std::string_view(Src.data() + From, To - From).find('\n') == std::string_view::npos) return;
			// アロー関数の後ろで文を終わらせる区切の追加
			TextEdit::Push(From, From, std::string(Src.GetRules().GetSpelling(Core::Word::StmtEnd)), Edits);
		}
	);
	TextEdit::Apply(Src, Edits);
	// 終了
	return;
}

/**
 * JS/TS の文境界と文脈依存の名前の読みの統一関数
 * 計算量：ノード数 N，直接子の最大数 C，祖先数 D，位置照会数 K，テキスト処理・編集の費用 Q に対し O(N (C + D) + K log(N + 1) + Q)
 * @param LocalSrc ソースコード
 * @return 改行が文法に違反する場合の見送理由，又は修復の完了
 * @throws FormatLimitExceeded 未反映の編集の解析が時間の上限を超えた場合
 * @throws std::length_error 未反映の編集のテキストがパーサーの位置の範囲を超えた場合
 * @throws std::bad_alloc 走査の道筋・領域又は編集を確保出来ない場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
FailureReason ParseAdapter::RepairJavaScriptJoints(ParsedTree &LocalSrc) {
	// 対象トークンの有無と編集状態の準備
	const std::string &Src = LocalSrc;
	const Core::SyntaxRules &Rules = LocalSrc.GetRules();
	const std::string_view Let = Rules.GetSpelling(Core::Word::ReassignableBinding);
	const std::string_view Await = Rules.GetSpelling(Core::Word::AwaitName);
	const std::string_view Export = Rules.GetSpelling(Core::Word::ExportName);
	const std::string_view Yield = Rules.GetSpelling(Core::Word::YieldName);
	const bool HasLet = Src.find(Let) != std::string::npos, HasAwait = Src.find(Await) != std::string::npos;
	const bool HasYield = Src.find(Yield) != std::string::npos, HasExport = Src.find(Export) != std::string::npos;
	const ReadingRules &Reading = LocalSrc.GetReading();
	// 確定した読取目標を使い，未確定の時だけモジュールのマーカーを求める
	std::optional<bool> IsModule;
	if(Reading.SrcGoal != Lang::SrcGoal::Undetermined) IsModule = Reading.SrcGoal == Lang::SrcGoal::Module;
	// 対象自身を含めず，最も内側の関数の非同期のマーカーの子へ受渡
	std::vector<std::optional<bool>> AwaitScopes = { std::nullopt };
	std::optional<bool> IsAwaitInFunction;
	std::vector<bool> GeneratorScopes { false };
	bool IsGenerator = false;
	std::vector<LexemeRestoration> KeywordNames;
	std::optional<RepairNodeRanges> Ranges;
	// モジュールの予約語とスクリプトの識別子を分ける読取目標の共有
	const auto IsModuleGoal = [&LocalSrc, &IsModule]() -> bool {
		if(!IsModule) {
			IsModule = HasChildOf(
				LocalSrc.GetRoot(),
				[&LocalSrc](const TSNode Child) -> bool {
					// モジュールのマーカーの文かの返戻
					return LocalSrc.Is(Child, Core::Role::ModuleStmt);
				}
			);
		}
		// 確定した読取目標の返戻
		return *IsModule;
	};
	// 未確定の最上位では，束縛の位置の名前が在る場合だけスクリプトと確定する
	std::optional<bool> HasAwaitBinding;
	const auto IsScriptAwaitName = [&]() -> bool {
		// 指定済の読取目標の返戻
		if(Reading.SrcGoal != Lang::SrcGoal::Undetermined) return Reading.SrcGoal == Lang::SrcGoal::Script;
		if(!HasAwaitBinding) {
			HasAwaitBinding = false;
			std::vector<bool> BindingScopes { false };
			WalkRecordedChildren(
				LocalSrc,
				LocalSrc.GetRoot(),
				[&](const TSNode Node, const std::span<const TSNode> Ancestors) -> bool {
					BindingScopes.resize(Ancestors.size());
					bool IsBinding = false;
					const TSNode Parent = Ancestors.back();
					const Core::BindingSyntax Shape = LocalSrc.GetProfile().GetKindOf(LocalSrc.GetSymbol(Parent)).Binding;
					// 初期化子・計算したプロパティ名を束縛の名前に含めない
					if(
						Shape == Core::BindingSyntax::ClassDecl || Shape == Core::BindingSyntax::Declarator ||
						Shape == Core::BindingSyntax::FunctionDecl || Shape == Core::BindingSyntax::NamedExpr
					) IsBinding = ts_node_eq(Node, LocalSrc.GetFieldChild(Parent, Core::ChildRole::Name));
					else if(Shape == Core::BindingSyntax::Params) IsBinding = true;
					else if(BindingScopes.back()) {
						if(Shape == Core::BindingSyntax::Pattern || Shape == Core::BindingSyntax::RestPattern) IsBinding = true;
						else if(Shape == Core::BindingSyntax::PairPattern) {
							IsBinding = ts_node_eq(Node, LocalSrc.GetFieldChild(Parent, Core::ChildRole::Val));
						} else if(Shape == Core::BindingSyntax::DefaultPattern) {
							IsBinding = ts_node_eq(Node, LocalSrc.GetFieldChild(Parent, Core::ChildRole::Left));
						} else if(Shape == Core::BindingSyntax::Param) {
							IsBinding = ts_node_eq(Node, LocalSrc.GetFieldChild(Parent, Core::ChildRole::Pattern)) ||
							ts_node_eq(Node, LocalSrc.GetFieldChild(Parent, Core::ChildRole::Name));
						}
					}
					BindingScopes.push_back(IsBinding);
					if(IsBinding && LocalSrc.GetView(Node) == Await) HasAwaitBinding = true;
					// 後続の宣言も１回の走査へ含める
					return true;
				}
			);
		}
		// 束縛の証拠が無い最上位の待機式は元の読取を保つ
		return *HasAwaitBinding;
	};
	// 待機の語を持つ演算子の判定
	const auto IsAwaitOperator = [&IsAwaitInFunction, &IsModuleGoal]() -> bool {
		// 最も内側の関数の非同期のマーカーの有無の返戻
		if(IsAwaitInFunction) return *IsAwaitInFunction;
		// 最上位の待機式を許す読取目標かの返戻
		return IsModuleGoal();
	};
	// JS の文境界と名前の修復の編集列
	std::vector<TextEdit> Edits;
	// 繋ぐ語と次のノードの間のコメント
	std::vector<TSNode> Comments;
	// 繋いだ行の行末へ置く行末コメント（本文・繋いだ次のノードの始まり・行コメントを含むか）と，繋いで改行の消える隙間
	struct JoinTail {
		std::string Text;
		uint32_t From;
		bool IsLineComment;
	};
	std::vector<JoinTail> Tails;
	std::vector<std::pair<uint32_t, uint32_t>> Gaps;
	// 語と次のノードの間を詰め，間のコメントを原文での種類の位置（独立は文の前の行・文中は繋いだ行内・行末は繋いだ行の行末）へ置く
	// パーサーはクラスのメンバをブロックコメントの中の改行でも分ける為，メンバの間 (`IsMemberGap`) の改行を含む文中コメントは隙間に置けず除く
	const auto Join = [&](const TSNode Head, const TSNode Word, const TSNode Next, const bool IsMemberGap) -> void {
		// 独立行・文中・行末のコメントの移動テキストの収集準備
		std::string Moved, InlineText = " ", Tail;
		bool IsTailLineComment = false;
		for(const TSNode Comment : Comments) {
			const std::string_view Text = LocalSrc.GetView(Comment);
			// 後に同じ行の次のノードが続く文中コメント・行にコメントだけが在る独立コメント・語の後で行が終わる行末コメント（行コメントの後へ積む改行を含む物は，行コメントの本文へ続けれず行末に置けない為に除く）
			if(const CommentKind Kind = LocalSrc.GetKindOf(Comment); Kind == CommentKind::Inline) {
				if(!IsMemberGap || Text.find('\n') == std::string_view::npos) InlineText.append(Text).push_back(' ');
			} else if(Kind == CommentKind::OwnLine) Moved.append(Text).push_back('\n');
			else if(!IsTailLineComment || Text.find('\n') == std::string_view::npos) {
				Tail.append(" ").append(Text);
				IsTailLineComment = IsTailLineComment || Text.starts_with(Core::CommentRepository::GetOpenerOf(CommentNotation::SlashLine));
			}
		}
		// 独立行コメントを見出前へ移動（見出の前に同じ行のコードが在れば改行を補い，其の行の行末コメントにしない）
		if(!Moved.empty()) {
			// 見出の前に同じ行で続くコメントの始まり（間へ入れると其のコメントが行末・独立へ変わる）
			uint32_t At = LocalSrc.GetStart(Head);
			for(
				uint32_t Before = TextEdit::SkipSpaceLeft(Src, Reading, At);
				Before > 1 && Src[Before - 1] == '/' && Src[Before - 2] == '*';
			) {
				// 位置で終わるコメントのノード
				const TSNode Prev = RepairNodeAt(LocalSrc, Before - 1, Ranges);
				if(ts_node_is_null(Prev) || !LocalSrc.Is(Prev, Core::Role::Comment) || LocalSrc.GetEnd(Prev) != Before) break;
				At = LocalSrc.GetStart(Prev);
				Before = TextEdit::SkipSpaceLeft(Src, Reading, At);
			}
			if(TextEdit::IsAfterCode(Src, Reading, At)) Moved.insert(0, 1, '\n');
			TextEdit::Push(At, At, std::move(Moved), Edits);
		}
		// 語と次ノードの間を行内本文へ置換
		TextEdit::Push(LocalSrc.GetEnd(Word), LocalSrc.GetStart(Next), std::move(InlineText), Edits);
		Gaps.emplace_back(LocalSrc.GetEnd(Word), LocalSrc.GetStart(Next));
		// 行末コメントは全ての繋ぎを集めた後に置く
		if(!Tail.empty()) Tails.push_back({ std::move(Tail), LocalSrc.GetStart(Next), IsTailLineComment });
	};
	// 登録された改行をコメントの内外で同じ様に読む
	const auto HasGapLineBreak = [&](const uint32_t Start, const uint32_t End) -> bool {
		const std::string_view Gap = std::string_view(Src).substr(Start, End - Start);
		LocalSrc.BoundaryRepairWork.Charge(Gap.size());
		// 間隙に改行が在るかの返戻
		return std::ranges::any_of(
			Reading.CommentLineBreaks,
			[Gap](const std::string_view Break) -> bool {
				// 登録した行の区切の有無の返戻
				return Gap.find(Break) != std::string_view::npos;
			}
		);
	};
	// JS ノード候補の処理
	const auto Collect = [&](const TSNode Node) -> void {
		// 前候補のコメントの初期化
		Comments.clear();
		// クラス本体の場合
		if(LocalSrc.Is(Node, Core::Role::ClassMembers)) {
			ForEachNamedChild(
				Node,
				// クラスメンバ候補の処理
				[&](const TSNode Member) -> void {
					// 注記等の余分な子の場合の終了
					if(ts_node_is_extra(Member)) return;
					// 改行前の修飾語を同名のフィールドへ分け，後続のメンバへ掛けない
					const uint32_t Count = ts_node_child_count(Member);
					size_t NameWork = 0;
					const TSNode Name = LocalSrc.GetFieldChild(Member, Core::ChildRole::Name, &NameWork);
					LocalSrc.BoundaryRepairWork.Charge(NameWork);
					// メンバ末尾より前の子を走査
					for(uint32_t Idx = 0; Idx + 1 < Count; ++Idx) {
						LocalSrc.BoundaryRepairWork.Charge(1);
						const TSNode Modifier = ts_node_child(Member, Idx);
						const std::string_view Word = LocalSrc.GetView(Modifier);
						// 名前とコメントを除き，修飾語の後だけを辿る
						if(ts_node_eq(Modifier, Name) || std::ranges::find(Reading.SeparatingModifiers, Word) == Reading.SeparatingModifiers.end()) {
							continue;
						}
						uint32_t Following = Idx + 1;
						while(Following + 1 < Count && ts_node_is_extra(ts_node_child(Member, Following))) {
							LocalSrc.BoundaryRepairWork.Charge(1);
							++Following;
						}
						if(const uint32_t Gap = LocalSrc.GetEnd(Modifier); HasGapLineBreak(Gap, LocalSrc.GetStart(ts_node_child(Member, Following)))) {
							TextEdit::Push(Gap, Gap, std::string(Rules.GetSpelling(Core::Word::StmtEnd)), Edits);
						}
					}
					// 後続のメンバへ掛かる修飾語だけのフィールド (`static\nx = 1`) でない場合（此の後で後続へ接続する）の終了
					if(std::ranges::find(Reading.JoiningModifiers, LocalSrc.GetView(Member)) == Reading.JoiningModifiers.end()) return;
					TSNode Next = ts_node_next_sibling(Member);
					Comments.clear();
					// 間のコメントを回収しつつ次ノードへ移動
					for(; !ts_node_is_null(Next) && ts_node_is_extra(Next); Next = ts_node_next_sibling(Next)) Comments.push_back(Next);
					// 次の名前付ノードへ修飾子を接続
					if(!ts_node_is_null(Next) && ts_node_is_named(Next)) Join(Member, Member, Next, true);
				}
			);
			// クラスの本体の子の走査の終了
			return;
		}
		// 式文でない場合の終了
		if(!LocalSrc.Is(Node, Core::Role::ExprStmt)) return;
		// 対象ノード先頭のキーワード
		const TSNode Word = ts_node_child(Node, 0);
		// 識別子でない語の場合の終了
		if(!LocalSrc.Is(Word, Core::Role::Identifier)) return;
		const bool IsLet = HasLet && LocalSrc.GetView(Word) == Let, IsExport = HasExport && LocalSrc.GetView(Word) == Export;
		// `let`・`export` と，演算子と読む所の `await`（パーサーは後の改行で文を終えると読むが，JS / TS は改行を越えて後の束縛・宣言・被演算子へ掛ける）以外の場合の終了
		if(!IsLet && !IsExport && !(HasAwait && LocalSrc.GetView(Word) == Await && IsAwaitOperator())) return;
		bool IsClosed = false;
		// キーワード後の兄弟走査
		for(TSNode Child = ts_node_next_sibling(Word); !ts_node_is_null(Child); Child = ts_node_next_sibling(Child)) {
			// 間のコメントを回収
			if(ts_node_is_extra(Child)) Comments.push_back(Child);
			// コードトークン通過を記録
			else IsClosed = true;
		}
		// ノード後の兄弟候補
		TSNode Next = ts_node_next_sibling(Node);
		for(; !ts_node_is_null(Next) && ts_node_is_extra(Next); Next = ts_node_next_sibling(Next)) Comments.push_back(Next);
		// 閉じた文と後続の無い文を結合しない場合の終了
		if(IsClosed || ts_node_is_null(Next)) return;
		if(IsExport) {
			// 予約語の `export` は改行を越えて後の宣言・指定子を公開する
			Join(Node, Word, Next, false);
			// export と後続の文の接続の終了
			return;
		}
		// `await` は後の式の文へ接続
		if(!IsLet) {
			if(LocalSrc.Is(Next, Core::Role::ExprStmt)) Join(Node, Word, Next, false);
			// await と後続式の接続の終了
			return;
		}
		TSNode Lead = Next;
		// 後続ノードの最左葉へ降下
		while(ts_node_child_count(Lead)) Lead = ts_node_child(Lead, 0);
		// 識別子（TS の型注釈の付く名前はラベルと誤読する）又はオブジェクトリテラル開始の場合
		if(IsNamedNode(Lead) ? LocalSrc.Is(Lead, Core::Role::SplitBindingHead) : LocalSrc.Is(Lead, Core::Role::OpenBrace)) {
			Join(Node, Word, Next, false); // キーワードと後続ノードを接続
		}
	};
	// 語の表と文法の役割で，改行が区切る文の境界を同じ葉の走査に記録する
	TSNode Restricted {};
	bool IsRestrictedAsync = false, IsRestrictedThrow = false, IsRestrictedYield = false;
	FailureReason BoundaryFailure;
	WalkRecordedChildren(
		LocalSrc,
		LocalSrc.GetRoot(),
		[&](const TSNode Node, const std::span<const TSNode> Ancestors) -> bool {
			if(HasAwait || HasYield) {
				AwaitScopes.resize(Ancestors.size());
				IsAwaitInFunction = AwaitScopes.back();
				AwaitScopes.push_back(
					LocalSrc.Is(Node, Core::Role::FunctionScope) ?
					HasChildOf(
						Node,
						[&](const TSNode Child) -> bool {
							LocalSrc.BoundaryRepairWork.Charge(1);
							// 同名のフィールドや通常の名前は非同期の関数を作らない事の返戻
							if(ts_node_is_named(Child) || !LocalSrc.GetProfile().IsSpelling(LocalSrc.GetView(Child), Core::Word::AsyncModifier)) {
								// 非同期の予約語でない場合の偽の返戻
								return false;
							}
							TSNode Next = ts_node_next_sibling(Child);
							while(!ts_node_is_null(Next) && ts_node_is_extra(Next)) {
								LocalSrc.BoundaryRepairWork.Charge(1);
								Next = ts_node_next_sibling(Next);
							}
							// 改行で分離するフィールドを，内側の await の読みに使わない事の返戻
							return !ts_node_is_null(Next) && !HasGapLineBreak(LocalSrc.GetEnd(Child), LocalSrc.GetStart(Next));
						}
					) :
					IsAwaitInFunction
				);
				GeneratorScopes.resize(Ancestors.size());
				IsGenerator = GeneratorScopes.back();
				GeneratorScopes.push_back(
					LocalSrc.Is(Node, Core::Role::FunctionScope) ?
					HasChildOf(
						Node,
						[&LocalSrc](const TSNode Child) -> bool {
							// 実体を持つ無名のジェネレータの記号かの返戻
							return !ts_node_is_named(Child) && LocalSrc.GetStart(Child) < LocalSrc.GetEnd(Child) &&
							LocalSrc.Is(Child, Core::Role::AsteriskToken);
						}
					) :
					IsGenerator
				);
			}
			// 語の役割は最内の関数で決まり，外側の非同期・ジェネレータの状態を引き継がない
			if(
				!ts_node_child_count(Node) && (
					LocalSrc.Is(Node, Core::Role::Identifier) || LocalSrc.Is(Node, Core::Role::AwaitVal) || LocalSrc.Is(Node, Core::Role::YieldVal)
				)
			) {
				const std::string_view Text = LocalSrc.GetView(Node);
				if(
					(
						Text == Await && !IsAwaitOperator() && (IsAwaitInFunction.has_value() || IsScriptAwaitName()) || Text == Yield && !IsGenerator
					) && !IsModuleGoal()
				) {
					// 位置で対応付ける識別子のエスケープは，他の名前を置換せず元の綴りへ戻す
					const uint32_t Start = LocalSrc.GetStart(Node), End = LocalSrc.GetEnd(Node);
					TextEdit::Push(
						Start,
						End,
						std::string(Rules.GetSpelling(Text == Await ? Core::Word::AwaitNameEscape : Core::Word::YieldNameEscape)),
						Edits
					);
					KeywordNames.push_back({ Start, End, std::string(Text) });
				}
			}
			// コメントの本文をトークンとして読まない事の返戻
			if(LocalSrc.Is(Node, Core::Role::Comment)) return false;
			if(!ts_node_child_count(Node) && LocalSrc.GetLen(Node)) {
				LocalSrc.BoundaryRepairWork.Charge(1);
				const uint32_t Start = LocalSrc.GetStart(Node);
				if(!ts_node_is_null(Restricted)) {
					const uint32_t End = LocalSrc.GetEnd(Restricted);
					const bool HasBreak = HasGapLineBreak(End, Start);
					if(HasBreak) {
						if(IsRestrictedAsync || IsRestrictedThrow || IsRestrictedYield && LocalSrc.Is(Node, Core::Role::AsteriskToken)) {
							BoundaryFailure = FailureReason::ParserUnreadable;
						} else if(!LocalSrc.Is(Node, Core::Role::StmtTerminatorToken)) {
							TextEdit::Push(End, End, std::string(Rules.GetSpelling(Core::Word::StmtEnd)), Edits);
						}
					}
				}
				Restricted = {};
				const TSNode Parent = LocalSrc.GetParentOf(Node);
				const std::string_view Text = LocalSrc.GetView(Node);
				const bool IsName = !KeywordNames.empty() && KeywordNames.back().Start == Start;
				if(
					!ts_node_is_named(Node) && !IsName && (
						LocalSrc.Is(Node, Core::Role::RestrictedLineWord) && (
							!LocalSrc.GetProfile().IsSpelling(Text, Core::Word::AsyncModifier) || LocalSrc.Is(Parent, Core::Role::ArrowFunctionNode) ||
							LocalSrc.Is(Parent, Core::Role::MethodDef) && !LocalSrc.Is(LocalSrc.GetParentOf(Parent), Core::Role::ClassMembers)
						) || LocalSrc.Is(Parent, Core::Role::AccessOrderedClass) &&
						std::ranges::find(Reading.SeparatingModifiers, Text) != Reading.SeparatingModifiers.end()
					)
				) {
					Restricted = Node;
					IsRestrictedAsync = LocalSrc.GetProfile().IsSpelling(Text, Core::Word::AsyncModifier);
					IsRestrictedThrow = LocalSrc.Is(Parent, Core::Role::ThrowStmt) || LocalSrc.Is(Parent, Core::Role::ThrowVal);
					IsRestrictedYield = LocalSrc.GetProfile().IsSpelling(Text, Core::Word::YieldName);
				}
			}
			Collect(Node);
			// 全ての子へ関数の事実を渡す事の返戻
			return true;
		}
	);
	// 文法で禁じた改行は，修復候補を適用せず原文の見送へ渡す
	if(BoundaryFailure.Val != FailureReason::None) return BoundaryFailure;
	// 繋いだ次のノードの行の最後のコードの後（行末の後続のコメントの前）への行末コメントの配置（隙間は二分探索の為に始まりの順へ並べる）
	std::sort(Gaps.begin(), Gaps.end());
	std::vector<TSNode> Following;
	for(JoinTail &Pending : Tails) {
		const uint32_t At = LocalSrc.FindLineEndAfter(Pending.From, Gaps, Following);
		// 行コメントを積むと其の行に続く改行を含むコメントが行コメントの本文に為る為，其の時は移す方の行末コメントを除く（其の行に元から在るコメントは動かさない）
		if(
			!Pending.IsLineComment || std::none_of(
				Following.begin(),
				Following.end(),
				[&LocalSrc](const TSNode Comment) -> bool {
					// 改行を含むコメントかの返戻
					return LocalSrc.GetView(Comment).find('\n') != std::string_view::npos;
				}
			)
		) TextEdit::Push(At, At, std::move(Pending.Text), Edits);
	}
	// 収集した文境界と名前の編集の適用
	TextEdit::Apply(LocalSrc, Edits, true);
	if(!KeywordNames.empty()) {
		// 編集後の位置を１回のマージで対応付け，既存のデコードデータへ併合する
		size_t Next = 0;
		std::vector<LexemeRestoration> Restored;
		for(const AppliedEdit &Edit : LocalSrc.EditBatches.back()) {
			while(Next < KeywordNames.size() && KeywordNames[Next].Start < Edit.Start) ++Next;
			if(Next == KeywordNames.size()) break;
			if(Edit.Start == KeywordNames[Next].Start && Edit.End == KeywordNames[Next].End) {
				Restored.push_back({ Edit.NewStart, Edit.NewEnd, std::move(KeywordNames[Next++].Original), true, true });
			}
		}
		const size_t Existing = LocalSrc.LexemeRestorations.size();
		LocalSrc.LexemeRestorations.insert(
			LocalSrc.LexemeRestorations.end(),
			std::make_move_iterator(Restored.begin()),
			std::make_move_iterator(Restored.end())
		);
		std::inplace_merge(
			LocalSrc.LexemeRestorations.begin(),
			LocalSrc.LexemeRestorations.begin() + Existing,
			LocalSrc.LexemeRestorations.end(),
			[](const LexemeRestoration &Left, const LexemeRestoration &Right) -> bool {
				return Left.Start < Right.Start;
			}
		);
	}
	// 終了
	return {};
}

/**
 * 改行を含むブロックコメントの行の区切の修復関数（整形仕様 4.2）
 * JS, TS, Swift の処理系は改行を含むブロックコメントを行の区切と読むが，パーサーは読まない為，同じ行のコードが続くコメントの後へ改行を補う
 * JS / TS は文の並びの直下と，改行で読みの変わるトークンの間（`return` の後，後置の `++` の前等）だけに補い，他の位置の読みを変えない
 * 補った改行は解析用の文の区切として控え，コメントの種類と原文の間隙から除く
 * 計算量：未収集ならノード数 N，収集済ならコメント数 C，修復数 E と編集・解析の費用 Q に対し O(N + E log(E + 1) + Q) 又は O(C + E log(E + 1) + Q)
 * @param Src 解析済のソース
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void ParseAdapter::RepairCommentLineBreaks(ParsedTree &Src) {
	// ブロックコメントの開始記号が無ければ，本文を調べるコメントも無い
	if(Src.find(Core::CommentRepository::GetOpenerOf(CommentNotation::Block)) == std::string::npos) return;
	// 未反映編集を先に解析し，此の世代のコメント列だけを共有する
	const TSNode Root = Src.GetRoot();
	// 一括適用する編集列
	std::vector<TextEdit> Edits;
	const auto Visit = [&Src, &Edits](const TSNode Node, const TSNode Parent) -> bool {
		// コメントでないノードの子を辿る事の返戻
		if(!Src.Is(Node, Core::Role::Comment)) return true;
		const std::string_view Text = Src.GetView(Node);
		if(
			const bool HasLineBreak = Text.starts_with(Core::CommentRepository::GetOpenerOf(CommentNotation::Block)) && std::ranges::any_of(
				Src.GetReading().CommentLineBreaks,
				[&Text](const std::string_view Break) -> bool {
					// 行の区切のテキストを含むかの返戻
					return Text.find(Break) != std::string_view::npos;
				}
			); !HasLineBreak
		// 行区切を含むブロックコメントでない事の返戻
		) return false;
		const ReadingRules &Reading = Src.GetReading();
		const uint32_t After = TextEdit::SkipSpaceRight(Src, Reading, Src.GetEnd(Node));
		// 同じ行にコードが続き，改行で読みが変わる改行を含むブロックコメントの後への改行の追加
		if(
			const bool IsBreaking = !Reading.HasRestrictedLineWords || Src.Is(Parent, Core::Role::StmtOrMemberSeq) ||
			Src.CanChangeReadingAt(TextEdit::SkipSpaceLeft(Src, Reading, Src.GetStart(Node)), After);
			IsBreaking && After < Src.size() && Src[After] != '\n'
		) TextEdit::Push(Src.GetEnd(Node), Src.GetEnd(Node), "\n", Edits);
		// コメントの中へ降りない事の返戻
		return false;
	};
	// 検証の空行照合等で既に集めたコメントでは，木全体の再走査を行わない
	if(Src.Syntax.HasCommentNodes) {
		for(const TSNode Node : Src.Syntax.Comments->Nodes) if(!ts_node_eq(Node, Root)) Visit(Node, Src.GetParentOf(Node));
	} else WalkChildrenCursor(Root, Visit);
	// 補った改行の一括適用
	if(!Edits.empty()) TextEdit::Apply(Src, Edits, true);
	// 終了
	return;
}

/**
 * Kotlin の自動の `;` の修復関数
 * 計算量：ノード数 V，調べる隙間の総長 L，挿入数 E，編集・再解析の費用 T に対し O(V + L + E log(E + 1) + T)
 * @param LocalSrc ソースコード
 * @throws FormatLimitExceeded 修復後の解析が時間の上限を超えた場合
 * @throws std::length_error 修復後のテキストがパーサーの位置の範囲を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 */
void ParseAdapter::RepairKotlinSemicolons(ParsedTree &LocalSrc) {
	// Kotlin 原文への参照
	const std::string &Src = LocalSrc;
	const std::string_view BlockOpen = Core::CommentRepository::GetOpenerOf(CommentNotation::Block);
	const bool HasBlockComment = Src.find(BlockOpen) != std::string::npos, HasError = ts_node_has_error(LocalSrc.GetRoot());
	// ブロックコメントも解析のエラーも無いソースの終了
	if(!HasBlockComment && !HasError) return;
	std::vector<uint32_t> Inserts;
	uint32_t PrevEnd = static_cast<uint32_t>(Src.size()), CodeEnd = PrevEnd;
	// 前のコードの後に，スキャナーが呑んでノードに為らなかったブロックコメントが在ったか（其の後の実のコメントのノードを越えて持ち越す）
	bool IsSwallowed = false;
	// 前のコードの後で開いて居る括弧（波括弧なら真，改行が文を終えない丸括弧・角括弧なら偽）
	std::vector<bool> OpenBrackets;
	WalkRecordedChildren(
		LocalSrc,
		LocalSrc.GetRoot(),
		// Kotlin ノードの処理
		[&](const TSNode Node, const TSNode Parent) -> bool {
			// 文字列は閉じの引用符がトークンに為らず，中のトークンの終わりが文字列の中に来る為，全体を１つのトークンとしての取扱
			if(!LocalSrc.Is(Node, Core::Role::StrLiteralNode) && ts_node_child_count(Node)) {
				// エラーの在る木では，健全なメンバの直後に同じ行で閉じの波括弧が続く位置（パーサーがメンバの終端を読めない形）へ区切を補う
				// 先のエラーを直すと後のクラスの本体が新たにエラーに為る連鎖も，エラーに包まれたメンバを含めて１度に集める
				if(HasError) {
					// 健全なメンバか，健全なメンバだけを包むエラー領域か
					const auto IsHealthyMember = [&LocalSrc](const TSNode Member) -> bool {
						// エラーを含まないメンバか，名前付の子が全て其の様なメンバのエラー領域かの返戻
						return LocalSrc.Is(Member, Core::Role::ClassMember) ?
						!ts_node_has_error(Member) :
						LocalSrc.Is(Member, Core::Role::Error) && ts_node_named_child_count(Member) && !HasChildOf(
							Member,
							[&LocalSrc](const TSNode Part) -> bool {
								// 健全なメンバでない名前付の子かの返戻
								return ts_node_is_named(Part) && (!LocalSrc.Is(Part, Core::Role::ClassMember) || ts_node_has_error(Part));
							}
						);
					};
					TSNode Prev {};
					ForEachChild(
						Node,
						[&](const TSNode Child) -> void {
							if(
								!ts_node_is_null(Prev) && IsHealthyMember(Prev) && LocalSrc.IsTokenSpelling(Child, Core::Role::ClosingBraceToken) &&
								std::string_view(Src).substr(LocalSrc.GetEnd(Prev), LocalSrc.GetStart(Child) - LocalSrc.GetEnd(Prev)).find_first_of(";\n") ==
								std::string_view::npos
							) Inserts.push_back(LocalSrc.GetEnd(Prev));
							if(!LocalSrc.Is(Child, Core::Role::Comment)) Prev = Child;
							// 終了
							return;
						}
					);
				}
				// トークンでないノードは子へ降りる事の返戻
				return true;
			}
			// 隙間の後のコメントノードの確認（幅０のノードでは本文が隙間に残る）
			const uint32_t Start = ts_node_start_byte(Node);
			IsSwallowed = IsSwallowed || HasBlockComment && PrevEnd < Start &&
			std::string_view(Src).substr(PrevEnd, Start - PrevEnd).find(BlockOpen) != std::string_view::npos;
			PrevEnd = ts_node_end_byte(Node);
			// コードノードなら，改行が文を終える位置で呑まれたコメントの後が式を続けるトークン（`when` の枝を始める `else` を除く）でなければ其の前のコード終端の挿入候補化と，コード終端・開いた括弧の更新
			if(!LocalSrc.Is(Node, Core::Role::Comment)) {
				if(
					IsSwallowed && (OpenBrackets.empty() || OpenBrackets.back()) && (
						std::ranges::find(LocalSrc.GetReading().LineContinuingTokens, LocalSrc.GetView(Node)) ==
						LocalSrc.GetReading().LineContinuingTokens.end() ||
						LocalSrc.Is(Node, Core::Role::ElseKeyword) && (IsNamedNode(Parent) && LocalSrc.Is(Parent, Core::Role::ValArm))
					)
				) Inserts.push_back(CodeEnd);
				IsSwallowed = false;
				CodeEnd = PrevEnd;
				if(LocalSrc.Is(Node, Core::Role::OpeningParenToken) || LocalSrc.Is(Node, Core::Role::OpeningBracketToken)) {
					OpenBrackets.push_back(false);
				} else if(LocalSrc.Is(Node, Core::Role::OpeningBraceToken)) OpenBrackets.push_back(true);
				else if(
					!OpenBrackets.empty() && (
						LocalSrc.Is(Node, Core::Role::ClosingParenToken) || LocalSrc.Is(Node, Core::Role::ClosingSquareToken) ||
						LocalSrc.Is(Node, Core::Role::ClosingBraceToken)
					)
				) OpenBrackets.pop_back();
			}
			// トークンの子へ降りない事の返戻
			return false;
		}
	);
	// 挿入が無い場合の終了
	if(Inserts.empty()) return;
	std::sort(Inserts.begin(), Inserts.end());
	// 重複した挿入位置を除去
	Inserts.erase(std::unique(Inserts.begin(), Inserts.end()), Inserts.end());
	// セミコロン挿入編集列
	std::vector<TextEdit> Edits;
	// 挿入位置数の領域を予約
	Edits.reserve(Inserts.size());
	// 各候補位置へセミコロンを挿入
	for(const uint32_t Pos : Inserts) {
		Edits.push_back({ Pos, Pos, std::string(LocalSrc.GetRules().GetSpelling(Core::Word::StmtEnd)) });
	}
	TextEdit::Apply(LocalSrc, Edits, true);
	// 終了
	return;
}

/**
 * Kotlin のオブジェクト本体をラムダと読んだ箇所の編集の収集関数
 * 計算量：ノードの直接子の照会費用 C と候補数 K に対し O(C + K + 1)
 * @param Src 同じ世代の解析済ソース
 * @param Node 本体の候補を持つノード
 * @param Edits 閉じの前へ挿す改行の格納先
 */
void ParseAdapter::CollectKotlinObjectEdit(const ParsedTree &Src, const TSNode Node, std::vector<TextEdit> &Edits) {
	// 解析用の閉括弧の分離
	const auto SeparateClose = [&Src, &Edits](const TSNode Body) -> void {
		// 子の無い本体の場合の終了
		if(!ts_node_child_count(Body)) return;
		if(
			const TSNode Close = ts_node_child(Body, ts_node_child_count(Body) - 1);
			Src.IsTokenSpelling(Close, Core::Role::ClosingBraceToken) && Src.GetStart(Close) && Src[Src.GetStart(Close) - 1] != '\n'
		) Edits.push_back({ Src.GetStart(Close), Src.GetStart(Close), "\n" });
		// 終了
		return;
	};
	// 名前付のオブジェクト本体をラムダと読んだ箇所の検出
	if(Src.Is(Node, Core::Role::NamedInfix) && ts_node_named_child_count(Node) == 3) {
		if(
			const TSNode Object = ts_node_named_child(Node, 0), Name = ts_node_named_child(Node, 1), Body = ts_node_named_child(Node, 2);
			IsNamedNode(Object) && Src.Is(Object, Core::Role::ObjectLiteralNode) &&
			Src.GetView(Object) == Src.GetRules().GetSpelling(Core::Word::ObjectHead) && IsNamedNode(Name) &&
			Src.Is(Name, Core::Role::SimpleName) && IsNamedNode(Body) && Src.Is(Body, Core::Role::LambdaLiteral) &&
			ts_node_is_null(Src.GetFirstNamedChild(Body, Core::Role::LambdaParams))
		) SeparateClose(Body);
		// 名前付の中置の式の収集の終了
		return;
	}
	// ２つの子を持つ呼出候補でないノードの終了
	if(!Src.Is(Node, Core::Role::CallExpr) || ts_node_named_child_count(Node) != 2) return;
	const TSNode Object = ts_node_named_child(Node, 0), Suffix = ts_node_named_child(Node, 1);
	const TSNode Annotated = ts_node_named_child(Suffix, 0), Lambda = ts_node_named_child(Annotated, 0);
	// 対象のオブジェクトとラムダの呼出候補でないノードの終了
	if(
		!(IsNamedNode(Object) && Src.Is(Object, Core::Role::ObjectLiteralNode)) ||
		!ts_node_is_null(Src.GetFirstNamedChild(Object, Core::Role::ClassMembers)) || ts_node_named_child_count(Suffix) != 1 ||
		!(IsNamedNode(Annotated) && Src.Is(Annotated, Core::Role::AnnotatedLambda)) || ts_node_named_child_count(Annotated) != 1 ||
		!(IsNamedNode(Lambda) && Src.Is(Lambda, Core::Role::LambdaLiteral)) ||
		!ts_node_is_null(Src.GetFirstNamedChild(Lambda, Core::Role::LambdaParams))
	) return;
	// 本体の閉じの前の改行
	SeparateClose(Lambda);
	// 此のノードの収集の終了
	return;
}

/**
 * Kotlin の改行を跨ぐ継目の区切関数
 * tree-sitter-kotlin は改行の後の呼出・添字・参照を前の式へ繋ぐが，Kotlin は改行で文を終える為，継目の前へ `;` を補って読み直させる
 * 他の編集（括弧除去等）が誤って繋いだ木を元に判断すると，区切った後の巡で結果が変わり冪等性が崩れる為，コメントの分離と正規化より前に行う
 * 計算量：ノード数 V と先頭子列・直接子列・隙間の総走査量 S，編集適用の費用 Q に対し O(V + S + Q)
 * @param Src ソースコード
 * @return 継目の編集が無く，同じ走査で本体の修復も完了した場合 true
 * @throws FormatLimitExceeded 未反映の編集の解析が時間の上限を超えた場合
 * @throws std::length_error 未反映の編集のテキストがパーサーの位置の範囲を超えた場合
 * @throws std::bad_alloc 走査の道筋又は編集のメモリを確保出来ない場合
 */
bool ParseAdapter::RepairKotlinJoints(ParsedTree &Src) {
	const GrammarProfile &Profile = Src.GetProfile();
	const TSNode Root = Src.GetRoot();
	// 同じ開始位置の子は，同じ位置より前に在る括弧だけを祖先の事実へ含める
	struct LineCtx {
		bool CanSplitLine; // 此のノードの内側で文を分け得るか
		std::optional<uint32_t> ChildStart; // 直前の直接子の開始位置
		int DepthBeforeStart; // 同じ開始位置より前の括弧の深さ
		int Depth; // 既に読んだ直接子が作る括弧の深さ
		const Core::RoleSet *Roles; // 此のノードの文法の固定定義
	};
	std::vector<LineCtx> Contexts = { { true, std::nullopt, 0, 0, &Profile.GetKindOf(Src.GetSymbol(Root)).Roles } };
	std::vector<TextEdit> Edits, ObjectEdits;
	CollectKotlinObjectEdit(Src, Root, ObjectEdits);
	WalkRecordedChildren(
		Src,
		Root,
		[&](const TSNode Node, const std::span<const TSNode> Ancestors) -> bool {
			Contexts.resize(Ancestors.size());
			LineCtx &Above = Contexts.back();
			if(const uint32_t Start = Src.GetStart(Node); !Above.ChildStart || *Above.ChildStart != Start) {
				Above.ChildStart = Start;
				Above.DepthBeforeStart = Above.Depth;
			}
			const Core::RoleSet &Roles = Profile.GetKindOf(Src.GetSymbol(Node)).Roles;
			const auto HasRole = [&Roles](const Core::Role Wanted) -> bool {
				// 此のノードの固定定義の役割の返戻
				return Roles.test(static_cast<size_t>(Wanted));
			};
			// 文の並びの祖先に達すると，其れより外の括弧と条件は対象にしない
			const bool CanSplitLine = Above.Roles->test(static_cast<size_t>(Core::Role::LineTerminatedCtx)) ||
			Above.CanSplitLine && !Above.Roles->test(static_cast<size_t>(Core::Role::WhenCond)) && Above.DepthBeforeStart < 1;
			if(!ts_node_is_named(Node)) {
				if(HasRole(Core::Role::OpeningBracketToken) || HasRole(Core::Role::OpeningParenToken)) ++Above.Depth;
				else if(HasRole(Core::Role::ClosingParenToken) || HasRole(Core::Role::ClosingSquareToken)) --Above.Depth;
			}
			Contexts.push_back({ CanSplitLine, std::nullopt, 0, 0, &Roles });
			if(CanSplitLine && HasRole(Core::Role::LineJoiningSuffix)) CollectKotlinJointEdit(Src, Node, Edits);
			if(Edits.empty() && (HasRole(Core::Role::NamedInfix) || HasRole(Core::Role::CallExpr))) {
				CollectKotlinObjectEdit(Src, Node, ObjectEdits);
			}
			if(!Edits.empty()) ObjectEdits.clear();
			// 全ての子へ文境界の事実を渡す事の返戻
			return true;
		}
	);
	// 継目の編集が無ければ本体も同じ世代の木で確定し，別の全走査を行わない
	if(Edits.empty()) {
		TextEdit::Apply(Src, ObjectEdits);
		// 本体の修復も完了した事の返戻
		return true;
	}
	TextEdit::Apply(Src, Edits, true);
	// 編集後の木で本体を読み直す必要が在る事の返戻
	return false;
}

/**
 * Kotlin のオブジェクト宣言・オブジェクト式の本体をラムダと読んだエラーの読直関数
 * パーサーは閉じの前に改行の無いオブジェクトの本体を後置ラムダと読む為，閉じの前へ改行を補って読み直させる
 * 名前付の `object O { … }` と，呼べないオブジェクト式 `object : A { … }` の双方を対象とする
 * 計算量：本文のバイト数 B，ノード数 N，編集適用の費用 Q に対し O(B + N + Q)，作業列は対象の語の数 K に対し O(K)
 * @param Src ソースコード
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void ParseAdapter::RepairKotlinObjectBodies(ParsedTree &Src) {
	// 欠落トークンを含む木は元の全走査を保ち，正常な木は対象の語を含む経路だけ辿る
	const TSNode Root = Src.GetRoot();
	const std::string_view Text = Src.GetCodeText(), Head = Src.GetRules().GetSpelling(Core::Word::ObjectHead);
	const bool ShouldPrune = !ts_node_has_error(Root) && !Head.empty();
	std::vector<uint32_t> ObjectStarts;
	if(ShouldPrune) for(size_t Pos = Text.find(Head); Pos != std::string_view::npos; Pos = Text.find(Head, Pos + Head.size())) {
		ObjectStarts.push_back(static_cast<uint32_t>(Pos));
	}
	size_t NextObject = 0;
	// 本体の閉じの直前へ挿す改行の収集
	std::vector<TextEdit> Edits;
	const auto Visit = [&](const TSNode Node) -> bool {
		if(ShouldPrune) {
			const uint32_t Start = Src.GetStart(Node);
			while(NextObject < ObjectStarts.size() && ObjectStarts[NextObject] < Start) ++NextObject;
			// 対象の語を含まない部分木の子へ降りない事の返戻
			if(NextObject == ObjectStarts.size() || ObjectStarts[NextObject] >= Src.GetEnd(Node)) return false;
		}
		CollectKotlinObjectEdit(Src, Node, Edits);
		// 入子のオブジェクトも調べる事の返戻
		return true;
	};
	if(Visit(Root)) WalkRecordedChildren(Src, Root, Visit);
	TextEdit::Apply(Src, Edits);
	// 終了
	return;
}

/**
 * C# のキャストと誤読した二項式の読みの統一関数
 * 計算量：ノード数 N，束縛の収集・照会と修復適用の総費用 Q に対し O(N + Q)
 * @param LocalSrc ソースコード
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void ParseAdapter::RepairCSharpMisreadCasts(ParsedTree &LocalSrc) {
	// 生のテキストを候補にせず，構文木の候補だけを１度列挙する
	// キャストと誤読した式の括弧の除去
	std::vector<TextEdit> Edits;
	WalkAst(
		LocalSrc.GetRoot(),
		// C# のキャスト式候補の処理
		[&](const TSNode Node) -> void {
			LocalSrc.CastRepairWork.Charge(1);
			// キャスト式でない場合の終了
			if(!LocalSrc.Is(Node, Core::Role::CastExpr)) return;
			const TSNode Type = LocalSrc.GetFieldChild(Node, Core::ChildRole::Type);
			const TSNode Val = LocalSrc.GetFieldChild(Node, Core::ChildRole::Val);
			// 型又は前置単項式を欠く候補の場合の終了
			if(ts_node_is_null(Type) || ts_node_is_null(Val) || !LocalSrc.Is(Val, Core::Role::PrefixUnaryOperation)) return;
			const std::string_view Operator = LocalSrc.GetView(ts_node_child(Val, 0));
			// 二項式と読み替えられない演算子の場合の終了
			if(Operator.size() != 1 || std::string_view("-+*&^").find(Operator) == std::string_view::npos) return;
			// 配列の型に読まれた添字は，直接の階数指定子だけを照合し，式の子孫を辿り直さない
			TSNode Name = Type;
			bool HasIndex = false;
			while(LocalSrc.Is(Name, Core::Role::ArrayTypeSyntax)) {
				ForEachNamedChild(
					Name,
					[&](const TSNode Child) -> void {
						LocalSrc.CastRepairWork.Charge(1);
						if(LocalSrc.Is(Child, Core::Role::ArrayRank) && ts_node_named_child_count(Child)) HasIndex = true;
						// 終了
						return;
					}
				);
				Name = LocalSrc.GetFieldChild(Name, Core::ChildRole::Type);
			}
			// 真の型の形と限定名の場合の終了
			if(!LocalSrc.Is(Name, Core::Role::Identifier) || !ts_node_eq(Name, Type) && !HasIndex) return;
			// 最初の候補で同じ世代の束縛を１度だけ収集し，候補が無ければ表を作らない
			LocalSrc.CollectDeclaredNames();
			// 型又は未確定の名前は括弧を保持する
			if(!LocalSrc.IsDeclaredName(LocalSrc.GetView(Name), LocalSrc.GetStart(Name))) return;
			TSNode Close = ts_node_next_sibling(Type);
			// 名前付子を越えて閉括弧へ移動
			while(!ts_node_is_null(Close) && ts_node_is_named(Close)) Close = ts_node_next_sibling(Close);
			// 閉括弧が無い場合の終了
			if(ts_node_is_null(Close) || !LocalSrc.Is(Close, Core::Role::ClosingParenToken)) return;
			// キャスト式の開括弧
			const TSNode Open = ts_node_child(Node, 0);
			TextEdit::Push(LocalSrc.GetStart(Open), LocalSrc.GetEnd(Open), "", Edits);
			TextEdit::Push(LocalSrc.GetStart(Close), LocalSrc.GetEnd(Close), "", Edits);
		}
	);
	TextEdit::Apply(LocalSrc, Edits);
	// 終了
	return;
}

/**
 * CSS / SCSS のセレクタのコメントの後の結合子の修復関数（整形仕様 4.2）
 * 処理系はコメントの前後の空白を子孫結合子と読み（Sass はセレクタの中のコメント其の物も空白と読む），パーサーはコメントに密着した後のセレクタを前のセレクタへ繋げる為，コメントの後へ空白を補う
 * 計算量：ノード数 N，修復区間の登録・編集適用の費用 Q に対し O(N + Q)
 * @param Src 解析済のソース
 * @param IsCommentSpace セレクタのコメント其の物を空白として読むか
 * @throws FormatLimitExceeded 未反映の編集の解析が時間の上限を超えた場合
 * @throws std::length_error 未反映の編集のテキストがパーサーの位置の範囲を超えた場合
 * @throws std::bad_alloc 祖先の事実又は編集を確保出来ない場合
 */
void ParseAdapter::RepairCssSelectorComments(ParsedTree &Src, const bool IsCommentSpace) {
	std::vector<TextEdit> Edits;
	std::vector<bool> SelectorContexts = { false };
	WalkRecordedChildren(
		Src,
		Src.GetRoot(),
		[&](const TSNode Node, const std::span<const TSNode> Ancestors) -> bool {
			SelectorContexts.resize(Ancestors.size());
			const TSNode Parent = Ancestors.back();
			// 本体の祖先で打ち切り，其の内側で最初のセレクタを認める
			const bool IsInSelectors =
			!Src.Is(Parent, Core::Role::ScopeBody) && (Src.Is(Parent, Core::Role::SelectorList) || SelectorContexts.back());
			SelectorContexts.push_back(IsInSelectors);
			// コメントでないノードの子を辿る事の返戻
			if(!Src.Is(Node, Core::Role::Comment)) return true;
			const uint32_t Start = Src.GetStart(Node), End = Src.GetEnd(Node);
			// 空白と読む境界のコメントに密着した後のセレクタの前への空白の追加
			if(
				const bool IsSpaceBefore = Start && std::isspace(static_cast<unsigned char>(Src[Start - 1])),
				IsGluedAfter = End < Src.size() && !std::isspace(static_cast<unsigned char>(Src[End])) && Src[End] != ',' && Src[End] != '{';
				IsInSelectors && IsGluedAfter && (IsSpaceBefore || IsCommentSpace)
			) TextEdit::Push(End, End, " ", Edits);
			// コメントの中へ降りない事の返戻
			return false;
		}
	);
	// 補った空白の一括適用
	if(!Edits.empty()) TextEdit::Apply(Src, Edits);
	// 終了
	return;
}

/**
 * Swift のパーサーの読違の補正関数
 * 計算量：ソース長 N，ノード数 V，候補数 K，編集・差分解析・見出式補正の費用 Q に対し O(N + V log(K + 1) + K log(V + 1) + Q)
 * @param Src ソースコード
 * @return コメントの密着した演算子を直し切れなかった見送理由（直し切れれば理由無）
 * @throws std::bad_alloc 編集・解析又は照会のメモリを確保出来ない場合
 * @throws FormatLimitExceeded 構文の照会又は編集が作業予算を超える場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
FailureReason ParseAdapter::RepairSwiftMisreads(ParsedTree &Src) {
	// 演算子を成す字
	static constexpr std::string_view OperatorChars = "/=-+!*%<>&|^~?";
	// 演算子の終端の取得
	const auto OperatorEnd = [](const std::string_view Text, const uint32_t Start) -> uint32_t {
		// 行コメントの開きの記法の取得
		const std::string_view LineOpen = Core::CommentRepository::GetOpenerOf(CommentNotation::SlashLine);
		const std::string_view BlockOpen = Core::CommentRepository::GetOpenerOf(CommentNotation::Block);
		// 演算子終端の初期化と連続記号走査
		uint32_t End = Start + 1;
		while(
			End < Text.size() && (OperatorChars.find(Text[End]) != std::string_view::npos || Text[End] == '.' && Text[Start] == '.') &&
			Text.compare(End, LineOpen.size(), LineOpen) && Text.compare(End, BlockOpen.size(), BlockOpen)
		) ++End;
		// 演算子の字の並びの終わりの返戻
		return End;
	};
	std::vector<TextEdit> Inserted = CollectSwiftCommentEdits(Src, OperatorChars);
	if(!Inserted.empty()) {
		// 同じ位置の再探索と再試行をせず，纏めた編集の後を１度だけ解析する
		TextEdit::Apply(Src, Inserted);
		static constexpr size_t GluedCommentBudget = 1 << 20;
		ParserReadBudget Budget { GluedCommentBudget };
		SyntaxParser Parser;
		// 文法を設定出来ないパーサーの初期化失敗の返戻
		// 条件に該当する値の返戻
		if(!Parser.CanSetGrammar(Src.GetProfile().GetLang().GetTreeSitterLang())) return FailureReason::ParserInitFailed;
		bool IsExpired = false;
		TSTree *const Tree =
		Parser.Parse(Src.Syntax.IsTreeEdited ? Src.Syntax.Tree.get() : nullptr, Src.GetCodeText(), IsExpired, &Budget);
		Src.Syntax.IsTreeEdited = false;
		Src.Syntax.IsParseExpired = IsExpired;
		Src.ReplaceTree(Tree);
		// 実読取予算を使い切った修復の見送の返戻
		if(Budget.IsCancelled) return FailureReason::GluedCommentsLimit;
		// 解析１回の期限の超過の返戻
		if(IsExpired) return FailureReason::ParseTimeExceeded;
		// パーサーが構文木を作れない事の返戻
		if(!Tree) return FailureReason::ParserUnreadable;
	}
	// 見出式補正編集列の準備
	std::vector<TextEdit> Edits;
	// 見出式の括弧に依るラッパー
	const auto Wrap = [&Edits](const uint32_t Start, const uint32_t End) -> void {
		TextEdit::Push(Start, Start, "(", Edits);
		TextEdit::Push(End, End, ")", Edits);
	};
	// 不透明なトークンの外の位置の判定
	const auto IsExposed = [&Src](const TSNode Expr) -> bool {
		// 要素１つの括弧でなく，紛れる式を持つかの返戻
		return (!Src.Is(Expr, Core::Role::TupleExpr) || ts_node_named_child_count(Expr) != 1) && Src.HasBodyLikeExpr(Expr);
	};
	// 実トークンで閉じた本体の終端と，本文を読まないコメントの範囲
	std::vector<uint32_t> BodyEnds;
	std::vector<std::pair<uint32_t, uint32_t>> Comments;
	// ノード毎の補正（祖先は走査の道筋から受け，`ts_node_parent` で根から降り直さない）
	const auto Visit = [&](const TSNode Node, const std::span<const TSNode> Ancestors) -> void {
		const bool IsComment = Src.Is(Node, Core::Role::Comment);
		if(!IsComment) Src.BoundaryRepairWork.Charge(1);
		if(!ts_node_is_missing(Node) && Src.Is(Node, Core::Role::ClosingBraceToken) && Src.GetLen(Node)) {
			BodyEnds.push_back(Src.GetEnd(Node));
		}
		if(IsComment) Comments.emplace_back(Src.GetStart(Node), Src.GetEnd(Node));
		// 空白の無い中置の演算子を名前の一部と読んだエラー (`a*b`) は，名前の内側の演算子の字の並びの前後へ空白を置き，前後とも空白を持つ同じ中置にする
		if(Src.Is(Node, Core::Role::SimpleName)) {
			const uint32_t Start = Src.GetStart(Node), End = Src.GetEnd(Node);
			for(uint32_t At = Start + 1; At < End; ++At) {
				if(OperatorChars.find(Src[At]) == std::string_view::npos) continue;
				const uint32_t RunEnd = OperatorEnd(Src, At);
				// 名前の終わり迄続く後置の形の終了
				if(RunEnd >= End) return;
				TextEdit::Push(At, At, " ", Edits);
				TextEdit::Push(RunEnd, RunEnd, " ", Edits);
				At = RunEnd;
			}
			// 名前のノードの終了
			return;
		}
		// ノード型に依る演算子又は見出文の振分
		if(Src.Is(Node, Core::Role::UserDefinedOperator)) {
			// 演算子の開始位置取得
			const uint32_t Start = Src.GetStart(Node);
			// 密着した `!` / `?` の後置への分離（参照と２字以上の記号列は誤読を防いで保持）
			// 切り離さない演算子の終了
			// 中置の式の左の被演算子へ密着しない演算子の終了
			if(
				Src.GetLen(Node) < 2 || Src[Start] != '!' && Src[Start] != '?' || Src[Start + 1] == '!' || Src[Start + 1] == '?' || !Start ||
				std::isspace(static_cast<unsigned char>(Src[Start - 1])) || [&]() -> bool {
					// 現在のノードの親の取得
					const TSNode Parent = Ancestors.empty() ? TSNode{} : Ancestors.back();
					return ts_node_is_null(Parent) || !Src.Is(Parent, Core::Role::NamedInfix) || !ts_node_is_named(ts_node_child(Parent, 0)) ||
					Src.GetEnd(ts_node_child(Parent, 0)) != Start;
				}()
			) return;
			// 演算子前後の空白分離編集
			const uint32_t End = std::max(OperatorEnd(Src, Start), Src.GetEnd(Node));
			TextEdit::Push(Start + 1, Start + 1, " ", Edits);
			if(End > Start + 1 && End < Src.size() && !std::isspace(static_cast<unsigned char>(Src[End]))) {
				TextEdit::Push(End, End, " ", Edits);
			}
			// 利用者定義の演算子の終了
			return;
		}
		// 波括弧の見出文でない場合の終了
		if(!Src.Is(Node, Core::Role::BracedHeaderStmt)) return;
		// 見出文の誤読範囲検出状態の初期化
		uint32_t MisreadEnd = 0;
		if(!Src.Is(ts_node_child(Node, ts_node_child_count(Node) - 1), Core::Role::IfStmtNode)) {
			// 親の祖先列を辿る見出文範囲の拡張
			size_t ParentNext = Ancestors.size();
			TSNode Child = Node, Parent = ParentNext ? Ancestors[ParentNext - 1] : TSNode{};
			// ネストの if 末尾から外側の見出への遡及
			while(
				!ts_node_is_null(Parent) && Src.Is(Parent, Core::Role::IfStmtNode) &&
				ts_node_eq(ts_node_child(Parent, ts_node_child_count(Parent) - 1), Child)
			) {
				Child = Parent;
				Parent = --ParentNext ? Ancestors[ParentNext - 1] : TSNode{};
			}
			// 後続クロージャ又は呼出接尾辞に依る誤読終端の検出
			if(
				const TSNode Next = ts_node_is_null(Parent) ? TSNode{} : ts_node_next_named_sibling(Parent);
				!ts_node_is_null(Next) && Src.Is(Parent, Core::Role::Error) && Src.GetEnd(Child) == Src.GetEnd(Parent) &&
				Src.Is(Next, Core::Role::LambdaLiteral)
			) MisreadEnd = Src.GetEnd(Parent);
			else if(
				const TSNode Follower = ts_node_next_named_sibling(Child);
				!ts_node_is_null(Follower) && Src.Is(Follower, Core::Role::StrayBlock) && Src[Src.GetStart(Follower)] == '{' &&
				!std::memchr(Src.data() + Src.GetEnd(Child), '\n', Src.GetStart(Follower) - Src.GetEnd(Child))
			) MisreadEnd = Src.GetEnd(Child);
			// 呼出の先頭側に連なる祖先から後続クロージャを探索
			else {
				while(!ts_node_is_null(Parent) && ts_node_eq(ts_node_child(Parent, 0), Child)) {
					if(
						const TSNode Suffix = ts_node_child(Parent, ts_node_child_count(Parent) - 1);
						Src.Is(Parent, Core::Role::CallExpr) && Src.Is(Suffix, Core::Role::CallSuffix) && ts_node_named_child_count(Suffix) == 1 &&
						Src.Is(ts_node_named_child(Suffix, 0), Core::Role::LambdaLiteral)
					) MisreadEnd = Src.GetEnd(Child);
					Child = Parent;
					Parent = --ParentNext ? Ancestors[ParentNext - 1] : TSNode{};
				}
			}
		}
		// 見出部の式候補走査
		TSNode Prev = {}, Last = {};
		ForEachChild(
			Node,
			[&](const TSNode Child) -> bool {
				// 本体に達した所で走査を打ち切る事の返戻
				if(IsNamedNode(Child) ? Src.Is(Child, Core::Role::Else) : Src.Is(Child, Core::Role::OpeningBraceToken)) return false;
				// 見出語の後に続く式候補の抽出
				if(
					const TSNode Expr = Src.Is(Child, Core::Role::WhereClauseNode) ?
					ts_node_named_child(Child, 1) :
					ts_node_is_named(Child) && !ts_node_is_null(Prev) && !ts_node_is_named(Prev) && Src.Is(Prev, Core::Role::HeaderExprLead) ?
					Child :
					TSNode{};
					!ts_node_is_null(Expr)
				) {
					if(!ts_node_is_null(Last) && IsExposed(Last)) Wrap(Src.GetStart(Last), Src.GetEnd(Last));
					Last = Expr;
				}
				// 次回用の直前子更新
				Prev = Child;
				// 走査を続ける事の返戻
				return true;
			}
		);
		// 最終見出の誤読本体範囲を含む終端設定
		if(MisreadEnd && !ts_node_is_null(Last)) Wrap(Src.GetStart(Last), MisreadEnd);
		else if(!ts_node_is_null(Last) && IsExposed(Last)) Wrap(Src.GetStart(Last), Src.GetEnd(Last));
	};
	HasDescendantWithAncestorsOf(
		Src.GetRoot(),
		[&Visit](const TSNode Node, const std::span<const TSNode> Ancestors) -> bool {
			Visit(Node, Ancestors);
			// 全てのノードを訪ねる迄走査を続ける事の返戻
			return false;
		}
	);
	// 閉じた本体の後の隙間だけを原文順に読み，実在する文区切を本体へ密着させて読ませる
	size_t CommentIdx = 0;
	for(const uint32_t End : BodyEnds) {
		Src.BoundaryRepairWork.Charge(1);
		uint32_t Pos = End;
		while(Pos < Src.size()) {
			if(std::isspace(static_cast<unsigned char>(Src[Pos]))) ++Pos;
			else {
				while(CommentIdx < Comments.size() && Comments[CommentIdx].second <= Pos) ++CommentIdx;
				if(CommentIdx == Comments.size() || Comments[CommentIdx].first != Pos) break;
				Pos = Comments[CommentIdx++].second;
			}
		}
		if(Pos > End && Pos < Src.size() && Src[Pos] == ';') {
			Src.BoundaryRepairWork.Charge(4); // 編集２件と区切２バイト
			TextEdit::Push(End, End, ";", Edits);
			TextEdit::Push(Pos, Pos + 1, "\n", Edits);
		}
	}
	// 見出式と文境界の補正の一括適用
	TextEdit::Apply(Src, Edits);
	// 補正し切った事の返戻
	return {};
}

/**
 * ========== パーサーの読違に対する修復 ==========
 * 此処の関数は投影がコメントを分離する前に走る（整形仕様 3 の設計要件）
 * 其の時点でコメントは未だ原文の一部の為，コメントのテキストを読み書きして良い唯一の工程とする
 * 修復の範囲は言語処理系の読みをパーサーへ伝える事に限り，書式の好みでトークンを整えない（整形仕様 4.2）
 * JSX の本文の意味付関数（整形仕様 4.2，6.6）
 * 本文と文字参照の並びを，式・要素の境界毎に正規化し，式の子は編集せず，本文の値と区別を保つ
 * 計算量：ノード数 N，本文の長さ L，本文の正規化・再解析の費用 Q に対し O(N + L + Q)
 * @param Src 解析済のソース
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void ParseAdapter::NormalizeJsxText(ParsedTree &Src) {
	// JSX の本文を持たないソースの終了
	if(!Src.IsParsed() || !Src.GetReading().HasJsxText) return;
	// 表示本文を持つ解析木の関係の取得
	Src.RecordNodeRelations();
	// 表示本文の並びが無い木の場合の終了
	if(!Src.Syntax.Relations->HasJsxCandidates) return;
	std::vector<TextEdit> Edits;
	WalkRecordedChildren(
		Src,
		Src.GetRoot(),
		[&](const TSNode Node, const std::span<const TSNode>) -> bool {
			++Src.Syntax.Relations->JsxWork;
			// 配置保持対象の JSX 要素の選別
			// 行端の描画が処理系で異なる要素は配置を保持（ネストは個別判定）
			const bool IsRendered = Src.Is(Node, Core::Role::RenderedChildren), IsDisputed = IsRendered && Src.HasDisputedJsxText(Node);
			// 配置を保つ要素も，両処理系が除く改行の前の半角空白とタブは除き，出力の行末の空白の除去と読みを揃える（整形仕様 6.6）
			if(IsDisputed) {
				ForEachNamedChild(
					Node,
					[&Src, &Edits](const TSNode Child) -> void {
						// 本文でない子の場合の終了
						if(!Src.Is(Child, Core::Role::RenderedText)) return;
						const std::string_view Text = Src.GetView(Child);
						for(size_t Newline = Text.find('\n'); Newline != std::string_view::npos; Newline = Text.find('\n', Newline + 1)) {
							if(const size_t Blank = Text.find_last_not_of(" \t", Newline - 1) + 1; Newline && Blank < Newline) {
								TextEdit::Push(Src.GetStart(Child) + Blank, Src.GetStart(Child) + Newline, "", Edits);
							}
						}
					}
				);
			}
			if(IsRendered && !IsDisputed) {
				// 式・要素の境界で区切る本文の並びの範囲
				uint32_t Begin = 0, End = 0, TextBegin = 0, TextEnd = 0;
				std::string Normalized;
				// UTF-8 のコードポイントの読取
				const auto ReadPoint = [](const std::string_view Val, const size_t Pos) -> std::pair<char32_t, size_t> {
					// 位置のコードポイントと其の次の位置（既定は置換文字）
					size_t Next = Pos;
					char32_t Point = 0XFFFD;
					// デコード出来ないバイトは UTF-8 のデコード器が置換文字に換える為，空白に数えない
					if(!TextEdit::IsUtf8Decoded(Val, Next, Point)) Next = Pos + 1;
					// コードポイントとバイト幅の返戻
					return std::pair<char32_t, size_t>{ Point, Next - Pos };
				};
				// 式を挟まない本文１つの表示内容を追記（本文毎に行端の空白を落とす）
				const auto AppendText = [&]() -> void {
					// 本文が無く追記を省く場合の終了
					if(TextBegin == TextEnd) return;
					// 今回の本文範囲
					const std::string_view Text(Src.data() + TextBegin, TextEnd - TextBegin);
					// 表示行毎の有効本文範囲の初期化
					size_t First = 0, Last = 0;
					bool HasText = false, HasLine = false;
					// 改行境界毎の端空白除去と本文連結
					for(size_t Pos = 0; Pos < Text.size();) {
						const std::pair<char32_t, size_t> Symbol = ReadPoint(Text, Pos);
						const char32_t Point = Symbol.first;
						const size_t Width = Symbol.second;
						if(const bool IsLineBreak = Point == '\n' || Point == '\r' || Point == 0X2028 || Point == 0X2029; IsLineBreak) {
							// 内容を持つ行の正規化本文への確定
							if(First != std::string_view::npos && HasText) {
								if(HasLine) Normalized += ' ';
								Normalized.append(Text.substr(First, Last - First));
								HasLine = true;
							}
							First = std::string_view::npos;
							HasText = false;
						} else if(!Src.GetReading().IsJsxSpace(Point)) {
							// 非空白符号を含む本文範囲の更新
							if(First == std::string_view::npos) First = Pos;
							Last = Pos + Width;
							HasText = true;
						}
						Pos += Width;
					}
					// 改行で終わらない最終行の確定
					if(First != std::string_view::npos) {
						if(HasLine) Normalized += ' ';
						Normalized.append(Text.substr(First));
					}
					// 確定済本文範囲の初期化
					TextBegin = TextEnd = 0;
				};
				// 本文の値を保つ文字参照のデコードと編集の確定
				const auto FlushText = [&]() -> void {
					// 溜めた本文の追記
					AppendText();
					// 本文の並びが無く編集しない場合の終了
					if(Begin == End) return;
					if(!Normalized.empty()) {
						// 本文の端の文字参照を同じ値の空白へデコード
						if(const size_t Amp = Normalized.rfind('&'); Amp != std::string::npos) {
							if(
								const std::pair<size_t, unsigned char> Ref = ParsedTree::BlankRef(Normalized, Amp);
								Ref.first && Amp + Ref.first == Normalized.size()
							) Normalized.replace(Amp, Ref.first, std::string(1, Ref.second));
						}
						if(const std::pair<size_t, unsigned char> Ref = ParsedTree::BlankRef(Normalized, 0); Ref.first) {
							Normalized.replace(0, Ref.first, std::string(1, Ref.second));
						}
						// UTF-8 末尾符号の開始位置探索
						size_t Tail = Normalized.size() - 1;
						while(Tail && (static_cast<unsigned char>(Normalized[Tail]) & 0XC0) == 0X80) --Tail;
						// 先頭・末尾の空白符号の安定した表現への置換
						for(const size_t Pos : { Tail, size_t{ 0 } }) {
							const std::pair<char32_t, size_t> Symbol = ReadPoint(Normalized, Pos);
							const char32_t Point = Symbol.first;
							const size_t Width = Symbol.second;
							if(Point != ' ' && Point != '\t' && Src.GetReading().IsJsxSpace(Point)) {
								Normalized.replace(Pos, Width, "&#" + std::to_string(Point) + ";");
							}
						}
					}
					// 原文と異なる正規化範囲の編集登録
					if(Normalized != std::string_view(Src.data() + Begin, End - Begin)) TextEdit::Push(Begin, End, std::move(Normalized), Edits);
					// 次の本文並びに備えたバッファ初期化
					Normalized.clear();
					// 並び範囲の初期化
					Begin = End = 0;
				};
				// 隣接する本文と文字参照だけを同じ本文の並びとして扱う
				ForEachNamedChild(
					Node,
					[&](const TSNode Child) -> void {
						// 式・要素の境界に於ける本文の確定
						if(!Src.Is(Child, Core::Role::RenderedText)) {
							FlushText();
							// 本文以外の子の処理の終了
							return;
						}
						// 隣接本文の範囲の拡張
						if(Begin == End) Begin = Src.GetStart(Child);
						End = Src.GetEnd(Child);
						if(TextBegin == TextEnd) TextBegin = Begin;
						TextEnd = End;
						// 終了
						return;
					}
				);
				FlushText();
			}
			// コメント本文の内側には降りず，構文要素の本文だけを走査する事の返戻
			return !Src.Is(Node, Core::Role::Comment);
		}
	);
	// 全要素範囲の確定後に於ける後向編集の一括適用
	TextEdit::Apply(Src, Edits);
	// 終了
	return;
}

/**
 * 論理行を越えた前処理の見出の終端の取得関数
 * 準備時の論理トークン走査で確定した範囲を共有し，コメント・リテラル内の改行で終端を作らない
 * 条件分岐の本体は調べず，修復で位置が変わる前に見出のフィールドだけを照合する
 * 計算量：未収集ならノード数 N，収集済なら意味判定の候補数 C，見出数 P に対し O(1 + N log(P + 1)) 又は O(1 + C + P log(P + 1))
 * @param Src 解析済のソース
 * @return 見出が確定済の論理行を越えた場合の終端の位置，其の他は無
 * @throws std::bad_alloc ノードの走査のメモリを確保出来ない場合
 * @throws std::length_error 走査列の要素数が上限を超える場合
 * @throws std::out_of_range ノード又は範囲の識別子が表に無い場合
 */
std::optional<uint32_t> ParseAdapter::GetMisreadDirectiveEnd(const ParsedTree &Src) {
	// 準備時に前処理の指示が無いソースの返戻
	if(Src.DirectiveLines.empty()) return std::nullopt;
	std::optional<uint32_t> LostAt;
	const TSNode Root = Src.GetRoot();
	const auto Visit = [&](const TSNode Node) -> void {
		// 前処理の見出以外と，既に見付かった位置より後のノードの読飛
		if(!Src.Is(Node, Core::Role::PreprocNode) || LostAt && Src.GetStart(Node) >= *LostAt) return;
		const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Line =
		std::lower_bound(Src.DirectiveLines.begin(), Src.DirectiveLines.end(), std::pair<uint32_t, uint32_t>(Src.GetStart(Node), 0));
		// 準備時に前処理の指示と読んで居ないノードの終了
		if(Line == Src.DirectiveLines.end() || Line->first != Src.GetStart(Node)) return;
		for(
			const Core::ChildRole Field :
			{
				Core::ChildRole::ArgField,
				Core::ChildRole::Cond,
				Core::ChildRole::Name,
				Core::ChildRole::Params,
				Core::ChildRole::ParamsField,
				Core::ChildRole::Path,
				Core::ChildRole::Val
			}
		) {
			const TSNode Header = Src.GetFieldChild(Node, Field);
			if(!ts_node_is_null(Header) && Src.GetEnd(Header) > Line->second) {
				LostAt = Line->second;
				break;
			}
		}
		// 終了
		return;
	};
	// 同じ世代の共通走査が選んだ見出を使い，条件分岐の本体を辿り直さない
	if(Src.Syntax.HasParents) {
		for(const ParsedTree::SemanticNode &Candidate : Src.Syntax.Relations->SemanticNodes) if(Candidate.IsHeader) {
			Visit(Src.GetNodeAt(Candidate.Relation).Node);
		}
	} else WalkAst(Root, Visit);
	// 最初の読違の位置の返戻
	return LostAt;
}

/**
 * 順位の係数・符号・整数を算術式と読む誤りの修復関数
 * 特殊記法のトークンを通常の二項式へ分けない様，符号の周囲の間隙だけを解析入力で除く
 * 計算量：ノード数 N，対象の名前と引数の長さ B，編集適用の費用 Q に対し O(N + B + Q)
 * @param Src 解析済の入力
 * @throws FormatLimitExceeded 修復後の解析が上限を超える場合
 * @throws std::bad_alloc 走査又は編集のメモリを確保出来ない場合
 * @throws std::length_error 走査又は編集の要素数上限を超える場合
 */
void ParseAdapter::RepairOrdinalArguments(ParsedTree &Src) {
	// 文法が登録する特殊記法の探索
	const ReadingRules &Reading = Src.GetReading();
	// 特殊記法を持たない文法の終了
	if(Reading.OrdinalSelectorNames.empty()) return;
	std::vector<TextEdit> Edits;
	WalkAst(
		Src.GetRoot(),
		[&](const TSNode Node) -> void {
			// セレクタの名前と文法が定める引数の用途の照合
			// セレクタでないノードの終了
			if(!Src.Is(Node, Core::Role::SelectorHost)) return;
			const TSNode NameNode = Src.GetFirstNamedChild(Node, Core::Role::SelectorName);
			// 名前を持たないセレクタの終了
			if(ts_node_is_null(NameNode)) return;
			std::string Name(Src.GetView(NameNode));
			for(char &Byte : Name) Byte = static_cast<char>(std::tolower(static_cast<unsigned char>(Byte)));
			// 通常のセレクタの引数を変えない事の終了
			if(std::ranges::find(Reading.OrdinalSelectorNames, std::string_view(Name)) == Reading.OrdinalSelectorNames.end()) return;
			uint32_t Pos = TextEdit::SkipSpaceRight(Src, Reading, Src.GetEnd(NameNode));
			// 引数を持たない形の終了
			if(Pos >= Src.size() || Src[Pos] != '(') return;
			const size_t End = Src.find(')', ++Pos);
			// 閉境界を欠く形の終了
			if(End == std::string::npos) return;
			Pos = TextEdit::SkipSpaceRight(Src, Reading, Pos);
			if(Pos < End && (Src[Pos] == '+' || Src[Pos] == '-')) ++Pos;
			while(Pos < End && std::isdigit(static_cast<unsigned char>(Src[Pos]))) ++Pos;
			// 係数と順位の単位を持たない形の終了
			if(Pos >= End || Src[Pos] != 'n' && Src[Pos] != 'N') return;
			const uint32_t CoefficientEnd = ++Pos;
			Pos = TextEdit::SkipSpaceRight(Src, Reading, Pos);
			// 符号と整数を持たない形の終了
			if(Pos >= End || Src[Pos] != '+' && Src[Pos] != '-') return;
			const uint32_t Sign = Pos++;
			Pos = TextEdit::SkipSpaceRight(Src, Reading, Pos);
			const uint32_t IntegerStart = Pos;
			while(Pos < End && std::isdigit(static_cast<unsigned char>(Src[Pos]))) ++Pos;
			// 整数のトークンを読み切れない形の終了
			if(Pos == IntegerStart || Pos < End && !std::isspace(static_cast<unsigned char>(Src[Pos]))) return;
			// 係数・符号・整数のトークン境界の修復
			if(CoefficientEnd < Sign) TextEdit::Push(CoefficientEnd, Sign, "", Edits);
			if(Sign + 1 < IntegerStart) TextEdit::Push(Sign + 1, IntegerStart, "", Edits);
			// 終了
			return;
		}
	);
	TextEdit::Apply(Src, Edits);
	// 終了
	return;
}

/**
 * 値の補間を初回解析の前に囲み又は密着した語として可逆に保護する関数
 * 宣言の終端で確定した値だけを対象とし，セレクタ・コメントの本文を変更しない
 * 計算量：入力長 B，編集数 E と適用の費用 Q に対し O(B + E log(E + 1) + Q)
 * @param Profile 対象の文法
 * @param Text 解析用のテキスト
 * @param Batches 準備前へ戻す位置の対応の格納先
 * @param Restorations 本来の本文と種類の格納先
 * @return 準備したテキスト（変更が無ければ空）
 * @throws FormatLimitExceeded 修復後の解析が上限を超える場合
 * @throws std::bad_alloc 編集又はデコードデータを確保出来ない場合
 * @throws std::length_error 本文又は編集の要素数上限を超える場合
 */
std::string ParseAdapter::PrepareValueInterpolations(
	const GrammarProfile &Profile,
	const std::string &Text,
	std::vector<AppliedEdit> &Batches,
	std::vector<LexemeRestoration> &Restorations
) {
	const ReadingRules &Reading = Profile.GetReading();
	if(Reading.ValueInterpolationOpen.empty() || Text.find(Reading.ValueInterpolationOpen) == std::string::npos) {
		// 値の補間の保護を要しない入力の返戻
		return std::string();
	}
	const std::string_view Open = Reading.ValueInterpolationOpen, Close = Reading.ValueInterpolationClose;
	const std::string_view BlockOpen = Core::CommentRepository::GetOpenerOf(CommentNotation::Block);
	const std::string_view BlockClose = Core::CommentRepository::GetCloserOf(CommentNotation::Block);
	const std::string_view LineOpen = Core::CommentRepository::GetOpenerOf(CommentNotation::SlashLine);
	struct Span {
		uint32_t Start; // 保護する範囲の始まり
		uint32_t End; // 保護する範囲の終わり
		bool IsContained; // 引用文字列又は引用符の無い値の本文内の補間か
		bool IsJoined; // 語又は単位と密着した列全体を保護するか
	};
	std::vector<Span> Pending, Selected;
	std::string PendingProbe, ProbeText;
	// 密着した列は言語の名前の字と単位・符号の登録定義で読む
	const auto IsJoinChar = [&Reading](const unsigned char Byte) -> bool {
		// 語の内側で補間と密着する字かの返戻
		return IsIdentifierChar(Byte) || Reading.ValueInterpolationJoinChars.find(Byte) != std::string_view::npos;
	};
	// 保護する式は独立した値として１度の試行解析へ纏める
	const auto AddProbe = [&Text, Open, Close, &PendingProbe](const size_t Start, const size_t End) -> void {
		PendingProbe += "x { y: (";
		PendingProbe.append(Text, Start + Open.size(), End - Start - Open.size() - Close.size());
		PendingProbe += "\n); }\n";
		// 終了
		return;
	};
	size_t ParenDepth = 0;
	bool IsValue = false, CanProperty = true, HasProperty = false, HasNameEnd = false;
	std::vector<std::pair<size_t, size_t>> UrlInterpolations;
	// 引用文字列と引用符の無い値の本文の補間を同じ範囲として収集する
	// @param Start 補間の始まり
	// @param End 補間の閉じの後
	const auto AddContainedInterpolation = [&](const size_t Start, const size_t End) -> void {
		if(IsValue) {
			Pending.push_back({ static_cast<uint32_t>(Start), static_cast<uint32_t>(End), true, false });
			AddProbe(Start, End);
		}
		// 終了
		return;
	};
	// 候補は宣言の終端で確定し，セレクタの本体の開きでは破棄する
	for(size_t Pos = 0; Pos < Text.size();) {
		const char Byte = Text[Pos];
		// 引用符の無い値の本文は斜線をコメントと読まずに保つ
		if(const std::optional<size_t> End = FindUnquotedUrlEnd(Reading, Text, Pos, IsValue ? &UrlInterpolations : nullptr)) {
			for(const auto &[Start, Next] : UrlInterpolations) {
				// 未閉じの補間を含む入力を変更しない事の返戻
				if(Next == std::string_view::npos) return std::string();
				AddContainedInterpolation(Start, Next);
			}
			UrlInterpolations.clear();
			Pos = *End;
			continue;
		}
		if(std::string_view(Text).substr(Pos).starts_with(BlockOpen)) {
			const size_t End = Text.find(BlockClose, Pos + BlockOpen.size());
			const size_t Next = End == std::string_view::npos ? Text.size() : End + BlockClose.size();
			Pos = Next;
			continue;
		}
		if(std::string_view(Text).substr(Pos).starts_with(LineOpen)) {
			const size_t Next = std::min(Text.find('\n', Pos + LineOpen.size()), Text.size());
			Pos = Next;
			continue;
		}
		if(Byte == '\\') {
			if(!IsValue) CanProperty = false;
			Pos = std::min(Pos + 2, Text.size());
			continue;
		}
		if(std::string_view(Text).substr(Pos).starts_with(Open)) {
			const size_t FirstEnd = FindValueInterpolationEnd(Reading, Text, Pos);
			// 閉じを欠く補間を推測して保護しない事の返戻
			if(FirstEnd == std::string_view::npos) return std::string();
			if(IsValue) {
				size_t Start = Pos, End = FirstEnd;
				while(Start && IsJoinChar(Text[Start - 1])) --Start;
				const bool IsJoined =
				Start < Pos || End < Text.size() && (IsJoinChar(Text[End]) || std::string_view(Text).substr(End).starts_with(Open));
				if(IsJoined) {
					AddProbe(Pos, FirstEnd);
					while(End < Text.size()) {
						if(std::string_view(Text).substr(End).starts_with(Open)) {
							const size_t Next = FindValueInterpolationEnd(Reading, Text, End);
							// 閉じを欠く密着した列を保護しない事の返戻
							if(Next == std::string_view::npos) return std::string();
							AddProbe(End, Next);
							End = Next;
						} else if(IsJoinChar(Text[End])) ++End;
						else break;
					}
				}
				Pending.push_back({ static_cast<uint32_t>(Start), static_cast<uint32_t>(End), false, IsJoined });
				Pos = End;
			} else {
				CanProperty = CanProperty && !HasNameEnd;
				HasProperty = true;
				Pos = FirstEnd;
			}
			continue;
		}
		if(Byte == '\'' || Byte == '"') {
			if(!IsValue) CanProperty = false;
			++Pos;
			while(Pos < Text.size() && Text[Pos] != Byte) {
				if(Text[Pos] == '\\') Pos = std::min(Pos + 2, Text.size());
				else if(std::string_view(Text).substr(Pos).starts_with(Open)) {
					const size_t End = FindValueInterpolationEnd(Reading, Text, Pos);
					// 未閉じの補間を含む入力を変更しない事の返戻
					if(End == std::string_view::npos) return std::string();
					AddContainedInterpolation(Pos, End);
					Pos = End;
				} else ++Pos;
			}
			// 引用を閉じれない入力を変更しない
			if(Pos == Text.size()) return std::string();
			++Pos;
			continue;
		}
		if(Byte == '(' || Byte == '[') ++ParenDepth;
		else if((Byte == ')' || Byte == ']') && ParenDepth) --ParenDepth;
		else if(Byte == ':' && !ParenDepth && !IsValue) IsValue = CanProperty && HasProperty;
		else if(!ParenDepth && (Byte == ';' || Byte == '{' || Byte == '}')) {
			if((Byte == ';' || Byte == '}') && IsValue) {
				Selected.insert(Selected.end(), Pending.begin(), Pending.end());
				ProbeText += PendingProbe;
			}
			PendingProbe.clear();
			Pending.clear();
			IsValue = false;
			CanProperty = true;
			HasProperty = HasNameEnd = false;
		} else if(!IsValue) {
			if(std::isspace(static_cast<unsigned char>(Byte))) HasNameEnd = HasProperty;
			else {
				CanProperty = CanProperty && !HasNameEnd && (
					std::isalpha(static_cast<unsigned char>(Byte)) || static_cast<unsigned char>(Byte) > 0X7F ||
					HasProperty && std::isdigit(static_cast<unsigned char>(Byte)) || Byte == '_' || Byte == '-' || Byte == '$'
				);
				HasProperty = true;
			}
		}
		++Pos;
	}
	// 逐語に保護する式を１度の試行解析で確かめ，誤った式を隠さない
	if(!ProbeText.empty()) {
		const ParsedTree Probe(Profile, std::move(ProbeText));
		if(!Probe.IsParsed()) throw FormatLimitExceeded("value interpolation probe did not complete", GetTrialFailure(Probe).Val);
		// 誤読の修復を要する式又は不正な式の保護を控える
		if(ts_node_has_error(Probe.GetRoot())) return std::string();
	}
	std::vector<TextEdit> Edits;
	for(const Span &Range : Selected) {
		if(Range.IsContained || Range.IsJoined) {
			// 本文内の補間又は密着した列全体を保護し，本文を１つのトークンへ戻す
			TextEdit::Push(Range.Start, Range.End, "x", Edits);
			Restorations.push_back(
				{ Range.Start, Range.End, std::string(Text.substr(Range.Start, Range.End - Range.Start)), Range.IsJoined, Range.IsJoined }
			);
		} else {
			TextEdit::Push(Range.Start, Range.Start + Open.size(), "(", Edits);
			TextEdit::Push(Range.End - Close.size(), Range.End, ")", Edits);
			Restorations.push_back({ Range.Start, Range.Start + static_cast<uint32_t>(Open.size()), std::string(Open) });
			Restorations.push_back({ Range.End - static_cast<uint32_t>(Close.size()), Range.End, std::string(Close) });
		}
	}
	// 値の補間を持たない入力の返戻
	if(Edits.empty()) return std::string();
	std::string Output;
	size_t Copied = 0, Next = Restorations.size() - Edits.size();
	for(const TextEdit &Edit : Edits) {
		Output.append(Text, Copied, Edit.StartPos - Copied);
		const uint32_t Start = static_cast<uint32_t>(Output.size());
		Output += Edit.NewText;
		RecordPreparationEdit(Start, Start + Edit.EndPos - Edit.StartPos, Edit.NewText.size(), Batches);
		LexemeRestoration &Restoration = Restorations[Next++];
		Restoration.Start = Start;
		Restoration.End = static_cast<uint32_t>(Output.size());
		Copied = Edit.EndPos;
	}
	Output.append(Text, Copied);
	// 準備したテキストの返戻
	return Output;
}

/**
 * キー値のコロンを値の本文へ吸収するエラーの修復関数
 * 解析用の演算子と仮括弧でキー・値を別の子へ分け，投影がコロンとキー値の役割へ戻す
 * 計算量：ノード数 N，入力長 B，編集数 E と適用の費用 Q に対し O(N log(N + 1) + B log(N + 2) + E log(E + 1) + Q)
 * @param Src 解析済の入力
 * @throws FormatLimitExceeded 修復後の解析が上限を超える場合
 * @throws std::bad_alloc 走査又は編集のメモリを確保出来ない場合
 * @throws std::length_error 走査又は編集の要素数上限を超える場合
 */
void ParseAdapter::RepairColonPairs(ParsedTree &Src) {
	// 文法の読違の対応と，逐語に保つ URL の引数の収集
	const ReadingRules &Reading = Src.GetReading();
	// 対象の読違を持たない文法の終了
	if(Reading.ColonPairStandIn.empty()) return;
	std::vector<std::pair<uint32_t, uint32_t>> Excluded;
	std::vector<uint32_t> ParsedOpenings;
	WalkRecordedChildren(
		Src,
		Src.GetRoot(),
		[&](const TSNode Node) -> bool {
			const bool IsArgs = Src.Is(Node, Core::Role::CallArgs);
			if(IsArgs && Src.IsCssUrlArgs(Node)) {
				Excluded.emplace_back(Src.GetStart(Node), Src.GetEnd(Node));
				// URL の本文へ降りない事の返戻
				return false;
			}
			// コロンを既に読めた構文と仮引数の括弧を，マップとして修復しない
			if(
				(Src.Is(Node, Core::Role::NormColon) || Src.Is(Node, Core::Role::MixinParams)) &&
				Src.Is(Src.GetChild(Node, 0), Core::Role::OpeningParenToken)
			) ParsedOpenings.push_back(Src.GetStart(Node));
			// 実引数の並びと，区切を関数名へ吸収した値の並びを区別する
			if(IsArgs) {
				const TSNode Parent = Src.GetParentOf(Node);
				const TSNode Head = Src.GetChild(Parent, 0);
				if(!Src.Is(Parent, Core::Role::CallExpr) || !Src.GetView(Head).ends_with(":")) ParsedOpenings.push_back(Src.GetStart(Node));
			}
			// 次の子を読む事の返戻
			return true;
		}
	);
	for(const ParsedTree::CommentSpan &Comment : Src.GetCommentSpans()) Excluded.emplace_back(Comment.Start, Comment.End);
	std::sort(Excluded.begin(), Excluded.end());
	std::sort(ParsedOpenings.begin(), ParsedOpenings.end());
	// １つのマップの全ての要素がキー値である事を，閉境界で確定する
	struct Pair {
		uint32_t KeyStart; // キーの始まり
		uint32_t KeyEnd; // キーの終わり
		uint32_t Colon; // キー値の区切
		uint32_t ValStart; // 値の始まり
		uint32_t ValEnd; // 値の終わり
	};
	struct Frame {
		uint32_t Item = 0; // 現在の要素の始まり
		uint32_t Colon = std::numeric_limits<uint32_t>::max(); // 区切の位置（無ければ最大値）
		char Close = '\0'; // 対応する閉記号
		bool CanPair = false; // 丸括弧の値の並びか
		bool HasOther = false; // キー値でない要素を含むか
		std::vector<Pair> Pairs; // 此の並びの要素
	};
	std::vector<Frame> Stack;
	std::vector<Pair> Pairs;
	const auto FinishItem = [&Src](const uint32_t End, Frame &Current) -> void {
		// 要素の外の空白を除き，区切の左右に値が在る事の確認
		// キー値を取らない本体の終了
		if(!Current.CanPair) return;
		uint32_t Start = Current.Item, Stop = End;
		while(Start < Stop && std::isspace(static_cast<unsigned char>(Src[Start]))) ++Start;
		while(Stop > Start && std::isspace(static_cast<unsigned char>(Src[Stop - 1]))) --Stop;
		// 空の並び又は末尾のカンマの後の終了
		if(Start == Stop) return;
		if(Current.Colon <= Start || Current.Colon >= Stop) {
			Current.HasOther = true;
			// キー値でない要素の終了
			return;
		}
		uint32_t KeyEnd = Current.Colon, ValStart = Current.Colon + 1;
		while(KeyEnd > Start && std::isspace(static_cast<unsigned char>(Src[KeyEnd - 1]))) --KeyEnd;
		while(ValStart < Stop && std::isspace(static_cast<unsigned char>(Src[ValStart]))) ++ValStart;
		if(KeyEnd == Start || ValStart == Stop) Current.HasOther = true;
		else Current.Pairs.push_back({ Start, KeyEnd, Current.Colon, ValStart, Stop });
		// 終了
		return;
	};
	// 引用本文・コメント・URL の外の括弧と，同じ深さの区切だけを読む
	const std::string_view Text = Src.GetCodeText();
	const std::string_view BlockOpen = Core::CommentRepository::GetOpenerOf(CommentNotation::Block);
	const std::string_view BlockClose = Core::CommentRepository::GetCloserOf(CommentNotation::Block);
	const std::string_view LineOpen = Core::CommentRepository::GetOpenerOf(CommentNotation::SlashLine);
	const bool HasLineComments = Src.GetRules().Notation.IsAvailable(CommentNotation::SlashLine);
	size_t NextExcluded = 0;
	bool IsStatementStart = true, IsQuery = false;
	for(uint32_t Pos = 0; Pos < Src.size(); ++Pos) {
		while(NextExcluded < Excluded.size() && Excluded[NextExcluded].second <= Pos) ++NextExcluded;
		if(NextExcluded < Excluded.size() && Excluded[NextExcluded].first <= Pos) {
			Pos = Excluded[NextExcluded].second - 1;
			continue;
		}
		// 補間の波括弧を，条件の終端又は宣言本体の開始と読まない
		if(!Reading.ValueInterpolationOpen.empty() && Text.substr(Pos).starts_with(Reading.ValueInterpolationOpen)) {
			const size_t End = FindValueInterpolationEnd(Reading, Text, Pos);
			if(End == std::string_view::npos) break;
			IsStatementStart = false;
			Pos = static_cast<uint32_t>(End - 1);
			continue;
		}
		// 回復でコメントのノードを失っても，本文を構文の区切として読まない
		if(Text.substr(Pos).starts_with(BlockOpen)) {
			const size_t End = Text.find(BlockClose, Pos + BlockOpen.size());
			if(End == std::string_view::npos) break;
			Pos = static_cast<uint32_t>(End + BlockClose.size() - 1);
			continue;
		}
		if(HasLineComments && Text.substr(Pos).starts_with(LineOpen)) {
			const size_t End = Text.find('\n', Pos + LineOpen.size());
			if(End == std::string_view::npos) break;
			Pos = static_cast<uint32_t>(End);
			continue;
		}
		// 引用符の無い URL の斜線を，コメントの開きへ読み替えない
		if(const std::optional<size_t> End = FindUnquotedUrlEnd(Reading, Text, Pos)) {
			IsStatementStart = false;
			Pos = static_cast<uint32_t>(*End - 1);
			continue;
		}
		const char Byte = Src[Pos];
		if(std::isspace(static_cast<unsigned char>(Byte))) continue;
		// 回復途中の木が条件のノードを失っても，条件のコロンをマップへ変えない
		if(Byte == '@' && IsStatementStart) {
			uint32_t End = Pos + 1;
			while(End < Src.size() && (IsIdentifierChar(static_cast<unsigned char>(Src[End])) || Src[End] == '-')) ++End;
			const std::string_view Head = std::string_view(Src).substr(Pos, End - Pos);
			IsQuery = std::ranges::find(Reading.ColonPairValueHeads, Head) == Reading.ColonPairValueHeads.end();
			IsStatementStart = false;
			Pos = End - 1;
			continue;
		}
		IsStatementStart = Byte == ';' || Byte == '{' || Byte == '}';
		if(IsStatementStart) IsQuery = false;
		if(Byte == '\\') {
			if(Pos + 1 < Src.size()) ++Pos;
			continue;
		}
		if(Byte == '\'' || Byte == '"') {
			const char Quote = Byte;
			while(++Pos < Src.size()) {
				if(Src[Pos] == '\\' && Pos + 1 < Src.size()) ++Pos;
				else if(Src[Pos] == Quote) break;
			}
			continue;
		}
		if(Byte == '(' || Byte == '[' || Byte == '{') {
			const unsigned char Before = Pos ? static_cast<unsigned char>(Src[Pos - 1]) : 0;
			Stack.emplace_back();
			Frame &Current = Stack.back();
			Current.Item = Pos + 1;
			Current.Close = Byte == '(' ? ')' : Byte == '[' ? ']' : '}';
			Current.CanPair = Byte == '(' && !IsQuery && !std::binary_search(ParsedOpenings.begin(), ParsedOpenings.end(), Pos) && !(
				std::isalnum(Before) || Before > 0X7F || Before == '_' || Before == '-' || Before == '$' || Before == '.' || Before == ')' ||
				Before == ']' || Before == '}'
			);
			continue;
		}
		if(Stack.empty()) continue;
		Frame &Current = Stack.back();
		if(Byte == ':' && Current.Colon == std::numeric_limits<uint32_t>::max()) Current.Colon = Pos;
		else if(Byte == ',' || Byte == Current.Close) {
			FinishItem(Pos, Current);
			if(Byte == Current.Close) {
				if(Current.CanPair && !Current.HasOther) Pairs.insert(Pairs.end(), Current.Pairs.begin(), Current.Pairs.end());
				Stack.pop_back();
			} else {
				Current.Item = Pos + 1;
				Current.Colon = std::numeric_limits<uint32_t>::max();
			}
		}
	}
	// キー・値の結合を解析用の囲みで保ち，コロンの読替を１度に適用
	std::vector<TextEdit> Edits;
	std::vector<std::pair<uint32_t, uint32_t>> Groups;
	std::sort(
		Pairs.begin(),
		Pairs.end(),
		[](const Pair &Left, const Pair &Right) -> bool {
			// 原文の区切の順の返戻
			return Left.Colon < Right.Colon;
		}
	);
	for(const Pair &Entry : Pairs) {
		TextEdit::Push(Entry.KeyStart, Entry.KeyStart, "(", Edits);
		TextEdit::Push(Entry.KeyEnd, Entry.KeyEnd, ")", Edits);
		TextEdit::Push(Entry.Colon, Entry.Colon + 1, " " + std::string(Reading.ColonPairStandIn) + " ", Edits);
		TextEdit::Push(Entry.ValStart, Entry.ValStart, "(", Edits);
		TextEdit::Push(Entry.ValEnd, Entry.ValEnd, ")", Edits);
		Groups.emplace_back(Entry.KeyStart, Entry.KeyEnd);
		Groups.emplace_back(Entry.ValStart, Entry.ValEnd);
	}
	// マップを持たない入力の終了
	if(Edits.empty()) return;
	std::sort(Groups.begin(), Groups.end());
	TextEdit::Apply(Src, Edits, false, &Groups);
	const std::vector<AppliedEdit> &Batch = Src.EditBatches.back();
	size_t NextPair = 0;
	for(const AppliedEdit &Edit : Batch) {
		if(NextPair == Pairs.size()) break;
		if(Edit.Start == Pairs[NextPair].Colon && Edit.End == Edit.Start + 1) {
			Src.AddLexemeRestoration(Edit.NewStart + 1, Edit.NewEnd - 1, ":");
			++NextPair;
		}
	}
	// 終了
	return;
}

/**
 * 幅の定数式へ初期化子を取り込むメンバ宣言の読違の修復関数
 * 解析用には配列の宣言子で幅と初期化子を分け，投影時に名前・コロン・幅の構造へ戻す
 * 計算量：ノード数 N，編集数 E と適用の費用 Q に対し O(N + E + Q)
 * @param Src 解析済のソース
 * @throws FormatLimitExceeded 修復又は解析の作業予算を超えた場合
 * @throws std::bad_alloc 編集又は復元データのメモリを確保出来ない場合
 * @throws std::length_error 本文又は編集の要素数上限を超える場合
 */
void ParseAdapter::RepairSizedMemberInitializers(ParsedTree &Src) {
	// 此の読違を持つ宣言子の文法の記号と編集列
	const TSSymbol Clause = Src.GetProfile().FindSymbolWithRole(Core::Role::SizedDeclarator);
	// 対象の宣言子を持たない文法の終了
	if(!Clause) return;
	std::vector<TextEdit> Edits;
	WalkRecordedChildren(
		Src,
		Src.GetRoot(),
		[&](const TSNode Node) -> bool {
			Src.BoundaryRepairWork.Charge(1);
			// メンバの幅を包む種別をエラー回復が落とした場合も，同じ構造の候補とする
			const bool IsBrokenClause = Src.Is(Node, Core::Role::Error) && Src.Is(Src.GetParentOf(Node), Core::Role::FieldDef);
			// 幅を指定しない構文の子へ降りる事の返戻
			if(Src.GetSymbol(Node) != Clause && !IsBrokenClause) return true;
			TSNode Width {}, Colon {};
			uint32_t Parts = 0;
			for(const ParsedChild &Child : Src.GetChildrenOf(Node)) {
				Src.BoundaryRepairWork.Charge(1);
				if(Src.Is(Child.Node, Core::Role::Comment)) continue;
				++Parts;
				if(!Child.IsNamed && Src.Is(Child.Node, Core::Role::ColonToken)) Colon = Child.Node;
				else if(Child.IsNamed && ts_node_is_null(Width)) Width = Child.Node;
			}
			// コロンと幅以外の部品，又は幅の欠落を修復しない事の返戻
			if(Parts != 2 || ts_node_is_null(Colon) || ts_node_is_null(Width) || Src.IsMissing(Width)) return true;
			uint32_t WidthEnd = Src.GetEnd(Width);
			// 定数式の右端だけを辿り，括弧の内部又は条件式の枝に在る代入を初期化子へ移さない
			TSNode Edge = Width;
			while(!ts_node_is_null(Edge)) {
				Src.BoundaryRepairWork.Charge(1);
				if(Src.Is(Edge, Core::Role::Assignment)) {
					const TSNode Left = Src.GetFieldChild(Edge, Core::ChildRole::Left);
					// 右端で誤読した単純代入だけを，幅の後の初期化子へ分ける
					if(
						!ts_node_is_null(Left) &&
						Src.GetView(Src.GetFieldChild(Edge, Core::ChildRole::OperatorField)) == Src.GetRules().Operators.Assignment
					) WidthEnd = Src.GetEnd(Left);
					break;
				}
				if(Src.Is(Edge, Core::Role::BinaryOperation)) Edge = Src.GetFieldChild(Edge, Core::ChildRole::Right);
				else if(Src.Is(Edge, Core::Role::SizeofExpr)) {
					const TSNode Operand = Src.GetFieldChild(Edge, Core::ChildRole::Val);
					const TSNode Type = Src.GetFieldChild(Operand, Core::ChildRole::Type);
					uint32_t Close = 0;
					bool HasInitializer = Src.Is(Src.GetFieldChild(Operand, Core::ChildRole::Val), Core::Role::InitializerExpr);
					// 型の測定の閉括弧の後の初期化を，キャスト又は複合リテラルの値と読んだ境界の確認
					if(!ts_node_is_null(Type) && !Src.HasError(Type)) for(const ParsedChild &Child : Src.GetChildrenOf(Operand)) {
						Src.BoundaryRepairWork.Charge(1);
						if(Src.Is(Child.Node, Core::Role::ClosingParenToken)) Close = Child.End;
						if(Src.Is(Child.Node, Core::Role::Error) && Src.GetView(Child.Node) == Src.GetRules().Operators.Assignment) {
							HasInitializer = true;
						}
					}
					if(Close && HasInitializer) {
						WidthEnd = Close;
						break;
					}
					Edge = Operand;
				} else if(Src.Is(Edge, Core::Role::UnaryOperation) || Src.Is(Edge, Core::Role::CastExpr)) {
					TSNode Operand = Src.GetFieldChild(Edge, Core::ChildRole::Val);
					if(ts_node_is_null(Operand)) Operand = Src.GetFieldChild(Edge, Core::ChildRole::ArgField);
					Edge = Operand;
				} else break;
			}
			// 幅の内部のエラーは，測定対象の後で誤読した初期化子と区別する
			if(Src.HasError(Width) && WidthEnd == Src.GetEnd(Width)) return true;
			// エラー回復の包みの外で，直前の名前付宣言子を確かめる
			TSNode Host = Node;
			if(Src.Is(Src.GetParentOf(Host), Core::Role::Error)) Host = Src.GetParentOf(Host);
			TSNode Name = ts_node_prev_sibling(Host);
			while(!ts_node_is_null(Name) && Src.Is(Name, Core::Role::Comment)) {
				Src.BoundaryRepairWork.Charge(1);
				Name = ts_node_prev_sibling(Name);
			}
			// 無名の幅指定は初期化子を持たず，配列の宣言子へ読み替えない事の返戻
			if(ts_node_is_null(Name) || Src.IsMissing(Name) || !Src.Is(Name, Core::Role::FieldName)) return true;
			TextEdit::Push(Src.GetStart(Colon), Src.GetEnd(Colon), "[", Edits);
			TextEdit::Push(WidthEnd, WidthEnd, "]", Edits);
			Src.BoundaryRepairWork.Charge(2);
			// 入子の宣言も修復する為，子へ降りる事の返戻
			return true;
		}
	);
	// 修復する宣言子の無い入力の終了
	if(Edits.empty()) return;
	TextEdit::Apply(Src, Edits);
	// 既存の復元データと今回のデータを，其々の原文順を保って１度だけ合流する
	std::vector<LexemeRestoration> Previous, Added;
	Previous.swap(Src.LexemeRestorations);
	for(const AppliedEdit &Edit : Src.EditBatches.back()) {
		Src.BoundaryRepairWork.Charge(1);
		Added.push_back({ Edit.NewStart, Edit.NewEnd, Edit.Start == Edit.End ? "" : ":" });
	}
	Src.LexemeRestorations.reserve(Previous.size() + Added.size());
	std::ranges::merge(
		std::make_move_iterator(Previous.begin()),
		std::make_move_iterator(Previous.end()),
		std::make_move_iterator(Added.begin()),
		std::make_move_iterator(Added.end()),
		std::back_inserter(Src.LexemeRestorations),
		std::less<uint32_t>(),
		&LexemeRestoration::Start,
		&LexemeRestoration::Start
	);
	Src.BoundaryRepairWork.Charge(Previous.size() + Added.size());
	// 終了
	return;
}

/**
 * コメント分離前にパーサーの読違を修復する共通入口
 * 入力と完成候補の再投影で同じ文法上の修復を用いる
 * 計算量：各修復の費用の和 Q に対し O(Q)
 * @param Src 解析済のソース
 * @return 修復出来ない入力の見送理由，修復出来た場合は理由無
 * @throws FormatLimitExceeded 既知の読違を修復しても正しい木を得れない場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
FailureReason ParseAdapter::RepairBeforeProjection(ParsedTree &Src) {
	// 文法の読取規則に応じた読違の修復
	const ReadingRules &Rules = Src.GetReading();
	RepairOrdinalArguments(Src);
	RepairColonPairs(Src);
	RepairSizedMemberInitializers(Src);
	if(Rules.CanMisreadCommandArgBoundary) RepairRubyCommandArgs(Src);
	if(Rules.HasArrowChainMisreads) {
		RepairJavaScriptArrowChains(Src);
		// 禁止された文境界を修復せず見送る理由の返戻
		if(const FailureReason Reason = RepairJavaScriptJoints(Src); Reason.Val != FailureReason::None) return Reason;
	}
	if(!Rules.CommentLineBreaks.empty()) RepairCommentLineBreaks(Src);
	if(Rules.CanMisreadCallContinuation) {
		RepairKotlinSemicolons(Src);
		if(!RepairKotlinJoints(Src)) RepairKotlinObjectBodies(Src);
	}
	if(Rules.CanMisreadCasts) RepairCSharpMisreadCasts(Src);
	if(Rules.CanMisreadSelectorCommentBoundary) RepairCssSelectorComments(Src, Rules.CanSelectorCommentActAsSpace);
	if(Rules.CanMisassociateOperators) {
		// コメントに密着した演算子の読違を上限迄に解消出来ない場合の返戻
		if(const FailureReason Reason = RepairSwiftMisreads(Src); Reason.Val != FailureReason::None) return Reason;
		// 結合を組み直せない演算子の連鎖の見送理由の返戻
		if(const FailureReason Reason = RepairOperatorBindings(Src); Reason.Val != FailureReason::None) return Reason;
	}
	if(Rules.CanMisbindConstructionCallee) RepairConstructionBindings(Src);
	if(Rules.CanMisreadSoftKeywordCalls) RepairSoftKeywordCalls(Src);
	// 修復を完了した事の返戻
	return {};
}

/**
 * CSS の空のカスタムプロパティ値を偽の値ノードと読む誤りの判定関数
 * パーサーは空値に幅０の整数値を作り，`;` が無い場合は値の候補の欠落ノードを補う
 * 計算量：ノード数 N と直接の子数 C に対し初回 O(N + C)，以後 O(C)
 * @param Src 解析済のソース
 * @param Node 判定対象のノード
 * @return 空のカスタムプロパティ値を表す偽のノードなら true
 * @throws std::bad_alloc 直接親の表を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool ParseAdapter::IsEmptyCssCustomVal(const ParsedTree &Src, const TSNode Node) {
	// 幅０の整数値又は欠落した値の候補でないノードの返戻
	if(
		!(Src.Is(Node, Core::Role::IntLiteralVal) || ts_node_is_missing(Node) && Src.Is(Node, Core::Role::NestingSelector)) ||
		Src.GetLen(Node)
	) return false;
	const TSNode Decl = Src.GetParentOf(Node);
	// 宣言の値でないノードの返戻
	if(ts_node_is_null(Decl) || !Src.Is(Decl, Core::Role::TypedDecl)) return false;
	const TSNode Property = ts_node_named_child(Decl, 0);
	const std::string_view Name = ts_node_is_null(Property) ? std::string_view() : Src.GetView(Property);
	const std::string_view Head = Src.GetReading().CustomPropertyHead;
	// カスタムプロパティの空値だけをパーサーの偽の値から区別する事の返戻
	return !Head.empty() && Src.Is(Property, Core::Role::PropertyNameNode) && Name.size() > Head.size() && Name.starts_with(Head);
}
