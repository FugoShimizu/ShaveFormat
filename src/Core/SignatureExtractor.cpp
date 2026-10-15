#include "SignatureExtractor.hpp"
#include "ControlFlow.hpp"
#include "SyntaxModel.hpp"
#include "SyntaxRole.hpp"
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>

namespace Core {
	/**
	 * 前置の属性を含むノードの頭の取得関数（整形仕様 7.4, 7.6）
	 * ノードに先行する連続した属性・デコレータの内，最初のノードを返戻する
	 * 先行する属性・デコレータが無ければ，指定のノード自身を返戻する
	 * 計算量：定義の前に付く接頭ノード数 N に対し O(N)
	 * @param Syntax 読取専用の構文
	 * @param Node ノード
	 * @return 前置の属性を含む頭のノード
	 * @throws FormatLimitExceeded 構文の照会・走査が作業予算を超える場合
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 */
	NodeId SignatureExtractor::GetLeadOf(const SyntaxModel &Syntax, const NodeId Node) {
		// 前置の属性を含む頭の探索
		NodeId Lead = Node;
		for(
			NodeId Prev = Syntax.GetPrevNamedSibling(Lead);
			Prev != NoNode && Syntax.Is(Prev, Role::AttachPrefix);
			Prev = Syntax.GetPrevNamedSibling(Prev)
		) Lead = Prev;
		// 前置の属性を含む頭の返戻
		return Lead;
	}

	/**
	 * ドキュメントのアンカーの取得関数（整形仕様 7.4）
	 * 計算量：前置の属性とラッパーの判定で読むノード数 N に対し O(N + 1)
	 * @param Syntax 抽出する構文
	 * @param FunctionNode 関数のノード
	 * @return ドキュメントのアンカーとするラッパー，本体の文の並び，又は前置の属性を含む定義の先頭ノード
	 * @throws FormatLimitExceeded 構文の照会・走査が作業予算を超える場合
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 */
	NodeId SignatureExtractor::GetDocAnchor(const SyntaxModel &Syntax, const NodeId FunctionNode) {
		// 定義の親の取得
		const NodeId Parent = Syntax.GetParentOf(FunctionNode);
		// 親の無い関数自身の返戻
		if(Parent == NoNode) return FunctionNode;
		// 宣言を包む型仮引数・書出・フレンド等の外側をドキュメントの対象とする
		if(Syntax.Is(Parent, Role::DocWrapper)) {
			NodeId Anchor = Parent;
			while(true) {
				const NodeId Outer = Syntax.GetParentOf(Anchor);
				if(!Syntax.Is(Outer, Role::DocWrapper)) break;
				Anchor = Outer;
			}
			// 宣言全体を包むアンカーの返戻
			return Anchor;
		}
		// Ruby の先頭のメソッドのドキュメントは，外へ巻き上げるパーサーに合わせて本体の文の並びから採る事の返戻
		if(Syntax.Is(Parent, Role::HoistedDocBody) && Syntax.GetNamedChild(Parent, 0) == FunctionNode) return Parent;
		// Rust の属性と TypeScript のメンバの装飾子は定義の前の兄弟に為る為，ドキュメントは最初の装飾の前に在る事の返戻
		return GetLeadOf(Syntax, FunctionNode);
	}

	/**
	 * ドキュメントを付ける定義かの判定関数（整形仕様 7.4）
	 * ドキュメントを補う工程と其の有無を検査する工程が同じ定義を見る為，選方を此処だけで定める
	 * 計算量：本体の子と `GetDocAnchor` が調べる先行兄弟の延数 A に対し O(A + 1)
	 * @param Syntax 抽出する構文
	 * @param Node ノード
	 * @param Anchor 同じ定義から求めたアンカー
	 * @return 名前付の関数の定義が本体を持ち，ドキュメントを付けれる位置に在れば `true`（ドキュメンテーション文字列を使う形式は対象外）
	 * @throws FormatLimitExceeded 構文の照会・走査が作業予算を超える場合
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 */
	bool SignatureExtractor::IsDocumentable(const SyntaxModel &Syntax, const NodeId Node, const NodeId Anchor) {
		// 名前付の定義と実装本体の有無の判定
		if(
			Syntax.GetRules().Notation.Doc == DocStyle::Docstring || !Syntax.IsNamed(Node) || !Syntax.Is(Node, Role::DocumentedDef) ||
			!Syntax.Is(Node, Role::InlineDefBody) && !Core::SyntaxModel::HasChildMatching(
				Syntax,
				Node,
				[&Syntax](const NodeId Child) -> bool {
					// 実装の本体の子かの返戻
					return Syntax.Is(Child, Role::DefBody);
				}
			)
		// ドキュメントを付ける対象でないノード又は本体を持たない宣言である事の返戻
		) return false;
		const NodeId Target = Anchor;
		// 文・メンバの並びに在る名前付の定義かの返戻（整形仕様 7.4 は最終テキストの行頭での判断を禁じる為，構文の親で決める）
		return Syntax.Is(Target, Role::DocHostSeq) || Syntax.Is(Syntax.GetParentOf(Target), Role::DocHostSeq) ||
		Syntax.Is(Node, Role::MethodDef) && Syntax.Is(Syntax.GetParentOf(Target), Role::ObjectVal);
	}

	/**
	 * 同じ構文の定義の判定とアンカーを共有する取得関数
	 * 親の種別・子列と，定義自身の種別・直接の本体の子を依存範囲とする
	 * 計算量：初回はアンカーと本体の照会費用 A に対し O(A + 1)，再利用は期待 O(1)
	 * @param Syntax 判定を所有する構文
	 * @param Node 定義のノード
	 * @return コメントの有無に依らないアンカーとドキュメント化の判定
	 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
	 * @throws std::bad_alloc 判定の表を確保出来ない場合
	 * @throws std::length_error 判定の表の要素数が格納先の上限を超える場合
	 * @throws std::out_of_range ノードが構文の表に無い場合
	 */
	DocumentationFact SignatureExtractor::GetDocumentationFact(const SyntaxModel &Syntax, const NodeId Node) {
		// 定義以外は投影済の役割だけで決まり，表を要しない
		if(Syntax.GetRules().Notation.Doc == DocStyle::Docstring || !Syntax.Is(Node, Role::DocumentedDef)) return {};
		#if !defined(ShaveFormatRefFacts)
		std::unordered_map<NodeId, DocumentationFact> &Facts = Syntax.Documentation[Syntax.GetParentOf(Node)];
		if(const std::unordered_map<NodeId, DocumentationFact>::const_iterator Found = Facts.find(Node); Found != Facts.end()) {
			Syntax.Charge(1);
			// 同じ依存範囲の確定済の判定の返戻
			return Found->second;
		}
		#endif
		const NodeId Anchor = GetDocAnchor(Syntax, Node);
		const DocumentationFact Fact { Anchor, IsDocumentable(Syntax, Node, Anchor) };
		#if !defined(ShaveFormatRefFacts)
		Facts.emplace(Node, Fact);
		#endif
		// 此の構文から独立に求めた判定の返戻
		return Fact;
	}

	/**
	 * シグネチャの抽出関数（整形仕様 7.4）
	 * 計算量：宣言子の深さ D，ドキュメント化の対象判定・仮引数と戻値の抽出の総費用 Q に対し O(D + Q)
	 * @param Syntax 抽出する構文
	 * @param Flow 同じ構文モデルの戻値と制御移動の共有事実
	 * @param FunctionNode 関数のノード
	 * @return 関数名・仮引数の構造と導入名・呼出結果の有無・テンプレート及びジェネリックの仮引数名
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws FormatLimitExceeded 構文の照会・走査が作業予算を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 * @throws FormatterDefect 導入名の比較用の名前が投影されて居ない場合
	 */
	Signature SignatureExtractor::Extract(const SyntaxModel &Syntax, const ControlFlow &Flow, const NodeId FunctionNode) {
		// シグネチャの抽出結果の準備
		Signature Result;
		const bool IsDeclSig = Syntax.Is(FunctionNode, Role::DeclaratorSig);
		const bool HasSplitParams = Syntax.Is(FunctionNode, Role::SeparateParamParts);
		// 仮引数のコンテナ（C / C++ は名前に最も近い関数宣言子の物，Swift は関数の直下，其の他は仮引数のフィールドか最後の仮引数のコンテナ）
		NodeId Container = NoNode;
		if(IsDeclSig) {
			for(
				NodeId Declarator = Flow.GetNextDeclarator(FunctionNode);
				Declarator != NoNode;
				Declarator = Flow.GetNextDeclarator(Declarator)
			) if(Syntax.Is(Declarator, Role::FunctionDeclarator)) Container = Syntax.GetFieldChild(Declarator, ChildRole::Params);
		} else if(Syntax.Is(FunctionNode, Role::DirectParamSig)) Container = FunctionNode;
		else if(Container = Syntax.GetFieldChild(FunctionNode, ChildRole::Params); Container == NoNode) {
			Core::SyntaxModel::ForEachNamedChildOf(
				Syntax,
				FunctionNode,
				[&Syntax, &Container](const NodeId Child) -> void {
					// 最後に見付けた仮引数のコンテナの保持
					if(Syntax.Is(Child, Role::ParamContainerLike)) Container = Child;
					// 終了
					return;
				}
			);
		}
		// 固定の供給契約が所有する型から導入する暗黙の仮引数も，共通の束縛の取得を使う
		if(Container == NoNode) {
			NodeId ParamOwner = NoNode;
			Container = Syntax.GetBindingParams(FunctionNode, ParamOwner);
		}
		uint32_t ParamOrdinal = 0;
		bool HasPendingRest = false;
		// 投影済の束縛と役割に依る仮引数名の抽出
		Core::SyntaxModel::ForEachChildOf(
			Syntax,
			Container,
			[&](const NodeId Entry) -> void {
				// 独立した可変長の指定と，型・区切を仮引数の名前と区別
				if(Syntax.Is(Entry, Role::RestParamPart) && !Syntax.IsNamed(Entry)) HasPendingRest = true;
				if(Syntax.Is(Entry, Role::ParamModifierList)) {
					HasPendingRest = HasPendingRest || Core::SyntaxModel::HasNodeMatching(
						Syntax,
						Entry,
						[&Syntax](const NodeId Part) -> bool {
							// 仮引数の可変長指定の返戻
							return Syntax.Is(Part, Role::RestParamPart);
						}
					);
				}
				// シグネチャに属する仮引数の選別
				if(
					!Syntax.IsNamed(Entry) || Syntax.Is(Entry, Role::DeclTypeChild) || HasSplitParams && !Syntax.Is(Entry, Role::Param) ||
					Syntax.Is(Entry, Role::AttachPrefix) || Syntax.Is(Entry, Role::Comment) ||
					IsDeclSig && (Syntax.IsWord(Entry, Word::VoidType) || Syntax.IsWord(Entry, Word::VariadicMarker))
				// 仮引数でない子を読み飛ばす場合の終了
				) return;
				++ParamOrdinal;
				// 分解パターンを持つ仮引数かの判定
				bool IsDestructure = Syntax.Is(Entry, Role::DestructurePattern) || Core::SyntaxModel::HasChildMatching(
					Syntax,
					Entry,
					[&Syntax](const NodeId Child) -> bool {
						// 名前付の分割代入のパターンの子かの返戻
						return Syntax.IsNamed(Child) && Syntax.Is(Child, Role::DestructurePattern);
					}
				);
				// Rust の組・構造体・`_` のパターンで束縛する仮引数も単一の名前を持たない
				if(
					const NodeId Pattern = Syntax.Is(Entry, Role::PatternParam) ? Syntax.GetFieldChild(Entry, ChildRole::Pattern) : NoNode;
					Pattern != NoNode
				) IsDestructure = !Syntax.Is(Pattern, Role::Identifier) && !Syntax.Is(Pattern, Role::SelfVal);
				ParamShape Shape { Entry, ParamOrdinal };
				Shape.NameBegin = Result.Params.size();
				Shape.IsDestructure = IsDestructure;
				Shape.HasRest = HasPendingRest;
				HasPendingRest = false;
				// 束縛の収集と名前の補助探索で共有する，既定値・型・装飾の探索打切
				const auto IsExcludedParamPart = [&](const NodeId Node) -> bool {
					// 仮引数自身を除外しない事の返戻
					if(Node == Entry) return false;
					// 入子の既定値も，投影済の束縛パターンの右辺として区別
					const NodeId PatternParent = Syntax.GetParentOf(Node);
					const KindDef *const Def = PatternParent == NoNode ? nullptr : Syntax.GetNode(PatternParent).Kind.Def;
					// シグネチャの束縛に属さない部分かの返戻
					return Syntax.Is(Node, Role::AttachPrefix) || Syntax.Is(Node, Role::DeclTypeChild) || Syntax.Is(Node, Role::ParamContainer) ||
					Syntax.Is(Node, Role::FunctionScope) || Node == Syntax.GetFieldChild(Entry, ChildRole::Val) ||
					Def && Def->Binding == BindingSyntax::DefaultPattern && Syntax.HasField(Node, ChildRole::Right);
				};
				// 属性・型・既定値の内部を仮引数の束縛と取り違えない構造の収集
				Core::SyntaxModel::HasStoppedTraversal(
					Syntax,
					Entry,
					[&](const NodeId Id) -> WalkStep {
						// シグネチャの値と装飾の探索打切の返戻
						if(IsExcludedParamPart(Id)) return WalkStep::Skip;
						if(!Shape.HasRest) Shape.HasRest = Syntax.Is(Id, Role::RestParamPart);
						if(Syntax.IsNamed(Id) && !Syntax.GetNamedChildCount(Id)) {
							const BindingIntroduction Binding = Syntax.GetBindingIntroduction(Id);
							// 仮引数の構造に属する値の導入名を，単一の供給元を持たない分解も含めて記録
							if(Binding.IsDecl && Binding.Namespace == BindingNamespace::Val) {
								Shape.Bindings.push_back(Id);
								Shape.IsUncertain = Shape.IsUncertain || Binding.IsUncertain;
							}
						}
						// 次の構造の探索の返戻
						return WalkStep::Descend;
					}
				);
				if(IsDestructure) {
					Result.HasUnnamedParam = true;
					Shape.IsUncertain = Shape.IsUncertain || Shape.Bindings.empty();
					for(const NodeId Binding : Shape.Bindings) Result.Params.push_back(Syntax.GetCmpName(Binding));
					Result.ParamShapes.push_back(std::move(Shape));
					// 分解の構造と導入名の記録の終了
					return;
				}
				NodeId Found = Shape.Bindings.empty() ? NoNode : Shape.Bindings.front();
				if(Found == NoNode) {
					// 仮引数名の照合関数
					const auto IsName = [&](const NodeId Node) -> bool {
						// メンバポインタの名前
						const NodeId Name = Syntax.Is(Node, Role::PtrTypeDeclarator) ? Syntax.GetFieldChild(Node, ChildRole::Declarator) : NoNode;
						const bool IsMemberName = Syntax.Is(Name, Role::TypeName);
						// 仮引数名の役割を持たないノードの返戻
						if(!Syntax.Is(Node, Role::ParamName) && !IsMemberName) return false;
						Found = IsMemberName ? Name : Node;
						// 最初の発見で打ち切る事の返戻
						return true;
					};
					// C / C++ は宣言子のフィールドを優先し，無ければ型の部位を除いて探す
					if(const NodeId Decl = IsDeclSig ? Syntax.GetFieldChild(Entry, ChildRole::Declarator) : NoNode; Decl != NoNode) {
						Core::SyntaxModel::HasNodeMatching(Syntax, Decl, IsName);
					} else if(IsDeclSig) {
						Core::SyntaxModel::ForEachNamedChildOf(
							Syntax,
							Entry,
							[&](const NodeId Child) -> void {
								// 型の部位を除く仮引数名の探索
								if(Found == NoNode && !Syntax.Is(Child, Role::DeclTypeChild)) Core::SyntaxModel::HasNodeMatching(Syntax, Child, IsName);
								// 終了
								return;
							}
						);
					// 名前のフィールド（C# ･ Swift の型名と外部名を仮引数名と取り違えない）と仮引数自身の照合
					} else if(
						const NodeId Named = Syntax.GetFieldChild(Entry, ChildRole::Name);
						(Named == NoNode || !IsName(Named)) && !IsName(Entry)
					) {
						// アノテーション・デコレータ・属性（`@PathVariable("id") long id` 等）の中の名前は仮引数名でない為，降りない
						Core::SyntaxModel::HasStoppedTraversal(
							Syntax,
							Entry,
							[&](const NodeId Node) -> WalkStep {
								// 値・型・装飾の中へ降りない事の返戻
								if(IsExcludedParamPart(Node)) return WalkStep::Skip;
								// 仮引数名で止める事の返戻
								if(Node != Entry && IsName(Node)) return WalkStep::Stop;
								// 次の構造の探索の返戻
								return WalkStep::Descend;
							}
						);
					}
				}
				// 見付けた仮引数名の記録
				if(Found != NoNode) Result.Params.push_back(Syntax.GetCmpName(Found));
				if(Found != NoNode && Shape.Bindings.empty()) Shape.Bindings.push_back(Found);
				if(Shape.Bindings.size() > 1) {
					// 分解でない複数名の宣言では，各導入名を１仮引数として数える
					for(size_t Idx = 1; Idx < Shape.Bindings.size(); ++Idx) Result.Params.push_back(Syntax.GetCmpName(Shape.Bindings[Idx]));
					ParamOrdinal += static_cast<uint32_t>(Shape.Bindings.size() - 1);
				}
				Result.ParamShapes.push_back(std::move(Shape));
				// 宣言形式の名前を持たない仮引数の記録（引数の無い事を表す `void` と独立した `...` は除く）
				if(
					Found == NoNode && (Syntax.Is(Entry, Role::ParamDecl) || Syntax.Is(Entry, Role::VariadicParamDecl)) &&
					!Syntax.IsWord(Entry, Word::VoidType) && !Syntax.IsWord(Entry, Word::VariadicMarker)
				) {
					Result.HasUnnamedParam = true;
					Result.UnnamedParams.push_back({ Entry, ParamOrdinal });
				}
				// 終了
				return;
			}
		);
		// 呼出結果の有無を共有し，未確定ならドキュメント同期で既存の説明を保持する
		Result.Return = Flow.GetReturnShapeOf(FunctionNode);
		Result.HasReturn = Result.Return.HasCallResult.value_or(false);
		// 名前のフィールドの関数名
		if(const NodeId Name = Syntax.GetFieldChild(FunctionNode, ChildRole::Name); Name != NoNode) Result.Name = Syntax.GetText(Name);
		// テンプレート・ジェネリックの仮引数名の収集（C++ の非型仮引数も含む）
		const NodeId Parent = Syntax.GetParentOf(FunctionNode);
		Core::SyntaxModel::ForEachNamedChildOf(
			Syntax,
			Syntax.Is(Parent, Role::TypeParamScope) ? Parent : FunctionNode,
			[&](const NodeId Child) -> void {
				// テンプレート・ジェネリックの仮引数の並びでない子の読飛
				if(!Syntax.Is(Child, Role::GenericParams)) return;
				Core::SyntaxModel::ForEachNamedChildOf(
					Syntax,
					Child,
					[&](const NodeId Param) -> void {
						// 内側の仮引数列・制約と，導入名を持つ子の宣言を区別
						NodeId NamedParam = Param;
						Core::SyntaxModel::ForEachNamedChildOf(
							Syntax,
							Param,
							[&](const NodeId Part) -> void {
								if(Syntax.Is(Part, Role::TemplateTypeParam)) NamedParam = Part;
								// 終了
								return;
							}
						);
						// 明示された名前のフィールドを優先し，宣言子と型の名前を区別
						NodeId Name = Syntax.GetFieldChild(NamedParam, ChildRole::Name);
						if(!Syntax.IsComparisonName(Name)) Name = NoNode;
						if(Name == NoNode) {
							if(const NodeId Decl = Syntax.GetFieldChild(NamedParam, ChildRole::Declarator); Decl != NoNode) {
								if(Syntax.Is(NamedParam, Role::TemplateValParam)) {
									Core::SyntaxModel::HasStoppedTraversal(
										Syntax,
										Decl,
										[&](const NodeId Part) -> WalkStep {
											// 関数型の仮引数・型・属性へ降りず，宣言子の導入名だけを収集
											if(
												Syntax.Is(Part, Role::ParamContainer) || Part != Decl && Syntax.Is(Part, Role::DeclTypeChild) ||
												Syntax.Is(Part, Role::AttachPrefix)
											) return WalkStep::Skip;
											const NodeId MemberName = Syntax.Is(Part, Role::PtrTypeDeclarator) ? Syntax.GetFieldChild(Part, ChildRole::Declarator) : NoNode;
											if(Syntax.Is(MemberName, Role::TypeName)) {
												Name = MemberName;
												// メンバポインタの導入名で探索を止める事の返戻
												return WalkStep::Stop;
											}
											if(Syntax.Is(Part, Role::ParamName)) {
												Name = Part;
												// 導入名で探索を止める事の返戻
												return WalkStep::Stop;
											}
											// 次の宣言構造の探索の返戻
											return WalkStep::Descend;
										}
									);
								} else {
									const std::vector<NodeId> Bindings = Syntax.GetBindingNames(Decl);
									if(!Bindings.empty()) Name = Bindings.front();
								}
							} else {
								// 宣言子の無い非型仮引数の型名を導入名へ数えない
								if(Syntax.Is(NamedParam, Role::TemplateValParam)) return;
								// 名前と宣言子のフィールドが無い仮引数の，子からの導入名の探索
								const NodeId Nested = Syntax.GetFieldChild(NamedParam, ChildRole::Params);
								bool HasDefault = false;
								Core::SyntaxModel::ForEachChildOf(
									Syntax,
									Nested == NoNode ? NamedParam : Nested,
									[&](const NodeId Part) -> void {
										// 装飾・制約・既定型を導入名へ数えない
										HasDefault = HasDefault || Syntax.Is(Part, Role::AssignmentToken);
										// 導入名の確定後と既定型の読飛
										if(Name != NoNode || HasDefault) return;
										const NodeId NamePart = Syntax.Is(Part, Role::UserType) ? Syntax.GetNamedChild(Part, 0) : Part;
										if(Syntax.Is(NamePart, Role::ParamName) || Syntax.Is(NamePart, Role::TypeName)) Name = NamePart;
										// 終了
										return;
									}
								);
							}
						}
						if(Name != NoNode) Result.TypeParams.push_back(Syntax.GetCmpName(Name));
						// 終了
						return;
					}
				);
				// 終了
				return;
			}
		);
		// 抽出結果の返戻
		return Result;
	}
} // namespace Core
