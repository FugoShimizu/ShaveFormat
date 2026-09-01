#pragma once

#include "Lang.hpp"
#include "TextEdit.hpp"
#include "TsSource.hpp"
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// 宣言編集で共有する変数解析とバイト位置基準コメント再付与のクラス
class DeclEdit {
private:

	static std::vector<CommentAttach> TakeOwnLineLeads(std::vector<CommentAttach> &Leads); // 独立行先行コメントの取出関数
	static bool IsEditBefore(const TextEdit &Lhs, const TextEdit &Rhs); // 編集の整列順序の判定関数（ApplyEditsNoTreeEdit / ApplyByteRebind 共通の比較子）
	static void ApplyEditsNoTreeEdit(TSSource &Src, std::vector<TextEdit> &Edits); // 木構造更新を伴わない編集列適用関数（ApplyByteRebind 専用の内部補助）

public:

	// 宣言ノードの解析結果（型接頭部と宣言子の範囲）
	struct DeclSummary {
		bool IsValid = false; // 解析成功か
		uint32_t Start = 0; // 宣言ノードの開始バイト
		uint32_t End = 0; // 宣言ノードの終端バイト
		uint32_t TypePrefixEnd = 0; // 最終の型子ノードの終端バイト
		uint32_t FirstDeclaratorStart = 0; // 最初の宣言子の開始バイト
		uint32_t LastDeclaratorEnd = 0; // 最終の宣言子の終端バイト
		uint32_t SemicolonStart = 0; // 末尾セミコロンの開始バイト
	};

	// 統合や分割で消費した先行・後続コメントを，編集適用後の対象バイト位置へ再付与する為の遅延情報
	struct PostEditAttach {
		uint32_t OldStartByte; // 編集の開始位置と同値
		uint32_t OffsetInReplacement; // 置換テキスト内の開始オフセット
		const char *NodeType; // ts_node_type の静的文字列か文字列リテラルを保持（同一パーサインスタンス内では再構文解析後も同一ポインタ）
		std::vector<CommentAttach> Leading; // 編集後の節点へ移す先行コメント
		std::vector<CommentAttach> Trailing; // 編集後の節点へ移す後続コメント
		uint32_t EndOffsetInReplacement = 0; // 置換テキスト内のノード末尾オフセット（未指定時は 0）
	};

	// 宣言の分割の候補（場所と条件は呼出側が決める）
	struct SplitCandidate {
		TSNode Node; // 分割する宣言
		TSNode Wrapper; // 宣言と同じ範囲を包む親（C# のファイル直下の global_statement 等，無ければ空）
		DeclSummary Decl; // 宣言の解析結果
		std::vector<TSNode> Declarators; // 宣言子
		const char *TypeRaw; // 宣言の型名（コメントの錨の型）
	};

	static TSNode UnwrapCSharpDecl(const TSNode Decl); // C# 宣言ラッパー除去関数（本体ノードの取出）
	static DeclSummary AnalyzeDecl(const TSSource &Src, const TSNode Decl, const Lang Language); // 宣言ノードの解析関数（型接頭部と宣言子範囲の抽出）
	static void CollectDeclarators(const TSNode Node, const uint32_t TypePrefixEnd, const Lang Language, std::vector<TSNode> &Out); // 宣言の宣言子の収集関数

	static void CollectSubAttachments(
		TSSource &Src,
		const TSNode WalkRoot,
		const std::span<const std::pair<uint32_t, uint32_t>> Ranges,
		const uint32_t OldStartByte,
		const std::span<const uint32_t> CombinedStarts,
		std::vector<PostEditAttach> &Out
	); // 指定範囲内付随コメントの収集関数

	static void TakeInnerDeclAttachments(
		TSSource &Src,
		const TSNode Decl,
		const Lang Language,
		std::vector<CommentAttach> &Leading,
		std::vector<CommentAttach> &Trailing
	); // 宣言内部付随コメントの取出関数

	static void ApplyByteRebind(TSSource &Src, std::vector<TextEdit> &Edits, std::vector<PostEditAttach> &PostAttaches); // 編集適用後バイト位置基準の先行・後続コメント再付与関数

	static bool SplitDeclarations(
		TSSource &Src,
		const Lang Language,
		const std::function<void(std::vector<SplitCandidate> &)> &Collect,
		const std::function<size_t(const std::vector<size_t> &, const size_t, const size_t)> &GroupEnd,
		const bool KeepsIndent,
		const std::function<void(TSSource &)> &AfterApply
	); // 宣言の分割の適用関数

	DeclEdit() = delete; // コンストラクタ（禁止）
};
