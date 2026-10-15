#pragma once

#include "BindingSources.hpp"
#include "NodeId.hpp"
#include "SyntaxRole.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class LaidOutCode;
class SyntaxValidator;

namespace Core {
	class SyntaxModel;

	enum class BoundaryKind : uint8_t {
		Glued,
		NoBreak,
		NoInlineSpace,
		Preserved,
		RequiredSpace,
		Separated,
		Spaced,
		TokenGlued,
		TokenSpaced
	}; // 子の間の意味を持つ境界の種類（整形仕様 3.2）

	enum class PreservedSrc : uint8_t {
		ErrorBoundary,
		ErrorContent,
		HiddenContent,
		LiteralContent,
		MacroTokens,
		RenderedLineEdges
	}; // 原文の間隙をコピーする理由（トークン値とマクロの間隙は完全一致，エラー領域の外縁と JSX の行端はインデントを通常の配置へ委ねる）

	enum class TokenSpacingMode : uint8_t { None, Punctuation, Tokens, Whitespace }; // トークン列で保持する境界の意味

	// 子の間の意味を持つ境界（表示桁・コメント本文を持たず，Preserved の領域だけ原文の間隙を持つ）
	struct SemanticBoundary {
		uint32_t Before = 0; // 境界の後に続く子の番号
		BoundaryKind Kind = BoundaryKind::Glued; // 境界の種類
		PreservedSrc Src = PreservedSrc::ErrorBoundary; // 保持する間隙の由来
		std::string Gap {}; // コピーする原文の間隙（`Preserved` だけ，空白とパーサーが読み飛ばしたトークン）

		SemanticBoundary(const uint32_t Before, const BoundaryKind Kind); // テキストを持たない境界のコンストラクタ

		SemanticBoundary(
			const uint32_t Before,
			const BoundaryKind Kind,
			std::string Gap,
			const PreservedSrc Src = PreservedSrc::ErrorBoundary
		); // 保持するテキストと由来を持つ境界のコンストラクタ

		SemanticBoundary() = default; // 空の境界のコンストラクタ
	};

	enum class ConstraintKind : uint8_t {
		ContainsError, // 投影で修復しなかったパーサーのエラーを部分木に含むノード
		AmbiguousDecl, // 式と区別出来ない型名と宣言子を持つ宣言
		DeclaratorMacro, // 宣言子の記号へ展開するマクロを含む型（宣言子の間で共有すると記号の適用先が変わる）
		DetachedElse, // 前処理の条件のブロックを挟んでパーサーが繋げなかった `else` が後に続く文と其の右端の文（波括弧の増減で `else` の結付が変わる）
		DirectiveZone, // プリプロセッサディレクティブで始まるエラー領域（条件のブロックの境界を保つ為に隣の文との間を改行する）
		EndsCode, // 後の原文を処理系がデータとして読む文（終端のトークンを補うと保持するデータが変わる）
		CallArguments, // 呼出の実引数を囲む括弧（外せない）
		ValueSequence, // 型引数ではなく値をカンマで区切る並び
		MacroBinding, // 入力の定義・取消と条件枝からマクロ置換が介在し得る
		MacroSensitiveParen, // 展開後の意味を保つ括弧
		NeedsArgParens, // 区切の無い実引数の先頭で演算子と紛れるトークン
		NoOptionalBreak, // 構文内の任意の改行で読みが変わるノード
		StmtMacro, // 複数の文や開いた `if` へ展開するマクロの呼出の文（単一の文の本体に置くと先頭の文だけが本体に為る）
		ValuedMacro, // 置換本体を持つマクロの定義（静的解析が定数の候補として読む）
		Verbatim, // 子へ降りずテキストを其の儘保つ領域
		UncertainMacroArguments, // 未読のインクルード等に依って実引数の評価が未確定の呼出先
		Count // 種類の数（番兵）
	}; // ノードの意味上の制約の種類（整形仕様 3.2, 4.2） // （各値が制約の集合のビット位置を表す為，順を保つ）

	static_assert(static_cast<uint8_t>(ConstraintKind::Count) < 17); // 制約の集合は２バイトに収める

	// 子の番号順に並ぶ意味を持つ境界の並び（大半のノードが持たない為，持つノードだけが領域を確保する）
	class SemanticBoundaryList {
	private:

		std::unique_ptr<std::vector<SemanticBoundary>> Entries; // 境界（持たなければ無）

	public:

		std::span<const SemanticBoundary> GetView() const; // 境界の並びの取得関数
		std::span<const SemanticBoundary>::iterator begin() const; // 境界の並びの先頭の取得関数
		std::span<const SemanticBoundary>::iterator end() const; // 境界の並びの終端の取得関数
		std::vector<SemanticBoundary> &Edit(); // 変更する境界の並びの取得関数（持たなければ空の並びを確保する）
		SemanticBoundaryList &operator=(const SemanticBoundaryList &Other); // コピー代入演算子

		SemanticBoundaryList &operator=(SemanticBoundaryList &&Other) = default; // ムーブ代入演算子

		explicit SemanticBoundaryList(std::vector<SemanticBoundary> &&Boundaries); // コンストラクタ（空の並びは領域を確保しない）
		SemanticBoundaryList(const SemanticBoundaryList &Other); // コピーコンストラクタ

		SemanticBoundaryList(SemanticBoundaryList &&Other) = default; // ムーブコンストラクタ
		SemanticBoundaryList() = default; // コンストラクタ（空）
	};

	// 所有領域（整形仕様 4.4，親の中核へ渡す子のコードだけで，コメントと検証期待は別の所有経路）
	struct OwnedRegion {
		std::shared_ptr<const LaidOutCode> Code; // 子が確定した断片・アンカー・配置枠・終端契約（完成テキストを持たない）
	};

	// 名前のテキストを初回だけ走査した事実
	struct NameSpelling {
		bool HasNonAscii = false; // 非 ASCII のバイトを含むか
		bool HasEscape = false; // エスケープの開始記号を含むか
	};

	struct ReferenceFacts {
		std::string Name; // 確認した供給元の名前
		WordSet Meanings {}; // 解決後の標準対象の役割
		bool IsUnknown = false; // 束縛を確認出来ないか
		WordSet UnboundMeanings {}; // 入力側が確認した未束縛時の役割
		NodeId BindingHead = NoNode; // 解決に使った先頭の名前
		NodeId BindingDecl = NoNode; // 投影時の供給元の宣言
		std::string BindingMember {}; // 入力側が分解したメンバの修飾
		bool IsGlobal = false; // 大域の束縛を使う名前か
	}; // 投影で確定した参照先の事実

	// ノードの構文事実（整形仕様 3.2，文境界・必須の空白・改行の禁止・トークンの密着を表し，原文の位置を持たない）
	struct SyntaxFacts {
		SemanticBoundaryList Boundaries; // 子の番号順に並ぶ意味を持つ境界
		CanonicalFormList Forms {}; // 文法の変更不能な定義を共有する生成形式
		uint32_t BlankKey = 0; // 宣言・ディレクティブ全体の構造種別の同異だけを表す空行比較の値
		uint16_t Constraints = 0; // ノードの意味上の制約の集合（制約の種類毎のビットマスク）
		bool DoesDiscardBinding : 1; // 名前の位置に在っても束縛を導入しない識別子
		bool CanReassignExistingBinding : 1; // 既存の束縛への代入を含み得る宣言
		bool NeedsAngleParens : 1; // 型引数の境界と演算子の読みを区別する括弧か
		bool HasFalseName : 1; // 使用位置の偽の名前が組込の定数と同じ値を表すか
		bool HasTrueName : 1; // 使用位置の真の名前が組込の定数と同じ値を表すか
		bool DoesAbsorbLeadingComment : 1; // 直前の独立コメントをドキュメント・前文として取り込む宣言か（Go のドキュメントを持つ宣言と cgo のインポート）
		bool IsKeywordName : 1; // 文脈から演算子でなく識別子と確定した語
		bool DoesObserveText : 1; // 式のテキスト自体が実行時に表示される領域か
		bool DoesRequireInlineCode : 1; // 囲むリテラルの記法がコード内の改行も許さない補間か
		bool DoesPreserveQuoteForm : 1; // 引用の変更で文法又はエンコーディング宣言の読みを変え得るトークンか
		std::shared_ptr<const ReferenceFacts> Reference {}; // 解決済の参照先（入力側だけが作る）
		WordSet ResultMeanings {}; // 供給元を確認した戻値型の候補
		uint8_t CallEffects = 0; // 束縛と再代入を確認した呼出効果
		bool IsRegisteredMacro = false; // 標準マクロへ解決したか
		bool CannotResolveTerminalMember = false; // 終端先の候補から除いたメンバか
		WordSet Meanings {}; // 投影又は生成で確定した語の意味
		AccessMeaning Access {}; // 入力で確定した可視性の指定
		const KindDef *TokenDef = nullptr; // 投影又は生成形式で確定したトークンの役割
		TokenSpacingMode TokenSpacing = TokenSpacingMode::None; // 保護するトークン列の空白の意味（親から継承済）
		const KindDef *DelimiterDef = nullptr; // 補間の可変長の区切に対応する固定トークンの意味
		std::optional<NameSpelling> Spelling {}; // 葉の名前の投影済のテキストの事実（未確定又は変更後は無）
		bool HasBuiltinCastType = false; // 完成候補の変換先が組込型だけで構成されるか
		NumericLiteralKind NumericLiteral = NumericLiteralKind::Other; // 完成候補の投影で確定した数値の分類
		ConstantTruth Truth = ConstantTruth::Unknown; // 入力で確定したリテラルの真偽（値を保つ書換で共有）
		std::optional<bool> HasCodeNewline; // コメントを除いた逐語コードに改行を含むか（未確定又は変更後は無）
		uint32_t NameIdx = 0; // 投影時に確定した識別子の比較用の名前の番号（構文モデルの名前の表の位置に１を足した値で，無ければ０）
		NodeId ImportScope = NoNode; // 関数インポートと宣言が属する名前空間の領域（分離を要しない文法では無）
		uint32_t RegionIdx = 0; // 所有領域とする埋込の整形結果の番号（構文モデルの領域の表の位置に１を足した値で，無ければ０）
		bool Has(const ConstraintKind Kind) const; // 制約の有無の判定関数
		const SemanticBoundary *GetBoundaryBefore(const uint32_t Idx) const; // 子の前の意味を持つ境界の取得関数
		void Add(const ConstraintKind Kind); // 制約の追加関数

		SyntaxFacts() noexcept; // コンストラクタ（構文のフラグを既定値にする）
	};

	// ノードの種別（同じ構文上の性質を持つノードで定義を共有する）
	struct SyntaxKind {
		const KindDef *Def = nullptr; // 共通のノード種別
		bool IsNamed() const; // 名前付のノードかの判定関数
	};

	// 現在の親子関係から継承する束縛の所属
	struct BindingLocation {
		NodeId Scope = NoNode; // 最内の有効範囲（無は未取得）
		NodeId Owner = NoNode; // 最外の関数又は独立した項目
		NodeId Module = NoNode; // 関数の探索を区切る最内のモジュール
	};

	// 独自のノード表の要素（整形仕様 3.2，トークンの葉と逐語領域だけがテキストを持つ）
	struct SyntaxNode {
		mutable uint64_t OrderFirst = 0; // 前順の番号（零は未採番）
		mutable uint64_t OrderLast = 0; // 後順の番号（部分木の区間の終端）
		NodeId Parent = NoNode; // 親（根は無）
		uint32_t Idx = 0; // 親の子の並びの中の位置
		SyntaxKind Kind; // ノードの種別
		std::string Lexeme; // トークンの葉・逐語領域のデコード済のテキスト（其の他のノードは空）
		SyntaxFacts Facts; // 構文事実
		std::pmr::vector<NodeId> Children; // 子の並び（格納先のモデルがメモリを所有する）
		uint64_t Fields = 0; // 親の中の子の役割の集合（隠れた規則の内外の役割を併せ持つ）

		SyntaxNode(std::pmr::memory_resource *const Storage, SyntaxNode &&Node); // 子列を指定メモリへ移す生成関数

		SyntaxNode() = default; // 独立した一時ノードの生成関数
	};

	// 構造照合が読むノードの値と所有者の表への参照
	struct ValidationNode {
		SyntaxKind Kind; // 固定の種別
		uint64_t Fields = 0; // 親の中の場
		std::string_view Lexeme; // 所有者が保持する葉の本文
		std::span<const NodeId> Children; // 所有者が保持する子列
	};

	// 部分木の端に在るトークンの葉と其の端の字
	struct LexicalEdge {
		NodeId Leaf = NoNode; // 端のトークンの葉
		unsigned char Char = '\0'; // 葉の端の字
	};

	// アンカーの行先（移る先のノードと，其のノードの前後の何方のアンカーに為るか）
	struct AnchorTarget {
		NodeId Node = NoNode; // 移る先のノード
		bool IsAfter = false; // 移る先のノードの後のアンカーに為るか
		bool IsEmptyBody = false; // 空に畳む本体の中へ移すコメントか
		bool IsCaseBody = false; // 空に為る `case` 本体の見出の後へ移すコメントか
	};

	// アンカーの移譲（整形仕様 3.2，消える又は移るノードの先行・末尾・内部のアンカーの行先）
	struct AnchorTransfer {
		NodeId From = NoNode; // 消える又は移るノード
		AnchorTarget Leading; // 先行のアンカーの行先
		AnchorTarget Trailing; // 末尾のアンカーの行先
		AnchorTarget Inner; // 内部のアンカーの行先
	};

	enum class AnchorSide : uint8_t { Before, After, Inside }; // アンカーに対する記録の置方（移譲の行先の選択に使い，コメントの配置に対応する） // アンカーの前 // アンカーの後 // アンカーの内部
	enum class TokenReset : uint8_t { Preserve, AllInsideAnchor, None }; // アンカーとの間のトークンの数の置換 // 保つ // アンカーの中の全てのトークンの後 // 間にトークンを置かない

	// 合成したアンカーの移譲の行先（整形仕様 7.2，移譲の連鎖を正規化の中で合成し，コメント側は１度だけ引く）
	struct AnchorMove {
		NodeId Node = NoNode; // 最終のアンカー（行先を持たない置方は無）
		AnchorSide Side = AnchorSide::Before; // 最終の置方
		bool IsEmptyBody = false; // 空に畳む本体の中へ移したか
		bool IsRemovedCaseBody = false; // 空に為る `case` 本体の見出の後へ移したか
		TokenReset Reset = TokenReset::Preserve; // アンカーとの間のトークンの数の置換
	};

	// ノードの統合・除去・分割の記録（整形仕様 3.2）
	struct RewriteTrace {
		std::vector<AnchorTransfer> Transfers; // アンカーの移譲の並び（適用順，合成前）
		std::unordered_map<uint64_t, AnchorMove> Moves; // 元のアンカー・置方・文中かの組毎の合成した行先
		static uint64_t GetMoveKey(const NodeId Anchor, const AnchorSide Side, const bool IsInline); // 合成した行先のキーの取得関数
		void Compose(const SyntaxModel &Syntax); // 移譲の連鎖の合成関数
	};

	enum class RewriteKind : uint8_t {
		Insert, // 生成するノードの追加
		Move, // 識別子とアンカーを保ったノードの移動
		RebuildChildren, // 子列の一括組立
		Remove, // ノードの除去
		RemoveSiblings, // 同じ親の複数の子の一括除去
		Reshape, // 識別子を保った種別・子・テキストの組直
		Retarget, // ノードを除かないアンカーの移譲
		SetLexeme, // トークンのテキストの置換
		Unwrap, // ラッパーの単一の中身を親へ移す除去
		Wrap // 対象の位置・フィールドへラッパーを加える操作
	}; // 書換の操作の種類

	// 規則から生成するノード（既存のノードを移す位置は其の識別子を持つ）
	struct GeneratedNode {
		std::string Kind {}; // 名前の無いトークンの種別を引くテキスト（`Lexeme` を省く場合は生成トークンのテキストにも使う）
		bool IsNamed = false; // 名前付のノードか
		std::vector<GeneratedNode> Children {}; // 子
		uint64_t Fields = 0; // 親の中の子の役割の集合（`SyntaxModel::GetFieldBit` のビットマスク）
		NodeId Existing = NoNode; // 此の位置へ移す既存のノード（生成するノードは無）
		std::string Lexeme {}; // 子を持たないトークンのテキスト（空ならコピー元のテキスト，生成するトークンの `Kind` の順に従い，何れも無い組直は元のテキストを保つ）
		FormPart Def {}; // 生成形式が与える種別と構文事実（種別無なら名前の無いトークンのテキストで引く）
		SyntaxFacts Facts {}; // 生成後に必要な構文事実（コピーは元の事実を保持する）
		NodeId Src = NoNode; // コピー又は雛形の元のノード（葉のテキスト・事実の継承・子の境界の組直に使う）
	};

	// 書換の１操作
	struct RewriteStep {
		RewriteKind Kind = RewriteKind::SetLexeme; // 操作の種類
		NodeId Target = NoNode; // 対象のノード（`Insert` は加える先の親）
		std::string Text {}; // 置換後のテキスト (`SetLexeme`)
		AnchorTransfer Transfer {}; // 除くノードのアンカーの行先（`Remove`，`Wrap` は包むノードを `From` に置くと外のアンカーを括弧の前後へ移す）
		std::vector<AnchorTransfer> RemovedSiblings {}; // 同じ親から纏めて除くノードと各アンカーの行先 (`RemoveSiblings`)
		uint32_t Idx = 0; // 加える又は移す先の子の位置 (`Insert` / `Move`)
		BoundaryKind Boundary = BoundaryKind::Glued; // 加える又は移すノードの前の境界（`Insert` / `Move`，隣接を記録した親だけ）
		GeneratedNode Inserted {}; // 加えるノード (`Insert` / `Wrap` / `Reshape`)
		NodeId Dst = NoNode; // 移す先の親 (`Move`)
		std::optional<uint64_t> DstFields = std::nullopt; // 移す先でのフィールド（`Move`，無指定なら元のフィールドを保持）
		std::optional<SemanticBoundary> RecordedBoundary = std::nullopt; // 元の子の前から移す境界（間隙のテキストと由来を含む）
	};

	// 利用集合を必要としない名前の導入の事実
	struct BindingIntroduction {
		bool IsDecl = false; // 名前の導入か
		BindingNamespace Namespace = BindingNamespace::Val; // 導入の名前空間
		bool IsUncertain = false; // 導入又は可視性を確定出来ないか
	};

	// 現在の構造で解決した名前の束縛（導入名のノードを同一性とし，捕捉も同じノードを参照する）
	struct BindingFact {
		NodeId Decl = NoNode; // 導入名（暗黙の導入は所有ノード，未解決なら無）
		NodeId Scope = NoNode; // 導入する有効範囲
		bool IsDecl = false; // 名前の導入か
		bool IsWrite = false; // 初期化でない再代入か
		bool IsCapture = false; // 関数の範囲を越える利用か
		bool IsUncertain = false; // 動的な探索・暗黙の仮引数・未確定の受手等に依存するか
		BindingNamespace Namespace = BindingNamespace::Val; // 解決した名前空間（型の導入を値の参照へ混ぜない）
		std::shared_ptr<const CallableIndex> Sources {}; // 呼出の供給元を指す間の所有
		const CallableIntroduction *Source = nullptr; // 現在の呼出の供給元
		bool HasCustomDefinition = false; // 独自定義が標準の契約を隠すか
		bool IsDeclaredConstant = false; // 導入した宣言に定数指定が在るか
		bool HasMemberChanges = false; // 先頭名のメンバへの明示的な変更を含むか
	};

	// 現在の導入位置が供給する値（値で返戻し，可変の親子構造を保存しない）
	struct BindingValSrc {
		NodeId Val = NoNode; // 宣言子の初期値（無い場合又は仮引数なら無）
		bool IsParam = false; // 呼出又は例外捕捉から値を受け取る仮引数か
		bool IsNonconstant = false; // 供給の規則だけで参照が非定数と確定するか（初期値の評価とは別）
	};

	// 書換の計画（整形仕様 3.2，構文正規化が作り `SyntaxModel::Apply` が適用する）
	struct RewritePlan {
		std::vector<RewriteStep> Steps; // 操作の並び（適用順）
	};

	// カンマで並べる宣言の並び（宣言子，又は Go の名前・初期化子）
	struct DeclList {
		NodeId Host = NoNode; // 要素を並べるノード
		std::vector<std::vector<NodeId>> Items {}; // 要素毎の子の並び（Swift の宣言子は名前の型式・型のアノテーション・`=`・初期化子）
		ChildRole SeparatorField = ChildRole::None; // 並びの生成形式が定める区切のフィールド
		size_t LastPiece = SIZE_MAX; // 所属する最後の部品の位置（空なら無）
	};

	// カンマで並べる宣言の子（宣言と並びのノードの子を原文順に辿った物，要素の間の `,` を除く）
	struct DeclPiece {
		NodeId Node = NoNode; // 子
		int32_t List = -1; // 属する並びの番号（宣言子で共有する子は -1）
		uint32_t Item = 0; // 並びの中の要素の番号
	};

	// 宣言の共有指定の構造の比較値
	struct DeclShape {
		const KindDef *Kind = nullptr; // 固定の種別
		uint64_t Fields = 0; // 親の中のフィールド
		std::string_view Lexeme {}; // 既存のテキストへの参照
		uint32_t Children = 0; // 前行順の構造を区切る子数
		WordSet Meanings {}; // 宣言の共有指定が持つ語の意味

		bool operator==(const DeclShape &Other) const = default; // 共有指定の一致判定関数
	};

	// カンマで並べる宣言の部品（整形仕様 5.6，宣言子毎に型・初期化子を持つ宣言は宣言子の並び，Go は名前の並びと初期化子の並びを持つ）
	struct CommaDecl {
		NodeId Node = NoNode; // 宣言
		std::vector<DeclShape> Key {}; // 共有する宣言指定の構造の比較値
		std::vector<DeclList> Lists {}; // 宣言子又は名前の並びと，初期化子の並び（原文順）
		std::vector<DeclPiece> Pieces {}; // 宣言の子（原文順）
	};

	class SourceProjector;

	// 正規化と中間表現の構築で共有する作業予算
	struct StructureWorkAccount {
		size_t Base = 0; // 上限の固定費を含まない投影直後のノード・本文の基準量
		size_t Limit = std::numeric_limits<size_t>::max(); // 投影直後の基準量から定めた上限
		size_t Used = 0; // 計上済の作業単位
		bool IsClosed = false; // 中間表現の構築を終えたか
		void Charge(const size_t Amount); // 構造の実作業の計上関数
	};

	// 制御の流れの照会先の前方宣言
	class ControlFlow;

	// コメントの有無に依らない定義のドキュメント化の構文事実
	struct DocumentationFact {
		NodeId Anchor = NoNode; // 定義又は先行する属性・ラッパーのアンカー
		bool IsDocumentable = false; // 定義の直前へドキュメントを置けるか
	};

	// 書換と共に失効する構文正規化の事実
	struct NormalizationFacts {
	private:

		friend class SyntaxModel;
		friend class SyntaxNormalizer;
		friend struct SyntaxNormalizerAccess;

		std::unordered_map<NodeId, bool> ReturnAttributeProtection; // 返戻の補完の規則内で共有する属性の保護
		std::shared_ptr<ControlFlow> FlowFacts {}; // 所有者と正規化工程だけが更新する制御の流れ

	public:

		using RefFact = std::shared_ptr<const void>; // 基準側の照会結果の所有型

		// 型引数の読分けに関わる部分木のトークン
		struct AngleFact {
			bool HasConflict = false; // 型引数の開始と紛れるか
			bool HasClose = false; // 無名の閉じの山括弧候補を含むか
			uint32_t LastCloseChild = 0; // 候補を含む最後の子の位置（`HasClose` が真の時だけ使う）
		};

		struct TypeEdgeFact {
			NodeId Edge = NoNode; // 括弧を透過した型の右端
			bool HasCallable = false; // 右端への経路に関数型が在るか
		}; // 型の括弧の除去では変わらない右端の要約

		// 括弧の判定で再利用するトークンの端と接続
		struct ParenScanMemo {
			std::unordered_map<NodeId, NodeId> After {}; // ノードの後に続くトークン
			std::unordered_map<NodeId, NodeId> BlockLead {}; // 本体に見える式の先頭
			std::unordered_map<NodeId, NodeId> BlockTop {}; // 本体に見える式を包む最上位のノード
			std::unordered_map<NodeId, NodeId> FirstLeaf {}; // 部分木の先頭のトークン
			std::unordered_map<NodeId, NodeId> OperatorTop {}; // 演算子を共有する最上位のノード
			std::unordered_map<NodeId, NodeId> LastLeaf {}; // 部分木の末尾のトークン
			std::unordered_map<NodeId, bool> ConditionalReceivers {}; // 後置連鎖に取り込めない短絡の受手
			std::unordered_map<NodeId, bool> ConstructionCalls {}; // 構築の受手へ取り込めない呼出・短絡
			std::unordered_map<NodeId, bool> LeadingAwait {}; // 後置連鎖の先頭の待機演算
			std::unordered_map<NodeId, bool> PostfixAtoms {}; // 後置連鎖の原子式の判定
			std::unordered_map<NodeId, NodeId> PostfixLead {}; // 括弧を透過した後置連鎖の先頭
			std::unordered_map<NodeId, bool> DeclArgs {}; // 宣言子・仮引数にも読める実引数の構造か
			std::unordered_map<NodeId, bool> DeclCalls {}; // 単一実引数の呼出の祖先が式の文へ続くか
			std::unordered_map<NodeId, bool> LeftConstraint {}; // 左側の文脈が括弧を必要とするか
			std::unordered_map<NodeId, TypeEdgeFact> TypeEdges {}; // 型の右端と関数型の経路
			std::unordered_map<NodeId, bool> RightJump {}; // 右側に結付を変える脱出式が在るか
		};

		// 値の利用の事実
		struct ValFact {
			bool IsUsed = false; // 値が結果として使われるか
		};

		// 対象自身を含むエラーと保護の障壁
		struct NormalizationBarrier {
			bool HasError = false; // 此のノードを含むエラー障壁
			bool HasTextBarrier = false; // マクロ又は観測されるテキストの障壁
			bool DoesRequireInlineCode = false; // 補間の文区切を改行へ置き換えれない範囲
			bool IsQuoted = false; // 此のノードを含む引用本文の保持範囲
			bool IsProtected = false; // 此のノードを含む保護障壁
		};

		// 括弧の判定で再利用する構造の文脈
		struct ParenCtx {
			std::unordered_map<NodeId, int> Prec {}; // ノード毎の演算子の優先順位
			std::unordered_map<NodeId, AngleFact> Angles {}; // 部分木の山括弧と最後の閉じの位置
			std::unordered_map<NodeId, bool> BraceHeaders {}; // 波括弧の本体を持つ見出か
			std::unordered_map<NodeId, bool> UnnamedAncestors {}; // 名前の無い祖先に包まれて居るか
			ParenScanMemo Scan {}; // トークンの端と接続の共有控え
		};

		std::vector<NodeId> BarrierPath {}; // 障壁の未取得の祖先を辿る再使用の作業列
		std::vector<RefFact> RefFacts {}; // 基準側の参照の保持
		std::unordered_map<NodeId, ValFact> ValFacts {}; // 値の利用位置
		std::unordered_map<NodeId, NormalizationBarrier> NormalizationBarriers {}; // エラー・保護障壁
		std::optional<ParenCtx> Parens {}; // 括弧の共有判定
	};

	enum class WalkStep : uint8_t { Descend, Skip, Stop }; // 前行順の走査の各ノードでの次の動作 // 子へ降りる // 子へ降りない // 走査を止める

	// 独自のノード表と構文事実を持つ構文モデル（整形仕様 3.2，tree-sitter の木の別名ではない）
	class SyntaxModel {
	private:

		friend class CoreObservation;
		friend class SourceProjector;
		friend class ReferenceProjection;
		friend class ControlFlow;
		friend class SignatureExtractor;
		friend class SyntaxNormalizer;
		friend struct FormatIR;
		friend class ::SyntaxValidator;
		friend struct SyntaxNormalizerAccess;

		using ParamScopeIdx = std::unordered_multimap<NodeId, NodeId>; // 暗黙仮引数の供給元の型毎の其れを読む有効範囲
		using RefFact = std::shared_ptr<const void>; // 基準側の照会結果の所有型

		enum class BindingVisibility : uint8_t { Lexical, ConstructorMember, Init, Receiver }; // 現在の構造から判定する名前の可視範囲と探索段階 // ローカル探索で解決する名前 // コンストラクタ頭部では仮引数，本体では暗黙の受手のメンバ // 非メンバのコンストラクタ仮引数（初期化中だけローカル名） // 暗黙の受手のメンバ（ローカル探索後に解決）

		// 呼出先の構造を子から共有する要約
		struct CalleeSummary {
			NodeId Head = NoNode; // 名前付の先頭の葉
			size_t TextSize = 0; // 子のトークンを連結した長さ
			bool HasCall = false; // 呼出の結果を含むか
			bool HasMacroBinding = false; // 呼出先の構造に未展開のマクロ置換を含むか
		};

		// １つの範囲が導入する名前（宣言の順序と開始位置を現在の構造から読む）
		struct BindingDecl {
			NodeId Name = NoNode; // 導入名
			NodeId Site = NoNode; // 宣言又は仮引数
			bool IsOrdered = false; // 宣言の後から見えるか
			bool IsFunctionScoped = false; // 関数の範囲で再宣言が同じ束縛を共有するか
			NodeId VisibleWithin = NoNode; // 本体だけへ導入する反復名の可視範囲
			BindingVisibility Visibility = BindingVisibility::Lexical; // 名前の可視範囲と探索段階
			bool IsConstant = false; // 宣言の定数指定
		};

		// 同じ宣言列から解決した可視性（宣言表の寿命内だけで参照する）
		struct BindingSelection {
			const BindingDecl *Visible = nullptr; // 可視のローカル宣言
			const BindingDecl *Member = nullptr; // 可視の受手のメンバ
			bool IsShared = true; // ローカル宣言を共有出来るか
			bool IsSharedMember = true; // 受手のメンバを共有出来るか
			bool IsDecl = false; // 照会した名前自身の導入か
		};

		// 同じ綴りの宣言列の順序と自己導入，範囲全体で有効な宣言のインデックス
		struct VisibleDecls {
			std::vector<size_t> Lexical; // ローカル探索で見える宣言の位置
			std::vector<size_t> Members; // 受手のメンバの宣言の位置
			std::vector<size_t> NonFunctionScoped; // 関数の範囲で共有しない宣言の位置
			std::vector<size_t> NonShadowItems; // 関数宣言以外の位置
		};

		struct DeclIdx {
			std::unordered_map<NodeId, size_t> Introductions; // 自己導入のノードから宣言列の位置への対応
			std::vector<size_t> Ordered; // 順序付の宣言の位置
			std::vector<size_t> SiteFirst; // 同じ宣言に属する先頭の位置
			std::vector<BindingSyntax> Sites; // 宣言元の構文分類
			std::array<size_t, 8> Contexts {}; // ３つの真偽毎の可視宣言の位置（０は未照会，１は最初の区分，２以降は共有表の位置と２の和）
			std::optional<VisibleDecls> FirstContext; // 最初の区分は個別に確保せず保持する
			NodeId LoopBody = NoNode; // 反復の本体
			bool HasLoop = false; // 反復名を含むか
			bool HasInit = false; // 初期化中だけ見える名前を含むか
			bool HasConstructor = false; // コンストラクタの頭部と本体で所属が変わる名前を含むか
		};

		// 関数領域から最上位の照会へ共有する利用集合
		struct BindingUses {
			std::unordered_set<NodeId> Writes {}; // 捕捉先への再代入
			std::unordered_set<NodeId> Refs {}; // 捕捉先の利用
			bool IsUncertain = false; // 未確定な作用を含むか
		};

		// 有効範囲の名前のインデックス（名前の導入と可視性の変更迄保持する）
		struct BindingScope {
			mutable std::unordered_map<uint64_t, DeclIdx> DeclIndexes {}; // 名前空間と登録名毎の順序と有限な可視区分（宣言列と共に失効）
			mutable std::vector<VisibleDecls> VisibleContexts {}; // ２区分目以降の可視宣言だけを宣言列と共に保持する表
			std::unordered_map<uint32_t, std::vector<BindingDecl>> Names {}; // 値の導入名毎の宣言
			std::unordered_map<uint32_t, std::vector<BindingDecl>> TypeNames {}; // 型・モジュールの導入名毎の宣言
			std::shared_ptr<const CallableIndex> Sources {}; // 同じ宣言収集で確定する呼出の供給元
			NodeId SourceOwner = NoNode; // 別名の解決を共有する束縛の領域
			NodeId ParamOwner = NoNode; // 暗黙仮引数の供給元の型（現在の構造から取得）
			bool HasImplicitParam = false; // 暗黙の仮引数を確定出来ないか
			bool HasUnknownReceiver = false; // 型に依存する受手が名前を供給し得るか
			bool HasSeparateParams = false; // 仮引数の既定値と本体の `var` が異なる環境か
			bool HasUncertainNames = false; // 導入名のパターンを完全には解決出来ないか
		};

		// 最外の関数と，其の外側の範囲毎の参照・捕捉・書込の事実
		struct BindingRegion {
			mutable std::optional<BindingUses> Captures {}; // 此の領域を捕捉する全関数の作用（必要時に１度収集）
			mutable std::unordered_map<uint64_t, std::unordered_map<std::string, BindingFact>> SourceFacts {}; // 名前・探索の起点・修飾毎の供給元の事実
			std::unordered_map<NodeId, BindingFact> Facts {}; // 名前毎の解決結果
			std::unordered_set<NodeId> Writes {}; // 再代入される導入名
			std::unordered_set<NodeId> Refs {}; // 導入名を除いて利用される束縛
			std::vector<NodeId> Dependencies {}; // 所有者から根迄の構造上の依存（変更範囲から逆引する）
			std::vector<NodeId> Functions {}; // 根から辿った最外の関数（根だけが持つ）
			bool IsUncertain = false; // 解決出来ない書込先を含むか
		};

		// 照合専用の表（各列の終端は次のノードの開始位置で表す）
		struct ValidationRecord {
			uint64_t OrderFirst = 0; // 接続済の構文の前順
			uint64_t OrderLast = 0; // 接続済の構文の後順
			SyntaxKind Kind; // 固定の種別
			WordSet Meanings {}; // 照合でも共有する語の意味
			const KindDef *TokenDef = nullptr; // 投影又は生成形式で確定したトークンの役割
			const KindDef *DelimiterDef = nullptr; // 照合でも共有する補間の区切の意味
			uint64_t Fields = 0; // 親の中の場
			NodeId Parent = NoNode; // 親
			uint32_t ChildrenBegin = 0; // 子列の開始
			uint32_t LexemeBegin = 0; // 葉の本文の開始
			uint32_t BoundariesBegin = 0; // 意味境界の開始
		};

		// 元の構文を解放した後も照合結果と参照先を所有する表
		struct ValidationStorage {
			struct Declaration {
				std::optional<CommaDecl> Parts; // 元の宣言の部品
				std::exception_ptr Error; // 部品を照会した時に再送出する失敗
			};

			struct Generation {
				GeneratedNode Node; // 選択した生成部品
				std::exception_ptr Error; // 部品を照会した時に再送出する失敗
			};

			std::vector<ValidationRecord> Records; // 元の識別子順のノードと列の終端の番兵
			std::vector<CanonicalFormList> Forms; // 末尾カンマを選んだ時だけ保持する直接参照の列
			std::vector<uint32_t> SiblingIndexes; // メンバ区切を選んだ時だけ保持する兄弟位置
			std::vector<uint8_t> Regions; // 埋込を持つ時だけ保持する所有領域（無は零，本文有は１，空は２）
			std::vector<NodeId> Children; // 全ノードの子列
			std::string Lexemes; // 全ノードの葉の本文
			std::vector<SemanticBoundary> Boundaries; // 全ノードの意味境界
			std::deque<std::string> DeclarationText; // 宣言の比較キーが参照する本文
			std::unordered_map<NodeId, Declaration> Declarations; // 選択した宣言区分だけの部品
			std::unordered_map<uint64_t, Generation> Generations; // ノードと操作の組毎の生成部品
		};

		// ノード表の確保と子列のプールを計数する所有者（移動元が再使用されても確保を同期する）
		struct NodeMemory : std::pmr::memory_resource {
			// 子列の個別解放を所有者の一括解放へ纏めるリソース
			struct ChildMemory : std::pmr::memory_resource {
				NodeMemory &Owner; // ノード表と共に生存する所有者
				bool do_is_equal(const std::pmr::memory_resource &Other) const noexcept override; // リソースの同一性の判定関数
				void *do_allocate(const size_t Bytes, const size_t Alignment) override; // 同期した共有メモリからの確保関数
				void do_deallocate(void *const Pointer, const size_t Bytes, const size_t Alignment) override; // 個別解放を遅延する関数

				explicit ChildMemory(NodeMemory &Owner); // 所有者を保持する生成関数
			};

			std::mutex Mutex; // 同じ所有者を持つアロケーターの確保の同期
			std::pmr::unsynchronized_pool_resource Resource; // 子列の同じ大きさの領域を共有するメモリ
			ChildMemory Children; // 子列の確保と解放を共有メモリへ接続するリソース
			size_t Requests = 0; // ノード表と子列が要求したブロックの数
			size_t Blocks = 0; // システムから確保した共有メモリのブロックの数
			size_t Bytes = 0; // ノード表と子列が要求したバイト数の和
			bool do_is_equal(const std::pmr::memory_resource &Other) const noexcept override; // リソースの同一性の判定関数
			void *do_allocate(const size_t Bytes, const size_t Alignment) override; // システムのブロックの確保と計数関数
			void do_deallocate(void *const Pointer, const size_t Bytes, const size_t Alignment) override; // システムのブロックの解放関数
			void *Allocate(const size_t Count, const size_t Alignment); // 同期した共有ブロックの取得関数
			void *AllocateNodes(const size_t Count, const size_t Alignment); // 予約の余りを持たないノード表の確保関数

			NodeMemory(); // 自身を共有メモリの上流にする生成関数
		};

		// deque の再束縛・コピー・移動でも共有メモリの寿命を保つアロケーター
		template<class T> struct NodeAllocator {
			using value_type = T; // 標準のアロケーターが要求する要素型
			using propagate_on_container_move_assignment = std::true_type; // ムーブ代入ではメモリの所有者も移す
			using propagate_on_container_swap = std::true_type; // 交換ではメモリの所有者も交換する

			std::shared_ptr<NodeMemory> Storage; // ブロックとマップの最後のアロケーター迄有効な所有者

			/**
			 * 独立したコピーのメモリの生成関数
			 * 計算量：O(1)
			 * @return 別のノード表のメモリを所有するアロケーター
			 * @throws std::bad_alloc 所有者のメモリを確保出来ない場合
			 */
			NodeAllocator select_on_container_copy_construction() const {
				return NodeAllocator();
			} // 別の世代の生成関数

			/**
			 * 要求量だけの要素列の確保関数
			 * 計算量：確保する要素数 N に対し O(N)
			 * @param Count 要素数
			 * @return 要素数に合った整列済の領域
			 * @throws std::bad_alloc メモリを確保出来ない場合
			 * @throws std::bad_array_new_length 要素数とサイズの積が上限を超える場合
			 * @throws std::system_error 確保の同期に失敗した場合
			 */
			T *allocate(const size_t Count) {
				if(Count > std::numeric_limits<size_t>::max() / sizeof(T)) throw std::bad_array_new_length();
				return static_cast<T *>(Storage->AllocateNodes(Count * sizeof(T), alignof(T))); // 要素列の返戻
			}

			/**
			 * 所有する子列のリソースを使うノードの生成関数
			 * @param Target 確保済のノードの格納先
			 * @param Node 移動するノード
			 * @throws std::bad_alloc 子列のメモリを確保出来ない場合
			 * @throws std::system_error 子列の確保の同期に失敗した場合
			 */
			void construct(SyntaxNode *const Target, SyntaxNode &&Node) {
				::new (static_cast<void *>(Target)) SyntaxNode(&Storage->Children, std::move(Node));
				// 終了
				return;
			}

			/**
			 * ノードのコピーと再束縛した要素の生成関数
			 * 対象のコンストラクタが送出する例外は其の儘呼出元へ伝える
			 * @param Target 確保済の要素の格納先
			 * @param Values コンストラクタの実引数
			 */
			template<class U, class ...Args> void construct(U *const Target, Args &&...Values) {
				::new (static_cast<void *>(Target)) U(std::forward<Args>(Values)...);
				// 終了
				return;
			}

			/**
			 * ノード表の領域をシステムへ返す関数
			 * 計算量：O(1)
			 * @param Pointer 此の所有者が確保した領域
			 * @param Count 確保時の要素数
			 */
			void deallocate(T *const Pointer, const size_t Count) noexcept {
				std::pmr::new_delete_resource()->deallocate(Pointer, Count * sizeof(T), alignof(T));
				// 終了
				return;
			}

			/**
			 * アロケーターの同一性の判定関数
			 * 計算量：O(1)
			 * @param Other 比較する別の要素型も含むアロケーター
			 * @return 同じ共有メモリを使えば true
			 */
			template<class U> bool operator==(const NodeAllocator<U> &Other) const noexcept {
				return Storage == Other.Storage;
			} // 同一性の返戻

			NodeAllocator &operator=(const NodeAllocator &Other) noexcept = default; // 元の所有者も保つコピー代入演算子

			/**
			 * 別の要素型への再束縛の生成関数
			 * 計算量：O(1)
			 * @param Other 同じノード表を持つアロケーター
			 */
			template<class U> NodeAllocator(const NodeAllocator<U> &Other) noexcept : Storage(Other.Storage) {} // 所有者を共有する生成関数

			/**
			 * 同じ要素型のアロケーターのコピー生成関数
			 * ムーブ元のノード表を再使用出来る様に，右辺値からも所有者をコピーする
			 * 計算量：O(1)
			 * @param Other 同じノード表のアロケーター
			 */
			NodeAllocator(const NodeAllocator &Other) noexcept = default; // 元の所有者も保つコピー生成関数
			/**
			 * ノード表の共有メモリの生成関数
			 * 計算量：O(1)
			 * @throws std::bad_alloc 所有者のメモリを確保出来ない場合
			 */
			NodeAllocator() : Storage(std::make_shared<NodeMemory>()) {} // 共有メモリの生成関数
		};

		static constexpr size_t WorkBudget = 0X80000; // 小さな入力でも足りる作業単位の固定費（整形仕様 9.2 の 524288）
		static constexpr size_t WorkPerBaseShift = 5; // 基準量１当たりの作業単位の桁送（整形仕様 9.2 の３２倍）
		mutable size_t LiveCallees = 0; // 現在の構造を要約する生きた呼出先の数
		mutable std::deque<std::optional<CalleeSummary>> Callees {}; // 呼出元と範囲の間で共有するノード番号の要約
		mutable std::vector<BindingLocation> BindingPlaces {}; // 束縛の所属を読む時だけ確保するノード番号の表
		mutable std::unordered_map<NodeId, BindingScope> BindingScopes {}; // 有効範囲毎の名前
		mutable std::vector<NodeId> TreeRoots {}; // 親子構造の根（未接続の部分木も別の根を持つ）
		mutable std::optional<std::unordered_set<NodeId>> RewriteSourceOwners {}; // １つの書換計画で既に供給値を解除した領域
		mutable std::unordered_map<NodeId, std::unordered_set<NodeId>> SourceReaders {}; // 外側の範囲から其の供給値を読む別名の範囲への逆引
		mutable ParamScopeIdx ParamScopes {}; // 暗黙仮引数の供給元の型毎の其れを読む有効範囲
		mutable std::unordered_map<NodeId, BindingRegion> BindingRegions {}; // 関数毎の利用集合
		mutable std::map<NodeId, uint32_t> OrderChanges {}; // ノード番号順に整列する親毎の子列の変更開始位置
		mutable std::vector<uint64_t *> OrderEvents {}; // 順序の修復で再使用する前順・後順の番号の書込先
		mutable std::vector<uint32_t> NamedChildCounts {}; // 必要時に求めた名前付の直接子の数（UINT32_MAX は未照会）
		mutable NodeId OrderRoot = NoNode; // 順序を採番した根
		mutable std::unordered_map<NodeId, std::unordered_set<NodeId>> BindingReaders {}; // 外側の範囲毎の其れを読む領域
		mutable std::unordered_map<NodeId, std::unordered_map<NodeId, DocumentationFact>> Documentation; // 親の構造を依存範囲とする定義毎の共有判定
		mutable NormalizationFacts Normalization {}; // 現在の構造の共有事実
		mutable std::vector<RefFact> RefFacts {}; // 基準側の利用集合の参照の保持
		std::shared_ptr<const ValidationStorage> Validation; // 構造照合専用の所有者（編集可能なモデルでは無）
		std::deque<SyntaxNode, NodeAllocator<SyntaxNode>> Nodes; // 識別子で引くノード表（除いたノードも識別子を再利用しない為に残し，追加で既存のノードを動かさない）
		std::vector<std::vector<NodeId>> ErrorZoneOwners {}; // 保護領域毎の投影時の所有ノード（原文の位置・テキストを持たない）
		std::vector<std::string> CmpNames {}; // ノードが番号で引く識別子の比較用の名前
		std::unordered_map<NodeId, CallableNames> ProjectedImports {}; // 入力で確定したインポートの導入
		uint32_t ImplicitFunctionNameIdx = 0; // 投影時に共有した暗黙の関数内導入名
		std::unordered_map<std::string, uint32_t> CmpNameIndexes {}; // 同じ綴りへ同じ名前番号を供給する表
		std::vector<std::shared_ptr<const OwnedRegion>> Regions {}; // ノードが番号で引く所有領域（別に所有する出力を共有し，親のトークンにしない）
		std::vector<bool> InsertedNodes; // 原文のノードを含まない生成部分木（初めて生成する時だけ作り，コメントの後続の境界から除く）
		std::vector<uint8_t> IsRemoved; // ノードを除いたか
		std::vector<std::vector<NodeId>> RoleNodes {}; // 役割毎のノードのインデックス（正規化の間だけ役割の数の要素を持ち，構文の値を小さく保つ）
		std::vector<size_t> IndexedPositions {}; // 全ノードの交換除去の位置を並べた共有の列
		std::vector<std::pair<size_t, size_t>> IndexedPositionRanges {}; // ノード毎の位置列の先頭と容量
		std::vector<size_t> PositionBuffer {}; // 種別の変更で旧位置列を読み終える迄の作業領域
		RoleSet IndexedRoles {}; // 此の正規化で照会する役割
		std::unordered_map<const KindDef *, std::vector<Role>> IndexedRoleLists {}; // 種別毎に１度選んだ照会対象の役割列
		std::vector<const KindDef *> IndexedKinds {}; // ノード毎にインデックスへ登録済の役割を持つ共通の種別（登録が無ければ無）
		bool HasRoleIdx = false; // 投影後の役割インデックスを構築したか
		std::vector<NodeId> Following {}; // 構文の順でノード毎の後に続く物（要素の区切を飛ばす，正規化の後に１度だけ作る）
		std::unordered_set<NodeId> LastTouched {}; // 最後の計画が変更したノードと其の親
		std::vector<uint32_t> DetachPositions {}; // 切離で再使用する親の旧位置から新位置への対応
		std::vector<NodeId> RewriteReused {}; // 書換で再使用する切離対象の列
		std::vector<const GeneratedNode *> RewritePending {}; // 書換で再使用する生成部分木の展開列
		std::vector<AnchorTransfer> RewriteRemovedRoots {}; // 書換で再使用する除去部分木のアンカーの列
		NodeId Root = NoNode; // 根
		uint64_t ChildRoleMask = 0; // 文法が投影する子の役割
		const SyntaxRules *Def = nullptr; // 文法の構文・意味の固定の定義
		std::shared_ptr<StructureWorkAccount> Work = std::make_shared<StructureWorkAccount>(); // 期待構文とは独立した作業予算

		static SyntaxModel CompactForValidation(
			const SyntaxModel &Syntax,
			const std::span<const NodeId> Declarations,
			const std::span<const std::pair<NodeId, FormOperation>> Generations,
			bool (*const KeepBoundary)(const SemanticBoundary &)
		); // 構造照合だけが読む期待の所有表の生成関数

		static bool IsBindingFunction(const BindingSyntax Shape); // 捕捉の境界を導入する分類かの判定関数
		static bool IsBindingScope(const BindingSyntax Shape); // 名前の有効範囲を導入する分類かの判定関数
		uint32_t GetValidationSiblingIndex(const NodeId Id) const; // 選択したメンバ区切の兄弟位置の取得関数
		ValidationNode GetValidationNode(const NodeId Id) const; // 構造照合用のノードの取得関数
		std::span<const SemanticBoundary> GetValidationBoundaries(const NodeId Id) const; // 構造照合用の意味境界の取得関数
		bool HasValidationRegion(const NodeId Id) const; // 構造照合用の所有領域の有無の判定関数
		bool IsValidationRegionEmpty(const NodeId Id) const; // 構造照合用の所有領域が空かの判定関数
		GeneratedNode GetValidationGeneration(const NodeId Id, const FormOperation Operation) const; // 選択した生成部品の取得関数
		const GeneratedNode *GetPreparedValidationGeneration(const NodeId Id, const FormOperation Operation) const; // 縮約時に確定した生成部品の参照の取得関数
		std::string_view ReadCmpNameView(const NodeId Id) const; // 予算を変更しない所有済の名前の読取関数（製品の照会は計上する入口から使う）
		const KindDef *GetDefOf(const NodeId Id) const; // 子・トークン・意味制約から独立した固定の種別の取得関数
		bool IsBindingFunction(const NodeId Node) const; // 捕捉の境界を導入するノードかの判定関数
		bool HasValBindingConstructor(const NodeId Node) const; // 固定契約と現在の本体から値のコンストラクタの導入を判定する関数
		bool IsBindingScope(const NodeId Node) const; // 名前の有効範囲を導入するノードかの判定関数
		bool IsOuterBindingHead(const NodeId Function, const NodeId Node) const; // メンバ名を関数の外側で評価する位置かの判定関数
		bool IsBindingDeclarationPart(const NodeId Node, const BindingSyntax Shape) const; // 宣言から名前を導入する直接の子の共通判定関数
		NodeId GetOuterBindingScope(const NodeId Parent, const NodeId Child) const; // 導入前に外側で評価する直接の子の有効範囲の取得関数
		BindingNamespace GetBindingNamespace(const NodeId Name) const; // 現在の導入位置が使う名前空間の取得関数
		NodeId GetBindingParams(const NodeId Scope, NodeId &Owner) const; // 直接又は所有する型から仮引数を取得する関数
		const BindingLocation &GetBindingLocation(const NodeId Node) const; // 親から共有する束縛の所属の取得関数
		NodeId GetBindingScope(const NodeId Node) const; // 名前を探し始める範囲の取得関数
		NodeId GetTreeRoot(const NodeId Node) const; // 現在の親子構造の根の共有取得関数
		bool HasMutableBindingBorrow(const NodeId Pattern) const; // 照合の対象を可変借用するパターンかの判定関数
		void InvalidateBindingSources(const NodeId Scope) const; // 同じ供給元と其れを読む別名の保持の終了関数
		void EraseBindingScope(const NodeId Scope) const; // 宣言と供給元の範囲を依存の逆引と共に解放する関数
		const CallableImports &GetBindingSourcePositions(const CallableIndex &Index, const uint32_t NameIdx) const; // 同じ供給元の可視位置の共有取得関数

		const CallableIntroduction *FindBindingSource(
			const CallableIndex &Index,
			const NodeId Scope,
			const NodeId Head,
			const uint32_t NameIdx,
			const std::string &Member,
			bool &HasCustomDefinition
		) const; // 同じ束縛の供給元とメンバ変更の可視位置の取得関数

		const BindingScope &GetBindingScopeFacts(const NodeId Scope) const; // 現在の範囲の名前と供給元の共有取得関数
		BindingScope BuildBindingScope(const NodeId Scope) const; // 現在の構造から名前を収集する関数
		NodeId GetBindingWriteTarget(const NodeId Node, const BindingSyntax Shape) const; // 束縛又は基底の可変性を必要とする対象の取得関数
		std::vector<NodeId> GetBindingWriteNames(const NodeId Target, bool &IsUncertain) const; // 基底の変更を含む書込名の取得関数
		std::pair<bool, bool> GetBindingInitUse(const NodeId Scope, const NodeId Name, const bool ShouldIncludeBody) const; // 初期化時の可視性

		BindingSelection SelectBindingDecl(
			const BindingScope &Defs,
			const NodeId Scope,
			const NodeId Name,
			const std::vector<BindingDecl> &Decls,
			const uint32_t NameIdx,
			const BindingNamespace Namespace
		) const; // 共有する宣言列から可視宣言と自己導入を解決する関数

		BindingFact ResolveBindingName(
			const NodeId Name,
			const NodeId InitialScope,
			std::unordered_map<NodeId, BindingScope> *const Scopes,
			std::unordered_set<NodeId> *const UncertainInitializers,
			bool &IsHead
		) const; // 利用集合を作らず宣言の同一性と可視性を解決する関数

		NodeId GetOuterDeclarationScope(const NodeId Scope) const; // 宣言探索の外側の有効範囲の取得関数
		BindingRegion BuildBindingRegion(const NodeId Owner) const; // 現在の構造から参照と書込を解決する関数
		NodeId GetBindingRegionOwner(const NodeId Node) const; // 最外の関数又は根の取得関数
		const BindingRegion &GetBindingRegion(const NodeId Owner) const; // 方針に従う利用集合の取得関数
		std::pair<NodeId, const BindingRegion *> GetBindingRegionForName(const NodeId Name) const; // 名前を解決する所有者と領域の取得関数

		const BindingUses &GetBindingCaptures(
			const BindingRegion &Region,
			const NodeId KnownOwner,
			const BindingRegion &KnownRegion
		) const; // 最上位の利用判定が共有する捕捉集合の取得関数

		bool IsCalleeErasureSuccessful(const NodeId Node) const; // 生きた呼出先の要約の解除関数
		bool HasBindingUse(const NodeId Name, const bool IsWrite) const; // 同じ領域を再取得せず作用を判定する関数
		std::string NormalizeIdentifierNfkc(const std::string_view Name) const; // 識別子の NFKC 比較名の生成関数
		NameSpelling ReadNameSpelling(const std::string_view Spelling) const; // 名前のテキストの初回走査関数
		NormalizationFacts &EditNormalizationFacts() const; // 正規化工程だけの共有事実の更新口
		void InvalidateCallees(const NodeId Node) const; // 変更したノードから祖先の呼出先の要約の解除関数
		void CloseWork() const; // 所有工程が作業予算を閉じる操作
		bool IsOrderNumberingSuccessful(const NodeId Id) const; // 保持した区間内で部分木を採番する関数
		bool IsOrderRepairSuccessful(const NodeId Id, const uint32_t FirstChild) const; // 既存の子の区間を保ち，移動・挿入した部分だけを採番する関数
		void EnsureOrder() const; // 変更された部分木の順序だけを読取前に更新する関数
		const std::vector<Role> &IndexedRolesOf(const KindDef &Kind); // 種別毎の照会対象の役割列の取得関数
		void MarkOrderChange(NodeId Id, uint32_t FirstChild); // 子列が変わる部分木の順序の失効関数
		void UpdateNamedChildCount(const NodeId Parent, const bool HadNamedChild, const bool HasNamedChild); // 既知の名前付の子数だけを接続又は種別変更に合わせて更新する関数
		void Touch(const NodeId Id); // 書換が変更したノードの記録関数
		void IdxRoles(const NodeId Id); // ノードの現在の役割のインデックスへの登録関数
		void InvalidateBindingLocation(const NodeId Node); // 移動・分類変更が影響する部分木の所属の失効関数
		void EraseBindingRegion(const NodeId Owner); // 依存の逆引と共に利用集合を解放する関数
		void MarkBindingChange(const NodeId Node); // 書換の前後で影響する範囲の記録関数
		void Detach(const NodeId Id); // 親の子の並びからの切離関数
		NodeId AddNode(SyntaxNode &&Node); // ノードの追加関数
		void DetachBatch(const std::vector<NodeId> &Moved); // 同じ親の既存ノードの一括切離関数

		NodeId AddGenerated(
			const GeneratedNode &Generated,
			const NodeId Parent,
			const uint32_t Idx,
			const bool ShouldMarkBinding = true
		); // 規則から生成するノードの部分木の追加関数

		uint32_t InternCmpName(std::string Name, const bool IsIdentifier); // 比較用の名前番号の共有関数

		void Attach(
			const NodeId Id,
			const NodeId Parent,
			const uint32_t Idx,
			const BoundaryKind Boundary,
			const std::optional<SemanticBoundary> &RecordedBoundary
		); // 親の子の並びへの接続関数

		void ReshapeNode(const RewriteStep &Step, const bool HasDetachedChildren, RewriteTrace &Trace); // 識別子を保ったノードの組直関数
		void UnwrapNode(const NodeId Id, RewriteTrace &Trace, const bool ShouldMarkBinding = true); // ラッパーの除去関数
		void InvalidateFacts(const RewritePlan &Plan); // 書換が変える事実の失効関数
		void TransferInnerAnchors(const AnchorTransfer &Transfer, RewriteTrace &Trace); // 除くノードの内部のアンカーの移譲関数

		void ApplyStep(
			const RewriteStep &Step,
			const bool HasDetachedChildren,
			RewriteTrace &Trace,
			std::vector<AnchorTransfer> &RemovedRoots
		); // 書換の１操作の適用関数

		void FixWorkLimit(); // 投影を終えた構文の基準量からの作業単位の上限の確定関数

	public:

		std::optional<bool> IsModuleSrc; // 入力で確定した最上位の束縛の前提（未確定なら構文から求める）

		/**
		 * 部分木の前行順の走査関数
		 * `Visit` の送出を其の儘伝播する
		 * 計算量：部分木のノード数 N と動作の総評価費用 Q に対し O(N + Q)
		 * @param Syntax 構文モデル
		 * @param Root 走査する部分木の根
		 * @param Visit ノード毎の動作を返す関数
		 * @return 走査を止めたなら true
		 * @throws FormatLimitExceeded ノードの走査で作業予算を超えた場合
		 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
		 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
		 */
		template<class Function> static bool HasStoppedTraversal(const SyntaxModel &Syntax, const NodeId Root, Function &&Visit) {
			// 部分木の前行順の走査
			std::vector<NodeId> Stack { Root };
			while(!Stack.empty()) {
				const NodeId Id = Stack.back();
				Stack.pop_back();
				// ノードの１回の検査を１作業単位とする（整形仕様 9.2）
				Syntax.Charge(1);
				const WalkStep Step = Visit(Id);
				// 走査を止めた事の返戻
				if(Step == WalkStep::Stop) return true;
				if(Step == WalkStep::Skip) continue;
				const std::span<const NodeId> Children = Syntax.GetChildrenOf(Id);
				Stack.insert(Stack.end(), Children.rbegin(), Children.rend());
			}
			// 最後迄辿った事の返戻
			return false;
		}

		/**
		 * 子の走査関数
		 * `Visit` の送出を其の儘伝播する
		 * 計算量：子の数 C と動作の総評価費用 Q に対し O(C + Q)
		 * @param Syntax 構文モデル
		 * @param Id 親
		 * @param Visit 子毎の関数（真偽を返す物は偽で走査を止める）
		 * @throws FormatLimitExceeded ノードの走査で作業予算を超えた場合
		 */
		template<class Function> static void ForEachChildOf(const SyntaxModel &Syntax, const NodeId Id, Function &&Visit) {
			// 子の走査
			for(const NodeId Child : Syntax.GetChildrenOf(Id)) {
				// 子の１回の検査を１作業単位とする（整形仕様 9.2）
				Syntax.Charge(1);
				if constexpr(std::is_same_v<std::invoke_result_t<Function, NodeId>, bool>) {
					if(!Visit(Child)) break;
				} else Visit(Child);
			}
			// 終了
			return;
		}

		/**
		 * 名前付の子の走査関数
		 * `Visit` の送出を其の儘伝播する
		 * 計算量：子の数 C と動作の総評価費用 Q に対し O(C + Q)
		 * @param Syntax 構文モデル
		 * @param Id 親
		 * @param Visit 名前付の子毎の関数（真偽を返す物は偽で走査を止める）
		 * @throws FormatLimitExceeded ノードの走査で作業予算を超えた場合
		 */
		template<class Function> static void ForEachNamedChildOf(const SyntaxModel &Syntax, const NodeId Id, Function &&Visit) {
			// 名前付の子の走査
			for(const NodeId Child : Syntax.GetChildrenOf(Id)) {
				Syntax.Charge(1);
				if(!Syntax.IsNamed(Child)) continue;
				if constexpr(std::is_same_v<std::invoke_result_t<Function, NodeId>, bool>) {
					if(!Visit(Child)) break;
				} else Visit(Child);
			}
			// 終了
			return;
		}

		/**
		 * 文の並びに属する文の走査関数
		 * `Visit` の送出を其の儘伝播する
		 * 名前付の子の内，開始行から後送された本文は独立した文として数えない
		 * 計算量：子の数 C と動作の総評価費用 Q に対し O(C + Q)
		 * @param Syntax 構文モデル
		 * @param Id 文を並べるノード
		 * @param Visit 文毎の関数（真偽を返す物は偽で走査を止める）
		 * @throws FormatLimitExceeded ノードの走査で作業予算を超えた場合
		 */
		template<class Function> static void ForEachStmtOf(const SyntaxModel &Syntax, const NodeId Id, Function &&Visit) {
			// 文の並びに属する文の走査
			for(const NodeId Child : Syntax.GetChildrenOf(Id)) {
				Syntax.Charge(1);
				if(!Syntax.IsNamed(Child) && !Syntax.Is(Child, Role::EmptyStmt) || Syntax.Is(Child, Role::DeferredLiteralBody)) continue;
				if constexpr(std::is_same_v<std::invoke_result_t<Function, NodeId>, bool>) {
					if(!Visit(Child)) break;
				} else Visit(Child);
			}
			// 終了
			return;
		}

		/**
		 * 述語を満たす子の有無の判定関数
		 * `Predicate` の送出を其の儘伝播する
		 * 計算量：子の数 C と述語の総評価費用 Q に対し O(C + Q)
		 * @param Syntax 構文モデル
		 * @param Id 親
		 * @param Predicate 子を受け取る述語
		 * @return 述語を満たす子が有れば true
		 * @throws FormatLimitExceeded ノードの走査で作業予算を超えた場合
		 */
		template<class Function> static bool HasChildMatching(const SyntaxModel &Syntax, const NodeId Id, Function &&Predicate) {
			// 述語を満たす子の有無の判定
			for(const NodeId Child : Syntax.GetChildrenOf(Id)) {
				Syntax.Charge(1);
				// 述語を満たす子の発見の返戻
				if(Predicate(Child)) return true;
			}
			// 述語を満たす子の無い事の返戻
			return false;
		}

		/**
		 * 述語を満たすノードの部分木の中の有無の判定関数（根を含む）
		 * `Predicate` の送出を其の儘伝播する
		 * 計算量：部分木のノード数 N と述語の総評価費用 Q に対し O(N + Q)
		 * @param Syntax 構文モデル
		 * @param Id 部分木の根
		 * @param Predicate ノードを受け取る述語
		 * @return 述語を満たすノードが有れば true
		 * @throws FormatLimitExceeded ノードの走査で作業予算を超えた場合
		 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
		 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
		 */
		template<class Function> static bool HasNodeMatching(const SyntaxModel &Syntax, const NodeId Id, Function &&Predicate) {
			// 述語を満たすノードで止めた走査の結果の返戻
			return Id != NoNode && Core::SyntaxModel::HasStoppedTraversal(
				Syntax,
				Id,
				[&Predicate](const NodeId Node) -> WalkStep {
					// 述語を満たせば止める事の返戻
					return Predicate(Node) ? WalkStep::Stop : WalkStep::Descend;
				}
			);
		}

		void Charge(const size_t Amount) const; // 構文の正規化と中間表現の構築の作業単位の計上関数
		std::span<const NodeId> GetChildrenOf(const NodeId Id) const; // 子の並びの取得関数
		bool IsNamed(const NodeId Id) const; // 名前付のノードかの判定関数
		bool Is(const NodeId Id, const Role Wanted) const; // 役割を持つノードかの判定関数
		bool IsAny(const NodeId Id, const std::initializer_list<Role> Wanted) const; // 何れかの役割を持つノードかの判定関数
		uint64_t GetFieldBit(const ChildRole Role) const; // 子の役割のビットの取得関数
		const SyntaxNode &GetNode(const NodeId Id) const; // ノードの取得関数
		/**
		 * ========== ノード・トークン・構文事実の取得 ==========
		 */
		std::string_view GetLexemeOf(const NodeId Id) const; // トークンのテキストの取得関数（ノードが無ければ空）
		NodeId GetParentOf(const NodeId Id) const; // 親の取得関数
		NodeId GetChild(const NodeId Id, const uint32_t Idx) const; // 子の取得関数
		NodeId GetFieldChild(const NodeId Id, const ChildRole Role) const; // 役割を持つ子の取得関数
		bool HasWordChild(const NodeId Id, const Word Meaning) const; // 語の意味の綴りの無名の子を持つかの判定関数
		// 正規化の規則・制御の流れ・シグネチャ・コメント
		NodeId GetFirstNamedChildWithRole(const NodeId Id, const Role Desired) const; // 役割を持つ最初の名前付の子の取得関数
		bool HasNodeWithin(const NodeId Ancestor, const NodeId Node) const; // ノードが部分木に含まれるかの判定関数
		std::pair<uint64_t, uint64_t> GetOrderRange(const NodeId Node) const; // 現在の構文順の共有区間の取得関数
		uint32_t GetContentChildCount(const NodeId Id) const; // 無名の空文も含む内容子数の取得関数
		NodeId GetContentChild(const NodeId Id, const uint32_t Idx) const; // 無名の空文も含む内容子の取得関数
		NodeId GetLastContentChild(const NodeId Id) const; // 無名の空文も含む最後の内容子の取得関数
		uint32_t GetNamedChildCount(const NodeId Id) const; // 名前付の子の数の取得関数
		NodeId GetNamedChild(const NodeId Id, const uint32_t Idx) const; // 名前付の子の取得関数
		bool HasField(const NodeId Id, const ChildRole Role) const; // 親の中の子の役割の判定関数
		const SyntaxRules &GetRules() const; // 文法の構文・意味の固定の定義の取得関数
		NodeId GetPrevSibling(const NodeId Id) const; // 前の兄弟の取得関数
		NodeId GetNextSibling(const NodeId Id) const; // 後の兄弟の取得関数
		bool IsTokenRole(const NodeId Id, const Role Wanted) const; // 実際の綴りのトークンの役割の判定関数
		const CanonicalForm *GetFormOf(const NodeId Id, const FormOperation Operation) const; // ノードの生成形式の取得関数
		bool IsWord(const NodeId Id, const Word Meaning) const; // 投影又は生成で確定した語の意味の判定関数
		FormPart GetGenerationPartOf(const NodeId Id) const; // 現存ノードの種別と空行比較を保持する生成部品の取得関数
		std::string GetText(const NodeId Id) const; // 部分木のトークンのテキストを区切無で繋いだテキストの取得関数
		bool IsComparisonName(const NodeId Id) const; // 比較名を記録・照会する名前の役割又は宣言の直接の名前かの判定関数
		uint32_t GetCmpNameIdx(const NodeId Id) const; // 比較用の名前番号の取得関数
		std::string_view GetCmpNameView(const NodeId Id) const; // 比較用の名前の所有テキスト参照の取得関数
		LexicalEdge GetFirstEdge(const NodeId Id) const; // 部分木の最初のトークンと字の取得関数
		LexicalEdge GetLastEdge(const NodeId Id) const; // 部分木の最後のトークンと字の取得関数
		bool IsEdgeCharRole(const LexicalEdge &Edge, const Role Wanted) const; // 部分木の端の文字の文字の接続の役割の判定関数
		bool IsEdgeRole(const LexicalEdge &Edge, const Role Wanted) const; // 部分木の端のトークンの役割の判定関数
		size_t GetNodeCount() const; // ノード表の大きさの取得関数（除いたノードを含む）
		NodeId GetRoot() const; // 根の取得関数
		const std::vector<std::vector<NodeId>> &GetProtectedErrorOwners() const; // 保護領域毎の投影時の所有ノードの取得関数
		const FormPart &GetDefFor(const GeneratedNode &Generated) const; // 生成するノードの種別と構文事実の定義の取得関数
		BindingSyntax GetBindingSyntax(const NodeId Node) const; // 固定の構造と現在の名前のフィールドから束縛の役割を取得する関数
		BindingValSrc GetBindingValSrc(const BindingFact &Binding) const; // 現在の束縛の値の供給と非定数性の取得関数
		BindingIntroduction GetBindingIntroduction(const NodeId Name) const; // 名前の導入・名前空間・未確定性だけの取得関数
		BindingFact GetBindingFact(const NodeId Name) const; // 現在の構造の束縛の取得関数（値で返戻し，書換後にも参照を残さない）
		BindingFact GetBindingFact(const NodeId Name, const std::string_view Member, const bool IsGlobal = false) const; // 束縛と修飾した呼出の供給元の取得関数
		bool HasBindingWrite(const NodeId Name) const; // 束縛への再代入又は未確定な作用の有無の判定関数
		bool HasBindingRef(const NodeId Name) const; // 導入以外の束縛の利用又は未確定な作用の有無の判定関数
		std::vector<NodeId> GetBindingNames(const NodeId Pattern, bool *const IsUncertain = nullptr) const; // パターンの導入・代入名だけの取得関数
		bool IsInsertedNode(const NodeId Id) const; // 原文のノードを含まない生成部分木かの判定関数
		bool IsRemovedNode(const NodeId Id) const; // 除いたノードかの判定関数
		std::shared_ptr<const StructureWorkAccount> GetWorkAccount() const; // 作業予算の取得関数
		size_t GetWorkLimit() const; // 作業単位の上限の取得関数
		void CheckNesting() const; // ネストの段数の検査関数
		std::span<const NodeId> GetNodesWith(const Role Wanted) const; // 役割を持つノードの候補の取得関数
		const OwnedRegion *GetRegionOf(const NodeId Id) const; // 所有領域の取得関数
		bool IsBuiltinArithmeticType(const NodeId Id) const; // 予約語から組込の算術型を確認する判定関数
		uint32_t GetChildCount(const NodeId Id) const; // 子の数の取得関数
		NodeId GetLastNamedChild(const NodeId Id) const; // 最後の名前付の子の取得関数
		NodeId GetLastStmt(const NodeId Id) const; // 文の並びの最後の文の取得関数
		NodeId GetPrevNamedSibling(const NodeId Id) const; // 前の名前付の兄弟の取得関数
		NodeId GetNextNamedSibling(const NodeId Id) const; // 後の名前付の兄弟の取得関数
		NodeId GetFollowingOf(const NodeId Id) const; // 構文の順でノードの後に続く物（要素の区切を飛ばす）の取得関数
		bool HasTokenChild(const NodeId Id, const std::string_view Token) const; // 指定のテキストの無名の子を持つかの判定関数
		bool HasTokenRoleChild(const NodeId Id, const Role Wanted) const; // 指定の役割の無名の子を持つかの判定関数
		bool IsBracedBody(const NodeId Node) const; // 波括弧で包んだ文のブロックかの判定関数
		bool ShouldSeparateBeforeClose(const NodeId Id) const; // 空の本体を閉じの語と同じ行へ畳むと構文エラーに為るかの判定関数
		bool IsBranchValUsed(const NodeId Node) const; // １対象の値の利用位置の判定関数
		bool IsValUnusedCtx(const NodeId Node, const NodeId Parent) const; // 値を使わない文又は反復の更新フィールドかの判定関数
		std::optional<CommaDecl> GetCommaDeclOf(const NodeId Node, const bool NeedsKey = true) const; // カンマで並べる宣言の部品の取得関数
		GeneratedNode CopyOf(const NodeId Target) const; // 部分木のコピーの生成ノードの取得関数
		void RemapGeneratedBoundaries(GeneratedNode &Generated) const; // 残る子の隣接に従う境界の組直関数
		bool IsBefore(const NodeId Lhs, const NodeId Rhs) const; // 構文の順で前に在るかの判定関数
		NodeId GetIfConsequence(const NodeId Id) const; // 分岐の真枝の取得関数
		NameSpelling GetNameSpellingOf(const NodeId Id) const; // 名前のテキストの事実の取得関数
		std::string GetCmpName(const NodeId Id) const; // 投影済の識別子の比較用の名前の取得関数
		unsigned char GetFirstChar(const NodeId Id) const; // 部分木の最初の字の取得関数
		unsigned char GetLastChar(const NodeId Id) const; // 部分木の最後の字の取得関数
		bool HasLastTokenRole(const NodeId Id, const Role Wanted) const; // 部分木の最後のトークンの役割の判定関数
		int GetBracketLeadDelta(const NodeId Id) const; // 括弧を開閉するトークンの深さの変化の取得関数
		bool IsBracketLead(const NodeId Id, const Role Wanted) const; // 括弧を開閉するトークンの役割の判定関数
		void BuildRoleIdx(const RoleSet &WantedRoles); // 必要な役割だけの正規化のインデックスの構築関数
		void BuildRoleIdx(); // 全役割のインデックスの構築関数
		void ReleaseRoleIdx(); // 正規化の役割インデックスの解放関数
		void BuildFollowingIdx(); // 構文の順でノード毎の後に続く物のインデックスの構築関数
		void ReleaseFacts(); // 正規化後の共有事実の解放関数
		RewriteTrace Apply(const RewritePlan &Plan); // 書換の計画の適用関数

		SyntaxModel(const SyntaxRules &Def, const uint64_t ChildRoleMask); // コンストラクタ
	};
} // namespace Core
