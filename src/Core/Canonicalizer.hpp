#pragma once

#include "FormatIR.hpp"
#include "SyntaxModel.hpp"
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Core {
	// 正規化した構文から幅非依存の中間表現を生成するクラス（整形仕様 3.1, 6.1）
	class Canonicalizer {
	private:

		enum class BreakSide : uint8_t { None, Head, Tail }; // 字句の折返の側（折らない，字句の前，字句の後）

		// 組立の端の要素（平坦時に字を持つ葉又は行端空白の平坦形と其の字句の節点）
		struct EdgeItem {
			FormatItemId Item = NoItem; // 葉の要素
			NodeId Node = NoNode; // 字句の節点
			char FirstChar = '\0';
			char LastChar = '\0'; // 要素を作る時に記録した両端の字
			bool IsWholeToken = false;
			bool EndsWithNewline = false;
			bool EndsWithSpace = false; // 原文の字句全体か，末尾が改行・空白か
		};

		// 組立済の要素（平坦時の端の要素と必須改行の有無を持つ）
		struct Built {
			FormatItemId Item = NoItem; // 要素
			EdgeItem Head;
			EdgeItem Tail;
			EdgeItem BeforeTail; // 平坦時の先頭・末尾・末尾の直前の要素
			bool HasBreak = false; // 必須改行か改行を含む逐語を含むか
			bool HasCodeBoundary = false; // 埋込コードの境界を持つか
			bool IsCodeOpenOnLine = false; // 最後の行に埋込コードの開きが在るか
		};

		// 空行の規則（前後の宣言の何方かが複数行か，常に置くか）
		struct BlankRule {
			NodeId Before = NoNode; // 前の宣言
			NodeId After = NoNode; // 後の宣言
			bool IsForced = false; // 常に空行を置くか
		};

		// 部分木のヒアドキュメントの開始と本文の数
		struct HeredocSummary {
			uint32_t Begins = 0; // 開始の数
			uint32_t Bodies = 0; // 本文の数
		};

		// 構築中の状態
		struct Builder {
			const SyntaxModel &Syntax; // 読取専用の期待構文
			FormatIR &IR; // 構築中の中間表現
			uint32_t NextGroup = 0; // 次に振る群の番号
			uint32_t NoBreakDepth = 0; // 選択可能な境界を作らない部分木の深さ（前処理指令の行・ヒアドキュメントの開始行・誤り区画・補間を持つ文字列）
			std::unordered_map<NodeId, Built> Flat {}; // 平坦形の控え
			std::unordered_map<NodeId, Built> SealedFlat {}; // 折らない文脈（前処理指令の行・ヒアドキュメントの開始行）の平坦形の控え
			std::unordered_map<NodeId, Built> Structured {}; // 構造化の控え
			std::unordered_map<NodeId, uint8_t> Bits {}; // 判定の控え（展開要因・Kotlin の矢印の後の if 式・分岐列の複数文）
			std::unordered_map<NodeId, uint32_t> ChainGroups {}; // 包装を揃える if 式の連鎖の根毎の共有する群
			std::unordered_map<NodeId, BlankRule> BlankBefore {}; // 節点の前に置く空行の規則
			std::unordered_set<NodeId> BlankContainers {}; // 空行の規則を集めた成員の並び
			std::vector<HeredocSummary> Heredocs {}; // 節点毎の部分木のヒアドキュメントの数（本文を後送する記法だけが持つ）
			std::vector<uint8_t> ErrorParents {}; // 節点毎の誤り区画を直接の子に持つか（誤りを含む構文だけが持つ）
			std::vector<std::coroutine_handle<>> Work {}; // 再開を待つ組立の作業列（整形仕様 9.2）
		};

		template<class Value> class Pending;

		// 作業列で再開する組立の状態（整形仕様 9.2，言語仕様が綴りを定める約束の名前は外部契約として其の儘用いる）
		template<class Value> struct PendingPromise {
			std::vector<std::coroutine_handle<>> *Work = nullptr; // 積む作業列
			std::optional<Value> Result {}; // 組立の結果
			std::exception_ptr Error {}; // 組立中に送出した例外

			/**
			 * 開始前の中断関数（作業列から再開する迄始めない）
			 * @return 常に中断する印
			 */
			std::suspend_always initial_suspend() const noexcept {
				// 中断の返戻
				return {};
			}

			/**
			 * 完了後の中断関数（待つ側が結果を受け取る迄状態を残す）
			 * @return 常に中断する印
			 */
			std::suspend_always final_suspend() const noexcept {
				// 中断の返戻
				return {};
			}

			Pending<Value> get_return_object(); // 組立の取得関数

			/**
			 * 結果の格納関数
			 * @param Completed 組立の結果
			 */
			void return_value(Value Completed) {
				Result.emplace(std::move(Completed));
				// 終了
				return;
			}

			/**
			 * 送出した例外の格納関数（待つ側で送出し直す）
			 */
			void unhandled_exception() noexcept {
				Error = std::current_exception();
				// 終了
				return;
			}

			/**
			 * コンストラクタ（組立の状態の作業列を受け取る）
			 * @param State 構築中の状態
			 * @param Rest 組立の其の他の引数（使わない）
			 */
			template<class ...Arguments> explicit PendingPromise(Builder &State, [[maybe_unused]] const Arguments &...Rest) : Work(
				&State.Work
			) {
				// 終了
				return;
			}
		};

		// 平坦時の端の要素と構文上の選択を記録する中間表現の組立器
		class Emitter {
		private:

			// 追加直後の端の要素
			struct EdgeState {
				EdgeItem Head;
				EdgeItem Tail;
				EdgeItem BeforeTail; // 先頭・末尾・末尾の直前の要素
			};

			FormatIR *Target; // 要素を置く中間表現
			std::vector<FormatItemId> Items; // 並べた要素
			std::vector<std::vector<FormatItemId>> Outer; // 開いた深さの範囲の外側の要素の並び
			EdgeItem Head;
			EdgeItem Tail;
			EdgeItem BeforeTail; // 先頭・末尾・末尾の直前の要素
			std::vector<EdgeState> EdgeHistory; // 追加毎の端の控え
			bool HasHardBreak = false; // 必須改行を出したか
			bool HasCodeBoundary = false; // 埋込コードの境界を持つか
			bool IsCodeOpenOnLine = false; // 最後の行に埋込コードの開きが在るか
			bool HasSoft = false; // 選択可能な境界を出したか
			static void BindConditions(const std::span<const FormatItemId> Targets, const uint32_t Group, FormatIR &IR); // 断片の群の確定関数
			void Push(const FormatItemId Item, const std::string_view Flat, const bool IsBreak); // 要素の追加関数
			void RecomputeTail(); // 末尾を取り消した後の端の再取得関数

		public:

			// 巻戻しの印
			struct Mark {
				size_t ItemCount = 0; // 並べた要素の数
				size_t HistoryCount = 0; // 端の控えの数
				size_t Depth = 0; // 開いた深さの範囲の数
				EdgeItem Head;
				EdgeItem Tail;
				EdgeItem BeforeTail; // 先頭・末尾・末尾の直前の要素
				bool HasHardBreak = false; // 必須改行を出したか
				bool HasCodeBoundary = false; // 埋込コードの境界を持つか
				bool IsCodeOpenOnLine = false; // 最後の行に埋込コードの開きが在るか
				bool HasSoft = false; // 選択可能な境界を出したか
			};

			bool HasInlineCode() const; // 最後の行のコードの開きの判定関数
			bool IsEmpty() const; // 字を持つ要素の無い組立かの判定関数
			bool IsAtLineStart() const; // 末尾が改行かの判定関数
			bool EndsWithBlank() const; // 末尾が空白かの判定関数
			bool TailIs(const SyntaxModel &Syntax, const Role Role, const GeneratedToken Generated = GeneratedToken::None) const; // 末尾の役割の判定関数
			EdgeItem Last() const; // 末尾の要素の取得関数
			EdgeItem Preceding() const; // 末尾の直前の要素の取得関数
			Mark Save() const; // 巻戻しの印の取得関数
			FormatIR &Store() const; // 要素を置く中間表現の取得関数
			void Text(const SyntaxModel &Syntax, const GeneratedToken Token); // 種類で指定した固定字句の追加関数
			void Text(const SyntaxModel &Syntax, const Word Meaning, const bool Before = false, const bool After = false); // 語の追加関数
			void Space(); // 空白の追加関数
			void Separator(const std::string_view Text); // 固定の区切記法の追加関数
			void Gap(const SemanticBoundary &Boundary); // 由来に従い原文の間隙を追加する関数
			void Break(); // 必須改行の追加関数
			void Blank(const NodeId Before, const NodeId After, const bool IsForced); // 空行の規則の追加関数
			void Soft(const std::string_view Flat); // 選択可能な境界の追加関数
			void IfBroken(const std::initializer_list<FormatItemId> Fragment); // 組立の群の展開時だけ出す断片の追加関数
			void Append(Built &&Sub); // 組立済の要素の追加関数
			void PopBack(); // 最後の字の除去関数
			void TrimSpaces(); // 末尾の空白の除去関数
			void OpenScope(); // 深さの範囲の開始関数
			void CloseScope(); // 深さの範囲の終了関数
			void Restore(Mark &&Saved); // 巻戻し関数
			Built Finish(const NodeId Node, const uint32_t Group); // 組立の完了関数

			explicit Emitter(FormatIR &IR); // コンストラクタ（要素を置く中間表現を受け取る）
		};

		// 作業列で再開する組立（整形仕様 9.2，子の組立を明示した作業列へ積み，呼出の段数を入れ子の段数に依らず一定に保つ）
		// 言語仕様が綴りを定める待機の名前（`await_ready` 等）と約束の型の名前 `promise_type` は外部契約として其の儘用いる
		template<class Value> class Pending {
			friend struct PendingPromise<Value>;

		private:

			std::coroutine_handle<PendingPromise<Value>> Handle; // 組立の状態

			/**
			 * コンストラクタ（組立の状態を所有する）
			 * @param Started 組立の状態
			 */
			explicit Pending(const std::coroutine_handle<PendingPromise<Value>> Started) : Handle(Started) {
				// 終了
				return;
			}

		public:

			using promise_type = PendingPromise<Value>; // 言語仕様が約束の型に要求する名前

			/**
			 * 待つ前に完了して居るかの判定関数
			 * @return 常に false（組立は作業列から再開する）
			 */
			bool await_ready() const noexcept {
				// 未完了の返戻
				return false;
			}

			/**
			 * 待つ側を中断して此の組立を作業列へ積む関数
			 * @param Waiting 待つ側の組立（作業列で此の組立の下に残り，完了後に再開する）
			 */
			void await_suspend([[maybe_unused]] const std::coroutine_handle<> Waiting) const {
				Handle.promise().Work->push_back(Handle);
				// 終了
				return;
			}

			/**
			 * 結果の取出関数
			 * @return 組立の結果
			 * @throws std::exception 組立中に送出した例外を其の儘送出し直す
			 */
			Value await_resume() {
				// 組立中に送出した例外の送出し直し
				if(Handle.promise().Error) std::rethrow_exception(Handle.promise().Error);
				// 結果の返戻
				return std::move(*Handle.promise().Result);
			}

			/**
			 * 作業列の実行関数（整形仕様 9.2）
			 * 此の組立を作業列へ積み，最後に積んだ組立を再開して，完了した組立を除けば其の下の待つ側を再開する事を作業列が空に為る迄繰り返す
			 * 計算量：組立の数 K に対し O(K) 回の再開（呼出の段数は入れ子の段数に依らず一定）
			 * @return 此の組立の結果
			 * @throws std::exception 組立中に送出した例外を其の儘送出し直す
			 */
			Value Run() {
				std::vector<std::coroutine_handle<>> &Work = *Handle.promise().Work;
				Work.push_back(Handle);
				while(!Work.empty()) {
					const std::coroutine_handle<> Top = Work.back();
					if(Top.done()) Work.pop_back();
					else Top.resume();
				}
				// 結果の返戻
				return await_resume();
			}

			Pending &operator=(const Pending &) = delete; // コピー代入演算子（禁止）
			Pending &operator=(Pending &&) = delete; // ムーブ代入演算子（禁止）
			Pending(const Pending &) = delete; // コピーコンストラクタ（禁止）

			/**
			 * ムーブコンストラクタ
			 * @param Other 所有を移す組立
			 */
			Pending(Pending &&Other) noexcept : Handle(std::exchange(Other.Handle, {})) {
				// 終了
				return;
			}

			/**
			 * デストラクタ（組立の状態を解放する）
			 */
			~Pending() {
				if(Handle) Handle.destroy();
				// 終了
				return;
			}
		};

		static constexpr uint32_t PendingGroup = NoGroup - 1; // 組立の完了時に群を決める展開時だけ出す断片の印
		static constexpr uint8_t ContainsBit = 0X1; // 展開要因を含む印
		static constexpr uint8_t ContainsComputedBit = 0X2; // 展開要因の算出済の印
		static constexpr uint8_t ArrowValueBit = 0X4; // Kotlin の `->` の後の値の if 式の印
		static constexpr uint8_t ChainMultiBit = 0X8; // 自身と祖先の分岐の節の何れかが複数文の印
		static constexpr uint8_t ChainMultiComputedBit = 0X10; // 分岐列の複数文の算出済の印
		static bool EndsWithOpenRange(const SyntaxModel &Syntax, NodeId Id); // 開いた範囲演算子で終わるかの判定関数
		static bool StartsWithParenType(const SyntaxModel &Syntax, NodeId Id); // `(` で始まる型かの判定関数
		static bool IsGluedBefore(const SyntaxModel &Syntax, const NodeId Parent, const uint32_t Index); // 子の前の境界が原文で密着して居たかの判定関数
		static const SemanticBoundary *PreservedAt(const SyntaxModel &Syntax, const NodeId Parent, const uint32_t Index); // 保持する境界の取得関数
		static const SemanticBoundary *PreservedBefore(const SyntaxModel &Syntax, const NodeId Child); // 子の前の保持する境界の取得関数

		static void AppendPreservedGap(
			const SyntaxModel &Syntax,
			const NodeId Previous,
			const SemanticBoundary &Boundary,
			Emitter &Out
		); // 保持する境界の追加関数

		static const SemanticBoundary *ErrorGapBefore(const SyntaxModel &Syntax, const NodeId Previous, const NodeId Child); // 誤り区画の境界の取得関数
		static void AppendLexicalSeparator(const SyntaxModel &Syntax, const NodeId Child, const EdgeItem Head, Emitter &Out); // 区切記法の追加関数
		static EdgeItem FindEdge(const FormatIR &IR, const FormatItemId Item, const bool First, const NodeId Node = NoNode); // 端の葉の取得関数
		static EdgeItem FindPrecedingEdge(const FormatIR &IR, const FormatItemId Item); // 末尾の直前の葉の取得関数
		static std::string_view GeneratedSpelling(const SyntaxModel &Syntax, const GeneratedToken Token); // 生成字句の綴りの取得関数
		static FormatItemId GeneratedItem(const SyntaxModel &Syntax, const GeneratedToken Token, FormatIR &IR); // 展開時に選ぶ生成字句の要素の追加関数
		static char EdgeChar(const SyntaxModel &Syntax, const FormatIR &IR, const EdgeItem Edge, const bool First); // 端の字句自身の字の取得関数

		static bool EdgeIs(
			const SyntaxModel &Syntax,
			const FormatIR &IR,
			const EdgeItem Edge,
			const Role Role,
			const GeneratedToken Generated = GeneratedToken::None
		); // 端の字句の役割の判定関数

		static bool EdgeIsBracket(const SyntaxModel &Syntax, const FormatIR &IR, const EdgeItem Edge, const bool Opening); // 端の括弧の判定関数
		static bool MergesEdgeTokens(const SyntaxModel &Syntax, const FormatIR &IR, const EdgeItem Before, const EdgeItem After); // 字句の融合の判定関数
		static char PrecedingChar(const SyntaxModel &Syntax, NodeId Id); // 節点の前の字句の最後の字の取得関数
		static char FollowingChar(const SyntaxModel &Syntax, NodeId Id); // 節点の後の字句の最初の字の取得関数
		static bool AttachesNumericSign(const SyntaxModel &Syntax, const NodeId Operand); // Ruby の符号を密着させても読みが変わらない被演算子かの判定関数
		static bool NeedsSpace(const SyntaxModel &Syntax, const NodeId Parent, const NodeId Cur, const NodeId Next); // 字句間の空白の判定関数
		static FormatItemId Anchored(const NodeId Node, const FormatItemId Content, FormatIR &IR); // 節点の錨で包む関数
		static BreakSide SplitSide(const SyntaxModel &Syntax, const NodeId Token, const bool IsFirst); // 字句の折返の側の判定関数
		static bool IsLineJoinedOutside(const SyntaxModel &Syntax, const NodeId Id); // 節点の外側で行が続くかの判定関数
		static std::vector<uint8_t> BreakBoundaries(const SyntaxModel &Syntax, const NodeId Id); // 子の間の選択可能な境界の判定関数
		static std::vector<uint32_t> ScopeClosers(const SyntaxModel &Syntax, const NodeId Id); // 子の括弧の対の閉じの位置の取得関数
		static bool IsHeaderField(const SyntaxModel &Syntax, const NodeId Child); // 見出の部品を指す場の子かの判定関数
		static size_t BodyStatementCount(const SyntaxModel &Syntax, const NodeId Id, const size_t Limit); // 本体の文の数の取得関数
		static bool HasBodyStatement(const SyntaxModel &Syntax, const NodeId Id); // 本体の文を持つかの判定関数
		static bool KeepsKeywordBodyExpanded(const SyntaxModel &Syntax, const NodeId Id); // Ruby の展開を続けるかの判定関数
		static uint32_t RenderedContentCount(const SyntaxModel &Syntax, const NodeId Id); // JSX の内容の子の数の取得関数
		static bool JoinsAcrossNewline(const SyntaxModel &Syntax, const FormatIR &IR, const EdgeItem Head); // 改行を跨いで前の文へ繋がる文の判定関数
		static bool IsAlternateBodyOpener(const SyntaxModel &Syntax, const NodeId Parent, const NodeId Colon); // 代替構文の本体の判定関数
		static bool EndsWithColonBody(const SyntaxModel &Syntax, const NodeId Id); // PHP の代替構文の本体で終わるかの判定関数
		static std::vector<HeredocSummary> SummarizeHeredocs(const SyntaxModel &Syntax); // 節点毎の部分木のヒアドキュメントの数の集計関数
		static void TrackOpenHeredocs(const Builder &State, const NodeId Id, uint32_t &Open); // 本文を控えたヒアドキュメントの開始の数の更新関数
		static FormatItemId LineEdgeSpace(const SyntaxModel &Syntax, const std::string_view Blank, FormatIR &IR); // 行端空白の追加関数
		static Built BuildLeaf(const SyntaxModel &Syntax, const NodeId Id, FormatIR &IR); // 字句の葉の組立関数
		static bool ShouldBreakAfterExpandedBrace(const SyntaxModel &Syntax, const EdgeItem NextHead, const Emitter &Out); // 閉じ括弧後の改行の判定関数
		static Pending<bool> AppendHeredocBody(Builder &State, const NodeId Body, Emitter &Out); // ヒアドキュメントの本体の追加関数
		static void AppendHeredocPendingSeparator(const SyntaxModel &Syntax, Emitter &Out); // 本文を控えたヒアドキュメントの後の文の区切りの追加関数
		static void AppendTrailingSeparator(const SyntaxModel &Syntax, const NodeId Id, Emitter &Out); // 末尾の区切りの追加関数
		static void MarkArrowValueBodies(Builder &State, const NodeId Top); // Kotlin の `->` の後の値の if 式の一括判定関数
		static bool IsArrowValueBody(const Builder &State, const NodeId Id); // Kotlin の `->` の後の値の if 式かの判定関数
		static bool ContainsStmtBlock(Builder &State, const NodeId Id); // 展開要因の包含の判定関数
		static bool AppendCodeBoundary(Builder &State, const NodeId Previous, const NodeId Next, Emitter &Out); // 地の文との境界の追加関数
		static void CollectBlankRules(Builder &State, const NodeId Container, const bool IsRootCall); // 空行の規則の収集関数
		static void AdjustErrorZoneScopes(const SyntaxModel &Syntax, const NodeId Child, Emitter &Out); // 誤り区画の閉じない括弧に依る深さの範囲の開閉関数
		static void AppendChild(Builder &State, const NodeId Child, Built &&Piece, Emitter &Out); // 子の組立済の要素の追加関数
		static bool IsUnbreakable(const Builder &State, const NodeId Id); // 選択可能な境界を作らない区画かの判定関数
		static bool NeedsContinuation(const SyntaxModel &Syntax, const NodeId Id); // 継続用の包装を要する式の判定関数
		static Pending<Built> BuildFlat(Builder &State, const NodeId Id); // 平坦形の組立関数
		static Pending<const Built *> GetFlat(Builder &State, const NodeId Id); // 平坦形の控えの取得関数
		static Pending<Built> TakeFlat(Builder &State, const NodeId Id); // 平坦形の控えの取出関数
		static Pending<Built> BuildSealedFlat(Builder &State, const NodeId Id); // 内部を折らない平坦形の組立関数
		static Pending<Built> BuildExpandedContainer(Builder &State, const NodeId Id); // 展開形の容器の組立関数
		static Pending<Built> BuildMarkup(Builder &State, const NodeId Id); // 文書の表示境界に従う組立関数
		static Pending<Built> BuildKeywordSection(Builder &State, const NodeId Id); // Ruby の節の組立関数
		static Pending<Built> BuildKeywordBlock(Builder &State, const NodeId Id); // Ruby の塊の組立関数

		static Pending<Built> BuildGenericStructure(
			Builder &State,
			const NodeId Id,
			const bool HasStmtBlock,
			const bool ShouldExpand,
			const bool IsElse
		); // 汎用の構造化の組立関数

		static Pending<Built> BuildStructuredCore(Builder &State, const NodeId Id); // 構造化の本体の組立関数
		static Pending<Built> BuildStructured(Builder &State, const NodeId Id); // 構造化の組立関数
		static Pending<const Built *> GetStructured(Builder &State, const NodeId Id); // 構造化の控えの取得関数
		static Pending<Built> TakeStructured(Builder &State, const NodeId Id); // 構造化の控えの取出関数
		static Pending<Built> TakeStatement(Builder &State, const NodeId Id); // 文の組立関数
		static Pending<Built> BuildPreprocBlock(Builder &State, const NodeId Id); // 前処理指令の条件の塊の組立関数

		static Built FinishDeclarationSplit(
			const SyntaxModel &Syntax,
			const NodeId Id,
			Built &&Head,
			Built &&Middle,
			Built &&Tail,
			std::vector<Built> &&Names,
			std::vector<Built> &&Values,
			FormatIR &IR
		); // 分割可能な統合宣言の要素の完了関数

		static Pending<Built> BuildCommaDecl(Builder &State, const NodeId Id); // カンマで並べる統合宣言の組立関数

		static const CanonicalForm *TrailingCommaForm(
			const SyntaxModel &Syntax,
			const NodeId Container,
			const NodeId Last,
			const NodeId Closer,
			const bool IsBroken
		); // 多行の並びの末尾のカンマの生成形式の取得関数

		static bool IsBraceOptionalBody(const SyntaxModel &Syntax, const NodeId Parent, const NodeId Child); // 波括弧を選べる本体の判定関数
		static Built BuildBraceGroup(Builder &State, const NodeId Parent, const NodeId Child, Built &&Body); // 行幅で波括弧を付ける制御本体の群の組立関数
		static Pending<Built> BuildTopLevel(Builder &State, const NodeId Root); // 最上位の組立関数

	public:

		static FormatIR Build(std::shared_ptr<const SyntaxModel> NormalizedSyntax); // 中間表現の構築関数

		Canonicalizer() = delete; // コンストラクタ（禁止）
	};

	/**
	 * 組立の取得関数
	 * @return 此の状態を持つ組立
	 */
	template<class Value> Canonicalizer::Pending<Value> Canonicalizer::PendingPromise<Value>::get_return_object() {
		// 組立の返戻
		return Pending<Value>(std::coroutine_handle<PendingPromise>::from_promise(*this));
	}
} // namespace Core
