#include "SyntaxNormalizer.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 本体を波括弧で包む生成形式の書換関数
	 * 正規化と出力検証が同じ文法定義から，選択した本体の構造を作る
	 * @param Syntax 書換前の構文モデル
	 * @param Body 包む本体
	 * @param Form 所属する制御構文の生成形式
	 * @return 波括弧を付ける書換操作
	 * @throws FormatLimitExceeded 生成形式の部品が足りない場合
	 */
	RewriteStep SyntaxNormalizer::BraceBodyStep(const SyntaxModel &Syntax, const NodeId Body, const CanonicalForm &Form) {
		// 包装の可否と必要な生成部品の確認
		if(!Form.CanWrapBody(Syntax.IsBracedBody(Body))) {
			throw FormatLimitExceeded("the brace form cannot wrap this body", FailureReason::FormatterDefect);
		}
		const bool IsContainer = Syntax.Is(Body, Role::BodyWrapper);
		if(!Form.PartOf(FormPartRole::Body)) {
			throw FormatLimitExceeded("the brace form lacks its body role", FailureReason::FormatterDefect);
		}
		const auto Part = [&Form](const FormPartRole Role, std::vector<GeneratedNode> &&Children) -> GeneratedNode {
			const FormPart *const Found = Form.PartOf(Role);
			if(!Found || !Found->Definition) {
				throw FormatLimitExceeded("the brace form lacks a required part", FailureReason::FormatterDefect);
			}
			// 部品の種別の生成節点の返戻
			return { {}, Found->Definition->IsNamed, std::move(Children), 0, NoNode, std::string(Found->FixedLexeme), Found->Definition };
		};
		// 本体の包みの再構成と単一の文の包装
		if(IsContainer) {
			std::vector<GeneratedNode> Statements;
			for(const NodeId Child : Syntax.ChildrenOf(Body)) Statements.push_back({ {}, false, {}, Syntax.GetNode(Child).Fields, Child });
			RewriteStep Step { RewriteKind::Reshape, Body };
			Step.Inserted = Part(
				FormPartRole::Block,
				{ Part(FormPartRole::Open, {}), Part(FormPartRole::Statements, std::move(Statements)), Part(FormPartRole::Close, {}) }
			);
			// 本体の包みを組み直す操作の返戻
			return Step;
		}
		RewriteStep Step { RewriteKind::Wrap, Body };
		Step.Inserted =
		Part(FormPartRole::Block, { Part(FormPartRole::Open, {}), { {}, false, {}, {}, Body }, Part(FormPartRole::Close, {}) });
		Step.Boundary = BoundaryKind::Spaced;
		// 本体の文を新しい塊で包む操作の返戻
		return Step;
	}

	/**
	 * else の枝を持つ分岐かの判定関数
	 * @param Syntax 構文モデル
	 * @param Node 節点
	 * @return 後続の枝の場か else の字句を持つ if なら true
	 */
	bool SyntaxNormalizer::IfWithElse(const SyntaxModel &Syntax, const NodeId Node) {
		// 後続の枝の有無の返戻
		return Syntax.Is(Node, Role::If) && (
			Syntax.FieldChild(Node, ChildRole::Alternative) != NoNode || AnyChildOf(
				Syntax,
				Node,
				[&Syntax](const NodeId Child) -> bool {
					// else の字句かの返戻
					return Syntax.Is(Child, Role::ElseKeyword);
				}
			)
		);
	}

	/**
	 * 後続の枝を受け得る開いた if で終わるかの判定関数
	 * @param Syntax 構文モデル
	 * @param Node 判定する文
	 * @param IsThroughBraces 単一の文の塊の中へも辿るか
	 * @return 末尾の子を辿って else を持たない if に達すれば true
	 */
	bool SyntaxNormalizer::EndsWithOpenIf(const SyntaxModel &Syntax, NodeId Node, const bool IsThroughBraces) {
		while(true) {
			// 開いた if で終わる事の返戻
			if(Syntax.Is(Node, Role::If) && !IfWithElse(Syntax, Node)) return true;
			const NodeId Last = Syntax.LastNamedChild(Node);
			const bool IsTail = Last != NoNode && Syntax.Child(Node, Syntax.ChildCount(Node) - 1) == Last;
			// 単一の文の塊と其の中の文の並び
			const bool IsSingleBlock =
			IsThroughBraces && Syntax.NamedChildCount(Node) == 1 && (Syntax.IsBracedBody(Node) || Syntax.Is(Node, Role::StatementSequence));
			// 末尾を辿れない事の返戻
			if(!IsTail && !IsSingleBlock) return false;
			Node = Last;
		}
	}

	/**
	 * 後に else が続く本体かの判定関数
	 * @param Syntax 構文モデル
	 * @param Body 本体
	 * @return 次の兄弟が else の字句か後続の枝の節なら true
	 */
	bool SyntaxNormalizer::IsFollowedByElse(const SyntaxModel &Syntax, const NodeId Body) {
		const NodeId Next = Syntax.NextSibling(Body);
		// 後続の枝かの返戻
		return Syntax.Is(Next, Role::ElseKeyword) || Syntax.Is(Next, Role::ElseClause) || Syntax.Is(Next, Role::ElseIfClause);
	}

	/**
	 * 値が使われる枝を持つ式と文の並びの文の収集関数
	 * 文の並びの最後の文は，ラムダの本体と，値として使う if / when / try の枝では値が結果に為る
	 * 役割索引の枝と文の並びから候補を集め，祖先を先に判定する
	 * @param Syntax 構文モデル
	 * @return 値が使われ得る節点
	 */
	std::unordered_set<NodeId> SyntaxNormalizer::UsedBranchValues(const SyntaxModel &Syntax) const {
		std::unordered_set<NodeId> Used;
		// 値が使われるか（祖先は先行順で先に判定済）
		const auto IsUsed = [&](const NodeId Node) -> bool {
			NodeId Holder = Syntax.ParentOf(Node);
			if(Syntax.Is(Holder, Role::StatementSequence)) {
				const NodeId Parent = Syntax.LastNamedChild(Holder) == Node ? Syntax.ParentOf(Holder) : NoNode;
				// 最後の文でない文と，最上位の文の並びの文の値は使われない事の返戻
				if(Parent == NoNode) return false;
				Holder = Parent;
			// 代入の右辺・実引数等の値の位置に在る事の返戻
			} else if(!Syntax.Is(Holder, Role::BodyWrapper)) return true;
			// ラムダの本体は最後の文の値を結果にする事の返戻
			if(Syntax.Is(Holder, Role::LastValueLambda)) return true;
			NodeId Branch = Holder;
			if(Syntax.Is(Holder, Role::BranchBody)) {
				Branch = Syntax.ParentOf(Holder);
				// 根の直下の本体は値を持たない事の返戻
				if(Branch == NoNode) return false;
				if(Syntax.Is(Branch, Role::CaseArm)) Branch = Syntax.ParentOf(Branch);
				// 繰返の本体等は値を持たない事の返戻
				if(Syntax.Is(Holder, Role::BodyWrapper) && !Syntax.Is(Branch, Role::ValueBranching)) return false;
			// 関数の本体・finally 等は値を持たない事の返戻
			} else if(!Syntax.Is(Holder, Role::ValueTry)) return false;
			// 枝を持つ式の値が使われるかの返戻
			return Used.contains(Branch);
		};
		std::vector<NodeId> Targets = Candidates(Syntax, { Role::ValueBranching, Role::ValueTry }, {});
		// 文の並びから値の候補を加えて構文順へ整列
		for(const NodeId Sequence : Candidates(Syntax, { Role::StatementSequence }, {})) {
			for(const NodeId Child : Syntax.ChildrenOf(Sequence)) Targets.push_back(Child);
		}
		Targets = InDocumentOrder(Syntax, Targets, {});
		for(const NodeId Node : Targets) if(Node != Syntax.GetRoot() && Syntax.IsNamed(Node) && IsUsed(Node)) Used.insert(Node);
		// 値が使われる節点の返戻
		return Used;
	}

	/**
	 * 値が使われるかの判定関数
	 * @param Node 枝を持つ式又は文の並びの文
	 * @return 値が結果に為る位置に在れば true
	 */
	bool SyntaxNormalizer::ValueUse::Contains(const NodeId Node) {
		if(!Used) Used = Normalizer.UsedBranchValues(Syntax);
		// 集めた節点に在るかの返戻
		return Used->contains(Node);
	}

	/**
	 * 制御本体の波括弧の付与・除去の候補の収集関数（整形仕様 5.5）
	 * 宣言・do-while・else を持つ if 等の結付や範囲に波括弧が要る本体へ付け，其れ以外の単一の文の塊の波括弧を除く
	 * @param Scan 走査の状態
	 * @param Body 本体
	 * @param IsElseBody else の枝の本体か
	 * @param ValueHost 本体の値を結果にし得る枝を持つ式（無ければ無し）
	 * @param Owner 本体を持つ制御構文
	 */
	void SyntaxNormalizer::CollectBodyBraces(
		BraceScan &Scan,
		const NodeId Body,
		const bool IsElseBody,
		const NodeId ValueHost,
		const NodeId Owner
	) const {
		const SyntaxModel &Syntax = Scan.Syntax;
		const CanonicalForm *const Form = Syntax.FormOf(Owner, FormOperation::BraceBody);
		// 本体の無い構文と，本体の波括弧を省略出来ない構文と，波括弧の増減で解析器が繋げなかった else の結付が変わる構文の終了
		if(
			Body == NoNode || !Form || Syntax.GetNode(Owner).Facts.Has(ConstraintKind::DetachedElse) ||
			Syntax.Is(Body, Role::DelimitedBody) || Syntax.Is(Body, Role::ElseIfClause) || Syntax.Is(Body, Role::ElseClause)
		) return;
		const bool IsBraced = Syntax.IsBracedBody(Body), IsFollowedByBranch = IsFollowedByElse(Syntax, Body);
		// ラベルを除いた本体の文（波括弧の無い本体の包みは其の中の文）
		NodeId Inner = !IsBraced && Syntax.Is(Body, Role::BodyWrapper) ? Syntax.NamedChild(Body, 0) : Body;
		while(Syntax.Is(Inner, Role::Labeled) && Syntax.NamedChildCount(Inner) > 1) Inner = Syntax.LastNamedChild(Inner);
		if(
			!IsBraced && (
				Syntax.Is(Body, Role::Declaration) || Syntax.Is(Inner, Role::DoLoop) ||
				Syntax.GetNode(Inner).Facts.Has(ConstraintKind::StatementMacro) ||
				!IsElseBody && (IfWithElse(Syntax, Inner) || IsFollowedByBranch && EndsWithOpenIf(Syntax, Inner, true))
			)
		) {
			// 宣言の範囲・do-while の終わり・else の結付に波括弧の要る本体の付与の候補の追加
			Scan.Candidates.push_back({ Body, true, {}, Form });
			// 終了
			return;
		}
		// 波括弧の無い本体と単一の文でない本体の場合の終了
		if(!IsBraced || Syntax.NamedChildCount(Body) != 1) return;
		// 単一の文を包む入れ子の塊と其の文（塊の中の文の並びは其の唯一の文）
		std::vector<NodeId> Layers { Body };
		NodeId Statement = Syntax.NamedChild(Body, 0);
		if(Syntax.Is(Statement, Role::StatementSequence)) {
			// 文の並びが単一の文でない場合の終了
			if(Syntax.NamedChildCount(Statement) != 1) return;
			Statement = Syntax.NamedChild(Statement, 0);
		}
		while(Syntax.IsBracedBody(Statement) && Syntax.NamedChildCount(Statement) == 1) {
			Layers.push_back(Statement);
			Statement = Syntax.NamedChild(Statement, 0);
		}
		// ラベル・注釈の後の文で判定する（付与と揃えないと付与と除去が交互に起きる）
		NodeId Head = Statement;
		while(
			Syntax.NamedChildCount(Head) > 1 &&
			(Syntax.Is(Head, Role::Labeled) || Syntax.Is(Head, Role::PrefixOperation) && Syntax.Is(Syntax.NamedChild(Head, 0), Role::Label))
		) Head = Syntax.LastNamedChild(Head);
		// using の宣言の文か
		NodeId Using = Syntax.NamedChild(Statement, 0);
		if(Syntax.Is(Using, Role::Await)) Using = Syntax.NamedChild(Using, 0);
		const bool IsUsing = Syntax.Is(Statement, Role::ExpressionStatement) && Syntax.Is(Using, Role::Assignment) &&
		Syntax.Is(Syntax.Child(Using, 0), Role::UsingKeyword);
		// 宣言・値の位置の文と，枝の結付や文の終端が変わる本体の波括弧を保つ事の終了
		if(
			Syntax.Is(Head, Role::Preprocessor) || Syntax.Is(Head, Role::Declaration) || Syntax.Is(Head, Role::InlineText) || IsUsing ||
			// 塊でない波括弧で始まる文（ラベル・注釈付のラムダ）と，改行が文を終える Kotlin の do の本体の唯一の繰返（外すと後の while が別の文の頭に読める）
			Syntax.GetNode(Head).Facts.Has(ConstraintKind::StatementMacro) || Syntax.Is(Head, Role::DoLoop) ||
			!Syntax.IsBracedBody(Head) && Syntax.IsTokenRole(FirstLeaf(Syntax, Head), Role::OpeningBraceToken) ||
			Syntax.Is(Owner, Role::NestedLoopBodyBraces) && Syntax.Is(Head, Role::Loop) || !IsElseBody && IfWithElse(Syntax, Head) ||
			IsFollowedByBranch && EndsWithOpenIf(Syntax, Head, true) ||
			ValueHost != NoNode && Syntax.Is(Head, Role::StatementOnly) && Scan.Values.Contains(ValueHost) ||
			Syntax.IsTokenRole(FirstLeaf(Syntax, Statement), Role::OpeningParenToken)
		) return;
		// 単一の文の塊の除去の候補の追加
		Scan.Candidates.push_back({ Body, false, std::move(Layers), Form, IsFollowedByBranch });
		// 終了
		return;
	}

	/**
	 * 枝の値を結果にする式の本体の波括弧の候補の収集関数（整形仕様 5.5）
	 * when の各枝の本体と if 式の本体を扱い，最後の枝の後へ演算が続く if 式は其の枝の波括弧を保つ
	 * @param Scan 走査の状態
	 * @param Node if 式又は when 式
	 * @param Parent 式の親
	 * @param Next 式の次の兄弟（無ければ無し）
	 */
	void SyntaxNormalizer::CollectBranchBraces(BraceScan &Scan, const NodeId Node, const NodeId Parent, const NodeId Next) const {
		const SyntaxModel &Syntax = Scan.Syntax;
		// 最初の本体と其の直前の字句，後続の枝の本体，when の各枝の本体
		NodeId First = NoNode, Previous = NoNode, Following = NoNode;
		for(const NodeId Child : Syntax.ChildrenOf(Node)) {
			if(Syntax.Is(Child, Role::CaseArm)) {
				for(const NodeId Part : Syntax.ChildrenOf(Child)) {
					if(Syntax.Is(Part, Role::BodyWrapper)) CollectBodyBraces(Scan, Part, true, Node, Node);
				}
			} else if(First == NoNode) {
				if(Syntax.Is(Child, Role::BodyWrapper)) First = Child;
				else Previous = Child;
			} else if(Following == NoNode && Syntax.IsNamed(Child)) Following = Child;
		}
		// if 式でない when 式の終了
		if(First == NoNode) return;
		// 真の枝の本体を持たない `if(a) else b` か
		const bool IsEmptyThen = Syntax.Is(Previous, Role::ElseKeyword);
		// 式の後へ演算が続くか
		const bool IsContinued =
		Next != NoNode && !Syntax.Is(Parent, Role::StatementHost) && !Syntax.Is(Next, Role::ExpressionTerminator);
		if(!IsContinued || IfWithElse(Syntax, Node)) CollectBodyBraces(Scan, IsEmptyThen ? NoNode : First, false, Node, Node);
		// 最後の枝の波括弧が後続の演算を式全体へ結び付ける時は保つ
		if(!IsContinued) CollectBodyBraces(Scan, IsEmptyThen ? First : Following, true, Node, Node);
		// 終了
		return;
	}

	/**
	 * 制御本体の波括弧の正規化の収集関数（整形仕様 5.5）
	 * 内側の波括弧の増減で判断が変わり得る候補（後続の枝の結付を保つ除去と付与）は，他の候補を内に持てば内側を確定した次の巡で判断する
	 * 役割索引から候補の所有者を集め，入れ子の候補だけ祖先を調べる
	 * @param Syntax 構文モデル
	 * @param Plan 書換の計画の格納先
	 */
	void SyntaxNormalizer::CollectBraceSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const {
		// 本体の所有者と枝の親の候補の整列
		BraceScan Scan { Syntax, { *this, Syntax } };
		std::vector<NodeId> Targets = Candidates(Syntax, { Role::BodyOwner, Role::ElseClause, Role::If }, { Role::Error });
		std::vector<NodeId> Parents = ParentsOfRole(Syntax, Role::ValueBranching, { Role::Error });
		Targets.insert(Targets.end(), Parents.begin(), Parents.end());
		Targets = InDocumentOrder(Syntax, Targets, {});
		// 誤りを含む構文の本体は既知の正常な構文へ組み直さない（整形仕様 4.6）
		const auto IsSound = [&Syntax](const NodeId Control) -> bool {
			// 誤りを含まない構文かの返戻
			return !Syntax.GetNode(Control).Facts.Has(ConstraintKind::ContainsError);
		};
		for(const NodeId Node : Targets) {
			// 子の枝の値を結果にする式へ親と次の兄弟を渡す
			const std::span<const NodeId> Children = Syntax.ChildrenOf(Node);
			for(size_t Index = 0; Index < Children.size(); ++Index) {
				if(!Syntax.Is(Children[Index], Role::ValueBranching) || !IsSound(Children[Index])) continue;
				CollectBranchBraces(Scan, Children[Index], Node, Index + 1 < Children.size() ? Children[Index + 1] : NoNode);
			}
			// 親の側で扱った枝の値を結果にする式と誤りを含む構文の読飛
			if(Syntax.Is(Node, Role::ValueBranching) || !IsSound(Node)) continue;
			if(Syntax.Is(Node, Role::ElseClause)) CollectBodyBraces(Scan, Syntax.NamedChild(Node, 0), true, NoNode, Node);
			else if(Syntax.Is(Node, Role::If)) {
				CollectBodyBraces(Scan, Syntax.IfConsequence(Node), false, NoNode, Node);
				CollectBodyBraces(Scan, Syntax.FieldChild(Node, ChildRole::Alternative), true, NoNode, Node);
			} else if(Syntax.Is(Node, Role::BodyOwner)) {
				// 繰返の本体（場か本体の包み）
				NodeId Body = Syntax.FieldChild(Node, ChildRole::Body);
				for(const NodeId Child : Children) if(Body == NoNode && Syntax.Is(Child, Role::BodyWrapper)) Body = Child;
				// 宣言の有効範囲と巻上げを保つラベル付の宣言の包装の除外
				if(!Syntax.Is(Node, Role::Labeled) || !Syntax.Is(Body, Role::Declaration)) CollectBodyBraces(Scan, Body, false, NoNode, Node);
			}
		}
		// 他の候補を内に持つ候補（各候補から最も近い候補の祖先迄を辿り，辿った祖先の上は先の走査が決めて居る）
		std::unordered_set<NodeId> Bodies, Outer, Visited;
		for(const BraceCandidate &Found : Scan.Candidates) Bodies.insert(Found.Body);
		for(const BraceCandidate &Found : Scan.Candidates) {
			for(NodeId Up = Syntax.ParentOf(Found.Body); Up != NoNode && Visited.insert(Up).second; Up = Syntax.ParentOf(Up)) {
				Syntax.Charge(1);
				if(!Bodies.contains(Up)) continue;
				Outer.insert(Up);
				break;
			}
		}
		for(const BraceCandidate &Found : Scan.Candidates) {
			// 内側の確定を待つ候補の除外
			if(Outer.contains(Found.Body) && Found.DependsOnInner) continue;
			// 波括弧の有無に依らず残る本体の包みか
			const bool IsContainer = Syntax.Is(Found.Body, Role::BodyWrapper);
			if(Found.IsAdd) Plan.Steps.push_back(BraceBodyStep(Syntax, Found.Body, *Found.Form));
			else if(IsContainer) {
				// 本体の包みの波括弧の除去（錨は中の文の前後へ移し，文の並びの包みを外す）
				const NodeId Statements = Syntax.NamedChild(Found.Body, 0);
				const NodeId Statement = Syntax.Is(Statements, Role::StatementSequence) ? Syntax.NamedChild(Statements, 0) : Statements;
				const AnchorTarget Before { Statement, false }, After { Statement, true };
				for(const NodeId Token : Syntax.ChildrenOf(Found.Body)) {
					if(Token == Statements) continue;
					const AnchorTarget &Target = Syntax.Is(Token, Role::OpenBrace) ? Before : After;
					Plan.Steps.push_back({ RewriteKind::Remove, Token, {}, { Token, Target, Target, Target } });
				}
				if(Statements != Statement) Plan.Steps.push_back({ RewriteKind::Unwrap, Statements });
			} else for(const NodeId Layer : Found.Layers) Plan.Steps.push_back({ RewriteKind::Unwrap, Layer });
		}
		// 終了
		return;
	}
} // namespace Core
