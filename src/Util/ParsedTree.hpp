#pragma once

#include "../Core/CommentRepository.hpp"
#include "CharClass.hpp"
#include "FormatLimitExceeded.hpp"
#include "SyntaxParser.hpp"
#include <tree_sitter/api.h>
#include <cctype>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Core {
	class SourceProjector;
} // namespace Core

class GrammarProfile;

struct ReadingRules;

// 解析器の読違を避ける為に解析用の入力から退避したコメント（所属と配置はコメント経路が決める，整形仕様 4.2, 7.6）
struct EscapedComment {
	uint32_t Position; // 退避した位置（現在の本文の位置で，所属する構文の端）
	std::string Text; // コメントの字面
	CommentKind Kind; // 原文での種類
};

// 解析用の補修が本文へ加えた編集（位置を補修前の本文へ戻す為に，適用前と適用後の範囲を持つ）
struct AppliedEdit {
	uint32_t Start; // 適用前の本文の始まり
	uint32_t End; // 適用前の本文の終わり
	uint32_t NewStart; // 適用後の本文の始まり
	uint32_t NewEnd; // 適用後の本文の終わり
};

// 解析用に別の字面へ置き換えた字句の復号資料
struct LexemeRestoration {
	uint32_t Start;
	uint32_t End; // 置換後の字句の始まりと終わり
	std::string Original; // 原文の字面
};

enum class ParseDecision : uint8_t { Continue, Fail, Skip }; // 入力の適応と受理の区分（継続・見送・失敗）

// 解析用の本文と構文木を所有し，受理の判定を併せて持つ解析結果（整形仕様 3.3 の ParsedTree）
class ParsedTree : public std::string {
	friend class ParseAdapter; // 入力の適応は解析用の印と其の控えを本文と共に更新する為，内側の状態へ触れる
	friend class SyntaxParser; // 受理の判定だけが解析結果の区分を記録する
	friend class Core::SourceProjector; // 投影はコメントの範囲・種類と解析用の控えを読む為，内側の状態へ触れる（書き換えない）

private:

	// 本文の差替は `Assign` だけを通す（直接書き換えると構文木の世代と本文がずれる為，継いだ変更操作を型の段階で閉じる）
	using std::string::append;
	using std::string::assign;
	using std::string::clear;
	using std::string::erase;
	using std::string::insert;
	using std::string::pop_back;
	using std::string::push_back;
	using std::string::replace;
	using std::string::resize;
	using std::string::operator+=;

	// 所有する名前と本文のビューを同じ値で引く為のハッシュ
	struct NameHash {
		using is_transparent = void; // 標準コンテナの異種検索を有効にする型名

		size_t operator()(const std::string_view Name) const noexcept; // 名前のハッシュ算出関数
	};

	// 原文順のコメントの範囲と，其の前後の並びから決まる状態（コメントの種類の判定を鎖の走査無で引く為，木の世代毎に１度作る）
	struct CommentSpan {
		uint32_t Start; // コメントの始まり
		uint32_t End; // 末尾の改行を除いた終わり
		uint32_t RunEnd; // 後の空白と，空白だけを挟んで続くコメントを飛ばした位置（行末の判定に使う）
		bool IsLineHead; // 行の前が行頭か，同じ行の前のコメントだけか
	};

	// 構文木の削除子（解放を所有者へ任せ，ムーブ・破棄を既定の物にする）
	struct TreeDeleter {
		void operator()(TSTree *const Target) const; // 構文木解放関数
	};

	// 本文に対応する構文木と其の控え（差替は ReplaceTree へ集約する）
	struct SyntaxState {
		mutable TSNode CachedRoot {}; // ルートノードのキャッシュ
		mutable bool HasCachedRoot = false; // ルートキャッシュ有効フラグ
		mutable std::vector<CommentSpan> CommentSpans; // 原文順のコメントの範囲の表（コメントの種類の判定で二分探索する）
		mutable bool HasCommentSpans = false; // コメントの位置表を作ったか
		std::optional<SyntaxParser> Parser; // 指定文法に依る構文解析器（整形仕様 3.1）
		std::unique_ptr<TSTree, TreeDeleter> Tree; // 解析済構文木
		bool IsDirty = false; // 未反映編集の有無
		bool IsTreeEdited = false; // 構文木編集済フラグ
		bool IsParseExpired = false; // 解析が時間の上限で打ち切られたか（言語の設定の失敗と区別する）
	};

	static constexpr size_t JsxSpaceMarkChars = 5; // JSX の行端の半角空白が開く `{" "}` の文字数（印の長さで其の幅を持たせる）
	const GrammarProfile *Grammar = nullptr; // 解析に用いる文法の定義（言語の指定が無ければ無し）
	size_t HaltedData = std::string::npos; // 実行停止後の逐語データの開始位置（無ければ無し）
	SyntaxState Syntax; // 本文に従属する解析状態
	std::unordered_set<std::string> StringizingMacros; // 実引数を字面の儘保つ関数形式マクロの名前（C / C++ の文字列化する物と assert）
	std::unordered_set<std::string> StatementMacros; // 文へ展開するマクロの名前（C / C++ の置換本体が `;` `{` か文の語を含む物）
	std::unordered_set<std::string> DeclaratorMacros; // 宣言子の記号へ展開するマクロの名前（C / C++ の置換本体が `*` `&` `[` `(` を含む物）
	std::unordered_set<std::string> ExpressionMacros; // 括弧で包まずに演算子を含む式へ展開するマクロの名前（C / C++ の `#define M 0 || 5` 等）
	std::unordered_set<std::string, NameHash, std::equal_to<>> FunctionMacros; // 関数形式マクロと其の別名の名前（C / C++ の実引数を字句の儘埋め込む物）
	std::unordered_map<std::string, std::vector<std::pair<uint32_t, uint32_t>>, NameHash, std::equal_to<>> DeclaredNames; // 同じファイルで変数・仮引数・関数・列挙子として宣言した名前の宣言位置と有効範囲の終端（C / C++，宣言位置順）
	std::unordered_map<std::string, std::vector<std::pair<uint32_t, uint32_t>>, NameHash, std::equal_to<>> DeclaredTypeNames; // 裸の型名の宣言位置と有効範囲の終端（C / C++，宣言位置順）
	std::vector<uint32_t> IncludeStarts; // 取込の指令 (`#include`) の位置（C / C++，昇順）
	std::vector<uint32_t> InsertedBreaks; // 解析用に補った文の区切（Kotlin の `;` と改行を含む囲みコメントの後の改行）の現在の本文位置（昇順）
	std::vector<std::pair<uint32_t, uint32_t>> SyntheticGroups; // 解析用に挿入した囲みの開きと閉じの現在の本文位置の組（開きの順）
	std::vector<LexemeRestoration> LexemeRestorations; // 解析器の字句種別を直す為に置き換えた字面の復号資料（始まりの順）
	std::vector<std::vector<AppliedEdit>> EditBatches; // 本文を全面差替えしてから加えた補修の編集の組（適用順，組の中は位置順で重ならない）
	std::vector<EscapedComment> EscapedComments; // 解析用の入力から退避したコメント（位置は補修の編集に合わせて動かす）
	uint32_t BoolNamesStart = UINT32_MAX; // C の `true` / `false` を宣言する `<stdbool.h>` の取込の終わり（取込が無ければ最大値）
	char AnalysisMark = '\0'; // 解析の為に原文へ置いた印（C / C++ の連結指定と前処理リテラル，CSS の逃避の逆斜線，Ruby の埋込文書，置いて居なければ無し）
	std::string JsxSpaceMark; // JSX の本文の端の半角空白を整形の間保つ印（原文に無い私用領域の文字を `{" "}` の文字数だけ並べた物，UTF-8）
	std::string JsxTabMark; // JSX の本文の端のタブを整形の間保つ印（同上）
	std::unordered_map<const void *, std::shared_ptr<const Core::OwnedRegion>> OwnedRegions; // 独立したコードとして整形した HTML の埋込の出力（埋込の中身の節点で引く，整形仕様 4.4）
	ParseDecision Decision = ParseDecision::Continue; // 受理の区分
	FailureReason Failure; // 受理しない理由
	bool HasInputError = false; // 受理した入力の構文誤りの有無
	static bool MentionsName(const std::string_view Text, const std::unordered_set<std::string> &Names); // 字面が名前の何れかを含むかの判定関数
	static std::pair<size_t, char> BlankReference(const std::string_view Text, const size_t Pos); // 空白・タブへ復号される数値文字参照の長さと其の文字の取得関数
	bool HasDisputedJsxLineEdges(const std::string_view Text) const; // 行端の扱いが TypeScript と Babel で分かれる JSX 本文かの判定関数
	bool IsVisibleDeclaration(const std::vector<std::pair<uint32_t, uint32_t>> &Declarations, const uint32_t Use) const; // 使う位置で有効な宣言の有無の判定関数
	TSNode SiblingBeforeImmediate(const TSNode Node) const; // 直前の兄弟ノード取得関数
	std::pair<uint32_t, uint32_t> CommentRangeOf(const TSNode Node) const; // コメントの本文の範囲の取得関数（幅０の節点は直前の本文を回収）
	const std::vector<CommentSpan> &GetCommentSpans() const; // 原文順のコメントの範囲の表の取得関数（木の世代毎に１度だけ作る）

	CommentKind ClassifyComment(
		const std::vector<CommentSpan> &Spans,
		const uint32_t Start,
		const uint32_t End,
		bool *const OutIsAtLineStart = nullptr
	) const; // コメントの種類の判定の共通関数

	void ReplaceTree(TSTree *const NewTree); // 構文木と其の控えの差替関数
	void Reparse(); // 構文木の再解析関数
	void AttachTrailingData(const std::string_view Data); // 解析済の本文へ逐語データを付ける関数
	void SetParseDecision(const ParseDecision NewDecision, const FailureReason Reason, const bool ContainsError); // 受理の結果の記録関数

	template<typename String> void AssignSource(String &&NewSource); // 本文差替と未反映状態の共通更新関数

	void Assign(std::string &&NewSource); // ソース移動代入関数

public:

	// Ruby の局所変数の導入の索引
	struct RubyLocals {
		std::unordered_map<const void *, std::unordered_map<std::string_view, uint32_t>> First; // スコープ毎の名前の最初の導入位置
		std::unordered_map<const void *, std::vector<const void *>> Chains; // 対象の節点毎の，導入の見えるスコープ（内から外へ）
		bool IsDefined(const TSNode Node, const std::string_view Name, const uint32_t Start) const; // 局所変数の導入済判定関数
	};

	ParseDecision GetParseDecision() const; // 受理の区分の取得関数
	FailureReason GetParseFailure() const; // 受理しない理由の取得関数
	bool ContainsInputError() const; // 入力の構文誤りの有無の取得関数
	TSNode FieldChild(const TSNode Node, const Core::ChildRole Role) const; // 指定された場の役割を持つ最初の子ノード取得関数
	TSNode FirstNamedChild(const TSNode Node, const Core::Role Role) const; // 指定された役割を持つ最初の名前付子ノード取得関数
	bool IsHeaderField(const TSNode Node, const TSNode Child) const; // 見出の部品を指す場の子かの判定関数
	TSNode IfConsequence(const TSNode Node) const; // if 文の真枝本体取得関数
	bool HasDisputedJsxText(const TSNode Container) const; // 行端の扱いが処理系で分かれる本文を持つ JSX 要素かの判定関数
	bool IsParsed() const; // 解析済判定関数
	bool HasExpiredParse() const; // 解析の打切の判定関数
	uint32_t End(const TSNode Node) const; // ノードの終了バイト取得関数
	uint32_t Start(const TSNode Node) const; // ノードの開始バイト取得関数
	uint32_t Len(const TSNode Node) const; // ノード範囲のバイト長取得関数
	std::string_view View(const TSNode Node) const; // ノード範囲のビュー取得関数
	bool ExpandsToStatements(const TSNode Stmt) const; // 文へ展開するマクロの呼出の文かの判定関数
	bool IsOmittedSemicolon(const TSNode Child, const TSNode PrevChild) const; // 構造化で省く最上位の空の式文 `;` かの判定関数
	bool MentionsDeclaratorMacro(const TSNode Type) const; // 宣言子の記号へ展開するマクロを含む型かの判定関数
	bool MentionsExpressionMacro(const TSNode Node) const; // 括弧で包まずに演算子を含む式へ展開するマクロを含むかの判定関数
	bool IsFunctionMacro(const std::string_view Name) const; // 同じファイルで定義した関数形式マクロ（別名を含む）の名前かの判定関数
	bool IsDeclaredName(const std::string_view Name, const uint32_t Use) const; // 使う位置で有効な宣言を同じファイルに持つ名前かの判定関数
	bool IsDeclaredTypeName(const std::string_view Name, const uint32_t Use) const; // 使う位置で有効な型宣言を同じファイルに持つ名前かの判定関数
	bool IsAmbiguousDeclaration(const TSNode Node) const; // 式と区別出来ない宣言かの判定関数
	bool HasBoolNamesAt(const uint32_t Use) const; // C の真偽値の名前を使える位置かの判定関数
	bool IsCssUrlArguments(const TSNode Node) const; // CSS の `url(...)` の引数の並びかの判定関数
	bool AttachesRubySign(const TSNode Operand) const; // Ruby の符号を密着させても読みが変わらない被演算子かの判定関数
	TSNode SwiftPrefixArguments(const TSNode Node) const; // Swift の前置の演算子の呼出と読んだ括弧の実引数の並びの取得関数
	bool HasBodyLikeExpression(const TSNode Node) const; // 見出の本体の始まりと紛れる式の判定関数
	bool LeadsRubyAmbiguously(TSNode Lead) const; // Ruby の実引数の先頭の曖昧さの判定関数
	RubyLocals IndexRubyLocals(const Core::Role Targets) const; // Ruby の局所変数の導入の索引の構築関数
	TSNode GetRoot() const; // ルートノード取得関数
	bool Is(const TSNode Node, const Core::Role Wanted) const; // 解析器の節点の記号が文法の定義で役割を持つかの判定関数
	bool SpellsToken(const TSNode Node, const Core::Role Wanted) const; // 節点の字面が役割を持つ字句の綴りかの判定関数
	const Core::SyntaxRules &GetRules() const; // 文法の構文・意味の固定の定義の取得関数
	const ReadingRules &GetReading() const; // 入力の読取規則の取得関数
	const GrammarProfile &GetProfile() const; // 解析に用いる文法の定義の取得関数
	size_t HaltedDataStart() const; // 実行停止後の逐語データの開始位置の取得関数
	std::string_view CodeText() const; // 逐語データを除いた解析対象の本文の取得関数
	bool BreaksReadingAt(const uint32_t PrevEnd, const uint32_t NextStart) const; // 字句の間の改行で読みが変わるかの判定関数

	uint32_t LineEndAfter(
		const uint32_t From,
		const std::span<const std::pair<uint32_t, uint32_t>> Gaps,
		std::vector<TSNode> &Following
	) const; // 行末コメントを置く行末の位置の取得関数

	CommentKind KindOf(const TSNode Comment) const; // 原文でのコメントの種類の判定関数
	char GetAnalysisMark() const; // 解析の為に原文へ置いた印の取得関数
	std::string_view GetJsxBlankMark(const char Blank) const; // JSX の本文の端の空白・タブを保つ印の取得関数
	std::shared_ptr<const Core::OwnedRegion> OwnedRegionOf(const TSNode RawText) const; // 独立したコードとして整形した埋込の出力の取得関数
	std::vector<std::pair<uint32_t, uint32_t>> CollectCommentRanges(const uint32_t BodyEnd) const; // コメントの節点と字句の隙間に残ったコメントの範囲の収集関数
	uint32_t MacroLineCommentStart(const TSNode Body) const; // マクロの本体の字句が呑んだ行コメントの開始位置の取得関数
	bool StringizesArguments(const TSNode List) const; // 実引数を字面の儘保つ呼出かの判定関数
	bool AbsorbsGoComment(const TSNode Node) const; // 直前のコメントを取り込む Go の最上位の宣言かの判定関数
	uint32_t OffsetBeforeEdits(uint32_t Offset) const; // 補修前の本文の位置の取得関数
	const std::vector<uint32_t> &GetInsertedBreaks() const; // 解析用の文の区切の位置の取得関数
	const std::vector<std::pair<uint32_t, uint32_t>> &GetSyntheticGroups() const; // 解析用の囲みの位置の組の取得関数
	const std::vector<LexemeRestoration> &GetLexemeRestorations() const; // 解析用に置き換えた字句の復号資料の取得関数
	const LexemeRestoration *RestorationAt(const uint32_t Start, const uint32_t End) const; // 範囲に対応する字句の復号資料の取得関数
	TSTree *GetMutableTree(); // 可変構文木取得関数
	void SetTreeEdited(const bool IsEdited); // 構文木編集済の設定関数
	void SetOwnedRegion(const TSNode RawText, Core::OwnedRegion &&Region); // 独立したコードとして整形した埋込の出力の控え関数

	void AssignEdited(
		std::string &&NewSource,
		std::vector<uint32_t> &&Breaks,
		std::vector<std::pair<uint32_t, uint32_t>> &&Groups,
		std::vector<LexemeRestoration> &&Restorations,
		std::vector<AppliedEdit> &&Batch
	); // 補修の編集を適用した本文の差替関数

	void AddLexemeRestoration(const uint32_t Start, const uint32_t End, std::string Original); // 解析用に置き換えた字句の記録関数
	void CollectMacros(); // 前処理マクロの性質（実引数の文字列化・文や宣言子の記号への展開）の収集関数
	void CollectDeclaredNames(); // 宣言した値と型の名前と取込の指令の位置の収集関数 (C / C++)

	ParsedTree &operator=(ParsedTree &&Other) = default; // 移動代入演算子

	ParsedTree(const GrammarProfile &Profile, std::string Str, const size_t DataStart = std::string::npos); // コンストラクタ（確定した文法と逐語データの開始）
	ParsedTree(const ParsedTree &Other); // 適応済の木の共有写しの構築関数

	ParsedTree(ParsedTree &&Other) = default; // 移動構築関数
};

// 取得時の reset 迄参照せず，確保済領域を再利用するスレッド局所カーソル貯留庫
struct CursorPool {
	std::vector<TSTreeCursor> Free; // 返却済カーソルの積重

	~CursorPool(); // デストラクタ（スレッド終了時の貯留カーソル解放）
};

inline thread_local CursorPool PooledCursors; // スレッド局所の貯留庫実体

/**
 * 貯留庫からのカーソル取得関数
 * @param Node 走査対象ノード
 * @return 対象ノードに位置付いたカーソル
 */
inline TSTreeCursor AcquireCursor(const TSNode Node) {
	// カーソル貯留状態に応じた取得元の選択
	// 貯留が無い場合に新規生成したカーソルの返戻
	if(PooledCursors.Free.empty()) return ts_tree_cursor_new(Node);
	// 貯留カーソルの取出と走査対象への再標的化
	TSTreeCursor Cursor = PooledCursors.Free.back();
	PooledCursors.Free.pop_back();
	ts_tree_cursor_reset(&Cursor, Node);
	// 再標的化済カーソルの返戻
	return Cursor;
}

/**
 * 貯留庫へのカーソル返却関数
 * @param Cursor 返却するカーソル（以後呼出側では使用しない）
 */
inline void ReleaseCursor(const TSTreeCursor &Cursor) {
	// 返却カーソルの貯留
	PooledCursors.Free.push_back(Cursor);
	// 終了
	return;
}

// 例外時も借用カーソルを返すインライン保持子
struct HeldCursor {
	TSTreeCursor Cursor; // 借りたカーソル

	/**
	 * カーソルの位置取得演算子
	 * @return 借りたカーソルへの位置
	 */
	inline TSTreeCursor *operator&() {
		// カーソルの位置の返戻
		return &Cursor;
	}

	HeldCursor &operator=(const HeldCursor &) = delete; // 複写代入演算子（禁止）
	/**
	 * カーソルの借用のコンストラクタ
	 * @param Node 走査対象ノード
	 */
	explicit inline HeldCursor(const TSNode Node) : Cursor(AcquireCursor(Node)) {}
	HeldCursor(const HeldCursor &) = delete; // 複写コンストラクタ（禁止）

	/**
	 * カーソルの返却のデストラクタ
	 */
	inline ~HeldCursor() noexcept {
		// 貯留庫への積直が失敗した場合は例外を出さずに解放する
		try {
			ReleaseCursor(Cursor);
		} catch(...) {
			ts_tree_cursor_delete(&Cursor);
		}
		// 終了
		return;
	}
};

/**
 * 子ノードの線形走査の共通関数
 * 計算量：子数 C と Fn の総費用 F に対し O(C + F)
 * @param Node 走査対象ノード
 * @param Fn 各子に適用する関数（bool 戻値の偽で打切，void なら最後迄走査）
 */
template<bool IsNamedOnly, typename F> inline void ForEachChildImpl(const TSNode Node, F &&Fn) {
	// 対象節点に位置付けたカーソルの借用
	HeldCursor Cursor(Node);
	// 先頭子からの兄弟走査
	if(ts_tree_cursor_goto_first_child(&Cursor)) {
		do {
			const TSNode Child = ts_tree_cursor_current_node(&Cursor);
			// 名前付限定版は無名トークン（`{`, `,` 等）の読飛
			if(IsNamedOnly && !ts_node_is_named(Child)) continue;
			// 戻値型に応じた打切可否の適用
			if constexpr(std::is_same_v<std::invoke_result_t<F, TSNode>, bool>) {
				if(!Fn(Child)) break;
			} else Fn(Child);
		} while(ts_tree_cursor_goto_next_sibling(&Cursor));
	}
	// 終了
	return;
}

/**
 * 子ノードの線形走査関数
 * 走査する子数 C とコールバックの総費用 F に対し O(C + F)
 * @param Node 走査対象ノード
 * @param Fn 各子に適用する関数（bool 戻値の偽で打切，void なら最後迄走査）
 */
template<typename F> inline void ForEachChild(const TSNode Node, F &&Fn) {
	// 全ての子を対象にする共通走査の呼出
	ForEachChildImpl<false>(Node, std::forward<F>(Fn));
	// 終了
	return;
}

/**
 * 名前付子限定の線形走査関数
 * 無名を含む子数 C とコールバックの総費用 F に対し O(C + F)
 * @param Node 走査対象ノード
 * @param Fn 各名前付子に適用する関数（bool 戻値の偽で打切）
 */
template<typename F> inline void ForEachNamedChild(const TSNode Node, F &&Fn) {
	// 無名トークン（`{`, `,` 等）を読み飛ばす名前付限定版へ委譲
	ForEachChildImpl<true>(Node, std::forward<F>(Fn));
	// 終了
	return;
}

/**
 * 最後の名前付子の取得関数
 * @param Node 走査対象ノード
 * @return 最後の名前付子（子が無ければ空ノード，添字を符号無で巻戻さない）
 */
inline TSNode LastNamedChild(const TSNode Node) {
	// 名前付子の数
	const uint32_t Count = ts_node_named_child_count(Node);
	// 最後の名前付子又は空ノードの返戻
	return Count ? ts_node_named_child(Node, Count - 1) : TSNode{};
}

/**
 * 指定トークンに一致する無名子の有無の判定関数
 * `typeof X` 等の演算子／キーワード検出に用いる
 * 子数 C と字面比較の総バイト数 B に対し O(C + B)
 * @param Source ソース文字列
 * @param Node 対象ノード
 * @param Token 探索するトークン
 * @return 含まれれば true
 */
inline bool HasUnnamedTokenChild(const std::string &Source, const TSNode Node, const std::string_view Token) {
	bool IsFound = false;
	// 無名子の対象トークンとの照合
	ForEachChild(
		Node,
		[&](const TSNode Child) -> bool {
			// 名前付の子を読み飛ばす事の返戻
			if(ts_node_is_named(Child)) return true;
			// 無名子の原稿範囲と探索トークンの照合
			if(
				const uint32_t Start = ts_node_start_byte(Child), End = ts_node_end_byte(Child);
				End > Start && std::string_view(Source.data() + Start, End - Start) == Token
			) {
				// 一致時の発見状態確定
				IsFound = true;
				// 一致トークン発見の為，走査打切の返戻
				return false;
			}
			// 次の子へ走査継続の返戻
			return true;
		}
	);
	// トークン一致無名子の有無の返戻
	return IsFound;
}

/**
 * 降下制御付の子ノード先行順走査関数
 * Func の bool 戻値が真なら当該ノード内部に降り，偽なら次の兄弟ノードへ進む
 * 走査節点数 V とコールバックの総費用 F に対し O(V + F)
 * @param Root 走査開始ノード（直下の子から処理し Root 自身には Func を適用しない）
 * @param Func 各ノードに適用し内部へ降りるか否かを返す関数（節点と親を受ける形なら親を，節点と祖先の並びを受ける形なら Root から親迄の並びも渡す）
 */
template<typename Fn> inline void WalkChildrenCursor(const TSNode Root, Fn &&Func) {
	// 親配列を降下・上昇と共に更新し，ts_node_parent の二乗走査を避ける
	constexpr bool WantsAncestors = std::is_invocable_r_v<bool, Fn, TSNode, std::span<const TSNode>>;
	constexpr bool WantsParent = WantsAncestors || std::is_invocable_r_v<bool, Fn, TSNode, TSNode>;
	std::vector<TSNode> Parents;
	HeldCursor Cursor(Root);
	// 根直下からの先行順走査
	if(ts_tree_cursor_goto_first_child(&Cursor)) {
		if constexpr(WantsParent) Parents.push_back(Root);
		for(bool IsDone = false; !IsDone;) {
			const TSNode Node = ts_tree_cursor_current_node(&Cursor);
			// コールバック形に応じた親情報の供給
			bool ShouldDescend;
			if constexpr(WantsAncestors) ShouldDescend = Func(Node, std::span<const TSNode>(Parents));
			else if constexpr(WantsParent) ShouldDescend = Func(Node, Parents.back());
			else ShouldDescend = Func(Node);
			if(ShouldDescend && ts_tree_cursor_goto_first_child(&Cursor)) {
				// 降下時の親経路の更新
				if constexpr(WantsParent) Parents.push_back(Node);
				continue;
			}
			// 次の兄弟又は未走査の祖先兄弟への移動
			while(!ts_tree_cursor_goto_next_sibling(&Cursor)) {
				// 根到達時の走査終了判定
				if(!ts_tree_cursor_goto_parent(&Cursor)) {
					IsDone = true;
					break;
				}
				if constexpr(WantsParent) Parents.pop_back();
			}
		}
	}
	// 終了
	return;
}

/**
 * 名前付の節点かの判定関数
 * @param Node 判定対象の節点（子や兄弟を引いた結果の空節点も受ける）
 * @return 空でない名前付の節点なら true
 */
inline bool IsNamedNode(const TSNode Node) {
	// 空節点は構文木を辿れず名前も持たない事の返戻
	return !ts_node_is_null(Node) && ts_node_is_named(Node);
}

/**
 * 構文木の根の判定関数
 * @param Node 判定対象ノード
 * @return 木の根なら true
 */
inline bool IsTreeRoot(const TSNode Node) {
	// 空節点は木を持たず，根の問合せが落ちる事の返戻
	if(ts_node_is_null(Node)) return false;
	// 親の有無で判定すると `ts_node_parent` が根から降下し直す為，根ノードとの同一性で判定する事の返戻
	return ts_node_eq(Node, ts_tree_root_node(Node.tree));
}

/**
 * 述語を満たす直接子の有無の判定関数
 * 名前付／無名の区別無く最初の発見で打ち切る
 * 走査する子数 C と述語の総費用 F に対し O(C + F)
 * @param Node 走査開始ノード
 * @param Pred ノードを受け取り bool を返す述語
 * @return 直接子に述語充足が有れば true
 */
template<typename Predicate> inline bool HasChildOf(const TSNode Node, Predicate Pred) {
	// 空ノードは false の返戻
	if(ts_node_is_null(Node)) return false;
	// 直接子を走査するカーソルと発見状態の準備
	HeldCursor Cursor(Node);
	bool IsFound = false;
	// 最初の述語充足迄の直接子走査
	if(ts_tree_cursor_goto_first_child(&Cursor)) {
		// 各直接子への述語適用
		do if(Pred(ts_tree_cursor_current_node(&Cursor))) {
			IsFound = true;
			break;
		}
		while(ts_tree_cursor_goto_next_sibling(&Cursor));
	}
	// 直接子の述語充足有無の返戻
	return IsFound;
}

/**
 * 述語を満たす子孫の有無の判定関数
 * 走査節点数 V と述語の総費用 F に対し O(V + F)
 * @param Node 走査開始ノード
 * @param Pred ノードを受け取り bool を返す述語
 * @return 述語を満たす子孫が見付かれば true（自身も含む）
 */
template<typename Predicate> inline bool HasDescendantOf(const TSNode Node, Predicate Pred) {
	// 空ノードは述語を満たさない事の返戻
	if(ts_node_is_null(Node)) return false;
	// TSTreeCursor で反復的に先行順走査し再帰呼出と ts_node_child の累積 O(C ²) を回避する
	HeldCursor Cursor(Node);
	bool IsFound = false;
	// 部分木内の先行順述語照合
	while(true) {
		// 現在節点への述語適用
		if(Pred(ts_tree_cursor_current_node(&Cursor))) {
			IsFound = true;
			break;
		}
		if(ts_tree_cursor_goto_first_child(&Cursor)) continue;
		// 未走査兄弟を持つ祖先迄の上昇
		while(!ts_tree_cursor_goto_next_sibling(&Cursor)) {
			// 部分木内に述語充足子孫無の返戻
			if(!ts_tree_cursor_goto_parent(&Cursor) || ts_node_eq(ts_tree_cursor_current_node(&Cursor), Node)) return false;
		}
	}
	// 部分木内の発見有無の返戻
	return IsFound;
}

/**
 * 祖先を伴う述語を満たす子孫の有無の判定関数
 * 走査節点数 V と述語の総費用 F に対し O(V + F)
 * @param Node 走査開始ノード（其の祖先は渡さない）
 * @param Pred ノードと其の祖先の並び（走査開始ノードから親迄，末尾が親，開始ノードでは空）を受け取り bool を返す述語
 * @return 述語を満たす子孫が見付かれば true（自身も含む）
 */
template<typename Predicate> inline bool HasDescendantWithAncestorsOf(const TSNode Node, Predicate Pred) {
	// 空ノードの返戻
	if(ts_node_is_null(Node)) return false;
	HeldCursor Cursor(Node);
	std::vector<TSNode> Path;
	bool IsFound = false;
	// 祖先経路を伴う部分木の先行順走査
	while(true) {
		// `ts_node_parent` は呼出毎に根から降り直し，子孫毎に呼ぶと深さの二乗の費用に為る為，走査の道筋を積んで祖先の並びを渡す
		const TSNode Cur = ts_tree_cursor_current_node(&Cursor);
		// 現在節点への祖先列付述語適用
		if(Pred(Cur, std::span<const TSNode>(Path))) {
			IsFound = true;
			break;
		}
		if(ts_tree_cursor_goto_first_child(&Cursor)) {
			// 降下先へ渡す祖先経路の更新
			Path.push_back(Cur);
			continue;
		}
		// 子無なら兄弟へ，兄弟無なら親へ戻り次の兄弟を試す（開始 Node より上には行かない）
		while(!ts_tree_cursor_goto_next_sibling(&Cursor)) {
			// 部分木内に述語充足子孫無の返戻
			if(!ts_tree_cursor_goto_parent(&Cursor) || ts_node_eq(ts_tree_cursor_current_node(&Cursor), Node)) return false;
			Path.pop_back();
		}
	}
	// 部分木内の発見有無の返戻
	return IsFound;
}

/**
 * 部分木の先行順走査関数
 * 走査節点数 V とコールバックの総費用 F に対し O(V + F)
 * @param Node 走査開始ノード（此の部分木のみを対象とし外へは出ない）
 * @param Func 各ノードに適用する関数
 */
template<typename F> inline void WalkAst(const TSNode Node, const F &Func) {
	// TSTreeCursor の反復走査で ts_node_child の累積 O(C²) と再帰を避ける
	HeldCursor Cursor(Node);
	while(true) {
		Func(ts_tree_cursor_current_node(&Cursor));
		if(ts_tree_cursor_goto_first_child(&Cursor)) continue;
		// 子無なら兄弟へ，兄弟無なら親へ戻り次の兄弟を試す（Node の親より上には行かない）
		while(!ts_tree_cursor_goto_next_sibling(&Cursor)) {
			// 開始節点へ戻った場合の返戻
			if(!ts_tree_cursor_goto_parent(&Cursor) || ts_node_eq(ts_tree_cursor_current_node(&Cursor), Node)) return;
		}
	}
}
