#include "Canonicalizer.hpp"
#include "FormatIR.hpp"
#include "NodeId.hpp"
#include "SyntaxModel.hpp"
#include "SyntaxRole.hpp"
#include "../Util/LayoutTypes.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * コンストラクタ（文脈のフラグを既定値にする）
	 */
	Canonicalizer::NodeCtx::NodeCtx() noexcept : IsMemberSequence(false),
	HasStatementBody(false),
	HasAccessSection(false),
	HasKeywordClose(false),
	HasBlankRules(false),
	IsSpacingTokenKnown(false),
	HasSpacingTokenWork(false),
	HasPreceding(false),
	HasFollowing(false),
	IsLeadingParenType(false),
	HasBraceOpen(false),
	HasBraceClose(false),
	HasParenOpen(false),
	HasColon(false),
	HasDirectError(false),
	HasErrorChild(false),
	HasRenderedChild(false),
	HasElseChild(false),
	HasElseGrandchild(false),
	HasExceptionChild(false),
	ShouldWrap(false),
	IsAnnotation(false),
	IsTypeUnion(false),
	IsOptionalBody(false),
	HasFollowingClause(false),
	IsArrowVal(false) {
		// 終了
		return;
	}

	/**
	 * 所有者の空き列へフレームを戻す関数
	 * 計算量：平均 O(1)，追加の確保無
	 * @param Buffer 使い終えたフレーム
	 */
	void Canonicalizer::FramePool::Release(void *const Buffer) noexcept {
		Header *const Block = static_cast<Header *>(Buffer) - 1;
		Header *&First = *Block->FreeList;
		Block->Next = First;
		First = Block;
		// 終了
		return;
	}

	/**
	 * 同じサイズのフレームを再使用する確保関数
	 * 計算量：サイズの種類数 S と確保済のフレーム数 F に対し平均償却 O(1)，列の伸長時 O(F)，追加領域 O(S + F)
	 * @param Size 必要なフレームのバイト数
	 * @return 所有者の解放迄有効なフレーム
	 * @throws std::bad_alloc フレーム又は管理用のメモリを確保出来ない場合
	 * @throws std::length_error 管理用の列の要素数が上限を超える場合
	 */
	void *Canonicalizer::FramePool::Allocate(const size_t Size) {
		Header *&First = Free[Size];
		++Requests;
		if(First) {
			Header *const Block = First;
			First = Block->Next;
			// 同じサイズの解放済のフレームの返戻
			return Block + 1;
		}
		Header *const Block = ::new (::operator new(sizeof(Header) + Size)) Header { nullptr, &First };
		try {
			Blocks.push_back(Block);
		} catch(...) {
			::operator delete(Block);
			throw;
		}
		++Allocations;
		// 新規に確保したフレームの返戻
		return Block + 1;
	}

	/**
	 * デストラクタ（此の構築のフレームを全て解放する）
	 * 計算量：新規に確保したフレーム数 F に対し O(F)
	 */
	Canonicalizer::FramePool::~FramePool() {
		for(Header *const Block : Blocks) ::operator delete(Block);
		// 終了
		return;
	}

	/**
	 * 完了した組立を作業列から外す関数
	 * @tparam Promise
	 * @param Completed 完了した組立（待つ側が所有する）
	 */
	template<class Promise> void Canonicalizer::FinalPause::await_suspend(
		const std::coroutine_handle<Promise> Completed
	) const noexcept {
		Completed.promise().Work->pop_back();
		// 終了
		return;
	}

	/**
	 * 開始前の中断関数（作業列から再開する迄始めない）
	 * @tparam Val
	 * @return 常に中断するマーカー
	 */
	template<class Val> std::suspend_always Canonicalizer::PendingPromise<Val>::initial_suspend() const noexcept {
		// 中断の返戻
		return {};
	}

	/**
	 * 完了後の中断関数（待つ側が結果を受け取る迄状態を残す）
	 * @tparam Val
	 * @return 常に中断するマーカー
	 */
	template<class Val> Canonicalizer::FinalPause Canonicalizer::PendingPromise<Val>::final_suspend() const noexcept {
		// 完了した組立を除いて中断するマーカーの返戻
		return {};
	}

	/**
	 * 結果の格納関数
	 * @tparam Val
	 * @param Completed 組立の結果
	 */
	template<class Val> void Canonicalizer::PendingPromise<Val>::return_value(Val Completed) {
		// 結果の格納
		Result.emplace(std::move(Completed));
		// 終了
		return;
	}

	/**
	 * 送出した例外の格納関数（待つ側で送出し直す）
	 * @tparam Val
	 */
	template<class Val> void Canonicalizer::PendingPromise<Val>::unhandled_exception() noexcept {
		// 送出した例外の格納
		Error = std::current_exception();
		// 終了
		return;
	}

	/**
	 * 破棄済のフレームを作業領域へ戻す関数
	 * 計算量：平均 O(1)
	 * @tparam Val
	 * @param Buffer 破棄したフレーム（戻す先はフレームのヘッダに記録済）
	 */
	template<class Val> void Canonicalizer::PendingPromise<Val>::operator delete(void *const Buffer) noexcept {
		FramePool::Release(Buffer);
		// 終了
		return;
	}

	/**
	 * コンストラクタ（組立の状態を所有する）
	 * @tparam Val
	 * @param Started 組立の状態
	 */
	template<class Val> Canonicalizer::Pending<Val>::Pending(const std::coroutine_handle<PendingPromise<Val>> Started) : Handle(
		Started
	) {
		// 終了
		return;
	}

	/**
	 * 待つ前に完了して居るかの判定関数
	 * @tparam Val
	 * @return 常に false（組立は作業列から再開する）
	 */
	template<class Val> bool Canonicalizer::Pending<Val>::await_ready() const noexcept {
		// 未完了の返戻
		return false;
	}

	/**
	 * 待つ側を中断して此の組立を作業列へ積む関数
	 * 計算量：作業列の長さ K に対し償却 O(1)，領域の伸長時 O(K)
	 * @tparam Val
	 * @param Waiting 待つ側の組立（作業列で此の組立の下に残り，完了後に再開する）
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	template<class Val> void Canonicalizer::Pending<Val>::await_suspend(
		[[maybe_unused]] const std::coroutine_handle<> Waiting
	) const {
		// 待つ側の中断と此の組立の作業列への登録
		Handle.promise().Work->push_back(Handle);
		// 終了
		return;
	}

	/**
	 * 結果の取出関数
	 * @tparam Val
	 * @return 組立の結果
	 * @throws std::exception 組立中に送出した例外を其の儘送出し直す
	 */
	template<class Val> Val Canonicalizer::Pending<Val>::await_resume() {
		// 組立中に送出した例外の送出直
		if(Handle.promise().Error) std::rethrow_exception(Handle.promise().Error);
		// 結果の返戻
		return std::move(*Handle.promise().Result);
	}

	/**
	 * 作業列の実行関数（整形仕様 9.2）
	 * 此の組立を作業列へ積み，列が空に為る迄最後の組立を再開する
	 * 完了した組立を除き，其の下で待つ側を次に再開する
	 * 計算量：組立の数 K に対し O(K) 回の再開（呼出の段数はネストの段数に依らず一定）
	 * @tparam Val
	 * @return 此の組立の結果
	 * @throws std::exception 組立中に送出した例外を其の儘送出し直す
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	template<class Val> Val Canonicalizer::Pending<Val>::Run() {
		// 作業列の実行
		std::vector<std::coroutine_handle<>> &Work = *Handle.promise().Work;
		Work.push_back(Handle);
		while(!Work.empty()) {
			const std::coroutine_handle<> Top = Work.back();
			Top.resume();
		}
		// 結果の返戻
		return await_resume();
	}

	/**
	 * ムーブコンストラクタ
	 * @tparam Val
	 * @param Other 所有を移す組立
	 */
	template<class Val> Canonicalizer::Pending<Val>::Pending(Pending &&Other) noexcept : Handle(std::exchange(Other.Handle, {})) {
		// 終了
		return;
	}

	/**
	 * デストラクタ（組立の状態を解放する）
	 * @tparam Val
	 */
	template<class Val> Canonicalizer::Pending<Val>::~Pending() {
		// デストラクタ
		if(Handle) Handle.destroy();
		// 終了
		return;
	}

	/**
	 * 組立の取得関数
	 * @tparam Val
	 * @return 此の状態を持つ組立
	 */
	template<class Val> Canonicalizer::Pending<Val> Canonicalizer::PendingPromise<Val>::get_return_object() {
		// 組立の返戻
		return Pending<Val>(std::coroutine_handle<PendingPromise>::from_promise(*this));
	}

	// 組立の状態と結果型の組立は，メンバの関数テンプレートを除く全てのメンバを実体化する
	template struct Canonicalizer::PendingPromise<Canonicalizer::Built>; // 結果型の組立の状態の実体化
	template class Canonicalizer::Pending<Canonicalizer::Built>; // 結果型の組立の実体化

	template void Canonicalizer::FinalPause::await_suspend(
		const std::coroutine_handle<Canonicalizer::PendingPromise<Canonicalizer::Built>> Completed
	) const noexcept; // 完了した組立の除去の実体化

	template struct Canonicalizer::PendingPromise<const Canonicalizer::Built *>; // 結果の参照の組立の状態の実体化

	template Canonicalizer::Pending<const Canonicalizer::Built *>::Pending(
		const std::coroutine_handle<Canonicalizer::PendingPromise<const Canonicalizer::Built *>> Started
	); // 状態の所有の実体化

	template Canonicalizer::Pending<const Canonicalizer::Built *>::Pending(
		Canonicalizer::Pending<const Canonicalizer::Built *> &&Other
	) noexcept; // 所有の移動の実体化

	template Canonicalizer::Pending<const Canonicalizer::Built *>::~Pending(); // 状態の解放の実体化
	template bool Canonicalizer::Pending<const Canonicalizer::Built *>::await_ready() const noexcept; // 未完了の判定の実体化
	template void Canonicalizer::Pending<const Canonicalizer::Built *>::await_suspend(const std::coroutine_handle<> Waiting) const; // 組立の登録の実体化
	template const Canonicalizer::Built *Canonicalizer::Pending<const Canonicalizer::Built *>::await_resume(); // 結果の取出の実体化

	template void Canonicalizer::FinalPause::await_suspend(
		const std::coroutine_handle<Canonicalizer::PendingPromise<const Canonicalizer::Built *>> Completed
	) const noexcept; // 完了した組立の除去の実体化

	/**
	 * 語で区切る節の独立した候補境界の追加関数
	 * 計算量：生成する固定数の要素に対し O(1)
	 * @param State 構築中の状態
	 * @param Output 節を並べる出力
	 * @throws FormatLimitExceeded 出力要素の生成が作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::AppendKeywordBoundary(Builder &State, Emitter &Output) {
		// 語で区切る節の独立した候補境界の追加
		if(Output.GetLast().ShouldBreakAfter) {
			Output.Break();
			// 後送の終端に必須境界を置いた場合の終了
			return;
		}
		if(State.NoBreakDepth) {
			Output.Space();
			// 封止した開始行の境界の終了
			return;
		}
		Emitter Boundary(State.IR);
		Boundary.Soft(" ");
		Output.Append(Boundary.Finish(NoNode, State.NextGroup++));
		// 終了
		return;
	}

	/**
	 * 語で区切る節の組立関数
	 * 計算量：直接の子数 C に対し O(C) と子の組立・本体の文数判定・出力生成の総費用
	 * @param State 構築中の状態
	 * @param Id 節
	 * @param HasLeadingBoundary 節の前の境界を此の節へ所属させるか
	 * @param ShouldBreakBefore 節の前を必須改行にするか
	 * @return 見出と本体を組み立てた要素
	 * @throws FormatLimitExceeded 構築の作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildKeywordSection(
		Builder &State,
		const NodeId Id,
		const bool HasLeadingBoundary,
		const bool ShouldBreakBefore
	) {
		// 節の展開要因と条件の区切の準備
		const SyntaxModel &Syntax = State.Syntax;
		Emitter Output(State.IR), Sections(State.IR);
		const bool IsTransparent = Syntax.Is(Id, Role::TransparentSection), IsConditional = Syntax.Is(Id, Role::CondSection);
		// 例外本体の `else` は分岐の単一文本体と異なり，`rescue`・`ensure` と同じ展開形へ揃える
		const NodeId Parent = Syntax.GetParentOf(Id);
		const bool IsExceptionAlternative =
		Syntax.Is(Id, Role::Else) && (Syntax.Is(Parent, Role::ExplicitKeywordBody) || Syntax.Is(Parent, Role::StmtSeq));
		const bool IsForced = Syntax.Is(Id, Role::ExceptionSection) || Syntax.Is(Id, Role::FinalSection) || IsExceptionAlternative;
		// 此の節の式のグループを先に確定し，後続節は別のグループとして基準順位を左から与える
		if(HasLeadingBoundary) {
			if(ShouldBreakBefore && State.NoBreakDepth) {
				Output.GetText(Syntax, GeneratedToken::StmtTerminator);
				Output.Space();
			} else if(ShouldBreakBefore) Output.Break();
			else if(State.NoBreakDepth) Output.Space();
			else Output.Soft(" ");
		}
		bool IsMulti = IsForced || !IsConditional && GetBodyStmtCount(State, Id) > 1;
		for(const NodeId Child : Syntax.GetChildrenOf(Id)) if(Syntax.Is(Child, Role::StmtSeq)) {
			IsMulti = IsMulti || GetBodyStmtCount(State, Child) > 1;
		}
		// 見出と本体の順次配置
		bool HasBody = false, IsScope = false, HasExpandedTail = IsMulti;
		uint32_t OpenHeredocs = 0;
		const std::pmr::vector<uint8_t> Candidates = BreakBoundaries(State, Id);
		for(const NodeId Child : Syntax.GetChildrenOf(Id)) {
			if(!Syntax.IsNamed(Child)) {
				if(IsTransparent || Syntax.IsTokenRole(Child, Role::StmtTerminatorToken)) continue;
				if(!Output.IsEmpty() && !Output.IsAtLineStart() && !Output.HasTrailingSpace() && !Syntax.IsTokenRole(Child, Role::CommaToken)) {
					Output.Space();
				}
				Output.Append(BuildLeaf(Syntax, State.IR, Child));
				continue;
			}
			if(!IsTransparent && Syntax.Is(Child, Role::KeywordSection) && !Syntax.Is(Child, Role::StmtSeq)) {
				if(IsScope) {
					Output.CloseScope();
					IsScope = false;
				}
				if(IsConditional && !HasBody) Output.GetText(Syntax, Word::CondThen, true);
				const bool ShouldSeparate = IsMulti || Output.GetLast().ShouldBreakAfter || Syntax.Is(Child, Role::ExceptionSection) ||
				Syntax.Is(Child, Role::FinalSection);
				Sections.Append(Output.Finish(NoNode, State.NextGroup++));
				Output = Emitter(State.IR);
				Built Section = co_await BuildKeywordSection(State, Child, true, ShouldSeparate);
				HasExpandedTail = Section.HasExpandedKeywordTail;
				Output.Append(std::move(Section));
				HasBody = true;
				continue;
			}
			if(Syntax.Is(Child, Role::DeferredLiteralBody)) {
				Output.Append(co_await BuildHeredocBody(State, Child));
				TrackOpenHeredocs(State, Child, OpenHeredocs);
				continue;
			}
			const bool IsBody = IsTransparent || !IsHeaderField(Syntax, Child);
			// ヒアドキュメントの本文を控えた開始行の続きは折らない（行分割仕様 2.3）
			Built Piece;
			if(OpenHeredocs) Piece = co_await BuildSealedFlat(State, Child);
			else if(IsBody) Piece = co_await TakeStructured(State, Child);
			else Piece = co_await BuildFlat(State, Child);
			if(Piece.Head.Item == NoItem) continue;
			if(IsBody && !IsTransparent) {
				if(!IsScope) {
					Output.OpenScope();
					IsScope = true;
				}
				if(State.NoBreakDepth) {
					if(IsConditional) Output.GetText(Syntax, Word::CondThen, true, true);
					else if(IsForced) AppendHeredocPendingSeparator(Syntax, Output);
					else Output.Space();
				} else if(IsMulti) {
					if(IsConditional && HasTrailingOpenRange(State, Syntax.GetFieldChild(Id, ChildRole::Pattern))) {
						Output.GetText(Syntax, Word::CondThen, true);
					}
					Output.Break();
				} else if(IsConditional) Output.GetText(Syntax, Word::CondThen, true, true);
				else Output.Space();
			} else if(!Output.IsEmpty()) {
				if(IsTransparent && (OpenHeredocs || State.NoBreakDepth)) AppendHeredocPendingSeparator(Syntax, Output);
				else if(IsTransparent && IsMulti) Output.Break();
				else if(!OpenHeredocs && !Candidates.empty() && Candidates[Syntax.GetNode(Child).Idx]) Output.Soft(" ");
				else Output.Space();
			}
			AppendChild(State, Child, std::move(Piece), Output);
			HasBody = HasBody || IsBody;
			TrackOpenHeredocs(State, Child, OpenHeredocs);
		}
		if(IsScope) Output.CloseScope();
		if(IsConditional && !HasBody && !Syntax.Is(Id, Role::PatternSection)) Output.GetText(Syntax, Word::CondThen, true);
		Built Result;
		if(Sections.IsEmpty()) Result = Output.Finish(Id, State.NextGroup++);
		else {
			Sections.Append(Output.Finish(NoNode, State.NextGroup++));
			Result = Sections.Finish(Id, NoGroup);
		}
		Result.HasBreak = Result.HasBreak || IsForced && !State.NoBreakDepth;
		Result.HasExpandedKeywordTail = HasExpandedTail && !State.NoBreakDepth;
		// 節の要素の返戻
		co_return Result;
	}

	/**
	 * 子列の共有要約と外側の継続を対応付ける関数
	 * 計算量：ノード数 N と子列の長さ L に対し O(N + L)
	 * @param State 構築中の状態
	 * @throws FormatLimitExceeded 構築の作業予算を超える場合
	 * @throws std::bad_alloc ノードの要約又は作業列を確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	void Canonicalizer::PrepareContexts(Builder &State) {
		// 子列の共有要約と外側の継続の対応付
		const SyntaxModel &Syntax = State.Syntax;
		const NodeId Root = Syntax.GetRoot();
		State.Contexts.resize(Syntax.GetNodeCount());
		const bool HasErrors = Syntax.GetNode(Root).Facts.Has(ConstraintKind::ContainsError);
		if(Syntax.GetRules().HasDeferredLiteralBodies) State.Heredocs.resize(Syntax.GetNodeCount());
		// 子の確定した要約を親へ１度継ぎ，子数・トークンの端・補間と節の判定を同じ走査で集める
		std::vector<uint32_t> Opens; // 完了した親毎に再使用する括弧の作業列
		NodeId Current = Root;
		bool IsEntering = true;
		// 構文の接続は変わらない為，既存の親・兄弟位置を使って同じ後行順を辿る
		while(1) {
			const SyntaxNode &Node = Syntax.GetNode(Current);
			const std::span<const NodeId> Children = Node.Children;
			if(IsEntering && !Children.empty()) {
				Syntax.Charge(1);
				Current = Children.front();
				continue;
			}
			Syntax.Charge(1);
			const NodeId Id = Current;
			NodeCtx &Ctx = State.Contexts[Id];
			const RoleSet *const Roles = Node.Kind.Def ? &Node.Kind.Def->Roles : nullptr;
			const auto Has = [Roles](const Role Wanted) -> bool {
				// 同じノードの固定定義から役割を照会する
				return Roles && Roles->test(static_cast<size_t>(Wanted));
			};
			const std::string &Lexeme = Node.Lexeme;
			if(!Lexeme.empty()) {
				Ctx.FirstEdge = { Id, static_cast<unsigned char>(Lexeme.front()) };
				Ctx.LastEdge = { Id, static_cast<unsigned char>(Lexeme.back()) };
			}
			Ctx.SingleToken = Lexeme.empty() ? NoNode : Id;
			bool IsSingleToken = true;
			const bool NeedsKeywordClose = Has(Role::TailValFunction) || Has(Role::TransparentSection);
			Ctx.FirstPathLeaf = Children.empty() ? Id : State.Contexts[Children.front()].FirstPathLeaf;
			Ctx.LastPathLeaf = Children.empty() ? Id : State.Contexts[Children.back()].LastPathLeaf;
			Ctx.IsLeadingParenType =
			Syntax.IsNamed(Id) && (Has(Role::ParenLeadType) || !Children.empty() && State.Contexts[Children.front()].IsLeadingParenType);
			Ctx.FirstNamed = static_cast<uint32_t>(Children.size());
			// コンテナ固有の囲みと無名トークンの対を，子の要約と同じ走査で対応付ける
			const uint32_t Count = static_cast<uint32_t>(Children.size());
			if(Count > 1 && Has(Role::MarkupElement)) State.Contexts[Children[0]].ScopeCloser = Count - 1;
			if(Count > 2 && Has(Role::ExprTag)) {
				const uint32_t Head = Count > 3 && Syntax.Is(Children[2], Role::GenericArgs) ? 2 : 1;
				State.Contexts[Children[Head]].ScopeCloser = Count - 1;
			}
			if(Count > 1 && (Has(Role::WordArray) || Has(Role::PipeParamList))) State.Contexts[Children[0]].ScopeCloser = Count - 1;
			Ctx.IsMemberSequence = Has(Role::TypeMemberBody) && Has(Role::ObjectLike) || Has(Role::ImportItems);
			Ctx.HasStatementBody = Has(Role::StmtFlowContainer) || Has(Role::MemberConstructor) && Has(Role::InlineDefBody);
			const bool IsObject = Count && Has(Role::ObjectLike), IsGroupedDecl = Count && Has(Role::GroupedDecl);
			const bool IsColonBody = Count && Has(Role::ColonBodyHost), NeedsChildTokens = IsObject || IsGroupedDecl || IsColonBody;
			// ブロックの展開とラムダの値の判定が共有する本体は，既存の子列走査で文数も集める
			bool ShouldCountBody = Has(Role::ClosureStmts);
			if(!ShouldCountBody && Has(Role::ValStmtSeq)) ShouldCountBody = Syntax.Is(Syntax.GetParentOf(Id), Role::LambdaLiteral);
			if(ShouldCountBody) Ctx.BodyStmtCount = 0;
			Opens.clear();
			unsigned char Prev = '\0';
			for(uint32_t Idx = 0; Idx < Children.size(); ++Idx) {
				Syntax.Charge(1);
				const NodeId Child = Children[Idx];
				Ctx.HasStatementBody = Ctx.HasStatementBody || State.Contexts[Child].HasStatementBody;
				Ctx.HasAccessSection = Ctx.HasAccessSection || Syntax.Is(Child, Role::AccessSection);
				NodeCtx &Part = State.Contexts[Child];
				const bool IsNamed = Syntax.IsNamed(Child), IsContent = IsNamed || Syntax.Is(Child, Role::EmptyStmt);
				// 空の葉は無視し，１つの非空トークンだけを含むラッパーの対応を同じ後行走査で継ぐ
				if(Part.FirstEdge.Leaf != NoNode) {
					if(!IsSingleToken || Part.SingleToken == NoNode || Ctx.SingleToken != NoNode) {
						IsSingleToken = false;
						Ctx.SingleToken = NoNode;
					} else Ctx.SingleToken = Part.SingleToken;
				}
				if(NeedsKeywordClose && !IsNamed && !Syntax.GetNode(Child).Lexeme.empty()) {
					Ctx.HasKeywordClose = Ctx.HasKeywordClose || Syntax.Is(Child, Role::BlockCloseKeyword);
				}
				Ctx.ContentCount += IsContent;
				if(
					ShouldCountBody && Ctx.BodyStmtCount < 2 && IsContent && !Syntax.Is(Child, Role::DeferredLiteralBody) &&
					!IsHeaderField(Syntax, Child)
				) ++Ctx.BodyStmtCount;
				if(Count > 1 || NeedsChildTokens) {
					const KindDef *const Token =
					Syntax.GetNode(Child).Kind.Def && !Syntax.GetChildCount(Child) ? GetSpacingTokenOf(State, Child) : nullptr;
					const auto Has = [](const KindDef *const Def, const Role Wanted) -> bool {
						// 実際の綴りの定義が役割を持つ事の返戻
						return IsRole(Def, Wanted);
					};
					if(NeedsChildTokens) {
						Ctx.HasBraceOpen = Ctx.HasBraceOpen || IsObject && Has(Token, Role::OpeningBraceToken);
						Ctx.HasBraceClose = Ctx.HasBraceClose || IsObject && Has(Token, Role::ClosingBraceToken);
						Ctx.HasParenOpen = Ctx.HasParenOpen || IsGroupedDecl && Has(Token, Role::OpeningParenToken);
						Ctx.HasColon = Ctx.HasColon || IsColonBody && Has(Token, Role::ColonToken);
					}
					if(
						Count > 1 &&
						(Has(Token, Role::OpeningParenToken) || Has(Token, Role::OpeningBracketToken) || Has(Token, Role::OpeningBraceToken))
					) Opens.push_back(Idx);
					else if(
						(Has(Token, Role::ClosingParenToken) || Has(Token, Role::ClosingSquareToken) || Has(Token, Role::ClosingBraceToken)) &&
						!Opens.empty()
					) {
						const uint32_t Open = Opens.back();
						Opens.pop_back();
						const KindDef *const Opening = State.Contexts[Children[Open]].SpacingToken;
						if(
							Has(Token, Role::ClosingParenToken) && Has(Opening, Role::OpeningParenToken) ||
							Has(Token, Role::ClosingSquareToken) && Has(Opening, Role::OpeningBracketToken) ||
							Has(Token, Role::ClosingBraceToken) && Has(Opening, Role::OpeningBraceToken)
						) State.Contexts[Children[Open]].ScopeCloser = Idx;
					}
				}
				if(IsNamed) {
					++Ctx.NamedCount;
					Ctx.FirstNamed = std::min(Ctx.FirstNamed, Idx);
					Ctx.LastNamed = Idx;
					Ctx.BracketContents += !Syntax.GetChildCount(Child) || Part.NamedCount;
				}
				Ctx.HasDirectError = Ctx.HasDirectError || HasErrors && Syntax.Is(Child, Role::Error);
				Ctx.HasErrorChild = Ctx.HasErrorChild || IsNamed && Syntax.GetNode(Child).Facts.Has(ConstraintKind::ContainsError);
				Ctx.HasRenderedChild = Ctx.HasRenderedChild || IsNamed && Syntax.Is(Child, Role::RenderedChildren);
				Ctx.HasElseChild = Ctx.HasElseChild || Syntax.Is(Child, Role::ElseLikeClause);
				Ctx.HasElseGrandchild = Ctx.HasElseGrandchild || Part.HasElseChild;
				Ctx.HasExceptionChild =
				Ctx.HasExceptionChild || Syntax.Is(Child, Role::ExceptionSection) || Syntax.Is(Child, Role::FinalSection);
				if(Lexeme.empty()) {
					if(Ctx.FirstEdge.Leaf == NoNode) Ctx.FirstEdge = Part.FirstEdge;
					if(Part.LastEdge.Leaf != NoNode) Ctx.LastEdge = Part.LastEdge;
				}
				Part.Preceding = Prev;
				Part.HasPreceding = Prev != '\0';
				if(Part.LastEdge.Char) Prev = Part.LastEdge.Char;
			}
			unsigned char Next = '\0';
			for(size_t Idx = Children.size(); Idx--;) {
				Syntax.Charge(1);
				NodeCtx &Part = State.Contexts[Children[Idx]];
				Part.Following = Next;
				Part.HasFollowing = Next != '\0';
				if(Part.FirstEdge.Char) Next = Part.FirstEdge.Char;
			}
			// 同じ後行順で後送の開始・本文と照合の節を要約し，未完了の親へ渡す
			if(!State.Heredocs.empty()) {
				HeredocSummary &Summary = State.Heredocs[Id];
				Summary.Begins += Has(Role::DeferredLiteralStart);
				Summary.Bodies += Has(Role::DeferredLiteralBody);
				Summary.HasPattern = Summary.HasPattern || Has(Role::PatternBranch);
				if(const NodeId Parent = Syntax.GetParentOf(Id); Parent != NoNode) {
					HeredocSummary &Whole = State.Heredocs[Parent];
					Whole.Begins += Summary.Begins;
					Whole.Bodies += Summary.Bodies;
					Whole.HasPattern = Whole.HasPattern || Summary.HasPattern;
				}
			}
			if(Id == Root) break;
			if(const NodeId Next = Syntax.GetNextSibling(Id); Next != NoNode) {
				Syntax.Charge(1);
				Current = Next;
				IsEntering = true;
			} else {
				Current = Syntax.GetParentOf(Id);
				IsEntering = false;
			}
		}
		// 外側から継ぐ型・継続・後続節の文脈は，確定した子数と端の要約を参照する
		const auto NextNode = [&Syntax, Root](NodeId Node, const std::span<const NodeId> Children) -> NodeId {
			// 先頭の子，又は現在の部分木の後に在る最初の兄弟の返戻
			if(!Children.empty()) return Children.front();
			for(; Node != Root; Node = Syntax.GetParentOf(Node)) if(const NodeId Next = Syntax.GetNextSibling(Node); Next != NoNode) {
				// 後続の兄弟が在る最内の所有範囲から再開する
				return Next;
			}
			// 根の所有範囲の終わりの返戻
			return NoNode;
		};
		std::span<const NodeId> Children;
		State.Contexts[Root].IsTypeUnion = false;
		for(Current = Root; Current != NoNode; Current = NextNode(Current, Children)) {
			const NodeId Id = Current;
			Syntax.Charge(1);
			NodeCtx &Ctx = State.Contexts[Id];
			const SyntaxNode &Node = Syntax.GetNode(Id);
			const RoleSet *const Roles = Node.Kind.Def ? &Node.Kind.Def->Roles : nullptr;
			const auto Has = [Roles](const Role Wanted) -> bool {
				// 同じノードの固定定義から役割を照会する
				return Roles && Roles->test(static_cast<size_t>(Wanted));
			};
			const NodeId Parent = Syntax.GetParentOf(Id);
			// 同じ親の子へ継ぐ役割は，親毎に１度だけ読む
			const bool IsIndentedBody = Has(Role::IndentedBody), IsContinuation = Has(Role::ContinuationExpr);
			// 型の範囲と値の分岐の連鎖の引継
			Ctx.IsAnnotation = Has(Role::TypeSyntax) || !IsIndentedBody && Ctx.IsAnnotation;
			if(Has(Role::BracedConditionalVal)) {
				const NodeId Host = Syntax.GetParentOf(Parent);
				Ctx.IsArrowVal = Syntax.Is(Parent, Role::BodyWrapper) &&
				(Syntax.Is(Host, Role::ConditionalVal) ? Host != NoNode && State.Contexts[Host].IsArrowVal : Syntax.Is(Host, Role::ValArm)) ||
				Syntax.Is(Parent, Role::ValStmtSeq) && Syntax.Is(Host, Role::LambdaLiteral) && GetBodyStmtCount(State, Parent) == 1;
			}
			Ctx.ArrowChainRoot = Id;
			if(Syntax.Is(Parent, Role::BodyWrapper) && Syntax.Is(Syntax.GetPrevSibling(Parent), Role::ElseKeyword)) {
				if(const NodeId Host = Syntax.GetParentOf(Parent); Syntax.Is(Host, Role::If)) {
					Ctx.ArrowChainRoot = State.Contexts[Host].ArrowChainRoot;
				}
			}
			Ctx.ShouldWrap = Node.Facts.TokenSpacing == TokenSpacingMode::None && IsContinuation && !Syntax.HasField(Id, ChildRole::Name) &&
			!Syntax.HasField(Id, ChildRole::Alias) &&
			!(Syntax.Is(Parent, Role::ExprStmt) && (Node.Children.empty() || Has(Role::StrLikeAll))) && !Ctx.IsContinuationBlocked &&
			!Node.Facts.Has(ConstraintKind::ContainsError) && !(
				Has(Role::TypeSyntax) && Ctx.NamedCount == 1 &&
				Syntax.IsEdgeCharRole(State.Contexts[Syntax.GetChild(Id, Ctx.FirstNamed)].FirstEdge, Role::OpeningParenLead)
			) && !(
				Syntax.HasField(Id, ChildRole::Left) && (
					Syntax.Is(Parent, Role::TypeAlias) ||
					!Has(Role::BareTuple) && (Syntax.Is(Parent, Role::Assignment) || Syntax.Is(Parent, Role::Loop))
				)
			);
			Ctx.IsLineJoined = Ctx.IsLineJoined || Ctx.ShouldWrap;
			Children = Node.Children;
			const CanonicalForm *const BodyForm = Syntax.GetFormOf(Id, FormOperation::BraceBody);
			const bool CanWrapBody = Node.Facts.TokenSpacing == TokenSpacingMode::None && BodyForm && !Has(Role::Labeled) &&
			!Node.Facts.Has(ConstraintKind::ContainsError) && !Ctx.HasColon;
			const bool IsBoundary = IsIndentedBody || Parent == NoNode, IsPostfixHost = !Children.empty() && Has(Role::PostfixReceiverHost);
			const bool IsChildTypeUnion = !Children.empty() && Syntax.IsNamed(Id) &&
			(Has(Role::BinaryOperator) || Has(Role::ValParens) ? Ctx.IsTypeUnion : Has(Role::TypeSlot));
			const bool HasNonExprNames =
			Has(Role::CasePattern) || Has(Role::ImportStmt) || Has(Role::ImportedMemberBinding) || Has(Role::ImportedModuleBinding) ||
			Has(Role::ImportedPathBinding) || Has(Role::TypeHintExternalBinding);
			int Depth = 0;
			for(const NodeId Child : Children) {
				Syntax.Charge(1);
				NodeCtx &ChildCtx = State.Contexts[Child];
				const bool ShouldStopContinuation = IsBoundary || !(Ctx.IsLineJoined || Depth > 0) && (
					Syntax.HasField(Child, ChildRole::Body) && !IsContinuation ||
					(Syntax.HasField(Child, ChildRole::Left) || Syntax.HasField(Child, ChildRole::Right)) &&
					(Has(Role::Assignment) || Has(Role::Loop))
				);
				const uint32_t OwnContent = Syntax.IsNamed(Child) && (!Syntax.GetChildCount(Child) || ChildCtx.NamedCount);
				const bool IsPostfixBlocked =
				IsPostfixHost && (Syntax.GetChild(Id, Ctx.FirstNamed) != Child || Ctx.BracketContents > OwnContent);
				ChildCtx.IsContinuationBlocked =
				!ShouldStopContinuation && (Ctx.IsContinuationBlocked || IsContinuation || IsPostfixBlocked || HasNonExprNames || Depth > 0);
				ChildCtx.IsLineJoined = !IsBoundary && (Ctx.IsLineJoined || Depth > 0);
				ChildCtx.IsAnnotation = Ctx.IsAnnotation;
				ChildCtx.IsTypeUnion = IsChildTypeUnion;
				ChildCtx.IsOptionalBody = CanWrapBody && IsBraceOptionalBody(State, *BodyForm, Id, Child);
				const bool IsOptionalBody = ChildCtx.IsOptionalBody;
				ChildCtx.HasFollowingClause = Ctx.HasFollowingClause && Syntax.GetNode(Child).Idx == Ctx.LastNamed &&
				(IsOptionalBody || Has(Role::BodyWrapper) || Syntax.Is(Child, Role::ElseClause) || Syntax.Is(Child, Role::ElseIfClause));
				if(IsOptionalBody) {
					const NodeId Next = Syntax.GetNextSibling(Child);
					ChildCtx.HasFollowingClause =
					ChildCtx.HasFollowingClause || Syntax.Is(Next, Role::ElseKeyword) || Syntax.Is(Next, Role::ElseClause) ||
					Syntax.Is(Next, Role::ElseIfClause) || Has(Role::DoLoop);
				}
				Depth += Syntax.GetBracketLeadDelta(Child);
			}
			// エラーの保持境界に接する子を継続括弧で包まない
			for(const SemanticBoundary &Boundary : Node.Facts.Boundaries) {
				if(
					Boundary.Kind != BoundaryKind::Preserved ||
					Boundary.Src != PreservedSrc::ErrorBoundary && Boundary.Src != PreservedSrc::ErrorContent
				) continue;
				if(Boundary.Before) State.Contexts[Children[Boundary.Before - 1]].IsContinuationBlocked = true;
				if(Boundary.Before < Children.size()) State.Contexts[Children[Boundary.Before]].IsContinuationBlocked = true;
			}
			if(
				Node.Facts.TokenSpacing == TokenSpacingMode::None && Has(Role::ClassBodyContainer) && !Has(Role::AmbiguousBodyContainer) &&
				!Syntax.Is(Parent, Role::ClassLike)
			) CollectBlankRules(State, Id, false);
			if(Node.Facts.TokenSpacing == TokenSpacingMode::None && Has(Role::ClassLike)) for(const NodeId Body : Children) {
				Syntax.Charge(1);
				if(Syntax.Is(Body, Role::ClassBodyContainer) || Syntax.Is(Body, Role::IndentedBody)) CollectBlankRules(State, Body, false);
			}
		}
		// 終了
		return;
	}

	/**
	 * ドキュメントの表示境界に従う組立関数
	 * 計算量：子数 C，境界数 B，間隙のテキスト長 L に対し O(C + B + L) と子の組立・出力要素の生成の総費用
	 * @param State 構築中の状態
	 * @param Id ドキュメント又は要素
	 * @return 本文と表示される空白を保った要素
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildMarkup(Builder &State, const NodeId Id) {
		// 子の展開と意味上の空白から親の展開を決める
		const SyntaxModel &Syntax = State.Syntax;
		const SyntaxNode &Node = Syntax.GetNode(Id);
		const bool IsRoot = Node.Parent == NoNode;
		bool ShouldExpand = IsRoot, HasBoundary = false, HasPreserved = false;
		uint32_t Elements = 0, ScopeEnd = static_cast<uint32_t>(Node.Children.size());
		for(uint32_t Idx = 0; Idx < Node.Children.size(); ++Idx) {
			const NodeId Child = Node.Children[Idx];
			if(ScopeEnd == Node.Children.size() && Syntax.Is(Child, Role::MarkupClose)) ScopeEnd = Idx;
			if(Syntax.Is(Child, Role::MarkupContainer)) {
				++Elements;
				if(!ShouldExpand) ShouldExpand = (co_await GetStructured(State, Child))->HasBreak;
			} else if(const OwnedRegion *const Owned = Syntax.GetRegionOf(Child)) {
				// 子のコードを置く必須改行を祖先へ伝え，コード０行の子は展開の要因としない
				ShouldExpand = ShouldExpand || !Owned->Code->GetFragments().empty();
			}
		}
		for(const SemanticBoundary &Boundary : Node.Facts.Boundaries) {
			HasPreserved = HasPreserved || Boundary.Kind == BoundaryKind::Preserved;
			if(
				Boundary.Before && Boundary.Before < Node.Children.size() && !Syntax.Is(Node.Children[Boundary.Before - 1], Role::MarkupOpen) &&
				!Syntax.Is(Node.Children[Boundary.Before], Role::MarkupClose)
			) HasBoundary = HasBoundary || Boundary.Kind == BoundaryKind::Spaced || Boundary.Kind == BoundaryKind::Separated;
		}
		ShouldExpand = !HasPreserved && (ShouldExpand || Elements > 1 && HasBoundary);
		// 閉タグの前で終える深さの範囲の位置の取得
		Emitter Output(State.IR);
		bool IsScope = false;
		if(ScopeEnd < Node.Children.size()) while(ScopeEnd && Syntax.Is(Node.Children[ScopeEnd - 1], Role::CommentNode)) --ScopeEnd;
		for(uint32_t Idx = 0; Idx <= Node.Children.size(); ++Idx) {
			const NodeId Child = Idx < Node.Children.size() ? Node.Children[Idx] : NoNode;
			// 閉タグの直前のコメントの為の，タグと同じ深さの境界の配置
			if(Idx == ScopeEnd && IsScope) {
				Output.CloseScope();
				IsScope = false;
			}
			// 表示境界に応じた原文の間隙と改行の配置
			if(const SemanticBoundary *const Boundary = Node.Facts.GetBoundaryBefore(Idx)) {
				if(Boundary->Kind == BoundaryKind::Preserved) {
					Built Gap;
					Gap.Item = State.IR.Leaf(Boundary->Gap, FormatItemKind::Verbatim);
					Gap.Head = FindEdge(State.IR, Gap.Item, true);
					Gap.Tail = Gap.Head;
					Gap.HasBreak = State.IR.Items[Gap.Item].HasNewline;
					Output.Append(std::move(Gap));
				} else if(ShouldExpand && (Boundary->Kind == BoundaryKind::Separated || Boundary->Kind == BoundaryKind::Spaced)) Output.Break();
				else if(Boundary->Kind == BoundaryKind::Spaced || Boundary->Kind == BoundaryKind::RequiredSpace) Output.Space();
			}
			// 子又は所有領域の配置
			if(Child != NoNode) {
				if(const OwnedRegion *const Owned = Syntax.GetRegionOf(Child); Owned && !Owned->Code->GetFragments().empty()) {
					// 所有領域を開始・終了タグとは別の行へ１段深く置く（整形仕様 4.4）
					// 領域内の行は配置処理で此の深さへ置き，親のトークンにはしない
					Emitter Code(State.IR);
					Code.OpenScope();
					Code.Break();
					FormatItem Region;
					Region.Kind = FormatItemKind::OwnedRegion;
					Region.Node = Child;
					Built Placed;
					Placed.Item = State.IR.Add(Region);
					// 領域は行を跨ぎ，後の内容を別の行へ置く端の要素
					Placed.Head = { Placed.Item, Child, '\0', '\0', false, true, false };
					Placed.Tail = Placed.Head;
					Placed.HasBreak = true;
					Code.Append(std::move(Placed));
					Code.CloseScope();
					Code.Break();
					Output.Append(Code.Finish(Child, State.NextGroup++));
				} else if(Owned) {
					// コードの無い子の零幅のアンカー（行・空白・折返の要因を追加しない）
					Emitter Marker(State.IR);
					Built Placed = Marker.Finish(Child, State.NextGroup++);
					Placed.Head = { Placed.Item, Child, '\0', '\0', false, false, false };
					Placed.Tail = Placed.Head;
					Output.Append(std::move(Placed));
				} else Output.Append(co_await TakeStructured(State, Child));
			}
			if(!IsRoot && Syntax.Is(Child, Role::MarkupOpen) && (ShouldExpand || HasPreserved)) {
				Output.OpenScope();
				IsScope = true;
			}
		}
		if(IsScope) Output.CloseScope();
		Built Result = Output.Finish(Id, State.NextGroup++);
		Result.HasBreak = Result.HasBreak || ShouldExpand;
		// ドキュメントの要素の返戻
		co_return Result;
	}

	/**
	 * 最上位の組立関数（整形仕様 6.1）
	 * 最上位の文・宣言を行毎に置き，本体の外の地の文は区切を置かず逐語の儘で本体の前後へコピーする
	 * 計算量：根の子数 C に対し O(C) と子の組立・境界の判定・出力生成の総費用
	 * @param State 構築中の状態
	 * @param Root 根
	 * @return 根の要素
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildTopLevel(Builder &State, const NodeId Root) {
		// 根の境界と文の配置状態の準備
		const SyntaxModel &Syntax = State.Syntax;
		Emitter Output(State.IR);
		NodeId PrevChild = NoNode;
		if(const SemanticBoundary *const Head = GetPreservedAt(Syntax, Root, 0)) Output.Gap(*Head);
		uint32_t OpenHeredocs = 0;
		for(const NodeId Child : Syntax.GetChildrenOf(Root)) {
			if(!Syntax.IsNamed(Child) && !Syntax.Is(Child, Role::EmptyStmt)) continue;
			// 本体の外の地の文は区切を置かず，其の儘本体の前後へコピーする（整形仕様 4.4）
			// 地の文は本体の配置の判断に加えない
			if(Syntax.Is(Child, Role::OuterText)) {
				Output.Append(co_await TakeStmt(State, Child));
				continue;
			}
			if(Syntax.Is(Child, Role::DeferredLiteralBody)) {
				Output.Append(co_await BuildHeredocBody(State, Child));
				TrackOpenHeredocs(State, Child, OpenHeredocs);
				PrevChild = Child;
				continue;
			}
			Built Piece;
			if(OpenHeredocs) Piece = co_await BuildSealedFlat(State, Child);
			else if(Syntax.Is(Child, Role::Preproc)) Piece = co_await TakeStructured(State, Child);
			else Piece = co_await TakeStmt(State, Child);
			if(PrevChild != NoNode && !Output.IsEmpty() && !Output.IsAtLineStart()) {
				// 誤読された宣言の終端の `;` は独立した最上位の子として現れる為，改行で切らず直前へ密着
				// 最上位のエラー領域に接する境界は原文の間隙を保つ
				const bool IsAttachSemicolon = !Syntax.Is(Child, Role::EmptyStmt) && Piece.Head.Item == Piece.Tail.Item &&
				IsEdgeRole(Syntax, State.IR, Piece.Head, Role::StmtTerminatorToken, GeneratedToken::StmtTerminator) && (
					Syntax.GetNode(PrevChild).Facts.Has(ConstraintKind::ContainsError) || Syntax.GetRules().CanAttachTopLevelSemicolon ||
					OpenHeredocs
				);
				if(
					const SemanticBoundary *const Preserved = IsAttachSemicolon ? nullptr : GetErrorGapBefore(Syntax, PrevChild, Child);
					Preserved
				) AppendPreservedGap(Syntax, PrevChild, *Preserved, Output);
				else if(HasProcessedCodeBoundary(State, PrevChild, Child, Output)) {} else if(
					!IsAttachSemicolon && OpenHeredocs && !Syntax.Is(Child, Role::DeferredLiteralBody)
				) AppendHeredocPendingSeparator(Syntax, Output);
				else if(!IsAttachSemicolon) {
					if(CanJoinAcrossNewline(Syntax, Output.GetStore(), Piece.Head)) Output.GetText(Syntax, GeneratedToken::StmtTerminator);
					Output.Break();
				}
			}
			AppendChild(State, Child, std::move(Piece), Output);
			TrackOpenHeredocs(State, Child, OpenHeredocs);
			PrevChild = Child;
		}
		// 根の要素の返戻
		co_return Output.Finish(Root, NoGroup);
	}

	/**
	 * ノードのアンカーで包む関数
	 * @param IR 構築中の中間表現
	 * @param Node アンカーのノード
	 * @param Content ノードの出力の要素
	 * @return アンカーの要素
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	FormatItemId Canonicalizer::Anchored(FormatIR &IR, const NodeId Node, const FormatItemId Content) {
		// ノードのアンカーに依るラッパー
		const FormatItemId Mark = IR.Parent(
			FormatItemKind::AnchorMark,
			Content == NoItem ? std::span<const FormatItemId>() : std::span<const FormatItemId>(&Content, 1)
		);
		IR.Items[Mark].Node = Node;
		// アンカーの要素の返戻
		return Mark;
	}

	/**
	 * 独立した選択部分の要素の取得関数
	 * 宣言区分の共有部分だけを独立した範囲として保持し，通常の並びは親の子列へ直接移す
	 * 計算量：並びの要素数 C に対し O(C)
	 * @param IR 構築中の中間表現
	 * @param Piece 選択部分の組立
	 * @return 独立した選択部分の要素
	 * @throws FormatLimitExceeded 要素の生成が作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 */
	FormatItemId Canonicalizer::GetItemOf(FormatIR &IR, Built &Piece) {
		// 独立した選択部分の要素の取得
		if(Piece.Item == NoItem && !Piece.Parts.empty()) {
			Piece.Item = IR.Parent(FormatItemKind::Sequence, Piece.Parts);
			Piece.Parts.clear();
		}
		// 独立した要素の返戻
		return Piece.Item;
	}

	/**
	 * 語で閉じる本体の組立関数
	 * 計算量：直接の子と節の子の総数 C，代替節の連鎖長 D に対し O(C + D) と子の組立・本体の文数判定・出力生成の総費用
	 * @param State 構築中の状態
	 * @param Id 本体の所有者
	 * @return 構造と文数で配置した要素
	 * @throws FormatLimitExceeded 構築の作業予算を超える場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	Canonicalizer::Pending<Canonicalizer::Built> Canonicalizer::BuildKeywordBlock(Builder &State, const NodeId Id) {
		// 本体を持つ子と後続節は独立した配置範囲として組み立てる
		const SyntaxModel &Syntax = State.Syntax;
		const bool IsForced = Syntax.Is(Id, Role::ExpandedBody), IsPattern = Syntax.Is(Id, Role::PatternBranch);
		const bool IsInvocationBody = Syntax.Is(Id, Role::InvocationBody);
		uint32_t PendingHeredocs = 0;
		TrackOpenHeredocs(State, Id, PendingHeredocs);
		// 式本体の定義は既存の平坦形を保つ事の返戻
		if(Syntax.Is(Id, Role::TailValFunction) && !State.Contexts[Id].HasKeywordClose) co_return co_await BuildFlat(State, Id);
		// 子の展開状態と本体の配置状態の準備
		Emitter Output(State.IR), Sections(State.IR);
		bool IsLastMulti = false, HasBody = false, IsDirectScope = false, HasHeredocHead = false;
		NodeId Prev = NoNode;
		// 見出と本体及び後続節の配置
		for(const NodeId Child : Syntax.GetChildrenOf(Id)) {
			const bool IsNamed = Syntax.IsNamed(Child);
			// 見出の後送の本文は終端の語の行で見出を終える為，ブロックの種別に依らず本体の範囲を開き，本体を複数行にする
			if(Syntax.Is(Child, Role::DeferredLiteralBody)) {
				if(!IsDirectScope) {
					Output.OpenScope();
					IsDirectScope = true;
				}
				HasHeredocHead = !HasBody;
				Output.Append(co_await BuildHeredocBody(State, Child));
				continue;
			}
			if(!IsNamed && Syntax.IsTokenRole(Child, Role::StmtTerminatorToken)) continue;
			if(
				IsDirectScope && (
					IsNamed && Syntax.Is(Child, Role::KeywordSection) && !Syntax.Is(Child, Role::StmtSeq) ||
					!IsNamed && (HasSingleTokenRole(State, Child, Role::BlockCloseKeyword) || Syntax.IsTokenRole(Child, Role::ClosingBraceToken))
				)
			) {
				Output.CloseScope();
				IsDirectScope = false;
			}
			// 本体を閉じる語と其の前の改行の配置
			if(
				!IsNamed && (HasSingleTokenRole(State, Child, Role::BlockCloseKeyword) || Syntax.IsTokenRole(Child, Role::ClosingBraceToken))
			) {
				if(!HasBody && Syntax.Is(Id, Role::CondSection)) Output.GetText(Syntax, Word::CondThen, true);
				Sections.Append(Output.Finish(NoNode, State.NextGroup++));
				Output = Emitter(State.IR);
				if(State.NoBreakDepth && ((IsForced || IsLastMulti) && !IsInvocationBody || !HasBody && Syntax.ShouldSeparateBeforeClose(Id))) {
					AppendHeredocPendingSeparator(Syntax, Sections);
				} else if(
					!State.NoBreakDepth && !PendingHeredocs && (IsLastMulti || IsPattern || !HasBody && Syntax.ShouldSeparateBeforeClose(Id))
				) Sections.Break();
				else if(!Sections.IsEmpty()) {
					// 本文を控えた開始行の閉じの同行配置
					if(PendingHeredocs) Sections.Space();
					else AppendKeywordBoundary(State, Sections);
				}
				Sections.Append(BuildLeaf(Syntax, State.IR, Child));
				continue;
			}
			// 子の本体・後続節・見出への分類
			const bool IsSeq = Syntax.Is(Child, Role::StmtSeq), IsSection = Syntax.Is(Child, Role::KeywordSection) && !IsSeq;
			const bool IsBody = IsSeq || IsNamed && !IsSection && !IsHeaderField(Syntax, Child) && Syntax.Is(Id, Role::ExplicitKeywordBody);
			// 例外節を含む文列は共有した子の事実から選び，捨てる通常の文列を先に組み立てない
			if(IsBody && IsSeq && State.Contexts[Child].HasExceptionChild) {
				Emitter Sequence(State.IR);
				if(State.NoBreakDepth) {
					if(IsInvocationBody) Output.Space();
					else AppendHeredocPendingSeparator(Syntax, Output);
				}
				bool IsScope = false;
				for(const NodeId Part : Syntax.GetChildrenOf(Child)) {
					Syntax.Charge(1);
					if(!Syntax.IsNamed(Part)) continue;
					if(const bool IsClause = Syntax.Is(Part, Role::KeywordSection); IsClause && IsScope) {
						Sequence.CloseScope();
						IsScope = false;
					} else if(!IsClause && !IsScope) {
						Sequence.OpenScope();
						IsScope = true;
					}
					if(State.NoBreakDepth) {
						if(!Sequence.IsEmpty()) AppendHeredocPendingSeparator(Syntax, Sequence);
					} else Sequence.Break();
					AppendChild(State, Part, co_await TakeStructured(State, Part), Sequence);
				}
				if(IsScope) Sequence.CloseScope();
				Output.Append(Sequence.Finish(Child, NoGroup));
				HasBody = IsLastMulti = true;
				Prev = Child;
				continue;
			}
			Built Piece;
			if(IsSection) {
				const bool ShouldSeparate =
				IsLastMulti || IsPattern || Output.GetLast().ShouldBreakAfter || Syntax.Is(Child, Role::ExceptionSection) ||
				Syntax.Is(Child, Role::FinalSection);
				Piece = co_await BuildKeywordSection(State, Child, true, ShouldSeparate);
			} else if(IsNamed) Piece = co_await TakeStructured(State, Child);
			else Piece = BuildLeaf(Syntax, State.IR, Child);
			if(Piece.Head.Item == NoItem && !Syntax.Is(Child, Role::TransparentSection)) continue;
			if(IsBody) {
				// 通常の本体の段と改行の選択
				HasBody = true;
				IsLastMulti =
				IsForced || HasHeredocHead || GetBodyStmtCount(State, Child) > 1 || !Syntax.Is(Id, Role::CondSection) && Piece.HasBreak;
				if(!State.NoBreakDepth && Syntax.Is(Id, Role::CondSection) && IsLastMulti && HasTrailingOpenRange(State, Prev)) {
					Output.GetText(Syntax, Word::CondThen, true);
				} else if(!State.NoBreakDepth && Syntax.Is(Child, Role::IterationSection) && !IsLastMulti) {
					Output.GetText(Syntax, Word::IterationDo, true);
				}
				// 空の本体（反復の `do` だけの本体）は，閉じの語の前の境界だけを折れる所とし，折れた閉じの行頭へ空白を残さない
				const bool IsEmptyBody = Piece.Head.Item == NoItem;
				if(State.NoBreakDepth) {
					if(!IsDirectScope) Output.OpenScope();
					if(Syntax.Is(Id, Role::CondSection)) Output.GetText(Syntax, Word::CondThen, true, !IsEmptyBody);
					else if(Syntax.Is(Child, Role::IterationSection)) Output.GetText(Syntax, Word::IterationDo, true, !IsEmptyBody);
					else if((IsForced || IsLastMulti) && !IsInvocationBody) AppendHeredocPendingSeparator(Syntax, Output);
					else if(!IsEmptyBody) Output.Space();
				} else if(IsLastMulti) {
					if(!IsDirectScope) Output.OpenScope();
					IsDirectScope = !IsSeq;
					if(GetEdgeChar(State.IR, Piece.Head, true) != '\n') Output.Break();
				} else {
					Output.OpenScope();
					if(Syntax.Is(Id, Role::CondSection)) Output.GetText(Syntax, Word::CondThen, true, !IsEmptyBody);
					else if(!IsEmptyBody) Output.Space();
				}
				AppendChild(State, Child, std::move(Piece), Output);
				if(!IsDirectScope) Output.CloseScope();
				if(Syntax.Is(Child, Role::TransparentSection) && State.Contexts[Child].HasKeywordClose) {
					if(IsLastMulti && State.NoBreakDepth && !IsInvocationBody) AppendHeredocPendingSeparator(Syntax, Output);
					else if(IsLastMulti && !State.NoBreakDepth) Output.Break();
					else if(IsEmptyBody && State.NoBreakDepth) Output.Space();
					else if(IsEmptyBody) Output.Soft(" ");
					else Output.Space();
					Output.Append(BuildLeaf(Syntax, State.IR, Syntax.GetChild(Child, Syntax.GetChildCount(Child) - 1)));
				}
			} else if(IsSection) {
				// 前の境界を含む節の範囲の確定と，グループの外への後続節の分離
				if(!HasBody && Syntax.Is(Id, Role::CondSection)) Output.GetText(Syntax, Word::CondThen, true);
				Sections.Append(Output.Finish(NoNode, State.NextGroup++));
				Output = Emitter(State.IR);
				HasBody = true;
				IsLastMulti = Piece.HasExpandedKeywordTail;
				AppendChild(State, Child, std::move(Piece), Output);
			} else {
				// 見出の残りのトークンと子の配置
				if(
					Prev != NoNode &&
					ShouldInsertSpace(State, Id, Prev, Child, Syntax.GetNode(Id).Facts.GetBoundaryBefore(Syntax.GetNode(Child).Idx))
				) Output.Space();
				const bool IsHeader = IsNamed && Syntax.HasField(Child, ChildRole::Cond);
				if(IsHeader) Output.OpenScope();
				AppendChild(State, Child, std::move(Piece), Output);
				if(IsHeader) Output.CloseScope();
			}
			Prev = Child;
		}
		// 本体の要素の返戻
		if(Sections.IsEmpty()) co_return Output.Finish(Id, State.NextGroup++);
		Sections.Append(Output.Finish(NoNode, State.NextGroup++));
		// 節境界を持つブロックの返戻
		co_return Sections.Finish(Id, NoGroup);
	}

	/**
	 * フレームの要求数と新規確保数を観測する中間表現の構築関数（整形仕様 6.1）
	 * 正規化後の構文だけを読み，原文・コメント・行幅を参照しない
	 * 最上位の文・宣言を行毎に置き，空行の規則を宣言の構造種別から記録する
	 * 計算量：構文・出力要素数 N，トークン長 B，グループ数 G，条件付境界数 C と直接依存数 D に対し O(N + B + (G + C) log(G + 1) + D)
	 * @param NormalizedSyntax 正規化後の構文（以後変更せず検証完了迄共有する期待構文）
	 * @param FrameWork フレームの要求数と新規確保数の格納先（無ければ観測値を返さない）
	 * @return 幅非依存の中間表現
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	FormatIR Canonicalizer::BuildWithWork(
		std::shared_ptr<const SyntaxModel> NormalizedSyntax,
		std::pair<size_t, size_t> *const FrameWork
	) {
		// 中間表現の領域と構築状態の準備
		const SyntaxModel &Syntax = *NormalizedSyntax;
		FormatIR Result;
		// 要素の生成を正規化と同じ作業単位の上限へ計上する（整形仕様 9.2）
		Result.NormalizedSyntax = std::move(NormalizedSyntax);
		// 要素と子の並びはノードの数の３倍に概ね収まる為，伸長の度に古い領域を捨てる再確保を避けて予約する
		Result.Items.reserve(3 * Syntax.GetNodeCount());
		Result.Links.reserve(3 * Syntax.GetNodeCount());
		uint32_t GroupCapacity = 0;
		std::pair<size_t, size_t> FrameCounts {};
		{
			// 組立の文脈・控え・フレームは，決定単位の表を作る前に解放する
			Builder State { Syntax, Result };
			PrepareContexts(State);
			const NodeId Root = Syntax.GetRoot();
			if(Syntax.GetNode(Root).Children.empty()) Result.Root = BuildLeaf(Syntax, Result, Root).Item;
			else if(Syntax.Is(Root, Role::MarkupContainer)) Result.Root = BuildMarkup(State, Root).Run().Item;
			else {
				// 最上位と型のメンバの並びの空行の規則
				CollectBlankRules(State, Root, true);
				Result.Root = BuildTopLevel(State, Root).Run().Item;
			}
			GroupCapacity = State.NextGroup;
			if(FrameWork) FrameCounts = { State.Frames.Requests, State.Frames.Allocations };
		}
		Result.BuildDecisions(GroupCapacity);
		if(FrameWork) *FrameWork = FrameCounts;
		// 根の要素と期待構文の返戻
		return Result;
	}

	/**
	 * 中間表現の構築関数（整形仕様 6.1）
	 * 正規化後の構文だけを読み，原文・コメント・行幅を参照しない
	 * 最上位の文・宣言を行毎に置き，空行の規則を宣言の構造種別から記録する
	 * 計算量：構文・出力要素数 N，トークン長 B，グループ数 G，条件付境界数 C と直接依存数 D に対し O(N + B + (G + C) log(G + 1) + D)
	 * @param NormalizedSyntax 正規化後の構文（以後変更せず検証完了迄共有する期待構文）
	 * @return 幅非依存の中間表現
	 * @throws FormatLimitExceeded 構築・走査又は出力がリソース上限を超えるか，生成形式の不変条件を満たさない場合
	 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
	 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
	 */
	FormatIR Canonicalizer::Build(std::shared_ptr<const SyntaxModel> NormalizedSyntax) {
		// 正規の構築経路の返戻
		return BuildWithWork(std::move(NormalizedSyntax), nullptr);
	}
} // namespace Core
