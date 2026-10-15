#include "SyntaxNormalizer.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * JavaScript・TypeScript の `await` 式を `await` 名への呼出・添字と誤読した後置の連鎖かの判定関数
	 * パーサーは `await (0, f)(x)` を `await` 名を被呼出側とする呼出と読む
	 * `await [a, b][0]` は `await` 名への添字と読む
	 * 計算量：初回に調べるノード数 N，照会数 Q とテキスト総長 L に対し期待値 O(N + Q + L)
	 * @param Syntax 構文モデル
	 * @param Node 判定する式
	 * @return 左端が `await` 名への呼出・添字の後置の連鎖なら `true`
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool SyntaxNormalizer::HasLeadingAwaitName(const SyntaxModel &Syntax, NodeId Node) {
		std::unordered_map<NodeId, bool> &Memo = Syntax.EditNormalizationFacts().Parens->Scan.LeadingAwait;
		std::vector<NodeId> Pending;
		bool Result = false;
		while(true) {
			Syntax.Charge(1);
			if(const std::unordered_map<NodeId, bool>::const_iterator Known = Memo.find(Node); Known != Memo.end()) {
				Result = Known->second;
				break;
			}
			Pending.push_back(Node);
			if(!Syntax.Is(Node, Role::AwaitNameReceiver)) break;
			const NodeId Head = Syntax.GetChild(Node, 0);
			if(Syntax.Is(Head, Role::Identifier) && Syntax.IsWord(Head, Word::AwaitName)) {
				Result = !Syntax.GetNode(Head).Facts.IsKeywordName && !Syntax.Is(Node, Role::MemberExpr);
				break;
			}
			Node = Head;
		}
		for(const NodeId Visited : Pending) Memo.emplace(Visited, Result);
		// 同じ左端を持つ連鎖の共有結果の返戻
		return Result;
	}

	/**
	 * 後置の連鎖の受手に置ける原子式かの判定関数
	 * 計算量：初回に調べるノード数 N，照会数 Q とテキスト総長 L に対し期待値 O(N + Q + L)
	 * @param Syntax 構文モデル
	 * @param Node 判定する式
	 * @return 原子式なら `true`
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool SyntaxNormalizer::IsPostfixAtom(const SyntaxModel &Syntax, NodeId Node) {
		// 待機式の読違は原子式の判定から除く
		if(HasLeadingAwaitName(Syntax, Node)) return false;
		std::unordered_map<NodeId, bool> &Memo = Syntax.EditNormalizationFacts().Parens->Scan.PostfixAtoms;
		std::vector<NodeId> Pending;
		bool Result = false;
		while(true) {
			Syntax.Charge(1);
			if(const std::unordered_map<NodeId, bool>::const_iterator Known = Memo.find(Node); Known != Memo.end()) {
				Result = Known->second;
				break;
			}
			Pending.push_back(Node);
			// 原子に達した事の返戻
			if(
				Syntax.Is(Node, Role::PostfixReceiverLeaf) ||
				Syntax.Is(Node, Role::Construction) && Syntax.GetFieldChild(Node, ChildRole::Args) != NoNode ||
				Syntax.Is(Node, Role::AtomicConstruction) || Syntax.Is(Node, Role::AtomicNum)
			) {
				Result = true;
				break;
			}
			const uint32_t Count = Syntax.GetChildCount(Node);
			const NodeId Last = Syntax.GetChild(Node, Count - 1);
			// 後置の連鎖でない式・メンバへのポインタ (`.*` / `->*`)・オプショナルチェーンの返戻
			if(
				const bool IsChain =
				Syntax.Is(Node, Role::PostfixReceiverChain) || Syntax.Is(Node, Role::PostfixTry) || Syntax.Is(Node, Role::PostfixExpr) &&
				(Syntax.Is(Last, Role::ExclamationMark) || Syntax.IsTokenRole(Last, Role::NonNullAssertionToken)); !Count || !IsChain ||
				Syntax.Is(Node, Role::FieldExpr) &&
				Syntax.HasLastTokenRole(Syntax.GetFieldChild(Node, ChildRole::Operator), Role::PtrMemberAccess) ||
				Core::SyntaxModel::HasChildMatching(
					Syntax,
					Node,
					[&Syntax](const NodeId Child) -> bool {
						// 後置の連鎖を阻むオプショナルチェーンの記号かの返戻
						return Syntax.Is(Child, Role::OptionalChain) || Syntax.Is(Child, Role::OptionalNavigationMark) ||
						Syntax.Is(Child, Role::NavigationSuffix) && Syntax.IsTokenRole(Syntax.GetChild(Child, 0), Role::OptionalAccessToken);
					}
				)
			) break;
			// パーサーが呼出と読んだ前置・中置式も，先頭を辿って除く
			Node = Syntax.GetChild(Node, 0);
		}
		for(const NodeId Visited : Pending) Memo.emplace(Visited, Result);
		// 各子の確定した原子式の判定の返戻
		return Result;
	}

	/**
	 * 前置の演算子の呼出と読んだ Swift の括弧の実引数の並びの取得関数
	 * tree-sitter-swift は前置の演算子の後の括弧 (`-(y)` / `!(y)`) を演算子の呼出と読む
	 * 否定の `!` は名前付の `bang` ノードとして読まれる
	 * 中置式に続く前置演算 (`x + -(y)`) は，中置式の末尾のトークンとして読まれる（`(x + -)(y)`）
	 * 計算量：参照する式のノード数 N とテキストの総長 L に対し O(N + L)
	 * @param Syntax 構文モデル
	 * @param Node ノード
	 * @return 前置の演算子の被演算子を包む括弧と読んだ実引数の並び（其の形でなければ `NoNode`）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	NodeId SyntaxNormalizer::GetPrefixCallArgs(const SyntaxModel &Syntax, const NodeId Node) {
		// 前置の演算子の呼出の形でない事の返戻
		if(Syntax.GetChildCount(Node) != 2 || !Syntax.Is(Node, Role::OperatorCall)) return NoNode;
		// 呼出側の前置の演算子（中置の式の末尾のトークンを含む）
		NodeId Prefix = Syntax.GetChild(Node, 0);
		if(Syntax.IsNamed(Prefix) && !Syntax.Is(Prefix, Role::ExclamationMark)) {
			const uint32_t Count = Syntax.GetChildCount(Prefix);
			Prefix = Count > 1 ? Syntax.GetChild(Prefix, Count - 1) : NoNode;
			// 末尾が演算子のトークンでない事の返戻
			if(Prefix == NoNode || Syntax.IsNamed(Prefix)) return NoNode;
		}
		const NodeId Suffix = Syntax.GetChild(Node, 1);
		// 前置の演算子と実引数の並びの組でない事の返戻
		if(
			!Syntax.GetRules().CanFuseOperatorChar(Syntax.GetFirstChar(Prefix)) || !Syntax.Is(Suffix, Role::CallSuffix) ||
			Syntax.GetNamedChildCount(Suffix) != 1
		) return NoNode;
		const NodeId Args = Syntax.GetNamedChild(Suffix, 0);
		// 実引数の並びの返戻
		return Syntax.Is(Args, Role::ValArgs) ? Args : NoNode;
	}

	/**
	 * 演算子の優先順位の取得関数
	 * 計算量：初回に評価する式のノード数 N とテキスト総長 L に対し期待値 O(N + L)，再照会は期待値 O(1)
	 * @param Syntax 構文モデル
	 * @param OperatorNode 優先順位を取得する演算子のノード
	 * @param Memo ノード別の優先順位の控え（連なる後置アサーションの途中のノードも共有する）
	 * @return 優先順位（不明時は０）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	int SyntaxNormalizer::GetOperatorPrec(
		const SyntaxModel &Syntax,
		const NodeId OperatorNode,
		std::unordered_map<NodeId, int> &Memo
	) const {
		// 確定済の照会では走査枠を確保しない
		if(const std::unordered_map<NodeId, int>::const_iterator Known = Memo.find(OperatorNode); Known != Memo.end()) {
			Syntax.Charge(1);
			// 同じ構造で確定した優先順位の返戻
			return Known->second;
		}
		// 単一ノードから確定する演算子の優先順位の取得
		const auto Direct = [&Syntax](const NodeId Node) -> int {
			// 比較のテキストを持つ中置の演算子の優先順位の返戻
			if(
				Syntax.Is(Node, Role::CmpInfix) &&
				Syntax.GetRules().CmpOperators.contains(Syntax.GetText(Syntax.GetFieldChild(Node, ChildRole::Operator)))
			) return 7;
			// 波括弧で閉じる Kotlin・Swift のラムダの後置の優先順位の返戻
			if(Syntax.Is(Node, Role::LambdaLiteral)) return 16;
			if(Syntax.Is(Node, Role::OperatorCall)) {
				const NodeId Head = Syntax.GetChild(Node, 0);
				// Swift の呼出での演算子始まりに応じた前置又は後置の優先順位の返戻
				return !Syntax.IsNamed(Head) || Syntax.Is(Head, Role::ExclamationMark) ? 15 : 16;
			}
			// 前置単項（Swift の片側だけの範囲 `..<n` と前置の増減 `++x` を含む）の優先順位の返戻
			if(
				Syntax.Is(Node, Role::PrefixUnary) || Syntax.Is(Node, Role::RangeWithoutStart) ||
				Syntax.Is(Node, Role::UpdateExpr) && !Syntax.IsNamed(Syntax.GetChild(Node, 0))
			) return 15;
			// 後置（Swift の片側だけの範囲 `n...` を含む）の優先順位の返戻
			if(Syntax.Is(Node, Role::PostfixOrUpdateExpr) || Syntax.Is(Node, Role::RangeWithoutEnd)) return 16;
			if(Syntax.Is(Node, Role::SwitchWithPrec)) {
				// C# の呼出の後置の優先順位の返戻
				if(Syntax.Is(Node, Role::InvocationExpr)) return 16;
				// C# の `switch` 式と `with` 式の優先順位の返戻
				if(Syntax.Is(Node, Role::SwitchOrWithExpr)) return 14;
			}
			if(Syntax.Is(Node, Role::KeywordExprPrec)) {
				// `await` の前置単項の優先順位の返戻
				if(Syntax.Is(Node, Role::AwaitVal)) return 15;
				// 後置のメンバの参照・呼出の優先順位（`(a + b)(c)` の被呼出式の括弧を保護）の返戻
				if(Syntax.Is(Node, Role::CallAttributeOrSubscript)) return 16;
				// トークン表に無い三項の優先順位の返戻
				if(Syntax.Is(Node, Role::ConditionalExpr)) return 2;
				// `not` の比較より下で `and` より上の優先順位（`not (a and b)` / `(not a) == b` の括弧を保持する）の返戻
				if(Syntax.Is(Node, Role::LogicalNegation)) return 6;
			}
			// Kotlin の中置関数の優先順位（利用者が順位を定める Swift は対象外）の返戻
			if(Syntax.Is(Node, Role::NamedInfixCall)) return 10;
			// Swift の `as` / `as?` の優先順位（`CastingPrecedence` は `nil` 合体と範囲の間）の返戻
			if(Syntax.Is(Node, Role::ComposedTypeCast)) return 9;
			// Ruby のメソッド呼出での受手の二項式保護用の後置の優先順位の返戻
			if(Syntax.Is(Node, Role::CommandCall)) return 16;
			const SyntaxRules &Rules = Syntax.GetRules();
			for(const NodeId Child : Syntax.GetChildrenOf(Node)) {
				if(Syntax.IsNamed(Child)) continue;
				const std::string_view Operator = Syntax.GetLexemeOf(Child);
				const std::unordered_map<std::string_view, int>::const_iterator Found = Rules.OperatorPrec.find(Operator);
				if(Found == Rules.OperatorPrec.end()) continue;
				if(Syntax.Is(Node, Role::IterationHeader) && !Rules.AssignmentOperators.contains(Operator)) {
					// 反復対象の文法が許す最弱の優先順位の返戻
					return Syntax.Is(Node, Role::ComprehensionIteration) ? 3 : 1;
				}
				// 投影されたトークンの優先順位の返戻
				return Found->second;
			}
			// 演算子を持たない事の返戻
			return 0;
		};
		enum class Phase : uint8_t { Enter, Prefix, Assertion, Complete }; // 式の優先順位を調べる段階の遷移順
		// 式の優先順位の評価の状態
		struct Frame {
			NodeId Node;
			NodeId Operand = NoNode;
			std::vector<NodeId> Chain {};
			Phase State = Phase::Enter;
		}; // 同じ結果を共有するアサーションの連鎖
		// ネストの式と後置のアサーションを段階毎に評価
		std::vector<Frame> Pending { { OperatorNode } };
		int Result = 0;
		while(!Pending.empty()) {
			switch(Frame &Cur = Pending.back(); Cur.State) {
			case Phase::Enter:
				Syntax.Charge(1);
				if(
					const std::unordered_map<NodeId, int>::const_iterator Known = Pending.size() > 1 ? Memo.find(Cur.Node) : Memo.end();
					Known != Memo.end()
				) {
					Result = Known->second;
					Pending.pop_back();
					continue;
				}
				if(
					const NodeId Node = Cur.Node;
					Syntax.Is(Node, Role::LowKeywordPrec) && (
						Syntax.Is(Node, Role::UnaryVal) && Syntax.Is(Syntax.GetChild(Node, 0), Role::LowPrecUnaryKeyword) ||
						Syntax.Is(Node, Role::LowPrecPatternTest)
					)
				) Result = 1;
				else if(Syntax.Is(Node, Role::TypeColonParent) || Syntax.Is(Node, Role::AngleBracketList)) Result = 0;
				else if(Syntax.Is(Node, Role::OpenPrefixExpr)) Result = 1;
				else if(Syntax.Is(Node, Role::KeywordUnary) || Syntax.Is(Node, Role::PrefixAwait) || HasLeadingAwaitName(Syntax, Node)) {
					Result = 15;
				} else {
					Cur.State = Phase::Prefix;
					if(Syntax.Is(Node, Role::PrefixAbsorbsInfix)) {
						Cur.Operand = Syntax.GetLastNamedChild(Node);
						while(Syntax.Is(Cur.Operand, Role::PrefixExpr)) Cur.Operand = Syntax.GetLastNamedChild(Cur.Operand);
						if(Cur.Operand != NoNode) {
							Pending.push_back({ Cur.Operand });
							continue;
						}
					}
					Result = 0;
					continue;
				}
				Cur.State = Phase::Complete;
				continue;
			case Phase::Prefix:
				if(Cur.Operand != NoNode && Result && Result < 15) {
					Cur.State = Phase::Complete;
					continue;
				}
				Cur.State = Phase::Assertion;
				Result = 0;
				if(Syntax.Is(Cur.Node, Role::NonNullAssertion)) {
					Cur.Operand = Syntax.GetNamedChild(Cur.Node, 0);
					for(; !Result && Syntax.Is(Cur.Operand, Role::NonNullAssertion); Cur.Operand = Syntax.GetNamedChild(Cur.Operand, 0)) {
						if(const std::unordered_map<NodeId, int>::const_iterator At = Memo.find(Cur.Operand); At != Memo.end()) {
							Result = At->second;
							continue;
						}
						Cur.Chain.push_back(Cur.Operand);
					}
					if(!Result && Cur.Operand != NoNode) {
						Pending.push_back({ Cur.Operand });
						continue;
					}
				} else {
					Result = Direct(Cur.Node);
					Cur.State = Phase::Complete;
					continue;
				}
				// 控え又は末端から得た優先順位の同じ巡回でのアサーションへの反映
				[[fallthrough]];
			case Phase::Assertion:
				Result = Result && Result < 16 ? Result : 17;
				for(const NodeId Id : Cur.Chain) Memo.insert_or_assign(Id, Result);
				Cur.State = Phase::Complete;
				continue;
			case Phase::Complete:
				Memo.insert_or_assign(Cur.Node, Result);
				Pending.pop_back();
				break;
			}
		}
		// 演算子の優先順位の返戻
		return Result;
	}

	/**
	 * 外すと宣言と読まれ得る括弧かの判定関数（整形仕様 5.4 の型と式の曖昧さ）
	 * 型名とも読める呼出の形 (`T(x)`) で，実引数も宣言子とも読める場合に式の文の先頭の括弧を保つ
	 * リテラル・値の演算は宣言子にも仮引数にも為らず，配列の長さや既定値は宣言の中でも式である
	 * 式の文から型の名前の単独の実引数を辿った呼出と，直接の初期化でも，其の形を包む実引数の括弧を保つ
	 * 計算量：共有する実引数と呼出のノード数 N，文頭の探索の総数 A，テキスト総長 L に対し期待値 O(N + A + L)
	 * @param Syntax 構文モデル
	 * @param Paren 括弧の最も外の層
	 * @param Inner 括弧の層を透過した中身
	 * @param ScanMemo 実引数の形と呼出の祖先の判定の控え
	 * @return 括弧を外すと宣言と読まれ得るなら `true`
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 実引数の探索又は共有表のメモリを確保出来ない場合
	 * @throws std::length_error 作業列又は共有表の要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool SyntaxNormalizer::IsParsedAsDecl(
		const SyntaxModel &Syntax,
		const NodeId Paren,
		const NodeId Inner,
		ParenScanMemo &ScanMemo
	) {
		// 型の名前とも読める被呼出側の呼出の形か
		const auto IsTypeNameCall = [&Syntax, &ScanMemo](const NodeId Node) -> bool {
			const NodeId Callee = Syntax.GetFieldChild(Node, ChildRole::Function);
			// 既存の短絡の評価順を保った，被呼出側が型名とも読める形でない事の返戻
			if(!Syntax.Is(Node, Role::CallNode) || !Syntax.Is(Callee, Role::DeclaratorCallee)) return false;
			const NodeId Args = Syntax.GetFieldChild(Node, ChildRole::Args);
			if(
				const std::unordered_map<NodeId, bool>::const_iterator Found = ScanMemo.DeclArgs.find(Args);
				Found != ScanMemo.DeclArgs.end()
			) {
				Syntax.Charge(1);
				// 共有済の実引数の宣言の可能性の返戻
				return Found->second;
			}
			// 実引数の宣言の可能性を子から共有する後行順の走査
			std::vector<std::pair<NodeId, bool>> Pending { { Args, false } };
			while(!Pending.empty()) {
				const std::pair<NodeId, bool> Cur = Pending.back();
				const NodeId Arg = Cur.first;
				Pending.pop_back();
				Syntax.Charge(1);
				if(!Cur.second && ScanMemo.DeclArgs.contains(Arg)) continue;
				const bool IsList = Syntax.Is(Arg, Role::DeclaratorContainer);
				const NodeId Child = Syntax.Is(Arg, Role::CallNode) ?
				Syntax.GetFieldChild(Arg, ChildRole::Args) :
				Syntax.Is(Arg, Role::DeclaratorFirstChild) ? Syntax.GetNamedChild(Arg, 0) : NoNode;
				if(Cur.second) {
					if(IsList) {
						for(const NodeId Item : Syntax.GetChildrenOf(Arg)) if(Syntax.IsNamed(Item)) {
							Syntax.Charge(1);
							if(!ScanMemo.DeclArgs.at(Item)) ScanMemo.DeclArgs.at(Arg) = false;
						}
					} else if(Child != NoNode) ScanMemo.DeclArgs.at(Arg) = ScanMemo.DeclArgs.at(Child);
					continue;
				}
				// 宣言子・仮引数の位置へ置けない値
				const bool CanBeDeclArg = !Syntax.Is(Arg, Role::DeclaratorInvalid) && (
					!Syntax.Is(Arg, Role::DeclaratorBinary) ||
					Syntax.IsTokenRole(Syntax.GetFieldChild(Arg, ChildRole::Operator), Role::DeclaratorOperator)
				);
				ScanMemo.DeclArgs.emplace(Arg, CanBeDeclArg);
				if(!CanBeDeclArg) continue;
				if(IsList || Child != NoNode) {
					Pending.emplace_back(Arg, true);
					if(IsList) {
						for(const NodeId Item : Syntax.GetChildrenOf(Arg)) if(Syntax.IsNamed(Item)) Pending.emplace_back(Item, false);
					} else Pending.emplace_back(Child, false);
				}
			// 添字の長さ・既定値・型引数・未確定の構文は宣言の一部に為り得るので中身を値と判定しない
			}
			// 実引数の構文だけでは宣言を除外出来ない事の返戻
			return ScanMemo.DeclArgs.at(Args);
		};
		const NodeId Parent = Syntax.GetParentOf(Paren);
		if(Syntax.Is(Parent, Role::ExprStmt)) {
			// 代入の左辺と添字の受手を辿った先頭の部分式
			NodeId Head = Inner;
			while(Syntax.Is(Head, Role::Assignment) || Syntax.Is(Head, Role::SubscriptExprNode)) Head = Syntax.GetNamedChild(Head, 0);
			// 式の文の先頭が型の名前の呼出かの返戻
			return IsTypeNameCall(Head);
		}
		// 実引数の並びでない括弧と，型の名前の呼出を包まない括弧の返戻
		if(!Syntax.Is(Parent, Role::ArgSeq) || !IsTypeNameCall(Inner)) return false;
		NodeId Call = Syntax.GetParentOf(Parent);
		// 直接の初期化の実引数の返戻
		if(Syntax.Is(Call, Role::InitDeclarator)) return true;
		// 同じ呼出の祖先は１度だけ判定し，辿った各呼出へ終端の結果を共有する
		std::vector<NodeId> Path;
		bool CanDeclare = false;
		while(true) {
			if(
				const std::unordered_map<NodeId, bool>::const_iterator Found = ScanMemo.DeclCalls.find(Call);
				Found != ScanMemo.DeclCalls.end()
			) {
				Syntax.Charge(1);
				CanDeclare = Found->second;
				break;
			}
			Path.push_back(Call);
			if(!IsTypeNameCall(Call) || Syntax.GetNamedChildCount(Syntax.GetFieldChild(Call, ChildRole::Args)) != 1) break;
			Syntax.Charge(1);
			const NodeId Up = Syntax.GetParentOf(Call);
			if(Syntax.Is(Up, Role::ExprStmt)) {
				CanDeclare = true;
				break;
			}
			if(!Syntax.Is(Up, Role::ArgSeq)) break;
			Call = Syntax.GetParentOf(Up);
		}
		for(const NodeId Visited : Path) {
			Syntax.Charge(1);
			ScanMemo.DeclCalls.emplace(Visited, CanDeclare);
		}
		// 同じ終端へ続く呼出の判定の返戻
		return CanDeclare;
	}

	/**
	 * 右端の子の取得関数
	 * 閉じのトークンで終わるノード（`a[b + c]` の添字は角括弧の中で閉じる）の最後の名前付の子は右端でない
	 * 計算量：最後の子を直接引く為 O(1)
	 * @param Syntax 構文モデル
	 * @param Node 対象のノード
	 * @return ノードの最後の子である名前付の子（無ければ `NoNode`）
	 */
	NodeId SyntaxNormalizer::GetRightEdgeChild(const SyntaxModel &Syntax, const NodeId Node) {
		// 右端の子の取得
		const NodeId Last = Syntax.GetChild(Node, Syntax.GetChildCount(Node) - 1);
		// 最後の子が名前付なら其の子の返戻
		return Syntax.IsNamed(Last) ? Last : NoNode;
	}

	/**
	 * キーワードで指定する型への変換かの判定関数
	 * 型の記述が基本型・型修飾子・抽象ポインタ宣言子だけなら，式・呼出と紛れない型変換として扱う
	 * 計算量：判定対象のノード数 N，祖先深度 H，テキスト総長 L に対し期待値 O(N H + N + L)
	 * @param Syntax 構文モデル
	 * @param Cast 型の変換のノード
	 * @return 基本型・型修飾子・抽象ポインタ宣言子だけの型指定への変換なら `true`
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool SyntaxNormalizer::IsKeywordTypeCast(const SyntaxModel &Syntax, const NodeId Cast) {
		// 型の変換以外の返戻
		if(!Syntax.Is(Cast, Role::CastExpr)) return false;
		const NodeId Descriptor = Syntax.GetFieldChild(Cast, ChildRole::Type);
		// 型の記述が基本型・型修飾子・抽象ポインタ宣言子だけかの返戻
		return Syntax.GetNamedChildCount(Descriptor) && !Core::SyntaxModel::HasChildMatching(
			Syntax,
			Descriptor,
			[&Syntax](const NodeId Child) -> bool {
				// 基本型・型修飾子・抽象ポインタ宣言子以外の名前付の子かの返戻
				return Syntax.IsNamed(Child) && !Syntax.Is(Child, Role::PrimitiveType) && !Syntax.Is(Child, Role::SizedTypeSpecifier) &&
				!Syntax.Is(Child, Role::TypeQualifier) && !(
					Syntax.Is(Child, Role::AbstractPtr) && !Core::SyntaxModel::HasNodeMatching(
						Syntax,
						Child,
						[&Syntax](const NodeId Part) -> bool {
							// 関数・配列の宣言子かの返戻
							return Syntax.Is(Part, Role::AbstractFunction) || Syntax.Is(Part, Role::AbstractArray);
						}
					)
				);
			}
		);
	}

	/**
	 * 部分木に型引数と比較の読分けに関わるトークンが在るかの判定関数
	 * 計算量：訪れたノード数 N と子の総数 C に対し期待値 O(N + C)，再照会は期待値 O(1)
	 * @param Syntax 構文モデル
	 * @param Root 調べる部分木
	 * @param Memo ノード毎の判定の控え
	 * @return 該当するトークンが在れば `true`
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool SyntaxNormalizer::HasAngleConflict(const SyntaxModel &Syntax, const NodeId Root, AngleFacts &Memo) const {
		if(const AngleFacts::const_iterator Found = Memo.find(Root); Found != Memo.end()) {
			Syntax.Charge(1);
			// 共有済の部分木の判定の返戻
			return Found->second.HasConflict;
		}
		// 子の判定を先に得る後行順の走査
		std::vector<std::pair<NodeId, bool>> Pending { { Root, false } };
		while(!Pending.empty()) {
			Syntax.Charge(1);
			const std::pair<NodeId, bool> Top = Pending.back();
			const NodeId Node = Top.first;
			Pending.pop_back();
			if(Memo.contains(Node)) continue;
			if(!Top.second) {
				Pending.emplace_back(Node, true);
				for(const NodeId Child : Syntax.GetChildrenOf(Node)) {
					Syntax.Charge(1);
					if(!Memo.contains(Child)) Pending.emplace_back(Child, false);
				}
				continue;
			}
			NormalizationFacts::AngleFact Fact;
			Fact.HasConflict = Syntax.IsTokenRole(Node, Role::AngleConflictToken);
			Fact.HasClose = !Syntax.IsNamed(Node) && Syntax.IsTokenRole(Node, Role::AngleCloseCandidate);
			for(const NodeId Child : Syntax.GetChildrenOf(Node)) {
				Syntax.Charge(1);
				const NormalizationFacts::AngleFact &ChildFact = Memo.at(Child);
				Fact.HasConflict |= ChildFact.HasConflict;
				if(ChildFact.HasClose) {
					Fact.HasClose = true;
					Fact.LastCloseChild = Syntax.GetNode(Child).Idx;
				}
			}
			Memo.emplace(Node, Fact);
		}
		// 部分木の判定の返戻
		return Memo.at(Root).HasConflict;
	}

	/**
	 * 対の括弧を開くノードかの判定関数
	 * 計算量：固定の文法の部品又は規則の表に対し O(1)
	 * @param Syntax 構文モデル
	 * @param Node ノード
	 * @return 最初の子が開きの丸括弧・角括弧・波括弧のトークンなら `true`
	 */
	bool SyntaxNormalizer::HasOpeningBracketPair(const SyntaxModel &Syntax, const NodeId Node) {
		// 対の括弧を開くノードかの判定
		const NodeId First = Syntax.GetChild(Node, 0);
		// 開きの括弧のトークンかの返戻
		return First != NoNode && !Syntax.IsNamed(First) && (
			Syntax.IsTokenRole(First, Role::OpeningParenToken) || Syntax.IsTokenRole(First, Role::OpeningBracketToken) ||
			Syntax.IsTokenRole(First, Role::OpeningBraceToken)
		);
	}

	/**
	 * 制御構文の見出で本体のブロックと紛れる式を持つかの判定関数
	 * Go の型名の複合リテラル，Rust の構造体の式，Swift の後置のクロージャは，括弧を外すと `{` が本体の開きと読まれる
	 * 実引数・配列・添字・文字列等の区切の中は紛れない
	 * 要素１つの括弧と Swift の前置の演算子の被演算子の括弧は，外れ得る為に透過して見る
	 * 計算量：判定対象のノード数 N，祖先深度 H，テキスト総長 L に対し期待値 O(N H + N + L)
	 * @param Syntax 構文モデル
	 * @param Node 判定する式
	 * @return 紛れる式を区切の括弧の外に持てば `true`
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool SyntaxNormalizer::HasBodyLikeExpr(const SyntaxModel &Syntax, const NodeId Node) {
		// 透過する Swift の前置の演算子の被演算子の括弧
		std::unordered_set<NodeId> PrefixArgs;
		// 言語毎の紛れる式の探索結果の返戻
		return Core::SyntaxModel::HasStoppedTraversal(
			Syntax,
			Node,
			[&](const NodeId Target) -> WalkStep {
				// 紛れる式で止める事の返戻
				if(
					Syntax.Is(Target, Role::NamedCompositeBody) &&
					Syntax.Is(Syntax.GetFieldChild(Target, ChildRole::Type), Role::CompositeTypeName) || Syntax.Is(Target, Role::HeaderBodyLikeVal)
				) return WalkStep::Stop;
				if(Syntax.Is(Target, Role::OperatorCall)) if(const NodeId Args = GetPrefixCallArgs(Syntax, Target); Args != NoNode) {
					PrefixArgs.insert(Args);
				}
				const NodeId First = Syntax.GetChild(Target, 0);
				const bool IsTransparent = Syntax.Is(Target, Role::ValParens) ||
				Syntax.GetNamedChildCount(Target) == 1 && (Syntax.Is(Target, Role::TransparentTupleParen) || PrefixArgs.contains(Target));
				// 区切の括弧・引用符のトークンで始まるノード（透過する括弧を除く）の中へ降りない事の返戻
				return Target == Node || First == NoNode || Syntax.IsNamed(First) || IsTransparent ||
				!Syntax.IsEdgeCharRole(Syntax.GetFirstEdge(First), Role::BodyDelimiterLead) ? WalkStep::Descend : WalkStep::Skip;
			}
		);
	}

	/**
	 * 前置の演算子と被演算子の先頭の記号の融合判定関数
	 * 計算量：固定の文法の部品又は規則の表に対し O(1)
	 * @param Rules トークンの融合の規則
	 * @param Operator 前置の演算子の末尾の文字
	 * @param Operand 被演算子の先頭の文字
	 * @return 密着させると１トークンへ融合するなら `true`
	 */
	bool SyntaxNormalizer::HasPrefixOperatorFusion(
		const SyntaxRules &Rules,
		const unsigned char Operator,
		const unsigned char Operand
	) {
		// 融合するかの返戻（Rust・Ruby・Python は `--` / `++` のトークンを持たず，Rust は `&&` を参照の重なりへ分けて読む）
		const char Pair[] { static_cast<char>(Operator), static_cast<char>(Operand) };
		// 投影された融合の組に含まれるかの返戻
		return Rules.FusedPrefixPairs.contains(std::string_view(Pair, 2));
	}

	/**
	 * 括弧の規則と同じ寿命の要約の準備関数
	 * 計算量：最初の参照又は再使用するノード数 N に対し O(N)，準備済なら O(1)
	 * @param Syntax 構文モデル
	 * @throws std::bad_alloc 共有する表を確保出来ない場合
	 */
	void SyntaxNormalizer::PrepareParenAncestors(const SyntaxModel &Syntax) const {
		#if defined(ShaveFormatRefFacts)
		ParenAncestors = ParenAncestorMemo();
		#endif
		if(ParenAncestors.Owner != &Syntax || !Syntax.EditNormalizationFacts().Parens) {
			ParenAncestors = ParenAncestorMemo();
			ParenAncestors.Owner = &Syntax;
		}
		if(!Syntax.EditNormalizationFacts().Parens) Syntax.EditNormalizationFacts().Parens.emplace();
		// 終了
		return;
	}

	/**
	 * 外したラッパーの現在の中身の取得関数
	 * 計算量：接続の検査・経路短縮の総数 A に対し O(A)，同じ接続の経路は共有する
	 * @param Syntax 構文モデル
	 * @param Node 外した可能性の在るノード
	 * @return 現存する中身，又は対応を持たない場合の `NoNode`
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 接続の作業列を確保出来ない場合
	 * @throws std::length_error 作業列の要素数上限を超える場合
	 */
	NodeId SyntaxNormalizer::GetParenReplacement(const SyntaxModel &Syntax, NodeId Node) const {
		std::vector<NodeId> &Path = ParenAncestors.AliasPath;
		Path.clear();
		while(Node != NoNode && Syntax.IsRemovedNode(Node)) {
			Syntax.Charge(1);
			// 対応の表に無い除去済ノードの返戻
			if(Node >= ParenAncestors.Replacements.size()) return NoNode;
			Path.push_back(Node);
			Node = ParenAncestors.Replacements[Node].Content;
		}
		for(const NodeId Visited : Path) {
			Syntax.Charge(1);
			ParenAncestors.Replacements[Visited].Content = Node;
		}
		// 現在の中身の返戻
		return Node;
	}

	/**
	 * 祖先の停止条件の判定関数
	 * 計算量：優先順位の共有照会と先頭の名前付の子迄の検査数 C に対し期待値 O(C + 1)
	 * @param Syntax 構文モデル
	 * @param Child 祖先の直下の子
	 * @param Parent 判定する祖先
	 * @param Kind 共有する停止条件
	 * @return 此の接続で探索を止める場合に `true`
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 優先順位の表を確保出来ない場合
	 * @throws std::length_error 表の要素数上限を超える場合
	 * @throws std::out_of_range ノード又はテキストの位置が範囲外の場合
	 */
	bool SyntaxNormalizer::IsParenAncestorBoundary(
		const SyntaxModel &Syntax,
		const NodeId Child,
		const NodeId Parent,
		const ParenAncestorKind Kind
	) const {
		// 根の外側では全ての探索を止める
		if(Parent == NoNode) return true;
		switch(Kind) {
		case ParenAncestorKind::BareStatement:
			return Syntax.Is(Parent, Role::StmtSeq) || Syntax.GetParentOf(Parent) == NoNode || Syntax.Is(Parent, Role::KeywordBlock);
		case ParenAncestorKind::CasePattern:
			return !Syntax.Is(Parent, Role::CasePattern);
		case ParenAncestorKind::CountedLoop:
			return Syntax.Is(Parent, Role::CountedLoop) || Syntax.Is(Parent, Role::StmtHost) ||
			Syntax.Is(Parent, Role::MembershipExprBoundary) && !Syntax.Is(Parent, Role::GroupingParen);
		case ParenAncestorKind::Following:
			return Parent == Syntax.GetRoot() || Syntax.Is(Parent, Role::StmtHost) || Syntax.GetNextSibling(Child) != NoNode;
		case ParenAncestorKind::GroupingParen:
			return !Syntax.Is(Parent, Role::GroupingParen);
		case ParenAncestorKind::OppositeLeft:
		case ParenAncestorKind::OppositeRight:
			{
				if(Syntax.Is(Parent, Role::GroupingParen)) return true;
				const int Prec = GetOperatorPrec(Syntax, Parent, Syntax.EditNormalizationFacts().Parens->Prec);
				if(!Prec) return true;
				const bool IsLeft = Syntax.GetNamedChild(Parent, 0) == Child;
				// 右隣を持たない前置単項は右側の探索に含めない
				return Kind == ParenAncestorKind::OppositeLeft ? !IsLeft : IsLeft && Syntax.IsNamed(Syntax.GetChild(Parent, 0));
			}
		case ParenAncestorKind::PostfixReceiver:
			return !(Syntax.Is(Parent, Role::PostfixExprHost) || Syntax.Is(Parent, Role::NonNullAssertion)) ||
			Syntax.GetNamedChild(Parent, 0) != Child;
		case ParenAncestorKind::PostfixUnary:
			return !Syntax.Is(Parent, Role::PostfixUnaryExpr);
		case ParenAncestorKind::PtrType:
			return !Syntax.Is(Parent, Role::PtrType);
		case ParenAncestorKind::ReturnHost:
			return Syntax.Is(Parent, Role::ReturnValHost);
		case ParenAncestorKind::StatementLead:
			return Syntax.Is(Parent, Role::ArrowFunction) || Syntax.Is(Parent, Role::Export) || Syntax.Is(Parent, Role::Iteration) ||
			Syntax.Is(Parent, Role::UpdateExpr) && Syntax.Is(Syntax.GetParentOf(Parent), Role::ExprStmt) ||
			Syntax.GetChild(Parent, 0) != Child || Syntax.Is(Parent, Role::ExprStmt);
		case ParenAncestorKind::TypeRightBoundary:
		case ParenAncestorKind::TypeRightCallable:
			return Syntax.Is(Parent, Role::ConditionalType) || Syntax.GetChild(Parent, Syntax.GetChildCount(Parent) - 1) != Child ||
			Kind == ParenAncestorKind::TypeRightCallable && Syntax.Is(Parent, Role::CallableType);
		case ParenAncestorKind::Count:
			break;
		}
		// 番兵では探索を止める
		return true;
	}

	/**
	 * 同じ停止条件の祖先の要約の取得関数
	 * 外した括弧で停止して居た要約は，其の括弧の共有要約へ接続する
	 * 計算量：表の要素数 M，接続の検査・更新総数 U，照会数 Q に対し O(M + U + Q)，左結合の括弧の連鎖長 N では U = O(N)
	 * @param Syntax 構文モデル
	 * @param Node 探索を始める子
	 * @param Kind 共有する停止条件
	 * @return 停止した祖先と其の直下の子
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 共有する表又は作業列を確保出来ない場合
	 * @throws std::length_error 表又は作業列の要素数上限を超える場合
	 * @throws std::out_of_range ノード又はテキストの位置が範囲外の場合
	 */
	SyntaxNormalizer::ParenAncestorBoundary SyntaxNormalizer::GetParenAncestorOf(
		const SyntaxModel &Syntax,
		const NodeId Node,
		const ParenAncestorKind Kind
	) const {
		PrepareParenAncestors(Syntax);
		Syntax.Charge(1);
		const NodeId DirectParent = Syntax.GetParentOf(Node);
		// 直上で停止する照会には経路の表を作らない
		if(IsParenAncestorBoundary(Syntax, Node, DirectParent, Kind)) return { Node, DirectParent, true };
		// 条件の表は必要な物だけノード数迄伸ばす
		if(ParenAncestors.Tables.empty()) {
			Syntax.Charge(static_cast<size_t>(ParenAncestorKind::Count));
			ParenAncestors.Tables.resize(static_cast<size_t>(ParenAncestorKind::Count));
		}
		std::vector<ParenAncestorBoundary> &Table = ParenAncestors.Tables[static_cast<size_t>(Kind)];
		if(Table.size() < Syntax.GetNodeCount()) {
			if(Table.capacity() < Syntax.GetNodeCount()) Syntax.Charge(Table.size());
			Syntax.Charge(Syntax.GetNodeCount() - Table.size());
			Table.resize(Syntax.GetNodeCount());
		}
		std::vector<NodeId> &Path = ParenAncestors.Path;
		Path.clear();
		ParenAncestorBoundary Result;
		NodeId At = Node;
		while(At != NoNode) {
			Syntax.Charge(1);
			NodeId Child = At;
			if(Table[At].IsKnown) {
				Result = Table[At];
				if(Result.Parent != NoNode && Syntax.IsRemovedNode(Result.Parent)) {
					Path.push_back(At);
					At = Result.Parent;
					continue;
				}
				Child = GetParenReplacement(Syntax, Result.Child);
				if(
					Child != NoNode && Syntax.GetParentOf(Child) == Result.Parent && IsParenAncestorBoundary(Syntax, Child, Result.Parent, Kind)
				) {
					Result.Child = Child;
					break;
				}
			} else Child = GetParenReplacement(Syntax, At);
			Path.push_back(At);
			// 対応を持たない除去は，変更範囲の失効後に現在の候補から読む
			if(Child == NoNode) Child = Node;
			const NodeId Parent = Syntax.GetParentOf(Child);
			if(IsParenAncestorBoundary(Syntax, Child, Parent, Kind)) {
				Result = { Child, Parent, true };
				break;
			}
			At = Parent;
		}
		for(const NodeId Visited : Path) {
			Syntax.Charge(1);
			Table[Visited] = Result;
		}
		// 共有した停止位置の返戻
		return Result;
	}

	/**
	 * 括弧の書換の影響範囲の要約の更新関数
	 * アンラップでは外したラッパーの接続を保持し，其の他の書換では関係する親の範囲だけ失効させる
	 * 計算量：追加した計画の長さ P と変更範囲のノード数 S に対し O(P + S)，接続表の初回だけノード数 N に対し O(N)
	 * @param Syntax 書換の適用前の構文モデル
	 * @param Plan 此の収集で作った書換の計画
	 * @param FirstStep 此の収集が追加した先頭の書換の位置
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 接続の表又は作業列を確保出来ない場合
	 * @throws std::length_error 表又は作業列の要素数上限を超える場合
	 */
	void SyntaxNormalizer::UpdateParenAncestors(const SyntaxModel &Syntax, const RewritePlan &Plan, const size_t FirstStep) const {
		// 要約又は書換の無い場合の終了
		if(
			ParenAncestors.Owner != &Syntax || !Syntax.EditNormalizationFacts().Parens || ParenAncestors.Tables.empty() ||
			FirstStep >= Plan.Steps.size()
		) return;
		std::vector<ParenReplacement> &Replacements = ParenAncestors.Replacements;
		if(Replacements.size() < Syntax.GetNodeCount()) {
			if(Replacements.capacity() < Syntax.GetNodeCount()) Syntax.Charge(Replacements.size());
			Syntax.Charge(Syntax.GetNodeCount() - Replacements.size());
			Replacements.resize(Syntax.GetNodeCount());
		}
		std::vector<NodeId> &Pending = ParenAncestors.Pending, &Invalidated = ParenAncestors.Invalidated;
		Pending.clear();
		Invalidated.clear();
		for(size_t Idx = FirstStep; Idx < Plan.Steps.size(); ++Idx) {
			Syntax.Charge(1);
			const RewriteStep &Step = Plan.Steps[Idx];
			if(Step.Kind == RewriteKind::Unwrap) {
				const NodeId Content = Syntax.GetNamedChildCount(Step.Target) == 1 ?
				Syntax.GetNamedChild(Step.Target, 0) :
				Syntax.GetChildCount(Step.Target) == 3 ? Syntax.GetChild(Step.Target, 1) : NoNode;
				Replacements[Step.Target].Content = Content;
				// 名前付の区分も保つ除去は，停止位置の接続だけを更新する
				if(Content != NoNode && Syntax.IsNamed(Step.Target) == Syntax.IsNamed(Content)) continue;
			}
			if(
				Step.Kind == RewriteKind::Retarget ||
				Step.Kind == RewriteKind::Move && !Step.DstFields && Step.Dst == Syntax.GetParentOf(Step.Target) &&
				Step.Idx == Syntax.GetNode(Step.Target).Idx
			) continue;
			// 子の配置又は役割の変わる親の範囲を収集する
			for(const NodeId Target : { Step.Target, Step.Dst }) {
				if(Target == NoNode) continue;
				const NodeId Parent = Syntax.GetParentOf(Target);
				Pending.push_back(Parent == NoNode ? Target : Parent);
			}
		}
		while(!Pending.empty()) {
			const NodeId Node = Pending.back();
			Pending.pop_back();
			Syntax.Charge(1);
			if(Replacements[Node].IsInvalidated) continue;
			Replacements[Node].IsInvalidated = true;
			Invalidated.push_back(Node);
			for(std::vector<ParenAncestorBoundary> &Table : ParenAncestors.Tables) {
				Syntax.Charge(1);
				if(Node >= Table.size() || !Table[Node].IsKnown) continue;
				Syntax.Charge(1);
				Table[Node] = ParenAncestorBoundary();
			}
			const std::span<const NodeId> Children = Syntax.GetChildrenOf(Node);
			Pending.insert(Pending.end(), Children.begin(), Children.end());
		}
		for(const NodeId Node : Invalidated) {
			Syntax.Charge(1);
			Replacements[Node].IsInvalidated = false;
		}
		// 終了
		return;
	}

	/**
	 * 被演算子の逆側で先に結合する演算子の優先順位の取得関数
	 * 対象の演算子を含む部分式が逆側の被演算子に置かれる最初の祖先を，同じ括弧内で遡って探す
	 * 右隣を探す時は，右隣を持たない前置単項を透過する
	 * 計算量：共有表の要素数 M，接続の検査・更新総数 U，照会数 Q，テキスト総長 L に対し期待値 O(M + U + Q + L)
	 * @param Syntax 構文モデル
	 * @param OperatorParent 括弧の層を透過した演算子の祖先
	 * @param IsLeftOperand 被演算子が左か
	 * @return 逆側の演算子の優先順位（無ければ０）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	int SyntaxNormalizer::GetOppositePrecOf(
		const SyntaxModel &Syntax,
		const NodeId OperatorParent,
		const bool IsLeftOperand
	) const {
		const ParenAncestorBoundary Boundary =
		GetParenAncestorOf(Syntax, OperatorParent, IsLeftOperand ? ParenAncestorKind::OppositeLeft : ParenAncestorKind::OppositeRight);
		// 括弧又は根で隔てられない最初の逆側の演算子の優先順位の返戻
		return Boundary.Parent == NoNode || Syntax.Is(Boundary.Parent, Role::GroupingParen) ?
		0 :
		GetOperatorPrec(Syntax, Boundary.Parent, Syntax.EditNormalizationFacts().Parens->Prec);
	}

	/**
	 * 後置の式の受手を包む括弧の保持判定関数
	 * 原子式の受手は括弧を外しても結合を保てる（`(a.b).c` → `a.b.c`）
	 * マクロ展開・後置ラムダ・構築式等の読みを変える場合は，原子式でも括弧を保つ
	 * 計算量：判定対象のノード数 N，祖先深度 H，テキスト総長 L に対し期待値 O(N H + N + L)
	 * @param Syntax 構文モデル
	 * @param Paren 受手を包む括弧
	 * @param Host 後置の式
	 * @return 括弧を保持する場合 `true`
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool SyntaxNormalizer::ShouldPreservePostfixReceiver(const SyntaxModel &Syntax, const NodeId Paren, const NodeId Host) const {
		// 括弧の層を透過した中身
		NodeId Inner = Paren;
		while(Syntax.Is(Inner, Role::GroupingParen) && Syntax.GetNamedChildCount(Inner) == 1) Inner = Syntax.GetNamedChild(Inner, 0);
		if(Syntax.Is(Paren, Role::ConstructionReceiverParen)) {
			const NodeId Chain = GetParenAncestorOf(Syntax, Paren, ParenAncestorKind::PostfixReceiver).Child;
			if(Syntax.Is(Syntax.GetParentOf(Chain), Role::Construction)) {
				std::unordered_map<NodeId, bool> &Memo = Syntax.EditNormalizationFacts().Parens->Scan.ConstructionCalls;
				std::vector<NodeId> Pending;
				bool HasCall = false;
				for(NodeId Receiver = Inner; Receiver != NoNode; Receiver = Syntax.GetNamedChild(Receiver, 0)) {
					Syntax.Charge(1);
					if(const std::unordered_map<NodeId, bool>::const_iterator Known = Memo.find(Receiver); Known != Memo.end()) {
						HasCall = Known->second;
						break;
					}
					Pending.push_back(Receiver);
					// 呼出・オプショナルチェーンを構築の被呼出側へ変えない括弧の保持の返戻
					if(
						Syntax.Is(Receiver, Role::CallExpr) && !Syntax.Is(Syntax.GetFieldChild(Receiver, ChildRole::Args), Role::InterpolatedLiteral) ||
						Core::SyntaxModel::HasChildMatching(
							Syntax,
							Receiver,
							[&Syntax](const NodeId Child) -> bool {
								return !Syntax.IsNamed(Child) && Syntax.IsTokenRole(Child, Role::OptionalAccessToken);
							}
						) || Syntax.GetFirstNamedChildWithRole(Receiver, Role::OptionalChain) != NoNode
					) {
						HasCall = true;
						break;
					}
					if(!Syntax.Is(Receiver, Role::PostfixExprHost) && !Syntax.Is(Receiver, Role::NonNullAssertion)) break;
				}
				for(const NodeId Visited : Pending) Memo.emplace(Visited, HasCall);
				// 構築の結合を保つ括弧の返戻
				if(HasCall) return true;
			}
		}
		// 実引数の括弧を持たない後置ラムダを，受手の呼出の実引数へ取り込ませない
		if(Syntax.Is(Host, Role::PostfixCallHost) && Syntax.Is(Inner, Role::CallExpr)) {
			const NodeId Suffix = Syntax.GetLastNamedChild(Host);
			// 呼出結果と後置ラムダの結合を保つ括弧の保持の返戻
			if(
				Syntax.Is(Suffix, Role::CallSuffix) && Syntax.GetFirstNamedChildWithRole(Suffix, Role::ValArgs) == NoNode && (
					Syntax.GetFirstNamedChildWithRole(Suffix, Role::LambdaLiteral) != NoNode ||
					Syntax.GetFirstNamedChildWithRole(Suffix, Role::AnnotatedLambda) != NoNode
				)
			) return true;
		}
		// Rust の前置単項と左結合の `as` の型変換の受手に置ける事の返戻
		if(
			Syntax.Is(Host, Role::UnaryReceiverCast) && (Syntax.Is(Inner, Role::PrefixUnary) || Syntax.Is(Inner, Role::TrailingTypeCast))
		) return false;
		// 呼出の意味を変える名前・フィールドと，原子式でない受手の保持の返戻
		return Syntax.Is(Host, Role::PostfixCallHost) && Syntax.Is(Inner, Role::CalleeNeedsGrouping) || !IsPostfixAtom(Syntax, Inner);
	}

	/**
	 * TypeScript の型引数の閉じ `>` の直後に来る被演算子かの判定関数
	 * TypeScript の比較式を型引数付の式へ変える先頭字を判定する
	 * 内側が比較とシフトより強く結合し，括弧必須の式でもない時は，括弧を透過して内側の先頭を見る
	 * 計算量：判定対象のノード数 N，祖先深度 H，テキスト総長 L に対し期待値 O(N H + N + L)
	 * @param Syntax 構文モデル
	 * @param Operand 比較の右の被演算子
	 * @param Memo 優先順位の控え
	 * @return 整形後に `(` `\`` `+` `-` `[` で始まるなら `true`
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool SyntaxNormalizer::HasTypeArgOpener(
		const SyntaxModel &Syntax,
		NodeId Operand,
		std::unordered_map<NodeId, int> &Memo
	) const {
		// TypeScript の型引数の閉じ `>` の直後に来る被演算子かの判定
		while(Syntax.Is(Operand, Role::ValParens)) {
			const NodeId Inner = Syntax.GetNamedChild(Operand, 0);
			// 残る括弧で始まる事の返戻
			if(
				const int Prec = Inner == NoNode ? 0 : GetOperatorPrec(Syntax, Inner, Memo);
				Syntax.GetNamedChildCount(Operand) != 1 || Prec && Prec < 12 || Syntax.Is(Inner, Role::ParenInnerNoUnwrap)
			) return true;
			Operand = Inner;
		}
		// 型引数として開き得る先頭の字かの返戻
		return Operand != NoNode && Syntax.IsEdgeCharRole(Syntax.GetFirstEdge(Operand), Role::TypeArgOpeningLead);
	}

	/**
	 * 型を包む括弧の中身の取得関数
	 * Swift の要素は名タグの無い型１つ（`(x: Int)` は名タグ付の要素，`(repeat each T)` は型の並びの展開を要素に持つ組）
	 * 計算量：固定個数の子を直接引く為 O(1)
	 * @param Syntax 構文モデル
	 * @param Node 括弧の型のノード（Go・Kotlin・TypeScript の `parenthesized_type`，Rust・Swift の要素１つの `tuple_type`）
	 * @return 型１つを包む括弧なら其の型，其れ以外は `NoNode`
	 */
	NodeId SyntaxNormalizer::GetTypeParenContent(const SyntaxModel &Syntax, const NodeId Node) {
		// 開閉の括弧と中身の３つの子で成らない型の返戻
		if(
			Syntax.GetChildCount(Node) != 3 || !Syntax.IsTokenRole(Syntax.GetChild(Node, 0), Role::OpeningParenToken) ||
			!Syntax.IsTokenRole(Syntax.GetChild(Node, 2), Role::ClosingParenToken)
		) return NoNode;
		NodeId Inner = Syntax.GetChild(Node, 1);
		if(Syntax.Is(Node, Role::WrappedTypeItem)) {
			// 名タグ付・展開の要素の返戻
			if(!Syntax.Is(Inner, Role::TupleTypeItem) || Syntax.GetChildCount(Inner) != 1) return NoNode;
			Inner = Syntax.GetChild(Inner, 0);
			// 型の並びの展開の返戻
			if(Syntax.Is(Inner, Role::TypePackExpansion)) return NoNode;
		}
		// 名前付の中身の返戻
		return Syntax.IsNamed(Inner) ? Inner : NoNode;
	}

	/**
	 * 実引数の先頭の読みを保つ括弧が必要かの判定関数
	 * @param Syntax 現在の構文
	 * @param Node 実引数に置く式
	 * @param HasDelimiter 実引数の開き又はカンマが直前に在るか
	 * @return 語の否定又は演算子と紛れるトークンで始まるなら `true`
	 * @throws FormatLimitExceeded トークン端の取得が作業予算を超える場合
	 * @throws std::bad_alloc 左端の探索に用いるメモリを確保出来ない場合
	 * @throws std::length_error 左端の探索列が要素数上限を超える場合
	 * @throws std::out_of_range ノードの参照が範囲外の場合
	 */
	bool SyntaxNormalizer::ShouldWrapArg(const SyntaxModel &Syntax, const NodeId Node, const bool HasDelimiter) {
		const NodeId Lead = Syntax.GetFirstEdge(Node).Leaf;
		// 区切だけでは置けない語の否定と，区切が必要なトークンの判定の返戻
		return Lead != NoNode &&
		(Syntax.IsWord(Lead, Word::Negation) || !HasDelimiter && Syntax.GetNode(Lead).Facts.Has(ConstraintKind::NeedsArgParens));
	}

	/**
	 * グループとして実引数へ置く制御値の判定関数
	 * 制御文の見出と値を取る語の修飾子を，返戻の実引数へ読み替えない
	 * 計算量：フィールドを調べる子の数 C に対し O(C)
	 * @param Syntax 構文モデル
	 * @param Node 実引数に置く値の構造
	 * @return グループの括弧と語の後の空白を要するなら `true`
	 * @throws FormatLimitExceeded フィールドの照会が作業予算を超える場合
	 * @throws std::out_of_range ノード又はフィールドが範囲外の場合
	 */
	bool SyntaxNormalizer::IsControlValArg(const SyntaxModel &Syntax, const NodeId Node) {
		// 後置の救済式である場合の返戻
		if(Syntax.Is(Node, Role::RescueExpr)) return true;
		// 制御値でない構造での返戻
		if(!Syntax.Is(Node, Role::KeywordBlock) && !Syntax.Is(Node, Role::ParenInnerNoUnwrap)) return false;
		Syntax.Charge(Syntax.GetChildCount(Node));
		// 条件を持つ制御値かの返戻
		return Syntax.GetFieldChild(Node, ChildRole::Cond) != NoNode;
	}

	/**
	 * 制御式の括弧を値に取る語の取得関数
	 * 裸の実引数の制御式と修飾子形は，括弧と語との空白を共に保ち，語自身の修飾子に読み替えない
	 * 計算量：値の左端の深度 D とフィールドの照会量 C に対し O(D + C)
	 * @param Syntax 構文モデル
	 * @param Paren 括弧のノード
	 * @param Inner 括弧の層を透過した中身
	 * @return 値を取る語のノード（対象の文脈でなければ無）
	 * @throws FormatLimitExceeded 左端の探索が作業予算を超える場合
	 * @throws std::out_of_range 照会するノードが表に無い場合
	 */
	NodeId SyntaxNormalizer::GetControlValParenOwner(const SyntaxModel &Syntax, const NodeId Paren, const NodeId Inner) {
		const NodeId Args = Syntax.GetParentOf(Paren), Owner = Syntax.GetParentOf(Args);
		// 値を取る語の裸の実引数だけを対象とする
		if(
			!Syntax.Is(Args, Role::BareCallArgs) || !Syntax.Is(Owner, Role::ValJump) ||
			Syntax.IsTokenRole(Syntax.GetChild(Args, 0), Role::OpeningParenToken)
		) return NoNode;
		NodeId Head = Inner;
		while(!IsControlValArg(Syntax, Head)) {
			const NodeId Child = Syntax.GetChild(Head, 0);
			// 制御値を含まない左端での終了
			if(!Syntax.IsNamed(Child)) return NoNode;
			Syntax.Charge(1);
			Head = Child;
		}
		// 制御値から始まる式の全体を包む括弧の所有者の返戻
		return Owner;
	}

	/**
	 * 式の括弧を保つ必要が在るかの判定関数（整形仕様 5.2）
	 * 親の構文が要する括弧・結合を変える括弧・読みを変える括弧を保ち，其の他の括弧を外す
	 * 計算量：判定対象のノード数 N，祖先深度 H，テキスト総長 L に対し期待値 O(N H + N + L)
	 * @param Syntax 構文モデル
	 * @param Paren 括弧のノード
	 * @param Inner 括弧の層を透過した中身
	 * @param ControlOwner グループの値を取る語の所有者（無ければ `NoNode`）
	 * @param Memo 優先順位の控え
	 * @param AngleMemo 山括弧に関わるトークンの有無の控え
	 * @param ScanMemo 端と連鎖の判定の控え
	 * @return 括弧を保つなら `true`
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool SyntaxNormalizer::ShouldPreserveExprParen(
		const SyntaxModel &Syntax,
		const NodeId Paren,
		const NodeId Inner,
		const NodeId ControlOwner,
		std::unordered_map<NodeId, int> &Memo,
		AngleFacts &AngleMemo,
		ParenScanMemo &ScanMemo
	) const {
		// 本体と紛れる構文の保持判定だけが読む制御見出の所属
		// 計算量：初回の祖先数 H に対し期待値 O(H + 1)，同じ祖先の再照会は期待値 O(1)
		// @return 最内の制御構文と本体の境界から確定した見出の所属
		// @throws FormatLimitExceeded 祖先又は共有表の参照が作業予算を超える場合
		// @throws std::bad_alloc 祖先の列又は共有表を確保出来ない場合
		// @throws std::length_error 祖先の列又は共有表の要素数上限を超える場合
		// @throws std::out_of_range 祖先のノードが表に無い場合
		const auto IsInBraceHeader = [&Syntax, Paren]() -> bool {
			if(Syntax.HasRoleIdx && Syntax.IndexedRoles.test(static_cast<size_t>(Role::BodyAmbiguousControl))) {
				Syntax.Charge(1);
				// 見出の曖昧性を持たない構文の返戻
				if(Syntax.GetNodesWith(Role::BodyAmbiguousControl).empty()) return false;
			}
			std::unordered_map<NodeId, bool> &BraceHeaders = Syntax.EditNormalizationFacts().Parens->BraceHeaders;
			std::vector<NodeId> Path;
			NodeId Up = Syntax.GetParentOf(Paren);
			while(Up != NoNode && !BraceHeaders.contains(Up)) {
				Syntax.Charge(1);
				Path.push_back(Up);
				if(Syntax.Is(Up, Role::BodyAmbiguousControl) || Syntax.Is(Up, Role::ScopeBody)) break;
				Up = Syntax.GetParentOf(Up);
			}
			const std::unordered_map<NodeId, bool>::const_iterator Known = BraceHeaders.find(Up);
			if(Known != BraceHeaders.end()) Syntax.Charge(1);
			bool IsHeader = Known != BraceHeaders.end() && Known->second;
			for(std::vector<NodeId>::const_reverse_iterator It = Path.rbegin(); It != Path.rend(); ++It) {
				Syntax.Charge(1);
				if(Syntax.Is(*It, Role::BodyAmbiguousControl)) IsHeader = true;
				else if(Syntax.Is(*It, Role::ScopeBody)) IsHeader = false;
				BraceHeaders.emplace(*It, IsHeader);
			}
			// 共通祖先と共有する所属の返戻
			return IsHeader;
		};
		// 式の括弧を保つ必要が在るかの判定
		const NodeId Parent = Syntax.GetParentOf(Paren), Prev = Syntax.GetPrevSibling(Paren);
		const NodeId Next = Syntax.GetNextSibling(Paren);
		// 値を取る語の制御式，返戻に置く代入と，裸の実引数の先頭の読みを保つ括弧の保持の返戻
		if(
			ControlOwner != NoNode || (Syntax.Is(Inner, Role::AssignmentOperation) || Syntax.Is(Inner, Role::CompoundAssignment)) &&
			Syntax.Is(Parent, Role::ArgSeq) && Syntax.Is(Syntax.GetParentOf(Parent), Role::ReturnKeyword) ||
			Syntax.Is(Parent, Role::BareCallArgs) && ShouldWrapArg(
				Syntax,
				Inner,
				Syntax.IsTokenRole(Syntax.GetChild(Parent, 0), Role::OpeningParenToken) || Syntax.GetNamedChild(Parent, 0) != Paren
			)
		) return true;
		if(Syntax.Is(Inner, Role::NamedVal)) {
			// Python 3.8 で代入式のラッパーが必須である位置かの返戻
			return !Syntax.Is(Parent, Role::UnparenthesizedBindingValSlot) &&
			!(Syntax.Is(Parent, Role::OptionalCondParen) && Syntax.HasField(Paren, ChildRole::Cond));
		}
		// 最後のトークンの葉の控付の探索
		const auto LastLeafOf = [&Syntax, &ScanMemo](NodeId Node) -> NodeId {
			std::vector<NodeId> Path;
			while(Node < Syntax.GetNodeCount() && !ScanMemo.LastLeaf.contains(Node)) {
				Syntax.Charge(1);
				Path.push_back(Node);
				const std::span<const NodeId> Children = Syntax.GetChildrenOf(Node);
				if(Children.empty()) break;
				Node = Children.back();
			}
			if(ScanMemo.LastLeaf.contains(Node)) {
				Syntax.Charge(1);
				Node = ScanMemo.LastLeaf.at(Node);
			} else if(Node >= Syntax.GetNodeCount()) Node = NoNode;
			for(const NodeId Visited : Path) ScanMemo.LastLeaf.emplace(Visited, Node);
			// 最後の葉の返戻
			return Node;
		};
		// データのマーカー・マクロ展開・宣言との読違を防ぐ括弧の保持の返戻
		if(
			!Syntax.GetRules().StandaloneDataMarker.empty() && Syntax.Is(Inner, Role::Identifier) &&
			Syntax.GetLexemeOf(Inner) == Syntax.GetRules().StandaloneDataMarker && Parent == Syntax.GetRoot() ||
			Syntax.GetNode(Paren).Facts.Has(ConstraintKind::MacroSensitiveParen) ||
			Syntax.GetRules().CanCallFormsDeclare && IsParsedAsDecl(Syntax, Paren, Inner, ScanMemo)
		) return true;
		if(Syntax.Is(Paren, Role::CommandArgParen)) {
			const NodeId Args = Syntax.GetFieldChild(Inner, ChildRole::Args);
			// 実引数の終端・数値の符号・代入の右側の `not` を保つラッパーの返戻
			if(
				const bool IsCommand = Syntax.Is(Inner, Role::ReceiverCall) && Args != NoNode &&
				!Syntax.IsTokenRole(Syntax.GetChild(Args, 0), Role::OpeningParenToken);
				IsCommand && (
					Syntax.Is(Parent, Role::ElementRef) || Syntax.Is(Parent, Role::ReceiverCall) || Syntax.Is(Parent, Role::ConditionalOperator)
				) || Syntax.Is(Inner, Role::NumLiteral) && Syntax.Is(Parent, Role::ReceiverCall) &&
				Syntax.Is(Syntax.GetParentOf(Parent), Role::UnaryVal) ||
				Syntax.Is(Parent, Role::UnaryVal) && Syntax.IsWord(Syntax.GetChild(Parent, 0), Word::Negation) &&
				!Syntax.Is(Syntax.GetParentOf(Parent), Role::CommandStmtHost)
			) return true;
			// 数値で始まる累乗と前置の符号の結合の判定
			if(
				Syntax.Is(Parent, Role::UnaryVal) && (
					Syntax.GetText(Syntax.GetChild(Parent, 0)) == Syntax.GetRules().Operators.Minus ||
					Syntax.GetText(Syntax.GetChild(Parent, 0)) == Syntax.GetRules().Operators.Plus
				) && Syntax.Is(Inner, Role::Binary)
			) {
				NodeId First = Inner;
				while(Syntax.GetChildCount(First)) First = Syntax.GetChild(First, 0);
				// 数値の前の符号と累乗の結合を保つラッパーの返戻
				if(Syntax.Is(First, Role::NumLiteral)) return true;
			}
		}
		if(Syntax.Is(Paren, Role::ConstructionReceiverParen)) {
			// 括弧を透過した先頭のトークンと，其の後の構文上のトークン
			std::unordered_map<NodeId, NodeId> &LeadMemo = Syntax.EditNormalizationFacts().Parens->Scan.PostfixLead;
			std::vector<NodeId> Pending;
			NodeId Lead = Inner;
			while(true) {
				Syntax.Charge(1);
				if(const std::unordered_map<NodeId, NodeId>::const_iterator Known = LeadMemo.find(Lead); Known != LeadMemo.end()) {
					Lead = Known->second;
					break;
				}
				Pending.push_back(Lead);
				if(!Syntax.GetChildCount(Lead)) break;
				Lead = Syntax.Is(Lead, Role::GroupingParen) ? Syntax.GetNamedChild(Lead, 0) : Syntax.GetChild(Lead, 0);
			}
			for(const NodeId Visited : Pending) LeadMemo.emplace(Visited, Lead);
			// 取り除く括弧の後に続くノードの取得
			const auto Following = [&](const NodeId Node) -> NodeId {
				const ParenAncestorBoundary Boundary = GetParenAncestorOf(Syntax, Node, ParenAncestorKind::Following);
				// 後続のノードの返戻
				return Syntax.GetNextSibling(Boundary.Child);
			};
			NodeId AfterHead = Following(Lead);
			while(Syntax.IsTokenRole(AfterHead, Role::ClosingParenToken)) AfterHead = Following(AfterHead);
			// 先頭の語の後の最初のトークン（非同期の関数の宣言の語か，束縛の後の括弧かを見る）
			const NodeId AfterLeaf = Syntax.GetFirstEdge(AfterHead).Leaf;
			const bool IsDecl = Syntax.IsWord(Lead, Word::StmtDeclHead) ||
			Syntax.IsWord(Lead, Word::AsyncModifier) && Syntax.IsWord(AfterLeaf, Word::StmtDeclHead);
			const bool IsLetBracket = Syntax.IsWord(Lead, Word::ReassignableBinding) &&
			Syntax.IsTokenRole(Syntax.GetFirstEdge(AfterHead).Leaf, Role::OpeningBracketToken);
			// 指示文へ変わる文字列を包む括弧の保持の返戻
			if(Syntax.Is(Parent, Role::ExprStmt) && Syntax.Is(Inner, Role::QuotedStr)) return true;
			const ParenAncestorBoundary LeadBoundary = GetParenAncestorOf(Syntax, Paren, ParenAncestorKind::StatementLead);
			const NodeId LeadHost = LeadBoundary.Parent, LeadChild = LeadBoundary.Child;
			// 本体のブロック・宣言・反復の区切へ変わる括弧の判定
			if(Syntax.Is(LeadHost, Role::ArrowFunction)) {
				// 式の本体をブロックへ変えない括弧の保持の返戻
				if(Syntax.GetFieldChild(LeadHost, ChildRole::Body) == LeadChild && Syntax.IsTokenRole(Lead, Role::OpeningBraceToken)) {
					// 式の本体を保つ括弧の返戻
					return true;
				}
			} else if(Syntax.Is(LeadHost, Role::Export)) {
				// 公開する式を宣言へ変えない括弧の保持の返戻
				if(IsDecl) return true;
			} else if(Syntax.Is(LeadHost, Role::Iteration)) {
				// 反復の束縛と紛れない括弧の保持の返戻
				if(
					Syntax.GetFieldChild(LeadHost, ChildRole::Left) == LeadChild && (
						IsLetBracket || (Syntax.IsWord(Lead, Word::ReassignableBinding) || Syntax.IsWord(Lead, Word::AsyncModifier)) &&
						Syntax.IsWord(Syntax.GetNextSibling(LeadChild), Word::IterationOf)
					)
				) return true;
			// 文頭の解釈を保つ括弧の保持の返戻
			} else if(
				Syntax.Is(LeadHost, Role::ExprStmt) && Syntax.GetChild(LeadHost, 0) == LeadChild &&
				!(Syntax.Is(LeadHost, Role::UpdateExpr) && Syntax.Is(Syntax.GetParentOf(LeadHost), Role::ExprStmt)) &&
				(Syntax.IsTokenRole(Lead, Role::OpeningBraceToken) || IsDecl || IsLetBracket)
			) return true;
			// 型をアサーションした代入先の括弧又は実引数の囲みの保持の返戻
			if(
				Syntax.Is(Inner, Role::TypeAssertion) && (Syntax.Is(Parent, Role::Assignment) || Syntax.Is(Parent, Role::Iteration)) &&
				Syntax.GetFieldChild(Parent, ChildRole::Left) == Paren || Syntax.GetNode(Paren).Facts.Has(ConstraintKind::CallArguments)
			) return true;
			const NodeId Follower = Following(Paren);
			if(
				const LexicalEdge FollowingEdge = Syntax.GetFirstEdge(Follower);
				Syntax.IsEdgeCharRole(FollowingEdge, Role::InstantiationContinuationLead) || FollowingEdge.Leaf != NoNode &&
				(Syntax.IsEdgeRole(FollowingEdge, Role::ExclamationToken) || Syntax.IsEdgeRole(FollowingEdge, Role::OptionalAccessToken)) &&
				Syntax.GetLastEdge(Follower).Leaf == FollowingEdge.Leaf
			) for(NodeId Edge = Inner; Edge != NoNode; Edge = GetRightEdgeChild(Syntax, Edge)) {
				// 型引数の具体化を比較や呼出へ変える括弧の保持の返戻
				if(Syntax.Is(Edge, Role::Instantiation)) return true;
			}
			// `for` の初期化部の `in` を反復の区切へ変えない
			const ParenAncestorBoundary LoopBoundary = GetParenAncestorOf(Syntax, Paren, ParenAncestorKind::CountedLoop);
			// 初期化部の `in` を反復の区切へ変えない括弧の保持の返戻
			if(Syntax.Is(LoopBoundary.Parent, Role::CountedLoop)) if(
				Syntax.GetNode(LoopBoundary.Child).Idx < 3 && Core::SyntaxModel::HasStoppedTraversal(
					Syntax,
					Inner,
					[&](const NodeId Node) -> WalkStep {
						// 保護する `in` の発見時の返戻
						if(Syntax.IsWord(Syntax.GetFieldChild(Node, ChildRole::Operator), Word::In)) return WalkStep::Stop;
						// 文法上 `in` を許す区切の内側へ降りない事の返戻
						return !Syntax.Is(Node, Role::GroupingParen) && Syntax.Is(Node, Role::MembershipExprBoundary) ?
						WalkStep::Skip :
						WalkStep::Descend;
					}
				)
			) return true;
		}
		// 型と式の文法の境界と，制限された被演算子の読みを保つ括弧の返戻
		if(
			Syntax.Is(Parent, Role::TypeSyntax) || Syntax.Is(Parent, Role::RestrictedExprOperand) && (
				Syntax.Is(Parent, Role::ComprehensionFilter) &&
				(Syntax.Is(Inner, Role::ConditionalExpr) || Syntax.Is(Inner, Role::GreedyTailExpr)) ||
				Syntax.Is(Parent, Role::Attribute) && Syntax.Is(Inner, Role::AttributeSensitiveNum) ||
				Syntax.Is(Parent, Role::AwaitVal) && (Syntax.Is(Inner, Role::AwaitVal) || Syntax.Is(Inner, Role::PrefixUnary))
			)
		) return true;
		if(Syntax.Is(Paren, Role::RefValParen)) {
			if(Syntax.Is(Parent, Role::ConditionalExpr) && Syntax.Is(Inner, Role::ConditionalExpr) || Syntax.IsWord(Prev, Word::TypeTest)) {
				// 結付と型の読みを保つ括弧の返戻
				return true;
			}
			if(Syntax.Is(Parent, Role::ReturnValParent)) {
				const NodeId Owner =
				Syntax.Is(Parent, Role::ReturnValHost) ? Parent : GetParenAncestorOf(Syntax, Paren, ParenAncestorKind::ReturnHost).Parent;
				// 参照を値へ変える括弧の保持の返戻
				if(Syntax.GetFirstNamedChildWithRole(Owner, Role::RefModifier) != NoNode) return true;
			}
		}
		const bool IsInDelimiter = !Syntax.IsNamed(Prev) && Syntax.IsWord(Prev, Word::In);
		// `in` の右辺はカンマ式を直接置ける事の返戻
		if(Syntax.Is(Parent, Role::Iteration) && IsInDelimiter) return false;
		// 親の構文の区切と受手の位置の判定
		const bool IsAfterOwnDelimiter = Syntax.Is(Parent, Role::ForEachHeader) ?
		IsInDelimiter || !Syntax.IsNamed(Prev) && Syntax.IsWord(Prev, Word::IterationOf) ||
		Syntax.IsTokenRole(Prev, Role::ColonToken) || Syntax.IsTokenRole(Prev, Role::OpeningParenToken) &&
		(Syntax.Is(Parent, Role::ForInStmt) || Syntax.Is(Parent, Role::DelimitedIterationTarget)) :
		(Syntax.IsTokenRole(Prev, Role::OpeningParenToken) || Syntax.IsTokenRole(Prev, Role::OpeningBracketToken)) &&
		!Syntax.Is(Parent, Role::ValCategoryType);
		const bool IsPostfixReceiver = (
			Syntax.Is(Parent, Role::NonNullAssertion) || Syntax.Is(Parent, Role::PostfixExprHost) ||
			Syntax.Is(Parent, Role::PostfixValOperation)
		) && Syntax.GetChild(Parent, 0) == Paren || Syntax.Is(Parent, Role::TypeSwitch) && Syntax.IsTokenRole(Next, Role::DotToken);
		const bool IsConstructible = Syntax.Is(Parent, Role::Construction) && !ShouldPreservePostfixReceiver(Syntax, Paren, Parent);
		// 文法が省略を許す見出と親自身の区切の直後は，親が要するラッパーの対象から除く
		// 原子の受手と構築式の被呼出側は，呼出等の読みを保てる時だけ同じ対象から除く
		// C# の `switch` の対象と C 系の基本型等への変換の値も，結合を変えずに括弧を外せる
		// 親の構文に必須のラッパー，実の組・ラベル・複文，又は `chan` で始まる受信値の括弧の保持の返戻
		if(
			(
				Syntax.Is(Parent, Role::StructuralParenBoundary) || Syntax.Is(Parent, Role::MeaningfulParenGroup) ||
				Syntax.Is(Parent, Role::ReceiverParenHost) || Syntax.Is(Parent, Role::PostfixAwait)
			) && !(
				Syntax.Is(Parent, Role::OptionalCondParen) || IsPostfixReceiver && !ShouldPreservePostfixReceiver(Syntax, Paren, Parent) ||
				IsAfterOwnDelimiter || IsConstructible || Syntax.Is(Parent, Role::UnwrappedSwitchSubject) ||
				Syntax.Is(Parent, Role::KeywordCastOperand) && IsKeywordTypeCast(Syntax, Parent)
			) || Syntax.GetNamedChildCount(Paren) > 1 || !Syntax.Is(Paren, Role::ValParens) && Core::SyntaxModel::HasChildMatching(
				Syntax,
				Paren,
				[&Syntax](const NodeId Child) -> bool {
					// 組・ラベル・複文の区切のトークンかの返戻
					return Syntax.IsTokenRole(Child, Role::CommaToken) || Syntax.IsTokenRole(Child, Role::ColonToken) ||
					Syntax.IsTokenRole(Child, Role::StmtTerminatorToken);
				}
			) || Syntax.Is(Parent, Role::ChannelReceive) && Syntax.IsTokenRole(Syntax.GetChild(Parent, 0), Role::ChannelReceiveToken) &&
			Syntax.IsWord(Syntax.GetChild(Inner, 0), Word::Channel)
		) return true;
		if(Syntax.Is(Inner, Role::RangeLike) && !Syntax.IsNamed(Syntax.GetChild(Inner, Syntax.GetChildCount(Inner) - 1))) {
			// 括弧の後のトークン（親に無ければ祖先の後）
			const NodeId Following = GetParenAncestorOf(Syntax, Paren, ParenAncestorKind::Following).Child;
			// 終端の無い範囲が後続を取り込む括弧（範囲を閉じる区切の前は除く）の保持の返戻
			if(
				const unsigned char Close = Following == NoNode ? '\0' : Syntax.GetFirstChar(Syntax.GetNextSibling(Following));
				Close == '\0' || Syntax.GetRules().OpenRangeClosers.find(Close) == std::string_view::npos
			) return true;
		}
		// 括弧の層を透過した演算子の祖先と，其の直下の被演算子
		const ParenAncestorBoundary OperatorBoundary = GetParenAncestorOf(Syntax, Paren, ParenAncestorKind::GroupingParen);
		const NodeId OperatorParent = OperatorBoundary.Parent, Operand = OperatorBoundary.Child;
		if(Syntax.Is(Paren, Role::ConditionalAccessParen) && !Syntax.Is(Inner, Role::GroupingParen)) {
			// 後置の `!` を透過した後置の連鎖の祖先
			const NodeId Chain = Syntax.Is(OperatorParent, Role::PostfixUnaryExpr) ?
			GetParenAncestorOf(Syntax, OperatorParent, ParenAncestorKind::PostfixUnary).Parent :
			OperatorParent;
			if(Syntax.Is(Chain, Role::ConditionalAccessChain)) {
				std::unordered_map<NodeId, bool> &Receivers = ScanMemo.ConditionalReceivers;
				std::vector<NodeId> Pending;
				bool HasConditional = false;
				for(NodeId Target = Inner; Target != NoNode;) {
					Syntax.Charge(1);
					if(const std::unordered_map<NodeId, bool>::const_iterator Known = Receivers.find(Target); Known != Receivers.end()) {
						HasConditional = Known->second;
						break;
					}
					Pending.push_back(Target);
					if(Syntax.Is(Target, Role::ConditionalAccessExpr)) {
						HasConditional = true;
						break;
					}
					if(!Syntax.Is(Target, Role::ConditionalAccessChain)) break;
					const bool IsPostfixUnary = Syntax.Is(Target, Role::PostfixUnaryExpr);
					Target = Syntax.GetNamedChild(Target, 0);
					// 後置の `!` の内側の括弧だけを透過する
					if(!IsPostfixUnary && Syntax.Is(Target, Role::GroupingParen)) break;
					while(Syntax.Is(Target, Role::GroupingParen)) {
						Syntax.Charge(1);
						Target = Syntax.GetNamedChild(Target, 0);
					}
				}
				for(const NodeId Visited : Pending) {
					Syntax.Charge(1);
					Receivers.emplace(Visited, HasConditional);
				}
				// 後置の `!` を介した連鎖を `null` 条件の連鎖へ取り込ませない括弧の保持の返戻
				if(HasConditional) return true;
			}
		}
		// 入力側で確定した型引数の区切と演算子の読みを守る括弧の保持
		if(Syntax.GetNode(Paren).Facts.NeedsAngleParens) return true;
		const bool HasConflictingAngle = Syntax.GetRules().HasAngleListSyntax && HasAngleConflict(Syntax, Paren, AngleMemo);
		if(HasConflictingAngle && Syntax.Is(Paren, Role::TypeArgAmbiguousParen)) {
			// 一覧の要素の中の比較・シフトが型引数と紛れる括弧（外すとカンマの帰属が変わる）の保持
			const bool IsListElement = Syntax.Is(Parent, Role::TypeArgLookalikeSlot);
			const NodeId List = Syntax.Is(Parent, Role::Arg) ? Syntax.GetParentOf(Parent) : Parent;
			// 一覧の要素だけ，括弧自身と後続の兄弟の閉じを部分木の事実から照会する
			bool HasLaterClose = false;
			if(IsListElement) {
				HasAngleConflict(Syntax, List, AngleMemo);
				const NormalizationFacts::AngleFact &ParenFact = AngleMemo.at(Paren), &ParentFact = AngleMemo.at(Parent);
				const NormalizationFacts::AngleFact &ListFact = AngleMemo.at(List);
				HasLaterClose = ParenFact.HasClose || ParentFact.HasClose && Syntax.GetNode(Paren).Idx < ParentFact.LastCloseChild ||
				Parent != List && ListFact.HasClose && Syntax.GetNode(Parent).Idx < ListFact.LastCloseChild;
			}
			bool ShouldKeep = false;
			Core::SyntaxModel::HasStoppedTraversal(
				Syntax,
				Inner,
				[&](const NodeId Node) -> WalkStep {
					// トークンの内側へ降りない事の返戻
					if(!Syntax.IsNamed(Node)) return WalkStep::Skip;
					if(Syntax.Is(Node, Role::AngleBracketList)) {
						ShouldKeep = Syntax.GetNode(Node).Facts.Has(ConstraintKind::ValueSequence);
						// 値の並びのカンマで保持を決め，中へ降りない事の返戻
						return ShouldKeep ? WalkStep::Stop : WalkStep::Skip;
					}
					ShouldKeep = IsListElement && Syntax.Is(Node, Role::BinaryOperation) &&
					Syntax.IsTokenRole(Syntax.GetFieldChild(Node, ChildRole::Operator), Role::LessThanToken) && HasLaterClose;
					// 保持が決まった後の終了と，対の括弧の外側だけへ降りる事の返戻
					return ShouldKeep ? WalkStep::Stop : HasOpeningBracketPair(Syntax, Node) ? WalkStep::Skip : WalkStep::Descend;
				}
			);
			// 型引数と紛れる括弧の保持の返戻
			if(ShouldKeep) return true;
		}
		// 二項式を型の変換と読み違えた式の外側の括弧の保持の返戻
		if(
			Syntax.Is(Inner, Role::CastBeforeUnary) &&
			Syntax.IsEdgeCharRole(Syntax.GetFirstEdge(Syntax.GetLastNamedChild(Inner)), Role::CastOperandLead) &&
			(Syntax.Is(Inner, Role::KeywordCastOperand) || Syntax.Is(Syntax.GetFieldChild(Inner, ChildRole::Type), Role::GenericName))
		) return true;
		if(Syntax.Is(Inner, Role::DeducedValCategoryName)) {
			// 括弧の層を透過した最初の祖先
			const NodeId Host = OperatorParent;
			// 部分木が `decltype(auto)` を含むか
			const auto HasDecltypeAuto = [&Syntax](const NodeId Root) -> bool {
				// `auto` を中身に持つ `decltype` の有無の返戻
				return Core::SyntaxModel::HasNodeMatching(
					Syntax,
					Root,
					[&Syntax](const NodeId Node) -> bool {
						// `decltype(auto)` かの返戻
						return Syntax.Is(Node, Role::ValCategoryType) && Syntax.Is(Syntax.GetNamedChild(Node, 0), Role::DeducedTypeKeyword);
					}
				);
			};
			if(Syntax.Is(Host, Role::InitDeclarator) && HasDecltypeAuto(Syntax.GetFieldChild(Syntax.GetParentOf(Host), ChildRole::Type))) {
				// 名前・フィールドの参照を `decltype(auto)` の変数の初期化子に置く括弧（値と参照を変える）の保持の返戻
				return true;
			}
			if(Syntax.Is(Host, Role::ReturnStmt)) {
				const NodeId Function =
				Syntax.Is(Host, Role::ReturnValHost) ? Host : GetParenAncestorOf(Syntax, Host, ParenAncestorKind::ReturnHost).Parent;
				// 戻値の型（後置の戻値の型を含む）が `decltype(auto)` の関数の返戻の値の括弧（値と参照を変える）の保持の返戻
				if(
					const NodeId Body = Syntax.GetFieldChild(Function, ChildRole::Body); Core::SyntaxModel::HasChildMatching(
						Syntax,
						Function,
						[&](const NodeId Child) -> bool {
							// 本体以外の型に `decltype(auto)` を持つかの返戻
							return Child != Body && Syntax.IsNamed(Child) && HasDecltypeAuto(Child);
						}
					)
				) return true;
			}
		}
		if(Syntax.Is(Paren, Role::CastAmbiguousParen)) {
			// 括弧の後の最初のトークン（括弧が親の最後の子なら祖先の後のトークン）
			const NodeId After = GetParenAncestorOf(Syntax, Paren, ParenAncestorKind::Following).Child;
			NodeId Follower = NoNode;
			for(NodeId Leaf = After == NoNode ? NoNode : Syntax.GetNextSibling(After); Leaf != NoNode; Leaf = Syntax.GetChild(Leaf, 0)) {
				if(!Syntax.GetChildCount(Leaf)) Follower = Leaf;
			}
			// 更新の被演算子の括弧の後のトークン（間接参照・呼出が続けば型と読める）
			const bool IsUpdateOperand = Syntax.Is(Parent, Role::UpdateExpr) && Syntax.GetChild(Parent, 0) == Paren;
			// 型と変数を見分けれない名前の括弧（後続の `*` `&` `-` `+` が括弧を型の変換と読ませる）の保持の返戻
			if(
				const bool HasUpdateFollower =
				IsUpdateOperand && Syntax.IsEdgeCharRole(Syntax.GetFirstEdge(Syntax.GetNextSibling(Parent)), Role::CastUpdateFollowerLead);
				Syntax.Is(Inner, Role::CastTypePart) || Syntax.Is(Inner, Role::Identifier) && Syntax.GetNamedChild(Paren, 0) == Inner &&
				Syntax.IsTokenRole(Follower, Role::CastContinuationToken) && (!IsUpdateOperand || HasUpdateFollower)
			) return true;
		}
		if(Syntax.Is(Parent, Role::ColonSensitiveInterpolation)) {
			// 補間の書式の区切と紛れる最上位の `:` の括弧（対の括弧・文字列の中は除く）の保持
			bool HasColon = false;
			Core::SyntaxModel::HasStoppedTraversal(
				Syntax,
				Inner,
				[&](const NodeId Node) -> WalkStep {
					if(!Syntax.IsNamed(Node)) {
						HasColon = Syntax.IsTokenRole(Node, Role::InterpolationFormatSeparator);
						// 保持が決まった後の終了と，トークンの内側へ降りない事の返戻
						return HasColon ? WalkStep::Stop : WalkStep::Skip;
					}
					// 対の括弧・文字列の中へ降りない事の返戻
					return Node == Inner ||
					!HasOpeningBracketPair(Syntax, Node) && !Syntax.IsEdgeCharRole(Syntax.GetFirstEdge(Node), Role::InterpolationOpaqueLead) ?
					WalkStep::Descend :
					WalkStep::Skip;
				}
			);
			// 書式の区切と紛れる括弧の保持の返戻
			if(HasColon) return true;
		}
		// 区切だけが続く値の位置か（右端迄取る式も外せる）
		const bool IsClosedValSlot = Syntax.Is(Parent, Role::ParenValSlot) && (
			Next == NoNode || Syntax.IsNamed(Next) || Syntax.GetLexemeOf(Next).empty() || Syntax.IsTokenRole(Next, Role::ExprTerminator) ||
			Syntax.IsTokenRole(Next, Role::ColonToken) && Syntax.Is(Parent, Role::TernaryExpr) ||
			Syntax.IsTokenRole(Next, Role::ClosedValTerminator)
		);
		// 括弧の直後の構文（親の末尾なら祖先の後続）
		const ParenAncestorBoundary Following = GetParenAncestorOf(Syntax, Paren, ParenAncestorKind::Following);
		// 文の並びの次の文は，現在の式の後続に含めない
		const NodeId AfterNode =
		Following.Child == NoNode || Following.Parent == Syntax.GetRoot() || Syntax.Is(Following.Parent, Role::StmtHost) ?
		NoNode :
		Syntax.GetNextSibling(Following.Child);
		NodeId AfterLead = NoNode;
		if(Syntax.Is(Inner, Role::GreedyTailExpr) && AfterNode != NoNode) {
			if(
				const std::unordered_map<NodeId, NodeId>::const_iterator At = ScanMemo.FirstLeaf.find(AfterNode);
				At != ScanMemo.FirstLeaf.end()
			) {
				Syntax.Charge(1);
				AfterLead = At->second;
			} else {
				AfterLead = Syntax.GetFirstEdge(AfterNode).Leaf;
				ScanMemo.FirstLeaf.emplace(AfterNode, AfterLead);
			}
		}
		// 値の位置又は後続の区切が，貪欲な式の右端を閉じるか
		const bool IsClosedTail = IsClosedValSlot || AfterNode == NoNode || Syntax.IsTokenRole(AfterLead, Role::SeparatorLead) ||
		Syntax.IsTokenRole(AfterLead, Role::ClosingBracketLead);
		// 文頭のトークン・括弧を要する中身の保持の返戻
		if(
			Syntax.Is(Parent, Role::ExprStmt) && Syntax.Is(Inner, Role::ExprStmtLiteral) ||
			Syntax.Is(Inner, Role::ParenInnerNoUnwrap) && !(Syntax.Is(Inner, Role::GreedyTailExpr) && IsClosedTail) ||
			Syntax.Is(Parent, Role::FoldExpr)
		) return true;
		// 左端の多重代入・セイウチの括弧の保持
		NodeId First = Syntax.GetChild(Inner, 0);
		std::vector<NodeId> FirstPath;
		bool HasLeftConstraint = false;
		while(Syntax.IsNamed(First) && !ScanMemo.LeftConstraint.contains(First)) {
			Syntax.Charge(1);
			FirstPath.push_back(First);
			if(Syntax.Is(First, Role::ParenInnerFirstChildNoUnwrap)) {
				HasLeftConstraint = true;
				break;
			}
			First = Syntax.GetChild(First, 0);
		}
		if(ScanMemo.LeftConstraint.contains(First)) {
			Syntax.Charge(1);
			HasLeftConstraint = ScanMemo.LeftConstraint.at(First);
		}
		for(const NodeId Visited : FirstPath) ScanMemo.LeftConstraint.emplace(Visited, HasLeftConstraint);
		if(HasLeftConstraint || Syntax.Is(Parent, Role::BodyAmbiguousHeader) && IsInBraceHeader() && HasBodyLikeExpr(Syntax, Paren)) {
			// 左端の制約か，制御構文の見出で本体のブロックと紛れる式の括弧の保持の返戻
			return true;
		}
		if(Syntax.Is(Paren, Role::BlockEndingParen)) {
			// 左端のブロックの式
			NodeId Lead = Inner;
			std::vector<NodeId> LeadPath;
			while(!ScanMemo.BlockLead.contains(Lead)) {
				Syntax.Charge(1);
				LeadPath.push_back(Lead);
				if(Syntax.Is(Lead, Role::StmtEndingBlock) || !Syntax.IsNamed(Syntax.GetChild(Lead, 0))) break;
				Lead = Syntax.GetChild(Lead, 0);
			}
			if(ScanMemo.BlockLead.contains(Lead)) {
				Syntax.Charge(1);
				Lead = ScanMemo.BlockLead.at(Lead);
			}
			for(const NodeId Visited : LeadPath) ScanMemo.BlockLead.emplace(Visited, Lead);
			if(Syntax.Is(Lead, Role::StmtEndingBlock)) {
				// 括弧を左端に持つ最も外の式（文の頭で止める）
				NodeId Top = Paren;
				std::vector<NodeId> TopPath;
				while(!ScanMemo.BlockTop.contains(Top)) {
					Syntax.Charge(1);
					TopPath.push_back(Top);
					const NodeId Up = Syntax.GetParentOf(Top);
					if(Up == NoNode || Syntax.GetChild(Up, 0) != Top || Syntax.Is(Up, Role::BlockExprStmtHost)) break;
					Top = Up;
				}
				if(ScanMemo.BlockTop.contains(Top)) {
					Syntax.Charge(1);
					Top = ScanMemo.BlockTop.at(Top);
				}
				for(const NodeId Visited : TopPath) ScanMemo.BlockTop.emplace(Visited, Top);
				// 文頭のブロックの式の括弧（閉じの `}` で文が終わり，後続が別の文へ離れる）の保持の返戻
				if(
					Syntax.Is(Syntax.GetParentOf(Top), Role::BlockExprStmtHost) &&
					(LastLeafOf(Top) != LastLeafOf(Paren) || LastLeafOf(Inner) != LastLeafOf(Lead))
				) return true;
			}
			if(IsInBraceHeader()) {
				NodeId Edge = Inner;
				std::vector<NodeId> EdgePath;
				bool HasJump = false;
				while(Edge != NoNode && !ScanMemo.RightJump.contains(Edge)) {
					Syntax.Charge(1);
					EdgePath.push_back(Edge);
					if(
						const uint32_t Count = Syntax.GetNamedChildCount(Edge);
						Syntax.Is(Edge, Role::OptionalValJump) && (!Count || Count == 1 && Syntax.Is(Syntax.GetNamedChild(Edge, 0), Role::JumpLabel))
					) {
						HasJump = true;
						break;
					}
					Edge = GetRightEdgeChild(Syntax, Edge);
				}
				if(Edge != NoNode && ScanMemo.RightJump.contains(Edge)) {
					Syntax.Charge(1);
					HasJump = ScanMemo.RightJump.at(Edge);
				}
				for(const NodeId Visited : EdgePath) ScanMemo.RightJump.emplace(Visited, HasJump);
				// 見出の右端の値の無い脱出の式の括弧の保持の返戻
				if(HasJump) return true;
			}
		}
		// 単項と値の記号の融合を防ぐ最内の括弧の保持の返戻
		if(
			Syntax.Is(OperatorParent, Role::UnaryPreOrUpdate) && !Syntax.IsNamed(Syntax.GetChild(OperatorParent, 0)) &&
			HasPrefixOperatorFusion(Syntax.GetRules(), Syntax.GetLastChar(Syntax.GetChild(OperatorParent, 0)), Syntax.GetFirstChar(Inner))
		) return true;
		if(Syntax.Is(Paren, Role::OperatorBoundaryParen)) {
			// 括弧を左端に持つ最も外の式と，其の前の兄弟
			NodeId Top = Paren;
			std::vector<NodeId> TopPath;
			while(!ScanMemo.OperatorTop.contains(Top)) {
				Syntax.Charge(1);
				TopPath.push_back(Top);
				const NodeId Up = Syntax.GetParentOf(Top);
				if(Up == NoNode || Syntax.GetChild(Up, 0) != Top) break;
				Top = Up;
			}
			if(ScanMemo.OperatorTop.contains(Top)) {
				Syntax.Charge(1);
				Top = ScanMemo.OperatorTop.at(Top);
			}
			for(const NodeId Visited : TopPath) ScanMemo.OperatorTop.emplace(Visited, Top);
			// 単項・範囲の記号と中身の記号の融合，`try`・`await`・型の変換の結合の変化を防ぐ括弧の保持の返戻
			if(
				const NodeId Before = Syntax.GetPrevSibling(Top);
				Syntax.Is(Inner, Role::AwaitTryCastExpr) || Syntax.Is(Parent, Role::RangeOrUnaryExpr) && Next != NoNode &&
				Syntax.GetRules().CanFuseOperatorChar(Syntax.GetFirstChar(Next)) &&
				Syntax.GetRules().CanFuseOperatorChar(Syntax.GetLastChar(Inner)) ||
				Syntax.Is(Syntax.GetParentOf(Top), Role::RangeOrUnaryExpr) && (
					Syntax.GetRules().CanFuseOperatorChar(Before == NoNode ? '\0' : Syntax.GetLastChar(Before)) ||
					Syntax.HasLastTokenRole(Before, Role::MemberAccessTail)
				) && Syntax.GetRules().CanFuseOperatorChar(Syntax.GetFirstChar(Inner))
			) return true;
		}
		const int ParentPrec = OperatorParent == NoNode ? 0 : GetOperatorPrec(Syntax, OperatorParent, Memo);
		// 値全体を占める式だけを外し，演算子の被演算子では優先順位の照合を保つ
		if(
			IsClosedTail && Syntax.Is(Inner, Role::GreedyTailExpr) &&
			(IsClosedValSlot || ParentPrec < 2 && !Syntax.Is(OperatorParent, Role::InfixOperator))
		) return false;
		int InnerPrec = GetOperatorPrec(Syntax, Inner, Memo);
		// 幅の定数式の文法と，条件式の偽の枝へ後続の初期化を取り込まない境界を保つ括弧の返戻
		if(
			Syntax.Is(OperatorParent, Role::SizedDeclarator) && Syntax.HasField(Operand, ChildRole::Length) && (
				Syntax.Is(Inner, Role::CommaExpr) || InnerPrec && InnerPrec < Syntax.GetRules().ConditionalPrec ||
				InnerPrec == Syntax.GetRules().ConditionalPrec &&
				Syntax.IsTokenRole(Syntax.GetNextSibling(OperatorParent), Role::AssignmentToken)
			)
		) return true;
		// 内側と親の優先順位からのラッパーの判定
		const bool IsIterationHeaderParent = Syntax.Is(OperatorParent, Role::IterationHeader);
		// 構文で指定した優先順位の囲みと，前置式が後続の型アサーションを取り込む囲みの保持の返戻
		if(
			Syntax.GetRules().RequiredValParenPrecs.contains(InnerPrec) && !IsIterationHeaderParent ||
			Syntax.Is(Inner, Role::PrefixAbsorbsTypeCast) && Syntax.Is(OperatorParent, Role::TypeCastRight) &&
			Syntax.GetNamedChild(OperatorParent, 0) == Operand
		) return true;
		// 優先順位の分からない中置の演算子の，原子式でない被演算子の括弧の保持の返戻
		if(!ParentPrec) return Syntax.Is(OperatorParent, Role::InfixOperator) && !IsPostfixAtom(Syntax, Inner);
		// 優先順位の分からない内側の中置の演算子の括弧の保持の返戻
		if(!InnerPrec && Syntax.Is(Inner, Role::InfixOperator)) return true;
		if(!InnerPrec) InnerPrec = UnknownInnerPrec;
		// 累乗より弱い単項の内側の括弧を外せる言語か（単項を左に置けない JavaScript・TypeScript と誤読する PHP は保持）
		const std::string_view Sign =
		ParentPrec == 15 && InnerPrec == 14 ? Syntax.GetLexemeOf(Syntax.GetChild(OperatorParent, 0)) : std::string_view();
		const bool IsSignOverPower = !Sign.empty() && Syntax.GetRules().SignsOverPower.find(Sign) != std::string_view::npos;
		bool ShouldKeep = InnerPrec < ParentPrec && !IsSignOverPower;
		// 親の左の被演算子か
		const bool IsLeftOperand = Syntax.GetNamedChild(OperatorParent, 0) == Operand;
		const bool IsPower = ParentPrec == Syntax.GetRules().PowerPrec;
		// 代入の左辺の条件式の括弧（代入が偽の枝へ入る事を防ぐ）の保持
		if(
			!ShouldKeep && IsLeftOperand && !IsIterationHeaderParent &&
			(ParentPrec == Syntax.GetRules().AssignmentPrec && InnerPrec == Syntax.GetRules().ConditionalPrec)
		) ShouldKeep = true;
		// 型変換が前後の演算を取り込む括弧の保持
		if(Syntax.Is(Inner, Role::TypeCastRight)) {
			if(IsLeftOperand) {
				const NodeId NextToken = Syntax.GetNextSibling(Operand);
				const std::string_view Token = Syntax.IsNamed(NextToken) ? std::string_view() : Syntax.GetLexemeOf(NextToken);
				const bool IsAbsorbed = !Syntax.IsNamed(NextToken) && Syntax.IsTokenRole(NextToken, Role::OpeningAngleLead) ||
				Syntax.GetRules().TypeContinuationTokens.contains(Token);
				const int OppositeBoundary = Syntax.Is(Inner, Role::LeftAbsorbingTypeCast) ? Syntax.GetRules().ConditionalPrec : InnerPrec - 1;
				// 逆側の二項演算又は同格以上の演算子の被演算子を変換へ取り込ませない括弧の保持
				ShouldKeep = Syntax.Is(Inner, Role::ReopenableTypeCast) && Syntax.Is(OperatorParent, Role::InfixOperator) ?
				IsAbsorbed || GetOppositePrecOf(Syntax, OperatorParent, IsLeftOperand) > OppositeBoundary :
				ShouldKeep || IsAbsorbed;
			// 右辺の型変換へ手前の二項式全体を取り込ませない括弧の保持
			} else if(Syntax.Is(Inner, Role::LeftAbsorbingTypeCast) && Syntax.Is(OperatorParent, Role::BinaryExpr)) ShouldKeep = true;
		}
		// 内側の右端の弱い式が親の右辺を取り込む括弧（後続の無い前置単項は除く）の保持
		if(!ShouldKeep && IsLeftOperand && Syntax.GetLastNamedChild(OperatorParent) != Operand) {
			for(NodeId Edge = GetRightEdgeChild(Syntax, Inner); !ShouldKeep && Edge != NoNode; Edge = GetRightEdgeChild(Syntax, Edge)) {
				const int EdgePrec = GetOperatorPrec(Syntax, Edge, Memo);
				if(!EdgePrec) break;
				ShouldKeep = EdgePrec < ParentPrec;
			}
		}
		// 累乗の左辺の単項の括弧（結合の変化と JavaScript・TypeScript の構文破壊を防ぐ）の保持
		if(!ShouldKeep && IsPower && IsLeftOperand && InnerPrec == 15) ShouldKeep = true;
		// 演算子のトークンの優先順位に基付く，同格の結合方向の保持
		if(!ShouldKeep && InnerPrec == ParentPrec && !IsIterationHeaderParent) {
			// 非結合の比較・シフト・範囲と，TypeScript の型引数の呼出に紛れる比較
			const bool IsNonAssoc = Syntax.GetRules().NonAssocPrecs.contains(ParentPrec) || Syntax.Is(Inner, Role::RangeExpr) ||
			Syntax.Is(OperatorParent, Role::GenericLookalikeOperator) && ParentPrec == 10 && IsLeftOperand &&
			Syntax.GetText(Syntax.GetFieldChild(OperatorParent, ChildRole::Operator)) == Syntax.GetRules().Operators.Greater &&
			Syntax.GetText(Syntax.GetFieldChild(Inner, ChildRole::Operator)) == Syntax.GetRules().Operators.Less &&
			HasTypeArgOpener(Syntax, Syntax.GetFieldChild(OperatorParent, ChildRole::Right), Memo);
			const bool IsRightAssoc = IsPower || Syntax.GetRules().RightAssocPrecs.contains(ParentPrec);
			// Python の三項を置けない条件部（最後の被演算子以外）
			const bool IsConditionalOperand =
			Syntax.Is(OperatorParent, Role::ConditionalTailOnly) && Syntax.GetLastNamedChild(OperatorParent) != Operand;
			// Python の `and`・`or` は，被演算子の値を返す平坦な列として評価する
			// 左側のグループを平坦化すると，其の結果の真偽値化が１度減る為に保持する
			const NodeId Operator = Syntax.GetFieldChild(OperatorParent, ChildRole::Operator);
			const std::string_view Text = Syntax.GetLexemeOf(Operator);
			const bool IsLeftLogicalGroup = IsLeftOperand && Syntax.IsWord(Operator, Word::LogicalWord) &&
			(Text == Syntax.GetRules().Operators.LogicalAnd || Text == Syntax.GetRules().Operators.LogicalOr) &&
			Text == Syntax.GetLexemeOf(Syntax.GetFieldChild(Inner, ChildRole::Operator));
			ShouldKeep =
			IsNonAssoc || IsConditionalOperand || IsLeftLogicalGroup || ParentPrec != 15 && (IsRightAssoc ? IsLeftOperand : !IsLeftOperand);
		}
		// 結合の弱い前置の語は，中身が原子でも後続の演算を被演算子へ取り込む
		const bool IsLowPrefix = Syntax.Is(OperatorParent, Role::LowKeywordPrec) && Syntax.Is(OperatorParent, Role::UnaryVal);
		const int BindingPrec = IsLowPrefix && ParentPrec < InnerPrec ? ParentPrec : InnerPrec;
		// 言語固有の同格演算子・誤読・構文制約と逆側の結合を保つ括弧の保持の返戻
		return ShouldKeep || std::ranges::find(Syntax.GetRules().ProtectedPrecPairs, std::pair{ ParentPrec, InnerPrec }) !=
		Syntax.GetRules().ProtectedPrecPairs.end() ||
		GetOppositePrecOf(Syntax, OperatorParent, IsLeftOperand && !IsLowPrefix) > BindingPrec;
	}

	/**
	 * 型の右端の共有要約の取得関数
	 * 括弧の除去で変わらない末端と関数型の通過だけを，未取得の経路に沿って子から合成する
	 * 計算量：初回の経路の長さ N に対し O(N)，取得済の参照は O(1)
	 * @param Syntax 構文モデル
	 * @param Node 型の根
	 * @param Memo 括弧の共有事実
	 * @return 右端と関数型の通過の要約
	 * @throws FormatLimitExceeded 探索又は要約の参照が作業予算を超える場合
	 * @throws std::bad_alloc 経路又は要約のメモリを確保出来ない場合
	 * @throws std::length_error 経路の要素数上限を超える場合
	 */
	NormalizationFacts::TypeEdgeFact SyntaxNormalizer::GetTypeEdge(
		const SyntaxModel &Syntax,
		const NodeId Node,
		ParenScanMemo &Memo
	) {
		std::vector<NodeId> Path;
		NodeId Edge = Node;
		NormalizationFacts::TypeEdgeFact Result;
		while(true) {
			Syntax.Charge(1);
			if(
				const std::unordered_map<NodeId, NormalizationFacts::TypeEdgeFact>::const_iterator Found = Memo.TypeEdges.find(Edge);
				Found != Memo.TypeEdges.end()
			) {
				Result = Found->second;
				break;
			}
			Syntax.Charge(1);
			NodeId Next = NoNode;
			if(!Syntax.Is(Edge, Role::InferredType)) {
				Next = Syntax.Is(Edge, Role::GroupingTypeParen) ? GetTypeParenContent(Syntax, Edge) : GetRightEdgeChild(Syntax, Edge);
			}
			if(Next == NoNode) {
				Result.Edge = Edge;
				Memo.TypeEdges.emplace(Edge, Result);
				break;
			}
			Path.push_back(Edge);
			Edge = Next;
		}
		while(!Path.empty()) {
			Syntax.Charge(1);
			const NodeId Parent = Path.back();
			Path.pop_back();
			Result.HasCallable = Result.HasCallable || Syntax.Is(Parent, Role::CallableType);
			Memo.TypeEdges.emplace(Parent, Result);
		}
		// 合成した右端の返戻
		return Result;
	}

	/**
	 * 型を包む１ノードの冗長な括弧の除去の収集関数（整形仕様 5.2）
	 * 名前等の原子の型 (`(text)` / `(Int)?`) と，型の演算子の被演算子でない位置（アノテーション・別名・型引数・戻値の型）の型を包む括弧は冗長の為に外す
	 * 型の演算子の被演算子に置いた演算子を持つ型 (`(A | B)[]` / `(() -> Unit)?` / `&(dyn A + Send)`) は外すと結合が変わる為に保つ
	 * 中身が括弧だけの層 (`((A | B))[]`) は最も外の層を最も内側の中身で判断し，内側の層は判断に依らず外す
	 * 計算量：参照するノードと祖先の総数 N，テキストの総長 L に対し O(N + L)
	 * @param Syntax 構文モデル
	 * @param Node 括弧の型のノード
	 * @param Plan 書換の計画の格納先
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectTypeParenSteps(const SyntaxModel &Syntax, const NodeId Node, RewritePlan &Plan) const {
		// 括弧の層と最も内側の型の取得
		Syntax.Charge(1);
		const NodeId Parent = Syntax.GetParentOf(Node);
		NodeId Inner = GetTypeParenContent(Syntax, Node);
		// 型の括弧を外さない言語と，型１つを包む括弧でない型と，括弧が構文の Swift の関数の型の仮引数の並び (`(Int) -> Int`) の返戻
		if(
			!Syntax.Is(Node, Role::RemovableTypeParen) || Inner == NoNode ||
			Syntax.Is(Parent, Role::TupleParamFunctionType) && Syntax.GetNamedChild(Parent, 0) == Node
		) return;
		std::vector<NodeId> Layers { Node };
		while(Syntax.Is(Inner, Role::GroupingTypeParen)) {
			Syntax.Charge(1);
			const NodeId Content = GetTypeParenContent(Syntax, Inner);
			if(Content == NoNode) break;
			Layers.push_back(Inner);
			Inner = Content;
		}
		bool IsOperand = false, ShouldKeep = false;
		if(Syntax.Is(Node, Role::AlgebraicTypeParen)) {
			// 制約付の `infer` の右端と，途中の関数型の共有
			const NormalizationFacts::TypeEdgeFact EdgeFact = GetTypeEdge(Syntax, Inner, Syntax.EditNormalizationFacts().Parens->Scan);
			const NodeId Edge = EdgeFact.Edge;
			bool IsReopened = EdgeFact.HasCallable;
			Syntax.Charge(1);
			// 条件型の祖先の右端の追跡
			if(Syntax.Is(Edge, Role::InferredType) && Syntax.GetNamedChildCount(Edge) == 2) {
				const ParenAncestorBoundary Boundary = GetParenAncestorOf(Syntax, Node, ParenAncestorKind::TypeRightBoundary);
				const ParenAncestorBoundary Callable = GetParenAncestorOf(Syntax, Node, ParenAncestorKind::TypeRightCallable);
				IsReopened =
				IsReopened || Syntax.Is(Callable.Parent, Role::CallableType) && !Syntax.Is(Callable.Parent, Role::ConditionalType) &&
				Syntax.GetChild(Callable.Parent, Syntax.GetChildCount(Callable.Parent) - 1) == Callable.Child;
				if(Syntax.Is(Boundary.Parent, Role::ConditionalType)) {
					ShouldKeep = IsReopened && Syntax.GetFieldChild(Boundary.Parent, ChildRole::Right) == Boundary.Child;
				}
			}
		} else if(Syntax.Is(Node, Role::WrappedTypeItem)) {
			// 後続の暗黙のアンラップの `!` と型の `?` の区別（`?` は結合を争わず括弧を外せる）
			const NodeId Next = Syntax.GetNextSibling(Node);
			IsOperand = !Syntax.Is(Inner, Role::OptionalType) && (
				Syntax.Is(Parent, Role::ComposedTypeOperandHost) &&
				!(Syntax.Is(Parent, Role::ProtocolComposition) && Syntax.Is(Inner, Role::ProtocolComposition)) ||
				Syntax.IsTokenRole(Next, Role::ExclamationToken) || Syntax.IsTokenRole(Next, Role::QuestionToken)
			);
		} else if(Syntax.Is(Node, Role::ConversionTypeParen)) {
			// パーサーは参照外しの変換 `*(*T)(x)` を型 `*(*T)` への変換と読む為，ポインタ型を遡った変換の型の括弧と看做す
			const NodeId Owner = GetParenAncestorOf(Syntax, Node, ParenAncestorKind::PtrType).Parent;
			// 型の変換の名前・並び・本体の型だけの括弧を外す
			ShouldKeep = Syntax.Is(Owner, Role::TypeConversion) && !Syntax.Is(Inner, Role::UnambiguousConversionType);
			// 向きの無いチャネルの受信専用の要素の括弧（`<-` の掛かる先を変えない）の保持
			IsOperand = Syntax.Is(Parent, Role::ChannelType) && Syntax.GetChildCount(Parent) == 2 &&
			Syntax.IsTokenRole(Syntax.GetFirstEdge(Inner).Leaf, Role::ChannelReceiveToken);
		}
		IsOperand = IsOperand || Syntax.Is(Parent, Role::NullableTypeOperandHost) || Syntax.Is(Parent, Role::BorrowedTypeOperandHost);
		// 結合を保てる時だけ最も外の層も外し，其の他は内側の層だけを外す
		NodeId CanonicalInner = Inner;
		while(Syntax.Is(CanonicalInner, Role::TypeCombination) && Syntax.GetChildCount(CanonicalInner) == 2) {
			Syntax.Charge(1);
			const CanonicalForm *const Form = Syntax.GetFormOf(CanonicalInner, FormOperation::LeftAssociateBinary);
			const SyntaxFacts &Facts = Syntax.GetNode(CanonicalInner).Facts;
			if(
				!Form || Form->Cond != FormCond::BinaryChainWithOptionalLeft || Facts.Has(ConstraintKind::ContainsError) ||
				Facts.Has(ConstraintKind::Verbatim)
			) break;
			bool HasPreservedBoundary = false;
			for(const SemanticBoundary &Boundary : Facts.Boundaries.GetView()) {
				Syntax.Charge(1);
				HasPreservedBoundary |= Boundary.Kind == BoundaryKind::Preserved;
			}
			if(HasPreservedBoundary) break;
			if(
				const FormPart *const Separator = Form->GetPartOf(FormPartRole::Separator);
				!Separator || Syntax.GetLexemeOf(Syntax.GetChild(CanonicalInner, 0)) != Separator->FixedLexeme
			) break;
			CanonicalInner = Syntax.GetChild(CanonicalInner, 1);
		}
		if(Syntax.Is(Node, Role::AlgebraicTypeParen)) {
			Syntax.Charge(1);
			// 条件の左右とアロー関数の戻値では，後続を取る型の結付を保つ
			IsOperand = Syntax.Is(Parent, Role::ConditionalType) && (
				Syntax.GetFieldChild(Parent, ChildRole::Left) == Node ?
				Syntax.Is(CanonicalInner, Role::GreedyType) :
				Syntax.GetFieldChild(Parent, ChildRole::Right) == Node && Syntax.Is(CanonicalInner, Role::ConditionalType)
			) || Syntax.Is(Parent, Role::TypeAnnotation) && Syntax.Is(CanonicalInner, Role::GreedyType) &&
			Syntax.Is(Syntax.GetParentOf(Parent), Role::ArrowFunction);
			const KindDef *const OuterDef = Syntax.GetDefOf(Parent), *const InnerDef = Syntax.GetDefOf(CanonicalInner);
			if(OuterDef && InnerDef && OuterDef->TypeBinding.HasOperand) {
				const TypeBindingDef &Outer = OuterDef->TypeBinding, &Inside = InnerDef->TypeBinding;
				const bool IsWhole = !Outer.OperandPrecedence || Outer.IsIndex && Syntax.GetNamedChild(Parent, 0) != Node;
				if(IsWhole) IsOperand = false;
				else if(!Inside.Precedence) IsOperand = Syntax.Is(CanonicalInner, Role::TypeOperatorInner);
				else {
					IsOperand = Inside.Precedence < Outer.OperandPrecedence || Outer.PreservesGrouping && Inside.Precedence == Outer.Precedence ||
					Outer.RequiresCallableGroup && Syntax.Is(CanonicalInner, Role::CallableType);
				}
				// 型全体を置けても，後続の演算子を型の続きへ取り込ませない
				if(Outer.IsOpenTail) {
					const ParenAncestorBoundary Boundary = GetParenAncestorOf(Syntax, Parent, ParenAncestorKind::TypeRightBoundary);
					const NodeId Next = Syntax.GetNextSibling(Boundary.Child);
					ShouldKeep = ShouldKeep || Syntax.IsTokenRole(Next, Role::OpeningAngleLead) ||
					Syntax.GetRules().TypeContinuationTokens.contains(Syntax.GetLexemeOf(Next));
				}
			}
		}
		const bool ShouldKeepOuter = ShouldKeep || IsOperand && Syntax.Is(CanonicalInner, Role::TypeOperatorInner);
		for(size_t Layer = ShouldKeepOuter ? 1 : 0; Layer < Layers.size(); ++Layer) {
			Syntax.Charge(1);
			Plan.Steps.push_back({ RewriteKind::Unwrap, Layers[Layer] });
			// Swift の組の型の要素のラッパーも外す（パーサーは括弧の無い型を要素で包まない）
			if(Syntax.Is(Layers[Layer], Role::WrappedTypeItem)) {
				Plan.Steps.push_back({ RewriteKind::Unwrap, Syntax.GetChild(Layers[Layer], 1) });
			}
		}
		// 終了
		return;
	}

	/**
	 * 組と照合の冗長なラッパーの収集関数
	 * 要素の数と配置先の役割から，組の値・代入対象・照合の結付を保つ表現へ揃える
	 * 対象の評価毎に現在の親を読み，祖先のラッパー除去を後続の対象へ反映する
	 * 計算量：参照するノードと祖先の総数 N，テキストの総長 L に対し O(N + L)
	 * @param Syntax 構文モデル
	 * @param Id 現在の規則が評価する１ノード
	 * @param Plan 書換の計画
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectTupleNode(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const {
		const size_t FirstStep = Plan.Steps.size();
		// エラー領域の内側の場合の終了
		if(HasErrorNormalizationAncestor(Syntax, Id)) return;
		const CanonicalForm *const Form = Syntax.GetFormOf(Id, FormOperation::UnwrapSeq);
		// ラッパーを外す形式の無いノードの終了
		if(!Form) return;
		const std::span<const NodeId> Children = Syntax.GetChildrenOf(Id);
		// 括弧の組でない場合の終了
		if(
			Children.size() < 3 || !Syntax.IsTokenRole(Children.front(), Role::OpeningParenToken) ||
			!Syntax.IsTokenRole(Children.back(), Role::ClosingParenToken)
		) return;
		const NodeId Parent = Syntax.GetParentOf(Id), First = Syntax.GetNamedChild(Id, 0);
		const bool IsLeft = Syntax.GetFieldChild(Parent, ChildRole::Left) == Id;
		if(Children.size() == 3 && Syntax.Is(Id, Role::TuplePattern)) {
			// 単純名のアノテーションの記録を抑えるラッパーと，`as` の結付を指定するラッパーを保つ
			// アノテーションの記録を抑える括弧を保つ場合の終了
			if(IsLeft && Syntax.GetFieldChild(Parent, ChildRole::Type) != NoNode && Syntax.Is(First, Role::Identifier)) return;
			NodeId Val = First;
			const NodeId Owner = GetParenAncestorOf(Syntax, Id, ParenAncestorKind::CasePattern).Parent;
			while(Syntax.Is(Val, Role::CasePattern)) Val = Syntax.GetNamedChild(Val, 0);
			if(Syntax.Is(Val, Role::AliasPattern) && (Syntax.Is(Owner, Role::AliasPattern) || Syntax.Is(Owner, Role::AlternativePattern))) {
				// 照合の結付を指定する括弧を保つ場合の終了
				return;
			}
			if(IsLeft && Syntax.GetFieldChild(Parent, ChildRole::Type) != NoNode && Syntax.Is(First, Role::TuplePattern)) {
				NodeId Inner = First;
				while(Syntax.Is(Inner, Role::TuplePattern) && Syntax.GetChildCount(Inner) == 3) {
					Plan.Steps.push_back({ RewriteKind::Unwrap, Inner });
					Inner = Syntax.GetNamedChild(Inner, 0);
				}
				// 単純名のアノテーションの記録を抑える外層を保つ場合の終了
				if(Syntax.Is(Inner, Role::Identifier)) {
					UpdateParenAncestors(Syntax, Plan, FirstStep);
					// 外層のアノテーションの抑止を保つ事の終了
					return;
				}
			}
			Plan.Steps.push_back({ RewriteKind::Unwrap, Id });
			if(Syntax.Is(Parent, Role::CasePattern) && Syntax.Is(First, Role::CasePattern)) {
				Plan.Steps.push_back({ RewriteKind::Unwrap, First });
			}
			UpdateParenAncestors(Syntax, Plan, FirstStep);
			// 単一要素のラッパーの除去の終了
			return;
		}
		// 要素の形と配置先に依る括弧の必要性の判定
		bool HasStar = false, HasRestricted = false;
		for(const NodeId Child : Children) {
			HasStar = HasStar || Syntax.Is(Child, Role::SeqExpansion);
			// 括弧無の展開は単純名だけが同じ子構造を持ち，演算・後置連鎖は展開の外へ結び直される
			HasRestricted = HasRestricted || Syntax.Is(Child, Role::NamedVal) || Syntax.Is(Child, Role::YieldVal) ||
			Syntax.Is(Child, Role::SeqExpansion) && !Syntax.Is(Syntax.GetNamedChild(Child, 0), Role::Identifier);
		}
		const bool IsReturn = Syntax.Is(Parent, Role::ReturnStmt) || Syntax.Is(Parent, Role::YieldVal);
		const bool IsSubscript =
		Syntax.Is(Parent, Role::Subscript) && Syntax.GetChildCount(Parent) == 4 && Syntax.GetChild(Parent, 0) != Id;
		const bool IsTarget = IsLeft && (
			Syntax.Is(Parent, Role::AssignmentExpr) || Syntax.Is(Parent, Role::CountedLoop) ||
			Syntax.Is(Parent, Role::ComprehensionIteration)
		);
		// 単一要素のタプルを文の単一の値と取り違えない為の括弧の保持
		const bool IsSingleStmt =
		Syntax.GetNamedChildCount(Id) == 1 && (Syntax.Is(Parent, Role::Module) || Syntax.Is(Parent, Role::LexicalBlock));
		// 括弧を要する要素又は配置先と，委譲する組の場合の終了
		if(
			const bool IsVal = Syntax.GetFieldChild(Parent, ChildRole::Right) == Id &&
			(Syntax.Is(Parent, Role::AssignmentExpr) || Syntax.Is(Parent, Role::CountedLoop));
			HasRestricted || IsSingleStmt || HasStar && (IsReturn || IsSubscript || Syntax.Is(Parent, Role::CountedLoop)) || !(
				IsReturn || IsSubscript || IsTarget || IsVal || Syntax.Is(Parent, Role::DeleteStmt) || Syntax.Is(Parent, Role::Module) ||
				Syntax.Is(Parent, Role::LexicalBlock)
			) || Syntax.Is(Parent, Role::YieldVal) && Syntax.IsWord(Syntax.GetChild(Parent, 1), Word::Delegation)
		) return;
		// 添字の組の添字フィールドへの投影と値・対象の並びへの投影
		const AnchorTarget Before { First, false }, After { Syntax.GetLastNamedChild(Id), true };
		if(IsSubscript) {
			uint32_t Idx = Syntax.GetNode(Id).Idx;
			for(size_t At = 1; At + 1 < Children.size(); ++At) {
				RewriteStep Move { RewriteKind::Move, Children[At] };
				Move.Dst = Parent;
				Move.Idx = Idx++;
				Move.DstFields = Syntax.IsNamed(Children[At]) ? Syntax.GetNode(Id).Fields : 0;
				Plan.Steps.push_back(std::move(Move));
			}
			Plan.Steps.push_back({ RewriteKind::Remove, Id, {}, { Id, Before, After, Before } });
		} else {
			RewriteStep List { RewriteKind::Reshape, Id, {}, { NoNode, Before, After, Before } };
			const FormPartRole Ctx = IsTarget ?
			FormPartRole::Target :
			Syntax.Is(Parent, Role::Module) || Syntax.Is(Parent, Role::LexicalBlock) ? FormPartRole::Result : FormPartRole::Val;
			List.Inserted = FormNode(*Form, Ctx);
			List.Inserted.Facts = Syntax.GetNode(Id).Facts;
			for(size_t At = 1; At + 1 < Children.size(); ++At) {
				List.Inserted.Children.push_back({ {}, false, {}, Syntax.GetNode(Children[At]).Fields, Children[At] });
			}
			Plan.Steps.push_back(std::move(List));
		}
		UpdateParenAncestors(Syntax, Plan, FirstStep);
		// 終了
		return;
	}

	/**
	 * 型の同値な連鎖１ノードの左結合の収集関数
	 * 同じ固定生成形式の連鎖だけを纏め，保護境界と項・アンカーの順を保つ
	 * 計算量：連鎖のノード数 N，境界数 B と障壁取得の費用 A に対し O(N + B + A)，通常側の同じ規則の評価で A はノード当たり期待値償却 O(1)
	 * @param Syntax 現在の収集が読む構文
	 * @param Node `TypeCombination` の対象（内側の対象も前提を読む）
	 * @param Plan 左結合の構造とアンカー移譲の計画
	 * @throws FormatLimitExceeded 固定生成形式又は項の並びが不完全な場合，又は作業予算を超えた場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectTypeAssociationNodeStep(const SyntaxModel &Syntax, const NodeId Node, RewritePlan &Plan) const {
		// 対象外又はエラー障壁が在る場合の終了
		if(HasErrorNormalizationAncestor(Syntax, Node)) return;
		const CanonicalForm *const Form = Syntax.GetFormOf(Node, FormOperation::LeftAssociateBinary);
		// 対応する連鎖の生成形式が無い場合の終了
		if(!Form || Form->Cond != FormCond::BinaryChainWithOptionalLeft) return;
		const FormPart *const ResultPart = Form->GetPartOf(FormPartRole::Result),
		*const Left = Form->GetPartOf(FormPartRole::Left),
		*const Separator = Form->GetPartOf(FormPartRole::Separator),
		*const Right = Form->GetPartOf(FormPartRole::Right);
		if(!ResultPart || !Left || !Separator || !Right || Separator->FixedLexeme.empty()) {
			throw FormatLimitExceeded("a binary association form has missing parts", FailureReason::FormatterDefect);
		}
		// 同じ生成形式の二項又は左項省略形だけを連鎖へ取り込み，異なる演算と必須のラッパーは項として保つ
		const auto Arity = [&Syntax, ResultPart, Separator](const NodeId Part) -> uint32_t {
			const CanonicalForm *const Found = Syntax.GetFormOf(Part, FormOperation::LeftAssociateBinary);
			// 左結合へ対応する生成形式を持たない事の返戻
			if(!Found || Found->Cond != FormCond::BinaryChainWithOptionalLeft) return 0;
			if(
				const FormPart *const Target = Found->GetPartOf(FormPartRole::Result),
				*const Operator = Found->GetPartOf(FormPartRole::Separator);
				!Target || !Operator || Target->Def != ResultPart->Def || Operator->FixedLexeme != Separator->FixedLexeme
			// 異なる連鎖の返戻
			) return 0;
			const SyntaxFacts &Facts = Syntax.GetNode(Part).Facts;
			// 書換不能な領域の返戻
			if(Facts.Has(ConstraintKind::ContainsError) || Facts.Has(ConstraintKind::Verbatim)) return 0;
			// 原文の間隙を保つ境界を持つ事の返戻
			for(const SemanticBoundary &Boundary : Facts.Boundaries.GetView()) if(Boundary.Kind == BoundaryKind::Preserved) return 0;
			const uint32_t Count = Syntax.GetChildCount(Part);
			// 形式が定める項数を持たない事の返戻
			if(Count != 2 && Count != 3) return 0;
			const NodeId Token = Syntax.GetChild(Part, Count - 2);
			// 文法の固定トークンと，名前付の左右の項を持つ構造の項数の返戻
			return !Syntax.IsNamed(Token) && Syntax.GetLexemeOf(Token) == Separator->FixedLexeme &&
			Syntax.IsNamed(Syntax.GetChild(Part, Count - 1)) && (Count == 2 || Syntax.IsNamed(Syntax.GetChild(Part, 0))) ? Count - 1 : 0;
		};
		// 対応しない対象，又は外側が纏めて扱う内側の連鎖の場合の終了
		if(!Arity(Node) || Arity(Syntax.GetParentOf(Node))) return;
		// 型の結合ノードの部品と除去範囲を集める走査の状態
		struct Frame {
			NodeId Id;
			size_t Begin; // 此の部分木の最初の部品の位置
			uint32_t Next; // 次に調べる子の番号
		};
		// 除くノードのアンカーを移す部品の範囲
		struct RemovedRange {
			NodeId Id;
			size_t Begin; // アンカーの移譲先の最初の部品の位置
			size_t End; // アンカーの移譲先の最後の部品の直後の位置
		};
		std::vector<NodeId> Items, Unary;
		std::vector<Frame> Pending { { Node, 0, 0 } };
		std::vector<RemovedRange> Removed;
		bool HasRight = false, HasLeading = false;
		// 子の順を保つ深さ優先走査と，消えるノードのアンカーを移す部品の範囲の同時収集
		while(!Pending.empty()) {
			Frame &Cur = Pending.back();
			Syntax.Charge(1);
			const uint32_t Count = Syntax.GetChildCount(Cur.Id);
			if(!Cur.Next) {
				HasRight = HasRight || Arity(Syntax.GetChild(Cur.Id, Count - 1));
				if(Count == 2) {
					HasLeading = true;
					Unary.push_back(Cur.Id);
					++Cur.Next;
				}
			}
			if(Cur.Next == Count) {
				if(Cur.Id != Node) Removed.push_back({ Cur.Id, Cur.Begin, Items.size() });
				if(Count == 2) Removed.push_back({ Syntax.GetChild(Cur.Id, 0), Cur.Begin, Cur.Begin + 1 });
				Pending.pop_back();
				continue;
			}
			if(const NodeId Child = Syntax.GetChild(Cur.Id, Cur.Next++); Arity(Child)) Pending.push_back({ Child, Items.size(), 0 });
			else Items.push_back(Child);
		}
		// 既に同じ左結合の正規形の場合の終了
		if(!HasRight && !HasLeading) return;
		// 左項省略形だけの連鎖は，項の識別子とアンカーを保ったラッパー除去へ対応する
		if(Items.size() == 1) {
			for(const NodeId Wrapper : Unary) Plan.Steps.push_back({ RewriteKind::Unwrap, Wrapper });
			// 単一項のラッパー除去を収集した場合の終了
			return;
		}
		if(!(Items.size() % 2)) {
			throw FormatLimitExceeded("a binary association chain has missing operands", FailureReason::FormatterDefect);
		}
		// 除くノードの先行・末尾・内部のアンカーの，移譲先の部品の範囲への移動
		for(const RemovedRange &Range : Removed) {
			const AnchorTarget Before { Items.at(Range.Begin), false };
			const AnchorTarget After = Syntax.IsNamed(Range.Id) ? AnchorTarget{ Items.at(Range.End - 1), true } : Before;
			Plan.Steps.push_back({ RewriteKind::Retarget, Range.Id, {}, { Range.Id, Before, After, Before } });
		}
		GeneratedNode Result { {}, false, {}, 0, Items.front() };
		for(size_t Idx = 1; Idx + 1 < Items.size(); Idx += 2) {
			GeneratedNode Combined = FormNode(*Form, FormPartRole::Result);
			Result.Fields = Syntax.GetFieldBit(Left->Field);
			// 初期化リストを介した成長中の部分木のコピーを避け，各子を直接移す
			Combined.Children.reserve(3);
			Combined.Children.push_back(std::move(Result));
			Combined.Children.push_back({ {}, false, {}, Syntax.GetFieldBit(Separator->Field), Items[Idx] });
			Combined.Children.push_back({ {}, false, {}, Syntax.GetFieldBit(Right->Field), Items[Idx + 1] });
			Result = std::move(Combined);
		}
		const AnchorTarget Before { Items.front(), false }, After { Items.back(), true };
		RewriteStep Step { RewriteKind::Reshape, Node, {}, { NoNode, Before, After, Before } };
		Step.Inserted = std::move(Result);
		Plan.Steps.push_back(std::move(Step));
		// 終了
		return;
	}

	/**
	 * 同じ保持判定で一括除去する括弧の層かの判定関数
	 * 計算量：直接の子の照会だけに限り O(1)
	 * @param Syntax 構文モデル
	 * @param Outer 外側の括弧
	 * @param Inner 内側の括弧
	 * @return 単一の値を包む直接の括弧の層なら真
	 * @throws FormatLimitExceeded 子の照会が作業予算を超える場合
	 */
	bool SyntaxNormalizer::CanShareParenLayers(const SyntaxModel &Syntax, const NodeId Outer, const NodeId Inner) {
		// 中身を区切らない単一の値の括弧の層の返戻
		return Syntax.Is(Outer, Role::GroupingParen) && Syntax.Is(Inner, Role::GroupingParen) && Syntax.GetChildCount(Outer) == 3 &&
		Syntax.GetChildCount(Inner) == 3 && Syntax.GetNamedChildCount(Outer) == 1 && Syntax.GetNamedChildCount(Inner) == 1;
	}

	/**
	 * １ノードの冗長な括弧の除去の収集関数（整形仕様 5.2）
	 * 保護されて居ない括弧は最も外の層の保持を判断し，内側の冗長な層を外す
	 * エラー領域の中と，エラーを含む括弧・エラー領域に接する括弧は保つ
	 * 通常側では，同じ規則の評価で優先順位と祖先の判定を共有する
	 * 計算量：判定対象のノード数 N，祖先深度 H，テキスト総長 L に対し期待値 O(N H + N + L)
	 * @param Syntax 構文モデル
	 * @param Id 現在の規則が評価する１ノード
	 * @param Plan 書換の計画の格納先
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectParenNode(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const {
		// 直接重なる層は最外の括弧の１度の収集で扱い，層毎の書換と祖先の失効を繰り返さない
		if(CanShareParenLayers(Syntax, Syntax.GetParentOf(Id), Id)) return;
		const size_t FirstStep = Plan.Steps.size();
		// 祖先とトークンの判定結果の控え
		#if defined(ShaveFormatRefFacts)
		Syntax.EditNormalizationFacts().Parens.reset();
		#endif
		PrepareParenAncestors(Syntax);
		ParenCtx &Shared = *Syntax.EditNormalizationFacts().Parens;
		std::unordered_map<NodeId, int> &Memo = Shared.Prec;
		AngleFacts &AngleMemo = Shared.Angles;
		std::unordered_map<NodeId, bool> &UnnamedAncestors = Shared.UnnamedAncestors;
		ParenScanMemo &ScanMemo = Shared.Scan;
		// エラーを含む括弧と，エラー領域に接して原文の間隙を保つ括弧か（整形仕様 4.6）
		const auto IsProtected = [&Syntax](const NodeId Paren) -> bool {
			const NodeId Parent = Syntax.GetParentOf(Paren);
			const uint32_t Idx = Syntax.GetNode(Paren).Idx;
			for(const uint32_t Boundary : { Idx, Idx + 1 }) {
				// 原文の間隙を保つ境界に接する事の返戻
				if(
					const SemanticBoundary *const Found = Syntax.GetNode(Parent).Facts.GetBoundaryBefore(Boundary);
					Found && Found->Kind == BoundaryKind::Preserved
				) return true;
			}
			// エラーを含むかの返戻
			return Syntax.GetNode(Paren).Facts.Has(ConstraintKind::ContainsError);
		};
		// エラー領域の内側の場合の終了
		if(HasErrorNormalizationAncestor(Syntax, Id)) return;
		// 括弧候補の祖先と制御構文の見出の判定
		std::vector<NodeId> Path;
		NodeId Up = Syntax.GetParentOf(Id);
		while(Up != NoNode && !UnnamedAncestors.contains(Up)) {
			Syntax.Charge(1);
			Path.push_back(Up);
			Up = Syntax.GetParentOf(Up);
		}
		if(Up != NoNode) Syntax.Charge(1);
		bool HasUnnamedAncestor = Up != NoNode && UnnamedAncestors.at(Up);
		for(std::vector<NodeId>::const_reverse_iterator It = Path.rbegin(); It != Path.rend(); ++It) {
			HasUnnamedAncestor |= *It != Syntax.GetRoot() && !Syntax.IsNamed(*It);
			UnnamedAncestors.emplace(*It, HasUnnamedAncestor);
		}
		// 名前の無いノードと其の内側の場合の終了
		if(HasUnnamedAncestor || Id != Syntax.GetRoot() && !Syntax.IsNamed(Id)) return;
		// Swift の１要素の照合の括弧の除去（見出はパーサーが読める束縛・列挙の場合だけ）
		if(Syntax.Is(Id, Role::RemovablePatternParen) && !IsProtected(Id)) {
			const std::span<const NodeId> Kids = Syntax.GetChildrenOf(Id);
			for(size_t Idx = 0; Idx + 2 < Kids.size(); ++Idx) {
				const NodeId Open = Kids[Idx], Pattern = Kids[Idx + 1], Close = Kids[Idx + 2], Lead = Syntax.GetChild(Pattern, 0);
				// 括弧１組で照合１つを包む形でない位置の継続
				if(
					!Syntax.IsTokenRole(Open, Role::OpeningParenToken) || !Syntax.Is(Pattern, Role::Pattern) ||
					!Syntax.IsTokenRole(Close, Role::ClosingParenToken)
				) continue;
				if(Syntax.Is(Id, Role::Pattern)) {
					// 照合だけを包む照合のラッパーの除去
					if(Kids.size() == 3) Plan.Steps.push_back({ RewriteKind::Unwrap, Id });
				} else if(
					Idx && Syntax.IsWord(Kids[Idx - 1], Word::Case) && Lead != NoNode &&
					(Syntax.IsNamed(Lead) ? Syntax.Is(Lead, Role::ValBindingPattern) : Syntax.IsTokenRole(Lead, Role::DotToken))
				) {
					// 見出の `case` の後の照合の括弧とラッパーを除き，アンカーを子の前後へ移す
					// パーサーはラッパーの無い照合の子を見出へ直に並べる
					const std::vector<NodeId> Parts(Syntax.GetChildrenOf(Pattern).begin(), Syntax.GetChildrenOf(Pattern).end());
					const AnchorTarget Before { Parts.front(), false }, After { Parts.back(), true };
					Plan.Steps.push_back({ RewriteKind::Remove, Open, {}, { Open, Before, Before, Before } });
					uint32_t At = static_cast<uint32_t>(Idx);
					for(const NodeId Part : Parts) {
						// 照合の中の子の前の元の境界の保持
						const SemanticBoundary *const Recorded = Syntax.GetNode(Pattern).Facts.GetBoundaryBefore(Syntax.GetNode(Part).Idx);
						RewriteStep Move { RewriteKind::Move, Part };
						Move.Dst = Id;
						Move.Idx = At++;
						if(Part != Parts.front() && Recorded) {
							Move.Boundary = Recorded->Kind;
							Move.RecordedBoundary = *Recorded;
						} else Move.Boundary = BoundaryKind::Spaced;
						// 見出の条件を表すフィールドは，自身のフィールドを持たない中身へ移す
						// パーサーは隠れた規則のフィールドを，自身のフィールドを持たない子へ受け継がせる
						if(!Syntax.GetNode(Part).Fields) Move.DstFields = Syntax.GetNode(Open).Fields;
						Plan.Steps.push_back(std::move(Move));
					}
					Plan.Steps.push_back({ RewriteKind::Remove, Pattern, {}, { Pattern, Before, After, Before } });
					Plan.Steps.push_back({ RewriteKind::Remove, Close, {}, { Close, After, After, After } });
				}
			}
		}
		// Swift の前置の演算子を呼出と読んだ括弧の除去（強く結合し，記号・`.` で始まらない値だけを前置の式へ組み直す）
		if(const NodeId Args = GetPrefixCallArgs(Syntax, Id); Args != NoNode) {
			const NodeId Operator = Syntax.GetChild(Id, 0), Arg = Syntax.GetNamedChild(Args, 0);
			const NodeId Val = Syntax.GetChildCount(Arg) == 1 ? Syntax.GetNamedChild(Arg, 0) : NoNode;
			if(
				const int ValPrec = Val == NoNode ? 0 : GetOperatorPrec(Syntax, Val, Memo);
				(!Syntax.IsNamed(Operator) || Syntax.Is(Operator, Role::ExclamationMark)) && !IsProtected(Id) && Val != NoNode &&
				Syntax.GetNamedChildCount(Args) == 1 && Syntax.GetChildCount(Args) == 3 &&
				(ValPrec ? ValPrec > 14 : !Syntax.Is(Val, Role::InfixOperator)) &&
				!Syntax.GetRules().CanFuseOperatorChar(Syntax.GetFirstChar(Val)) &&
				!Syntax.IsEdgeCharRole(Syntax.GetFirstEdge(Val), Role::MemberAccessLead)
			) {
				// 括弧のアンカーの値の前後への移動と，其の他のラッパーのアンカーの値への移動
				const AnchorTarget Before { Val, false }, After { Val, true };
				const NodeId Open = Syntax.GetChild(Args, 0), Close = Syntax.GetChild(Args, 2);
				Plan.Steps.push_back({ RewriteKind::Remove, Open, {}, { Open, Before, Before, Before } });
				Plan.Steps.push_back({ RewriteKind::Remove, Close, {}, { Close, After, After, After } });
				RewriteStep Prefix { RewriteKind::Reshape, Id, {}, { NoNode, Before, After, Before } };
				Prefix.Inserted = { {}, true };
				Prefix.Inserted.Def = Syntax.GetRules().PrefixExprDef;
				Prefix.Inserted.Children.push_back({ {}, false, {}, Syntax.GetFieldBit(ChildRole::Operator), Operator });
				Prefix.Inserted.Children.push_back({ {}, false, {}, Syntax.GetFieldBit(ChildRole::Target), Val });
				Plan.Steps.push_back(std::move(Prefix));
			}
		}
		if(Syntax.Is(Id, Role::GroupingParen) && Syntax.GetParentOf(Id) != NoNode) {
			std::vector<NodeId> Layers { Id };
			// 括弧の中身（C# の `this` 等の無名の語１つも中身とする）
			NodeId Inner = Syntax.GetNamedChildCount(Id) == 1 ?
			Syntax.GetNamedChild(Id, 0) :
			Syntax.Is(Id, Role::ValParens) && Syntax.GetChildCount(Id) == 3 ? Syntax.GetChild(Id, 1) : NoNode;
			while(CanShareParenLayers(Syntax, Layers.back(), Inner)) {
				Layers.push_back(Inner);
				Inner = Syntax.GetNamedChild(Inner, 0);
			}
			const bool IsBroken = IsProtected(Id);
			// 括弧の保持と境界の記録が読む同じ所有者を１度だけ求める
			const NodeId ControlOwner = IsBroken || Inner == NoNode ? NoNode : GetControlValParenOwner(Syntax, Id, Inner);
			const bool ShouldKeepOuter =
			IsBroken || Inner == NoNode || ShouldPreserveExprParen(Syntax, Id, Inner, ControlOwner, Memo, AngleMemo, ScanMemo);
			// 語への密着で実引数の区切へ読み替える事を防ぐ境界（構造と子の順は保つ）
			if(ControlOwner != NoNode) {
				const NodeId Args = Syntax.GetParentOf(Id);
				const uint32_t Idx = Syntax.GetNode(Args).Idx;
				if(
					const SemanticBoundary *const Boundary = Syntax.GetNode(ControlOwner).Facts.GetBoundaryBefore(Idx);
					!Boundary || Boundary->Kind != BoundaryKind::RequiredSpace && Boundary->Kind != BoundaryKind::Preserved
				) {
					RewriteStep Space { RewriteKind::Move, Args };
					Space.Dst = ControlOwner;
					Space.Idx = Idx;
					Space.RecordedBoundary = SemanticBoundary{ Idx, BoundaryKind::RequiredSpace };
					Plan.Steps.push_back(std::move(Space));
				}
			}
			for(size_t Layer = ShouldKeepOuter ? 1 : 0; !IsBroken && Layer < Layers.size(); ++Layer) {
				Plan.Steps.push_back({ RewriteKind::Unwrap, Layers[Layer] });
			}
		} else if(Syntax.Is(Id, Role::GroupingTypeParen) && Syntax.GetParentOf(Id) != NoNode && !IsProtected(Id)) {
			CollectTypeParenSteps(Syntax, Id, Plan);
		}
		UpdateParenAncestors(Syntax, Plan, FirstStep);
		// 終了
		return;
	}

	/**
	 * 前置の演算子１ノードの被演算子のラッパーの収集関数
	 * 先頭の記号が１トークンへ融合する場合だけ包み，外のアンカーを括弧の外へ移す
	 * 計算量：被演算子の左端の深度 D と障壁取得の費用 A に対し O(D + A)，通常側の同じ規則の評価で A はノード当たり期待値償却 O(1)
	 * @param Syntax 現在の収集が読む構文
	 * @param Id `UnaryPreOrUpdate` の対象
	 * @param Plan ラッパーとアンカーの移譲の計画
	 * @throws FormatLimitExceeded 固定生成形式が不足した場合，又は作業予算を超えた場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectFusedPrefixNodeStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const {
		// 対象外又はエラー障壁が在る場合の終了
		if(HasErrorNormalizationAncestor(Syntax, Id)) return;
		const NodeId Operator = Syntax.GetChild(Id, 0), Operand = Syntax.GetChild(Id, 1);
		// 記号の融合が起きない場合の終了
		if(
			Operator == NoNode || Syntax.IsNamed(Operator) || !Syntax.IsNamed(Operand) ||
			!HasPrefixOperatorFusion(Syntax.GetRules(), Syntax.GetLastChar(Operator), Syntax.GetFirstChar(Operand))
		) return;
		RewriteStep Wrap { RewriteKind::Wrap, Operand };
		Wrap.Inserted = {
			{},
			true,
			{
				{ Generation::GetGeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallOpen) },
				{ {}, false, {}, {}, Operand },
				{ Generation::GetGeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallClose) }
			}
		};
		Wrap.Inserted.Def = Syntax.GetRules().ExprParenDef;
		Wrap.Boundary = BoundaryKind::Glued;
		// 包んだ被演算子の外のアンカーの括弧の外への移動
		Wrap.Transfer.From = Operand;
		Plan.Steps.push_back(std::move(Wrap));
		// 終了
		return;
	}
} // namespace Core
