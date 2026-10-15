#include "SyntaxNormalizer.hpp"
#include "SyntaxGeneration.hpp"
#include <cstddef>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * `else` の枝を持つ分岐かの判定関数
	 * 計算量：参照する子又は兄弟の数 C に対し O(C)
	 * @param Syntax 構文モデル
	 * @param Node ノード
	 * @return 後続の枝のフィールド又は `else` のトークンを持つ `if` なら `true`
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool SyntaxNormalizer::HasElseBranch(const SyntaxModel &Syntax, const NodeId Node) {
		// 後続の枝の有無の返戻
		return Syntax.Is(Node, Role::If) && (
			Syntax.GetFieldChild(Node, ChildRole::Alternative) != NoNode || Core::SyntaxModel::HasChildMatching(
				Syntax,
				Node,
				[&Syntax](const NodeId Child) -> bool {
					// `else` のトークンかの返戻
					return Syntax.Is(Child, Role::ElseKeyword);
				}
			)
		);
	}

	/**
	 * 後続の枝又は終端の前で改行を要する制御文かの判定関数
	 * 空の波括弧の閉じには後続の語を同行に置けるが，波括弧を省く単一文の後には置けない
	 * 計算量：後続枝の連鎖で読む直接子の総数 C に対し O(C + 1)
	 * @param Syntax 構文モデル
	 * @param Node 制御文
	 * @return 枝又は終端の前に必須の改行が在れば `true`
	 * @throws FormatLimitExceeded 枝の探索が作業予算を超える場合
	 * @throws std::out_of_range 照会する本体又は子がノード表に無い場合
	 */
	bool SyntaxNormalizer::HasClauseLineBreak(const SyntaxModel &Syntax, NodeId Node) {
		// 本体の正規形が空の波括弧で終わるかの判定
		const auto IsEmptyBlock = [&Syntax](NodeId Body) -> bool {
			if(Syntax.Is(Body, Role::BodyWrapper) && !Syntax.IsBracedBody(Body)) Body = Syntax.GetContentChild(Body, 0);
			// 波括弧を省く文の終了には改行を要する事の返戻
			if(!Syntax.IsBracedBody(Body)) return false;
			const size_t Count = Syntax.GetContentChildCount(Body);
			const NodeId Inner = Count == 1 ? Syntax.GetContentChild(Body, 0) : NoNode;
			// 空のブロック又は空の文の並びを包むブロックかの返戻
			return !Count || Syntax.Is(Inner, Role::StmtSeq) && !Syntax.GetContentChildCount(Inner);
		};
		// `while` の前で改行を要する `do` の本体の判定
		if(Syntax.Is(Node, Role::DoLoop)) {
			const NodeId Body = Syntax.GetFieldChild(Node, ChildRole::Body);
			// 本体のフィールドを持たない文法でも，登録済の本体ラッパーを判定する
			return !IsEmptyBlock(Body == NoNode ? Syntax.GetFirstNamedChildWithRole(Node, Role::BodyWrapper) : Body);
		}
		// 最初の枝と，後続の `if` 又は独立した中間枝の終了の判定
		while(HasElseBranch(Syntax, Node)) {
			// 後続枝の前に必須の改行が在る事の返戻
			if(!IsEmptyBlock(Syntax.GetIfConsequence(Node))) return true;
			for(const NodeId Child : Syntax.GetChildrenOf(Node)) {
				// 中間枝の後に必須の改行が在る事の返戻
				if(Syntax.Is(Child, Role::ElseIfClause) && !IsEmptyBlock(Syntax.GetFieldChild(Child, ChildRole::Body))) return true;
			}
			Node = Syntax.GetFieldChild(Node, ChildRole::Alternative);
			if(Syntax.Is(Node, Role::ElseClause) || Syntax.Is(Node, Role::BodyWrapper)) Node = Syntax.GetContentChild(Node, 0);
		}
		// 各枝の閉じに後続の語を続けられる事の返戻
		return false;
	}

	/**
	 * 後に `else` が続く本体かの判定関数
	 * 計算量：O(1)
	 * @param Syntax 構文モデル
	 * @param Body 本体
	 * @return 次の兄弟が `else` のトークン又は後続の枝の節なら `true`
	 */
	bool SyntaxNormalizer::IsFollowedByElse(const SyntaxModel &Syntax, const NodeId Body) {
		// 後に `else` が続く本体かの判定
		const NodeId Next = Syntax.GetNextSibling(Body);
		// 後続の枝かの返戻
		return Syntax.Is(Next, Role::ElseKeyword) || Syntax.Is(Next, Role::ElseClause) || Syntax.Is(Next, Role::ElseIfClause);
	}

	/**
	 * 後続の枝を受け得る開いた `if` で終わるかの判定関数
	 * 計算量：末尾の連鎖で参照するノードと子の総数 N に対し期待値 O(N + 1)
	 * @param Syntax 構文モデル
	 * @param Node 判定する文
	 * @param IsThroughBraces 単一の文のブロックの中へも辿るか
	 * @param BraceForms 判定済の本体毎の正規形で波括弧を持つか
	 * @return 正規形の波括弧で遮られず，末尾から `else` を持たない `if` に達すれば `true`
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::out_of_range 照会するフィールドの子がノード表に無い場合
	 */
	bool SyntaxNormalizer::HasOpenIfSuffix(
		const SyntaxModel &Syntax,
		NodeId Node,
		const bool IsThroughBraces,
		const std::unordered_map<NodeId, bool> &BraceForms
	) {
		// 後続の枝を受け得る開いた `if` で終わるかの判定
		while(true) {
			const std::unordered_map<NodeId, bool>::const_iterator Form = BraceForms.find(Node);
			// 正規形の波括弧で閉じる末尾の返戻
			if(Form != BraceForms.end() && Form->second) return false;
			// 開いた `if` で終わる事の返戻
			if(Syntax.Is(Node, Role::If) && !HasElseBranch(Syntax, Node)) return true;
			const NodeId Last = Syntax.GetLastContentChild(Node);
			const bool IsTail = Last != NoNode && Syntax.GetChild(Node, Syntax.GetChildCount(Node) - 1) == Last;
			// 単一の文のブロックと其の中の文の並び
			// 末尾を辿れない事の返戻
			if(
				const bool IsSingleBlock = IsThroughBraces && Syntax.GetContentChildCount(Node) == 1 &&
				(Syntax.Is(Node, Role::StmtSeq) || Syntax.IsBracedBody(Node) && Form != BraceForms.end() && !Form->second);
				!IsTail && !IsSingleBlock
			) return false;
			Node = Last;
		}
	}

	/**
	 * 制御本体の波括弧の付与・除去の候補の収集関数（整形仕様 5.5）
	 * 宣言の有効範囲・`do-while` の終端・`else` の結付を保つ必要が在る本体へ波括弧を付ける
	 * 省略可能な単一文のブロックでは，値の利用位置等の条件を満たす場合だけ波括弧を除く
	 * 計算量：本体・末尾・値位置の総探索量 Q と候補の生成量 G に対し期待値 O(Q + G + 1)
	 * @param Scan 走査の状態
	 * @param Body 本体
	 * @param ValHost 本体の値を結果にし得る枝を持つ式（無ければ `NoNode`）
	 * @param Owner 本体を持つ制御構文
	 * @param IsElseBody 後続の枝又は値を持つ枝の本体か
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectBodyBraces(
		BraceScan &Scan,
		const NodeId Body,
		const NodeId ValHost,
		const NodeId Owner,
		const bool IsElseBody
	) const {
		// 制御本体の波括弧の付与・除去の候補の収集
		const SyntaxModel &Syntax = Scan.Syntax;
		const CanonicalForm *const Form = Syntax.GetFormOf(Owner, FormOperation::BraceBody);
		// 本体又は生成形式が無い構文と，波括弧の変更が許されない本体の終了
		if(
			Body == NoNode || !Form || Syntax.GetNode(Owner).Facts.Has(ConstraintKind::DetachedElse) ||
			Syntax.Is(Body, Role::DelimitedBody) || Syntax.Is(Body, Role::ElseIfClause) || Syntax.Is(Body, Role::ElseClause)
		) return;
		const bool IsBraced = Syntax.IsBracedBody(Body), IsFollowedByBranch = IsFollowedByElse(Syntax, Body);
		// ラベルを除いた本体の文（波括弧の無い本体ラッパーでは其の中の文）
		NodeId Inner = !IsBraced && Syntax.Is(Body, Role::BodyWrapper) ? Syntax.GetContentChild(Body, 0) : Body;
		while(Syntax.Is(Inner, Role::Labeled) && Syntax.GetContentChildCount(Inner) > 1) Inner = Syntax.GetLastContentChild(Inner);
		if(
			!IsBraced && (
				Syntax.Is(Body, Role::Decl) || Syntax.GetNode(Inner).Facts.Has(ConstraintKind::StmtMacro) ||
				(!IsElseBody || Syntax.Is(Inner, Role::DoLoop)) && HasClauseLineBreak(Syntax, Inner) ||
				IsFollowedByBranch && HasOpenIfSuffix(Syntax, Inner, true, Scan.Forms)
			)
		) {
			// 宣言の範囲・マクロの文境界・`else` の結付に波括弧を要する本体の付与候補の追加
			Scan.Candidates.push_back({ Body, true, {}, Form });
			// 波括弧の付与候補を追加した為の終了
			return;
		}
		// 波括弧の無い本体と単一の文でない本体の場合の終了
		if(!IsBraced || Syntax.GetContentChildCount(Body) != 1) return;
		// 単一の文を包むネストのブロックと其の文（ブロックの中の文の並びは其の唯一の文）
		std::vector<NodeId> Layers { Body };
		NodeId Stmt = Syntax.GetContentChild(Body, 0);
		if(Syntax.Is(Stmt, Role::StmtSeq)) {
			// 文の並びが単一の文でない場合の終了
			if(Syntax.GetContentChildCount(Stmt) != 1) return;
			Stmt = Syntax.GetContentChild(Stmt, 0);
		}
		while(Syntax.IsBracedBody(Stmt) && Syntax.GetContentChildCount(Stmt) == 1) {
			Layers.push_back(Stmt);
			Stmt = Syntax.GetContentChild(Stmt, 0);
		}
		// ラベル・アノテーションの後の文で判定する（付与と揃えないと付与と除去が交互に起きる）
		NodeId Head = Stmt;
		while(
			(
				Syntax.Is(Head, Role::Labeled) ||
				Syntax.Is(Head, Role::PrefixOperation) && Syntax.Is(Syntax.GetContentChild(Head, 0), Role::Label)
			) && Syntax.GetContentChildCount(Head) > 1
		) Head = Syntax.GetLastContentChild(Head);
		// `using` の宣言の文か
		NodeId Using = Syntax.GetContentChild(Stmt, 0);
		if(Syntax.Is(Using, Role::Await)) Using = Syntax.GetContentChild(Using, 0);
		// 宣言・値の位置の文と，枝の結付や文の終端が変わる本体の波括弧を保つ事の終了
		if(
			const bool IsUsing = Syntax.Is(Stmt, Role::ExprStmt) && Syntax.Is(Using, Role::Assignment) &&
			Syntax.Is(Syntax.GetChild(Using, 0), Role::UsingKeyword);
			Syntax.Is(Head, Role::Preproc) || Syntax.Is(Head, Role::Decl) || Syntax.Is(Head, Role::InlineText) || IsUsing ||
			// ブロックでない波括弧で始まる文は，波括弧の省略で読みが変わる為に保持
			// Kotlin の `do` の本体の唯一の繰返は，後の `while` が別の文の頭に読まれない様に保持
			Syntax.GetNode(Head).Facts.Has(ConstraintKind::StmtMacro) ||
			(!IsElseBody || Syntax.Is(Head, Role::DoLoop)) && HasClauseLineBreak(Syntax, Head) ||
			!Syntax.IsBracedBody(Head) && Syntax.IsTokenRole(Syntax.GetFirstEdge(Head).Leaf, Role::OpeningBraceToken) ||
			Syntax.Is(Owner, Role::NestedLoopBodyBraces) && Syntax.Is(Head, Role::Loop) ||
			IsFollowedByBranch && HasOpenIfSuffix(Syntax, Head, true, Scan.Forms) ||
			ValHost != NoNode && Syntax.Is(Head, Role::StmtOnly) && Syntax.IsBranchValUsed(ValHost)
		) return;
		// 単一の文のブロックの除去の候補の追加
		Scan.Candidates.push_back({ Body, false, std::move(Layers), Form });
		// 終了
		return;
	}

	/**
	 * 枝の値を結果にする式の本体の波括弧の候補の収集関数（整形仕様 5.5）
	 * `when` の各枝の本体と `if` 式の本体を扱う
	 * 後続の演算が最後の枝の値に結び付かない様に，演算が続く `if` 式では其の枝の波括弧を保つ
	 * 計算量：枝と其の本体・末尾・値位置の総探索量 Q と候補の生成量 G に対し期待値 O(Q + G + 1)
	 * @param Scan 走査の状態
	 * @param Node `if` 式又は `when` 式
	 * @param Parent 式の親
	 * @param Next 式の次の兄弟（無ければ `NoNode`）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectBranchBraces(BraceScan &Scan, const NodeId Node, const NodeId Parent, const NodeId Next) const {
		// 枝の値を結果にする式の本体の波括弧の候補の収集
		const SyntaxModel &Syntax = Scan.Syntax;
		// `if` の最初の本体・其の直前のトークン・後続の枝の本体と，`when` の各枝の本体
		NodeId First = NoNode, Prev = NoNode, Following = NoNode;
		for(const NodeId Child : Syntax.GetChildrenOf(Node)) {
			if(Syntax.Is(Child, Role::CaseArm)) {
				for(const NodeId Part : Syntax.GetChildrenOf(Child)) if(Syntax.Is(Part, Role::BodyWrapper)) {
					CollectBodyBraces(Scan, Part, Node, Node, true);
				}
			} else if(First == NoNode) {
				if(Syntax.Is(Child, Role::BodyWrapper)) First = Child;
				else Prev = Child;
			} else if(Following == NoNode && Syntax.IsNamed(Child)) Following = Child;
		}
		// `if` 式でない `when` 式の終了
		if(First == NoNode) return;
		// 真の枝の本体を持たない `if(a) else b` か
		const bool IsEmptyThen = Syntax.Is(Prev, Role::ElseKeyword);
		// 式の後へ演算が続くか
		const bool IsContinued = Next != NoNode && !Syntax.Is(Parent, Role::StmtHost) && !Syntax.Is(Next, Role::ExprTerminator);
		if(!IsContinued || HasElseBranch(Syntax, Node)) CollectBodyBraces(Scan, IsEmptyThen ? NoNode : First, Node, Node, false);
		// 最後の枝の波括弧が後続の演算を式全体へ結び付ける時は保つ
		if(!IsContinued) CollectBodyBraces(Scan, IsEmptyThen ? First : Following, Node, Node, true);
		// 終了
		return;
	}

	/**
	 * １所有者の波括弧の意味候補の収集関数
	 * 計算量：対象の子・本体・末尾の連鎖と，値位置の祖先の総探索量 Q に対し期待値 O(Q + 1)
	 * @param Scan 現在の構文と値位置の判定
	 * @param Node 本体又は枝を所有するノード
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectBraceCandidates(BraceScan &Scan, const NodeId Node) const {
		// １所有者の波括弧の意味候補の収集
		const SyntaxModel &Syntax = Scan.Syntax;
		// エラーを含む構文の本体は既知の正常な構文へ組み直さない（整形仕様 4.6）
		const auto IsSound = [&Syntax](const NodeId Control) -> bool {
			// エラーを含まない構文かの返戻
			return !Syntax.GetNode(Control).Facts.Has(ConstraintKind::ContainsError);
		};
		// 子の枝の値を結果にする式への親と次の兄弟の受渡
		const std::span<const NodeId> Children = Syntax.GetChildrenOf(Node);
		for(size_t Idx = 0; Idx < Children.size(); ++Idx) {
			if(!Syntax.Is(Children[Idx], Role::ValBranching) || !IsSound(Children[Idx])) continue;
			CollectBranchBraces(Scan, Children[Idx], Node, Idx + 1 < Children.size() ? Children[Idx + 1] : NoNode);
		}
		// 親の側で扱った枝の値を結果にする式とエラーを含む構文の読飛
		// 親が扱う枝又はエラーを含む構文の場合の終了
		if(Syntax.Is(Node, Role::ValBranching) || !IsSound(Node)) return;
		if(Syntax.Is(Node, Role::ElseClause)) CollectBodyBraces(Scan, Syntax.GetContentChild(Node, 0), NoNode, Node, true);
		else if(Syntax.Is(Node, Role::If)) {
			CollectBodyBraces(Scan, Syntax.GetIfConsequence(Node), NoNode, Node, false);
			CollectBodyBraces(Scan, Syntax.GetFieldChild(Node, ChildRole::Alternative), NoNode, Node, true);
		} else if(Syntax.Is(Node, Role::BodyOwner)) {
			// 本体のフィールド又は本体ラッパーの選択
			NodeId Body = Syntax.GetFieldChild(Node, ChildRole::Body);
			for(const NodeId Child : Children) if(Body == NoNode && Syntax.Is(Child, Role::BodyWrapper)) Body = Child;
			// 宣言の有効範囲と巻上げを保つラベル付の宣言のラッパーの除外
			if(!Syntax.Is(Node, Role::Labeled) || !Syntax.Is(Body, Role::Decl)) CollectBodyBraces(Scan, Body, NoNode, Node, false);
		}
		// 終了
		return;
	}

	/**
	 * 全所有者の波括弧の正規化の一括収集関数（整形仕様 5.5）
	 * 計算量：候補整列の費用 S，判定の総探索量 Q，生成・移譲の総処理量 G に対し期待値 O(S + Q + G)
	 * @param Syntax 現在の構文
	 * @param Plan 全所有者の書換計画の格納先
	 * @throws FormatLimitExceeded 作業予算を超えた場合，又は必要な波括弧の生成形式・部品が不正な場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectBraceBatch(const SyntaxModel &Syntax, RewritePlan &Plan) const {
		// 全所有者の波括弧の正規化の一括収集
		std::vector<NodeId> Owners;
		for(const Role Wanted : { Role::BodyOwner, Role::ElseClause, Role::If }) {
			const std::span<const NodeId> Nodes = Syntax.GetNodesWith(Wanted);
			Syntax.Charge(Nodes.size());
			Owners.insert(Owners.end(), Nodes.begin(), Nodes.end());
		}
		for(const NodeId Branch : Syntax.GetNodesWith(Role::ValBranching)) {
			Syntax.Charge(1);
			Owners.push_back(Syntax.GetParentOf(Branch));
		}
		Owners = SortInDocOrder(Syntax, Owners);
		BraceScan Scan { Syntax };
		for(std::vector<NodeId>::const_reverse_iterator Owner = Owners.rbegin(); Owner != Owners.rend(); ++Owner) {
			if(HasErrorNormalizationAncestor(Syntax, *Owner)) continue;
			const size_t First = Scan.Candidates.size();
			CollectBraceCandidates(Scan, *Owner);
			for(size_t Idx = First; Idx < Scan.Candidates.size(); ++Idx) {
				const BraceCandidate &Found = Scan.Candidates[Idx];
				Scan.Forms.insert_or_assign(Found.Body, Found.IsAdd);
				for(const NodeId Layer : Found.Layers) Scan.Forms.insert_or_assign(Layer, false);
			}
		}
		std::unordered_set<NodeId> Removed;
		for(const BraceCandidate &Found : Scan.Candidates) {
			if(Found.IsAdd) Plan.Steps.push_back(Generation::BraceBodyStep(Syntax, Found.Body, *Found.Form));
			else if(Syntax.Is(Found.Body, Role::BodyWrapper)) {
				// 本体ラッパーを残す形式での波括弧の除去（アンカーは中の文の前後へ移す）
				// 文の並びが別のラッパーに入る場合は，其のラッパーも除去
				const NodeId Stmts = Syntax.GetContentChild(Found.Body, 0);
				const NodeId Stmt = Syntax.Is(Stmts, Role::StmtSeq) ? Syntax.GetContentChild(Stmts, 0) : Stmts;
				const AnchorTarget Before { Stmt, false }, After { Stmt, true };
				for(const NodeId Token : Syntax.GetChildrenOf(Found.Body)) {
					if(Token == Stmts) continue;
					const AnchorTarget &Target = Syntax.Is(Token, Role::OpenBrace) ? Before : After;
					Plan.Steps.push_back({ RewriteKind::Remove, Token, {}, { Token, Target, Target, Target } });
				}
				if(Stmts != Stmt) Plan.Steps.push_back({ RewriteKind::Unwrap, Stmts });
			} else for(const NodeId Layer : Found.Layers) if(Removed.insert(Layer).second) {
				Plan.Steps.push_back({ RewriteKind::Unwrap, Layer });
			}
		}
		// 終了
		return;
	}
} // namespace Core
