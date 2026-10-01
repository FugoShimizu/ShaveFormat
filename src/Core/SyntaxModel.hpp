#pragma once

#include "NodeId.hpp"
#include "SyntaxRole.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace Core {
	enum class BoundaryKind : uint8_t { Glued, NoBreak, NoInlineSpace, Preserved, RequiredSpace, Separated, Spaced }; // 子の間の意味を持つ境界の種類（整形仕様 3.2）
	enum class PreservedSource : uint8_t { ErrorBoundary, HiddenContent, LiteralContent, MacroTokens }; // 原文の間隙を写す理由（字句値とマクロの間隙は完全一致，誤り区画の外縁だけは字下げを通常の配置へ委ねる）

	// 子の間の意味を持つ境界（表示桁・コメント本文を持たず，Preserved の区画だけ原文の間隙を持つ）
	struct SemanticBoundary {
		uint32_t Before = 0; // 境界の後に続く子の番号
		BoundaryKind Kind = BoundaryKind::Glued; // 境界の種類
		std::string Gap {}; // 写す原文の間隙（`Preserved` だけ，空白と解析器が読み飛ばした字句）
		PreservedSource Source = PreservedSource::ErrorBoundary; // 保持する間隙の由来
	};

	enum class ConstraintKind : uint8_t {
		ContainsError, // 投影で修復しなかった解析器の誤りを部分木に含む節点
		AmbiguousDeclaration, // 式と区別出来ない型名と宣言子を持つ宣言
		DeclaratorMacro, // 宣言子の記号へ展開するマクロを含む型（宣言子の間で共有すると記号の適用先が変わる）
		DetachedElse, // 前処理の条件の塊を挟んで解析器が繋げなかった else が後に続く文と其の右端の文（波括弧の増減で else の結付が変わる）
		DirectiveZone, // 前処理指令で始まる誤り区画（条件の塊の境界を保つ為に隣の文との間を改行する）
		EndsCode, // 後の原文を処理系がデータとして読む文（終端の字句を補うと保持するデータが変わる）
		MacroSensitiveParen, // 展開後の意味を保つ括弧
		MisparsedGenericCall, // 総称呼出を比較と誤読した外側の二項式
		MisparsedTemplate, // 比較を型引数と誤読した実体化・型引数の並び
		MisparsedTemplateDeclaration, // 型引数を持つ型の宣言を比較と誤読した式文の外側の二項式
		MisreadProduct, // C# の乗算を宣言式の型と誤読したポインタ型
		NoOptionalBreak, // 構文内の任意の改行で読みが変わる節点
		StatementMacro, // 複数の文や開いた if へ展開するマクロの呼出の文（単一の文の本体に置くと先頭の文だけが本体に為る）
		ValuedMacro, // 置換本体を持つマクロの定義（静的検査が定数の候補として読む）
		Verbatim, // 子へ降りず字面を其の儘保つ区画
		Count // 種類の数（番兵）
	}; // 節点の意味上の制約の種類（整形仕様 3.2, 4.2）

	static_assert(static_cast<uint8_t>(ConstraintKind::Count) < 17); // 制約の集合は２バイトに収める

	// 子の番号順に並ぶ意味を持つ境界の並び（大半の節点が持たない為，持つ節点だけが領域を確保する）
	class SemanticBoundaryList {
	private:

		std::unique_ptr<std::vector<SemanticBoundary>> Entries; // 境界（持たなければ無し）

	public:

		std::span<const SemanticBoundary> View() const; // 境界の並びの取得関数
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

	// 所有区画（整形仕様 4.4，単独のコードと同じ工程で整形し検証した埋込の出力で，親の配置の段が親の深さへ置く）
	struct OwnedRegion {
		std::string CodeText; // コメントを除いた中身のコード行（開始・終了タグの行を含まず，親の中核だけが読む）
		std::vector<uint32_t> CodeVerbatimLines; // コード行の内の逐語の中から始まる行の番号（０始まりの昇順，親は此の行へ字下げを付けない）
		std::string Text; // コメントを含む整形した中身の行（親の出力の組立と検証だけが読む）
		std::vector<uint32_t> VerbatimLines; // 中身の行の内の逐語の中から始まる行の番号（０始まりの昇順）
	};

	// 節点の構文事実（整形仕様 3.2，文境界・必須の空白・改行の禁止・字句の密着を表し，原文の位置を持たない）
	struct SyntaxFacts {
		SemanticBoundaryList Boundaries; // 子の番号順に並ぶ意味を持つ境界
		CanonicalFormList Forms {}; // 文法の変更不能な定義を共有する生成形式
		uint16_t Constraints = 0; // 節点の意味上の制約の集合（制約の種類の番号の位の和）
		bool DiscardsBinding = false; // 名前の位置に在っても束縛を導入しない識別子
		bool MayReassignExistingBinding = false; // 既存の束縛への代入を含み得る宣言
		bool HasBooleanNames = true; // 真偽値の名前 (`true` / `false`) を綴れる位置か（C は <stdbool.h> の取込の後だけ）
		bool AbsorbsLeadingComment = false; // 直前の独立コメントを文書・前文として取り込む宣言か（Go の文書を持つ宣言と cgo の取込）
		uint32_t NameIndex = 0; // 投影時に確定した識別子の比較用の名前の番号（構文モデルの名前の表の位置に１を足した値で，無ければ０）
		uint32_t RegionIndex = 0; // 所有区画とする埋込の整形結果の番号（構文モデルの区画の表の位置に１を足した値で，無ければ０）
		bool Has(const ConstraintKind Kind) const; // 制約の有無の判定関数
		const SemanticBoundary *BoundaryBefore(const uint32_t Index) const; // 子の前の意味を持つ境界の取得関数
		void Add(const ConstraintKind Kind); // 制約の追加関数
	};

	// 節点の種別（同じ構文上の性質を持つ節点で定義を共有する）
	struct SyntaxKind {
		const KindDefinition *Definition = nullptr; // 共通の節点種別
		bool IsNamed() const; // 名前付の節点かの判定関数
	};

	// 独自の節点表の要素（整形仕様 3.2，字句の葉と逐語区画だけが字面を持つ）
	struct SyntaxNode {
		NodeId Id = NoNode; // 節点の識別子
		NodeId Parent = NoNode; // 親（根は無し）
		SyntaxKind Kind; // 節点の種別
		std::string Lexeme; // 字句の葉・逐語区画の復号済の字面（其の他の節点は空）
		SyntaxFacts Facts; // 構文事実
		std::vector<NodeId> Children; // 子の並び
		uint32_t Index = 0; // 親の子の並びの中の位置
		uint32_t Generation = 0; // 節点と其の子の並びを書き換えた回数（同じ計画に纏めた規則の間の競合の確認に使う，整形仕様 5.1）
		uint64_t Fields = 0; // 親の中の子の役割の集合（隠れた規則の内外の役割を併せ持つ）
		bool IsInserted = false; // 整形で加えた原文の節点を含まない部分木か（補った返戻文等，コメントの後続の境界にしない，整形仕様 7.6）
	};

	// 部分木の端に在る字句の葉と其の端の字
	struct LexicalEdge {
		NodeId Leaf = NoNode; // 端の字句の葉
		char Char = '\0'; // 葉の端の字
	};

	// 錨の行先（移る先の節点と，其の節点の前後の何方の錨に為るか）
	struct AnchorTarget {
		NodeId Node = NoNode; // 移る先の節点
		bool IsAfter = false; // 移る先の節点の後の錨に為るか
		bool IsEmptyBody = false; // 空に畳む本体の中へ移すコメントか
		bool IsCaseBody = false; // 空に為る case 本体の見出の後へ移すコメントか
	};

	// 錨の移譲（整形仕様 3.2，消える又は移る節点の先行・末尾・内部の錨の行先）
	struct AnchorTransfer {
		NodeId From = NoNode; // 消える又は移る節点
		AnchorTarget Leading; // 先行の錨の行先
		AnchorTarget Trailing; // 末尾の錨の行先
		AnchorTarget Inner; // 内部の錨の行先
	};

	enum class AnchorSide : uint8_t { Before, After, Inside }; // 錨に対する記録の置き方（移譲の行先の選択に使い，コメントの配置に対応する） // 錨の前 // 錨の後 // 錨の内部
	enum class TokenReset : uint8_t { Preserve, AllInsideAnchor, None }; // 錨との間の字句の数の置換 // 保つ // 錨の中の全ての字句の後 // 間に字句を置かない

	// 合成した錨の移譲の行先（整形仕様 7.2，移譲の連鎖を正規化の中で合成し，コメント側は一度だけ引く）
	struct AnchorMove {
		NodeId Node = NoNode; // 最終の錨（行先を持たない置き方は無し）
		AnchorSide Side = AnchorSide::Before; // 最終の置き方
		bool IsEmptyBody = false; // 空に畳む本体の中へ移したか
		bool IsRemovedCaseBody = false; // 空に為る case 本体の見出の後へ移したか
		TokenReset Reset = TokenReset::Preserve; // 錨との間の字句の数の置換
	};

	// 節点の統合・除去・分割の記録（整形仕様 3.2）
	struct RewriteTrace {
		std::vector<AnchorTransfer> Transfers; // 錨の移譲の並び（適用順，合成前）
		std::unordered_map<uint64_t, AnchorMove> Moves; // 元の錨・置き方・文中かの組毎の合成した行先
		static uint64_t MoveKey(const NodeId Anchor, const AnchorSide Side, const bool IsInline); // 合成した行先の鍵の取得関数
		void Compose(); // 移譲の連鎖の合成関数
	};

	enum class RewriteKind : uint8_t { Insert, Move, Remove, RemoveSiblings, Reshape, Retarget, SetLexeme, Unwrap, Wrap }; // 書換の操作の種類 // 規則から生成する節点の追加 // 識別子と錨を保った節点の移動（移動先の場を指定出来る） // 節点の除去 // 同じ親の複数の子の一括除去 // 識別子を保った節点の組直（種別と子を生成する節点の物へ替え，残さない子の錨を移す） // 節点を除かない錨の移譲（宣言の分割） // 字句の字面の置換 // 包装の除去（唯一の名前付の子を包装の位置へ移し，包装と其の無名の字句を除く） // 包装の追加（生成する節点の中へ対象を移し，対象の位置・場へ生成した節点を置く）

	// 規則から生成する節点（既存の節点を移す位置は其の識別子を持つ）
	struct GeneratedNode {
		std::string Kind {}; // 名前の無い字句の種別を引く字面（Lexeme を省く場合は生成字句の字面にも使う）
		bool IsNamed = false; // 名前付の節点か
		std::vector<GeneratedNode> Children {}; // 子
		uint64_t Fields = 0; // 親の中の子の役割の集合（`SyntaxModel::FieldBit` の位の和）
		NodeId Existing = NoNode; // 此の位置へ移す既存の節点（生成する節点は無し）
		std::string Lexeme {}; // 子を持たない字句の字面（空は生成する字句の Kind に従う）
		const KindDefinition *Definition = nullptr; // 生成形式が与える種別（無ければ名前の無い字句の字面で引く）
		SyntaxFacts Facts {}; // 生成後に必要な構文事実（写しは元の事実を保持する）
		NodeId Source = NoNode; // 写し又は雛形の元の節点（子の境界を組直す時だけ参照する）
	};

	// 書換の１操作
	struct RewriteStep {
		RewriteKind Kind = RewriteKind::SetLexeme; // 操作の種類
		NodeId Target = NoNode; // 対象の節点（`Insert` は加える先の親）
		std::string Text {}; // 置換後の字面 (`SetLexeme`)
		AnchorTransfer Transfer {}; // 除く節点の錨の行先（`Remove`，`Wrap` は包む節点を `From` に置くと外の錨を括弧の前後へ移す）
		std::vector<AnchorTransfer> RemovedSiblings {}; // 同じ親から纏めて除く節点と各錨の行先 (`RemoveSiblings`)
		uint32_t Index = 0; // 加える又は移す先の子の位置 (`Insert` / `Move`)
		BoundaryKind Boundary = BoundaryKind::Glued; // 加える又は移す節点の前の境界（`Insert` / `Move`，隣接を記録した親だけ）
		bool RemoveFollowingBoundary = false; // 除く節点の後の境界も其の節点に属するなら除く (`Remove`)
		GeneratedNode Inserted {}; // 加える節点 (`Insert` / `Wrap` / `Reshape`)
		NodeId Destination = NoNode; // 移す先の親 (`Move`)
		std::optional<uint64_t> DestinationFields = std::nullopt; // 移す先での場（`Move`，無指定なら元の場を保持）
		std::optional<SemanticBoundary> RecordedBoundary = std::nullopt; // 元の子の前から移す境界（間隙の字面と由来を含む）
	};

	// 書換の計画（整形仕様 3.2，構文正規化が作り `SyntaxModel::Apply` が適用する）
	struct RewritePlan {
		std::vector<RewriteStep> Steps; // 操作の並び（適用順）
		std::vector<size_t> RuleStarts {}; // 規則毎の操作の始まり（複数の規則を纏めた計画だけ，整形仕様 5.1）
	};

	// カンマで並べる宣言の並び（宣言子，又は Go の名前・初期化子）
	struct DeclList {
		NodeId Host = NoNode; // 要素を並べる節点
		std::vector<std::vector<NodeId>> Items {}; // 要素毎の子の並び（Swift の宣言子は名前の型式・型の注釈・`=`・初期化子）
	};

	// カンマで並べる宣言の子（宣言と並びの節点の子を原文順に辿った物，要素の間の `,` を除く）
	struct DeclPiece {
		NodeId Node = NoNode; // 子
		int32_t List = -1; // 属する並びの番号（宣言子で共有する子は -1）
		uint32_t Item = 0; // 並びの中の要素の番号
	};

	// カンマで並べる宣言の部品（整形仕様 5.6，宣言子毎に型・初期化子を持つ宣言は宣言子の並び，Go は名前の並びと初期化子の並びを持つ）
	struct CommaDecl {
		NodeId Node = NoNode; // 宣言
		std::string Key {}; // 纏めれる宣言の照合の鍵（宣言と共有する子の種別の定義の同一性と字面）
		std::vector<DeclList> Lists {}; // 宣言子又は名前の並びと，初期化子の並び（原文順）
		std::vector<DeclPiece> Pieces {}; // 宣言の子（原文順）
	};

	class SourceProjector;

	// 正規化と中間表現の構築で共有する作業予算
	struct StructureWorkAccount {
		size_t Limit = std::numeric_limits<size_t>::max(); // 投影直後の基準量から定めた上限
		size_t Used = 0; // 計上済の作業単位
		bool IsClosed = false; // 中間表現の構築を終えたか
	};

	// 独自の節点表と構文事実を持つ構文モデル（整形仕様 3.2，tree-sitter の木の別名ではない）
	class SyntaxModel {
		friend class SourceProjector;

	private:

		static constexpr size_t WorkBudget = 0X80000; // 小さな入力でも足りる作業単位の固定費（整形仕様 9.2 の 524288）
		static constexpr size_t WorkPerBaseShift = 5; // 基準量１当たりの作業単位の桁送り（整形仕様 9.2 の３２倍）
		std::deque<SyntaxNode> Nodes; // 識別子で引く節点表（除いた節点も識別子を再利用しない為に残し，追加で既存の節点を動かさない）
		std::vector<std::string> ComparisonNames {}; // 節点が番号で引く識別子の比較用の名前
		std::vector<std::shared_ptr<const OwnedRegion>> Regions {}; // 節点が番号で引く所有区画（別に所有する出力を共有し，親の字句にしない）
		std::vector<uint8_t> IsRemoved; // 節点を除いたか
		std::array<std::vector<NodeId>, static_cast<size_t>(Role::Count)> RoleNodes {}; // 役割毎の節点番号順の索引
		std::unordered_map<const KindDefinition *, std::vector<Role>> KindRoles {}; // 共通の種別毎の役割
		std::vector<const KindDefinition *> IndexedKinds {}; // 節点毎に索引へ登録済の役割を持つ共通の種別（登録が無ければ無し）
		bool HasRoleIndex = false; // 投影後の役割索引を構築したか
		std::vector<NodeId> Following {}; // 構文の順で節点毎の後に続く物（要素の区切りを飛ばす，正規化の後に一度だけ作る）
		std::vector<NodeId> LastTouched {}; // 最後の計画が変更した節点と其の親
		std::optional<std::unordered_map<NodeId, SyntaxNode>> Journal {}; // 控えを始めた時点に在った節点の最初の変更前の値（控えて居なければ無し）
		size_t JournalEnd = 0; // 控えを始めた時点の節点表の大きさ
		NodeId Root = NoNode; // 根
		uint64_t ChildRoleMask = 0; // 文法が投影する子の役割
		const SyntaxRules *Definition = nullptr; // 文法の構文・意味の固定の定義
		std::shared_ptr<StructureWorkAccount> Work = std::make_shared<StructureWorkAccount>(); // 期待構文とは独立した作業予算
		SyntaxKind KindFor(const GeneratedNode &Generated) const; // 生成する節点の種別の取得関数
		NodeId AddNode(SyntaxNode &&Node); // 節点の追加関数
		const std::vector<Role> &KindRolesOf(const KindDefinition *const Definition); // 共通の種別の役割の並びの取得関数
		void IndexRoles(const NodeId Id); // 節点の現在の役割の索引への登録関数
		void Touch(const NodeId Id); // 書換が変更した節点の記録関数
		void Detach(const NodeId Id); // 親の子の並びからの切離関数
		NodeId AddGenerated(const GeneratedNode &Generated, const NodeId Parent, const uint32_t Index); // 規則から生成する節点の部分木の追加関数
		void ReshapeNode(const RewriteStep &Step, RewriteTrace &Trace); // 識別子を保った節点の組直関数

		void Attach(
			const NodeId Id,
			const NodeId Parent,
			const uint32_t Index,
			const BoundaryKind Boundary,
			const std::optional<SemanticBoundary> &RecordedBoundary
		); // 親の子の並びへの接続関数

		void UnwrapNode(const NodeId Id, RewriteTrace &Trace); // 包装の除去関数
		void TransferInnerAnchors(const AnchorTransfer &Transfer, RewriteTrace &Trace); // 除く節点の内部の錨の移譲関数
		void ApplyStep(const RewriteStep &Step, RewriteTrace &Trace, std::vector<AnchorTransfer> &RemovedRoots); // 書換の１操作の適用関数
		void FixWorkLimit(); // 投影を終えた構文の基準量からの作業単位の上限の確定関数

	public:

		NodeId GetRoot() const; // 根の取得関数
		size_t NodeCount() const; // 節点表の大きさの取得関数（除いた節点を含む）
		bool IsRemovedNode(const NodeId Id) const; // 除いた節点かの判定関数
		const SyntaxRules &Rules() const; // 文法の構文・意味の固定の定義の取得関数
		void Charge(const size_t Amount) const; // 構文の正規化と中間表現の構築の作業単位の計上関数
		std::shared_ptr<StructureWorkAccount> WorkAccount() const; // 作業予算の取得関数
		size_t GetWorkLimit() const; // 作業単位の上限の取得関数
		void CheckNesting() const; // ネストの段数の検査関数
		bool Is(const NodeId Id, const Role Wanted) const; // 役割を持つ節点かの判定関数
		std::span<const NodeId> NodesWith(const Role Wanted) const; // 役割を持つ節点の候補の取得関数
		std::span<const NodeId> TouchedNodes() const; // 最後の計画が変更した節点の取得関数
		const CanonicalForm *FormOf(const NodeId Id, const FormOperation Operation) const; // 節点の生成形式の取得関数
		const SyntaxNode &GetNode(const NodeId Id) const; // 節点の取得関数
		const SyntaxNode &JournaledNode(const NodeId Id) const; // 変更前の値の控えを始めた時点の節点の取得関数
		const OwnedRegion *RegionOf(const NodeId Id) const; // 所有区画の取得関数
		bool HasRegions() const; // 所有区画の有無の判定関数
		/** ========== 節点・字句・構文事実の取得 ========== */
		std::string_view LexemeOf(const NodeId Id) const; // 字句の字面の取得関数（節点が無ければ空）
		bool IsNamed(const NodeId Id) const; // 名前付の節点かの判定関数
		bool IsTokenRole(const NodeId Id, const Role Wanted) const; // 実際の綴りの字句の役割の判定関数
		std::span<const NodeId> ChildrenOf(const NodeId Id) const; // 子の並びの取得関数
		NodeId ParentOf(const NodeId Id) const; // 親の取得関数
		uint64_t FieldBit(const ChildRole Role) const; // 子の役割の位の取得関数
		NodeId FieldChild(const NodeId Id, const ChildRole Role) const; // 役割を持つ子の取得関数
		bool HasField(const NodeId Id, const ChildRole Role) const; // 親の中の子の役割の判定関数
		uint32_t ChildCount(const NodeId Id) const; // 子の数の取得関数
		NodeId Child(const NodeId Id, const uint32_t Index) const; // 子の取得関数
		uint32_t NamedChildCount(const NodeId Id) const; // 名前付の子の数の取得関数
		NodeId NamedChild(const NodeId Id, const uint32_t Index) const; // 名前付の子の取得関数
		NodeId LastNamedChild(const NodeId Id) const; // 最後の名前付の子の取得関数
		NodeId PrevSibling(const NodeId Id) const; // 前の兄弟の取得関数
		NodeId NextSibling(const NodeId Id) const; // 後の兄弟の取得関数
		NodeId PrevNamedSibling(const NodeId Id) const; // 前の名前付の兄弟の取得関数
		NodeId NextNamedSibling(const NodeId Id) const; // 後の名前付の兄弟の取得関数
		NodeId FollowingOf(const NodeId Id) const; // 構文の順で節点の後に続く物（要素の区切りを飛ばす）の取得関数
		// 正規化の規則・制御の流れ・署名・コメント
		NodeId FirstNamedChildWithRole(const NodeId Id, const Role Desired) const; // 役割を持つ最初の名前付の子の取得関数
		bool HasTokenChild(const NodeId Id, const std::string_view Token) const; // 指定の字面の無名の子を持つかの判定関数
		bool HasTokenRoleChild(const NodeId Id, const Role Wanted) const; // 指定の役割の無名の子を持つかの判定関数
		bool HasWordChild(const NodeId Id, const Word Meaning) const; // 語の意味の綴りの無名の子を持つかの判定関数
		bool IsWord(const NodeId Id, const Word Meaning) const; // 部分木の字面が語の意味の綴りかの判定関数
		bool IsBracedBody(const NodeId Node) const; // 波括弧で包んだ文の塊かの判定関数
		bool NeedsSeparatorToClose(const NodeId Id) const; // 空の本体を閉じの語と同じ行へ畳むと構文誤りに為るかの判定関数
		std::optional<CommaDecl> CommaDeclOf(const NodeId Node) const; // カンマで並べる宣言の部品の取得関数
		GeneratedNode CopyOf(const NodeId Id) const; // 部分木の写しの生成節点の取得関数
		void RemapGeneratedBoundaries(GeneratedNode &Generated) const; // 残る子の隣接に従う境界の組直関数
		bool Contains(const NodeId Ancestor, const NodeId Node) const; // 節点が部分木に含まれるかの判定関数
		bool Precedes(const NodeId Lhs, const NodeId Rhs) const; // 文書の順で前に在るかの判定関数
		NodeId IfConsequence(const NodeId Id) const; // 分岐の真枝の取得関数
		std::string Text(const NodeId Id) const; // 部分木の字句の字面を区切無で繋いだ字面の取得関数
		std::string ComparisonName(const NodeId Id) const; // 投影済の識別子の比較用の名前の取得関数
		char FirstChar(const NodeId Id) const; // 部分木の最初の字の取得関数
		char LastChar(const NodeId Id) const; // 部分木の最後の字の取得関数
		LexicalEdge FirstEdge(const NodeId Id) const; // 部分木の最初の字句と字の取得関数
		LexicalEdge LastEdge(const NodeId Id) const; // 部分木の最後の字句と字の取得関数
		bool IsEdgeRole(const LexicalEdge &Edge, const Role Wanted) const; // 部分木の端の字句の役割の判定関数
		bool FirstTokenRole(const NodeId Id, const Role Wanted) const; // 部分木の最初の字句の役割の判定関数
		bool LastTokenRole(const NodeId Id, const Role Wanted) const; // 部分木の最後の字句の役割の判定関数
		int BracketLeadDelta(const NodeId Id) const; // 括弧を開閉する字句の深さの変化の取得関数
		bool IsBracketLead(const NodeId Id, const Role Wanted) const; // 括弧を開閉する字句の役割の判定関数
		NodeId LastLeaf(NodeId Id) const; // 部分木の最後の葉の取得関数
		void BuildRoleIndex(); // 正規化の役割索引の構築関数
		void ReleaseRoleIndex(); // 正規化の役割索引の解放関数
		void BuildFollowingIndex(); // 構文の順で節点毎の後に続く物の索引の構築関数
		RewriteTrace Apply(const RewritePlan &Plan); // 書換の計画の適用関数
		void BeginJournal(); // 変更前の値の控えの開始関数
		void EndJournal(); // 変更前の値の控えの終了関数

		SyntaxModel(const SyntaxRules &Definition, const uint64_t ChildRoleMask); // コンストラクタ
	};

	enum class WalkStep : uint8_t { Descend, Skip, Stop }; // 前順の走査の各節点での次の動作 // 子へ降りる // 子へ降りない // 走査を止める

	/**
	 * 部分木の前順の走査関数
	 * @param Syntax 構文モデル
	 * @param Root 走査する部分木の根
	 * @param Visit 節点毎の動作を返す関数
	 * @return 走査を止めたなら true
	 */
	template<class Function> bool WalkNodes(const SyntaxModel &Syntax, const NodeId Root, Function &&Visit) {
		std::vector<NodeId> Stack { Root };
		while(!Stack.empty()) {
			const NodeId Id = Stack.back();
			Stack.pop_back();
			// 節点の１回の検査を１作業単位とする（整形仕様 9.2）
			Syntax.Charge(1);
			const WalkStep Step = Visit(Id);
			// 走査を止めた事の返戻
			if(Step == WalkStep::Stop) return true;
			if(Step == WalkStep::Skip) continue;
			const std::span<const NodeId> Children = Syntax.ChildrenOf(Id);
			Stack.insert(Stack.end(), Children.rbegin(), Children.rend());
		}
		// 最後迄辿った事の返戻
		return false;
	}

	/**
	 * 子の走査関数
	 * @param Syntax 構文モデル
	 * @param Id 親
	 * @param Visit 子毎の関数（真偽を返す物は偽で走査を止める）
	 */
	template<class Function> void ForEachChildOf(const SyntaxModel &Syntax, const NodeId Id, Function &&Visit) {
		for(const NodeId Child : Syntax.ChildrenOf(Id)) {
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
	 * @param Syntax 構文モデル
	 * @param Id 親
	 * @param Visit 名前付の子毎の関数（真偽を返す物は偽で走査を止める）
	 */
	template<class Function> void ForEachNamedChildOf(const SyntaxModel &Syntax, const NodeId Id, Function &&Visit) {
		for(const NodeId Child : Syntax.ChildrenOf(Id)) {
			// 子の１回の検査を１作業単位とする（整形仕様 9.2）
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
	 * 注記と開始行から後送された本文は独立した文として数えない
	 * 計算量：子の数 C に対し O(C)
	 * @param Syntax 構文モデル
	 * @param Id 文を並べる節点
	 * @param Visit 文毎の関数（真偽を返す物は偽で走査を止める）
	 */
	template<class Function> void ForEachStatementOf(const SyntaxModel &Syntax, const NodeId Id, Function &&Visit) {
		for(const NodeId Child : Syntax.ChildrenOf(Id)) {
			Syntax.Charge(1);
			if(!Syntax.IsNamed(Child) || Syntax.Is(Child, Role::Comment) || Syntax.Is(Child, Role::DeferredLiteralBody)) continue;
			if constexpr(std::is_same_v<std::invoke_result_t<Function, NodeId>, bool>) {
				if(!Visit(Child)) break;
			} else Visit(Child);
		}
		// 終了
		return;
	}

	/**
	 * 述語を満たす子の有無の判定関数
	 * @param Syntax 構文モデル
	 * @param Id 親
	 * @param Predicate 子を受け取る述語
	 * @return 述語を満たす子が有れば true
	 */
	template<class Function> bool AnyChildOf(const SyntaxModel &Syntax, const NodeId Id, Function &&Predicate) {
		for(const NodeId Child : Syntax.ChildrenOf(Id)) {
			// 子の１回の検査を１作業単位とする（整形仕様 9.2）
			Syntax.Charge(1);
			// 述語を満たす子の発見の返戻
			if(Predicate(Child)) return true;
		}
		// 述語を満たす子の無い事の返戻
		return false;
	}

	/**
	 * 述語を満たす節点の部分木の中の有無の判定関数（根を含む）
	 * @param Syntax 構文モデル
	 * @param Id 部分木の根
	 * @param Predicate 節点を受け取る述語
	 * @return 述語を満たす節点が有れば true
	 */
	template<class Function> bool AnyNodeOf(const SyntaxModel &Syntax, const NodeId Id, Function &&Predicate) {
		// 述語を満たす節点で止めた走査の結果の返戻
		return Id != NoNode && WalkNodes(
			Syntax,
			Id,
			[&Predicate](const NodeId Node) -> WalkStep {
				// 述語を満たせば止める事の返戻
				return Predicate(Node) ? WalkStep::Stop : WalkStep::Descend;
			}
		);
	}
} // namespace Core
