#include "Edit.hpp"
#include "../Util/NodeKind.hpp"
#include "../Util/Parallel.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * 文ブロック型かの判定関数
 * @param Node 判定対象の構文ノード
 * @param Language 対象言語
 * @return 文ブロック系なら true
 */
bool EditPass::IsStmtBlockType(const TSNode Node, const Lang Language) {
	// 節点型の取得
	const std::string_view Type(ts_node_type(Node));
	// 波括弧本体の確認
	if(Type == "control_structure_body") {
		const TSNode First = ts_node_child(Node, 0);
		// 波括弧開始かを返戻
		return !ts_node_is_null(First) && std::string_view(ts_node_type(First)) == "{";
	}
	// インデントで本体を表す型の返戻
	if(NodeKind::IndentContainer.Contains(Type)) return true;
	// Swift の switch 本体かを返戻
	return Language == Lang::Swift && Type == "switch_statement";
}

/**
 * Kotlin の文と枝を持つ式の値が使われるかの一括判定関数
 * 文の並びの最後の文は，ラムダの本体と，値として使う if / when / try の枝では値が結果に為る
 * @param Src ソースコード
 */
void EditPass::PrepareKotlinValueContexts(const TSSource &Src) {
	// 値利用状態の控え領域の取得
	std::unordered_map<const void *, uint8_t> &Memo = Src.GetContextMemo();
	// 控え済の終了
	if(!Memo.empty()) return;
	const TSNode Root = Src.GetRoot();
	// 根の記録済印との兼用
	Memo[Root.id] = 0;
	// 文の並びの最後の文（コメントを除く）
	const auto LastStatement = [](const TSNode Statements) -> TSNode {
		TSNode Last {};
		// 名前付子の列挙
		ForEachNamedChild(
			Statements,
			[&Last](const TSNode Child) -> void {
				// 余分節点以外を末尾として更新
				if(!ts_node_is_extra(Child)) Last = Child;
			}
		);
		// 最後の実文を返戻
		return Last;
	};
	// 祖先と末尾文の追跡領域の初期化
	std::vector<TSNode> Path { Root }, Lasts { TSNode{} };
	const auto IsUsed = [&](const TSNode Node) -> bool {
		// 文脈走査位置の初期化
		size_t BodyAt = Path.size() - 1;
		// 文列直下の場合
		if(const std::string_view HolderType = ts_node_type(Path[BodyAt]); HolderType == "statements") {
			// 末尾文以外を未使用として返戻
			if(!ts_node_eq(Lasts[BodyAt], Node) || !BodyAt) return false;
			--BodyAt;
			// 代入の右辺・実引数等の値の位置に在る事の返戻
		} else if(HolderType != "control_structure_body") return true;
		// 本体型の取得
		const std::string_view BodyType = ts_node_type(Path[BodyAt]);
		// ラムダの本体は最後の文の値を結果にする事の返戻
		if(BodyType == "lambda_literal") return true;
		// 分岐祖先位置の初期化
		size_t BranchAt = BodyAt;
		// 分岐本体の親式への移動
		if(NodeKind::KotlinBranchBody.Contains(BodyType)) {
			// 根の直下の本体は値を持たない事の返戻
			if(!BranchAt) return false;
			--BranchAt;
			if(BranchAt && std::string_view(ts_node_type(Path[BranchAt])) == "when_entry") --BranchAt;
			// 繰返の本体等は値を持たない事の返戻
			if(BodyType == "control_structure_body" && !NodeKind::KotlinValueBranch.Contains(Path[BranchAt])) return false;
			// 関数の本体・finally 等は値を持たない事の返戻
		} else if(BodyType != "try_expression") return false;
		// 祖先の値利用キャッシュを検索
		const std::unordered_map<const void *, uint8_t>::const_iterator Hit = Memo.find(Path[BranchAt].id);
		// キャッシュ済の利用有無を返戻
		return Hit != Memo.end() && Hit->second;
	};
	// 親の値利用状態を子へ渡す為の根からの走査
	HeldCursor Cursor(Root);
	if(ts_tree_cursor_goto_first_child(&Cursor)) for(bool IsDone = false; !IsDone;) {
		const TSNode Node = ts_tree_cursor_current_node(&Cursor);
		const std::string_view Type = ts_node_type(Node);
		// 枝を持つ式と文列内文の状態記録
		if(
			ts_node_is_named(Node) && (
				NodeKind::KotlinValueBranch.Contains(Type) || Type == "try_expression" ||
				std::string_view(ts_node_type(Path.back())) == "statements"
			)
		) Memo[Node.id] = IsUsed(Node);
		// 子への降下と祖先・末尾文の記録
		if(ts_tree_cursor_goto_first_child(&Cursor)) {
			Path.push_back(Node);
			Lasts.push_back(Type == "statements" ? LastStatement(Node) : TSNode{});
			continue;
		}
		// 次の兄弟が無い間の親復帰と祖先記録の復元
		while(!ts_tree_cursor_goto_next_sibling(&Cursor)) {
			if(!ts_tree_cursor_goto_parent(&Cursor)) {
				IsDone = true;
				break;
			}
			Path.pop_back();
			Lasts.pop_back();
		}
	}
	// 終了
	return;
}

/**
 * Kotlin の文の値が使われるかの判定関数
 * @param Src ソースコード（PrepareKotlinValueContexts が判定を控えた物）
 * @param Statement 文の並びの中の文，又は枝を持つ式
 * @return 値が使われ得る場合 true
 */
bool EditPass::IsKotlinStatementValueUsed(const TSSource &Src, const TSNode Statement) {
	// 対象文の控え検索
	const std::unordered_map<const void *, uint8_t> &Memo = Src.GetContextMemo();
	const std::unordered_map<const void *, uint8_t>::const_iterator Hit = Memo.find(Statement.id);
	// 文の値が使われるかを返戻
	return Hit != Memo.end() && Hit->second;
}

/**
 * 行幅に依らない波括弧編集の収集関数
 * @param Src ソースコード（整形途中の作業領域）
 * @param Language 対象言語
 * @param Edits 付与・削除編集の格納先
 */
void EditPass::CollectBraceEdits(const TSSource &Src, const Lang Language, std::vector<TextEdit> &Edits) {
	// 構文木走査で全ての波括弧ブロック候補を列挙し，最内側のみ残置
	struct Candidate {
		uint32_t BodyStart; // 本体範囲の開始位置
		uint32_t BodyEnd; // 本体範囲の終了位置
		uint32_t StmtStart; // 単一文範囲の開始位置
		uint32_t StmtEnd; // 単一文範囲の終了位置
		bool ShouldAdd; // 波括弧の付与か否か
		bool IsBeforeElse = false; // 除く波括弧の後に Kotlin の if 式の後続の枝 (`else`) が続くか
	};
	// 前処理条件内も含む後続枝の字句判定（終端毎の控えは走脈・呼出毎に分離）
	static std::atomic<uint64_t> MemoEpochCounter = 0;
	const uint64_t MemoEpoch = ++MemoEpochCounter;
	const auto FollowedByElse = [&Src, MemoEpoch](const TSNode Body) -> bool {
		static thread_local uint64_t Epoch = 0;
		static thread_local std::unordered_map<uint32_t, bool> FollowedByElseMemo;
		// 別収集巡で得た位置状態の持越防止
		if(Epoch != MemoEpoch) {
			Epoch = MemoEpoch;
			FollowedByElseMemo.clear();
		}
		// 本体終端毎の判定控えの取得
		const auto [Memo, Fresh] = FollowedByElseMemo.try_emplace(Src.End(Body), false);
		// 既存の検出結果を返戻
		if(!Fresh) return Memo->second;
		// 字句走査する原文と語取得関数
		const std::string &Text = Src;
		const auto WordAt = [&Text](size_t Pos) -> std::string_view {
			// 語の前の水平空白の走査
			while(Pos < Text.size() && (Text[Pos] == ' ' || Text[Pos] == '\t')) ++Pos;
			// 識別語終端の走査
			size_t End = Pos;
			while(End < Text.size() && IsIdentifierChar(Text[End])) ++End;
			// 識別語のビューを返戻
			return std::string_view(Text.data() + Pos, End - Pos);
		};
		// 後続走査位置と条件深さの初期化
		size_t Pos = Src.End(Body);
		uint32_t Depth = 0;
		// 後続字句の走査
		while(true) {
			while(Pos < Text.size() && (Text[Pos] == ' ' || Text[Pos] == '\t' || Text[Pos] == '\n' || Text[Pos] == '\r')) ++Pos;
			if(Pos >= Text.size()) break;
			// 前処理条件の深さと else 有無の追跡
			const bool IsDirective = Text[Pos] == '#';
			if(
				const std::string_view Directive = IsDirective ? WordAt(Pos + 1) : std::string_view{};
				IsDirective && NodeKind::PreprocConditionWord.Contains(Directive)
			) ++Depth;
			else if(IsDirective && Directive == "endif" && Depth) --Depth;
			else if(!IsDirective && !Depth) break;
			else if(!IsDirective && NodeKind::ElseKeyword.Contains(WordAt(Pos))) {
				Memo->second = true;
				// else 有を返戻
				return true;
			}
			// 条件内の行を読み終えて次行への移行
			Pos = std::min(Text.find('\n', Pos), Text.size());
		}
		// 最終位置の else 有無を保存
		Memo->second = NodeKind::ElseKeyword.Contains(WordAt(Pos));
		// 確定した else 有無を返戻
		return Memo->second;
	};
	const auto EndsWithOpenIf = [Language](TSNode Cur, const bool IsThroughBraces) -> bool {
		// 後続枝を受け得る末尾子の走査
		while(true) {
			if(
				NodeKind::IfNode.Contains(Cur) && !HasChildOf(
					Cur,
					[](const TSNode Child) -> bool {
						// else 系の子かを返戻
						return NodeKind::ElseAny.Contains(Child);
					}
				)
				// 開いた if で終わる事の返戻
			) return true;
			// 末尾子の取得
			const uint32_t Count = ts_node_named_child_count(Cur);
			// 名前付子の無い節が開いた if で終わらない事の返戻
			if(!Count) return false;
			const TSNode Last = ts_node_named_child(Cur, Count - 1);
			if(ts_node_end_byte(Last) != ts_node_end_byte(Cur) && !(IsThroughBraces && Count == 1 && IsStmtBlockType(Cur, Language))) {
				// 末尾を辿れない場合の返戻
				return false;
			}
			// 次層への降下
			Cur = Last;
		}
	};
	const auto IsTsUsing = [](const TSNode Stmt) -> bool {
		// await の包みを解いて using 宣言の実体の確認
		TSNode Expr = ts_node_named_child(Stmt, 0);
		if(NamedTypeOf(Expr) == "await_expression") Expr = ts_node_named_child(Expr, 0);
		// 空節点で失敗する型・名前付判定の前に，空を扱える NamedTypeOf で除外
		if(NamedTypeOf(Expr) != "assignment_expression") return false;
		// using 宣言の先頭字句取得
		const TSNode First = ts_node_child(Expr, 0);
		// using トークンかを返戻
		return !ts_node_is_null(First) && !ts_node_is_named(First) && std::string_view(ts_node_type(First)) == "using";
	};
	const auto IfWithElse = [&Src, Language](const TSNode Node) -> bool {
		// else の枝の有無の返戻
		return NodeKind::IfNode.Contains(Node) && (
			!ts_node_is_null(TSSource::FieldChild(Node, "alternative")) ||
			Language == Lang::Kotlin && HasUnnamedTokenChild(Src, Node, "else")
		);
	};
	// else の本体・値を使う Kotlin の枝・do-while 等の所有節点に依る文脈の区別
	const auto CollectBody = [&Src, Language, &FollowedByElse, &EndsWithOpenIf, &IsTsUsing, &IfWithElse](
		const TSNode Body,
		const bool IsElseBody,
		const TSNode ValueHost,
		const TSNode Owner,
		std::vector<Candidate> &Sink
	) -> void {
		// 本体が無効なら処理打切
		if(ts_node_is_null(Body)) return;
		// 本体範囲と波括弧有無の取得
		const uint32_t BodyStart = Src.Start(Body), BodyEnd = Src.End(Body);
		const bool IsBraced = BodyStart < BodyEnd && Src[BodyStart] == '{';
		TSNode Inner = Language == Lang::Kotlin && !IsBraced ? ts_node_named_child(Body, 0) : Body;
		// ラベル名を除いた本体で包込の要否を判定
		while(!ts_node_is_null(Inner) && NodeKind::LabeledStatement.Contains(Inner) && ts_node_named_child_count(Inner) > 1) {
			Inner = ts_node_named_child(Inner, ts_node_named_child_count(Inner) - 1);
		}
		// 波括弧追加を要する本体の検査
		if(
			NodeKind::BodyDeclaration.Contains(Body) || !ts_node_is_null(Inner) && (
				NodeKind::DoLoop.Contains(Inner) || !IsElseBody &&
				(IfWithElse(Inner) || !IsBraced && FollowedByElse(Body) && EndsWithOpenIf(Inner, true) && !EndsWithOpenIf(Inner, false))
			)
		) {
			Sink.push_back({ BodyStart, BodyEnd, BodyStart, BodyEnd, true });
			// 付与の候補を積んで終了
			return;
		}
		if(
			BodyStart >= BodyEnd || BodyStart >= Src.size() || Src[BodyStart] != '{' || Src[BodyEnd - 1] != '}' ||
			ts_node_named_child_count(Body) != 1
			// 条件成立時の返戻
		) return;
		// 単一文の実体と型の取得
		TSNode Stmt = ts_node_named_child(Body, 0);
		std::string_view StmtType(ts_node_type(Stmt));
		// Kotlin の `control_structure_body` は内側に `statements` を包む為，文数判定は中身での実施
		if(StmtType == "statements") {
			// Kotlin の statements が単一文でないなら対象外
			if(ts_node_named_child_count(Stmt) != 1) return;
			Stmt = ts_node_named_child(Stmt, 0);
			StmtType = std::string_view(ts_node_type(Stmt));
		}
		// 除去範囲の初期化
		TSNode Block = Body;
		// 単一文を包むネストブロックの走査
		while(
			Language != Lang::Kotlin && IsStmtBlockType(Stmt, Language) && ts_node_named_child_count(Stmt) == 1 &&
			Src[Src.Start(Stmt)] == '{'
		) {
			Block = Stmt;
			Stmt = ts_node_named_child(Stmt, 0);
			StmtType = ts_node_type(Stmt);
		}
		// Kotlin のラベル・注釈の内側で文の意味を判定
		TSNode Head = Stmt;
		if(Language == Lang::Kotlin) {
			while(std::string_view(ts_node_type(Head)) == "prefix_expression" && ts_node_named_child_count(Head) > 1) {
				// ラベル以外の前置式で終了
				if(!NodeKind::LabelNode.Contains(ts_node_type(ts_node_named_child(Head, 0)))) break;
				Head = ts_node_named_child(Head, ts_node_named_child_count(Head) - 1);
			}
			StmtType = ts_node_type(Head);
			// ラベル・注釈付ラムダの外側波括弧除去後の本体扱い
			if(Src[Src.Start(Head)] == '{') return;
		}
		// ラベルの付いた文は付与と同じくラベルの後の文で判定する（揃えないと付与と除去が交互に起きる）
		while(NodeKind::LabeledStatement.Contains(StmtType) && ts_node_named_child_count(Head) > 1) {
			Head = ts_node_named_child(Head, ts_node_named_child_count(Head) - 1);
			StmtType = ts_node_type(Head);
		}
		// 複文・宣言・値位置と，枝の結付や文の終端が変わる本体の波括弧の保護
		if(
			NodeKind::Comment.Contains(StmtType) || NodeKind::Preproc.Contains(StmtType) || NodeKind::BodyDeclaration.Contains(StmtType) ||
			NodeKind::PhpInlineHtml.Contains(StmtType) || Language.IsCFamily() && Src.ExpandsToStatements(Stmt) ||
			Language == Lang::TypeScript && StmtType == "expression_statement" && IsTsUsing(Stmt) || NodeKind::DoLoop.Contains(StmtType) ||
			NodeKind::DoLoop.Contains(Owner) && NodeKind::Loop.Contains(StmtType) || !IsElseBody && IfWithElse(Head) ||
			FollowedByElse(Body) && EndsWithOpenIf(Head, true) ||
			!ts_node_is_null(ValueHost) && NodeKind::KotlinNonExpression.Contains(StmtType) && IsKotlinStatementValueUsed(Src, ValueHost)
			// 条件成立時の返戻
		) return;
		const uint32_t StmtBeg = Src.Start(Stmt);
		// 条件成立時の返戻
		if(Src[StmtBeg] == '(') return;
		uint32_t ContentEnd = ts_node_start_byte(ts_node_child(Block, ts_node_child_count(Block) - 1));
		// 単一文後の空白を後退走査
		while(ContentEnd > StmtBeg && (Src[ContentEnd - 1] == ' ' || Src[ContentEnd - 1] == '\t' || Src[ContentEnd - 1] == '\n')) {
			--ContentEnd;
		}
		// 文の実体と後続枝の区切を除去候補へ記録
		Sink.push_back({ BodyStart, BodyEnd, StmtBeg, ContentEnd, false, Language == Lang::Kotlin && FollowedByElse(Body) });
	};
	// Kotlin の枝へ親・次兄弟を渡し，深い else if で根から親を引き直す二乗費用の回避
	const auto DispatchKotlinBranch =
	[&CollectBody, &Src](const TSNode Node, const TSNode Parent, const TSNode Next, std::vector<Candidate> &Sink) -> void {
		// when の単文枝本体での後続枝を保つ波括弧除去
		if(std::string_view(ts_node_type(Node)) == "when_expression") {
			ForEachNamedChild(
				Node,
				[&](const TSNode Entry) -> void {
					if(std::string_view(ts_node_type(Entry)) == "when_entry") {
						CollectBody(FirstNamedChildOfType(Entry, "control_structure_body"), true, Node, Node, Sink);
					}
				}
			);
			// when 式の枝の候補化の終了
			return;
		}
		// if 式の本体と後続枝の抽出
		TSNode First {}, Previous {}, Following {};
		ForEachChild(
			Node,
			[&](const TSNode Child) -> bool {
				if(ts_node_is_null(First)) {
					// 最初の本体と直前字句の取得
					if(std::string_view(ts_node_type(Child)) == "control_structure_body") First = Child;
					else Previous = Child;
					// 本体の包み迄進む事の返戻
					return true;
				}
				// 後続本体の取得
				if(ts_node_is_named(Child)) Following = Child;
				// else の本体を得る迄進む事の返戻
				return ts_node_is_null(Following);
			}
		);
		const bool IsEmptyThen =
		!ts_node_is_null(First) && !ts_node_is_null(Previous) && std::string_view(ts_node_type(Previous)) == "else";
		const TSNode Then = IsEmptyThen ? TSNode{} : First, Else = IsEmptyThen ? First : Following;
		const bool HasElse = HasUnnamedTokenChild(Src, Node, "else");
		// 式直後の演算継続有無の兄弟字句からの確認
		const std::string_view Tail = ts_node_is_null(Next) ? std::string_view{} : Src.View(Next);
		const bool IsContinued = !Tail.empty() && !NodeKind::StatementHost.Contains(Parent) &&
		!(Tail.size() == 1 && std::string_view(")]},;").find(Tail) != std::string_view::npos);
		// 最終枝の波括弧が後続演算を if 全体へ結合している場合は保持
		if(!IsContinued || HasElse) CollectBody(Then, false, Node, Node, Sink);
		if(!IsContinued) CollectBody(Else, true, Node, Node, Sink);
	};
	std::atomic<bool> PhpMisread = false;
	const auto DispatchNode = [&CollectBody, &EndsWithOpenIf, &DispatchKotlinBranch, &PhpMisread, &Src, Language](
		const TSNode Node,
		std::vector<Candidate> &Sink
	) -> void {
		// 節点型に応じた候補振分
		const std::string_view NodeType(ts_node_type(Node));
		if(Language == Lang::Kotlin) {
			TSNode Pending {};
			ForEachChild(
				Node,
				[&](const TSNode Child) -> void {
					// 保留中の枝の確定と次候補の更新
					if(!ts_node_is_null(Pending)) DispatchKotlinBranch(Pending, Node, Child, Sink);
					Pending = NodeKind::KotlinValueBranch.Contains(Child) ? Child : TSNode{};
				}
			);
			// 次の兄弟が無い末尾の枝も確定
			if(!ts_node_is_null(Pending)) DispatchKotlinBranch(Pending, Node, TSNode{}, Sink);
			// if 式自身は親の側で候補化済の為，終了
			if(NodeType == "if_expression") return;
		}
		// 枝の包み方に応じて実際の本体を候補化
		if(NodeType == "else_clause" && ts_node_named_child_count(Node)) {
			CollectBody(ts_node_named_child(Node, 0), true, TSNode{}, Node, Sink);
		} else if(NodeType != "else_clause" && NodeKind::IfNode.Contains(NodeType)) {
			const TSNode Consequence = TSSource::IfConsequence(Node);
			// PHP の最内の開いた if に属する枝の誤読検出（包む範囲は後段で求める）
			if(Language == Lang::PHP && !PhpMisread.load(std::memory_order_relaxed)) {
				bool EndsOpen = false;
				ForEachNamedChild(
					Node,
					[&](const TSNode Child) -> bool {
						// 分岐種別の判定
						const bool IsBranch = NodeKind::PhpIfBranch.Contains(Child);
						if(IsBranch && EndsOpen) {
							PhpMisread.store(true, std::memory_order_relaxed);
							// 開いた if で終わる本体の後に枝が続く誤読の確定で打ち切る事の返戻
							return false;
						}
						// 次の開いた if 状態の更新
						const TSNode Body = IsBranch ? TSSource::FieldChild(Child, "body") : ts_node_eq(Child, Consequence) ? Child : TSNode{};
						EndsOpen = !ts_node_is_null(Body) && std::string_view(ts_node_type(Body)) != "colon_block" && Src[Src.Start(Body)] != '{' &&
						EndsWithOpenIf(Body, false);
						// 次の子へ進む事の返戻
						return true;
					}
				);
			}
			CollectBody(Consequence, false, TSNode{}, Node, Sink);
			// else 節を持たない文法（Java / C# 等）は枝の本体を直に持つ（else 節を持つ文法では節の側で扱う）
			CollectBody(TSSource::FieldChild(Node, "alternative"), true, TSNode{}, Node, Sink);
		} else if(NodeKind::SingleIndented.Contains(NodeType) && !NodeKind::BraceRemoveStmtExcluded.Contains(NodeType)) {
			// 言語別の繰返本体の取得（Swift の repeat は波括弧必須の為に除外）
			TSNode Body = TSSource::FieldChild(Node, "body");
			if(ts_node_is_null(Body)) Body = FirstNamedChildOfType(Node, "control_structure_body");
			// 宣言の有効範囲と巻上げを保つラベル付宣言の包装除外
			if(NodeKind::LabeledStatement.Contains(NodeType) && !ts_node_is_null(Body) && NodeKind::BodyDeclaration.Contains(Body)) return;
			CollectBody(Body, false, TSNode{}, Node, Sink);
		}
	};
	const auto CollectPhpRebinds = [&Src](std::vector<Candidate> &Sink) -> void {
		// 節点毎の未閉鎖 if 状態の走査枠
		struct Frame {
			int EndsOpen = 0; // 末尾の子の連なりの後に開いた儘の if の数（節自身の if を除く）
			uint32_t OpenStart = 0; // 其の数を正にした本体の開始位置
			uint32_t MisreadStart = 0; // 開いた if を残し後に枝が続く本体の開始位置
			bool IsMisread = false; // 其の本体の有無
			bool IsColon = false; // 代替構文の本体を持つか
		};
		// 構文木走査状態の初期化
		HeldCursor Cursor(Src.GetRoot());
		std::vector<Frame> Stack(1);
		for(bool ShouldDescend = true;;) {
			// 子の集計終了後の親への結果引渡し
			if(ShouldDescend && ts_tree_cursor_goto_first_child(&Cursor)) {
				Stack.emplace_back();
				continue;
			}
			const TSNode Node = ts_tree_cursor_current_node(&Cursor);
			// 巡り終えた節点の開いた if を確定
			const Frame Done = Stack.back();
			Stack.pop_back();
			// 根の走査を終えたら打切
			if(Stack.empty()) break;
			// コメントと PHP 地の文の枝結付からの除外
			if(!ts_node_is_extra(Node)) {
				const std::string_view Type(ts_node_type(Node));
				Frame &Parent = Stack.back();
				int EndsOpen = Done.IsColon ? 0 : Done.EndsOpen;
				if(Type == "if_statement") {
					if(Done.IsMisread) Sink.push_back({ Done.MisreadStart, Src.End(Node), Done.MisreadStart, Src.End(Node), true });
					// 代替構文でない if は自身も開いた儘
					if(!Done.IsColon) ++EndsOpen;
				} else if(Type == "colon_block") {
					EndsOpen = 0;
					Parent.IsColon = true;
				}
				// 後続枝が直前の開いた if を奪う位置を記録
				if(NodeKind::PhpIfBranch.Contains(Type)) {
					if(Parent.EndsOpen > 0 && !Parent.IsMisread) {
						Parent.IsMisread = true;
						Parent.MisreadStart = Parent.OpenStart;
					}
					// 開いた if が無い枝本体の次候補への記録
					if(Parent.EndsOpen < 1) Parent.OpenStart = Done.OpenStart;
					Parent.EndsOpen += EndsOpen;
					// else による開いた if １つの終了
					if(Type == "else_clause") --Parent.EndsOpen;
				} else {
					Parent.EndsOpen = EndsOpen;
					Parent.OpenStart = Src.Start(Node);
				}
			}
			// 兄弟へ進む時だけ新しい集計枠を用意
			if(ts_tree_cursor_goto_next_sibling(&Cursor)) {
				Stack.emplace_back();
				ShouldDescend = true;
			} else {
				ts_tree_cursor_goto_parent(&Cursor);
				ShouldDescend = false;
			}
		}
		// 終了
		return;
	};
	// 候補格納領域の初期化
	std::vector<Candidate> Candidates;
	// 構文木走査で制御構文ノード数の上限を粗く見積もり，再確保を回避
	Candidates.reserve((Src.size() >> 6) + 16);
	// 構文木の並列分割条件の取得
	const TSNode Root = Src.GetRoot();
	// Kotlin の枝本体の代入値利用状態の並列走査前記録
	if(Language == Lang::Kotlin) PrepareKotlinValueContexts(Src);
	const uint32_t RootChildCount = ts_node_child_count(Root);
	const size_t NumThreads = Parallel::DecideThreads(RootChildCount, 8);
	// 根節点自体の候補化
	DispatchNode(Root, Candidates);
	// 小さい木は共有の候補列へ直接収集
	if(NumThreads < 2) {
		ForEachChild(
			Root,
			[&](const TSNode Child) -> void {
				WalkAst(
					Child,
					[&](const TSNode Node) -> void {
						DispatchNode(Node, Candidates);
					}
				);
			}
		);
	} else {
		std::vector<TSNode> RootKids;
		RootKids.reserve(RootChildCount);
		ForEachChild(
			Root,
			[&](const TSNode Child) -> void {
				RootKids.push_back(Child);
			}
		);
		// 並列中の書込先を走脈毎に分離
		std::vector<std::vector<Candidate>> ThreadCands(NumThreads);
		Parallel::ForChunks(
			RootKids.size(),
			NumThreads,
			[&](const size_t Start, const size_t End, const size_t Tid) -> void {
				// 担当範囲の候補収集
				std::vector<Candidate> &Local = ThreadCands[Tid];
				for(size_t Idx = Start; Idx < End; ++Idx) {
					WalkAst(
						RootKids[Idx],
						[&](const TSNode Node) -> void {
							DispatchNode(Node, Local);
						}
					);
				}
			}
		);
		// 全走脈終了後の候補集約
		for(const std::vector<Candidate> &Local : ThreadCands) Candidates.insert(Candidates.end(), Local.begin(), Local.end());
	}
	// 誤読構文木の巡での枝結付だけの修正
	if(PhpMisread) {
		std::vector<Candidate> Rebinds;
		CollectPhpRebinds(Rebinds);
		// 重複する修正範囲の整列
		std::sort(
			Rebinds.begin(),
			Rebinds.end(),
			[](const Candidate &Lhs, const Candidate &Rhs) -> bool {
				return Lhs.BodyStart < Rhs.BodyStart;
			}
		);
		uint32_t CoveredEnd = 0;
		for(const Candidate &Entry : Rebinds) {
			if(Entry.BodyStart < CoveredEnd) continue;
			CoveredEnd = Entry.BodyEnd;
			TextEdit::Push(Entry.BodyStart, Entry.BodyStart, "{\n", Edits);
			TextEdit::Push(Entry.BodyEnd, Entry.BodyEnd, "\n}", Edits);
		}
		// 結付の修正だけを適用しての終了
		return;
	}
	// 付与を含むネストだけを次巡へ送り，互いに干渉しない除去は一括適用
	std::sort(
		Candidates.begin(),
		Candidates.end(),
		[](const Candidate &Lhs, const Candidate &Rhs) -> bool {
			return Lhs.BodyStart < Rhs.BodyStart;
		}
	);
	for(size_t OuterIdx = 0; OuterIdx < Candidates.size(); ++OuterIdx) {
		const Candidate &Outer = Candidates[OuterIdx];
		bool HasInner = false;
		for(
			size_t InnerIdx = OuterIdx + 1;
			InnerIdx < Candidates.size() && Candidates[InnerIdx].BodyStart < Outer.StmtEnd;
			++InnerIdx
		) {
			if(
				(Outer.ShouldAdd || Candidates[InnerIdx].ShouldAdd) && Candidates[InnerIdx].BodyStart > Outer.StmtStart &&
				Candidates[InnerIdx].BodyEnd <= Outer.StmtEnd
			) {
				HasInner = true;
				break;
			}
		}
		// 内側の付与を先に確定してから外側を再判定
		if(HasInner) continue;
		// 本体の前後へ対になる波括弧を挿入
		if(Outer.ShouldAdd) {
			TextEdit::Push(Outer.BodyStart, Outer.BodyStart, "{\n", Edits);
			TextEdit::Push(Outer.BodyEnd, Outer.BodyEnd, "\n}", Edits);
			continue;
		}
		const char StmtLastChar = Outer.StmtEnd ? Src[Outer.StmtEnd - 1] : '\0';
		TextEdit::Push(
			Outer.StmtEnd,
			Outer.BodyEnd,
			Outer.IsBeforeElse ? " " : Language.IsJsTs() && StmtLastChar != ';' && StmtLastChar != '}' ? ";\n" : "\n",
			Edits
		);
		TextEdit::Push(
			Outer.BodyStart,
			Outer.StmtStart,
			Outer.BodyStart && std::isalpha(static_cast<unsigned char>(Src[Outer.BodyStart - 1])) ? " " : "",
			Edits
		);
	}
	// 終了
	return;
}

/**
 * 行幅に依らない波括弧編集の適用関数
 * 計算量：原稿の長さ N と巡の数 R に対し O(N * R)（R は手間の上限で抑え，超過は見送る）
 * @param Src 解析済のソースコード
 * @param Language 対象言語
 * @return 編集後も構文木を保持していれば true（手間の上限の超過と巡りの停止は FormatLimitExceeded を投げる）
 */
bool EditPass::ApplyBraceEdits(TSSource &Src, const Lang Language) {
	// 本文要約による循環検出と総費用の制限（衝突時も原文を返す側へ倒す）
	static constexpr size_t BraceBudget = 0X80000, BracePerByteShift = 5;
	const size_t BraceCap = std::max(BraceBudget, Src.size() << BracePerByteShift);
	size_t BraceWork = 0;
	std::unordered_set<size_t> Seen { std::hash<std::string_view>{}(Src) };
	while(Src.IsParsed()) {
		// 手間上限超過の構文破壊と区別した見送
		if((BraceWork += Src.size()) > BraceCap) throw FormatLimitExceeded("brace edits exceed formatting limit");
		std::vector<TextEdit> Edits;
		// 更新済の構文木から今回の編集だけを収集
		CollectBraceEdits(Src, Language, Edits);
		// 波括弧の編集が無い事の返戻
		if(Edits.empty()) return true;
		TextEdit::Apply(Src, Edits);
		// 以前の本文への復帰時の構文破壊と区別した見送
		if(!Seen.insert(std::hash<std::string_view>{}(Src)).second) throw FormatLimitExceeded("brace edits do not settle", true);
	}
	// 構文解析失敗の返戻
	return false;
}
