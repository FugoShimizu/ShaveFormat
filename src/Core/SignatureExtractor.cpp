#include "SignatureExtractor.hpp"
#include <string>

namespace Core {
	/**
	 * 前置の属性を含む節点の頭の取得関数（整形仕様 7.4, 7.6）
	 * 節点の直前に続く前置の属性（Rust の属性，TypeScript の成員の装飾子等の前の兄弟）の最初の物，無ければ節点自身とする
	 * @param Syntax 読取専用の構文
	 * @param Node 節点
	 * @return 前置の属性を含む頭の節点
	 */
	NodeId SignatureExtractor::LeadOf(const SyntaxModel &Syntax, const NodeId Node) {
		NodeId Lead = Node;
		for(
			NodeId Previous = Syntax.PrevNamedSibling(Lead);
			Previous != NoNode && Syntax.Is(Previous, Role::AttachPrefix);
			Previous = Syntax.PrevNamedSibling(Previous)
		) Lead = Previous;
		// 前置の属性を含む頭の返戻
		return Lead;
	}

	/**
	 * 文書の錨の取得関数（整形仕様 7.4）
	 * @param Syntax 抽出する構文
	 * @param FuncNode 関数の節点
	 * @return 文書の先行コメントを紐付ける節点（包装・前置の属性の前）
	 */
	NodeId SignatureExtractor::DocAnchor(const SyntaxModel &Syntax, const NodeId FuncNode) {
		const NodeId Parent = Syntax.ParentOf(FuncNode);
		// 親の無い関数自身の返戻
		if(Parent == NoNode) return FuncNode;
		// C++ の template と JS / TS の export，TS のアンビエント宣言の包装は包装の前の文書を採る事の返戻
		if(Syntax.Is(Parent, Role::DocWrapper)) return Parent;
		// Ruby の先頭のメソッドの文書は，外へ巻き上げる解析器に合わせて本体の文の並びから採る事の返戻
		if(Syntax.Is(Parent, Role::HoistedDocumentBody) && Syntax.NamedChild(Parent, 0) == FuncNode) return Parent;
		// Rust の属性と TypeScript の成員の装飾子は定義の前の兄弟に為る為，文書は最初の装飾の前に在る事の返戻
		return LeadOf(Syntax, FuncNode);
	}

	/**
	 * 文書を付ける定義かの判定関数（整形仕様 7.4）
	 * 文書を補う工程と其の有無を検査する工程が同じ定義を見る為，選び方を此処だけで定める
	 * @param Syntax 抽出する構文
	 * @param Node 節点
	 * @return 本体付の名前付の関数の定義で，文・成員の並びの中に在れば true
	 */
	bool SignatureExtractor::IsDocumentable(const SyntaxModel &Syntax, const NodeId Node) {
		// 関数・構築子の定義で無い節点の返戻
		if(!Syntax.IsNamed(Node) || !Syntax.Is(Node, Role::DocumentedDefinition)) return false;
		// 実装・コールバック用途の JS / TS の物体リテラルの中のメソッドの返戻
		if(Syntax.Is(Node, Role::MethodDefinition) && Syntax.Is(Syntax.ParentOf(Node), Role::ObjectValue)) return false;
		// 本体を持たない抽象宣言・原型の返戻
		if(
			!Syntax.Is(Node, Role::InlineDefinitionBody) && !AnyChildOf(
				Syntax,
				Node,
				[&Syntax](const NodeId Child) -> bool {
					// 実装の本体の子かの返戻
					return Syntax.Is(Child, Role::FunctionBodyContainer);
				}
			)
		) return false;
		const NodeId Anchor = DocAnchor(Syntax, Node);
		// 値の位置へ置いた定義で無い事の返戻（整形仕様 7.4 は最終字面の行頭での判断を禁じる為，構文の親で決める）
		return Syntax.Is(Anchor, Role::DocHostSequence) || Syntax.Is(Syntax.ParentOf(Anchor), Role::DocHostSequence);
	}

	/**
	 * 署名の抽出関数（整形仕様 7.4）
	 * @param Syntax 抽出する構文
	 * @param FuncNode 関数の節点
	 * @return 関数名・仮引数名の並び・戻値の有無・型仮引数名
	 */
	Signature SignatureExtractor::Extract(const SyntaxModel &Syntax, const NodeId FuncNode) {
		// 署名の抽出結果の準備
		Signature Result;
		// 戻値の区分と宣言子の並びを判定する制御の流れ
		const ControlFlow Flow(Syntax);
		const bool IsDeclaratorSignature = Syntax.Is(FuncNode, Role::DeclaratorSignature);
		const bool HasSeparateParameterParts = Syntax.Is(FuncNode, Role::SeparateParameterParts);
		// 仮引数の容器（C / C++ は名前に最も近い関数宣言子の物，Swift は関数の直下，其の他は仮引数の場か最後の仮引数の容器）
		NodeId Container = NoNode;
		if(IsDeclaratorSignature) {
			for(NodeId Declarator = Flow.NextDeclarator(FuncNode); Declarator != NoNode; Declarator = Flow.NextDeclarator(Declarator)) {
				if(Syntax.Is(Declarator, Role::FunctionDeclarator)) Container = Syntax.FieldChild(Declarator, ChildRole::Parameters);
			}
		} else if(Syntax.Is(FuncNode, Role::DirectParameterSignature)) Container = FuncNode;
		else if(Container = Syntax.FieldChild(FuncNode, ChildRole::Parameters); Container == NoNode) {
			ForEachNamedChildOf(
				Syntax,
				FuncNode,
				[&Syntax, &Container](const NodeId Child) -> void {
					// 仮引数の容器の子の保持
					if(Syntax.Is(Child, Role::ParamContainerLike)) Container = Child;
				}
			);
		}
		// 仮引数名の型（C++ のメンバポインタは限定宣言子の名前）
		// 仮引数の名前は投影済の役割で照合する
		ForEachNamedChildOf(
			Syntax,
			Container,
			[&](const NodeId Entry) -> void {
				// Kotlin・Swift の既定値・関数の他の部品と Rust の属性等，仮引数でない子を読飛ばす場合の終了
				if(HasSeparateParameterParts && !Syntax.Is(Entry, Role::Parameter) || Syntax.Is(Entry, Role::AttachPrefix)) return;
				// 分割代入の模式は単一の名前を持たない為，既存の文書を全て保つ
				bool IsDestructure = Syntax.Is(Entry, Role::DestructurePattern) || AnyChildOf(
					Syntax,
					Entry,
					[&Syntax](const NodeId Sub) -> bool {
						// 名前付の分割代入の模式の子かの返戻
						return Syntax.IsNamed(Sub) && Syntax.Is(Sub, Role::DestructurePattern);
					}
				);
				// Rust の組・構造体・`_` の模式で束縛する仮引数も単一の名前を持たない
				if(
					const NodeId Pattern = Syntax.Is(Entry, Role::PatternParameter) ? Syntax.FieldChild(Entry, ChildRole::Pattern) : NoNode;
					Pattern != NoNode
				) IsDestructure = !Syntax.Is(Pattern, Role::Identifier) && !Syntax.Is(Pattern, Role::SelfValue);
				if(IsDestructure) {
					Result.HasUnnamedParam = true;
					// 分割代入の模式の終了
					return;
				}
				NodeId Found = NoNode;
				// 仮引数名の照合関数
				const auto MatchName = [&](const NodeId Node) -> bool {
					// メンバポインタの名前
					const NodeId Member = Syntax.Is(Node, Role::PointerTypeDeclarator) ? Syntax.FieldChild(Node, ChildRole::Declarator) : NoNode;
					const bool IsMemberName = Syntax.Is(Member, Role::TypeName);
					// 仮引数名の型でない節点の返戻
					if(!Syntax.Is(Node, Role::ParameterName) && !IsMemberName) return false;
					Found = IsMemberName ? Member : Node;
					// 最初の発見で打ち切る事の返戻
					return true;
				};
				// C / C++ は宣言子の場を優先し，場が無ければ型の部位を除いて探す
				if(
					const NodeId Declarator = IsDeclaratorSignature ? Syntax.FieldChild(Entry, ChildRole::Declarator) : NoNode;
					Declarator != NoNode
				) AnyNodeOf(Syntax, Declarator, MatchName);
				else if(IsDeclaratorSignature) {
					ForEachNamedChildOf(
						Syntax,
						Entry,
						[&](const NodeId Sub) -> void {
							// 型の部位を除く仮引数名の探索
							if(Found == NoNode && !Syntax.Is(Sub, Role::DeclTypeChild)) AnyNodeOf(Syntax, Sub, MatchName);
						}
					);
				// 名前の場（C#・Swift の型名と外部名を仮引数名と取り違えない）と仮引数自身の照合
				} else if(
					const NodeId Named = Syntax.FieldChild(Entry, ChildRole::Name);
					(Named == NoNode || !MatchName(Named)) && !MatchName(Entry)
				) {
					// 注釈・デコレータ・属性（`@PathVariable("id") long id` 等）の中の名前は仮引数名でない為，降りない
					WalkNodes(
						Syntax,
						Entry,
						[&](const NodeId Node) -> WalkStep {
							// 仮引数名で止める事の返戻
							if(Node != Entry && MatchName(Node)) return WalkStep::Stop;
							// 装飾の中へ降りない事の返戻
							return Node != Entry && Syntax.Is(Node, Role::AttachPrefix) ? WalkStep::Skip : WalkStep::Descend;
						}
					);
				}
				// 見付けた仮引数名の記録
				if(Found != NoNode) Result.Params.push_back(Syntax.Text(Found));
				// C / C++ の名前を持たない仮引数（`const T &` 等，引数の無い事を表す `void` と可変長の `...` を除く）の記録
				else if(IsDeclaratorSignature && Syntax.Is(Entry, Role::ParameterDeclaration)) {
					if(!Syntax.IsWord(Entry, Word::VoidType) && !Syntax.IsWord(Entry, Word::VariadicMarker)) Result.HasUnnamedParam = true;
				}
			}
		);
		// 戻値の有無（型の宣言か本体の返戻文から定まる物だけを値を返すとし，根拠の無い推論では既存の説明を保つ）
		const ReturnShape Shape = Flow.ReturnShapeOf(FuncNode);
		Result.HasReturn =
		Shape.Evidence != ReturnEvidence::Unknown && (Shape.Kind == ReturnKind::Value || Shape.Kind == ReturnKind::Main);
		Result.ReturnsFromSignature = Shape.Evidence == ReturnEvidence::Declared;
		// 名前の場の関数名
		if(const NodeId Name = Syntax.FieldChild(FuncNode, ChildRole::Name); Name != NoNode) Result.Name = Syntax.Text(Name);
		// 型仮引数名（KDoc 等が仮引数の札で説明する物）
		ForEachNamedChildOf(
			Syntax,
			FuncNode,
			[&](const NodeId Child) -> void {
				// 型仮引数の並びでない子の場合の終了
				if(!Syntax.Is(Child, Role::GenericParameters)) return;
				ForEachNamedChildOf(
					Syntax,
					Child,
					[&](const NodeId Parameter) -> void {
						// 型仮引数の最初の名前の記録
						if(const NodeId Name = Syntax.NamedChild(Parameter, 0); Name != NoNode) Result.TypeParams.push_back(Syntax.Text(Name));
					}
				);
			}
		);
		// 抽出結果の返戻
		return Result;
	}
} // namespace Core
