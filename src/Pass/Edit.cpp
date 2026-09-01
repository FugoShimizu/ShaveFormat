#include "Edit.hpp"
#include "../Util/NodeKind.hpp"
#include "../Util/Parallel.hpp"
#include "../Util/Preprocess.hpp"
#include "../Util/SyntaxCheck.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/**
 * 複数種別の正規化編集の収集関数（`TrailingComma` / `TsTypeSeparator` / `Paren` / `JsxExpr` / `Sizeof` / `Brace` を１回の構文木走査で収集）
 * @param Src ソースコード
 * @param StartNode 走査対象の部分木の根ノード
 * @param StartParent 起点の親（起点が根なら空）
 * @param StartPrev 起点の直前の兄弟（無ければ空）
 * @param StartNext 起点の直後の兄弟（無ければ空）
 * @param Language 対象言語（有効な編集種別の選別に使う）
 * @param Edits 収集先のエディット一覧
 */
void EditPass::CollectConsolidatedNamedWalk(
	const TSSource &Src,
	const TSNode StartNode,
	const TSNode StartParent,
	const TSNode StartPrev,
	const TSNode StartNext,
	const Lang Language,
	std::vector<TextEdit> &Edits
) {
	// 根の子列から渡す親・兄弟による再解析の回避
	if(ts_node_is_null(StartNode)) return;
	// 子列・カーソルを再利用し，実演算子の祖先を枠へ保持する反復先行順走査
	struct WalkFrame {
		TSNode Node; // 現在の走査節点
		TSNode Parent; // 現在節点の親
		TSNode OpAncestor; // 括弧層を透過した最初の非括弧の祖先
		TSNode OperandTop; // `OpAncestor` の直下に位置する被演算子（括弧層の最外）
		TSNode PrevSibling; // 直前の兄弟
		TSNode NextSibling; // 直後の兄弟（親の引直に依る二乗費用を避ける為に保持）
		bool IsInTypeAlias; // 祖先に `type_alias_statement` が在るか (Python)
		bool IsInDebugInterp; // 祖先に `=` を持つ `interpolation` が在るか（Python の f 文字列デバッグ）
		bool IsInInterp; // 祖先に文字列の補間が在るか（Python の f 文字列補間・Swift の `\(式)`）
		bool IsInCppDeclaration; // 宣言にも読める文頭又は直接初期化の引数へ繋がるか
		bool IsInBraceHeader; // 最寄の制御構文の見出の中か（本体の塊より制御構文が近い，Go / Rust / Swift）
		bool IsInForInitializer; // JS/TS の for 文の初期化子の中か（`in` を二項演算子に戻す式・括弧の中を除く）
		bool IsInError; // 祖先に構文誤り (`ERROR`) が在るか（誤読したコメントの中の字句をリテラルとして書き換えない）
	};
	// 未走査節点の後入先出列
	std::vector<WalkFrame> WorkStack;
	std::unordered_map<const void *, CppDeclarationHead> CppDeclarationHeads;
	std::unordered_map<const void *, uint32_t> LastCloseAngleOf;
	std::vector<TSNode> AllKids;
	std::unordered_set<const void *> NestedLayers;
	const bool IsPython = Language == Lang::Python;
	WorkStack.push_back(
		{
			StartNode,
			StartParent,
			StartParent,
			StartNode,
			StartPrev,
			StartNext,
			false,
			false,
			false,
			false,
			false,
			false,
			!ts_node_is_null(StartParent) && std::string_view(ts_node_type(StartParent)) == "ERROR"
		}
	);
	// 子列走査に再利用するカーソル
	HeldCursor KidCursor(StartNode);
	const bool IsJsTsLang = Language.IsJsTs(), IsCFamilyLang = Language.IsCFamily();
	const auto IsMacroStyleName = [](const std::string_view Name) -> bool {
		// 空でなく大文字・数字・`_` だけから成るかの返戻
		return !Name.empty() && std::all_of(
			Name.begin(),
			Name.end(),
			[](const char Char) -> bool {
				// マクロ名構成文字との一致返戻
				return Char >= 'A' && Char <= 'Z' || Char == '_' || Char >= '0' && Char <= '9';
			}
		);
	};
	const bool IsBoolLiteralLangOk = Language == Lang::Cpp && !Language.IsCShared;
	const bool IsNumPadLang = IsCFamilyLang || Language == Lang::CSharp || Language == Lang::Java;
	const bool IsNumLang =
	IsNumPadLang || IsJsTsLang || Language == Lang::Go || Language == Lang::Rust || Language == Lang::Kotlin ||
	Language == Lang::Swift || Language == Lang::PHP || Language == Lang::Ruby || Language == Lang::Python ||
	Language == Lang::JSON;
	// 祖先引直しを避ける親索引の遅延構築
	std::unordered_map<const void *, TSNode> ParentOf;
	bool IsParentMapBuilt = false;
	const auto ParentNode = [&ParentOf, &IsParentMapBuilt, StartNode](const TSNode Cur) -> TSNode {
		// 祖先照会が初めて必要に為った時だけ親の表を作る
		if(!IsParentMapBuilt) {
			IsParentMapBuilt = true;
			WalkAst(
				StartNode,
				[&ParentOf](const TSNode Ancestor) -> void {
					ForEachChild(
						Ancestor,
						[&ParentOf, Ancestor](const TSNode Child) -> void {
							// 子から親への登録
							ParentOf.emplace(Child.id, Ancestor);
						}
					);
				}
			);
		}
		// 索引内の親候補
		const std::unordered_map<const void *, TSNode>::const_iterator Iter = ParentOf.find(Cur.id);
		// 索引又は構文木の親の返戻
		return Iter != ParentOf.end() ? Iter->second : ts_node_parent(Cur);
	};
	std::unordered_map<const void *, int> PrecedenceMemo, OppositeOf[2];
	std::vector<const void *> OppositePath;
	const auto PrecedenceOf = [&](const TSNode OpNode) -> int {
		const auto [Entry, Fresh] = PrecedenceMemo.try_emplace(OpNode.id, 0);
		// 初回だけの優先順位計算
		if(Fresh) Entry->second = OpPrecedence(Src, OpNode, Language);
		// 控え済優先順位の返戻
		return Entry->second;
	};
	const auto OppositePrecOf = [&](TSNode Child, const bool IsLeftOperand) -> int {
		int Result = 0;
		// 反対側の演算子へ至る今回の経路の記録
		OppositePath.clear();
		for(TSNode Ancestor = ParentNode(Child);; Child = Ancestor, Ancestor = ParentNode(Ancestor)) {
			if(
				// 経路控え内の候補
				const std::unordered_map<const void *, int>::const_iterator Hit = OppositeOf[IsLeftOperand].find(Child.id);
				Hit != OppositeOf[IsLeftOperand].end()
			) {
				Result = Hit->second;
				break;
			}
			OppositePath.push_back(Child.id);
			// 根到達時の探索終了
			if(ts_node_is_null(Ancestor)) break;
			const int AncestorPrec = PrecedenceOf(Ancestor);
			// グルーピング括弧層は透過し，其れ以外の非演算子祖先で打ち切る
			if(!AncestorPrec) {
				if(NodeKind::GroupingParen.Contains(Ancestor)) continue;
				// 非演算子祖先での探索終了
				break;
			}
			// 左右の被演算子が初めて入れ替わる祖先の探索（右隣を持たない前置単項は越える）
			const bool IsChildLeft = ts_node_eq(ts_node_named_child(Ancestor, 0), Child);
			// 前置単項の透過
			if(!IsLeftOperand && IsChildLeft && !ts_node_is_named(ts_node_child(Ancestor, 0))) continue;
			if(IsLeftOperand ? !IsChildLeft : IsChildLeft) {
				// 最初に左右が交差する優先順位
				Result = AncestorPrec;
				// 逆側演算子の発見
				break;
			}
		}
		// 遡行経路全体への同一優先順位の配布
		for(const void *Id : OppositePath) OppositeOf[IsLeftOperand].emplace(Id, Result);
		// 逆側演算子優先順位の返戻
		return Result;
	};
	// PHP 閉じタグ前の文末判定（式文は ; の有無，其の他は最後の字句）
	const auto OmitsPhpTerminator = [&Src](TSNode Node) -> bool {
		// 現在位置の文末性の判定
		while(true) {
			const std::string_view Type(ts_node_type(Node));
			// 文終端不要位置の返戻
			if(NodeKind::PhpOpenTagEnd.Contains(Type)) return false;
			// 現在節点の全子数
			const uint32_t Count = ts_node_child_count(Node);
			if(NodeKind::PhpSemicolonStatement.Contains(Type)) {
				// 式文の終端省略可否の返戻
				return std::string_view(ts_node_type(ts_node_child(Node, Count - 1))) != ";" && !Preprocess::IsPhpHaltCall(Src, Node);
			}
			// 末端字句の終端要否の返戻
			if(!Count) return !NodeKind::PhpCompleteToken.Contains(Type);
			// 末端字句への降下
			Node = ts_node_child(Node, Count - 1);
		}
	};
	// 全節点の言語別正規化編集の共通列への収集
	while(!WorkStack.empty()) {
		const WalkFrame Frame = WorkStack.back();
		WorkStack.pop_back();
		const TSNode Node = Frame.Node;
		const std::string_view TypeView(ts_node_type(Node));
		// 字面が値になるマクロと assert の実引数内部の編集除外
		if(IsCFamilyLang && Src.StringizesArguments(Node)) continue;
		// typeof 比較の厳密化（null/undefined の縮約は束縛・読取回数等を変える為に除外）
		if(IsJsTsLang && TypeView == "binary_expression") {
			TSNode Lhs {}, Rhs {};
			std::string_view EqOp;
			std::vector<TSNode> EqTokens;
			ForEachChild(
				Node,
				[&](const TSNode Child) -> void {
					// 被演算子候補の識別
					const bool IsNamed = ts_node_is_named(Child);
					const std::string_view Tok = IsNamed ? std::string_view{} : Src.View(Child);
					// 左右被演算子の採用
					if(IsNamed && ts_node_is_null(Lhs)) Lhs = Child;
					else if(IsNamed && ts_node_is_null(Rhs)) Rhs = Child;
					else if(Tok == "==" || Tok == "!=" || Tok == "===" || Tok == "!==") {
						// 比較演算子の記録
						EqOp = Tok;
						// 緩い比較だけの編集候補化
						if(Tok.size() == 2) EqTokens.push_back(Child);
					}
				}
			);
			// 両被演算子の揃った比較だけの型確認
			if(!EqOp.empty() && !ts_node_is_null(Lhs) && !ts_node_is_null(Rhs)) {
				const std::string_view LhsType(ts_node_type(Lhs)), RhsType(ts_node_type(Rhs));
				const bool IsLhsTypeof = LhsType == "unary_expression" && HasUnnamedTokenChild(Src, Lhs, "typeof");
				const bool IsLhsString = LhsType == "string";
				const bool IsRhsTypeof = RhsType == "unary_expression" && HasUnnamedTokenChild(Src, Rhs, "typeof");
				// 型の確かな typeof 比較だけの厳密化（識別子・ラッパとの比較と逆変換は除外）
				if(
					const bool IsRhsString = RhsType == "string";
					(EqOp == "==" || EqOp == "!=") && (IsLhsTypeof && (IsRhsString || IsRhsTypeof) || IsLhsString && IsRhsTypeof)
				) for(const TSNode EqTok : EqTokens) {
					if(Src.View(EqTok) == "==") TextEdit::Push(Src.Start(EqTok), Src.End(EqTok), "===", Edits);
					else TextEdit::Push(Src.Start(EqTok), Src.End(EqTok), "!==", Edits);
				}
			}
		}
		// JS/TS の文相当節点の最後の子に無い文末 ; の補完
		if(IsJsTsLang && NodeKind::JsTsAsiTarget.Contains(TypeView)) {
			const std::string_view NodeView = Src.View(Node);
			const bool IsExport = TypeView == "export_statement";
			const uint32_t NamedCount = IsExport ? ts_node_named_child_count(Node) : 0;
			const TSNode Inner = NamedCount ? ts_node_named_child(Node, NamedCount - 1) : TSNode{};
			const bool IsInnerComplete =
			!ts_node_is_null(Inner) && NodeKind::JsTsAsiTarget.Contains(Inner) && Src.End(Inner) == Src.End(Node);
			// 次行との意図しない連結を防ぐ文末セミコロンの補完
			const TSNode Value = IsExport ? TSSource::FieldChild(Node, "value") : TSNode{};
			if(
				const bool IsDeclarationBody = IsExport && (
					!ts_node_is_null(TSSource::FieldChild(Node, "declaration")) ||
					!ts_node_is_null(Value) && NodeKind::JsDefaultDeclaration.Contains(Value)
				) || TypeView == "expression_statement" && std::string_view(ts_node_type(ts_node_named_child(Node, 0))) == "internal_module";
				!NodeView.empty() && NodeView.back() != ';' && !IsInnerComplete && (NodeView.back() != '}' || !IsDeclarationBody)
			) TextEdit::Push(Src.End(Node), Src.End(Node), ";", Edits);
		}
		// 成員の外に在る区切字句の確認（後続コンマは後段で ; へ置換）
		if(
			IsJsTsLang && NodeKind::JsTsClassMemberAsi.Contains(TypeView) && !ts_node_is_null(Frame.Parent) &&
			std::string_view(ts_node_type(Frame.Parent)) == "class_body" &&
			(ts_node_is_null(Frame.NextSibling) || Src.View(Frame.NextSibling) != ";" && Src.View(Frame.NextSibling) != ",")
		) TextEdit::Push(Src.End(Node), Src.End(Node), ";", Edits);
		// PHP 閉じタグ前の ; の明示（見出直後の空本体も補い，後続文の誤読を防止）
		if(
			Language == Lang::PHP && TypeView == "text_interpolation" && !ts_node_is_null(Frame.PrevSibling) &&
			OmitsPhpTerminator(Frame.PrevSibling)
		) TextEdit::Push(Src.End(Frame.PrevSibling), Src.End(Frame.PrevSibling), ";", Edits);
		const uint32_t AllChildCount = ts_node_child_count(Node);
		// `ERROR` に包まれる JSONC 末尾コンマの個別除去
		if(
			const TSNode Comma = Language == Lang::JSON ? SyntaxCheck::JsonTrailingCommaToken(Src, Node) : TSNode{};
			!ts_node_is_null(Comma)
		) TextEdit::Push(Src.Start(Comma), Src.End(Comma), "", Edits);
		// 共有カーソルと子列の再利用による，節点毎の確保と二乗の添字走査の回避
		AllKids.clear();
		if(AllChildCount) {
			AllKids.reserve(AllChildCount);
			ts_tree_cursor_reset(&KidCursor, Node);
			if(ts_tree_cursor_goto_first_child(&KidCursor)) {
				do AllKids.push_back(ts_tree_cursor_current_node(&KidCursor));
				while(ts_tree_cursor_goto_next_sibling(&KidCursor));
			}
		}
		// 行継続を除く指定位置以降の直近一文字字句の探索
		const auto NextSingleCharToken = [&Src, &AllKids](const size_t FromIdx) -> char {
			// 後続字句の順次確認
			for(size_t NextIdx = FromIdx; NextIdx < AllKids.size(); ++NextIdx) {
				const TSNode Next = AllKids[NextIdx];
				const uint32_t NextStart = Src.Start(Next), NextEnd = Src.End(Next);
				// 空字句と行継続の除外
				if(NextEnd == NextStart || std::string_view(ts_node_type(Next)) == "line_continuation") continue;
				// 複数字句での探索終了
				if(NextEnd != NextStart + 1) break;
				// 直近一文字字句の返戻
				return Src[NextStart];
			}
			// 未発見番兵の返戻
			return '\0';
		};
		// Rust マクロの字句照合を保つ末尾コンマ除去（祖先探索は最初の候補迄遅延）
		int InMacroTokens = Language == Lang::Rust ? -1 : 0;
		const TSNode Parent = Frame.Parent;
		bool IsPythonTupleSubscript = Language == Lang::Python && TypeView == "subscript";
		if(
			Language == Lang::Python && TypeView == "type_parameter" && !ts_node_is_null(Parent) &&
			std::string_view(ts_node_type(Parent)) == "generic_type"
		) {
			const TSNode Type = ts_node_parent(Parent), TypeAlias = ts_node_parent(Type);
			// `type Alias[T,]` の左辺は型引数式でなく型仮引数宣言である為，通常の末尾コンマ除去対象
			IsPythonTupleSubscript = ts_node_is_null(TypeAlias) || std::string_view(ts_node_type(TypeAlias)) != "type_alias_statement" ||
			!ts_node_eq(TSSource::FieldChild(TypeAlias, "left"), Type);
		}
		const bool IsPythonBareList = Language == Lang::Python && NodeKind::PythonBareList.Contains(TypeView);
		size_t TupleCommas = 0;
		if(IsPythonTupleSubscript || IsPythonBareList || NodeKind::TupleLike.Contains(TypeView)) {
			for(const TSNode &Kid : AllKids) if(Src.End(Kid) == Src.Start(Kid) + 1 && Src[Src.Start(Kid)] == ',') ++TupleCommas;
		}
		// CSS の空代替値を表す `var(--x,)` の末尾コンマ保持
		if(AllChildCount && Language != Lang::CSS) for(size_t ChildIdx = 0; ChildIdx < AllKids.size(); ++ChildIdx) {
			const TSNode Child = AllKids[ChildIdx];
			const uint32_t Start = Src.Start(Child);
			// 値を表す名前付コンマの保持（Ruby の配列照合では残りの要素を受ける）
			if(Src.End(Child) != Start + 1 || Src[Start] != ',' || ts_node_is_named(Child)) continue;
			// TS のクラス成員後の区切コンマの ; への統一
			if(IsJsTsLang && TypeView == "class_body" && ChildIdx && NodeKind::JsTsClassMemberAsi.Contains(AllKids[ChildIdx - 1])) {
				TextEdit::Push(Start, Start + 1, ";", Edits);
				continue;
			}
			// 末尾コンマ候補発見時だけのマクロ内外の確認
			if(InMacroTokens < 0) {
				InMacroTokens = NodeKind::RustMacroTokens.Contains(TypeView) || HasAncestorOf(
					Node,
					[](const TSNode Ancestor) -> bool {
						// マクロ字句列祖先との一致返戻
						return NodeKind::RustMacroTokens.Contains(Ancestor);
					}
				);
			}
			// マクロ字句列内のコンマを含む原文保持
			if(InMacroTokens) break;
			// 組と引数分解に必須のコンマの保持
			if(
				const char NextChar = NextSingleCharToken(ChildIdx + 1);
				NextChar == ')' || NextChar == ']' || NextChar == '}' ||
				ChildIdx + 1 == AllKids.size() && (IsPythonBareList || Language == Lang::Python && TypeView == "lambda_parameters") ||
				ChildIdx + 1 < AllKids.size() && std::string_view(ts_node_type(AllKids[ChildIdx + 1])) == "enum_body_declarations"
			) {
				// 一要素組の型を決める唯一のコンマの保持
				if(TupleCommas == 1) continue;
				// 空要素を表す末尾コンマの保持
				if(NextChar == ']') {
					const uint32_t Prev = TextEdit::SkipCharsLeftBounded(Src, Start, 0, " \t\n");
					if(const char PrevChar = Prev ? Src[Prev - 1] : '\0'; PrevChar == ',' || PrevChar == '[') continue;
				}
				// 冗長な末尾コンマの除去予約
				TextEdit::Push(Start, Start + 1, "", Edits);
			}
		}
		// `TsTypeSeparatorEdits`：TS 型本体のメンバ間を `,` に統一
		if(Language == Lang::TypeScript && AllChildCount && NodeKind::TsTypeBody.Contains(TypeView)) {
			TSNode PrevNamed = {};
			bool HasPrevNamed = false;
			for(size_t ChildIdx = 0; ChildIdx < AllKids.size(); ++ChildIdx) {
				const TSNode Child = AllKids[ChildIdx];
				const uint32_t Start = Src.Start(Child);
				if(ts_node_is_named(Child)) {
					if(HasPrevNamed) {
						if(
							const uint32_t PrevEnd = Src.End(PrevNamed);
							std::string_view(Src.data() + PrevEnd, Start - PrevEnd).find_first_of(",;") == std::string_view::npos
						) TextEdit::Push(PrevEnd, PrevEnd, ",", Edits);
					}
					PrevNamed = Child;
					HasPrevNamed = true;
				}
				// 型本体の末尾区切除去と中間区切のコンマ化
				if(Src.End(Child) == Start + 1 && Src[Start] == ';') {
					if(NextSingleCharToken(ChildIdx + 1) == '}') TextEdit::Push(Start, Start + 1, "", Edits);
					else TextEdit::Push(Start, Start + 1, ",", Edits);
				}
			}
		}
		// 最外層で判断済の内側括弧の重複編集除外
		const bool IsNestedLayer = !NestedLayers.empty() && NestedLayers.erase(Node.id);
		const bool IsNodeParen = NodeKind::GroupingParen.Contains(TypeView);
		if(IsNodeParen && !IsNestedLayer && !ts_node_is_null(Parent)) {
			const std::string_view ParentType = ts_node_type(Parent);
			const TSNode PrevSibling = Frame.PrevSibling;
			const std::string_view Delimiter =
			ts_node_is_null(PrevSibling) || ts_node_is_named(PrevSibling) ? std::string_view() : Src.View(PrevSibling);
			const bool IsAfterOwnDelimiter = NodeKind::ForEachHeader.Contains(ParentType) ?
			Delimiter == "in" || Delimiter == "of" || Delimiter == ":" ||
			Delimiter == "(" && (ParentType == "for_in_statement" || Language == Lang::PHP) :
			(Delimiter == "(" || Delimiter == "[") && ParentType != "decltype";
			// 現在括弧周辺の構文誤り標識
			const bool IsTreeBroken =
			ParentType == "ERROR" || !ts_node_is_null(PrevSibling) && std::string_view(ts_node_type(PrevSibling)) == "ERROR" ||
			ts_node_has_error(Node);
			// 構文括弧でない被演算子の判定を委譲（new の呼出・任意連鎖と不明な型は保持）
			const auto IsReleasedOperand = [&]() -> bool {
				// 括弧を省ける親構文の判定
				// switch 対象の構文括弧返戻
				if(Language == Lang::CSharp && ParentType == "switch_expression") return true;
				if(IsJsTsLang && ParentType == "new_expression" && ts_node_eq(TSSource::FieldChild(Parent, "constructor"), Node)) {
					TSNode Name = ts_node_named_child_count(Node) == 1 ? ts_node_named_child(Node, 0) : TSNode{};
					while(
						!ts_node_is_null(Name) && NodeKind::JsMemberTarget.Contains(Name) &&
						ts_node_is_null(TSSource::FieldChild(Name, "optional_chain"))
						// メンバ受け手への遡行
					) Name = TSSource::FieldChild(Name, "object");
					// 原子へ達するメンバ連鎖の返戻
					return !ts_node_is_null(Name) && NodeKind::PostfixReceiverLeaf.Contains(Name);
				}
				if(IsCFamilyLang && ParentType == "cast_expression") {
					// キャストの型記述
					const TSNode Descriptor = TSSource::FieldChild(Parent, "type");
					const TSNode Declarator = ts_node_is_null(Descriptor) ? TSNode{} : TSSource::FieldChild(Descriptor, "declarator");
					// 型が基本型かポインタかの返戻
					return !ts_node_is_null(Descriptor) && (
						ts_node_is_null(Declarator) ?
						NodeKind::CppBuiltinType.Contains(TSSource::FieldChild(Descriptor, "type")) :
						std::string_view(ts_node_type(Declarator)) == "abstract_pointer_declarator"
					);
				}
				// 構文括弧を持つ親の返戻
				return false;
			};
			// 親構文に必須の括弧の先行保護
			bool ShouldKeep =
			(NodeKind::ParenKeeper.Contains(ParentType) || Language == Lang::Rust && ParentType == "await_expression") && !(
				(
					Language == Lang::Go || Language == Lang::Rust || Language == Lang::Swift || Language == Lang::Python || Language == Lang::Ruby
				) && NodeKind::ParenOptionalHeader.Contains(ParentType) || (
					(
						NodeKind::PostfixReceiverHost.Contains(ParentType) ||
						Language == Lang::Rust && NodeKind::AwaitTryCastExpression.Contains(ParentType)
					) && ts_node_eq(ts_node_named_child(Parent, 0), Node) ||
					ParentType == "type_switch_statement" && !ts_node_is_null(Frame.NextSibling) && Src.View(Frame.NextSibling) == "."
				) && !KeepsPostfixReceiver(Src, Node, Parent, Language) || IsAfterOwnDelimiter || IsReleasedOperand()
			) || IsTreeBroken;
			// Python の型別名の誤読内の括弧保護（他言語は祖先探索を省略）
			if(!ShouldKeep && Frame.IsInTypeAlias) ShouldKeep = true;
			TSNode InnerNode = ts_node_named_child_count(Node) == 1 ? ts_node_named_child(Node, 0) : TSNode{};
			uint32_t Depth = 1;
			if(!IsTreeBroken && !Frame.IsInDebugInterp && !Frame.IsInTypeAlias && ts_node_child_count(Node) == 3) {
				while(
					!ts_node_is_null(InnerNode) && NodeKind::GroupingParen.Contains(InnerNode) && ts_node_child_count(InnerNode) == 3 &&
					ts_node_named_child_count(InnerNode) == 1
				) {
					// 一括判断済の内側層の後続走査からの除外
					NestedLayers.insert(InnerNode.id);
					// 一括除去層数の加算
					++Depth;
					// 次の内側層への降下
					InnerNode = ts_node_named_child(InnerNode, 0);
				}
			}
			// 最内節点の型名
			const std::string_view InnerType = !ts_node_is_null(InnerNode) ? std::string_view(ts_node_type(InnerNode)) : std::string_view();
			// 処理系が型表明として読む JSDoc の直後の括弧保持
			if(!ShouldKeep && IsJsTsLang && TypeView == "parenthesized_expression") {
				const auto HasJsDocType = [&Src](const TSNode Target) -> bool {
					for(const CommentAttach &Comment : Src.GetLeading(Target)) {
						// 先行コメントの本文
						const std::string_view Text = Comment.Text;
						for(size_t Tag = Text.find("@type"); Tag != std::string_view::npos; Tag = Text.find("@type", Tag + 5)) {
							// @type 後の探索位置
							size_t Type = Tag + 5;
							// 別語内の一致除外
							if(Type < Text.size() && !std::isspace(static_cast<unsigned char>(Text[Type])) && Text[Type] != '{') continue;
							// 型指定開始への移動
							while(Type < Text.size() && std::isspace(static_cast<unsigned char>(Text[Type]))) ++Type;
							// 型表明タグ発見の返戻
							if(Type < Text.size() && Text[Type] == '{') return true;
						}
					}
					// 型表明不在の返戻
					return false;
				};
				// JSDoc 型表明による保持
				ShouldKeep = HasJsDocType(Node) || !ts_node_is_null(InnerNode) && HasJsDocType(InnerNode);
			}
			// 後続を取り込む裸呼出・終端無範囲の括弧保護（範囲を閉じる区切前は除外）
			if(!ShouldKeep && !ts_node_is_null(InnerNode)) {
				if(Language == Lang::Ruby && InnerType == "call") {
					// `(` で始まらない引数列による裸呼出の識別
					if(const TSNode Args = TSSource::FieldChild(InnerNode, "arguments"); !ts_node_is_null(Args) && ts_node_child_count(Args)) {
						// 裸呼出の括弧保持
						if(Src.View(ts_node_child(Args, 0)) != "(") ShouldKeep = true;
					}
				} else if(
					NodeKind::RangeLike.Contains(InnerType) && !ts_node_is_named(ts_node_child(InnerNode, ts_node_child_count(InnerNode) - 1))
				) {
					// 範囲後続の区切確認
					ShouldKeep = std::string_view(Language == Lang::Ruby ? ")]}," : ")]},;").find(Src[TextEdit::SkipSpRight(Src, Src.End(Node))]) ==
					std::string_view::npos;
				}
			}
			// 式の包装でない複数名前付子を持つ括弧の保持
			if(!ShouldKeep && TypeView == "parenthesized_expression" && ts_node_named_child_count(Node) > 1) ShouldKeep = true;
			// Go の chan で始まる受信値の括弧保護（受信専用チャネル型への変換と誤読する為）
			if(
				!ShouldKeep && Language == Lang::Go && ParentType == "unary_expression" && !ts_node_is_null(InnerNode) &&
				Src.View(ts_node_child(Parent, 0)) == "<-"
			) if(const std::string_view InnerText = Src.View(InnerNode); InnerText.starts_with("chan")) {
				// chan 語境界の確認
				ShouldKeep = InnerText.size() == 4 || !IsIdentifierChar(InnerText[4]);
			}
			// 後置 ! を介する連鎖の null 条件連鎖への取込み抑止
			if(!ShouldKeep && Language == Lang::CSharp && !NodeKind::GroupingParen.Contains(InnerType)) {
				// 括弧層外の後置祖先候補
				TSNode PostfixParent = Frame.OpAncestor;
				while(!ts_node_is_null(PostfixParent) && std::string_view(ts_node_type(PostfixParent)) == "postfix_unary_expression") {
					// 後置 ! 層の透過
					PostfixParent = ParentNode(PostfixParent);
				}
				if(
					// 後置祖先の型名
					const std::string_view PostfixType = ts_node_is_null(PostfixParent) ? std::string_view() : ts_node_type(PostfixParent);
					NodeKind::CsPostfixChain.Contains(PostfixType)
				) for(TSNode Target = InnerNode; !ts_node_is_null(Target);) {
					const std::string_view TargetType = ts_node_type(Target);
					if(TargetType == "conditional_access_expression") {
						// null 条件連鎖の境界保持
						ShouldKeep = true;
						// null 条件連鎖発見時の終了
						break;
					}
					// 連鎖外での探索終了
					if(!NodeKind::CsPostfixChain.Contains(TargetType)) break;
					// 連鎖の受け手への降下
					Target = ts_node_named_child(Target, 0);
					// 後置 ! 内側だけを透過する連鎖括弧の探索
					if(TargetType != "postfix_unary_expression" && NodeKind::GroupingParen.Contains(Target)) break;
					// 後置 ! 内側の括弧透過
					while(!ts_node_is_null(Target) && NodeKind::GroupingParen.Contains(Target)) Target = ts_node_named_child(Target, 0);
				}
			}
			// C++ の型引数列の閉じと紛れる比較括弧の保護（括弧対を開かない祖先は透過）
			if(!ShouldKeep && Src.View(Node).find_first_of("<>") != std::string_view::npos) {
				for(TSNode Up = Parent; !ts_node_is_null(Up); Up = ts_node_parent(Up)) {
					// 祖先の先頭字句
					const TSNode Head = ts_node_child(Up, 0);
					// 括弧を開かない祖先の透過
					if(ts_node_is_named(Head)) continue;
					// 型引数列内の識別
					ShouldKeep = NodeKind::AngleBracketList.Contains(Up);
					// 括弧境界での終了
					if(ShouldKeep || IsOpenBracketChar(Src[Src.Start(Head)])) break;
				}
			}
			// 変数・関数宣言へ化け得る関数形式の括弧の保護（多重括弧は最内の１組だけ）
			if(
				!ShouldKeep && Frame.IsInCppDeclaration && InnerType != "parenthesized_expression" &&
				HasCppDeclarationHead(Src, InnerNode, CppDeclarationHeads)
				// 宣言化し得る関数形式の保持
			) ShouldKeep = true;
			// 実タプル・ラベル・複文の括弧保護
			if(!ShouldKeep && TypeView != "parenthesized_expression") if(
				const bool IsSeparated = HasChildOf(
					Node,
					[&Src](const TSNode Child) -> bool {
						// 名前付子（被演算子）は区切り対象外の為，偽の返戻
						if(ts_node_is_named(Child)) return false;
						// 無名字句の区切候補
						const std::string_view Tok = Src.View(Child);
						// タプル・ラベル・複文の区切り字句かの返戻
						return Tok == "," || Tok == ":" || Tok == ";";
					}
				); ts_node_is_null(InnerNode) || IsSeparated // 実タプルかラベルの括弧保持
			) ShouldKeep = true;
			TSNode MacroArguments {};
			if(!ShouldKeep && IsCFamilyLang) for(TSNode Host = Parent; !ts_node_is_null(Host); Host = ParentNode(Host)) {
				if(std::string_view(ts_node_type(Host)) == "argument_list") {
					// マクロ候補の実引数列
					MacroArguments = Host;
					// 最内実引数列の採用
					break;
				}
				// 実引数端を離れた探索の終了
				if(Src.Start(Host) != Src.Start(Node) && Src.End(Host) != Src.End(Node)) break;
			}
			// 実引数端の到達時だけの呼出名のマクロ性確認
			if(!ts_node_is_null(MacroArguments)) {
				if(
					// 実引数列を所有する呼出
					const TSNode Call = ParentNode(MacroArguments);
					!ts_node_is_null(Call) && std::string_view(ts_node_type(Call)) == "call_expression"
				) if(const TSNode Callee = ts_node_named_child(Call, 0); !ts_node_is_null(Callee)) {
					// 宣言済関数を除く全大文字名と，同一ファイルの関数形式マクロ・別名の保護
					if(
						// 呼出対象の名前
						const std::string_view Name = Src.View(Callee);
						IsMacroStyleName(Name) && !Src.IsDeclaredName(Name, Src.Start(Callee)) || Src.IsFunctionMacro(Name)
						// マクロ実引数端の括弧保持
					) ShouldKeep = true;
				}
			}
			// C# 補間の書式と紛れる最上位 : の括弧保護（括弧対・文字列の内部は除外）
			if(!ShouldKeep && Language == Lang::CSharp && ParentType == "interpolation" && !ts_node_is_null(InnerNode)) {
				WalkChildrenCursor(
					InnerNode,
					[&](const TSNode Cur) -> bool {
						// 子孫字句の走査
						if(!ts_node_is_named(Cur)) {
							// 最上位コロンの記録
							ShouldKeep = ShouldKeep || Src.View(Cur).find(':') != std::string_view::npos;
							// 字句の内側へ降りない事の返戻
							return false;
						}
						const TSNode First = ts_node_child(Cur, 0);
						const std::string_view Lead = ts_node_is_null(First) ? std::string_view() : Src.View(First);
						// 保持が決まった後と，閉じる対の中・文字列の中へ降りない事の返戻
						return !ShouldKeep && Lead != "(" && Lead != "[" && Lead != "{" &&
						std::string_view("\"'@$").find(Src[Src.Start(Cur)]) == std::string_view::npos;
					}
				);
			}
			// 後続が区切だけの値位置の括弧除去（辞書鍵・代入左辺等の後続を取る位置は保持）
			const std::string_view FollowingToken = ts_node_is_null(Frame.NextSibling) ? std::string_view() : Src.View(Frame.NextSibling);
			const bool IsClosedValueSlot = NodeKind::ParenValueSlot.Contains(ParentType) && (
				FollowingToken.empty() ||
				FollowingToken.size() == 1 && std::string_view(",;)]}").find(FollowingToken) != std::string_view::npos ||
				FollowingToken == ":" && ParentType == "ternary_expression" || FollowingToken == ":]"
			);
			// 区切直前の C++26 反映式の括弧除去
			if(
				const bool IsClosedReflect = InnerType == "reflect_expression" &&
				std::string_view(",;)]}").find(Src[TextEdit::SkipSpRight(Src, Src.End(Node))]) != std::string_view::npos;
				!ShouldKeep && !InnerType.empty() && (
					ParentType == "expression_statement" && NodeKind::ExprStmtLiteral.Contains(InnerType) ||
					NodeKind::ParenInnerNoUnwrap.Contains(InnerType) &&
					!(NodeKind::GreedyTailExpression.Contains(InnerType) && (IsClosedValueSlot || IsClosedReflect)) &&
					!(InnerType == "sequence_expression" && ParentType == "for_in_statement" && Delimiter == "in") ||
					ParentType == "fold_expression"
				)
			) ShouldKeep = true;
			// 左端の多重代入・セイウチの括弧保護（括弧・前置字句の内側へは降りない）
			if(!ShouldKeep && !ts_node_is_null(InnerNode)) {
				for(
					// 内側式の左端候補
					TSNode InnerFirst = ts_node_named_child(InnerNode, 0);
					!ts_node_is_null(InnerFirst) && Src.Start(InnerFirst) == Src.Start(InnerNode);
					InnerFirst = ts_node_named_child(InnerFirst, 0)
				) if(NodeKind::ParenInnerFirstChildNoUnwrap.Contains(InnerFirst)) {
					// 左端制約による括弧保持
					ShouldKeep = true;
					// 左端制約発見時の終了
					break;
				}
			}
			// JS/TS の式が塊・宣言・文字列指令へ化ける先頭括弧の保護
			if(!ShouldKeep && IsJsTsLang && !ts_node_is_null(InnerNode)) {
				// JS 文頭と for-in の括弧保持
				ShouldKeep = ReadsAsJsStatementHead(Src, Node, Parent, InnerNode) || Frame.IsInForInitializer && ShieldsJsForIn(Src, InnerNode);
			}
			// 代入先・増減対象・for-in 左辺の型表明の括弧保護（式の結合と構文を維持）
			if(!ShouldKeep && Language == Lang::TypeScript && !ts_node_is_null(InnerNode)) {
				ShouldKeep = NodeKind::TsAssertion.Contains(InnerType) && (
					(NodeKind::AssignmentExpression.Contains(ParentType) || ParentType == "for_in_statement") &&
					ts_node_eq(TSSource::FieldChild(Parent, "left"), Node) || ParentType == "update_expression"
				);
			}
			// 要素間の比較・シフトが型引数と紛れる括弧の保護（除去でコンマの帰属が変わる為）
			if(!ShouldKeep && (Language == Lang::TypeScript || Language == Lang::CSharp || Language == Lang::Cpp)) {
				// 型引数に似る一覧要素
				const bool IsListElement = NodeKind::TypeArgumentLookalikeSlot.Contains(ParentType);
				const auto HasLaterClose = [&]() -> bool {
					const TSNode List = ParentType == "argument" ? ts_node_parent(Parent) : Parent;
					// 一覧毎の閉じ角括弧位置の控え
					const auto [Slot, IsNew] = LastCloseAngleOf.try_emplace(List.id, 0);
					if(IsNew) if(const size_t At = Src.View(List).rfind('>'); At != std::string_view::npos) {
						// 絶対位置への換算
						Slot->second = Src.Start(List) + static_cast<uint32_t>(At) + 1;
					}
					// 後続閉じ角括弧の有無の返戻
					return Slot->second > Src.End(Node);
				};
				WalkChildrenCursor(
					Node,
					[&](const TSNode Cur) -> bool {
						// 保持が決まった後と，字句の内側へ降りない事の返戻
						if(ShouldKeep || !ts_node_is_named(Cur)) return false;
						// 子孫候補の型名
						const std::string_view Type = ts_node_type(Cur);
						if(NodeKind::AngleBracketList.Contains(Type)) {
							ShouldKeep =
							Src.End(Cur) < Src.size() && (Src[Src.End(Cur)] == '>' || Src[Src.End(Cur)] == '=') && HasUnnamedTokenChild(Src, Cur, ",");
							// 読み違えた型引数の並びの中のコンマで保持を決め，中へ降りない事の返戻
							return false;
						}
						// 比較式と閉じ角括弧の衝突判定
						ShouldKeep =
						IsListElement && Type == "binary_expression" && Src.View(TSSource::FieldChild(Cur, "operator")) == "<" && HasLaterClose();
						// 子孫区画の先頭字句
						const TSNode First = ts_node_child(Cur, 0);
						const std::string_view Lead = ts_node_is_null(First) ? std::string_view() : Src.View(First);
						// 対の外側だけへの降下返戻
						return Lead != "(" && Lead != "[" && Lead != "{";
					}
				);
			}
			// C/C++ の型と変数の判別不能な括弧の保護（キャストに読めない後続・多重層は除外）
			if(!ShouldKeep && IsCFamilyLang) {
				const uint32_t Follower = TextEdit::SkipSpRight(Src, Src.End(Node));
				ShouldKeep = NodeKind::CCastInner.Contains(InnerType) ||
				InnerType == "identifier" && Depth == 1 && std::string_view("*&-+").find(Src[Follower]) != std::string_view::npos &&
				Src[Follower + 1] != '=' && Src.compare(Follower, 2, "->") && !(
					ParentType == "update_expression" && ts_node_eq(ts_node_child(Parent, 0), Node) &&
					std::string_view("*(").find(Src[TextEdit::SkipSpRight(Src, Src.End(Parent))]) == std::string_view::npos
				);
			}
			// 展開後の結合を変えるマクロ括弧の保護（宣言済名と return 全体は除き，throw は保持）
			if(!ShouldKeep && IsCFamilyLang && !NodeKind::ReturnStatement.Contains(ParentType)) {
				// 括弧内の識別子名
				const std::string_view InnerName = InnerType == "identifier" ? Src.View(InnerNode) : std::string_view();
				ShouldKeep = Src.MentionsExpressionMacro(Node) || IsMacroStyleName(InnerName) && (
					!Src.IsDeclaredName(InnerName, Src.Start(InnerNode)) || ts_node_eq(ts_node_child(Parent, 0), Node) &&
					(NodeKind::PostfixReceiverHost.Contains(ParentType) || NodeKind::PostfixOrUpdateExpression.Contains(ParentType))
				);
			}
			// 二項式をキャストと誤読した外側括弧の保護（C# の名前括弧は補修済）
			if(!ShouldKeep && (IsCFamilyLang || Language == Lang::CSharp) && InnerType == "cast_expression") {
				if(
					// キャストの値節点
					const TSNode Value = ts_node_named_child(InnerNode, ts_node_named_child_count(InnerNode) - 1);
					!ts_node_is_null(Value) && std::string_view("-+*&^").find(Src[Src.Start(Value)]) != std::string_view::npos &&
					(IsCFamilyLang || std::string_view(ts_node_type(TSSource::FieldChild(InnerNode, "type"))) == "generic_name")
					// 誤読キャスト外側の括弧保持
				) ShouldKeep = true;
			}
			const std::string_view HostType = ts_node_is_null(Frame.OpAncestor) ? std::string_view() : ts_node_type(Frame.OpAncestor);
			if(
				!ShouldKeep && Language == Lang::Cpp && HostType == "init_declarator" && NodeKind::DecltypeIdExpression.Contains(InnerType)
			) {
				if(
					// 初期化宣言の型欄
					const TSNode Type = TSSource::FieldChild(ts_node_parent(Frame.OpAncestor), "type");
					!ts_node_is_null(Type) && std::string_view(ts_node_type(Type)) == "placeholder_type_specifier"
				) ShouldKeep = std::string_view(ts_node_type(ts_node_named_child(Type, 0))) == "decltype";
			}
			// decltype(auto) と PHP の参照返戻で，値と参照を変える括弧の保持
			if(
				!ShouldKeep && (
					Language == Lang::Cpp && HostType == "return_statement" && NodeKind::DecltypeIdExpression.Contains(InnerType) ||
					Language == Lang::PHP && NodeKind::ReturnValueParent.Contains(ParentType)
				)
			) {
				// 返戻値を所有する祖先候補
				TSNode Host = Language == Lang::Cpp ? Frame.OpAncestor : Parent;
				// 返戻主体への遡行
				while(!ts_node_is_null(Host) && !NodeKind::ReturnValueHost.Contains(Host)) Host = ts_node_parent(Host);
				if(!ts_node_is_null(Host)) {
					ShouldKeep = Language == Lang::PHP ? !ts_node_is_null(FirstNamedChildOfType(Host, "reference_modifier")) : HasChildOf(
						Host,
						[&Host](const TSNode Child) -> bool {
							// 本体以外（戻値の型・後置の戻値の型）に decltype(auto) を持つかの返戻
							return ts_node_is_named(Child) && !ts_node_eq(Child, TSSource::FieldChild(Host, "body")) && HasDescendantOf(
								Child,
								[](const TSNode Descendant) -> bool {
									// decltype(auto) 子孫の返戻
									return NamedTypeOf(Descendant) == "decltype" && NamedTypeOf(ts_node_named_child(Descendant, 0)) == "auto";
								}
							);
						}
					);
				}
			}
			// Python/Ruby の継続改行の括弧保持（構文内で閉じる改行は除外して冪等性を維持）
			if(!ShouldKeep && (Language == Lang::Python || Language == Lang::Ruby && !NodeKind::RubyBlock.Contains(InnerType))) {
				if(
					// 継続改行確認の原文範囲
					const uint32_t Start = Src.Start(Node), End = Src.End(Node);
					Start < End && End <= Src.size() && std::memchr(Src.data() + Start, '\n', End - Start)
				) ShouldKeep = true;
			}
			// Ruby の defined? の密着と not の結合変化・構文破壊を防ぐ括弧の保持
			if(!ShouldKeep && Language == Lang::Ruby && NodeKind::UnaryPreOrUpdate.Contains(ParentType)) {
				if(const TSNode Operator = ts_node_child(Parent, 0); !ts_node_is_null(Operator) && !ts_node_is_named(Operator)) {
					// Ruby 単項演算子の字面
					const std::string_view Word = Src.View(Operator);
					ShouldKeep = Word == "defined?" ||
					Word == "not" && Src.End(Operator) == Src.Start(Node) && !NodeKind::RubyStatementHost.Contains(ParentNode(Parent));
				}
			}
			// Ruby の数値を包む括弧の保持（符号付リテラル化で後続呼出の対象が変わる為）
			if(!ShouldKeep && Language == Lang::Ruby && !ts_node_is_null(InnerNode)) {
				TSNode Leaf = InnerNode;
				// 左端葉への降下
				while(ts_node_child_count(Leaf)) Leaf = ts_node_child(Leaf, 0);
				if(NodeKind::NumberLiteral.Contains(Leaf)) {
					for(TSNode Up = Node, Above = Parent; !ts_node_is_null(Above); Up = Above, Above = ParentNode(Above)) {
						if(std::string_view(ts_node_type(Above)) == "unary") {
							// 祖先単項の符号
							const std::string_view Sign = Src.View(ts_node_child(Above, 0));
							// 符号付値の境界保持
							ShouldKeep = (Sign == "-" || Sign == "+") && !(ts_node_eq(Up, Node) && ts_node_eq(Leaf, InnerNode));
							// 最初の単項祖先での終了
							break;
						}
						// 左端祖先経路から外れた終了
						if(!ts_node_eq(ts_node_child(Above, 0), Up)) break;
					}
				}
			}
			// 内包と三項の衝突・PHP の instanceof 右辺と三項ネストに必要な括弧の保持
			if(
				!ShouldKeep && (
					ParentType == "if_clause" && InnerType == "conditional_expression" || Language == Lang::PHP && (
						ParentType == "binary_expression" && !ts_node_is_null(PrevSibling) && Src.View(PrevSibling) == "instanceof" ||
						InnerType == "conditional_expression" && ParentType == "conditional_expression"
					)
				)
			) ShouldKeep = true;
			// Rust の文頭の塊式の括弧保持（閉じ } で文が終わり後続が別文へ離れる為）
			if(!ShouldKeep && Language == Lang::Rust && !ts_node_is_null(InnerNode)) {
				// 式の左端候補
				TSNode Lead = InnerNode;
				while(!NodeKind::RustBlockLike.Contains(Lead) && ts_node_child_count(Lead) && ts_node_is_named(ts_node_child(Lead, 0))) {
					// 左端の塊式への降下
					Lead = ts_node_child(Lead, 0);
				}
				if(NodeKind::RustBlockLike.Contains(Lead)) {
					TSNode Top = Node;
					// 同一開始位置の祖先走査
					for(TSNode Up = Parent; !ts_node_is_null(Up) && Src.Start(Up) == Src.Start(Node); Up = ParentNode(Up)) {
						// 文頭祖先での停止
						if(NodeKind::RustStatementHead.Contains(Up)) break;
						// 同位置祖先への更新
						Top = Up;
					}
					// 式全体の親
					const TSNode Host = ParentNode(Top);
					ShouldKeep = !ts_node_is_null(Host) && NodeKind::RustStatementHead.Contains(Host) &&
					(Src.End(Top) > Src.End(Node) || Src.End(InnerNode) > Src.End(Lead));
				}
			}
			// Python の数値と見出の本体に紛れる式の括弧保持
			if(
				!ShouldKeep && (
					Language == Lang::Python && ParentType == "attribute" && NodeKind::PythonNumber.Contains(InnerType) ||
					Frame.IsInBraceHeader && (Language != Lang::Swift || NodeKind::SwiftHeaderExpressionHost.Contains(ParentType)) &&
					HasBodyLikeExpression(Src, Node, Language)
				)
			) ShouldKeep = true;
			// Rust の見出末尾の値無脱出式の括弧保持（本体を値へ取り込ませない）
			if(!ShouldKeep && Frame.IsInBraceHeader && Language == Lang::Rust) {
				for(TSNode Edge = InnerNode; !ts_node_is_null(Edge); Edge = RightEdgeChild(Src, Edge)) {
					if(
						// 右端脱出式の引数数
						const uint32_t Count = ts_node_named_child_count(Edge);
						NodeKind::RustValueJump.Contains(Edge) &&
						(!Count || Count == 1 && std::string_view(ts_node_type(ts_node_named_child(Edge, 0))) == "label")
					) {
						// 値無脱出式の境界保持
						ShouldKeep = true;
						// 脱出式発見時の終了
						break;
					}
				}
			}
			// Python の await の被演算子とデバッグ差込に必要な括弧の保持
			if(
				!ShouldKeep && (
					Language == Lang::Python && ParentType == "await" && (InnerType == "await" || NodeKind::UnaryPre.Contains(InnerType)) ||
					Frame.IsInDebugInterp
				)
			) ShouldKeep = true;
			// TS の型引数具体化の括弧保護（後続字句で比較・呼出等へ読みが変わる場合）
			if(
				!ShouldKeep && Language == Lang::TypeScript && !ts_node_is_null(InnerNode) && (
					FollowingToken.starts_with('<') || FollowingToken.starts_with('>') || FollowingToken == "!" || FollowingToken == "+" ||
					FollowingToken == "-" || FollowingToken == "." || FollowingToken == "?." || FollowingToken == "[" || FollowingToken == "(" ||
					FollowingToken.starts_with('`')
				)
			) {
				for(TSNode Edge = InnerNode; !ts_node_is_null(Edge); Edge = RightEdgeChild(Src, Edge)) {
					if(std::string_view(ts_node_type(Edge)) == "instantiation_expression") {
						// 型引数具体化の境界保持
						ShouldKeep = true;
						// 具体化式発見時の終了
						break;
					}
				}
			}
			// 単項と値の記号融合を防ぐ最内括弧の保持
			if(
				!ShouldKeep && !ts_node_is_null(InnerNode) && !ts_node_is_null(Frame.OpAncestor) &&
				NodeKind::UnaryPreOrUpdate.Contains(Frame.OpAncestor)
			) {
				if(
					const TSNode Operator = ts_node_child(Frame.OpAncestor, 0);
					!ts_node_is_named(Operator) && FusesPrefixOperator(Language, Src[Src.End(Operator) - 1], Src[Src.Start(InnerNode)])
				) ShouldKeep = true;
			}
			// Swift の単項・範囲の記号融合と try/await の結合変化の防止（補間内の空白も保持）
			if(!ShouldKeep && Language == Lang::Swift && !ts_node_is_null(InnerNode)) {
				const auto IsOperatorSide = [&Src](const TSNode Side) -> bool {
					// 隣接演算子の有無の返戻
					return !ts_node_is_null(Side) && IsOperatorChar(Src[Src.Start(Side)]);
				};
				// 同じ開始位置を持つ最上位候補
				TSNode Top = Node;
				// 同位置祖先への更新
				for(TSNode Up = Parent; !ts_node_is_null(Up) && Src.Start(Up) == Src.Start(Node); Up = ParentNode(Up)) Top = Up;
				// 祖先の直前の兄弟は，親を表から引いて其の子の並びから求める（`ts_node_prev_sibling` は根から降り直す）
				const TSNode Host = ParentNode(Top);
				TSNode Before = ts_node_eq(Top, Node) ? Frame.PrevSibling : TSNode{};
				if(!ts_node_eq(Top, Node) && !ts_node_is_null(Host)) {
					ForEachChild(
						Host,
						[&](const TSNode Sibling) -> bool {
							// 祖先に達した所で走査を打ち切る事の返戻
							if(ts_node_eq(Sibling, Top)) return false;
							// 直前兄弟候補の更新
							Before = Sibling;
							// 祖先へ達する迄走査を続ける事の返戻
							return true;
						}
					);
				}
				ShouldKeep = NodeKind::AwaitTryCastExpression.Contains(InnerType) ||
				NodeKind::RangeOrUnaryExpression.Contains(ParentType) && IsOperatorSide(Frame.NextSibling) &&
				IsOperatorChar(Src[Src.End(InnerNode) - 1]) ||
				!ts_node_is_null(Host) && NodeKind::RangeOrUnaryExpression.Contains(Host) && !ts_node_is_null(Before) &&
				(IsOperatorChar(Src[Src.End(Before) - 1]) || Src[Src.End(Before) - 1] == '.') && IsOperatorChar(Src[Src.Start(InnerNode)]) ||
				Frame.IsInInterp && (
					IsOperatorChar(Src[Src.Start(Node) - 1]) && IsOperatorChar(Src[Src.Start(InnerNode)]) ||
					IsOperatorChar(Src[Src.End(Node)]) && IsOperatorChar(Src[Src.End(InnerNode) - 1])
				);
			}
			if(!ShouldKeep) {
				const TSNode OpParent = Frame.OpAncestor, OperandNode = Frame.OperandTop, Inner = ts_node_is_null(InnerNode) ? Node : InnerNode;
				int InnerPrec = PrecedenceOf(Inner);
				const bool IsIterationHeaderParent = !ts_node_is_null(OpParent) && NodeKind::IterationHeader.Contains(OpParent);
				// Ruby の引数・要素・条件で構文に要る and/or の括弧保持（反復対象は除外）
				if(Language == Lang::Ruby && InnerPrec == 1 && !IsIterationHeaderParent) ShouldKeep = true;
				// 区切だけが続く値位置での右端式の優先順位比較除外
				if(!ShouldKeep && !(IsClosedValueSlot && NodeKind::GreedyTailExpression.Contains(InnerType))) {
					// 優先順位不明の親中置演算子に対する括弧保持
					if(const int ParentPrec = ts_node_is_null(OpParent) ? 0 : PrecedenceOf(OpParent); ParentPrec) {
						// 優先順位不明の内側中置演算子に対する括弧保持
						const bool IsInnerUnknownOp = !InnerPrec && NodeKind::InfixOp.Contains(Inner);
						// 未知優先順位の番兵化
						if(!InnerPrec) InnerPrec = UnknownInnerPrec;
						// 累乗より弱い単項の内側の括弧除去（単項を左に置けない JS/TS と誤読する PHP は保持）
						const std::string_view Sign = ParentPrec == 15 && InnerPrec == 14 ? Src.View(ts_node_child(OpParent, 0)) : std::string_view();
						const bool IsSignOverPower =
						Language == Lang::Python && (Sign == "-" || Sign == "+" || Sign == "~") || Language == Lang::Ruby && Sign == "-";
						// 優先順位差による保持結果
						ShouldKeep = InnerPrec < ParentPrec && !IsSignOverPower || IsInnerUnknownOp;
						// 結合方向と逆側隣接の判定に共用する，親の左被演算子かの判定
						const bool IsLeftOperand = ts_node_eq(ts_node_named_child(OpParent, 0), OperandNode);
						const bool IsPower =
						ParentPrec == 14 && (Language == Lang::Python || IsJsTsLang || Language == Lang::Ruby || Language == Lang::PHP);
						// 代入左辺の条件式の括弧保持（代入が偽の枝へ入る事を防止）
						if(
							!ShouldKeep && IsLeftOperand && !IsIterationHeaderParent &&
							(Language == Lang::Ruby ? ParentPrec == 2 && InnerPrec == 4 : ParentPrec == 1 && InnerPrec == 2)
						) ShouldKeep = true;
						// 変換先の型が後続字句を取る括弧の保護（TS/Swift は取り込まなければ除去）
						if(IsLeftOperand && NodeKind::TypeCastRight.Contains(Inner)) {
							const std::string_view NextToken = Src.View(ts_node_next_sibling(OperandNode));
							const bool IsAbsorbed =
							NextToken.starts_with('<') || IsJsTsLang && (NextToken == "&" || NextToken == "|" || NextToken == "?") ||
							Language == Lang::Swift && NextToken == "&";
							// 逆側の同格以上の演算子の被演算子を変換へ取り込ませない括弧の保持
							ShouldKeep = (IsJsTsLang || Language == Lang::Swift) && NodeKind::InfixOp.Contains(OpParent) ?
							IsAbsorbed || OppositePrecOf(OpParent, IsLeftOperand) >= InnerPrec :
							ShouldKeep || IsAbsorbed;
						}
						// 内側右端の弱い式が親の右辺を取り込む括弧の保持（後続無の前置単項は除外）
						if(
							!ShouldKeep && IsLeftOperand && !ts_node_eq(ts_node_named_child(OpParent, ts_node_named_child_count(OpParent) - 1), OperandNode)
						) for(TSNode Edge = RightEdgeChild(Src, Inner); !ts_node_is_null(Edge); Edge = RightEdgeChild(Src, Edge)) {
							// 右端式の優先順位
							const int EdgePrec = PrecedenceOf(Edge);
							// 非演算子右端での終了
							if(!EdgePrec) break;
							if(EdgePrec < ParentPrec) {
								// 弱い右端式の境界保持
								ShouldKeep = true;
								// 弱い右端発見時の終了
								break;
							}
						}
						// 累乗の左辺の単項の括弧保持（結合変化と JS/TS の構文破壊を防止）
						if(!ShouldKeep && IsPower && IsLeftOperand && InnerPrec == 15) ShouldKeep = true;
						// 演算子字句の優先度に基付く，同格の結合方向の保持
						if(!ShouldKeep && InnerPrec == ParentPrec && !IsIterationHeaderParent) {
							// 非結合の比較・シフト・範囲と，TS の型引数呼出に紛れる比較の括弧保持
							const bool IsNonAssoc = (Language == Lang::Python || Language == Lang::Rust || Language == Lang::Swift) && ParentPrec == 7 ||
							InnerType == "range_expression" || Language == Lang::Swift && ParentPrec == 18 || Language == Lang::Ruby && ParentPrec == 8 ||
							Language == Lang::PHP && (ParentPrec == 9 || ParentPrec == 10) ||
							Language == Lang::TypeScript && ParentPrec == 10 && IsLeftOperand &&
							Src.View(TSSource::FieldChild(OpParent, "operator")) == ">" && Src.View(TSSource::FieldChild(Inner, "operator")) == "<" &&
							OpensTsTypeArguments(Src, TSSource::FieldChild(OpParent, "right"), Language);
							const bool IsRightAssoc =
							IsPower || (Language == Lang::Ruby ? ParentPrec == 2 || ParentPrec == 4 : ParentPrec == 1 || ParentPrec == 2) ||
							Language == Lang::Swift && ParentPrec == 8 || Language == Lang::CSharp && ParentPrec == 3;
							// Python の三項を置けない条件部の括弧保持（最後の被演算子以外）
							if(
								const bool IsPythonTernaryInner =
								Language == Lang::Python && std::string_view(ts_node_type(OpParent)) == "conditional_expression" &&
								!ts_node_eq(ts_node_named_child(OpParent, ts_node_named_child_count(OpParent) - 1), OperandNode);
								IsNonAssoc || IsPythonTernaryInner || ParentPrec != 15 && (IsRightAssoc ? IsLeftOperand : !IsLeftOperand)
							) ShouldKeep = true;
						}
						// 言語固有の同格演算子・誤読・構文制約と逆側の結合を保つ括弧の保持
						if(
							!ShouldKeep && (
								Language == Lang::Ruby && ParentPrec == 10 && InnerPrec == 10 ||
								(IsJsTsLang || Language == Lang::Cpp) && (ParentPrec == 9 || ParentPrec == 10) && InnerPrec == 11 ||
								IsJsTsLang && (ParentPrec == 3 ? InnerPrec == 4 || InnerPrec == 5 : InnerPrec == 3 && (ParentPrec == 4 || ParentPrec == 5)) ||
								OppositePrecOf(OpParent, IsLeftOperand) > InnerPrec
							)
						) ShouldKeep = true;
					} else if(
						!ts_node_is_null(OpParent) && NodeKind::InfixOp.Contains(OpParent) &&
						(ts_node_is_null(InnerNode) || !IsPostfixAtom(Src, InnerNode, Language))
						// 優先順位の分からない中置の演算子でも，原子式の被演算子は結合を争わない為に外す
					) ShouldKeep = true;
				}
			}
			if(
				// 括弧節点の原文範囲
				const uint32_t Start = Src.Start(Node), End = Src.End(Node);
				!ShouldKeep && Start < End && Src[Start] == '(' && Src[End - 1] == ')'
			) {
				// 外側位置で冗長となる Python 組括弧の同時除去
				if(IsPython && NodeKind::TupleLike.Contains(InnerType) && CanBarePythonTuple(Src, InnerNode, Parent, Node)) {
					// 内側組括弧の処理済登録
					NestedLayers.insert(InnerNode.id);
					// 同時除去層数の加算
					++Depth;
				}
				CollectParenPairEdit(
					Src,
					Start,
					End,
					Depth,
					NodeKind::UnaryPreOrUpdate.Contains(ParentType) ? ts_node_child(Parent, 0) : TSNode{},
					true,
					Edits
				);
				// 括弧に依存する Kotlin 継続改行の同時縮約
				if(Language == Lang::Kotlin && !ts_node_is_null(InnerNode)) CollapseParenLines(Src, InnerNode, Edits);
				// Ruby の受け手括弧除去時の後置字句前空白の除去
				if(
					Language == Lang::Ruby && NodeKind::PostfixReceiverHost.Contains(ParentType) && ts_node_eq(ts_node_named_child(Parent, 0), Node)
				) {
					if(const uint32_t Next = TextEdit::SkipSpRight(Src, End); Next > End && Next < Src.size() && Src[Next] != '\n') {
						// 後置字句前空白の除去予約
						TextEdit::Push(End, Next, "", Edits);
					}
				}
				// Ruby の裸実引数と紛れる演算子後の空白補完（元から密着する二項式は保持）
				if(Language == Lang::Ruby && ParentType == "binary" && ts_node_eq(ts_node_named_child(Parent, 0), Node)) {
					if(
						const TSNode Operator = TSSource::FieldChild(Parent, "operator");
						!ts_node_is_null(Operator) && End < Src.Start(Operator) && NodeKind::RubySpacedBinary.Contains(Src.View(Operator)) &&
						Src.End(Operator) < Src.size() && Src[Src.End(Operator)] != ' ' && Src[Src.End(Operator)] != '\t'
						// 二項演算子後空白の補完予約
					) TextEdit::Push(Src.End(Operator), Src.End(Operator), " ", Edits);
				}
				// Ruby の三項 ? の前後の空白補完（名前の一部・文字リテラルとの誤読防止）
				if(Language == Lang::Ruby && ParentType == "conditional" && ts_node_eq(ts_node_named_child(Parent, 0), Node)) {
					if(const TSNode Mark = Frame.NextSibling; !ts_node_is_null(Mark) && Src.View(Mark) == "?") {
						// 三項疑問符前空白の補完予約
						if(Src.Start(Mark) == End) TextEdit::Push(End, End, " ", Edits);
						if(const uint32_t After = Src.End(Mark); After < Src.size() && Src[After] != ' ' && Src[After] != '\t' && Src[After] != '\n') {
							// 三項疑問符後空白の補完予約
							TextEdit::Push(After, After, " ", Edits);
						}
					}
				}
				// 外層保持時の内層除去予約
			} else if(Depth > 1) CollectNestedLayerEdit(Src, Node, InnerNode, Edits);
		}
		// `SizeofParenEdits`：`sizeof` 引数の括弧付与
		if(IsCFamilyLang && TypeView == "sizeof_expression" && ts_node_named_child_count(Node) == 1) {
			if(const TSNode Arg = ts_node_named_child(Node, 0); !NodeKind::SizeofParenArgument.Contains(Arg)) {
				if(
					// sizeof 後の空白を越えた既存括弧の確認
					const uint32_t ArgStart = Src.Start(Arg), Probe = TextEdit::SkipCharsLeftBounded(Src, ArgStart, 0, " \t\n");
					!Probe || Src[Probe - 1] != '('
				) {
					const uint32_t ArgEnd = Src.End(Arg);
					TextEdit::Push(TextEdit::SkipSpLeft(Src, ArgStart), ArgStart, "(", Edits);
					TextEdit::Push(ArgEnd, ArgEnd, ")", Edits);
				}
			}
		}
		// Swift の１要素照合の括弧除去（見出は解析器が読める束縛・列挙の場合だけ）
		if(Language == Lang::Swift && (TypeView == "pattern" || NodeKind::SwiftHeaderStatement.Contains(TypeView))) {
			for(size_t ChildIdx = 0; ChildIdx + 2 < AllKids.size(); ++ChildIdx) {
				if(
					// 括弧内照合と先頭子
					const TSNode Pattern = AllKids[ChildIdx + 1], Lead = ts_node_child(Pattern, 0);
					Src.View(AllKids[ChildIdx]) == "(" && !ts_node_is_named(AllKids[ChildIdx]) &&
					std::string_view(ts_node_type(Pattern)) == "pattern" && Src.View(AllKids[ChildIdx + 2]) == ")" && (
						TypeView == "pattern" ? AllKids.size() == 3 : ChildIdx && Src.View(AllKids[ChildIdx - 1]) == "case" && !ts_node_is_null(Lead) &&
						(ts_node_is_named(Lead) ? std::string_view(ts_node_type(Lead)) == "value_binding_pattern" : Src.View(Lead) == ".")
					)
				) {
					// 照合の開き括弧除去予約
					TextEdit::Push(Src.Start(AllKids[ChildIdx]), Src.End(AllKids[ChildIdx]), "", Edits);
					TextEdit::Push(Src.Start(AllKids[ChildIdx + 2]), Src.End(AllKids[ChildIdx + 2]), "", Edits);
				}
			}
		}
		// Python の裸で置ける組の括弧除去（１要素・コンマ無の包装層も透過して判定）
		if(
			IsPython && !IsNestedLayer && NodeKind::TupleLike.Contains(TypeView) && !ts_node_is_null(Parent) &&
			CanBarePythonTuple(Src, Node, Parent, Node)
		) {
			// Python 組の連続括弧層数
			uint32_t Depth = 1;
			for(TSNode Layer = Node; ts_node_named_child_count(Layer) == 1 && ts_node_child_count(Layer) == 3;) {
				const TSNode Content = ts_node_named_child(Layer, 0);
				// 除去不能層での停止
				if(!NodeKind::TupleLike.Contains(Content) || !CanBarePythonTuple(Src, Content, Parent, Node)) break;
				// 内側組層の処理済登録
				NestedLayers.insert(Content.id);
				// 同時除去層数の加算
				++Depth;
				// 次の内側層への降下
				Layer = Content;
			}
			// 裸組括弧の除去予約
			CollectParenPairEdit(Src, Src.Start(Node), Src.End(Node), Depth, TSNode{}, false, Edits);
		}
		// Swift の前置を呼出と誤読した括弧の除去（強く結合し，記号で始まらない値だけ）
		if(Language == Lang::Swift) if(const TSNode Arguments = SwiftPrefixArguments(Src, Node); !ts_node_is_null(Arguments)) {
			// 前置後の単一実引数
			const TSNode Argument = ts_node_named_child(Arguments, 0);
			const TSNode Value = ts_node_is_null(Argument) ? TSNode{} : ts_node_named_child(Argument, 0);
			if(
				// Swift 前置値の優先順位
				const int ValuePrec = ts_node_is_null(Value) ? 0 : PrecedenceOf(Value);
				ts_node_named_child_count(Arguments) == 1 && ts_node_named_child_count(Argument) == 1 && ts_node_child_count(Arguments) == 3 &&
				(ValuePrec ? ValuePrec > 14 : !NodeKind::InfixOp.Contains(Value)) && !IsOperatorChar(Src[Src.Start(Value)]) &&
				Src[Src.Start(Value)] != '.'
			) {
				TextEdit::Push(Src.Start(Arguments), Src.Start(Arguments) + 1, "", Edits);
				TextEdit::Push(Src.End(Arguments) - 1, Src.End(Arguments), "", Edits);
			}
		}
		// 前置単項と融合する記号始まりの値の括弧化（`- -9` → `-(-9)`）
		if(NodeKind::UnaryPreOrUpdate.Contains(TypeView)) {
			if(
				const TSNode Operator = ts_node_child(Node, 0), Operand = ts_node_child(Node, 1);
				!ts_node_is_named(Operator) && !ts_node_is_null(Operand) &&
				FusesPrefixOperator(Language, Src[Src.End(Operator) - 1], Src[Src.Start(Operand)])
			) {
				// 融合境界への開き括弧挿入予約
				TextEdit::Push(Src.End(Operator), Src.Start(Operand), "(", Edits);
				TextEdit::Push(Src.End(Operand), Src.End(Operand), ")", Edits);
			}
		}
		// 値を使わない文・更新位置だけの増減の前置化（波括弧は別途処理）
		if(NodeKind::PostfixOrUpdateExpression.Contains(TypeView) && !ts_node_is_null(Parent)) {
			// 増減式の親型
			const std::string_view ParentType(ts_node_type(Parent));
			bool IsValidContext = NodeKind::StmtCtxParent.Contains(ParentType);
			if(!IsValidContext && ParentType == "for_statement") {
				TSNode UpdateSlot = TSSource::FieldChild(Parent, "update");
				// JS 系の更新欄
				if(ts_node_is_null(UpdateSlot)) UpdateSlot = TSSource::FieldChild(Parent, "increment");
				// 現節点との一致確認
				IsValidContext = !ts_node_is_null(UpdateSlot) && ts_node_eq(UpdateSlot, Node);
			}
			// Kotlin の値を返す本体・枝の最終文の保持（増減の値を変えない）
			if(IsValidContext && Language == Lang::Kotlin && ParentType == "statements") {
				// Kotlin の値不使用確認
				IsValidContext = !IsKotlinStatementValueUsed(Src, Node);
			}
			// Java switch 式の値を保つ増減前置化の除外
			if(IsValidContext && Language == Lang::Java && ParentType == "expression_statement") {
				if(
					// switch 規則の候補
					const TSNode Rule = ts_node_parent(Parent);
					!ts_node_is_null(Rule) && std::string_view(ts_node_type(Rule)) == "switch_rule"
				) {
					// switch と外側主体
					const TSNode Switch = ts_node_parent(ts_node_parent(Rule)), Host = ts_node_is_null(Switch) ? TSNode{} : ts_node_parent(Switch);
					// 文脈での値不使用確認
					IsValidContext = !ts_node_is_null(Host) && NodeKind::JavaStatementHost.Contains(Host);
				}
			}
			// 値不使用を確定した増減だけの前置化
			if(IsValidContext) MaybeCollectIncrementEdit(Src, Node, Language, Edits);
		}
		// 回数既知の for と条件既知の while の用途を揃える無限ループの変換（for(;;) → while(true)）
		if(TypeView == "for_statement") MaybeCollectInfiniteForEdit(Src, Node, Language, Edits);
		// `BoolLiteralEdits`：真偽値型の初期値 `0` / `1` を `false` / `true` へ正規化
		if(IsBoolLiteralLangOk && NodeKind::BoolInitContext.Contains(TypeView)) MaybeCollectBoolLiteralEdit(Src, Node, Edits);
		// CSS の色の大文字化（参照・ID と構文誤り内のリテラルは除外）
		if(
			Language == Lang::CSS && TypeView == "color_value" && !Frame.IsInError && !Src.IsCssUrlArguments(Parent) &&
			!NodeKind::CssSelectorHost.Contains(
				ts_node_type(std::string_view(ts_node_type(Parent)) == "arguments" ? ts_node_parent(Parent) : Parent)
			)
		) {
			const std::string_view Text = Src.View(Node);
			std::string Upper(Text);
			// 十六進英字の大文字化
			for(char &Char : Upper) if(Char >= 'a' && Char <= 'f') Char = static_cast<char>(Char + 'A' - 'a');
			// 変化時だけの置換予約
			if(Upper != Text) TextEdit::Push(Src.Start(Node), Src.End(Node), std::move(Upper), Edits);
		}
		// 数値の正規化（固定の接頭辞・接尾辞と元字句を受け取る利用者定義リテラルは保持）
		if(
			IsNumLang && !Frame.IsInError && NodeKind::NumberLiteral.Contains(TypeView) && !(
				Language == Lang::Cpp && !ts_node_is_null(Frame.Parent) &&
				std::string_view(ts_node_type(Frame.Parent)) == "user_defined_literal"
			)
		) {
			// 数値リテラルの原文
			const std::string_view Text = Src.View(Node);
			if(std::string Fixed = NormalizeNumericLiteral(Text, Language, IsNumPadLang); Fixed != Text) {
				// 正規化済数値の置換予約
				TextEdit::Push(Src.Start(Node), Src.End(Node), std::move(Fixed), Edits);
			}
		}
		// 単引用符から二重引用符への正規化（内容に二重引用符・脱出を含む物と Python の三重引用符は除外）
		if(
			(IsJsTsLang || Language == Lang::Python || Language == Lang::CSS || Language == Lang::Ruby) && !Frame.IsInError &&
			NodeKind::StringLikeAll.Contains(TypeView)
		) {
			// 文字列節点の終端
			const uint32_t StringEnd = Src.End(Node);
			uint32_t QuotePos = Src.Start(Node);
			// 接頭辞後の引用符探索
			while(QuotePos < StringEnd && Src[QuotePos] != '\'' && Src[QuotePos] != '"') ++QuotePos;
			const bool IsStringValid = StringEnd >= QuotePos + 2 && Src[QuotePos] == '\'' && Src[StringEnd - 1] == '\'';
			const bool IsTripleQuoted = StringEnd >= QuotePos + 3 && Src[QuotePos + 1] == '\'' && Src[QuotePos + 2] == '\'';
			const bool IsInPyInterp = IsStringValid && !IsTripleQuoted && Frame.IsInInterp;
			if(IsStringValid && !IsTripleQuoted && !IsInPyInterp) {
				// 引用符変換の中止標識
				bool ShouldBail = false;
				ForEachNamedChild(
					Node,
					[&](const TSNode Child) -> bool {
						if(const std::string_view ChildType = ts_node_type(Child); ChildType == "escape_sequence") {
							// エスケープによる変換中止
							ShouldBail = true;
							// エスケープを発見→対象外と判断し打切の返戻
							return false;
						} else if(NodeKind::StringContentLeaf.Contains(ChildType) && Src.View(Child).find('"') != std::string_view::npos) {
							// 二重引用符による変換中止
							ShouldBail = true;
							// `"` を含む断片を発見→対象外と判断し打切の返戻
							return false;
						}
						// 未該当の為，次の兄弟へ走査継続の返戻
						return true;
					}
				);
				// CSS の子に現れない引用符と，Ruby の脱出・補間の危険の生バイト判定
				if(!ShouldBail) {
					// Python の f 文字列内の二重引用符と衝突する外側引用符の保持
					if(
						const std::string_view Body(Src.data() + QuotePos + 1, StringEnd - QuotePos - 2);
						(Language == Lang::CSS || Language == Lang::Python) && Body.find('"') != std::string_view::npos ||
						Language == Lang::Ruby && Body.find_first_of("\"\\#") != std::string_view::npos
						// 原文バイト衝突による変換中止
					) ShouldBail = true;
				}
				if(!ShouldBail) {
					// 開始引用符の置換予約
					TextEdit::Push(QuotePos, QuotePos + 1, "\"", Edits);
					TextEdit::Push(StringEnd - 1, StringEnd, "\"", Edits);
				}
			}
		}
		// const の型直前への移動（属性・記憶域の前へ出さず，宣言子マクロの型は保持）
		if(IsCFamilyLang && TypeView == "declaration") {
			TSNode QualNode {}, TypeNode {};
			ForEachNamedChild(
				Node,
				[&](const TSNode Child) -> void {
					const std::string_view ChildType = ts_node_type(Child);
					if(
						const bool IsTypeSpecifier = NodeKind::CppDeclTypeSpecifier.Contains(ChildType);
						IsTypeSpecifier && ts_node_is_null(TypeNode)
						// 最初の型指定子の採用
					) TypeNode = Child;
					else if(
						!IsTypeSpecifier && ChildType == "type_qualifier" && !ts_node_is_null(TypeNode) && Src.View(Child) == "const" &&
						ts_node_is_null(QualNode)
						// 移動対象 const の採用
					) QualNode = Child;
				}
			);
			// 後置 const を抜き，型の直前へ移す
			if(!ts_node_is_null(QualNode) && !Src.MentionsDeclaratorMacro(TypeNode)) {
				// const 周囲の空白範囲
				const uint32_t Left = TextEdit::SkipSpLeft(Src, Src.Start(QualNode)), Right = TextEdit::SkipSpRight(Src, Src.End(QualNode));
				TextEdit::Push(Left, Right, " ", Edits);
				TextEdit::Push(Src.Start(TypeNode), Src.Start(TypeNode), "const ", Edits);
			}
		}
		// C++ 専用の空仮引数 `(void)` の `()` への変換
		if(Language == Lang::Cpp && !Language.IsCShared && TypeView == "parameter_list" && ts_node_named_child_count(Node) == 1) {
			if(
				const TSNode Param = ts_node_named_child(Node, 0);
				std::string_view(ts_node_type(Param)) == "parameter_declaration" && ts_node_named_child_count(Param) == 1
			) {
				if(
					const TSNode Inner = ts_node_named_child(Param, 0);
					std::string_view(ts_node_type(Inner)) == "primitive_type" && Src.View(Inner) == "void"
				) {
					if(
						const uint32_t ParamStart = Src.Start(Node), ParamEnd = Src.End(Node);
						ParamEnd > ParamStart + 1 && Src[ParamStart] == '(' && Src[ParamEnd - 1] == ')'
						// void 仮引数の除去予約
					) TextEdit::Push(ParamStart + 1, ParamEnd - 1, "", Edits);
				}
			}
		}
		// 言語が省略を許す仮引数・実引数括弧の除去
		if(NodeKind::OptionalParenHost.Contains(TypeView)) {
			MaybeCollectOptionalParenEdit(Src, Node, Frame.OpAncestor, Frame.OperandTop, Language, Edits);
		}
		// 型を包む冗長な括弧の除去
		if(!IsNestedLayer && NodeKind::GroupingTypeParen.Contains(TypeView)) {
			MaybeCollectTypeParenEdit(Src, Node, Parent, Frame.NextSibling, Language, NestedLayers, Edits);
		}
		const bool IsKidInTypeAlias = Frame.IsInTypeAlias || IsPython && TypeView == "type_alias_statement";
		const bool IsKidInInterp =
		Frame.IsInInterp || IsPython && TypeView == "interpolation" || Language == Lang::Swift && TypeView == "interpolated_expression";
		// 子へ渡すデバッグ差込内標識
		const bool IsKidInDebugInterp = Frame.IsInDebugInterp || IsPython && TypeView == "interpolation" && HasChildOf(
			Node,
			[&Src](const TSNode Tok) -> bool {
				// 表示用の `=` を持つ差込（デバッグ用 `{式=}`）かの判定の返戻
				return !ts_node_is_named(Tok) && Src.View(Tok) == "=";
			}
		);
		// 見出内外の状態の継承（本体で外へ戻し，子を積む前に１回だけ判定）
		const bool IsKidInBraceHeader = (Language == Lang::Go || Language == Lang::Rust || Language == Lang::Swift) &&
		(NodeKind::Control.Contains(TypeView) || !NodeKind::ScopeBody.Contains(TypeView) && Frame.IsInBraceHeader);
		const bool IsCppCall =
		Frame.IsInCppDeclaration && TypeView == "call_expression" && HasCppDeclarationHead(Src, Node, CppDeclarationHeads);
		for(size_t KidIdx = AllKids.size(); KidIdx;) if(const TSNode Kid = AllKids[--KidIdx]; ts_node_is_named(Kid)) {
			// 宣言への再解釈が届く経路だけを伝播し，return・代入右辺・メンバ呼出へ広げない
			const bool IsKidInCppDeclaration = Language == Lang::Cpp && (
				TypeView == "expression_statement" || TypeView == "init_declarator" && std::string_view(ts_node_type(Kid)) == "argument_list" &&
				ts_node_eq(TSSource::FieldChild(Node, "value"), Kid) || Frame.IsInCppDeclaration && (
					IsNodeParen || TypeView == "argument_list" || IsCppCall && ts_node_eq(TSSource::FieldChild(Node, "arguments"), Kid) ||
					TypeView == "subscript_expression" && ts_node_eq(TSSource::FieldChild(Node, "argument"), Kid) || (
						TypeView == "comma_expression" || TypeView == "assignment_expression" && Src.View(TSSource::FieldChild(Node, "operator")) == "="
					) && ts_node_eq(TSSource::FieldChild(Node, "left"), Kid)
				)
			);
			// 実演算子と直下被演算子の子枠への継承
			WorkStack.push_back(
				{
					Kid,
					Node,
					IsNodeParen ? Frame.OpAncestor : Node,
					IsNodeParen ? Frame.OperandTop : Kid,
					KidIdx ? AllKids[KidIdx - 1] : TSNode{},
					KidIdx + 1 < AllKids.size() ? AllKids[KidIdx + 1] : TSNode{},
					IsKidInTypeAlias,
					IsKidInDebugInterp,
					IsKidInInterp,
					IsKidInCppDeclaration,
					IsKidInBraceHeader,
					IsJsTsLang && (
						TypeView == "for_statement" && ts_node_eq(TSSource::FieldChild(Node, "initializer"), Kid) ||
						Frame.IsInForInitializer && !NodeKind::JsInRestoring.Contains(TypeView)
					),
					Frame.IsInError || TypeView == "ERROR"
				}
			);
		}
	}
	// 終了
	return;
}

/**
 * 複数種別の正規化編集の適用関数
 * @param Src 対象のソース
 * @param Language 対象言語
 */
void EditPass::ApplyConsolidated(TSSource &Src, const Lang Language) {
	// 複数編集種別の統合による再構文解析の一回化
	if(!Src.IsParsed() || Language == Lang::HTML) return;
	const TSNode Root = Src.GetRoot();
	// Kotlin 増減前置化の可否の並列走査前での収集
	if(Language == Lang::Kotlin && (Src.find("++") != std::string::npos || Src.find("--") != std::string::npos)) {
		// Kotlin 文値利用状況の先行収集
		PrepareKotlinValueContexts(Src);
	}
	// 括弧判定用の宣言名・取込位置の収集（編集済の木から並列走査前に構築）
	if(Language.IsCFamily()) Src.CollectDeclaredNames();
	const uint32_t NamedCount = ts_node_named_child_count(Root);
	std::vector<TextEdit> Edits;
	const size_t NumThreads = Parallel::DecideThreads(NamedCount, 8);
	if(NumThreads < 2 || NamedCount < 2) CollectConsolidatedNamedWalk(Src, Root, TSNode{}, TSNode{}, TSNode{}, Language, Edits);
	else {
		std::vector<TSNode> RootChildren;
		std::vector<uint32_t> NamedIndices;
		// 名前付子数分の先行確保
		NamedIndices.reserve(NamedCount);
		ForEachChild(
			Root,
			[&](const TSNode Child) -> void {
				// 名前付子の位置と全子列の記録
				if(ts_node_is_named(Child)) NamedIndices.push_back(static_cast<uint32_t>(RootChildren.size()));
				RootChildren.push_back(Child);
			}
		);
		// 収集中の編集列の走脈別分離
		std::vector<std::vector<TextEdit>> ThreadEdits(NumThreads);
		Parallel::ForChunks(
			NamedIndices.size(),
			NumThreads,
			[&](const size_t Start, const size_t End, const size_t Tid) -> void {
				for(size_t Idx = Start; Idx < End; ++Idx) {
					// 対象子の全子列内位置
					const uint32_t Kid = NamedIndices[Idx];
					CollectConsolidatedNamedWalk(
						Src,
						RootChildren[Kid],
						Root,
						Kid ? RootChildren[Kid - 1] : TSNode{},
						Kid + 1 < RootChildren.size() ? RootChildren[Kid + 1] : TSNode{},
						Language,
						ThreadEdits[Tid]
					);
				}
			}
		);
		size_t Total = 0;
		for(const std::vector<TextEdit> &Local : ThreadEdits) Total += Local.size();
		Edits.reserve(Total);
		for(std::vector<TextEdit> &Local : ThreadEdits) {
			// 局所編集の移動結合
			Edits.insert(Edits.end(), std::make_move_iterator(Local.begin()), std::make_move_iterator(Local.end()));
		}
	}
	// 言語別の２走査を要する収集器の呼出（節点単独の正規化は共通走査へ集約済）
	switch(Language.Id) {
	case Lang::Java:
		// Java final 編集の収集
		CollectJavaFinalEdits(Src, Edits);
		break;
	case Lang::Rust:
		// Rust mut 除去編集の収集
		CollectRustMutRemoveEdits(Src, Edits);
		break;
	case Lang::Kotlin:
		// Kotlin val 化編集の収集
		CollectVarToValEdits(Src, Edits);
		break;
	case Lang::JavaScript:
	case Lang::TypeScript:
		// JS 系 const 化編集の収集
		CollectLetToConstEdits(Src, Edits);
		break;
	case Lang::Ruby:
		// Ruby 明示 return 編集の収集
		CollectRubyExplicitReturnEdits(Src, Edits);
		// Ruby セミコロン編集の収集
		CollectRubySemicolonEdits(Src, Edits);
		break;
	default:
		break;
	}
	// 全言語別編集の一括反映と構文木の更新
	TextEdit::Apply(Src, Edits);
	static thread_local uint32_t RecursionDepth = 0;
	if(!Edits.empty() && Src.IsParsed() && ts_node_has_error(Src.GetRoot()) && !RecursionDepth) {
		// 再入抑止の開始
		++RecursionDepth;
		// 誤り構文木の追加収束走査
		ApplyConsolidated(Src, Language);
		// 再入抑止の解除
		--RecursionDepth;
	}
	// 終了
	return;
}
