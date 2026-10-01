#include "CodeLayout.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 空行の規則が参照する宣言の内の複数行の宣言の収集関数
	 * 宣言の出力の中に解決済の改行が在り，其の後に内容が続く物と，改行を含む逐語其の物の宣言を複数行とする
	 * 計算量：解決済の要素の数 N と規則が参照する宣言の入れ子の深さ D に対し O(N * D)
	 * @param IR 全ての選択を解決した中間表現
	 * @return 複数行の宣言の集合
	 */
	std::unordered_set<NodeId> CodeLayout::MultilineNodes(const BrokenIR &IR) {
		// 空行の規則が参照する宣言の収集
		std::unordered_set<NodeId> Referenced, Result;
		for(const ResolvedItem &Item : IR.Items) {
			if(Item.Kind != ResolvedKind::BlankLine) continue;
			if(Item.Before != NoNode) Referenced.insert(Item.Before);
			if(Item.After != NoNode && Item.After != Item.Before) Referenced.insert(Item.After);
		}
		// 開いた参照される宣言と，其の中で改行を見たか
		std::vector<std::pair<NodeId, bool>> Open;
		// 改行後に内容を持つ宣言の収集
		for(const ResolvedItem &Item : IR.Items) switch(Item.Kind) {
		case ResolvedKind::AnchorBegin:
			if(Referenced.contains(Item.Node)) Open.emplace_back(Item.Node, false);
			break;
		case ResolvedKind::AnchorEnd:
			if(!Open.empty() && Open.back().first == Item.Node) Open.pop_back();
			break;
		case ResolvedKind::Newline:
			for(std::pair<NodeId, bool> &Entry : Open) Entry.second = true;
			break;
		case ResolvedKind::OwnedRegion:
			// 所有区画は改行の後に置く内容で，其の行は後の内容と別の行に為る
			for(std::pair<NodeId, bool> &Entry : Open) {
				if(Entry.second) Result.insert(Entry.first);
				Entry.second = true;
			}
			break;
		case ResolvedKind::Text:
		case ResolvedKind::Separator:
		case ResolvedKind::Verbatim:
			{
				const bool IsMultiline = Item.Kind == ResolvedKind::Verbatim && IR.TextOf(Item).find('\n') != std::string_view::npos;
				for(const std::pair<NodeId, bool> &Entry : Open) if(Entry.second || IsMultiline) Result.insert(Entry.first);
				if(IsMultiline && Referenced.contains(Item.Node)) Result.insert(Item.Node);
				break;
			}
		}
		// 複数行の宣言の返戻
		return Result;
	}

	/**
	 * 字句の要素の端の字句の形の取得関数（整形仕様 7.6）
	 * 規則から生成した字句は其の種類から，節点の字句と逐語は最初と最後の葉の字句の役割から，配置の段が挿入位置の前後の繋がりの判断に使う形を決める
	 * @param Syntax 読取専用の期待構文
	 * @param Item 字面を持つ解決済の要素
	 * @return 端の字句の形の集合（`CodePiece::Shape`，字句の役割を持たない要素は無し）
	 */
	uint16_t CodeLayout::ShapesOf(const SyntaxModel &Syntax, const ResolvedItem &Item) {
		switch(Item.Generated) {
		case GeneratedToken::BlockOpen:
			// 塊を開く波括弧の形の返戻
			return CodePiece::EndsOpeningBracket | CodePiece::EndsOpeningBrace | CodePiece::EndsContinuation;
		case GeneratedToken::BlockClose:
			// 塊を閉じる波括弧の形の返戻
			return CodePiece::StartsClosingBracket | CodePiece::StartsClosingBrace;
		case GeneratedToken::CallOpen:
			// 呼出を開く丸括弧の形の返戻
			return CodePiece::IsOpenParen | CodePiece::EndsOpeningBracket | CodePiece::EndsContinuation;
		case GeneratedToken::CallClose:
			// 呼出を閉じる丸括弧の形の返戻
			return CodePiece::StartsClosingBracket | CodePiece::StartsCloseParen;
		case GeneratedToken::ListSeparator:
		case GeneratedToken::StatementTerminator:
			// 区切りの形の返戻
			return CodePiece::StartsSeparator | CodePiece::EndsContinuation;
		case GeneratedToken::None:
			break;
		}
		// 節点を持たない要素の返戻
		if(Item.Node == NoNode) return 0;
		// 要素の最初と最後の葉の字句
		NodeId First = Item.Node, Last = Item.Node;
		while(Syntax.ChildCount(First)) First = Syntax.Child(First, 0);
		while(const uint32_t Count = Syntax.ChildCount(Last)) Last = Syntax.Child(Last, Count - 1);
		// 役割と形の対応
		static constexpr std::pair<Role, uint16_t> LeadShapes[] = {
			{ Role::ClosingBraceLead, CodePiece::StartsClosingBrace },
			{ Role::ClosingBracketLead, CodePiece::StartsClosingBracket },
			{ Role::ClosingParenLead, CodePiece::StartsCloseParen },
			{ Role::EmptyParenthesesLead, CodePiece::StartsEmptyParens },
			{ Role::ExpressionContinuationLead, CodePiece::StartsOperator },
			{ Role::OpeningParenToken, CodePiece::IsOpenParen },
			{ Role::SeparatorLead, CodePiece::StartsSeparator }
		};
		static constexpr std::pair<Role, uint16_t> TailShapes[] = {
			{ Role::ExpressionContinuationTail, CodePiece::EndsContinuation },
			{ Role::OpeningBraceTail, CodePiece::EndsOpeningBrace },
			{ Role::OpeningBracketTail, CodePiece::EndsOpeningBracket }
		};
		// 葉の役割（綴りに対応する字句の役割か，名前付の葉の種別の役割）
		const auto Has = [&Syntax](const NodeId Leaf, const Role Wanted) -> bool {
			// 役割の有無の返戻
			return Syntax.IsTokenRole(Leaf, Wanted) || Syntax.IsNamed(Leaf) && Syntax.Is(Leaf, Wanted);
		};
		// 端の葉からの字句の形の合成
		uint16_t Shapes = 0;
		for(const auto &[Wanted, Shape] : LeadShapes) if(Has(First, Wanted)) Shapes |= Shape;
		for(const auto &[Wanted, Shape] : TailShapes) if(Has(Last, Wanted)) Shapes |= Shape;
		// 端の字句の形の返戻
		return Shapes;
	}

	/**
	 * 変更不能なコードの出力の構築関数（整形仕様 6.5）
	 * 深さの範囲が行を跨ぐ時に内容を１段深くし，同じ行で開いた複数の範囲を重ねて深くせず，閉じの行は開いた行の深さへ戻す
	 * 範囲を閉じた行で開く範囲 (`} else {`) は閉じた範囲の起点を引き継ぎ，節見出（`case`・ラベル・アクセス修飾子等）で始まる行は内容より１段浅くして其の行で開く範囲を外側の範囲の起点に揃える
	 * 空行は構造から決めた物だけを断片として固定し，ファイルの先頭・末尾と連続する空行を作らない
	 * 計算量：解決済の要素の数 N と出力の長さ M に対し O(N + M)
	 * @param IR 全ての選択を解決した中間表現
	 * @return 変更不能なコードの出力
	 * @throws FormatLimitExceeded 出力が大きさの上限を超える場合
	 */
	LaidOutCode CodeLayout::Materialize(const BrokenIR &IR) {
		// 出力と追跡状態の準備
		LaidOutCode Code;
		const SyntaxModel &Syntax = *IR.Expectation.NormalizedSyntax;
		std::string Text;
		std::vector<CodeFragment> &Fragments = Code.Fragments;
		std::vector<NodeSpan> &Spans = Code.Anchors.Nodes;
		Code.Anchors.Syntax = IR.Expectation.NormalizedSyntax;
		// 錨の節点は正規化後の節点に限る為，全ての節点の範囲を先に用意する（配置の段は節点番号で範囲を引く）
		Spans.resize(Syntax.NodeCount());
		// 開いて居る深さの範囲毎の，範囲を開いた行の断片の番号と，範囲を開いた時の断片の数
		std::vector<uint32_t> &FragmentTabs = Code.Anchors.FragmentTabs, ScopeLines, ScopeStarts;
		std::vector<uint8_t> &VerbatimBreaks = Code.Anchors.VerbatimBreaks;
		// 内容を出した後に閉じた最も外側の範囲を開いた行（閉じの行は其の深さへ戻す）
		std::optional<uint32_t> ClosedLine;
		// 此の行で閉じた範囲の起点（閉じは閉じの字句の行に属する為，内容の前に閉じた物は次の内容の行へ送る）と，其れを此の行で開く範囲が引き継いだ数，此の行で最後に開いた範囲の起点
		std::vector<uint32_t> LineClosed, UnplacedClosed;
		size_t Inherited = 0;
		std::optional<uint32_t> LineOrigin;
		// 現在の行を開いたか，節見出で始まる行か，其の行頭，前の行との間に空行を置くか
		bool IsLineOpen = false, IsHeaderLine = false, IsBlankPending = false;
		uint32_t LineStart = 0;
		// 内容の前に始まった錨と，最後に出した内容の終わり
		std::vector<NodeId> PendingBegins;
		uint32_t LastFragment = 0, LastEnd = 0;
		NodeId LastEndedAnchor = NoNode;
		// 節点の出力上の範囲
		const auto SpanOf = [&Spans](const NodeId Node) -> NodeSpan & {
			// 節点の範囲の返戻
			return Spans.at(Node);
		};
		// 行の開始（前の行の後の改行・空行，範囲の深さのタブ）
		const auto OpenLine = [&]() -> void {
			const bool HasPreviousLine = !Fragments.empty();
			// 前の行との改行と構造上の空行の出力
			if(HasPreviousLine) Text += '\n';
			if(IsBlankPending && HasPreviousLine) {
				const uint32_t Position = static_cast<uint32_t>(Fragments.size());
				for(uint32_t &Origin : ScopeLines) if(Origin >= Position) ++Origin;
				for(uint32_t &Start : ScopeStarts) if(Start >= Position) ++Start;
				if(LineOrigin && *LineOrigin >= Position) ++*LineOrigin;
				const uint32_t Blank = static_cast<uint32_t>(Text.size());
				Fragments.push_back({ Blank, Blank, true, {} });
				FragmentTabs.push_back(0);
				VerbatimBreaks.push_back(0);
				Text += '\n';
			}
			// 新しい行の開始位置の確定
			IsBlankPending = false;
			LineStart = static_cast<uint32_t>(Text.size());
			// 範囲を閉じた直後の行は開いた行の深さ，其の他は前の行で開いた範囲の数（同じ行で開いた範囲は１段と数える）
			const uint32_t Current = static_cast<uint32_t>(Fragments.size());
			uint32_t Depth = 0;
			if(ClosedLine && *ClosedLine < Current) Depth = FragmentTabs[*ClosedLine];
			else for(size_t Index = 0; Index < ScopeLines.size() && ScopeLines[Index] < Current; ++Index) {
				if(!Index || ScopeLines[Index] != ScopeLines[Index - 1]) ++Depth;
			}
			// 節見出で始まる行の１段浅い深さ（整形仕様 6.5）
			IsHeaderLine = std::any_of(
				PendingBegins.begin(),
				PendingBegins.end(),
				[&Syntax](const NodeId Node) -> bool {
					// 名前付の節見出の節点かの返戻
					return Syntax.IsNamed(Node) && Syntax.Is(Node, Role::SectionHeader);
				}
			);
			if(IsHeaderLine && Depth) --Depth;
			Text.append(Depth, '\t');
			FragmentTabs.push_back(Depth);
			IsLineOpen = true;
		};
		// 行の終了
		const auto CloseLine = [&]() -> void {
			// 開いた行の無い場合の終了
			if(!IsLineOpen) return;
			// 行の断片の記録と一時状態の消去
			Fragments.push_back({ LineStart, static_cast<uint32_t>(Text.size()), false, {} });
			VerbatimBreaks.push_back(0);
			IsLineOpen = false;
			IsHeaderLine = false;
			LineClosed.clear();
			Inherited = 0;
			LineOrigin.reset();
		};
		// 内容の前に閉じた範囲の此の行への所属
		const auto PlaceClosed = [&LineClosed, &UnplacedClosed]() -> void {
			LineClosed.insert(LineClosed.end(), UnplacedClosed.begin(), UnplacedClosed.end());
			UnplacedClosed.clear();
		};
		// 内容の追記（行を開き，内容の前に始まった錨の始まりを確定する）
		const auto Append = [&](const std::string_view Content) -> void {
			if(!IsLineOpen) OpenLine();
			PlaceClosed();
			// 内容の前に始まった錨の位置の確定
			for(const NodeId Node : PendingBegins) {
				NodeSpan &Span = SpanOf(Node);
				Span.StartFragment = static_cast<uint32_t>(Fragments.size());
				Span.Start = static_cast<uint32_t>(Text.size());
				Span.IsPlaced = true;
			}
			PendingBegins.clear();
			ClosedLine.reset();
			// 区切の空白を除いた字句片の記録（配置の段が挿入位置の前後の繋がりを判ずる）
			if(const size_t First = Content.find_first_not_of(" \t\r"); First != std::string_view::npos) {
				const uint32_t Start = static_cast<uint32_t>(Text.size());
				Code.Anchors.Pieces.push_back(
					{ Start + static_cast<uint32_t>(First), Start + static_cast<uint32_t>(Content.find_last_not_of(" \t\r") + 1), 0 }
				);
			}
			// 内容の追加と末尾位置の更新
			Text += Content;
			LastFragment = static_cast<uint32_t>(Fragments.size());
			LastEnd = static_cast<uint32_t>(Text.size());
			// 再入力と同じ上限の超過時の見送（整形仕様 9.1）
			if(Text.size() > MaxInputBytes) throw FormatLimitExceeded("output exceeds size limit");
		};
		// 錨の終わり（内容を出さなかった錨は出力に現れない）
		const auto EndAnchor = [&](const NodeId Node) -> void {
			if(
				const std::vector<NodeId>::iterator Pending = std::find(PendingBegins.begin(), PendingBegins.end(), Node);
				Pending != PendingBegins.end()
			) {
				PendingBegins.erase(Pending);
				// 内容を出さなかった錨の終了
				return;
			}
			// 内容を持つ錨の終端位置の記録
			SpanOf(Node).EndFragment = LastFragment;
			SpanOf(Node).End = LastEnd;
		};
		// 要素の出した字句片の最初へ先頭の形，最後へ末尾の形の付与
		const auto MarkShapes = [&Code, &Syntax](const ResolvedItem &Item, const size_t PiecesBefore) -> void {
			std::vector<CodePiece> &Pieces = Code.Anchors.Pieces;
			// 字句片を出さなかった要素の終了
			if(Pieces.size() == PiecesBefore) return;
			const uint16_t Shapes = ShapesOf(Syntax, Item);
			Pieces[PiecesBefore].Shapes |= Shapes & CodePiece::Leads;
			Pieces.back().Shapes |= Shapes & CodePiece::Tails;
		};
		const std::unordered_set<NodeId> Multiline = MultilineNodes(IR);
		// 解決済の各要素の断片と位置への反映
		for(const ResolvedItem &Item : IR.Items) switch(Item.Kind) {
		case ResolvedKind::Text:
			{
				// 錨を兼ねる字句は其の字面を節点の出力とする
				if(Item.Node != NoNode) PendingBegins.push_back(Item.Node);
				const size_t PiecesBefore = Code.Anchors.Pieces.size();
				Append(IR.TextOf(Item));
				MarkShapes(Item, PiecesBefore);
				if(Item.Node != NoNode) EndAnchor(Item.Node);
				LastEndedAnchor = Item.Node;
				break;
			}
		case ResolvedKind::Separator:
			// 文法がコメントと読む区切記法は空行の照合基準で除く（整形仕様 8.1）
			Append(IR.TextOf(Item));
			Code.Separators.emplace_back(static_cast<uint32_t>(Text.size() - Item.Text.Size), static_cast<uint32_t>(Text.size()));
			break;
		case ResolvedKind::Verbatim:
			{
				// 改行を含む逐語は続きの行へインデントを付けない
				const std::string_view Content = IR.TextOf(Item);
				if(Item.Node != NoNode) PendingBegins.push_back(Item.Node);
				const size_t PiecesBefore = Code.Anchors.Pieces.size();
				for(size_t Cursor = 0;;) {
					const size_t Newline = Content.find('\n', Cursor);
					Append(Content.substr(Cursor, Newline == std::string_view::npos ? std::string_view::npos : Newline - Cursor));
					if(Newline == std::string_view::npos) break;
					CloseLine();
					// 逐語の頭の改行は前の行を終える為，行末は逐語の外で次の行の前だけが逐語の中
					VerbatimBreaks.back() = AnchorPositionMap::NextInVerbatim | (Cursor || Newline ? AnchorPositionMap::EndsInVerbatim : 0);
					Text += '\n';
					LineStart = static_cast<uint32_t>(Text.size());
					FragmentTabs.push_back(0);
					IsLineOpen = true;
					Cursor = Newline + 1;
				}
				MarkShapes(Item, PiecesBefore);
				if(Item.Node != NoNode) EndAnchor(Item.Node);
				break;
			}
		case ResolvedKind::OwnedRegion:
			{
				// 所有区画の行（空行は構造の空行，逐語の中から始まる行は字下げを付けない続きの行，其の他は此の深さの行，整形仕様 4.4）
				const OwnedRegion &Region = *Syntax.RegionOf(Item.Node);
				const std::string_view Content = Region.CodeText;
				CloseLine();
				const uint32_t FirstFragment = static_cast<uint32_t>(Fragments.size());
				PendingBegins.push_back(Item.Node);
				uint32_t Line = 0;
				for(size_t Cursor = 0; Cursor <= Content.size(); ++Line) {
					const size_t End = std::min(Content.find('\n', Cursor), Content.size());
					const std::string_view Row = Content.substr(Cursor, End - Cursor);
					CloseLine();
					if(Line && std::ranges::binary_search(Region.CodeVerbatimLines, Line)) {
						// 逐語の中の改行で前の行を終えた続きの行
						VerbatimBreaks.back() = AnchorPositionMap::EndsInVerbatim | AnchorPositionMap::NextInVerbatim;
						Text += '\n';
						LineStart = static_cast<uint32_t>(Text.size());
						FragmentTabs.push_back(0);
						IsLineOpen = true;
						Append(Row);
					} else if(Row.empty()) IsBlankPending = true;
					else Append(Row);
					Cursor = End + 1;
				}
				CloseLine();
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
			IsBlankPending = IsBlankPending || Item.IsForced || Multiline.contains(Item.Before) || Multiline.contains(Item.After);
			break;
		case ResolvedKind::IndentBegin:
			{
				// 節見出の行は外側の範囲の起点，閉じた範囲の後は其の起点，其の他は此の行で先に開いた範囲の起点か此の行
				PlaceClosed();
				uint32_t Origin = static_cast<uint32_t>(Fragments.size());
				if(IsHeaderLine && !ScopeLines.empty()) Origin = ScopeLines.back();
				else if(Inherited < LineClosed.size()) Origin = LineClosed[Inherited++];
				else if(LineOrigin) Origin = *LineOrigin;
				LineOrigin = Origin;
				ScopeLines.push_back(Origin);
				ScopeStarts.push_back(static_cast<uint32_t>(Fragments.size()));
				break;
			}
		case ResolvedKind::IndentEnd:
			// 行を跨がなかった範囲（平坦に選んだ群の範囲）は深さにも閉じの行にも関わらない
			if(Syntax.Is(LastEndedAnchor, Role::IndentedBody)) {
				ClosedLine.reset();
				UnplacedClosed.clear();
			} else if(ScopeStarts.back() != Fragments.size()) {
				ClosedLine = ScopeLines.back();
				UnplacedClosed.push_back(ScopeLines.back());
			}
			ScopeLines.pop_back();
			ScopeStarts.pop_back();
			LastEndedAnchor = NoNode;
			break;
		case ResolvedKind::AnchorBegin:
			PendingBegins.push_back(Item.Node);
			break;
		case ResolvedKind::AnchorEnd:
			EndAnchor(Item.Node);
			LastEndedAnchor = Item.Node;
			break;
		}
		CloseLine();
		// 各断片へ其の行の字面を渡し，完成したコード全体の字面は保持しない
		for(CodeFragment &Fragment : Fragments) Fragment.Text = Text.substr(Fragment.Start, Fragment.End - Fragment.Start);
		// 逐語の末尾データは其のバイト列で終え，其の他のコードだけ終端契約の改行を補う
		const std::span<const NodeId> RootChildren = Syntax.ChildrenOf(Syntax.GetRoot());
		const bool HasTrailingData = !RootChildren.empty() && Syntax.Is(RootChildren.back(), Role::TrailingDataSection);
		Code.Ending.EndsWithNewline =
		!HasTrailingData && !Fragments.empty() && (Fragments.back().End != Fragments.back().Start || !Text.ends_with('\n'));
		// 置ける零幅の位置だけの配置枠の表（行単位は空行と差込禁止区画に接する物を除き，文中は節点の出力の端，整形仕様 3.3）
		CommentSlotTable &Slots = Code.Slots;
		Slots.Owner = Code.Identity;
		Slots.LineKinds.assign(Fragments.size(), 0);
		for(uint32_t Fragment = 0; Fragment < Fragments.size(); ++Fragment) {
			if(Fragments[Fragment].IsBlank) continue;
			// 前の断片の終わりが逐語の中の改行なら行の前は逐語の中，此の断片の終わりが逐語の中の改行なら行末と行の後は逐語の中
			if(!Fragment || !(VerbatimBreaks[Fragment - 1] & AnchorPositionMap::NextInVerbatim)) {
				Slots.LineKinds[Fragment] |= 1 << static_cast<uint8_t>(CommentSlotKind::BeforeLine);
			}
			if(!(VerbatimBreaks[Fragment] & AnchorPositionMap::EndsInVerbatim)) {
				Slots.LineKinds[Fragment] |=
				1 << static_cast<uint8_t>(CommentSlotKind::LineEnd) | 1 << static_cast<uint8_t>(CommentSlotKind::OwnLine);
			}
		}
		// 節点の出力端から文中の配置枠の収集
		for(const NodeSpan &Span : Spans) if(Span.IsPlaced) {
			if(!Fragments[Span.StartFragment].IsBlank) Slots.InlineSlots.emplace_back(Span.Start, Span.StartFragment);
			if(!Fragments[Span.EndFragment].IsBlank) Slots.InlineSlots.emplace_back(Span.End, Span.EndFragment);
		}
		// 文中の配置枠の重複除去
		std::sort(Slots.InlineSlots.begin(), Slots.InlineSlots.end());
		Slots.InlineSlots.erase(std::unique(Slots.InlineSlots.begin(), Slots.InlineSlots.end()), Slots.InlineSlots.end());
		// 変更不能なコードの出力の返戻
		return Code;
	}
} // namespace Core
