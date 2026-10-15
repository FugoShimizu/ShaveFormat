#include "CodeLayout.hpp"
#include "FormatIR.hpp"
#include "NodeId.hpp"
#include "SyntaxModel.hpp"
#include "SyntaxRole.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include "../Util/LayoutTypes.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * トークン要素の先頭・末尾の形の取得関数（整形仕様 7.6）
	 * 規則から生成したトークンは其の種類から，ノードのトークンと逐語は最初と最後の葉のトークンの役割から，配置の段が挿入位置の前後の繋がりの判断に使う形を決める
	 * 計算量：ノードの深さ D に対し O(D)
	 * @param Syntax 読取専用の期待構文
	 * @param Item テキストを持つ解決済の要素
	 * @return 端のトークンの形の集合（`CodePiece::Shape`，該当する役割が無ければ `0`）
	 * @throws std::out_of_range 端のトークンのノード又は子の位置が範囲外の場合
	 */
	uint16_t CodeLayout::GetShapesOf(const SyntaxModel &Syntax, const ResolvedItem &Item) {
		// トークン要素の先頭・末尾の形の取得
		switch(Item.Generated) {
		case GeneratedToken::BlockOpen:
			// ブロックを開く波括弧の形の返戻
			return CodePiece::EndsOpeningBracket | CodePiece::EndsOpeningBrace | CodePiece::EndsContinuation;
		case GeneratedToken::BlockClose:
			// ブロックを閉じる波括弧の形の返戻
			return CodePiece::StartsClosingBracket | CodePiece::StartsClosingBrace;
		case GeneratedToken::CallOpen:
			// 呼出を開く丸括弧の形の返戻
			return CodePiece::IsOpenParen | CodePiece::EndsOpeningBracket | CodePiece::EndsContinuation;
		case GeneratedToken::CallClose:
			// 呼出を閉じる丸括弧の形の返戻
			return CodePiece::StartsClosingBracket | CodePiece::StartsCloseParen;
		case GeneratedToken::ListSeparator:
		case GeneratedToken::StmtTerminator:
			// 区切の形の返戻
			return CodePiece::StartsSeparator | CodePiece::EndsContinuation;
		case GeneratedToken::None:
			break;
		}
		// ノードを持たない要素の返戻
		if(Item.Node == NoNode) return 0;
		// 要素の最初と最後の葉のトークン
		const SyntaxNode *First = &Syntax.GetNode(Item.Node), *Last = First;
		// １つの子だけを持つ共通の経路を１度辿り，分岐した後だけ両端を別々に辿る
		while(First->Children.size() == 1) First = Last = &Syntax.GetNode(First->Children.front());
		if(!First->Children.empty()) {
			Last = &Syntax.GetNode(First->Children.back());
			First = &Syntax.GetNode(First->Children.front());
			while(!First->Children.empty()) First = &Syntax.GetNode(First->Children.front());
			while(!Last->Children.empty()) Last = &Syntax.GetNode(Last->Children.back());
		}
		// 役割と形の対応
		static constexpr std::pair<Role, uint16_t> LeadShapes[] = {
			{ Role::ClosingBraceLead, CodePiece::StartsClosingBrace },
			{ Role::ClosingBracketLead, CodePiece::StartsClosingBracket },
			{ Role::ClosingParenLead, CodePiece::StartsCloseParen },
			{ Role::EmptyParensLead, CodePiece::StartsEmptyParens },
			{ Role::ExprContinuationLead, CodePiece::StartsOperator },
			{ Role::OpeningParenToken, CodePiece::IsOpenParen },
			{ Role::SeparatorLead, CodePiece::StartsSeparator }
		};
		static constexpr std::pair<Role, uint16_t> TailShapes[] = {
			{ Role::ExprContinuationTail, CodePiece::EndsContinuation },
			{ Role::OpeningBraceTail, CodePiece::EndsOpeningBrace },
			{ Role::OpeningBracketTail, CodePiece::EndsOpeningBracket }
		};
		// 同じ葉の綴りの定義を，形毎に引き直さず１度だけ取得する
		const auto GetToken = [&Syntax](const SyntaxNode &Node) -> const KindDef * {
			// 種別の無い葉の返戻
			if(!Node.Kind.Def) return nullptr;
			// 可変長の補間の区切の固定の意味の返戻
			if(Node.Facts.DelimiterDef) return Node.Facts.DelimiterDef;
			const std::unordered_map<std::string_view, FormPart> &Tokens = Syntax.GetRules().TokenDefs;
			const std::unordered_map<std::string_view, FormPart>::const_iterator Found = Tokens.find(Node.Lexeme);
			// 実際の綴りの定義の返戻
			return Found == Tokens.end() ? nullptr : Found->second.Def;
		};
		const KindDef *const LeadToken = GetToken(*First), *const TailToken = First == Last ? LeadToken : GetToken(*Last);
		const KindDef *const LeadNamed = First->Kind.IsNamed() ? First->Kind.Def : nullptr;
		const KindDef *const TailNamed = First == Last ? LeadNamed : Last->Kind.IsNamed() ? Last->Kind.Def : nullptr;
		// 葉の役割（綴りに対応するトークンの役割か，名前付の葉の種別の役割）
		const auto Has = [](const KindDef *const Token, const KindDef *const Named, const Role Wanted) -> bool {
			// 役割の有無の返戻
			return Token && Token->Roles.test(static_cast<size_t>(Wanted)) || Named && Named->Roles.test(static_cast<size_t>(Wanted));
		};
		// 端の葉からのトークンの形の合成
		uint16_t Shapes = 0;
		for(const std::pair<Role, uint16_t> &Entry : LeadShapes) if(Has(LeadToken, LeadNamed, Entry.first)) Shapes |= Entry.second;
		for(const std::pair<Role, uint16_t> &Entry : TailShapes) if(Has(TailToken, TailNamed, Entry.first)) Shapes |= Entry.second;
		// 端のトークンの形の返戻
		return Shapes;
	}

	/**
	 * 子の断片・アンカー・配置枠の親の深さへの具体化関数
	 * 子のアンカー・配置枠を親の深さへ対応付け，子のノード番号は親のアンカーへ混ぜない
	 * 計算量：子の断片の数 F，ノードの数 V，トークン片と配置枠の数 S に対し O(F + V + S log F)
	 * @param Code 親の確定したコードと配置枠
	 * @param Region 親の中の子の範囲と配置先
	 * @throws FormatLimitExceeded 子の断片の数と親の配置範囲が一致しない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	void CodeLayout::PlaceChild(LaidOutCode &Code, OwnedRegionSpan &Region) {
		// 子の断片・アンカー・配置枠の親の深さへの具体化
		const LaidOutCode &Child = *Code.Anchors.Syntax->GetRegionOf(Region.Node)->Code;
		const std::span<const CodeFragment> Src = Child.GetFragments();
		if(Src.size() != Region.EndFragment - Region.FirstFragment) {
			throw FormatLimitExceeded("embedded fragment mapping is incomplete", FailureReason::FormatterDefect);
		}
		LaidOutCode Placed;
		Placed.Anchors = Child.GetAnchors();
		Placed.Ending = Child.GetEnding();
		Placed.FollowingTabs = 0;
		Placed.Slots = Child.GetSlots();
		Placed.Slots.Owner = Placed.Identity;
		// 子のテキストを共有する断片の親の深さへの配置と，子の位置毎のマッピング
		uint32_t Cursor = 0;
		std::vector<uint32_t> Prefixes;
		Prefixes.reserve(Src.size());
		Placed.Fragments.reserve(Src.size());
		Placed.Separators.reserve(Child.GetSeparators().size());
		for(uint32_t Idx = 0; Idx < Src.size(); ++Idx) {
			const uint32_t Parent = Region.FirstFragment + Idx;
			const CodeFragment &Row = Code.Fragments[Parent];
			Prefixes.push_back(Row.End - Row.Start - (Src[Idx].End - Src[Idx].Start));
			Placed.Fragments.push_back(Row);
			Placed.Fragments.back().Start = Cursor;
			Placed.Fragments.back().End = Cursor + Row.End - Row.Start;
			Placed.Anchors.FragmentTabs[Idx] = Code.Anchors.FragmentTabs[Parent];
			Placed.Slots.LineKinds[Idx] &= Code.Slots.LineKinds[Parent];
			Cursor = Placed.Fragments.back().End + 1;
		}
		// 子のテキストの位置から親の位置への変換
		const auto Pos = [&](const uint32_t Fragment, const uint32_t Offset) -> uint32_t {
			// 子の同じトークンの位置の返戻
			return Placed.Fragments[Fragment].Start + Prefixes[Fragment] + Offset - Src[Fragment].Start;
		};
		for(NodeSpan &Span : Placed.Anchors.Nodes) if(Span.IsPlaced) {
			Span.Start = Pos(Span.StartFragment, Span.Start);
			Span.End = Pos(Span.EndFragment, Span.End);
		}
		// 子の位置を含む断片の照会
		const auto FragmentOf = [&Src](const uint32_t Offset) -> uint32_t {
			const std::span<const CodeFragment>::iterator After = std::ranges::upper_bound(Src, Offset, {}, &CodeFragment::Start);
			// トークンを含む子の断片の返戻
			return static_cast<uint32_t>(After - Src.begin() - 1);
		};
		for(CodePiece &Piece : Placed.Anchors.Pieces) {
			const uint32_t Fragment = FragmentOf(Piece.Start);
			Piece.Start = Pos(Fragment, Piece.Start);
			Piece.End = Pos(Fragment, Piece.End);
		}
		for(const std::pair<uint32_t, uint32_t> &Separator : Child.GetSeparators()) {
			Placed.Separators.emplace_back(
				Pos(FragmentOf(Separator.first), Separator.first),
				Pos(FragmentOf(Separator.second), Separator.second)
			);
		}
		for(std::pair<uint32_t, uint32_t> &Slot : Placed.Slots.InlineSlots) {
			Slot.first = Pos(Slot.second, Slot.first);
			Code.Slots.InlineSlots.emplace_back(
				Code.Fragments[Region.FirstFragment + Slot.second].Start + Slot.first - Placed.Fragments[Slot.second].Start,
				Region.FirstFragment + Slot.second
			);
		}
		// 子の末尾の独立行は閉タグの行頭を行先とし，深さは後続の閉タグの段に保つ
		const SyntaxModel &Syntax = *Code.Anchors.Syntax;
		for(const NodeId Boundary : Syntax.GetChildrenOf(Syntax.GetParentOf(Region.Node))) {
			if(!Syntax.Is(Boundary, Role::MarkupClose)) continue;
			const NodeSpan &Close = Code.Anchors.Nodes[Boundary];
			if(!Close.IsPlaced) continue;
			if(
				const CodeFragment &Row = Code.Fragments[Close.StartFragment];
				Row.Start + Row.PrefixTabs + Row.Text.find_first_not_of('\t') != Close.Start
			) continue;
			Region.Terminal = Code.Slots.Find(CommentSlotKind::BeforeLine, Close.StartFragment, 0);
			if(Region.Terminal) Placed.FollowingTabs = Code.Anchors.FragmentTabs[Close.StartFragment];
			break;
		}
		Placed.Slots.HasDocEnd = Region.Terminal.has_value();
		Region.Placement = std::make_shared<const LaidOutCode>(std::move(Placed));
		// 終了
		return;
	}

	/**
	 * 変更不能なコードの出力の構築関数（整形仕様 6.5）
	 * 深さの範囲が行を跨ぐ時に内容を１段深くし，同じ行で開いた複数の範囲を重ねて深くせず，閉じの行は開いた行の深さへ戻す
	 * 範囲を閉じた行で開く範囲 (`} else {`) は閉じた範囲の起点を引き継ぎ，節見出（`case`・ラベル・アクセス修飾子等）で始まる行は内容より１段浅くして其の行で開く範囲を外側の範囲の起点に揃える
	 * 空行は構造から決めた物だけを断片として固定し，ファイルの先頭・末尾と連続する空行を作らない
	 * 計算量：要素の数 N，宣言の深さ D，子のノードの数 V，配置枠の数 S，断片の数 F，出力の長さ M に対し O((N + F) * (D + 1) + V + (N + S) log(N + S) + S log F + M)
	 * @param IR 全ての選択を解決した中間表現
	 * @param TextWork 非空の本文の参照数と連続範囲の数の格納先（無ければ観測しない）
	 * @return 変更不能なコードの出力
	 * @throws FormatLimitExceeded 出力が大きさの上限を超える場合，又は埋込の子の断片数と親の配置範囲が一致しない場合（フォーマッターの不具合）
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	LaidOutCode CodeLayout::MaterializeWithWork(const BrokenIR &IR, std::pair<size_t, size_t> *const TextWork) {
		// 出力と追跡状態の準備
		LaidOutCode Code;
		const SyntaxModel &Syntax = *IR.Expectation.NormalizedSyntax;
		// 親のテキストの参照（出力上限を確認してから実体化する）
		std::vector<std::string_view> TextParts;
		// 出力の既存の走査で求めた件数を使い，容量の為だけの再走査を行わない
		TextParts.reserve(IR.TextItemCount);
		Code.Anchors.Pieces.reserve(IR.TextItemCount);
		if(TextWork) *TextWork = { 0, 0 };
		size_t TextBytes = 0; // 生成するインデントと共有する子の本文を除いた，親の本文の長さ
		uint32_t Pos = 0;
		// テキストを確保する前の行の配置計画
		struct FragmentPlan {
			uint32_t Start;
			uint32_t End;
			bool IsBlank;
			uint8_t Breaks = 0; // リテラル内の改行の所属
			uint32_t Tabs = 0; // 行のインデントの深さ
			const CodeFragment *Shared = nullptr; // 子が所有する変更不能な行
		};
		std::vector<FragmentPlan> Fragments;
		if(IR.LineBreakCount || IR.TextItemCount) {
			if(IR.LineBreakCount >= Fragments.max_size()) throw std::length_error("layout fragment count exceeds capacity");
			Fragments.reserve(IR.LineBreakCount + 1);
		}
		std::vector<NodeSpan> &Spans = Code.Anchors.Nodes;
		Code.Anchors.Syntax = IR.Expectation.NormalizedSyntax;
		// アンカーのノードは正規化後のノードに限る為，全てのノードの範囲を先に用意する（配置の段はノード番号で範囲を引く）
		Spans.resize(Syntax.GetNodeCount());
		std::vector<uint32_t> &FragmentTabs = Code.Anchors.FragmentTabs; // 断片毎のインデントの深さ
		// 深さの範囲の起点と，同じ起点で開いた範囲の数
		struct ScopeOrigin {
			uint32_t Line;
			uint32_t Count;
			uint32_t PreservedDepth;
		};
		std::vector<ScopeOrigin> ScopeLines;
		std::vector<uint32_t> ScopeStarts; // 深さの範囲を開いた時の内容断片の数
		uint32_t ContentFragments = 0;
		std::vector<uint8_t> &VerbatimBreaks = Code.Anchors.VerbatimBreaks;
		std::vector<NodeId> OpenLiterals; // 確定したアンカーの開始と終了で継ぐ外側からのリテラル
		// 内容を出した後に閉じた最も外側の範囲を開いた行（閉じの行は其の深さへ戻す）
		std::optional<uint32_t> ClosedLine;
		uint32_t ClosedDepth = UINT32_MAX; // 逐語の先頭空白で始まる行は生成タブ数から構造上の深さを得れない為，範囲の開始時に控える
		std::vector<uint32_t> LineClosed, UnplacedClosed; // 此の行へ配置した閉じの起点と，次の内容の行へ送る閉じの起点
		size_t Inherited = 0; // 此の行で開く範囲が引き継いだ閉じの数
		std::optional<uint32_t> LineOrigin; // 此の行で最後に開いた範囲の起点
		// 現在の行の開始・節見出・空行の状態と，行頭位置
		bool IsLineOpen = false, IsHeaderLine = false, IsBlankPending = false;
		uint32_t LineStart = 0, LineTabs = 0;
		// 内容の前に始まったアンカーと，最後に出した内容の終わり
		std::vector<NodeId> PendingBegins;
		uint32_t LastFragment = 0, LastEnd = 0;
		NodeId LastEndedAnchor = NoNode;
		// ノードの出力上の範囲
		const auto SpanOf = [&Spans](const NodeId Node) -> NodeSpan & {
			// ノードの範囲の返戻
			return Spans.at(Node);
		};
		// 出力位置の加算（上限超過時はテキストの確保前に FormatLimitExceeded を送出する）
		const auto Advance = [&Pos](const size_t Amount) -> void {
			if(Amount > MaxInputBytes - Pos) throw FormatLimitExceeded("output exceeds size limit");
			Pos += static_cast<uint32_t>(Amount);
			// 終了
			return;
		};
		// 行の開始（前の行の後の改行・空行，範囲の深さのタブ）
		const auto OpenLine = [&]() -> void {
			const bool HasPrevLine = !Fragments.empty();
			// 前の行との改行と構造上の空行の出力
			if(HasPrevLine) Advance(1);
			if(IsBlankPending && HasPrevLine) {
				const uint32_t Fragment = static_cast<uint32_t>(Fragments.size());
				if(!ScopeLines.empty() && ScopeLines.back().Line >= Fragment) ++ScopeLines.back().Line;
				if(LineOrigin && *LineOrigin >= Fragment) ++*LineOrigin;
				const uint32_t Blank = Pos;
				Fragments.push_back({ Blank, Blank, true });
				Advance(1);
			}
			// 新しい行の開始位置の確定
			IsBlankPending = false;
			LineStart = Pos;
			// 範囲を閉じた直後の行は開いた行の深さ，其の他は前の行で開いた範囲の数（同じ行で開いた範囲は１段と数える）
			const uint32_t Cur = static_cast<uint32_t>(Fragments.size());
			uint32_t Depth = 0;
			if(ClosedLine && *ClosedLine < Cur) Depth = ClosedDepth == UINT32_MAX ? Fragments[*ClosedLine].Tabs : ClosedDepth;
			else {
				Depth = static_cast<uint32_t>(ScopeLines.size());
				if(Depth && static_cast<uint64_t>(ScopeLines.back().Line) >= Cur) --Depth;
			}
			// 節見出で始まる行の１段浅い深さ（整形仕様 6.5）
			IsHeaderLine = std::any_of(
				PendingBegins.begin(),
				PendingBegins.end(),
				// 節見出を始める子を見付ける述語
				[&Syntax](const NodeId Node) -> bool {
					// 名前付の節見出のノードかの返戻
					return Syntax.IsNamed(Node) && Syntax.Is(Node, Role::SectionHeader);
				}
			);
			if(IsHeaderLine && Depth) --Depth;
			Advance(Depth);
			LineTabs = Depth;
			IsLineOpen = true;
			// 終了
			return;
		};
		// 行の終了
		const auto CloseLine = [&]() -> void {
			// 開いた行の無い場合の終了
			if(!IsLineOpen) return;
			// 行の断片の記録と一時状態の消去
			Fragments.push_back({ LineStart, Pos, false, 0, LineTabs });
			++ContentFragments;
			IsLineOpen = false;
			IsHeaderLine = false;
			LineClosed.clear();
			Inherited = 0;
			LineOrigin.reset();
			// 終了
			return;
		};
		// 内容の前に閉じた範囲の此の行への所属
		const auto PlaceClosed = [&LineClosed, &UnplacedClosed]() -> void {
			LineClosed.insert(LineClosed.end(), UnplacedClosed.begin(), UnplacedClosed.end());
			UnplacedClosed.clear();
			// 終了
			return;
		};
		// 内容の追記（行を開き，内容の前に始まったアンカーの始まりを確定する）
		const auto Append = [&](const std::string_view Content, const bool ShouldRecordPiece) -> void {
			if(!IsLineOpen) OpenLine();
			PlaceClosed();
			// 内容の前に始まったアンカーの位置の確定
			for(const NodeId Node : PendingBegins) {
				NodeSpan &Span = SpanOf(Node);
				Span.StartFragment = static_cast<uint32_t>(Fragments.size());
				Span.Start = Pos;
				Span.IsPlaced = true;
			}
			PendingBegins.clear();
			ClosedLine.reset();
			// 区切の空白を除いたトークン片の記録（配置の段が挿入位置の前後の繋がりを判ずる）
			if(ShouldRecordPiece) if(const size_t First = Content.find_first_not_of(" \t\r"); First != std::string_view::npos) {
				const uint32_t Start = Pos;
				Code.Anchors.Pieces.push_back(
					{ Start + static_cast<uint32_t>(First), Start + static_cast<uint32_t>(Content.find_last_not_of(" \t\r") + 1), 0 }
				);
			}
			// 内容の追加と末尾位置の更新
			Advance(Content.size());
			TextBytes += Content.size(); // 出力上限を確認した長さの一部なので，積算も同じ上限内
			if(!Content.empty()) {
				if(TextWork) ++TextWork->first;
				// 同じ中間表現の本文で連続する範囲は，１つの参照として最終のコピーへ渡す
				if(!TextParts.empty() && TextParts.back().data() + TextParts.back().size() == Content.data()) {
					TextParts.back() = std::string_view(TextParts.back().data(), TextParts.back().size() + Content.size());
				} else TextParts.push_back(Content);
			}
			LastFragment = static_cast<uint32_t>(Fragments.size());
			LastEnd = Pos;
			// 終了
			return;
		};
		// アンカーの終端の記録（内容の無い通常のアンカーは除き，コード０行の子の所有境界は残す）
		const auto EndAnchor = [&](const NodeId Node) -> void {
			if(const OwnedRegion *const Region = Syntax.GetRegionOf(Node); Region && Region->Code->GetFragments().empty()) {
				// コード０行の子の所有境界の現在位置への記録
				std::erase(PendingBegins, Node);
				const uint32_t Fragment = static_cast<uint32_t>(Fragments.size());
				Code.Anchors.OwnedRegions.push_back({ Node, Fragment, Fragment });
				// 零幅の所有境界の記録後の終了
				return;
			}
			if(
				const std::vector<NodeId>::iterator Pending = std::find(PendingBegins.begin(), PendingBegins.end(), Node);
				Pending != PendingBegins.end()
			) {
				PendingBegins.erase(Pending);
				// 内容を出さなかったアンカーの終了
				return;
			}
			// 内容を持つアンカーの終端位置の記録
			SpanOf(Node).EndFragment = LastFragment;
			SpanOf(Node).End = LastEnd;
			// 終了
			return;
		};
		// 要素の出したトークン片の最初へ先頭の形，最後へ末尾の形の付与
		const auto MarkShapes = [&Code, &Syntax](const ResolvedItem &Item, const size_t PiecesBefore) -> void {
			std::vector<CodePiece> &Pieces = Code.Anchors.Pieces;
			// トークン片を出さなかった要素の終了
			if(Pieces.size() == PiecesBefore) return;
			const uint16_t Shapes = GetShapesOf(Syntax, Item);
			Pieces[PiecesBefore].Shapes |= Shapes & CodePiece::Leads;
			Pieces.back().Shapes |= Shapes & CodePiece::Tails;
			// 終了
			return;
		};
		// 解決済の各要素の断片と位置への反映
		for(const ResolvedItem &Item : IR.Items) switch(Item.Kind) {
		case ResolvedKind::Text:
			{
				// アンカーを兼ねるトークンのテキストとノードの出力の対応付
				if(Item.Node != NoNode) PendingBegins.push_back(Item.Node);
				const size_t PiecesBefore = Code.Anchors.Pieces.size();
				Append(IR.GetTextOf(Item), true);
				MarkShapes(Item, PiecesBefore);
				if(Item.Node != NoNode) EndAnchor(Item.Node);
				LastEndedAnchor = Item.Node;
				break;
			}
		case ResolvedKind::Separator:
			// 文法がコメントと読む区切記法は空行の照合基準で除く（整形仕様 8.1）
			Append(IR.GetTextOf(Item), true);
			Code.Separators.emplace_back(Pos - Item.Text.Size, Pos);
			break;
		case ResolvedKind::Verbatim:
			{
				// 改行を含む逐語は続きの行へインデントを付けない
				const std::string_view Content = IR.GetTextOf(Item);
				if(Item.Node != NoNode) PendingBegins.push_back(Item.Node);
				const size_t PiecesBefore = Code.Anchors.Pieces.size();
				for(size_t Cursor = 0;;) {
					const size_t Newline = Content.find('\n', Cursor);
					Append(Content.substr(Cursor, Newline == std::string_view::npos ? std::string_view::npos : Newline - Cursor), true);
					if(Newline == std::string_view::npos) break;
					// 開いたリテラルの実際の出力が改行の前に在るか（本文の先頭改行だけでは開始行を内部にしない）
					const bool IsInsideLiteral = !OpenLiterals.empty() && SpanOf(OpenLiterals.front()).Start < Pos;
					CloseLine();
					// 補間のコードの末尾も，外側のリテラルのアンカーの内側なら差込禁止の境界
					Fragments.back().Breaks =
					AnchorPositionMap::NextInVerbatim | (IsInsideLiteral || Cursor || Newline ? AnchorPositionMap::EndsInVerbatim : 0);
					Advance(1);
					LineStart = Pos;
					LineTabs = 0;
					IsLineOpen = true;
					Cursor = Newline + 1;
				}
				MarkShapes(Item, PiecesBefore);
				if(Item.Node != NoNode) EndAnchor(Item.Node);
				break;
			}
		case ResolvedKind::OwnedRegion:
			{
				// 子が確定した全断片の同じ順での配置（空行と逐語の改行も子のコードの事実）
				const LaidOutCode &Child = *Syntax.GetRegionOf(Item.Node)->Code;
				CloseLine();
				uint32_t FirstFragment = static_cast<uint32_t>(Fragments.size());
				PendingBegins.push_back(Item.Node);
				const std::span<const CodeFragment> Rows = Child.GetFragments();
				const AnchorPositionMap &ChildAnchors = Child.GetAnchors();
				const std::span<const std::pair<uint32_t, uint32_t>> Separators = Child.GetSeparators();
				size_t NextPiece = 0, NextSeparator = 0;
				for(uint32_t Row = 0; Row < Rows.size(); ++Row) {
					CloseLine();
					if(
						const bool IsInside = Row && ChildAnchors.VerbatimBreaks[Row - 1] & AnchorPositionMap::NextInVerbatim;
						Rows[Row].IsBlank || IsInside
					) {
						if(!Fragments.empty()) Advance(1);
						LineStart = Pos;
						LineTabs = 0;
						IsLineOpen = true;
					} else OpenLine();
					IsBlankPending = false;
					if(!Row) FirstFragment = static_cast<uint32_t>(Fragments.size());
					const uint32_t Start = Pos;
					Append(std::string_view(), false);
					Advance(Rows[Row].End - Rows[Row].Start);
					LastEnd = Pos;
					LineTabs += ChildAnchors.FragmentTabs[Row];
					CloseLine();
					Fragments.back().IsBlank = Rows[Row].IsBlank;
					Fragments.back().Shared = &Rows[Row];
					Fragments.back().Breaks = ChildAnchors.VerbatimBreaks[Row];
					// 子のトークン片と区切記法の位置の親へのマッピング
					while(NextPiece < ChildAnchors.Pieces.size() && ChildAnchors.Pieces[NextPiece].Start < Rows[Row].End) {
						const CodePiece &Piece = ChildAnchors.Pieces[NextPiece++];
						Code.Anchors.Pieces.push_back({ Start + Piece.Start - Rows[Row].Start, Start + Piece.End - Rows[Row].Start, Piece.Shapes });
					}
					while(NextSeparator < Separators.size() && Separators[NextSeparator].first < Rows[Row].End) {
						const std::pair<uint32_t, uint32_t> &Separator = Separators[NextSeparator++];
						Code.Separators.emplace_back(Start + Separator.first - Rows[Row].Start, Start + Separator.second - Rows[Row].Start);
					}
				}
				Code.Anchors.OwnedRegions.push_back({ Item.Node, FirstFragment, static_cast<uint32_t>(Fragments.size()) });
				EndAnchor(Item.Node);
				break;
			}
		case ResolvedKind::Newline:
			CloseLine();
			break;
		case ResolvedKind::BlankLine:
			CloseLine();
			// 構造種別の違い又は前後の宣言が複数行の時の空行（整形仕様 6.5）
			IsBlankPending = IsBlankPending || Item.IsForced || IR.Multiline.contains(Item.Before) || IR.Multiline.contains(Item.After);
			break;
		case ResolvedKind::IndentBegin:
			{
				// 節見出の行は外側の範囲の起点，閉じた範囲の後は其の起点，其の他は此の行で先に開いた範囲の起点か此の行
				PlaceClosed();
				uint32_t Origin = static_cast<uint32_t>(Fragments.size());
				if(IsHeaderLine && !ScopeLines.empty()) Origin = ScopeLines.back().Line;
				else if(Inherited < LineClosed.size()) Origin = LineClosed[Inherited++];
				else if(LineOrigin) Origin = *LineOrigin;
				LineOrigin = Origin;
				if(!ScopeLines.empty() && ScopeLines.back().Line == Origin) ++ScopeLines.back().Count;
				else {
					ScopeLines.push_back(
						{
							Origin,
							1,
							Origin && Origin - 1 < Fragments.size() && Fragments[Origin - 1].Breaks & AnchorPositionMap::NextInVerbatim ?
							static_cast<uint32_t>(ScopeLines.size()) :
							UINT32_MAX
						}
					);
				}
				ScopeStarts.push_back(ContentFragments);
				break;
			}
		case ResolvedKind::IndentEnd:
			// 行を跨がなかった範囲（平坦に選んだグループの範囲）は深さにも閉じの行にも関わらない
			if(Syntax.Is(LastEndedAnchor, Role::IndentedBody)) {
				ClosedLine.reset();
				UnplacedClosed.clear();
			} else if(ScopeStarts.back() != ContentFragments) {
				ClosedLine = ScopeLines.back().Line;
				ClosedDepth = ScopeLines.back().PreservedDepth;
				UnplacedClosed.push_back(ScopeLines.back().Line);
			}
			if(!--ScopeLines.back().Count) ScopeLines.pop_back();
			ScopeStarts.pop_back();
			LastEndedAnchor = NoNode;
			break;
		case ResolvedKind::AnchorBegin:
			if(IsLiteralAnchor(Syntax, Item.Node)) OpenLiterals.push_back(Item.Node);
			PendingBegins.push_back(Item.Node);
			break;
		case ResolvedKind::AnchorEnd:
			if(IsLiteralAnchor(Syntax, Item.Node)) OpenLiterals.pop_back();
			EndAnchor(Item.Node);
			LastEndedAnchor = Item.Node;
			break;
		}
		CloseLine();
		// 逐語の末尾データと表示上の終端境界は其のテキストで終え，其の他のコードだけ終端契約の改行を補う
		const std::span<const NodeId> RootChildren = Syntax.GetChildrenOf(Syntax.GetRoot());
		const bool HasTrailingData = !RootChildren.empty() && Syntax.Is(RootChildren.back(), Role::TrailingDataSection);
		const SemanticBoundary *const Terminal =
		Syntax.GetNode(Syntax.GetRoot()).Facts.GetBoundaryBefore(static_cast<uint32_t>(RootChildren.size()));
		const bool HasLiteralEnding =
		Terminal && Terminal->Kind == BoundaryKind::Preserved && Terminal->Src == PreservedSrc::LiteralContent;
		Code.Ending.DoesEndWithNewline =
		!HasTrailingData && !HasLiteralEnding && !Fragments.empty() && (Fragments.back().End != Fragments.back().Start || !Pos);
		// 終端込みの正確な出力長の確認後の，参照したテキストの１度の実体化（整形仕様 6.7, 9.1）
		if(Code.Ending.DoesEndWithNewline && Pos > MaxInputBytes - 1) throw FormatLimitExceeded("output exceeds size limit");
		// 親の本文を１度繋ぎ，行毎の個別確保を行わず変更不能な範囲で共有する
		std::string Text;
		Text.reserve(TextBytes);
		if(TextWork) TextWork->second = TextParts.size();
		for(const std::string_view Part : TextParts) Text += Part;
		std::shared_ptr<const std::string> Storage;
		Code.Fragments.reserve(Fragments.size());
		FragmentTabs.reserve(Fragments.size());
		VerbatimBreaks.reserve(Fragments.size());
		size_t TextOffset = 0;
		for(const FragmentPlan &Fragment : Fragments) {
			FragmentTabs.push_back(Fragment.Tabs);
			VerbatimBreaks.push_back(Fragment.Breaks);
			// 子の本文は其のメモリを共有し，追加のインデントだけを別に保持する
			if(Fragment.Shared) {
				Code.Fragments.push_back(*Fragment.Shared);
				CodeFragment &Row = Code.Fragments.back();
				Row.Start = Fragment.Start;
				Row.End = Fragment.End;
				Row.IsBlank = Fragment.IsBlank;
				Row.PrefixTabs = Fragment.End - Fragment.Start - static_cast<uint32_t>(Row.Text.size());
				continue;
			}
			// 生成したインデントは既存の `PrefixTabs` に保ち，シリアライズ時だけタブを出す
			const uint32_t PrefixTabs = Fragment.Tabs, TextSize = Fragment.End - Fragment.Start - PrefixTabs;
			if(!Storage) Storage = std::make_shared<const std::string>(std::move(Text));
			Code.Fragments.emplace_back(Fragment.Start, Fragment.End, Fragment.IsBlank, Storage, PrefixTabs);
			Code.Fragments.back().Text = std::string_view(*Storage).substr(TextOffset, TextSize);
			TextOffset += TextSize;
		}
		// 置ける零幅の位置だけの配置枠の表（行単位は空行と差込禁止領域に接する物を除き，文中はノードの出力の端，整形仕様 3.3）
		CommentSlotTable &Slots = Code.Slots;
		Slots.Owner = Code.Identity;
		Slots.LineKinds.assign(Fragments.size(), 0);
		for(uint32_t Fragment = 0; Fragment < Fragments.size(); ++Fragment) {
			if(Fragments[Fragment].IsBlank) continue;
			// 前の断片の終端が逐語領域の改行なら，此の行の前を逐語領域とする
			// 此の断片の終端が逐語領域の改行なら，行末と次の行の前を逐語領域とする
			if(!Fragment || !(VerbatimBreaks[Fragment - 1] & AnchorPositionMap::NextInVerbatim)) {
				Slots.LineKinds[Fragment] |= 1 << static_cast<uint8_t>(CommentSlotKind::BeforeLine);
			}
			if(!(VerbatimBreaks[Fragment] & AnchorPositionMap::EndsInVerbatim)) {
				Slots.LineKinds[Fragment] |=
				1 << static_cast<uint8_t>(CommentSlotKind::LineEnd) | 1 << static_cast<uint8_t>(CommentSlotKind::OwnLine);
			}
		}
		// ノードの出力端から文中の配置枠の収集
		for(const NodeSpan &Span : Spans) if(Span.IsPlaced) {
			if(!Fragments[Span.StartFragment].IsBlank) Slots.InlineSlots.emplace_back(Span.Start, Span.StartFragment);
			if(!Fragments[Span.EndFragment].IsBlank) Slots.InlineSlots.emplace_back(Span.End, Span.EndFragment);
		}
		// 子の配置枠と深さの具体化（親の通常のアンカーとは番号を分ける）
		for(OwnedRegionSpan &Region : Code.Anchors.OwnedRegions) PlaceChild(Code, Region);
		// 文中の配置枠の重複除去
		std::sort(Slots.InlineSlots.begin(), Slots.InlineSlots.end());
		Slots.InlineSlots.erase(std::unique(Slots.InlineSlots.begin(), Slots.InlineSlots.end()), Slots.InlineSlots.end());
		// 変更不能なコードの出力の返戻
		return Code;
	}

	/**
	 * 変更不能なコードの出力の構築関数（整形仕様 6.5）
	 * 計算量：配置の総費用 Q に対し O(Q)
	 * @param IR 全ての選択を解決した中間表現
	 * @return 変更不能なコードの出力
	 * @throws FormatLimitExceeded 出力が大きさの上限を超える場合，又は埋込の子の断片数と親の配置範囲が一致しない場合（フォーマッターの不具合）
	 * @throws std::bad_alloc 出力又は作業列を確保出来ない場合
	 * @throws std::length_error 出力又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range 構文又は共有テキストの参照先が範囲外の場合
	 */
	LaidOutCode CodeLayout::Materialize(const BrokenIR &IR) {
		// 共通の配置結果の返戻
		return MaterializeWithWork(IR, nullptr);
	}
} // namespace Core
