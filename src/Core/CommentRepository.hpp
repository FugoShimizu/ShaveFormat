#pragma once

#include "CommentFormat.hpp"
#include "NodeId.hpp"
#include "SyntaxModel.hpp"
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

enum class CommentKind : uint8_t { OwnLine, Inline, Trailing }; // 原文でのコメントの種別（整形仕様 3.2，整形後も同じ種別の位置へ置く）

// コメントの所属（整形仕様 3.2，錨との関係と，其の関係の中の位置）
struct CommentPlacement {
	enum Relation : uint8_t { Before, After, Inside }; // 錨との関係 // 錨の前 // 錨の後 // 錨の中の区切り・閉じの字句の前
	Relation Side = Before; // 錨との関係
	uint16_t TokensBefore = 0; // 原文で先行コメントと錨の間に在った無名字句の数（配置の段は其の字句の前へ置き，錨の中の物は其の前の錨の無名字句の数，文中の末尾コメントは錨との間の数で其の字句の後へ置く）
	bool IsGluedAfter = false; // 後の字句へ空白無で続く文中コメントか（閉じの直後の字句の位置を定める Go の `/*line`）
	bool IsEmptyBody = false; // 畳んで空に為る本体の中に在った独立コメントか（錨の頭でなく畳んだ本体の在る行の前へ置く）
	bool IsRemovedCaseBody = false; // 空文と共に消える case 本体から錨を移したコメントか
};

enum class CommentRole : uint8_t { Presentation, Directive }; // コメントの役割（整形仕様 3.2，通常の説明か，整形仕様 4.5 の対応範囲に基付いて保護する標準の指示コメントか）

// 名前付の指示記法を連続した値の範囲で判定する為に宣言順を保つ
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
}; // コメントの記法（整形仕様 3.2，開始・終端を表す２６値で，種別・所属・役割とは独立して記録する）

enum class DirectiveDefinitionId : uint8_t {
	Shebang, // 標準が定める先頭の `#!`
	BlockLinePosition, // `/*line ... */`
	CgoExport, // `//export`
	CgoPreamble, // `import "C"` の前文
	DocumentAttribute, // 処理系が属性として読む文書の記法
	EncodingDeclaration, // 符号化宣言
	EncodingPrecondition, // ２行目の符号化宣言の成立条件である１行目
	ExampleOutput, // Example 関数の出力注記
	GccgoExtern, // `//extern`
	GoBuild, // `//go:build`
	GoDebug, // `//go:debug`
	GoEmbed, // `//go:embed`
	GoFunctionPragma, // 直後の関数宣言へ作用するコンパイラの指示子（`//go:noinline` 等）
	GoGenerate, // `//go:generate`
	GoLinkname, // `//go:linkname`
	LinePosition, // `//line`
	MagicComment, // 頭部のマジックコメント
	TypeScriptReference // 三重スラッシュ指示子
}; // 登録済の指示記法の定義の識別子（整形仕様 4.5 の登録定義と言語仕様上の指示子）

enum class DirectiveState : uint8_t { Active, Inactive }; // 指示子の成立（整形仕様 3.2，`Active` は成立，`Inactive` は不成立で，ビルド条件式の真偽とは異なる）

// 登録済の指示記法の記録（整形仕様 3.2，原文の所有者・任意の処理への参照を持たず，中核の工程へ渡さない）
struct DirectiveInfo {
	DirectiveDefinitionId Definition = DirectiveDefinitionId::Shebang; // 登録定義
	DirectiveState State = DirectiveState::Active; // 位置・区切りの条件に依る成立
	std::vector<std::string> Conditions; // 成立・保持を照合する本文と値
};

struct CommentRecord {
	std::string Text; // コメント本文
	CommentPlacement Placement; // 錨との関係（整形仕様 3.2）
	size_t OriginalOrder = 0; // 原文での順（同じ錨へ集まったコメントの安定した並びにだけ用いる，整形仕様 3.2）
	CommentKind Kind = CommentKind::OwnLine; // 原文でのコメントの種類（整形で補ったコメントは独立コメント）
	CommentRole Role = CommentRole::Presentation; // 通常の説明か，整形仕様 4.5 の対応範囲に基付いて保護する標準の指示コメントか（整形仕様 3.2）
	CommentNotation Notation = CommentNotation::SlashLine; // 開始・終端を表す記法（整形仕様 3.2）
	Core::CommentFormat Format {}; // 投影時に確定する記法と文書の構造
	Core::NodeId Anchor = Core::NoNode; // 錨の節点（整形仕様 3.2，節点表の識別子）
	std::optional<DirectiveInfo> Directive {}; // 登録済の指示記法の記録（無効な位置の物も記録する，整形仕様 3.2, 4.5）
};

using CommentRecordList = std::vector<CommentRecord>; // 原文順のコメントの記録

namespace Core {
	// 文書化対象の定義（整形仕様 3.2，コメントの有無に依らず投影が作り，錨は整形仕様 7.2 の移譲に従う）
	struct DocumentationSite {
		NodeId Anchor = NoNode; // 定義の錨
		const NotationRules *Rules = nullptr; // 共有する言語の記法の定義
	};

	// 実行時設定の組（整形仕様 4.5）
	struct MagicCommentPair {
		std::string Name; // 大小文字と連結の字を揃えた名前
		std::string_view Value; // 引用符の内側の値
	};

	// 錨の節点毎のコメントの器（整形仕様 3.2，本文と配置は此処だけが持ち，中核の工程へ渡さない）
	class CommentRepository {
	private:

		enum class OutputChange : uint8_t { None, DocumentNotation, Frame, PlainLine, Separator }; // 出力記法への変え方（整形仕様 7.6） // 変えない // 同じ文書形式の文書の囲み // 囲みの文書へ揃える行形式の文書 // 通常コメントの記法へ揃える埋込文書 // 区切の記法
		CommentRecordList Records; // 原文順の記録
		std::vector<DocumentationSite> DocumentationSites; // 文書化対象の定義（原文順）
		std::unordered_map<NodeId, std::vector<size_t>> ByAnchor; // 錨毎の記録の位置（記録の並びの順）
		static std::string_view TrimTrailingSeparator(std::string_view Line); // 後の区切空白を除いた字面の取得関数
		static std::vector<std::string> BlockLines(const std::string_view Interior); // 囲みの内側の論理行の取得関数
		static std::string HeadedLine(const std::string_view Line, const std::string_view Head); // 記号の一部の頭と本文の間の区切空白を揃えた論理行の取得関数
		static bool IsVerbatimRecord(const CommentRecord &Record); // 本文を１バイトも変えない記録かの判定関数
		static bool IsSymbolsOnly(const std::string_view Line); // 本文の無い記号だけの行かの判定関数
		static std::optional<std::string_view> SeparatorBodyOf(const CommentRecord &Record); // 区切コメントの本文の取得関数
		static bool HasEncodingDeclaration(const RegisteredLineForms &Forms, const std::string_view Text); // 組として読めない注記の符号化の宣言の判定関数

		static std::optional<std::vector<MagicCommentPair>> MagicCommentPairsOf(
			const RegisteredLineForms &Forms,
			std::string_view Text
		); // 実行時設定の組の並びの取得関数

		static OutputChange OutputChangeOf(const CommentRecord &Record); // 出力記法への変え方の取得関数
		static CommentNotation OutputNotationFor(const CommentRecord &Record, const OutputChange Change); // 変え方に従う出力記法の取得関数
		void Reindex(); // 錨毎の記録の位置の再構築関数

	public:

		enum class LinePlacement : uint8_t { None, GeneralBlock, LineRecord, OtherLine }; // 配置の段が採用する行記法で置く記録の種類（整形仕様 7.6，`GeneralBlock` は一般の囲みと文書でない位置の文書の囲み，`LineRecord` は論理行毎に記法を選ぶ採用記法の行コメントと埋込文書の独立した記録，`OtherLine` は採用しない行記法と文書でない位置の文書の行記法）

		// 行コメントの登録形の判定結果（整形仕様 4.5）
		struct RegisteredLineForm {
			std::optional<std::vector<MagicCommentPair>> Pairs; // 実行時設定の組の並び（組として読めない注記は無し）
			bool IsEncoding = false; // 処理系が符号化の指定として読む形か
			bool IsMagic = false; // 実行時設定の登録語の組を持つか
		};

		static bool IsNamedDirective(const CommentNotation Notation); // 名前迄を記号に含む指示記法かの判定関数
		static bool IsFileHeadDirective(const DirectiveDefinitionId Definition); // ファイルの頭部へ置く登録定義かの判定関数
		static bool IsSpacedDirective(const CommentNotation Notation); // 記号と本文の間の空白を揃える指示記法かの判定関数
		static bool IsValueSpacing(const DirectiveDefinitionId Definition); // 記号の後の空白を処理系が値として読む指示子の定義かの判定関数
		static std::string_view TrimLeadingSeparator(std::string_view Line); // 前の区切空白を除いた字面の取得関数

		static std::string_view TrimDirectiveSeparator(
			const std::optional<DirectiveDefinitionId> Definition,
			const std::string_view Line
		); // 指示子の前の区切空白を除いた字面の取得関数

		static std::string_view OpenerOf(const CommentNotation Notation); // 記法の開始記号の取得関数
		static bool IsBlockNotation(const CommentNotation Notation); // 囲みの記法かの判定関数
		static std::string_view CloserOf(const CommentNotation Notation); // 記法の終端記号の取得関数
		static bool IsLineHeadWord(const std::string_view Line, const std::string_view Word); // 行頭の語かの判定関数
		static const NotationRules &RulesOf(const CommentRecord &Record); // 記録の記法の定義の取得関数
		static std::string_view SymbolHeadOf(const CommentRecord &Record, const std::string_view Body); // 開始記号の一部として密着させる本文の頭の取得関数
		static void Decompose(CommentRecord &Record); // 原文の形の字面から記号と区切空白を除いた本文への分解関数
		static std::string Compose(const CommentRecord &Record); // 記号と本文の合成関数
		static bool HasValidLinePosition(const CommentNotation Notation, const std::string_view Text); // 位置指示の本文の成立判定関数
		static bool ActivatesAtLineHead(const CommentNotation Notation, const std::string_view Text); // 行頭（桁０）へ置くと成立する記法かの判定関数
		static void FrameDocument(CommentRecord &Record); // 複数の論理行を持つ文書の囲みの開始記号と終端記号を独立した行に置く関数
		static void AttachRules(const NotationRules &Rules, CommentRecord &Record); // 記録への記法の定義と入力の文書形式の記録関数
		static CommentNotation NotationOf(const NotationRules &Rules, const std::string_view Text, const bool IsSourceHead); // コメント記法の判定関数
		static RegisteredLineForm RegisteredLineFormOf(const RegisteredLineForms &Forms, const std::string_view Text); // 行コメントの登録形の判定関数
		static bool CanHoldInLineComment(const NotationRules &Rules, const std::string_view Body); // 論理行の本文を行コメントで保てるかの判定関数
		static bool IsPlainLineText(const NotationRules &Rules, const std::string_view Text, const CommentNotation Notation); // 通常コメントの行の字面かの判定関数
		static bool IsSeparator(const CommentRecord &Record); // 区切コメントの記録かの判定関数
		static bool IsSeparatorText(const NotationRules &Rules, const std::string_view Text); // 区切コメントの字面かの判定関数
		static bool IsMemberDocument(const NotationRules &Rules, const CommentRecord &Record); // 前の成員の文書かの判定関数
		static bool IsBlockLineDocument(const CommentRecord &Record); // 囲みの文書へ揃える行形式の文書かの判定関数
		static bool CanFrame(const NotationRules &Rules, const std::string_view Text); // 本文を囲みの文書へ入れれるかの判定関数
		static CommentNotation OutputNotationOf(const CommentRecord &Record); // 出力記法の取得関数（記録を写さない）
		static LinePlacement LinePlacementOf(const CommentRecord &Record, const bool IsDocument); // 配置の段が採用する行記法で置く記録の種類の取得関数
		static CommentNotation GroupNotationOf(const CommentRecord &Record, const bool IsDocument); // 群を纏める記法の取得関数

		static std::vector<CommentGroupId> MembershipOf(
			const std::span<const CommentRecord> Records,
			const std::span<const uint8_t> IsDocuments
		); // 群の所属の取得関数

		static std::vector<bool> EmbeddedEdgesOf(const std::span<const CommentRecord> Records); // 埋込文書の前後の空の行の印の取得関数
		static CommentRecord OutputFormOf(const CommentRecord &Record); // 出力記法の記録の取得関数
		static bool IsDocumentAttribute(const NotationRules &Rules, const CommentNotation Notation); // コメントの記法が処理系の読む文書属性に為るかの判定関数
		std::span<const CommentRecord> GetRecords() const; // 全ての記録の取得関数
		std::span<const DocumentationSite> GetDocumentationSites() const; // 文書化対象の定義の取得関数
		CommentRecordList Get(const NodeId Anchor) const; // 錨の記録の取得関数
		void AddDocumentationSite(const DocumentationSite &Site); // 文書化対象の定義の追加関数
		void Add(CommentRecord &&Record); // 記録の追加関数
		void Update(CommentRecordList &&Replacement); // 記録の差替関数
		void UpdateTexts(std::vector<std::pair<size_t, std::string>> &&Texts); // 本文の差替関数
		void UpdateGroups(const std::vector<CommentGroupId> &Groups); // 群の差替関数
		void Remap(const RewriteTrace &Trace); // 錨の移譲の適用関数
	};
} // namespace Core
