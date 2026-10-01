#include "Canonicalizer.hpp"
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 節点の錨で包む関数
	 * @param Node 錨の節点
	 * @param Content 節点の出力の要素
	 * @param IR 構築中の中間表現
	 * @return 錨の要素
	 */
	FormatItemId Canonicalizer::Anchored(const NodeId Node, const FormatItemId Content, FormatIR &IR) {
		const FormatItemId Mark = IR.Parent(FormatItemKind::AnchorMark, std::span<const FormatItemId>(&Content, 1));
		IR.Items[Mark].Node = Node;
		// 錨の要素の返戻
		return Mark;
	}

	/**
	 * 文書の表示境界に従う組立関数
	 * @param State 構築中の状態
	 * @param Id 文書又は要素
	 * @return 本文と表示される空白を保った要素
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildMarkup(Builder &State, const NodeId Id) {
		// 子の展開と意味上の空白から親の展開を決める
		const SyntaxModel &Syntax = State.Syntax;
		const SyntaxNode &Node = Syntax.GetNode(Id);
		const bool IsRoot = Node.Parent == NoNode;
		bool ShouldExpand = IsRoot, HasBoundary = false, HasPreserved = false;
		uint32_t Elements = 0;
		for(const NodeId Child : Node.Children) if(Syntax.Is(Child, Role::MarkupContainer)) {
			++Elements;
			ShouldExpand = ShouldExpand || (co_await GetStructured(State, Child))->HasBreak;
		}
		for(const SemanticBoundary &Boundary : Node.Facts.Boundaries) {
			HasPreserved = HasPreserved || Boundary.Kind == BoundaryKind::Preserved;
			if(
				Boundary.Before > 0 && Boundary.Before < Node.Children.size() &&
				!Syntax.Is(Node.Children[Boundary.Before - 1], Role::MarkupOpen) &&
				!Syntax.Is(Node.Children[Boundary.Before], Role::MarkupClose)
			) HasBoundary = HasBoundary || Boundary.Kind == BoundaryKind::Spaced || Boundary.Kind == BoundaryKind::Separated;
		}
		ShouldExpand = !HasPreserved && (ShouldExpand || Elements > 1 && HasBoundary);
		// 閉じタグの前で終える深さの範囲の位置の取得
		Emitter Out(State.IR);
		bool IsScope = false;
		uint32_t ScopeEnd = static_cast<uint32_t>(Node.Children.size());
		for(uint32_t Index = 0; Index < Node.Children.size(); ++Index) if(Syntax.Is(Node.Children[Index], Role::MarkupClose)) {
			ScopeEnd = Index;
			while(ScopeEnd > 0 && Syntax.Is(Node.Children[ScopeEnd - 1], Role::CommentNode)) --ScopeEnd;
			break;
		}
		for(uint32_t Index = 0; Index <= Node.Children.size(); ++Index) {
			const NodeId Child = Index < Node.Children.size() ? Node.Children[Index] : NoNode;
			// 閉じタグを錨とする直前のコメントはタグと同じ深さへ置く
			if(Index == ScopeEnd && IsScope) {
				Out.CloseScope();
				IsScope = false;
			}
			// 表示境界に応じた原文の間隙と改行の配置
			if(const SemanticBoundary *const Boundary = Node.Facts.BoundaryBefore(Index)) {
				if(Boundary->Kind == BoundaryKind::Preserved) {
					Built Gap;
					Gap.Item = State.IR.Leaf(Boundary->Gap, FormatItemKind::Verbatim);
					Gap.Head = FindEdge(State.IR, Gap.Item, true);
					Gap.Tail = Gap.Head;
					Gap.HasBreak = Boundary->Gap.find('\n') != std::string::npos;
					Out.Append(std::move(Gap));
				} else if(ShouldExpand && (Boundary->Kind == BoundaryKind::Separated || Boundary->Kind == BoundaryKind::Spaced)) Out.Break();
				else if(Boundary->Kind == BoundaryKind::Spaced || Boundary->Kind == BoundaryKind::RequiredSpace) Out.Space();
			}
			// 子又は所有区画の配置
			if(Child != NoNode) {
				if(const OwnedRegion *const Owned = Syntax.RegionOf(Child); Owned && !Owned->CodeText.empty()) {
					// 所有区画を開始・終了タグとは別の行へ１段深く置く（区画の行は配置の段が此の深さへ置き，親の字句にしない，整形仕様 4.4）
					Emitter Code(State.IR);
					Code.OpenScope();
					Code.Break();
					FormatItem Region;
					Region.Kind = FormatItemKind::OwnedRegion;
					Region.Node = Child;
					Built Placed;
					Placed.Item = State.IR.Add(Region);
					// 区画は行を跨ぎ，後の内容を別の行へ置く端の要素
					Placed.Head = { Placed.Item, Child, '\0', '\0', false, true, false };
					Placed.Tail = Placed.Head;
					Placed.HasBreak = true;
					Code.Append(std::move(Placed));
					Code.CloseScope();
					Code.Break();
					Out.Append(Code.Finish(Child, State.NextGroup++));
				} else if(!Owned) Out.Append(co_await TakeStructured(State, Child));
			}
			if(!IsRoot && Syntax.Is(Child, Role::MarkupOpen) && ShouldExpand) {
				Out.OpenScope();
				IsScope = true;
			}
		}
		if(IsScope) Out.CloseScope();
		Built Result = Out.Finish(Id, State.NextGroup++);
		Result.HasBreak = ShouldExpand;
		// 文書の要素の返戻
		co_return Result;
	}

	/**
	 * 語で区切る節の組立関数
	 * @param State 構築中の状態
	 * @param Id 節
	 * @return 見出と本体を組み立てた要素
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildKeywordSection(Builder &State, const NodeId Id) {
		// 節の展開要因と条件の区切りの準備
		const SyntaxModel &Syntax = State.Syntax;
		Emitter Out(State.IR);
		const bool IsTransparent = Syntax.Is(Id, Role::TransparentSection), IsConditional = Syntax.Is(Id, Role::ConditionSection);
		const bool IsForced = Syntax.Is(Id, Role::ExceptionSection) || Syntax.Is(Id, Role::FinalSection);
		// 条件と本体を分ける語の前に空白を置いた字面と，後にも空白を置いた字面
		const std::string Then = std::string(" ").append(Syntax.Rules().Spelling(Word::ConditionThen)), SpacedThen = Then + " ";
		bool IsMulti = IsForced || !IsConditional && BodyStatementCount(Syntax, Id, 2) > 1;
		for(const NodeId Child : Syntax.ChildrenOf(Id)) {
			if(Syntax.Is(Child, Role::StatementSequence)) IsMulti = IsMulti || BodyStatementCount(Syntax, Child, 2) > 1;
			if(Syntax.IsNamed(Child) && !IsHeaderField(Syntax, Child)) {
				IsMulti = IsMulti || (co_await GetStructured(State, Child))->HasBreak;
			}
		}
		// 条件分岐の一節が複数文なら，同じ分岐列の空でない節を展開（祖先の節の判定は控えを使い，節毎に分岐列を辿り直さない）
		std::vector<NodeId> Chain;
		bool IsChainMulti = false;
		for(
			NodeId Up = Id;
			Syntax.Is(Up, Role::ConditionSection) || Syntax.Is(Up, Role::TransparentSection);
			Up = Syntax.ParentOf(Up)
		) {
			if(const uint8_t Known = State.Bits[Up]; Known & ChainMultiComputedBit) {
				IsChainMulti = Known & ChainMultiBit;
				break;
			}
			Chain.push_back(Up);
		}
		for(std::vector<NodeId>::const_reverse_iterator Up = Chain.rbegin(); Up != Chain.rend(); ++Up) {
			for(const NodeId Part : Syntax.ChildrenOf(*Up)) {
				if(Syntax.Is(Part, Role::StatementSequence)) IsChainMulti = IsChainMulti || BodyStatementCount(Syntax, Part, 2) > 1;
			}
			State.Bits[*Up] |= ChainMultiComputedBit | (IsChainMulti ? ChainMultiBit : 0);
		}
		IsMulti = IsMulti || IsChainMulti;
		// 見出と本体の順次配置
		bool HasBody = false, IsScope = false;
		uint32_t OpenHeredocs = 0;
		for(const NodeId Child : Syntax.ChildrenOf(Id)) {
			if(!Syntax.IsNamed(Child)) {
				if(IsTransparent || Syntax.IsTokenRole(Child, Role::StatementTerminatorToken)) continue;
				if(!Out.IsEmpty() && !Syntax.IsTokenRole(Child, Role::CommaToken)) Out.Space();
				Out.Append(BuildLeaf(Syntax, Child, State.IR));
				continue;
			}
			if(!IsTransparent && Syntax.Is(Child, Role::KeywordSection) && !Syntax.Is(Child, Role::StatementSequence)) {
				if(IsScope) {
					Out.CloseScope();
					IsScope = false;
				}
				if(const bool IsJoined = IsConditional && !HasBody && Syntax.Is(Child, Role::ConditionSection); IsJoined) {
					Out.Text(Syntax, Word::ConditionThen, true, true);
				} else {
					if(IsConditional && !HasBody) Out.Soft(Then);
					if(IsMulti) Out.Break();
					else Out.Soft(" ");
				}
				Out.Append(co_await TakeStructured(State, Child));
				HasBody = true;
				continue;
			}
			if(Syntax.Is(Child, Role::DeferredLiteralBody)) {
				co_await AppendHeredocBody(State, Child, Out);
				TrackOpenHeredocs(State, Child, OpenHeredocs);
				continue;
			}
			const bool IsBody = IsTransparent || !IsHeaderField(Syntax, Child);
			// ヒアドキュメントの本文を控えた開始行の続きは折らない（行分割仕様 2.3）
			Built Piece = OpenHeredocs ?
			co_await BuildSealedFlat(State, Child) :
			IsBody ? co_await TakeStructured(State, Child) : co_await TakeFlat(State, Child);
			if(Piece.Head.Item == NoItem) continue;
			if(IsBody && !IsTransparent) {
				if(!IsScope) {
					Out.OpenScope();
					IsScope = true;
				}
				if(IsMulti) {
					if(IsConditional && EndsWithOpenRange(Syntax, Syntax.FieldChild(Id, ChildRole::Pattern))) {
						Out.Text(Syntax, Word::ConditionThen, true);
					}
					Out.Break();
				} else Out.Soft(IsConditional ? SpacedThen : " ");
			} else if(!Out.IsEmpty()) {
				if(IsTransparent && OpenHeredocs) AppendHeredocPendingSeparator(Syntax, Out);
				else if(IsTransparent && IsMulti) Out.Break();
				else Out.Space();
			}
			AppendChild(State, Child, std::move(Piece), Out);
			HasBody = HasBody || IsBody;
			TrackOpenHeredocs(State, Child, OpenHeredocs);
		}
		if(IsScope) Out.CloseScope();
		if(IsConditional && !HasBody && !Syntax.Is(Id, Role::PatternSection)) Out.Text(Syntax, Word::ConditionThen, true);
		Built Result = Out.Finish(Id, NoGroup);
		Result.HasBreak = Result.HasBreak || IsForced;
		// 節の要素の返戻
		co_return Result;
	}

	/**
	 * 語で閉じる本体の組立関数
	 * @param State 構築中の状態
	 * @param Id 本体の所有者
	 * @return 構造と文数で配置した要素
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildKeywordBlock(Builder &State, const NodeId Id) {
		// 本体を持つ子と後続節は独立した配置範囲として組み立てる
		const SyntaxModel &Syntax = State.Syntax;
		const bool IsForced = Syntax.Is(Id, Role::ExpandedBody), IsPattern = Syntax.Is(Id, Role::PatternBranch);
		// 条件と本体を分ける語の前に空白を置いた字面と，後にも空白を置いた字面，反復の本体を始める語の前に空白を置いた字面
		const std::string Then = std::string(" ").append(Syntax.Rules().Spelling(Word::ConditionThen)), SpacedThen = Then + " ";
		uint32_t PendingHeredocs = 0;
		TrackOpenHeredocs(State, Id, PendingHeredocs);
		if(Syntax.Is(Id, Role::TailValueFunction) && !Syntax.HasWordChild(Id, Word::BlockEnd)) {
			// 式本体の定義は既存の平坦形を保つ事の返戻
			co_return Built(*co_await GetFlat(State, Id));
		}
		// 子の展開状態と本体の配置状態の準備
		Emitter Out(State.IR);
		bool IsLastMulti = false, HasBody = false, HasExpandedSection = false, IsDirectScope = false, HasHeredocHead = false;
		for(const NodeId Child : Syntax.ChildrenOf(Id)) {
			if(Syntax.Is(Child, Role::KeywordSection) || Syntax.Is(Child, Role::DeferredLiteralBody)) {
				HasExpandedSection = HasExpandedSection || (co_await GetStructured(State, Child))->HasBreak;
			}
		}
		NodeId Previous = NoNode;
		// 見出と本体及び後続節の配置
		for(const NodeId Child : Syntax.ChildrenOf(Id)) {
			const bool IsNamed = Syntax.IsNamed(Child);
			// 見出の後送の本文は終端の語の行で見出を終える為，塊の種別に依らず本体の範囲を開き，本体を複数行にする
			if(Syntax.Is(Child, Role::DeferredLiteralBody)) {
				if(!IsDirectScope) {
					Out.OpenScope();
					IsDirectScope = true;
				}
				HasHeredocHead = !HasBody;
				co_await AppendHeredocBody(State, Child, Out);
				continue;
			}
			if(!IsNamed && Syntax.IsTokenRole(Child, Role::StatementTerminatorToken)) continue;
			if(
				IsDirectScope && (
					IsNamed && Syntax.Is(Child, Role::KeywordSection) && !Syntax.Is(Child, Role::StatementSequence) ||
					!IsNamed && (Syntax.IsWord(Child, Word::BlockEnd) || Syntax.IsTokenRole(Child, Role::ClosingBraceToken))
				)
			) {
				Out.CloseScope();
				IsDirectScope = false;
			}
			// 本体を閉じる語と其の前の改行の配置
			if(!IsNamed && (Syntax.IsWord(Child, Word::BlockEnd) || Syntax.IsTokenRole(Child, Role::ClosingBraceToken))) {
				if(!HasBody && Syntax.Is(Id, Role::ConditionSection)) Out.Text(Syntax, Word::ConditionThen, true);
				NodeId LastSection = Previous;
				for(
					NodeId Alternative = Syntax.FieldChild(LastSection, ChildRole::Alternative);
					Alternative != NoNode;
					Alternative = Syntax.FieldChild(Alternative, ChildRole::Alternative)
				) LastSection = Alternative;
				const bool IsEmptyTail = Syntax.Is(LastSection, Role::KeywordSection) && !BodyStatementCount(Syntax, LastSection, 1);
				if(
					!PendingHeredocs &&
					(IsLastMulti || IsPattern || HasExpandedSection && !IsEmptyTail || !HasBody && Syntax.NeedsSeparatorToClose(Id))
				) Out.Break();
				else if(IsEmptyTail) Out.Space();
				else if(!Out.IsEmpty()) Out.Soft(" ");
				Out.Append(BuildLeaf(Syntax, Child, State.IR));
				continue;
			}
			// 子の本体・後続節・見出への分類
			const bool IsSequence = Syntax.Is(Child, Role::StatementSequence);
			const bool IsSection = Syntax.Is(Child, Role::KeywordSection) && !IsSequence;
			const bool IsBody =
			IsSequence || IsNamed && !IsSection && !IsHeaderField(Syntax, Child) && Syntax.Is(Id, Role::ExplicitKeywordBody);
			Built Piece = IsNamed ? co_await TakeStructured(State, Child) : BuildLeaf(Syntax, Child, State.IR);
			if(Piece.Head.Item == NoItem && !Syntax.Is(Child, Role::TransparentSection)) continue;
			if(IsBody) {
				// 例外節を含む文列の独立した配置
				bool HasSections = false;
				if(IsSequence) for(const NodeId Part : Syntax.ChildrenOf(Child)) {
					HasSections = HasSections || Syntax.Is(Part, Role::ExceptionSection) || Syntax.Is(Part, Role::FinalSection);
				}
				if(HasSections) {
					Emitter Sequence(State.IR);
					bool IsScope = false;
					for(const NodeId Part : Syntax.ChildrenOf(Child)) {
						if(!Syntax.IsNamed(Part)) continue;
						if(const bool IsClause = Syntax.Is(Part, Role::KeywordSection); IsClause && IsScope) {
							Sequence.CloseScope();
							IsScope = false;
						} else if(!IsClause && !IsScope) {
							Sequence.OpenScope();
							IsScope = true;
						}
						Sequence.Break();
						AppendChild(State, Part, co_await TakeStructured(State, Part), Sequence);
					}
					if(IsScope) Sequence.CloseScope();
					Out.Append(Sequence.Finish(Child, NoGroup));
					HasBody = IsLastMulti = true;
					Previous = Child;
					continue;
				}
				// 通常の本体の段と改行の選択
				HasBody = true;
				IsLastMulti = IsForced || HasHeredocHead || BodyStatementCount(Syntax, Child, 2) > 1 || Piece.HasBreak ||
				HasExpandedSection && Syntax.Is(Id, Role::ConditionSection);
				if(Syntax.Is(Id, Role::ConditionSection) && IsLastMulti && EndsWithOpenRange(Syntax, Previous)) {
					Out.Text(Syntax, Word::ConditionThen, true);
				} else if(Syntax.Is(Child, Role::IterationSection) && !IsLastMulti) Out.Text(Syntax, Word::IterationDo, true);
				// 空の本体（反復の `do` だけの本体）は，閉じの語の前の境界だけを折れる所とし，折れた閉じの行頭へ空白を残さない
				const bool IsEmptyBody = Piece.Head.Item == NoItem && !Syntax.Is(Id, Role::ConditionSection);
				if(IsLastMulti) {
					if(!IsDirectScope) Out.OpenScope();
					IsDirectScope = !IsSequence;
					if(EdgeChar(Syntax, State.IR, Piece.Head, true) != '\n') Out.Break();
				} else {
					Out.OpenScope();
					if(!IsEmptyBody) Out.Soft(Syntax.Is(Id, Role::ConditionSection) ? SpacedThen : " ");
				}
				AppendChild(State, Child, std::move(Piece), Out);
				if(!IsDirectScope) Out.CloseScope();
				if(Syntax.Is(Child, Role::TransparentSection) && Syntax.HasWordChild(Child, Word::BlockEnd)) {
					if(IsLastMulti) Out.Break();
					else if(IsEmptyBody) Out.Soft(" ");
					else Out.Space();
					Out.Append(BuildLeaf(Syntax, Syntax.Child(Child, Syntax.ChildCount(Child) - 1), State.IR));
				}
			} else if(IsSection) {
				// 後続節の区切りと群の配置
				if(
					const bool IsJoined = !HasBody && Syntax.Is(Id, Role::ConditionSection) && Syntax.Is(Child, Role::ConditionSection);
					IsJoined
				) Out.Text(Syntax, Word::ConditionThen, true, true);
				else {
					if(!HasBody && Syntax.Is(Id, Role::ConditionSection) && !HasExpandedSection) Out.Soft(Then);
					if(
						IsLastMulti || IsPattern || HasExpandedSection || Syntax.Is(Child, Role::ExceptionSection) ||
						Syntax.Is(Child, Role::FinalSection)
					) Out.Break();
					else Out.Soft(" ");
				}
				HasBody = true;
				IsLastMulti = Piece.HasBreak;
				if((IsPattern || HasExpandedSection) && !Syntax.Is(Id, Role::ConditionSection)) {
					const FormatItemId Content = Piece.Item;
					Piece.Item = State.IR.Parent(FormatItemKind::Group, std::span<const FormatItemId>(&Content, 1));
					State.IR.Items[Piece.Item].Group = State.NextGroup++;
					State.IR.Items[Piece.Item].Node = Child;
				}
				AppendChild(State, Child, std::move(Piece), Out);
			} else {
				// 見出の残りの字句と子の配置
				if(Previous != NoNode && NeedsSpace(Syntax, Id, Previous, Child)) Out.Space();
				const bool IsHeader = IsNamed && Syntax.HasField(Child, ChildRole::Condition);
				if(IsHeader) Out.OpenScope();
				AppendChild(State, Child, std::move(Piece), Out);
				if(IsHeader) Out.CloseScope();
			}
			Previous = Child;
		}
		// 本体の要素の返戻
		co_return Out.Finish(Id, State.NextGroup++);
	}

	/**
	 * 最上位の組立関数（整形仕様 6.1）
	 * 最上位の名前付の子を行毎に置き，本体の外の地の文は区切りを置かず逐語の儘で本体の前後へ写す
	 * @param State 構築中の状態
	 * @param Root 根
	 * @return 根の要素
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildTopLevel(Builder &State, const NodeId Root) {
		// 根の境界と文の配置状態の準備
		const SyntaxModel &Syntax = State.Syntax;
		Emitter Out(State.IR);
		NodeId PrevChild = NoNode;
		if(const SemanticBoundary *const Head = PreservedAt(Syntax, Root, 0)) Out.Gap(*Head);
		uint32_t OpenHeredocs = 0;
		for(const NodeId Child : Syntax.ChildrenOf(Root)) {
			if(!Syntax.IsNamed(Child)) continue;
			// 本体の外の地の文は区切りを置かず逐語の儘で本体の前後へ写し，本体の配置の判断に加えない（整形仕様 4.4）
			if(Syntax.Is(Child, Role::OuterText)) {
				Out.Append(co_await TakeStatement(State, Child));
				continue;
			}
			if(Syntax.Is(Child, Role::DeferredLiteralBody)) {
				co_await AppendHeredocBody(State, Child, Out);
				TrackOpenHeredocs(State, Child, OpenHeredocs);
				PrevChild = Child;
				continue;
			}
			// ヒアドキュメントの本文を控えた開始行の続きは折らない（行分割仕様 2.3）
			Built Piece = OpenHeredocs ?
			co_await BuildSealedFlat(State, Child) :
			Syntax.Is(Child, Role::Preprocessor) ? co_await TakeStructured(State, Child) : co_await TakeStatement(State, Child);
			if(PrevChild != NoNode && !Out.IsEmpty() && !Out.IsAtLineStart()) {
				// 誤読された宣言の終端の `;` は独立した最上位の子として現れる為，改行で切らず直前へ密着
				const bool IsAttachSemicolon = Piece.Head.Item == Piece.Tail.Item &&
				EdgeIs(Syntax, State.IR, Piece.Head, Role::StatementTerminatorToken, GeneratedToken::StatementTerminator) && (
					Syntax.GetNode(PrevChild).Facts.Has(ConstraintKind::ContainsError) || Syntax.Rules().CanAttachTopLevelSemicolon || OpenHeredocs
				);
				// 最上位の誤り区画に接する境界は原文の間隙を保つ
				if(
					const SemanticBoundary *const Preserved = IsAttachSemicolon ? nullptr : ErrorGapBefore(Syntax, PrevChild, Child);
					Preserved
				) AppendPreservedGap(Syntax, PrevChild, *Preserved, Out);
				else if(AppendCodeBoundary(State, PrevChild, Child, Out)) {} else if(
					!IsAttachSemicolon && OpenHeredocs && !Syntax.Is(Child, Role::DeferredLiteralBody)
				) AppendHeredocPendingSeparator(Syntax, Out);
				else if(!IsAttachSemicolon) {
					if(JoinsAcrossNewline(Syntax, Out.Store(), Piece.Head)) Out.Text(Syntax, GeneratedToken::StatementTerminator);
					Out.Break();
				}
			}
			AppendChild(State, Child, std::move(Piece), Out);
			TrackOpenHeredocs(State, Child, OpenHeredocs);
			PrevChild = Child;
		}
		// 根の要素の返戻
		co_return Out.Finish(Root, NoGroup);
	}

	/**
	 * 中間表現の構築関数（整形仕様 6.1）
	 * 正規化後の構文だけを読み，原文・コメント・行幅を参照しない
	 * 最上位の名前付の子を行毎に置き，空行の規則を宣言の構造種別から記録する
	 * 計算量：節点数 V に対し O(V)
	 * @param NormalizedSyntax 正規化後の構文（以後変更せず検証完了迄共有する期待構文）
	 * @return 幅非依存の中間表現
	 */
	FormatIR Canonicalizer::Build(std::shared_ptr<const SyntaxModel> NormalizedSyntax) {
		// 中間表現の領域と構築状態の準備
		const SyntaxModel &Syntax = *NormalizedSyntax;
		FormatIR Result;
		// 要素の生成を正規化と同じ作業単位の上限へ計上する（整形仕様 9.2）
		Result.NormalizedSyntax = NormalizedSyntax;
		// 要素と子の並びは節点の数の３倍に概ね収まる為，伸長の度に古い領域を捨てる再確保を避けて予約する
		Result.Items.reserve(Syntax.NodeCount() * 3);
		Result.Links.reserve(Syntax.NodeCount() * 3);
		Builder State { Syntax, Result };
		if(Syntax.Rules().HasDeferredLiteralBodies) State.Heredocs = SummarizeHeredocs(Syntax);
		// 誤り区画を直接の子に持つ節点の表（誤り区画の兄弟の判定で親毎に子を走査し直さない，整形仕様 9.2）
		const NodeId Root = Syntax.GetRoot();
		if(Syntax.GetNode(Root).Facts.Has(ConstraintKind::ContainsError)) {
			State.ErrorParents.assign(Syntax.NodeCount(), 0);
			for(NodeId Id = 0; Id < Syntax.NodeCount(); ++Id) {
				if(Syntax.Is(Id, Role::Error)) if(const NodeId Parent = Syntax.ParentOf(Id); Parent != NoNode) State.ErrorParents[Parent] = 1;
			}
		}
		if(Syntax.GetNode(Root).Children.empty()) {
			Result.Root = BuildLeaf(Syntax, Root, Result).Item;
			// 子を持たない根（読めなかった区画）の返戻
			return Result;
		}
		if(Syntax.Is(Root, Role::MarkupContainer)) {
			Result.Root = BuildMarkup(State, Root).Run().Item;
			// 文書構造の返戻
			return Result;
		}
		// 最上位と型の成員の並びの空行の規則
		CollectBlankRules(State, Root, true);
		std::vector<NodeId> Stack { Root };
		while(!Stack.empty()) {
			const NodeId Node = Stack.back();
			Stack.pop_back();
			// 型の直下の本体は型の節点で集める
			if(
				Syntax.Is(Node, Role::ClassBodyContainer) && !Syntax.Is(Node, Role::AmbiguousBodyContainer) &&
				!Syntax.Is(Syntax.ParentOf(Node), Role::ClassLike)
			) CollectBlankRules(State, Node, false);
			if(Syntax.Is(Node, Role::ClassLike)) for(const NodeId Body : Syntax.ChildrenOf(Node)) {
				if(Syntax.Is(Body, Role::ClassBodyContainer) || Syntax.Is(Body, Role::IndentedBody)) CollectBlankRules(State, Body, false);
			}
			const std::span<const NodeId> Children = Syntax.ChildrenOf(Node);
			Stack.insert(Stack.end(), Children.begin(), Children.end());
		}
		MarkArrowValueBodies(State, Root);
		Result.Root = BuildTopLevel(State, Root).Run().Item;
		// 根の要素と期待構文の返戻
		return Result;
	}
} // namespace Core
