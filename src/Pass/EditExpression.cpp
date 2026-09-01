#include "Edit.hpp"
#include "../Util/NodeKind.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/**
 * 演算子の優先順位の取得関数
 * @param Src ソースコード
 * @param OpNode 優先順位を取得する演算子ノード
 * @param Language 対象言語
 * @return 優先順位（1〜18，不明時 0）
 */
int EditPass::OpPrecedence(const TSSource &Src, const TSNode OpNode, const Lang Language) {
	const std::string_view NodeType(ts_node_type(OpNode));
	// Ruby の not/defined? と照合での and/or 格の優先度
	if(
		Language == Lang::Ruby && (
			NodeType == "unary" && NodeKind::RubyLowUnary.Contains(Src.View(ts_node_child(OpNode, 0))) ||
			NodeKind::RubyPatternTest.Contains(NodeType)
		)
		// and / or 格の返戻
	) return 1;
	// 鍵値の : と型引数の山括弧での右端探索停止用優先度
	if(NodeKind::TypeColonParent.Contains(NodeType) || NodeKind::AngleBracketList.Contains(NodeType)) return 0;
	// 語で始まり右端迄取る式での後続取込防止用優先度
	if(NodeKind::OpenPrefixExpression.Contains(NodeType)) return 1;
	// 字句表で誤分類する前置単項は優先度１５を返す（後置の Rust await は除外）
	if(
		NodeKind::KeywordUnary.Contains(NodeType) || (Language.IsJsTs() || Language == Lang::CSharp) && NodeType == "await_expression"
		// 前置単項格の返戻
	) return 15;
	// Swift の前置が中置全体へ掛かる誤読での頂点中置の優先度
	if(Language == Lang::Swift && NodeType == "prefix_expression") {
		// 前置式の被演算子
		if(const TSNode Operand = ts_node_named_child(OpNode, ts_node_named_child_count(OpNode) - 1); !ts_node_is_null(Operand)) {
			// 右の中置式の優先順位の返戻
			if(const int OperandPrec = OpPrecedence(Src, Operand, Language); OperandPrec && OperandPrec < 15) return OperandPrec;
		}
	}
	// 波括弧で閉じる Kotlin/Swift のラムダでの後置優先度
	if(NodeType == "lambda_literal") return 16;
	// Swift の呼出での演算子始まりに応じた前置又は後置優先度
	if(Language == Lang::Swift && NodeType == "call_expression") {
		const TSNode Head = ts_node_child(OpNode, 0);
		// 後置 `!` の有無に応じた優先度の返戻
		return !ts_node_is_named(Head) || std::string_view(ts_node_type(Head)) == "bang" ? 15 : 16;
	}
	// 前置単項（Swift の片側だけの範囲 `..<n` も被演算子へ密着する前置の演算子，前置の増減 `++x` も含む）
	if(
		NodeKind::UnaryPre.Contains(NodeType) || NodeType == "open_start_range_expression" ||
		NodeType == "update_expression" && !ts_node_is_named(ts_node_child(OpNode, 0))
		// 前置単項の優先順位の返戻
	) return 15;
	// 後置の優先順位の返戻（Swift の片側だけの範囲 `n...` も後置の演算子）
	if(NodeKind::PostfixOrUpdateExpression.Contains(NodeType) || NodeType == "open_end_range_expression") return 16;
	// C# の専用ノードも後置式・switch の優先順位で括弧を判定
	if(Language == Lang::CSharp) {
		// 呼出は後置格の返戻
		if(NodeType == "invocation_expression") return 16;
		// switch 式と with 式は累乗格の返戻
		if(NodeKind::SwitchOrWithExpression.Contains(NodeType)) return 14;
	}
	// Python の専用節点からの優先度判定（共通集合の変更で空白判定へ波及させない）
	if(Language == Lang::Python) {
		// await は単項前置（後置メンバアクセスより下，二項より上）の為 15 の返戻
		if(NodeType == "await") return 15;
		// 後置のメンバ参照・呼出の優先度１６の返戻（`(a + b)(c)` の被呼出式の括弧を保護）
		if(NodeKind::PythonPostfix.Contains(NodeType)) return 16;
		// 字句表に無い Python の三項での優先度２
		if(NodeType == "conditional_expression") return 2;
		// not は比較（補正後 7）より下・and (5) より上の為 6 を返戻する（`not (a and b)` / `(not a) == b` の括弧を保持する）
		if(NodeType == "not_operator") return 6;
	}
	// Kotlin 中置関数の優先度１０の返戻（利用者が順位を定める Swift は対象外）
	if(Language == Lang::Kotlin && NodeType == "infix_expression") return 10;
	// Swift の as/as? の優先度の確定（名前付 as_operator は字句走査に掛からず，CastingPrecedence は nil 合体８と範囲１０の間）
	if(Language == Lang::Swift && NodeType == "as_expression") return 9;
	// Ruby のメソッド呼出での受け手の二項式保護用後置優先度
	if(Language == Lang::Ruby && NodeType == "call") return 16;
	static const NodeKind::NodeTypeSet CmpOps = { "!=", "!==", "!~", "<", "<=", "<=>", "==", "===", "=~", ">", ">=" };
	// Swift が利用者定義演算子と読む比較への，字句に依る比較格の付与
	if(Language == Lang::Swift && NodeType == "infix_expression") {
		if(
			// Swift 中置演算子節点
			const TSNode Operator = TSSource::FieldChild(OpNode, "op");
			!ts_node_is_null(Operator) && CmpOps.Contains(Src.View(Operator))
			// 比較の格の返戻
		) return 7;
	}
	static const NodeKind::NodeTypeSet AssignOps =
	{ "%=", "&&=", "&=", "&^=", "**=", "*=", "+=", "-=", "//=", "/=", "<<=", "=", ">>=", ">>>=", "?\?=", "^=", "|=", "||=" };
	static const std::unordered_map<std::string_view, int> Table = {
		{ "?", 2 },
		{ ":", 2 },
		{ "??", 3 },
		{ "?:", 3 },
		{ "rescue", 3 },
		{ "||", 4 },
		{ "or", 4 },
		{ "&&", 5 },
		{ "and", 5 },
		{ "|", 6 },
		{ "^", 7 },
		{ "&", 8 },
		{ "==", 9 },
		{ "!=", 9 },
		{ "===", 9 },
		{ "!==", 9 },
		{ "<=>", 9 },
		{ "=~", 9 },
		{ "!~", 9 },
		{ "not_eq", 9 },
		{ "<", 10 },
		{ ">", 10 },
		{ "<=", 10 },
		{ ">=", 10 },
		{ "in", 10 },
		{ "!in", 10 },
		{ "instanceof", 10 },
		{ "is", 10 },
		{ "!is", 10 },
		{ "is not", 10 },
		{ "not in", 10 },
		{ "as", 10 },
		{ "as?", 10 },
		{ "satisfies", 10 },
		{ "..", 10 },
		{ "...", 10 },
		{ "..=", 10 },
		{ "..<", 10 },
		{ "<<", 11 },
		{ ">>", 11 },
		{ ">>>", 11 },
		{ "+", 12 },
		{ "-", 12 },
		{ "*", 13 },
		{ "/", 13 },
		{ "%", 13 },
		{ "//", 13 },
		{ "&^", 13 },
		{ "@", 13 },
		{ "**", 14 }
	};
	int Result = 0;
	// 対象式の子トークン走査
	ForEachChild(
		OpNode,
		// 各子トークンの優先度判定
		[&](const TSNode Child) -> bool {
			// 名前付の子を読み飛ばす事の返戻
			if(ts_node_is_named(Child)) return true;
			const uint32_t Start = ts_node_start_byte(Child), End = ts_node_end_byte(Child);
			// 空の字句を読み飛ばす事の返戻
			if(Start >= End || Start >= Src.size()) return true;
			const std::string_view Operator(Src.data() + Start, End - Start);
			// 共通優先度表との照合
			if(const std::unordered_map<std::string_view, int>::const_iterator Iter = Table.find(Operator); Iter != Table.end()) {
				// 反復対象の最弱値１と三項・ラムダを取れない Python 内包値３への補正
				if(NodeKind::IterationHeader.Contains(NodeType)) {
					// 反復見出用の優先度へ補正
					Result = NodeType == "for_in_clause" ? 3 : 1;
					// 反復の見出の優先度の確定で走査打切の返戻
					return false;
				}
				// 共通表の優先度を採用
				Result = Iter->second;
				// 演算子字句の優先度の言語別補正（ビット演算を比較より下に置く C 系との差）
				switch(Language.Id) {
				case Lang::Kotlin:
					// Kotlin の範囲・中置関数・エルビス・型判定・比較の順への補正
					if(Operator == "as" || Operator == "as?") Result = 14;
					else if(Operator == ".." || Operator == "..<") Result = 11;
					else if(Operator == "?:") Result = 9;
					else if(Operator == "in" || Operator == "!in" || Operator == "is" || Operator == "!is") Result = 8;
					else if(Operator == "<" || Operator == ">" || Operator == "<=" || Operator == ">=") Result = 7;
					else if(Operator == "==" || Operator == "!=" || Operator == "===" || Operator == "!==") Result = 6;
					// Kotlin の優先度確定
					return false;
				case Lang::Go:
					// Go の優先順位：`<<` / `>>` / `&` / `&^` は乗除格，`|` / `^` は加減格，比較（`==` / `!=` / `<` / `<=` / `>` / `>=`）は同格の左結合
					if(Operator == "<<" || Operator == ">>" || Operator == "&") Result = 13;
					else if(Operator == "|" || Operator == "^") Result = 12;
					else if(CmpOps.Contains(Operator)) Result = 10;
					// Go の優先度確定
					return false;
				case Lang::Swift:
					// Swift: << >> は乗除より上・& は乗除格・| ^ は加減格・比較は加減より下
					if(Operator == "<<" || Operator == ">>") Result = 18;
					else if(Operator == "&") Result = 13;
					else if(Operator == "|" || Operator == "^") Result = 12;
					else if(CmpOps.Contains(Operator)) Result = 7;
					else if(Operator == "??") Result = 8;
					// Swift の優先度確定
					return false;
				case Lang::Rust:
				case Lang::Python:
					// 比較より上のビット演算と，Rust の前置・乗除間の as，論理・代入間の範囲の補正
					if(Operator == "as" && Language == Lang::Rust) Result = 14;
					else if((Operator == ".." || Operator == "..=") && Language == Lang::Rust) Result = 3;
					else if(Operator == "&") Result = 10;
					else if(Operator == "^") Result = 9;
					else if(Operator == "|") Result = 8;
					else if(CmpOps.Contains(Operator) || Operator == "is" || Operator == "in" || Operator == "is not" || Operator == "not in") {
						// 比較演算子の優先度を採用
						Result = 7;
					}
					// Rust・Python の優先度確定
					return false;
				// PHP の演算子
				case Lang::PHP:
					// PHP の or < xor < and < 代入は数値空間に３段を収めれない為，優先度不明の０として内外の括弧を保護
					if(Operator == "and" || Operator == "or" || Operator == "xor") Result = 0;
					// PHP の優先度確定
					return false;
				case Lang::Ruby:
					// Ruby の低位演算子の再配置（`&` / `|` / `^` は同格とし，混在時の括弧を保護）
					if(Operator == "&" || Operator == "|" || Operator == "^") Result = 10;
					else if(Operator == "<" || Operator == "<=" || Operator == ">" || Operator == ">=") Result = 9;
					else if(CmpOps.Contains(Operator)) Result = 8;
					else if(Operator == "&&") Result = 7;
					else if(Operator == "||") Result = 6;
					else if(Operator == ".." || Operator == "...") Result = 5;
					else if(Operator == "?" || Operator == ":") Result = 4;
					else if(Operator == "and" || Operator == "or") Result = 1;
					// Ruby の優先度確定
					return false;
				// 共通規則だけを使う言語
				default:
					// 言語固有補正無で確定
					return false;
				}
			}
			// Table 未登録なら代入演算子か判定（Ruby は語演算子が代入より低い為 2，他言語は最低の 1）
			if(AssignOps.Contains(Operator)) {
				// Ruby と他言語の代入優先度
				Result = Language == Lang::Ruby ? 2 : 1;
				// 優先度確定の為，残り兄弟は走査不要の返戻
				return false;
			}
			// 演算子トークンでない為，次の兄弟へ走査継続の返戻
			return true;
		}
	);
	// 確定した演算子優先度の返戻
	return Result;
}

/**
 * 右端の子の取得関数
 * 閉じの字句で終わる節点（`a[b + c]` の添字は角括弧の中で閉じる）の最後の名前付の子は右端でない
 * @param Src ソースコード
 * @param Node 対象ノード
 * @return 節点の終わりで終わる最後の名前付の子（無ければ null）
 */
TSNode EditPass::RightEdgeChild(const TSSource &Src, const TSNode Node) {
	// 右端子の抽出
	const uint32_t Count = ts_node_named_child_count(Node);
	const TSNode Last = Count ? ts_node_named_child(Node, Count - 1) : TSNode{};
	// 親と終端を共有する右端子の返戻
	return !ts_node_is_null(Last) && Src.End(Last) == Src.End(Node) ? Last : TSNode{};
}

/**
 * 後置の連鎖の原子式の判定関数
 * @param Src ソースコード
 * @param Node 判定する式
 * @param Language 対象言語
 * @return 原子式なら true
 */
bool EditPass::IsPostfixAtom(const TSSource &Src, TSNode Node, const Lang Language) {
	// 後置原子まで左端を辿る反復
	while(true) {
		const std::string_view Type = ts_node_type(Node);
		if(
			NodeKind::PostfixReceiverLeaf.Contains(Type) ||
			Type == "new_expression" && Language.IsJsTs() && !ts_node_is_null(TSSource::FieldChild(Node, "arguments")) ||
			Type == "object_creation_expression" && (Language == Lang::Java || Language == Lang::CSharp) ||
			NodeKind::NumberLiteral.Contains(Type) && (Language == Lang::CSharp || Language == Lang::Kotlin || Language == Lang::Swift)
			// 終点に達した事の返戻
		) return true;
		// 現在節点の全子数
		const uint32_t Count = ts_node_child_count(Node);
		const std::string_view Last = Count ? std::string_view(ts_node_type(ts_node_child(Node, Count - 1))) : std::string_view();
		if(
			const bool IsChain = NodeKind::PostfixReceiverChain.Contains(Type) || Language == Lang::Rust && Type == "try_expression" ||
			Type == "postfix_expression" && (Last == "bang" || Last == "!!");
			!Count || !IsChain || Type == "field_expression" && Src.View(TSSource::FieldChild(Node, "operator")).ends_with('*') ||
			HasChildOf(
				Node,
				// 後置連鎖を阻む子の判定
				[Language](const TSNode Child) -> bool {
					// 後置連鎖の阻害要素の判定
					const std::string_view ChildType = ts_node_type(Child);
					const TSNode Lead = ts_node_child(Child, 0);
					// 後置連鎖を阻む任意連鎖又は安全航行接尾辞かの返戻
					return ChildType == "optional_chain" || ChildType == "?" && Language == Lang::Swift ||
					ChildType == "navigation_suffix" && !ts_node_is_null(Lead) && std::string_view(ts_node_type(Lead)) == "?.";
				}
			)
		) {
			// 後置の連鎖でない式・メンバへのポインタ (`.*` / `->*`)・任意の連鎖の返戻
			return false;
		}
		// 解析器が呼出と読んだ前置・中置式も，先頭を辿って除外
		Node = ts_node_child(Node, 0);
	}
}

/**
 * 後置の式の受け手を包む括弧の保持判定関数
 * 受け手（括弧層を透過した内側）が原子式 (IsPostfixAtom) なら，括弧を外しても後置の演算子の結合は変わらない（`(a.b).c` → `a.b.c`）
 * 括弧が呼出種別やマクロ展開の抑止を保つ場合は保持する
 * @param Src ソースコード
 * @param Paren 受け手を包む括弧
 * @param Host 後置の式
 * @param Language 対象言語
 * @return 括弧を保持する場合 true
 */
bool EditPass::KeepsPostfixReceiver(const TSSource &Src, const TSNode Paren, const TSNode Host, const Lang Language) {
	TSNode Inner = Paren;
	// 一重のグループ括弧を剥離
	while(NodeKind::GroupingParen.Contains(Inner) && ts_node_named_child_count(Inner) == 1) Inner = ts_node_named_child(Inner, 0);
	const std::string_view InnerType = ts_node_type(Inner);
	// Rust の前置単項と左結合 as の裸の型変換受け手扱い
	if(
		Language == Lang::Rust && std::string_view(ts_node_type(Host)) == "type_cast_expression" &&
		(NodeKind::UnaryPre.Contains(InnerType) || InnerType == "type_cast_expression")
		// 型の変換の受け手に置ける事の返戻
	) return false;
	// 被呼出側の名前・欄と，原子式でない受け手の保持の返戻
	return NodeKind::PostfixCallHost.Contains(Host) && (
		Language.IsCFamily() && IsIdentifierChar(Src[Src.End(Inner) - 1]) ||
		(Language == Lang::Rust || Language == Lang::Kotlin || Language == Lang::PHP) && NodeKind::FieldCallee.Contains(InnerType) ||
		Language == Lang::Kotlin && InnerType == "simple_identifier" || Language == Lang::PHP && InnerType == "name"
	) || !IsPostfixAtom(Src, Inner, Language);
}

/**
 * 宣言にも読める C++ の式先頭の判定関数
 * @param Src ソースコード
 * @param Node 判定する式
 * @param Cache 同じ構文木のノード毎の判定控え
 * @return 型名の関数形式を先頭に持ち得れば true
 */
bool EditPass::HasCppDeclarationHead(
	const TSSource &Src,
	TSNode Node,
	std::unordered_map<const void *, CppDeclarationHead> &Cache
) {
	std::vector<std::pair<const void *, bool>> Trail;
	CppDeclarationHead Head = CppDeclarationHead::Other;
	// 先頭式を内側へ辿る反復
	while(!ts_node_is_null(Node)) {
		if(
			const std::unordered_map<const void *, CppDeclarationHead>::const_iterator Found = Cache.find(Node.id);
			Found != Cache.end()
		) {
			// 既知の宣言先頭種別を採用
			Head = Found->second;
			// 既知結果に達して探索終了
			break;
		}
		const std::string_view Type = ts_node_type(Node);
		Trail.emplace_back(Node.id, Type == "call_expression");
		// 括弧式の内側へ移動
		if(Type == "parenthesized_expression") Node = ts_node_named_child(Node, 0);
		// 呼出式の関数部へ移動
		else if(Type == "call_expression") Node = TSSource::FieldChild(Node, "function");
		// 添字式の対象部へ移動
		else if(Type == "subscript_expression") Node = TSSource::FieldChild(Node, "argument");
		else if(
			Type == "comma_expression" || Type == "assignment_expression" && Src.View(TSSource::FieldChild(Node, "operator")) == "="
		) Node = TSSource::FieldChild(Node, "left");
		else {
			// 型と関数の名前解決を仮定せず，宣言に使える名前・型の構文で判定
			if(NodeKind::CppDeclarationName.Contains(Type)) Head = CppDeclarationHead::Name;
			// 先頭分類を確定して探索終了
			break;
		}
	}
	// 左辺と括弧の交互ネストでの同一先頭再走査の防止
	for(size_t Idx = Trail.size(); Idx;) {
		const std::pair<const void *, bool> &Entry = Trail[--Idx];
		// 宣言名の呼出を呼出先頭へ昇格
		if(Entry.second && Head == CppDeclarationHead::Name) Head = CppDeclarationHead::Call;
		// 現在節点の判定結果をキャッシュ
		Cache.emplace(Entry.first, Head);
	}
	// 呼出形式の宣言先頭かを返戻
	return Head == CppDeclarationHead::Call;
}

/**
 * JS/TS の括弧を外すと内側が文頭の塊・宣言・指示として読まれるかの判定関数
 * 位置に依り塊又は宣言へ変わる式の先頭を判定する
 * 式文と for-in の左辺の先頭の `let [` は字句の宣言と読み，for-of の左辺の先頭の `let` と左辺其の物の `async` は構文誤りに為る
 * 文の後置の増減は前置へ移す為（`(let)[k]++;` は `++(let)[k];`），其の括弧は文頭に来ない
 * @param Src ソースコード
 * @param Paren 判定する括弧の式
 * @param Parent 括弧の親
 * @param Inner 括弧の内側の式
 * @return 括弧を外すと読みが変わる場合 true
 */
bool EditPass::ReadsAsJsStatementHead(const TSSource &Src, const TSNode Paren, const TSNode Parent, const TSNode Inner) {
	// 文字列式文の指示文字列化を保護
	if(std::string_view(ts_node_type(Inner)) == "string" && std::string_view(ts_node_type(Parent)) == "expression_statement") {
		// 括弧保持の返戻
		return true;
	}
	// 読みが変わる先頭字句の抽出
	const std::string_view Source = Src;
	const size_t Lead = Source.find_first_not_of("( \t\n", Src.Start(Inner));
	if(
		const std::string_view Rest = Lead == std::string_view::npos ? std::string_view() : Source.substr(Lead);
		!Rest.starts_with('{') && !Rest.starts_with("function") && !Rest.starts_with("class") && !Rest.starts_with("async") &&
		!Rest.starts_with("let")
		// 読みの変わる字句で始まらない括弧の返戻
	) return false;
	TSNode Leftmost = Inner;
	// 最左の葉まで降下
	while(!ts_node_is_null(Leftmost) && ts_node_child_count(Leftmost)) {
		// 括弧を透過して最初の子へ移動
		Leftmost = NodeKind::GroupingParen.Contains(Leftmost) ? ts_node_named_child(Leftmost, 0) : ts_node_child(Leftmost, 0);
	}
	// 空の括弧は読みの変わる字句を持たない事の返戻
	if(ts_node_is_null(Leftmost)) return false;
	// 先頭字句の種類判定
	const std::string_view Head = Src.View(Leftmost);
	const bool IsBrace = Head == "{";
	bool IsDeclaration = Head == "function" || Head == "class";
	// `async function` の宣言扱い
	if(Head == "async") {
		const uint32_t Next = TextEdit::SkipSpRight(Src, Src.End(Leftmost));
		// async function 宣言かを確定
		IsDeclaration = std::string_view(Src).substr(Next).starts_with("function");
	}
	const size_t After = Source.find_first_not_of(") \t\n", Src.End(Leftmost));
	const std::string_view Following = After == std::string_view::npos ? std::string_view() : Source.substr(After);
	const bool IsLetBracket = Head == "let" && Following.starts_with('[');
	const bool IsForOfHead =
	Head == "let" || Head == "async" && Following.starts_with("of") && (Following.size() == 2 || !IsIdentifierChar(Following[2]));
	// 位置に依らず読みの変わらない字句の返戻
	if(!IsBrace && !IsDeclaration && !IsLetBracket && !IsForOfHead) return false;
	// 開始位置を共有する祖先からの最左字句位置の取得
	const uint32_t Start = Src.Start(Paren);
	// 括弧を包む祖先の走査
	for(TSNode Ancestor = Paren, Up = Parent; !ts_node_is_null(Up); Ancestor = Up, Up = ts_node_parent(Up)) {
		const std::string_view Type(ts_node_type(Up));
		// 矢印関数本体の物体式かを返戻
		if(Type == "arrow_function") return IsBrace && ts_node_eq(TSSource::FieldChild(Up, "body"), Ancestor);
		// export 配下の宣言かを返戻
		if(Type == "export_statement") return IsDeclaration;
		// for-in・for-of 見出の場合
		if(Type == "for_in_statement") {
			// for-in の左辺の先頭の `let [`，for-of の左辺の先頭の `let` と左辺其の物の `async` の返戻
			return ts_node_eq(TSSource::FieldChild(Up, "left"), Ancestor) &&
			(HasUnnamedTokenChild(Src, Up, "of") ? IsForOfHead : IsLetBracket);
		}
		// 文の後置の増減又は文の途中に在る事の返戻
		if(
			Type == "update_expression" && ts_node_is_named(ts_node_child(Up, 0)) &&
			std::string_view(ts_node_type(ts_node_parent(Up))) == "expression_statement" || Src.Start(Up) != Start
			// 文頭でない事の返戻
		) return false;
		// 式文先頭で構文が変わるかを返戻
		if(Type == "expression_statement") return IsBrace || IsDeclaration || IsLetBracket;
	}
	// 文頭構文を変えない事の返戻
	return false;
}

/**
 * JS/TS の for 文の初期化子の `in` を守る括弧かの判定関数
 * 初期化子内の二項 `in` を for-in の区切りへ変えない
 * ネストの括弧は同じ走査で外れ得る為，内側の探索では透過し，外側の括弧が守る
 * @param Src ソースコード
 * @param Inner 括弧の内側の式
 * @return 括弧を外すと初期化子の `in` が for-in の区切りに為る場合 true
 */
bool EditPass::ShieldsJsForIn(const TSSource &Src, const TSNode Inner) {
	// 二項 `in` の子孫探索
	bool HasIn = false;
	const auto Visit = [&Src, &HasIn](const TSNode Node) -> bool {
		const std::string_view Type = ts_node_type(Node);
		// 二項 `in` の発見を記録
		if(Type == "binary_expression" && Src.View(TSSource::FieldChild(Node, "operator")) == "in") HasIn = true;
		// 見付けた後と，`in` を二項演算子に戻す式（括弧を除く）の中へは降りない事の返戻
		return !HasIn && (Type == "parenthesized_expression" || !NodeKind::JsInRestoring.Contains(Type));
	};
	// 根自身の確認後に必要な子孫への探索拡張
	if(Visit(Inner)) WalkChildrenCursor(Inner, Visit);
	// `in` を守る括弧かを返戻
	return HasIn;
}

/**
 * 前置の演算子と被演算子の先頭の記号の融合判定関数
 * 密着で別の前置演算子へ融合する記号を判定する
 * @param Language 対象言語
 * @param Operator 前置の演算子の末尾の文字
 * @param Operand 被演算子の先頭の文字
 * @return 密着させると１字句へ融合するなら true
 */
bool EditPass::FusesPrefixOperator(const Lang Language, const char Operator, const char Operand) {
	// 融合するかの返戻（Python・Ruby・Rust は `--` / `++` の字句を持たず，Rust は `&&` を参照の重なりへ分けて読む）
	return Operator == Operand && (Operator == '-' || Operator == '+' || Operator == '&') && Language != Lang::Python &&
	Language != Lang::Ruby && Language != Lang::Rust ||
	Operator == '!' && (Language == Lang::Ruby && Operand == '~' || Language == Lang::Kotlin && Operand == '!');
}

/**
 * TS の型引数の閉じ `>` の直後に来る被演算子かの判定関数
 * TS の比較式を型引数付の式へ変える先頭字を判定する
 * 外れる括弧（内側が比較とシフトより強く結合し，括弧必須の式でもない）は透過して内側の先頭を見る
 * @param Src ソースコード
 * @param Operand 比較の右の被演算子
 * @param Language 対象言語
 * @return 整形後に `(` `\`` `+` `-` `[` で始まるなら true
 */
bool EditPass::OpensTsTypeArguments(const TSSource &Src, TSNode Operand, const Lang Language) {
	// 被演算子の外側括弧を走査
	while(!ts_node_is_null(Operand) && std::string_view(ts_node_type(Operand)) == "parenthesized_expression") {
		const TSNode Inner = ts_node_named_child(Operand, 0);
		if(
			const int Prec = ts_node_is_null(Inner) ? 0 : OpPrecedence(Src, Inner, Language);
			ts_node_named_child_count(Operand) != 1 || Prec && Prec < 12 || NodeKind::ParenInnerNoUnwrap.Contains(Inner)
			// 残る括弧で始まる事の返戻
		) return true;
		// 次の内側式へ移動
		Operand = Inner;
	}
	// TS 型引数として開き得る先頭字句かを返戻
	return !ts_node_is_null(Operand) && std::string_view("(`+-[").find(Src[Src.Start(Operand)]) != std::string_view::npos;
}

/**
 * 括弧の直下の改行の畳込関数
 * Kotlin は改行で文を終える為，括弧の中で次の行の二項の演算子へ続く式（`(a` の次の行に `+ b)`）は，括弧を外すと `+ b` が別の文に為る
 * 括弧直下だけを畳み，内側の独立した区切りを保つ
 * @param Src ソースコード
 * @param Inner 外す括弧の内側の式
 * @param Edits 収集先のエディット一覧
 */
void EditPass::CollapseParenLines(const TSSource &Src, const TSNode Inner, std::vector<TextEdit> &Edits) {
	const auto Visit = [&Src, &Edits](const TSNode Node, const auto &Self) -> void {
		const std::string_view Type = ts_node_type(Node);
		// 終端節点の除外
		// 字句・文字列・括弧・塊の内側へ降りない事の終了
		if(
			!ts_node_child_count(Node) || NodeKind::Leaf.Contains(Type) || NodeKind::StringLikeInnerPreserve.Contains(Type) || HasChildOf(
				Node,
				// 内側括弧の有無判定関数
				[&Src](const TSNode Child) -> bool {
					// 開き括弧の字句かの返戻
					return !ts_node_is_named(Child) && Src.Len(Child) == 1 &&
					std::string_view("([{").find(Src[Src.Start(Child)]) != std::string_view::npos;
				}
			)
			// 条件成立時の返戻
		) return;
		TSNode Prev {};
		// 子節点の順次走査
		ForEachChild(
			Node,
			// 現在の子節点の処理
			[&](const TSNode Child) -> void {
				if(!ts_node_is_null(Prev)) {
					if(
						const uint32_t GapStart = Src.End(Prev), GapEnd = Src.Start(Child);
						GapStart < GapEnd && std::memchr(Src.data() + GapStart, '\n', GapEnd - GapStart)
						// 改行を単一空白へ置換
					) TextEdit::Push(GapStart, GapEnd, " ", Edits);
				}
				// 子節点の再帰走査と記録
				Self(Child, Self);
				Prev = Child;
			}
		);
	};
	// 括弧内の再帰走査を開始
	Visit(Inner, Visit);
	// 終了
	return;
}

/**
 * 文文脈の後置増減 (`x++`/`x--`) を前置 (`++x`/`--x`) へ正規化する編集の収集関数
 * @param Src ソースコード
 * @param StmtChild 文の直下に有る式ノード
 * @param Language 対象言語
 * @param Edits 収集先のエディット一覧
 */
void EditPass::MaybeCollectIncrementEdit(
	const TSSource &Src,
	const TSNode StmtChild,
	const Lang Language,
	std::vector<TextEdit> &Edits
) {
	// 多重定義の C++ と前置構文無の Go を除外（複合代入は型不明の為に警告のみ）
	if(
		ts_node_is_null(StmtChild) || Language == Lang::Go || Language == Lang::Cpp ||
		!NodeKind::PostfixOrUpdateExpression.Contains(StmtChild)
		// 条件成立時の返戻
	) return;
	const uint32_t ChildCount = ts_node_child_count(StmtChild);
	// 条件成立時の返戻
	if(ChildCount < 2) return;
	const TSNode Last = ts_node_child(StmtChild, ChildCount - 1);
	// 条件成立時の返戻
	if(ts_node_is_named(Last)) return;
	const std::string_view OpToken = Src.View(Last);
	// 条件成立時の返戻
	if(OpToken != "++" && OpToken != "--") return;
	// 前置増減への置換編集
	const uint32_t LhsStart = Src.Start(StmtChild);
	TextEdit::Push(LhsStart, LhsStart, std::string(OpToken), Edits);
	TextEdit::Push(Src.Start(Last), Src.End(Last), "", Edits);
	// 前置化で冗長に為る括弧の除去（マクロの展開後の結合を変える物は保持）
	if(
		// 増減演算子の被演算子
		const TSNode Operand = ts_node_named_child(StmtChild, 0);
		NodeKind::GroupingParen.Contains(Operand) && ts_node_named_child_count(Operand) == 1 && Src[LhsStart] == '(' &&
		!Src.MentionsExpressionMacro(Operand)
	) {
		// 被演算子内側の結合判定
		const TSNode Inner = ts_node_named_child(Operand, 0);
		const std::string_view InnerType = ts_node_type(Inner);
		if(
			const int InnerPrec = OpPrecedence(Src, Inner, Language);
			!NodeKind::GroupingParen.Contains(InnerType) && (InnerPrec > 14 || !InnerPrec && !NodeKind::InfixOp.Contains(InnerType))
		) {
			// 冗長な括弧対の除去
			TextEdit::Push(LhsStart, LhsStart + 1, "", Edits);
			TextEdit::Push(Src.End(Operand) - 1, Src.End(Operand), "", Edits);
		}
	}
	// 終了
	return;
}

/**
 * 無限 for ループ (`for(;;)`) を `while(true)`/`while(1)` へ正規化する編集の収集関数
 * @param Src ソースコード
 * @param Node for_statement ノード
 * @param Language 対象言語
 * @param Edits 収集先のエディット一覧
 */
void EditPass::MaybeCollectInfiniteForEdit(
	const TSSource &Src,
	const TSNode Node,
	const Lang Language,
	std::vector<TextEdit> &Edits
) {
	// 両構文を持つ言語の無限 for の while 化（C だけは stdbool.h に依存しない while(1)）
	if(!Language.IsBraceLang() || ts_node_is_null(Node)) return;
	const TSNode Body = TSSource::FieldChild(Node, "body");
	// 条件成立時の返戻
	if(ts_node_is_null(Body)) return;
	bool HasHeaderClause = false;
	// for 見出の名前付子を走査
	ForEachNamedChild(
		Node,
		// 現在の見出子の処理
		[&](const TSNode Child) -> bool {
			// 本体と空文を読み飛ばす事の返戻
			if(ts_node_eq(Child, Body) || std::string_view(ts_node_type(Child)) == "empty_statement") return true;
			// 見出節の存在を記録
			HasHeaderClause = true;
			// 何れかの節を発見した為，走査打切の返戻
			return false;
		}
	);
	// 条件成立時の返戻
	if(HasHeaderClause) return;
	// 無限 while 見出への置換
	const uint32_t Start = Src.Start(Node), BodyStart = Src.Start(Body);
	TextEdit::Push(
		Start,
		TextEdit::SkipCharsLeftBounded(Src, BodyStart, Start, " \t\n"),
		Language == Lang::C || Language.IsCShared ? "while(1)" : "while(true)",
		Edits
	);
	// 終了
	return;
}

/**
 * 真偽値リテラル編集の収集関数（真偽値型の初期値 `0` / `1` を `false` / `true` へ置き換える）
 * @param Src ソースコード
 * @param Node 宣言又は代入のノード
 * @param Edits 収集先のエディット一覧
 */
void EditPass::MaybeCollectBoolLiteralEdit(const TSSource &Src, const TSNode Node, std::vector<TextEdit> &Edits) {
	// C++ の bool 初期化での値型を保つ真偽値表記の統一
	if(ts_node_is_null(Node)) return;
	if(
		// 宣言型が bool かを調査
		const bool IsBoolType = HasChildOf(
			Node,
			// 型子節点の判定関数
			[&Src](const TSNode Child) -> bool {
				// bool 基本型かを返戻
				return std::string_view(ts_node_type(Child)) == "primitive_type" && Src.View(Child) == "bool";
			}
		); !IsBoolType // 条件成立時の返戻
	) return;
	const auto PushIfZeroOne = [&Src, &Edits](const TSNode Value) -> void {
		// 条件成立時の返戻
		if(ts_node_is_null(Value) || !NodeKind::NumberLiteral.Contains(Value)) return;
		// 数値リテラルの抽出
		const std::string_view Text = Src.View(Value);
		// 条件成立時の返戻
		if(Text != "0" && Text != "1") return;
		// 真偽値リテラルへの置換
		TextEdit::Push(Src.Start(Value), Src.End(Value), Text == "1" ? "true" : "false", Edits);
	};
	// クラスメンバ宣言は宣言子で包まず初期値を直接の子に持つ（`bool IsFlag = 1;` 形）
	if(std::string_view(ts_node_type(Node)) == "field_declaration") {
		bool IsPlainDeclarator = false;
		ForEachNamedChild(
			Node,
			// フィールド宣言の子を走査
			[&](const TSNode Child) -> void {
				// フィールド名を発見した場合
				if(std::string_view(ts_node_type(Child)) == "field_identifier") {
					IsPlainDeclarator = true;
					// フィールド識別子を発見した場合の返戻
					return;
				}
				// 初期値の置換と探索状態の復元
				if(IsPlainDeclarator) PushIfZeroOne(Child);
				IsPlainDeclarator = false;
			}
		);
		// クラスメンバ経路の処理完了
		return;
	}
	// C++ は宣言子毎に初期値を持ち得る（`bool IsA = 1, B = 0;` 形）
	ForEachNamedChild(
		Node,
		// 各宣言子の真偽値候補を処理
		[&](const TSNode Child) -> void {
			// 条件成立時の返戻
			if(std::string_view(ts_node_type(Child)) != "init_declarator") return;
			const uint32_t NamedCount = ts_node_named_child_count(Child);
			// 素の bool 識別子だけの置換（間接型の０を真偽値へ変えない）
			if(NamedCount < 2 || std::string_view(ts_node_type(ts_node_named_child(Child, 0))) != "identifier") return;
			// 宣言子末尾の初期値を置換
			PushIfZeroOne(ts_node_named_child(Child, NamedCount - 1));
		}
	);
	// 終了
	return;
}

/**
 * Rust 数値リテラルの型接尾辞（u8/i32/f64 等）開始位置の取得関数
 * @param Text 数値リテラルの文字列
 * @return 型接尾辞の開始添字（接尾辞が無ければ Text.size()）
 */
size_t EditPass::RustNumericSuffixStart(const std::string_view Text) {
	const bool IsBasePrefix = Text.size() > 1 && Text[0] == '0' && std::strchr("xbo", Text[1]) != nullptr;
	static constexpr std::string_view Suffixes[] =
	{ "usize", "isize", "u128", "i128", "f32", "f64", "u64", "i64", "u32", "i32", "u16", "i16", "u8", "i8" };
	// 既知の Rust 接尾辞を走査
	for(const std::string_view Sfx : Suffixes) {
		// 接頭辞付整数からの浮動小数接尾辞の除外
		if(IsBasePrefix && Sfx[0] == 'f') continue;
		// 一致した接尾辞の開始位置を返戻
		if(Text.size() > Sfx.size() && Text.substr(Text.size() - Sfx.size()) == Sfx) return Text.size() - Sfx.size();
	}
	// 接尾辞無の終端位置を返戻
	return Text.size();
}

/**
 * 数値リテラルの正規化関数
 * @param Text 数値リテラルの文字列
 * @param Language 対象言語
 * @param ShouldPad 真で小数点前後の 0 を補完する
 * @return 正規化後の文字列
 */
std::string EditPass::NormalizeNumericLiteral(const std::string_view Text, const Lang Language, const bool ShouldPad) {
	// 数値先頭の符号長
	const size_t Sign = !Text.empty() && (Text[0] == '+' || Text[0] == '-') ? 1 : 0;
	const bool IsZeroLed = Text.size() > Sign + 1 && Text[Sign] == '0';
	const bool IsSwiftBasePrefix =
	Language == Lang::Swift && IsZeroLed && (Text[Sign + 1] == 'x' || Text[Sign + 1] == 'b' || Text[Sign + 1] == 'o');
	const bool IsHexBin =
	ShouldPad && IsZeroLed && (Text[Sign + 1] == 'x' || Text[Sign + 1] == 'X' || Text[Sign + 1] == 'b' || Text[Sign + 1] == 'B');
	const bool IsRustBasePrefix =
	Language == Lang::Rust && IsZeroLed && (Text[Sign + 1] == 'x' || Text[Sign + 1] == 'b' || Text[Sign + 1] == 'o');
	const bool KeepsBasePrefix = IsRustBasePrefix || IsSwiftBasePrefix;
	const size_t SuffixStart = Language == Lang::Rust ? RustNumericSuffixStart(Text) : Text.size();
	std::string Fixed;
	// 正規化後の最大長を予約
	Fixed.reserve(Text.size() + 1);
	// 数値字面の各文字を走査
	for(size_t Idx = 0; Idx < Text.size(); ++Idx) {
		const char Char = Text[Idx];
		// 小数点前の 0 は符号の後に補完
		if(ShouldPad && !IsHexBin && Idx == Sign && Char == '.') Fixed.push_back('0');
		// Rust 接頭辞・型接尾辞と BigInt の `n` の大文字化除外
		Fixed.push_back(
			Idx >= SuffixStart || Char == 'n' || KeepsBasePrefix && Idx == Sign + 1 ?
			Char :
			static_cast<char>(std::toupper(static_cast<unsigned char>(Char)))
		);
		// 小数点後の 0 補完 (C/C++/C#/Java)
		if(
			ShouldPad && !IsHexBin && Char == '.' && (Idx + 1 == Text.size() || !std::isdigit(static_cast<unsigned char>(Text[Idx + 1])))
		) Fixed.push_back('0');
	}
	// 正規化した数値リテラルの返戻
	return Fixed;
}

/**
 * 括弧の対の除去編集の収集関数
 * @param Src ソースコード
 * @param Start 開き括弧の位置
 * @param End 閉じ括弧の直後の位置
 * @param Depth 剥がす括弧の層の数（ネストの括弧 `((x))` は層毎に剥がす）
 * @param UnaryOp 括弧を被演算子に取る前置の単項の演算子（無ければ null）
 * @param IsSymbolGap 内側の端と外の字句が共に記号の時に空白を残すか
 * @param Edits 収集先のエディット一覧
 */
void EditPass::CollectParenPairEdit(
	const TSSource &Src,
	const uint32_t Start,
	const uint32_t End,
	const uint32_t Depth,
	const TSNode UnaryOp,
	const bool IsSymbolGap,
	std::vector<TextEdit> &Edits
) {
	// 内側の実内容範囲の算出
	const auto IsBlank = [](const char Char) -> bool {
		// 空白・改行かの返戻
		return Char == ' ' || Char == '\t' || Char == '\n' || Char == '\r';
	};
	uint32_t InnerStart = Start, InnerEnd = End;
	for(uint32_t Layer = 0; Layer < Depth; ++Layer) {
		++InnerStart;
		--InnerEnd;
		while(InnerStart < InnerEnd && IsBlank(Src[InnerStart])) ++InnerStart;
		while(InnerEnd > InnerStart && IsBlank(Src[InnerEnd - 1])) --InnerEnd;
	}
	// 左側の区切要否の判定
	const bool IsSymbolPrefix = !ts_node_is_null(UnaryOp) && !ts_node_is_named(UnaryOp);
	const bool IsWordOperator = IsSymbolPrefix && std::isalpha(static_cast<unsigned char>(Src[Src.Start(UnaryOp)]));
	const bool IsSpaceLeft = Start && (
		std::isalpha(static_cast<unsigned char>(Src[Start - 1])) || IsWordOperator ||
		IsWordChar(Src[Start - 1]) && IsWordChar(Src[InnerStart]) ||
		IsSymbolGap && IsOperatorChar(Src[Start - 1]) && IsOperatorChar(Src[InnerStart]) && !IsSymbolPrefix
	);
	// 左右字句の融合防止用区切を保つ括弧除去
	TextEdit::Push(Start, InnerStart, IsSpaceLeft ? " " : "", Edits);
	// 右側の区切を含む閉じ括弧の除去
	TextEdit::Push(
		InnerEnd,
		End,
		End < Src.size() && (
			IsSymbolGap && IsOperatorChar(Src[End]) && IsOperatorChar(Src[InnerEnd - 1]) ||
			IsWordChar(Src[End]) && IsWordChar(Src[InnerEnd - 1])
		) ? " " : "",
		Edits
	);
	// 終了
	return;
}

/**
 * ネストの括弧の内側の層の除去編集の収集関数
 * 保つ最も外の層の開閉の括弧は残し，其の内側の層の括弧と空白を中身の端迄除く（`((x))` → `(x)`）
 * @param Src ソースコード
 * @param Outer 保つ最も外の層
 * @param Content 最も内側の層の中身
 * @param Edits 収集先のエディット一覧
 */
void EditPass::CollectNestedLayerEdit(
	const TSSource &Src,
	const TSNode Outer,
	const TSNode Content,
	std::vector<TextEdit> &Edits
) {
	// 内側括弧対の除去
	TextEdit::Push(Src.Start(Outer) + 1, Src.Start(Content), "", Edits);
	TextEdit::Push(Src.End(Content), Src.End(Outer) - 1, "", Edits);
	// 終了
	return;
}

/**
 * 省ける括弧の除去編集の収集関数
 * 名前だけのラムダの仮引数（`(x) => x` → `x => x`）と，言語が省く事を許す空の仮引数・実引数
 * 引数無の呼出で省略可能な括弧を外す
 * @param Src ソースコード
 * @param Node 括弧の並びを子に持つノード
 * @param OpAncestor 括弧層を透過した最初の非括弧の祖先
 * @param OperandTop OpAncestor の直下に位置する被演算子（括弧層の最外）
 * @param Language 対象言語
 * @param Edits 収集先のエディット一覧
 */
void EditPass::MaybeCollectOptionalParenEdit(
	const TSSource &Src,
	const TSNode Node,
	const TSNode OpAncestor,
	const TSNode OperandTop,
	const Lang Language,
	std::vector<TextEdit> &Edits
) {
	// 節点種別の取得
	const std::string_view Type(ts_node_type(Node));
	// 括弧範囲の除去関数
	const auto Erase = [&Src, &Edits](const uint32_t Start, const uint32_t End) -> void {
		TextEdit::Push(Start, End, Start && End < Src.size() && IsWordChar(Src[Start - 1]) && IsWordChar(Src[End]) ? " " : "", Edits);
	};
	// 空の括弧列の判定関数
	const auto IsEmpty = [&Src](const TSNode List) -> bool {
		// 子が開閉の括弧だけかの返戻
		return !ts_node_is_null(List) && ts_node_child_count(List) == 2 && Src[Src.Start(List)] == '(';
	};
	// 空の括弧列の除去関数
	const auto Drop = [&Src, &Erase, &IsEmpty](const TSNode List) -> void {
		if(IsEmpty(List)) Erase(Src.Start(List), Src.End(List));
	};
	// 名前だけの仮引数列の括弧除去関数
	const auto Unwrap = [&Src, &Erase](const TSNode List, const auto &IsName) -> void {
		// 括弧付並びの構造検査
		const uint32_t Count = ts_node_is_null(List) ? 0 : ts_node_child_count(List);
		// 条件成立時の返戻
		if(Count < 3 || Src[Src.Start(List)] != '(' || Src.View(ts_node_child(List, Count - 1)) != ")") return;
		// 仮引数名とコンマが交互に並ぶ事を確認
		for(uint32_t Idx = 1; Idx + 1 < Count; ++Idx) {
			// 条件成立時の返戻
			if(Idx & 1 ? !IsName(ts_node_child(List, Idx)) : Src.View(ts_node_child(List, Idx)) != ",") return;
		}
		// 開閉括弧の除去範囲の算出
		const uint32_t Start = Src.Start(List), End = Src.End(List);
		Erase(Start, Start + 1);
		Erase(Count & 1 ? End - 1 : Src.Start(ts_node_child(List, Count - 2)), End);
	};
	// 包装を許す単一名の判定関数
	const auto IsBare = [](const TSNode Param, const std::string_view Wrapper, const std::string_view Name) -> bool {
		const TSNode Inner =
		std::string_view(ts_node_type(Param)) == Wrapper && ts_node_child_count(Param) == 1 ? ts_node_child(Param, 0) : Param;
		// 名前の節点かの返戻
		return std::string_view(ts_node_type(Inner)) == Name;
	};
	// JavaScript と TypeScript の括弧省略
	if(Language.IsJsTs()) {
		// 仮引数が２つ以上の矢印関数と，型の仮引数・戻値の型を持つ矢印関数 (`<T>(x) => x` / `(x): T => x`) は括弧の必要
		const TSNode Params = Type == "arrow_function" ? TSSource::FieldChild(Node, "parameters") : TSNode{};
		if(
			!ts_node_is_null(Params) && ts_node_named_child_count(Params) == 1 &&
			ts_node_is_null(TSSource::FieldChild(Node, "type_parameters")) && ts_node_is_null(TSSource::FieldChild(Node, "return_type"))
		) {
			Unwrap(
				Params,
				[&IsBare](const TSNode Param) -> bool {
					// TS の無修飾仮引数の `required_parameter` による包装
					return IsBare(Param, "required_parameter", "identifier");
				}
			);
		}
		// 生成が受け手の空実引数の保持（TS の比較左辺も型引数と紛れる為に保持）
		if(
			Type == "new_expression" && ts_node_is_null(TSSource::FieldChild(Node, "type_arguments")) && !(
				!ts_node_is_null(OpAncestor) && ts_node_eq(ts_node_named_child(OpAncestor, 0), OperandTop) && (
					NodeKind::ConstructionReceiverHost.Contains(OpAncestor) ||
					Language == Lang::TypeScript && Src.View(TSSource::FieldChild(OpAncestor, "operator")) == "<"
				)
			)
		) Drop(TSSource::FieldChild(Node, "arguments"));
		// 終了
		return;
	}
	// 言語毎に省略可能なラムダの仮引数形式の選択
	if(Type == "lambda_expression") {
		// ラムダの言語別処理
		switch(Language.Id) {
		case Lang::Java:
			{
				// Java の単一仮引数の処理
				if(
					const TSNode Params = TSSource::FieldChild(Node, "parameters");
					!ts_node_is_null(Params) && std::string_view(ts_node_type(Params)) == "inferred_parameters" &&
					ts_node_named_child_count(Params) == 1
				) {
					Unwrap(
						Params,
						[](const TSNode Param) -> bool {
							// 名前の仮引数かの返戻
							return std::string_view(ts_node_type(Param)) == "identifier";
						}
					);
				}
				break;
			}
		case Lang::CSharp:
			{
				// C# の仮引数修飾の検査
				const TSNode Params = TSSource::FieldChild(Node, "parameters");
				const bool IsDecorated = ts_node_is_null(Params) || HasChildOf(
					Node,
					[&Src, &Params](const TSNode Child) -> bool {
						// 仮引数より前に修飾以外の名前付の子が有るかの返戻
						return ts_node_is_named(Child) && Src.Start(Child) < Src.Start(Params) && std::string_view(ts_node_type(Child)) != "modifier";
					}
				);
				if(!IsDecorated && ts_node_named_child_count(Params) == 1) {
					Unwrap(
						Params,
						[&IsBare](const TSNode Param) -> bool {
							// 型・修飾の無い仮引数かの返戻
							return IsBare(Param, "parameter", "identifier");
						}
					);
				}
				break;
			}
		case Lang::Cpp:
			// 修飾・戻値型の無い空仮引数の除去（付随時の省略は C++23 より前では不可）
			if(ts_node_is_null(TSSource::FieldChild(Node, "constraint"))) {
				if(
					const TSNode Declarator = TSSource::FieldChild(Node, "declarator");
					!ts_node_is_null(Declarator) && ts_node_child_count(Declarator) == 1
				) Drop(ts_node_child(Declarator, 0));
			}
			break;
		default:
			break;
		}
		// 終了
		return;
	}
	// ラムダ以外の言語別処理
	switch(Language.Id) {
	case Lang::Swift:
		{
			// 型・外部名の無いクロージャ仮引数の裸形式への統一
			if(Type == "lambda_function_type" && ts_node_child_count(Node) > 2) {
				if(
					const TSNode Params = ts_node_child(Node, 1);
					Src[Src.Start(Node)] == '(' && std::string_view(ts_node_type(Params)) == "lambda_function_type_parameters" &&
					Src.View(ts_node_child(Node, 2)) == ")" && !HasChildOf(
						Params,
						[](const TSNode Param) -> bool {
							// 名前だけでない仮引数かの返戻
							return ts_node_is_named(Param) &&
							(std::string_view(ts_node_type(Param)) != "lambda_parameter" || ts_node_child_count(Param) != 1);
						}
					)
				) {
					Erase(Src.Start(Node), Src.Start(Node) + 1);
					Erase(Src.Start(ts_node_child(Node, 2)), Src.End(ts_node_child(Node, 2)));
				}
			}
			// 後置クロージャだけの空実引数の除去（被呼出側が呼出なら結付を保護）
			if(Type == "call_expression" && ts_node_child_count(Node) == 2) {
				const TSNode Callee = ts_node_child(Node, 0), Suffix = ts_node_child(Node, 1);
				if(
					const TSNode Closure = ts_node_named_child(Suffix, 0);
					std::string_view(ts_node_type(Callee)) != "call_expression" || ts_node_child_count(Callee) != 2 ||
					std::string_view(ts_node_type(Suffix)) != "call_suffix" || ts_node_is_null(Closure) ||
					std::string_view(ts_node_type(Closure)) != "lambda_literal"
					// 条件成立時の返戻
				) return;
				// 内側呼出の空実引数の除去
				if(
					const TSNode InnerSuffix = ts_node_child(Callee, 1);
					std::string_view(ts_node_type(ts_node_child(Callee, 0))) != "call_expression" && ts_node_child_count(InnerSuffix) == 1
				) {
					if(const TSNode Arguments = ts_node_child(InnerSuffix, 0); std::string_view(ts_node_type(Arguments)) == "value_arguments") {
						Drop(Arguments);
					}
				}
			}
			// 連鎖内呼出の実引数列と後置クロージャの単一後置化
			if(
				Type == "call_suffix" && ts_node_named_child_count(Node) > 1 && !ts_node_is_null(OpAncestor) &&
				std::string_view(ts_node_type(ts_node_named_child(OpAncestor, 0))) != "call_expression" &&
				std::string_view(ts_node_type(ts_node_named_child(Node, 1))) == "lambda_literal"
			) {
				if(const TSNode Arguments = ts_node_named_child(Node, 0); std::string_view(ts_node_type(Arguments)) == "value_arguments") {
					Drop(Arguments);
				}
			}
			// 終了
			return;
		}
	case Lang::Kotlin:
		{
			// 後置ラムダだけの空実引数の除去（被呼出側が呼出なら結付を保護）
			if(
				Type == "call_suffix" && !ts_node_is_null(FirstNamedChildOfType(Node, "annotated_lambda")) && !ts_node_is_null(OpAncestor) &&
				std::string_view(ts_node_type(ts_node_named_child(OpAncestor, 0))) != "call_expression"
			) Drop(FirstNamedChildOfType(Node, "value_arguments"));
			// 型の位置の注釈は，後続の関数型の仮引数列との誤読の防止
			else if(
				const TSNode Invocation = FirstNamedChildOfType(Node, "constructor_invocation");
				Type == "annotation" && !ts_node_is_null(Invocation) && !ts_node_is_null(OpAncestor) &&
				std::string_view(ts_node_type(OpAncestor)) != "type_modifiers"
			) Drop(FirstNamedChildOfType(Invocation, "value_arguments"));
			else if(Type == "enum_entry") Drop(FirstNamedChildOfType(Node, "value_arguments"));
			// 主構築子の省略可否の判定
			else if(Type == "class_declaration") {
				TSNode Body = FirstNamedChildOfType(Node, "class_body");
				if(ts_node_is_null(Body)) Body = FirstNamedChildOfType(Node, "enum_class_body");
				if(ts_node_is_null(Body) || ts_node_is_null(FirstNamedChildOfType(Body, "secondary_constructor"))) {
					Drop(FirstNamedChildOfType(Node, "primary_constructor"));
				}
			}
			// 終了
			return;
		}
	case Lang::Ruby:
		{
			// Ruby の名前だけの仮引数の裸の並びへの統一
			if(Type == "lambda") {
				const TSNode Params = TSSource::FieldChild(Node, "parameters");
				Drop(Params);
				Unwrap(
					Params,
					[](const TSNode Param) -> bool {
						// 名前の仮引数かの返戻（`&b` 等の記号付の仮引数は括弧を外すと読みが変わる）
						return std::string_view(ts_node_type(Param)) == "identifier";
					}
				);
			} else if(NodeKind::FunctionLikeDefinition.Contains(Type)) {
				// Ruby の定義の括弧除去後の区切補完（本体の仮引数化と名前末尾への = の融合を防止）
				if(const TSNode Params = TSSource::FieldChild(Node, "parameters"); IsEmpty(Params)) {
					const uint32_t After = TextEdit::SkipSpRight(Src, Src.End(Params));
					const char Next = After < Src.size() ? Src[After] : '\n';
					TextEdit::Push(Src.Start(Params), Src.End(Params), Next == '=' ? " " : Next == '\n' || Next == ';' ? "" : "\n", Edits);
				}
			} else {
				// 呼出対象と実引数列の取得
				const TSNode Arguments = Type == "call" ? TSSource::FieldChild(Node, "arguments") : ts_node_named_child(Node, 0);
				const TSNode Method = Type == "call" ? TSSource::FieldChild(Node, "method") : TSNode{};
				if(
					const std::string_view MethodType = ts_node_is_null(Method) ? std::string_view() : ts_node_type(Method);
					!IsEmpty(Arguments) || Type == "call" && (
						!NodeKind::FunctionNameIdentifier.Contains(MethodType) || (
							ts_node_is_null(TSSource::FieldChild(Node, "receiver")) ?
							!Src.View(Method).ends_with('?') && !Src.View(Method).ends_with('!') :
							MethodType == "constant" && Src.View(TSSource::FieldChild(Node, "operator")) == "::"
						)
					)
					// 条件成立時の返戻
				) return;
				const uint32_t End = Src.End(Arguments), After = TextEdit::SkipSpRight(Src, End);
				const char Next = After < Src.size() ? Src[After] : '\n', Second = After + 1 < Src.size() ? Src[After + 1] : '\n';
				// 条件成立時の返戻
				if(Next == '(') return;
				// 後続字句との結付判定
				const bool ShouldAttach =
				After > End && (Next == '[' || Next == '.' || Next == ':' && Second == ':' || Next == '&' && Second == '.');
				TextEdit::Push(
					Src.Start(Arguments),
					ShouldAttach ? After : End,
					After == End && (IsWordChar(Next) || Next == '?' || Next == '!') ? " " : "",
					Edits
				);
				// 空実引数除去後の演算子と次値の分離
				if(!ShouldAttach && IsOperatorChar(Next)) {
					uint32_t OperatorEnd = After;
					while(OperatorEnd < Src.size() && (IsOperatorChar(Src[OperatorEnd]) || Src[OperatorEnd] == '.')) ++OperatorEnd;
					if(OperatorEnd < Src.size() && Src[OperatorEnd] != ' ' && Src[OperatorEnd] != '\t' && Src[OperatorEnd] != '\n') {
						TextEdit::Push(OperatorEnd, OperatorEnd, " ", Edits);
					}
				}
			}
			// 終了
			return;
		}
	case Lang::Python:
		// Python の類の空の基底の並び (`class A():`)
		if(Type == "class_definition") Drop(TSSource::FieldChild(Node, "superclasses"));
		break;
	case Lang::CSS:
		// SCSS のミックスインの空の仮引数（`@mixin m()`，解析器は空の仮引数を補う）と空の実引数（`@include m()`，解析器は `ERROR` と読む）
		if(NodeKind::ScssMixinRule.Contains(Type)) {
			if(
				const TSNode List =
				Type == "mixin_statement" ? FirstNamedChildOfType(Node, "parameters") : FirstNamedChildOfType(Node, "ERROR");
				!ts_node_is_null(List) && Src[Src.Start(List)] == '(' && (
					Type == "include_statement" ?
					ts_node_child_count(List) == 2 :
					ts_node_named_child_count(List) == 1 && !Src.Len(ts_node_named_child(List, 0))
				)
			) Erase(Src.Start(List), Src.End(List));
		}
		break;
	case Lang::Java:
		// Java の注釈の空の実引数 (`@A()`) と列挙の定数の空の実引数 (`X()`)
		if(NodeKind::JavaEmptyArgumentHost.Contains(Type)) Drop(TSSource::FieldChild(Node, "arguments"));
		break;
	case Lang::CSharp:
		{
			// C# の属性の空の実引数 (`[A()]`) と，初期化子を伴う生成の空の実引数 (`new A() { X = 1 }`)
			if(Type == "attribute") Drop(FirstNamedChildOfType(Node, "attribute_argument_list"));
			if(Type == "object_creation_expression" && !ts_node_is_null(FirstNamedChildOfType(Node, "initializer_expression"))) {
				Drop(FirstNamedChildOfType(Node, "argument_list"));
			}
		}
		break;
	case Lang::PHP:
		{
			// 属性の空の実引数 (`#[A()]`)，`exit()` / `die()` の空の実引数
			if(Type == "attribute") Drop(FirstNamedChildOfType(Node, "arguments"));
			if(
				Type == "exit_statement" && ts_node_child_count(Node) > 2 && Src.View(ts_node_child(Node, 1)) == "(" &&
				Src.View(ts_node_child(Node, 2)) == ")"
			) Erase(Src.Start(ts_node_child(Node, 1)), Src.End(ts_node_child(Node, 2)));
			if(Type == "function_call_expression") {
				// 関数名の取得
				const std::string_view Name = Src.View(TSSource::FieldChild(Node, "function"));
				// 英字大小を無視する照合関数
				const auto Is = [&Name](const std::string_view Word) -> bool {
					// 英字の大小を揃えて一致するかの返戻
					return Name.size() == Word.size() && std::equal(
						Name.begin(),
						Name.end(),
						Word.begin(),
						[](const char Char, const char Lower) -> bool {
							// 英字の小文字化の一致の返戻
							return (Char | 0X20) == Lower;
						}
					);
				};
				// 特別関数の空実引数の除去
				if(Is("die") || Is("exit")) Drop(TSSource::FieldChild(Node, "arguments"));
			}
			// 後置演算子が生成の名前へ移らない様，受け手の生成の空実引数の保持
			if(
				Type == "object_creation_expression" && !(
					!ts_node_is_null(OpAncestor) && ts_node_eq(ts_node_named_child(OpAncestor, 0), OperandTop) &&
					NodeKind::ConstructionReceiverHost.Contains(OpAncestor)
				)
			) {
				const TSNode AnonymousClass = FirstNamedChildOfType(Node, "anonymous_class");
				Drop(FirstNamedChildOfType(ts_node_is_null(AnonymousClass) ? Node : AnonymousClass, "arguments"));
			}
		}
		break;
	default:
		break;
	}
	// 終了
	return;
}

/**
 * 型を包む括弧の中身の取得関数
 * Swift の要素は名札の無い型１つ（`(x: Int)` は名札付の要素，`(repeat each T)` は型の並びの展開を要素に持つ組）
 * @param Src ソースコード
 * @param Node 括弧の型のノード（TS / Kotlin / Go の parenthesized_type，Swift / Rust の要素１つの tuple_type）
 * @param Language 対象言語
 * @return 型１つを包む括弧なら其の型，其れ以外は null
 */
TSNode EditPass::TypeParenContent(const TSSource &Src, const TSNode Node, const Lang Language) {
	// 括弧内型の抽出
	// 開閉の括弧と中身の３つの子で成らない型の空の返戻
	if(ts_node_child_count(Node) != 3 || Src[Src.Start(Node)] != '(' || Src.View(ts_node_child(Node, 2)) != ")") return {};
	TSNode Inner = ts_node_child(Node, 1);
	// Swift 組要素の内側型への透過
	if(Language == Lang::Swift) {
		// 名札付・展開の要素の空の返戻
		if(std::string_view(ts_node_type(Inner)) != "tuple_type_item" || ts_node_child_count(Inner) != 1) return {};
		Inner = ts_node_child(Inner, 0);
		// 型の並びの展開の空の返戻
		if(std::string_view(ts_node_type(Inner)) == "type_pack_expansion") return {};
	}
	// 名前付型の確定
	// 名前付の中身（字句は空）の返戻
	return ts_node_is_named(Inner) ? Inner : TSNode{};
}

/**
 * 型を包む冗長な括弧の除去編集の収集関数
 * 名前等の原子の型 (`(string)` / `(Int)?`) と，型の演算子の被演算子でない位置（注釈・別名・型引数・戻値の型）の型を包む括弧は冗長の為に外す
 * 型の演算子の被演算子に置いた演算子を持つ型 (`(A | B)[]` / `(() -> Unit)?` / `&(dyn A + Send)`) は外すと結合が変わる為に保つ
 * 中身が括弧だけの層 (`((A | B))[]`) は最も外の層を最も内側の中身で判断し，内側の層は判断に依らず外す
 * @param Src ソースコード
 * @param Node 括弧の型のノード（TS / Kotlin / Go の parenthesized_type，Swift / Rust の要素１つの tuple_type）
 * @param Parent 親ノード
 * @param NextSibling 直後の兄弟ノード
 * @param Language 対象言語
 * @param NestedLayers 纏めて判断した内側の層の追加先（走査が其の層の判断を省く）
 * @param Edits 収集先のエディット一覧
 */
void EditPass::MaybeCollectTypeParenEdit(
	const TSSource &Src,
	const TSNode Node,
	const TSNode Parent,
	const TSNode NextSibling,
	const Lang Language,
	std::unordered_set<const void *> &NestedLayers,
	std::vector<TextEdit> &Edits
) {
	// 対象言語と親節点の検査
	const bool IsJsTsLang = Language.IsJsTs();
	if(
		ts_node_is_null(Parent) ||
		!IsJsTsLang && Language != Lang::Swift && Language != Lang::Kotlin && Language != Lang::Rust && Language != Lang::Go
		// 条件成立時の返戻
	) return;
	// 内側型と親型の抽出
	TSNode Inner = TypeParenContent(Src, Node, Language);
	const std::string_view ParentType = ts_node_type(Parent);
	// Swift の関数の型の仮引数の並び (`(Int) -> Int`) は括弧が構文の為に対象外
	if(
		ts_node_is_null(Inner) ||
		Language == Lang::Swift && ParentType == "function_type" && ts_node_eq(ts_node_named_child(Parent, 0), Node)
		// 条件成立時の返戻
	) return;
	uint32_t Depth = 1;
	// 型を包む多重括弧をまとめて判定
	while(NodeKind::GroupingTypeParen.Contains(Inner)) {
		const TSNode Content = TypeParenContent(Src, Inner, Language);
		if(ts_node_is_null(Content)) break;
		NestedLayers.insert(Inner.id);
		++Depth;
		Inner = Content;
	}
	// 最内型の位置関係の初期化
	const std::string_view InnerType = ts_node_type(Inner);
	bool IsOperand = false, ShouldKeep = false;
	if(IsJsTsLang) {
		// TS 型演算子文脈の判定
		IsOperand = NodeKind::TsTypeOperatorHost.Contains(ParentType) || ParentType == "conditional_type" && (
			ts_node_eq(TSSource::FieldChild(Parent, "left"), Node) ?
			NodeKind::TsGreedyType.Contains(InnerType) :
			ts_node_eq(TSSource::FieldChild(Parent, "right"), Node) && InnerType == "conditional_type"
		) || ParentType == "type_annotation" && NodeKind::TsGreedyType.Contains(InnerType) &&
		std::string_view(ts_node_type(ts_node_parent(Parent))) == "arrow_function";
		// 同種の合併・交差ネストと合併内交差での括弧除去許可
		if(
			ParentType == InnerType && NodeKind::TypeCombination.Contains(InnerType) ||
			ParentType == "union_type" && InnerType == "intersection_type"
		) IsOperand = false;
		// 制約付 infer の右端探索
		TSNode Edge = Inner;
		bool IsReopened = false;
		while(std::string_view(ts_node_type(Edge)) != "infer_type") {
			const TSNode Next =
			NodeKind::GroupingTypeParen.Contains(Edge) ? TypeParenContent(Src, Edge, Language) : RightEdgeChild(Src, Edge);
			if(ts_node_is_null(Next)) break;
			IsReopened = IsReopened || NodeKind::TsFunctionType.Contains(Edge);
			Edge = Next;
		}
		// 条件型祖先の末尾追跡
		if(std::string_view(ts_node_type(Edge)) == "infer_type" && ts_node_named_child_count(Edge) == 2) {
			for(TSNode Cur = Node, Up = Parent; !ts_node_is_null(Up); Cur = Up, Up = ts_node_parent(Up)) {
				const std::string_view UpType = ts_node_type(Up);
				if(UpType == "conditional_type") {
					ShouldKeep = IsReopened && ts_node_eq(TSSource::FieldChild(Up, "right"), Cur);
					break;
				}
				if(Src.End(Up) != Src.End(Cur)) break;
				IsReopened = IsReopened || NodeKind::TsFunctionType.Contains(UpType);
			}
		}
	} else switch(Language.Id) {
	case Lang::Swift:
		// 後続の暗黙開封 ! と型の ? の区別（? は結合を争わず括弧を外せる）
		IsOperand = InnerType != "optional_type" && (
			NodeKind::SwiftTypeOperatorHost.Contains(ParentType) &&
			!(ParentType == "protocol_composition_type" && InnerType == "protocol_composition_type") ||
			!ts_node_is_null(NextSibling) && !ts_node_is_named(NextSibling) &&
			(Src.View(NextSibling) == "!" || Src.View(NextSibling) == "?")
		);
		break;
	case Lang::Kotlin:
		// Kotlin 型演算子の被演算子判定
		IsOperand = NodeKind::KotlinTypeOperatorHost.Contains(ParentType);
		break;
	case Lang::Rust:
		// Rust 型演算子の被演算子判定
		IsOperand = NodeKind::RustTypeOperatorHost.Contains(ParentType);
		break;
	default:
		// Go の型変換での名前・並び・本体型だけの括弧除去
		ShouldKeep = ParentType == "type_conversion_expression" && !NodeKind::GoConversionType.Contains(InnerType);
		// Go の向き無チャネルの受信専用要素の括弧保持（<- の掛かる先を変えない）
		IsOperand = ParentType == "channel_type" && ts_node_child_count(Parent) == 2 && Src[Src.Start(Inner)] == '<';
	}
	// 結合を保てる時だけ最外層も外し，其の他は内側だけの除外
	if(!ShouldKeep && !(IsOperand && NodeKind::TypeOperatorInner.Contains(InnerType))) {
		CollectParenPairEdit(Src, Src.Start(Node), Src.End(Node), Depth, TSNode{}, false, Edits);
	} else if(Depth > 1) CollectNestedLayerEdit(Src, Node, Inner, Edits);
	// 終了
	return;
}

/**
 * Python の組の括弧を外せるかの判定関数
 * @param Src ソースコード
 * @param Tuple 組のノード
 * @param Host 組を置く位置の親（組を包む冗長な括弧の外の親）
 * @param Slot Host の子として組を置く位置のノード（組自身又は組を包む括弧）
 * @return 括弧を外せる場合 true
 */
bool EditPass::CanBarePythonTuple(const TSSource &Src, const TSNode Tuple, const TSNode Host, const TSNode Slot) {
	// 組と配置文脈の抽出
	const uint32_t Count = ts_node_named_child_count(Tuple);
	// 裸・空・改行付の組を保持する事の返戻
	if(!Count || Src[Src.Start(Tuple)] != '(' || std::memchr(Src.data() + Src.Start(Tuple), '\n', Src.Len(Tuple))) return false;
	const std::string_view HostType = ts_node_type(Host);
	const bool IsTargetHost = NodeKind::PythonTargetHost.Contains(HostType);
	const bool IsLeft = IsTargetHost && ts_node_eq(TSSource::FieldChild(Host, "left"), Slot);
	const std::string_view Field =
	IsLeft ? "left" : IsTargetHost && ts_node_eq(TSSource::FieldChild(Host, "right"), Slot) ? "right" : "";
	// Python の注釈を記録しない印である代入対象の括弧の保持（多重層は最内で判定）
	if(Field == "left" && !ts_node_is_null(TSSource::FieldChild(Host, "type"))) {
		TSNode Inner = Tuple;
		while(
			ts_node_named_child_count(Inner) == 1 && ts_node_child_count(Inner) == 3 &&
			NodeKind::TupleLike.Contains(ts_node_named_child(Inner, 0))
		) Inner = ts_node_named_child(Inner, 0);
		// 名前付の子を持たない括弧対での空節点
		if(
			const TSNode InnerName = ts_node_named_child(Inner, 0);
			!ts_node_is_null(InnerName) && std::string_view(ts_node_type(InnerName)) == "identifier"
			// 名前を包む括弧を保つ事の返戻
		) return false;
	}
	// 星付要素の検出
	const bool IsStarred = HasChildOf(
		Tuple,
		[](const TSNode Element) -> bool {
			// 星付の要素かの返戻
			return std::string_view(ts_node_type(Element)) == "list_splat";
		}
	);
	// 括弧必須要素の検出
	const bool IsBlocked = HasChildOf(
		Tuple,
		[](const TSNode Element) -> bool {
			// 括弧の無い組に置けない要素かの返戻
			return NodeKind::PythonParenOnlyElement.Contains(Element);
		}
	);
	// 括弧を外せるかの返戻（`return` / `yield` の値の星付の要素は 3.8 以降に限る為に保つ）
	return Count == 1 && ts_node_child_count(Tuple) == 3 || !IsBlocked && (
		NodeKind::PythonBareTupleHost.Contains(HostType) && !HasUnnamedTokenChild(Src, Host, "from") &&
		!(IsStarred && NodeKind::PythonReturnValueHost.Contains(HostType)) || (
			HostType == "for_in_clause" ? Field == "left" : HostType == "for_statement" && Field == "right" ? !IsStarred : !Field.empty()
		) || HostType == "subscript" && !IsStarred && ts_node_child_count(Host) == 4 && !ts_node_eq(ts_node_child(Host, 0), Slot)
	);
}
