#pragma once

#include "FormatIR.hpp"
#include "NodeId.hpp"
#include "SyntaxModel.hpp"
#include "SyntaxRole.hpp"
#include <array>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Core {
	// 正規化した構文から幅非依存の中間表現を生成するクラス（整形仕様 3.1，6.1）
	class Canonicalizer {
	private:

		friend struct CanonicalizerAccess; // 構築毎のフレームの要求数と新規確保数を検査から観測する

		struct SpacingCandidates; // 各照会元の空白規則の候補
		struct SpacingPair; // 記号の組の固定の候補と密着条件

		using SpacingPairRow = std::pmr::vector<SpacingPair>; // 後の記号の定義の番号で引く行
		using SpacingRoleIdx = std::array<SpacingCandidates, static_cast<size_t>(Role::Count)>; // 役割から引く固定のインデックス

		enum class BreakSide : uint8_t { None, Head, Tail }; // トークンの折返位置 // 折返無 // トークンの前 // トークンの後
		enum class SpacingSite : uint8_t { Parent, Cur, Following, CurToken, FollowingToken, FollowingEdge, Count }; // 規則の照会元（ノード・トークン・端の照会の並びを表し，種類別のインデックスの添字にも使う為に順を保つ）

		enum class SpacingRule : uint8_t {
			AttachedPostfix,
			Deconstruction,
			AwaitOperand,
			CodeBoundary,
			AlternateColon,
			IndivisibleType,
			SelectorOperands,
			RenderedContent,
			NumericUnit,
			WordArrayElements,
			EmptyStmt,
			CaptureElements,
			ControlBody,
			GeneratorName,
			IndivisibleSyntax,
			AttributesAndTypes,
			CastBoundary,
			LiteralInterior,
			NamedOperator,
			CompositeBrace,
			NamedAttachment,
			AnnotationArgs,
			LambdaParams,
			NamedCall,
			GenericArgs,
			NamedBracket,
			NamedElements,
			ExceptionStar,
			PostfixPtr,
			PtrDeclarator,
			PrefixOperator,
			PostfixOperator,
			ConditionalColon,
			RangeFusion,
			OptionalAndRange,
			BinaryOperands,
			TagInterior,
			TightAssignment,
			DestructorName,
			SpacedAssignment,
			AttachedSeparator,
			AfterSeparator,
			BraceInterior,
			PipeDelimiter,
			BeforeColon,
			KeywordAndInt,
			MemberAccess,
			ArrowOperator,
			TypeBody,
			BodyBrace,
			ValCall,
			TypeQuery,
			DeclParen,
			KeywordParen,
			UnnamedAttachment,
			OrdinaryElements, // 通常の代入又は読捨の値の区切
			Count
		}; // 意味保存・構文の区切・融合・通常の名前の順で選ぶ空白規則

		// 記号の組だけで決まる候補と密着の条件（親・名前付の条件は境界で照合する）
		struct SpacingPair {
			uint64_t Candidates = 0; // 前後のトークン全体が選ぶ候補の和
			bool IsCurAttached = false; // 前の無名トークンに常に密着するか
			bool IsFollowingAttached = false; // 後の無名トークンへ常に密着するか
			bool IsKnown = false; // 此の記号の組を合成済か
		};

		// １つの定義が各照会元で選ぶ規則（同じ順位の定義は１つの switch の分岐だけを共有する）
		struct SpacingCandidates {
			std::array<uint64_t, static_cast<size_t>(SpacingSite::Count)> Masks {}; // 固定の順位を位とした候補
			size_t PairIndex = 0; // 記号の組の表の番号（定義無は０）
		};

		// 組立の端の要素（平坦時に字を持つ葉又は行端空白の平坦形と其のトークンのノード）
		struct EdgeItem {
			FormatItemId Item = NoItem; // 葉の要素
			NodeId Node = NoNode; // トークンのノード
			// 要素を作る時に記録した両端の字
			unsigned char FirstChar = '\0';
			unsigned char LastChar = '\0';
			// 原文のトークン全体と末尾の改行・空白の事実
			bool IsWholeToken = false;
			bool DoesEndWithNewline = false;
			bool DoesEndWithSpace = false;
			bool ShouldBreakAfter = false; // ヒアドキュメントの終端として後続のトークンの前に改行を要するか
		};

		// 空行の規則（前後の宣言の何方かが複数行か，常に置くか）
		struct BlankRule {
			NodeId Before = NoNode; // 前の宣言
			NodeId After = NoNode; // 後の宣言
			bool IsForced = false; // 常に空行を置くか
		};

		struct BuiltSlot; // 構造化の控えを置く再使用可能な枠

		// ノード毎に共有する子列と外側の継続の要約
		struct NodeCtx {
			BuiltSlot *Structured = nullptr; // 此のノードの構造化の控え（未使用は無）
			const SpacingCandidates *SpacingTokenRules = nullptr; // トークン全体が選ぶ空白候補
			const SpacingCandidates *SpacingLeadingRules = nullptr; // 先頭の端が選ぶ空白候補
			const SpacingCandidates *SpacingKind = nullptr; // ノードの種別から１度引いた変更不能な空白候補
			const KindDef *SpacingToken = nullptr; // トークンの綴りから１度照会した定義
			BlankRule *BlankBefore = nullptr; // 此のノードの前に置く空行の規則
			uint32_t ClosingGroup = NoGroup; // 自身の閉トークンを折るグループ（無ければ番兵）
			uint32_t ChainGroup = NoGroup; // 連鎖で共有するグループ（未登録は番兵）
			uint32_t ScopeCloser = NoDetail; // 同じ親の開きに対応する閉じの子番号（対を持たなければ番兵）
			LexicalEdge FirstEdge; // 部分木の先頭のトークン
			LexicalEdge LastEdge; // 部分木の末尾のトークン
			NodeId SingleToken = NoNode; // 内容が１つの非空のトークンだけなら其のノード
			NodeId FirstPathLeaf = NoNode; // 最初の子だけを辿る経路の末端
			NodeId LastPathLeaf = NoNode; // 最後の子だけを辿る経路の末端
			uint32_t ContentCount = 0; // 文の役割を持つトークンも含む直接の内容子数
			uint32_t NamedCount = 0; // 名前付の直接の子数
			uint32_t BracketContents = 0; // 括弧の中の名前付の内容子数
			uint32_t FirstNamed = 0; // 最初の名前付の子の位置
			uint32_t LastNamed = 0; // 最後の名前付の子の位置
			NodeId ArrowChainRoot = NoNode; // else if の連鎖の根
			uint8_t Bits = 0; // 展開要因の判定の控え
			uint8_t BodyStmtCount = 3; // 本体の文数（０・１・２以上，３は未照会）
			unsigned char Preceding = '\0'; // 所有範囲の直前のトークンの字
			unsigned char Following = '\0'; // 所有範囲の直後のトークンの字
			bool IsContinuationBlocked = false; // 外側の式又は括弧が継続を担うか
			bool IsLineJoined = false; // 此のノードの外側で行が続くか
			bool IsMemberSequence : 1; // メンバ又はアクセサを要素として並べる本体か
			bool HasStatementBody : 1; // 文を並べる本体を自身又は子孫に持つか
			bool HasAccessSection : 1; // 子列にアクセスセクションの見出を持つか
			bool HasKeywordClose : 1; // 直接の無名の子に本体を閉じる語を持つか
			bool HasBlankRules : 1; // 此のコンテナの空行の規則を収集済か
			bool IsSpacingTokenKnown : 1; // トークン全体を照会したか
			bool HasSpacingTokenWork : 1; // 空白の本文照会の作業量を計上済か
			bool HasPreceding : 1; // 祖先から継ぐ直前の字を確定済か
			bool HasFollowing : 1; // 祖先から継ぐ直後の字を確定済か
			bool IsLeadingParenType : 1; // 名前付の先頭経路に括弧で始まる型を持つか
			bool HasBraceOpen : 1; // オブジェクトの直接の無名の開波括弧を持つか
			bool HasBraceClose : 1; // オブジェクトの直接の無名の閉波括弧を持つか
			bool HasParenOpen : 1; // 一括宣言の直接の無名の開括弧を持つか
			bool HasColon : 1; // コロンで始める本体の直接の無名のコロンを持つか
			bool HasDirectError : 1; // エラー領域を直接の子に持つか
			bool HasErrorChild : 1; // エラーを含む名前付の直接の子を持つか
			bool HasRenderedChild : 1; // JSX の内容を持つ名前付の直接の子を持つか
			bool HasElseChild : 1; // else 系の節を直接の子に持つか
			bool HasElseGrandchild : 1; // else 系の節を孫に持つか
			bool HasExceptionChild : 1; // rescue 又は ensure の直接の子を持つか
			bool ShouldWrap : 1; // 此の式に継続括弧を選ぶか
			bool IsAnnotation : 1; // 本体の境界より内側の型注釈か
			bool IsTypeUnion : 1; // 二項式と値の括弧だけを介して型のフィールドに属するか
			bool IsOptionalBody : 1; // 親の生成形式が幅で波括弧を付ける本体か
			bool HasFollowingClause : 1; // 最後の制御本体が外側の後続節へ接続するか
			bool IsArrowVal : 1; // `->` の後の値としてラッパーを揃える `if` 式か

			NodeCtx() noexcept; // コンストラクタ（文脈のフラグを既定値にする）
		};

		// 組立済の要素（平坦時の端の要素と必須改行の有無を持つ）
		struct Built {
			FormatItemId Item = NoItem; // 要素
			std::vector<FormatItemId> Parts = {}; // アンカーと選択を持たない並び（親の子列へ直接移す）
			// 平坦時の先頭・末尾・末尾の直前の要素
			EdgeItem Head;
			EdgeItem Tail;
			EdgeItem BeforeTail;
			bool HasBreak = false; // 必須改行か改行を含む逐語を含むか
			bool HasClauseBoundary = false; // 末尾が後続節へ接続する本体の境界か
			bool HasExpandedKeywordTail = false; // 最後の語で区切る節の本体を複数文で展開したか
			bool HasCodeBoundary = false; // 埋込コードの境界を持つか
			bool IsCodeOpenOnLine = false; // 最後の行に埋込コードの開きが在るか
		};

		// 控えの参照を保持した儘，一定数の枠を纏めて確保する
		struct BuiltSlot {
			Built Value; // 構造化の控え
			BuiltSlot *Next = nullptr; // 取出済の次の空き枠
		};

		// 展開要因の後行走査で再使用する作業枠
		struct ExpansionFrame {
			NodeId Id; // 対象のノード
			uint32_t Next = 0; // 次に検査する子の番号
			bool IsReady = false; // 自身の展開要因を判定済か
			bool IsResult = false; // 展開要因を含むか
		};

		// 部分木のヒアドキュメントの開始と本文の数
		struct HeredocSummary {
			uint32_t Begins = 0; // 開始の数
			uint32_t Bodies = 0; // 本文の数
			bool HasPattern = false; // 部分木が照合の節を含むか
		};

		// 完了した組立だけを外し，結果の所有者が破棄する迄中断する
		struct FinalPause : std::suspend_always {
			template<class Promise> void await_suspend(const std::coroutine_handle<Promise> Completed) const noexcept; // 完了した組立を作業列から外す関数
		};

		// 同じ大きさの解放済フレームを構築単位で再使用する作業領域
		class FramePool {
		private:

			struct alignas(std::max_align_t) Header {
				Header *Next; // 同じサイズの次の空きフレーム
				Header **FreeList; // 再ハッシュでも位置を保つ，同じサイズの空き列の先頭
			};

			std::unordered_map<size_t, Header *> Free; // サイズ毎の空きフレームの先頭
			std::vector<Header *> Blocks; // 一括解放する全ての確保済フレーム

		public:

			size_t Requests = 0; // フレームの要求数
			size_t Allocations = 0; // 新規のフレームの確保数
			static void Release(void *const Buffer) noexcept; // 所有者の空き列へフレームを戻す関数
			void *Allocate(const size_t Size); // 同じサイズのフレームを再使用する確保関数

			FramePool &operator=(const FramePool &) = delete; // コピー代入演算子（禁止）
			FramePool(const FramePool &) = delete; // コピーコンストラクタ（禁止）
			FramePool() = default; // コンストラクタ（空の作業領域）

			~FramePool(); // デストラクタ（此の構築のフレームを全て解放する）
		};

		// 構築中の状態
		struct Builder {
			static constexpr size_t StructuredBlockSize = 64; // 参照の位置を保って一括確保する控えの枠数
			const SyntaxModel &Syntax; // 読取専用の期待構文
			FormatIR &IR; // 構築中の中間表現
			std::deque<BlankRule> BlankRules {}; // 空行の規則を持つノードだけの，位置を保つ共有領域
			std::vector<NodeCtx> Contexts {}; // 構築の走査で対応付けたノード毎の要約
			std::array<const KindDef *, 256> SpacingChars {}; // 同じ文法の１バイトの綴りが持つ端の定義
			std::array<bool, 256> IsSpacingCharKnown {}; // 端の字の定義を照会済か（定義無も共有する）
			std::array<std::byte, 4096> SpacingBuffer {}; // 小さな定義表を個別確保せず保持する領域
			std::pmr::monotonic_buffer_resource SpacingMemory { SpacingBuffer.data(), SpacingBuffer.size() }; // 定義表の一括解放
			std::pmr::unordered_map<const KindDef *, SpacingCandidates> SpacingKinds { &SpacingMemory }; // 定義毎の空白候補
			std::pmr::vector<SpacingPairRow> SpacingPairs { &SpacingMemory }; // 定義の番号で直接引く記号の組の表
			uint32_t NextGroup = 0; // 次に振るグループの番号
			uint32_t NoBreakDepth = 0; // 選択可能な境界を作らない部分木の深さ（プリプロセッサディレクティブの行・ヒアドキュメントの開始行・エラー領域・観測されるテキスト・改行を許さない補間）
			std::vector<std::unique_ptr<std::array<BuiltSlot, StructuredBlockSize>>> StructuredBlocks {}; // 参照の位置を保つ控えの領域
			BuiltSlot *FreeStructured = nullptr; // 取出済の枠を繋ぐ空き列
			size_t NextStructured = StructuredBlockSize; // 最後の領域の次の未使用の枠
			std::pmr::unsynchronized_pool_resource BoundaryMemory {}; // 入子の候補列を別々に保ち，解放済の領域を再使用する
			std::vector<ExpansionFrame> ExpansionStack {}; // 展開要因の走査毎に再使用する作業列
			std::vector<HeredocSummary> Heredocs {}; // ノード毎の部分木のヒアドキュメントの数（本文を後送する記法だけが持つ）
			FramePool Frames {}; // 此の構築だけで所有するフレームの作業領域
			std::vector<std::coroutine_handle<>> Work {}; // 再開を待つ組立の作業列（整形仕様 9.2）
		};

		// 作業列で再開する組立の前方宣言
		template<class Val> class Pending;

		// 作業列で再開する組立の状態（整形仕様 9.2，言語仕様が綴りを定める約束の名前は外部契約として其の儘用いる）
		template<class Val> struct PendingPromise {
			std::vector<std::coroutine_handle<>> *Work = nullptr; // 積む作業列
			std::optional<Val> Result {}; // 組立の結果
			std::exception_ptr Error {}; // 組立中に送出した例外
			std::suspend_always initial_suspend() const noexcept; // 開始前の中断関数（作業列から再開する迄始めない）
			FinalPause final_suspend() const noexcept; // 完了後の中断関数（待つ側が結果を受け取る迄状態を残す）
			Pending<Val> get_return_object(); // 組立の取得関数
			void return_value(Val Completed); // 結果の格納関数
			void unhandled_exception() noexcept; // 送出した例外の格納関数（待つ側で送出し直す）

			/**
			 * 構築中の作業領域からコルーチンのフレームを確保する関数
			 * 計算量：作業領域の確保費用 Q に対し O(Q)
			 * @tparam Args
			 * @param Size コンパイラが要求するフレームのバイト数
			 * @param State 構築中の状態
			 * @param Rest コルーチンの其の他の引数（使わない）
			 * @return 再使用又は新規確保したフレーム
			 * @throws std::bad_alloc フレーム又は管理用のメモリを確保出来ない場合
			 * @throws std::length_error 管理用の列の要素数が上限を超える場合
			 */
			template<class ...Args> static void *operator new(const size_t Size, Builder &State, [[maybe_unused]] const Args &...Rest) {
				// 此の構築のフレームの返戻
				return State.Frames.Allocate(Size);
			}

			static void operator delete(void *const Buffer) noexcept; // 破棄済のフレームを作業領域へ戻す関数

			/**
			 * コンストラクタ（組立の状態の作業列を受け取る）
			 * @tparam Args
			 * @param State 構築中の状態
			 * @param Rest 組立の其の他の引数（使わない）
			 */
			template<class ...Args> explicit PendingPromise(Builder &State, [[maybe_unused]] const Args &...Rest) : Work(&State.Work) {
				// 終了
				return;
			}
		};

		// 平坦時の端の要素と構文上の選択を記録する中間表現のビルダー
		class Emitter {
		private:

			FormatIR *Target; // 要素を置く中間表現
			std::array<FormatItemId, 4> InlineItems {}; // 短い並びは個別に確保せず保持する
			size_t InlineCount = 0; // 直接保持する要素数（連続領域を使う時は０）
			std::vector<FormatItemId> Items; // 並べた要素
			std::vector<std::vector<FormatItemId>> Outer; // 開いた深さの範囲の外側の要素の並び
			// 先頭・末尾・末尾の直前の要素
			EdgeItem Head;
			EdgeItem Tail;
			EdgeItem BeforeTail;
			bool HasPendingSpace = false; // 後続の内容を待つ空白
			bool HasClauseBoundary = false; // 末尾の本体が後続節の境界を持つか
			bool HasHardBreak = false; // 必須改行を出したか
			bool HasCodeBoundary = false; // 埋込コードの境界を持つか
			bool IsCodeOpenOnLine = false; // 最後の行に埋込コードの開きが在るか
			bool HasSoft = false; // 選択可能な境界を出したか
			bool HasPendingConditions = false; // 此の組立で所属グループが未確定の条件付断片を出したか
			static size_t BindConditions(FormatIR &IR, const std::span<FormatItemId> Targets, const uint32_t Group); // 断片のグループの確定関数
			std::span<FormatItemId> GetItems(); // 要素の並びの取得関数
			void PushItem(const FormatItemId Item); // 並びの末尾への要素の追加関数
			void Push(const FormatItemId Item, const std::string_view Flat, const bool IsBreak); // 要素の追加関数
			void FlushSpace(); // 後続が確定した空白の生成関数

		public:

			FormatIR &GetStore() const; // 要素を置く中間表現の取得関数
			bool HasInlineCode() const; // 最後の行のコードの開きの判定関数
			bool IsEmpty() const; // 字を持つ要素の無い組立かの判定関数
			bool IsAtLineStart() const; // 末尾が改行かの判定関数
			bool HasTrailingSpace() const; // 末尾が空白かの判定関数
			bool IsTailRole(const SyntaxModel &Syntax, const Role Role, const GeneratedToken Generated = GeneratedToken::None) const; // 末尾の役割の判定関数
			EdgeItem GetLast() const; // 末尾の要素の取得関数
			EdgeItem GetPreceding() const; // 末尾の直前の要素の取得関数
			void GetText(const SyntaxModel &Syntax, const GeneratedToken Token); // 種類で指定した固定トークンの追加関数

			void GetText(
				const SyntaxModel &Syntax,
				const Word Meaning,
				const bool ShouldSpaceBefore = false,
				const bool ShouldSpaceAfter = false
			); // 語の追加関数

			void Space(); // 空白の追加関数
			void Separator(const std::string_view Text); // 固定の区切記法の追加関数
			void Break(); // 必須改行の追加関数
			void Gap(const SemanticBoundary &Boundary); // 由来に従い原文の間隙を追加する関数
			void Blank(const NodeId Before, const NodeId After, const bool IsForced); // 空行の規則の追加関数
			void Soft(const std::string_view Flat); // 選択可能な境界の追加関数
			void IfBroken(const std::initializer_list<FormatItemId> Fragment, const bool IsActivatedWhenFlat = false); // グループの選択形だけ出す断片の追加関数
			void Append(Built &&Subtree); // 組立済の要素の追加関数
			void OmitSpace(); // 後続へ密着させる為の未生成の空白の省略関数
			void OpenScope(); // 深さの範囲の開始関数
			void CloseScope(); // 深さの範囲の終了関数
			Built Finish(const NodeId Node, const uint32_t Group); // 組立の完了関数

			explicit Emitter(FormatIR &IR); // コンストラクタ（要素を置く中間表現を受け取る）
		};

		// 作業列で再開する組立（整形仕様 9.2，子の組立を明示した作業列へ積み，呼出の段数をネストの段数に依らず一定に保つ）
		// 言語仕様が綴りを定める待機の名前（`await_ready` 等）と約束の型の名前 `promise_type` は外部契約として其の儘用いる
		template<class Val> class Pending {
		private:

			friend struct PendingPromise<Val>;

			std::coroutine_handle<PendingPromise<Val>> Handle; // 組立の状態

			explicit Pending(const std::coroutine_handle<PendingPromise<Val>> Started); // コンストラクタ（組立の状態を所有する）

		public:

			using promise_type = PendingPromise<Val>; // 言語仕様が約束の型に要求する名前

			bool await_ready() const noexcept; // 待つ前に完了して居るかの判定関数
			void await_suspend([[maybe_unused]] const std::coroutine_handle<> Waiting) const; // 待つ側を中断して此の組立を作業列へ積む関数
			Val await_resume(); // 結果の取出関数
			Val Run(); // 作業列の実行関数（整形仕様 9.2）

			Pending &operator=(const Pending &) = delete; // コピー代入演算子（禁止）
			Pending &operator=(Pending &&) = delete; // ムーブ代入演算子（禁止）
			Pending(const Pending &) = delete; // コピーコンストラクタ（禁止）

			Pending(Pending &&Other) noexcept; // ムーブコンストラクタ
			~Pending(); // デストラクタ（組立の状態を解放する）
		};

		static constexpr uint32_t PendingGroup = NoGroup - 1; // 組立の完了時に所属グループを決める条件付断片のマーカー
		static constexpr uint8_t ContainsBit = 0X1; // 展開要因を含むマーカー
		static constexpr uint8_t ContainsComputedBit = 0X2; // 展開要因の算出済のマーカー
		static bool IsHeaderField(const SyntaxModel &Syntax, const NodeId Child); // 見出の部品を指す場の子かの判定関数
		static std::string_view GetGeneratedSpelling(const SyntaxModel &Syntax, const GeneratedToken Token); // 生成トークンの綴りの取得関数
		static size_t GetBodyStmtCount(Builder &State, const NodeId Id); // 本体の文の数の取得関数
		static const KindDef *GetSpacingTokenOf(Builder &State, const NodeId Id); // トークンの綴りの定義の取得関数
		static const SpacingRoleIdx &GetSpacingRoles(); // 役割毎の空白規則の固定インデックスの取得関数
		static bool IsGluedBefore(const SyntaxModel &Syntax, const NodeId Parent, const uint32_t Idx); // 子の前の境界が原文で密着して居たかの判定関数

		static bool IsEdgeRole(
			const SyntaxModel &Syntax,
			const FormatIR &IR,
			const EdgeItem Edge,
			const Role Role,
			const GeneratedToken Generated = GeneratedToken::None
		); // 端のトークンの役割の判定関数

		static bool IsBlockClose(const SyntaxModel &Syntax, const FormatIR &IR, const EdgeItem Edge); // 本体のブロックを閉じる端かの判定関数
		static bool CanMergeEdgeTokens(const SyntaxModel &Syntax, const FormatIR &IR, const EdgeItem Before, const EdgeItem After); // トークンの融合の判定関数
		static bool HasBodyStmt(Builder &State, const NodeId Id); // 本体の文を持つかの判定関数
		static const SemanticBoundary *GetPreservedAt(const SyntaxModel &Syntax, const NodeId Parent, const uint32_t Idx); // 保持する境界の取得関数
		static EdgeItem FindEdge(const FormatIR &IR, const FormatItemId Item, const bool IsFirst, const NodeId Node = NoNode); // 端の葉の取得関数
		static EdgeItem FindPrecedingEdge(const FormatIR &IR, const FormatItemId Item); // 末尾の直前の葉の取得関数
		static bool HasCodeOpeningSuffix(const std::string_view Text, const std::string_view Suffix); // コードの開きに直結する接尾部の判定関数
		static unsigned char GetAdjacentChar(Builder &State, const NodeId Id, const bool IsBefore); // 所有範囲の前後のトークンの字の取得関数
		static bool HasSingleTokenRole(const Builder &State, const NodeId Id, const Role Wanted); // 単独のトークンを包む部分木の役割の判定関数
		static bool IsRole(const KindDef *const Def, const Role Wanted); // 定義が役割を持つかの判定関数
		static const KindDef *GetBreakTokenOf(Builder &State, const NodeId Id); // 折返で使うトークンの定義の取得関数
		static BreakSide SplitSide(const SyntaxModel &Syntax, const NodeId Id, const KindDef *const Def, const bool IsFirst); // 折返の側
		static const KindDef *GetSpacingEdgeOf(Builder &State, const LexicalEdge Edge, const KindDef *const KnownToken); // 端のトークンの定義の取得関数
		static const SpacingCandidates &GetSpacingRulesOf(Builder &State, const KindDef *const Kind); // 該当する規則の取得関数

		static const SpacingCandidates &GetNodeSpacingRules(
			Builder &State,
			const KindDef *const Kind,
			const SpacingCandidates *&Cached
		); // ノードに対応付けた空白候補の取得関数

		static const SpacingPair &GetSpacingPairOf(
			Builder &State,
			const KindDef *const Cur,
			const KindDef *const Following,
			const SpacingCandidates &CurRules,
			const SpacingCandidates &FollowingRules
		); // 記号の組の固定の候補と密着条件の取得関数

		static bool HasTrailingOpenRange(const Builder &State, const NodeId Id); // 開いた範囲演算子で終わるかの判定関数
		static bool HasLeadingParenType(const Builder &State, const NodeId Id); // `(` で始まる型かの判定関数
		static bool CanAttachNumericSign(const SyntaxModel &Syntax, const NodeId Operand); // Ruby の符号を密着させても読みが変わらない被演算子かの判定関数
		static bool IsArrowValBody(const Builder &State, const NodeId Id); // Kotlin の `->` の後の値の if 式かの判定関数
		static FormatItemId GeneratedItem(FormatIR &IR, const std::string_view Spelling, const GeneratedToken Token); // 取得済の綴りから生成トークンの要素を追加する関数
		static FormatItemId GeneratedItem(const SyntaxModel &Syntax, FormatIR &IR, const GeneratedToken Token); // 展開時に選ぶ生成トークンの要素の追加関数
		static bool IsEdgeBracket(const SyntaxModel &Syntax, const FormatIR &IR, const EdgeItem Edge, const bool IsOpening); // 端の括弧の判定関数
		static void AdjustErrorZoneScopes(const SyntaxModel &Syntax, const NodeId Child, Emitter &Output); // エラー領域の閉じない括弧に依る深さの範囲の開閉関数
		static void AppendLexicalSeparator(const SyntaxModel &Syntax, const NodeId Child, const EdgeItem Head, Emitter &Output); // 区切記法の追加関数
		static bool ShouldExpandKeywordBody(Builder &State, const NodeId Id); // Ruby の展開を続けるかの判定関数
		static const SemanticBoundary *GetPreservedBefore(const SyntaxModel &Syntax, const NodeId Child); // 子の前の保持する境界の取得関数
		static Built BuildLeaf(const SyntaxModel &Syntax, FormatIR &IR, const NodeId Id); // トークンの葉の組立関数
		static FormatItemId GetItemOf(FormatIR &IR, Built &Piece); // 独立した選択部分の要素の取得関数
		static FormatItemId Anchored(FormatIR &IR, const NodeId Node, const FormatItemId Content); // ノードのアンカーで包む関数
		static void TrackOpenHeredocs(const Builder &State, const NodeId Id, uint32_t &Open); // 本文を控えたヒアドキュメントの開始の数の更新関数
		static bool IsUnbreakable(const Builder &State, const NodeId Id); // 選択可能な境界を作らない領域かの判定関数
		static bool CanBreakTightBoundary(const SyntaxModel &Syntax, const NodeId Id, const uint32_t Idx); // 密着する括弧内で改行が合法かの判定関数
		static std::pmr::vector<uint8_t> BreakBoundaries(Builder &State, const NodeId Id); // 子の間の選択可能な境界の判定関数
		static void AppendPreservedGap(const SyntaxModel &Syntax, const NodeId Prev, const SemanticBoundary &Boundary, Emitter &Output); // 保持する境界の追加関数

		static bool ShouldInsertSpace(
			Builder &State,
			const NodeId Parent,
			const NodeId CurNode,
			const NodeId Next,
			const SemanticBoundary *const Boundary
		); // トークン間の空白の判定関数

		static const CanonicalForm *TrailingCommaForm(
			const SyntaxModel &Syntax,
			const NodeId Container,
			const NodeId Last,
			const NodeId Closer,
			const bool IsBroken
		); // 多行の並びの末尾のカンマの生成形式の取得関数

		static Built BuildBraceGroup(Builder &State, const NodeId Parent, const NodeId Child, Built &&Body); // 行幅で波括弧を付ける制御本体のグループの組立関数
		static bool ShouldBreakAfterExpandedBrace(const SyntaxModel &Syntax, const EdgeItem NextHead, const Emitter &Output); // 閉括弧後の改行の判定関数
		static void AppendHeredocPendingSeparator(const SyntaxModel &Syntax, Emitter &Output); // 本文を控えたヒアドキュメントの後の文の区切の追加関数
		static void AppendChild(Builder &State, const NodeId Child, Built &&Piece, Emitter &Output); // 子の組立済の要素の追加関数
		static void AppendKeywordBoundary(Builder &State, Emitter &Output); // 独立した節境界の追加関数
		static unsigned char GetEdgeChar(const FormatIR &IR, const EdgeItem Edge, const bool IsFirst); // 端のトークン自身の字の取得関数
		static bool IsAlternateBodyOpener(const SyntaxModel &Syntax, const NodeId Parent, const NodeId Colon); // 代替構文の本体の判定関数
		static bool HasProcessedCodeBoundary(Builder &State, const NodeId Prev, const NodeId Next, Emitter &Output); // 地の文との境界の追加関数
		static bool HasTrailingColonBody(const Builder &State, const NodeId Id); // PHP の代替構文の本体で終わるかの判定関数
		static bool HasExpansionReason(Builder &State, const NodeId Id); // 展開要因の包含の判定関数
		static const SemanticBoundary *GetErrorGapBefore(const SyntaxModel &Syntax, const NodeId Prev, const NodeId Child); // エラー領域の境界の取得関数
		static bool CanJoinAcrossNewline(const SyntaxModel &Syntax, const FormatIR &IR, const EdgeItem Head); // 改行を跨いで前の文へ繋がる文の判定関数
		static void AppendTrailingSeparator(const SyntaxModel &Syntax, const NodeId Id, Emitter &Output); // 末尾の区切の追加関数

		static Built FinishDeclSplit(
			const SyntaxModel &Syntax,
			FormatIR &IR,
			const NodeId Id,
			Built &&Head,
			Built &&Middle,
			Built &&Tail,
			std::vector<Built> &&Names,
			std::vector<Built> &&Vals
		); // 分割可能な統合宣言の要素の完了関数

		static Pending<Built> BuildHeredocBody(Builder &State, const NodeId Body); // ヒアドキュメントの本体の組立関数
		static Pending<Built> BuildFlat(Builder &State, const NodeId Id); // 平坦形の組立関数
		static Pending<Built> BuildSealedFlat(Builder &State, const NodeId Id); // 内部を折らない平坦形の組立関数
		static Pending<Built> BuildExpandedContainer(Builder &State, const NodeId Id); // 展開形のコンテナの組立関数
		static Pending<Built> BuildMarkup(Builder &State, const NodeId Id); // ドキュメントの表示境界に従う組立関数

		static Pending<Built> BuildKeywordSection(
			Builder &State,
			const NodeId Id,
			const bool HasLeadingBoundary,
			const bool ShouldBreakBefore
		); // Ruby の節と其の独立した先頭境界の組立関数

		static Pending<Built> BuildKeywordBlock(Builder &State, const NodeId Id); // Ruby のブロックの組立関数

		static Pending<Built> BuildGenericStructure(
			Builder &State,
			const NodeId Id,
			const bool HasStmtBlock,
			const bool ShouldExpand,
			const bool IsElse
		); // 汎用の構造化の組立関数

		static Pending<Built> BuildStructuredCore(Builder &State, const NodeId Id); // 構造化の本体の組立関数
		static Pending<Built> BuildStructured(Builder &State, const NodeId Id); // 構造化の組立関数
		static const Built *StoreStructured(Builder &State, const NodeId Id, Built &&Piece); // 構造化の控えの格納関数
		static Pending<const Built *> GetStructured(Builder &State, const NodeId Id); // 構造化の控えの取得関数
		static Built ReleaseStructured(Builder &State, const NodeId Id); // 使用済の構造化の控えの取出関数
		static Pending<Built> TakeStoredStructured(Builder &State, const NodeId Id); // 構造化の控えを作業列で取り出す関数
		static Pending<Built> TakeStructured(Builder &State, const NodeId Id); // 構造化の控えの取出関数
		static Pending<Built> TakeStmt(Builder &State, const NodeId Id); // 文の組立関数
		static Pending<Built> BuildPreprocBlock(Builder &State, const NodeId Id); // プリプロセッサディレクティブの条件のブロックの組立関数
		static bool HasMultipleDeclarators(const SyntaxModel &Syntax, const NodeId Id); // 併記した宣言子が在るかの判定関数
		static bool CanSplitDecl(const SyntaxModel &Syntax, const CommaDecl &Shape); // 宣言の分割で束縛が変わらないかの判定関数
		static bool IsEmptyStmt(const SyntaxModel &Syntax, const NodeId Id); // セミコロンだけの空文かの判定関数
		static Pending<Built> BuildCommaDecl(Builder &State, const NodeId Id, const CommaDecl &Shape); // カンマで並べる統合宣言の組立関数
		static bool IsBraceOptionalBody(const Builder &State, const CanonicalForm &Form, const NodeId Parent, const NodeId Child); // 波括弧を選べる本体の判定関数
		static void StoreBlankRule(Builder &State, const NodeId Id, const BlankRule Rule); // ノードの空行の規則を共有領域へ記録する関数
		static void CollectBlankRules(Builder &State, const NodeId Container, const bool IsRootCall); // 空行の規則の収集関数
		static void PrepareContexts(Builder &State); // 子列と継続の文脈の共有要約の構築関数
		static Pending<Built> BuildTopLevel(Builder &State, const NodeId Root); // 最上位の組立関数
		static FormatIR BuildWithWork(std::shared_ptr<const SyntaxModel> NormalizedSyntax, std::pair<size_t, size_t> *const FrameWork); // フレームの要求数と新規確保数を観測する構築関数

	public:

		static FormatIR Build(std::shared_ptr<const SyntaxModel> NormalizedSyntax); // 中間表現の構築関数

		Canonicalizer() = delete; // コンストラクタ（禁止）
	};
} // namespace Core
