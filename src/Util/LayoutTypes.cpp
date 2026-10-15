#include "LayoutTypes.hpp"
#include "FormatLimitExceeded.hpp"
#include "ParseAdapter.hpp"
#include "../Core/CommentRepository.hpp"
#include "../Core/SyntaxModel.hpp"
#include "../Core/SyntaxRole.hpp"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

/**
 * 配置で本文の内部を保護するリテラルのアンカーかの判定関数
 * コード配置とコメント配置で同じ役割を照合し，照会の短絡順も保つ
 * 計算量：固定数の役割に対し O(1)
 * @param Syntax 読取専用の期待構文
 * @param Node アンカーのノード
 * @param Work 検証時の役割照会数の格納先（製品の配置では nullptr）
 * @return 本文の内部を保護するリテラルなら true
 * @throws FormatLimitExceeded 役割の照会が作業予算を超える場合
 * @throws std::out_of_range ノードの識別子が構文表に無い場合
 */
bool IsLiteralAnchor(const Core::SyntaxModel &Syntax, const Core::NodeId Node, Core::StructureWorkAccount *const Work) {
	// 共通の役割の照会と，検証時だけの計上
	const auto Is = [&](const Core::Role Wanted) -> bool {
		// 役割の照会と返戻
		if(Work) Work->Charge(1);
		return Syntax.Is(Node, Wanted);
	};
	// 元の短絡順を保ったリテラルの判定の返戻
	return Is(Core::Role::LiteralInterior) || Is(Core::Role::StrLikeAll) || Is(Core::Role::DelimitedLiteral);
}

/**
 * 配置枠の取得関数
 * @return 計画の構築時に確認した枠
 */
const CommentSlotId &CommentInsertion::GetSlot() const {
	// 配置枠の返戻
	return Slot;
}

/**
 * コメント行の取得関数
 * @return 検査済のコメント行
 */
const CommentBlock &CommentInsertion::GetComment() const {
	// コメントの返戻
	return Comment;
}

/**
 * 計画だけの構築口
 * @param Slot 確認済の配置枠
 * @param Comment 合法なコメント行
 */
CommentInsertion::CommentInsertion(const CommentSlotId &Slot, CommentBlock &&Comment) : Slot(Slot),
Comment(std::move(Comment)) {
	// 終了
	return;
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

/**
 * 識別番号の取得関数
 * @return 配置枠の所属の照合に使う番号
 */
uint64_t LaidOutCode::GetIdentity() const {
	// 識別番号の返戻
	return Identity;
}

/**
 * ========== コメントの配置計画 ==========
 * コンストラクタ
 * @param Code 配置枠を与える出力
 */
CommentPlacementPlan::CommentPlacementPlan(const LaidOutCode &Code) : Owner(Code.GetIdentity()) {
	// 終了
	return;
}

/**
 * ========== 合成結果 ==========
 * コンストラクタ
 * @param Src 共有する変更不能なコードの出力
 * @param Plan 出力の順に並べたコメントの配置計画
 * @param Len シリアライズするテキストの長さ
 */
OutputDocument::OutputDocument(std::shared_ptr<const LaidOutCode> Src, CommentPlacementPlan &&Plan, const size_t Len) : Code(
	std::move(Src)
), Comments(std::move(Plan)), Size(Len) {
	// 終了
	return;
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
 * 配置枠の表の取得関数
 * @return 此の出力が与える配置枠の表
 */
const CommentSlotTable &LaidOutCode::GetSlots() const {
	// 配置枠の表の返戻
	return Slots;
}

/**
 * ========== 配置枠の表 ==========
 * 此の表の配置枠かの判定関数
 * 計算量：文中の配置枠の数 S に対し O(log(S + 1))
 * @param Slot 配置枠
 * @return 此の表が与える配置枠なら true
 */
bool CommentSlotTable::IsOwnedSlot(const CommentSlotId &Slot) const {
	// 別の出力の配置枠の返戻
	if(Slot.Owner != Owner) return false;
	if(Slot.Kind == CommentSlotKind::Inline) {
		// 文中の配置枠の返戻
		return std::binary_search(InlineSlots.begin(), InlineSlots.end(), std::pair<uint32_t, uint32_t>(Slot.Offset, Slot.Fragment));
	}
	// テキストの位置を持つ行単位・ドキュメントの末尾の配置枠の返戻
	if(Slot.Offset) return false;
	// ドキュメントの末尾の配置枠の返戻
	if(Slot.Kind == CommentSlotKind::DocEnd) return HasDocEnd && Slot.Fragment == LineKinds.size();
	// 行単位の配置枠の返戻
	return Slot.Fragment < LineKinds.size() && LineKinds[Slot.Fragment] & 1 << static_cast<uint8_t>(Slot.Kind);
}

/**
 * 物理行の取得関数
 * @return 物理行の並び
 */
std::span<const std::string> CommentBlock::GetLines() const {
	// 物理行の並びの返戻
	return Lines;
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
 * テキストの位置より後で終わる最初のトークン片の取得関数（配置の段と検証が共有する）
 * 計算量：トークン片の数 P に対し O(log P)
 * @param Offset テキストの位置
 * @return 位置を含むか位置以後に始まる最初のトークン片（無ければ無い）
 */
const CodePiece *LaidOutCode::GetPieceFrom(const uint32_t Offset) const {
	// テキストの位置より後で終わる最初のトークン片の取得
	const std::vector<CodePiece>::const_iterator Found = std::upper_bound(
		Anchors.Pieces.begin(),
		Anchors.Pieces.end(),
		Offset,
		// 位置より後で終わるトークン片を探す比較
		[](const uint32_t Pos, const CodePiece &Piece) -> bool {
			// トークン片の終わりとの比較の返戻
			return Pos < Piece.End;
		}
	);
	// 位置より後で終わる最初のトークン片の返戻
	return Found == Anchors.Pieces.end() ? nullptr : &*Found;
}

/**
 * ========== コメント行の列 ==========
 * コンストラクタ
 * @param Src 条件を確かめた物理行の並び
 * @param Dst 記法の成立を確認した配置枠
 * @param NeedsLineEnd 行末を必要とする記法を持つか
 * @param HasLineHeadNotation 行頭で開く記法を持つか
 */
CommentBlock::CommentBlock(
	std::vector<std::string> &&Src,
	const CommentSlotId &Dst,
	const bool NeedsLineEnd,
	const bool HasLineHeadNotation
) : Lines(std::move(Src)), Slot(Dst), NeedsLineEnd(NeedsLineEnd), HasLineHeadNotation(HasLineHeadNotation) {
	// 終了
	return;
}

/**
 * ========== 確定したコードの出力 ==========
 * インデントを含む行内の範囲の追記関数
 * 共有する本文を繋ぎ直さず，接頭部と本文の必要な範囲を出力先へ直接書く
 * 計算量：追記するバイト数 B に対し O(B)
 * @param Offset 行頭からの開始位置
 * @param Size 範囲のバイト数
 * @param Dst 追記先
 * @throws std::bad_alloc 出力先のメモリを確保出来ない場合
 * @throws std::length_error 出力先の長さが上限を超える場合
 * @throws std::out_of_range 範囲の開始位置が行末を超える場合
 */
void CodeFragment::AppendTo(uint32_t Offset, uint32_t Size, std::string &Dst) const {
	// 接頭部に重なる範囲の追記
	if(Offset < PrefixTabs) {
		const uint32_t Count = std::min(Size, PrefixTabs - Offset);
		Dst.append(Count, '\t');
		Offset += Count;
		Size -= Count;
	}
	// 本文に重なる範囲の追記
	if(Size) Dst.append(Text, Offset - PrefixTabs, Size);
	// 終了
	return;
}

/**
 * 行のテキストを共有するコンストラクタ
 * 計算量：固定数のメンバに対し O(1)
 * @param Start テキスト全体の中の行頭
 * @param End テキスト全体の中の行末
 * @param IsBlank 構造から定めた空行か
 * @param Storage 共有する変更不能なテキスト
 * @param PrefixTabs 共有するテキストの前へ加えるタブの数
 */
CodeFragment::CodeFragment(
	const uint32_t Start,
	const uint32_t End,
	const bool IsBlank,
	std::shared_ptr<const std::string> Storage,
	const uint32_t PrefixTabs
) : Start(Start), End(End), IsBlank(IsBlank), Storage(std::move(Storage)), Text(*this->Storage), PrefixTabs(PrefixTabs) {}

/**
 * 断片の読取専用の並びの取得関数
 * @return 行毎の断片（空行を含む）
 */
std::span<const CodeFragment> LaidOutCode::GetFragments() const {
	// 断片の並びの返戻
	return Fragments;
}

/**
 * コードとコメントの配置計画の合成関数（整形仕様 3.2, 9.1）
 * コードの全断片を元の順に１回ずつ出す間へ，配置枠へ差し込むコメント行の列を出力の順に並べ，テキストを実体化する前に大きさを見積もる
 * 同じ行では配置枠の種類の順，文中ではテキストの位置の順，同じ配置枠では計画の順に出す
 * 計算量：割当の数 C，コメントの行の数 L に対し O(C log(C + 1) + L + 1)
 * @param Code 共有する変更不能なコードの出力
 * @param Plan コメントの配置計画
 * @return 合成結果
 * @throws FormatLimitExceeded 配置の計画が別のコードの物である場合と，出力が大きさの上限を超える場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 */
OutputDocument OutputDocument::Compose(std::shared_ptr<const LaidOutCode> Code, CommentPlacementPlan Plan) {
	// 別の出力に属する計画の不具合の検出
	if(Plan.GetOwner() != Code->GetIdentity()) {
		throw FormatLimitExceeded("comment placement plan belongs to another output", FailureReason::FormatterDefect);
	}
	// 配置枠の出力順への整列
	std::stable_sort(
		Plan.Insertions.begin(),
		Plan.Insertions.end(),
		// 配置枠の断片・種類・位置に従う割当の比較
		[](const CommentInsertion &Left, const CommentInsertion &Right) -> bool {
			// 断片・種類・テキストの位置の順の比較の返戻
			return std::tie(Left.Slot.Fragment, Left.Slot.Kind, Left.Slot.Offset) <
			std::tie(Right.Slot.Fragment, Right.Slot.Kind, Right.Slot.Offset);
		}
	);
	// 実際にシリアライズするコードとコメントの長さ
	const std::span<const CodeFragment> Fragments = Code->GetFragments();
	size_t Size = Fragments.empty() ? 0 : Fragments.back().End; // 配置済の連続した行は０から始まり，末尾位置が行間の改行込みの長さを持つ
	for(const CommentInsertion &Insertion : Plan.Insertions) {
		const bool HasPrev = Size;
		const std::span<const std::string> Lines = Insertion.Comment.GetLines();
		Size += Lines.size() - 1;
		for(const std::string &Line : Lines) Size += Line.size();
		switch(Insertion.Slot.Kind) {
		case CommentSlotKind::BeforeLine:
		case CommentSlotKind::OwnLine:
			++Size;
			break;
		case CommentSlotKind::DocEnd:
			Size += HasPrev;
			break;
		case CommentSlotKind::Inline:
		case CommentSlotKind::LineEnd:
			break;
		}
	}
	if(Size + (Size && (Code->GetEnding().DoesEndWithNewline || Fragments.empty())) > MaxInputBytes) {
		throw FormatLimitExceeded("output exceeds size limit");
	}
	// 合成結果の返戻
	return OutputDocument(std::move(Code), std::move(Plan), Size);
}

/**
 * アンカーと断片の位置表の取得関数
 * @return 配置の段がアンカーの位置と行毎の深さを引く読取専用の位置表
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
 * 埋込の後続コード行の深さの取得関数
 * @return 後続の閉タグの深さ（埋込以外は無い，閉タグに独立枠が無い子は０）
 */
std::optional<uint32_t> LaidOutCode::GetFollowingTabs() const {
	// 埋込の後続コード行の深さの返戻
	return FollowingTabs;
}

/**
 * テキストの位置から始まるトークン片の取得関数（配置の段と検証が共有する）
 * 計算量：出力したトークン片の数 P に対し O(log(P + 1))
 * @param Offset テキストの位置
 * @return 位置から始まるトークン片（無ければ無い）
 */
const CodePiece *LaidOutCode::GetPieceAt(const uint32_t Offset) const {
	// テキストの位置から始まるトークン片の取得
	const CodePiece *const Found = GetPieceFrom(Offset);
	// 位置から始まるトークン片の返戻
	return Found && Found->Start == Offset ? Found : nullptr;
}

/**
 * 後のコードの断片の取得関数（配置の段と検証が共有する）
 * 計算量：次の非空断片迄の断片数 N に対し O(N)
 * @param Fragment 基準の断片
 * @return 空行を飛ばした次の断片（無ければ断片の数）
 */
uint32_t LaidOutCode::GetNextCodeFragment(uint32_t Fragment) const {
	// 後のコードの断片の取得
	const size_t Count = Fragments.size();
	for(++Fragment; Fragment < Count && Fragments[Fragment].IsBlank;) ++Fragment;
	// 次のコードの断片の返戻
	return static_cast<uint32_t>(std::min<size_t>(Fragment, Count));
}

/**
 * 配置枠の取得関数
 * 置ける零幅の位置にだけ配置枠を与え，空行の断片・差込禁止領域に接する行単位の位置・ノードの出力の端でない文中の位置には与えない
 * 計算量：文中の配置枠の数 S に対し O(log(S + 1))
 * @param Kind 配置枠の種類
 * @param Fragment 断片の番号（ドキュメントの末尾では使わない）
 * @param Offset 文中の配置枠のテキストの位置（文中でなければ使わない）
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
		Kind == CommentSlotKind::DocEnd ? static_cast<uint32_t>(LineKinds.size()) : Fragment,
		Kind == CommentSlotKind::Inline ? Offset : 0,
		Kind
	};
	// 此の配置に在る場合だけの求めた配置枠の返戻
	return IsOwnedSlot(Slot) ? std::optional<CommentSlotId>(Slot) : std::nullopt;
}

/**
 * コメント行の記法と終端条件の確認関数
 * 物理行の間に在る LF を境界とし，ブロックコメントの深さと埋込ドキュメントの状態を次行へ渡す
 * 計算量：行の長さの和 N と行数 L に対し O(N + L)
 * @param Rules 言語の記法の定義
 * @param Lines デコード不要の場合に読む物理行
 * @param Decoded デコードが必要な場合に読む末尾 LF を伴う展開済のテキスト
 * @param NeedsLineEnd 行末を必要とする記法を持つかの格納先
 * @param HasLineHeadNotation 行頭で開く記法を持つかの格納先
 * @return 列全体が使用可能なコメント記法だけから成れば true
 */
bool CommentBlock::CanReadNotation(
	const Core::NotationRules &Rules,
	const std::span<const std::string> Lines,
	const std::string_view Decoded,
	bool &NeedsLineEnd,
	bool &HasLineHeadNotation
) {
	// 行を跨ぐ記法の状態と開始・終端記号
	size_t Depth = 0;
	bool IsEmbedded = false;
	const std::string_view BlockOpener = Core::CommentRepository::GetOpenerOf(CommentNotation::Block);
	const std::string_view BlockCloser = Core::CommentRepository::GetCloserOf(CommentNotation::Block);
	const std::string_view EmbeddedOpener = Core::CommentRepository::GetOpenerOf(CommentNotation::RubyEmbeddedDocument);
	const std::string_view EmbeddedCloser = Core::CommentRepository::GetCloserOf(CommentNotation::RubyEmbeddedDocument);
	constexpr CommentNotation LineForms[] =
	{ CommentNotation::HashBangLine, CommentNotation::HashLine, CommentNotation::SlashLine };
	// 元の行又はデコード済の行の順次読取
	for(size_t Next = 0; Rules.ShouldDecodeUnicodeEscapes ? Next < Decoded.size() : Next < Lines.size();) {
		std::string_view Line;
		if(Rules.ShouldDecodeUnicodeEscapes) {
			const size_t End = Decoded.find('\n', Next);
			Line = Decoded.substr(Next, End - Next);
			Next = End + 1;
		} else Line = Lines[Next++];
		if(IsEmbedded) {
			IsEmbedded = !Core::CommentRepository::IsLineHeadWord(Line, EmbeddedCloser);
			continue;
		}
		// 各行のコメントの始まりから終わり迄の読取
		for(size_t Pos = 0; Pos < Line.size();) {
			if(Depth) {
				if(Rules.IsBlockCommentNested && Line.substr(Pos).starts_with(BlockOpener)) ++Depth, Pos += BlockOpener.size();
				else if(Line.substr(Pos).starts_with(BlockCloser)) --Depth, Pos += BlockCloser.size();
				else ++Pos;
				continue;
			}
			Pos = Line.find_first_not_of(" \t", Pos);
			if(Pos == std::string_view::npos) break;
			const std::string_view Rest = Line.substr(Pos);
			if(Rules.IsAvailable(CommentNotation::Block) && Rest.starts_with(BlockOpener)) {
				Depth = 1;
				Pos += BlockOpener.size();
			} else if(
				Rules.IsAvailable(CommentNotation::RubyEmbeddedDocument) && !Pos &&
				Core::CommentRepository::IsLineHeadWord(Rest, EmbeddedOpener)
			) {
				HasLineHeadNotation = true;
				NeedsLineEnd = true;
				IsEmbedded = true;
				break;
			} else if(
				std::ranges::any_of(
					LineForms,
					// 使用可能な行コメントの開始記号との照合
					[&Rules, Rest](const CommentNotation Notation) -> bool {
						// 行形式の開始記号との一致の返戻
						return Rules.IsAvailable(Notation) && Rest.starts_with(Core::CommentRepository::GetOpenerOf(Notation));
					}
				)
			) {
				// 終端・行連結で本文又は後続コードが変わる列の返戻
				if(!Core::CommentRepository::CanHoldInLineComment(Rules, Rest, true)) return false;
				NeedsLineEnd = true;
				break;
			// コメントで始まらないテキストの返戻
			} else return false;
		}
	}
	// 全てのコメントが閉じた事の返戻
	return !Depth && !IsEmbedded;
}

/**
 * 確認済の記法を配置枠で使えるかの判定関数
 * 計算量：配置先のコード行の長さ N に対し O(N)
 * @param Rules 言語の記法の定義
 * @param Code 配置枠を与えた変更不能なコード
 * @param Slot 所属を確認済の配置枠
 * @param NeedsLineEnd 行末を必要とする記法を持つか
 * @param HasLineHeadNotation 行頭で開く記法を持つか
 * @return 記法の開始・終端条件を配置枠で満たせば true
 * @throws std::out_of_range 配置枠の位置がコード行の範囲を超える場合
 */
bool CommentBlock::CanUseSlot(
	const Core::NotationRules &Rules,
	const LaidOutCode &Code,
	const CommentSlotId &Slot,
	const bool NeedsLineEnd,
	const bool HasLineHeadNotation
) {
	// 行頭で開く記法をコード行の末尾へ置かない事の返戻
	if(Slot.Kind == CommentSlotKind::LineEnd && HasLineHeadNotation) return false;
	// 行末を必要とする記法で後続コードを吸わせない事の確認
	if(Slot.Kind == CommentSlotKind::Inline && NeedsLineEnd) {
		const CodeFragment &Row = Code.GetFragments()[Slot.Fragment];
		const std::string_view Tail = Core::CommentRepository::TrimLeadingSeparator(
			Row.Text.substr(std::max(Slot.Offset - Row.Start, Row.PrefixTabs) - Row.PrefixTabs)
		);
		// コメントを閉じる言語の終端以外の後続コードの返戻
		return !Rules.LineCommentTerminator.empty() && Tail.starts_with(Rules.LineCommentTerminator);
	}
	// 配置出来る記法の返戻
	return true;
}

/**
 * 条件を満たすコメント行の列の構築関数（整形仕様 3.2）
 * 各物理行はコメントの本文又は記号を含み，空白だけの行を持たず，列の全体が其の言語のコメントだけから成る
 * 行コメントは行末迄，ブロックコメントは閉じる迄（ネストに出来る言語は対を数え）とし，其の外に空白以外を持たない
 * 計算量：行の長さの和 N と文中の配置枠の数 S に対し O(N + log(S + 1))
 * @param Rules 言語の記法の定義（コメントのトークンの規則）
 * @param Code 配置枠を与えた変更不能なコード
 * @param Lines 物理行の並び（LF を含まない，言語が本文として保持する CR は含み得る）
 * @param Slot 配置する枠
 * @return コメント行の列（条件を満たせない場合は空）
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 * @throws std::out_of_range ノード・グループ又は共有範囲が参照先の表又はテキストに無い場合
 */
std::optional<CommentBlock> CommentBlock::Make(
	const Core::NotationRules &Rules,
	const LaidOutCode &Code,
	std::vector<std::string> Lines,
	const CommentSlotId &Slot
) {
	// 行を持たない列又は此の出力の配置枠でない物の返戻
	if(Lines.empty() || !Code.GetSlots().IsOwnedSlot(Slot)) return std::nullopt;
	// 各物理行の条件の確認
	for(const std::string &Line : Lines) {
		// 改行を含む行と空白だけの行の返戻
		if(Line.find('\n') != std::string::npos || Line.find_first_not_of(" \t") == std::string::npos) return std::nullopt;
	}
	// デコードを必要とする記法だけの連結
	std::string Decoded;
	if(Rules.ShouldDecodeUnicodeEscapes) {
		std::string Joined;
		for(const std::string &Line : Lines) Joined.append(Line).push_back('\n');
		std::optional<std::string> Text = ParseAdapter::DecodeCommentEscapes(Joined);
		// トークンとして不正なエスケープを含む列の返戻
		if(!Text) return std::nullopt;
		Decoded = std::move(*Text);
	}
	bool NeedsLineEnd = false, HasLineHeadNotation = false;
	// 記法又は配置の条件を満たさない列の返戻
	if(
		!CanReadNotation(Rules, Lines, Decoded, NeedsLineEnd, HasLineHeadNotation) ||
		!CanUseSlot(Rules, Code, Slot, NeedsLineEnd, HasLineHeadNotation)
	) return std::nullopt;
	// 条件を満たした列の返戻
	return CommentBlock(std::move(Lines), Slot, NeedsLineEnd, HasLineHeadNotation);
}

/**
 * 記法の確認済の条件を別の配置枠へ移す関数
 * 配置計画に属する非空のブロックを受け取り，本文を再読せず確認済の配置先で記法の開始・終端条件だけを確認する
 * 計算量：配置先のコード行の長さ N に対し O(N)
 * @param Rules 構築時と同じ言語の記法の定義
 * @param Code 配置先の変更不能なコード
 * @param Dst 配置先のコードが与えた確認済の枠
 * @return 記法の条件を満たす配置枠へ変更出来れば true
 * @throws std::out_of_range 配置枠の位置がコード行の範囲を超える場合
 */
bool CommentBlock::IsRebindingSuccessful(const Core::NotationRules &Rules, const LaidOutCode &Code, const CommentSlotId &Dst) {
	// 記法の条件を満たさない配置枠を変更しない事の返戻
	if(!CanUseSlot(Rules, Code, Dst, NeedsLineEnd, HasLineHeadNotation)) return false;
	Slot = Dst;
	// 配置先を変更した事の返戻
	return true;
}

/**
 * 確認済の配置枠と一致するかの判定関数
 * @param Dst 配置先の枠
 * @return 確認済の配置枠と同じ場合 true
 */
bool CommentBlock::CanPlaceAt(const CommentSlotId &Dst) const {
	// 確認済の配置枠との一致の返戻
	return Slot.Owner == Dst.Owner && Slot.Fragment == Dst.Fragment && Slot.Offset == Dst.Offset && Slot.Kind == Dst.Kind;
}

/**
 * 割当の追加関数
 * 計算量：格納済の割当数 C に対し償却 O(1)，領域の伸長時 O(C)
 * @param Slot 配置枠
 * @param Comment コメント行の列
 * @return 確認済の非空のブロックを此の計画の配置枠へ追加出来れば true
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error テキスト・要素又は作業列が保持領域の最大長を超える場合
 */
bool CommentPlacementPlan::IsInsertionSuccessful(const CommentSlotId &Slot, CommentBlock &&Comment) {
	// 所有者・配置枠の不一致又は空のブロックの返戻
	if(Slot.Owner != Owner || Comment.GetLines().empty() || !Comment.CanPlaceAt(Slot)) return false;
	Insertions.push_back({ Slot, std::move(Comment) });
	// 追加した事の返戻
	return true;
}

/**
 * 割当の読取専用の並びの取得関数
 * @return 割当の並び（同じ配置枠では並びの順に出力する）
 */
std::span<const CommentInsertion> CommentPlacementPlan::GetInsertions() const {
	// 割当の並びの返戻
	return Insertions;
}

/**
 * コメントを全て除いた中核の合成関数
 * 計算量：コードの断片の数 F に対して O(F)
 * @param Code 共有する変更不能なコードの出力
 * @return 子の配置計画も差し込まない合成結果
 * @throws FormatLimitExceeded 出力が大きさの上限を超える場合
 * @throws std::bad_alloc テキスト・要素・作業列又は呼出先のメモリを確保出来ない場合
 */
OutputDocument OutputDocument::CodeOnly(std::shared_ptr<const LaidOutCode> Code) {
	// 共有所有を移す前の配置計画の構築
	CommentPlacementPlan Plan(*Code);
	// 中核の合成結果の返戻
	return Compose(std::move(Code), std::move(Plan));
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
 * @return 配置枠とコメント行の列の組の並び（シリアライズする順）
 */
const CommentPlacementPlan &OutputDocument::GetComments() const {
	// 配置計画の返戻
	return Comments;
}

/**
 * シリアライズするテキストの長さの取得関数
 * @return 合成で見積もった長さ（終端契約の改行を除く）
 */
size_t OutputDocument::GetSize() const {
	// 長さの返戻
	return Size;
}
