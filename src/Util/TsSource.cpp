#include "Util/TsSource.hpp"
#include "Util/DocSig.hpp"
#include "Util/FileIO.hpp"
#include "Util/Lang.hpp"
#include "Util/NodeKind.hpp"
#include "Util/Parallel.hpp"
#include "Util/Postprocess.hpp"
#include "Util/TextEdit.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// 数 KB でも二乗化する誤り回復の時間上限（健全な解析は線形の為，固定猶予＋１バイト当り１マイクロ秒で遅い機械の大入力も許容）
static constexpr int64_t ParseDeadlineBaseMicros = 5000000;

/** ========== 構文木管理 ========== */
/**
 * 上限の超過の例外の構築関数
 * @param Text 利用者へ示す見送の理由（文法の静的領域か文字列リテラルを渡す）
 * @param IsFormatterDefect 発火してはならない防壁なら true
 */
FormatLimitExceeded::FormatLimitExceeded(const std::string_view Text, const bool IsFormatterDefect) : Reason(Text),
IsDefect(IsFormatterDefect) {}

/**
 * 複写代入演算子（複写先は数え直す）
 * @return 自身への参照
 */
ResettingCounter &ResettingCounter::operator=(const ResettingCounter &) {
	// 複写先計数値の初期化
	Value.store(0, std::memory_order_relaxed);
	// 自身への参照の返戻
	return *this;
}

/**
 * 複写コンストラクタ（複写先は数え直す，原稿の移動も此処を通る）
 */
ResettingCounter::ResettingCounter(const ResettingCounter &) {}

/**
 * パーサ解放関数
 * @param Target 解放するパーサ
 */
void TSSource::ParserDeleter::operator()(TSParser *const Target) const {
	// 所有パーサの解放
	ts_parser_delete(Target);
	// 終了
	return;
}

/**
 * 構文木解放関数
 * @param Target 解放する構文木
 */
void TSSource::TreeDeleter::operator()(TSTree *const Target) const {
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
 * 解析器へ文面を渡す読出関数
 * @param Payload 読ませる文面
 * @param ByteIndex 読み始める位置
 * @param Position 読み始める行と桁（位置で決まる読出をしない為に使わない）
 * @param BytesRead 返した長さの格納先
 * @return 読み始めた位置からの残り全体（末尾では長さ０）
 */
static const char *ReadWholeSource(
	void *const Payload,
	const uint32_t ByteIndex,
	[[maybe_unused]] const TSPoint Position,
	uint32_t *const BytesRead
) {
	// 解析対象の文面
	const std::string_view &Text = *static_cast<const std::string_view *>(Payload);
	if(ByteIndex >= Text.size()) {
		// 末尾での読出量
		*BytesRead = 0;
		// 末尾に達し読む物が無い事の返戻
		return "";
	}
	// 返す残余の長さ
	*BytesRead = static_cast<uint32_t>(Text.size() - ByteIndex);
	// 残り全体の返戻
	return Text.data() + ByteIndex;
}

/**
 * 解析の打切の判定関数
 * @param State 解析器が渡す進捗（期限を積んだ荷を持つ）
 * @return 期限を過ぎて居れば true（解析器は其処で解析を捨てる）
 */
static bool HasReachedParseDeadline(TSParseState *const State) {
	// 呼出側が設定した解析期限
	const std::chrono::steady_clock::time_point &Until =
	*static_cast<const std::chrono::steady_clock::time_point *>(State->payload);
	// 期限の超過の返戻
	return std::chrono::steady_clock::now() > Until;
}

/**
 * 期限付の構文解析関数
 * @param Parser 言語を設定済の解析器
 * @param Old 増分解析の土台（全体解析では nullptr）
 * @param Text 解析する文面
 * @param OutIsExpired 期限の超過の格納先（超過した時だけ真を書く）
 * @return 解析した構文木（期限の超過と失敗では nullptr）
 */
TSTree *ParseWithDeadline(TSParser *const Parser, const TSTree *const Old, const std::string_view Text, bool &OutIsExpired) {
	// 今回の解析期限
	const std::chrono::steady_clock::time_point Until =
	std::chrono::steady_clock::now() + std::chrono::microseconds(ParseDeadlineBaseMicros + static_cast<int64_t>(Text.size()));
	// 全文読出の入力
	const TSInput Input { const_cast<std::string_view *>(&Text), ReadWholeSource, TSInputEncodingUTF8, nullptr };
	// 期限監視の指定
	const TSParseOptions Options { const_cast<std::chrono::steady_clock::time_point *>(&Until), HasReachedParseDeadline };
	// 期限付の解析結果
	TSTree *const Tree = ts_parser_parse_with_options(Parser, Old, Input, Options);
	// 次の解析前に於ける打切後の解析器状態の初期化
	if(!Tree) {
		// 中断した解析状態の初期化
		ts_parser_reset(Parser);
		// 期限超過の通知
		if(std::chrono::steady_clock::now() > Until) OutIsExpired = true;
	}
	// 解析した構文木の返戻
	return Tree;
}

/**
 * 指定フィールド名の子ノード取得関数
 * @param Node 親ノード
 * @param Name フィールド名（文字列リテラルを想定し，ポインタを解決結果の記憶鍵に使う）
 * @return 対応する子ノード（無ければ空ノード）
 */
TSNode TSSource::FieldChild(const TSNode Node, const char *const Name) {
	// 場名解決の記憶（固定個の言語と不変のリテラルポインタを線形表で引き，毎回の長さ計算・文法表探索を省略）
	struct FieldIdEntry {
		const TSLanguage *Lang; // 対象言語
		const char *Name; // 場名リテラルのポインタ
		TSFieldId FieldId; // 解決済の場識別子（場が無い文法では 0）
	};
	// 走脈別の場識別子キャッシュ
	static thread_local std::vector<FieldIdEntry> ResolvedFieldIds;
	// 古い配布版にも対応する木経由の言語取得（木を持たない空節点は先に返戻）
	if(ts_node_is_null(Node)) return TSNode{};
	// 節点が属する文法
	const TSLanguage *const Lang = ts_tree_language(Node.tree);
	for(const FieldIdEntry &Entry : ResolvedFieldIds) {
		// 解決済の場識別子での直引き結果の返戻
		if(Entry.Lang == Lang && Entry.Name == Name) return Entry.FieldId ? ts_node_child_by_field_id(Node, Entry.FieldId) : TSNode{};
	}
	// 解決済の場識別子
	const TSFieldId FieldId = ts_language_field_id_for_name(Lang, Name, static_cast<uint32_t>(strlen(Name)));
	// 後続照会用の記憶
	ResolvedFieldIds.push_back({ Lang, Name, FieldId });
	// 解決した場識別子での直引き結果の返戻（場が無い文法では空ノード）
	return FieldId ? ts_node_child_by_field_id(Node, FieldId) : TSNode{};
}

/**
 * if 文の真枝本体取得関数
 * @param Node if 文又は if 式のノード
 * @return 条件成立時に実行する本体（無ければ空ノード）
 */
TSNode TSSource::IfConsequence(const TSNode Node) {
	// 真枝本体の場名は文法で異なり，C/C++/C#/Java/JS/TS 等は `consequence`，PHP は `body` を用いる
	const TSNode Consequence = FieldChild(Node, "consequence");
	// 真枝本体の返戻（`consequence` を持たない文法では `body` へ退避する）
	return ts_node_is_null(Consequence) ? FieldChild(Node, "body") : Consequence;
}

/**
 * 紐付コメント有無の判定関数
 * @return 先行／末尾コメントの何れかに登録が有れば true
 */
bool TSSource::HasAttachments() const {
	// 先行又は後続の紐付有無の返戻
	return !Attachments.LeadingByNode.empty() || !Attachments.TrailingByNode.empty();
}

/**
 * 解析済判定関数
 * @return 構文木を保持していれば true
 */
bool TSSource::IsParsed() const {
	// 構文木を保持して居るかの返戻
	return Syntax.Tree != nullptr;
}

/**
 * 解析の打切の判定関数
 * @return 解析が時間の上限で打ち切られて居れば true
 */
bool TSSource::HasExpiredParse() const {
	// 打切の有無の返戻
	return Syntax.IsParseExpired;
}

/**
 * ノードの名前ビュー取得関数
 * @param Node 対象ノード
 * @return `name` フィールドへのビュー（無ければ空ビュー）
 */
std::string_view TSSource::NodeNameView(const TSNode Node) const {
	// 構築時に解決済の場識別子で直引き（０は name 場不在の為，空ビューを返戻）
	if(!Syntax.NameFieldId) return {};
	// 名前場の子節点
	const TSNode NameField = ts_node_child_by_field_id(Node, Syntax.NameFieldId);
	// name 場が無いか，整形で字面が変わる非葉の名前なら空の返戻
	if(ts_node_is_null(NameField) || ts_node_child_count(NameField)) return {};
	// 名前場の原文範囲
	const uint32_t Start = ts_node_start_byte(NameField), End = ts_node_end_byte(NameField);
	// 範囲が空又は範囲外の場合の返戻
	if(Start >= End || End > size()) return {};
	// NameField のバイト範囲をビューとして返戻
	return std::string_view(data() + Start, End - Start);
}

/**
 * 紐付スナップショットの控えの有無の判定関数
 * @return 錨の控えが無ければ true
 */
bool TSSource::AttachSnapshots::IsEmpty() const {
	// 錨の控えの有無の返戻
	return Anchors.empty();
}

/**
 * 紐付スナップショットの控えの破棄関数
 */
void TSSource::AttachSnapshots::Clear() {
	// 経路段の破棄
	Steps.clear();
	// 錨情報の破棄
	Anchors.clear();
	// 終了
	return;
}

/**
 * 現在の木に紐付く情報だけの消去関数
 * 保留中と停止区間の控えは，再紐付が終わる迄保持する
 */
void TSSource::AttachmentState::ClearNodes() {
	// 先行紐付の破棄
	LeadingByNode.clear();
	// 末尾紐付の破棄
	TrailingByNode.clear();
	// 紐付位置索引の破棄
	AttachedStartBytes.clear();
	// 終了
	return;
}

/**
 * 現在の紐付状態のスナップショット取得関数
 * 再解析後でも再特定可能なパス列で紐付を記録する
 * 計算量：走査節点数 V，錨数 K，照合・複写する字面の総長 B に対し平均 O(V * log(K + 1) + B)
 * @return 紐付対象ノード毎のスナップショット列
 */
TSSource::AttachSnapshots TSSource::SnapshotAttachments() const {
	// 再解析へ渡す控え
	AttachSnapshots Snap;
	// 未解析又は紐付が無い場合の返戻
	if(!IsParsed() || !HasAttachments()) return Snap;
	// 走査中の再確保を避ける紐付節点数上界での予約
	Snap.Anchors.reserve(Attachments.LeadingByNode.size() + Attachments.TrailingByNode.size());
	// 型・名前・同名兄弟番号で安定した経路を保存し，深さ毎のハッシュ集計で二乗照合を回避
	struct SiblingKeyHash {
		/**
		 * （型，名前）鍵のハッシュ算出関数
		 * @param Key 型名と名前のビュー対
		 * @return ハッシュ値
		 */
		size_t operator()(const std::pair<std::string_view, std::string_view> &Key) const {
			// 型と名前のハッシュの合成値の返戻
			return std::hash<std::string_view>()(Key.first) + 31 * std::hash<std::string_view>()(Key.second);
		}
	};
	// 深さ毎の（型，名前）毎の出現数
	using SiblingCounts = std::unordered_map<std::pair<std::string_view, std::string_view>, uint32_t, SiblingKeyHash>;
	// 深さ別の兄弟計数領域
	std::vector<SiblingCounts> CountPool;
	// 紐付開始位置が無い範囲の走査と経路保存の省略
	const auto HoldsAttachment = [this](const TSNode Node) -> bool {
		// 部分木内の先頭候補
		const std::set<uint32_t>::const_iterator Iter = Attachments.AttachedStartBytes.lower_bound(ts_node_start_byte(Node));
		// 部分木の範囲に紐付の開始が有るかの返戻
		return Iter != Attachments.AttachedStartBytes.end() && *Iter < ts_node_end_byte(Node);
	};
	// Leaf は節点の段の添字（根は NoStep）Depth は深さ
	const auto Walk = [&](const TSNode Node, const uint32_t Leaf, const size_t Depth, auto &Self) -> void {
		// 走脈領域を使い切る前の深いネストの新規走脈への移行
		const RecursionGuard Guard;
		if(Guard.IsOverflow) {
			Parallel::RunOnFreshStack(
				[&]() -> void {
					// 遅延処理の実行
					Self(Node, Leaf, Depth, Self);
				}
			);
			// 新しい走脈で記録を終えた事の返戻
			return;
		}
		// 紐付表の節点識別子
		const void *const NodeId = Node.id;
		if(
			const std::unordered_map<const void *, std::vector<CommentAttach>>::const_iterator LIter =
			Attachments.LeadingByNode.find(NodeId), TIter = Attachments.TrailingByNode.find(NodeId);
			LIter != Attachments.LeadingByNode.end() || TIter != Attachments.TrailingByNode.end()
		) {
			// 現在錨の控え
			AttachSnapshot Entry;
			// 経路末端の段番号
			Entry.Leaf = Leaf;
			// 再照合用の節点型
			Entry.NodeType = ts_node_type(Node);
			// 錨原文の開始位置
			const uint32_t AnchorStart = ts_node_start_byte(Node);
			if(
				const std::string_view AnchorText(data() + AnchorStart, ts_node_end_byte(Node) - AnchorStart);
				AnchorText.size() > 1 && AnchorText.back() == ')'
			) {
				// 最後の括弧と内容位置
				const size_t Open = AnchorText.rfind('('), Inner = AnchorText.find_first_not_of(" \t\n", Open + 1);
				if(AnchorText.front() == '(') Entry.Paren = Inner < AnchorText.size() - 1 ? ParenAnchor::Filled : ParenAnchor::Empty;
				else if(Open != std::string_view::npos && Inner == AnchorText.size() - 1) Entry.Paren = ParenAnchor::EmptyCall;
			}
			// 先行紐付の複写
			if(LIter != Attachments.LeadingByNode.end()) Entry.Leading = LIter->second;
			// 末尾紐付の複写
			if(TIter != Attachments.TrailingByNode.end()) Entry.Trailing = TIter->second;
			// 錨控えの追加
			Snap.Anchors.push_back(std::move(Entry));
		}
		// EditPass で相互変換する for_statement / while_statement は合算し，其の他は型・名前毎に兄弟番号を付与
		if(CountPool.size() <= Depth) CountPool.resize(Depth + 1);
		// 現在親の兄弟計数初期化
		CountPool[Depth].clear();
		// 反復合算番号と全子番号
		uint32_t LoopCounter = 0, Ordinal = 0;
		ForEachNamedChild(
			Node,
			[&](const TSNode Child) -> void {
				// 子の型名と名前
				const std::string_view CTypeView(ts_node_type(Child)), CNameView = NodeNameView(Child);
				// 反復以外の兄弟を型・名前毎にハッシュ集計（子再帰で CountPool が伸び参照が失効し得る為，子毎に添字で再取得）
				const uint32_t SubIdx =
				NodeKind::LoopForOrWhile.Contains(CTypeView) ? LoopCounter++ : CountPool[Depth][{ CTypeView, CNameView }]++;
				// 全名前付子内の順番
				const uint32_t ChildOrdinal = Ordinal++;
				// コメントの無い部分木へ降りない事の終了
				if(!HoldsAttachment(Child)) return;
				// 子への経路段の保存
				Snap.Steps.push_back({ CTypeView, std::string(CNameView), SubIdx, ChildOrdinal, Leaf });
				Self(Child, static_cast<uint32_t>(Snap.Steps.size() - 1), Depth + 1, Self);
			}
		);
	};
	Walk(ts_tree_root_node(Syntax.Tree.get()), NoStep, 0, Walk);
	// スナップショットの返戻
	return Snap;
}

/**
 * 警告出力可否の判定関数
 * @return 警告が有効なら true
 */
bool TSSource::AreWarningsEnabled() {
	// 環境変数 `SHAVEFMT_WARNINGS` の存在で切替の返戻（値の中身は問わない）
	return std::getenv("SHAVEFMT_WARNINGS");
}

/**
 * 同一親内の直前の名前付非コメント子取得関数（親を遡らない）
 * @param Node 基準ノード
 * @return 直前の名前付非コメント子（見付からなければ空ノード）
 */
TSNode TSSource::SiblingBeforeImmediate(const TSNode Node) {
	// 根から親を辿り直す ts_node_prev_named_sibling は，二乗走査を避け錨直前の稀な１件照会に限定
	for(TSNode Cur = ts_node_prev_named_sibling(Node); !ts_node_is_null(Cur); Cur = ts_node_prev_named_sibling(Cur)) {
		// コメントでない直前の兄弟の返戻
		if(!NodeKind::Comment.Contains(Cur)) return Cur;
	}
	// 直前の名前付非コメント子（見付からなければ空ノード）の返戻
	return {};
}

/**
 * 前方の兄弟ノード取得関数
 * 同一親内で見付からなければ親へ遡って繰り返す
 * @param Node 基準ノード
 * @return 直前の名前付非コメントノード（見付からなければ空ノード）
 */
TSNode TSSource::SiblingBefore(const TSNode Node) {
	// 現在の節点から親方向への探索
	for(TSNode Cur = Node; !ts_node_is_null(Cur); Cur = ts_node_parent(Cur)) {
		// 見付けた兄弟の返戻
		if(const TSNode Found = SiblingBeforeImmediate(Cur); !ts_node_is_null(Found)) return Found;
	}
	// 根に達した事の空の返戻
	return {};
}

/**
 * スナップショットからの再紐付関数
 * 計算量：索引節点数 V，経路段数 P，錨・コメントの総数 K，代替探索・字面比較の総費用 F に対し平均 O(V + P + K * log(K + 1) + F)
 * @param Snapshot 再解析前に取得したスナップショット列（紐付列は移動で消費される）
 */
void TSSource::RebindFromSnapshot(AttachSnapshots &&Snapshot) {
	// 再束縛前に於ける差替後に無効な旧木節点識別子の破棄
	Attachments.ClearNodes();
	// 再紐付不要な状態の場合の返戻
	if(!IsParsed() || Snapshot.IsEmpty()) return;
	// 再解析後の根節点
	const TSNode NewRoot = ts_tree_root_node(Syntax.Tree.get());
	// for(;;) → while(true) の段照合・兄弟番号の等価判定（混在時は元型優先，其の他は代替経路）
	const auto AreTypesEquivalent = [](const std::string_view Stored, const std::string_view Found) -> bool {
		// 同じ型名は同等の返戻
		if(Stored == Found) return true;
		// 完全一致以外の for / while 相互入替を許す型等価判定の返戻
		return NodeKind::LoopForOrWhile.Contains(Stored) && NodeKind::LoopForOrWhile.Contains(Found);
	};
	// 親毎の子索引を走脈内で再利用し，錨毎の兄弟再走査による二乗化を防止
	struct StepKeyHash {
		/**
		 * （型，名前）鍵のハッシュ算出関数
		 * @param Key 型名と名前のビュー対
		 * @return ハッシュ値
		 */
		size_t operator()(const std::pair<std::string_view, std::string_view> &Key) const {
			// 型と名前のハッシュの合成値の返戻
			return std::hash<std::string_view>()(Key.first) + 31 * std::hash<std::string_view>()(Key.second);
		}
	};
	struct ChildIndex {
		using TypeNameMap = std::unordered_map<std::pair<std::string_view, std::string_view>, std::vector<TSNode>, StepKeyHash>;

		TypeNameMap ByTypeName; // （型，名前）別の出現順の子列
		std::vector<std::pair<TSNode, std::string_view>> LoopPool; // for/while 合算プール
		std::vector<TSNode> Transparents; // 発見失敗時に経由する透過包装子
	};
	using IndexCache = std::unordered_map<const void *, ChildIndex>;
	// Ruby 固有経路の有効化
	const bool IsRubySource = ts_tree_language(Syntax.Tree.get()) == Lang::Get(Lang::Ruby).TsLang();
	const auto GetChildIndex = [this, IsRubySource](const TSNode Parent, IndexCache &Cache) -> const ChildIndex & {
		// 大きな木の未使用部分を除く実際に通る親だけの索引化
		const auto [Iter, Inserted] = Cache.try_emplace(Parent.id);
		if(Inserted) {
			// 親直下の照合用索引構築
			ChildIndex &Index = Iter->second;
			TSNode LastChild {};
			ForEachNamedChild(
				Parent,
				[&](const TSNode Child) -> void {
					// 子を型・名前別又は透過包装の索引へ登録
					const std::string_view CTypeView(ts_node_type(Child)), CNameView = NodeNameView(Child);
					if(NodeKind::TransparentWrap.Contains(CTypeView)) Index.Transparents.push_back(Child);
					if(NodeKind::LoopForOrWhile.Contains(CTypeView)) Index.LoopPool.emplace_back(Child, CNameView);
					else Index.ByTypeName[{ CTypeView, CNameView }].push_back(Child);
					// 節を除く末尾子の記録
					if(IsRubySource && !NodeKind::SectionHeader.Contains(CTypeView)) LastChild = Child;
				}
			);
			// Ruby の末尾 return が包む唯一の式も直下へ登録し，付与前の経路で錨を保持（節は除外）
			if(NamedTypeOf(LastChild) == "return") {
				if(
					const TSNode Arguments = ts_node_named_child(LastChild, 0);
					!ts_node_is_null(Arguments) && std::string_view(ts_node_type(Arguments)) == "argument_list" &&
					ts_node_named_child_count(Arguments) == 1
				) {
					// return が包む唯一の式
					const TSNode Inner = ts_node_named_child(Arguments, 0);
					Index.ByTypeName[{ ts_node_type(Inner), NodeNameView(Inner) }].push_back(Inner);
				}
			}
		}
		// メモ化済の子索引の返戻
		return Iter->second;
	};
	// 追加した塊は中へ再試行し，外した塊・括弧は経路の段を透過
	const auto MatchStep = [&](const TSNode From, const PathStep &Step, IndexCache &Cache, std::vector<TSNode> &Queue) -> TSNode {
		// 保存時と同じ for / while 合算番号での一意照合
		const bool IsLoopStep = NodeKind::LoopForOrWhile.Contains(Step.Type);
		// Swift の組と Ruby の括弧も外れる包装に含め，中身の錨を保持
		const bool IsTransparentStep = NodeKind::TransparentWrap.Contains(Step.Type) || NodeKind::GroupingParen.Contains(Step.Type);
		// 再利用する探索キューの初期内容への再設定
		Queue.clear();
		// 最初の探索親
		Queue.push_back(From);
		while(!Queue.empty()) {
			// 今回調べる親
			const TSNode Here = Queue.back();
			// 取得済候補の除去
			Queue.pop_back();
			// 親直下の子索引
			const ChildIndex &Index = GetChildIndex(Here, Cache);
			// 経路段に一致した子
			TSNode Found {};
			if(IsLoopStep) {
				// 合算プールから名前一致の Step.Index 番目を選ぶ（プールは通常少数の為，線形照合で足りる）
				uint32_t Counter = 0;
				for(const std::pair<TSNode, std::string_view> &Loop : Index.LoopPool) if(Loop.second == Step.Name) {
					if(Counter == Step.Index) {
						// 指定番号の反復子
						Found = Loop.first;
						break;
					}
					// 次の同名反復番号
					++Counter;
				}
			} else if(
				const ChildIndex::TypeNameMap::const_iterator Iter = Index.ByTypeName.find({ Step.Type, std::string_view(Step.Name) });
				Iter != Index.ByTypeName.end() && Step.Index < Iter->second.size()
			) Found = Iter->second[Step.Index];
			// 包装の除去で同型番号が詰まる為，全名前付子内の順番を優先し，別型なら除去済として透過
			if(!ts_node_is_null(Found) && IsTransparentStep && ts_node_eq(Here, From)) {
				if(
					const TSNode AtOrdinal = ts_node_named_child(From, Step.Ordinal);
					!ts_node_is_null(AtOrdinal) && !ts_node_eq(AtOrdinal, Found)
				) {
					// 其の位置の子が別の型なら，外れた包装を素通りする事の返戻
					if(!AreTypesEquivalent(Step.Type, ts_node_type(AtOrdinal))) return From;
					// 全子順を優先した一致節点
					Found = AtOrdinal;
				}
			}
			// 路を辿って見付けた節点の返戻
			if(!ts_node_is_null(Found)) return Found;
			// 直下に無ければ追加された波括弧・幅折返の丸括弧を経由して再試行（括弧除去の透過と対になる１階層の降下）
			for(const TSNode Wrap : Index.Transparents) Queue.push_back(Wrap);
		}
		// 除去され得る透過包装が見付からなければ，其の段を飛ばして照合
		if(IsTransparentStep) return From;
		// 空値又は既定値の返戻
		return {};
	};
	// 独立した控えを並列処理して走脈毎に蓄え，逐次で統合（空の Anchor は未一致扱い）
	struct AnchorResult {
		size_t SnapIdx;
		TSNode Anchor {};
	};
	// 錨数に応じた照合走脈数
	const size_t NumThreads = Parallel::DecideThreads(Snapshot.Anchors.size(), 8);
	// 走脈別の照合結果
	std::vector<std::vector<AnchorResult>> ThreadResults(NumThreads);
	// 独立した錨照合と後段への共有状態更新の集約
	Parallel::ForChunks(
		Snapshot.Anchors.size(),
		NumThreads,
		[&](const size_t Start, const size_t End, const size_t Tid) -> void {
			// 走脈別の結果領域と探索索引の準備
			std::vector<AnchorResult> &Local = ThreadResults[Tid];
			Local.reserve(End - Start);
			std::vector<TSNode> Queue;
			IndexCache Cache;
			// 根側で共有する段の照合を再利用し，錨数×深さの再探索を回避
			struct StepMatch {
				TSNode Node {}; // 其の段迄に一致した最も深い節点
				std::optional<uint32_t> Passed; // 素通りした透過の型の段の順番
				bool IsResolved = false; // 照合済か
				bool IsFailed = false; // 其の段又は根側の段を見付けれなかったか
				bool IsLost = false; // 其の段自身を見失ったか
			};
			// 経路段別の照合状態と未照合段列
			std::vector<StepMatch> Matches(Snapshot.Steps.size());
			std::vector<uint32_t> Chain;
			for(size_t Idx = Start; Idx < End; ++Idx) {
				// 錨から根側へ未照合経路を収集
				const AttachSnapshot &Snap = Snapshot.Anchors[Idx];
				Chain.clear();
				for(uint32_t Step = Snap.Leaf; Step != NoStep && !Matches[Step].IsResolved; Step = Snapshot.Steps[Step].Parent) {
					// 未照合段の追加
					Chain.push_back(Step);
				}
				for(size_t Remaining = Chain.size(); Remaining--;) {
					// 経路段と親段から探索条件を復元
					const PathStep &Step = Snapshot.Steps[Chain[Remaining]];
					StepMatch &Match = Matches[Chain[Remaining]];
					const StepMatch *const Above = Step.Parent == NoStep ? nullptr : &Matches[Step.Parent];
					const TSNode Cur = Above ? Above->Node : NewRoot;
					const std::optional<uint32_t> Passed = Above ? Above->Passed : std::nullopt;
					// 照合状態を現在の親節点で初期化
					Match.IsResolved = true;
					Match.Node = Cur;
					if(Above && Above->IsFailed) {
						// 親段失敗の引継
						Match.IsFailed = true;
						continue;
					}
					// 透過前の全子順又は型名経路による段照合
					TSNode Found {};
					if(Passed) {
						if(
							const TSNode At = ts_node_named_child(Cur, *Passed);
							!ts_node_is_null(At) && AreTypesEquivalent(Step.Type, ts_node_type(At))
							// 透過前の全子順による一致
						) Found = At;
					}
					if(ts_node_is_null(Found)) Found = MatchStep(Cur, Step, Cache, Queue);
					// 段の消失又は透過状態の記録
					const bool IsPassedThrough = !ts_node_is_null(Found) && ts_node_eq(Found, Cur);
					Match.IsLost = ts_node_is_null(Found) || IsPassedThrough;
					if(ts_node_is_null(Found)) {
						// 此の段以降の照合停止
						Match.IsFailed = true;
						continue;
					}
					// 一致節点を次段の探索親へ引継
					Match.Node = Found;
					if(IsPassedThrough) Match.Passed = Passed ? Passed : Step.Ordinal;
				}
				// 錨段から最終候補の照合状態を取得
				const StepMatch *const Leaf = Snap.Leaf == NoStep ? nullptr : &Matches[Snap.Leaf];
				const TSNode Cur = Leaf ? Leaf->Node : NewRoot;
				const bool IsPathMatched = !Leaf || !Leaf->IsFailed, IsLastLost = Leaf && Leaf->IsLost;
				const char *const Type = IsPathMatched ? ts_node_type(Cur) : nullptr;
				// 経路の照合で得た節点は Matched 系で名付け，走査中の節点 (Cur) と作り直した根 (NewRoot) と役割を分ける
				TSNode Matched = IsPathMatched && Type && AreTypesEquivalent(Snap.NodeType, Type) ? Cur : TSNode{};
				// 外れた包装の錨を中身へ移し，空括弧だけ直前要素へ，後続節の無い最終塊だけ親へ退避
				if(
					ts_node_is_null(Matched) && IsLastLost && (
						Snap.Paren != ParenAnchor::None || NodeKind::TransparentWrap.Contains(Snap.NodeType) &&
						!ts_node_is_null(ts_node_named_child(Cur, Snapshot.Steps[Snap.Leaf].Ordinal + 1))
					)
				) {
					// 消えた錨の全子順
					const uint32_t Ordinal = Snapshot.Steps[Snap.Leaf].Ordinal;
					Matched = Snap.Paren != ParenAnchor::Empty ?
					ts_node_named_child(Cur, Ordinal) :
					Ordinal ? ts_node_named_child(Cur, Ordinal - 1) : SiblingBefore(Cur);
				}
				// 消えた錨は最後に一致した最も近い祖先（if 等）へ退避（波括弧除去・宣言統合でも著者の記述を破棄しない）
				if(ts_node_is_null(Matched) && !ts_node_eq(Cur, NewRoot)) Matched = Cur;
				// 逐次統合用の照合結果
				Local.push_back({ Idx, Matched });
			}
		}
	);
	// 実行中不変な警告可否の統合ループ外での単一評価
	const bool ShouldWarn = AreWarningsEnabled();
	// 走脈番号順に統合し，同一錨へ集まるコメントの順序を安定させる
	for(const std::vector<AnchorResult> &Local : ThreadResults) for(const AnchorResult &Res : Local) {
		// 一致エントリのコメント列は以降未参照の為，移動で取込複写を省く（未一致エントリは警告分岐で読む為，不変）
		AttachSnapshot &Snap = Snapshot.Anchors[Res.SnapIdx];
		if(!ts_node_is_null(Res.Anchor)) {
			if(!Snap.Leading.empty()) {
				// 新しい先行紐付列
				std::vector<CommentAttach> &Dest = Attachments.LeadingByNode[Res.Anchor.id];
				Dest.insert(Dest.end(), std::make_move_iterator(Snap.Leading.begin()), std::make_move_iterator(Snap.Leading.end()));
			}
			if(!Snap.Trailing.empty()) {
				// 新しい末尾紐付列
				std::vector<CommentAttach> &Dest = Attachments.TrailingByNode[Res.Anchor.id];
				Dest.insert(Dest.end(), std::make_move_iterator(Snap.Trailing.begin()), std::make_move_iterator(Snap.Trailing.end()));
			}
			// 紐付開始位置の索引更新
			Attachments.AttachedStartBytes.insert(ts_node_start_byte(Res.Anchor));
			continue;
		}
		if(ShouldWarn) {
			// 警告へ載せる先頭コメント
			std::string Preview =
			!Snap.Leading.empty() ? Snap.Leading[0].Text : !Snap.Trailing.empty() ? Snap.Trailing[0].Text : std::string();
			// 警告字面の上限適用
			if(Preview.size() > WarnPreviewMax) Preview = Preview.substr(0, WarnPreviewMax) + "...";
			// 静的 NUL 終端の NodeType は直接使用し，利用者のコメント本文は端末制御列を無害化
			fprintf(
				stderr,
				"[warn] comment dropped on reparse (anchor '%s' not found, %zu comments): %s\n",
				Snap.NodeType.data(),
				Snap.Leading.size() + Snap.Trailing.size(),
				FileIO::ForDisplay(Preview).c_str()
			);
		}
	}
	// 終了
	return;
}

/**
 * 葉の字句の長さの和の算出関数
 * 字句の間の空白を含まない為，同じ構文木なら入力の字下げに依らず同じ値に為る
 * 構造化の並列の区間から数えると走脈が重なり，木を辿る手間も走脈の数だけ掛かる為，木を差し替えた時に一度だけ数える
 * 計算量：構文木の節点数 V に対し O(V)（葉を一度だけ訪れる）
 */
void TSSource::CountTokenBytes() {
	size_t Bytes = 0;
	if(Syntax.Tree) {
		// 空白に影響されない作業量としての葉字句幅の積算
		WalkAst(
			ts_tree_root_node(Syntax.Tree.get()),
			[&Bytes](const TSNode Cur) -> void {
				// 葉字句の長さの加算
				if(!ts_node_child_count(Cur)) Bytes += ts_node_end_byte(Cur) - ts_node_start_byte(Cur);
			}
		);
	}
	// 木世代に対応する作業量基準
	Syntax.TokenBytes = Bytes;
	// 終了
	return;
}

/**
 * 構文木と其の控えの差替関数
 * @param NewTree 所有権を引き取る構文木（解析失敗時は nullptr）
 */
void TSSource::ReplaceTree(TSTree *const NewTree) {
	// 派生情報を同じ世代へ揃える構文木所有権の先行移動
	Syntax.Tree.reset(NewTree);
	// 本文同期状態の確定と旧根キャッシュの無効化
	Syntax.IsDirty = false;
	Syntax.HasCachedRoot = false;
	// 木世代に依る作業量・字句量・節点文脈の更新
	Syntax.FormatWork.Value.store(0, std::memory_order_relaxed);
	CountTokenBytes();
	Syntax.NodeContexts.clear();
	// 終了
	return;
}

/**
 * 構文木の再解析関数
 */
void TSSource::Reparse() {
	// パーサを持たない場合の返戻
	if(!Syntax.Parser) return;
	// 既存の控えを優先し，停止区間外だけ旧木解放前に退避（停止中は Node.id が無効）
	if(!Attachments.SuspendDepth && Attachments.PendingSnapshot.IsEmpty() && HasAttachments()) {
		Attachments.PendingSnapshot = SnapshotAttachments();
	}
	// 再紐付用控えの移動と保存先の初期化
	AttachSnapshots Snap = std::move(Attachments.PendingSnapshot);
	Attachments.PendingSnapshot.Clear();
	// 局所編集の増分解析と `ERROR` 時の全体解析への退避
	if(size() > std::numeric_limits<uint32_t>::max()) throw std::length_error("source exceeds the parser's position range");
	// 解析期限状態の初期化と増分解析
	Syntax.IsParseExpired = false;
	TSTree *NewTree =
	ParseWithDeadline(Syntax.Parser.get(), Syntax.IsTreeEdited ? Syntax.Tree.get() : nullptr, *this, Syntax.IsParseExpired);
	if(Syntax.IsTreeEdited && NewTree && ts_node_has_error(ts_tree_root_node(NewTree))) {
		// 増分位置の不整合時の木破棄と全体再解析
		ts_tree_delete(NewTree);
		NewTree = ParseWithDeadline(Syntax.Parser.get(), nullptr, *this, Syntax.IsParseExpired);
	}
	// 増分編集通知と期限超過の確定後に木を差替
	Syntax.IsTreeEdited = false;
	if(!NewTree && Syntax.IsParseExpired) throw FormatLimitExceeded("parsing exceeds the time limit");
	ReplaceTree(NewTree);
	// Suspend 中の再紐付の省略と ResumeAttachments での一括復元
	if(!Attachments.SuspendDepth) RebindFromSnapshot(std::move(Snap));
	// 終了
	return;
}

/**
 * 最新構文木のスナップショット取得関数
 * @return 最新構文木に対する紐付スナップショット列
 */
TSSource::AttachSnapshots TSSource::FreshSnapshot() {
	// 未反映編集の再解析後の取得
	if(Syntax.Parser && Syntax.IsDirty) Reparse();
	// 最新構文木に対するスナップショットの返戻
	return SnapshotAttachments();
}

/**
 * 代入直前のスナップショット保存関数
 */
void TSSource::PrepareAssign() {
	// 本文・木の変更前に錨経路を保存（停止中と確定済の控えは再取得せず，旧本文の誤参照を防止）
	if(!Attachments.SuspendDepth && Attachments.PendingSnapshot.IsEmpty() && HasAttachments()) {
		Attachments.PendingSnapshot = FreshSnapshot();
	}
	// 終了
	return;
}

/**
 * 可変構文木取得関数
 * 呼出側は得た木を `ts_tree_edit` で編集後のバイト位置へ動かす為，動かす前に紐付のスナップショットを確定させる
 * 動かした後の木で未だ差替前のソースを読むと，経路の名前が別の範囲を指し再紐付が錨を見失う
 * @return 内部の構文木（解析前は nullptr）
 */
TSTree *TSSource::GetMutableTree() {
	// 変更前の紐付状態の保存
	if(Syntax.Tree) PrepareAssign();
	// スナップショット取得の過程で再解析が走ると木が入れ替わる為，取得後の木の返戻
	return Syntax.Tree.get();
}

/**
 * 構文木編集済の設定関数
 * 編集済なら次の再解析で旧構文木を土台に増分解析し，増分通知を諦めた適用の後は全体解析へ倒す
 * @param IsEdited 旧構文木へ編集を通知したら true
 */
void TSSource::SetTreeEdited(const bool IsEdited) {
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
uint32_t TSSource::End(const TSNode Node) const {
	// 空節点を含む終了位置の返戻
	return ts_node_is_null(Node) ? ts_node_start_byte(Node) : ts_node_end_byte(Node);
}

/**
 * ノードの開始バイト取得関数
 * @param Node 対象ノード
 * @return 開始バイト位置
 */
uint32_t TSSource::Start(const TSNode Node) const {
	// 開始位置の返戻
	return ts_node_start_byte(Node);
}

/**
 * ノード範囲のバイト長取得関数
 * @param Node 対象ノード
 * @return 終了バイトと開始バイトの差
 */
uint32_t TSSource::Len(const TSNode Node) const {
	// 節点範囲の長さの返戻
	return End(Node) - Start(Node);
}

/**
 * ノード範囲のビュー取得関数
 * @param Node 対象ノード
 * @return ノード範囲への複写無のビュー
 */
std::string_view TSSource::View(const TSNode Node) const {
	// 節点の原文範囲
	const uint32_t Begin = Start(Node), End = this->End(Node);
	// 範囲が有効ならビュー，空なら空ビューの返戻
	return Begin < End ? std::string_view(data() + Begin, End - Begin) : std::string_view();
}

/** ========== 字句・文脈判定 ========== */
/**
 * 文へ展開するマクロの呼出の文かの判定関数
 * 置換本体が複数の文や開いた if に為るマクロ (`#define SWAP(a, b) t = a; a = b; b = t`) の呼出は，単文の本体に置くと先頭の文だけが本体に為る
 * @param Stmt 判定する文
 * @return 文へ展開するマクロ（CollectMacros で集めた名前）の呼出だけの式文なら true
 */
bool TSSource::ExpandsToStatements(const TSNode Stmt) const {
	// 対象のマクロが無い場合と，式文でない場合の返戻
	if(StatementMacros.empty() || std::string_view(ts_node_type(Stmt)) != "expression_statement") return false;
	// 式文の先頭式
	TSNode Name = ts_node_named_child(Stmt, 0);
	if(!ts_node_is_null(Name) && std::string_view(ts_node_type(Name)) == "call_expression") Name = FieldChild(Name, "function");
	// マクロの名前かの返戻
	return !ts_node_is_null(Name) && std::string_view(ts_node_type(Name)) == "identifier" &&
	StatementMacros.contains(std::string(View(Name)));
}

/**
 * 字面が名前の何れかを含むかの判定関数
 * 計算量：字面の長さ N に対し平均 O(N)
 * @param Text 判定する字面
 * @param Names 名前の集合
 * @return 識別子として名前の何れかを含めば true
 */
bool TSSource::MentionsName(const std::string_view Text, const std::unordered_set<std::string> &Names) {
	// 識別子候補の先頭からの走査
	for(size_t Pos = 0; Pos < Text.size();) {
		// 識別子外のバイトの読飛し
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
bool TSSource::MentionsDeclaratorMacro(const TSNode Type) const {
	// 対象のマクロの名前を含むかの返戻
	return !DeclaratorMacros.empty() && MentionsName(View(Type), DeclaratorMacros);
}

/**
 * 括弧で包まずに演算子を含む式へ展開するマクロを含むかの判定関数
 * 括弧無の式へ展開するマクロは周囲の括弧除去で結合が変わる
 * @param Node 判定するノード
 * @return 字面に CollectMacros で集めた名前が有れば true
 */
bool TSSource::MentionsExpressionMacro(const TSNode Node) const {
	// 対象のマクロの名前を含むかの返戻
	return !ExpressionMacros.empty() && MentionsName(View(Node), ExpressionMacros);
}

/**
 * 名前のハッシュ算出関数
 * 計算量：名前の長さ N に対し O(N)
 * @param Name 所有する名前又は本文から得た名前のビュー
 * @return 名前の字面に対応するハッシュ値
 */
size_t TSSource::NameHash::operator()(const std::string_view Name) const noexcept {
	// 文字列とビューの照会を同じハッシュで扱う為の返戻
	return std::hash<std::string_view>()(Name);
}

/**
 * 同じファイルで定義した関数形式マクロ（別名を含む）の名前かの判定関数
 * @param Name 判定する名前
 * @return CollectMacros で集めた関数形式マクロか其の別名なら true
 */
bool TSSource::IsFunctionMacro(const std::string_view Name) const {
	// 関数形式マクロの名前かの返戻
	return !FunctionMacros.empty() && FunctionMacros.contains(Name);
}

/**
 * 使う位置で有効な宣言を同じファイルに持つ名前かの判定関数
 * 計算量：名前の長さ N，同名の宣言数 D，取込数 I に対し平均 O(N + log(D + 1) + log(I + 1))
 * 最後の宣言と使用位置の間に取込指令が無い名前だけを有効と看做す
 * @param Name 名前
 * @param Use 使う位置
 * @return 使う位置より前に変数・仮引数・関数・列挙子として宣言し，其の後に取込の指令の無い名前なら true
 */
bool TSSource::IsDeclaredName(const std::string_view Name, const uint32_t Use) const {
	// 同名宣言の位置列
	const decltype(DeclaredNames)::const_iterator Found = DeclaredNames.find(Name);
	// 宣言の無い名前の返戻
	if(Found == DeclaredNames.end()) return false;
	// 使用位置以後の宣言
	const std::vector<uint32_t>::const_iterator Declared = std::lower_bound(Found->second.begin(), Found->second.end(), Use);
	// 使う位置より前に宣言の無い名前の返戻
	if(Declared == Found->second.begin()) return false;
	const std::vector<uint32_t>::const_iterator Include =
	std::upper_bound(IncludeStarts.begin(), IncludeStarts.end(), *(Declared - 1));
	// 宣言と使う位置の間に取込の指令が無いかの返戻
	return Include == IncludeStarts.end() || *Include > Use;
}

/**
 * CSS の `url(...)` の引数の並びかの判定関数（関数名の大小は CSS 仕様上不問）
 * @param Node 判定対象のノード
 * @return `url` 呼出の引数の並びなら true
 */
bool TSSource::IsCssUrlArguments(const TSNode Node) const {
	// 別の名前・成員呼出を除く url の前置区切り
	static constexpr std::string_view UrlName = "url", NameDelimiters = ":(,;{} \t\n";
	// 関数名は構文木でなく開き括弧直前の字面で見る（SCSS のマップ記法では関数名ノードが `ERROR` へ崩れ木からは辿れない）
	const uint32_t Start = ts_node_start_byte(Node);
	// 引数の並びでない場合と，開き括弧の前に関数名の長さが取れない場合の返戻
	if(std::string_view(ts_node_type(Node)) != "arguments" || Start < UrlName.size() || Start > size()) return false;
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
 * 祖先で決まる節点毎の文脈の控えの取得関数
 * 祖先依存の文脈を節点毎に控えて再走査を避ける
 * @return 節点の識別子から文脈への表（構文木の差替で空に戻る）
 */
std::unordered_map<const void *, uint8_t> &TSSource::GetContextMemo() const {
	// 控えの表の返戻
	return Syntax.NodeContexts;
}

/**
 * 組み立てた字面の量の加算と上限の検査関数
 * 計算量：O(1)（原子的な加算と比較だけ，字句の総量は木を差し替えた時に数えて控える）
 * @param Amount 今回組み立てた字面のバイト数
 * @return 上限内なら true
 */
bool TSSource::AddFormatWork(const size_t Amount) const {
	// 固定費と１バイト当たりの組立費用
	static constexpr size_t WorkBudget = 0X80000, WorkPerByteShift = 5;
	const size_t Cap = std::max(WorkBudget, Syntax.TokenBytes << WorkPerByteShift);
	// 加算後の作業量が上限内かの返戻
	return Syntax.FormatWork.Value.fetch_add(Amount, std::memory_order_relaxed) + Amount <= Cap;
}

/**
 * 組み立てた字面の量の上限超過判定関数
 * @return 上限を超えていれば true
 */
bool TSSource::HasExceededFormatWork() const {
	// 固定費と１バイト当たりの組立費用
	static constexpr size_t WorkBudget = 0X80000, WorkPerByteShift = 5;
	const size_t Cap = std::max(WorkBudget, Syntax.TokenBytes << WorkPerByteShift);
	// 現在の作業量判定の返戻
	return Syntax.FormatWork.Value.load(std::memory_order_relaxed) > Cap;
}

/**
 * Ruby の符号を密着させても読みが変わらない被演算子かの判定関数
 * @param Operand 符号の被演算子
 * @return 密着させても読みが変わらなければ true
 */
bool TSSource::AttachesRubySign(const TSNode Operand) const {
	// 数字で始まる被演算子の検査
	if(ts_node_is_null(Operand) || Start(Operand) >= size() || !std::isdigit(static_cast<unsigned char>(at(Start(Operand))))) {
		// 空節点と数字で始まらない被演算子の返戻（空節点は位置も親も引けない）
		return true;
	}
	// 空節点の親は引けない為，先に確かめてから外側を辿る
	if(
		const TSNode Unary = ts_node_parent(Operand), Outer = ts_node_is_null(Unary) ? TSNode{} : ts_node_parent(Unary);
		!ts_node_is_null(Unary) && std::string_view(ts_node_type(Unary)) == "unary" && Start(Unary) + 1 < Start(Operand) &&
		NodeKind::PostfixReceiverChain.Contains(Outer) && ts_node_eq(ts_node_named_child(Outer, 0), Unary)
		// 後置の受け手と読まれた離れた符号の返戻
	) return false;
	// 符号直下の数値底
	const TSNode Base = std::string_view(ts_node_type(Operand)) == "binary" && View(FieldChild(Operand, "operator")) == "**" ?
	ts_node_named_child(Operand, 0) :
	Operand;
	// 数値底の節点型
	const std::string_view BaseType = ts_node_type(Base);
	// 数値だけか，数値を底とする冪かの返戻
	return NodeKind::NumberLiteral.Contains(BaseType) || NodeKind::RubyNumericLiteralSuffix.Contains(BaseType);
}

/**
 * ルートノード取得関数
 * @return ルートノード
 */
TSNode TSSource::GetRoot() const {
	// 未反映編集が有る場合の再解析
	if(Syntax.IsDirty) const_cast<TSSource *>(this)->Reparse();
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
 * JSX の行内空白（TypeScript の空白の集合）かの判定関数
 * @param Point 符号位置
 * @return TypeScript が JSX 本文の行端で落とす空白なら true
 */
bool TSSource::IsJsxSpace(const uint32_t Point) {
	// 行内空白かの返戻
	return Point == ' ' || Point == '\t' || Point == '\v' || Point == '\f' || Point == 0XA0 || Point == 0X85 || Point == 0X1680 ||
	Point > 0X1FFF && Point < 0X200C || Point == 0X202F || Point == 0X205F || Point == 0X3000 || Point == 0XFEFF;
}

/**
 * 空白・タブへ復号される数値文字参照の長さと其の文字の取得関数
 * 計算量：Pos 以降の長さ N に対し O(N)
 * @param Text 本文
 * @param Pos 判定する位置
 * @return Pos から始まる `&#32;` / `&#x20;` / `&#9;` 等の長さと復号した文字（該当しなければ長さ 0）
 */
std::pair<size_t, char> TSSource::BlankReference(const std::string_view Text, const size_t Pos) {
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
bool TSSource::HasDisputedJsxLineEdges(const std::string_view Text) {
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
				if(
					const size_t Amp = Before == ';' ? Text.rfind('&', Lead) : std::string_view::npos;
					IsJsxSpace(Before) || Amp != std::string_view::npos && BlankReference(Text, Amp).first == Left - Amp
					// 行末に在る半角空白・タブ以外の空白又は空白の文字参照の返戻
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
			if(TextEdit::DecodeUtf8(Text, After, Head) && IsJsxSpace(Head) || BlankReference(Text, Right).first) return true;
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
 * @param Text ソースコード
 * @param Container JSX の要素
 * @return 行端の扱いが分かれる本文を持つなら true
 */
bool TSSource::HasDisputedJsxText(const std::string_view Text, const TSNode Container) {
	// 次に調べる本文隙間の開始
	uint32_t Gap = ts_node_start_byte(Container);
	// 処理系差の検出結果
	bool IsDisputed = false;
	ForEachNamedChild(
		Container,
		[&](const TSNode Child) -> bool {
			// 本文と文字参照は字面で読む為，次の子へ進む事の返戻
			if(NodeKind::JsxTextLike.Contains(Child)) return true;
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
 * ノード範囲の文字列取得関数
 * @param Node 対象ノード
 * @return ノード範囲のテキスト（新規複写）
 */
std::string TSSource::Text(const TSNode Node) const {
	// ノード範囲のビューを複写して返戻
	return std::string(View(Node));
}

/**
 * JSX の本文の端の空白・タブを保つ印の取得関数
 * @param Blank 半角空白又はタブ
 * @return 印の字面（私用領域の文字を選べなかった場合は文字参照）
 */
std::string_view TSSource::GetJsxBlankMark(const char Blank) const {
	// 半角空白又はタブの印の返戻（原文が私用領域の文字を使い切る場合は，TypeScript の読みを保つ文字参照の返戻）
	if(Blank == ' ') return JsxSpaceMark.empty() ? "&#32;" : std::string_view(JsxSpaceMark);
	// タブの印の返戻
	return JsxTabMark.empty() ? "&#9;" : std::string_view(JsxTabMark);
}

/**
 * 実引数を字面の儘保つ呼出かの判定関数
 * 計算量：引数の子節点数 C，呼出名と直前の空白の長さ N に対し平均 O(C + N)
 * @param List 判定する括弧の並び（実引数列・仮引数列）
 * @return 実引数を文字列化する関数形式マクロ（CollectMacros で集めた名前）の実引数の並びか，波括弧の塊を実引数に持つ並びなら true
 */
bool TSSource::StringizesArguments(const TSNode List) const {
	// 括弧の並びでない場合の返戻
	if(!NodeKind::MultilineCommaContainer.Contains(List)) return false;
	if(
		HasChildOf(
			List,
			[](const TSNode Child) -> bool {
				// 波括弧の塊かの返戻
				return std::string_view(ts_node_type(Child)) == "compound_statement";
			}
		)
		// 波括弧の塊を実引数に持つ場合の返戻
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
 * 本文差替と未反映状態の共通更新関数
 * @param NewSource 新しいソース文字列（右辺値なら所有権を移す）
 */
template<typename String> void TSSource::AssignSource(String &&NewSource) {
	// 本文が同一の場合の返戻
	if(NewSource == static_cast<const std::string &>(*this)) return;
	const bool HasTree = Syntax.Tree != nullptr;
	if(HasTree) PrepareAssign();
	// 木の錨の退避後に於ける本文の差替
	std::string::operator=(std::forward<String>(NewSource));
	if(HasTree) Syntax.IsDirty = true;
	// 終了
	return;
}

/**
 * ソース複製代入関数
 * @param NewSource 新しいソース文字列
 */
void TSSource::Assign(const std::string &NewSource) {
	// 複製本文の共通代入処理
	AssignSource(NewSource);
	// 終了
	return;
}

/**
 * ソース移動代入関数
 * @param NewSource 新しいソース文字列（ムーブ済）
 */
void TSSource::Assign(std::string &&NewSource) {
	// 移動本文の共通代入処理
	AssignSource(std::move(NewSource));
	// 終了
	return;
}

/** ========== コメント紐付 ========== */
/**
 * コメント紐付関数
 * コメントノードを直前／直後の名前付非コメントノードに紐付ける
 * 計算量：原稿長 N，節点数 V，コメント数 K，錨判定・本文正規化・再解析の総費用 A に対し平均 O(N + V + K * log(K + 1) + A)
 * @param IsJsxSource JSX 本文の行端空白を分離前に確定するか
 * @param IsScssSource CSS の文法で読む入力が SCSS か（Sass はコメントを空白と読む）
 * @param DocMarker Ruby の埋込ドキュメントの本文の `=end` へ解析の為に挟んだ印（挟んで居なければ '\0'）
 */
void TSSource::AttachComments(const bool IsJsxSource, const bool IsScssSource, const char DocMarker) {
	// 未解析原稿の場合の返戻
	if(!IsParsed()) return;
	// コメント包装分離前の JSX 本文行端空白の解釈確定
	if(IsJsxSource) {
		// 編集中も参照する原文
		const TSSource &Src = *this;
		// 原文に無い私用領域の１文字を空白・タブの印に使い，利用者の参照と区別して幅を保持
		JsxSpaceMark.clear();
		JsxTabMark.clear();
		for(uint32_t Point = 0XE000; Point < 0XF900 && JsxTabMark.empty(); ++Point) {
			std::string Mark;
			TextEdit::EncodeUtf8(Mark, Point);
			if(find(Mark) == npos) (JsxSpaceMark.empty() ? JsxSpaceMark : JsxTabMark) = std::move(Mark);
		}
		std::vector<TextEdit> Edits;
		WalkChildrenCursor(
			GetRoot(),
			[&](const TSNode Node) -> bool {
				// 配置保持対象の JSX 要素の選別
				const std::string_view TypeView = ts_node_type(Node);
				// 行端の描画が処理系で異なる要素は配置を保持（ネストは個別判定）
				if(NodeKind::JsxContainer.Contains(TypeView) && !HasDisputedJsxText(Src, Node)) {
					// 本文の並び（空白１文字の式を含む）の範囲と，其の内の式を挟まない本文の範囲
					uint32_t Begin = 0, End = 0, TextBegin = 0, TextEnd = 0;
					std::string Normalized;
					const auto ReadPoint = [](const std::string_view Value, const size_t Pos) -> std::pair<uint32_t, size_t> {
						// 対象位置の処理
						size_t Next = Pos;
						uint32_t Point = 0XFFFD;
						// 復号出来ないバイトは UTF-8 の復号器が置換文字に換える為，空白に数えない
						if(!TextEdit::DecodeUtf8(Value, Next, Point)) Next = Pos + 1;
						// 符号位置とバイト幅の返戻
						return std::pair{ Point, Next - Pos };
					};
					// 式を挟まない本文１つの表示内容を追記（本文毎に行端の空白を落とす）
					const auto AppendText = [&]() -> void {
						// 本文が無い場合の追記の省略
						if(TextBegin == TextEnd) return;
						// 今回の本文範囲
						const std::string_view Text(Src.data() + TextBegin, TextEnd - TextBegin);
						// 表示行毎の有効本文範囲の初期化
						size_t First = 0, Last = 0;
						bool HasText = false, HasLine = false;
						// 改行境界毎の端空白除去と本文連結
						for(size_t Pos = 0; Pos < Text.size();) {
							const auto [Point, Width] = ReadPoint(Text, Pos);
							if(const bool IsLineBreak = Point == '\n' || Point == '\r' || Point == 0X2028 || Point == 0X2029; IsLineBreak) {
								// 内容を持つ行の正規化本文への確定
								if(First != std::string_view::npos && HasText) {
									if(HasLine) Normalized += ' ';
									Normalized.append(Text.substr(First, Last - First));
									HasLine = true;
								}
								First = std::string_view::npos;
								HasText = false;
							} else if(!IsJsxSpace(Point)) {
								// 非空白符号を含む本文範囲の更新
								if(First == std::string_view::npos) First = Pos;
								Last = Pos + Width;
								HasText = true;
							}
							Pos += Width;
						}
						// 改行で終わらない最終行の確定
						if(First != std::string_view::npos) {
							if(HasLine) Normalized += ' ';
							Normalized.append(Text.substr(First));
						}
						// 確定済本文範囲の初期化
						TextBegin = TextEnd = 0;
					};
					// 吸収した式のコメント（本文の中に置く所が無い為，JSX の外へ退避する）
					std::string Moved, MovedLines;
					// 空白・タブは印から改行境界毎に復元し，其の他は両処理系が保持する文字参照化
					const auto FlushText = [&]() -> void {
						// 遅延処理の実行
						AppendText();
						// 本文の並びが無ければ編集せず終了
						if(Begin == End) return;
						if(!Normalized.empty()) {
							// 利用者の端の空白参照も同じ印へ揃え，再整形時の幅を保持
							if(const size_t Amp = Normalized.rfind('&'); Amp != std::string::npos) {
								if(const auto [Length, Blank] = BlankReference(Normalized, Amp); Length && Amp + Length == Normalized.size()) {
									Normalized.replace(Amp, Length, GetJsxBlankMark(Blank));
								}
							}
							if(const auto [Length, Blank] = BlankReference(Normalized, 0); Length) Normalized.replace(0, Length, GetJsxBlankMark(Blank));
							// UTF-8 末尾符号の開始位置探索
							size_t Tail = Normalized.size() - 1;
							while(Tail && (static_cast<unsigned char>(Normalized[Tail]) & 0XC0) == 0X80) --Tail;
							// 先頭・末尾の空白符号の安定した表現への置換
							for(const size_t Pos : { Tail, size_t(0) }) {
								const auto [Point, Width] = ReadPoint(Normalized, Pos);
								if(Point == ' ' || Point == '\t') Normalized.replace(Pos, Width, GetJsxBlankMark(static_cast<char>(Point)));
								else if(IsJsxSpace(Point)) Normalized.replace(Pos, Width, "&#" + std::to_string(Point) + ";");
							}
						}
						// 原文と異なる正規化範囲の編集登録
						if(Normalized != std::string_view(Src.data() + Begin, End - Begin)) TextEdit::Push(Begin, End, std::move(Normalized), Edits);
						// 次の本文並びに備えたバッファ初期化
						Normalized.clear();
						// 並び範囲の初期化
						Begin = End = 0;
					};
					// 半角空白だけの文字列の式 (`{" "}` / `{"  "}`) の空白の数（其れ以外の子は 0）
					const auto SpaceExpressionWidth = [&Src](const TSNode Child) -> size_t {
						// 式でない子の返戻
						if(std::string_view(ts_node_type(Child)) != "jsx_expression") return 0;
						// 唯一の名前付非コメント子が半角空白だけの文字列か判定（タブは描画が異なる為に除外）
						uint32_t Values = 0;
						size_t Width = 0;
						// 名前付の値からコメントを除いて空白文字列幅を収集
						ForEachNamedChild(
							Child,
							[&](const TSNode Value) -> void {
								// JSX 式内の非コメント値の収集
								if(NodeKind::Comment.Contains(Value)) return;
								++Values;
								// 値の原文字列
								const std::string_view Text = Src.View(Value);
								// 値の引用文字列条件
								const bool IsQuoted = Text.size() > 2 && (Text.front() == '"' || Text.front() == '\'') && Text.back() == Text.front();
								Width = IsQuoted && Text.find_first_not_of(' ', 1) == Text.size() - 1 ? Text.size() - 2 : 0;
							}
						);
						// 空白だけの文字列を唯一の値に持つ式の空白の数の返戻
						return Values == 1 ? Width : 0;
					};
					// 改行を含む空白だけの隙間も置換範囲へ含め，非表示の改行の有無で出力を変えない
					const auto BlankGap = [&Src](const uint32_t From, const uint32_t To) -> bool {
						// 子間の空白だけの隙間の判定
						const std::string_view Gap(Src.data() + From, To - From);
						// 改行を含む空白だけの隙間かの返戻
						return From && From < To && Gap.find_first_not_of(" \t\r\n") == std::string_view::npos &&
						Gap.find('\n') != std::string_view::npos;
					};
					uint32_t PrevEnd = 0;
					bool IsTrailingSpace = false;
					// 文字参照と空白の式 (`{" "}`) は隣接本文と同じ文字列として扱い，式・要素の境界毎に JSX の行端空白を確定
					ForEachNamedChild(
						Node,
						[&](const TSNode Child) -> void {
							// JSX 子の幅と先行範囲の取得
							const bool IsText = NodeKind::JsxTextLike.Contains(ts_node_type(Child));
							const size_t Width = IsText ? 0 : SpaceExpressionWidth(Child);
							const uint32_t ChildStart = Src.Start(Child), Before = PrevEnd;
							PrevEnd = Src.End(Child);
							// 表示本文でない子に於ける現在範囲の確定
							if(!IsText && !Width) {
								if(IsTrailingSpace && BlankGap(End, ChildStart)) End = ChildStart;
								FlushText();
								// 本文の並びの境界の終了
								return;
							}
							// 隣接本文又は空白式を含む範囲の拡張
							if(Begin == End) Begin = Width && BlankGap(Before, ChildStart) ? Before : ChildStart;
							End = PrevEnd;
							IsTrailingSpace = Width;
							// 本文と空白式の各蓄積先への振分
							if(IsText) {
								if(TextBegin == TextEnd) TextBegin = Src.Start(Child);
								TextEnd = End;
							} else {
								// 前の本文確定後に於ける式の空白の逐語追加
								AppendText();
								Normalized.append(Width, ' ');
								ForEachNamedChild(
									Child,
									[&](const TSNode Comment) -> void {
										// JSX 子内コメントの再配置
										if(!NodeKind::Comment.Contains(Comment)) return;
										// 行末迄続くコメントは所有文の前へ，其の他は要素の前へ移動
										if(const std::string_view Text = Src.View(Comment); Text.starts_with("//") || Text.find('\n') != std::string_view::npos) {
											// 所有文前の退避列への追加
											MovedLines.append(Text).push_back('\n');
										} else Moved.append(Text).push_back(' ');
									}
								);
							}
						}
					);
					FlushText();
					if(!Moved.empty()) {
						// 最外 JSX 要素の前に在る式位置へのコメント移動
						TSNode Top = Node;
						while(!ts_node_is_null(ts_node_parent(Top)) && NodeKind::JsxContainer.Contains(ts_node_parent(Top))) Top = ts_node_parent(Top);
						TextEdit::Push(Src.Start(Top), Src.Start(Top), std::move(Moved), Edits);
					}
					if(!MovedLines.empty()) {
						// 行末迄続くコメントは所有文の前へ移し，要素の前から本文へ戻る事を防止
						TSNode Statement = Node;
						for(TSNode Parent = ts_node_parent(Statement); !ts_node_is_null(Parent); Parent = ts_node_parent(Parent)) {
							if(NodeKind::JsStatementSequence.Contains(Parent)) break;
							Statement = Parent;
						}
						TextEdit::Push(Src.Start(Statement), Src.Start(Statement), std::move(MovedLines), Edits);
					}
				}
				// コメント本文の内側には降りず，構文要素の本文だけを走査する事の返戻
				return !NodeKind::Comment.Contains(TypeView);
			}
		);
		// 全要素範囲の確定後に於ける後ろ向き編集の一括適用
		TextEdit::Apply(*this, Edits);
	}
	// コメント処理に使う言語の区分
	const bool IsCssSource = ts_tree_language(Syntax.Tree.get()) == Lang::Get(Lang::CSS).TsLang();
	const bool IsGoSource = ts_tree_language(Syntax.Tree.get()) == Lang::Get(Lang::Go).TsLang();
	const bool IsRustSource = ts_tree_language(Syntax.Tree.get()) == Lang::Get(Lang::Rust).TsLang();
	const bool IsSwiftSource = ts_tree_language(Syntax.Tree.get()) == Lang::Get(Lang::Swift).TsLang();
	const bool IsRubySource = ts_tree_language(Syntax.Tree.get()) == Lang::Get(Lang::Ruby).TsLang();
	const bool IsJsTsSource = IsJsxSource || ts_tree_language(Syntax.Tree.get()) == Lang::Get(Lang::JavaScript).TsLang() ||
	ts_tree_language(Syntax.Tree.get()) == Lang::Get(Lang::TypeScript).TsLang();
	// 再実行時に旧木の紐付を重ねない為の初期化
	Attachments.ClearNodes();
	// 行末・出力コメント群・最後の子・次行範囲・包装降下先の判定を再利用
	struct LineScan {
		uint32_t End = 0;
		uint32_t Scan = 0;
		uint32_t GoComment = 0;
		bool IsGoOutput = false;
		std::unordered_map<const void *, TSNode> LastChild;
		uint32_t DirectiveBreak = 0; // 0 は次行指令の改行位置を未計算である事を表す
		uint32_t DirectiveLineEnd = 0;
		std::unordered_map<const void *, TSNode> Descent;
		std::vector<TSNode> DescentPath;
	};
	// コメント本文の削除範囲
	struct DeleteRange {
		uint32_t Begin;
		uint32_t End;
		bool IsIdentifierBoundary = false; // CSS の結合子・数値と単位を保持する英数字間だけの空白補完
	};
	struct Pending {
		uint32_t CommentStart;
		uint32_t AnchorStart;
		uint32_t AnchorEnd;
		const char *AnchorType = nullptr; // tree-sitter の型文字列は文法定義の静的領域 Reparse 後も同一ポインタが返る為，ポインタ比較で十分
		CommentAttach Comment;
		uint32_t CommentEnd = 0;
		bool ShouldNormalizeLeading = false;
		bool ShouldBlockify = false;
		bool IsVerbatim = false;
	};
	// 同親の前後の名前付非コメント子と祖先側の前後を１度控え，並列収集中は読取専用（兄弟・祖先の再照会に依る三乗費用を回避）
	struct Neighbors {
		TSNode Prev;
		TSNode Next;
		TSNode OuterPrev; // 親から遡った直前の名前付非コメントの節点（SiblingBefore（親）と同じ）
		TSNode OuterNext;
		TSNode ParentPrev;
		TSNode ParentNext;
		TSNode Following; // 後に続く最初のコメントでない子（字句を含み，親の中に無ければコメントと共に終わる祖先の後に続く物）
		bool IsAfterOpener;
		bool IsAfterCloser;
		bool IsAfterToken;
		bool IsLineHead; // 行頭のコメントか（同じ行の前に囲みコメントだけが並ぶ `/* a */ /* b */` の `/* b */` を含む）
		bool IsSignChain;
	};
	// 並列処理前の隣接関係確定と各走脈からの読取専用化
	std::unordered_map<const void *, Neighbors> Adjacent;
	// 位置が空白無で続く符号付の数 (`-2px` / `+.5em`) の符号かの判定（Sass は値の後の其れを加減算と読む）
	const auto IsSignAt = [this](const uint32_t Pos) -> bool {
		// 対象位置の処理
		const std::string &Text = *this;
		// 符号の後に数字か小数点が続くかの返戻
		return Pos + 1 < Text.size() && (Text[Pos] == '-' || Text[Pos] == '+') &&
		(std::isdigit(static_cast<unsigned char>(Text[Pos + 1])) || Text[Pos + 1] == '.');
	};
	// 祖先経路と最後の子を再利用し，コメントの保留分・削除範囲を走脈別に収集
	const auto LastChildOf = [](const TSNode Node, LineScan &Memo) -> TSNode {
		// 非コメント末尾子の取得と保持
		const auto [Slot, IsNew] = Memo.LastChild.try_emplace(Node.id);
		if(IsNew) {
			ForEachNamedChild(
				Node,
				[& Last = Slot->second](const TSNode Child) -> void {
					// 非コメント子による末尾子の更新
					if(!NodeKind::Comment.Contains(Child)) Last = Child;
				}
			);
		}
		// コメントでない最後の名前付の子の返戻
		return Slot->second;
	};
	const auto ProcessComment = [
		this,
		DocMarker,
		IsCssSource,
		IsScssSource,
		IsGoSource,
		IsJsTsSource,
		IsRubySource,
		IsRustSource,
		IsSwiftSource,
		&Adjacent,
		&LastChildOf,
		&IsSignAt
	](
		const TSNode Node,
		const std::span<const TSNode> Ancestors,
		std::vector<Pending> &PendingList,
		std::vector<DeleteRange> &DeleteRanges,
		LineScan &LastScan
	) -> void {
		// 親と祖父（根の直下のコメントは祖父を持たず，根其の物のコメントは親も持たない）
		const TSNode NodeParent = Ancestors.empty() ? TSNode{} : Ancestors.back();
		// コメントの祖父節点
		const TSNode NodeGrandparent = Ancestors.size() > 1 ? Ancestors[Ancestors.size() - 2] : TSNode{};
		// コメントを含む原文
		const std::string &Self = *this;
		// 末尾に \n を含むノード（Rust の doc_comment 等）を除外したテキスト範囲の決定
		uint32_t TokenStart = Start(Node), TextEnd = End(Node);
		while(TextEnd > TokenStart && (Self[TextEnd - 1] == '\n' || Self[TextEnd - 1] == '\r')) --TextEnd;
		// 幅０の Kotlin コメントは直前の囲み本文をネスト対応で回収し，消失・残留を防止
		const bool IsRelocated = TextEnd == TokenStart;
		if(IsRelocated) {
			uint32_t Close = TokenStart;
			// 空白を越えて直前の囲みコメント終端を探す
			while(Close && (Self[Close - 1] == ' ' || Self[Close - 1] == '\t' || Self[Close - 1] == '\n' || Self[Close - 1] == '\r')) {
				--Close;
			}
			// 直前が囲みコメントの終わりでない幅０の節点を扱わない事の返戻
			if(Close < 4 || Self[Close - 2] != '*' || Self[Close - 1] != '/') return;
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
			// 対を為す開きが無い幅０の節点を扱わない事の返戻
			if(Depth) return;
			TokenStart = Open;
			TextEnd = Close;
		}
		uint32_t LeadStart = TokenStart;
		while(LeadStart && (Self[LeadStart - 1] == ' ' || Self[LeadStart - 1] == '\t')) --LeadStart;
		// 同じ親の直前・直後の名前付非コメント子と，同じ親に無ければ親から遡った隣接と行頭のコメントか（控えた物を引く）
		const std::unordered_map<const void *, Neighbors>::const_iterator Neighbor = Adjacent.find(Node.id);
		const auto [
			PrevImmediate,
			NextImmediate,
			OuterPrev,
			OuterNext,
			ParentPrev,
			ParentNext,
			Following,
			IsAfterOpener,
			IsAfterCloser,
			IsAfterToken,
			IsLineHead,
			IsSignChain
		] = Neighbor == Adjacent.end() ? Neighbors{} : Neighbor->second;
		// 同じ行の囲みコメント群の行頭判定を継承（本文を移した幅０節点だけ再判定）
		const bool IsAtLineStart = !LeadStart || Self[LeadStart - 1] == '\n' || !IsRelocated && IsLineHead;
		// 空白・コメントを飛ばして最初のコードか改行で行末判定し，同じ行の結果を再利用
		uint32_t Scan = LastScan.End < TextEnd && TextEnd <= LastScan.Scan ? LastScan.Scan : TextEnd;
		// 原文の連続領域
		const char *const SelfData = Self.data();
		const uint32_t SelfSize = static_cast<uint32_t>(Self.size());
		// 括弧・塊の所有構文を取得（包装は祖父，直置の塊は自身，括弧を直に持つ構文は親）
		const auto BlockOwner = [&]() -> TSNode {
			// 持主の構文の返戻
			return IsOpenBracketChar(SelfData[Start(NodeParent)]) && !ts_node_is_null(NodeGrandparent) &&
			!NodeKind::StatementHost.Contains(NodeGrandparent) ? NodeGrandparent : NodeParent;
		};
		if(Scan == TextEnd) while(Scan < SelfSize) {
			if(const char Char = SelfData[Scan], Next = Scan + 1 < SelfSize ? SelfData[Scan + 1] : '\0'; Char == ' ' || Char == '\t') {
				++Scan;
			} else if(Char == '/' && Next == '*') {
				Scan += 2;
				while(Scan + 1 < SelfSize && !(SelfData[Scan] == '*' && SelfData[Scan + 1] == '/')) ++Scan;
				Scan = std::min(Scan + 2, SelfSize);
			} else if(Char == '#' || Char == '/' && Next == '/') {
				const void *const Newline = std::memchr(SelfData + Scan, '\n', SelfSize - Scan);
				Scan = Newline ? static_cast<uint32_t>(static_cast<const char *>(Newline) - SelfData) : SelfSize;
			} else break;
		}
		LastScan.End = TextEnd;
		// 再利用する行末探索結果
		LastScan.Scan = Scan;
		const bool IsAtLineEnd = Scan >= SelfSize || SelfData[Scan] == '\n' || SelfData[Scan] == '\r';
		// 行頭に在っても後ろに同じ行のコードが続くコメントは其のコードの説明の為，行内のコメントとして後ろの要素へ紐付ける
		const bool IsLeading = IsAtLineStart && IsAtLineEnd, IsInline = !IsAtLineEnd;
		// 原文から除く範囲の終端
		uint32_t DelEnd = TextEnd;
		if(IsLeading && DelEnd < Self.size() && Self[DelEnd] == '\n') ++DelEnd;
		CommentAttach Attach;
		// Sass の加減算・負数の読みを保つ為，後の符号付数と左の値への密着状態を保持
		Attach.IsGluedAfter = IsCssSource && IsSignAt(TextEnd);
		Attach.IsGluedBefore = IsSignChain && LeadStart == TokenStart && TokenStart && SelfData[TokenStart - 1] != '\n';
		// 所在行の字下げを除いて保存し，復元毎の字下げ増加を防止（行頭は字句の桁から算出）
		uint32_t IndentStart = IsRelocated ? TokenStart : TokenStart - ts_node_start_point(Node).column;
		while(IsRelocated && IndentStart && Self[IndentStart - 1] != '\n') --IndentStart;
		uint32_t IndentEnd = IndentStart;
		while(IndentEnd < TokenStart && (Self[IndentEnd] == ' ' || Self[IndentEnd] == '\t')) ++IndentEnd;
		// Raw / OrigIndent は走査中不変の Self を指すビュー（複製は出力テキスト構築時の１回に限定する）
		const std::string_view Raw(data() + TokenStart, TextEnd - TokenStart);
		const bool IsJSDocTypeCast =
		IsJsTsSource && Raw.starts_with("/**") && Raw.find("@type") != std::string_view::npos && Scan < SelfSize &&
		SelfData[Scan] == '(';
		if(const std::string_view OrigIndent(data() + IndentStart, IndentEnd - IndentStart); OrigIndent.empty()) {
			Attach.Text = std::string(Raw);
		} else {
			// 本文内の相対字下げを残す各行の共通字下げ除去
			std::string Normalized;
			// 原文長の領域予約
			Normalized.reserve(Raw.size());
			size_t Cursor = 0;
			bool IsFirst = true;
			while(Cursor < Raw.size()) {
				const size_t NewlinePos = Raw.find('\n', Cursor), LineEnd = NewlinePos == std::string::npos ? Raw.size() : NewlinePos;
				if(IsFirst) Normalized.append(Raw, Cursor, LineEnd - Cursor);
				else {
					// 一致した共通字下げ長
					size_t StripLen = 0;
					while(StripLen < OrigIndent.size() && Cursor + StripLen < LineEnd && Raw[Cursor + StripLen] == OrigIndent[StripLen]) ++StripLen;
					Normalized.append(Raw, Cursor + StripLen, LineEnd - Cursor - StripLen);
				}
				if(NewlinePos == std::string::npos) break;
				// 行境界の復元
				Normalized.push_back('\n');
				Cursor = NewlinePos + 1;
				IsFirst = false;
			}
			Attach.Text = std::move(Normalized);
		}
		// Ruby の埋込ドキュメントの本文へ解析の為に挟んだ印を除き，本文を原文へ戻す（`Formatter::EscapeRubyEmbeddedDocs`）
		if(DocMarker && Raw.starts_with("=begin")) {
			const char Escaped[] = { '=', DocMarker, 'e', 'n', 'd' };
			for(
				size_t Found = Attach.Text.find(Escaped, 0, sizeof(Escaped));
				Found != std::string::npos;
				Found = Attach.Text.find(Escaped, Found + 1, sizeof(Escaped))
			) Attach.Text.erase(Found + 1, 1);
		}
		// マーカ空白・日本語表記を一括正規化し，句点分割は独立行だけ適用（行末の分割・再融合に依る字下げ消失と非冪等を防止）
		Attach.IsLeading = Attach.IsStandalone = IsLeading;
		// 後段で付ける原文順
		Attach.Seq = 0;
		// go test が照合する例関数の出力コメント群を検出
		const auto IsGoExampleOutput = [&]() -> bool {
			// 遅延処理の実行
			if(!Raw.starts_with("//")) return false;
			const std::span<const TSNode>::reverse_iterator Enclosing = std::find_if(
				Ancestors.rbegin(),
				Ancestors.rend(),
				[](const TSNode Ancestor) -> bool {
					// 関数の宣言かの返戻
					return std::string_view(ts_node_type(Ancestor)) == "function_declaration";
				}
			);
			// 例の関数の中でない事の返戻
			if(Enclosing == Ancestors.rend() || !View(FieldChild(*Enclosing, "name")).starts_with("Example")) return false;
			// 行頭の行コメント群を遡り大小を問わず出力見出を探索（既判定を継承し群の行数の二乗走査を回避）
			bool IsOutput = false;
			for(uint32_t Line = TokenStart;;) {
				// 行コメントの `//` の後の本文（行末迄，先頭の空白を除く）
				std::string_view Body(SelfData + Line + 2, std::min<size_t>(Self.find('\n', Line), SelfSize) - Line - 2);
				while(!Body.empty() && (Body.front() == ' ' || Body.front() == '\t')) Body.remove_prefix(1);
				const auto StartsWithWord = [&Body](const std::string_view Word) -> bool {
					// 大小を区別しない前方一致の返戻
					return Body.size() >= Word.size() && std::equal(
						Word.begin(),
						Word.end(),
						Body.begin(),
						[](const char Expected, const char Actual) -> bool {
							// 英字の大小を揃えた一致の返戻
							return Expected == std::tolower(static_cast<unsigned char>(Actual));
						}
					);
				};
				IsOutput = StartsWithWord("output:") || StartsWithWord("unordered output:");
				const uint32_t LineHead = TextEdit::LineStartOf(Self, Line);
				// 出力の見出に達したか，前の行が無いか行コメントだけの行でなければ群の外
				if(IsOutput || !LineHead) break;
				Line = TextEdit::SkipSpRight(Self, TextEdit::LineStartOf(Self, LineHead - 1));
				if(Self.compare(Line, 2, "//")) break;
				if(Line == LastScan.GoComment) {
					// 既判定群の結果再利用
					IsOutput = LastScan.IsGoOutput;
					break;
				}
			}
			LastScan.GoComment = TokenStart;
			LastScan.IsGoOutput = IsOutput;
			// 出力のコメントかの返戻
			return IsOutput;
		};
		// 処理系が読む Rust 文書・cgo 前文・Go の出力コメントを逐語保持
		const bool IsVerbatim = IsRustSource && (
			Raw.starts_with("///") && !Raw.starts_with("////") || Raw.starts_with("//!") || Raw.starts_with("/*!") ||
			Raw.starts_with("/**") && !Raw.starts_with("/***") && !Raw.starts_with("/**/")
		) || IsGoSource && (
			!ts_node_is_null(NextImmediate) && std::string_view(ts_node_type(NextImmediate)) == "import_declaration" && [&]() -> bool {
				// 遅延処理の実行
				const TSNode Spec = ts_node_named_child(NextImmediate, 0), Path = ts_node_is_null(Spec) ? TSNode{} : FieldChild(Spec, "path");
				// 取り込むパッケージが cgo の `"C"` かの返戻
				return !ts_node_is_null(Path) && View(Path) == "\"C\"";
			}() || IsGoExampleOutput()
		);
		// 逐語保持状態の記録
		Attach.IsVerbatim = IsVerbatim;
		// 直前・直後の兄弟（無ければ親から遡った直前・直後の兄弟）
		const TSNode PrevOrOuter = ts_node_is_null(PrevImmediate) ? OuterPrev : PrevImmediate;
		const TSNode NextOrOuter = ts_node_is_null(NextImmediate) ? OuterNext : NextImmediate;
		// 行末の次の行に作用する指令の次の行の終わり（同じ改行の後のコメントで引き継ぎ，コメント毎に改行を探し直さない）
		const bool IsLineEndDirective = Scan < SelfSize && SelfData[Scan] == '\n' && DocSig::IsNextLineDirective(Raw);
		const auto DirectiveLineEnd = [&]() -> uint32_t {
			// 遅延処理の実行
			if(LastScan.DirectiveBreak != Scan + 1) {
				LastScan.DirectiveBreak = Scan + 1;
				const void *const LineEnd = std::memchr(SelfData + Scan + 1, '\n', SelfSize - Scan - 1);
				LastScan.DirectiveLineEnd = LineEnd ? static_cast<uint32_t>(static_cast<const char *>(LineEnd) - SelfData) : SelfSize;
			}
			// 次の行の終わりの返戻
			return LastScan.DirectiveLineEnd;
		};
		// 指令の次の行に始まる節点かの判定
		const auto StartsOnNextLine = [&](const TSNode Node) -> bool {
			// 節点の始まりが次の行に在るかの返戻
			return !ts_node_is_null(Node) && ts_node_start_byte(Node) > Scan && ts_node_start_byte(Node) <= DirectiveLineEnd();
		};
		// 指令の次の行が閉じ字句で始まり其の行にコードが始まらない（作用する行が `);` / `}` だけの）かの判定
		const auto IsBeforeCloserLine = [&]() -> bool {
			// コメントの後の字句が次の行の閉じ字句で，直後の兄弟が次の行に始まらないかの返戻
			return !ts_node_is_null(Following) && !ts_node_is_named(Following) && StartsOnNextLine(Following) &&
			NodeKind::CloserToken.Contains(Following) && !StartsOnNextLine(NextOrOuter);
		};
		// 子文字列境界を保つコメントだけの JSX 式包装の錨化
		if(!ts_node_is_null(NodeParent) && std::string_view(ts_node_type(NodeParent)) == "jsx_expression") {
			bool IsAllComment = ts_node_named_child_count(NodeParent);
			ForEachNamedChild(
				NodeParent,
				[&IsAllComment](const TSNode Child) -> bool {
					// コメントを読み飛ばす事の返戻
					if(NodeKind::Comment.Contains(Child)) return true;
					IsAllComment = false;
					// コメント以外を発見した為，走査打切の返戻
					return false;
				}
			);
			if(IsAllComment) {
				Attach.IsLeading = true;
				PendingList.push_back(
					{
						TokenStart,
						Start(NodeParent),
						End(NodeParent),
						ts_node_type(NodeParent),
						std::move(Attach),
						TextEnd,
						IsLeading,
						Raw.starts_with("//")
					}
				);
				// 包装内本文の除去範囲
				DeleteRanges.push_back({ TokenStart, TextEnd });
				// 最終出力時に包装内へ戻すコメント本文だけの分離
				return;
			}
		}
		// 選択子コメントは規則前へ移し，CSS / SCSS の空白の意味と数値・単位の境界を保持
		if(IsCssSource) {
			if(!ts_node_is_null(NodeGrandparent) && NodeKind::CssNumericValue.Contains(NodeParent)) {
				DeleteRanges.push_back({ TokenStart, TextEnd, true });
				// CSS 数値内コメントの保留情報
				Pending Entry;
				Entry.CommentStart = TokenStart;
				Entry.CommentEnd = TextEnd;
				Entry.ShouldNormalizeLeading = IsLeading;
				Entry.AnchorStart = ts_node_start_byte(NodeGrandparent);
				Entry.AnchorEnd = ts_node_end_byte(NodeGrandparent);
				Entry.AnchorType = ts_node_type(NodeGrandparent);
				Attach.IsLeading = false;
				// 復元情報の保留
				Entry.Comment = std::move(Attach);
				PendingList.push_back(std::move(Entry));
				// 値を含む節点の末尾コメントとして紐付けた後の終了
				return;
			}
			// 選択子の並びの祖先の位置の次（無ければ 0，規則は其の親）
			size_t SelectorsNext = 0;
			for(size_t Up = Ancestors.size(); Up--;) {
				const std::string_view UpType = ts_node_type(Ancestors[Up]);
				if(UpType == "selectors") {
					SelectorsNext = Up + 1;
					break;
				}
				if(NodeKind::ScopeBody.Contains(UpType)) break;
			}
			if(const TSNode Rule = SelectorsNext > 1 ? Ancestors[SelectorsNext - 2] : TSNode{}; !ts_node_is_null(Rule)) {
				DeleteRanges.push_back({ TokenStart, TextEnd, !IsScssSource });
				// 選択子コメントの保留情報
				Pending Entry;
				Entry.CommentStart = TokenStart;
				Entry.CommentEnd = TextEnd;
				Entry.ShouldNormalizeLeading = IsLeading;
				Entry.AnchorStart = ts_node_start_byte(Rule);
				Entry.AnchorEnd = ts_node_end_byte(Rule);
				Entry.AnchorType = ts_node_type(Rule);
				Attach.IsLeading = true;
				// 復元情報の保留
				Entry.Comment = std::move(Attach);
				PendingList.push_back(std::move(Entry));
				// 規則の前の先行コメントとして紐付けた後の終了
				return;
			}
		}
		// Swift 範囲は片側空白の誤読を避け式末尾へ，CSS 値は再解析で錨を失わない宣言末尾へ紐付
		const std::string_view ParentType = ts_node_is_null(NodeParent) ? std::string_view() : ts_node_type(NodeParent);
		TSNode Anchor = IsInline && (
			IsSwiftSource && ParentType == "range_expression" || IsCssSource && ParentType == "declaration" && (
				!ts_node_is_null(PrevImmediate) && std::string_view(ts_node_type(PrevImmediate)) == "identifier" ||
				!ts_node_is_null(NextImmediate) && std::string_view(ts_node_type(NextImmediate)) == "identifier"
			)
		) ? NodeParent : TSNode{};
		if(!ts_node_is_null(Anchor)) Attach.IsLeading = false;
		// 行内：直接兄弟優先で解決し，見付からなければ後段の汎用代替経路（祖先兄弟 → ルート）でアンカー化
		if(IsInline && ts_node_is_null(Anchor)) {
			// 区切り・閉じ前は直前兄弟の説明として末尾紐付（空実引数内は被呼出側へ）
			uint32_t Next = TextEnd;
			while(Next < SelfSize && (SelfData[Next] == ' ' || SelfData[Next] == '\t')) ++Next;
			if(Next < SelfSize && (SelfData[Next] == ',' || SelfData[Next] == ';' || IsCloseBracketChar(SelfData[Next]))) {
				Anchor = PrevOrOuter;
			}
			if(ts_node_is_null(Anchor)) {
				Anchor = NextImmediate;
				// 直後の兄弟を説明する `g(/* 個数 */ 3)` 等は先行紐付し，Layout で錨直前へ復元（行末へ移さない）
				if(!ts_node_is_null(Anchor)) Attach.IsLeading = true;
				else Anchor = PrevImmediate;
			}
		} else if(IsLeading) {
			// 直接後兄弟→祖先側の順に先行錨を探索（Ruby の節前・空本体の閉じ前は本体末尾）
			const bool IsBeforeRubyClause = IsRubySource && !ts_node_is_null(NextImmediate) && (
				NodeKind::SectionHeader.Contains(NextImmediate) ||
				NodeKind::ThenDo.Contains(NextImmediate) && !ts_node_named_child_count(NextImmediate)
			);
			// PHP の空の代替節のコメントは次の枝へ移さず，見出後へ独立配置
			const TSNode ColonBody = ts_node_is_null(PrevImmediate) || !NodeKind::PhpIfBranch.Contains(PrevImmediate) ?
			PrevImmediate :
			LastChildOf(PrevImmediate, LastScan);
			const bool IsInEmptyColonBody = !ts_node_is_null(ColonBody) && std::string_view(ts_node_type(ColonBody)) == "colon_block" &&
			!ts_node_named_child_count(ColonBody);
			// 直後が閉じ字句なら，後の節の先行でなく塊の末尾として紐付
			Anchor = IsBeforeRubyClause || IsInEmptyColonBody ||
			!ts_node_is_null(Following) && !ts_node_is_named(Following) && NodeKind::CloserToken.Contains(Following) ?
			TSNode{} :
			NextImmediate;
			// 塊と後続節の間の独立コメントは本体先頭へ紐付し，非コメント子で塊の終端を判定
			const auto EndsWithBrace = [&](TSNode Block) -> bool {
				// 末尾が閉じ波括弧の節点の判定
				while(true) {
					// 幅０の節点は終端の直前の字を持たない為，先に幅を見る（`End - 1` が巻き戻る）
					if(End(Block) > Start(Block) && SelfData[End(Block) - 1] == '}') return true;
					const TSNode Last = LastChildOf(Block, LastScan);
					if(ts_node_is_null(Last) || End(Last) != End(Block)) {
						uint32_t At = ts_node_is_null(Last) ? Start(Block) + 1 : End(Last);
						while(At < End(Block) && (SelfData[At] == ' ' || SelfData[At] == '\t' || SelfData[At] == '\n' || SelfData[At] == '\r')) ++At;
						// コメントでない最後の子の後が閉じ波括弧かの返戻
						return At < End(Block) && SelfData[At] == '}';
					}
					Block = Last;
				}
			};
			if(
				!ts_node_is_null(Anchor) && (IsAfterCloser || !ts_node_is_null(PrevImmediate) && EndsWithBrace(PrevImmediate)) &&
				!DocSig::IsNextLineDirective(Raw)
			) {
				TSNode Body {};
				if(NodeKind::BraceClause.Contains(Anchor)) {
					Body = ts_node_named_child_count(Anchor) ? LastChildOf(Anchor, LastScan) : ts_node_next_named_sibling(Anchor);
				} else if(!ts_node_is_null(Following) && !ts_node_is_named(Following) && View(Following) == "else") Body = Anchor;
				if(!ts_node_is_null(Body) && NodeKind::IfNode.Contains(Body)) Body = IfConsequence(Body);
				if(!ts_node_is_null(Body) && NodeKind::StatementHost.Contains(Body)) {
					ForEachNamedChild(
						Body,
						[&Anchor](const TSNode Child) -> bool {
							// コメントを読み飛ばす事の返戻
							if(NodeKind::Comment.Contains(Child)) return true;
							Anchor = Child;
							// 本体の最初の文を得た為，走査打切の返戻
							return false;
						}
					);
				}
			}
			// 次行指令は作用対象へ先行紐付し，閉じ字句だけなら畳込後の錨行前配置を記録
			if(ts_node_is_null(Anchor) && !IsBeforeRubyClause && !IsInEmptyColonBody && IsLineEndDirective) {
				Attach.IsCloserDirective = IsBeforeCloserLine();
				if(StartsOnNextLine(NextOrOuter)) {
					Anchor = NextOrOuter;
					// 節の本体（Swift の `else` は字句だけの葉で本体は次の兄弟の文の並び，else if の連鎖は if の本体）
					TSNode Body {};
					if(NodeKind::BraceClause.Contains(Anchor)) {
						Body = ts_node_named_child_count(Anchor) ? LastChildOf(Anchor, LastScan) : ts_node_next_named_sibling(Anchor);
					}
					if(!ts_node_is_null(Body) && NodeKind::IfNode.Contains(Body)) Body = IfConsequence(Body);
					if(!ts_node_is_null(Body) && NodeKind::StatementHost.Contains(Body)) {
						TSNode First {};
						ForEachNamedChild(
							Body,
							[&First](const TSNode Child) -> bool {
								// コメントを読み飛ばす事の返戻
								if(NodeKind::Comment.Contains(Child)) return true;
								First = Child;
								// 本体の最初の文を得た為，走査打切の返戻
								return false;
							}
						);
						if(
							const uint32_t From = ts_node_start_byte(Anchor), FirstStart = ts_node_is_null(First) ? From : ts_node_start_byte(First);
							FirstStart > From && !std::memchr(SelfData + From, '\n', FirstStart - From)
						) Anchor = First;
					}
				}
			}
			if(ts_node_is_null(Anchor)) {
				// 祖先側も含め構文木の後続字句で末尾判定し，Ruby の次節も閉じ扱い
				const std::string_view FollowingType = ts_node_is_null(Following) ? std::string_view() : ts_node_type(Following);
				bool IsScopeEnd = IsBeforeRubyClause || IsInEmptyColonBody || (
					!ts_node_is_null(Following) && ts_node_is_named(Following) ?
					IsRubySource && NodeKind::SectionHeader.Contains(FollowingType) :
					NodeKind::CloserToken.Contains(FollowingType)
				);
				// スコープ末尾は直前兄弟後の独立行へ固定（見出との間に開きが在れば空塊扱い）
				if(
					(IsScopeEnd || !ts_node_is_null(NodeParent) && NodeKind::SequentialStmtContainer.Contains(NodeParent)) &&
					!ts_node_is_null(PrevImmediate) && !IsAfterOpener
				) {
					// 後続節と同じ深さのコメントは手前の本体を錨とし，再解析後も位置を保持
					const bool HasParentClause = !ts_node_is_null(ParentNext) && NodeKind::BraceClause.Contains(ParentNext);
					const TSNode NextClause = HasParentClause ? ParentNext : OuterNext;
					const bool IsClauseComment =
					IndentEnd - IndentStart < ts_node_start_point(PrevImmediate).column && !ts_node_is_null(NextClause) &&
					NodeKind::BraceClause.Contains(NextClause);
					Anchor = IsClauseComment && !HasParentClause ? BlockOwner() : PrevImmediate;
					Attach.IsLeading = false;
					Attach.IsOwnLine = true;
					IsScopeEnd = false;
					if(!IsClauseComment) {
						// 空本体の Ruby コメントは見出後へ独立配置し，多行コメントとの順序を保持
						Attach.ForcesOwnLine = IsRubySource && NodeKind::RubyHeadedBody.Contains(NodeParent);
						// 直前の節の最後の文の後へ配置し，文が無ければ見出後へ独立配置
						for(
							std::string_view Type = ts_node_type(Anchor);
							NodeKind::SectionHeader.Contains(Type) || NodeKind::ClauseBody.Contains(Type);
							Type = ts_node_type(Anchor)
						) {
							const TSNode Last = LastChildOf(Anchor, LastScan);
							Attach.ForcesOwnLine =
							ts_node_is_null(Last) || ts_node_eq(Last, FieldChild(Anchor, "value")) || NodeKind::SectionHeader.Contains(Last);
							if(Attach.ForcesOwnLine) break;
							Anchor = Last;
						}
					}
				}
				// コメントだけの塊は所有構文末尾へ連結（空の Ruby 節は見出後へ独立配置）
				if(IsScopeEnd && !ts_node_is_null(NodeParent)) {
					Anchor = BlockOwner();
					Attach.IsLeading = false;
					Attach.IsOwnLine = Attach.ForcesOwnLine = NodeKind::SectionHeader.Contains(Anchor);
				}
				if(ts_node_is_null(Anchor)) Anchor = NextOrOuter;
			}
			// 全 null は Root に末尾コメント化（SiblingBefore は先行コメント解釈で先頭出力／行内化の危険有為，回避）
			if(ts_node_is_null(Anchor)) {
				Anchor = GetRoot();
				Attach.IsLeading = false;
			}
		} else if(
			!ts_node_is_null(NextImmediate) && IsAfterToken && (ts_node_is_null(PrevImmediate) || IsAfterCloser) ||
			IsLineEndDirective && StartsOnNextLine(NextOrOuter)
		) {
			// 見出行末と次行指令は後の作用対象へ先行紐付し，構文編集後も配置を保持
			Anchor = NextOrOuter;
			Attach.IsLeading = Attach.IsHead = true;
		} else {
			// 次行が閉じ字句だけの指令は，畳込後の錨行前配置を記録
			Attach.IsCloserDirective = IsLineEndDirective && IsBeforeCloserLine();
			// 空塊・空の Ruby begin は所有構文末尾へ固定し，前の文への移動を防止
			if(
				!ts_node_is_null(NodeParent) && (
					NodeKind::JsStatementBlockOrBlock.Contains(NodeParent) && ts_node_is_null(LastChildOf(NodeParent, LastScan)) ||
					IsAfterToken && ts_node_is_null(PrevImmediate) && ts_node_is_null(NextImmediate) &&
					!IsOpenBracketChar(SelfData[Start(NodeParent)]) && std::string_view(ts_node_type(NodeParent)) != "do"
				)
			) Anchor = BlockOwner();
			// 閉じ後は塊又は直持ち構文へ末尾紐付（同親の後続節が在れば直前兄弟側に保持）
			if(ts_node_is_null(Anchor) && IsAfterCloser && ts_node_is_null(NextImmediate)) Anchor = NodeParent;
			// 末尾錨は直前兄弟を優先し，制御見出と本体の間だけ直後兄弟にも退避
			if(ts_node_is_null(Anchor)) Anchor = PrevOrOuter;
			// 空括弧は同行の直前の名前・捕捉へ紐付（兄弟無は現錨を保持して除去時だけ再紐付）
			if(!ts_node_is_null(Anchor)) {
				if(
					const std::string_view Text = View(Anchor);
					Text.size() > 1 && Text.front() == '(' && Text.back() == ')' && Text.find_first_not_of(" \t\n", 1) == Text.size() - 1
				) {
					if(
						const TSNode Before = SiblingBeforeImmediate(Anchor);
						!ts_node_is_null(Before) &&
						!std::memchr(SelfData + ts_node_end_byte(Before), '\n', TokenStart - std::min(TokenStart, ts_node_end_byte(Before)))
					) Anchor = Before;
				}
			}
			// 空括弧の開き後は同じ行から始まる所有文へ末尾紐付し，文の並びの外へは遡らない
			if(!ts_node_is_null(Anchor) && !ts_node_is_null(NodeParent)) {
				if(
					const uint32_t Opener = TextEdit::SkipCharsLeftBounded(Self, TokenStart, 0, " \t"), AnchorEnd = ts_node_end_byte(Anchor);
					Opener && std::string_view("([{").find(SelfData[Opener - 1]) != std::string_view::npos && AnchorEnd < Opener &&
					ts_node_is_null(ParentPrev)
				) {
					const uint32_t LineStart = TextEdit::LineStartOf(Self, Opener);
					// 親が文の並び又は根に届く迄の祖先位置による遡行
					size_t OwnerAt = Ancestors.size() - 1;
					bool IsStatement = false;
					while(OwnerAt) {
						const TSNode Up = Ancestors[OwnerAt - 1];
						IsStatement = OwnerAt == 1 || NodeKind::SequentialStmtContainer.Contains(Up);
						if(IsStatement || ts_node_start_byte(Up) < LineStart) break;
						--OwnerAt;
					}
					if(
						const TSNode Owner = Ancestors[OwnerAt];
						IsStatement && ts_node_start_byte(Owner) >= LineStart && AnchorEnd <= ts_node_start_byte(Owner)
					) Anchor = Owner;
				}
			}
			if(ts_node_is_null(Anchor)) Anchor = NextOrOuter;
			// 制御構文の同行末尾錨は本体へ降り，包装後に閉じ波括弧へ移る事を防止
			if(!ts_node_is_null(Anchor)) {
				// A..B 間に改行が無ければ同一行 O(|B-A|) の局所走査で充分（LineOf 全体走査を回避）
				const auto SameLineAs = [&Self](const uint32_t Pos1, const uint32_t Pos2) -> bool {
					// 対象位置の処理
					const uint32_t Low = std::min(Pos1, Pos2), High = std::max(Pos1, Pos2);
					// 原文内の上限
					const uint32_t Lim = std::min<uint32_t>(High, static_cast<uint32_t>(Self.size()));
					// 同行判定（区間内に改行を含まない）で真の返戻
					return Lim <= Low || !std::memchr(Self.data() + Low, '\n', Lim - Low);
				};
				while(true) {
					if(!NodeKind::Control.Contains(Anchor)) break;
					// コメントより前で同行の最後の名前付子を線形探索（直下の閉じ後は構文末尾の儘）
					TSNode Next {};
					ForEachChild(
						Anchor,
						[&](const TSNode Child) -> bool {
							// 錨の非コメント子の探索
							const std::string_view Type = ts_node_type(Child);
							// コメントを読み飛ばす事の返戻
							if(NodeKind::Comment.Contains(Type)) return true;
							const uint32_t ChildEnd = ts_node_end_byte(Child);
							// 字句より後の子で打ち切る事の返戻
							if(ChildEnd > TokenStart) return false;
							if(!ts_node_is_named(Child) && Type == "}") Next = TSNode{};
							else if(ts_node_is_named(Child) && SameLineAs(ChildEnd, TokenStart)) Next = Child;
							// 子孫探索結果（発見有無）の返戻
							return true;
						}
					);
					if(ts_node_is_null(Next)) break;
					Anchor = Next;
				}
			}
		}
		// 直接兄弟で未発見なら祖先側の兄弟へ退避（整形への影響を防ぐ為，コメント本文は必ず原稿から除去）
		if(ts_node_is_null(Anchor)) Anchor = NextOrOuter;
		if(ts_node_is_null(Anchor)) Anchor = PrevOrOuter;
		// 錨が無ければ根へ末尾紐付し，コメントだけの原稿も本文に残さない
		if(ts_node_is_null(Anchor)) Anchor = GetRoot();
		// 錨との間の無名字句より前に置く印を付け，先行コメントと字句の順序を保持
		Attach.IsBeforeToken =
		Attach.IsLeading && !IsCssSource && !ts_node_is_null(NextImmediate) && ts_node_eq(Anchor, NextImmediate) &&
		!ts_node_is_null(Following) && !ts_node_is_named(Following) &&
		ts_node_start_byte(Following) < ts_node_start_byte(NextImmediate);
		// 先行錨だけの透過包装の解包
		const TSNode RootAnchor = GetRoot();
		// 閉じ字句の無い文の並び・末尾改行を含む節点は最後の文へ紐付し，閉じ行への移動を防止
		while(!Attach.IsLeading && !IsAfterCloser && !ts_node_is_null(Anchor) && !ts_node_eq(Anchor, RootAnchor)) {
			if(
				const uint32_t Count = ts_node_child_count(Anchor), AnchorEnd = ts_node_end_byte(Anchor);
				!(Count && ts_node_is_named(ts_node_child(Anchor, Count - 1)) && NodeKind::TransparentWrap.Contains(Anchor)) &&
				!(AnchorEnd > ts_node_start_byte(Anchor) && SelfData[AnchorEnd - 1] == '\n')
			) break;
			const TSNode LastChild = LastChildOf(Anchor, LastScan);
			if(ts_node_is_null(LastChild)) break;
			Anchor = LastChild;
		}
		// 包装より前のコメントに対する降下先の再利用
		const bool IsBeforeAnchor = Attach.IsLeading && !ts_node_is_null(Anchor) && TokenStart < ts_node_start_byte(Anchor);
		LastScan.DescentPath.clear();
		while(Attach.IsLeading && !ts_node_is_null(Anchor) && !ts_node_eq(Anchor, RootAnchor)) {
			if(IsBeforeAnchor) {
				if(
					const std::unordered_map<const void *, TSNode>::const_iterator Hit = LastScan.Descent.find(Anchor.id);
					Hit != LastScan.Descent.end()
				) {
					// 既知の降下先再利用
					Anchor = Hit->second;
					break;
				}
			}
			const std::string_view AnchorTypeView(ts_node_type(Anchor));
			if(!NodeKind::TransparentWrap.Contains(AnchorTypeView) || IsJSDocTypeCast && AnchorTypeView == "parenthesized_expression") {
				break;
			}
			const uint32_t InnerCount = ts_node_named_child_count(Anchor);
			// 整形中に消えるコメントを錨にせず，最初の名前付非コメント子へ降下
			TSNode InnerChild {};
			ForEachNamedChild(
				Anchor,
				[&InnerChild](const TSNode Child) -> bool {
					// コメントを飛ばして次の子へ進む事の返戻
					if(NodeKind::Comment.Contains(Child)) return true;
					InnerChild = Child;
					// 最初のコメント以外の子で打ち切る事の返戻
					return false;
				}
			);
			// 複数子の包装はコメントが先頭子より前なら降下し，既存文書の見落し・重複生成を防止
			if(
				ts_node_is_null(InnerChild) || InnerCount != 1 && TokenStart >= ts_node_start_byte(InnerChild) ||
				AnchorTypeView == "parenthesized_expression" && NodeKind::JsxElement.Contains(InnerChild)
			) break;
			if(IsBeforeAnchor) LastScan.DescentPath.push_back(Anchor);
			Anchor = InnerChild;
		}
		for(const TSNode Visited : LastScan.DescentPath) LastScan.Descent[Visited.id] = Anchor;
		if(!ts_node_is_null(Anchor)) {
			// 削除前の座標と型を保存し，再解析後の節点へ引き直す
			Pending Entry;
			Entry.CommentStart = TokenStart;
			Entry.CommentEnd = TextEnd;
			const bool IsRootAnchor = ts_node_eq(Anchor, RootAnchor);
			// 行末へ置く末尾コメントは句点で分割せず次行頭への分離を防止（根の末尾だけ独立行の為に分割）
			Entry.ShouldNormalizeLeading = IsLeading && (Attach.IsLeading || Attach.IsOwnLine || IsRootAnchor);
			// 逐語保持状態
			Entry.IsVerbatim = IsVerbatim;
			Entry.AnchorStart = ts_node_start_byte(Anchor);
			Entry.AnchorEnd = ts_node_end_byte(Anchor);
			Entry.AnchorType = ts_node_type(Anchor);
			// ルート代替経路の場合は末尾コメントとして紐付（先行コメントだとファイル先頭に来て仕舞う）
			if(IsRootAnchor) Attach.IsLeading = false;
			// 復元情報の保留
			Entry.Comment = std::move(Attach);
			PendingList.push_back(std::move(Entry));
		}
		if(IsInline) {
			// 行内コメントの除去範囲
			uint32_t DelStartInline = TokenStart, DelEndInline = TextEnd;
			// 隣接１空白を仮消費し，単語融合は削除適用時に補完（CSS の子孫空白は保持）
			if(!IsCssSource) {
				if(DelEndInline < Self.size() && Self[DelEndInline] == ' ') ++DelEndInline;
				else if(DelStartInline && Self[DelStartInline - 1] == ' ') --DelStartInline;
			}
			DeleteRanges.push_back({ DelStartInline, DelEndInline });
			// 終了
			return;
		}
		// 独立行の字下げと終端改行を含む範囲の除去
		DeleteRanges.push_back({ LeadStart, DelEnd });
	};
	// 逐語内容のコメント形は保護し，コードとなる JS / TS のテンプレート置換内だけ抽出
	const auto HoldsLiteralComments = [this, IsCssSource](const TSNode Node, const std::string_view TypeView) -> bool {
		// 内容其の物としてコメントを抽出しない節点かの返戻
		return NodeKind::StringLikeInnerPreserve.Contains(TypeView) && TypeView != "template_string" || StringizesArguments(Node) ||
		IsCssSource && IsCssUrlArguments(Node);
	};
	const TSNode RootNode = GetRoot();
	const auto WalkSubtree = [&ProcessComment, &HoldsLiteralComments, RootNode](
		const TSNode SubRoot,
		std::vector<Pending> &PendingOut,
		std::vector<DeleteRange> &DelRanges,
		LineScan &LastScan
	) -> void {
		// コメント毎の親探索を避けるカーソルと祖先列の同期
		HeldCursor Cursor(SubRoot);
		// 根から現在の節点の親迄の祖先（末尾が親）
		std::vector<TSNode> Path { RootNode, SubRoot };
		if(ts_tree_cursor_goto_first_child(&Cursor)) for(bool IsDone = false; !IsDone;) {
			const TSNode Node = ts_tree_cursor_current_node(&Cursor);
			// コメント判定と内容判定で共有する型名の単一取得
			const std::string_view TypeView(ts_node_type(Node));
			const bool IsComment = NodeKind::Comment.Contains(TypeView);
			if(IsComment) ProcessComment(Node, Path, PendingOut, DelRanges, LastScan);
			// コメント又はコメント様内容を持つ節点内部の走査・抽出除外
			if(!IsComment && !HoldsLiteralComments(Node, TypeView) && ts_tree_cursor_goto_first_child(&Cursor)) {
				Path.push_back(Node);
				continue;
			}
			while(!ts_tree_cursor_goto_next_sibling(&Cursor)) {
				if(!ts_tree_cursor_goto_parent(&Cursor)) {
					IsDone = true;
					break;
				}
				Path.pop_back();
			}
		}
	};
	// ルート直下の子の振分：コメントは直接処理し，其れ以外は部分木走査へ委譲する（逐次枝／並列枝で共用）
	const auto DispatchTop =
	[&](const TSNode Child, std::vector<Pending> &PendingOut, std::vector<DeleteRange> &DelOut, LineScan &LastScan) -> void {
		// 最上位子のコメント又は部分木への振分
		if(const std::string_view TypeView(ts_node_type(Child)); NodeKind::Comment.Contains(TypeView)) {
			ProcessComment(Child, { &RootNode, 1 }, PendingOut, DelOut, LastScan);
		} else if(!HoldsLiteralComments(Child, TypeView)) WalkSubtree(Child, PendingOut, DelOut, LastScan);
	};
	// 同親の前後の子を保存し，不在時は祖先側を継承（親の後の最初の非コメント子も保持）
	struct ParentFrame {
		TSNode Node;
		TSNode Prev;
		TSNode Next;
		TSNode OuterPrev;
		TSNode OuterNext;
		TSNode Following; // 親の後に続く最初のコメントでない子（字句を含み，親の兄弟に無ければ祖先から遡った物）
	};
	// 未走査親のスタック
	std::vector<ParentFrame> Parents { { RootNode, TSNode{}, TSNode{}, TSNode{}, TSNode{}, TSNode{} } };
	std::vector<TSNode> Kids, NextOf, FollowOf, CommentOrder;
	// 深い構文木での呼出しスタック消費を避ける親の明示スタック走査
	while(!Parents.empty()) {
		// 未処理の親節点の深さ優先走査
		const ParentFrame Top = Parents.back();
		Parents.pop_back();
		Kids.clear();
		ForEachChild(
			Top.Node,
			[&Kids](const TSNode Child) -> void {
				// 最上位節点の子の収集
				Kids.push_back(Child);
			}
		);
		// 後続子を逆順に記録し，空白化して錨を失う Python の行継続はコメント同様に除外
		const auto IsAnchorless = [](const TSNode Child) -> bool {
			// 錨を持たない子の判定
			const std::string_view Type = ts_node_type(Child);
			// コメントか行継続かの返戻
			return NodeKind::Comment.Contains(Type) || Type == "line_continuation";
		};
		// 兄弟関係索引の構築
		NextOf.assign(Kids.size(), TSNode{});
		FollowOf.assign(Kids.size(), Top.Following);
		for(size_t Idx = Kids.size(); Idx > 1; --Idx) {
			const TSNode After = Kids[Idx - 1];
			const bool IsComment = IsAnchorless(After);
			NextOf[Idx - 2] = IsComment || !ts_node_is_named(After) ? NextOf[Idx - 1] : After;
			FollowOf[Idx - 2] = IsComment ? FollowOf[Idx - 1] : After;
		}
		// 直前の名前付非コメント子と，其の後に開き波括弧・字句が在ったか，前に閉じ波括弧が在ったか
		TSNode Prev {};
		bool IsAfterOpener = false, IsAfterCloser = false, IsAfterToken = false;
		for(size_t Idx = 0; Idx < Kids.size(); ++Idx) {
			const TSNode Child = Kids[Idx], Next = NextOf[Idx];
			if(!ts_node_is_named(Child)) {
				const std::string_view Token = ts_node_type(Child);
				IsAfterOpener = IsAfterOpener || Token == "{";
				IsAfterCloser = IsAfterCloser || Token == "}";
				IsAfterToken = true;
				continue;
			}
			const bool IsComment = NodeKind::Comment.Contains(Child);
			if(IsComment) {
				Adjacent[Child.id] = {
					Prev,
					Next,
					Top.OuterPrev,
					Top.OuterNext,
					Top.Prev,
					Top.Next,
					FollowOf[Idx],
					IsAfterOpener,
					IsAfterCloser,
					IsAfterToken,
					false,
					false
				};
				// 原文順整列用の収集
				CommentOrder.push_back(Child);
			}
			if(ts_node_named_child_count(Child)) {
				Parents.push_back(
					{ Child, Prev, Next, ts_node_is_null(Prev) ? Top.OuterPrev : Prev, ts_node_is_null(Next) ? Top.OuterNext : Next, FollowOf[Idx] }
				);
			}
			if(IsComment || IsAnchorless(Child)) continue;
			Prev = Child;
			IsAfterOpener = IsAfterCloser = IsAfterToken = false;
		}
	}
	// 原文順に行頭判定を１度求め，同じ行の囲みコメントの判定を継承（並列区切数×コメント数の逆走費用を回避）
	std::sort(
		CommentOrder.begin(),
		CommentOrder.end(),
		[](const TSNode Lhs, const TSNode Rhs) -> bool {
			// 左右要素の比較
			return ts_node_start_byte(Lhs) < ts_node_start_byte(Rhs);
		}
	);
	// 行頭判定用の原文
	const std::string &Self = *this;
	uint32_t PreviousEnd = 0;
	bool IsPreviousHead = false;
	for(const TSNode Comment : CommentOrder) {
		// コメント別の前後隙間の取得
		uint32_t Lead = ts_node_start_byte(Comment);
		while(Lead && (Self[Lead - 1] == ' ' || Self[Lead - 1] == '\t')) --Lead;
		IsPreviousHead = !Lead || Self[Lead - 1] == '\n' ||
		Lead == PreviousEnd && Lead > 1 && Self[Lead - 1] == '/' && Self[Lead - 2] == '*' && IsPreviousHead;
		// コメントの行頭状態保存
		Adjacent[Comment.id].IsLineHead = IsPreviousHead;
		PreviousEnd = ts_node_end_byte(Comment);
	}
	// CSS の符号付数へ密着するコメント列を逆順に１度判定し，後続の結果を継承
	if(IsCssSource) {
		// CSS コメントの直後に在る字句位置の取得
		uint32_t NextStart = std::numeric_limits<uint32_t>::max();
		bool IsNextChain = false;
		for(size_t Idx = CommentOrder.size(); Idx--;) {
			const TSNode Comment = CommentOrder[Idx];
			const uint32_t End = ts_node_end_byte(Comment);
			uint32_t After = End;
			while(After < Self.size() && (Self[After] == ' ' || Self[After] == '\t')) ++After;
			IsNextChain = IsSignAt(End) || After == NextStart && IsNextChain;
			// 符号密着列の状態保存
			Adjacent[Comment.id].IsSignChain = IsNextChain;
			NextStart = ts_node_start_byte(Comment);
		}
	}
	const uint32_t RootChildCount = ts_node_child_count(RootNode);
	// 原文順に統合する紐付候補
	std::vector<Pending> PendingList;
	std::vector<DeleteRange> DeleteRanges;
	const bool IsRootComment = NodeKind::Comment.Contains(RootNode);
	// 並列化の固定費を避ける小さな木とコメント単体の逐次処理
	if(const size_t NumThreads = IsRootComment ? 1 : Parallel::DecideThreads(RootChildCount, 8); NumThreads < 2) {
		// 逐次走査の再利用状態
		LineScan LastScan;
		// Root 自身が Comment の時も処理対象への包含
		if(IsRootComment) ProcessComment(RootNode, {}, PendingList, DeleteRanges, LastScan);
		ForEachChild(
			RootNode,
			[&](const TSNode Child) -> void {
				// 最上位子の走査処理への振分
				DispatchTop(Child, PendingList, DeleteRanges, LastScan);
			}
		);
	} else {
		// `ts_node_child(RootNode, Idx)` の累積 O(N²) を避ける為，１度の TSTreeCursor でベクタに収集
		std::vector<TSNode> RootKids;
		RootKids.reserve(RootChildCount);
		ForEachChild(
			RootNode,
			[&](const TSNode Child) -> void {
				// 根直下の子の原文順収集
				RootKids.push_back(Child);
			}
		);
		std::vector<std::vector<Pending>> ThreadPending(NumThreads);
		std::vector<std::vector<DeleteRange>> ThreadDeletes(NumThreads);
		// 走脈別の再利用状態
		std::vector<LineScan> ThreadScans(NumThreads);
		// コメント収集中の同期を不要にする走脈別書込先の分離
		Parallel::ForChunks(
			RootChildCount,
			NumThreads,
			[&](const size_t ChunkStart, const size_t ChunkEnd, const size_t Tid) -> void {
				// 担当区間の実行
				for(size_t Idx = ChunkStart; Idx < ChunkEnd; ++Idx) {
					DispatchTop(RootKids[Idx], ThreadPending[Tid], ThreadDeletes[Tid], ThreadScans[Tid]);
				}
			}
		);
		size_t TotalPending = 0, TotalDeletes = 0;
		for(const std::vector<Pending> &Pendings : ThreadPending) TotalPending += Pendings.size();
		for(const std::vector<DeleteRange> &Deletes : ThreadDeletes) TotalDeletes += Deletes.size();
		PendingList.reserve(TotalPending);
		// 削除範囲の統合領域予約
		DeleteRanges.reserve(TotalDeletes);
		for(std::vector<Pending> &Local : ThreadPending) {
			PendingList.insert(PendingList.end(), std::make_move_iterator(Local.begin()), std::make_move_iterator(Local.end()));
		}
		for(const std::vector<DeleteRange> &Local : ThreadDeletes) DeleteRanges.insert(DeleteRanges.end(), Local.begin(), Local.end());
	}
	// 並列収集の完了順に依らない原文コメント順への復元
	std::stable_sort(
		PendingList.begin(),
		PendingList.end(),
		[](const Pending &Lhs, const Pending &Rhs) -> bool {
			// 左右要素の比較
			return Lhs.CommentStart < Rhs.CommentStart;
		}
	);
	// 原文で連続する行コメントだけの統合と文書内コード範囲の確定
	for(size_t Begin = 0; Begin < PendingList.size();) {
		// 保留コメントの連続群への区分
		if(PendingList[Begin].IsVerbatim) {
			++Begin;
			continue;
		}
		size_t End = Begin + 1;
		while(End < PendingList.size()) {
			const Pending &Previous = PendingList[End - 1], &Next = PendingList[End];
			if(
				!Previous.ShouldNormalizeLeading || !Next.ShouldNormalizeLeading || Next.IsVerbatim || Next.CommentStart < Previous.CommentEnd
			) break;
			if(
				const std::string_view Gap(data() + Previous.CommentEnd, Next.CommentStart - Previous.CommentEnd);
				!Postprocess::CanJoinComments(Previous.Comment.Text, Next.Comment.Text, Gap)
			) break;
			++End;
		}
		// 独立コメントの行末配置に備えた本文の保持
		const auto IsJoinable = [](const Pending &Entry) -> bool {
			// 独立行へ分けられるコメントの判定
			const std::string &Text = Entry.Comment.Text;
			// 行を分けて正規化し，行末へも置かれ得るコメントかの返戻
			return Entry.ShouldNormalizeLeading && (Text.find("。") != std::string::npos || Text.find("．") != std::string::npos);
		};
		std::vector<CommentAttach> Group, Joined;
		Group.reserve(End - Begin);
		bool HasJoinable = false;
		for(size_t Idx = Begin; Idx < End; ++Idx) {
			HasJoinable |= IsJoinable(PendingList[Idx]);
			Group.push_back({ std::move(PendingList[Idx].Comment.Text), PendingList[Idx].ShouldNormalizeLeading });
		}
		std::vector<CommentAttach *> Pointers;
		if(HasJoinable) {
			// 行末配置への切替に備えた句点分割前の代替本文生成
			Joined.reserve(Group.size());
			for(const CommentAttach &Comment : Group) Joined.push_back({ Comment.Text, false });
			for(CommentAttach &Comment : Joined) Pointers.push_back(&Comment);
			Postprocess::NormalizeCommentGroup(Pointers);
			Pointers.clear();
		}
		for(CommentAttach &Comment : Group) Pointers.push_back(&Comment);
		Postprocess::NormalizeCommentGroup(Pointers);
		for(size_t Idx = Begin; Idx < End; ++Idx) {
			std::string &Text = Group[Idx - Begin].Text;
			const bool ShouldBlockify = PendingList[Idx].ShouldBlockify;
			if(ShouldBlockify) Text = Postprocess::LineCommentToBlock(Text);
			// 群に分割が有れば全正規化コメントの非分割形を保持（本文と同じなら省略）
			if(HasJoinable && PendingList[Idx].ShouldNormalizeLeading) {
				std::string &Alternative = Joined[Idx - Begin].Text;
				if(ShouldBlockify) Alternative = Postprocess::LineCommentToBlock(Alternative);
				if(Alternative != Text) PendingList[Idx].Comment.Joined = std::move(Alternative);
			}
			PendingList[Idx].Comment.Text = std::move(Text);
		}
		Begin = End;
	}
	// 統合後の原文順による出力通し番号の確定
	for(size_t Idx = 0; Idx < PendingList.size(); ++Idx) PendingList[Idx].Comment.Seq = static_cast<uint32_t>(Idx);
	// 削除範囲の昇順整列（ShiftPos の二分探索が前提）
	std::sort(
		DeleteRanges.begin(),
		DeleteRanges.end(),
		[](const DeleteRange &Lhs, const DeleteRange &Rhs) -> bool {
			// 左右要素の比較
			return Lhs.Begin < Rhs.Begin;
		}
	);
	// 重複・隣接する範囲を結合（PrefixLen/ShiftPos が重なりで２重計上するのを防ぐ）
	if(!DeleteRanges.empty()) {
		// 圧縮済範囲の末尾位置
		size_t Write = 0;
		// 後段の座標写像を一意にする整列済範囲のその場圧縮
		for(size_t Read = 1; Read < DeleteRanges.size(); ++Read) {
			if(DeleteRanges[Read].Begin <= DeleteRanges[Write].End) {
				if(DeleteRanges[Read].End > DeleteRanges[Write].End) DeleteRanges[Write].End = DeleteRanges[Read].End;
				DeleteRanges[Write].IsIdentifierBoundary = DeleteRanges[Write].IsIdentifierBoundary || DeleteRanges[Read].IsIdentifierBoundary;
			} else DeleteRanges[++Write] = DeleteRanges[Read];
		}
		DeleteRanges.resize(Write + 1);
	}
	// 字句の融合境界へ空白を補い，残す１バイトを除いた累積削除長で錨を調整
	std::vector<uint32_t> PrefixLen(DeleteRanges.size() + 1, 0);
	std::vector<char> SepInserted(DeleteRanges.size(), 0);
	for(size_t Idx = 0; Idx < DeleteRanges.size(); ++Idx) {
		// 削除範囲の両端
		const uint32_t RangeBegin = DeleteRanges[Idx].Begin, RangeEnd = DeleteRanges[Idx].End;
		// 削除範囲の隣接字
		const char Before = RangeBegin ? data()[RangeBegin - 1] : '\0', After = RangeEnd < size() ? data()[RangeEnd] : '\0';
		// コメント除去で演算子・単位を融合させず，CSS の加減算と値の境界を保持
		const bool IsTokenBoundary = IsIdentifierChar(Before) && IsIdentifierChar(After) ||
		!DeleteRanges[Idx].IsIdentifierBoundary && Before && After && !std::isspace(static_cast<unsigned char>(Before)) &&
		!std::isspace(static_cast<unsigned char>(After));
		const std::string_view Removed(data() + RangeBegin, RangeEnd - RangeBegin);
		// 後続解析迄の改行含有コメントの自動セミコロン挿入境界保持
		const bool IsLineBoundary = !IsCssSource && (
			Removed.find('\n') != std::string_view::npos || IsJsxSource && (
				Removed.find('\r') != std::string_view::npos || Removed.find("\u2028") != std::string_view::npos ||
				Removed.find("\u2029") != std::string_view::npos
			)
		);
		SepInserted[Idx] = IsLineBoundary ? '\n' : IsTokenBoundary ? ' ' : '\0';
		PrefixLen[Idx + 1] = PrefixLen[Idx] + RangeEnd - RangeBegin - (SepInserted[Idx] != '\0');
	}
	const auto ShiftPos = [&DeleteRanges, &PrefixLen](const uint32_t OldPos) -> uint32_t {
		// 最初に始まる範囲を二分探索し，直前までの累積削除量を得る
		const std::vector<DeleteRange>::const_iterator Iter = std::lower_bound(
			DeleteRanges.begin(),
			DeleteRanges.end(),
			OldPos,
			[](const DeleteRange &Range, const uint32_t Pos) -> bool {
				// 対象位置の処理
				return Range.Begin < Pos;
			}
		);
		// 対象位置直後の範囲番号
		const size_t RangeIdx = static_cast<size_t>(Iter - DeleteRanges.begin());
		// OldPos の直前削除範囲内に在る場合
		if(RangeIdx && DeleteRanges[RangeIdx - 1].End > OldPos) return DeleteRanges[RangeIdx - 1].Begin - PrefixLen[RangeIdx - 1];
		// 範囲外位置は通常の累積前置分だけ前方へ移動の返戻
		return OldPos - PrefixLen[RangeIdx];
	};
	// 全削除範囲の一括適用による新ソースの構築
	if(!DeleteRanges.empty()) {
		// 削除範囲を除く本文の再構築
		std::string NewSource;
		// 原文長の領域予約
		NewSource.reserve(size());
		uint32_t Pos = 0;
		// 未削除区間と必要区切りだけの順次連結
		for(size_t Idx = 0; Idx < DeleteRanges.size(); ++Idx) {
			// 今回除く範囲
			const DeleteRange &Range = DeleteRanges[Idx];
			if(Range.Begin > Pos) NewSource.append(data() + Pos, Range.Begin - Pos);
			// 区切りの要否は PrefixLen と同じ判定結果を使い，位置写像と実際の構築を食い違わせない
			if(SepInserted[Idx]) NewSource += SepInserted[Idx];
			Pos = Range.End;
		}
		if(Pos < size()) NewSource.append(data() + Pos, size() - Pos);
		Assign(std::move(NewSource));
	}
	// コメント削除後のソースを Reparse し，新構文木でアンカーを再配置
	if(!IsParsed()) return;
	const TSNode NewRoot = GetRoot();
	const std::string &NewSrc = *this;
	// 開始差が空白だけなら一致扱いとし，SCSS の先行改行取込による削除後のずれを救済
	const auto BlankStart = [&NewSrc](uint32_t Pos) -> uint32_t {
		// 対象位置の処理
		while(Pos && (NewSrc[Pos - 1] == ' ' || NewSrc[Pos - 1] == '\t' || NewSrc[Pos - 1] == '\n' || NewSrc[Pos - 1] == '\r')) --Pos;
		// 直前の空白の並びの始まりの返戻
		return Pos;
	};
	// 錨開始・直前空白で引く型別の範囲索引を１度構築し，最深候補と空白位置を再利用
	std::unordered_set<uint32_t> Starts;
	std::unordered_map<uint32_t, uint32_t> LowerOf;
	// 錨別の空白許容範囲の重複無索引化
	for(const Pending &Entry : PendingList) {
		// 編集後の錨位置とコメント位置の算出
		const uint32_t NewAnchorStart = ShiftPos(Entry.AnchorStart);
		if(const auto [Slot, IsNew] = LowerOf.try_emplace(NewAnchorStart); IsNew) {
			// 錨前空白の下限保存
			Slot->second = BlankStart(NewAnchorStart);
			for(uint32_t Pos = Slot->second; Pos <= NewAnchorStart; ++Pos) Starts.insert(Pos);
		}
	}
	struct TypeIndex {
		std::unordered_map<uint64_t, TSNode> Exact; // （開始，終わり）の一致する最も深い節点
		std::unordered_map<uint32_t, TSNode> Loose;
	};
	// tree-sitter の型文字列は文法定義の静的領域を指し，ポインタで引ける
	std::unordered_map<const char *, TypeIndex> ByType;
	const auto IndexNode = [&Starts, &ByType](const TSNode Node) -> void {
		// 開始位置と型に依る節点索引の構築
		const uint32_t StartB = ts_node_start_byte(Node);
		// 索引対象外の節点の返戻
		if(!ts_node_is_named(Node) || !Starts.contains(StartB)) return;
		TypeIndex &Index = ByType[ts_node_type(Node)];
		Index.Exact[static_cast<uint64_t>(StartB) << 32 | ts_node_end_byte(Node)] = Node;
		Index.Loose[StartB] = Node;
	};
	// 通常の子孫走査に含まれない根の索引への先行登録
	IndexNode(NewRoot);
	WalkChildrenCursor(
		NewRoot,
		[&IndexNode](const TSNode Node) -> bool {
			// 新構文木の節点の索引登録
			IndexNode(Node);
			// 全ての子孫を走査する事の返戻
			return true;
		}
	);
	// 錨（型・開始・終わり）毎の再解析後の節点（同じ錨のコメントは同じ節点に為り，コメント毎に空白を遡って探し直さない）
	std::map<std::tuple<const char *, uint32_t, uint32_t>, TSNode> Resolved;
	// 同じ旧錨を共有するコメント間での解決結果再利用
	for(Pending &Entry : PendingList) {
		// アンカーの開始位置は削除分の前ずれを ShiftPos で新座標へ変換
		const uint32_t NewAnchorStart = ShiftPos(Entry.AnchorStart), NewAnchorEnd = ShiftPos(Entry.AnchorEnd);
		// 同開始・型の最深節点で範囲一致を優先し，終端だけ異なる候補へ退避
		const auto [Slot, IsNew] = Resolved.try_emplace({ Entry.AnchorType, NewAnchorStart, NewAnchorEnd });
		TSNode &Target = Slot->second;
		if(IsNew) {
			if(
				const std::unordered_map<const char *, TypeIndex>::const_iterator Iter = ByType.find(Entry.AnchorType);
				Iter != ByType.end()
			) {
				TSNode Fallback {};
				for(uint32_t Pos = NewAnchorStart + 1, Lower = LowerOf[NewAnchorStart]; Pos-- > Lower && ts_node_is_null(Target);) {
					if(
						const std::unordered_map<uint64_t, TSNode>::const_iterator Hit =
						Iter->second.Exact.find(static_cast<uint64_t>(Pos) << 32 | NewAnchorEnd);
						Hit != Iter->second.Exact.end()
					) Target = Hit->second;
					if(ts_node_is_null(Fallback)) {
						if(
							const std::unordered_map<uint32_t, TSNode>::const_iterator Hit = Iter->second.Loose.find(Pos);
							Hit != Iter->second.Loose.end()
						) Fallback = Hit->second;
					}
				}
				// 開始一致候補への退避
				if(ts_node_is_null(Target)) Target = Fallback;
			}
		}
		// コメントのみで再解析後が空白になり子孫が無ければ，元の根相当の錨を NewRoot の末尾へ復元
		if(ts_node_is_null(Target)) Target = NewRoot;
		if(ts_node_is_null(Target)) continue;
		// NewRoot 代替経路のコメントはルート紐付として末尾コメント扱い（末尾出力経路で復元される）
		if(ts_node_eq(Target, NewRoot)) Entry.Comment.IsLeading = false;
		if(Entry.Comment.IsLeading) Attachments.LeadingByNode[Target.id].push_back(std::move(Entry.Comment));
		else Attachments.TrailingByNode[Target.id].push_back(std::move(Entry.Comment));
		Attachments.AttachedStartBytes.insert(ts_node_start_byte(Target));
	}
	// 終了
	return;
}

/**
 * 先行／末尾コメント検索の共通関数
 * @param Map 参照する紐付マップ（先行又は末尾）
 * @param Node 対象ノード
 * @return 紐付けられたコメント列（無ければ静的空配列への参照）
 */
const std::vector<CommentAttach> &TSSource::FindAttachments(
	const std::unordered_map<const void *, std::vector<CommentAttach>> &Map,
	const TSNode Node
) {
	// 未紐付時の共有空列
	static constexpr std::vector<CommentAttach> Empty;
	const std::unordered_map<const void *, std::vector<CommentAttach>>::const_iterator Iter = Map.find(Node.id);
	// Node に紐付くコメントの返戻（未紐付なら静的空配列）
	return Iter == Map.end() ? Empty : Iter->second;
}

/**
 * 先行コメント取得関数
 * @param Node 対象ノード
 * @return 紐付けられたコメント列（無ければ空リストへの参照）
 */
const std::vector<CommentAttach> &TSSource::GetLeading(const TSNode Node) const {
	// 先行紐付マップの共通検索へ委譲の返戻
	return FindAttachments(Attachments.LeadingByNode, Node);
}

/**
 * 後続コメント取得関数
 * @param Node 対象ノード
 * @return 紐付けられたコメント列（無ければ空リストへの参照）
 */
const std::vector<CommentAttach> &TSSource::GetTrailing(const TSNode Node) const {
	// 末尾紐付マップの共通検索へ委譲の返戻
	return FindAttachments(Attachments.TrailingByNode, Node);
}

/**
 * 範囲内紐付コメント有無の判定関数
 * @param From 範囲開始（含む）
 * @param To 範囲終了（含まず）
 * @return 範囲内に紐付済コメントが有る場合 true
 */
bool TSSource::HasAttachedCommentInRange(const uint32_t From, const uint32_t To) const {
	// アンカー開始バイト昇順集合の lower_bound による範囲検査
	const std::set<uint32_t>::const_iterator Iter = Attachments.AttachedStartBytes.lower_bound(From);
	// 範囲内に紐付済コメントが有るかの結果の返戻
	return Iter != Attachments.AttachedStartBytes.end() && *Iter < To;
}

/**
 * 紐付状態の全消去関数
 * コメントを消費し終えた後に呼び出す
 */
void TSSource::ClearAttachments() {
	// 現在木の紐付破棄
	Attachments.ClearNodes();
	Attachments.PendingSnapshot.Clear();
	// 終了
	return;
}

/**
 * 先行コメント取出除去関数（複製と除去の統合形）
 * @param Node 対象ノード
 * @return 紐付けられていたコメント列（無ければ空）
 */
std::vector<CommentAttach> TSSource::TakeLeading(const TSNode Node) {
	// 先行紐付の位置
	const std::unordered_map<const void *, std::vector<CommentAttach>>::iterator Iter = Attachments.LeadingByNode.find(Node.id);
	// 未紐付なら空列の返戻
	if(Iter == Attachments.LeadingByNode.end()) return {};
	std::vector<CommentAttach> Result = std::move(Iter->second);
	Attachments.LeadingByNode.erase(Iter);
	// 取り出したコメント列の返戻
	return Result;
}

/**
 * 後続コメント取出除去関数（複製と除去の統合形）
 * @param Node 対象ノード
 * @return 紐付けられていたコメント列（無ければ空）
 */
std::vector<CommentAttach> TSSource::TakeTrailing(const TSNode Node) {
	// 末尾紐付の位置
	const std::unordered_map<const void *, std::vector<CommentAttach>>::iterator Iter = Attachments.TrailingByNode.find(Node.id);
	// 未紐付なら空列の返戻
	if(Iter == Attachments.TrailingByNode.end()) return {};
	std::vector<CommentAttach> Result = std::move(Iter->second);
	Attachments.TrailingByNode.erase(Iter);
	// 取り出したコメント列の返戻
	return Result;
}

/**
 * 紐付追加の共通関数
 * 同じ錨へ二度目を追加する事は有る（統合編集で別々のノードのコメントが同じ位置へ寄る等）為，上書きでなく末尾へ足す
 * 上書きにすると先に置いたコメントが無言で失われる
 * @param Primary 追加対象の紐付マップ
 * @param Node 対象ノード
 * @param Comments 追加するコメント列
 */
void TSSource::AddAttach(
	std::unordered_map<const void *, std::vector<CommentAttach>> &Primary,
	const TSNode Node,
	std::vector<CommentAttach> Comments
) {
	// 錨の既存コメント列
	std::vector<CommentAttach> &Dest = Primary[Node.id];
	Dest.insert(Dest.end(), std::make_move_iterator(Comments.begin()), std::make_move_iterator(Comments.end()));
	Attachments.AttachedStartBytes.insert(ts_node_start_byte(Node));
	// 終了
	return;
}

/**
 * 先行コメント追加関数（手動で紐付を再構築する場合に使用）
 * @param Node 対象ノード
 * @param Comments 既存の紐付の末尾へ足すコメント列
 */
void TSSource::AddLeading(const TSNode Node, std::vector<CommentAttach> Comments) {
	// 先行紐付への追加
	AddAttach(Attachments.LeadingByNode, Node, std::move(Comments));
	// 終了
	return;
}

/**
 * 後続コメント追加関数（手動で紐付を再構築する場合に使用）
 * @param Node 対象ノード
 * @param Comments 既存の紐付の末尾へ足すコメント列
 */
void TSSource::AddTrailing(const TSNode Node, std::vector<CommentAttach> Comments) {
	// 末尾紐付への追加
	AddAttach(Attachments.TrailingByNode, Node, std::move(Comments));
	// 終了
	return;
}

/**
 * 紐付停止区間の開始関数
 * 紐付消費が無い区間で用い，区間中は紐付を読み出してはならない
 */
void TSSource::SuspendAttachments() {
	// 停止計数器を上げる前に紐付を退避し，再解析が無効にした Node.id の参照を防止
	if(!Attachments.SuspendDepth && HasAttachments()) Attachments.SavedSnapshot = FreshSnapshot();
	++Attachments.SuspendDepth;
	// 終了
	return;
}

/**
 * 紐付停止区間の終了関数
 * 最終ソース／構文木へ退避中スナップショットを再紐付する
 */
void TSSource::ResumeAttachments() {
	// ネストした停止の最外終了時だけの再紐付実行
	if(!Attachments.SuspendDepth || --Attachments.SuspendDepth || Attachments.SavedSnapshot.IsEmpty()) return;
	// 未反映代入時の構文木最終化と同時再紐付
	if(Syntax.Parser && Syntax.IsDirty) {
		// 変更前の紐付スナップショットの再利用
		Attachments.PendingSnapshot = std::move(Attachments.SavedSnapshot);
		Reparse();
	} else RebindFromSnapshot(std::move(Attachments.SavedSnapshot));
	Attachments.SavedSnapshot.Clear();
	// 終了
	return;
}

/** ========== 構文情報収集 ========== */
/**
 * 宣言した名前と取込の指令の位置の収集関数 (C / C++)
 * 計算量：原稿長 N，走査節点数 V，収集する名前の総長 B に対し平均 O(N + V + B)
 * 位置は照会する時の構文木の位置と比べる為 (IsDeclaredName)，コメントの除去や編集の後の構文木から照会の直前に集め直す
 * （コメントを除く前に集めると，コメントの長さの分だけ宣言・取込と使う位置の前後が入れ替わり，コメントの有無で括弧の要否が変わる）
 */
void TSSource::CollectDeclaredNames() {
	// 前回の宣言索引破棄
	DeclaredNames.clear();
	IncludeStarts.clear();
	// 構文木が無い場合の返戻
	if(!IsParsed()) return;
	// 前処理指令の有無の事前判定
	const bool HasDirective = find('#') != npos;
	// 前処理条件の終端を外側から積み，定義・取消・条件で使うマクロ名を記録
	std::vector<uint32_t> ConditionEnds;
	std::unordered_set<std::string_view> MacroNames;
	// 宣言した名前の位置との記録
	const auto Declare = [&](const TSNode Name) -> void {
		// 宣言名と開始位置の登録
		DeclaredNames[std::string(View(Name))].push_back(ts_node_start_byte(Name));
	};
	WalkChildrenCursor(
		GetRoot(),
		[&](const TSNode Node) -> bool {
			// 宣言名収集対象の節点種別と位置の取得
			const std::string_view Type = ts_node_type(Node);
			const uint32_t Start = ts_node_start_byte(Node);
			// 現在位置より前に終わった条件範囲の除去
			while(!ConditionEnds.empty() && ConditionEnds.back() <= Start) ConditionEnds.pop_back();
			// 宣言位置でマクロでない名前を収集（前処理条件内はマクロ時に飛ばす枝の可能性が有る為に除外）
			if(ConditionEnds.empty() && Type == "enumerator") Declare(FieldChild(Node, "name"));
			else if(ConditionEnds.empty() && NodeKind::CNameDeclaration.Contains(Type)) {
				// 宣言子を識別子迄解包して宣言名を収集
				ForEachNamedChild(
					Node,
					[&](TSNode Declarator) -> void {
						// 型の子を除き，ネストの宣言子を名前迄解包
						while(!ts_node_is_null(Declarator) && std::string_view(ts_node_type(Declarator)) != "identifier") {
							Declarator = FieldChild(Declarator, "declarator");
						}
						if(!ts_node_is_null(Declarator)) Declare(Declarator);
					}
				);
			}
			// 前処理の指令を持たないソースは子孫走査を続ける事の返戻
			if(!HasDirective) return !NodeKind::Leaf.Contains(Type);
			// 条件範囲の終端追加
			if(NodeKind::PreprocConditional.Contains(Type)) ConditionEnds.push_back(ts_node_end_byte(Node));
			// 条件指令で問う名前もファイル内のマクロ名として記録
			if(NodeKind::PreprocNameQuery.Contains(Type)) {
				if(const TSNode Name = FirstNamedChildOfType(Node, "identifier"); !ts_node_is_null(Name)) MacroNames.insert(View(Name));
			} else if(Type == "preproc_include") IncludeStarts.push_back(Start);
			else if(Type == "preproc_call" && View(FieldChild(Node, "directive")) == "#undef") {
				std::string_view Argument = View(FieldChild(Node, "argument"));
				TextEdit::TrimView(Argument);
				MacroNames.insert(Argument);
			}
			// マクロの定義以外の子孫走査を続ける事の返戻
			if(!NodeKind::MacroDefinition.Contains(Type)) return !NodeKind::Leaf.Contains(Type);
			// 定義マクロ名の記録
			if(const TSNode Name = FieldChild(Node, "name"); !ts_node_is_null(Name)) MacroNames.insert(View(Name));
			// 定義の内側へ降りない事の返戻
			return false;
		}
	);
	for(const std::string_view Name : MacroNames) DeclaredNames.erase(std::string(Name));
	// 終了
	return;
}

/**
 * 前処理マクロの性質（実引数の文字列化・文への展開）の収集関数 (C / C++)
 * 計算量：原稿長 N，節点数 V，定義数 M，単一定義の判定の最大費用 T に対し平均 O(N + V + M * M * T)
 */
void TSSource::CollectMacros() {
	// 前回の文字列化性質破棄
	StringizingMacros.clear();
	StatementMacros.clear();
	DeclaratorMacros.clear();
	ExpressionMacros.clear();
	FunctionMacros.clear();
	// 標準表明マクロの文字列化性質登録
	if(find("assert") != npos) StringizingMacros.emplace("assert");
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
			const std::string_view Type = ts_node_type(Node);
			// マクロの定義以外の子孫走査を続ける事の返戻
			if(!NodeKind::MacroDefinition.Contains(Type)) return !NodeKind::Leaf.Contains(Type);
			// 定義の各場
			const TSNode Name = FieldChild(Node, "name"), Parameters = FieldChild(Node, "parameters"), Value = FieldChild(Node, "value");
			const bool IsFunctionLike = Type == "preproc_function_def";
			// 置換本体の無い定義の内側へ降りない事の返戻
			if(ts_node_is_null(Name) || IsFunctionLike && ts_node_is_null(Parameters) || ts_node_is_null(Value)) return false;
			// 有効な定義の名前・仮引数・本体の収集
			Definition Entry { View(Name), {}, View(Value), IsFunctionLike };
			if(IsFunctionLike) {
				// 可変長仮引数と明示仮引数の登録
				Entry.Parameters.push_back("__VA_ARGS__");
				ForEachNamedChild(
					Parameters,
					[&](const TSNode Parameter) -> void {
						// 関数形式マクロの仮引数名の収集
						if(std::string_view(ts_node_type(Parameter)) == "identifier") Entry.Parameters.push_back(View(Parameter));
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
		// 対象位置の処理
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
			// 呼出の開き括弧迄の空白読飛し
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
			std::vector<size_t> &Users = MentionedBy[Body.substr(Pos, End - Pos)];
			if(Users.empty() || Users.back() != Index) Users.push_back(Index);
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
			// 文字列化するマクロ又は別名の判定
			// 判定済マクロの返戻
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
	const auto ExpandsToStatementBody = [&IdentifierEnd](const std::string_view Body) -> bool {
		// 文へ展開する置換本体の判定
		size_t Lead = 0;
		while(Lead < Body.size() && (Body[Lead] == ' ' || Body[Lead] == '\t')) ++Lead;
		// 文の語で始まる事の返戻
		if(NodeKind::CStatementKeyword.Contains(Body.substr(Lead, IdentifierEnd(Body, Lead) - Lead))) return true;
		// 文字列外の文区切り又は塊開始の探索
		for(size_t Pos = 0; Pos < Body.size(); ++Pos) {
			if(const char Char = Body[Pos]; Char == '"' || Char == '\'') {
				// 引用文字列内の区切り記号の読飛し
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
			// 引用文字列内の演算子の読飛し
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
			// 引用文字列内の名前の読飛し
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
 * パーサ無で文字列のみ保持するコンストラクタ
 * @param Str ソース文字列
 */
TSSource::TSSource(std::string Str) : std::string(std::move(Str)) {
	// 終了
	return;
}

/**
 * 言語指定でパーサを生成しソースを解析するコンストラクタ
 * @param Str ソース文字列
 * @param Lang tree-sitter の言語定義（nullptr なら解析しない）
 */
TSSource::TSSource(std::string Str, const TSLanguage *const Lang) : std::string(std::move(Str)) {
	// 言語指定が無い場合の返戻
	if(!Lang) return;
	// ３２ビット位置に収まらない原稿のパーサ生成前の拒否
	if(size() > std::numeric_limits<uint32_t>::max()) throw std::length_error("source exceeds the parser's position range");
	Syntax.Parser.reset(ts_parser_new());
	// 文法と実行時ライブラリの非互換時に於ける未解析状態での返却
	if(!ts_parser_set_language(Syntax.Parser.get(), Lang)) return;
	// スナップショット・再束縛時の名前場照合に使う `name` 場識別子の単一解決
	Syntax.NameFieldId = ts_language_field_id_for_name(Lang, "name", 4);
	ReplaceTree(ParseWithDeadline(Syntax.Parser.get(), nullptr, *this, Syntax.IsParseExpired));
	// 終了
	return;
}
