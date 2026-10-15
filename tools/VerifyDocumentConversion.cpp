#include "Core/CommentRepository.hpp"
#include "Core/NodeId.hpp"
#include "Core/SyntaxRole.hpp"
#include "Profile/GrammarProfile.hpp"
#include "Util/CommentNormalizer.hpp"
#include "Util/Lang.hpp"
#include "Util/LayoutTypes.hpp"
#include "Util/SyntaxValidator.hpp"
#include "Util/ValidationContext.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * テスト条件の検査関数
 * 計算量：失敗時の説明の長さ B に対し O(B)（成功時は O(1)）
 * @param IsSatisfied 成功条件
 * @param Msg 失敗箇所の説明（NUL 終端の有効な文字列）
 * @throws std::runtime_error 条件が成り立たない場合
 * @throws std::bad_alloc 失敗を通知する例外のメモリを確保出来ない場合
 */
static void Check(const bool IsSatisfied, const char *const Msg) {
	// 最初の不一致でのテスト停止と例外文面への失敗位置保存
	if(!IsSatisfied) throw std::runtime_error(Msg);
	// 終了
	return;
}

/**
 * 通常コメントへ変換したドキュメントの照合のテスト関数（整形仕様 8.1, 11.2）
 * 期待コメントの正規化後の本文・論理行・順序・移動先を保つブロックを認め，欠落・複製・順序違反・移動先の不一致・改変を拒む事を確かめる
 * 正規化し直せば期待の本文に一致する完成候補（正規化の違反）と，入力の儘の本文を置いた完成候補も拒む事を確かめる
 * 直付のドキュメントの照合が，説明対象毎の期待の論理行と，ドキュメントに読まれた通常コメントの論理行を認め，ドキュメントの論理行の複製・欠落・改変，記法の区別の変更，期待に無い対象へのドキュメントの追加を拒む事も確かめる
 * 計算量：固定テストケースの構築と照合の総費用 Q に対し O(Q)
 * @return 検査の成否に応じた終了状態
 */
int main() {
	// 全検査の例外境界
	try {
		// 期待コメントの２つのメンバの独立行のドキュメントの正規化後の本文（入力の `青。空` は文毎の論理行と為る）を，何れも断片０の行の前へ移す変換
		const std::vector<SyntaxValidator::ConvertedDoc> Converted { { { "赤" }, { 0 } }, { { "青", "空" }, { 0 } } };
		// 通常コメントのブロックの論理行の照合
		const auto HasMatchingRun = [&Converted](const uint32_t Next, std::vector<std::string> &&Lines) -> bool {
			// 通常コメントのブロックのドキュメント標識の準備
			std::vector<uint8_t> Docs(Lines.size(), 0);
			// 照合の結果の返戻
			return SyntaxValidator::HasMatchingConvertedDocs(
				Converted,
				{},
				std::vector<SyntaxValidator::CommentRun>{ { Next, std::move(Lines), std::move(Docs) } }
			);
		};
		// 正規化後の本文（文毎の論理行）を原文の順に保つブロックは認める
		Check(HasMatchingRun(0, { "赤", "青", "空" }), "a kept conversion was rejected");
		// 変換しない通常コメントが間に在っても認める
		Check(HasMatchingRun(0, { "赤", "TODO", "青", "空" }), "an ordinary comment between conversions was rejected");
		// 本文の欠落
		Check(!HasMatchingRun(0, { "赤" }), "a lost conversion was accepted");
		// 論理行の欠落
		Check(!HasMatchingRun(0, { "赤", "青" }), "a lost logical line was accepted");
		// 本文の複製
		Check(!HasMatchingRun(0, { "赤", "青", "空", "赤" }), "a duplicated conversion was accepted");
		// 原文の順の違反
		Check(!HasMatchingRun(0, { "青", "空", "赤" }), "a reordered conversion was accepted");
		// 認めない移動先
		Check(!HasMatchingRun(1, { "赤", "青", "空" }), "a conversion at another destination was accepted");
		// 本文の改変
		Check(!HasMatchingRun(0, { "赤い", "青", "空" }), "an altered conversion was accepted");
		// 正規化し直せば一致する句点の残った本文（完成候補を再正規化して差を消さない）
		Check(!HasMatchingRun(0, { "赤．", "青", "空" }), "a body that only renormalization would match was accepted");
		// 同期・本文正規化済の期待と異なる入力の儘の本文
		Check(!HasMatchingRun(0, { "赤", "青。空" }), "an unnormalized input body was accepted");
		{
			// 空行で分けたブロックを跨ぐ，同じコード行の前のドキュメントの原文順の照合
			const std::vector<SyntaxValidator::ConvertedDoc> Pair { { { "A" }, { 0 } }, { { "B" }, { 0 } } };
			Check(
				SyntaxValidator::HasMatchingConvertedDocs(
					Pair,
					{},
					std::vector<SyntaxValidator::CommentRun>{ { 0, { "A" }, { 0 } }, { 0, { "B" }, { 0 } } }
				),
				"conversions split by a blank line were rejected"
			);
			Check(
				!SyntaxValidator::HasMatchingConvertedDocs(
					Pair,
					{},
					std::vector<SyntaxValidator::CommentRun>{ { 0, { "B" }, { 0 } }, { 0, { "A" }, { 0 } } }
				),
				"conversions reordered across runs were accepted"
			);
		}
		{
			// 同じ本文を同じ移動先へ置くドキュメントグループの全件一致と複製の数の照合
			const std::vector<SyntaxValidator::ConvertedDoc> Repeated(2, { { "A" }, { 0 } });
			// 同じドキュメントの複製の拒否の確認
			const auto HasMatchingRepeats = [&Repeated](const size_t Count) -> bool {
				// 複製した論理行のブロックの構築
				const std::vector<SyntaxValidator::CommentRun> Runs {
					{ 0, std::vector<std::string>(Count, "A"), std::vector<uint8_t>(Count, 0) }
				};
				return SyntaxValidator::HasMatchingConvertedDocs(Repeated, {}, Runs);
			};
			Check(HasMatchingRepeats(2), "repeated conversions were rejected");
			Check(!HasMatchingRepeats(1), "a missing repeated conversion was accepted");
			Check(!HasMatchingRepeats(3), "an extra repeated conversion was accepted");
		}
		{
			// 複数の移動先を持つ最初のドキュメントへの，完成候補で先に現れる一致の割当
			const std::vector<SyntaxValidator::ConvertedDoc> Pair { { { "A" }, { 1, 0 } }, { { "A" }, { 0 } } };
			const std::vector<SyntaxValidator::CommentRun> Runs { { 0, { "A" }, { 0 } }, { 1, { "A" }, { 0 } } };
			Check(!SyntaxValidator::HasMatchingConvertedDocs(Pair, {}, Runs), "a later destination was chosen before an earlier match");
		}
		{
			// ドキュメントの記法の儘置いた変換（別の対象のドキュメントに為る）
			const std::vector<SyntaxValidator::CommentRun> Runs { { 0, { "赤", "青", "空" }, { 1, 0, 0 } } };
			Check(!SyntaxValidator::HasMatchingConvertedDocs(Converted, {}, Runs), "a conversion left as a document was accepted");
		}
		{
			// 他のドキュメントの論理行と同じ本文のドキュメント（部分の一致を複製と数えない）
			const std::vector<SyntaxValidator::ConvertedDoc> Nested { { { "古い", "使わない" }, { 0 } }, { { "使わない" }, { 0 } } };
			const std::vector<SyntaxValidator::CommentRun> Runs { { 0, { "古い", "使わない", "使わない" }, { 0, 0, 0 } } };
			Check(SyntaxValidator::HasMatchingConvertedDocs(Nested, {}, Runs), "a partial match was counted as a duplicate");
			// 同じ本文の期待コメントの通常コメントは複製と数えない
			const std::vector<SyntaxValidator::CommentRun> WithOrdinary { { 0, { "古い", "使わない", "使わない", "使わない" }, { 0, 0, 0, 0 } } };
			Check(
				SyntaxValidator::HasMatchingConvertedDocs(Nested, std::vector<std::vector<std::string>>{ { "使わない" } }, WithOrdinary),
				"an ordinary comment was counted as a duplicate"
			);
			Check(!SyntaxValidator::HasMatchingConvertedDocs(Nested, {}, WithOrdinary), "an extra copy was accepted");
		}
		{
			// 期待コメントの一部と同じ本文のドキュメントの許容と，別のブロックへの複製の拒否
			const std::vector<SyntaxValidator::ConvertedDoc> Single { { { "使わない" }, { 0 } } };
			const std::vector<SyntaxValidator::CommentRun> Runs { { 0, { "古い", "使わない", "使わない" }, { 0, 0, 0 } } };
			Check(
				SyntaxValidator::HasMatchingConvertedDocs(Single, std::vector<std::vector<std::string>>{ { "古い", "使わない" } }, Runs),
				"a part of an ordinary comment was counted as a duplicate"
			);
			Check(
				!SyntaxValidator::HasMatchingConvertedDocs(
					Single,
					{},
					std::vector<SyntaxValidator::CommentRun>{ { 0, { "使わない" }, { 0 } }, { 5, { "使わない" }, { 0 } } }
				),
				"a copy in another run was accepted"
			);
		}
		// 完成候補のテキストから組むブロック（行の最初から行の終わり迄のコメントの続きだけ，空行で分ける）
		const GrammarProfile &Profile = GrammarProfile::Resolve(Lang::Get(Lang::Cpp));
		const Core::NotationRules &Rules = Profile.GetRules().Notation;
		{
			// テキストから組んだブロックを其の儘照合へ渡し，句点の残った本文を正規化し直さずに拒む事を一続きで確かめる
			constexpr std::string_view Converted = "int a;\n// 赤．\nint b;\n";
			Core::CommentRepository Record;
			CommentRecord Body { "赤．", { CommentPlacement::Before } };
			Core::CommentRepository::AttachRules(Rules, Body);
			Record.Add(std::move(Body));
			const std::vector<SyntaxValidator::CommentRun> Built = SyntaxValidator::CommentRuns(
				Profile.GetReading(),
				Record,
				Converted,
				std::vector<std::pair<uint32_t, uint32_t>>{ { 7, 16 } },
				std::vector<size_t>{ 0, 17 },
				std::vector<uint32_t>{ 0, 2 }
			);
			Check(
				!SyntaxValidator::HasMatchingConvertedDocs(std::vector<SyntaxValidator::ConvertedDoc>{ { { "赤" }, { 2 } } }, {}, Built),
				"an unnormalized conversion read from the text was accepted"
			);
		}
		{
			// ドキュメントとして置かない期待コメントの通常コメント（同じ記法の言語ではドキュメントの前に並べばドキュメントに読まれる）
			CommentRecord Loose { "TODO", { CommentPlacement::Before } };
			Core::CommentRepository::AttachRules(Rules, Loose);
			const std::vector<CommentRecord> Outputs { std::move(Loose) };
			const std::vector<Core::NodeId> Placed { Core::NoNode };
			// 説明対象と論理行からドキュメントの事実を作る関数（本文は論理行を連結した所有者，行は其の範囲）
			const auto MakeDoc =
			[](const std::string_view Target, const std::initializer_list<std::string_view> Lines, const bool IsDedicated)
			-> ValidationContext::DocFact {
				const std::shared_ptr<ValidationContext::DocStorage> Storage = std::make_shared<ValidationContext::DocStorage>();
				Storage->Targets = Target;
				for(const std::string_view Line : Lines) {
					Storage->Lines.emplace_back(Storage->Bodies.size(), Line.size());
					Storage->Bodies += Line;
				}
				// 論理行を範囲で持つ事実の返戻
				return { Storage, Storage->Targets, Storage->Bodies, Storage->Lines, IsDedicated };
			};
			// 説明対象 `f` の通常コメントと同じ記法のドキュメントと，其の照合
			const std::vector<ValidationContext::DocFact> Expected { MakeDoc("f", { "説明" }, false) };
			// ドキュメントの説明対象と論理行の照合
			const auto HasExpectedDocs = [&](std::vector<ValidationContext::DocFact> &&Actual) -> bool {
				return SyntaxValidator::HasSameDocs(Expected, Actual, Outputs, Placed);
			};
			Check(HasExpectedDocs({ MakeDoc("f", { "説明" }, false) }), "a kept document was rejected");
			// 同じ説明対象でも内側・外側の違いを失った候補は拒否する
			ValidationContext::DocFact Inner = MakeDoc("f", { "説明" }, false);
			Inner.IsInner = true;
			Check(!HasExpectedDocs({ std::move(Inner) }), "a changed documentation side was accepted");
			const std::vector<ValidationContext::DocFact> Blank { MakeDoc("f", {}, true) };
			Inner = Blank.front();
			Inner.IsInner = true;
			Check(
				!SyntaxValidator::HasSameDocs(Blank, std::vector<ValidationContext::DocFact>{ std::move(Inner) }, {}, {}),
				"a changed side of an empty document was accepted"
			);
			// ドキュメントの前に並べた通常コメントがドキュメントに読まれた完成候補は認める
			Check(HasExpectedDocs({ MakeDoc("f", { "TODO", "説明" }, false) }), "an ordinary comment read as a document was rejected");
			// ドキュメントの論理行の複製
			Check(!HasExpectedDocs({ MakeDoc("f", { "説明", "説明" }, false) }), "a duplicated document line was accepted");
			// 通常コメントの論理行の複製
			Check(
				!HasExpectedDocs({ MakeDoc("f", { "TODO", "TODO", "説明" }, false) }),
				"a duplicated ordinary line in a document was accepted"
			);
			// ドキュメントの欠落と改変
			Check(!HasExpectedDocs({}), "a lost document was accepted");
			Check(!HasExpectedDocs({ MakeDoc("f", { "説明する" }, false) }), "an altered document was accepted");
			// 通常コメントとの記法の区別の変更
			Check(!HasExpectedDocs({ MakeDoc("f", { "説明" }, true) }), "a document with another notation class was accepted");
			// 期待に無い説明対象へ増えた，通常コメントと区別する記法のドキュメント
			Check(
				!HasExpectedDocs({ MakeDoc("f", { "説明" }, false), MakeDoc("g", { "TODO" }, true) }),
				"a document on another target was accepted"
			);
		}
		std::puts(
			"document conversion: kept, lost, duplicated, reordered (within and across runs), moved, altered, "
			"unnormalized and document-notation conversions, and kept, merged, duplicated, lost, altered, "
			"reclassified and added documents passed"
		);
	} catch(const std::exception &E) {
		std::fprintf(stderr, "document conversion verification failed: %s\n", E.what());
		// 検査失敗の返戻
		return 1;
	}
	// 全検査成功の返戻
	return 0;
}
