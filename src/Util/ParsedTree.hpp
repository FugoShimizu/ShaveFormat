#pragma once

#include "FormatLimitExceeded.hpp"
#include "ParserAllocator.hpp"
#include "SyntaxParser.hpp"
#include "../Core/CommentRepository.hpp"
#include "../Core/SyntaxModel.hpp"
#include <tree_sitter/api.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
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

enum class BoolNameSource : uint8_t; // 読取規則が定義する真偽値名の判定の供給元

// パーサーの読違を避ける為に解析用の入力から退避したコメント（所属と配置はコメント経路が決める，整形仕様 4.2, 7.6）
struct EscapedComment {
	uint32_t Pos; // 退避した位置（現在の本文の位置で，所属する構文の端）
	std::string Text; // コメントのテキスト
	CommentKind Kind; // 原文での種類
	std::optional<std::pair<uint32_t, uint32_t>> InputRange = std::nullopt; // 退避前の入力上のコメント範囲（所属の位置とは別に保つ）
};

// 解析用の修復が本文へ加えた編集（位置を修復前の本文へ戻す為に，適用前と適用後の範囲を持つ）
struct AppliedEdit {
	uint32_t Start; // 適用前の本文の始まり
	uint32_t End; // 適用前の本文の終わり
	uint32_t NewStart; // 適用後の本文の始まり
	uint32_t NewEnd; // 適用後の本文の終わり
};

using AppliedEditBatches = std::vector<std::vector<AppliedEdit>>; // 適用順の編集の組

// 解析用に別のテキストへ置き換えたトークンのデコードデータ
// 置換後のトークンの範囲
struct LexemeRestoration {
	uint32_t Start;
	uint32_t End;
	std::string Original; // 原文のテキスト
	bool HasKindRestoration = true; // テキストだけでなくトークンの種類も戻す置換か
	bool IsIdentifier = false; // 元の綴りを演算子でなく識別子として戻すか
};

enum class ParseIssue : uint8_t { None, InputSyntax, UnrecognizedSyntax }; // 入力エラーが確定した破損と未識別構文の区分
enum class ParseDecision : uint8_t { Continue, Fail, Skip }; // 入力の適応と受理の区分（継続・見送・失敗）

// 木の世代で１度読む直接子の構文情報
struct ParsedChild {
	TSNode Node; // 子のノード
	TSFieldId Field; // 親の中のフィールド
	TSSymbol Symbol; // 記号別の固定定義のキー
	uint32_t Relation; // 同じ木の親子表の記録番号
	uint64_t Fields; // 最内と隠れた外側のフィールドの役割
	uint32_t End; // 同じ木での終了バイト
	bool IsNamed; // 名前付の構文か
	bool IsMissing; // 必須トークンの欠落か
	bool HasError; // 子孫を含むエラーの有無

	/**
	 * ノードに保持した開始バイトの取得関数
	 * 計算量：O(1)
	 * @return 同じ木での開始バイト
	 */
	inline uint32_t GetStart() const {
		// パーサーのノードに含まれる開始位置の返戻
		return ts_node_start_byte(Node);
	}
};

// 解析用の本文と構文木を所有し，受理の判定を併せて持つ解析結果（整形仕様 3.3 の ParsedTree）
class ParsedTree : public std::string {
private:

	friend class ParseAdapter; // 入力の適応は解析用のマーカーと其の控えを本文と共に更新する為，内側の状態へ触れる
	friend class SyntaxParser; // 受理の判定だけが解析結果の区分を記録する
	friend struct ParsedTreeAccess; // コメントの共通走査の作業量を検査から観測する
	friend class Core::SourceProjector; // 原文を変更せず投影用のフィールドを確定し，コメントの範囲・種類と解析用の控えを読む為，内側の状態へ触れる

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

	using BoolNameSource = ::BoolNameSource; // 読取規則と共有する真偽値名の判定の供給元

	enum class MacroProp : uint8_t { Declarator = 1, Expr = 2, Stmt = 4, Stringizing = 8 }; // マクロの展開の性質（独立したビット）
	enum class MacroState { Undefined, Cancelled, Defined, Unknown }; // 前処理で確定する名前の状態

	// 式と読まれた括弧付宣言の確定した構成（名前の登録と中核への投影が共有する）
	struct DeclaratorPart {
		TSNode Node {}; // 宣言子へ読み直す入力の部品
		Core::Role Shape = Core::Role::Identifier; // 部品の宣言子の種別
	};

	// 原文順のコメントの範囲と，其の前後の並びから決まる状態（コメントの種類の判定を鎖の走査無で引く為，木の世代毎に１度作る）
	struct CommentSpan {
		uint32_t Start; // コメントの始まり
		uint32_t End; // 末尾の改行を除いた終わり
		uint32_t RunEnd; // 後の空白と，空白だけを挟んで続くコメントを飛ばした位置（行末の判定に使う）
		bool IsLineHead; // 行の前が行頭か，同じ行の前のコメントだけか
	};

	// ノードと隙間のコメントを同じ世代の１度の走査で収集した事実
	struct CommentFacts {
		struct Range {
			uint32_t Start; // 本文の始まり
			uint32_t End; // 本文の終わり
			uint32_t At; // 本体内かを判断する位置（コメントノードは其の始まり）
			bool IsGap; // 本体の終わりで切る隙間のコメントか
		};

		std::pmr::monotonic_buffer_resource Storage; // 此の世代のコメントの表の共有メモリ
		std::pmr::vector<Range> Ranges { &Storage }; // ノード・マクロ・隙間のコメントの範囲
		std::pmr::vector<TSNode> Nodes { &Storage }; // 原文順のコメントノード
		std::pmr::unordered_set<const void *> Parents { std::pmr::polymorphic_allocator<const void *>(&Storage) }; // コメントノードを子孫に持つ祖先（`&Storage` は要素の型へ変換される為にアロケーターを明示）
		std::pmr::unordered_map<const void *, TSNode> Following { &Storage }; // 同親の後続の名前付非コメント子（無ければ空）
		size_t Visits = 0; // 共通走査が訪ねたノード数（構造の予算から独立した観測値）
	};

	// 構文木の削除子（解放を所有者へ任せ，ムーブ・破棄を既定の物にする）
	struct TreeDeleter {
		void operator()(TSTree *const Target) const; // 構文木解放関数
	};

	// 括弧付宣言子の導入名と初期化子の部品
	struct DeclaratorItem {
		TSNode Expr {}; // 初期化子を含む要素の全体
		TSNode Root {}; // 初期化子を除いた宣言子
		TSNode Name {}; // 導入名
		TSNode Val {}; // 初期化子（無ければ無）
		std::vector<DeclaratorPart> Parts {}; // 外側からの宣言子の部品
	};

	// 親と，世代の子の列の直接の範囲
	struct NodeRelation {
		uint32_t Parent = UINT32_MAX; // 直接親の記録番号（根は番兵）
		uint32_t Begin = 0; // 子の列の先頭
		uint32_t Count = 0; // 直接の子の数
		uint32_t Slot = 0; // 親の子列内での此のノードの格納先
		uint32_t FieldsBegin = 0; // フィールド毎の最初の子の格納先
		uint16_t FieldsCount = 0; // 記録した非空のフィールド数（文法のフィールド番号の種類数以下）
		bool HasFieldsReady : 1; // 隠れた外側のフィールドを補完済か
		bool HasInnerFields : 1; // カーソルの最内のフィールドを持つ直接子が在るか
		bool HasNameWork : 1; // 宣言又は所属の収集を要するノードを部分木に含むか
		uint32_t FirstNamedSlot = UINT32_MAX; // 名前付の最初の直接子の格納先（無ければ番兵）

		NodeRelation(const uint32_t Parent, const uint32_t Begin, const uint32_t Count, const uint32_t Slot = 0) noexcept; // 直接子の範囲の記録の生成関数
		NodeRelation() noexcept; // 空の記録の生成関数
	};

	// 識別子の平坦なインデックスと前順の記録（削除・再ハッシュは世代内で行わない）
	struct ParentTable {
		std::vector<uint32_t> Slots; // 子の格納先＋１（根は０，空は番兵），使用率は２分の１以下
		std::vector<NodeRelation> Records; // 前順の番号で読む親と子の範囲
		const void *RootId = nullptr; // 子列に属さない根の識別子
		std::span<const ParsedChild> Children; // 記録済の子が保持する識別子の参照先
		size_t Insertions = 0; // 重複を除いたノードの挿入数
		size_t InsertProbes = 0; // 挿入で読んだインデックスのスロット数
		size_t GetSlot(const void *const Id, size_t *const Work = nullptr) const; // 識別子又は空のスロットの探索関数
		const NodeRelation *Find(const void *const Id) const; // 記録の読取専用の取得関数
		NodeRelation *Find(const void *const Id); // 構築中の記録の取得関数
		void Reserve(const size_t Count); // 世代のノード数に合わせた２列の確保関数
		size_t Add(const void *const Id, NodeRelation Record); // 前順の記録とインデックスの追加関数
	};

	// 修復と意味の照合に使うノードだけの，親子表の番号と用途
	struct SemanticNode {
		uint32_t Relation; // 同じ世代の前順の記録の番号
		bool IsFunction : 1; // 関数の種類を照合するか
		bool IsDirective : 1; // 空のディレクティブを照合するか
		bool IsHeader : 1; // 前処理の見出の終端を照合するか
		bool IsOperand : 1; // 後置の式との結付を照合するか
		bool IsBody : 1; // 空の本体を照合するか
		bool IsName : 1; // 被演算子内の名前を数えるか
		bool IsPostfix : 1; // 後置の式を結び直すか
	};

	// フィールドの最初の子は，既存の記録か異なる解析文脈を持つノードだけの列を引く
	struct FieldChild {
		TSFieldId Field; // 文法のフィールド
		bool IsStored; // 親子表と同じ解析文脈か
		uint32_t Index; // 親子表又は別の文脈の列の位置
	};

	// 木の世代に属する親・子・トークン・許容エラーの表（列の伸長では旧領域を解放する）
	struct NodeRelations {
		std::pmr::monotonic_buffer_resource Storage; // 許容エラーの集合の要素とバケットの共有メモリ
		std::vector<uint32_t> LeafEnds; // 終端の昇順に並ぶ長さの在る葉の子列内の位置
		ParentTable Parents; // 直接親の前順の記録と識別子の平坦なインデックス
		std::vector<SemanticNode> SemanticNodes; // 共通走査で選んだ修復と意味の照合の候補（前順）
		ParsedChild Root {}; // 子列に属さない根の同じ形式の記録
		std::unique_ptr<ParsedChild[]> ChildData; // 木のノード数で１度だけ確保する直接子の所有権
		std::span<ParsedChild> Children; // 親毎の直接子（原文順，所有する列の記録済の範囲）
		std::vector<std::pair<TSFieldId, uint64_t>> SemanticFields; // 意味を持つ文法のフィールドと役割
		std::vector<FieldChild> FirstFields; // 親毎のフィールドの最初の子の番号（フィールド順）
		std::vector<TSNode> FieldNodes; // 親子表と異なる解析文脈を持つフィールドの子だけの保持
		size_t FieldWork = 0; // 隠れたフィールドの補完でパーサーへ照会した回数
		size_t ReadWork = 0; // カーソルで直接子の情報を読んだ回数
		size_t SymbolReads = 0; // 共通走査で子の記号をパーサーへ照会した回数
		std::vector<uint32_t> DeclCandidates; // 原文順の曖昧な宣言と括弧付宣言の記録番号
		std::vector<uint32_t> FunctionDefCandidates; // 関数宣言子の成立を確認する定義の前順の記録番号
		bool HasFunctionDefFacts = false; // 同じ木と前処理から定義の成立条件を収集済か
		uint32_t MalformedFunctionDefs = 0; // 宣言子を確認出来ない関数定義の個数
		std::optional<uint32_t> MalformedFunctionDef; // 不正な関数定義の最初の位置
		std::optional<uint32_t> UncertainFunctionDef; // 前処理が未確定の関数定義の最初の位置
		std::vector<uint32_t> SoftKeywordCandidates; // 型の別名の文と読まれた最外の候補の記録番号
		size_t DeclWork = 0; // 修復判定で候補を訪ねた回数
		size_t BoolWork = 0; // 真偽値名の遮蔽の専用走査の訪問数
		size_t JsxWork = 0; // JSX の表示本文の専用走査の訪問数
		size_t MacroWork = 0; // 前処理の状態の専用走査の訪問数
		std::optional<uint32_t> EmptyBlock; // 最初の幅０のインデントの本体
		std::optional<uint32_t> ArrowOperand; // 最初の括弧無のアロー関数の被演算子
		std::optional<uint32_t> ContinuationComment; // 最初の行継続の直後のコメント
		std::optional<uint32_t> RejectedTerminator; // 自動の文終端が構文の内部へ入る最初の位置
		std::optional<uint32_t> RejectedSpace; // 最初の処理系が拒むトークン間の空白
		std::optional<uint32_t> ExtraRootVal; // 単一の値を要求する文法の２個目の値
		std::optional<uint32_t> RejectedVal; // 最初の処理系が拒む小数部又は文字列の位置
		int OpenDepth = 0; // エラーを持つ木の根を除く括弧の開閉数の差
		std::vector<size_t> AdmissionLeaves; // 文の改行判定で使う原文順の葉の格納先
		size_t AdmissionLeafWork = 0; // 文の改行判定で記録済の葉を読んだ回数
		size_t TerminatorGapWork = 0; // 自動の文終端の入力受理でトークン間を読んだバイト数（中核の予算と分ける）
		size_t IndentationWork = 0; // 原文の隙間と本体の深さの入力受理での読取量（中核の予算と分ける）
		size_t AdmissionWork = 0; // 共通走査で受理の事実を調べたノード数
		size_t NameChildQueries = 0; // 宣言収集で記録済の直接子の範囲を照会した回数
		size_t NameLeafSkips = 0; // 宣言収集で子列の照会を省いた葉の数
		size_t NameVisits = 0; // 記録済の子から宣言の有効範囲を継承した訪問数
		std::vector<uint8_t> MacroSubtrees; // 子の格納先毎の前処理の状態を持つ部分木か（前処理の言語だけ）
		bool HasMacroCandidates = false; // 前処理の状態を変更するノードが在るか
		bool HasBoolCandidates = false; // 真偽値名の遮蔽の候補が在るか
		bool HasJsxCandidates = false; // JSX の表示本文の並びが在るか
		std::pmr::unordered_set<const void *> ModuleErrors { std::pmr::polymorphic_allocator<const void *>(&Storage) }; // 許容するモジュールのエラー（`&Storage` は要素の型へ変換される為にアロケーターを明示）
		std::pmr::unordered_set<const void *> UrlValErrors { std::pmr::polymorphic_allocator<const void *>(&Storage) }; // 許容する URL の欠落（`&Storage` は要素の型へ変換される為にアロケーターを明示）
	};

	// 本文に対応する構文木と其の控え（差替は `ReplaceTree` へ集約する）
	struct SyntaxState {
		TSNode CachedRoot {}; // ルートノードのキャッシュ
		bool HasCachedRoot = false; // ルートキャッシュ有効フラグ
		std::vector<CommentSpan> CommentSpans; // 原文順のコメントの範囲の表（コメントの種類の判定で二分探索する）
		std::unique_ptr<CommentFacts> Comments; // 移動後も共有メモリのアドレスを保つ同じ世代のコメントの事実
		bool HasMacroStates = false; // 此の世代のマクロの状態を収集済か
		bool IsCollectingNames = false; // 位置順の束縛の収集中か
		bool HasNameBindings = false; // 此の世代の宣言と所属の位置インデックスを収集済か
		std::unique_ptr<NodeRelations> Relations; // 移動後も共有メモリのアドレスを保つ表の所有権
		bool HasParents = false; // 直接親の表を作ったか
		bool HasCommentNodes = false; // コメントのノードの範囲を共有走査で収集済か
		bool HasCommentSpans = false; // コメントの位置表を作ったか
		std::vector<std::pair<uint32_t, uint32_t>> BlankLines; // 空白だけを挟む改行の対（原文順）
		std::optional<SyntaxParser> Parser; // 指定文法に依るパーサー（整形仕様 3.1）
		std::unique_ptr<TSTree, TreeDeleter> Tree; // 解析済構文木
		bool IsDirty = false; // 未反映編集の有無
		bool IsTreeEdited = false; // 構文木編集済フラグ
		bool IsParseExpired = false; // 解析が時間の上限で打ち切られたか（言語の設定の失敗と区別する）
	};

	// 式と読まれた括弧付宣言の構成
	struct ParenthesizedDecl {
		TSNode Stmt {}; // 読み違えた式文
		TSNode Type {}; // 共有する型
		TSNode Call {}; // 型への呼出と読まれたラッパー（ポインタ・参照で始まる場合は無）
		TSNode Args {}; // 型への呼出で最初の括弧付宣言子（ポインタ・参照で始まる場合は無）
		std::vector<DeclaratorItem> Items {}; // 原文順の全宣言子
		std::vector<TSNode> Commas {}; // 原文順の区切
		std::vector<TSNode> Wrappers {}; // 宣言へ戻すカンマ式のラッパー
	};

	struct TemplateSymbols {
		TSSymbol Arguments; // 型引数の並び
		TSSymbol Binary; // 読違を修復する文法の二項式
		TSSymbol Declaration; // 型付の宣言
		TSSymbol Identifier; // 宣言子の名前
		TSSymbol NamespaceName; // 修飾先の名前
		TSSymbol Qualified; // 修飾付の名前
		TSSymbol Template; // 型のテンプレートの適用
		TSSymbol TypeName; // 型名のトークン
	}; // 同じ文法で共有する修復先の記号

	struct TemplatePart {
		TSSymbol Symbol; // 確定した型又はトークンの文法の記号
		uint32_t Start; // 原文の始まり
		uint32_t End; // 原文の終わり
		uint32_t Parent; // 投影の計画内の親（先行順，最初は UINT32_MAX）
		uint64_t Fields; // 親の中のフィールド
		bool IsLeaf; // トークンとして本文を持つか
	}; // 束縛と山括弧の対応から確定した宣言の投影計画

	// 所有する名前と本文のビューを同じ値で引く為のハッシュ
	struct NameHash {
		using is_transparent = void; // 標準コンテナの異種検索を有効にする型名

		size_t operator()(const std::string_view Name) const noexcept; // 名前のハッシュ算出関数
	};

	// 木の世代の名前を１度だけ所有し，本文と木の移動から寿命を分離する共有メモリ
	struct NameTexts {
		std::pmr::monotonic_buffer_resource Storage; // 文字列・集合の要素・バケットの共有メモリ
		std::pmr::unordered_set<std::pmr::string, NameHash, std::equal_to<>> Values { &Storage }; // 異なる綴り毎の所有名
	};

	// 使用位置で値と型を共通に解決する宣言の束縛
	struct NameBinding {
		uint32_t Pos; // 宣言の有効位置
		uint32_t End; // 有効範囲の終端
		bool IsType; // 型の名前か
		size_t Namespace; // クラス・列挙型又は名前空間の所属の識別番号
		bool IsKnown = true; // 区分を確定出来る束縛か
		bool IsNamespace = false; // 名前空間の名前か
		bool IsMember = false; // 所属の外から導入出来る宣言か
		size_t Target = std::string::npos; // 名前が指すクラス・列挙型又は名前空間の所属
		uint32_t Origin = 0; // 導入前の宣言位置（同じ宣言の複数の導入を区別しない）
		bool IsTemplate = false; // 型引数を受ける型の宣言か
	};

	// 宣言位置順の束縛と，区間毎の有効範囲の終端の最大値
	struct NameIndex {
		std::pmr::vector<NameBinding> Bindings; // 同名の値と型の宣言（有効位置順）
		std::pmr::vector<uint32_t> MaxEnds; // 完全二分木の区間毎の終端の最大値
		size_t Capacity = 1; // 葉の数（２の冪）

		const NameBinding *Find(
			const uint32_t Use,
			Core::StructureWorkAccount *const Work = nullptr,
			uint32_t *const ValidUntil = nullptr
		) const; // 使用位置で有効な束縛の取得と作業量の計数関数

		void Add(const NameBinding Binding); // 宣言の追加と区間インデックスの更新関数

		explicit NameIndex(std::pmr::memory_resource *const Storage); // 区間の列を世代の共有メモリへ置く生成関数
	};

	// 名前の解決結果と，其の参照元の所属・導入の階層・有効区間の終端
	struct NameResolution {
		const NameBinding *Binding = nullptr; // 世代内の束縛（無ければ無）
		size_t Scope = 0; // 束縛を取得した所属
		size_t Tier = 0; // 導入を比較する階層
		uint32_t End = UINT32_MAX; // 此の解決結果が変わり得る次の使用位置
	};

	// 名前の無い所属を飛ばす共有結果と，使用元で確定した最寄の束縛
	struct NameCache {
		using Results = std::map<uint32_t, NameResolution>; // 最初に解決した使用位置順の共有結果

		Results Inherited; // 外側で最初に名前を持つ所属
		Results Resolved; // 遮蔽・導入の競合を照合済の束縛
		static const NameResolution *Find(const Results &Table, const uint32_t Use); // 使用位置で共有出来る解決結果の取得関数
	};

	// 名前空間・クラス・列挙型の同一性と所属する宣言
	struct NamespaceBindings {
		mutable bool HasRoutes = false; // 導入経路の初回構築を終えたか
		mutable std::vector<NameBinding> PendingRoutes; // 未反映の導入経路だけ
		mutable std::unordered_map<size_t, std::map<uint32_t, uint32_t>> RouteWindows; // 導入先毎の被覆済の区間
		mutable std::vector<size_t> RouteUsers; // 此の所属へ達する構築済の経路の所有者
		mutable std::pmr::unordered_map<size_t, NameIndex> Routes; // 共有する導入先毎の有効範囲
		mutable NameIndex UnknownRoutes; // 未確定の導入先へ達する共有の有効範囲
		mutable std::pmr::unordered_map<std::string_view, NameCache> NameResults; // 名前と使用区間毎の解決結果
		size_t Parent = 0; // 外側の名前空間の識別番号
		size_t Depth = 0; // 所属のネストの深さ
		bool IsClass = false; // クラスの所属か
		bool IsEnum = false; // 列挙型の所属か
		bool HasEnumBody = false; // 列挙子を確定する本体を持つか
		bool IsUnresolved = false; // 限定名の所属を確定出来ない本体か
		bool HasInline = false; // 内側のインライン名前空間を暗黙に導入するか
		uint32_t Start = 0; // 完全なクラスのメンバを解決する開始位置
		std::pmr::unordered_map<std::string_view, NameIndex> Names; // 名前毎の宣言インデックス
		std::pmr::unordered_map<std::string_view, NameIndex> Members; // 外側から導入する名前のインデックス
		std::pmr::unordered_map<std::string_view, size_t> Children; // 同名の内側の名前空間
		std::vector<NameBinding> Imports; // 導入位置・終端・導入先の所属
		NameIndex UnknownImports; // 導入先を確定出来ないディレクティブの有効範囲
		NameIndex UnknownEnumerators; // 導入名を確定出来ない列挙子の有効範囲

		explicit NamespaceBindings(std::pmr::memory_resource *const Storage); // 世代の共有メモリを名前の表へ渡す生成関数
	};

	struct MacroArgFlow {
		uint32_t From; // 値の流れる元の仮引数又は実引数ノード
		uint32_t To; // 流れる先のノード又は呼出先の仮引数
		std::string_view Callee; // 外の呼出先（空なら同じ定義の内側）
	}; // 文字列化へ流れるトークンの共有する辺

	struct StringizingArgs {
		std::vector<uint32_t> Params; // 文字列化される固定の仮引数の番号（昇順）
		uint32_t VariadicFrom = UINT32_MAX; // 文字列化される可変実引数の開始番号
		uint32_t Start = 0; // 同じ入力事実を共有出来る区間の開始
		uint32_t Limit = UINT32_MAX; // 参照した名前の次の変更位置
	}; // 使用位置で確定した文字列化の対象

	// 定義毎の直接の性質と，使用位置で解決する参照先
	struct MacroBody {
		std::string Signature; // 仮引数と置換トークンの一致を判定する記録
		uint8_t Props = 0; // 本体から直接確定した性質
		bool IsFunctionLike = false; // 仮引数へ束縛する定義か
		uint32_t ParamCount = 0; // 仮引数の数
		uint32_t VariadicParam = UINT32_MAX; // 可変仮引数の番号
		uint32_t ArgNodes = 0; // 仮引数と呼出の実引数ノードの総数
		std::string_view Alias; // 実引数の番号を其の儘渡す別名
		std::vector<MacroArgFlow> ArgFlows; // 元の仮引数・入子・呼出先の束縛の辺
		std::vector<uint32_t> Stringized; // 直接文字列化する仮引数
		std::vector<std::pair<std::string_view, uint8_t>> Refs; // 性質を引き継ぐ名前とビット
		std::array<std::shared_ptr<const MacroBody>, 2> Alternatives {}; // 条件枝の合流で残る定義
	};

	// 使用位置で照会するマクロの定義
	struct MacroDef {
		uint32_t Start; // 定義の位置
		uint32_t Epoch; // 未読のインクルードを区別する世代
		MacroState State; // 定義・未確定・未定義・取消済
		std::string_view Alias; // 単一トークンの置換本体（其の他は空）
		bool IsFunctionLike; // 関数形式か
		std::shared_ptr<const MacroBody> Body {}; // 使用位置の状態に属する置換本体
		bool IsVersionedBool = false; // 標準ヘッダの真偽値名で，マクロとして定義されるかは C のバージョンに依るか
		MacroState InputState = MacroState::Undefined; // 入力の定義・取消と条件枝だけで決まる呼出の置換状態
	};

	// 置換本体のトークン（本文のテキストを参照する）
	struct MacroToken {
		std::string_view Text; // トークンの本文
		bool IsIdentifier; // 名前に為れるトークンか
		bool IsOpaque; // 引用領域か
	};

	// 名前の所有メモリと其のビューを使う全ての表を，同じ世代として移動・破棄する
	struct NameTables {
		NameTexts Texts; // 表より先に作り，表を破棄した後に解放する共有メモリ
		std::vector<NamespaceBindings> Bindings; // 値と型を共通に解決する所属毎の表
		NameIndex Regions { &Texts.Storage }; // 所属の領域の区間インデックス
		std::pmr::unordered_map<std::string_view, std::vector<size_t>> MemberOwners { &Texts.Storage }; // 名前を導入する所属
		bool HasUnknownImports = false; // 未確定の導入先を持つ関係が在るか
		std::pmr::unordered_map<std::string_view, std::vector<MacroDef>> MacroDefs { &Texts.Storage }; // 原文順の定義と別名
		std::vector<std::pair<uint32_t, uint32_t>> MacroEpochs; // 位置毎のインクルード世代
		std::unordered_map<std::string_view, std::vector<StringizingArgs>> Stringizing; // 名前と有効区間毎の共有する文字列化の対象
	};

	// 同じ木を読む並列区間だけで，遅延構築と共有計数の変更を止める
	struct NameReadGuard {
		const bool HasOuterReader; // 入る前の並列照会の状態
		const ParsedTree &Src; // 読む木

		NameReadGuard &operator=(const NameReadGuard &) = delete; // 区間の所有権を代入しない

		explicit NameReadGuard(const ParsedTree &Src); // 名前の表を確定して読取専用の区間へ入る生成関数

		NameReadGuard(const NameReadGuard &) = delete; // 区間の所有権を複製しない

		~NameReadGuard(); // 合流又は例外の後に元の計数の状態を戻す破棄関数
	};

	mutable SyntaxState Syntax; // 本文に従属する解析状態
	mutable AppliedEditBatches PreparationBatches; // 単調な準備の編集を纏めた逆マッピング
	mutable bool IsReadingNamesInParallel = false; // 共有木の並列照会中は作業計数を変更しない
	mutable Core::StructureWorkAccount NameWork; // 中核の予算へ渡す束縛の区間探索と導入関係の累積作業量
	mutable std::vector<ParenthesizedDecl> ReboundDecls {}; // 入力の読違から確定した宣言
	mutable std::unordered_map<const void *, std::vector<TemplatePart>> TemplateDecls {}; // 同じ世代の束縛で確定した型引数付宣言
	mutable std::optional<bool> HasUnrepairedInputError; // 束縛で修復した宣言を除く入力エラーの共有結果
	mutable std::optional<TemplateSymbols> TemplateKinds; // 木の世代で共有する修復先の文法の記号
	mutable std::unique_ptr<NameTables> NameData; // 宣言を読む時だけ作る世代の所有権（移動後もビューを保つ）
	mutable std::array<std::vector<std::pair<uint32_t, uint32_t>>, 2> BoolShadows; // 共通走査で確定する真偽値名の宣言位置と遮蔽の有効範囲（名前毎の半開区間）
	Core::StructureWorkAccount PrefixTypeRepairWork; // 中核の予算へ渡す前置型の修復の累積作業量
	Core::StructureWorkAccount BoundaryRepairWork; // 中核の予算へ渡す文境界修復の累積作業量
	Core::StructureWorkAccount CastRepairWork; // 中核の予算へ渡すキャスト修復の候補の累積訪問回数
	const GrammarProfile *Grammar = nullptr; // 解析に用いる文法の定義（言語の指定が無ければ無い）
	size_t HaltedData = std::string::npos; // 実行停止後の逐語データの開始位置（無ければ無い）
	std::vector<uint32_t> InsertedBreaks; // 解析用に補った文の区切（Kotlin の `;` と改行を含むブロックコメントの後の改行）の現在の本文位置（昇順）
	std::vector<std::pair<uint32_t, uint32_t>> SyntheticGroups; // 解析用に挿入した囲みの開きと閉じの現在の本文位置の組（開きの順）
	std::vector<LexemeRestoration> LexemeRestorations; // パーサーのトークン種別を直す為に置き換えたテキストのデコードデータ（始まりの順）
	std::vector<std::pair<uint32_t, uint32_t>> DirectiveLines; // 入力の走査で確定した前処理の見出の範囲（修復前に１度消費する）
	AppliedEditBatches EditBatches; // 修復の編集の組（適用順，組の中は位置順で重ならない）
	std::vector<AppliedEdit> PreparationEdits; // テキストの準備から正規化した入力へ戻す対応（適用順）
	std::vector<EscapedComment> EscapedComments; // 解析用の入力から退避したコメント（位置は修復の編集に合わせて動かす）
	unsigned char AnalysisMark = '\0'; // 解析の為に原文へ置いたマーカー（C / C++ の連結指定と前処理リテラル，CSS のエスケープのバックスラッシュ，Ruby の埋込ドキュメント，置いて居なければ無い）
	// 独立したコードとして整形した HTML の埋込の出力（埋込の中身のノードで引く，整形仕様 4.4）
	std::unordered_map<const void *, std::shared_ptr<const Core::OwnedRegion>> OwnedRegions;
	ParseDecision Decision = ParseDecision::Continue; // 受理の区分
	FailureReason Failure; // 受理しない理由
	bool HasInputError = false; // 受理した入力の構文エラーの有無
	ParseIssue Issue = ParseIssue::None; // 破損の原因の確度
	bool HasMissingBody = false; // 受理判定で確定した幅０の本体の有無
	std::optional<uint32_t> IssueOffset; // 最初の破損の修復後の本文位置
	static std::pair<size_t, unsigned char> BlankRef(const std::string_view Text, const size_t Pos); // 空白・タブへデコードされる数値文字参照の長さと其の文字の取得関数
	static uint32_t OffsetBeforeBatches(uint32_t Offset, const AppliedEditBatches &Batches); // 適用前の本文位置への逆マッピング関数
	static void JoinMacroDef(const MacroDef &Other, MacroDef &Joined); // 条件枝で異なる定義の性質を合流する関数
	std::string_view StoreName(const std::string_view Name) const; // 世代内で所有した名前の共通ビューの取得関数
	BoolNameSource GetBoolNameSource() const; // 真偽値名の判定の供給元の取得関数
	const MacroDef *FindMacroDef(const std::string_view Name, const uint32_t Use) const; // 使用位置の入力のマクロの記録の取得関数
	MacroDef GetMacroDef(const std::string_view Name, const uint32_t Use) const; // 使用位置のマクロ状態の取得関数
	uint32_t GetBoolNameLimit(const uint32_t Use) const; // 真偽値名の束縛が変わり得る次の位置の取得関数
	bool IsAmbiguousDeclType(const TSNode Node, const TSNode Type) const; // 取得済の型から宣言と式の曖昧性を判定する関数
	uint8_t GetMacroProps(const std::string_view Name, const uint32_t Use, const uint8_t Mask) const; // 使用位置の展開の性質の共有取得関数
	bool HasMacroProp(const std::string_view Name, const uint32_t Use, const MacroProp Prop) const; // 使用位置の定義と参照先が性質を持つかの判定関数
	std::vector<MacroToken> TokenizeMacroBody(const std::string_view Body) const; // コメントと引用領域を区分する置換本体のトークン化関数

	void CollectMacroUses(
		const TSNode Node,
		const uint8_t Mask,
		std::vector<uint32_t> &ExprUses,
		std::vector<uint32_t> &DeclaratorUses
	) const; // 対象内の式・宣言子マクロの使用位置の共通収集関数

	void RecordNodeRelations() const; // 木の世代毎の親・子・記号・トークン末尾・許容するエラーの所属，値の受理・括弧の開閉数と真偽値名の遮蔽範囲の収集関数
	const ParsedChild &GetNodeAt(const size_t Relation) const; // 収集済の記録番号からノードの事実を読む関数
	void PrepareChildFields(const TSNode Node, const size_t Relation = std::string::npos) const; // 所有スレッドで投影前に外側のフィールドと最初の子を確定する関数
	std::pair<uint32_t, uint32_t> GetCommentRangeOf(const TSNode Node) const; // コメントの本文の範囲の取得関数（幅０のノードは直前の本文を回収）
	TSNode GetParentOf(const TSNode Node) const; // 根からの降下を繰り返さない直接親の取得関数
	void ReplaceTree(TSTree *const NewTree) const; // 構文木と其の控えの差替関数
	const std::vector<CommentSpan> &GetCommentSpans() const; // 原文順のコメントの範囲の表の取得関数（木の世代毎に１度だけ作る）
	bool HasDisputedJsxLineEdges(const std::string_view Text) const; // 行端の扱いが TypeScript と Babel で分かれる JSX 本文かの判定関数
	Core::StructureWorkAccount *GetNameWorkCounter() const noexcept; // 共有木の並列区間では計数先を渡さない関数
	void ChargeNameWork() const; // 所有スレッドの束縛照会だけを計上する関数
	size_t GetCommonNameScope(size_t Left, size_t Right) const; // ２つの所属の共通の外側の取得関数
	void BuildNameRoutes(const size_t Scope) const; // 導入関係の未反映の差分だけを共有区間へ纏める関数
	void PrepareNameReads() const; // 並列区間が読む宣言と導入経路を所有スレッドで確定する関数

	const NameBinding *GetScopeBinding(
		const size_t Scope,
		const std::string_view Name,
		const uint32_t Use,
		const bool ShouldUseMembersOnly = false,
		size_t *const Tier = nullptr,
		uint32_t *const ValidUntil = nullptr
	) const; // 所属内の名前の解決関数（限定名はメンバだけを対象とする）

	NameResolution GetInheritedBinding(
		const size_t Scope,
		const std::string_view Name,
		const uint32_t Use,
		const size_t Stop
	) const; // 名前の無い外側の所属を共有結果で飛ばす関数

	const NameBinding *GetNameBinding(const std::string_view Name, const uint32_t Use) const; // 使用位置で最寄の束縛の取得関数
	const TemplateSymbols &GetTemplateSymbols() const; // 修復先の文法の記号を１度だけ取得する関数
	std::vector<TemplatePart> ReadTemplateDecl(const TSNode Node, TSNode &DeclaredName) const; // 型引数付宣言と導入名の確定関数
	const std::vector<TemplatePart> &GetTemplateDecl(const TSNode Node) const; // 使用位置の束縛から確定した宣言の共有関数
	bool IsDeclaredTemplateName(const std::string_view Name, const uint32_t Use) const; // 使用位置で有効な型のテンプレート名かの判定関数
	const NameBinding *GetQualifiedNameBinding(const std::string_view Name, const uint32_t Use) const; // 修飾先を含む使用位置の束縛の取得関数
	size_t ResolveNameScope(std::string_view Name, const uint32_t Use) const; // 限定名が指す所属の解決関数
	bool IsToleratedModuleError(const TSNode Node) const; // Sass のモジュールの節に属する許容エラーかの判定関数
	bool IsToleratedCssUrlVal(const TSNode Node) const; // 逐語の URL 値に属する偽の欠落かの判定関数
	std::vector<TSNode> GetAncestorsAt(const uint32_t Pos) const; // 指定バイトを含むノードの降下経路の取得関数
	TSNode GetSiblingBeforeImmediate(const TSNode Node) const; // 直前の兄弟ノード取得関数

	CommentKind ClassifyComment(
		const std::vector<CommentSpan> &Spans,
		const uint32_t Start,
		const uint32_t End,
		bool *const IsAtLineStart = nullptr,
		uint32_t *const AfterComments = nullptr
	) const; // コメントの種類の判定の共通関数

	std::shared_ptr<const MacroBody> ReadMacroBody(const TSNode Node, const std::span<const MacroToken> Tokens) const; // 定義のトークンから性質と参照先を収集する関数
	void CollectMacroStates() const; // 条件枝毎の前処理状態の収集関数
	void AddNameImport(const size_t Owner, const NameBinding Import) const; // 導入辺の追加と影響する経路への差分の登録関数
	void Reparse() const; // 構文木の再解析関数

	template<typename Str> void AssignSrc(Str &&NewSrc); // 本文差替と未反映状態の共通更新関数

	void AttachTrailingData(const std::string_view Data); // 解析済の本文へ逐語データを付ける関数
	void SetParseDecision(const ParseDecision NewDecision, const FailureReason Reason, const bool HasSyntaxError); // 受理の結果の記録関数
	void Assign(std::string &&NewSrc); // ソース移動代入関数

public:

	bool HasRecordedRelations() const noexcept; // 子列の収集を終えた現在の世代かの判定関数
	const ParsedChild *FindRecordedNode(const TSNode Node) const; // 収集済の世代のノードの取得関数（未収集・空・別の木なら無）
	TSSymbol GetSymbol(const TSNode Node) const; // パーサーの記号の取得関数
	bool IsNamed(const TSNode Node) const; // パーサーの名前付フラグの取得関数
	bool IsMissing(const TSNode Node) const; // パーサーの欠落フラグの取得関数
	bool IsError(const TSNode Node) const; // パーサーのエラーノードフラグの取得関数
	bool HasError(const TSNode Node) const; // パーサーの部分木のエラーフラグの取得関数
	uint32_t GetChildCount(const TSNode Node) const; // パーサーの直接子数の取得関数
	TSNode GetChild(const TSNode Node, const uint32_t Index) const; // パーサーの直接子の取得関数
	TSNode GetNextSibling(const TSNode Node) const; // パーサーの次の直接兄弟の取得関数
	uint32_t GetStart(const TSNode Node) const; // ノードの開始バイト取得関数
	uint32_t GetEnd(const TSNode Node) const; // ノードの終了バイト取得関数
	std::string_view GetView(const TSNode Node) const; // ノード範囲のビュー取得関数
	CommentKind GetKindOf(const TSNode Comment) const; // 原文でのコメントの種類の判定関数
	uint32_t OffsetBeforePreparation(const uint32_t Offset) const; // 準備だけを戻した位置の取得関数
	uint32_t OffsetBeforeEdits(const uint32_t Offset) const; // 修復前の本文の位置の取得関数
	bool Is(const TSNode Node, const Core::Role Wanted) const; // パーサーのノードの記号が文法の定義で役割を持つかの判定関数
	TSNode GetFieldChild(const TSNode Node, const Core::ChildRole Role, size_t *const Work = nullptr) const; // 指定された場の役割を持つ最初の子ノード取得関数
	unsigned char GetAnalysisMark() const; // 解析の為に原文へ置いたマーカーの取得関数
	size_t GetRelationWork() const noexcept; // 此の世代で直接子の情報を読んだ回数の取得関数
	bool HasCollectedNames() const noexcept; // 此の世代の宣言と所属の表を収集済かの取得関数
	std::span<const ParsedChild> GetChildrenAt(const size_t Relation) const; // 同じ木で取得済の記録番号から子列を読む関数
	std::span<const ParsedChild> GetChildrenOf(const TSNode Node) const; // 世代で共有する直接子の範囲の取得関数
	size_t GetBoundaryRepairWork() const noexcept; // 文境界修復の累積作業量の取得関数
	size_t GetCastRepairWork() const noexcept; // キャスト修復の候補の訪問回数の取得関数
	size_t GetPrefixTypeRepairWork() const noexcept; // 前置型の修復の累積作業量の取得関数
	size_t GetNameWork() const noexcept; // 束縛の探索と宣言子の成立確認の累積作業量の取得関数
	void CollectDeclaredNames() const; // 宣言した値と型の名前の収集関数（C / C++ / C#）
	bool IsDeclaredName(const std::string_view Name, const uint32_t Use) const; // 最寄の束縛が値の名前かの判定関数
	bool IsNonMacroName(const std::string_view Name, const uint32_t Use) const; // 使用位置で非マクロの事実を持つ名前かの判定関数
	bool HasMacroBinding(const std::string_view Name, const uint32_t Use) const; // 入力の定義に依るマクロ置換が介在し得るかの判定関数
	bool IsMacroUncertain(const std::string_view Name, const uint32_t Use) const; // 置換の有無が未確定かの判定関数
	bool IsFunctionMacro(std::string_view Name, const uint32_t Use) const; // 使用位置で有効な関数形式マクロ又は其の別名かの判定関数
	bool IsDeclaredTypeName(const std::string_view Name, const uint32_t Use) const; // 最寄の束縛が型の名前かの判定関数
	const GrammarProfile &GetProfile() const; // 解析に用いる文法の定義の取得関数
	bool IsTokenSpelling(const TSNode Node, const Core::Role Wanted) const; // ノードのテキストが役割を持つトークンの綴りかの判定関数
	uint32_t OffsetInInput(const uint32_t Offset) const; // 準備と修復を戻した入力位置の取得関数
	const Core::SyntaxRules &GetRules() const; // 文法の構文・意味の固定の定義の取得関数
	const ReadingRules &GetReading() const; // 入力の読取規則の取得関数
	TSNode GetSwiftPrefixArgs(const TSNode Node) const; // Swift の前置の演算子の呼出と読んだ括弧の実引数の並びの取得関数
	bool CanAttachRubySign(const TSNode Operand) const; // Ruby の符号を密着させても読みが変わらない被演算子かの判定関数
	std::string_view GetCodeText() const; // 逐語データを除いた解析対象の本文の取得関数
	bool IsParsed() const; // 解析済判定関数
	TSNode GetRoot() const; // ルートノード取得関数
	uint32_t GetMacroLineCommentStart(const TSNode Body) const; // マクロの本体のトークンが呑んだ行コメントの開始位置の取得関数
	bool IsAmbiguousDecl(const TSNode Node) const; // 式と区別出来ない宣言かの判定関数
	std::optional<ParenthesizedDecl> ReadParenthesizedDecl(const TSNode Stmt) const; // 括弧付宣言の構成の確定関数
	TSNode GetFirstNamedChild(const TSNode Node, const Core::Role Role) const; // 指定された役割を持つ最初の名前付子ノード取得関数
	const std::vector<uint32_t> &GetInsertedBreaks() const; // 解析用の文の区切の位置の取得関数
	const std::vector<std::pair<uint32_t, uint32_t>> &GetSyntheticGroups() const; // 解析用の囲みの位置の組の取得関数
	const std::vector<LexemeRestoration> &GetLexemeRestorations() const; // 解析用に置き換えたトークンのデコードデータの取得関数
	bool HasCodeTextDecoding() const; // コード行の比較でデコードする状態を持つかの判定関数
	ParseDecision GetParseDecision() const; // 受理の区分の取得関数
	FailureReason GetParseFailure() const; // 受理しない理由の取得関数
	ParseIssue GetParseIssue() const; // 入力エラーと未識別構文の区分の取得関数
	std::optional<uint32_t> GetIssueOffset() const; // 準備と修復を戻した入力の破損位置の取得関数
	bool HasOriginalInputError() const; // 読違を修復しても残る入力の構文エラーの有無の取得関数
	bool IsHeaderField(const TSNode Node, const TSNode Child) const; // 見出の部品を指す場の子かの判定関数
	TSNode GetIfConsequence(const TSNode Node) const; // if 文の真枝本体取得関数
	bool HasDisputedJsxText(const TSNode Container) const; // 行端の扱いが処理系で分かれる本文を持つ JSX 要素かの判定関数
	bool HasExpiredParse() const; // 解析の打切の判定関数
	uint32_t GetLen(const TSNode Node) const; // ノード範囲のバイト長取得関数
	bool IsStmtMacroCall(const TSNode Stmt) const; // 文へ展開するマクロの呼出の文かの判定関数
	bool IsOmittedSemicolon(const TSNode Child, const TSNode PrevChild) const; // 構造化で省く最上位の空の式文 `;` かの判定関数
	bool HasBoolNameAt(const std::string_view Name, const uint32_t Use) const; // 使用位置で真偽値名が組込の定数と同じ値へ束縛されるかの判定関数
	bool IsCssUrlArgs(const TSNode Node) const; // CSS の `url(...)` の引数の並びかの判定関数
	bool HasBodyLikeExpr(const TSNode Node) const; // 見出の本体の始まりと紛れる式の判定関数
	bool HasAmbiguousRubyLead(TSNode Lead) const; // Ruby の実引数の先頭の曖昧さの判定関数
	size_t GetHaltedDataStart() const; // 実行停止後の逐語データの開始位置の取得関数
	bool CanChangeReadingAt(const uint32_t PrevEnd, const uint32_t NextStart) const; // トークンの間の改行で読みが変わるかの判定関数

	uint32_t FindLineEndAfter(
		const uint32_t From,
		const std::span<const std::pair<uint32_t, uint32_t>> Gaps,
		std::vector<TSNode> &Following
	) const; // 行末コメントを置く行末の位置の取得関数

	std::shared_ptr<const Core::OwnedRegion> GetOwnedRegionOf(const TSNode RawText) const; // 独立したコードとして整形した埋込の出力の取得関数
	std::vector<std::pair<uint32_t, uint32_t>> CollectCommentRanges(const uint32_t BodyEnd) const; // コメントのノードとトークンの隙間に残ったコメントの範囲の収集関数
	std::vector<std::pair<uint32_t, uint32_t>> CollectInputCommentRanges(const uint32_t BodyEnd) const; // 退避したコメントも含む入力座標の範囲の収集関数
	const StringizingArgs *GetStringizingArgs(const TSNode List) const; // 使用位置の文字列化する仮引数と可変範囲の共有取得関数
	bool HasBlankLineBetween(const uint32_t Begin, const uint32_t End) const; // 範囲に空行が在るかの判定関数
	bool CanAbsorbGoComment(const TSNode Node) const; // 直前のコメントを取り込む Go の最上位の宣言かの判定関数
	bool HasEmptyBlock() const; // 受理判定で確定した欠落本体の有無の取得関数
	const LexemeRestoration *GetRestorationAt(const uint32_t Start, const uint32_t End) const; // 範囲に対応するトークンのデコードデータの取得関数
	TSTree *GetMutableTree(); // 可変構文木取得関数
	void MarkTreeEdited(); // 構文木へ編集を通知済である事の設定関数
	void SetOwnedRegion(const TSNode RawText, Core::OwnedRegion &&Region); // 独立したコードとして整形した埋込の出力の控関数

	void AssignEdited(
		std::string &&NewSrc,
		std::vector<uint32_t> &&Breaks,
		std::vector<std::pair<uint32_t, uint32_t>> &&Groups,
		std::vector<LexemeRestoration> &&Restorations,
		std::vector<AppliedEdit> &&Batch
	); // 修復の編集を適用した本文の差替関数

	void AddLexemeRestoration(const uint32_t Start, const uint32_t End, std::string Original); // 解析用に置き換えたトークンの記録関数
	void ReleaseSyntax(); // 投影済の木と解析の控えを解放し，本文と位置の対応を保持する関数

	ParsedTree &operator=(ParsedTree &&Other) = default; // 移動代入演算子

	// コンストラクタ（確定した文法と逐語データの開始）
	ParsedTree(const GrammarProfile &Profile, std::string Text, const size_t DataStart = std::string::npos);

	ParsedTree(const ParsedTree &) = delete; // 複写構築の禁止
	ParsedTree(ParsedTree &&Other) = default; // 移動構築関数
};

// 取得時の reset 迄参照せず，確保済領域を再利用するスレッドローカルカーソル貯留庫
struct CursorPool {
	std::vector<TSTreeCursor> Free; // 返却済カーソルの積重

	/**
	 * 貯留庫からのカーソル取得関数
	 * 計算量：O(1)
	 * @param Node 走査対象ノード
	 * @return 対象ノードに位置付いたカーソル
	 */
	inline TSTreeCursor AcquireCursor(const TSNode Node) {
		// カーソル貯留状態に応じた取得元の選択
		// 貯留が無い場合に新規生成したカーソルの返戻
		if(Free.empty()) return ts_tree_cursor_new(Node);
		// 貯留カーソルの取出と走査対象への再標的化
		TSTreeCursor Cursor = Free.back();
		Free.pop_back();
		ts_tree_cursor_reset(&Cursor, Node);
		// 再標的化済カーソルの返戻
		return Cursor;
	}

	/**
	 * 貯留庫へのカーソル返却関数
	 * 計算量：O(1)
	 * @param Cursor 返却するカーソル（以後呼出側では使用しない）
	 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
	 */
	inline void ReleaseCursor(const TSTreeCursor &Cursor) {
		// 返却カーソルの貯留
		Free.push_back(Cursor);
		// 終了
		return;
	}

	~CursorPool(); // デストラクタ（スレッド終了時の貯留カーソル解放）
};

inline thread_local CursorPool PooledCursors; // スレッドローカルの貯留庫実体

// 例外時も借用カーソルを返すインライン保持子
struct HeldCursor {
	TSTreeCursor Cursor; // 借りたカーソル

	/**
	 * カーソルの位置取得演算子
	 * 計算量：O(1)
	 * @return 借りたカーソルへの位置
	 */
	inline TSTreeCursor *operator&() {
		// カーソルの位置の返戻
		return &Cursor;
	}

	HeldCursor &operator=(const HeldCursor &) = delete; // 複写代入演算子（禁止）
	/**
	 * カーソルの借用のコンストラクタ
	 * 計算量：O(1)
	 * @param Node 走査対象ノード
	 */
	explicit inline HeldCursor(const TSNode Node) : Cursor(PooledCursors.AcquireCursor(Node)) {}
	HeldCursor(const HeldCursor &) = delete; // 複写コンストラクタ（禁止）

	/**
	 * カーソルの返却のデストラクタ
	 * 計算量：走査中に保持する深さ D に対し O(D)
	 */
	inline ~HeldCursor() noexcept {
		// 貯留庫への積直が失敗した場合は例外を出さずに解放する
		try {
			PooledCursors.ReleaseCursor(Cursor);
		} catch(...) {
			ts_tree_cursor_delete(&Cursor);
		}
		// 終了
		return;
	}
};

/**
 * 子ノードの線形走査の共通関数
 * 訪問関数又は述語が送出する例外は捕捉せず，同じ型と発生条件で呼出元へ伝える
 * 計算量：子数 C と Visit の総費用 F に対し O(C + F)
 * @param Node 走査対象ノード
 * @param Visit 各子に適用する関数（bool 戻値の偽で打切，void なら最後迄走査）
 */
template<bool IsNamedOnly, typename F> inline void ForEachChildImpl(const TSNode Node, F &&Visit) {
	// 対象ノードに位置付けたカーソルの借用
	HeldCursor Cursor(Node);
	// 先頭子からの兄弟走査
	if(ts_tree_cursor_goto_first_child(&Cursor)) {
		do {
			const TSNode Child = ts_tree_cursor_current_node(&Cursor);
			// 名前付限定バージョンは無名トークン（`{`, `,` 等）の読飛
			if(IsNamedOnly && !ts_node_is_named(Child)) continue;
			// 戻値型に応じた打切可否の適用
			if constexpr(!std::is_same_v<std::invoke_result_t<F, TSNode>, bool>) Visit(Child);
			else if(!Visit(Child)) break;
		} while(ts_tree_cursor_goto_next_sibling(&Cursor));
	}
	// 終了
	return;
}

/**
 * 子ノードの線形走査関数
 * 走査する子数 C とコールバックの総費用 F に対し O(C + F)
 * 訪問関数又は述語が送出する例外は捕捉せず，同じ型と発生条件で呼出元へ伝える
 * @param Node 走査対象ノード
 * @param Visit 各子に適用する関数（bool 戻値の偽で打切，void なら最後迄走査）
 */
template<typename F> inline void ForEachChild(const TSNode Node, F &&Visit) {
	// 全ての子を対象にする共通走査の呼出
	ForEachChildImpl<false>(Node, std::forward<F>(Visit));
	// 終了
	return;
}

/**
 * 名前付子限定の線形走査関数
 * 無名を含む子数 C とコールバックの総費用 F に対し O(C + F)
 * 訪問関数又は述語が送出する例外は捕捉せず，同じ型と発生条件で呼出元へ伝える
 * @param Node 走査対象ノード
 * @param Visit 各名前付子に適用する関数（bool 戻値の偽で打切）
 */
template<typename F> inline void ForEachNamedChild(const TSNode Node, F &&Visit) {
	// 無名トークン（`{`, `,` 等）を読み飛ばす名前付限定バージョンへ委譲
	ForEachChildImpl<true>(Node, std::forward<F>(Visit));
	// 終了
	return;
}

/**
 * 最後の名前付子の取得関数
 * 計算量：直接の子数 C に対し O(C)
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
 * 子数 C とテキスト比較の総バイト数 B に対し O(C + B)
 * @param Src ソース文字列
 * @param Node 対象ノード
 * @param Token 探索するトークン
 * @return 含まれれば true
 */
inline bool HasUnnamedTokenChild(const std::string &Src, const TSNode Node, const std::string_view Token) {
	// 無名のトークンの探索準備
	bool IsFound = false;
	// 無名子の対象トークンとの照合
	ForEachChild(
		Node,
		[&](const TSNode Child) -> bool {
			// 名前付の子を読み飛ばす事の返戻
			if(ts_node_is_named(Child)) return true;
			// 無名子のソース範囲と探索トークンの照合
			if(
				const uint32_t Start = ts_node_start_byte(Child), End = ts_node_end_byte(Child);
				End > Start && std::string_view(Src.data() + Start, End - Start) == Token
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
 * Visit の bool 戻値が真なら当該ノード内部に降り，偽なら次の兄弟ノードへ進む
 * 訪問関数又は述語が送出する例外は捕捉せず，同じ型と発生条件で呼出元へ伝える
 * 計算量：走査ノード数 V とコールバックの総費用 C に対し O(V + C)
 * @param Root 走査開始ノード（直下の子から処理し Root 自身には Visit を適用しない）
 * @param Visit 各ノードに適用し内部へ降りるか否かを返す関数（ノードと親を受ける形なら親を，ノードと祖先の並びを受ける形なら Root から親迄の並びも渡す）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws ProcessingInterrupted 書戻以外の処理の中断を要求済の場合
 */
template<typename F> inline void WalkChildrenCursor(const TSNode Root, F &&Visit) {
	// 親配列を降下・上昇と共に更新し，ts_node_parent の２乗走査を避ける
	constexpr bool ShouldVisitAncestors = std::is_invocable_r_v<bool, F, TSNode, std::span<const TSNode>>;
	constexpr bool ShouldVisitParent = ShouldVisitAncestors || std::is_invocable_r_v<bool, F, TSNode, TSNode>;
	std::vector<TSNode> Parents;
	HeldCursor Cursor(Root);
	// 根直下からの先行順走査
	if(ts_tree_cursor_goto_first_child(&Cursor)) {
		if constexpr(ShouldVisitParent) Parents.push_back(Root);
		for(bool IsDone = false; !IsDone;) {
			ParserAllocator::CheckInterruption();
			const TSNode Node = ts_tree_cursor_current_node(&Cursor);
			// コールバック形に応じた親情報の供給
			bool ShouldDescend;
			if constexpr(ShouldVisitAncestors) ShouldDescend = Visit(Node, std::span<const TSNode>(Parents));
			else if constexpr(ShouldVisitParent) ShouldDescend = Visit(Node, Parents.back());
			else ShouldDescend = Visit(Node);
			if(ShouldDescend && ts_tree_cursor_goto_first_child(&Cursor)) {
				// 降下時の親経路の更新
				if constexpr(ShouldVisitParent) Parents.push_back(Node);
				continue;
			}
			// 次の兄弟又は未走査の祖先兄弟への移動
			while(!ts_tree_cursor_goto_next_sibling(&Cursor)) {
				// 根到達時の走査終了判定
				if(!ts_tree_cursor_goto_parent(&Cursor)) {
					IsDone = true;
					break;
				}
				if constexpr(ShouldVisitParent) Parents.pop_back();
			}
		}
	}
	// 終了
	return;
}

/**
 * 記録済の子列を優先する降下制御付の先行順走査関数
 * 未収集の木ではカーソルを使い，走査の為だけの表を作らない
 * 計算量：訪問数 V とコールバックの総費用 C に対し O(V + C)
 * @param Src 同じ世代の木と収集済の親子表
 * @param Root 直下の子から走査する根
 * @param Visit 降下を選ぶ訪問関数（記録を受ける形は表を収集し，走査中は木を差し替えない）
 * @param Work 訪問数の格納先（無ければ計上しない）
 * @param Queries 子列の照会数の格納先（無ければ計上しない）
 * @param LeafSkips 葉の照会を省いた数の格納先（無ければ計上しない）
 * @throws std::bad_alloc 作業列を確保出来ない場合
 * @throws std::length_error 作業列の長さが上限を超える場合
 * @throws std::out_of_range 記録済の子列が表の範囲外の場合
 * @throws ProcessingInterrupted 書戻以外の処理の中断を要求済の場合
 */
template<typename F> inline void WalkRecordedChildren(
	const ParsedTree &Src,
	const TSNode Root,
	F &&Visit,
	size_t *const Work = nullptr,
	size_t *const Queries = nullptr,
	size_t *const LeafSkips = nullptr
) {
	constexpr bool NeedsRecord = std::is_invocable_r_v<bool, F, const ParsedChild &, std::span<const TSNode>>;
	std::span<const ParsedChild> Children;
	if constexpr(NeedsRecord) Children = Src.GetChildrenOf(Root);
	else {
		const ParsedChild *const Record = Src.HasRecordedRelations() ? Src.FindRecordedNode(Root) : nullptr;
		if(!Record) {
			WalkChildrenCursor(Root, Visit);
			// 未収集の木の走査の終了
			return;
		}
		Children = Src.GetChildrenAt(Record->Relation);
	}
	if(Queries) ++*Queries;
	constexpr bool NeedsAncestors = NeedsRecord || std::is_invocable_r_v<bool, F, TSNode, std::span<const TSNode>>;
	constexpr bool NeedsParent = NeedsAncestors || std::is_invocable_r_v<bool, F, TSNode, TSNode>;
	struct Frame {
		std::span<const ParsedChild> Children; // 同じ世代で確定した直接子
		size_t Next = 0; // 次に訪問する子の位置
	};
	std::vector<Frame> Frames;
	std::vector<TSNode> Ancestors;
	if(!Children.empty()) {
		Frames.push_back({ Children });
		if constexpr(NeedsParent) Ancestors.push_back(Root);
	}
	while(!Frames.empty()) {
		ParserAllocator::CheckInterruption();
		Frame &Current = Frames.back();
		if(Current.Next == Current.Children.size()) {
			Frames.pop_back();
			if constexpr(NeedsParent) Ancestors.pop_back();
			continue;
		}
		const ParsedChild &Child = Current.Children[Current.Next++];
		if(Work) ++*Work;
		bool ShouldDescend;
		if constexpr(NeedsRecord) ShouldDescend = Visit(Child, std::span<const TSNode>(Ancestors));
		else if constexpr(NeedsAncestors) ShouldDescend = Visit(Child.Node, std::span<const TSNode>(Ancestors));
		else if constexpr(NeedsParent) ShouldDescend = Visit(Child.Node, Ancestors.back());
		else ShouldDescend = Visit(Child.Node);
		if(!ShouldDescend) continue;
		if constexpr(NeedsRecord) if(!Src.GetChildCount(Child.Node)) {
			if(LeafSkips) ++*LeafSkips;
			continue;
		}
		if(Queries) ++*Queries;
		const std::span<const ParsedChild> Nested = Src.GetChildrenAt(Child.Relation);
		if(Nested.empty()) continue;
		Frames.push_back({ Nested });
		if constexpr(NeedsParent) Ancestors.push_back(Child.Node);
	}
	// 終了
	return;
}

/**
 * 宣言の収集が持つ訪問・照会・葉の省略の計数を共通走査へ渡す関数
 * @param Src 記録済の親子表を持つ木
 * @param Root 直下の子から走査する根
 * @param Visit 記録と祖先の並びを受ける訪問関数
 * @param Work 訪問数の格納先
 * @param Queries 子列の照会数の格納先
 * @param LeafSkips 葉の照会を省いた数の格納先
 * @throws std::bad_alloc 作業列を確保出来ない場合
 * @throws std::length_error 作業列の長さが上限を超える場合
 * @throws std::out_of_range 子列の記録番号が表の範囲外の場合
 */
template<typename F> inline void WalkRecordedChildren(
	const ParsedTree &Src,
	const TSNode Root,
	F &&Visit,
	size_t &Work,
	size_t &Queries,
	size_t &LeafSkips
) {
	WalkRecordedChildren(Src, Root, Visit, &Work, &Queries, &LeafSkips);
	// 終了
	return;
}

/**
 * 名前付のノードかの判定関数
 * 計算量：O(1)
 * @param Node 判定対象のノード（子や兄弟を引いた結果の空ノードも受ける）
 * @return 空でない名前付のノードなら true
 */
inline bool IsNamedNode(const TSNode Node) {
	// 空ノードは構文木を辿れず名前も持たない事の返戻
	return !ts_node_is_null(Node) && ts_node_is_named(Node);
}

/**
 * 構文木の根の判定関数
 * 計算量：木の深さ D に対し O(D)
 * @param Node 判定対象ノード
 * @return 木の根なら true
 */
inline bool IsTreeRoot(const TSNode Node) {
	// 空ノードは木を持たず，根の問合せが落ちる事の返戻
	if(ts_node_is_null(Node)) return false;
	// 親の有無で判定すると `ts_node_parent` が根から降下し直す為，根ノードとの同一性で判定する事の返戻
	return ts_node_eq(Node, ts_tree_root_node(Node.tree));
}

/**
 * 述語を満たす直接子の有無の判定関数
 * 名前付／無名の区別無く最初の発見で打ち切る
 * 走査する子数 C と述語の総費用 F に対し O(C + F)
 * 訪問関数又は述語が送出する例外は捕捉せず，同じ型と発生条件で呼出元へ伝える
 * @param Node 走査開始ノード
 * @param PredicateFunction ノードを受取 bool を返す述語
 * @return 直接子に述語充足が有れば true
 * @throws ProcessingInterrupted 書戻以外の処理の中断を要求済の場合
 */
template<typename Predicate> inline bool HasChildOf(const TSNode Node, Predicate PredicateFunction) {
	// 空ノードは false の返戻
	if(ts_node_is_null(Node)) return false;
	// 直接子を走査するカーソルと発見状態の準備
	HeldCursor Cursor(Node);
	bool IsFound = false;
	// 最初の述語充足迄の直接子走査
	if(ts_tree_cursor_goto_first_child(&Cursor)) {
		// 各直接子への述語適用
		do {
			ParserAllocator::CheckInterruption();
			if(PredicateFunction(ts_tree_cursor_current_node(&Cursor))) {
				IsFound = true;
				break;
			}
		} while(ts_tree_cursor_goto_next_sibling(&Cursor));
	}
	// 直接子の述語充足有無の返戻
	return IsFound;
}

/**
 * 述語を満たす子孫の有無の判定関数
 * 走査ノード数 V と述語の総費用 F に対し O(V + F)
 * 訪問関数又は述語が送出する例外は捕捉せず，同じ型と発生条件で呼出元へ伝える
 * @param Node 走査開始ノード
 * @param PredicateFunction ノードを受取 bool を返す述語
 * @return 述語を満たす子孫が見付かれば true（自身も含む）
 * @throws ProcessingInterrupted 書戻以外の処理の中断を要求済の場合
 */
template<typename Predicate> inline bool HasDescendantOf(const TSNode Node, Predicate PredicateFunction) {
	// 空ノードは述語を満たさない事の返戻
	if(ts_node_is_null(Node)) return false;
	// TSTreeCursor で反復的に先行順走査し再帰呼出と ts_node_child の累積 O(C²) を回避する
	HeldCursor Cursor(Node);
	bool IsFound = false;
	// 部分木内の先行順述語照合
	while(true) {
		ParserAllocator::CheckInterruption();
		// 現在ノードへの述語適用
		if(PredicateFunction(ts_tree_cursor_current_node(&Cursor))) {
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
 * 走査ノード数 V と述語の総費用 F に対し O(V + F)
 * 訪問関数又は述語が送出する例外は捕捉せず，同じ型と発生条件で呼出元へ伝える
 * @param Node 走査開始ノード（其の祖先は渡さない）
 * @param PredicateFunction ノードと其の祖先の並び（走査開始ノードから親迄，末尾が親，開始ノードでは空）を受取 bool を返す述語
 * @param ShouldPruneHealthy エラーと欠落ノードだけを調べる述語で，健全な部分木の内側を省くか
 * @return 述語を満たす子孫が見付かれば true（自身も含む）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws ProcessingInterrupted 書戻以外の処理の中断を要求済の場合
 */
template<typename Predicate> inline bool HasDescendantWithAncestorsOf(
	const TSNode Node,
	Predicate PredicateFunction,
	const bool ShouldPruneHealthy = false
) {
	// 空ノードの返戻
	if(ts_node_is_null(Node)) return false;
	HeldCursor Cursor(Node);
	std::vector<TSNode> Path;
	bool IsFound = false;
	// 祖先経路を伴う部分木の先行順走査
	while(true) {
		ParserAllocator::CheckInterruption();
		// `ts_node_parent` は呼出毎に根から降り直し，子孫毎に呼ぶと深さの２乗の費用に為る為，走査の道筋を積んで祖先の並びを渡す
		const TSNode Cur = ts_tree_cursor_current_node(&Cursor);
		// 現在ノードへの祖先列付述語適用
		if(PredicateFunction(Cur, std::span<const TSNode>(Path))) {
			IsFound = true;
			break;
		}
		if((!ShouldPruneHealthy || ts_node_has_error(Cur)) && ts_tree_cursor_goto_first_child(&Cursor)) {
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
 * 走査ノード数 V とコールバックの総費用 F に対し O(V + F)
 * 訪問関数又は述語が送出する例外は捕捉せず，同じ型と発生条件で呼出元へ伝える
 * @param Node 走査開始ノード（此の部分木のみを対象とし外へは出ない）
 * @param Visit 各ノードに適用する関数
 * @throws ProcessingInterrupted 書戻以外の処理の中断を要求済の場合
 */
template<typename F> inline void WalkAst(const TSNode Node, const F &Visit) {
	// TSTreeCursor の反復走査で ts_node_child の累積 O(C²) と再帰を避ける
	HeldCursor Cursor(Node);
	while(true) {
		ParserAllocator::CheckInterruption();
		Visit(ts_tree_cursor_current_node(&Cursor));
		if(ts_tree_cursor_goto_first_child(&Cursor)) continue;
		// 子無なら兄弟へ，兄弟無なら親へ戻り次の兄弟を試す（Node の親より上には行かない）
		while(!ts_tree_cursor_goto_next_sibling(&Cursor)) {
			// 開始ノードへ戻った場合の返戻
			if(!ts_tree_cursor_goto_parent(&Cursor) || ts_node_eq(ts_tree_cursor_current_node(&Cursor), Node)) return;
		}
	}
}
