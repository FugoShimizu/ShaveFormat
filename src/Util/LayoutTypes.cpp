#include "LayoutTypes.hpp"
#include "../Core/CommentRepository.hpp"
#include "../Core/SyntaxRole.hpp"
#include "FormatLimitExceeded.hpp"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

/** ========== 確定したコードの出力 ========== */
/**
 * 断片の読取専用の並びの取得関数
 * @return 行毎の断片（空行を含む）
 */
std::span<const CodeFragment> LaidOutCode::GetFragments() const {
	// 断片の並びの返戻
	return Fragments;
}

/**
 * 配置枠の表の取得関数
 * @return 此の出力が与える配置枠の表
 */
const CommentSlotTable &LaidOutCode::GetSlots() const {
	// 配置枠の表の返戻
	return Slots;
}

/**
 * 錨と断片の位置表の取得関数
 * @return 配置の段が錨の位置と行毎の深さを引く読取専用の位置表
 */
const AnchorPositionMap &LaidOutCode::GetAnchors() const {
	// 位置表の返戻
	return Anchors;
}

/**
 * 文法がコメントと読む区切記法の範囲の取得関数
 * @return 位置順の範囲（空行の照合で除く）
 */
std::span<const std::pair<uint32_t, uint32_t>> LaidOutCode::GetSeparators() const {
	// 区切記法の範囲の返戻
	return Separators;
}

/**
 * 終端契約の取得関数
 * @return 出力の終端契約（整形仕様 6.7）
 */
const EndingContract &LaidOutCode::GetEnding() const {
	// 終端契約の返戻
	return Ending;
}

/**
 * 識別番号の取得関数
 * @return 配置枠の所属の照合に使う番号
 */
uint64_t LaidOutCode::GetIdentity() const {
	// 識別番号の返戻
	return Identity;
}

/**
 * 字面の位置より後で終わる最初の字句片の取得関数（配置の段と検証が共有する）
 * 計算量：字句片の数 P に対し O(log P)
 * @param Offset 字面の位置
 * @return 位置を含むか位置以後に始まる最初の字句片（無ければ無し）
 */
const CodePiece *LaidOutCode::PieceFrom(const uint32_t Offset) const {
	const std::vector<CodePiece>::const_iterator Found = std::upper_bound(
		Anchors.Pieces.begin(),
		Anchors.Pieces.end(),
		Offset,
		[](const uint32_t Position, const CodePiece &Piece) -> bool {
			// 字句片の終わりとの比較の返戻
			return Position < Piece.End;
		}
	);
	// 位置より後で終わる最初の字句片の返戻
	return Found == Anchors.Pieces.end() ? nullptr : &*Found;
}

/**
 * 字面の位置から始まる字句片の取得関数（配置の段と検証が共有する）
 * @param Offset 字面の位置
 * @return 位置から始まる字句片（無ければ無し）
 */
const CodePiece *LaidOutCode::PieceAt(const uint32_t Offset) const {
	const CodePiece *const Found = PieceFrom(Offset);
	// 位置から始まる字句片の返戻
	return Found && Found->Start == Offset ? Found : nullptr;
}

/**
 * 後のコードの断片の取得関数（配置の段と検証が共有する）
 * @param Fragment 基準の断片
 * @return 空行を飛ばした次の断片（無ければ断片の数）
 */
uint32_t LaidOutCode::NextCodeFragment(uint32_t Fragment) const {
	const size_t Count = Fragments.size();
	for(++Fragment; Fragment < Count && Fragments[Fragment].IsBlank;) ++Fragment;
	// 次のコードの断片の返戻
	return static_cast<uint32_t>(std::min<size_t>(Fragment, Count));
}

/**
 * コンストラクタ（識別番号を振る）
 * 配置枠の所属の照合に使う番号を，出力毎に重ならない様に振る
 */
LaidOutCode::LaidOutCode() {
	// 出力毎の識別番号の発行元
	static std::atomic<uint64_t> NextIdentity = 1;
	Identity = NextIdentity.fetch_add(1, std::memory_order_relaxed);
	// 終了
	return;
}

/** ========== 配置枠の表 ========== */
/**
 * 此の表の配置枠かの判定関数
 * @param Slot 配置枠
 * @return 此の表が与える配置枠なら true
 */
bool CommentSlotTable::Contains(const CommentSlotId &Slot) const {
	// 別の出力の配置枠の返戻
	if(Slot.Owner != Owner) return false;
	if(Slot.Kind == CommentSlotKind::Inline) {
		// 文中の配置枠の返戻
		return std::binary_search(InlineSlots.begin(), InlineSlots.end(), std::pair<uint32_t, uint32_t>(Slot.Offset, Slot.Fragment));
	}
	// 字面の位置を持つ行単位・文書の末尾の配置枠の返戻
	if(Slot.Offset) return false;
	// 文書の末尾の配置枠の返戻
	if(Slot.Kind == CommentSlotKind::DocumentEnd) return Slot.Fragment == LineKinds.size();
	// 行単位の配置枠の返戻
	return Slot.Fragment < LineKinds.size() && LineKinds[Slot.Fragment] & 1 << static_cast<uint8_t>(Slot.Kind);
}

/**
 * 配置枠の取得関数
 * 置ける零幅の位置にだけ配置枠を与え，空行の断片・差込禁止区画に接する行単位の位置・節点の出力の端でない文中の位置には与えない
 * @param Kind 配置枠の種類
 * @param Fragment 断片の番号（文書の末尾では使わない）
 * @param Offset 文中の配置枠の字面の位置（文中でなければ使わない）
 * @return 配置枠（与えない位置は空）
 */
std::optional<CommentSlotId> CommentSlotTable::Find(
	const CommentSlotKind Kind,
	const uint32_t Fragment,
	const uint32_t Offset
) const {
	// 求める配置枠
	const CommentSlotId Slot {
		Owner,
		Kind == CommentSlotKind::DocumentEnd ? static_cast<uint32_t>(LineKinds.size()) : Fragment,
		Kind == CommentSlotKind::Inline ? Offset : 0,
		Kind
	};
	// 此の配置に在る場合だけの求めた配置枠の返戻
	return Contains(Slot) ? std::optional<CommentSlotId>(Slot) : std::nullopt;
}

/** ========== コメント行の列 ========== */
/**
 * コンストラクタ
 * @param Source 条件を確かめた物理行の並び
 */
CommentBlock::CommentBlock(std::vector<std::string> &&Source) : Lines(std::move(Source)) {
	// 終了
	return;
}

/**
 * 条件を満たすコメント行の列の構築関数（整形仕様 3.2）
 * 各物理行はコメントの本文又は記号を含み，空白だけの行を持たず，列の全体が其の言語のコメントだけから成る
 * 行コメントは行末迄，囲みコメントは閉じる迄（入れ子に出来る言語は対を数え）とし，其の外に空白以外を持たない
 * 計算量：行の長さの和 N に対し O(N)
 * @param Rules 言語の記法の定義（コメントの字句の規則）
 * @param Lines 物理行の並び（改行を含まない）
 * @return コメント行の列（条件を満たせない場合は空）
 */
std::optional<CommentBlock> CommentBlock::Make(const Core::NotationRules &Rules, std::vector<std::string> Lines) {
	// 行を持たない列の返戻
	if(Lines.empty()) return std::nullopt;
	// 各物理行の条件の確認
	for(const std::string &Line : Lines) {
		// 改行を含む行と空白だけの行の返戻
		if(Line.find('\n') != std::string::npos || Line.find_first_not_of(" \t") == std::string::npos) return std::nullopt;
	}
	// コメントの連結と使用する記法の取得
	std::string Joined;
	for(const std::string &Line : Lines) Joined.append(Line).push_back('\n');
	const std::string_view BlockOpener = Core::CommentRepository::OpenerOf(CommentNotation::Block);
	const std::string_view BlockCloser = Core::CommentRepository::CloserOf(CommentNotation::Block);
	// 各コメントの始まりから終わり迄の読取
	for(size_t Pos = Joined.find_first_not_of(" \t\n"); Pos != std::string::npos; Pos = Joined.find_first_not_of(" \t\n", Pos)) {
		if(
			const std::string_view Rest = std::string_view(Joined).substr(Pos);
			Rules.IsAvailable(CommentNotation::Block) && Rest.starts_with(BlockOpener)
		) {
			// 閉じる迄（入れ子に出来る言語は開きと閉じの対を数える）
			size_t Depth = 1;
			for(Pos += BlockOpener.size(); Depth && Pos < Joined.size();) {
				if(Rules.IsBlockCommentNested && !Joined.compare(Pos, BlockOpener.size(), BlockOpener)) ++Depth, Pos += BlockOpener.size();
				else if(!Joined.compare(Pos, BlockCloser.size(), BlockCloser)) --Depth, Pos += BlockCloser.size();
				else ++Pos;
			}
			// 閉じない囲みの返戻
			if(Depth) return std::nullopt;
		} else if(
			const std::string_view Opener = Core::CommentRepository::OpenerOf(CommentNotation::RubyEmbeddedDocument);
			Rules.IsAvailable(CommentNotation::RubyEmbeddedDocument) && (!Pos || Joined[Pos - 1] == '\n') &&
			Core::CommentRepository::IsLineHeadWord(Rest, Opener)
		) {
			// 行頭の終端記号（後に空白か行末が続く物）の行の終わり迄
			const std::string_view Closer = Core::CommentRepository::CloserOf(CommentNotation::RubyEmbeddedDocument);
			for(Pos = Joined.find('\n', Pos); Pos != std::string::npos; Pos = Joined.find('\n', Pos + 1)) {
				if(Core::CommentRepository::IsLineHeadWord(std::string_view(Joined).substr(Pos + 1), Closer)) break;
			}
			// 閉じない埋込文書の返戻
			if(Pos == std::string::npos) return std::nullopt;
			Pos = Joined.find('\n', Pos + 1);
		} else if(
			std::ranges::any_of(
				std::initializer_list<CommentNotation>{ CommentNotation::HashBangLine, CommentNotation::HashLine, CommentNotation::SlashLine },
				[&Rules, Rest](const CommentNotation Notation) -> bool {
					// 行末迄を本文とする使用可能な記法で始まるかの返戻
					return Rules.IsAvailable(Notation) && Rest.starts_with(Core::CommentRepository::OpenerOf(Notation));
				}
			)
		) Pos = Joined.find('\n', Pos);
		// 其の言語のコメントで始まらない字面の返戻
		else return std::nullopt;
	}
	// 条件を満たした列の返戻
	return CommentBlock(std::move(Lines));
}

/**
 * 物理行の取得関数
 * @return 物理行の並び
 */
std::span<const std::string> CommentBlock::GetLines() const {
	// 物理行の並びの返戻
	return Lines;
}

/** ========== コメントの配置計画 ========== */
/**
 * コンストラクタ
 * @param Code 配置枠を与える出力
 */
CommentPlacementPlan::CommentPlacementPlan(const LaidOutCode &Code) : Owner(Code.GetIdentity()) {
	// 終了
	return;
}

/**
 * 割当の追加関数
 * @param Slot 配置枠
 * @param Comment コメント行の列
 * @return 此の計画の出力の配置枠なら true（別の出力の配置枠は追加しない）
 */
bool CommentPlacementPlan::Add(const CommentSlotId &Slot, CommentBlock &&Comment) {
	// 別の出力の配置枠の返戻
	if(Slot.Owner != Owner) return false;
	Insertions.push_back({ Slot, std::move(Comment) });
	// 追加した事の返戻
	return true;
}

/**
 * 所属する出力の識別番号の取得関数
 * @return 配置枠を与えた出力の識別番号
 */
uint64_t CommentPlacementPlan::GetOwner() const {
	// 識別番号の返戻
	return Owner;
}

/**
 * 割当の読取専用の並びの取得関数
 * @return 割当の並び（同じ配置枠では並びの順に出力する）
 */
std::span<const CommentInsertion> CommentPlacementPlan::GetInsertions() const {
	// 割当の並びの返戻
	return Insertions;
}

/** ========== 合成結果 ========== */
/**
 * コンストラクタ
 * @param Source 共有する変更不能なコードの出力
 * @param Plan 出力の順に並べたコメントの配置計画
 * @param Length 直列化する字面の長さ
 */
OutputDocument::OutputDocument(
	std::shared_ptr<const LaidOutCode> Source,
	CommentPlacementPlan &&Plan,
	const size_t Length
) : Code(std::move(Source)), Comments(std::move(Plan)), Size(Length) {
	// 終了
	return;
}

/**
 * コードとコメントの配置計画の合成関数（整形仕様 3.2, 9.1）
 * コードの全断片を元の順に１回ずつ出す間へ，配置枠へ差し込むコメント行の列を出力の順に並べ，字面を実体化する前に大きさを見積もる
 * 同じ行では配置枠の種類の順，文中では字面の位置の順，同じ配置枠では計画の順に出す
 * 計算量：断片の数 F，割当の数 C，コメントの行の数 L に対し O(F + C log C + L)
 * @param Code 共有する変更不能なコードの出力
 * @param Plan コメントの配置計画
 * @return 合成結果
 * @throws FormatLimitExceeded 配置の計画が別のコードの物か此の出力の与えない配置枠を指す場合と，出力が大きさの上限を超える場合
 */
OutputDocument OutputDocument::Compose(std::shared_ptr<const LaidOutCode> Code, CommentPlacementPlan Plan) {
	// 別の出力の計画は合成せず整形器の不具合とする
	if(Plan.GetOwner() != Code->GetIdentity()) {
		throw FormatLimitExceeded("comment placement plan belongs to another output", FailureReason::FormatterDefect);
	}
	// コードとコメントの字面の長さの見積
	const std::span<const CodeFragment> Fragments = Code->GetFragments();
	size_t Size = Fragments.empty() ? 0 : Fragments.size() - 1;
	for(const CodeFragment &Fragment : Fragments) Size += Fragment.Text.size();
	for(const CommentInsertion &Insertion : Plan.Insertions) {
		// 此の出力が与えない配置枠は合成せず整形器の不具合とする
		if(!Code->GetSlots().Contains(Insertion.Slot)) {
			throw FormatLimitExceeded("comment placement plan has an invalid slot", FailureReason::FormatterDefect);
		}
		for(const std::string &Line : Insertion.Comment.GetLines()) Size += Line.size() + 1;
	}
	// 深いインデントのコメントで膨らむ出力の再入力と同じ 16 MiB の上限の確認（整形仕様 9.1）
	if(Size > MaxInputBytes) throw FormatLimitExceeded("output exceeds size limit");
	// 配置枠の出力順への整列
	std::stable_sort(
		Plan.Insertions.begin(),
		Plan.Insertions.end(),
		[](const CommentInsertion &Lhs, const CommentInsertion &Rhs) -> bool {
			// 断片・種類・字面の位置の順の比較の返戻
			return std::tie(Lhs.Slot.Fragment, Lhs.Slot.Kind, Lhs.Slot.Offset) <
			std::tie(Rhs.Slot.Fragment, Rhs.Slot.Kind, Rhs.Slot.Offset);
		}
	);
	// 合成結果の返戻
	return OutputDocument(std::move(Code), std::move(Plan), Size);
}

/**
 * コメントを全て除いた中核の合成関数
 * 計算量：コードの断片の数 F に対して O(F)
 * @param Code 共有する変更不能なコードの出力
 * @return 子の配置計画も差し込まない合成結果
 * @throws FormatLimitExceeded 出力が大きさの上限を超える場合
 */
OutputDocument OutputDocument::CodeOnly(std::shared_ptr<const LaidOutCode> Code) {
	OutputDocument Document = Compose(Code, CommentPlacementPlan(*Code));
	Document.IncludeOwnedComments = false;
	// 中核の合成結果の返戻
	return Document;
}

/**
 * 共有するコードの出力の取得関数
 * @return 変更不能なコードの出力
 */
const LaidOutCode &OutputDocument::GetCode() const {
	// コードの出力の返戻
	return *Code;
}

/**
 * 出力の順に並べたコメントの配置計画の取得関数
 * @return 配置枠とコメント行の列の組の並び（直列化する順）
 */
const CommentPlacementPlan &OutputDocument::GetComments() const {
	// 配置計画の返戻
	return Comments;
}

/**
 * 直列化する字面の長さの取得関数
 * @return 合成で見積もった長さ（終端契約の改行を除く）
 */
size_t OutputDocument::GetSize() const {
	// 長さの返戻
	return Size;
}

/**
 * 子の配置計画を合成するかの取得関数
 * @return 最終出力なら true，中核の観測なら false
 */
bool OutputDocument::IncludesOwnedComments() const {
	// 子の配置計画の合成の可否の返戻
	return IncludeOwnedComments;
}
