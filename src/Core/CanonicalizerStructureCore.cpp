#include "Canonicalizer.hpp"
#include "../Util/CharClass.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 地の文との境界の追加関数
	 * 開きに続く単独の文は同行へ置き，複数の文は文毎に改行する
	 * @param State 構築中の状態
	 * @param Previous 直前の子
	 * @param Next 次の子
	 * @param Out 出力
	 * @return 境界を処理した場合は true
	 */
	bool Canonicalizer::AppendCodeBoundary(Builder &State, const NodeId Previous, const NodeId Next, Emitter &Out) {
		const SyntaxModel &Syntax = State.Syntax;
		// 埋込コードの境界でない場合の返戻
		if(Previous == NoNode || Out.IsEmpty()) return false;
		// 地の文へ密着する境界の返戻
		if(Syntax.Is(Previous, Role::InlineText) && !Syntax.Is(Previous, Role::CodeBoundary)) return true;
		// 原稿の最初の開始タグ（本体の外の先頭の空白の後を含む）か
		const NodeId Before = Syntax.PrevSibling(Previous);
		const bool IsInitial = Syntax.Is(Previous, Role::CodeOpener) && (Before == NoNode || Syntax.Is(Before, Role::OuterText)) &&
		!Syntax.IsWord(Previous, Word::ValueOutputTag);
		if(Syntax.Is(Next, Role::CodeBoundary)) {
			if(!IsInitial && (Out.HasInlineCode() || Out.TailIs(Syntax, Role::ColonToken))) Out.Space();
			else if(!Out.IsAtLineStart()) Out.Break();
			// コードの閉じの前を処理した事の返戻
			return true;
		}
		if(Syntax.Is(Previous, Role::CodeBoundary) || Syntax.Is(Previous, Role::CodeOpener)) {
			if(
				const NodeId After = Syntax.NextSibling(Next);
				IsInitial || After != NoNode && Syntax.IsNamed(After) && !Syntax.Is(After, Role::CodeBoundary)
			) Out.Break();
			else Out.Space();
			// コードの開きの後を処理した事の返戻
			return true;
		}
		// 通常の境界の返戻
		return false;
	}

	/**
	 * 空行の規則の収集関数（整形仕様 6.5）
	 * 最上位と型の成員の並びで，構造種別の違い・複数行の宣言・裸の塊の後に空行を置く規則を，後の宣言（前置の注釈が有れば其の先頭）へ記録する
	 * 計算量：辿る子の総数 V に対し O(V)
	 * @param State 構築中の状態
	 * @param Container 成員の並びの容器
	 * @param IsRootCall 根の並びか（裸の波括弧の中の解析断片を読み飛ばす）
	 */
	void Canonicalizer::CollectBlankRules(Builder &State, const NodeId Container, const bool IsRootCall) {
		// 入れ子の宣言列を辿る作業枠の準備
		const SyntaxModel &Syntax = State.Syntax;
		struct Frame {
			NodeId Container;
			bool IsRootCall;
			std::vector<NodeId> Children {};
			size_t Next = 0;
			NodeId PendingClose = NoNode;
			bool IsReady = false;
			bool HasPrev = false;
			bool IsPendingBlock = false;
			bool IsAfterInlineOpen = false;
			bool IsPrevForced = false;
			uint32_t PrevBlankKey = 0;
			NodeId Prev = NoNode;
			NodeId FirstAttach = NoNode;
			uint32_t BraceDepth = 0;
		};
		std::vector<Frame> Stack;
		if(!Syntax.Is(Container, Role::MarkupContainer) && State.BlankContainers.insert(Container).second) {
			Stack.push_back({ Container, IsRootCall });
		}
		while(!Stack.empty()) {
			Frame &Current = Stack.back();
			// 列挙の宣言部を含む子の並びの取得
			if(!Current.IsReady) {
				Current.IsReady = true;
				for(const NodeId Child : Syntax.ChildrenOf(Current.Container)) {
					if(Syntax.Is(Child, Role::EnumBodyDeclarations)) {
						const std::span<const NodeId> Inner = Syntax.ChildrenOf(Child);
						Current.Children.insert(Current.Children.end(), Inner.begin(), Inner.end());
					} else Current.Children.push_back(Child);
				}
			}
			if(Current.PendingClose != NoNode) {
				// 条件付指令の終端の空行の記録
				const NodeId Child = Current.PendingClose;
				Current.PendingClose = NoNode;
				if(Syntax.Is(Child, Role::PreprocConditional)) if(const uint32_t Count = Syntax.ChildCount(Child); Count) {
					State.BlankBefore.insert_or_assign(Syntax.Child(Child, Count - 1), BlankRule{ NoNode, NoNode, true });
				}
			}
			// 完了した作業枠の除去と次の子の取得
			if(Current.Next == Current.Children.size()) {
				Stack.pop_back();
				continue;
			}
			const NodeId Child = Current.Children[Current.Next++];
			bool &HasPrev = Current.HasPrev, &IsPendingBlock = Current.IsPendingBlock, &IsAfterInlineOpen = Current.IsAfterInlineOpen;
			bool &IsPrevForced = Current.IsPrevForced;
			uint32_t &PrevBlankKey = Current.PrevBlankKey;
			NodeId &Prev = Current.Prev, &FirstAttach = Current.FirstAttach;
			uint32_t &BraceDepth = Current.BraceDepth;
			const bool IsRootCall = Current.IsRootCall;
			if(!Syntax.IsNamed(Child)) {
				// 裸の波括弧の中に在る解析断片は最上位の空行の対象から除く
				if(IsRootCall && Syntax.IsTokenRole(Child, Role::OpeningBraceToken)) ++BraceDepth;
				else if(IsRootCall && Syntax.IsTokenRole(Child, Role::ClosingBraceToken) && BraceDepth && !--BraceDepth) IsPendingBlock = true;
				continue;
			}
			// 前置の注釈と次の実体の項目は空行無で連ねる
			if(Syntax.Is(Child, Role::AttachPrefix)) {
				if(FirstAttach == NoNode) FirstAttach = Child;
				continue;
			}
			// 直前のプロパティに属する Kotlin のアクセサは空行無の続きとし，後の項目との間を複数行の後として扱う
			if(HasPrev && Syntax.Is(Child, Role::AttachedAccessor)) {
				IsPrevForced = true;
				continue;
			}
			if(
				IsRootCall && BraceDepth || Syntax.Is(Child, Role::ExpressionStatement) && Syntax.Text(Child).size() == 1 && (
					Syntax.IsTokenRole(Child, Role::StatementTerminatorToken) ||
					Syntax.IsTokenRole(Syntax.Child(Child, 0), Role::StatementTerminatorToken)
				) || Syntax.Is(Child, Role::Error) || Syntax.Is(Child, Role::OuterText)
			) continue;
			// 前後の宣言の構造から空行の規則を記録
			const uint32_t BlankKey = Syntax.GetNode(Child).Kind.Definition->BlankKey;
			if(
				const bool IsInlineTextEdge = Syntax.Is(Child, Role::InlineText) || Syntax.Is(Child, Role::CodeOpener) || IsAfterInlineOpen;
				HasPrev && !IsInlineTextEdge
			) {
				BlankRule Rule { Prev, Child, PrevBlankKey != BlankKey || IsPendingBlock || IsPrevForced };
				State.BlankBefore.insert_or_assign(FirstAttach == NoNode ? Child : FirstAttach, Rule);
			}
			FirstAttach = NoNode;
			IsPendingBlock = false;
			IsPrevForced = false;
			HasPrev = true;
			// 値を出力する `<?=` はファイルの先頭でも地の文の中の開始タグと同じく扱う
			IsAfterInlineOpen = Syntax.Is(Child, Role::EmbeddedOpeningBoundary) &&
			(!Syntax.Is(Child, Role::CodeOpeningTag) || Syntax.Is(Prev, Role::LiteralText) || Syntax.IsWord(Child, Word::ValueOutputTag));
			PrevBlankKey = BlankKey;
			Prev = Child;
			if(Syntax.Is(Child, Role::DirectiveBlankBoundary)) {
				// `#else` の最初の本体の文の前の空行
				if(Syntax.Is(Child, Role::PreprocElse)) if(const NodeId FirstBody = Syntax.NamedChild(Child, 0); FirstBody != NoNode) {
					State.BlankBefore.insert_or_assign(FirstBody, BlankRule{ NoNode, NoNode, true });
				}
				Current.PendingClose = Child;
				if(!Syntax.Is(Child, Role::MarkupContainer) && State.BlankContainers.insert(Child).second) Stack.push_back({ Child, false });
			}
		}
		// 終了
		return;
	}

	/**
	 * 誤り区画の閉じない括弧に依る深さの範囲の開閉関数（整形仕様 4.6, 6.5）
	 * 誤り区画の中で閉じない開きの括弧は後の兄弟へ跨る為，区画の前で其の数だけ深さの範囲を開き，閉じだけの区画は其の数だけ閉じる
	 * @param Syntax 構文モデル
	 * @param Child 追加する子
	 * @param Out 組立中の出力
	 */
	void Canonicalizer::AdjustErrorZoneScopes(const SyntaxModel &Syntax, const NodeId Child, Emitter &Out) {
		// 誤り区画でない子の場合の終了
		if(!Syntax.Is(Child, Role::Error)) return;
		// 区画の中の開きの括弧の数から閉じの括弧の数を引いた数
		int Balance = 0;
		std::vector<NodeId> Stack { Child };
		while(!Stack.empty()) {
			const NodeId Id = Stack.back();
			Stack.pop_back();
			const SyntaxNode &Node = Syntax.GetNode(Id);
			if(Node.Children.empty() && Node.Lexeme.size() == 1) {
				Balance += IsOpenBracketChar(Node.Lexeme.front()) ? 1 : IsCloseBracketChar(Node.Lexeme.front()) ? -1 : 0;
			}
			Stack.insert(Stack.end(), Node.Children.begin(), Node.Children.end());
		}
		for(; Balance > 0; --Balance) Out.OpenScope();
		for(; Balance < 0; ++Balance) Out.CloseScope();
		// 終了
		return;
	}

	/**
	 * 子の組立済の要素の追加関数
	 * 改行の後に置く子に空行の規則が記録されて居れば，其の規則を先に置く
	 * @param State 構築中の状態
	 * @param Child 子
	 * @param Piece 子の組立済の要素
	 * @param Out 組立中の出力
	 */
	void Canonicalizer::AppendChild(Builder &State, const NodeId Child, Built &&Piece, Emitter &Out) {
		AdjustErrorZoneScopes(State.Syntax, Child, Out);
		if(Out.IsAtLineStart()) if(
			const std::unordered_map<NodeId, BlankRule>::const_iterator Rule = State.BlankBefore.find(Child);
			Rule != State.BlankBefore.end()
		) Out.Blank(Rule->second.Before, Rule->second.After, Rule->second.IsForced);
		AppendLexicalSeparator(State.Syntax, Child, Piece.Head, Out);
		Out.Append(std::move(Piece));
		// 終了
		return;
	}

	/**
	 * 構造化の本体の組立関数（整形仕様 6.3）
	 * @param State 構築中の状態
	 * @param Id 節点
	 * @return 組立済の要素
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildStructuredCore(Builder &State, const NodeId Id) {
		// 特殊な区画と平坦形の選択
		const SyntaxModel &Syntax = State.Syntax;
		const SyntaxNode &Node = Syntax.GetNode(Id);
		// Ruby のヒアドキュメントの本体は終端の語が独自の行頭・行末を要する為，逐語に末尾の改行を加える
		if(Syntax.Is(Id, Role::DeferredLiteralBody)) {
			Emitter Out(State.IR);
			co_await AppendHeredocBody(State, Id, Out);
			// 本体の返戻
			co_return Out.Finish(NoNode, NoGroup);
		}
		// 字句の葉と字面を保つ区画の返戻
		if(Node.Children.empty()) co_return BuildLeaf(Syntax, Id, State.IR);
		// 原文の字句と間隙を写す誤り区画と，補間を持つ文字列の平坦形の返戻（整形仕様 4.4, 4.6）
		if(IsUnbreakable(State, Id)) co_return Built(*co_await GetFlat(State, Id));
		// 別の宣言へ分けれる文の並びのカンマで並べる宣言の返戻（行分割仕様 4.1）
		if(
			const std::optional<CommaDecl> Shape = Syntax.CommaDeclOf(Id);
			Shape && Shape->Lists.front().Items.size() > 1 && Syntax.Is(Node.Parent, Role::StatementHost)
		) co_return co_await BuildCommaDecl(State, Id);
		// 表示の境界を持つ文書の返戻
		if(Syntax.Is(Id, Role::MarkupContainer)) co_return co_await BuildMarkup(State, Id);
		// タグと属性の平坦形の返戻
		if(Syntax.Is(Id, Role::MarkupSyntax)) co_return Built(*co_await GetFlat(State, Id));
		const uint32_t ChildCount = static_cast<uint32_t>(Node.Children.size()), NamedCount = Syntax.NamedChildCount(Id);
		// Ruby の連結文字列の独立した文への誤読を避ける平坦形の返戻
		if(Syntax.Is(Id, Role::UnbrokenStringSequence)) co_return Built(*co_await GetFlat(State, Id));
		// 本体の展開要因の判定
		const bool HasStmtBlock = ContainsStmtBlock(State, Id);
		bool ShouldExpand = HasStmtBlock;
		if(HasStmtBlock && Syntax.Is(Id, Role::KeywordExpandedContainer)) {
			ShouldExpand = false;
			for(const NodeId Child : Node.Children) {
				ShouldExpand = ShouldExpand || Syntax.IsNamed(Child) && (co_await GetStructured(State, Child))->HasBreak;
			}
		}
		const bool IsParen = Syntax.Is(Id, Role::ValueParentheses);
		// 展開を要しない節点の平坦形の返戻
		if(
			!HasStmtBlock &&
			(Syntax.Is(Id, Role::ExpressionTag) || IsParen || Syntax.Is(Id, Role::TagElement) && RenderedContentCount(Syntax, Id) < 2)
		) co_return Built(*co_await GetFlat(State, Id));
		// 展開形の容器の返戻（Go の括弧式は自動のセミコロンに依る構文の崩壊を防ぐ為に汎用の経路で密着を保つ）
		if(
			ShouldExpand && (Syntax.Is(Id, Role::ExpandContainer) || Syntax.Is(Id, Role::ExpandedTypeParameters)) &&
			!(Syntax.Rules().HasAutomaticLineTerminator && IsParen)
		) co_return co_await BuildExpandedContainer(State, Id);
		// 暗黙の結果を持つ本体の子毎の配置
		if(Syntax.Is(Id, Role::ImplicitResultBody)) {
			Emitter Out(State.IR);
			uint32_t OpenHeredocs = 0;
			for(const NodeId Child : Node.Children) {
				if(!Syntax.IsNamed(Child)) continue;
				if(Syntax.Is(Child, Role::DeferredLiteralBody)) {
					co_await AppendHeredocBody(State, Child, Out);
					TrackOpenHeredocs(State, Child, OpenHeredocs);
					continue;
				}
				Built Piece = OpenHeredocs ? co_await BuildSealedFlat(State, Child) : co_await TakeStructured(State, Child);
				if(Piece.Head.Item == NoItem) continue;
				if(OpenHeredocs) AppendHeredocPendingSeparator(Syntax, Out);
				else if(!Out.IsEmpty() && !Out.IsAtLineStart()) Out.Break();
				AppendChild(State, Child, std::move(Piece), Out);
				TrackOpenHeredocs(State, Child, OpenHeredocs);
			}
			// 子毎に改行した要素の返戻
			co_return Out.Finish(Id, NoGroup);
		}
		// 丸括弧で指定を並べる import 宣言は１行１指定で組む
		const bool HasImportParenthesis = Syntax.Is(Id, Role::GroupedImport) && std::ranges::any_of(
			Node.Children,
			[&Syntax](const NodeId Child) -> bool {
				// 括弧で囲む指定の並びの子か，開き丸括弧の子かの返戻
				return Syntax.Is(Child, Role::ImportItems) || Syntax.IsTokenRole(Child, Role::OpeningParenToken);
			}
		);
		if(HasImportParenthesis) {
			Emitter Out(State.IR);
			for(const NodeId Child : Node.Children) {
				if(!Syntax.IsNamed(Child)) {
					if(Child == Node.Children.front()) Out.Append(BuildLeaf(Syntax, Child, State.IR));
					continue;
				}
				// 指定の並び（括弧の中）
				const bool IsList = Syntax.Is(Child, Role::ImportItems);
				Out.Space();
				Emitter List(State.IR);
				List.Text(Syntax, GeneratedToken::CallOpen);
				List.OpenScope();
				List.Break();
				// 並べる指定（括弧で囲む並びの中の指定か，括弧の外の指定）
				std::vector<NodeId> Specs;
				if(IsList) {
					for(const NodeId Spec : Syntax.ChildrenOf(Child)) if(Syntax.Is(Spec, Role::ImportItem)) Specs.push_back(Spec);
				} else if(Syntax.Is(Child, Role::ImportItem)) Specs.push_back(Child);
				for(const NodeId Spec : Specs) {
					Built Piece(*co_await GetFlat(State, Spec));
					if(Piece.Head.Item == NoItem) continue;
					AppendChild(State, Spec, std::move(Piece), List);
					List.Break();
				}
				List.CloseScope();
				List.Text(Syntax, GeneratedToken::CallClose);
				Out.Append(List.Finish(IsList ? Child : NoNode, NoGroup));
			}
			// import の一覧の返戻
			co_return Out.Finish(Id, NoGroup);
		}
		// 行の前処理指令は１論理行の儘で内部を折らない（整形仕様 4.2，行分割仕様 2.1）
		if(Syntax.Is(Id, Role::PreprocessorLine)) co_return co_await BuildSealedFlat(State, Id);
		// 前処理指令の条件の塊
		if(Syntax.Is(Id, Role::Preprocessor)) co_return co_await BuildPreprocBlock(State, Id);
		const bool IsThen = Syntax.Is(Id, Role::ThenSection), IsDo = Syntax.Is(Id, Role::IterationSection);
		const bool IsElse = Syntax.Is(Id, Role::Else), IsEnsure = Syntax.Is(Id, Role::EnsureSection);
		const bool IsRescue = Syntax.Is(Id, Role::RescueSection);
		// Ruby の節の返戻
		if(Syntax.Is(Id, Role::KeywordSection)) co_return co_await BuildKeywordSection(State, Id);
		// Ruby の塊の返戻
		if(Syntax.Is(Id, Role::KeywordBlock)) co_return co_await BuildKeywordBlock(State, Id);
		// 名前空間の子と区切りの配置
		if(Syntax.Is(Id, Role::NamespaceLike)) {
			Emitter Out(State.IR);
			for(const NodeId Child : Node.Children) {
				if(Syntax.IsNamed(Child)) {
					if(!Out.IsEmpty() && !Out.EndsWithBlank() && !Out.IsAtLineStart()) Out.Space();
					Out.Append(co_await TakeStructured(State, Child));
				} else if(const std::string &Token = Syntax.GetNode(Child).Lexeme; !Token.empty()) {
					// 文末の区切り（PHP の `namespace A\B;` 等）は直前の名前へ密着
					if(
						!Syntax.IsTokenRole(Child, Role::StatementTerminatorToken) && !Syntax.IsTokenRole(Child, Role::CommaToken) && !Out.IsEmpty() &&
						!Out.EndsWithBlank() && !Out.IsAtLineStart()
					) Out.Space();
					Out.Append(BuildLeaf(Syntax, Child, State.IR));
				}
			}
			// 名前空間の要素の返戻
			co_return Out.Finish(Id, NoGroup);
		}
		// 透過する親と字下げした本体の文の配置
		if(Syntax.Is(Id, Role::Transparent) || Syntax.Is(Id, Role::IndentedBody)) {
			Emitter Out(State.IR);
			bool IsAfterLabel = false;
			NodeId Previous = NoNode;
			for(const NodeId Child : Node.Children) {
				const bool IsNamed = Syntax.IsNamed(Child);
				// 文の区切りの `;` 以外の無名の子も文其の物
				if(!IsNamed && Syntax.IsTokenRole(Child, Role::StatementTerminatorToken)) continue;
				Built Piece = IsNamed ? co_await TakeStructured(State, Child) : BuildLeaf(Syntax, Child, State.IR);
				if(Piece.Head.Item == NoItem) continue;
				if(!Out.IsEmpty()) {
					// 誤り区画に接する境界は原文の間隙を保つ
					if(const SemanticBoundary *const Preserved = ErrorGapBefore(Syntax, Previous, Child)) {
						AppendPreservedGap(Syntax, Previous, *Preserved, Out);
					} else if(AppendCodeBoundary(State, Previous, Child, Out)) {} else if(IsAfterLabel) Out.Space();
					else if(JoinsAcrossNewline(Syntax, Out.Store(), Piece.Head)) {
						Out.Text(Syntax, GeneratedToken::StatementTerminator);
						Out.Break();
					} else Out.Break();
				}
				AppendChild(State, Child, std::move(Piece), Out);
				IsAfterLabel = Syntax.Is(Child, Role::PrefixLabel) || IsAfterLabel && Syntax.Is(Child, Role::TransparentLabelPrefix);
				Previous = Child;
			}
			// 透過する親の文を分けた要素の返戻
			co_return Out.Finish(Id, NoGroup);
		}
		// 継続括弧を選べる式の平坦形の返戻
		if(Syntax.Is(Id, Role::ContinuationExpression)) co_return Built(*co_await GetFlat(State, Id));
		if(ChildCount < 2) {
			// 同じ範囲を占める唯一の名前付の子の構造化の返戻
			if(NamedCount == 1) {
				Emitter Out(State.IR);
				Out.Append(co_await TakeStructured(State, Node.Children.front()));
				// 唯一の子で組み立てた要素の返戻
				co_return Out.Finish(Id, NoGroup);
			}
			// 構造の展開の要らない節点の平坦形の返戻
			co_return Built(*co_await GetFlat(State, Id));
		}
		// 名前付の子の無い節点の平坦形の返戻
		if(!NamedCount) co_return Built(*co_await GetFlat(State, Id));
		if(Syntax.Is(Id, Role::ClosureBlock)) {
			bool HasMultiBody = false;
			for(const NodeId Child : Node.Children) {
				HasMultiBody = HasMultiBody ||
				Syntax.IsNamed(Child) && Syntax.Is(Child, Role::AmbiguousBodyContainer) && BodyStatementCount(Syntax, Child, 2) > 1;
			}
			// 単一の本体の Ruby の塊の平坦形の返戻
			if(!HasMultiBody) co_return Built(*co_await GetFlat(State, Id));
		}
		// 汎用の構造化の返戻
		co_return co_await BuildGenericStructure(
			State,
			Id,
			HasStmtBlock,
			ShouldExpand,
			IsThen || IsDo || IsElse || IsEnsure || IsRescue
		);
	}

	/**
	 * 構造化の組立関数（整形仕様 6.3）
	 * 本体の文の数・宣言の構造・子の必須展開から幅に依らない改行を決め，其れ以外の子は平坦形で繋ぐ
	 * ヒアドキュメントの開始の後の改行で後続の字句が本文へ入る場合は平坦形にする
	 * @param State 構築中の状態
	 * @param Id 節点
	 * @return 組立済の要素
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildStructured(Builder &State, const NodeId Id) {
		Built Result = co_await BuildStructuredCore(State, Id);
		// 改行を含まない又は Ruby でない要素の返戻
		if(!Result.HasBreak || State.Syntax.Is(Id, Role::PatternBranch)) co_return Result;
		uint32_t Open = 0;
		TrackOpenHeredocs(State, Id, Open);
		// 本文を控えた開始が無い要素の返戻
		if(!Open) co_return Result;
		for(std::vector<NodeId> Stack { Id }; !Stack.empty();) {
			const NodeId Child = Stack.back();
			Stack.pop_back();
			// 照合の節を始める改行はヒアドキュメントの開始より前に在る事の返戻
			if(State.Syntax.Is(Child, Role::PatternBranch)) co_return Result;
			for(const NodeId Next : State.Syntax.ChildrenOf(Child)) Stack.push_back(Next);
		}
		// 開始の後で改行しない１行の平坦形の返戻
		co_return Built(*co_await GetFlat(State, Id));
	}
} // namespace Core
