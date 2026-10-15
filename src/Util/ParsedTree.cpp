#include "Util/ParsedTree.hpp"
#include "Core/CommentRepository.hpp"
#include "Profile/GrammarProfile.hpp"
#include "Util/CharClass.hpp"
#include "Util/TextEdit.hpp"
#include <tree_sitter/api.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/**
 * 直接子の範囲の記録の生成関数
 * 計算量：O(1)
 * @param Parent 直接親の記録番号
 * @param Begin 子列の開始
 * @param Count 直接の子の数
 * @param Slot 此のノードの子列内の格納先
 */
ParsedTree::NodeRelation::NodeRelation(
	const uint32_t Parent,
	const uint32_t Begin,
	const uint32_t Count,
	const uint32_t Slot
) noexcept : Parent(Parent),
Begin(Begin),
Count(Count),
Slot(Slot),
HasFieldsReady(false),
HasInnerFields(false),
HasNameWork(false) {} // 範囲とフラグの初期化

/**
 * 空の親子の記録の生成関数
 * 計算量：O(1)
 */
ParsedTree::NodeRelation::NodeRelation() noexcept : HasFieldsReady(false), HasInnerFields(false), HasNameWork(false) {} // フラグの初期化

/**
 * 収集済の記録番号からノードの事実を読む関数
 * 計算量：O(1)
 * @param Relation 同じ世代で取得した親子表の記録番号
 * @return 木を差し替える迄有効なノードの事実
 * @throws std::out_of_range 記録番号が範囲外の場合
 */
const ParsedChild &ParsedTree::GetNodeAt(const size_t Relation) const {
	// 根だけは直接親の子列に属さない
	return Relation ? Syntax.Relations->Children[Syntax.Relations->Parents.Records.at(Relation).Slot] : Syntax.Relations->Root;
}

/**
 * 子列の収集を終えた現在の世代かの判定関数
 * @return 収集を完了し，未反映の編集が無い場合は真
 */
bool ParsedTree::HasRecordedRelations() const noexcept {
	return Syntax.HasParents && !Syntax.IsDirty;
} // 現在の世代の収集状態の返戻

/**
 * 収集済の世代のノードの取得関数
 * 収集中は訪問済のノード自身だけを返し，未記録の子列を読まない
 * 計算量：平均 O(1)，最悪 O(N)（識別子の衝突数 N）
 * @param Node 同じ木のノード
 * @return 木の差替迄有効な記録，又は未収集・空・別の木なら nullptr
 */
const ParsedChild *ParsedTree::FindRecordedNode(const TSNode Node) const {
	// 表の無い世代と別の木を参照しない為の終了
	if(!Syntax.Relations || !Node.id || Node.tree != Syntax.CachedRoot.tree) return nullptr;
	const NodeRelation *const Relation = Syntax.Relations->Parents.Find(Node.id);
	// 未記録のノードの空の返戻
	if(!Relation) return nullptr;
	// 根又は親の子列に１度記録した事実の返戻
	return Relation->Parent == UINT32_MAX ? &Syntax.Relations->Root : &Syntax.Relations->Children[Relation->Slot];
}

/**
 * パーサーの記号の取得関数
 * 計算量：O(1)
 * @param Node 対象のノード
 * @return パーサーから直接読んだ記号
 */
TSSymbol ParsedTree::GetSymbol(const TSNode Node) const {
	// 識別子の再探索を挟まない値の返戻
	return ts_node_symbol(Node);
}

/**
 * パーサーの名前付フラグの取得関数
 * 計算量：O(1)
 * @param Node 対象のノード
 * @return パーサーから直接読んだ名前付フラグ
 */
bool ParsedTree::IsNamed(const TSNode Node) const {
	// 識別子の再探索を挟まない値の返戻
	return ts_node_is_named(Node);
}

/**
 * パーサーの欠落フラグの取得関数
 * 計算量：O(1)
 * @param Node 対象のノード
 * @return パーサーから直接読んだ欠落フラグ
 */
bool ParsedTree::IsMissing(const TSNode Node) const {
	// 識別子の再探索を挟まない値の返戻
	return ts_node_is_missing(Node);
}

/**
 * パーサーのエラーノードフラグの取得関数
 * 計算量：O(1)
 * @param Node 対象のノード
 * @return パーサーから直接読んだエラーノードフラグ
 */
bool ParsedTree::IsError(const TSNode Node) const {
	// 識別子の再探索を挟まない値の返戻
	return ts_node_is_error(Node);
}

/**
 * パーサーの部分木のエラーフラグの取得関数
 * 計算量：O(1)
 * @param Node 対象のノード
 * @return パーサーから直接読んだ部分木のエラーフラグ
 */
bool ParsedTree::HasError(const TSNode Node) const {
	// 識別子の再探索を挟まない値の返戻
	return ts_node_has_error(Node);
}

/**
 * パーサーの直接子数の取得関数
 * 計算量：O(1)
 * @param Node 親ノード
 * @return 直接の子の数
 */
uint32_t ParsedTree::GetChildCount(const TSNode Node) const {
	// 識別子の再探索を挟まない値の返戻
	return ts_node_child_count(Node);
}

/**
 * パーサーの直接子の取得関数
 * 計算量：パーサーの子の探索費用 Q に対し O(Q)
 * @param Node 親ノード
 * @param Index 直接子の位置
 * @return 指定位置の子，範囲外なら空のノード
 */
TSNode ParsedTree::GetChild(const TSNode Node, const uint32_t Index) const {
	// 識別子の再探索を挟まない値の返戻
	return ts_node_child(Node, Index);
}

/**
 * パーサーの次の直接兄弟の取得関数
 * 計算量：パーサーの兄弟の探索費用 Q に対し O(Q)
 * @param Node 対象の子
 * @return 次の兄弟，最後の子又は根なら空のノード
 */
TSNode ParsedTree::GetNextSibling(const TSNode Node) const {
	// 識別子の再探索を挟まない値の返戻
	return ts_node_next_sibling(Node);
}

/**
 * ノードの開始バイト取得関数
 * 計算量：O(1)
 * @param Node 対象ノード
 * @return 開始バイト位置
 */
uint32_t ParsedTree::GetStart(const TSNode Node) const {
	// 空ノードへの `ts_node_start_byte` はパーサーの保証外の為，`End` と対称に０を返す
	// 開始位置の返戻
	return ts_node_is_null(Node) ? 0 : ts_node_start_byte(Node);
}

/**
 * ノードの終了バイト取得関数
 * 空のノード（無い場の子）は ts_node_end_byte が中身を辿って落ちる為，開始と同じ位置（空の範囲）を返す
 * 計算量：O(1)
 * @param Node 対象ノード
 * @return 終了バイト位置（排他的）
 */
uint32_t ParsedTree::GetEnd(const TSNode Node) const {
	// 空ノードを含む終了位置の返戻
	return ts_node_is_null(Node) ? 0 : ts_node_end_byte(Node);
}

/**
 * 入力の読取規則の取得関数
 * 計算量：O(1)
 * @return 解析に用いる文法の読取規則
 */
const ReadingRules &ParsedTree::GetReading() const {
	// 読取規則の返戻
	return Grammar->GetReading();
}

/**
 * パーサーのノードの記号が文法の定義で役割を持つかの判定関数（整形仕様 4.3，ノード名の照合を文法の定義へ集める）
 * 計算量：O(1)
 * @param Node パーサーのノード
 * @param Wanted 役割
 * @return 文法の定義でノードの記号が役割を持てば true（言語の指定が無ければ false）
 */
bool ParsedTree::Is(const TSNode Node, const Core::Role Wanted) const {
	// 文法の定義の役割の返戻
	return Grammar && !ts_node_is_null(Node) && Grammar->GetKindOf(GetSymbol(Node)).Roles.test(static_cast<size_t>(Wanted));
}

/**
 * 構文木と其の控えの差替関数
 * 計算量：旧木と其の控えの要素数 N に対し O(N)
 * @param NewTree 所有権を引き取る構文木（解析失敗時は nullptr）
 */
void ParsedTree::ReplaceTree(TSTree *const NewTree) const {
	// 派生情報を同じ世代へ揃える構文木所有権の先行移動
	Syntax.Tree.reset(NewTree);
	// 本文同期状態の確定と旧根キャッシュの無効化
	Syntax.IsDirty = false;
	Syntax.HasCachedRoot = false;
	// 木の世代毎のコメントの位置表の破棄
	Syntax.CommentSpans.clear();
	Syntax.HasCommentNodes = false;
	Syntax.HasCommentSpans = false;
	Syntax.BlankLines.clear();
	Syntax.Relations.reset();
	Syntax.HasParents = false;
	Syntax.HasNameBindings = false;
	Syntax.HasMacroStates = false;
	NameData.reset();
	ReboundDecls.clear();
	TemplateDecls.clear();
	TemplateKinds.reset();
	HasUnrepairedInputError.reset();
	// 終了
	return;
}

/**
 * 投影を終えた解析木と其の控えの解放関数
 * 取得済の解析ノード・子列の参照を無効にする
 * 以後は保持した本文と，準備・修復の位置の対応だけを照会する
 * 計算量：木と控えの要素数 N に対し O(N)
 */
void ParsedTree::ReleaseSyntax() {
	// 木に従属する名前と修復の控えも，同じ差替の経路で解放する
	ReplaceTree(nullptr);
	Syntax = SyntaxState();
	// 終了
	return;
}

/**
 * 構文木の再解析関数
 * 計算量：パーサーの実作業量 Q に対し O(Q)
 * @throws std::length_error 原文がパーサーの位置の範囲を超える場合
 * @throws FormatLimitExceeded 解析が時間の上限を超える場合
 */
void ParsedTree::Reparse() const {
	// パーサを持たない場合の返戻
	if(!Syntax.Parser) return;
	// 編集通知に対応する木を土台に１度だけ解析
	if(size() > std::numeric_limits<uint32_t>::max()) throw std::length_error("source exceeds the parser's position range");
	Syntax.IsParseExpired = false;
	const std::string_view Code(data(), HaltedData == std::string::npos ? size() : HaltedData);
	TSTree *const NewTree = Syntax.Parser->Parse(Syntax.IsTreeEdited ? Syntax.Tree.get() : nullptr, Code, Syntax.IsParseExpired);
	// 増分編集通知と期限超過の確定後に木を差替
	Syntax.IsTreeEdited = false;
	if(!NewTree && Syntax.IsParseExpired) throw FormatLimitExceeded("parsing exceeds the time limit");
	ReplaceTree(NewTree);
	// 終了
	return;
}

/**
 * ルートノード取得関数
 * 計算量：既存木 O(1)，未反映編集は解析費用 P と破棄する控えの要素数 N に対し O(P + N)
 * @return ルートノード
 */
TSNode ParsedTree::GetRoot() const {
	// 未反映編集が有る場合の再解析
	if(Syntax.IsDirty) Reparse();
	if(!Syntax.HasCachedRoot) {
		// 現在木の根ノード
		Syntax.CachedRoot = ts_tree_root_node(Syntax.Tree.get());
		// 根キャッシュの有効化
		Syntax.HasCachedRoot = true;
	}
	// キャッシュ済 Root ノードの返戻（Reparse 時に無効化）
	return Syntax.CachedRoot;
}

/**
 * ノード範囲のビュー取得関数
 * 計算量：O(1)
 * @param Node 対象ノード
 * @return ノード範囲への複写無のビュー
 */
std::string_view ParsedTree::GetView(const TSNode Node) const {
	// ノードの原文範囲
	const uint32_t Begin = ts_node_is_null(Node) ? 0 : ts_node_start_byte(Node);
	const uint32_t End = ts_node_is_null(Node) ? 0 : ts_node_end_byte(Node);
	// 範囲が有効ならビュー，空なら空ビューの返戻
	return Begin < End ? std::string_view(data() + Begin, End - Begin) : std::string_view();
}

/**
 * ノード範囲のバイト長取得関数
 * 計算量：O(1)
 * @param Node 対象ノード
 * @return 終了バイトと開始バイトの差
 */
uint32_t ParsedTree::GetLen(const TSNode Node) const {
	// ノード範囲の長さの返戻
	return ts_node_is_null(Node) ? 0 : ts_node_end_byte(Node) - ts_node_start_byte(Node);
}

/**
 * CSS の `url(...)` の引数の並びかの判定関数（関数名の大小は CSS 仕様上不問）
 * 計算量：URL 関数名の長さ L に対し O(L)
 * @param Node 判定対象のノード
 * @return `url` 呼出の引数の並びなら true
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
bool ParsedTree::IsCssUrlArgs(const TSNode Node) const {
	// 別の名前・メンバ呼出を除く url の前置区切
	static constexpr std::string_view NameDelimiters = ":(,;{} \t\n";
	const std::string_view Head = GetReading().UrlFunctionHead, UrlName = Head.empty() ? Head : Head.substr(0, Head.size() - 1);
	// 関数名は構文木でなく開括弧直前のテキストで見る（SCSS のマップ記法では関数名ノードが `ERROR` へ崩れ木からは辿れない）
	const uint32_t Start = ts_node_start_byte(Node);
	// 引数の並びでない場合と，開括弧の前に関数名の長さが取れない場合の返戻
	if(UrlName.empty() || !Is(Node, Core::Role::CallArgs) || Start < UrlName.size() || Start > size()) return false;
	// １文字でも異なる関数名の場合の返戻（CSS の関数名は大小不問の為，ASCII の大小差分を落として照合する）
	for(size_t Idx = 0; Idx < UrlName.size(); ++Idx) {
		// url の名前と異なる事の返戻
		if((static_cast<unsigned char>(at(Start + Idx - UrlName.size())) | 0X20) != UrlName[Idx]) return false;
	}
	// 関数名直前の字
	const unsigned char Preceding = Start > UrlName.size() ? at(Start - UrlName.size() - 1) : '\0';
	// 関数名の直前が区切又はファイル先頭で `url` 呼出と確定した事の返戻
	return !Preceding || NameDelimiters.find(Preceding) != std::string_view::npos;
}

/**
 * 識別子又は空のスロットの探索関数
 * 計算量：平均 O(1)，最悪はスロット数 S に対し O(S)
 * @param Id 解析ノードの識別子
 * @param Work 挿入時のスロットの読取数の格納先（無ければ観測しない）
 * @return 同じ識別子又は空のスロットの番号
 */
size_t ParsedTree::ParentTable::GetSlot(const void *const Id, size_t *const Work) const {
	// ポインタの整列で下位ビットが偏る為，上位と下位を混ぜてから２冪のインデックスへマッピングする
	size_t Hash = std::hash<const void *>()(Id);
	Hash ^= Hash >> 4; // １６バイトの整列で偏る下位ビットの分散
	Hash *= static_cast<size_t>(0X9E3779B9); // ビットの偏りを分散する乗数
	Hash ^= Hash >> (std::numeric_limits<size_t>::digits >> 1);
	const size_t Mask = Slots.size() - 1;
	for(size_t At = Hash & Mask;; At = At + 1 & Mask) {
		if(Work) ++*Work;
		const uint32_t Entry = Slots[At];
		// 同じ識別子又は未使用のスロットの返戻
		if(Entry == UINT32_MAX || (Entry ? Children[Entry - 1].Node.id : RootId) == Id) return At;
	}
}

/**
 * 記録の読取専用の取得関数
 * 計算量：平均 O(1)，最悪はスロット数 S に対し O(S)
 * @param Id 解析ノードの識別子
 * @return 此の木の記録（無ければ nullptr）
 */
const ParsedTree::NodeRelation *ParsedTree::ParentTable::Find(const void *const Id) const {
	// 空の識別子又は未構築の表の返戻
	if(!Id || Slots.empty()) return nullptr;
	const uint32_t Entry = Slots[GetSlot(Id)];
	// 識別子の記録の返戻
	return Entry == UINT32_MAX ? nullptr : &Records[Entry ? Children[Entry - 1].Relation : 0];
}

/**
 * 構築中の記録の取得関数
 * 計算量：平均 O(1)，最悪はスロット数 S に対し O(S)
 * @param Id 解析ノードの識別子
 * @return 此の木の記録（無ければ nullptr）
 */
ParsedTree::NodeRelation *ParsedTree::ParentTable::Find(const void *const Id) {
	// 空の識別子又は未構築の表の返戻
	if(!Id || Slots.empty()) return nullptr;
	const uint32_t Entry = Slots[GetSlot(Id)];
	// 識別子の記録の返戻
	return Entry == UINT32_MAX ? nullptr : &Records[Entry ? Children[Entry - 1].Relation : 0];
}

/**
 * 世代のノード数に合わせた２列の確保関数
 * 計算量：ノード数 N に対し O(N)
 * @param Count ルートを含む解析ノード数
 * @throws std::bad_alloc 表のメモリを確保出来ない場合
 * @throws std::length_error 表の要素数が格納先の上限を超える場合
 */
void ParsedTree::ParentTable::Reserve(const size_t Count) {
	if(Count > UINT32_MAX) throw std::length_error("parent index exceeds capacity");
	size_t Capacity = 1;
	while((Capacity >> 1) < Count) {
		if(Capacity > (Slots.max_size() >> 1)) throw std::length_error("parent index exceeds capacity");
		Capacity <<= 1;
	}
	Slots.resize(Capacity, UINT32_MAX);
	Records.reserve(Count);
	// 終了
	return;
}

/**
 * 前順の記録とインデックスの追加関数
 * 計算量：平均 O(1)，記録の再確保時又は最悪の衝突ではノード数 N に対し O(N)
 * @param Id 解析ノードの識別子
 * @param Record 親と直接子の範囲
 * @return 前順の記録の番号（同じ識別子は既存の番号）
 * @throws std::bad_alloc 記録のメモリを確保出来ない場合
 * @throws std::length_error 記録の要素数が上限を超える場合
 */
size_t ParsedTree::ParentTable::Add(const void *const Id, NodeRelation Record) {
	uint32_t &Entry = Slots[GetSlot(Id, &InsertProbes)];
	if(Entry == UINT32_MAX) {
		const size_t Index = Records.size();
		if(Index >= UINT32_MAX) throw std::length_error("parent index exceeds capacity");
		Records.push_back(Record);
		Entry = Index ? Record.Slot + 1 : 0;
		++Insertions;
		// 新しい前順の番号の返戻
		return Index;
	}
	// 前順の番号の返戻
	return Entry ? Children[Entry - 1].Relation : 0;
}

/**
 * 記号毎の自動の文終端の入力定義の取得関数
 * 計算量：O(1)
 * @param Reading 文法の入力側の定義
 * @param Symbol 確認する文法の記号
 * @return 表の範囲内の定義，又は範囲外なら文終端に関与しない定義
 */
static AutomaticTerminatorReading GetAutomaticTerminatorReading(const ReadingRules &Reading, const TSSymbol Symbol) {
	// エラー領域等の表の外の記号を，参照前に除く事の返戻
	return Symbol < Reading.AutomaticTerminators.size() ? Reading.AutomaticTerminators[Symbol] : AutomaticTerminatorReading();
}

/**
 * 木の世代毎の親・子・フィールド・記号・照合候補・トークン末尾・コメント・許容するエラーの所属，値の受理・括弧の開閉数と真偽値名の遮蔽範囲の収集関数
 * 計算量：初回はノード数 V，文法の記号数 G，照合バイト数 B，フィールド照会の総費用 Q，遮蔽範囲数 D に対し平均 O(V + G + B + Q + D log(D + 1))，以後 O(1)，追加領域 O(V + D)
 * @throws std::bad_alloc 親子・コメント・トークン末尾・許容エラー又は遮蔽範囲の表を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void ParsedTree::RecordNodeRelations() const {
	// 同じ木で記録済の場合の終了
	if(Syntax.HasParents) return;
	// 未反映編集の再解析を終えてから，此の木の表を確保する
	const TSNode Root = GetRoot();
	// 此の世代の表の共有メモリの準備
	Syntax.Relations = std::make_unique<NodeRelations>();
	// モジュールの節と逐語の URL の所属
	struct ReadingCtx {
		bool CanReachModule = true;
		bool IsInModule = false;
		bool HasClause = false;
		bool IsInUrl = false;
	};
	const GrammarProfile &Profile = GetProfile();
	const ReadingRules &Reading = Profile.GetReading();
	const bool HasScopedErrors = !Reading.ModuleClauseWords.empty() || !Reading.UrlFunctionHead.empty();
	const bool HasDeclCandidates = Reading.HasPreprocMacros, CanReadParenthesized = HasDeclCandidates;
	// 宣言の収集が使う部分木の要約を，親子の共通走査で確定する
	const bool HasNameSummary = Reading.HasPreprocMacros;
	// 記号表の外の記号（エラー領域）は表の末尾の要素で分類する
	std::vector<uint8_t> NameKinds(HasNameSummary ? ts_language_symbol_count(Profile.GetLang().GetTreeSitterLang()) + 1 : 0);
	const auto RecordNameWork = [&](const size_t Relation, const TSSymbol Symbol, const Core::RoleSet &Roles) -> void {
		// 宣言収集の要約を使わない文法の終了
		if(!HasNameSummary) return;
		uint8_t &Kind = NameKinds[std::min<size_t>(Symbol, NameKinds.size() - 1)];
		if(!Kind) {
			Kind = 1;
			for(
				const Core::Role Wanted :
				{
					Core::Role::BareTypeDecl,
					Core::Role::BindingBaseClause,
					Core::Role::CaptureInitializer,
					Core::Role::ClassLike,
					Core::Role::EnumSpecifier,
					Core::Role::EnumerationItem,
					Core::Role::ExprStmt,
					Core::Role::ForRangeLoop,
					Core::Role::FunctionScope,
					Core::Role::GenericParams,
					Core::Role::LeadingConstDecl,
					Core::Role::NameDecl,
					Core::Role::NamespaceAliasDef,
					Core::Role::NamespaceDef,
					Core::Role::ScopeImport
				}
			) if(Roles.test(static_cast<size_t>(Wanted))) {
				Kind = 2;
				break;
			}
		}
		NodeRelation &Record = Syntax.Relations->Parents.Records[Relation];
		if(Kind == 2) {
			Record.HasNameWork = true;
			if(Record.Parent != UINT32_MAX) Syntax.Relations->Parents.Records[Record.Parent].HasNameWork = true;
		}
		// 記号の分類と自身の要約の登録の終了
		return;
	};
	const bool HasBoolNames = GetBoolNameSource() == BoolNameSource::Shadow;
	const bool HasSemanticNodes =
	Reading.HasPreprocMacros || Reading.DoesMisbindPostfixOperand || Reading.DoesMisbindEmbeddedBranches ||
	Profile.FindSymbolWithRole(Core::Role::FunctionScope) || Profile.FindSymbolWithRole(Core::Role::PreprocCall);
	if(HasBoolNames) for(std::vector<std::pair<uint32_t, uint32_t>> &Ranges : BoolShadows) Ranges.clear();
	// モジュールの節の判定
	const auto IsModuleClause = [this](const TSNode Error) -> bool {
		// 対象テキストの取得
		const std::string_view Text = GetView(Error);
		// 節の語の後が区切かの返戻
		return std::any_of(
			GetReading().ModuleClauseWords.begin(),
			GetReading().ModuleClauseWords.end(),
			[&Text](const std::string_view Word) -> bool {
				// 語の境界で一致するかの返戻
				return Text.starts_with(Word) && (Text.size() == Word.size() || std::isspace(static_cast<unsigned char>(Text[Word.size()])));
			}
		);
	};
	// 親子表の同じ訪問で，コメントとリテラル外の隙間を記録する
	Syntax.Comments = std::make_unique<CommentFacts>();
	Syntax.CommentSpans.clear();
	CommentFacts &Facts = *Syntax.Comments;
	// 前処理の走査で確定した，パーサーがトークンの中へ吸うコメントの範囲
	if(GetReading().HasPreprocMacros) {
		for(const LexemeRestoration &Restoration : LexemeRestorations) {
			if(
				!Restoration.HasKindRestoration && (
					Restoration.Original.starts_with(Core::CommentRepository::GetOpenerOf(CommentNotation::Block)) ||
					Restoration.Original.starts_with(Core::CommentRepository::GetOpenerOf(CommentNotation::SlashLine))
				)
			) Facts.Ranges.push_back({ Restoration.Start, Restoration.End, Restoration.Start, false });
		}
	}
	const std::string_view Text = *this;
	const std::vector<Core::AvailableNotation> &Notations = GetRules().Notation.AvailableNotations;
	uint32_t PrevEnd = 0;
	// リテラル内の隙間は本文の一部なのでコメントとして読まない
	struct CommentFrame {
		TSNode Node; // 辿って居る祖先
		bool IsLiteral; // 子の隙間を本文として保つか
		std::vector<TSNode> Pending; // 後続の名前付非コメント兄弟を待つコメント
	};
	std::vector<CommentFrame> Path;
	// トークンの間に残ったコメントの範囲の収集
	const auto AddGap = [&Text, &Notations, &PrevEnd, &Facts](const uint32_t End) -> void {
		if(
			const size_t First = Text.find_first_not_of(" \t\r\n", PrevEnd); First < End && std::ranges::any_of(
				Notations,
				[Rest = Text.substr(First)](const Core::AvailableNotation &Available) -> bool {
					const std::string_view Opener = Core::CommentRepository::GetOpenerOf(Available.Notation);
					// 登録した開始記号と一致するかの返戻
					return !Opener.empty() && Rest.starts_with(Opener);
				}
			)
		) {
			const uint32_t Start = static_cast<uint32_t>(First);
			Facts.Ranges.push_back({ Start, static_cast<uint32_t>(Text.find_last_not_of(" \t\r\n", End - 1) + 1), Start, true });
		}
		// 終了
		return;
	};
	const auto RecordComment = [&](const ParsedChild &Record, const Core::RoleSet &Roles, const uint32_t Count) -> void {
		const TSNode Node = Record.Node;
		++Facts.Visits;
		const bool IsComment = Roles.test(static_cast<size_t>(Core::Role::Comment));
		// 同親の後続兄弟と，コメントを持つ祖先も範囲の収集と同時に確定する
		if(Record.IsNamed && !Path.empty()) {
			if(IsComment) Path.back().Pending.push_back(Node);
			else {
				for(const TSNode Pending : Path.back().Pending) Facts.Following[Pending.id] = Node;
				Path.back().Pending.clear();
			}
		}
		if(IsComment) {
			const std::pair<uint32_t, uint32_t> Range = GetCommentRangeOf(Node);
			Facts.Nodes.push_back(Node);
			Facts.Following.try_emplace(Node.id, TSNode{});
			for(size_t Idx = Path.size(); Idx--;) if(!Facts.Parents.insert(Path[Idx].Node.id).second) break;
			Syntax.CommentSpans.push_back({ Range.first, Range.second, Range.second, false });
			if(Range.first < Range.second) Facts.Ranges.push_back({ Range.first, Range.second, Record.GetStart(), false });
		}
		// コメントでないノードの子だけを辿る
		if(!IsComment && Count) {
			Path.push_back({ Node, Roles.test(static_cast<size_t>(Core::Role::LiteralInterior)), {} });
			// 子の訪問で本文の終わりを確定する為の終了
			return;
		}
		if(const uint32_t Start = Record.GetStart(); Start > PrevEnd && (Path.empty() || !Path.back().IsLiteral)) AddGap(Start);
		// 適応前の木を直接読む場合だけ，マクロ本文の行コメントを補う
		if(Roles.test(static_cast<size_t>(Core::Role::MacroBody)) && LexemeRestorations.empty() && !GetAnalysisMark()) {
			const uint32_t Start = GetMacroLineCommentStart(Node), End = Record.End;
			if(Start < End) {
				Facts.Ranges.push_back({ Start, static_cast<uint32_t>(find_last_not_of(" \t\r\n", End - 1) + 1), Record.GetStart(), false });
			}
		}
		PrevEnd = std::max(PrevEnd, Record.End);
		// 終了
		return;
	};
	const auto FinishComment = [&](const TSNode Node) -> void {
		const bool IsLiteral = Path.back().IsLiteral;
		Path.pop_back();
		if(const uint32_t End = GetEnd(Node); End > PrevEnd) {
			if(!IsLiteral) AddGap(End);
			PrevEnd = End;
		}
		// 終了
		return;
	};
	// 遮蔽の有効範囲は Go 専用の列で継承し，共通の作業枠を大きくしない
	struct BoolScope {
		uint32_t ScopeEnd = 0; // 現在のブロックの終端
		uint32_t BodyStart = 0; // 仮引数を使える関数本体の開始
		uint32_t BodyEnd = 0; // 関数本体の終端
		uint32_t NameStart = 0; // 直接の名前フィールドの有効範囲の開始
		uint32_t NameEnd = 0; // 直接の名前フィールドの有効範囲の終端
		bool IsFile = true; // パッケージ全体へ名前を導入するか
		bool ShouldCollect = true; // 名前付ノードで，関数型の内側でないか
		bool IsShortDecl = false; // 左辺へ名前を導入し得る短い宣言か
		bool IsTypeSwitch = false; // ケース毎の有効範囲へ名前を導入するか
	};
	// 親の直接子の範囲を先に割当て，１本のカーソルで親・子・フィールド・記号を記録する
	struct Frame {
		TSNode Node; // 現在の親
		ReadingCtx Ctx; // 親から継承する読取状態
		size_t Begin; // 直接子の格納先
		size_t Next = 0; // 次の子の添字
		size_t Slot = 0; // 此の親自身の前処理の部分木フラグの格納先（根は使わない）
		bool IsCommentInterior = false; // コメントノードの子孫を範囲収集から除くか
		size_t Relation = 0; // 前順の記録の番号（列の再確保でも変わらない）
	};
	// 未解析の根ではカーソルを作らない
	if(ts_node_is_null(Root)) {
		Syntax.HasParents = Syntax.HasCommentNodes = true;
		// 空の木の収集の終了
		return;
	}
	NodeRelations &Relations = *Syntax.Relations;
	// 記号を取得した訪問で照合の候補を選び，後段の全ノードの種別照会を省く
	const auto RecordSemanticNode = [&](const size_t Relation, const Core::RoleSet &Roles) -> void {
		const SemanticNode Candidate {
			static_cast<uint32_t>(Relation),
			Roles.test(static_cast<size_t>(Core::Role::FunctionScope)),
			Roles.test(static_cast<size_t>(Core::Role::PreprocCall)),
			Reading.HasPreprocMacros && Roles.test(static_cast<size_t>(Core::Role::PreprocNode)),
			Reading.DoesMisbindPostfixOperand && (
				Roles.test(static_cast<size_t>(Core::Role::PostfixBindingOperand)) || Roles.test(static_cast<size_t>(Core::Role::OperatorCall))
			),
			Reading.DoesMisbindEmbeddedBranches && (
				Roles.test(static_cast<size_t>(Core::Role::BodyOwner)) || Roles.test(static_cast<size_t>(Core::Role::If)) ||
				Roles.test(static_cast<size_t>(Core::Role::ElseIfClause))
			),
			Reading.DoesMisbindPostfixOperand && Roles.test(static_cast<size_t>(Core::Role::NameToken)),
			Reading.DoesMisbindPostfixOperand && Roles.test(static_cast<size_t>(Core::Role::PostfixBindingExpr))
		};
		if(
			Candidate.IsFunction || Candidate.IsDirective || Candidate.IsHeader || Candidate.IsOperand || Candidate.IsBody ||
			Candidate.IsName || Candidate.IsPostfix
		) Relations.SemanticNodes.push_back(Candidate);
		// 候補の記録の終了
		return;
	};
	// 名前フィールドと短い宣言が共有する遮蔽範囲の登録
	const auto AddBoolName = [&](const TSNode Name, const uint32_t Start, const uint32_t End) -> void {
		const std::string_view Text = GetView(Name);
		// 真偽値名を遮蔽しない宣言の終了
		if(!GetProfile().IsSpelling(Text, Core::Word::True) && !GetProfile().IsSpelling(Text, Core::Word::False)) return;
		std::vector<std::pair<uint32_t, uint32_t>> &Ranges = BoolShadows[GetProfile().IsSpelling(Text, Core::Word::True)];
		Ranges.emplace_back(GetStart(Name), GetEnd(Name));
		if(Start < End) Ranges.emplace_back(Start, End);
		// 範囲の登録を完了した場合の終了
		return;
	};
	// 親の範囲から，直接の名前と子孫の範囲を分けて確定する
	// @param Node 範囲を確定する対象
	// @param Parent 直接の親（根では空）
	// @param Roles 此の訪問で取得済の種別の役割
	// @param Scope 親から継承し，子へ渡す有効範囲
	const auto PrepareBoolScope =
	[&](const TSNode Node, const TSNode Parent, const Core::RoleSet &Roles, BoolScope &Scope) -> void {
		// 名前の遮蔽を収集しないノードの終了
		if(!Scope.ShouldCollect) return;
		const auto Has = [&Roles](const Core::Role Wanted) -> bool {
			// 共通カーソルが取得した役割の返戻
			return Roles.test(static_cast<size_t>(Wanted));
		};
		if(Has(Core::Role::FunctionType)) {
			Scope.ShouldCollect = false;
			// 関数型の仮引数を外側の範囲へ持ち出さない為の終了
			return;
		}
		const bool IsFunction = Has(Core::Role::FunctionDeclNode) || Has(Core::Role::MethodDecl) || Has(Core::Role::FunctionLiteral);
		const bool IsParam = Has(Core::Role::ParamDecl) || Has(Core::Role::VariadicParamDecl);
		if(Has(Core::Role::ExpandedTypeParams)) {
			Scope.ScopeEnd = GetEnd(Parent);
			Scope.IsFile = false;
		}
		Scope.IsShortDecl = Has(Core::Role::ShortVarDecl) || Has(Core::Role::RangeClause) || Has(Core::Role::ReceiveStmt);
		Scope.IsTypeSwitch = Has(Core::Role::TypeSwitch);
		Scope.NameStart = IsParam ? Scope.BodyStart : Scope.IsFile ? 0 : GetEnd(Node);
		Scope.NameEnd = IsParam ? Scope.BodyEnd : Scope.ScopeEnd;
		if(IsFunction) {
			const TSNode Body = GetFieldChild(Node, Core::ChildRole::Body);
			Scope.BodyStart = GetStart(Body);
			Scope.BodyEnd = GetEnd(Body);
			Scope.ScopeEnd = Scope.BodyEnd;
			Scope.IsFile = false;
		} else if(
			Has(Core::Role::LexicalBlock) || Has(Core::Role::IfStmtNode) || Has(Core::Role::Loop) || Has(Core::Role::SwitchStmtLike) ||
			Has(Core::Role::CaseClause)
		) {
			Scope.ScopeEnd = GetEnd(Node);
			Scope.IsFile = false;
		}
		// 子へ渡す範囲の確定後の終了
		return;
	};
	// 子列を埋めた同じ訪問で，短い宣言と型スイッチの範囲を収集する
	const auto RecordBoolNames = [&](const TSNode Node, const size_t Relation, const BoolScope &Scope) -> void {
		// 関数型の内部と無名ノードの終了
		if(!Scope.ShouldCollect) return;
		if(Scope.IsShortDecl) {
			const std::span<const ParsedChild> Children = GetChildrenAt(Relation);
			bool HasShortAssignment = false;
			for(const ParsedChild &Child : Children) {
				HasShortAssignment = Is(Child.Node, Core::Role::ShortAssignmentToken);
				if(HasShortAssignment) break;
			}
			if(HasShortAssignment) {
				const TSNode Left = GetFieldChild(Node, Core::ChildRole::Left);
				for(uint32_t Idx = 0; !ts_node_is_null(Left) && Idx < ts_node_named_child_count(Left); ++Idx) {
					AddBoolName(ts_node_named_child(Left, Idx), GetEnd(Node), Scope.ScopeEnd);
				}
			}
		}
		if(Scope.IsTypeSwitch) {
			const TSNode Alias = GetFieldChild(Node, Core::ChildRole::Alias);
			for(uint32_t Idx = 0; !ts_node_is_null(Alias) && Idx < ts_node_named_child_count(Alias); ++Idx) {
				const TSNode Name = ts_node_named_child(Alias, Idx);
				AddBoolName(Name, 0, 0);
				for(uint32_t CaseIdx = 0; CaseIdx < ts_node_named_child_count(Node); ++CaseIdx) {
					const TSNode Case = ts_node_named_child(Node, CaseIdx);
					if(Is(Case, Core::Role::TypeCase) || Is(Case, Core::Role::DefaultCase)) AddBoolName(Name, GetStart(Case), GetEnd(Case));
				}
			}
		}
		// 子の確定後の収集の終了
		return;
	};
	// 既知のノード数で直接予約し，共有メモリの余剰チャンクを作らない
	const uint32_t NodeCount = ts_node_descendant_count(Root);
	Relations.Parents.Reserve(NodeCount);
	// 葉の列は此の訪問でだけ作り，個別のハッシュ要素と成長時の再確保を避ける
	if(Reading.HasLineSensitiveBoundary) Relations.LeafEnds.reserve(NodeCount);
	// 全ての枠をカーソルの訪問で上書きする為，読取前の零埋と未記録の枠の移動を行わない
	static_assert(std::is_trivially_default_constructible_v<ParsedChild>);
	static_assert(std::is_trivially_destructible_v<ParsedChild>);
	Relations.ChildData = std::make_unique_for_overwrite<ParsedChild[]>(NodeCount);
	ParsedChild *const ChildData = Relations.ChildData.get();
	Relations.Parents.RootId = Root.id;
	Relations.Parents.Children = { ChildData, NodeCount };
	if(Reading.HasPreprocMacros) Relations.MacroSubtrees.resize(NodeCount, 0);
	const uint32_t FieldCount = ts_language_field_count(Profile.GetLang().GetTreeSitterLang());
	for(TSFieldId Field = 1; Field <= FieldCount; ++Field) if(const uint64_t Bits = Profile.GetFieldBits(Field)) {
		Relations.SemanticFields.emplace_back(Field, Bits);
	}
	// 受理の事実も同じ世代の訪問で求め，判定の優先順は受理側に残す
	const bool HasAdmission = Reading.HasIndentBlocks || Reading.CanAcceptArrowOperand || Reading.CanAcceptContinuationComment ||
	Reading.CanAcceptStmtLineBreak || Reading.CanAcceptVerticalTabSpace || Reading.CanAcceptFormFeedSpace ||
	!Reading.AutomaticTerminators.empty();
	std::string Rejected;
	if(Reading.CanAcceptVerticalTabSpace) Rejected += '\v';
	if(Reading.CanAcceptFormFeedSpace) Rejected += '\f';
	std::vector<uint32_t> TerminatorEnds;
	uint32_t TokenEnd = 0, AllowedEnd = 0;
	bool CanEndToken = false;
	size_t AdmissionEnd = 0;
	const auto RecordAdmission =
	[&](const TSNode Node, const Core::RoleSet &Roles, const uint32_t Count, const size_t Slot) -> void {
		++Relations.AdmissionWork;
		if(
			Reading.HasIndentBlocks && !Relations.EmptyBlock && !ts_node_eq(Node, Root) && !GetLen(Node) &&
			Roles.test(static_cast<size_t>(Core::Role::LexicalBlock))
		) Relations.EmptyBlock = GetStart(Node);
		if(
			Reading.CanAcceptArrowOperand && !Relations.ArrowOperand && Roles.test(static_cast<size_t>(Core::Role::OperandHeadedAccess))
		) {
			const TSNode Object = ts_node_child(Node, 0);
			if(!ts_node_is_null(Object) && Is(Object, Core::Role::ArrowFunction)) {
				if(const TSNode Next = ts_node_child(Node, 1); !ts_node_is_null(Next)) {
					const uint32_t From = GetEnd(Object), To = GetStart(Next);
					if(std::string_view(data() + From, To - From).find('\n') == std::string_view::npos) Relations.ArrowOperand = GetStart(Object);
				}
			}
		}
		if(
			Reading.CanAcceptContinuationComment && !Relations.ContinuationComment &&
			Roles.test(static_cast<size_t>(Core::Role::LineContinuation))
		) {
			const TSNode Next = ts_node_next_sibling(Node);
			if(!ts_node_is_null(Next) && Is(Next, Core::Role::Comment)) Relations.ContinuationComment = GetStart(Next);
		}
		// 自動の文終端の成立は，同じ前順の訪問でトークンと完結した子の境界から求める
		if(
			!Reading.AutomaticTerminators.empty() && !Relations.RejectedTerminator && !ts_node_eq(Node, Root) &&
			!Roles.test(static_cast<size_t>(Core::Role::Comment)) && GetStart(Node) >= TokenEnd
		) {
			const ParsedChild &Record = Relations.Children[Slot];
			const uint32_t Start = Record.GetStart(), End = Record.End;
			if(IsError(Node)) {
				// 局所化したエラー領域は文境界の推測に使わず，既存の部分整形へ渡す
				TokenEnd = End;
				CanEndToken = false;
			} else {
				const NodeRelation &Parent = Relations.Parents.Records[Relations.Parents.Records[Record.Relation].Parent];
				const TSSymbol ParentSymbol = Parent.Parent == UINT32_MAX ? Relations.Root.Symbol : Relations.Children[Parent.Slot].Symbol;
				const AutomaticTerminatorReading Def = GetAutomaticTerminatorReading(Reading, Record.Symbol);
				while(!TerminatorEnds.empty() && TerminatorEnds.back() <= Start) {
					AllowedEnd = TerminatorEnds.back();
					TerminatorEnds.pop_back();
				}
				if(GetAutomaticTerminatorReading(Reading, ParentSymbol).IsHost && Record.IsNamed) TerminatorEnds.push_back(End);
				if(Start != End && (!Count || Def.IsWholeToken)) {
					++Relations.AdmissionLeafWork;
					if(CanEndToken && AllowedEnd != TokenEnd) {
						const std::string_view Gap = std::string_view(*this).substr(TokenEnd, Start - TokenEnd);
						const size_t Newline = Gap.find('\n');
						Relations.TerminatorGapWork += std::min(Newline, Gap.size()) + (Newline != std::string_view::npos);
						if(Newline != std::string_view::npos) Relations.RejectedTerminator = TokenEnd + static_cast<uint32_t>(Newline);
					}
					TokenEnd = End;
					CanEndToken = Def.IsToken;
				}
			}
		}
		// 本文を１トークンとして記録する為，本文以外のコンテナと記録済の内部の終了
		if(Count && !Roles.test(static_cast<size_t>(Core::Role::LiteralContent)) || GetStart(Node) < AdmissionEnd) return;
		if(Reading.CanAcceptStmtLineBreak && !ts_node_eq(Node, Root)) Relations.AdmissionLeaves.push_back(Slot);
		if(!Rejected.empty() && !Relations.RejectedSpace) {
			const std::string_view Code = GetCodeText();
			const size_t Start = std::min<size_t>(GetStart(Node), Code.size());
			const size_t Pos =
			Start > AdmissionEnd ? Code.substr(AdmissionEnd, Start - AdmissionEnd).find_first_of(Rejected) : std::string_view::npos;
			if(Pos != std::string_view::npos) Relations.RejectedSpace = static_cast<uint32_t>(AdmissionEnd + Pos);
		}
		AdmissionEnd = std::max<size_t>(AdmissionEnd, GetEnd(Node));
		// 終了
		return;
	};
	// 値の受理判定の位置を記録し，内部を検査するかを返す関数
	// 計算量：リテラルの長さ B に対し O(B)，其の他は O(1)
	// @param Node 根を除く前順の記録済の対象ノード
	// @param Roles 対象の文法の役割
	// @return 値の受理判定が子を検査する場合は真
	const auto RecordVal = [&](const ParsedChild &Node, const Core::RoleSet &Roles) -> bool {
		if(
			Reading.ShouldRejectComments && Roles.test(static_cast<size_t>(Core::Role::Comment)) ||
			Reading.HasSingleRootVal && !Reading.CanTolerateTrailingCommaError && Roles.test(static_cast<size_t>(Core::Role::Error)) &&
			GetProfile().IsSpelling(GetView(Node.Node), Core::Word::ListSeparator)
		) {
			if(!Relations.RejectedVal) Relations.RejectedVal = Node.GetStart();
			// 文法が認めないコメントと区切の内部を検査しない事の返戻
			return false;
		}
		if(Reading.ShouldHaveFractionDigits && Roles.test(static_cast<size_t>(Core::Role::NumLiteral))) {
			const uint32_t Start = Node.GetStart();
			const std::string_view Text = Start < Node.End ? std::string_view(data() + Start, Node.End - Start) : std::string_view();
			if(
				const size_t Fraction = Text.find('.');
				!Relations.RejectedVal && Fraction != std::string_view::npos &&
				(Fraction + 1 == Text.size() || !std::isdigit(static_cast<unsigned char>(Text[Fraction + 1])))
			) Relations.RejectedVal = Start + static_cast<uint32_t>(Fraction);
			// 数値の内部を検査しない事の返戻
			return false;
		}
		// 文字列でないノードの子を検査する事の返戻
		if(!Reading.HasStrictStrEscapes || !Roles.test(static_cast<size_t>(Core::Role::QuotedStr))) return !Relations.RejectedVal;
		const uint32_t Start = Node.GetStart();
		const std::string_view Text = Start < Node.End ? std::string_view(data() + Start, Node.End - Start) : std::string_view();
		bool HasRejected = Relations.RejectedVal.has_value();
		for(size_t Pos = 1; Pos + 1 < Text.size() && !HasRejected; ++Pos) {
			if(static_cast<unsigned char>(Text[Pos]) < 0X20) HasRejected = true;
			else if(Text[Pos] == '\\') {
				++Pos;
				if(Pos + 2 > Text.size()) HasRejected = true;
				else if(Text[Pos] == 'u') for(uint32_t Digit = 0; Digit < 4 && !HasRejected; ++Digit) {
					++Pos;
					HasRejected = Pos + 2 > Text.size() || !std::isxdigit(static_cast<unsigned char>(Text[Pos]));
				} else HasRejected = std::string_view("\"\\/bfnrt").find(Text[Pos]) == std::string_view::npos;
			}
			if(HasRejected) Relations.RejectedVal = Start + static_cast<uint32_t>(Pos);
		}
		// 文字列の内部を検査しない事の返戻
		return false;
	};
	const uint32_t RootCount = ts_node_child_count(Root);
	const TSSymbol RootSymbol = ts_node_symbol(Root);
	const Core::RoleSet &RootRoles = Profile.GetKindOf(RootSymbol).Roles;
	Relations.Children = { ChildData, RootCount };
	const size_t RootRelation = Relations.Parents.Add(Root.id, NodeRelation{ UINT32_MAX, 0, RootCount });
	RecordNameWork(RootRelation, RootSymbol, RootRoles);
	Relations.Root = {
		Root,
		0,
		RootSymbol,
		static_cast<uint32_t>(RootRelation),
		0,
		ts_node_end_byte(Root),
		ts_node_is_named(Root),
		ts_node_is_missing(Root),
		ts_node_has_error(Root)
	};
	std::vector<Frame> Frames {
		{ Root, { true, RootRoles.test(static_cast<size_t>(Core::Role::ModuleRule)), false, false }, 0, 0, 0, false, RootRelation }
	};
	std::vector<BoolScope> BoolScopes;
	if(HasBoolNames) {
		BoolScopes.emplace_back();
		BoolScopes.back().ScopeEnd = GetEnd(Root);
		PrepareBoolScope(Root, TSNode{}, RootRoles, BoolScopes.back());
	}
	if(HasSemanticNodes) RecordSemanticNode(RootRelation, RootRoles);
	RecordComment(Relations.Root, RootRoles, RootCount);
	if(HasAdmission) RecordAdmission(Root, RootRoles, RootCount, 0);
	Frames.back().IsCommentInterior = RootRoles.test(static_cast<size_t>(Core::Role::Comment));
	size_t SoftKeywordDepth = 0, ValDepth = 0; // 最外の型の別名の文の深さ（０は其の外側） // 値の検査が内部を除外する数値又は文字列の深さ
	uint32_t RootVals = 0; // 単一の値を要求する文法の根の直接子の数
	const bool HasValAdmission = Reading.HasStrictStrEscapes || Reading.ShouldHaveFractionDigits;
	const bool HasBracketAdmission = Relations.Root.HasError && !Reading.CanRecoveryRestructureDoc;
	HeldCursor Cursor(Root);
	if(RootCount && ts_tree_cursor_goto_first_child(&Cursor)) for(bool IsDone = false; !IsDone;) {
		const TSNode Child = ts_tree_cursor_current_node(&Cursor);
		const TSSymbol Symbol = ts_node_symbol(Child);
		const Core::RoleSet &Roles = Profile.GetKindOf(Symbol).Roles;
		++Relations.SymbolReads;
		const auto Has = [&Roles](const Core::Role Wanted) -> bool {
			// 此の訪問で確定した種別の役割の返戻
			return Roles.test(static_cast<size_t>(Wanted));
		};
		Frame &Parent = Frames.back();
		const bool IsCommentInterior = Parent.IsCommentInterior;
		const uint32_t ChildCount = ts_node_child_count(Child);
		const size_t Slot = Parent.Begin + Parent.Next++;
		const TSFieldId Field = ts_tree_cursor_current_field_id(&Cursor);
		if(Field) Relations.Parents.Records[Parent.Relation].HasInnerFields = true;
		Relations.Children[Slot] = {
			Child,
			Field,
			Symbol,
			0,
			Field ? Profile.GetFieldBits(Field) : 0,
			ts_node_end_byte(Child),
			ts_node_is_named(Child),
			ts_node_is_missing(Child),
			Relations.Root.HasError && ts_node_has_error(Child)
		};
		const size_t Begin = Relations.Children.size();
		Relations.Children = { ChildData, Begin + ChildCount };
		const size_t ChildRelation = Relations.Parents.Add(
			Child.id,
			NodeRelation{ static_cast<uint32_t>(Parent.Relation), static_cast<uint32_t>(Begin), ChildCount, static_cast<uint32_t>(Slot) }
		);
		Relations.Children[Slot].Relation = static_cast<uint32_t>(ChildRelation);
		RecordNameWork(ChildRelation, Symbol, Roles);
		if(Relations.Children[Slot].IsNamed && Relations.Parents.Records[Parent.Relation].FirstNamedSlot == UINT32_MAX) {
			Relations.Parents.Records[Parent.Relation].FirstNamedSlot = static_cast<uint32_t>(Slot);
		}
		if(Reading.HasSingleRootVal && Frames.size() == 1 && Relations.Children[Slot].IsNamed && !Has(Core::Role::Comment)) {
			if(++RootVals == 2) Relations.ExtraRootVal = Relations.Children[Slot].GetStart();
		}
		if(HasValAdmission) {
			if(Frames.size() <= ValDepth) ValDepth = 0;
			if(!ValDepth && !RecordVal(Relations.Children[Slot], Roles)) ValDepth = Frames.size();
		}
		if(HasBracketAdmission) {
			if(Has(Core::Role::OpeningBraceToken) || Has(Core::Role::OpeningBracketToken) || Has(Core::Role::OpeningParenToken)) {
				++Relations.OpenDepth;
			} else if(Has(Core::Role::ClosingBraceToken) || Has(Core::Role::ClosingParenToken) || Has(Core::Role::ClosingSquareToken)) {
				--Relations.OpenDepth;
			}
		}
		if(HasSemanticNodes) RecordSemanticNode(ChildRelation, Roles);
		if(!IsCommentInterior) RecordComment(Relations.Children[Slot], Roles, ChildCount);
		++Relations.ReadWork;
		if(Reading.HasPreprocMacros && Has(Core::Role::Preproc)) {
			Relations.HasMacroCandidates = true;
			Relations.MacroSubtrees[Slot] = 1;
			if(Frames.size() > 1) Relations.MacroSubtrees[Parent.Slot] = 1;
		}
		if(
			HasBoolNames && (
				GetLen(Child) == GetRules().GetSpelling(Core::Word::True).size() ||
				GetLen(Child) == GetRules().GetSpelling(Core::Word::False).size()
			)
		) {
			const std::string_view Text = GetView(Child);
			if(GetProfile().IsSpelling(Text, Core::Word::True) || GetProfile().IsSpelling(Text, Core::Word::False)) {
				Relations.HasBoolCandidates = true;
				if(BoolScopes.back().ShouldCollect && !Has(Core::Role::FieldName)) {
					// 最内のフィールドと外側のフィールドを取り違えず，遮蔽候補だけを従来の定義で照会する
					const char *const NameField = ts_node_field_name_for_child(Parent.Node, static_cast<uint32_t>(Parent.Next - 1));
					if(NameField && Profile.GetChildRoleOf(std::string_view(NameField)) == Core::ChildRole::Name) {
						AddBoolName(Child, BoolScopes.back().NameStart, BoolScopes.back().NameEnd);
					}
				}
			}
		}
		if(Reading.HasJsxText && !Relations.HasJsxCandidates) Relations.HasJsxCandidates = Has(Core::Role::RenderedChildren);
		if(HasDeclCandidates && (Has(Core::Role::PotentialExprDecl) || CanReadParenthesized && Has(Core::Role::ExprStmt))) {
			Relations.DeclCandidates.push_back(static_cast<uint32_t>(ChildRelation));
		}
		if(Reading.CanMisreadSoftKeywordCalls) {
			if(Frames.size() <= SoftKeywordDepth) SoftKeywordDepth = 0;
			if(!SoftKeywordDepth && Has(Core::Role::TypeAlias)) {
				Relations.SoftKeywordCandidates.push_back(static_cast<uint32_t>(ChildRelation));
				SoftKeywordDepth = Frames.size();
			}
		}
		if(Reading.ShouldRequireFunctionDeclarator && Has(Core::Role::FunctionDef)) {
			Relations.FunctionDefCandidates.push_back(static_cast<uint32_t>(ChildRelation));
		}
		if(HasAdmission) RecordAdmission(Child, Roles, ChildCount, Slot);
		if(Reading.HasLineSensitiveBoundary && !ChildCount && Relations.Children[Slot].GetStart() < Relations.Children[Slot].End) {
			Relations.LeafEnds.push_back(static_cast<uint32_t>(Slot));
		}
		ReadingCtx Ctx = Parent.Ctx;
		if(HasScopedErrors) {
			if(!Ctx.IsInModule && Ctx.CanReachModule) {
				if(Has(Core::Role::ModuleRule)) Ctx.IsInModule = true;
				else if(!Has(Core::Role::Error)) Ctx.CanReachModule = false;
			}
			if(Ctx.IsInModule && Has(Core::Role::Error)) {
				Ctx.HasClause = Ctx.HasClause || IsModuleClause(Child);
				if(Ctx.HasClause) Relations.ModuleErrors.insert(Child.id);
			}
			if(!Ctx.IsInUrl && IsCssUrlArgs(Child)) {
				const std::string_view Args = GetView(Child);
				const size_t Content = Args.find_first_not_of(" \t\r\n\f", 1);
				Ctx.IsInUrl = Content != std::string_view::npos && Args[Content] != '"' && Args[Content] != '\'';
			}
			const bool IsMissingUrl = Ctx.IsInUrl && Relations.Children[Slot].IsMissing && Has(Core::Role::NestingSelector);
			if(IsMissingUrl) Relations.UrlValErrors.insert(Child.id);
		}
		const bool ShouldDescend = ChildCount && ts_tree_cursor_goto_first_child(&Cursor);
		if(HasBoolNames) {
			BoolScope Bools = BoolScopes.back();
			Bools.ShouldCollect = Bools.ShouldCollect && Relations.Children[Slot].IsNamed;
			PrepareBoolScope(Child, Parent.Node, Roles, Bools);
			if(ShouldDescend) BoolScopes.push_back(Bools);
			else RecordBoolNames(Child, ChildRelation, Bools);
		}
		if(ShouldDescend) {
			Frames.push_back({ Child, Ctx, Begin, 0, Slot, IsCommentInterior || Has(Core::Role::Comment), ChildRelation });
			continue;
		}
		while(!ts_tree_cursor_goto_next_sibling(&Cursor)) {
			if(!ts_tree_cursor_goto_parent(&Cursor)) {
				IsDone = true;
				break;
			}
			if(!Frames.back().IsCommentInterior) FinishComment(Frames.back().Node);
			if(HasBoolNames) {
				RecordBoolNames(Frames.back().Node, Frames.back().Relation, BoolScopes.back());
				BoolScopes.pop_back();
			}
			// 完了した部分木の前処理フラグを直接親へ継承
			if(Reading.HasPreprocMacros && Frames.size() > 2 && Relations.MacroSubtrees[Frames.back().Slot]) {
				Relations.MacroSubtrees[Frames[Frames.size() - 2].Slot] = 1;
			}
			// 完了した子の宣言の要約を直接親へ１度だけ渡す
			if(HasNameSummary && Frames.size() > 1 && Relations.Parents.Records[Frames.back().Relation].HasNameWork) {
				Relations.Parents.Records[Frames[Frames.size() - 2].Relation].HasNameWork = true;
			}
			Frames.pop_back();
		}
	}
	if(HasBoolNames && !RootCount) RecordBoolNames(Root, RootRelation, BoolScopes.front());
	// ネストした遮蔽の範囲を統合し，使用位置を二分探索で照会する
	if(HasBoolNames) for(std::vector<std::pair<uint32_t, uint32_t>> &Ranges : BoolShadows) {
		std::ranges::sort(Ranges);
		size_t Count = 0;
		for(const std::pair<uint32_t, uint32_t> &Range : Ranges) {
			if(Count && Range.first <= Ranges[Count - 1].second) {
				Ranges[Count - 1].second = std::max(Ranges[Count - 1].second, Range.second);
			} else Ranges[Count++] = Range;
		}
		Ranges.resize(Count);
	}
	if(!Rejected.empty() && !Relations.RejectedSpace) {
		const std::string_view Code = GetCodeText();
		if(AdmissionEnd < Code.size()) {
			const size_t Pos = Code.substr(AdmissionEnd).find_first_of(Rejected);
			if(Pos != std::string_view::npos) Relations.RejectedSpace = static_cast<uint32_t>(AdmissionEnd + Pos);
		}
	}
	if(size() > PrevEnd) AddGap(static_cast<uint32_t>(size()));
	Syntax.HasParents = Syntax.HasCommentNodes = true;
	// 終了
	return;
}

/**
 * 同じ木で取得済の記録番号から直接子の範囲を読む関数
 * 計算量：O(1)
 * @param Relation 親子表の記録番号（関係の収集から木の編集迄だけ有効）
 * @return 原文順の子・フィールド・記号
 * @throws std::out_of_range 記録番号が親子表の範囲外の場合
 */
std::span<const ParsedChild> ParsedTree::GetChildrenAt(const size_t Relation) const {
	const NodeRelation &Record = Syntax.Relations->Parents.Records.at(Relation);
	return std::span<const ParsedChild>(Syntax.Relations->Children).subspan(Record.Begin, Record.Count);
} // 記録済の子列の返戻

/**
 * 投影前に隠れた外側のフィールドと最初の子を確定する関数
 * カーソルの最内のフィールドへ外側の役割を加え，後の照会には変更しない表を返す
 * 計算量：意味を持つフィールド数 F，親子表の費用 R に対し初回平均 O(F + Q + R)，補完済なら平均 O(1)（パーサーのフィールド照会の総費用 Q）
 * @param Node 投影する親
 * @param Relation 同じ木で取得済の記録番号（無ければ識別子で探す）
 * @throws std::out_of_range 記録番号が親子表の範囲外の場合
 * @throws std::bad_alloc 親子表又は最初の子の表を確保出来ない場合
 * @throws std::length_error 表の要素数が上限を超える場合
 */
void ParsedTree::PrepareChildFields(const TSNode Node, const size_t Relation) const {
	// 空のノードの終了
	if(ts_node_is_null(Node)) return;
	RecordNodeRelations();
	NodeRelations &Relations = *Syntax.Relations;
	NodeRelation *const Found =
	Relation == std::string::npos ? Relations.Parents.Find(Node.id) : &Relations.Parents.Records.at(Relation);
	// 此の木に属さないノードと補完済の親の終了
	if(!Found || Found->HasFieldsReady) return;
	NodeRelation &Parent = *Found;
	const size_t ParentRelation = static_cast<size_t>(Found - Relations.Parents.Records.data());
	// 最内のフィールドが無い親は，従来の投影と同じく外側のフィールドも補わない
	if(Parent.HasInnerFields) {
		if(Relations.FirstFields.size() > UINT32_MAX) throw std::length_error("field index exceeds capacity");
		Parent.FieldsBegin = static_cast<uint32_t>(Relations.FirstFields.size());
		for(const std::pair<TSFieldId, uint64_t> &Field : Relations.SemanticFields) {
			const TSNode First = ts_node_child_by_field_id(Node, Field.first);
			++Relations.FieldWork;
			if(ts_node_is_null(First)) continue;
			// 短い直接子列は既知の格納先で照合し，隠れた子孫と長い子列だけ識別子の表を引く
			const NodeRelation *Child = nullptr;
			if(Parent.Count < 9) for(const ParsedChild &Part : GetChildrenAt(ParentRelation)) if(Part.Node.id == First.id) {
				Child = &Relations.Parents.Records[Part.Relation];
				break;
			}
			if(!Child) Child = Relations.Parents.Find(First.id);
			const TSNode Stored = Child ? GetNodeAt(static_cast<size_t>(Child - Relations.Parents.Records.data())).Node : TSNode{};
			const bool IsStored = Child && First.tree == Stored.tree && std::ranges::equal(First.context, Stored.context);
			if(IsStored) {
				Relations.FirstFields.push_back({ Field.first, true, static_cast<uint32_t>(Child - Relations.Parents.Records.data()) });
			} else {
				if(Relations.FieldNodes.size() > UINT32_MAX - 1) throw std::length_error("parser field nodes exceed their index range");
				Relations.FirstFields.push_back({ Field.first, false, static_cast<uint32_t>(Relations.FieldNodes.size()) });
				Relations.FieldNodes.push_back(First);
			}
			if(Child && Child->Parent == ParentRelation && Node.tree == Syntax.CachedRoot.tree) {
				Relations.Children[Child->Slot].Fields |= Field.second;
			}
		}
		Parent.FieldsCount = static_cast<uint16_t>(Relations.FirstFields.size() - Parent.FieldsBegin);
		Parent.HasFieldsReady = true;
	}
	// 終了
	return;
}

/**
 * 区間の列を世代の共有メモリへ置く生成関数
 * 計算量：O(1)，要素は最初の束縛の追加で確保する
 * @param Storage 列の破棄迄生存する共有メモリ
 */
ParsedTree::NameIndex::NameIndex(std::pmr::memory_resource *const Storage) : Bindings(Storage), MaxEnds(Storage) {}

/**
 * 使用位置で有効な最寄の区間の取得関数
 * 計算量：区間数 D に対し O(log(D + 1))
 * @param Use 使用位置
 * @param Work 所有スレッドの区間探索の作業予算（並列区間なら無）
 * @param ValidUntil 結果が変わり得る次の位置の最小値（不要なら無）
 * @return 使用位置より前で有効な最寄の区間，無ければ無
 * @throws FormatLimitExceeded 累積作業量が計数範囲を超える場合
 */
const ParsedTree::NameBinding *ParsedTree::NameIndex::Find(
	const uint32_t Use,
	Core::StructureWorkAccount *const Work,
	uint32_t *const ValidUntil
) const {
	// 単一の束縛は区間の木を持たず，位置と終端を直接照合する
	if(Capacity == 1) {
		// 束縛の無い範囲の返戻
		if(Bindings.empty()) return nullptr;
		if(Work) Work->Charge(1);
		// 使用位置より後の宣言の返戻
		if(Bindings.front().Pos >= Use) {
			if(ValidUntil && Bindings.front().Pos != UINT32_MAX) *ValidUntil = std::min(*ValidUntil, Bindings.front().Pos + 1);
			// 使用位置では未だ有効でない宣言の返戻
			return nullptr;
		}
		if(Work) Work->Charge(1);
		// 使用位置より前に終了した束縛の返戻
		if(Use >= Bindings.front().End) return nullptr;
		if(ValidUntil) *ValidUntil = std::min(*ValidUntil, Bindings.front().End);
		// 使用位置を含む単一の束縛の返戻
		return &Bindings.front();
	}
	const std::pmr::vector<NameBinding>::const_iterator After = std::lower_bound(
		Bindings.begin(),
		Bindings.end(),
		Use,
		[&](const NameBinding &Decl, const uint32_t Pos) -> bool {
			if(Work) Work->Charge(1);
			// 使用位置より前の宣言かの返戻
			return Decl.Pos < Pos;
		}
	);
	if(ValidUntil && After != Bindings.end()) {
		if(Work) Work->Charge(1);
		if(After->Pos != UINT32_MAX) *ValidUntil = std::min(*ValidUntil, After->Pos + 1);
	}
	// 使用位置より前の列を右から区間単位で調べ，終了済の区間全体を飛ばす
	for(
		size_t Begin = Capacity, End = Capacity + static_cast<size_t>(After - Bindings.begin());
		Begin < End;
		Begin >>= 1, End >>= 1
	) {
		if(Work) Work->Charge(1);
		size_t Idx = End & 1 ? --End : Begin & 1 ? Begin++ : 0;
		if(!Idx || MaxEnds[Idx] <= Use) continue;
		while(Idx < Capacity) {
			if(Work) Work->Charge(1);
			Idx <<= 1;
			if(MaxEnds[Idx | 1] > Use) Idx |= 1;
		}
		// 使用位置より前で有効な最も右の束縛
		const NameBinding &Binding = Bindings[Idx - Capacity];
		if(ValidUntil) *ValidUntil = std::min(*ValidUntil, Binding.End);
		// 最も右の束縛の返戻
		return &Binding;
	}
	// 使用位置で有効な宣言が無い事の返戻
	return nullptr;
}

/**
 * 同名の宣言と有効範囲の区間インデックスの追加関数
 * 計算量：同名の宣言数 D に対し償却 O(log(D + 1))，追加領域 O(D)
 * @param Binding 位置順に追加する宣言
 * @throws std::bad_alloc 束縛と区間のメモリを確保出来ない場合
 * @throws std::length_error 記録の要素数の上限を超える場合
 */
void ParsedTree::NameIndex::Add(const NameBinding Binding) {
	// 最初の束縛だけなら最大終端の表を確保しない
	if(Bindings.empty()) {
		Bindings.push_back(Binding);
		// 単一の束縛の登録の終了
		return;
	}
	// 容量を倍増し，既存の葉から区間の最大値を１度組み立てる
	if(Bindings.size() == Capacity) {
		// 容量と区間の要素数のシフト前の上限検査
		if(Capacity > MaxEnds.max_size() >> 2) throw std::length_error("binding index exceeds the element limit");
		Capacity <<= 1;
		MaxEnds.assign(Capacity << 1, 0);
		for(size_t Idx = 0; Idx < Bindings.size(); ++Idx) MaxEnds[Capacity + Idx] = Bindings[Idx].End;
		for(size_t Idx = Capacity; --Idx;) MaxEnds[Idx] = std::max(MaxEnds[Idx << 1], MaxEnds[Idx << 1 | 1]);
	}
	size_t Idx = Capacity + Bindings.size();
	Bindings.push_back(Binding);
	MaxEnds[Idx] = Binding.End;
	// 追加した葉の経路だけを更新する
	while(Idx >>= 1) MaxEnds[Idx] = std::max(MaxEnds[Idx << 1], MaxEnds[Idx << 1 | 1]);
	// 終了
	return;
}

/**
 * 所有スレッドだけへ束縛の作業計数先を渡す関数
 * 計算量：O(1)
 * @return 共有木の並列照会中は無，其の他は此の木の計数先
 */
Core::StructureWorkAccount *ParsedTree::GetNameWorkCounter() const noexcept {
	return IsReadingNamesInParallel ? nullptr : &NameWork;
}

/**
 * 所有スレッドの束縛の照会だけを計上する関数
 * 計算量：O(1)
 * @throws FormatLimitExceeded 累積作業量が計数範囲を超える場合
 */
void ParsedTree::ChargeNameWork() const {
	if(!IsReadingNamesInParallel) NameWork.Charge(1);
	// 終了
	return;
}

/**
 * ２つの所属の最も近い共通の外側の取得関数
 * 所属の深さ H に対し O(H)
 * @param Left 第１の所属
 * @param Right 第２の所属
 * @return 共通の外側の所属
 */
size_t ParsedTree::GetCommonNameScope(size_t Left, size_t Right) const {
	while(NameData->Bindings[Left].Depth > NameData->Bindings[Right].Depth) {
		Left = NameData->Bindings[Left].Parent;
		ChargeNameWork();
	}
	while(NameData->Bindings[Right].Depth > NameData->Bindings[Left].Depth) {
		Right = NameData->Bindings[Right].Parent;
		ChargeNameWork();
	}
	while(Left != Right) {
		Left = NameData->Bindings[Left].Parent;
		Right = NameData->Bindings[Right].Parent;
		ChargeNameWork();
	}
	// 最も近い共通の外側の返戻
	return Left;
}

/**
 * 導入辺の追加と，此の所属へ達する経路だけへの差分の登録関数
 * 計算量：影響する所有者数 S，照合する区間数 R に対し平均 O(S log(R + 1) + R)
 * @param Owner 導入を置く所属
 * @param Import 新たな導入先と有効範囲
 * @throws std::bad_alloc 導入辺又は差分のメモリを確保出来ない場合
 * @throws std::length_error 導入辺又は差分の要素数上限を超える場合
 */
void ParsedTree::AddNameImport(const size_t Owner, const NameBinding Import) const {
	// 自身への導入又は空の有効範囲なら登録しない事の返戻
	if(Import.Namespace == Owner || Import.Pos >= Import.End) return;
	NamespaceBindings &Scope = NameData->Bindings[Owner];
	if(Import.Namespace == std::string::npos) {
		Scope.UnknownImports.Add(Import);
		NameData->HasUnknownImports = true;
	} else Scope.Imports.push_back(Import);
	if(Scope.HasRoutes) Scope.PendingRoutes.push_back(Import);
	// 構築済の経路が此の所属へ達する範囲だけへ，追加した辺を伝播する
	for(const size_t User : Scope.RouteUsers) {
		ChargeNameWork();
		NamespaceBindings &Reader = NameData->Bindings[User];
		const std::map<uint32_t, uint32_t> &Windows = Reader.RouteWindows.at(Owner);
		std::map<uint32_t, uint32_t>::const_iterator Next = Windows.upper_bound(Import.Pos);
		if(Next != Windows.begin()) --Next;
		for(; Next != Windows.end() && Next->first < Import.End; ++Next) {
			ChargeNameWork();
			if(Next->second <= Import.Pos) continue;
			Reader.PendingRoutes.push_back(
				{ std::max(Import.Pos, Next->first), std::min(Import.End, Next->second), false, Import.Namespace }
			);
		}
	}
	// 終了
	return;
}

/**
 * 所属毎に共有する導入先と有効範囲の差分の反映関数
 * 計算量：追加経路区間数 R，影響する導入辺数 E，所属の深さ H に対し平均 O((E + R) log(E + R + 1) + RH)
 * @param Scope 探索を開始する所属
 * @throws std::bad_alloc 導入先と作業列のメモリを確保出来ない場合
 * @throws std::length_error 作業列の要素数上限を超える場合
 */
void ParsedTree::BuildNameRoutes(const size_t Scope) const {
	const NamespaceBindings &Owner = NameData->Bindings[Scope];
	// 確定した経路を読む場合の終了
	if(Owner.HasRoutes && Owner.PendingRoutes.empty()) return;
	if(!Owner.HasRoutes) {
		for(const NameBinding &Import : Owner.Imports) Owner.PendingRoutes.push_back(Import);
		for(const NameBinding &Import : Owner.UnknownImports.Bindings) Owner.PendingRoutes.push_back(Import);
		Owner.HasRoutes = true;
	}
	std::vector<NameBinding> &Pending = Owner.PendingRoutes;
	// 早い開始と長い終端を先に伝播し，既に覆った経路を再走査しない
	const auto IsLater = [](const NameBinding &Left, const NameBinding &Right) -> bool {
		// 遅い開始又は同じ開始で短い終端かの返戻
		return Left.Pos > Right.Pos || Left.Pos == Right.Pos && Left.End < Right.End;
	};
	std::make_heap(Pending.begin(), Pending.end(), IsLater);
	while(!Pending.empty()) {
		std::pop_heap(Pending.begin(), Pending.end(), IsLater);
		const NameBinding Route = Pending.back();
		Pending.pop_back();
		ChargeNameWork();
		if(Route.Namespace == Scope || Route.Pos >= Route.End) continue;
		const auto [Found, Added] = Owner.RouteWindows.try_emplace(Route.Namespace);
		std::map<uint32_t, uint32_t> &Windows = Found->second;
		std::map<uint32_t, uint32_t>::iterator NextWindow = Windows.upper_bound(Route.Pos);
		const std::map<uint32_t, uint32_t>::iterator Before = NextWindow == Windows.begin() ? Windows.end() : std::prev(NextWindow);
		if(Before != Windows.end() && Route.End <= Before->second) continue;
		// 区間の合併は被覆判定だけへ使い，位置順の探索インデックスは差分だけを追加する
		uint32_t Start = Route.Pos, End = Route.End;
		if(Before != Windows.end() && Start <= Before->second) {
			Start = Before->first;
			NextWindow = Windows.erase(Before);
		}
		while(NextWindow != Windows.end() && NextWindow->first <= End) {
			End = std::max(End, NextWindow->second);
			NextWindow = Windows.erase(NextWindow);
			ChargeNameWork();
		}
		Windows.emplace(Start, End);
		NameBinding Export = Route;
		Export.Target = Route.Namespace == std::string::npos || Owner.IsClass ? Scope : GetCommonNameScope(Scope, Route.Namespace);
		NameIndex &Index = Route.Namespace == std::string::npos ?
		Owner.UnknownRoutes :
		Owner.Routes.try_emplace(Route.Namespace, &NameData->Texts.Storage).first->second;
		Index.Add(Export);
		if(Route.Namespace == std::string::npos) continue;
		if(Added) NameData->Bindings[Route.Namespace].RouteUsers.push_back(Scope);
		for(const NameBinding &Next : NameData->Bindings[Route.Namespace].Imports) {
			ChargeNameWork();
			Pending.push_back({ std::max(Route.Pos, Next.Pos), std::min(Route.End, Next.End), false, Next.Namespace });
			std::push_heap(Pending.begin(), Pending.end(), IsLater);
		}
		for(const NameBinding &Next : NameData->Bindings[Route.Namespace].UnknownImports.Bindings) {
			ChargeNameWork();
			Pending.push_back({ std::max(Route.Pos, Next.Pos), std::min(Route.End, Next.End), false, std::string::npos });
			std::push_heap(Pending.begin(), Pending.end(), IsLater);
		}
	}
	// 終了
	return;
}

/**
 * 所属内の直接の宣言と導入した名前の解決関数
 * 名前長 N，導入先数 R，同名の所属数 S，同名の宣言数 D に対し，経路構築後は平均 O(N + min(R, S)(N + log(D + R + 1)))
 * @param Scope 探索する所属
 * @param Name 名前
 * @param Use 使用位置
 * @param ShouldUseMembersOnly 限定名としてメンバだけを調べるか
 * @param Tier 任意の非限定名の探索階層の格納先
 * @param ValidUntil 結果が変わり得る次の位置の最小値（不要なら無）
 * @return 有効な束縛，曖昧なら未確定の束縛，無ければ無
 * @throws std::bad_alloc 導入経路のメモリを確保出来ない場合
 * @throws std::length_error 導入経路の要素数上限を超える場合
 */
const ParsedTree::NameBinding *ParsedTree::GetScopeBinding(
	const size_t Scope,
	const std::string_view Name,
	const uint32_t Use,
	const bool ShouldUseMembersOnly,
	size_t *const Tier,
	uint32_t *const ValidUntil
) const {
	static constexpr NameBinding Unknown { 0, 0, false, 0, false };
	const NamespaceBindings &Owner = NameData->Bindings[Scope];
	ChargeNameWork();
	if(Tier) *Tier = Scope;
	size_t ResultTier = Scope;
	const decltype(Owner.Names) &Names = ShouldUseMembersOnly ? Owner.Members : Owner.Names;
	const decltype(Owner.Names)::const_iterator Direct = Names.find(Name);
	const NameBinding *Result = Direct == Names.end() ? nullptr : Direct->second.Find(Use, GetNameWorkCounter(), ValidUntil);
	// 完全なクラスのメンバは，本体内の定義から宣言の前後に依らず見える
	if(!Result && Owner.IsClass && !ShouldUseMembersOnly) {
		const decltype(Owner.Members)::const_iterator Member = Owner.Members.find(Name);
		if(Member != Owner.Members.end()) Result = Member->second.Find(Use, GetNameWorkCounter(), ValidUntil);
	}
	// 未確定の列挙子の導入より後で宣言した名前以外は，型又は値と推測しない
	const NameBinding *UnknownEnum = Owner.UnknownEnumerators.Find(Use, GetNameWorkCounter(), ValidUntil);
	// 導入した列挙子の名前を確定出来ない場合の返戻
	if(UnknownEnum && (!Result || Result->Pos <= UnknownEnum->Pos)) return &Unknown;
	// ローカルの宣言とクラス自身のメンバは導入先より先に名前を隠す
	// 導入先を隠す直接の束縛の返戻
	if(Result && (!Result->IsMember || Owner.IsClass || ShouldUseMembersOnly && !Owner.HasInline)) return Result;
	// 所属又は導入先が未確定なら，外側の名前を採って推測しない事の返戻
	if(Owner.IsUnresolved || Owner.UnknownImports.Find(Use, GetNameWorkCounter(), ValidUntil)) return &Unknown;
	if(NameData->HasUnknownImports && !Owner.Imports.empty()) {
		BuildNameRoutes(Scope);
		// 未確定の導入先へ達する経路が有効な場合の返戻
		if(Owner.UnknownRoutes.Find(Use, GetNameWorkCounter(), ValidUntil)) return &Unknown;
	}
	const decltype(NameData->MemberOwners)::const_iterator Sources = NameData->MemberOwners.find(Name);
	// 自身だけが持つ名前には，導入先の探索を行わない
	if(
		Sources == NameData->MemberOwners.end() || Owner.Imports.empty() ||
		Sources->second.size() == 1 && Sources->second.front() == Scope
	// 導入先に候補が無い場合の返戻
	) return Result;
	BuildNameRoutes(Scope);
	// 導入先と同名の所属の小さい方だけを調べ，名前の無い所属全体を繰り返し走査しない
	const auto Consider = [&](const size_t Source, const NameIndex &Window) -> void {
		ChargeNameWork();
		const NameBinding *Route = Window.Find(Use, GetNameWorkCounter(), ValidUntil);
		// 導入の有効範囲外の返戻
		if(!Route) return;
		const size_t NewTier = ShouldUseMembersOnly ? Scope : Route->Target;
		// 直接の宣言又は近い導入が見付かって居る場合の返戻
		if(Result && NameData->Bindings[NewTier].Depth < NameData->Bindings[ResultTier].Depth) return;
		const decltype(Owner.Members)::const_iterator Found = NameData->Bindings[Source].Members.find(Name);
		// 対象名を持たない所属の返戻
		if(Found == NameData->Bindings[Source].Members.end()) return;
		if(const NameBinding *Binding = Found->second.Find(Use, GetNameWorkCounter(), ValidUntil); Binding) {
			if(!Result || NameData->Bindings[NewTier].Depth > NameData->Bindings[ResultTier].Depth) {
				Result = Binding;
				ResultTier = NewTier;
			} else if(
				Result->Origin != Binding->Origin && !(Result->Target != std::string::npos && Result->Target == Binding->Target) ||
				!Result->IsKnown || !Binding->IsKnown
			) Result = &Unknown;
		}
		// 終了
		return;
	};
	if(Sources->second.size() < Owner.Routes.size()) for(const size_t Source : Sources->second) {
		const decltype(Owner.Routes)::const_iterator Route = Owner.Routes.find(Source);
		if(Route != Owner.Routes.end()) Consider(Source, Route->second);
	} else for(const auto &[Source, Window] : Owner.Routes) Consider(Source, Window);
	if(Tier) *Tier = ResultTier;
	// 同じ元の宣言の複数の導入は１つとして扱う束縛の返戻
	return Result;
}

/**
 * 使用位置で共有出来る名前の解決結果の取得関数
 * 計算量：同じ所属・名前の使用区間数 E に対し O(log(E + 1))
 * @param Table 使用位置順の解決結果
 * @param Use 使用位置
 * @return 此の位置でも有効な結果，無ければ無
 */
const ParsedTree::NameResolution *ParsedTree::NameCache::Find(const Results &Table, const uint32_t Use) {
	const NameCache::Results::const_iterator After = Table.upper_bound(Use);
	// 此の位置より前の結果が無い場合の返戻
	if(After == Table.begin()) return nullptr;
	const NameResolution &Result = std::prev(After)->second;
	// 束縛が変わり得る位置より前の結果だけを共有する返戻
	return Use < Result.End ? &Result : nullptr;
}

/**
 * 名前の無い外側の所属を共有結果で飛ばす関数
 * 計算量：未解決の所属数 H，所属内の照合費用 Q，名前長 B，使用区間数 E に対し平均 O(H(B + log(E + 1)) + Q)，共有済なら平均 O(B + log(E + 1))
 * @param Scope 探索を始める所属
 * @param Name 使用する名前
 * @param Use 使用位置
 * @param Stop 其れ以上外側へ持ち越せない導入の階層（無ければ根）
 * @return 最初に束縛を持つ外側の所属と導入の階層及び有効区間
 * @throws FormatLimitExceeded 累積作業量が計数範囲を超える場合
 * @throws std::bad_alloc 名前・解決結果又は作業列のメモリを確保出来ない場合
 * @throws std::length_error 名前・解決結果又は作業列の要素数上限を超える場合
 */
ParsedTree::NameResolution ParsedTree::GetInheritedBinding(
	const size_t Scope,
	const std::string_view Name,
	const uint32_t Use,
	const size_t Stop
) const {
	const bool CanCache = !Syntax.IsCollectingNames && !IsReadingNamesInParallel;
	NameResolution Result;
	std::vector<std::pair<size_t, uint32_t>> Pending;
	for(size_t Owner = Scope;;) {
		ChargeNameWork();
		const NamespaceBindings &Bindings = NameData->Bindings[Owner];
		// 並列の読取では既存の結果だけを読み，収集中の未確定の表は共有しない
		if(!Syntax.IsCollectingNames) {
			const decltype(Bindings.NameResults) &Results = Bindings.NameResults;
			if(const decltype(NamespaceBindings::NameResults)::const_iterator Found = Results.find(Name); Found != Results.end()) {
				if(const NameResolution *Cached = NameCache::Find(Found->second.Inherited, Use); Cached) {
					Result = *Cached;
					// 既存の解決先も，今回の導入の階層より外側なら参照しない
					if(NameData->Bindings[Result.Scope].Depth < NameData->Bindings[Stop].Depth) {
						Result.Binding = nullptr;
						Result.Scope = Result.Tier = Stop;
						break;
					}
					if(Result.Binding || Result.Scope == Stop) break;
					// 名前の無い共有区間の先だけを，今回必要な階層迄延ばす
					if(CanCache) Pending.emplace_back(Owner, Result.End);
					Owner = NameData->Bindings[Result.Scope].Parent;
					continue;
				}
			}
		}
		Result.Scope = Result.Tier = Owner;
		Result.End = UINT32_MAX;
		Result.Binding = GetScopeBinding(Owner, Name, Use, false, &Result.Tier, CanCache ? &Result.End : nullptr);
		if(CanCache) Pending.emplace_back(Owner, Result.End);
		if(Result.Binding || Owner == Stop) break;
		Owner = NameData->Bindings[Owner].Parent;
	}
	// 未読だった所属だけに同じ解決先を登録し，子から次の照会で祖先を辿り直さない
	if(!Pending.empty()) {
		const std::string_view Key = StoreName(Name);
		for(size_t Idx = Pending.size(); Idx--;) {
			ChargeNameWork();
			Result.End = std::min(Result.End, Pending[Idx].second);
			NameData->Bindings[Pending[Idx].first].NameResults[Key].Inherited[Use] = Result;
		}
	}
	// 使用位置で最初に名前を持つ所属の返戻
	return Result;
}

/**
 * 使用位置で最寄の宣言の束縛の取得関数
 * 新しい所属・名前・有効区間の初回は宣言収集費用 C と所属の照合費用 Q を加え，共有済なら名前長 B，所属区間数 K，使用区間数 E に対し平均 O(B + log(K + 1) + log(E + 1))
 * @param Name 名前
 * @param Use 使用位置
 * @return 使用位置で有効な最寄の束縛，曖昧なら未確定，無ければ無
 * @throws FormatLimitExceeded 累積作業量が計数範囲を超える場合
 * @throws std::bad_alloc 導入経路のメモリを確保出来ない場合
 * @throws std::length_error 導入経路の要素数上限を超える場合
 */
const ParsedTree::NameBinding *ParsedTree::GetNameBinding(const std::string_view Name, const uint32_t Use) const {
	// 使う名前が在る時だけ，型と値の遮蔽を共通に解決する表を作る
	if(!Syntax.HasNameBindings && !Syntax.IsCollectingNames) CollectDeclaredNames();
	// 木が無い場合の返戻
	if(!NameData || NameData->Bindings.empty()) return nullptr;
	const NameBinding *Region = NameData->Regions.Find(Use, GetNameWorkCounter());
	const size_t StartScope = Region ? Region->Namespace : 0;
	ChargeNameWork();
	if(!Syntax.IsCollectingNames) {
		const decltype(NameData->Bindings[StartScope].NameResults) &Results = NameData->Bindings[StartScope].NameResults;
		if(const decltype(NamespaceBindings::NameResults)::const_iterator Found = Results.find(Name); Found != Results.end()) {
			// 同じ所属・名前・有効区間で照合を終えた束縛の返戻
			if(const NameResolution *Cached = NameCache::Find(Found->second.Resolved, Use); Cached) return Cached->Binding;
		}
	}
	NameResolution Result;
	const NameBinding *Pending = nullptr;
	size_t PendingTier = 0;
	static constexpr NameBinding Unknown { 0, 0, false, 0, false };
	// ディレクティブの導入名は最も近い共通の外側で直接の宣言と合わせて解決する
	for(size_t Scope = StartScope;;) {
		const NameResolution Inherited = GetInheritedBinding(Scope, Name, Use, Pending ? PendingTier : 0);
		Result.End = std::min(Result.End, Inherited.End);
		Scope = Inherited.Scope;
		const size_t Tier = Inherited.Tier;
		const NameBinding *Binding = Inherited.Binding;
		// 名前の無い所属を飛ばした時も，導入の階層へ達した候補を外側へ持ち越さない
		if(!Binding) {
			Result.Binding = Pending;
			break;
		}
		// ローカルの束縛又はクラス内の最寄の束縛
		if(!Binding->IsMember || NameData->Bindings[Scope].IsClass) {
			Result.Binding = Binding;
			break;
		}
		if(!Pending || NameData->Bindings[Tier].Depth > NameData->Bindings[PendingTier].Depth) {
			Pending = Binding;
			PendingTier = Tier;
		} else if(
			Tier == PendingTier && Pending->Origin != Binding->Origin &&
			!(Pending->Target != std::string::npos && Pending->Target == Binding->Target)
		) Pending = &Unknown;
		// 導入の探索階層へ到達した束縛
		if(Pending && PendingTier == Scope) {
			Result.Binding = Pending;
			break;
		}
		if(!Scope) break;
		Scope = NameData->Bindings[Scope].Parent;
	}
	if(!Syntax.IsCollectingNames && !IsReadingNamesInParallel) {
		ChargeNameWork();
		NameData->Bindings[StartScope].NameResults[StoreName(Name)].Resolved.emplace(Use, Result);
	}
	// 有効区間で確定した最寄の束縛（無ければ無）の返戻
	return Result.Binding;
}

/**
 * 修復先の文法の記号を１度だけ取得する関数
 * 計算量：初回は文法の記号数 G に対し O(G)，以後は O(1)
 * @return 同じ木の文法に属する型と宣言の記号
 */
const ParsedTree::TemplateSymbols &ParsedTree::GetTemplateSymbols() const {
	if(!TemplateKinds) {
		const GrammarProfile &Profile = GetProfile();
		TemplateKinds.emplace(
			TemplateSymbols{
				Profile.FindSymbolWithRole(Core::Role::TypeArgList),
				Profile.FindSymbolWithRole(Core::Role::BinaryOperation),
				Profile.FindSymbolWithRole(Core::Role::LeadingConstDecl),
				Profile.FindSymbolWithRole(Core::Role::Identifier),
				Profile.FindSymbolWithRole(Core::Role::NamespaceName),
				Profile.FindSymbolWithRole(Core::Role::QualifiedIdentifier),
				Profile.FindSymbolWithRole(Core::Role::TemplateInstantiationType),
				Profile.FindSymbolWithRole(Core::Role::TypeName)
			}
		);
	}
	// 共有する文法の記号の返戻
	return *TemplateKinds;
}

/**
 * 束縛で確定した型引数付宣言の構造の取得関数
 * 文全体が宣言済の型と型引数と単一の導入名から成る場合だけ，比較・シフト・エラーの読違を型の構造へ戻す
 * 入力と完成候補に同じ判定を適用し，閉じのトークンの分割と空白で構造を変えない
 * 計算量：候補のノード数 N，トークン数 T，名前のバイト数 B，束縛照会の総費用 Q に対し O(N + T + B + Q)
 * @param Node 宣言又は式文の候補
 * @param DeclaredName 確定した導入名の格納先
 * @return 型・型引数・宣言子の先行順の投影計画（未確定なら空）
 * @throws std::bad_alloc 束縛又は投影計画のメモリを確保出来ない場合
 * @throws std::length_error 投影計画の要素数上限を超える場合
 */
std::vector<ParsedTree::TemplatePart> ParsedTree::ReadTemplateDecl(const TSNode Node, TSNode &DeclaredName) const {
	using Core::ChildRole;
	using Core::Role;
	const GrammarProfile &Profile = GetProfile();
	// 宣言又は式文でない候補の返戻
	if(!Is(Node, Role::ExprStmt) && !Is(Node, Role::LeadingConstDecl)) return {};
	// 型の定義の本体を候補として再走査せず，宣言の型の範囲だけで絞る
	const TSNode Type = Is(Node, Role::LeadingConstDecl) ? GetFieldChild(Node, ChildRole::Type) : Node;
	// 型本体と型引数の無い候補の返戻
	if(Is(Type, Role::ClassLike) || GetView(Type).find('<') == std::string_view::npos) return {};
	const TemplateSymbols &Symbols = GetTemplateSymbols();
	// 宣言の読違を修復しない文法の返戻
	if(!Profile.GetProjectionChecks(Symbols.Binary).CanReadTemplateDecl) return {};
	struct Token {
		uint32_t Start;
		uint32_t End;
		TSSymbol Symbol;
		bool IsName;
		bool IsPrimitive;
		TSNode Original;
	}; // 元のトークンの範囲と名前・基本型の区分
	std::vector<Token> Tokens;
	bool IsMissing = false;
	size_t Bytes = 0;
	WalkChildrenCursor(
		Node,
		[&](const TSNode Part) -> bool {
			ChargeNameWork();
			if(Is(Part, Role::Comment)) {
				for(const unsigned char Char : GetView(Part)) Bytes += !std::isspace(Char);
				// コメントをトークンに含めず，子へ降りない事の返戻
				return false;
			}
			if(ts_node_is_missing(Part)) {
				IsMissing = true;
				// 欠落したトークンから子へ降りない事の返戻
				return false;
			}
			// 葉以外は子のトークンを読む事の返戻
			if(ts_node_child_count(Part)) return true;
			const uint32_t Start = ts_node_start_byte(Part), End = ts_node_end_byte(Part);
			const std::string_view Text = GetView(Part);
			// 結合した閉じは，後で文全体の対応が確定した場合だけ採用する
			if(!Text.empty() && Text.find_first_not_of('>') == std::string_view::npos) for(uint32_t Pos = Start; Pos < End; ++Pos) {
				Tokens.push_back({ Pos, Pos + 1, Profile.GetTokenSymbol(">"), false, false, Part });
			} else {
				Tokens.push_back(
					{
						Start,
						End,
						ts_node_symbol(Part),
						Is(Part, Role::Identifier) || Is(Part, Role::NamespaceName) || Is(Part, Role::TypeName),
						Is(Part, Role::PrimitiveType),
						Part
					}
				);
			}
			for(const unsigned char Char : Text) Bytes += !std::isspace(Char);
			// 葉の後の子は無い事の返戻
			return false;
		}
	);
	// 欠落又は宣言に足りないトークン列の返戻
	if(IsMissing || Tokens.size() < 5) return {};
	const auto TextOf = [&](const size_t Idx) -> std::string_view {
		// 対象トークンの原文の返戻
		return std::string_view(*this).substr(Tokens[Idx].Start, Tokens[Idx].End - Tokens[Idx].Start);
	};
	// 導入名と終端が無い物，又は宣言子がマクロに置換される候補の返戻
	if(
		TextOf(Tokens.size() - 1) != ";" || !Tokens[Tokens.size() - 2].IsName ||
		HasMacroBinding(TextOf(Tokens.size() - 2), Tokens[Tokens.size() - 2].Start)
	) return {};
	// パーサーが読み飛ばしたコードも含め，文全体のトークンを覆う事を確認する
	size_t CodeBytes = 0;
	for(const unsigned char Char : GetView(Node)) CodeBytes += !std::isspace(Char);
	// 元のコードを完全には覆わない候補の返戻
	if(Bytes != CodeBytes) return {};
	std::vector<size_t> Closes(Tokens.size(), std::string::npos), Opens;
	for(size_t Idx = 0; Idx + 2 < Tokens.size(); ++Idx) {
		ChargeNameWork();
		if(TextOf(Idx) == "<") Opens.push_back(Idx);
		else if(TextOf(Idx) == ">") {
			// 対応する開きの無い候補の返戻
			if(Opens.empty()) return {};
			Closes[Opens.back()] = Idx;
			Opens.pop_back();
		}
	}
	// 閉じの足りない候補の返戻
	if(!Opens.empty()) return {};
	const TSSymbol Declaration = Symbols.Declaration, Qualified = Symbols.Qualified, Template = Symbols.Template;
	const TSSymbol Arguments = Symbols.Arguments, TypeName = Symbols.TypeName, NamespaceName = Symbols.NamespaceName;
	// 型の構造を投影する記号の無い文法の返戻
	if(!Declaration || !Symbols.Identifier || !Qualified || !Template || !Arguments || !TypeName || !NamespaceName) return {};
	const auto Field = [&](const ChildRole Role) -> uint64_t {
		// 投影可能なフィールドのビットの返戻
		return Role == ChildRole::None ? 0 : uint64_t{ 1 } << static_cast<uint8_t>(Role) & Profile.GetChildRoleMask();
	};
	const uint64_t ScopeField = Profile.GetFieldBits(ChildRole::ScopeField);
	std::vector<TemplatePart> Parts;
	const auto Add = [&](
		const uint32_t Parent,
		const size_t Begin,
		const size_t End,
		const TSSymbol Symbol,
		const uint64_t Fields,
		const bool IsLeaf
	) -> uint32_t {
		const uint32_t Id = static_cast<uint32_t>(Parts.size());
		Parts.push_back({ Symbol, Tokens[Begin].Start, Tokens[End - 1].End, Parent, Fields, IsLeaf });
		// 追加した構造の番号の返戻
		return Id;
	};
	Add(UINT32_MAX, 0, Tokens.size(), Declaration, 0, false);
	struct Task {
		size_t Begin;
		size_t End;
		uint32_t Parent;
		uint64_t Fields;
		TSSymbol Symbol; // ０は型の構文，其の他は葉
	}; // 右から積み，原文順に作る部分型又はトークン
	std::vector<Task> Pending {
		{ Tokens.size() - 1, Tokens.size(), 0, 0, Tokens.back().Symbol },
		{ Tokens.size() - 2, Tokens.size() - 1, 0, Field(ChildRole::Declarator), Symbols.Identifier },
		{ 0, Tokens.size() - 2, 0, Field(ChildRole::Type), 0 }
	};
	std::vector<Task> Children;
	while(!Pending.empty()) {
		ChargeNameWork();
		const Task Cur = Pending.back();
		Pending.pop_back();
		if(Cur.Symbol) {
			Add(Cur.Parent, Cur.Begin, Cur.End, Cur.Symbol, Cur.Fields, true);
			continue;
		}
		size_t NameEnd = Cur.Begin;
		std::string Name;
		if(TextOf(NameEnd) == "::") Name = std::string(TextOf(NameEnd++));
		while(NameEnd < Cur.End && (Tokens[NameEnd].IsName || Tokens[NameEnd].IsPrimitive)) {
			// マクロに置換される名前の候補の返戻
			if(HasMacroBinding(TextOf(NameEnd), Tokens[NameEnd].Start)) return {};
			Name += TextOf(NameEnd++);
			if(NameEnd == Cur.End || TextOf(NameEnd) != "::") break;
			Name += TextOf(NameEnd++);
		}
		// 名前又は基本型で終わらない名前の並びの返戻
		if(NameEnd == Cur.Begin || NameEnd > Cur.End || !Tokens[NameEnd - 1].IsName && !Tokens[NameEnd - 1].IsPrimitive) return {};
		const bool HasArguments = NameEnd < Cur.End && TextOf(NameEnd) == "<" && Closes[NameEnd] == Cur.End - 1;
		// 宣言の型引数の外に別の演算が残る候補の返戻
		if((NameEnd != Cur.End || !Cur.Parent) && !HasArguments) return {};
		const uint32_t Use = Tokens[Cur.Begin].Start;
		if(HasArguments ? !IsDeclaredTemplateName(Name, Use) : !Tokens[Cur.Begin].IsPrimitive && !IsDeclaredTypeName(Name, Use)) {
			// 使用位置で型と確定しない名前の候補の返戻
			return {};
		}
		// 修飾名は右の名前へ繋ぎ，型引数の型と値のフィールドを混同しない
		uint32_t Parent = Cur.Parent;
		uint64_t Fields = Cur.Fields;
		size_t Begin = Cur.Begin;
		if(TextOf(Begin) == "::") {
			Parent = Add(Parent, Begin, Cur.End, Qualified, Fields, false);
			Add(Parent, Begin, Begin + 1, Tokens[Begin].Symbol, 0, true);
			Fields = Field(ChildRole::Name);
			++Begin;
		}
		while(Begin + 1 < NameEnd) {
			Parent = Add(Parent, Begin, Cur.End, Qualified, Fields, false);
			Add(Parent, Begin, Begin + 1, NamespaceName, ScopeField, true);
			Add(Parent, Begin + 1, Begin + 2, Tokens[Begin + 1].Symbol, 0, true);
			Fields = Field(ChildRole::Name);
			Begin += 2;
		}
		if(!HasArguments) {
			Add(Parent, Begin, Cur.End, Tokens[Begin].IsPrimitive ? Tokens[Begin].Symbol : TypeName, Fields, true);
			continue;
		}
		Parent = Add(Parent, Begin, Cur.End, Template, Fields, false);
		Add(Parent, Begin, NameEnd, TypeName, Field(ChildRole::Name), true);
		Parent = Add(Parent, NameEnd, Cur.End, Arguments, Field(ChildRole::Args), false);
		Add(Parent, NameEnd, NameEnd + 1, Tokens[NameEnd].Symbol, 0, true);
		Children.clear();
		for(size_t Arg = NameEnd + 1, Pos = Arg; Pos < Cur.End; ++Pos) {
			if(TextOf(Pos) == "<") {
				Pos = Closes[Pos];
				continue;
			}
			if(TextOf(Pos) == "," || Pos == Cur.End - 1) {
				// 空の型引数を持つ候補の返戻
				if(Pos == Arg) return {};
				Children.push_back({ Arg, Pos, Parent, 0, 0 });
				Children.push_back({ Pos, Pos + 1, Parent, 0, Tokens[Pos].Symbol });
				Arg = Pos + 1;
			}
		}
		Pending.insert(Pending.end(), Children.rbegin(), Children.rend());
	}
	DeclaredName = Tokens[Tokens.size() - 2].Original;
	// 同じ宣言へ確定した構造だけの返戻
	return Parts;
}

/**
 * 使用位置の束縛から確定した宣言の共有関数
 * 計算量：初回は宣言収集費用 C，以後は平均 O(1)
 * @param Node 宣言又は式文の候補
 * @return 同じ世代で１度だけ確定した投影計画（対象外又は未確定なら空）
 * @throws std::bad_alloc 束縛又は投影計画のメモリを確保出来ない場合
 * @throws std::length_error 投影計画の要素数上限を超える場合
 */
const std::vector<ParsedTree::TemplatePart> &ParsedTree::GetTemplateDecl(const TSNode Node) const {
	static const std::vector<TemplatePart> Empty;
	const GrammarProfile &Profile = GetProfile();
	// 対象外の構文又は文法へ修復を広げない事の返戻
	if(
		!Is(Node, Core::Role::ExprStmt) && !Is(Node, Core::Role::LeadingConstDecl) ||
		!Profile.GetProjectionChecks(GetTemplateSymbols().Binary).CanReadTemplateDecl
	) return Empty;
	if(!Syntax.HasNameBindings) {
		const TSNode Type = Is(Node, Core::Role::LeadingConstDecl) ? GetFieldChild(Node, Core::ChildRole::Type) : Node;
		// 型本体と型引数の無い候補は名前の表を要求しない事の返戻
		if(Is(Type, Core::Role::ClassLike) || GetView(Type).find('<') == std::string_view::npos) return Empty;
		CollectDeclaredNames();
	}
	const decltype(TemplateDecls)::const_iterator Found = TemplateDecls.find(Node.id);
	// 確定した候補だけの返戻
	return Found == TemplateDecls.end() ? Empty : Found->second;
}

/**
 * 修飾先を含む使用位置の束縛の取得関数
 * 計算量：修飾の要素数 P と束縛の解決費用 Q に対し O(PQ)
 * @param Name 修飾付又は修飾の無い名前
 * @param Use 使用位置
 * @return 可視の束縛，未確定の修飾先なら無
 * @throws std::bad_alloc 束縛のメモリを確保出来ない場合
 * @throws std::length_error 束縛の要素数上限を超える場合
 */
const ParsedTree::NameBinding *ParsedTree::GetQualifiedNameBinding(const std::string_view Name, const uint32_t Use) const {
	const size_t Separator = Name.rfind("::");
	// 修飾の無い名前は既存の有効範囲の探索を共有する返戻
	if(Separator == std::string_view::npos) return GetNameBinding(Name, Use);
	if(!Syntax.HasNameBindings && !Syntax.IsCollectingNames) CollectDeclaredNames();
	const size_t Scope = Separator ? ResolveNameScope(Name.substr(0, Separator), Use) : 0;
	// 解決した修飾先の宣言の返戻（最も外の修飾はローカルの遮蔽を受けない）
	return Scope == std::string::npos ? nullptr : GetScopeBinding(Scope, Name.substr(Separator + 2), Use, true);
}

/**
 * 限定名が指すクラス・列挙型又は名前空間の所属の解決関数
 * 限定名の要素数 P，各要素の束縛の解決費用 Q に対し O(PQ)
 * @param Name 限定名（依存する型引数は解決しない）
 * @param Use 使用位置
 * @return 確定した所属，解決出来なければ `std::string::npos`
 * @throws std::bad_alloc 導入経路のメモリを確保出来ない場合
 * @throws std::length_error 導入経路の要素数上限を超える場合
 */
size_t ParsedTree::ResolveNameScope(std::string_view Name, const uint32_t Use) const {
	// 名前と区切の前後の空白だけを除き，型引数の中を限定名として読まない
	const auto Trim = [](const std::string_view Text) -> std::string_view {
		const size_t Pos = Text.find_first_not_of(" \t\r\n");
		// 空の名前又は前後の空白を除いた名前の返戻
		return Pos == std::string_view::npos ? std::string_view{} : Text.substr(Pos, Text.find_last_not_of(" \t\r\n") - Pos + 1);
	};
	Name = Trim(Name);
	// 依存する限定名の解決を見送る返戻
	if(Name.empty() || Name.find_first_of("<>()[]*&") != std::string_view::npos) return std::string::npos;
	bool IsQualified = Name.starts_with("::");
	size_t Scope = 0;
	if(IsQualified) Name.remove_prefix(2);
	while(!Name.empty()) {
		const size_t End = Name.find("::");
		const std::string_view Part = Trim(Name.substr(0, End));
		const NameBinding *Binding = IsQualified ? GetScopeBinding(Scope, Part, Use, true) : GetNameBinding(Part, Use);
		// 確定した所属を持たない名前の返戻
		if(!Binding || !Binding->IsKnown || Binding->Target == std::string::npos) return std::string::npos;
		Scope = Binding->Target;
		// 限定名の最後の所属の返戻
		if(End == std::string_view::npos) return Scope;
		IsQualified = true;
		Name.remove_prefix(End + 2);
	}
	// 未確定の所属の返戻
	return std::string::npos;
}

/**
 * 根からの降下を繰り返さない直接親の取得関数
 * 計算量：ノード数 N に対し木の世代毎に初回 O(N)，以後平均 O(1)，追加領域 O(N)
 * @param Node 対象のノード
 * @return 同じ木での直接親（根又は空のノードなら空）
 * @throws std::bad_alloc 直接親の表を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
TSNode ParsedTree::GetParentOf(const TSNode Node) const {
	// 空のノードの親の返戻
	if(ts_node_is_null(Node)) return {};
	RecordNodeRelations();
	const NodeRelation *const Parent = Syntax.Relations->Parents.Find(Node.id);
	// 記録済の直接親又は根の空の親の返戻
	return !Parent || Parent->Parent == UINT32_MAX ? TSNode{} : GetNodeAt(Parent->Parent).Node;
}

/**
 * 指定された場の役割を持つ最初の子ノード取得関数
 * 計算量：役割に対応するフィールド数 F と補完済のフィールド数 K に対し期待値 O(1 + F log(K + 1))，補完前は O(1 + F + Q)（パーサーへの照会の総費用 Q）
 * @param Node 親ノード
 * @param Role 子の場の役割
 * @param Work パーサーへの直接のフィールド照会の観測用計数（無ければ計上しない）
 * @return 対応する最初の子ノード（無ければ空ノード）
 */
TSNode ParsedTree::GetFieldChild(const TSNode Node, const Core::ChildRole Role, size_t *const Work) const {
	// 文法の定義の無いソースと空のノードのフィールドが無い事の返戻
	if(!Grammar || ts_node_is_null(Node)) return TSNode{};
	const std::span<const TSFieldId> Fields = Grammar->GetFieldsOf(Role);
	// 対応するフィールドが無い役割では親子表を照会しない
	if(Fields.empty()) return TSNode{};
	// 投影前に所有スレッドが完成した表だけを読み，未構築の場合も此の照会から表を変更しない
	const NodeRelation *const Parent = Syntax.HasParents ? Syntax.Relations->Parents.Find(Node.id) : nullptr;
	// 記録済の子の数から，葉のフィールドが無い事の返戻
	if(Parent ? !Parent->Count : !ts_node_child_count(Node)) return TSNode{};
	std::span<const FieldChild> Cached;
	const bool HasCached = Parent && Parent->HasFieldsReady;
	if(HasCached) {
		Cached = std::span<const FieldChild>(Syntax.Relations->FirstFields).subspan(Parent->FieldsBegin, Parent->FieldsCount);
	}
	TSNode Found {};
	for(const TSFieldId Field : Fields) {
		TSNode Child {};
		if(HasCached && Grammar->GetFieldBits(Field)) {
			const std::span<const FieldChild>::iterator First = std::lower_bound(
				Cached.begin(),
				Cached.end(),
				Field,
				[](const FieldChild &Entry, const TSFieldId Wanted) -> bool {
					// 対象のフィールドより前かの返戻
					return Entry.Field < Wanted;
				}
			);
			if(First != Cached.end() && First->Field == Field) {
				Child = First->IsStored ? GetNodeAt(First->Index).Node : Syntax.Relations->FieldNodes[First->Index];
			}
		} else {
			Child = ts_node_child_by_field_id(Node, Field);
			if(Work) ++*Work;
		}
		if(ts_node_is_null(Child)) continue;
		if(ts_node_is_null(Found) || ts_node_start_byte(Child) < ts_node_start_byte(Found)) Found = Child;
	}
	// 役割へ写る場の最も前の子の返戻
	return Found;
}

/**
 * 逐語データを除いた解析対象の本文の取得関数
 * 計算量：O(1)
 * @return パーサーへ渡すコードの範囲
 */
std::string_view ParsedTree::GetCodeText() const {
	// 実行停止後のデータを除いたテキストの返戻
	return std::string_view(data(), HaltedData == std::string::npos ? size() : HaltedData);
}

/**
 * 文法の構文・意味の固定の定義の取得関数
 * 計算量：O(1)
 * @return 解析に用いる文法の構文・意味の固定の定義（言語の指定の有るソースだけが持つ）
 */
const Core::SyntaxRules &ParsedTree::GetRules() const {
	// 固定の定義の返戻
	return Grammar->GetRules();
}

/**
 * コメントの本文の範囲の取得関数
 * 末尾の改行と，解析のエラーの回復で頭へ付いた空白を除き，幅０のノード（tree-sitter-kotlin がスキャナーで呑んだブロックコメント）は直前の本文をネストに対応して回収する
 * 計算量：コメントと直前の本文で辿るテキストの長さ L に対し O(L)
 * @param Node コメントのノード
 * @return 本文の始まりと終わり（回収出来ない幅０のノードは同じ位置）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::pair<uint32_t, uint32_t> ParsedTree::GetCommentRangeOf(const TSNode Node) const {
	// ソースのテキストへの参照
	const std::string &Self = *this;
	uint32_t Begin = ts_node_start_byte(Node), End = ts_node_end_byte(Node);
	// 末尾の改行を含むノード（Rust のドキュメントコメント等）の除外
	while(End > Begin && (Self[End - 1] == '\n' || Self[End - 1] == '\r')) --End;
	// 解析のエラーの回復でノードの頭へ付いた空白の除外
	while(Begin < End && (Self[Begin] == ' ' || Self[Begin] == '\t')) ++Begin;
	// 幅の有るノードの範囲の返戻
	if(Begin < End) return { Begin, End };
	// 幅０のノード (tree-sitter-kotlin) の直前のブロックコメントの終わり
	uint32_t Close = Begin;
	while(Close && (Self[Close - 1] == ' ' || Self[Close - 1] == '\t' || Self[Close - 1] == '\n' || Self[Close - 1] == '\r')) {
		--Close;
	}
	// 直前がブロックコメントで終わらない幅０のノードの返戻
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
	// 対を為す開きの無い幅０のノードと，回収した本文の範囲の返戻
	return Depth ? std::pair{ Begin, End } : std::pair{ Open, Close };
}

/**
 * コメントの種類の判定の共通関数
 * 行の前に空白とブロックコメントだけが在れば行頭，後に空白とコメントだけが続いて改行（PHP は閉タグ `?>` も）に至れば行末とする
 * 計算量：範囲の表の要素数 N に対し O(log N)（表に無いコメントだけ其の場で前後の空白を見る）
 * @param Spans 原文順のコメントの範囲の表
 * @param Start コメントの本文の始まり
 * @param End コメントの本文の終わり
 * @param IsAtLineStart 行頭に在るか（不要なら `nullptr`）
 * @param AfterComments 後の空白と解析済コメントを飛ばした位置（不要なら `nullptr`）
 * @return 原文でのコメントの種類
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
CommentKind ParsedTree::ClassifyComment(
	const std::vector<CommentSpan> &Spans,
	const uint32_t Start,
	const uint32_t End,
	bool *const IsAtLineStart,
	uint32_t *const AfterComments
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
	// 同じ行の前が行頭かブロックコメントだけかと，後の空白とコメントを飛ばした位置
	bool IsLineHead;
	uint32_t Scan;
	// 表に無いコメント（根其の物等）は前後を其の場で見る
	if(Span != Spans.end() && Span->Start == Start) {
		IsLineHead = Span->IsLineHead;
		Scan = Span->RunEnd;
	} else {
		// 表に無いコメントの行頭
		const uint32_t Lead = TextEdit::SkipSpaceLeft(Self, GetReading(), Start);
		IsLineHead = !Lead || Self[Lead - 1] == '\n';
		Scan = TextEdit::SkipSpaceRight(Self, GetReading(), End);
	}
	// 所属の判定へ渡す位置は解析用に補った改行も境界として残す
	if(AfterComments) *AfterComments = Scan;
	// 解析用に補った改行は原文に無い為，行末に数えず其の後を見る
	const size_t CodeEnd = GetCodeText().size();
	while(Scan < CodeEnd && Self[Scan] == '\n' && std::binary_search(InsertedBreaks.begin(), InsertedBreaks.end(), Scan)) {
		Scan = TextEdit::SkipSpaceRight(GetCodeText(), GetReading(), Scan + 1);
	}
	// 行コメントの本文を終えるテキスト（PHP の閉タグ `?>`）はコードの行を終える為，行末に数える
	const std::string_view Terminator = GetRules().Notation.LineCommentTerminator;
	const bool IsAtLineEnd = Scan >= CodeEnd || Self[Scan] == '\n' || Self[Scan] == '\r' ||
	!Terminator.empty() && !Self.compare(Scan, Terminator.size(), Terminator);
	// 行頭の状態を求めた呼出側への引渡（文中のコメントは種類から行頭かを引けない）
	if(IsAtLineStart) *IsAtLineStart = IsLineHead;
	// 種類の返戻
	return !IsAtLineEnd ? CommentKind::Inline : IsLineHead ? CommentKind::OwnLine : CommentKind::Trailing;
}

/**
 * 空白・タブへデコードされる数値文字参照の長さと其の文字の取得関数
 * 計算量：Pos 以降の長さ N に対し O(N)
 * @param Text 本文
 * @param Pos 判定する位置
 * @return Pos から始まる `&#32;` / `&#x20;` / `&#9;` 等の長さとデコードした文字（該当しなければ長さ 0）
 */
std::pair<size_t, unsigned char> ParsedTree::BlankRef(const std::string_view Text, const size_t Pos) {
	// 数値文字参照でない事の返戻
	if(Text.substr(Pos, 2) != "&#") return { 0, '\0' };
	// 数字部又は基数の接頭辞の位置
	size_t At = Pos + 2;
	// １６進文字参照か
	const bool IsHex = At < Text.size() && (Text[At] == 'x' || Text[At] == 'X');
	// 基数の接頭辞の読飛
	if(IsHex) ++At;
	// 数字部の開始位置
	const size_t Digits = At;
	// デコード途中の符号値
	char32_t Code = 0;
	for(; At < Text.size() && Code < 0X110000; ++At) {
		// 数値文字参照の桁の累積
		if(const unsigned char Char = Text[At]; Char >= '0' && Char <= '9') {
			// 数字の取込
			Code = (IsHex ? Code << 4 : 10 * Code) + static_cast<uint32_t>(Char - '0');
		} else if(IsHex && (Char | 0X20) >= 'a' && (Char | 0X20) <= 'f') {
			// １６進英字の取込
			Code = Code << 4 | static_cast<uint32_t>((Char | 0X20) + 10 - 'a');
		} else break;
	}
	// 空白・タブへデコードされる完結した文字参照でない事の返戻
	if(At == Digits || At >= Text.size() || Text[At] != ';' || Code != ' ' && Code != '\t') return { 0, '\0' };
	// 文字参照の長さとデコードした文字の返戻
	return { At - Pos + 1, static_cast<unsigned char>(Code) };
}

/**
 * 解析の為に原文へ置いたマーカーの取得関数（整形仕様 4.2）
 * 計算量：O(1)
 * @return マーカーの字（置いて居なければ '\0'）
 */
unsigned char ParsedTree::GetAnalysisMark() const {
	// マーカーの返戻
	return AnalysisMark;
}

/**
 * 本文差替と未反映状態の共通更新関数
 * 計算量：新旧の本文長 B と控えの要素数 N に対し O(B + N)
 * @tparam Str
 * @param NewSrc 新しいソース文字列（右辺値なら所有権を移す）
 */
template<typename Str> void ParsedTree::AssignSrc(Str &&NewSrc) {
	// 本文が同一の場合の返戻
	if(NewSrc == static_cast<const std::string &>(*this)) return;
	// 本文の差替
	const size_t DataLen = HaltedData == std::string::npos ? 0 : size() - HaltedData;
	std::string::operator=(std::forward<Str>(NewSrc));
	// 本文差替後の位置と解析状態の更新
	if(HaltedData != std::string::npos) HaltedData = size() - DataLen;
	if(Syntax.Tree) Syntax.IsDirty = true;
	// 本文と共に古く為るコメントの位置表の破棄
	Syntax.CommentSpans.clear();
	Syntax.HasCommentNodes = false;
	Syntax.HasCommentSpans = false;
	Syntax.BlankLines.clear();
	Syntax.Relations.reset();
	Syntax.HasParents = false;
	Syntax.HasNameBindings = false;
	Syntax.HasMacroStates = false;
	NameData.reset();
	ReboundDecls.clear();
	TemplateDecls.clear();
	TemplateKinds.reset();
	HasUnrepairedInputError.reset();
	// 終了
	return;
}

/**
 * 適用前の本文位置への逆マッピング関数（整形仕様 3.3 の診断位置）
 * 修復の編集の組を適用の逆順に辿り，置き換えたテキストの中の位置は置き換えた範囲の始まりへ戻す
 * 計算量：編集の組の数 B と組毎の編集の数 E に対し O(B log E)
 * @param Offset 現在の本文の位置
 * @param Batches 適用順の編集の組
 * @return 修復を加える前の本文の位置
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
uint32_t ParsedTree::OffsetBeforeBatches(uint32_t Offset, const AppliedEditBatches &Batches) {
	// 対象範囲の走査
	for(AppliedEditBatches::const_reverse_iterator Batch = Batches.rbegin(); Batch != Batches.rend(); ++Batch) {
		// 位置より前から始まる最後の編集
		const std::vector<AppliedEdit>::const_iterator After = std::upper_bound(
			Batch->begin(),
			Batch->end(),
			Offset,
			[](const uint32_t Pos, const AppliedEdit &Edit) -> bool {
				// 編集後の始まりとの比較の返戻
				return Pos < Edit.NewStart;
			}
		);
		if(After == Batch->begin()) continue;
		const AppliedEdit &Edit = *std::prev(After);
		Offset = Offset < Edit.NewEnd ? Edit.Start : Offset + Edit.End - Edit.NewEnd;
	}
	// 修復前の本文の位置の返戻
	return Offset;
}

/**
 * 此の世代で直接子の情報を読んだ回数の取得関数
 * 計算量：O(1)
 * @return 親・子・フィールド・記号を収集した直接子の数
 */
size_t ParsedTree::GetRelationWork() const noexcept {
	// 未収集の場合は０とする作業量の返戻
	return Syntax.Relations ? Syntax.Relations->ReadWork : 0;
}

/**
 * 此の世代の宣言と所属の表を収集済かの取得関数
 * 計算量：O(1)
 * @return 名前の照会に必要な表を生成済なら真
 */
bool ParsedTree::HasCollectedNames() const noexcept {
	// 現在の木の収集状態の返戻
	return Syntax.HasNameBindings;
}

/**
 * 世代で共有する直接子の範囲の取得関数
 * 計算量：初回は関係の収集費用 Q，以後平均 O(1)
 * @param Node 対象のノード
 * @return 原文順の子・フィールド・記号（木を編集する迄有効）
 * @throws std::bad_alloc 世代の表を確保出来ない場合
 * @throws std::length_error 表又は作業列の要素数上限を超える場合
 */
std::span<const ParsedChild> ParsedTree::GetChildrenOf(const TSNode Node) const {
	// 空ノードの子の返戻
	if(ts_node_is_null(Node)) return {};
	RecordNodeRelations();
	const NodeRelation *const Found = Syntax.Relations->Parents.Find(Node.id);
	// 所属する木に子が無い場合の返戻
	if(!Found || !Found->Count) return {};
	// １度収集した直接子の範囲の返戻
	return std::span<const ParsedChild>(Syntax.Relations->Children).subspan(Found->Begin, Found->Count);
}

/**
 * キャスト修復の候補の訪問回数の取得関数
 * 計算量：O(1)
 * @return キャスト候補の走査と配列の型の降下の回数
 */
size_t ParsedTree::GetCastRepairWork() const noexcept {
	// 修復の走査量の返戻
	return CastRepairWork.Used;
}

/**
 * 文境界修復の累積作業量の取得関数
 * 計算量：O(1)
 * @return 修復の判定で調べた葉の総数
 */
size_t ParsedTree::GetBoundaryRepairWork() const noexcept {
	// 計上した作業量の返戻
	return BoundaryRepairWork.Used;
}

/**
 * 前置型の修復の累積作業量の取得関数
 * 計算量：O(1)
 * @return トークンと型の修復候補の検査・被演算子の探索の作業量
 */
size_t ParsedTree::GetPrefixTypeRepairWork() const noexcept {
	// 独立した入力適応で消費した作業量の返戻
	return PrefixTypeRepairWork.Used;
}

/**
 * 並列区間が読む宣言と導入経路の事前構築関数
 * 計算量：宣言の収集費用 C，全所属の導入経路の構築費用 Q に対し O(C + Q)
 * @throws std::bad_alloc 宣言又は経路のメモリを確保出来ない場合
 * @throws std::length_error 名前又は経路の要素数が上限を超える場合
 */
void ParsedTree::PrepareNameReads() const {
	// 名前の表と，照会時にも遅延して作る導入経路の確定
	CollectMacroStates();
	CollectDeclaredNames();
	if(NameData) for(size_t Scope = 0; Scope < NameData->Bindings.size(); ++Scope) BuildNameRoutes(Scope);
	// 終了
	return;
}

/**
 * 名前の表を確定して共有木の並列照会へ入る生成関数
 * 計算量：名前と導入経路の構築費用 Q に対し O(Q)
 * @param Src 読む木（並列区間中は差替と再解析を行わない）
 * @throws std::bad_alloc 名前又は経路の領域を確保出来ない場合
 * @throws std::length_error 名前又は経路の要素数が上限を超える場合
 */
ParsedTree::NameReadGuard::NameReadGuard(const ParsedTree &Src) : HasOuterReader(Src.IsReadingNamesInParallel), Src(Src) {
	// 作業スレッドの起動前の収集と，共有計数の停止
	Src.PrepareNameReads();
	Src.IsReadingNamesInParallel = true;
	// 終了
	return;
}

/**
 * 全作業スレッドの合流又は例外の後の計数の復元関数
 * 計算量：O(1)
 */
ParsedTree::NameReadGuard::~NameReadGuard() {
	Src.IsReadingNamesInParallel = HasOuterReader;
	// 終了
	return;
}

/**
 * 束縛の探索・導入経路と宣言子の成立確認の累積作業量の取得関数
 * 計算量：O(1)
 * @return 区間の比較・降下，所属・導入辺の照合と宣言子の成立確認の累積作業量
 */
size_t ParsedTree::GetNameWork() const noexcept {
	// 読取専用の作業量の返戻
	return NameWork.Used;
}

/**
 * 使う位置で有効な宣言を同じファイルに持つ名前かの判定関数
 * 計算量：名前の束縛の解決費用 Q に対し O(Q)
 * @param Name 名前
 * @param Use 使う位置
 * @return 最寄の束縛が変数・仮引数・関数・列挙子なら true
 * @throws std::bad_alloc 導入経路のメモリを確保出来ない場合
 * @throws std::length_error 導入経路の要素数上限を超える場合
 */
bool ParsedTree::IsDeclaredName(const std::string_view Name, const uint32_t Use) const {
	const NameBinding *const Binding = GetNameBinding(Name, Use);
	// 最寄の束縛の区分の返戻
	return Binding && Binding->IsKnown && !Binding->IsType && !Binding->IsNamespace;
}

/**
 * 名前のハッシュ算出関数
 * 計算量：名前の長さ N に対し O(N)
 * @param Name 所有する名前又は本文から得た名前のビュー
 * @return 名前のテキストに対応するハッシュ値
 */
size_t ParsedTree::NameHash::operator()(const std::string_view Name) const noexcept {
	// 文字列とビューの照会を同じハッシュで扱う為の返戻
	return std::hash<std::string_view>()(Name);
}

/**
 * 世代内で所有した名前の共通ビューの取得関数
 * 計算量：名前の長さ B に対し平均 O(B)，新しい綴りだけを１度複写する
 * @param Name 所有する名前
 * @return 木と本文の移動で無効に為らない世代内のビュー
 * @throws std::bad_alloc 名前の共有メモリを確保出来ない場合
 * @throws std::length_error 名前又は集合の要素数上限を超える場合
 */
std::string_view ParsedTree::StoreName(const std::string_view Name) const {
	using ValuesType = std::pmr::unordered_set<std::pmr::string, NameHash, std::equal_to<>>;
	const ValuesType &Values = NameData->Texts.Values;
	// 既に所有する名前の返戻
	if(const ValuesType::const_iterator Found = Values.find(Name); Found != Values.end()) return *Found;
	// 新しい綴りの１度だけの所有と其のビューの返戻
	return *NameData->Texts.Values.emplace(Name).first;
}

/**
 * 使用位置で有効な入力のマクロの記録の取得関数
 * 計算量：名前の長さ B と同名の記録数 N に対し O(B + log(N + 1))（入力の定義表が空なら O(1)）
 * @param Name 対象名
 * @param Use 使用位置
 * @return 最後の記録（入力に定義・取消の記録が無ければ nullptr）
 */
const ParsedTree::MacroDef *ParsedTree::FindMacroDef(const std::string_view Name, const uint32_t Use) const {
	// 未収集又は空の入力の定義表には記録が無い事の返戻
	if(!NameData || NameData->MacroDefs.empty()) return nullptr;
	if(
		const decltype(NameData->MacroDefs)::const_iterator Found = NameData->MacroDefs.find(Name);
		Found != NameData->MacroDefs.end()
	) {
		const std::vector<MacroDef>::const_iterator Next = std::upper_bound(
			Found->second.begin(),
			Found->second.end(),
			Use,
			[](const uint32_t Pos, const MacroDef &Item) -> bool {
				// 使用位置より後の記録かの返戻
				return Pos < Item.Start;
			}
		);
		// 使用位置以前の最後の記録の返戻
		if(Next != Found->second.begin()) return &*std::prev(Next);
	}
	// 使用位置以前の記録が無い事の返戻
	return nullptr;
}

/**
 * 使用位置の前処理状態の取得関数
 * 計算量：名前の長さ B，名前の記録数 N，インクルードの世代の数 E に対し O(B + log(N + 1) + log(E + 1))
 * @param Name 対象名
 * @param Use 使用位置
 * @return 使用位置の定義・未確定・未定義・取消済の記録
 */
ParsedTree::MacroDef ParsedTree::GetMacroDef(const std::string_view Name, const uint32_t Use) const {
	MacroDef Def { 0, 0, MacroState::Undefined, {}, false };
	// 翻訳開始時の標準の事前定義（ヘッダに依る定義を含めない）
	const ReadingRules &Reading = GetReading();
	if(Reading.PredefinedMacroNames.contains(Name)) Def.State = MacroState::Defined;
	else if(Reading.ConditionalMacroNames.contains(Name)) Def.State = MacroState::Unknown;
	// 収集前の空の表と同じ，標準の事前定義だけの返戻
	if(!NameData) return Def;
	if(const MacroDef *const Input = FindMacroDef(Name, Use); Input) Def = *Input;
	const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Epoch = std::upper_bound(
		NameData->MacroEpochs.begin(),
		NameData->MacroEpochs.end(),
		Use,
		[](const uint32_t Pos, const std::pair<uint32_t, uint32_t> &Item) -> bool {
			// 走査の継続に用いる位置関係の返戻
			return Pos < Item.first;
		}
	);
	if(Epoch != NameData->MacroEpochs.begin() && Def.Epoch != std::prev(Epoch)->second) Def.State = MacroState::Unknown;
	// 判定結果の返戻
	return Def;
}

/**
 * 入力の定義に依るマクロ置換が介在し得るかの判定関数
 * 未読のインクルードに依る括弧保護の状態は呼出の解決へ渡さない
 * 計算量：名前の長さ B と同名の記録数 N に対し O(B + log(N + 1))（入力の定義表が空なら O(1)）
 * @param Name 対象名
 * @param Use 使用位置
 * @return 定義済又は未確定なら true
 */
bool ParsedTree::HasMacroBinding(const std::string_view Name, const uint32_t Use) const {
	const MacroDef *const Input = FindMacroDef(Name, Use);
	// 入力の束縛だけを読み，事前定義とインクルードの世代は求めない判定結果の返戻
	return Input && (Input->InputState == MacroState::Defined || Input->InputState == MacroState::Unknown);
}

/**
 * マクロ置換の有無が未確定かの判定関数
 * @param Name 対象名
 * @param Use 使用位置
 * @return インクルード又は条件枝に依って未確定なら true
 */
bool ParsedTree::IsMacroUncertain(const std::string_view Name, const uint32_t Use) const {
	std::unordered_set<std::string_view> Seen;
	std::string_view Alias = Name;
	while(Seen.insert(Alias).second) {
		const MacroDef Def = GetMacroDef(Alias, Use);
		// 参照先が未確定である事の返戻
		if(Def.State == MacroState::Unknown) return true;
		// 別名の無い確定した状態の返戻
		if(Def.State != MacroState::Defined || Def.Alias.empty()) return false;
		Alias = Def.Alias;
	}
	// 循環により解決が確定しない事の返戻
	return true;
}

/**
 * 使用位置で関数形式マクロ又は其の別名に束縛される名前かの判定関数
 * 計算量：別名の段数 H，同名の定義数 D，インクルード数 I と名前の総長 B に対し期待値 O(B + H log(D + I + 1))
 * @param Name 呼出先の名前
 * @param Use 使用位置
 * @return 有効な定義を辿って関数形式マクロへ達するなら true
 * @throws std::bad_alloc 別名の循環検出の表を確保出来ない場合
 * @throws std::length_error 表が要素数上限を超える場合
 */
bool ParsedTree::IsFunctionMacro(std::string_view Name, const uint32_t Use) const {
	std::unordered_set<std::string_view> Seen;
	std::string_view Alias = Name;
	while(Seen.insert(Alias).second) {
		const MacroDef Def = GetMacroDef(Alias, Use);
		// 有効な定義が無い事の返戻
		if(Def.State != MacroState::Defined) return false;
		// 関数形式の定義へ達した事の返戻
		if(Def.IsFunctionLike) return true;
		// 別名でないオブジェクト形式の定義の返戻
		if(Def.Alias.empty()) return false;
		Alias = Def.Alias;
	}
	// 循環する別名は関数形式の定義へ達しない事の返戻
	return false;
}

/**
 * 使う位置で有効な型宣言を同じファイルに持つ名前かの判定関数
 * 計算量：修飾の要素数 P と束縛の解決費用 Q に対し O(PQ)
 * @param Name 修飾付又は修飾の無い名前
 * @param Use 使う位置
 * @return 最寄の束縛が型なら true
 * @throws std::bad_alloc 導入経路のメモリを確保出来ない場合
 * @throws std::length_error 導入経路の要素数上限を超える場合
 */
bool ParsedTree::IsDeclaredTypeName(const std::string_view Name, const uint32_t Use) const {
	const NameBinding *const Binding = GetQualifiedNameBinding(Name, Use);
	// 最寄の束縛の区分の返戻
	return Binding && Binding->IsKnown && Binding->IsType;
}

/**
 * 使用位置で有効な型のテンプレート名かの判定関数
 * 計算量：修飾の要素数 P と束縛の解決費用 Q に対し O(PQ)
 * @param Name 修飾付又は修飾の無い名前
 * @param Use 使用位置
 * @return 最寄の束縛が確定した型のテンプレートなら true
 * @throws std::bad_alloc 束縛のメモリを確保出来ない場合
 * @throws std::length_error 束縛の要素数上限を超える場合
 */
bool ParsedTree::IsDeclaredTemplateName(const std::string_view Name, const uint32_t Use) const {
	const NameBinding *const Binding = GetQualifiedNameBinding(Name, Use);
	// 値と非テンプレートの型を区別した結果の返戻
	return Binding && Binding->IsKnown && Binding->IsType && Binding->IsTemplate;
}

/**
 * 使用位置で非マクロの事実を持つ名前かの判定関数
 * 通常の宣言・標準契約と，使用位置でのマクロ置換の有無を別に照会する
 * 計算量：名前の解決費用 Q，インクルード数 I に対し O(Q + log(I + 1))
 * @param Name 名前
 * @param Use 使用位置
 * @return プリプロセッサディレクティブの影響を受けない有効な宣言が有れば true
 * @throws std::bad_alloc 導入経路のメモリを確保出来ない場合
 * @throws std::length_error 導入経路の要素数上限を超える場合
 */
bool ParsedTree::IsNonMacroName(const std::string_view Name, const uint32_t Use) const {
	// 非マクロの照会は，宣言の遅延収集より先に前処理の状態を確定する
	CollectMacroStates();
	const MacroState State = GetMacroDef(Name, Use).State;
	// マクロ置換の可能性を除けない場合の返戻
	if(State != MacroState::Undefined && State != MacroState::Cancelled) return false;
	const NameBinding *Binding = GetNameBinding(Name, Use);
	// マクロ置換が無い事と通常の束縛の双方が確定した場合だけ採用
	return Binding && Binding->IsKnown || GetProfile().IsSpelling(Name, Core::Word::NonMacroName);
}

/**
 * 解析に用いる文法の定義の取得関数
 * 計算量：O(1)
 * @return 言語の指定と一致する文法の定義
 */
const GrammarProfile &ParsedTree::GetProfile() const {
	// 文法の定義の返戻
	return *Grammar;
}

/**
 * 準備だけを戻した位置の取得関数
 * 同じ方向へ適用した重ならない編集を１組へ纏め，組毎の逆マッピングを二分探索する
 * 計算量：初回の編集数 P に対し O(P)，以後は準備の組数 B に対し O(B log(P + 1))，追加領域 O(P)
 * @param Offset 修復前の解析用の位置
 * @return 準備前のテキストの位置
 * @throws std::bad_alloc 準備の組の領域を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
uint32_t ParsedTree::OffsetBeforePreparation(const uint32_t Offset) const {
	// 準備だけを戻した位置の取得
	if(PreparationBatches.empty() && !PreparationEdits.empty()) {
		AppliedEditBatches Batches;
		for(size_t Begin = 0; Begin < PreparationEdits.size();) {
			size_t End = Begin + 1;
			const bool IsForward = End == PreparationEdits.size() || PreparationEdits[End].Start > PreparationEdits[Begin].Start;
			for(; End < PreparationEdits.size(); ++End) {
				if(
					const AppliedEdit &Prev = PreparationEdits[End - 1], &Cur = PreparationEdits[End];
					IsForward ? Cur.Start < Prev.NewEnd : Cur.End > Prev.Start
				) break;
			}
			std::vector<AppliedEdit> Batch(
				PreparationEdits.begin() + static_cast<std::ptrdiff_t>(Begin),
				PreparationEdits.begin() + static_cast<std::ptrdiff_t>(End)
			);
			int64_t Delta = 0;
			if(IsForward) for(AppliedEdit &Edit : Batch) {
				const int64_t Change = static_cast<int64_t>(Edit.End - Edit.Start) - (Edit.NewEnd - Edit.NewStart);
				Edit.Start = static_cast<uint32_t>(Edit.Start + Delta);
				Edit.End = static_cast<uint32_t>(Edit.End + Delta);
				Delta += Change;
			} else {
				std::reverse(Batch.begin(), Batch.end());
				for(AppliedEdit &Edit : Batch) {
					const int64_t Change = static_cast<int64_t>(Edit.NewEnd - Edit.NewStart) - (Edit.End - Edit.Start);
					Edit.NewStart = static_cast<uint32_t>(Edit.NewStart + Delta);
					Edit.NewEnd = static_cast<uint32_t>(Edit.NewEnd + Delta);
					Delta += Change;
				}
			}
			Batches.push_back(std::move(Batch));
			Begin = End;
		}
		PreparationBatches = std::move(Batches);
	}
	// 準備前の位置の返戻
	return OffsetBeforeBatches(Offset, PreparationBatches);
}

/**
 * 修復前の本文の位置の取得関数
 * 計算量：編集の組の数 B と組毎の編集数 E に対し O(B log(E + 1))
 * @param Offset 修復後の位置
 * @return 修復前の本文の位置
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
uint32_t ParsedTree::OffsetBeforeEdits(const uint32_t Offset) const {
	// 修復だけを戻した位置の返戻
	return OffsetBeforeBatches(Offset, EditBatches);
}

/**
 * 解析済判定関数
 * 計算量：O(1)
 * @return 構文木を保持して居れば true
 */
bool ParsedTree::IsParsed() const {
	// 構文木を保持して居るかの返戻
	return Syntax.Tree != nullptr;
}

/**
 * 原文順のコメントの範囲の表の取得関数
 * コメントの種類の判定（行頭・行末で前後のコメントを飛ばす）を二分探索にし，位置からノードを引き直す費用を避ける
 * 木の世代毎に１度だけ作り，木・本文の差替で捨てる（並列の区間へ入る前に呼ぶ事）
 * 計算量：ノード数 V，コメント数 C，位置と隙間の読取量 R，Go の本文長 B に対し初回 O(V + C log(C + 1) + R + B)，以後 O(1)
 * @return 始まりと，末尾の改行を除いた終わりの対の列（始まりの順）
 * @throws std::bad_alloc コメント又は空行の位置表のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
const std::vector<ParsedTree::CommentSpan> &ParsedTree::GetCommentSpans() const {
	// 原文順のコメントの範囲の表の取得
	if(!Syntax.HasCommentSpans) {
		// 未反映の編集が有れば此処で再解析が走り，表とマーカーを捨てる為，マーカーは作った後に立てる
		if(IsParsed()) {
			const ReadingRules &Reading = GetReading();
			if(!Syntax.HasCommentNodes) CollectCommentRanges(0);
			// 幅０のノードの回収で前後する為の始まりでの整列
			std::sort(
				Syntax.CommentSpans.begin(),
				Syntax.CommentSpans.end(),
				[](const CommentSpan &Lhs, const CommentSpan &Rhs) -> bool {
					// 始まりの順の比較の返戻
					return Lhs.Start < Rhs.Start;
				}
			);
			const std::string_view Code = GetCodeText();
			// 原文順に，同じ行の前のコメントを継いだ行頭の判定（`/* a */ /* b */` の `/* b */` も行頭）
			for(size_t Idx = 0; Idx < Syntax.CommentSpans.size(); ++Idx) {
				CommentSpan &Span = Syntax.CommentSpans[Idx];
				const uint32_t Lead = TextEdit::SkipSpaceLeft(*this, Reading, Span.Start);
				Span.IsLineHead =
				!Lead || at(Lead - 1) == '\n' || Idx && Syntax.CommentSpans[Idx - 1].End == Lead && Syntax.CommentSpans[Idx - 1].IsLineHead;
			}
			// 逆順に，後の空白と続くコメントを飛ばした位置（鎖の末尾から継ぐ）
			for(size_t Idx = Syntax.CommentSpans.size(); Idx--;) {
				CommentSpan &Span = Syntax.CommentSpans[Idx];
				const uint32_t After = TextEdit::SkipSpaceRight(Code, Reading, Span.End);
				const bool IsChained =
				Idx + 1 < Syntax.CommentSpans.size() && Syntax.CommentSpans[Idx + 1].Start == After && Syntax.CommentSpans[Idx + 1].End > After;
				Span.RunEnd = IsChained ? Syntax.CommentSpans[Idx + 1].RunEnd : After;
			}
			// Go の前文と最上位の所属が共有する空行の位置表
			if(Reading.HasCommentAbsorbingDecls && !Syntax.CommentSpans.empty()) {
				uint32_t Prev = 0;
				bool HasNewline = false, IsBlank = false;
				for(uint32_t Pos = 0; Pos < size(); ++Pos) switch(at(Pos)) {
				case '\n':
					if(HasNewline && IsBlank) Syntax.BlankLines.emplace_back(Prev, Pos);
					Prev = Pos;
					HasNewline = true;
					IsBlank = true;
					break;
				case ' ':
				case '\t':
				case '\r':
					break;
				default:
					IsBlank = false;
					break;
				}
			}
		}
		Syntax.HasCommentSpans = true;
	}
	// コメントの範囲の表の返戻
	return Syntax.CommentSpans;
}

/**
 * 原文でのコメントの種類の判定関数
 * 行の前に空白とブロックコメントだけが在れば行頭，後に空白とコメントだけが続いて改行（PHP は閉タグ `?>` も）に至れば行末とする
 * 計算量：ノード数 N とコメント数 C に対し初回 O(N)，以後 O(log(C + 1))
 * @param Comment コメントのノード
 * @return 原文でのコメントの種類
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
CommentKind ParsedTree::GetKindOf(const TSNode Comment) const {
	// コメントの本文の範囲（表と同じキーで引く為，末尾の改行とエラー回復の頭の空白を除いた位置）
	const std::pair<uint32_t, uint32_t> Range = GetCommentRangeOf(Comment);
	// 種類の返戻
	return ClassifyComment(GetCommentSpans(), Range.first, Range.second);
}

/**
 * ノードのテキストが役割を持つトークンの綴りかの判定関数（エラーの回復がトークンをエラー領域のテキストだけで残す形を含む）
 * 計算量：テキスト長 L と固定の候補数 K に対し O(K L)
 * @param Node パーサーのノード
 * @param Wanted トークンの役割
 * @return テキストと同じ綴りのトークンが文法の定義で役割を持てば true（言語の指定が無ければ false）
 */
bool ParsedTree::IsTokenSpelling(const TSNode Node, const Core::Role Wanted) const {
	// 言語の指定が無いかノードの無い事の返戻
	if(!Grammar || ts_node_is_null(Node)) return false;
	const TSSymbol Symbol = Grammar->GetTokenSymbol(GetView(Node));
	// 同じ綴りのトークンの役割の返戻
	return Symbol && Grammar->GetKindOf(Symbol).Roles.test(static_cast<size_t>(Wanted));
}

/**
 * 使用位置の定義から，必要なマクロの性質を共通に辿る関数
 * 参照辺毎に伝わる性質を保ち，循環又は合流した経路で確認済のビットは読み直さない
 * 計算量：到達する名前数 V，参照数 E，名前毎の状態検索費用 Q に対し O(V Q + E)（性質の数は固定）
 * @param Name 対象名
 * @param Use 使用位置
 * @param Mask 確認する性質のビット
 * @return 有効な定義から確定した性質のビット
 * @throws std::bad_alloc 走査のメモリを確保出来ない場合
 * @throws std::length_error 走査列が要素数上限を超える場合
 */
uint8_t ParsedTree::GetMacroProps(const std::string_view Name, const uint32_t Use, const uint8_t Mask) const {
	// 通常の名前と取消済のマクロは，探索列を確保する前に判定する
	const MacroDef Initial = GetMacroDef(Name, Use);
	// 取消済の定義の返戻
	if(Initial.State == MacroState::Cancelled) return 0;
	const uint8_t Stringizing = static_cast<uint8_t>(MacroProp::Stringizing);
	// 標準の `assert` の契約の返戻
	if(!Initial.Body && Mask & Stringizing && GetProfile().IsSpelling(Name, Core::Word::StringizingAssertion)) return Stringizing;
	// 展開する本体の無い名前の返戻
	if(Initial.State == MacroState::Undefined || !Initial.Body) return 0;
	uint8_t Result = Initial.Body->Props & Mask;
	// 定義自身から全ての必要な性質を確定した結果の返戻
	if(Result == Mask) return Result;
	std::vector<std::pair<std::string_view, uint8_t>> Names;
	std::vector<std::pair<const MacroBody *, uint8_t>> Bodies { { Initial.Body.get(), Mask } };
	std::unordered_map<std::string_view, uint8_t> SeenNames { { Name, Mask } };
	std::unordered_map<const MacroBody *, uint8_t> SeenBodies;
	while(!Names.empty() || !Bodies.empty()) {
		if(!Names.empty()) {
			const auto [Current, Needed] = Names.back();
			Names.pop_back();
			uint8_t &Seen = SeenNames[Current];
			const uint8_t Wanted = Needed & ~Seen & ~Result;
			if(!Wanted) continue;
			Seen |= Wanted;
			const MacroDef Def = GetMacroDef(Current, Use);
			if(Def.State == MacroState::Cancelled) continue;
			// 別名から到達した標準の `assert` の契約の記録
			const bool HasStandardContract =
			!Def.Body && Wanted & Stringizing && GetProfile().IsSpelling(Current, Core::Word::StringizingAssertion);
			if(HasStandardContract) Result |= Stringizing;
			if(Def.State != MacroState::Undefined && Def.Body) Bodies.emplace_back(Def.Body.get(), Wanted);
		} else {
			const auto [Body, Needed] = Bodies.back();
			Bodies.pop_back();
			uint8_t &Seen = SeenBodies[Body];
			const uint8_t Wanted = Needed & ~Seen & ~Result;
			if(!Wanted) continue;
			Seen |= Wanted;
			Result |= Body->Props & Wanted;
			for(const std::pair<std::string_view, uint8_t> &Ref : Body->Refs) {
				const uint8_t Forwarded = Wanted & Ref.second & ~Result;
				if(Forwarded) Names.emplace_back(Ref.first, Forwarded);
			}
			for(const std::shared_ptr<const MacroBody> &Alt : Body->Alternatives) if(Alt) Bodies.emplace_back(Alt.get(), Wanted);
		}
		// 全ての必要な性質を確認した結果の返戻
		if((Result & Mask) == Mask) return Result;
	}
	// 到達した定義から確定した性質の返戻
	return Result;
}

/**
 * 使用位置の定義と参照先が指定した性質を持つかの判定関数
 * 計算量：性質の共有取得の費用 Q に対し O(Q)
 * @param Name 対象名
 * @param Use 使用位置
 * @param Prop 照会する性質
 * @return 到達する有効な定義が性質を持つ場合は true
 * @throws std::bad_alloc 走査のメモリを確保出来ない場合
 * @throws std::length_error 走査列が要素数上限を超える場合
 */
bool ParsedTree::HasMacroProp(const std::string_view Name, const uint32_t Use, const MacroProp Prop) const {
	// 照会した性質の有無の返戻
	return static_cast<bool>(GetMacroProps(Name, Use, static_cast<uint8_t>(Prop)));
}

/**
 * マクロの置換本体のトークン化関数
 * 計算量：本文の長さ N に対し O(N)
 * @param Body 置換本体
 * @return 引用領域を分離しコメントを除いたトークン列
 * @throws std::bad_alloc トークン列を確保出来ない場合
 * @throws std::length_error トークン列が要素数上限を超える場合
 */
std::vector<ParsedTree::MacroToken> ParsedTree::TokenizeMacroBody(const std::string_view Body) const {
	// ディレクティブの途中のブロックコメントの開きに置いた解析用のマーカー
	const unsigned char Marker = GetAnalysisMark();
	const std::string_view Concatenation = GetRules().GetSpelling(Core::Word::MacroConcatenation);
	std::vector<MacroToken> Tokens;
	for(size_t Pos = 0; Pos < Body.size();) {
		if(std::isspace(static_cast<unsigned char>(Body[Pos])) || Body.substr(Pos, 2) == "\\\n") {
			Pos += Body[Pos] == '\\' ? 2 : 1;
			continue;
		}
		// 原記号と解析用のマーカーの両方でブロックコメントを読飛
		if(Body[Pos] == '/' && Pos + 1 < Body.size() && (Body[Pos + 1] == '*' || Marker && Body[Pos + 1] == Marker)) {
			const size_t End = Body.find("*/", Pos + 2);
			Pos = End == std::string_view::npos ? Body.size() : End + 2;
			continue;
		}
		if(Body.substr(Pos, 2) == "//") {
			while(Pos < Body.size() && (Body[Pos] != '\n' || Pos && Body[Pos - 1] == '\\')) ++Pos;
			continue;
		}
		const size_t Start = Pos;
		const bool IsIdentifier = IsIdentifierChar(Body[Pos]) && !std::isdigit(static_cast<unsigned char>(Body[Pos]));
		if(IsIdentifierChar(Body[Pos])) {
			while(Pos < Body.size() && (IsIdentifierChar(Body[Pos]) || !IsIdentifier && Body[Pos] == '\'')) ++Pos;
		}
		const std::string_view Prefix = Body.substr(Start, Pos - Start);
		const bool IsRawPrefix = std::ranges::find(GetReading().RawStrPrefixes, Prefix) != GetReading().RawStrPrefixes.end();
		const bool IsRaw =
		IsRawPrefix && std::ranges::find(GetReading().MacroRawStrPrefixes, Prefix) != GetReading().MacroRawStrPrefixes.end() &&
		Pos < Body.size() && Body[Pos] == '"';
		const bool IsPrefixed =
		IsRaw || !IsRawPrefix && std::ranges::find(GetReading().StrPrefixes, Prefix) != GetReading().StrPrefixes.end();
		if(Pos < Body.size() && (Body[Pos] == '"' || Body[Pos] == '\'') && (Pos == Start || IsPrefixed)) {
			const unsigned char Quote = Body[Pos++];
			if(IsRaw) {
				const size_t Delimiter = Pos;
				while(Pos < Body.size() && Body[Pos] != '(' && Pos - Delimiter < 17) ++Pos;
				if(Pos < Body.size() && Body[Pos] == '(') {
					const std::string Close = ")" + std::string(Body.substr(Delimiter, Pos - Delimiter)) + "\"";
					const size_t End = Body.find(Close, Pos + 1);
					Pos = End == std::string_view::npos ? Body.size() : End + Close.size();
				} else Pos = Body.size();
			} else while(Pos < Body.size()) {
				const unsigned char Byte = Body[Pos++];
				if(Byte == Quote) break;
				if(Byte == '\\' && Pos < Body.size()) ++Pos;
			}
			Tokens.push_back({ Body.substr(Start, Pos - Start), false, true });
		} else if(Pos > Start) Tokens.push_back({ Prefix, IsIdentifier, false });
		else {
			Pos += !Concatenation.empty() && Body.substr(Pos).starts_with(Concatenation) ? Concatenation.size() : 1;
			Tokens.push_back({ Body.substr(Start, Pos - Start), false, false });
		}
	}
	// 引用領域を分離したトークン列の返戻
	return Tokens;
}

/**
 * 対象内の名前の使用位置で式・宣言子マクロの性質を収集する関数
 * 引用領域とコメントの内部は名前として読まず，両方の性質に同じトークン列を使う
 * 計算量：テキスト長 N，名前の数 K，性質の照会費用 Q に対し O(N + K Q)，定義表が空で文字列化を照会しなければ O(1)
 * @param Node 対象ノード
 * @param Mask 収集するマクロの性質
 * @param ExprUses 式へ展開するマクロの使用位置の追記先
 * @param DeclaratorUses 宣言子へ展開するマクロの使用位置の追記先
 * @throws std::bad_alloc トークン列・マクロの照会又は使用位置の追記先を確保出来ない場合
 * @throws std::length_error 作業列又は使用位置の追記先が要素数上限を超える場合
 */
void ParsedTree::CollectMacroUses(
	const TSNode Node,
	const uint8_t Mask,
	std::vector<uint32_t> &ExprUses,
	std::vector<uint32_t> &DeclaratorUses
) const {
	// 置換本体の無い定義表から式・宣言子の性質は生じず，標準の文字列化の契約だけは別に残す
	if((!NameData || NameData->MacroDefs.empty()) && !(Mask & static_cast<uint8_t>(MacroProp::Stringizing))) return;
	const std::string_view Text = GetView(Node);
	for(const MacroToken &Token : TokenizeMacroBody(Text)) {
		if(!Token.IsIdentifier) continue;
		const uint32_t Use = GetStart(Node) + static_cast<uint32_t>(Token.Text.data() - Text.data());
		const uint8_t Props = GetMacroProps(Token.Text, Use, Mask);
		if(Props & static_cast<uint8_t>(MacroProp::Expr)) ExprUses.push_back(Use);
		if(Props & static_cast<uint8_t>(MacroProp::Declarator)) DeclaratorUses.push_back(Use);
	}
	// 終了
	return;
}

/**
 * Swift の前置の演算子の呼出と読んだ括弧の実引数の並びの取得関数
 * tree-sitter-swift は前置の演算子の後の括弧 (`-(y)` / `!(y)`) を演算子の呼出と読む
 * 否定の `!` は名前付の `bang` のノードに為り，中置の式の後の前置 (`x + -(y)`) は其の中置の式の末尾のトークンに為る（`(x + -)(y)` と読む）
 * 計算量：ノード数 N に対し初回 O(N)，以後の取得は平均 O(1)
 * @param Node 対象ノード
 * @return 前置の演算子の被演算子を包む括弧と読んだ実引数の並び（其の形でなければ null）
 * @throws std::bad_alloc 直接親の表を確保出来ない場合
 */
TSNode ParsedTree::GetSwiftPrefixArgs(const TSNode Node) const {
	// 前置の演算子の呼出の形でない事の返戻
	if(ts_node_child_count(Node) != 2 || !Is(Node, Core::Role::CallExpr)) return {};
	// 呼出側の前置演算子の候補
	TSNode Prefix = ts_node_child(Node, 0);
	if(IsNamedNode(Prefix) && !Is(Prefix, Core::Role::ExclamationMark)) {
		// 中置式末尾の演算子候補への移動
		const uint32_t Count = ts_node_child_count(Prefix);
		Prefix = Count > 1 ? ts_node_child(Prefix, Count - 1) : TSNode{};
		// 末尾が演算子のトークンでない事の返戻
		if(ts_node_is_null(Prefix) || ts_node_is_named(Prefix)) return {};
	}
	// 呼出接尾辞と実引数列の検査
	const TSNode Suffix = ts_node_child(Node, 1);
	if(
		!IsOperatorChar((*this)[GetStart(Prefix)]) || !Is(Suffix, Core::Role::CallSuffix) || ts_node_named_child_count(Suffix) != 1
	// 前置の演算子と実引数の並びの組でない事の返戻
	) return {};
	// 実引数ノードの取得
	const TSNode Args = ts_node_named_child(Suffix, 0);
	// 実引数の並びの返戻
	return Is(Args, Core::Role::ValArgs) ? Args : TSNode{};
}

/**
 * Ruby の符号を密着させても読みが変わらない被演算子かの判定関数
 * 計算量：被演算子と其の子のテキストの総長 L に対し O(L)
 * @param Operand 符号の被演算子
 * @return 密着させても読みが変わらなければ true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
bool ParsedTree::CanAttachRubySign(const TSNode Operand) const {
	// 数字で始まる被演算子の検査
	if(
		ts_node_is_null(Operand) || GetStart(Operand) >= size() || !std::isdigit(static_cast<unsigned char>(at(GetStart(Operand))))
	// 空ノードと数字で始まらない被演算子の返戻（空ノードは位置も親も引けない）
	) return true;
	// 空ノードの親は引けない為，先に確かめてから外側を辿る
	// 後置の受手と読まれた離れた符号の返戻
	if(
		const TSNode Unary = GetParentOf(Operand), Outer = ts_node_is_null(Unary) ? TSNode{} : GetParentOf(Unary);
		!ts_node_is_null(Unary) && Is(Unary, Core::Role::UnaryVal) && GetStart(Unary) + 1 < GetStart(Operand) &&
		Is(Outer, Core::Role::PostfixReceiverChain) && ts_node_eq(ts_node_named_child(Outer, 0), Unary)
	) return false;
	// 符号直下の数値底
	const TSNode Base =
	Is(Operand, Core::Role::Binary) && Is(GetFieldChild(Operand, Core::ChildRole::OperatorField), Core::Role::ExponentToken) ?
	ts_node_named_child(Operand, 0) :
	Operand;
	// 数値だけか，数値を底とする冪かの返戻
	return Is(Base, Core::Role::NumLiteral) || Is(Base, Core::Role::NumericUnitSuffix);
}

/**
 * 指定バイトを含むノードの降下経路の取得関数
 * 計算量：走査する子数 C と深さ D に対し O(C + D)，追加領域 O(D)
 * @param Pos 対象のバイト位置
 * @return 根から最も内側のノード迄の経路
 * @throws std::bad_alloc 降下経路の領域を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::vector<TSNode> ParsedTree::GetAncestorsAt(const uint32_t Pos) const {
	// 対象の根の取得
	const TSNode Root = GetRoot();
	std::vector<TSNode> Path { Root };
	HeldCursor Cursor(Root);
	while(ts_tree_cursor_goto_first_child_for_byte(&Cursor, Pos) > -1) {
		const TSNode Node = ts_tree_cursor_current_node(&Cursor);
		if(GetStart(Node) > Pos || Pos >= GetEnd(Node)) break;
		Path.push_back(Node);
	}
	// 親を根から引き直さずに得た経路の返戻
	return Path;
}

/**
 * 行端の扱いが TypeScript と Babel で分かれる JSX 本文かの判定関数
 * 計算量：本文長 N に対し O(N)
 * @param Text 処理系が１つの本文と読むテキスト（前後の子の間）
 * @return 行端を動かすと何方かの表示が変わるなら true
 */
bool ParsedTree::HasDisputedJsxLineEdges(const std::string_view Text) const {
	// 最後に通過した空白の文字参照の終わり
	size_t LastBlankRefEnd = std::string_view::npos;
	// JSX 本文の各トークンの走査
	for(size_t Pos = 0; Pos < Text.size();) {
		if(Text[Pos] == '&') {
			const size_t Len = BlankRef(Text, Pos).first;
			LastBlankRefEnd = Len ? Pos + Len : std::string_view::npos;
		}
		// 次のコードポイント位置
		size_t Next = Pos;
		// 現在のコードポイント
		char32_t Point = 0;
		if(!TextEdit::IsUtf8Decoded(Text, Next, Point)) Next = Pos + 1;
		// Babel が改行と読まない行の区切の返戻
		if(Point == 0X2028 || Point == 0X2029) return true;
		if(Point == '\n' || Point == '\r') {
			const ReadingRules &Reading = GetReading();
			// 改行の前の半角空白・タブを除いた行末の字
			size_t Left = Pos;
			while(Left && (Text[Left - 1] == ' ' || Text[Left - 1] == '\t')) --Left;
			if(Left) {
				// 行末コードポイントの先頭候補
				size_t Lead = Left - 1;
				while(Lead && (static_cast<unsigned char>(Text[Lead]) & 0XC0) == 0X80) --Lead;
				size_t After = Lead;
				// 改行直前のコードポイント
				char32_t Before = 0;
				TextEdit::IsUtf8Decoded(Text, After, Before);
				// 行末に在る半角空白・タブ以外の空白又は空白の文字参照の返戻
				if(Reading.IsJsxSpace(Before) || Before == ';' && LastBlankRefEnd == Left) return true;
			}
			// 改行の後の半角空白・タブを除いた行頭の字
			size_t Right = Next;
			while(Right < Text.size() && (Text[Right] == ' ' || Text[Right] == '\t')) ++Right;
			// 行頭コードポイントの次位置
			size_t After = Right;
			// 改行直後のコードポイント
			if(char32_t Head = 0; TextEdit::IsUtf8Decoded(Text, After, Head) && Reading.IsJsxSpace(Head) || BlankRef(Text, Right).first) {
				// 行頭に在る半角空白・タブ以外の空白又は空白の文字参照の返戻
				return true;
			}
		}
		// 次のコードポイントへの前進
		Pos = Next;
	}
	// 両者の読みが同じ本文の返戻
	return false;
}

/**
 * マクロの本体のトークンが呑んだ行コメントの開始位置の取得関数（整形仕様 4.2 のディレクティブ本文内のコメント）
 * tree-sitter-c はマクロの本体を行末迄の１トークンとして読み，本体の後の `//` もコメントのノードにせずトークンへ含める
 * 文字・文字列のリテラルと，解析用のマーカーで覆ったブロックコメントの中の `//` はコメントの開始でない
 * 計算量：マクロ本体のテキスト長 L に対し O(L)
 * @param Body マクロの本体のトークン
 * @return 行コメントの開始位置（無ければ本体の終わり）
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
uint32_t ParsedTree::GetMacroLineCommentStart(const TSNode Body) const {
	// リテラルと覆ったブロックコメントを飛ばす走査状態の準備
	const uint32_t End = ts_node_end_byte(Body);
	const unsigned char Marker = GetAnalysisMark();
	unsigned char Quote = '\0';
	bool IsMaskedComment = false;
	for(uint32_t Pos = ts_node_start_byte(Body); Pos + 1 < End; ++Pos) {
		if(const unsigned char Char = at(Pos), Next = at(Pos + 1); IsMaskedComment && Char == '*' && Next == '/') {
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
 * ソース移動代入関数
 * 計算量：新旧の本文長 B と控えの要素数 N に対し O(B + N)
 * @param NewSrc 新しいソース文字列（ムーブ済）
 */
void ParsedTree::Assign(std::string &&NewSrc) {
	// 移動本文の共通代入処理
	AssignSrc(std::move(NewSrc));
	// 解析用の挿入・置換位置と修復の編集・退避したコメントは，本文を全面差替えした後のトークンの由来を表さない
	InsertedBreaks.clear();
	SyntheticGroups.clear();
	LexemeRestorations.clear();
	DirectiveLines.clear();
	EditBatches.clear();
	PreparationEdits.clear();
	PreparationBatches.clear();
	EscapedComments.clear();
	OwnedRegions.clear();
	// 終了
	return;
}

/**
 * 取得済の型から宣言と式の曖昧性を判定する関数
 * 型名と宣言子の間に演算子又は型引数が在る場合は，同じファイルの型宣言で型の読みに確証を要する
 * 計算量：修飾名の深さ D，型引数の子数 C，各名前の宣言判定の費用 Q に対し O(D + C + Q)
 * @param Node 式とも読める宣言の候補
 * @param Type 宣言の型のフィールド
 * @return 型の読みを確定出来ない宣言なら true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool ParsedTree::IsAmbiguousDeclType(const TSNode Node, const TSNode Type) const {
	// 型の記号から変更不能な役割の定義を１度だけ引く
	const auto GetTypeRoles = [this](const TSNode TypeNode) -> const Core::RoleSet * {
		// 文法又は型ノードが無い場合の返戻
		if(!Grammar || ts_node_is_null(TypeNode)) return nullptr;
		// 同じ型の判定で共有する固定定義の返戻
		return &Grammar->GetKindOf(GetSymbol(TypeNode)).Roles;
	};
	const bool IsKnown = TemplateDecls.contains(Node.id);
	const Core::RoleSet *const TypeRoles = IsKnown ? nullptr : GetTypeRoles(Type);
	// 宣言子を読まずに確定出来る形の返戻（頻出する組込型から確かめる）
	if(
		IsKnown || TypeRoles && (
			TypeRoles->test(static_cast<size_t>(Core::Role::PrimitiveType)) ||
			TypeRoles->test(static_cast<size_t>(Core::Role::SizedTypeSpecifier)) ||
			TypeRoles->test(static_cast<size_t>(Core::Role::ClassLike)) ||
			TypeRoles->test(static_cast<size_t>(Core::Role::EnumSpecifier)) ||
			TypeRoles->test(static_cast<size_t>(Core::Role::PlaceholderTypeSpecifier)) ||
			TypeRoles->test(static_cast<size_t>(Core::Role::ValCategoryType))
		)
	) return false;
	TSNode Declarator = GetFieldChild(Node, Core::ChildRole::Declarator), Last = Type;
	// 初期化子を除いた宣言子（`T x = 1` も名前が並ぶ為に式に為らない）と，修飾名の最後の成分（`ns::T<1>` の型引数付の名前）
	if(Is(Declarator, Core::Role::InitDeclarator)) Declarator = GetFieldChild(Declarator, Core::ChildRole::Declarator);
	const Core::RoleSet *LastRoles = TypeRoles;
	while(LastRoles && LastRoles->test(static_cast<size_t>(Core::Role::QualifiedIdentifier))) {
		Last = GetFieldChild(Last, Core::ChildRole::Name);
		LastRoles = GetTypeRoles(Last);
	}
	const bool IsTemplate = LastRoles && LastRoles->test(static_cast<size_t>(Core::Role::TemplateInstantiationType));
	TSNode Binding = Declarator;
	while(Is(Binding, Core::Role::RefDeclarator)) Binding = ts_node_named_child(Binding, 0);
	// ２つの名前が並ぶ形又は構造化束縛の返戻
	if(!IsTemplate && Is(Declarator, Core::Role::Identifier) || Is(Binding, Core::Role::StructuredBindingDeclarator)) return false;
	if(IsTemplate) {
		const TSNode Args = GetFieldChild(Last, Core::ChildRole::Args);
		for(uint32_t Idx = 0; Idx < ts_node_named_child_count(Args); ++Idx) {
			// 型引数に組込型を持つ形（`<` を比較と読むと組込型の語が式に為らない）の返戻
			if(
				const TSNode Arg = GetFieldChild(ts_node_named_child(Args, Idx), Core::ChildRole::Type);
				Is(Arg, Core::Role::PrimitiveType) || Is(Arg, Core::Role::SizedTypeSpecifier)
			) return false;
		}
	}
	const TSNode Name = IsTemplate ? ts_node_named_child(Last, 0) : Type;
	const bool IsTypeName =
	IsTemplate ? Is(Name, Core::Role::TypeName) : TypeRoles && TypeRoles->test(static_cast<size_t>(Core::Role::TypeName));
	// 裸の型名でない型（修飾名等）は式を否定出来ない事の返戻
	if(!IsTypeName) return true;
	const std::string_view Spelling = GetView(Name);
	const uint32_t Use = ts_node_start_byte(Node);
	// 値の宣言が先行する名前と，使用位置で型の宣言を確認出来ない名前は式を否定出来ない事の返戻
	return !IsDeclaredTypeName(Spelling, Use);
}

/**
 * 式と区別出来ない宣言かの判定関数
 * 型名と宣言子の間に演算子又は型引数が在る場合は，同じファイルの型宣言で型の読みに確証を要する
 * 計算量：修飾名の深さ D，型引数の子数 C，各名前の宣言判定の費用 Q に対し O(D + C + Q)
 * @param Node 宣言の候補
 * @return 型の読みを確定出来ない宣言なら true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool ParsedTree::IsAmbiguousDecl(const TSNode Node) const {
	// 宣言の種類を絞ってから，共通の判定へ型のフィールドを渡す
	if(!Is(Node, Core::Role::PotentialExprDecl)) return false;
	// 型と宣言子に依る曖昧性の返戻
	return IsAmbiguousDeclType(Node, GetFieldChild(Node, Core::ChildRole::Type));
}

/**
 * ========== 構文情報収集 ==========
 * 式と読まれた括弧付宣言の構成の確定関数
 * 計算量：宣言のノード数 N と名前の照会量 Q に対し O(N + Q)
 * @param Stmt 宣言の位置の式文
 * @return 型と全宣言子を確定した構成，宣言と確定出来ない場合は無
 * @throws std::bad_alloc 宣言子とラッパーの列を確保出来ない場合
 * @throws std::length_error 宣言子又はラッパーの列の上限を超える場合
 */
std::optional<ParsedTree::ParenthesizedDecl> ParsedTree::ReadParenthesizedDecl(const TSNode Stmt) const {
	// 宣言を置けない文は親の表を構築せずに除外
	if(!GetReading().HasPreprocMacros || !Is(Stmt, Core::Role::ExprStmt) || ts_node_has_error(Stmt)) return std::nullopt;
	if(const std::vector<Core::Role> &Owners = GetReading().ParenthesizedDeclOwners; !Owners.empty()) {
		const TSNode Owner = GetParentOf(Stmt);
		if(
			!std::ranges::any_of(
				Owners,
				[this, Owner](const Core::Role Shape) -> bool {
					// 所有構文が登録した宣言の場かの返戻
					return Is(Owner, Shape);
				}
			)
		// 所有構文が宣言を許さない場合の返戻
		) return std::nullopt;
	}
	ParenthesizedDecl Result;
	Result.Stmt = Stmt;
	// 単一の式は作業列を確保せずに読み，カンマ式の右側だけを保留する
	std::vector<TSNode> Pending;
	TSNode Expr = ts_node_named_child(Stmt, 0);
	while(true) {
		while(Is(Expr, Core::Role::CommaExpr)) {
			Result.Wrappers.push_back(Expr);
			Pending.push_back(GetFieldChild(Expr, Core::ChildRole::Right));
			Pending.push_back(ts_node_child(Expr, 1));
			Expr = GetFieldChild(Expr, Core::ChildRole::Left);
		}
		if(Is(Expr, Core::Role::CommaToken)) {
			Result.Commas.push_back(Expr);
			if(Pending.empty()) break;
			Expr = Pending.back();
			Pending.pop_back();
			continue;
		}
		DeclaratorItem Item;
		Item.Expr = Expr;
		const bool HasInitializer = Is(Expr, Core::Role::AssignmentOperation);
		// 代入記号でない初期化の返戻
		if(HasInitializer && !IsTokenSpelling(ts_node_child(Expr, 1), Core::Role::AssignmentToken)) return std::nullopt;
		TSNode Node = HasInitializer ? GetFieldChild(Expr, Core::ChildRole::Left) : Expr;
		Item.Root = Node;
		Item.Val = HasInitializer ? GetFieldChild(Expr, Core::ChildRole::Right) : TSNode{};
		if(Result.Items.empty()) {
			if(Is(Node, Core::Role::BinaryOperation)) {
				// 型とポインタ・参照から始まる宣言列の修復
				const TSNode Token = ts_node_child(Node, 1);
				if(IsTokenSpelling(Token, Core::Role::AsteriskToken)) Item.Parts.push_back({ Node, Core::Role::PtrDeclarator });
				else if(IsTokenSpelling(Token, Core::Role::AmpersandToken) && GetProfile().FindSymbolWithRole(Core::Role::RefDeclarator)) {
					Item.Parts.push_back({ Node, Core::Role::RefDeclarator });
				// 宣言子を表す演算子でない形の返戻
				} else return std::nullopt;
				Result.Type = GetFieldChild(Node, Core::ChildRole::Left);
				Node = GetFieldChild(Node, Core::ChildRole::Right);
			} else {
				while(Is(Node, Core::Role::SubscriptExprNode) || Is(Node, Core::Role::CallExpr)) {
					if(Is(Node, Core::Role::CallExpr)) {
						const TSNode Args = GetFieldChild(Node, Core::ChildRole::Args);
						if(ts_node_named_child_count(Args)) break;
						Item.Parts.push_back({ Node, Core::Role::FunctionDeclarator });
						Item.Parts.push_back({ Args, Core::Role::DeclaratorParams });
					} else Item.Parts.push_back({ Node, Core::Role::ArrayDeclarator });
					Node = ts_node_named_child(Node, 0);
				}
				// 型と括弧付宣言子を持たない形の返戻
				if(!Is(Node, Core::Role::CallExpr)) return std::nullopt;
				Result.Call = Node;
				Result.Type = ts_node_named_child(Node, 0);
				Result.Args = GetFieldChild(Node, Core::ChildRole::Args);
				// 宣言子を１つに確定出来ない型の呼出の返戻
				if(ts_node_named_child_count(Result.Args) != 1) return std::nullopt;
				if(ts_node_eq(Item.Root, Result.Call)) Item.Root = Result.Args;
				Item.Parts.push_back({ Result.Args, Core::Role::ParenthesizedDeclarator });
				Node = ts_node_named_child(Result.Args, 0);
			}
			// 宣言へ戻せる型の構文以外の返戻
			if(
				!Is(Result.Type, Core::Role::IdentifierLeafLike) && !Is(Result.Type, Core::Role::PrimitiveType) &&
				!Is(Result.Type, Core::Role::SizedTypeSpecifier)
			) return std::nullopt;
		}
		while(!ts_node_is_null(Node) && !Is(Node, Core::Role::IdentifierLeafLike)) {
			if(Is(Node, Core::Role::AssignableWrapper) && ts_node_named_child_count(Node) == 1) {
				Item.Parts.push_back({ Node, Core::Role::ParenthesizedDeclarator });
			} else if(Is(Node, Core::Role::AddressOperation) && ts_node_named_child_count(Node) == 1) {
				if(const TSNode Operator = ts_node_child(Node, 0); IsTokenSpelling(Operator, Core::Role::AsteriskToken)) {
					Item.Parts.push_back({ Node, Core::Role::PtrDeclarator });
				} else if(IsTokenSpelling(Operator, Core::Role::AmpersandToken) && GetProfile().FindSymbolWithRole(Core::Role::RefDeclarator)) {
					Item.Parts.push_back({ Node, Core::Role::RefDeclarator });
				} else break;
			} else if(Is(Node, Core::Role::SubscriptExprNode)) Item.Parts.push_back({ Node, Core::Role::ArrayDeclarator });
			else if(Is(Node, Core::Role::CallExpr) && !ts_node_named_child_count(GetFieldChild(Node, Core::ChildRole::Args))) {
				Item.Parts.push_back({ Node, Core::Role::FunctionDeclarator });
				Item.Parts.push_back({ GetFieldChild(Node, Core::ChildRole::Args), Core::Role::DeclaratorParams });
			} else break;
			Node = ts_node_named_child(Node, 0);
		}
		// 宣言名を持たない形の返戻
		if(!Is(Node, Core::Role::IdentifierLeafLike)) return std::nullopt;
		Item.Name = Node;
		for(const DeclaratorPart &Part : Item.Parts) {
			if(Part.Shape == Core::Role::ArrayDeclarator) {
				if(
					const TSNode Dimension = ts_node_named_child(Part.Node, 1);
					Is(Dimension, Core::Role::ArgSeq) ?
					ts_node_named_child_count(Dimension) > 1 || Is(ts_node_named_child(Dimension, 0), Core::Role::ObjectLike) :
					Is(Dimension, Core::Role::ObjectLike)
				// 宣言の寸法でない形の返戻
				) return std::nullopt;
			}
		}
		// 宣言子の構文を確定してから，値の名前又は未確認の型を除く返戻
		if(
			Result.Items.empty() && Is(Result.Type, Core::Role::IdentifierLeafLike) &&
			!IsDeclaredTypeName(GetView(Result.Type), ts_node_start_byte(Result.Type))
		) return std::nullopt;
		Result.Items.push_back(std::move(Item));
		if(Pending.empty()) break;
		Expr = Pending.back();
		Pending.pop_back();
	}
	// 文全体を確定した宣言の返戻
	return Result;
}

/**
 * 指定された役割を持つ最初の名前付子ノード取得関数
 * 計算量：直接の子数 C に対し O(C)
 * @param Node 親ノード
 * @param Role 子の役割
 * @return 対応する最初の名前付子ノード（無ければ空ノード）
 */
TSNode ParsedTree::GetFirstNamedChild(const TSNode Node, const Core::Role Role) const {
	// 対応する項目の探索
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
 * 準備と修復を戻した入力位置の取得関数
 * 計算量：初回の準備の編集数 P に対し O(P)，以後は準備・修復の組数 B と編集数 E に対し O(B log(E + 1))
 * @param Offset 準備・修復後の位置
 * @return 正規化した入力の位置
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
uint32_t ParsedTree::OffsetInInput(const uint32_t Offset) const {
	// 修復と準備を順に戻した入力位置の返戻
	return OffsetBeforePreparation(OffsetBeforeEdits(Offset));
}

/**
 * ========== 構文木管理 ==========
 * 構文木解放関数
 * 計算量：解放対象の構文木のノード数 V に対し O(V)
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
 * 実行停止後の逐語データの開始位置の取得関数
 * 計算量：原文長 B と試行解析・照会の総費用 Q に対し O(B + Q)
 * @return データの開始位置（無ければ `std::string::npos`）
 */
size_t ParsedTree::GetHaltedDataStart() const {
	// 保護する末尾の開始位置の返戻
	return HaltedData;
}

/**
 * 解析済の本文へ逐語データを付ける関数
 * 解析対象の本文を変えずに後置データだけを加える為，構文木の世代を保つ
 * 計算量：既存の本文長 B と追加のデータ長 D に対し O(B + D)
 * @param Data 実行停止後の逐語データ
 */
void ParsedTree::AttachTrailingData(const std::string_view Data) {
	// 解析済の本文へ逐語データを付ける
	HaltedData = size();
	std::string::append(Data);
	// 終了
	return;
}

/**
 * Sass のモジュールの節に属する許容エラーかの判定関数
 * 計算量：木の世代毎に初回 O(N + B)，以後平均 O(1)（ノード数 N，節の語の照合バイト数 B）
 * @param Node 対象のエラーノード
 * @return 最上位のモジュールの節又は其の節のエラーの内側なら true
 * @throws std::bad_alloc 直接親と許容エラーの表を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool ParsedTree::IsToleratedModuleError(const TSNode Node) const {
	// Sass のモジュールの節に属する許容エラーかの判定
	RecordNodeRelations();
	// 同じ木で１度確認したモジュールの節への所属の返戻
	return Syntax.Relations->ModuleErrors.contains(Node.id);
}

/**
 * 逐語の URL 値に属する偽の欠落の判定関数
 * 計算量：ノード数 N と照合するテキストの総長 B に対し初回平均 O(N + B)，以後平均 O(1)
 * @param Node 判定するノード
 * @return 引用符の無い URL 値に補われた偽の欠落なら true
 * @throws std::bad_alloc 木の世代毎の直接親と許容エラーの表を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
bool ParsedTree::IsToleratedCssUrlVal(const TSNode Node) const {
	// 逐語の URL 値の候補と異なるノードの返戻
	if(!Is(Node, Core::Role::NestingSelector) || !ts_node_is_missing(Node)) return false;
	RecordNodeRelations();
	// 同じ木で１度確認した逐語の URL 値への所属の返戻
	return Syntax.Relations->UrlValErrors.contains(Node.id);
}

/**
 * 見出の部品を指す場の子かの判定関数
 * 本体を指す場 (`body` / `consequence` / `alternative` / `clauses` / `else`) は含めない
 * 同じ問いを２箇所で書くと一覧が乖離する為，場の名前の一覧を此処だけで定める
 * 計算量：見出の役割数 F（固定）と各役割の子取得の費用 T に対し O(F + T)
 * @param Node 親のノード
 * @param Child 判定する子
 * @return 見出の部品なら true
 */
bool ParsedTree::IsHeaderField(const TSNode Node, const TSNode Child) const {
	// 見出の部品を指す場の名前
	static constexpr Core::ChildRole HeaderFields[] = {
		Core::ChildRole::Cond,
		Core::ChildRole::Exceptions,
		Core::ChildRole::Guard,
		Core::ChildRole::Name,
		Core::ChildRole::ObjectField,
		Core::ChildRole::ParamsField,
		Core::ChildRole::Pattern,
		Core::ChildRole::Superclass,
		Core::ChildRole::Val,
		Core::ChildRole::Var
	};
	// 場の名前毎の子の照合
	for(const Core::ChildRole Field : HeaderFields) {
		// 見出の部品である事の返戻
		if(const TSNode Part = GetFieldChild(Node, Field); !ts_node_is_null(Part) && ts_node_eq(Part, Child)) return true;
	}
	// 見出の部品でない事の返戻
	return false;
}

/**
 * if 文の真枝本体取得関数
 * 計算量：直接の子数 C に対し O(C)
 * @param Node if 文又は if 式のノード
 * @return 条件成立時に実行する本体（無ければ空ノード）
 */
TSNode ParsedTree::GetIfConsequence(const TSNode Node) const {
	// 真枝本体の場名は文法で異なり，C/C++/C#/Java/JS/TS 等は `consequence`，PHP は `body` を用いる
	const TSNode Consequence = GetFieldChild(Node, Core::ChildRole::Consequence);
	// 真枝本体の返戻（`consequence` を持たない文法では `body` へ退避する）
	return ts_node_is_null(Consequence) ? GetFieldChild(Node, Core::ChildRole::Body) : Consequence;
}

/**
 * 解析の打切の判定関数
 * 計算量：O(1)
 * @return 解析が時間の上限で打ち切られて居れば true
 */
bool ParsedTree::HasExpiredParse() const {
	// 打切の有無の返戻
	return Syntax.IsParseExpired;
}

/**
 * 同一親内の直前の名前付非コメント子取得関数（親を遡らない）
 * 計算量：ノード数 N と前の名前付兄弟数 C に対し初回 O(N + C)，以後 O(C)
 * @param Node 基準ノード
 * @return 直前の名前付非コメント子（見付からなければ空ノード）
 * @throws std::bad_alloc 直接親の表を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
TSNode ParsedTree::GetSiblingBeforeImmediate(const TSNode Node) const {
	// 現在のノードの親の取得
	const TSNode Parent = GetParentOf(Node);
	// 親の無いノードの直前の兄弟の返戻
	if(ts_node_is_null(Parent)) return {};
	TSNode Prev {};
	ForEachChild(
		Parent,
		[&](const TSNode Child) -> bool {
			// 対象自身に達した時の走査打切の返戻
			if(ts_node_eq(Child, Node)) return false;
			if(ts_node_is_named(Child) && !Is(Child, Core::Role::Comment)) Prev = Child;
			// 次の兄弟へ進む事の返戻
			return true;
		}
	);
	// 直前の名前付非コメント子又は空のノードの返戻
	return Prev;
}

/**
 * 可変構文木取得関数
 * 呼出側は得た木を `ts_tree_edit` で編集後のバイト位置へ動かす
 * 計算量：控えの破棄と未反映編集の解析の総費用 Q に対し O(Q)
 * @return 内部の構文木（解析前は nullptr）
 */
TSTree *ParsedTree::GetMutableTree() {
	// 内部の構文木の返戻
	return Syntax.Tree.get();
}

/**
 * 構文木へ編集を通知済である事の設定関数
 * 次の再解析で編集を通知した旧構文木を土台にする
 * 計算量：O(1)
 */
void ParsedTree::MarkTreeEdited() {
	// 差分解析の土台が編集後の本文に対応する事の記録
	Syntax.IsTreeEdited = true;
	// 終了
	return;
}

/**
 * ========== トークン・文脈判定 ==========
 * 文へ展開するマクロの呼出の文かの判定関数
 * 置換本体が複数の文や開いた if に為るマクロ (`#define SWAP(a, b) t = a; a = b; b = t`) の呼出は，単文の本体に置くと先頭の文だけが本体に為る
 * 計算量：テキスト長 L と名前の検索費用 Q に対し O(L + Q)
 * @param Stmt 判定する文
 * @return 使用位置で文へ展開するマクロの呼出だけの式文なら true
 */
bool ParsedTree::IsStmtMacroCall(const TSNode Stmt) const {
	// 式文でない場合の返戻
	if(!Is(Stmt, Core::Role::ExprStmt)) return false;
	// 式文の先頭式
	TSNode Name = ts_node_named_child(Stmt, 0);
	if(!ts_node_is_null(Name) && Is(Name, Core::Role::CallExpr)) Name = GetFieldChild(Name, Core::ChildRole::FunctionField);
	// マクロの名前かの返戻
	return !ts_node_is_null(Name) && Is(Name, Core::Role::Identifier) &&
	HasMacroProp(GetView(Name), GetStart(Name), MacroProp::Stmt);
}

/**
 * 構造化で省く最上位の空の式文 `;` かの判定関数
 * 破損した直前の子の宣言を終える `;` は其の子へ密着させて残す為に省かない
 * コメントの分離は省く `;` をアンカーにせず，中間表現の構築は其れを組立から除く
 * 計算量：対象のテキスト長 L と構文の照会費用 Q に対し O(L + Q)
 * @param Child 根の子
 * @param PrevChild 其の前のコメントでない根の名前付の子（無ければ空）
 * @return 省く `;` だけの式文なら true
 */
bool ParsedTree::IsOmittedSemicolon(const TSNode Child, const TSNode PrevChild) const {
	// 省く空の式文かの返戻
	return GetProfile().IsSpelling(GetView(Child), Core::Word::StmtEnd) && Is(Child, Core::Role::ExprStmt) &&
	(ts_node_is_null(PrevChild) || !ts_node_has_error(PrevChild));
}

/**
 * 真偽値名の判定の供給元の取得関数
 * 計算量：O(1)
 * @return 言語が使う予約語・前処理・有効範囲の判定の供給元
 */
ParsedTree::BoolNameSource ParsedTree::GetBoolNameSource() const {
	// 入力側で確定した真偽値名の判定の供給元の返戻
	return GetReading().BoolNames;
}

/**
 * 使用位置の真偽値名の束縛の判定関数
 * C 系は前処理状態，Go は宣言の有効範囲を使い，其の他は予約されたリテラルを使う
 * 計算量：名前毎の記録数 N とマクロの置換テキスト長 L に対し O(log(N + 1) + L)
 * @param Name 真偽値の名前
 * @param Use 使用位置
 * @return 同じ真偽値を表す名前を使える場合は true
 * @throws std::bad_alloc マクロの定義のコピー用のメモリを確保出来ない場合
 * @throws std::length_error マクロの定義のテキスト長が上限を超える場合
 */
bool ParsedTree::HasBoolNameAt(const std::string_view Name, const uint32_t Use) const {
	// 遮蔽の対象でない綴りの返戻
	if(!GetProfile().IsSpelling(Name, Core::Word::True) && !GetProfile().IsSpelling(Name, Core::Word::False)) return true;
	const BoolNameSource Source = GetBoolNameSource();
	if(Source == BoolNameSource::Macro) {
		const MacroDef Def = GetMacroDef(Name, Use);
		if(Def.State == MacroState::Defined) {
			// C で組込の真偽値と同じ値へ展開する定義の返戻
			return GetRules().DoesBoolNameNeedDecl && !Def.IsFunctionLike && Def.Alias == (
				GetProfile().IsSpelling(Name, Core::Word::True) ?
				GetRules().GetSpelling(Core::Word::IntTruth) :
				GetRules().GetSpelling(Core::Word::IntZero)
			);
		}
		// マクロ置換の無い予約語を使える事の返戻
		return !GetRules().DoesBoolNameNeedDecl && (Def.State == MacroState::Undefined || Def.State == MacroState::Cancelled);
	}
	if(Source == BoolNameSource::Shadow) {
		const std::vector<std::pair<uint32_t, uint32_t>> &Ranges = BoolShadows[GetProfile().IsSpelling(Name, Core::Word::True)];
		const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Found =
		std::upper_bound(Ranges.begin(), Ranges.end(), std::pair<uint32_t, uint32_t>(Use, UINT32_MAX));
		// 使用位置を含む遮蔽範囲が無い事の返戻
		return Found == Ranges.begin() || Use >= std::prev(Found)->second;
	}
	// 遮蔽出来ないリテラルを使える事の返戻
	return true;
}

/**
 * 真偽値名の束縛が変わり得る次の位置の取得関数
 * 前処理の世代・両方の名前の定義，又は遮蔽の開始・終了で区間を切る
 * 計算量：各記録列の長さの最大値 D に対し O(log(D + 1))，追加領域 O(1)
 * @param Use 今回判定した使用位置
 * @return 同じ束縛を使える半開区間の終端，後続の変更が無ければ UINT32_MAX
 */
uint32_t ParsedTree::GetBoolNameLimit(const uint32_t Use) const {
	const BoolNameSource Source = GetBoolNameSource();
	uint32_t Limit = UINT32_MAX;
	if(Source == BoolNameSource::Macro) {
		// 収集前は別の照会で表が作られ得る為，同じ位置だけを共有する
		if(!Syntax.HasMacroStates) return Use + (Use != UINT32_MAX);
		if(NameData) {
			const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Epoch = std::upper_bound(
				NameData->MacroEpochs.begin(),
				NameData->MacroEpochs.end(),
				Use,
				[](const uint32_t Pos, const std::pair<uint32_t, uint32_t> &Item) -> bool {
					// 使用位置より後に始まる世代かの返戻
					return Pos < Item.first;
				}
			);
			if(Epoch != NameData->MacroEpochs.end()) Limit = Epoch->first;
			for(const std::string_view Name : { GetRules().GetSpelling(Core::Word::False), GetRules().GetSpelling(Core::Word::True) }) {
				const decltype(NameData->MacroDefs)::const_iterator Found = NameData->MacroDefs.find(Name);
				if(Found == NameData->MacroDefs.end()) continue;
				const std::vector<MacroDef>::const_iterator Next = std::upper_bound(
					Found->second.begin(),
					Found->second.end(),
					Use,
					[](const uint32_t Pos, const MacroDef &Item) -> bool {
						// 使用位置より後に始まる定義かの返戻
						return Pos < Item.Start;
					}
				);
				if(Next != Found->second.end()) Limit = std::min(Limit, Next->Start);
			}
		}
	} else if(Source == BoolNameSource::Shadow) for(const std::vector<std::pair<uint32_t, uint32_t>> &Ranges : BoolShadows) {
		const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Next =
		std::upper_bound(Ranges.begin(), Ranges.end(), std::pair<uint32_t, uint32_t>(Use, UINT32_MAX));
		if(Next != Ranges.end()) Limit = std::min(Limit, Next->first);
		if(Next != Ranges.begin() && Use < std::prev(Next)->second) Limit = std::min(Limit, std::prev(Next)->second);
	}
	// 次の変化位置の返戻
	return Limit;
}

/**
 * 見出の本体の始まりと紛れる式の判定関数
 * 制御見出で本体の波括弧と誤読される式の包含判定関数
 * 実引数・配列・添字・文字列等の区切の中と，関数・クロージャの本体の中の物は紛れない（要素１つの括弧は外れ得る為に透過して見る）
 * 計算量：部分木のノード数 N に対し O(N)
 * @param Node 対象ノード
 * @return 紛れる式を持つなら true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool ParsedTree::HasBodyLikeExpr(const TSNode Node) const {
	// 入力の読取規則
	const ReadingRules &Reading = GetReading();
	// 本体の始まりと紛れる式の判定
	const auto IsBodyLike = [this, &Reading](const TSNode Target) -> bool {
		// 本体の始まりと紛れる式かの返戻
		return Reading.HasCompositeHeaderExpr && Is(Target, Core::Role::CompositeLiteral) &&
		Is(GetFieldChild(Target, Core::ChildRole::Type), Core::Role::CompositeTypeName) ||
		Reading.HasStructHeaderExpr && Is(Target, Core::Role::StructExpr) ||
		Reading.HasPrefixCallHeaderExpr && Is(Target, Core::Role::LambdaLiteral);
	};
	// 紛れる式其の物の返戻
	if(IsBodyLike(Node)) return true;
	// 子孫探索状態と Swift 括弧透過候補の準備
	bool IsFound = false;
	// Swift の前置の演算子の被演算子を包む括弧 (`!(x)`) は，演算子の呼出の実引数の並びと読まれても外れ得る為に透過
	std::vector<const void *> PrefixArgs;
	// 見出を包む前置の記録
	const auto NotePrefix = [&](const TSNode Call) -> void {
		if(const TSNode Args = GetSwiftPrefixArgs(Call); !ts_node_is_null(Args)) PrefixArgs.push_back(Args.id);
		return;
	};
	// 根自身の前置呼出の括弧透過候補への追加
	if(Reading.HasPrefixCallHeaderExpr) NotePrefix(Node);
	WalkChildrenCursor(
		Node,
		[&](const TSNode Child) -> bool {
			// 見付けた後に降りない事の返戻
			if(IsFound) return false;
			// 紛れる式の検出と探索打切
			if(IsBodyLike(Child)) {
				IsFound = true;
				// 見付けた為に降りない事の返戻
				return false;
			}
			// 子の型に応じた Swift 括弧候補の記録
			if(Reading.HasPrefixCallHeaderExpr && Is(Child, Core::Role::CallExpr)) NotePrefix(Child);
			// 区切トークンを持つ子の降下可否判定
			const TSNode First = ts_node_child(Child, 0);
			// 区切の括弧・引用符のトークンで始まるノード（要素１つの括弧を除く）の中へは降りない事の返戻
			return ts_node_is_null(First) || ts_node_is_named(First) || !IsTokenSpelling(First, Core::Role::BodyDelimiterLead) ||
			Is(Child, Core::Role::ValParens) || Reading.HasPrefixCallHeaderExpr && ts_node_named_child_count(Child) == 1 &&
			(Is(Child, Core::Role::TupleExpr) || std::find(PrefixArgs.begin(), PrefixArgs.end(), Child.id) != PrefixArgs.end());
		}
	);
	// 紛れる式を持つかの返戻
	return IsFound;
}

/**
 * Ruby の実引数の先頭の曖昧さの判定関数
 * 空白を挟んだ単項の `-`・単項の `+`・範囲演算子で始まる値は，前の語との二項式・範囲と読み得る（括弧の中の先頭も見る）
 * 計算量：実引数の先頭へ辿る深さ D，符号の密着判定の費用 Q に対し O(D + Q)
 * @param Lead 実引数の値
 * @return 先頭が曖昧なら true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
bool ParsedTree::HasAmbiguousRubyLead(TSNode Lead) const {
	// 実引数先頭の葉迄降下してテキストを取得
	while(!ts_node_is_null(Lead) && ts_node_child_count(Lead)) {
		// 空白を挟んで残る符号の返戻
		if(
			Is(Lead, Core::Role::UnaryVal) && GetView(ts_node_child(Lead, 0)) == GetRules().Operators.Minus &&
			!CanAttachRubySign(ts_node_named_child(Lead, 0))
		) return true;
		// 括弧を透過して最左子へ移動
		Lead = Is(Lead, Core::Role::GroupingParen) ? ts_node_named_child(Lead, 0) : ts_node_child(Lead, 0);
	}
	// 単項加算又は範囲演算子かの返戻
	return !ts_node_is_null(Lead) && (GetView(Lead).starts_with(GetRules().Operators.Plus) || Is(Lead, Core::Role::RangeOperator));
}

/**
 * トークンの間の改行で読みが変わるかの判定関数（改行を含むブロックコメントも改行と読む言語で，其のコメントを置き又は外す所の検査）
 * Go は名前・リテラル・閉括弧・後置の増減・`return` 等の後の改行に自動の `;` を補い，Swift も其処の改行で文を終える
 * Kotlin は改行の有無をコメントを読み飛ばして判定する為，コメントの中の改行で読みが変わらない
 * JS/TS は続くトークンを読める限り改行で文を終えず，制限された語（`return` / `yield` 等）の後と `=>`・後置の増減の前だけで読みが変わる
 * 計算量：境界間のテキスト長 L，葉数 F，初回の親子表の準備費用 R に対し O(L + log(F + 1) + R)
 * @param PrevEnd 前のトークンの終わり
 * @param NextStart 後のトークンの始まり
 * @return 改行で読みが変わるなら true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
bool ParsedTree::CanChangeReadingAt(const uint32_t PrevEnd, const uint32_t NextStart) const {
	// 対象文法の記法
	const Core::SyntaxRules &Rules = GetRules();
	const ReadingRules &Reading = GetReading();
	// 改行を読まない言語と，既に改行の後の位置の返戻
	if(!Reading.HasLineSensitiveBoundary || !PrevEnd || at(PrevEnd - 1) == '\n') return false;
	const std::string_view Code = GetCodeText(), Next = Code.substr(std::min<size_t>(NextStart, Code.size()));
	// 前に改行を禁じるトークン（矢印・後置の増減）の前の返戻
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
	const unsigned char Prev = at(PrevEnd - 1);
	if(Reading.ExprContinuationChars.find(Prev) != std::string_view::npos) {
		// 演算子・開括弧・区切の後は式が続き，後置の増減の後だけ式を終える事の返戻
		return PrevEnd > 1 && (Prev == '+' || Prev == '-') && at(PrevEnd - 2) == Prev;
	}
	// 語の字で終わる時だけ，木の世代毎に集めたトークン末尾の表で前のトークンを引く
	if(IsIdentifierChar(Prev)) {
		RecordNodeRelations();
		const NodeRelations &Relations = *Syntax.Relations;
		const std::vector<uint32_t>::const_iterator Found = std::lower_bound(
			Relations.LeafEnds.begin(),
			Relations.LeafEnds.end(),
			PrevEnd,
			[&Relations](const uint32_t Slot, const uint32_t End) -> bool {
				// 求める終端より前の葉かの返戻
				return Relations.Children[Slot].End < End;
			}
		);
		if(Found != Relations.LeafEnds.end()) {
			const ParsedChild &Leaf = Relations.Children[*Found];
			if(Leaf.End == PrevEnd && !Leaf.IsNamed) {
				const Core::RoleSet &Roles = GetProfile().GetKindOf(Leaf.Symbol).Roles;
				// 後続を要する語（`else` / `typeof` 等）の後の返戻
				if(Roles.test(static_cast<size_t>(Core::Role::ContinuingWord))) return false;
				// JS / TS は後に改行を挟めない語の後だけ読みが変わる事の返戻
				return !Reading.HasRestrictedLineWords || Roles.test(static_cast<size_t>(Core::Role::RestrictedLineWord));
			}
		}
	}
	// 名前・リテラル・閉括弧の後は，其処の改行で文を終える言語（Go, Swift）だけが読みを変える事の返戻
	return !Reading.HasRestrictedLineWords;
}

/**
 * 行末コメントを置く行末の位置の取得関数
 * 位置の後の最初の改行の行の最後のコードの後（行末の後続のコメントの前）を返す
 * 其処が文字列等のテキスト・JSX の要素の中なら其の終わる行へ，繋いで改行の消える隙間の中なら其の後の行へ移る（行の組方は整形で変わる為，原文のテキストの中の行末では決めない）
 * 計算量：前進するノード数 N，最大の祖先数 D，間隙数 G，本文長 L，ノード取得・コメントインデックス生成の費用 Q に対し O(N (D + log(G + 1)) + L + Q)
 * @param From 探し始める位置
 * @param Gaps 繋いで改行の消える隙間の範囲の列（始まりの順）
 * @param Following 返す位置の後に在る同じ行のコメントの格納先
 * @return 置く位置
 * @throws std::bad_alloc 降下経路又は後続コメントの並びの確保に失敗した場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
uint32_t ParsedTree::FindLineEndAfter(
	const uint32_t From,
	const std::span<const std::pair<uint32_t, uint32_t>> Gaps,
	std::vector<TSNode> &Following
) const {
	// 実行停止後のデータを除いたコードテキストへの参照
	const std::string_view Src = GetCodeText();
	Following.clear();
	// 行末候補は前方だけへ進め，後続コメントの除去は同じ候補内で後退する
	uint32_t LineEnd = static_cast<uint32_t>(std::min(Src.find('\n', From), Src.size()));
	for(uint32_t At = LineEnd;;) {
		if(
			const std::span<const std::pair<uint32_t, uint32_t>>::iterator Gap = std::upper_bound(
				Gaps.begin(),
				Gaps.end(),
				LineEnd,
				[](const uint32_t Val, const std::pair<uint32_t, uint32_t> &Range) -> bool {
					// 隙間の始まりとの比較の返戻
					return Val < Range.first;
				}
			); Gap != Gaps.begin() && LineEnd < (Gap - 1)->second
		) {
			Following.clear();
			LineEnd = static_cast<uint32_t>(std::min(Src.find('\n', (Gap - 1)->second), Src.size()));
			At = LineEnd;
			continue;
		}
		At = TextEdit::SkipSpaceLeft(Src, GetReading(), At);
		// ソースの先頭に達した位置の返戻
		if(!At) return At;
		const std::vector<TSNode> Path = GetAncestorsAt(At - 1);
		if(const TSNode Leaf = Path.back(); Is(Leaf, Core::Role::Comment)) {
			if(GetEnd(Leaf) > LineEnd) {
				Following.clear();
				LineEnd = static_cast<uint32_t>(std::min(Src.find('\n', GetEnd(Leaf)), Src.size()));
				At = LineEnd;
			} else {
				Following.push_back(Leaf);
				At = GetStart(Leaf);
			}
			continue;
		}
		TSNode Outer {};
		for(const TSNode Up : Path) {
			if(
				ts_node_is_null(Outer) && GetStart(Up) < At && LineEnd < GetEnd(Up) &&
				(Is(Up, Core::Role::LiteralInterior) || Is(Up, Core::Role::TagElement))
			) Outer = Up;
		}
		// 行末候補を跨がないトークンの後の位置の返戻
		if(ts_node_is_null(Outer)) return At;
		Following.clear();
		LineEnd = static_cast<uint32_t>(std::min(Src.find('\n', GetEnd(Outer)), Src.size()));
		At = LineEnd;
	}
}

/**
 * 行端の扱いが処理系で分かれる本文を持つ JSX 要素かの判定関数
 * 処理系は前後の子（タグ・式・要素）の間のテキストを１つの本文と読む為，子の間毎に判定する
 * 此の要素は子の並べ方を変えると何方かの処理系で表示が変わる為，整形せずに原文の並びで保つ
 * 計算量：子の数 C と本文総長 N に対し O(C + N)
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
			// 本文と文字参照はテキストで読む為，次の子へ進む事の返戻
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
 * コメントのノードとトークンの隙間に残ったコメントの範囲の収集関数（整形仕様 8.1 の空行の照合）
 * スキャナーがノードにせずトークンの間の隙間へ残した囲み・行コメント（Kotlin の呑まれたコメント等）も，隙間のテキストから拾う
 * 隙間は文法のコメントの記法の開始記号で始まる物だけとし，文字列等の内部のテキストを保つノードの中の隙間（テキストの一部）は読まない
 * 同じ木の世代ではコメントノード・隙間・祖先・後続兄弟を１度だけ収集し，照会する本体の範囲を絞る
 * 計算量：ノード数 V とテキスト長 N に対し初回 O(V + N)，コメント範囲数 C に対し以後 O(C log(C + 1))
 * @param BodyEnd PHP の本体の終わり（其の後の地の文の中の物は，パーサーがコメントと読んでも地の文の為に集めない）
 * @return 位置順の範囲（末尾の改行を除く）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
std::vector<std::pair<uint32_t, uint32_t>> ParsedTree::CollectCommentRanges(const uint32_t BodyEnd) const {
	// 構文木を持たないテキストの返戻
	if(!IsParsed()) return {};
	// 親子・フィールド・コメントの事実を同じカーソルの１度の訪問から得る
	RecordNodeRelations();
	const std::string_view Text = *this;
	// 本体の範囲だけを返戻し，PHP の地の文を同じ木の別のコメント照会に混ぜない
	std::vector<std::pair<uint32_t, uint32_t>> Ranges;
	for(const CommentFacts::Range &Range : Syntax.Comments->Ranges) if(Range.At < BodyEnd) {
		const uint32_t End =
		Range.IsGap && BodyEnd < Range.End ? static_cast<uint32_t>(Text.find_last_not_of(" \t\r\n", BodyEnd - 1) + 1) : Range.End;
		if(Range.Start < End) Ranges.emplace_back(Range.Start, End);
	}
	std::sort(Ranges.begin(), Ranges.end());
	Ranges.erase(std::unique(Ranges.begin(), Ranges.end()), Ranges.end());
	// 範囲の返戻
	return Ranges;
}

/**
 * 退避したコメントも含む入力座標の範囲の収集関数
 * 同じ読取で消したコメントの入力範囲と，解析用の本文に残るコメントの範囲を合成する
 * 計算量：残るコメント数 C，退避数 E，位置の逆マッピング費用 Q に対し O(CQ + (C + E) log(C + E + 1))，初回の収集費用は CollectCommentRanges と同じ
 * @param BodyEnd 解析用の本文の終端（其の後の地の文は集めない）
 * @return 準備と修復を戻した入力座標で開始順に並ぶコメント範囲
 * @throws std::bad_alloc 範囲又は位置表のメモリを確保出来ない場合
 * @throws std::length_error 範囲又は位置表の要素数上限を超える場合
 * @throws std::out_of_range 本文又は位置表の範囲外を参照した場合
 */
std::vector<std::pair<uint32_t, uint32_t>> ParsedTree::CollectInputCommentRanges(const uint32_t BodyEnd) const {
	// 解析用の本文の範囲を入力の座標へ戻す
	std::vector<std::pair<uint32_t, uint32_t>> Ranges = CollectCommentRanges(BodyEnd);
	for(auto &[Start, End] : Ranges) {
		Start = OffsetInInput(Start);
		End = OffsetInInput(End);
	}
	// 同じ入力の読取で退避した範囲は既に入力座標であり，編集を逆適用しない
	for(const EscapedComment &Comment : EscapedComments) if(Comment.Pos <= BodyEnd && Comment.InputRange) {
		Ranges.push_back(*Comment.InputRange);
	}
	std::sort(Ranges.begin(), Ranges.end());
	Ranges.erase(std::unique(Ranges.begin(), Ranges.end()), Ranges.end());
	// 入力の範囲の返戻
	return Ranges;
}

/**
 * 独立したコードとして整形した埋込の出力の取得関数（整形仕様 4.4）
 * 計算量：期待値 O(1)
 * @param RawText 埋込の中身のノード
 * @return 整形した出力（整形しなかった埋込と埋込の中身でないノードは nullptr）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::shared_ptr<const Core::OwnedRegion> ParsedTree::GetOwnedRegionOf(const TSNode RawText) const {
	const std::unordered_map<const void *, std::shared_ptr<const Core::OwnedRegion>>::const_iterator Found =
	OwnedRegions.find(RawText.id);
	// 整形した出力の返戻
	return Found == OwnedRegions.end() ? nullptr : Found->second;
}

/**
 * 独立したコードとして整形した埋込の出力の控関数（整形仕様 4.4，親の原文へ書き戻さず投影が所有領域とする）
 * 計算量：領域の移動構築の費用 Q に対し期待値 O(1 + Q)
 * @param RawText 埋込の中身のノード
 * @param Region 整形した出力と其の逐語の中から始まる行
 */
void ParsedTree::SetOwnedRegion(const TSNode RawText, Core::OwnedRegion &&Region) {
	// 独立したコードとして整形した埋込の出力の控関数（整形仕様 4.4，親の原文へ書き戻さず投影が所有領域とする）
	OwnedRegions.insert_or_assign(RawText.id, std::make_shared<const Core::OwnedRegion>(std::move(Region)));
	// 終了
	return;
}

/**
 * 使用位置で文字列化される仮引数を取得する関数
 * 定義の共有する辺を逆向きに辿り，文字列化から元の仮引数への到達を１度確定する
 * 参照した名前の次の変更で有効区間を切り，同じ区間では辺・本文・依存先を読み直さない
 * 計算量：初回の到達する名前数 M，ノード数 V，辺数 E と状態検索費用 Q に対し O(MQ + V + E + K)，共有時は区間数 K に対し O(log(K + 1))
 * @param List 実引数列
 * @return 使用位置の文字列化の対象（対象が無ければ nullptr）
 * @throws std::bad_alloc 定義・辺又は結果を所有するメモリを確保出来ない場合
 * @throws std::length_error ノード・辺又は結果の要素数が上限を超える場合
 * @throws FormatLimitExceeded 名前の収集又は未構築の木の解析が処理予算を超える場合
 * @throws std::out_of_range 照会するテキスト又はノードが範囲外の場合
 */
const ParsedTree::StringizingArgs *ParsedTree::GetStringizingArgs(const TSNode List) const {
	if(!GetReading().HasPreprocMacros || !(Is(List, Core::Role::DelimitedList) || Is(List, Core::Role::ParamContainer))) {
		// 前処理の実引数列でも，其れを型の構文へ読んだ仮引数列でもない場合の返戻
		return nullptr;
	}
	const TSNode Parent = GetParentOf(List);
	TSNode Callee = GetFieldChild(Parent, Core::ChildRole::FunctionField);
	// 抽象関数宣言子へ読まれた呼出も，型の名前に対応する元のマクロ名を使う
	if(ts_node_is_null(Callee) && Is(Parent, Core::Role::AbstractFunction)) {
		Callee = GetFieldChild(GetParentOf(Parent), Core::ChildRole::Type);
	}
	// 型の構文へ読まれた実引数列も，開括弧の直前の名前の束縛で判ずる
	uint32_t NameEnd = ts_node_is_null(Callee) ? GetStart(List) : GetEnd(Callee);
	while(NameEnd && (at(NameEnd - 1) == ' ' || at(NameEnd - 1) == '\t' || at(NameEnd - 1) == '\n' || at(NameEnd - 1) == '\r')) {
		ChargeNameWork();
		--NameEnd;
	}
	uint32_t NameStart = NameEnd;
	while(NameStart && IsIdentifierChar(at(NameStart - 1))) {
		ChargeNameWork();
		--NameStart;
	}
	// 呼出名を取得出来ない場合の返戻
	if(NameStart == NameEnd) return nullptr;
	const std::string_view Name = std::string_view(*this).substr(NameStart, NameEnd - NameStart);
	// 通常の関数又は変数に束縛された名前の返戻
	if(IsNonMacroName(Name, NameStart)) return nullptr;
	CollectMacroStates();
	const uint32_t Use = NameStart;
	std::vector<StringizingArgs> &Cached = NameData->Stringizing[StoreName(Name)];
	const std::vector<StringizingArgs>::iterator After = std::upper_bound(
		Cached.begin(),
		Cached.end(),
		Use,
		[](const uint32_t Position, const StringizingArgs &Entry) -> bool {
			// 使用位置より後の区間かの返戻
			return Position < Entry.Start;
		}
	);
	if(After != Cached.begin() && Use < std::prev(After)->Limit) {
		const StringizingArgs &Result = *std::prev(After);
		// 有効区間の共有結果の返戻
		return Result.Params.empty() && Result.VariadicFrom == UINT32_MAX ? nullptr : &Result;
	}
	StringizingArgs Result;
	Result.Start = Use;
	// 別名と条件枝の先に在る関数形式の定義を，同じ照会では名前毎に１度だけ解決する
	std::unordered_map<std::string_view, std::vector<const MacroBody *>> Targets;
	const auto Resolve = [&](const std::string_view Callee) -> const std::vector<const MacroBody *> & {
		const auto [Found, Added] = Targets.try_emplace(Callee);
		// 解決済の呼出先の返戻
		if(!Added) return Found->second;
		std::vector<std::string_view> Names { Callee };
		std::vector<const MacroBody *> Bodies;
		std::unordered_set<std::string_view> SeenNames;
		std::unordered_set<const MacroBody *> SeenBodies;
		std::vector<const MacroBody *> &Output = Found->second;
		while(!Names.empty() || !Bodies.empty()) {
			ChargeNameWork();
			if(!Names.empty()) {
				const std::string_view Current = Names.back();
				Names.pop_back();
				if(!SeenNames.insert(Current).second) continue;
				const std::unordered_map<std::string_view, std::vector<MacroDef>>::const_iterator State = NameData->MacroDefs.find(Current);
				if(State != NameData->MacroDefs.end()) {
					const std::vector<MacroDef>::const_iterator Next = std::upper_bound(
						State->second.begin(),
						State->second.end(),
						Use,
						[](const uint32_t Position, const MacroDef &Entry) -> bool {
							// 次に束縛を変える位置かの返戻
							return Position < Entry.Start;
						}
					);
					if(Next != State->second.end()) Result.Limit = std::min(Result.Limit, Next->Start);
				}
				const MacroDef Def = GetMacroDef(Current, Use);
				if(Def.State == MacroState::Cancelled) continue;
				if(Def.Body) Bodies.push_back(Def.Body.get());
				else if(GetProfile().IsSpelling(Current, Core::Word::StringizingAssertion)) Output.push_back(nullptr);
			} else {
				const MacroBody *const Body = Bodies.back();
				Bodies.pop_back();
				if(!SeenBodies.insert(Body).second) continue;
				if(Body->Alternatives[0] || Body->Alternatives[1]) {
					for(const std::shared_ptr<const MacroBody> &Alternative : Body->Alternatives) if(Alternative) {
						ChargeNameWork();
						Bodies.push_back(Alternative.get());
					}
				} else if(Body->IsFunctionLike) Output.push_back(Body);
				else if(!Body->Alias.empty()) Names.push_back(Body->Alias);
			}
		}
		// 呼出先の共有結果の返戻
		return Output;
	};
	const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Epoch = std::upper_bound(
		NameData->MacroEpochs.begin(),
		NameData->MacroEpochs.end(),
		Use,
		[](const uint32_t Position, const std::pair<uint32_t, uint32_t> &Entry) -> bool {
			// 先行の有効区間を終えるインクルード世代かの返戻
			return Position < Entry.first;
		}
	);
	if(Epoch != NameData->MacroEpochs.end()) Result.Limit = Epoch->first;
	const std::vector<const MacroBody *> &Roots = Resolve(Name);
	// 各定義のノードは１度だけ共有し，文字列化から元のトークンへ逆向きの辺を作る
	std::unordered_map<const MacroBody *, size_t> Offsets;
	std::vector<const MacroBody *> Pending;
	std::vector<std::vector<size_t>> Parents;
	std::vector<size_t> Stringized;
	const auto AddBody = [&](const MacroBody *const Body) -> size_t {
		const auto [Found, Added] = Offsets.try_emplace(Body, Parents.size());
		if(Added) {
			if(Body->ArgNodes > Parents.max_size() - Parents.size()) throw std::length_error("macro argument flow exceeds its node range");
			if(Core::StructureWorkAccount *const Work = GetNameWorkCounter()) Work->Charge(Body->ArgNodes);
			Parents.resize(Parents.size() + Body->ArgNodes);
			Pending.push_back(Body);
			for(const uint32_t Param : Body->Stringized) {
				ChargeNameWork();
				Stringized.push_back(Found->second + Param);
			}
		}
		// 同じ定義の共有範囲の返戻
		return Found->second;
	};
	for(const MacroBody *const Body : Roots) {
		if(Body) AddBody(Body);
		else Result.VariadicFrom = 0;
	}
	while(!Pending.empty()) {
		ChargeNameWork();
		const MacroBody *const Body = Pending.back();
		Pending.pop_back();
		const size_t Offset = Offsets.at(Body);
		for(const MacroArgFlow &Flow : Body->ArgFlows) {
			ChargeNameWork();
			if(Flow.Callee.empty()) Parents[Offset + Flow.To].push_back(Offset + Flow.From);
			else for(const MacroBody *const Target : Resolve(Flow.Callee)) {
				ChargeNameWork();
				if(!Target) Stringized.push_back(Offset + Flow.From);
				else {
					const uint32_t Param = Target->VariadicParam == UINT32_MAX ? Flow.To : std::min(Flow.To, Target->VariadicParam);
					if(Param < Target->ParamCount) Parents[AddBody(Target) + Param].push_back(Offset + Flow.From);
				}
			}
		}
	}
	std::vector<uint8_t> Observed(Parents.size(), 0);
	while(!Stringized.empty()) {
		ChargeNameWork();
		const size_t Node = Stringized.back();
		Stringized.pop_back();
		if(Observed[Node]) continue;
		Observed[Node] = 1;
		for(const size_t Parent : Parents[Node]) {
			ChargeNameWork();
			Stringized.push_back(Parent);
		}
	}
	uint32_t ParamCount = 0;
	for(const MacroBody *const Body : Roots) if(Body) {
		ChargeNameWork();
		ParamCount = std::max(ParamCount, Body->ParamCount);
	}
	if(Core::StructureWorkAccount *const Work = GetNameWorkCounter()) Work->Charge(ParamCount);
	std::vector<uint8_t> Selected(ParamCount, 0);
	for(const MacroBody *const Body : Roots) if(Body) {
		const size_t Offset = Offsets.at(Body);
		for(uint32_t Param = 0; Param < Body->ParamCount; ++Param) {
			ChargeNameWork();
			if(!Observed[Offset + Param]) continue;
			if(Param == Body->VariadicParam) Result.VariadicFrom = std::min(Result.VariadicFrom, Param);
			else Selected[Param] = 1;
		}
	}
	for(uint32_t Param = 0; Param < ParamCount; ++Param) {
		ChargeNameWork();
		if(Selected[Param]) Result.Params.push_back(Param);
	}
	// 有効区間の順に結果を所有し，依存先の再照合を共有時へ持ち込まない
	const std::vector<StringizingArgs>::iterator Stored = Cached.insert(After, std::move(Result));
	// 有効区間に所有した対象の返戻
	return Stored->Params.empty() && Stored->VariadicFrom == UINT32_MAX ? nullptr : &*Stored;
}

/**
 * 範囲に空行が在るかの判定関数
 * コメントと宣言の間の空行を位置表で共有し，連続するコメント毎に残りの原文を走査しない
 * 計算量：空行数 N に対し照会毎に O(log(N + 1))（位置表の初回構築は GetCommentSpans の費用）
 * @param Begin 範囲の始まり（含む）
 * @param End 範囲の終わり（含まない）
 * @return 範囲内の２つの改行の間が空白だけなら true
 * @throws std::bad_alloc 位置表のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
bool ParsedTree::HasBlankLineBetween(const uint32_t Begin, const uint32_t End) const {
	// コメント範囲と空行の表は，並列収集前に同じ段で確定
	GetCommentSpans();
	const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Found =
	std::lower_bound(Syntax.BlankLines.begin(), Syntax.BlankLines.end(), std::pair<uint32_t, uint32_t>{ Begin, 0 });
	// 範囲の中に完結する空行の返戻
	return Found != Syntax.BlankLines.end() && Found->second < End;
}

/**
 * 直前のコメントを取り込む Go の最上位の宣言かの判定関数
 * godoc は直前のコメントを宣言のドキュメントに，cgo は `import "C"` の直前のコメントを前文に読む
 * 計算量：コメントの長さ L と構文の照会費用 Q に対し O(L + Q)
 * @param Node 判定するノード
 * @return ドキュメントを持つ宣言か `import "C"` なら true
 */
bool ParsedTree::CanAbsorbGoComment(const TSNode Node) const {
	// ドキュメントを持つ宣言の返戻
	if(Is(Node, Core::Role::DocumentedTopDecl)) return true;
	// インポートの宣言でない事の返戻
	if(!Is(Node, Core::Role::ImportDeclNode)) return false;
	const TSNode Spec = ts_node_named_child(Node, 0);
	const TSNode Path = ts_node_is_null(Spec) ? TSNode{} : GetFieldChild(Spec, Core::ChildRole::Path);
	const std::string_view ForeignPackage = GetReading().ImportedForeignPackage;
	// 取り込むパッケージが cgo の `"C"` かの返戻
	return !ForeignPackage.empty() && !ts_node_is_null(Path) && GetView(Path) == ForeignPackage;
}

/**
 * 修復の編集を適用した本文の差替関数
 * 本文を全面差替えした時と同じく古い位置の記録を捨て，編集後の位置へ移した解析用の区切・囲み・トークンと編集の組を残す
 * 計算量：エスケープコメント数 C，編集区間数 E，本文の差替・編集履歴の格納費用 Q に対し O(C log(E + 1) + Q)
 * @param NewSrc 編集を適用した本文
 * @param Breaks 編集後の本文に対応する解析用の区切の位置
 * @param Groups 編集後の本文に対応する解析用の囲みの位置の組
 * @param Restorations 編集後の本文に対応するトークンのデコードデータ
 * @param Batch 適用した編集の組（位置順）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void ParsedTree::AssignEdited(
	std::string &&NewSrc,
	std::vector<uint32_t> &&Breaks,
	std::vector<std::pair<uint32_t, uint32_t>> &&Groups,
	std::vector<LexemeRestoration> &&Restorations,
	std::vector<AppliedEdit> &&Batch
) {
	// 旧編集履歴と退避コメントの保存
	AppliedEditBatches Batches = std::move(EditBatches);
	std::vector<AppliedEdit> Preparation = std::move(PreparationEdits);
	std::vector<EscapedComment> Escaped = std::move(EscapedComments);
	Assign(std::move(NewSrc));
	PreparationEdits = std::move(Preparation);
	InsertedBreaks = std::move(Breaks);
	SyntheticGroups = std::move(Groups);
	// 退避したコメントの位置の編集後の位置への移動（置き換えた範囲の中の物は置き換えたテキストの始まり）
	for(EscapedComment &Comment : Escaped) {
		const std::vector<AppliedEdit>::const_iterator After = std::upper_bound(
			Batch.begin(),
			Batch.end(),
			Comment.Pos,
			[](const uint32_t Pos, const AppliedEdit &Edit) -> bool {
				// 適用前の始まりとの比較の返戻
				return Pos < Edit.Start;
			}
		);
		if(After == Batch.begin()) continue;
		const AppliedEdit &Edit = *std::prev(After);
		Comment.Pos = Comment.Pos < Edit.End ? Edit.NewStart : Comment.Pos + Edit.NewEnd - Edit.End;
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
 * 受理判定で確定した欠落本体の有無の取得関数
 * 計算量：O(1)
 * @return 幅０の本体を検出した場合 true
 */
bool ParsedTree::HasEmptyBlock() const {
	// 記録済の欠落本体の有無の返戻
	return HasMissingBody;
}

/**
 * 解析用に補った文の区切の位置の取得関数
 * 計算量：O(1)
 * @return 現在の本文に対応する位置の並び
 */
const std::vector<uint32_t> &ParsedTree::GetInsertedBreaks() const {
	// 解析用の位置の返戻
	return InsertedBreaks;
}

/**
 * 解析用に挿入した囲みの位置の組の取得関数
 * 計算量：O(1)
 * @return 現在の本文に対応する開きと閉じの位置の組
 */
const std::vector<std::pair<uint32_t, uint32_t>> &ParsedTree::GetSyntheticGroups() const {
	// 解析用の括弧の位置の組の返戻
	return SyntheticGroups;
}

/**
 * 解析用に置き換えたトークンのデコードデータの取得関数
 * 計算量：O(1)
 * @return 現在の本文に対応する置換後の範囲と原文のテキスト
 */
const std::vector<LexemeRestoration> &ParsedTree::GetLexemeRestorations() const {
	// トークンのデコードデータの返戻
	return LexemeRestorations;
}

/**
 * コード行の比較でデコードする状態を持つかの判定関数
 * 復元するトークン，解析用の括弧又は本来の字へ戻すマーカーが無ければ，断片のデコードは本文を変更しない
 * 計算量：O(1)
 * @return コード行の断片のデコードが本文を変更し得る状態なら true
 */
bool ParsedTree::HasCodeTextDecoding() const {
	// 復元するトークンを持つ事の返戻
	if(!LexemeRestorations.empty()) return true;
	const ReadingRules &Reading = GetReading();
	// 解析用の括弧又は本来の字へ戻すマーカーを持つ事の返戻
	return !Reading.TrailingDataMarker.empty() && !PreparationEdits.empty() ||
	AnalysisMark && (Reading.ShouldMaskEscapedBackslashes || GetRules().HasLineDirectives);
}

/**
 * 範囲に対応するトークンのデコードデータの取得関数
 * 計算量：デコードデータの数 R に対し O(log R)
 * @param Start 解析用の本文に於けるトークンの始まり
 * @param End 解析用の本文に於けるトークンの終わり
 * @return 同じ範囲のトークンのデコードデータ（無ければ nullptr）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
const LexemeRestoration *ParsedTree::GetRestorationAt(const uint32_t Start, const uint32_t End) const {
	const std::vector<LexemeRestoration>::const_iterator Found =
	std::ranges::lower_bound(LexemeRestorations, Start, std::less<uint32_t>(), &LexemeRestoration::Start);
	// 同じ範囲のデコードデータの返戻
	return Found != LexemeRestorations.end() && Found->Start == Start && Found->End == End && Found->HasKindRestoration ?
	&*Found :
	nullptr;
}

/**
 * 解析用に置き換えたトークンの記録関数
 * 計算量：デコードデータ数 R に対し O(R)
 * @param Start 置換後のトークンの始まり
 * @param End 置換後のトークンの終わり
 * @param Original 原文のテキスト
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void ParsedTree::AddLexemeRestoration(const uint32_t Start, const uint32_t End, std::string Original) {
	// 前進する走査位置
	const std::vector<LexemeRestoration>::const_iterator Pos =
	std::ranges::upper_bound(LexemeRestorations, Start, std::less<uint32_t>(), &LexemeRestoration::Start);
	LexemeRestorations.insert(Pos, { Start, End, std::move(Original) });
	// 終了
	return;
}

/**
 * 定義のトークンから直接の性質と転送関係を収集する関数
 * 本体は１度だけトークン化し，参照先の性質は使用位置で解決する
 * 計算量：定義のトークン数 N とテキスト長 B に対し O(N + B)
 * @param Node マクロの定義
 * @param Tokens 置換本体のトークン列
 * @return 定義に属する性質と参照先
 * @throws std::bad_alloc 定義の記録用のメモリを確保出来ない場合
 * @throws std::length_error 記録の要素数上限を超える場合
 */
std::shared_ptr<const ParsedTree::MacroBody> ParsedTree::ReadMacroBody(
	const TSNode Node,
	const std::span<const MacroToken> Tokens
) const {
	std::shared_ptr<MacroBody> Body = std::make_shared<MacroBody>();
	const bool IsFunctionLike = Is(Node, Core::Role::PreprocFunctionDef);
	Body->IsFunctionLike = IsFunctionLike;
	std::unordered_map<std::string_view, uint32_t> ParamIndices;
	std::unordered_set<std::string_view> Params;
	if(IsFunctionLike) {
		Params.emplace(GetReading().VariadicParam);
		bool HasParam = false;
		for(const MacroToken &Token : TokenizeMacroBody(GetView(GetFieldChild(Node, Core::ChildRole::ParamsField)))) {
			ChargeNameWork();
			Body->Signature.append(Token.Text).push_back('\0');
			if(Token.IsIdentifier) {
				Params.emplace(Token.Text);
				ParamIndices.emplace(Token.Text, Body->ParamCount++);
				HasParam = true;
			} else if(Token.Text == ",") HasParam = false;
			else if(Token.Text == "." && Body->VariadicParam == UINT32_MAX) {
				Body->VariadicParam = HasParam ? Body->ParamCount - 1 : Body->ParamCount++;
				ParamIndices.emplace(GetReading().VariadicParam, Body->VariadicParam);
			}
		}
	}
	Body->ArgNodes = Body->ParamCount;
	if(Core::StructureWorkAccount *const Work = GetNameWorkCounter()) Work->Charge(Body->ParamCount);
	std::vector<uint8_t> DirectStringized(Body->ParamCount, 0);
	Body->Signature.push_back('\0');
	for(const MacroToken &Token : Tokens) Body->Signature.append(Token.Text).push_back('\0');
	// 仮引数のトークンは直属の実引数へ，入子の実引数は外側へ辺で渡し，子孫の仮引数集合を複写しない
	struct CallFrame {
		std::string_view Name; // 呼出名（ラッパーなら空）
		uint32_t Argument; // 現在の実引数の番号
		uint32_t ArgNode; // 現在の実引数のノード
		uint32_t ParentArg; // 入子を含む外側の実引数（無ければ UINT32_MAX）
		bool HasParam; // 此の呼出で仮引数のトークンを使うか
	};
	std::vector<CallFrame> Calls;
	for(size_t Pos = 0; Pos < Tokens.size(); ++Pos) {
		ChargeNameWork();
		const MacroToken &Token = Tokens[Pos];
		if(Token.IsOpaque) continue;
		if(
			GetProfile().IsSpelling(Token.Text, Core::Word::MacroStringizing) && Pos + 1 < Tokens.size() && Tokens[Pos + 1].IsIdentifier
		) {
			const std::unordered_map<std::string_view, uint32_t>::const_iterator Found = ParamIndices.find(Tokens[Pos + 1].Text);
			if(Found != ParamIndices.end()) DirectStringized[Found->second] = 1;
		}
		if(Token.IsIdentifier && !Calls.empty()) {
			const std::unordered_map<std::string_view, uint32_t>::const_iterator Found = ParamIndices.find(Token.Text);
			if(Found != ParamIndices.end()) {
				Body->ArgFlows.push_back({ Found->second, Calls.back().ArgNode, {} });
				Calls.back().HasParam = true;
			}
		}
		if(Token.Text == "(") {
			const std::string_view Name = Pos && Tokens[Pos - 1].IsIdentifier && !Params.contains(Tokens[Pos - 1].Text) ?
			StoreName(Tokens[Pos - 1].Text) :
			std::string_view();
			const uint32_t Parent = Calls.empty() ? UINT32_MAX : Calls.back().ArgNode;
			if(Body->ArgNodes == UINT32_MAX) throw std::length_error("macro argument flow exceeds its node range");
			Calls.push_back({ Name, 0, Body->ArgNodes++, Parent, false });
		} else if((Token.Text == "," || Token.Text == ")") && !Calls.empty()) {
			const CallFrame Call = Calls.back();
			if(Token.Text == ")" && Call.HasParam && !Call.Name.empty()) {
				Body->Refs.emplace_back(Call.Name, static_cast<uint8_t>(MacroProp::Stringizing));
			}
			if(!Call.Name.empty()) Body->ArgFlows.push_back({ Call.ArgNode, Call.Argument, Call.Name });
			if(Call.ParentArg != UINT32_MAX) Body->ArgFlows.push_back({ Call.ArgNode, Call.ParentArg, {} });
			if(Token.Text == ")") {
				Calls.pop_back();
				if(Call.HasParam && !Calls.empty()) Calls.back().HasParam = true;
			} else {
				if(Body->ArgNodes == UINT32_MAX) throw std::length_error("macro argument flow exceeds its node range");
				++Calls.back().Argument;
				Calls.back().ArgNode = Body->ArgNodes++;
			}
		}
	}
	// 閉じて居ない置換本体も内側から確定し，仮引数の使用を外側へ渡す
	while(!Calls.empty()) {
		ChargeNameWork();
		const CallFrame Call = Calls.back();
		Calls.pop_back();
		if(Call.HasParam && !Call.Name.empty()) Body->Refs.emplace_back(Call.Name, static_cast<uint8_t>(MacroProp::Stringizing));
		if(!Call.Name.empty()) Body->ArgFlows.push_back({ Call.ArgNode, Call.Argument, Call.Name });
		if(Call.ParentArg != UINT32_MAX) Body->ArgFlows.push_back({ Call.ArgNode, Call.ParentArg, {} });
		if(Call.HasParam && !Calls.empty()) Calls.back().HasParam = true;
	}
	for(uint32_t Param = 0; Param < Body->ParamCount; ++Param) {
		ChargeNameWork();
		if(DirectStringized[Param]) Body->Stringized.push_back(Param);
	}
	const uint8_t Stmt = static_cast<uint8_t>(MacroProp::Stmt), Declarator = static_cast<uint8_t>(MacroProp::Declarator);
	const uint8_t Expr = static_cast<uint8_t>(MacroProp::Expr), Stringizing = static_cast<uint8_t>(MacroProp::Stringizing);
	if(
		!Tokens.empty() && Tokens.front().IsIdentifier &&
		std::ranges::find(GetReading().StmtWords, Tokens.front().Text) != GetReading().StmtWords.end()
	) Body->Props |= Stmt;
	int Depth = 0;
	for(const MacroToken &Token : Tokens) {
		if(Token.IsOpaque) continue;
		if(Token.IsIdentifier) {
			if(!Params.contains(Token.Text)) {
				Body->Refs.emplace_back(StoreName(Token.Text), static_cast<uint8_t>(Stmt | Declarator | (!Depth ? Expr : 0)));
			}
		} else {
			if(Token.Text == ";" || Token.Text == "{") Body->Props |= Stmt;
			if(Token.Text == "*" || Token.Text == "&" || Token.Text == "[" || Token.Text == "(") Body->Props |= Declarator;
			if(Token.Text == "(" || Token.Text == "[") ++Depth;
			else if(Token.Text == ")" || Token.Text == "]") --Depth;
			else if(
				!Depth && Token.Text.size() == 1 && std::string_view("+-*/%<>=&|^!?:,~").find(Token.Text.front()) != std::string_view::npos
			) Body->Props |= Expr;
		}
	}
	if(Tokens.size() == 1 && Tokens.front().IsIdentifier && !IsFunctionLike) {
		Body->Alias = StoreName(Tokens.front().Text);
		Body->Refs.emplace_back(Body->Alias, Stringizing);
	}
	if(IsFunctionLike && !Body->Stringized.empty()) Body->Props |= Stringizing;
	// 定義の記録の返戻
	return Body;
}

/**
 * 到達し得る条件枝の定義を合流する関数
 * 異なる本体は不確定な状態とし，何方かの枝で必要な保護の性質を共有した記録へ残す
 * 計算量：置換トークンの記録の長さ N に対し O(N)
 * @param Other 合流する枝の定義
 * @param Joined 合流先
 * @throws std::bad_alloc 合流の記録のメモリを確保出来ない場合
 */
void ParsedTree::JoinMacroDef(const MacroDef &Other, MacroDef &Joined) {
	// 呼出の置換状態はインクルードの世代から独立して合流
	if(Joined.InputState != Other.InputState) Joined.InputState = MacroState::Unknown;
	// 同じ状態と置換トークンを持つ定義の合流の終了
	if(
		Joined.State == Other.State && Joined.Alias == Other.Alias && Joined.IsFunctionLike == Other.IsFunctionLike &&
		Joined.IsVersionedBool == Other.IsVersionedBool && (
			Joined.Body == Other.Body ||
			Joined.Body && Other.Body && !Joined.Body->Alternatives[0] && !Joined.Body->Alternatives[1] && !Other.Body->Alternatives[0] &&
			!Other.Body->Alternatives[1] && Joined.Body->Signature == Other.Body->Signature
		)
	) return;
	Joined.State = MacroState::Unknown;
	if(!Joined.Body) Joined.Body = Other.Body;
	else if(Other.Body && Joined.Body != Other.Body) {
		std::shared_ptr<MacroBody> Body = std::make_shared<MacroBody>();
		Body->Alternatives = { Joined.Body, Other.Body };
		Joined.Body = std::move(Body);
	}
	// 合流の終了
	return;
}

/**
 * 条件枝毎の前処理状態の収集関数
 * 各枝の変更名だけを戻し，合流では到達する枝の一致を取る
 * 通常の宣言は此の状態を変更しない
 * 計算量：ノード数 V，ディレクティブのテキストの総長 B，ディレクティブ数 M，条件のネストの深さ D に対し O(V + B + M D log(M + 1))
 * @throws std::bad_alloc 状態と走査列を確保出来ない場合
 * @throws std::length_error 状態又は走査列が上限を超える場合
 */
void ParsedTree::CollectMacroStates() const {
	// 収集済又は木が無い場合の終了
	if(Syntax.HasMacroStates || !IsParsed()) return;
	// 未反映編集の再解析は，世代の表を確保する前に済ませる
	const TSNode Root = GetRoot();
	NameData = std::make_unique<NameTables>();
	RecordNodeRelations();
	if(!Syntax.Relations->HasMacroCandidates) {
		Syntax.HasMacroStates = true;
		// マクロの状態を変えるノードが無い木の終了
		return;
	}
	struct Vote {
		MacroDef Before;
		MacroDef Joined;
		size_t Count = 0;
	};
	struct Branch {
		std::unordered_map<std::string_view, Vote> Names;
		std::unordered_set<std::string_view> Changed;
		uint32_t Epoch;
		uint32_t Start;
		bool HasPriorBoolHeader; // 分岐前の標準ヘッダの読取可能性
		bool HasBoolHeader = false; // 到達し得る枝での標準ヘッダの読取可能性
		size_t Count = 0;
		bool CanContinue = true;
		bool IsReachable = true;
		bool HasInclude = false;
	};
	struct Visit {
		TSNode Node;
		uint8_t Phase;
	};
	std::vector<Branch> Branches;
	std::vector<Visit> Pending { { Root, 0 } };
	uint32_t Epoch = 0;
	bool HasBoolHeader = false;
	const auto Record = [&](const std::string_view Name, MacroDef Def, const uint32_t Pos) -> void {
		for(Branch &Frame : Branches) {
			if(!Frame.Names.contains(Name)) {
				const MacroDef Before = GetMacroDef(Name, Frame.Start);
				Frame.Names.emplace(Name, Vote{ Before, Before, 0 });
			}
			Frame.Changed.insert(Name);
		}
		Def.Start = Pos;
		Def.Epoch = Epoch;
		NameData->MacroDefs[Name].push_back(std::move(Def));
	};
	// 外側の全ての条件枝が到達し得るかの判定（ディレクティブの位置だけで照会する）
	const auto IsReachable = [&Branches]() -> bool {
		// 判定結果の返戻
		return std::ranges::all_of(
			Branches,
			[](const Branch &Frame) -> bool {
				// 判定結果の返戻
				return Frame.IsReachable;
			}
		);
	};
	// 記録時に確定した部分木のフラグを読み，状態を変えないコードの降下を省く
	const auto HasMacroDescendant = [this](const ParsedChild &Child) -> bool {
		// 同じ子の列内の格納先に対応するフラグの返戻
		return Syntax.Relations->MacroSubtrees[static_cast<size_t>(&Child - Syntax.Relations->Children.data())];
	};
	while(!Pending.empty()) {
		const Visit Item = Pending.back();
		Pending.pop_back();
		const TSNode Node = Item.Node;
		++Syntax.Relations->MacroWork;
		const uint32_t Start = GetStart(Node), End = GetEnd(Node);
		if(Item.Phase == 2) {
			Branch &Frame = Branches.back();
			const TSNode Alt = GetFieldChild(Node, Core::ChildRole::Alternative);
			const uint32_t Pos = ts_node_is_null(Alt) ? End : GetStart(Alt);
			if(Frame.IsReachable) ++Frame.Count;
			Frame.HasInclude = Frame.HasInclude || Frame.IsReachable && Epoch != Frame.Epoch;
			Frame.HasBoolHeader = Frame.HasBoolHeader || Frame.IsReachable && HasBoolHeader;
			for(const std::string_view Name : Frame.Changed) {
				Vote &Val = Frame.Names.at(Name);
				const MacroDef Current = GetMacroDef(Name, Pos);
				if(Frame.IsReachable) {
					if(!Val.Count) Val.Joined = Current;
					else JoinMacroDef(Current, Val.Joined);
					++Val.Count;
				}
				MacroDef Before = Val.Before;
				Before.Start = Pos;
				Before.Epoch = Frame.Epoch;
				NameData->MacroDefs[Name].push_back(std::move(Before));
			}
			Frame.Changed.clear();
			Epoch = Frame.Epoch;
			HasBoolHeader = Frame.HasPriorBoolHeader;
			NameData->MacroEpochs.emplace_back(Pos, Epoch);
			continue;
		}
		if(Item.Phase == 3) {
			Branch Frame = std::move(Branches.back());
			Branches.pop_back();
			if(Frame.CanContinue) ++Frame.Count;
			Epoch = Frame.HasInclude ? End : Frame.Epoch;
			HasBoolHeader = Frame.HasPriorBoolHeader || Frame.HasBoolHeader;
			NameData->MacroEpochs.emplace_back(End, Epoch);
			for(auto &[Name, Val] : Frame.Names) {
				if(!Val.Count) Val.Joined = Val.Before;
				if(Val.Count < Frame.Count) {
					JoinMacroDef(Val.Before, Val.Joined);
					if(Frame.HasInclude) Val.Joined.State = MacroState::Unknown;
				}
				Record(Name, std::move(Val.Joined), End);
			}
			continue;
		}
		if(Item.Phase == 1) {
			Branch &Frame = Branches.back();
			TSNode Cond = GetFieldChild(Node, Core::ChildRole::Cond);
			if(ts_node_is_null(Cond)) Cond = GetFieldChild(Node, Core::ChildRole::Name);
			const std::string_view Directive = GetView(ts_node_child(Node, 0));
			const std::string_view Text = GetView(Cond);
			const std::vector<MacroToken> Tokens = TokenizeMacroBody(Text);
			std::optional<bool> IsTrue;
			if(
				GetProfile().IsSpelling(Directive, Core::Word::PreprocAlternateBranch) || GetProfile().IsSpelling(Text, Core::Word::IntTruth)
			) IsTrue = true;
			else if(GetProfile().IsSpelling(Text, Core::Word::IntZero)) IsTrue = false;
			else if(
				GetProfile().IsSpelling(Directive, Core::Word::PreprocDefinedBranch) ||
				GetProfile().IsSpelling(Directive, Core::Word::PreprocUndefinedBranch)
			) {
				const MacroDef Def = GetMacroDef(Text, Start);
				// C23 の予約語と C17 以前の標準マクロを，定義の有無で同一視しない
				if(Def.State != MacroState::Unknown && !Def.IsVersionedBool) {
					IsTrue = Def.State == MacroState::Defined == GetProfile().IsSpelling(Directive, Core::Word::PreprocDefinedBranch);
				}
			} else {
				size_t At = 0;
				bool IsNegated = false;
				while(At < Tokens.size() && (Tokens[At].Text == "!" || Tokens[At].Text == "(")) {
					if(Tokens[At].Text == "!") IsNegated ^= true;
					++At;
				}
				const bool IsDefined = At < Tokens.size() && GetProfile().IsSpelling(Tokens[At].Text, Core::Word::PreprocDefined);
				if(IsDefined) {
					++At;
					if(At < Tokens.size() && Tokens[At].Text == "(") ++At;
				}
				if(At < Tokens.size()) {
					const MacroToken Token = Tokens[At++];
					while(At < Tokens.size() && Tokens[At].Text == ")") ++At;
					if(At == Tokens.size()) {
						if(GetProfile().IsSpelling(Token.Text, Core::Word::IntZero) || GetProfile().IsSpelling(Token.Text, Core::Word::IntTruth)) {
							IsTrue = GetProfile().IsSpelling(Token.Text, Core::Word::IntTruth);
						} else if(Token.IsIdentifier) {
							const MacroDef Def = GetMacroDef(Token.Text, Start);
							if(Def.State != MacroState::Unknown && (!IsDefined || !Def.IsVersionedBool)) {
								if(IsDefined) IsTrue = Def.State == MacroState::Defined;
								else if(Def.State != MacroState::Defined) IsTrue = false;
								else if(GetProfile().IsSpelling(Def.Alias, Core::Word::IntZero) || GetProfile().IsSpelling(Def.Alias, Core::Word::IntTruth)) {
									IsTrue = GetProfile().IsSpelling(Def.Alias, Core::Word::IntTruth);
								}
							}
						}
					}
				}
				if(IsTrue && IsNegated) IsTrue = !*IsTrue;
			}
			Frame.IsReachable = Frame.CanContinue && IsTrue != false;
			Frame.CanContinue = Frame.CanContinue && IsTrue != true;
			Pending.push_back({ Node, 2 });
			const TSNode Alt = GetFieldChild(Node, Core::ChildRole::Alternative);
			const std::span<const ParsedChild> Children = GetChildrenOf(Node);
			for(size_t Idx = Children.size(); Idx; --Idx) {
				const TSNode Child = Children[Idx - 1].Node;
				if(HasMacroDescendant(Children[Idx - 1]) && ts_node_is_named(Child) && !ts_node_eq(Child, Alt) && !ts_node_eq(Child, Cond)) {
					Pending.push_back({ Child, 0 });
				}
			}
			continue;
		}
		if(Is(Node, Core::Role::PreprocConditional)) {
			Branches.push_back({ {}, {}, Epoch, Start, HasBoolHeader });
			Pending.push_back({ Node, 3 });
			std::vector<TSNode> Arms;
			for(TSNode Arm = Node; !ts_node_is_null(Arm); Arm = GetFieldChild(Arm, Core::ChildRole::Alternative)) Arms.push_back(Arm);
			for(std::vector<TSNode>::const_reverse_iterator It = Arms.crbegin(); It != Arms.crend(); ++It) Pending.push_back({ *It, 1 });
			continue;
		}
		if(Is(Node, Core::Role::PreprocInclude)) {
			if(IsReachable()) {
				const bool IsBoolHeader =
				!GetReading().BoolNameHeader.empty() && GetView(GetFieldChild(Node, Core::ChildRole::Path)) == GetReading().BoolNameHeader;
				MacroDef True { Start, Epoch, MacroState::Defined, GetRules().GetSpelling(Core::Word::IntTruth), false, {}, true };
				MacroDef False { Start, Epoch, MacroState::Defined, GetRules().GetSpelling(Core::Word::IntZero), false, {}, true };
				// 再インクルードで，既に取り消した名前を復活させない（インクルードの前の定義を引き継ぐ）
				if(IsBoolHeader && (HasBoolHeader || !GetRules().DoesBoolNameNeedDecl)) {
					True = GetMacroDef(GetRules().GetSpelling(Core::Word::True), Start);
					False = GetMacroDef(GetRules().GetSpelling(Core::Word::False), Start);
				}
				// ディレクティブの効果は其の終わりから及ぼす（同じ位置から始まる親のノードの照会を，前に置いたコメントの有無で変えない）
				Epoch = End;
				NameData->MacroEpochs.emplace_back(End, Epoch);
				if(IsBoolHeader) {
					True.InputState = GetMacroDef(GetRules().GetSpelling(Core::Word::True), Start).InputState;
					False.InputState = GetMacroDef(GetRules().GetSpelling(Core::Word::False), Start).InputState;
					Record(GetRules().GetSpelling(Core::Word::True), std::move(True), End);
					Record(GetRules().GetSpelling(Core::Word::False), std::move(False), End);
				}
				// 未読のインクルードも既に標準ヘッダを取り込んだ可能性を持つ
				HasBoolHeader = true;
			}
			continue;
		}
		const bool IsDef = Is(Node, Core::Role::MacroDef);
		const bool IsUndef = Is(Node, Core::Role::PreprocCall) &&
		GetView(GetFieldChild(Node, Core::ChildRole::DirectiveField)) == GetReading().UndefinitionDirective;
		if(IsDef || IsUndef) {
			if(!IsReachable()) continue;
			const std::vector<MacroToken> Names =
			TokenizeMacroBody(GetView(GetFieldChild(Node, IsDef ? Core::ChildRole::Name : Core::ChildRole::ArgField)));
			if(Names.size() != 1 || !Names.front().IsIdentifier) continue;
			const std::string_view Name = StoreName(Names.front().Text);
			MacroDef Def {
				Start,
				Epoch,
				IsDef ? MacroState::Defined : MacroState::Cancelled,
				{},
				Is(Node, Core::Role::PreprocFunctionDef)
			};
			Def.InputState = IsDef ? MacroState::Defined : MacroState::Undefined;
			if(IsDef) {
				const std::vector<MacroToken> Tokens = TokenizeMacroBody(GetView(GetFieldChild(Node, Core::ChildRole::Val)));
				if(!Def.IsFunctionLike && Tokens.size() == 1) {
					const MacroToken &Token = Tokens.front();
					if(!Token.IsOpaque) Def.Alias = StoreName(Token.Text);
				}
				Def.Body = ReadMacroBody(Node, Tokens);
			}
			Record(Name, std::move(Def), End);
			continue;
		}
		if(Is(Node, Core::Role::SrcLeaf)) continue;
		const std::span<const ParsedChild> Children = GetChildrenOf(Node);
		for(size_t Idx = Children.size(); Idx; --Idx) {
			const TSNode Child = Children[Idx - 1].Node;
			if(HasMacroDescendant(Children[Idx - 1]) && ts_node_is_named(Child)) Pending.push_back({ Child, 0 });
		}
	}
	Syntax.HasMacroStates = true;
	// 終了
	return;
}

/**
 * 世代の共有メモリを名前の表へ渡す生成関数
 * 計算量：O(1)，要素は初回の登録で確保する
 * @param Storage 表の破棄迄生存する世代の共有メモリ
 */
ParsedTree::NamespaceBindings::NamespaceBindings(std::pmr::memory_resource *const Storage) : Routes(Storage),
UnknownRoutes(Storage),
NameResults(Storage),
Names(Storage),
Members(Storage),
Children(Storage),
UnknownImports(Storage),
UnknownEnumerators(Storage) {}

/**
 * 宣言した値と型の名前の収集関数（C / C++ / C#）
 * 位置は照会する時の構文木の位置と比べる為（`IsDeclaredName`），コメントの除去や編集の後の構文木から照会の直前に集め直す
 * （コメントを除く前に集めると，コメントの長さの分だけ宣言と使う位置の前後が入れ替わり，コメントの有無で括弧の要否が変わる）
 * ソース長 N，走査ノード数 V，収集する名前の総長 B，値・型の宣言数 M，所属の領域数 K，有効範囲の照会費用 Q
 * 計算量：平均 O(N + V + B + M log(M + 1) + K log(K + 1) + Q)
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void ParsedTree::CollectDeclaredNames() const {
	using Core::Role;
	// 収集済・位置順の収集中・木が無い場合の終了
	if(Syntax.HasNameBindings || Syntax.IsCollectingNames || !IsParsed()) return;
	// 未反映編集の再解析で無効に為る表は，此の木から改めて作る
	const TSNode Root = GetRoot();
	const bool HasNameSummary = GetReading().HasPreprocMacros;
	// 無名の直接子の語を，文法が定める意味で照合する
	const auto HasWordChild = [this](const TSNode Node, const Core::Word Meaning) -> bool {
		// 語の意味に対応する子の有無の返戻
		return HasChildOf(
			Node,
			[&](const TSNode Child) -> bool {
				// 名前付の構文を除いた直接の語の一致の返戻
				return !ts_node_is_named(Child) && GetProfile().IsSpelling(GetView(Child), Meaning);
			}
		);
	};
	// マクロだけを読む工程の収集結果を保ち，宣言の表は最初の照会で作る
	CollectMacroStates();
	ReboundDecls.clear();
	TemplateDecls.clear();
	NameData->Bindings.clear();
	NameData->MemberOwners.clear();
	NameData->Regions.Bindings.clear();
	NameData->Regions.MaxEnds.clear();
	NameData->Regions.Capacity = 1;
	NameData->HasUnknownImports = false;
	// 収集中の位置順の照会は，公開済の束縛だけを読む
	struct CollectionGuard {
		bool &IsActive; // 収集中のフラグ

		/**
		 * 収集の終了又は例外での再入フラグの復元
		 * 計算量：O(1)
		 */
		~CollectionGuard() {
			IsActive = false;
			// 終了
			return;
		}
	};
	Syntax.IsCollectingNames = true;
	const CollectionGuard Guard { Syntax.IsCollectingNames };
	NameData->Bindings.emplace_back(&NameData->Texts.Storage);
	// 宣言した名前の位置との記録
	// 宣言した名前が見える最も内側の範囲の終端（仮引数は関数・テンプレートの定義又は宣言の終わり，其の他は本体・クラスの終わり，名前空間の所属は物理的なブロックから分離）
	struct DeclScope {
		uint32_t BlockEnd;
		uint32_t ParamEnd;
		uint32_t BranchEnd;
		uint32_t EnumEnd; // 列挙子を導入する範囲の終端
		uint32_t NamespaceEnd; // 所属に直接導入するブロックの終端（定義の本体は０）
		size_t Namespace; // 現在の所属の識別番号
		size_t EnumNamespace = std::string::npos; // 限定名で列挙子を解決する所属
		uint32_t DefinitionPos = 0; // 限定名の後で所属を切り替える位置
		size_t DefinitionNamespace = 0; // 限定名が指す定義の所属
		size_t LexicalNamespace = 0; // 限定名より前の戻値の型を読む所属
		const Core::RoleSet *Roles = nullptr; // 子へ引き継ぐ固定の役割の集合
	};
	// 所有者から先に得た宣言を，本来の有効位置で公開する為の登録待ちの束縛
	struct PendingBinding {
		std::string_view Name; // 収集中の本文を参照する名前
		NameBinding Binding; // 共通の位置・範囲・区分
	};
	std::vector<PendingBinding> Pending;
	std::vector<NameBinding> PendingRegions;
	// 限定名の前の戻値の型に属する領域も，開始位置順で公開する
	const auto IsLaterRegion = [](const NameBinding &Left, const NameBinding &Right) -> bool {
		// 後の開始位置かの返戻
		return Left.Pos > Right.Pos;
	};
	const auto RegisterRegion = [&](const NameBinding Region) -> void {
		PendingRegions.push_back(Region);
		std::push_heap(PendingRegions.begin(), PendingRegions.end(), IsLaterRegion);
		// 終了
		return;
	};
	// 有効位置の早い束縛を先頭に置く比較
	const auto IsLater = [](const PendingBinding &Left, const PendingBinding &Right) -> bool {
		// 後の有効位置かの返戻
		return Left.Binding.Pos > Right.Binding.Pos;
	};
	const uint32_t RootEnd = ts_node_end_byte(Root);
	// 使用位置に達した宣言と所属を位置順のインデックスへ公開
	const auto PublishBefore = [&](const uint32_t Use) -> void {
		while(!PendingRegions.empty() && PendingRegions.front().Pos < Use) {
			std::pop_heap(PendingRegions.begin(), PendingRegions.end(), IsLaterRegion);
			NameData->Regions.Add(PendingRegions.back());
			PendingRegions.pop_back();
		}
		while(!Pending.empty() && Pending.front().Binding.Pos < Use) {
			std::pop_heap(Pending.begin(), Pending.end(), IsLater);
			const PendingBinding &Site = Pending.back();
			NamespaceBindings &Owner = NameData->Bindings[Site.Binding.Namespace];
			const std::string_view Name = StoreName(Site.Name);
			Owner.Names.try_emplace(Name, &NameData->Texts.Storage).first->second.Add(Site.Binding);
			if(Site.Binding.IsMember) {
				const auto [Member, Added] = Owner.Members.try_emplace(Name, &NameData->Texts.Storage);
				NameBinding Export = Site.Binding;
				if(Owner.IsClass && Export.End == RootEnd) Export.Pos = Owner.Start;
				Member->second.Add(Export);
				if(Added) NameData->MemberOwners[Name].push_back(Site.Binding.Namespace);
			}
			Pending.pop_back();
		}
		// 終了
		return;
	};
	// 宣言の有効位置と区分を１度だけ登録
	const auto Register = [&](const std::string_view Name, const NameBinding Binding) -> void {
		Pending.push_back({ Name, Binding });
		std::push_heap(Pending.begin(), Pending.end(), IsLater);
		// 終了
		return;
	};
	// 最上位の所属はブロックを持たない為，末尾で閉じるローカル範囲と終端を共有しない
	std::vector<DeclScope> Scopes { { 0, RootEnd, RootEnd, RootEnd, 0, 0 } };
	Scopes.front().Roles = &GetProfile().GetKindOf(ts_node_symbol(Root)).Roles;
	DeclScope Cur { 0, RootEnd, RootEnd, RootEnd, 0, 0 };
	// 名前の有効範囲の終端の取得
	const auto ScopeEndOf = [&](const TSNode Owner) -> uint32_t {
		// scoped enum の列挙子は列挙内部だけへ導入する事の返戻
		if(Is(Owner, Core::Role::EnumerationItem)) {
			// 列挙子の所属先で有効な終端の返戻
			return std::min(Cur.BranchEnd, Cur.EnumEnd == Cur.NamespaceEnd ? RootEnd : Cur.EnumEnd);
		}
		// 範囲 for 自身が導入名の有効範囲の終端である事の返戻
		if(Is(Owner, Core::Role::ForRangeLoop)) return std::min(Cur.BranchEnd, ts_node_end_byte(Owner));
		// 所有者の親から継いだ最寄の有効範囲の返戻
		return std::min(
			Cur.BranchEnd,
			Is(Owner, Core::Role::ParamDecl) || Is(Owner, Core::Role::CaptureInitializer) || Is(Owner, Core::Role::TemplateTypeParam) ||
			Is(Owner, Core::Role::TemplateValParam) ? Cur.ParamEnd : Cur.BlockEnd == Cur.NamespaceEnd ? RootEnd : Cur.BlockEnd
		);
	};
	// 宣言子を指定の名前迄解包したノード（無ければ空）
	const auto NameOf = [&](TSNode Declarator, const Core::Role Wanted) -> TSNode {
		// 値の導入名は通常の識別子・フィールド名・構造化束縛の何れか
		const auto IsName = [&](const TSNode Node) -> bool {
			// 指定した型名又は値の導入名かの返戻
			return Is(Node, Wanted) ||
			Wanted == Core::Role::Identifier && (Is(Node, Core::Role::FieldName) || Is(Node, Core::Role::StructuredBindingDeclarator));
		};
		while(!ts_node_is_null(Declarator) && !IsName(Declarator)) {
			TSNode Next = GetFieldChild(Declarator, Core::ChildRole::Declarator);
			if(ts_node_is_null(Next)) {
				ForEachNamedChild(
					Declarator,
					[&](const TSNode Child) -> void {
						// 宣言子の発見後の終了
						if(!ts_node_is_null(Next)) return;
						if(
							Is(Child, Core::Role::DeclaratorWrapper) || Is(Child, Core::Role::ParenthesizedDeclarator) ||
							Is(Child, Core::Role::ArrayDeclarator) ||
							(Is(Declarator, Core::Role::RefDeclarator) || Is(Declarator, Core::Role::ParenthesizedDeclarator)) && IsName(Child)
						) Next = Child;
						// 終了
						return;
					}
				);
			}
			Declarator = Next;
		}
		// 名前のノードの返戻
		return Declarator;
	};
	// 宣言した名前の位置と有効範囲の終端の記録
	const auto Declare = [&](const TSNode Name, const TSNode Owner, const bool IsType) -> void {
		if(!ts_node_is_null(Name)) {
			// 範囲 `for` の導入名は反復対象の評価後，本体から有効に為る
			const uint32_t Pos =
			Is(Owner, Core::Role::ForRangeLoop) ? GetEnd(GetFieldChild(Owner, Core::ChildRole::Right)) : ts_node_start_byte(Name);
			NameBinding Binding { Pos, ScopeEndOf(Owner), IsType, Cur.Namespace };
			Binding.Origin = ts_node_start_byte(Name);
			Binding.IsMember =
			Cur.BlockEnd == Cur.NamespaceEnd && !Is(Owner, Core::Role::ParamDecl) && !Is(Owner, Core::Role::TemplateTypeParam) &&
			!Is(Owner, Core::Role::TemplateValParam);
			if(IsType && Is(Owner, Core::Role::AliasDecl)) {
				Binding.Target = ResolveNameScope(GetView(GetFieldChild(Owner, Core::ChildRole::Type)), Pos);
				Binding.IsTemplate = Is(GetParentOf(Owner), Core::Role::TypeParamScope);
			}
			Register(GetView(Name), Binding);
		}
		// 終了
		return;
	};
	// １つの宣言子が導入する値の名前（構造化束縛は其々の導入名）を記録
	const auto DeclareVals = [&](const TSNode Declarator, const TSNode Owner) -> void {
		if(const TSNode Name = NameOf(Declarator, Core::Role::Identifier); Is(Name, Core::Role::StructuredBindingDeclarator)) {
			ForEachNamedChild(
				Name,
				[&](const TSNode Binding) -> void {
					Declare(Binding, Owner, false);
					// 終了
					return;
				}
			);
		} else Declare(Name, Owner, false);
		// 終了
		return;
	};
	// 型・非型・既定値・パックの名前の抽出を，テンプレートと定義の所属で共有する
	const auto DeclareTemplateParams = [&](const TSNode Params) -> void {
		ForEachNamedChild(
			Params,
			[&](const TSNode Param) -> void {
				if(Is(Param, Core::Role::TemplateTypeParam)) {
					TSNode Name = GetFieldChild(Param, Core::ChildRole::Name);
					if(ts_node_is_null(Name)) {
						ForEachNamedChild(
							Param,
							[&](const TSNode Child) -> void {
								if(ts_node_is_null(Name) && Is(Child, Core::Role::TypeName)) Name = Child;
								else if(ts_node_is_null(Name) && Is(Child, Core::Role::TemplateTypeParam)) {
									Name = GetFieldChild(Child, Core::ChildRole::Name);
									if(ts_node_is_null(Name)) Name = GetFirstNamedChild(Child, Core::Role::TypeName);
								}
								// 終了
								return;
							}
						);
					}
					Declare(Name, Param, true);
				} else if(Is(Param, Core::Role::TemplateValParam)) {
					TSNode Declarator = GetFieldChild(Param, Core::ChildRole::Declarator);
					if(Is(Param, Core::Role::VariadicParamDecl)) {
						const TSNode Inner = GetFieldChild(Declarator, Core::ChildRole::Declarator);
						Declarator = ts_node_is_null(Inner) ? GetFirstNamedChild(Declarator, Core::Role::Identifier) : Inner;
					}
					DeclareVals(Declarator, Param);
				}
				// 終了
				return;
			}
		);
		// 終了
		return;
	};
	// 所属を跨ぐ導入の有効位置と終端だけを登録し，宣言列はコピーしない
	const auto Import = [&](const size_t Owner, const size_t Target, const uint32_t Pos, const uint32_t End) -> void {
		if(Target != std::string::npos) AddNameImport(Owner, { Pos, End, false, Target });
		// 終了
		return;
	};
	// 導入した型・名前空間の別名も元の宣言の同一性を保つ
	const auto RegisterAlias =
	[&](const std::string_view Name, const NameBinding *Source, const uint32_t Pos, const uint32_t End) -> void {
		NameBinding Binding = Source ? *Source : NameBinding{ Pos, End, false, Cur.Namespace, false };
		Binding.Pos = Pos;
		Binding.End = End;
		Binding.Namespace = Cur.Namespace;
		Binding.IsMember = Cur.BlockEnd == Cur.NamespaceEnd;
		Register(Name, Binding);
		// 終了
		return;
	};
	WalkRecordedChildren(
		*this,
		Root,
		[&](const ParsedChild &Record, const std::span<const TSNode> Ancestors) -> bool {
			// 宣言と所属の無い部分木では，葉の省略数だけを記録して降りない
			if(HasNameSummary && !Syntax.Relations->Parents.Records[Record.Relation].HasNameWork) {
				if(!Syntax.Relations->Parents.Records[Record.Relation].Count) ++Syntax.Relations->NameLeafSkips;
				// 収集する操作の無い部分木の返戻
				return false;
			}
			const TSNode Node = Record.Node;
			const uint32_t Start = Record.GetStart();
			PublishBefore(Start);
			Scopes.resize(Ancestors.size());
			Cur = Scopes.back();
			// 現在の記号は１度だけ引き，親の役割は祖先から継いだ同じ定義を読む
			const Core::RoleSet &Roles = GetProfile().GetKindOf(Record.Symbol).Roles;
			const Core::RoleSet &ParentRoles = *Cur.Roles;
			const auto HasRole = [&Roles](const Core::Role Wanted) -> bool {
				// 固定定義の役割の返戻
				return Roles.test(static_cast<size_t>(Wanted));
			};
			const auto HasParentRole = [&ParentRoles](const Core::Role Wanted) -> bool {
				// 親から継いだ固定定義の役割の返戻
				return ParentRoles.test(static_cast<size_t>(Wanted));
			};
			if(Cur.DefinitionPos) Cur.Namespace = Start < Cur.DefinitionPos ? Cur.LexicalNamespace : Cur.DefinitionNamespace;
			const TSNode Parent = Ancestors.back();
			const bool IsBlockScope =
			HasParentRole(Role::CompoundBody) || HasParentRole(Role::LexicalBlock) || HasParentRole(Role::NamespaceDef) ||
			HasParentRole(Role::ClassLike) || HasParentRole(Role::Loop) || HasParentRole(Role::Conditional) ||
			HasParentRole(Role::SwitchStmt) || HasParentRole(Role::CatchClause);
			if(IsBlockScope) Cur.BlockEnd = ts_node_end_byte(Parent);
			if(
				IsBlockScope || HasParentRole(Role::FunctionScope) || HasParentRole(Role::PotentialExprDecl) ||
				HasParentRole(Role::TypeParamScope)
			) Cur.ParamEnd = ts_node_end_byte(Parent);
			// 外側の条件枝の終端を継ぎ，直接親の条件枝だけを重ねる
			if(HasParentRole(Role::PreprocBranch)) {
				const TSNode Alternative = GetFieldChild(Parent, Core::ChildRole::Alternative);
				const uint32_t End = !ts_node_is_null(Alternative) && Start < ts_node_start_byte(Alternative) ?
				ts_node_start_byte(Alternative) :
				ts_node_end_byte(Parent);
				Cur.BranchEnd = std::min(Cur.BranchEnd, End);
			}
			// `enum class` / `enum struct` だけを列挙内部へ限り，通常の列挙は外側へ導入する
			if(HasRole(Core::Role::EnumSpecifier)) {
				const TSNode Name = GetFieldChild(Node, Core::ChildRole::Name);
				const TSNode Body = GetFieldChild(Node, Core::ChildRole::Body);
				Cur.EnumNamespace = std::string::npos;
				if(HasRole(Core::Role::BareTypeDecl) && Is(Name, Core::Role::TypeName)) {
					const size_t Owner = Cur.Namespace;
					NameBinding Binding { ts_node_start_byte(Name), ScopeEndOf(Node), true, Owner };
					Binding.IsMember = Cur.BlockEnd == Cur.NamespaceEnd;
					Binding.Origin = ts_node_start_byte(Name);
					const NamespaceBindings &Enclosing = NameData->Bindings[Owner];
					const decltype(Enclosing.Names)::const_iterator Found = Enclosing.Names.find(GetView(Name));
					const NameBinding *Previous = nullptr;
					if(Found != Enclosing.Names.end()) Previous = Found->second.Find(Start, GetNameWorkCounter());
					// 同じ所属・有効範囲の再宣言だけを共有し，ローカルの列挙型が外側を遮蔽する事を保つ
					Binding.Target = Previous && Previous->IsType && Previous->IsMember == Binding.IsMember && Previous->End == Binding.End ?
					Previous->Target :
					std::string::npos;
					if(Binding.Target == std::string::npos || !NameData->Bindings[Binding.Target].IsEnum) {
						Binding.Target = NameData->Bindings.size();
						NameData->Bindings.emplace_back(&NameData->Texts.Storage);
						NameData->Bindings.back().Parent = Owner;
						NameData->Bindings.back().Depth = NameData->Bindings[Owner].Depth + 1;
						NameData->Bindings.back().IsEnum = true;
					}
					if(!ts_node_is_null(Body)) NameData->Bindings[Binding.Target].HasEnumBody = true;
					Cur.EnumNamespace = Binding.Target;
					Register(GetView(Name), Binding);
				}
				Cur.EnumEnd = HasWordChild(Node, Core::Word::ScopedEnumModifier) ? ts_node_end_byte(Node) : Cur.BlockEnd;
			}
			// 名前空間の同一性は外側の所属と名前で共有し，物理的なブロックは区間で記録
			if(HasRole(Core::Role::NamespaceDef)) {
				Cur.DefinitionPos = 0;
				const TSNode Name = GetFieldChild(Node, Core::ChildRole::Name);
				const auto Enter = [&](const std::string_view Part, const bool IsInline) -> void {
					const size_t Parent = Cur.Namespace;
					NamespaceBindings &Outer = NameData->Bindings[Parent];
					const auto [Found, Added] = Outer.Children.try_emplace(StoreName(Part), NameData->Bindings.size());
					Cur.Namespace = Found->second;
					if(Added) {
						NameData->Bindings.emplace_back(&NameData->Texts.Storage);
						NameData->Bindings.back().Parent = Parent;
						NameData->Bindings.back().Depth = NameData->Bindings[Parent].Depth + 1;
					}
					if(!Part.empty()) {
						NameBinding Binding { Start, RootEnd, false, Parent };
						Binding.IsNamespace = true;
						Binding.IsMember = true;
						Binding.Target = Cur.Namespace;
						Binding.Origin = Start;
						Register(Part, Binding);
					}
					if(Part.empty() || IsInline) {
						if(IsInline) NameData->Bindings[Parent].HasInline = true;
						Import(Parent, Cur.Namespace, Start, std::min(RootEnd, Cur.BranchEnd));
					}
					// 終了
					return;
				};
				if(ts_node_is_null(Name)) Enter({}, false);
				else if(Is(Name, Core::Role::NamespaceName) || Is(Name, Core::Role::Identifier) || Is(Name, Core::Role::ExprTypeName)) {
					Enter(GetView(Name), HasWordChild(Node, Core::Word::InlineNamespace));
				} else {
					// ネストの綴りでは `inline` の直後の名前だけを暗黙に導入する
					bool IsInline = HasWordChild(Node, Core::Word::InlineNamespace);
					for(uint32_t Idx = 0; Idx < ts_node_child_count(Name); ++Idx) {
						const TSNode Part = ts_node_child(Name, Idx);
						if(Is(Part, Core::Role::NamespaceName)) {
							Enter(GetView(Part), IsInline);
							IsInline = false;
						} else if(!ts_node_is_named(Part) && GetProfile().IsSpelling(GetView(Part), Core::Word::InlineNamespace)) IsInline = true;
					}
				}
				Cur.NamespaceEnd = ts_node_end_byte(Node);
				RegisterRegion({ Start, Cur.NamespaceEnd, false, Cur.Namespace });
			}
			// クラスの宣言位置と所有するメンバの範囲を，外側のローカル範囲から分離する
			if(HasRole(Core::Role::ClassLike) && HasRole(Core::Role::BareTypeDecl)) {
				const TSNode Name = GetFieldChild(Node, Core::ChildRole::Name);
				if(Is(Name, Core::Role::TypeName) || Is(Name, Core::Role::Identifier)) {
					const size_t Owner = Cur.Namespace;
					NameBinding Binding { ts_node_start_byte(Name), ScopeEndOf(Node), true, Owner };
					Binding.IsMember = Cur.BlockEnd == Cur.NamespaceEnd;
					Binding.Origin = ts_node_start_byte(Name);
					Binding.IsTemplate = HasParentRole(Role::TypeParamScope);
					const NamespaceBindings &Enclosing = NameData->Bindings[Owner];
					const decltype(Enclosing.Names)::const_iterator Found = Enclosing.Names.find(GetView(Name));
					const NameBinding *Previous = nullptr;
					if(Found != Enclosing.Names.end()) Previous = Found->second.Find(Start, GetNameWorkCounter());
					Binding.Target = Binding.IsMember && Previous && Previous->IsType ? Previous->Target : std::string::npos;
					if(Binding.Target == std::string::npos) {
						Binding.Target = NameData->Bindings.size();
						NameData->Bindings.emplace_back(&NameData->Texts.Storage);
						NameData->Bindings.back().Parent = Owner;
						NameData->Bindings.back().Depth = NameData->Bindings[Owner].Depth + 1;
						NameData->Bindings.back().IsClass = true;
						NameData->Bindings.back().Start = Start;
					}
					Register(GetView(Name), Binding);
					Cur.Namespace = Binding.Target;
					Cur.DefinitionPos = 0;
					Cur.BlockEnd = Cur.NamespaceEnd = ts_node_end_byte(Node);
					RegisterRegion({ Start, Cur.NamespaceEnd, false, Cur.Namespace });
					if(HasParentRole(Role::TypeParamScope)) DeclareTemplateParams(GetFirstNamedChild(Parent, Core::Role::GenericParams));
					// 完全なクラスの本体で後の型を使える様に，直接の型名だけを先に共有する
					// 関数の本体とネストしたクラスの内部は再走査しない
					const auto ExportType = [&](const TSNode Name) -> void {
						if(!ts_node_is_null(Name)) {
							NamespaceBindings &Owner = NameData->Bindings[Cur.Namespace];
							NameBinding Type { Owner.Start, std::min(RootEnd, Cur.BranchEnd), true, Cur.Namespace };
							Type.IsMember = true;
							Type.Origin = ts_node_start_byte(Name);
							Type.IsTemplate = Is(GetParentOf(GetParentOf(Name)), Core::Role::TypeParamScope);
							const std::string_view Text = StoreName(GetView(Name));
							const auto [Member, Added] = Owner.Members.try_emplace(Text, &NameData->Texts.Storage);
							Member->second.Add(Type);
							if(Added) NameData->MemberOwners[Text].push_back(Cur.Namespace);
						}
						// 終了
						return;
					};
					// 前方宣言は本体を持たない
					const TSNode Body = GetFieldChild(Node, Core::ChildRole::Body);
					if(!ts_node_is_null(Body)) {
						ForEachNamedChild(
							Body,
							[&](TSNode Member) -> void {
								ChargeNameWork();
								if(Is(Member, Core::Role::TypeParamScope) && ts_node_named_child_count(Member)) {
									Member = ts_node_named_child(Member, ts_node_named_child_count(Member) - 1);
								}
								if(Is(Member, Core::Role::BareTypeDecl)) {
									if(Is(Member, Core::Role::TypeDef)) {
										const TSNode Type = GetFieldChild(Member, Core::ChildRole::Type);
										ForEachNamedChild(
											Member,
											[&](const TSNode Part) -> void {
												if(!ts_node_eq(Part, Type)) ExportType(NameOf(Part, Core::Role::TypeName));
												// 終了
												return;
											}
										);
									} else ExportType(GetFieldChild(Member, Core::ChildRole::Name));
								}
								// 終了
								return;
							}
						);
					}
				}
			}
			// クラス外の定義では，限定名の後の仮引数・戻値・本体へクラスの所属を渡す
			if(HasRole(Core::Role::FunctionScope)) {
				TSNode Name = GetFieldChild(Node, Core::ChildRole::Declarator);
				while(!ts_node_is_null(Name) && !Is(Name, Core::Role::QualifiedIdentifier)) {
					Name = GetFieldChild(Name, Core::ChildRole::Declarator);
				}
				if(!ts_node_is_null(Name)) {
					const std::string_view Text = GetView(Name);
					const size_t Lexical = Cur.Namespace, Sep = Text.rfind("::");
					size_t Target = ResolveNameScope(Text.substr(0, Sep), Start);
					if(Target == std::string::npos) {
						Target = NameData->Bindings.size();
						NameData->Bindings.emplace_back(&NameData->Texts.Storage);
						NameData->Bindings.back().Parent = Lexical;
						NameData->Bindings.back().Depth = NameData->Bindings[Lexical].Depth + 1;
						NameData->Bindings.back().IsUnresolved = true;
					}
					Cur.Namespace = Target;
					Cur.DefinitionPos = ts_node_end_byte(Name);
					Cur.DefinitionNamespace = Target;
					Cur.LexicalNamespace = Lexical;
					Cur.BlockEnd = ts_node_end_byte(Node);
					Cur.NamespaceEnd = 0; // 非公開の本体の名前をメンバへ含めない
					RegisterRegion({ Cur.DefinitionPos, ts_node_end_byte(Node), false, Target });
					if(Target != Lexical && HasParentRole(Role::TypeParamScope)) {
						DeclareTemplateParams(GetFirstNamedChild(Parent, Core::Role::GenericParams));
					}
				}
			}
			// 基底はクラスのメンバだけを導入し，依存する型の綴りから所属を推測しない
			if(HasRole(Core::Role::BindingBaseClause)) {
				ForEachNamedChild(
					Node,
					[&](const TSNode Base) -> void {
						const size_t Target = ResolveNameScope(GetView(Base), Start);
						if(Target == std::string::npos && (Is(Base, Core::Role::TypeName) || Is(Base, Core::Role::QualifiedIdentifier))) {
							AddNameImport(Cur.Namespace, { Start, RootEnd, false, std::string::npos });
						} else Import(Cur.Namespace, Target, Start, RootEnd);
						// 終了
						return;
					}
				);
			}
			// 宣言の名前と同じく最後のトークン（`;`）の開始を有効位置にし，直後に空白が無くても次のトークンから見える様にする
			const auto TerminatorStart = [](const TSNode Owner) -> uint32_t {
				// 最後のトークンの開始位置の返戻
				return ts_node_start_byte(ts_node_child(Owner, ts_node_child_count(Owner) - 1));
			};
			// 名前空間の別名は導入位置以降にだけ有効にする（別名の対象はフィールドを持たない最後の名前付子）
			if(HasRole(Core::Role::NamespaceAliasDef)) {
				const TSNode Name = GetFieldChild(Node, Core::ChildRole::Name);
				const size_t Target = ResolveNameScope(GetView(LastNamedChild(Node)), Start);
				NameBinding Binding { Start, ScopeEndOf(Node), false, Cur.Namespace, Target != std::string::npos };
				Binding.IsNamespace = true;
				Binding.Target = Target;
				Binding.Origin = Start;
				RegisterAlias(GetView(Name), &Binding, TerminatorStart(Node), ScopeEndOf(Node));
			}
			// 明示のディレクティブと単一の名前の導入を区別する
			if(HasRole(Core::Role::ScopeImport) && ts_node_named_child_count(Node)) {
				const TSNode Name = ts_node_named_child(Node, ts_node_named_child_count(Node) - 1);
				const std::string_view Text = GetView(Name);
				if(HasWordChild(Node, Core::Word::UsingNamespace)) {
					const size_t Target = ResolveNameScope(Text, Start);
					if(Target == std::string::npos) {
						AddNameImport(Cur.Namespace, { TerminatorStart(Node), ScopeEndOf(Node), false, std::string::npos });
					} else Import(Cur.Namespace, Target, TerminatorStart(Node), ScopeEndOf(Node));
				} else if(HasWordChild(Node, Core::Word::EnumImport)) {
					const size_t Target = ResolveNameScope(Text, Start);
					const bool HasTarget = Target != std::string::npos;
					const NamespaceBindings *const Enumeration = HasTarget ? &NameData->Bindings[Target] : nullptr;
					if(!Enumeration || !Enumeration->IsEnum || !Enumeration->HasEnumBody) {
						const NameBinding Uncertain { TerminatorStart(Node), ScopeEndOf(Node), false, std::string::npos, false };
						NameData->Bindings[Cur.Namespace].UnknownEnumerators.Add(Uncertain);
						AddNameImport(Cur.Namespace, Uncertain);
					} else for(const auto &[Part, Index] : NameData->Bindings[Target].Members) {
						const NameBinding *Source = Index.Find(Start, GetNameWorkCounter());
						if(Source) RegisterAlias(Part, Source, TerminatorStart(Node), ScopeEndOf(Node));
					}
				} else if(const size_t Sep = Text.rfind("::"); Sep != std::string_view::npos) {
					const size_t Target = ResolveNameScope(Text.substr(0, Sep), Start);
					const TSNode Last = GetFieldChild(Name, Core::ChildRole::Name);
					const std::string_view Part = ts_node_is_null(Last) ? Text.substr(Sep + 2) : GetView(Last);
					const NameBinding *Source = Target == std::string::npos ? nullptr : GetScopeBinding(Target, Part, Start, true);
					RegisterAlias(Part, Source, TerminatorStart(Node), ScopeEndOf(Node));
				}
			}
			// 葉として投影する仮引数の導入名だけを登録する
			if(
				HasRole(Core::Role::GenericParams) &&
				(HasParentRole(Role::TypeParamScope) || HasParentRole(Role::FunctionScope) || HasParentRole(Role::ClassLike))
			) DeclareTemplateParams(Node);
			Cur.Roles = &Roles;
			Scopes.push_back(Cur);
			if(HasRole(Core::Role::BareTypeDecl) && !HasRole(Core::Role::ClassLike) && !HasRole(Core::Role::EnumSpecifier)) {
				if(HasRole(Core::Role::TypeDef)) {
					const TSNode Type = GetFieldChild(Node, Core::ChildRole::Type);
					ForEachNamedChild(
						Node,
						[&](const TSNode Declarator) -> void {
							// 型の子を除き，宣言子を型名迄解包して記録
							if(!ts_node_eq(Declarator, Type)) Declare(NameOf(Declarator, Core::Role::TypeName), Node, true);
							return;
						}
					);
				} else if(
					const TSNode Name = GetFieldChild(Node, Core::ChildRole::Name);
					Is(Name, Core::Role::TypeName) || Is(Name, Core::Role::Identifier)
				) Declare(Name, Node, true);
			}
			// 列挙子・初期化捕捉・範囲 for・宣言の名前を有効範囲へ収集
			bool HasDeclaredValues = false;
			if(HasRole(Core::Role::EnumerationItem)) {
				const TSNode Name = GetFieldChild(Node, Core::ChildRole::Name);
				Declare(Name, Node, false);
				if(Cur.EnumNamespace != std::string::npos && !ts_node_is_null(Name)) {
					NameBinding Binding { ts_node_start_byte(Name), RootEnd, false, Cur.EnumNamespace };
					Binding.IsMember = true;
					Binding.Origin = ts_node_start_byte(Name);
					Register(GetView(Name), Binding);
				}
			} else if(HasRole(Core::Role::CaptureInitializer)) {
				// 初期化捕捉の名前は初期値の外側，ラムダの本体から見える
				const std::string_view Name = GetView(GetFieldChild(Node, Core::ChildRole::Left));
				if(!Name.empty()) {
					Register(
						Name,
						{
							ts_node_end_byte(Node),
							ScopeEndOf(Node),
							false,
							Cur.Namespace,
							true,
							false,
							false,
							std::string::npos,
							ts_node_start_byte(Node)
						}
					);
				}
			// 反復対象と本体の名前を導入名へ含めない
			} else if(HasRole(Core::Role::ForRangeLoop)) DeclareVals(GetFieldChild(Node, Core::ChildRole::Declarator), Node);
			else if(HasRole(Core::Role::NameDecl) && !IsAmbiguousDecl(Node)) {
				HasDeclaredValues = true;
				// 名前フィールドを持つ宣言は其の名前だけを収集し，型と初期化子の参照を導入名へ含めない
				DeclareVals(GetFieldChild(Node, Core::ChildRole::Name), Node);
				for(const ParsedChild &Child : GetChildrenOf(Node)) if(Grammar->GetChildRoleOf(Child.Field) == Core::ChildRole::Declarator) {
					DeclareVals(Child.Node, Node);
				}
			}
			if(HasRole(Core::Role::ExprStmt)) if(std::optional<ParenthesizedDecl> Decl = ReadParenthesizedDecl(Node); Decl) {
				for(const DeclaratorItem &Item : Decl->Items) Declare(Item.Name, Node, false);
				ReboundDecls.push_back(std::move(*Decl));
			}
			// 読違から確定した導入名も，同じ有効範囲の値として後続の照会へ渡す
			TSNode Name {};
			if(std::vector<TemplatePart> Parts = ReadTemplateDecl(Node, Name); !Parts.empty()) {
				if(!HasDeclaredValues) Declare(Name, Node, false);
				TemplateDecls.emplace(Node.id, std::move(Parts));
			}
			// マクロの定義本文と葉以外へ降りる事の返戻
			return !HasRole(Core::Role::MacroDef) && !HasRole(Core::Role::SrcLeaf);
		},
		Syntax.Relations->NameVisits,
		Syntax.Relations->NameChildQueries,
		Syntax.Relations->NameLeafSkips
	);
	// 末尾の宣言を位置順に公開して収集を確定
	PublishBefore(RootEnd);
	Syntax.HasNameBindings = true;
	// 終了
	return;
}

/**
 * 受理の区分の取得関数
 * 計算量：O(1)
 * @return 継続，見送又は失敗
 */
ParseDecision ParsedTree::GetParseDecision() const {
	// 記録済の区分の返戻
	return Decision;
}

/**
 * 受理しない理由の取得関数
 * 計算量：O(1)
 * @return 見送又は失敗の理由
 */
FailureReason ParsedTree::GetParseFailure() const {
	// 記録済の理由の返戻
	return Failure;
}

/**
 * 入力エラーと未識別構文の区分の取得関数
 * 計算量：O(1)
 * @return 受理時に記録した破損の原因の区分
 */
ParseIssue ParsedTree::GetParseIssue() const {
	// 破損の原因の区分の返戻
	return Issue;
}

/**
 * 準備と修復を戻した破損位置の取得関数
 * 計算量：準備の編集数 P，修復の組数 E と１組の編集数 M に対し O(P + E log(M + 1))
 * @return 最初の破損位置（破損又は位置情報が無ければ空）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::optional<uint32_t> ParsedTree::GetIssueOffset() const {
	// 修復前の破損位置又は位置不在の返戻
	return IssueOffset ? std::optional<uint32_t>(OffsetInInput(*IssueOffset)) : std::nullopt;
}

/**
 * 読違を修復しても残る入力の構文エラーの有無の取得関数
 * 束縛と文全体から確定した宣言の中のエラーだけを除き，未確定の式と其の他の破損は残す
 * 計算量：初回は宣言収集費用 C と訪問ノード数 N に対し O(C + N)，以後は O(1)
 * @return 修復した宣言の外に入力エラーが残れば true
 * @throws std::bad_alloc 束縛又は走査のメモリを確保出来ない場合
 * @throws std::length_error 束縛又は走査の要素数上限を超える場合
 */
bool ParsedTree::HasOriginalInputError() const {
	// 正常入力又は処理を続けない入力の既存の分類の返戻
	if(!Grammar || !HasInputError || Decision != ParseDecision::Continue) return HasInputError;
	// 宣言の読違を修復しない文法の既存の分類の返戻
	if(!GetProfile().GetProjectionChecks(GetTemplateSymbols().Binary).CanReadTemplateDecl) return true;
	if(!HasUnrepairedInputError) {
		CollectDeclaredNames();
		bool IsBroken = false;
		WalkChildrenCursor(
			GetRoot(),
			[&](const TSNode Node) -> bool {
				ChargeNameWork();
				// 修復した宣言又は破損確定後の子へは降りない事の返戻
				if(IsBroken || TemplateDecls.contains(Node.id)) return false;
				IsBroken = Is(Node, Core::Role::Error) || ts_node_is_missing(Node);
				// 確定した宣言の外のエラーを保つ事の返戻
				return !IsBroken;
			}
		);
		HasUnrepairedInputError = IsBroken;
	}
	// 同じ世代で１度だけ確定した結果の返戻
	return *HasUnrepairedInputError;
}

/**
 * 受理の結果の記録関数
 * 計算量：O(1)
 * @param NewDecision 継続，見送又は失敗
 * @param Reason 見送又は失敗の理由
 * @param HasSyntaxError 構文エラーを含むか
 */
void ParsedTree::SetParseDecision(const ParseDecision NewDecision, const FailureReason Reason, const bool HasSyntaxError) {
	// 受理の結果の記録
	Decision = NewDecision;
	Failure = Reason;
	HasInputError = HasSyntaxError;
	HasUnrepairedInputError.reset();
	// 終了
	return;
}

/**
 * 確定した文法でソースを解析するコンストラクタ
 * 計算量：原文長 B と解析費用 P に対し O(B + P)
 * @param Profile 解析に用いる文法
 * @param Text ソース文字列
 * @param DataStart パーサーから除く逐語データの開始位置
 * @throws std::length_error 原文がパーサーの位置の範囲を超える場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 */
ParsedTree::ParsedTree(const GrammarProfile &Profile, std::string Text, const size_t DataStart) : std::string(std::move(Text)),
HaltedData(DataStart) {
	// ３２ビット位置に収まらないソースのパーサ生成前の拒否
	if(size() > std::numeric_limits<uint32_t>::max()) throw std::length_error("source exceeds the parser's position range");
	// 文法を設定したパーサーの準備
	Grammar = &Profile;
	Syntax.Parser.emplace();
	// 文法と実行時ライブラリが非互換で未解析の儘とする場合の終了
	if(!Syntax.Parser->CanSetGrammar(Profile.GetLang().GetTreeSitterLang())) return;
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
