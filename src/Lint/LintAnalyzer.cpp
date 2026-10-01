#include "LintAnalyzer.hpp"
#include "../Core/CommentFormat.hpp"
#include "../Core/ControlFlow.hpp"
#include "../Core/DocCommentSynchronizer.hpp"
#include "../Core/SignatureExtractor.hpp"
#include "../Util/CharClass.hpp"
#include "../Util/CommentNormalizer.hpp"
#include "../Util/TextEdit.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using Core::NodeId;
using Core::NoNode;
using Core::Role;
using Core::Word;
using Core::ChildRole;
using StaticLint::OutputPoint;
using StaticLint::FunctionFacts;
using Core::DocumentationStyle;
using Core::DocItem;
using Core::DocItemKind;
using Core::DocCommentSynchronizer;

/** ========== 共通判定 ========== */
/**
 * ノードのソース上テキスト参照取得関数
 * @param Source ソース文字列
 * @param Node 対象ノード
 * @return ノード範囲のテキスト参照（不正範囲なら空）
 */
std::string_view LintAnalyzer::NodeView(const std::string &Source, const NodeId Node) {
	// 字面を参照するバイト範囲の取得
	const uint32_t StartByte = BeginOf(Node), EndByte = EndOf(Node);
	// 範囲の壊れた節点の空の返戻
	if(StartByte >= EndByte || EndByte > Source.size()) return {};
	// ソース上の対象範囲の参照の返戻
	return std::string_view(Source.data() + StartByte, EndByte - StartByte);
}

/**
 * 代入可能式の包み剥離識別子名取得関数
 * @param Source ソース文字列
 * @param Lhs 開始ノード
 * @return 識別子名（剥離失敗時は空）
 */
std::string_view LintAnalyzer::UnwrapAssignableName(const std::string &Source, NodeId Lhs) {
	// 代入先を包む構文の順次除去
	while(Lhs != NoNode) {
		// 識別子の名前の返戻
		if(Syntax.Is(Lhs, Role::IdentifierLeafLike)) return NodeView(Source, Lhs);
		// 名前を包む節点でない代入先は名前を持たない事の返戻
		if(!Syntax.Is(Lhs, Role::AssignableWrapper)) return {};
		// 最初の名前付子を直接取得する（全子走査を省略，中身の無い包みは空ノードで繰返を離脱）
		Lhs = Syntax.NamedChild(Lhs, 0);
	}
	// 剥離失敗時は空参照の返戻
	return {};
}

/**
 * 違反１件追加関数
 * @param Node 違反位置のノード（行・列を取得する）
 * @param Rule 警告一覧の識別子
 * @param Message 警告文
 * @param Out 警告格納先
 */
void LintAnalyzer::Push(const NodeId Node, const LintRule Rule, const std::string_view Message, std::vector<LintWarning> &Out) {
	// 警告を結び付けるソース位置の取得
	Push(PointOf(Node), Rule, Message, Out);
	// 終了
	return;
}

/**
 * 出力位置の警告の追加関数
 * @param Point 最終字面の位置
 * @param Rule 警告一覧の識別子
 * @param Message 警告の本文
 * @param Out 警告の格納先
 */
void LintAnalyzer::Push(
	const OutputPoint Point,
	const LintRule Rule,
	const std::string_view Message,
	std::vector<LintWarning> &Out
) {
	// 集成体は `emplace_back` の括弧初期化を受けれない為，波括弧で構築して追加
	Out.push_back({ Point.Row, Point.Column, std::string(Message), Rule });
	// 終了
	return;
}

/** ========== 規約検査 ========== */
/**
 * アクセス修飾子順序違反検出関数
 * @param Source ソース文字列
 * @param Node 対象ノード
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckAccessSpecifierOrder(const std::string &Source, const NodeId Node, std::vector<LintWarning> &Out) {
	// アクセス順序を定めない言語の場合の終了
	if(Rules.AccessOrder.empty()) return;
	const auto RankOf = [this](const std::string_view Access) -> int {
		const std::vector<std::string_view>::const_iterator Found = std::ranges::find(Rules.AccessOrder, Access);
		return Found == Rules.AccessOrder.end() ? -1 : static_cast<int>(Found - Rules.AccessOrder.begin());
	};
	std::string Order;
	for(const std::string_view Access : Rules.AccessOrder) {
		if(!Order.empty()) Order += " → ";
		Order += Access;
	}
	int LastRank = -1;
	bool IsReported = false;
	const auto TrackRank = [&](const NodeId Target, const int Rank) -> bool {
		// 順位を持たない修飾子を通す事の返戻
		if(Rank < 0) return true;
		if(Rank < LastRank && !IsReported) {
			Push(
				Target,
				LintRule::AccessOrder,
				"Review: Reorder access specifiers as " + Order + "; verify lifetime, external contract",
				Out
			);
			IsReported = true;
		}
		LastRank = Rank;
		// 順序違反を未報告かの返戻
		return !IsReported;
	};
	Core::ForEachNamedChildOf(
		Syntax,
		Node,
		[&](const NodeId Body) -> bool {
			const NodeId Members = Rules.HasModifierGroups && Syntax.Is(Body, Role::EnumerationBody) ?
			Syntax.FirstNamedChildWithRole(Body, Role::EnumBodyDeclarations) :
			Syntax.Is(Body, Role::AccessOrderedBody) || Rules.HasAccessSections && Syntax.Is(Body, Role::ClassBody) ? Body : NoNode;
			// 成員を持たない本体を通す事の返戻
			if(Members == NoNode) return true;
			bool HasSection = false, ReportedPrivate = false;
			Core::ForEachNamedChildOf(
				Syntax,
				Members,
				[&](const NodeId Member) -> bool {
					if(Rules.HasAccessSections) {
						if(Syntax.Is(Member, Role::AccessSection)) {
							HasSection = true;
							std::string_view Access = NodeView(Source, Member);
							while(!Access.empty() && (Access.back() == ' ' || Access.back() == '\t')) Access.remove_suffix(1);
							if(Access.ends_with(':')) Access.remove_suffix(1);
							while(!Access.empty() && (Access.back() == ' ' || Access.back() == '\t')) Access.remove_suffix(1);
							// 明示したアクセス修飾子の順位判定の返戻
							return TrackRank(Member, RankOf(Access));
						}
						// 対象外の成員又は既存の区分を通す事の返戻
						if(!Syntax.Is(Member, Role::AccessOrderMember) || HasSection) return true;
						if(Rules.RequireExplicitPrivate && Syntax.Is(Node, Role::ImplicitPrivateClass) && !ReportedPrivate) {
							Push(Member, LintRule::ExplicitAccess, "Violation: Make private: explicit", Out);
							ReportedPrivate = true;
						}
						// 暗黙のアクセス修飾子の順位判定の返戻
						return TrackRank(Member, RankOf(Syntax.Is(Node, Role::ImplicitPrivateClass) ? "private" : "public"));
					}
					// 対象外の成員を通す事の返戻
					if(!Syntax.Is(Member, Role::AccessOrderMember)) return true;
					std::string Access;
					NodeId Specifier = NoNode;
					const auto ReadModifier = [&](const NodeId Modifier) -> void {
						// 順位を持たない修飾子を読み飛ばす場合の終了
						if(RankOf(NodeView(Source, Modifier)) < 0) return;
						if(Specifier == NoNode) Specifier = Modifier;
						if(!Access.empty()) Access += ' ';
						Access += NodeView(Source, Modifier);
					};
					for(const NodeId Child : Syntax.ChildrenOf(Member)) {
						if(Syntax.Is(Child, Role::AccessModifier)) ReadModifier(Child);
						else if(Syntax.Is(Child, Role::ModifierSequence)) for(const NodeId Modifier : Syntax.ChildrenOf(Child)) ReadModifier(Modifier);
					}
					if(Access.empty()) Access = Rules.ImplicitAccess;
					if(Specifier == NoNode && Rules.RequireExplicitInternal && Access == Rules.ImplicitAccess) {
						Push(Member, LintRule::ExplicitAccess, "Violation: Make internal explicit", Out);
					}
					// 成員のアクセス修飾子の順位判定の返戻
					return TrackRank(Specifier == NoNode ? Member : Specifier, RankOf(Access));
				}
			);
			// 順序違反を未報告かの返戻
			return !IsReported;
		}
	);
	// 終了
	return;
}

/**
 * goto／ラベル付 break・continue 検出関数
 * @param Source ソース文字列（ラベルの名前の照合に使う）
 * @param Node 対象ノード
 * @param Labels ラベルが名付けるループの索引（親を一度だけ走って纏めて記録する）
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckGotoOrLabel(
	const std::string &Source,
	const NodeId Node,
	LabelTargetIndex &Labels,
	std::vector<LintWarning> &Out
) {
	// 節点の型は `Run` の振分で確定済
	if(Syntax.Is(Node, Role::GotoStatement)) {
		// C# の `goto case` / `goto default` を除く場合の終了（`switch` の転送で，素通りを禁じる言語仕様が要求する書き方）
		if(const NodeId Target = Syntax.Child(Node, 1); Syntax.Is(Target, Role::SwitchGotoTarget)) return;
		const NodeId Name = Syntax.NamedChild(Node, 0);
		NodeId Scope = Syntax.GetRoot();
		for(NodeId Parent = Syntax.ParentOf(Node); Parent != NoNode; Parent = Syntax.ParentOf(Parent)) {
			if(Syntax.Is(Parent, Role::FlowBoundary)) {
				Scope = Parent;
				break;
			}
		}
		NodeId Target = NoNode;
		if(Name != NoNode) {
			WalkChildrenCursor(
				Scope,
				[this, &Source, &Target, Name](const NodeId Cur) -> bool {
					// 関数境界へ達した場合の探索打切の返戻
					if(Syntax.Is(Cur, Role::FlowBoundary)) return false;
					if(
						Syntax.Is(Cur, Role::LabeledConstruct) && Syntax.NamedChildCount(Cur) &&
						NodeView(Source, Syntax.NamedChild(Cur, 0)) == NodeView(Source, Name)
					) Target = Cur;
					// 対象の境界が無い事の返戻
					return Target == NoNode;
				}
			);
		}
		uint32_t ExitedLoops = 0;
		if(Target != NoNode) for(NodeId Parent = Syntax.ParentOf(Node); Parent != NoNode; Parent = Syntax.ParentOf(Parent)) {
			if(Syntax.Is(Parent, Role::FlowBoundary)) break;
			if(!Syntax.Is(Parent, Role::RepeatedControl)) continue;
			bool ContainsTarget = false;
			for(NodeId Enclosing = Target; Enclosing != NoNode; Enclosing = Syntax.ParentOf(Enclosing)) if(Enclosing == Parent) {
				ContainsTarget = true;
				break;
			}
			if(!ContainsTarget) ++ExitedLoops;
		}
		// 入れ子の繰返を脱出する場合の終了
		if(ExitedLoops > 1) return;
		Push(
			Node,
			LintRule::JumpUsage,
			Target == NoNode ?
			"Review: Replace this jump with structured control flow; verify applicability, binding, lifetime" :
			"Review: Replace this jump with structured control flow; verify applicability, lifetime",
			Out
		);
	} else {
		// 定義でない脱出の参照を除く場合の終了
		if(Syntax.Is(Syntax.ParentOf(Node), Role::JumpWithLabel)) return;
		std::string_view Label = Syntax.Is(Node, Role::LabeledStatement) && Syntax.NamedChildCount(Node) ?
		NodeView(Source, Syntax.NamedChild(Node, 0)) :
		NodeView(Source, Node);
		if(Label.starts_with("'")) Label.remove_prefix(1);
		if(Label.ends_with(":") || Label.ends_with("@")) Label.remove_suffix(1);
		// ラベルを持たない場合の終了
		if(Label.empty()) return;
		// ラベル対象がループかの判定関数
		const auto IsLoopNode = [this](const NodeId Cur) -> bool {
			// 繰返かの返戻
			return Syntax.Is(Cur, Role::LabeledLoop);
		};
		// 言語別配置に応じたラベル対象ループの位置解決
		NodeId Target = NoNode;
		Core::ForEachNamedChildOf(
			Syntax,
			Node,
			[this, &Target, &IsLoopNode](const NodeId Child) -> bool {
				// 重ねたラベル (`a: b: for`) は内側のラベルの後の文を名付ける為，ラベルを辿って其の文で見る
				NodeId Inner = Child;
				while(Syntax.Is(Inner, Role::LabeledConstruct)) {
					const uint32_t Count = Syntax.NamedChildCount(Inner);
					if(Count < 2) break;
					Inner = Syntax.NamedChild(Inner, Count - 1);
				}
				if(IsLoopNode(Inner)) Target = Inner;
				// ループを包むラベル (Java / JavaScript / TypeScript / Go / C#) を見付ける迄の走査継続の返戻
				return Target == NoNode;
			}
		);
		// 文を包むラベルの子に限定した対象解決
		const bool IsWrapping = Syntax.Is(Node, Role::LabeledStatement);
		// ラベルがループの子に在る形（Rust の `'outer: for`）
		if(!IsWrapping && Target == NoNode) if(const NodeId Parent = Syntax.ParentOf(Node); IsLoopNode(Parent)) Target = Parent;
		// ラベルがループの後の兄弟に在る形
		if(!IsWrapping && Target == NoNode) {
			if(Labels.find(Node) == Labels.end()) {
				NodeId Pending = NoNode;
				Core::ForEachNamedChildOf(
					Syntax,
					Syntax.ParentOf(Node),
					// ラベルと直後の文の対応付け関数
					[this, &Labels, &Pending](const NodeId Child) -> void {
						if(Pending != NoNode) {
							// ラベル名別の節点索引への登録
							Labels.emplace(Pending, Child);
							Pending = NoNode;
						}
						if(Syntax.Is(Child, Role::LabelInspection)) Pending = Child;
					}
				);
				if(Pending != NoNode) Labels.emplace(Pending, NoNode);
			}
			if(const LabelTargetIndex::const_iterator Hit = Labels.find(Node); Hit != Labels.end()) Target = Hit->second;
		}
		// ラベルの存在でなく，関数境界内の同じラベルへ向かう脱出毎に検査
		WalkChildrenCursor(
			Target == NoNode ? Node : Target,
			[this, &Source, Label, Target, Node, &Out](const NodeId Cur) -> bool {
				// 別の関数の内部を調べない事の返戻
				if(Syntax.Is(Cur, Role::FlowBoundary)) return false;
				// 検査対象の脱出文でない場合の継続の返戻
				if(
					!Syntax.Is(Cur, Role::LoopExitStatement) && !Syntax.Is(Cur, Role::KeywordJump) || !Syntax.NamedChildCount(Cur) ||
					!Core::AnyChildOf(
						Syntax,
						Cur,
						[this](const NodeId Child) -> bool {
							return Syntax.Is(Child, Role::BreakKeyword) || Syntax.Is(Child, Role::ContinueKeyword);
						}
					)
				) return true;
				std::string_view ExitLabel = NodeView(Source, Syntax.NamedChild(Cur, 0));
				if(ExitLabel.starts_with("'")) ExitLabel.remove_prefix(1);
				if(ExitLabel.ends_with(":") || ExitLabel.ends_with("@")) ExitLabel.remove_suffix(1);
				// 異なるラベルへ向かう場合の継続の返戻
				if(ExitLabel != Label) return true;
				const bool IsBreak = Core::AnyChildOf(
					Syntax,
					Cur,
					[this](const NodeId Child) -> bool {
						return Syntax.Is(Child, Role::BreakKeyword);
					}
				);
				uint32_t LoopDepth = 0;
				for(NodeId Parent = Syntax.ParentOf(Cur); Parent != NoNode; Parent = Syntax.ParentOf(Parent)) {
					// 関数の境界の外と，脱出先を構文から確定出来ないラムダ越し（非局所の脱出又は解析器の読違い）は報告しない事の返戻
					if(Syntax.Is(Parent, Role::FlowBoundary) || Syntax.Is(Parent, Role::LambdaLiteral)) return false;
					if(Syntax.Is(Parent, IsBreak ? Role::NestedBreakTarget : Role::RepeatedControl)) ++LoopDepth;
					if(Parent == Target || Parent == Node) break;
				}
				// 複数の繰返を脱出する場合の探索打切の返戻
				if(Target != NoNode && LoopDepth > 1) return false;
				Push(Cur, LintRule::JumpUsage, "Review: Replace this jump with structured control flow; verify applicability, lifetime", Out);
				// 一致した脱出文の探索打切の返戻
				return false;
			}
		);
	}
	// 終了
	return;
}

/**
 * 無意味な局所変数検出関数
 * @param Source ソース文字列
 * @param Node 対象ノード
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckUselessLocalVariable(const std::string &Source, const NodeId Node, std::vector<LintWarning> &Out) {
	// 直前の宣言と返戻文の対応追跡状態の初期化
	NodeId Prev = NoNode;
	Core::ForEachNamedChildOf(
		Syntax,
		Node,
		// 直前宣言と返戻文の照合関数
		[&](const NodeId Child) -> void {
			const NodeId Decl = std::exchange(Prev, Child);
			// 宣言の直後の return でない場合の終了
			if(Decl == NoNode || !Syntax.Is(Child, Role::ReturnStatement) || !Syntax.Is(Decl, Role::DeclLike)) return;
			// 観測される修飾子を持つ宣言を除く場合の終了
			if(
				Core::AnyNodeOf(
					Syntax,
					Decl,
					[this](const NodeId Part) -> bool {
						return Syntax.Is(Part, Role::ObservableQualifier);
					}
				)
			) return;
			// 変数の束縛を包む宣言が有れば其の中の宣言子を確認
			const NodeId Wrapper = Syntax.FirstNamedChildWithRole(Decl, Role::VariableDeclaration);
			std::string_view DeclName;
			NodeId DeclIdentifier = NoNode;
			Core::ForEachNamedChildOf(
				Syntax,
				Wrapper == NoNode ? Decl : Wrapper,
				[this, &Source, &DeclName, &DeclIdentifier](const NodeId Declarator) -> bool {
					// 初期化付の宣言子以外を読み飛ばす事の返戻
					if(!Syntax.Is(Declarator, Role::DeclInitDeclarator)) return true;
					// 宣言子からの識別名抽出
					if(const NodeId Identifier = Syntax.FirstNamedChildWithRole(Declarator, Role::Identifier); Identifier != NoNode) {
						DeclName = NodeView(Source, Identifier);
						DeclIdentifier = Identifier;
					}
					// 宣言名取得済なら走査打切，未取得なら次の兄弟への返戻
					return DeclName.empty();
				}
			);
			// 宣言名が無い場合の終了
			if(DeclName.empty()) return;
			// 宣言名と一致する返却識別子の冗長局所変数認定
			if(
				// 返却された識別子節点
				const NodeId Returned = Syntax.FirstNamedChildWithRole(Child, Role::Identifier);
				Returned != NoNode && NodeView(Source, Returned) == DeclName
			) {
				Push(
					DeclIdentifier,
					LintRule::ImmediateReturnVariable,
					"Review: Inline immediately returned variable " + std::string(DeclName) + "; verify type, binding, lifetime, intent",
					Out
				);
			}
		}
	);
	// 終了
	return;
}

/**
 * 型推論キーワードの例外文脈判定関数
 * @param AutoNode auto / var / any 等のキーワードノード
 * @return 標準仕様の例外文脈に有れば true
 */
bool LintAnalyzer::IsAutoExceptionContext(const NodeId AutoNode) {
	// 型推論の置かれた宣言文脈の取得
	const NodeId Parent = Syntax.ParentOf(AutoNode);
	// 親の無い auto は例外でない事の返戻
	if(Parent == NoNode) return false;
	// `[this](auto &x){}` のジェネリックラムダ仮引数を例外とする事の返戻
	if(Syntax.Is(Parent, Role::AutoParameterHost)) return true;
	// 通常宣言でない親を例外としない事の返戻
	if(!Syntax.Is(Parent, Role::TypedDeclaration)) return false;
	// 例外文脈に当たる初期化子を持つ宣言子の有無の返戻
	return Core::AnyChildOf(
		Syntax,
		Parent,
		// 初期化子付宣言子の判定関数
		[this](const NodeId Child) -> bool {
			// 初期化子付の宣言子が例外文脈の初期化子を持つかの返戻
			return Syntax.Is(Child, Role::InitDeclarator) && Core::AnyChildOf(
				Syntax,
				Child,
				// 型推論を許容する初期化子の判定関数
				[this](const NodeId GrandChild) -> bool {
					// 型名を書けない構造化束縛・ラムダの初期化子かの返戻
					return Syntax.Is(GrandChild, Role::UnnameableInitializer);
				}
			);
		}
	);
}

/**
 * auto / var / any 使用検出関数
 * @param Node 対象ノード
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckTypeInferenceKeyword(const NodeId Node, std::vector<LintWarning> &Out) {
	// 対象の構文は `Run` が役割で選択済
	switch(Rules.InferredType) {
	case Core::InferredTypeNotation::Placeholder:
		// 型名を書けない又は冗長な構文に於ける auto の許容
		if(!IsAutoExceptionContext(Node)) {
			NodeId Keyword = NoNode;
			WalkChildrenCursor(
				Node,
				[this, &Keyword](const NodeId Child) -> bool {
					if(Syntax.Is(Child, Role::DeducedTypeKeyword)) Keyword = Child;
					return Keyword == NoNode;
				}
			);
			if(Keyword != NoNode) {
				Push(
					Keyword,
					LintRule::InferredType,
					"Review: Use an explicit type instead of " + std::string(NodeView(Source, Keyword)) + "; verify applicability, type",
					Out
				);
			}
		}
		// 終了
		return;
	case Core::InferredTypeNotation::Implicit:
		// 推論型の専用構文を対象とし，同じ綴りの変数名や参照は対象外
		Push(
			Node,
			LintRule::InferredType,
			"Review: Use an explicit type instead of " + std::string(NodeView(Source, Node)) + "; verify applicability, type",
			Out
		);
		// 終了
		return;
	case Core::InferredTypeNotation::Named:
		// 型の名前の位置で推論を指定する綴りの照合
		if(Syntax.IsWord(Node, Word::InferredType)) {
			Push(
				Node,
				LintRule::InferredType,
				"Review: Use an explicit type instead of " + std::string(NodeView(Source, Node)) + "; verify applicability, type",
				Out
			);
		}
		// 終了
		return;
	case Core::InferredTypeNotation::DynamicTop:
		if(Syntax.IsWord(Node, Word::DynamicTopType)) {
			Push(Node, LintRule::AnyType, "Violation: Replace `any` with `unknown` or a specific type", Out);
		}
		// 終了
		return;
	default:
		// 終了
		return;
	}
}

/**
 * 名前無子トークンからの候補一致トークン検索関数
 * @param Source ソース文字列
 * @param Node 走査対象ノード
 * @param Option1 １番目の候補トークン
 * @param Option2 ２番目の候補トークン
 * @return 一致トークン参照（見付からなければ空）
 */
std::string_view LintAnalyzer::FindUnnamedToken(
	const std::string &Source,
	const NodeId Node,
	const std::string_view Option1,
	const std::string_view Option2
) {
	// 検索結果の格納先準備
	std::string_view Result;
	Core::ForEachChildOf(
		Syntax,
		Node,
		[&](const NodeId Child) -> bool {
			// 名前無子の候補照合
			if(!Syntax.IsNamed(Child)) if(const std::string_view Token = NodeView(Source, Child); Token == Option1 || Token == Option2) {
				Result = Token;
				// 一致発見時の走査打切の返戻（対象字句は１節点に高々１つ）
				return false;
			}
			// 未発見の為，走査継続の返戻
			return true;
		}
	);
	// 一致トークンの返戻
	return Result;
}

/**
 * 関数本体終端と到達不能 return 検証関数
 * 通常関数の末尾へ到達する経路が残る物，到達を確かめれない物（要確認）と，終端した後の返戻文を報告する（値を返さない関数の末尾は整形で補う為，補えなかった物だけが残る）
 * @param Node 対象ノード
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckMissingExplicitReturn(const NodeId Node, std::vector<LintWarning> &Out) {
	// 到達不能な返戻と関数末尾の不足の報告
	const FunctionFacts &Function = Facts.Functions.at(Node);
	for(const NodeId Statement : Function.UnreachableReturningStatements) {
		const NodeId Return = Syntax.FirstNamedChildWithRole(Statement, Role::ReturnStatement);
		Push(
			Return == NoNode ? Statement : Return,
			LintRule::UnreachableReturn,
			"Violation: Remove the unreachable trailing `return`",
			Out
		);
	}
	const NodeId Name = FunctionNameOf(Node);
	if(Function.IsMissingReturn) {
		Push(Name == NoNode ? Node : Name, LintRule::MissingReturn, "Violation: Function is missing an explicit trailing return", Out);
	} else if(Function.IsReturnUncertain) {
		Push(
			Name == NoNode ? Node : Name,
			LintRule::ReturnReachability,
			"Review: Determine whether the function needs an explicit trailing return; verify type, reachability",
			Out
		);
	}
	// 終了
	return;
}

/**
 * 部分木の JSX 要素包含判定関数
 * @param Node 対象ノード
 * @return JSX 子孫を含めば true
 */
bool LintAnalyzer::HasMarkupDescendant(const NodeId Node) {
	// JSX 要素子孫の有無の返戻
	return Core::AnyNodeOf(
		Syntax,
		Node,
		// JSX 要素との一致判定関数
		[this](const NodeId Cur) -> bool {
			// jsx_element / jsx_self_closing_element の何れかに合致するかの返戻
			return Syntax.Is(Cur, Role::TagElement);
		}
	);
}

/**
 * 関数命名規則違反検出関数
 * @param Source ソース文字列
 * @param Node 対象ノード
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckFunctionNamingConvention(const std::string &Source, const NodeId Node, std::vector<LintWarning> &Out) {
	// 命名規則を適用する関数名の抽出
	NodeId Identifier = NoNode;
	Core::ForEachNamedChildOf(
		Syntax,
		Node,
		[&](const NodeId Child) -> bool {
			// 単純な識別子で指定する関数名の照合
			if(Syntax.Is(Child, Role::FunctionNameIdentifier)) Identifier = Child;
			else if(Syntax.Is(Child, Role::FunctionDeclarator)) Identifier = Syntax.FirstNamedChildWithRole(Child, Role::Identifier);
			// 関数名識別子の取得済なら走査打切，未取得なら次の兄弟への返戻
			return Identifier == NoNode;
		}
	);
	if(Identifier == NoNode) Identifier = FunctionNameOf(Node);
	// 識別子が無い場合の終了
	if(Identifier == NoNode) return;
	// 関数名の字面取得
	const std::string_view Name = NodeView(Source, Identifier);
	// 名前が無いか外部契約で名前が決まる定義の場合の終了
	if(
		Name.empty() || std::ranges::find(Rules.FixedFunctionNames, Name) != Rules.FixedFunctionNames.end() ||
		!Rules.FixedFunctionPrefix.empty() && Name.starts_with(Rules.FixedFunctionPrefix)
	) return;
	// 外部契約で名前が決まる修飾子を持つ定義の場合の終了
	if(
		Core::AnyNodeOf(
			Syntax,
			Syntax.FirstNamedChildWithRole(Node, Role::ModifierSequence),
			[&](const NodeId Modifier) -> bool {
				return std::ranges::find(Rules.FixedNameModifiers, NodeView(Source, Modifier)) != Rules.FixedNameModifiers.end();
			}
		)
	) return;
	const bool HasInvalidCharacter = [this, Name, Identifier]() -> bool {
		// 演算子名を通常の関数名から除く事の返戻
		if(Syntax.Is(Identifier, Role::UserDefinedOperator) || Syntax.Is(Identifier, Role::OperatorName)) return false;
		for(size_t Index = 0; Index < Name.size(); ++Index) {
			const unsigned char Character = static_cast<unsigned char>(Name[Index]);
			if(
				Character >= '0' && Character <= '9' || Character >= 'A' && Character <= 'Z' || Character >= 'a' && Character <= 'z' ||
				Character == '_'
			) continue;
			if(
				Index == Name.size() - 1 && (
					!Rules.BooleanQuerySuffix.empty() && Name.ends_with(Rules.BooleanQuerySuffix) ||
					!Rules.DestructiveMethodSuffix.empty() && Name.ends_with(Rules.DestructiveMethodSuffix) ||
					!Rules.SetterNameSuffix.empty() && Name.ends_with(Rules.SetterNameSuffix)
				)
			) continue;
			// 先頭字が条件を満たす場合の返戻
			return true;
		}
		// 先頭字が条件を満たさない場合の返戻
		return false;
	}();
	if(HasInvalidCharacter) {
		Push(
			Identifier,
			LintRule::FunctionNameCharacters,
			"Review: Use an ASCII function name instead of " + std::string(Name) + "; verify external contract",
			Out
		);
	}
	// 命名形式別の違反検査
	const char FirstChar = Name[0];
	const bool HasUnderscore = Name.find('_') != std::string_view::npos;
	switch(Rules.FunctionCase) {
	case Core::NameCase::UpperInitial:
		if(FirstChar < 'A' || FirstChar > 'Z' || HasUnderscore) {
			Push(
				Identifier,
				LintRule::FunctionNameStyle,
				"Review: Use PascalCase for function " + std::string(Name) + "; verify external contract",
				Out
			);
		}
		break;
	case Core::NameCase::NoUppercase:
		if(
			HasUnderscore && (Name.front() == '_' || Name.back() == '_') || std::ranges::any_of(
				Name,
				[](const char Character) -> bool {
					return Character >= 'A' && Character <= 'Z';
				}
			)
		) {
			Push(
				Identifier,
				LintRule::FunctionNameStyle,
				"Review: Use snake_case for function " + std::string(Name) + "; verify external contract",
				Out
			);
		}
		break;
	case Core::NameCase::LowerInitial:
		if(HasMarkupDescendant(Node)) {
			if(FirstChar < 'A' || FirstChar > 'Z' || HasUnderscore) {
				Push(
					Identifier,
					LintRule::FunctionNameStyle,
					"Review: Use PascalCase for function " + std::string(Name) + "; verify external contract, intent",
					Out
				);
			}
		} else if(FirstChar < 'a' || FirstChar > 'z' || HasUnderscore) {
			Push(
				Identifier,
				LintRule::FunctionNameStyle,
				"Review: Use camelCase for function " + std::string(Name) + "; verify external contract",
				Out
			);
		}
		break;
	case Core::NameCase::Unrestricted:
		if(Rules.FunctionCaseByFirst && HasUnderscore) {
			Push(
				Identifier,
				LintRule::FunctionNameStyle,
				std::string("Review: Use ") + (FirstChar >= 'A' && FirstChar <= 'Z' ? "PascalCase" : "camelCase") + " for function " +
				std::string(Name) + "; verify external contract",
				Out
			);
		}
		break;
	}
	const auto IsBooleanType = [this, &Source](const NodeId Candidate) -> bool {
		return std::ranges::find(Rules.BooleanTypeNames, NodeView(Source, Candidate)) != Rules.BooleanTypeNames.end();
	};
	const NodeId ReturnType = ExplicitReturnTypeOf(Node);
	// 複合型の内側に在る bool は関数の戻値型でない為，戻値型の節点自体だけを照合
	bool ReturnsBoolean = ReturnType != NoNode && IsBooleanType(ReturnType);
	if(ReturnType == NoNode) for(const NodeId Child : Syntax.ChildrenOf(Node)) {
		if(BeginOf(Child) >= BeginOf(Identifier)) break;
		ReturnsBoolean = ReturnsBoolean || Syntax.Is(Child, Role::TypeNameNode) && IsBooleanType(Child);
	}
	if(!ReturnsBoolean && !Rules.BooleanQuerySuffix.empty()) {
		const NodeId Body = Syntax.FieldChild(Node, ChildRole::Body);
		NodeId Value = NoNode;
		if(Body != NoNode) {
			Core::ForEachNamedChildOf(
				Syntax,
				Body,
				[this, &Value, Body](const NodeId Child) -> void {
					if(Syntax.Is(Child, Role::Comment)) return;
					Value = Value == NoNode ? Child : Body;
				}
			);
		}
		if(Value != NoNode && Core::ControlFlow::IsReturn(Syntax, Value)) Value = Syntax.NamedChild(Value, 0);
		if(
			Value != NoNode && Syntax.NamedChildCount(Value) == 1 && !Syntax.IsWord(Value, Word::True) && !Syntax.IsWord(Value, Word::False)
		) Value = Syntax.NamedChild(Value, 0);
		ReturnsBoolean = Value != NoNode && Value != Body && (Syntax.IsWord(Value, Word::True) || Syntax.IsWord(Value, Word::False));
	}
	const bool HasBooleanQueryName = Rules.BooleanQuerySuffix.empty() ?
	std::ranges::any_of(
		Rules.BooleanQueryPrefixes,
		[Name](const std::string_view Prefix) -> bool {
			return Name.starts_with(Prefix);
		}
	) :
	Name.ends_with(Rules.BooleanQuerySuffix);
	if(ReturnsBoolean && !HasBooleanQueryName) {
		Push(
			Identifier,
			LintRule::FunctionNameRole,
			"Review: Use boolean-query naming for function " + std::string(Name) + "; verify external contract, intent",
			Out
		);
	}
	if(Syntax.Is(Syntax.ParentOf(Node), Role::ClassBody)) {
		const NodeId Body = Syntax.FieldChild(Node, ChildRole::Body);
		std::vector<NodeId> Statements;
		if(Body != NoNode) {
			Core::ForEachNamedChildOf(
				Syntax,
				Body,
				[this, &Statements](const NodeId Child) -> void {
					if(!Syntax.Is(Child, Role::Comment)) Statements.push_back(Child);
				}
			);
		}
		const size_t StatementCount = Statements.size();
		NodeId Statement = StatementCount ? Statements[0] : NoNode;
		if(
			StatementCount > 1 &&
			(StatementCount != 2 || !Core::ControlFlow::IsReturn(Syntax, Statements[1]) || Syntax.NamedChildCount(Statements[1]))
		) Statement = NoNode;
		const auto IsSelfMember = [this](const NodeId Expression) -> bool {
			return Expression != NoNode && Syntax.Is(Expression, Role::Member) &&
			Syntax.Is(Syntax.NamedChild(Expression, 0), Role::ThisReference);
		};
		NodeId Parameters = NoNode;
		WalkChildrenCursor(
			Node,
			[this, &Parameters](const NodeId Child) -> bool {
				// 仮引数を取得済又は本体に達した場合の探索打切の返戻
				if(Parameters != NoNode || Syntax.Is(Child, Role::FunctionBodyContainer)) return false;
				if(Syntax.Is(Child, Role::ParameterContainer)) Parameters = Child;
				// 仮引数が見付からない場合の探索継続の返戻
				return Parameters == NoNode;
			}
		);
		const uint32_t ParameterCount = Parameters == NoNode ? 0 : Syntax.NamedChildCount(Parameters);
		if(
			Statement != NoNode && !ReturnsBoolean && !ParameterCount && Core::ControlFlow::IsReturn(Syntax, Statement) &&
			IsSelfMember(Syntax.NamedChild(Statement, 0)) && !std::ranges::any_of(
				Rules.GetterPrefixes,
				[Name](const std::string_view Prefix) -> bool {
					return Name.starts_with(Prefix);
				}
			)
		) {
			Push(
				Identifier,
				LintRule::FunctionNameRole,
				"Review: Use getter naming for function " + std::string(Name) + "; verify external contract",
				Out
			);
		}
		const NodeId Assignment = Statement != NoNode ? Syntax.NamedChild(Statement, 0) : NoNode;
		if(
			Statement != NoNode && Syntax.Is(Statement, Role::ExpressionStatement) && ParameterCount == 1 &&
			Syntax.Is(Assignment, Role::AssignmentOperation) && Syntax.HasTokenChild(Assignment, Rules.Operators.Assignment) &&
			IsSelfMember(Syntax.NamedChild(Assignment, 0)) &&
			ParameterNameOf(Node, NodeView(Source, Syntax.NamedChild(Assignment, 1))) != NoNode && !std::ranges::any_of(
				Rules.SetterPrefixes,
				[Name](const std::string_view Prefix) -> bool {
					return Name.starts_with(Prefix);
				}
			)
		) {
			Push(
				Identifier,
				LintRule::FunctionNameRole,
				"Review: Use setter naming for function " + std::string(Name) + "; verify external contract",
				Out
			);
		}
	}
	if(!Rules.DestructiveMethodSuffix.empty() && Name.ends_with(Rules.DestructiveMethodSuffix)) {
		const NodeId Body = Syntax.FieldChild(Node, ChildRole::Body);
		bool MutatesReceiver = false;
		WalkChildrenCursor(
			Body,
			[this, &Source, &MutatesReceiver](const NodeId Child) -> bool {
				// 受手の変更を検出済又は別の関数に達した場合の探索打切の返戻
				if(MutatesReceiver || Syntax.Is(Child, Role::FunctionScope)) return false;
				MutatesReceiver = (Syntax.Is(Child, Role::AssignmentOperation) || Syntax.Is(Child, Role::IncrementInspection)) &&
				NodeView(Source, Syntax.NamedChild(Child, 0)).starts_with(Rules.InstanceStatePrefix);
				// 受手の変更を未検出の場合の探索継続の返戻
				return !MutatesReceiver;
			}
		);
		const std::string_view BaseName = Name.substr(0, Name.size() - Rules.DestructiveMethodSuffix.size());
		bool HasPair = false;
		Core::ForEachNamedChildOf(
			Syntax,
			Syntax.ParentOf(Node),
			[this, &Source, Node, BaseName, &HasPair](const NodeId Sibling) -> void {
				// 自身又は名前付関数でない兄弟を除く場合の終了
				if(Sibling == Node || !Syntax.Is(Sibling, Role::NamedFunctionDefinition)) return;
				const NodeId PairName = FunctionNameOf(Sibling);
				HasPair = HasPair || PairName != NoNode && NodeView(Source, PairName) == BaseName;
			}
		);
		if(MutatesReceiver && !HasPair) {
			Push(
				Identifier,
				LintRule::FunctionNameRole,
				"Review: Use paired destructive-method naming for function " + std::string(Name) + "; verify external contract",
				Out
			);
		}
	}
	// 終了
	return;
}

/**
 * 関数戻値型の不在検出関数
 * @param Node 対象ノード
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckReturnTypeHint(const NodeId Node, std::vector<LintWarning> &Out) {
	// 対象の定義は文法の役割で選択済
	if(HasError(Node)) return;
	// 戻値型節点の確認
	if(ExplicitReturnTypeOf(Node) == NoNode) {
		const NodeId Name = FunctionNameOf(Node);
		Push(
			Name == NoNode ? Node : Name,
			LintRule::ReturnType,
			Rules.TypeAnnotationObserved ?
			"Review: Add an explicit return type to this function; verify type, evaluation, external contract" :
			"Review: Add an explicit return type to this function; verify type, external contract",
			Out
		);
	}
	// 終了
	return;
}

/**
 * 型を明示出来る宣言子の検査関数
 * @param Node 対象の宣言子
 * @param Out 警告の格納先
 */
void LintAnalyzer::CheckDeclarationType(const NodeId Node, std::vector<LintWarning> &Out) {
	const NodeId Name = Syntax.FieldChild(Node, ChildRole::Name);
	// 型を明記済か，分解等で名前が１つの識別子でない宣言子か，関数の値で型を関数の注釈が表す宣言子の場合の終了
	if(
		HasError(Node) || Syntax.FirstNamedChildWithRole(Node, Role::TypeAnnotation) != NoNode || !Syntax.Is(Name, Role::Identifier) ||
		Syntax.Is(Syntax.FieldChild(Node, ChildRole::Value), Role::FunctionScope)
	) return;
	Push(Name, LintRule::DeclarationType, "Review: Add an explicit type to this declaration; verify type, external contract", Out);
	// 終了
	return;
}

/**
 * 型を明示出来る仮引数の検査関数
 * @param Node 対象の仮引数
 * @param Out 警告の格納先
 */
void LintAnalyzer::CheckParameterType(const NodeId Node, std::vector<LintWarning> &Out) {
	const NodeId Parent = Syntax.ParentOf(Node);
	// 定義を包む構文（注釈等）を除いた関数の親
	NodeId Owner = Syntax.ParentOf(Syntax.ParentOf(Parent));
	while(Syntax.Is(Owner, Role::DocWrapper)) Owner = Syntax.ParentOf(Owner);
	// 仮引数の並びの中の型を推論する位置か型を明記済の仮引数の場合の終了
	if(
		HasError(Node) || !Syntax.Is(Parent, Role::ParameterContainer) || Syntax.Is(Node, Role::AutoParameterHost) ||
		Syntax.FirstNamedChildWithRole(Node, Role::TypeAnnotation) != NoNode
	) return;
	// 署名順の仮引数の序数（１始まり，コメントと区切の字句を数えない）
	uint32_t Ordinal = 1;
	for(const NodeId Sibling : Syntax.ChildrenOf(Parent)) {
		if(Sibling == Node) break;
		if(Syntax.IsNamed(Sibling) && !Syntax.Is(Sibling, Role::Comment)) ++Ordinal;
	}
	// 型の成員の最初の仮引数で受け手を受ける言語の受け手の場合の終了
	if(Rules.HasReceiverParameter && Ordinal == 1 && Syntax.Is(Syntax.ParentOf(Owner), Role::ClassLike)) return;
	const NodeId Name = Syntax.Is(Node, Role::ParameterName) ? Node : Syntax.FirstNamedChildWithRole(Node, Role::ParameterName);
	Push(
		Name == NoNode ? Node : Name,
		LintRule::ParameterType,
		"Review: Add an explicit type to " +
		(Name == NoNode ? "parameter #" + std::to_string(Ordinal) : "parameter '" + std::string(NodeView(Source, Name)) + "'") +
		(Rules.TypeAnnotationObserved ? "; verify type, evaluation, external contract" : "; verify type, external contract"),
		Out
	);
	// 終了
	return;
}

/**
 * 錨の手前に続く独立したコメントの本文の収集関数
 * 文書コメントの有無と完全性の双方が同じ並びを見る為，走査を此処だけで定める
 * 計算量：コメントの数 K と遡るコメントの数 M に対し O(log K + M)
 * @param Source ソース文字列
 * @param Anchor 錨の節点
 * @param DirectiveStarts 標準の指示コメントの始まり（昇順）
 * @return 空白だけを挟んで錨の手前に続き，前のコードの行末コメントより後の，指示コメント（Rust の文書を除く）を除いたコメントの本文（原文の順）
 */
std::vector<std::string_view> LintAnalyzer::LeadingCommentTexts(
	const std::string &Source,
	const NodeId Anchor,
	const std::vector<uint32_t> &DirectiveStarts
) {
	std::vector<std::string_view> Texts;
	const std::vector<StaticLint::CommentFacts> &Comments = Facts.Comments;
	// 錨より前に始まるコメントを後ろから遡る（間に空白以外の字が在れば並びを終える）
	uint32_t Boundary = BeginOf(Anchor);
	for(
		size_t Index = static_cast<size_t>(
			std::lower_bound(
				Comments.begin(),
				Comments.end(),
				Boundary,
				[](const StaticLint::CommentFacts &Comment, const uint32_t Position) -> bool {
					// 始まりとの比較の返戻
					return Comment.Range.Start < Position;
				}
			) - Comments.begin()
		);
		Index--;
	) {
		const StaticLint::CommentFacts &Comment = Comments[Index];
		const uint32_t Start = Comment.Range.Start, End = Comment.Range.End;
		if(End > Boundary || Source.find_first_not_of(" \t\r\n", End) < Boundary) break;
		Boundary = Start;
		// 前のコードの行末コメントで並びを終える
		if(Source.find_first_not_of(" \t", Start ? Source.find_last_of('\n', Start - 1) + 1 : 0) < Start) break;
		// 指示コメント（文書其の物が指示コメントに為る Rust の文書を除く）の読飛
		if(std::binary_search(DirectiveStarts.begin(), DirectiveStarts.end(), Start) && !Comment.IsDocumentAttribute) continue;
		Texts.push_back(std::string_view(Source).substr(Start, End - Start));
	}
	std::reverse(Texts.begin(), Texts.end());
	// 原文の順に並べたコメントの本文の返戻
	return Texts;
}

/**
 * Python の文書文字列に為る文かの判定関数
 * @param Source ソース文字列
 * @param Statement 本体の最初の文
 * @return f 文字列・バイト列を含まない文字列（暗黙の連結を含む）なら true
 */
bool LintAnalyzer::IsDocumentationLiteral(const std::string &Source, const NodeId Statement) {
	// 接頭辞に f・b を持たない文字列かの判定
	const auto IsPlainString = [this, &Source](const NodeId Text) -> bool {
		// 文字列で無い節点の返戻
		if(!Syntax.IsNamed(Text) || !Syntax.Is(Text, Role::QuotedString)) return false;
		const std::string_view Start = NodeView(Source, Text);
		// 引用符の前の接頭辞に f・b を持たない事の返戻
		// 文法が文書文字列から除く接頭辞を持たない事の返戻
		return std::ranges::none_of(
			Start.substr(0, Start.find_first_of("\"'")),
			[this](const unsigned char Letter) -> bool {
				// 大文字の接頭辞も同じ意味として判定した結果の返戻
				return Rules.NonDocumentQuotePrefixes.find(static_cast<char>(Letter | 0X20)) != std::string_view::npos;
			}
		);
	};
	// 連結でない文字列の返戻
	if(!Syntax.Is(Statement, Role::ConcatenatedLiteral)) return IsPlainString(Statement);
	bool IsPlain = true;
	Core::ForEachNamedChildOf(
		Syntax,
		Statement,
		[&](const NodeId Part) -> void {
			IsPlain = IsPlain && IsPlainString(Part);
		}
	);
	// 連結した全ての文字列が文書文字列に為るかの返戻
	return IsPlain;
}

/**
 * 関数ドキュメントコメント不在検出関数
 * @param Source ソース文字列
 * @param Node 文書を付ける定義
 * @param Leading 錨の手前に続く独立したコメントの本文（文書文字列の言語では空）
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckDocCommentPresence(
	const std::string &Source,
	const NodeId Node,
	const std::span<const std::string_view> Leading,
	std::vector<LintWarning> &Out
) {
	// 文書化コメントの保持有無
	bool HasDoc = false;
	if(Rules.Notation.Documentation == Core::DocumentationStyle::Docstring) {
		// Python は前置コメントではなく関数本体先頭のドキュメント文字列をドキュメントコメントと認定
		if(const NodeId Body = Syntax.FieldChild(Node, ChildRole::Body); Body != NoNode) {
			// 本体先頭の文節点
			const NodeId FirstStmt = Syntax.NamedChild(Body, 0);
			HasDoc = FirstStmt != NoNode && IsDocumentationLiteral(Source, FirstStmt);
			// 引用符の内側が空白だけの文書文字列は説明を持たない（空の項目を充足としない）
			if(HasDoc) {
				std::string_view Content = NodeView(Source, FirstStmt);
				Content.remove_prefix(std::min(Content.find_first_of("\"'"), Content.size()));
				if(Content.find_first_not_of("\"' \t\r\n") == std::string_view::npos) {
					Push(FirstStmt, LintRule::DocumentationSummary, "Violation: Documentation is missing a summary", Out);
				}
			}
		}
	} else {
		// Rust の文書属性と，先行するコメントの並びの文書化コメント（普通のコメントが挟まっても見落とさない）
		HasDoc = Facts.Functions.at(Node).HasDocumentAttribute;
		for(const std::string_view Text : Leading) HasDoc = HasDoc || DocCommentSynchronizer::IsDocComment(Rules.Notation, Text);
	}
	if(!HasDoc) {
		const NodeId Name = FunctionNameOf(Node);
		Push(
			Name == NoNode ? Node : Name,
			LintRule::MissingDocumentation,
			"Violation: Function is missing a documentation comment",
			Out
		);
	}
	// 終了
	return;
}

/**
 * 捕捉の節が送出を確かに受け止めるかの判定関数
 * 型を挙げない節，全ての例外の基底を挙げた節，送出する型の名前を挙げた節を，確かに受け止める物とする
 * 計算量：節の見出の長さ L に対し O(L)
 * @param Source ソース文字列
 * @param Handler 捕捉の節（Ruby は `rescue`）
 * @param Thrown 送出する型の最後の名前（決まらなければ空）
 * @return 確かに受け止めれば true
 */
bool LintAnalyzer::CatchesThrow(const std::string &Source, const NodeId Handler, const std::string_view Thrown) {
	// 型を持たない JavaScript / TypeScript の捕捉は全てを受ける事の返戻
	if(Rules.CatchAllBindings) return true;
	std::string_view Header = NodeView(Source, Handler);
	if(Rules.CatchExceptionList) {
		const NodeId Exceptions = Syntax.FirstNamedChildWithRole(Handler, Role::ExceptionTypes);
		// 型を挙げない `rescue` は利用者の送出の既定の StandardError を受ける事の返戻
		if(Exceptions == NoNode) return true;
		Header = NodeView(Source, Exceptions);
	} else Header = Header.substr(0, Header.find('{'));
	// 見出の型・束縛の名前（捕捉の語と修飾の語を除く）と，Swift の束縛の有無
	std::vector<std::string_view> Names;
	bool HasBinding = false;
	for(size_t Pos = 0; Pos < Header.size();) {
		if(!std::isalpha(static_cast<unsigned char>(Header[Pos])) && Header[Pos] != '_') {
			++Pos;
			continue;
		}
		size_t End = Pos;
		while(End < Header.size() && (std::isalnum(static_cast<unsigned char>(Header[End])) || Header[End] == '_')) ++End;
		const std::string_view Candidate = Header.substr(Pos, End - Pos);
		Pos = End;
		// C# の例外の絞込は条件次第で受け止めない事の返戻
		if(Rules.CatchFilter && Syntax.Rules().IsSpelling(Candidate, Word::CatchFilter)) return false;
		// 型・束縛の名前でない語の読飛
		if(Syntax.Rules().IsSpelling(Candidate, Word::CatchHead) || Syntax.Rules().IsSpelling(Candidate, Word::CatchQualifier)) {
			continue;
		}
		if(Syntax.Rules().IsSpelling(Candidate, Word::CatchBinding)) {
			HasBinding = true;
			continue;
		}
		// 送出する型か全ての例外の基底を挙げた節の返戻
		if(!Thrown.empty() && Candidate == Thrown || Syntax.Rules().IsSpelling(Candidate, Word::UniversalException)) return true;
		Names.push_back(Candidate);
	}
	// Swift の束縛だけの `catch let e` は全てを受ける事の返戻
	if(Rules.CatchBindingPattern && HasBinding) return Names.size() == 1 && Header.find('.') == std::string_view::npos;
	// 型を挙げない節（C++ の `...`，C# と Swift の素の `catch`）かの返戻
	return Names.empty();
}

/**
 * 呼出側へ伝わる失敗の判定関数（整形仕様 8.4 の文書の不足で文書の失敗条件の項目と照合する）
 * 入れ子の関数・ラムダ・クラスの送出は其の呼出側の物として除き，例外処理の本体の中の送出は確かに受け止める節が無ければ要確認とする
 * 計算量：本体の節点数 N と入れ子の深さ D に対し O(N D)
 * @param Source ソース文字列
 * @param Node 関数の定義
 * @return 呼出側へ伝わる失敗
 */
LintAnalyzer::FailureFlow LintAnalyzer::FailureFlowOf(const std::string &Source, const NodeId Node) {
	FailureFlow Flow;
	// 署名が宣言した送出（Java・Swift の `throws`，Kotlin の `@Throws`）
	Core::ForEachNamedChildOf(
		Syntax,
		Node,
		[&](const NodeId Child) -> void {
			const std::string_view Text = NodeView(Source, Child);
			if(Syntax.Is(Child, Role::ThrowsDeclaration)) {
				Flow.IsThrown = Flow.IsThrown || Text.starts_with(Syntax.Rules().Spelling(Word::ThrowsMarker));
			} else if(Syntax.Is(Child, Role::FailureAnnotation)) {
				const std::string_view Annotation = Syntax.Rules().Spelling(Word::ThrowsAnnotation);
				for(size_t Pos = Text.find(Annotation); Pos != std::string_view::npos; Pos = Text.find(Annotation, Pos + 1)) {
					// 注釈の名前の始まり（修飾した名前の最後の名前を含み，名前の途中を除く）
					Flow.IsThrown = Flow.IsThrown || Pos && !IsIdentifierChar(Text[Pos - 1]);
				}
			}
		}
	);
	// Rust の `Result` の戻値（パスの最後の名前で見る）
	if(
		const NodeId Return = Syntax.FieldChild(Node, ChildRole::ReturnType);
		Syntax.Is(Node, Role::ErrorResult) && Return != NoNode
	) {
		std::string_view Result = NodeView(Source, Return);
		Result = Result.substr(0, Result.find('<'));
		Flow.IsError = Syntax.Rules().IsSpelling(Result.substr(Result.rfind(':') + 1), Word::ErrorResultType);
	}
	const NodeId Body = Syntax.FieldChild(Node, ChildRole::Body);
	WalkChildrenCursor(
		Body == NoNode ? Node : Body,
		[&](const NodeId Child, const std::span<const NodeId> Ancestors) -> bool {
			// 入れ子の関数・ラムダ・クラスの送出は其の呼出側へ伝わる為，内側へ降りない事の返戻
			if(Syntax.Is(Child, Role::FlowBoundary)) return false;
			if(Syntax.Is(Child, Role::TerminatingMacroCall)) {
				const NodeId Macro = Syntax.FieldChild(Child, ChildRole::Macro);
				std::string_view Name = Macro == NoNode ? std::string_view() : NodeView(Source, Macro);
				Name = Name.substr(Name.rfind(':') + 1);
				// 呼出側の誤りで中断するマクロ
				Flow.IsPanic = Flow.IsPanic || Syntax.Rules().IsSpelling(Name, Word::PanicMacro);
				// マクロの中を辿り続ける事の返戻
				return true;
			}
			// 送出（Kotlin の `throw` の跳躍式，Swift の `throw` の制御移譲文，Ruby の受け手の無い `raise` / `fail` を含む）と送出する値
			bool IsThrow = Syntax.Is(Child, Role::ThrowStatement) || Syntax.Is(Child, Role::ThrowValue) ||
			Syntax.Is(Child, Role::JumpExpression) && NodeView(Source, Child).starts_with(Syntax.Rules().Spelling(Word::ThrowHead));
			NodeId Value = IsThrow ? Syntax.NamedChild(Child, 0) : NoNode;
			if(
				Syntax.Is(Child, Role::ControlTransfer) && Syntax.NamedChildCount(Child) &&
				Syntax.Is(Syntax.NamedChild(Child, 0), Role::ThrowKeyword)
			) {
				IsThrow = true;
				Value = Syntax.NamedChild(Child, 1);
			} else if(Syntax.Is(Child, Role::ImplicitRaiseName) && !Ancestors.empty()) {
				const std::string_view Name = NodeView(Source, Child);
				if(const NodeId Parent = Ancestors.back(); !Syntax.Is(Parent, Role::ReceiverCall)) {
					IsThrow = Syntax.Rules().IsSpelling(Name, Word::ImplicitRaise) && !Syntax.Is(Parent, Role::InstanceMethodDefinition) &&
					!Syntax.Is(Parent, Role::SingletonMethod);
				} else if(
					Syntax.Rules().IsSpelling(Name, Word::ImplicitRaise) && Syntax.FieldChild(Parent, ChildRole::Function) == Child &&
					Syntax.FieldChild(Parent, ChildRole::Object) == NoNode
				) {
					IsThrow = true;
					Value = Syntax.NamedChild(Syntax.FieldChild(Parent, ChildRole::Arguments), 0);
				}
			}
			// 送出でない節点を辿り続ける事の返戻
			if(!IsThrow) return true;
			// 送出する型の最後の名前（`new E(…)` / `E(…)` / `E.new(…)` / `raise E, …` から）
			std::string_view Thrown = Value == NoNode ? std::string_view() : NodeView(Source, Value);
			const std::string_view Constructor = Syntax.Rules().Spelling(Word::ConstructionMethod);
			if(Thrown.starts_with(Constructor) && Thrown.size() > Constructor.size() && Thrown[Constructor.size()] == ' ') {
				Thrown.remove_prefix(Constructor.size() + 1);
			}
			Thrown = Thrown.substr(0, Thrown.find_first_of("(<{ \t\r\n,"));
			if(
				Thrown.size() > Constructor.size() && Thrown[Thrown.size() - Constructor.size() - 1] == '.' &&
				Syntax.Rules().IsSpelling(Thrown.substr(Thrown.size() - Constructor.size()), Word::ConstructionMethod)
			) Thrown.remove_suffix(Constructor.size() + 1);
			Thrown = Thrown.substr(Thrown.find_last_of(".:\\") + 1);
			// 内側の例外処理から順に，送出を受け止め得る本体の中かを確かめる
			bool IsCaught = false, IsUnsure = false;
			for(size_t Index = Ancestors.size(); Index-- && !IsCaught && !IsUnsure;) {
				const NodeId Up = Ancestors[Index], Path = Index + 1 < Ancestors.size() ? Ancestors[Index + 1] : Child;
				// 捕捉・後始末・例外の無い時の節の送出は外側へ伝わる
				if(Syntax.Is(Path, Role::ExceptionContinuation)) continue;
				// Ruby の後置の `rescue` は本体の送出を StandardError として受ける
				if(Syntax.Is(Up, Role::RescueExpression)) {
					IsCaught = Path == Syntax.NamedChild(Up, 0);
					continue;
				}
				if(!Syntax.Is(Up, Role::ExceptionBody)) continue;
				std::vector<NodeId> Handlers;
				Core::ForEachNamedChildOf(
					Syntax,
					Up,
					[&](const NodeId Clause) -> void {
						if(Syntax.Is(Clause, Role::ExceptionHandler)) Handlers.push_back(Clause);
					}
				);
				// 捕捉の節の無い例外処理（後始末だけの物）は外側へ伝える
				if(Handlers.empty()) continue;
				IsCaught = std::any_of(
					Handlers.begin(),
					Handlers.end(),
					[&](const NodeId Handler) -> bool {
						// 送出を確かに受け止める節かの返戻
						return CatchesThrow(Source, Handler, Thrown);
					}
				);
				IsUnsure = !IsCaught;
			}
			Flow.IsThrown = Flow.IsThrown || !IsCaught && !IsUnsure;
			Flow.IsUnsure = Flow.IsUnsure || IsUnsure;
			// 送出の値の中を辿り続ける事の返戻
			return true;
		}
	);
	// 呼出側へ伝わる失敗の返戻
	return Flow;
}

/**
 * ドキュメントコメントの項目と説明の不足の検出関数
 * 同期しない文書（Rust の文書，順を保てない行形式の文書等）の項目の不足・余分も，署名と照合して報告する
 * @param Src 構文木保持元
 * @param Node 対象ノード
 * @param Texts 錨の手前に続く独立したコメントの本文（原文の順，一本でも挟まると直前の兄弟が文書で無く為る為，並び全体を見る）
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckDocCommentCompleteness(
	const std::string &Src,
	const NodeId Node,
	const std::span<const std::string_view> Texts,
	std::vector<LintWarning> &Out
) {
	// 自動付与の雛形は説明文が空の為，概要・仮引数・戻値の説明記入を要求
	const Core::DocumentationStyle Dialect = Rules.Notation.Documentation;
	if(Core::VocabularyOf(Dialect, Core::DocDescription::Summary).Policy == Core::DocStylePolicy::SourceOwned || HasError(Node)) {
		// 見出書式の定まらない文書文字列と解析失敗の定義の場合の終了
		return;
	}
	const std::string &Source = Src;
	std::vector<std::string> Lines;
	OutputPoint DocumentPoint = PointOf(Node);
	size_t DocumentIndex = Facts.Comments.size();
	// 文書の有無（空の骨格の囲みの文書は論理行を持たない）
	bool HasDocument = false;
	if(const size_t DocIdx = DocCommentSynchronizer::LastDocBlockIndex(Rules.Notation, Texts); DocIdx < Texts.size()) {
		Lines = SourceLogicalLines(Texts[DocIdx]);
		DocumentPoint = CommentInfo(Texts[DocIdx]).Range.Point;
		DocumentIndex = CommentInfo(Texts[DocIdx]).Record;
		HasDocument = true;
	} else for(const std::string_view Text : Texts) {
		// 行形式の文書の行
		if(DocCommentSynchronizer::DocLineMarker(Rules.Notation, Text).empty()) continue;
		if(!HasDocument) {
			DocumentPoint = CommentInfo(Text).Range.Point;
			DocumentIndex = CommentInfo(Text).Record;
		}
		const std::vector<std::string> DocLines = SourceLogicalLines(Text);
		Lines.insert(Lines.end(), DocLines.begin(), DocLines.end());
		HasDocument = true;
	}
	// 文書の無い定義の返戻
	if(!HasDocument) return;
	const Core::Signature &Sig = Facts.Functions.at(Node).Signature;
	const std::optional<std::vector<DocItem>> Items = DocCommentSynchronizer::ParseItems(Rules.Notation, Lines, Sig);
	// 同期出来ない文書（未閉鎖のコード区画，継承を宣言した文書）の返戻
	if(!Items) return;
	bool HasScaleLoop = false;
	WalkChildrenCursor(
		Syntax.FieldChild(Node, ChildRole::Body),
		[this, &Source, &Sig, &HasScaleLoop](const NodeId Child) -> bool {
			// 規模に依る繰返を検出済又は別の関数に達した場合の探索打切の返戻
			if(HasScaleLoop || Syntax.Is(Child, Role::FunctionScope)) return false;
			// 繰返でない子を読み飛ばす事の返戻
			if(!Syntax.Is(Child, Role::RepeatedControl)) return true;
			const NodeId Condition = Syntax.FieldChild(Child, ChildRole::Condition);
			HasScaleLoop = Condition != NoNode && Core::AnyNodeOf(
				Syntax,
				Condition,
				[this, &Source, &Sig](const NodeId Leaf) -> bool {
					// 識別子でない葉を除く事の返戻
					if(!Syntax.Is(Leaf, Role::Identifier)) return false;
					const std::string_view Name = NodeView(Source, Leaf);
					// 仮引数と一致する識別子の有無の返戻
					return std::ranges::any_of(
						Sig.Params,
						[Name](const std::string &Param) -> bool {
							return Param == Name;
						}
					);
				}
			);
			// 規模に依る繰返を未検出の場合の探索継続の返戻
			return !HasScaleLoop;
		}
	);
	// 利用条件又は計算量の説明が不足する文書の候補
	bool ReportedConstraint = false, ReportedComplexity = false;
	for(size_t Index = 0; Index < Lines.size(); ++Index) {
		std::string_view Line = Lines[Index];
		Line.remove_prefix(std::min(Line.find_first_not_of(" \t"), Line.size()));
		const auto MatchHeading = [Line](const std::vector<std::string_view> &Headings) -> std::string_view {
			for(const std::string_view Heading : Headings) if(Line.starts_with(Heading)) return Heading;
			return {};
		};
		const std::string_view Constraint = MatchHeading(Rules.DocumentationConstraintHeadings);
		const std::string_view Complexity = MatchHeading(Rules.DocumentationComplexityHeadings);
		const bool IsConstraint = !Constraint.empty(), IsComplexity = !Complexity.empty();
		if(!IsConstraint && !IsComplexity) continue;
		if(IsConstraint && ReportedConstraint || IsComplexity && ReportedComplexity) continue;
		std::string_view Description = Line.substr(IsConstraint ? Constraint.size() : Complexity.size());
		Description.remove_prefix(std::min(Description.find_first_not_of(" \t"), Description.size()));
		while(!Description.empty() && (Description.back() == ' ' || Description.back() == '\t')) Description.remove_suffix(1);
		std::string_view Next;
		for(size_t Following = Index + 1; Following < Lines.size(); ++Following) {
			Next = Lines[Following];
			Next.remove_prefix(std::min(Next.find_first_not_of(" \t"), Next.size()));
			if(!Next.empty()) break;
		}
		const bool StartsNextItem =
		Next.empty() || Next.starts_with('@') || Next.starts_with('#') || Next.starts_with('<') || std::ranges::any_of(
			Rules.DocumentationConstraintHeadings,
			[Next](const std::string_view Heading) -> bool {
				return Next.starts_with(Heading);
			}
		) || std::ranges::any_of(
			Rules.DocumentationComplexityHeadings,
			[Next](const std::string_view Heading) -> bool {
				return Next.starts_with(Heading);
			}
		);
		if(!StartsNextItem) continue;
		if(!Description.empty()) {
			if(IsComplexity) {
				const size_t StartNotation = Description.find("O(");
				const size_t EndNotation =
				StartNotation == std::string_view::npos ? std::string_view::npos : Description.find(')', StartNotation + 2);
				const bool HasMeasureBefore =
				StartNotation != std::string_view::npos && Description.substr(0, StartNotation).find("に対し") != std::string_view::npos;
				const bool HasMeasureAfter =
				EndNotation != std::string_view::npos && Description.find_first_not_of(" \t", EndNotation + 1) != std::string_view::npos;
				const bool IsConstant = Description.find("O(1)") != std::string_view::npos;
				if(EndNotation == std::string_view::npos || !HasMeasureBefore && !HasMeasureAfter && !IsConstant) {
					ReportedComplexity = true;
					Push(
						DocumentPoint,
						LintRule::DocumentationComplexity,
						HasScaleLoop ?
						"Violation: Document the complexity and its input-size measure" :
						"Review: Document the complexity and its input-size measure; verify applicability",
						Out
					);
				}
			}
			continue;
		}
		if(IsConstraint) ReportedConstraint = true;
		else ReportedComplexity = true;
		Push(
			DocumentPoint,
			IsConstraint ? LintRule::DocumentationConstraint : LintRule::DocumentationComplexity,
			IsConstraint ? "Review: Document the required usage constraints; verify applicability" : HasScaleLoop ?
			"Violation: Document the complexity and its input-size measure" :
			"Review: Document the complexity and its input-size measure; verify applicability",
			Out
		);
	}
	if(Dialect == Core::DocumentationStyle::Prose && !Sig.Name.empty() && DocumentIndex < Facts.Comments.size()) {
		const std::string_view Body = Repository.GetRecords()[DocumentIndex].Text;
		const size_t First = Body.find_first_not_of(" \t\r\n");
		if(
			First != std::string_view::npos && (
				Body.compare(First, Sig.Name.size(), Sig.Name) ||
				First + Sig.Name.size() < Body.size() && IsIdentifierChar(Body[First + Sig.Name.size()])
			)
		) {
			Push(
				PointInComment(DocumentIndex, Body, First),
				LintRule::DocumentationName,
				"Violation: Start the documentation with " + Sig.Name,
				Out
			);
		}
	}
	if(DocumentIndex < Facts.Comments.size()) {
		const std::string_view Body = Repository.GetRecords()[DocumentIndex].Text;
		size_t Search = 0;
		for(const DocItem &Item : *Items) {
			if(Item.Begin >= Lines.size()) continue;
			std::string_view Line = Lines[Item.Begin];
			Line.remove_prefix(std::min(Line.find_first_not_of(" \t"), Line.size()));
			for(const Core::DocumentTagSynonym &Synonym : Rules.Notation.DocumentTagSynonyms) {
				if(!Line.starts_with(Synonym.From) || Line.size() > Synonym.From.size() && IsIdentifierChar(Line[Synonym.From.size()])) {
					continue;
				}
				const size_t Found = Body.find(Synonym.From, Search);
				if(Found != std::string_view::npos) {
					Push(
						PointInComment(DocumentIndex, Body, Found),
						LintRule::DocumentationItemForm,
						"Violation: Use " + std::string(Synonym.To) + " for this documentation item",
						Out
					);
					Search = Found + Synonym.From.size();
				}
				break;
			}
		}
	}
	for(const DocItem &Item : *Items) {
		if(
			Item.Kind != DocItemKind::Param && Item.Kind != DocItemKind::Return ||
			DocCommentSynchronizer::HasItemDescription(Rules.Notation, Item, Lines)
		) continue;
		if(Item.Kind == DocItemKind::Param && DocCommentSynchronizer::CoversAnyParam(Item.Name, Sig)) {
			const NodeId Parameter = ParameterNameOf(Node, Item.Name);
			Push(
				Parameter == NoNode ? PointOf(Node) : PointOf(Parameter),
				LintRule::DocumentationParameter,
				"Violation: Documentation is missing a description for parameter '" + std::string(Item.Name) + "'",
				Out
			);
		} else if(Item.Kind == DocItemKind::Return && Sig.HasReturn) {
			const NodeId ReturnType = ExplicitReturnTypeOf(Node);
			const NodeId Name = FunctionNameOf(Node);
			Push(
				ReturnType != NoNode ? ReturnType : Name == NoNode ? Node : Name,
				LintRule::DocumentationReturn,
				"Violation: Documentation is missing a return-value description",
				Out
			);
		}
	}
	// 項目を持つ書式の署名との照合（不足の項目と，対応する仮引数・戻値の無い項目）
	if(Dialect != Core::DocumentationStyle::Prose) {
		// 種類毎の項目の有無
		const auto HasItem = [&Items](const auto &Pred) -> bool {
			// 条件を満たす項目の有無の返戻
			return std::any_of(Items->begin(), Items->end(), Pred);
		};
		for(const std::string &Param : Sig.Params) if(
			!HasItem(
				[&Param](const DocItem &Item) -> bool {
					// 仮引数を説明する項目かの返戻
					return Item.Kind == DocItemKind::Param && DocCommentSynchronizer::CoversParam(Item.Name, Param);
				}
			)
		) {
			const NodeId Parameter = ParameterNameOf(Node, Param);
			Push(
				Parameter == NoNode ? PointOf(Node) : PointOf(Parameter),
				LintRule::DocumentationParameter,
				"Violation: Documentation is missing a description for parameter '" + Param + "'",
				Out
			);
		}
		for(const DocItem &Item : *Items) {
			if(Item.Kind == DocItemKind::Param && !Sig.HasUnnamedParam && !DocCommentSynchronizer::CoversAnyParam(Item.Name, Sig)) {
				Push(Node, LintRule::Other, DocCommentSynchronizer::ItemLabel(Dialect, Lines, Item) + " does not match a parameter", Out);
			}
		}
		const bool HasReturnItem = HasItem(
			[](const DocItem &Item) -> bool {
				// 戻値の項目かの返戻
				return Item.Kind == DocItemKind::Return;
			}
		);
		if(Sig.HasReturn && !HasReturnItem) {
			const NodeId ReturnType = ExplicitReturnTypeOf(Node);
			const NodeId Name = FunctionNameOf(Node);
			Push(
				ReturnType != NoNode ? ReturnType : Name == NoNode ? Node : Name,
				LintRule::DocumentationReturn,
				"Violation: Documentation is missing a return-value description",
				Out
			);
		}
		for(const DocItem &Item : *Items) if(Item.Kind == DocItemKind::Return && Sig.ReturnsFromSignature && !Sig.HasReturn) {
			Push(
				Node,
				LintRule::Other,
				DocCommentSynchronizer::ItemLabel(Dialect, Lines, Item) + " documents a function that returns no value",
				Out
			);
		}
		// 失敗条件の項目の説明の不足（空の項目は充足としない）と，呼出側へ伝わる失敗の項目の不足
		bool HasFailure = false, HasErrors = false, HasPanics = false, NeedsFailureDescription = false;
		for(const DocItem &Item : *Items) {
			const std::string_view Title = DocCommentSynchronizer::FailureTitle(Dialect, Lines, Item);
			if(Title.empty()) continue;
			NeedsFailureDescription = NeedsFailureDescription || !DocCommentSynchronizer::HasItemDescription(Rules.Notation, Item, Lines);
			HasFailure = true;
			const std::array<std::string_view, 28> &FailureWords = Core::VocabularyOf(Dialect, Core::DocDescription::Failure).Aliases;
			HasErrors = HasErrors || Title == FailureWords[0];
			HasPanics = HasPanics || Title == FailureWords[1];
		}
		const FailureFlow Flow = FailureFlowOf(Source, Node);
		if(NeedsFailureDescription) {
			Push(
				DocumentPoint,
				LintRule::DocumentationFailure,
				"Violation: Document the failure types and conditions visible to callers",
				Out
			);
		}
		if(Dialect == Core::DocumentationStyle::MarkdownHeading) {
			if((Flow.IsError && !HasErrors || Flow.IsPanic && !HasPanics) && !NeedsFailureDescription) {
				Push(
					DocumentPoint,
					LintRule::DocumentationFailure,
					"Violation: Document the failure types and conditions visible to callers",
					Out
				);
			}
		} else if(!HasFailure && (Flow.IsThrown || Flow.IsUnsure) && !NeedsFailureDescription) {
			Push(
				DocumentPoint,
				LintRule::DocumentationFailure,
				Flow.IsThrown ?
				"Violation: Document the failure types and conditions visible to callers" :
				"Review: Document the failure types and conditions visible to callers; verify reachability",
				Out
			);
		}
	}
	// 概要の無い文書コメントの報告（godoc の書出しの関数名は概要に数えない）
	if(!DocCommentSynchronizer::HasSummary(Dialect, Lines, *Items, Sig.Name)) {
		Push(DocumentPoint, LintRule::DocumentationSummary, "Violation: Documentation is missing a summary", Out);
	}
	// 終了
	return;
}

/**
 * クラスメンバ宣言順違反検出関数
 * @param Node 対象ノード
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckMemberDeclarationOrder(const NodeId Node, std::vector<LintWarning> &Out) {
	// 成員の順序を検査しない言語の場合の終了
	if(!Rules.OrderMembers) return;
	// C/C++ のフィールド宣言内の関数宣言子判定関数
	const auto IsFieldDeclarationMethod = [this](const NodeId FieldNode) -> bool {
		bool IsFound = false;
		// 宣言子の包みの内側だけを探索
		WalkChildrenCursor(
			FieldNode,
			[this, &IsFound](const NodeId WalkChild) -> bool {
				// 発見済なら降りない事の返戻
				if(IsFound) return false;
				// 関数宣言子の検出
				if(Syntax.Is(WalkChild, Role::FunctionDeclarator)) {
					IsFound = true;
					// 関数宣言子を発見した事の返戻
					return false;
				}
				// 宣言子の包みへ降りる事の返戻
				return Syntax.Is(WalkChild, Role::DeclaratorWrapper);
			}
		);
		// 関数宣言子を子孫に含むかの返戻
		return IsFound;
	};
	const NodeId Owner = Syntax.ParentOf(Node);
	const NodeId OwnerName = Syntax.FirstNamedChildWithRole(Owner, Role::TypeNameNode);
	const std::string_view TypeName = OwnerName == NoNode ? std::string_view() : NodeView(Source, OwnerName);
	// 文法が詳細順位を定める成員の分類
	const auto MemberRank = [this, &IsFieldDeclarationMethod, TypeName](const NodeId Child) -> int {
		if(Rules.DetailedMemberOrder) {
			// 友達宣言の順位の返戻
			if(Syntax.Is(Child, Role::MemberFriendRank)) return 1;
			// 型宣言の順位の返戻
			if(Syntax.Is(Child, Role::MemberTypeRank)) return 2;
			// 型宣言を兼ねる欄の順位の返戻
			if(
				Syntax.Is(Child, Role::FieldDefinition) && Syntax.FieldChild(Child, ChildRole::Declarator) == NoNode &&
				Syntax.Is(Syntax.FieldChild(Child, ChildRole::Type), Role::MemberTypeRank)
			) return 2;
			const NodeId Declarator = Syntax.FirstNamedChildWithRole(Child, Role::FunctionDeclarator);
			const bool IsMethod = Declarator != NoNode || IsFieldDeclarationMethod(Child) || Syntax.Is(Child, Role::MemberMethodRank);
			bool IsStatic = false;
			for(const NodeId Part : Syntax.ChildrenOf(Child)) {
				if(Syntax.Is(Part, Role::MemberStorageSpecifier) && NodeView(Source, Part) == Rules.MemberStaticWord) IsStatic = true;
			}
			if(IsMethod) {
				if(Declarator != NoNode) {
					// デストラクタの順位の返戻
					if(Syntax.FirstNamedChildWithRole(Declarator, Role::MemberDestructorName) != NoNode) return 10;
					// コンストラクタの順位の返戻
					if(!TypeName.empty() && NodeView(Source, Syntax.NamedChild(Declarator, 0)) == TypeName) return 9;
					// 演算子の順位の返戻
					if(Syntax.FirstNamedChildWithRole(Declarator, Role::OperatorName) != NoNode) return 8;
				}
				// 変換演算子の順位の返戻
				if(Syntax.FirstNamedChildWithRole(Child, Role::OperatorCast) != NoNode) return 8;
				// 静的メソッドの順位の返戻
				if(IsStatic) return 5;
				// 仮想メソッドの順位の返戻
				if(Syntax.HasTokenChild(Child, Rules.MemberVirtualWord)) return 6;
				// 通常のメソッドの順位の返戻
				return 7;
			}
			// 欄の静的指定に応じた順位の返戻
			if(Syntax.Is(Child, Role::FieldDefinition)) return IsStatic ? 3 : 4;
			// 分類出来ない成員の順位の返戻
			return -1;
		}
		// 型エイリアス／ネストの型宣言の順位１の返戻
		if(Syntax.Is(Child, Role::MemberTypeRank)) return 1;
		// 宣言子を持たない型の定義だけの成員（`struct S { … };`）は型の順位の返戻
		if(
			Syntax.Is(Child, Role::FieldDefinition) && Syntax.FieldChild(Child, ChildRole::Declarator) == NoNode &&
			Syntax.Is(Syntax.FieldChild(Child, ChildRole::Type), Role::MemberTypeRank)
		) return 1;
		// 成員変数又はプロパティの順位２と，内部の宣言子が関数の成員の順位３の返戻
		if(Syntax.Is(Child, Role::FieldDefinition)) return IsFieldDeclarationMethod(Child) ? 3 : 2;
		// メンバ変数の順位の返戻
		if(Syntax.Is(Child, Role::MemberFieldRank)) return 2;
		// メンバ関数の順位の返戻
		if(Syntax.Is(Child, Role::MemberMethodRank)) return 3;
		// 何れのメンバ順位にも該当しない事の返戻
		return -1;
	};
	// 順位追跡状態の初期化
	int LastRank = -1;
	Core::ForEachNamedChildOf(
		Syntax,
		Node,
		[&](const NodeId Child) -> bool {
			// アクセス区切りでの順位リセット
			if(Syntax.Is(Child, Role::AccessSection)) {
				LastRank = -1;
				// リセットのみで次の子へ走査継続の返戻
				return true;
			}
			// アクセス範囲の順位
			const int Rank = MemberRank(Child);
			// 順位の無いメンバを読み飛ばす事の返戻
			if(Rank < 0) return true;
			if(Rank < LastRank) {
				Push(
					Child,
					LintRule::MemberOrder,
					"Review: Reorder this member according to its role and dependencies; verify lifetime, external contract",
					Out
				);
			}
			LastRank = std::max(LastRank, Rank);
			// 次のメンバへ走査継続の返戻
			return true;
		}
	);
	// 終了
	return;
}

/**
 * constexpr 化候補検出関数
 * @param Node 対象ノード
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckConstexprCandidate(const NodeId Node, std::vector<LintWarning> &Out) {
	// `const` + リテラル右辺の組合せのみの抽出（関数呼出や式は実行時依存の可能性が有る為，対象外）
	bool HasConst = false, HasConstexpr = false, HasLiteral = false;
	NodeId Candidate = NoNode;
	uint32_t Declarators = 0;
	Core::ForEachChildOf(
		Syntax,
		Node,
		[&](const NodeId Child) -> void {
			// 修飾子と初期値の分類
			if(Syntax.Is(Child, Role::TypeQualifier)) {
				if(Syntax.IsWord(Child, Word::ConstantDeclaration)) HasConst = true;
				else if(Syntax.IsWord(Child, Word::ConstexprQualifier)) HasConstexpr = true;
			} else if(Syntax.Is(Child, Role::InitDeclarator)) {
				++Declarators;
				Candidate = Syntax.FirstNamedChildWithRole(Child, Role::Identifier);
				HasLiteral = Core::AnyChildOf(
					Syntax,
					Child,
					// 定数初期値リテラルの判定関数
					[this](const NodeId GrandChild) -> bool {
						return Syntax.Is(GrandChild, Role::ConstantLiteral);
					}
				);
			}
		}
	);
	// 候補該当時の警告登録
	if(HasConst && !HasConstexpr && HasLiteral && Declarators == 1 && Candidate != NoNode) {
		Push(
			Candidate,
			LintRule::ConstexprCandidate,
			"Review: Use `constexpr` for this compile-time constant; verify applicability, type, external contract",
			Out
		);
	}
	// 終了
	return;
}

/**
 * C/C++ const 候補検出関数
 * @param Source ソース文字列
 * @param Node 対象宣言ノード
 * @param MutatedNames 明示的な代入・更新を受ける名前
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckConstBindingCandidate(
	const std::string &Source,
	const NodeId Node,
	const std::unordered_set<std::string> &MutatedNames,
	std::vector<LintWarning> &Out
) {
	// const 修飾子の保持有無
	bool HasConst = false, HasBuiltinType = false;
	NodeId Init = NoNode;
	uint32_t InitCount = 0;
	Core::ForEachNamedChildOf(
		Syntax,
		Node,
		// 宣言子構成の収集関数
		[&](const NodeId Child) -> void {
			// 宣言子構成の分類
			if(Syntax.Is(Child, Role::TypeQualifier)) {
				if(Syntax.IsWord(Child, Word::ConstantDeclaration) || Syntax.IsWord(Child, Word::ConstexprQualifier)) HasConst = true;
			} else if(Syntax.Is(Child, Role::BuiltinNumericType)) HasBuiltinType = true;
			else if(Syntax.Is(Child, Role::InitDeclarator)) {
				Init = Child;
				// 初期化式の個数の進行
				++InitCount;
			}
		}
	);
	// const 付・組込型でない・初期化が１個でない宣言の場合の終了
	if(HasConst || !HasBuiltinType || InitCount != 1) return;
	// 変数名を持つ宣言子
	const NodeId Declarator = Syntax.FieldChild(Init, ChildRole::Declarator);
	// 宣言子が識別子でない場合の終了
	if(Declarator == NoNode || !Syntax.Is(Declarator, Role::Identifier)) return;
	const std::string Name(NodeView(Source, Declarator));
	// 名前が無いか変更される変数の場合の終了
	if(Name.empty() || MutatedNames.contains(Name)) return;
	// 局所でない宣言の場合の終了
	if(const NodeId Parent = Syntax.ParentOf(Node); Parent == NoNode || !Syntax.Is(Parent, Role::CompoundBody)) return;
	// const 候補変数の警告登録
	Push(Declarator, LintRule::LocalConst, "Review: Make this local binding `const`; verify type, binding, lifetime", Out);
	// 終了
	return;
}

/**
 * リテラル文字列の浮動小数判定関数
 * @param Text リテラルの文字列
 * @return 浮動小数なら true
 */
bool LintAnalyzer::IsFloatLiteral(const std::string_view Text) {
	// 大文字化されない Rust の十六進接頭辞に対する x・X 両方の検査
	bool HasHexPrefix = false, HasHexExponent = false, HasDecExponent = false;
	// 構成文字の走査
	for(const char Char : Text) switch(Char) {
	case '.':
		// 小数点包含時は浮動小数の返戻（整数リテラルは小数点を持たない・全言語共通）
		return true;
	case 'x':
	case 'X':
		HasHexPrefix = true;
		break;
	case 'P':
		// 指数記号の記録
		HasHexExponent = true;
		break;
	case 'E':
		HasDecExponent = true;
		break;
	}
	// １６進接頭辞包含時は指数 P の有無で判定し桁 E の１０進誤認を回避する，１０進は指数 E の有無で判定する事の返戻
	return HasHexPrefix ? HasHexExponent : HasDecExponent;
}

/**
 * C/C++ 浮動小数接尾辞候補検出関数
 * @param Source ソース文字列
 * @param Node 対象宣言ノード
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckFloatSuffixCandidate(const std::string &Source, const NodeId Node, std::vector<LintWarning> &Out) {
	// float 型の宣言でない場合の終了
	if(
		!Core::AnyChildOf(
			Syntax,
			Node,
			[this](const NodeId Child) -> bool {
				// float 型の指定子かの返戻
				return Syntax.IsNamed(Child) && Syntax.Is(Child, Role::PrimitiveType) && Syntax.IsWord(Child, Word::FloatingType);
			}
		)
	) return;
	// 宣言子毎の初期値検査
	Core::ForEachNamedChildOf(
		Syntax,
		Node,
		[&](const NodeId Child) -> void {
			// 初期化付の宣言子でない場合の終了
			if(!Syntax.Is(Child, Role::InitDeclarator)) return;
			// 変数名を持つ宣言子が無い場合の終了
			if(
				const NodeId Declarator = Syntax.FieldChild(Child, ChildRole::Declarator);
				Declarator == NoNode || !Syntax.Is(Declarator, Role::Identifier)
			) return;
			NodeId Value = Syntax.FieldChild(Child, ChildRole::Value);
			// 初期値を包む単項式の除去
			while(Value != NoNode) {
				if(
					Syntax.Is(Value, Role::ValueParentheses) ||
					Syntax.Is(Value, Role::UnaryOperation) && !FindUnnamedToken(Source, Value, Rules.Operators.Plus, Rules.Operators.Minus).empty()
				) Value = Syntax.NamedChild(Value, 0);
				else break;
			}
			// 初期値が数値リテラルでない場合の終了
			if(Value == NoNode || !Syntax.Is(Value, Role::NumericToken)) return;
			// 初期値のリテラル節点
			const std::string_view Literal = NodeView(Source, Value);
			// 浮動小数でないか空のリテラルの場合の終了
			if(!IsFloatLiteral(Literal) || Literal.empty()) return;
			// 既に接尾辞を持つリテラルの場合の終了
			if(const char Last = Literal.back(); Last == 'F' || Last == 'f' || Last == 'L' || Last == 'l') return;
			Push(
				Value,
				LintRule::FloatSuffix,
				"Review: Use the `F` suffix for this `float` literal; verify type, value range, evaluation",
				Out
			);
		}
	);
	// 終了
	return;
}

/**
 * #define 定数検出関数
 * @param Node 対象ノード
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckMacroConstant(const NodeId Node, std::vector<LintWarning> &Out) {
	// 値無 `#define` の検査対象からの除外（インクルードガード又は機能フラグ）
	if(Syntax.GetNode(Node).Facts.Has(Core::ConstraintKind::ValuedMacro)) {
		const NodeId Name = Syntax.FirstNamedChildWithRole(Node, Role::Identifier);
		Push(
			Name == NoNode ? Node : Name,
			LintRule::ConstantMacro,
			"Review: Replace this constant macro with a language constant; verify applicability, external contract",
			Out
		);
	}
	// 終了
	return;
}

/**
 * using namespace 検出関数
 * 競技プログラミング向の小規模な単一ファイルでは許される為，用途を確かめれない字面からは要確認の候補として示す
 * @param Source ソース文字列
 * @param Node 対象ノード
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckUsingNamespace(const std::string &Source, const NodeId Node, std::vector<LintWarning> &Out) {
	// `using namespace X` と `using X::Y`（エイリアス）は同じノード型の為，テキストで判別
	if(NodeView(Source, Node).find(Syntax.Rules().Spelling(Word::UsingNamespace)) != std::string_view::npos) {
		Push(Node, LintRule::UsingNamespace, "Review: Avoid `using namespace`; qualify names explicitly; verify applicability", Out);
	}
	// 終了
	return;
}

/**
 * 本体の文の始まる行の数の取得関数
 * 返戻コメントを省ける１〜２行の単純な関数を，行長制限に依る折返とコメントを数えず本体の必須改行で区切る文の行で判定する
 * @param Source ソース文字列
 * @param Function 関数又はラムダ
 * @param Limit 数える上限（達した時点で打ち切る）
 * @return 本体の中で文の始まる行の数（上限で打ち切る，式本体は０）
 */
size_t LintAnalyzer::CountStatementLines(const std::string &Source, const NodeId Function, const size_t Limit) const {
	NodeId Body = Syntax.FieldChild(Function, ChildRole::Body);
	// 場を持たない Kotlin / Swift の関数本体の包みと Swift のクロージャの文の並び
	if(Body == NoNode) Body = Syntax.FirstNamedChildWithRole(Function, Role::FunctionBody);
	if(Body == NoNode) Body = Syntax.FirstNamedChildWithRole(Function, Role::InnerFunctionStatements);
	std::vector<uint32_t> Lines;
	// 文の並びを持つ本体と，其の中の塊の文の始まる行の収集
	if(Body != NoNode) {
		Core::WalkNodes(
			Syntax,
			Body,
			[&](const NodeId Node) -> Core::WalkStep {
				// 上限に達した事の打切の返戻
				if(Lines.size() >= Limit) return Core::WalkStep::Stop;
				const NodeId Parent = Syntax.ParentOf(Node);
				const bool IsStatement = Node != Body && Syntax.IsNamed(Node) && !Syntax.Is(Node, Role::Comment) && (
					Syntax.Is(Node, Role::ElseClause) || Syntax.Is(Node, Role::ElseIfClause) || Parent == Body || Syntax.Is(Parent, Role::Block) ||
					Syntax.Is(Parent, Role::CompoundBody) || Syntax.Is(Parent, Role::IndentedBody) ||
					Syntax.Is(Parent, Role::InnerFunctionStatements) || Syntax.Is(Parent, Role::LexicalBlock) ||
					Syntax.Is(Parent, Role::StatementSequence)
				);
				if(IsStatement) {
					if(const uint32_t Line = TextEdit::LineStartOf(Source, BeginOf(Node)); std::ranges::find(Lines, Line) == Lines.end()) {
						Lines.push_back(Line);
					}
				}
				// 子孫の文へ降りる事の返戻
				return Core::WalkStep::Descend;
			}
		);
	}
	// 文の始まる行の数の返戻
	return std::min(Lines.size(), Limit);
}

/**
 * return コメント不在検出関数
 * @param Source ソース文字列
 * @param Node 対象ノード
 * @param Comments コメントの範囲（開始の昇順）
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckReturnCommentPresence(
	const std::string &Source,
	const NodeId Node,
	const std::vector<std::pair<uint32_t, uint32_t>> &Comments,
	std::vector<LintWarning> &Out
) {
	// 返戻の説明を規約の対象としない文法（CSS / SCSS の @return）の場合の終了
	if(!Rules.HasReturnComments) return;
	// 返戻の役割を問わない省略条件を先に適用
	for(NodeId Ancestor = Syntax.ParentOf(Node); Ancestor != NoNode; Ancestor = Syntax.ParentOf(Ancestor)) {
		// 切替の節に在る返戻コメントを省く場合の終了
		if(Syntax.Is(Ancestor, Role::CaseClause)) return;
		if(Syntax.Is(Ancestor, Role::FlowBoundary)) {
			// 短い関数の返戻コメントを省く場合の終了
			if(CountStatementLines(Source, Ancestor, 3) < 3) return;
			break;
		}
	}
	// 波括弧の無い制御構文の本体の return は，見出の折返で行の途中に在っても其の制御構文の行の前を返戻コメントの位置とする（整形仕様 7.6 の畳んだ本体）
	const NodeId Parent = Syntax.ParentOf(Node);
	const NodeId Owner = Syntax.Is(Parent, Role::BodyOwner) || Syntax.Is(Parent, Role::If) ? Parent : NoNode;
	// return の行又は其の制御構文の行の直前の行で終わるコメント（行の途中の return は其の行で始まる文の直前のコメント）が有れば警告不要
	for(const NodeId Target : { Node, Owner }) {
		const uint32_t TargetLine = Target == NoNode ? 0 : TextEdit::LineStartOf(Source, BeginOf(Target));
		if(!TargetLine) continue;
		// コメントは重ならず開始の昇順が終端の昇順でもある為，直前の行より後で終わる最初のコメントを確認
		const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Above = std::lower_bound(
			Comments.begin(),
			Comments.end(),
			TextEdit::LineStartOf(Source, TargetLine - 1) + 1,
			[](const std::pair<uint32_t, uint32_t> &Comment, const uint32_t Pos) -> bool {
				// コメントの終端が位置より前かの返戻
				return Comment.second < Pos;
			}
		);
		// 独立した直前のコメントだけを返戻の説明として読む
		if(Above != Comments.end() && Above->second <= TargetLine) {
			const size_t LineStart = TextEdit::LineStartOf(Source, Above->first);
			if(std::string_view(Source).substr(LineStart, Above->first - LineStart).find_first_not_of(" \t") != std::string_view::npos) {
				continue;
			}
			const size_t Index = static_cast<size_t>(Above - Comments.begin());
			const std::string_view Text = Repository.GetRecords()[Facts.Comments[Index].Record].Text;
			const size_t First = Text.find_first_not_of(" \t\r\n");
			if(First == std::string_view::npos) break;
			// 値の返戻は「〜の返戻」，値の無い終端と早期の終了は「〜終了」で結ぶ説明を満たす場合の終了
			if(Text.ends_with(Syntax.NamedChildCount(Node) ? "の返戻" : "終了")) return;
			Push(
				PointInComment(Index, Text, First),
				LintRule::ReturnCommentForm,
				"Review: Describe the termination or returned value in the return comment; verify documentation, intent",
				Out
			);
			// 返戻コメントの不足を報告した後の終了
			return;
		}
	}
	// return 前コメント欠落の警告登録
	Push(Node, LintRule::MissingReturnComment, "Violation: Add the required comment before this `return`", Out);
	// 終了
	return;
}

/**
 * 真偽値として評価される位置の印付関数
 * 制御構文の条件から括弧・論理積・論理和・論理否定だけを降り，其の下に在る節点へ印を付ける
 * 計算量：条件の節点数 C に対し O(C)（各節点を一度だけ訪れる）
 * @param Source ソース文字列
 * @param Host 条件を持つ制御構文の節点
 * @param Index 印を付ける索引（節点の識別子 → JSX の式の中か）
 */
void LintAnalyzer::MarkCondContext(const std::string &Source, const NodeId Host, CondContextIndex &Index) {
	// 条件根の収集準備
	const bool IsInJsx = Syntax.Is(Host, Role::EmbeddedExpression);
	std::vector<NodeId> Pending;
	const bool HasConditionField = Syntax.Is(Host, Role::ConditionFieldHost);
	const NodeId Condition = HasConditionField ? Syntax.FieldChild(Host, ChildRole::Condition) : NoNode;
	// 条件根の登録
	if(HasConditionField && Condition != NoNode) Pending.push_back(Condition);
	else if(!HasConditionField) {
		Core::ForEachNamedChildOf(
			Syntax,
			Host,
			// 条件候補の登録関数
			[&Pending](const NodeId Child) -> void {
				Pending.push_back(Child);
			}
		);
	}
	while(!Pending.empty()) {
		// 現在の節点
		const NodeId Cur = Pending.back();
		// 未処理の節点列からの末尾要素取出
		Pending.pop_back();
		Index.emplace(Cur, IsInJsx);
		// 括弧・論理積・論理和・論理否定の下も同じ真偽値の文脈に存在
		if(
			!Syntax.Is(Cur, Role::CondParenWrap) && !(
				Syntax.Is(Cur, Role::BinaryOperation) &&
				!FindUnnamedToken(Source, Cur, Rules.Operators.LogicalAnd, Rules.Operators.LogicalOr).empty()
			) && !(Syntax.Is(Cur, Role::UnaryOperation) && Syntax.HasTokenChild(Cur, Rules.Operators.LogicalNot))
		) continue;
		Core::ForEachNamedChildOf(
			Syntax,
			Cur,
			// 次の条件候補の登録関数
			[&Pending](const NodeId Child) -> void {
				Pending.push_back(Child);
			}
		);
	}
	// 終了
	return;
}

/**
 * 制御構文条件の整数ゼロ比較候補の検出関数
 * @param Source ソース文字列
 * @param Node 対象ノード
 * @param Conditions 真偽値として評価される位置の索引（制御構文の側から先に印を付けた物）
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckZeroComparisonCondition(
	const std::string &Source,
	const NodeId Node,
	const CondContextIndex &Conditions,
	std::vector<LintWarning> &Out
) {
	// 比較被演算子の取得
	const NodeId Lhs = Syntax.NamedChild(Node, 0), Rhs = Syntax.NamedChild(Node, 1);
	// 比較被演算子が無い場合の終了
	if(Lhs == NoNode || Rhs == NoNode) return;
	// ゼロ値リテラルの判定関数
	const auto IsZeroLiteral = [this, &Source](const NodeId Target) -> bool {
		// 数値リテラルでない事の返戻
		if(!Syntax.Is(Target, Role::ZeroLiteral)) return false;
		// リテラル字面の取得
		const std::string_view View = NodeView(Source, Target);
		// ０で始まらない事の返戻
		if(View.empty() || View[0] != '0') return false;
		// 整数接尾辞の検証
		for(size_t Idx = 1; Idx < View.size(); ++Idx) {
			// 整数の接尾辞以外が有る事の返戻
			if(const char Suffix = View[Idx]; Suffix != 'L' && Suffix != 'U' && Suffix != 'l' && Suffix != 'u') return false;
		}
		// `0` と整数接尾辞だけで構成される値の返戻
		return true;
	};
	// 何れの被演算子もゼロ値でない場合の終了
	if(!IsZeroLiteral(Lhs) && !IsZeroLiteral(Rhs)) return;
	// 比較演算子の抽出
	std::string_view Operator = FindUnnamedToken(Source, Node, Rules.Operators.Equal, Rules.Operators.NotEqual);
	if(Operator.empty() && Rules.HasStrictEquality) {
		Operator = FindUnnamedToken(Source, Node, Rules.Operators.StrictEqual, Rules.Operators.StrictNotEqual);
	}
	// 比較演算子が無い場合の終了
	if(Operator.empty()) return;
	// 真偽値として評価される位置に在る比較だけを対象にする（印は制御構文の側から先に付けて存在）
	const CondContextIndex::const_iterator Hit = Conditions.find(Node);
	// 真偽値として評価される位置に無い場合の終了
	if(Hit == Conditions.end()) return;
	const NodeId Token = TokenChildOf(Node, Operator);
	const NodeId Parent = Syntax.ParentOf(Node);
	if(Hit->second && Operator == Rules.Operators.NotEqual || Hit->second && Operator == Rules.Operators.StrictNotEqual) if(
		Syntax.Is(Parent, Role::BinaryOperation) && Syntax.NamedChild(Parent, 0) == Node &&
		Syntax.HasTokenChild(Parent, Rules.Operators.LogicalAnd) && Syntax.Is(Syntax.NamedChild(Parent, 1), Role::TagElement)
	) {
		Push(
			Token == NoNode ? Node : Token,
			LintRule::JsxNonzeroCondition,
			"Review: Use `!!x` for nonzero in JSX condition; verify type, value range, evaluation",
			Out
		);
		// 比較対象を確定出来ない場合の終了
		return;
	}
	Push(
		Token == NoNode ? Node : Token,
		LintRule::ZeroComparison,
		"Review: Use the zero-test form required for this numeric type; verify type, value range",
		Out
	);
	// 終了
	return;
}

/**
 * 数値比較の厳密等価検出関数
 * @param Source ソース文字列
 * @param Node 対象ノード
 * @param Conditions 真偽判定に使われる比較の索引
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckNumberStrictEquality(
	const std::string &Source,
	const NodeId Node,
	const CondContextIndex &Conditions,
	std::vector<LintWarning> &Out
) {
	// 比較被演算子の取得
	const NodeId Lhs = Syntax.NamedChild(Node, 0), Rhs = Syntax.NamedChild(Node, 1);
	// 比較被演算子が無い場合の終了
	if(Lhs == NoNode || Rhs == NoNode) return;
	// 厳密等価演算子の抽出
	const std::string_view Operator = FindUnnamedToken(Source, Node, Rules.Operators.StrictEqual, Rules.Operators.StrictNotEqual);
	// 厳密等価演算子が無い場合の終了
	if(Operator.empty()) return;
	// 数値リテラルの判定関数
	const auto IsNumberLiteral = [this](const NodeId Target) -> bool {
		// 数値リテラルの返戻
		if(Syntax.Is(Target, Role::NumericLiteral)) return true;
		// 負の数値リテラルでない事の返戻
		if(!Syntax.Is(Target, Role::UnaryOperation) || !Syntax.HasTokenChild(Target, Rules.Operators.Minus)) return false;
		// 負号の被演算子取得
		const NodeId Operand = Syntax.NamedChild(Target, 0);
		// 負号の被演算子が数値リテラルかの返戻
		return Operand != NoNode && Syntax.Is(Operand, Role::NumericLiteral);
	};
	// 厳密比較を維持する値の判定関数
	const auto IsExcluded = [this, &Source](const NodeId Target) -> bool {
		// 除外候補の字面取得
		// null 系又は typeof 式かの返戻
		return Syntax.Rules().IsSpelling(NodeView(Source, Target), Word::NullishValue) ||
		Syntax.Is(Target, Role::UnaryOperation) && Syntax.HasWordChild(Target, Word::TypeQuery);
	};
	// 両辺が数値リテラルなら同じ Number 型と確定出来る
	if(IsExcluded(Lhs) || IsExcluded(Rhs) || !IsNumberLiteral(Lhs) || !IsNumberLiteral(Rhs)) return;
	// BigInt と Number の混在及び零判定との競合を避ける
	const std::string_view Left = NodeView(Source, Lhs), Right = NodeView(Source, Rhs);
	// BigInt の混在又は零判定に該当する場合の終了
	if(
		!Rules.BigIntegerSuffix.empty() && Left.ends_with(Rules.BigIntegerSuffix) != Right.ends_with(Rules.BigIntegerSuffix) ||
		Conditions.contains(Node) && (Rules.IsSpelling(Left, Word::IntegerZero) || Rules.IsSpelling(Right, Word::IntegerZero))
	) return;
	const NodeId Token = TokenChildOf(Node, Operator);
	Push(
		Token == NoNode ? Node : Token,
		LintRule::NumericComparison,
		"Violation: Use numeric equality instead of strict equality for matching numeric types",
		Out
	);
	// 終了
	return;
}

/**
 * ヌリッシュ比較縮約候補検出関数
 * @param Source ソース文字列
 * @param Node 対象二項式
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckNullishComparisonCandidate(
	const std::string &Source,
	const NodeId Node,
	std::vector<LintWarning> &Out
) {
	// 外側の論理演算子の抽出
	const std::string_view OuterOperator = FindUnnamedToken(Source, Node, Rules.Operators.LogicalOr, Rules.Operators.LogicalAnd);
	// 論理演算子が無い場合の終了
	if(OuterOperator.empty()) return;
	// 比較式二項の取得
	const NodeId Lhs = Syntax.NamedChild(Node, 0), Rhs = Syntax.NamedChild(Node, 1);
	// 比較式が無い場合の終了
	if(Lhs == NoNode || Rhs == NoNode) return;
	struct Comparison {
		std::string_view Subject; // 条件式の比較対象
		std::string_view Nullish; // ヌル値の被演算子
		std::string_view Operator; // 演算子節点
	};
	// 比較式の解析関数
	const auto Parse = [this, &Source](const NodeId Target, Comparison &Result) -> bool {
		// 二項式でない事の返戻
		if(!Syntax.Is(Target, Role::BinaryOperation)) return false;
		// 厳密等価演算子の抽出
		Result.Operator = FindUnnamedToken(Source, Target, Rules.Operators.StrictEqual, Rules.Operators.StrictNotEqual);
		// 厳密等価でない事の返戻
		if(Result.Operator.empty()) return false;
		// 両被演算子の抽出
		const NodeId Left = Syntax.NamedChild(Target, 0), Right = Syntax.NamedChild(Target, 1);
		// 被演算子の欠けた式の返戻
		if(Left == NoNode || Right == NoNode) return false;
		// ヌル値側と比較主体の分離
		if(
			const std::string_view LeftText = NodeView(Source, Left), RightText = NodeView(Source, Right);
			Syntax.Rules().IsSpelling(LeftText, Word::NullishValue)
		) {
			// 左辺がヌル値の場合の解析結果格納
			Result.Nullish = LeftText;
			Result.Subject = RightText;
		} else if(Syntax.Rules().IsSpelling(RightText, Word::NullishValue)) {
			Result.Nullish = RightText;
			Result.Subject = LeftText;
		// null / undefined との比較でない事の返戻
		} else return false;
		// 比較主体を取得出来たかの返戻
		return !Result.Subject.empty();
	};
	// 左右の比較結果の取得
	Comparison Left {}, Right {};
	// 同じ対象を異なるヌル値の各々と比べる組でない場合の終了
	if(!Parse(Lhs, Left) || !Parse(Rhs, Right) || Left.Subject != Right.Subject || Left.Nullish == Right.Nullish) return;
	// 縮約可能な演算子対の判定
	const bool IsEqualityPair = OuterOperator == Rules.Operators.LogicalOr && Left.Operator == Rules.Operators.StrictEqual &&
	Right.Operator == Rules.Operators.StrictEqual;
	// 同値比較の論理和と非同値比較の論理積の何れでもない場合の終了
	if(
		const bool IsInequalityPair = OuterOperator == Rules.Operators.LogicalAnd && Left.Operator == Rules.Operators.StrictNotEqual &&
		Right.Operator == Rules.Operators.StrictNotEqual;
		!IsEqualityPair && !IsInequalityPair
	) return;
	const NodeId Token = TokenChildOf(Node, OuterOperator);
	Push(
		Token == NoNode ? Node : Token,
		LintRule::NullishComparison,
		"Review: Combine the `null` and `undefined` checks; verify binding, evaluation, intent",
		Out
	);
	// 終了
	return;
}

/**
 * C 形式キャスト検出関数
 * @param Node 対象ノード
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckCStyleCast(const NodeId Node, std::vector<LintWarning> &Out) {
	// C 形式キャストの検出
	Push(
		Node,
		LintRule::CStyleCast,
		"Review: Replace the C-style cast with the appropriate C++ cast; verify type, value range, intent",
		Out
	);
	// 終了
	return;
}

/**
 * 空ポインターの旧来のマクロの検出関数
 * @param Node 対象ノード
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckNullMacroCandidate(const NodeId Node, std::vector<LintWarning> &Out) {
	// `NULL` マクロの警告登録
	if(Syntax.IsWord(Node, Word::NullMacro)) {
		Push(Node, LintRule::Nullptr, "Review: Replace `NULL` with `nullptr`; verify type, binding", Out);
	}
	// 終了
	return;
}

/**
 * 波括弧言語の if 統合候補検出関数
 * @param Source ソース文字列
 * @param Node 対象 if 文
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckUnsafeIfMergeCandidate(const std::string &Source, const NodeId Node, std::vector<LintWarning> &Out) {
	// 単一本体の抽出関数
	const auto SingleBody = [this](NodeId Body) -> NodeId {
		// 波括弧本体の単一文への縮約
		if(Body != NoNode && Syntax.Is(Body, Role::FunctionBodyContainer) && Syntax.NamedChildCount(Body) == 1) {
			Body = Syntax.NamedChild(Body, 0);
		}
		// 波括弧を除いた単一本体の返戻
		return Body;
	};
	// 統合可能な単純条件の判定関数
	const auto SimpleCondition = [this, &Source](const NodeId IfNode) -> bool {
		// 条件式の節点
		NodeId Condition = Syntax.FieldChild(IfNode, ChildRole::Condition);
		if(Condition != NoNode && Syntax.Is(Condition, Role::CondParenWrap)) {
			// 括弧内の節点
			NodeId Inner = NoNode;
			Core::ForEachNamedChildOf(
				Syntax,
				Condition,
				[this, &Inner](const NodeId Child) -> void {
					if(!Syntax.Is(Child, Role::ConditionInitializer)) Inner = Child;
				}
			);
			Condition = Inner;
		}
		// 条件の無い分岐を対象外とする事の返戻
		if(Condition == NoNode) return false;
		// 評価順と回数を保持可能な否定条件の統合
		if(
			Syntax.Is(Condition, Role::PrefixUnary) && Syntax.NamedChildCount(Condition) == 1 &&
			NodeView(Source, Syntax.Child(Condition, 0)) == Rules.Operators.LogicalNot
		) Condition = Syntax.NamedChild(Condition, 0);
		// 型を構文から確定出来ない単純識別子条件かの返戻
		return Syntax.Is(Condition, Role::PlainVariable);
	};
	const NodeId Consequence = SingleBody(Syntax.IfConsequence(Node));
	const NodeId Alternative = Syntax.FieldChild(Node, ChildRole::Alternative);
	// ネストした if は論理積への統合候補
	if(
		Alternative == NoNode && Consequence != NoNode && Syntax.Is(Consequence, Role::Conditional) &&
		Syntax.FieldChild(Consequence, ChildRole::Alternative) == NoNode && SimpleCondition(Node) && SimpleCondition(Consequence)
	) {
		Push(
			Node,
			LintRule::MergeIf,
			"Review: Merge the conditions of these identical branch bodies; verify evaluation, lifetime",
			Out
		);
		// 論理積への統合候補を報告した後の終了
		return;
	}
	// 束縛を導入しない else 無の if が同じ脱出文だけを本体に持つ組かの判定関数
	// 先の脱出で後の条件を評価しない為，条件の形を問わず論理和の短絡評価と同じ順と回数に為る
	const auto IsExitPair = [this, &Source, &SingleBody](const NodeId First, const NodeId Second) -> bool {
		// else 無の if の見出（本体を除く子）が束縛を導入するかの判定関数（束縛は統合すると有効範囲が変わる）
		const auto IsBindingOrBranched = [this](const NodeId If) -> bool {
			const NodeId Consequence = Syntax.IfConsequence(If);
			// else を持つか，見出の何れかの子が束縛を含むかの返戻
			return Syntax.FieldChild(If, ChildRole::Alternative) != NoNode || std::ranges::any_of(
				Syntax.ChildrenOf(If),
				[this, Consequence](const NodeId Header) -> bool {
					return Header != Consequence && Core::AnyNodeOf(
						Syntax,
						Header,
						[this](const NodeId Part) -> bool {
							return Syntax.Is(Part, Role::ConditionInitializer) || Syntax.Is(Part, Role::BindingCondition) ||
							Syntax.Is(Part, Role::ValueBindingPattern) || Syntax.Is(Part, Role::DeclLike) ||
							Syntax.Is(Part, Role::ShortVariableDeclaration);
						}
					);
				}
			);
		};
		const NodeId Body =
		Syntax.Is(First, Role::Conditional) && Syntax.Is(Second, Role::Conditional) ? SingleBody(Syntax.IfConsequence(First)) : NoNode;
		// 両方の本体が同じ字面の脱出文で，何方も else と束縛を持たないかの返戻
		return Body != NoNode && Syntax.Is(Body, Role::FlowExit) &&
		NodeView(Source, Body) == NodeView(Source, SingleBody(Syntax.IfConsequence(Second))) && !IsBindingOrBranched(First) &&
		!IsBindingOrBranched(Second);
	};
	// コメントを除いた隣の文の取得関数
	const auto Adjacent = [this](NodeId Statement, const bool IsForward) -> NodeId {
		do Statement = IsForward ? Syntax.NextNamedSibling(Statement) : Syntax.PrevNamedSibling(Statement);
		while(Statement != NoNode && Syntax.Is(Statement, Role::Comment));
		// 隣の文の返戻
		return Statement;
	};
	// 同じ脱出文を持つ連続した if の最長の群の先頭だけの論理和への統合候補の報告
	if(IsExitPair(Node, Adjacent(Node, true))) {
		if(!IsExitPair(Adjacent(Node, false), Node)) {
			Push(Node, LintRule::MergeIf, "Review: Merge the conditions of these identical branch bodies; verify lifetime", Out);
		}
		// 論理和への統合候補を判じた後の終了
		return;
	}
	// 同一本体の else-if は論理和への統合候補
	NodeId ElseIf = NoNode;
	if(Alternative != NoNode) {
		if(Syntax.Is(Alternative, Role::ConditionalAlternative)) ElseIf = Alternative;
		else if(Syntax.Is(Alternative, Role::AlternativeClause)) {
			Core::ForEachNamedChildOf(
				Syntax,
				Alternative,
				// else-if 節点の採用関数
				[this, &ElseIf](const NodeId Child) -> void {
					if(ElseIf == NoNode && Syntax.Is(Child, Role::Conditional)) ElseIf = Child;
				}
			);
		}
	}
	if(ElseIf != NoNode && SimpleCondition(Node) && SimpleCondition(ElseIf)) if(
		const NodeId ElseBody = SingleBody(Syntax.IfConsequence(ElseIf));
		Consequence != NoNode && ElseBody != NoNode && NodeView(Source, Consequence) == NodeView(Source, ElseBody)
	) {
		Push(
			Node,
			LintRule::MergeIf,
			"Review: Merge the conditions of these identical branch bodies; verify evaluation, lifetime",
			Out
		);
	}
	// 終了
	return;
}

/**
 * 規約番号参照の検出関数（節番号は規約の改訂でずれ，参照が陳腐化する為，意図其の物を書く）
 * @param Index 対象の投影済コメントの添字
 * @param Structure コード例の保護位置
 * @param Out 警告の格納先
 */
void LintAnalyzer::CheckStandardsReference(
	const size_t Index,
	const CommentStructure &Structure,
	std::vector<LintWarning> &Out
) {
	const std::string_view Text = Repository.GetRecords()[Facts.Comments[Index].Record].Text;
	const std::span<const unsigned char> Protected = Structure.GetCodeMask(Facts.Comments[Index].Record);
	const auto IsProtected = [Protected](const size_t Pos) -> bool {
		return Pos < Protected.size() && Protected[Pos];
	};
	size_t Reference = std::string_view::npos;
	for(size_t Pos = Text.find("CODING_STANDARDS"); Pos != std::string_view::npos; Pos = Text.find("CODING_STANDARDS", Pos + 1)) {
		if(!IsProtected(Pos)) Reference = std::min(Reference, Pos);
	}
	static constexpr std::string_view SectionMark = "§";
	for(size_t Pos = Text.find(SectionMark); Pos != std::string_view::npos; Pos = Text.find(SectionMark, Pos + 1)) if(
		const size_t Scan = Pos + SectionMark.size();
		!IsProtected(Pos) && Scan < Text.size() && std::isdigit(static_cast<unsigned char>(Text[Scan]))
	) Reference = std::min(Reference, Pos);
	// 「規約」に続く `N.M` 形の節番号を探す語と番号の間に助詞が挟まる形も同じ参照の為，数字が現れる迄の仮名の読飛
	static constexpr size_t KeywordLen = 6;
	for(size_t Pos = Text.find("規約"); Pos != std::string_view::npos; Pos = Text.find("規約", Pos + KeywordLen)) {
		if(IsProtected(Pos)) continue;
		size_t Scan = Pos + KeywordLen;
		// 「規約」と番号の間に置かれ得るのは空白と１語の助詞だけで，其れ以外が挟まれば別の文脈と確認
		while(Scan < Text.size() && (Text[Scan] == ' ' || Text[Scan] == '\t')) ++Scan;
		// 番号を括弧で包む形も同じ参照の為，助詞の後の開き括弧（全角・半角）も読み飛ばす
		for(const std::string_view Joint : { std::string_view("の"), std::string_view("は"), std::string_view("で") }) {
			if(Text.compare(Scan, Joint.size(), Joint)) continue;
			// 規約番号の走査位置の進行
			Scan += Joint.size();
			while(Scan < Text.size() && (Text[Scan] == ' ' || Text[Scan] == '\t')) ++Scan;
			break;
		}
		if(!Text.compare(Scan, 3, "（")) Scan += 3;
		else if(Scan < Text.size() && Text[Scan] == '(') ++Scan;
		// 節番号の数字開始位置
		const size_t DigitStart = Scan;
		while(Scan < Text.size() && std::isdigit(static_cast<unsigned char>(Text[Scan]))) ++Scan;
		if(Scan == DigitStart || Scan >= Text.size() || Text[Scan] != '.') continue;
		// 規約番号の走査位置の進行
		++Scan;
		if(Scan >= Text.size() || !std::isdigit(static_cast<unsigned char>(Text[Scan]))) continue;
		// 版番号と節番号の区別
		while(Scan < Text.size() && (std::isdigit(static_cast<unsigned char>(Text[Scan])) || Text[Scan] == '.')) ++Scan;
		while(Scan < Text.size() && (Text[Scan] == ' ' || Text[Scan] == '\t')) ++Scan;
		if(Text.compare(Scan, 3, "版")) Reference = std::min(Reference, Pos);
	}
	if(Reference != std::string_view::npos) {
		Push(
			PointInComment(Index, Text, Reference),
			LintRule::StandardsReference,
			"Violation: Describe the intent instead of referring to the coding standards",
			Out
		);
	}
	// 終了
	return;
}

/**
 * 規約の採用形式でない文書の検出関数（整形仕様 8.4 の文書の形式）
 * 文書属性として保つ Rust の行形式の文書と，本文の解釈が異なる為に変換しない Java の Markdown の文書を，連続する行の最初で１度報告する
 * @param Src 検証を通った最終字面
 * @param Index 最終字面のコメントの原文順の番号
 * @param Out 警告の格納先
 */
void LintAnalyzer::CheckDocCommentForm(const std::string &Src, const size_t Index, std::vector<LintWarning> &Out) {
	// 行形式の文書を保つ言語以外の返戻
	if(!Rules.Notation.DocumentCommentIsAttribute && !Rules.Notation.ReviewMarkdownLine) return;
	const std::string_view Text = Src;
	// コメントの記法
	const auto NotationAt = [this](const size_t At) -> CommentNotation {
		// 記録の記法の返戻
		return Repository.GetRecords()[Facts.Comments[At].Record].Notation;
	};
	const CommentNotation Notation = NotationAt(Index);
	// 行形式の文書（Rust は内側の文書を含む）以外の返戻
	if(
		Notation != CommentNotation::TripleSlashLine &&
		(!Rules.Notation.DocumentCommentIsAttribute || Notation != CommentNotation::SlashBangLine)
	) return;
	const StaticLint::OutputRange &Range = Facts.Comments[Index].Range;
	// 改行１つだけを挟んで続く同じ記法の行は，並びの最初の行で報告済の事の返戻
	if(Index && NotationAt(Index - 1) == Notation) {
		// 前の行の始まりから此の行の始まり迄と，其の間の空白
		const StaticLint::OutputRange &Prev = Facts.Comments[Index - 1].Range;
		const std::string_view Span = Text.substr(Prev.Start, Range.Start - Prev.Start);
		const std::string_view Gap = Text.substr(Prev.End, Range.Start - std::min(Prev.End, Range.Start));
		// 前の行と改行１つだけを挟んで続く場合の終了
		if(Gap.find_first_not_of(" \t\r\n") == std::string_view::npos && std::count(Span.begin(), Span.end(), '\n') == 1) return;
	}
	Push(
		Range.Point,
		LintRule::DocumentationNotation,
		Notation == CommentNotation::SlashBangLine ?
		"Violation: Use inner Rust block documentation for this documentation" :
		"Violation: Use " + std::string(Rules.DocumentationForm) + " for this documentation",
		Out
	);
	// 終了
	return;
}

/**
 * コメントの文区切の検査関数
 * 説明文は使用言語・引用・記法に依らず１文１論理行とし，句点を書かない
 * 本文を値として保つ指示コメントは対象外とする
 * @param Index 対象の投影済コメントの位置（最終字面の順）
 * @param Structure 最終字面のコメントの記録の本文構造（正規化と同じ判定の物）
 * @param DirectiveStarts 指示コメントの始まりの位置の昇順
 * @param Out 警告の格納先
 */
void LintAnalyzer::CheckCommentSentences(
	const size_t Index,
	const CommentStructure &Structure,
	const std::vector<uint32_t> &DirectiveStarts,
	std::vector<LintWarning> &Out
) {
	const StaticLint::CommentFacts &Comment = Facts.Comments[Index];
	const uint32_t Start = Comment.Range.Start, End = Comment.Range.End;
	// 本文の無いコメントと指示コメントの場合の終了
	if(Start >= End || std::binary_search(DirectiveStarts.begin(), DirectiveStarts.end(), Start)) return;
	std::string_view Text = Comment.Text;
	// HTML のコメントは記号の間を本文とする（文書の内容でソース側に残り，記法の値を持たない，整形仕様 3.2, 4.4）
	if(Rules.Notation.HasMarkupComments) {
		if(Text.starts_with(Rules.MarkupCommentOpen)) Text.remove_prefix(Rules.MarkupCommentOpen.size());
		if(Text.ends_with(Rules.MarkupCommentClose)) Text.remove_suffix(Rules.MarkupCommentClose.size());
		// 記号の間の各論理行から区切空白を除いた本文
		std::string Body;
		for(size_t Cursor = 0; Cursor <= Text.size();) {
			const size_t End = std::min(Text.find('\n', Cursor), Text.size());
			std::string_view Line = Text.substr(Cursor, End - Cursor);
			Line.remove_prefix(std::min(Line.find_first_not_of(" \t"), Line.size()));
			Body.append(Line);
			if(End == Text.size()) break;
			Body.push_back('\n');
			Cursor = End + 1;
		}
		// 記法の値を持たない本文の本文構造
		CommentRecord Markup { std::move(Body), { CommentPlacement::Before } };
		Markup.Format.Rules = &Rules.Notation;
		const CommentStructure MarkupStructure = CommentNormalizer::Inspect(ReadOnlyCommentView({ &Markup, 1 }));
		for(
			const CommentNormalizer::SentenceIssue &Issue :
			CommentNormalizer::FindSentenceIssues(Markup.Text, MarkupStructure.GetCodeMask(0), {})
		) {
			Push(
				PointInComment(Index, Markup.Text, Issue.Offset),
				Issue.IsPeriod ? LintRule::CommentPeriod : LintRule::CommentSentences,
				Issue.IsPeriod ?
				"Violation: Remove sentence-final periods from this comment sentence" :
				"Violation: Use one sentence per logical comment line",
				Out
			);
		}
		// HTML のコメントの検査の終了
		return;
	}
	// 記号と区切空白を除いた本文の検査
	const CommentRecord &Record = Repository.GetRecords()[Comment.Record];
	const CommentNotation Notation = Record.Notation;
	const std::span<const unsigned char> Protected = Structure.GetCodeMask(Comment.Record);
	const std::vector<CommentNormalizer::SentenceIssue> Issues =
	CommentNormalizer::FindSentenceIssues(Record.Text, Protected, CommentNormalizer::StackMarkerOf(Notation));
	if(!Issues.empty()) {
		for(const CommentNormalizer::SentenceIssue &Issue : Issues) {
			Push(
				PointInComment(Index, Record.Text, Issue.Offset),
				Issue.IsPeriod ? LintRule::CommentPeriod : LintRule::CommentSentences,
				Issue.IsPeriod ?
				"Violation: Remove sentence-final periods from this comment sentence" :
				"Violation: Use one sentence per logical comment line",
				Out
			);
		}
		// 違反の報告の終了
		return;
	}
	// 行幅で折った１文の候補（記録の中の隣の論理行と，同じ記号で直前の行を占める行コメントの最後の論理行，コード例の行を除く）
	std::string_view Previous;
	bool IsPreviousCode = false;
	const std::string_view Opener = Core::CommentRepository::OpenerOf(Notation);
	if(
		const size_t LineBegin = Source.rfind('\n', Start ? Start - 1 : 0) + 1;
		LineBegin > 1 && !Core::CommentRepository::IsBlockNotation(Notation)
	) {
		const size_t PreviousBegin = Source.rfind('\n', LineBegin - 2) + 1;
		std::string_view Line = std::string_view(Source).substr(PreviousBegin, LineBegin - 1 - PreviousBegin);
		// 前の行のコメントの始まり
		const size_t PreviousStart = PreviousBegin + std::min(Line.find_first_not_of(" \t"), Line.size());
		TextEdit::TrimView(Line);
		// 独立行の行コメントの前の行が同じ記号の説明の行コメント（前の記録が其の行の頭から始まる物で，文字列の中の記号の行と本文が説明文でない指示コメントを除く）の時の本文
		if(
			Index && Facts.Comments[Index - 1].Range.Start == PreviousStart &&
			std::string_view(Source).substr(LineBegin, Start - LineBegin).find_first_not_of(" \t") == std::string_view::npos &&
			Line.starts_with(Opener) && !std::binary_search(DirectiveStarts.begin(), DirectiveStarts.end(), PreviousStart)
		) {
			Previous = Line.substr(Opener.size());
			// 直前の行コメントの最後の論理行がコード例か（論理行は行頭に空白を持たない）
			const std::string_view Text = Repository.GetRecords()[Facts.Comments[Index - 1].Record].Text;
			const size_t Last = Text.rfind('\n') + 1;
			IsPreviousCode = Last < Text.size() && Structure.GetCodeMask(Facts.Comments[Index - 1].Record)[Last];
		}
	}
	for(size_t Cursor = 0; Cursor <= Record.Text.size();) {
		const size_t End = std::min(Record.Text.find('\n', Cursor), Record.Text.size());
		const std::string_view Line = std::string_view(Record.Text).substr(Cursor, End - Cursor);
		// 論理行の最初の字がコード例か（論理行は行頭に空白を持たない）
		const bool IsCode = Cursor < End && Protected[Cursor];
		if(!IsCode && !IsPreviousCode && CommentNormalizer::ContinuesSentence(Previous, Line)) {
			const size_t First = std::min(Line.find_first_not_of(" \t"), Line.size());
			Push(
				PointInComment(Index, Record.Text, Cursor + First),
				LintRule::CommentSentences,
				"Review: Use one sentence per logical comment line; verify documentation",
				Out
			);
			// 候補の報告の終了
			return;
		}
		Previous = Line;
		IsPreviousCode = IsCode;
		Cursor = End + 1;
	}
	// 終了
	return;
}

/**
 * 真偽値の排他的論理和の代入を勧める反転代入の検出関数
 * @param Source ソース文字列
 * @param Node 対象ノード
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckBoolFlipPattern(const std::string &Source, const NodeId Node, std::vector<LintWarning> &Out) {
	// 型情報を持たない真偽値反転に対する条件付警告
	const NodeId Lhs = Syntax.NamedChild(Node, 0), Rhs = Syntax.NamedChild(Node, 1);
	// 両辺が揃った左辺の単純識別子への反転代入でない場合の終了
	if(
		Lhs == NoNode || Rhs == NoNode || !Syntax.Is(Lhs, Role::Identifier) || !Syntax.Is(Rhs, Role::UnaryPrefixExpression) ||
		!Syntax.HasTokenChild(Node, Rules.Operators.Assignment)
	) return;
	NodeId Operand = NoNode;
	bool HasNegation = false;
	const std::vector<std::string_view> &NegationWords = Rules.Words[static_cast<size_t>(Word::Negation)];
	Core::ForEachChildOf(
		Syntax,
		Rhs,
		// 反転演算子と被演算子の抽出関数
		[&](const NodeId Child) -> void {
			if(Syntax.IsNamed(Child)) {
				if(Operand == NoNode) Operand = Child;
			} else if(
				const std::string_view Token = NodeView(Source, Child);
				Token == Rules.Operators.LogicalNot || std::ranges::find(NegationWords, Token) != NegationWords.end()
			) HasNegation = true;
		}
	);
	// 左辺と同じ識別子の否定でない場合の終了
	if(
		!HasNegation || Operand == NoNode || !Syntax.Is(Operand, Role::Identifier) || NodeView(Source, Lhs) != NodeView(Source, Operand)
	) return;
	// 真偽値反転式の置換候補警告登録
	const NodeId Token = TokenChildOf(Node, Rules.Operators.Assignment);
	Push(
		Token == NoNode ? Node : Token,
		LintRule::BooleanNegation,
		"Review: Use the boolean-negation assignment required for this target; verify type, binding, evaluation",
		Out
	);
	// 終了
	return;
}

/**
 * 整数リテラルの値の取得関数
 * 桁区切 (`_` / `'`) と基数の接頭辞を解き，値の後の接尾辞を分ける
 * 計算量：字面の長さ N に対し O(N)
 * @param Text 整数リテラルの字面
 * @param Suffix 値の後の接尾辞（`u8` / `L` 等）の格納先
 * @return 値（６４ビットに収まらなければ空）
 */
std::optional<uint64_t> LintAnalyzer::IntegerLiteralValue(const std::string_view Text, std::string_view &Suffix) {
	// 基数接頭辞の判定
	size_t Pos = 0;
	uint32_t Base = 10;
	if(Text.size() > 1 && Text[0] == '0') {
		const char Mark = static_cast<char>(Text[1] | 0X20);
		Base = Mark == 'x' ? 16 : Mark == 'b' ? 2 : Mark == 'o' ? 8 : 10;
		if(Base != 10) Pos = 2;
		// C 系・Java・Go・PHP・JavaScript の `0` で始まる８進
		else if(std::isdigit(static_cast<unsigned char>(Text[1])) && Rules.HasLeadingZeroOctal) Base = 8;
	}
	// 桁区切と接尾辞を分けた整数値の読取
	uint64_t Value = 0;
	for(; Pos < Text.size(); ++Pos) {
		const char Char = Text[Pos];
		// 桁区切の読飛
		if(Char == '_' || Char == '\'') continue;
		const int Lower = std::tolower(static_cast<unsigned char>(Char));
		const uint32_t Digit = std::isdigit(static_cast<unsigned char>(Char)) ?
		static_cast<uint32_t>(Char - '0') :
		Lower >= 'a' && Lower <= 'f' ? static_cast<uint32_t>(Lower - 'a' + 10) : Base;
		if(Digit >= Base) break;
		// ６４ビットに収まらない値の返戻
		if(Value > (UINT64_MAX - Digit) / Base) return std::nullopt;
		Value = Value * Base + Digit;
	}
	Suffix = Text.substr(Pos);
	// 値の返戻
	return Value;
}

/**
 * 型の字面の数値の区分の取得関数
 * 環境で幅の変わる型（C 系の `long` / `size_t`，Go の `int`，Rust の `isize`，Swift の `Int` 等）は最も狭い幅とする
 * @param Text 型の字面
 * @return 数値の区分（数値の型でなければ未知）
 */
LintAnalyzer::NumericType LintAnalyzer::NumericTypeOf(const std::string_view Text) {
	// 型名から数値種別と幅の分類
	NumericType Type;
	Type.IsKnown = true;
	if(std::ranges::find(Rules.FloatingTypeNames, Text) != Rules.FloatingTypeNames.end()) Type.IsFloat = true;
	else if(std::ranges::find(Rules.CharacterTypeNames, Text) != Rules.CharacterTypeNames.end()) Type.IsCharacter = true;
	// `byte` は Java だけ符号付，`long` は Java・C# だけ６４ビット
	else if(Syntax.Rules().IsSpelling(Text, Word::SignedByteType)) {
		Type.Bits = 8;
		Type.IsSigned = Rules.ByteIsSigned;
	} else if(Syntax.Rules().IsSpelling(Text, Word::LongIntegerType)) Type.Bits = Rules.LongBits;
	else if(
		const std::vector<Core::SyntaxRules::IntegerTypeName>::const_iterator Found = std::find_if(
			Rules.IntegerTypeNames.begin(),
			Rules.IntegerTypeNames.end(),
			[Text](const Core::SyntaxRules::IntegerTypeName &Candidate) -> bool {
				// 名前の一致の返戻
				return Candidate.Name == Text;
			}
		); Found != Rules.IntegerTypeNames.end()
	) {
		Type.Bits = Found->Bits;
		Type.IsSigned = Found->IsSigned;
	} else Type.IsKnown = false;
	// 数値の区分の返戻
	return Type;
}

/**
 * 局所の名前の宣言の型の字面の取得関数
 * 使う位置を含む最も内の関数の中で，使う位置より前の仮引数・局所変数の宣言を探す
 * 計算量：関数の節点数 N に対し O(N)
 * @param Source ソース文字列
 * @param Use 名前を使う節点
 * @return 最も後の宣言の型の字面（宣言か型が見付からなければ空）
 */
std::string_view LintAnalyzer::DeclaredTypeText(const std::string &Source, const NodeId Use) {
	const std::string_view Name = NodeView(Source, Use);
	NodeId Function = Syntax.ParentOf(Use);
	while(Function != NoNode && !Syntax.Is(Function, Role::FunctionScope)) Function = Syntax.ParentOf(Function);
	// 関数の外の名前の返戻
	if(Function == NoNode) return {};
	std::string_view Found;
	const uint32_t Before = BeginOf(Use);
	WalkChildrenCursor(
		Function,
		[&](const NodeId Node, const std::span<const NodeId> Ancestors) -> bool {
			// 使う位置以後へ降りない事の返戻
			if(BeginOf(Node) >= Before) return false;
			// 同じ名前でない節点の中を辿る事の返戻
			if(!Syntax.Is(Node, Role::Identifier) && !Syntax.Is(Node, Role::SimpleName) || NodeView(Source, Node) != Name) return true;
			// 宣言子・パターンを介した宣言の名前か
			size_t Level = Ancestors.size();
			if(Level && Syntax.Is(Ancestors[Level - 1], Role::NameDeclarator)) --Level;
			// 宣言の名前でない事の返戻
			if(!Level || !Syntax.Is(Ancestors[Level - 1], Role::DeclarationWithType)) return false;
			Core::ForEachNamedChildOf(
				Syntax,
				Ancestors[Level - 1],
				[&](const NodeId Child) -> bool {
					// Swift の型注釈は其の中の型の名前
					const NodeId Candidate = Syntax.Is(Child, Role::TypeAnnotation) ? Syntax.NamedChild(Child, 0) : Child;
					// 型の名前でない子を辿る事の返戻
					if(!Syntax.Is(Candidate, Role::TypeNameNode)) return true;
					Found = NodeView(Source, Candidate);
					// 型の名前で打ち切る事の返戻
					return false;
				}
			);
			// 名前の中へ降りない事の返戻
			return false;
		}
	);
	// 宣言の型の字面の返戻
	return Found;
}

/**
 * 整数リテラル境界比較検出関数（全言語）
 * 整数と保証出来ない値，境界値の加減算の桁溢れ，文字コードの境界を候補から除く
 * @param Source ソース文字列
 * @param Node 対象ノード
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckCmpBoundary(const std::string &Source, const NodeId Node, std::vector<LintWarning> &Out) {
	// 比較の両辺と演算子の抽出状態初期化
	NodeId Lhs = NoNode, Rhs = NoNode;
	std::string_view Operator;
	uint32_t NamedSeen = 0;
	Core::ForEachChildOf(
		Syntax,
		Node,
		// 比較の両辺と演算子の抽出関数
		[&](const NodeId Child) -> void {
			// 利用者定義の演算子の名前を，無名の字句と同じく演算子として確認
			if(Syntax.IsNamed(Child) && !Syntax.Is(Child, Role::UserDefinedOperator)) {
				if(++NamedSeen == 1) Lhs = Child;
				else if(NamedSeen == 2) Rhs = Child;
			} else if(
				const std::string_view Tok = NodeView(Source, Child);
				Tok == Rules.Operators.GreaterEqual || Tok == Rules.Operators.LessEqual
			) Operator = Tok;
		}
	);
	// 比較演算子が無いか右辺が整数のリテラルでない場合の終了
	if(Operator.empty() || Rhs == NoNode || !Syntax.Is(Rhs, Role::IntegerLiteralCandidate)) return;
	const std::string_view RhsText = NodeView(Source, Rhs);
	// 浮動小数も表せる数値リテラルの字面の除外
	if(IsFloatLiteral(RhsText)) return;
	std::string_view Suffix;
	const std::optional<uint64_t> Value = IntegerLiteralValue(RhsText, Suffix);
	// ６４ビットを超える値の返戻
	if(!Value) return;
	NumericType Type;
	if(Rules.RequiresKnownInteger) {
		const NodeId CalleeNode = Syntax.FieldChild(Lhs, Rules.CountNameRole);
		const std::string_view Callee = CalleeNode == NoNode ? std::string_view() : NodeView(Source, CalleeNode);
		// 既知の個数を返す呼出でない場合の終了
		if(!Syntax.Is(Lhs, Role::CountExpression) || std::ranges::find(Rules.CountNames, Callee) == Rules.CountNames.end()) return;
		Type.IsKnown = true;
		Type.Bits = 32;
	} else if(Syntax.Is(Lhs, Role::Identifier) || Syntax.Is(Lhs, Role::SimpleName)) {
		Type = NumericTypeOf(DeclaredTypeText(Source, Lhs));
	}
	// 浮動小数点と文字コードの境界の返戻
	if(Type.IsFloat || Type.IsCharacter) return;
	if(Operator == Rules.Operators.GreaterEqual) {
		// 符号無か型の分からない値の `>= 0` は `> -1` に出来ない事の返戻
		if(!*Value && !(Type.IsKnown && Type.IsSigned)) return;
	} else {
		// 型の上限（６４ビット以上の符号無と１２８ビットの型は６４ビットの値の上限）
		const auto MaxOf = [](const NumericType &Of) -> uint64_t {
			// 符号の有無と幅に依る上限の返戻
			return Of.Bits > 64 || Of.Bits == 64 && !Of.IsSigned ?
			UINT64_MAX :
			Of.IsSigned ? (uint64_t{ 1 } << Of.Bits - 1) - 1 : (uint64_t{ 1 } << Of.Bits) - 1;
		};
		// 左辺の型の上限で `N+1` が溢れる事の返戻
		if(Type.IsKnown && Type.Bits < 128 && *Value >= MaxOf(Type)) return;
		// 型の分からない左辺で何れかの整数型の上限の値の返戻
		if(!Type.IsKnown && *Value > 126 && !(*Value & *Value + 1)) return;
		// リテラル自身の型（Java の `L` の無い物は int，Rust の接尾辞の型）で `N+1` が溢れる事の返戻
		NumericType Literal;
		if(!Rules.DefaultIntegerType.empty() && Suffix.empty()) Literal = NumericTypeOf(Rules.DefaultIntegerType);
		else if(!Rules.NumericTypeSuffixes.empty() && !Suffix.empty()) Literal = NumericTypeOf(Suffix);
		// リテラル自身の型で `N+1` が溢れる場合の終了
		if(Literal.IsKnown && Literal.Bits < 128 && *Value >= MaxOf(Literal)) return;
	}
	// 境界比較候補の警告登録
	const NodeId Token = TokenChildOf(Node, Operator);
	Push(
		Token == NoNode ? Node : Token,
		LintRule::ComparisonBoundary,
		"Review: Use a strict inequality with an equivalent adjusted boundary; verify type, value range",
		Out
	);
	// 終了
	return;
}

/**
 * 変数の文字列文脈推定判定関数
 * @param Source ソース文字列
 * @param AssignNode 代入式ノード
 * @param Name 判定対象の変数名
 * @param Index スコープ毎の宣言・代入の事象の索引（初めて訪れたスコープの物を此処で作り，同じ走査の後の判定で使い回す）
 * @return 文字列文脈と推定される場合 true
 */
bool LintAnalyzer::LooksLikeStringVariable(
	const std::string &Source,
	const NodeId AssignNode,
	const std::string_view Name,
	StringEventIndex &Index
) {
	// 名前の無い左辺を文字列の変数と見ない事の返戻
	if(Name.empty()) return false;
	// 型ノードのテキストが `String` / `string` なら true（Java / C# / TS の文字列型）
	const auto IsStringTypeText = [this](const std::string_view TypeText) -> bool {
		// 文法で定める文字列型との一致の返戻
		return Syntax.Rules().IsSpelling(TypeText, Word::StringType);
	};
	// スコープ内の文字列事象索引の構築関数
	const auto BuildEvents = [this, &Source, &IsStringTypeText](const NodeId Scope) -> StringEventIndex::mapped_type {
		// 事象収集先の初期化
		std::unordered_map<std::string_view, std::vector<std::pair<uint32_t, bool>>> Events;
		std::unordered_map<std::string_view, uint32_t> FirstPos;
		// 走査節点の事象収集関数
		const auto Visit = [&](const NodeId VisitNode) -> bool {
			// 別の関数の内側へ降りない事の返戻
			if(Syntax.Is(VisitNode, Role::FunctionScope)) return false;
			if(Syntax.Is(VisitNode, Role::VariableDeclarationStatement)) {
				// 宣言内の識別子と文字列初期化位置の収集
				FirstPos.clear();
				// 直近文字列の終端位置
				uint32_t LastString = 0;
				bool IsAnyString = false, IsStringType = false;
				Core::WalkNodes(
					Syntax,
					VisitNode,
					[&](const NodeId Cur) -> Core::WalkStep {
						// 識別名の初出位置・文字列・文字列型の分類
						if(Syntax.Is(Cur, Role::IdentifierLeafLike)) FirstPos.try_emplace(NodeView(Source, Cur), BeginOf(Cur));
						else if(Syntax.Is(Cur, Role::StringLikeAll)) {
							IsAnyString = true;
							LastString = BeginOf(Cur);
						} else if(Syntax.Is(Cur, Role::SimpleTypeLeaf) && IsStringTypeText(NodeView(Source, Cur))) IsStringType = true;
						// 子へ降りる事の返戻
						return Core::WalkStep::Descend;
					}
				);
				// 宣言名毎の文字列文脈の登録
				for(const auto &[Declared, Pos] : FirstPos) {
					Events[Declared].push_back({ BeginOf(VisitNode), IsStringType || IsAnyString && LastString > Pos });
				}
				// 宣言の内側へ降りない事の返戻
				return false;
			}
			// 代入事象の収集
			if(Syntax.Is(VisitNode, Role::AssignmentOperation)) if(const NodeId Lhs = Syntax.NamedChild(VisitNode, 0); Lhs != NoNode) {
				if(const std::string_view Assigned = UnwrapAssignableName(Source, Lhs); !Assigned.empty()) {
					const NodeId Rhs = Syntax.NamedChild(VisitNode, 1);
					Events[Assigned].push_back({ BeginOf(VisitNode), Rhs != NoNode && Syntax.Is(Rhs, Role::StringLikeAll) });
					// 代入の内側へ降りない事の返戻
					return false;
				}
			}
			// 子孫へ降りる事の返戻
			return true;
		};
		// 直下の子からの事象走査
		Core::ForEachNamedChildOf(
			Syntax,
			Scope,
			[this, &Visit](const NodeId Sib) -> void {
				if(Visit(Sib)) WalkChildrenCursor(Sib, Visit);
			}
		);
		// 名前毎の事象の返戻
		return Events;
	};
	// 関数／メソッドの仮引数コンテナを部分木から探索
	const auto FindParamContainer = [this](const NodeId Func) -> NodeId {
		// 探索結果の初期化
		NodeId Result = NoNode;
		// 仮引数容器までの走査関数
		const auto Visit = [this, &Result](const NodeId Cur) -> bool {
			// 発見済なら降りない事の返戻
			if(Result != NoNode) return false;
			// 仮引数容器の採用
			if(Syntax.Is(Cur, Role::ParameterContainer)) {
				Result = Cur;
				// 発見した事の返戻
				return false;
			}
			// 本体系以外へ降りる事の返戻
			return !Syntax.Is(Cur, Role::FunctionBodyContainer);
		};
		if(Func != NoNode && Visit(Func)) WalkChildrenCursor(Func, Visit);
		// 部分木走査結果の返戻
		return Result;
	};
	// 同名仮引数の文字列型判定関数
	const auto FindParameterEvent =
	[this, &Source, &Name, &IsStringTypeText, &FindParamContainer](const NodeId Func, bool &OutIsString) -> bool {
		// 仮引数節点の列
		const NodeId Params = FindParamContainer(Func);
		// 仮引数の並びが無い事の返戻
		if(Params == NoNode) return false;
		// 仮引数列の名前一致探索
		bool IsFound = false;
		Core::ForEachNamedChildOf(
			Syntax,
			Params,
			[&](const NodeId Param) -> bool {
				std::string_view ParamName, ParamType;
				// JS の型注釈無等の単純な識別子仮引数
				if(Syntax.Is(Param, Role::IdentifierLeafLike)) ParamName = NodeView(Source, Param);
				else if(Syntax.Is(Param, Role::NamedParameterKind)) {
					Core::ForEachNamedChildOf(
						Syntax,
						Param,
						[&](const NodeId Sub) -> void {
							// 言語別の型包みの解決
							if(Syntax.Is(Sub, Role::IdentifierLeafLike) && ParamName.empty()) ParamName = NodeView(Source, Sub);
							else if(Syntax.Is(Sub, Role::SimpleTypeLeaf) && ParamType.empty()) ParamType = NodeView(Source, Sub);
							else if(Syntax.Is(Sub, Role::UserType)) {
								Core::ForEachChildOf(
									Syntax,
									Sub,
									// Kotlin 型識別子の採用関数
									[&](const NodeId GrandChild) -> void {
										if(Syntax.Is(GrandChild, Role::TypeName) && ParamType.empty()) ParamType = NodeView(Source, GrandChild);
									}
								);
							} else if(Syntax.Is(Sub, Role::TypeAnnotation)) {
								Core::ForEachChildOf(
									Syntax,
									Sub,
									// TypeScript 型注釈の採用関数
									[&](const NodeId GrandChild) -> void {
										if(Syntax.Is(GrandChild, Role::SimpleTypeLeaf) && ParamType.empty()) ParamType = NodeView(Source, GrandChild);
									}
								);
							}
						}
					);
				}
				// 同名の仮引数が持つ型情報の採用
				if(ParamName == Name) {
					IsFound = true;
					OutIsString = IsStringTypeText(ParamType);
				}
				// 対象仮引数発見で走査打切，未発見なら次の仮引数への返戻
				return !IsFound;
			}
		);
		// 対象名一致の仮引数発見有無の返戻
		return IsFound;
	};
	// 祖先の範囲に在る代入前の直近事象の探索
	bool HasEvent = false, IsLatestString = false;
	NodeId Cur = AssignNode;
	while(true) {
		// 対象の親節点
		const NodeId Parent = Syntax.ParentOf(Cur);
		if(Parent == NoNode) break;
		if(Syntax.Is(Parent, Role::ScopeContainer)) {
			StringEventIndex::iterator Scope = Index.find(Parent);
			// 未調査の範囲だけの事象索引の構築
			if(Scope == Index.end()) Scope = Index.emplace(Parent, BuildEvents(Parent)).first;
			// 代入を含む子より前の兄弟の事象（位置が其の子の始まりより前の物）の直近
			if(
				const std::unordered_map<std::string_view, std::vector<std::pair<uint32_t, bool>>>::const_iterator Named =
				Scope->second.find(Name);
				Named != Scope->second.end()
			) {
				// 引数の使用履歴
				const std::vector<std::pair<uint32_t, bool>> &Events = Named->second;
				const std::vector<std::pair<uint32_t, bool>>::const_iterator After = std::lower_bound(
					Events.begin(),
					Events.end(),
					BeginOf(Cur),
					// 利用位置より前の事象判定関数
					[](const std::pair<uint32_t, bool> &Event, const uint32_t Pos) -> bool {
						// 位置が手前かの返戻
						return Event.first < Pos;
					}
				);
				// 利用位置より手前の直近事象の採用
				if(After != Events.begin()) {
					HasEvent = true;
					IsLatestString = std::prev(After)->second;
				}
			}
			if(HasEvent) break;
		}
		// 関数／メソッド境界到達時の仮引数に依る遮蔽確認（同名仮引数有時は其の事象を追加し外側スコープ走査を停止）
		if(Syntax.Is(Parent, Role::FunctionScope)) if(bool IsParamString = false; FindParameterEvent(Parent, IsParamString)) {
			HasEvent = true;
			IsLatestString = IsParamString;
			break;
		}
		Cur = Parent;
	}
	// 直近の事象が文字列文脈なら true の返戻
	return HasEvent && IsLatestString;
}

/**
 * １の増減を増減演算子で書く言語の増分代入の検出関数
 * @param Source ソース文字列
 * @param Node 対象ノード
 * @param Index スコープ毎の宣言・代入の事象の索引（文字列の変数の推定に使い回す）
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckCompoundAssignIncrement(
	const std::string &Source,
	const NodeId Node,
	StringEventIndex &Index,
	std::vector<LintWarning> &Out
) {
	// 数値の１加減算へ言語の記法で増減を勧め，文字列結合・演算子多重定義が有る為自動書換は見送
	const NodeId Lhs = Syntax.NamedChild(Node, 0), Rhs = Syntax.NamedChild(Node, 1);
	// 左右の子が無い場合又は右辺が整数の真値の綴りでない場合の終了
	if(Lhs == NoNode || Rhs == NoNode || !Syntax.Rules().IsSpelling(NodeView(Source, Rhs), Word::IntegerTruth)) return;
	const std::string_view OpToken = FindUnnamedToken(Source, Node, Rules.Operators.AddAssign, Rules.Operators.SubtractAssign);
	// 複合代入演算子を持たない節点の場合の終了
	if(OpToken.empty()) return;
	// 代入式を包む括弧の最も外側（式値を使う代入は対象外）
	const NodeId Expression = OutermostGroup(Node);
	// 式の値を使う代入の場合の終了
	if(!IsValueUnusedContext(Expression, Syntax.ParentOf(Expression)) && !Syntax.Is(Node, Role::ValueDiscardingAssignment)) return;
	// 左辺の単純識別子が近傍で文字列リテラルに初期化した変数で，文字列連結と見て警告を抑える場合の終了
	if(
		const std::string_view LhsName = UnwrapAssignableName(Source, Lhs);
		!LhsName.empty() && LooksLikeStringVariable(Source, Node, LhsName, Index)
	) return;
	const NodeId Token = TokenChildOf(Node, OpToken);
	Push(
		Token == NoNode ? Node : Token,
		LintRule::UnitAssignment,
		"Review: Use the increment or decrement form required for this target; verify type, evaluation",
		Out
	);
	// 終了
	return;
}

/**
 * 結果未使用の後置増減候補検出関数
 * @param Source ソース文字列
 * @param Node 対象更新式
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckUnusedPostfixIncrement(const std::string &Source, const NodeId Node, std::vector<LintWarning> &Out) {
	// 被演算子の取得
	const NodeId Operand = Syntax.NamedChild(Node, 0);
	// 被演算子が無い場合の終了
	if(Operand == NoNode) return;
	// 後置演算子の探索
	std::string_view Operator;
	NodeId OperatorNode = NoNode;
	uint32_t OperatorStart = 0;
	Core::ForEachChildOf(
		Syntax,
		Node,
		[&](const NodeId Child) -> bool {
			// 名前付の子を読み飛ばす事の返戻
			if(Syntax.IsNamed(Child)) return true;
			// 名前無字句の抽出
			const std::string_view Token = NodeView(Source, Child);
			// 増減以外の字句を読み飛ばす事の返戻
			if(Token != Rules.Operators.Increment && Token != Rules.Operators.Decrement) return true;
			// 演算子と開始位置の記録
			Operator = Token;
			OperatorNode = Child;
			OperatorStart = BeginOf(Child);
			// 更新演算子発見で走査打切の返戻
			return false;
		}
	);
	// 後置の演算子が無い場合の終了
	if(Operator.empty() || OperatorStart < EndOf(Operand)) return;
	// 増減式を包む括弧の最も外側
	const NodeId Expression = OutermostGroup(Node);
	// 式の値を使う増減の場合の終了
	if(!IsValueUnusedContext(Expression, Syntax.ParentOf(Expression))) return;
	// 後置増減候補の警告登録
	Push(
		OperatorNode,
		LintRule::UnusedPostfix,
		"Review: Use prefix increment or decrement when the old value is unused; verify type, evaluation",
		Out
	);
	// 終了
	return;
}

/**
 * 冗長な二重符号 (`-(-x)` / `+(+x)`) 検出関数
 * @param Source ソース文字列
 * @param Node 前置単項ノード
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckRedundantDoubleSign(const std::string &Source, const NodeId Node, std::vector<LintWarning> &Out) {
	const NodeId Sign = Syntax.Child(Node, 0);
	// 演算子が１文字の `-`/`+` 以外（`*` `&` `!` `~` `--` 等）の場合の終了
	if(Syntax.IsNamed(Sign) || EndOf(Sign) != BeginOf(Sign) + 1) return;
	const char Operator = Source[BeginOf(Sign)];
	// 符号でない演算子の場合の終了
	if(Operator != '-' && Operator != '+') return;
	// 符号の融合を避ける括弧 (`-(-x)`) の内側を被演算子として確認
	NodeId Operand = Syntax.NamedChild(Node, 0);
	const bool IsWrapped = Operand != NoNode && Syntax.Is(Operand, Role::GroupingParen) && Syntax.NamedChildCount(Operand) == 1;
	if(IsWrapped) Operand = Syntax.NamedChild(Operand, 0);
	// 被演算子が前置単項でなく冗長でない場合の終了（`-a` / `-(a + b)` 等）
	if(Operand == NoNode || !Syntax.Is(Operand, Role::PrefixUnary)) return;
	if(
		const NodeId Inner = Syntax.Child(Operand, 0);
		!Syntax.IsNamed(Inner) && EndOf(Inner) == BeginOf(Inner) + 1 && Source[BeginOf(Inner)] == Operator
	) Push(Sign, LintRule::RedundantUnary, "Review: Simplify the redundant unary signs; verify type, evaluation", Out);
	// 終了
	return;
}

/**
 * ネストの深さの検査関数
 * @param Source 整形済の文面
 * @param Root 走査の起点
 * @param Out 警告格納先
 */
void LintAnalyzer::CheckNestingDepth(const std::string &Source, const NodeId Root, std::vector<LintWarning> &Out) {
	// 段数は整形後のインデントで数え，上限は其の表示幅で決定
	const size_t Limit = Rules.NestingLimit;
	bool IsPrevDeep = false;
	for(size_t Pos = 0, Row = 0; Pos < Source.size(); ++Row) {
		const size_t End = std::min(Source.find('\n', Pos), Source.size());
		size_t Depth = 0;
		while(Pos + Depth < End && Source[Pos + Depth] == '\t') ++Depth;
		// 上限を超えた行と，深い塊が続いて居る間の行だけ節点を引く（其の他の行は判定に非関与）
		if(Depth > Limit || IsPrevDeep) {
			// 複数行字句内の行頭タブのインデント計数からの除外
			const uint32_t Before = Pos ? static_cast<uint32_t>(Pos - 1) : 0, At = static_cast<uint32_t>(Pos + Depth);
			const NodeId Outer = DescendantAt(Root, Before, At);
			// 字句の内側の行は構文の段を成さず，深い塊の切れ目にも回避
			if(
				const bool IsInsideToken =
				Pos && PointOf(Outer).Row < Row && (!Syntax.ChildCount(Outer) || Syntax.Is(Outer, Role::LiteralInterior));
				!IsInsideToken
			) {
				// 同じ塊の続きは同じ違反の繰返に為る為，上限を超え始めた行だけを報告
				if(Depth > Limit && !IsPrevDeep) {
					Push(
						DescendantAt(Root, At, At),
						LintRule::NestingDepth,
						"Violation: Nesting depth " + std::to_string(Depth) + " exceeds the limit of " + std::to_string(Limit),
						Out
					);
				}
				IsPrevDeep = Depth > Limit;
			}
		}
		Pos = End + 1;
	}
	// 終了
	return;
}

/** ========== 検査実行 ========== */
/**
 * 違反警告リスト返戻関数
 * 計算量：節点数 V，原稿長 N，コメント数 K，個別検査の総費用 F に対し O(V + N + K log K + F)
 * @return 検出された違反警告リスト（検査の順）
 */
std::vector<LintWarning> LintAnalyzer::Run() {
	std::vector<LintWarning> Out;
	const NodeId Root = Syntax.GetRoot();
	// 空の構文に対する空の警告列の返戻
	if(Root == NoNode) return Out;
	// 整形済の字下げの深さの検査
	CheckNestingDepth(Source, Root, Out);
	const std::vector<uint32_t> &DirectiveStarts = Facts.DirectiveStarts;
	// 最終字面のコメントの範囲（原文順）
	std::vector<std::pair<uint32_t, uint32_t>> Comments;
	Comments.reserve(Facts.Comments.size());
	for(const StaticLint::CommentFacts &Comment : Facts.Comments) Comments.emplace_back(Comment.Range.Start, Comment.Range.End);
	// 更新される変数名の収集
	std::unordered_set<std::string> MutatedNames;
	Core::WalkNodes(
		Syntax,
		Root,
		[&](const NodeId Node) -> Core::WalkStep {
			if(Syntax.Is(Node, Role::MutableArgumentList)) {
				Core::ForEachNamedChildOf(
					Syntax,
					Node,
					[&](const NodeId Argument) -> void {
						if(const std::string_view Name = UnwrapAssignableName(Source, Argument); !Name.empty()) MutatedNames.emplace(Name);
					}
				);
				// 実引数の子へ降りる事の返戻
				return Core::WalkStep::Descend;
			}
			// 値を変えない節点の子へ降りる事の返戻
			if(
				!Syntax.Is(Node, Role::MutatingExpression) &&
				!(Syntax.Is(Node, Role::AddressOperation) && NodeView(Source, Node).starts_with('&'))
			) return Core::WalkStep::Descend;
			const NodeId Operand = Syntax.NamedChild(Node, 0);
			// 被演算子の無い節点の子へ降りる事の返戻
			if(Operand == NoNode) return Core::WalkStep::Descend;
			if(const std::string_view Name = UnwrapAssignableName(Source, Operand); !Name.empty()) MutatedNames.emplace(Name);
			// 子へ降りる事の返戻
			return Core::WalkStep::Descend;
		}
	);
	StringEventIndex Index;
	CondContextIndex Conditions;
	LabelTargetIndex Labels;
	// 節点の役割別の規約検査
	Core::WalkNodes(
		Syntax,
		Root,
		[&](const NodeId Node) -> Core::WalkStep {
			// 名前の無い字句の読飛の返戻
			if(!Syntax.IsNamed(Node)) return Core::WalkStep::Descend;
			if(Syntax.Is(Node, Role::ZeroConditionHost)) MarkCondContext(Source, Node, Conditions);
			if(Syntax.Is(Node, Role::OrdinaryFunctionDefinition)) CheckMissingExplicitReturn(Node, Out);
			if(Syntax.Is(Node, Role::OptionalDeclarationType)) CheckDeclarationType(Node, Out);
			if(Syntax.Is(Node, Role::OptionalParameterType)) CheckParameterType(Node, Out);
			if(Syntax.Is(Node, Role::AccessOrderedClass)) CheckAccessSpecifierOrder(Source, Node, Out);
			else if(Syntax.Is(Node, Role::GotoInspection) || Syntax.Is(Node, Role::LabelInspection)) {
				CheckGotoOrLabel(Source, Node, Labels, Out);
			} else if(Syntax.Is(Node, Role::FunctionBodyContainer)) CheckUselessLocalVariable(Source, Node, Out);
			else if(Syntax.Is(Node, Role::TypeInferenceInspection)) CheckTypeInferenceKeyword(Node, Out);
			else if(Syntax.Is(Node, Role::NamedFunctionDefinition)) {
				CheckFunctionNamingConvention(Source, Node, Out);
				if(Syntax.Is(Node, Role::ReturnHintRequired)) CheckReturnTypeHint(Node, Out);
			} else if(Syntax.Is(Node, Role::ClassBody)) CheckMemberDeclarationOrder(Node, Out);
			else if(Syntax.Is(Node, Role::LeadingConstDeclaration)) {
				if(Syntax.Is(Node, Role::ConstexprInspection)) CheckConstexprCandidate(Node, Out);
				CheckConstBindingCandidate(Source, Node, MutatedNames, Out);
				CheckFloatSuffixCandidate(Source, Node, Out);
			} else if(Syntax.Is(Node, Role::MacroConstantInspection)) CheckMacroConstant(Node, Out);
			else if(Syntax.Is(Node, Role::ScopeImport)) CheckUsingNamespace(Source, Node, Out);
			else if(Core::ControlFlow::IsReturn(Syntax, Node)) CheckReturnCommentPresence(Source, Node, Comments, Out);
			else if(Syntax.Is(Node, Role::BinaryOperation)) {
				if(Rules.AllowsNumericTruthiness && Syntax.Is(Node, Role::ZeroComparisonInspection)) {
					CheckZeroComparisonCondition(Source, Node, Conditions, Out);
				}
				if(Syntax.Is(Node, Role::StrictEqualityInspection)) {
					CheckNumberStrictEquality(Source, Node, Conditions, Out);
					CheckNullishComparisonCandidate(Source, Node, Out);
				}
			} else if(Syntax.Is(Node, Role::CastInspection)) CheckCStyleCast(Node, Out);
			else if(Syntax.Is(Node, Role::NullPointerInspection)) CheckNullMacroCandidate(Node, Out);
			else if(Syntax.Is(Node, Role::ConditionMergeInspection)) CheckUnsafeIfMergeCandidate(Source, Node, Out);
			else if(Rules.PrefersBooleanXorAssignment && Syntax.Is(Node, Role::BoolFlipInspection)) CheckBoolFlipPattern(Source, Node, Out);
			if(
				Rules.AllowsNumericTruthiness && Syntax.Is(Node, Role::ZeroComparisonInspection) && !Syntax.Is(Node, Role::BinaryOperation)
			) CheckZeroComparisonCondition(Source, Node, Conditions, Out);
			if(Syntax.Is(Node, Role::DocumentedDefinition)) {
				const StaticLint::FunctionFacts &Function = Facts.Functions.at(Node);
				const bool IsDocumentable = Function.IsDocumentable;
				// 錨の手前に続くコメントの本文（文書の有無と項目の検査が共有し，文書文字列の言語と項目を検査しない解析失敗の定義では集めない）
				const std::vector<std::string_view> Leading =
				Rules.Notation.Documentation != Core::DocumentationStyle::Docstring && (IsDocumentable || !HasError(Node)) ?
				LeadingCommentTexts(Source, Function.DocumentAnchor, DirectiveStarts) :
				std::vector<std::string_view>();
				if(IsDocumentable) CheckDocCommentPresence(Source, Node, Leading, Out);
				CheckDocCommentCompleteness(Source, Node, Leading, Out);
			}
			// 数値演算と単項式の候補検査
			if(Syntax.Is(Node, Role::BoundaryComparison)) CheckCmpBoundary(Source, Node, Out);
			if(Rules.PrefersUnitUpdate && Syntax.Is(Node, Role::IncrementInspection)) {
				CheckCompoundAssignIncrement(Source, Node, Index, Out);
			}
			if(Rules.PrefersPrefixUpdate && Syntax.Is(Node, Role::PostfixInspection)) CheckUnusedPostfixIncrement(Source, Node, Out);
			if(Syntax.Is(Node, Role::PrefixUnary)) CheckRedundantDoubleSign(Source, Node, Out);
			// 子へ降りる事の返戻
			return Core::WalkStep::Descend;
		}
	);
	// 最終字面のコメントの検査（原文順，文区切は正規化と同じコード例の判定を用いる）
	const CommentStructure Structure = CommentNormalizer::Inspect(ReadOnlyCommentView(Repository.GetRecords()));
	for(size_t Index = 0; Index < Facts.Comments.size(); ++Index) {
		CheckStandardsReference(Index, Structure, Out);
		CheckDocCommentForm(Source, Index, Out);
		CheckCommentSentences(Index, Structure, DirectiveStarts, Out);
	}
	// 検査の順の警告の返戻（位置順の整列は部分整形の通知と合わせて整形器が行う）
	return Out;
}

/**
 * 検証済の出力の検査資料の取得関数（整形仕様 8.4）
 * 検証が一度だけ解析・投影した最終構文・コメント・診断位置を読み，関数の署名と到達可能性の共通解析を求める（解析・投影し直さない）
 * @param Output 検証を通った最終構文とコメント・診断位置
 * @return 検査資料
 */
StaticLint::OutputFacts LintAnalyzer::ReadFacts(const Core::ValidatedOutput &Output) {
	// 最終構文と診断位置の資料の取込
	StaticLint::OutputFacts Facts;
	const Core::SyntaxModel &Syntax = Output.FinalSyntax;
	const Core::SyntaxRules &Rules = Syntax.Rules();
	const Core::DiagnosticLocationMap &Locations = Output.Locations;
	Facts.Positions = Locations.Nodes;
	Facts.DirectiveStarts = Locations.DirectiveStarts;
	// 最終字面のコメントの位置と記録の対応（逐語区画に残る物も含め原文順）
	const std::span<const CommentRecord> Records = Output.FinalComments.GetRecords();
	for(size_t Index = 0; Index < Locations.Comments.size(); ++Index) {
		const Core::OutputRange &Range = Locations.Comments[Index];
		const bool IsAttribute = Core::CommentRepository::IsDocumentAttribute(Rules.Notation, Records[Index].Notation);
		Facts.Comments.push_back({ Range, Index, IsAttribute, Locations.Text.substr(Range.Start, Range.End - Range.Start) });
	}
	const Core::ControlFlow Flow(Syntax);
	// 展開前の属性の範囲の判定の控え（関数毎の返戻の補完の前提の判定で共有する）
	std::unordered_map<Core::NodeId, bool> AttributeProtection;
	Core::WalkNodes(
		Syntax,
		Syntax.GetRoot(),
		[&](const Core::NodeId Id) -> Core::WalkStep {
			// 関数の署名と到達可能性の共通解析
			const bool IsDocumented = Syntax.Is(Id, Core::Role::DocumentedDefinition);
			if(const bool IsOrdinary = Syntax.Is(Id, Core::Role::OrdinaryFunctionDefinition); IsDocumented || IsOrdinary) {
				StaticLint::FunctionFacts Function;
				Function.IsDocumentable = Core::SignatureExtractor::IsDocumentable(Syntax, Id);
				Function.DocumentAnchor = Core::SignatureExtractor::DocAnchor(Syntax, Id);
				if(IsDocumented) {
					Function.Signature = Core::SignatureExtractor::Extract(Syntax, Id);
					// 文書属性を持つ前置の節点の探索
					if(Rules.Notation.DocumentCommentIsAttribute) {
						for(Core::NodeId Prev = Syntax.PrevNamedSibling(Id); Prev != Core::NoNode; Prev = Syntax.PrevNamedSibling(Prev)) {
							if(!Syntax.Is(Prev, Core::Role::OuterExpansionAttribute)) break;
							const std::string Attribute = Syntax.Text(Prev);
							std::string_view Inner = Attribute;
							if(!Inner.starts_with(Rules.Notation.DocumentAttributeOpen)) continue;
							Inner.remove_prefix(Rules.Notation.DocumentAttributeOpen.size());
							while(!Inner.empty() && std::isspace(static_cast<unsigned char>(Inner.front()))) Inner.remove_prefix(1);
							const std::string_view DocumentName = Syntax.Rules().Spelling(Word::DocumentAttributeName);
							Function.HasDocumentAttribute = Function.HasDocumentAttribute || Inner.starts_with(DocumentName) && (
								Inner.size() == DocumentName.size() ||
								!std::isalnum(static_cast<unsigned char>(Inner[DocumentName.size()])) && Inner[DocumentName.size()] != '_'
							);
						}
					}
				}
				if(IsOrdinary) {
					if(const std::optional<Core::FunctionEnd> End = Flow.ClassifyFunction(Id); End && End->Kind != Core::ReturnKind::Never) {
						Function.UnreachableReturningStatements =
						Flow.ReturningStatementsAfterTermination(End->Statements, Flow.CollectGotoTargets(End->Statements));
						// 末尾の補完と同じ到達可能性の判定（確実に到達する物は不足，確かめれない物は要確認，整形仕様 5.2, 8.4）
						const Core::EndReachability Reach = Flow.EndReach(Id, *End);
						// 補完と同じ前提の判定（前提を確かめれない物は要確認）
						const bool IsUncertain = Flow.IsReturnCompletionUncertain(Id, *End, AttributeProtection);
						Function.IsMissingReturn = Reach == Core::EndReachability::Reachable && !IsUncertain;
						Function.IsReturnUncertain =
						Reach == Core::EndReachability::Unknown || Reach == Core::EndReachability::Reachable && IsUncertain;
					}
				}
				Facts.Functions.emplace(Id, std::move(Function));
			}
			// 子の投影資料も読む指定の返戻
			return Core::WalkStep::Descend;
		}
	);
	// 検査資料の返戻
	return Facts;
}

/**
 * 検証済の出力の規約検査関数（整形仕様 3.4 の Analyze，8.4）
 * 検証が一度だけ解析・投影した最終構文・コメント・診断位置を読み，解析・投影し直さずに規約違反を求める
 * @param Output 検証を通った最終構文・コメント・診断位置
 * @return 検査の順の違反警告リスト（位置順の整列は部分整形の通知と合わせて整形器が行う，整形仕様 8.4, 10.4）
 */
std::vector<LintWarning> LintAnalyzer::Analyze(const Core::ValidatedOutput &Output) {
	// 検査資料を読むだけの検査結果の返戻
	return LintAnalyzer(Output.FinalSyntax, Output.Locations.Text, Output.FinalComments, ReadFacts(Output)).Run();
}
