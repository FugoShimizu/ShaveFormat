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
	 * 部分木に型引数と比較の読分けに関わる字句が在るかの判定関数
	 * @param Syntax 構文モデル
	 * @param Root 調べる部分木
	 * @param Memo 節点毎の判定の控え
	 * @return 該当する字句が在れば true
	 */
	static bool HasAngleConflict(const SyntaxModel &Syntax, const NodeId Root, std::unordered_map<NodeId, bool> &Memo) {
		// 子の判定を先に得る後行順の走査
		std::vector<std::pair<NodeId, bool>> Pending { { Root, false } };
		while(!Pending.empty()) {
			const auto [Node, IsVisited] = Pending.back();
			Pending.pop_back();
			if(Memo.contains(Node)) continue;
			if(!IsVisited) {
				Pending.emplace_back(Node, true);
				for(const NodeId Child : Syntax.ChildrenOf(Node)) if(!Memo.contains(Child)) Pending.emplace_back(Child, false);
				continue;
			}
			bool HasConflict = Syntax.IsTokenRole(Node, Role::AngleConflictToken);
			for(const NodeId Child : Syntax.ChildrenOf(Node)) HasConflict |= Memo.at(Child);
			Memo.emplace(Node, HasConflict);
		}
		// 部分木の判定の返戻
		return Memo.at(Root);
	}

	/**
	 * 部分木の最初の字句の葉の取得関数
	 * @param Syntax 構文モデル
	 * @param Id 部分木の節点
	 * @return 最初の字句の葉又は節点無
	 */
	NodeId SyntaxNormalizer::FirstLeaf(const SyntaxModel &Syntax, const NodeId Id) {
		std::vector<NodeId> Pending { Id };
		while(!Pending.empty()) {
			const NodeId Current = Pending.back();
			Pending.pop_back();
			if(Current >= Syntax.NodeCount()) continue;
			Syntax.Charge(1);
			const SyntaxNode &Node = Syntax.GetNode(Current);
			// 最初の字句の葉の返戻
			if(!Node.Lexeme.empty()) return Current;
			Pending.insert(Pending.end(), Node.Children.rbegin(), Node.Children.rend());
		}
		// 字句の無い事の返戻
		return NoNode;
	}

	/**
	 * 部分木の最後の字句の葉の取得関数
	 * @param Syntax 構文モデル
	 * @param Id 部分木の節点
	 * @return 最後の字句の葉又は節点無
	 */
	NodeId SyntaxNormalizer::LastLexicalLeaf(const SyntaxModel &Syntax, const NodeId Id) {
		std::vector<NodeId> Pending { Id };
		while(!Pending.empty()) {
			const NodeId Current = Pending.back();
			Pending.pop_back();
			if(Current >= Syntax.NodeCount()) continue;
			Syntax.Charge(1);
			const SyntaxNode &Node = Syntax.GetNode(Current);
			// 最後の字句の葉の返戻
			if(!Node.Lexeme.empty()) return Current;
			Pending.insert(Pending.end(), Node.Children.begin(), Node.Children.end());
		}
		// 字句の無い事の返戻
		return NoNode;
	}

	/**
	 * 継続括弧の選択に対応する期待構文の構築関数
	 * @param Syntax 期待構文
	 * @param Id 包装する式
	 * @return 包装の書換
	 * @throws FormatLimitExceeded 継続括弧の生成形式を持たない場合
	 */
	RewriteStep SyntaxNormalizer::ContinuationStep(const SyntaxModel &Syntax, const NodeId Id) {
		// 継続括弧の生成形式の確認
		const CanonicalForm *const Form = Syntax.FormOf(Id, FormOperation::Continuation);
		const FormPart *const ResultPart = Form ? Form->PartOf(FormPartRole::Result) : nullptr;
		if(!ResultPart) throw FormatLimitExceeded("a continuation has no grammar form", FailureReason::FormatterDefect);
		const KindDefinition *const Result = ResultPart->Definition;
		if(Syntax.Is(Id, Role::TypeSyntax)) {
			const auto Expression = [&](const NodeId Value, const uint64_t Fields) -> GeneratedNode {
				struct Frame {
					NodeId Value;
					uint64_t Fields;
					GeneratedNode *Out;
					size_t NextChild = 0;
					bool Entered = false;
				};
				GeneratedNode ExpressionNode;
				// 型の適用を式の添字として組み立てる走査
				std::vector<Frame> Pending { { Value, Fields, &ExpressionNode } };
				while(!Pending.empty()) {
					Frame &Current = Pending.back();
					if(!Current.Entered) {
						Current.Entered = true;
						while(Syntax.Is(Current.Value, Role::TypeSyntax)) Current.Value = Syntax.NamedChild(Current.Value, 0);
						if(!Syntax.Is(Current.Value, Role::TypeApplication)) {
							*Current.Out = { {}, false, {}, Current.Fields, Current.Value };
							Pending.pop_back();
							continue;
						}
						*Current.Out = { {}, true, {}, Current.Fields };
						Current.Out->Definition = Form->PartOf(FormPartRole::Body)->Definition;
					}
					NodeId Child;
					uint64_t ChildFields;
					if(!Current.NextChild) {
						Child = Syntax.Child(Current.Value, 0);
						ChildFields = uint64_t{ 1 } << static_cast<uint8_t>(ChildRole::Value);
					} else {
						const std::span<const NodeId> Arguments = Syntax.ChildrenOf(Syntax.Child(Current.Value, 1));
						if(Current.NextChild > Arguments.size()) {
							Pending.pop_back();
							continue;
						}
						Child = Arguments[Current.NextChild - 1];
						ChildFields = Syntax.IsNamed(Child) ? uint64_t{ 1 } << static_cast<uint8_t>(ChildRole::Other) : 0;
					}
					++Current.NextChild;
					Current.Out->Children.emplace_back();
					Pending.push_back({ Child, ChildFields, &Current.Out->Children.back() });
				}
				// 式の文法で表した添字の返戻
				return ExpressionNode;
			};
			GeneratedNode Paren { {}, true };
			Paren.Definition = Result;
			Paren.Children = {
				{ GeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallOpen) },
				Expression(Syntax.NamedChild(Id, 0), 0),
				{ GeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallClose) }
			};
			RewriteStep Type { RewriteKind::Reshape, Id };
			Type.Inserted = { {}, true, { std::move(Paren) } };
			Type.Inserted.Definition = Syntax.GetNode(Id).Kind.Definition;
			// 型の内側を継続括弧で包む操作の返戻
			return Type;
		}
		// 型以外の式と組の包装
		const bool IsList = Syntax.Is(Id, Role::BareTuple);
		RewriteStep Step { IsList ? RewriteKind::Reshape : RewriteKind::Wrap, Id };
		Step.Inserted = { {}, true };
		Step.Inserted.Definition = Result;
		Step.Inserted.Children.push_back({ GeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallOpen) });
		if(IsList) for(const NodeId Child : Syntax.ChildrenOf(Id)) {
			Step.Inserted.Children.push_back({ {}, false, {}, Syntax.GetNode(Child).Fields, Child });
		}
		else {
			Step.Inserted.Children.push_back({ {}, false, {}, 0, Id });
			// 包んだ式の外の錨は括弧の外へ移す
			Step.Transfer.From = Id;
		}
		Step.Inserted.Children.push_back({ GeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallClose) });
		// 包装する操作の返戻
		return Step;
	}

	/**
	 * 組と照合の冗長な包装の収集関数
	 * 要素の数と配置先の役割から，組の値・代入対象・照合の結付を保つ表現へ揃える
	 * 既に処理した祖先の包装は控えて再利用する
	 * @param Syntax 構文モデル
	 * @param Plan 書換の計画
	 */
	void SyntaxNormalizer::CollectTupleSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const {
		std::unordered_map<NodeId, bool> SkippedAncestors;
		// 組の候補を祖先の処理結果と共に調べる走査
		for(const NodeId Id : Candidates(Syntax, { Role::TuplePattern, Role::TupleValue }, { Role::Error })) {
			std::vector<NodeId> Path;
			NodeId Up = Syntax.ParentOf(Id);
			while(Up != NoNode && !SkippedAncestors.contains(Up)) {
				Syntax.Charge(1);
				Path.push_back(Up);
				Up = Syntax.ParentOf(Up);
			}
			if(Up != NoNode) Syntax.Charge(1);
			bool IsSkipped = Up != NoNode && SkippedAncestors.at(Up);
			for(std::vector<NodeId>::const_reverse_iterator It = Path.rbegin(); It != Path.rend(); ++It) {
				SkippedAncestors.emplace(*It, IsSkipped);
			}
			if(IsSkipped) continue;
			const std::span<const NodeId> Children = Syntax.ChildrenOf(Id);
			// 括弧の組でない候補の継続
			if(
				Children.size() < 3 || !Syntax.IsTokenRole(Children.front(), Role::OpeningParenToken) ||
				!Syntax.IsTokenRole(Children.back(), Role::ClosingParenToken)
			) continue;
			const NodeId Parent = Syntax.ParentOf(Id), First = Syntax.NamedChild(Id, 0);
			const bool IsLeft = Syntax.FieldChild(Parent, ChildRole::Left) == Id;
			if(Children.size() == 3 && Syntax.Is(Id, Role::TuplePattern)) {
				// 単純名の注釈の記録を抑える包装と，as の結付を指定する包装を保つ
				// 注釈の記録を抑える括弧を保つ為の読飛
				if(IsLeft && Syntax.FieldChild(Parent, ChildRole::Type) != NoNode && Syntax.Is(First, Role::Identifier)) continue;
				NodeId Value = First, Owner = Parent;
				while(Syntax.Is(Value, Role::CasePattern)) Value = Syntax.NamedChild(Value, 0);
				while(Syntax.Is(Owner, Role::CasePattern)) Owner = Syntax.ParentOf(Owner);
				// 照合の結付を指定する括弧を保つ為の読飛
				if(
					Syntax.Is(Value, Role::AliasPattern) && (Syntax.Is(Owner, Role::AliasPattern) || Syntax.Is(Owner, Role::AlternativePattern))
				) continue;
				if(IsLeft && Syntax.FieldChild(Parent, ChildRole::Type) != NoNode && Syntax.Is(First, Role::TuplePattern)) {
					Plan.Steps.push_back({ RewriteKind::Unwrap, First });
					// 外側の包装を保ち，次の巡で中身を確かめる事の返戻
					SkippedAncestors[Id] = true;
					continue;
				}
				Plan.Steps.push_back({ RewriteKind::Unwrap, Id });
				if(Syntax.Is(Parent, Role::CasePattern) && Syntax.Is(First, Role::CasePattern)) {
					Plan.Steps.push_back({ RewriteKind::Unwrap, First });
				}
				// 次の候補の継続
				continue;
			}
			// 要素の形と配置先による括弧の必要性の判定
			bool HasStar = false, HasRestricted = false;
			for(const NodeId Child : Children) {
				HasStar = HasStar || Syntax.Is(Child, Role::SequenceExpansion);
				const NodeId ExpansionLead =
				Syntax.Is(Child, Role::SequenceExpansion) ? FirstLeaf(Syntax, Syntax.NamedChild(Child, 0)) : NoNode;
				HasRestricted = HasRestricted || Syntax.Is(Child, Role::NamedValue) || Syntax.Is(Child, Role::YieldValue) ||
				Syntax.IsTokenRole(ExpansionLead, Role::OpeningBracketLead);
			}
			const bool IsReturn = Syntax.Is(Parent, Role::ReturnStatement) || Syntax.Is(Parent, Role::YieldValue);
			const bool IsSubscript = Syntax.Is(Parent, Role::Subscript) && Syntax.ChildCount(Parent) == 4 && Syntax.Child(Parent, 0) != Id;
			const bool IsTarget = IsLeft && (
				Syntax.Is(Parent, Role::AssignmentExpression) || Syntax.Is(Parent, Role::CountedLoop) ||
				Syntax.Is(Parent, Role::ComprehensionIteration)
			);
			const bool IsValue = Syntax.FieldChild(Parent, ChildRole::Right) == Id &&
			(Syntax.Is(Parent, Role::AssignmentExpression) || Syntax.Is(Parent, Role::CountedLoop));
			// 括弧を要する要素又は配置先の読飛
			if(
				HasRestricted || HasStar && (IsReturn || IsSubscript || Syntax.Is(Parent, Role::CountedLoop)) || !(
					IsReturn || IsSubscript || IsTarget || IsValue || Syntax.Is(Parent, Role::DeleteStatement) || Syntax.Is(Parent, Role::Module) ||
					Syntax.Is(Parent, Role::LexicalBlock)
				)
			) continue;
			// 委譲する組の括弧を保つ為の読飛
			if(Syntax.Is(Parent, Role::YieldValue) && Syntax.IsWord(Syntax.Child(Parent, 1), Word::Delegation)) continue;
			// 添字の組は添字欄の並びへ，値と対象は其々の並びへ投影する
			const AnchorTarget Before { First, false }, After { Syntax.LastNamedChild(Id), true };
			if(IsSubscript) {
				uint32_t Index = Syntax.GetNode(Id).Index;
				for(size_t At = 1; At + 1 < Children.size(); ++At) {
					RewriteStep Move { RewriteKind::Move, Children[At] };
					Move.Destination = Parent;
					Move.Index = Index++;
					Move.DestinationFields = Syntax.IsNamed(Children[At]) ? Syntax.GetNode(Id).Fields : 0;
					Plan.Steps.push_back(std::move(Move));
				}
				Plan.Steps.push_back({ RewriteKind::Remove, Id, {}, { Id, Before, After, Before } });
			} else {
				RewriteStep List { RewriteKind::Reshape, Id, {}, { NoNode, Before, After, Before } };
				List.Inserted = { {}, true };
				List.Inserted.Definition =
				IsTarget ? Syntax.Rules().PatternListDefinition : Syntax.Is(Parent, Role::Module) || Syntax.Is(Parent, Role::LexicalBlock) ?
				Syntax.Rules().TupleExpressionDefinition :
				Syntax.Rules().ExpressionListDefinition;
				List.Inserted.Facts = Syntax.GetNode(Id).Facts;
				for(size_t At = 1; At + 1 < Children.size(); ++At) {
					List.Inserted.Children.push_back({ {}, false, {}, Syntax.GetNode(Children[At]).Fields, Children[At] });
				}
				Plan.Steps.push_back(std::move(List));
			}
			// 此の巡では移す中身を重ねて変更しない事の返戻
			SkippedAncestors[Id] = true;
		}
		// 終了
		return;
	}

	/**
	 * JS/TS の await の式を `await` 名への呼出・添字と誤読した後置の連鎖かの判定関数
	 * 解析器は `await (0, f)(x)` を `await` を被呼出側の名前とする呼出と，`await [a, b][0]` を `await` 名への添字と読む
	 * @param Syntax 構文モデル
	 * @param Node 判定する式
	 * @return 左端が `await` 名への呼出・添字の後置の連鎖なら true
	 */
	bool SyntaxNormalizer::LeadsWithAwaitName(const SyntaxModel &Syntax, NodeId Node) {
		for(; Syntax.Is(Node, Role::AwaitNameReceiver); Node = Syntax.Child(Node, 0)) {
			// 左端の `await` 名での判定（メンバの参照 `await.x` の `await` は真の名前）
			if(const NodeId Head = Syntax.Child(Node, 0); Syntax.Is(Head, Role::Identifier) && Syntax.IsWord(Head, Word::AwaitName)) {
				// 呼出・添字の誤読かの返戻
				return !Syntax.Is(Node, Role::MemberExpression);
			}
		}
		// 誤読の無い事の返戻
		return false;
	}

	/**
	 * 演算子の優先順位の取得関数
	 * @param Syntax 構文モデル
	 * @param OpNode 優先順位を取得する演算子の節点
	 * @param Memo 節点別の優先順位の控え（連なる後置の途中の節点も纏めて埋める）
	 * @return 優先順位（1~18，不明時 0）
	 */
	int SyntaxNormalizer::OpPrecedence(const SyntaxModel &Syntax, const NodeId OpNode, std::unordered_map<NodeId, int> &Memo) {
		const auto Direct = [&Syntax](const NodeId Node) -> int {
			if(Syntax.Is(Node, Role::ComparisonInfix)) {
				// 比較の字面を持つ中置の演算子の優先順位の返戻
				if(Syntax.Rules().ComparisonOperators.contains(Syntax.Text(Syntax.FieldChild(Node, ChildRole::Operator)))) return 7;
			}
			// 波括弧で閉じる Kotlin/Swift のラムダの後置の優先度の返戻
			if(Syntax.Is(Node, Role::LambdaLiteral)) return 16;
			if(Syntax.Is(Node, Role::OperatorCall)) {
				const NodeId Head = Syntax.Child(Node, 0);
				// Swift の呼出での演算子始まりに応じた前置又は後置の優先度の返戻
				return !Syntax.IsNamed(Head) || Syntax.Is(Head, Role::ExclamationMark) ? 15 : 16;
			}
			// 前置単項（Swift の片側だけの範囲 `..<n` と前置の増減 `++x` を含む）の優先順位の返戻
			if(
				Syntax.Is(Node, Role::PrefixUnary) || Syntax.Is(Node, Role::RangeWithoutStart) ||
				Syntax.Is(Node, Role::UpdateExpression) && !Syntax.IsNamed(Syntax.Child(Node, 0))
			) return 15;
			// 後置（Swift の片側だけの範囲 `n...` を含む）の優先順位の返戻
			if(Syntax.Is(Node, Role::PostfixOrUpdateExpression) || Syntax.Is(Node, Role::RangeWithoutEnd)) return 16;
			if(Syntax.Is(Node, Role::SwitchWithPrecedence)) {
				// C# の呼出の後置格の返戻
				if(Syntax.Is(Node, Role::InvocationExpression)) return 16;
				// C# の switch 式と with 式の累乗格の返戻
				if(Syntax.Is(Node, Role::SwitchOrWithExpression)) return 14;
			}
			if(Syntax.Is(Node, Role::KeywordExpressionPrecedence)) {
				// await の単項前置格の返戻
				if(Syntax.Is(Node, Role::AwaitValue)) return 15;
				// 後置のメンバの参照・呼出の優先度の返戻（`(a + b)(c)` の被呼出式の括弧を保護）
				if(Syntax.Is(Node, Role::CallAttributeOrSubscript)) return 16;
				// 字句表に無い三項の優先度の返戻
				if(Syntax.Is(Node, Role::ConditionalExpression)) return 2;
				// not の比較より下で and より上の優先度の返戻（`not (a and b)` / `(not a) == b` の括弧を保持する）
				if(Syntax.Is(Node, Role::LogicalNegation)) return 6;
			}
			// Kotlin の中置関数の優先度の返戻（利用者が順位を定める Swift は対象外）
			if(Syntax.Is(Node, Role::NamedInfixCall)) return 10;
			// Swift の as/as? の優先度の返戻（CastingPrecedence は nil 合体と範囲の間）
			if(Syntax.Is(Node, Role::ComposedTypeCast)) return 9;
			// Ruby のメソッド呼出での受け手の二項式保護用の後置の優先度の返戻
			if(Syntax.Is(Node, Role::CommandCall)) return 16;
			const SyntaxRules &Rules = Syntax.Rules();
			for(const NodeId Child : Syntax.ChildrenOf(Node)) {
				if(Syntax.IsNamed(Child)) continue;
				const std::string_view Operator = Syntax.LexemeOf(Child);
				const std::unordered_map<std::string_view, int>::const_iterator Found = Rules.OperatorPrecedence.find(Operator);
				if(Found == Rules.OperatorPrecedence.end()) continue;
				if(Syntax.Is(Node, Role::IterationHeader) && !Rules.AssignmentOperators.contains(Operator)) {
					// 反復対象の文法が許す最弱の優先順位の返戻
					return Syntax.Is(Node, Role::ComprehensionIteration) ? 3 : 1;
				}
				// 投影された字句の優先順位の返戻
				return Found->second;
			}
			// 演算子を持たない事の返戻
			return 0;
		};
		// 式の優先順位を調べる段階を遷移順に並べる
		enum class Phase : uint8_t { Enter, Prefix, Assertion, Complete };
		struct Frame {
			NodeId Node;
			NodeId Operand = NoNode;
			std::vector<NodeId> Chain {};
			Phase State = Phase::Enter;
		};
		// 入れ子の式と後置の表明を段階毎に評価
		std::vector<Frame> Pending { { OpNode } };
		int Result = 0;
		while(!Pending.empty()) {
			Frame &Current = Pending.back();
			if(Current.State == Phase::Enter) {
				if(
					const NodeId Node = Current.Node;
					Syntax.Is(Node, Role::LowKeywordPrecedence) && (
						Syntax.Is(Node, Role::UnaryValue) && Syntax.Is(Syntax.Child(Node, 0), Role::LowPrecedenceUnaryKeyword) ||
						Syntax.Is(Node, Role::LowPrecedencePatternTest)
					)
				) Result = 1;
				else if(Syntax.Is(Node, Role::TypeColonParent) || Syntax.Is(Node, Role::AngleBracketList)) Result = 0;
				else if(Syntax.Is(Node, Role::OpenPrefixExpression)) Result = 1;
				else if(Syntax.Is(Node, Role::KeywordUnary) || Syntax.Is(Node, Role::PrefixAwait) || LeadsWithAwaitName(Syntax, Node)) {
					Result = 15;
				} else {
					Current.State = Phase::Prefix;
					if(Syntax.Is(Node, Role::PrefixAbsorbsInfix)) {
						Current.Operand = Syntax.LastNamedChild(Node);
						while(Syntax.Is(Current.Operand, Role::PrefixExpression)) Current.Operand = Syntax.LastNamedChild(Current.Operand);
						if(Current.Operand != NoNode) {
							Pending.push_back({ Current.Operand });
							continue;
						}
					}
					Result = 0;
					continue;
				}
				Current.State = Phase::Complete;
				continue;
			}
			if(Current.State == Phase::Prefix) {
				if(Current.Operand != NoNode && Result && Result < 15) {
					Current.State = Phase::Complete;
					continue;
				}
				Current.State = Phase::Assertion;
				Result = 0;
				if(Syntax.Is(Current.Node, Role::NonNullAssertion)) {
					Current.Operand = Syntax.NamedChild(Current.Node, 0);
					for(; !Result && Syntax.Is(Current.Operand, Role::NonNullAssertion); Current.Operand = Syntax.NamedChild(Current.Operand, 0)) {
						if(const std::unordered_map<NodeId, int>::const_iterator At = Memo.find(Current.Operand); At != Memo.end()) {
							Result = At->second;
							continue;
						}
						Current.Chain.push_back(Current.Operand);
					}
					if(!Result && Current.Operand != NoNode) {
						Pending.push_back({ Current.Operand });
						continue;
					}
				} else {
					Result = Direct(Current.Node);
					Current.State = Phase::Complete;
					continue;
				}
			}
			if(Current.State == Phase::Assertion) {
				Result = Result && Result < 16 ? Result : 17;
				for(const NodeId Id : Current.Chain) Memo.insert_or_assign(Id, Result);
				Current.State = Phase::Complete;
				continue;
			}
			Pending.pop_back();
		}
		// 演算子の優先順位の返戻
		return Result;
	}

	/**
	 * 右端の子の取得関数
	 * 閉じの字句で終わる節点（`a[b + c]` の添字は角括弧の中で閉じる）の最後の名前付の子は右端でない
	 * @param Syntax 構文モデル
	 * @param Node 対象の節点
	 * @return 節点の最後の子である名前付の子（無ければ無し）
	 */
	NodeId SyntaxNormalizer::RightEdgeChild(const SyntaxModel &Syntax, const NodeId Node) {
		const NodeId Last = Syntax.Child(Node, Syntax.ChildCount(Node) - 1);
		// 最後の子が名前付なら其の子の返戻
		return Syntax.IsNamed(Last) ? Last : NoNode;
	}

	/**
	 * 後置の連鎖の原子式の判定関数
	 * @param Syntax 構文モデル
	 * @param Node 判定する式
	 * @return 原子式なら true
	 */
	bool SyntaxNormalizer::IsPostfixAtom(const SyntaxModel &Syntax, NodeId Node) {
		// `await` 名への呼出・添字と誤読した await の式は前置単項である事の返戻
		if(LeadsWithAwaitName(Syntax, Node)) return false;
		while(true) {
			// 原子に達した事の返戻
			if(
				Syntax.Is(Node, Role::PostfixReceiverLeaf) ||
				Syntax.Is(Node, Role::Construction) && Syntax.FieldChild(Node, ChildRole::Arguments) != NoNode ||
				Syntax.Is(Node, Role::AtomicConstruction) || Syntax.Is(Node, Role::AtomicNumber)
			) return true;
			const uint32_t Count = Syntax.ChildCount(Node);
			const NodeId Last = Syntax.Child(Node, Count - 1);
			const bool IsChain =
			Syntax.Is(Node, Role::PostfixReceiverChain) || Syntax.Is(Node, Role::PostfixTry) || Syntax.Is(Node, Role::PostfixExpression) &&
			(Syntax.Is(Last, Role::ExclamationMark) || Syntax.IsTokenRole(Last, Role::NonNullAssertionToken));
			// 後置の連鎖でない式・メンバへのポインタ (`.*` / `->*`)・任意の連鎖の返戻
			if(
				!Count || !IsChain || Syntax.Is(Node, Role::FieldExpression) &&
				Syntax.LastTokenRole(Syntax.FieldChild(Node, ChildRole::Operator), Role::PointerMemberAccess) || AnyChildOf(
					Syntax,
					Node,
					[&Syntax](const NodeId Child) -> bool {
						// 後置の連鎖を阻む任意の連鎖又は安全航行の接尾辞かの返戻
						return Syntax.Is(Child, Role::OptionalChain) || Syntax.Is(Child, Role::OptionalNavigationMark) ||
						Syntax.Is(Child, Role::NavigationSuffix) && Syntax.IsTokenRole(Syntax.Child(Child, 0), Role::OptionalAccessToken);
					}
				)
			) return false;
			// 解析器が呼出と読んだ前置・中置式も，先頭を辿って除く
			Node = Syntax.Child(Node, 0);
		}
	}

	/**
	 * 後置の式の受け手を包む括弧の保持判定関数
	 * 受け手（括弧層を透過した内側）が原子式なら，括弧を外しても後置の演算子の結合は変わらない（`(a.b).c` → `a.b.c`）
	 * @param Syntax 構文モデル
	 * @param Paren 受け手を包む括弧
	 * @param Host 後置の式
	 * @return 括弧を保持する場合 true
	 */
	bool SyntaxNormalizer::KeepsPostfixReceiver(const SyntaxModel &Syntax, const NodeId Paren, const NodeId Host) {
		// 一重の括弧を剥離した中身
		NodeId Inner = Paren;
		while(Syntax.Is(Inner, Role::GroupingParen) && Syntax.NamedChildCount(Inner) == 1) Inner = Syntax.NamedChild(Inner, 0);
		if(Syntax.Is(Paren, Role::ConstructionReceiverParen)) {
			NodeId Chain = Paren;
			for(
				NodeId Up = Syntax.ParentOf(Chain);
				Syntax.Is(Up, Role::PostfixExpressionHost) || Syntax.Is(Up, Role::NonNullAssertion);
				Up = Syntax.ParentOf(Chain)
			) {
				if(Syntax.NamedChild(Up, 0) != Chain) break;
				Chain = Up;
			}
			if(Syntax.Is(Syntax.ParentOf(Chain), Role::Construction)) {
				for(NodeId Receiver = Inner; Receiver != NoNode; Receiver = Syntax.NamedChild(Receiver, 0)) {
					// 呼出・任意連鎖を構築の被呼出側へ変えない括弧の保持の返戻
					if(
						Syntax.Is(Receiver, Role::CallExpression) || AnyChildOf(
							Syntax,
							Receiver,
							[&Syntax](const NodeId Child) -> bool {
								return !Syntax.IsNamed(Child) && Syntax.IsTokenRole(Child, Role::OptionalAccessToken);
							}
						) || Syntax.FirstNamedChildWithRole(Receiver, Role::OptionalChain) != NoNode
					) return true;
					if(!Syntax.Is(Receiver, Role::PostfixExpressionHost) && !Syntax.Is(Receiver, Role::NonNullAssertion)) break;
				}
			}
		}
		// Rust の前置単項と左結合の as の型変換の受け手に置ける事の返戻
		if(
			Syntax.Is(Host, Role::UnaryReceiverCast) && (Syntax.Is(Inner, Role::PrefixUnary) || Syntax.Is(Inner, Role::TrailingTypeCast))
		) return false;
		// 被呼出側の名前・欄（C 系は関数形式マクロの展開を止める括弧）と，原子式でない受け手の保持の返戻
		return Syntax.Is(Host, Role::PostfixCallHost) && Syntax.Is(Inner, Role::CalleeNeedsGrouping) || !IsPostfixAtom(Syntax, Inner);
	}

	/**
	 * 前置の演算子と被演算子の先頭の記号の融合判定関数
	 * @param Rules 字句の融合の規則
	 * @param Operator 前置の演算子の末尾の文字
	 * @param Operand 被演算子の先頭の文字
	 * @return 密着させると１字句へ融合するなら true
	 */
	bool SyntaxNormalizer::FusesPrefixOperator(const SyntaxRules &Rules, const char Operator, const char Operand) {
		// 融合するかの返戻（Python・Ruby・Rust は `--` / `++` の字句を持たず，Rust は `&&` を参照の重なりへ分けて読む）
		const char Pair[] { Operator, Operand };
		// 投影された融合の組に含まれるかの返戻
		return Rules.FusedPrefixPairs.contains(std::string_view(Pair, 2));
	}

	/**
	 * 前置の演算子の呼出と読んだ Swift の括弧の実引数の並びの取得関数
	 * tree-sitter-swift は前置の演算子の後の括弧 (`-(y)` / `!(y)`) を演算子の呼出と読む
	 * 否定の `!` は名前付の `bang` の節点に為り，中置の式の後の前置 (`x + -(y)`) は其の中置の式の末尾の字句に為る（`(x + -)(y)` と読む）
	 * @param Syntax 構文モデル
	 * @param Node 節点
	 * @return 前置の演算子の被演算子を包む括弧と読んだ実引数の並び（其の形でなければ無し）
	 */
	NodeId SyntaxNormalizer::PrefixCallArguments(const SyntaxModel &Syntax, const NodeId Node) {
		// 前置の演算子の呼出の形でない事の返戻
		if(Syntax.ChildCount(Node) != 2 || !Syntax.Is(Node, Role::OperatorCall)) return NoNode;
		// 呼出側の前置の演算子（中置の式の末尾の字句を含む）
		NodeId Prefix = Syntax.Child(Node, 0);
		if(Syntax.IsNamed(Prefix) && !Syntax.Is(Prefix, Role::ExclamationMark)) {
			const uint32_t Count = Syntax.ChildCount(Prefix);
			Prefix = Count > 1 ? Syntax.Child(Prefix, Count - 1) : NoNode;
			// 末尾が演算子の字句でない事の返戻
			if(Prefix == NoNode || Syntax.IsNamed(Prefix)) return NoNode;
		}
		const NodeId Suffix = Syntax.Child(Node, 1);
		// 前置の演算子と実引数の並びの組でない事の返戻
		if(
			!Syntax.Rules().FusesOperatorChar(Syntax.FirstChar(Prefix)) || !Syntax.Is(Suffix, Role::CallSuffix) ||
			Syntax.NamedChildCount(Suffix) != 1
		) return NoNode;
		const NodeId Arguments = Syntax.NamedChild(Suffix, 0);
		// 実引数の並びの返戻
		return Syntax.Is(Arguments, Role::ValueArguments) ? Arguments : NoNode;
	}

	/**
	 * 制御構文の見出で本体の塊と紛れる式を持つかの判定関数
	 * Go の型名の複合リテラル，Rust の構造体の式，Swift の後置のクロージャは，括弧を外すと `{` が本体の開きと読まれる
	 * 実引数・配列・添字・文字列等の区切りの中は紛れない（要素１つの括弧と Swift の前置の演算子の被演算子の括弧は外れ得る為に透過して見る）
	 * @param Syntax 構文モデル
	 * @param Node 判定する式
	 * @return 紛れる式を区切の括弧の外に持てば true
	 */
	bool SyntaxNormalizer::HasBodyLikeExpression(const SyntaxModel &Syntax, const NodeId Node) {
		// 透過する Swift の前置の演算子の被演算子の括弧
		std::unordered_set<NodeId> PrefixArguments;
		// 言語毎の紛れる式の探索結果の返戻
		return WalkNodes(
			Syntax,
			Node,
			[&](const NodeId Target) -> WalkStep {
				// 紛れる式で止める事の返戻
				if(
					Syntax.Is(Target, Role::NamedCompositeBody) && Syntax.Is(Syntax.FieldChild(Target, ChildRole::Type), Role::CompositeTypeName) ||
					Syntax.Is(Target, Role::HeaderBodyLikeValue)
				) return WalkStep::Stop;
				if(Syntax.Is(Target, Role::OperatorCall)) {
					if(const NodeId Arguments = PrefixCallArguments(Syntax, Target); Arguments != NoNode) PrefixArguments.insert(Arguments);
				}
				const NodeId First = Syntax.Child(Target, 0);
				const bool IsTransparent = Syntax.Is(Target, Role::ValueParentheses) ||
				Syntax.NamedChildCount(Target) == 1 && (Syntax.Is(Target, Role::TransparentTupleParen) || PrefixArguments.contains(Target));
				// 区切りの括弧・引用符の字句で始まる節点（透過する括弧を除く）の中へ降りない事の返戻
				return Target == Node || First == NoNode || Syntax.IsNamed(First) || IsTransparent ||
				!Syntax.FirstTokenRole(First, Role::BodyDelimiterLead) ? WalkStep::Descend : WalkStep::Skip;
			}
		);
	}

	/**
	 * TS の型引数の閉じ `>` の直後に来る被演算子かの判定関数
	 * TS の比較式を型引数付の式へ変える先頭字を判定し，外れる括弧（内側が比較とシフトより強く結合し，括弧必須の式でもない）は透過して内側の先頭を見る
	 * @param Syntax 構文モデル
	 * @param Operand 比較の右の被演算子
	 * @param Memo 優先順位の控え
	 * @return 整形後に `(` `\`` `+` `-` `[` で始まるなら true
	 */
	bool SyntaxNormalizer::OpensTypeArguments(const SyntaxModel &Syntax, NodeId Operand, std::unordered_map<NodeId, int> &Memo) {
		while(Syntax.Is(Operand, Role::ValueParentheses)) {
			const NodeId Inner = Syntax.NamedChild(Operand, 0);
			const int Prec = Inner == NoNode ? 0 : OpPrecedence(Syntax, Inner, Memo);
			// 残る括弧で始まる事の返戻
			if(Syntax.NamedChildCount(Operand) != 1 || Prec && Prec < 12 || Syntax.Is(Inner, Role::ParenInnerNoUnwrap)) return true;
			Operand = Inner;
		}
		// 型引数として開き得る先頭の字かの返戻
		return Operand != NoNode && Syntax.FirstTokenRole(Operand, Role::TypeArgumentOpeningLead);
	}

	/**
	 * 被演算子の逆側で先に結合する演算子の優先順位の取得関数
	 * 被演算子から括弧の層を透過して祖先を遡り，被演算子と逆の側へ初めて被演算子が現れる演算子の優先順位を返す
	 * @param Syntax 構文モデル
	 * @param OpParent 括弧の層を透過した演算子の祖先
	 * @param IsLeftOperand 被演算子が左か
	 * @param Memo 優先順位の控え
	 * @return 逆側の演算子の優先順位（無ければ０）
	 */
	int SyntaxNormalizer::OppositePrecOf(
		const SyntaxModel &Syntax,
		const NodeId OpParent,
		const bool IsLeftOperand,
		std::unordered_map<NodeId, int> &Memo
	) {
		NodeId Child = OpParent;
		for(NodeId Ancestor = Syntax.ParentOf(Child); Ancestor != NoNode; Child = Ancestor, Ancestor = Syntax.ParentOf(Ancestor)) {
			const int AncestorPrec = OpPrecedence(Syntax, Ancestor, Memo);
			if(!AncestorPrec) {
				// 括弧の層の透過と，演算子でない祖先での終了
				if(Syntax.Is(Ancestor, Role::GroupingParen)) continue;
				break;
			}
			const bool IsChildLeft = Syntax.NamedChild(Ancestor, 0) == Child;
			// 右隣を持たない前置単項の透過
			if(!IsLeftOperand && IsChildLeft && !Syntax.IsNamed(Syntax.Child(Ancestor, 0))) continue;
			// 左右が初めて入れ替わる祖先の優先順位の返戻
			if(IsLeftOperand ? !IsChildLeft : IsChildLeft) return AncestorPrec;
		}
		// 逆側の演算子の無い事の返戻
		return 0;
	}

	/**
	 * 対の括弧を開く節点かの判定関数
	 * @param Syntax 構文モデル
	 * @param Node 節点
	 * @return 最初の子が開きの丸括弧・角括弧・波括弧の字句なら true
	 */
	bool SyntaxNormalizer::OpensBracketPair(const SyntaxModel &Syntax, const NodeId Node) {
		const NodeId First = Syntax.Child(Node, 0);
		// 開きの括弧の字句かの返戻
		return First != NoNode && !Syntax.IsNamed(First) && (
			Syntax.IsTokenRole(First, Role::OpeningParenToken) || Syntax.IsTokenRole(First, Role::OpeningBracketToken) ||
			Syntax.IsTokenRole(First, Role::OpeningBraceToken)
		);
	}

	/**
	 * 語の型への変換かの判定関数
	 * 型が語の基本型（抽象のポインタの宣言子だけを伴う）の変換は型と読むしかなく，名前の型は括弧の式・呼出とも読める
	 * @param Syntax 構文モデル
	 * @param Cast 型の変換の節点
	 * @return 語の基本型への変換なら true
	 */
	bool SyntaxNormalizer::IsKeywordTypeCast(const SyntaxModel &Syntax, const NodeId Cast) {
		// 型の変換以外の返戻
		if(!Syntax.Is(Cast, Role::CastExpression)) return false;
		const NodeId Descriptor = Syntax.FieldChild(Cast, ChildRole::Type);
		// 型の記述の子が語の基本型と抽象のポインタの宣言子だけかの返戻
		return Syntax.NamedChildCount(Descriptor) && !AnyChildOf(
			Syntax,
			Descriptor,
			[&Syntax](const NodeId Child) -> bool {
				// 語の基本型・抽象のポインタの宣言子以外の名前付の子かの返戻
				return Syntax.IsNamed(Child) && !Syntax.Is(Child, Role::PrimitiveType) && !Syntax.Is(Child, Role::SizedTypeSpecifier) &&
				!Syntax.Is(Child, Role::TypeQualifier) && !(
					Syntax.Is(Child, Role::AbstractPointer) && !AnyNodeOf(
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
	 * 外すと宣言と読まれ得る括弧かの判定関数（整形仕様 5.2 の型と式の曖昧さ）
	 * 名前が型か判別出来ない記法では型の名前の呼出の形 (`T(x)`) が宣言子とも読める為，式の文の先頭の其の形を包む括弧と，式の文から型の名前の単独の実引数を辿った呼出の実引数と直接の初期化の実引数で其の形を包む括弧を保つ
	 * @param Syntax 構文モデル
	 * @param Paren 括弧の最も外の層
	 * @param Inner 括弧の層を透過した中身
	 * @return 括弧を外すと宣言と読まれ得るなら true
	 */
	bool SyntaxNormalizer::ReadsAsDeclaration(const SyntaxModel &Syntax, const NodeId Paren, const NodeId Inner) {
		// 型の名前とも読める被呼出側の呼出の形か
		const auto IsTypeNameCall = [&Syntax](const NodeId Node) -> bool {
			const NodeId Callee = Syntax.FieldChild(Node, ChildRole::Function);
			// 名前・修飾付の名前・型引数を伴う名前・基本型を被呼出側に持つ呼出かの返戻
			return Syntax.Is(Node, Role::CallNode) && (
				Syntax.Is(Callee, Role::Identifier) || Syntax.Is(Callee, Role::QualifiedIdentifier) ||
				Syntax.Is(Callee, Role::GenericFunction) || Syntax.Is(Callee, Role::PrimitiveType)
			);
		};
		const NodeId Parent = Syntax.ParentOf(Paren);
		if(Syntax.Is(Parent, Role::ExpressionStatement)) {
			// 代入の左辺と添字の受け手を辿った先頭の部分式
			NodeId Head = Inner;
			while(Syntax.Is(Head, Role::Assignment) || Syntax.Is(Head, Role::SubscriptExpressionNode)) Head = Syntax.NamedChild(Head, 0);
			// 式の文の先頭が型の名前の呼出かの返戻
			return IsTypeNameCall(Head);
		}
		// 実引数の並びでない括弧と，型の名前の呼出を包まない括弧の返戻
		if(!Syntax.Is(Parent, Role::ArgumentSequence) || !IsTypeNameCall(Inner)) return false;
		NodeId Call = Syntax.ParentOf(Parent);
		// 直接の初期化の実引数の返戻
		if(Syntax.Is(Call, Role::InitDeclarator)) return true;
		// 式の文から型の名前の単独の実引数を辿った呼出の実引数かの判定
		while(IsTypeNameCall(Call) && Syntax.NamedChildCount(Syntax.FieldChild(Call, ChildRole::Arguments)) == 1) {
			Syntax.Charge(1);
			const NodeId Up = Syntax.ParentOf(Call);
			// 式の文の先頭の呼出迄辿れた事の返戻
			if(Syntax.Is(Up, Role::ExpressionStatement)) return true;
			// 単独の実引数でない呼出の返戻
			if(!Syntax.Is(Up, Role::ArgumentSequence)) return false;
			Call = Syntax.ParentOf(Up);
		}
		// 宣言と読めない位置の返戻
		return false;
	}

	/**
	 * 冗長な式の括弧を外すかの判定関数（整形仕様 5.2）
	 * 親の構文が要する括弧・結合を変える括弧・読みを変える括弧を保ち，其の他の括弧を外す
	 * @param Syntax 構文モデル
	 * @param Paren 括弧の節点
	 * @param Inner 括弧の層を透過した中身
	 * @param IsInBraceHeader 制御構文の見出の中か
	 * @param Memo 優先順位の控え
	 * @param AngleMemo 山括弧に関わる字句の有無の控え
	 * @param ScanMemo 端と連鎖の判定の控え
	 * @return 括弧を保つなら true
	 */
	bool SyntaxNormalizer::KeepsExpressionParen(
		const SyntaxModel &Syntax,
		const NodeId Paren,
		const NodeId Inner,
		const bool IsInBraceHeader,
		std::unordered_map<NodeId, int> &Memo,
		std::unordered_map<NodeId, bool> &AngleMemo,
		ParenScanMemo &ScanMemo
	) {
		const NodeId Parent = Syntax.ParentOf(Paren), Previous = Syntax.PrevSibling(Paren), Next = Syntax.NextSibling(Paren);
		// 最後の字句の葉の控え付の探索
		const auto LastLeafOf = [&Syntax, &ScanMemo](NodeId Node) -> NodeId {
			std::vector<NodeId> Path;
			while(Node < Syntax.NodeCount() && !ScanMemo.LastLeaf.contains(Node)) {
				Syntax.Charge(1);
				Path.push_back(Node);
				const std::span<const NodeId> Children = Syntax.ChildrenOf(Node);
				if(Children.empty()) break;
				Node = Children.back();
			}
			if(ScanMemo.LastLeaf.contains(Node)) {
				Syntax.Charge(1);
				Node = ScanMemo.LastLeaf.at(Node);
			} else if(Node >= Syntax.NodeCount()) Node = NoNode;
			for(const NodeId Visited : Path) ScanMemo.LastLeaf.emplace(Visited, Node);
			// 最後の葉の返戻
			return Node;
		};
		// マクロ展開後の結合を守る括弧と，外すと宣言と読まれ得る括弧の保持の返戻
		if(
			Syntax.GetNode(Paren).Facts.Has(ConstraintKind::MacroSensitiveParen) ||
			Syntax.Rules().CallFormsMayDeclare && ReadsAsDeclaration(Syntax, Paren, Inner)
		) return true;
		if(Syntax.Is(Paren, Role::CommandArgumentParen)) {
			const NodeId Arguments = Syntax.FieldChild(Inner, ChildRole::Arguments);
			const bool IsCommand = Syntax.Is(Inner, Role::ReceiverCall) && Arguments != NoNode &&
			!Syntax.IsTokenRole(Syntax.Child(Arguments, 0), Role::OpeningParenToken);
			// 括弧無の呼出の実引数が後続の字句を取り込む包装の返戻
			if(
				IsCommand && (
					Syntax.Is(Parent, Role::ElementReference) || Syntax.Is(Parent, Role::ReceiverCall) ||
					Syntax.Is(Parent, Role::ConditionalOperator)
				)
			) return true;
			// 数値の受け手の前の符号を，数値自身の符号へ変えない包装の返戻
			if(
				Syntax.Is(Inner, Role::NumberLiteral) && Syntax.Is(Parent, Role::ReceiverCall) &&
				Syntax.Is(Syntax.ParentOf(Parent), Role::UnaryValue)
			) return true;
			// 代入の右側の not は引数の括弧を要する事の返戻
			if(
				Syntax.Is(Parent, Role::UnaryValue) && Syntax.IsWord(Syntax.Child(Parent, 0), Word::Negation) &&
				!Syntax.Is(Syntax.ParentOf(Parent), Role::CommandStatementHost)
			) return true;
			// 数値で始まる累乗を符号付の値へ結合させない包装の返戻
			if(
				Syntax.Is(Parent, Role::UnaryValue) && (
					Syntax.Text(Syntax.Child(Parent, 0)) == Syntax.Rules().Operators.Minus ||
					Syntax.Text(Syntax.Child(Parent, 0)) == Syntax.Rules().Operators.Plus
				) && Syntax.Is(Inner, Role::Binary)
			) {
				NodeId First = Inner;
				while(Syntax.ChildCount(First)) First = Syntax.Child(First, 0);
				// 数値の前の符号と累乗の結合を保つ包装の返戻
				if(Syntax.Is(First, Role::NumberLiteral)) return true;
			}
		}
		if(Syntax.Is(Paren, Role::ConstructionReceiverParen)) {
			// 括弧を透過した先頭の字句と，其の後の構文上の字句
			NodeId Lead = Inner;
			while(Syntax.ChildCount(Lead)) Lead = Syntax.Is(Lead, Role::GroupingParen) ? Syntax.NamedChild(Lead, 0) : Syntax.Child(Lead, 0);
			const auto Following = [&](NodeId Node) -> NodeId {
				while(Node != NoNode && Syntax.NextSibling(Node) == NoNode) Node = Syntax.ParentOf(Node);
				// 後続の節点の返戻
				return Syntax.NextSibling(Node);
			};
			NodeId AfterHead = Following(Lead);
			while(Syntax.IsTokenRole(AfterHead, Role::ClosingParenToken)) AfterHead = Following(AfterHead);
			// 先頭の語の後の最初の字句（非同期の関数の宣言の語か，束縛の後の括弧かを見る）
			NodeId AfterLeaf = AfterHead;
			while(Syntax.ChildCount(AfterLeaf)) AfterLeaf = Syntax.Child(AfterLeaf, 0);
			const bool IsDeclaration = Syntax.IsWord(Lead, Word::StatementDeclarationHead) ||
			Syntax.IsWord(Lead, Word::AsyncModifier) && Syntax.IsWord(AfterLeaf, Word::StatementDeclarationHead);
			const bool IsLetBracket =
			Syntax.IsWord(Lead, Word::ReassignableBinding) && Syntax.IsTokenRole(FirstLeaf(Syntax, AfterHead), Role::OpeningBracketToken);
			// 指示文へ変わる文字列を包む括弧の保持の返戻
			if(Syntax.Is(Parent, Role::ExpressionStatement) && Syntax.Is(Inner, Role::QuotedString)) return true;
			for(NodeId Child = Paren, Up = Parent; Up != NoNode; Child = Up, Up = Syntax.ParentOf(Up)) {
				// 本体の塊・宣言・反復の区切りへ変わる括弧の保持の返戻
				if(Syntax.Is(Up, Role::ArrowFunction)) {
					// 式の本体を塊へ変えない括弧の保持の返戻
					if(Syntax.FieldChild(Up, ChildRole::Body) == Child && Syntax.IsTokenRole(Lead, Role::OpeningBraceToken)) return true;
					break;
				}
				if(Syntax.Is(Up, Role::Export)) {
					// 公開する式を宣言へ変えない括弧の保持の返戻
					if(IsDeclaration) return true;
					break;
				}
				if(Syntax.Is(Up, Role::Iteration)) {
					// 反復の束縛と紛れない括弧の保持の返戻
					if(
						Syntax.FieldChild(Up, ChildRole::Left) == Child && (
							IsLetBracket || (Syntax.IsWord(Lead, Word::ReassignableBinding) || Syntax.IsWord(Lead, Word::AsyncModifier)) &&
							Syntax.IsWord(Syntax.NextSibling(Child), Word::IterationOf)
						)
					) return true;
					break;
				}
				if(Syntax.Is(Up, Role::UpdateExpression) && Syntax.Is(Syntax.ParentOf(Up), Role::ExpressionStatement)) break;
				if(Syntax.Child(Up, 0) != Child) break;
				if(Syntax.Is(Up, Role::ExpressionStatement)) {
					// 文頭の解釈を保つ括弧の保持の返戻
					if(Syntax.IsTokenRole(Lead, Role::OpeningBraceToken) || IsDeclaration || IsLetBracket) return true;
					break;
				}
			}
			// 型を表明した代入先の括弧の保持の返戻
			if(
				Syntax.Is(Inner, Role::TypeAssertion) && (Syntax.Is(Parent, Role::Assignment) || Syntax.Is(Parent, Role::Iteration)) &&
				Syntax.FieldChild(Parent, ChildRole::Left) == Paren
			) return true;
			// 総称呼出を比較と読んだ場合の実引数の括弧の保持の返戻
			for(NodeId Child = Paren, Up = Parent; Up != NoNode; Child = Up, Up = Syntax.ParentOf(Up)) {
				// 総称呼出の実引数の括弧の保持の返戻
				if(Syntax.GetNode(Up).Facts.Has(ConstraintKind::MisparsedGenericCall)) return true;
				if(
					Syntax.Is(Up, Role::BinaryOperation) && Syntax.FieldChild(Up, ChildRole::Right) == Child &&
					Syntax.IsTokenRole(Syntax.FieldChild(Up, ChildRole::Operator), Role::GreaterThanToken)
				) {
					NodeId Earlier = Syntax.PrevSibling(Up);
					while(Earlier != NoNode && !Syntax.IsNamed(Earlier)) Earlier = Syntax.PrevSibling(Earlier);
					// 比較と誤読した型引数の後の括弧の保持の返戻
					if(
						Syntax.IsTokenRole(Syntax.FieldChild(Syntax.FieldChild(Up, ChildRole::Left), ChildRole::Operator), Role::LessThanToken) ||
						Syntax.Is(Syntax.ParentOf(Up), Role::TypeArgumentLookalikeSlot) &&
						Syntax.IsTokenRole(Syntax.FieldChild(Earlier, ChildRole::Operator), Role::LessThanToken)
					) return true;
				}
				if(Syntax.Child(Up, 0) != Child) break;
			}
			const NodeId Follower = Following(Paren);
			if(
				const LexicalEdge FollowingEdge = Syntax.FirstEdge(Follower);
				Syntax.IsEdgeRole(FollowingEdge, Role::InstantiationContinuationLead) || FollowingEdge.Leaf != NoNode &&
				(Syntax.IsEdgeRole(FollowingEdge, Role::ExclamationToken) || Syntax.IsEdgeRole(FollowingEdge, Role::OptionalAccessToken)) &&
				Syntax.LastEdge(Follower).Leaf == FollowingEdge.Leaf
			) for(NodeId Edge = Inner; Edge != NoNode; Edge = RightEdgeChild(Syntax, Edge)) {
				// 型引数の具体化を比較や呼出へ変える括弧の保持の返戻
				if(Syntax.Is(Edge, Role::Instantiation)) return true;
			}
			// for の初期化部の in を反復の区切りへ変えない
			for(NodeId Child = Paren, Up = Parent; Up != NoNode; Child = Up, Up = Syntax.ParentOf(Up)) {
				if(Syntax.Is(Up, Role::CountedLoop)) {
					// 初期化部の in を保つ括弧の保持の返戻
					if(
						Syntax.GetNode(Child).Index < 3 && WalkNodes(
							Syntax,
							Inner,
							[&](const NodeId Node) -> WalkStep {
								// 保護する in の発見時の返戻
								if(Syntax.IsWord(Syntax.FieldChild(Node, ChildRole::Operator), Word::In)) return WalkStep::Stop;
								// 文法上 in を許す区切りの内側へ降りない事の返戻
								return !Syntax.Is(Node, Role::GroupingParen) && Syntax.Is(Node, Role::MembershipExpressionBoundary) ?
								WalkStep::Skip :
								WalkStep::Descend;
							}
						)
					) return true;
					break;
				}
				if(
					Syntax.Is(Up, Role::StatementHost) || Syntax.Is(Up, Role::MembershipExpressionBoundary) && !Syntax.Is(Up, Role::GroupingParen)
				) break;
			}
		}
		// 型と式の文法の境界を保つ括弧の返戻
		if(Syntax.Is(Parent, Role::TypeSyntax)) return true;
		// 型又は式の読みを保つ括弧の返戻
		if(
			Syntax.Is(Parent, Role::RestrictedExpressionOperand) && (
				Syntax.Is(Parent, Role::ComprehensionFilter) && Syntax.Is(Inner, Role::ConditionalExpression) ||
				Syntax.Is(Parent, Role::Attribute) && Syntax.Is(Inner, Role::AttributeSensitiveNumber) ||
				Syntax.Is(Parent, Role::AwaitValue) && (Syntax.Is(Inner, Role::AwaitValue) || Syntax.Is(Inner, Role::PrefixUnary))
			)
		) return true;
		if(Syntax.Is(Paren, Role::ReferenceValueParen)) {
			// 結付と型の読みを保つ括弧の返戻
			if(
				Syntax.Is(Parent, Role::ConditionalExpression) && Syntax.Is(Inner, Role::ConditionalExpression) ||
				Syntax.IsWord(Previous, Word::TypeTest)
			) return true;
			if(Syntax.Is(Parent, Role::ReturnValueParent)) {
				NodeId Owner = Parent;
				while(Owner != NoNode && !Syntax.Is(Owner, Role::ReturnValueHost)) Owner = Syntax.ParentOf(Owner);
				// 参照を値へ変える括弧の保持の返戻
				if(Syntax.FirstNamedChildWithRole(Owner, Role::ReferenceModifier) != NoNode) return true;
			}
		}
		const bool IsInDelimiter = !Syntax.IsNamed(Previous) && Syntax.IsWord(Previous, Word::In);
		// in の右辺はコンマ式を直接置ける事の返戻
		if(Syntax.Is(Parent, Role::Iteration) && IsInDelimiter) return false;
		// 親の構文の区切りと受け手の位置の判定
		const bool IsAfterOwnDelimiter = Syntax.Is(Parent, Role::ForEachHeader) ?
		IsInDelimiter || !Syntax.IsNamed(Previous) && Syntax.IsWord(Previous, Word::IterationOf) ||
		Syntax.IsTokenRole(Previous, Role::ColonToken) || Syntax.IsTokenRole(Previous, Role::OpeningParenToken) &&
		(Syntax.Is(Parent, Role::ForInStatement) || Syntax.Is(Parent, Role::DelimitedIterationTarget)) :
		(Syntax.IsTokenRole(Previous, Role::OpeningParenToken) || Syntax.IsTokenRole(Previous, Role::OpeningBracketToken)) &&
		!Syntax.Is(Parent, Role::ValueCategoryType);
		const bool IsPostfixReceiver = (
			Syntax.Is(Parent, Role::NonNullAssertion) || Syntax.Is(Parent, Role::PostfixExpressionHost) ||
			Syntax.Is(Parent, Role::PostfixValueOperation)
		) && Syntax.Child(Parent, 0) == Paren || Syntax.Is(Parent, Role::TypeSwitch) && Syntax.IsTokenRole(Next, Role::DotToken);
		bool IsConstructible = Syntax.Is(Parent, Role::Construction);
		if(IsConstructible) {
			NodeId Receiver = Inner;
			while(Syntax.Is(Receiver, Role::PostfixExpressionHost) || Syntax.Is(Receiver, Role::NonNullAssertion)) {
				if(
					Syntax.Is(Receiver, Role::CallExpression) &&
					!Syntax.Is(Syntax.FieldChild(Receiver, ChildRole::Arguments), Role::InterpolatedLiteral) || AnyChildOf(
						Syntax,
						Receiver,
						[&Syntax](const NodeId Child) -> bool {
							return !Syntax.IsNamed(Child) && Syntax.IsTokenRole(Child, Role::OptionalAccessToken);
						}
					) || Syntax.FirstNamedChildWithRole(Receiver, Role::OptionalChain) != NoNode
				) {
					IsConstructible = false;
					break;
				}
				Receiver = Syntax.NamedChild(Receiver, 0);
			}
		}
		// 見出の括弧を省ける言語か
		const bool IsHeaderParenOptional = Syntax.Is(Parent, Role::OptionalConditionParen);
		// 親の構文に必須の括弧（省ける見出・原子の受け手・親自身の区切りの後・C# の switch の対象・優先順位で決まる C 系の語の型への変換の値を除く）の保持の返戻
		if(
			(
				Syntax.Is(Parent, Role::StructuralParenBoundary) || Syntax.Is(Parent, Role::MeaningfulParenGroup) ||
				Syntax.Is(Parent, Role::ReceiverParenHost) || Syntax.Is(Parent, Role::PostfixAwait)
			) && !(
				IsHeaderParenOptional || IsPostfixReceiver && !KeepsPostfixReceiver(Syntax, Paren, Parent) || IsAfterOwnDelimiter ||
				IsConstructible || Syntax.Is(Parent, Role::UnwrappedSwitchSubject) ||
				Syntax.Is(Parent, Role::KeywordCastOperand) && IsKeywordTypeCast(Syntax, Parent)
			)
		) return true;
		// 式の包装でない複数の名前付の子を持つ括弧と，実の組・ラベル・複文の括弧の保持の返戻
		if(
			Syntax.NamedChildCount(Paren) > 1 || !Syntax.Is(Paren, Role::ValueParentheses) && AnyChildOf(
				Syntax,
				Paren,
				[&Syntax](const NodeId Child) -> bool {
					// 組・ラベル・複文の区切りの字句かの返戻
					return Syntax.IsTokenRole(Child, Role::CommaToken) || Syntax.IsTokenRole(Child, Role::ColonToken) ||
					Syntax.IsTokenRole(Child, Role::StatementTerminatorToken);
				}
			)
		) return true;
		// Go の chan で始まる受信値の括弧（受信専用のチャネル型への変換と読まれる）の保持の返戻
		if(
			Syntax.Is(Parent, Role::ChannelReceive) && Syntax.IsTokenRole(Syntax.Child(Parent, 0), Role::ChannelReceiveToken) &&
			Syntax.IsWord(Syntax.Child(Inner, 0), Word::Channel)
		) return true;
		if(Syntax.Is(Inner, Role::RangeLike) && !Syntax.IsNamed(Syntax.Child(Inner, Syntax.ChildCount(Inner) - 1))) {
			// 括弧の後の字句（親に無ければ祖先の後）
			NodeId Following = Paren;
			while(Following != NoNode && Syntax.NextSibling(Following) == NoNode) Following = Syntax.ParentOf(Following);
			const char Close = Following == NoNode ? '\0' : Syntax.FirstChar(Syntax.NextSibling(Following));
			// 終端の無い範囲が後続を取り込む括弧（範囲を閉じる区切りの前は除く）の保持の返戻
			if(Close == '\0' || Syntax.Rules().OpenRangeClosers.find(Close) == std::string_view::npos) return true;
		}
		// 括弧の層を透過した演算子の祖先と，其の直下の被演算子
		NodeId OpParent = Parent, Operand = Paren;
		while(Syntax.Is(OpParent, Role::GroupingParen)) {
			Operand = OpParent;
			OpParent = Syntax.ParentOf(OpParent);
		}
		if(Syntax.Is(Paren, Role::ConditionalAccessParen) && !Syntax.Is(Inner, Role::GroupingParen)) {
			// 後置の `!` を透過した後置の連鎖の祖先
			NodeId Chain = OpParent;
			while(Syntax.Is(Chain, Role::PostfixUnaryExpression)) Chain = Syntax.ParentOf(Chain);
			if(Syntax.Is(Chain, Role::ConditionalAccessChain)) for(NodeId Target = Inner; Target != NoNode;) {
				// 後置の `!` を介した連鎖を null 条件の連鎖へ取り込ませない括弧の保持の返戻
				if(Syntax.Is(Target, Role::ConditionalAccessExpression)) return true;
				if(!Syntax.Is(Target, Role::ConditionalAccessChain)) break;
				const bool IsPostfixUnary = Syntax.Is(Target, Role::PostfixUnaryExpression);
				Target = Syntax.NamedChild(Target, 0);
				// 後置の `!` の内側だけの括弧を透過する
				if(!IsPostfixUnary && Syntax.Is(Target, Role::GroupingParen)) break;
				while(Syntax.Is(Target, Role::GroupingParen)) Target = Syntax.NamedChild(Target, 0);
			}
		}
		// 型引数の並びの中の比較の括弧（括弧の対を開かない祖先は透過）の保持
		if(HasAngleConflict(Syntax, Paren, AngleMemo)) for(NodeId Up = Parent; Up != NoNode; Up = Syntax.ParentOf(Up)) {
			const NodeId Head = Syntax.Child(Up, 0);
			if(Syntax.IsNamed(Head)) continue;
			// 型引数の並びの中の括弧の保持の返戻
			if(Syntax.Is(Up, Role::AngleBracketList)) return true;
			if(Syntax.IsBracketLead(Head, Role::OpeningBracketLead)) break;
		}
		if(Syntax.Is(Paren, Role::TypeArgumentAmbiguousParen)) {
			// 一覧の要素の中の比較・シフトが型引数と紛れる括弧（外すとカンマの帰属が変わる）の保持
			const bool IsListElement = Syntax.Is(Parent, Role::TypeArgumentLookalikeSlot);
			const NodeId List = Syntax.Is(Parent, Role::Argument) ? Syntax.ParentOf(Parent) : Parent;
			// 括弧の後の一覧の中に閉じの山括弧に読める字句が在るか
			const auto HasLaterClose = [&]() -> bool {
				// 括弧の後の山括弧の字句の返戻
				return AnyNodeOf(
					Syntax,
					List,
					[&](const NodeId Node) -> bool {
						// 括弧の後の無名の字句で `>` を含む物かの返戻
						return !Syntax.IsNamed(Node) && Syntax.Precedes(Paren, Node) && Syntax.IsTokenRole(Node, Role::AngleCloseCandidate);
					}
				);
			};
			bool ShouldKeep = false;
			WalkNodes(
				Syntax,
				Paren,
				[&](const NodeId Node) -> WalkStep {
					// 括弧自身から中身へ降りる事の返戻
					if(Node == Paren) return WalkStep::Descend;
					// 字句の内側へ降りない事の返戻
					if(!Syntax.IsNamed(Node)) return WalkStep::Skip;
					if(Syntax.Is(Node, Role::AngleBracketList)) {
						ShouldKeep = Syntax.GetNode(Node).Facts.Has(ConstraintKind::MisparsedTemplate) && AnyChildOf(
							Syntax,
							Node,
							[&Syntax](const NodeId Child) -> bool {
								// 並びのカンマかの返戻
								return Syntax.IsTokenRole(Child, Role::CommaToken);
							}
						);
						// 読み違えた型引数の並びのカンマで保持を決め，中へ降りない事の返戻
						return ShouldKeep ? WalkStep::Stop : WalkStep::Skip;
					}
					ShouldKeep = IsListElement && Syntax.Is(Node, Role::BinaryOperation) &&
					Syntax.IsTokenRole(Syntax.FieldChild(Node, ChildRole::Operator), Role::LessThanToken) && HasLaterClose();
					// 保持が決まった後の終了と，対の括弧の外側だけへ降りる事の返戻
					return ShouldKeep ? WalkStep::Stop : OpensBracketPair(Syntax, Node) ? WalkStep::Skip : WalkStep::Descend;
				}
			);
			// 型引数と紛れる括弧の保持の返戻
			if(ShouldKeep) return true;
		}
		if(Syntax.Is(Inner, Role::CastBeforeUnary)) {
			const NodeId Value = Syntax.LastNamedChild(Inner);
			// 二項式を型の変換と読み違えた式の外側の括弧の保持の返戻
			if(
				Syntax.FirstTokenRole(Value, Role::CastOperandLead) &&
				(Syntax.Is(Inner, Role::KeywordCastOperand) || Syntax.Is(Syntax.FieldChild(Inner, ChildRole::Type), Role::GenericName))
			) return true;
		}
		if(Syntax.Is(Inner, Role::DeducedValueCategoryName)) {
			// 括弧の層を透過した最初の祖先
			NodeId Host = Parent;
			while(Syntax.Is(Host, Role::GroupingParen)) Host = Syntax.ParentOf(Host);
			// 部分木が `decltype(auto)` を含むか
			const auto HasDecltypeAuto = [&Syntax](const NodeId Root) -> bool {
				// `auto` を中身に持つ decltype の有無の返戻
				return AnyNodeOf(
					Syntax,
					Root,
					[&Syntax](const NodeId Node) -> bool {
						// `decltype(auto)` かの返戻
						return Syntax.Is(Node, Role::ValueCategoryType) && Syntax.Is(Syntax.NamedChild(Node, 0), Role::DeducedTypeKeyword);
					}
				);
			};
			if(Syntax.Is(Host, Role::InitDeclarator) && HasDecltypeAuto(Syntax.FieldChild(Syntax.ParentOf(Host), ChildRole::Type))) {
				// 名前・欄の参照を `decltype(auto)` の変数の初期化子に置く括弧（値と参照を変える）の保持の返戻
				return true;
			}
			if(Syntax.Is(Host, Role::ReturnStatement)) {
				NodeId Function = Host;
				while(Function != NoNode && !Syntax.Is(Function, Role::ReturnValueHost)) Function = Syntax.ParentOf(Function);
				const NodeId Body = Syntax.FieldChild(Function, ChildRole::Body);
				// 戻値の型（後置の戻値の型を含む）が `decltype(auto)` の関数の返戻の値の括弧（値と参照を変える）の保持の返戻
				if(
					AnyChildOf(
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
			// 括弧の後の最初の字句（括弧が親の最後の子なら祖先の後の字句）
			NodeId After = Paren;
			while(After != NoNode && Syntax.NextSibling(After) == NoNode) After = Syntax.ParentOf(After);
			NodeId Follower = NoNode;
			for(NodeId Leaf = After == NoNode ? NoNode : Syntax.NextSibling(After); Leaf != NoNode; Leaf = Syntax.Child(Leaf, 0)) {
				if(!Syntax.ChildCount(Leaf)) Follower = Leaf;
			}
			// 更新の被演算子の括弧の後の字句（間接参照・呼出が続けば型と読める）
			const bool IsUpdateOperand = Syntax.Is(Parent, Role::UpdateExpression) && Syntax.Child(Parent, 0) == Paren;
			const bool HasUpdateFollower =
			IsUpdateOperand && Syntax.FirstTokenRole(Syntax.NextSibling(Parent), Role::CastUpdateFollowerLead);
			// 型と変数を見分けれない名前の括弧（後続の `*` `&` `-` `+` が括弧を型の変換と読ませる）の保持の返戻
			if(
				Syntax.Is(Inner, Role::CastTypePart) || Syntax.Is(Inner, Role::Identifier) && Syntax.NamedChild(Paren, 0) == Inner &&
				Syntax.IsTokenRole(Follower, Role::CastContinuationToken) && (!IsUpdateOperand || HasUpdateFollower)
			) return true;
		}
		if(Syntax.Is(Parent, Role::ColonSensitiveInterpolation)) {
			// 補間の書式の区切りと紛れる最上位の `:` の括弧（対の括弧・文字列の中は除く）の保持
			bool HasColon = false;
			WalkNodes(
				Syntax,
				Inner,
				[&](const NodeId Node) -> WalkStep {
					if(!Syntax.IsNamed(Node)) {
						HasColon = Syntax.IsTokenRole(Node, Role::InterpolationFormatSeparator);
						// 保持が決まった後の終了と，字句の内側へ降りない事の返戻
						return HasColon ? WalkStep::Stop : WalkStep::Skip;
					}
					// 対の括弧・文字列の中へ降りない事の返戻
					return Node == Inner || !OpensBracketPair(Syntax, Node) && !Syntax.FirstTokenRole(Node, Role::InterpolationOpaqueLead) ?
					WalkStep::Descend :
					WalkStep::Skip;
				}
			);
			// 書式の区切りと紛れる括弧の保持の返戻
			if(HasColon) return true;
		}
		// 区切りだけが続く値の位置か（右端迄取る式も外せる）
		const bool IsClosedValueSlot = Syntax.Is(Parent, Role::ParenValueSlot) && (
			(Next == NoNode || Syntax.IsNamed(Next) || Syntax.LexemeOf(Next).empty()) ||
			Syntax.IsTokenRole(Next, Role::ExpressionTerminator) ||
			Syntax.IsTokenRole(Next, Role::ColonToken) && Syntax.Is(Parent, Role::TernaryExpression) ||
			Syntax.IsTokenRole(Next, Role::ClosedValueTerminator)
		);
		// 括弧の直後の構文（親の末尾なら祖先の後続）
		NodeId After = Paren;
		std::vector<NodeId> AfterPath;
		while(After != NoNode && !ScanMemo.After.contains(After)) {
			Syntax.Charge(1);
			AfterPath.push_back(After);
			if(Syntax.NextSibling(After) != NoNode) break;
			After = Syntax.ParentOf(After);
		}
		if(After != NoNode && ScanMemo.After.contains(After)) {
			Syntax.Charge(1);
			After = ScanMemo.After.at(After);
		}
		for(const NodeId Visited : AfterPath) ScanMemo.After.emplace(Visited, After);
		const NodeId AfterNode = After == NoNode ? NoNode : Syntax.NextSibling(After);
		NodeId AfterLead = NoNode;
		if(Syntax.Is(Inner, Role::UnaryPreOrUpdate) && AfterNode != NoNode) {
			if(
				const std::unordered_map<NodeId, NodeId>::const_iterator At = ScanMemo.FirstLeaf.find(AfterNode);
				At != ScanMemo.FirstLeaf.end()
			) {
				Syntax.Charge(1);
				AfterLead = At->second;
			} else {
				AfterLead = FirstLeaf(Syntax, AfterNode);
				ScanMemo.FirstLeaf.emplace(AfterNode, AfterLead);
			}
		}
		// 右端だけを貪欲に伸ばす前置の式の直後が区切りか
		const bool IsClosedPrefixTail = Syntax.Is(Inner, Role::UnaryPreOrUpdate) && (
			AfterNode == NoNode || Syntax.IsTokenRole(AfterLead, Role::SeparatorLead) ||
			Syntax.IsTokenRole(AfterLead, Role::ClosingBracketLead)
		);
		// 文頭の字句・括弧を要する中身の保持の返戻
		if(
			Syntax.Is(Parent, Role::ExpressionStatement) && Syntax.Is(Inner, Role::ExprStmtLiteral) ||
			Syntax.Is(Inner, Role::ParenInnerNoUnwrap) &&
			!(Syntax.Is(Inner, Role::GreedyTailExpression) && (IsClosedValueSlot || IsClosedPrefixTail)) ||
			Syntax.Is(Parent, Role::FoldExpression)
		) return true;
		// 左端の多重代入・セイウチの括弧の保持
		NodeId First = Syntax.Child(Inner, 0);
		std::vector<NodeId> FirstPath;
		bool HasLeftConstraint = false;
		while(Syntax.IsNamed(First) && !ScanMemo.LeftConstraint.contains(First)) {
			Syntax.Charge(1);
			FirstPath.push_back(First);
			if(Syntax.Is(First, Role::ParenInnerFirstChildNoUnwrap)) {
				HasLeftConstraint = true;
				break;
			}
			First = Syntax.Child(First, 0);
		}
		if(ScanMemo.LeftConstraint.contains(First)) {
			Syntax.Charge(1);
			HasLeftConstraint = ScanMemo.LeftConstraint.at(First);
		}
		for(const NodeId Visited : FirstPath) ScanMemo.LeftConstraint.emplace(Visited, HasLeftConstraint);
		// 左端の制約に依る保持の返戻
		if(HasLeftConstraint) return true;
		// 制御構文の見出で本体の塊と紛れる式の括弧（Swift は見出の式を直に持つ構文の中だけ）の保持の返戻
		if(IsInBraceHeader && Syntax.Is(Parent, Role::BodyAmbiguousHeader) && HasBodyLikeExpression(Syntax, Paren)) return true;
		if(Syntax.Is(Paren, Role::BlockEndingParen)) {
			// 左端の塊の式
			NodeId Lead = Inner;
			std::vector<NodeId> LeadPath;
			while(!ScanMemo.BlockLead.contains(Lead)) {
				Syntax.Charge(1);
				LeadPath.push_back(Lead);
				if(Syntax.Is(Lead, Role::StatementEndingBlock) || !Syntax.IsNamed(Syntax.Child(Lead, 0))) break;
				Lead = Syntax.Child(Lead, 0);
			}
			if(ScanMemo.BlockLead.contains(Lead)) {
				Syntax.Charge(1);
				Lead = ScanMemo.BlockLead.at(Lead);
			}
			for(const NodeId Visited : LeadPath) ScanMemo.BlockLead.emplace(Visited, Lead);
			if(Syntax.Is(Lead, Role::StatementEndingBlock)) {
				// 括弧を左端に持つ最も外の式（文の頭で止める）
				NodeId Top = Paren;
				std::vector<NodeId> TopPath;
				while(!ScanMemo.BlockTop.contains(Top)) {
					Syntax.Charge(1);
					TopPath.push_back(Top);
					const NodeId Up = Syntax.ParentOf(Top);
					if(Up == NoNode || Syntax.Child(Up, 0) != Top || Syntax.Is(Up, Role::BlockExpressionStatementHost)) break;
					Top = Up;
				}
				if(ScanMemo.BlockTop.contains(Top)) {
					Syntax.Charge(1);
					Top = ScanMemo.BlockTop.at(Top);
				}
				for(const NodeId Visited : TopPath) ScanMemo.BlockTop.emplace(Visited, Top);
				// 文頭の塊の式の括弧（閉じの `}` で文が終わり，後続が別の文へ離れる）の保持の返戻
				if(
					Syntax.Is(Syntax.ParentOf(Top), Role::BlockExpressionStatementHost) &&
					(LastLeafOf(Top) != LastLeafOf(Paren) || LastLeafOf(Inner) != LastLeafOf(Lead))
				) return true;
			}
			if(IsInBraceHeader) {
				NodeId Edge = Inner;
				std::vector<NodeId> EdgePath;
				bool HasJump = false;
				while(Edge != NoNode && !ScanMemo.RightJump.contains(Edge)) {
					Syntax.Charge(1);
					EdgePath.push_back(Edge);
					if(
						const uint32_t Count = Syntax.NamedChildCount(Edge);
						Syntax.Is(Edge, Role::OptionalValueJump) && (!Count || Count == 1 && Syntax.Is(Syntax.NamedChild(Edge, 0), Role::JumpLabel))
					) {
						HasJump = true;
						break;
					}
					Edge = RightEdgeChild(Syntax, Edge);
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
			Syntax.Is(OpParent, Role::UnaryPreOrUpdate) && !Syntax.IsNamed(Syntax.Child(OpParent, 0)) &&
			FusesPrefixOperator(Syntax.Rules(), Syntax.LastChar(Syntax.Child(OpParent, 0)), Syntax.FirstChar(Inner))
		) return true;
		if(Syntax.Is(Paren, Role::OperatorBoundaryParen)) {
			// 括弧を左端に持つ最も外の式と，其の前の兄弟
			NodeId Top = Paren;
			std::vector<NodeId> TopPath;
			while(!ScanMemo.OperatorTop.contains(Top)) {
				Syntax.Charge(1);
				TopPath.push_back(Top);
				const NodeId Up = Syntax.ParentOf(Top);
				if(Up == NoNode || Syntax.Child(Up, 0) != Top) break;
				Top = Up;
			}
			if(ScanMemo.OperatorTop.contains(Top)) {
				Syntax.Charge(1);
				Top = ScanMemo.OperatorTop.at(Top);
			}
			for(const NodeId Visited : TopPath) ScanMemo.OperatorTop.emplace(Visited, Top);
			const NodeId Before = Syntax.PrevSibling(Top);
			const char BeforeLast = Before == NoNode ? '\0' : Syntax.LastChar(Before);
			// 単項・範囲の記号と中身の記号の融合，try・await・型の変換の結合の変化を防ぐ括弧の保持の返戻
			if(
				Syntax.Is(Inner, Role::AwaitTryCastExpression) ||
				Syntax.Is(Parent, Role::RangeOrUnaryExpression) && Next != NoNode && Syntax.Rules().FusesOperatorChar(Syntax.FirstChar(Next)) &&
				Syntax.Rules().FusesOperatorChar(Syntax.LastChar(Inner)) || Syntax.Is(Syntax.ParentOf(Top), Role::RangeOrUnaryExpression) &&
				(Syntax.Rules().FusesOperatorChar(BeforeLast) || Syntax.LastTokenRole(Before, Role::MemberAccessTail)) &&
				Syntax.Rules().FusesOperatorChar(Syntax.FirstChar(Inner))
			) return true;
		}
		// in の右辺はコンマ式を直接置ける事の返戻
		if(Syntax.Is(Parent, Role::Iteration) && IsInDelimiter) return false;
		int InnerPrec = OpPrecedence(Syntax, Inner, Memo);
		// 内側と親の優先順位からの包装の判定
		const bool IsIterationHeaderParent = Syntax.Is(OpParent, Role::IterationHeader);
		// Ruby の引数・要素・条件で構文に要る and/or の括弧（反復対象は除く）の保持の返戻
		if(Syntax.Rules().RequiredValueParenPrecedences.contains(InnerPrec) && !IsIterationHeaderParent) return true;
		// 区切りだけが続く値の位置の右端迄取る式を外す事の返戻
		if(IsClosedValueSlot && Syntax.Is(Inner, Role::GreedyTailExpression)) return false;
		const int ParentPrec = OpParent == NoNode ? 0 : OpPrecedence(Syntax, OpParent, Memo);
		// 優先順位の分からない中置の演算子の，原子式でない被演算子の括弧の保持の返戻
		if(!ParentPrec) return Syntax.Is(OpParent, Role::InfixOp) && !IsPostfixAtom(Syntax, Inner);
		// 優先順位の分からない内側の中置の演算子の括弧の保持の返戻
		if(!InnerPrec && Syntax.Is(Inner, Role::InfixOp)) return true;
		if(!InnerPrec) InnerPrec = UnknownInnerPrec;
		// 累乗より弱い単項の内側の括弧を外せる言語か（単項を左に置けない JS/TS と誤読する PHP は保持）
		const std::string_view Sign =
		ParentPrec == 15 && InnerPrec == 14 ? Syntax.LexemeOf(Syntax.Child(OpParent, 0)) : std::string_view();
		const bool IsSignOverPower = !Sign.empty() && Syntax.Rules().SignsOverPower.find(Sign) != std::string_view::npos;
		bool ShouldKeep = InnerPrec < ParentPrec && !IsSignOverPower;
		// 親の左の被演算子か
		const bool IsLeftOperand = Syntax.NamedChild(OpParent, 0) == Operand, IsPower = ParentPrec == Syntax.Rules().PowerPrecedence;
		// 代入の左辺の条件式の括弧（代入が偽の枝へ入る事を防ぐ）の保持
		if(
			!ShouldKeep && IsLeftOperand && !IsIterationHeaderParent &&
			(ParentPrec == Syntax.Rules().AssignmentPrecedence && InnerPrec == Syntax.Rules().ConditionalPrecedence)
		) ShouldKeep = true;
		// 変換先の型が後続の字句を取る括弧の保持（TS / Swift は取り込まなければ外す）
		if(IsLeftOperand && Syntax.Is(Inner, Role::TypeCastRight)) {
			const NodeId NextToken = Syntax.NextSibling(Operand);
			const std::string_view Token = Syntax.IsNamed(NextToken) ? std::string_view() : Syntax.LexemeOf(NextToken);
			const bool IsAbsorbed = !Syntax.IsNamed(NextToken) && Syntax.IsTokenRole(NextToken, Role::OpeningAngleLead) ||
			Syntax.Rules().TypeContinuationTokens.contains(Token);
			// 逆側の同格以上の演算子の被演算子を変換へ取り込ませない括弧の保持
			ShouldKeep = Syntax.Is(Inner, Role::ReopenableTypeCast) && Syntax.Is(OpParent, Role::InfixOp) ?
			IsAbsorbed || OppositePrecOf(Syntax, OpParent, IsLeftOperand, Memo) >= InnerPrec :
			ShouldKeep || IsAbsorbed;
		}
		// 内側の右端の弱い式が親の右辺を取り込む括弧（後続の無い前置単項は除く）の保持
		if(!ShouldKeep && IsLeftOperand && Syntax.LastNamedChild(OpParent) != Operand) {
			for(NodeId Edge = RightEdgeChild(Syntax, Inner); !ShouldKeep && Edge != NoNode; Edge = RightEdgeChild(Syntax, Edge)) {
				const int EdgePrec = OpPrecedence(Syntax, Edge, Memo);
				if(!EdgePrec) break;
				ShouldKeep = EdgePrec < ParentPrec;
			}
		}
		// 累乗の左辺の単項の括弧（結合の変化と JS/TS の構文破壊を防ぐ）の保持
		if(!ShouldKeep && IsPower && IsLeftOperand && InnerPrec == 15) ShouldKeep = true;
		// 演算子の字句の優先度に基付く，同格の結合方向の保持
		if(!ShouldKeep && InnerPrec == ParentPrec && !IsIterationHeaderParent) {
			// 非結合の比較・シフト・範囲と，TS の型引数の呼出に紛れる比較
			const bool IsNonAssoc =
			Syntax.Rules().NonAssociativePrecedences.contains(ParentPrec) || Syntax.Is(Inner, Role::RangeExpression) ||
			Syntax.Is(OpParent, Role::GenericLookalikeOperator) && ParentPrec == 10 && IsLeftOperand &&
			Syntax.Text(Syntax.FieldChild(OpParent, ChildRole::Operator)) == Syntax.Rules().Operators.Greater &&
			Syntax.Text(Syntax.FieldChild(Inner, ChildRole::Operator)) == Syntax.Rules().Operators.Less &&
			OpensTypeArguments(Syntax, Syntax.FieldChild(OpParent, ChildRole::Right), Memo);
			const bool IsRightAssoc = IsPower || Syntax.Rules().RightAssociativePrecedences.contains(ParentPrec);
			// Python の三項を置けない条件部（最後の被演算子以外）
			const bool IsConditionalOperand = Syntax.Is(OpParent, Role::ConditionalTailOnly) && Syntax.LastNamedChild(OpParent) != Operand;
			ShouldKeep = IsNonAssoc || IsConditionalOperand || ParentPrec != 15 && (IsRightAssoc ? IsLeftOperand : !IsLeftOperand);
		}
		// 言語固有の同格演算子・誤読・構文制約と逆側の結合を保つ括弧の保持の返戻
		return ShouldKeep || Syntax.Rules().ProtectedPrecedencePairs.contains((ParentPrec << 5) + InnerPrec) ||
		OppositePrecOf(Syntax, OpParent, IsLeftOperand, Memo) > InnerPrec;
	}

	/**
	 * 型を包む括弧の中身の取得関数
	 * Swift の要素は名札の無い型１つ（`(x: Int)` は名札付の要素，`(repeat each T)` は型の並びの展開を要素に持つ組）
	 * @param Syntax 構文モデル
	 * @param Node 括弧の型の節点（TS / Kotlin / Go の parenthesized_type，Swift / Rust の要素１つの tuple_type）
	 * @return 型１つを包む括弧なら其の型，其れ以外は無し
	 */
	NodeId SyntaxNormalizer::TypeParenContent(const SyntaxModel &Syntax, const NodeId Node) {
		// 開閉の括弧と中身の３つの子で成らない型の返戻
		if(
			Syntax.ChildCount(Node) != 3 || !Syntax.IsTokenRole(Syntax.Child(Node, 0), Role::OpeningParenToken) ||
			!Syntax.IsTokenRole(Syntax.Child(Node, 2), Role::ClosingParenToken)
		) return NoNode;
		NodeId Inner = Syntax.Child(Node, 1);
		if(Syntax.Is(Node, Role::WrappedTypeItem)) {
			// 名札付・展開の要素の返戻
			if(!Syntax.Is(Inner, Role::TupleTypeItem) || Syntax.ChildCount(Inner) != 1) return NoNode;
			Inner = Syntax.Child(Inner, 0);
			// 型の並びの展開の返戻
			if(Syntax.Is(Inner, Role::TypePackExpansion)) return NoNode;
		}
		// 名前付の中身の返戻
		return Syntax.IsNamed(Inner) ? Inner : NoNode;
	}

	/**
	 * 型を包む冗長な括弧の除去の収集関数（整形仕様 5.2）
	 * 名前等の原子の型 (`(string)` / `(Int)?`) と，型の演算子の被演算子でない位置（注釈・別名・型引数・戻値の型）の型を包む括弧は冗長の為に外す
	 * 型の演算子の被演算子に置いた演算子を持つ型 (`(A | B)[]` / `(() -> Unit)?` / `&(dyn A + Send)`) は外すと結合が変わる為に保つ
	 * 中身が括弧だけの層 (`((A | B))[]`) は最も外の層を最も内側の中身で判断し，内側の層は判断に依らず外す
	 * @param Syntax 構文モデル
	 * @param Node 括弧の型の節点
	 * @param Plan 書換の計画の格納先
	 * @return 纏めて判断した層の数
	 */
	uint32_t SyntaxNormalizer::CollectTypeParenSteps(const SyntaxModel &Syntax, const NodeId Node, RewritePlan &Plan) {
		// 括弧の層と最も内側の型の取得
		const bool IsAlgebraicType = Syntax.Is(Node, Role::AlgebraicTypeParen);
		const NodeId Parent = Syntax.ParentOf(Node);
		NodeId Inner = TypeParenContent(Syntax, Node);
		// 型の括弧を外さない言語と，型１つを包む括弧でない型と，括弧が構文の Swift の関数の型の仮引数の並び (`(Int) -> Int`) の返戻
		if(
			!Syntax.Is(Node, Role::RemovableTypeParen) || Inner == NoNode ||
			Syntax.Is(Parent, Role::TupleParameterFunctionType) && Syntax.NamedChild(Parent, 0) == Node
		) return 1;
		std::vector<NodeId> Layers { Node };
		while(Syntax.Is(Inner, Role::GroupingTypeParen)) {
			const NodeId Content = TypeParenContent(Syntax, Inner);
			if(Content == NoNode) break;
			Layers.push_back(Inner);
			Inner = Content;
		}
		bool IsOperand = false, ShouldKeep = false;
		if(IsAlgebraicType) {
			// TS の型の演算子の被演算子か
			IsOperand = Syntax.Is(Parent, Role::AlgebraicTypeOperandHost) || Syntax.Is(Parent, Role::ConditionalType) && (
				Syntax.FieldChild(Parent, ChildRole::Left) == Node ?
				Syntax.Is(Inner, Role::GreedyType) :
				Syntax.FieldChild(Parent, ChildRole::Right) == Node && Syntax.Is(Inner, Role::ConditionalType)
			) || Syntax.Is(Parent, Role::TypeAnnotation) && Syntax.Is(Inner, Role::GreedyType) &&
			Syntax.Is(Syntax.ParentOf(Parent), Role::ArrowFunction);
			// 同じ種類の合併・交差の入れ子と合併の中の交差は括弧を外せる
			if(
				Syntax.Is(Parent, Role::UnionType) && Syntax.Is(Inner, Role::UnionType) ||
				Syntax.Is(Parent, Role::IntersectionType) && Syntax.Is(Inner, Role::IntersectionType) ||
				Syntax.Is(Parent, Role::UnionType) && Syntax.Is(Inner, Role::IntersectionType)
			) IsOperand = false;
			// 制約付の infer の右端の探索
			NodeId Edge = Inner;
			bool IsReopened = false;
			while(!Syntax.Is(Edge, Role::InferredType)) {
				const NodeId Next = Syntax.Is(Edge, Role::GroupingTypeParen) ? TypeParenContent(Syntax, Edge) : RightEdgeChild(Syntax, Edge);
				if(Next == NoNode) break;
				IsReopened = IsReopened || Syntax.Is(Edge, Role::CallableType);
				Edge = Next;
			}
			// 条件型の祖先の右端の追跡
			if(Syntax.Is(Edge, Role::InferredType) && Syntax.NamedChildCount(Edge) == 2) {
				for(NodeId Cur = Node, Up = Parent; Up != NoNode; Cur = Up, Up = Syntax.ParentOf(Up)) {
					if(Syntax.Is(Up, Role::ConditionalType)) {
						ShouldKeep = IsReopened && Syntax.FieldChild(Up, ChildRole::Right) == Cur;
						break;
					}
					if(Syntax.Child(Up, Syntax.ChildCount(Up) - 1) != Cur) break;
					IsReopened = IsReopened || Syntax.Is(Up, Role::CallableType);
				}
			}
		} else if(Syntax.Is(Node, Role::WrappedTypeItem)) {
			// 後続の暗黙開封の `!` と型の `?` の区別（`?` は結合を争わず括弧を外せる）
			const NodeId Next = Syntax.NextSibling(Node);
			IsOperand = !Syntax.Is(Inner, Role::OptionalType) && (
				Syntax.Is(Parent, Role::ComposedTypeOperandHost) &&
				!(Syntax.Is(Parent, Role::ProtocolComposition) && Syntax.Is(Inner, Role::ProtocolComposition)) ||
				Syntax.IsTokenRole(Next, Role::ExclamationToken) || Syntax.IsTokenRole(Next, Role::QuestionToken)
			);
		} else if(Syntax.Is(Node, Role::ConversionTypeParen)) {
			// 解析器は参照外しの変換 `*(*T)(x)` を型 `*(*T)` への変換と読む為，ポインタ型を遡った変換の型の括弧と看做す
			NodeId Owner = Parent;
			while(Syntax.Is(Owner, Role::PointerType)) Owner = Syntax.ParentOf(Owner);
			// 型の変換の名前・並び・本体の型だけの括弧を外す
			ShouldKeep = Syntax.Is(Owner, Role::TypeConversion) && !Syntax.Is(Inner, Role::UnambiguousConversionType);
			// 向きの無いチャネルの受信専用の要素の括弧（<- の掛かる先を変えない）の保持
			IsOperand = Syntax.Is(Parent, Role::ChannelType) && Syntax.ChildCount(Parent) == 2 &&
			Syntax.IsTokenRole(FirstLeaf(Syntax, Inner), Role::ChannelReceiveToken);
		}
		IsOperand = IsOperand || Syntax.Is(Parent, Role::NullableTypeOperandHost) || Syntax.Is(Parent, Role::BorrowedTypeOperandHost);
		// 結合を保てる時だけ最も外の層も外し，其の他は内側の層だけを外す
		const bool KeepsOuter = ShouldKeep || IsOperand && Syntax.Is(Inner, Role::TypeOperatorInner);
		for(size_t Layer = KeepsOuter ? 1 : 0; Layer < Layers.size(); ++Layer) {
			Plan.Steps.push_back({ RewriteKind::Unwrap, Layers[Layer] });
			// Swift の組の型の要素の包みも外す（解析器は括弧の無い型を要素で包まない）
			if(Syntax.Is(Layers[Layer], Role::WrappedTypeItem)) {
				Plan.Steps.push_back({ RewriteKind::Unwrap, Syntax.Child(Layers[Layer], 1) });
			}
		}
		// 纏めて判断した層の数の返戻
		return static_cast<uint32_t>(Layers.size());
	}

	/**
	 * 型の合併・交差の左結合への正規化の収集関数
	 * @param Syntax 構文モデル
	 * @param Plan 書換の計画の格納先
	 */
	void SyntaxNormalizer::CollectTypeAssociationSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const {
		for(const NodeId Id : Candidates(Syntax, { Role::TypeCombination }, {})) {
			Role Combination = Role::TypeCombination;
			// 複数の役割に一致する節点では先に挙げた型を採る
			for(
				const Role Candidate :
				{
					Role::UnionType,
					Role::IntersectionType,
					Role::ProtocolComposition,
					Role::CatchTypeAlternatives,
					Role::NormalFormType,
					Role::NonNullableType,
					Role::TypeBounds,
					Role::TypeElements,
					Role::TypeList
				}
			) if(Syntax.Is(Id, Candidate)) {
				Combination = Candidate;
				break;
			}
			// 内側の連鎖は外側が纏めて扱う
			if(Syntax.Is(Syntax.ParentOf(Id), Combination)) continue;
			std::vector<NodeId> Items, Pending { Id };
			bool HasRight = false;
			while(!Pending.empty()) {
				const NodeId Part = Pending.back();
				Pending.pop_back();
				if(Syntax.Is(Part, Combination)) {
					HasRight |= Syntax.Is(Syntax.LastNamedChild(Part), Combination);
					for(
						std::span<const NodeId>::reverse_iterator Position = Syntax.ChildrenOf(Part).rbegin();
						Position != Syntax.ChildrenOf(Part).rend();
						++Position
					) Pending.push_back(*Position);
				} else Items.push_back(Part);
			}
			if(HasRight && Items.size() > 2) {
				GeneratedNode Result { {}, false, {}, 0, Items[0] };
				for(size_t Index = 1; Index + 1 < Items.size(); Index += 2) {
					Result = { {}, true, { std::move(Result), { {}, false, {}, 0, Items[Index] }, { {}, false, {}, 0, Items[Index + 1] } } };
					Result.Definition = Syntax.GetNode(Id).Kind.Definition;
				}
				RewriteStep Step { RewriteKind::Reshape, Id };
				Step.Inserted = std::move(Result);
				Plan.Steps.push_back(std::move(Step));
			}
		}
		// 終了
		return;
	}

	/**
	 * 冗長な括弧の除去の収集関数（整形仕様 5.2）
	 * 式と型を包む括弧を外せるかを括弧の最も外の層で判断し，外せない層の内側の層を外す
	 * 誤り区画の中と，誤りを含む括弧・誤り区画に接する括弧は保つ
	 * 役割索引又は作業列の候補を構文順に調べ，優先順位と必要な祖先を控える
	 * @param Syntax 構文モデル
	 * @param Plan 書換の計画の格納先
	 */
	void SyntaxNormalizer::CollectParenSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const {
		// 祖先と字句の判定結果の控え
		std::unordered_map<NodeId, int> Memo;
		std::unordered_map<NodeId, bool> AngleMemo, BraceHeaders, UnnamedAncestors;
		ParenScanMemo ScanMemo;
		std::unordered_set<NodeId> Skipped;
		// 誤りを含む括弧と，誤り区画に接して原文の間隙を保つ括弧か（整形仕様 4.6）
		const auto IsProtected = [&Syntax](const NodeId Paren) -> bool {
			const NodeId Parent = Syntax.ParentOf(Paren);
			const uint32_t Index = Syntax.GetNode(Paren).Index;
			for(const uint32_t Boundary : { Index, Index + 1 }) {
				const SemanticBoundary *const Found = Syntax.GetNode(Parent).Facts.BoundaryBefore(Boundary);
				// 原文の間隙を保つ境界に接する事の返戻
				if(Found && Found->Kind == BoundaryKind::Preserved) return true;
			}
			// 誤りを含むかの返戻
			return Syntax.GetNode(Paren).Facts.Has(ConstraintKind::ContainsError);
		};
		// 括弧候補の祖先と制御構文の見出の判定
		for(
			const NodeId Id :
			Candidates(
				Syntax,
				{ Role::GroupingParen, Role::GroupingTypeParen, Role::OperatorCall, Role::RemovablePatternParen },
				{ Role::Error }
			)
		) {
			bool IsSkipped = Skipped.contains(Id);
			std::vector<NodeId> Path;
			NodeId Up = Syntax.ParentOf(Id);
			while(Up != NoNode && !UnnamedAncestors.contains(Up)) {
				Syntax.Charge(1);
				Path.push_back(Up);
				Up = Syntax.ParentOf(Up);
			}
			if(Up != NoNode) Syntax.Charge(1);
			bool HasUnnamedAncestor = Up != NoNode && UnnamedAncestors.at(Up);
			for(std::vector<NodeId>::const_reverse_iterator It = Path.rbegin(); It != Path.rend(); ++It) {
				HasUnnamedAncestor |= *It != Syntax.GetRoot() && !Syntax.IsNamed(*It);
				UnnamedAncestors.emplace(*It, HasUnnamedAncestor);
			}
			IsSkipped |= HasUnnamedAncestor;
			if(IsSkipped || Id != Syntax.GetRoot() && !Syntax.IsNamed(Id)) continue;
			Path.clear();
			Up = Syntax.ParentOf(Id);
			while(Up != NoNode && !BraceHeaders.contains(Up)) {
				Syntax.Charge(1);
				Path.push_back(Up);
				if(Syntax.Is(Up, Role::BodyAmbiguousControl) || Syntax.Is(Up, Role::ScopeBody)) break;
				Up = Syntax.ParentOf(Up);
			}
			if(Up != NoNode && BraceHeaders.contains(Up)) Syntax.Charge(1);
			bool IsInBraceHeader = Up != NoNode && BraceHeaders.contains(Up) && BraceHeaders.at(Up);
			for(std::vector<NodeId>::const_reverse_iterator It = Path.rbegin(); It != Path.rend(); ++It) {
				if(Syntax.Is(*It, Role::BodyAmbiguousControl)) IsInBraceHeader = true;
				else if(Syntax.Is(*It, Role::ScopeBody)) IsInBraceHeader = false;
				BraceHeaders.emplace(*It, IsInBraceHeader);
			}
			// Swift の１要素の照合の括弧の除去（見出は解析器が読める束縛・列挙の場合だけ）
			if(Syntax.Is(Id, Role::RemovablePatternParen) && !IsProtected(Id)) {
				const std::span<const NodeId> Kids = Syntax.ChildrenOf(Id);
				for(size_t Index = 0; Index + 2 < Kids.size(); ++Index) {
					const NodeId Open = Kids[Index], Pattern = Kids[Index + 1], Close = Kids[Index + 2], Lead = Syntax.Child(Pattern, 0);
					// 括弧１組で照合１つを包む形でない位置の継続
					if(
						!Syntax.IsTokenRole(Open, Role::OpeningParenToken) || !Syntax.Is(Pattern, Role::Pattern) ||
						!Syntax.IsTokenRole(Close, Role::ClosingParenToken)
					) continue;
					if(Syntax.Is(Id, Role::Pattern)) {
						// 照合だけを包む照合の包みの除去
						if(Kids.size() == 3) Plan.Steps.push_back({ RewriteKind::Unwrap, Id });
					} else if(
						Index && Syntax.IsWord(Kids[Index - 1], Word::Case) && Lead != NoNode &&
						(Syntax.IsNamed(Lead) ? Syntax.Is(Lead, Role::ValueBindingPattern) : Syntax.IsTokenRole(Lead, Role::DotToken))
					) {
						// 見出の `case` の後の照合の括弧と照合の包みの除去（解析器は包みの無い照合の子を見出へ直に並べる，錨は子の前後へ移す）
						const std::vector<NodeId> Parts(Syntax.ChildrenOf(Pattern).begin(), Syntax.ChildrenOf(Pattern).end());
						const AnchorTarget Before { Parts.front(), false }, After { Parts.back(), true };
						Plan.Steps.push_back({ RewriteKind::Remove, Open, {}, { Open, Before, Before, Before } });
						uint32_t At = static_cast<uint32_t>(Index);
						for(const NodeId Part : Parts) {
							// 照合の中の子の前は元の境界を保つ
							const SemanticBoundary *const Recorded = Syntax.GetNode(Pattern).Facts.BoundaryBefore(Syntax.GetNode(Part).Index);
							RewriteStep Move { RewriteKind::Move, Part };
							Move.Destination = Id;
							Move.Index = At++;
							if(Part != Parts.front() && Recorded) {
								Move.Boundary = Recorded->Kind;
								Move.RecordedBoundary = *Recorded;
							} else Move.Boundary = BoundaryKind::Spaced;
							// 見出の条件を表す場は，自身の場を持たない中身へ移す（解析器は隠れた規則の場を自身の場の無い子へ受け継がせる）
							if(!Syntax.GetNode(Part).Fields) Move.DestinationFields = Syntax.GetNode(Open).Fields;
							Plan.Steps.push_back(std::move(Move));
						}
						Plan.Steps.push_back({ RewriteKind::Remove, Pattern, {}, { Pattern, Before, After, Before } });
						Plan.Steps.push_back({ RewriteKind::Remove, Close, {}, { Close, After, After, After } });
					}
				}
			}
			// Swift の前置の演算子を呼出と読んだ括弧の除去（強く結合し，記号・`.` で始まらない値だけを前置の式へ組み直す）
			if(const NodeId Arguments = PrefixCallArguments(Syntax, Id); Arguments != NoNode) {
				const NodeId Operator = Syntax.Child(Id, 0), Argument = Syntax.NamedChild(Arguments, 0);
				const NodeId Value = Syntax.ChildCount(Argument) == 1 ? Syntax.NamedChild(Argument, 0) : NoNode;
				const int ValuePrec = Value == NoNode ? 0 : OpPrecedence(Syntax, Value, Memo);
				if(
					(!Syntax.IsNamed(Operator) || Syntax.Is(Operator, Role::ExclamationMark)) && !IsProtected(Id) && Value != NoNode &&
					Syntax.NamedChildCount(Arguments) == 1 && Syntax.ChildCount(Arguments) == 3 &&
					(ValuePrec ? ValuePrec > 14 : !Syntax.Is(Value, Role::InfixOp)) && !Syntax.Rules().FusesOperatorChar(Syntax.FirstChar(Value)) &&
					!Syntax.FirstTokenRole(Value, Role::MemberAccessLead)
				) {
					// 括弧の錨は値の前後へ，其の他の包みの錨は値へ移す
					const AnchorTarget Before { Value, false }, After { Value, true };
					const NodeId Open = Syntax.Child(Arguments, 0), Close = Syntax.Child(Arguments, 2);
					Plan.Steps.push_back({ RewriteKind::Remove, Open, {}, { Open, Before, Before, Before } });
					Plan.Steps.push_back({ RewriteKind::Remove, Close, {}, { Close, After, After, After } });
					RewriteStep Prefix { RewriteKind::Reshape, Id, {}, { NoNode, Before, After, Before } };
					Prefix.Inserted = { {}, true };
					Prefix.Inserted.Definition = Syntax.Rules().PrefixExpressionDefinition;
					Prefix.Inserted.Children.push_back({ {}, false, {}, Syntax.FieldBit(ChildRole::Operator), Operator });
					Prefix.Inserted.Children.push_back({ {}, false, {}, Syntax.FieldBit(ChildRole::Target), Value });
					Plan.Steps.push_back(std::move(Prefix));
				}
			}
			// 纏めて判断した括弧の層を飛ばした中身
			NodeId Descend = Id;
			if(Syntax.Is(Id, Role::GroupingParen) && Syntax.ParentOf(Id) != NoNode) {
				std::vector<NodeId> Layers { Id };
				// 括弧の中身（C# の `this` 等の無名の語１つも中身とする）
				NodeId Inner = Syntax.NamedChildCount(Id) == 1 ?
				Syntax.NamedChild(Id, 0) :
				Syntax.Is(Id, Role::ValueParentheses) && Syntax.ChildCount(Id) == 3 ? Syntax.Child(Id, 1) : NoNode;
				while(
					Syntax.Is(Inner, Role::GroupingParen) && Syntax.ChildCount(Inner) == 3 && Syntax.NamedChildCount(Inner) == 1 &&
					Syntax.ChildCount(Layers.back()) == 3
				) {
					Layers.push_back(Inner);
					Inner = Syntax.NamedChild(Inner, 0);
				}
				const bool IsBroken = IsProtected(Id);
				const bool KeepsOuter =
				IsBroken || Inner == NoNode || KeepsExpressionParen(Syntax, Id, Inner, IsInBraceHeader, Memo, AngleMemo, ScanMemo);
				for(size_t Layer = KeepsOuter ? 1 : 0; !IsBroken && Layer < Layers.size(); ++Layer) {
					Plan.Steps.push_back({ RewriteKind::Unwrap, Layers[Layer] });
				}
				Descend = Layers.back();
				for(size_t Layer = 1; Layer < Layers.size(); ++Layer) Skipped.insert(Layers[Layer]);
			} else if(Syntax.Is(Id, Role::GroupingTypeParen) && Syntax.ParentOf(Id) != NoNode && !IsProtected(Id)) {
				// 纏めて判断した型の括弧の層を飛ばす
				for(uint32_t Layers = CollectTypeParenSteps(Syntax, Id, Plan); --Layers;) {
					Descend = TypeParenContent(Syntax, Descend);
					Skipped.insert(Descend);
				}
			}
		}
		// 終了
		return;
	}

	/**
	 * 前置の演算子と融合する被演算子の包装の収集関数（整形仕様 5.4）
	 * 被演算子の先頭の記号が前置の演算子と１字句へ融合する場合（`+ +x` の `++`）は，被演算子を括弧で包む
	 * @param Syntax 構文モデル
	 * @param Plan 書換の計画（包装を加える）
	 */
	void SyntaxNormalizer::CollectFusedPrefixSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const {
		for(const NodeId Id : Candidates(Syntax, { Role::UnaryPreOrUpdate }, { Role::Error })) if(
			const NodeId Operator = Syntax.Child(Id, 0), Operand = Syntax.Child(Id, 1);
			Operator != NoNode && !Syntax.IsNamed(Operator) && Syntax.IsNamed(Operand) &&
			FusesPrefixOperator(Syntax.Rules(), Syntax.LastChar(Operator), Syntax.FirstChar(Operand))
		) {
			RewriteStep Wrap { RewriteKind::Wrap, Operand };
			Wrap.Inserted = {
				{},
				true,
				{
					{ GeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallOpen) },
					{ {}, false, {}, {}, Operand },
					{ GeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallClose) }
				}
			};
			Wrap.Inserted.Definition = Syntax.Rules().ExpressionParenDefinition;
			Wrap.Boundary = BoundaryKind::Glued;
			// 包んだ被演算子の外の錨は括弧の外へ移す
			Wrap.Transfer.From = Operand;
			Plan.Steps.push_back(std::move(Wrap));
		}
		// 終了
		return;
	}
} // namespace Core
