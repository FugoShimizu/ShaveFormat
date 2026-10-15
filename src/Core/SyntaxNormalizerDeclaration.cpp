#include "SyntaxNormalizer.hpp"
#include "ControlFlow.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 未確定な展開と名前の正規化の事実の取得関数
	 * 計算量：構文のノード数 N と未投影の名前の走査費用 B に対し期待値 O(N + B)
	 * @param Syntax 現在の構文
	 * @return 各部分木の終端・最後のマクロの走査位置と，非 ASCII 名の有無を持つ事実
	 * @throws FormatLimitExceeded 集約又は事実の収集が作業予算を超えた場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	SyntaxNormalizer::UncertainNameFacts SyntaxNormalizer::GetUncertainNames(const SyntaxModel &Syntax) const {
		// 未確定な展開と名前の正規化の事実の取得
		UncertainNameFacts Facts;
		std::vector<std::pair<NodeId, bool>> Pending { { Syntax.GetRoot(), false } };
		size_t Pos = 0;
		while(!Pending.empty()) {
			const std::pair<NodeId, bool> Cur = Pending.back();
			Pending.pop_back();
			Syntax.Charge(1);
			UncertainScope &Summary = Facts.Scopes[Cur.first];
			if(!Cur.second) {
				++Pos;
				if(Syntax.Is(Cur.first, Role::NormalizedBindingName) && Syntax.GetNameSpellingOf(Cur.first).HasNonAscii) {
					Summary.HasUnicode = true;
				}
				if(Syntax.Is(Cur.first, Role::TerminatingMacroCall)) Summary.LastMacro = Pos;
				Pending.emplace_back(Cur.first, true);
				const std::span<const NodeId> Children = Syntax.GetChildrenOf(Cur.first);
				for(size_t Idx = Children.size(); Idx--;) Pending.emplace_back(Children[Idx], false);
			} else {
				Summary.End = Pos;
				for(const NodeId Child : Syntax.GetChildrenOf(Cur.first)) {
					Syntax.Charge(1);
					const UncertainScope &Inner = Facts.Scopes.at(Child);
					Summary.LastMacro = std::max(Summary.LastMacro, Inner.LastMacro);
					Summary.HasUnicode |= Inner.HasUnicode;
				}
			}
		}
		// 此の規則で使う未確定名の事実の返戻
		return Facts;
	}

	/**
	 * 不変指定の正規化の収集関数（整形仕様 5.3）
	 * 変更の無いローカルの束縛を，言語の不変指定へ揃える
	 * Java は `final` を付け，Rust は `mut` を除き，Kotlin は `var` を `val` にする
	 * 導入名を識別する共通の束縛の書込・参照を読み，各言語の不変化の条件を確認する
	 * Java・Kotlin は名前への代入と増減を束縛の変更とする
	 * Rust はフィールド・添字を介した代入，可変借用，可変の呼出の受手，`ref mut` の照合も束縛の変更とする
	 * Java の定数式で初期化する名前は定数変数化で意味が変わり得る為に利用が無い物だけを対象とする
	 * Rust は後続に未展開のマクロが在る範囲，非 ASCII 名を含む範囲，展開前の属性，条件・クロージャの束縛を除く
	 * Kotlin はローカルの文列に直接属さない束縛，`lateinit` と委譲先が可変性を観測出来る宣言を除く
	 * 役割から候補を選び，Rust の候補が在る場合はソース全体の未確定名の事実も収集する
	 * 計算量：候補選択費用 S，依存グラフ・未確定名の処理量 U，束縛照会費用 Q，書換の生成量 G に対し期待値 O(S + U + Q + G)
	 * @param Syntax 構文モデル
	 * @param Plan 書換の計画の格納先
	 * @return 不変指定する Java の宣言子の集合（其の他の書換は計画へ追加し，集合は空）
	 * @throws FormatLimitExceeded 候補・束縛・依存グラフの走査が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	std::unordered_set<NodeId> SyntaxNormalizer::CollectImmutableBindings(const SyntaxModel &Syntax, RewritePlan &Plan) const {
		// 不変指定の正規化の収集
		std::unordered_set<NodeId> Frozen;
		// 不変の束縛を宣言する語
		const std::string Immutable(Syntax.GetRules().GetSpelling(Word::ImmutableBinding));
		if(
			Syntax.GetRules().HasBindingIdentity && !Syntax.GetRules().ShouldPreserveConstantStatus &&
			!Syntax.GetRules().ShouldRemoveMutableMarker
		) {
			// ローカル位置・委譲・`lateinit` の条件を保つパターン全体の書込判定
			for(const NodeId Node : GetCandidates(Syntax, { Role::ReplaceableBindingKeyword })) {
				if(
					GetNormalizationBarrierOf(Syntax, Node).IsProtected || !Syntax.Is(Syntax.GetParentOf(Node), Role::StmtSeq) ||
					Core::SyntaxModel::HasChildMatching(
						Syntax,
						Node,
						[&Syntax](const NodeId Child) -> bool {
							// 委譲先の初期化とアクセサが宣言の可変性を観測出来る事の返戻
							return Syntax.GetBindingSyntax(Child) == BindingSyntax::Initializer;
						}
					)
				) continue;
				const bool IsLateInit = Core::SyntaxModel::HasChildMatching(
					Syntax,
					Syntax.GetFirstNamedChildWithRole(Node, Role::ModifierSeq),
					[&Syntax](const NodeId Modifier) -> bool {
						// 後で初期化する修飾かの返戻
						return Syntax.IsWord(Modifier, Word::DeferredInit);
					}
				);
				const NodeId Keyword = Syntax.GetChild(Syntax.GetFirstNamedChildWithRole(Node, Role::BindingMarker), 0);
				if(IsLateInit || !Syntax.IsWord(Keyword, Word::ReassignableBinding)) continue;
				bool IsUncertain = false;
				if(
					const std::vector<NodeId> Names = Syntax.GetBindingNames(Node, &IsUncertain); IsUncertain || std::ranges::any_of(
						Names,
						[&Syntax](const NodeId Name) -> bool {
							// １つでも再代入又は未確定な作用を持つ束縛が在るかの返戻
							return Syntax.HasBindingWrite(Name);
						}
					)
				) continue;
				RewriteStep Step { RewriteKind::Reshape, Keyword };
				Step.Inserted = { Immutable };
				Plan.Steps.push_back(std::move(Step));
			}
			// 共通の束縛に依る指定を収集した後の空集合の返戻
			return Frozen;
		}
		// 不変指定を持たない言語の空集合の返戻
		if(Immutable.empty() && !Syntax.GetRules().ShouldRemoveMutableMarker) return Frozen;
		// 束縛の変更と不変指定の候補の収集準備
		std::vector<ImmutableCandidate> Bindings;
		// 此の収集だけの依存グラフ（保存キャッシュを使わず，依存の閉路も共有する）
		std::unordered_set<NodeId> Expanded, Nonconstant;
		std::unordered_map<NodeId, std::vector<NodeId>> Users;
		std::unordered_map<NodeId, BindingValSrc> Sources;
		// 導入名の初期値からの非定数の種への到達の共有
		const auto HasNonconstantDependency = [&](const NodeId Val) -> bool {
			std::vector<NodeId> Pending { Val }, Changed;
			while(!Pending.empty()) {
				const NodeId Cur = Pending.back();
				Pending.pop_back();
				Syntax.Charge(1);
				if(Cur == NoNode || !Expanded.insert(Cur).second) continue;
				// 式の子又は導入名の初期値から此の式へ伝わる逆辺
				const auto AddDependency = [&](const NodeId Dependency) -> void {
					// 初期値を持たない場合の終了
					if(Dependency == NoNode) return;
					Syntax.Charge(1);
					Users[Dependency].push_back(Cur);
					Pending.push_back(Dependency);
					if(Nonconstant.contains(Dependency)) Changed.push_back(Cur);
					// 終了
					return;
				};
				if(Syntax.Is(Cur, Role::Identifier)) {
					if(const BindingFact Binding = Syntax.GetBindingFact(Cur); Binding.Decl != NoNode && !Binding.IsUncertain) {
						Syntax.Charge(1);
						const std::unordered_map<NodeId, BindingValSrc>::const_iterator Known = Sources.find(Binding.Decl);
						BindingValSrc Src;
						if(Known != Sources.end()) Src = Known->second;
						else {
							Src = Syntax.GetBindingValSrc(Binding);
							Sources.emplace(Binding.Decl, Src);
						}
						if(Src.IsNonconstant) Changed.push_back(Cur);
						else AddDependency(Src.Val);
					}
				}
				if(Syntax.Is(Cur, Role::NonConstantExpr)) Changed.push_back(Cur);
				if(Syntax.Is(Cur, Role::BinaryOperation)) {
					AddDependency(Syntax.GetFieldChild(Cur, ChildRole::Left));
					AddDependency(Syntax.GetFieldChild(Cur, ChildRole::Right));
				} else if(Syntax.Is(Cur, Role::TernaryExpr)) {
					AddDependency(Syntax.GetFieldChild(Cur, ChildRole::Cond));
					AddDependency(Syntax.GetFieldChild(Cur, ChildRole::Consequence));
					AddDependency(Syntax.GetFieldChild(Cur, ChildRole::Alternative));
				} else if(Syntax.Is(Cur, Role::UnaryOperation)) AddDependency(Syntax.GetFieldChild(Cur, ChildRole::Operand));
				else if(Syntax.Is(Cur, Role::CastExpr)) AddDependency(Syntax.GetFieldChild(Cur, ChildRole::Val));
				else if(Syntax.Is(Cur, Role::ValParens)) for(const NodeId Child : Syntax.GetChildrenOf(Cur)) if(Syntax.IsNamed(Child)) {
					AddDependency(Child);
				}
			}
			while(!Changed.empty()) {
				const NodeId Node = Changed.back();
				Changed.pop_back();
				Syntax.Charge(1);
				if(!Nonconstant.insert(Node).second) continue;
				if(const std::unordered_map<NodeId, std::vector<NodeId>>::const_iterator Found = Users.find(Node); Found != Users.end()) {
					for(const NodeId User : Found->second) {
						Syntax.Charge(1);
						Changed.push_back(User);
					}
				}
			}
			// 非定数の種へ到達する初期値かの返戻
			return Nonconstant.contains(Val);
		};
		// 宣言・可変のマーカー・束縛名と，其の不変化を妨げる条件の収集
		std::unordered_map<NodeId, bool> AttributeProtection;
		// 可変パターンの束縛と不変化可能な宣言の候補の走査
		const std::vector<NodeId> Inputs = GetCandidates(Syntax, { Role::FinalizableLocal, Role::MutablePatternBinding });
		for(const NodeId Node : Inputs) {
			if(GetNormalizationBarrierOf(Syntax, Node).IsProtected) continue;
			if(Syntax.Is(Node, Role::MutablePatternBinding)) {
				const NodeId Pattern = Syntax.GetFieldChild(Node, ChildRole::Pattern);
				// 条件の束縛と，可変の呼出の要否を型無で確定出来ないクロージャは保つ
				if(
					const NodeId Val = Syntax.GetFieldChild(Node, ChildRole::Val);
					Syntax.Is(Node, Role::BindingCond) || Syntax.Is(Val, Role::ClosureExpr)
				) continue;
				std::vector<std::pair<NodeId, NodeId>> MarkedNames;
				// 束縛のパターンと不変指定の対応の収集
				const auto AddPattern = [&](const NodeId Part, const NodeId Specifier) -> void {
					NodeId Name = Syntax.Is(Part, Role::MutablePattern) ? Syntax.GetNamedChild(Part, 1) : Part;
					if(Syntax.Is(Name, Role::CapturedPattern)) Name = Syntax.GetNamedChild(Name, 0);
					if(Specifier != NoNode && Syntax.Is(Name, Role::Identifier)) MarkedNames.emplace_back(Name, Specifier);
					// 終了
					return;
				};
				if(const NodeId Specifier = Syntax.GetFirstNamedChildWithRole(Node, Role::MutableSpecifier); Specifier != NoNode) {
					AddPattern(Pattern, Specifier);
				}
				Core::SyntaxModel::HasStoppedTraversal(
					Syntax,
					Pattern,
					[&](const NodeId Part) -> WalkStep {
						// `ref mut` の借用の指定は保ち，捕捉の内側の別の束縛へは降りる
						if(Syntax.Is(Part, Role::MutablePattern) && !Syntax.HasWordChild(Syntax.GetParentOf(Part), Word::RefBinding)) {
							AddPattern(Part, Syntax.GetFirstNamedChildWithRole(Part, Role::MutableSpecifier));
						}
						// ネストしたパターンへ降りる事の返戻
						return WalkStep::Descend;
					}
				);
				// 可変のマーカーを持つ名前の所属範囲と属性の保護の判定
				if(MarkedNames.empty()) continue;
				NodeId Scope = Syntax.GetParentOf(Node);
				while(Scope != NoNode && !Syntax.Is(Scope, Role::LexicalBlock)) Scope = Syntax.GetParentOf(Scope);
				const bool IsAttributed = ControlFlow::HasUnexpandedAttribute(Syntax, Node, AttributeProtection);
				for(const std::pair<NodeId, NodeId> &Marked : MarkedNames) {
					Bindings.push_back({ Node, Marked.second, Scope, IsAttributed, { Marked.first }, {} });
				}
			} else if(Syntax.Is(Node, Role::FinalizableLocal)) {
				const NodeId Marker = Syntax.GetFirstNamedChildWithRole(Node, Role::ModifierSeq);
				if(Syntax.HasWordChild(Marker, Word::ImmutableBinding)) continue;
				for(const NodeId Declarator : Syntax.GetChildrenOf(Node)) {
					if(!Syntax.Is(Declarator, Role::VarDeclaratorItem)) continue;
					const NodeId Val = Syntax.GetFieldChild(Declarator, ChildRole::Val);
					const NodeId Name = Syntax.GetFieldChild(Declarator, ChildRole::Name);
					if(Val == NoNode || !Syntax.Is(Name, Role::Identifier)) continue;
					ImmutableCandidate Found { Declarator, Marker, NoNode, false, { Name }, {} };
					if(!HasNonconstantDependency(Val)) Found.ConstantBindings.push_back(Name);
					Bindings.push_back(std::move(Found));
				}
			}
		}
		// 判定結果を使う不変化候補が無い場合の空集合の返戻
		if(Bindings.empty()) return Frozen;
		const UncertainNameFacts Uncertain =
		Syntax.GetRules().ShouldRemoveMutableMarker ? GetUncertainNames(Syntax) : UncertainNameFacts{};
		// 変更と参照の結果に基付く候補の絞込と書換
		for(const ImmutableCandidate &Found : Bindings) {
			Syntax.Charge(1);
			if(
				std::ranges::any_of(
					Found.BindingNames,
					[&](const NodeId Name) -> bool {
						// 同じ導入名に書込又は未確定な作用が在るかの返戻
						return Syntax.HasBindingWrite(Name);
					}
				) || std::ranges::any_of(
					Found.ConstantBindings,
					[&](const NodeId Name) -> bool {
						// 定数変数化する可能性の有る束縛を利用する式が在るかの返戻
						return Syntax.HasBindingRef(Name);
					}
				) || Syntax.Is(Found.Decl, Role::MutablePatternBinding) && (
					Found.Scope == NoNode || Found.IsAttributed ||
					Uncertain.Scopes.at(Found.Scope).LastMacro > Uncertain.Scopes.at(Found.Decl).End || Uncertain.Scopes.at(Found.Scope).HasUnicode
				)
			) continue;
			if(Syntax.Is(Found.Decl, Role::VarDeclaratorItem)) Frozen.insert(Found.Decl);
			else if(Syntax.Is(Found.Decl, Role::MutablePatternBinding)) {
				// `mut` の指定と其のトークンの除去（アンカーは束縛のパターンの前へ移す）
				const AnchorTarget Next { Syntax.GetNextSibling(Found.Marker), false };
				Plan.Steps.push_back({ RewriteKind::Remove, Found.Marker, {}, { Found.Marker, Next, Next, Next } });
				for(const NodeId Token : Syntax.GetChildrenOf(Found.Marker)) {
					Plan.Steps.push_back({ RewriteKind::Remove, Token, {}, { Token, Next, Next, Next } });
				}
				if(Syntax.Is(Syntax.GetParentOf(Found.Marker), Role::MutablePattern)) {
					Plan.Steps.push_back({ RewriteKind::Unwrap, Syntax.GetParentOf(Found.Marker) });
				}
			}
		}
		// 不変指定する宣言子の返戻
		return Frozen;
	}

	/**
	 * ソース全体のレキシカル宣言の書換方針の取得関数
	 * モジュールかの判定，名前のエスケープ・動的な有効範囲への呼出，型照会・捕捉の導入名を求める
	 * 計算量：候補選択費用 Q，構文のノード数 N，束縛照会と未解決名毎の祖先の収集費用 S に対し O(Q + N + S)
	 * @param Syntax 現在の構文
	 * @return 動的な名前解決の有無，モジュールかの判定，型保持の要否と型を観測する束縛
	 * @throws FormatLimitExceeded 集約又は事実の収集が作業予算を超えた場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	SyntaxNormalizer::LexicalPolicy SyntaxNormalizer::GetLexicalPolicy(const SyntaxModel &Syntax) const {
		// ソース全体のレキシカル宣言の書換方針の取得
		LexicalPolicy Policy;
		Policy.IsModule = Syntax.IsModuleSrc.value_or(false);
		// 不変指定が束縛の型へ及ぼす条件を固定の意味の定義から読む
		Policy.ShouldPreserveBindingTypes = Syntax.GetRules().ShouldPreserveBindingTypes;
		// ソース全体の動的な名前解決と最上位の記法の確認
		std::unordered_set<NodeId> Visited;
		for(const Role Wanted : { Role::BindingName, Role::CallExpr, Role::Export, Role::Identifier, Role::ImportStmt }) {
			for(const NodeId Node : Syntax.GetNodesWith(Wanted)) {
				Syntax.Charge(1);
				if(!Visited.insert(Node).second) continue;
				if(
					!Syntax.IsModuleSrc.has_value() && Syntax.GetParentOf(Node) == Syntax.GetRoot() &&
					(Syntax.Is(Node, Role::Export) || Syntax.Is(Node, Role::ImportStmt))
				) Policy.IsModule = true;
				if((Syntax.Is(Node, Role::Identifier) || Syntax.Is(Node, Role::BindingName)) && Syntax.GetNameSpellingOf(Node).HasEscape) {
					Policy.IsDynamic = true;
				}
				if(Syntax.Is(Node, Role::CallExpr)) {
					NodeId Function = Syntax.GetFieldChild(Node, ChildRole::Function);
					while(Syntax.Is(Function, Role::GroupingParen)) Function = Syntax.GetNamedChild(Function, 0);
					if(Syntax.IsWord(Function, Word::DynamicScope)) Policy.IsDynamic = true;
				}
			}
		}
		// 型照会と捕捉を同じ走査で収集し，クラスの初期化位置も区別する
		struct Frame {
			NodeId Node;
			NodeId ClassBody;
			bool IsTypeQuery;
		}; // 型照会の状態と，参照を含むクラスの本体
		std::unordered_set<uint32_t> BindingNames;
		std::unordered_map<NodeId, NodeId> BindingClasses;
		std::vector<std::pair<NodeId, bool>> UnresolvedRefs;
		std::vector<std::pair<NodeId, NodeId>> ClassRefs;
		std::vector<Frame> Pending { { Syntax.GetRoot(), NoNode, false } };
		while(!Pending.empty()) {
			const Frame Cur = Pending.back();
			Pending.pop_back();
			Syntax.Charge(1);
			const KindDef *const Def = Syntax.GetNode(Cur.Node).Kind.Def;
			const bool IsTypeQuery = Cur.IsTypeQuery ||
			Def && Def->Evaluation.Children == EvaluationClass::Unevaluated && Syntax.HasWordChild(Cur.Node, Word::TypeQuery);
			const NodeId ClassBody = Syntax.Is(Cur.Node, Role::ClassBody) ? Cur.Node : Cur.ClassBody;
			if(
				IsTypeQuery && Syntax.Is(Cur.Node, Role::Identifier) ||
				Policy.ShouldPreserveBindingTypes && Def && Def->Binding == BindingSyntax::Name
			) {
				const BindingFact Binding = Syntax.GetBindingFact(Cur.Node);
				if(Policy.ShouldPreserveBindingTypes && Binding.Decl != NoNode) {
					if(Binding.IsDecl) {
						BindingNames.insert(Syntax.GetCmpNameIdx(Cur.Node));
						BindingClasses.insert_or_assign(Binding.Decl, ClassBody);
					} else if(ClassBody != NoNode && !Binding.IsCapture && !Binding.IsUncertain) ClassRefs.emplace_back(Binding.Decl, ClassBody);
				}
				if(IsTypeQuery || Policy.ShouldPreserveBindingTypes && (Binding.IsCapture || Binding.IsUncertain && !Binding.IsDecl)) {
					if(Binding.Decl != NoNode && !Binding.IsUncertain) Policy.TypeObservedBindings.insert(Binding.Decl);
					else UnresolvedRefs.emplace_back(Cur.Node, IsTypeQuery);
				}
			}
			for(const NodeId Child : Syntax.GetChildrenOf(Cur.Node)) Pending.push_back({ Child, ClassBody, IsTypeQuery });
		}
		// 前方の参照も，導入名と同じクラスの本体かを収集後に１度照合する
		for(const std::pair<NodeId, NodeId> &Ref : ClassRefs) {
			Syntax.Charge(1);
			const std::unordered_map<NodeId, NodeId>::const_iterator Found = BindingClasses.find(Ref.first);
			if(Found == BindingClasses.end() || Found->second != Ref.second) Policy.TypeObservedBindings.insert(Ref.first);
		}
		// 未解決の参照は，同名の導入が在る場合又は型照会だけ可視範囲を保護する
		for(const std::pair<NodeId, bool> &Ref : UnresolvedRefs) {
			Syntax.Charge(1);
			const uint32_t NameIdx = Syntax.GetCmpNameIdx(Ref.first);
			if(!Ref.second && !BindingNames.contains(NameIdx)) continue;
			std::unordered_set<NodeId> &Scopes = Policy.UnresolvedTypeNames[NameIdx];
			for(NodeId Up = Syntax.GetParentOf(Ref.first); Up != NoNode; Up = Syntax.GetParentOf(Up)) {
				Syntax.Charge(1);
				if(!Scopes.insert(Up).second) break;
			}
		}
		// 此の規則で使う束縛の方針の返戻
		return Policy;
	}

	/**
	 * 不変指定で束縛の型を保持出来るかの判定関数
	 * 型注釈の無い単純名では，リテラル型を生まない初期化子だけを許す
	 * 捕捉等で型を観測する束縛は，共通の方針で此の判定とは別に除く
	 * 分割代入は型と初期化子から束縛間の相関を否定出来る形だけを許す
	 * 計算量：初期化子を包む括弧の深さ D，分割パターンのノード数 P に対し O(D + P + 1)
	 * @param Syntax 現在の構文
	 * @param Policy レキシカル宣言の方針
	 * @param Decl 不変指定の候補の宣言子
	 * @return 型の保持を確認出来る場合は真
	 * @throws FormatLimitExceeded 初期化子の括弧又はパターンの走査が作業予算を超えた場合
	 * @throws std::bad_alloc パターンの走査のメモリを確保出来ない場合
	 * @throws std::length_error パターンの作業列の上限を超える場合
	 * @throws std::out_of_range 照会するノードが範囲外の場合
	 */
	bool SyntaxNormalizer::CanPreserveBindingType(const SyntaxModel &Syntax, const LexicalPolicy &Policy, const NodeId Decl) const {
		// 型への影響を確認しない言語の受理
		if(!Policy.ShouldPreserveBindingTypes) return true;
		const NodeId Type = Syntax.GetFieldChild(Decl, ChildRole::Type);
		const NodeId Pattern = Syntax.GetFieldChild(Decl, ChildRole::Name);
		const bool IsSimple = Syntax.Is(Pattern, Role::BindingName);
		// 単純名の宣言型を保持し，捕捉等の参照条件は共通の方針で確かめる
		if(Type != NoNode && IsSimple) return true;
		NodeId Val = Syntax.GetFieldChild(Decl, ChildRole::Val);
		while(Syntax.Is(Val, Role::GroupingParen)) {
			Syntax.Charge(1);
			Val = Syntax.GetNamedChild(Val, 0);
		}
		// 分割代入は，合併型の相関と既存の値への別名を作らない形だけ受理する
		if(!IsSimple) {
			const NodeId Annotation = Syntax.Is(Type, Role::TypeAnnotation) ? Syntax.GetNamedChild(Type, 0) : Type;
			// 型の相関又は既存の値への別名を否定出来ない場合の除外
			if(
				!(Syntax.Is(Annotation, Role::TypeMemberBody) && Syntax.Is(Val, Role::ObjectVal)) &&
				!(Syntax.Is(Annotation, Role::ArrayTypeSyntax) && Syntax.Is(Val, Role::ArrayLiteral))
			) return false;
			// 更に入子の型を分解しないパターンかの返戻
			return !SyntaxModel::HasNodeMatching(
				Syntax,
				Pattern,
				[&Syntax, Pattern](const NodeId Node) -> bool {
					const KindDef *const Def = Syntax.GetNode(Node).Kind.Def;
					// 入子で分解する型の相関を推定しない事の返戻
					return Node != Pattern && Def && Def->Binding == BindingSyntax::Pattern;
				}
			);
		}
		// 束縛自体がオブジェクト又は関数の型に為る初期化子の受理
		return Syntax.IsAny(Val, { Role::ArrayLiteral, Role::ArrowFunctionNode, Role::FunctionExpr, Role::ObjectVal, Role::Regex });
	}

	/**
	 * 型照会・捕捉等から型を観測される導入名かの判定関数
	 * 解決済の同一性を優先し，未解決の名前も可視の範囲の連鎖だけに限定する
	 * 計算量：集合の照会は期待値 O(1)，束縛の取得費用を Q として O(1 + Q)
	 * @param Syntax 現在の構文
	 * @param Policy 型を観測する束縛を集めた方針
	 * @param Name 不変指定の候補の導入名
	 * @return 型を観測される又は観測を否定出来ない場合は真
	 * @throws FormatLimitExceeded 束縛の照会が作業予算を超えた場合
	 * @throws std::bad_alloc 束縛の事実のメモリを確保出来ない場合
	 * @throws std::length_error 束縛の事実の要素数上限を超える場合
	 * @throws std::out_of_range 束縛の照会するノードが範囲外の場合
	 */
	bool SyntaxNormalizer::IsTypeObservedBinding(const SyntaxModel &Syntax, const LexicalPolicy &Policy, const NodeId Name) const {
		// 解決済の参照から型を観測される導入名の判定
		if(Policy.TypeObservedBindings.contains(Name)) return true;
		const std::unordered_map<uint32_t, std::unordered_set<NodeId>>::const_iterator Found =
		Policy.UnresolvedTypeNames.find(Syntax.GetCmpNameIdx(Name));
		// 未解決の照会と同じ綴りの可視の束縛だけを保護する
		return Found != Policy.UnresolvedTypeNames.end() && Found->second.contains(Syntax.GetBindingFact(Name).Scope);
	}

	/**
	 * 分割出来ない位置のレキシカル宣言の不変指定の収集関数
	 * 計算量：対象選択量 S と束縛照会量 Q に対し O(S + Q)
	 * @param Syntax 現在の構文
	 * @param Policy 此の収集で１度確定した束縛の方針
	 * @param CanChangeBinding 動的な名前解決を含まず，不変化を許すか
	 * @param Plan 書換の格納先
	 * @throws FormatLimitExceeded 対象選択又は束縛照会が作業予算を超える場合
	 * @throws std::bad_alloc 候補又は計画のメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectIndivisibleLexicalBindings(
		const SyntaxModel &Syntax,
		const LexicalPolicy &Policy,
		const bool CanChangeBinding,
		RewritePlan &Plan
	) const {
		// 不変化を許さない場合の終了
		if(!CanChangeBinding) return;
		const std::string Immutable(Syntax.GetRules().GetSpelling(Word::ImmutableBinding));
		// 再代入されない束縛の不変の語への置換
		for(const NodeId Node : GetCandidates(Syntax, { Role::CommaDecl, Role::Iteration })) {
			if(GetNormalizationBarrierOf(Syntax, Node).IsProtected) continue;
			const NodeId Parent = Syntax.GetParentOf(Node);
			const bool IsIteration = Syntax.Is(Node, Role::Iteration);
			// 宣言の統合が扱う文列内の宣言と，可変の束縛を持たないノードの読飛
			// モジュールでない最上位の束縛と，最上位でないエクスポート内の束縛の読飛
			if(
				const NodeId Container = Syntax.Is(Parent, Role::Export) ? Syntax.GetParentOf(Parent) : Parent;
				!IsIteration && Syntax.Is(Container, Role::DeclSeq) || !Syntax.HasWordChild(Node, Word::ReassignableBinding) ||
				!Policy.IsModule && Parent == Syntax.GetRoot() ||
				Syntax.Is(Parent, Role::Export) && Syntax.GetParentOf(Parent) != Syntax.GetRoot()
			) continue;
			std::vector<NodeId> Patterns;
			bool CanFreeze = true;
			if(IsIteration) {
				CanFreeze = !Policy.ShouldPreserveBindingTypes;
				Patterns.push_back(Syntax.GetFieldChild(Node, ChildRole::Left));
			} else for(const NodeId Declarator : Syntax.GetChildrenOf(Node)) if(Syntax.IsNamed(Declarator)) {
				CanFreeze &= Syntax.GetFieldChild(Declarator, ChildRole::Val) != NoNode && CanPreserveBindingType(Syntax, Policy, Declarator);
				Patterns.push_back(Syntax.GetFieldChild(Declarator, ChildRole::Name));
			}
			for(const NodeId Pattern : Patterns) {
				const std::vector<NodeId> Names = Syntax.GetBindingNames(Pattern);
				CanFreeze &= !Names.empty();
				for(const NodeId Name : Names) CanFreeze &= !Syntax.HasBindingWrite(Name) && !IsTypeObservedBinding(Syntax, Policy, Name);
			}
			if(CanFreeze && !Patterns.empty()) for(const NodeId Keyword : Syntax.GetChildrenOf(Node)) {
				if(Syntax.IsNamed(Keyword) || !Syntax.IsWord(Keyword, Word::ReassignableBinding)) continue;
				RewriteStep Step { RewriteKind::Reshape, Keyword };
				Step.Inserted = { Immutable };
				Plan.Steps.push_back(std::move(Step));
			}
		}
		// 終了
		return;
	}

	/**
	 * 纏めた宣言の除去の収集関数
	 * 共有する修飾・型のアンカーは，統合先に残る同じ形のノードへ移す
	 * 宣言の根の末尾アンカーは指定の行先へ移す
	 * 共有先の無い部品のアンカーは，移した最初の宣言子の前後へ移す
	 * 計算量：訪れるノード数 V と移したノード数 K に対し期待値 O(V + K)
	 * @param Syntax 構文モデル
	 * @param Node 除く宣言
	 * @param End 宣言の末尾のアンカーの行先
	 * @param Kept 移した宣言子の部品の並び（空でなく，其の部分木は除かない）
	 * @param SharedAnchors 纏めた宣言に残る共有部の前後のアンカー（除くノードから引く）
	 * @param Removal 同じ親の宣言を纏めて除く操作
	 * @param Plan 書換の計画の格納先
	 * @throws FormatLimitExceeded 部品の走査が作業予算を超える場合
	 * @throws std::bad_alloc 移したノードの集合又は書換の計画のメモリを確保出来ない場合
	 * @throws std::length_error 移した部品の集合又は書換計画の要素数上限を超える場合
	 */
	void SyntaxNormalizer::RemoveMergedDecl(
		const SyntaxModel &Syntax,
		const NodeId Node,
		const AnchorTarget End,
		const std::vector<NodeId> &Kept,
		const std::unordered_map<NodeId, AnchorTransfer> &SharedAnchors,
		RewriteStep &Removal,
		RewritePlan &Plan
	) {
		// 残す部分木とアンカーの移譲先の準備
		const AnchorTarget Before { Kept.front(), false }, After { Kept.front(), true };
		Syntax.Charge(Kept.size());
		const std::unordered_set<NodeId> KeptNodes(Kept.begin(), Kept.end());
		Core::SyntaxModel::HasStoppedTraversal(
			Syntax,
			Node,
			[&](const NodeId Sub) -> WalkStep {
				// 移した部分木を読み飛ばす事の返戻
				if(KeptNodes.contains(Sub)) return WalkStep::Skip;
				if(Sub == Node) {
					Removal.RemovedSiblings.push_back({ Sub, Before, End, Before });
					// 子のアンカーを個別に移す為の走査の返戻
					return WalkStep::Descend;
				}
				if(
					const std::unordered_map<NodeId, AnchorTransfer>::const_iterator Shared = SharedAnchors.find(Sub);
					Shared != SharedAnchors.end()
				) {
					Plan.Steps.push_back({ RewriteKind::Remove, Sub, {}, Shared->second });
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
	 * 不変指定と宣言グループの最終形の一括収集関数
	 * 宣言子毎の不変指定と共有指定を求め，同じ連続範囲を１つのアルゴリズムでグループにする
	 * 計算量：構文のノード数 N，候補整列の費用 S，判定の総探索量 Q，生成の総処理量 G に対し期待値 O(N + S + Q + G)
	 * @param Syntax 現在の構文
	 * @param Plan 所有者毎の子列を１度更新する計画
	 * @throws FormatLimitExceeded 作業予算を超える場合，又は必要な生成形式・比較名が未定義の場合
	 * @throws std::bad_alloc 宣言グループ又は生成部品のメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectDeclBatch(const SyntaxModel &Syntax, RewritePlan &Plan) const {
		// 不変指定と宣言グループの最終形の一括収集
		const std::unordered_set<NodeId> Frozen = CollectImmutableBindings(Syntax, Plan);
		const bool HasLexicalKeyword = Syntax.GetRules().HasReassignableLexicalKeyword;
		const LexicalPolicy Policy = HasLexicalKeyword ? GetLexicalPolicy(Syntax) : LexicalPolicy{};
		const bool CanChangeBinding = HasLexicalKeyword && !Policy.IsDynamic;
		if(HasLexicalKeyword) CollectIndivisibleLexicalBindings(Syntax, Policy, CanChangeBinding, Plan);
		// 宣言の雛形と，要素を挿す位置の表現（雛形の根からの経路と子の番号）
		struct FrameSlot {
			std::vector<size_t> Path {}; // 雛形の根から要素を置くノードへの子の番号の経路
			size_t Idx = 0; // 要素を置く子の番号
		};
		// 宣言の雛形の組立（並びのノードは其の中を組み立て，要素と要素の間の `,` は除いて其の位置を控える）
		const auto BuildFrame = [&Syntax](
			const CommaDecl &LocalDecl,
			const std::unordered_map<NodeId, int32_t> &ListOf,
			const NodeId Parent,
			std::vector<size_t> &Path,
			std::vector<FrameSlot> &Slots
		) -> GeneratedNode {
			// 宣言の雛形の１ノードの生成
			const auto MakeFrame = [&Syntax](const NodeId Target) -> GeneratedNode {
				const SyntaxNode &Original = Syntax.GetNode(Target);
				GeneratedNode Output { {}, true, {}, Original.Fields };
				Output.Def = Syntax.GetGenerationPartOf(Target);
				Output.Facts = Original.Facts;
				Output.Src = Target;
				// 宣言の雛形のノードの返戻
				return Output;
			};
			// 部分木の走査の枠
			struct Frame {
				NodeId Parent;
				GeneratedNode *Out; // 生成したノードの格納先
				size_t NextChild = 0;
				bool IsHost = false;
			};
			GeneratedNode Output = MakeFrame(Parent);
			// 共有する子への参照と，要素を挿す位置の記録
			std::vector<Frame> Pending { { Parent, &Output } };
			while(!Pending.empty()) {
				Frame &Cur = Pending.back();
				const std::span<const NodeId> Children = Syntax.GetChildrenOf(Cur.Parent);
				if(Cur.NextChild == Children.size()) {
					if(Cur.IsHost) Path.pop_back();
					Pending.pop_back();
					continue;
				}
				const NodeId Child = Children[Cur.NextChild++];
				Syntax.Charge(1);
				const std::unordered_map<NodeId, int32_t>::const_iterator Found = ListOf.find(Child);
				if(
					const bool IsHost = std::ranges::any_of(
						LocalDecl.Lists,
						[Child](const DeclList &List) -> bool {
							// 並びの所有者かの返戻
							return List.Host == Child;
						}
					); Child != Cur.Parent && IsHost
				) {
					Path.push_back(Cur.Out->Children.size());
					Cur.Out->Children.push_back(MakeFrame(Child));
					Pending.push_back({ Child, &Cur.Out->Children.back(), 0, true });
				} else if(Found == ListOf.end() || Found->second == -1) {
					Cur.Out->Children.push_back({ {}, false, {}, Syntax.GetNode(Child).Fields, Child });
				} else if(Found->second > -1 && Slots[static_cast<size_t>(Found->second)].Idx == SIZE_MAX) {
					Slots[static_cast<size_t>(Found->second)] = { Path, Cur.Out->Children.size() };
				}
			}
			// 組み立てた雛形の返戻
			return Output;
		};
		// 宣言の共有指定と分割する宣言子の情報
		struct Decl {
			CommaDecl Parts; // 共有指定と宣言子の投影済の部品
			NodeId Wrapper = NoNode;
			NodeId Keyword = NoNode;
			NodeId Modifiers = NoNode;
			std::vector<DeclShape> Key {}; // 不変指定を除いた共有指定の比較値
			std::vector<NodeId> InheritedTypes {}; // 後続の明示アノテーションを共有する初期値の無い束縛
			std::vector<NodeId> FollowingTerminators {}; // 宣言の子でなくコンテナの子として続く文区切
			bool IsImmutable = false;
			bool HasDeducedType = false;
		};
		// グループに属する宣言子の原位置
		struct ItemPos {
			size_t Origin; // グループの雛形とする元の宣言の番号
			size_t Item; // 元の宣言の中の宣言子の番号
		};
		// 共有指定が一致する宣言子のグループ
		struct Group {
			size_t Origin; // グループの雛形とする元の宣言の番号
			std::string Keyword; // グループに使う導入語のテキスト
			bool IsImmutable;
			std::vector<ItemPos> Items;
		};
		// 既存の宣言を保ち，不変指定だけを直接追加する収集関数
		// 計算量：生成する指定の長さ L に対し償却 O(L + 1)
		// @param Node 指定を追加する宣言
		// @param Modifiers 既存の修飾子列（無も可）
		// @param Edits 編集の格納先
		// @throws std::bad_alloc 指定又は編集列を確保出来ない場合
		// @throws std::length_error 指定又は編集列の要素数上限を超える場合
		const auto AddImmutableSpecifier =
		[&Syntax](const NodeId Node, const NodeId Modifiers, std::vector<RewriteStep> &Edits) -> void {
			RewriteStep Modifier { RewriteKind::Insert, Modifiers == NoNode ? Node : Modifiers };
			Modifier.Idx = Modifiers == NoNode ? 0 : Syntax.GetChildCount(Modifiers);
			Modifier.Boundary = BoundaryKind::Spaced;
			const std::string Immutable(Syntax.GetRules().GetSpelling(Word::ImmutableBinding));
			if(Modifiers == NoNode) Modifier.Inserted = { {}, true, { { Immutable } }, 0, NoNode, {}, Syntax.GetRules().ModifierListDef };
			else Modifier.Inserted = { Immutable };
			Edits.push_back(std::move(Modifier));
			// 指定の収集の終了
			return;
		};
		// 文列で分割出来ない宣言は，全宣言子の指定が一致する場合だけ共有指定を変える
		for(const NodeId Node : Syntax.GetNodesWith(Role::FinalizableLocal)) {
			if(GetNormalizationBarrierOf(Syntax, Node).IsProtected || Syntax.Is(Syntax.GetParentOf(Node), Role::DeclSeq)) continue;
			bool HasDeclarators = false, AreBindingsImmutable = true;
			for(const NodeId Child : Syntax.GetChildrenOf(Node)) {
				Syntax.Charge(1);
				if(!Syntax.Is(Child, Role::VarDeclaratorItem)) continue;
				HasDeclarators = true;
				AreBindingsImmutable &= Frozen.contains(Child);
			}
			if(!HasDeclarators || !AreBindingsImmutable) continue;
			const NodeId Modifiers = Syntax.GetFirstNamedChildWithRole(Node, Role::ModifierSeq);
			AddImmutableSpecifier(Node, Modifiers, Plan.Steps);
		}
		// 照会するコンテナからの共通祖先だけでローカル所属を継承する
		std::vector<std::optional<bool>> LocalScopes;
		std::vector<NodeId> ScopePath;
		// 共通祖先の結果を共有するローカル所属の判定関数
		// 計算量：未照会の祖先数 H に対し O(H + 1)
		// @param Node 宣言を含むコンテナ
		// @return 最内の所属が関数又はラムダなら真，型又は最上位なら偽
		// @throws FormatLimitExceeded 作業予算を超えた場合
		// @throws std::bad_alloc 経路又は共有表を確保出来ない場合
		// @throws std::length_error 経路又は共有表の要素数上限を超える場合
		const auto IsLocalScope = [&Syntax, &LocalScopes, &ScopePath](const NodeId Node) -> bool {
			if(LocalScopes.empty()) LocalScopes.resize(Syntax.GetNodeCount());
			std::vector<NodeId> &Path = ScopePath;
			Path.clear();
			bool IsLocal = false;
			for(NodeId Up = Node; Up != NoNode; Up = Syntax.GetParentOf(Up)) {
				Syntax.Charge(1);
				if(LocalScopes[Up]) {
					IsLocal = *LocalScopes[Up];
					break;
				}
				Path.push_back(Up);
				if(Syntax.Is(Up, Role::ClassLike) || Syntax.Is(Up, Role::ClassBodyContainer)) break;
				if(Syntax.Is(Up, Role::FunctionScope) || Syntax.Is(Up, Role::FunctionDef) || Syntax.Is(Up, Role::LambdaLiteral)) {
					IsLocal = true;
					break;
				}
			}
			for(const NodeId Part : Path) LocalScopes[Part] = IsLocal;
			// 最内の関数又は型で確定した所属の返戻
			return IsLocal;
		};
		std::vector<NodeId> Containers;
		for(const NodeId DeclNode : Syntax.GetNodesWith(Role::CommaDecl)) {
			Syntax.Charge(1);
			NodeId Parent = Syntax.GetParentOf(DeclNode);
			if(Syntax.Is(Parent, Role::GlobalStmt) || Syntax.Is(Parent, Role::Export)) Parent = Syntax.GetParentOf(Parent);
			if(Parent != NoNode) Containers.push_back(Parent);
		}
		Containers = SortInDocOrder(Syntax, Containers);
		std::vector<NodeId> UnchangedChildren;
		for(const NodeId Container : Containers) {
			UnchangedChildren.clear();
			const NormalizationBarrier &Barrier = GetNormalizationBarrierOf(Syntax, Container);
			if(Barrier.IsProtected || Barrier.DoesRequireInlineCode) continue;
			const bool IsSeq = Syntax.Is(Container, Role::DeclSeq), IsSplitScope = IsSeq && !IsLocalScope(Container);
			const bool CanReplaceTerminators = IsSeq && Syntax.Is(Container, Role::OptionalSemicolonHost);
			const bool CanReplaceEmptyStmts = CanReplaceTerminators && Syntax.Is(Container, Role::EmptySemicolonSeq);
			std::unordered_set<uint32_t> PreservedBoundaries;
			if(CanReplaceTerminators) for(const SemanticBoundary &Boundary : Syntax.GetNode(Container).Facts.Boundaries.GetView()) {
				Syntax.Charge(1);
				if(Boundary.Kind == BoundaryKind::Preserved) PreservedBoundaries.insert(Boundary.Before);
			}
			std::vector<Decl> Decls;
			std::vector<Group> Groups;
			std::vector<GeneratedNode> Children;
			std::vector<RewriteStep> Edits, SeparatorEdits;
			RewriteStep Removal { RewriteKind::RemoveSiblings, Container };
			std::unordered_set<uint32_t> RunNames;
			bool HasChange = false;
			// 組立が必要と確定する迄は既存の子の識別子だけを保持する追加関数
			// 計算量：償却 O(1)
			// @param Node 保持する既存の子
			// @throws std::bad_alloc 子の列を確保出来ない場合
			// @throws std::length_error 子の列の要素数上限を超える場合
			// @throws std::out_of_range 子の識別子がノード表に無い場合
			const auto AppendUnchanged = [&](const NodeId Node) -> void {
				if(HasChange) Children.push_back({ {}, false, {}, Syntax.GetNode(Node).Fields, Node });
				else UnchangedChildren.push_back(Node);
				// 終了
				return;
			};
			// 連続した宣言のグループの確定と組立
			const auto Flush = [&]() -> void {
				// 宣言グループが無い場合の終了
				if(Decls.empty()) return;
				// 宣言子の所属と順が同じなら，子列を組み直さず指定だけを変更する
				bool KeepsItems = Groups.size() == Decls.size(), ChangesSpecifier = false, HasInheritedType = false;
				for(size_t Idx = 0; KeepsItems && Idx < Groups.size(); ++Idx) {
					const Group &Cur = Groups[Idx];
					const Decl &Original = Decls[Idx];
					KeepsItems = Cur.Origin == Idx && Cur.Items.size() == Original.Parts.Lists.front().Items.size();
					ChangesSpecifier |=
					Cur.IsImmutable != Original.IsImmutable || Original.Keyword != NoNode && Cur.Keyword != Syntax.GetLexemeOf(Original.Keyword);
					for(size_t Item = 0; KeepsItems && Item < Cur.Items.size(); ++Item) {
						Syntax.Charge(1);
						KeepsItems = Cur.Items[Item].Origin == Idx && Cur.Items[Item].Item == Item;
					}
					for(const NodeId Inherited : Original.InheritedTypes) {
						Syntax.Charge(1);
						HasInheritedType |= Inherited != NoNode;
					}
				}
				if(KeepsItems && (!ChangesSpecifier || !HasInheritedType)) {
					for(size_t Idx = 0; Idx < Decls.size(); ++Idx) {
						const Decl &Original = Decls[Idx];
						const Group &Cur = Groups[Idx];
						if(Original.Keyword != NoNode && Cur.Keyword != Syntax.GetLexemeOf(Original.Keyword)) {
							RewriteStep Keyword { RewriteKind::Reshape, Original.Keyword };
							Keyword.Inserted = { Cur.Keyword };
							Edits.push_back(std::move(Keyword));
						}
						if(Cur.IsImmutable && !Original.IsImmutable && Original.Keyword == NoNode) {
							AddImmutableSpecifier(Original.Parts.Node, Original.Modifiers, Edits);
						}
						const NodeId Node = Original.Wrapper == NoNode ? Original.Parts.Node : Original.Wrapper;
						AppendUnchanged(Node);
						for(const NodeId Terminator : Original.FollowingTerminators) AppendUnchanged(Terminator);
					}
					Decls.clear();
					Groups.clear();
					RunNames.clear();
					// 指定だけを変え，宣言子の所属と順を保持した場合の終了
					return;
				}
				HasChange = true;
				for(const NodeId Node : UnchangedChildren) Children.push_back({ {}, false, {}, Syntax.GetNode(Node).Fields, Node });
				UnchangedChildren.clear();
				// 宣言の生成木と宣言子の挿入位置
				struct DeclFrame {
					GeneratedNode Tree; // 組み立てた宣言の生成部分木
					std::vector<FrameSlot> Slots; // 宣言子を挿入する雛形内の位置
				};
				DeclFrame Frame;
				std::unordered_set<size_t> Retained;
				std::vector<uint8_t> ShouldKeepTerminators(Decls.size(), 0);
				std::vector<size_t> DstGroups(Decls.size()), FirstDstGroups(Decls.size());
				size_t GroupIdx = 0;
				for(const Group &Cur : Groups) {
					for(const ItemPos &Pos : Cur.Items) {
						DstGroups[Pos.Origin] = GroupIdx;
						if(!Pos.Item) FirstDstGroups[Pos.Origin] = GroupIdx;
					}
					++GroupIdx;
					const Decl &Original = Decls[Cur.Origin];
					const bool IsRetained = Retained.insert(Cur.Origin).second;
					// グループの原点は宣言順に進む為，同じ原点の分割グループだけが直前の雛形を共有する
					if(IsRetained) {
						std::unordered_map<NodeId, int32_t> ListOf;
						for(const DeclPiece &Piece : Original.Parts.Pieces) {
							Syntax.Charge(1);
							ListOf.emplace(Piece.Node, Piece.List);
						}
						for(const DeclList &List : Original.Parts.Lists) for(size_t Item = 1; Item < List.Items.size(); ++Item) {
							Syntax.Charge(1);
							ListOf.emplace(Syntax.GetPrevSibling(List.Items[Item].front()), -2);
						}
						Frame = { {}, std::vector<FrameSlot>(Original.Parts.Lists.size(), { {}, SIZE_MAX }) };
						std::vector<size_t> Path;
						Frame.Tree = BuildFrame(Original.Parts, ListOf, Original.Parts.Node, Path, Frame.Slots);
					}
					if(GroupIdx < Groups.size()) Syntax.Charge(1);
					const bool IsLastForOrigin = GroupIdx == Groups.size() || Groups[GroupIdx].Origin != Cur.Origin;
					GeneratedNode Result = IsLastForOrigin ? std::move(Frame.Tree) : Frame.Tree;
					if(!IsRetained) {
						std::vector<GeneratedNode *> Pending { &Result };
						while(!Pending.empty()) {
							GeneratedNode &Part = *Pending.back();
							Pending.pop_back();
							Syntax.Charge(1);
							if(Part.Existing != NoNode) Part = Syntax.CopyOf(Part.Existing);
							else for(GeneratedNode &Child : Part.Children) Pending.push_back(&Child);
						}
					}
					// 後の並びからの挿入（後の追加に依る挿入位置の変化を防止）
					for(size_t ListIdx = Original.Parts.Lists.size(); ListIdx--;) {
						GeneratedNode *Host = &Result;
						for(const size_t Step : Frame.Slots[ListIdx].Path) Host = &Host->Children[Step];
						std::vector<GeneratedNode> Pieces;
						for(size_t Idx = 0; Idx < Cur.Items.size(); ++Idx) {
							if(Idx) {
								GeneratedNode Separator { Generation::GetGeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::Separator) };
								Separator.Fields = Syntax.GetFieldBit(Original.Parts.Lists[ListIdx].SeparatorField);
								Pieces.push_back(std::move(Separator));
							}
							const ItemPos &Pos = Cur.Items[Idx];
							for(const NodeId Piece : Decls[Pos.Origin].Parts.Lists[ListIdx].Items[Pos.Item]) {
								Pieces.push_back({ {}, false, {}, Syntax.GetNode(Piece).Fields, Piece });
							}
							if(IsSplitScope && Syntax.GetRules().HasBackwardBindingTypeAnnotation) {
								if(const NodeId Inherited = Decls[Pos.Origin].InheritedTypes[Pos.Item]; Inherited != NoNode) {
									Pieces.push_back(Syntax.CopyOf(Inherited));
								}
							}
						}
						Host->Children.insert(
							Host->Children.begin() + static_cast<std::ptrdiff_t>(Frame.Slots[ListIdx].Idx),
							std::make_move_iterator(Pieces.begin()),
							std::make_move_iterator(Pieces.end())
						);
					}
					if(Original.Keyword != NoNode && Cur.Keyword != Syntax.GetLexemeOf(Original.Keyword)) {
						if(IsRetained) {
							RewriteStep Keyword { RewriteKind::Reshape, Original.Keyword };
							Keyword.Inserted = { Cur.Keyword };
							Edits.push_back(std::move(Keyword));
						} else for(GeneratedNode &Child : Result.Children) if(Child.Src == Original.Keyword) {
							GeneratedNode Keyword { Cur.Keyword, false, {}, Child.Fields, NoNode, Cur.Keyword };
							Keyword.Src = Child.Src;
							Keyword.Facts.HasFalseName = Child.Facts.HasFalseName;
							Keyword.Facts.HasTrueName = Child.Facts.HasTrueName;
							Child = std::move(Keyword);
						}
					}
					if(Cur.IsImmutable && !Original.IsImmutable && Original.Keyword == NoNode) {
						if(Original.Modifiers == NoNode) {
							const std::string Immutable(Syntax.GetRules().GetSpelling(Word::ImmutableBinding));
							GeneratedNode Modifiers { {}, true, { { Immutable } } };
							Modifiers.Def = Syntax.GetRules().ModifierListDef;
							Result.Children.insert(Result.Children.begin(), std::move(Modifiers));
						} else if(IsRetained) AddImmutableSpecifier(Original.Parts.Node, Original.Modifiers, Edits);
						else {
							const std::string Immutable(Syntax.GetRules().GetSpelling(Word::ImmutableBinding));
							for(GeneratedNode &Child : Result.Children) if(Child.Src == Original.Modifiers) Child.Children.push_back({ Immutable });
						}
					}
					Syntax.RemapGeneratedBoundaries(Result);
					const NodeId First = Decls[Cur.Items.front().Origin].Parts.Lists.front().Items[Cur.Items.front().Item].front();
					const ItemPos &LastItem = Cur.Items.back();
					const NodeId Last = Decls[LastItem.Origin].Parts.Lists.back().Items[LastItem.Item].back();
					const AnchorTarget Before { First, false }, After { Last, true };
					if(IsRetained) {
						// 識別子と先頭・内部のアンカーを保持する並びの所有者の組直
						std::vector<std::pair<GeneratedNode *, bool>> Pending { { &Result, false } };
						while(!Pending.empty()) {
							const std::pair<GeneratedNode *, bool> CurPart = Pending.back();
							Pending.pop_back();
							GeneratedNode &Part = *CurPart.first;
							Syntax.Charge(1);
							if(Part.Existing != NoNode) continue;
							if(!CurPart.second) {
								// `Src` はコピーの由来も表す為，雛形の宣言と並びの所有者だけを組直の対象にする
								if(
									Part.Src != Original.Parts.Node && std::ranges::none_of(
										Original.Parts.Lists,
										[&Syntax, &Part](const DeclList &List) -> bool {
											Syntax.Charge(1);
											// 元の並びの所有者かの返戻
											return List.Host == Part.Src;
										}
									)
								) continue;
								Pending.emplace_back(&Part, true);
								for(GeneratedNode &Child : Part.Children) Pending.emplace_back(&Child, false);
								continue;
							}
							if(Part.Src == Original.Parts.Node) continue;
							const NodeId Host = Part.Src;
							RewriteStep ReshapeHost { RewriteKind::Reshape, Host, {}, { NoNode, Before, After, Before } };
							ReshapeHost.Inserted = std::move(Part);
							Part = { {}, false, {}, ReshapeHost.Inserted.Fields, Host };
							Edits.push_back(std::move(ReshapeHost));
						}
						RewriteStep Reshape { RewriteKind::Reshape, Original.Parts.Node, {}, { NoNode, Before, After, Before } };
						Reshape.Inserted = std::move(Result);
						Edits.push_back(std::move(Reshape));
						const NodeId Node = Original.Wrapper == NoNode ? Original.Parts.Node : Original.Wrapper;
						AppendUnchanged(Node);
					} else {
						if(Original.Wrapper != NoNode) {
							GeneratedNode Wrapped { {}, true };
							Wrapped.Def = Syntax.GetGenerationPartOf(Original.Wrapper);
							Wrapped.Facts = Syntax.GetNode(Original.Wrapper).Facts;
							Wrapped.Src = Original.Wrapper;
							for(const NodeId Child : Syntax.GetChildrenOf(Original.Wrapper)) {
								Syntax.Charge(1);
								if(Child == Original.Parts.Node) Wrapped.Children.push_back(std::move(Result));
								else Wrapped.Children.push_back(Syntax.CopyOf(Child));
							}
							Syntax.RemapGeneratedBoundaries(Wrapped);
							Result = std::move(Wrapped);
						}
						Children.push_back(std::move(Result));
					}
					const Decl &LastDecl = Decls[LastItem.Origin];
					ShouldKeepTerminators[LastItem.Origin] = LastItem.Item + 1 == LastDecl.Parts.Lists.front().Items.size();
					if(ShouldKeepTerminators[LastItem.Origin]) for(const NodeId Terminator : LastDecl.FollowingTerminators) {
						AppendUnchanged(Terminator);
					}
				}
				for(size_t Idx = 0; Idx < Decls.size(); ++Idx) {
					const Decl &Original = Decls[Idx];
					const AnchorTarget Before { Original.Parts.Lists.front().Items.front().front(), false };
					const AnchorTarget After { Original.Parts.Lists.back().Items.back().back(), true };
					const NodeId Node = Original.Wrapper == NoNode ? Original.Parts.Node : Original.Wrapper;
					if(Retained.contains(Idx)) {
						const bool HasOriginalStart = Groups[FirstDstGroups[Idx]].Origin == Idx;
						const AnchorTarget Start = HasOriginalStart ? AnchorTarget{ Node, false } : Before;
						Edits.push_back({ RewriteKind::Retarget, Node, {}, { Node, Start, After, Start } });
						if(Original.Wrapper != NoNode) {
							const AnchorTarget DeclStart = HasOriginalStart ? AnchorTarget{ Original.Parts.Node, false } : Before;
							Edits.push_back({ RewriteKind::Retarget, Original.Parts.Node, {}, { Original.Parts.Node, DeclStart, After, DeclStart } });
						}
					}
					for(const DeclList &List : Original.Parts.Lists) {
						RewriteStep Separators { RewriteKind::RemoveSiblings, List.Host };
						for(size_t Item = 1; Item < List.Items.size(); ++Item) {
							const AnchorTarget Prev { List.Items[Item - 1].back(), true };
							Separators.RemovedSiblings.push_back({ Syntax.GetPrevSibling(List.Items[Item].front()), Prev, Prev, Prev });
						}
						if(Retained.contains(Idx) && !Separators.RemovedSiblings.empty()) SeparatorEdits.push_back(std::move(Separators));
					}
					if(!Retained.contains(Idx)) {
						std::vector<NodeId> Kept;
						for(const DeclPiece &Piece : Original.Parts.Pieces) if(Piece.List > -1) Kept.push_back(Piece.Node);
						const Group *const Dst = &Groups[DstGroups[Idx]];
						const Decl &First = Decls[Dst->Origin];
						std::unordered_map<const KindDef *, std::vector<NodeId>> Shared;
						for(const DeclPiece &Piece : First.Parts.Pieces) if(Piece.List < 0 && Syntax.IsNamed(Piece.Node)) {
							Shared[Syntax.GetNode(Piece.Node).Kind.Def].push_back(Piece.Node);
						}
						std::unordered_map<const KindDef *, size_t> Positions;
						std::unordered_map<NodeId, AnchorTransfer> SharedAnchors;
						std::vector<std::pair<NodeId, NodeId>> Pending;
						for(const DeclPiece &Piece : Original.Parts.Pieces) {
							if(Piece.List < 0 && Syntax.IsNamed(Piece.Node)) {
								const KindDef *const Kind = Syntax.GetNode(Piece.Node).Kind.Def;
								if(
									const std::unordered_map<const KindDef *, std::vector<NodeId>>::const_iterator Found = Shared.find(Kind);
									Found != Shared.end()
								) Pending.emplace_back(Piece.Node, Found->second.at(Positions[Kind]++));
							}
						}
						while(!Pending.empty()) {
							const std::pair<NodeId, NodeId> Pair = Pending.back();
							Pending.pop_back();
							Syntax.Charge(1);
							const AnchorTarget Leading { Pair.second, false }, Trailing { Pair.second, true };
							SharedAnchors.emplace(Pair.first, AnchorTransfer{ Pair.first, Leading, Trailing, Leading });
							const bool IsModifiers = Syntax.Is(Pair.first, Role::ModifierSeq);
							uint32_t DstChild = 0;
							for(const NodeId Child : Syntax.GetChildrenOf(Pair.first)) {
								Syntax.Charge(1);
								if(IsModifiers && Syntax.IsWord(Child, Word::ImmutableBinding)) continue;
								while(
									IsModifiers && DstChild < Syntax.GetChildCount(Pair.second) &&
									Syntax.IsWord(Syntax.GetChild(Pair.second, DstChild), Word::ImmutableBinding)
								) ++DstChild;
								Pending.emplace_back(Child, Syntax.GetChild(Pair.second, DstChild++));
							}
						}
						// 不変指定は共有キーから除いて比較する為，其のトークンのアンカーは最終指定の境界へ対応する
						if(Original.IsImmutable && Original.Modifiers != NoNode) {
							AnchorTarget Leading { First.Parts.Node, false };
							AnchorTarget Trailing { Syntax.GetFieldChild(First.Parts.Node, ChildRole::Type), false };
							if(First.Modifiers == NoNode) {
								SharedAnchors.emplace(Original.Modifiers, AnchorTransfer{ Original.Modifiers, Leading, Trailing, Leading });
							} else if(!First.IsImmutable) {
								Leading = { Syntax.GetChildrenOf(First.Modifiers).back(), true };
								Trailing = { First.Modifiers, true };
							} else for(const NodeId Child : Syntax.GetChildrenOf(First.Modifiers)) {
								Syntax.Charge(1);
								if(Syntax.IsWord(Child, Word::ImmutableBinding)) {
									Leading = { Child, false };
									Trailing = { Child, true };
									break;
								}
							}
							for(const NodeId Child : Syntax.GetChildrenOf(Original.Modifiers)) {
								Syntax.Charge(1);
								if(Syntax.IsWord(Child, Word::ImmutableBinding)) {
									SharedAnchors.emplace(Child, AnchorTransfer{ Child, Leading, Trailing, Leading });
								}
							}
						}
						const AnchorTarget End = Dst->Items.back().Origin == Idx ? AnchorTarget{ First.Parts.Node, true } : After;
						RemoveMergedDecl(Syntax, Original.Parts.Node, End, Kept, SharedAnchors, Removal, Plan);
						if(Original.Wrapper != NoNode) Removal.RemovedSiblings.back().From = Original.Wrapper;
					}
					// 保持する宣言子へのアンカーの直接移譲（移譲済の宣言の根は不経由）
					// 文区切の直前の末尾への対応付と空文の後続の先頭への対応付
					if(!ShouldKeepTerminators[Idx]) for(const NodeId Terminator : Original.FollowingTerminators) {
						Syntax.Charge(1);
						const AnchorTarget Dst =
						Syntax.IsNamed(Terminator) ? AnchorTarget{ Decls[Idx + 1].Parts.Lists.front().Items.front().front(), false } : After;
						Removal.RemovedSiblings.push_back({ Terminator, Dst, Dst, Dst });
					}
				}
				Decls.clear();
				Groups.clear();
				RunNames.clear();
				// 終了
				return;
			};
			for(const NodeId Child : Syntax.GetChildrenOf(Container)) {
				// 改行で代えれる文区切は前の宣言に属し，宣言の連続を切らない
				if(
					CanReplaceTerminators && !Decls.empty() && Syntax.IsWord(Child, Word::StmtEnd) &&
					(!Syntax.IsNamed(Child) || CanReplaceEmptyStmts && Syntax.Is(Child, Role::EmptyStmt)) &&
					!PreservedBoundaries.contains(Syntax.GetNode(Child).Idx) && !PreservedBoundaries.contains(Syntax.GetNode(Child).Idx + 1) &&
					!Syntax.Is(Syntax.GetNextSibling(Child), Role::Error)
				) {
					Syntax.Charge(1);
					Decls.back().FollowingTerminators.push_back(Child);
					continue;
				}
				const NodeId Wrapper = Syntax.Is(Child, Role::GlobalStmt) || Syntax.Is(Child, Role::Export) ? Child : NoNode;
				const NodeId Node = Wrapper == NoNode ? Child : Syntax.GetNamedChild(Child, 0);
				std::optional<CommaDecl> Parts = Syntax.GetCommaDeclOf(Node, IsSeq);
				if(!Parts) {
					if(!IsSeq) continue;
					Flush();
					AppendUnchanged(Child);
					continue;
				}
				// 部品の収集時に確定した末尾の所属を使い，並び毎の逆探索を繰り返さない
				for(const DeclList &List : Parts->Lists) {
					if(List.Host == Parts->Node || List.LastPiece == SIZE_MAX) continue;
					const DeclPiece &Last = Parts->Pieces[List.LastPiece];
					if(Last.List > -1) {
						const AnchorTarget Own { List.Host, false }, Tail { Last.Node, true };
						Plan.Steps.push_back({ RewriteKind::Retarget, List.Host, {}, { List.Host, Own, Tail, Own } });
					}
				}
				// 見出の宣言の並びのアンカーの整理（宣言子毎には分離しない）
				if(!IsSeq) continue;
				Decl Original { std::move(*Parts), Wrapper };
				if(IsSplitScope && Syntax.GetRules().HasBackwardBindingTypeAnnotation) {
					Original.InheritedTypes.resize(Original.Parts.Lists.front().Items.size(), NoNode);
					NodeId FollowingType = NoNode;
					for(size_t Item = Original.Parts.Lists.front().Items.size(); Item--;) {
						NodeId Annotation = NoNode;
						bool HasVal = false;
						for(const NodeId Piece : Original.Parts.Lists.front().Items[Item]) {
							Syntax.Charge(1);
							if(Syntax.Is(Piece, Role::TypeAnnotation)) Annotation = Piece;
							HasVal |= Syntax.HasField(Piece, ChildRole::Val);
						}
						if(Annotation != NoNode) FollowingType = Annotation;
						else if(HasVal) FollowingType = NoNode;
						else Original.InheritedTypes[Item] = FollowingType;
					}
				}
				Original.Modifiers = Syntax.GetFirstNamedChildWithRole(Node, Role::ModifierSeq);
				Original.IsImmutable = Syntax.HasWordChild(Original.Modifiers, Word::ImmutableBinding);
				if(
					HasLexicalKeyword && (Syntax.HasWordChild(Node, Word::ReassignableBinding) || Syntax.HasWordChild(Node, Word::ImmutableBinding))
				) {
					Original.Keyword = Syntax.GetChild(Node, 0);
					Original.IsImmutable = Syntax.IsWord(Original.Keyword, Word::ImmutableBinding);
				}
				// 比較キーを移し，同じ列の中で不変指定を除く
				Original.Key = std::move(Original.Parts.Key);
				size_t KeyCnt = 0;
				for(DeclShape &Shape : Original.Key) {
					if(
						!Shape.Kind->IsNamed && (
							Original.Keyword != NoNode && Shape.Meanings.test(static_cast<size_t>(Word::ReassignableBinding)) ||
							Original.IsImmutable && Shape.Meanings.test(static_cast<size_t>(Word::ImmutableBinding))
						)
					) continue;
					if(Shape.Kind->Roles.test(static_cast<size_t>(Role::ModifierSeq)) && Original.IsImmutable) {
						if(Shape.Children == 1) continue;
						--Shape.Children;
					}
					if(&Shape != &Original.Key[KeyCnt]) Original.Key[KeyCnt] = std::move(Shape);
					++KeyCnt;
				}
				Original.Key.resize(KeyCnt);
				NodeId Type = Syntax.GetFieldChild(Node, ChildRole::Type);
				while(Syntax.GetFieldChild(Type, ChildRole::Name) != NoNode && Syntax.GetFieldChild(Type, ChildRole::Args) == NoNode) {
					Type = Syntax.GetFieldChild(Type, ChildRole::Name);
				}
				Original.HasDeducedType = Syntax.Is(Type, Role::DeducedType) && std::ranges::any_of(
					Original.Parts.Lists.front().Items,
					[&Syntax](const std::vector<NodeId> &Item) -> bool {
						// 初期値毎の推論の必要の返戻
						return Syntax.GetFieldChild(Item.front(), ChildRole::Val) != NoNode;
					}
				);
				if(
					!Decls.empty() && (
						Original.Key != Decls.back().Key || Original.Parts.Lists.size() != Decls.back().Parts.Lists.size() || Original.HasDeducedType ||
						Decls.back().HasDeducedType || Syntax.GetNode(Node).Facts.CanReassignExistingBinding ||
						Syntax.GetNode(Decls.back().Parts.Node).Facts.CanReassignExistingBinding
					)
				) Flush();
				if(Original.Parts.Lists.size() > 1) {
					bool HasDependency = false;
					for(const std::vector<NodeId> &Name : Original.Parts.Lists.front().Items) {
						HasDependency |=
						!Syntax.GetNode(Name.front()).Facts.DoesDiscardBinding && RunNames.contains(Syntax.GetCmpNameIdx(Name.front()));
					}
					if(!HasDependency) {
						HasDependency = Core::SyntaxModel::HasNodeMatching(
							Syntax,
							Original.Parts.Lists[1].Host,
							[&](const NodeId Sub) -> bool {
								// 前の名前への参照かの返戻
								return Syntax.Is(Sub, Role::Identifier) && RunNames.contains(Syntax.GetCmpNameIdx(Sub));
							}
						);
					}
					if(HasDependency) Flush();
					for(const std::vector<NodeId> &Name : Original.Parts.Lists.front().Items) {
						if(!Syntax.GetNode(Name.front()).Facts.DoesDiscardBinding) RunNames.insert(Syntax.GetCmpNameIdx(Name.front()));
					}
				}
				const size_t Origin = Decls.size();
				Decls.push_back(std::move(Original));
				const Decl &Added = Decls.back();
				for(size_t Item = 0; Item < Added.Parts.Lists.front().Items.size(); ++Item) {
					const std::vector<NodeId> &Pieces = Added.Parts.Lists.front().Items[Item];
					bool IsImmutable = Added.IsImmutable || Frozen.contains(Pieces.front());
					if(
						CanChangeBinding && (Policy.IsModule || Container != Syntax.GetRoot()) &&
						(!Syntax.Is(Added.Wrapper, Role::Export) || Container == Syntax.GetRoot()) && Added.Keyword != NoNode &&
						Syntax.IsWord(Added.Keyword, Word::ReassignableBinding) && Syntax.GetFieldChild(Pieces.front(), ChildRole::Val) != NoNode
					) {
						const std::vector<NodeId> Names = Syntax.GetBindingNames(Syntax.GetFieldChild(Pieces.front(), ChildRole::Name));
						bool CanFreeze = !Names.empty() && CanPreserveBindingType(Syntax, Policy, Pieces.front());
						for(const NodeId Name : Names) CanFreeze &= !Syntax.HasBindingWrite(Name) && !IsTypeObservedBinding(Syntax, Policy, Name);
						IsImmutable |= CanFreeze;
					}
					if(
						const std::string Keyword = Added.Keyword == NoNode ? std::string() : IsImmutable ?
						std::string(Syntax.GetRules().GetSpelling(Word::ImmutableBinding)) :
						std::string(Syntax.GetLexemeOf(Added.Keyword));
						Groups.empty() || IsSplitScope || Groups.back().IsImmutable != IsImmutable || Groups.back().Keyword != Keyword
					) Groups.push_back({ Origin, Keyword, IsImmutable, {} });
					Groups.back().Items.push_back({ Origin, Item });
				}
			}
			Flush();
			for(RewriteStep &Edit : SeparatorEdits) Plan.Steps.push_back(std::move(Edit));
			for(RewriteStep &Edit : Edits) Plan.Steps.push_back(std::move(Edit));
			if(!HasChange) continue;
			if(!Removal.RemovedSiblings.empty()) Plan.Steps.push_back(std::move(Removal));
			RewriteStep Assemble { RewriteKind::RebuildChildren, Container };
			Assemble.Inserted.Children = std::move(Children);
			Assemble.Inserted.Src = Container;
			Syntax.RemapGeneratedBoundaries(Assemble.Inserted);
			Plan.Steps.push_back(std::move(Assemble));
		}
		// 終了
		return;
	}
} // namespace Core
