#pragma once

#include "NodeKind.hpp"
#include <tree_sitter/api.h>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// 整形の手間・時間・出力の上限に達した事を表す例外（見送の理由を持たせ，上限毎に型を分けない）
struct FormatLimitExceeded : std::exception {
	std::string_view Reason; // 利用者へ示す見送の理由
	bool IsDefect; // 発火してはならない防壁か（資源の上限でなく整形器の不具合で，終了コードで知らせる）

	explicit FormatLimitExceeded(const std::string_view Text, const bool IsFormatterDefect = false); // 上限超過の例外の構築関数
};

// 原稿の移動後に数え直す原子的な計数（`std::atomic` を直に持つと移動不可）
struct ResettingCounter {
	std::atomic<size_t> Value { 0 }; // 計数の実体
	ResettingCounter &operator=(const ResettingCounter &); // 複写代入演算子（複写先は数え直す）

	ResettingCounter(const ResettingCounter &); // 複写コンストラクタ（原稿の移動も此処を通る）

	ResettingCounter() = default; // コンストラクタ
};

// コメント紐付情報構造体
struct CommentAttach {
	std::string Text; // コメント本文
	bool IsLeading; // 先行コメントか否か
	uint32_t Seq = 0; // 出現順
	bool IsOwnLine = false; // 錨の後の独立した行へ置く末尾コメントか（塊の末尾のコメント）
	bool ForcesOwnLine = false; // 本体の文の無い節の末尾コメントを独立行へ置くか
	bool IsGluedAfter = false; // 後の符号付の数 (`-2px`) へ空白無で続く CSS のコメントか（Sass はコメントの後に空白の無い `-` を減算と読む）
	bool IsGluedBefore = false; // 後の符号付の数へ続く CSS のコメントの前も空白無か（Sass は `1px/* c */-2px` を減算，`1px /* c */-2px` を非推奨の読みにする）
	bool IsHead = false; // 見出の字句（`{` / `else` / `:` 等）の直後の行末コメントを，錨（本体の最初の要素）の前の見出の行の行末へ置く先行コメントか
	bool IsBeforeToken = false; // 錨との間の無名字句より前に在った先行コメントか
	bool IsCloserDirective = false; // 閉じ字句だけの次の行 (`);` / `}`) に作用する次の行に作用する指令か（整形で其の行が畳まれれば錨の行の前へ置く）
	bool IsStandalone = false; // 原文で独立した行に在ったコメントか（宣言の分割で新しい宣言の最初に来た宣言子のコメントを宣言の前へ置く）
	bool IsVerbatim = false; // 処理系が読むコメントか（Rust の文書化コメント・cgo の前文等，本文を１バイトも変えず継続行の `*` も揃えない）
	std::string Joined {}; // 行末へ置く時の本文（句点で行を分けない形で，本文と同じなら空）
};

// 本文・構文木・コメント紐付を一体で更新し，並列読取中は変更しないソース文字列クラス
class TSSource : public std::string {
private:

	enum class ParenAnchor : uint8_t { None, Empty, Filled, EmptyCall };

	// 所有する名前と本文のビューを同じ値で引く為のハッシュ
	struct NameHash {
		using is_transparent = void; // 標準コンテナの異種検索を有効にする型名

		size_t operator()(const std::string_view Name) const noexcept; // 名前のハッシュ算出関数
	};

	// 静的な型名と共有親索引で経路の複写を避ける紐付スナップショット
	struct PathStep {
		std::string_view Type; // ノード型名
		std::string Name; // フィールド名
		uint32_t Index; // 親内での位置
		uint32_t Ordinal; // 親の名前付の子の中での順番（型と名前を問わない）
		uint32_t Parent; // 親の段の添字（根の直下の段は NoStep）
	};

	// 丸括弧を外した後も対応先を特定する紐付スナップショット
	struct AttachSnapshot {
		uint32_t Leaf = NoStep; // 錨の段の添字（根の錨は NoStep）
		std::string_view NodeType; // ノード型名（文法の静的領域を指す為，ビュー保持で安全）
		ParenAnchor Paren = ParenAnchor::None; // アンカーが丸括弧の対か（空か中身を持つか）
		std::vector<CommentAttach> Leading; // 先行コメントの控え
		std::vector<CommentAttach> Trailing; // 後続コメントの控え
	};

	// 紐付スナップショットの一式（段の表と錨毎の控え）
	struct AttachSnapshots {
		std::vector<PathStep> Steps; // 経路の段の表
		std::vector<AttachSnapshot> Anchors; // 錨毎の控え
		bool IsEmpty() const; // 控えの有無の判定関数
		void Clear(); // 控えの破棄関数
	};

	// パーサ・構文木の削除子（解放を所有者へ任せ，ムーブ・破棄を既定の物にする）
	struct ParserDeleter {
		void operator()(TSParser *const Target) const; // パーサ解放関数
	};

	struct TreeDeleter {
		void operator()(TSTree *const Target) const; // 構文木解放関数
	};

	// 本文に対応する構文木と其の控え（差替は ReplaceTree へ集約する）
	struct SyntaxState {
		mutable TSNode CachedRoot {}; // ルートノードのキャッシュ
		mutable bool HasCachedRoot = false; // ルートキャッシュ有効フラグ
		mutable ResettingCounter FormatWork; // 節点毎に組み立てた字面の総量（深いネストで二乗に膨らむ工程を資源の上限で打ち切る）
		mutable std::unordered_map<const void *, uint8_t> NodeContexts; // 祖先依存の節点文脈キャッシュ
		size_t TokenBytes = 0; // 字下げに依存しない葉字句長の和（木の差替時だけ更新）
		std::unique_ptr<TSParser, ParserDeleter> Parser; // tree-sitter のパーサ
		std::unique_ptr<TSTree, TreeDeleter> Tree; // 解析済構文木
		TSFieldId NameFieldId = 0; // `name` 場の識別子（構築時に１回だけ文字列解決し，以後の場引きを識別子直引きにする）
		bool IsDirty = false; // 未反映編集の有無
		bool IsTreeEdited = false; // 構文木編集済フラグ
		bool IsParseExpired = false; // 解析が時間の上限で打ち切られたか（言語の設定の失敗と区別する）
	};

	// 木の識別子に従属する紐付と，木を跨いで保持する退避情報
	struct AttachmentState {
		uint32_t SuspendDepth = 0; // 紐付停止のネスト深度
		std::unordered_map<const void *, std::vector<CommentAttach>> LeadingByNode; // ノード別の先行コメント
		std::unordered_map<const void *, std::vector<CommentAttach>> TrailingByNode; // ノード別の後続コメント
		std::set<uint32_t> AttachedStartBytes; // 紐付済ノードの開始バイト昇順集合（部分木の枝刈り専用，同じ位置の節点が他に在り得る為に取出しでは消さない）
		AttachSnapshots PendingSnapshot; // 保留中の紐付スナップショット
		AttachSnapshots SavedSnapshot; // 退避中の紐付スナップショット
		void ClearNodes(); // 現在の木に紐付く情報だけの消去関数
	};

	static constexpr uint32_t NoStep = UINT32_MAX; // 段の無い印（根）
	static constexpr size_t WarnPreviewMax = 40; // 警告プレビューの切詰幅
	SyntaxState Syntax; // 本文に従属する解析状態
	AttachmentState Attachments; // 本文に従属する紐付状態
	std::unordered_set<std::string> StringizingMacros; // 実引数を字面の儘保つ関数形式マクロの名前（C / C++ の文字列化する物と assert）
	std::unordered_set<std::string> StatementMacros; // 文へ展開するマクロの名前（C / C++ の置換本体が `;` `{` か文の語を含む物）
	std::unordered_set<std::string> DeclaratorMacros; // 宣言子の記号へ展開するマクロの名前（C / C++ の置換本体が `*` `&` `[` `(` を含む物）
	std::unordered_set<std::string> ExpressionMacros; // 括弧で包まずに演算子を含む式へ展開するマクロの名前（C / C++ の `#define M 0 || 5` 等）
	std::unordered_set<std::string, NameHash, std::equal_to<>> FunctionMacros; // 関数形式マクロと其の別名の名前（C / C++ の実引数を字句の儘埋め込む物）
	std::unordered_map<std::string, std::vector<uint32_t>, NameHash, std::equal_to<>> DeclaredNames; // 同じファイルで変数・仮引数・関数・列挙子として宣言した名前と宣言の位置（C / C++，昇順）
	std::vector<uint32_t> IncludeStarts; // 取込の指令 (`#include`) の位置（C / C++，昇順）
	std::string JsxSpaceMark; // JSX の本文の端の半角空白を整形の間保つ印（原文に無い私用領域の文字，UTF-8）
	std::string JsxTabMark; // JSX の本文の端のタブを整形の間保つ印（同上）
	static bool AreWarningsEnabled(); // 警告出力可否の判定関数
	static bool MentionsName(const std::string_view Text, const std::unordered_set<std::string> &Names); // 字面が名前の何れかを含むかの判定関数
	static TSNode SiblingBeforeImmediate(const TSNode Node); // 直前の兄弟ノード取得関数
	static bool IsJsxSpace(const uint32_t Point); // JSX の行内空白（TypeScript の空白の集合）かの判定関数
	static std::pair<size_t, char> BlankReference(const std::string_view Text, const size_t Pos); // 空白・タブへ復号される数値文字参照の長さと其の文字の取得関数
	static bool HasDisputedJsxLineEdges(const std::string_view Text); // 行端の扱いが TypeScript と Babel で分かれる JSX 本文かの判定関数
	static TSNode SiblingBefore(const TSNode Node); // 前方の兄弟ノード取得関数

	static const std::vector<CommentAttach> &FindAttachments(
		const std::unordered_map<const void *, std::vector<CommentAttach>> &Map,
		const TSNode Node
	); // 先行／末尾コメント検索の共通関数

	std::string_view NodeNameView(const TSNode Node) const; // ノードの名前ビュー取得関数（事前解決済の場識別子で直引き）
	AttachSnapshots SnapshotAttachments() const; // 現在の紐付状態のスナップショット取得関数
	void RebindFromSnapshot(AttachSnapshots &&Snapshot); // スナップショットからの再紐付関数
	void ReplaceTree(TSTree *const NewTree); // 構文木と其の控えの差替関数
	void Reparse(); // 構文木の再解析関数
	AttachSnapshots FreshSnapshot(); // 最新構文木のスナップショット取得関数
	void PrepareAssign(); // 代入直前のスナップショット保存関数

	template<typename String> void AssignSource(String &&NewSource); // 本文差替と未反映状態の共通更新関数

	void AddAttach(
		std::unordered_map<const void *, std::vector<CommentAttach>> &Primary,
		const TSNode Node,
		std::vector<CommentAttach> Comments
	); // 紐付追加の共通関数

public:

	static TSNode FieldChild(const TSNode Node, const char *const Name); // 指定フィールド名の子ノード取得関数
	static TSNode IfConsequence(const TSNode Node); // if 文の真枝本体取得関数
	static bool HasDisputedJsxText(const std::string_view Text, const TSNode Container); // 行端の扱いが処理系で分かれる本文を持つ JSX 要素かの判定関数
	bool HasAttachments() const; // 紐付コメント有無の判定関数
	bool IsParsed() const; // 解析済判定関数
	bool HasExpiredParse() const; // 解析の打切の判定関数
	uint32_t End(const TSNode Node) const; // ノードの終了バイト取得関数
	uint32_t Start(const TSNode Node) const; // ノードの開始バイト取得関数
	uint32_t Len(const TSNode Node) const; // ノード範囲のバイト長取得関数
	std::string_view View(const TSNode Node) const; // ノード範囲のビュー取得関数
	bool ExpandsToStatements(const TSNode Stmt) const; // 文へ展開するマクロの呼出の文かの判定関数
	bool MentionsDeclaratorMacro(const TSNode Type) const; // 宣言子の記号へ展開するマクロを含む型かの判定関数
	bool MentionsExpressionMacro(const TSNode Node) const; // 括弧で包まずに演算子を含む式へ展開するマクロを含むかの判定関数
	bool IsFunctionMacro(const std::string_view Name) const; // 同じファイルで定義した関数形式マクロ（別名を含む）の名前かの判定関数
	bool IsDeclaredName(const std::string_view Name, const uint32_t Use) const; // 使う位置で有効な宣言を同じファイルに持つ名前かの判定関数
	bool IsCssUrlArguments(const TSNode Node) const; // CSS の `url(...)` の引数の並びかの判定関数
	std::unordered_map<const void *, uint8_t> &GetContextMemo() const; // 祖先で決まる節点毎の文脈の控えの取得関数
	bool AddFormatWork(const size_t Amount) const; // 組み立てた字面の量の加算と上限の検査関数
	bool HasExceededFormatWork() const; // 組み立てた字面の量の上限超過判定関数
	bool AttachesRubySign(const TSNode Operand) const; // Ruby の符号を密着させても読みが変わらない被演算子かの判定関数
	TSNode GetRoot() const; // ルートノード取得関数
	std::string Text(const TSNode Node) const; // ノード範囲の文字列取得関数
	std::string_view GetJsxBlankMark(const char Blank) const; // JSX の本文の端の空白・タブを保つ印の取得関数
	bool StringizesArguments(const TSNode List) const; // 実引数を字面の儘保つ呼出かの判定関数
	const std::vector<CommentAttach> &GetLeading(const TSNode Node) const; // 先行コメント取得関数
	const std::vector<CommentAttach> &GetTrailing(const TSNode Node) const; // 後続コメント取得関数
	bool HasAttachedCommentInRange(const uint32_t From, const uint32_t To) const; // 範囲内紐付コメント有無の判定関数
	TSTree *GetMutableTree(); // 可変構文木取得関数
	void CountTokenBytes(); // 葉の字句の長さの和の算出関数
	void SetTreeEdited(const bool IsEdited); // 構文木編集済の設定関数
	void Assign(const std::string &NewSource); // ソース複製代入関数
	void Assign(std::string &&NewSource); // ソース移動代入関数
	void AttachComments(const bool IsJsxSource, const bool IsScssSource, const char DocMarker); // コメント紐付関数
	void ClearAttachments(); // 紐付状態の全消去関数
	std::vector<CommentAttach> TakeLeading(const TSNode Node); // 先行コメント取出除去関数
	std::vector<CommentAttach> TakeTrailing(const TSNode Node); // 後続コメント取出除去関数
	void AddLeading(const TSNode Node, std::vector<CommentAttach> Comments); // 先行コメント追加関数
	void AddTrailing(const TSNode Node, std::vector<CommentAttach> Comments); // 後続コメント追加関数
	void SuspendAttachments(); // 紐付停止区間の開始関数
	void ResumeAttachments(); // 紐付停止区間の終了関数
	void CollectMacros(); // 前処理マクロの性質（実引数の文字列化・文や宣言子の記号への展開）の収集関数
	void CollectDeclaredNames(); // 宣言した名前と取込の指令の位置の収集関数 (C / C++)

	TSSource(std::string Str); // コンストラクタ
	TSSource(std::string Str, const TSLanguage *const Lang); // コンストラクタ（言語指定）
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
	// 貯留が無い場合は新規生成して返戻
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
		// 貯留庫への積み直しが失敗した場合は例外を出さずに解放する
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
			// 名前付限定版は無名トークン（`{`, `,` 等）の読飛し
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
 * 指定型に一致する最初の名前付子の取得関数
 * 子数 C と型名比較の総バイト数 B に対し O(C + B)
 * @param Node 走査対象ノード
 * @param Type 探す子のノード型名
 * @return 最初に一致した名前付子（不在時は空ノード）
 */
inline TSNode FirstNamedChildOfType(const TSNode Node, const std::string_view Type) {
	// 一致節点の格納域の初期化
	TSNode Found {};
	ForEachNamedChild(
		Node,
		[&](const TSNode Child) -> bool {
			// 指定型に一致する名前付子の判定
			const bool IsMatch = std::string_view(ts_node_type(Child)) == Type;
			if(IsMatch) Found = Child;
			// 一致時は打切り，未発見なら次の兄弟へ進む事の返戻
			return !IsMatch;
		}
	);
	// 一致した子の返戻
	return Found;
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
 * @param Func 各ノードに適用し内部へ降りるか否かを返す関数（節点と親を受ける形なら親も渡す）
 */
template<typename Fn> inline void WalkChildrenCursor(const TSNode Root, Fn &&Func) {
	// 親配列を降下・上昇と共に更新し，ts_node_parent の二乗走査を避ける
	constexpr bool WantsParent = std::is_invocable_r_v<bool, Fn, TSNode, TSNode>;
	std::vector<TSNode> Parents;
	HeldCursor Cursor(Root);
	// 根直下からの先行順走査
	if(ts_tree_cursor_goto_first_child(&Cursor)) {
		if constexpr(WantsParent) Parents.push_back(Root);
		for(bool IsDone = false; !IsDone;) {
			const TSNode Node = ts_tree_cursor_current_node(&Cursor);
			// コールバック形に応じた親情報の供給
			bool ShouldDescend;
			if constexpr(WantsParent) ShouldDescend = Func(Node, Parents.back());
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

TSTree *ParseWithDeadline(TSParser *const Parser, const TSTree *const Old, const std::string_view Text, bool &OutIsExpired); // 期限付構文解析関数

/**
 * 名前付節点の型の名前取得関数
 * 同名の無名字句を除き名前付節点だけを型名で判定する
 * @param Node 対象節点（子や兄弟を引いた結果の空節点も受ける）
 * @return 名前付節点の型の名前（空節点と無名の字句は空のビュー）
 */
inline std::string_view NamedTypeOf(const TSNode Node) {
	// 空節点は型を持たず，無名の字句は構文を成さない事の返戻
	return !ts_node_is_null(Node) && ts_node_is_named(Node) ? std::string_view(ts_node_type(Node)) : std::string_view();
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

// 走脈毎の相互再帰深度を数え，上限後は新しい走脈で続ける守衛
struct RecursionGuard {
	static constexpr uint32_t MaxDepth = 512; // １つの走脈で続ける再帰深度の上限
	static inline thread_local uint32_t Depth = 0; // 現在の再帰深度（走脈毎に独立）
	bool IsOverflow; // 深度上限超過フラグ

	/**
	 * コンストラクタ（深度を進め上限超過を記録する）
	 */
	inline RecursionGuard() : IsOverflow(++Depth > MaxDepth) {}

	/**
	 * デストラクタ（深度を戻す）
	 */
	inline ~RecursionGuard() {
		// 再帰深度の復元
		--Depth;
		// 終了
		return;
	}
};

/**
 * 識別子構成文字の判定関数
 * @param Char 判定対象の文字
 * @return 英数字又は下線なら true
 */
inline bool IsIdentifierChar(const char Char) {
	// 識別子を構成し得る文字かの返戻
	return std::isalnum(static_cast<unsigned char>(Char)) || Char == '_';
}

/**
 * 語を構成する文字の判定関数
 * @param Char 判定対象の文字
 * @return 英数字・下線・`$`・UTF-8 の多バイト文字のバイトなら true
 */
inline bool IsWordChar(const char Char) {
	// 隣り合うと一続きの語に為る文字かの返戻
	return IsIdentifierChar(Char) || Char == '$' || static_cast<unsigned char>(Char) > 0X7F;
}

/**
 * 閉じ括弧文字の判定関数
 * @param Char 判定対象の文字
 * @return 閉じ括弧文字 (`)` `]` `}`) なら true
 */
inline bool IsCloseBracketChar(const char Char) {
	// 閉じ括弧の判定の返戻
	return Char == ')' || Char == ']' || Char == '}';
}

/**
 * 開き括弧文字の判定関数
 * @param Char 判定対象の文字
 * @return 開き括弧文字 (`(` `[` `{`) なら true
 */
inline bool IsOpenBracketChar(const char Char) {
	// 開き括弧の判定の返戻
	return Char == '(' || Char == '[' || Char == '{';
}

/**
 * 範囲末尾が `end` キーワードかの判定関数
 * Ruby はブロックを `end` で閉じる為，波括弧言語の閉じ括弧と同じ位置付けで扱う
 * @param Source ソースコード
 * @param Start 範囲の開始バイト位置
 * @param End 範囲の終了バイト位置
 * @return 末尾が語の区切を伴う `end` なら true
 */
inline bool EndsWithEndKeyword(const std::string &Source, const uint32_t Start, const uint32_t End) {
	// `append` 等の語尾一致を除く為，直前が識別子構成文字でない事も併せて見る事の返戻
	return End >= Start + 3 && End <= Source.size() && !Source.compare(End - 3, 3, "end") &&
	(End == 3 || !IsIdentifierChar(Source[End - 4]));
}

/**
 * 述語を満たす祖先の有無の判定関数
 * 計算量：祖先深度 D と述語の総費用 F に対し最悪 O(D ² + F)，通常 O(D + F)
 * @param Node 走査開始ノード（祖先は親からルートへ辿り自身は含まない）
 * @param Pred ノードを受け取り bool を返す述語
 * @return 述語を満たす祖先が見付かれば true
 */
template<typename Predicate> inline bool HasAncestorOf(const TSNode Node, Predicate Pred) {
	// 空節点は位置も木も持たない事の返戻
	if(ts_node_is_null(Node)) return false;
	const uint32_t Start = ts_node_start_byte(Node);
	// 幅の有無に応じた祖先探索方式の選択
	// 幅の無い節点は開始位置で降りる先を定めれない為，親を辿る
	if(ts_node_end_byte(Node) == Start) {
		// 親鎖の直接走査
		// 述語を満たす祖先が有る事の返戻
		for(TSNode Cur = ts_node_parent(Node); !ts_node_is_null(Cur); Cur = ts_node_parent(Cur)) if(Pred(Cur)) return true;
		// 祖先に述語充足無の返戻
		return false;
	}
	// 根から一度だけ降り，祖先毎の ts_node_parent に因る二乗走査を避ける
	HeldCursor Cursor(ts_tree_root_node(Node.tree));
	bool IsFound = false;
	// 対象位置へ至る経路上の述語照合
	for(TSNode Cur = ts_tree_cursor_current_node(&Cursor); !ts_node_eq(Cur, Node); Cur = ts_tree_cursor_current_node(&Cursor)) {
		IsFound = Pred(Cur);
		if(IsFound || ts_tree_cursor_goto_first_child_for_byte(&Cursor, Start) < 0) break;
	}
	// 祖先に述語充足が有るかの返戻
	return IsFound;
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
		} while(ts_tree_cursor_goto_next_sibling(&Cursor));
	}
	// 直接子の述語充足有無の返戻
	return IsFound;
}

/**
 * 述語を満たす直接子又は孫の有無の判定関数
 * 走査する子・孫の総数 V と述語の総費用 F に対し O(V + F)
 * @param Node 走査開始ノード
 * @param Pred ノードを受け取り bool を返す述語
 * @return 子又は孫に述語充足が有れば true
 */
template<typename Predicate> inline bool HasChildOrGrandchildOf(const TSNode Node, Predicate Pred) {
	// 子層／孫層共に HasChildOf へ委譲しカーソル管理の二重記述を避ける事の返戻
	return HasChildOf(
		Node,
		[&Pred](const TSNode Child) -> bool {
			// 子又は孫の述語充足判定
			return Pred(Child) || HasChildOf(Child, Pred);
		}
	);
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
		// 現在節点への祖先列付き述語適用
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
