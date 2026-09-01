#include "Layout.hpp"
#include "../Util/BracketPairs.hpp"
#include "../Util/DocSig.hpp"
#include "../Util/FileIO.hpp"
#include "../Util/HtmlTag.hpp"
#include "../Util/NodeKind.hpp"
#include "../Util/Parallel.hpp"
#include "../Util/Postprocess.hpp"
#include "../Util/TextEdit.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/** ========== 行・字下げ ========== */
/**
 * 行情報の構築関数
 * @param Src ソースコード
 * @param LineStarts 行開始位置の配列（出力）
 * @param ContentStarts 内容開始位置の配列（出力）
 */
void LayoutPass::BuildLineInfo(const TSSource &Src, std::vector<uint32_t> &LineStarts, std::vector<uint32_t> &ContentStarts) {
	// 原文参照の取得
	const std::string &Source = Src;
	const uint32_t Size = static_cast<uint32_t>(Source.size());
	// 同一添字で参照する行頭と内容開始位置の再構築
	LineStarts.clear();
	ContentStarts.clear();
	// 平均行幅から行数を見積った両位置列の容量確保
	const size_t Reserve = (static_cast<size_t>(Size) >> 5) + 1;
	LineStarts.reserve(Reserve);
	ContentStarts.reserve(Reserve);
	// 改行で終わらない末尾行を含む全行の走査
	uint32_t LineStart = 0;
	while(true) {
		// 次の改行位置
		const size_t Found = Source.find('\n', LineStart);
		const uint32_t LineEnd = Found == std::string::npos ? Size : static_cast<uint32_t>(Found);
		uint32_t ContentStart = LineStart;
		while(ContentStart < LineEnd && (Source[ContentStart] == ' ' || Source[ContentStart] == '\t')) ++ContentStart;
		// 行頭位置列への追加
		LineStarts.push_back(LineStart);
		// 内容開始位置列への追加
		ContentStarts.push_back(ContentStart);
		if(Found == std::string::npos) break;
		LineStart = static_cast<uint32_t>(Found + 1);
	}
	// 終了
	return;
}

/**
 * バイト位置の行番号の取得関数
 * @param LineStarts 行開始位置の配列
 * @param Byte バイト位置
 * @return 行インデックス
 */
size_t LayoutPass::LineIndexAt(const std::vector<uint32_t> &LineStarts, const uint32_t Byte) {
	const std::vector<uint32_t>::const_iterator Iter = std::upper_bound(LineStarts.begin(), LineStarts.end(), Byte);
	// 対象バイトを含む行の添字の返戻
	return Iter == LineStarts.begin() ? 0 : static_cast<size_t>(std::prev(Iter) - LineStarts.begin());
}

/**
 * レイアウト用ノード情報の収集関数
 * @param Src ソースコード
 * @param LineStarts 行開始位置の配列
 * @param InCommentLines コメント内部行の開始位置集合（結果格納先）
 * @param InHeredocContentLines ヒアドキュメント本文行の開始位置集合（結果格納先）
 * @param SectionStartBytes セクション見出開始位置集合（不要なら nullptr）
 * @param LeafOpenerLines 複数行文字列リテラルの開始行集合（末尾空白の逐語保持用，不要なら nullptr）
 */
void LayoutPass::CollectLayoutNodeInfo(
	const TSSource &Src,
	const std::vector<uint32_t> &LineStarts,
	std::unordered_set<uint32_t> &InCommentLines,
	std::unordered_set<uint32_t> &InHeredocContentLines,
	std::unordered_set<uint32_t> *const SectionStartBytes,
	std::unordered_set<uint32_t> *const LeafOpenerLines
) {
	// FinalizeLayout から渡される解析済ソースの局所情報型
	struct LocalInfo {
		std::vector<uint32_t> CommentLines;
		std::vector<uint32_t> HeredocContentLines;
		std::vector<uint32_t> SectionStarts;
		std::vector<uint32_t> LeafOpenerLines;
	};
	const TSNode Root = Src.GetRoot();
	const auto PushInteriorLineStarts = [&LineStarts](const TSNode Node, std::vector<uint32_t> &Local) -> void {
		// 対象範囲
		const uint32_t Start = ts_node_start_byte(Node), End = ts_node_end_byte(Node);
		// 範囲内の行頭位置の収集
		for(
			// 探索位置
			std::vector<uint32_t>::const_iterator Iter = std::upper_bound(LineStarts.begin(), LineStarts.end(), Start);
			Iter != LineStarts.end() && *Iter < End;
			++Iter // 走脈内の収集列への追加
		) Local.push_back(*Iter);
	};
	// Ruby `heredoc_body` の全行を逐語登録し，元ソースのバイト列を完全に保持して出力
	const auto VisitHeredoc = [&LineStarts](const TSNode Node, std::vector<uint32_t> &Local) -> void {
		// 終端行フラグ不要時の未参照 SrcSize への 0 指定
		ForEachHeredocBodyLine(
			Node,
			LineStarts,
			0U,
			[&](const uint32_t LineStart) -> void {
				// 終端タグも含めて全行を登録する（終端行フラグ不要の１引数形で終端タグ探索を省略）
				Local.push_back(LineStart);
			}
		);
	};
	// `ERROR` を祖先に持つ葉の誤登録防止用の走査除外
	const auto IsError = [](const TSNode Cur) -> bool {
		// `ERROR` 節点該当状態の返戻
		return std::string_view(ts_node_type(Cur)) == "ERROR";
	};
	// 複数行葉と HTML 内容の逐語保持及び再構築対象の除外
	const bool HasRootError = ts_node_has_error(Root), IsProgramRoot = std::string_view(ts_node_type(Root)) == "program";
	const auto VisitNode = [
		&PushInteriorLineStarts,
		&VisitHeredoc,
		&IsError,
		SectionStartBytes,
		LeafOpenerLines,
		&Src,
		&LineStarts,
		HasRootError,
		IsProgramRoot
	](const TSNode Node, LocalInfo &Local) -> void {
		// 節点型名
		const std::string_view TypeView(ts_node_type(Node));
		bool IsVerbatimOpener = LeafOpenerLines && NodeKind::Leaf.Contains(TypeView) && !NodeKind::NonVerbatimLeaf.Contains(TypeView);
		if(NodeKind::Comment.Contains(TypeView)) PushInteriorLineStarts(Node, Local.CommentLines);
		else if(TypeView == "heredoc_body") VisitHeredoc(Node, Local.HeredocContentLines);
		else if(NodeKind::Leaf.Contains(TypeView)) {
			// `ERROR` の無い構文木での葉毎の祖先遡及の省略
			if(!NodeKind::AngleBracketList.Contains(TypeView) && (!HasRootError || !HasAncestorOf(Node, IsError))) {
				PushInteriorLineStarts(Node, Local.CommentLines);
			}
		} else if(
			// 地の文の親節点
			const TSNode TextParent = IsProgramRoot && TypeView == "text" ? ts_node_parent(Node) : TSNode{};
			!ts_node_is_null(TextParent) && std::string_view(ts_node_type(TextParent)) == "program"
		) {
			const uint32_t TextEnd = ts_node_end_byte(Node);
			for(size_t LineIdx = 0; LineIdx < LineStarts.size() && LineStarts[LineIdx] < TextEnd; ++LineIdx) {
				// 走脈内のコメント行列への追加
				Local.CommentLines.push_back(LineStarts[LineIdx]);
			}
		} else if(TypeView == "element" && HtmlVerbatimOf(Src, Node) != HtmlVerbatim::None) {
			// HTML `<pre>` / `<textarea>` 等の内容の行の逐語化
			PushInteriorLineStarts(Node, Local.CommentLines);
			// 開始タグ行の末尾空白の逐語内容としての保持
			IsVerbatimOpener = LeafOpenerLines;
		} else if(SectionStartBytes && ts_node_is_named(Node) && NodeKind::SectionHeader.Contains(TypeView)) {
			// 走脈内の節見出位置列への追加
			Local.SectionStarts.push_back(ts_node_start_byte(Node));
		}
		// 複数行逐語要素の開始行末尾空白の保護
		if(IsVerbatimOpener) {
			// 逐語要素の範囲
			const uint32_t OpenerStart = ts_node_start_byte(Node), OpenerEnd = ts_node_end_byte(Node);
			if(
				// 逐語要素の開始行
				const size_t OpenerLine = LineIndexAt(LineStarts, OpenerStart);
				OpenerEnd > OpenerStart && OpenerLine != LineIndexAt(LineStarts, OpenerEnd - 1)
				// 走脈内の逐語要素開始行列への追加
			) Local.LeafOpenerLines.push_back(LineStarts[OpenerLine]);
		}
	};
	// 部分木の根配下を１つの TSTreeCursor で反復走査して Local に追記
	const auto WalkSubtree = [&VisitNode](const TSNode SubRoot, LocalInfo &Local) -> void {
		// 構文木の走査カーソル
		HeldCursor Cursor(SubRoot);
		while(true) {
			// 現在節点の情報収集
			VisitNode(ts_tree_cursor_current_node(&Cursor), Local);
			if(ts_tree_cursor_goto_first_child(&Cursor)) continue;
			while(!ts_tree_cursor_goto_next_sibling(&Cursor)) {
				// 部分木の根へ戻った場合の返戻
				if(!ts_tree_cursor_goto_parent(&Cursor) || ts_node_eq(ts_tree_cursor_current_node(&Cursor), SubRoot)) return;
			}
		}
	};
	// 単一・根・走脈毎の収集結果を出力集合へ統合
	const auto MergeLocal =
	[&InCommentLines, &InHeredocContentLines, SectionStartBytes, LeafOpenerLines](const LocalInfo &Local) -> void {
		// コメント内部行集合への登録
		for(const uint32_t Pos : Local.CommentLines) InCommentLines.insert(Pos);
		// ヒアドキュメント本文行集合への登録
		for(const uint32_t Pos : Local.HeredocContentLines) InHeredocContentLines.insert(Pos);
		// 任意出力集合への登録
		if(SectionStartBytes) for(const uint32_t Pos : Local.SectionStarts) SectionStartBytes->insert(Pos);
		if(LeafOpenerLines) for(const uint32_t Pos : Local.LeafOpenerLines) LeafOpenerLines->insert(Pos);
	};
	// 根直下の名前付子数
	const uint32_t RootChildCount = ts_node_named_child_count(Root);
	const size_t NumThreads = NodeKind::Comment.Contains(Root) ? 1 : Parallel::DecideThreads(RootChildCount, 8);
	if(NumThreads < 2) {
		// 単一走脈の収集領域とコメント行容量の準備
		LocalInfo Local;
		Local.CommentLines.reserve((LineStarts.size() >> 3) + 1);
		// 走脈内の節見出位置列の容量確保
		if(SectionStartBytes) Local.SectionStarts.reserve((LineStarts.size() >> 5) + 1);
		// 部分木の情報収集
		WalkSubtree(Root, Local);
		// 局所収集結果の統合
		MergeLocal(Local);
		// 終了
		return;
	}
	// 走脈別収集領域による共有集合への同時書込の回避
	std::vector<LocalInfo> ThreadLocal(NumThreads);
	for(LocalInfo &Local : ThreadLocal) {
		// 走脈内のコメント行列の容量確保
		Local.CommentLines.reserve(LineStarts.size() / NumThreads + 1);
		// 走脈内の節見出位置列の容量確保
		if(SectionStartBytes) Local.SectionStarts.reserve((LineStarts.size() >> 5) / NumThreads + 1);
	}
	// 根の単一走脈処理と子からの並列化
	LocalInfo RootLocal;
	// 現在節点の情報収集
	VisitNode(Root, RootLocal);
	// Root の名前付子を１度の TSTreeCursor で収集
	std::vector<TSNode> RootChildren;
	// 根直下の名前付子列の容量確保
	RootChildren.reserve(RootChildCount);
	ForEachNamedChild(
		Root,
		[&](const TSNode Child) -> void {
			// 根直下の名前付子列への追加
			RootChildren.push_back(Child);
		}
	);
	Parallel::ForChunks(
		RootChildren.size(),
		NumThreads,
		[&](const size_t Start, const size_t End, const size_t Tid) -> void {
			LocalInfo &Local = ThreadLocal[Tid];
			// 部分木の情報収集
			for(size_t Idx = Start; Idx < End; ++Idx) WalkSubtree(RootChildren[Idx], Local);
		}
	);
	// 根と全走脈の結果の合流後の出力集合への統合
	MergeLocal(RootLocal);
	// 局所収集結果の統合
	for(const LocalInfo &Local : ThreadLocal) MergeLocal(Local);
	// 終了
	return;
}

/**
 * 行インデント深さの設定関数
 * @param Indents インデント深さマップ
 * @param LineStart 行の開始位置
 * @param Depth インデント深さ
 */
void LayoutPass::SetLineIndent(std::unordered_map<uint32_t, int> &Indents, const uint32_t LineStart, const int Depth) {
	// 登録済行での浅いインデント深さの採用
	const auto [Iter, Inserted] = Indents.emplace(LineStart, Depth);
	if(!Inserted && Depth < Iter->second) Iter->second = Depth;
	// 終了
	return;
}

/**
 * トークン単位のインデント収集関数
 * @param Src ソースコード
 * @param Node 対象ノード
 * @param LineStarts 行開始位置の配列
 * @param LineContentStarts 内容開始位置の配列
 * @param Depth 現在のインデント深さ
 * @param Indents インデント深さマップ
 */
void LayoutPass::CollectTokenIndents(
	const TSSource &Src,
	const TSNode Node,
	const std::vector<uint32_t> &LineStarts,
	const std::vector<uint32_t> &LineContentStarts,
	const int Depth,
	std::unordered_map<uint32_t, int> &Indents
) {
	// 行頭に来た閉じ括弧のみ親の深さへの統一
	ForEachChild(
		Node,
		[&](const TSNode Child) -> void {
			// 条件成立時の返戻
			if(ts_node_is_named(Child)) return;
			// 無名字句の範囲取得
			const uint32_t Start = Src.Start(Child);
			// 条件成立時の返戻
			if(Start >= Src.End(Child)) return;
			// 行番号の二分探索を１回に集約し，行頭判定とインデント設定で共有
			const size_t LineIdx = LineIndexAt(LineStarts, Start);
			// 条件成立時の返戻
			if(Start > LineContentStarts[LineIdx]) return;
			// 対象行のインデント登録
			if(NodeKind::LayoutCloserToken.Contains(Src.View(Child))) SetLineIndent(Indents, LineStarts[LineIdx], Depth);
		}
	);
	// 終了
	return;
}

/**
 * バイト位置の行頭オフセットの取得関数
 * @param LineStarts 行開始位置の配列
 * @param Byte バイト位置
 * @return 行の開始バイト位置
 */
uint32_t LayoutPass::LineStartAt(const std::vector<uint32_t> &LineStarts, const uint32_t Byte) {
	// 行頭位置の返戻
	return LineStarts[LineIndexAt(LineStarts, Byte)];
}

/**
 * 行インデントの収集関数
 * @param Src ソースコード
 * @param Node 走査対象のノード
 * @param Language 対象言語
 * @param LineStarts 行開始位置の配列
 * @param LineContentStarts 内容開始位置の配列
 * @param Depth 現在のインデント深さ
 * @param OuterContIndent 親スコープからの継続行インデント（Ruby の継続構文用，-1 で未指定）
 * @param Indents インデント深さマップ（結果格納先）
 */
void LayoutPass::CollectLineIndents(
	const TSSource &Src,
	const TSNode Node,
	const Lang Language,
	const std::vector<uint32_t> &LineStarts,
	const std::vector<uint32_t> &LineContentStarts,
	const int Depth,
	const int OuterContIndent,
	std::unordered_map<uint32_t, int> &Indents
) {
	// 条件成立時の返戻
	if(ts_node_is_null(Node)) return;
	// 深いネストでの新しい走脈による収集継続
	const RecursionGuard Guard;
	if(Guard.IsOverflow) {
		Parallel::RunOnFreshStack(
			[&]() -> void {
				// 子節点のインデント収集
				CollectLineIndents(Src, Node, Language, LineStarts, LineContentStarts, Depth, OuterContIndent, Indents);
			}
		);
		// 新しい走脈での収集完了後の終了
		return;
	}
	// コンテナ節点での深さ加算
	const auto SetIfLineStart = [&LineStarts, &LineContentStarts, &Indents](const uint32_t Pos, const int IndentDepth) -> void {
		if(const size_t LineIdx = LineIndexAt(LineStarts, Pos); Pos <= LineContentStarts[LineIdx]) {
			// 対象行のインデント登録
			SetLineIndent(Indents, LineStarts[LineIdx], IndentDepth);
		}
	};
	// 名前付子の深さを DepthOf で算出し，行頭インデントを登録した上で再帰
	const auto WalkNamed = [&](const TSNode Cur, auto &&DepthOf) -> void {
		ForEachNamedChild(
			Cur,
			[&](const TSNode Child) -> void {
				// 子節点のインデント深さ
				const int ChildDepth = DepthOf(Child);
				// 行頭節点のインデント登録
				SetIfLineStart(Src.Start(Child), ChildDepth);
				// 子節点のインデント収集
				CollectLineIndents(Src, Child, Language, LineStarts, LineContentStarts, ChildDepth, -1, Indents);
			}
		);
	};
	// 節点型名
	const std::string_view TypeView(ts_node_type(Node));
	// Ruby `heredoc_body` の本文行と終端行の深さ設定
	if(TypeView == "heredoc_body") {
		ForEachHeredocBodyLine(
			Node,
			LineStarts,
			static_cast<uint32_t>(Src.size()),
			[&](const uint32_t LineStart, const bool IsTerm) -> void {
				// 対象行のインデント登録
				SetLineIndent(Indents, LineStart, IsTerm ? Depth : Depth + 1);
			}
		);
		// 終了
		return;
	}
	// SkipButRecurseForLayout の構造的子へのインデント再帰
	if(NodeKind::Skip.Contains(TypeView) && !NodeKind::SkipButRecurseForLayout.Contains(TypeView)) return;
	// Ruby の引数・配列・ハッシュ自身の括弧だけによる深さ加算
	const auto OpensWithBracket = [&](const TSNode Target) -> bool {
		// 対象開始位置
		const uint32_t Start = Src.Start(Target);
		// 範囲外の節点は括弧で始まらない事の返戻
		if(Start >= Src.size()) return false;
		// 括弧で始まらない事の返戻
		if(const char Char = Src[Start]; Char != '(' && Char != '[' && Char != '{') return false;
		// 先頭子の有無と名前付状態による対象節点自身の括弧保持確認
		const TSNode FirstChild = ts_node_child(Target, 0);
		// 節点自身の括弧保持状態の返戻
		return ts_node_is_null(FirstChild) || !ts_node_is_named(FirstChild);
	};
	bool IsIndentBlock = NodeKind::IndentBlock.Contains(TypeView) && !(Language == Lang::Python && TypeView == "lambda");
	// 開き括弧を伴う argument_list / array / hash だけの深いコンテナ扱い
	if(IsIndentBlock && NodeKind::RubyBracketIndentBlock.Contains(TypeView) && !OpensWithBracket(Node)) IsIndentBlock = false;
	// 節見出の親への統一と本体だけの深さ加算
	if(NodeKind::SectionHeader.Contains(TypeView)) {
		// 行頭節点のインデント登録
		SetIfLineStart(Src.Start(Node), Depth);
		CollectTokenIndents(Src, Node, LineStarts, LineContentStarts, Depth, Indents);
		// Ruby の `elsif`/`else` 連鎖の同一深さへの配置
		WalkNamed(
			Node,
			[Depth](const TSNode Child) -> int {
				// 節見出に応じた子の深さの返戻
				return NodeKind::SectionHeader.Contains(Child) ? Depth : Depth + 1;
			}
		);
		// 終了
		return;
	}
	// 型名衝突を避ける親の欠如によるファイル根の確認
	if(NodeKind::Transparent.Contains(TypeView) || NodeKind::ThenDo.Contains(TypeView) || IsTreeRoot(Node)) {
		// Ruby の body_statement 直下の節見出の def / begin と同じ深さへの統一
		const bool IsRubyBodyStatement = Language == Lang::Ruby && TypeView == "body_statement";
		WalkNamed(
			Node,
			[IsRubyBodyStatement, Depth](const TSNode Child) -> int {
				// 浅い節見出の該当状態
				const bool IsShallowSection = IsRubyBodyStatement && NodeKind::SectionHeader.Contains(Child) && Depth;
				// Ruby の節見出に応じた深さの返戻
				return IsShallowSection ? Depth - 1 : Depth;
			}
		);
		// 終了
		return;
	}
	// Python の添字での値後方の `[` からの開始
	if(Language == Lang::Python && TypeView == "subscript") {
		// 基準インデント深さ
		int Base = Depth;
		bool IsOpened = false;
		ForEachChild(
			Node,
			[&](const TSNode Child) -> void {
				// 対象開始位置
				const uint32_t Start = Src.Start(Child);
				if(!ts_node_is_named(Child)) {
					// 行頭節点のインデント登録
					if(const std::string_view Token = Src.View(Child); IsOpened && Token == "]") SetIfLineStart(Start, Base);
					else if(!IsOpened && Token == "[") {
						IsOpened = true;
						if(
							const std::unordered_map<uint32_t, int>::const_iterator Iter = Indents.find(LineStartAt(LineStarts, Start));
							Iter != Indents.end()
						) Base = Iter->second;
					}
					// 字句子での基準深さと閉じ行の設定後の終了
					return;
				}
				// 子節点のインデント深さ
				const int ChildDepth = IsOpened ? Base + 1 : Depth;
				// 行頭節点のインデント登録
				SetIfLineStart(Start, ChildDepth);
				// 子節点のインデント収集
				CollectLineIndents(Src, Child, Language, LineStarts, LineContentStarts, ChildDepth, -1, Indents);
			}
		);
		// 終了
		return;
	}
	if(IsIndentBlock) {
		// 独自行頭を持たないインデントブロックでの登録済深さの使用
		const uint32_t NodeStart = Src.Start(Node), End = Src.End(Node);
		const size_t StartLineIdx = LineIndexAt(LineStarts, NodeStart);
		const std::unordered_map<uint32_t, int>::const_iterator BaseIter =
		NodeStart > LineContentStarts[StartLineIdx] ? Indents.find(LineStarts[StartLineIdx]) : Indents.end();
		// 実際に使うインデント深さ
		const int EffectiveDepth = BaseIter != Indents.end() ? BaseIter->second : Depth;
		// Python の block での字句深さ設定の省略
		if(Language != Lang::Python) CollectTokenIndents(Src, Node, LineStarts, LineContentStarts, EffectiveDepth, Indents);
		// 節見出を持つ親の該当状態
		const bool IsSectionParent = NodeKind::SectionParent.Contains(TypeView);
		WalkNamed(
			Node,
			[IsSectionParent, EffectiveDepth](const TSNode Child) -> int {
				// 節見出に応じた実効深さの返戻
				return IsSectionParent && NodeKind::SectionHeader.Contains(Child) ? EffectiveDepth : EffectiveDepth + 1;
			}
		);
		// 閉じ字句を持たない Python の block での内側インデントの保持
		if(End > NodeStart && End <= Src.size() && !(Language == Lang::Python && TypeView == "block")) {
			// 閉じ行の親深さへの復元と単行対・継続行末尾の深さ保持
			if(const bool IsEndKeyword = EndsWithEndKeyword(Src, NodeStart, End); IsCloseBracketChar(Src[End - 1]) || IsEndKeyword) {
				if(
					const size_t EndLineIdx = LineIndexAt(LineStarts, End - 1);
					StartLineIdx != EndLineIdx && (IsEndKeyword ? End - 3 : End - 1) <= LineContentStarts[EndLineIdx]
					// 対象行のインデント登録
				) SetLineIndent(Indents, LineStarts[EndLineIdx], EffectiveDepth);
			}
		}
		// 終了
		return;
	}
	// Ruby 継続行の１段加算と構文木深さによる多重加算の防止
	const bool IsRubyContinuation = Language == Lang::Ruby && NodeKind::RubyContinuation.Contains(TypeView);
	const uint32_t NodeStart = Src.Start(Node);
	const size_t NodeStartLine = IsRubyContinuation ? LineIndexAt(LineStarts, NodeStart) : 0;
	const auto LookupNodeStartIndent = [&Indents, &LineStarts, NodeStartLine, Depth]() -> int {
		const std::unordered_map<uint32_t, int>::const_iterator Iter = Indents.find(LineStarts[NodeStartLine]);
		// 登録済又は構文木由来深さの返戻
		return Iter != Indents.end() ? Iter->second : Depth;
	};
	// 外側で決めた継続行深さの子への引継
	int CurContIndent = OuterContIndent;
	if(IsRubyContinuation && CurContIndent < 0) {
		// 基準インデント深さ
		int Base = LookupNodeStartIndent();
		// 兄弟継続で汚染された開始行インデントの親スコープ深さへの復元
		if(Depth && Base >= Depth) Base = Depth - 1;
		CurContIndent = Base + 1;
	}
	// Python の演算子チェーンと属性参照の行頭字句の親と同じ深さへの統一
	if(Language == Lang::Python && (NodeKind::PySplittableExpr.Contains(TypeView) || TypeView == "attribute")) {
		// 添字アクセスの累積二次計算量を避けるカーソル走査による線形化
		ForEachChild(
			Node,
			[&](const TSNode Child) -> void {
				// 行頭節点のインデント登録
				if(!ts_node_is_named(Child)) SetIfLineStart(Src.Start(Child), Depth);
			}
		);
	}
	// 本体フィールド子のループ外探索による子毎の再探索の排除
	TSNode BodyFieldChild = {}, ConsequenceChild = {};
	if(Language != Lang::Python) {
		if(TypeView == "else_clause" && ts_node_named_child_count(Node)) ConsequenceChild = ts_node_named_child(Node, 0);
		else if(TypeView != "else_clause") {
			ConsequenceChild = TSSource::FieldChild(Node, "consequence");
			BodyFieldChild = TSSource::FieldChild(Node, "body");
		}
	}
	ForEachNamedChild(
		Node,
		[&](const TSNode Child) -> void {
			// 子の開始位置と単一字下げ対象の反映
			const uint32_t Start = Src.Start(Child);
			if(
				!ts_node_is_null(ConsequenceChild) && ts_node_eq(Child, ConsequenceChild) ||
				!ts_node_is_null(BodyFieldChild) && ts_node_eq(Child, BodyFieldChild)
				// 行頭節点のインデント登録
			) SetIfLineStart(Start, Depth);
			// Python の透過的な子と行頭括弧式の親深さ登録による誤タブ化の防止
			if(
				// 子節点の型名
				const std::string_view ChildType(ts_node_type(Child));
				Language == Lang::Python && (
					!NodeKind::IndentBlock.Contains(ChildType) && !NodeKind::IndentContainer.Contains(ChildType) ||
					ChildType != "block" && OpensWithBracket(Child)
				)
				// 行頭節点のインデント登録
			) SetIfLineStart(Start, Depth);
			// 言語固有の子インデント登録と再帰収集
			if(IsRubyContinuation) if(const size_t ChildLine = LineIndexAt(LineStarts, Start); ChildLine != NodeStartLine) {
				// 対象行のインデント登録
				SetLineIndent(Indents, LineStarts[ChildLine], CurContIndent);
			}
			CollectLineIndents(
				Src,
				Child,
				Language,
				LineStarts,
				LineContentStarts,
				Depth,
				IsRubyContinuation ? CurContIndent : -1,
				Indents
			);
		}
	);
	// 対象終了位置
	const uint32_t End = Src.End(Node);
	// 行頭節点のインデント登録
	if(EndsWithEndKeyword(Src, NodeStart, End)) SetIfLineStart(End - 3, Depth);
	// Ruby の閉じトークンは分割後も基底行のインデントへの統一
	if(IsRubyContinuation && End > NodeStart && End <= Src.size() && IsCloseBracketChar(Src[End - 1])) {
		// 行頭節点のインデント登録
		SetIfLineStart(End - 1, LookupNodeStartIndent());
	}
	// 終了
	return;
}

/**
 * 括弧に依るインデントの算出関数
 * @param Src ソースコード
 * @param Language 対象言語
 * @param LineStarts 行開始位置の配列
 * @param LineContentStarts 内容開始位置の配列
 * @param InCommentLines コメント内部行（此の行のインデントは出力時に無視）
 * @param SectionStartBytes セクション見出の開始バイト位置の集合
 * @param LineIndents インデント深さマップ（結果格納先）
 */
void LayoutPass::ComputeBracketIndent(
	const TSSource &Src,
	const Lang Language,
	const std::vector<uint32_t> &LineStarts,
	const std::vector<uint32_t> &LineContentStarts,
	const std::unordered_set<uint32_t> &InCommentLines,
	const std::unordered_set<uint32_t> &SectionStartBytes,
	std::vector<int> &LineIndents
) {
	// 起点行の集合を保つスタックで深さを計算
	const std::string &Source = Src;
	std::unordered_map<uint32_t, uint32_t> CloseOf, OpenOf;
	std::vector<std::pair<uint32_t, uint32_t>> OpaqueRanges;
	BracketPairs::CollectOpaqueRanges(Src, Language, OpaqueRanges);
	// 逐語範囲を除いて開閉括弧の対応の作成
	BracketPairs::Build(Source, Language, OpaqueRanges, CloseOf, OpenOf);
	// AddJsx の構文木全走査の JS / TS / HTML だけへの限定
	if(Language.IsJsTs() || Language == Lang::HTML) BracketPairs::AddJsx(Source, Src.GetRoot(), CloseOf, OpenOf);
	if(Language == Lang::PHP) BracketPairs::AddPhpColonBlocks(Source, Src.GetRoot(), CloseOf, OpenOf);
	struct Event {
		uint32_t Pos; // 対象位置
		uint8_t Type; // 1 は開き括弧，2 は閉じ括弧
		uint32_t Other; // 対応する括弧のバイト位置
	};
	// 括弧事象だけの整列と昇順の行頭事象との走査時統合
	std::vector<Event> Events;
	// 括弧開閉事象列の容量確保
	Events.reserve(CloseOf.size() << 1);
	// 括弧ペア毎に開始位置と終了位置の両方のイベントを生成し均衡の保持
	for(const std::pair<const uint32_t, uint32_t> &Entry : CloseOf) {
		if(LineIndexAt(LineStarts, Entry.first) == LineIndexAt(LineStarts, Entry.second)) continue;
		// 括弧開閉事象列への追加
		Events.push_back({ Entry.first, 1, Entry.second });
		// 括弧開閉事象列への追加
		Events.push_back({ Entry.second, 2, Entry.first });
	}
	// 行頭との統合走査用の括弧事象の位置順整列
	std::sort(
		Events.begin(),
		Events.end(),
		[](const Event &Lhs, const Event &Rhs) -> bool {
			// 位置の順の比較の返戻
			if(Lhs.Pos != Rhs.Pos) return Lhs.Pos < Rhs.Pos;
			// 同一位置の事象種別順の返戻
			return Lhs.Type < Rhs.Type;
		}
	);
	// インデント深さの開いている括弧の起点行数による決定
	std::vector<size_t> OriginStack, PoppedOrigins;
	size_t PoppedHead = 0, LineOrigin = std::numeric_limits<size_t>::max();
	std::vector<size_t> OriginCount(LineStarts.size(), 0);
	size_t DistinctSize = 0, CurrentLine = 0;
	std::vector<int> SegIndent(LineStarts.size(), 0);
	size_t NextLineIdx = 0;
	const auto BeginLinesUpTo = [&](const uint32_t Pos) -> void {
		// 対象位置迄の行開始の反映
		while(NextLineIdx < LineStarts.size() && LineStarts[NextLineIdx] <= Pos) {
			CurrentLine = NextLineIdx;
			// 閉じた括弧の起点行列の初期化
			PoppedOrigins.clear();
			PoppedHead = 0;
			LineOrigin = std::numeric_limits<size_t>::max();
			SegIndent[CurrentLine] = static_cast<int>(DistinctSize);
			++NextLineIdx;
		}
	};
	for(const Event &CurrentEvent : Events) {
		// 括弧イベント位置以前の行開始を先に処理する（同一バイト位置では行開始が先行）
		BeginLinesUpTo(CurrentEvent.Pos);
		if(CurrentEvent.Type == 1) {
			// 開き括弧：見出以外は同じ行で直前に閉じた起点を再利用し，無ければ同じ行で先に開いた起点の共有
			const size_t OriginLine = !OriginStack.empty() && SectionStartBytes.count(LineContentStarts[CurrentLine]) ?
			OriginStack.back() :
			PoppedHead < PoppedOrigins.size() ?
			PoppedOrigins[PoppedHead++] :
			LineOrigin != std::numeric_limits<size_t>::max() ? LineOrigin : CurrentLine;
			LineOrigin = OriginLine;
			// 開き括弧の起点行スタックへの追加
			OriginStack.push_back(OriginLine);
			if(++OriginCount[OriginLine] == 1) ++DistinctSize;
		} else if(!OriginStack.empty()) {
			// 閉じ括弧：スタックから取出 PoppedOrigins への移動
			const size_t OriginLine = OriginStack.back();
			// 閉じた括弧の起点行列への追加
			PoppedOrigins.push_back(OriginLine);
			// 開き括弧の起点行スタックの末尾要素除去
			OriginStack.pop_back();
			if(!--OriginCount[OriginLine]) --DistinctSize;
		}
	}
	// 最終括弧イベント以降に残る行開始の処理
	BeginLinesUpTo(std::numeric_limits<uint32_t>::max());
	// 行毎の基準値への閉じ括弧と節見出の補正適用
	for(size_t LineIdx = 0; LineIdx < LineStarts.size(); ++LineIdx) {
		if(InCommentLines.count(LineStarts[LineIdx])) continue;
		// 行の内容開始位置
		const uint32_t ContentByte = LineContentStarts[LineIdx];
		int Indent = SegIndent[LineIdx];
		if(
			const std::unordered_map<uint32_t, uint32_t>::const_iterator CloseIter = OpenOf.find(ContentByte);
			CloseIter != OpenOf.end() && LineIndexAt(LineStarts, CloseIter->second) != LineIdx
		) --Indent;
		if(ContentByte < Source.size() && SectionStartBytes.count(ContentByte) && Indent > 0) --Indent;
		if(Indent < 0) Indent = 0;
		// 行別の括弧インデント登録
		LineIndents[LineIdx] = Indent;
	}
	// 終了
	return;
}

/** ========== コメント配置 ========== */
/**
 * コメントを書き戻さない検査用の設定の判定関数
 * 環境変数 `SHAVEFMT_DROP_COMMENTS` が `1` の時はコメントを一切書き戻さない
 * 「コメントを除いた出力がコメントに左右されない」事を検査工程が機械的に確かめる為の口で，通常の整形では立たない
 * 他の値（`0` や空）で立てると，切った積りの利用者のコメントが消える為，`1` だけを受ける
 * @return コメントを書き戻さないなら true
 */
bool LayoutPass::DropsComments() {
	// コメント破棄設定
	static const bool Drops = []() -> bool {
		// 環境変数の読取と値の判定
		const char *const Value = std::getenv("SHAVEFMT_DROP_COMMENTS");
		const bool IsDropping = Value && std::string_view(Value) == "1";
		// 立って居る事を知らせないと，利用者は自分のコメントが消えた理由への到達不能
		if(IsDropping) std::fprintf(stderr, "Warning: SHAVEFMT_DROP_COMMENTS=1 is set; comments are dropped\n");
		// 環境変数値の返戻
		return IsDropping;
	}();
	// コメント破棄設定の返戻
	return Drops;
}

/**
 * 行末迄を本文とするコメントかの判定関数
 * 行コメントは一度置くと後続の挿入が種別に依らず其の本文へ吸われる為，並べて良いかの判定に用いる
 * @param Text コメント本文
 * @return `//` / `#` で始まるなら true
 */
bool LayoutPass::OpensLineComment(const std::string_view Text) {
	// 行末迄を本文とするコメントかの返戻
	return Text.starts_with("//") || Text.starts_with("#");
}

/**
 * 行内の行コメントを行末へ置く行かの判定関数
 * 閉じ括弧で始まる行のコメントを直前の括弧内へ移さない
 * @param Source ソースコード
 * @param ContentStart 行の最初の非空白の位置
 * @return 行の最初の非空白が閉じ括弧なら true
 */
bool LayoutPass::IsCloserLine(const std::string_view Source, const size_t ContentStart) {
	// 閉じ要素開始状態の返戻
	return ContentStart < Source.size() && IsCloseBracketChar(Source[ContentStart]);
}

/**
 * コメントの挿入候補の並び順の比較関数
 * コメントを挿入先・種別・出現順で安定整列する
 * @param Lhs 左の挿入候補
 * @param Rhs 右の挿入候補
 * @return Lhs を先に置くなら true
 */
bool LayoutPass::InsertsBefore(const CommentInsertion &Lhs, const CommentInsertion &Rhs) {
	// 並び順の返戻
	return Lhs.Dest != Rhs.Dest ?
	Lhs.Dest < Rhs.Dest :
	Lhs.IsLeading != Rhs.IsLeading ? Lhs.IsLeading < Rhs.IsLeading : Lhs.IsInline != Rhs.IsInline ?
	Lhs.IsInline > Rhs.IsInline :
	Lhs.IsOwnLine != Rhs.IsOwnLine ? Lhs.IsOwnLine < Rhs.IsOwnLine : Lhs.Seq < Rhs.Seq;
}

/**
 * コメント挿入候補の収集関数
 * @param Src 構文木と添付コメントの保持元
 * @param Language 対象言語（JSX 越境判定を JS / TS のみに限定する為）
 * @param LineStarts ソースの改行位置を表す行頭バイト位置の配列
 * @return 挿入候補のリスト
 */
std::vector<LayoutPass::CommentInsertion> LayoutPass::CollectCommentInsertions(
	const TSSource &Src,
	const Lang Language,
	const std::vector<uint32_t> &LineStarts
) {
	// 挿入先位置は元ソースのバイト位置（先行コメントは行頭，末尾コメントは行末空白を除いた位置）
	std::vector<CommentInsertion> Insertions;
	// コメントを書き戻さない場合と，紐付の無い場合の空の返戻
	if(DropsComments() || !Src.HasAttachments() || !Src.IsParsed()) return Insertions;
	const std::string &Source = Src;
	// 原文のバイト数
	const uint32_t SourceSize = static_cast<uint32_t>(Source.size());
	// １行あたり高々数件のコメントを想定して粗く予約する（追加時の再確保を回避する）
	Insertions.reserve(Source.size() / 40 + 16);
	// 閉じ又は自己終端タグの無いソースでの JSX 祖先探索省略
	const bool IsJsxPossible = (Language == Lang::JavaScript || Language.IsJsx) &&
	(Source.find("</") != std::string::npos || Source.find("/>") != std::string::npos);
	// JSX の可視本文へコメントが漏れない様に退避し，祖先状態と位置索引で三乗走査の回避
	struct JsxFrame {
		TSNode Braces; // 要素を越えずに届く最寄の祖先の `{…}`(jsx_expression)
		TSNode Outermost; // 最も外側の JSX 要素（自身を含む）
		uint32_t TagEnd = std::numeric_limits<uint32_t>::max(); // 最寄の祖先のタグの終端（`>` の直後）
		bool IsChild = false; // JSX 要素（断片 `<>` を含む）の直接の子か
	};
	// 子の JSX 文脈の親文脈と型からの取得
	const auto ChildFrame = [SourceSize](const JsxFrame &Outer, const TSNode Parent, const TSNode Child) -> JsxFrame {
		// 親節点の型名
		const std::string_view ParentType = ts_node_type(Parent);
		// 親文脈を継承した子文脈の構築
		JsxFrame Frame;
		Frame.IsChild = NodeKind::JsxContainer.Contains(ParentType);
		Frame.Outermost = !ts_node_is_null(Outer.Outermost) || !NodeKind::JsxElement.Contains(Child) ? Outer.Outermost : Child;
		Frame.TagEnd =
		NodeKind::JsxOrHtmlTagAny.Contains(ParentType) ? std::min<uint32_t>(ts_node_end_byte(Parent), SourceSize) : Outer.TagEnd;
		Frame.Braces = NodeKind::JsxElement.Contains(ParentType) ? TSNode{} : ParentType == "jsx_expression" ? Parent : Outer.Braces;
		// 子節点用 JSX 文脈の返戻
		return Frame;
	};
	// 其の枠の錨のコメントの退避先
	const auto FrameEscape = [&LineStarts](const JsxFrame &Frame) -> uint32_t {
		// 退避先の返戻
		return ts_node_is_null(Frame.Outermost) ?
		std::numeric_limits<uint32_t>::max() :
		LineStartAt(LineStarts, ts_node_start_byte(Frame.Outermost));
	};
	struct JsxSpan {
		TSNode Node; // `{…}`・タグ・要素の節点
		uint32_t Start; // 対象開始位置
		uint32_t End; // 対象終了位置
		uint32_t Parent; // 索引の中の最寄の祖先（無ければ NoSpan）
	};
	static constexpr uint32_t NoSpan = std::numeric_limits<uint32_t>::max();
	// JSX 範囲列
	std::vector<JsxSpan> Spans;
	// JSX の包含関係を１回の走査で索引化
	if(IsJsxPossible) {
		// 開いている JSX 範囲の索引列
		std::vector<uint32_t> Open;
		WalkChildrenCursor(
			Src.GetRoot(),
			[&](const TSNode Node) -> bool {
				if(NodeKind::JsxSpan.Contains(Node)) {
					const uint32_t Start = ts_node_start_byte(Node);
					// 開いている JSX 範囲索引列の末尾要素除去
					while(!Open.empty() && Spans[Open.back()].End <= Start) Open.pop_back();
					// JSX 範囲列への追加
					Spans.push_back({ Node, Start, ts_node_end_byte(Node), Open.empty() ? NoSpan : Open.back() });
					// 開いている JSX 範囲索引列への追加
					Open.push_back(static_cast<uint32_t>(Spans.size() - 1));
				}
				// 走査継続指示の返戻
				return true;
			}
		);
	}
	// From 以前に始まり To より後迄続く最も内側の索引の節点（先行順で最後に始まる節点から祖先へ遡る）
	const auto InnermostSpan = [&Spans](const uint32_t From, const uint32_t To) -> uint32_t {
		// 開始位置以前の最後の範囲探索
		const std::vector<JsxSpan>::const_iterator Next = std::upper_bound(
			Spans.begin(),
			Spans.end(),
			From,
			[](const uint32_t Pos, const JsxSpan &Span) -> bool {
				// 範囲開始位置との比較結果の返戻
				return Pos < Span.Start;
			}
		);
		// 対象範囲の索引
		uint32_t Idx = Next == Spans.begin() ? NoSpan : static_cast<uint32_t>(Next - Spans.begin() - 1);
		while(Idx != NoSpan && Spans[Idx].End <= To) Idx = Spans[Idx].Parent;
		// 最内側 JSX 範囲索引の返戻
		return Idx;
	};
	// 索引の節点を含む最も外側の JSX 要素が始まる行の先頭（JSX の外なら UINT32_MAX）
	const auto SpanEscape = [&](uint32_t Idx) -> uint32_t {
		// 最外側 JSX 要素の索引
		uint32_t Outermost = NoSpan;
		// 祖先列から最外の要素索引を選択
		for(; Idx != NoSpan; Idx = Spans[Idx].Parent) if(NodeKind::JsxElement.Contains(Spans[Idx].Node)) Outermost = Idx;
		// JSX 外への退避位置の返戻
		return Outermost == NoSpan ? std::numeric_limits<uint32_t>::max() : LineStartAt(LineStarts, Spans[Outermost].Start);
	};
	// 位置を跨ぐ最内節点による JSX 本文の確認
	const auto InJsxText = [&](const uint32_t Pos) -> uint32_t {
		// 対象範囲の索引
		const uint32_t Idx = Pos ? InnermostSpan(Pos - 1, Pos) : NoSpan;
		// JSX 地の文範囲索引の返戻
		return Idx != NoSpan && NodeKind::JsxContainer.Contains(Spans[Idx].Node) ? Idx : NoSpan;
	};
	// 囲みコメント後の行コメントを行末へ回した行を控え，後の錨でも原文順の保持
	std::unordered_set<size_t> LineEndLines;
	const auto ProcessNode = [&](const TSNode Node, const TSNode Parent, const JsxFrame &Frame) -> void {
		const std::vector<CommentAttach> &Leads = Src.GetLeading(Node), &Trails = Src.GetTrailing(Node);
		// 空の JSX 式に紐付いたコメントの包装内への復元
		if(IsJsxPossible && std::string_view(ts_node_type(Node)) == "jsx_expression" && !ts_node_named_child_count(Node)) {
			// コメントの配置位置
			const uint32_t Dest = Src.Start(Node) + 1;
			// コメント挿入候補列への追加
			for(const CommentAttach &Comment : Leads) Insertions.push_back({ Dest, Comment.Seq, Comment.Text, false, true });
			// コメント挿入候補列への追加
			for(const CommentAttach &Comment : Trails) Insertions.push_back({ Dest, Comment.Seq, Comment.Text, false, true });
			// 汎用の JSX 越境退避を適用せず終了
			return;
		}
		// 先行コメントの錨と同じ行のコードを基準に置場の決定
		if(!Leads.empty()) {
			uint32_t EffStart = ts_node_start_byte(Node);
			if(std::string_view(ts_node_type(Node)) != "uninterpreted") {
				while(EffStart < SourceSize && (Source[EffStart] == ' ' || Source[EffStart] == '\t' || Source[EffStart] == '\n')) ++EffStart;
			}
			// コメントの配置位置
			const uint32_t Dest = LineStartAt(LineStarts, EffStart);
			uint32_t Probe = Dest;
			while(Probe < EffStart && (Source[Probe] == ' ' || Source[Probe] == '\t')) ++Probe;
			// 錨の行頭配置状態
			const bool IsAnchorAtLineHead = Probe >= EffStart;
			const uint32_t LeadDest = IsAnchorAtLineHead ? Dest : EffStart;
			const bool IsCrossesToJsxText =
			IsJsxPossible && (InJsxText(Dest) != NoSpan || !IsAnchorAtLineHead && InJsxText(EffStart) != NoSpan);
			// 越境する先行コメントの JSX 外への退避
			const uint32_t Escape = IsCrossesToJsxText ? FrameEscape(Frame) : std::numeric_limits<uint32_t>::max();
			const bool IsEscaped = IsCrossesToJsxText && Escape != std::numeric_limits<uint32_t>::max();
			uint32_t LineTail = std::numeric_limits<uint32_t>::max();
			const auto AnchorLineTail = [&]() -> uint32_t {
				if(LineTail == std::numeric_limits<uint32_t>::max()) {
					LineTail = TextEdit::SkipSpLeft(Source, static_cast<uint32_t>(std::min(Source.find('\n', EffStart), Source.size())));
				}
				// 錨行末位置の返戻
				return LineTail;
			};
			// 錨の直前に続く名前無字句列の始点の取得
			uint32_t TokenRun = std::numeric_limits<uint32_t>::max();
			const auto TokenRunStart = [&]() -> uint32_t {
				// 遅延計算した開始位置の再利用
				if(TokenRun == std::numeric_limits<uint32_t>::max()) {
					TokenRun = EffStart;
					// 錨の兄弟字句の親範囲内からの取得
					const TSNode SearchRoot = ts_node_is_null(Parent) ? Src.GetRoot() : Parent;
					for(
						// 対象終了位置
						uint32_t End = TextEdit::SkipSpLeft(Source, TokenRun);
						End && Source[End - 1] != '\n' && !IsCloseBracketChar(Source[End - 1]);
					) {
						// 対象字句節点
						const TSNode Token = ts_node_descendant_for_byte_range(SearchRoot, End - 1, End);
						if(ts_node_is_named(Token)) break;
						TokenRun = ts_node_start_byte(Token);
						End = TextEdit::SkipSpLeft(Source, TokenRun);
					}
				}
				// 末尾字句列の開始位置の返戻
				return TokenRun;
			};
			// 行頭の錨に先行する行末コメントは直前のコード末尾への配置
			uint32_t HeadAt = std::numeric_limits<uint32_t>::max();
			const size_t AnchorLineIdx = LineIndexAt(LineStarts, EffStart);
			bool IsHeadEscaped = false, IsAfterInlineBlock = false;
			bool IsAtLineEnd = !IsAnchorAtLineHead && LineEndLines.contains(AnchorLineIdx);
			// 越境するが退避先も得れないコメントは捨てずファイル末尾へ逃がす（著者の記述をソースから消してはならない）
			for(const CommentAttach &Comment : Leads) {
				// 次の行に作用する指令は，錨が行の途中へ移れば他の先行コメントと同じく錨の行の前への配置
				if(Comment.IsHead && (IsAnchorAtLineHead || !DocSig::IsNextLineDirective(Comment.Text))) {
					if(HeadAt == std::numeric_limits<uint32_t>::max()) {
						HeadAt = IsAnchorAtLineHead ? TextEdit::SkipCharsLeftBounded(Source, Dest, 0, " \t\r\n") : AnchorLineTail();
						IsHeadEscaped = IsJsxPossible && InJsxText(HeadAt) != NoSpan;
						if(IsHeadEscaped) {
							const uint32_t HeadEscape = FrameEscape(Frame);
							HeadAt = HeadEscape != std::numeric_limits<uint32_t>::max() ? HeadEscape : SourceSize;
						}
					}
					// 次行へ作用する指令は錨の直前の独立行への配置
					if(
						const bool IsDetached = IsAnchorAtLineHead && !IsHeadEscaped && DocSig::IsNextLineDirective(Comment.Text) && (
							LineIndexAt(LineStarts, HeadAt) + 1 < LineIndexAt(LineStarts, Dest) || std::any_of(
								Leads.begin(),
								Leads.end(),
								[](const CommentAttach &Other) -> bool {
									// 後置コメント該当状態の返戻
									return !Other.IsHead;
								}
							)
						); IsDetached // コメント挿入候補列への追加
					) Insertions.push_back({ Dest, std::numeric_limits<uint32_t>::max(), Comment.Text, true, false });
					// コメント挿入候補列への追加
					else Insertions.push_back({ HeadAt, Comment.Seq, Comment.Text, IsHeadEscaped, false });
					continue;
				}
				// JSX の外へ出せるコメントは安全な退避先への配置
				if(IsEscaped) {
					// コメント挿入候補列への追加
					Insertions.push_back({ Escape, Comment.Seq, Comment.Text, true, false });
					continue;
				}
				// 退避先の無い越境コメントは可視本文への混入防止と末尾送出
				if(IsCrossesToJsxText) {
					// コメント挿入候補列への追加
					Insertions.push_back({ SourceSize, Comment.Seq, Comment.Text, true, false });
					continue;
				}
				// 行コメントの該当状態
				const bool IsLineComment = OpensLineComment(Comment.Text);
				IsAtLineEnd = IsAtLineEnd || IsAfterInlineBlock && IsLineComment && !DocSig::IsNextLineDirective(Comment.Text);
				// 行内の囲みコメントとの順序を保って行末への連結
				if(IsAtLineEnd) {
					// コメント挿入候補列への追加
					Insertions.push_back({ AnchorLineTail(), Comment.Seq, Comment.Text, false, false });
					// 行末コメント配置済行集合への登録
					LineEndLines.insert(AnchorLineIdx);
				} else {
					IsAfterInlineBlock = IsAfterInlineBlock || !IsAnchorAtLineHead && !IsLineComment;
					// コメント挿入候補列への追加
					Insertions.push_back(
						{
							!IsAnchorAtLineHead && Comment.IsBeforeToken ? TokenRunStart() : LeadDest,
							Comment.Seq,
							Comment.Text,
							IsAnchorAtLineHead,
							!IsAnchorAtLineHead,
							false,
							0,
							Comment.IsGluedAfter,
							Comment.IsGluedBefore,
							Comment.Joined
						}
					);
				}
				Insertions.back().IsVerbatim = Comment.IsVerbatim;
				Insertions.back().Origin = EffStart;
			}
		}
		if(!Trails.empty()) {
			// JSX 要素の直接の子である錨後方の本文扱い
			const bool IsChild = IsJsxPossible && Frame.IsChild;
			const bool IsInline = !ts_node_is_null(Parent) && std::string_view(ts_node_type(Parent)) == "named_imports";
			const size_t NlPos = Source.find('\n', std::min<uint32_t>(Src.End(Node), SourceSize));
			const uint32_t InsertAt =
			IsInline ? Src.End(Node) : TextEdit::SkipSpLeft(Source, NlPos == std::string::npos ? SourceSize : static_cast<uint32_t>(NlPos));
			// 挿入先が包含する jsx_*_element の終端 (`>`) 以後だと jsx_text 化
			const uint32_t TagEnd = IsJsxPossible ? Frame.TagEnd : std::numeric_limits<uint32_t>::max();
			const TSNode Braces = IsJsxPossible ? Frame.Braces : TSNode{};
			if(!ts_node_is_null(Braces) && InsertAt >= Src.End(Braces) && !IsChild) {
				// 閉じ波括弧と退避位置
				const uint32_t Close = Src.End(Braces) - 1, Escape = FrameEscape(Frame);
				for(const CommentAttach &Comment : Trails) {
					// コメント挿入候補列への追加
					Insertions.push_back(
						!Comment.Text.starts_with("//") ?
						CommentInsertion{ Close, Comment.Seq, Comment.Text, false, true } :
						CommentInsertion{ Escape != std::numeric_limits<uint32_t>::max() ? Escape : SourceSize, Comment.Seq, Comment.Text, true, false }
					);
				}
			} else if(const uint32_t TextHost = IsJsxPossible ? InJsxText(InsertAt) : NoSpan; TextHost != NoSpan && !IsChild) {
				// 行末が JSX の要素の本文に当たるコメントは，其の要素を含む行の前へ退避
				for(const CommentAttach &Comment : Trails) {
					// コメント挿入候補列への追加
					Insertions.push_back({ SpanEscape(TextHost), Comment.Seq, Comment.Text, true, false });
				}
			} else if(InsertAt < TagEnd && !IsChild) {
				// 塊の末尾コメントは原則として後続行への配置
				const size_t AnchorLine = LineIndexAt(LineStarts, ts_node_start_byte(Node));
				const uint32_t AfterAnchor = TextEdit::SkipSpRight(Source, std::min<uint32_t>(Src.End(Node), SourceSize));
				const bool IsFollowed = AfterAnchor < SourceSize && Source[AfterAnchor] != '\n';
				const auto IsBlockLast = [&]() -> bool {
					// 配置対象を所有する節点
					TSNode Owner = Node;
					// 同じ末尾を共有する所有節点への遡及
					for(
						// 対象の親節点
						TSNode Up = ts_node_parent(Owner);
						!ts_node_is_null(Up) && ts_node_end_byte(Up) == ts_node_end_byte(Node);
						Up = ts_node_parent(Up)
					) {
						if(
							// 親節点の型名
							const std::string_view UpType = ts_node_type(Up);
							NodeKind::StatementHost.Contains(UpType) || NodeKind::ThenDo.Contains(UpType) || NodeKind::SectionHeader.Contains(UpType) ||
							NodeKind::ClauseBody.Contains(UpType) || NodeKind::Control.Contains(UpType)
						) break;
						Owner = Up;
					}
					// 配置対象の行頭位置
					uint32_t Head = ts_node_start_byte(Owner);
					while(Head < SourceSize && (Source[Head] == ' ' || Source[Head] == '\t' || Source[Head] == '\n')) ++Head;
					const TSNode Next = ts_node_next_named_sibling(Owner);
					// 錨の中身が行頭から始まり後に兄弟が続かない（Ruby の節（`rescue` 等）の前の最後の文を含む）かの返戻
					return TextEdit::SkipSpRight(Source, LineStartAt(LineStarts, Head)) == Head &&
					(ts_node_is_null(Next) || Language == Lang::Ruby && NodeKind::SectionHeader.Contains(Next));
				};
				// 対象の最終行頭位置
				const uint32_t LastLine = Src.End(Node) ? LineStartAt(LineStarts, Src.End(Node) - 1) : 0;
				const bool IsCloserEnd =
				IsFollowed && LastLine > LineStarts[AnchorLine] && IsCloserLine(Source, TextEdit::SkipSpRight(Source, LastLine));
				// 独立した行へ置いたコメントの後のコメントも独立した行へ置く（行末へ置くと前のコメントより前に出て順が入替る）
				bool IsAfterOwnLine = false;
				for(const CommentAttach &Comment : Trails) {
					if(Comment.IsOwnLine && IsCloserEnd) {
						// コメント挿入候補列への追加
						Insertions.push_back({ LastLine, Comment.Seq, Comment.Text, true, false, false, 0 });
						Insertions.back().IsVerbatim = Comment.IsVerbatim;
						continue;
					}
					// 行末へ置くコメントは句点で行を分けない本文の使用
					const std::string &LineEndText = Comment.Joined.empty() ? Comment.Text : Comment.Joined;
					const bool IsOwnLinePlaced = Comment.IsOwnLine && !IsFollowed &&
					(IsAfterOwnLine || Comment.ForcesOwnLine || LineEndText.find('\n') != std::string::npos || IsBlockLast());
					IsAfterOwnLine = IsOwnLinePlaced;
					// 閉じ字句へ作用する指令の錨前への配置
					if(Comment.IsCloserDirective && !IsOwnLinePlaced && !IsInline) {
						if(const size_t TailLine = LineIndexAt(LineStarts, InsertAt); TailLine + 1 < LineStarts.size()) {
							if(
								// 次行の内容開始位置
								const uint32_t NextContent = TextEdit::SkipSpRight(Source, LineStarts[TailLine + 1]);
								NextContent < SourceSize && Source[NextContent] != '\n' && !IsCloserLine(Source, NextContent)
							) {
								// 後置位置の行頭
								const uint32_t TailHead = LineStarts[TailLine];
								// コメント挿入候補列への追加
								Insertions.push_back({ TailHead, Comment.Seq, Comment.Text, true, false, false, 0 });
								Insertions.back().IsVerbatim = Comment.IsVerbatim;
								continue;
							}
						}
					}
					// コメント挿入候補列への追加
					Insertions.push_back(
						{
							InsertAt,
							Comment.Seq,
							IsOwnLinePlaced ? Comment.Text : LineEndText,
							false,
							IsInline && !Comment.IsOwnLine,
							IsOwnLinePlaced,
							AnchorLine
						}
					);
					Insertions.back().IsVerbatim = Comment.IsVerbatim;
					Insertions.back().Origin = Src.End(Node);
				}
			} else if(const uint32_t Escape = FrameEscape(Frame); Escape != std::numeric_limits<uint32_t>::max()) {
				// 置場も退避先も得れないコメントは捨てずファイル末尾へ逃がす（著者の記述をソースから消してはならない）
				for(const CommentAttach &Comment : Trails) Insertions.push_back({ Escape, Comment.Seq, Comment.Text, true, false });
				// コメント挿入候補列への追加
			} else for(const CommentAttach &Comment : Trails) Insertions.push_back({ SourceSize, Comment.Seq, Comment.Text, true, false });
		}
	};
	// 子孫とは別に根自身の添付も候補化
	ProcessNode(Src.GetRoot(), TSNode{}, JsxFrame{});
	// 部分木範囲に紐付の開始バイトが無ければ配下の走査を丸ごと枝刈り
	HeldCursor Cursor(Src.GetRoot());
	std::vector<std::pair<TSNode, JsxFrame>> Frames { { Src.GetRoot(), JsxFrame{} } };
	for(bool IsWalking = ts_tree_cursor_goto_first_child(&Cursor); IsWalking;) {
		// 現在の節点
		const TSNode Node = ts_tree_cursor_current_node(&Cursor);
		const JsxFrame Frame = IsJsxPossible ? ChildFrame(Frames.back().second, Frames.back().first, Node) : JsxFrame{};
		if(Src.HasAttachedCommentInRange(ts_node_start_byte(Node), ts_node_end_byte(Node) + 1)) {
			// 節点の添付コメント処理
			ProcessNode(Node, Frames.back().first, Frame);
			if(ts_tree_cursor_goto_first_child(&Cursor)) {
				// 走査階層別 JSX 文脈への追加
				Frames.push_back({ Node, Frame });
				continue;
			}
		}
		while(IsWalking && !ts_tree_cursor_goto_next_sibling(&Cursor)) {
			// 走査階層別 JSX 文脈の末尾要素除去
			Frames.pop_back();
			IsWalking = !Frames.empty() && ts_tree_cursor_goto_parent(&Cursor);
		}
	}
	// 置場の調整前に位置・種別・出現順の統一
	std::stable_sort(Insertions.begin(), Insertions.end(), InsertsBefore);
	const auto LineStartsWithCloser = [&Source](const uint32_t LineStart) -> bool {
		// 対象位置
		const size_t Pos = TextEdit::SkipSpRight(Source, LineStart);
		// 末尾に達した行は閉じの字句で始まらない事の返戻
		if(Pos >= Source.size()) return false;
		// Ruby はスコープを `end` で閉じる為，閉じ括弧と同じ位置付けで扱う，行の最初の非空白が閉じ字句かの返戻
		return IsCloseBracketChar(Source[Pos]) ||
		!Source.compare(Pos, 3, "end") && (Pos + 3 >= Source.size() || !IsIdentifierChar(Source[Pos + 3]));
	};
	std::vector<uint32_t> Relocations;
	std::vector<size_t> Climbed;
	const auto RelocationHead = [&](size_t Line) -> uint32_t {
		// 閉じ行別移動先表の初期化
		if(Relocations.empty()) Relocations.assign(LineStarts.size(), std::numeric_limits<uint32_t>::max());
		// 移動先探索で遡った行列の初期化
		Climbed.clear();
		while(Relocations[Line] == std::numeric_limits<uint32_t>::max() && LineStartsWithCloser(LineStarts[Line])) {
			// 移動先探索で遡った行列への追加
			Climbed.push_back(Line);
			// 対象位置
			const uint32_t Pos = static_cast<uint32_t>(TextEdit::SkipSpRight(Source, LineStarts[Line]));
			const TSNode Owner = ts_node_parent(ts_node_descendant_for_byte_range(Src.GetRoot(), Pos, Pos + 1));
			if(ts_node_is_null(Owner)) break;
			// 対応する開き要素の行番号
			const size_t OpenLine = LineIndexAt(LineStarts, ts_node_start_byte(Owner));
			if(OpenLine >= Line) break;
			Line = OpenLine;
		}
		uint32_t &Head = Relocations[Line];
		if(Head == std::numeric_limits<uint32_t>::max()) {
			Head = LineStarts[Line];
			// JSX の子で始まる行の前へ置くと本文として描画される為，最も外側の JSX 要素の行頭へ退避
			if(IsJsxPossible) {
				// 対象位置
				const uint32_t Pos = static_cast<uint32_t>(TextEdit::SkipSpRight(Source, LineStarts[Line]));
				const uint32_t Innermost = InnermostSpan(Pos, Pos);
				uint32_t Host = Innermost;
				while(Host != NoSpan && !NodeKind::JsxContainer.Contains(Spans[Host].Node)) Host = Spans[Host].Parent;
				if(Host != NoSpan) {
					if(
						const TSNode Child = ts_node_first_child_for_byte(Spans[Host].Node, Pos);
						!ts_node_is_null(Child) && ts_node_start_byte(Child) == Pos
					) if(const uint32_t Escape = SpanEscape(Innermost); Escape != std::numeric_limits<uint32_t>::max()) Head = Escape;
				}
			}
		}
		// 遡った各行にも確定した退避先の記録
		for(const size_t Passed : Climbed) Relocations[Passed] = Head;
		// 閉じ行コメントの移動先の返戻
		return Head;
	};
	// コメントの置場を変えたか（変えると挿入候補の並び順が変わる）
	bool IsReordered = false;
	// 行内の行コメントは出現順で先行化し，} else 等の節は行前
	for(CommentInsertion &Ins : Insertions) {
		if(!Ins.IsInline || !OpensLineComment(Ins.Text)) continue;
		// 配置対象の行頭位置
		uint32_t Head = LineStartAt(LineStarts, Ins.Dest);
		// 節点に属さない行頭字句での親節点の欠如
		if(Language == Lang::Ruby) if(const uint32_t Content = TextEdit::SkipSpRight(Source, Head); Content < SourceSize) {
			if(
				// 配置対象を所有する節点
				const TSNode Owner = ts_node_parent(ts_node_descendant_for_byte_range(Src.GetRoot(), Content, Content + 1));
				!ts_node_is_null(Owner) && NodeKind::SectionHeader.Contains(Owner)
			) {
				// 行末へ置く為，句点で行を分けない本文を使う（分けた行を行末へ置くと，後の行が次の行の頭へ別れる）
				if(!Ins.Joined.empty()) Ins.Text = Ins.Joined;
				Ins.IsInline = false;
				IsReordered = true;
				continue;
			}
		}
		if(const uint32_t Content = TextEdit::SkipSpRight(Source, Head); IsCloserLine(Source, Content)) {
			// 対象行の終端位置
			const size_t LineEnd = std::min(Source.find('\n', Content), Source.size());
			const bool OpensBlock = Source[TextEdit::SkipSpLeft(Source, static_cast<uint32_t>(LineEnd)) - 1] == '{';
			if(Source[Content] != '}') {
				if(!OpensBlock) continue;
				Head = RelocationHead(LineIndexAt(LineStarts, Head));
			} else {
				// 閉じ波括弧の節点
				const TSNode Brace = ts_node_descendant_for_byte_range(Src.GetRoot(), Content, Content + 1);
				TSNode Clause = ts_node_next_named_sibling(Brace);
				if(const TSNode Owner = ts_node_parent(Brace); ts_node_is_null(Clause) && !ts_node_is_null(Owner)) {
					Clause = ts_node_next_named_sibling(Owner);
				}
				const uint32_t AfterBrace = TextEdit::SkipSpRight(Source, Content + 1);
				const bool FollowsClause = !ts_node_is_null(Clause) && Src.Start(Clause) == AfterBrace;
				// 後の節の見出の中のコメントは，行の前へ回すと手前の塊の末尾のコメントに為る為，行末への配置
				if(
					Ins.Dest > AfterBrace && (
						FollowsClause ||
						NodeKind::ElseKeyword.Contains(Src.View(ts_node_descendant_for_byte_range(Src.GetRoot(), AfterBrace, AfterBrace + 1)))
					)
				) continue;
				if(!FollowsClause || Ins.Dest != Src.Start(Clause)) {
					if(!OpensBlock) continue;
					if(!FollowsClause) Head = RelocationHead(LineIndexAt(LineStarts, Head));
				}
			}
		}
		Ins.Dest = Head;
		Ins.IsLeading = true;
		Ins.IsInline = false;
		IsReordered = true;
	}
	// 行コメント有無への行内コメントと行末コメントの合算
	size_t PrevTailLine = std::numeric_limits<size_t>::max();
	bool HasLineCommentOnLine = false, HasMovedDownLine = false;
	uint32_t MovedDownHead = std::numeric_limits<uint32_t>::max();
	for(CommentInsertion &Ins : Insertions) {
		// 独立行コメントの下方移動からの除外
		if(Ins.IsLeading || Ins.IsOwnLine) continue;
		if(const size_t LineIdx = LineIndexAt(LineStarts, Ins.Dest); LineIdx != PrevTailLine) {
			PrevTailLine = LineIdx;
			HasLineCommentOnLine = HasMovedDownLine = false;
		}
		// 行内コメントと安全な囲みコメントは其の儘置き，行コメントは順に行末への連結
		if(
			// 行コメントの該当状態
			const bool IsLineComment = OpensLineComment(Ins.Text);
			!HasMovedDownLine && (Ins.IsInline || !HasLineCommentOnLine || IsLineComment && Ins.Text.find('\n') == std::string_view::npos)
		) {
			HasLineCommentOnLine = HasLineCommentOnLine || IsLineComment;
			continue;
		}
		// 行コメント後の囲み・複数行コメントは次の独立行へ順に置き，閉じ字句が続く時だけ錨の深さで其の行後への配置
		if(!HasMovedDownLine) {
			MovedDownHead = std::numeric_limits<uint32_t>::max();
			for(size_t Next = PrevTailLine + 1; Next < LineStarts.size(); ++Next) {
				// 行の内容開始位置
				const uint32_t Content = TextEdit::SkipSpRight(Source, LineStarts[Next]);
				if(Content >= SourceSize) break;
				if(Source[Content] == '\n') continue;
				if(
					// 対象字句節点
					const TSNode Token = ts_node_descendant_for_byte_range(Src.GetRoot(), Content, Content + 1);
					(ts_node_is_named(Token) || !NodeKind::CloserToken.Contains(ts_node_type(Token))) &&
					(!IsJsxPossible || InJsxText(Content) == NoSpan)
				) MovedDownHead = LineStarts[Next];
				break;
			}
		}
		// 後続行へ移せないコメントの元行後方への独立配置
		if(MovedDownHead == std::numeric_limits<uint32_t>::max()) {
			Ins.IsOwnLine = true;
			Ins.IndentLine = PrevTailLine;
		} else {
			// 次行構文の先行コメント前への配置
			Ins.Dest = MovedDownHead;
			Ins.IsLeading = true;
			Ins.Seq = 0;
		}
		HasMovedDownLine = IsReordered = true;
	}
	// 次行指令の錨行前への配置と囲み指令の位置保持
	for(CommentInsertion &Ins : Insertions) {
		if(!(Ins.IsLeading || Ins.IsInline && OpensLineComment(Ins.Text)) || !DocSig::IsNextLineDirective(Ins.Text)) continue;
		// 配置対象の行頭と内容開始位置
		const uint32_t Head = LineStartAt(LineStarts, Ins.Dest), HeadContent = TextEdit::SkipSpRight(Source, Head);
		if(Ins.IsLeading && HeadContent >= Ins.Dest) continue;
		Ins.Dest = Head;
		Ins.IsLeading = true;
		Ins.IsInline = false;
		IsReordered = true;
	}
	if(!Insertions.empty()) {
		// 先行要素の末尾コメントの複数行リテラル本文への混入防止
		struct StringRange {
			uint32_t Start; // 対象開始位置
			uint32_t End; // 対象終了位置
			uint32_t SafeHead; // 文字列の外に在る先行行頭
		};
		// 複数行文字列の保護範囲列
		std::vector<StringRange> Strings;
		WalkChildrenCursor(
			Src.GetRoot(),
			[&](const TSNode Node) -> bool {
				const std::string_view Type = ts_node_type(Node);
				// PHP の地の文は出力其の物で，其処へ落ちるコメントは DropCommentsInInlineHtml が扱う為，範囲に含めない事の返戻
				if(NodeKind::PhpInlineHtml.Contains(Type)) return false;
				// 文字列・逐語の葉以外の構文子を走査する事の返戻
				if(!NodeKind::StringLikeInnerPreserve.Contains(Type) && !NodeKind::Leaf.Contains(Type)) return true;
				if(const uint32_t Start = Src.Start(Node), End = Src.End(Node); std::memchr(Src.data() + Start, '\n', End - Start)) {
					// 配置対象の行頭位置
					uint32_t Head = LineStartAt(LineStarts, Start);
					// 連続する複数行文字列での本文外行頭の引継
					if(!Strings.empty() && Strings.back().Start < Head && Head < Strings.back().End) Head = Strings.back().SafeHead;
					// 複数行文字列保護範囲列への追加
					Strings.push_back({ Start, End, Head });
				}
				// 葉配下の走査停止指示の返戻
				return false;
			}
		);
		for(CommentInsertion &Ins : Insertions) {
			const uint32_t Dest = Ins.IsInline && OpensLineComment(Ins.Text) ? LineStartAt(LineStarts, Ins.Dest) : Ins.Dest;
			const std::vector<StringRange>::const_iterator Iter = std::upper_bound(
				Strings.begin(),
				Strings.end(),
				Dest,
				[](const uint32_t Pos, const StringRange &Range) -> bool {
					// 文字列範囲開始位置との比較結果の返戻
					return Pos < Range.Start;
				}
			);
			if(Iter == Strings.begin()) continue;
			const StringRange &Range = *(Iter - 1);
			if(Range.Start < Dest && Dest < Range.End) {
				Ins.Dest = Range.SafeHead;
				Ins.IsLeading = true;
				Ins.IsInline = false;
				IsReordered = true;
			}
		}
	}
	// Ruby の埋込ドキュメントの独立行配置による構文保持
	if(Language == Lang::Ruby) for(CommentInsertion &Ins : Insertions) {
		if(Ins.IsLeading || !Ins.Text.starts_with("=begin")) continue;
		if(Ins.IsInline) Ins.Dest = LineStartAt(LineStarts, Ins.Dest);
		else {
			// 次の探索位置
			const std::vector<uint32_t>::const_iterator Next = std::upper_bound(LineStarts.begin(), LineStarts.end(), Ins.Dest);
			Ins.Dest = Next == LineStarts.end() ? SourceSize : *Next;
		}
		Ins.IsLeading = true;
		Ins.IsInline = false;
		IsReordered = true;
	}
	// 移動で変わった挿入候補位置順の出力前確定
	if(IsReordered) std::stable_sort(Insertions.begin(), Insertions.end(), InsertsBefore);
	// コメント挿入候補列の返戻
	return Insertions;
}

/**
 * PHP の地の文へ差し込まれるコメントの除去関数
 * 地の文（最初の開始タグより前・`?>` から次の開始タグ迄）は出力其の物で，コメントを置くと其の本文が出力へ混入する
 * 差込先が PHP の地の文に当たるコメントを除く
 * コメントの消失は整形原則が許容する
 * @param Src ソースコード
 * @param LineStarts 行開始位置の配列
 * @param Insertions コメントの挿入候補（地の文へ差し込まれる物を除く）
 */
void LayoutPass::DropCommentsInInlineHtml(
	const TSSource &Src,
	const std::vector<uint32_t> &LineStarts,
	std::vector<CommentInsertion> &Insertions
) {
	// 地の文の範囲と，閉じタグ `?>` と地の文の後の開始タグの位置
	std::vector<std::pair<uint32_t, uint32_t>> Html;
	std::vector<uint32_t> CloseTags, OpenTags;
	WalkChildrenCursor(
		Src.GetRoot(),
		[&](const TSNode Node) -> bool {
			if(
				const std::string_view Type = ts_node_type(Node);
				Type == "text" && ts_node_eq(Node, ts_node_named_child(Src.GetRoot(), 0))
			) {
				// 先頭 HTML 範囲と開始タグ位置の登録
				OpenTags.push_back(Src.End(Node));
				Html.emplace_back(0, Src.End(Node));
			} else if(Type == "text_interpolation") {
				// `?>` の直後から次の開始タグの直前迄（開始タグが無ければ構文木の外の末尾の改行を含めファイル末尾迄）
				const TSNode Close = FirstNamedChildOfType(Node, "php_end_tag"), Open = FirstNamedChildOfType(Node, "php_tag");
				// PHP タグ位置の登録
				if(!ts_node_is_null(Close)) CloseTags.push_back(Src.Start(Close));
				if(!ts_node_is_null(Open)) OpenTags.push_back(Src.Start(Open));
				// PHP 内 HTML 範囲列への追加
				Html.emplace_back(
					ts_node_is_null(Close) ? Src.Start(Node) : Src.End(Close),
					ts_node_is_null(Open) ? static_cast<uint32_t>(Src.size()) : Src.Start(Open)
				);
				// 地の文以外の子孫走査を続ける事の返戻
			} else return true;
			// PHP 範囲配下の走査停止指示の返戻
			return false;
		}
	);
	// 地の文が無い場合の終了
	if(Html.empty()) return;
	// 差込位置を二分探索出来る様に地の文の範囲の統一
	std::sort(Html.begin(), Html.end());
	const std::string &Source = Src;
	// HTML 範囲内の確認関数
	const auto InHtml = [&Html](const uint32_t Pos) -> bool {
		const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Iter = std::upper_bound(
			Html.begin(),
			Html.end(),
			Pos,
			[](const uint32_t Value, const std::pair<uint32_t, uint32_t> &Range) -> bool {
				// HTML 範囲開始位置との比較結果の返戻
				return Value < Range.first;
			}
		);
		// HTML 範囲内の該当状態の返戻
		return Iter != Html.begin() && Pos <= (Iter - 1)->second;
	};
	// PHP 終了タグ位置列
	std::sort(CloseTags.begin(), CloseTags.end());
	std::sort(OpenTags.begin(), OpenTags.end());
	const auto LandsInHtml =
	[&](const CommentInsertion &Ins, const size_t LineIdx, const uint32_t LineStart, const uint32_t LineEnd) -> bool {
		// コメント種別に応じた配置先の判定
		if(Ins.IsLeading) {
			// 配置対象の行頭位置
			const uint32_t Head = LineStart < Ins.Dest && LineIdx + 1 < LineStarts.size() ? LineStarts[LineIdx + 1] : LineStart;
			// 前置コメントの HTML 配置状態の返戻
			return !Head || InHtml(Head);
		}
		// 行内の囲みコメントの置く先（錨）が地の文かの返戻
		if(Ins.IsInline && !OpensLineComment(Ins.Text)) return InHtml(Ins.Dest);
		// 行内の行コメントの置く先が地の文かの返戻
		if(Ins.IsInline) return IsCloserLine(Source, TextEdit::SkipSpRight(Source, LineStart)) ? InHtml(LineEnd) : !LineStart;
		// 後置コメントの HTML 配置状態の返戻
		return InHtml(LineEnd);
	};
	// PHP の地の文に当たるコメントは同じ行の閉じタグ直前への移動
	bool IsMoved = false;
	for(CommentInsertion &Ins : Insertions) {
		const size_t LineIdx = LineIndexAt(LineStarts, Ins.Dest);
		const uint32_t LineStart = LineStarts[LineIdx];
		const uint32_t LineEnd = LineIdx + 1 < LineStarts.size() ? LineStarts[LineIdx + 1] - 1 : static_cast<uint32_t>(Source.size());
		const bool IsHtmlLine = InHtml(LineStart) && !std::binary_search(OpenTags.begin(), OpenTags.end(), LineStart);
		if(!IsHtmlLine && !LandsInHtml(Ins, LineIdx, LineStart, LineEnd)) continue;
		// 錨の後の最初の閉じタグの直前への移動
		const bool IsTail = !Ins.IsLeading && !Ins.IsInline;
		const std::vector<uint32_t>::const_iterator Close =
		// PHP 終了タグ位置列
		std::lower_bound(CloseTags.begin(), CloseTags.end(), Ins.Origin ? Ins.Origin : Ins.Dest);
		const bool IsCloseMissing = Close == CloseTags.end() || *Close > LineEnd;
		if(!IsTail && !IsHtmlLine && InHtml(Ins.Dest) && !std::binary_search(OpenTags.begin(), OpenTags.end(), Ins.Dest)) Ins.Text = {};
		// 次行へ続く PHP 区画での行末コメント配置
		else if(IsCloseMissing && IsHtmlLine && !InHtml(LineEnd)) {
			Ins.Dest = TextEdit::SkipSpLeft(Source, LineEnd);
			Ins.IsLeading = Ins.IsInline = false;
			IsMoved = true;
		} else if(IsCloseMissing) Ins.Text = {};
		else {
			Ins.Dest = *Close;
			Ins.IsLeading = false;
			Ins.IsInline = true;
			IsMoved = true;
			// `?>` で終わる行コメントは本文に閉じタグの前の空白を含む為，其れを除いて区切りの空白の重複防止
			while(Ins.Text.ends_with(' ') || Ins.Text.ends_with('\t')) Ins.Text.remove_suffix(1);
		}
	}
	// 安全な差込先を持たないコメントの候補の除外
	std::erase_if(
		Insertions,
		[](const CommentInsertion &Ins) -> bool {
			// 空のコメント挿入候補の除去条件の返戻
			return Ins.Text.empty();
		}
	);
	// PHP 側へ移した後の挿入順の統一
	if(IsMoved) std::stable_sort(Insertions.begin(), Insertions.end(), InsertsBefore);
	// 終了
	return;
}

/**
 * 文字数のタブ数への切上関数
 * @param Chars 空白の文字数
 * @param IndentChars インデント幅（非零）
 * @return 切上後のタブ数
 */
size_t LayoutPass::CeilTabs(const size_t Chars, const uint32_t IndentChars) {
	// 必要タブ数の返戻
	return Chars / IndentChars + (Chars % IndentChars ? 1 : 0);
}

/** ========== 出力確定 ========== */
/**
 * レイアウト確定の適用関数
 * 計算量：行数 L とインデントの段数 D に対し O(L * D)（行毎にインデントを書き出す）
 * @param Src 整形対象のソース（結果で上書きされる）
 * @param Language 対象言語
 */
void LayoutPass::FinalizeLayout(TSSource &Src, const Lang Language) {
	// インデント深さ計算・タブ変換・空行制御・行末空白除去の一括適用
	constexpr uint32_t IndentChars = 4;
	const std::string &Source = Src;
	// インデント判定とコメント復元に共通の行位置列を用意
	std::vector<uint32_t> LineStarts, LineContentStarts;
	BuildLineInfo(Src, LineStarts, LineContentStarts);
	std::unordered_map<uint32_t, int> Indents;
	// 行別インデント表の初期化
	Indents.reserve(LineStarts.size());
	std::vector<int> LineIndents(LineStarts.size(), std::numeric_limits<int>::max());
	bool UsesLineIndents = false;
	std::unordered_set<uint32_t> InCommentLines, InHeredocContentLines, SectionStartBytes, RootBlankBefore;
	std::unordered_set<uint32_t> MultilineLeafOpenerLines;
	// 逐語行と構造境界の集合容量の確保
	InCommentLines.reserve((LineStarts.size() >> 2) + 1);
	InHeredocContentLines.reserve((LineStarts.size() >> 5) + 1);
	SectionStartBytes.reserve((LineStarts.size() >> 3) + 1);
	RootBlankBefore.reserve((LineStarts.size() >> 5) + 1);
	if(Src.IsParsed()) {
		// 言語のインデント方式に必要な位置情報の収集
		const bool NeedsSectionStarts = Language != Lang::Ruby && Language != Lang::Python;
		// 逐語行と節見出位置の収集
		CollectLayoutNodeInfo(
			Src,
			LineStarts,
			InCommentLines,
			InHeredocContentLines,
			NeedsSectionStarts ? &SectionStartBytes : nullptr,
			&MultilineLeafOpenerLines
		);
		// 構文木の根
		const TSNode Root = Src.GetRoot();
		const auto IsMultiByAst = [&](const TSNode Node) -> bool {
			// 節点範囲と最初の改行の取得
			const uint32_t NStart = ts_node_start_byte(Node), NEnd = ts_node_end_byte(Node);
			// 範囲の壊れた節点は対象外の返戻
			if(NEnd <= NStart || NEnd > Src.size()) return false;
			const void *const NlPtr = std::memchr(Source.data() + NStart, '\n', NEnd - NStart);
			// 改行を含まない節点は対象外の返戻
			if(!NlPtr) return false;
			// 範囲内の先頭改行位置
			const uint32_t NlPos = static_cast<uint32_t>(static_cast<const char *>(NlPtr) - Source.data());
			for(uint32_t Idx = NlPos + 1; Idx < NEnd; ++Idx) {
				// 改行の後に内容が有る事の返戻
				if(const char Char = Source[Idx]; Char != ' ' && Char != '\t' && Char != '\n' && Char != '\r') return true;
			}
			// 単一行又は空範囲の返戻
			return false;
		};
		// クラス様の本体のメンバはトップレベルと同様に空行挿入の対象とする為，深さを維持して再帰
		std::unordered_set<const void *> VisitedMemberContainers;
		const auto WalkContainer = [&](const TSNode Container, const size_t Depth, const bool IsRootCall, const auto &Self) -> void {
			// 通常走査と局所型の補完走査で同じメンバ列を処理済の場合の返戻
			if(!Depth && !VisitedMemberContainers.insert(Container.id).second) return;
			// 深いネストは走脈領域を使い切る前に，新しい走脈で続きの調整
			const RecursionGuard Guard;
			if(Guard.IsOverflow) {
				Parallel::RunOnFreshStack(
					[&]() -> void {
						Self(Container, Depth, IsRootCall, Self);
					}
				);
				// 新しい走脈で調整を終えた事の返戻
				return;
			}
			// 最上位走査の該当状態
			const bool IsTopLevel = !Depth;
			bool HasPrevRoot = false, IsPrevMulti = false, IsPendingBlock = false, IsAfterInlineOpen = false;
			std::string_view PrevRootKind;
			size_t PrevRootEndLine = 0;
			uint32_t BraceDepth = 0;
			HeldCursor Cursor(Container);
			size_t FirstAttachStartLine = std::numeric_limits<size_t>::max();
			if(ts_tree_cursor_goto_first_child(&Cursor)) {
				do {
					const TSNode Child = ts_tree_cursor_current_node(&Cursor);
					// 節点の元の型名
					const std::string_view RawKind(ts_node_type(Child));
					// Java の enum の宣言部は列挙子に続く同じ成員列としての取扱
					if(RawKind == "enum_body_declarations" && ts_tree_cursor_goto_first_child(&Cursor)) continue;
					if(!ts_node_is_named(Child)) {
						// 裸の波括弧内に在る解析断片はトップレベルの空行登録からの除外
						if(IsRootCall) {
							if(RawKind == "{") ++BraceDepth;
							else if(RawKind == "}" && BraceDepth && !--BraceDepth) IsPendingBlock = true;
						}
						continue;
					}
					// AttachPrefix と次の実体項目の空行無での連鎖
					if(NodeKind::AttachPrefix.Contains(RawKind)) {
						if(FirstAttachStartLine == std::numeric_limits<size_t>::max()) {
							FirstAttachStartLine = LineIndexAt(LineStarts, ts_node_start_byte(Child));
						}
						continue;
					}
					// 直前のプロパティに属する Kotlin アクセサの空行無の継続扱い
					if(Language == Lang::Kotlin && HasPrevRoot && NodeKind::KotlinAccessor.Contains(RawKind)) {
						PrevRootEndLine = LineIndexAt(LineStarts, ts_node_end_byte(Child) - 1);
						IsPrevMulti = true;
						continue;
					}
					// 裸の波括弧内に在る解析断片は状態の保持と読飛し
					if(
						IsRootCall && BraceDepth || RawKind == "expression_statement" && Src.View(Child) == ";" ||
						NodeKind::Comment.Contains(RawKind) || RawKind == "ERROR"
					) continue;
					// 取込系を import，Python 直下の生の実行ノードを stmt へ正規化し，同種の文間の誤った空行の防止
					const std::string_view Kind =
					NodeKind::ImportLike.Contains(RawKind) ? "import" : Language == Lang::Python && !NodeKind::PyDefLike.Contains(RawKind) ?
					"stmt" :
					RawKind == "preproc_function_def" ? "preproc_def" : RawKind;
					// 子節点の範囲
					const uint32_t Start = ts_node_start_byte(Child), End = ts_node_end_byte(Child);
					const size_t StartLine = LineIndexAt(LineStarts, Start), EndLine = End > Start ? LineIndexAt(LineStarts, End - 1) : StartLine;
					const bool IsSameKind = PrevRootKind == Kind, IsMulti = IsMultiByAst(Child), IsMultiLineTrigger = IsPrevMulti || IsMulti;
					const bool IsInlineHtmlEdge =
					Language == Lang::PHP && (NodeKind::PhpInlineHtml.Contains(Kind) || Kind == "php_tag" || IsAfterInlineOpen);
					const bool ShouldRegister = HasPrevRoot && IsTopLevel && Language != Lang::HTML && StartLine > PrevRootEndLine &&
					(IsMultiLineTrigger || !IsSameKind || IsPendingBlock) && !IsInlineHtmlEdge;
					// 先行する AttachPrefix の連鎖が有れば，空行前は連鎖の先頭行に登録
					const size_t BlankAtLine = FirstAttachStartLine != std::numeric_limits<size_t>::max() ? FirstAttachStartLine : StartLine;
					// 根直下要素前の空行集合への登録
					if(ShouldRegister) RootBlankBefore.insert(LineStarts[BlankAtLine]);
					FirstAttachStartLine = std::numeric_limits<size_t>::max();
					IsPendingBlock = false;
					HasPrevRoot = true;
					IsPrevMulti = IsMulti;
					// 値を出力する `<?=` はファイルの先頭でも地の文の中の開始タグと同様の取扱
					IsAfterInlineOpen = Language == Lang::PHP && NodeKind::PhpOpenTagEnd.Contains(Kind) &&
					(Kind != "php_tag" || PrevRootKind == "text" || Src.View(Child) == "<?=");
					PrevRootKind = Kind;
					PrevRootEndLine = EndLine;
					const bool IsChildPreprocRoot = Language.IsCFamily() && NodeKind::PreprocConditional.Contains(Kind);
					if(Language.IsCFamily() && NodeKind::PreprocBlock.Contains(Kind)) {
						// `#else` の次行（最初の本体の文）に空行前を登録する（全子走査をせず最初の名前付子を直接取得）
						if(IsTopLevel && Kind == "preproc_else") {
							if(const TSNode FirstBody = ts_node_named_child(Child, 0); !ts_node_is_null(FirstBody)) {
								// 根直下要素前の空行集合への登録
								RootBlankBefore.insert(LineStarts[LineIndexAt(LineStarts, ts_node_start_byte(FirstBody))]);
							}
						}
						Self(Child, Depth, false, Self);
						// 根直下要素前の空行集合への登録
						if(IsTopLevel && IsChildPreprocRoot && End) RootBlankBefore.insert(LineStarts[LineIndexAt(LineStarts, End - 1)]);
					}
					// 子（孫の親）がクラス様なら孫を深さ維持で再帰し，其れ以外は深さ + 1
					const size_t NextDepth = NodeKind::ClassLike.Contains(RawKind) ? Depth : Depth + 1;
					ForEachNamedChild(
						Child,
						[&](const TSNode GrandChild) -> void {
							const std::string_view GcTypeView(ts_node_type(GrandChild));
							// 通常の型本体と Python の class の block の走査
							if(
								const bool IsRecursive = NodeKind::ClassBodyContainer.Contains(GcTypeView) ||
								Language == Lang::Python && GcTypeView == "block" && RawKind == "class_definition";
								IsRecursive
							) Self(GrandChild, NextDepth, false, Self);
							else if(NodeKind::ClassLike.Contains(GcTypeView)) {
								// 宣言が class_specifier 等を内包する C++ の形も再帰
								ForEachNamedChild(
									GrandChild,
									[&](const TSNode GreatGc) -> void {
										if(NodeKind::ClassBodyContainer.Contains(GreatGc)) Self(GreatGc, Depth, false, Self);
									}
								);
							}
						}
					);
				} while(ts_tree_cursor_goto_next_sibling(&Cursor));
			}
		};
		// 最上位と型の成員間へ空行の境界の登録
		WalkContainer(Root, 0, true, WalkContainer);
		// 関数本体に局所定義された型も独立したメンバ列を持つ為，コメントの有無に依らず同じ空行規則の適用
		WalkChildrenCursor(
			Root,
			[&](const TSNode Node) -> bool {
				const std::string_view Type = ts_node_type(Node);
				// 匿名型は ClassLike の宣言節点を持たないが，明確な型本体には同じメンバ規則の適用
				if(NodeKind::ClassBodyContainer.Contains(Type) && !NodeKind::AmbiguousBodyContainer.Contains(Type)) {
					// コンテナ内要素の走査
					WalkContainer(Node, 0, false, WalkContainer);
				}
				if(NodeKind::ClassLike.Contains(Type)) {
					ForEachNamedChild(
						Node,
						[&](const TSNode Body) -> void {
							if(
								// 本体子の型名
								const std::string_view BodyType = ts_node_type(Body);
								NodeKind::ClassBodyContainer.Contains(BodyType) || Language == Lang::Python && BodyType == "block"
								// コンテナ内要素の走査
							) WalkContainer(Body, 0, false, WalkContainer);
						}
					);
				}
				// 対象コンテナ発見状態の返戻
				return true;
			}
		);
		// 構文木の深さを使う言語と括弧の深さを使う言語の分離
		if(Language == Lang::Ruby || Language == Lang::Python) {
			// 子節点のインデント収集
			CollectLineIndents(Src, Src.GetRoot(), Language, LineStarts, LineContentStarts, 0, -1, Indents);
		} else {
			// 括弧構造由来インデントの算出
			ComputeBracketIndent(Src, Language, LineStarts, LineContentStarts, InCommentLines, SectionStartBytes, LineIndents);
			UsesLineIndents = true;
		}
	}
	// 全体に共通する余分な深さを出力時の基準からの除外
	int BaseIndent = std::numeric_limits<int>::max();
	if(UsesLineIndents) {
		for(const int Depth : LineIndents) if(Depth != std::numeric_limits<int>::max()) BaseIndent = std::min(BaseIndent, Depth);
	}
	if(BaseIndent == std::numeric_limits<int>::max()) BaseIndent = 0;
	// コメント復元と統合：構文木からの挿入候補取得と行ループ中の消費
	std::vector<CommentInsertion> CommentInsertions = CollectCommentInsertions(Src, Language, LineStarts);
	// PHP のコメントを出力本文へ混ぜない位置へ調整
	if(Language == Lang::PHP) DropCommentsInInlineHtml(Src, LineStarts, CommentInsertions);
	// 出力構築状態と容量の初期化
	size_t InsIdx = 0;
	std::string Result;
	Result.reserve(Source.size() + (CommentInsertions.size() << 4));
	const size_t OutputLimit = std::max<size_t>(FileIO::MaxInputBytes, Source.size() << 3);
	std::vector<size_t> ResultVerbatimLines;
	ResultVerbatimLines.reserve(InCommentLines.size() + InHeredocContentLines.size());
	bool SawContent = false;
	char PrevNonBlankLast = '\0';
	std::vector<size_t> LineTabs;
	if(
		std::any_of(
			CommentInsertions.begin(),
			CommentInsertions.end(),
			[](const CommentInsertion &Ins) -> bool {
				// 独立行コメント該当状態の返戻
				return Ins.IsOwnLine;
			}
		)
		// 行別インデント数の初期化
	) LineTabs.assign(LineStarts.size(), 0);
	// コメント本文の出力コメントは分離時に外側のインデントだけを除いてある為，本文の相対インデントを保持
	const auto AppendCommentBody = [&Result, &ResultVerbatimLines, Language](
		const std::string_view TextView,
		const size_t Tabs,
		const bool IsIndentFirst,
		const bool IsVerbatim
	) -> void {
		// 空のコメント本文の出力省略
		if(TextView.empty()) return;
		// Ruby 埋込文書のコード行判定準備
		const bool IsRubyEmbeddedDoc = Language == Lang::Ruby && TextView.starts_with("=begin");
		const std::vector<unsigned char> RubyCode =
		IsRubyEmbeddedDoc ? Postprocess::CommentCodeMask(TextView, false) : std::vector<unsigned char>();
		for(size_t Cursor = 0, Emitted = 0;; ++Emitted) {
			// 改行区切毎のコメント本文出力
			const size_t NewlinePos = TextView.find('\n', Cursor);
			std::string_view Line =
			TextView.substr(Cursor, NewlinePos == std::string_view::npos ? std::string_view::npos : NewlinePos - Cursor);
			if(Emitted) Result += '\n';
			if((Emitted || IsIndentFirst) && (!IsRubyEmbeddedDoc || RubyCode[Cursor])) ResultVerbatimLines.push_back(Result.size());
			if(!IsRubyEmbeddedDoc && (Emitted || IsIndentFirst)) {
				// `*` で始まる継続行の文書化コメント位置への整合
				const bool IsStarLine =
				Emitted && !IsVerbatim && Line.find_first_not_of(" \t") != std::string_view::npos && Line[Line.find_first_not_of(" \t")] == '*';
				if(IsStarLine) Line.remove_prefix(Line.find_first_not_of(" \t"));
				// 整形結果への追記
				Result.append(Tabs, '\t');
				if(IsStarLine) Result += ' ';
			}
			Result.append(Line);
			// 最終行の出力後の終了
			if(NewlinePos == std::string_view::npos) break;
			// 次の本文行への走査位置更新
			Cursor = NewlinePos + 1;
		}
		// 終了
		return;
	};
	// 行位置順での本文と対応コメントの出力構築
	for(size_t LineIdx = 0; LineIdx < LineStarts.size(); ++LineIdx) {
		const size_t LineStart = LineStarts[LineIdx];
		const size_t FullLineEnd = LineIdx + 1 < LineStarts.size() ? LineStarts[LineIdx + 1] - 1 : Source.size();
		size_t LineEnd = FullLineEnd;
		// Python の複数行文字列の開始行は末尾空白を逐語保持
		if(!MultilineLeafOpenerLines.count(static_cast<uint32_t>(LineStart))) {
			while(LineEnd > LineStart && (Source[LineEnd - 1] == ' ' || Source[LineEnd - 1] == '\t' || Source[LineEnd - 1] == '\n')) {
				--LineEnd;
			}
		}
		// 原文の先頭空白数
		const size_t LeadingSpace = LineContentStarts[LineIdx] - LineStarts[LineIdx];
		const bool IsInComment = InCommentLines.count(static_cast<uint32_t>(LineStart));
		const bool IsInHeredoc = !IsInComment && InHeredocContentLines.count(static_cast<uint32_t>(LineStart));
		// 元ソースの空行は読み飛ばし，空行は構文木の境界に登録した物だけの配置
		if(IsInComment || IsInHeredoc || LineStart + LeadingSpace < LineEnd) {
			// 内容開始位置
			const size_t ContentStart = LineStart + LeadingSpace;
			const std::string_view Trimmed(Source.data() + ContentStart, LineEnd - ContentStart);
			const bool IsContinuationBlank = !Trimmed.empty() && Trimmed.front() == '=' && IsCloseBracketChar(PrevNonBlankLast);
			const bool IsPythonClauseBlank = Language == Lang::Python && PrevNonBlankLast == ':';
			// 整形結果への改行追加
			if(!Result.empty()) Result += '\n';
			// 構文木に深さが未登録の行だけタブ数を計算
			const auto FallbackTabs = [&]() -> size_t {
				// 原文インデントの文字数
				size_t TabChars = 0;
				while(TabChars < LeadingSpace && Source[LineStart + TabChars] == '\t') ++TabChars;
				// 原文空白を換算したタブ数の返戻
				return TabChars + CeilTabs(LeadingSpace - TabChars, IndentChars);
			};
			// 未登録インデント要素の最大値による未確定表現
			const std::unordered_map<uint32_t, int>::const_iterator IndentIter =
			UsesLineIndents ? Indents.end() : Indents.find(static_cast<uint32_t>(LineStart));
			const int RawIndent =
			UsesLineIndents ? LineIndents[LineIdx] : IndentIter != Indents.end() ? IndentIter->second : std::numeric_limits<int>::max();
			const size_t Tabs =
			RawIndent != std::numeric_limits<int>::max() ? static_cast<size_t>(std::max(RawIndent - BaseIndent, 0)) : FallbackTabs();
			if(!LineTabs.empty()) LineTabs[LineIdx] = Tabs;
			// 空行配置は元ソースを完全に無視し，構文木上の意味的境界のみで決定
			if(
				const bool IsRegisteredBlank = SawContent && RootBlankBefore.count(static_cast<uint32_t>(LineStart));
				IsRegisteredBlank && !IsContinuationBlank && !IsPythonClauseBlank
				// 整形結果への改行追加
			) Result += '\n';
			// 此の行の先行コメントを錨と同じ深さで本文より先に出力
			bool IsPrevBuildConstraint = false;
			while(InsIdx < CommentInsertions.size() && CommentInsertions[InsIdx].IsLeading && CommentInsertions[InsIdx].Dest <= LineStart) {
				const std::string_view TextView = CommentInsertions[InsIdx].Text;
				const bool IsCurBuildConstraint =
				Language == Lang::Go && (TextView.starts_with("//go:build") || TextView.starts_with("// +build"));
				// 整形結果への改行追加
				if(IsPrevBuildConstraint && !IsCurBuildConstraint) Result += '\n';
				IsPrevBuildConstraint = IsCurBuildConstraint;
				// 本文が空の挿入は何も書かない（改行だけが残ると空行に為り，次回の整形で消えて冪等性が崩れる）
				if(!TextView.empty()) {
					// コメント本文の出力
					AppendCommentBody(TextView, Tabs, true, CommentInsertions[InsIdx].IsVerbatim);
					// 整形結果への改行追加
					Result += '\n';
				}
				++InsIdx;
			}
			// 最後の先行コメントがビルド制約なら後続のコード行とも空行で分離する（Go 仕様）
			if(IsPrevBuildConstraint) Result += '\n';
			// 行コメント (`// ` `#`) は行末迄が本文の為，一度置くと後続の挿入は種別に依らず其の本文へ吸収
			bool IsLineCommentOpen = false;
			std::vector<std::string_view> CloserLineTails;
			if(IsInComment || IsInHeredoc) {
				// コメント内部行とヒアドキュメント本文行の完全な逐語保持
				ResultVerbatimLines.push_back(Result.size());
				// 逐語の行へ置く行内のコメントは，行の前へ回さずに其の位置への挿入
				uint32_t CopyFrom = static_cast<uint32_t>(LineStart);
				while(
					InsIdx < CommentInsertions.size() && !CommentInsertions[InsIdx].IsLeading && CommentInsertions[InsIdx].IsInline &&
					CommentInsertions[InsIdx].Dest <= FullLineEnd
				) {
					const uint32_t Dest = std::max<uint32_t>(CommentInsertions[InsIdx].Dest, CopyFrom);
					// 整形結果への追記
					Result.append(Source, CopyFrom, Dest - CopyFrom);
					// 整形結果への区切空白追加
					if(!Result.empty() && Result.back() != ' ' && Result.back() != '\t' && Result.back() != '\n') Result += ' ';
					// コメント本文の出力
					AppendCommentBody(CommentInsertions[InsIdx].Text, Tabs, false, CommentInsertions[InsIdx].IsVerbatim);
					// 整形結果への区切空白追加
					if(Dest < FullLineEnd && Source[Dest] != ' ' && Source[Dest] != '\t' && Source[Dest] != '\n') Result += ' ';
					CopyFrom = Dest;
					++InsIdx;
				}
				// 整形結果への追記
				Result.append(Source, CopyFrom, FullLineEnd - CopyFrom);
			} else {
				// 行頭位置の記録
				size_t LineHead = Result.size();
				if(LineHead + Tabs > OutputLimit) throw FormatLimitExceeded("output exceeds size limit");
				// 整形結果への追記
				Result.append(Tabs, '\t');
				// 原文の次の複製開始位置
				uint32_t CopyFrom = static_cast<uint32_t>(ContentStart);
				while(
					InsIdx < CommentInsertions.size() && !CommentInsertions[InsIdx].IsLeading && CommentInsertions[InsIdx].IsInline &&
					CommentInsertions[InsIdx].Dest <= LineEnd
				) {
					// コメントの配置位置
					const uint32_t Dest = std::max<uint32_t>(CommentInsertions[InsIdx].Dest, CopyFrom);
					const std::string_view InlineText = CommentInsertions[InsIdx].Text;
					// PHP の閉じタグの直前へ移した行コメントは `?>` で終わる為，其の位置への配置
					if(
						const bool IsLineCommentHere = OpensLineComment(InlineText) && (Language != Lang::PHP || Source.compare(Dest, 2, "?>"));
						IsLineCommentHere && IsCloserLine(Source, ContentStart)
						// 閉じ行後置コメント列への追加
					) CloserLineTails.push_back(CommentInsertions[InsIdx].Joined.empty() ? InlineText : CommentInsertions[InsIdx].Joined);
					else if(IsLineCommentHere) {
						// 本文が複数行に及ぶ形でも各行にインデントを付ける為，末尾へ一旦組み立ててから行頭への移動
						const size_t Assembled = Result.size(), PreviousVerbatim = ResultVerbatimLines.size();
						// コメント本文の出力
						AppendCommentBody(InlineText, Tabs, true, CommentInsertions[InsIdx].IsVerbatim);
						// 整形結果への改行追加
						Result += '\n';
						// 行頭へ移したコメント本文
						const std::string Hoisted = Result.substr(Assembled);
						// 整形結果の出力巻戻し
						Result.resize(Assembled);
						// 行頭へのコメント挿入
						Result.insert(LineHead, Hoisted);
						// 回送したコメント行と挿入で動いた既存行の位置を更新し，単調照会の順序の保持
						const size_t InsertAt = static_cast<size_t>(
							std::lower_bound(ResultVerbatimLines.begin(), ResultVerbatimLines.begin() + PreviousVerbatim, LineHead) -
							ResultVerbatimLines.begin()
						);
						for(size_t Idx = InsertAt; Idx < PreviousVerbatim; ++Idx) ResultVerbatimLines[Idx] += Hoisted.size();
						for(size_t Idx = PreviousVerbatim; Idx < ResultVerbatimLines.size(); ++Idx) {
							ResultVerbatimLines[Idx] = LineHead + ResultVerbatimLines[Idx] - Assembled;
						}
						std::rotate(ResultVerbatimLines.begin() + InsertAt, ResultVerbatimLines.begin() + PreviousVerbatim, ResultVerbatimLines.end());
						LineHead += Hoisted.size();
					} else {
						// 整形結果への追記
						if(Dest > CopyFrom) Result.append(Source, CopyFrom, Dest - CopyFrom);
						// 開き括弧直後での区切空白の省略
						if(CommentInsertions[InsIdx].IsGluedBefore) while(!Result.empty() && Result.back() == ' ') Result.pop_back();
						// 整形結果への区切空白追加
						else if(!Result.empty() && Result.back() != ' ' && Result.back() != '\n' && !IsOpenBracketChar(Result.back())) Result += ' ';
						// コメント本文の出力
						AppendCommentBody(InlineText, Tabs, false, CommentInsertions[InsIdx].IsVerbatim);
						// 後続のコードがコメントへ密着しない様に区切りの空白の配置
						if(
							Dest < LineEnd && Source[Dest] != ' ' && Source[Dest] != '\t' && Source[Dest] != ',' && Source[Dest] != ';' &&
							!IsCloseBracketChar(Source[Dest]) && !CommentInsertions[InsIdx].IsGluedAfter
							// 整形結果への区切空白追加
						) Result += ' ';
						CopyFrom = Dest;
					}
					++InsIdx;
				}
				// 整形結果への追記
				if(LineEnd > CopyFrom) Result.append(Source, CopyFrom, LineEnd - CopyFrom);
			}
			// 行頭が閉じ括弧の行へ回せなかった行コメントを，行末のコメントと同じ扱いで末尾への配置
			for(const std::string_view TailText : CloserLineTails) {
				// 独立行コメントの該当状態
				const bool IsSeparate = IsLineCommentOpen && (!OpensLineComment(TailText) || TailText.find('\n') != std::string_view::npos);
				// 整形結果への改行追加
				if(IsSeparate) Result += '\n';
				// 整形結果への区切空白追加
				else if(!Result.empty() && Result.back() != ' ' && Result.back() != '\n') Result += ' ';
				// コメント本文の出力
				AppendCommentBody(TailText, Tabs, IsSeparate, false);
				IsLineCommentOpen = true;
			}
			// 此の行末範囲 (LineStart~LineEnd) に対応する末尾挿入を行末に追記
			for(
				;
				InsIdx < CommentInsertions.size() && !CommentInsertions[InsIdx].IsLeading && !CommentInsertions[InsIdx].IsInline &&
				CommentInsertions[InsIdx].Dest <= LineEnd;
				++InsIdx
			) {
				// 行末へ配置するコメント本文
				const std::string_view TailText = CommentInsertions[InsIdx].Text;
				// 塊末尾コメントの次行への錨行インデントでの配置
				if(CommentInsertions[InsIdx].IsOwnLine) {
					// 整形結果への改行追加
					Result += '\n';
					// コメント本文の出力
					AppendCommentBody(TailText, LineTabs[CommentInsertions[InsIdx].IndentLine], true, CommentInsertions[InsIdx].IsVerbatim);
					IsLineCommentOpen = OpensLineComment(TailText);
					continue;
				}
				// 独立行コメントの該当状態
				const bool IsSeparate = IsLineCommentOpen && (!OpensLineComment(TailText) || TailText.find('\n') != std::string_view::npos);
				// 整形結果への改行追加
				if(IsSeparate) Result += '\n';
				// 整形結果への区切空白追加
				else if(!Result.empty() && Result.back() != ' ' && Result.back() != '\n') Result += ' ';
				// コメント本文の出力
				AppendCommentBody(TailText, Tabs, IsSeparate, CommentInsertions[InsIdx].IsVerbatim);
				IsLineCommentOpen = OpensLineComment(TailText);
			}
			// 次行の空行判定にはコメントでなくコードの末尾の使用
			if(!Trimmed.empty()) PrevNonBlankLast = Trimmed.back();
			SawContent = true;
		}
	}
	// 行ループ後に未消費の挿入（ソース末尾以降の物）を末尾に出力
	for(; InsIdx < CommentInsertions.size(); ++InsIdx) {
		// 整形結果への改行追加
		if(!Result.empty() && Result.back() != '\n') Result += '\n';
		// コメント本文の出力
		AppendCommentBody(CommentInsertions[InsIdx].Text, 0, true, CommentInsertions[InsIdx].IsVerbatim);
	}
	// 原文の末尾改行の有無の保持
	if(!Result.empty() && !Source.empty() && Source.back() == '\n') Result += '\n';
	{
		// 構文木駆動のインデントが機能しない行の安全網：半角空白のみの先頭部をインデント幅単位でタブ化
		bool NeedsNormalize = false;
		size_t ScanVerbIdx = 0;
		for(size_t Pos = 0; Pos < Result.size();) {
			while(ScanVerbIdx < ResultVerbatimLines.size() && ResultVerbatimLines[ScanVerbIdx] < Pos) ++ScanVerbIdx;
			if(!(ScanVerbIdx < ResultVerbatimLines.size() && ResultVerbatimLines[ScanVerbIdx] == Pos) && Result[Pos] == ' ') {
				// 行頭コメントの該当状態
				size_t Lead = Pos;
				while(Lead < Result.size() && Result[Lead] == ' ') ++Lead;
				// 半角空白のみの先頭部で，行末・ドキュメントコメント継続・タブ混入の何れでもない場合に再構築が必要
				if(Lead < Result.size() && Result[Lead] != '*' && Result[Lead] != '\n' && Result[Lead] != '\t') {
					NeedsNormalize = true;
					break;
				}
			}
			const size_t Eol = Result.find('\n', Pos);
			if(Eol == std::string::npos) break;
			Pos = Eol + 1;
		}
		// 補正対象行が有る場合だけの逐語行を保つ再構築
		if(NeedsNormalize) {
			// 空白正規化後のコメント本文
			std::string Normalized;
			// 空白正規化後の出力の容量確保
			Normalized.reserve(Result.size());
			// 出力走査位置と逐語行索引
			size_t Pos = 0, BuildVerbIdx = 0;
			while(Pos < Result.size()) {
				// 改行位置と現在行の終端位置
				const size_t Eol = Result.find('\n', Pos), LineLimit = Eol == std::string::npos ? Result.size() : Eol;
				while(BuildVerbIdx < ResultVerbatimLines.size() && ResultVerbatimLines[BuildVerbIdx] < Pos) ++BuildVerbIdx;
				// 逐語行のインデント変換を省く逐語複写
				if(BuildVerbIdx < ResultVerbatimLines.size() && ResultVerbatimLines[BuildVerbIdx] == Pos) {
					// 空白正規化後の出力への追記
					Normalized.append(Result, Pos, LineLimit - Pos);
				} else {
					// 行頭コメントの該当状態
					size_t Lead = Pos;
					bool HasTab = false;
					while(Lead < LineLimit && (Result[Lead] == ' ' || Result[Lead] == '\t')) {
						if(Result[Lead] == '\t') HasTab = true;
						++Lead;
					}
					if(const size_t SpCount = Lead - Pos; HasTab || !SpCount || Lead < LineLimit && Result[Lead] == '*') {
						// 空白正規化後の出力への追記
						Normalized.append(Result, Pos, LineLimit - Pos);
					} else {
						// 空白正規化後の出力への追記
						Normalized.append(CeilTabs(SpCount, IndentChars), '\t');
						// 空白正規化後の出力への追記
						Normalized.append(Result, Lead, LineLimit - Lead);
					}
				}
				if(Eol == std::string::npos) break;
				Normalized += '\n';
				Pos = Eol + 1;
			}
			// 正規化済出力への差替
			Result = std::move(Normalized);
		}
	}
	// 添付への参照ビューを消費済の為，Assign 前に添付を破棄し，保存の費用の省略
	Src.ClearAttachments();
	// 変更判定は Assign に任せ，全文比較の重複防止
	Src.Assign(std::move(Result));
	// 終了
	return;
}
