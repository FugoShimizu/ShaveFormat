#pragma once

#include "CommentFormat.hpp"
#include "NodeId.hpp"
#include "SyntaxModel.hpp"
#include "SyntaxRole.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// 文法の読取規則の前方宣言
struct ReadingRules;

// 確定したコード配置の前方宣言
class LaidOutCode;
// 共有するコメント本文構造の前方宣言
class CommentStructure;
// コメント本文の正規化の前方宣言
class CommentNormalizer;

enum class CommentKind : uint8_t { OwnLine, Inline, Trailing }; // 原文でのコメントの種別（整形仕様 3.2，整形後も同じ種別の位置へ置く）

// コメントの所属（整形仕様 3.2，アンカーとの関係と，其の関係の中の位置）
struct CommentPlacement {
	enum Relation : uint8_t { Before, After, Inside }; // アンカーとの関係 // アンカーの前 // アンカーの後 // アンカーの中の区切・閉じのトークンの前
	Relation Side = Before; // アンカーとの関係
	uint16_t TokensBefore = 0; // 原文で先行コメントとアンカーの間に在った無名トークンの数（配置の段は其のトークンの前へ置き，アンカーの中の物は其の前のアンカーの無名トークンの数，文中の末尾コメントはアンカーとの間の数で其のトークンの後へ置く）
	bool IsGluedAfter = false; // 後のトークンへ空白無で続く文中コメントか（閉じの直後のトークンの位置を定める Go の `/*line`）
	bool IsEmptyBody = false; // 畳んで空に為る本体の中に在った独立コメントか（アンカーの頭でなく畳んだ本体の在る行の前へ置く）
	bool IsRemovedCaseBody = false; // 空文と共に消える `case` 本体からアンカーを移したコメントか
};

enum class CommentRole : uint8_t { Presentation, Directive }; // コメントの役割（整形仕様 3.2，通常の説明か，整形仕様 4.5 の対応範囲に基付いて保護する標準のディレクティブコメントか）

enum class CommentNotation : uint8_t {
	SlashLine, // `//`
	TripleSlashLine, // `///`
	SlashBangLine, // `//!`
	GoBuildLine, // `//go:build`
	GoDebugLine, // `//go:debug`
	GoEmbedLine, // `//go:embed`
	GoGenerateLine, // `//go:generate`
	GoLinknameLine, // `//go:linkname`
	GoNoescapeLine, // `//go:noescape`
	GoNoinlineLine, // `//go:noinline`
	GoNoraceLine, // `//go:norace`
	GoNosplitLine, // `//go:nosplit`
	GoUintptrescapesLine, // `//go:uintptrescapes`
	GoWasmexportLine, // `//go:wasmexport`
	GoWasmimportLine, // `//go:wasmimport`
	GoLineLine, // `//line`
	GoLineBlock, // `/*line ... */`
	CgoExportLine, // `//export`
	GccgoExternLine, // `//extern`
	HashLine, // `#`
	HashBangLine, // `#!`
	HashColonLine, // `#:`
	Block, // `/* ... */`
	DoubleStarBlock, // `/** ... */`
	StarBangBlock, // `/*! ... */`
	RubyEmbeddedDocument // 行頭の `=begin` から行頭の `=end` 迄
}; // コメントの記法（整形仕様 3.2，開始・終端を表す２６値で，種別・所属・役割とは独立して記録する） // 名前付のディレクティブ記法を連続した値の範囲で判定する為に宣言順を保つ

enum class DirectiveDefinitionId : uint8_t {
	Shebang, // 標準が定める先頭の `#!`
	BlockLinePos, // `/*line ... */`
	CgoExport, // `//export`
	CgoPreamble, // `import "C"` の前文
	EncodingDecl, // エンコーディング宣言
	EncodingPrecondition, // ２行目のエンコーディング宣言の成立条件である１行目
	ExampleOutput, // `Example` 関数の出力コメント
	GccgoExtern, // `//extern`
	GoBuild, // `//go:build`
	GoDebug, // `//go:debug`
	GoEmbed, // `//go:embed`
	GoFunctionPragma, // 直後の関数宣言へ作用するコンパイラのディレクティブ（`//go:noinline` 等）
	GoGenerate, // `//go:generate`
	GoLinkname, // `//go:linkname`
	LinePos, // `//line`
	MagicComment, // 頭部のマジックコメント
	TypeScriptRef // 三重スラッシュディレクティブ
}; // 登録済のディレクティブ記法の定義の識別子（整形仕様 4.5 の登録定義と言語仕様上のディレクティブ）

enum class DirectiveState : uint8_t { Active, Inactive }; // ディレクティブの成立（整形仕様 3.2，ビルド条件式の真偽とは異なる） // 成立 // 不成立

// 登録済のディレクティブ記法の記録（整形仕様 3.2，原文の所有者・任意の処理への参照を持たず，中核の工程へ渡さない）
struct DirectiveInfo {
	DirectiveDefinitionId Definition = DirectiveDefinitionId::Shebang; // 登録定義
	DirectiveState State = DirectiveState::Active; // 位置・区切の条件に依る成立
	std::vector<std::string> Conditions; // 成立・保持を照合する本文と値
};

// 本文世代に対して１度確定する出力記法
struct CommentOutput {
	enum class Change : uint8_t { None, DocNotation, Frame, PlainLine }; // 記法への変方
	Change Transformation = Change::None; // 確定した変方
	CommentNotation Notation = CommentNotation::SlashLine; // 出力の記法
	std::optional<std::string> Text; // 記法に依り変わる本文（変わらない物は元の本文を共有する）
	bool ShouldAttachFirstLine = false; // 出力の開始記号への密着
	bool ShouldFrame = false; // 論理行の確定後に囲みの枠を付けるか
};

// コメントの本文・所属・入力記法・出力記法の記録
struct CommentRecord {
	std::string Text; // コメント本文
	CommentPlacement Placement; // アンカーとの関係（整形仕様 3.2）
	size_t OriginalOrder = 0; // 原文での順（配置位置が一致するコメントの整列と，グループ内の並びの保持に用いる，整形仕様 3.2）
	CommentKind Kind = CommentKind::OwnLine; // 原文でのコメントの種類（整形で補ったコメントは独立コメント）
	CommentRole Role = CommentRole::Presentation; // 通常の説明か，整形仕様 4.5 の対応範囲に基付いて保護する標準のディレクティブコメントか（整形仕様 3.2）
	CommentNotation Notation = CommentNotation::SlashLine; // 開始・終端を表す記法（整形仕様 3.2）
	Core::CommentFormat Format {}; // 投影時に確定する記法とドキュメントの構造
	Core::NodeId Anchor = Core::NoNode; // アンカーのノード（整形仕様 3.2，ノード表の識別子）
	bool IsEmbeddedEdge = false; // ドキュメントの論理行に含めない埋込ドキュメントの空の縁
	std::optional<size_t> StructureIndex {}; // 共有構造の記録位置（本文変更時に解除し，構造確定時に更新する）
	std::optional<CommentOutput> Output {}; // 本文世代に対して確定した出力記法
	std::optional<DirectiveInfo> Directive {}; // 登録済のディレクティブ記法の記録（無効な位置の物も記録する，整形仕様 3.2，4.5）
};

using CommentRecordList = std::vector<CommentRecord>; // 原文順のコメントの記録

namespace Core {
	// ドキュメント化対象の定義（整形仕様 3.2，コメントの有無に依らず投影が作り，アンカーは整形仕様 7.2 の移譲に従う）
	struct DocumentationSite {
		NodeId Anchor = NoNode; // 定義のアンカー
		const NotationRules *Rules = nullptr; // 共有する言語の記法の定義
	};

	// 実行時設定の組（整形仕様 4.5）
	struct MagicCommentPair {
		std::string Name; // 大小文字と連結の字を揃えた名前
		std::string_view Val; // 引用符の内側の値
	};

	// コメント経路が共有するノード毎のドキュメントの所属
	struct DocTargetTables {
		std::vector<NodeId> Decls; // 所属する宣言
		std::vector<NodeId> Enclosing; // アンカーの囲む本体の所有者
		std::vector<uint8_t> IsInDefBody; // 定義の本体の中か
		std::vector<NodeId> Defs; // アンカー・ラッパー・見出のノードが属する定義
		std::vector<uint8_t> IsDefs; // ノード自身がドキュメント化対象の定義か
		std::vector<NodeId> Anchors; // 定義毎の先行する属性・ラッパーを含むアンカー
	};

	// アンカーのノード毎のコメント記録の管理クラス（整形仕様 3.2，本文と配置は此処だけが持ち，中核の工程へ渡さない）
	class CommentRepository {
	private:

		// 記録へ直接アクセスするコメントの工程
		friend class ::CommentNormalizer;
		friend class CommentPlacer;
		friend class DocCommentSynchronizer;
		friend class DocCommentReader;
		friend class SourceProjector;

		using OutputChange = CommentOutput::Change; // 出力記法への変方

		// 登録語とエンコーディングの名前の範囲
		struct EncodingDecl {
			size_t Start; // 登録語の位置
			size_t NameStart; // 名前の始まり
			size_t NameEnd; // 名前の終わり
		};

		// 所属と本文世代を共有する記録と構造
		std::optional<DocTargetTables> TargetTables; // 同じ構文の同期・所属確定・検証・静的解析が共有する表
		std::vector<NodeId> DocTargets; // 所属確定済の説明対象
		std::vector<NodeId> PlacementTargets; // 確定した配置の説明対象
		std::shared_ptr<const CommentStructure> Structure; // 同じ本文世代の共有構造
		CommentRecordList Records; // 原文順の記録
		std::unordered_map<NodeId, std::vector<size_t>> AnchorIdx; // 同じ記録世代のアンカーのインデックス
		std::vector<DocumentationSite> DocumentationSites; // ドキュメント化対象の定義（原文順）
		std::unordered_map<NodeId, std::shared_ptr<const CommentRepository>> OwnedChildren; // 子が所有する記録（通常記録の取得・同期・正規化へ公開せず，配置だけが読む）
		// 記法の分解と出力記法の内部操作
		static std::string_view TrimTrailingSeparator(std::string_view Line); // 後の区切空白を除いたテキストの取得関数
		static std::optional<size_t> GetDecorationWidth(const std::string_view Line, bool &IsDecorated); // 継続行の装飾と区切空白の長さの取得関数
		static std::vector<std::string> GetBlockLines(const NotationRules &Rules, const std::string_view Interior); // 囲みの内側の論理行の取得関数
		static std::string GetHeadedLine(const std::string_view Line, const std::string_view Head); // 記号の一部の頭と本文の間の区切空白を揃えた論理行の取得関数
		static bool IsVerbatimRecord(const CommentRecord &Record); // 本文を１バイトも変えない記録かの判定関数
		static bool IsSymbolsOnly(const std::string_view Line); // 本文の無い記号だけの行かの判定関数

		static std::optional<EncodingDecl> FindEncodingDecl(
			const RegisteredLineForms &Forms,
			const std::string_view Text,
			const bool ShouldFindLast,
			StructureWorkAccount *const Work = nullptr
		); // エンコーディング宣言の登録語と名前の範囲の取得関数

		static bool HasEncodingDecl(const RegisteredLineForms &Forms, const std::string_view Text); // 組として読めない注記のエンコーディングの宣言の判定関数

		static std::optional<std::vector<MagicCommentPair>> MagicCommentPairsOf(
			const RegisteredLineForms &Forms,
			std::string_view Text,
			const std::optional<size_t> MarkerAt = std::nullopt,
			StructureWorkAccount *const Work = nullptr
		); // 実行時設定の組の並びの取得関数

		static OutputChange OutputChangeOf(const CommentRecord &Record); // 出力記法への変方の取得関数
		static CommentNotation GetOutputNotationFor(const CommentRecord &Record, const OutputChange Change); // 変方に従う出力記法の取得関数
		void Reindex(size_t *const Work = nullptr); // 所有者が更新した記録のアンカーのインデックスの確定

	public:

		enum class LinePlacement : uint8_t { None, GeneralBlock, LineRecord, OtherLine }; // 配置の段が採用する行記法で置く記録の種類（整形仕様 7.6） // 行記法へ揃えない記録 // 一般の囲みとドキュメントでない位置のドキュメントの囲み // 論理行毎に記法を選ぶ採用記法の行コメントと埋込ドキュメントの独立した記録 // 採用しない行記法とドキュメントでない位置のドキュメントの行記法

		// 原文のエンコーディング宣言の位置と値の範囲
		struct EncodingHeader {
			size_t End = 0; // 処理系が宣言を読む範囲の終わり
			size_t FirstLineEnd = 0; // １行目の終わり
			size_t DeclarationStart = 0; // 最初の有効な宣言を含むコメントの始まり
			size_t NameStart = 0; // 最初の有効な名前の始まり
			size_t NameEnd = 0; // 名前の終わり（始まりと等しければ宣言無し）
			bool HasSecondLineEncoding = false; // ２行目の宣言に１行目のコメントが必要か
		};

		// 行コメントの登録形の判定結果（整形仕様 4.5）
		struct RegisteredLineForm {
			std::optional<std::vector<MagicCommentPair>> Pairs; // 実行時設定の組の並び（組として読めない注記は無い）
			bool IsEncoding = false; // 処理系がエンコーディングの指定として読む形か
			bool IsMagic = false; // 実行時設定の登録された名前と有効な値の組を持つか
		};

		static bool IsBlockNotation(const CommentNotation Notation); // 囲みの記法かの判定関数
		static const NotationRules &GetRulesOf(const CommentRecord &Record); // 記録の記法の定義の取得関数
		static bool IsMemberDoc(const NotationRules &Rules, const CommentRecord &Record); // 前のメンバのドキュメントかの判定関数
		// コメント記法と本文の共通操作
		static std::string_view GetEncodingName(const RegisteredLineForms &Forms, const std::string_view Text); // 登録定義に従うエンコーディング宣言の値の取得関数
		static bool IsNamedDirective(const CommentNotation Notation); // 名前迄を記号に含むディレクティブ記法かの判定関数
		static std::string_view TrimLeadingSeparator(std::string_view Line); // 前の区切空白を除いたテキストの取得関数
		static std::string_view GetOpenerOf(const CommentNotation Notation); // 記法の開始記号の取得関数
		static std::string_view GetCloserOf(const CommentNotation Notation); // 記法の終端記号の取得関数

		static EncodingHeader ReadEncodingHeader(
			const ReadingRules &Reading,
			const RegisteredLineForms &Forms,
			const std::string_view Text
		); // 原文から処理系が読むエンコーディング宣言の位置と値の範囲を得る関数

		static std::pair<std::string_view, std::string_view> ReadShebangEncoding(const ReadingRules &Reading, std::string_view Line); // シバンの先の指定と後の指定のエンコーディングの値を得る関数
		static LinePlacement GetLinePlacementOf(const CommentRecord &Record, const bool IsDoc); // 配置の段が採用する行記法で置く記録の種類の取得関数
		static CommentNotation GetOutputNotationOf(const CommentRecord &Record); // 出力記法の取得関数（記録をコピーしない）
		static bool IsSpacedDirective(const CommentNotation Notation); // 記号と本文の間の空白を揃えるディレクティブ記法かの判定関数
		static bool IsValSpacing(const DirectiveDefinitionId Def); // 記号の後の空白を処理系が値として読むディレクティブの定義かの判定関数
		static bool IsLineHeadWord(const std::string_view Line, const std::string_view Word); // 行頭の語かの判定関数
		static std::string_view GetSymbolHeadOf(const CommentRecord &Record, const std::string_view Body); // 開始記号の一部として密着させる本文の頭の取得関数
		static std::string_view TrimDirectiveSeparator(const std::optional<DirectiveDefinitionId> Def, const std::string_view Line); // ディレクティブの前の区切空白を除いたテキストの取得関数
		static bool HasValidLinePos(const std::string_view Text, const CommentNotation Notation); // 位置指示の本文の成立判定関数
		static bool IsMagicCommentValue(const std::string_view Val, const std::span<const std::string_view> Vals); // 実行時設定の値の照合関数

		static RegisteredLineForm RegisteredLineFormOf(
			const RegisteredLineForms &Forms,
			const std::string_view Text,
			const std::optional<size_t> MarkerAt = std::nullopt,
			const std::optional<bool> HasEncoding = std::nullopt,
			StructureWorkAccount *const Work = nullptr
		); // 行コメントの登録形の判定関数

		static bool WouldJoinEncodingDecl(
			const RegisteredLineForms &Forms,
			const std::string_view Text,
			const size_t GapStart,
			const size_t GapEnd
		); // 空白を除くとエンコーディング宣言に為る区切かの判定関数

		static std::vector<size_t> GetRegisteredSentenceStarts(
			const RegisteredLineForms &Forms,
			const std::string_view Text,
			const std::span<const size_t> Starts,
			StructureWorkAccount *const Work = nullptr
		); // 行記法の登録形へ読替わる後続文の開始位置の取得関数

		static CommentNotation NotationOf(const NotationRules &Rules, const std::string_view Text, const bool IsSrcHead); // コメント記法の判定関数
		static CommentNotation GetGroupNotationOf(const CommentRecord &Record, const bool IsDoc); // グループを纏める記法の取得関数
		static std::string_view GetOutputTextOf(const CommentRecord &Record); // 共有する出力本文の取得関数
		static void ResolveOutput(CommentRecord &Record); // 本文世代の出力記法の確定関数
		static bool IsFileHeadDirective(const DirectiveDefinitionId Def); // ファイルの頭部へ置く登録定義かの判定関数
		static void Decompose(CommentRecord &Record); // 原文の形のテキストから記号と区切空白を除いた本文への分解関数
		static std::string Compose(const CommentRecord &Record); // 記号と本文の合成関数
		static bool ShouldActivateAtLineHead(const std::string_view Text, const CommentNotation Notation); // 行頭（桁０）へ置くと成立する記法かの判定関数
		static void AttachRules(const NotationRules &Rules, CommentRecord &Record); // 記録への記法の定義と入力のドキュメント形式の記録関数
		static bool HasLineCommentBreak(const NotationRules &Rules, const std::string_view Text); // 行コメントを途中で終える LF 以外の改行を含むかの判定関数
		static bool CanHoldInLineComment(const NotationRules &Rules, const std::string_view Body, const bool IsDecoded = false); // 論理行の本文を行コメントで保てるかの判定関数
		static bool IsPlainLineText(const NotationRules &Rules, const std::string_view Text, const CommentNotation Notation); // 通常コメントの行のテキストかの判定関数
		static bool IsBlockLineDoc(const CommentRecord &Record); // 囲みのドキュメントへ揃える行形式のドキュメントかの判定関数
		static bool CanFrame(const NotationRules &Rules, const std::string_view Text); // 本文を囲みのドキュメントへ入れれるかの判定関数

		static std::vector<CommentGroupId> MembershipOf(
			const std::span<const CommentRecord> Records,
			const std::span<const uint8_t> IsDocs
		); // グループの所属の取得関数

		static CommentRecord GetOutputFormOf(const CommentRecord &Record); // 出力記法の記録の取得関数
		// 確定した記録と共有構造の読取・更新操作
		const CommentStructure *GetStructure() const; // 未判定を含む共有構造の取得関数
		const CommentStructure &GetOutputStructure() const; // 出力記法の共有本文構造の取得関数
		CommentRecordList Get(const NodeId Anchor) const; // アンカーに属する記録を原文順に取得する操作
		std::span<const CommentRecord> GetRecords() const; // 全ての記録の取得関数
		std::span<const DocumentationSite> GetDocumentationSites() const; // ドキュメント化対象の定義の取得関数
		std::span<const NodeId> GetDocTargets() const; // 所属確定済の説明対象の取得関数
		std::span<const NodeId> GetPlacementTargets() const; // 配置確定済の説明対象の取得関数
		void AddDocSite(const DocumentationSite &Site); // ドキュメント化対象の定義の追加関数
		void Add(CommentRecord &&Record); // 記録の追加関数
		void Update(CommentRecordList &&Replacement); // 記録の差替関数
		void SetStructure(CommentStructure &&Replacement, size_t *const Work = nullptr); // 本文構造の確定関数
		std::span<CommentRecord> EditRecords(); // 本文更新段の記録の取得関数
		void Remap(const RewriteTrace &Trace); // アンカーの移譲の適用関数
	};
} // namespace Core
