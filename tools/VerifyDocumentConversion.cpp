#include "Core/CommentRepository.hpp"
#include "Profile/GrammarProfile.hpp"
#include "Util/LayoutTypes.hpp"
#include "Util/SyntaxValidator.hpp"
#include "Util/ValidationContext.hpp"
#include <cstddef>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * 試験条件の検査関数
 * @param IsSatisfied 成功条件
 * @param Message 失敗箇所の説明
 * @throws std::runtime_error 条件が成り立たない場合
 */
static void Check(const bool IsSatisfied, const char *const Message) {
	// 最初の不一致での試験停止と例外文面への失敗位置保存
	if(!IsSatisfied) throw std::runtime_error(Message);
	// 終了
	return;
}

/**
 * 通常コメントへ変換した文書の照合の試験関数（整形仕様 8.1, 11.2）
 * 期待コメントの正規化後の本文・論理行・順序・移動先を保つ塊を認め，欠落・複製・順序違反・移動先の不一致・改変を拒む事を確かめる
 * 正規化し直せば期待の本文に一致する完成候補（正規化の違反）と，入力の儘の本文を置いた完成候補も拒む事を確かめる
 * 完成候補の字面から塊を組む段が，行の最初から始まり行の終わりで終わるコメントの続きだけを塊とし，塊の後のコード行を断片へ対応付ける事も確かめる
 * 直付の文書の照合が，説明対象毎の期待の論理行と，文書に読まれた通常コメントの論理行を認め，文書の論理行の複製・欠落・改変，記法の区別の変更，期待に無い対象への文書の追加を拒む事も確かめる
 * @return 検査の成否に応じた終了状態
 */
int main() {
	// 全検査の例外境界
	try {
		// 期待コメントの２つの成員の独立行の文書の正規化後の本文（入力の `青。空` は文毎の論理行と為る）を，何れも断片０の行の前へ移す変換
		const std::vector<SyntaxValidator::ConvertedDocument> Converted { { { "赤" }, { 0 } }, { { "青", "空" }, { 0 } } };
		// 塊の論理行（通常コメントの物）を与えて照合する
		const auto Keeps = [&Converted](const uint32_t Next, std::vector<std::string> &&Lines) -> bool {
			std::vector<uint8_t> Documents(Lines.size(), 0);
			const std::vector<SyntaxValidator::CommentRun> Runs { { Next, std::move(Lines), std::move(Documents) } };
			// 照合の結果の返戻
			return SyntaxValidator::KeepsConvertedDocuments(Converted, {}, Runs);
		};
		// 正規化後の本文（文毎の論理行）を原文の順に保つ塊は認める
		Check(Keeps(0, { "赤", "青", "空" }), "a kept conversion was rejected");
		// 変換しない通常コメントが間に在っても認める
		Check(Keeps(0, { "赤", "TODO", "青", "空" }), "an ordinary comment between conversions was rejected");
		// 本文の欠落
		Check(!Keeps(0, { "赤" }), "a lost conversion was accepted");
		// 論理行の欠落
		Check(!Keeps(0, { "赤", "青" }), "a lost logical line was accepted");
		// 本文の複製
		Check(!Keeps(0, { "赤", "青", "空", "赤" }), "a duplicated conversion was accepted");
		// 原文の順の違反
		Check(!Keeps(0, { "青", "空", "赤" }), "a reordered conversion was accepted");
		// 認めない移動先
		Check(!Keeps(1, { "赤", "青", "空" }), "a conversion at another destination was accepted");
		// 本文の改変
		Check(!Keeps(0, { "赤い", "青", "空" }), "an altered conversion was accepted");
		// 正規化し直せば一致する句点の残った本文（完成候補を再正規化して差を消さない）
		Check(!Keeps(0, { "赤．", "青", "空" }), "a body that only renormalization would match was accepted");
		// 同期・本文正規化済の期待と異なる入力の儘の本文
		Check(!Keeps(0, { "赤", "青。空" }), "an unnormalized input body was accepted");
		{
			// 空行で分けた塊を跨いでも，同じコード行の前の文書は原文の順に現れる事を求める
			const std::vector<SyntaxValidator::ConvertedDocument> Pair { { { "A" }, { 0 } }, { { "B" }, { 0 } } };
			const std::vector<SyntaxValidator::CommentRun> Ordered { { 0, { "A" }, { 0 } }, { 0, { "B" }, { 0 } } };
			const std::vector<SyntaxValidator::CommentRun> Reversed { { 0, { "B" }, { 0 } }, { 0, { "A" }, { 0 } } };
			Check(SyntaxValidator::KeepsConvertedDocuments(Pair, {}, Ordered), "conversions split by a blank line were rejected");
			Check(!SyntaxValidator::KeepsConvertedDocuments(Pair, {}, Reversed), "conversions reordered across runs were accepted");
		}
		{
			// 同じ本文を同じ移動先へ置く文書群の全件一致と複製の数の照合
			const std::vector<SyntaxValidator::ConvertedDocument> Repeated(2, { { "A" }, { 0 } });
			const auto KeepsRepeated = [&Repeated](const size_t Count) -> bool {
				const std::vector<SyntaxValidator::CommentRun> Runs {
					{ 0, std::vector<std::string>(Count, "A"), std::vector<uint8_t>(Count, 0) }
				};
				// 照合の結果の返戻
				return SyntaxValidator::KeepsConvertedDocuments(Repeated, {}, Runs);
			};
			Check(KeepsRepeated(2), "repeated conversions were rejected");
			Check(!KeepsRepeated(1), "a missing repeated conversion was accepted");
			Check(!KeepsRepeated(3), "an extra repeated conversion was accepted");
		}
		{
			// 複数の移動先を認めても，最初の文書には完成候補で先に現れる一致を割り当てる
			const std::vector<SyntaxValidator::ConvertedDocument> Pair { { { "A" }, { 1, 0 } }, { { "A" }, { 0 } } };
			const std::vector<SyntaxValidator::CommentRun> Runs { { 0, { "A" }, { 0 } }, { 1, { "A" }, { 0 } } };
			Check(!SyntaxValidator::KeepsConvertedDocuments(Pair, {}, Runs), "a later destination was chosen before an earlier match");
		}
		{
			// 文書の記法の儘置いた変換（別の対象の文書に為る）
			const std::vector<SyntaxValidator::CommentRun> Runs { { 0, { "赤", "青", "空" }, { 1, 0, 0 } } };
			Check(!SyntaxValidator::KeepsConvertedDocuments(Converted, {}, Runs), "a conversion left as a document was accepted");
		}
		{
			// 他の文書の論理行と同じ本文の文書（部分の一致を複製と数えない）
			const std::vector<SyntaxValidator::ConvertedDocument> Nested { { { "古い", "使わない" }, { 0 } }, { { "使わない" }, { 0 } } };
			const std::vector<SyntaxValidator::CommentRun> Runs { { 0, { "古い", "使わない", "使わない" }, { 0, 0, 0 } } };
			Check(SyntaxValidator::KeepsConvertedDocuments(Nested, {}, Runs), "a partial match was counted as a duplicate");
			// 同じ本文の期待コメントの通常コメントは複製と数えない
			const std::vector<SyntaxValidator::CommentRun> WithOrdinary { { 0, { "古い", "使わない", "使わない", "使わない" }, { 0, 0, 0, 0 } } };
			const std::vector<std::vector<std::string>> Ordinary { { "使わない" } };
			Check(
				SyntaxValidator::KeepsConvertedDocuments(Nested, Ordinary, WithOrdinary),
				"an ordinary comment was counted as a duplicate"
			);
			Check(!SyntaxValidator::KeepsConvertedDocuments(Nested, {}, WithOrdinary), "an extra copy was accepted");
		}
		{
			// 期待コメントの通常コメントの一部と同じ本文の文書は認め，別の塊への写しは複製とする
			const std::vector<SyntaxValidator::ConvertedDocument> Single { { { "使わない" }, { 0 } } };
			const std::vector<std::vector<std::string>> Ordinary { { "古い", "使わない" } };
			const std::vector<SyntaxValidator::CommentRun> Runs { { 0, { "古い", "使わない", "使わない" }, { 0, 0, 0 } } };
			Check(
				SyntaxValidator::KeepsConvertedDocuments(Single, Ordinary, Runs),
				"a part of an ordinary comment was counted as a duplicate"
			);
			const std::vector<SyntaxValidator::CommentRun> Copied { { 0, { "使わない" }, { 0 } }, { 5, { "使わない" }, { 0 } } };
			Check(!SyntaxValidator::KeepsConvertedDocuments(Single, {}, Copied), "a copy in another run was accepted");
		}
		// 完成候補の字面から組む塊（行の最初から行の終わり迄のコメントの続きだけ，空行で分ける）
		const GrammarProfile &Profile = GrammarProfile::Resolve(Lang::Get(Lang::Cpp));
		const Core::NotationRules &Rules = Profile.GetRules().Notation;
		constexpr std::string_view Text = "int a;\n// 赤\n// 青\n\n// 空\nint b; // 行末\nint c;\n";
		const std::vector<std::pair<uint32_t, uint32_t>> Ranges { { 7, 13 }, { 14, 20 }, { 22, 28 }, { 36, 45 } };
		Core::CommentRepository Records;
		for(const char *const Body : { "赤", "青", "空", "行末" }) {
			CommentRecord Record { Body, { CommentPlacement::Before } };
			Core::CommentRepository::AttachRules(Rules, Record);
			Records.Add(std::move(Record));
		}
		// コード行 `int a;` `int b;` `int c;` の始まりと，其れを断片０，２，３とする対応
		const std::vector<size_t> CodeStarts { 0, 29, 46 };
		const std::vector<uint32_t> CodeLines { 0, 2, 3 };
		const std::vector<SyntaxValidator::CommentRun> Runs =
		SyntaxValidator::CommentRuns(Text, Ranges, Records, CodeStarts, CodeLines, Profile.GetReading());
		Check(Runs.size() == 2, "comment runs were not split at the blank line or included a line-end comment");
		Check(Runs[0].Documents == std::vector<uint8_t>{ 0, 0 }, "ordinary comments were marked as documents");
		Check(
			Runs[0].Lines == std::vector<std::string>{ "赤", "青" } && Runs[0].Next == 2,
			"the first run or its next code line was wrong"
		);
		Check(Runs[1].Lines == std::vector<std::string>{ "空" } && Runs[1].Next == 2, "the second run or its next code line was wrong");
		{
			// 字面から組んだ塊を其の儘照合へ渡し，句点の残った本文を正規化し直さずに拒む事を一続きで確かめる
			constexpr std::string_view Converted = "int a;\n// 赤．\nint b;\n";
			const std::vector<std::pair<uint32_t, uint32_t>> Range { { 7, 16 } };
			Core::CommentRepository Record;
			CommentRecord Body { "赤．", { CommentPlacement::Before } };
			Core::CommentRepository::AttachRules(Rules, Body);
			Record.Add(std::move(Body));
			const std::vector<size_t> Starts { 0, 17 };
			const std::vector<uint32_t> Lines { 0, 2 };
			const std::vector<SyntaxValidator::CommentRun> Built =
			SyntaxValidator::CommentRuns(Converted, Range, Record, Starts, Lines, Profile.GetReading());
			Check(Built.size() == 1 && Built[0].Next == 2, "the run of the unnormalized conversion was wrong");
			Check(
				!SyntaxValidator::KeepsConvertedDocuments(std::vector<SyntaxValidator::ConvertedDocument>{ { { "赤" }, { 2 } } }, {}, Built),
				"an unnormalized conversion read from the text was accepted"
			);
		}
		{
			// 文書として置かない期待コメントの通常コメント（同じ記法の言語では文書の前に並べば文書に読まれる）
			CommentRecord Loose { "TODO", { CommentPlacement::Before } };
			Core::CommentRepository::AttachRules(Rules, Loose);
			const std::vector<CommentRecord> Outputs { std::move(Loose) };
			const std::vector<Core::NodeId> Placed { Core::NoNode };
			// 説明対象 `f` の通常コメントと同じ記法の文書と，其の照合
			const std::vector<ValidationContext::DocumentFact> Expected { { "f", { "説明" }, false } };
			const auto KeepsDocument = [&](std::vector<ValidationContext::DocumentFact> &&Actual) -> bool {
				// 照合の結果の返戻
				return SyntaxValidator::KeepsDocuments(Expected, Actual, Outputs, Placed);
			};
			Check(KeepsDocument({ { "f", { "説明" }, false } }), "a kept document was rejected");
			// 文書の前に並べた通常コメントが文書に読まれた完成候補は認める
			Check(KeepsDocument({ { "f", { "TODO", "説明" }, false } }), "an ordinary comment read as a document was rejected");
			// 文書の論理行の複製
			Check(!KeepsDocument({ { "f", { "説明", "説明" }, false } }), "a duplicated document line was accepted");
			// 通常コメントの論理行の複製
			Check(!KeepsDocument({ { "f", { "TODO", "TODO", "説明" }, false } }), "a duplicated ordinary line in a document was accepted");
			// 文書の欠落と改変
			Check(!KeepsDocument({}), "a lost document was accepted");
			Check(!KeepsDocument({ { "f", { "説明する" }, false } }), "an altered document was accepted");
			// 通常コメントとの記法の区別の変更
			Check(!KeepsDocument({ { "f", { "説明" }, true } }), "a document with another notation class was accepted");
			// 期待に無い説明対象へ増えた，通常コメントと区別する記法の文書
			Check(!KeepsDocument({ { "f", { "説明" }, false }, { "g", { "TODO" }, true } }), "a document on another target was accepted");
		}
		std::puts(
			"document conversion: kept, lost, duplicated, reordered (within and across runs), moved, altered, unnormalized and document-notation conversions, and kept, merged, duplicated, lost, altered, reclassified and added documents passed"
		);
	} catch(const std::exception &E) {
		std::fprintf(stderr, "document conversion verification failed: %s\n", E.what());
		// 検査失敗の返戻
		return 1;
	}
	// 全検査成功の返戻
	return 0;
}
