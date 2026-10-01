#include "Util/ParsedTree.hpp"
#include "Core/CommentRepository.hpp"
#include "Profile/GrammarProfile.hpp"
#include "Util/TextEdit.hpp"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/** ========== 構文木管理 ========== */
/**
 * 構文木解放関数
 * @param Target 解放する構文木
 */
void ParsedTree::TreeDeleter::operator()(TSTree *const Target) const {
	// 所有構文木の解放
	ts_tree_delete(Target);
	// 終了
	return;
}

/**
 * デストラクタ（スレッド終了時の貯留カーソル解放）
 * 計算量：貯留カーソル数 K と其の領域の解放に対し O(K)
 */
CursorPool::~CursorPool() {
	// 貯留カーソルの解放
	for(TSTreeCursor &Cursor : Free) ts_tree_cursor_delete(&Cursor);
	// 終了
	return;
}

/**
 * 解析器の節点の記号が文法の定義で役割を持つかの判定関数（整形仕様 4.3，節点名の照合を文法の定義へ集める）
 * @param Node 解析器の節点
 * @param Wanted 役割
 * @return 文法の定義で節点の記号が役割を持てば true（言語の指定が無ければ false）
 */
bool ParsedTree::Is(const TSNode Node, const Core::Role Wanted) const {
	// 文法の定義の役割の返戻
	return Grammar && Grammar->Is(Node, Wanted);
}

/**
 * 文法の構文・意味の固定の定義の取得関数
 * @return 解析に用いる文法の構文・意味の固定の定義（言語の指定の在る原稿だけが持つ）
 */
const Core::SyntaxRules &ParsedTree::GetRules() const {
	// 固定の定義の返戻
	return Grammar->GetRules();
}

/**
 * 入力の読取規則の取得関数
 * @return 解析に用いる文法の読取規則
 */
const ReadingRules &ParsedTree::GetReading() const {
	// 読取規則の返戻
	return Grammar->GetReading();
}

/**
 * 解析に用いる文法の定義の取得関数
 * @return 言語の指定と一致する文法の定義
 */
const GrammarProfile &ParsedTree::GetProfile() const {
	// 文法の定義の返戻
	return *Grammar;
}

/**
 * 実行停止後の逐語データの開始位置の取得関数
 * @return データの開始位置（無ければ `std::string::npos`）
 */
size_t ParsedTree::HaltedDataStart() const {
	// 保護する末尾の開始位置の返戻
	return HaltedData;
}

/**
 * 解析済の本文へ逐語データを付ける関数
 * 解析対象の本文を変えずに後置データだけを加える為，構文木の世代を保つ
 * @param Data 実行停止後の逐語データ
 */
void ParsedTree::AttachTrailingData(const std::string_view Data) {
	HaltedData = size();
	std::string::append(Data);
	// 終了
	return;
}

/**
 * 逐語データを除いた解析対象の本文の取得関数
 * @return 解析器へ渡すコードの範囲
 */
std::string_view ParsedTree::CodeText() const {
	// 実行停止後のデータを除いた字面の返戻
	return std::string_view(data(), HaltedData == std::string::npos ? size() : HaltedData);
}

/**
 * 指定された場の役割を持つ最初の子ノード取得関数
 * @param Node 親ノード
 * @param Role 子の場の役割
 * @return 対応する最初の子ノード（無ければ空ノード）
 */
TSNode ParsedTree::FieldChild(const TSNode Node, const Core::ChildRole Role) const {
	// 文法の定義の無い原稿と空の節点の子の無い事の返戻
	if(!Grammar || ts_node_is_null(Node)) return TSNode{};
	TSNode Found {};
	for(const TSFieldId Field : Grammar->FieldsOf(Role)) {
		const TSNode Child = ts_node_child_by_field_id(Node, Field);
		if(ts_node_is_null(Child)) continue;
		if(ts_node_is_null(Found) || ts_node_start_byte(Child) < ts_node_start_byte(Found)) Found = Child;
	}
	// 役割へ写る場の最も前の子の返戻
	return Found;
}

/**
 * 指定された役割を持つ最初の名前付子ノード取得関数
 * @param Node 親ノード
 * @param Role 子の役割
 * @return 対応する最初の名前付子ノード（無ければ空ノード）
 */
TSNode ParsedTree::FirstNamedChild(const TSNode Node, const Core::Role Role) const {
	TSNode Found {};
	ForEachNamedChild(
		Node,
		[&](const TSNode Child) -> bool {
			if(Is(Child, Role)) Found = Child;
			// 役割を持つ最初の子を見付ける迄読み続ける事の返戻
			return ts_node_is_null(Found);
		}
	);
	// 役割を持つ最初の子（無ければ空）の返戻
	return Found;
}

/**
 * 見出の部品を指す場の子かの判定関数
 * 本体を指す場 (`body` / `consequence` / `alternative` / `clauses` / `else`) は含めない
 * 同じ問いを二箇所で書くと一覧が乖離する為，場の名前の一覧を此処だけで定める
 * @param Node 親の節点
 * @param Child 判定する子
 * @return 見出の部品なら true
 */
bool ParsedTree::IsHeaderField(const TSNode Node, const TSNode Child) const {
	// 見出の部品を指す場の名前
	static constexpr Core::ChildRole HeaderFields[] = {
		Core::ChildRole::Condition,
		Core::ChildRole::Exceptions,
		Core::ChildRole::Guard,
		Core::ChildRole::Name,
		Core::ChildRole::ObjectField,
		Core::ChildRole::ParametersField,
		Core::ChildRole::Pattern,
		Core::ChildRole::Superclass,
		Core::ChildRole::Value,
		Core::ChildRole::Variable
	};
	// 場の名前毎の子の照合
	for(const Core::ChildRole Field : HeaderFields) {
		// 見出の部品である事の返戻
		if(const TSNode Part = FieldChild(Node, Field); !ts_node_is_null(Part) && ts_node_eq(Part, Child)) return true;
	}
	// 見出の部品でない事の返戻
	return false;
}

/**
 * if 文の真枝本体取得関数
 * @param Node if 文又は if 式のノード
 * @return 条件成立時に実行する本体（無ければ空ノード）
 */
TSNode ParsedTree::IfConsequence(const TSNode Node) const {
	// 真枝本体の場名は文法で異なり，C/C++/C#/Java/JS/TS 等は `consequence`，PHP は `body` を用いる
	const TSNode Consequence = FieldChild(Node, Core::ChildRole::Consequence);
	// 真枝本体の返戻（`consequence` を持たない文法では `body` へ退避する）
	return ts_node_is_null(Consequence) ? FieldChild(Node, Core::ChildRole::Body) : Consequence;
}

/**
 * 解析済判定関数
 * @return 構文木を保持していれば true
 */
bool ParsedTree::IsParsed() const {
	// 構文木を保持して居るかの返戻
	return Syntax.Tree != nullptr;
}

/**
 * 解析の打切の判定関数
 * @return 解析が時間の上限で打ち切られて居れば true
 */
bool ParsedTree::HasExpiredParse() const {
	// 打切の有無の返戻
	return Syntax.IsParseExpired;
}

/**
 * コメントの本文の範囲の取得関数
 * 末尾の改行と，解析の誤りの回復で頭へ付いた空白を除き，幅０の節点（tree-sitter-kotlin が走査器で呑んだ囲みコメント）は直前の本文をネストに対応して回収する
 * @param Node コメントの節点
 * @return 本文の始まりと終わり（回収出来ない幅０の節点は同じ位置）
 */
std::pair<uint32_t, uint32_t> ParsedTree::CommentRangeOf(const TSNode Node) const {
	const std::string &Self = *this;
	uint32_t Begin = ts_node_start_byte(Node), End = ts_node_end_byte(Node);
	// 末尾の改行を含む節点（Rust の文書化コメント等）の除外
	while(End > Begin && (Self[End - 1] == '\n' || Self[End - 1] == '\r')) --End;
	// 解析の誤りの回復で節点の頭へ付いた空白の除外
	while(Begin < End && (Self[Begin] == ' ' || Self[Begin] == '\t')) ++Begin;
	// 幅の有る節点の範囲の返戻
	if(Begin < End) return { Begin, End };
	// 幅０の節点 (tree-sitter-kotlin) の直前の囲みコメントの終わり
	uint32_t Close = Begin;
	while(Close && (Self[Close - 1] == ' ' || Self[Close - 1] == '\t' || Self[Close - 1] == '\n' || Self[Close - 1] == '\r')) {
		--Close;
	}
	// 直前が囲みコメントで終わらない幅０の節点の返戻
	if(Close < 4 || Self[Close - 2] != '*' || Self[Close - 1] != '/') return { Begin, End };
	// ネストに対応した開きの探索
	uint32_t Open = Close - 2, Depth = 1;
	while(Depth && Open > 1) {
		if(Self[Open - 2] == '/' && Self[Open - 1] == '*') --Depth;
		else if(Self[Open - 2] == '*' && Self[Open - 1] == '/') ++Depth;
		else {
			--Open;
			continue;
		}
		Open -= 2;
	}
	// 対を為す開きの無い幅０の節点と，回収した本文の範囲の返戻
	return Depth ? std::pair{ Begin, End } : std::pair{ Open, Close };
}

/**
 * 構文木と其の控えの差替関数
 * @param NewTree 所有権を引き取る構文木（解析失敗時は nullptr）
 */
void ParsedTree::ReplaceTree(TSTree *const NewTree) {
	// 派生情報を同じ世代へ揃える構文木所有権の先行移動
	Syntax.Tree.reset(NewTree);
	// 本文同期状態の確定と旧根キャッシュの無効化
	Syntax.IsDirty = false;
	Syntax.HasCachedRoot = false;
	// 木の世代毎のコメントの位置表の破棄
	Syntax.CommentSpans.clear();
	Syntax.HasCommentSpans = false;
	// 終了
	return;
}

/**
 * 構文木の再解析関数
 * @throws std::length_error 原文が解析器の位置の範囲を超える場合
 * @throws FormatLimitExceeded 解析が時間の上限を超える場合
 */
void ParsedTree::Reparse() {
	// パーサを持たない場合の返戻
	if(!Syntax.Parser) return;
	// 局所編集の増分解析と `ERROR` 時の全体解析への退避
	if(size() > std::numeric_limits<uint32_t>::max()) throw std::length_error("source exceeds the parser's position range");
	// 解析期限状態の初期化と増分解析
	Syntax.IsParseExpired = false;
	const std::string_view Code(data(), HaltedData == std::string::npos ? size() : HaltedData);
	TSTree *NewTree = Syntax.IsTreeEdited ? Syntax.Parser->Parse(Syntax.Tree.get(), Code, Syntax.IsParseExpired) : nullptr;
	if(!Syntax.IsTreeEdited || NewTree && ts_node_has_error(ts_tree_root_node(NewTree))) {
		// 増分位置の不整合時の木破棄と全体解析（土台にしない旧木は先に放し，新旧２本の木を同時に持たない）
		if(NewTree) ts_tree_delete(NewTree);
		ReplaceTree(nullptr);
		NewTree = Syntax.Parser->Parse(nullptr, Code, Syntax.IsParseExpired);
	}
	// 増分編集通知と期限超過の確定後に木を差替
	Syntax.IsTreeEdited = false;
	if(!NewTree && Syntax.IsParseExpired) throw FormatLimitExceeded("parsing exceeds the time limit");
	ReplaceTree(NewTree);
	// 終了
	return;
}

/**
 * ルートノード取得関数
 * @return ルートノード
 */
TSNode ParsedTree::GetRoot() const {
	// 未反映編集が有る場合の再解析
	if(Syntax.IsDirty) const_cast<ParsedTree *>(this)->Reparse();
	if(!Syntax.HasCachedRoot) {
		// 現在木の根節点
		Syntax.CachedRoot = ts_tree_root_node(Syntax.Tree.get());
		// 根キャッシュの有効化
		Syntax.HasCachedRoot = true;
	}
	// キャッシュ済 Root ノードの返戻（Reparse 時に無効化）
	return Syntax.CachedRoot;
}

/**
 * 原文順のコメントの範囲の表の取得関数
 * コメントの種類の判定（行頭・行末で前後のコメントを飛ばす）を二分探索にし，位置から節点を引き直す費用を避ける
 * 木の世代毎に１度だけ作り，木・本文の差替で捨てる（並列の区間へ入る前に呼ぶ事）
 * 計算量：節点数 V に対し初回 O(V)，以後 O(1)
 * @return 始まりと，末尾の改行を除いた終わりの対の列（始まりの順）
 */
const std::vector<ParsedTree::CommentSpan> &ParsedTree::GetCommentSpans() const {
	if(!Syntax.HasCommentSpans) {
		// 未反映の編集が有れば此処で再解析が走り，表と印を捨てる為，印は作った後に立てる
		if(IsParsed()) {
			const ReadingRules &Reading = GetReading();
			WalkChildrenCursor(
				GetRoot(),
				[this](const TSNode Node) -> bool {
					// コメントでない節点の子を辿る事の返戻
					if(!Is(Node, Core::Role::Comment)) return true;
					// 本文の範囲（幅０の節点は直前の囲みコメントの本文を回収する）
					const auto [Begin, End] = CommentRangeOf(Node);
					// 原文順の範囲の収集
					Syntax.CommentSpans.push_back({ Begin, End, End, false });
					// コメントの中へ降りない事の返戻
					return false;
				}
			);
			// 幅０の節点の回収で前後する為の始まりでの整列
			std::sort(
				Syntax.CommentSpans.begin(),
				Syntax.CommentSpans.end(),
				[](const CommentSpan &Lhs, const CommentSpan &Rhs) -> bool {
					// 始まりの順の比較の返戻
					return Lhs.Start < Rhs.Start;
				}
			);
			const std::string_view Code = CodeText();
			// 原文順に，同じ行の前のコメントを継いだ行頭の判定（`/* a */ /* b */` の `/* b */` も行頭）
			for(size_t Idx = 0; Idx < Syntax.CommentSpans.size(); ++Idx) {
				CommentSpan &Span = Syntax.CommentSpans[Idx];
				const uint32_t Lead = TextEdit::SkipSpLeft(*this, Span.Start, Reading);
				Span.IsLineHead =
				!Lead || at(Lead - 1) == '\n' || Idx && Syntax.CommentSpans[Idx - 1].End == Lead && Syntax.CommentSpans[Idx - 1].IsLineHead;
			}
			// 逆順に，後の空白と続くコメントを飛ばした位置（鎖の末尾から継ぐ）
			for(size_t Idx = Syntax.CommentSpans.size(); Idx--;) {
				CommentSpan &Span = Syntax.CommentSpans[Idx];
				const uint32_t After = TextEdit::SkipSpRight(Code, Span.End, Reading);
				const bool IsChained =
				Idx + 1 < Syntax.CommentSpans.size() && Syntax.CommentSpans[Idx + 1].Start == After && Syntax.CommentSpans[Idx + 1].End > After;
				Span.RunEnd = IsChained ? Syntax.CommentSpans[Idx + 1].RunEnd : After;
			}
		}
		Syntax.HasCommentSpans = true;
	}
	// コメントの範囲の表の返戻
	return Syntax.CommentSpans;
}

/**
 * 同一親内の直前の名前付非コメント子取得関数（親を遡らない）
 * @param Node 基準ノード
 * @return 直前の名前付非コメント子（見付からなければ空ノード）
 */
TSNode ParsedTree::SiblingBeforeImmediate(const TSNode Node) const {
	// 根から親を辿り直す ts_node_prev_named_sibling は，二乗走査を避け錨直前の稀な１件照会に限定
	for(TSNode Cur = ts_node_prev_named_sibling(Node); !ts_node_is_null(Cur); Cur = ts_node_prev_named_sibling(Cur)) {
		// コメントでない直前の兄弟の返戻
		if(!Is(Cur, Core::Role::Comment)) return Cur;
	}
	// 直前の名前付非コメント子（見付からなければ空ノード）の返戻
	return {};
}

/**
 * 可変構文木取得関数
 * 呼出側は得た木を `ts_tree_edit` で編集後のバイト位置へ動かす
 * @return 内部の構文木（解析前は nullptr）
 */
TSTree *ParsedTree::GetMutableTree() {
	// 内部の構文木の返戻
	return Syntax.Tree.get();
}

/**
 * 構文木編集済の設定関数
 * 編集済なら次の再解析で旧構文木を土台に増分解析し，増分通知を諦めた適用の後は全体解析へ倒す
 * @param IsEdited 旧構文木へ編集を通知したら true
 */
void ParsedTree::SetTreeEdited(const bool IsEdited) {
	// 次回解析方式の記録
	Syntax.IsTreeEdited = IsEdited;
	// 終了
	return;
}

/**
 * ノードの終了バイト取得関数
 * 空のノード（無い場の子）は ts_node_end_byte が中身を辿って落ちる為，開始と同じ位置（空の範囲）を返す
 * @param Node 対象ノード
 * @return 終了バイト位置（排他的）
 */
uint32_t ParsedTree::End(const TSNode Node) const {
	// 空節点を含む終了位置の返戻
	return ts_node_is_null(Node) ? 0 : ts_node_end_byte(Node);
}

/**
 * ノードの開始バイト取得関数
 * @param Node 対象ノード
 * @return 開始バイト位置
 */
uint32_t ParsedTree::Start(const TSNode Node) const {
	// 空節点への `ts_node_start_byte` は解析器の保証外の為，`End` と対称に０を返す
	// 開始位置の返戻
	return ts_node_is_null(Node) ? 0 : ts_node_start_byte(Node);
}

/**
 * ノード範囲のバイト長取得関数
 * @param Node 対象ノード
 * @return 終了バイトと開始バイトの差
 */
uint32_t ParsedTree::Len(const TSNode Node) const {
	// 節点範囲の長さの返戻
	return End(Node) - Start(Node);
}

/**
 * ノード範囲のビュー取得関数
 * @param Node 対象ノード
 * @return ノード範囲への複写無のビュー
 */
std::string_view ParsedTree::View(const TSNode Node) const {
	// 節点の原文範囲
	const uint32_t Begin = Start(Node), End = this->End(Node);
	// 範囲が有効ならビュー，空なら空ビューの返戻
	return Begin < End ? std::string_view(data() + Begin, End - Begin) : std::string_view();
}

/**
 * 節点の字面が役割を持つ字句の綴りかの判定関数（誤りの回復が字句を誤り区画の字面だけで残す形を含む）
 * @param Node 解析器の節点
 * @param Wanted 字句の役割
 * @return 字面と同じ綴りの字句が文法の定義で役割を持てば true（言語の指定が無ければ false）
 */
bool ParsedTree::SpellsToken(const TSNode Node, const Core::Role Wanted) const {
	// 言語の指定が無いか節点の無い事の返戻
	if(!Grammar || ts_node_is_null(Node)) return false;
	const TSSymbol Symbol = Grammar->TokenSymbol(View(Node));
	// 同じ綴りの字句の役割の返戻
	return Symbol && Grammar->KindOf(Symbol).Roles.test(static_cast<size_t>(Wanted));
}

/** ========== 字句・文脈判定 ========== */
/**
 * 文へ展開するマクロの呼出の文かの判定関数
 * 置換本体が複数の文や開いた if に為るマクロ (`#define SWAP(a, b) t = a; a = b; b = t`) の呼出は，単文の本体に置くと先頭の文だけが本体に為る
 * @param Stmt 判定する文
 * @return 文へ展開するマクロ（CollectMacros で集めた名前）の呼出だけの式文なら true
 */
bool ParsedTree::ExpandsToStatements(const TSNode Stmt) const {
	// 対象のマクロが無い場合と，式文でない場合の返戻
	if(StatementMacros.empty() || !Is(Stmt, Core::Role::ExpressionStatement)) return false;
	// 式文の先頭式
	TSNode Name = ts_node_named_child(Stmt, 0);
	if(!ts_node_is_null(Name) && Is(Name, Core::Role::CallExpression)) Name = FieldChild(Name, Core::ChildRole::FunctionField);
	// マクロの名前かの返戻
	return !ts_node_is_null(Name) && Is(Name, Core::Role::Identifier) && StatementMacros.contains(std::string(View(Name)));
}

/**
 * 構造化で省く最上位の空の式文 `;` かの判定関数
 * 破損した直前の子の宣言を終える `;` は其の子へ密着させて残す為に省かない
 * コメントの分離は省く `;` を錨にせず，中間表現の構築は其れを組立から除く
 * @param Child 根の子
 * @param PrevChild 其の前のコメントでない根の名前付の子（無ければ空）
 * @return 省く `;` だけの式文なら true
 */
bool ParsedTree::IsOmittedSemicolon(const TSNode Child, const TSNode PrevChild) const {
	// 省く空の式文かの返戻
	return GetRules().IsSpelling(View(Child), Core::Word::StatementEnd) && Is(Child, Core::Role::ExpressionStatement) &&
	(ts_node_is_null(PrevChild) || !ts_node_has_error(PrevChild));
}

/**
 * 字面が名前の何れかを含むかの判定関数
 * 計算量：字面の長さ N に対し平均 O(N)
 * @param Text 判定する字面
 * @param Names 名前の集合
 * @return 識別子として名前の何れかを含めば true
 */
bool ParsedTree::MentionsName(const std::string_view Text, const std::unordered_set<std::string> &Names) {
	// 識別子候補の先頭からの走査
	for(size_t Pos = 0; Pos < Text.size();) {
		// 識別子外のバイトの読飛
		if(!IsIdentifierChar(Text[Pos])) {
			// 非識別子の読み飛ばし
			++Pos;
			continue;
		}
		// 識別子の終端候補
		size_t End = Pos;
		while(End < Text.size() && IsIdentifierChar(Text[End])) ++End;
		// 名前を含む事の返戻
		if(Names.contains(std::string(Text.substr(Pos, End - Pos)))) return true;
		// 次の字句位置
		Pos = End;
	}
	// 名前を含まない事の返戻
	return false;
}

/**
 * 宣言子の記号へ展開するマクロを含む型かの判定関数
 * 型位置の宣言子マクロは宣言の統合・分割や `const` 移動で型を変え得る
 * @param Type 判定する型のノード
 * @return 型の字面に CollectMacros で集めた名前が有れば true
 */
bool ParsedTree::MentionsDeclaratorMacro(const TSNode Type) const {
	// 対象のマクロの名前を含むかの返戻
	return !DeclaratorMacros.empty() && MentionsName(View(Type), DeclaratorMacros);
}

/**
 * 括弧で包まずに演算子を含む式へ展開するマクロを含むかの判定関数
 * 括弧無の式へ展開するマクロは周囲の括弧除去で結合が変わる
 * @param Node 判定するノード
 * @return 字面に CollectMacros で集めた名前が有れば true
 */
bool ParsedTree::MentionsExpressionMacro(const TSNode Node) const {
	// 対象のマクロの名前を含むかの返戻
	return !ExpressionMacros.empty() && MentionsName(View(Node), ExpressionMacros);
}

/**
 * 名前のハッシュ算出関数
 * 計算量：名前の長さ N に対し O(N)
 * @param Name 所有する名前又は本文から得た名前のビュー
 * @return 名前の字面に対応するハッシュ値
 */
size_t ParsedTree::NameHash::operator()(const std::string_view Name) const noexcept {
	// 文字列とビューの照会を同じハッシュで扱う為の返戻
	return std::hash<std::string_view>()(Name);
}

/**
 * 同じファイルで定義した関数形式マクロ（別名を含む）の名前かの判定関数
 * @param Name 判定する名前
 * @return CollectMacros で集めた関数形式マクロか其の別名なら true
 */
bool ParsedTree::IsFunctionMacro(const std::string_view Name) const {
	// 関数形式マクロの名前かの返戻
	return !FunctionMacros.empty() && FunctionMacros.contains(Name);
}

/**
 * 使う位置で有効な宣言の有無の判定関数
 * 最も新しい有効範囲内の宣言と使用位置の間に取込指令が無い場合だけ有効と看做す
 * 計算量：同名の宣言数 D，取込数 I に対し O(D + log(I + 1))
 * @param Declarations 宣言位置と有効範囲の終端（宣言位置順）
 * @param Use 使う位置
 * @return 使う位置を有効範囲に含む宣言が使う位置より前に在り，其の後に取込の指令の無い場合 true
 */
bool ParsedTree::IsVisibleDeclaration(
	const std::vector<std::pair<uint32_t, uint32_t>> &Declarations,
	const uint32_t Use
) const {
	// 使用位置より前の宣言を新しい順に調べる
	std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Declared = std::lower_bound(
		Declarations.begin(),
		Declarations.end(),
		Use,
		[](const std::pair<uint32_t, uint32_t> &Declaration, const uint32_t Position) -> bool {
			// 使用位置より前の宣言かの返戻
			return Declaration.first < Position;
		}
	);
	while(Declared != Declarations.begin()) {
		if((--Declared)->second <= Use) continue;
		const std::vector<uint32_t>::const_iterator Include =
		std::upper_bound(IncludeStarts.begin(), IncludeStarts.end(), Declared->first);
		// 最も新しい有効な宣言から使う位置迄に取込の無いかの返戻
		return Include == IncludeStarts.end() || *Include > Use;
	}
	// 使う位置で有効な宣言が無い事の返戻
	return false;
}

/**
 * 使う位置で有効な宣言を同じファイルに持つ名前かの判定関数
 * 計算量：名前の長さ N，同名の宣言数 D，取込数 I に対し平均 O(N + D + log(I + 1))
 * @param Name 名前
 * @param Use 使う位置
 * @return 使う位置で有効な変数・仮引数・関数・列挙子の宣言が前に在り，其の後に取込の指令の無い名前なら true
 */
bool ParsedTree::IsDeclaredName(const std::string_view Name, const uint32_t Use) const {
	// 同名の宣言の位置と有効範囲
	const decltype(DeclaredNames)::const_iterator Found = DeclaredNames.find(Name);
	// 使う位置で有効な宣言の有無の返戻
	return Found != DeclaredNames.end() && IsVisibleDeclaration(Found->second, Use);
}

/**
 * 使う位置で有効な型宣言を同じファイルに持つ名前かの判定関数
 * 計算量：名前の長さ N，同名の宣言数 D，取込数 I に対し平均 O(N + D + log(I + 1))
 * @param Name 名前
 * @param Use 使う位置
 * @return 使う位置で有効な型の宣言が前に在り，其の後に取込の指令の無い名前なら true
 */
bool ParsedTree::IsDeclaredTypeName(const std::string_view Name, const uint32_t Use) const {
	// 同名の型宣言の位置と有効範囲
	const decltype(DeclaredTypeNames)::const_iterator Found = DeclaredTypeNames.find(Name);
	// 使う位置で有効な型宣言の有無の返戻
	return Found != DeclaredTypeNames.end() && IsVisibleDeclaration(Found->second, Use);
}

/**
 * 式と区別出来ない宣言かの判定関数
 * 型名と宣言子の間に演算子又は型引数が在る場合は，同じファイルの型宣言で型の読みに確証を要する
 * @param Node 宣言の候補
 * @return 型の読みを確定出来ない宣言なら true
 */
bool ParsedTree::IsAmbiguousDeclaration(const TSNode Node) const {
	// 式とも読める宣言以外の返戻
	if(!Is(Node, Core::Role::PotentialExpressionDeclaration)) return false;
	const TSNode Type = FieldChild(Node, Core::ChildRole::Type);
	TSNode Declarator = FieldChild(Node, Core::ChildRole::Declarator), Last = Type;
	// 初期化子を除いた宣言子（`T x = 1` も名前が並ぶ為に式に為らない）と，修飾名の最後の成分（`ns::T<1>` の型引数付の名前）
	if(Is(Declarator, Core::Role::InitDeclarator)) Declarator = FieldChild(Declarator, Core::ChildRole::Declarator);
	while(Is(Last, Core::Role::QualifiedIdentifier)) Last = FieldChild(Last, Core::ChildRole::Name);
	const bool IsTemplate = Is(Last, Core::Role::TemplateInstantiationType);
	// 二つの名前が並ぶ形（演算式に為らない）の返戻
	if(!IsTemplate && Is(Declarator, Core::Role::Identifier)) return false;
	// 型を語又は組込型で明示した形（式に為らない）の返戻
	if(
		Is(Type, Core::Role::PrimitiveType) || Is(Type, Core::Role::SizedTypeSpecifier) || Is(Type, Core::Role::ClassLike) ||
		Is(Type, Core::Role::EnumSpecifier)
	) return false;
	if(IsTemplate) {
		const TSNode Arguments = FieldChild(Last, Core::ChildRole::Arguments);
		for(uint32_t Index = 0; Index < ts_node_named_child_count(Arguments); ++Index) {
			const TSNode Argument = FieldChild(ts_node_named_child(Arguments, Index), Core::ChildRole::Type);
			// 型引数に組込型を持つ形（`<` を比較と読むと組込型の語が式に為らない）の返戻
			if(Is(Argument, Core::Role::PrimitiveType) || Is(Argument, Core::Role::SizedTypeSpecifier)) return false;
		}
	}
	const TSNode Name = IsTemplate ? ts_node_named_child(Last, 0) : Type;
	// 裸の型名でない型（修飾名等）は式を否定出来ない事の返戻
	if(!Is(Name, Core::Role::TypeName)) return true;
	const std::string_view Spelling = View(Name);
	const uint32_t Use = ts_node_start_byte(Node);
	// 値の宣言が先行する名前と，使用位置で型の宣言を確認出来ない名前は式を否定出来ない事の返戻
	return IsDeclaredName(Spelling, Use) || !IsDeclaredTypeName(Spelling, Use);
}

/**
 * C の真偽値の名前を使える位置かの判定関数
 * `<stdbool.h>` を条件の外で取り込んだ後だけ `true` / `false` を宣言済と看做す（版に依る組込の語は構文から決めない）
 * @param Use 使う位置
 * @return 使う位置より前に真偽値の名前を宣言する取込が在れば true
 */
bool ParsedTree::HasBoolNamesAt(const uint32_t Use) const {
	// 取込の後の位置かの返戻
	return BoolNamesStart <= Use;
}

/**
 * CSS の `url(...)` の引数の並びかの判定関数（関数名の大小は CSS 仕様上不問）
 * @param Node 判定対象のノード
 * @return `url` 呼出の引数の並びなら true
 */
bool ParsedTree::IsCssUrlArguments(const TSNode Node) const {
	// 別の名前・成員呼出を除く url の前置区切り
	static constexpr std::string_view NameDelimiters = ":(,;{} \t\n";
	const std::string_view Head = GetReading().UrlFunctionHead;
	const std::string_view UrlName = Head.empty() ? Head : Head.substr(0, Head.size() - 1);
	// 関数名は構文木でなく開き括弧直前の字面で見る（SCSS のマップ記法では関数名ノードが `ERROR` へ崩れ木からは辿れない）
	const uint32_t Start = ts_node_start_byte(Node);
	// 引数の並びでない場合と，開き括弧の前に関数名の長さが取れない場合の返戻
	if(UrlName.empty() || !Is(Node, Core::Role::CallArguments) || Start < UrlName.size() || Start > size()) return false;
	// 一文字でも異なる関数名の場合の返戻（CSS の関数名は大小不問の為，ASCII の大小差分を落として照合する）
	for(size_t Idx = 0; Idx < UrlName.size(); ++Idx) {
		// url の名前と異なる事の返戻
		if((static_cast<unsigned char>(at(Start + Idx - UrlName.size())) | 0X20) != UrlName[Idx]) return false;
	}
	// 関数名直前の字
	const char Preceding = Start > UrlName.size() ? at(Start - UrlName.size() - 1) : '\0';
	// 関数名の直前が区切り又はファイル先頭で `url` 呼出と確定した事の返戻
	return !Preceding || NameDelimiters.find(Preceding) != std::string_view::npos;
}

/**
 * Ruby の符号を密着させても読みが変わらない被演算子かの判定関数
 * @param Operand 符号の被演算子
 * @return 密着させても読みが変わらなければ true
 */
bool ParsedTree::AttachesRubySign(const TSNode Operand) const {
	// 数字で始まる被演算子の検査
	if(ts_node_is_null(Operand) || Start(Operand) >= size() || !std::isdigit(static_cast<unsigned char>(at(Start(Operand))))) {
		// 空節点と数字で始まらない被演算子の返戻（空節点は位置も親も引けない）
		return true;
	}
	// 空節点の親は引けない為，先に確かめてから外側を辿る
	// 後置の受け手と読まれた離れた符号の返戻
	if(
		const TSNode Unary = ts_node_parent(Operand), Outer = ts_node_is_null(Unary) ? TSNode{} : ts_node_parent(Unary);
		!ts_node_is_null(Unary) && Is(Unary, Core::Role::UnaryValue) && Start(Unary) + 1 < Start(Operand) &&
		Is(Outer, Core::Role::PostfixReceiverChain) && ts_node_eq(ts_node_named_child(Outer, 0), Unary)
	) return false;
	// 符号直下の数値底
	const TSNode Base =
	Is(Operand, Core::Role::Binary) && Is(FieldChild(Operand, Core::ChildRole::OperatorField), Core::Role::ExponentToken) ?
	ts_node_named_child(Operand, 0) :
	Operand;
	// 数値だけか，数値を底とする冪かの返戻
	return Is(Base, Core::Role::NumberLiteral) || Is(Base, Core::Role::NumericUnitSuffix);
}

/**
 * Swift の前置の演算子の呼出と読んだ括弧の実引数の並びの取得関数
 * tree-sitter-swift は前置の演算子の後の括弧 (`-(y)` / `!(y)`) を演算子の呼出と読む
 * 否定の `!` は名前付の `bang` の節点に為り，中置の式の後の前置 (`x + -(y)`) は其の中置の式の末尾の字句に為る（`(x + -)(y)` と読む）
 * @param Node 対象ノード
 * @return 前置の演算子の被演算子を包む括弧と読んだ実引数の並び（其の形でなければ null）
 */
TSNode ParsedTree::SwiftPrefixArguments(const TSNode Node) const {
	// 前置の演算子の呼出の形でない事の返戻
	if(ts_node_child_count(Node) != 2 || !Is(Node, Core::Role::CallExpression)) return {};
	// 呼出側の前置演算子の候補
	TSNode Prefix = ts_node_child(Node, 0);
	if(IsNamedNode(Prefix) && !Is(Prefix, Core::Role::ExclamationMark)) {
		// 中置式末尾の演算子候補への移動
		const uint32_t Count = ts_node_child_count(Prefix);
		Prefix = Count > 1 ? ts_node_child(Prefix, Count - 1) : TSNode{};
		// 末尾が演算子の字句でない事の返戻
		if(ts_node_is_null(Prefix) || ts_node_is_named(Prefix)) return {};
	}
	// 呼出接尾辞と実引数列の検査
	const TSNode Suffix = ts_node_child(Node, 1);
	if(!IsOperatorChar((*this)[Start(Prefix)]) || !Is(Suffix, Core::Role::CallSuffix) || ts_node_named_child_count(Suffix) != 1) {
		// 前置の演算子と実引数の並びの組でない事の返戻
		return {};
	}
	// 実引数節点の取得
	const TSNode Arguments = ts_node_named_child(Suffix, 0);
	// 実引数の並びの返戻
	return Is(Arguments, Core::Role::ValueArguments) ? Arguments : TSNode{};
}

/**
 * 見出の本体の始まりと紛れる式の判定関数
 * 制御見出で本体の波括弧と誤読される式の包含判定関数
 * 実引数・配列・添字・文字列等の区切りの中と，関数・クロージャの本体の中の物は紛れない（要素１つの括弧は外れ得る為に透過して見る）
 * @param Node 対象ノード
 * @return 紛れる式を持つなら true
 */
bool ParsedTree::HasBodyLikeExpression(const TSNode Node) const {
	const ReadingRules &Reading = GetReading();
	const auto BodyLike = [this, &Reading](const TSNode Target) -> bool {
		// 本体の始まりと紛れる式かの返戻
		return Reading.HasCompositeHeaderExpression && Is(Target, Core::Role::CompositeLiteral) &&
		Is(FieldChild(Target, Core::ChildRole::Type), Core::Role::CompositeTypeName) ||
		Reading.HasStructHeaderExpression && Is(Target, Core::Role::StructExpression) ||
		Reading.HasPrefixCallHeaderExpression && Is(Target, Core::Role::LambdaLiteral);
	};
	// 紛れる式其の物の返戻
	if(BodyLike(Node)) return true;
	// 子孫探索状態と Swift 括弧透過候補の準備
	bool IsFound = false;
	// Swift の前置の演算子の被演算子を包む括弧 (`!(x)`) は，演算子の呼出の実引数の並びと読まれても外れ得る為に透過
	std::vector<const void *> PrefixArguments;
	const auto NotePrefix = [&](const TSNode Call) -> void {
		if(const TSNode Arguments = SwiftPrefixArguments(Call); !ts_node_is_null(Arguments)) PrefixArguments.push_back(Arguments.id);
	};
	// 根自身の前置呼出の括弧透過候補への追加
	if(Reading.HasPrefixCallHeaderExpression) NotePrefix(Node);
	WalkChildrenCursor(
		Node,
		[&](const TSNode Child) -> bool {
			// 見付けた後に降りない事の返戻
			if(IsFound) return false;
			// 紛れる式の検出と探索打切
			if(BodyLike(Child)) {
				IsFound = true;
				// 見付けた為に降りない事の返戻
				return false;
			}
			// 子の型に応じた Swift 括弧候補の記録
			if(Reading.HasPrefixCallHeaderExpression && Is(Child, Core::Role::CallExpression)) NotePrefix(Child);
			// 区切り字句を持つ子の降下可否判定
			const TSNode First = ts_node_child(Child, 0);
			// 区切りの括弧・引用符の字句で始まる節点（要素１つの括弧を除く）の中へは降りない事の返戻
			return ts_node_is_null(First) || ts_node_is_named(First) || !SpellsToken(First, Core::Role::BodyDelimiterLead) ||
			Is(Child, Core::Role::ValueParentheses) || Reading.HasPrefixCallHeaderExpression && ts_node_named_child_count(Child) == 1 && (
				Is(Child, Core::Role::TupleExpression) ||
				std::find(PrefixArguments.begin(), PrefixArguments.end(), Child.id) != PrefixArguments.end()
			);
		}
	);
	// 紛れる式を持つかの返戻
	return IsFound;
}

/**
 * Ruby の実引数の先頭の曖昧さの判定関数
 * 空白を挟んだ単項の `-`・単項の `+`・範囲演算子で始まる値は，前の語との二項式・範囲と読み得る（括弧の中の先頭も見る）
 * @param Lead 実引数の値
 * @return 先頭が曖昧なら true
 */
bool ParsedTree::LeadsRubyAmbiguously(TSNode Lead) const {
	// 実引数先頭の葉迄降下して字面を取得
	while(!ts_node_is_null(Lead) && ts_node_child_count(Lead)) {
		// 空白を挟んで残る符号の返戻
		if(
			Is(Lead, Core::Role::UnaryValue) && View(ts_node_child(Lead, 0)) == GetRules().Operators.Minus &&
			!AttachesRubySign(ts_node_named_child(Lead, 0))
		) return true;
		// 括弧を透過して最左子へ移動
		Lead = Is(Lead, Core::Role::GroupingParen) ? ts_node_named_child(Lead, 0) : ts_node_child(Lead, 0);
	}
	// 単項加算又は範囲演算子かの返戻
	return !ts_node_is_null(Lead) && (View(Lead).starts_with(GetRules().Operators.Plus) || Is(Lead, Core::Role::RangeOperator));
}

/**
 * Ruby の局所変数の導入済判定関数（索引の対象の節点の手前で，名前が局所変数に為って居るか）
 * @param Node 索引の対象の節点
 * @param Name 局所変数の名前
 * @param Start 判定位置
 * @return 同じスコープと外を見るブロックの，判定位置の手前の代入・仮引数で導入済なら true
 */
bool ParsedTree::RubyLocals::IsDefined(const TSNode Node, const std::string_view Name, const uint32_t Start) const {
	const std::unordered_map<const void *, std::vector<const void *>>::const_iterator Chain = Chains.find(Node.id);
	// 索引を持たない節点の未定義の返戻
	if(Chain == Chains.end()) return false;
	// 参照可能な有効範囲だけで名前の導入位置を照合
	for(const void *const Scope : Chain->second) {
		const std::unordered_map<const void *, std::unordered_map<std::string_view, uint32_t>>::const_iterator Names =
		// スコープ別の名前導入索引
		First.find(Scope);
		// 名前を持たないスコープの読飛
		if(Names == First.end()) continue;
		// 対象名を手前で導入済の場合の返戻
		if(
			const std::unordered_map<std::string_view, uint32_t>::const_iterator Found = Names->second.find(Name);
			Found != Names->second.end() && Found->second <= Start
		) return true;
	}
	// 全スコープで未定義の返戻
	return false;
}

/**
 * Ruby の局所変数の導入の索引の構築関数
 * Ruby は代入を解析した時点から名前を局所変数と読み，其れ迄は同じ名前をメソッドの呼出と読む
 * スコープ毎に名前の最初の導入位置（代入の左辺・仮引数の名前の終わり）を１回の走査で集め，対象の節点毎に導入の見えるスコープを控える
 * 読み切れない形（ネストの分割代入等）は導入して居ないと看做し，呼出側を保守側へ倒す
 * @param Targets 導入の見えるスコープを控える節点の役割
 * @return 導入の索引
 */
ParsedTree::RubyLocals ParsedTree::IndexRubyLocals(const Core::Role Targets) const {
	// 構築する Ruby 局所変数索引
	RubyLocals Locals;
	std::vector<std::pair<const void *, bool>> Scopes;
	std::vector<bool> Opened;
	const auto Record = [&](const TSNode Identifier) -> void {
		// 識別子でないか有効範囲の外の場合の終了
		if(ts_node_is_null(Identifier) || !Is(Identifier, Core::Role::Identifier) || Scopes.empty()) return;
		// 再代入による最初の導入位置の上書防止
		Locals.First[Scopes.back().first].try_emplace(View(Identifier), End(Identifier));
	};
	// 節点進入時の索引更新関数
	const auto Enter = [&](const TSNode Node) -> void {
		// 仮引数の並びの場合
		if(Is(Node, Core::Role::LocalParameters)) {
			// 仮引数は名前か，名前を先頭の子に持つ節点（`b = 1` / `*c` 等）
			ForEachNamedChild(
				Node,
				// 各仮引数の識別子抽出
				[this, &Record](const TSNode Parameter) -> void {
					// 包装を剥がした仮引数名の記録
					Record(Is(Parameter, Core::Role::Identifier) ? Parameter : ts_node_named_child(Parameter, 0));
				}
			);
		} else if(
			// 代入左辺となる束縛節点
			const TSNode Left = Is(Node, Core::Role::BindingExpression) ? FieldChild(Node, Core::ChildRole::Left) : TSNode{};
			!ts_node_is_null(Left)
		) {
			// 分割代入は並びの直下の名前
			if(Is(Left, Core::Role::LeftAssignmentList)) ForEachNamedChild(Left, Record);
			else Record(Left);
		}
		// 索引対象となる候補節点の場合
		if(Is(Node, Targets)) {
			std::vector<const void *> &Chain = Locals.Chains[Node.id];
			// 内側から外側へのスコープ走査
			for(size_t Idx = Scopes.size(); Idx--;) {
				Chain.push_back(Scopes[Idx].first);
				if(Scopes[Idx].second) break;
			}
		}
		// 子の導入記録前の有効範囲開始
		const bool IsScope = Is(Node, Core::Role::LocalScope);
		// スコープ経路へ境界を追加
		if(IsScope) Scopes.emplace_back(Node.id, Is(Node, Core::Role::IsolatedScope));
		// 節点のスコープ開始有無を記録
		Opened.push_back(IsScope);
		// 終了
		return;
	};
	const auto Leave = [&]() -> void {
		// 開始したスコープと節点印の除去
		if(Opened.back()) Scopes.pop_back();
		Opened.pop_back();
		// 終了
		return;
	};
	// 深さ優先走査カーソルと根の有効範囲の初期化
	HeldCursor Cursor(GetRoot());
	Enter(ts_tree_cursor_current_node(&Cursor));
	// 構文木の深さ優先走査
	while(true) {
		// 最初の子へ降りれる場合
		if(ts_tree_cursor_goto_first_child(&Cursor)) {
			Enter(ts_tree_cursor_current_node(&Cursor));
			continue;
		}
		// 兄弟又は親を探す退出走査
		bool IsDone = false;
		while(true) {
			// 現在節点の退出処理
			Leave();
			if(ts_tree_cursor_goto_next_sibling(&Cursor)) {
				Enter(ts_tree_cursor_current_node(&Cursor));
				break;
			}
			// 親へ戻れない場合
			if(!ts_tree_cursor_goto_parent(&Cursor)) {
				IsDone = true;
				// 祖先復帰の終了
				break;
			}
		}
		// 完了時に外側走査を終了
		if(IsDone) break;
	}
	// 構築した Ruby 局所変数索引の返戻
	return Locals;
}

/**
 * 字句の間の改行で読みが変わるかの判定関数（改行を含む囲みコメントも改行と読む言語で，其のコメントを置き又は外す所の検査）
 * Go は名前・リテラル・閉じ括弧・後置の増減・`return` 等の後の改行に自動の `;` を補い，Swift も其処の改行で文を終える
 * Kotlin は改行の有無をコメントを読み飛ばして判定する為，コメントの中の改行で読みが変わらない
 * JS/TS は続く字句を読める限り改行で文を終えず，制限された語（`return` / `yield` 等）の後と `=>`・後置の増減の前だけで読みが変わる
 * @param PrevEnd 前の字句の終わり
 * @param NextStart 後の字句の始まり
 * @return 改行で読みが変わるなら true
 */
bool ParsedTree::BreaksReadingAt(const uint32_t PrevEnd, const uint32_t NextStart) const {
	const Core::SyntaxRules &Rules = GetRules();
	const ReadingRules &Reading = GetReading();
	// 改行を読まない言語と，既に改行の後の位置の返戻
	if(!Reading.HasLineSensitiveBoundary || !PrevEnd || at(PrevEnd - 1) == '\n') return false;
	const std::string_view Code = CodeText();
	const std::string_view Next = Code.substr(std::min<size_t>(NextStart, Code.size()));
	// 前に改行を禁じる字句（矢印・後置の増減）の前の返戻
	if(
		Reading.HasRestrictedLineWords && (
			Next.starts_with(Reading.RestrictedLineArrow) || Next.starts_with(Rules.Operators.Increment) ||
			Next.starts_with(Rules.Operators.Decrement)
		)
	) return true;
	// Swift は後に空白の在る二項演算子（前の改行も空白）の前の改行で式を続ける事の返戻
	if(
		const size_t Operator = Next.find_first_not_of(Reading.LeadingBinaryOperatorChars);
		Reading.HasLeadingBinaryOperators && Operator && Operator < Next.size() &&
		(Next[Operator] == ' ' || Next[Operator] == '\t' || Next[Operator] == '\n')
	) return false;
	const char Prev = at(PrevEnd - 1);
	if(Reading.ExpressionContinuationChars.find(Prev) != std::string_view::npos) {
		// 演算子・開き括弧・区切りの後は式が続き，後置の増減の後だけ式を終える事の返戻
		return PrevEnd > 1 && (Prev == '+' || Prev == '-') && at(PrevEnd - 2) == Prev;
	}
	// 前の字句（語は名前の無い字句，語の字でなければ引かず，位置からの引直の費用を避ける）
	if(
		const TSNode Token = IsIdentifierChar(Prev) ? ts_node_descendant_for_byte_range(GetRoot(), PrevEnd - 1, PrevEnd) : TSNode{};
		!ts_node_is_null(Token) && !ts_node_is_named(Token)
	) {
		// 後続を要する語（`else` / `typeof` 等）の後の返戻
		if(Is(Token, Core::Role::ContinuingWord)) return false;
		// JS / TS は後に改行を挟めない語（`return` / `async` 等）の後だけ読みが変わり，其の他の言語は語の後でも文を終え得る事の返戻
		return !Reading.HasRestrictedLineWords || Is(Token, Core::Role::RestrictedLineWord);
	}
	// 名前・リテラル・閉じ括弧の後は，其処の改行で文を終える言語（Go・Swift）だけが読みを変える事の返戻
	return !Reading.HasRestrictedLineWords;
}

/**
 * 行末コメントを置く行末の位置の取得関数
 * 位置の後の最初の改行の行の最後のコードの後（行末の後続のコメントの前）を返す
 * 其処が文字列等の字面・JSX の要素の中なら其の終わる行へ，繋いで改行の消える隙間の中なら其の後の行へ移る（行の組み方は整形で変わる為，原文の字面の中の行末では決めない）
 * @param From 探し始める位置
 * @param Gaps 繋いで改行の消える隙間の範囲の列（始まりの順）
 * @param Following 返す位置の後に在る同じ行のコメントの格納先
 * @return 置く位置
 */
uint32_t ParsedTree::LineEndAfter(
	const uint32_t From,
	const std::span<const std::pair<uint32_t, uint32_t>> Gaps,
	std::vector<TSNode> &Following
) const {
	const std::string_view Source = CodeText();
	Following.clear();
	// 走査の上限（前へ動く移動は必ず後の改行へ進み，後ろへ動く移動はコメント毎に１度で足りる為，前後へ動く移動が循環しても止まる）
	const size_t StepLimit = Gaps.size() + GetCommentSpans().size() + Source.size() - std::min<size_t>(From, Source.size()) + 2;
	size_t Steps = 0;
	for(uint32_t At = static_cast<uint32_t>(std::min(Source.find('\n', From), Source.size()));;) {
		// 上限に達した位置の返戻
		if(++Steps > StepLimit) return At;
		// 繋いで消える隙間（始まりの順に並べた物）の改行は行末でない為，繋いだ次の節点の行の行末への移動
		if(
			const std::span<const std::pair<uint32_t, uint32_t>>::iterator Gap = std::upper_bound(
				Gaps.begin(),
				Gaps.end(),
				At,
				[](const uint32_t Value, const std::pair<uint32_t, uint32_t> &Range) -> bool {
					// 隙間の始まりとの比較の返戻
					return Value < Range.first;
				}
			); Gap != Gaps.begin() && At < (Gap - 1)->second
		) {
			Following.clear();
			At = static_cast<uint32_t>(std::min(Source.find('\n', (Gap - 1)->second), Source.size()));
			continue;
		}
		At = TextEdit::SkipSpLeft(Source, At, GetReading());
		// 原稿の先頭に達した位置の返戻
		if(!At) return At;
		const TSNode Leaf = ts_node_descendant_for_byte_range(GetRoot(), At - 1, At);
		if(Is(Leaf, Core::Role::Comment)) {
			// 改行を中に含む囲みコメントの其の改行は行末でない為，其の終わる行の行末への移動（後のコードを行コメントの中へ入れない）
			if(End(Leaf) > At) {
				Following.clear();
				At = static_cast<uint32_t>(std::min(Source.find('\n', End(Leaf)), Source.size()));
				continue;
			}
			// 後続コメントの前への移動
			Following.push_back(Leaf);
			At = Start(Leaf);
			continue;
		}
		// 行末を跨ぐ最も外の文字列等の字面・JSX の要素
		TSNode Outer {};
		for(TSNode Up = Leaf; !ts_node_is_null(Up); Up = ts_node_parent(Up)) {
			if(Start(Up) < At && At < End(Up) && (Is(Up, Core::Role::LiteralInterior) || Is(Up, Core::Role::TagElement))) Outer = Up;
		}
		// 置く位置の返戻
		if(ts_node_is_null(Outer)) return At;
		// 字面・JSX の要素の中なら其の終わる行の行末への移動
		Following.clear();
		At = static_cast<uint32_t>(std::min(Source.find('\n', End(Outer)), Source.size()));
	}
}

/**
 * コメントの種類の判定の共通関数
 * 行の前に空白と囲みコメントだけが在れば行頭，後に空白とコメントだけが続いて改行（PHP は閉じタグ `?>` も）に至れば行末とする
 * 計算量：範囲の表の要素数 N に対し O(log N)（表に無いコメントだけ其の場で前後の空白を見る）
 * @param Spans 原文順のコメントの範囲の表
 * @param Start コメントの本文の始まり
 * @param End コメントの本文の終わり
 * @param OutIsAtLineStart 行頭に在るか（不要なら `nullptr`）
 * @return 原文でのコメントの種類
 */
CommentKind ParsedTree::ClassifyComment(
	const std::vector<CommentSpan> &Spans,
	const uint32_t Start,
	const uint32_t End,
	bool *const OutIsAtLineStart
) const {
	const std::string &Self = *this;
	// 表の中の対象の位置（前後の並びの状態は表が持つ）
	const std::vector<CommentSpan>::const_iterator Span = std::lower_bound(
		Spans.begin(),
		Spans.end(),
		Start,
		[](const CommentSpan &Entry, const uint32_t Pos) -> bool {
			// コメントの始まりとの比較の返戻
			return Entry.Start < Pos;
		}
	);
	// 同じ行の前が行頭か囲みコメントだけかと，後の空白とコメントを飛ばした位置
	bool IsAtLineStart;
	uint32_t Scan;
	// 表に無いコメント（根其の物等）は前後を其の場で見る
	if(Span != Spans.end() && Span->Start == Start) {
		IsAtLineStart = Span->IsLineHead;
		Scan = Span->RunEnd;
	} else {
		// 表に無いコメントの行頭
		const uint32_t Lead = TextEdit::SkipSpLeft(Self, Start, GetReading());
		IsAtLineStart = !Lead || Self[Lead - 1] == '\n';
		Scan = TextEdit::SkipSpRight(Self, End, GetReading());
	}
	// 解析用に補った改行は原文に無い為，行末に数えず其の後を見る
	const size_t CodeEnd = CodeText().size();
	while(Scan < CodeEnd && Self[Scan] == '\n' && std::binary_search(InsertedBreaks.begin(), InsertedBreaks.end(), Scan)) {
		Scan = TextEdit::SkipSpRight(CodeText(), Scan + 1, GetReading());
	}
	// 行コメントの本文を終える字面（PHP の閉じタグ `?>`）はコードの行を終える為，行末に数える
	const std::string_view Terminator = GetRules().Notation.LineCommentTerminator;
	const bool IsAtLineEnd = Scan >= CodeEnd || Self[Scan] == '\n' || Self[Scan] == '\r' ||
	!Terminator.empty() && !Self.compare(Scan, Terminator.size(), Terminator);
	// 行頭の状態を求めた呼出側への引渡（文中のコメントは種類から行頭かを引けない）
	if(OutIsAtLineStart) *OutIsAtLineStart = IsAtLineStart;
	// 種類の返戻
	return !IsAtLineEnd ? CommentKind::Inline : IsAtLineStart ? CommentKind::OwnLine : CommentKind::Trailing;
}

/**
 * 原文でのコメントの種類の判定関数
 * 行の前に空白と囲みコメントだけが在れば行頭，後に空白とコメントだけが続いて改行（PHP は閉じタグ `?>` も）に至れば行末とする
 * @param Comment コメントの節点
 * @return 原文でのコメントの種類
 */
CommentKind ParsedTree::KindOf(const TSNode Comment) const {
	// コメントの本文の範囲（表と同じ鍵で引く為，末尾の改行と誤り回復の頭の空白を除いた位置）
	const std::pair<uint32_t, uint32_t> Range = CommentRangeOf(Comment);
	// 種類の返戻
	return ClassifyComment(GetCommentSpans(), Range.first, Range.second);
}

/**
 * 空白・タブへ復号される数値文字参照の長さと其の文字の取得関数
 * 計算量：Pos 以降の長さ N に対し O(N)
 * @param Text 本文
 * @param Pos 判定する位置
 * @return Pos から始まる `&#32;` / `&#x20;` / `&#9;` 等の長さと復号した文字（該当しなければ長さ 0）
 */
std::pair<size_t, char> ParsedTree::BlankReference(const std::string_view Text, const size_t Pos) {
	// 数値文字参照でない事の返戻
	if(Text.substr(Pos, 2) != "&#") return { 0, '\0' };
	// 数字部又は基数印の位置
	size_t At = Pos + 2;
	// １６進文字参照か
	const bool IsHex = At < Text.size() && (Text[At] == 'x' || Text[At] == 'X');
	// 基数印の読み飛ばし
	if(IsHex) ++At;
	// 数字部の開始位置
	const size_t Digits = At;
	// 復号途中の符号値
	uint32_t Code = 0;
	for(; At < Text.size() && Code < 0X110000; ++At) {
		// 数値文字参照の桁の累積
		if(const char Char = Text[At]; Char >= '0' && Char <= '9') {
			// 数字の取込
			Code = (IsHex ? Code << 4 : 10 * Code) + static_cast<uint32_t>(Char - '0');
		} else if(IsHex && (Char | 0X20) >= 'a' && (Char | 0X20) <= 'f') {
			// １６進英字の取込
			Code = Code << 4 | static_cast<uint32_t>((Char | 0X20) + 10 - 'a');
		} else break;
	}
	// 空白・タブへ復号される完結した文字参照でない事の返戻
	if(At == Digits || At >= Text.size() || Text[At] != ';' || Code != ' ' && Code != '\t') return { 0, '\0' };
	// 文字参照の長さと復号した文字の返戻
	return { At - Pos + 1, static_cast<char>(Code) };
}

/**
 * 行端の扱いが TypeScript と Babel で分かれる JSX 本文かの判定関数
 * 計算量：本文長 N，行数 L に対し最悪 O(N * (L + 1))
 * @param Text 処理系が１つの本文と読む字面（前後の子の間）
 * @return 行端を動かすと何方かの表示が変わるなら true
 */
bool ParsedTree::HasDisputedJsxLineEdges(const std::string_view Text) const {
	// JSX 本文の各字句の走査
	for(size_t Pos = 0; Pos < Text.size();) {
		// 次の符号点位置
		size_t Next = Pos;
		// 現在の符号点
		uint32_t Point = 0;
		if(!TextEdit::DecodeUtf8(Text, Next, Point)) Next = Pos + 1;
		// Babel が改行と読まない行の区切りの返戻
		if(Point == 0X2028 || Point == 0X2029) return true;
		if(Point == '\n' || Point == '\r') {
			const ReadingRules &Reading = GetReading();
			// 改行の前の半角空白・タブを除いた行末の字
			size_t Left = Pos;
			while(Left && (Text[Left - 1] == ' ' || Text[Left - 1] == '\t')) --Left;
			if(Left) {
				// 行末符号点の先頭候補
				size_t Lead = Left - 1;
				while(Lead && (static_cast<unsigned char>(Text[Lead]) & 0XC0) == 0X80) --Lead;
				size_t After = Lead;
				// 改行直前の符号点
				uint32_t Before = 0;
				TextEdit::DecodeUtf8(Text, After, Before);
				// 行末に在る半角空白・タブ以外の空白又は空白の文字参照の返戻
				if(
					const size_t Amp = Before == ';' ? Text.rfind('&', Lead) : std::string_view::npos;
					Reading.IsJsxSpace(Before) || Amp != std::string_view::npos && BlankReference(Text, Amp).first == Left - Amp
				) return true;
			}
			// 改行の後の半角空白・タブを除いた行頭の字
			size_t Right = Next;
			while(Right < Text.size() && (Text[Right] == ' ' || Text[Right] == '\t')) ++Right;
			// 行頭符号点の次位置
			size_t After = Right;
			// 改行直後の符号点
			uint32_t Head = 0;
			// 行頭に在る半角空白・タブ以外の空白又は空白の文字参照の返戻
			if(TextEdit::DecodeUtf8(Text, After, Head) && Reading.IsJsxSpace(Head) || BlankReference(Text, Right).first) return true;
		}
		// 次の符号点への前進
		Pos = Next;
	}
	// 両者の読みが同じ本文の返戻
	return false;
}

/**
 * 行端の扱いが処理系で分かれる本文を持つ JSX 要素かの判定関数
 * 計算量：子の数 C，本文総長 N，本文の行数 L に対し最悪 O(C + N * (L + 1))
 * 処理系は前後の子（タグ・式・要素）の間の字面を１つの本文と読む為，子の間毎に判定する
 * 此の要素は子の並べ方を変えると何方かの処理系で表示が変わる為，整形せずに原文の並びで保つ
 * @param Container JSX の要素
 * @return 行端の扱いが分かれる本文を持つなら true
 */
bool ParsedTree::HasDisputedJsxText(const TSNode Container) const {
	const std::string_view Text = *this;
	// 次に調べる本文隙間の開始
	uint32_t Gap = ts_node_start_byte(Container);
	// 処理系差の検出結果
	bool IsDisputed = false;
	ForEachNamedChild(
		Container,
		[&](const TSNode Child) -> bool {
			// 本文と文字参照は字面で読む為，次の子へ進む事の返戻
			if(Is(Child, Core::Role::RenderedText)) return true;
			// 子直前の本文判定
			IsDisputed = HasDisputedJsxLineEdges(Text.substr(Gap, ts_node_start_byte(Child) - Gap));
			// 次の本文隙間の開始
			Gap = ts_node_end_byte(Child);
			// 分かれる本文が見付かる迄走査を続ける事の返戻
			return !IsDisputed;
		}
	);
	// 判定結果の返戻
	return IsDisputed;
}

/**
 * 解析の為に原文へ置いた印の取得関数（整形仕様 4.2）
 * @return 印の字（置いて居なければ '\0'）
 */
char ParsedTree::GetAnalysisMark() const {
	// 印の返戻
	return AnalysisMark;
}

/**
 * JSX の本文の端の空白・タブを保つ印の取得関数
 * @param Blank 半角空白又はタブ
 * @return 印の字面（私用領域の文字を選べなかった場合は文字参照）
 */
std::string_view ParsedTree::GetJsxBlankMark(const char Blank) const {
	// 半角空白又はタブの印の返戻（原文が私用領域の文字を使い切る場合は，TypeScript の読みを保つ文字参照の返戻）
	if(Blank == ' ') return JsxSpaceMark.empty() ? "&#32;" : std::string_view(JsxSpaceMark);
	// タブの印の返戻
	return JsxTabMark.empty() ? "&#9;" : std::string_view(JsxTabMark);
}

/**
 * マクロの本体の字句が呑んだ行コメントの開始位置の取得関数（整形仕様 4.2 の指令本文内のコメント）
 * tree-sitter-c はマクロの本体を行末迄の１字句として読み，本体の後の `//` もコメントの節点にせず字句へ含める
 * 文字・文字列のリテラルと，解析用の印で覆った囲みコメントの中の `//` はコメントの開始でない
 * @param Body マクロの本体の字句
 * @return 行コメントの開始位置（無ければ本体の終わり）
 */
uint32_t ParsedTree::MacroLineCommentStart(const TSNode Body) const {
	// リテラルと覆った囲みコメントを飛ばす走査状態の準備
	const uint32_t End = ts_node_end_byte(Body);
	const char Marker = GetAnalysisMark();
	char Quote = '\0';
	bool IsMaskedComment = false;
	for(uint32_t Pos = ts_node_start_byte(Body); Pos + 1 < End; ++Pos) {
		const char Char = at(Pos), Next = at(Pos + 1);
		if(IsMaskedComment && Char == '*' && Next == '/') {
			IsMaskedComment = false;
			++Pos;
		} else if(IsMaskedComment) continue;
		else if(Quote && Char == '\\') ++Pos;
		else if(Quote) Quote = Char == Quote ? '\0' : Quote;
		else if(Char == '"' || Char == '\'') Quote = Char;
		else if(Char == '/' && Marker && Next == Marker) IsMaskedComment = true;
		// 行コメントの開始位置の返戻
		else if(Char == '/' && Next == '/') return Pos;
	}
	// 行コメントを持たない事の返戻
	return End;
}

/**
 * コメントの節点と字句の隙間に残ったコメントの範囲の収集関数（整形仕様 8.1 の空行の照合）
 * 走査器が節点にせず字句の間の隙間へ残した囲み・行コメント（Kotlin の呑まれたコメント等）も，隙間の字面から拾う
 * 隙間は文法のコメントの記法の開始記号で始まる物だけとし，文字列等の内部の字面を保つ節点の中の隙間（字面の一部）は読まない
 * 計算量：節点数 V と字面の長さ N に対し O(V + N)
 * @param BodyEnd PHP の本体の終わり（其の後の地の文の中の物は，解析器がコメントと読んでも地の文の為に集めない）
 * @return 位置順の範囲（末尾の改行を除く）
 */
std::vector<std::pair<uint32_t, uint32_t>> ParsedTree::CollectCommentRanges(const uint32_t BodyEnd) const {
	// 構文木とコメント記法の準備
	std::vector<std::pair<uint32_t, uint32_t>> Ranges;
	// 構文木を持たない字面の返戻
	if(!IsParsed()) return Ranges;
	const std::string_view Text = *this;
	const std::vector<Core::AvailableNotation> &Notations = GetRules().Notation.AvailableNotations;
	uint32_t PrevEnd = 0;
	// 前の字句との隙間の中のコメント（空白の外の字面が文法のコメントの記法の開始記号で始まる物）
	const auto AddGap = [&Text, &Notations, &PrevEnd, &Ranges](const uint32_t Start) -> void {
		if(
			const size_t First = Text.find_first_not_of(" \t\r\n", PrevEnd); First < Start && std::ranges::any_of(
				Notations,
				[Rest = Text.substr(First)](const Core::AvailableNotation &Available) -> bool {
					const std::string_view Opener = Core::CommentRepository::OpenerOf(Available.Notation);
					// 開始記号で始まるかの返戻
					return !Opener.empty() && Rest.starts_with(Opener);
				}
			)
		) Ranges.emplace_back(static_cast<uint32_t>(First), static_cast<uint32_t>(Text.find_last_not_of(" \t\r\n", Start - 1) + 1));
	};
	// 辿って居る祖先毎の内部の字面を保つ節点か（其の子の間の隙間は字面の一部でコメントでない）
	std::vector<uint8_t> IsLiteral;
	// 字句とコメントを原文順に辿り，子を辿り終えた節点の終わり迄も処理済とする（子が覆わない閉じの引用符等は記号で始まらない為に数えない）
	TSTreeCursor Cursor = ts_tree_cursor_new(GetRoot());
	for(bool IsGoing = true; IsGoing;) {
		const TSNode Node = ts_tree_cursor_current_node(&Cursor);
		// 本体の後の地の文に達した事の打切（原文順に辿る為，以降の節点も地の文の中）
		if(ts_node_start_byte(Node) >= BodyEnd) break;
		const bool IsComment = Is(Node, Core::Role::Comment);
		// 字句でもコメントでもない節点の子を辿る
		if(!IsComment && ts_node_child_count(Node) && ts_tree_cursor_goto_first_child(&Cursor)) {
			IsLiteral.push_back(Is(Node, Core::Role::LiteralInterior));
			continue;
		}
		if(const uint32_t Start = ts_node_start_byte(Node); Start > PrevEnd && !(!IsLiteral.empty() && IsLiteral.back())) AddGap(Start);
		// コメントの本文の範囲（末尾の改行を除き，幅０の節点は直前の本文を回収する）
		if(IsComment) if(const auto [Begin, End] = CommentRangeOf(Node); Begin < End) Ranges.emplace_back(Begin, End);
		// マクロの本体の字句が呑んだ行コメントの範囲（本体の終わりは論理行の終わり）
		if(Is(Node, Core::Role::MacroBody)) if(const uint32_t Begin = MacroLineCommentStart(Node); Begin < ts_node_end_byte(Node)) {
			Ranges.emplace_back(Begin, static_cast<uint32_t>(Text.find_last_not_of(" \t\r\n", ts_node_end_byte(Node) - 1) + 1));
		}
		PrevEnd = std::max(PrevEnd, ts_node_end_byte(Node));
		// 次の兄弟か，辿り終えた親の終わり迄の処理済の更新（最後の子の後に親が覆う隙間のコメントも拾う）
		while(!ts_tree_cursor_goto_next_sibling(&Cursor)) {
			if(!ts_tree_cursor_goto_parent(&Cursor)) {
				IsGoing = false;
				break;
			}
			const bool IsLiteralParent = IsLiteral.back();
			IsLiteral.pop_back();
			if(const uint32_t ParentEnd = std::min(ts_node_end_byte(ts_tree_cursor_current_node(&Cursor)), BodyEnd); ParentEnd > PrevEnd) {
				if(!IsLiteralParent) AddGap(ParentEnd);
				PrevEnd = ParentEnd;
			}
		}
	}
	ts_tree_cursor_delete(&Cursor);
	// 根の末尾に残った隙間の収集
	if(BodyEnd > PrevEnd) AddGap(BodyEnd);
	// 幅０の節点の回収で前後する為の始まりでの整列
	std::sort(Ranges.begin(), Ranges.end());
	// 範囲の返戻
	return Ranges;
}

/**
 * 独立したコードとして整形した埋込の出力の取得関数（整形仕様 4.4）
 * @param RawText 埋込の中身の節点
 * @return 整形した出力（整形しなかった埋込と埋込の中身でない節点は nullptr）
 */
std::shared_ptr<const Core::OwnedRegion> ParsedTree::OwnedRegionOf(const TSNode RawText) const {
	const std::unordered_map<const void *, std::shared_ptr<const Core::OwnedRegion>>::const_iterator Found =
	OwnedRegions.find(RawText.id);
	// 整形した出力の返戻
	return Found == OwnedRegions.end() ? nullptr : Found->second;
}

/**
 * 独立したコードとして整形した埋込の出力の控え関数（整形仕様 4.4，親の原文へ書き戻さず投影が所有区画とする）
 * @param RawText 埋込の中身の節点
 * @param Region 整形した出力と其の逐語の中から始まる行
 */
void ParsedTree::SetOwnedRegion(const TSNode RawText, Core::OwnedRegion &&Region) {
	OwnedRegions.insert_or_assign(RawText.id, std::make_shared<const Core::OwnedRegion>(std::move(Region)));
	// 終了
	return;
}

/**
 * 実引数を字面の儘保つ呼出かの判定関数
 * 計算量：引数の子節点数 C，呼出名と直前の空白の長さ N に対し平均 O(C + N)
 * @param List 判定する括弧の並び（実引数列・仮引数列）
 * @return 実引数を文字列化する関数形式マクロ（CollectMacros で集めた名前）の実引数の並びか，波括弧の塊を実引数に持つ並びなら true
 */
bool ParsedTree::StringizesArguments(const TSNode List) const {
	// 括弧の並びでない場合の返戻
	if(!Is(List, Core::Role::DelimitedList)) return false;
	// 波括弧の塊を実引数に持つ場合の返戻
	if(
		HasChildOf(
			List,
			[this](const TSNode Child) -> bool {
				// 波括弧の塊かの返戻
				return Is(Child, Core::Role::CompoundBody);
			}
		)
	) return true;
	// 対象のマクロが無い場合の返戻
	if(StringizingMacros.empty()) return false;
	// 呼出名の終端候補
	uint32_t NameEnd = Start(List);
	while(NameEnd && (at(NameEnd - 1) == ' ' || at(NameEnd - 1) == '\t' || at(NameEnd - 1) == '\n')) --NameEnd;
	// 呼出名の開始候補
	uint32_t NameStart = NameEnd;
	while(NameStart && IsIdentifierChar(at(NameStart - 1))) --NameStart;
	// 直前の名前が対象のマクロかの返戻
	return NameStart < NameEnd && StringizingMacros.contains(substr(NameStart, NameEnd - NameStart));
}

/**
 * 直前のコメントを取り込む Go の最上位の宣言かの判定関数（整形仕様 7.6 の Go の区切コメント）
 * godoc は直前のコメントを宣言の文書に，cgo は `import "C"` の直前のコメントを前文に読む
 * @param Node 判定する節点
 * @return 文書を持つ宣言か `import "C"` なら true
 */
bool ParsedTree::AbsorbsGoComment(const TSNode Node) const {
	// 文書を持つ宣言の返戻
	if(Is(Node, Core::Role::DocumentedTopDeclaration)) return true;
	// 取込の宣言でない事の返戻
	if(!Is(Node, Core::Role::ImportDeclarationNode)) return false;
	const TSNode Spec = ts_node_named_child(Node, 0);
	const TSNode Path = ts_node_is_null(Spec) ? TSNode{} : FieldChild(Spec, Core::ChildRole::Path);
	const std::string_view ForeignPackage = GetReading().ImportedForeignPackage;
	// 取り込むパッケージが cgo の `"C"` かの返戻
	return !ForeignPackage.empty() && !ts_node_is_null(Path) && View(Path) == ForeignPackage;
}

/**
 * 本文差替と未反映状態の共通更新関数
 * @param NewSource 新しいソース文字列（右辺値なら所有権を移す）
 */
template<typename String> void ParsedTree::AssignSource(String &&NewSource) {
	// 本文が同一の場合の返戻
	if(NewSource == static_cast<const std::string &>(*this)) return;
	// 本文の差替
	const size_t DataLength = HaltedData == std::string::npos ? 0 : size() - HaltedData;
	std::string::operator=(std::forward<String>(NewSource));
	// 本文差替後の位置と解析状態の更新
	if(HaltedData != std::string::npos) HaltedData = size() - DataLength;
	if(Syntax.Tree) Syntax.IsDirty = true;
	// 本文と共に古く為るコメントの位置表の破棄
	Syntax.CommentSpans.clear();
	Syntax.HasCommentSpans = false;
	// 終了
	return;
}

/**
 * ソース移動代入関数
 * @param NewSource 新しいソース文字列（ムーブ済）
 */
void ParsedTree::Assign(std::string &&NewSource) {
	// 移動本文の共通代入処理
	AssignSource(std::move(NewSource));
	// 解析用の挿入・置換位置と補修の編集・退避したコメントは，本文を全面差替えした後の字句の由来を表さない
	InsertedBreaks.clear();
	SyntheticGroups.clear();
	LexemeRestorations.clear();
	EditBatches.clear();
	EscapedComments.clear();
	OwnedRegions.clear();
	// 終了
	return;
}

/**
 * 補修の編集を適用した本文の差替関数
 * 本文を全面差替えした時と同じく古い位置の記録を捨て，編集後の位置へ移した解析用の区切・囲み・字句と編集の組を残す
 * @param NewSource 編集を適用した本文
 * @param Breaks 編集後の本文に対応する解析用の区切の位置
 * @param Groups 編集後の本文に対応する解析用の囲みの位置の組
 * @param Restorations 編集後の本文に対応する字句の復号資料
 * @param Batch 適用した編集の組（位置順）
 */
void ParsedTree::AssignEdited(
	std::string &&NewSource,
	std::vector<uint32_t> &&Breaks,
	std::vector<std::pair<uint32_t, uint32_t>> &&Groups,
	std::vector<LexemeRestoration> &&Restorations,
	std::vector<AppliedEdit> &&Batch
) {
	// 旧編集履歴と退避コメントの保存
	std::vector<std::vector<AppliedEdit>> Batches = std::move(EditBatches);
	std::vector<EscapedComment> Escaped = std::move(EscapedComments);
	Assign(std::move(NewSource));
	InsertedBreaks = std::move(Breaks);
	SyntheticGroups = std::move(Groups);
	// 退避したコメントの位置の編集後の位置への移動（置き換えた範囲の中の物は置き換えた字面の始まり）
	for(EscapedComment &Comment : Escaped) {
		const std::vector<AppliedEdit>::const_iterator After = std::upper_bound(
			Batch.begin(),
			Batch.end(),
			Comment.Position,
			[](const uint32_t Position, const AppliedEdit &Edit) -> bool {
				// 適用前の始まりとの比較の返戻
				return Position < Edit.Start;
			}
		);
		if(After == Batch.begin()) continue;
		const AppliedEdit &Edit = *std::prev(After);
		Comment.Position = Comment.Position < Edit.End ? Edit.NewStart : Comment.Position - Edit.End + Edit.NewEnd;
	}
	// 編集後の控えと履歴の確定
	EscapedComments = std::move(Escaped);
	LexemeRestorations = std::move(Restorations);
	EditBatches = std::move(Batches);
	EditBatches.push_back(std::move(Batch));
	// 終了
	return;
}

/**
 * 補修前の本文の位置の取得関数（整形仕様 3.3 の診断位置）
 * 補修の編集の組を適用の逆順に辿り，置き換えた字面の中の位置は置き換えた範囲の始まりへ戻す
 * 計算量：編集の組の数 B と組毎の編集の数 E に対し O(B log E)
 * @param Offset 現在の本文の位置
 * @return 補修を加える前の本文の位置
 */
uint32_t ParsedTree::OffsetBeforeEdits(uint32_t Offset) const {
	for(
		std::vector<std::vector<AppliedEdit>>::const_reverse_iterator Batch = EditBatches.rbegin();
		Batch != EditBatches.rend();
		++Batch
	) {
		// 位置より前から始まる最後の編集
		const std::vector<AppliedEdit>::const_iterator After = std::upper_bound(
			Batch->begin(),
			Batch->end(),
			Offset,
			[](const uint32_t Position, const AppliedEdit &Edit) -> bool {
				// 編集後の始まりとの比較の返戻
				return Position < Edit.NewStart;
			}
		);
		if(After == Batch->begin()) continue;
		const AppliedEdit &Edit = *std::prev(After);
		Offset = Offset < Edit.NewEnd ? Edit.Start : Offset - Edit.NewEnd + Edit.End;
	}
	// 補修前の本文の位置の返戻
	return Offset;
}

/**
 * 解析用に補った文の区切の位置の取得関数
 * @return 現在の本文に対応する位置の並び
 */
const std::vector<uint32_t> &ParsedTree::GetInsertedBreaks() const {
	// 解析用の位置の返戻
	return InsertedBreaks;
}

/**
 * 解析用に挿入した囲みの位置の組の取得関数
 * @return 現在の本文に対応する開きと閉じの位置の組
 */
const std::vector<std::pair<uint32_t, uint32_t>> &ParsedTree::GetSyntheticGroups() const {
	// 解析用の括弧の位置の組の返戻
	return SyntheticGroups;
}

/**
 * 解析用に置き換えた字句の復号資料の取得関数
 * @return 現在の本文に対応する置換後の範囲と原文の字面
 */
const std::vector<LexemeRestoration> &ParsedTree::GetLexemeRestorations() const {
	// 字句の復号資料の返戻
	return LexemeRestorations;
}

/**
 * 範囲に対応する字句の復号資料の取得関数
 * 計算量：復号資料の数 R に対し O(log R)
 * @param Start 解析用の本文に於ける字句の始まり
 * @param End 解析用の本文に於ける字句の終わり
 * @return 同じ範囲の字句の復号資料（無ければ nullptr）
 */
const LexemeRestoration *ParsedTree::RestorationAt(const uint32_t Start, const uint32_t End) const {
	const std::vector<LexemeRestoration>::const_iterator Found =
	std::ranges::lower_bound(LexemeRestorations, Start, std::less<uint32_t>(), &LexemeRestoration::Start);
	// 同じ範囲の復号資料の返戻
	return Found != LexemeRestorations.end() && Found->Start == Start && Found->End == End ? &*Found : nullptr;
}

/**
 * 解析用に置き換えた字句の記録関数
 * @param Start 置換後の字句の始まり
 * @param End 置換後の字句の終わり
 * @param Original 原文の字面
 */
void ParsedTree::AddLexemeRestoration(const uint32_t Start, const uint32_t End, std::string Original) {
	const std::vector<LexemeRestoration>::const_iterator Position =
	std::ranges::upper_bound(LexemeRestorations, Start, std::less<uint32_t>(), &LexemeRestoration::Start);
	LexemeRestorations.insert(Position, { Start, End, std::move(Original) });
	// 終了
	return;
}

/** ========== 構文情報収集 ========== */
/**
 * 宣言した値と型の名前と取込の指令の位置の収集関数 (C / C++)
 * 計算量：原稿長 N，走査節点数 V，収集する名前の総長 B，型宣言数 M に対し平均 O(N + V + B + M log(M + 1))
 * 位置は照会する時の構文木の位置と比べる為 (IsDeclaredName)，コメントの除去や編集の後の構文木から照会の直前に集め直す
 * （コメントを除く前に集めると，コメントの長さの分だけ宣言・取込と使う位置の前後が入れ替わり，コメントの有無で括弧の要否が変わる）
 */
void ParsedTree::CollectDeclaredNames() {
	// 前回の宣言索引破棄
	DeclaredNames.clear();
	DeclaredTypeNames.clear();
	IncludeStarts.clear();
	BoolNamesStart = UINT32_MAX;
	// 構文木が無い場合の返戻
	if(!IsParsed()) return;
	const ReadingRules &Reading = GetReading();
	// 前処理指令の有無の事前判定
	const bool HasDirective = find('#') != npos;
	// 前処理条件の終端を外側から積み，定義・取消・条件で使うマクロ名を記録
	std::vector<uint32_t> ConditionEnds;
	std::unordered_set<std::string_view> MacroNames;
	// 宣言した名前の位置との記録
	// 宣言した名前が見える最も内側の範囲の終端（仮引数は関数の定義又は宣言の終わり，其の他は本体・名前空間・クラスの終わり，無ければ原稿の終わり）
	const auto ScopeEndOf = [&](const TSNode Owner) -> uint32_t {
		const bool IsParameter = Is(Owner, Core::Role::ParameterDeclaration);
		for(TSNode Scope = ts_node_parent(Owner); !ts_node_is_null(Scope); Scope = ts_node_parent(Scope)) if(
			Is(Scope, Core::Role::CompoundBody) || Is(Scope, Core::Role::NamespaceDefinition) || Is(Scope, Core::Role::ClassLike) ||
			IsParameter && (Is(Scope, Core::Role::FunctionDefinition) || Is(Scope, Core::Role::PotentialExpressionDeclaration))
		// 名前の見える範囲の終わりの返戻
		) return ts_node_end_byte(Scope);
		// 原稿の終わりの返戻
		return ts_node_end_byte(GetRoot());
	};
	// 宣言子を指定の名前迄解包した節点（無ければ空）
	const auto NameOf = [&](TSNode Declarator, const Core::Role Wanted) -> TSNode {
		while(!ts_node_is_null(Declarator) && !Is(Declarator, Wanted)) Declarator = FieldChild(Declarator, Core::ChildRole::Declarator);
		// 名前の節点の返戻
		return Declarator;
	};
	// 宣言した名前の位置と有効範囲の終端の記録
	const auto Declare = [&](decltype(DeclaredNames) &Names, const TSNode Name, const TSNode Owner) -> void {
		if(!ts_node_is_null(Name)) Names[std::string(View(Name))].emplace_back(ts_node_start_byte(Name), ScopeEndOf(Owner));
	};
	WalkChildrenCursor(
		GetRoot(),
		[&](const TSNode Node) -> bool {
			// 宣言名収集対象の節点種別と位置の取得
			const uint32_t Start = ts_node_start_byte(Node);
			// 現在位置より前に終わった条件範囲の除去
			while(!ConditionEnds.empty() && ConditionEnds.back() <= Start) ConditionEnds.pop_back();
			const bool IsOutsideCondition = ConditionEnds.empty();
			if(IsOutsideCondition && Is(Node, Core::Role::BareTypeDeclaration)) {
				if(Is(Node, Core::Role::TypeDefinition)) {
					const TSNode Type = FieldChild(Node, Core::ChildRole::Type);
					ForEachNamedChild(
						Node,
						[&](const TSNode Declarator) -> void {
							// 型の子を除き，宣言子を型名迄解包して記録
							if(!ts_node_eq(Declarator, Type)) Declare(DeclaredTypeNames, NameOf(Declarator, Core::Role::TypeName), Node);
						}
					);
				} else if(const TSNode Name = FieldChild(Node, Core::ChildRole::Name); Is(Name, Core::Role::TypeName)) {
					Declare(DeclaredTypeNames, Name, Node);
				}
			}
			// 宣言位置でマクロでない名前を収集（前処理条件内はマクロ時に飛ばす枝の可能性が有る為に除外）
			if(IsOutsideCondition && Is(Node, Core::Role::EnumerationItem)) {
				Declare(DeclaredNames, FieldChild(Node, Core::ChildRole::Name), Node);
			} else if(IsOutsideCondition && Is(Node, Core::Role::NameDeclaration) && !IsAmbiguousDeclaration(Node)) {
				// 宣言子を識別子迄解包して宣言名を収集
				ForEachNamedChild(
					Node,
					[&](const TSNode Declarator) -> void {
						// 型の子を除き，ネストの宣言子を名前迄解包して記録
						Declare(DeclaredNames, NameOf(Declarator, Core::Role::Identifier), Node);
					}
				);
			}
			// 前処理の指令を持たないソースは子孫走査を続ける事の返戻
			if(!HasDirective) return !Is(Node, Core::Role::SourceLeaf);
			// 条件範囲の終端追加
			if(Is(Node, Core::Role::PreprocConditional)) ConditionEnds.push_back(ts_node_end_byte(Node));
			// 条件指令で問う名前もファイル内のマクロ名として記録
			if(Is(Node, Core::Role::MacroNameQuery)) {
				if(const TSNode Name = FirstNamedChild(Node, Core::Role::Identifier); !ts_node_is_null(Name)) MacroNames.insert(View(Name));
			} else if(Is(Node, Core::Role::PreprocessorInclude)) {
				IncludeStarts.push_back(Start);
				// 条件の外で `true` / `false` を宣言する標準の取込
				if(
					!Reading.BooleanNameHeader.empty() && IsOutsideCondition &&
					View(FieldChild(Node, Core::ChildRole::Path)) == Reading.BooleanNameHeader
				) BoolNamesStart = std::min(BoolNamesStart, ts_node_end_byte(Node));
			} else if(
				!Reading.UndefinitionDirective.empty() && Is(Node, Core::Role::PreprocessorCall) &&
				View(FieldChild(Node, Core::ChildRole::DirectiveField)) == Reading.UndefinitionDirective
			) {
				std::string_view Argument = View(FieldChild(Node, Core::ChildRole::ArgumentField));
				TextEdit::TrimView(Argument);
				MacroNames.insert(Argument);
			}
			// マクロの定義以外の子孫走査を続ける事の返戻
			if(!Is(Node, Core::Role::MacroDefinition)) return !Is(Node, Core::Role::SourceLeaf);
			// 定義マクロ名の記録
			if(const TSNode Name = FieldChild(Node, Core::ChildRole::Name); !ts_node_is_null(Name)) MacroNames.insert(View(Name));
			// 定義の内側へ降りない事の返戻
			return false;
		}
	);
	// 型の別名を外側の宣言から先に採った場合も宣言位置で並べる
	for(decltype(DeclaredTypeNames)::value_type &Entry : DeclaredTypeNames) std::ranges::sort(Entry.second);
	// マクロ名と同名の宣言の除外
	for(const std::string_view Name : MacroNames) {
		const std::string Spelling(Name);
		DeclaredNames.erase(Spelling);
		DeclaredTypeNames.erase(Spelling);
	}
	// 終了
	return;
}

/**
 * 前処理マクロの性質（実引数の文字列化・文への展開）の収集関数 (C / C++)
 * 計算量：原稿長 N，節点数 V，定義数 M，単一定義の判定の最大費用 T に対し平均 O(N + V + M * M * T)
 */
void ParsedTree::CollectMacros() {
	// 前回の文字列化性質破棄
	StringizingMacros.clear();
	StatementMacros.clear();
	DeclaratorMacros.clear();
	ExpressionMacros.clear();
	FunctionMacros.clear();
	// 標準表明マクロの文字列化性質登録
	for(const std::string_view Name : GetRules().Words[static_cast<size_t>(Core::Word::StringizingAssertion)]) {
		if(find(Name) != npos) StringizingMacros.emplace(Name);
	}
	// マクロの定義を持ち得ない（`#` の無い）ソースと解析出来ないソースの終了
	if(!IsParsed() || find('#') == npos) return;
	// マクロの定義（名前・仮引数名・置換本体，オブジェクト形式は仮引数を持たない）
	struct Definition {
		std::string_view Name;
		std::vector<std::string_view> Parameters;
		std::string_view Body;
		bool IsFunctionLike;
	};
	// 原文中のマクロ定義列
	std::vector<Definition> Definitions;
	WalkChildrenCursor(
		GetRoot(),
		[&](const TSNode Node) -> bool {
			// マクロ定義節点の選別
			// マクロの定義以外の子孫走査を続ける事の返戻
			if(!Is(Node, Core::Role::MacroDefinition)) return !Is(Node, Core::Role::SourceLeaf);
			// 定義の各場
			const TSNode Name = FieldChild(Node, Core::ChildRole::Name), Parameters = FieldChild(Node, Core::ChildRole::ParametersField);
			const TSNode Value = FieldChild(Node, Core::ChildRole::Value);
			const bool IsFunctionLike = Is(Node, Core::Role::PreprocessorFunctionDefinition);
			// 置換本体の無い定義の内側へ降りない事の返戻
			if(ts_node_is_null(Name) || IsFunctionLike && ts_node_is_null(Parameters) || ts_node_is_null(Value)) return false;
			// 有効な定義の名前・仮引数・本体の収集
			Definition Entry { View(Name), {}, View(Value), IsFunctionLike };
			if(IsFunctionLike) {
				// 可変長仮引数と明示仮引数の登録
				Entry.Parameters.push_back(GetReading().VariadicParameter);
				ForEachNamedChild(
					Parameters,
					[&](const TSNode Parameter) -> void {
						// 関数形式マクロの仮引数名の収集
						if(Is(Parameter, Core::Role::Identifier)) Entry.Parameters.push_back(View(Parameter));
					}
				);
			}
			Definitions.push_back(std::move(Entry));
			// 定義の内側へ降りない事の返戻
			return false;
		}
	);
	// 置換本体の Pos から始まる識別子の終わり
	const auto IdentifierEnd = [](const std::string_view Body, size_t Pos) -> size_t {
		// 識別子の字の読飛
		while(Pos < Body.size() && IsIdentifierChar(Body[Pos])) ++Pos;
		// 識別子の終わりの返戻
		return Pos;
	};
	// 置換本体が仮引数の字面を文字列にするか（`#` の後の仮引数，又は文字列化するマクロの実引数の中の仮引数）
	const auto Stringizes = [&](const Definition &Entry) -> bool {
		// 文字列化判定対象のマクロ定義の取得
		const auto IsParameter = [&Entry](const std::string_view Word) -> bool {
			// 仮引数名かの返戻
			return std::find(Entry.Parameters.begin(), Entry.Parameters.end(), Word) != Entry.Parameters.end();
		};
		// 置換本体の字句走査準備
		const std::string_view Body = Entry.Body;
		// 直接又は別マクロ経由の文字列化箇所探索
		for(size_t Pos = 0; Pos < Body.size();) {
			if(Body[Pos] == '#') {
				// `##` の連結は文字列化でない
				if(Pos + 1 < Body.size() && Body[Pos + 1] == '#') {
					Pos += 2;
					continue;
				}
				// 文字列化演算子後の仮引数位置探索
				size_t Start = Pos + 1;
				while(Start < Body.size() && (Body[Start] == ' ' || Body[Start] == '\t')) ++Start;
				// `#` の後の仮引数の返戻
				if(IsParameter(Body.substr(Start, IdentifierEnd(Body, Start) - Start))) return true;
				Pos = Start;
				continue;
			}
			// 非識別子を飛ばして呼出候補へ移動
			if(!IsIdentifierChar(Body[Pos])) {
				++Pos;
				continue;
			}
			// 文字列化マクロ名との照合
			const size_t End = IdentifierEnd(Body, Pos);
			const bool IsCallee = StringizingMacros.contains(std::string(Body.substr(Pos, End - Pos)));
			Pos = End;
			if(!IsCallee) continue;
			// 呼出の開き括弧迄の空白読飛
			size_t Open = Pos;
			while(Open < Body.size() && (Body[Open] == ' ' || Body[Open] == '\t')) ++Open;
			// 関数形式の呼出である事の確認
			if(Open >= Body.size() || Body[Open] != '(') continue;
			// 文字列化するマクロの実引数の中の仮引数を探す
			size_t Depth = 0;
			// 括弧深さを追跡した実引数走査
			for(size_t Inner = Open; Inner < Body.size(); ++Inner) {
				if(Body[Inner] == '(') ++Depth;
				else if(Body[Inner] == ')' && !--Depth) break;
				else if(IsIdentifierChar(Body[Inner]) && (Inner == Open || !IsIdentifierChar(Body[Inner - 1]))) {
					// 文字列化する実引数内の仮引数発見の返戻
					if(IsParameter(Body.substr(Inner, IdentifierEnd(Body, Inner) - Inner))) return true;
				}
			}
		}
		// 仮引数を文字列にしない事の返戻
		return false;
	};
	// 名前から置換本体の定義を逆引きし，性質の増えた名前を含む定義だけ再判定
	std::unordered_map<std::string_view, std::vector<size_t>> MentionedBy;
	// 各定義の参照名から利用定義への逆索引構築
	for(size_t Index = 0; Index < Definitions.size(); ++Index) {
		// マクロ定義から参照先と被参照元の索引構築
		const std::string_view Body = Definitions[Index].Body;
		for(size_t Pos = 0; Pos < Body.size();) {
			if(!IsIdentifierChar(Body[Pos])) {
				++Pos;
				continue;
			}
			const size_t End = IdentifierEnd(Body, Pos);
			if(std::vector<size_t> &Users = MentionedBy[Body.substr(Pos, End - Pos)]; Users.empty() || Users.back() != Index) {
				Users.push_back(Index);
			}
			Pos = End;
		}
	}
	// 性質が増えた名前を含む定義の再判定待ちへの重複無の追加
	std::vector<size_t> Pending;
	// 定義別の待機重複防止
	std::vector<uint8_t> Queued(Definitions.size(), 0);
	const auto Requeue = [&MentionedBy, &Pending, &Queued](const std::string_view Name) -> void {
		// 性質の再判定対象の待ち行列登録
		if(
			const std::unordered_map<std::string_view, std::vector<size_t>>::const_iterator Users = MentionedBy.find(Name);
			Users != MentionedBy.end()
		) for(const size_t Index : Users->second) {
			// 未登録の利用定義だけを待機列へ追加
			if(Queued[Index]) continue;
			Queued[Index] = 1;
			Pending.push_back(Index);
		}
	};
	// 全ての定義を１度判定し，性質の加わった名前を含む定義を判定し直す事を，判定し直す物が無くなる迄繰り返す
	const auto Converge = [&Definitions, &Pending, &Queued](const auto &Judge) -> void {
		// マクロ性質の収束計算
		for(size_t Index = 0; Index < Definitions.size(); ++Index) Judge(Definitions[Index]);
		// 性質変化の影響を受けた定義の再判定
		while(!Pending.empty()) {
			const size_t Index = Pending.back();
			Pending.pop_back();
			Queued[Index] = 0;
			Judge(Definitions[Index]);
		}
	};
	// 文字列化マクロへの仮引数転送とオブジェクト形式別名の性質伝播
	Converge(
		[&](const Definition &Entry) -> void {
			// 判定済の文字列化マクロの場合の終了
			if(StringizingMacros.contains(std::string(Entry.Name))) return;
			// オブジェクト形式の別名候補抽出
			std::string_view Alias = Entry.Body;
			TextEdit::TrimView(Alias);
			// 文字列化しないマクロの返戻
			if(Entry.IsFunctionLike ? !Stringizes(Entry) : !StringizingMacros.contains(std::string(Alias))) return;
			// 文字列化性質の登録と依存定義の再判定予約
			StringizingMacros.emplace(Entry.Name);
			Requeue(Entry.Name);
		}
	);
	// 文の語又は文字列外の文区切りを持つ置換本体の文展開判定
	const std::vector<std::string_view> &StatementWords = GetReading().StatementWords;
	const auto ExpandsToStatementBody = [&IdentifierEnd, &StatementWords](const std::string_view Body) -> bool {
		// 文へ展開する置換本体の判定
		size_t Lead = 0;
		while(Lead < Body.size() && (Body[Lead] == ' ' || Body[Lead] == '\t')) ++Lead;
		// 文の語で始まる事の返戻
		if(std::ranges::find(StatementWords, Body.substr(Lead, IdentifierEnd(Body, Lead) - Lead)) != StatementWords.end()) return true;
		// 文字列外の文区切り又は塊開始の探索
		for(size_t Pos = 0; Pos < Body.size(); ++Pos) {
			if(const char Char = Body[Pos]; Char == '"' || Char == '\'') {
				// 引用文字列内の区切り記号の読飛
				while(++Pos < Body.size() && Body[Pos] != Char) if(Body[Pos] == '\\') ++Pos;
			// 文の区切り・塊を含む事の返戻
			} else if(Char == ';' || Char == '{') return true;
		}
		// 式だけに展開する事の返戻
		return false;
	};
	// 置換本体の外側の括弧の外に演算子を持つマクロは，周りの括弧で結合を守る
	const auto ExposesOperator = [](std::string_view Body) -> bool {
		// 外側の括弧で守られない演算子の探索
		TextEdit::TrimView(Body);
		int Depth = 0;
		// 引用文字列と括弧深さを考慮した置換本体走査
		for(size_t Pos = 0; Pos < Body.size(); ++Pos) switch(const char Char = Body[Pos]; Char) {
		case '"':
		case '\'':
			// 引用文字列内の演算子の読飛
			while(++Pos < Body.size() && Body[Pos] != Char) if(Body[Pos] == '\\') ++Pos;
			break;
		case '(':
		case '[':
			// 括弧内へ入る深さの加算
			++Depth;
			break;
		case ')':
		case ']':
			--Depth;
			break;
		default:
			// 括弧の外に演算子が在る事の返戻
			if(!Depth && std::string_view("+-*/%<>=&|^!?:,~").find(Char) != std::string_view::npos) return true;
		}
		// 演算子を括弧の中にだけ持つ事の返戻
		return false;
	};
	for(const Definition &Entry : Definitions) {
		// 確定したマクロ性質の集合への登録
		if(Entry.IsFunctionLike) FunctionMacros.emplace(Entry.Name);
		if(ExpandsToStatementBody(Entry.Body)) StatementMacros.emplace(Entry.Name);
		// 宣言子候補名の登録
		if(Entry.Body.find_first_of("*&[(") != std::string_view::npos) DeclaratorMacros.emplace(Entry.Name);
		if(ExposesOperator(Entry.Body)) ExpressionMacros.emplace(Entry.Name);
	}
	// 置換本体の外側の括弧の外に名前の何れかを含むか（括弧の中の名前は展開後も其の括弧が結合を守る）
	const auto MentionsOutsideParens =
	[&IdentifierEnd](const std::string_view Body, const std::unordered_set<std::string> &Names) -> bool {
		// 括弧外で参照する名前の探索
		int Depth = 0;
		// 引用文字列と括弧深さを考慮した識別子走査
		for(size_t Pos = 0; Pos < Body.size();) switch(const char Char = Body[Pos]; Char) {
		case '"':
		case '\'':
			// 引用文字列内の名前の読飛
			while(++Pos < Body.size() && Body[Pos] != Char) if(Body[Pos] == '\\') ++Pos;
			++Pos;
			break;
		case '(':
		case '[':
			// 括弧内へ入る深さの加算
			++Depth;
			++Pos;
			break;
		case ')':
		case ']':
			// 括弧外へ戻る深さの減算
			--Depth;
			++Pos;
			break;
		default:
			if(IsIdentifierChar(Char)) {
				// 識別子終端と括弧外での集合照合
				const size_t End = IdentifierEnd(Body, Pos);
				// 括弧の外の名前の返戻
				if(!Depth && Names.contains(std::string(Body.substr(Pos, End - Pos)))) return true;
				Pos = End;
			} else ++Pos;
		}
		// 括弧の外に名前を含まない事の返戻
		return false;
	};
	// 置換先の性質を別名・転送へ伝播し，関数別名も実引数の逐語展開を継承
	Converge(
		[&](const Definition &Entry) -> void {
			// 文・宣言子・式へ展開するマクロの判定
			const std::string Name(Entry.Name);
			// オブジェクト形式又は関数形式の別名候補抽出
			std::string_view Alias = Entry.Body;
			TextEdit::TrimView(Alias);
			// 未登録の各展開性質の判定
			const bool IsFunctionAlias = !FunctionMacros.contains(Name) && FunctionMacros.contains(Alias);
			const bool IsStatement = !StatementMacros.contains(Name) && MentionsName(Entry.Body, StatementMacros);
			const bool IsDeclarator = !DeclaratorMacros.contains(Name) && MentionsName(Entry.Body, DeclaratorMacros);
			const bool IsExpression = !ExpressionMacros.contains(Name) && MentionsOutsideParens(Entry.Body, ExpressionMacros);
			// 新たに判明した性質の集合登録
			if(IsFunctionAlias) FunctionMacros.insert(Name);
			if(IsStatement) StatementMacros.insert(Name);
			if(IsDeclarator) DeclaratorMacros.insert(Name);
			if(IsExpression) ExpressionMacros.insert(Name);
			// 性質変化を参照する定義の再判定予約
			if(IsFunctionAlias || IsStatement || IsDeclarator || IsExpression) Requeue(Entry.Name);
		}
	);
	// 終了
	return;
}

/**
 * 適応済の本文と解析木の共有写しの構築関数
 * @param Other 写す入力
 * @throws std::runtime_error 解析器の初期化に失敗した場合
 */
ParsedTree::ParsedTree(const ParsedTree &Other) : std::string(Other),
Grammar(Other.Grammar),
HaltedData(Other.HaltedData),
StringizingMacros(Other.StringizingMacros),
StatementMacros(Other.StatementMacros),
DeclaratorMacros(Other.DeclaratorMacros),
ExpressionMacros(Other.ExpressionMacros),
FunctionMacros(Other.FunctionMacros),
DeclaredNames(Other.DeclaredNames),
DeclaredTypeNames(Other.DeclaredTypeNames),
IncludeStarts(Other.IncludeStarts),
InsertedBreaks(Other.InsertedBreaks),
SyntheticGroups(Other.SyntheticGroups),
LexemeRestorations(Other.LexemeRestorations),
EditBatches(Other.EditBatches),
EscapedComments(Other.EscapedComments),
BoolNamesStart(Other.BoolNamesStart),
AnalysisMark(Other.AnalysisMark),
JsxSpaceMark(Other.JsxSpaceMark),
JsxTabMark(Other.JsxTabMark),
OwnedRegions(Other.OwnedRegions),
Decision(Other.Decision),
Failure(Other.Failure),
HasInputError(Other.HasInputError) {
	// 構文木と解析状態の写し
	Syntax.Tree.reset(Other.Syntax.Tree ? ts_tree_copy(Other.Syntax.Tree.get()) : nullptr);
	Syntax.IsParseExpired = Other.Syntax.IsParseExpired;
	Syntax.IsDirty = Other.Syntax.IsDirty;
	Syntax.IsTreeEdited = Other.Syntax.IsTreeEdited;
	if(Syntax.IsDirty) {
		Syntax.Parser.emplace();
		if(!Syntax.Parser->Use(Grammar->GetLanguage().TsLang())) throw std::runtime_error("parser initialization failed");
		Reparse();
	}
	// 木の写しと本文を同じ世代に揃えた事の終了
	return;
}

/**
 * 受理の区分の取得関数
 * @return 継続，見送又は失敗
 */
ParseDecision ParsedTree::GetParseDecision() const {
	// 記録済の区分の返戻
	return Decision;
}

/**
 * 受理しない理由の取得関数
 * @return 見送又は失敗の理由
 */
FailureReason ParsedTree::GetParseFailure() const {
	// 記録済の理由の返戻
	return Failure;
}

/**
 * 入力の構文誤りの有無の取得関数
 * @return 受理時に構文誤りを含めば true
 */
bool ParsedTree::ContainsInputError() const {
	// 受理時の構文誤りの返戻
	return HasInputError;
}

/**
 * 受理の結果の記録関数
 * @param NewDecision 継続，見送又は失敗
 * @param Reason 見送又は失敗の理由
 * @param ContainsError 構文誤りを含むか
 */
void ParsedTree::SetParseDecision(const ParseDecision NewDecision, const FailureReason Reason, const bool ContainsError) {
	Decision = NewDecision;
	Failure = Reason;
	HasInputError = ContainsError;
	// 終了
	return;
}

/**
 * 確定した文法でソースを解析するコンストラクタ
 * @param Profile 解析に用いる文法
 * @param Str ソース文字列
 * @param DataStart 解析器から除く逐語データの開始位置
 * @throws std::length_error 原文が解析器の位置の範囲を超える場合
 */
ParsedTree::ParsedTree(const GrammarProfile &Profile, std::string Str, const size_t DataStart) : std::string(std::move(Str)),
HaltedData(DataStart) {
	// ３２ビット位置に収まらない原稿のパーサ生成前の拒否
	if(size() > std::numeric_limits<uint32_t>::max()) throw std::length_error("source exceeds the parser's position range");
	// 文法を設定した解析器の準備
	Grammar = &Profile;
	Syntax.Parser.emplace();
	// 文法と実行時ライブラリが非互換で未解析の儘とする場合の終了
	if(!Syntax.Parser->Use(Profile.GetLanguage().TsLang())) return;
	ReplaceTree(
		Syntax.Parser->Parse(
			nullptr,
			std::string_view(data(), HaltedData == std::string::npos ? size() : HaltedData),
			Syntax.IsParseExpired
		)
	);
	// 終了
	return;
}
/** ========== 構文木の解放 ========== */
