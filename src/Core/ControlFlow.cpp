#include "ControlFlow.hpp"
#include "../Util/CharClass.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include "../Util/WorkSort.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 終端の判定結果のキーの生成関数
	 * @param Node 判定したノード
	 * @param Scope 内部転送を認める switch 文の判定範囲
	 * @return ノードと判定範囲の組のキー
	 */
	uint64_t ControlFlow::TerminationKey(const NodeId Node, const NodeId Scope) {
		// 上位にノード，下位に判定範囲を置いたキーの返戻
		return uint64_t{ Node } << 32 | Scope;
	}

	/**
	 * 再使用する終端表の照会関数
	 * 計算量：期待値 O(1)
	 * @param Node 判定するノード
	 * @param Scope 判定範囲
	 * @return 登録済の要約又は無
	 */
	const ControlFlow::TerminationSummary *ControlFlow::TerminationResults::Find(const NodeId Node, const NodeId Scope) const {
		// 未使用のノードの返戻
		if(Node >= Positions.size() || Positions[Node] == NoNode) return nullptr;
		const Entry &Found = Entries[Positions[Node]];
		// 最初の判定範囲の結果の返戻
		if(Found.Scope == Scope) return &Found.Summary;
		const std::unordered_map<uint64_t, TerminationSummary>::const_iterator Other = Additional.find(TerminationKey(Node, Scope));
		// 同じノードの別の判定範囲の返戻
		return Other == Additional.end() ? nullptr : &Other->second;
	}

	/**
	 * 再使用する終端表の登録関数
	 * 計算量：償却した期待値 O(1)
	 * @param Node 判定したノード
	 * @param Scope 判定範囲
	 * @param Summary 終端の結果
	 * @throws std::bad_alloc 結果のメモリを確保出来ない場合
	 * @throws std::length_error 結果の要素数上限を超える場合
	 */
	void ControlFlow::TerminationResults::Store(const NodeId Node, const NodeId Scope, const TerminationSummary Summary) {
		// 終端表の新規登録と既存結果の更新
		if(Positions[Node] == NoNode) {
			const NodeId Position = static_cast<NodeId>(Entries.size());
			Entries.push_back({ Node, Scope, Summary });
			Positions[Node] = Position;
		} else if(Entries[Positions[Node]].Scope == Scope) Entries[Positions[Node]].Summary = Summary;
		else Additional.insert_or_assign(TerminationKey(Node, Scope), Summary);
		// 終了
		return;
	}

	/**
	 * 使用したノードだけを失効する終端表の初期化関数
	 * 計算量：使用したノードと追加の判定範囲の数 N に対し O(N)
	 * @param Syntax 作業量を記録する構文モデル
	 * @throws FormatLimitExceeded 使用位置の初期化が作業予算を超える場合
	 */
	void ControlFlow::TerminationResults::Clear(const SyntaxModel &Syntax) {
		// 使用した位置の失効
		for(const Entry &Item : Entries) {
			Syntax.Charge(1);
			Positions[Item.Node] = NoNode;
		}
		Entries.clear();
		Additional.clear();
		// 終了
		return;
	}

	/**
	 * 独立した制御フロー境界の判定関数
	 * @param Type 判定するノード
	 * @return 外側の制御フローから分離する境界なら true
	 */
	bool ControlFlow::IsFlowBoundary(const NodeId Type) const {
		// 関数・クラスの本体と Rust の非同期のブロックは独立し，Kotlin の inline ラムダからは外側の繰返へ脱出出来る事の返戻
		return Syntax.Is(Type, Role::FlowBoundary);
	}

	/**
	 * 標準の戻値型の限定名の照合関数
	 * 計算量：限定名の長さ L に対し O(L)
	 * @param Meanings 入力で確認した戻値型の意味
	 * @return 標準の供給元と，文法の無値・非同期の無値・戻らない型の語が一致すれば其の語，其の他は空
	 */
	std::optional<Word> ControlFlow::StandardResultType(const WordSet &Meanings) {
		for(const Word Meaning : { Word::VoidType, Word::AsyncVoidType, Word::NeverType }) {
			// 無値・非復帰の優先順に従う確認済の型の返戻
			if(Meanings.test(static_cast<size_t>(Meaning))) return Meaning;
		}
		// 確認出来ない型の返戻
		return std::nullopt;
	}

	/**
	 * 中核が共有する宣言の有効範囲の取得関数
	 * 計算量：所属取得の初回の探索量 H に対し O(H + 1)，再照会は O(1)
	 * @param Decl 宣言又は参照（無も可）
	 * @return 中核の最内の有効範囲（無なら無）
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc 所属の表を確保出来ない場合
	 * @throws std::length_error 所属の表の上限を超える場合
	 * @throws std::out_of_range ノードが表に無い場合
	 */
	NodeId ControlFlow::GetScopeOf(const NodeId Decl) const {
		// 同じ親子構造から確定した所属の返戻
		return Decl == NoNode ? NoNode : Syntax.GetBindingScope(Decl);
	}

	/**
	 * 戻らない事を宣言した C / C++ の関数かの判定関数
	 * 計算量：初回はノード表の長さ V，参照する指定のノード数 N とテキストの総長 L に対し O(V + N + L)，再照会は O(1)
	 * @param Node 関数のノード
	 * @param Name 宣言子を選ぶ名前（省略時は関数自身の宣言子）
	 * @return `noreturn` 属性・`_Noreturn` 指定を持てば true
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool ControlFlow::IsNoReturnDeclared(const NodeId Node, const NodeId Name) const {
		// 非復帰の指定の綴りを持たない文法は，子の探索を要しない事の返戻
		if(
			Node == NoNode || Syntax.GetRules().Words[static_cast<size_t>(Word::NoReturnSpecifier)].empty() &&
			Syntax.GetRules().Words[static_cast<size_t>(Word::NoReturnAttributeName)].empty()
		) return false;
		if(NoReturnDeclarations.size() < Syntax.GetNodeCount()) {
			Syntax.Charge(Syntax.GetNodeCount() - NoReturnDeclarations.size());
			NoReturnDeclarations.resize(Syntax.GetNodeCount());
		}
		const NodeId Key = Name == NoNode ? Node : Name;
		#if !defined(ShaveFormatRefFacts)
		if(NoReturnDeclarations.at(Key)) {
			Syntax.Charge(1);
			// 同じ宣言の指定の返戻
			return *NoReturnDeclarations[Key];
		}
		#endif
		// 宣言と宣言子の経路だけを調べ，仮引数・本体・属性の引数を契約から除く
		const bool HasNoReturn = SyntaxModel::HasStoppedTraversal(
			Syntax,
			Node,
			[&](const NodeId Part) -> WalkStep {
				if(Syntax.Is(Part, Role::ContractAttribute)) {
					const NodeId NameField = Syntax.GetFieldChild(Part, ChildRole::Name);
					const NodeId AttributeName = NameField == NoNode ? Syntax.GetNamedChild(Part, 0) : NameField;
					NodeId Qualifier = Syntax.GetFieldChild(Part, ChildRole::Qualifier);
					if(Qualifier == NoNode) {
						const NodeId Group = Syntax.GetParentOf(Part);
						const NodeId First = Syntax.GetFirstNamedChildWithRole(Group, Role::ContractAttribute);
						Qualifier = Syntax.GetFieldChild(First, ChildRole::SharedQualifier);
					}
					const bool HasArgs = SyntaxModel::HasChildMatching(
						Syntax,
						Part,
						[&](const NodeId Child) -> bool {
							// 属性の実引数の並びかの返戻
							return Syntax.Is(Child, Role::ArgSeq);
						}
					);
					// 名前空間と引数の契約を満たす属性だけの返戻
					return !HasArgs && (
						Syntax.IsWord(AttributeName, Word::NoReturnAttributeName) &&
						(Qualifier == NoNode || Syntax.IsWord(Qualifier, Word::NoReturnAttributeNamespace)) ||
						Qualifier == NoNode && Syntax.IsWord(AttributeName, Word::NoReturnSpecifier)
					) ? WalkStep::Stop : WalkStep::Skip;
				}
				if(Syntax.Is(Part, Role::ContractAttributeList)) {
					// 実引数形式でも指定の各項目の名前だけを読み，項目の引数へ降りない
					const NodeId Items = Syntax.GetNamedChild(Part, 0);
					const bool HasAttribute = SyntaxModel::HasChildMatching(
						Syntax,
						Items,
						[&](const NodeId Item) -> bool {
							// 引数を持たない属性名の返戻
							return Syntax.Is(Item, Role::Identifier) && Syntax.IsWord(Item, Word::NoReturnAttributeName);
						}
					);
					// 非復帰の項目が在るかの返戻
					return HasAttribute ? WalkStep::Stop : WalkStep::Skip;
				}
				if(Syntax.Is(Part, Role::DeclSpecifier) && !Syntax.Is(Part, Role::ContractAttributeGroup)) {
					// 属性以外の指定子の直下の語だけの照合
					return Syntax.HasWordChild(Part, Word::NoReturnSpecifier) || Syntax.HasWordChild(Part, Word::NoReturnAttributeName) ?
					WalkStep::Stop :
					WalkStep::Skip;
				}
				// 共有宣言でも後置の属性は当該宣言子だけへ適用する
				if(Syntax.HasField(Part, ChildRole::Declarator) && Name != NoNode && !Syntax.HasNodeWithin(Part, Name)) return WalkStep::Skip;
				// 宣言子と属性のコンテナだけへ降りる事の返戻
				return Part == Node || Syntax.Is(Part, Role::ContractAttributeGroup) || Syntax.HasField(Part, ChildRole::Declarator) ?
				WalkStep::Descend :
				WalkStep::Skip;
			}
		);
		NoReturnDeclarations[Key] = HasNoReturn;
		// 宣言毎に共有した指定の返戻
		return HasNoReturn;
	}

	/**
	 * 関数の戻値の型の取得関数
	 * 戻値型・結果・型の各フィールド，型を宣言子に持つ変換関数，フィールドを持たず仮引数の後に置く型の順に探す
	 * 計算量：対象と参照先の子の総数 C，照合するテキストの総長 L に対し O(C + L)
	 * @param Node 関数のノード
	 * @return 戻値の型を表すノード（型を省いた関数は無）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	NodeId ControlFlow::GetResultTypeOf(const NodeId Node) const {
		// 関数の戻値の型の取得
		for(const ChildRole Field : { ChildRole::ReturnType, ChildRole::Result, ChildRole::Type }) {
			// 戻値の型を持つフィールドの子の返戻
			if(const NodeId Type = Syntax.GetFieldChild(Node, Field); Type != NoNode) return Type;
		}
		if(const NodeId Declarator = Syntax.GetFieldChild(Node, ChildRole::Declarator); Syntax.Is(Declarator, Role::OperatorCast)) {
			// 型を宣言子の中に持つ変換関数の返戻
			return Declarator;
		}
		// 戻値の型と仮引数の後の位置の状態
		NodeId Type = NoNode;
		bool IsAfterParams = false;
		Core::SyntaxModel::ForEachNamedChildOf(
			Syntax,
			Node,
			[&](const NodeId Child) -> void {
				if(Syntax.Is(Child, Role::FunctionValParams)) IsAfterParams = true;
				else if(IsAfterParams && Type == NoNode && Syntax.Is(Child, Role::PostParamResultType)) Type = Child;
				// 終了
				return;
			}
		);
		// 仮引数の並びの後の最初の型の返戻
		return Type;
	}

	/**
	 * 分解・括弧を含む代入先のメンバ参照と指定時の名前の取得関数
	 * 受手・添字・既定値等の評価式は代入対象にしない
	 * 計算量：代入先のノード数 N に対し O(N)
	 * @param Target 代入先の根
	 * @param ShouldCollectNames 削除先の単純名も集めるか
	 * @return 記述順のメンバ参照と指定時の名前
	 * @throws FormatLimitExceeded 対象の走査が作業予算を超える場合
	 * @throws std::bad_alloc 対象又は走査列を確保出来ない場合
	 * @throws std::length_error 対象又は走査列の長さが上限を超える場合
	 */
	std::vector<NodeId> ControlFlow::GetAssignedTargets(const NodeId Target, const bool ShouldCollectNames) const {
		std::vector<NodeId> Targets;
		SyntaxModel::HasStoppedTraversal(
			Syntax,
			Target,
			[&](const NodeId Part) -> WalkStep {
				const NodeId Parent = Syntax.GetParentOf(Part);
				const BindingSyntax ParentShape = Syntax.GetBindingSyntax(Parent);
				// パターンのキーと既定値を代入先から除く事の返戻
				if(
					Part != Target && (
						ParentShape == BindingSyntax::PairPattern && Part != Syntax.GetFieldChild(Parent, ChildRole::Val) ||
						ParentShape == BindingSyntax::DefaultPattern && Part != Syntax.GetFieldChild(Parent, ChildRole::Left)
					)
				) return WalkStep::Skip;
				const bool IsMember = Syntax.Is(Part, Role::Member);
				const BindingSyntax Shape = IsMember ? BindingSyntax::None : Syntax.GetBindingSyntax(Part);
				if(IsMember || ShouldCollectNames && Shape == BindingSyntax::Name) {
					Targets.push_back(Part);
					// 受手を代入先として重ねて読まない事の返戻
					return WalkStep::Skip;
				}
				// 削除先の組とリストは，束縛パターンでなく値の構造としても現れる
				const bool IsList = Syntax.GetChildCount(Part) && Syntax.IsTokenRole(Syntax.GetChild(Part, 0), Role::OpeningBracketToken);
				// 分解と単一のラッパーだけへ降りる事の返戻
				return Shape == BindingSyntax::DefaultPattern || Shape == BindingSyntax::PairPattern || Shape == BindingSyntax::Pattern ||
				Shape == BindingSyntax::RestPattern || IsList ||
				Syntax.IsAny(Part, { Role::ExprList, Role::GroupingParen, Role::LeftAssignmentList, Role::TupleLike }) ?
				WalkStep::Descend :
				WalkStep::Skip;
			}
		);
		// 代入先だけの返戻
		return Targets;
	}

	/**
	 * 束縛の収集から呼出の供給元を登録する関数
	 * 計算量：供給値のフィールドと名前の長さ B に対し O(1 + B)
	 * @param Scope 導入範囲
	 * @param Name 導入又は変更する名前
	 * @param Site 宣言又は代入
	 * @param Names 供給元の格納先
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc 名前の表を確保出来ない場合
	 * @throws std::length_error 名前の表の上限を超える場合
	 * @throws std::out_of_range ノードが表に無い場合
	 */
	void ControlFlow::CollectBindingSource(const NodeId Scope, const NodeId Name, const NodeId Site, CallableNames &Names) const {
		// 前処理の置換は使用位置の事実で扱い，通常の供給元へ残さない事の終了
		if(Name == NoNode || !Syntax.GetNode(Name).Facts.NameIdx || Syntax.Is(Site, Role::MacroDef)) return;
		Syntax.Charge(1);
		const KindDef *const Def = Syntax.GetDefOf(Site);
		NodeId Alias = NoNode;
		if(
			Syntax.Is(Site, Role::AssignmentOperation) || Syntax.Is(Site, Role::VarDeclarator) ||
			Syntax.GetBindingSyntax(Site) == BindingSyntax::Declarator || Syntax.GetBindingSyntax(Site) == BindingSyntax::Decl
		) {
			Alias = Syntax.GetFieldChild(Site, ChildRole::Right);
			if(Alias == NoNode) Alias = Syntax.GetFieldChild(Site, Def ? Def->Bindings.ValueInput : ChildRole::Val);
		}
		NodeId Function = Alias == NoNode ? Site : NoNode;
		if(Syntax.IsBindingFunction(Site)) {
			NodeId Owner = NoNode;
			if(Syntax.HasNodeWithin(Syntax.GetBindingParams(Site, Owner), Name)) Function = NoNode;
		}
		std::vector<CallableIntroduction> &Introductions = Names[Syntax.GetCmpNameIdx(Name)];
		// 同じ供給元の連続した登録の終了
		if(!Introductions.empty() && Introductions.back().Name == Name && Introductions.back().Member.empty()) return;
		Introductions.push_back(
			{
				.Name = Name,
				.AliasSrc = Alias,
				.Function = Function,
				.IsScopeWide = Def && Def->Bindings.IsStaticItem && (
					!Syntax.GetRules().HasNamespaceFunctionImports || Scope == Syntax.GetRoot() ||
					Syntax.Is(Syntax.GetParentOf(Scope), Role::NamespaceDef)
				),
				.IsCustomDefinition = Function != NoNode && Syntax.Is(Site, Role::FunctionScope),
				.IsUncertain = Syntax.Is(Site, Role::CompoundAssignment) || Syntax.Is(Site, Role::DeleteStmt) ||
				Def && Def->Bindings.ValueInput == ChildRole::Alias,
				.IsSequential = (Function != NoNode || Alias != NoNode) && Syntax.IsWord(Name, Word::ImportedTarget) &&
				Syntax.IsWord(Name, Word::StandardTerminalDecl)
			}
		);
		// 終了
		return;
	}

	/**
	 * 束縛の収集と同じ訪問から代入・削除の供給元を登録する関数
	 * 計算量：更新先の探索量 N と修飾の長さ B に対し O(N + B)
	 * @param Scope 現在の範囲
	 * @param Node 代入又は削除
	 * @param Names 供給元の格納先
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc 更新先又は名前の表を確保出来ない場合
	 * @throws std::length_error 更新先又は名前の表の上限を超える場合
	 * @throws std::out_of_range ノードが表に無い場合
	 */
	void ControlFlow::CollectBindingChanges(const NodeId Scope, const NodeId Node, CallableNames &Names) const {
		const bool IsDelete = Syntax.Is(Node, Role::DeleteStmt);
		// 値を変更しない構文の終了
		if(!IsDelete && !Syntax.Is(Node, Role::AssignmentOperation)) return;
		const NodeId Left = Syntax.GetFieldChild(Node, ChildRole::Left);
		const std::span<const NodeId> Targets = IsDelete ? Syntax.GetChildrenOf(Node) : std::span<const NodeId>(&Left, 1);
		for(const NodeId Target : Targets) for(const NodeId Changed : GetAssignedTargets(Target, true)) {
			const CalleeSummary Summary = GetCalleeSummary(Changed);
			const NodeId Head = Summary.Head;
			if(Head == NoNode || !Syntax.GetNode(Head).Facts.NameIdx) continue;
			if(Syntax.GetBindingSyntax(Changed) == BindingSyntax::Name) {
				CollectBindingSource(Scope, Changed, Node, Names);
				continue;
			}
			const std::string Name = Syntax.GetText(Head), Full = Syntax.GetText(Changed);
			if(!Full.starts_with(Name)) continue;
			Names[Syntax.GetCmpNameIdx(Head)].push_back(
				{
					.Name = Head,
					.Member = Full.substr(Name.size()),
					.AliasSrc = IsDelete ? NoNode : Syntax.GetFieldChild(Node, ChildRole::Right),
					.IsUncertain = IsDelete || Syntax.Is(Node, Role::CompoundAssignment)
				}
			);
		}
		// 終了
		return;
	}

	/**
	 * 同じ照会中の採番確認を共有する導入位置の比較関数
	 * 計算量：最初の採番又は修復費用 Q に対し O(Q + 1)，同じ照会の後続比較は O(1)
	 * @param Left 左の導入名
	 * @param Right 右の導入名又は使用位置
	 * @param HasOrder 此の照会で現在の採番を確認したかの格納先
	 * @return 左の名前が構文順で先なら真
	 * @throws FormatLimitExceeded 採番又は修復が作業予算を超える場合
	 * @throws std::bad_alloc 採番又は修復の列を確保出来ない場合
	 * @throws std::length_error 採番又は修復の列の要素数上限を超える場合
	 */
	bool ControlFlow::IsCallableBefore(const NodeId Left, const NodeId Right, [[maybe_unused]] bool &HasOrder) const {
		#if defined(ShaveFormatRefFacts)
		// 基準側では祖先から構文順を比較する
		return Syntax.IsBefore(Left, Right);
		#else
		// 同じ名前又は無効な名前は先に無い事の返戻
		if(Left == Right || Left == NoNode || Right == NoNode) return false;
		if(!HasOrder) {
			Syntax.EnsureOrder();
			HasOrder = true;
		}
		Syntax.Charge(2);
		// 同じ照会中には変更しない現在の前順の比較の返戻
		return Syntax.GetNode(Left).OrderFirst < Syntax.GetNode(Right).OrderFirst;
		#endif
	}

	/**
	 * 呼出先を隠し得る直下の導入名の取得関数
	 * 計算量：未収集の範囲の走査量 N，宣言子の内部の延探索量 D，名前の長さ B とインポートの接頭辞の複写量 P に対し O(N + D + B + P)，宣言数 M の順序付の比較費用 S を加え，再利用時は O(1)
	 * @param Scope 導入名の可視範囲
	 * @return 名前毎の現在の宣言の位置を保つ表
	 * @throws FormatLimitExceeded 名前の探索が作業予算を超える場合
	 * @throws std::bad_alloc 名前表又は走査列を確保出来ない場合
	 * @throws std::length_error 名前表の要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	ControlFlow::CallableNameOwner ControlFlow::GetCallableNamesOf(const NodeId Scope) const {
		// 名前と供給元を同じ範囲の事実から共有する
		return Syntax.GetBindingScopeFacts(Scope).Sources;
	}

	/**
	 * 実引数の要約が所属する可視束縛の範囲の登録の解除関数
	 * 計算量：可視束縛の範囲の数 H に対し期待値 O(H)
	 * @param Node 入子のマクロを含むトークン列
	 * @throws FormatLimitExceeded 範囲の照会が作業予算を超える場合
	 * @throws std::bad_alloc 未収集の所属を確保出来ない場合
	 * @throws std::length_error 所属の列の要素数上限を超える場合
	 */
	void ControlFlow::ForgetMacroBindings(const NodeId Node) const {
		// 束縛に依存しないトークン列の終了
		if(!BoundMacroArguments.erase(Node)) return;
		for(NodeId Scope = GetScopeOf(Node); Scope != NoNode; Scope = GetScopeOf(Syntax.GetParentOf(Scope))) {
			Syntax.Charge(1);
			const std::unordered_map<NodeId, std::unordered_set<NodeId>>::iterator Found = MacroBindingScopes.find(Scope);
			if(Found == MacroBindingScopes.end()) continue;
			Found->second.erase(Node);
			if(Found->second.empty()) MacroBindingScopes.erase(Found);
		}
		// 終了
		return;
	}

	/**
	 * 有効範囲の所属に依存する供給値の保持の終了関数
	 * 計算量：内側の範囲数 S と解放する事実数 F に対し期待値 O(S + F)
	 * @param Scope 変更した有効範囲
	 * @throws FormatLimitExceeded 失効する事実が作業予算を超える場合
	 */
	void ControlFlow::InvalidateAliases(const NodeId Scope) const {
		Syntax.InvalidateBindingSources(Scope);
		// 終了
		return;
	}

	/**
	 * 導入表と，其の結果を継承した名前の照会の失効関数
	 * 計算量：失効する名前の照会数 Q とマクロが依存する範囲の延数 M に対し期待値 O(Q + M + 1)
	 * @param Scope 導入が変わる範囲
	 * @throws FormatLimitExceeded 失効する照会の数が作業予算を超える場合
	 * @throws std::bad_alloc 失効する照会の列を確保出来ない場合
	 * @throws std::length_error 失効する照会の列の長さが上限を超える場合
	 */
	void ControlFlow::InvalidateCallableScope(const NodeId Scope) const {
		InvalidateAliases(Scope);
		// 束縛の変更が影響する終端の失効
		if(Terminations.Body != NoNode && Scope != NoNode && Syntax.HasNodeWithin(Scope, Terminations.Body)) {
			Terminations.Body = NoNode;
			Terminations.Known.Clear(Syntax);
		}
		// 可視範囲の逆引から依存する実引数だけを失効し，別関数の要約を全走査しない
		while(true) {
			const std::unordered_map<NodeId, std::unordered_set<NodeId>>::iterator Found = MacroBindingScopes.find(Scope);
			if(Found == MacroBindingScopes.end()) break;
			const NodeId Args = *Found->second.begin();
			Syntax.Charge(1);
			MacroArguments[Args].reset();
			ForgetMacroBindings(Args);
		}
		// 終了
		return;
	}

	/**
	 * 呼出結果のメンバへ明示的に代入した名前の候補の有無の判定関数
	 * 長さは候補の除外だけに使い，一致候補は従来の全文比較・可視位置の照会へ渡す
	 * 計算量：同名を導入する範囲数 S と範囲の収集費用 Q に対し期待値 O(S + Q + 1)
	 * @param Summary 呼出先の共有した構造
	 * @return 明示的なメンバ代入の候補が在れば真
	 * @throws FormatLimitExceeded 範囲・名前の照会が作業予算を超える場合
	 * @throws std::bad_alloc 範囲又は名前表を確保出来ない場合
	 * @throws std::length_error 名前表の長さが上限を超える場合
	 * @throws std::out_of_range 先頭の名前又は範囲が表に無い場合
	 */
	bool ControlFlow::HasCallableMember(const CalleeSummary &Summary) const {
		// 先頭の名前が無い対象の返戻
		if(Summary.Head == NoNode) return false;
		// 修飾への変更の候補も，中核の同じ供給元の記録から読む
		Syntax.Charge(1);
		// 中核が記録したメンバ変更の候補の返戻
		return Syntax.GetBindingFact(Summary.Head, std::string_view()).HasMemberChanges;
	}

	/**
	 * 子から共有した呼出先の先頭名と呼出の有無の取得関数
	 * 計算量：新しく確保する要約数 G と未収集の部分木の構造 S に対し O(G + S + 1)，再利用時は O(1)
	 * @param Node 呼出先又は別名の式
	 * @return 先頭名と，動的な呼出の結果を含むかの要約
	 * @throws FormatLimitExceeded 部分木の収集が作業予算を超える場合
	 * @throws std::bad_alloc 要約又は作業列を確保出来ない場合
	 * @throws std::length_error 作業列の長さが上限を超える場合
	 * @throws std::out_of_range 子の要約又はノードが表に無い場合
	 */
	ControlFlow::CalleeSummary ControlFlow::GetCalleeSummary(const NodeId Node) const {
		// 対象が無い事の返戻
		if(Node == NoNode) return {};
		// 葉は現在の固定のテキストと役割だけで確定し，部分木の表を要しない
		if(Syntax.GetChildrenOf(Node).empty()) {
			Syntax.Charge(1);
			// 現在の葉から確定した呼出先の返戻
			return {
				Node,
				Syntax.GetLexemeOf(Node).size(),
				Syntax.Is(Node, Role::CallNode) || Syntax.Is(Node, Role::MacroCall),
				Syntax.GetNode(Node).Facts.Has(ConstraintKind::MacroBinding)
			};
		}
		if(Syntax.Callees.size() < Syntax.GetNodeCount()) {
			Syntax.Charge(Syntax.GetNodeCount() - Syntax.Callees.size());
			Syntax.Callees.resize(Syntax.GetNodeCount());
		}
		#if !defined(ShaveFormatRefFacts)
		if(Syntax.Callees[Node]) {
			Syntax.Charge(1);
			// 構築済の呼出先の構造の返戻
			return *Syntax.Callees[Node];
		}
		#endif
		std::vector<std::pair<NodeId, bool>> &Pending = CalleeWork;
		Pending.clear();
		Pending.emplace_back(Node, false);
		while(!Pending.empty()) {
			const std::pair<NodeId, bool> Entry = Pending.back();
			const NodeId Part = Entry.first;
			const bool IsExpanded = Entry.second;
			Pending.pop_back();
			Syntax.Charge(1);
			#if !defined(ShaveFormatRefFacts)
			if(Syntax.Callees[Part].has_value()) continue;
			#endif
			const std::span<const NodeId> Children = Syntax.GetChildrenOf(Part);
			if(!IsExpanded && !Children.empty()) {
				Pending.emplace_back(Part, true);
				for(const NodeId Child : Children) Pending.emplace_back(Child, false);
				continue;
			}
			CalleeSummary Summary;
			Summary.Head = Children.empty() ? Part : NoNode;
			Summary.TextSize = Syntax.GetLexemeOf(Part).size();
			Summary.HasCall = Syntax.Is(Part, Role::CallNode) || Syntax.Is(Part, Role::MacroCall);
			Summary.HasMacroBinding = Syntax.GetNode(Part).Facts.Has(ConstraintKind::MacroBinding);
			bool HasHead = Children.empty();
			for(const NodeId Child : Children) {
				Syntax.Charge(1);
				const CalleeSummary &Inner = *Syntax.Callees.at(Child);
				Summary.TextSize += Inner.TextSize;
				Summary.HasCall |= Inner.HasCall;
				Summary.HasMacroBinding |= Inner.HasMacroBinding;
				if(!HasHead && Syntax.IsNamed(Child)) {
					Summary.Head = Inner.Head;
					HasHead = true;
				}
			}
			if(!Syntax.Callees[Part]) ++Syntax.LiveCallees;
			Syntax.Callees[Part] = Summary;
		}
		// 子から共有した要約の返戻
		return *Syntax.Callees.at(Node);
	}

	/**
	 * 繰返条件の確定した真偽の取得関数
	 * 計算量：辿るラッパーのノード数 N に対し O(N)
	 * @param Cond 判定対象の条件式ノード（最内の値のノードへ進める）
	 * @return 入力で確定した真偽，又は未確定
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	ConstantTruth ControlFlow::GetCondTruth(NodeId &Cond) const {
		// 条件のラッパーと数値に付く符号だけを辿る
		bool NeedsNumeric = false;
		while(Cond != NoNode) {
			Syntax.Charge(1);
			const ConstantTruth Truth = Syntax.GetNode(Cond).Facts.Truth;
			if(Truth != ConstantTruth::Unknown) {
				// 数値の符号を適用出来るかを確認した真偽の返戻
				return !NeedsNumeric || Truth == ConstantTruth::NumericTrue || Truth == ConstantTruth::NumericFalse ?
				Truth :
				ConstantTruth::Unknown;
			}
			if(Syntax.Is(Cond, Role::UnaryPrefixExpr)) {
				const std::string_view Operator = Syntax.GetLexemeOf(Syntax.GetChild(Cond, 0));
				// 数値の符号以外は，此の定数の判定で評価しない事の返戻
				if(Operator != "+" && Operator != "-") return ConstantTruth::Unknown;
				NeedsNumeric = true;
				Cond = Syntax.GetLastNamedChild(Cond);
				continue;
			}
			// 真値キーワードの返戻
			if(!NeedsNumeric && Syntax.Is(Cond, Role::TrueVal)) return ConstantTruth::True;
			if(!NeedsNumeric && Syntax.Is(Cond, Role::Boolean)) {
				// 文法の真偽値の綴りに対応する値の返戻
				return Syntax.IsWord(Cond, Word::True) ? ConstantTruth::True : ConstantTruth::False;
			}
			// C# / Kotlin / Swift は真値キーワードを真偽値リテラルノードで包む為，名前無子トークンの型名が `true` なら真
			if(!NeedsNumeric && Syntax.Is(Cond, Role::BoolLiteral)) {
				// 括弧内のノード
				const NodeId Inner = Syntax.GetChild(Cond, 0);
				// 中身の無い真偽値は未確定である事の返戻
				if(Inner == NoNode) return ConstantTruth::Unknown;
				// 内側トークンの真値キーワード該当有無の返戻
				return Syntax.Is(Inner, Role::TrueVal) ? ConstantTruth::True : ConstantTruth::False;
			}
			if(Syntax.Is(Cond, Role::CondParenWrap)) {
				Cond = Syntax.GetNamedChild(Cond, 0);
				continue;
			}
			// 真値以外の条件は無限繰返と判定不可の返戻
			return ConstantTruth::Unknown;
		}
		// 条件が空の為，無限繰返判定不可の返戻
		return ConstantTruth::Unknown;
	}

	/**
	 * 定数式と評価され得る条件かの判定関数
	 * Java / C# の到達可能性は定数式の値を用いる為，変数でない名前とトークンだけの条件を定数の候補とする
	 * 名前は中核の束縛の事実を読み，確定した宣言の定数指定だけを判定する
	 * 計算量：条件のノード数 N と中核の束縛の取得費用 Q に対し O(N + Q)
	 * @param Cond 条件のノード
	 * @return 定数式でないと構文から確定出来なければ true
	 * @throws FormatLimitExceeded 構文の探索又は収集中の事実の収集が作業上限を超えた場合，又は比較用の名前が未投影の場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool ControlFlow::CanBeConstantCond(const NodeId Cond) const {
		// 定数式でない事を確定出来ないかの返戻
		return !Core::SyntaxModel::HasNodeMatching(
			Syntax,
			Cond,
			[&](const NodeId Node) -> bool {
				// 呼出・代入・増減・型検査等は定数式でない事の返戻
				if(Syntax.Is(Node, Role::NonConstantExpr)) return true;
				// 名前以外のノードは判定を子へ委ねる事の返戻
				if(!Syntax.Is(Node, Role::Identifier)) return false;
				// 修飾の後の名前の照合（this のフィールド又は型の定数の候補）
				if(const NodeId Parent = Syntax.GetParentOf(Node); Syntax.Is(Parent, Role::QualifiedMember)) {
					// 受手の名前と this のフィールドの照合結果の返戻
					// 条件に該当する値の返戻
					if(const NodeId Object = Syntax.GetNamedChild(Parent, 0); Object != Node && !Syntax.Is(Object, Role::ThisRef)) return false;
				}
				const BindingFact Fact = Syntax.GetBindingFact(Node);
				// 未確定の探索を，非定数と証明した事実へ変えない
				if(Fact.Decl != NoNode && !Fact.IsUncertain) return !Fact.IsDeclaredConstant;
				// 未解決の名前は定数の可能性を保つ
				return false;
			}
		);
	}

	/**
	 * 呼出先の名前を隠す現在の可視宣言の判定関数
	 * 計算量：供給元を含む祖先探索量 H，名前表の収集費用 Q と同名の導入位置の照合量 D に対し O(H + Q + D)
	 * @param Name 呼出先又は受手の先頭名
	 * @param IsGlobal 最上位から探索を始める修飾を持つか
	 * @param IsQualified 既知の終端呼出又は標準型が此の名前に続く修飾名を持つか
	 * @param TypeMeaning 戻値型の照会なら其の語の意味，呼出の照会なら空
	 * @return 呼出の位置を囲む宣言が同名の束縛を導入する場合は true
	 * @throws FormatLimitExceeded 束縛の探索が作業予算を超える場合
	 * @throws std::bad_alloc 名前表又は探索列を確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool ControlFlow::HasShadowingDecl(
		const NodeId Name,
		const bool IsGlobal,
		const bool IsQualified,
		const std::optional<Word> TypeMeaning
	) const {
		const BindingFact Fact = Syntax.GetBindingFact(Name, std::string_view(), IsGlobal);
		// 独自の定義に隠された名前の返戻
		if(Fact.HasCustomDefinition) return true;
		// 供給値を持たない宣言に隠された名前の返戻
		if(!Fact.Source) return Fact.Decl != NoNode;
		const CallableIntroduction &Declared = *Fact.Source;
		const bool IsImportedType = TypeMeaning && Declared.ImportedResultType == TypeMeaning;
		if(Declared.IsTerminatingImport || IsImportedType) {
			// インポートの供給元を別の束縛が隠すかの返戻
			return Declared.SrcRoot != NoNode && HasShadowingDecl(Declared.SrcRoot, false, IsImportedType);
		}
		// 同名のモジュールの直接インポートは，既知の修飾した標準呼出を供給する
		return !IsQualified || !Declared.IsModuleImport;
	}

	/**
	 * 条件が無条件の反復を作る仮定の位の取得関数
	 * 計算量：条件の真値判定と必要な定数式判定の費用の和 T に対し O(T)
	 * @param Ctx 同じ走査で共有する関数と必要時に求める名前の表
	 * @param Cond 条件のノード
	 * @return 通常・被覆・悲観の判定の順の位
	 * @throws FormatLimitExceeded 条件の探索が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	uint8_t ControlFlow::GetCondTermination(TerminationCtx &Ctx, const NodeId Cond) const {
		NodeId Val = Cond;
		const ConstantTruth Truth = GetCondTruth(Val);
		// 入力で真と確定した条件の返戻
		if(Truth == ConstantTruth::True || Truth == ConstantTruth::NumericTrue) return AllTermination;
		// 未確定のリテラルは悲観の仮定を残し，後続への到達を確定しない
		if(
			Val != NoNode && Truth == ConstantTruth::Unknown &&
			Syntax.IsAny(Val, { Role::Boolean, Role::BoolLiteral, Role::NumLiteral, Role::StrLikeAll, Role::TrueVal })
		// 未確定のリテラルの値を仮定した位の返戻
		) return PessimisticTermination;
		// 組込定数へ解決しない真偽値名は，値が未確定の定数である可能性も残す
		const bool HasUnresolvedBool = Val != NoNode && Syntax.Is(Val, Role::Identifier) && (
			Syntax.IsWord(Val, Word::True) && !Syntax.GetNode(Val).Facts.HasTrueName ||
			Syntax.IsWord(Val, Word::False) && !Syntax.GetNode(Val).Facts.HasFalseName
		);
		// 呼出・代入等の条件は，可視名を収集せず非定数と確定出来る
		if(
			Ctx.Function != NoNode && Cond != NoNode && (Syntax.GetRules().DoesReachabilityUseConstantCond || HasUnresolvedBool) &&
			SyntaxModel::HasNodeMatching(
				Syntax,
				Cond,
				[&](const NodeId Part) -> bool {
					// 名前の束縛に依らない非定数の構文かの返戻
					return Syntax.Is(Part, Role::NonConstantExpr);
				}
			)
		) return 0;
		// 定数条件を読む場合だけ，関数の異なる照会との共用を制限する
		Ctx.DoesUseFunction =
		Ctx.DoesUseFunction || Cond != NoNode && (Syntax.GetRules().DoesReachabilityUseConstantCond || HasUnresolvedBool);
		// 定数式の値に依る仮定を使わない条件の返戻
		if(Ctx.Function == NoNode || !(Syntax.GetRules().DoesReachabilityUseConstantCond || HasUnresolvedBool) || Cond == NoNode) {
			// 条件の値に依る終端を仮定しない事の返戻
			return 0;
		}
		// 定数式の値の仮定下で終端する位の返戻
		return CanBeConstantCond(Cond) ? PessimisticTermination : 0;
	}

	/**
	 * 脱出文のラベルの取得関数
	 * 計算量：参照する子・トークンの総数 C とテキストの総長 L に対し O(C + L)
	 * @param Node 脱出文
	 * @return 脱出先を名付けるラベル又は PHP の段数（無ければ空）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 */
	NodeId ControlFlow::GetExitLabel(const NodeId Node) const {
		// Rust の脱出は値を伴え，ラベルは専用のノードに限る事の返戻
		if(Syntax.Is(Node, Role::LabeledValExit)) return Syntax.GetFirstNamedChildWithRole(Node, Role::JumpLabel);
		// 其の他の言語は最初の名前付子がラベル又は段数である事の返戻
		return Syntax.GetNamedChild(Node, 0);
	}

	/**
	 * 先頭の子の取得関数
	 * @param Node 対象ノード
	 * @return 先頭の子（無名のトークンを含む）の番号（子が無ければ NoNode）
	 */
	NodeId ControlFlow::GetFirstChildNode(const NodeId Node) const {
		// 先頭の子の番号の返戻
		return Syntax.GetChild(Node, 0);
	}

	/**
	 * 脱出文のジャンプ先の解決関数
	 * 無名の脱出は祖先のインデックスから選び，ラベル付の脱出は元の祖先順で照合する
	 * 計算量：調べる子・祖先・先行兄弟の延数 A，脱出指定と照合するラベルの延べ長 B に対し O(A + B)
	 * @param Path 現在の祖先の脱出先
	 * @param Node 脱出文
	 * @param IsContinuation 繰返の継続か
	 * @return ジャンプ先と継続の扱い（未解決ならジャンプ先は無）
	 * @throws FormatLimitExceeded 構文の探索又は収集中の事実の収集が作業上限を超えた場合，又は比較用の名前が未投影の場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	std::pair<NodeId, bool> ControlFlow::ResolveExitTarget(
		const ExitPath &Path,
		const NodeId Node,
		const bool IsContinuation
	) const {
		// 脱出の指定と解決に用いる名前・段数
		NodeId Arg = GetExitLabel(Node);
		uint32_t Label = 0, Level = 1;
		if(Arg != NoNode) {
			if(const SyntaxRules &Rules = Syntax.GetRules(); Rules.HasNumericExitDepth) {
				while(Arg != NoNode && Syntax.Is(Arg, Role::ValParens)) {
					Syntax.Charge(1);
					Arg = Syntax.GetNamedChild(Arg, 0);
				}
				// ジャンプ先の無い指定の返戻
				if(Arg == NoNode) return { NoNode, false };
				// 段数の記法と進数の解釈
				std::string Digits(Syntax.GetText(Arg));
				std::erase(Digits, Rules.DigitSeparator);
				std::string_view Num(Digits);
				int Base = 10;
				if(Num.size() > 1 && Num.front() == '0') {
					// 進数の接頭辞と其の照合結果
					const std::string_view Prefix = Num.substr(1, 1);
					const std::array<std::string_view, 3> &Prefixes = Rules.ExitDepthBasePrefixes;
					const bool IsHex = Prefixes[0].find(Prefix) != std::string_view::npos;
					const bool IsBinary = Prefixes[1].find(Prefix) != std::string_view::npos;
					Base = IsHex ? 16 : IsBinary ? 2 : 8;
					if(IsHex || IsBinary || Prefixes[2].find(Prefix) != std::string_view::npos) Num.remove_prefix(2);
				}
				// 解析不能な段数の返戻
				if(
					const std::from_chars_result Parsed = std::from_chars(Num.data(), Num.data() + Num.size(), Level, Base);
					Parsed.ec != std::errc{} || Parsed.ptr != Num.data() + Num.size() || !Level
				) return { NoNode, false };
			} else if(Syntax.Is(Arg, Role::NameToken)) Label = Syntax.GetCmpNameIdx(Arg);
			// 名前でない値を伴う脱出のジャンプ先を解決しない事の返戻
			else return { NoNode, false };
		}
		if(!Label) {
			const std::vector<NodeId> &Targets = IsContinuation ? Path.ContinueTargets : Path.BreakTargets;
			// 指定段数のジャンプ先を持たない場合の返戻
			if(Level > Targets.size()) return { NoNode, false };
			const NodeId Target = Targets[Targets.size() - Level];
			// 段数で選んだジャンプ先の返戻
			return { Target, IsContinuation && !Syntax.Is(Target, Role::ContinueExitsSwitch) };
		}
		// ラベルを指定した脱出先の祖先探索
		for(NodeId Parent = Syntax.GetParentOf(Node); Parent != NoNode; Parent = Syntax.GetParentOf(Parent)) {
			Syntax.Charge(1);
			// 独立した制御の境界の返戻
			if(IsFlowBoundary(Parent)) return { Parent, false };
			if(Syntax.GetRules().IsExitLabelBodyChild) {
				// 本体のラベルに一致するジャンプ先の返戻
				if(
					const NodeId Own = Syntax.GetFirstNamedChildWithRole(Parent, Role::JumpLabel);
					Own != NoNode && Syntax.GetCmpNameIdx(Own) == Label
				) return { Parent, IsContinuation };
				continue;
			}
			// 最も内側のラベルに一致するジャンプ先の返戻
			if(
				const bool IsMatchingLabel = Syntax.Is(Parent, Role::LabeledStmt) && Syntax.GetNamedChildCount(Parent) &&
				Syntax.GetCmpNameIdx(Syntax.GetNamedChild(Parent, 0)) == Label;
				IsMatchingLabel
			) return { Parent, IsContinuation };
			if(Syntax.GetRules().IsExitLabelPrevSibling) {
				for(NodeId Prev = Syntax.GetPrevNamedSibling(Parent); Prev != NoNode; Prev = Syntax.GetPrevNamedSibling(Prev)) {
					Syntax.Charge(1);
					if(Syntax.Is(Prev, Role::TransparentLabelPrefix)) continue;
					if(!Syntax.Is(Prev, Role::Label)) break;
					// 直前のラベルに一致するジャンプ先の返戻
					if(Syntax.Is(Prev, Role::NameToken) && Syntax.GetCmpNameIdx(Prev) == Label) return { Parent, IsContinuation };
				}
			}
		}
		// 未解決のラベルの返戻
		return { NoNode, false };
	}

	/**
	 * 制御の流れの所有者の取得関数
	 * 計算量：未確定の共通祖先の総数 H と照会数 Q に対し期待値 O(H + Q)
	 * @param Node 変更又は判定するノード
	 * @return 最も内側の独立した制御の流れのノード又は根（何れにも属さなければ `NoNode`）
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 所属の表又は祖先の列を確保出来ない場合
	 * @throws std::length_error 所属の表又は祖先の列の要素数上限を超える場合
	 */
	NodeId ControlFlow::GetFlowOwner(const NodeId Node) const {
		// 所属の記録領域の確保
		if(FlowOwners.size() < Syntax.GetNodeCount()) FlowOwners.resize(Syntax.GetNodeCount());
		// 共通の祖先経路で制御フローの所属を継承する
		std::vector<NodeId> Path;
		NodeId Owner = NoNode;
		for(NodeId Up = Node; Up != NoNode; Up = Syntax.GetParentOf(Up)) {
			Syntax.Charge(1);
			#if !defined(ShaveFormatRefFacts)
			if(FlowOwners[Up]) {
				Owner = *FlowOwners[Up];
				break;
			}
			#endif
			Path.push_back(Up);
			if(IsFlowBoundary(Up) || Up == Syntax.GetRoot()) {
				Owner = Up;
				break;
			}
		}
		#if !defined(ShaveFormatRefFacts)
		for(const NodeId Part : Path) FlowOwners[Part] = Owner;
		#endif
		// 最内の制御フローの所有者の返戻
		return Owner;
	}

	/**
	 * 分岐を全て列挙する切替かの判定関数
	 * `switch` 文と，本体を持たず全ての分岐をアローで列挙する Java の切替式を同じ切替として扱う
	 * 分岐の中で脱出文を書けない切替の式は含めない
	 * @param Type 判定するノード
	 * @return 全ての分岐を列挙する切替なら true
	 */
	bool ControlFlow::IsArmSwitch(const NodeId Type) const {
		// switch 文と Java の切替式かの返戻
		return Syntax.Is(Type, Role::ArmSwitch);
	}

	/**
	 * 同じ所有者の訪問済の位置の取得関数
	 * 計算量：O(1)
	 * @param Idx 所有者のインデックス
	 * @param Node 対象ノード
	 * @return 連続記録の位置（対象外は最大値）
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 */
	size_t ControlFlow::GetLoopExitPosition(const LoopExitIdx &Idx, const NodeId Node) const {
		Syntax.Charge(1);
		// 世代の異なる直接参照は再構築済の所有者に属さない事の返戻
		return Node < LoopExitPositions.size() && LoopExitPositions[Node].first == Idx.Generation ?
		LoopExitPositions[Node].second :
		SIZE_MAX;
	}

	/**
	 * 部分木の判定範囲毎の最小脱出深さの取得関数
	 * 計算量：期待値 O(1)
	 * @param Idx 所有者のインデックス
	 * @param Position 連続記録の位置
	 * @param Scope 判定範囲
	 * @return 判定済の最小脱出深さ（未判定は空）
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::out_of_range 連続記録の位置が範囲外の場合
	 */
	std::optional<uint32_t> ControlFlow::GetLoopExitMinimum(
		const LoopExitIdx &Idx,
		const size_t Position,
		const NodeId Scope
	) const {
		Syntax.Charge(1);
		const std::optional<std::pair<NodeId, uint32_t>> &Minimum = Idx.Nodes.at(Position).Minimum;
		// 未判定の範囲の返戻
		if(!Minimum) return std::nullopt;
		// 最初の判定範囲の共有結果の返戻
		if(Minimum->first == Scope) return Minimum->second;
		const uint64_t Key = uint64_t{ Scope } << 32 | Position;
		const std::pmr::unordered_map<uint64_t, uint32_t>::const_iterator Found = Idx.AdditionalMin.find(Key);
		// 別の判定範囲の結果の返戻
		return Found == Idx.AdditionalMin.end() ? std::nullopt : std::optional<uint32_t>(Found->second);
	}

	/**
	 * 判定済の最小脱出深さの登録関数
	 * 計算量：期待値 O(1)
	 * @param Idx 所有者のインデックス
	 * @param Position 連続記録の位置
	 * @param Scope 判定範囲
	 * @param Depth 最小脱出深さ
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc 別の判定範囲の記録を確保出来ない場合
	 * @throws std::length_error 別の判定範囲の記録の上限を超える場合
	 * @throws std::out_of_range 連続記録の位置が範囲外の場合
	 */
	void ControlFlow::StoreLoopExitMinimum(
		LoopExitIdx &Idx,
		const size_t Position,
		const NodeId Scope,
		const uint32_t Depth
	) const {
		Syntax.Charge(1);
		std::optional<std::pair<NodeId, uint32_t>> &Minimum = Idx.Nodes.at(Position).Minimum;
		if(!Minimum) Minimum = { Scope, Depth };
		else Idx.AdditionalMin.emplace(uint64_t{ Scope } << 32 | Position, Depth);
		// 終了
		return;
	}

	/**
	 * 脱出のインデックスの構築関数
	 * 構文順と深さを記録し，各脱出のジャンプ先を走査中の祖先のインデックスから１度だけ選ぶ
	 * 計算量：走査ノード数 N，取得関数内も含む子・祖先・ジャンプ先の延照合数 A，数字で指定する脱出先のテキスト長 B，破棄するインデックスの総要素数 K に対し期待値 O(N + A + B + K)
	 * @param Gotos 同じ関数内の goto ジャンプ先
	 * @param Root 制御フローの所有者（基準検証では繰返又はラベル）
	 * @param Idx 構築するインデックス
	 * @throws FormatLimitExceeded 構文の探索又は収集中の事実の収集が作業上限を超えた場合，又は比較用の名前が未投影の場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void ControlFlow::BuildLoopExitIdx(const GotoIdx &Gotos, const NodeId Root, LoopExitIdx &Idx) const {
		// 脱出のインデックスの構築
		// 未構築の所有範囲だけを初期化し，失効時は所有範囲と共に共有メモリを破棄する
		Idx.Root = Root;
		if(LoopExitGeneration == SIZE_MAX) throw FormatLimitExceeded("structured text exceeds formatting limit");
		Idx.Generation = ++LoopExitGeneration;
		if(LoopExitPositions.size() < Syntax.GetNodeCount()) {
			Syntax.Charge(Syntax.GetNodeCount() - LoopExitPositions.size());
			LoopExitPositions.resize(Syntax.GetNodeCount());
		}
		// 構文順と深さの集計を待つノード
		struct Pending {
			NodeId Node;
			uint32_t Depth;
			bool IsClosing; // 祖先から此のノードを除く後行の枠か
			bool HasAddedBreak; // 脱出先の祖先の列へ追加したか
			bool HasAddedContinue; // 継続先の祖先の列へ追加したか
			NodeId Scope = NoNode; // 最内の切替
		};
		// 走査の作業列と祖先の脱出先・構文順の位置
		std::vector<Pending> Work { { Root, 0, false, false, false } };
		ExitPath Path;
		// 深さと脱出事象の収集
		while(!Work.empty()) {
			const Pending Cur = Work.back();
			Work.pop_back();
			if(Cur.IsClosing) {
				Syntax.Charge(1);
				LoopExitIdx::NodeRecord &Record = Idx.Nodes.at(GetLoopExitPosition(Idx, Cur.Node));
				// 後始末に依存しない最小深さは，同じ後行で親へ１度だけ渡す
				if(Cur.Node != Root) {
					Syntax.Charge(1);
					LoopExitIdx::NodeRecord &Parent = Idx.Nodes.at(GetLoopExitPosition(Idx, Syntax.GetParentOf(Cur.Node)));
					if(!Record.PlainMin) Parent.PlainMin.reset();
					else if(Parent.PlainMin) *Parent.PlainMin = std::min(*Parent.PlainMin, *Record.PlainMin);
				}
				if(Cur.HasAddedBreak) Path.BreakTargets.pop_back();
				if(Cur.HasAddedContinue) Path.ContinueTargets.pop_back();
				continue;
			}
			Syntax.Charge(1);
			LoopExitPositions[Cur.Node] = { Idx.Generation, Idx.Nodes.size() };
			Idx.Nodes.push_back({ Cur.Node, Cur.Depth, std::nullopt });
			LoopExitIdx::NodeRecord &Record = Idx.Nodes.back();
			Record.HasTry = Syntax.Is(Cur.Node, Role::TryStmt) || Syntax.Is(Cur.Node, Role::CatchableBody);
			if(Record.HasTry) {
				Record.Scope = Cur.Scope;
				Record.PlainMin.reset();
			}
			// 脱出文のジャンプ先の記録
			const size_t EventBegin = Idx.Events.size();
			if(Syntax.Is(Cur.Node, Role::GotoStmt)) {
				Syntax.Charge(1);
				const std::unordered_map<NodeId, NodeId>::const_iterator Found = Gotos.Targets.find(Cur.Node);
				Idx.Events.push_back({ Cur.Node, Found == Gotos.Targets.end() ? NoNode : Found->second, NoNode, 0, true });
			} else if(
				const bool IsLoopExit = Syntax.Is(Cur.Node, Role::LoopExitStmt);
				IsLoopExit || Syntax.Is(Cur.Node, Role::KeywordJump)
			) {
				const NodeId Keyword = GetFirstChildNode(Cur.Node);
				if(
					const bool ShouldContinue = Syntax.Is(Keyword, Role::ContinueKeyword);
					IsLoopExit || ShouldContinue || Syntax.Is(Keyword, Role::BreakKeyword)
				) {
					Syntax.Charge(1);
					const std::pair<NodeId, bool> Target = ResolveExitTarget(Path, Cur.Node, ShouldContinue);
					Idx.Events.push_back({ Cur.Node, NoNode, Target.first, 0, false, Target.second });
				}
			}
			if(Idx.Events.size() != EventBegin) {
				Record.Event = EventBegin;
				const ExitEvent &Event = Idx.Events.back();
				// 走査中に確定した祖先への脱出だけを共有し，goto とラベルの継続は既存の判定へ渡す
				if(Event.IsGoto || Event.IsContinuation && Syntax.Is(Event.Target, Role::LabeledStmt)) Record.PlainMin.reset();
				else if(const size_t Target = GetLoopExitPosition(Idx, Event.Target); Event.Target != NoNode && Target == SIZE_MAX) {
					Record.PlainMin.reset();
				} else if(Record.PlainMin) {
					*Record.PlainMin = Event.Target == NoNode ? 0 : Idx.Nodes[Target].Depth + static_cast<uint32_t>(Event.IsContinuation);
				}
			}
			// 祖先の脱出先の更新
			const bool HasAddedBreak = Syntax.Is(Cur.Node, Role::BreakTarget);
			const bool HasAddedContinue = HasAddedBreak && (Syntax.GetRules().DoesContinueCountSwitch || Syntax.Is(Cur.Node, Role::Loop));
			if(HasAddedBreak) Path.BreakTargets.push_back(Cur.Node);
			if(HasAddedContinue) Path.ContinueTargets.push_back(Cur.Node);
			Work.push_back({ Cur.Node, Cur.Depth, true, HasAddedBreak, HasAddedContinue });
			if(Cur.Node != Root && IsFlowBoundary(Cur.Node)) continue;
			const bool IsSwitch = IsArmSwitch(Cur.Node);
			if(IsSwitch) Idx.SwitchParents.emplace(Cur.Node, Cur.Scope);
			const NodeId Scope = IsSwitch ? Cur.Node : Cur.Scope;
			const std::span<const NodeId> Children = Syntax.GetChildrenOf(Cur.Node);
			for(std::span<const NodeId>::reverse_iterator Child = Children.rbegin(); Child != Children.rend(); ++Child) {
				Work.push_back({ *Child, Cur.Depth + 1, false, false, false, Scope });
			}
		}
		// ジャンプ先と判定範囲からの脱出の深さの確定
		for(ExitEvent &Event : Idx.Events) {
			Syntax.Charge(1);
			if(!Event.IsGoto) {
				NodeId Target = Event.Target;
				if(Event.IsContinuation) while(Syntax.Is(Target, Role::LabeledStmt)) {
					Syntax.Charge(1);
					const uint32_t Count = Syntax.GetNamedChildCount(Target);
					if(!Count) {
						Target = NoNode;
						break;
					}
					Target = Syntax.GetNamedChild(Target, Count - 1);
				}
				const size_t Found = GetLoopExitPosition(Idx, Target);
				Event.Depth = Found != SIZE_MAX ? Idx.Nodes[Found].Depth + (Event.IsContinuation ? 1 : 0) : 0;
				continue;
			}
			// goto の着地点と共通の祖先の確認
			NodeId Target = Event.Dst;
			while(Target != NoNode && Syntax.Is(Target, Role::NamedLabelStmt)) {
				Syntax.Charge(1);
				Target = Syntax.GetNextNamedSibling(Target);
			}
			while(Target != NoNode && Syntax.Is(Target, Role::LabeledStmt)) {
				Syntax.Charge(1);
				const uint32_t Count = Syntax.GetNamedChildCount(Target);
				if(!Count) break;
				Target = Syntax.GetNamedChild(Target, Count - 1);
			}
			if(GetLoopExitPosition(Idx, Event.Dst) != SIZE_MAX) for(NodeId Up = Event.Node; Up != NoNode; Up = Syntax.GetParentOf(Up)) {
				Syntax.Charge(1);
				const size_t Position = GetLoopExitPosition(Idx, Up);
				if(Position != SIZE_MAX && Syntax.HasNodeWithin(Up, Event.Dst)) {
					Event.Depth = Idx.Nodes[Position].Depth + 1;
					break;
				}
			}
			if(const size_t Position = GetLoopExitPosition(Idx, Target); Position != SIZE_MAX && Syntax.HasNodeWithin(Target, Event.Node)) {
				Event.Depth = std::max(Event.Depth, Idx.Nodes[Position].Depth + 1);
			}
		}
		// 終了
		return;
	}

	/**
	 * 次の節へ進む文かの判定関数
	 * @param Node 判定する文
	 * @return 独立した語又は文で次の節へ進むなら true
	 */
	bool ControlFlow::IsFallthroughStmt(const NodeId Node) const {
		// 素通りの文かの返戻
		return Syntax.Is(Node, Role::BareFallthrough) || Syntax.Is(Node, Role::FallthroughStmt);
	}

	/**
	 * 全ての値に一致する照合かの判定関数
	 * @param Pattern 節の照合（Python の case の照合）
	 * @return 読捨の照合 `_` か，名前１つの捕捉の照合なら true
	 */
	bool ControlFlow::IsIrrefutablePattern(const NodeId Pattern) const {
		// 部品１つでない照合の返戻
		if(Syntax.GetChildCount(Pattern) != 1) return false;
		const NodeId Part = Syntax.GetChild(Pattern, 0);
		// 読捨のトークンか，名前１つだけの捕捉かの返戻
		return Syntax.Is(Part, Role::WildcardToken) || Syntax.Is(Part, Role::Identifier) ||
		Syntax.GetChildCount(Part) == 1 && Syntax.Is(Syntax.GetChild(Part, 0), Role::Identifier);
	}

	/**
	 * ノード内への goto 入口有無判定関数
	 * 計算量：既存の範囲表の中は入口数 L に対し O(log(L + 1))，表の外はジャンプ先数 G と祖先の最大深さ H に対し O(G*H)
	 * @param Gotos 同じ関数内の goto ジャンプ先
	 * @param Node 検査するノード（無なら入口無し）
	 * @return 外から到達候補と為るラベルが有れば true
	 * @throws FormatLimitExceeded 構文の探索又は収集中の事実の収集が作業上限を超えた場合，又は比較用の名前が未投影の場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	bool ControlFlow::HasGotoEntry(const GotoIdx &Gotos, const NodeId Node) const {
		// ノードの部分木に入口のラベルが含まれるかの返戻
		if(Node == NoNode || Gotos.Entries.empty()) return false;
		Syntax.Charge(1);
		const std::pair<uint64_t, uint64_t> Range = Syntax.GetOrderRange(Node);
		const std::vector<NodeId>::const_iterator Entry = Gotos.Entries.begin() + WorkSort::LowerBound(
			std::span<const NodeId>(Gotos.Entries),
			Range.first,
			[this](const NodeId Label, const uint64_t Wanted) -> bool {
				Syntax.Charge(1);
				// 入口の位置が部分木の始端より前かの返戻
				return Syntax.GetOrderRange(Label).first < Wanted;
			}
		);
		// 始端以降の入口が無い場合の返戻
		if(Entry == Gotos.Entries.end()) return false;
		Syntax.Charge(1);
		// 最初の入口位置が部分木内に在るかの返戻
		return Syntax.GetOrderRange(*Entry).first < Range.second;
	}

	/**
	 * 判定済の文の終端と効果の取得関数
	 * 計算量：期待値 O(1)
	 * @param Known 判定範囲毎のノードの終端の判定結果
	 * @param Node 文（無は終端しない）
	 * @param Scope 内部転送を認める switch 文の判定範囲
	 * @return 通常・被覆・悲観の終端の仮定と，外側へ伝播する効果
	 * @throws FormatLimitExceeded 判定する子を作業列へ積まずに結果を読んだ場合（フォーマッターの不具合）
	 */
	ControlFlow::TerminationSummary ControlFlow::GetKnownSummary(
		const TerminationResults &Known,
		const NodeId Node,
		const NodeId Scope
	) {
		// 文の無い事の返戻
		if(Node == NoNode) return {};
		const TerminationSummary *const Found = Known.Find(Node, Scope);
		// 判定して居ない文を読む事はフォーマッターの不具合とする
		if(!Found) throw FormatLimitExceeded("termination of an unvisited statement was read", FailureReason::FormatterDefect);
		// 判定済の終端の返戻
		return *Found;
	}

	/**
	 * 判定済の文の終端の仮定の取得関数
	 * @param Known 判定結果の表
	 * @param Node 判定済の文
	 * @param Scope 制御移動を判定する範囲
	 * @return 終端の成立する仮定
	 * @throws FormatLimitExceeded 未判定の文を照会した場合
	 */
	uint8_t ControlFlow::GetKnownTermination(const TerminationResults &Known, const NodeId Node, const NodeId Scope) {
		// 終端の仮定の返戻
		return GetKnownSummary(Known, Node, Scope).Modes;
	}

	/**
	 * 分岐文の枝の取得関数
	 * 計算量：対象と参照先の子の総数 C，照合するテキストの総長 L に対し O(C + L)
	 * @param Node 分岐文
	 * @return 真枝，後続の枝の本体及び条件を問わない最後の枝の有無
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	const ControlFlow::BranchParts &ControlFlow::GetBranchesOf(const NodeId Node) const {
		// 同じ評価で抽出済の構造の再利用
		Syntax.Charge(1);
		#if defined(ShaveFormatRefFacts)
		BranchStructures.erase(Node);
		#endif
		if(
			const std::unordered_map<NodeId, BranchParts>::const_iterator Found = BranchStructures.find(Node);
			Found != BranchStructures.end()
		// 変更不能な構造の返戻
		) return Found->second;
		// 分岐文の枝の取得
		BranchParts Parts;
		Parts.Then = Syntax.GetIfConsequence(Node);
		if(Parts.Then == NoNode) {
			// フィールドを持たない Swift の分岐は最初の文の並びが真枝，`else` の後の子が偽枝
			bool IsAfterElse = false;
			Core::SyntaxModel::ForEachChildOf(
				Syntax,
				Node,
				[&](const NodeId Child) -> void {
					if(Syntax.Is(Child, Role::Else)) IsAfterElse = true;
					else if(IsAfterElse && Syntax.IsNamed(Child)) {
						Parts.Alternatives.push_back(Child);
						Parts.HasElse = true;
						IsAfterElse = false;
					} else if(Parts.Then == NoNode && Syntax.Is(Child, Role::ValStmtSeq)) Parts.Then = Child;
					// 終了
					return;
				}
			);
		} else {
			const uint32_t Count = Syntax.GetChildCount(Node);
			for(uint32_t Idx = 0; Idx < Count; ++Idx) {
				Syntax.Charge(1);
				// 後続の枝のフィールド
				NodeId Branch = Syntax.GetChild(Node, Idx);
				if(!Syntax.HasField(Branch, ChildRole::Alternative)) continue;
				// 条件を持つ枝の本体の取得（Python の elif ･ PHP の elseif は続く枝を要する）
				if(Syntax.Is(Branch, Role::ConditionalBranchClause)) {
					Branch = Syntax.GetIfConsequence(Branch);
					Parts.HasElse = false;
				} else {
					// else 節のラッパーを除いた本体の取得
					if(Syntax.Is(Branch, Role::ElseAny)) {
						const NodeId Inner = Syntax.GetFieldChild(Branch, ChildRole::Body);
						Branch = Inner == NoNode ? Syntax.GetNamedChild(Branch, 0) : Inner;
					}
					Parts.HasElse = true;
				}
				Parts.Alternatives.push_back(Branch);
			}
		}
		// 分岐文の枝の返戻
		return BranchStructures.emplace(Node, std::move(Parts)).first->second;
	}

	/**
	 * 対象繰返からの脱出有無判定関数
	 * 後始末とジャンプ先に依存しない本体の最小脱出深さは，親子表の構築時に集計する
	 * 其れ以外は同じ判定範囲の脱出の最小深さを本体へ集計して再利用する
	 * 集計済なら共有事実の収集を含め期待値 O(1)，子構成・種別・ジャンプ先・後始末の終端が変わった時だけ再構築する
	 * 計算量：初回はジャンプ先前提の取得と BuildLoopExitIdx の費用，新しい判定範囲は対象部分木のノード数 N，goto 数 G，最大深さ H に対し O(N + G*H)
	 * @param PrevGotos 同じ関数内の goto ジャンプ先
	 * @param Known 判定済の後始末とジャンプの伝播元
	 * @param Body 検索する本体
	 * @param Loop 対象繰返
	 * @param TransferScope 内部転送を認める switch 文の判定範囲
	 * @param ShouldIncludeContinue 本体の終端を調べる為に条件判定への継続も含めるか
	 * @return 繰返自身又は其の外側への脱出（指定時は条件判定への継続も）が有れば true
	 * @throws FormatLimitExceeded 構文の探索又は収集中の事実の収集が作業上限を超えた場合，又は比較用の名前が未投影の場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool ControlFlow::HasLoopExit(
		const GotoIdx &PrevGotos,
		const TerminationResults &Known,
		const NodeId Body,
		const NodeId Loop,
		const NodeId TransferScope,
		const bool ShouldIncludeContinue
	) const {
		// 本体の無い繰返は脱出しない事の返戻
		if(Body == NoNode) return false;
		Syntax.Charge(1);
		#if defined(ShaveFormatRefFacts)
		LoopExitIdx Idx;
		const NodeId Root = Loop;
		#else
		const NodeId Owner = GetFlowOwner(Loop), Root = Owner == NoNode ? Syntax.GetRoot() : Owner;
		LoopExitIdx &Idx = LoopExits[Root];
		#endif
		if(Idx.Root == NoNode) {
			#if defined(ShaveFormatRefFacts)
			BuildLoopExitIdx(PrevGotos, Root, Idx);
			#else
			// 関数外の繰返も同じ根を共有し，別の繰返の記録を上書きしない
			const GotoIdx &Gotos = Owner == NoNode ? CollectGotoTargets(Root) : PrevGotos;
			BuildLoopExitIdx(Gotos, Root, Idx);
			#endif
		}
		#if !defined(ShaveFormatRefFacts)
		// 無条件に合成出来る本体は，構築時の集計を読み追加の部分木走査をしない
		if(const std::optional<uint32_t> &Plain = Idx.Nodes.at(GetLoopExitPosition(Idx, Body)).PlainMin; Plain) {
			// 本体が対象の反復から脱出するかの返戻
			return *Plain <= Idx.Nodes.at(GetLoopExitPosition(Idx, Loop)).Depth + static_cast<uint32_t>(ShouldIncludeContinue);
		}
		#endif
		// 評価済の本体だけを後行で集計し，外側の照会は確定済の子を共有する
		std::pmr::vector<std::pair<NodeId, bool>> &Pending = Idx.Pending;
		Pending.clear();
		Pending.emplace_back(Body, false);
		while(!Pending.empty()) {
			const std::pair<NodeId, bool> Cur = Pending.back();
			const NodeId Node = Cur.first;
			const bool IsClosing = Cur.second;
			Pending.pop_back();
			Syntax.Charge(1);
			const size_t Position = GetLoopExitPosition(Idx, Node);
			if(!IsClosing && GetLoopExitMinimum(Idx, Position, TransferScope)) continue;
			if(!IsClosing) {
				Pending.emplace_back(Node, true);
				if(Node != Root && IsFlowBoundary(Node)) continue;
				for(const NodeId Child : Syntax.GetChildrenOf(Node)) if(GetLoopExitPosition(Idx, Child) != SIZE_MAX) {
					Pending.emplace_back(Child, false);
				}
				continue;
			}
			const LoopExitIdx::NodeRecord &Record = Idx.Nodes.at(GetLoopExitPosition(Idx, Node));
			uint32_t Depth = UINT32_MAX;
			if(Record.Event != std::numeric_limits<size_t>::max()) {
				const ExitEvent &Event = Idx.Events[Record.Event];
				if(!Event.IsGoto || TransferScope == NoNode || !Syntax.HasNodeWithin(TransferScope, Event.Dst)) Depth = Event.Depth;
			}
			NodeId Source = NoNode;
			if(Record.HasTry) {
				NodeId Scope = TransferScope;
				// 独立して評価した本体の外側の切替は，判定範囲へ持ち込まない
				if(Scope == NoNode) for(
					NodeId Up = Record.Scope;
					Up != NoNode && Idx.Nodes.at(GetLoopExitPosition(Idx, Up)).Depth >= Idx.Nodes.at(GetLoopExitPosition(Idx, Loop)).Depth;
				) {
					Syntax.Charge(1);
					Scope = Up;
					Up = Idx.SwitchParents.at(Up);
				}
				Source = GetKnownSummary(Known, Node, Scope).TransferSource;
			}
			if(Node == Root || !IsFlowBoundary(Node)) for(const NodeId Child : Syntax.GetChildrenOf(Node)) {
				Syntax.Charge(1);
				const size_t ChildPosition = GetLoopExitPosition(Idx, Child);
				if(ChildPosition == SIZE_MAX) continue;
				const std::optional<uint32_t> ChildMin = GetLoopExitMinimum(Idx, ChildPosition, TransferScope);
				if(!ChildMin || Source != NoNode && !Syntax.HasNodeWithin(Child, Source)) continue;
				Depth = std::min(Depth, *ChildMin);
			}
			StoreLoopExitMinimum(Idx, Position, TransferScope, Depth);
		}
		// 本体の脱出先が繰返の外か，指定時は条件判定への継続かの返戻
		return *GetLoopExitMinimum(Idx, GetLoopExitPosition(Idx, Body), TransferScope) <=
		Idx.Nodes.at(GetLoopExitPosition(Idx, Loop)).Depth + static_cast<uint32_t>(ShouldIncludeContinue);
	}

	/**
	 * 捕捉節又は捕捉式の実行部分の取得関数
	 * 計算量：直接の子の数 C に対し O(C)
	 * @param Handler 捕捉を持つ構文
	 * @return 捕捉した後に評価する本体又は式
	 * @throws FormatLimitExceeded 構文又は要約の照会が作業予算を超える場合
	 * @throws std::out_of_range ノード又は要約の識別子が範囲外の場合
	 */
	NodeId ControlFlow::GetHandlerBody(const NodeId Handler) const {
		// 捕捉節の本体の選択
		const KindDef *const Def = Syntax.GetDefOf(Handler);
		const ChildRole Field = Def && Def->Evaluation.CatchInput != ChildRole::None ? Def->Evaluation.CatchInput : ChildRole::Body;
		const NodeId Body = Syntax.GetFieldChild(Handler, Field);
		// 投影済のフィールド又はフィールドを持たない節の最後の子の返戻
		return Body == NoNode ? Syntax.GetLastNamedChild(Handler) : Body;
	}

	/**
	 * 子の評価時機の取得関数
	 * 直接の子の位置条件を共有し，子孫の入力を別の親で読む照会とは混ぜない
	 * 計算量：位置定義 P，親の子 C と保持列の拡張 G に対し初回 O(P C + G)，共有済の直接の子は O(1)，其の他の再照会は O(P)
	 * @param Node 親ノード
	 * @param Child 判定する子
	 * @return フィールド又は名前付の位置に対応する評価時機
	 * @throws FormatLimitExceeded 構文又は要約の照会が作業予算を超える場合
	 * @throws std::bad_alloc 評価時機又は位置条件の保持列を確保出来ない場合
	 * @throws std::length_error 保持列の大きさがコンテナの上限を超える場合
	 * @throws std::out_of_range ノード又は要約の識別子が範囲外の場合
	 */
	EvaluationClass ControlFlow::GetChildEvaluation(const NodeId Node, const NodeId Child) const {
		// 子の評価位置の照合
		const KindDef *const Def = Syntax.GetDefOf(Node);
		// 定義が無い場合と，展開した枝の子孫へ入口の契約を適用しない場合の返戻
		if(!Def) return EvaluationClass::Conditional;
		const bool IsDirect = Syntax.GetParentOf(Child) == Node;
		// 展開した枝の子孫へ入口の契約を継がない事の返戻
		if(!IsDirect && !Def->Evaluation.DoesEvaluateChildInputs) return EvaluationClass::Conditional;
		#if !defined(ShaveFormatRefFacts)
		if(IsDirect && Child < ChildEvaluations.size() && ChildEvaluations[Child].first == Def) {
			Syntax.Charge(1);
			// 同じ親の入力契約から求めた評価時機の返戻
			return ChildEvaluations[Child].second;
		}
		#endif
		// 位置条件を要する直接の子だけを共有し，固定定義で足りる値は控えない
		const auto Remember = [&](const EvaluationClass Value) -> EvaluationClass {
			#if !defined(ShaveFormatRefFacts)
			if(IsDirect && (!Def->Evaluation.Inputs.empty() || !Def->Evaluation.UnevaluatedHead.empty())) {
				if(ChildEvaluations.size() <= Child) {
					Syntax.Charge(static_cast<size_t>(Child) + 1 - ChildEvaluations.size());
					ChildEvaluations.resize(static_cast<size_t>(Child) + 1);
				}
				ChildEvaluations[Child] = { Def, Value };
			}
			#endif
			// 現在の構文で求めた評価時機の返戻
			return Value;
		};
		if(
			!Def->Evaluation.UnevaluatedHead.empty() && Syntax.GetLexemeOf(Syntax.GetNamedChild(Node, 0)) == Def->Evaluation.UnevaluatedHead
		// 呼出形で表す非評価演算の入力の返戻
		) return Remember(EvaluationClass::Unevaluated);
		if(!Def->Evaluation.Inputs.empty()) {
			#if defined(ShaveFormatRefFacts)
			if(Node < EvaluationSelectors.size()) EvaluationSelectors[Node].reset();
			#endif
			// 登録済の条件列は，此の親が位置条件を要する事も確定して居る
			const bool NeedsSelectors = Node < EvaluationSelectors.size() && EvaluationSelectors[Node].has_value() || std::ranges::any_of(
				Def->Evaluation.Inputs,
				[](const EvaluationInput &Input) -> bool {
					// 子だけの役割・フィールドで足りない条件かの返戻
					return Input.WhenSiblingKind || !Input.AfterToken.empty() || !Input.WhenToken.empty() || Input.WhenPresent != ChildRole::None ||
					!Input.ChildKind && Input.Field == ChildRole::None;
				}
			);
			if(NeedsSelectors) {
				if(EvaluationSelectors.size() < Syntax.GetNodeCount()) EvaluationSelectors.resize(Syntax.GetNodeCount());
				if(!EvaluationSelectors[Node]) {
					std::vector<EvaluationSelector> Selectors;
					Selectors.reserve(Def->Evaluation.Inputs.size());
					for(const EvaluationInput &Input : Def->Evaluation.Inputs) {
						EvaluationSelector Selector;
						if(Input.WhenSiblingKind && Syntax.GetFirstNamedChildWithRole(Node, *Input.WhenSiblingKind) == NoNode) {
							Selector.IsEnabled = false;
						} else if(!Input.AfterToken.empty()) {
							bool IsAfter = false;
							for(const NodeId Part : Syntax.GetChildrenOf(Node)) {
								Syntax.Charge(1);
								if(IsAfter && Syntax.IsNamed(Part)) {
									Selector.Selected = Part;
									break;
								}
								IsAfter = IsAfter || Syntax.GetLexemeOf(Part) == Input.AfterToken;
							}
						} else {
							Selector.IsEnabled = Input.WhenToken.empty() || std::ranges::any_of(
								Syntax.GetChildrenOf(Node),
								[&](const NodeId Token) -> bool {
									Syntax.Charge(1);
									// 直接の修飾語の一致の返戻
									return Syntax.GetLexemeOf(Token) == Input.WhenToken;
								}
							);
							Selector.IsEnabled =
							Selector.IsEnabled && (Input.WhenPresent == ChildRole::None || Syntax.GetFieldChild(Node, Input.WhenPresent) != NoNode);
							if(Selector.IsEnabled && !Input.ChildKind && Input.Field == ChildRole::None) {
								Selector.Selected = Syntax.GetNamedChild(Node, Input.NamedIndex);
							}
						}
						Selectors.push_back(Selector);
					}
					EvaluationSelectors[Node] = std::move(Selectors);
				}
			}
			for(size_t Idx = 0; Idx < Def->Evaluation.Inputs.size(); ++Idx) {
				Syntax.Charge(1);
				const EvaluationInput &Input = Def->Evaluation.Inputs[Idx];
				const EvaluationSelector Default;
				const EvaluationSelector &Selector = NeedsSelectors ? (*EvaluationSelectors[Node])[Idx] : Default;
				if(!Selector.IsEnabled || Input.UnlessKind && Syntax.Is(Child, *Input.UnlessKind)) continue;
				// 定義順で最初に一致した位置の評価時機の返戻
				if(
					!Input.AfterToken.empty() ? Selector.Selected != NoNode && Selector.Selected == Child : Input.ChildKind ?
					Syntax.Is(Child, *Input.ChildKind) :
					Input.Field == ChildRole::None ? Selector.Selected == Child : Syntax.HasField(Child, Input.Field)
				) return Remember(Input.Class);
			}
		}
		// 個別の指定を持たない子の既定の評価時機の返戻
		return Remember(Def->Evaluation.Children);
	}

	/**
	 * 例外処理文の節の取得関数
	 * 計算量：対象と参照先の子の総数 C，照合するテキストの総長 L に対し O(C + L)
	 * @param Node 例外処理文（Swift の do は捕捉の節を持つ）
	 * @return 本体，例外の無かった時の節，後始末の節及び捕捉の節
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	const ControlFlow::TryParts &ControlFlow::GetTryPartsOf(const NodeId Node) const {
		// 同じ評価で抽出済の構造の再利用
		Syntax.Charge(1);
		#if defined(ShaveFormatRefFacts)
		TryStructures.erase(Node);
		#endif
		if(const std::unordered_map<NodeId, TryParts>::const_iterator Found = TryStructures.find(Node); Found != TryStructures.end()) {
			// 変更不能な構造の返戻
			return Found->second;
		}
		// 節の本体の取得関数（フィールドの無い文法は最後の名前付の子）
		const auto ClauseBody = [this](const NodeId Clause) -> NodeId {
			const NodeId Inner = Syntax.GetFieldChild(Clause, ChildRole::Body);
			// 本体の返戻
			return Inner == NoNode ? Syntax.GetLastNamedChild(Clause) : Inner;
		};
		TryParts Parts;
		Parts.Body = Syntax.GetFieldChild(Node, ChildRole::Body);
		if(const KindDef *const Def = Syntax.GetDefOf(Node); Def && Def->Evaluation.CatchInput != ChildRole::None) {
			Parts.Handlers.push_back(Node);
			// 独立した捕捉節を持たない捕捉式の返戻
			return TryStructures.emplace(Node, std::move(Parts)).first->second;
		}
		Core::SyntaxModel::ForEachNamedChildOf(
			Syntax,
			Node,
			[&](const NodeId Child) -> void {
				// Kotlin / Swift の本体は最初の文の並び
				if(Parts.Body == NoNode && Syntax.Is(Child, Role::ValStmtSeq)) Parts.Body = Child;
				else if(Syntax.Is(Child, Role::CatchClause)) Parts.Handlers.push_back(Child);
				else if(Syntax.Is(Child, Role::FinallyClause)) Parts.Finally = ClauseBody(Child);
				else if(Syntax.Is(Child, Role::ElseClause)) Parts.Else = ClauseBody(Child);
				if(Child != Parts.Body && GetChildEvaluation(Node, Child) == EvaluationClass::Required) Parts.Entries.push_back(Child);
				// 終了
				return;
			}
		);
		// 文の本体を持たない試行の式は，被演算子の送出も外側の捕捉へ渡す
		if(Parts.Body == NoNode && Syntax.Is(Node, Role::AwaitTryCastExpr)) {
			Parts.Body = Syntax.GetLastNamedChild(Node);
			const NodeId First = Syntax.GetNamedChild(Node, 0);
			if(First != Parts.Body) Parts.Operator = First;
		}
		// 例外処理文の節の返戻
		return TryStructures.emplace(Node, std::move(Parts)).first->second;
	}

	/**
	 * switch 文の節の取得関数
	 * 計算量：対象と参照先の子の総数 C，照合するテキストの総長 L に対し O(C + L)
	 * @param Node switch 文
	 * @return 本体と直下の節及び抽出が未確定か（未確定と節０件を区別する）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	const ControlFlow::SwitchParts &ControlFlow::GetSwitchCases(const NodeId Node) const {
		// 同じ評価で抽出済の構造の再利用
		Syntax.Charge(1);
		#if defined(ShaveFormatRefFacts)
		SwitchStructures.erase(Node);
		#endif
		if(
			const std::unordered_map<NodeId, SwitchParts>::const_iterator Found = SwitchStructures.find(Node);
			Found != SwitchStructures.end()
		// 変更不能な構造の返戻
		) return Found->second;
		// 切替本体の抽出
		NodeId Body = Syntax.GetFieldChild(Node, ChildRole::Body);
		if(Body == NoNode) {
			// 節を直接の子に並べる切替
			if(Syntax.Is(Node, Role::DirectSwitchArms)) Body = Node;
			else {
				Core::SyntaxModel::ForEachNamedChildOf(
					Syntax,
					Node,
					// 切替本体ノードの採用関数
					[&](const NodeId Child) -> void {
						if(Syntax.Is(Child, Role::SwitchBody)) Body = Child;
						// 終了
						return;
					}
				);
			}
		}
		std::vector<NodeId> Cases;
		// 本体の無い switch 文の返戻
		if(Body == NoNode) return SwitchStructures.emplace(Node, SwitchParts{ Body, {}, true }).first->second;
		bool IsInside = Body != Node, IsUnknown = false, HasNestedCase = false;
		// 分岐構造の前順の収集（ノードと本体からの深さ）
		std::vector<std::pair<NodeId, uint32_t>> Stack { { Body, 0 } };
		while(!Stack.empty() && !IsUnknown) {
			const std::pair<NodeId, uint32_t> Cur = Stack.back();
			const NodeId Child = Cur.first;
			const uint32_t Depth = Cur.second;
			Stack.pop_back();
			Syntax.Charge(1);
			const bool IsDirect = Depth == 1, IsRoot = !Depth;
			if(IsDirect && Syntax.IsTokenRole(Child, Role::OpeningBraceToken)) IsInside = true;
			if(!IsRoot && IsInside && Syntax.IsNamed(Child)) {
				if(Syntax.Is(Child, Role::PreprocBranch)) IsUnknown = true;
				else if(const bool IsCase = Syntax.Is(Child, Role::CaseClause) || Syntax.Is(Child, Role::CaseExprArm); IsCase && IsDirect) {
					Cases.push_back(Child);
				} else if(IsCase) HasNestedCase = true;
				else if(IsDirect) IsUnknown = true;
			}
			// C/C++ の case は文の内側にも置けるが，ネスト switch・関数には非所属
			if(
				!IsUnknown && (IsRoot || Syntax.GetRules().CanCaseNestInStmt && !Syntax.Is(Child, Role::SwitchStmt) && !IsFlowBoundary(Child))
			) {
				const std::span<const NodeId> Children = Syntax.GetChildrenOf(Child);
				for(std::span<const NodeId>::reverse_iterator Next = Children.rbegin(); Next != Children.rend(); ++Next) {
					Stack.emplace_back(*Next, Depth + 1);
				}
			}
		}
		// 文の内部への入口は直下の節の終端を無効にし，未確定の節の構造とは区別
		if(IsUnknown || HasNestedCase) Cases.clear();
		// 本体と節の返戻
		return SwitchStructures.emplace(Node, SwitchParts{ Body, std::move(Cases), IsUnknown }).first->second;
	}

	/**
	 * 切替の節の本文の取得関数
	 * 計算量：対象と参照先の子の総数 C，照合するテキストの総長 L に対し O(C + L)
	 * @param Case 切替の節
	 * @return 本文を開く記号より後の文（素通りの文を含む）と本文を開く記号の有無・種類
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	const ControlFlow::CaseBody &ControlFlow::GetCaseBodyOf(const NodeId Case) const {
		// 同じ評価で抽出済の構造の再利用
		Syntax.Charge(1);
		#if defined(ShaveFormatRefFacts)
		CaseStructures.erase(Case);
		#endif
		if(
			const std::unordered_map<NodeId, CaseBody>::const_iterator Found = CaseStructures.find(Case);
			Found != CaseStructures.end()
		// 変更不能な構造の返戻
		) return Found->second;
		// 切替の節の本文の取得
		CaseBody Body;
		Core::SyntaxModel::ForEachChildOf(
			Syntax,
			Case,
			[&](const NodeId Child) -> void {
				// ラベルを本文に数えない場合の終了
				if(Syntax.Is(Child, Role::SwitchLabel)) return;
				if(!Body.IsOpened) {
					if(Syntax.Is(Child, Role::CaseBodyOpener) || Syntax.GetRules().DoesCaseOpenWithStmtEnd && Syntax.IsWord(Child, Word::StmtEnd)) {
						Body.IsOpened = true;
						Body.IsArrow = Syntax.IsTokenRole(Child, Role::RightArrowToken);
					}
				} else if(IsFallthroughStmt(Child) || Syntax.IsNamed(Child)) Body.Stmts.push_back(Child);
				// 終了
				return;
			}
		);
		// 節の本文の返戻
		return CaseStructures.emplace(Case, std::move(Body)).first->second;
	}

	/**
	 * 不一致の値を受ける切替の節かの判定関数（整形仕様 5.2，既定の節の省略だけを網羅の根拠にしない）
	 * 計算量：参照する子・トークンの総数 C とテキストの総長 L に対し O(C + L)
	 * @param Case 切替の節
	 * @return 既定の枝，既定の語・else の語を持つ節，又は条件の無い全てに一致する照合の節なら true
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool ControlFlow::IsDefaultCase(const NodeId Case) const {
		// 既定の枝のノードの返戻
		if(Syntax.Is(Case, Role::DefaultCase)) return true;
		if(const NodeId Pattern = Syntax.GetFirstNamedChildWithRole(Case, Role::CasePattern); Pattern != NoNode) {
			// 照合を持つ節の，条件の無い全てに一致する照合かの返戻
			return Syntax.GetFieldChild(Case, ChildRole::Guard) == NoNode && IsIrrefutablePattern(Pattern);
		}
		// 既定の語・else の語を直接の子又は節の見出の先頭に持つかの返戻
		return Core::SyntaxModel::HasChildMatching(
			Syntax,
			Case,
			[this](const NodeId Child) -> bool {
				const NodeId Head = Syntax.Is(Child, Role::SwitchLabel) ? GetFirstChildNode(Child) : Child;
				// 既定を示す語かの返戻
				return Syntax.IsWord(Head, Word::Default) || Syntax.Is(Head, Role::ElseKeyword);
			}
		);
	}

	/**
	 * 処理系が網羅を検査する照合の切替の節かの判定関数（整形仕様 5.2）
	 * 計算量：参照する子・トークンの総数 C とテキストの総長 L に対し O(C + L)
	 * @param Case 切替の節
	 * @return 網羅を要求する照合・値（Java の型の照合と `case null`）を持つ枝のラベルを持てば true
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 */
	bool ControlFlow::IsExhaustivenessRequired(const NodeId Case) const {
		// 網羅を要求する照合・値を持つ枝のラベルの有無の返戻
		return Core::SyntaxModel::HasChildMatching(
			Syntax,
			Case,
			[this](const NodeId Label) -> bool {
				// 網羅を要求する照合・値を持つ枝のラベルかの返戻
				return Syntax.Is(Label, Role::SwitchLabel) && Core::SyntaxModel::HasChildMatching(
					Syntax,
					Label,
					[this](const NodeId Part) -> bool {
						// 網羅を要求する照合・値かの返戻
						return Syntax.Is(Part, Role::ExhaustiveCaseLabel);
					}
				);
			}
		);
	}

	/**
	 * 次の枝へ進む語の属する切替の節の取得関数
	 * 計算量：祖先深度 H と照合するテキストの総長 L に対し O(H + L)
	 * @param Node 判定するノード
	 * @param Scope 内部転送を認める switch 文の判定範囲
	 * @return 文の並びに置いた Swift の `fallthrough` を囲む最も内の切替の節（其の他は無）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	NodeId ControlFlow::GetFallthroughEntry(const NodeId Node, const NodeId Scope) const {
		// 次の枝へ進む語でない事の返戻
		if(
			Scope == NoNode || !Syntax.Is(Node, Role::FallthroughName) || !Syntax.IsWord(Node, Word::Fallthrough) ||
			!Syntax.Is(Syntax.GetParentOf(Node), Role::ValStmtSeq)
		) return NoNode;
		for(NodeId Parent = Syntax.GetParentOf(Node); Parent != NoNode; Parent = Syntax.GetParentOf(Parent)) {
			Syntax.Charge(1);
			// 囲む切替の節の返戻
			if(Syntax.Is(Parent, Role::SwitchEntry)) return Parent;
		}
		// 切替の外の語の返戻
		return NoNode;
	}

	/**
	 * 文の並びの終端判定関数
	 * 計算量：文数 N と入口照会量 Q に対し O(N + Q)
	 * @param Gotos 同じ関数内の goto ジャンプ先
	 * @param Known 判定範囲毎のノードの終端の判定結果
	 * @param Stmts 実行順の文
	 * @param Scope 内部転送を認める switch 文の判定範囲
	 * @return 最後の入口より後で終端する仮定の位
	 * @throws FormatLimitExceeded 抽出又は照会が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	uint8_t ControlFlow::SeqTermination(
		const GotoIdx &Gotos,
		const TerminationResults &Known,
		const std::span<const NodeId> Stmts,
		const NodeId Scope
	) const {
		// 文の並びの終端判定
		uint8_t Modes = 0;
		for(const NodeId Child : Stmts) {
			Syntax.Charge(1);
			if(HasGotoEntry(Gotos, Child)) Modes = 0;
			Modes |= GetKnownTermination(Known, Child, Scope);
		}
		// 最後の入口より後で終端する仮定の返戻
		return Modes;
	}

	/**
	 * 文を直下に並べるノードの文の取得関数
	 * 計算量：対象と参照先の子の総数 C，照合するテキストの総長 L に対し O(C + L)
	 * @param Container 文を直下に並べるノード
	 * @return コメントと後送された本文を除く文（実行順）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	std::vector<NodeId> ControlFlow::GetStmtsIn(const NodeId Container) const {
		// 文を直下に並べるノードの文の取得
		std::vector<NodeId> Stmts;
		Core::SyntaxModel::ForEachStmtOf(
			Syntax,
			Container,
			[&](const NodeId Child) -> void {
				Stmts.push_back(Child);
				// 終了
				return;
			}
		);
		// 文の並びの返戻
		return Stmts;
	}

	/**
	 * 本体を１度だけ実行する文の本体の取得関数
	 * 計算量：対象と参照先の子の総数 C，照合するテキストの総長 L に対し O(C + L)
	 * @param Node 本体を１度だけ実行する文
	 * @return 本体のフィールドの子（無ければ最後の名前付の子）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	NodeId ControlFlow::GetSingleBodyOf(const NodeId Node) const {
		// 本体を１度だけ実行する文の本体の取得
		const NodeId Inner = Syntax.GetFieldChild(Node, ChildRole::Body);
		// 本体の返戻
		return Inner == NoNode ? Syntax.GetLastNamedChild(Node) : Inner;
	}

	/**
	 * 前処理分岐の文の取得関数
	 * 計算量：対象と参照先の子の総数 C，照合するテキストの総長 L に対し O(C + L)
	 * @param Node 前処理分岐
	 * @return 条件と代替枝とコメントを除く名前付の子（#else は条件を持たない）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	std::vector<NodeId> ControlFlow::GetPreprocStmts(const NodeId Node) const {
		// 後続の枝と収集する文・条件の有無
		const NodeId Alternative = Syntax.GetFieldChild(Node, ChildRole::Alternative);
		std::vector<NodeId> Stmts;
		bool IsGuarded = !Syntax.Is(Node, Role::PreprocElse);
		Core::SyntaxModel::ForEachNamedChildOf(
			Syntax,
			Node,
			[&](const NodeId Child) -> void {
				// コメントと後の分岐（`#else` 等）の子の場合の終了
				if(Child == Alternative) return;
				// 最初の子は分岐の条件
				if(IsGuarded) IsGuarded = false;
				else Stmts.push_back(Child);
				// 終了
				return;
			}
		);
		// 分岐の文の返戻
		return Stmts;
	}

	/**
	 * 分岐文の全枝終端判定関数
	 * 計算量：枝の抽出と必要な脱出照会の総費用 Q に対し O(Q)
	 * @param Gotos 同じ関数内の goto ジャンプ先
	 * @param Known 判定範囲毎のノードの終端の判定結果
	 * @param Node 分岐文
	 * @param TransferScope 内部転送を認める switch 文の判定範囲
	 * @param IsAbrupt 直後へ通常完了しない事実の格納先
	 * @return 条件を問わない最後の枝を持ち，全ての枝が終端する仮定の位
	 * @throws FormatLimitExceeded 抽出又は照会が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	uint8_t ControlFlow::BranchTermination(
		const GotoIdx &Gotos,
		const TerminationResults &Known,
		const NodeId Node,
		const NodeId TransferScope,
		bool &IsAbrupt
	) const {
		// 分岐文の全枝終端判定
		const BranchParts &Parts = GetBranchesOf(Node);
		// 条件を問わない枝を持たない分岐の返戻
		if(!Parts.HasElse || Parts.Then == NoNode) return 0;
		const TerminationSummary Then = GetKnownSummary(Known, Parts.Then, TransferScope);
		uint8_t Modes = Then.Modes;
		bool AreAllAbrupt = Then.IsAbrupt;
		for(const NodeId Branch : Parts.Alternatives) {
			Syntax.Charge(1);
			const TerminationSummary Alternative = GetKnownSummary(Known, Branch, TransferScope);
			Modes &= Alternative.Modes;
			AreAllAbrupt = AreAllAbrupt && Alternative.IsAbrupt;
		}
		if(Syntax.Is(Node, Role::SiblingLabeledBranch)) {
			// 自身のラベルへ抜ける分岐の返戻
			if(
				const NodeId Prev = Syntax.GetPrevNamedSibling(Node);
				Prev != NoNode && Syntax.Is(Prev, Role::StmtLabel) && HasLoopExit(Gotos, Known, Node, Node, TransferScope)
			) return 0;
		}
		IsAbrupt = AreAllAbrupt;
		// 全ての枝が終端する仮定の返戻
		return Modes;
	}

	/**
	 * switch 文の全分岐終端判定関数
	 * 記述した全ての節が終端し，不一致の値も既定の節か処理系の網羅・送出で後続へ進まない切替を終端とする（整形仕様 5.2）
	 * 不一致の値が来ないと看做す被覆の位では，記述した全ての節が終端する切替を終端とする
	 * 計算量：節の抽出・文の照合・脱出判定の総費用 Q に対し O(Q)
	 * @param Gotos 同じ関数内の goto ジャンプ先
	 * @param Known 判定範囲毎のノードの終端の判定結果
	 * @param Node switch 文
	 * @param TransferScope 外側の switch 文の判定範囲（単独判定時は空）
	 * @return 素通り・外部脱出の経路が無い仮定の位
	 * @throws FormatLimitExceeded 抽出又は照会が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	uint8_t ControlFlow::SwitchTermination(
		const GotoIdx &Gotos,
		const TerminationResults &Known,
		const NodeId Node,
		const NodeId TransferScope
	) const {
		// switch 文の全分岐終端判定
		const SwitchParts &Branches = GetSwitchCases(Node);
		const NodeId Body = Branches.Body;
		const std::vector<NodeId> &Cases = Branches.Cases;
		const NodeId Scope = TransferScope == NoNode ? Node : TransferScope;
		// 本体又は判定可能な節が無いか本体から抜ける切替の返戻
		if(Branches.IsUnknown) return CoveredTermination;
		// 空の網羅切替は，正常に後続へ進む経路を持たない
		if(Cases.empty()) return Syntax.Is(Node, Role::ExhaustiveSwitch) ? AllTermination : 0;
		// 本体から抜ける経路を持つ切替の返戻
		if(HasLoopExit(Gotos, Known, Body, Node, Scope)) return 0;
		const SyntaxRules &Rules = Syntax.GetRules();
		uint8_t Next = 0, Modes = AllTermination;
		bool HasCoverage = Syntax.Is(Node, Role::ExhaustiveSwitch);
		for(std::vector<NodeId>::const_reverse_iterator Iter = Cases.rbegin(); Iter != Cases.rend(); ++Iter) {
			Syntax.Charge(1);
			const CaseBody &Case = GetCaseBodyOf(*Iter);
			// 本文を持たない節の返戻
			if(!Case.IsOpened) return CoveredTermination;
			uint8_t Closed = 0;
			for(const NodeId Child : Case.Stmts) {
				if(IsFallthroughStmt(Child)) Closed = Next;
				else {
					if(HasGotoEntry(Gotos, Child)) Closed = 0;
					Closed |= GetKnownTermination(Known, Child, Scope);
				}
			}
			if(
				const bool IsFallthrough = !Syntax.Is(*Iter, Role::CaseExprArm) && (
					Rules.CanCaseFallThrough || Rules.CanColonCaseFallThrough && !Case.IsArrow ||
					Rules.CanEmptyCaseFallThrough && Case.Stmts.empty()
				);
				IsFallthrough
			) Closed |= Next;
			Modes &= Closed;
			Next = Closed;
			HasCoverage = HasCoverage || IsDefaultCase(*Iter) || IsExhaustivenessRequired(*Iter);
		}
		// 全節が閉被覆も満たす仮定の返戻
		return Modes & (HasCoverage ? AllTermination : CoveredTermination);
	}

	/**
	 * 現役割インデックスの空探索で指定役割の不在を確定出来るかの判定関数
	 * インデックスを構築しない取得入口では確定せず，呼出側の構造走査を維持する
	 * 計算量：期待値 O(1)
	 * @param Desired 不在を確認する役割
	 * @return 指定の役割のインデックスが構築済で，其の役割のノードが無い場合だけ `true`
	 * @throws FormatLimitExceeded インデックスの照会が作業予算を超えた場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	bool ControlFlow::CanExcludeRole(const Role Desired) const {
		// インデックスの無い取得入口では不在を断定しない事の返戻
		if(!Syntax.HasRoleIdx || !Syntax.IndexedRoles.test(static_cast<size_t>(Desired))) return false;
		Syntax.Charge(1);
		// 現在の役割インデックスの空探索の返戻
		return Syntax.GetNodesWith(Desired).empty();
	}

	/**
	 * C / C++ の次の宣言子の取得関数
	 * 計算量：フィールドが在れば O(1)，無ければ直接の子数 C に対し O(C)
	 * @param Node 外側の宣言子又は関数のノード
	 * @return 仮引数・属性を除いた内側の宣言子（無ければ無）
	 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	NodeId ControlFlow::GetNextDeclarator(const NodeId Node) const {
		// C / C++ の次の宣言子の取得
		// 宣言子のフィールドの子の返戻
		if(const NodeId Next = Syntax.GetFieldChild(Node, ChildRole::Declarator); Next != NoNode) return Next;
		for(const NodeId Child : Syntax.GetChildrenOf(Node)) {
			Syntax.Charge(1);
			if(
				Syntax.IsNamed(Child) && (
					Syntax.Is(Child, Role::DeclaratorWrapper) || Syntax.Is(Child, Role::ParenthesizedDeclarator) ||
					Syntax.Is(Child, Role::ArrayDeclarator) ||
					(Syntax.Is(Node, Role::RefDeclarator) || Syntax.Is(Node, Role::ParenthesizedDeclarator)) &&
					(Syntax.Is(Child, Role::IdentifierLeafLike) || Syntax.Is(Child, Role::StructuredBindingDeclarator))
				)
			// フィールドで取得出来ない内側の最初の宣言子の返戻
			) return Child;
		}
		// 内側の宣言子の無い事の返戻
		return NoNode;
	}

	/**
	 * 同じ関数に属する値付の返戻文の有無の判定関数
	 * 計算量：初回は参照するノード数 N とテキスト長 L に対し O(N + L)，同じ本体の再照会は O(1)
	 * @param Body 関数本体
	 * @return ネスト関数の本体を除いて値を返す返戻文が有れば true
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	bool ControlFlow::HasValReturn(const NodeId Body) const {
		// 本体が空ノードか，現インデックスに返戻の構文が無い（宣言の内部を辿らない）本体の値付の返戻文の無い事の返戻
		if(Body == NoNode || CanExcludeRole(Role::FlowReturn) && CanExcludeRole(Role::ArgReturn)) return false;
		if(ValueReturns.size() < Syntax.GetNodeCount()) ValueReturns.resize(Syntax.GetNodeCount());
		Syntax.Charge(1);
		#if !defined(ShaveFormatRefFacts)
		// 同じ本体の推論済の有無の返戻
		if(ValueReturns[Body]) return *ValueReturns[Body];
		#endif
		// ネスト関数定義の返戻文は当該関数に属さない為，其の部分木へは降りない
		const bool HasValueReturn = Core::SyntaxModel::HasStoppedTraversal(
			Syntax,
			Body,
			[&](const NodeId Node) -> WalkStep {
				// 境界の中へ降りない事の返戻
				if(Node != Body && IsFlowBoundary(Node)) return WalkStep::Skip;
				// Ruby の return は実引数の並び，其の他の言語は名前付の子（戻値式）の有無で値付かを判別する事の返戻
				return (
					Syntax.Is(Node, Role::ArgReturn) ?
					Syntax.Is(Node, Role::ReturnKeyword) && Syntax.GetFirstNamedChildWithRole(Node, Role::ArgSeq) != NoNode :
					Syntax.Is(Node, Role::FlowReturn) && Syntax.GetNamedChild(Node, 0) != NoNode
				) ? WalkStep::Stop : WalkStep::Descend;
			}
		);
		ValueReturns[Body] = HasValueReturn;
		// 同じ本体の推論に共有する有無の返戻
		return HasValueReturn;
	}

	/**
	 * C / C++ のポインタ・参照を返す関数かの判定関数
	 * 計算量：辿る被演算子・宣言子のノード数 N とテキストの総長 L に対し O(N + L)
	 * @param Function 関数のノード
	 * @return 戻値の宣言子がポインタ又は参照なら true
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool ControlFlow::HasPtrOrRefReturn(const NodeId Function) const {
		// C / C++ のポインタ・参照を返す関数かの判定
		for(NodeId Node = GetNextDeclarator(Function); Node != NoNode; Node = GetNextDeclarator(Node)) {
			Syntax.Charge(1);
			// ポインタ・参照の宣言子が有る事の返戻
			if(Syntax.Is(Node, Role::PtrOrRefDeclarator)) return true;
		}
		// 間接返戻でない事の返戻
		return false;
	}

	/**
	 * 式を本体とする関数かの判定関数
	 * 計算量：対象と参照先の子の総数 C，照合するテキストの総長 L に対し O(C + L)
	 * @param Node 関数のノード
	 * @return 関数又は本体のラッパーが式の本体を導くトークン（Kotlin・Ruby の `=` 等）を持てば `true`
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool ControlFlow::HasExprBody(const NodeId Node) const {
		// 式を本体とする関数かの判定
		NodeId Body = Syntax.GetFieldChild(Node, ChildRole::Body);
		if(Body == NoNode) Body = Syntax.GetFirstNamedChildWithRole(Node, Role::FunctionBody);
		// 式の本体を導くトークンの有無の返戻
		return Syntax.HasWordChild(Node, Word::ExprBody) || Body != NoNode && Syntax.HasWordChild(Body, Word::ExprBody);
	}

	/**
	 * マクロ置換の無い int 型の判定関数
	 * 計算量：型のノード数 N とテキスト長 L に対し O(N + L)
	 * @param Type 確認する型
	 * @return 登録された int 型の指定子だけで構成されれば true
	 * @throws FormatLimitExceeded 型の走査が作業予算を超える場合
	 * @throws std::bad_alloc 型の走査列を確保出来ない場合
	 * @throws std::length_error 型の走査列の上限を超える場合
	 * @throws std::out_of_range 型のノードが表に無い場合
	 */
	bool ControlFlow::IsPlainIntType(const NodeId Type) const {
		// 型を持たない候補の返戻
		if(Type == NoNode) return false;
		bool HasResultType = false;
		std::vector<NodeId> Pending { Type };
		while(!Pending.empty()) {
			const NodeId Part = Pending.back();
			Pending.pop_back();
			Syntax.Charge(1);
			// マクロ置換後の型を確定出来ない候補の返戻
			if(Syntax.GetNode(Part).Facts.Has(ConstraintKind::MacroBinding)) return false;
			const std::span<const NodeId> Children = Syntax.GetChildrenOf(Part);
			if(Children.empty()) {
				// int 型を構成しない指定子を含む候補の返戻
				if(!Syntax.IsWord(Part, Word::EntryResultType)) return false;
				HasResultType = true;
			} else Pending.insert(Pending.end(), Children.begin(), Children.end());
		}
		// 型の構成要素の有無の返戻
		return HasResultType;
	}

	/**
	 * C / C++ のエントリーポイントかの判定関数
	 * 計算量：参照する部分木のノード総数 N とテキストの総長 L に対し O(N + L)
	 * @param Node 関数のノード
	 * @param Type 後置の指定を解決した有効な戻値型
	 * @return 名前・型にマクロ束縛が無く，翻訳単位に属する int 型の main なら true
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool ControlFlow::IsProgramEntry(const NodeId Node, const NodeId Type) const {
		// C / C++ のエントリーポイントかの判定
		const NodeId Declarator = Syntax.GetFieldChild(Node, ChildRole::Declarator);
		NodeId Name = Syntax.GetFieldChild(Declarator, ChildRole::Declarator);
		while(Syntax.Is(Name, Role::ParenthesizedDeclarator)) {
			Syntax.Charge(1);
			Name = GetNextDeclarator(Name);
		}
		// 裸の名前と，後置の指定を含む解決済の戻値型を使う
		// エントリーポイントでない事の返戻
		if(
			Type == NoNode || !Syntax.Is(Declarator, Role::FunctionDeclarator) || !Syntax.IsWord(Name, Word::EntryFunction) ||
			Syntax.GetNode(Name).Facts.Has(ConstraintKind::MacroBinding) || !IsPlainIntType(Type)
		) return false;
		// 翻訳単位迄の祖先が外部結合の指定と前処理の分岐だけかの返戻
		for(NodeId Parent = Syntax.GetParentOf(Node); Parent != NoNode; Parent = Syntax.GetParentOf(Parent)) {
			Syntax.Charge(1);
			// 翻訳単位に達したエントリーポイントである事の返戻
			if(Syntax.Is(Parent, Role::TranslationUnit)) return true;
			// 名前空間・クラスに属する関数はエントリーポイントでない事の返戻
			if(
				const bool IsLocalScope =
				!Syntax.Is(Parent, Role::PreprocBranch) && !Syntax.Is(Parent, Role::LinkageSpec) && !Syntax.Is(Parent, Role::DeclList);
				IsLocalScope
			) return false;
		}
		// 翻訳単位に達しない事の返戻
		return false;
	}

	/**
	 * 関数本体の返戻区分の判定関数
	 * 操作の種類，戻らない事の宣言，戻値の型，型を省いた関数の既定の順に，文法の事実だけで決める
	 * 計算量：参照する部分木のノード総数 N とテキストの総長 L に対し O(N + L)
	 * @param Node 関数のノード
	 * @param IsAsync 非同期の修飾を持つか
	 * @param IsNeverOnly 非復帰以外の区分を省いて良いか
	 * @param Name 共有宣言の適用先の名前（省略時は関数自身）
	 * @return 戻値の区分と其の根拠（IsNeverOnly では非復帰以外を Void・Unknown で返す場合が在る）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	ReturnShape ControlFlow::GetBodyReturnShapeOf(
		const NodeId Node,
		const bool IsAsync,
		const bool IsNeverOnly,
		const NodeId Name
	) const {
		// 関数の戻値の区分の判定
		const SyntaxRules &Rules = Syntax.GetRules();
		// 本体の返戻文からの推論
		const auto Inferred = [&]() -> ReturnShape {
			NodeId Body = Syntax.GetFieldChild(Node, ChildRole::Body);
			if(Body == NoNode) Body = Syntax.GetFirstNamedChildWithRole(Node, Role::CompoundBody);
			// 値付の返戻文の有無に依る区分の返戻
			return { HasValReturn(Body) ? ReturnKind::Val : ReturnKind::Void, ReturnEvidence::Inferred };
		};
		// 構築・破棄・設定・監視の操作が値を返さない事の返戻
		if(Syntax.Is(Node, Role::ResultlessRoutine)) return { ReturnKind::Void, ReturnEvidence::Declared };
		// 読出の操作が値を返す事の返戻
		if(Syntax.Is(Node, Role::ReaderRoutine)) return { ReturnKind::Val, ReturnEvidence::Declared };
		if(Syntax.Is(Node, Role::NamedAccessor)) {
			// 読出の名前の操作だけが値を返す事の返戻
			return {
				Syntax.IsWord(Syntax.GetFieldChild(Node, ChildRole::Name), Word::Getter) ? ReturnKind::Val : ReturnKind::Void,
				ReturnEvidence::Declared
			};
		}
		// 戻らない事の宣言の返戻
		if(IsNoReturnDeclared(Node, Name)) return { ReturnKind::Never, ReturnEvidence::Declared };
		if(IsNeverOnly && Rules.Words[static_cast<size_t>(Word::NeverType)].empty()) {
			// 非復帰の型を持たない文法では，宣言以外から Never に為る事は無い
			return { ReturnKind::Void, ReturnEvidence::Unknown };
		}
		// ポインタ・参照を返す宣言子の返戻
		if(HasPtrOrRefReturn(Node)) return { ReturnKind::Val, ReturnEvidence::Declared };
		NodeId Type = GetResultTypeOf(Node);
		if(Type == NoNode) {
			// 型を省いた関数が整数を返す事の返戻
			if(Rules.OmittedResult == OmittedResultNotation::Int) return { ReturnKind::Val, ReturnEvidence::Declared };
			const bool IsUnit = Rules.OmittedResult == OmittedResultNotation::Unit;
			// 式の本体は其の値を返し，単位型を既定とする文法では値の型をテキストから決めれない事の返戻
			if(HasExprBody(Node)) return { ReturnKind::Val, IsUnit ? ReturnEvidence::Unknown : ReturnEvidence::Inferred };
			// 単位型を返す事と本体から推論する事の返戻
			return IsUnit ? ReturnShape{ ReturnKind::Void, ReturnEvidence::Declared } : Inferred();
		}
		if(Syntax.Is(Type, Role::PlaceholderTypeSpecifier)) {
			// 推論を要求する型を置換える後置の戻値の型
			NodeId Trailing = NoNode;
			Core::SyntaxModel::HasNodeMatching(
				Syntax,
				Syntax.GetFieldChild(Node, ChildRole::Declarator),
				[this, &Trailing](const NodeId Inner) -> bool {
					// 後置の戻値の型でないノードの返戻
					if(!Syntax.Is(Inner, Role::TrailingReturnType)) return false;
					Trailing = Inner;
					// 発見で打切の返戻
					return true;
				}
			);
			// 後置の型が無ければ本体から推論する事の返戻
			if(Trailing == NoNode) return Inferred();
			Type = Syntax.GetLastNamedChild(Trailing);
		}
		// アサーションのアノテーションが値を返さない事の返戻
		if(Syntax.Is(Type, Role::AssertsAnnotation)) return { ReturnKind::Void, ReturnEvidence::Declared };
		// 型のアノテーションの中の型の選択
		if(Syntax.Is(Type, Role::TypeAnnotation)) Type = Syntax.GetLastNamedChild(Type);
		// 型名と，標準の供給元を持つ限定名の確認
		const std::string TypeName = Syntax.GetText(Type);
		const size_t Separator = TypeName.rfind('.');
		// 語の定義と可視導入から確認する無値・非同期の無値・戻らない型
		const auto IsStandardType = [&](const Word Meaning) -> bool {
			// 文法の語又は標準の供給元を確認出来ない事の返戻
			if(Type == NoNode || !Syntax.GetNode(Type).Facts.ResultMeanings.test(static_cast<size_t>(Meaning))) return false;
			// 絶対修飾を除いた供給元の名前と探索の始点
			const std::string_view AbsolutePrefix = Rules.GetSpelling(Word::AbsoluteNamePrefix);
			const bool IsGlobal = !AbsolutePrefix.empty() && TypeName.starts_with(AbsolutePrefix);
			NodeId Supplier = Type;
			if(IsGlobal) {
				while(Syntax.Is(Supplier, Role::QualifiedNameNode)) {
					Syntax.Charge(1);
					Supplier = Syntax.GetNamedChild(Supplier, 0);
				}
				Supplier = Syntax.GetLastNamedChild(Supplier);
			}
			// 同名の利用者型・型仮引数・インポートに隠されて居ない事の返戻
			return !HasShadowingDecl(Syntax.GetFirstEdge(Supplier).Leaf, IsGlobal, Separator != std::string::npos, Meaning);
		};
		// 値を返さない本体の返戻
		if(IsStandardType(Word::VoidType)) return { ReturnKind::Void, ReturnEvidence::Declared };
		// 非同期の本体は値を返さず，呼出はラッパーの値を返す事の返戻
		if(IsAsync && IsStandardType(Word::AsyncVoidType)) return { ReturnKind::Void, ReturnEvidence::Declared, true };
		// 戻らない型の返戻
		if(IsStandardType(Word::NeverType)) return { ReturnKind::Never, ReturnEvidence::Declared };
		// エントリーポイント又は値を返す関数の区分の返戻
		return { IsProgramEntry(Node, Type) ? ReturnKind::Main : ReturnKind::Val, ReturnEvidence::Declared };
	}

	/**
	 * 導入名が属する最内の条件付の実行範囲の取得関数
	 * 親・役割に依る所属だけを名前のインデックスで共有し，構造の変更時はインデックスと共に失効する
	 * 計算量：同じ関数境界を使う導入全体の祖先数 N に対し期待値 O(N)，同じ経路の再照会は期待値 O(1)
	 * @param Index 導入の所属を共有する名前表
	 * @param Binding 可視の導入の記録
	 * @return 最内の条件付の実行範囲（条件に依らなければ NoNode）
	 * @throws FormatLimitExceeded 祖先の照会が作業予算を超える場合
	 * @throws std::bad_alloc 祖先の列又は所属の表を確保出来ない場合
	 * @throws std::length_error 祖先の列又は所属の表の要素数上限を超える場合
	 * @throws std::out_of_range ノードが表に無い場合
	 */
	NodeId ControlFlow::GetCallableGuard(const CallableIndex &Index, const CallableIntroduction &Binding) const {
		Syntax.Charge(1);
		// 同じ導入の所属の返戻
		std::vector<NodeId> Path;
		const auto SaveGuard = [&](const NodeId Guard) -> NodeId {
			for(const NodeId Child : Path) {
				Syntax.Charge(1);
				Index.GuardPaths.emplace(static_cast<uint64_t>(Binding.Function) << 32 | Child, Guard);
			}
			// 同じ祖先を持つ導入にも共有する所属の返戻
			return Guard;
		};
		for(
			NodeId Child = Binding.Name, Owner = Syntax.GetParentOf(Child);
			Owner != NoNode;
			Child = Owner, Owner = Syntax.GetParentOf(Owner)
		) {
			Syntax.Charge(1);
			const uint64_t Key = static_cast<uint64_t>(Binding.Function) << 32 | Child;
			if(
				const std::unordered_map<uint64_t, NodeId>::const_iterator Found = Index.GuardPaths.find(Key);
				Found != Index.GuardPaths.end()
			// 同じ祖先の所属を読み直さない事の返戻
			) return SaveGuard(Found->second);
			Path.push_back(Child);
			if(IsFlowBoundary(Owner) && Owner != Binding.Function) break;
			const KindDef *const Def = Syntax.GetDefOf(Owner);
			const bool IsConditionalInput =
			Def && !Def->Evaluation.ConditionalOperators.empty() && Child != Syntax.GetNamedChild(Owner, 0) &&
			std::ranges::find(Def->Evaluation.ConditionalOperators, Syntax.GetLexemeOf(Syntax.GetFieldChild(Owner, ChildRole::Operator))) !=
			Def->Evaluation.ConditionalOperators.end();
			if(
				Syntax.IsAny(
					Owner,
					{
						Role::CaseClause,
						Role::CatchClause,
						Role::Conditional,
						Role::ConditionalOperator,
						Role::If,
						Role::Loop,
						Role::PreprocBranch,
						Role::TryStmt
					}
				) || IsConditionalInput || (Syntax.Is(Owner, Role::BinaryExpr) || Syntax.Is(Owner, Role::ConditionalAccessExpr)) &&
				GetChildEvaluation(Owner, Child) != EvaluationClass::Required
			// 同じ構文の別の枝へ確定した供給値を渡さない実行範囲の返戻
			) return SaveGuard(Syntax.Is(Owner, Role::PreprocBranch) ? Owner : Child);
		}
		// 条件枝に依らない所属の返戻
		return SaveGuard(NoNode);
	}

	/**
	 * 呼出先の括弧の除去関数
	 * 計算量：括弧の深さ H に対し O(H)
	 * @param Node 呼出先又は別名の参照先
	 * @return 単一の中身を包む括弧を除いたノード
	 * @throws std::out_of_range 子のノードが表に無い場合
	 */
	NodeId ControlFlow::UnwrapCallTarget(NodeId Node) const {
		// 単一の呼出先を包む括弧の除去
		while(Syntax.Is(Node, Role::GroupingParen) && Syntax.GetNamedChildCount(Node) == 1) Node = Syntax.GetNamedChild(Node, 0);
		// 呼出先の返戻
		return Node;
	}

	/**
	 * 呼出の先頭名の投影済の比較番号の取得関数
	 * 計算量：投影番号を直接使う場合は O(1)，綴りの一致又は逆引を要する場合は名前長 B に対し期待値 O(B)
	 * @param Head 呼出の先頭名
	 * @param HeadName 先頭名の所有テキストへの参照
	 * @return 導入と比較する名前番号（無ければ０）
	 * @throws FormatLimitExceeded 名前の照会が作業予算を超える場合
	 * @throws std::out_of_range 名前又はノードの番号が範囲外の場合
	 */
	uint32_t ControlFlow::GetCallHeadNameIdx(const NodeId Head, const std::string &HeadName) const {
		const SyntaxNode &HeadNode = Syntax.GetNode(Head);
		// 同じ投影済の比較名を，メンバ代入と呼出の解決の双方で使う
		if(
			Syntax.GetRules().AreFunctionNamesCaseInsensitive && Syntax.Is(Head, Role::NameToken) || Syntax.Is(Head, Role::ParamName) ||
			Syntax.Is(Head, Role::TypeName) || Syntax.Is(Head, Role::NamespaceName)
		// 名前の役割に従う共通の比較番号の返戻
		) return Syntax.GetCmpNameIdx(Head);
		// 投影時と同じ綴りの番号の返戻
		if(HeadNode.Facts.NameIdx && Syntax.GetCmpNameView(Head) == std::string_view(HeadName)) return HeadNode.Facts.NameIdx;
		const std::unordered_map<std::string, uint32_t>::const_iterator Found = Syntax.CmpNameIndexes.find(HeadName);
		// 綴りの照会で得た番号の返戻
		return Found == Syntax.CmpNameIndexes.end() ? 0 : Found->second;
	}

	/**
	 * 呼出先の共通取得関数
	 * 計算量：呼出の直接の子数 C と括弧の深さ H に対し O(C + H)
	 * @param Node 呼出のノード
	 * @return 意味を持たない括弧を除いた呼出先
	 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
	 * @throws std::out_of_range 子のノードが表に無い場合
	 */
	NodeId ControlFlow::GetCallTarget(const NodeId Node) const {
		// 呼出先のフィールドの選択
		NodeId Function = Syntax.GetFieldChild(Node, Syntax.Is(Node, Role::MacroCall) ? ChildRole::Macro : ChildRole::Function);
		if(Function == NoNode) Function = Syntax.GetNamedChild(Node, 0);
		// 呼出先を纏めるだけの括弧（行分割で包んだ物を含む）を除いた呼出先の返戻
		return UnwrapCallTarget(Function);
	}

	/**
	 * 終端の要約表を要しない単純な本体の確認関数
	 * 偽枝の無い分岐と前判定の繰返の本体の終端を，外側の確実な終端へ合成しない
	 * 評価時機が未知の構文・必須経路の呼出・捕捉・後始末は詳細判定へ戻す
	 * 計算量：初回の部分木数 N と呼出の解決費用 Q に対し O(N + Q)，同じ本体の再照会は O(1)
	 * @param Body 文の並びの根
	 * @return 単純な文・式の並びなら末尾の到達可能性，其の他は空
	 * @throws FormatLimitExceeded 構文又は名前の走査が作業予算を超える場合
	 * @throws std::bad_alloc 名前又は走査列を確保出来ない場合
	 * @throws std::length_error 名前又は走査列の長さが上限を超える場合
	 */
	std::optional<EndReachability> ControlFlow::GetSimpleEndReach(const NodeId Body) const {
		#if defined(ShaveFormatRefFacts)
		// 基準側は詳細な子の要約から独立して判定する
		static_cast<void>(Body);
		// 詳細な判定へ渡す空の結果の返戻
		return std::nullopt;
		#else
		if(SimpleBody == Body) {
			Syntax.Charge(1);
			// 同じ本体の確認結果の返戻
			return SimpleReach;
		}
		const NodeId Last = Syntax.GetLastStmt(Body);
		const bool HasTailReturn = Syntax.Is(Last, Role::FlowReturn);
		struct PendingNode {
			NodeId Node;
			bool IsConditional = false; // 外側の終端へ合成しない本体に属するか
		};
		bool IsComplex = false;
		for(std::vector<PendingNode> Pending { { Body } }; !Pending.empty();) {
			const PendingNode Cur = Pending.back();
			const NodeId Node = Cur.Node;
			Pending.pop_back();
			Syntax.Charge(1);
			const KindDef *const Def = Syntax.GetDefOf(Node);
			// 関数境界・未知の構文・特殊な呼出は詳細な位置の契約に委ねる
			if(!Def || IsFlowBoundary(Node) || Syntax.Is(Node, Role::Error) || Syntax.Is(Node, Role::MacroCall)) {
				IsComplex = true;
				break;
			}
			const EvaluationDef &Evaluation = Def->Evaluation;
			const bool IsBranch = Evaluation.Rule == EvaluationRule::Branch, IsLoop = Evaluation.Rule == EvaluationRule::Loop;
			// 型・呼出時機・捕捉の追加の事実を要する位置の除外
			if(
				Evaluation.HasDeferredArguments || Evaluation.DoesEvaluateChildInputs || Evaluation.HasEntry && !IsBranch && !IsLoop ||
				Evaluation.Children == EvaluationClass::Unknown || Evaluation.Children == EvaluationClass::Deferred ||
				Evaluation.Children == EvaluationClass::Invocation || Evaluation.MaySkipInputs ||
				!Evaluation.UnknownHead.empty() && Syntax.GetLexemeOf(Syntax.GetNamedChild(Node, 0)) == Evaluation.UnknownHead ||
				Evaluation.CatchInput != ChildRole::None
			) {
				IsComplex = true;
				break;
			}
			NodeId ConditionalBody = NoNode;
			switch(HasTailReturn && Node == Last ? EvaluationRule::Expr : Evaluation.Rule) {
			case EvaluationRule::None:
			case EvaluationRule::Container:
			case EvaluationRule::Eager:
			case EvaluationRule::Expr:
			case EvaluationRule::Stmts:
				break;
			case EvaluationRule::Branch:
				{
					const BranchParts &Parts = GetBranchesOf(Node);
					if(Parts.HasElse || !Parts.Alternatives.empty() || Parts.Then == NoNode || Syntax.GetParentOf(Parts.Then) != Node) {
						IsComplex = true;
						break;
					}
					ConditionalBody = Parts.Then;
					break;
				}
			case EvaluationRule::Loop:
				{
					// 前判定の条件が反復の継続を仮定せず，関数の可視名にも依存しない場合だけ扱う
					TerminationCtx Ctx { NoNode };
					if(
						!Syntax.Is(Node, Role::WhileLoop) || Syntax.Is(Node, Role::PostCondLoop) || Syntax.Is(Node, Role::PostfixLoop) ||
						Syntax.GetRules().DoesReachabilityUseConstantCond || Syntax.GetFieldChild(Node, ChildRole::Alternative) != NoNode ||
						LoopTermination(Ctx, Node, ConditionalBody) || Ctx.DoesUseFunction || ConditionalBody == NoNode ||
						Syntax.GetParentOf(ConditionalBody) != Node
					) IsComplex = true;
					break;
				}
			default:
				// 移動・捕捉等は詳細判定へ戻す
				IsComplex = true;
				break;
			}
			if(IsComplex) break;
			for(const EvaluationInput &Input : Evaluation.Inputs) {
				Syntax.Charge(1);
				// 選択条件が成立しない場合も保守的に詳細判定へ戻す
				if(Input.Class == EvaluationClass::Unknown || Input.Class == EvaluationClass::Invocation) {
					IsComplex = true;
					break;
				}
			}
			if(IsComplex) break;
			if(Syntax.Is(Node, Role::CallNode)) {
				NodeId Head = Syntax.GetFieldChild(Node, ChildRole::Function);
				if(Head == NoNode) Head = Syntax.GetNamedChild(Node, 0);
				// 未展開の実引数の評価と，必須経路の非復帰契約は詳細判定へ渡す
				if(
					Head != NoNode && (
						Syntax.GetNode(Head).Facts.Has(ConstraintKind::MacroBinding) ||
						Syntax.GetNode(Head).Facts.Has(ConstraintKind::UncertainMacroArguments)
					) || !Cur.IsConditional && !CanSkipMemberResolution(GetCallTarget(Node))
				) {
					IsComplex = true;
					break;
				}
			}
			// 条件は必須経路の儘，分岐と繰返の任意の本体だけを外側の終端へ合成しない
			const std::span<const NodeId> Children = Syntax.GetChildrenOf(Node);
			for(std::span<const NodeId>::reverse_iterator Child = Children.rbegin(); Child != Children.rend(); ++Child) {
				Pending.push_back({ *Child, Cur.IsConditional || *Child == ConditionalBody });
			}
		}
		SimpleBody = Body;
		SimpleReach =
		IsComplex ? std::nullopt : std::optional(HasTailReturn ? EndReachability::Unreachable : EndReachability::Reachable);
		// 単純な本体の末尾の到達可能性の返戻
		return SimpleReach;
		#endif
	}

	/**
	 * 送出の効果と捕捉条件の一致を判定する関数
	 * 計算量：捕捉条件のノード数 N と名前解決の費用 Q に対し O(N + Q)
	 * @param Handler 捕捉の節
	 * @param Body 捕捉時の本体
	 * @param Effect 本体から伝播する送出の効果
	 * @return 捕捉するなら真，捕捉しないなら偽，型・束縛が未確定なら空
	 * @throws FormatLimitExceeded 条件の探索又は名前解決が作業予算を超える場合
	 * @throws std::bad_alloc 条件の探索列又は名前表を確保出来ない場合
	 * @throws std::length_error 探索列又はテキストの要素数上限を超える場合
	 * @throws std::out_of_range 条件のノード又は名前が表に無い場合
	 */
	std::optional<bool> ControlFlow::MatchesException(const NodeId Handler, const NodeId Body, const uint8_t Effect) const {
		// 非復帰だけの契約から捕捉を確定しない
		if(Effect == UnknownExitEffect) return std::nullopt;
		// 型を明記しない捕捉式は，通常の送出を受け得るが非復帰だけの契約とは混同しない
		if(const KindDef *const Def = Syntax.GetDefOf(Handler); Def && Def->Evaluation.CatchInput != ChildRole::None) {
			// 通常の送出だけを受ける捕捉式の返戻
			return Effect == ThrowEffect;
		}
		bool HasType = false, IsUnknown = false, IsMatch = false;
		Core::SyntaxModel::HasStoppedTraversal(
			Syntax,
			Handler,
			[&](const NodeId Part) -> WalkStep {
				// 本体と捕捉先の別名を除き，捕捉条件だけを調べる
				if(Part == Body || Syntax.HasField(Part, ChildRole::Alias)) return WalkStep::Skip;
				if(Syntax.IsWord(Part, Word::CatchAllException)) {
					IsMatch = true;
					// 型を問わない捕捉の確定
					return WalkStep::Stop;
				}
				if(
					Effect == SystemExitEffect && Part != Handler && Syntax.IsNamed(Part) && Syntax.GetChildCount(Part) &&
					!Syntax.Is(Part, Role::AliasPattern) && !Syntax.Is(Part, Role::TupleLike) && !Syntax.Is(Part, Role::GroupingParen)
				) {
					HasType = IsUnknown = true;
					// 修飾名・呼出等の中の綴りだけで標準型へ解決しない
					return WalkStep::Skip;
				}
				// 空の型タプルは，型を問わない捕捉と区別する
				if(Syntax.Is(Part, Role::TupleLike)) HasType = true;
				// 名前を持つ葉へ降りる事の返戻
				if(Syntax.GetChildCount(Part) || !Syntax.IsNamed(Part)) return WalkStep::Descend;
				HasType = true;
				if(Effect == SystemExitEffect) {
					const std::shared_ptr<const ReferenceFacts> &Reference = Syntax.GetNode(Part).Facts.Reference;
					if(Reference && Reference->Meanings.test(static_cast<size_t>(Word::SystemExitHandlerType))) IsMatch = true;
					else if(!Reference || !Reference->Meanings.test(static_cast<size_t>(Word::SystemExitExcludedType))) IsUnknown = true;
				} else IsUnknown = true;
				// 他の型候補も調べる
				return WalkStep::Descend;
			}
		);
		// 全例外又は一致する型の捕捉
		if(IsMatch || !HasType) return true;
		// 型の階層と束縛に依る結果の返戻
		return IsUnknown ? std::nullopt : std::optional<bool>(false);
	}

	/**
	 * 共有契約に従う入口の必須評価の合成関数
	 * 計算量：直接の入力の数 C と位置の照会費用 Q に対し O(C Q)
	 * @param Known 判定済の入力の要約
	 * @param Node 入力の親
	 * @param Scope 制御移動の判定範囲
	 * @param Operands 投影した入力の並び
	 * @param Excluded 条件付の反復本体（無ければ無）
	 * @return 入口の終端と，終端より前に生じ得る効果
	 * @throws FormatLimitExceeded 構文又は要約の照会が作業予算を超える場合
	 * @throws std::out_of_range ノード又は要約の識別子が範囲外の場合
	 */
	ControlFlow::TerminationSummary ControlFlow::SummarizeEntry(
		const TerminationResults &Known,
		const NodeId Node,
		const NodeId Scope,
		const std::span<const NodeId> Operands,
		const NodeId Excluded
	) const {
		// 必須評価の入口の合成
		TerminationSummary Result;
		for(const NodeId Operand : Operands) {
			Syntax.Charge(1);
			if(Operand == NoNode || Operand == Excluded || GetChildEvaluation(Node, Operand) != EvaluationClass::Required) continue;
			const TerminationSummary Part = GetKnownSummary(Known, Operand, Scope);
			Result.Effects |= Part.Effects;
			Result.Modes |= Part.Modes;
			Result.HasUnknownEvaluation |= Part.HasUnknownEvaluation;
			if(Part.IsAbrupt) {
				Result.IsAbrupt = true;
				break;
			}
		}
		// 入口の要約の返戻
		return Result;
	}

	/**
	 * 例外処理の本体・実行可能な捕捉・後始末の終端と効果を合成する関数
	 * 計算量：捕捉条件の走査量 N と名前解決の費用 Q に対し O(N + Q)
	 * @param Known 判定済の子の要約
	 * @param Node 例外処理文
	 * @param TransferScope 制御移動の判定範囲
	 * @return 終端の仮定と外側へ伝播する効果
	 * @throws FormatLimitExceeded 条件の探索又は名前解決が作業予算を超える場合
	 * @throws std::bad_alloc 節又は名前表を確保出来ない場合
	 * @throws std::length_error 節又はテキストの要素数上限を超える場合
	 * @throws std::out_of_range ノード又は名前が表に無い場合
	 */
	ControlFlow::TerminationSummary ControlFlow::TryTermination(
		const TerminationResults &Known,
		const NodeId Node,
		const NodeId TransferScope
	) const {
		// 本体と捕捉・後始末の効果の合成
		const TryParts &Parts = GetTryPartsOf(Node);
		TerminationSummary Result = GetKnownSummary(Known, Parts.Body, TransferScope);
		const TerminationSummary Entry = SummarizeEntry(Known, Node, TransferScope, Parts.Entries, NoNode);
		if(Entry.IsAbrupt) Result = Entry;
		else {
			Result.Modes |= Entry.Modes;
			Result.Effects |= Entry.Effects;
			Result.HasUnknownEvaluation |= Entry.HasUnknownEvaluation;
		}
		Result.TransferSource = NoNode;
		if(Parts.Operator != NoNode) {
			const uint8_t Raised = Result.Effects & (ThrowEffect | UnknownExitEffect);
			if(Syntax.IsWord(Parts.Operator, Word::OptionalTry)) {
				// 送出を値に変える試行では，捕捉に依って通常完了する経路を残す
				if(Raised) {
					Result.Modes = 0;
					Result.IsAbrupt = false;
					Result.Effects &= static_cast<uint8_t>(~Raised);
					if(Raised & UnknownExitEffect) Result.Effects |= ProcessExitEffect;
				}
			// 強制した試行の送出は捕捉へ渡さず，プロセス終了に変わる
			} else if(Syntax.IsWord(Parts.Operator, Word::ForcedTry) && Raised) {
				Result.Effects = Result.Effects & static_cast<uint8_t>(~Raised) | ProcessExitEffect;
			}
		}
		// リソースの初期化と後始末の送出も，同じ捕捉節へ渡す
		if(Syntax.Is(Node, Role::ResourceBody) && !Entry.IsAbrupt) Result.Effects |= ThrowEffect;
		const TerminationSummary Else = GetKnownSummary(Known, Parts.Else, TransferScope);
		const uint8_t Raised = Result.Effects & (SystemExitEffect | ThrowEffect | UnknownExitEffect);
		Result.Effects &= static_cast<uint8_t>(~Raised);
		if(!(Result.Modes & CertainTermination)) Result.Effects |= Else.Effects;
		Result.Modes |= Else.Modes;
		Result.HasUnknownEvaluation |= Else.HasUnknownEvaluation;
		Result.IsAbrupt = Result.IsAbrupt || Else.IsAbrupt;
		// 捕捉は送出された対象だけへ記述順に適用し，捕捉の中の送出は同じ捕捉列へ戻さない
		for(const uint8_t Effect : { SystemExitEffect, ThrowEffect, UnknownExitEffect }) {
			if(!(Raised & Effect)) continue;
			uint8_t Routes = AllTermination, Escaping = Effect, HandlerEffects = 0, EntryModes = 0;
			bool AreRoutesAbrupt = true;
			for(const NodeId Handler : Parts.Handlers) {
				Syntax.Charge(1);
				const NodeId Body = GetHandlerBody(Handler);
				// 型の照合より先に捕捉条件を評価し，此処の送出は外側へ渡す
				TerminationSummary Test;
				if(Handler != Node) Test = SummarizeEntry(Known, Handler, TransferScope, Syntax.GetChildrenOf(Handler), Body); // 捕捉式自身を節に使う形には，独立した型の見出が無い
				Result.HasUnknownEvaluation |= Test.HasUnknownEvaluation;
				HandlerEffects |= Test.Effects;
				EntryModes |= Test.Modes;
				if(Test.IsAbrupt) {
					Routes &= EntryModes;
					// 未確定の非復帰契約には，捕捉条件を実行しないプロセス終了も残す
					Escaping = Effect == UnknownExitEffect ? ProcessExitEffect : 0;
					break;
				}
				const std::optional<bool> Match = MatchesException(Handler, Body, Effect);
				if(Match && !*Match) continue;
				const TerminationSummary Caught = GetKnownSummary(Known, Body, TransferScope);
				Result.HasUnknownEvaluation |= Caught.HasUnknownEvaluation;
				AreRoutesAbrupt = AreRoutesAbrupt && Caught.IsAbrupt;
				if(Match) {
					Routes &= Caught.Modes | EntryModes;
					Escaping = 0;
					HandlerEffects |= Caught.Effects;
					break;
				}
				// 捕捉の不一致を確定出来ない経路も，後続への経路として合成する
				Routes &= Caught.Modes | EntryModes;
				HandlerEffects |= Caught.Effects;
			}
			Result.Modes &= Routes;
			Result.IsAbrupt = Result.IsAbrupt && AreRoutesAbrupt;
			Result.Effects |= Escaping | HandlerEffects;
		}
		const bool CanRunFinally = !(Result.Modes & CertainTermination) || Result.Effects & ~ProcessExitEffect;
		// Java の有効な捕捉節は，非検査例外又は送出可能な検査例外を受ける（JLS 14.22）
		// 型の階層・送出を確認出来ない節も除外せず，C# の全捕捉節と同じく到達可能側へ合成する
		// 此の合成はコンパイル時の到達可能性だけを変え，プロセス終了を捕捉する効果は作らない
		if(Syntax.GetRules().HasStaticCatchReachability) for(const NodeId Handler : Parts.Handlers) {
			Syntax.Charge(1);
			const NodeId Body = GetHandlerBody(Handler);
			const TerminationSummary Caught = GetKnownSummary(Known, Body, TransferScope);
			Result.HasUnknownEvaluation |= Caught.HasUnknownEvaluation;
			Result.Modes &= Caught.Modes;
			Result.IsAbrupt = Result.IsAbrupt && Caught.IsAbrupt;
		}
		// コンパイル時にも後始末の終端を優先するが，プロセス終了後の実行時の効果は加えない
		if(CanRunFinally || Syntax.GetRules().HasStaticCatchReachability) {
			const TerminationSummary Finally = GetKnownSummary(Known, Parts.Finally, TransferScope);
			Result.HasUnknownEvaluation |= Finally.HasUnknownEvaluation;
			// 後始末のジャンプは，元の返戻・脱出の仮定も置き換える
			Result.Modes = Finally.IsAbrupt ? Finally.Modes : Result.Modes | Finally.Modes;
			Result.IsAbrupt = Result.IsAbrupt || Finally.IsAbrupt;
			if(Finally.IsAbrupt) Result.TransferSource = Parts.Finally;
			if(CanRunFinally) {
				if(Finally.IsAbrupt) Result.Effects &= ProcessExitEffect;
				Result.Effects |= Finally.Effects;
			}
		}
		// 例外処理全体の要約の返戻
		return Result;
	}

	/**
	 * 本体を１度は実行する繰返かの判定関数
	 * @param Node 繰返のノード
	 * @param Body 繰返の本体
	 * @return 後判定の文，又は明示の語で囲む本体に付く後置の繰返なら true
	 */
	bool ControlFlow::IsPostCondLoop(const NodeId Node, const NodeId Body) const {
		// 後判定の役割と本体の構成からの返戻
		return Syntax.Is(Node, Role::PostCondLoop) || Syntax.Is(Node, Role::PostfixLoop) && Syntax.Is(Body, Role::ExplicitKeywordBody);
	}

	/**
	 * 条件で終わらない繰返かの判定関数
	 * 計算量：子の抽出と条件判定の総費用 Q に対し O(Q)
	 * @param Ctx 同じ走査で共有する関数と可視名
	 * @param Node 判定対象ノード
	 * @param Body 繰返の本体の格納先（繰返でなければ空）
	 * @return 条件だけで反復が終わらない仮定の位
	 * @throws FormatLimitExceeded 抽出又は照会が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	uint8_t ControlFlow::LoopTermination(TerminationCtx &Ctx, const NodeId Node, NodeId &Body) const {
		// 条件で終わらない繰返かの判定
		Body = Syntax.GetFieldChild(Node, ChildRole::Body);
		// 後置の繰返は本体を先頭に持ち，条件の真偽の向きは此処では仮定しない
		if(Syntax.Is(Node, Role::PostfixLoop)) {
			if(Body == NoNode) Body = Syntax.GetNamedChild(Node, 0);
			// 条件だけからは終端を確定しない事の返戻
			return 0;
		}
		// Rust の無条件の繰返は常に続く事の返戻
		if(Syntax.Is(Node, Role::LoopExpr)) return AllTermination;
		if(Syntax.Is(Node, Role::CountedLoop)) {
			if(Syntax.Is(Node, Role::ClauseOrCondLoop)) {
				// Go は条件の節で条件を省くか，節も条件も持たない形が無条件
				if(const NodeId Clause = Syntax.GetFirstNamedChildWithRole(Node, Role::ForClause); Clause != NoNode) {
					const NodeId LocalCond = Syntax.GetFieldChild(Clause, ChildRole::Cond);
					// 条件の無い節か常真の条件かの返戻
					return LocalCond == NoNode ? AllTermination : GetCondTermination(Ctx, LocalCond);
				}
				// 範囲の反復は要素が尽きて終わる事の返戻
				if(Syntax.GetFirstNamedChildWithRole(Node, Role::RangeClause) != NoNode) return 0;
				const NodeId First = Syntax.GetNamedChild(Node, 0);
				// 本体だけの繰返か常真の条件かの返戻
				return First == Body ? AllTermination : GetCondTermination(Ctx, First);
			}
			// 条件を省ける C 形式の for は C 系・C#・Java ･ JS/TS ･ PHP だけ
			// 其の他の言語の for は要素の反復である事の返戻
			if(!Syntax.Is(Node, Role::OmittableCondLoop)) return 0;
			const NodeId LocalCond = Syntax.GetFieldChild(Node, ChildRole::Cond);
			// JS/TS の省いた条件は区切だけのトークン又は空文で表す
			const bool IsOmitted = LocalCond == NoNode || !Syntax.IsNamed(LocalCond) || Syntax.Is(LocalCond, Role::EmptyStmt);
			// 省いた条件か常真の条件かの返戻
			return IsOmitted ? AllTermination : GetCondTermination(Ctx, LocalCond);
		}
		const bool IsDoLoop = Syntax.Is(Node, Role::PostCondLoop);
		// 条件で終わる繰返以外は無条件の繰返でない事の返戻
		if(!IsDoLoop && !Syntax.Is(Node, Role::WhileLoop)) return 0;
		NodeId LocalCond = Syntax.GetFieldChild(Node, ChildRole::Cond);
		// フィールドを持たない Kotlin と，本体のフィールドを持たない Swift の子の並びからの取得
		if(LocalCond == NoNode || Body == NoNode) {
			NodeId First = NoNode, Last = NoNode;
			Core::SyntaxModel::ForEachNamedChildOf(
				Syntax,
				Node,
				[&](const NodeId Child) -> void {
					if(First == NoNode) First = Child;
					Last = Child;
					// 終了
					return;
				}
			);
			// 後置の条件の繰返は本体が先，其の他は条件が先
			if(LocalCond == NoNode) LocalCond = IsDoLoop ? Last : First;
			if(Body == NoNode && First != Last) Body = IsDoLoop ? First : Last;
			// 空の本体は Swift の本体の子を持たない
			if(Body != NoNode && Body == LocalCond) Body = NoNode;
		}
		// Swift の複数の条件フィールドの仮定の共通の位
		uint8_t Modes = AllTermination;
		bool HasConditions = false;
		for(const NodeId Child : Syntax.GetChildrenOf(Node)) {
			Syntax.Charge(1);
			// 条件フィールドの常真性の判定
			if(Syntax.HasField(Child, ChildRole::Cond)) {
				HasConditions = true;
				Modes &= GetCondTermination(Ctx, Child);
			}
		}
		// 条件が常真かの返戻
		return HasConditions ? Modes : GetCondTermination(Ctx, LocalCond);
	}

	/**
	 * 投影した制御構文の条件又は対象の取得関数
	 * 計算量：直接の子の数 C に対し O(C)
	 * @param Node 制御構文
	 * @return 入口で評価する条件又は対象（無ければ無）
	 * @throws FormatLimitExceeded 構文又は要約の照会が作業予算を超える場合
	 * @throws std::out_of_range ノード又は要約の識別子が範囲外の場合
	 */
	NodeId ControlFlow::GetEvaluationCondition(const NodeId Node) const {
		// 条件の投影先の選択
		const KindDef *const Def = Syntax.GetDefOf(Node);
		// 固定の定義を持たない場合の返戻
		if(!Def) return NoNode;
		// 名前付の位置で指定した条件の返戻
		if(Def->Evaluation.ConditionIndex) return Syntax.GetNamedChild(Node, *Def->Evaluation.ConditionIndex);
		const NodeId Cond = Syntax.GetFieldChild(Node, Def->Evaluation.ConditionField);
		// フィールド又は入口の役割に対応する条件の返戻
		return Cond != NoNode ? Cond : Syntax.GetFirstNamedChildWithRole(Node, Role::EvaluationCondition);
	}

	/**
	 * 判定済の子からの終端判定関数
	 * 計算量：選んだ規則の子の抽出と照会の総費用 Q に対し O(Q)
	 * @param Gotos 同じ関数内の goto ジャンプ先
	 * @param Known 判定範囲毎のノードの終端の判定結果（規則が読む子を全て含む）
	 * @param Node 判定対象ノード
	 * @param Scope 内部転送を認める switch 文の判定範囲
	 * @param Operands 展開時に収集した子の評価対象
	 * @param Rule 此のノードと判定範囲で１度確定した規則
	 * @param LoopBody 反復の本体
	 * @param LoopModes 条件だけで反復が終わらない仮定の位
	 * @param IsAbrupt 直後へ通常完了しない事実の格納先
	 * @return 通常・被覆・悲観の各仮定で終端する位
	 * @throws FormatLimitExceeded 抽出又は照会が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	uint8_t ControlFlow::NodeTermination(
		const GotoIdx &Gotos,
		const TerminationResults &Known,
		const NodeId Node,
		const NodeId Scope,
		const std::span<const NodeId> Operands,
		const TerminationRule Rule,
		const NodeId LoopBody,
		const uint8_t LoopModes,
		bool &IsAbrupt
	) const {
		// 判定済の子からの終端判定
		switch(Rule) {
		case TerminationRule::Container:
		case TerminationRule::Eager:
			// 必須の子の終端は合成時に評価順と共に読む
			return 0;
		case TerminationRule::Choice:
			{
				const TerminationSummary Then = GetKnownSummary(Known, Operands[1], Scope);
				const TerminationSummary Else = GetKnownSummary(Known, Operands[2], Scope);
				IsAbrupt = Then.IsAbrupt && Else.IsAbrupt;
				// 入口の条件は共有合成へ委ね，両枝に共通する終端だけを返す
				return Then.Modes & Else.Modes;
			}
		case TerminationRule::ShortCircuit:
			{
				const TerminationSummary First = GetKnownSummary(Known, Operands.empty() ? NoNode : Operands.front(), Scope);
				IsAbrupt = First.IsAbrupt;
				// 短絡の先頭の評価に依る終端の返戻
				return First.Modes;
			}
		case TerminationRule::Macro:
			return LoopModes;
		case TerminationRule::Expr:
			// 式の文の中の式の終端の返戻
			return GetKnownTermination(Known, Syntax.GetNamedChild(Node, 0), Scope);
		case TerminationRule::Transfer:
			// 制御を転送する文の返戻
			return AllTermination;
		case TerminationRule::Labeled:
			{
				const uint32_t Count = Syntax.GetNamedChildCount(Node);
				// 文を持たないラベルは終端しない事の返戻
				if(!Count) return 0;
				const NodeId Child = Syntax.GetNamedChild(Node, Count - 1);
				// 自身のラベルへの脱出は，文全体では通常完了する
				if(HasLoopExit(Gotos, Known, Child, Node, Scope)) return 0;
				const TerminationSummary Body = GetKnownSummary(Known, Child, Scope);
				IsAbrupt = Body.IsAbrupt;
				// ラベル末尾の終端の返戻
				return Body.Modes;
			}
		case TerminationRule::Fallthrough:
			{
				const NodeId Next = Syntax.GetNextNamedSibling(GetFallthroughEntry(Node, Scope));
				// 次節が有る場合だけ内部転送とする事の返戻
				return Next != NoNode && Syntax.Is(Next, Role::SwitchEntry) && Syntax.HasNodeWithin(Scope, Next) ? AllTermination : 0;
			}
		case TerminationRule::KeywordJump:
			// 先頭の制御語が脱出の語かの返戻
			return Syntax.Is(GetFirstChildNode(Node), Role::ExitKeyword) ? AllTermination : 0;
		case TerminationRule::InfiniteLoop:
			// 脱出の無い無限繰返として終端の返戻
			return HasLoopExit(Gotos, Known, LoopBody, Node, Scope) ? 0 : LoopModes;
		case TerminationRule::Loop:
			{
				// Python の繰返の else 節は脱出せずに終えた時だけ実行する
				const NodeId Else = Syntax.GetFieldChild(Node, ChildRole::Alternative);
				// 条件で繰返が続く仮定と，脱出せずに進む else 節の終端の合成
				uint8_t Modes = LoopModes |
				(Syntax.Is(Node, Role::LoopElse) && Else != NoNode ? GetKnownTermination(Known, Syntax.GetLastNamedChild(Else), Scope) : 0);
				// 後判定では本体を必ず１度実行するが，条件へ継続する経路から本体の終端は導かない
				if(IsPostCondLoop(Node, LoopBody) && !HasLoopExit(Gotos, Known, LoopBody, Node, Scope, true)) {
					Modes |= GetKnownTermination(Known, LoopBody, Scope);
				}
				// 後判定の条件へ進む経路も，脱出が無ければ通常完了する為に条件を評価する
				if(IsPostCondLoop(Node, LoopBody)) Modes |= GetKnownTermination(Known, GetEvaluationCondition(Node), Scope);
				// 脱出せず終端する繰返の仮定の返戻
				return Modes && !HasLoopExit(Gotos, Known, LoopBody, Node, Scope) ? Modes : 0;
			}
		case TerminationRule::Stmts:
			// 文列は順に判定し，終端後の宣言・文で終端性を保持
			// Rust のラベル付のブロックは自身への脱出で素通りする事の返戻
			if(
				Syntax.Is(Node, Role::LabeledBlockExit) && Syntax.GetFirstNamedChildWithRole(Node, Role::JumpLabel) != NoNode &&
				HasLoopExit(Gotos, Known, Node, Node, Scope)
			// 自身のラベルへ抜けるブロックの返戻
			) return 0;
			// 文列が終端する仮定の返戻
			return SeqTermination(Gotos, Known, Operands, Scope);
		case TerminationRule::SingleBody:
			{
				const TerminationSummary Body = GetKnownSummary(Known, GetSingleBodyOf(Node), Scope);
				// 後のリソースの開始と束縛の失敗は，既に開始したリソースが抑止し得る
				if(Syntax.Is(Node, Role::SuppressingBody)) {
					const NodeId Resources = Syntax.GetFirstNamedChildWithRole(Node, Role::ResourceList);
					const NodeId First = Syntax.GetNamedChild(Resources, 0);
					if(
						Body.Effects & (SystemExitEffect | ThrowEffect | UnknownExitEffect) || Syntax.GetNamedChildCount(Resources) > 1 ||
						Syntax.Is(Syntax.GetNamedChild(First, 0), Role::AliasPattern)
					) return 0;
				}
				IsAbrupt = Body.IsAbrupt;
				// 抑止し得る送出が無い本体の返戻・転送・プロセス終了の保持
				return Body.Modes;
			}
		case TerminationRule::Preproc:
			{
				const uint8_t Closed = SeqTermination(Gotos, Known, Operands.first(Operands.size() - 1), Scope);
				// #else 以外は後続の枝が無いと全条件不成立で素通り出来る事の返戻
				return Closed & (
					Syntax.Is(Node, Role::PreprocElse) ?
					AllTermination :
					GetKnownTermination(Known, Syntax.GetFieldChild(Node, ChildRole::Alternative), Scope)
				);
			}
		case TerminationRule::Branch:
			// 全ての枝の終端の返戻
			return BranchTermination(Gotos, Known, Node, Scope, IsAbrupt);
		case TerminationRule::Switch:
			// switch 文の全分岐判定の返戻
			return SwitchTermination(Gotos, Known, Node, Scope);
		default:
			// 上記何れにも非該当のノードの非終端の返戻
			return 0;
		}
	}

	/**
	 * 終端の判定規則の取得関数（子を積む段と結果を合わせる段が同じ規則を選ぶ）
	 * 計算量：役割・条件の照会の総費用 Q に対し O(Q)
	 * @param Ctx 同じ走査で共有する関数と可視名
	 * @param Node 判定するノード
	 * @param Scope 内部転送を認める switch 文の判定範囲
	 * @param LoopBody １度確定した反復の本体の格納先
	 * @param LoopModes 反復を続ける仮定の位の格納先
	 * @param Effects 呼出の効果の格納先
	 * @return ノードに適用する規則
	 * @throws FormatLimitExceeded 抽出又は照会が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	ControlFlow::TerminationRule ControlFlow::GetTerminationRuleOf(
		TerminationCtx &Ctx,
		const NodeId Node,
		const NodeId Scope,
		NodeId &LoopBody,
		uint8_t &LoopModes,
		uint8_t &Effects
	) const {
		// ノードの無い事の返戻
		if(Node == NoNode) return TerminationRule::None;
		const KindDef *const Def = Syntax.GetDefOf(Node);
		const TerminationRule Rule = Def ? Def->Evaluation.Rule : TerminationRule::None;
		if(Rule == TerminationRule::Transfer && Syntax.IsWord(GetFirstChildNode(Node), Word::ProcessExitCall)) {
			Effects = ProcessExitEffect;
		}
		// 語・ジャンプ先が固定の制御演算の返戻
		if(
			Rule == TerminationRule::Expr || Rule == TerminationRule::Transfer || Rule == TerminationRule::Labeled ||
			Rule == TerminationRule::KeywordJump
		) return Rule;
		// 次の枝へ進む語の返戻
		if(GetFallthroughEntry(Node, Scope) != NoNode) return TerminationRule::Fallthrough;
		bool IsRegisteredMacro = false;
		Effects = GetCallEffects(Node, IsRegisteredMacro);
		if(IsRegisteredMacro) {
			LoopModes = Effects ? AllTermination : 0;
			// 登録済マクロの実引数を評価する規則の返戻
			return TerminationRule::Macro;
		}
		// 解決済の非復帰の契約の返戻
		if(Effects && !(Effects & UnknownCallEffect)) return TerminationRule::Transfer;
		if(Rule == TerminationRule::Loop) {
			LoopModes = LoopTermination(Ctx, Node, LoopBody);
			// 確定した反復条件を加えた制御演算の返戻
			return LoopModes == AllTermination ? TerminationRule::InfiniteLoop : Rule;
		}
		// 逐次実行でない節を持つ明示の本体の返戻
		if(
			Rule == TerminationRule::Stmts && Syntax.Is(Node, Role::ExplicitKeywordBody) && Core::SyntaxModel::HasChildMatching(
				Syntax,
				Node,
				[&](const NodeId Child) -> bool {
					// 逐次実行でない節の有無の返戻
					return Syntax.Is(Child, Role::ExceptionSection) || Syntax.Is(Child, Role::FinalSection);
				}
			)
		) return TerminationRule::None;
		// 固定の制御演算の返戻
		if(Rule != TerminationRule::Eager && Rule != TerminationRule::Container) return Rule;
		// 各到達時の実行が確定しないメモリ指定付の初期化の返戻
		if(Syntax.Is(Node, Role::TypedDecl) && Syntax.GetFirstNamedChildWithRole(Node, Role::MemberStorageSpecifier) != NoNode) {
			// 毎回の初期化を仮定しない規則の返戻
			return TerminationRule::None;
		}
		if(Def && !Def->Evaluation.ConditionalOperators.empty()) {
			const std::string_view Operator = Syntax.GetLexemeOf(Syntax.GetFieldChild(Node, ChildRole::Operator));
			// 投影済の短絡演算子の契約の返戻
			if(std::ranges::find(Def->Evaluation.ConditionalOperators, Operator) != Def->Evaluation.ConditionalOperators.end()) {
				// 一致した短絡の規則の返戻
				return TerminationRule::ShortCircuit;
			}
		}
		// 評価位置の契約から選んだ合成規則の返戻
		return Rule;
	}

	/**
	 * 関数の文の並びの取得関数
	 * 計算量：関数と本体の直接の子の数 C に対し O(C)
	 * @param Node 関数・ラムダ又はアクセサのノード
	 * @return 文を直下に並べるノード（本体が無いか式本体なら空）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	NodeId ControlFlow::GetStmtsOf(const NodeId Node) const {
		// 関数の文の並びの取得
		NodeId Body = Syntax.GetFieldChild(Node, ChildRole::Body);
		// フィールドを持たない Kotlin / Swift の関数本体のラッパー
		if(Body == NoNode) Body = Syntax.GetFirstNamedChildWithRole(Node, Role::FunctionBody);
		// 本体のラッパーを持たないクロージャの文の並び
		if(Body == NoNode) Body = Syntax.GetFirstNamedChildWithRole(Node, Role::InnerFunctionStmts);
		// 本体のラッパーを持たず波括弧を直接の子に持つ Kotlin のセカンダリコンストラクタ・Swift のアクセサ
		if(
			Body == NoNode && !Syntax.Is(Node, Role::LambdaLiteral) && Core::SyntaxModel::HasChildMatching(
				Syntax,
				Node,
				[this](const NodeId Child) -> bool {
					return Syntax.IsTokenRole(Child, Role::OpeningBraceToken);
				}
			)
		) Body = Node;
		// 本体のフィールドで波括弧又はキーワードのブロックが包む文の並びの読取
		if((Syntax.Is(Body, Role::LexicalBlock) || Syntax.Is(Body, Role::KeywordBlock)) && !CanExcludeRole(Role::StmtSeq)) {
			if(const NodeId Inner = Syntax.GetFieldChild(Body, ChildRole::Body); Syntax.Is(Inner, Role::StmtSeq)) Body = Inner;
		}
		// 本体が無いか，式本体か，文の並びでない関数の try のブロックである事の返戻
		if(
			Body == NoNode || !Syntax.Is(Body, Role::FunctionBodyContainer) && !Syntax.Is(Body, Role::StmtSeq) && Body != Node ||
			!Syntax.IsBracedBody(Body) && Syntax.HasWordChild(Body, Word::ExprBody) ||
			Syntax.Is(Node, Role::CompoundFunctionBody) && !Syntax.Is(Body, Role::CompoundBody)
		) return NoNode;
		// 文の並びのラッパーの無い構文では，本体の全文を探索しない
		if(!CanExcludeRole(Role::InnerFunctionStmts)) {
			// Kotlin / Swift の `stmts` と Go の `statement_list` が包む文の並びの返戻
			if(const NodeId Inner = Syntax.GetFirstNamedChildWithRole(Body, Role::InnerFunctionStmts); Inner != NoNode) return Inner;
		}
		// 本体の文の並びの返戻
		return Body;
	}

	/**
	 * 呼出時機の照会対象の本体の取得関数
	 * 値の作成だけでは本体へ降りず，呼出へ渡す位置の契約が要求する場合に限る
	 * 計算量：直接の親子の照会費用 C に対し O(C)
	 * @param Node 渡す呼出可能な値
	 * @return 照会対象の文の並び（対象外又は空なら無）
	 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
	 * @throws std::out_of_range 構文の位置が範囲外の場合
	 */
	NodeId ControlFlow::GetInvocationBody(const NodeId Node) const {
		// 呼出時に評価する本体の選択
		const KindDef *const Def = Syntax.GetDefOf(Node);
		// 遅延する本体を持たないノードでは，親の入力の契約を再照会しない
		if(!Def || Def->Evaluation.Children != EvaluationClass::Deferred) return NoNode;
		const NodeId Parent = Syntax.GetParentOf(Node);
		// 本体の実行が未確定な入力だけを調べる
		return Parent != NoNode && GetChildEvaluation(Parent, Node) == EvaluationClass::Invocation ? GetStmtsOf(Node) : NoNode;
	}

	/**
	 * 終端の判定が読む子の収集関数
	 * 規則が短絡で読まずに済ませ得る子も含め，規則が読み得る子を全て積む
	 * 計算量：対象と参照先の子の総数 C，照合するテキストの総長 L に対し O(C + L)
	 * @param Node 判定するノード
	 * @param Rule 此のノードと判定範囲で１度確定した規則
	 * @param LoopBody 判定規則の取得時に確定した繰返の本体
	 * @param Output 子の評価対象の格納先
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void ControlFlow::TerminationOperands(
		const NodeId Node,
		const TerminationRule Rule,
		const NodeId LoopBody,
		std::vector<NodeId> &Output
	) const {
		const KindDef *const Def = Syntax.GetDefOf(Node);
		// 全てが遅延又は非評価の位置は，後続の要約も読まない為に展開しない
		if(
			Def && (Def->Evaluation.Children == EvaluationClass::Unevaluated || Def->Evaluation.Children == EvaluationClass::Deferred) &&
			Def->Evaluation.Inputs.empty()
		) return;
		// 判定対象の子の追加関数
		const auto Add = [&Output](const NodeId Child) -> void {
			Output.push_back(Child);
			// 終了
			return;
		};
		// 遅らせるのは呼出だけであり，呼出先と実引数の評価は此の文に属する
		if(Def && Def->Evaluation.DoesEvaluateChildInputs) {
			const NodeId Call = Syntax.GetNamedChild(Node, 0);
			for(const NodeId Child : Syntax.GetChildrenOf(Call)) if(Syntax.IsNamed(Child)) Add(Child);
			// 呼出自身の効果を含めず収集を終える
			return;
		}
		if(
			Rule == TerminationRule::Container || Rule == TerminationRule::Eager || Rule == TerminationRule::None ||
			Rule == TerminationRule::Transfer || Rule == TerminationRule::KeywordJump || Rule == TerminationRule::ShortCircuit
		) {
			const NodeId First = Def ? Syntax.GetFieldChild(Node, Def->Evaluation.FirstInput) : NoNode;
			const std::span<const NodeId> Children = Syntax.GetChildrenOf(Node);
			const bool IsRequiredFirst = Def && Def->Evaluation.IsRequiredFirst;
			// 通常は記述順で読み，既定値とアノテーションだけは定義された段階順で読む
			constexpr EvaluationClass Stages[] {
				EvaluationClass::Required,
				EvaluationClass::Conditional,
				EvaluationClass::Invocation,
				EvaluationClass::Unknown
			};
			std::vector<std::pair<NodeId, EvaluationClass>> Later;
			for(size_t Position = 0; Position <= Children.size(); ++Position) {
				Syntax.Charge(1);
				const NodeId Child = Position ? Children[Position - 1] : First;
				if(Child == NoNode || Position && Child == First) continue;
				const bool IsAccessToken = Syntax.Is(Child, Role::OptionalAccessToken) || Syntax.Is(Child, Role::SafeAccessToken);
				if(!Syntax.IsNamed(Child) && !IsAccessToken) continue;
				const EvaluationClass Class = GetChildEvaluation(Node, Child);
				if(Class == EvaluationClass::Deferred || Class == EvaluationClass::Unevaluated) continue;
				if(!IsRequiredFirst || Class == EvaluationClass::Required) Add(Child);
				else Later.emplace_back(Child, Class);
			}
			// 後の段階へ渡す子だけを控え，評価契約とフィールドを再照会しない
			for(size_t Stage = 1; Stage < std::size(Stages) && !Later.empty(); ++Stage) for(const auto &[Child, Class] : Later) {
				Syntax.Charge(1);
				if(Class == Stages[Stage]) Add(Child);
			}
			// 遅延・非評価の位置へ降りず収集を終える
			return;
		}
		const bool HasCond = Rule == TerminationRule::Branch || Rule == TerminationRule::Choice || Rule == TerminationRule::Switch ||
		Rule == TerminationRule::Loop || Rule == TerminationRule::InfiniteLoop;
		const NodeId Condition = HasCond ? GetEvaluationCondition(Node) : NoNode;
		// 規則が読み得る子の収集
		switch(Rule) {
		case TerminationRule::Macro:
			break;
		case TerminationRule::Choice:
			Add(Condition);
			Add(Syntax.GetNamedChild(Node, Condition == Syntax.GetNamedChild(Node, 0) ? 1 : 0));
			Add(Syntax.GetNamedChild(Node, 2));
			break;
		case TerminationRule::Container:
		case TerminationRule::Eager:
		case TerminationRule::ShortCircuit:
		case TerminationRule::None:
		case TerminationRule::Transfer:
		case TerminationRule::KeywordJump:
			break;
		case TerminationRule::Expr:
			Add(Syntax.GetNamedChild(Node, 0));
			break;
		case TerminationRule::Labeled:
			if(const uint32_t Count = Syntax.GetNamedChildCount(Node)) Add(Syntax.GetNamedChild(Node, Count - 1));
			break;
		case TerminationRule::InfiniteLoop:
		case TerminationRule::Loop:
			// 初期化・反復対象は入口で評価し，更新と代替本体は必須評価に含めない
			for(const NodeId Child : Syntax.GetChildrenOf(Node)) {
				if(
					Syntax.IsNamed(Child) && Child != LoopBody && Child != Condition && !Syntax.HasField(Child, ChildRole::Alternative) &&
					!Syntax.HasField(Child, ChildRole::Update) && !Syntax.HasField(Child, ChildRole::Increment)
				) Add(Child);
			}
			Add(Condition);
			Add(LoopBody);
			if(const NodeId Else = Syntax.GetFieldChild(Node, ChildRole::Alternative); Syntax.Is(Node, Role::LoopElse) && Else != NoNode) {
				Add(Syntax.GetLastNamedChild(Else));
			}
			break;
		case TerminationRule::Stmts:
			Core::SyntaxModel::ForEachStmtOf(Syntax, Node, Add);
			break;
		case TerminationRule::SingleBody:
			{
				const NodeId Body = GetSingleBodyOf(Node);
				for(const NodeId Child : Syntax.GetChildrenOf(Node)) if(Syntax.IsNamed(Child) && Child != Body) Add(Child);
				Add(Body);
				break;
			}
		case TerminationRule::Preproc:
			for(const NodeId Child : GetPreprocStmts(Node)) Add(Child);
			Add(Syntax.GetFieldChild(Node, ChildRole::Alternative));
			break;
		case TerminationRule::Branch:
			{
				const BranchParts &Parts = GetBranchesOf(Node);
				Add(Syntax.GetFieldChild(Node, ChildRole::Initializer));
				Add(Condition);
				Add(Parts.Then);
				for(const NodeId Branch : Parts.Alternatives) Add(Branch);
				break;
			}
		case TerminationRule::Try:
			{
				const TryParts &Parts = GetTryPartsOf(Node);
				for(const NodeId Entry : Parts.Entries) Add(Entry);
				for(const NodeId Part : { Parts.Finally, Parts.Body, Parts.Else }) Add(Part);
				for(const NodeId Handler : Parts.Handlers) {
					const NodeId Body = GetHandlerBody(Handler);
					if(Handler != Node) for(const NodeId Child : Syntax.GetChildrenOf(Handler)) {
						if(Child != Body && GetChildEvaluation(Handler, Child) == EvaluationClass::Required) Add(Child);
					}
					Add(Body);
				}
				break;
			}
		case TerminationRule::Switch:
			{
				Add(Syntax.GetFieldChild(Node, ChildRole::Initializer));
				Add(Condition);
				for(const NodeId Child : Syntax.GetChildrenOf(Node)) {
					if(
						Syntax.IsNamed(Child) && Child != Condition && !Syntax.HasField(Child, ChildRole::Initializer) &&
						GetChildEvaluation(Node, Child) == EvaluationClass::Required
					) Add(Child);
				}
				for(const NodeId Case : GetSwitchCases(Node).Cases) {
					for(const NodeId Child : GetCaseBodyOf(Case).Stmts) if(!IsFallthroughStmt(Child)) Add(Child);
				}
				break;
			}
		}
		// 終了
		return;
	}

	/**
	 * 関数から戻る文・式かの判定関数
	 * 計算量：参照する子・トークンの総数 C とテキストの総長 L に対し O(C + L)
	 * @param Syntax 構文モデル
	 * @param Node ノード
	 * @return 返戻の文・式か，先頭の語が返戻の脱出の文 (Kotlin / Swift) なら true
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool ControlFlow::IsReturn(const SyntaxModel &Syntax, const NodeId Node) {
		// 返戻の構文か，脱出の文の先頭の語で区別する返戻かの返戻
		return Syntax.Is(Node, Role::FlowReturn) ||
		Syntax.Is(Node, Role::KeywordJump) && Syntax.IsWord(Syntax.GetChild(Node, 0), Word::Return);
	}

	/**
	 * ========== 共通判定 ==========
	 * 同じ関数に属するジェネレータのマーカーの有無の判定関数
	 * 計算量：初回は参照するノード数 N とテキスト長 L に対し O(N + L)，同じ本体の再照会は O(1)
	 * @param Node 検索するノード
	 * @return ネスト関数の本体を除いてジェネレータのマーカーの名前付ノードが有れば `true`
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	bool ControlFlow::HasGeneratorIn(const NodeId Node) const {
		// 現在の役割インデックスで指定の役割の不在を確定した場合の返戻
		if(Node == NoNode || CanExcludeRole(Role::GeneratorMarker)) return false;
		if(GeneratorBodies.size() < Syntax.GetNodeCount()) GeneratorBodies.resize(Syntax.GetNodeCount());
		Syntax.Charge(1);
		#if !defined(ShaveFormatRefFacts)
		// 同じ本体の確定済のマーカーの有無の返戻
		if(GeneratorBodies[Node]) return *GeneratorBodies[Node];
		#endif
		// 同じ関数に属するマーカーのノードで止めた走査
		const bool HasGeneratorMarker = Core::SyntaxModel::HasStoppedTraversal(
			Syntax,
			Node,
			[&](const NodeId Cur) -> WalkStep {
				// 境界の中へ降りない事の返戻
				if(Cur != Node && IsFlowBoundary(Cur)) return WalkStep::Skip;
				// マーカーで止める事の返戻
				return Syntax.IsNamed(Cur) && Syntax.Is(Cur, Role::GeneratorMarker) ? WalkStep::Stop : WalkStep::Descend;
			}
		);
		GeneratorBodies[Node] = HasGeneratorMarker;
		// 同じ本体のジェネレータの有無の返戻
		return HasGeneratorMarker;
	}

	/**
	 * ブロックの値を与える末尾式の取得関数
	 * 計算量：参照する部分木のノード総数 N とテキストの総長 L に対し O(N + L)
	 * @param Last ブロックの最後の文
	 * @return 末尾式（`;` で終わらないブロックの式の文は中の式，文で終わるブロックは空）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	NodeId ControlFlow::GetTailExpr(const NodeId Last) const {
		// ブロックの値を与える末尾式の取得
		if(Syntax.Is(Last, Role::ExprStmt)) {
			// 式の文は `;` で終わらないブロックの式だけが本体の値を与える事の返戻
			return Syntax.IsWord(Syntax.GetChild(Last, Syntax.GetChildCount(Last) - 1), Word::StmtEnd) ?
			NoNode :
			Syntax.GetNamedChild(Last, 0);
		}
		if(Syntax.Is(Last, Role::MacroCall)) {
			// 波括弧で呼ぶマクロは文である事の返戻
			return Syntax.IsTokenRole(GetFirstChildNode(Syntax.GetLastNamedChild(Last)), Role::OpeningBraceToken) ? NoNode : Last;
		}
		// 宣言の文以外の末尾の式の返戻
		return Last == NoNode || Syntax.Is(Last, Role::NonValDecl) ? NoNode : Last;
	}

	/**
	 * 現在の束縛に対応するマクロの役割の取得関数
	 * 投影時と同じ供給元なら確認済の役割を使い，遮蔽の除去後は入力側が確認した未束縛時の役割を使う
	 * 計算量：束縛の照会費用 Q に対し O(Q)
	 * @param Macro マクロの名前
	 * @param HasSameBinding 投影時と同じ供給元かの格納先（無ければ格納しない）
	 * @return 現在の束縛に対応する役割（供給元が変われば未確認）
	 * @throws FormatLimitExceeded 束縛の照会が作業予算を超える場合
	 * @throws std::bad_alloc 束縛の照会のメモリを確保出来ない場合
	 */
	WordSet ControlFlow::GetMacroMeanings(const NodeId Macro, bool *HasSameBinding) const {
		if(HasSameBinding) *HasSameBinding = false;
		// 名前の無い対象の未確認の役割の返戻
		if(Macro == NoNode) return {};
		const std::shared_ptr<const ReferenceFacts> &Reference = Syntax.GetNode(Macro).Facts.Reference;
		// 役割の記録を持たない対象の未確認の返戻
		if(!Reference) return {};
		// 束縛に依存しない投影済の役割の返戻
		if(Reference->BindingHead == NoNode) {
			if(HasSameBinding) *HasSameBinding = true;
			// 束縛に依存しない役割の返戻
			return Reference->Meanings;
		}
		const BindingFact Fact = Syntax.GetBindingFact(Reference->BindingHead, Reference->BindingMember, Reference->IsGlobal);
		const NodeId Decl = Fact.Source ? Fact.Source->Name : Fact.Decl;
		if(HasSameBinding) *HasSameBinding = Decl == Reference->BindingDecl;
		// 独自定義に隠された標準の役割の未確認の返戻
		if(Fact.HasCustomDefinition) return {};
		// 投影時と同じ供給元の役割の返戻
		if(Decl == Reference->BindingDecl) return Reference->Meanings;
		// 宣言も供給値も無い名前は，投影で確認した未束縛時の役割へ戻す
		if(Decl == NoNode && !Fact.Source) return Reference->UnboundMeanings;
		// 新しい供給元を綴りから推測しない事の返戻
		return {};
	}

	/**
	 * 未展開マクロの実引数の評価順と制御移動の要約関数
	 * トークン列の囲みを子から合成し，短絡・分岐・遅延本体を式として確定出来ない列は終端を主張しない
	 * 文字列の中身はコードと読まず，未確定の構文は返戻補完の判定にも渡す
	 * 計算量：初回はトークン数 N，束縛の解決費用 R，可視範囲の登録数 M に対し O(N + R + M)，再照会は O(1)
	 * @param Node 実引数のトークン列
	 * @return 確定出来た終端と可能な効果（未確定は `UnknownExitEffect` を含む）
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 集計用の列又は表を確保出来ない場合
	 * @throws std::length_error 集計用の列又は表の要素数上限を超える場合
	 * @throws std::out_of_range ノード又は子の要約が表に無い場合
	 */
	ControlFlow::TerminationSummary ControlFlow::GetMacroArguments(const NodeId Node) const {
		// エラー回復で実引数の列が無い場合は元の未確定の効果を保つ
		if(Node == NoNode) {
			TerminationSummary Missing;
			Missing.Effects = UnknownExitEffect;
			// 未確定の実引数の返戻
			return Missing;
		}
		if(MacroArguments.size() < Syntax.GetNodeCount()) MacroArguments.resize(Syntax.GetNodeCount());
		Syntax.Charge(1);
		#if !defined(ShaveFormatRefFacts)
		// 書換の無いトークン列の確定済の要約の返戻
		if(MacroArguments[Node]) return *MacroArguments[Node];
		#endif
		std::vector<std::pair<NodeId, bool>> Pending { { Node, false } };
		while(!Pending.empty()) {
			const std::pair<NodeId, bool> Cur = Pending.back();
			const NodeId Part = Cur.first;
			const bool IsClosing = Cur.second;
			Pending.pop_back();
			Syntax.Charge(1);
			if(!IsClosing) {
				#if !defined(ShaveFormatRefFacts)
				if(MacroArguments[Part]) continue;
				#endif
				Pending.emplace_back(Part, true);
				for(const NodeId Child : Syntax.GetChildrenOf(Part)) if(Syntax.Is(Child, Role::PreservedTokenSeq)) {
					Pending.emplace_back(Child, false);
				}
				continue;
			}
			TerminationSummary Result;
			bool IsKnown = Syntax.Is(Part, Role::PreservedTokenSeq), HasPendingReturn = false, DoesUseBindings = false;
			uint8_t MacroEffect = 0;
			NodeId Attribute = NoNode;
			for(const NodeId Child : Syntax.GetChildrenOf(Part)) {
				Syntax.Charge(1);
				if(Child == Attribute) continue;
				if(
					!Result.IsAbrupt && (
						Syntax.Is(Child, Role::Identifier) || Syntax.Is(Child, Role::TrailingOperatorToken) ||
						Syntax.Is(Child, Role::OpeningBracketToken)
					)
				) Result.Effects |= ThrowEffect;
				if(HasPendingReturn && Syntax.Is(Child, Role::ExprTerminator)) {
					if(!Result.IsAbrupt) {
						Result.Modes = AllTermination;
						Result.Effects |= ReturnEffect;
						Result.IsAbrupt = true;
					}
					HasPendingReturn = false;
				}
				if(Syntax.Is(Child, Role::PreservedTokenSeq)) {
					const TerminationSummary &Inner = *MacroArguments.at(Child);
					DoesUseBindings |= BoundMacroArguments.contains(Child);
					IsKnown &= !(Inner.Effects & UnknownExitEffect);
					if(!Result.IsAbrupt) {
						Result.Effects |= Inner.Effects;
						Result.Modes |= Inner.Modes;
						Result.IsAbrupt = Inner.IsAbrupt;
						if(MacroEffect && !Result.IsAbrupt) {
							Result.Modes = AllTermination;
							Result.Effects |= MacroEffect;
							Result.IsAbrupt = true;
						}
						MacroEffect = 0;
					}
				} else if(Syntax.Is(Child, Role::HashToken)) {
					Attribute = Syntax.GetNextSibling(Child);
					if(Syntax.Is(Attribute, Role::ExclamationToken)) Attribute = Syntax.GetNextSibling(Attribute);
					IsKnown &= IsKnownAttribute(Syntax, Attribute);
				} else if(Syntax.Is(Child, Role::ExclamationToken)) {
					NodeId Head = Syntax.GetPrevSibling(Child);
					if(!Syntax.Is(Head, Role::Identifier) || !Syntax.Is(Syntax.GetNextSibling(Child), Role::PreservedTokenSeq)) continue;
					DoesUseBindings = true;
					const WordSet Meanings = GetMacroMeanings(Head);
					if(Meanings.test(static_cast<size_t>(Word::TerminatingMacro))) MacroEffect = ThrowEffect;
					else IsKnown &= Meanings.test(static_cast<size_t>(Word::OrdinaryMacro));
				} else if(Syntax.IsWord(Child, Word::Return)) HasPendingReturn = !Result.IsAbrupt;
				else if(Syntax.Is(Child, Role::QuestionToken)) {
					if(!Result.IsAbrupt) Result.Effects |= ReturnEffect;
				} else if(
					Syntax.Is(Child, Role::Identifier) || Syntax.Is(Child, Role::ExprStmtLiteral) || Syntax.Is(Child, Role::StrLikeAll) ||
					Syntax.Is(Child, Role::BoolLiteral) || Syntax.Is(Child, Role::Comment)
				) continue;
				// 評価を選択・遅延しない区切と演算子だけを認め，語の制御構文を逐次文と決め付けない
				else IsKnown &= Syntax.IsWord(Child, Word::EagerMacroToken);
			}
			if(HasPendingReturn && !Result.IsAbrupt) {
				Result.Modes = AllTermination;
				Result.Effects |= ReturnEffect;
				Result.IsAbrupt = true;
			}
			if(!IsKnown) {
				Result.Modes = 0;
				Result.IsAbrupt = false;
				Result.Effects |= UnknownExitEffect;
			}
			MacroArguments[Part] = Result;
			if(DoesUseBindings && BoundMacroArguments.insert(Part).second) {
				// 束縛の依存範囲は評価履歴でなく，文法が定める可視スコープの列で決める
				for(NodeId Scope = GetScopeOf(Part); Scope != NoNode; Scope = GetScopeOf(Syntax.GetParentOf(Scope))) {
					Syntax.Charge(1);
					MacroBindingScopes[Scope].insert(Part);
				}
			}
		}
		// 同じトークン列から求めた要約の返戻
		return *MacroArguments.at(Node);
	}

	/**
	 * 判定済の子の終端と送出の効果を合成する関数
	 * 計算量：対象の子の数 C と規則・捕捉条件の照会費用 Q に対し O(C + Q)
	 * @param Gotos 同じ関数内のジャンプ先
	 * @param Known 判定済の子の要約
	 * @param Node 判定するノード
	 * @param Scope 制御移動の判定範囲
	 * @param Operands 展開時に収集した子の評価対象
	 * @param OperandScope 子の評価対象に共通の判定範囲
	 * @param Rule 確定した判定規則
	 * @param LoopBody 繰返の本体
	 * @param InvocationBody 子の展開時に求めた呼出時機の本体
	 * @param LoopModes 条件に依る終端の仮定
	 * @param Effects 呼出先の解決時に確定した効果
	 * @return ノードの終端と外側へ伝播する効果
	 * @throws FormatLimitExceeded 探索又は規則の照会が作業予算を超える場合
	 * @throws std::bad_alloc 子の列又は捕捉条件を確保出来ない場合
	 * @throws std::length_error 子の列又はテキストの要素数上限を超える場合
	 * @throws std::out_of_range ノード又は名前が表に無い場合
	 */
	ControlFlow::TerminationSummary ControlFlow::SummarizeTermination(
		const GotoIdx &Gotos,
		const TerminationResults &Known,
		const NodeId Node,
		const NodeId Scope,
		const std::span<const NodeId> Operands,
		const NodeId OperandScope,
		const TerminationRule Rule,
		const NodeId LoopBody,
		const NodeId InvocationBody,
		const uint8_t LoopModes,
		uint8_t Effects
	) const {
		const KindDef *const Def = Syntax.GetDefOf(Node);
		// 解決が未確定の状態を，送出等の効果から分離する
		const bool IsUnknownCall = Effects & UnknownCallEffect;
		Effects &= ~UnknownCallEffect;
		TerminationSummary Invocation;
		if(InvocationBody != NoNode) {
			const TerminationSummary Deferred = GetKnownSummary(Known, InvocationBody, OperandScope);
			// 通常完了する本体と独立した関数のローカル返戻は，呼出の回数の事実を要しない
			Invocation.HasUnknownEvaluation = Deferred.HasUnknownEvaluation ||
			static_cast<bool>(Deferred.Effects & (ProcessExitEffect | SystemExitEffect | UnknownExitEffect)) ||
			(Deferred.Modes || Deferred.IsAbrupt) && !(Deferred.Effects & ReturnEffect) ||
			!IsFlowBoundary(Node) && static_cast<bool>(Deferred.Effects & ReturnEffect);
		}
		// 非評価の構文は，内側の送出も此の評価へ伝播させない
		if(
			Def && (Def->Evaluation.Children == EvaluationClass::Unevaluated || Def->Evaluation.Children == EvaluationClass::Deferred) &&
			Def->Evaluation.Inputs.empty()
		) return Invocation;
		// 文法が呼出の形で表す非評価演算は，自身にも送出の効果を持たない
		if(
			Def && !Def->Evaluation.UnevaluatedHead.empty() &&
			Syntax.GetLexemeOf(Syntax.GetNamedChild(Node, 0)) == Def->Evaluation.UnevaluatedHead
		) return {};
		// 捕捉と後始末の専用の合成
		if(Rule == TerminationRule::Try) return TryTermination(Known, Node, Scope);
		if(Rule == TerminationRule::Macro) {
			TerminationSummary Args = GetMacroArguments(Syntax.GetLastNamedChild(Node));
			if(!Args.IsAbrupt) {
				Args.Modes |= LoopModes;
				Args.Effects |= Effects ? Effects : ThrowEffect;
				Args.IsAbrupt = static_cast<bool>(Args.Modes & CertainTermination);
			}
			// 実引数と登録済の効果を合成した要約の返戻
			return Args;
		}
		TerminationSummary Result;
		const bool IsConditionalToken = Syntax.Is(Node, Role::OptionalAccessToken) || Syntax.Is(Node, Role::SafeAccessToken);
		Result.HasUnknownEvaluation = Invocation.HasUnknownEvaluation || Def && (
			Def->Evaluation.Children == EvaluationClass::Unknown ||
			!Def->Evaluation.UnknownHead.empty() && Syntax.GetLexemeOf(Syntax.GetNamedChild(Node, 0)) == Def->Evaluation.UnknownHead
		);
		NodeId UncertainArgs = NoNode;
		if(Def && Def->Evaluation.HasDeferredArguments && !Effects) {
			UncertainArgs = Syntax.GetFieldChild(Node, ChildRole::Args);
			if(UncertainArgs == NoNode) UncertainArgs = Syntax.GetFirstNamedChildWithRole(Node, Role::CallSuffix);
		}
		bool HasMacroArguments = false;
		if(!Effects && Syntax.Is(Node, Role::CallNode)) {
			NodeId Head = Syntax.GetFieldChild(Node, ChildRole::Function);
			if(Head == NoNode) Head = Syntax.GetNamedChild(Node, 0);
			HasMacroArguments = Head != NoNode && (
				Syntax.GetNode(Head).Facts.Has(ConstraintKind::MacroBinding) ||
				Syntax.GetNode(Head).Facts.Has(ConstraintKind::UncertainMacroArguments)
			);
		}
		Result.HasConditionalAccess =
		IsConditionalToken || Syntax.Is(Node, Role::OptionalChain) || Syntax.Is(Node, Role::ConditionalAccessExpr);
		const bool IsAccess =
		Syntax.Is(Node, Role::CallNode) || Syntax.Is(Node, Role::MemberExpr) || Syntax.Is(Node, Role::FieldExpr) ||
		Syntax.Is(Node, Role::Subscript) || Syntax.Is(Node, Role::SubscriptExprNode) ||
		Syntax.Is(Node, Role::CallAttributeOrSubscript) || Syntax.Is(Node, Role::ConditionalAccessExpr);
		Result.Modes = NodeTermination(Gotos, Known, Node, Scope, Operands, Rule, LoopBody, LoopModes, Result.IsAbrupt);
		if(IsUnknownCall) Result.Modes |= PessimisticTermination;
		if(Def && Def->Evaluation.HasEntry && !HasGotoEntry(Gotos, Node)) {
			const TerminationSummary Entry = SummarizeEntry(Known, Node, OperandScope, Operands, LoopBody);
			// 本体の前に終端する共有の入口の返戻
			if(Entry.IsAbrupt) return Entry;
			Result.Modes |= Entry.Modes;
			Result.HasUnknownEvaluation |= Entry.HasUnknownEvaluation;
		}
		const bool IsSequential =
		Rule == TerminationRule::Stmts || Rule == TerminationRule::Expr || Rule == TerminationRule::SingleBody ||
		Rule == TerminationRule::Transfer || Rule == TerminationRule::KeywordJump;
		// 契約を解決済の呼出先は再評価せず，実引数に残る演算・参照の送出を合成する
		const bool IsKnownCall = Effects && (Syntax.Is(Node, Role::CallNode) || Syntax.Is(Node, Role::MacroCall));
		NodeId CallHead = NoNode;
		if(IsKnownCall) {
			CallHead = Syntax.GetFieldChild(Node, Syntax.Is(Node, Role::MacroCall) ? ChildRole::Macro : ChildRole::Function);
			if(CallHead == NoNode) CallHead = Syntax.GetNamedChild(Node, 0);
		}
		const bool IsOrdered = IsAccess || Def && Def->Evaluation.IsOrdered;
		const ChildRole ThrowBefore = Def ? Def->Evaluation.ThrowBeforeField : ChildRole::None;
		bool IsClosed = false, HasEagerExit = false;
		for(const NodeId Operand : Operands) {
			Syntax.Charge(1);
			if(
				IsKnownCall && (
					Operand == CallHead || Syntax.Is(Node, Role::MethodInvocation) &&
					(Syntax.HasField(Operand, ChildRole::Object) || Syntax.HasField(Operand, ChildRole::Name))
				)
			) continue;
			if(HasGotoEntry(Gotos, Operand)) IsClosed = false;
			// 記述順の必須評価は終端した子より後へ進まない
			if(IsSequential && IsClosed || HasEagerExit && IsOrdered) continue;
			if(ThrowBefore != ChildRole::None && Syntax.HasField(Operand, ThrowBefore)) Result.Effects |= ThrowEffect;
			const TerminationSummary Child = GetKnownSummary(Known, Operand, OperandScope);
			Result.Effects |= Child.Effects;
			const bool IsUncertainInput = HasMacroArguments || Operand == UncertainArgs || Def && Def->Evaluation.MaySkipInputs;
			// 同じ子の評価位置を，要る最初の判定で１度だけ照会する
			std::optional<EvaluationClass> Evaluation;
			const auto GetEvaluation = [&]() -> EvaluationClass {
				if(!Evaluation) Evaluation = GetChildEvaluation(Node, Operand);
				// 此の子の取得済の評価位置の返戻
				return *Evaluation;
			};
			Result.HasUnknownEvaluation |= Child.HasUnknownEvaluation || GetEvaluation() == EvaluationClass::Unknown ||
			IsUncertainInput && (Child.Modes || Child.IsAbrupt);
			if(
				(Rule == TerminationRule::Container || Rule == TerminationRule::Eager) && GetEvaluation() == EvaluationClass::Required &&
				!IsUncertainInput && (!Result.HasConditionalAccess || IsAccess && Operand == Operands.front())
			) {
				Result.Modes |= Child.Modes;
				HasEagerExit = HasEagerExit || Child.IsAbrupt;
			}
			if(
				IsAccess && (
					Operand == Operands.front() || Syntax.Is(Operand, Role::OptionalChain) || Syntax.Is(Operand, Role::OptionalAccessToken) ||
					Syntax.Is(Operand, Role::SafeAccessToken)
				)
			) Result.HasConditionalAccess = Result.HasConditionalAccess || Child.HasConditionalAccess;
			IsClosed = Child.IsAbrupt;
		}
		if(!HasEagerExit && (!IsSequential || !IsClosed)) {
			if(Effects) Result.Effects |= Effects;
			else if(Rule == TerminationRule::Transfer || Rule == TerminationRule::KeywordJump) {
				if(IsReturn(Syntax, Node)) Result.Effects |= ReturnEffect;
				else if(
					Syntax.Is(Node, Role::GotoStmt) ||
					Rule == TerminationRule::KeywordJump && !Syntax.IsWord(GetFirstChildNode(Node), Word::ThrowHead) &&
					!Syntax.Is(GetFirstChildNode(Node), Role::ThrowKeyword)
				) Result.Effects |= TransferEffect;
				else Result.Effects |= ThrowEffect;
			} else if(Syntax.Is(Node, Role::CallNode) || Syntax.Is(Node, Role::MacroCall)) Result.Effects |= ThrowEffect;
		}
		// 演算・名前解決・宣言等は，送出しない事実が無ければ捕捉へ進み得る
		// 数値と実引数・括弧のコンテナ自身には効果を足さず，内側の式の効果を保つ
		if(
			(
				Rule == TerminationRule::None || Rule == TerminationRule::Eager || Rule == TerminationRule::Choice ||
				Rule == TerminationRule::ShortCircuit
			) && !HasEagerExit && Syntax.IsNamed(Node) && !Syntax.Is(Node, Role::Comment) && !Syntax.Is(Node, Role::NumLiteral) &&
			!Syntax.Is(Node, Role::EmptyStmt) || Rule == TerminationRule::Branch || Rule == TerminationRule::Loop ||
			Rule == TerminationRule::InfiniteLoop || Rule == TerminationRule::Switch || Rule == TerminationRule::SingleBody
		) Result.Effects |= ThrowEffect;
		// 自身のラベルへ抜けるブロックは，内部のジャンプを通常完了として受ける
		if(
			Rule == TerminationRule::Stmts && IsClosed && Syntax.Is(Node, Role::LabeledBlockExit) &&
			Syntax.GetFirstNamedChildWithRole(Node, Role::JumpLabel) != NoNode && HasLoopExit(Gotos, Known, Node, Node, Scope)
		) IsClosed = false;
		// 関数の終端と，直後へ進まないジャンプを区別して後始末へ渡す
		Result.IsAbrupt = Result.IsAbrupt || HasEagerExit || static_cast<bool>(Result.Modes & CertainTermination) ||
		Syntax.Is(Node, Role::LoopExitStmt) || Rule == TerminationRule::KeywordJump &&
		(Syntax.Is(GetFirstChildNode(Node), Role::BreakKeyword) || Syntax.Is(GetFirstChildNode(Node), Role::ContinueKeyword)) ||
		IsSequential && Rule != TerminationRule::SingleBody && IsClosed;
		// 終端と効果の返戻
		return Result;
	}

	/**
	 * 抽出済の制御構造の保持の終了関数
	 * 計算量：保持して居る構造の件数 F に対し O(F)
	 * @throws FormatLimitExceeded 解放する構造の件数が作業予算を超える場合
	 */
	void ControlFlow::ClearTerminationStructures() const {
		// 要約の表の解放
		Syntax.Charge(BranchStructures.size() + TryStructures.size() + SwitchStructures.size() + CaseStructures.size());
		BranchStructures.clear();
		TryStructures.clear();
		SwitchStructures.clear();
		CaseStructures.clear();
		// 終了
		return;
	}

	/**
	 * 終端の判定結果の構築関数（整形仕様 9.2，深い構文は明示した作業列で辿る）
	 * 規則が読み得る子を作業列で辿り，子から親の順に規則を適用して判定範囲毎に控える
	 * 通常・被覆・悲観の位は同じ子の結果から求め，同じ繰返の脱出インデックスを共有する
	 * 変更した部分木だけ再構築する
	 * 計算量：規則が読み得るノードの数 V と未収集の脱出インデックスの費用 Q に対し期待値 O(V + Q)
	 * @param Gotos 同じ関数内の goto ジャンプ先
	 * @param Roots 判定するノード
	 * @param Scope 内部転送を認める switch 文の判定範囲
	 * @param Function 可視名を求める関数（定数式の仮定を使わない場合は無）
	 * @param ShouldStopAtTermination 入口の無い確実な終端で，後続の根の判定を止めるか
	 * @return 判定範囲毎のノードが終端する仮定の位
	 * @throws FormatLimitExceeded 抽出又は照会が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	const ControlFlow::TerminationResults &ControlFlow::EvaluateTermination(
		const GotoIdx &Gotos,
		const std::span<const NodeId> Roots,
		const NodeId Scope,
		const NodeId Function,
		const bool ShouldStopAtTermination
	) const {
		const bool HasJumps = !Gotos.Targets.empty() || !Gotos.Entries.empty() || Gotos.HasUnresolvedTarget;
		#if defined(ShaveFormatRefFacts)
		Terminations.Body = NoNode;
		#endif
		if(
			Gotos.Body == NoNode || Terminations.Body != Gotos.Body || HasJumps ||
			Terminations.DoesUseFunction && Terminations.Function != Function
		) {
			ClearTerminationStructures();
			Terminations.Known.Clear(Syntax);
			Terminations.DoesUseFunction = false;
		}
		Terminations.Body = HasJumps ? NoNode : Gotos.Body;
		Terminations.Function = Function;
		TerminationResults &Known = Terminations.Known;
		if(Known.Positions.size() < Syntax.GetNodeCount()) Known.Positions.resize(Syntax.GetNodeCount(), NoNode);
		// 共有済の先頭の結果は，展開用の作業列へ積まずに読む
		size_t FirstUnknown = 0;
		if(!Known.Entries.empty()) while(FirstUnknown < Roots.size()) {
			const NodeId Root = Roots[FirstUnknown];
			if(Root != NoNode) {
				Syntax.Charge(1);
				if(!Known.Find(Root, Scope)) break;
				// 判定済の終端より後の根を使わない照会の返戻
				if(ShouldStopAtTermination && GetKnownTermination(Known, Root, Scope) & CertainTermination) return Known;
			}
			++FirstUnknown;
		}
		// 作業列の確保を要しない共有済の判定結果の返戻
		if(FirstUnknown == Roots.size()) return Known;
		TerminationCtx Ctx { Function };
		std::vector<TerminationFrame> &Work = Terminations.Work;
		Work.clear();
		std::vector<NodeId> &Operands = Terminations.Operands;
		Operands.clear();
		// 子を待たないノードも，同じ規則の合成と作業列の解放を使う
		const auto Store = [&](const TerminationFrame &Frame) -> void {
			Known.Store(
				Frame.Node,
				Frame.Scope,
				SummarizeTermination(
					Gotos,
					Known,
					Frame.Node,
					Frame.Scope,
					std::span<const NodeId>(Operands).subspan(Frame.OperandBegin, Frame.OperandEnd - Frame.OperandBegin),
					Frame.OperandScope,
					Frame.Rule,
					Frame.LoopBody,
					Frame.InvocationBody,
					Frame.LoopModes,
					Frame.Effects
				)
			);
			Operands.resize(Frame.OperandBegin);
			// 終了
			return;
		};
		size_t NextRoot = FirstUnknown;
		if(!ShouldStopAtTermination) for(const NodeId Root : Roots.subspan(FirstUnknown)) Work.push_back({ Root, Scope, false });
		// 子を先に評価し，後続の根を使わない照会では必要な根だけを順に積む
		while(!Work.empty() || ShouldStopAtTermination && NextRoot < Roots.size()) {
			if(Work.empty()) {
				if(
					NextRoot > FirstUnknown && Roots[NextRoot - 1] != NoNode &&
					GetKnownTermination(Known, Roots[NextRoot - 1], Scope) & CertainTermination
				) break;
				Work.push_back({ Roots[NextRoot++], Scope, false });
			}
			const TerminationFrame Cur = Work.back();
			Work.pop_back();
			if(Cur.Node == NoNode) continue;
			Syntax.Charge(1);
			if(Known.Find(Cur.Node, Cur.Scope)) continue;
			if(Cur.IsExpanded) {
				Store(Cur);
				continue;
			}
			Syntax.Charge(1);
			NodeId LoopBody = NoNode;
			uint8_t LoopModes = 0, Effects = 0;
			const TerminationRule Rule = GetTerminationRuleOf(Ctx, Cur.Node, Cur.Scope, LoopBody, LoopModes, Effects);
			// 節の本文は外側の `switch` 文（単独判定時は此の `switch` 文）を判定範囲とする
			const NodeId OperandScope = Rule == TerminationRule::Switch && Cur.Scope == NoNode ? Cur.Node : Cur.Scope;
			const size_t Begin = Operands.size();
			TerminationOperands(Cur.Node, Rule, LoopBody, Operands);
			const size_t End = Operands.size();
			// 子の展開中は位置だけを保持し，再確保で失効する参照を作業枠へ残さない
			const NodeId InvocationBody = GetInvocationBody(Cur.Node);
			const TerminationFrame Frame {
				Cur.Node,
				Cur.Scope,
				true,
				Rule,
				LoopBody,
				LoopModes,
				Effects,
				OperandScope,
				Begin,
				End,
				InvocationBody
			};
			// 評価する子も呼出時機の本体も無ければ，継続の枠へ積まずに確定する
			if(Begin == End && InvocationBody == NoNode) {
				Store(Frame);
				continue;
			}
			Work.push_back(Frame);
			// 渡す本体の要約は作成時の効果と混ぜず，共有表へ１度だけ記録する
			if(InvocationBody != NoNode) Work.push_back({ InvocationBody, OperandScope, false });
			for(size_t Idx = Begin; Idx < End; ++Idx) Work.push_back({ Operands[Idx], OperandScope, false });
		}
		Terminations.DoesUseFunction = Terminations.DoesUseFunction || Ctx.DoesUseFunction;
		// 判定結果の返戻
		return Known;
	}

	/**
	 * 同じ制御フロー内の返戻文包含判定関数
	 * 計算量：参照する部分木のノード総数 N とテキストの総長 L に対し O(N + L)
	 * @param Node 検索するノード
	 * @return ネスト関数の本体を除いて返戻文が有れば true
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool ControlFlow::HasReturnInFlow(const NodeId Node) const {
		// 同じ関数に属する返戻文で止めた走査の結果の返戻
		return Core::SyntaxModel::HasStoppedTraversal(
			Syntax,
			Node,
			[&](const NodeId Cur) -> WalkStep {
				// 境界の中へ降りない事の返戻
				if(IsFlowBoundary(Cur)) return WalkStep::Skip;
				// 返戻文で止める事の返戻
				return IsReturn(Syntax, Cur) ? WalkStep::Stop : WalkStep::Descend;
			}
		);
	}

	/**
	 * ========== 通常関数の分類 ==========
	 * 通常関数のノードかの判定関数
	 * 名前を持つ関数・メソッドの定義，コンストラクタ，デストラクタ及びアクセサを通常関数とし，ラムダ・無名関数・初期化のブロックを含めない
	 * @param Type 判定するノード
	 * @return 通常関数のノードなら true
	 */
	bool ControlFlow::IsOrdinaryFunction(const NodeId Type) const {
		// 通常関数のノードの型かの返戻（Ruby は専用の規定に従う）
		return Syntax.Is(Type, Role::ExplicitResultFunction);
	}

	/**
	 * ジェネレータ・コルーチンかの判定関数
	 * 計算量：参照する部分木のノード総数 N とテキストの総長 L に対し O(N + L)
	 * @param Node 通常関数のノード
	 * @param Stmts 文を直下に並べるノード
	 * @return 本体がジェネレータ・コルーチンとして実行されるなら true
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	bool ControlFlow::IsGeneratorLike(const NodeId Node, const NodeId Stmts) const {
		// JS/TS の `*` を持つメソッドと，本体の yield ･ C++ の co_ 系の文の有無の返戻
		return Syntax.Is(Node, Role::StarGeneratorFunction) && Syntax.HasTokenRoleChild(Node, Role::AsteriskToken) ||
		HasGeneratorIn(Stmts);
	}

	/**
	 * 本体の返戻区分と呼出結果の判定関数（整形仕様 5.2, 7.4）
	 * 本体の終端用の区分を保持し，暗黙の返戻・非同期・ジェネレータの呼出結果を別に確定する
	 * 値付の返戻文が無い事だけでは，値を返さない呼出と確定しない
	 * 計算量：本体の判定とジェネレータの探索が読むノード数 N とテキスト長 L に対し O(N + L)
	 * @param Node 関数のノード
	 * @return 本体の返戻区分と，確認済の呼出結果の有無
	 * @throws FormatLimitExceeded 構文の探索又は事実の収集が作業予算を超える場合
	 * @throws std::bad_alloc 照会に用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	ReturnShape ControlFlow::GetReturnShapeOf(const NodeId Node) const {
		// 非同期の修飾と本体の返戻区分
		const bool IsAsync = Core::SyntaxModel::HasChildMatching(
			Syntax,
			Node,
			[this](const NodeId Child) -> bool {
				// 非同期の修飾のトークン又は修飾語かの返戻
				return (!Syntax.IsNamed(Child) || Syntax.Is(Child, Role::Modifier)) && Syntax.IsWord(Child, Word::AsyncModifier);
			}
		);
		ReturnShape Result = GetBodyReturnShapeOf(Node, IsAsync);
		if(Syntax.GetRules().DoesDeferredCallReturnVal && (IsAsync || IsGeneratorLike(Node, GetStmtsOf(Node)))) {
			Result.HasCallResult = true;
		} else if(!Result.HasCallResult.has_value()) {
			if(Result.Evidence == ReturnEvidence::Declared) {
				Result.HasCallResult = Result.Kind == ReturnKind::Val || Result.Kind == ReturnKind::Main;
			} else if(Result.Evidence == ReturnEvidence::Inferred && Result.Kind == ReturnKind::Val) Result.HasCallResult = true;
		}
		// 同じ構文から求めた区分の返戻
		return Result;
	}

	/**
	 * 値を変えない単一の式の文又は括弧かの判定関数
	 * カンマを持つ式の文はタプルを作る為，単一要素でも透過しない
	 * 計算量：O(1)
	 * @param Syntax 構文モデル
	 * @param Node 調べる式又は文
	 * @return 単一の値を透過するラッパーなら true
	 * @throws std::out_of_range ノードの番号が表に無い場合
	 */
	bool ControlFlow::IsTransparentExpr(const SyntaxModel &Syntax, const NodeId Node) {
		// 値を変えないラッパーかの返戻（子の数を先に絞り，名前付の子を数えるのは子が３つ以下の時だけ）
		return (
			Syntax.Is(Node, Role::ExprStmt) && Syntax.GetChildCount(Node) == 1 ||
			Syntax.Is(Node, Role::GroupingParen) && Syntax.GetChildCount(Node) == 3
		) && Syntax.GetNamedChildCount(Node) == 1;
	}

	/**
	 * 単一の式の文と透過的な括弧の中の値の取得関数
	 * 計算量：透過する層数 H に対し O(H + 1)
	 * @param Syntax 構文モデル
	 * @param Node 調べる式又は文
	 * @return ラッパーの中の値
	 * @throws FormatLimitExceeded 探索の作業予算を超える場合
	 * @throws std::out_of_range ノードの番号が表に無い場合
	 */
	NodeId ControlFlow::GetUnwrappedExpr(const SyntaxModel &Syntax, NodeId Node) {
		while(IsTransparentExpr(Syntax, Node)) {
			Syntax.Charge(1);
			Node = Syntax.GetNamedChild(Node, 0);
		}
		// 式の値の返戻
		return Node;
	}

	/**
	 * Python の先頭の説明文字列の式の共有判定関数
	 * 括弧と暗黙の連結を透過し，バイト列と補間する文字列を除く
	 * 計算量：祖先深度 H，初回に読む式のノード数 N と接頭辞の総長 L に対し O(H + N + L)，同じ式の再照会は期待値 O(H + 1)
	 * @param Syntax 構文モデル
	 * @param Node 説明文字列の候補又は其の文字列の部品
	 * @param Values 同じ構文で共有する説明文字列の先頭の部品
	 * @return 説明文字列の先頭の文字列ノード（説明文字列でなければ無）
	 * @throws FormatLimitExceeded 探索の作業予算を超える場合
	 * @throws std::bad_alloc 記録又は作業列を確保出来ない場合
	 * @throws std::length_error 記録又は作業列の要素数上限を超える場合
	 * @throws std::out_of_range ノードの番号が表に無い場合
	 */
	NodeId ControlFlow::GetDocstring(const SyntaxModel &Syntax, const NodeId Node, std::unordered_map<NodeId, NodeId> &Values) {
		NodeId Expr = Node;
		for(NodeId Parent = Syntax.GetParentOf(Expr); Parent != NoNode; Parent = Syntax.GetParentOf(Expr)) {
			Syntax.Charge(1);
			if(!Syntax.Is(Parent, Role::ConcatenatedLiteral) && !IsTransparentExpr(Syntax, Parent)) break;
			Expr = Parent;
		}
		const NodeId Parent = Syntax.GetParentOf(Expr);
		// 説明文字列を持つ本体の先頭でない場合の返戻
		if(
			!(
				Syntax.Is(Parent, Role::Module) ||
				Syntax.Is(Parent, Role::LexicalBlock) && Syntax.Is(Syntax.GetParentOf(Parent), Role::LeadingStrDocOwner)
			) || Syntax.GetNamedChild(Parent, 0) != Expr
		) return NoNode;
		if(const std::unordered_map<NodeId, NodeId>::const_iterator Found = Values.find(Expr); Found != Values.end()) {
			// 同じ式の判定済の値の種類の返戻
			return Found->second;
		}
		bool IsText = true;
		NodeId First = NoNode;
		std::vector<NodeId> Pending { Expr };
		while(!Pending.empty() && IsText) {
			const NodeId Part = GetUnwrappedExpr(Syntax, Pending.back());
			Pending.pop_back();
			Syntax.Charge(1);
			if(Syntax.Is(Part, Role::ConcatenatedLiteral)) {
				IsText = static_cast<bool>(Syntax.GetNamedChildCount(Part));
				const std::span<const NodeId> Children = Syntax.GetChildrenOf(Part);
				for(size_t Pos = Children.size(); Pos; --Pos) if(Syntax.IsNamed(Children[Pos - 1])) Pending.push_back(Children[Pos - 1]);
				continue;
			}
			IsText = Syntax.Is(Part, Role::DocStr);
			if(!IsText) break;
			if(First == NoNode) First = Part;
			const std::string_view Start = Syntax.GetLexemeOf(Syntax.GetChildCount(Part) ? Syntax.GetFirstEdge(Part).Leaf : Part);
			const size_t Quote = Start.find_first_of("\"'");
			IsText = Quote != std::string_view::npos;
			for(size_t Pos = 0; IsText && Pos < Quote; ++Pos) {
				Syntax.Charge(1);
				IsText = Syntax.GetRules().NonDocQuotePrefixes.find(static_cast<char>(static_cast<unsigned char>(Start[Pos]) | 0X20)) ==
				std::string_view::npos;
			}
		}
		const NodeId Result = IsText ? First : NoNode;
		Values.emplace(Expr, Result);
		// 同じ式の意味から判定した説明文字列かの返戻
		return Result;
	}

	/**
	 * 中身を持たない事を表す本体かの判定関数
	 * 計算量：参照する部分木のノード総数 N とテキストの総長 L に対し O(N + L)
	 * @param Stmts 文を直下に並べるノード
	 * @return 文が無いか，Python のドキュメンテーション文字列・`pass`・`...` だけなら `true`
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool ControlFlow::IsPlaceholderBody(const NodeId Stmts) const {
		// 先頭の判定と空の本体を許す範囲
		std::unordered_map<NodeId, NodeId> Values;
		const bool CanUsePlaceholders = Syntax.Is(Stmts, Role::PlaceholderBody);
		bool IsInBody = !IsOrdinaryFunction(Stmts);
		// 中身を表す文が無いかの返戻
		return !Core::SyntaxModel::HasChildMatching(
			Syntax,
			Stmts,
			[&](const NodeId Child) -> bool {
				// 波括弧を直接持つ関数・アクセサでは，見出を本体の文として数えない
				if(!IsInBody) {
					IsInBody = Syntax.IsTokenRole(Child, Role::OpeningBraceToken);
					// 見出と開始の波括弧を中身として数えない事の返戻
					return false;
				}
				// 無名のトークンは中身でない事の返戻
				if(!Syntax.IsNamed(Child)) return false;
				// Python 以外は何れの文も中身である事の返戻
				if(!CanUsePlaceholders) return true;
				const NodeId Inner = GetUnwrappedExpr(Syntax, Child);
				// 最初の文字列はドキュメンテーション文字列で，`pass`・`...` は中身の無い事を表す事の返戻
				return !(
					GetDocstring(Syntax, Child, Values) != NoNode || Syntax.Is(Inner, Role::PlaceholderStmt) ||
					Syntax.Is(Inner, Role::Ellipsis) && Syntax.IsWord(Inner, Word::VariadicMarker)
				);
			}
		);
	}

	/**
	 * 関数内 goto ジャンプ先収集関数
	 * 同じ本体の再照会は O(1)
	 * 計算量：走査ノード数 N，子・祖先・同名ラベルの延照合数 A，整列前の入口候補数 L に対し期待値 O(N + A + L log(L + 1))
	 * @param Body 関数本体
	 * @return ジャンプ先と其れに因る到達候補位置（当該本体の変更又は所有者の破棄迄有効）
	 * @throws FormatLimitExceeded 構文の探索又は収集中の事実の収集が作業上限を超えた場合，又は比較用の名前が未投影の場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	const GotoIdx &ControlFlow::CollectGotoTargets(const NodeId Body) const {
		// 空の本体にはノードの表を割り当てない
		if(Body == NoNode) {
			static const GotoIdx Empty;
			// 空のジャンプ先の返戻
			return Empty;
		}
		if(GotoTargets.size() < Syntax.GetNodeCount()) GotoTargets.resize(Syntax.GetNodeCount());
		Syntax.Charge(1);
		#if !defined(ShaveFormatRefFacts)
		// 同じ本体のジャンプ先の返戻
		if(GotoTargets[Body]) return *GotoTargets[Body];
		#endif
		// 完了前の要約を公開せず，既存の参照は配列の再確保から独立させる
		std::unique_ptr<GotoIdx> Built = std::make_unique<GotoIdx>();
		GotoIdx &Result = *Built;
		Result.Body = Body;
		// goto を持たない言語又は現インデックスに goto が無い場合の空の結果の返戻
		if(!Syntax.GetRules().HasLabelJump || CanExcludeRole(Role::GotoStmt)) {
			GotoTargets[Body] = std::move(Built);
			// ジャンプを持たない本体の返戻
			return Result;
		}
		using LabelIdx = std::unordered_multimap<uint32_t, NodeId>; // ラベル名の比較番号からラベルへの対応
		// ラベル名別のノードインデックス
		LabelIdx Labels;
		std::vector<NodeId> Jumps, Entries;
		Core::SyntaxModel::HasStoppedTraversal(
			Syntax,
			Body,
			[&](const NodeId Node) -> WalkStep {
				if(Syntax.Is(Node, Role::GotoStmt)) Jumps.push_back(Node);
				// 対象の識別名（名前の子を持たないエラー回復のノードは登録しない）
				else if(Syntax.Is(Node, Role::Labeled)) {
					if(const NodeId Name = Syntax.GetNamedChild(Node, 0); Syntax.Is(Name, Role::NameToken)) {
						Labels.emplace(Syntax.GetCmpNameIdx(Name), Node);
					}
				}
				// 制御フローの境界の中へ降りない事の返戻
				return IsFlowBoundary(Node) ? WalkStep::Skip : WalkStep::Descend;
			}
		);
		for(const NodeId Jump : Jumps) {
			Syntax.Charge(1);
			NodeId Target = NoNode;
			if(
				Syntax.Is(Jump, Role::CaseLabelJump) && (Syntax.HasWordChild(Jump, Word::Case) || Syntax.HasWordChild(Jump, Word::Default))
			) for(NodeId Parent = Syntax.GetParentOf(Jump); Parent != NoNode; Parent = Syntax.GetParentOf(Parent)) {
				Syntax.Charge(1);
				if(Syntax.Is(Parent, Role::SwitchStmt)) {
					Target = Parent;
					break;
				}
				if(IsFlowBoundary(Parent)) break;
			} else if(const NodeId Name = Syntax.GetNamedChild(Jump, 0); Syntax.Is(Name, Role::NameToken)) {
				// 担当範囲の終端インデックス（名前でない計算したジャンプ先は未解決とする）
				const std::pair<LabelIdx::iterator, LabelIdx::iterator> Matches = Labels.equal_range(Syntax.GetCmpNameIdx(Name));
				// 採ったジャンプ先の有効範囲（C# は goto を含む最も内の有効範囲のラベルを採る）
				NodeId Closest = NoNode;
				for(LabelIdx::const_iterator Iter = Matches.first; Iter != Matches.second; ++Iter) {
					Syntax.Charge(1);
					// 対象ラベルのノード
					const NodeId Label = Iter->second;
					if(Syntax.GetRules().IsLabelJumpScoped) {
						NodeId Scope = Syntax.GetParentOf(Label);
						while(Scope != NoNode) {
							Syntax.Charge(1);
							if(Syntax.Is(Scope, Role::ScopeBody)) break;
							Scope = Syntax.GetParentOf(Scope);
						}
						if(
							Scope == NoNode || !Syntax.HasNodeWithin(Scope, Jump) ||
							Target != NoNode && (Scope == Closest || !Syntax.HasNodeWithin(Closest, Scope))
						) continue;
						Closest = Scope;
					}
					Target = Label;
				}
			}
			// ジャンプ別の到達先インデックスへの登録
			Result.Targets.emplace(Jump, Target);
			if(Target == NoNode) Result.HasUnresolvedTarget = true;
			else Entries.push_back(Target);
		}
		// 未解決のジャンプ先を持つ関数では，何のラベルも到達不能と断定の回避
		if(Result.HasUnresolvedTarget) for(const std::pair<const uint32_t, NodeId> &Entry : Labels) {
			Syntax.Charge(1);
			Entries.push_back(Entry.second);
		}
		// 外部から到達するラベルの重複の除去
		Syntax.Charge(Entries.size());
		std::sort(Entries.begin(), Entries.end());
		Entries.erase(std::unique(Entries.begin(), Entries.end()), Entries.end());
		if(!Entries.empty()) {
			for(const NodeId Entry : Entries) {
				Syntax.Charge(1);
				Result.Entries.push_back(Entry);
			}
			// 入口の構文順への整列
			WorkSort::Stable(
				std::span(Result.Entries),
				[this](const NodeId Left, const NodeId Right) -> bool {
					// 共有する現在の構文順でラベルを並べる比較の返戻
					return Syntax.IsBefore(Left, Right);
				},
				[this](const size_t Amount) -> void {
					Syntax.Charge(Amount);
				}
			);
		}
		// 関数内で共有するジャンプ先の確定
		GotoTargets[Body] = std::move(Built);
		// 関数内で共有するジャンプ先の返戻
		return Result;
	}

	/**
	 * 返戻の補完へ影響しない登録済の属性かの判定関数
	 * @param Syntax 対象構文
	 * @param Node 属性のノード
	 * @return 標準の通常属性の役割が確定する場合 true
	 */
	bool ControlFlow::IsKnownAttribute(const SyntaxModel &Syntax, NodeId Node) {
		// 属性のラッパーだけを透過し，修飾したパスの先頭名へ降りない
		while(Syntax.IsAny(Node, { Role::Attribute, Role::InnerExpansionAttribute, Role::OuterExpansionAttribute })) {
			Syntax.Charge(1);
			Node = Syntax.GetNamedChild(Node, 0);
		}
		if(Syntax.Is(Node, Role::PreservedTokenSeq)) {
			Syntax.Charge(1);
			const NodeId Name = Syntax.GetNamedChild(Node, 0);
			// トークン列でも単独名の属性だけを標準登録へ照合する
			if(
				!Syntax.Is(Syntax.GetPrevSibling(Name), Role::OpeningBracketToken) ||
				Syntax.IsTokenRole(Syntax.GetNextSibling(Name), Role::ScopeQualifierToken)
			) return false;
			Node = Name;
		}
		// 登録した単独の名前が一致する場合だけ標準属性とする
		return !Syntax.GetChildCount(Node) && Syntax.IsWord(Node, Word::OrdinaryAttribute);
	}

	/**
	 * Rust の展開前の属性の範囲かの判定関数
	 * 属性は展開前のマクロとして本体を書き換え得る為，其の範囲の束縛・返戻を構文だけで変えない
	 * 計算量：祖先深度 H と各祖先の子の総数 C，保持する事実数 F に対し期待値 O(H + C + F)
	 * @param Syntax 構文モデル
	 * @param Node 判定を始めるノード（祖先へ遡る）
	 * @param Cache ノード毎の判定の控え（遡ったノードへ同じ結果を配る）
	 * @return ノード又は祖先が外部属性の後に在るか内部属性を持てば true
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool ControlFlow::HasUnexpandedAttribute(const SyntaxModel &Syntax, NodeId Node, std::unordered_map<NodeId, bool> &Cache) {
		// 今回遡ったノード
		std::vector<NodeId> Path;
		bool IsProtected = false;
		for(; Node != NoNode && !IsProtected; Node = Syntax.GetParentOf(Node)) {
			if(const std::unordered_map<NodeId, bool>::const_iterator Cached = Cache.find(Node); Cached != Cache.end()) {
				// 既知の判定の採用
				IsProtected = Cached->second;
				break;
			}
			Path.push_back(Node);
			// 直前の外部属性か本体の内部属性に依る保護
			for(
				NodeId Prev = Syntax.GetPrevNamedSibling(Node);
				Syntax.Is(Prev, Role::OuterExpansionAttribute);
				Prev = Syntax.GetPrevNamedSibling(Prev)
			) IsProtected = IsProtected || !IsKnownAttribute(Syntax, Prev);
			IsProtected = IsProtected || Core::SyntaxModel::HasChildMatching(
				Syntax,
				Node,
				[&Syntax](const NodeId Child) -> bool {
					// 内部属性かの返戻
					return Syntax.Is(Child, Role::InnerExpansionAttribute) && !IsKnownAttribute(Syntax, Child);
				}
			);
		}
		// 遡ったノードへの同じ判定の配布
		for(const NodeId Visited : Path) Cache.emplace(Visited, IsProtected);
		// 属性に依る保護の有無の返戻
		return IsProtected;
	}

	/**
	 * 本体内の未展開マクロ・属性が返戻の束縛又は制御の流れを変え得るかの判定関数
	 * 独立した名前付関数とマクロの定義本文を除き，シグネチャ・クロージャ・非同期ブロックも調べる
	 * 計算量：本体のノード数 V，呼出毎の束縛解決の総費用 R に対し O(V + R)
	 * @param Stmts シグネチャと本体を含む関数のノード
	 * @return 未展開の呼出又は属性が在れば true
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	bool ControlFlow::HasUnexpandedBodySyntax(const NodeId Stmts) const {
		// 本体内の未展開マクロ・属性が返戻の束縛又は制御の流れを変え得るかの判定
		bool IsFound = false;
		Core::SyntaxModel::HasStoppedTraversal(
			Syntax,
			Stmts,
			[this, &IsFound, Stmts](const NodeId Node) -> WalkStep {
				if(
					Syntax.Is(Node, Role::MacroRuleDef) ||
					Node != Stmts && Syntax.Is(Node, Role::FunctionScope) && Syntax.GetFieldChild(Node, ChildRole::Name) != NoNode
				// 独立した本体を除く事の返戻
				) return WalkStep::Skip;
				const bool IsTokenCall =
				Syntax.Is(Node, Role::ExclamationToken) && Syntax.Is(Syntax.GetParentOf(Node), Role::PreservedTokenSeq) &&
				Syntax.Is(Syntax.GetNextSibling(Node), Role::PreservedTokenSeq) && Syntax.Is(Syntax.GetPrevSibling(Node), Role::Identifier);
				if(Syntax.Is(Node, Role::MacroCall) || IsTokenCall) {
					const NodeId Macro = IsTokenCall ? Syntax.GetPrevSibling(Node) : Syntax.GetFieldChild(Node, ChildRole::Macro);
					if(Macro == NoNode) {
						IsFound = true;
						// 名前の無いマクロの未確認の効果を保つ事の返戻
						return WalkStep::Stop;
					}
					const WordSet Meanings = GetMacroMeanings(Macro);
					IsFound =
					!(Meanings.test(static_cast<size_t>(Word::TerminatingMacro)) || Meanings.test(static_cast<size_t>(Word::OrdinaryMacro)));
					if(!IsFound && !IsTokenCall) {
						IsFound = static_cast<bool>(GetMacroArguments(Syntax.GetLastNamedChild(Node)).Effects & UnknownExitEffect);
					}
				} else if(Syntax.Is(Node, Role::OuterExpansionAttribute) || Syntax.Is(Node, Role::InnerExpansionAttribute)) {
					IsFound = !IsKnownAttribute(Syntax, Node);
				}
				// 判定結果の返戻
				return IsFound ? WalkStep::Stop : WalkStep::Descend;
			}
		);
		// 未展開の呼出又は属性の有無の返戻
		return IsFound;
	}

	/**
	 * 末尾式が文の終わり迄生きる一時値を作るかの判定関数
	 * 末尾式の一時値の破棄の時機はバージョンに依りローカル変数の前後が異なる為，`return` の文へ移すと破棄の順が変わり得る
	 * 場所を要する文脈（受手・フィールド・添字・参照・比較・照合の対象）へ値を作る式を置く場合と，展開を追えないマクロを一時値の候補とする
	 * 子毎のフィールドの照合が同じ親の子を再び探索する費用も含む
	 * T は `GetTailExpr` の全照会の費用を表す
	 * 計算量：訪れるノード数 N と其々の子の数 Cᵤ，語の照合長の和 B に対し O(N + Σ Cᵤ ² + B + T)
	 * @param Tail 末尾式
	 * @return 一時値を作り得るなら true
	 * @throws FormatLimitExceeded 構文の探索又は収集中の事実の収集が作業上限を超えた場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool ControlFlow::CanCreateTailTemps(const NodeId Tail) const {
		// 場所を要する文脈の子かの判定関数
		const auto IsPlaceCtx = [this](const NodeId Parent, const NodeId Child) -> bool {
			// フィールド・添字の被演算子の返戻
			if(Syntax.Is(Parent, Role::PlaceProjection)) return Syntax.GetNamedChild(Parent, 0) == Child;
			// 参照と参照外しの被演算子の返戻
			if(
				const bool IsRefOperand = Syntax.Is(Parent, Role::RefExpr) ||
				Syntax.Is(Parent, Role::UnaryOperation) && Syntax.IsTokenRole(GetFirstChildNode(Parent), Role::AsteriskToken);
				IsRefOperand
			) return true;
			// 照合の対象は照合の終わり迄生きる事の返戻
			if(Syntax.Is(Parent, Role::ScrutineeHost)) return Syntax.GetFieldChild(Parent, ChildRole::Val) == Child;
			// 比較の演算子は被演算子を参照で受ける事の返戻
			return Syntax.Is(Parent, Role::BinaryOperation) &&
			Syntax.Is(Syntax.GetFieldChild(Parent, ChildRole::Operator), Role::BorrowingOperator);
		};
		// 名前の構文だけでは，定数の展開で生む一時値とローカルの場所を区別出来ない
		const auto IsKnownPlace = [this](const NodeId Node) -> bool {
			// 名前以外の場所と，一時値の破棄を伴わない値の返戻
			if(Syntax.GetBindingSyntax(Node) != BindingSyntax::Name) return Syntax.Is(Node, Role::PlaceOrPlainVal);
			const BindingFact Binding = Syntax.GetBindingFact(Node);
			if(Binding.Decl == NoNode || Binding.IsUncertain || Binding.IsDeclaredConstant || Binding.Namespace != BindingNamespace::Val) {
				// 場所である事を確定出来ない名前の返戻
				return false;
			}
			const BindingValSrc Source = Syntax.GetBindingValSrc(Binding);
			// 解決済のローカル変数と仮引数だけを場所として扱う事の返戻
			return Source.IsParam || Source.Val != NoNode;
		};
		// 子の走査位置を保持した一時値探索の作業列
		struct TailFrame {
			NodeId Node; // 判定中のノード
			std::span<const NodeId> Children; // 判定中のノードの子
			size_t NextChild = 0; // 次の子の位置
			bool IsEntered = false; // ノード自身の判定済状態
		};
		// 末尾式の探索の作業列と発見の状態
		std::vector<TailFrame> Stack { { Tail, {} } };
		bool IsFound = false;
		// 末尾式の一時値の候補の探索
		while(!Stack.empty() && !IsFound) {
			const NodeId Node = Stack.back().Node;
			if(!Stack.back().IsEntered) {
				Stack.back().IsEntered = true;
				if(Node != NoNode) Syntax.Charge(1);
				if(Syntax.Is(Node, Role::MacroCall)) {
					IsFound = true;
					break;
				}
				if(Node == NoNode || IsFlowBoundary(Node) || Syntax.Is(Node, Role::BodyTempScopeLoop)) {
					Stack.pop_back();
					continue;
				}
				// ブロックの末尾式への走査の絞込
				if(Syntax.Is(Node, Role::LexicalBlock)) {
					NodeId Last = NoNode;
					Core::SyntaxModel::ForEachNamedChildOf(
						Syntax,
						Node,
						[this, &Last](const NodeId Child) -> void {
							if(!Syntax.Is(Child, Role::JumpLabel)) Last = Child;
							// 終了
							return;
						}
					);
					Stack.pop_back();
					Stack.push_back({ GetTailExpr(Last), {} });
					continue;
				}
				Stack.back().Children = Syntax.GetChildrenOf(Node);
			}
			// 一時値を保つ文脈の子の選別
			if(Stack.back().NextChild == Stack.back().Children.size()) {
				Stack.pop_back();
				continue;
			}
			const NodeId Child = Stack.back().Children[Stack.back().NextChild++];
			Syntax.Charge(1);
			// トークンと，独立した一時値の範囲に置く条件・枝・本体・遅延評価の右辺の除外
			if(
				!Syntax.IsNamed(Child) || Syntax.Is(Node, Role::ConditionalVal) &&
				(Child != Syntax.GetFieldChild(Node, ChildRole::Cond) || !Syntax.Is(Child, Role::PatternCond)) ||
				Syntax.Is(Node, Role::IterationVal) && Child != Syntax.GetFieldChild(Node, ChildRole::Val) ||
				Syntax.Is(Child, Role::MatchBlock) ||
				Syntax.Is(Node, Role::BinaryOperation) && Child == Syntax.GetFieldChild(Node, ChildRole::Right) &&
				Syntax.Is(Syntax.GetFieldChild(Node, ChildRole::Operator), Role::LazyLogicalOperator)
			) continue;
			// 値・場所を変えないラッパーは，括弧の正規化より前も中身と同じ寿命を持つ
			if(IsPlaceCtx(Node, Child) && !IsKnownPlace(GetUnwrappedExpr(Syntax, Child))) IsFound = true;
			else Stack.push_back({ Child, {} });
		}
		// 一時値の候補の有無の返戻
		return IsFound;
	}

	/**
	 * 対象と祖先で共有する本体の事実の失効関数
	 * 未変更の子の事実は保持し，返戻とジェネレータは関数境界で失効を止める
	 * 計算量：祖先深度 H，解除する束縛範囲の登録数 M と変更した宣言経路の長さ D に対し期待値 O(H + M + D)
	 * @param Node テキスト又は子構造が変わる対象
	 * @throws FormatLimitExceeded 失効する祖先の走査が作業予算を超える場合
	 * @throws std::bad_alloc 未収集の束縛の所属を確保出来ない場合
	 * @throws std::length_error 所属の列の要素数上限を超える場合
	 */
	void ControlFlow::InvalidateBodyFacts(const NodeId Node) const {
		// 未使用の要約には失効の走査を要しない
		if(
			MacroArguments.empty() && ValueReturns.empty() && NoReturnDeclarations.empty() && GeneratorBodies.empty() &&
			GotoTargets.empty() && BranchStructures.empty() && TryStructures.empty() && SwitchStructures.empty() && CaseStructures.empty()
		) return;
		bool IsInBody = true;
		NodeId ChangedChild = NoNode;
		for(NodeId Up = Node; Up != NoNode; Up = Syntax.GetParentOf(Up)) {
			Syntax.Charge(1);
			BranchStructures.erase(Up);
			TryStructures.erase(Up);
			SwitchStructures.erase(Up);
			CaseStructures.erase(Up);
			if(Up < MacroArguments.size()) MacroArguments[Up].reset();
			ForgetMacroBindings(Up);
			if(IsInBody && Up < ValueReturns.size()) ValueReturns[Up].reset();
			if(IsInBody && Up < GeneratorBodies.size()) GeneratorBodies[Up].reset();
			// 非復帰の指定は宣言自身又は直下の指定の変更に依り失効し，本体だけの変更では保つ
			if(
				Up < NoReturnDeclarations.size() && (Syntax.Is(Up, Role::Decl) || Syntax.Is(Up, Role::FunctionScope)) &&
				(Up == Node || Syntax.Is(ChangedChild, Role::DeclSpecifier) || Syntax.HasField(ChangedChild, ChildRole::Declarator))
			) {
				NoReturnDeclarations[Up].reset();
				SyntaxModel::HasStoppedTraversal(
					Syntax,
					Up,
					[&](const NodeId Part) -> WalkStep {
						if(Part < NoReturnDeclarations.size()) NoReturnDeclarations[Part].reset();
						// 宣言契約と名前の範囲だけの失効の返戻
						return Part == Up || Syntax.HasField(Part, ChildRole::Declarator) ? WalkStep::Descend : WalkStep::Skip;
					}
				);
			}
			if(Up < GotoTargets.size()) GotoTargets[Up].reset();
			if(IsFlowBoundary(Up)) IsInBody = false;
			ChangedChild = Up;
		}
		// 終了
		return;
	}

	/**
	 * 対象と祖先の呼出先の構造の保持の終了関数
	 * 要約は子から作る為，保持されて居ない祖先へ達した後は探索を要しない
	 * 計算量：失効する祖先数 H と位置条件を持つ対象・親の子数 C に対し期待値 O(H + C + 1)
	 * @param Node テキスト又は子構造が変わる対象
	 * @throws FormatLimitExceeded 失効する要約の数が作業予算を超える場合
	 */
	void ControlFlow::InvalidateCallees(const NodeId Node) const {
		// 子構造に依存する抽出結果の失効
		InvalidateBodyFacts(Node);
		SimpleBody = NoNode;
		if(
			Terminations.Body != NoNode &&
			(Syntax.HasNodeWithin(GetFlowOwner(Node), Terminations.Body) || Syntax.HasNodeWithin(Terminations.Body, Node))
		) {
			Terminations.Body = NoNode;
			Terminations.Known.Clear(Syntax);
		}
		// トークン・役割・フィールドの変更は，自身と直接の親の位置条件へ影響する
		if(Node < ChildEvaluations.size()) ChildEvaluations[Node].first = nullptr;
		for(const NodeId Owner : { Node, Syntax.GetParentOf(Node) }) {
			if(Owner < EvaluationSelectors.size()) EvaluationSelectors[Owner].reset();
			const KindDef *const Def = Syntax.GetDefOf(Owner);
			// 子の役割・フィールドだけの条件は変更した子で失効し，親の定義の変更は保持した定義との一致で確認する
			if(
				ChildEvaluations.empty() || !Def || Def->Evaluation.UnevaluatedHead.empty() && !std::ranges::any_of(
					Def->Evaluation.Inputs,
					[this](const EvaluationInput &Input) -> bool {
						Syntax.Charge(1);
						// 兄弟・語・順序・別フィールドの存在へ依存する条件かの返戻
						return Input.WhenSiblingKind || !Input.AfterToken.empty() || !Input.WhenToken.empty() || Input.WhenPresent != ChildRole::None ||
						!Input.ChildKind && Input.Field == ChildRole::None;
					}
				)
			) continue;
			for(const NodeId Child : Syntax.GetChildrenOf(Owner)) {
				Syntax.Charge(1);
				if(Child < ChildEvaluations.size()) ChildEvaluations[Child].first = nullptr;
			}
		}
		Syntax.InvalidateCallees(Node);
		// 終了
		return;
	}

	/**
	 * 書換の影響する制御の流れと外側の名前の保持の終了関数
	 * 計算量：祖先深度 H，変更部分木のノード数 S，外側の名前を失効する範囲のノード数 V，解放する事実数 F，候補列の拡張量 C と変更インポートの名前の収集費用 I に対し O(H + S + V + F + C + I)
	 * @param Node 書換の前又は後に接続された対象
	 * @param IsStructure 子・接続・種別を変える書換か
	 * @param HasBindingChanges 名前・供給元又は其の所属を変える書換か
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void ControlFlow::Invalidate(const NodeId Node, const bool IsStructure, const bool HasBindingChanges) const {
		if(HasBindingChanges && !IsStructure && !Syntax.BindingScopes.empty()) {
			const NodeId Scope = GetScopeOf(Node);
			InvalidateAliases(Scope);
			// 関数の宣言契約は，本文内の編集と区別して外側の供給値にも反映する
			if(IsFlowBoundary(Scope) && !Syntax.HasNodeWithin(GetStmtsOf(Scope), Node)) {
				InvalidateAliases(GetScopeOf(Syntax.GetParentOf(Scope)));
			}
		}
		// 書換の影響する表の失効
		if(HasBindingChanges && HasTerminalMemberNames) {
			ChangedCallNames.push_back(Node);
			for(NodeId Up = Syntax.GetParentOf(Node); Up != NoNode; Up = Syntax.GetParentOf(Up)) {
				Syntax.Charge(1);
				if(Syntax.Is(Up, Role::AssignmentOperation) || Syntax.Is(Up, Role::DeleteStmt)) ChangedCallNames.push_back(Up);
				if(IsFlowBoundary(Up)) break;
			}
		}
		InvalidateCallees(Node);
		if(
			HasBindingChanges && !IsStructure && !BoundMacroArguments.empty() &&
			(Syntax.Is(Node, Role::NameToken) || Syntax.Is(Node, Role::Identifier))
		) InvalidateCallableScope(GetScopeOf(Node));
		// 書換の影響する制御の流れと外側の名前の保持の失効
		// 後始末の終端は条件のトークンにも依る為，子構造以外の変更でも同じ制御フローを失効する
		if(!LoopExits.empty()) {
			const NodeId Owner = GetFlowOwner(Node);
			LoopExits.erase(Owner == NoNode ? Syntax.GetRoot() : Owner);
		}
		// 名前を導入しない返戻の追加では，既存の子の所属・呼出先・位置条件を保つ
		const bool HasFlowChanges = !LoopExits.empty() || HasBindingChanges && IsStructure && !FlowOwners.empty();
		const bool HasBindingScopeChanges =
		HasBindingChanges && IsStructure && (!Syntax.BindingScopes.empty() || !BoundMacroArguments.empty());
		const bool HasScopeChanges = HasBindingChanges && IsStructure &&
		(HasBindingScopeChanges || Syntax.LiveCallees || !EvaluationSelectors.empty() || !ChildEvaluations.empty());
		const NodeId Scope = HasBindingScopeChanges ? GetScopeOf(Node) : NoNode;
		const NodeId Outer = HasScopeChanges && Scope == Node ? GetScopeOf(Syntax.GetParentOf(Node)) : NoNode;
		if(HasFlowChanges || HasScopeChanges) {
			Core::SyntaxModel::HasStoppedTraversal(
				Syntax,
				Node,
				[&](const NodeId Part) -> WalkStep {
					if(HasFlowChanges) {
						LoopExits.erase(Part);
						if(IsStructure) if(Part < FlowOwners.size()) FlowOwners[Part].reset();
					}
					if(HasScopeChanges) {
						if(HasBindingScopeChanges && (Syntax.BindingScopes.contains(Part) || MacroBindingScopes.contains(Part))) {
							InvalidateCallableScope(Part);
						}
						Syntax.IsCalleeErasureSuccessful(Part);
						if(Part < EvaluationSelectors.size()) EvaluationSelectors[Part].reset();
						if(Part < ChildEvaluations.size()) ChildEvaluations[Part].first = nullptr;
					}
					// 変更部分木の所有者・範囲・呼出先を同じ走査で失効する事の返戻
					return WalkStep::Descend;
				}
			);
		}
		if(HasBindingScopeChanges) {
			InvalidateCallableScope(Scope);
			if(Outer != NoNode) InvalidateCallableScope(Outer);
		}
		// 終了
		return;
	}

	/**
	 * 終端した後の返戻文を含む文の取得関数（整形仕様 8.4）
	 * 最後の返戻を含む文の前だけを判定し，入口の無い終端後は返戻の有無だけを読む
	 * 計算量：返戻の探索と必要な終端判定が読むノードの数 V に対し O(V)
	 * @param Container 文を直下に並べるノード
	 * @param Gotos 同じ関数内の goto ジャンプ先
	 * @return 到達しない返戻文を含む文（原文順）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	std::vector<NodeId> ControlFlow::GetReturningStmtsAfterTermination(const NodeId Container, const GotoIdx &Gotos) const {
		// 途中で終端せず，末尾の返戻だけを許す本体には到達不能の返戻文が無い
		if(GetSimpleEndReach(Container)) return {};
		// 順に判定する文と終端の控え・到達不能の文
		const std::vector<NodeId> Stmts = GetStmtsIn(Container);
		// 最初の文は到達不能に為らず，最後の返戻より後の終端は結果を変えない
		size_t LastReturning = Stmts.size();
		while(LastReturning && !HasReturnInFlow(Stmts[LastReturning - 1])) --LastReturning;
		// 後続の返戻を持たない本体の空の結果の返戻
		if(LastReturning < 2) return {};
		const NodeId Function = GetFlowOwner(Container);
		const bool HasJumps = !Gotos.Targets.empty() || !Gotos.Entries.empty() || Gotos.HasUnresolvedTarget;
		// goto を持つ木は，照会毎の失効を繰り返さず同じ判定範囲を１度に求める
		const TerminationResults &Known =
		EvaluateTermination(Gotos, std::span<const NodeId>(Stmts).first(LastReturning - 1), NoNode, Function, !HasJumps);
		std::vector<NodeId> Unreachable;
		bool IsClosed = false;
		for(size_t Idx = 0; Idx < LastReturning; ++Idx) {
			const NodeId Child = Stmts[Idx];
			Syntax.Charge(1);
			// 文毎の終端状態更新（ラベルへの goto は終端後の文へ入口を作る）
			if(HasGotoEntry(Gotos, Child)) IsClosed = false;
			if(IsClosed && (Idx + 1 == LastReturning || HasReturnInFlow(Child))) Unreachable.push_back(Child);
			if(!IsClosed && Idx + 1 < LastReturning) IsClosed = GetKnownTermination(Known, Child, NoNode) & CertainTermination;
		}
		// 到達しない返戻文を含む文の返戻
		return Unreachable;
	}

	/**
	 * 通常関数の本体の要約関数
	 * 計算量：参照する部分木のノード総数 N とテキストの総長 L に対し O(N + L)
	 * @param Node 判定対象ノード
	 * @param KnownShape 同じ構文から既に求めた戻値の区分（無ければ必要時に求める）
	 * @param NeedsCompletion 返戻補完の候補だけを求め，末尾の明示返戻を除くか
	 * @return 通常関数で文の並びの本体を持てば其の要約（ジェネレータ・コルーチン・値を返さない関数の空の本体，補完専用の照会では末尾の明示返戻も除く）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	std::optional<FunctionEnd> ControlFlow::ClassifyFunction(
		const NodeId Node,
		const std::optional<ReturnShape> &KnownShape,
		const bool NeedsCompletion
	) const {
		// 通常関数でないノードを対象外とする事の返戻
		if(!IsOrdinaryFunction(Node)) return std::nullopt;
		const NodeId Stmts = GetStmtsOf(Node);
		// 本体の無い宣言・式本体・エラーを含む関数の返戻
		if(Stmts == NoNode || Syntax.GetNode(Node).Facts.Has(ConstraintKind::ContainsError)) return std::nullopt;
		const NodeId Last = NeedsCompletion ? Syntax.GetLastStmt(Stmts) : NoNode;
		// 末尾の返戻が既に明示された補完の候補と，ジェネレータを分類しない事の返戻
		if(NeedsCompletion && IsReturn(Syntax, Last) || IsGeneratorLike(Node, Stmts)) return std::nullopt;
		const ReturnShape Shape = KnownShape ? *KnownShape : GetReturnShapeOf(Node);
		// 値を返さない関数の空の本体と，Python の中身の無い本体（型の宣言だけの定義を含む）の除外
		// 対象外の関数の返戻
		if(
			(Shape.Kind == ReturnKind::Void || Shape.Kind == ReturnKind::Never || Syntax.Is(Node, Role::MayDeclarePlaceholderFunction)) &&
			IsPlaceholderBody(Stmts)
		) return std::nullopt;
		// 最後の文を含む本体の要約の返戻
		return FunctionEnd{ Stmts, NeedsCompletion ? Last : Syntax.GetLastStmt(Stmts), Shape.Kind, Shape.Evidence };
	}

	/**
	 * 通常関数の末尾の到達可能性の判定関数
	 * 計算量：`CollectGotoTargets`，１回の終端判定及び必要な `GetBindingFact` の費用の和 T に対し O(T)
	 * @param Function 関数のノード
	 * @param End 関数の本体の要約
	 * @return 末尾の到達可能性
	 * @throws FormatLimitExceeded 構文の探索・事実の収集が作業上限を超えた場合，又は読む子の終端結果が未登録の場合，又は比較用の名前が未投影の場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	EndReachability ControlFlow::EndReach(const NodeId Function, const FunctionEnd &End) const {
		// 通常関数の末尾の到達可能性の判定
		const GotoIdx &Gotos = CollectGotoTargets(End.Stmts);
		// 未解決のジャンプ先に依る到達可能性の返戻
		if(Gotos.HasUnresolvedTarget) return EndReachability::Unknown;
		std::vector<NodeId> Stmts;
		for(const NodeId Child : Syntax.GetChildrenOf(Function)) {
			const KindDef *const Def = Syntax.GetDefOf(Child);
			if(Def && Def->Evaluation.Invocation != EvaluationClass::Deferred) Stmts.push_back(Child);
		}
		// 呼出時に読む見出が無く，本体が単純なら要約表を作らない
		if(Stmts.empty()) if(const std::optional<EndReachability> Reach = GetSimpleEndReach(End.Stmts)) return *Reach;
		std::vector<NodeId> Body = GetStmtsIn(End.Stmts);
		if(Stmts.empty()) Stmts = std::move(Body);
		else Stmts.insert(Stmts.end(), Body.begin(), Body.end());
		const TerminationResults &Known = EvaluateTermination(Gotos, Stmts, NoNode, Function);
		// 同じ本体の照会は要約を保ち，変更又は別の本体の評価時に失効する
		for(const NodeId Stmt : Stmts) {
			const TerminationSummary Summary = GetKnownSummary(Known, Stmt, NoNode);
			// 評価時機の事実が不足する候補では補完しない
			if(Summary.HasUnknownEvaluation) return EndReachability::Unknown;
			if(Summary.IsAbrupt) break;
		}
		const uint8_t Modes = SeqTermination(Gotos, Known, Stmts, NoNode);
		// 値の仮定を使わず終端する末尾の返戻
		if(Modes & CertainTermination) return EndReachability::Unreachable;
		// 被覆又は定数式の値に依存する末尾の返戻
		return Modes & (CoveredTermination | PessimisticTermination) ? EndReachability::Unknown : EndReachability::Reachable;
	}

	/**
	 * 暗黙返戻の候補と為る単一式の本体かの判定関数（整形仕様 5.2）
	 * 値を返す関数の本体がコメントを除いて式１つだけから成るかを調べる
	 * 計算量：本体の子の数 C に対し O(C)
	 * @param Function 関数のノード
	 * @param End 関数の本体の要約
	 * @return 単一式の本体なら true
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool ControlFlow::IsSingleImplicitReturnBody(const NodeId Function, const FunctionEnd &End) const {
		// 単一式の返戻を持ち得ない関数と，最後の文が式でない物と，偽枝の無い分岐の返戻
		if(
			!Syntax.Is(Function, Role::SingleExprResultFunction) || End.Kind != ReturnKind::Val ||
			!Syntax.Is(End.Last, Role::ImplicitReturnCandidateExpr) || Syntax.Is(End.Last, Role::If) && !GetBranchesOf(End.Last).HasElse
		) return false;
		uint32_t Count = 0;
		Core::SyntaxModel::ForEachStmtOf(
			Syntax,
			End.Stmts,
			[&](const NodeId) -> void {
				++Count;
				// 終了
				return;
			}
		);
		// 式１つだけの本体かの返戻
		return Count == 1;
	}

	/**
	 * 関数へのリザルトビルダーの適用を除外出来るかの判定関数
	 * 計算量：宣言と包含型の直接の子の総数 C に対し O(C)
	 * @param Function 対象の通常関数
	 * @return 属性・継承・適合・拡張からの適用が無い事を確認出来れば真
	 * @throws FormatLimitExceeded 構文又は要約の照会が作業予算を超える場合
	 * @throws std::out_of_range ノード又は要約の識別子が範囲外の場合
	 */
	bool ControlFlow::CanExcludeResultBuilder(const NodeId Function) const {
		// 宣言が属性を持つか
		const auto HasAttribute = [this](const NodeId Decl) -> bool {
			const NodeId Modifiers = Syntax.GetFirstNamedChildWithRole(Decl, Role::ModifierSeq);
			// 修飾の並びの中の属性の有無の返戻
			return Modifiers != NoNode && Core::SyntaxModel::HasChildMatching(
				Syntax,
				Modifiers,
				[this](const NodeId Child) -> bool {
					// 属性かの返戻
					return Syntax.Is(Child, Role::Attribute);
				}
			);
		};
		// 属性を持つ関数の返戻
		if(HasAttribute(Function)) return false;
		const NodeId Parent = Syntax.GetParentOf(Function);
		// 型の外の関数の返戻
		if(Syntax.Is(Parent, Role::SrcFileNode)) return true;
		// 型のメンバでない関数の返戻
		if(!Syntax.Is(Parent, Role::ClassMembers)) return false;
		const NodeId Nominal = Syntax.GetParentOf(Parent);
		// 適合・継承・拡張・属性の在る型のメンバの返戻
		if(
			!Syntax.Is(Nominal, Role::ImplicitReturnNominal) ||
			Syntax.GetFirstNamedChildWithRole(Nominal, Role::ImplicitReturnConformance) != NoNode ||
			Syntax.HasTokenRoleChild(Nominal, Role::ImplicitReturnExtToken) || HasAttribute(Nominal)
		) return false;
		// 最も外の範囲の型のメンバかの返戻
		return Syntax.Is(Syntax.GetParentOf(Nominal), Role::SrcFileNode);
	}

	/**
	 * 単一式の暗黙返戻を明示してもリザルトビルダーの適用が変わらないかの判定関数（整形仕様 5.2）
	 * 関数と所属型の属性，適合・継承及び拡張を調べ，リザルトビルダーを明示・推論で適用し得ない型の外の関数と型のメンバだけを許す
	 * 計算量：参照する部分木のノード総数 N とテキストの総長 L に対し O(N + L)
	 * @param Function 関数のノード
	 * @param End 関数の本体の要約
	 * @param HasSingleImplicitResult 同じ本体で１度確定した単一式の暗黙返戻の有無
	 * @return 値と束縛を構文から保てる場合 true
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 */
	bool ControlFlow::CanMakeImplicitReturnExplicit(
		const NodeId Function,
		const FunctionEnd &End,
		const bool HasSingleImplicitResult
	) const {
		// 宣言した戻値の型を持つ関数の暗黙の返戻でない物の返戻
		if(
			!Syntax.Is(Function, Role::ImplicitReturnCandidate) || !Syntax.Is(End.Last, Role::ImplicitReturnExpr) ||
			!HasSingleImplicitResult || End.Evidence != ReturnEvidence::Declared
		) return false;
		// リザルトビルダーの適用を排除した事の返戻
		return CanExcludeResultBuilder(Function);
	}

	/**
	 * 末尾の返戻の補完を構文から確かめれないかの判定関数（整形仕様 5.2, 8.4）
	 * Rust のシグネチャ・本体・包含項目に効果が未確定のマクロ・属性を持つ関数を未確定とする
	 * 破棄の順を変え得る一時値を作る末尾式と，明示返戻を確かめれない単一式も未確定とする
	 * 補完と静的解析が同じ判定を使う
	 * 属性は本文から祖先へ遡り，関数自身の内部属性も含める
	 * 計算量：属性・本体・末尾・暗黙返戻の各判定費用の和 T に対し O(T)，基準側の判定では破棄する Cache の要素数 K を加え O(T + K)
	 * @param Function 関数のノード
	 * @param End 関数の本体の要約
	 * @param HasSingleImplicitResult 同じ本体で１度確定した単一式の暗黙返戻の有無
	 * @param CanMakeExplicit 同じ関数と本体で確定した明示返戻の可否
	 * @param Cache 展開前の属性の範囲の判定の控え
	 * @return 補完の前提を確かめれなければ true
	 * @throws FormatLimitExceeded 構文の探索又は収集中の事実の収集が作業上限を超えた場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	bool ControlFlow::IsReturnCompletionUncertain(
		const NodeId Function,
		const FunctionEnd &End,
		const bool HasSingleImplicitResult,
		const bool CanMakeExplicit,
		std::unordered_map<NodeId, bool> &Cache
	) const {
		// 返戻・借用・破棄の順へ影響する未展開の属性又はマクロが在る場合の返戻
		if(
			Syntax.Is(Function, Role::AttributedTailFunction) &&
			(HasUnexpandedAttribute(Syntax, End.Stmts, Cache) || HasUnexpandedBodySyntax(Function))
		) return true;
		// 破棄の順を変え得る一時値を作る末尾式の返戻
		if(
			const NodeId Tail = Syntax.GetRules().IsTailExprVal ? GetTailExpr(End.Last) : NoNode;
			Tail != NoNode && CanCreateTailTemps(Tail)
		) return true;
		// 式の型・束縛やリザルトビルダーの適用を確かめれない暗黙の返戻かの返戻
		return HasSingleImplicitResult && !CanMakeExplicit;
	}

	/**
	 * 制御の流れの判定区間の開始関数
	 * 計算量：O(1)
	 * @param Syntax 判定する構文
	 * @throws std::bad_alloc 脱出インデックスのメモリを確保出来ない場合
	 */
	ControlFlow::ControlFlow(const SyntaxModel &Syntax) : Syntax(Syntax) {}

	/**
	 * 投影で確認した呼出効果の取得関数
	 * @param Node 呼出のノード
	 * @param IsRegisteredMacro 登録済のマクロかの格納先
	 * @return 束縛・再代入・インポートを確認した呼出の効果
	 */
	uint8_t ControlFlow::GetCallEffects(const NodeId Node, bool &IsRegisteredMacro) const {
		IsRegisteredMacro = false;
		// 呼出を持たない対象の既定の効果の返戻
		if(Node == NoNode) return 0;
		const SyntaxFacts &Facts = Syntax.GetNode(Node).Facts;
		if(Syntax.Is(Node, Role::MacroCall)) {
			const NodeId Macro = Syntax.GetFieldChild(Node, ChildRole::Macro);
			bool HasSameBinding = false;
			const WordSet Meanings = GetMacroMeanings(Macro, &HasSameBinding);
			IsRegisteredMacro =
			Meanings.test(static_cast<size_t>(Word::OrdinaryMacro)) || Meanings.test(static_cast<size_t>(Word::TerminatingMacro));
			// 同じ供給元に対する確認済の効果の返戻
			if(HasSameBinding) return Facts.CallEffects;
			// 現在も非復帰の役割を持つマクロの効果の返戻
			if(Meanings.test(static_cast<size_t>(Word::TerminatingMacro))) return ThrowEffect;
			// 通常のマクロの効果の返戻
			if(IsRegisteredMacro) return 0;
			// 新しい供給元の効果を確定しない事の返戻
			if(Facts.Reference && Facts.Reference->BindingHead != NoNode) return Facts.Reference->IsUnknown ? UnknownCallEffect : 0;
		}
		IsRegisteredMacro = Facts.IsRegisteredMacro;
		// 投影済の呼出効果の返戻
		return Facts.CallEffects;
	}

	/**
	 * 終端先へ解決され得ないメンバの事実の取得関数
	 * @param Function 呼出先
	 * @return 入力で終端先の候補から除いたメンバなら真
	 */
	bool ControlFlow::CanSkipMemberResolution(const NodeId Function) const {
		return Function != NoNode && Syntax.GetNode(Function).Facts.CannotResolveTerminalMember;
	}

	/**
	 * 投影済のインポートの導入の取得関数
	 * @param Import インポートの宣言
	 * @param IsCustom 現在の祖先の独自定義の判定
	 * @param Names 導入の格納先
	 * @throws std::bad_alloc 導入のコピーを確保出来ない場合
	 */
	void ControlFlow::CollectImportedPaths(
		const NodeId Import,
		const std::function<bool(NodeId)> &IsCustom,
		CallableNames &Names
	) const {
		const std::unordered_map<NodeId, CallableNames>::const_iterator Found = Syntax.ProjectedImports.find(Import);
		// 導入を持たないインポートの終了
		if(Found == Syntax.ProjectedImports.end()) return;
		for(const auto &[Name, Entries] : Found->second) for(const CallableIntroduction &Entry : Entries) {
			Syntax.Charge(1 + Entry.Target.size() + Entry.Member.size());
			CallableIntroduction Added = Entry;
			if(Syntax.Is(Import, Role::ImportedPathBinding) && !Entry.IsSequential) Added.IsCustomDefinition = IsCustom(Added.Name);
			Names[Name].push_back(std::move(Added));
		}
		// 終了
		return;
	}
} // namespace Core
