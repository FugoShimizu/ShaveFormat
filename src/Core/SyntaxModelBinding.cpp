#include "SyntaxModel.hpp"
#include "ControlFlow.hpp"
#include "../Util/WorkSort.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 固定の定義と現在の親・フィールドから束縛の分類を取得する関数
	 * 型名・メンバ名の分類は現在の親との関係に従って決める
	 * 計算量：親の最後の名前付の子を探す走査量 C に対し O(1 + C)
	 * @param Node ノード（無も可）
	 * @return `Profile` が投影した分類を現在の親・フィールドで解決した結果
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 */
	BindingSyntax SyntaxModel::GetBindingSyntax(const NodeId Node) const {
		// 無又は範囲外の構造の返戻
		if(Node == NoNode || Node >= Nodes.size()) return BindingSyntax::None;
		const KindDef *const Kind = GetDefOf(Node);
		if(GetRules().HasReassignableLexicalKeyword) {
			// 型の中の値の照会も，通常の束縛と同じ有効範囲を使う
			if(
				Is(Node, Role::CallableType) || Kind && Kind->Binding == BindingSyntax::None && Is(Node, Role::ReturnHintRequired) &&
				GetFieldChild(Node, ChildRole::Params) != NoNode || Is(Node, Role::TypeMember) && (
					GetFieldChild(Node, ChildRole::Params) != NoNode ||
					GetBindingSyntax(GetFieldChild(Node, ChildRole::Name)) == BindingSyntax::Name
				)
			// 型の仮引数だけを導入し，暗黙の実行時の名前を追加しない範囲の返戻
			) return BindingSyntax::Closure;
			// 内部の型照会の名前を通常の収集へ渡す構造の返戻
			if(Is(Node, Role::TypeAnnotation) || Is(Node, Role::TypeArgList) || Is(Node, Role::TypeParamList)) return BindingSyntax::None;
		}
		// 本体を持たないシグネチャの仮引数も，周囲へ漏れない導入の範囲を持つ
		if(Kind && Kind->Binding == BindingSyntax::NamePattern && Is(Node, Role::FunctionDeclarator)) {
			NodeId Owner = GetParentOf(Node);
			while(
				!Is(Owner, Role::FunctionDeclarator) && (Is(Owner, Role::DeclaratorWrapper) || Is(Owner, Role::ParenthesizedDeclarator))
			) {
				Charge(1);
				Owner = GetParentOf(Owner);
			}
			// 定義の頭部では外側の関数が仮引数を供給する
			if(!Is(Owner, Role::FunctionScope)) return BindingSyntax::Closure;
		}
		// 関数全体の束縛では，文列だけの本体が新しい範囲を作らない
		if(
			Kind && Kind->Binding == BindingSyntax::ClassBody && !Is(GetParentOf(Node), Role::MemberOwner) &&
			(GetRules().HasFunctionLocalBindings || Is(GetParentOf(Node), Role::FunctionBindingScope))
		) return BindingSyntax::None;
		// 複合した名前の内側のトークンを別の利用名として記録しない
		if(Kind && Kind->Binding == BindingSyntax::Name) {
			const KindDef *const Owner = GetDefOf(GetParentOf(Node));
			// 複合した名前が所有するトークンの返戻
			if(Owner && Owner->Binding == BindingSyntax::Name) return BindingSyntax::MemberPart;
		}
		// 型のトークンから名前を導入するフィールドの確認（移動後も現在の親とフィールドで解決する）
		if(Kind && Kind->Binding == BindingSyntax::TypeName) {
			const NodeId Owner = GetParentOf(Node);
			// 文法が名前のフィールドを持たない型仮引数でも，最初の型の名前だけを導入とする返戻
			if(
				const KindDef *const Definition = GetDefOf(Owner);
				Definition && Definition->Binding == BindingSyntax::TypeParam && GetFirstNamedChildWithRole(Owner, Role::TypeName) == Node
			// 型の参照でなく導入名である事の返戻
			) return BindingSyntax::Name;
			if(const NodeId Parent = GetParentOf(Node); Parent != NoNode && HasField(Node, ChildRole::Name)) {
				Charge(1);
				if(
					const KindDef *const Owner = GetDefOf(Parent);
					Owner && (Owner->Binding == BindingSyntax::ClassDecl || Owner->Binding == BindingSyntax::ClassExpr)
				// 型のトークンが導入名を供給する場合の返戻
				) return BindingSyntax::Name;
			}
			// 型の参照位置の返戻
			return BindingSyntax::Ignore;
		}
		if(Kind && Kind->Binding == BindingSyntax::Name) {
			const NodeId Parent = GetParentOf(Node);
			// ローカル変数の参照でないメンバ名の返戻
			if(
				const KindDef *const Owner = GetDefOf(Parent);
				Owner && (
					Owner->Binding == BindingSyntax::NamedMember && (HasField(Node, ChildRole::Name) || GetLastNamedChild(Parent) == Node) ||
					Owner->Binding == BindingSyntax::MemberFunction && GetRules().HasBareBindingParams && HasField(Node, ChildRole::Name)
				)
			) {
				NodeId Object = GetFieldChild(Parent, ChildRole::ObjectField);
				if(Object == NoNode) Object = GetNamedChild(Parent, 0);
				// 明示した受手のメンバも，同じ型の宣言の事実を読む
				if(Owner->Binding == BindingSyntax::NamedMember && Is(Object, Role::ThisRef)) return BindingSyntax::Name;
				// 型・受手を確定出来ないメンバ名の返戻
				return BindingSyntax::MemberPart;
			}
		}
		// 固定の構造の返戻
		return Kind ? Kind->Binding : BindingSyntax::None;
	}

	/**
	 * 捕捉の境界を導入する分類かの判定関数
	 * @param Shape 投影済の束縛の分類
	 * @return 関数又はクロージャなら `true`
	 */
	bool SyntaxModel::IsBindingFunction(const BindingSyntax Shape) {
		// 捕捉の境界の有無の返戻
		return Shape == BindingSyntax::Function || Shape == BindingSyntax::FunctionDecl || Shape == BindingSyntax::Closure ||
		Shape == BindingSyntax::NamedExpr || Shape == BindingSyntax::MemberFunction;
	}

	/**
	 * 捕捉の境界を導入するノードかの判定関数
	 * @param Node ノード
	 * @return 関数又はクロージャなら `true`
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 */
	bool SyntaxModel::IsBindingFunction(const NodeId Node) const {
		// 取得した分類に依る捕捉の境界の判定の返戻
		return IsBindingFunction(GetBindingSyntax(Node));
	}

	/**
	 * 名前の有効範囲を導入する分類かの判定関数
	 * @param Shape 投影済の束縛の分類
	 * @return 本体・関数・反復・捕捉等なら `true`
	 */
	bool SyntaxModel::IsBindingScope(const BindingSyntax Shape) {
		// 名前を解決する範囲の有無の返戻
		return IsBindingFunction(Shape) || Shape == BindingSyntax::Block || Shape == BindingSyntax::Catch ||
		Shape == BindingSyntax::Loop || Shape == BindingSyntax::VarBlock || Shape == BindingSyntax::ClassBody ||
		Shape == BindingSyntax::ClassDecl || Shape == BindingSyntax::ClassExpr || Shape == BindingSyntax::ModuleDecl ||
		Shape == BindingSyntax::DynamicScope;
	}

	/**
	 * 名前の有効範囲を導入するノードかの判定関数
	 * @param Node ノード
	 * @return 根・本体・関数・反復・捕捉等なら `true`
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 */
	bool SyntaxModel::IsBindingScope(const NodeId Node) const {
		// 根又は取得した分類が名前を解決する範囲を導入するかの返戻
		return Node == Root || IsBindingScope(GetBindingSyntax(Node));
	}

	/**
	 * メンバ名を関数の外側で評価する位置かの判定関数
	 * 計算量：評価するノードの祖先深度 H と，関数・親の子の総探索量 C に対し O(H + C)
	 * @param Function メンバ関数の候補
	 * @param Node 評価する名前又は式
	 * @return 計算されたメンバ名の中なら `true`
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::out_of_range 照会するノード又は子の識別子がノード表に無い場合
	 */
	bool SyntaxModel::IsOuterBindingHead(const NodeId Function, const NodeId Node) const {
		// 仮引数の導入前に外側の範囲で評価する位置かの返戻
		return GetBindingSyntax(Function) == BindingSyntax::MemberFunction &&
		HasNodeWithin(GetFieldChild(Function, ChildRole::Name), Node);
	}

	/**
	 * 固定契約と現在の本体から値のコンストラクタの導入を判定する関数
	 * 計算量：本体を探す直接の子の数 C に対し O(C)
	 * @param Node 型を導入するノード
	 * @return 本体無又は位置付のフィールドを持つ名目型のコンストラクタなら `true`
	 * @throws std::out_of_range 照会するノード又は子の識別子がノード表に無い場合
	 */
	bool SyntaxModel::HasValBindingConstructor(const NodeId Node) const {
		// 値のコンストラクタを供給しない固定契約の返戻
		if(Node == NoNode || GetNode(Node).Kind.Def->Bindings.Constructor != BindingConstructorForm::Nominal) return false;
		const NodeId Body = GetFieldChild(Node, ChildRole::Body);
		// 本体の形から確定するコンストラクタの有無の返戻
		return Body == NoNode || GetNode(Body).Kind.Def->Bindings.Constructor == BindingConstructorForm::PositionalFields;
	}

	/**
	 * 宣言から名前を導入する子の共通判定関数
	 * 計算量：O(1)
	 * @param Node 宣言の直接の子
	 * @param Shape 取得済の束縛の分類
	 * @return 宣言子・名前・パターン又は関数の宣言子なら true
	 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
	 */
	bool SyntaxModel::IsBindingDeclarationPart(const NodeId Node, const BindingSyntax Shape) const {
		// 共通の導入形の有無の返戻
		return Shape == BindingSyntax::Declarator || Shape == BindingSyntax::Name || Shape == BindingSyntax::NamePattern ||
		Shape == BindingSyntax::Pattern || Is(Node, Role::FunctionDeclarator);
	}

	/**
	 * パターンの導入・代入名だけの取得関数
	 * 既定値の右辺，計算キー，型，フィールド・添字の受手を導入・再代入名にしない
	 * 計算量：パターンの検査数 P と，分類・フィールド照会で子を辿る総量 C に対し O(P + C)
	 * @param Pattern パターンのノード
	 * @param IsUncertain 未解釈のパターンが在るかの格納先（不要なら無）
	 * @return 構文順の名前（未解釈の部分は `IsUncertain` に記録）
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 名前又はパターンの作業列のメモリを確保出来ない場合
	 * @throws std::length_error 名前又はパターンの作業列の要素数上限を超える場合
	 * @throws std::out_of_range 走査するノードがノード表に無い場合（内部の不変条件の破壊）
	 */
	std::vector<NodeId> SyntaxModel::GetBindingNames(const NodeId Pattern, bool *const IsUncertain) const {
		// パターンの導入・代入名だけの取得
		if(IsUncertain) *IsUncertain = false;
		std::vector<NodeId> Names;
		for(std::vector<NodeId> Pending { Pattern }; !Pending.empty();) {
			const NodeId Node = Pending.back();
			Pending.pop_back();
			if(Node == NoNode) {
				if(IsUncertain) *IsUncertain = true;
				continue;
			}
			Charge(1);
			switch(GetBindingSyntax(Node)) {
			case BindingSyntax::Name:
				if(!GetNode(Node).Facts.DoesDiscardBinding) Names.push_back(Node);
				break;
			case BindingSyntax::PairPattern:
				Pending.push_back(GetFieldChild(Node, ChildRole::Val));
				break;
			case BindingSyntax::DefaultPattern:
				Pending.push_back(GetFieldChild(Node, ChildRole::Left));
				break;
			case BindingSyntax::Decl:
				{
					bool HasPattern = false;
					const std::span<const NodeId> Children = GetChildrenOf(Node);
					const NodeId Val = GetFieldChild(Node, ChildRole::Val), Type = GetFieldChild(Node, ChildRole::Type);
					for(std::span<const NodeId>::reverse_iterator Child = Children.rbegin(); Child != Children.rend(); ++Child) if(
						const BindingSyntax Shape = GetBindingSyntax(*Child);
						*Child != Val && *Child != Type && IsBindingDeclarationPart(*Child, Shape)
					) {
						HasPattern = true;
						Pending.push_back(*Child);
					}
					if(!HasPattern && IsUncertain) *IsUncertain = true;
				}
				break;
			case BindingSyntax::Closure:
				if(Is(Node, Role::FunctionDeclarator)) Pending.push_back(GetFieldChild(Node, ChildRole::Declarator));
				else if(IsUncertain) *IsUncertain = true;
				break;
			case BindingSyntax::Declarator:
				{
					NodeId Name = GetFieldChild(Node, ChildRole::Name);
					if(Name == NoNode) Name = GetFieldChild(Node, ChildRole::Declarator);
					if(Name == NoNode) Name = GetFieldChild(Node, ChildRole::Pattern);
					if(Name == NoNode) Name = GetFieldChild(Node, ChildRole::Left);
					Pending.push_back(Name);
				}
				break;
			case BindingSyntax::ImportSpecifier:
				{
					const NodeId Alias = GetFieldChild(Node, ChildRole::Alias);
					Pending.push_back(Alias == NoNode ? GetFieldChild(Node, ChildRole::Name) : Alias);
				}
				break;
			case BindingSyntax::Param:
			case BindingSyntax::ConstructorParam:
			case BindingSyntax::NamePattern:
				{
					NodeId Name = GetFieldChild(Node, ChildRole::Pattern);
					if(Name == NoNode) Name = GetFieldChild(Node, ChildRole::Name);
					if(Name == NoNode) Name = GetFieldChild(Node, ChildRole::Declarator);
					if(Name == NoNode) for(const NodeId Child : GetChildrenOf(Node)) if(
						const BindingSyntax Shape = GetBindingSyntax(Child);
						Shape == BindingSyntax::Name || Shape == BindingSyntax::NamePattern || Shape == BindingSyntax::Pattern ||
						Shape == BindingSyntax::Declarator
					) {
						Name = Child;
						break;
					}
					// 型だけを持つ仮引数は名前を導入しない
					if(Name != NoNode || GetFieldChild(Node, ChildRole::Type) == NoNode) Pending.push_back(Name);
				}
				break;
			case BindingSyntax::TypeParam:
				{
					NodeId Name = GetFieldChild(Node, ChildRole::Name);
					// フィールドの名前だけでは制約の型を導入名にせず，投影した導入の構造を確認する
					const BindingSyntax NamedPart = GetBindingSyntax(Name);
					if(NamedPart != BindingSyntax::Name && NamedPart != BindingSyntax::TypeParam) Name = NoNode;
					if(Name == NoNode) for(const NodeId Child : GetChildrenOf(Node)) {
						Charge(1);
						const BindingSyntax Part = GetBindingSyntax(Child);
						if(Part == BindingSyntax::Name || Part == BindingSyntax::TypeParam) {
							Name = Child;
							break;
						}
					}
					if(Name != NoNode) Pending.push_back(Name);
				}
				break;
			case BindingSyntax::RestPattern:
				if(GetNamedChildCount(Node) == 1) Pending.push_back(GetNamedChild(Node, 0));
				else if(IsUncertain) *IsUncertain = true;
				break;
			case BindingSyntax::Pattern:
			case BindingSyntax::Params:
				{
					const std::span<const NodeId> Children = GetChildrenOf(Node);
					const ChildRole Ref = GetNode(Node).Kind.Def->Bindings.PatternRef;
					for(std::span<const NodeId>::reverse_iterator Child = Children.rbegin(); Child != Children.rend(); ++Child) {
						Charge(1);
						if(HasField(*Child, ChildRole::Type) || Ref != ChildRole::None && HasField(*Child, Ref)) continue;
						Pending.push_back(*Child);
					}
				}
				break;
			case BindingSyntax::MemberPart:
				if(GetBindingSyntax(GetParentOf(Node)) == BindingSyntax::MemberFunction && HasField(Node, ChildRole::Name)) {
					Names.push_back(Node);
				}
				break;
			case BindingSyntax::Member:
			case BindingSyntax::NamedMember:
			case BindingSyntax::Ignore:
				break;
			default:
				if(IsUncertain && IsNamed(Node)) *IsUncertain = true;
				break;
			}
		}
		// 導入・再代入名の返戻
		return Names;
	}

	/**
	 * 現在の導入位置が使う名前空間の取得関数
	 * 型と値を同じ綴りでインデックスしても，各利用は対応する名前空間だけを探索する
	 * 計算量：コンストラクタの本体を探す直接の子の数 C に対し O(1 + C)
	 * @param Name 名前のノード
	 * @return 普通の値の利用又は定義が供給する導入の名前空間
	 * @throws std::out_of_range 照会するノード又は子の識別子がノード表に無い場合
	 */
	BindingNamespace SyntaxModel::GetBindingNamespace(const NodeId Name) const {
		// 現在の導入位置が使う名前空間の取得
		if(
			const KindDef *const Definition = GetDefOf(GetParentOf(Name));
			Definition && Definition->Binding == BindingSyntax::TypeParam
		// 型仮引数の供給する名前空間の返戻
		) return Definition->Bindings.Namespace;
		if(const NodeId Parent = GetParentOf(Name); Parent != NoNode && HasField(Name, ChildRole::Name)) {
			if(
				const KindDef &Kind = *GetNode(Parent).Kind.Def;
				Kind.Binding == BindingSyntax::ClassDecl || Kind.Binding == BindingSyntax::ClassExpr ||
				Kind.Binding == BindingSyntax::ModuleDecl
			// 値も供給するコンストラクタ又は固定の名前空間の返戻
			) return HasValBindingConstructor(Parent) ? BindingNamespace::Val : Kind.Bindings.Namespace;
		}
		// 式とローカル導入が使う値の名前空間の返戻
		return BindingNamespace::Val;
	}

	/**
	 * 固定の供給契約に従う現在の仮引数の取得関数
	 * 外側の型が供給する仮引数の導入名を同一性に使い，導入先の範囲は関数自身に限定する
	 * 計算量：祖先の段数 H と調べた子の総数 C に対し O(H + C)
	 * @param Scope 仮引数を導入する関数
	 * @param Owner 仮引数の供給元の型の格納先（直接の導入なら無）
	 * @return 現在の仮引数の並び又は無
	 * @throws FormatLimitExceeded モデルの作業予算を超えた場合
	 * @throws std::out_of_range 照会するノード又は子の識別子がノード表に無い場合
	 */
	NodeId SyntaxModel::GetBindingParams(const NodeId Scope, NodeId &Owner) const {
		// 固定の供給契約に従う現在の仮引数の取得
		Owner = NoNode;
		NodeId Params = GetFieldChild(Scope, ChildRole::Params);
		if(Params == NoNode && Is(Scope, Role::TypeMember)) {
			const NodeId Name = GetFieldChild(Scope, ChildRole::Name);
			if(GetBindingSyntax(Name) == BindingSyntax::Name) Params = Name;
		}
		if(Params == NoNode) for(
			NodeId Head = GetFieldChild(Scope, ChildRole::Declarator);
			Head != NoNode;
			Head = GetFieldChild(Head, ChildRole::Declarator)
		) {
			Charge(1);
			if(const NodeId Inner = GetFieldChild(Head, ChildRole::Params); Inner != NoNode) Params = Inner;
		}
		if(Params == NoNode) for(const NodeId Child : GetChildrenOf(Scope)) {
			Charge(1);
			if(GetBindingSyntax(Child) == BindingSyntax::Params) {
				Params = Child;
				break;
			}
		}
		// 自身が持つ仮引数の返戻
		if(Params != NoNode || GetNode(Scope).Kind.Def->Bindings.Params == BindingParamSrc::Direct) return Params;
		for(NodeId Up = GetParentOf(Scope); Up != NoNode; Up = GetParentOf(Up)) {
			Charge(1);
			const BindingSyntax Shape = GetBindingSyntax(Up);
			if(Shape == BindingSyntax::ClassDecl || Shape == BindingSyntax::ClassExpr) {
				Owner = Up;
				// 外側の型が供給する現在の仮引数の返戻
				return GetFieldChild(Up, ChildRole::Params);
			}
			// 別の関数又は項目の範囲を越えた供給を証明しない事の返戻
			if(IsBindingFunction(Up) || Shape == BindingSyntax::ModuleDecl) return NoNode;
		}
		// 仮引数の供給元が無い事の返戻
		return NoNode;
	}

	/**
	 * 仮引数・捕捉・内包表記の外側で評価する子の範囲の取得関数
	 * 計算量：直接の子の照会量 C と未取得の所属の収集量 L に対し O(C + L)
	 * @param Parent 評価の境界を持つ親
	 * @param Child 親の直接の子
	 * @return 外側の有効範囲，通常の継承なら無
	 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
	 * @throws std::bad_alloc 所属のメモリを確保出来ない場合
	 * @throws std::length_error 所属の列の上限を超える場合
	 * @throws std::out_of_range ノードが表に無い場合
	 */
	NodeId SyntaxModel::GetOuterBindingScope(const NodeId Parent, const NodeId Child) const {
		// 評価の境界を持たない親の返戻
		if(Parent == NoNode) return NoNode;
		if(
			Is(Parent, Role::CaptureInitializer) && (HasField(Child, ChildRole::Right) || HasField(Child, ChildRole::Val)) ||
			GetRules().HasFunctionLocalBindings && GetBindingSyntax(Parent) == BindingSyntax::Param && HasField(Child, ChildRole::Val)
		) {
			const NodeId Scope = GetBindingScope(Parent);
			// 導入前に評価する初期値・既定値の外側の返戻
			return GetBindingScope(GetParentOf(Scope));
		}
		if(Is(Parent, Role::ComprehensionIteration) && HasField(Child, ChildRole::Right)) {
			const NodeId Scope = GetParentOf(Parent);
			// 最初の反復対象だけは内包表記の外側で評価する
			if(GetFirstNamedChildWithRole(Scope, Role::ComprehensionIteration) == Parent) return GetBindingScope(GetParentOf(Scope));
		}
		// 通常の親の範囲を継承する子の返戻
		return NoNode;
	}

	/**
	 * 親から共有する束縛の所属の取得関数
	 * 計算量：初回は表の追加量 N と未取得の祖先数 H に対し O(N + H)，再利用時は O(1)
	 * @param Node 所属を求めるノード
	 * @return 最内の範囲と領域の所有者
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc 未取得の祖先の列を確保出来ない場合
	 * @throws std::length_error 祖先の列の上限を超える場合
	 */
	const BindingLocation &SyntaxModel::GetBindingLocation(const NodeId Node) const {
		// 確定済の祖先から未取得の所属だけを内側へ継承する
		if(BindingPlaces.size() < Nodes.size()) {
			Charge(Nodes.size() - BindingPlaces.size());
			BindingPlaces.resize(Nodes.size());
		}
		std::vector<NodeId> Pending;
		for(NodeId Up = Node; Up != NoNode && BindingPlaces[Up].Scope == NoNode; Up = Nodes[Up].Parent) {
			Charge(1);
			Pending.push_back(Up);
		}
		for(const NodeId Cur : Pending | std::views::reverse) {
			Charge(1);
			const NodeId Parent = Nodes[Cur].Parent;
			BindingLocation Place = Parent == NoNode ? BindingLocation{ Root, Root, Root } : BindingPlaces[Parent];
			if(
				Parent != NoNode && GetBindingSyntax(Parent) == BindingSyntax::MemberFunction && HasField(Cur, ChildRole::Name) &&
				GetFieldChild(Parent, ChildRole::Name) == Cur
			) {
				const NodeId Outer = Nodes[Parent].Parent;
				Place = Outer == NoNode ? BindingLocation{ Root, Root, Root } : BindingPlaces[Outer];
			}
			if(const NodeId Outer = GetOuterBindingScope(Parent, Cur); Outer != NoNode) Place.Scope = Outer;
			const BindingSyntax Shape = GetBindingSyntax(Cur);
			if(Cur == Root || IsBindingScope(Shape)) Place.Scope = Cur;
			if(Shape == BindingSyntax::ModuleDecl) Place.Owner = Place.Module = Cur;
			else if(IsBindingFunction(Shape) && Place.Owner == Place.Module) Place.Owner = Cur;
			BindingPlaces[Cur] = Place;
		}
		Charge(1);
		// 確定済の所属の返戻
		return BindingPlaces[Node];
	}

	/**
	 * 移動・分類変更が影響する部分木の所属の失効関数
	 * 計算量：取得済の所属を持つ変更部分木のノード数 N に対し O(N)
	 * @param Node 変更する部分木
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc 走査の列を確保出来ない場合
	 * @throws std::length_error 走査の列の上限を超える場合
	 */
	void SyntaxModel::InvalidateBindingLocation(const NodeId Node) {
		// 未取得の親の下には取得済の所属が無い
		if(Node == NoNode) return;
		for(std::vector<NodeId> Pending { Node }; !Pending.empty();) {
			const NodeId Cur = Pending.back();
			Pending.pop_back();
			Charge(1);
			const bool HasPlace = Cur < BindingPlaces.size() && BindingPlaces[Cur].Scope != NoNode;
			const bool HasTree = Cur < TreeRoots.size() && TreeRoots[Cur] != NoNode;
			if(!HasPlace && !HasTree) continue;
			if(HasPlace) BindingPlaces[Cur] = BindingLocation();
			if(HasTree) TreeRoots[Cur] = NoNode;
			Pending.insert(Pending.end(), Nodes[Cur].Children.begin(), Nodes[Cur].Children.end());
		}
		// 終了
		return;
	}

	/**
	 * 名前を探し始める範囲の取得関数
	 * 親から所属を継承し，名前を外側で評価するメンバ頭部を除く
	 * 計算量：通常側の再利用は O(1)，初回は所属表の追加と未取得の祖先の探索量に比例する
	 * @param Node 名前又は構造のノード
	 * @return 最も近い範囲（無なら根）
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 所属表又は祖先の列を確保出来ない場合
	 * @throws std::length_error 所属表又は祖先の列の上限を超える場合
	 * @throws std::out_of_range 照会するノード又は子の識別子がノード表に無い場合
	 */
	NodeId SyntaxModel::GetBindingScope(const NodeId Node) const {
		#if defined(ShaveFormatRefFacts)
		// 名前を探し始める範囲の取得
		for(NodeId Up = Node; Up != NoNode; Up = GetParentOf(Up)) {
			Charge(1);
			// 導入前の評価範囲が定まった場合の返戻
			if(const NodeId Outer = GetOuterBindingScope(GetParentOf(Up), Up); Outer != NoNode) return Outer;
			// 最内の有効範囲の返戻
			if(IsBindingScope(Up) && !IsOuterBindingHead(Up, Node)) return Up;
		}
		// 最外の有効範囲の返戻
		return Root;
		#else
		// 空の照会は従来通り根へ解決する
		return Node == NoNode ? Root : GetBindingLocation(Node).Scope;
		#endif
	}

	/**
	 * 照合の対象を可変借用するパターンかの判定関数
	 * 計算量：パターン内のノード数 P と，語の照合の総費用 L に対し O(P + L)
	 * @param Pattern 照合又は導入のパターン
	 * @return 参照を束縛する可変のパターンが在れば `true`
	 * @throws FormatLimitExceeded パターンの走査で作業予算を超えた場合
	 * @throws std::bad_alloc パターンを探索する作業列のメモリを確保出来ない場合
	 * @throws std::length_error パターンを探索する作業列の要素数上限を超える場合
	 */
	bool SyntaxModel::HasMutableBindingBorrow(const NodeId Pattern) const {
		// 何れかの部分のパターンが可変借用するかの返戻
		return Core::SyntaxModel::HasNodeMatching(
			*this,
			Pattern,
			[&](const NodeId Sub) -> bool {
				// 参照を束縛するパターンの可変指定の有無の返戻
				return Is(Sub, Role::RefBindingPattern) && HasWordChild(Sub, Word::RefBinding) && (
					Is(Sub, Role::RefPattern) ?
					Is(GetNamedChild(Sub, 0), Role::MutablePattern) :
					GetFirstNamedChildWithRole(Sub, Role::MutableSpecifier) != NoNode
				);
			}
		);
	}

	/**
	 * 現在の範囲の名前と供給元の共有取得関数
	 * 計算量：未収集の範囲の構築費用 Q に対し O(Q + 1)，再照会は期待値 O(1)
	 * @param Scope 名前の有効範囲
	 * @return 宣言・可視性・呼出の供給元を共有する記録
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc 範囲の記録を確保出来ない場合
	 * @throws std::length_error 範囲の表の上限を超える場合
	 * @throws std::out_of_range ノードが表に無い場合
	 */
	const SyntaxModel::BindingScope &SyntaxModel::GetBindingScopeFacts(const NodeId Scope) const {
		Charge(1);
		#if defined(ShaveFormatRefFacts)
		const std::shared_ptr<const BindingScope> Result = std::make_shared<const BindingScope>(BuildBindingScope(Scope));
		RefFacts.push_back(Result);
		// 独立に収集した範囲の返戻
		return *Result;
		#else
		if(
			const std::unordered_map<NodeId, BindingScope>::const_iterator Found = BindingScopes.find(Scope);
			Found != BindingScopes.end()
		// 現在の範囲の共有記録の返戻
		) return Found->second;
		const BindingScope &Built = BindingScopes.emplace(Scope, BuildBindingScope(Scope)).first->second;
		if(Built.ParamOwner != NoNode) ParamScopes.emplace(Built.ParamOwner, Scope);
		if(Built.SourceOwner != NoNode) {
			Charge(1);
			SourceReaders[Built.SourceOwner].insert(Scope);
		}
		// 同じ宣言収集の記録の返戻
		return Built;
		#endif
	}

	/**
	 * 現在の構造から名前を収集する関数
	 * 関数自身では仮引数と頭部の導入名を収集し，本体の宣言は本体の範囲で収集する
	 * 本体等の範囲では，関数の範囲へ属する宣言だけを内側のブロックからも収集する
	 * 計算量：範囲・パターン・綴りの総走査量 S，名前の数 K，祖先の総走査量 A に対し期待値 O(S + K + A)
	 * @param Scope 名前を導入する範囲
	 * @return 名前のインデックスと未確定な名前供給の有無
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 名前インデックス・宣言列又は作業列のメモリを確保出来ない場合
	 * @throws std::length_error 宣言列又は作業列の要素数上限を超える場合
	 * @throws std::out_of_range 走査するノードがノード表に無い場合（内部の不変条件の破壊）
	 */
	SyntaxModel::BindingScope SyntaxModel::BuildBindingScope(const NodeId Scope) const {
		// 現在の構造から名前を収集
		BindingScope Result;
		const ControlFlow Flow(*this);
		const std::shared_ptr<CallableIndex> Sources = std::make_shared<CallableIndex>();
		Result.Sources = Sources;
		const bool HasFunctionBindings = GetRules().HasFunctionLocalBindings || Is(Scope, Role::FunctionBindingScope);
		const bool HasConstantDecl = !GetRules().GetSpelling(Word::ConstantDecl).empty();
		std::unordered_set<uint32_t> Seen;
		std::unordered_map<uint64_t, bool> Constants; // 最下位ビットが０なら導入時，１なら祖先として読む結果
		std::vector<std::pair<NodeId, bool>> ConstantWork;
		// 名前の導入位置と可視範囲の登録
		const auto Add = [&](
			const NodeId Pattern,
			const NodeId Site,
			const bool IsOrdered,
			const bool IsFunctionScoped = false,
			const NodeId VisibleWithin = NoNode,
			const BindingVisibility Visibility = BindingVisibility::Lexical
		) -> void {
			bool IsUncertain = false;
			const std::vector<NodeId> Names = GetBindingNames(Pattern, &IsUncertain);
			Result.HasUncertainNames |= IsUncertain;
			for(const NodeId Name : Names) {
				if(!GetNode(Name).Facts.NameIdx) {
					Result.HasUncertainNames = true;
					continue;
				}
				const uint32_t NameIdx = GetCmpNameIdx(Name);
				// 関数全体の同じローカル名への代入は新しい束縛を導入しない
				if(
					IsFunctionScoped && HasFunctionBindings && (
						Result.Names.contains(NameIdx) ||
						!GetRules().HasFunctionLocalBindings && Is(Site, Role::FirstAssignmentBinding) && Seen.contains(NameIdx)
					)
				) continue;
				const auto [Constant, IsNew] = Constants.try_emplace(uint64_t{ Site } << 1, false);
				bool &IsConstant = Constant->second;
				if(IsNew && HasConstantDecl && GetBindingSyntax(Site) != BindingSyntax::TypeParam) {
					const NodeId NameOwner = GetParentOf(Name);
					std::vector<std::pair<NodeId, bool>> &PendingConstants = ConstantWork;
					PendingConstants.clear();
					bool HasAncestorConstant = false;
					for(NodeId Owner = Site; Owner != NoNode && Owner != Scope && !IsBindingFunction(Owner); Owner = GetParentOf(Owner)) {
						Charge(1);
						if(Owner != Site && Owner != NameOwner) if(
							const std::unordered_map<uint64_t, bool>::const_iterator Known = Constants.find(uint64_t{ Owner } << 1 | 1);
							Known != Constants.end()
						) {
							HasAncestorConstant = Known->second;
							break;
						}
						bool HasConstant = false;
						for(const NodeId Child : GetChildrenOf(Owner)) {
							Charge(1);
							if(
								Child == Name || HasField(Child, ChildRole::Val) || HasField(Child, ChildRole::Right) || Is(Child, Role::VarDeclarator) ||
								Is(Child, Role::VarDecl) || HasField(Child, ChildRole::Body)
							) break;
							HasConstant |= HasNodeMatching(
								*this,
								Child,
								[&](const NodeId Modifier) -> bool {
									return !GetChildCount(Modifier) && IsWord(Modifier, Word::ConstantDecl);
								}
							);
						}
						PendingConstants.emplace_back(Owner, HasConstant);
					}
					// 導入名で探索を止める結果と，外側の導入を祖先として読む結果を区別する
					for(const std::pair<NodeId, bool> &Part : PendingConstants | std::views::reverse) {
						Charge(1);
						HasAncestorConstant |= Part.second;
						if(Part.first != Site && Part.first != NameOwner) Constants.emplace(uint64_t{ Part.first } << 1 | 1, HasAncestorConstant);
					}
					IsConstant = HasAncestorConstant;
				}
				Flow.CollectBindingSource(Scope, Name, Site, Sources->Names);
				Charge(1);
				const CallableNames::const_iterator Source = Sources->Names.find(NameIdx);
				const bool IsSequential = Source != Sources->Names.end() && !Source->second.empty() && Source->second.back().IsSequential;
				const BindingDecl Decl { Name, Site, IsOrdered || IsSequential, IsFunctionScoped, VisibleWithin, Visibility, IsConstant };
				if(GetBindingNamespace(Name) == BindingNamespace::Val) Result.Names[NameIdx].push_back(Decl);
				else Result.TypeNames[NameIdx].push_back(Decl);
				// 同じ項目の型の導入も保ち，値のコンストラクタと型の利用を別のインデックスへ入れる
				if(HasValBindingConstructor(Site)) Result.TypeNames[NameIdx].push_back(Decl);
			}
			// 終了
			return;
		};
		const BindingSyntax Shape = GetBindingSyntax(Scope);
		// 頭部の型仮引数は定義の可視範囲へ１度だけ導入し，本体の宣言の収集とは分ける
		for(const NodeId Child : GetChildrenOf(Scope)) if(Is(Child, Role::GenericParams)) {
			Charge(1);
			for(const NodeId Param : GetChildrenOf(Child)) if(GetBindingSyntax(Param) == BindingSyntax::TypeParam) {
				Charge(1);
				Add(Param, Param, false);
			}
		}
		if(IsBindingFunction(Shape)) {
			const NodeId Params = GetBindingParams(Scope, Result.ParamOwner);
			Result.HasUncertainNames = Params == NoNode && GetNode(Scope).Kind.Def->Bindings.Params == BindingParamSrc::EnclosingType;
			if(
				const BindingSyntax ParamShape = GetBindingSyntax(Params);
				ParamShape == BindingSyntax::Name || ParamShape == BindingSyntax::Param
			) Add(Params, Scope, false);
			else if(Params != NoNode) {
				for(const NodeId Param : GetChildrenOf(Params)) {
					if(!IsNamed(Param)) continue;
					// フィールドを持たない既定値は仮引数と兄弟に現れる為，導入する構造だけを読む
					if(
						const BindingSyntax ParamShape = GetBindingSyntax(Param);
						GetRules().HasOrderedBindings && ParamShape != BindingSyntax::Param && ParamShape != BindingSyntax::NamePattern &&
						ParamShape != BindingSyntax::Pattern && ParamShape != BindingSyntax::Params &&
						!(GetRules().HasBareBindingParams && ParamShape == BindingSyntax::Name)
					) continue;
					Add(Param, Scope, false);
				}
			}
			if(
				Shape != BindingSyntax::Closure && !GetRules().ImplicitFunctionBinding.empty() &&
				!Result.Names.contains(ImplicitFunctionNameIdx)
			) Result.Names[ImplicitFunctionNameIdx].push_back({ Scope, Scope, false });
			if(Shape == BindingSyntax::NamedExpr) {
				if(
					const NodeId Name = GetFieldChild(Scope, ChildRole::Name);
					Name != NoNode && (!GetNode(Name).Facts.NameIdx || !Result.Names.contains(GetCmpNameIdx(Name)))
				) Add(Name, Scope, false);
			}
			Result.HasImplicitParam = Shape == BindingSyntax::Closure && !GetRules().ImplicitBindingParam.empty() && Params == NoNode &&
			!HasTokenRoleChild(Scope, Role::RightArrowToken);
			Result.HasUnknownReceiver = Result.HasUnknownReceiver || Shape == BindingSyntax::Closure && GetRules().HasLocalBindingPriority;
			// 非単純な仮引数の既定値の環境と，本体の `var` の環境を区別する
			Result.HasSeparateParams = Params != NoNode && Core::SyntaxModel::HasNodeMatching(
				*this,
				Params,
				[&](const NodeId Node) -> bool {
					const BindingSyntax Part = GetBindingSyntax(Node);
					// 非単純な仮引数の有無の返戻
					return Part == BindingSyntax::DefaultPattern || Part == BindingSyntax::RestPattern || Part == BindingSyntax::Pattern ||
					Part == BindingSyntax::Param && GetFieldChild(Node, ChildRole::Val) != NoNode;
				}
			);
			// 捕捉の初期化名は関数の本体から共有する
			for(const NodeId Child : GetChildrenOf(Scope)) {
				if(Is(Child, Role::CaptureList)) for(const NodeId Capture : GetChildrenOf(Child)) if(Is(Capture, Role::CaptureInitializer)) {
					Charge(1);
					Add(GetFieldChild(Capture, ChildRole::Left), Capture, false);
				}
			}
			// 本体も同じ範囲へ導入する関数以外の頭部の名前の返戻
			if(!HasFunctionBindings) return Result;
		}
		if(Shape == BindingSyntax::ClassExpr) if(const NodeId Name = GetFieldChild(Scope, ChildRole::Name); Name != NoNode) {
			Add(Name, Scope, false);
		}
		if(Shape == BindingSyntax::ClassDecl || Shape == BindingSyntax::ClassExpr) {
			for(const NodeId Child : GetChildrenOf(Scope)) if(GetBindingSyntax(Child) == BindingSyntax::Params) {
				for(const NodeId Param : GetChildrenOf(Child)) if(GetBindingSyntax(Param) == BindingSyntax::ConstructorParam) {
					const NodeId Marker = GetFirstNamedChildWithRole(Param, Role::BindingMarker);
					const bool IsProperty = IsWord(Marker, Word::ImmutableBinding) || IsWord(Marker, Word::ReassignableBinding);
					Add(Param, Param, false, false, NoNode, IsProperty ? BindingVisibility::ConstructorMember : BindingVisibility::Init);
				}
			}
		}
		Result.HasUnknownReceiver = Result.HasUnknownReceiver || Shape == BindingSyntax::DynamicScope ||
		Shape == BindingSyntax::ClassBody && GetRules().HasMemberBindingReceiver;
		if(Shape == BindingSyntax::Catch) {
			NodeId Param = GetFieldChild(Scope, ChildRole::Params);
			if(Param == NoNode) Param = GetFieldChild(Scope, ChildRole::Pattern);
			if(Param == NoNode) for(const NodeId Child : GetChildrenOf(Scope)) {
				if(const BindingSyntax Part = GetBindingSyntax(Child); Part == BindingSyntax::Name || Part == BindingSyntax::Param) {
					Param = Child;
					break;
				}
			}
			if(Param != NoNode) Add(Param, Scope, false);
		}
		if(Shape == BindingSyntax::Loop) {
			if(Is(Scope, Role::ForRangeLoop)) {
				Add(GetFieldChild(Scope, ChildRole::Declarator), Scope, false, false, GetFieldChild(Scope, ChildRole::Body));
			} else if(GetRules().HasOrderedBindings) {
				NodeId Body = GetFieldChild(Scope, ChildRole::Body);
				if(Body == NoNode) for(const NodeId Child : GetChildrenOf(Scope)) if(GetBindingSyntax(Child) == BindingSyntax::Block) {
					Body = Child;
				}
				NodeId Pattern = GetFieldChild(Scope, ChildRole::Pattern);
				if(Pattern == NoNode) Pattern = GetFieldChild(Scope, ChildRole::Name);
				if(Pattern != NoNode) Add(Pattern, Scope, false, false, Body);
				else for(const NodeId Child : GetChildrenOf(Scope)) {
					if(const BindingSyntax Part = GetBindingSyntax(Child); Part == BindingSyntax::NamePattern || Part == BindingSyntax::Pattern) {
						Add(Child, Scope, false, false, Body);
					}
				}
			} else if(HasWordChild(Scope, Word::ReassignableBinding) || HasWordChild(Scope, Word::ImmutableBinding)) {
				Add(GetFieldChild(Scope, ChildRole::Left), Scope, false);
			}
		}
		if(Is(Scope, Role::ComprehensionScope)) {
			for(const NodeId Clause : GetChildrenOf(Scope)) if(Is(Clause, Role::ComprehensionIteration)) {
				Charge(1);
				Add(GetFieldChild(Clause, ChildRole::Left), Clause, false);
			}
		}
		bool ShouldCollectFunctionDecls = Scope == Root || Shape == BindingSyntax::VarBlock;
		if(!ShouldCollectFunctionDecls) {
			const BindingSyntax ParentShape = GetBindingSyntax(GetParentOf(Scope));
			ShouldCollectFunctionDecls = IsBindingFunction(ParentShape) || ParentShape == BindingSyntax::VarBlock;
		}
		for(std::vector<NodeId> Pending(GetChildrenOf(Scope).rbegin(), GetChildrenOf(Scope).rend()); !Pending.empty();) {
			const NodeId Node = Pending.back();
			Pending.pop_back();
			Charge(1);
			const BindingSyntax Part = GetBindingSyntax(Node);
			// インポートの導入と供給元も，同じ範囲の訪問で１度だけ登録する
			if(GetBindingScope(GetParentOf(Node)) == Scope) {
				CallableNames Imports;
				if(IsAny(Node, { Role::ImportedMemberBinding, Role::ImportedModuleBinding, Role::ImportedPathBinding })) {
					Flow.CollectImportedPaths(
						Node,
						[](const NodeId) -> bool {
							return false;
						},
						Imports
					);
				}
				for(auto &[Name, Introductions] : Imports) {
					Charge(1);
					for(const CallableIntroduction &Introduction : Introductions) {
						Charge(1);
						Result.Names[Name].push_back(
							{
								Introduction.Name,
								Node,
								Introduction.IsSequential ||
								GetRules().HasOrderedBindings && Scope != Root && Shape != BindingSyntax::ClassBody && !Introduction.IsScopeWide
							}
						);
					}
					std::vector<CallableIntroduction> &Target = Sources->Names[Name];
					Target.insert(Target.end(), std::make_move_iterator(Introductions.begin()), std::make_move_iterator(Introductions.end()));
				}
				Flow.CollectBindingChanges(Scope, Node, Sources->Names);
				if(Is(Node, Role::MacroRuleDef)) Add(GetFieldChild(Node, ChildRole::Name), Node, false);
				if(!Is(Node, Role::ImportedPathBinding) && GetNode(Node).Kind.Def->Bindings.ValueInput != ChildRole::Alias) {
					if(const NodeId Alias = GetFieldChild(Node, ChildRole::Alias); Alias != NoNode) Add(Alias, Node, false);
				}
			}
			Result.HasUncertainNames |=
			Is(Node, Role::Error) || Is(Node, Role::UncertainLocalBindings) || Is(Node, Role::TypeHintExternalBinding) ||
			GetFirstNamedChildWithRole(Node, Role::ImportWildcard) != NoNode ||
			HasFunctionBindings && !GetChildCount(Node) && IsWord(Node, Word::DynamicScope);
			if(
				Part == BindingSyntax::Ignore || Part == BindingSyntax::MemberPart || Part == BindingSyntax::TypeParam ||
				Is(Node, Role::ImportedPathBinding) || Is(Node, Role::ImportedMemberBinding) || Is(Node, Role::ImportedModuleBinding)
			) continue;
			if(
				HasFunctionBindings && !GetRules().HasFunctionLocalBindings && Part == BindingSyntax::Name && GetNode(Node).Facts.NameIdx &&
				GetBindingScope(Node) == Scope
			) Seen.insert(GetCmpNameIdx(Node));
			if(
				(
					Part == BindingSyntax::FunctionDecl || Part == BindingSyntax::MemberFunction || Part == BindingSyntax::ClassDecl ||
					Part == BindingSyntax::ModuleDecl
				) && GetBindingScope(GetParentOf(Node)) == Scope
			) {
				NodeId Name = GetFieldChild(Node, ChildRole::Name);
				if(Name == NoNode) for(const NodeId Child : GetChildrenOf(Node)) if(GetBindingSyntax(Child) == BindingSyntax::Name) {
					Name = Child;
					break;
				}
				if(Name == NoNode) Name = GetFieldChild(Node, ChildRole::Declarator);
				if(Name != NoNode) {
					Add(
						Name,
						Node,
						false,
						false,
						NoNode,
						GetRules().HasLocalBindingPriority && Shape == BindingSyntax::ClassBody ?
						BindingVisibility::Receiver :
						BindingVisibility::Lexical
					);
				}
			}
			if(HasFunctionBindings && Is(Node, Role::FirstAssignmentBinding) && GetBindingScope(GetParentOf(Node)) == Scope) {
				Add(GetFieldChild(Node, ChildRole::Left), Node, true, true);
			}
			if(GetRules().HasFunctionLocalBindings && Is(Node, Role::Loop) && GetBindingScope(GetParentOf(Node)) == Scope) {
				Add(GetFieldChild(Node, ChildRole::Left), Node, false, true);
			}
			if(Part == BindingSyntax::Decl) {
				const bool IsFunctionScoped = HasWordChild(Node, Word::FunctionScopedBinding);
				NodeId Target = GetBindingScope(GetParentOf(Node));
				while(IsFunctionScoped && Target != Root) {
					const BindingSyntax ParentShape = GetBindingSyntax(GetParentOf(Target));
					if(
						IsBindingFunction(ParentShape) || GetBindingSyntax(Target) == BindingSyntax::VarBlock || ParentShape == BindingSyntax::VarBlock
					) break;
					Charge(1);
					Target = GetBindingScope(GetParentOf(Target));
				}
				if(Target == Scope) {
					const NodeId Val = GetFieldChild(Node, ChildRole::Val), Type = GetFieldChild(Node, ChildRole::Type);
					for(const NodeId Child : GetChildrenOf(Node)) {
						if(
							const BindingSyntax ChildShape = GetBindingSyntax(Child);
							Child != Val && Child != Type && IsBindingDeclarationPart(Child, ChildShape)
						) {
							Add(
								Child,
								ChildShape == BindingSyntax::Declarator ? Child : Node,
								GetRules().HasOrderedBindings && Shape != BindingSyntax::ClassBody,
								IsFunctionScoped,
								NoNode,
								GetRules().HasLocalBindingPriority && Shape == BindingSyntax::ClassBody ?
								BindingVisibility::Receiver :
								BindingVisibility::Lexical
							);
						}
					}
				}
			}
			if(
				Part == BindingSyntax::Declarator && GetBindingSyntax(GetParentOf(Node)) != BindingSyntax::Decl &&
				GetBindingScope(GetParentOf(Node)) == Scope
			) {
				const bool IsShort = Is(Node, Role::ShortVarDecl);
				const NodeId Left = GetFieldChild(Node, ChildRole::Left);
				// 新規の導入と再代入を区別出来ない複数の短縮宣言は確定しない
				if(IsShort && GetBindingNames(Left).size() != 1) Result.HasUncertainNames = true;
				else Add(Node, Node, GetRules().HasOrderedBindings, HasFunctionBindings);
			}
			if(Part == BindingSyntax::Loop && HasWordChild(Node, Word::FunctionScopedBinding) && ShouldCollectFunctionDecls) {
				Add(GetFieldChild(Node, ChildRole::Left), Node, false, true);
			}
			if(Part == BindingSyntax::Import) {
				for(const NodeId Child : GetChildrenOf(Node)) if(GetBindingSyntax(Child) == BindingSyntax::Pattern) Add(Child, Node, false);
			}
			if(
				IsBindingScope(Part) && (
					!ShouldCollectFunctionDecls || IsBindingFunction(Part) || Part == BindingSyntax::ClassBody ||
					Part == BindingSyntax::ClassDecl || Part == BindingSyntax::ClassExpr || Part == BindingSyntax::ModuleDecl
				)
			) continue;
			const std::span<const NodeId> Children = GetChildrenOf(Node);
			Pending.insert(Pending.end(), Children.rbegin(), Children.rend());
		}
		// 宣言の順序と供給元の修飾は同じ収集の記録から確定する
		bool HasAliases = false;
		for(auto &[Name, Introductions] : Sources->Names) {
			const auto IsBefore = [&](const CallableIntroduction &Left, const CallableIntroduction &Right) -> bool {
				return this->IsBefore(Left.Name, Right.Name);
			};
			if(!WorkSort::IsSorted(std::span<const CallableIntroduction>(Introductions), IsBefore)) {
				WorkSort::Stable(
					std::span(Introductions),
					IsBefore,
					[this](const size_t Amount) -> void {
						Charge(Amount);
					}
				);
			}
			for(size_t Position = 0; Position < Introductions.size(); ++Position) {
				Charge(1);
				const CallableIntroduction &Introduction = Introductions[Position];
				HasAliases = HasAliases || Introduction.AliasSrc != NoNode;
				if(Introduction.Member.empty() || !Introduction.Target.empty()) continue;
				Charge(Introduction.Member.size());
				Sources->MemberSizes[Name].insert(Introduction.Member.size());
				Sources->MemberAssignments[Introduction.Member].emplace_back(Name, Position);
			}
		}
		// ハッシュ表の巡回順を，メンバ変更の短絡判定へ持ち込まない
		for(auto &[Member, Positions] : Sources->MemberAssignments) {
			Charge(Positions.size());
			std::sort(Positions.begin(), Positions.end());
		}
		if(HasAliases) Result.SourceOwner = GetBindingRegionOwner(Scope);
		// 範囲が導入する名前の返戻
		return Result;
	}

	/**
	 * 束縛又は基底の可変性を必要とする対象の取得関数
	 * 計算量：直接の子の数 C，トークンの長さの和 L，借用パターンの走査量 P に対し O(C + L + P)
	 * @param Node 書込を構成するノード
	 * @param Shape 取得済の束縛の分類
	 * @return 更新対象のノード（導入又は対象外の演算なら無）
	 * @throws FormatLimitExceeded モデルの作業予算を超えた場合
	 * @throws std::bad_alloc 借用パターンを探索する作業列のメモリを確保出来ない場合
	 * @throws std::length_error 借用パターンを探索する作業列の要素数上限を超える場合
	 * @throws std::out_of_range 走査するノードがノード表に無い場合（内部の不変条件の破壊）
	 */
	NodeId SyntaxModel::GetBindingWriteTarget(const NodeId Node, const BindingSyntax Shape) const {
		// 束縛又は基底の可変性を必要とする対象の取得
		switch(Shape) {
		case BindingSyntax::Declarator:
			// 関数全体で共有するローカル名への代入先の返戻
			if(GetRules().HasFunctionLocalBindings) return GetFieldChild(Node, ChildRole::Left);
			break;
		case BindingSyntax::Write:
			if(const NodeId Target = GetFieldChild(Node, ChildRole::Left); Target != NoNode) return Target;
			return GetNamedChild(Node, 0);
		case BindingSyntax::Update:
			return HasTokenChild(Node, GetRules().Operators.Increment) || HasTokenChild(Node, GetRules().Operators.Decrement) ?
			GetNamedChild(Node, 0) :
			NoNode;
		case BindingSyntax::Loop:
			return !GetRules().HasOrderedBindings && !HasWordChild(Node, Word::ReassignableBinding) &&
			!HasWordChild(Node, Word::ImmutableBinding) && !HasWordChild(Node, Word::FunctionScopedBinding) ?
			GetFieldChild(Node, ChildRole::Left) :
			NoNode;
		}
		// 基底の可変性を必要としない記法の返戻
		if(!GetRules().CanMutateBase) return NoNode;
		if(Is(Node, Role::BorrowExpr) && GetFirstNamedChildWithRole(Node, Role::MutableSpecifier) != NoNode) {
			// 可変借用の対象の返戻
			return GetFieldChild(Node, ChildRole::Val);
		}
		// 受手又は可変のクロージャを呼ぶ対象の返戻
		if(Is(Node, Role::ReceiverMutationCall)) return GetNamedChild(Node, 0);
		// 照合の腕又は参照束縛が可変借用する対象の返戻
		if(
			const bool HasMutableValBorrow = Is(Node, Role::BorrowingMatch) && Core::SyntaxModel::HasChildMatching(
				*this,
				GetFieldChild(Node, ChildRole::Body),
				[&](const NodeId Arm) -> bool {
					// 可変借用する腕かの返戻
					return HasMutableBindingBorrow(GetFieldChild(Arm, ChildRole::Pattern));
				}
			) ||
			Is(Node, Role::MutablePatternBinding) && HasMutableBindingBorrow(GetFieldChild(Node, ChildRole::Pattern)); HasMutableValBorrow
		) return GetFieldChild(Node, ChildRole::Val);
		// 更新を構成しない普通のノードの返戻
		return NoNode;
	}

	/**
	 * 基底の変更を含む書込先の名前の取得関数
	 * フィールド・添字の受手を変更する記法では，分割した左辺の基底だけを取得する
	 * 計算量：左辺の走査量 S と基底連鎖の総走査量 B に対し O(S + B)
	 * @param Target 更新対象のノード
	 * @param IsUncertain 未解釈の左辺が在るかの格納先
	 * @return 現在の左辺が変更する名前のノード
	 * @throws FormatLimitExceeded 左辺又は基底の走査で作業予算を超えた場合
	 * @throws std::bad_alloc 書込先の名前又は作業列のメモリを確保出来ない場合
	 * @throws std::length_error 書込先の名前又は作業列の要素数上限を超える場合
	 * @throws std::out_of_range 走査するノードがノード表に無い場合（内部の不変条件の破壊）
	 */
	std::vector<NodeId> SyntaxModel::GetBindingWriteNames(const NodeId Target, bool &IsUncertain) const {
		// 束縛だけを更新する記法の返戻
		if(!GetRules().CanMutateBase) return GetBindingNames(Target, &IsUncertain);
		IsUncertain = false;
		std::vector<NodeId> Names;
		for(std::vector<NodeId> Pending { Target }; !Pending.empty();) {
			const NodeId Node = Pending.back();
			Pending.pop_back();
			Charge(1);
			if(!IsNamed(Node)) continue;
			NodeId Base = Node;
			while(Base != NoNode && !Is(Base, Role::Identifier) && Is(Base, Role::MutationBaseTraversal)) {
				Charge(1);
				Base = GetNamedChild(Base, 0);
			}
			if(Is(Base, Role::Identifier)) {
				Names.push_back(Base);
				continue;
			}
			if(const BindingSyntax Shape = GetBindingSyntax(Node); Shape == BindingSyntax::Ignore || Shape == BindingSyntax::MemberPart) {
				continue;
			}
			const std::span<const NodeId> Children = GetChildrenOf(Node);
			if(Children.empty()) IsUncertain = true;
			Pending.insert(Pending.end(), Children.rbegin(), Children.rend());
		}
		// 基底を解決出来ない左辺は作用無と断定しない
		IsUncertain |= Names.empty();
		// 基底の名前の返戻
		return Names;
	}

	/**
	 * コンストラクタの初期化名が見える位置かの判定関数
	 * 基底の構築・委譲と既定値を頭部とし，本体の初期化子とは区別する
	 * 初期化式の中のクロージャも同じ名前を捕捉し，メンバ関数・ゲッター・セッターの本体とは区別する
	 * 計算量：名前の祖先深度 H と，分類・フィールド照会で子を辿る総量 C に対し O(H + C)
	 * @param Scope 型の名前を導入する範囲
	 * @param Name 参照する名前
	 * @param ShouldIncludeBody 本体の初期化領域も照会するか
	 * @return 初期化領域とコンストラクタ頭部の可視性
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::out_of_range 照会するノード又は子の識別子がノード表に無い場合
	 */
	std::pair<bool, bool> SyntaxModel::GetBindingInitUse(
		const NodeId Scope,
		const NodeId Name,
		const bool ShouldIncludeBody
	) const {
		// コンストラクタの初期化名が見える位置かの判定
		NodeId Child = Name, Member = NoNode;
		for(NodeId Up = GetParentOf(Name); Up != NoNode; Child = Up, Up = GetParentOf(Up)) {
			Charge(1);
			if(Up == Scope) {
				const BindingSyntax Part = GetBindingSyntax(Child);
				// コンストラクタ頭部の仮引数・基底の構築・委譲の位置の返戻
				if(Part == BindingSyntax::Params || Part == BindingSyntax::Initializer) return { true, true };
				// 型の初期化に属さない位置の返戻
				if(!ShouldIncludeBody || Part != BindingSyntax::ClassBody) return { false, false };
				const BindingSyntax Shape = GetBindingSyntax(Member);
				// 初期化領域か，メンバの初期化式の中かの返戻
				return {
					Shape == BindingSyntax::Initializer ||
					Shape == BindingSyntax::Decl && HasNodeWithin(GetFieldChild(Member, ChildRole::Val), Name),
					false
				};
			}
			if(GetBindingSyntax(Up) == BindingSyntax::ClassBody) Member = Child;
		}
		// 対象の型の外側の返戻
		return { false, false };
	}

	/**
	 * 最外の関数・独立した項目又は根の取得関数
	 * クロージャの捕捉先と利用集合を同じ領域で扱い，別の関数のローカル名を混同しない
	 * 計算量：通常側の再利用は O(1)，初回は所属表の追加と未取得の祖先の探索量に比例する
	 * @param Node 名前又は有効範囲
	 * @return 最外の関数又は独立した項目の範囲，何方も無なら根
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 所属表又は祖先の列を確保出来ない場合
	 * @throws std::length_error 所属表又は祖先の列の上限を超える場合
	 * @throws std::out_of_range 照会するノード又は子の識別子がノード表に無い場合
	 */
	NodeId SyntaxModel::GetBindingRegionOwner(const NodeId Node) const {
		#if defined(ShaveFormatRefFacts)
		// 最外の関数・独立した項目又は根の取得
		NodeId Owner = Root;
		for(NodeId Up = Node; Up != NoNode; Up = GetParentOf(Up)) {
			Charge(1);
			if(IsBindingFunction(Up) && !IsOuterBindingHead(Up, Node)) Owner = Up;
			if(GetBindingSyntax(Up) == BindingSyntax::ModuleDecl) {
				if(Owner == Root) Owner = Up;
				break;
			}
		}
		// 利用集合の所有者の返戻
		return Owner;
		#else
		// 空の照会は従来通り根へ解決する
		return Node == NoNode ? Root : GetBindingLocation(Node).Owner;
		#endif
	}

	/**
	 * 共有する宣言列から可視宣言と自己導入を解決する関数
	 * 計算量：初回の宣言数 D と順序の照会量 Q に対し期待値 O(D + Q)，再利用時は O(Q)
	 * @param Defs 現在の範囲の共有する宣言表
	 * @param Scope 宣言を導入する範囲
	 * @param Name 照会する名前
	 * @param Decls 同じ名前空間と綴りの宣言列
	 * @param NameIdx 比較名の番号
	 * @param Namespace 照会する名前空間
	 * @return 可視宣言と其の共有可否，自己導入の有無
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc 宣言のインデックスを確保出来ない場合
	 * @throws std::length_error 宣言のインデックスの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノードが表に無い場合
	 */
	SyntaxModel::BindingSelection SyntaxModel::SelectBindingDecl(
		const BindingScope &Defs,
		const NodeId Scope,
		const NodeId Name,
		const std::vector<BindingDecl> &Decls,
		const uint32_t NameIdx,
		const BindingNamespace Namespace
	) const {
		Charge(1);
		// 唯一の非順序付の自己導入は，可視区分の表を必要としない
		if(
			Decls.size() == 1 && Decls.front().Name == Name && !Decls.front().IsOrdered &&
			Decls.front().Visibility == BindingVisibility::Lexical
		// 同名の他の宣言が無い自己導入の返戻
		) return { &Decls.front(), nullptr, true, true, true };
		const uint64_t Key = static_cast<uint64_t>(NameIdx) << 1 | static_cast<uint64_t>(Namespace == BindingNamespace::Type);
		std::unordered_map<uint64_t, DeclIdx>::iterator Known = Defs.DeclIndexes.find(Key);
		if(Known == Defs.DeclIndexes.end()) {
			DeclIdx Built;
			for(size_t Pos = 0; Pos < Decls.size(); ++Pos) {
				Charge(1);
				const BindingDecl &Decl = Decls[Pos];
				Built.Sites.push_back(GetBindingSyntax(Decl.Site));
				Built.Introductions.emplace(Decl.Name, Pos);
				if(Decl.IsOrdered) {
					const size_t First =
					!Built.Ordered.empty() && Decls[Built.Ordered.back()].Site == Decl.Site ? Built.SiteFirst.back() : Built.Ordered.size();
					Built.Ordered.push_back(Pos);
					Built.SiteFirst.push_back(First);
				} else {
					Built.HasInit |= Decl.Visibility == BindingVisibility::Init;
					Built.HasConstructor |= Decl.Visibility == BindingVisibility::ConstructorMember;
					if(GetRules().HasOrderedBindings && Built.Sites.back() == BindingSyntax::Loop) {
						Built.HasLoop = true;
						Built.LoopBody = Decl.VisibleWithin;
					}
				}
			}
			Known = Defs.DeclIndexes.emplace(Key, std::move(Built)).first;
		}
		DeclIdx &Idx = Known->second;
		// 初期化・コンストラクタ・反復本体の有限な可視区分
		const std::pair<bool, bool> InitUse =
		Idx.HasInit || Idx.HasConstructor ? GetBindingInitUse(Scope, Name, Idx.HasInit) : std::pair<bool, bool>(false, false);
		const bool IsInit = Idx.HasInit && InitUse.first, IsConstructor = Idx.HasConstructor && InitUse.second;
		const bool IsLoopBody = Idx.HasLoop && HasNodeWithin(Idx.LoopBody, Name);
		const size_t Ctx = static_cast<size_t>(IsInit) | static_cast<size_t>(IsConstructor) << 1 | static_cast<size_t>(IsLoopBody) << 2;
		if(!Idx.Contexts[Ctx]) {
			VisibleDecls Built;
			for(size_t Pos = 0; Pos < Decls.size(); ++Pos) {
				Charge(1);
				const BindingDecl &Decl = Decls[Pos];
				if(
					Decl.IsOrdered || Decl.Visibility == BindingVisibility::Init && !IsInit ||
					GetRules().HasOrderedBindings && Idx.Sites[Pos] == BindingSyntax::Loop && !IsLoopBody
				) continue;
				if(
					Decl.Visibility == BindingVisibility::Receiver || Decl.Visibility == BindingVisibility::ConstructorMember && !IsConstructor
				) Built.Members.push_back(Pos);
				else {
					Built.Lexical.push_back(Pos);
					if(!Decl.IsFunctionScoped) Built.NonFunctionScoped.push_back(Pos);
					if(Idx.Sites[Pos] != BindingSyntax::FunctionDecl) Built.NonShadowItems.push_back(Pos);
				}
			}
			// 通常の１区分だけの照会では共有表の確保も行わない
			if(!Idx.FirstContext) {
				Idx.FirstContext = std::move(Built);
				Idx.Contexts[Ctx] = 1;
			} else {
				Defs.VisibleContexts.push_back(std::move(Built));
				Idx.Contexts[Ctx] = Defs.VisibleContexts.size() + 1;
			}
		}
		const VisibleDecls &Visible = Idx.Contexts[Ctx] == 1 ? *Idx.FirstContext : Defs.VisibleContexts[Idx.Contexts[Ctx] - 2];
		const std::unordered_map<NodeId, size_t>::const_iterator Introduction = Idx.Introductions.find(Name);
		const size_t Self = Introduction == Idx.Introductions.end() ? SIZE_MAX : Introduction->second;
		BindingSelection Selected;
		Selected.IsDecl = Self != SIZE_MAX;
		// 宣言列の中の位置に達する要素数の取得
		const auto PrefixCount = [&](const std::vector<size_t> &Positions, const size_t Limit) -> size_t {
			// 二分探索での境界の返戻
			return WorkSort::LowerBound(
				std::span<const size_t>(Positions),
				Limit,
				[this](const size_t Pos, const size_t Boundary) -> bool {
					Charge(1);
					// 境界より前の位置である事の返戻
					return Pos < Boundary;
				}
			);
		};
		if(Selected.IsDecl && GetRules().HasOrderedBindingShadow) {
			Selected.Visible = &Decls[Self];
			const size_t Members = PrefixCount(Visible.Members, Self);
			if(Members) Selected.Member = &Decls[Visible.Members.front()];
			Selected.IsSharedMember = Members < 2;
			// 自己導入の手前迄の受手と，自己導入の優先の返戻
			return Selected;
		}
		// 初期化中の可視性を含む，現在位置より前の順序付宣言の境界
		const size_t OrderedCount = WorkSort::LowerBound(
			std::span<const size_t>(Idx.Ordered),
			0,
			[&](const size_t Pos, const int) -> bool {
				Charge(1);
				const BindingDecl &Decl = Decls[Pos];
				// 自身の初期値を除き，既に有効な導入である事の返戻
				return (GetRules().HasBindingInInitializer || !HasNodeWithin(Decl.Site, Name)) &&
				IsBefore(GetRules().HasBindingInInitializer ? Decl.Name : Decl.Site, Name);
			}
		);
		const size_t SelfPos = PrefixCount(Visible.Lexical, Self);
		const bool HasLexicalSelf = SelfPos < Visible.Lexical.size() && Visible.Lexical[SelfPos] == Self;
		const size_t MemberSelfPos = PrefixCount(Visible.Members, Self);
		const bool HasMemberSelf = MemberSelfPos < Visible.Members.size() && Visible.Members[MemberSelfPos] == Self;
		const size_t Count = Visible.Lexical.size() + OrderedCount + static_cast<size_t>(Selected.IsDecl && !HasLexicalSelf);
		size_t First = Visible.Lexical.empty() ? SIZE_MAX : Visible.Lexical.front();
		if(OrderedCount) First = std::min(First, Idx.Ordered.front());
		if(Selected.IsDecl) First = std::min(First, Self);
		if(Count) {
			Selected.Visible = &Decls[First];
			Selected.IsShared = Count < 2 ||
			Visible.NonFunctionScoped.empty() && !OrderedCount && (!Selected.IsDecl || HasLexicalSelf || Decls[Self].IsFunctionScoped);
			if(
				GetRules().HasOrderedBindingShadow && OrderedCount &&
				(Decls[First].IsOrdered || Idx.Sites[First] == BindingSyntax::FunctionDecl)
			) {
				const size_t FirstOrdered = Idx.Ordered.front();
				Selected.Visible = &Decls[Idx.Ordered[Idx.SiteFirst[OrderedCount - 1]]];
				const size_t BeforeOrdered = PrefixCount(Visible.Lexical, FirstOrdered);
				Selected.IsShared = (BeforeOrdered < 2 || !PrefixCount(Visible.NonFunctionScoped, FirstOrdered)) &&
				(Visible.NonShadowItems.empty() || Visible.NonShadowItems.back() < FirstOrdered);
			}
		}
		const size_t MemberCount = Visible.Members.size() - static_cast<size_t>(HasMemberSelf);
		if(MemberCount) Selected.Member = &Decls[Visible.Members[static_cast<size_t>(HasMemberSelf && !MemberSelfPos)]];
		Selected.IsSharedMember = MemberCount < 2;
		// 同じ名前の可視宣言と共有可否の返戻
		return Selected;
	}

	/**
	 * 利用集合を作らず名前の宣言だけを解決する関数
	 * 頭部の導入と可視性は，シグネチャと参照・書込の収集で同じ処理を使う
	 * 計算量：未収集の宣言表の構築量 S と有効範囲の探索量 Q に対し期待値 O(S + Q)
	 * @param Name 解決する名前
	 * @param InitialScope 名前の最内の有効範囲
	 * @param Scopes 独立した基準側の照会内の宣言表（通常側は無）
	 * @param UncertainInitializers 先行する未確定な初期化の導入名（利用集合を作らなければ無）
	 * @param IsHead 利用の判定を必要としない頭部の導入かの格納先
	 * @return 宣言の同一性・可視性・名前空間の解決結果
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc 宣言表又は作業列を確保出来ない場合
	 * @throws std::length_error 宣言表又は作業列の要素数上限を超える場合
	 * @throws std::out_of_range 照会するノードが表に無い場合
	 */
	BindingFact SyntaxModel::ResolveBindingName(
		const NodeId Name,
		const NodeId InitialScope,
		std::unordered_map<NodeId, BindingScope> *const Scopes,
		std::unordered_set<NodeId> *const UncertainInitializers,
		bool &IsHead
	) const {
		IsHead = false;
		// 照会の中で共有する宣言表の取得
		const auto ScopeOf = [&](const NodeId Scope) -> const BindingScope & {
			Charge(1);
			#if defined(ShaveFormatRefFacts)
			if(const std::unordered_map<NodeId, BindingScope>::const_iterator Found = Scopes->find(Scope); Found != Scopes->end()) {
				// 同じ独立した照会内の宣言表の返戻
				return Found->second;
			}
			// 同じ独立した照会の宣言表の返戻
			return Scopes->emplace(Scope, BuildBindingScope(Scope)).first->second;
			#else
			static_cast<void>(Scopes);
			// モデルで共有する宣言表の返戻
			return GetBindingScopeFacts(Scope);
			#endif
		};
		BindingFact Fact;
		Fact.Namespace = GetBindingNamespace(Name);
		if(!GetNode(Name).Facts.NameIdx) {
			Fact.IsUncertain = true;
			// 比較名が無い候補の未確定な解決の返戻
			return Fact;
		}
		const NodeId Parent = GetParentOf(Name);
		const BindingSyntax ParentShape = GetBindingSyntax(Parent);
		NodeId Object = GetFieldChild(Parent, ChildRole::ObjectField);
		if(Object == NoNode) Object = GetNamedChild(Parent, 0);
		const bool IsReceiverUse = ParentShape == BindingSyntax::NamedMember && Object != Name && Is(Object, Role::ThisRef);
		// 型の頭部で供給する導入名の同一性は保ち，メンバ本体へ仮引数を導入しない
		if(ParentShape == BindingSyntax::Param && GetFieldChild(Parent, ChildRole::Name) == Name) {
			const NodeId Scope = InitialScope;
			if(
				const BindingSyntax ScopeShape = GetBindingSyntax(Scope);
				(ScopeShape == BindingSyntax::ClassDecl || ScopeShape == BindingSyntax::ClassExpr) &&
				GetFieldChild(Scope, ChildRole::Params) == GetParentOf(Parent)
			) {
				Fact.Decl = Name;
				Fact.Scope = Scope;
				Fact.IsDecl = true;
				ScopeOf(Scope);
				IsHead = true;
				// 頭部の自己導入の返戻
				return Fact;
			}
		}
		if(
			ParentShape == BindingSyntax::FunctionDecl || ParentShape == BindingSyntax::NamedExpr ||
			ParentShape == BindingSyntax::ClassDecl || ParentShape == BindingSyntax::ClassExpr || ParentShape == BindingSyntax::ModuleDecl
		) {
			NodeId Head = GetFieldChild(Parent, ChildRole::Name);
			if(Head == NoNode) for(const NodeId Child : GetChildrenOf(Parent)) if(GetBindingSyntax(Child) == BindingSyntax::Name) {
				Head = Child;
				break;
			}
			if(Head == Name) {
				Fact.Decl = Name;
				const bool IsLocalName = ParentShape == BindingSyntax::NamedExpr || ParentShape == BindingSyntax::ClassExpr;
				Fact.Scope = IsLocalName ? Parent : GetBindingScope(GetParentOf(Parent));
				ScopeOf(Fact.Scope);
				Fact.IsDecl = true;
				IsHead = true;
				// 頭部の自己導入の返戻
				return Fact;
			}
		}
		const uint32_t NameIdx = GetCmpNameIdx(Name);
		// 受手の候補を控え，外側のローカル名を最後迄先に探す
		BindingFact Receiver;
		NodeId UnknownReceiver = NoNode;
		for(NodeId Scope = InitialScope; Scope != NoNode; Scope = GetOuterDeclarationScope(Scope)) {
			Charge(1);
			if(IsReceiverUse && GetBindingSyntax(Scope) != BindingSyntax::ClassBody) continue;
			const BindingScope &Defs = ScopeOf(Scope);
			const bool IsReceiverScope = GetRules().HasLocalBindingPriority && GetBindingSyntax(Scope) == BindingSyntax::ClassBody;
			if(!IsReceiverScope && (!GetRules().HasLocalBindingPriority || Scope != Root)) Fact.IsUncertain |= Defs.HasUncertainNames;
			const std::unordered_map<uint32_t, std::vector<BindingDecl>> &NamesInSpace =
			Fact.Namespace == BindingNamespace::Val ? Defs.Names : Defs.TypeNames;
			if(
				const std::unordered_map<uint32_t, std::vector<BindingDecl>>::const_iterator Found = NamesInSpace.find(NameIdx);
				Found != NamesInSpace.end()
			) {
				const BindingSelection Selection = SelectBindingDecl(Defs, Scope, Name, Found->second, NameIdx, Fact.Namespace);
				const BindingDecl *const Visible = Selection.Visible, *const Member = Selection.Member;
				const bool IsShared = Selection.IsShared, IsSharedMember = Selection.IsSharedMember;
				Fact.IsDecl = Selection.IsDecl;
				if(!Visible && Fact.IsUncertain) for(const BindingDecl &Decl : Found->second) {
					Charge(1);
					if(UncertainInitializers && Decl.IsOrdered && HasNodeWithin(Decl.Site, Name)) UncertainInitializers->insert(Decl.Name);
				}
				if(Member && Receiver.Decl == NoNode) {
					Receiver.Decl = Member->Name;
					Receiver.IsDeclaredConstant = Member->IsConstant;
					Receiver.Scope = Scope;
					// 同じ型の本体の受手は，頭部が供給する既知のメンバを曖昧にしない
					Receiver.IsUncertain = !IsSharedMember || Defs.HasUncertainNames || UnknownReceiver != NoNode &&
					(GetBindingSyntax(UnknownReceiver) != BindingSyntax::ClassBody || GetParentOf(UnknownReceiver) != Scope);
				}
				if(Visible) {
					// 最上位の名前はローカル名ではなく，暗黙の受手の探索より後の候補
					if(GetRules().HasLocalBindingPriority && Scope == Root && !Fact.IsDecl && Receiver.Decl != NoNode) break;
					Fact.Decl = Visible->Name;
					Fact.IsDeclaredConstant = Visible->IsConstant;
					Fact.Scope = Scope;
					Fact.IsUncertain = Fact.IsUncertain || !IsShared || Defs.HasUncertainNames ||
					GetRules().HasLocalBindingPriority && Scope == Root && !Fact.IsDecl && UnknownReceiver != NoNode;
					if(Visible->IsFunctionScoped && IsBindingFunction(GetParentOf(Scope))) {
						const NodeId Function = GetParentOf(Scope);
						const BindingScope &Params = ScopeOf(Function);
						if(
							const std::unordered_map<uint32_t, std::vector<BindingDecl>>::const_iterator Param = Params.Names.find(NameIdx);
							!Params.HasSeparateParams && Param != Params.Names.end() && Param->second.size() == 1
						) {
							Fact.Decl = Param->second.front().Name;
							Fact.Scope = Function;
						}
					}
					break;
				}
			}
			if(!GetRules().HasLocalBindingPriority) Fact.IsUncertain |= Defs.HasUnknownReceiver;
			else if(UnknownReceiver == NoNode && (Defs.HasUnknownReceiver || IsReceiverScope && Defs.HasUncertainNames)) {
				UnknownReceiver = Scope;
			}
			Fact.IsUncertain = Fact.IsUncertain || Defs.HasImplicitParam && GetCmpName(Name) == GetRules().ImplicitBindingParam;
			if(IsReceiverUse) break;
		}
		if(Fact.Decl == NoNode && Receiver.Decl != NoNode) {
			Fact.Decl = Receiver.Decl;
			Fact.Scope = Receiver.Scope;
			Fact.IsDeclaredConstant = Receiver.IsDeclaredConstant;
			Fact.IsUncertain |= Receiver.IsUncertain;
		}
		// 初期化中の未確定な同名の参照を持つ値は，後続の参照でも使用回数を確定しない
		Fact.IsUncertain =
		Fact.IsUncertain || Fact.Decl == NoNode || UncertainInitializers && UncertainInitializers->contains(Fact.Decl);
		// 利用集合から独立した宣言の解決の返戻
		return Fact;
	}

	/**
	 * 宣言探索の外側の範囲の取得関数
	 * モジュールの項目は外側のモジュールを起点とし，其の他は最内の有効範囲を使う
	 * 計算量：モジュールの祖先の探索量 H と通常の所属の取得量 Q に対し O(H + Q)
	 * @param Scope 現在の有効範囲
	 * @return 外側の探索起点（根の外側は無）
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc 所属の作業列を確保出来ない場合
	 * @throws std::length_error 所属の作業列の要素数上限を超える場合
	 * @throws std::out_of_range 照会するノードが表に無い場合
	 */
	NodeId SyntaxModel::GetOuterDeclarationScope(const NodeId Scope) const {
		// 根の外側の返戻
		if(Scope == Root) return NoNode;
		// 通常の範囲が継承する外側の返戻
		if(GetBindingSyntax(Scope) != BindingSyntax::ModuleDecl) return GetBindingScope(GetParentOf(Scope));
		#if !defined(ShaveFormatRefFacts)
		// 所属表で共有する外側のモジュールの返戻
		return GetBindingLocation(GetParentOf(Scope)).Module;
		#else
		for(NodeId Up = GetParentOf(Scope); Up != NoNode; Up = GetParentOf(Up)) {
			Charge(1);
			// 最内のモジュール又は根の返戻
			if(Up == Root || GetBindingSyntax(Up) == BindingSyntax::ModuleDecl) return Up;
		}
		// 接続先が無い場合の返戻
		return NoNode;
		#endif
	}

	/**
	 * 現在の構造から参照と書込を解決する関数
	 * 所属関数と最内範囲は収集時に引き継ぎ，外側の範囲との関係も共有する
	 * 計算量：領域ノード数 N，範囲・パターン等の総走査量 S，有効範囲・祖先の総探索量 A，未収集の宣言列の総長 D，照会する最大宣言数 M，名前数 K に対し期待値 O(N + S + A + D + K log(1 + M))
	 * @param Owner 最外の関数・独立した項目又は根
	 * @return 名前毎の束縛と再代入集合
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 範囲インデックス・依存・束縛事実・書込・参照又は走査のメモリを確保出来ない場合
	 * @throws std::length_error 走査・宣言又は関数列の要素数上限を超える場合
	 * @throws std::out_of_range ノードがノード表に無い場合，又は所属範囲が収集済の対応表に無い場合
	 */
	SyntaxModel::BindingRegion SyntaxModel::BuildBindingRegion(const NodeId Owner) const {
		// 現在の構造から参照と書込を解決
		BindingRegion Result;
		std::unordered_map<NodeId, BindingScope> *ScopeCache = nullptr;
		#if defined(ShaveFormatRefFacts)
		std::unordered_map<NodeId, BindingScope> CurScopes;
		ScopeCache = &CurScopes;
		#endif
		// 収集の走査で所属する関数と範囲を引き継ぎ，名前毎の所属を記録する
		struct Context {
			NodeId Scope;
			NodeId Function;
			NodeId PhysicalFunction;
		};
		struct NameUse {
			NodeId Node;
			NodeId Scope;
			NodeId Function;
		};
		struct PendingNode {
			NodeId Node;
			Context Parent;
		};
		struct ScopeContext {
			NodeId Function; // 捕捉を判定する所属関数
		};
		std::unordered_map<NodeId, ScopeContext> ScopeContexts;
		// 捕捉の判定に使う物理的な所属関数を１度記録する
		const auto Enter = [&](Context Parent, const NodeId Node, const BindingSyntax Shape) -> Context {
			Charge(1);
			if(IsBindingFunction(Shape)) Parent.Function = Parent.PhysicalFunction = Node;
			if(Node == Root || IsBindingScope(Shape)) {
				ScopeContexts.emplace(Node, ScopeContext{ Parent.PhysicalFunction });
				Parent.Scope = Node;
			}
			// 現在のノードを含む文脈の返戻
			return Parent;
		};
		// 計算されたメンバ名は，関数の仮引数の導入前の外側で評価する
		const auto ForChild = [&](const Context &Parent, Context Current, const NodeId Name, const NodeId Child) -> Context {
			if(Name != NoNode && Name == Child) {
				Current.Scope = Parent.Scope;
				Current.Function = Parent.Function;
			}
			if(const NodeId Outer = GetOuterBindingScope(GetParentOf(Child), Child); Outer != NoNode) {
				Current.Scope = Outer;
				Current.Function = ScopeContexts.at(Outer).Function;
			}
			// 子へ引き継ぐ文脈の返戻
			return Current;
		};
		std::vector<NodeId> Ancestors;
		for(NodeId Up = GetParentOf(Owner); Up != NoNode; Up = GetParentOf(Up)) {
			Charge(1);
			Ancestors.push_back(Up);
		}
		#if !defined(ShaveFormatRefFacts)
		Charge(Ancestors.size() + 1);
		Result.Dependencies = Ancestors;
		Result.Dependencies.push_back(Owner);
		#endif
		Context Initial { Root, NoNode, NoNode };
		for(size_t Idx = Ancestors.size(); Idx; --Idx) {
			const NodeId Node = Ancestors[Idx - 1], Child = Idx > 1 ? Ancestors[Idx - 2] : Owner;
			const BindingSyntax Shape = GetBindingSyntax(Node);
			const NodeId Name = Shape == BindingSyntax::MemberFunction ? GetFieldChild(Node, ChildRole::Name) : NoNode;
			Initial = ForChild(Initial, Enter(Initial, Node, Shape), Name, Child);
		}
		std::vector<NameUse> Names;
		std::unordered_set<NodeId> Writes;
		for(std::vector<PendingNode> Pending { { Owner, Initial } }; !Pending.empty();) {
			const PendingNode Cur = Pending.back();
			const NodeId Node = Cur.Node;
			Pending.pop_back();
			const BindingSyntax Shape = GetBindingSyntax(Node);
			const Context Current = Enter(Cur.Parent, Node, Shape);
			const NodeId MemberName = Shape == BindingSyntax::MemberFunction ? GetFieldChild(Node, ChildRole::Name) : NoNode;
			Result.IsUncertain |= Is(Node, Role::Error);
			if(Shape == BindingSyntax::Ignore || Shape == BindingSyntax::MemberPart) continue;
			if(Owner == Root && Node != Root && IsBindingFunction(Shape)) {
				Result.Functions.push_back(Node);
				if(MemberName != NoNode) Pending.push_back({ MemberName, ForChild(Cur.Parent, Current, MemberName, MemberName) });
				continue;
			}
			if(Shape == BindingSyntax::Name) Names.push_back({ Node, Current.Scope, Current.Function });
			if(NodeId Target = GetBindingWriteTarget(Node, Shape); Target != NoNode) {
				while(GetBindingSyntax(Target) == BindingSyntax::RestPattern && GetNamedChildCount(Target) == 1) {
					Charge(1);
					Target = GetNamedChild(Target, 0);
				}
				bool IsUncertain = false;
				const std::vector<NodeId> Targets = GetBindingWriteNames(Target, IsUncertain);
				Writes.insert(Targets.begin(), Targets.end());
				Result.IsUncertain |= IsUncertain;
			}
			const std::span<const NodeId> Children = GetChildrenOf(Node);
			for(std::span<const NodeId>::reverse_iterator Child = Children.rbegin(); Child != Children.rend(); ++Child) {
				Pending.push_back({ *Child, ForChild(Cur.Parent, Current, MemberName, *Child) });
			}
		}
		std::unordered_set<NodeId> UncertainInitializers;
		for(const NameUse &Use : Names) {
			const NodeId Name = Use.Node;
			bool IsHead = false;
			BindingFact Fact = ResolveBindingName(Name, Use.Scope, ScopeCache, &UncertainInitializers, IsHead);
			Fact.IsWrite = Writes.contains(Name);
			if(IsHead || !GetNode(Name).Facts.NameIdx) {
				Result.IsUncertain |= Fact.IsUncertain;
				Result.Facts.emplace(Name, Fact);
				continue;
			}
			const NodeId UseFunction = Use.Function;
			const NodeId DeclaringFunction = Fact.Scope == NoNode ? NoNode : ScopeContexts.at(Fact.Scope).Function;
			const NodeId Introduction = GetParentOf(Fact.Decl);
			const bool IsStaticItem = Introduction != NoNode && GetNode(Introduction).Kind.Def->Bindings.IsStaticItem;
			// 可変のクロージャの呼出と，変更不能な項目・コンストラクタの呼出を区別する
			Fact.IsWrite = Fact.IsWrite && !Fact.IsDecl && !IsStaticItem;
			Fact.IsCapture = !IsStaticItem && !Fact.IsDecl && Fact.Decl != NoNode && UseFunction != DeclaringFunction;
			// 未解決の綴りは事実に残し，探索で区別した別のローカル名へ書込を帰属させない
			if(Fact.Namespace == BindingNamespace::Val) {
				if(Fact.IsWrite && Fact.Decl != NoNode) Result.Writes.insert(Fact.Decl);
				if(!Fact.IsDecl && Fact.Decl != NoNode) Result.Refs.insert(Fact.Decl);
			}
			Result.Facts.emplace(Name, Fact);
		}
		// 現在の参照・捕捉・書込の返戻
		return Result;
	}

	/**
	 * 束縛に作用する書換迄保持する利用集合の取得関数
	 * 通常側は保持済の集合を再利用し，基準側は各呼出で再収集する
	 * 計算量：必要な領域の収集量 R に対し期待値 O(1 + R)
	 * @param Owner 利用集合の所有者
	 * @return 束縛に作用する書換迄有効な集合
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 利用集合の再収集又は領域表の登録のメモリを確保出来ない場合
	 * @throws std::length_error 再収集する名前の文字数又は走査・宣言・関数列の要素数上限を超える場合
	 * @throws std::out_of_range ノードがノード表に無い場合，又は所属範囲が収集済の対応表に無い場合
	 */
	const SyntaxModel::BindingRegion &SyntaxModel::GetBindingRegion(const NodeId Owner) const {
		// 束縛に作用する書換迄保持する利用集合の取得
		Charge(1);
		#if defined(ShaveFormatRefFacts)
		// 呼出中に取得済の参照を保つ基準側の利用集合（テスト専用の構築だけが持つ）
		const std::shared_ptr<const BindingRegion> Result = std::make_shared<const BindingRegion>(BuildBindingRegion(Owner));
		RefFacts.push_back(Result);
		// 取得済の参照を保つ照会結果の返戻
		return *Result;
		#else
		const std::unordered_map<NodeId, BindingRegion>::const_iterator Found = BindingRegions.find(Owner);
		// 保持して居る利用集合の返戻
		if(Found != BindingRegions.end()) return Found->second;
		// 現在構造から収集した利用集合の返戻
		const BindingRegion &Built = BindingRegions.emplace(Owner, BuildBindingRegion(Owner)).first->second;
		for(const NodeId Scope : Built.Dependencies) {
			Charge(1);
			BindingReaders[Scope].insert(Owner);
		}
		// 依存を登録した利用集合の返戻
		return Built;
		#endif
	}

	/**
	 * 名前を解決する領域と其の所有者の取得関数
	 * 計算量：所有者の取得費用 Q と領域の再収集量 R に対し期待値 O(Q + R)，通常側の再利用時は R = 0
	 * @param Name 名前のノード（無又は未存在も可）
	 * @return 名前を調べる所有者と領域，対象外では `NoNode` と `nullptr`
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 範囲又は利用集合の取得のメモリを確保出来ない場合
	 * @throws std::length_error 利用集合の再収集で名前又は作業列の上限を超える場合
	 * @throws std::out_of_range ノードがノード表に無い場合，又は所属範囲が収集済の対応表に無い場合
	 */
	std::pair<NodeId, const SyntaxModel::BindingRegion *> SyntaxModel::GetBindingRegionForName(const NodeId Name) const {
		// 名前を解決する領域と其の所有者の取得
		// 値の名前以外の領域の返戻
		if(Name == NoNode || Name >= Nodes.size() || IsRemoved[Name] || GetBindingSyntax(Name) != BindingSyntax::Name) {
			// 名前を解決しない空の領域の返戻
			return { NoNode, nullptr };
		}
		const NodeId Owner = GetBindingRegionOwner(Name);
		// 現在の名前を解決する領域の返戻
		return { Owner, &GetBindingRegion(Owner) };
	}

	/**
	 * 最上位の束縛を捕捉する全関数の作用の集約関数
	 * 基準側は各照会が所有する新しい領域へ集約し，書換を跨いで利用しない
	 * 計算量：初回は関数数 F と作用数 U と領域収集量 Q に対し O(F + U + Q)，再利用時は O(1)
	 * @param Region 現在の最上位の領域
	 * @param KnownOwner 名前の照会で取得済の所有者
	 * @param KnownRegion 取得済の領域
	 * @return 各導入名への作用と未確定性を纏めた集合
	 * @throws FormatLimitExceeded 収集が作業予算を超える場合
	 * @throws std::bad_alloc 集合のメモリを確保出来ない場合
	 * @throws std::length_error 集合の要素数上限を超える場合
	 * @throws std::out_of_range ノードがノード表に無い場合，又は所属範囲が収集済の対応表に無い場合
	 */
	const SyntaxModel::BindingUses &SyntaxModel::GetBindingCaptures(
		const BindingRegion &Region,
		const NodeId KnownOwner,
		const BindingRegion &KnownRegion
	) const {
		// 最上位の束縛を捕捉する全関数の作用の集約
		if(!Region.Captures) {
			BindingUses Captures;
			for(const NodeId Function : Region.Functions) {
				Charge(1);
				const BindingRegion &Uses = Function == KnownOwner ? KnownRegion : GetBindingRegion(Function);
				Captures.IsUncertain |= Uses.IsUncertain;
				for(const NodeId Write : Uses.Writes) {
					Charge(1);
					Captures.Writes.insert(Write);
				}
				for(const NodeId Ref : Uses.Refs) {
					Charge(1);
					Captures.Refs.insert(Ref);
				}
			}
			Region.Captures.emplace(std::move(Captures));
		}
		// 最上位の照会が共有する集合の返戻
		return *Region.Captures;
	}

	/**
	 * 取得済の領域を共有して束縛への作用を判定する関数
	 * 計算量：所有者の取得の総費用 Q，必要な領域の再収集量 R と未集約の捕捉集合の収集量 C に対し期待値 O(Q + R + C)
	 * @param Name 導入名又は利用名のノード
	 * @param IsWrite 再代入を調べる場合は `true`，利用を調べる場合は `false`
	 * @return 指定した作用又は未確定な作用が在れば `true`
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 範囲又は利用集合の取得のメモリを確保出来ない場合
	 * @throws std::length_error 利用集合の再収集で名前又は作業列の上限を超える場合
	 * @throws std::out_of_range ノードがノード表に無い場合，又は所属範囲が収集済の対応表に無い場合
	 */
	bool SyntaxModel::HasBindingUse(const NodeId Name, const bool IsWrite) const {
		// 取得済の領域を共有して束縛への作用を判定
		const std::pair<NodeId, const BindingRegion *> Cur = GetBindingRegionForName(Name);
		// 束縛を確定出来ない場合の返戻
		if(!Cur.second) return true;
		const std::unordered_map<NodeId, BindingFact>::const_iterator Found = Cur.second->Facts.find(Name);
		// 未確認の束縛の作用の返戻
		if(Found == Cur.second->Facts.end() || Found->second.Decl == NoNode || Found->second.IsUncertain) return true;
		const BindingFact &Binding = Found->second;
		// 束縛の利用集合に対象を含むかの判定
		const auto HasUse = [&](const BindingRegion &Region) -> bool {
			// 指定した作用又は未確定な作用の有無の返戻
			return Region.IsUncertain || (IsWrite ? Region.Writes : Region.Refs).contains(Binding.Decl);
		};
		const NodeId Owner = GetBindingRegionOwner(Binding.Scope);
		const BindingRegion &Region = Owner == Cur.first ? *Cur.second : GetBindingRegion(Owner);
		// 宣言を所有する領域の作用の返戻
		if(HasUse(Region)) return true;
		if(Owner == Root) {
			const BindingUses &Captures = GetBindingCaptures(Region, Cur.first, *Cur.second);
			// 全関数から捕捉する作用の返戻
			return Captures.IsUncertain || (IsWrite ? Captures.Writes : Captures.Refs).contains(Binding.Decl);
		}
		// 作用が無い事の返戻
		return false;
	}

	/**
	 * 現在の束縛の導入位置から値の供給元を取得する関数
	 * `Binding` は同じ構文から直前に取得した事実とし，編集を挟んだ事実を渡さない
	 * 未確定の導入では初期値・仮引数・非定数性の何れも証明しない
	 * 計算量：祖先の総段数 H，調べた子の総数 C，修飾子の判定費用 M に対し O(H + C + M)
	 * @param Binding 現在の構造で解決した束縛（未解決又は未確定も可）
	 * @return 初期値のノード，仮引数かの判定，供給規則で確定する非定数性
	 * @throws FormatLimitExceeded モデルの作業予算を超えた場合
	 * @throws std::out_of_range 束縛の範囲又は子の識別子が構文表に無い場合
	 */
	BindingValSrc SyntaxModel::GetBindingValSrc(const BindingFact &Binding) const {
		// 値の供給を確定出来ない束縛の返戻
		if(Binding.Decl == NoNode || Binding.IsUncertain) return {};
		NodeId Child = Binding.Decl;
		for(NodeId Owner = GetParentOf(Child); Owner != NoNode; Owner = GetParentOf(Owner)) {
			Charge(1);
			const BindingSyntax Shape = GetBindingSyntax(Owner);
			switch(Shape) {
			case BindingSyntax::Param:
			case BindingSyntax::ConstructorParam:
				return { NoNode, true, true };
			case BindingSyntax::Declarator:
				{
					const NodeId Decl = GetParentOf(Owner), Val = GetFieldChild(Owner, ChildRole::Val);
					const KindDef *const Def = Decl == NoNode ? nullptr : GetNode(Decl).Kind.Def;
					const bool IsNonconstant = Def && Def->Bindings.ShouldRequireImmutableConstantSrc &&
					(Val == NoNode || !HasWordChild(GetFirstNamedChildWithRole(Decl, Role::ModifierSeq), Word::ImmutableBinding));
					return { Val, false, IsNonconstant };
				}
			case BindingSyntax::Params:
				if(Binding.Scope != NoNode) {
					Charge(1);
					// 関数の仮引数供給でない並びの返戻
					if(!IsBindingFunction(Binding.Scope)) return {};
					NodeId ParamOwner = NoNode;
					const bool IsParam = HasNodeWithin(GetBindingParams(Binding.Scope, ParamOwner), Binding.Decl);
					// 仮引数の供給元に含まれる導入名かの返戻
					return { NoNode, IsParam, IsParam };
				}
				break;
			}
			if(Owner == Binding.Scope && IsBindingFunction(Owner) && GetFieldChild(Owner, ChildRole::Params) == Child) {
				// ラッパーを除いた導入名も，現在の仮引数のフィールドに限って供給を認識する事の返戻
				return { NoNode, true, true };
			}
			// 名前のパターン以外の構造を仮引数の導入と推測しない事の返戻
			if(Shape != BindingSyntax::NamePattern) return {};
			Child = Owner;
		}
		// 初期値又は仮引数の供給を持たない導入の返戻
		return {};
	}

	/**
	 * 名前の導入・名前空間・未確定性だけの取得関数
	 * 仮引数の導入は共有する宣言表で解決し，参照・書込・捕捉の利用集合を作らない
	 * 仮引数以外又は他の導入を参照する候補は，利用集合の初期化の判定を保つ
	 * 計算量：宣言表の初回の収集量 S と宣言の探索量 Q に対し期待値 O(S + Q)
	 * @param Name 名前の候補（無又は未存在も可）
	 * @return 導入・名前空間・未確定性の専用の事実
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc 宣言表又は作業列を確保出来ない場合
	 * @throws std::length_error 宣言表又は作業列の要素数上限を超える場合
	 * @throws std::out_of_range 照会するノードが表に無い場合
	 */
	BindingIntroduction SyntaxModel::GetBindingIntroduction(const NodeId Name) const {
		// 束縛の名前以外の空の判定の返戻
		if(Name == NoNode || Name >= Nodes.size() || IsRemoved[Name] || GetBindingSyntax(Name) != BindingSyntax::Name) return {};
		const NodeId Scope = GetBindingScope(Name);
		NodeId Owner = NoNode;
		if(IsBindingFunction(Scope)) {
			const NodeId Params = GetBindingParams(Scope, Owner);
			if(Params != NoNode && HasNodeWithin(Params, Name)) {
				std::unordered_map<NodeId, BindingScope> *ScopeCache = nullptr;
				#if defined(ShaveFormatRefFacts)
				std::unordered_map<NodeId, BindingScope> Scopes;
				ScopeCache = &Scopes;
				#endif
				bool IsHead = false;
				const BindingFact Fact = ResolveBindingName(Name, Scope, ScopeCache, nullptr, IsHead);
				// 自己導入又は未解決の名前は，利用集合の影響を受けない判定の返戻
				if(Fact.IsDecl || Fact.Decl == NoNode) return { Fact.IsDecl, Fact.Namespace, Fact.IsUncertain };
			}
		}
		const BindingFact Fact = GetBindingFact(Name);
		// 仮引数以外又は既存の導入を参照する候補の判定の返戻
		return { Fact.IsDecl, Fact.Namespace, Fact.IsUncertain };
	}

	/**
	 * 現在の構造の束縛の取得関数
	 * 通常側では束縛に作用する書換迄利用集合を保持する
	 * 計算量：所有者の取得費用 Q と領域の再収集量 R に対し期待値 O(Q + R)，通常側の再利用時は R = 0
	 * @param Name 名前のノード（無又は未存在も可）
	 * @return 導入名を同一性とする束縛と利用の事実
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 範囲又は利用集合の取得のメモリを確保出来ない場合
	 * @throws std::length_error 利用集合の再収集で名前又は作業列の上限を超える場合
	 * @throws std::out_of_range ノードがノード表に無い場合，又は所属範囲が収集済の対応表に無い場合
	 */
	BindingFact SyntaxModel::GetBindingFact(const NodeId Name) const {
		// 現在の構造の束縛の取得
		const std::pair<NodeId, const BindingRegion *> Cur = GetBindingRegionForName(Name);
		// 対象外の構造の返戻
		if(!Cur.second) return {};
		const std::unordered_map<NodeId, BindingFact>::const_iterator Found = Cur.second->Facts.find(Name);
		// 利用集合に記録の無い名前も，未確認を作用無にしない事実の返戻
		return Found == Cur.second->Facts.end() ? BindingFact{ NoNode, NoNode, false, false, false, true } : Found->second;
	}

	/**
	 * 使用する名前の可視位置のインデックスを共有する取得関数
	 * 位置を読まない遮蔽の候補には，インポート範囲と修飾毎の表を作らない
	 * 計算量：同名の導入数 D に対し初回期待値 O(D)，再照会は期待値 O(1)
	 * @param Index 有効範囲で共有する導入のインデックス
	 * @param NameIdx 比較用の名前の番号
	 * @return インポート範囲と修飾毎の宣言位置
	 * @throws FormatLimitExceeded 導入の走査が作業予算を超える場合
	 * @throws std::bad_alloc 宣言位置の表又は列を確保出来ない場合
	 * @throws std::length_error 宣言位置の表又は列の要素数上限を超える場合
	 * @throws std::out_of_range 名前の番号が収集済の表に無い場合
	 */
	const CallableImports &SyntaxModel::GetBindingSourcePositions(const CallableIndex &Index, const uint32_t NameIdx) const {
		const SyntaxModel &Syntax = *this;
		// 収集済の名前の位置の返戻
		if(
			const std::unordered_map<uint32_t, CallableImports>::const_iterator Found = Index.Positions.find(NameIdx);
			Found != Index.Positions.end()
		) return Found->second;
		const std::vector<CallableIntroduction> &Decls = Index.Names.at(NameIdx);
		CallableImports Imports;
		for(size_t Pos = 0; Pos < Decls.size(); ++Pos) {
			Syntax.Charge(1);
			const CallableIntroduction &Decl = Decls[Pos];
			const NodeId Import = Syntax.GetRules().HasNamespaceFunctionImports ? Syntax.GetNode(Decl.Name).Facts.ImportScope : NoNode;
			CallablePositions &Positions = Imports[Import][Decl.Member];
			if(Decl.IsSequential) {
				Positions.Sequential.push_back(
					{ Pos, (Positions.Sequential.empty() ? 0 : Positions.Sequential.back().CustomCount) + Decl.IsCustomDefinition }
				);
				continue;
			}
			// 同じ名前ノードの重複した登録は，元の先頭の導入を選ぶ
			if(!Positions.Ordered.empty() && Decls[Positions.Ordered.back().Decl].Name == Decl.Name) continue;
			Positions.Ordered.push_back(
				{ Pos, (Positions.Ordered.empty() ? 0 : Positions.Ordered.back().CustomCount) + Decl.IsCustomDefinition }
			);
			if(Decl.IsScopeWide) {
				Positions.Wide.push_back({ Pos, (Positions.Wide.empty() ? 0 : Positions.Wide.back().CustomCount) + Decl.IsCustomDefinition });
			}
		}
		// 途中の失敗では共有表を変更せず，完成した位置の表の返戻
		return Index.Positions.emplace(NameIdx, std::move(Imports)).first->second;
	}

	/**
	 * 同じ名前の宣言位置から可視の最後の導入と独自定義を求める関数
	 * 計算量：同名宣言数 D に対し，同じ宣言間では各列で最大２回，区間が変われば O(log(D + 1)) 回の位置比較とテキスト照合の費用
	 * @param Index 有効範囲で共有する導入のインデックス
	 * @param Scope 照会する有効範囲
	 * @param Head 使用位置の先頭名
	 * @param NameIdx 比較用の名前の番号
	 * @param Member 先頭名の後の修飾
	 * @param HasCustomDefinition 可視の独自定義の有無の格納先
	 * @return 最後の可視の導入（無ければ空）
	 * @throws FormatLimitExceeded 位置の比較が作業予算を超える場合
	 * @throws std::bad_alloc 比較する祖先の列又は導入のコピーを確保出来ない場合
	 * @throws std::length_error コピーするテキスト又は祖先の列の要素数上限を超える場合
	 * @throws std::out_of_range ノード又は収集済の名前の番号が表に無い場合
	 */
	const CallableIntroduction *SyntaxModel::FindBindingSource(
		const CallableIndex &Index,
		const NodeId Scope,
		const NodeId Head,
		const uint32_t NameIdx,
		const std::string &Member,
		bool &HasCustomDefinition
	) const {
		const SyntaxModel &Syntax = *this;
		const ControlFlow Flow(*this);
		// 位置とメンバに一致する可視の導入の照会
		const std::vector<CallableIntroduction> &Decls = Index.Names.at(NameIdx);
		const CallableImports &Imports = GetBindingSourcePositions(Index, NameIdx);
		const NodeId Import = Syntax.GetRules().HasNamespaceFunctionImports ? Syntax.GetNode(Head).Facts.ImportScope : NoNode;
		const CallableImports::const_iterator Found = Imports.find(Import);
		// 異なるインポート範囲の除外
		if(Found == Imports.end()) return nullptr;
		const bool IsEntire =
		Syntax.Is(Scope, Role::ClassBody) || Scope == Syntax.GetRoot() && !Syntax.GetRules().HasNamespaceFunctionImports &&
		(!Syntax.GetRules().HasFunctionLocalBindings || GetBindingScope(Head) != Scope);
		const CallableIntroduction *Result = nullptr;
		size_t Selected = Decls.size();
		bool HasOrder = false;
		// 順序付の列を二分探索し，範囲全体の導入と累積した独自定義を合わせる
		const auto Select = [&](const std::vector<CallablePosition> &Positions, const bool IsEntireScope, size_t &CachedEnd) -> void {
			// 範囲全体で可視の唯一の導入は，自身との一致だけを調べ記述順を採番しない
			const bool IsOnly = IsEntireScope && Positions.size() == 1;
			if(IsOnly) Syntax.Charge(1);
			// 短い列は従来の探索だけを使い，同じ宣言間の照会は両端の比較で確定する
			const auto IsBeforeHead = [&](const size_t Pos) -> bool {
				Syntax.Charge(1);
				// 現在の使用位置より前かの返戻
				return Flow.IsCallableBefore(Decls[Positions[Pos].Decl].Name, Head, HasOrder);
			};
			const bool CanReuse = Positions.size() > 2 && CachedEnd != SIZE_MAX && (!CachedEnd || IsBeforeHead(CachedEnd - 1)) &&
			(CachedEnd == Positions.size() || !IsBeforeHead(CachedEnd));
			const std::vector<CallablePosition>::const_iterator Before =
			IsOnly ? Positions.begin() : CanReuse ? Positions.begin() + CachedEnd : Positions.begin() + WorkSort::LowerBound(
				std::span<const CallablePosition>(Positions),
				Head,
				[&](const CallablePosition &Position, const NodeId Name) -> bool {
					Syntax.Charge(1);
					// 使用位置より前かの返戻
					return Flow.IsCallableBefore(Decls[Position.Decl].Name, Name, HasOrder);
				}
			);
			if(Positions.size() > 2) CachedEnd = static_cast<size_t>(Before - Positions.begin());
			size_t Count = IsEntireScope ? Positions.size() : static_cast<size_t>(Before - Positions.begin());
			// 可視の導入が無い場合の終了
			if(!Count) return;
			size_t CustomCount = Positions[Count - 1].CustomCount;
			if(IsEntireScope && Before != Positions.end() && Decls[Before->Decl].Name == Head) {
				CustomCount -= Decls[Before->Decl].IsCustomDefinition;
				if(Before + 1 == Positions.end()) --Count;
			}
			HasCustomDefinition = HasCustomDefinition || CustomCount;
			// 自身を除く導入が無い場合の終了
			if(!Count) return;
			const size_t Position = Positions[Count - 1].Decl;
			const CallableIntroduction &Decl = Decls[Position];
			// 宣言列は構文順で整列済なので，異なる名前ノードの順は列の位置で決まる
			if(!Result || Result->Name != Decl.Name && Selected < Position || Result->Name == Decl.Name && Position < Selected) {
				Result = &Decl;
				Selected = Position;
			}
			// 選択の終了
			return;
		};
		const std::string Empty;
		for(const std::string *const Suffix : { &Empty, &Member }) {
			const CallableMembers::const_iterator Positions = Found->second.find(*Suffix);
			if(Positions != Found->second.end()) {
				Select(Positions->second.Ordered, IsEntire, Positions->second.OrderedEnd);
				Select(Positions->second.Sequential, false, Positions->second.SequentialEnd);
				if(!IsEntire) Select(Positions->second.Wide, true, Positions->second.WideEnd);
			}
			if(Member.empty()) break; // 同じ修飾を２度照会しない
		}
		// 可視の最後の導入の返戻
		return Result;
	}

	/**
	 * 束縛と修飾した呼出の供給元の取得関数
	 * 計算量：束縛の取得費用 Q と供給値を探す範囲数 H，導入位置の照合費用 D に対し O(Q + H + D)
	 * @param Name 呼出先の先頭名
	 * @param Member 先頭名の後の修飾
	 * @param IsGlobal 根の名前空間から解決するか
	 * @return 現在の束縛と供給元（所有を保つ値で返戻する）
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc 範囲の事実又は修飾を確保出来ない場合
	 * @throws std::length_error 範囲又は修飾の上限を超える場合
	 * @throws std::out_of_range ノード又は事実のキーが範囲外の場合
	 */
	BindingFact SyntaxModel::GetBindingFact(const NodeId Name, const std::string_view Member, const bool IsGlobal) const {
		// 対象の無い束縛の返戻
		if(Name == NoNode || Name >= GetNodeCount()) return {};
		BindingFact Fact = GetBindingFact(Name);
		const ControlFlow Flow(*this);
		const SyntaxNode &Node = GetNode(Name);
		const std::string Joined = Node.Children.empty() ? std::string() : GetText(Name);
		const std::string &Text = Node.Children.empty() ? Node.Lexeme : Joined;
		const uint32_t NameIdx = Flow.GetCallHeadNameIdx(Name, Text);
		// 導入名を持たない供給元の返戻
		if(!NameIdx) return Fact;
		const std::string Suffix(Member);
		Charge(1 + Suffix.size());
		#if !defined(ShaveFormatRefFacts)
		const BindingRegion &Region = GetBindingRegion(GetBindingRegionOwner(Name));
		const uint64_t Key = uint64_t{ Name } << 1 | static_cast<uint64_t>(IsGlobal);
		if(
			const std::unordered_map<uint64_t, std::unordered_map<std::string, BindingFact>>::const_iterator Found =
			Region.SourceFacts.find(Key);
			Found != Region.SourceFacts.end()
		) {
			if(
				const std::unordered_map<std::string, BindingFact>::const_iterator Known = Found->second.find(Suffix);
				Known != Found->second.end()
			// 同じ束縛の領域で共有する供給元の事実の返戻
			) return Known->second;
		}
		#endif
		// 根の外側には探索を続けず，空の照会が根へ戻る契約を保つ
		for(
			NodeId Scope = IsGlobal ? Root : GetBindingScope(Name);
			Scope != NoNode;
			Scope = Scope == Root ? NoNode : GetBindingScope(GetParentOf(Scope))
		) {
			Charge(1);
			const BindingScope &Defs = GetBindingScopeFacts(Scope);
			if(!Defs.Sources->Names.contains(NameIdx)) continue;
			Fact.HasMemberChanges |= Defs.Sources->MemberSizes.contains(NameIdx);
			Fact.Sources = Defs.Sources;
			Fact.Source = FindBindingSource(*Defs.Sources, Scope, Name, NameIdx, Suffix, Fact.HasCustomDefinition);
			if(Fact.Source) break;
			// 関数全体の未初期化のローカル名を，外側の標準名へ戻さない
			if(GetRules().HasFunctionLocalBindings && IsBindingFunction(Scope) && Defs.Names.contains(NameIdx)) {
				Charge(1);
				const BindingDecl &Decl = Defs.Names.at(NameIdx).front();
				Fact.Decl = Decl.Name;
				Fact.Scope = Scope;
				Fact.IsDeclaredConstant = Decl.IsConstant;
				Fact.IsUncertain = true;
				break;
			}
		}
		#if !defined(ShaveFormatRefFacts)
		Region.SourceFacts[Key].emplace(Suffix, Fact);
		#endif
		// 同じ範囲の記録から解決した供給元の返戻
		return Fact;
	}

	/**
	 * 束縛への再代入又は未確定な作用の有無の判定関数
	 * 計算量：束縛取得と領域取得の費用 Q，未集約の捕捉集合の収集量 C に対し O(Q + C)
	 * @param Name 導入名のノード
	 * @return 再代入又は未確定な作用が在れば `true`
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 書込判定の範囲又は利用集合の取得のメモリを確保出来ない場合
	 * @throws std::length_error 利用集合の再収集で名前又は作業列の上限を超える場合
	 * @throws std::out_of_range ノードがノード表に無い場合，又は所属範囲が収集済の対応表に無い場合
	 */
	bool SyntaxModel::HasBindingWrite(const NodeId Name) const {
		// 束縛への再代入又は未確定な作用の有無の返戻
		return HasBindingUse(Name, true);
	}

	/**
	 * 束縛の利用又は未確定な作用の有無の判定関数
	 * 計算量：束縛取得と領域取得の費用 Q，未集約の捕捉集合の収集量 C に対し O(Q + C)
	 * @param Name 導入名のノード
	 * @return 利用又は未確定な作用が在れば `true`
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 参照判定の範囲又は利用集合の取得のメモリを確保出来ない場合
	 * @throws std::length_error 利用集合の再収集で名前又は作業列の上限を超える場合
	 * @throws std::out_of_range ノードがノード表に無い場合，又は所属範囲が収集済の対応表に無い場合
	 */
	bool SyntaxModel::HasBindingRef(const NodeId Name) const {
		// 束縛への利用又は未確定な作用の有無の返戻
		return HasBindingUse(Name, false);
	}

	/**
	 * 依存の逆引と共に利用集合を解放する関数
	 * 計算量：此の領域の依存数 D に対し期待値 O(D)
	 * @param Owner 解放する領域
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 */
	void SyntaxModel::EraseBindingRegion(const NodeId Owner) {
		// 此の領域が登録した逆引だけを解除する
		const std::unordered_map<NodeId, BindingRegion>::iterator Found = BindingRegions.find(Owner);
		InvalidateBindingSources(Owner);
		// 未取得の領域には解除する依存も無い
		if(Found == BindingRegions.end()) return;
		for(const NodeId Scope : Found->second.Dependencies) {
			Charge(1);
			const std::unordered_map<NodeId, std::unordered_set<NodeId>>::iterator Readers = BindingReaders.find(Scope);
			if(Readers == BindingReaders.end()) continue;
			Readers->second.erase(Owner);
			if(Readers->second.empty()) BindingReaders.erase(Readers);
		}
		BindingRegions.erase(Found);
		// 終了
		return;
	}

	/**
	 * 同じ供給元と其れを読む別名の保持の終了関数
	 * 計算量：依存する範囲数 S と解放する記録数 F に対し O(S + F)
	 * @param Scope 供給値が変わる範囲
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 */
	void SyntaxModel::InvalidateBindingSources(const NodeId Scope) const {
		const auto Clear = [&](const NodeId Owner) -> void {
			Charge(1);
			const std::unordered_map<NodeId, BindingScope>::const_iterator Found = BindingScopes.find(Owner);
			// 未収集の供給元の終了
			if(Found == BindingScopes.end()) return;
			const CallableIndex &Sources = *Found->second.Sources;
			Charge(
				Sources.Resolutions.size() + Sources.GuardPaths.size() + Sources.AliasSources.size() + Sources.EntireMemberReassignments.size()
			);
			Sources.Resolutions.clear();
			Sources.GuardPaths.clear();
			Sources.AliasSources.clear();
			Sources.EntireMemberReassignments.clear();
			// 終了
			return;
		};
		Clear(Scope);
		const NodeId Owner = GetBindingRegionOwner(Scope);
		// 計画の適用中は供給値を再収集せず，同じ領域の逆引を１度だけ解除する
		if(RewriteSourceOwners) {
			Charge(1);
			// 同じ領域の逆引を解除済の場合の終了
			if(!RewriteSourceOwners->insert(Owner).second) return;
		}
		if(
			const std::unordered_map<NodeId, std::unordered_set<NodeId>>::const_iterator Readers = SourceReaders.find(Owner);
			Readers != SourceReaders.end()
		) for(const NodeId Reader : Readers->second) if(Reader != Scope) Clear(Reader);
		// 終了
		return;
	}

	/**
	 * 宣言と供給元の範囲を依存の逆引と共に解放する関数
	 * 計算量：供給値の失効費用 Q と自身の依存数 D に対し O(Q + D)
	 * @param Scope 解放する範囲
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 */
	void SyntaxModel::EraseBindingScope(const NodeId Scope) const {
		InvalidateBindingSources(Scope);
		const std::unordered_map<NodeId, BindingScope>::const_iterator Found = BindingScopes.find(Scope);
		// 解放済の範囲の終了
		if(Found == BindingScopes.end()) return;
		if(Found->second.SourceOwner != NoNode) {
			Charge(1);
			const std::unordered_map<NodeId, std::unordered_set<NodeId>>::iterator Readers = SourceReaders.find(Found->second.SourceOwner);
			if(Readers != SourceReaders.end()) {
				Readers->second.erase(Scope);
				if(Readers->second.empty()) SourceReaders.erase(Readers);
			}
		}
		BindingScopes.erase(Found);
		// 終了
		return;
	}

	/**
	 * 書換の前後で影響する範囲の記録関数
	 * 計算量：部分木・祖先・子の総探索量 S，破棄する表の総要素数 K，影響する依存の総数 D に対し期待値 O(S + K + D)
	 * @param Node 変更するノード又は接続済のノード
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 変更範囲又は構文分類の作業列のメモリを確保出来ない場合
	 * @throws std::length_error 構文分類の作業列の要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード又は子の識別子がノード表に無い場合
	 */
	void SyntaxModel::MarkBindingChange(const NodeId Node) {
		// 書換の前後で影響する範囲の記録
		// 利用集合を未だ収集して居ない構築中の木と，対象外の書換の終了
		if(BindingScopes.empty() && BindingRegions.empty() || Node == NoNode || Node >= Nodes.size()) return;
		// 値の束縛を読まない領域と，作用の無いトークンの除外
		const BindingSyntax Shape = GetBindingSyntax(Node);
		bool HasBindingEffect = false;
		switch(Shape) {
		case BindingSyntax::Ignore:
		case BindingSyntax::MemberPart:
			// 呼出の供給値も同じ範囲の記録が所有する
			HasBindingEffect = true;
			break;
		case BindingSyntax::None:
			{
				const NodeId Parent = Nodes[Node].Parent;
				const BindingSyntax ParentShape = GetBindingSyntax(Parent);
				// 束縛のマーカーの有無は仮引数をメンバにするか等の可視性を変える
				HasBindingEffect =
				IsWord(Node, Word::FunctionScopedBinding) || IsWord(Node, Word::ConstantDecl) || IsTokenRole(Node, Role::RightArrowToken) ||
				Is(Node, Role::BindingMarker);
				if(Parent != NoNode) {
					// 演算子・借用指定・反復の導入語は書込又は有効範囲を変える
					HasBindingEffect = HasBindingEffect ||
					ParentShape == BindingSyntax::Update && (HasField(Node, ChildRole::Operator) || IsTokenRole(Node, Role::ChangeByOneToken)) ||
					ParentShape == BindingSyntax::Loop && (IsWord(Node, Word::ReassignableBinding) || IsWord(Node, Word::ImmutableBinding)) ||
					Is(Parent, Role::BorrowExpr) && Is(Node, Role::MutableSpecifier) || Is(Parent, Role::RefBindingPattern) ||
					Is(Parent, Role::MutablePattern) && Is(Nodes[Parent].Parent, Role::RefBindingPattern);
				}
				if(!HasBindingEffect) {
					Charge(Nodes[Node].Children.size());
					for(
						std::vector<NodeId> Pending(Nodes[Node].Children.begin(), Nodes[Node].Children.end());
						!HasBindingEffect && !Pending.empty();
					) {
						const NodeId Child = Pending.back();
						Pending.pop_back();
						Charge(1);
						switch(GetBindingSyntax(Child)) {
						case BindingSyntax::Ignore:
						case BindingSyntax::MemberPart:
							break;
						case BindingSyntax::None:
							Charge(Nodes[Child].Children.size());
							Pending.insert(Pending.end(), Nodes[Child].Children.begin(), Nodes[Child].Children.end());
							break;
						default:
							HasBindingEffect = true;
							break;
						}
					}
				}
			}
			break;
		default:
			HasBindingEffect = true;
			break;
		}
		// 束縛に作用しない分類・トークンの書換の終了
		if(!HasBindingEffect) return;
		const NodeId Owner = GetBindingRegionOwner(Node);
		EraseBindingRegion(Owner);
		if(Owner != Root) {
			if(const std::unordered_map<NodeId, BindingRegion>::iterator Outer = BindingRegions.find(Root); Outer != BindingRegions.end()) {
				Outer->second.Captures.reset();
			}
		}
		// 参照・書込先のトークンだけの変更は導入名のインデックスを変えない
		if(Shape == BindingSyntax::Name) {
			NodeId Part = Node;
			for(NodeId Up = GetParentOf(Part); Up != NoNode; Up = GetParentOf(Part)) {
				Charge(1);
				bool IsIntroduction = false;
				switch(GetBindingSyntax(Up)) {
				case BindingSyntax::NamePattern:
				case BindingSyntax::Pattern:
				case BindingSyntax::RestPattern:
				case BindingSyntax::DefaultPattern:
					// 既定値の参照の変更の終了
					if(HasField(Part, ChildRole::Val) || HasField(Part, ChildRole::Right)) return;
					Part = Up;
					continue;
				case BindingSyntax::Params:
				case BindingSyntax::Param:
				case BindingSyntax::ConstructorParam:
				case BindingSyntax::Catch:
				case BindingSyntax::Decl:
					IsIntroduction = true;
					break;
				case BindingSyntax::Write:
					IsIntroduction = Is(Up, Role::FirstAssignmentBinding) && HasField(Part, ChildRole::Left);
					break;
				case BindingSyntax::Declarator:
					IsIntroduction =
					HasField(Part, ChildRole::Name) || HasField(Part, ChildRole::Declarator) || HasField(Part, ChildRole::Pattern) ||
					HasField(Part, ChildRole::Left);
					break;
				case BindingSyntax::Function:
				case BindingSyntax::FunctionDecl:
				case BindingSyntax::Closure:
				case BindingSyntax::NamedExpr:
				case BindingSyntax::MemberFunction:
				case BindingSyntax::ClassDecl:
				case BindingSyntax::ClassExpr:
				case BindingSyntax::ModuleDecl:
					IsIntroduction = HasField(Part, ChildRole::Name);
					break;
				case BindingSyntax::Loop:
					IsIntroduction = HasField(Part, ChildRole::Pattern) || HasField(Part, ChildRole::Left) || HasField(Part, ChildRole::Name);
					break;
				}
				if(IsIntroduction) break;
				// 同じ名前の供給値と比較名も変更する為，範囲の共有記録を失効する
				break;
			}
		}
		// 変更部分木の有効範囲と，導入を読む外側の範囲の解放
		std::vector<NodeId> ChangedScopes;
		for(std::vector<NodeId> Pending { Node }; !Pending.empty();) {
			const NodeId Cur = Pending.back();
			Pending.pop_back();
			Charge(Nodes[Cur].Children.size() + 1);
			if(BindingScopes.contains(Cur)) {
				EraseBindingScope(Cur);
				ChangedScopes.push_back(Cur);
			}
			Pending.insert(Pending.end(), Nodes[Cur].Children.begin(), Nodes[Cur].Children.end());
		}
		const NodeId Scope = GetBindingScope(Node), OuterScope = GetBindingScope(GetParentOf(Node));
		if(
			const bool IsFunctionBody =
			IsBindingFunction(Scope) && !GetRules().HasFunctionLocalBindings && !Is(Scope, Role::FunctionBindingScope) &&
			HasNodeWithin(GetFieldChild(Scope, ChildRole::Body), Node);
			!IsFunctionBody
		) {
			EraseBindingScope(Scope);
			ChangedScopes.push_back(Scope);
		}
		// `var` の供給元と，名前付の項目を導入する外側の範囲の失効
		for(NodeId Prev = Node, Up = GetParentOf(Node); Up != NoNode; Prev = Up, Up = GetParentOf(Up)) {
			Charge(1);
			if(
				IsBindingScope(Up) && !(
					IsBindingFunction(Up) && !GetRules().HasFunctionLocalBindings && !Is(Up, Role::FunctionBindingScope) &&
					GetFieldChild(Up, ChildRole::Body) == Prev
				) && (
					Up == OuterScope || GetBindingSyntax(Up) == BindingSyntax::VarBlock || IsBindingFunction(GetParentOf(Up)) ||
					GetBindingSyntax(GetParentOf(Up)) == BindingSyntax::VarBlock
				)
			) {
				EraseBindingScope(Up);
				ChangedScopes.push_back(Up);
			}
			if(IsBindingFunction(Up) || GetBindingSyntax(Up) == BindingSyntax::ModuleDecl) break;
		}
		// 変更された型頭部から仮引数を供給する範囲の解放
		const std::unordered_set<NodeId> Changed(ChangedScopes.begin(), ChangedScopes.end());
		for(const NodeId Owner : Changed) {
			Charge(1);
			const std::pair<ParamScopeIdx::const_iterator, ParamScopeIdx::const_iterator> Readers = ParamScopes.equal_range(Owner);
			for(ParamScopeIdx::const_iterator Reader = Readers.first; Reader != Readers.second; ++Reader) {
				Charge(1);
				EraseBindingScope(Reader->second);
			}
			ParamScopes.erase(Readers.first, Readers.second);
		}
		// 変更範囲から直接引いた領域だけを解放する
		std::unordered_set<NodeId> Affected;
		for(const NodeId Scope : Changed) {
			Charge(1);
			if(
				const std::unordered_map<NodeId, std::unordered_set<NodeId>>::iterator Readers = BindingReaders.find(Scope);
				Readers != BindingReaders.end()
			) {
				Charge(Readers->second.size());
				Affected.insert(Readers->second.begin(), Readers->second.end());
			}
		}
		for(const NodeId Region : Affected) EraseBindingRegion(Region);
		// 終了
		return;
	}
} // namespace Core
