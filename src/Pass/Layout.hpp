#pragma once

#include "../Util/Lang.hpp"
#include "../Util/TsSource.hpp"
#include <algorithm>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// レイアウトパスクラス
class LayoutPass {
private:

	// Dest 順に消費するコメント挿入候補（Text の参照中は添付を消去しない）
	struct CommentInsertion {
		uint32_t Dest; // 挿入先のバイトオフセット
		uint32_t Seq; // 同一挿入先内での順序
		std::string_view Text; // 添付コメント本体への参照ビュー
		bool IsLeading; // 行頭コメントか
		bool IsInline; // 行内へ挿入するコメントか
		bool IsOwnLine = false; // 錨の行の後の独立した行へ置く末尾コメントか
		size_t IndentLine = 0; // 独立した行へ置くコメントのインデントを借りる行（錨の始まりの行）の番号
		bool IsGluedAfter = false; // 後のコードとの間に空白を置かない行内コメントか（CSS のコメントの後の符号付の数）
		bool IsGluedBefore = false; // 前のコードとの間にも空白を置かない行内コメントか（前後共に密着した CSS のコメント）
		std::string_view Joined {}; // 行末へ置く時の本文（句点で行を分けない形，本文と同じなら空）
		bool IsVerbatim = false; // 処理系が読むコメントか（継続行の `*` を揃えない）
		uint32_t Origin = 0; // PHP の地の文で始まる行で錨の区画の閉じタグを選ぶ錨の位置（先行・行内のコメントは始まり，末尾コメントは終わり，０は挿入先と同じ）
	};

	static void BuildLineInfo(const TSSource &Src, std::vector<uint32_t> &LineStarts, std::vector<uint32_t> &ContentStarts); // 行情報の構築関数
	static size_t LineIndexAt(const std::vector<uint32_t> &LineStarts, const uint32_t Byte); // バイト位置の行番号の取得関数

	static void CollectLayoutNodeInfo(
		const TSSource &Src,
		const std::vector<uint32_t> &LineStarts,
		std::unordered_set<uint32_t> &InCommentLines,
		std::unordered_set<uint32_t> &InHeredocContentLines,
		std::unordered_set<uint32_t> *const SectionStartBytes,
		std::unordered_set<uint32_t> *const LeafOpenerLines
	); // レイアウト用ノード情報の収集関数

	static void SetLineIndent(std::unordered_map<uint32_t, int> &Indents, const uint32_t LineStart, const int Depth); // 行インデント深さの設定関数

	static void CollectTokenIndents(
		const TSSource &Src,
		const TSNode Node,
		const std::vector<uint32_t> &LineStarts,
		const std::vector<uint32_t> &LineContentStarts,
		const int Depth,
		std::unordered_map<uint32_t, int> &Indents
	); // トークン単位のインデント収集関数

	static uint32_t LineStartAt(const std::vector<uint32_t> &LineStarts, const uint32_t Byte); // バイト位置の行頭オフセットの取得関数

	static void CollectLineIndents(
		const TSSource &Src,
		const TSNode Node,
		const Lang Language,
		const std::vector<uint32_t> &LineStarts,
		const std::vector<uint32_t> &LineContentStarts,
		const int Depth,
		const int OuterContIndent,
		std::unordered_map<uint32_t, int> &Indents
	); // 行インデントの収集関数

	static void ComputeBracketIndent(
		const TSSource &Src,
		const Lang Language,
		const std::vector<uint32_t> &LineStarts,
		const std::vector<uint32_t> &LineContentStarts,
		const std::unordered_set<uint32_t> &InCommentLines,
		const std::unordered_set<uint32_t> &SectionStartBytes,
		std::vector<int> &LineIndents
	); // 括弧に依るインデントの算出関数

	static bool OpensLineComment(const std::string_view Text); // 行末迄を本文とするコメントかの判定関数
	static bool IsCloserLine(const std::string_view Source, const size_t ContentStart); // 行内の行コメントを行末へ置く行かの判定関数
	static bool InsertsBefore(const CommentInsertion &Lhs, const CommentInsertion &Rhs); // コメントの挿入候補の並び順の比較関数

	static std::vector<CommentInsertion> CollectCommentInsertions(
		const TSSource &Src,
		const Lang Language,
		const std::vector<uint32_t> &LineStarts
	); // コメント挿入候補の収集関数

	static void DropCommentsInInlineHtml(
		const TSSource &Src,
		const std::vector<uint32_t> &LineStarts,
		std::vector<CommentInsertion> &Insertions
	); // PHP の地の文へ差し込まれるコメントの除去関数

	static size_t CeilTabs(const size_t Chars, const uint32_t IndentChars); // 文字数のタブ数への切上関数

public:

	static bool DropsComments(); // コメントを書き戻さない検査用の設定の判定関数
	static void FinalizeLayout(TSSource &Src, const Lang Language); // レイアウト確定の適用関数

	LayoutPass() = delete; // コンストラクタ（禁止）
};

/**
 * ヒアドキュメント本文行の走査関数
 * 計算量：行頭数 L，子数 C，型名比較 B，本文行数 H，Visit の総費用 F に対し O(log L + C + B + H + F)
 * @param HeredocBody heredoc_body ノード
 * @param LineStarts 行頭位置を昇順で保持するベクタ
 * @param SrcSize ソースコード全体のサイズ
 * @param Visit 行頭位置（と終端行フラグ）を受け取る関数オブジェクト
 */
template<class F> static void ForEachHeredocBodyLine(
	const TSNode HeredocBody,
	const std::vector<uint32_t> &LineStarts,
	const uint32_t SrcSize,
	F &&Visit
) {
	// 本文範囲と終端識別子の既定位置
	const uint32_t BodyStart = ts_node_start_byte(HeredocBody), BodyEnd = ts_node_end_byte(HeredocBody);
	uint32_t TermStart = BodyEnd;
	// 終端行通知時の終端識別子位置の解決
	if constexpr(std::is_invocable_v<F, uint32_t, bool>) {
		if(const TSNode Term = FirstNamedChildOfType(HeredocBody, "heredoc_end"); !ts_node_is_null(Term)) {
			TermStart = ts_node_start_byte(Term);
		}
	}
	// 昇順の行頭位置列を使う本文各行の走査
	for(
		std::vector<uint32_t>::const_iterator Iter = std::upper_bound(LineStarts.begin(), LineStarts.end(), BodyStart);
		Iter != LineStarts.end() && *Iter < BodyEnd;
		++Iter
	) {
		// 行頭位置と終端行該当状態の通知
		const uint32_t LineStart = *Iter;
		if constexpr(std::is_invocable_v<F, uint32_t, bool>) {
			Visit(LineStart, TermStart >= LineStart && TermStart < (Iter + 1 != LineStarts.end() ? *(Iter + 1) : SrcSize));
		} else Visit(LineStart); // 行位置だけを受取る呼出形への通知
	}
	// 終了
	return;
}
