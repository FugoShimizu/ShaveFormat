#include "ParseAdapter.hpp"
#include "ParsedTree.hpp"
#include "TextEdit.hpp"
#include "../Core/SyntaxRole.hpp"
#include "../Profile/GrammarProfile.hpp"
#include "../Profile/ReadingRules.hpp"
#include <tree_sitter/api.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

/**
 * コメントと補完トークンを除いた直接の子の取得関数
 * 計算量：直接の子の数 N に対し O(N)（記録済の親子表の照会は平均 O(1)），格納の上限を超えた時点で打ち切る
 * @param Src 解析用の本文
 * @param Node 親のノード
 * @param Children 原文順の構文の子の格納先
 * @return 子の数（格納の上限を超えれば上限＋１）
 */
size_t ParseAdapter::GetCodeChildren(const ParsedTree &Src, const TSNode Node, const std::span<TSNode> Children) {
	// 空ノードの子数の返戻
	if(ts_node_is_null(Node)) return 0;
	size_t Count = 0;
	// 照合に使う子だけを保持し，余分な子の有無で形が違う事を確定する
	const auto Record = [&Children, &Count](const TSNode Child) -> bool {
		if(Count == Children.size()) {
			++Count;
			// 必要な形の上限を超えた事の返戻
			return false;
		}
		Children[Count++] = Child;
		// 後続の子を調べる事の返戻
		return true;
	};
	if(Src.Syntax.HasParents) {
		// 完成した同じ世代の子列は共有し，此の照会の為だけに全木の表を作らない
		for(const ParsedChild &Child : Src.GetChildrenOf(Node)) {
			if(!Src.Is(Child.Node, Core::Role::Comment) && !Child.IsMissing && !Record(Child.Node)) break;
		}
	} else {
		ForEachChild(
			Node,
			[&Src, &Record](const TSNode Child) -> bool {
				// コメントと補完トークンを数えず，格納の上限迄辿る事の返戻
				return Src.Is(Child, Core::Role::Comment) || ts_node_is_missing(Child) || Record(Child);
			}
		);
	}
	// 照合に使う構文の子数の返戻
	return Count;
}

/**
 * 解析用の括弧とトークンの修復を一括で適用する関数
 * 計算量：領域数 G とトークンの編集数 E，編集１回分の費用 Q に対し O(G + E + Q)
 * @param Src 解析用の本文
 * @param Selected 包む部分式の範囲
 * @param Edits トークンの修復と括弧の挿入の格納先
 * @throws FormatLimitExceeded 挿入後の解析が期限を超えた場合
 * @throws std::bad_alloc 編集のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void ParseAdapter::WrapExprs(ParsedTree &Src, const ExprGroups &Selected, std::vector<TextEdit> &Edits) {
	// 原文順の修復領域の取得
	const std::vector<std::pair<uint32_t, uint32_t>> Groups(Selected.begin(), Selected.end());
	for(const std::pair<uint32_t, uint32_t> &Group : Groups) {
		Edits.push_back({ Group.first, Group.first, "(" });
		Edits.push_back({ Group.second, Group.second, ")" });
	}
	TextEdit::Apply(Src, Edits, false, &Groups);
	// 終了
	return;
}

/**
 * 二項式の左右と演算子の取得関数
 * 計算量：直接の子の数 N，デコードデータの数 R に対し O(N + log R)
 * @param Src 解析用の本文
 * @param Node 調べるノード
 * @return 対象の二項式なら其の部品
 */
ParseAdapter::BinaryParts ParseAdapter::GetBinaryOf(const ParsedTree &Src, const TSNode Node) {
	// 解析エラーの無い二項式でなければ部品を持たない事の返戻
	if(ts_node_is_null(Node) || !Src.Is(Node, Core::Role::BinaryPrecExpr) || Src.HasError(Node)) return {};
	std::array<TSNode, 3> Parts {};
	// 左右と演算子の３部品が無ければ対象外の返戻
	if(GetCodeChildren(Src, Node, Parts) != Parts.size()) return {};
	// 原文の演算子と優先順位の復元
	const LexemeRestoration *const Restoration = Src.GetRestorationAt(Src.GetStart(Parts[1]), Src.GetEnd(Parts[1]));
	const std::string_view Operator = Restoration ? std::string_view(Restoration->Original) : Src.GetView(Parts[1]);
	const Core::SyntaxRules &Rules = Src.GetRules();
	const std::unordered_map<std::string_view, int>::const_iterator Found = Rules.OperatorPrec.find(Operator);
	const int Prec =
	Src.GetReading().ReassociatedOperators.contains(Operator) && Found != Rules.OperatorPrec.end() ? Found->second : 0;
	// 演算子を葉として持つ二項式の部品の返戻
	return { Parts[0], Parts[2], Prec, Src.Is(Parts[1], Core::Role::UserDefinedOperator) && !Prec };
}

/**
 * 優先順位で連鎖を１度だけ簡約する関数
 * 計算量：連鎖のノード数 N と修復領域数 G に対し O(N + G log G)
 * @param Src 解析用の本文
 * @param Root 連鎖の根
 * @param Parts ノード毎に１度求めた連鎖の部品
 * @param Groups パーサーの結合と異なる部分式の格納先
 * @return 非結合の連鎖の見送理由，組み直せれば理由無
 * @throws FormatLimitExceeded 修復の作業予算を超えた場合
 * @throws std::bad_alloc 連鎖の作業列を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
FailureReason ParseAdapter::CollectMisboundChain(
	ParsedTree &Src,
	const TSNode Root,
	const ChainIdx &Parts,
	ExprGroups &Groups
) {
	// 対象文法の記法
	const Core::SyntaxRules &Rules = Src.GetRules();
	std::vector<OperandSpan> Vals;
	std::vector<TSNode> Pending;
	// 演算子連鎖の先頭の簡約
	const auto Reduce = [&]() -> void {
		Src.BoundaryRepairWork.Charge(1);
		// 未処理の演算子連鎖の部品の取得
		const TSNode Operation = Pending.back();
		Pending.pop_back();
		if(Parts.at(Operation.id).IsPrefix) Vals.back().Start = Src.GetStart(Operation);
		else {
			const OperandSpan Right = Vals.back();
			Vals.pop_back();
			Vals.back().End = Right.End;
		}
		if(
			const OperandSpan &Reduced = Vals.back();
			(Src.GetStart(Operation) != Reduced.Start || Src.GetEnd(Operation) != Reduced.End) &&
			(Reduced.Start != Src.GetStart(Root) || Reduced.End != Src.GetEnd(Root))
		) Groups.emplace(Reduced.Start, Reduced.End);
		// 終了
		return;
	};
	for(std::vector<std::pair<TSNode, bool>> Stack { { Root, false } }; !Stack.empty();) {
		const std::pair<TSNode, bool> Frame = Stack.back();
		Stack.pop_back();
		Src.BoundaryRepairWork.Charge(1);
		const TSNode Node = Frame.first;
		const ChainIdx::const_iterator Found = Parts.find(Node.id);
		if(Found == Parts.end()) {
			Vals.push_back({ Src.GetStart(Node), Src.GetEnd(Node) });
			continue;
		}
		const ChainParts &Shape = Found->second;
		if(Shape.IsPostfix) {
			if(!Frame.second) {
				Stack.push_back({ Node, true });
				Stack.push_back({ Shape.Left, false });
			} else {
				// 後置の式は未簡約の最も右の値へ結び，二項・前置の式の簡約より先に範囲を確定する
				OperandSpan &Right = Vals.back();
				Right.End = Src.GetEnd(Node);
				// 前置の被演算子だけを包むと演算子の呼出へ誤読される為，前置の式全体の簡約へ任せる
				if(Right.Start != Src.GetStart(Shape.Left) && (Pending.empty() || !Parts.at(Pending.back().id).IsPrefix)) {
					Groups.emplace(Right.Start, Right.End);
				}
			}
			continue;
		}
		if(Shape.IsPrefix) {
			Pending.push_back(Node);
			Stack.push_back({ Shape.Left, false });
			continue;
		}
		if(!Frame.second) {
			Stack.push_back({ Shape.Right, false });
			Stack.push_back({ Node, true });
			Stack.push_back({ Shape.Left, false });
			continue;
		}
		while(!Pending.empty()) {
			const ChainParts &Top = Parts.at(Pending.back().id);
			// 非結合の連鎖の見送理由の返戻
			if(Top.Prec == Shape.Prec && Rules.NonAssocPrecs.contains(Shape.Prec)) return FailureReason::NonAssocOperators;
			if(Top.Prec < Shape.Prec || Top.Prec == Shape.Prec && Rules.RightAssocPrecs.contains(Shape.Prec)) break;
			Reduce();
		}
		Pending.push_back(Node);
	}
	while(!Pending.empty()) Reduce();
	// 標準の結合を確定した連鎖の返戻
	return {};
}

/**
 * 値の名前を型と読んだ比較・乗算の読みの修復関数
 * 使う位置で値の宣言を確認出来る裸の名前だけを解析用の括弧で包み，型・未確定の名前・修飾名は読み替えない
 * 計算量：候補数 C，名前の長さの和 L，同名の宣言数 D，インクルード数 I，修復数 K に対し O(C + L + C(log(D + 1) + log(I + 1)) + K log K) と世代毎の親子表・必要な再解析・宣言収集の費用
 * @param Src 解析済の本文（必要な束縛は最初の照会で収集）
 * @throws FormatLimitExceeded 挿入後の解析が期限を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void ParseAdapter::RepairValDecls(ParsedTree &Src) {
	ExprGroups Groups;
	// 親子表の１度のカーソル走査で記録した候補だけを原文順に読む
	Src.RecordNodeRelations();
	for(const uint32_t Relation : Src.Syntax.Relations->DeclCandidates) {
		const ParsedChild &Record = Src.GetNodeAt(Relation);
		const TSNode Node = Record.Node;
		const Core::RoleSet &Roles = Src.GetProfile().GetKindOf(Record.Symbol).Roles;
		++Src.Syntax.Relations->DeclWork;
		// 括弧付宣言の修復も，候補が在る場合だけ共通の表を要する
		if(!Src.HasCollectedNames() && Roles.test(static_cast<size_t>(Core::Role::ExprStmt)) && Src.ReadParenthesizedDecl(Node)) {
			Src.CollectDeclaredNames();
		}
		// 括弧付宣言の候補は上で扱い，型の名前を式へ戻す候補だけを残す
		if(!Roles.test(static_cast<size_t>(Core::Role::PotentialExprDecl))) continue;
		const TSNode Type = Src.GetFieldChild(Node, Core::ChildRole::Type);
		TSNode Name = Type;
		if(Src.Is(Name, Core::Role::TemplateInstantiationType) && Src.GetReading().CanMisreadTemplateDecls) {
			Name = ts_node_named_child(Name, 0);
		}
		// 裸の名前でない型候補と曖昧でない型候補の読飛（曖昧性は修復で包める裸の型候補だけで照会する）
		if(!Src.Is(Name, Core::Role::TypeName) || ts_node_child_count(Name) || !Src.IsAmbiguousDeclType(Node, Type)) continue;
		const std::string_view Spelling = Src.GetView(Name);
		if(const uint32_t Use = Src.GetStart(Node); Src.IsDeclaredName(Spelling, Use)) {
			Groups.emplace(Src.GetStart(Name), Src.GetEnd(Name));
		}
	}
	// 読違を持たない本文の終了
	if(Groups.empty()) return;
	std::vector<TextEdit> Edits;
	WrapExprs(Src, Groups, Edits);
	// 修復した構文の宣言と位置の再収集
	Src.CollectDeclaredNames();
	// 終了
	return;
}

/**
 * 前置・二項・後置の演算子の結合の修復関数
 * 後置の式の受手を最も右の値として先に確定し，呼出・添字・参照の外側も同じ連鎖で簡約する
 * 計算量：ノード数 N と修復領域数 G，トークン復元・編集１回分の費用 Q に対し O(N + G log G + Q)
 * @param Src 解析用の本文
 * @return 非結合又は優先順位不明の連鎖の見送理由，修復出来れば理由無
 * @throws FormatLimitExceeded 挿入後の解析が期限を超えた場合
 * @throws std::bad_alloc 修復計画のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
FailureReason ParseAdapter::RepairOperatorBindings(ParsedTree &Src) {
	// ノード毎の演算子連鎖の部品の収集準備
	ChainIdx Parts;
	std::vector<std::pair<TSNode, bool>> Roots; // 根と，前置又は二項の演算を含むか
	bool HasUnknown = false;
	std::vector<TextEdit> Edits;
	std::vector<LexemeRestoration> Originals;
	const ReadingRules &Reading = Src.GetReading();
	WalkRecordedChildren(
		Src,
		Src.GetRoot(),
		[&](const TSNode Node, const TSNode Parent) -> bool {
			// コードの連鎖と作業予算へ通常コメントを含めない事の返戻
			if(Src.Is(Node, Core::Role::Comment)) return false;
			Src.BoundaryRepairWork.Charge(1);
			// 二項式の被演算子と優先順位の取得
			const BinaryParts Binary = GetBinaryOf(Src, Node);
			if(Binary.IsUnknown) {
				// 演算子連鎖の境界の判定
				const auto IsBoundary = [&Src](const TSNode Adjacent) -> bool {
					// 結合を確定する式との接続の返戻
					return !ts_node_is_null(Adjacent) &&
					(Src.Is(Adjacent, Core::Role::BinaryPrecExpr) || Src.Is(Adjacent, Core::Role::OperatorPrecBoundary));
				};
				HasUnknown |= IsBoundary(Binary.Left) || IsBoundary(Binary.Right) || IsBoundary(Parent) ||
				Src.Is(Parent, Core::Role::ConditionalOperatorExpr) && !ts_node_eq(Src.GetFieldChild(Parent, Core::ChildRole::IfTrue), Node);
			}
			if(Binary.Prec) Parts.emplace(Node.id, ChainParts{ Binary.Left, Binary.Right, Binary.Prec, 0, false, false });
			else if(Src.Is(Node, Core::Role::PrefixExpr)) {
				std::array<TSNode, 2> Children {};
				if(GetCodeChildren(Src, Node, Children) == Children.size() && !Src.Is(Children[0], Core::Role::DotToken)) {
					Parts.emplace(Node.id, ChainParts{ Children[1], {}, std::numeric_limits<int>::max(), 0, true, false });
				}
			} else if(Reading.DoesMisbindPostfixOperand && Src.Is(Node, Core::Role::PostfixBindingExpr) && !Src.HasError(Node)) {
				std::array<TSNode, 1> Children {};
				if(GetCodeChildren(Src, Node, Children)) Parts.emplace(Node.id, ChainParts{ Children[0], {}, 0, 0, false, true });
			}
			if(const ChainIdx::iterator Current = Parts.find(Node.id); Current != Parts.end()) {
				// 同じ連鎖の根を直接参照し，演算子の無い後置連鎖を簡約の対象にしない
				if(const ChainIdx::const_iterator Up = Parts.find(Parent.id); Up == Parts.end()) {
					Current->second.RootIdx = Roots.size();
					Roots.emplace_back(Node, false);
				} else Current->second.RootIdx = Up->second.RootIdx;
				Roots[Current->second.RootIdx].second |= !Current->second.IsPostfix;
			}
			if(Src.Is(Node, Core::Role::UserDefinedOperator)) {
				const std::string_view Original = Src.GetView(Node);
				if(
					const std::unordered_map<std::string_view, std::string_view>::const_iterator StandIn = Reading.OperatorStandIns.find(Original);
					Src.Is(Parent, Core::Role::BinaryPrecExpr) && StandIn != Reading.OperatorStandIns.end()
				) {
					TextEdit::Push(Src.GetStart(Node), Src.GetEnd(Node), std::string(StandIn->second), Edits);
					Originals.push_back({ Src.GetStart(Node), Src.GetEnd(Node), std::string(Original) });
				}
				// 演算子の子を重ねて集めない事の返戻
				return false;
			}
			// 括弧で分かれた別の連鎖も辿る事の返戻
			return true;
		}
	);
	// 優先順位を確定出来ない連鎖の見送理由の返戻
	if(HasUnknown) return FailureReason::UnknownOperatorPrec;
	ExprGroups Groups;
	for(const std::pair<TSNode, bool> &Root : Roots) {
		Src.BoundaryRepairWork.Charge(1);
		if(!Root.second) continue;
		if(const FailureReason Reason = CollectMisboundChain(Src, Root.first, Parts, Groups); Reason.Val != FailureReason::None) {
			// 非結合の連鎖の見送理由の返戻
			return Reason;
		}
	}
	// トークンの置換と全ての囲みの一括適用
	WrapExprs(Src, Groups, Edits);
	if(!Originals.empty()) {
		const std::vector<AppliedEdit> &Batch = Src.EditBatches.back();
		size_t Next = 0;
		for(const AppliedEdit &Edit : Batch) {
			if(Next == Originals.size()) break;
			if(Edit.Start == Originals[Next].Start && Edit.End == Originals[Next].End) {
				Src.AddLexemeRestoration(Edit.NewStart, Edit.NewEnd, std::move(Originals[Next++].Original));
			}
		}
	}
	// 標準の結合に修復した結果の返戻
	return {};
}

/**
 * 構築の被呼出側と型引数の結合の修復関数
 * パーサーが実引数の無い構築の外へ結んだ後置の式を被呼出側に含め，型引数より前の単項演算子は外へ出す
 * 後置の式の連鎖は最初の呼出迄を１度に含め，ネストする修復領域も同じ木から全て求める
 * 計算量：ノード数 N と修復領域数 G，編集１回分の費用 Q に対し O(N + G log G + Q)
 * @param Src 解析用の本文
 * @throws FormatLimitExceeded 挿入後の解析が期限を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void ParseAdapter::RepairConstructionBindings(ParsedTree &Src) {
	// 構築の外へ結ばれ得る後置の式か
	const auto IsPostfix = [&Src](const TSNode Node) -> bool {
		// 非空のアサーション・メンバ・添字・呼出の式かの返戻
		return Src.Is(Node, Core::Role::CallExpr) || Src.Is(Node, Core::Role::MemberExpr) || Src.Is(Node, Core::Role::NonNullExpr) ||
		Src.Is(Node, Core::Role::SubscriptExprNode);
	};
	ExprGroups Groups;
	WalkRecordedChildren(
		Src,
		Src.GetRoot(),
		[&](const TSNode Node, const std::span<const TSNode> Ancestors) -> bool {
			// コンストラクタの最初の名前付の子の取得
			const TSNode First = ts_node_named_child(Node, 0);
			if(Src.Is(Node, Core::Role::Instantiation) && Src.Is(First, Core::Role::UnaryOperation)) {
				// 型引数より前の単項演算子の被演算子と型引数の範囲
				if(const uint32_t Count = ts_node_named_child_count(First)) {
					Groups.emplace(Src.GetStart(ts_node_named_child(First, Count - 1)), Src.GetEnd(Node));
				}
				// 具体化の内側も辿る事の返戻
				return true;
			}
			// 実引数の無い構築を後置の式が包まないノードの子を辿る事の返戻
			if(
				!Src.Is(First, Core::Role::Construction) || !ts_node_is_null(Src.GetFieldChild(First, Core::ChildRole::Args)) ||
				!IsPostfix(Node)
			) return true;
			// 構築の外へ結ばれた後置の式の連鎖の最も外の式（最初の呼出で止め，其の実引数は構築の実引数）
			TSNode Top = Node;
			for(size_t Depth = Ancestors.size(); Depth && !Src.Is(Top, Core::Role::CallExpr);) {
				const TSNode Up = Ancestors[--Depth];
				if(!IsPostfix(Up) || !ts_node_eq(ts_node_named_child(Up, 0), Top)) break;
				Top = Up;
			}
			const TSNode Constructor = ts_node_named_child(First, 0), Args = Src.GetFieldChild(Top, Core::ChildRole::Args);
			// 被構築の式から後置の式の連鎖の終わり（呼出は実引数の前）迄
			if(!ts_node_is_null(Constructor)) {
				Groups.emplace(Src.GetStart(Constructor), ts_node_is_null(Args) ? Src.GetEnd(Top) : Src.GetStart(Args));
			}
			// ネストする構築も辿る事の返戻
			return true;
		}
	);
	// 修復領域を持たない本文の終了
	if(Groups.empty()) return;
	// 誤って結ばれた部分式の囲み
	std::vector<TextEdit> Edits;
	WrapExprs(Src, Groups, Edits);
	// 終了
	return;
}

/**
 * 文脈上の語と同じ名前の呼出を其の語の文と読んだエラーの読直関数
 * パーサーは Python の `type(x).y = v` 等，語 `type` の後に別名の名前以外が続く文を型の別名の文と読む為，語を解析用の括弧で包んで呼出の式と読ませる
 * 計算量：記録済なら候補数 K，未記録なら本文の長さ B と訪問数 V，再解析１回分の費用 Q に対し O(K + Q) 又は O(B + V + Q)
 * @param Src 解析用の本文
 * @throws FormatLimitExceeded 挿入後の解析が期限を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void ParseAdapter::RepairSoftKeywordCalls(ParsedTree &Src) {
	ExprGroups Groups;
	const auto Collect = [&](const TSNode Node) -> void {
		// 別名の名前（型の包みの中の名前か，型の仮引数を付けた名前の頭）
		const TSNode Left = Src.GetFieldChild(Node, Core::ChildRole::Left);
		TSNode Name = ts_node_is_null(Left) ? TSNode{} : ts_node_named_child(Left, 0);
		if(Src.Is(Name, Core::Role::TypeApplication)) Name = ts_node_named_child(Name, 0);
		if(const TSNode Keyword = ts_node_child(Node, 0); !Src.Is(Name, Core::Role::Identifier)) {
			Groups.emplace(Src.GetStart(Keyword), Src.GetEnd(Keyword));
		}
		// 終了
		return;
	};
	// 修復前に記録済なら候補だけを読み，未記録の世代の表は此の判定の為だけに作らない
	if(Src.Syntax.HasParents) for(const uint32_t Relation : Src.Syntax.Relations->SoftKeywordCandidates) {
		Collect(Src.GetNodeAt(Relation).Node);
	} else {
		const TSNode Root = Src.GetRoot();
		const std::string_view Text = Src.GetCodeText(), Head = Src.GetReading().SoftKeywordCallHead;
		// 欠落した語を持ち得る木は全走査を保ち，正常な木では対象の語を含む経路だけ辿る
		const bool ShouldPrune = !ts_node_has_error(Root) && !Head.empty();
		std::vector<uint32_t> Heads;
		if(ShouldPrune) for(size_t Pos = Text.find(Head); Pos != std::string_view::npos; Pos = Text.find(Head, Pos + Head.size())) {
			Heads.push_back(static_cast<uint32_t>(Pos));
		}
		size_t NextHead = 0;
		WalkChildrenCursor(
			Root,
			[&](const TSNode Node) -> bool {
				if(ShouldPrune) {
					const uint32_t Start = Src.GetStart(Node);
					while(NextHead < Heads.size() && Heads[NextHead] < Start) ++NextHead;
					// 先頭の語を含まない部分木へ降りない事の返戻
					if(NextHead == Heads.size() || Heads[NextHead] >= Src.GetEnd(Node)) return false;
				}
				// 型の別名の文でないノードの子を辿る事の返戻
				if(!Src.Is(Node, Core::Role::TypeAlias)) return true;
				Collect(Node);
				// 文の中に別の文は無い為に子を辿らない事の返戻
				return false;
			}
		);
	}
	std::vector<TextEdit> Edits;
	if(!Groups.empty()) WrapExprs(Src, Groups, Edits);
	// 終了
	return;
}
