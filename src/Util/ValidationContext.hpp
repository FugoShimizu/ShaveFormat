#pragma once

#include "../Core/NodeId.hpp"
#include "../Core/OutputPosition.hpp"
#include <tree_sitter/api.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class GrammarProfile;
class LaidOutCode;
class SyntaxValidator;

namespace Core {
	class CommentRepository;
	class SourceProjector;

	struct OutputExpectation;
} // namespace Core

// 投影が解析木から組み立て，検証が照合するデータ（整形仕様 3.2, 3.3）
struct ValidationContext {
private:

	friend class Core::SourceProjector;
	friend class SyntaxValidator;

	// 子の意味・保護領域の独立した照合期待（親の構文モデルへ公開しない）
	struct ChildExpectation {
		size_t Ordinal = 0; // 親ドキュメントの埋込の順
		const GrammarProfile *Profile = nullptr; // 子の入力で確定した文法
		std::shared_ptr<const LaidOutCode> Code; // 子の空行・断片の照合基準
		std::shared_ptr<Core::OutputExpectation> Syntax; // 子の正規化後の期待構文と選択表現
		std::shared_ptr<const Core::CommentRepository> Comments; // 子所有の期待コメント（親の通常コメントへ混ぜない）
		std::shared_ptr<ValidationContext> Ctx; // 子の入力の意味・保護領域
	};

	std::shared_ptr<const std::string> InputOwner; // 保護領域の入力ビューを生かす入力の所有者
	std::vector<ChildExpectation> OwnedChildren; // 子の独立検証の経路

public:

	// 所有する宣言の表示の共有接頭部（０番は空の根）
	struct DocScope {
		size_t Parent = 0; // 前の表示の番号（自身より前の要素）
		std::string Label; // 此の宣言だけの種類と名前
	};

	// １度の独立投影のドキュメントのテキストと行の範囲（ビューの公開後は変更しない）
	struct DocStorage {
		std::string Targets; // 記録順の説明対象の識別（連続する同じ対象は範囲を共有する）
		std::vector<DocScope> Scopes; // 祖先の表示を１度ずつ保持する接頭部の表
		std::vector<size_t> TargetScopes; // 説明対象毎の接頭部の番号
		std::string Bodies; // 記録順に繋いだ本文
		std::vector<std::pair<size_t, size_t>> Lines; // 各記録の本文内の論理行の開始と長さ
	};

	// ドキュメントの説明対象と本文（整形仕様 7.4, 8.1）
	struct DocFact {
		std::shared_ptr<const DocStorage> Owner; // 此の投影のテキストと範囲を生かす共有の所有者
		std::string_view Target; // 説明対象の識別
		std::string_view Text; // 此の記録の本文
		std::span<const std::pair<size_t, size_t>> Lines; // 本文内の論理行の開始と長さ（原文順）
		bool IsDedicated = false; // 通常コメントと区別する記法のドキュメントか
		size_t Scope = 0; // 此の投影だけの所有する宣言の表示の番号
		bool IsInner = false; // 囲む本体のドキュメントか
	};

	// 逐語領域のテキストに残したコメントの範囲（整形仕様 8.1 の空行の照合基準で除く範囲で，本文を持たない）
	struct KeptComment {
		Core::NodeId Node = Core::NoNode; // 逐語領域のノード
		uint32_t Start = 0; // テキストの中の開始
		uint32_t End = 0; // テキストの中の終了
	};

	// 解析木で確かめる意味上の区別（整形仕様 3.2, 8.1，投影が組み替える前のパーサーの結付を含む）
	struct SemanticCheck {
		TSSymbol Node; // 区別する構文の記号
		uint32_t Bound; // 構文の結付を表す量（演算子は右の被演算子の意味上の範囲の名前のトークンの数，制御構文は本体が空か，其の他は０）

		bool operator==(const SemanticCheck &Other) const = default; // 等価比較演算子
	};

	// 行末の記号の前置区切（整形仕様 8.1）
	struct LeadingSeparator {
		std::string Line; // 記号の後の最初の論理行（期待コメントの記録との照合用）
		size_t Tabs = 0; // 記号の前置区切のタブの数（不正な区切は最大値）
	};

	// 構文エラーの周りのテキスト
	struct ErrorZone {
		std::string Text; // 空白を除きコメントをマーカーとしたテキスト
		uint32_t Count = 0; // 入力の全体に現れる数（整形後に減れば何処かの其のテキストを失った）
		uint32_t OwnerPrefix = 0; // 最初の所有ノードの中でエラー本文より前に在るコードのバイト数（空白・コメント・解析用の区切を除く）
		uint32_t OwnerSuffix = 0; // 最後の所有ノードの中でエラー本文より後に在るコードのバイト数（同上）
	};

	// 保護領域（整形仕様 4.4, 4.5, 8.1，整形後も本文・位置・有効性を保つかを確かめる領域）
	struct ProtectedRegionSet {
		std::vector<std::string> Directives; // 言語標準のディレクティブコメント（整形仕様 4.5，本文・有効性・位置条件）
		std::vector<uint32_t> DirectiveStarts; // ディレクティブコメントの修復後の開始位置
		std::vector<DocFact> Docs; // 完成候補の原文順のドキュメントの説明対象と本文（整形仕様 7.4, 8.1，期待側は検証が期待コメントから求める）
		std::vector<KeptComment> KeptComments; // 逐語領域のテキストに残したコメントの範囲（投影が記録し，検証が出力の位置へコピーする）
		std::vector<std::string> AnalysisMarks; // 入力側が解析の為に置いたマーカー（最終テキストに残らない事を確かめる，整形仕様 4.2）
		std::string_view InputText; // 入力のテキスト（整形仕様 8.1 のトークンと値の保持をテキストで確かめる検査に使う）
	};

	// 意味上の区別の並び（整形仕様 3.2, 8.1）
	struct SemanticCheckList {
		std::vector<SemanticCheck> Checks; // 解析木で確かめた意味上の区別
		std::vector<TSSymbol> Symbols; // 投影元の記号（ノードの番号順，生成ノードは記号表の外）
	};

	// 入力の診断（整形仕様 8.1, 8.3）
	struct DiagnosticList {
		uint32_t InputBrokenCount = 0; // 入力時点で壊れて居たノードの数（整形後の増加の判定に使う）
		uint32_t BrokenCount = 0; // 修復後の壊れたノードの数
		std::vector<std::pair<uint32_t, uint32_t>> CommentRanges; // 修復後のコメントノードの範囲
		std::vector<std::pair<uint32_t, uint32_t>> CommentRecords; // 独立投影の記録毎の修復前の範囲（記録と同じ原文順）
		std::vector<Core::OutputRange> Positions; // 修復前の本文へコピーしたノード毎の診断位置（静的解析が読む検証の構文だけが持つ）
		std::vector<LeadingSeparator> LeadingSeparators; // 行末の各記号の論理行と前置区切（原文順，検証の構文だけが持つ，整形仕様 8.1）
	};

	const GrammarProfile *EntryProfile = nullptr; // 元の入口で候補に為る文法のリソース受理条件
	bool IsEmbedded = false; // 本文の先頭をファイルの BOM と区別する独立した埋込の経路
	std::vector<ErrorZone> ErrorZones; // 入力の構文エラーの周りのテキスト（部分整形が保つかの判定に使う）
	ProtectedRegionSet ProtectedRegions; // 保護領域
	SemanticCheckList SemanticChecks; // 意味上の区別
	DiagnosticList InputDiagnostics; // 入力の診断
};
