#pragma once

#include "../Core/NodeId.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

enum class CommentSlotKind : uint8_t { BeforeLine, Inline, LineEnd, OwnLine, DocumentEnd }; // コメントを置ける零幅の位置の種類（宣言の順は同じ行での出力順）

// 配置枠の識別子（`LaidOutCode` が与え，所属する出力を識別する，整形仕様 3.2）
struct CommentSlotId {
	uint64_t Owner = 0; // 配置枠を与えた `LaidOutCode` の識別番号
	uint32_t Fragment = 0; // 配置枠の在るコード行の断片の番号（文書の末尾は断片の数）
	uint32_t Offset = 0; // 文中の配置枠の字面の位置（其の他の種類は０）
	CommentSlotKind Kind = CommentSlotKind::DocumentEnd; // 配置枠の種類
};

// 確定したコードの出力の１行（空行も前後の構文境界に結び付いた断片として固定する）
struct CodeFragment {
	uint32_t Start = 0; // 字面の中の行頭
	uint32_t End = 0; // 字面の中の行末（改行を含まない）
	bool IsBlank = false; // 構造から決めた空行か
	std::string Text; // 此の行のコードの字面（改行を含まない）
};

// 節点の出力上の範囲（字面の位置は断片の頭からでなく字面全体の中の位置）
struct NodeSpan {
	uint32_t StartFragment = 0; // 始まりの断片
	uint32_t Start = 0; // 始まりの字面の位置
	uint32_t EndFragment = 0; // 終わりの断片
	uint32_t End = 0; // 終わりの字面の位置
	bool IsPlaced = false; // 出力に現れたか
};

// 出力した字句片（`CodeLayout` が行毎の字句片を位置順に記録し，`CommentPlacer` が挿入位置の前後の繋がりの判断に読む，整形仕様 7.6）
struct CodePiece {
	enum Shape : uint16_t {
		StartsClosingBracket = 0X1, // 閉じ括弧で始まる
		StartsClosingBrace = 0X2, // 閉じ波括弧で始まる
		StartsCloseParen = 0X4, // 閉じ丸括弧で始まる
		StartsOperator = 0X8, // 前の式へ続く演算子・参照の記号で始まる
		StartsSeparator = 0X10, // 並び又は文の区切りで始まる
		StartsEmptyParens = 0X20, // 空の丸括弧で始まる
		IsOpenParen = 0X40, // 開き丸括弧だけから成る
		EndsOpeningBracket = 0X80, // 開き括弧で終わる
		EndsOpeningBrace = 0X100, // 塊を開く波括弧で終わる
		EndsContinuation = 0X200, // 後の式へ続き文を終えない記号で終わる（後置の増減を除く）
		Leads = 0X7F, // 先頭の字句の形の集合
		Tails = 0X380 // 末尾の字句の形の集合
	}; // 字句片の端の字句の形（ビットの集合）

	uint32_t Start = 0;
	uint32_t End = 0; // 字面の中の始まりと終わり（区切の空白を除く）
	uint16_t Shapes = 0; // 端の字句の形の集合
};

namespace Core {
	class CodeLayout;
	class CommentPlacer;
	class SyntaxModel;

	struct NotationRules;
} // namespace Core

// 親のコード断片へ置いた所有区画の範囲（子のコメントを含まない）
struct OwnedRegionSpan {
	Core::NodeId Node = Core::NoNode; // 所有する節点
	uint32_t FirstFragment = 0; // 最初の断片
	uint32_t EndFragment = 0; // 最後の断片の次
};

// 錨と出力の断片の位置表（`CommentPlacer` が錨の位置と所属から配置枠と行毎の深さを引く為に読む）
struct AnchorPositionMap {
	enum VerbatimBreak : uint8_t { EndsInVerbatim = 0X1, NextInVerbatim = 0X2 }; // 断片の行末と行の後が逐語の中 // 次の断片の前が逐語の中 // 断片の終わりの改行と逐語の区画の関係（ビットの集合，逐語が改行で始まる時は行末だけが逐語の外）
	std::shared_ptr<const Core::SyntaxModel> Syntax; // 錨の所属を引く読取専用の正規化後の構文
	std::vector<NodeSpan> Nodes; // 節点の識別子で引く出力上の範囲（錨の位置）
	std::vector<uint32_t> FragmentTabs; // 断片毎のインデントのタブの段数（独立コメントは後続の行の段数に置く）
	std::vector<uint8_t> VerbatimBreaks; // 断片毎の終わりの改行と逐語の区画の関係（差込禁止区画で，行末・行の後と次の断片の前へコメントを置けない）
	std::vector<CodePiece> Pieces; // 出力した字句片（位置順，区切の空白とインデントを除き，行を跨がない）
	std::vector<OwnedRegionSpan> OwnedRegions; // 親の断片内に在る所有区画の範囲
};

// 出力が与える配置枠の表（整形仕様 3.3 の GetSlots，置ける零幅の位置だけを与え，其れ以外を拒む）
class CommentSlotTable {
	friend class Core::CodeLayout;

private:

	uint64_t Owner = 0; // 配置枠を与える出力の識別番号
	std::vector<uint8_t> LineKinds; // 断片毎の行単位の配置枠の種類の集合（種類毎のビット，空行と差込禁止区画に接する物は持たない）
	std::vector<std::pair<uint32_t, uint32_t>> InlineSlots; // 文中の配置枠の字面の位置と断片（節点の出力の端，位置順）

public:

	bool Contains(const CommentSlotId &Slot) const; // 此の表の配置枠かの判定関数
	std::optional<CommentSlotId> Find(const CommentSlotKind Kind, const uint32_t Fragment, const uint32_t Offset) const; // 配置枠の取得関数
};

// 出力の終端契約（整形仕様 6.7）
struct EndingContract {
	bool EndsWithNewline = false; // 空でない出力を改行で終えるか
};

// 変更不能なコードの出力（`CodeLayout` だけが構築し，空行を断片として固定する，整形仕様 3.2）
class LaidOutCode {
	friend class Core::CodeLayout;

private:

	uint64_t Identity = 0; // 識別番号（配置枠の所属の照合に使う）
	std::vector<CodeFragment> Fragments; // 行毎の断片
	AnchorPositionMap Anchors; // 錨と断片の位置表
	std::vector<std::pair<uint32_t, uint32_t>> Separators; // 文法がコメントと読む区切記法の字面の範囲（位置順，空行の照合で除く）
	CommentSlotTable Slots; // 配置枠の表
	EndingContract Ending; // 終端契約

	LaidOutCode(); // コンストラクタ（識別番号を振る，`CodeLayout` だけが使う）

public:

	std::span<const CodeFragment> GetFragments() const; // 断片の読取専用の並びの取得関数
	const CommentSlotTable &GetSlots() const; // 配置枠の表の取得関数
	const AnchorPositionMap &GetAnchors() const; // 錨と断片の位置表の取得関数
	std::span<const std::pair<uint32_t, uint32_t>> GetSeparators() const; // 文法がコメントと読む区切記法の範囲の取得関数
	const EndingContract &GetEnding() const; // 終端契約の取得関数
	uint64_t GetIdentity() const; // 識別番号の取得関数
	const CodePiece *PieceFrom(const uint32_t Offset) const; // 字面の位置より後で終わる最初の字句片の取得関数
	const CodePiece *PieceAt(const uint32_t Offset) const; // 字面の位置から始まる字句片の取得関数
	uint32_t NextCodeFragment(uint32_t Fragment) const; // 後のコードの断片の取得関数
};

// 確定した記法に従うコメント行の列（空行を要素として持たない，整形仕様 3.2）
class CommentBlock {
private:

	std::vector<std::string> Lines; // 物理行の並び

	explicit CommentBlock(std::vector<std::string> &&Source); // コンストラクタ（`Make` だけが使う）

public:

	static std::optional<CommentBlock> Make(const Core::NotationRules &Rules, std::vector<std::string> Lines); // 条件を満たすコメント行の列の構築関数
	std::span<const std::string> GetLines() const; // 物理行の取得関数
};

// 配置枠へのコメントの割当（整形仕様 3.2）
struct CommentInsertion {
	CommentSlotId Slot; // 配置枠
	CommentBlock Comment; // コメント行の列
};

// コメントの配置計画（配置枠とコメント行の列の組だけを持ち，空行・コードの変更を表せない，整形仕様 3.2）
class CommentPlacementPlan {
	friend class OutputDocument;

private:

	uint64_t Owner = 0; // 配置枠を与えた `LaidOutCode` の識別番号
	std::vector<CommentInsertion> Insertions; // 割当の並び

public:

	uint64_t GetOwner() const; // 所属する出力の識別番号の取得関数
	std::span<const CommentInsertion> GetInsertions() const; // 割当の読取専用の並びの取得関数
	bool Add(const CommentSlotId &Slot, CommentBlock &&Comment); // 割当の追加関数

	explicit CommentPlacementPlan(const LaidOutCode &Code); // コンストラクタ
};

// コードとコメントの変更不能な合成結果（整形仕様 3.2）
class OutputDocument {
private:

	std::shared_ptr<const LaidOutCode> Code; // 共有する変更不能なコードの出力
	CommentPlacementPlan Comments; // 出力の順に並べたコメントの配置計画
	size_t Size = 0; // 直列化する字面の長さ（終端契約の改行を除く）
	bool IncludeOwnedComments = true; // 子の配置計画も合成するか（中核の観測では false）

	OutputDocument(std::shared_ptr<const LaidOutCode> Source, CommentPlacementPlan &&Plan, const size_t Length); // コンストラクタ（`Compose` だけが使う）

public:

	static OutputDocument Compose(std::shared_ptr<const LaidOutCode> Code, CommentPlacementPlan Plan); // コードと配置計画の合成関数
	static OutputDocument CodeOnly(std::shared_ptr<const LaidOutCode> Code); // コメントを全て除いた中核の合成関数
	const LaidOutCode &GetCode() const; // 共有するコードの出力の取得関数
	const CommentPlacementPlan &GetComments() const; // 出力の順に並べたコメントの配置計画の取得関数
	size_t GetSize() const; // 直列化する字面の長さの取得関数
	bool IncludesOwnedComments() const; // 子の配置計画を合成するかの取得関数
};
