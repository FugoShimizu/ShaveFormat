#include "SyntaxNormalizer.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include "ControlFlow.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 纏めた宣言の除去の収集関数
	 * 纏めた宣言に同じ形の節点が残る物（共有する修飾・型）の錨は其の節点へ，其の他の移さずに残る節点（宣言種別・仕様・区切り）の錨は其の宣言の最初の名前の前後へ，宣言の末尾の錨は指定の行先へ移す
	 * 計算量：訪れる節点数 V と移した節点数 K に対し期待値 O(V + K)
	 * @param Syntax 構文モデル
	 * @param Node 除く宣言
	 * @param End 宣言の末尾の錨の行先
	 * @param Kept 移した名前と初期化子（其の部分木は除かない）
	 * @param Twins 纏めた宣言に残る同じ形の節点（除く節点から引く）
	 * @param Removal 同じ親の宣言を纏めて除く操作
	 * @param Plan 書換の計画の格納先
	 */
	void SyntaxNormalizer::RemoveMergedDecl(
		const SyntaxModel &Syntax,
		const NodeId Node,
		const AnchorTarget End,
		const std::vector<NodeId> &Kept,
		const std::unordered_map<NodeId, NodeId> &Twins,
		RewriteStep &Removal,
		RewritePlan &Plan
	) {
		// 残す部分木と錨の移譲先の準備
		const AnchorTarget Before { Kept.front(), false }, After { Kept.front(), true };
		Syntax.Charge(Kept.size());
		const std::unordered_set<NodeId> KeptNodes(Kept.begin(), Kept.end());
		WalkNodes(
			Syntax,
			Node,
			[&](const NodeId Sub) -> WalkStep {
				// 移した部分木を読飛ばす事の返戻
				if(KeptNodes.contains(Sub)) return WalkStep::Skip;
				if(Sub == Node) {
					Removal.RemovedSiblings.push_back({ Sub, Before, End, Before });
					// 子の錨を個別に移す為の走査の返戻
					return WalkStep::Descend;
				}
				if(const std::unordered_map<NodeId, NodeId>::const_iterator Twin = Twins.find(Sub); Twin != Twins.end()) {
					const AnchorTarget TwinBefore { Twin->second, false }, TwinAfter { Twin->second, true };
					Plan.Steps.push_back({ RewriteKind::Remove, Sub, {}, { Sub, TwinBefore, TwinAfter, TwinBefore } });
					// 子の除去へ進む事の返戻
					return WalkStep::Descend;
				}
				Plan.Steps.push_back({ RewriteKind::Remove, Sub, {}, { Sub, Before, After, Before } });
				// 子の除去へ進む事の返戻
				return WalkStep::Descend;
			}
		);
		// 終了
		return;
	}

	/**
	 * 不変指定の正規化の収集関数（整形仕様 5.3）
	 * 変更される名前を集め，変更の無い局所の束縛を言語の不変の形へ揃える（Java は `final` を付け，Rust は `mut` を除き，Kotlin は `var` を `val` にする）
	 * 名前は同じ綴り毎に数え（束縛を追わず保守的に除く），変更の読み方は言語の意味に従う
	 * Java・Kotlin は名前への代入と増減を，Rust は欄・添字を介した代入・可変借用・呼出の受け手・`ref mut` の照合も束縛の変更とする
	 * Java の定数式で初期化する名前は定数変数化で意味が変わり得る為に参照が１回の物だけを，Rust は未展開のマクロ・NFC 正規化を要する識別子・展開前の属性の範囲とクロージャの束縛を，Kotlin は型・物体式の成員と `lateinit` を除く
	 * 変更・束縛・有効範囲の役割から対象を集め，候補が読む範囲だけを調べる
	 * @param Syntax 構文モデル
	 * @param Plan 書換の計画の格納先
	 */
	void SyntaxNormalizer::CollectImmutableBindingSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const {
		// 束縛の変更・参照の事実は文書全体から集め，正規化で変わらない為，再評価では候補の宣言が変わった時だけ決め直す
		if(
			!WorkNodes.empty() && std::ranges::none_of(
				WorkNodes,
				[&Syntax](const NodeId Node) -> bool {
					// 候補の宣言かの返戻
					return Syntax.Is(Node, Role::Iteration) || Syntax.Is(Node, Role::CommaDeclaration) ||
					Syntax.Is(Node, Role::MutablePatternBinding) || Syntax.Is(Node, Role::FinalizableLocal) ||
					Syntax.Is(Node, Role::ReplaceableBindingKeyword);
				}
			)
		) return;
		// 不変の束縛を宣言する語
		const std::string Immutable(Syntax.Rules().Spelling(Word::ImmutableBinding));
		if(Syntax.Rules().FreezesLexicalBindings) {
			// 未解析の参照を持つ為に束縛の変更を確定しない場合の終了
			if(Syntax.GetNode(Syntax.GetRoot()).Facts.Has(ConstraintKind::ContainsError)) return;
			std::unordered_set<std::string> Mutated;
			bool IsDynamic = false, IsModule = false;
			const auto Mark = [&](const NodeId Target) -> void {
				WalkNodes(
					Syntax,
					Target,
					[&](const NodeId Part) -> WalkStep {
						// 欄・添字の更新は受け手の再束縛でない事の返戻
						if(Syntax.Is(Part, Role::MemberMutationTarget)) return WalkStep::Skip;
						if(Syntax.Is(Part, Role::BindingName)) Mutated.insert(Syntax.ComparisonName(Part));
						// 子を辿る事の返戻
						return WalkStep::Descend;
					}
				);
				// 終了
				return;
			};
			// 文書全体の動的な参照と変更される束縛の収集
			for(
				const NodeId Node :
				Candidates(
					Syntax,
					{
						Role::AssignmentOperation,
						Role::BindingName,
						Role::CallExpression,
						Role::Export,
						Role::Identifier,
						Role::ImportStatement,
						Role::Iteration,
						Role::UpdateExpression
					},
					{},
					true
				)
			) {
				if(Syntax.ParentOf(Node) == Syntax.GetRoot() && (Syntax.Is(Node, Role::Export) || Syntax.Is(Node, Role::ImportStatement))) {
					IsModule = true;
				}
				if(
					(Syntax.Is(Node, Role::Identifier) || Syntax.Is(Node, Role::BindingName)) && Syntax.Text(Node).find('\\') != std::string::npos
				) IsDynamic = true;
				if(Syntax.Is(Node, Role::CallExpression)) {
					NodeId Function = Syntax.FieldChild(Node, ChildRole::Function);
					while(Syntax.Is(Function, Role::GroupingParen)) Function = Syntax.NamedChild(Function, 0);
					if(Syntax.IsWord(Function, Word::DynamicScope)) IsDynamic = true;
				}
				if(Syntax.Is(Node, Role::AssignmentOperation) || Syntax.Is(Node, Role::UpdateExpression)) Mark(Syntax.NamedChild(Node, 0));
				if(
					Syntax.Is(Node, Role::Iteration) && !Syntax.HasWordChild(Node, Word::ReassignableBinding) &&
					!Syntax.HasWordChild(Node, Word::ImmutableBinding) && !Syntax.HasWordChild(Node, Word::FunctionScopedBinding)
				) Mark(Syntax.FieldChild(Node, ChildRole::Left));
			}
			// 直接の eval と逃避した識別子を持つ場合の終了
			if(IsDynamic) return;
			// 再代入されない束縛の不変の語への置換
			for(const NodeId Node : Candidates(Syntax, { Role::CommaDeclaration, Role::Iteration }, {}, true)) {
				const NodeId Parent = Syntax.ParentOf(Node);
				const bool IsIteration = Syntax.Is(Node, Role::Iteration);
				// 可変の束縛を持たない節点の継続
				if(!Syntax.HasWordChild(Node, Word::ReassignableBinding)) continue;
				if(!IsModule && Parent == Syntax.GetRoot() || Syntax.Is(Parent, Role::Export) && Syntax.ParentOf(Parent) != Syntax.GetRoot()) {
					// 有効範囲外の束縛の継続
					continue;
				}
				std::vector<NodeId> Patterns;
				bool CanFreeze = true;
				if(IsIteration) Patterns.push_back(Syntax.FieldChild(Node, ChildRole::Left));
				else for(const NodeId Declarator : Syntax.ChildrenOf(Node)) if(Syntax.IsNamed(Declarator)) {
					CanFreeze &= Syntax.FieldChild(Declarator, ChildRole::Value) != NoNode;
					Patterns.push_back(Syntax.FieldChild(Declarator, ChildRole::Name));
				}
				for(const NodeId Pattern : Patterns) {
					bool HasName = false;
					WalkNodes(
						Syntax,
						Pattern,
						[&](const NodeId Part) -> WalkStep {
							if(Syntax.Is(Part, Role::BindingName)) {
								HasName = true;
								CanFreeze &= !Mutated.contains(Syntax.ComparisonName(Part));
							}
							// 子を辿る事の返戻
							return WalkStep::Descend;
						}
					);
					CanFreeze &= HasName;
				}
				if(CanFreeze && !Patterns.empty()) for(const NodeId Keyword : Syntax.ChildrenOf(Node)) {
					if(Syntax.IsNamed(Keyword) || !Syntax.IsWord(Keyword, Word::ReassignableBinding)) continue;
					RewriteStep Step { RewriteKind::Reshape, Keyword };
					Step.Inserted = { Immutable };
					Plan.Steps.push_back(std::move(Step));
					Plan.Steps.push_back({ RewriteKind::SetLexeme, Keyword, Immutable });
				}
			}
			// 終了
			return;
		}
		const bool ShouldRemoveMutableMarker = Syntax.Rules().ShouldRemoveMutableMarker;
		// 不変指定を持たない言語と，未解析の参照や変更が在り得る誤りを含む木の終了
		if(
			Immutable.empty() && !ShouldRemoveMutableMarker || Syntax.GetNode(Syntax.GetRoot()).Facts.Has(ConstraintKind::ContainsError)
		) return;
		// 束縛の変更と不変指定の候補の収集準備
		std::unordered_set<std::string> MutatedNames;
		// 欄・添字の連鎖の最も左の基底の識別子 (Rust)
		const auto BaseIdentifier = [&Syntax](NodeId Node) -> NodeId {
			while(Node != NoNode && !Syntax.Is(Node, Role::Identifier)) {
				// 連鎖でない節点の基底の無い事の返戻
				if(!Syntax.Is(Node, Role::MutationBaseTraversal)) return NoNode;
				Node = Syntax.NamedChild(Node, 0);
			}
			// 基底の識別子の返戻
			return Node;
		};
		// 変更の対象の名前の記録（Rust は分割代入を含む左辺の基底の名前，其の他は包みを除いた名前）
		const auto MarkTarget = [&](NodeId Target) -> void {
			if(Syntax.Rules().CanMutateBase) {
				WalkNodes(
					Syntax,
					Target,
					[&](const NodeId Sub) -> WalkStep {
						// 名前付でない字句の内側へ降りない事の返戻
						if(Sub != Target && !Syntax.IsNamed(Sub)) return WalkStep::Skip;
						const NodeId Base = BaseIdentifier(Sub);
						// 基底の名前を得れない部分木の内側へ降りる事の返戻
						if(Base == NoNode) return WalkStep::Descend;
						MutatedNames.insert(Syntax.ComparisonName(Base));
						// 基底の名前を得た部分木の内側へ降りない事の返戻
						return WalkStep::Skip;
					}
				);
				// 終了
				return;
			}
			while(Syntax.NamedChildCount(Target) == 1 && Syntax.Is(Target, Role::AssignableWrapper)) Target = Syntax.NamedChild(Target, 0);
			if(Syntax.Is(Target, Role::Identifier) || Syntax.Is(Target, Role::SimpleName)) {
				MutatedNames.insert(Syntax.ComparisonName(Target));
			}
		};
		// `ref mut` の部分の模式を持ち照合の対象を可変借用する模式か (Rust)
		const auto MutablyBorrows = [&Syntax](const NodeId Pattern) -> bool {
			// 何れかの部分の模式が `ref mut` かの返戻
			return AnyNodeOf(
				Syntax,
				Pattern,
				[&Syntax](const NodeId Sub) -> bool {
					// `ref` を持つ参照・欄の模式の可変の指定の有無の返戻
					return Syntax.Is(Sub, Role::ReferenceBindingPattern) && Syntax.HasWordChild(Sub, Word::ReferenceBinding) && (
						Syntax.Is(Sub, Role::ReferencePattern) ?
						Syntax.Is(Syntax.NamedChild(Sub, 0), Role::MutablePattern) :
						Syntax.FirstNamedChildWithRole(Sub, Role::MutableSpecifier) != NoNode
					);
				}
			);
		};
		// Java の仮引数の有効範囲（型の本体は外側の仮引数を参照しない境界）
		struct ParameterScope {
			NodeId Owner = NoNode; // 有効範囲の節点
			std::unordered_set<std::string> Names {}; // 仮引数名
			bool Inherits = false; // 外側の仮引数も参照出来るか（ラムダ）
		};
		std::vector<ParameterScope> Scopes;
		std::unordered_map<std::string, uint32_t> NameCounts;
		// 仮引数へ依存するか定数式に為れない初期値か (Java)
		const auto IsNonconstant = [&](const NodeId Value) -> bool {
			for(std::vector<NodeId> Pending { Value }; !Pending.empty();) {
				const NodeId Current = Pending.back();
				Pending.pop_back();
				if(Current == NoNode) continue;
				if(Syntax.Is(Current, Role::Identifier)) {
					const std::string Name = Syntax.ComparisonName(Current);
					for(std::vector<ParameterScope>::const_reverse_iterator Scope = Scopes.crbegin(); Scope != Scopes.crend(); ++Scope) {
						// 仮引数への依存の返戻
						if(Scope->Names.contains(Name)) return true;
						if(!Scope->Inherits) break;
					}
				}
				// 定数式に為れない式の返戻
				if(Syntax.Is(Current, Role::NonConstantExpression)) return true;
				if(Syntax.Is(Current, Role::BinaryOperation)) {
					Pending.push_back(Syntax.FieldChild(Current, ChildRole::Left));
					Pending.push_back(Syntax.FieldChild(Current, ChildRole::Right));
				} else if(Syntax.Is(Current, Role::TernaryExpression)) {
					Pending.push_back(Syntax.FieldChild(Current, ChildRole::Condition));
					Pending.push_back(Syntax.FieldChild(Current, ChildRole::Consequence));
					Pending.push_back(Syntax.FieldChild(Current, ChildRole::Alternative));
				} else if(Syntax.Is(Current, Role::UnaryOperation)) Pending.push_back(Syntax.FieldChild(Current, ChildRole::Operand));
				else if(Syntax.Is(Current, Role::CastExpression)) Pending.push_back(Syntax.FieldChild(Current, ChildRole::Value));
				else if(Syntax.Is(Current, Role::ValueParentheses)) {
					for(const NodeId Child : Syntax.ChildrenOf(Current)) if(Syntax.IsNamed(Child)) Pending.push_back(Child);
				}
			}
			// 仮引数へ依存しない初期値の返戻
			return false;
		};
		// 不変指定の候補（宣言・可変の印又は修飾子の並び・束縛の名前・定数式で初期化する名前・有効範囲の塊・展開前の属性の範囲か）
		struct Candidate {
			NodeId Declaration = NoNode;
			NodeId Marker = NoNode;
			std::vector<std::string> Names {};
			std::vector<std::string> ConstantNames {};
			NodeId Scope = NoNode;
			bool IsAttributed = false;
		};
		std::vector<Candidate> Bindings;
		// 未展開のマクロ・非 ASCII の識別子（構文順，Rust）
		std::vector<NodeId> Macros, UnicodeIdentifiers;
		std::unordered_map<NodeId, bool> AttributeProtection;
		// 参照・変更・宣言を構文順に調べる走査
		for(
			const NodeId Node :
			Candidates(
				Syntax,
				{
					Role::AssignmentExpression,
					Role::BorrowExpression,
					Role::BorrowingMatch,
					Role::FinalizableLocal,
					Role::Identifier,
					Role::MutablePatternBinding,
					Role::NormalizedBindingName,
					Role::ParameterScope,
					Role::ParameterScopeBoundary,
					Role::PostfixOrUpdateExpression,
					Role::RangeOrUnaryExpression,
					Role::ReceiverMutationCall,
					Role::ReplaceableBindingKeyword,
					Role::TerminatingMacroCall,
					Role::WritableAssignment
				},
				{},
				true
			)
		) {
			while(!Scopes.empty() && !Syntax.Contains(Scopes.back().Owner, Node)) Scopes.pop_back();
			if(Syntax.Rules().ShouldPreserveConstantStatus) {
				// 仮引数の有効範囲と名前の参照回数の記録
				if(Syntax.Is(Node, Role::ParameterScopeBoundary)) Scopes.push_back({ Node, {}, false });
				else if(Syntax.Is(Node, Role::ParameterScope)) {
					ParameterScope Scope { Node, {}, Syntax.Is(Node, Role::LambdaExpression) };
					NodeId Parameters = Syntax.FieldChild(Node, ChildRole::Parameters);
					// 簡略コンストラクタの仮引数のレコードの宣言からの補完
					if(
						const NodeId Record = Syntax.ParentOf(Syntax.ParentOf(Node));
						Syntax.Is(Node, Role::CompactConstructor) && Syntax.Is(Record, Role::RecordDeclaration)
					) Parameters = Syntax.FieldChild(Record, ChildRole::Parameters);
					if(Syntax.Is(Parameters, Role::Identifier)) Scope.Names.insert(Syntax.ComparisonName(Parameters));
					else for(const NodeId Parameter : Syntax.ChildrenOf(Parameters)) {
						if(!Syntax.IsNamed(Parameter)) continue;
						NodeId Name = Syntax.FieldChild(Parameter, ChildRole::Name);
						if(Syntax.Is(Parameter, Role::SpreadParameter)) {
							Name = Syntax.FieldChild(Syntax.FirstNamedChildWithRole(Parameter, Role::VariableDeclaratorItem), ChildRole::Name);
						} else if(Syntax.Is(Parameter, Role::Identifier)) Name = Parameter;
						if(Name != NoNode) Scope.Names.insert(Syntax.ComparisonName(Name));
					}
					Scopes.push_back(std::move(Scope));
				}
				// 同じ名前の参照の回数
				if(Syntax.Is(Node, Role::Identifier)) ++NameCounts[Syntax.ComparisonName(Node)];
			}
			// 名前への代入と増減の対象の記録
			if(Syntax.Is(Node, Role::WritableAssignment) || Syntax.Is(Node, Role::AssignmentExpression)) {
				const NodeId Left = Syntax.FieldChild(Node, ChildRole::Left);
				MarkTarget(Left != NoNode ? Left : Syntax.NamedChild(Node, 0));
			} else if(Syntax.Is(Node, Role::PostfixOrUpdateExpression) || Syntax.Is(Node, Role::RangeOrUnaryExpression)) {
				if(
					const SyntaxRules::OperatorNotation &Operators = Syntax.Rules().Operators;
					Syntax.HasTokenChild(Node, Operators.Increment) || Syntax.HasTokenChild(Node, Operators.Decrement)
				) MarkTarget(Syntax.NamedChild(Node, 0));
			} else if(Syntax.Is(Node, Role::NormalizedBindingName)) {
				const std::string Name = Syntax.Text(Node);
				if(
					std::any_of(
						Name.begin(),
						Name.end(),
						[](const char Char) -> bool {
							return static_cast<unsigned char>(Char) > 0X7F;
						}
					)
				) UnicodeIdentifiers.push_back(Node);
			} else if(Syntax.Is(Node, Role::TerminatingMacroCall)) Macros.push_back(Node);
			else if(Syntax.Is(Node, Role::BorrowExpression) && Syntax.FirstNamedChildWithRole(Node, Role::MutableSpecifier) != NoNode) {
				// `&mut x` の可変借用された基底の名前
				if(const NodeId Base = BaseIdentifier(Syntax.FieldChild(Node, ChildRole::Value)); Base != NoNode) {
					MutatedNames.insert(Syntax.ComparisonName(Base));
				}
			} else if(Syntax.Is(Node, Role::ReceiverMutationCall)) {
				// メソッドと FnMut の呼出は受け手を変更し得る為，関数部の基底の名前を保つ
				if(const NodeId Base = BaseIdentifier(Syntax.NamedChild(Node, 0)); Base != NoNode) {
					MutatedNames.insert(Syntax.ComparisonName(Base));
				}
			} else if(Syntax.Is(Node, Role::BorrowingMatch)) {
				// １つでも可変借用する腕を持つ照合の対象の保持
				if(
					AnyChildOf(
						Syntax,
						Syntax.FieldChild(Node, ChildRole::Body),
						[&](const NodeId Arm) -> bool {
							// 可変借用する腕かの返戻
							return MutablyBorrows(Syntax.FieldChild(Arm, ChildRole::Pattern));
						}
					)
				) MarkTarget(Syntax.FieldChild(Node, ChildRole::Value));
			} else if(Syntax.Is(Node, Role::MutablePatternBinding)) {
				const NodeId Pattern = Syntax.FieldChild(Node, ChildRole::Pattern), Value = Syntax.FieldChild(Node, ChildRole::Value);
				// `ref mut` の束縛では初期値の側を可変として保つ
				if(MutablyBorrows(Pattern)) MarkTarget(Value);
				const NodeId Specifier = Syntax.FirstNamedChildWithRole(Node, Role::MutableSpecifier);
				// 通常の束縛の名前と `@` の模式の捕捉の名前
				NodeId Name = Syntax.Is(Pattern, Role::Identifier) ? Pattern : NoNode;
				if(Syntax.Is(Pattern, Role::CapturedPattern) && Syntax.Is(Syntax.NamedChild(Pattern, 0), Role::Identifier)) {
					Name = Syntax.NamedChild(Pattern, 0);
				}
				// 条件の中の束縛と，型情報無では可変の呼出の要否を確定出来ないクロージャの束縛は保つ
				if(
					!Syntax.Is(Node, Role::BindingCondition) && !Syntax.Is(Value, Role::ClosureExpression) && Specifier != NoNode && Name != NoNode
				) {
					NodeId Scope = Syntax.ParentOf(Node);
					while(Scope != NoNode && !Syntax.Is(Scope, Role::LexicalBlock)) Scope = Syntax.ParentOf(Scope);
					Bindings.push_back(
						{
							Node,
							Specifier,
							{ Syntax.ComparisonName(Name) },
							{},
							Scope,
							ControlFlow::HasUnexpandedAttribute(Syntax, Node, AttributeProtection)
						}
					);
				}
			} else if(Syntax.Is(Node, Role::FinalizableLocal)) {
				Candidate Found { Node };
				bool HasFinal = false, IsStructureOk = true;
				for(const NodeId Child : Syntax.ChildrenOf(Node)) {
					if(Syntax.Is(Child, Role::ModifierSequence) && Found.Marker == NoNode) {
						Found.Marker = Child;
						HasFinal = Syntax.HasWordChild(Child, Word::ImmutableBinding);
					} else if(Syntax.Is(Child, Role::VariableDeclaratorItem)) {
						const NodeId Value = Syntax.FieldChild(Child, ChildRole::Value), Name = Syntax.FieldChild(Child, ChildRole::Name);
						// 初期値の無い宣言と単純な名前でない宣言は候補にしない
						IsStructureOk = Value != NoNode && Syntax.Is(Name, Role::Identifier);
						if(!IsStructureOk) break;
						Found.Names.push_back(Syntax.ComparisonName(Name));
						if(!IsNonconstant(Value)) Found.ConstantNames.push_back(Found.Names.back());
					}
				}
				if(!HasFinal && IsStructureOk && !Found.Names.empty()) Bindings.push_back(std::move(Found));
			} else if(Syntax.Is(Node, Role::ReplaceableBindingKeyword) && Syntax.Is(Syntax.ParentOf(Node), Role::StatementSequence)) {
				// 関数の中（型・物体式の本体でない文の並び）の `lateinit` でない `var` の単純な名前の束縛
				const NodeId Kind = Syntax.FirstNamedChildWithRole(Node, Role::BindingMarker);
				const NodeId Name = Syntax.NamedChild(Syntax.FirstNamedChildWithRole(Node, Role::VariableDeclaration), 0);
				const NodeId Modifiers = Syntax.FirstNamedChildWithRole(Node, Role::ModifierSequence);
				const bool IsLateInit = AnyChildOf(
					Syntax,
					Modifiers,
					[&Syntax](const NodeId Modifier) -> bool {
						// 初期化を後へ延ばす修飾かの返戻
						return Syntax.IsWord(Modifier, Word::DeferredInitialization);
					}
				);
				if(
					const NodeId Keyword = Syntax.Child(Kind, 0);
					!IsLateInit && Syntax.IsWord(Keyword, Word::ReassignableBinding) && Syntax.Is(Name, Role::SimpleName)
				) Bindings.push_back({ Node, Keyword, { Syntax.ComparisonName(Name) } });
			}
		}
		// 宣言より後の同じ有効範囲に未展開のマクロが在るか
		const auto HasLaterMacro = [&](const NodeId Declaration, const NodeId Scope) -> bool {
			// 宣言の外で後に現れるマクロの有無の返戻
			return std::ranges::any_of(
				Macros,
				[&](const NodeId Macro) -> bool {
					return Syntax.Contains(Scope, Macro) && !Syntax.Contains(Declaration, Macro) && Syntax.Precedes(Declaration, Macro);
				}
			);
		};
		// 有効範囲に非 ASCII の識別子が在るか
		const auto HasUnicodeName = [&](const NodeId Scope) -> bool {
			// 識別子の有無の返戻
			return std::ranges::any_of(
				UnicodeIdentifiers,
				[&](const NodeId Name) -> bool {
					return Syntax.Contains(Scope, Name);
				}
			);
		};
		// 変更と参照の結果に基付く候補の絞込と書換
		for(const Candidate &Found : Bindings) {
			if(
				std::any_of(
					Found.Names.begin(),
					Found.Names.end(),
					[&MutatedNames](const std::string &Name) -> bool {
						// 変更される名前かの返戻
						return MutatedNames.contains(Name);
					}
				) || std::any_of(
					Found.ConstantNames.begin(),
					Found.ConstantNames.end(),
					[&NameCounts](const std::string &Name) -> bool {
						// 参照が１回でない定数変数化の候補かの返戻
						return NameCounts[Name] != 1;
					}
				) || Syntax.Is(Found.Declaration, Role::MutablePatternBinding) &&
				(Found.Scope == NoNode || Found.IsAttributed || HasLaterMacro(Found.Declaration, Found.Scope) || HasUnicodeName(Found.Scope))
			) continue;
			if(Syntax.Is(Found.Declaration, Role::FinalizableLocal)) {
				// 既存の修飾子の後か，宣言の先頭の修飾子の並びへの `final` の追加
				RewriteStep Step { RewriteKind::Insert, Found.Marker == NoNode ? Found.Declaration : Found.Marker };
				Step.Index = Found.Marker == NoNode ? 0 : Syntax.ChildCount(Found.Marker);
				Step.Boundary = BoundaryKind::Spaced;
				Step.Inserted = Found.Marker == NoNode ?
				GeneratedNode{ {}, true, { { Immutable } }, 0, NoNode, {}, Syntax.Rules().ModifierListDefinition } :
				GeneratedNode{ Immutable };
				Plan.Steps.push_back(std::move(Step));
			} else if(Syntax.Is(Found.Declaration, Role::MutablePatternBinding)) {
				// `mut` の指定と其の字句の除去（錨は束縛の模式の前へ移す）
				const AnchorTarget Next { Syntax.NextSibling(Found.Marker), false };
				Plan.Steps.push_back({ RewriteKind::Remove, Found.Marker, {}, { Found.Marker, Next, Next, Next } });
				for(const NodeId Token : Syntax.ChildrenOf(Found.Marker)) {
					Plan.Steps.push_back({ RewriteKind::Remove, Token, {}, { Token, Next, Next, Next } });
				}
			} else {
				// `var` の字句の `val` への組直
				RewriteStep Keyword { RewriteKind::Reshape, Found.Marker };
				Keyword.Inserted = { Immutable };
				Plan.Steps.push_back(std::move(Keyword));
				Plan.Steps.push_back({ RewriteKind::SetLexeme, Found.Marker, Immutable });
			}
		}
		// 終了
		return;
	}

	/**
	 * 宣言を分ける有効範囲かの判定関数（整形仕様 5.6）
	 * 個別の文書・アクセス修飾・意味群を持つ最上位と型の本体の宣言は纏めず，宣言子毎の宣言へ分ける
	 * 前処理指令の条件の塊は其の塊を置いた範囲に従う
	 * @param Syntax 構文モデル
	 * @param Container 宣言を並べる親
	 * @return 最上位か型の本体なら true
	 */
	bool SyntaxNormalizer::IsDeclSplitScope(const SyntaxModel &Syntax, NodeId Container) {
		while(Syntax.Is(Container, Role::PreprocessorBranch) && Syntax.ParentOf(Container) != NoNode) {
			Container = Syntax.ParentOf(Container);
		}
		// 根か型の本体かの返戻
		return Syntax.ParentOf(Container) == NoNode || Syntax.Is(Container, Role::ClassBodyContainer);
	}

	/**
	 * カンマで並べる宣言の統合・分割の収集関数（整形仕様 5.6）
	 * 関数の中の宣言の種別と共有する子の同じ連続した宣言を最初の宣言へ要素を移して纏め，最上位と型の本体の宣言は宣言子毎の宣言へ分ける
	 * 名前と初期化子を別に並べる宣言（全ての初期化子の後で名前を束縛する）は，後の初期化子が前の名前を参照又は再宣言すれば纏めない
	 * 要素の子は識別子を保って移す為，其のコメントは移した節点に付いた儘に為る
	 * 分けた宣言は共有する子の写しを持ち，元の宣言の末尾の錨は最後の要素の後へ移す
	 * 宣言の役割から並びの容器を選び，各容器の子だけを文書順に調べる
	 * @param Syntax 構文モデル
	 * @param Plan 書換の計画の格納先
	 * @param ShouldSplitBindings let の束縛を宣言子毎に分けるか
	 */
	void SyntaxNormalizer::CollectCommaDeclSteps(
		const SyntaxModel &Syntax,
		RewritePlan &Plan,
		const bool ShouldSplitBindings
	) const {
		// 節点の末尾の錨を要素の子の後へ移す計画の追加
		const auto MoveTrailing = [&Plan](const NodeId Holder, const NodeId Piece) -> void {
			const AnchorTarget Own { Holder, false }, Last { Piece, true };
			Plan.Steps.push_back({ RewriteKind::Retarget, Holder, {}, { Holder, Own, Last, Own } });
		};
		// 宣言子毎に型を推論し得る宣言か（`auto` と型引数を省いた型名は宣言子毎の初期化子から型を推論し得る為，初期化子を持つ宣言は型を共有しない）
		const auto DeducesDeclaratorTypes = [&Syntax](const CommaDecl &Decl) -> bool {
			NodeId Type = Syntax.FieldChild(Decl.Node, ChildRole::Type);
			// 修飾した名前は其の名前の部分（型引数を持つ名前は推論しない）
			while(true) {
				const NodeId Name = Syntax.FieldChild(Type, ChildRole::Name);
				if(Name == NoNode || Syntax.FieldChild(Type, ChildRole::Arguments) != NoNode) break;
				Type = Name;
			}
			// 推論し得ない型の返戻
			if(!Syntax.Is(Type, Role::DeducedType)) return false;
			// 初期化子を持つ宣言子の有無の返戻
			return std::ranges::any_of(
				Decl.Lists.front().Items,
				[&Syntax](const std::vector<NodeId> &Item) -> bool {
					// 宣言子が初期化子を持つかの返戻
					return std::ranges::any_of(
						Item,
						[&Syntax](const NodeId Piece) -> bool {
							// 初期化子の場の子を持つかの返戻
							return Syntax.FieldChild(Piece, ChildRole::Value) != NoNode;
						}
					);
				}
			);
		};
		// 宣言の最後の要素の最後の子
		const auto LastPiece = [](const CommaDecl &Decl) -> NodeId {
			// 最後の並びの最後の要素の最後の子の返戻
			return Decl.Lists.back().Items.back().back();
		};
		// 後の宣言が並びの前の名前を参照又は再宣言するか（名前と初期化子を別に並べる宣言だけ）
		const auto RefersEarlier = [&Syntax](const CommaDecl &Next, const std::unordered_set<std::string> &Earlier) -> bool {
			// 名前と初期化子を別に並べない宣言の返戻
			if(Next.Lists.size() < 2) return false;
			// 前の名前を再宣言する宣言の返戻
			for(const std::vector<NodeId> &Name : Next.Lists[0].Items) {
				// 既存の束縛を重ねる宣言の返戻
				if(!Syntax.GetNode(Name.front()).Facts.DiscardsBinding && Earlier.contains(Syntax.ComparisonName(Name.front()))) return true;
			}
			// 初期化子の中の前の名前の参照の有無の返戻
			return AnyNodeOf(
				Syntax,
				Next.Lists[1].Host,
				[&](const NodeId Sub) -> bool {
					// 前の名前の参照かの返戻
					return Syntax.Is(Sub, Role::Identifier) && Earlier.contains(Syntax.ComparisonName(Sub));
				}
			);
		};
		// 宣言子毎の宣言の雛形（共有する子の写し）と，並び毎の要素の子を置く位置（雛形の中の経路と子の番号）
		struct FrameSlot {
			std::vector<size_t> Path {}; // 雛形の根から要素を置く節点への子の番号の経路
			size_t Index = 0; // 要素を置く子の番号
		};
		// 宣言の雛形の組立（並びの節点は其の中を組み立て，要素と要素の間の `,` は除いて其の位置を控える）
		const auto BuildFrame = [&Syntax](
			const CommaDecl &Decl,
			const std::unordered_map<NodeId, int32_t> &ListOf,
			const NodeId Parent,
			std::vector<size_t> &Path,
			std::vector<FrameSlot> &Slots
		) -> GeneratedNode {
			const auto MakeFrame = [&Syntax](const NodeId Id) -> GeneratedNode {
				const SyntaxNode &Original = Syntax.GetNode(Id);
				GeneratedNode Out { {}, true, {}, Original.Fields };
				Out.Definition = Original.Kind.Definition;
				Out.Facts = Original.Facts;
				Out.Source = Id;
				// 宣言の雛形の節点の返戻
				return Out;
			};
			struct Frame {
				NodeId Parent;
				GeneratedNode *Out;
				size_t NextChild = 0;
				bool IsHost = false;
			};
			GeneratedNode Out = MakeFrame(Parent);
			// 共有する子の複写と要素を挿す位置の記録
			std::vector<Frame> Pending { { Parent, &Out } };
			while(!Pending.empty()) {
				Frame &Current = Pending.back();
				const std::span<const NodeId> Children = Syntax.ChildrenOf(Current.Parent);
				if(Current.NextChild == Children.size()) {
					if(Current.IsHost) Path.pop_back();
					Pending.pop_back();
					continue;
				}
				const NodeId Child = Children[Current.NextChild++];
				const std::unordered_map<NodeId, int32_t>::const_iterator Found = ListOf.find(Child);
				const bool IsHost = std::ranges::any_of(
					Decl.Lists,
					[Child](const DeclList &List) -> bool {
						return List.Host == Child;
					}
				);
				if(Child != Current.Parent && IsHost) {
					Path.push_back(Current.Out->Children.size());
					Current.Out->Children.push_back(MakeFrame(Child));
					Pending.push_back({ Child, &Current.Out->Children.back(), 0, true });
				} else if(Found == ListOf.end() || Found->second == -1) Current.Out->Children.push_back(Syntax.CopyOf(Child));
				else if(Found->second >= 0 && Slots[static_cast<size_t>(Found->second)].Index == SIZE_MAX) {
					Slots[static_cast<size_t>(Found->second)] = { Path, Current.Out->Children.size() };
				}
			}
			// 組み立てた雛形の返戻
			return Out;
		};
		// 宣言を並べる容器の構文順の収集
		std::vector<NodeId> Containers = ParentsOfRole(Syntax, Role::CommaDeclaration, { Role::Error }), OuterContainers;
		for(const NodeId Wrapper : Containers) if(Syntax.Is(Wrapper, Role::GlobalStatement)) {
			if(Syntax.ParentOf(Wrapper) != NoNode) OuterContainers.push_back(Syntax.ParentOf(Wrapper));
		}
		Containers.insert(Containers.end(), OuterContainers.begin(), OuterContainers.end());
		Containers = InDocumentOrder(Syntax, Containers, {});
		for(const NodeId Container : Containers) {
			// 連続する宣言と分割による位置のずれの記録
			const bool IsSplitScope = IsDeclSplitScope(Syntax, Container);
			std::vector<CommaDecl> Run;
			std::unordered_set<std::string> RunNames;
			// 分けて加えた宣言の数（後の子の位置のずれ）
			uint32_t Added = 0;
			// 並びの宣言を最初の宣言へ纏める計画の追加
			const auto Flush = [&]() -> void {
				if(Run.size() > 1) {
					const CommaDecl &First = Run.front();
					std::vector<RewriteStep> Removals;
					std::unordered_map<NodeId, size_t> RemovalByParent;
					// 最初の宣言の末尾の錨は其の最後の要素の後へ移す（纏めた宣言の後は最後の宣言の物）
					MoveTrailing(First.Node, LastPiece(First));
					// 並び毎に後の宣言の要素を最初の宣言の最後の要素の後へ移す
					for(size_t List = 0; List < First.Lists.size(); ++List) {
						const NodeId Host = First.Lists[List].Host;
						uint32_t Index = Syntax.GetNode(First.Lists[List].Items.back().back()).Index + 1;
						for(size_t Part = 1; Part < Run.size(); ++Part) for(const std::vector<NodeId> &Item : Run[Part].Lists[List].Items) {
							RewriteStep Comma { RewriteKind::Insert, Host };
							Comma.Index = Index++;
							Comma.Inserted = { GeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::Separator) };
							Plan.Steps.push_back(std::move(Comma));
							for(const NodeId Piece : Item) {
								// 要素の中の子の前は元の境界を保つ
								const SyntaxNode &Moved = Syntax.GetNode(Piece);
								const SemanticBoundary *const Recorded = Syntax.GetNode(Moved.Parent).Facts.BoundaryBefore(Moved.Index);
								RewriteStep Move { RewriteKind::Move, Piece };
								Move.Destination = Host;
								Move.Index = Index++;
								if(Piece != Item.front() && Recorded) {
									Move.Boundary = Recorded->Kind;
									Move.RecordedBoundary = *Recorded;
								} else Move.Boundary = BoundaryKind::Spaced;
								Plan.Steps.push_back(std::move(Move));
							}
						}
					}
					for(size_t Part = 1; Part < Run.size(); ++Part) {
						// 最後の宣言の末尾の錨は纏めた宣言の後，其の他は其の宣言の最後の要素の後
						const AnchorTarget End = Part + 1 == Run.size() ? AnchorTarget{ First.Node, true } : AnchorTarget{ LastPiece(Run[Part]), true };
						// 宣言子の前後に在る共有する名前付の子の対応（Go の型は名前の後に続く）
						std::unordered_map<NodeId, NodeId> Twins;
						std::vector<std::pair<NodeId, NodeId>> Pairs;
						std::vector<NodeId> Shared, FirstShared;
						for(const DeclPiece &Piece : Run[Part].Pieces) if(Piece.List < 0 && Syntax.IsNamed(Piece.Node)) Shared.push_back(Piece.Node);
						for(const DeclPiece &Piece : First.Pieces) if(Piece.List < 0 && Syntax.IsNamed(Piece.Node)) FirstShared.push_back(Piece.Node);
						if(Shared.size() != FirstShared.size()) {
							throw FormatLimitExceeded("merged declarations have different shared syntax", FailureReason::FormatterDefect);
						}
						for(size_t Piece = 0; Piece < Shared.size(); ++Piece) Pairs.emplace_back(Shared[Piece], FirstShared[Piece]);
						while(!Pairs.empty()) {
							const auto [Removed, Twin] = Pairs.back();
							Pairs.pop_back();
							Twins.emplace(Removed, Twin);
							for(uint32_t Child = 0; Child < Syntax.ChildCount(Removed) && Child < Syntax.ChildCount(Twin); ++Child) {
								Pairs.emplace_back(Syntax.Child(Removed, Child), Syntax.Child(Twin, Child));
							}
						}
						std::vector<NodeId> Kept;
						for(const DeclPiece &Piece : Run[Part].Pieces) if(Piece.List >= 0) Kept.push_back(Piece.Node);
						const NodeId Parent = Syntax.ParentOf(Run[Part].Node);
						const auto [Place, IsNew] = RemovalByParent.try_emplace(Parent, Removals.size());
						if(IsNew) Removals.push_back({ RewriteKind::RemoveSiblings, Parent });
						RemoveMergedDecl(Syntax, Run[Part].Node, End, Kept, Twins, Removals[Place->second], Plan);
					}
					for(RewriteStep &Removal : Removals) Plan.Steps.push_back(std::move(Removal));
				}
				Run.clear();
				RunNames.clear();
			};
			for(const NodeId Child : Syntax.ChildrenOf(Container)) {
				// C# の最上位の文の包み
				const NodeId Wrapper = Syntax.Is(Child, Role::GlobalStatement) ? Child : NoNode;
				std::optional<CommaDecl> Part = Syntax.CommaDeclOf(Wrapper == NoNode ? Child : Syntax.NamedChild(Child, 0));
				if(!Part) {
					Flush();
					continue;
				}
				// 並びの節点の末尾の錨は其の中の最後の要素の子の後へ移す（統合・分割の区分が節点を要素の区切りで分ける為，行分割仕様 4.1）
				for(const DeclList &List : Part->Lists) {
					if(List.Host == Part->Node) continue;
					const std::vector<DeclPiece>::const_reverse_iterator Last = std::ranges::find_if(
						Part->Pieces.rbegin(),
						Part->Pieces.rend(),
						[&Syntax, &List](const DeclPiece &Piece) -> bool {
							// 並びの節点の中の子かの返戻
							return Syntax.Contains(List.Host, Piece.Node);
						}
					);
					if(Last->List >= 0) MoveTrailing(List.Host, Last->Node);
				}
				if(ShouldSplitBindings && !Syntax.HasWordChild(Part->Node, Word::ReassignableBinding)) continue;
				if(IsSplitScope || ShouldSplitBindings) {
					Flush();
					if(Part->Lists.front().Items.size() < 2) continue;
					// 要素毎の宣言への分割（２つ目以降の要素を共有する子の写しと共に後へ置く）
					// 要素の子と要素の間の `,` の並びの番号（共有する子は -1，`,` は並びの番号の外）
					std::unordered_map<NodeId, int32_t> ListOf;
					for(const DeclPiece &Piece : Part->Pieces) ListOf.emplace(Piece.Node, Piece.List);
					for(const DeclList &List : Part->Lists) {
						for(size_t Item = 1; Item < List.Items.size(); ++Item) ListOf.emplace(Syntax.PrevSibling(List.Items[Item].front()), -2);
					}
					std::vector<FrameSlot> Slots(Part->Lists.size(), { {}, SIZE_MAX });
					std::vector<size_t> Path;
					const GeneratedNode Template = BuildFrame(*Part, ListOf, Part->Node, Path, Slots);
					uint32_t Index = Syntax.GetNode(Child).Index + Added + 1;
					for(uint32_t Item = 1; Item < Part->Lists.front().Items.size(); ++Item) {
						GeneratedNode Split = Template;
						// 並び毎の其の要素の子を雛形の控えた位置へ置く
						for(size_t List = 0; List < Part->Lists.size(); ++List) {
							GeneratedNode *At = &Split;
							for(const size_t Step : Slots[List].Path) At = &At->Children[Step];
							std::vector<GeneratedNode> Pieces;
							for(const NodeId Piece : Part->Lists[List].Items[Item]) {
								Pieces.push_back({ {}, false, {}, Syntax.GetNode(Piece).Fields, Piece });
							}
							const std::vector<GeneratedNode>::iterator Place = At->Children.begin() + static_cast<std::ptrdiff_t>(Slots[List].Index);
							At->Children.insert(Place, Pieces.begin(), Pieces.end());
						}
						if(Wrapper != NoNode) {
							GeneratedNode Wrapped { {}, true, { std::move(Split) } };
							Wrapped.Definition = Syntax.GetNode(Wrapper).Kind.Definition;
							Wrapped.Facts = Syntax.GetNode(Wrapper).Facts;
							Wrapped.Source = Wrapper;
							Split = std::move(Wrapped);
						}
						Syntax.RemapGeneratedBoundaries(Split);
						RewriteStep Insert { RewriteKind::Insert, Container };
						Insert.Index = Index++;
						Insert.Boundary = BoundaryKind::Spaced;
						Insert.Inserted = std::move(Split);
						Plan.Steps.push_back(std::move(Insert));
						++Added;
					}
					// 要素の間のカンマの錨は前の要素の後へ，元の宣言（と包み）の末尾の錨は最後の要素の後へ移す
					for(const DeclList &List : Part->Lists) for(size_t Item = 1; Item < List.Items.size(); ++Item) {
						const NodeId Separator = Syntax.PrevSibling(List.Items[Item].front());
						const AnchorTarget Previous { List.Items[Item - 1].back(), true };
						Plan.Steps.push_back({ RewriteKind::Remove, Separator, {}, { Separator, Previous, Previous, Previous } });
					}
					if(Wrapper != NoNode) MoveTrailing(Wrapper, LastPiece(*Part));
					MoveTrailing(Part->Node, LastPiece(*Part));
					continue;
				}
				// 型引数を初期化子から推論し得る型名の初期化付の宣言は宣言子毎に型が決まる為に纏めない
				if(DeducesDeclaratorTypes(*Part)) {
					Flush();
					continue;
				}
				// 共有する子・束縛の違いと，前後の評価と代入の順を保てない宣言での区切り
				if(
					!Run.empty() && (
						Part->Key != Run.front().Key || RefersEarlier(*Part, RunNames) ||
						Syntax.GetNode(Run.back().Node).Facts.MayReassignExistingBinding || Syntax.GetNode(Part->Node).Facts.MayReassignExistingBinding
					)
				) Flush();
				if(Part->Lists.size() > 1) for(const std::vector<NodeId> &Name : Part->Lists[0].Items) {
					if(!Syntax.GetNode(Name.front()).Facts.DiscardsBinding) RunNames.insert(Syntax.ComparisonName(Name.front()));
				}
				Run.push_back(std::move(*Part));
			}
			Flush();
		}
		// 終了
		return;
	}
} // namespace Core
