#include "Formatter.hpp"
#include "Core/Canonicalizer.hpp"
#include "Core/CodeLayout.hpp"
#include "Core/CommentPlacer.hpp"
#include "Core/CommentRepository.hpp"
#include "Core/ControlFlow.hpp"
#include "Core/CoreObservation.hpp"
#include "Core/DocCommentReader.hpp"
#include "Core/DocCommentSynchronizer.hpp"
#include "Core/FormatIR.hpp"
#include "Core/LineBreaker.hpp"
#include "Core/OutputPosition.hpp"
#include "Core/SignatureExtractor.hpp"
#include "Core/SourceProjector.hpp"
#include "Core/SyntaxModel.hpp"
#include "Core/SyntaxNormalizer.hpp"
#include "Core/ValidatedOutput.hpp"
#include "Lint/LintAnalyzer.hpp"
#include "Profile/GrammarProfile.hpp"
#include "Profile/ReferenceProjection.hpp"
#include "Util/CommentNormalizer.hpp"
#include "Util/FormatLimitExceeded.hpp"
#include "Util/InputNormalizer.hpp"
#include "Util/Lang.hpp"
#include "Util/LayoutTypes.hpp"
#include "Util/LineDiff.hpp"
#include "Util/OutputFinalizer.hpp"
#include "Util/Parallel.hpp"
#include "Util/ParseAdapter.hpp"
#include "Util/ParsedTree.hpp"
#include "Util/ParserAllocator.hpp"
#include "Util/SyntaxParser.hpp"
#include "Util/SyntaxValidator.hpp"
#include "Util/WorkingMemory.hpp"
#include "Util/WorkSort.hpp"
#include <tree_sitter/api.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/**
 * テスト条件の検査関数
 * 計算量：失敗時の説明の長さ B に対し O(B)（成功時は O(1)）
 * @param IsSatisfied 成功条件
 * @param Msg 失敗箇所の説明
 * @param Src 固定入力のテキスト（指定しなければ入力を付けない）
 * @throws std::runtime_error 条件が成り立たない場合
 * @throws std::bad_alloc 失敗の説明のメモリを確保出来ない場合
 * @throws std::length_error 失敗の説明がテキストの上限を超える場合
 */
static void Check(const bool IsSatisfied, const char *const Msg, const char *const Src = nullptr) {
	// 最初の不一致でのテスト停止
	if(!IsSatisfied) throw std::runtime_error(Src ? std::string(Msg) + " input=" + Src : Msg);
	// 終了
	return;
}

/**
 * 安定整列の比較・移動の計上と二分探索の比較順の検査関数
 * 同じ値の順序と計数は最小の列から手で定める
 * @throws std::runtime_error 値・順序又は実作業の計上が期待と違う場合
 * @throws std::bad_alloc テストの列を確保出来ない場合
 */
static void CheckWorkSort() {
	std::vector<std::pair<int, int>> Values { { 2, 0 }, { 1, 1 }, { 2, 2 }, { 1, 3 } };
	size_t Work = 0, Comparisons = 0;
	WorkSort::Stable(
		std::span(Values),
		[&](const std::pair<int, int> &Left, const std::pair<int, int> &Right) -> bool {
			Work += 2;
			++Comparisons;
			// キーだけの大小の返戻
			return Left.first < Right.first;
		},
		[&](const size_t Amount) -> void {
			Work += Amount;
		}
	);
	// ２組の比較と次の合成の３回の比較，作業列４件と２回の移動各４件
	Check(Values == std::vector<std::pair<int, int>>{ { 1, 1 }, { 1, 3 }, { 2, 0 }, { 2, 2 } }, "work sort lost stability");
	Check(Comparisons == 5 && Work == 22, "work sort accounting changed");
	const std::array<int, 4> Sorted { 1, 3, 3, 5 };
	std::vector<int> Compared;
	const size_t Boundary = WorkSort::LowerBound(
		std::span<const int>(Sorted),
		3,
		[&](const int Item, const int Wanted) -> bool {
			Compared.push_back(Item);
			// 境界より小さい事の返戻
			return Item < Wanted;
		}
	);
	Check(Boundary == 1 && Compared == std::vector<int>{ 3, 3, 1 }, "work search comparison order changed");
	// 終了
	return;
}

// 製品の共通走査の回数を，構造の予算とは独立して観測する入口
struct ParsedTreeAccess {
	/**
	 * 宣言の有効範囲と前処理の訪問数の取得関数
	 * 計算量：O(1)
	 * @param Src 観測する解析結果
	 * @return 宣言の訪問数と前処理の訪問数
	 */
	static std::pair<size_t, size_t> GetBindingVisits(const ParsedTree &Src) {
		// 同じ世代の表に記録した訪問数の返戻
		return { Src.Syntax.Relations->NameVisits, Src.Syntax.Relations->MacroWork };
	}

	/**
	 * 親の平坦な記録とインデックスの作業量の取得関数
	 * 計算量：O(1)
	 * @param Src 観測する解析結果
	 * @return 挿入数・前順の記録数・スロット数・挿入時の読取数
	 */
	static std::array<size_t, 4> GetParentStorage(const ParsedTree &Src) {
		const ParsedTree::ParentTable &Table = Src.Syntax.Relations->Parents;
		return { Table.Insertions, Table.Records.size(), Table.Slots.size(), Table.InsertProbes }; // 平坦な表の作業量の返戻
	}

	/**
	 * 共通走査の受理判定と宣言の子列の照会数の取得関数
	 * 計算量：O(1)
	 * @param Src 観測する解析結果
	 * @return 受理の訪問数・文の改行判定の葉数・宣言の子列の照会数・子列の照会を省いた葉数・改行判定の葉の訪問数
	 */
	static std::array<size_t, 5> GetAdmissionMetrics(const ParsedTree &Src) {
		// 同じ世代の計数の返戻
		return {
			Src.Syntax.Relations->AdmissionWork,
			Src.Syntax.Relations->AdmissionLeaves.size(),
			Src.Syntax.Relations->NameChildQueries,
			Src.Syntax.Relations->NameLeafSkips,
			Src.Syntax.Relations->AdmissionLeafWork
		};
	}

	/**
	 * 本体のインデントを読む入力側の作業量の取得関数
	 * 計算量：O(1)
	 * @param Src 観測する解析結果
	 * @return 原文の隙間と本体の深さの入力受理での読取量
	 */
	static size_t GetIndentationWork(const ParsedTree &Src) {
		// 中核の予算へ混入させない入力側の計数の返戻
		return Src.Syntax.Relations->IndentationWork;
	}

	/**
	 * 親子表に共有した修復候補とフィールドの作業量の取得関数
	 * 計算量：O(1)
	 * @param Src 観測する解析結果
	 * @return 親子の読取数・修復候補の訪問数・候補数・フィールド照会数・フィールドの種類数
	 * @throws std::runtime_error 同じ訪問で記号を重複して照会した場合
	 */
	static std::array<size_t, 5> GetRelationMetrics(const ParsedTree &Src) {
		// 未構築の表の観測値の返戻
		if(!Src.Syntax.HasParents) return {};
		const ParsedTree::NodeRelations &Relations = *Src.Syntax.Relations;
		Check(Relations.SymbolReads == Relations.ReadWork, "relation collection repeated a child symbol query");
		Check(Relations.Children.size() == Relations.ReadWork, "relation collection left an unwritten child slot");
		// 世代の１度の収集と候補の判定の観測値の返戻
		return {
			Relations.ReadWork,
			Relations.DeclWork,
			Relations.DeclCandidates.size(),
			Relations.FieldWork,
			Relations.SemanticFields.size()
		};
	}

	/**
	 * 候補が無い木では専用の適応走査を省く事の検査関数
	 * 計算量：O(1)
	 * @param Src 観測する解析結果
	 * @throws std::runtime_error 不要な走査を繰り返した場合
	 */
	static void CheckUnusedAdaptation(const ParsedTree &Src) {
		// 親子表を要しない言語の終了
		if(!Src.Syntax.HasParents) return;
		const ParsedTree::NodeRelations &Relations = *Src.Syntax.Relations;
		Check(Relations.HasMacroCandidates || !Relations.MacroWork, "macro-free code repeated a state scan");
		Check(Relations.HasBoolCandidates || !Relations.BoolWork, "code without boolean names repeated a shadow scan");
		Check(Relations.HasJsxCandidates || !Relations.JsxWork, "code without JSX repeated a text scan");
		// 終了
		return;
	}

	/**
	 * 補完済のフィールドを再照会しても木の探索を繰り返さない事の検査関数
	 * 計算量：直接子と其の子の総数 C，フィールド数 F，記録済の種類数 K，補完費用 Q に対し O(C + F log(K + 1) + Q)
	 * @param Src 観測する解析結果
	 * @param Node 親ノード
	 * @param Expected 親子表で確かめた本文のノード
	 * @throws std::runtime_error フィールド又は再照会の回数が異なる場合
	 * @throws std::bad_alloc 補完前の共有表を確保出来ない場合
	 * @throws std::length_error 表の要素数が上限を超える場合
	 */
	static void CheckFieldReads(const ParsedTree &Src, const TSNode Node, const TSNode Expected) {
		Src.PrepareChildFields(Node);
		const std::span<const ParsedChild> Children = Src.GetChildrenOf(Node);
		for(const ParsedChild &Child : Children) {
			const std::span<const ParsedChild> Direct = Src.GetChildrenAt(Child.Relation), Named = Src.GetChildrenOf(Child.Node);
			Check(Direct.size() == Named.size(), "direct child range changed its length");
			for(size_t Idx = 0; Idx < Direct.size(); ++Idx) {
				Check(ts_node_eq(Direct[Idx].Node, Named[Idx].Node), "direct child range changed its order");
			}
		}
		Check(
			Src.Syntax.Relations->Parents.Find(Node.id)->HasInnerFields == std::ranges::any_of(
				Children,
				[](const ParsedChild &Child) -> bool {
					// 最内のフィールドを持つ子かの返戻
					return Child.Field;
				}
			),
			"field presence changed when shared with the cursor"
		);
		// 最内と外側の双方を含む最初の子の解析文脈を，パーサーの独立した照会と比較する
		const ParsedTree::NodeRelation &Parent = *Src.Syntax.Relations->Parents.Find(Node.id);
		for(
			const ParsedTree::FieldChild &Field :
			std::span<const ParsedTree::FieldChild>(Src.Syntax.Relations->FirstFields).subspan(Parent.FieldsBegin, Parent.FieldsCount)
		) {
			const TSNode ExpectedField = ts_node_child_by_field_id(Node, Field.Field);
			const TSNode ActualField = Field.IsStored ? Src.GetNodeAt(Field.Index).Node : Src.Syntax.Relations->FieldNodes[Field.Index];
			Check(
				ExpectedField.id == ActualField.id && ExpectedField.tree == ActualField.tree &&
				std::ranges::equal(ExpectedField.context, ActualField.context),
				"completed fields changed their parser context"
			);
		}
		// 葉のフィールドは必ず空であり，パーサーのフィールド探索を要しない
		for(const ParsedChild &Child : Children) if(!ts_node_child_count(Child.Node)) {
			size_t LeafQueries = 0;
			Check(ts_node_is_null(Src.GetFieldChild(Child.Node, Core::ChildRole::Body, &LeafQueries)), "a leaf acquired a body field");
			Check(!LeafQueries, "leaf field reads called the parser");
		}
		const size_t Before = Src.Syntax.Relations->FieldWork;
		size_t Queries = 0;
		Src.PrepareChildFields(Node);
		Check(
			ts_node_eq(Src.GetFieldChild(Node, Core::ChildRole::Body, &Queries), Expected),
			"cached fields changed their first child"
		);
		Check(!Queries && Src.Syntax.Relations->FieldWork == Before, "completed fields repeated a parser query");
		// 終了
		return;
	}

	/**
	 * コメントの共通走査で訪ねたノード数の取得関数
	 * 計算量：O(1)
	 * @param Src 観測する解析結果
	 * @return 此の木の世代で訪ねたノード数
	 * @throws std::runtime_error コメントの表が世代の外のメモリを使う場合
	 */
	static size_t GetCommentVisits(const ParsedTree &Src) {
		// 未収集の作業量の返戻
		if(!Src.Syntax.HasCommentNodes) return 0;
		const ParsedTree::CommentFacts &Facts = *Src.Syntax.Comments;
		Check(
			Facts.Ranges.get_allocator().resource() == &Facts.Storage && Facts.Nodes.get_allocator().resource() == &Facts.Storage &&
			Facts.Parents.get_allocator().resource() == &Facts.Storage && Facts.Following.get_allocator().resource() == &Facts.Storage,
			"comment facts allocated outside their generation"
		);
		// 製品の共通走査が計上した値の返戻
		return Facts.Visits;
	}

	/**
	 * ノードに為らない隙間のコメントを収集したかの判定関数
	 * 計算量：収集済のコメント範囲数 C に対し O(C)
	 * @param Src 観測する解析結果
	 * @return 隙間からのコメント範囲を持てば `true`
	 */
	static bool HasGapComment(const ParsedTree &Src) {
		// 製品の収集結果の出所の返戻
		return Src.Syntax.HasCommentNodes && std::ranges::any_of(
			Src.Syntax.Comments->Ranges,
			[](const ParsedTree::CommentFacts::Range &Range) -> bool {
				// 隙間を出所とするかの返戻
				return Range.IsGap;
			}
		);
	}

	/**
	 * 共有木の並列の名前照会が遅延の表と計数を変更しない事の検査関数
	 * 計算量：固定した照会数 K と導入経路の構築費用 Q に対し O(K + Q)
	 * @param Src 対象の木
	 * @param Use 型と値の使用位置
	 * @throws std::runtime_error 束縛又は読取専用の契約が異なる場合
	 * @throws std::bad_alloc 名前の表又は結果を確保出来ない場合
	 * @throws std::length_error 名前又は経路の要素数が上限を超える場合
	 * @throws std::system_error 作業スレッドの合流に失敗した場合
	 */
	static void CheckParallelNameReads(const ParsedTree &Src, const uint32_t Use) {
		// 製品と同じ事前構築と計数の停止を通す
		Src.GetRoot();
		std::vector<uint8_t> Results(64);
		{
			const ParsedTree::NameReadGuard Guard(Src);
			const size_t Before = Src.GetNameWork();
			Parallel::ForChunks(
				Results.size(),
				2,
				[&](const size_t Begin, const size_t End, size_t) -> void {
					// 各スレッドは其の結果の区間だけを書き換える
					for(size_t Idx = Begin; Idx < End; ++Idx) {
						Results[Idx] = Src.IsDeclaredTypeName("T", Use) && Src.IsDeclaredName("V", Use) && Src.IsNonMacroName("V", Use) &&
						!Src.IsNonMacroName("KEEP", Use);
					}
					// 終了
					return;
				}
			);
			Check(Src.GetNameWork() == Before, "parallel binding queries changed shared work");
			for(const ParsedTree::NamespaceBindings &Owner : Src.NameData->Bindings) {
				Check(Owner.HasRoutes && Owner.PendingRoutes.empty(), "parallel binding queries left an unbuilt route");
			}
		}
		Check(!Src.IsReadingNamesInParallel, "parallel binding queries did not restore serial counting");
		Check(
			std::ranges::all_of(
				Results,
				[](const uint8_t Result) -> bool {
					return Result;
				}
			),
			"parallel bindings differ"
		);
		const size_t Before = Src.GetNameWork();
		Check(Src.IsDeclaredTypeName("T", Use) && Src.GetNameWork() > Before, "serial binding work remained disabled");
		// 終了
		return;
	}

	/**
	 * 修復と名前の探索の累積予算の境界の検査関数
	 * 計算量：入力長 B と投影費用 Q に対し O(B + Q)
	 * @param Profile 読取と投影の文法
	 * @param Text 正常に受理する修復の入力
	 * @throws FormatLimitExceeded 正常な入力が予算を超える場合
	 * @throws std::bad_alloc 入力又は投影のメモリを確保出来ない場合
	 * @throws std::runtime_error 作業量又は境界の扱いが仕様と異なる場合
	 */
	static void CheckInputWorkBudget(const GrammarProfile &Profile, const std::string &Text) {
		InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, Text);
		ParseInput Input = ParseAdapter::Adapt(Profile, Normalized);
		ParsedTree Src = SyntaxParser::Parse(*Input.Profile, Input);
		const Core::ProjectionResult Original = Core::SourceProjector::Project(Src, *Input.Profile, Input);
		Check(Src.GetNameWork() && Src.GetCastRepairWork(), "the work-budget fixture lost its input work");
		const size_t Limit = Original.Syntax.GetWorkLimit();
		// 中核の上限丁度は受理し，２種類の計上の合計が１単位超える時に見送る
		Src.NameWork.Used = Limit - Src.CastRepairWork.Used;
		const Core::ProjectionResult Boundary = Core::SourceProjector::Project(Src, *Input.Profile, Input);
		Check(Boundary.Syntax.GetWorkAccount()->Used == Limit, "input work at the budget boundary was rejected or lost");
		Src.NameWork.Charge(1);
		bool IsRejected = false;
		try {
			Core::SourceProjector::Project(Src, *Input.Profile, Input);
		} catch(const FormatLimitExceeded &E) {
			IsRejected = E.Kind == FailureReason::LimitExceeded && E.Reason == "structured text exceeds formatting limit";
		}
		Check(IsRejected, "combined input work beyond the budget was accepted");
		// 終了
		return;
	}
};

// 本文の文境界判定と所属の読取を，構文の予算とは独立して観測する入口
struct CommentNormalizerAccess {
	/**
	 * グループの所属を照会した記録数の取得関数
	 * 計算量：記録数 C に対し O(C log(C + 1))
	 * @param Records 原文順を指定した記録
	 * @param Work 所属を照会した記録数の格納先
	 * @return 原文順に並んだグループ
	 * @throws std::bad_alloc 所属のキーとグループのメモリを確保出来ない場合
	 * @throws std::length_error 記録の列の要素数が上限を超える場合
	 */
	static std::vector<std::vector<size_t>> GetGroups(const std::span<const CommentRecord> Records, size_t &Work) {
		// 製品の所属の読取と整列の結果の返戻
		return CommentNormalizer::GetGroupsOf(ReadOnlyCommentView(Records), &Work);
	}

	/**
	 * 文末記号の判定と除去の作業量の取得関数
	 * 計算量：本文長 N に対し O(N)
	 * @param Text 正規化前の本文
	 * @param Work 本文と文末記号の訪問回数の格納先
	 * @param EndingBytes 括弧・終了タグの後続判定の表のバイト数（無ければ計上しない）
	 * @return 文区切・句点を処理した本文
	 */
	static std::string Split(std::string Text, size_t &Work, size_t *const EndingBytes = nullptr) {
		std::vector<unsigned char> Protected(Text.size(), 0);
		const std::vector<CommentNormalizer::SentenceBoundary> Boundaries =
		CommentNormalizer::FindSentenceBoundaries(Text, Protected, true, &Work, EndingBytes);
		CommentNormalizer::SplitSentences(Text, Boundaries, {}, {}, Protected);
		// 製品と同じ転写結果の返戻
		return Text;
	}

	/**
	 * UTF-8 と読点候補の共通走査の作業量の取得関数
	 * 計算量：本文長 B に対し O(B)
	 * @param Text 正規化前の本文
	 * @param HasCommas 読点候補の有無の手期待
	 * @param IsValidUtf8 UTF-8 の成立の手期待
	 * @return デコードした文字数
	 * @throws std::runtime_error 本文の事実が手期待と異なる場合
	 */
	static size_t GetBodyFactWork(const std::string_view Text, const bool HasCommas, const bool IsValidUtf8) {
		size_t Work = 0;
		const CommentNormalizer::BodyFacts Facts = CommentNormalizer::GetBodyFacts(Text, &Work);
		Check(Facts.HasCommas == HasCommas && Facts.IsValidUtf8 == IsValidUtf8, "body facts differ from the specified characters");
		// 共通走査が計上した文字数の返戻
		return Work;
	}

	/**
	 * 空白の正規化で本文を転写した作業量の取得関数
	 * 計算量：本文長 B と期待長 E に対し O(B + E)
	 * @param Rules 記法と本文の判定条件
	 * @param Text 正規化前の本文
	 * @param Expected 空白だけを整理した手期待
	 * @param IsProtected 本文全体をコード例として保護するか
	 * @return 本文を転写したバイト数
	 * @throws std::runtime_error 本文又は保護標識が手期待と異なる場合
	 * @throws std::bad_alloc 本文又は保護標識を確保出来ない場合
	 * @throws std::length_error 本文又は保護標識の要素数が上限を超える場合
	 */
	static size_t GetSpaceWork(
		const Core::NotationRules &Rules,
		std::string Text,
		const std::string_view Expected,
		const bool IsProtected
	) {
		std::vector<unsigned char> Protected(Text.size(), IsProtected);
		size_t CopiedBytes = 0;
		CommentNormalizer::NormalizeSpaces(Rules.OutputForms.Conditions, Text, Rules.CodeExamples.TagMarks, Protected, &CopiedBytes);
		Check(Text == Expected && Protected.size() == Text.size(), "spacing changed the specified text or mask size");
		if(IsProtected) {
			Check(
				std::ranges::all_of(
					Protected,
					[](const unsigned char Byte) -> bool {
						// 全体の保護が保持されたかの返戻
						return Byte == 1;
					}
				),
				"spacing lost the protection of a code example"
			);
		}
		// 本文の転写量の返戻
		return CopiedBytes;
	}

	/**
	 * 行端の除去で本文中の制御文字を保つ事と，読取・移動の作業量の検査関数
	 * 計算量：本文長 B と期待長 E に対し O(B + E)
	 * @param Text 整理前の論理行
	 * @param Expected 行端だけを除いた手期待
	 * @return 製品の行端整理が計上した作業量
	 * @throws std::runtime_error 本文又は制御文字が変わった場合
	 * @throws std::bad_alloc 保護標識を確保出来ない場合
	 * @throws std::length_error 保護標識の要素数が上限を超える場合
	 */
	static size_t GetEdgeWork(std::string Text, const std::string_view Expected) {
		const std::vector<unsigned char> Protected(Text.size(), 0);
		size_t Work = 0;
		CommentNormalizer::FinishBody(Text, Protected, {}, false, &Work);
		Check(Text == Expected, "edge compaction changed interior control characters");
		// 読取・移動の計数の返戻
		return Work;
	}

	/**
	 * コードスパンの無い本文で対応付を省き，有る本文は同じ範囲を保護する事の検査関数
	 * 計算量：本文長 B と期待長 E に対し O(B + E)
	 * @param Forms 文法のコード例の書式
	 * @param Text バッククォートの有無を指定した本文
	 * @param Expected 手で指定した保護標識
	 * @return バッククォートの検出と対応付の作業量
	 * @throws std::runtime_error 保護する範囲が変わった場合
	 * @throws std::bad_alloc 本文構造を確保出来ない場合
	 * @throws std::length_error 本文構造の要素数が上限を超える場合
	 */
	static size_t GetTickWork(
		const Core::CodeExampleForms &Forms,
		const std::string_view Text,
		const std::vector<unsigned char> &Expected
	) {
		bool IsUnclosed = false;
		size_t Work = 0;
		Check(
			CommentNormalizer::GetCodeMask(Forms, Text, IsUnclosed, &Work) == Expected && !IsUnclosed,
			"code span correspondence changed the handwritten mask"
		);
		// 対応付の計数の返戻
		return Work;
	}
};

namespace Core {
	// 製品の構築経路のフレームの再使用を観測する入口
	struct CanonicalizerAccess {
		/**
		 * フレームの要求数と新規確保数を観測する構築関数
		 * 計算量：製品の構築費用 Q に対し O(Q)
		 * @param Syntax 正規化した読取専用の期待構文
		 * @param Work フレームの要求数と新規確保数の格納先
		 * @return 製品の中間表現
		 * @throws FormatLimitExceeded 構築又は出力がリソース上限を超える場合
		 * @throws std::bad_alloc 構築のメモリを確保出来ない場合
		 * @throws std::length_error 構築の要素数が上限を超える場合
		 * @throws std::out_of_range 構築のノード又は要素が表に無い場合
		 */
		static FormatIR BuildWithWork(std::shared_ptr<const SyntaxModel> Syntax, std::pair<size_t, size_t> &Work) {
			// 製品と同じ構築結果の返戻
			return Canonicalizer::BuildWithWork(std::move(Syntax), &Work);
		}

		/**
		 * 同じ２文本体の文数と存在判定が共有される事の照合関数
		 * 計算量：ノード数 N と本体の直接の子数の和 C に対し O(N + C)
		 * @param Syntax 読取専用の構文
		 * @return 初回と後続の参照の作業量と照合した本体数
		 * @throws FormatLimitExceeded 計数又は参照が作業予算を超える場合
		 * @throws std::runtime_error 文数・存在判定又は再利用の作業量が異なる場合
		 * @throws std::bad_alloc 構築の控えを確保出来ない場合
		 * @throws std::length_error 控えの件数が上限を超える場合
		 * @throws std::out_of_range 構文又は要約の参照先が表に無い場合
		 */
		static std::array<size_t, 3> GetBodySummaryWork(const SyntaxModel &Syntax) {
			// 共通走査の完了後，異なる呼出元が同じ本体を読む場合の計数を照合する
			FormatIR IR;
			Canonicalizer::Builder State { Syntax, IR };
			Canonicalizer::PrepareContexts(State);
			std::array<size_t, 3> Work {};
			for(NodeId Id = 0; Id < Syntax.GetNodeCount(); ++Id) {
				if(
					!Syntax.Is(Id, Role::ClosureStmts) &&
					!(Syntax.Is(Id, Role::ValStmtSeq) && Syntax.Is(Syntax.GetParentOf(Id), Role::LambdaLiteral))
				) continue;
				const size_t Before = Syntax.GetWorkAccount()->Used;
				Check(Canonicalizer::GetBodyStmtCount(State, Id) == 2, "two-statement body lost its saturated count");
				const size_t Counted = Syntax.GetWorkAccount()->Used;
				Check(Canonicalizer::GetBodyStmtCount(State, Id) == 2, "shared body count changed");
				Check(Canonicalizer::HasBodyStmt(State, Id), "shared body existence changed");
				Work[0] += Counted - Before;
				Work[1] += Syntax.GetWorkAccount()->Used - Counted;
				++Work[2];
			}
			Check(Work[2] && Work[0] == Work[2] && Work[1] == Work[2] << 1, "body references rescanned their children");
			// 観測した作業量の返戻
			return Work;
		}

		/**
		 * 宣言の分割の意味保存と文脈の共有の観測関数
		 * 計算量：文脈のノード数 N と宣言の部品数 D に対し O(N + D)
		 * @param Syntax 読取専用の構文
		 * @param IsDependent 並列の初期化が前の名前へ依存するか
		 * @return 文脈の準備と分割条件の判定の作業量
		 * @throws FormatLimitExceeded 構築の作業が予算を超える場合
		 * @throws std::runtime_error 束縛を変える分割を許可するか，対象外の宣言を併記と誤る場合
		 * @throws std::bad_alloc 文脈又は宣言の部品を確保出来ない場合
		 * @throws std::length_error 作業列が最大長を超える場合
		 * @throws std::out_of_range 宣言又は文脈の参照先が表に無い場合
		 */
		static size_t GetDeclarationSplitWork(const SyntaxModel &Syntax, const bool IsDependent) {
			FormatIR IR;
			Canonicalizer::Builder State { Syntax, IR };
			const size_t Before = Syntax.GetWorkAccount()->Used;
			Canonicalizer::PrepareContexts(State);
			size_t Count = 0;
			for(NodeId Id = 0; Id < Syntax.GetNodeCount(); ++Id) {
				if(
					!Syntax.Is(Id, Role::CommaDecl) || !Syntax.Is(Syntax.GetParentOf(Id), Role::StmtHost) ||
					!Canonicalizer::HasMultipleDeclarators(Syntax, Id)
				) continue;
				const std::optional<CommaDecl> Shape = Syntax.GetCommaDeclOf(Id, false);
				Check(Shape.has_value(), "parallel declaration lost its shape");
				Check(Canonicalizer::CanSplitDecl(Syntax, *Shape) != IsDependent, "declaration splitting changed binding starts");
				++Count;
			}
			Check(Count, "declaration splitting fixture had no parallel declaration");
			// 判定の作業量の返戻
			return Syntax.GetWorkAccount()->Used - Before;
		}

		/**
		 * 文脈の共有と空文の限定した照会の観測関数
		 * 計算量：文脈のノード・直接の子数 N と空文候補のノード数 E に対し O(N + E)
		 * @param Syntax 読取専用の構文
		 * @return 文脈の準備と空文候補の照会の作業量
		 * @throws FormatLimitExceeded 構文の照会が予算を超える場合
		 * @throws std::runtime_error セミコロンだけの空文を誤判定する場合
		 * @throws std::bad_alloc 文脈又は走査枠を確保出来ない場合
		 * @throws std::length_error 作業列が最大長を超える場合
		 * @throws std::out_of_range 文脈の参照先が表に無い場合
		 */
		static size_t GetLimitedContextWork(const SyntaxModel &Syntax) {
			// 文脈の準備と空文候補の照会の作業量を観測する
			FormatIR IR;
			Canonicalizer::Builder State { Syntax, IR };
			const size_t Before = Syntax.GetWorkAccount()->Used;
			Canonicalizer::PrepareContexts(State);
			size_t ExpectedWork = 0;
			for(NodeId Id = 0; Id < Syntax.GetNodeCount(); ++Id) {
				if(!Syntax.Is(Id, Role::ExprStmt)) continue;
				const size_t BeforeText = Syntax.GetWorkAccount()->Used;
				const std::string Text = Syntax.GetText(Id);
				ExpectedWork += Syntax.GetWorkAccount()->Used - BeforeText;
				Check(Canonicalizer::IsEmptyStmt(Syntax, Id) == (Text == ";"), "limited empty statement lookup changed its meaning");
			}
			// 手期待の読取を除いた，準備と限定照会の作業量の返戻
			return Syntax.GetWorkAccount()->Used - Before - ExpectedWork;
		}
	};

	// 製品のコメント処理を，構文の予算とは独立して観測する入口
	struct SourceProjectorAccess {
		/**
		 * 分離しないコメントを，投影の既存の訪問で拒否するかの判定関数
		 * 計算量：失敗するノード迄の投影費用 Q に対し O(Q)
		 * @param Src 解析済の原文
		 * @param Input 原文の入力条件
		 * @return 分離経路の違反と同じ失敗理由なら true
		 */
		static bool IsUnseparatedRejected(const ParsedTree &Src, const ParseInput &Input) {
			SourceProjector::CommentSeparation Separated;
			try {
				SourceProjector::ProjectNodes(Src, *Input.Profile, Input, Input.Owned, Separated);
			} catch(const FormatLimitExceeded &E) {
				// 経路の分離違反の分類と理由の返戻
				return E.Kind == FailureReason::FormatterDefect && E.Reason == "comment nodes reach the core passes";
			}
			// 分離漏を拒否しなかった事の返戻
			return false;
		}

		/**
		 * 埋込ドキュメントのマーカー除去の作業量の取得関数
		 * 計算量：本文のバイト数 B に対し O(B)
		 * @param Text マーカーを挟んだ本文（除去結果の格納先）
		 * @param Marker 正規の入力経路で選んだ解析用の制御文字
		 * @return 製品のマーカー除去が行ったバイトの読取・複写の計数
		 * @throws FormatLimitExceeded 観測用計数の上限を超える場合
		 * @throws std::bad_alloc 照合する終端記号のメモリを確保出来ない場合
		 * @throws std::length_error 照合する終端記号の長さが文字列の上限を超える場合
		 */
		static size_t GetRestoreWork(std::string &Text, const unsigned char Marker) {
			StructureWorkAccount Work;
			SourceProjector::RestoreEmbeddedDoc(Text, Marker, &Work);
			// 製品の経路が計上した作業量の返戻
			return Work.Used;
		}

		/**
		 * コメントの分離とディレクティブ収集の作業量の取得関数
		 * 計算量：製品の分離・収集の費用 Q に対し O(Q)
		 * @param Src 解析済の原文又は完成候補
		 * @param Input 原文又は検証用の入力の条件
		 * @param Observation 隣接表・最後の子の読取と統合用の移動の観測先（無ければ観測しない）
		 * @return 共通走査を除いた分離・収集のノードと記録の観測値
		 * @throws FormatLimitExceeded 観測用計数の上限を超える場合
		 * @throws std::bad_alloc コメント又は位置表のメモリを確保出来ない場合
		 * @throws std::length_error コメント又は位置表の要素数上限を超える場合
		 */
		static size_t GetCommentWork(
			const ParsedTree &Src,
			const ParseInput &Input,
			std::array<size_t, 3> *const Observation = nullptr
		) {
			StructureWorkAccount Work;
			const SourceProjector::CommentSeparation Separated = SourceProjector::SeparateComments(Src, Input, &Work, Observation);
			SourceProjector::CollectDirectives(Src, Separated.Head, nullptr, &Work);
			// 製品の経路が計上した作業量の返戻
			return Work.Used;
		}

		/**
		 * 既存の投影が記録したドキュメントの本文の所有量の検査関数
		 * 計算量：記録数 C と本文の総長 B に対し O(C + B)
		 * @param Records 記録の並び（全て説明対象を持つテストケース）
		 * @param Facts 同じ投影で確定したドキュメントの記録（所有者を引き取る）
		 * @return 本文のコピー量と論理行の範囲数
		 * @throws std::runtime_error 本文の範囲又は所有者の移動が異なる場合
		 */
		static std::pair<size_t, size_t> GetDocStorage(
			const std::span<const CommentRecord> Records,
			std::vector<ValidationContext::DocFact> Facts
		) {
			Check(Facts.size() == Records.size(), "document storage lost a described record");
			size_t Bytes = 0, Lines = 0, TargetBytes = 0;
			// 所有者を移動してもビューは同じ共有の本文を指し，記録自身の内部領域に依存しない
			const std::vector<ValidationContext::DocFact> Moved = std::move(Facts);
			const ValidationContext::DocStorage *const Owner = Moved.empty() ? nullptr : Moved.front().Owner.get();
			for(size_t Idx = 0; Idx < Moved.size(); ++Idx) {
				const ValidationContext::DocFact &Fact = Moved[Idx];
				Check(Fact.Owner.get() == Owner, "document facts allocated separate owners");
				Check(Fact.Text == CommentRepository::GetOutputTextOf(Records[Idx]), "document body changed after moving its owner");
				Bytes += Fact.Text.size();
				TargetBytes += Fact.Target.size();
				Lines += Fact.Lines.size();
				for(const std::pair<size_t, size_t> &Line : Fact.Lines) {
					Check(Line.first <= Fact.Text.size() && Line.second <= Fact.Text.size() - Line.first, "document line range outlived its body");
				}
			}
			Check(
				!Owner || (Bytes == Owner->Bodies.size() && Lines == Owner->Lines.size() && TargetBytes == Owner->Targets.size()),
				"document facts duplicated shared buffers"
			);
			// 本文の所有量と範囲数の返戻
			return { Bytes, Lines };
		}

		/**
		 * 分離後の断片の複写量の取得関数
		 * 計算量：分離・合成の費用 Q と期待の長さ E に対し O(Q + E)
		 * @param Src 解析結果
		 * @param Input 解析用の入力
		 * @param From 対象範囲の始まり
		 * @param To 対象範囲の終わり
		 * @param Expected 分離済のテキストの手期待
		 * @return 断片の一時コピーと合成先への複写のバイト数
		 * @throws std::runtime_error 分離後のテキストが手期待と異なる場合
		 * @throws FormatLimitExceeded 分離に要る構造の照会が予算を超える場合
		 * @throws std::bad_alloc 分離・合成に要るメモリを確保出来ない場合
		 * @throws std::length_error 分離・合成する列・テキストが上限を超える場合
		 */
		static size_t GetDecodedWork(
			const ParsedTree &Src,
			const ParseInput &Input,
			const uint32_t From,
			const uint32_t To,
			const std::string_view Expected
		) {
			const SourceProjector::CommentSeparation Separated = SourceProjector::SeparateComments(Src, Input);
			size_t Work = 0;
			Check(Separated.GetDecodedText(Src, From, To, &Work) == Expected, "direct fragment composition changed source text");
			// 製品の複写量の返戻
			return Work;
		}

		/**
		 * マーカーと復元データの対象外の断片の複写量の検査関数
		 * 計算量：断片長 B と復元数 R に対し O(B + log(R + 1))
		 * @param Src マーカー又は復元データを持つ解析結果
		 * @param Begin 対象外の断片の開始
		 * @param Expected 断片の手期待
		 * @throws std::runtime_error 対象外の断片を複写し直すか，テキストが異なる場合
		 * @throws std::bad_alloc 断片のメモリを確保出来ない場合
		 * @throws std::length_error 断片長が格納先の上限を超える場合
		 */
		static void CheckUnaffectedFragment(const ParsedTree &Src, const uint32_t Begin, const std::string_view Expected) {
			const SourceProjector::CommentSeparation Empty;
			size_t Work = 0;
			Check(
				Empty.GetDecodedText(Src, Begin, Begin + static_cast<uint32_t>(Expected.size()), &Work) == Expected,
				"an unrelated analysis marker changed a fragment"
			);
			Check(Work == Expected.size(), "an unaffected fragment made a temporary decoded copy");
			// 終了
			return;
		}

		/**
		 * 構文事実の準備・記録の訪問回数を観測する投影関数
		 * 計算量：製品の投影の総費用 Q に対し O(Q)
		 * @param Src 解析済の原文又は完成候補
		 * @param Profile 入力で確定した文法
		 * @param Input 適応済の入力と修復前のテキスト
		 * @param Work 投影の既存の訪問と事実の準備・記録のノード訪問回数の格納先
		 * @param Hidden 隠れたトークンの分割バイト数・照会数・分割数の格納先（無ければ観測しない）
		 * @param Facts 真偽値名の照会数と最初のトークンの継承で読んだ子の数の格納先（無ければ観測しない）
		 * @param CtxVisits 投影後に破損を分類するノードの訪問数の格納先（無ければ観測しない）
		 * @param ProjectionQueries 実引数の子の読取・区切記法の合成・読違と隣接条件の照会の回数の格納先（無ければ観測しない）
		 * @param DocQueries 名前取得・種別の表示・表示の照会・論理行の観測先（無ければ観測しない）
		 * @param GapSearches 短い空白の照会数とトークン表の検索数の格納先（無ければ観測しない）
		 * @return 製品のノード表・コメント・検証データ
		 * @throws FormatLimitExceeded 投影又は観測用計数の上限を超える場合
		 * @throws std::bad_alloc 投影のメモリを確保出来ない場合
		 * @throws std::invalid_argument 投影の引数が契約を満たさない場合
		 * @throws std::length_error テキスト又は作業列の要素数上限を超える場合
		 * @throws std::out_of_range 照会する位置又はノードが範囲外の場合
		 */
		static ProjectionResult ProjectWithWork(
			const ParsedTree &Src,
			const GrammarProfile &Profile,
			const ParseInput &Input,
			size_t &Work,
			std::array<size_t, 3> *const Hidden = nullptr,
			std::array<size_t, 2> *const Facts = nullptr,
			size_t *const CtxVisits = nullptr,
			std::array<size_t, 4> *const ProjectionQueries = nullptr,
			std::array<size_t, 4> *const DocQueries = nullptr,
			std::array<size_t, 2> *const GapSearches = nullptr
		) {
			// 製品と同じ分離結果を渡し，正規化前の事実の走査だけを計数
			SourceProjector::CommentSeparation Separated = SourceProjector::SeparateComments(Src, Input);
			StructureWorkAccount Account;
			SourceProjector::HiddenWork Observation;
			ProjectionResult Result = SourceProjector::ProjectNodes(
				Src,
				Profile,
				Input,
				Input.Owned,
				Separated,
				true,
				&Account,
				Hidden || Facts || CtxVisits || ProjectionQueries || DocQueries || GapSearches ? &Observation : nullptr
			);
			Work = Account.Used;
			if(Hidden) *Hidden = { Observation.Bytes, Observation.Queries, Observation.Splits };
			if(Facts) *Facts = { Observation.BoolQueries, Observation.EdgeReads };
			if(CtxVisits) *CtxVisits = Observation.CtxVisits;
			if(DocQueries) *DocQueries = Observation.Docs;
			if(GapSearches) *GapSearches = { Observation.SpaceReads, Observation.TokenSearches };
			if(ProjectionQueries) {
				*ProjectionQueries =
				{ Observation.ArgTextReads, Observation.KeptCopies, Observation.ReadingQueries, Observation.AdjacencyQueries };
			}
			// 公開の投影と同じく，検証データのビューを入力の寿命から独立させる
			SourceProjector::RetainValidationInput(Profile, Result);
			// 観測した投影結果の返戻
			return Result;
		}
	};

	// 所属表の実際の訪問回数を観測する入口
	struct DocCommentReaderAccess {
		/**
		 * 宣言呼出のドキュメントの説明対象数の取得関数
		 * 計算量：所属表の構築費用 Q，ノード数 N と対象の照会費用 A に対し O(Q + N + A)
		 * @param Syntax 説明対象の構文
		 * @return アンカー自身が説明対象である宣言呼出の数
		 * @throws FormatLimitExceeded 所属表又は説明対象の照会が作業予算を超える場合
		 * @throws std::bad_alloc 所属表又は対象のメモリを確保出来ない場合
		 * @throws std::length_error 所属表又は対象の要素数上限を超える場合
		 * @throws std::out_of_range 説明対象のノードが表に無い場合
		 */
		static size_t GetDeclarationCallCount(const SyntaxModel &Syntax) {
			const DocTargetTables Tables = DocCommentReader::TablesOf(Syntax);
			size_t Count = 0;
			for(NodeId Node = 0; Node < Syntax.GetNodeCount(); ++Node) {
				if(Syntax.Is(Node, Role::MemberDeclCall) && DocCommentReader::GetDocTargetOf(Syntax, Tables, Node) == Node) ++Count;
			}
			// 宣言の認識経路で得た説明対象数の返戻
			return Count;
		}

		/**
		 * 説明対象の共有表の構築の作業量の取得関数
		 * 計算量：表の構築費用 Q に対し O(Q)
		 * @param Syntax 説明対象の構文
		 * @return 表を構築するノードの訪問回数
		 */
		static size_t GetWork(const SyntaxModel &Syntax) {
			size_t Work = 0;
			DocCommentReader::TablesOf(Syntax, &Work);
			// 製品の表の構築の訪問回数の返戻
			return Work;
		}

		/**
		 * ドキュメントの論理行の共有範囲の作業量の取得関数
		 * 計算量：本文長 B と期待長 E に対し O(B + E)
		 * @param Record 本文を持つドキュメントの記録
		 * @param Expected 記法に従う論理行範囲の手期待
		 * @param Begin 元の本文での開始位置の手期待
		 * @return 訪問した論理行のバイト数と区切の和
		 * @throws std::runtime_error 論理行の範囲又は本文の共有が手期待と異なる場合
		 */
		static size_t GetLogicalTextWork(const CommentRecord &Record, const std::string_view Expected, const size_t Begin) {
			size_t Work = 0;
			const std::string_view Text = DocCommentReader::GetLogicalText(Record, &Work);
			const std::string_view Body = CommentRepository::GetOutputTextOf(Record);
			Check(Text == Expected && Text.data() == Body.data() + Begin, "logical text is not the specified shared body range");
			// 本文の範囲の走査量の返戻
			return Work;
		}
	};

	// 埋込ドキュメントの製品の分離経路と，共有本文の読取・複写の作業計数の観測
	struct DocCommentSynchronizerAccess {
		/**
		 * 埋込ドキュメントの分離の作業量の取得関数
		 * 計算量：本文構造の確定と製品の分離の総費用 Q に対し O(Q)
		 * @param Comments 正規の投影が分解した埋込ドキュメント
		 * @return 製品の分離経路の読取・複写の作業計数
		 * @throws FormatLimitExceeded 記法の定義が無い場合
		 * @throws std::bad_alloc 本文構造又は分離片のメモリを確保出来ない場合
		 * @throws std::length_error 本文構造又は分離片の要素数が格納先の上限を超える場合
		 */
		static size_t GetSplitWork(CommentRepository &Comments) {
			Comments.SetStructure(CommentNormalizer::Inspect(ReadOnlyCommentView(Comments.GetRecords())));
			StructureWorkAccount Work;
			DocCommentSynchronizer::SplitEmbedded(Comments, &Work);
			// 製品の経路が計上した作業量の返戻
			return Work.Used;
		}
	};

	// 製品の候補抽出を変更せず，検証ツールだけが構文の照会数を観測する入口
	struct CommentPlacerAccess {
		/**
		 * 配置候補の構文の照会数の取得関数
		 * 計算量：製品の候補抽出の費用 Q に対し O(Q)
		 * @param Code 確定したコードの配置
		 * @param Comments 本文構造を共有する正規化済の記録
		 * @param Count 行末の配置候補の手期待の数
		 * @return 製品と同じ候補抽出が行った役割・親の照会数
		 * @throws std::runtime_error 本文構造が無いか，候補の数又は種類が手期待と異なる場合
		 * @throws FormatLimitExceeded 構文の照会数又は配置の条件が製品の上限に反する場合
		 * @throws std::bad_alloc 候補抽出に要るメモリを確保出来ない場合
		 * @throws std::length_error 候補又は作業表の要素数が格納先の上限を超える場合
		 * @throws std::out_of_range 照会する識別子又は位置が範囲の外の場合
		 */
		static size_t GetCandidateWork(const LaidOutCode &Code, const CommentRepository &Comments, const size_t Count) {
			// 本文構造を共有した製品の候補抽出と，其の構文の照会数の記録
			Check(Comments.GetStructure() != nullptr, "comment placement lost its shared body structure");
			StructureWorkAccount Work;
			std::vector<std::pair<uint32_t, uint32_t>> OpaqueRanges;
			const std::vector<CommentPlacer::Candidate> Candidates =
			CommentPlacer::CollectCandidates(Code, *Code.GetAnchors().Syntax, Comments, OpaqueRanges, nullptr, &Work);
			// 全ての行末コメントが同じ種別の候補に為る事の照合
			Check(Candidates.size() == Count, "nested call comments lost a placement candidate");
			for(const CommentPlacer::Candidate &Candidate : Candidates) {
				Check(Candidate.Where == CommentSlotKind::LineEnd, "a nested call comment changed its placement kind");
			}
			// 観測した構文の照会数の返戻
			return Work.Used;
		}
	};

	// 製品の規則と適用順を共有し，テストだけから個別の収集を行う入口
	struct SyntaxNormalizerAccess {
		using RuleFixture = SyntaxNormalizer::RuleDescriptor; // 製品規則のテスト内の参照型

		// 検査する工程の結果と記録
		struct Result {
			RewriteTrace Trace;
			size_t Work;
		};

		/**
		 * 束縛の利用集合の構築数の観測関数
		 * 計算量：O(1)
		 * @param Syntax 観測する構文
		 * @return 共有する束縛領域の個数
		 */
		static size_t GetBindingRegionCount(const SyntaxModel &Syntax) {
			return Syntax.BindingRegions.size();
		}

		/**
		 * 呼出先の構造の表の保持数の観測関数
		 * 計算量：O(1)
		 * @param Syntax 観測する構文
		 * @return 呼出先の表の容量と記録数
		 */
		static std::pair<size_t, size_t> GetCalleeStorage(const SyntaxModel &Syntax) {
			// 葉だけの照会に依る未使用の表の生成の検査
			return { Syntax.Callees.size(), Syntax.LiveCallees };
		}

		/**
		 * 単純な末尾判定と詳細な終端の要約の照合関数
		 * 計算量：本体の走査量 S と詳細判定の総費用 Q に対し O(S + Q)
		 * @param Syntax 受理した構文
		 * @param Function 通常関数の候補
		 * @throws std::runtime_error 単純な判定が詳細な判定と異なる場合
		 * @throws FormatLimitExceeded 詳細な判定が作業予算を超える場合
		 * @throws std::bad_alloc 判定の作業列又は事実を確保出来ない場合
		 * @throws std::length_error 作業列又は事実の上限を超える場合
		 * @throws std::out_of_range ノード又は事実の番号が表に無い場合
		 */
		static void CheckSimpleEndReach(const SyntaxModel &Syntax, const NodeId Function) {
			// 通常関数の末尾と単純な判定の取得
			const ControlFlow Simple(Syntax);
			const std::optional<FunctionEnd> End = Simple.ClassifyFunction(Function);
			// 単純な判定の対象でない関数の終了
			if(!End || !Simple.GetSimpleEndReach(End->Stmts)) return;
			const EndReachability Actual = Simple.EndReach(Function, *End);
			ControlFlow Detailed(Syntax);
			// 共有表への空の結果で単純な判定だけを省き，通常の詳細合成を使う
			Detailed.SimpleBody = End->Stmts;
			Detailed.SimpleReach = std::nullopt;
			Check(Actual == Detailed.EndReach(Function, *End), "simple reach differs from detailed termination");
			// 照合の終了
			return;
		}

		/**
		 * ノード表と子列の共有メモリの作業量の取得関数
		 * 計算量：ノード数 N に対し O(N)
		 * @param Syntax 観測する構文モデル
		 * @return コンテナの要求数・システムの確保数・要求バイト数
		 * @throws std::runtime_error 子列のリソースがノード表の所有者と異なる場合
		 */
		static std::array<size_t, 3> GetNodeStorage(const SyntaxModel &Syntax) {
			const std::shared_ptr<SyntaxModel::NodeMemory> Storage = Syntax.Nodes.get_allocator().Storage;
			for(const SyntaxNode &Node : Syntax.Nodes) {
				Check(Node.Children.get_allocator().resource() == &Storage->Children, "projected children escaped pooled storage");
			}
			// ノード表のメモリの作業量の返戻
			return { Storage->Requests, Storage->Blocks, Storage->Bytes };
		}

		/**
		 * 比較名の表が所有するテキスト量の取得関数
		 * 計算量：登録した名前数 K に対し O(K)
		 * @param Syntax 名前を投影済の構文
		 * @return 比較名の総バイト数
		 */
		static size_t GetComparisonNameBytes(const SyntaxModel &Syntax) {
			size_t Bytes = 0;
			for(const std::string &Name : Syntax.CmpNames) Bytes += Name.size();
			// 共有した名前表の保持量の返戻
			return Bytes;
		}

		/**
		 * 役割の位置列の格納数の観測関数
		 * 計算量：O(1)
		 * @param Syntax 役割インデックスを構築済の構文
		 * @return 共有する位置列の要素数とノード毎の範囲数
		 */
		static std::pair<size_t, size_t> GetRoleStorage(const SyntaxModel &Syntax) {
			// 平坦な位置列とノード毎の範囲の格納数の返戻
			return { Syntax.IndexedPositions.size(), Syntax.IndexedPositionRanges.size() };
		}

		/**
		 * 規則候補の構文順を直接照合する取得関数
		 * 計算量：候補数 K，今回と前回の祖先数 P と追加ノード数 A に対し O(K + P log(P + 1) + A)
		 * @param Syntax 構文モデル
		 * @param Found 重複・除去済の候補を含む列
		 * @param Normalizer 同じ整列の作業枠を再使用する正規化
		 * @return 現在の木に接続する構文順の候補
		 * @throws FormatLimitExceeded 作業予算を超える場合
		 * @throws std::bad_alloc 作業列を確保出来ない場合
		 * @throws std::length_error 作業列の上限を超える場合
		 * @throws std::out_of_range 候補の識別子が表に無い場合
		 */
		static std::vector<NodeId> SortCandidates(
			const SyntaxModel &Syntax,
			const std::vector<NodeId> &Found,
			const SyntaxNormalizer &Normalizer
		) {
			// 現在の規則候補の返戻
			return Normalizer.SortInDocOrder(Syntax, Found);
		}

		/**
		 * 整列の再使用可能な作業枠の観測関数
		 * 計算量：O(1)
		 * @param Normalizer 整列を実行した正規化
		 * @return ノード枠数と各作業列の容量
		 */
		static std::array<size_t, 4> GetOrderStorage(const SyntaxNormalizer &Normalizer) {
			// 整列の作業枠の格納数と容量の返戻
			return {
				Normalizer.DocOrder.Paths.size(),
				Normalizer.DocOrder.Paths.capacity(),
				Normalizer.DocOrder.Visited.capacity(),
				Normalizer.DocOrder.Missing.capacity()
			};
		}

		/**
		 * 製品規則表の取得関数
		 * 計算量：初回は規則数 R に対し O(R)，以後は O(1)
		 * @return 製品規則表
		 * @throws std::bad_alloc 規則表のメモリを確保出来ない場合
		 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
		 */
		static const std::vector<RuleFixture> &GetFixtureRules() {
			// 製品規則表の返戻
			return SyntaxNormalizer::GetRules();
		}

		/**
		 * 固定識別名に依る製品規則の取得関数
		 * 計算量：規則数 R に対し O(R)
		 * @param Name 固定の規則名
		 * @return 製品の規則
		 * @throws std::runtime_error 規則が存在しない場合
		 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
		 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
		 */
		static const RuleFixture &GetFixtureRule(const std::string_view Name) {
			// 一致する規則の返戻
			for(const RuleFixture &Rule : GetFixtureRules()) if(Rule.Name == Name) return Rule;
			throw std::runtime_error("a named normalization rule is missing");
		}

		/**
		 * 呼出の供給値と未確定の状態の観測関数
		 * 計算量：呼出先と供給値の照会費用 Q に対し O(Q)
		 * @param Flow 現在の制御の流れ
		 * @param Call 調べる呼出
		 * @return 解決した名前と未確定の有無
		 * @throws FormatLimitExceeded 呼出の解決が作業予算を超える場合
		 * @throws std::bad_alloc 呼出先又は名前を確保出来ない場合
		 * @throws std::length_error 呼出先又は名前の要素数上限を超える場合
		 */
		static std::pair<std::string, bool> GetCallResolution(const ControlFlow &Flow, const NodeId Call) {
			const std::shared_ptr<const ReferenceFacts> &Reference = Flow.Syntax.GetNode(Call).Facts.Reference;
			return Reference ? std::pair(Reference->Name, Reference->IsUnknown) : std::pair(std::string(), false);
		}

		/**
		 * 単純な通常完了で省略する表の大きさの取得関数
		 * 計算量：O(1)
		 * @param Flow 調べる制御フロー
		 * @return 終端の要約数と名前の範囲数
		 */
		static std::pair<size_t, size_t> GetDetailedFlowStorage(const ControlFlow &Flow) {
			// 使用済の表の大きさの返戻
			return { Flow.Terminations.Known.Entries.size() + Flow.Terminations.Known.Additional.size(), Flow.Syntax.BindingScopes.size() };
		}

		/**
		 * 名前の位置を必要時だけ収集する表の観測関数
		 * 計算量：導入名数 M と選択した名前の宣言数 D に対し O(M + D)
		 * @param Flow 調べる制御フロー
		 * @param Scope 導入を調べる有効範囲
		 * @param Text 位置を読む名前（空なら位置を読まない）
		 * @return 導入名数と構築済の位置の表の数
		 * @throws FormatLimitExceeded 導入又は位置の収集が作業予算を超える場合
		 * @throws std::bad_alloc 導入又は位置の表を確保出来ない場合
		 * @throws std::length_error 導入又は位置の表が上限を超える場合
		 * @throws std::out_of_range 参照する名前又は範囲が表に無い場合
		 */
		static std::pair<size_t, size_t> GetCallablePositionDemand(
			const ControlFlow &Flow,
			const NodeId Scope,
			const std::string_view Text
		) {
			const ControlFlow::CallableNameOwner Index = Flow.GetCallableNamesOf(Scope);
			if(!Text.empty()) for(const std::pair<const uint32_t, std::vector<ControlFlow::CallableIntroduction>> &Entry : Index->Names) {
				if(Flow.Syntax.GetCmpNameView(Entry.second.front().Name) == Text) {
					Flow.Syntax.GetBindingSourcePositions(*Index, Entry.first);
					break;
				}
			}
			// 導入だけの照会と位置を使う照会を区別した保持数の返戻
			return { Index->Names.size(), Index->Positions.size() };
		}

		/**
		 * 脱出インデックスの共有メモリと作業列の観測関数
		 * 計算量：所有範囲の数 F，判定範囲の数 S，ノード記録数 N に対し O(F + S + N)
		 * @param Flow 調べる制御フロー
		 * @param Identity 単一の所有範囲のインデックス・根の記録・脱出事象列・作業列のアドレスの格納先（不要なら無）
		 * @return 所有範囲数・ノード記録数・作業列容量・集計済の最小深さの数・脱出事象数・判定範囲数
		 * @throws std::runtime_error 表が別のメモリを使うか，作業列に未処理の要素が残る場合
		 * @throws std::out_of_range 単一の所有範囲の根の記録が無い場合
		 */
		static std::array<size_t, 6> GetLoopExitStorage(const ControlFlow &Flow, std::array<const void *, 4> *Identity = nullptr) {
			std::array<size_t, 6> Result { Flow.LoopExits.size(), 0, 0, 0, 0, 0 };
			if(Identity) Identity->fill(nullptr);
			for(const std::pair<const NodeId, ControlFlow::LoopExitIdx> &Entry : Flow.LoopExits) {
				const ControlFlow::LoopExitIdx &Idx = Entry.second;
				if(Identity && Flow.LoopExits.size() == 1) {
					*Identity = { &Idx, &Idx.Nodes.at(Flow.GetLoopExitPosition(Idx, Idx.Root)), Idx.Events.data(), Idx.Pending.data() };
				}
				const std::pmr::memory_resource *const Storage = &Idx.Storage;
				Check(
					Idx.Nodes.get_allocator().resource() == Storage && Idx.SwitchParents.get_allocator().resource() == Storage &&
					Idx.Events.get_allocator().resource() == Storage && Idx.AdditionalMin.get_allocator().resource() == Storage &&
					Idx.Pending.get_allocator().resource() == Storage,
					"loop exit storage did not share its owner's memory"
				);
				Check(Idx.Pending.empty(), "loop exit scratch retained pending work");
				Result[1] += Idx.Nodes.size();
				Result[2] += Idx.Pending.capacity();
				for(const ControlFlow::LoopExitIdx::NodeRecord &Record : Idx.Nodes) if(Record.PlainMin.has_value()) ++Result[3];
				Result[4] += Idx.Events.size();
				Result[5] += Idx.AdditionalMin.size();
				for(const ControlFlow::LoopExitIdx::NodeRecord &Record : Idx.Nodes) Result[5] += Record.Minimum.has_value();
			}
			// 同じ所有範囲の記録数と再使用する容量の返戻
			return Result;
		}

		/**
		 * 書換通知を製品と共有する制御フローの取得関数
		 * @param Syntax テスト内の構文
		 * @return 当該モデルが所有する制御フロー
		 * @throws std::bad_alloc 共有する制御フローを確保出来ない場合
		 */
		static const ControlFlow &GetSharedFlow(SyntaxModel &Syntax) {
			if(!Syntax.Normalization.FlowFacts) Syntax.Normalization.FlowFacts = std::make_shared<ControlFlow>(Syntax);
			// 書換で失効する事実の返戻
			return *Syntax.Normalization.FlowFacts;
		}

		/**
		 * 一時の制御フローが共有する呼出先の要約の計上量の観測関数
		 * 計算量：未収集の部分木の構造 S に対し O(S)，再利用時 O(1)
		 * @param Syntax 同じ所有者の構文
		 * @param Node 呼出先の構造の対象
		 * @return 照会が計上した作業量
		 * @throws FormatLimitExceeded 部分木の収集が作業予算を超えた場合
		 * @throws std::bad_alloc 要約又は作業列を確保出来ない場合
		 * @throws std::length_error 作業列の要素数上限を超える場合
		 * @throws std::out_of_range 対象のノードが表に無い場合
		 */
		static size_t GetFreshCalleeWork(const SyntaxModel &Syntax, const NodeId Node) {
			const size_t Before = Syntax.GetWorkAccount()->Used;
			const ControlFlow Flow(Syntax);
			Flow.GetCalleeSummary(Node);
			// 一時の制御フローを跨ぐ生成又は再利用の費用の返戻
			return Syntax.GetWorkAccount()->Used - Before;
		}

		/**
		 * 独立した関数の宣言と外側の共有要約の計上量の観測関数
		 * 計算量：製品の宣言収集と外側の要約の生成費用 Q に対し O(Q)
		 * @param Syntax テスト内の構文
		 * @param Functions 同じ構文の独立した関数
		 * @return 収集の計上量
		 * @throws FormatLimitExceeded 収集が作業予算を超える場合
		 * @throws std::bad_alloc 名前又は作業列を確保出来ない場合
		 * @throws std::length_error 名前又は作業列の上限を超える場合
		 * @throws std::out_of_range ノード又は名前が表に無い場合
		 */
		static size_t GetVisibleNameWork(const SyntaxModel &Syntax, const std::span<const NodeId> Functions) {
			const size_t Before = Syntax.GetWorkAccount()->Used;
			for(const NodeId Function : Functions) Syntax.GetBindingScopeFacts(Function);
			// 製品の計上値から求めた実際の増分の返戻
			return Syntax.GetWorkAccount()->Used - Before;
		}

		/**
		 * メンバ再代入の照会と実際の計上量の観測関数
		 * 計算量：製品の照会費用 Q に対し O(Q)
		 * @param Syntax テスト内の構文
		 * @param Flow 当該構文の制御フロー
		 * @param Reference メンバを取得する位置
		 * @param Target 標準対象の完全名
		 * @return 再代入の有無と増えた計上量
		 * @throws FormatLimitExceeded 照会が作業予算を超える場合
		 * @throws std::bad_alloc 照会する表を確保出来ない場合
		 * @throws std::length_error 名前又は表が上限を超える場合
		 * @throws std::out_of_range 照会するノードが表に無い場合
		 */
		static std::pair<bool, size_t> GetMemberQueryWork(
			const SyntaxModel &Syntax,
			const ReferenceProjection &Flow,
			const NodeId Reference,
			const std::string &Target
		) {
			const size_t Before = Syntax.GetWorkAccount()->Used;
			const bool HasReassignment = Flow.HasMemberReassignment(Reference, Target);
			// 同じ製品経路の結果と作業量の返戻
			return { HasReassignment, Syntax.GetWorkAccount()->Used - Before };
		}

		/**
		 * 同じ側の制御フローへの構造変更通知関数
		 * 計算量：変更部分木のノード数 N，祖先深度 H と失効する事実数 F に対し O(N + H + F)
		 * @param Flow 対象モデルの制御フロー
		 * @param Node 変更部分木の根
		 * @throws FormatLimitExceeded 失効の作業予算を超える場合
		 * @throws std::bad_alloc 失効する名前の作業列を確保出来ない場合
		 * @throws std::length_error 作業列の要素数上限を超える場合
		 * @throws std::out_of_range 対象ノードがモデルに無い場合
		 */
		static void InvalidateFlowStructure(const ControlFlow &Flow, const NodeId Node) {
			Flow.Invalidate(Node, true);
			// 終了
			return;
		}

		/**
		 * 実引数の共有要約の未確定性の取得関数
		 * 計算量：初回はトークン数 N に対し O(N)，再照会は O(1)
		 * @param Flow 対象の制御フロー
		 * @param Node マクロの実引数のトークン列
		 * @return 未確定な制御移動が有れば true
		 * @throws FormatLimitExceeded 実引数の探索が作業予算を超える場合
		 * @throws std::bad_alloc 要約又は作業列を確保出来ない場合
		 * @throws std::length_error 要約の列の要素数上限を超える場合
		 * @throws std::out_of_range ノード又は子の要約が表に無い場合
		 */
		static bool HasUnknownMacroArguments(const ControlFlow &Flow, const NodeId Node) {
			// 両方の製品経路が使う要約の返戻
			return static_cast<bool>(Flow.GetMacroArguments(Node).Effects & ControlFlow::UnknownExitEffect);
		}

		/**
		 * 観測が比較する実事実の変更関数
		 * 計算量：編集関数の費用 Q に対し O(Q)
		 * @param Syntax テスト内の独立した構文
		 * @param Node 変更するノード
		 * @param Edit 観測の反例を作る編集
		 * @throws std::out_of_range ノードが表に無い場合
		 */
		static void EditObservationFact(SyntaxModel &Syntax, const NodeId Node, const std::function<void(SyntaxFacts &)> &Edit) {
			// 観測が比較する実事実の変更
			Edit(Syntax.Nodes.at(Node).Facts);
			// 終了
			return;
		}

		/**
		 * 観測用の固定契約の取得関数
		 * 計算量：固定契約の列の要素数 N に対し O(N)
		 * @param Syntax テスト内の構文
		 * @param Node 契約を取得するノード
		 * @return 構文が参照する固定契約のコピー
		 * @throws std::bad_alloc 固定契約のコピーのメモリを確保出来ない場合
		 * @throws std::length_error 固定契約の列の要素数上限を超える場合
		 */
		static KindDef GetObservationKind(const SyntaxModel &Syntax, const NodeId Node) {
			// `friend` の入口から取得した固定契約のコピーの返戻
			return *Syntax.GetDefOf(Node);
		}

		/**
		 * 観測用の固定契約の差替関数
		 * 計算量：O(1)
		 * @param Syntax テスト内の独立した構文
		 * @param Node 契約を変えるノード
		 * @param Def 観測の終了迄生存する固定契約
		 * @throws std::out_of_range ノードが表に無い場合
		 */
		static void SetObservationKind(SyntaxModel &Syntax, const NodeId Node, const KindDef &Def) {
			// テスト内で生存する固定契約の参照の設定
			Syntax.Nodes.at(Node).Kind.Def = &Def;
			// 終了
			return;
		}

		/**
		 * 構造を保つ観測専用のノード番号の移動関数
		 * 未使用のノードを前へ加え，観測が読む親子と導入先の番号だけを合わせる
		 * 計算量：ノード数 N と子の総数 C に対し O(N + C)
		 * @param Syntax 観測だけに使うテスト内の構文
		 * @throws std::bad_alloc ノード表のメモリを確保出来ない場合
		 * @throws std::length_error ノード表の要素数上限を超える場合
		 */
		static void ShiftObservationNodes(SyntaxModel &Syntax) {
			// 未使用の先頭を加えても，木の前順と事実の構造上の対応は変わらない
			Syntax.Nodes.emplace_front();
			if(!Syntax.InsertedNodes.empty()) Syntax.InsertedNodes.insert(Syntax.InsertedNodes.begin(), false);
			for(size_t Idx = 1; Idx < Syntax.Nodes.size(); ++Idx) {
				SyntaxNode &Node = Syntax.Nodes[Idx];
				if(Node.Parent != NoNode) ++Node.Parent;
				if(Node.Facts.ImportScope != NoNode) ++Node.Facts.ImportScope;
				for(NodeId &Child : Node.Children) ++Child;
			}
			++Syntax.Root;
			// 終了
			return;
		}

		/**
		 * 投影だけで変更ノードの記録を残したかの観測関数
		 * 計算量：O(1)
		 * @param Syntax 投影直後の構文
		 * @return 変更ノードの記録が空なら true
		 */
		static bool HasEmptyProjectionTouches(const SyntaxModel &Syntax) {
			// 投影後の変更記録の空判定の返戻
			return Syntax.LastTouched.empty();
		}

		/**
		 * 根のエラー祖先を独立に設定するテスト関数
		 * 計算量：O(1)
		 * @param Syntax 正常な子孫を持つ投影済の構文
		 * @param Error 此のテスト中に保持するエラーの種別
		 */
		static void SetErrorAncestor(SyntaxModel &Syntax, const KindDef &Error) {
			// 根のエラー祖先を独立に設定するテスト
			Syntax.Nodes[Syntax.Root].Kind.Def = &Error;
			// 終了
			return;
		}

		/**
		 * 通常関数の返戻計画の収集関数
		 * 計算量：制御の流れの判定量 Q に対し O(Q)
		 * @param Syntax 構文モデル
		 * @param Function 対象関数
		 * @return 返戻計画
		 * @throws FormatLimitExceeded 判定の作業予算を超える場合
		 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
		 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
		 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
		 */
		static RewritePlan CollectExplicitReturns(SyntaxModel &Syntax, const NodeId Function) {
			SyntaxNormalizer Normalizer;
			RewritePlan Plan;
			Normalizer.CollectExplicitReturnNode(Syntax, Function, Plan);
			// 返戻計画の返戻
			return Plan;
		}

		/**
		 * 不変指定だけの収集関数
		 * 計算量：束縛候補と其の判定量 Q に対し O(Q)
		 * @param Syntax 役割インデックスを構築済の構文モデル
		 * @return 不変指定する宣言子と書換計画
		 * @throws FormatLimitExceeded 候補の収集又は判定の作業予算を超える場合
		 * @throws std::bad_alloc 候補又は計画のメモリを確保出来ない場合
		 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
		 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
		 */
		static std::pair<std::unordered_set<NodeId>, RewritePlan> CollectImmutableCandidates(SyntaxModel &Syntax) {
			SyntaxNormalizer Normalizer;
			RewritePlan Plan;
			std::unordered_set<NodeId> Frozen = Normalizer.CollectImmutableBindings(Syntax, Plan);
			// 宣言子と書換計画の返戻
			return { std::move(Frozen), std::move(Plan) };
		}

		/**
		 * １つの製品規則の実行関数
		 * 計算量：規則の対象選択と収集・適用の総量 Q に対し O(Q)
		 * @param Syntax 構文モデル
		 * @param Rule 実行する規則
		 * @return 適用した書換の有無
		 * @throws FormatLimitExceeded 判定又は適用の作業予算を超える場合
		 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
		 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
		 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
		 */
		static size_t RunIndividualRule(SyntaxModel &Syntax, const RuleFixture &Rule) {
			// １つの製品規則の実行
			Syntax.BuildRoleIdx();
			SyntaxNormalizer Normalizer;
			Syntax.LastTouched.clear();
			RewriteTrace Trace;
			Normalizer.RunRule(Syntax, Rule, true, Trace);
			// 適用した書換の有無の返戻
			return !Trace.Transfers.empty() || !Syntax.LastTouched.empty();
		}

		/**
		 * 型保持条件と型宣言の語の独立を検査する固定定義の差替関数
		 * 呼出側はモデルを破棄する迄固定定義を保持する
		 * @param Syntax 構文モデル
		 * @param Rules 差替後の固定定義
		 */
		static void SetRules(SyntaxModel &Syntax, const SyntaxRules &Rules) {
			// 既に投影した構文へ別の固定定義を設定する
			Syntax.Def = &Rules;
			// 終了
			return;
		}

		/**
		 * 製品の１回の正規化の実行関数
		 * 計算量：正規化の総作業量 Q に対し O(Q)
		 * @param Syntax 構文モデル
		 * @param Work 規則毎の作業量の格納先（省略時は記録しない）
		 * @return アンカーの移譲と作業量
		 * @throws FormatLimitExceeded 正規化の作業予算を超える場合
		 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
		 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
		 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
		 */
		static Result Run(SyntaxModel &Syntax, std::vector<std::pair<std::string_view, size_t>> *const Work = nullptr) {
			// 検査前の構造作業量の取得
			const size_t Before = Syntax.GetWorkAccount()->Used;
			SyntaxNormalizer Normalizer;
			Normalizer.RuleWork = Work;
			RewriteTrace Trace = Normalizer.Normalize(Syntax);
			// アンカーの移譲と作業量の返戻
			return { std::move(Trace), Syntax.GetWorkAccount()->Used - Before };
		}
	};
} // namespace Core

// 正規化後の期待構文と，独立して投影した候補をテストだけから照合する入口
struct SyntaxValidatorCmp {
	/**
	 * 保護標識の省略と指定を伴うコメントの論理行の検査関数
	 * 計算量：固定入力の本文長 N に対し O(N)
	 * @throws std::runtime_error 本文・順序又は内容の無い論理行の除去が期待と異なる場合
	 * @throws std::bad_alloc 論理行のメモリを確保出来ない場合
	 * @throws std::length_error 論理行の列が上限を超える場合
	 */
	static void CheckPlacedLines() {
		constexpr std::string_view Text = "alpha // beta\nomega // tail";
		const std::vector<std::string> Split { "alpha", "beta", "omega", "tail" };
		Check(
			SyntaxValidator::PlacedLines(Text) == std::vector<std::string>{ "alpha // beta", "omega // tail" },
			"unstacked comment lines changed"
		);
		Check(SyntaxValidator::PlacedLines(Text, "// ") == Split, "comment lines with omitted protection changed");
		Check(
			SyntaxValidator::PlacedLines(Text, "// ", std::span<const unsigned char>()) == Split,
			"comment lines with empty protection changed"
		);
		std::vector<unsigned char> Protected(Text.size(), 0);
		Check(SyntaxValidator::PlacedLines(Text, "// ", Protected) == Split, "unprotected comment lines changed");
		Protected[6] = 1; // 最初の行の本文中の記号をコード例として保護
		Check(
			SyntaxValidator::PlacedLines(Text, "// ", Protected) == std::vector<std::string>{ "alpha // beta", "omega", "tail" },
			"protected code marker was split or following lines were lost"
		);
		Check(
			SyntaxValidator::PlacedLines("alpha\n\nomega", "// ") == std::vector<std::string>{ "alpha", "omega" },
			"a comment line without content was kept"
		);
		// 終了
		return;
	}

	/**
	 * 独立して投影した前処理の見出のコメントの照合関数
	 * 計算量：両側のノード数 N，記録数 C，本文長 L，本文構造の判定費用 Q に対し O(N + C + L + Q)
	 * @param Syntax 入力側の正規化とコード配置を終えた構文
	 * @param Expected 正規化済の入力側の記録と検証用の範囲
	 * @param Actual 候補から独立に得た記録と構文
	 * @return 見出の本文・種別・順序・出現数が一致すれば true
	 * @throws std::bad_alloc 保持事実のメモリを確保出来ない場合
	 * @throws std::length_error 作業列又は本文が上限を超える場合
	 * @throws std::out_of_range アンカー又は逐語コメントの範囲が不正の場合
	 */
	static bool HasSameDirectiveComments(
		const Core::SyntaxModel &Syntax,
		const Core::ProjectionResult &Expected,
		const Core::ProjectionResult &Actual
	) {
		return SyntaxValidator::GetDirectiveCommentFacts(
			Syntax,
			Expected.Comments,
			Expected.Validation.ProtectedRegions.KeptComments
		) == SyntaxValidator::GetDirectiveCommentFacts(Actual.Syntax, Actual.Comments, Actual.Validation.ProtectedRegions.KeptComments);
	}

	/**
	 * 独立した両側のドキュメントの照合と作業量の取得関数
	 * 計算量：製品の照合費用 Q に対し O(Q)
	 * @param Expected 入力側で得た期待の記録
	 * @param Actual 完成候補側で独立に得た記録
	 * @param Work 記録と論理行の訪問数の格納先
	 * @param TargetQueries 説明対象の照会数の格納先
	 * @return 説明対象・本文・記法の区別が保たれれば true
	 * @throws std::bad_alloc 照合のメモリを確保出来ない場合
	 * @throws std::length_error 作業列の要素数上限を超える場合
	 */
	static bool HasSameDocs(
		const std::span<const ValidationContext::DocFact> Expected,
		const std::span<const ValidationContext::DocFact> Actual,
		size_t &Work,
		size_t &TargetQueries
	) {
		return SyntaxValidator::HasSameDocs(Expected, Actual, {}, {}, &Work, &TargetQueries);
	}

	/**
	 * 宣言の選択結果を含む候補の構造の照合関数
	 * 計算量：構文・宣言区分と共通種別の照合の費用 Q に対し O(Q)
	 * @param Lang 入出力で共通の言語
	 * @param Input 入力の投影元の記号
	 * @param Output 独立して投影した出力候補
	 * @param Expectation 正規化後の構文と選択した形式
	 * @param Work 対応数・容量を増したノード数・最大容量の格納先（無ければ観測しない）
	 * @param BoundaryAdvances 境界の前進数の格納先（無ければ観測しない）
	 * @return 許可した表現対応だけで一致するなら true
	 * @throws FormatLimitExceeded 候補の構造の照合でモデルの作業予算を超えた場合
	 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
	 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
	 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
	 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
	 */
	static bool IsKept(
		const ::Lang Lang,
		const Core::ProjectionResult &Input,
		const Core::ProjectionResult &Output,
		const Core::OutputExpectation &Expectation,
		std::array<size_t, 3> *const Work = nullptr,
		size_t *const BoundaryAdvances = nullptr
	) {
		// 原文と候補の種別比較の定義
		const SyntaxValidator::KindCmp Kinds {
			Input.Validation.SemanticChecks.Symbols,
			Output.Validation.SemanticChecks.Symbols,
			GrammarProfile::Resolve(Lang)
		};
		// 許可済の選択だけを適用する製品の照合と作業量の観測
		SyntaxValidator::MatchWork Observation;
		const bool IsSame =
		SyntaxValidator::HasSameStructure(Output.Syntax, Expectation, Kinds, false, Work || BoundaryAdvances ? &Observation : nullptr);
		if(Work) *Work = { Observation.Pairs, Observation.Growth, Observation.Peak };
		if(BoundaryAdvances) *BoundaryAdvances = Observation.BoundaryAdvances;
		// 観測した照合結果の返戻
		return IsSame;
	}

	/**
	 * コメント経路へ分離した独立コメントの本文の照合関数
	 * 計算量：両側のコメント本文の総長 B に対し O(B)
	 * @param Expected 入力側のコメントと認識済の説明対象
	 * @param Actual 独立して投影した候補のコメントと説明対象
	 * @return 通常コメントの論理行が同じなら true
	 * @throws std::bad_alloc 本文の比較列を確保出来ない場合
	 * @throws std::length_error 本文の比較列が最大長を超える場合
	 */
	static bool HasSameOrdinaryComments(const Core::ProjectionResult &Expected, const Core::ProjectionResult &Actual) {
		// コードのトークン比較から独立した本文の一致の返戻
		return SyntaxValidator::OrdinaryBodies(Expected.Comments.GetRecords(), Expected.Comments.GetDocTargets()) ==
		SyntaxValidator::OrdinaryBodies(Actual.Comments.GetRecords(), Actual.Comments.GetDocTargets());
	}

	/**
	 * エラー領域のテキストとコメント位置の照合関数
	 * 計算量：製品の領域照合の総費用 Q に対し O(Q)
	 * @param Src 独立して解析した候補
	 * @param InputZones 入力に残す領域のテキスト
	 * @param Ranges 所有ノードから定まる領域毎のコード位置
	 * @return 候補で失った領域数
	 * @throws FormatLimitExceeded 領域の識別と所有範囲の数が一致しない場合
	 * @throws std::bad_alloc テキスト・照合インデックス又はコメント位置の確保に失敗した場合
	 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
	 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
	 */
	static size_t LostZones(
		const ParsedTree &Src,
		const std::vector<ValidationContext::ErrorZone> &InputZones,
		const std::vector<std::pair<uint32_t, uint32_t>> &Ranges
	) {
		// コメントの保護範囲の取得
		const std::vector<std::pair<uint32_t, uint32_t>> Comments = Src.CollectCommentRanges(static_cast<uint32_t>(Src.size()));
		// 領域の位置を共有し，別のテキストの出現を代わりに使わない照合
		SyntaxValidator::ErrorZoneLayout Layout;
		Layout.After = SyntaxValidator::IndexCode(Src.GetCodeText(), Comments);
		Layout.Ranges = Ranges;
		// 製品の領域照合で失った数の返戻
		return SyntaxValidator::CountLostErrorZones(InputZones, Layout);
	}
};

/**
 * 正規の入力経路での構文の投影関数
 * 計算量：入力処理・解析・投影の費用 Q に対し O(Q)
 * @param Lang 原文の言語
 * @param Src 解析・投影する原文
 * @param Work 親子表・修復候補・フィールドの観測値の格納先（無ければ観測しない）
 * @param Facts 真偽値名の照会と最初のトークンの継承の観測先（無ければ観測しない）
 * @param ProjectionQueries 実引数の子の読取・区切記法の合成・読違と隣接条件の照会の回数の格納先（無ければ観測しない）
 * @return 原文と解析結果を所有しない独自のノード表と検証データ
 * @throws std::runtime_error 入力を受理出来ない場合
 * @throws FormatLimitExceeded 入力の処理・解析・投影の上限を超えた場合
 * @throws std::bad_alloc 入力・解析結果又は投影する構文表の確保に失敗した場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static Core::ProjectionResult ProjectWithCtx(
	const ::Lang Lang,
	const std::string &Src,
	std::array<size_t, 5> *const Work = nullptr,
	std::array<size_t, 2> *const Facts = nullptr,
	std::array<size_t, 4> *const ProjectionQueries = nullptr
) {
	// 入力の表現とパーサーの読違の正規化
	const GrammarProfile &Profile = GrammarProfile::Resolve(Lang);
	InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, Src);
	Check(Normalized.Reason == FailureReason::None, "valid input was rejected before parsing");
	ParseInput Input = ParseAdapter::Adapt(Profile, Normalized);
	Check(Input.Decision == ParseDecision::Continue, "valid input was rejected during adaptation");
	// 受理した解析結果から独自の構文表への投影
	const ParsedTree Parsed = SyntaxParser::Parse(*Input.Profile, Input);
	Check(Parsed.GetParseDecision() == ParseDecision::Continue, "valid input was rejected during parsing");
	// 入力と解析結果の寿命から独立した構文表の取得
	size_t FactWork = 0;
	Core::ProjectionResult Projected = Facts || ProjectionQueries ?
	Core::SourceProjectorAccess::ProjectWithWork(
		Parsed,
		*Input.Profile,
		Input,
		FactWork,
		nullptr,
		Facts,
		nullptr,
		ProjectionQueries
	) :
	Core::SourceProjector::Project(Parsed, *Input.Profile, Input);
	Check(
		Core::SyntaxNormalizerAccess::HasEmptyProjectionTouches(Projected.Syntax),
		"projection retained a normalization change record"
	);
	if(Work) {
		*Work = ParsedTreeAccess::GetRelationMetrics(Parsed);
		ParsedTreeAccess::CheckUnusedAdaptation(Parsed);
	}
	// 投影結果の返戻
	return Projected;
}

/**
 * 検証用の投影と診断位置の収集関数
 * 計算量：入力処理・修復・投影の総費用 Q に対し O(Q)
 * @param Lang テストケースの言語
 * @param Src 完成候補として読む手ソース
 * @param ShouldPrepareLint 投影後に製品の遅延経路で静的解析の事実を準備するか
 * @param ProjectionQueries 実引数の子の読取・区切記法の合成・読違と隣接条件の照会の回数の格納先（無ければ観測しない）
 * @param DocQueries 名前取得・種別の表示・表示の照会・論理行の観測先（無ければ観測しない）
 * @return 最終構文と検証データ
 * @throws std::runtime_error 手ソースの読取又は修復が失敗した場合
 * @throws FormatLimitExceeded 入力処理・修復・投影の上限を超えた場合
 * @throws std::bad_alloc テキスト・投影又は検証データの確保に失敗した場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static Core::ProjectionResult ProjectDiagnosticCase(
	const ::Lang Lang,
	const std::string &Src,
	const bool ShouldPrepareLint = true,
	std::array<size_t, 4> *const ProjectionQueries = nullptr,
	std::array<size_t, 4> *const DocQueries = nullptr
) {
	const GrammarProfile &Profile = GrammarProfile::Resolve(Lang);
	InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, Src);
	Check(Normalized.Reason == FailureReason::None, "a diagnostic fixture failed normalization");
	ParseInput Input = ParseAdapter::Read(Profile, Normalized);
	Check(Input.Decision == ParseDecision::Continue, "a diagnostic fixture failed reading");
	Input.Origin = ParseInput::VerificationSrc{ Normalized.Text };
	Check(ParseAdapter::CanRepair(Input), "a diagnostic fixture failed repair");
	const ParsedTree Parsed = SyntaxParser::Parse(*Input.Profile, Input);
	Check(Parsed.GetParseDecision() == ParseDecision::Continue, "a diagnostic fixture failed parsing");
	size_t FactWork = 0;
	Core::ProjectionResult Result = ProjectionQueries || DocQueries ?
	Core::SourceProjectorAccess::ProjectWithWork(
		Parsed,
		*Input.Profile,
		Input,
		FactWork,
		nullptr,
		nullptr,
		nullptr,
		ProjectionQueries,
		DocQueries,
		nullptr
	) :
	Core::SourceProjector::Project(Parsed, *Input.Profile, Input);
	if(ShouldPrepareLint) {
		Core::DiagnosticLocationMap Locations;
		Locations.Text = Src;
		Locations.Nodes = std::move(Result.Validation.InputDiagnostics.Positions);
		Locations.LineStarts.push_back(0);
		for(size_t Pos = 0; Pos < Src.size(); ++Pos) if(Src[Pos] == '\n') {
			Locations.LineStarts.push_back(static_cast<uint32_t>(Pos + 1));
		}
		Check(static_cast<bool>(Result.PrepareLint), "a diagnostic fixture did not defer lint facts");
		Result.PrepareLint(Result.Syntax, Result.Comments, Locations);
		Result.PrepareLint = Core::LintPreparation();
		Result.Validation.InputDiagnostics.Positions = std::move(Locations.Nodes);
	}
	// 製品と同じ遅延経路で準備した診断データの返戻
	return Result;
}

/**
 * 検証データを要しないテストの構文表の投影関数
 * 計算量：入力処理・解析・投影の費用 Q に対し O(Q)
 * @param Lang 原文の言語
 * @param Src 投影する原文
 * @param Work 親子表・修復候補・フィールドの観測値の格納先（無ければ観測しない）
 * @param Facts 真偽値名の照会と最初のトークンの継承の観測先（無ければ観測しない）
 * @param ProjectionQueries 実引数の子の読取・区切記法の合成・読違と隣接条件の照会の回数の格納先（無ければ観測しない）
 * @return パーサーから独立した構文表
 * @throws std::runtime_error 入力を受理出来ない場合
 * @throws FormatLimitExceeded 入力の処理・解析・投影の上限を超えた場合
 * @throws std::bad_alloc 入力・解析結果又は投影する構文表の確保に失敗した場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static Core::SyntaxModel Project(
	const ::Lang Lang,
	const std::string &Src,
	std::array<size_t, 5> *const Work = nullptr,
	std::array<size_t, 2> *const Facts = nullptr,
	std::array<size_t, 4> *const ProjectionQueries = nullptr
) {
	// 検証データを除いた独立したノード表の返戻
	return ProjectWithCtx(Lang, Src, Work, Facts, ProjectionQueries).Syntax;
}

/**
 * 公開のノード表からの役割の観測関数
 * 正規化中だけ有効な候補インデックスを使わず，投影直後と正規化後の両方を観測する
 * 計算量：ノード表の大きさ N に対し期待値 O(N)
 * @param Syntax 観測するノード表
 * @param Role 集めるノードの役割
 * @return 指定した役割を持つ現存ノードの並び
 * @throws FormatLimitExceeded 収集中の事実の収集が作業予算を超える場合
 * @throws std::bad_alloc 現存ノードの並び又は収集中の事実のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
static std::vector<Core::NodeId> GetNodesWithRole(const Core::SyntaxModel &Syntax, const Core::Role Role) {
	// 除かれたノードを除外した全ノードの観測
	std::vector<Core::NodeId> Nodes;
	for(Core::NodeId Id = 0; Id < Syntax.GetNodeCount(); ++Id) if(!Syntax.IsRemovedNode(Id) && Syntax.Is(Id, Role)) {
		Nodes.push_back(Id);
	}
	// 役割を持つ現存ノードの返戻
	return Nodes;
}

// 構文順に対応するノードの双方向の表（投影直後と正規化後は別々に作る）
struct NodeCorrespondence {
	std::unordered_map<Core::NodeId, Core::NodeId> LeftToRight;
	std::unordered_map<Core::NodeId, Core::NodeId> RightToLeft;

	/**
	 * ノードが登録した対応かの判定関数
	 * 計算量：O(1)
	 * @param Left 左側のノード（ノード無も可）
	 * @param Right 右側のノード（ノード無も可）
	 * @return 対応が登録済か，両方がノード無なら true
	 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
	 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
	 */
	bool IsPaired(const Core::NodeId Left, const Core::NodeId Right) const {
		// ノード無の対応の返戻
		if(Left == Core::NoNode || Right == Core::NoNode) return Left == Right;
		const std::unordered_map<Core::NodeId, Core::NodeId>::const_iterator Found = LeftToRight.find(Left);
		// 番号の偶然の一致を用いない，登録した対応の返戻
		return Found != LeftToRight.end() && Found->second == Right;
	}
};

/**
 * 所有する子の変更不能なコードの比較関数
 * 計算量：断片数 F，逐語境界数 V，区切数 S，テキストの長さ B に対し O(F + V + S + B)
 * @param Left 左側の子のコード
 * @param Right 右側の子のコード
 * @return 子の全断片・逐語境界・区切及び終端契約が一致すれば true
 */
static bool IsSameOwnedCode(const LaidOutCode &Left, const LaidOutCode &Right) {
	// 比較する所有領域の断片列の取得
	const std::span<const CodeFragment> First = Left.GetFragments(), Second = Right.GetFragments();
	// 断片数と逐語境界及び終端契約が異なる事の返戻
	if(
		First.size() != Second.size() || Left.GetAnchors().VerbatimBreaks != Right.GetAnchors().VerbatimBreaks ||
		Left.GetAnchors().FragmentTabs != Right.GetAnchors().FragmentTabs ||
		Left.GetEnding().DoesEndWithNewline != Right.GetEnding().DoesEndWithNewline ||
		Left.GetSeparators().size() != Right.GetSeparators().size()
	) return false;
	for(size_t Idx = 0; Idx < First.size(); ++Idx) {
		// 断片の位置・空行又はテキストが異なる事の返戻
		if(
			First[Idx].Start != Second[Idx].Start || First[Idx].End != Second[Idx].End || First[Idx].IsBlank != Second[Idx].IsBlank ||
			First[Idx].Text != Second[Idx].Text
		) return false;
	}
	for(size_t Idx = 0; Idx < Left.GetSeparators().size(); ++Idx) if(Left.GetSeparators()[Idx] != Right.GetSeparators()[Idx]) {
		// 区切が異なる事の返戻
		return false;
	}
	// 子の現在のコードの一致の返戻
	return true;
}

/**
 * 構造上対応する名前の束縛事実の比較関数
 * 計算量：O(1)
 * @param Left 左側の束縛
 * @param Right 右側の束縛
 * @param Nodes 現存する全ノードの構造の対応
 * @return 導入先・範囲・利用の事実が対応して居れば true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
static bool IsSameBinding(const Core::BindingFact &Left, const Core::BindingFact &Right, const NodeCorrespondence &Nodes) {
	// 番号の一致に依らない束縛事実の一致の返戻
	return Nodes.IsPaired(Left.Decl, Right.Decl) && Nodes.IsPaired(Left.Scope, Right.Scope) && Left.IsDecl == Right.IsDecl &&
	Left.IsWrite == Right.IsWrite && Left.IsCapture == Right.IsCapture && Left.IsUncertain == Right.IsUncertain &&
	Left.Namespace == Right.Namespace;
}

/**
 * 現在のノード表を比較する関数
 * 書換の控え・要約・候補インデックスを比較元に使わず，現存する値を全て読む
 * 計算量：現存ノード数 N とトークン・境界・所有領域のバイト数 B に対し O(N + B)，束縛の収集量を加算する
 * @param Left 左側の構文
 * @param Right 右側の構文
 * @param Correspondence 一致した場合に返す現在のノードの対応（不一致なら空）
 * @return 構文上対応するノードが同じ構造・事実・トークンを持つなら true
 * @throws FormatLimitExceeded 現在の束縛の収集でモデルの作業予算を超えた場合
 * @throws std::bad_alloc ノードの対応・走査列又は束縛の確保に失敗した場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
static bool IsSameSyntax(const Core::SyntaxModel &Left, const Core::SyntaxModel &Right, NodeCorrespondence &Correspondence) {
	// 現在のノード表の比較
	Correspondence = {};
	NodeCorrespondence Cur;
	// 除去履歴を含む表の大きさでなく，根から構文順に対応する現存ノードの比較
	for(std::vector<std::pair<Core::NodeId, Core::NodeId>> Pending { { Left.GetRoot(), Right.GetRoot() } }; !Pending.empty();) {
		const std::pair<Core::NodeId, Core::NodeId> Pair = Pending.back();
		const Core::NodeId LeftId = Pair.first, RightId = Pair.second;
		Pending.pop_back();
		// 除去済のノード又は重複した対応の返戻
		if(
			Left.IsRemovedNode(LeftId) || Right.IsRemovedNode(RightId) || !Cur.LeftToRight.emplace(LeftId, RightId).second ||
			!Cur.RightToLeft.emplace(RightId, LeftId).second
		) return false;
		const Core::SyntaxNode &LeftNode = Left.GetNode(LeftId), &RightNode = Right.GetNode(RightId);
		// 現在の種別・トークン・構造が異なる事の返戻
		if(
			LeftNode.Kind.Def != RightNode.Kind.Def || LeftNode.Fields != RightNode.Fields ||
			!Cur.IsPaired(LeftNode.Parent, RightNode.Parent) || LeftNode.Idx != RightNode.Idx ||
			Left.IsInsertedNode(LeftId) != Right.IsInsertedNode(RightId) || LeftNode.Lexeme != RightNode.Lexeme ||
			LeftNode.Children.size() != RightNode.Children.size()
		) return false;
		const Core::SyntaxFacts &LeftFacts = LeftNode.Facts, &RightFacts = RightNode.Facts;
		// 現在の構文事実が異なる事の返戻
		if(
			LeftFacts.BlankKey != RightFacts.BlankKey || LeftFacts.Constraints != RightFacts.Constraints ||
			LeftFacts.TokenSpacing != RightFacts.TokenSpacing || LeftFacts.DoesDiscardBinding != RightFacts.DoesDiscardBinding ||
			LeftFacts.CanReassignExistingBinding != RightFacts.CanReassignExistingBinding ||
			LeftFacts.HasFalseName != RightFacts.HasFalseName || LeftFacts.HasTrueName != RightFacts.HasTrueName ||
			LeftFacts.DoesAbsorbLeadingComment != RightFacts.DoesAbsorbLeadingComment ||
			static_cast<bool>(LeftFacts.NameIdx) != static_cast<bool>(RightFacts.NameIdx) ||
			LeftFacts.NameIdx && Left.GetCmpName(LeftId) != Right.GetCmpName(RightId)
		) return false;
		const std::span<const Core::SemanticBoundary> First = LeftFacts.Boundaries.GetView(), Second = RightFacts.Boundaries.GetView();
		// 境界の数が異なる事の返戻
		if(First.size() != Second.size()) return false;
		for(size_t Idx = 0; Idx < First.size(); ++Idx) {
			// 意味境界の値が異なる事の返戻
			if(
				First[Idx].Before != Second[Idx].Before || First[Idx].Kind != Second[Idx].Kind || First[Idx].Gap != Second[Idx].Gap ||
				First[Idx].Src != Second[Idx].Src
			) return false;
		}
		// 所有領域の有無又は現在の値が異なる事の返戻
		if(
			const Core::OwnedRegion *const LeftRegion = Left.GetRegionOf(LeftId), *const RightRegion = Right.GetRegionOf(RightId);
			static_cast<bool>(LeftRegion) != static_cast<bool>(RightRegion) ||
			LeftRegion && !IsSameOwnedCode(*LeftRegion->Code, *RightRegion->Code)
		) return false;
		for(size_t i = LeftNode.Children.size(); i--;) Pending.emplace_back(LeftNode.Children[i], RightNode.Children[i]);
	}
	// 全ノードの対応を定めてから，前方の宣言・範囲の参照する束縛も照合
	for(const std::pair<const Core::NodeId, Core::NodeId> &Pair : Cur.LeftToRight) {
		const Core::NodeId LeftId = Pair.first, RightId = Pair.second;
		// 導入先・捕捉・書込が異なる事の返戻
		if(
			const Core::BindingFact FirstBinding = Left.GetBindingFact(LeftId);
			!IsSameBinding(FirstBinding, Right.GetBindingFact(RightId), Cur) || FirstBinding.IsDecl &&
			(Left.HasBindingWrite(LeftId) != Right.HasBindingWrite(RightId) || Left.HasBindingRef(LeftId) != Right.HasBindingRef(RightId))
		) return false;
	}
	Correspondence = std::move(Cur);
	// 現存する構文の一致の返戻
	return true;
}

/**
 * 修飾した呼出・参照・宣言の名前の保持量と照会を検査する関数
 * 内側の接尾名を別々に保持せず，完全名と単純名の両方を保つ
 * 計算量：倍増する各入力の投影と照会の総費用 Q に対し O(Q)
 * @throws std::runtime_error 比較名の保持量が線形の上限を超えるか，完全名を失った場合
 * @throws FormatLimitExceeded 投影又は照会の予算を超えた場合
 * @throws std::bad_alloc 入力・構文又は観測のメモリを確保出来ない場合
 * @throws std::length_error 入力又は構文の要素数上限を超える場合
 * @throws std::out_of_range ノード又は名前番号が表に無い場合
 */
static void CheckQualifiedNameProjection() {
	// 同じ名前経路を通る呼出・値・変数の型・返戻型・仮引数の型を，独立した入力で照合する
	const std::pair<std::string_view, std::string_view> Uses[] {
		{ "void G() {}", "void Test(){@();}" },
		{ "inline int G=1;", "int Test(){return @;}" },
		{ "struct G {};", "void Test(){@ value;}" }
	};
	for(const std::pair<std::string_view, std::string_view> &Use : Uses) {
		size_t PreviousBytes = 0;
		for(const size_t Count : { 128U, 256U, 512U, 1024U }) {
			std::string Name = "A::";
			for(size_t Idx = 0; Idx < Count; ++Idx) Name += "Next::";
			Name += "G";
			std::string Src = "namespace A { namespace Next = ::A; " + std::string(Use.first) + " }\n";
			for(const char Char : Use.second) {
				if(Char == '@') Src += Name;
				else Src += Char;
			}
			Src += "\n";
			const Core::SyntaxModel Syntax = Project(Lang::Get(Lang::Cpp), Src);
			const size_t Bytes = Core::SyntaxNormalizerAccess::GetComparisonNameBytes(Syntax);
			Check(Bytes < (Src.size() << 1) + 1, "qualified suffixes retained overlapping complete names");
			Check(!PreviousBytes || Bytes < PreviousBytes * 3 + 1, "comparison-name bytes grow quadratically");
			PreviousBytes = Bytes;
			bool Found = false, FoundSimpleName = false;
			for(Core::NodeId Id = 0; Id < Syntax.GetNodeCount(); ++Id) {
				if(Syntax.GetNode(Id).Facts.NameIdx && Syntax.GetCmpNameView(Id) == "Next") FoundSimpleName = true;
				if(Syntax.Is(Id, Core::Role::QualifiedIdentifier) && Syntax.Is(Syntax.GetParentOf(Id), Core::Role::QualifiedIdentifier)) {
					Check(!Syntax.IsComparisonName(Id), "qualified suffix became a separate comparison name");
				}
				if(!Syntax.Is(Id, Core::Role::QualifiedIdentifier) || Syntax.Is(Syntax.GetParentOf(Id), Core::Role::QualifiedIdentifier)) {
					continue;
				}
				if(Syntax.GetText(Id) == Name) Found = true;
			}
			Check(Found && FoundSimpleName, "qualified projection lost its complete structure or simple comparison names");
			const size_t BeforeObservation = Syntax.GetWorkAccount()->Used;
			Core::CoreObservation Observation(Syntax);
			Observation.ObserveSyntax(Syntax, Core::RewriteTrace());
			Check(Syntax.GetWorkAccount()->Used == BeforeObservation, "qualified observation consumed product work");
		}
	}
	// 終了
	return;
}

/**
 * 手記述した構文との比較用のトークン列の取得関数
 * 配置文字列を構築せず，根から到達する葉と逐語領域だけを読む
 * 計算量：現存ノード数 N とトークン量 B に対し O(N + B)
 * @param Syntax ソース又は書換後の構文
 * @param Root トークンを集める根（ノード無の場合は構文全体の根）
 * @return 構文順のトークン列
 * @throws FormatLimitExceeded 構文の取得の予算又は前提を満たさない場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
static std::vector<std::string> GetOracleLexemes(const Core::SyntaxModel &Syntax, const Core::NodeId Root = Core::NoNode) {
	// 比較用のトークン列の収集準備
	std::vector<std::string> Lexemes;
	for(std::vector<Core::NodeId> Pending { Root == Core::NoNode ? Syntax.GetRoot() : Root }; !Pending.empty();) {
		const Core::NodeId Node = Pending.back();
		Pending.pop_back();
		if(Syntax.IsRemovedNode(Node)) continue;
		if(const Core::SyntaxNode &Cur = Syntax.GetNode(Node); !Cur.Lexeme.empty()) Lexemes.push_back(Cur.Lexeme);
		else Pending.insert(Pending.end(), Cur.Children.rbegin(), Cur.Children.rend());
	}
	// 到達可能なトークンだけの返戻
	return Lexemes;
}

/**
 * 投影のトークンと保持境界を出現順に取得する検査用の関数
 * 配置用の空白は作らず，逐語の内容の一重の所有を手期待と照合する
 * 計算量：部分木のノード数 N，境界数 E と保持するテキスト長 B に対し O(N + E + B)
 * @param Syntax 投影直後の構文
 * @param Root 逐語の所有を調べる根
 * @return トークンと意味上の保持境界のテキスト
 * @throws std::bad_alloc 走査又はテキストのメモリを確保出来ない場合
 * @throws std::length_error 走査又はテキストの上限を超える場合
 * @throws std::out_of_range ノードが構文表に無い場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 */
static std::string GetPreservedText(const Core::SyntaxModel &Syntax, const Core::NodeId Root) {
	// 反復走査のノードと進行状態
	struct Frame {
		Core::NodeId Node;
		uint32_t NextChild = 0;
		size_t NextBoundary = 0;
	};
	std::string Text;
	std::vector<Frame> Pending { { Root } };
	while(!Pending.empty()) {
		Frame &Cur = Pending.back();
		const Core::SyntaxNode &Node = Syntax.GetNode(Cur.Node);
		const std::span<const Core::SemanticBoundary> Boundaries = Node.Facts.Boundaries.GetView();
		while(Cur.NextBoundary < Boundaries.size() && Boundaries[Cur.NextBoundary].Before == Cur.NextChild) {
			if(const Core::SemanticBoundary &Boundary = Boundaries[Cur.NextBoundary++]; Boundary.Kind == Core::BoundaryKind::Preserved) {
				Text += Boundary.Gap;
			}
		}
		if(Cur.NextChild == Node.Children.size()) {
			Text += Node.Lexeme;
			Pending.pop_back();
			continue;
		}
		Pending.push_back({ Node.Children[Cur.NextChild++] });
	}
	// 所有するテキストの出現順の返戻
	return Text;
}

/**
 * 独立した手期待と比較する構文順の共通種別と子の役割の取得関数
 * 計算量：現存ノード数 N に対し O(N)
 * @param Syntax 比較する構文
 * @return 根から構文順に並べた固定の種別と子の役割
 * @throws std::bad_alloc 走査列又は種別の列を確保出来ない場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
static std::vector<std::pair<const Core::KindDef *, uint64_t>> GetOracleStructure(const Core::SyntaxModel &Syntax) {
	// 種別と値の構造比較データの収集準備
	std::vector<std::pair<const Core::KindDef *, uint64_t>> Structure;
	std::vector<Core::NodeId> Pending { Syntax.GetRoot() };
	while(!Pending.empty()) {
		const Core::NodeId Node = Pending.back();
		Pending.pop_back();
		Structure.emplace_back(Syntax.GetNode(Node).Kind.Def, Syntax.GetNode(Node).Fields);
		const std::span<const Core::NodeId> Children = Syntax.GetChildrenOf(Node);
		Pending.insert(Pending.end(), Children.rbegin(), Children.rend());
	}
	// 構文順の共通種別と子の役割の返戻
	return Structure;
}

/**
 * 許容したパーサーの欠落と真の破損の投影事実の検査関数
 * 零幅の仮引数の誤読は許容し，名前の欠落と閉じない構文を正常なモデルへ読み替えない
 * 計算量：固定テストケースの適応・解析・投影・分類の総費用 Q に対し O(Q)
 * @throws std::runtime_error 根のエラー事実・診断件数又は受理の区分が独立条件と異なる場合
 * @throws FormatLimitExceeded 適応・解析・投影の上限を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckProjectionErrorFacts() {
	// 言語 ID とファイルの入口で同じ方言を保ち，キャッシュでも区別する
	for(const bool IsJsonc : { false, true }) {
		const Lang Named = Lang::FromName(IsJsonc ? "jsonc" : "json");
		const Lang Detected = Lang::Detect(IsJsonc ? "data.jsonc" : "data.json");
		Check(Named.IsJsonc == IsJsonc && Detected.IsJsonc == IsJsonc, "a JSON entry point lost its dialect");
		// JSON と JSONC の読みの区別は，言語 ID が同じでも保持する
		Check(
			Named.IsJsonc != Lang::Detect(IsJsonc ? "data.json" : "data.jsonc").IsJsonc,
			"JSON entries lost their dialect distinction"
		);
		Check(
			&GrammarProfile::Resolve(Named) == &GrammarProfile::Resolve(Detected),
			"identical JSON dialects did not share their profile"
		);
	}
	Check(
		&GrammarProfile::Resolve(Lang::FromName("json")) != &GrammarProfile::Resolve(Lang::FromName("jsonc")),
		"JSON dialects shared an admission profile"
	);
	// 公式の設定形式が許すコメントを，既存の名前の対応表から読む
	for(const char *Name : { ".babelrc", ".eslintrc", ".hintrc", ".jsfmtrc", ".jshintrc", ".swcrc" }) {
		const Lang Detected = Lang::Detect(Name);
		Check(Detected.Id == Lang::JSON && Detected.IsJsonc, "a comment-capable configuration lost its JSON dialect");
	}
	// 投影する構文の事実のテストケース
	struct FactCase {
		const char *Lang;
		const char *Src;
		bool HasRootError; // 投影した根の ContainsError の手期待
		bool HasBroken; // 入力診断の BrokenCount 非零の手期待
	};
	const FactCase Inputs[] {
		{ "scss", "@mixin m() {}", false, false },
		{ "scss", "@mixin m($) {}", true, true },
		{ "jsonc", "[1,]", false, true }
	};
	for(const FactCase &Input : Inputs) {
		const Core::ProjectionResult Projected = ProjectWithCtx(Lang::FromName(Input.Lang), Input.Src);
		const bool HasRootError = Projected.Syntax.GetNode(Projected.Syntax.GetRoot()).Facts.Has(Core::ConstraintKind::ContainsError);
		const uint32_t BrokenCount = Projected.Validation.InputDiagnostics.BrokenCount;
		std::fprintf(
			stdout,
			"projection facts %s %s: root=%d broken=%u\n",
			Input.Lang,
			Input.Src,
			HasRootError,
			static_cast<unsigned int>(BrokenCount)
		);
		Check(HasRootError == Input.HasRootError, "projection root error differs from its independent classification");
		Check(static_cast<bool>(BrokenCount) == Input.HasBroken, "projection broken count was erased or tolerance became damage");
	}
	// 異なる括弧や重ねた開括弧は，正常な空引数へ修復せず診断する
	for(const char *const Src : { "@include m((;", "@include m(];" }) {
		const GrammarProfile &Profile = GrammarProfile::Resolve(Lang::FromName("scss"));
		InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, Src);
		Check(Normalized.Reason == FailureReason::None, "a broken include fixture stopped before its intended phase");
		ParseInput Adapted = ParseAdapter::Adapt(Profile, Normalized);
		Check(Adapted.Decision == ParseDecision::Continue, "a broken include fixture stopped during adaptation");
		const ParsedTree Parsed = SyntaxParser::Parse(*Adapted.Profile, Adapted);
		Check(Parsed.HasOriginalInputError(), "a broken include became a valid empty argument list");
		Check(static_cast<bool>(SyntaxParser::CountBrokenNodes(Parsed, Parsed.GetRoot())), "a broken include lost every diagnostic");
		if(Parsed.GetParseDecision() == ParseDecision::Continue) {
			const Core::ProjectionResult Projection = Core::SourceProjector::Project(Parsed, *Adapted.Profile, Adapted);
			Check(
				Projection.Syntax.GetNode(Projection.Syntax.GetRoot()).Facts.Has(Core::ConstraintKind::ContainsError),
				"a partial include lost its root error"
			);
			Check(Projection.Validation.InputDiagnostics.BrokenCount, "a partial include lost its broken classification");
		} else {
			Check(
				Parsed.GetParseDecision() == ParseDecision::Skip &&
				(Parsed.GetParseFailure() == FailureReason::InputSyntaxError || Parsed.GetParseFailure() == FailureReason::UnrecognizedSyntax),
				"a broken include failed in an unrelated category"
			);
		}
	}
	// 受理しない入力と手期待
	struct RejectedCase {
		const char *Lang;
		const char *Src;
		bool HasMissing; // 拒否した解析木に欠落トークンが有るかの手期待（一般の破損と区別）
	};
	const RejectedCase Rejected[] {
		{ "c", "int A=1", true },
		{ "css", ".p{background:URL(a/*b*/c.png);}", false },
		{ "css", ".p{background:uRl(a/*b*/c.png);}", false },
		{ "scss", ".p{background:url(a/*b*/c.png);}", false },
		{ "scss", "@mixin m( {}", false }
	};
	for(const RejectedCase &Input : Rejected) {
		// 投影前に拒否したソースには成功モデルを作らず，解析段階の区分と破損を測る
		const GrammarProfile &Profile = GrammarProfile::Resolve(Lang::FromName(Input.Lang));
		InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, Input.Src);
		Check(Normalized.Reason == FailureReason::None, "a rejected parse fixture failed before its intended phase");
		ParseInput Adapted = ParseAdapter::Adapt(Profile, Normalized);
		Check(Adapted.Decision == ParseDecision::Continue, "a rejected parse fixture stopped during adaptation");
		const ParsedTree Parsed = SyntaxParser::Parse(*Adapted.Profile, Adapted);
		Check(Parsed.GetParseDecision() == ParseDecision::Skip, "an unreadable fixture was accepted or failed in another category");
		// 引用の未閉じでなく，一般の破損だけでは入力のエラーを断定しない
		Check(Parsed.GetParseFailure() == FailureReason::UnrecognizedSyntax, "an unreadable fixture acquired an unrelated rejection");
		Check(Parsed.HasOriginalInputError(), "a rejected fixture lost its true input error");
		const uint32_t BrokenCount = SyntaxParser::CountBrokenNodes(Parsed, Parsed.GetRoot());
		Check(static_cast<bool>(BrokenCount), "a rejected fixture lost every broken node");
		Check(
			HasDescendantOf(
				Parsed.GetRoot(),
				[](const TSNode Node) -> bool {
					// 欠落トークンの有無の返戻
					return ts_node_is_missing(Node);
				}
			) == Input.HasMissing,
			"a rejection fixture changed its missing-token classification"
		);
		std::fprintf(stdout, "projection rejected %s %s: broken=%u\n", Input.Lang, Input.Src, static_cast<unsigned int>(BrokenCount));
	}
	// 終了
	return;
}

/**
 * 条件・本体・代替枝の所属をトークンの並びと独立に取得する関数
 * ノード番号・生成履歴・括弧だけのラッパーは比較条件へ含めない
 * 計算量：構文ノード数 N と各分岐の条件・本体・代替枝のトークン走査量 S に対し O(N + S)
 * @param Syntax 手ソース又は書換後の構文
 * @return 構文順の条件・本体・代替枝のトークン列を其々並べた所属表
 * @throws FormatLimitExceeded 構文又は分岐所属の取得の予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
static std::vector<std::vector<std::string>> GetOracleBranches(const Core::SyntaxModel &Syntax) {
	// 比較用の分岐の所属の収集準備
	std::vector<std::vector<std::string>> Branches;
	for(std::vector<Core::NodeId> Pending { Syntax.GetRoot() }; !Pending.empty();) {
		const Core::NodeId Node = Pending.back();
		Pending.pop_back();
		if(const Core::NodeId Cond = Syntax.GetFieldChild(Node, Core::ChildRole::Cond); Cond != Core::NoNode) {
			Core::NodeId Body = Syntax.GetFieldChild(Node, Core::ChildRole::Body);
			if(Body == Core::NoNode) Body = Syntax.GetFieldChild(Node, Core::ChildRole::Consequence);
			const Core::NodeId Alternative = Syntax.GetFieldChild(Node, Core::ChildRole::Alternative);
			Branches.push_back(GetOracleLexemes(Syntax, Cond));
			Branches.push_back(Body == Core::NoNode ? std::vector<std::string>() : GetOracleLexemes(Syntax, Body));
			Branches.push_back(Alternative == Core::NoNode ? std::vector<std::string>() : GetOracleLexemes(Syntax, Alternative));
		}
		const std::span<const Core::NodeId> Children = Syntax.GetChildrenOf(Node);
		Pending.insert(Pending.end(), Children.rbegin(), Children.rend());
	}
	// 構文順の分岐の所属表の返戻
	return Branches;
}

/**
 * 標準の戻値型と，同名の可視導入・別の供給元の区別の検査関数
 * 計算量：固定テストケースの投影・束縛・戻値判定の総費用 Q に対し O(Q)
 * @throws std::runtime_error 戻値の区分が入力の型の手期待と異なる場合
 * @throws FormatLimitExceeded 投影又は判定が作業予算を超える場合
 * @throws std::bad_alloc 構文又は名前表を確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range ノード又はテキストの位置が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckResultTypeBindings() {
	// 標準型の肯定例と，同じ末尾名を持つ利用者型の否定例
	struct Fixture {
		::Lang::ID Lang;
		const char *Src;
		Core::ReturnKind Kind;
		bool IsEmptyVoid = false; // 値を返さない空の本体か（末尾の補完から除く）
	};
	const Fixture Inputs[] {
		{ Lang::C, "int main(void) {}", Core::ReturnKind::Main },
		{ Lang::C, "signed main(void) {}", Core::ReturnKind::Main },
		{ Lang::C, "signed int main(void) {}", Core::ReturnKind::Main },
		{ Lang::C, "int signed main(void) {}", Core::ReturnKind::Main },
		{ Lang::C, "int signed (main)(void) {G();}", Core::ReturnKind::Main },
		{ Lang::C, "#define int void\n#define main F\nint main(void){G();}", Core::ReturnKind::Val },
		{ Lang::C, "#define main F\nint main(void){}", Core::ReturnKind::Val },
		{ Lang::C, "#define main(x) F(x)\nint main(void){G();}", Core::ReturnKind::Val },
		{ Lang::C, "#if FLAG\n#define main F\n#endif\nint main(void){}", Core::ReturnKind::Main },
		{ Lang::C, "#include <external.h>\n#if FLAG\n#define main F\n#endif\nint main(void){}", Core::ReturnKind::Val },
		{ Lang::C, "#define signed unsigned\nint signed main(void){}", Core::ReturnKind::Val },
		{ Lang::C, "#define main F\n#undef main\n#define int void\n#undef int\nint main(void){}", Core::ReturnKind::Main },
		{ Lang::Cpp, "int signed main() {}", Core::ReturnKind::Main },
		{ Lang::Cpp, "signed int (main)() {}", Core::ReturnKind::Main },
		{ Lang::Cpp, "auto main()->int signed {}", Core::ReturnKind::Main },
		{ Lang::Cpp, "#define main F\nint main(){}", Core::ReturnKind::Val },
		{ Lang::Cpp, "#define main(x) F(x)\nint main(){G();}", Core::ReturnKind::Val },
		{ Lang::Cpp, "#if FLAG\n#define main F\n#endif\nint main(){}", Core::ReturnKind::Main },
		{ Lang::Cpp, "#include <external.h>\n#if FLAG\n#define main F\n#endif\nint main(){}", Core::ReturnKind::Val },
		{ Lang::Cpp, "#define signed unsigned\nauto main()->int signed {}", Core::ReturnKind::Val },
		{ Lang::Cpp, "#define main F\n#undef main\nint main(){}", Core::ReturnKind::Main },
		{ Lang::Cpp, "int (main)() {}", Core::ReturnKind::Main },
		{ Lang::Cpp, "auto main()->int {}", Core::ReturnKind::Main },
		{ Lang::Cpp, "namespace N {int (main)() {}}", Core::ReturnKind::Val },
		{ Lang::Cpp, "struct C {auto main()->int {}};", Core::ReturnKind::Val },
		{ Lang::Swift, "struct Void {}\nfunc f()->Void { Void() }\n", Core::ReturnKind::Val },
		{ Lang::Swift, "typealias Void=Int\nfunc f()->Void { 1 }\n", Core::ReturnKind::Val },
		{ Lang::Swift, "typealias Never=Int\nfunc f()->Never { 1 }\n", Core::ReturnKind::Val },
		{ Lang::Swift, "func f<Void>(_ value:Void)->Void { value }\n", Core::ReturnKind::Val },
		{ Lang::Swift, "func f<Never>(_ value:Never)->Never { value }\n", Core::ReturnKind::Val },
		{ Lang::Swift, "func f<Void:AnyObject>(_ value:Void)->Void { value }\n", Core::ReturnKind::Val },
		{ Lang::Swift, "func f<Never:AnyObject>(_ value:Never)->Never { value }\n", Core::ReturnKind::Val },
		{ Lang::Swift, "func f<T:AnyObject>(_ value:T)->Void {}\n", Core::ReturnKind::Void, true },
		{ Lang::Swift, "struct Box<Void> {func f(_ value:Void)->Void { value }}\n", Core::ReturnKind::Val },
		{ Lang::Swift, "struct Box<Void> {}\nfunc f()->Void {}\n", Core::ReturnKind::Void, true },
		{ Lang::Kotlin, "fun <Unit> f(value:Unit):Unit=value\n", Core::ReturnKind::Val },
		{ Lang::Swift, "struct Other{struct Void{}}\nfunc f()->Other.Void { Other.Void() }\n", Core::ReturnKind::Val },
		{ Lang::Swift, "func f()->Void { print(1) }\n", Core::ReturnKind::Void },
		{ Lang::Swift, "func f()->Swift.Void { print(1) }\n", Core::ReturnKind::Void },
		{ Lang::Swift, "func f()->Swift.Never { fatalError() }\n", Core::ReturnKind::Never },
		{ Lang::Kotlin, "class Unit\nfun f():Unit { return Unit() }\n", Core::ReturnKind::Val },
		{ Lang::Kotlin, "fun f():kotlin.Unit { println(1) }\n", Core::ReturnKind::Void },
		{ Lang::Kotlin, "import kotlin.Unit\nfun f():Unit { println(1) }\n", Core::ReturnKind::Void },
		{ Lang::Kotlin, "fun f():kotlin.Nothing { error(1) }\n", Core::ReturnKind::Never },
		{ Lang::CSharp, "class Task{} class C{async Task F(){await Work();}}", Core::ReturnKind::Val },
		{ Lang::CSharp, "using System.Threading.Tasks;class C{async Task F(){await Work();}}", Core::ReturnKind::Void },
		{ Lang::CSharp, "class C{async System.Threading.Tasks.Task F(){await Work();}}", Core::ReturnKind::Void },
		{ Lang::CSharp, "class C{async global::System.Threading.Tasks.Task F(){await Work();}}", Core::ReturnKind::Void },
		{ Lang::Python, "class NoReturn: pass\ndef f()->NoReturn:\n return NoReturn()\n", Core::ReturnKind::Val },
		{ Lang::Python, "def f()->typing.NoReturn:\n raise ValueError()\n", Core::ReturnKind::Never },
		{ Lang::Python, "from typing import NoReturn\ndef f()->NoReturn:\n raise ValueError()\n", Core::ReturnKind::Never },
		{ Lang::Python, "from other import NoReturn\ndef f()->NoReturn:\n return NoReturn()\n", Core::ReturnKind::Val },
		{ Lang::Python, "import typing\nfrom typing import NoReturn\ndef f()->NoReturn:\n raise OSError\n", Core::ReturnKind::Never },
		{ Lang::Python, "typing=Other()\ndef f()->typing.NoReturn:\n return typing.NoReturn()\n", Core::ReturnKind::Val }
	};
	for(const Fixture &Input : Inputs) {
		const Core::SyntaxModel Syntax = Project(Lang::Get(Input.Lang), Input.Src);
		const std::vector<Core::NodeId> Functions = GetNodesWithRole(Syntax, Core::Role::NamedFunctionDef);
		Check(Functions.size() == 1, "a result type fixture lost its function", Input.Src);
		const Core::ControlFlow Flow(Syntax);
		const Core::ReturnShape Shape = Flow.GetReturnShapeOf(Functions.front());
		Check(Shape.Kind == Input.Kind, "a result type spelling was mistaken for standard type evidence", Input.Src);
		if(Input.IsEmptyVoid) {
			Check(!Flow.ClassifyFunction(Functions.front()).has_value(), "an empty void body became a return completion target", Input.Src);
		}
	}
	// 終了
	return;
}

/**
 * ネストの宣言分割と，部品の破壊を拒否する共通の照合の検査関数
 * 計算量：固定テストケースの投影・正規化・構文の走査・行分割・比較の総費用 Q に対し O(Q)
 * @throws std::runtime_error 入力を受理出来ない場合，又は許可済の分割を拒否するか，トークン・演算・順序・束縛の変更を受理する場合
 * @throws FormatLimitExceeded 投影・正規化・構文の走査・行分割・比較が作業予算又は構造上限を超えるか，構文の取得・更新・比較の前提が不正な場合
 * @throws std::bad_alloc 投影・正規化・比較・行分割又は選択のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckNestedPartitions() {
	// 初期化子の中にも分割対象の宣言を持つ入力
	const std::string Src = R"(function Outer() {
const env = { painting: 1, stage: 2, viewer: { position: { clone: () => 3 }, quaternion: { clone: () => 4 } }, handlers: 5 };
const assembleOverviewImage = () => {
const { painting, stage, viewer, handlers } = env, storedOrigins = viewer.position.clone();
const storedRotations = viewer.quaternion.clone();
return [painting, stage, handlers, storedOrigins, storedRotations];
}, onSnapshotDone = assembleOverviewImage;
console.log(JSON.stringify(onSnapshotDone()));
return 0;
}
Outer();
)";
	// 宣言子の順と初期化の単位を保持した，幅６４での独立した対応候補
	const std::string Ref = R"(function Outer() {
const env = { painting: 1, stage: 2, viewer: { position: { clone: () => 3 }, quaternion: { clone: () => 4 } }, handlers: 5 };
const assembleOverviewImage = () => {
const { painting, stage, viewer, handlers } = env;
const storedOrigins = viewer.position.clone();
const storedRotations = viewer.quaternion.clone();
return [painting, stage, handlers, storedOrigins, storedRotations];
};
const onSnapshotDone = assembleOverviewImage;
console.log(JSON.stringify(onSnapshotDone()));
return 0;
}
Outer();
)";
	for(const ::Lang Lang : { Lang::Get(Lang::JavaScript), Lang::Get(Lang::TypeScript), Lang::FromName("tsx") }) {
		Core::ProjectionResult Input = ProjectWithCtx(Lang, Src);
		Core::SyntaxNormalizer().Normalize(Input.Syntax);
		const std::shared_ptr<const Core::SyntaxModel> Normalized = std::make_shared<const Core::SyntaxModel>(Input.Syntax);
		Core::FormatIR IR = Core::Canonicalizer::Build(Normalized);
		IR.CloseWork();
		const Core::BrokenIR Broken = Core::LineBreaker(64).Resolve(std::move(IR));
		// 外側と内側の両方で複数の出力区分が選ばれた事を先に確認する
		bool HasNestedPartition = false;
		for(const Core::DeclPartition &Outer : Broken.Expectation.DeclarationPartitions) {
			for(const Core::DeclPartition &Inner : Broken.Expectation.DeclarationPartitions) {
				HasNestedPartition |=
				Outer.Node != Inner.Node && Outer.Rows.size() > 1 && Inner.Rows.size() > 1 && Normalized->HasNodeWithin(Outer.Node, Inner.Node);
			}
		}
		Check(HasNestedPartition, "the nested declaration test had no nested output partitions");
		Check(
			SyntaxValidatorCmp::IsKept(Lang, Input, ProjectWithCtx(Lang, Ref), Broken.Expectation),
			"selected nested declarations were rejected"
		);
		// 許可した分割の形が同じでも，部品の意味又は被覆を変えた候補を拒む
		for(
			const std::pair<std::string, std::string> &Replacement :
			std::vector<std::pair<std::string, std::string>>{
				{ "painting: 1", "painting: 9" },
				{ "storedOrigins = viewer.position.clone()", "storedOrigins = viewer.position.clone() + 1" },
				{ "storedOrigins = viewer.position.clone()", "storedOrigins = viewer.quaternion.clone()" },
				{ "return [painting, stage", "return [stage, painting" },
				{ "const storedRotations = viewer.quaternion.clone();", "" },
				{
					"const storedOrigins = viewer.position.clone();\nconst storedRotations = viewer.quaternion.clone();",
					"const storedRotations = viewer.quaternion.clone();\nconst storedOrigins = viewer.position.clone();"
				}
			}
		) {
			const std::string &Before = Replacement.first, &After = Replacement.second;
			std::string Changed = Ref;
			const size_t Pos = Changed.find(Before);
			Check(Pos != std::string::npos, "a declaration mutation did not match its source");
			Changed.replace(Pos, Before.size(), After);
			Check(
				!SyntaxValidatorCmp::IsKept(Lang, Input, ProjectWithCtx(Lang, Changed), Broken.Expectation),
				"a damaged declaration piece was accepted"
			);
		}
	}
	// 終了
	return;
}

/**
 * エラー回復の祖先差と独立した正常文の構造差の照合関数
 * 計算量：解析・投影・正規化・配置・照合の総費用 Q に対し O(Q)
 * @throws FormatLimitExceeded 解析・生成・探索の上限又は内部の不変条件に違反する場合
 * @throws std::bad_alloc 呼出先のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約に違反する場合
 * @throws std::length_error テキスト又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 正常文又は呼出先の対応表の位置が無い場合
 * @throws std::runtime_error テストケースを投影出来ない場合又は分類・理由が仕様と異なる場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗する場合
 */
static void CheckErrorZoneValidation() {
	// エラー領域の外に正常文を持つ入力の構築
	std::string Src = "normal(1)\n";
	for(size_t Idx = 0; Idx < 8; ++Idx) Src += "x = 1 if c else ";
	Src += "0\nnormal(2)\n";
	const GrammarProfile &Profile = GrammarProfile::Resolve(Lang::Get(Lang::Python));
	InputNormalizer::NormalizedInput InputText = InputNormalizer::Normalize(Profile, Src);
	ParseInput Input = ParseAdapter::Adapt(Profile, InputText);
	Check(Input.Decision == ParseDecision::Continue, "error-zone input adaptation failed");
	const ParsedTree Parsed = SyntaxParser::Parse(*Input.Profile, Input);
	Check(Parsed.GetParseDecision() == ParseDecision::Continue, "error-zone input parsing failed");
	for(const bool HasBoundaryLoss : { false, true }) {
		Core::ProjectionResult Projection = Core::SourceProjector::Project(Parsed, *Input.Profile, Input);
		Projection.Comments.Remap(Core::SyntaxNormalizer().Normalize(Projection.Syntax));
		Core::FormatIR Intermediate =
		Core::Canonicalizer::Build(std::make_shared<const Core::SyntaxModel>(std::move(Projection.Syntax)));
		Intermediate.CloseWork();
		const Core::BrokenIR Broken = Core::LineBreaker(128).Resolve(std::move(Intermediate));
		const std::shared_ptr<const LaidOutCode> Code = std::make_shared<const LaidOutCode>(Core::CodeLayout::Materialize(Broken));
		std::string Candidate = OutputFinalizer::Finalize(OutputDocument::CodeOnly(Code));
		if(!HasBoundaryLoss) {
			// 製品はエラー領域を跨ぐ継続括弧を生成せず，保持した部分整形を検証出来る（整形仕様 4.6）
			Check(Candidate.find("x = (") == std::string::npos, "a continuation crossed an error zone");
			const SyntaxValidator::ValidationResult Kept = SyntaxValidator().Validate(
				*Input.Profile,
				Code,
				Candidate,
				Core::OutputExpectation(Broken.Expectation),
				Projection.Comments,
				Projection.Validation
			);
			Check(Kept.GetStatus() == SyntaxValidator::ValidationStatus::Verified, "a preserved error zone was rejected");
		}
		// 同じ領域の本文又は直前のトークンを変え，保持違反を明示する（整形仕様 8.2）
		const size_t Head = Candidate.find("else x"), Error = Candidate.find("= 1", Head);
		Check(Head != std::string::npos && Error != std::string::npos, "the error-zone fixture lost its erroneous assignment");
		if(HasBoundaryLoss) Candidate[Head + 5] = 'y';
		else Candidate[Error + 2] = '2';
		const SyntaxValidator::ValidationResult Lost = SyntaxValidator().Validate(
			*Input.Profile,
			Code,
			Candidate,
			Core::OutputExpectation(Broken.Expectation),
			Projection.Comments,
			Projection.Validation
		);
		Check(
			Lost.GetStatus() == SyntaxValidator::ValidationStatus::Skipped && Lost.GetReason() == FailureReason::ErrorZoneLost,
			"lost error zone did not produce the specified skip"
		);
		// エラー領域の喪失と同時に，正常文の前の空行を追加・削除・移動した候補は失敗する（整形仕様 8.1, 8.3）
		for(const int Change : { -1, 1, 0 }) {
			std::string Changed = Candidate;
			if(!Change) {
				const size_t First = Changed.find("\n\n");
				Check(First != std::string::npos, "the error-zone fixture lost its first blank boundary");
				Changed.erase(First, 1);
			}
			const size_t Last = Changed.find("\n\nnormal(2)");
			Check(Last != std::string::npos, "the error-zone fixture lost its last blank boundary");
			if(Change < 0) Changed.erase(Last, 1);
			else Changed.insert(Last, 1, '\n');
			const SyntaxValidator::ValidationResult Blanks = SyntaxValidator().Validate(
				*Input.Profile,
				Code,
				Changed,
				Core::OutputExpectation(Broken.Expectation),
				Projection.Comments,
				Projection.Validation
			);
			Check(
				Blanks.GetStatus() == SyntaxValidator::ValidationStatus::Failed && Blanks.GetReason() == FailureReason::BlankLineChanged,
				"independent blank-line damage did not take precedence over error-zone loss"
			);
		}
		// 無関係な正常文の呼出を添字参照へ改変した候補の独立した構造差の照合
		Candidate.replace(Candidate.find("normal(2)"), std::string("normal(2)").size(), "normal[2]");
		const SyntaxValidator::ValidationResult Changed = SyntaxValidator().Validate(
			*Input.Profile,
			Code,
			Candidate,
			Core::OutputExpectation(Broken.Expectation),
			Projection.Comments,
			Projection.Validation
		);
		Check(
			Changed.GetStatus() == SyntaxValidator::ValidationStatus::Failed && Changed.GetReason() == FailureReason::StructureChanged,
			"independent structural damage did not take precedence over error-zone loss"
		);
	}
	// 終了
	return;
}

/**
 * ネストしたリテラルの保持境界と，補間のコメント分離の検査関数
 * 期待は入力の値とコメントの領域から定め，フォーマッターの出力を取得しない
 * 計算量：固定したテストケースの投影とトークン・境界の走査の総費用 Q に対し O(Q)
 * @throws std::runtime_error 保持する値又はコメントの件数・種別・本文が手期待と異なる場合
 * @throws FormatLimitExceeded 投影の前提又は処理予算を満たさない場合
 * @throws std::bad_alloc 投影・観測のメモリを確保出来ない場合
 * @throws std::length_error 投影・観測の要素数又はテキスト長の上限を超える場合
 * @throws std::out_of_range ノードが構文表に無い場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckLiteralProjection() {
	// ネストしたリテラルの保持境界と，補間のコメント分離の検査の入力と手期待
	struct Fixture {
		const char *Lang;
		const char *Src;
		const char *Literal;
		size_t Comments;
		CommentKind Kind;
		Core::Role LiteralRole = Core::Role::StrLikeAll; // 文字列以外の区切付の値も同じ所有規則で照合
	};
	const Fixture Inputs[] {
		{
			"csharp",
			"class C{string F(int x){return $@\"// literal\n{x // note\n}\nend\";}}",
			"$@\"// literal\n{x}\nend\"",
			1,
			CommentKind::Trailing
		}
	};
	for(const Fixture &Input : Inputs) {
		const Core::ProjectionResult Projected = ProjectWithCtx(Lang::FromName(Input.Lang), Input.Src);
		const std::vector<Core::NodeId> Literals = GetNodesWithRole(Projected.Syntax, Input.LiteralRole);
		const std::string Ctx = std::string("literal projection (") + Input.Lang + "): " + Input.Src + ": ";
		Check(!Literals.empty(), (Ctx + "a literal has no shared role").c_str());
		Check(
			GetPreservedText(Projected.Syntax, Literals.front()) == Input.Literal,
			(Ctx + "literal content has overlapping ownership").c_str()
		);
		// 同じ入力のリテラルと非リテラルを，共有の判定と照会数で区別する
		for(const size_t Count : { 16U, 32U, 64U }) {
			Core::StructureWorkAccount LiteralWork, OtherWork;
			LiteralWork.Limit = OtherWork.Limit = 3 * Count;
			for(size_t Idx = 0; Idx < Count; ++Idx) {
				Check(IsLiteralAnchor(Projected.Syntax, Literals.front(), &LiteralWork), "a literal anchor lost its protected boundary");
				Check(!IsLiteralAnchor(Projected.Syntax, Projected.Syntax.GetRoot(), &OtherWork), "a root became a literal anchor");
			}
			Check(LiteralWork.Used >= Count && LiteralWork.Used <= 3 * Count, "literal anchor queries exceeded linear work");
			Check(OtherWork.Used == 3 * Count, "non-literal anchor queries changed their charged short-circuit order");
		}
		const std::span<const CommentRecord> Comments = Projected.Comments.GetRecords();
		Check(Comments.size() == Input.Comments, (Ctx + "a code comment was lost or literal text was separated").c_str());
		for(const CommentRecord &Comment : Comments) {
			Check(
				Comment.Kind == Input.Kind && Comment.Text.find("note") != std::string::npos,
				(Ctx + "the code comment changed kind or body").c_str()
			);
		}
	}
	const Core::SyntaxModel PhpSyntax =
	Project(Lang::Get(Lang::PHP), "<?php $s=\"// literal {$x}\"; $h=<<<TXT\n// literal $x\nTXT;\n");
	Check(GetNodesWithRole(PhpSyntax, Core::Role::StrLikeAll).size() == 2, "PHP interpolated literals have no shared literal role");
	// 型・修飾・演算の役割は，葉とラッパーと生成形式の何れからも照会出来る
	const Core::SyntaxModel Tokens =
	Project(Lang::Get(Lang::Cpp), "class C{virtual void F() const final;static constexpr unsigned Value=0;};");
	const std::pair<const char *, Core::Role> Meanings[] {
		{ "const", Core::Role::ImmutableTypeQualifier },
		{ "constexpr", Core::Role::ModifierConstantExpression },
		{ "final", Core::Role::ExplicitVirtualModifier },
		{ "static", Core::Role::StaticLifetimeModifier },
		{ "unsigned", Core::Role::BuiltinArithmeticType },
		{ "unsigned", Core::Role::NonnegativeNumericType }
	};
	for(const std::pair<const char *, Core::Role> &Meaning : Meanings) {
		const std::vector<Core::NodeId> Nodes = GetNodesWithRole(Tokens, Meaning.second);
		Check(!Nodes.empty(), "a projected token lost its semantic role");
		for(const Core::NodeId Node : Nodes) {
			Check(Tokens.GetText(Node) == Meaning.first, "a semantic role leaked to a different token");
		}
	}
	for(Core::NodeId Id = 0; Id < Tokens.GetNodeCount(); ++Id) {
		if(Tokens.Is(Id, Core::Role::TypeQualifier) && Tokens.GetText(Id) == "const") {
			Check(
				Core::SyntaxModel::HasNodeMatching(
					Tokens,
					Id,
					[&Tokens](const Core::NodeId Part) -> bool {
						// 修飾のトークンに投影した役割の返戻
						return Tokens.Is(Part, Core::Role::ImmutableTypeQualifier);
					}
				),
				"a qualifier wrapper lost its projected token meaning"
			);
		}
	}
	const GrammarProfile &Cpp = GrammarProfile::Resolve(Lang::Get(Lang::Cpp));
	Check(
		Cpp.GetRules().TokenDefs.at("const").Def->Roles.test(static_cast<size_t>(Core::Role::ImmutableTypeQualifier)),
		"a generated qualifier lost its semantic role"
	);
	const Core::SyntaxModel Words = Project(Lang::Get(Lang::Cpp), "void F(){while(true){int Value=0;}}\n");
	for(Core::NodeId Id = 0; Id < Words.GetNodeCount(); ++Id) {
		if(Words.GetLexemeOf(Id) == "while") Check(Words.IsWord(Id, Core::Word::While), "a projected loop word lost its meaning");
		if(Words.GetLexemeOf(Id) == "true") Check(Words.IsWord(Id, Core::Word::True), "a projected true word lost its meaning");
		if(Words.GetLexemeOf(Id) == "0") Check(Words.IsWord(Id, Core::Word::IntZero), "a projected zero word lost its meaning");
	}
	Check(Cpp.GetRules().TrueDef.Meanings.test(static_cast<size_t>(Core::Word::True)), "a generated true word lost its meaning");
	Check(Cpp.GetRules().TokenDefs.at(";").TokenDef != nullptr, "a generated terminator lost its token fact");
	Check(Cpp.IsBuiltinCastType("const unsigned long *const"), "a builtin cast type lost its projected fact");
	Check(!Cpp.IsBuiltinCastType("const UserType *"), "a user type became a builtin cast type");
	Check(Cpp.ReadNumericLiteral("1.0f") == Core::NumericLiteralKind::FloatingPoint, "a floating literal lost its projected type");
	Check(Cpp.ReadNumericLiteral("1ULL") == Core::NumericLiteralKind::Integer, "an integer literal lost its projected type");
	Check(Cpp.ReadNumericLiteral("1unknown") == Core::NumericLiteralKind::Other, "an unknown suffix became a numeric type");
	const GrammarProfile &CSharp = GrammarProfile::Resolve(Lang::Get(Lang::CSharp));
	const Core::AccessMeaning Access = CSharp.ReadAccessMeaning("private protected");
	const Core::AccessMeaning Reversed = CSharp.ReadAccessMeaning("protected private");
	Check(Access.Mask == Reversed.Mask && Access.Length == Reversed.Length, "access modifier order changed the projected set");
	Check(Access.ExactRank == 1 && Reversed.ExactRank == -1, "the exact access form was confused with its unordered meaning");
	// 記号の密着とコメントの有無に依らない可視性の事実
	for(const char *const Text : { "pub(crate) struct Entry{}", "pub /* access */ ( crate ) struct Entry{}" }) {
		const Core::SyntaxModel Visibility = Project(Lang::Get(Lang::Rust), Text);
		const std::vector<Core::NodeId> Modifiers = GetNodesWithRole(Visibility, Core::Role::AccessModifier);
		Check(Modifiers.size() == 1, "a restricted visibility lost its modifier");
		const Core::AccessMeaning &Meaning = Visibility.GetNode(Modifiers.front()).Facts.Access;
		Check(Meaning.Length == 10 && Meaning.ExactRank == 3, "restricted visibility changed with trivia or punctuation");
	}
	// 親の補間が，未訪問の子の等号と通常の演算子を区別して表示するテキストを保護する
	const std::pair<const char *, bool> Interpolations[] {
		{ "f\"{a+b}\"", false },
		{ "f\"{a==b}\"", false },
		{ "f\"{a+b:>10}\"", false },
		{ "f\"{ a + b = }\"", true },
		{ "f\"{ a + b = :>10}\"", true },
		{ "f\"{ a + b = !r:>10}\"", true }
	};
	for(const std::pair<const char *, bool> &Input : Interpolations) {
		const Core::SyntaxModel Interpolation = Project(Lang::Get(Lang::Python), Input.first);
		const std::vector<Core::NodeId> Parts = GetNodesWithRole(Interpolation, Core::Role::DebugInterpolation);
		Check(Parts.size() == 1, "an interpolation lost its projected syntax", Input.first);
		Check(
			Interpolation.GetNode(Parts.front()).Facts.DoesObserveText == Input.second,
			"a parent read an unprepared interpolation token fact",
			Input.first
		);
	}
	const Core::SyntaxModel Ordinary = Project(Lang::Get(Lang::Cpp), "void F(){unsignedValue();finalValue();}");
	Check(GetNodesWithRole(Ordinary, Core::Role::NonnegativeNumericType).empty(), "an ordinary name became a numeric type");
	Check(GetNodesWithRole(Ordinary, Core::Role::ExplicitVirtualModifier).empty(), "an ordinary name became a virtual modifier");
	// 終了
	return;
}

/**
 * 個別規則の発火と反例を手記述したソースと照合する関数
 * 期待ソースは仕様と入力から定め，整形の実出力を取得しない
 * 計算量：手記述した各組の投影・収集・書換・構文・トークン・束縛比較の総費用 Q に対し O(Q)
 * @throws std::runtime_error 対象が無いか，所定の発火・反例又はトークンが手期待と異なる場合
 * @throws FormatLimitExceeded 投影・収集・書換・比較の予算又は前提を満たさない場合
 * @throws std::bad_alloc 投影・計画・期待トークン又は比較データの確保に失敗した場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckIndividualRuleContracts() {
	// 個別規則の発火と反例の手記述したソースと照合
	CheckProjectionErrorFacts();
	// 型の宣言の語を反転しても，不変指定は束縛の意味上の条件に従う
	const std::pair<std::string_view, std::string_view> LexicalInputs[] {
		{ "javascript", "function f(){const n=1;return n;}" },
		{ "javascriptreact", "function f(){const n=1;return n;}" },
		{ "typescript", "function f(){let n=1;return n;}" },
		{ "typescriptreact", "function f(){let n=1;return n;}" }
	};
	for(const std::pair<std::string_view, std::string_view> &Input : LexicalInputs) {
		const ::Lang Lang = Lang::FromName(Input.first);
		Core::SyntaxRules Rules = GrammarProfile::Resolve(Lang).GetRules();
		const bool HasTypeDecl = !Rules.GetSpelling(Core::Word::TypeDecl).empty();
		Rules.Words[static_cast<size_t>(Core::Word::TypeDecl)] =
		HasTypeDecl ? std::vector<std::string_view>{} : std::vector<std::string_view>{ "type" };
		Rules.WordMaxLengths[static_cast<size_t>(Core::Word::TypeDecl)] = HasTypeDecl ? 0 : 4;
		Core::SyntaxModel Syntax = Project(Lang, "function f(){let n=1;return n;}");
		Core::SyntaxNormalizerAccess::SetRules(Syntax, Rules);
		Core::SyntaxNormalizerAccess::RunIndividualRule(Syntax, Core::SyntaxNormalizerAccess::GetFixtureRule("Declarations"));
		const Core::SyntaxModel Oracle = Project(Lang, std::string(Input.second));
		Check(GetOracleLexemes(Syntax) == GetOracleLexemes(Oracle), "type declaration spelling changed binding type preservation");
	}
	// 規則毎の入力と手期待
	struct RuleCase {
		const char *Rule; // 製品の固定規則名
		const char *Lang;
		const char *Input;
		const char *Ref;
		const char *Counterexample;
	};
	const RuleCase Inputs[] {
		{
			"Declarations",
			"go",
			"package p\nfunc f(){const a=1;const b=2;const c=3;return;}\n",
			"package p\nfunc f(){const a,b,c=1,2,3;return;}\n",
			"package p\nfunc f(){const a=1;use(a);const b=2;return;}\n"
		},
		{
			"ExplicitReturn",
			"python",
			"def f():\n from other import sys\n sys.exit()\n",
			"def f():\n from other import sys\n sys.exit()\n return\n",
			"def f():\n import sys\n sys.exit()\n"
		},
		{ "Increment", "cpp", "void f(){size_t a;a++;int b;b++;}", "void f(){size_t a;a++;int b;++b;}", "void f(){size_t a;a++;}" },
		{
			"Braces",
			"kotlin",
			"fun f(){if(a){while(b){g()}}else z()}",
			"fun f(){if(a)while(b)g() else z()}",
			"fun f(){if(a)while(b)g() else z()}"
		},
		{
			"TailReturn",
			"ruby",
			"def self.f\n1\nrescue\n2\nelse\n3\nensure\nh()\nend\n",
			"def self.f\n1\nrescue\nreturn 2\nelse\nreturn 3\nensure\nh()\nend\n",
			"def self.f\n1\nrescue\nreturn 2\nelse\nreturn 3\nensure\nh()\nend\n"
		},
		{ "OptionalParentheses", "python", "f((((x for x in values))))\n", "f(x for x in values)\n", "f((x for x in values),other)\n" },
		{ "Parentheses", "python", "z=((x:=1),)\n", "z=(x:=1,)\n", "z=(x:=1)\n" },
		{
			"Terminator",
			"typescript",
			"interface I{x:number\ny:number\nz:number;}",
			"interface I{x:number,y:number,z:number}",
			"interface I{x:number,y:number,z:number}"
		},
		{ "StatementForms", "ruby", "if ready\nwork()\nend\n", "work() if ready\n", "if ready\nwork()\nmore()\nend\n" },
		{ "TrailingComma", "javascript", "const a=[1,2,];", "const a=[1,2];", "const a=[1,,];" },
		{
			"LoopTerminator",
			"php",
			"<?php while ($ready): break; endwhile ?>",
			"<?php while ($ready): break; endwhile; ?>",
			"<?php while ($ready){break;} ?>"
		},
		{ "Number", "cpp", "int n=0xab;", "int n=0XAB;", "int n=0XAB;" },
		{ "Style", "scss", "@mixin m() {}", "@mixin m {}", "@mixin m($x) {}" },
		{ "Style", "scss", "@mixin m ( ) {}", "@mixin m {}", "@mixin m($x:()) {}" },
		{ "Style", "scss", "@include m();", "@include m;", "@include m(());" },
		{ "Style", "scss", "@include ns.m( );", "@include ns.m;", "@function f(){@return 1;}" },
		{ "Quote", "javascript", "const s='cat';", "const s=\"cat\";", "const s=\"cat\";" },
		{ "BareNotation", "ruby", "def f()\n1\nend\n", "def f\n1\nend\n", "f=1\nf()\n" },
		{ "TypeQuery", "javascript", "typeof x==\"number\";", "typeof x===\"number\";", "typeof x==1;" },
		{
			"ImportOrder",
			"javascript",
			"import {z,a as b} from \"m\";",
			"import {a as b,z} from \"m\";",
			"import {a as b,z} from \"m\";"
		},
		{ "Loop", "cpp", "void f(){for(;;){break;}}", "void f(){while(true){break;}}", "void f(){for(int i=0;i<2;++i){break;}}" },
		{ "BooleanInitializer", "cpp", "void f(){bool b=1;}", "void f(){bool b=true;}", "void f(){int b=1;}" },
		{ "ConstPosition", "cpp", "int const x=1;", "const int x=1;", "int *const p=0;" },
		{ "FusedPrefix", "cpp", "int f(int x){return - -x;}", "int f(int x){return -(-x);}", "int f(int x){return - +x;}" },
		{
			"Separator",
			"go",
			"package main\nfunc f(){\nx:=1;\n_ = x;\n}\n",
			"package main\nfunc f(){x:=1\n_ = x\n}\n",
			"package main\nfunc f(){for i:=0;i<1;i++{}}\n"
		},
		{ "TypeAssociation", "typescript", "type T=|number|string;", "type T=number|string;", "type T=number&(string&{});" },
		{ "Tuple", "python", "x=(1,2)\n", "x=1,2\n", "f((1,2))\n" }
	};
	for(const RuleCase &Input : Inputs) {
		if(
			std::string_view(Input.Rule) == "ExplicitReturn" || std::string_view(Input.Rule) == "TailReturn" ||
			std::string_view(Input.Rule) == "StatementForms"
		) {
			Core::SyntaxModel Protected = Project(Lang::FromName(Input.Lang), Input.Input);
			const std::vector<std::string> ProtectedLexemes = GetOracleLexemes(Protected);
			Core::KindDef Error = *Protected.GetNode(Protected.GetRoot()).Kind.Def;
			Error.Roles.set(static_cast<size_t>(Core::Role::Error));
			Error.InitRoleList();
			Core::SyntaxNormalizerAccess::SetErrorAncestor(Protected, Error);
			Check(
				!Core::SyntaxNormalizerAccess::RunIndividualRule(Protected, Core::SyntaxNormalizerAccess::GetFixtureRule(Input.Rule)),
				"a rule crossed an error ancestor"
			);
			Check(GetOracleLexemes(Protected) == ProtectedLexemes, "an error ancestor permitted a token change");
		}
		std::fprintf(stdout, "individual contract %s\n", Input.Rule);
		const ::Lang Lang = Lang::FromName(Input.Lang);
		const Core::SyntaxModel Oracle = Project(Lang, Input.Ref);
		Check(!Oracle.GetNode(Oracle.GetRoot()).Facts.Has(Core::ConstraintKind::ContainsError), "a handwritten oracle is unreadable");
		const std::vector<std::string> Expected = GetOracleLexemes(Oracle);
		for(const bool IsCounterexample : { false, true }) {
			const char *const Src = IsCounterexample ? Input.Counterexample : Input.Input;
			Core::SyntaxModel Syntax = Project(Lang, Src);
			Check(
				!Syntax.GetNode(Syntax.GetRoot()).Facts.Has(Core::ConstraintKind::ContainsError),
				"an individual rule source is unreadable"
			);
			const Core::SyntaxModel Original = Syntax;
			std::fprintf(stdout, "individual rule %s %s\n", Input.Rule, IsCounterexample ? "negative" : "positive");
			const size_t Applied =
			Core::SyntaxNormalizerAccess::RunIndividualRule(Syntax, Core::SyntaxNormalizerAccess::GetFixtureRule(Input.Rule));
			const std::string Ctx = std::string(Input.Rule) + " (" + Input.Lang + "): " + Src;
			if(IsCounterexample) {
				NodeCorrespondence Nodes;
				Check(IsSameSyntax(Syntax, Original, Nodes), (Ctx + ": a counterexample changed structure, lexemes or binding").c_str());
			} else {
				Check(Applied, (Ctx + ": a rule did not fire").c_str());
				Check(GetOracleLexemes(Syntax) == Expected, (Ctx + ": individual rule lexemes differ from the independent oracle").c_str());
				Check(GetOracleBranches(Syntax) == GetOracleBranches(Oracle), "a rule changed condition or body ownership");
				if(std::string_view(Input.Rule) == "Declarations") {
					Check(
						GetOracleStructure(Syntax) == GetOracleStructure(Oracle),
						(Ctx + ": generated declaration kinds or child roles differ from the oracle").c_str()
					);
				}
			}
		}
	}
	// 終了
	return;
}

/**
 * 後置ラムダの呼出先を保持した括弧と，呼出先を変えた候補の照合関数
 * 計算量：固定テストケースの投影・正規化・構造比較の総費用 Q に対し O(Q)
 * @throws FormatLimitExceeded 投影・正規化・照合の作業予算を超えた場合
 * @throws std::bad_alloc モデル又は対応表の確保に失敗した場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error テキスト又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会するノード又は対応先が表に無い場合
 * @throws std::runtime_error 必要な括弧の喪失又は正常な候補の拒否が有る場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckTrailingLambdaCalls() {
	// 冗長な外層だけを除いた手期待と，内側の呼出へラムダを渡して仕舞う不正な候補
	for(const Lang::ID Id : { Lang::Kotlin, Lang::Swift }) {
		const ::Lang Lang = Lang::Get(Id);
		const std::string Binding = Id == Lang::Kotlin ? "val" : "let";
		Core::ProjectionResult Input = ProjectWithCtx(Lang, Binding + " x = ((f())) { 42 }\n");
		Core::SyntaxNormalizer().Normalize(Input.Syntax);
		Core::OutputExpectation Expectation;
		Expectation.NormalizedSyntax = std::make_shared<const Core::SyntaxModel>(std::move(Input.Syntax));
		Check(
			SyntaxValidatorCmp::IsKept(Lang, Input, ProjectWithCtx(Lang, Binding + " x = (f()) { 42 }\n"), Expectation),
			"a trailing lambda lost its parenthesized receiver"
		);
		Check(
			!SyntaxValidatorCmp::IsKept(Lang, Input, ProjectWithCtx(Lang, Binding + " x = f() { 42 }\n"), Expectation),
			"a trailing lambda moved into the receiver call"
		);
	}
	// 終了
	return;
}

enum class WorkScaleKind : uint8_t {
	CSharpCallChain,
	CallArgParens,
	JavaBracedCallBranches,
	JavaCallBranches,
	LeadingUnion,
	LongMixedDecl,
	MixedDecls,
	NestedBindingRefs,
	NestedParens,
	NestedStmt,
	NestedUnchangedStmt,
	PhpBracedCallBranches,
	PhpCallBranches,
	PostfixIncrement,
	PrefixIncrement,
	RustForeignMacros,
	RustLocalMacros,
	SplitDecls
}; // 入力規模を同じ倍率で増やす正規化のソースの種類

/**
 * 作業量の反例と手期待のソースの構築関数
 * 計算量：出力のトークン長 B に対し O(B)
 * @param Kind ソースの種類
 * @param Count 増やすノード又は宣言の数
 * @return 言語，製品の規則名，入力及び手期待のソース
 * @throws std::bad_alloc ソースのメモリを確保出来ない場合
 * @throws std::length_error ソースの長さが上限を超える場合
 */
static std::tuple<Lang::ID, const char *, std::string, std::string> WorkScaleSrc(const WorkScaleKind Kind, const size_t Count) {
	std::string Src, Expected;
	Lang::ID Lang = Lang::Cpp;
	const char *Rule = "Increment";
	switch(Kind) {
	case WorkScaleKind::CSharpCallChain:
		Lang = Lang::CSharp;
		Rule = "ExplicitReturn";
		Src = "class C{void F(){var x=a";
		for(size_t Idx = 0; Idx < Count; ++Idx) Src += ".F()";
		Expected = Src + ";return;}}";
		Src += ";}}";
		break;
	case WorkScaleKind::CallArgParens:
		Rule = "Parentheses";
		Src = Expected = "void f(){";
		for(size_t Idx = 0; Idx < Count; ++Idx) {
			Src += "(g(";
			Expected += "g(";
		}
		Src += "1";
		Expected += "1";
		for(size_t Idx = 0; Idx < Count; ++Idx) {
			Src += "))";
			Expected += ")";
		}
		Src += ";}";
		Expected += ";}";
		break;
	case WorkScaleKind::JavaBracedCallBranches:
	case WorkScaleKind::JavaCallBranches:
	case WorkScaleKind::PhpBracedCallBranches:
	case WorkScaleKind::PhpCallBranches:
		{
			const bool IsJava = Kind == WorkScaleKind::JavaBracedCallBranches || Kind == WorkScaleKind::JavaCallBranches;
			const bool IsBraced = Kind == WorkScaleKind::JavaBracedCallBranches || Kind == WorkScaleKind::PhpBracedCallBranches;
			Lang = IsJava ? Lang::Java : Lang::PHP;
			Rule = "ExplicitReturn";
			Src = IsJava ? "class C{void f(){" : "<?php function f(){";
			for(size_t Idx = 0; Idx < Count; ++Idx) {
				Src += IsJava ? "if(x)" : "if($x)";
				Src += IsBraced ? "{g();g();}else " : "g();else ";
			}
			Src += IsBraced ? "{h();h();}" : "h();";
			Expected = Src + (IsJava ? "return;}}" : "return;}");
			Src += IsJava ? "}}" : "}";
			break;
		}
	case WorkScaleKind::LeadingUnion:
		Lang = Lang::TypeScript;
		Rule = "TypeAssociation";
		Src = "type T=|";
		Expected = "type T=";
		for(size_t Idx = 0; Idx < Count; ++Idx) {
			if(Idx) {
				Src += "|";
				Expected += "|";
			}
			const std::string Name = "A" + std::to_string(Idx);
			Src += Name;
			Expected += Name;
		}
		Src += ";";
		Expected += ";";
		break;
	case WorkScaleKind::PostfixIncrement:
	case WorkScaleKind::PrefixIncrement:
		Src = Expected = "void f(){int i=0;";
		for(size_t Idx = 0; Idx < Count; ++Idx) {
			Src += Kind == WorkScaleKind::PostfixIncrement ? "i++;" : "++i;";
			Expected += "++i;";
		}
		Src += "return;}";
		Expected += "return;}";
		break;
	case WorkScaleKind::RustLocalMacros:
	case WorkScaleKind::RustForeignMacros:
		Lang = Lang::Rust;
		Rule = "Declarations";
		if(Kind == WorkScaleKind::RustForeignMacros) for(size_t Idx = 0; Idx < Count; ++Idx) {
			const std::string Function = "fn h" + std::to_string(Idx) + "(){println!(\"x\");}\n";
			Src += Function;
			Expected += Function;
		}
		Src += "fn f(){\n";
		Expected += "fn f(){\n";
		if(Kind == WorkScaleKind::RustLocalMacros) for(size_t Idx = 0; Idx < Count; ++Idx) {
			Src += "println!(\"x\");\n";
			Expected += "println!(\"x\");\n";
		}
		for(size_t Idx = 0; Idx < Count; ++Idx) {
			const std::string Item = "a" + std::to_string(Idx) + "=0;\n";
			Src += "let mut " + Item;
			Expected += "let " + Item;
		}
		Src += "g(a0);}";
		Expected += "g(a0);}";
		break;
	case WorkScaleKind::NestedBindingRefs:
		Lang = Lang::JavaScript;
		Rule = "Declarations";
		Src = "function f(y){let x=";
		Expected = "function f(y){const x=";
		for(size_t Idx = 0; Idx < Count; ++Idx) {
			Src += "y+(";
			Expected += "y+(";
		}
		Src += "y" + std::string(Count, ')') + ";}";
		Expected += "y" + std::string(Count, ')') + ";}";
		break;
	case WorkScaleKind::NestedParens:
		Lang = Lang::C;
		Rule = "Parentheses";
		for(size_t Idx = 0; Idx < Count; ++Idx) Src += Idx % 2 ? "b*(" : "a+(";
		Src += "x";
		Src.append(Count, ')');
		Src = "int f(int a,int b,int x){return " + Src + ";}";
		// 加算の右項の乗算は括弧を要さず，乗算の右項の加算は要する
		for(size_t Idx = 0; Idx < Count; ++Idx) Expected += !(Idx % 2) ? "a+" : Idx + 1 < Count ? "b*(" : "b*";
		Expected += "x";
		Expected.append(Count - 1 >> 1, ')');
		Expected = "int f(int a,int b,int x){return " + Expected + ";}";
		break;
	case WorkScaleKind::NestedStmt:
	case WorkScaleKind::NestedUnchangedStmt:
		Lang = Lang::Ruby;
		Rule = "StatementForms";
		for(size_t Idx = 0; Idx < Count; ++Idx) Src += "if ready\n";
		Src += Kind == WorkScaleKind::NestedStmt ? "g\n" : "g\nh\n";
		for(size_t Idx = 0; Idx < Count; ++Idx) Src += "end\n";
		if(Kind == WorkScaleKind::NestedUnchangedStmt) Expected = Src;
		else {
			// 後置形式は左結合なので，内側の条件から同じ順に並ぶ
			Expected = "g";
			for(size_t Idx = 0; Idx < Count; ++Idx) Expected += " if ready";
			Expected += "\n";
		}
		break;
	case WorkScaleKind::SplitDecls:
		Lang = Lang::C;
		Rule = "Declarations";
		for(size_t Idx = 0; Idx < Count; ++Idx) {
			const std::string Num = std::to_string(Idx);
			Src += "int a" + Num + ",b" + Num + ";\n";
			Expected += "int a" + Num + ";int b" + Num + ";\n";
		}
		break;
	case WorkScaleKind::MixedDecls:
	case WorkScaleKind::LongMixedDecl:
		Lang = Lang::TypeScript;
		Rule = "Declarations";
		Src = Expected = "function f(){";
		if(Kind == WorkScaleKind::LongMixedDecl) Src += "let ";
		for(size_t Idx = 0; Idx < Count; ++Idx) {
			const std::string Num = std::to_string(Idx);
			if(Kind == WorkScaleKind::MixedDecls) Src += "let ";
			else if(Idx) Src += ",";
			Src += "a" + Num + ":number=1,b" + Num + ":number=2";
			Expected += "const a" + Num + ":number=1;let b" + Num + ":number=2;";
			if(Kind == WorkScaleKind::MixedDecls) {
				Src += ";b" + Num + "=0;";
				Expected += "b" + Num + "=0;";
			}
		}
		if(Kind == WorkScaleKind::LongMixedDecl) {
			Src += ";";
			for(size_t Idx = 0; Idx < Count; ++Idx) {
				const std::string Write = "b" + std::to_string(Idx) + "=0;";
				Src += Write;
				Expected += Write;
			}
		}
		Src += "return;}";
		Expected += "return;}";
		break;
	}
	// 独立に構築した入力と手期待の返戻
	return { Lang, Rule, std::move(Src), std::move(Expected) };
}

/**
 * 構文の事実を含む観測の取得関数
 * 計算量：構文・固定定義・テキストの総量 Q に対し O(Q)
 * @param Syntax 観測する構文
 * @return 比較可能な観測
 * @throws std::bad_alloc 観測のメモリを確保出来ない場合
 * @throws std::length_error 観測の長さが上限を超える場合
 */
static std::string ObservationOf(const Core::SyntaxModel &Syntax) {
	// 作業状態の準備
	Core::CoreObservation Observation(Syntax);
	Observation.ObserveSyntax(Syntax, Core::RewriteTrace());
	// 観測の返戻
	return Observation.Take();
}

/**
 * 入力を倍増する個別の正規化規則の意味と作業量の検査関数
 * 計算量：固定した規模の投影・規則適用・照合の総費用 Q に対し O(Q)
 * @throws std::runtime_error 手期待又は計数の条件と異なる場合
 * @throws FormatLimitExceeded 呼出先の作業予算又は解析の前提を満たさない場合
 * @throws std::bad_alloc ソース又は構文のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckNormalizationWorkScale() {
	for(
		const WorkScaleKind Kind :
		{
			WorkScaleKind::CSharpCallChain,
			WorkScaleKind::CallArgParens,
			WorkScaleKind::JavaBracedCallBranches,
			WorkScaleKind::JavaCallBranches,
			WorkScaleKind::LeadingUnion,
			WorkScaleKind::LongMixedDecl,
			WorkScaleKind::MixedDecls,
			WorkScaleKind::NestedBindingRefs,
			WorkScaleKind::NestedParens,
			WorkScaleKind::NestedStmt,
			WorkScaleKind::NestedUnchangedStmt,
			WorkScaleKind::PhpBracedCallBranches,
			WorkScaleKind::PhpCallBranches,
			WorkScaleKind::PostfixIncrement,
			WorkScaleKind::PrefixIncrement,
			WorkScaleKind::RustForeignMacros,
			WorkScaleKind::RustLocalMacros,
			WorkScaleKind::SplitDecls
		}
	) {
		size_t PrevWork = 0;
		const bool IsBranchScale = Kind == WorkScaleKind::JavaBracedCallBranches || Kind == WorkScaleKind::JavaCallBranches ||
		Kind == WorkScaleKind::PhpBracedCallBranches || Kind == WorkScaleKind::PhpCallBranches;
		// 分岐連鎖の費用と呼出の深さに合わせ，手期待と倍増時の比を保つ小規模の入力にする
		const std::vector<size_t> Counts = IsBranchScale || Kind == WorkScaleKind::CallArgParens ?
		std::vector<size_t>{ 32, 64, 128 } :
		Kind == WorkScaleKind::CSharpCallChain ? std::vector<size_t>{ 256, 512, 1024, 2048 } : std::vector<size_t>{ 200, 400 };
		for(const size_t Count : Counts) {
			const std::tuple<Lang::ID, const char *, std::string, std::string> Input = WorkScaleSrc(Kind, Count);
			const ::Lang Lang = Lang::Get(std::get<0>(Input));
			Core::SyntaxModel Syntax = Project(Lang, std::get<2>(Input));
			const Core::SyntaxModel Oracle = Project(Lang, std::get<3>(Input));
			Check(
				!Syntax.GetNode(Syntax.GetRoot()).Facts.Has(Core::ConstraintKind::ContainsError) &&
				!Oracle.GetNode(Oracle.GetRoot()).Facts.Has(Core::ConstraintKind::ContainsError),
				"a review scale source or hand oracle was unreadable"
			);
			const size_t Before = Syntax.GetWorkAccount()->Used;
			Core::SyntaxNormalizerAccess::RunIndividualRule(Syntax, Core::SyntaxNormalizerAccess::GetFixtureRule(std::get<1>(Input)));
			const size_t Work = Syntax.GetWorkAccount()->Used - Before;
			Check(GetOracleLexemes(Syntax) == GetOracleLexemes(Oracle), "a review scale result differs from its handwritten oracle");
			Check(GetOracleBranches(Syntax) == GetOracleBranches(Oracle), "a review scale result changed condition or body ownership");
			std::fprintf(stdout, "review scale kind=%u n=%zu work=%zu\n", static_cast<unsigned>(Kind), Count, Work);
			Check(Work && (!PrevWork || Work <= 3 * PrevWork), "review work grows quadratically");
			PrevWork = Work;
		}
	}
	// 既知の計数を持つ木で，接続・切離・種別の名前付の変化を確認する
	{
		Core::SyntaxModel Syntax = Project(Lang::Get(Lang::JSON), "[1,2]\n");
		const Core::NodeId Array = Syntax.GetNamedChild(Syntax.GetRoot(), 0);
		const Core::NodeId First = Syntax.GetNamedChild(Array, 0), Second = Syntax.GetNamedChild(Array, 1);
		const Core::FormPart Number = Syntax.GetGenerationPartOf(First);
		const Core::FormPart Punctuation = Syntax.GetGenerationPartOf(Syntax.GetChild(Array, 2));
		const auto CheckCounts = [&]() -> void {
			for(Core::NodeId Id = 0; Id < Syntax.GetNodeCount(); ++Id) {
				uint32_t Expected = 0;
				for(const Core::NodeId Child : Syntax.GetChildrenOf(Id)) Expected += static_cast<uint32_t>(Syntax.IsNamed(Child));
				Check(Syntax.GetNamedChildCount(Id) == Expected, "an edit left a stale named child count");
			}
			// 全ての直接の子の照合の終了
			return;
		};
		CheckCounts();
		for(const bool IsNamed : { false, true }) {
			Core::RewriteStep Change { Core::RewriteKind::Reshape, First };
			Change.Inserted.Def = IsNamed ? Number : Punctuation;
			Change.Inserted.IsNamed = IsNamed;
			Change.Inserted.Lexeme = IsNamed ? "1" : ",";
			Core::RewritePlan Plan;
			Plan.Steps.push_back(Change);
			Syntax.Apply(Plan);
			CheckCounts();
		}
		for(const Core::NodeId Parent : { Syntax.GetRoot(), Array }) {
			Core::RewriteStep Move { Core::RewriteKind::Move, Second };
			Move.Dst = Parent;
			Move.Idx = Syntax.GetChildCount(Parent) - static_cast<uint32_t>(Parent == Array);
			Core::RewritePlan Plan;
			Plan.Steps.push_back(Move);
			Syntax.Apply(Plan);
			CheckCounts();
		}
		Core::RewriteStep Insert { Core::RewriteKind::Insert, Array };
		Insert.Idx = 0;
		Insert.Inserted.Def = Number;
		Insert.Inserted.IsNamed = true;
		Insert.Inserted.Lexeme = "3";
		Core::RewritePlan Plan;
		Plan.Steps.push_back(Insert);
		Syntax.Apply(Plan);
		CheckCounts();
		const Core::NodeId Added = Syntax.GetChild(Array, 0);
		Core::RewriteStep Wrap { Core::RewriteKind::Wrap, Added };
		Wrap.Inserted.Def = Syntax.GetGenerationPartOf(Array);
		Wrap.Inserted.IsNamed = true;
		Wrap.Inserted.Children = { Core::GeneratedNode{ .Existing = Added } };
		Plan.Steps = { Wrap };
		Syntax.Apply(Plan);
		CheckCounts();
		Core::RewriteStep Unwrap { Core::RewriteKind::Unwrap, Syntax.GetChild(Array, 0) };
		Plan.Steps = { Unwrap };
		Syntax.Apply(Plan);
		CheckCounts();
		Core::RewriteStep Rebuild { Core::RewriteKind::RebuildChildren, Array };
		Rebuild.Inserted.Children = {
			Core::GeneratedNode{ .Existing = First },
			Core::GeneratedNode{ .Existing = Second },
			Core::GeneratedNode{ .Existing = Added }
		};
		Plan.Steps = { Rebuild };
		Syntax.Apply(Plan);
		CheckCounts();
		Core::RewriteStep Remove { Core::RewriteKind::RemoveSiblings, Array };
		Remove.RemovedSiblings = { { First, {}, {}, {} }, { Second, {}, {}, {} } };
		Plan.Steps = { Remove };
		Syntax.Apply(Plan);
		CheckCounts();
		Core::RewriteStep RemoveOne { Core::RewriteKind::Remove, Added };
		Plan.Steps = { RemoveOne };
		Syntax.Apply(Plan);
		CheckCounts();
	}
	// 引用符の正規化と，同じ親への繰返す計数を文字列の配列で倍増して照合する
	for(const Lang::ID Language : { Lang::JavaScript, Lang::TypeScript, Lang::Ruby, Lang::Python }) {
		size_t Previous = 0;
		for(const size_t Count : { 250, 500, 1000, 2000 }) {
			std::string Src = Language == Lang::JavaScript || Language == Lang::TypeScript ? "const H=[\n" : "H=[\n";
			for(size_t Idx = 0; Idx < Count; ++Idx) {
				if(Idx) Src += ",\n";
				Src += "\"a" + std::to_string(Idx) + "\"";
			}
			Src += "];\n";
			Core::SyntaxModel Syntax = Project(Lang::Get(Language), Src);
			Core::NodeId Widest = Syntax.GetRoot();
			for(Core::NodeId Id = 0; Id < Syntax.GetNodeCount(); ++Id) if(Syntax.GetChildCount(Id) > Syntax.GetChildCount(Widest)) {
				Widest = Id;
			}
			const size_t Before = Syntax.GetWorkAccount()->Used;
			const uint32_t Named = Syntax.GetNamedChildCount(Widest);
			Check(Named == Count, "a string array lost a named element");
			#if !defined(ShaveFormatRefFacts)
			for(size_t Idx = 0; Idx < Count; ++Idx) Check(Syntax.GetNamedChildCount(Widest) == Count, "a repeated child count changed");
			#endif
			const size_t Queries = Syntax.GetWorkAccount()->Used - Before, NormalizeBefore = Syntax.GetWorkAccount()->Used;
			Core::SyntaxNormalizerAccess::Run(Syntax);
			const size_t NormalizeWork = Syntax.GetWorkAccount()->Used - NormalizeBefore;
			Check(Syntax.GetNamedChildCount(Widest) == Count, "quote normalization changed the array's named child count");
			std::printf("string array lang=%d n=%zu child_queries=%zu normalization=%zu\n", Language, Count, Queries, NormalizeWork);
			#if !defined(ShaveFormatRefFacts)
			const size_t Work = Queries + NormalizeWork;
			Check(!Previous || Work <= 3 * Previous, "string array work grows quadratically");
			Previous = Work;
			#else
			static_cast<void>(Previous);
			#endif
		}
	}
	// 終了
	return;
}

/**
 * 入子の関数の宣言・返戻と可視名の共有の作業量の検査関数
 * 計算量：固定した規模の投影・規則適用・照合の総費用 Q に対し O(Q)
 * @throws std::runtime_error 手期待又は計数の条件と異なる場合
 * @throws FormatLimitExceeded 呼出先の作業予算又は解析の前提を満たさない場合
 * @throws std::bad_alloc ソース又は構文のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckVisibleScopeWork() {
	// 入子の本体は自身の宣言だけを収集し，呼出又はローカル変数の条件は外側の表を要しない
	for(const Lang::ID Language : { Lang::CSharp, Lang::Java }) for(const bool UsesLocal : { false, true }) {
		size_t PreviousNames = 0, PreviousNormalization = 0;
		std::vector<std::pair<std::string_view, size_t>> PreviousRules;
		for(const size_t Count : { 8U, 16U, 32U, 64U, 128U }) {
			const bool IsJava = Language == Lang::Java;
			const std::string Type = IsJava ? "boolean" : "bool";
			std::string Src = "class C { " + Type + " Get(){return false;} void Use(){} void F0(){";
			for(size_t Idx = 0; Idx < Count; ++Idx) {
				const std::string Name = "c" + std::to_string(Idx);
				if(UsesLocal) Src += Type + " " + Name + "=Get();while(" + Name + "){}";
				else Src += "while(Get()){}";
				if(IsJava) Src += "class L" + std::to_string(Idx) + "{";
				Src += "void F" + std::to_string(Idx + 1) + "(){";
			}
			Src += "Use();" + std::string((IsJava ? Count << 1 : Count) + 2, '}');
			Core::SyntaxModel Syntax = Project(Lang::Get(Language), Src);
			Syntax.BuildRoleIdx();
			const std::vector<Core::NodeId> Functions = GetNodesWithRole(Syntax, Core::Role::ExplicitResultFunction);
			Check(Functions.size() == Count + 3, "nested declarations lost their independent functions");
			const size_t Names = Core::SyntaxNormalizerAccess::GetVisibleNameWork(Syntax, Functions);
			Check(Names && (!PreviousNames || Names <= 3 * PreviousNames), "visible declaration work grows quadratically");
			PreviousNames = Names;
			const Core::ControlFlow Flow(Syntax);
			for(const Core::NodeId Function : Functions) {
				if(!Syntax.GetLexemeOf(Syntax.GetFieldChild(Function, Core::ChildRole::Name)).starts_with("F")) continue;
				const std::optional<Core::FunctionEnd> End = Flow.ClassifyFunction(Function);
				Check(End.has_value(), "a nested function lost its return shape");
				Check(Flow.EndReach(Function, *End) == Core::EndReachability::Reachable, "a nonconstant loop closed the nested function");
			}
			std::vector<std::pair<std::string_view, size_t>> RuleWork;
			const size_t Work = Core::SyntaxNormalizerAccess::Run(Syntax, &RuleWork).Work;
			// 規則別の増加も検査し，他の規則の固定費で２乗の増加を隠さない
			if(!PreviousRules.empty()) {
				Check(RuleWork.size() == PreviousRules.size(), "nested normalization changed its rule sequence");
				for(size_t Idx = 0; Idx < RuleWork.size(); ++Idx) {
					Check(RuleWork[Idx].first == PreviousRules[Idx].first, "nested normalization changed its rule order");
					Check(RuleWork[Idx].second <= 3 * PreviousRules[Idx].second, "a nested normalization rule grows quadratically");
				}
			}
			PreviousRules = std::move(RuleWork);
			if(IsJava && UsesLocal) {
				const std::vector<Core::NodeId> Declarations = GetNodesWithRole(Syntax, Core::Role::FinalizableLocal);
				Check(Declarations.size() == Count, "nested local declarations lost their expected count");
				for(const Core::NodeId Declaration : Declarations) {
					const Core::NodeId Modifiers = Syntax.GetFirstNamedChildWithRole(Declaration, Core::Role::ModifierSeq);
					Check(Syntax.HasWordChild(Modifiers, Core::Word::ImmutableBinding), "a nested unchanged binding lost its final modifier");
				}
			}
			Check(Work && (!PreviousNormalization || Work <= 3 * PreviousNormalization), "nested normalization work grows quadratically");
			PreviousNormalization = Work;
		}
	}
	// 終了
	return;
}

/**
 * 呼出先の表の所有と，独立・入子の関数の束縛の作業量の検査関数
 * 計算量：固定した規模の投影・照会・規則適用の総費用 Q に対し O(Q)
 * @throws std::runtime_error 共有・失効又は線形の作業量の条件と異なる場合
 * @throws FormatLimitExceeded 投影又は束縛の作業予算を超えた場合
 * @throws std::bad_alloc ソース・構文又は照会のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error ソース又は構文の要素数上限を超える場合
 * @throws std::out_of_range 照会する識別子が表に無い場合
 */
static void CheckCallableScopeWork() {
	for(const Lang::ID Language : { Lang::Cpp, Lang::JavaScript, Lang::TypeScript, Lang::Python }) {
		size_t Previous = 0;
		for(const size_t Count : { 64U, 128U, 256U, 512U }) {
			std::string Src;
			for(size_t Idx = 0; Idx < Count; ++Idx) {
				const std::string Name = "F" + std::to_string(Idx);
				if(Language == Lang::Python) Src += "def " + Name + "(x):\n x = G(x)\n return x\n";
				else {
					Src +=
					(Language == Lang::Cpp ? "int " : "function ") + Name + (Language == Lang::Cpp ? "(int x)" : "(x)") + "{x=G(x);return x;}\n";
				}
			}
			Core::SyntaxModel Syntax = Project(Lang::Get(Language), Src);
			const std::vector<Core::NodeId> Calls = GetNodesWithRole(Syntax, Core::Role::CallNode);
			Check(Calls.size() == Count, "a callable scope fixture lost its assignments or calls");
			const size_t Built = Core::SyntaxNormalizerAccess::GetFreshCalleeWork(Syntax, Syntax.GetRoot());
			Check(Built >= Syntax.GetNodeCount(), "a callee subtree was not accounted");
			#if !defined(ShaveFormatRefFacts)
			Check(
				Core::SyntaxNormalizerAccess::GetFreshCalleeWork(Syntax, Syntax.GetRoot()) == 1,
				"temporary flows rebuilt shared callee facts"
			);
			#endif
			const size_t Before = Syntax.GetWorkAccount()->Used;
			for(const Core::NodeId Call : Calls) {
				const Core::ControlFlow Flow(Syntax);
				Core::SyntaxNormalizerAccess::GetCallResolution(Flow, Call);
			}
			const size_t Work = Built + Syntax.GetWorkAccount()->Used - Before;
			Check(Work >= Syntax.GetNodeCount(), "the shared callee table initialization was not charged");
			Check(!Previous || Work <= 3 * Previous, "doubling callable scopes multiplied whole-model initialization");
			std::printf("callable scopes lang=%u n=%zu work=%zu\n", static_cast<unsigned>(Language), Count, Work);
			Previous = Work;
		}
	}
	for(const Lang::ID Language : { Lang::JavaScript, Lang::TypeScript }) {
		size_t Previous = 0;
		for(const size_t Count : { 32U, 64U, 128U, 256U }) {
			std::string Src = "function Outer(){", Expected = Src;
			for(size_t Idx = 0; Idx < Count; ++Idx) {
				const std::string Head = "function F" + std::to_string(Idx) + "(x){x=G(x);H(x);";
				Src += Head + "}";
				Expected += Head + "return;}";
			}
			Src += "H();}";
			Expected += "H();return;}";
			Core::SyntaxModel Syntax = Project(Lang::Get(Language), Src);
			const Core::SyntaxModel Oracle = Project(Lang::Get(Language), Expected);
			const size_t Before = Syntax.GetWorkAccount()->Used;
			Core::SyntaxNormalizerAccess::RunIndividualRule(Syntax, Core::SyntaxNormalizerAccess::GetFixtureRule("ExplicitReturn"));
			const size_t Work = Syntax.GetWorkAccount()->Used - Before;
			Check(GetOracleLexemes(Syntax) == GetOracleLexemes(Oracle), "nested return completion changed its hand oracle");
			Check(Work && (!Previous || Work <= 3 * Previous), "return insertion repeatedly invalidated unrelated binding sources");
			std::printf("nested callable scopes lang=%u n=%zu work=%zu\n", static_cast<unsigned>(Language), Count, Work);
			Previous = Work;
		}
	}
	// 終了
	return;
}

/**
 * Rust の末尾の名前を，束縛先の場所と一時値に区別する検査関数
 * 計算量：固定した規模の投影・規則適用・照合の総費用 Q に対し O(Q)
 * @throws std::runtime_error 末尾の補完が寿命保存の手期待と異なる場合
 * @throws FormatLimitExceeded 投影又は正規化の作業予算を超えた場合
 * @throws std::bad_alloc ソース又は構文のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error ソース又は構文の要素数上限を超える場合
 * @throws std::out_of_range 照会する識別子が表に無い場合
 */
static void CheckTailBindingTemps() {
	const std::string Prelude =
	"struct D(u8);impl Drop for D{fn drop(&mut self){return;}}struct Unit;const TEMP:D=D(1);const ARRAY:[D;1]=[D(1)];enum E{Unit}impl D{const ASSOCIATED:D=D(1);}mod m{pub const TEMP:super::D=super::D(1);}use m::TEMP as ALIAS;fn Use<T>(_:&T)->i32{return 0;}";
	for(
		const std::string_view Value :
		{ "ALIAS", "ARRAY[0]", "D::ASSOCIATED", "E::Unit", "TEMP", "TEMP.0", "UNKNOWN", "Unit", "m::TEMP" }
	) for(const size_t Layers : { 0U, 1U, 2U }) {
		const std::string Val = std::string(Layers, '(') + std::string(Value) + std::string(Layers, ')');
		const std::string Src = Prelude + "fn F(arg:D)->i32{let guard=arg;Use(&" + Val + ")}";
		Core::SyntaxModel Syntax = Project(Lang::Get(Lang::Rust), Src);
		const Core::SyntaxModel Oracle = Project(Lang::Get(Lang::Rust), Src);
		Core::SyntaxNormalizerAccess::RunIndividualRule(Syntax, Core::SyntaxNormalizerAccess::GetFixtureRule("ExplicitReturn"));
		Check(
			GetOracleLexemes(Syntax) == GetOracleLexemes(Oracle),
			"a constant or unresolved path certified tail temporary lifetime",
			Src.c_str()
		);
	}
	for(const std::string_view Value : { "arg", "guard", "guard.0", "guard[0]" }) {
		const std::string Type = Value == "arg" ? "&D" : Value == "guard[0]" ? "[D;1]" : "D";
		const std::string Head = Prelude + "fn F(arg:" + Type + ")->i32{let guard=arg;";
		const std::string Src = Head + "Use(&" + std::string(Value) + ")}";
		const std::string Expected = Head + "return Use(&" + std::string(Value) + ");}";
		Core::SyntaxModel Syntax = Project(Lang::Get(Lang::Rust), Src);
		const Core::SyntaxModel Oracle = Project(Lang::Get(Lang::Rust), Expected);
		Core::SyntaxNormalizerAccess::RunIndividualRule(Syntax, Core::SyntaxNormalizerAccess::GetFixtureRule("ExplicitReturn"));
		Check(
			GetOracleLexemes(Syntax) == GetOracleLexemes(Oracle),
			"a resolved local place lost safe tail return completion",
			Src.c_str()
		);
	}
	// 文法が見逃す成立条件も，入力と完成候補の各々から同じ手期待で確かめる
	struct AdmissionCase {
		const char *Lang;
		const char *Src;
		bool IsAccepted;
	};
	const AdmissionCase Admissions[] {
		{ "c", "int F { return 1; }", false },
		{ "c", "int (*F) {return 1;}", false },
		{ "c", "int F[2] {return 1;}", false },
		{ "c", "class A {int x;};", false },
		{ "c", "int F(void){return 1;}", true },
		{ "c", "int (*F(void))(void){return 0;}", true },
		{ "c", "#define F f()\nint F {return 1;}", true },
		{ "c", "#define F 1\nint F {return 1;}", false },
		{ "c", "#define G f()\n#define F G\nint F {return 1;}", true },
		{ "cpp", "int &F {return 1;}", false },
		{ "cpp", "struct C{operator int(){return 1;} C(){} ~C(){}};", true },
		{ "h", "int F {return 1;}", false },
		{ "h", "class A {int x;};", true },
		{ "json", "// c\n{\"a\":1}", false },
		{ "json", "[1,]", false },
		{ "json", "{\"a\":\"// c /* c */\"}", true },
		{ "jsonc", "// c\n{\"a\":1}", true },
		{ "jsonc", "[1,]", true },
		{ "css", ".a{content:\"\v\";}", true },
		{ "css", ".a{\vcontent:\"x\";}", false },
		{ "scss", ".a{content:\"\v\";}", true },
		{ "scss", ".a{\vcontent:\"x\";}", false }
	};
	for(const AdmissionCase &Fixture : Admissions) for(const bool IsVerification : { false, true }) {
		const GrammarProfile &Profile = GrammarProfile::Resolve(Lang::FromName(Fixture.Lang));
		const std::string Src = Fixture.Src;
		InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, Src);
		ParseInput Input = ParseAdapter::Read(Profile, Normalized);
		if(IsVerification) Input.Origin = ParseInput::VerificationSrc{ Src };
		Check(Input.Decision == ParseDecision::Continue, "an admission fixture stopped before parsing", Fixture.Src);
		const ParsedTree Parsed = SyntaxParser::Parse(*Input.Profile, Input);
		Check(
			(Parsed.GetParseDecision() == ParseDecision::Continue) == Fixture.IsAccepted,
			"declarator or dialect admission differs",
			Fixture.Src
		);
		if(!Fixture.IsAccepted) {
			Check(
				Parsed.GetParseFailure() == FailureReason::InputSyntaxError,
				"a malformed definition or dialect lost its input error",
				Fixture.Src
			);
		}
	}
	// 終了
	return;
}

/**
 * 構文受理の事実と文の改行の共通走査の作業量の検査関数
 * 計算量：固定した規模の投影・規則適用・照合の総費用 Q に対し O(Q)
 * @throws std::runtime_error 手期待又は計数の条件と異なる場合
 * @throws FormatLimitExceeded 呼出先の作業予算又は解析の前提を満たさない場合
 * @throws std::bad_alloc ソース又は構文のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckParseAdmissionWork() {
	// 受理の事実は親子表の同じ訪問で求め，改行の判定は原文順の葉を使う
	{
		const GrammarProfile &Profile = GrammarProfile::Resolve(Lang::Get(Lang::Python));
		size_t PreviousVisits = 0, PreviousLeaves = 0, PreviousIndentation = 0;
		for(const size_t Count : { 16U, 32U, 64U }) {
			std::string Text;
			for(size_t Idx = 0; Idx < Count; ++Idx) {
				Text += "def f" + std::to_string(Idx) + "():\n    x = (1 +\n         2)\n    return x\n";
			}
			ParseInput Input;
			Input.Profile = &Profile;
			Input.Src.emplace(Profile, Text);
			Input.Origin = ParseInput::FormattingSrc{ Text };
			const ParsedTree Parsed = SyntaxParser::Parse(Profile, Input);
			Check(Parsed.GetParseDecision() == ParseDecision::Continue, "cached admission rejected a valid multiline statement");
			const std::array<size_t, 5> Metrics = ParsedTreeAccess::GetAdmissionMetrics(Parsed);
			Check(
				Metrics[0] == Parsed.GetRelationWork() + 1 && Metrics[1] && Metrics[1] < Metrics[0] && Metrics[4] == Metrics[1],
				"admission facts repeated a whole-tree scan"
			);
			Check(!PreviousVisits || Metrics[0] <= 3 * PreviousVisits, "shared admission visits exceeded linear growth");
			Check(!PreviousLeaves || Metrics[1] <= 3 * PreviousLeaves, "statement admission leaves exceeded linear growth");
			const size_t Indentation = ParsedTreeAccess::GetIndentationWork(Parsed);
			Check(
				Indentation && (!PreviousIndentation || Indentation <= 3 * PreviousIndentation),
				"input indentation reads exceeded linear growth"
			);
			PreviousIndentation = Indentation;
			PreviousVisits = Metrics[0];
			PreviousLeaves = Metrics[1];
		}
	}
	// 宣言子の受理を繰返しても木を再走査せず，候補数に比例する作業を保つ
	for(const Lang Lang : { Lang::Get(Lang::C), Lang::Get(Lang::Cpp) }) {
		const GrammarProfile &Profile = GrammarProfile::Resolve(Lang);
		size_t PreviousWork = 0;
		for(const size_t Count : { 16U, 32U, 64U, 128U }) {
			std::string Text;
			for(size_t Idx = 0; Idx < Count; ++Idx) Text += "int F" + std::to_string(Idx) + "(void){return 1;}\n";
			const ParsedTree Parsed(Profile, Text);
			Check(!SyntaxParser::CountMalformedCDefs(Parsed), "declarator admission rejected ordinary functions", Text.c_str());
			const size_t Work = ParsedTreeAccess::GetAdmissionMetrics(Parsed)[0], BudgetWork = Parsed.GetNameWork();
			Check(BudgetWork, "declarator admission lost its product budget charge", Text.c_str());
			Check(!PreviousWork || Work <= 3 * PreviousWork, "declarator admission repeated whole-tree work", Text.c_str());
			Check(!SyntaxParser::CountMalformedCDefs(Parsed), "cached declarator facts changed their result", Text.c_str());
			Check(
				ParsedTreeAccess::GetAdmissionMetrics(Parsed)[0] == Work,
				"repeated declarator admission rescanned the tree",
				Text.c_str()
			);
			Check(Parsed.GetNameWork() == BudgetWork, "cached declarator admission charged twice", Text.c_str());
			PreviousWork = Work;
		}
	}
	// コメント末尾のバックスラッシュと，正規のコードの行継続を区別する
	struct BreakFixture {
		const char *Text; // 調べる論理行と見出
		bool Accepted; // 処理系が受理するか
	};
	const BreakFixture Fixtures[] = {
		{ "x = # c\\\n1\n", false },
		{ "x = 1 + # c\\\n2\n", false },
		{ "x = - # c\\\n2\n", false },
		{ "if True # c\\\n:\n    pass\n", false },
		{ "while True # c\\\n:\n    break\n", false },
		{ "for x in # c\\\nxs:\n    pass\n", false },
		{ "def f # c\\\n():\n    pass\n", false },
		{ "class C # c\\\n:\n    pass\n", false },
		{ "import # c\\\nos\n", false },
		{ "from os # c\\\nimport path\n", false },
		{ "assert True, # c\\\n'message'\n", false },
		{ "def f():\n    return 1 + # c\\\n    2\n", false },
		{ "x = 1 + # c\\\n# other\\\n2\n", false },
		{ "x = 1 + \\\n2\n", true },
		{ "x = (1 + # c\\\n2)\n", true }
	};
	const GrammarProfile &Profile = GrammarProfile::Resolve(Lang::Get(Lang::Python));
	for(const BreakFixture &Fixture : Fixtures) for(const bool CrLf : { false, true }) {
		std::string Text;
		for(const char Ch : std::string_view(Fixture.Text)) {
			if(CrLf && Ch == '\n') Text += '\r';
			Text += Ch;
		}
		InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, Text);
		ParseInput Input = ParseAdapter::Adapt(Profile, Normalized);
		ParseDecision Decision = Input.Decision;
		if(Decision == ParseDecision::Continue) Decision = SyntaxParser::Parse(*Input.Profile, Input).GetParseDecision();
		Check(
			(Decision == ParseDecision::Continue) == Fixture.Accepted,
			"statement admission confused comment text with a code continuation",
			Text.c_str()
		);
	}
	// 不正な所属の１９形と，コメント・引用・継続・深さの正規の対照を同じ受理経路で確かめる
	const BreakFixture Indentation[] {
		{ "x = 1, \\\n        2\n", true },
		{ "for x in 1, \\\n        2:\n    pass\n", true },
		{ "    x=1\n", false },
		{ "x=1\n    y=2\n", false },
		{ "if True:\n    x=1\n  y=2\n", false },
		{ "if True:\n    x=1\n     y=2\n", false },
		{ "if True:\n\tx=1\n        y=2\n", false },
		{ "if True:\n    x=1\n\ty=2\n", false },
		{ "def F():\n x=1\n  return x\n", false },
		{ "class A:\n x=1\n  y=2\n", false },
		{ "for x in X:\n    f()\n     g()\n", false },
		{ "while P:\n    f()\n     g()\n", false },
		{ "with M():\n    f()\n     g()\n", false },
		{ "try:\n    f()\n     g()\nexcept E:\n    h()\n", false },
		{ "try:\n    f()\nexcept E:\n    g()\n     h()\n", false },
		{ "try:\n    f()\nfinally:\n    g()\n     h()\n", false },
		{ "if P:\n    f()\nelse:\n    g()\n     h()\n", false },
		{ "if P:\n    f()\nelif Q:\n    g()\n     h()\n", false },
		{ "match x:\n    case 1:\n        f()\n         g()\n", false },
		{ "async def F():\n    f()\n     g()\n", false },
		{ "if P:\n    f()\n    # anchor\n     g()\n", false },
		{ "if True:\n    x=1\n    y=2\nz=3\n", true },
		{ "if True:\n\tx=1\n\tif True:\n\t\ty=2\n\tz=3\n", true },
		{ "if True:\n x=1\n if True:\n       y=2\n z=3\nif True:\n           x=4\n", true },
		{ "    # first\nif True:\n# shallow\n    x=1\n              # deep\n    y=2\n", true },
		{ "if True:\n    x=1\n \t \n\n    y=2\n", true },
		{ "if True:\n    x=(1+\n        2)\n    y=[\n1,\n    2]\n", true },
		{ "if True:\n    x=1+\\\n2\n    y=2\n", true },
		{ "if True:\n    \\\nx=1\n    y=2\n", true },
		{ "if True:\n\\\n    x=1\n    y=2\n", true },
		{ "if True:\n  \\\n       \\\nx=1\n  y=2\n", true },
		{ "if True:\n    x=1\n\t\f    y=2\n", true },
		{ "if True:\n\t x=1\n\t if True:\n\t\t y=2\n\t z=3\n", true },
		{ "if True: x=1; y=2\nif False: z=3\n", true },
		{ "if True:\n    x=\"\"\"first\n    second\nzero\n\"\"\"\n    y=2\n", true },
		{ "if True:\n    x=\"\"\"first\n\\n\nlast\"\"\"\n    y=2\n", true },
		{ "if True:\n    x=\"\"\"first\n\\n\nlast\"\"\"\n     y=2\n", false },
		{ "x=f\"\"\"first\n\v\\n\n{1+\n2}\nlast\"\"\"\n", true },
		{ "x=f\"\"\"first\n{1\v+2}\nlast\"\"\"\n", false },
		{ "if True:\n    x=f\"\"\"first {\n1+2\n} last\"\"\"\n    y=2\n", true },
		{ "match x:\n  case 1:\n      x=1\n  case 2:\n      x=2\n", true },
		{ "@decorator\ndef f():\n  pass\n", true },
		{ "if (\nTrue\n):\n    pass\n", true },
		{ "if \\\n True:\n    pass\n", true },
		{ "\t\fx=1\n", true },
		{ "if True:\n\tx=1\n\tif True:\n\t\ty=2\n        z=3\n", false },
		{ "x=lambda:\n    1\n", false }
	};
	for(const BreakFixture &Fixture : Indentation) {
		for(const bool IsVerification : { false, true }) for(const bool CrLf : { false, true }) {
			std::string Text;
			for(const char Char : std::string_view(Fixture.Text)) {
				if(CrLf && Char == '\n') Text += '\r';
				Text += Char;
			}
			InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, Text);
			ParseInput Input = ParseAdapter::Read(Profile, Normalized);
			if(IsVerification) Input.Origin = ParseInput::VerificationSrc{ Text };
			Check(Input.Decision == ParseDecision::Continue, "an indentation fixture failed before admission", Fixture.Text);
			const ParsedTree Parsed = SyntaxParser::Parse(*Input.Profile, Input);
			Check(
				(Parsed.GetParseDecision() == ParseDecision::Continue) == Fixture.Accepted,
				"indentation admission differs from the hand expectation",
				Fixture.Text
			);
			if(!Fixture.Accepted) {
				Check(
					Parsed.GetParseFailure() == FailureReason::InputSyntaxError,
					"invalid indentation lost its input-syntax classification",
					Fixture.Text
				);
			}
		}
	}
	// 終了
	return;
}

/**
 * 空白・コメントの共有事実と入力側・検証側の投影の作業量の検査関数
 * 計算量：固定した規模の投影・規則適用・照合の総費用 Q に対し O(Q)
 * @throws std::runtime_error 手期待又は計数の条件と異なる場合
 * @throws FormatLimitExceeded 呼出先の作業予算又は解析の前提を満たさない場合
 * @throws std::bad_alloc ソース又は構文のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckProjectionWork() {
	// 短い空白の照会は一定長迄に留め，共有トークン表の再検索を省く
	{
		const GrammarProfile &Profile = GrammarProfile::Resolve(Lang::Get(Lang::JSON));
		size_t PreviousSpaceReads = 0;
		for(const size_t Count : { 32U, 64U, 128U }) {
			std::string Text = "[";
			for(size_t Idx = 0; Idx < Count; ++Idx) Text += Idx ? ", 1" : "1";
			Text += "]";
			InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, Text);
			ParseInput Input = ParseAdapter::Read(Profile, Normalized);
			Input.Origin = ParseInput::FormattingSrc{ Normalized.Text };
			const ParsedTree Parsed = SyntaxParser::Parse(Profile, Input);
			size_t FactWork = 0;
			std::array<size_t, 3> Hidden {};
			std::array<size_t, 2> GapSearches {};
			const Core::ProjectionResult Projection = Core::SourceProjectorAccess::ProjectWithWork(
				Parsed,
				Profile,
				Input,
				FactWork,
				&Hidden,
				nullptr,
				nullptr,
				nullptr,
				nullptr,
				&GapSearches
			);
			Check(Projection.Syntax.GetNodeCount() > Count, "spaced input lost its syntax");
			Check(Hidden[2] == 1 && Hidden[0] == Parsed.size(), "short gaps repeated tokenization");
			Check(GapSearches[0] && GapSearches[1] < Hidden[1], "short spaces still searched the token table");
			Check(GapSearches[0] <= Hidden[1] << 4, "gap lookahead exceeded its fixed bound");
			Check(!PreviousSpaceReads || GapSearches[0] <= 3 * PreviousSpaceReads, "gap lookahead exceeded linear growth");
			PreviousSpaceReads = GapSearches[0];
		}
	}
	// コメントの無い木は世代毎に１度だけ調べ，其の後の分離・ディレクティブ収集では木を辿らない
	{
		const GrammarProfile &Profile = GrammarProfile::Resolve(Lang::Get(Lang::JSON));
		size_t PreviousFactWork = 0, PreviousHiddenQueries = 0, PreviousEdgeReads = 0, PreviousMatches = 0, PreviousGrowth = 0;
		size_t PreviousParentWork = 0, PreviousNodeBytes = 0;
		std::shared_ptr<Core::ProjectionResult> ExpectedProjection;
		for(const size_t Count : { 32U, 64U, 128U }) for(const bool IsVerification : { false, true }) {
			std::string Src = "{\"items\":[";
			for(size_t Idx = 0; Idx < Count; ++Idx) {
				if(Idx) Src += ',';
				Src += "{\"name\":\"// text /* text */\",\"values\":[\"left\",\"right\"]}";
			}
			Src += "]}\n";
			InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, Src);
			ParseInput Input = ParseAdapter::Read(Profile, Normalized);
			Input.Origin = ParseInput::FormattingSrc{ Normalized.Text };
			if(IsVerification) Input.Origin = ParseInput::VerificationSrc{ Src };
			Check(Input.Decision == ParseDecision::Continue && ParseAdapter::CanRepair(Input), "comment-free input failed adaptation");
			const ParsedTree Parsed = SyntaxParser::Parse(*Input.Profile, Input);
			Check(Parsed.GetParseDecision() == ParseDecision::Continue, "comment-free input failed parsing");
			const uint32_t End = ParseAdapter::GetOuterTextBounds(Parsed).second;
			Check(Parsed.CollectCommentRanges(End).empty(), "string contents were collected as gap comments");
			const size_t Visits = ParsedTreeAccess::GetCommentVisits(Parsed);
			const std::array<size_t, 4> ParentStorage = ParsedTreeAccess::GetParentStorage(Parsed);
			Check(
				ParentStorage[0] == Visits && ParentStorage[1] == Visits && ParentStorage[2] >= Visits << 1 && ParentStorage[2] < Visits << 2,
				"parent storage lost a node or exceeded its flat capacity"
			);
			Check(
				ParentStorage[3] >= Visits && (!PreviousParentWork || ParentStorage[3] <= 3 * PreviousParentWork),
				"flat parent insertion work exceeded linear growth"
			);
			PreviousParentWork = ParentStorage[3];
			Check(
				Visits && Visits <= static_cast<size_t>(ts_node_descendant_count(Parsed.GetRoot())) + 1,
				"comment facts exceeded one tree scan"
			);
			Check(Visits == Parsed.GetRelationWork() + 1, "comment facts used a separate cursor from parent relations");
			Check(!Core::SourceProjectorAccess::GetCommentWork(Parsed, Input), "comment-free phases still traverse the tree");
			size_t FactWork = 0;
			std::array<size_t, 3> Hidden {};
			std::array<size_t, 2> Facts {};
			size_t CtxVisits = 0;
			Core::ProjectionResult Projection =
			Core::SourceProjectorAccess::ProjectWithWork(Parsed, *Input.Profile, Input, FactWork, &Hidden, &Facts, &CtxVisits);
			Check(!CtxVisits, "error-free projection repeated its context scan");
			const std::array<size_t, 3> NodeStorage = Core::SyntaxNormalizerAccess::GetNodeStorage(Projection.Syntax);
			Check(
				NodeStorage[0] && NodeStorage[1] && NodeStorage[1] <= NodeStorage[0] &&
				NodeStorage[2] >= Projection.Syntax.GetNodeCount() * sizeof(Core::SyntaxNode),
				"pooled node storage lost a block"
			);
			Check(!PreviousNodeBytes || NodeStorage[2] <= 3 * PreviousNodeBytes, "pooled node bytes exceeded linear growth");
			PreviousNodeBytes = NodeStorage[2];
			Check(!Facts[0], "reserved boolean literals still query bindings per node");
			Check(Facts[1] <= Projection.Syntax.GetNodeCount(), "first token summaries reread a child");
			if(!IsVerification) {
				Check(!PreviousEdgeReads || Facts[1] <= 3 * PreviousEdgeReads, "first token preparation exceeded linear growth");
				PreviousEdgeReads = Facts[1];
			} else Check(!Facts[1], "verification prepared unused first token summaries");
			Check(Hidden[0] == Parsed.size() && Hidden[2] == 1, "hidden gaps repartitioned the source");
			Check(
				Hidden[1] && (!PreviousHiddenQueries || Hidden[1] <= 3 * PreviousHiddenQueries),
				"hidden gap queries exceeded linear growth"
			);
			PreviousHiddenQueries = Hidden[1];
			Check(FactWork == 3 * Projection.Syntax.GetNodeCount() + Visits, "projection repeated its parser visit or fact preparation");
			if(PreviousFactWork) Check(FactWork <= 3 * PreviousFactWork, "fact preparation exceeded linear growth");
			PreviousFactWork = FactWork;
			Check(Projection.Comments.GetRecords().empty(), "comment-free projection manufactured comments");
			Check(Projection.Validation.ProtectedRegions.Directives.empty(), "comment-free projection manufactured directives");
			Check(Parsed.CollectCommentRanges(End).empty(), "repeated collection changed comment-free ranges");
			Check(ParsedTreeAccess::GetCommentVisits(Parsed) == Visits, "projection repeated the comment fact scan");
			if(IsVerification) {
				Core::OutputExpectation Expectation;
				Expectation.NormalizedSyntax = std::shared_ptr<const Core::SyntaxModel>(ExpectedProjection, &ExpectedProjection->Syntax);
				std::array<size_t, 3> Matches {};
				size_t BoundaryAdvances = 0, BoundaryCount = 0;
				for(Core::NodeId Id = 0; Id < Projection.Syntax.GetNodeCount(); ++Id) {
					BoundaryCount += ExpectedProjection->Syntax.GetNode(Id).Facts.Boundaries.GetView().size();
					BoundaryCount += Projection.Syntax.GetNode(Id).Facts.Boundaries.GetView().size();
				}
				Check(
					SyntaxValidatorCmp::IsKept(Profile.GetLang(), *ExpectedProjection, Projection, Expectation, &Matches, &BoundaryAdvances),
					"independent input and verification projections differ"
				);
				Check(BoundaryAdvances <= BoundaryCount, "structure matching revisited a boundary");
				Check(Matches[0] == Projection.Syntax.GetNodeCount(), "structure matching repeated or omitted a syntax pair");
				Check(!PreviousMatches || Matches[0] <= 3 * PreviousMatches, "structure matching exceeded linear growth");
				// 作業領域を再使用すれば，伸長の回数は照合する対の数に依らず，入力を倍にしても増えない
				Check(
					(!PreviousMatches || Matches[1] <= PreviousGrowth) && Matches[2] <= (Count << 2) + 4,
					"structure matching did not reuse its scratch buffer"
				);
				PreviousMatches = Matches[0];
				PreviousGrowth = Matches[1];
			} else ExpectedProjection = std::make_shared<Core::ProjectionResult>(std::move(Projection));
			std::fprintf(
				stdout,
				"comment-free phases n=%zu verification=%u visits=%zu extra=0 allocations=%zu blocks=%zu bytes=%zu\n",
				Count,
				static_cast<unsigned>(IsVerification),
				Visits,
				NodeStorage[0],
				NodeStorage[1],
				NodeStorage[2]
			);
		}
	}
	// 既存の呑まれたコメントのテストケースと同じ形で，隙間の範囲も入力側・検証側から失われない
	{
		const GrammarProfile &Profile = GrammarProfile::Resolve(Lang::Get(Lang::Kotlin));
		const std::string Src = "val x = a\n/* block */\n// line\n.foo()\n";
		for(const bool IsVerification : { false, true }) {
			InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, Src);
			ParseInput Input = ParseAdapter::Adapt(Profile, Normalized);
			Check(Input.Decision == ParseDecision::Continue, "gap comment input failed adaptation");
			if(IsVerification) Input.Origin = ParseInput::VerificationSrc{ Src };
			const ParsedTree Parsed = SyntaxParser::Parse(*Input.Profile, Input);
			Check(Parsed.GetParseDecision() == ParseDecision::Continue, "gap comment input failed parsing");
			const uint32_t End = ParseAdapter::GetOuterTextBounds(Parsed).second;
			const std::vector<std::pair<uint32_t, uint32_t>> Ranges = Parsed.CollectCommentRanges(End);
			Check(ParsedTreeAccess::HasGapComment(Parsed), "swallowed Kotlin comments did not reach the gap collector");
			const size_t Visits = ParsedTreeAccess::GetCommentVisits(Parsed);
			std::string Bodies;
			for(const std::pair<uint32_t, uint32_t> &Range : Ranges) Bodies.append(Parsed, Range.first, Range.second - Range.first);
			Check(Bodies.find("/* block */") != std::string::npos, "a swallowed block comment was lost");
			Check(Bodies.find("// line") != std::string::npos, "a swallowed line comment was lost");
			const Core::ProjectionResult Projection = Core::SourceProjector::Project(Parsed, *Input.Profile, Input);
			Check(Parsed.CollectCommentRanges(End) == Ranges, "projection changed the shared gap comment ranges");
			Check(ParsedTreeAccess::GetCommentVisits(Parsed) == Visits, "gap comments caused a second fact scan");
			std::string ProjectedBodies;
			for(const CommentRecord &Record : Projection.Comments.GetRecords()) ProjectedBodies += Record.Text;
			for(const ValidationContext::KeptComment &Kept : Projection.Validation.ProtectedRegions.KeptComments) {
				ProjectedBodies.append(Projection.Syntax.GetNode(Kept.Node).Lexeme, Kept.Start, Kept.End - Kept.Start);
			}
			Check(ProjectedBodies.find("block") != std::string::npos, "projection lost the swallowed block comment body");
			Check(ProjectedBodies.find("line") != std::string::npos, "projection lost the swallowed line comment body");
		}
	}
	// 終了
	return;
}

/**
 * コメントの文境界と句点・クォートの正規化の作業量の検査関数
 * 計算量：固定した規模の投影・規則適用・照合の総費用 Q に対し O(Q)
 * @throws std::runtime_error 手期待又は計数の条件と異なる場合
 * @throws FormatLimitExceeded 呼出先の作業予算又は解析の前提を満たさない場合
 * @throws std::bad_alloc ソース又は構文のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckSentenceBoundaryWork() {
	// 文末記号の数を倍増し，クォートの有無と非句点の点列を同じ本文規則で確認する
	for(const bool HasQuote : { false, true }) {
		size_t Previous = 0;
		for(const size_t Count : { 32U, 64U, 128U }) {
			std::string Text, Expected, Plain, PlainExpected;
			for(size_t Idx = 0; Idx < Count; ++Idx) {
				Text += HasQuote ? "('a.'b.'x.'c). Next.\n" : "Read... Return..?! Next.\n";
				Expected += HasQuote ? "('a 'b.'x 'c)\nNext\n" : "Read\nReturn..?!\nNext\n";
				if(!HasQuote) {
					Plain += "Read... Return..?! Next.\n";
					PlainExpected += "Read\nReturn..?!\nNext\n";
				}
				Text += HasQuote ? "(Read!　。Next)\n" : "設定を読む。.NET を使う。\n";
				Expected += HasQuote ? "(Read! Next)\n" : "設定を読む\n.NET を使う\n";
				if(!HasQuote) {
					Text += "１２. First. Next.\n𝟙𝟚. First. Next.\nＡ. First. Next.\n";
					Expected += "１２. First\nNext\n𝟙𝟚. First\nNext\nＡ. First\nNext\n";
				}
				if(HasQuote) {
					Text += "('café's result. Return.'). Next.\n𝔄'𝔅 result. Next.\n";
					Expected += "('café's result Return')\nNext\n𝔄'𝔅 result\nNext\n";
				}
			}
			size_t Work = 0;
			Check(CommentNormalizerAccess::Split(Text, Work) == Expected, "sentence work fixture changed the specified body");
			Check(Work && (!Previous || Work <= 3 * Previous), "sentence boundary work exceeded linear growth");
			if(!HasQuote) {
				// 和文の括弧候補の照合を含めず，ASCII の本文で不要な再走査を検出する
				size_t PlainWork = 0;
				Check(CommentNormalizerAccess::Split(Plain, PlainWork) == PlainExpected, "plain sentence fixture changed the specified body");
				Check(PlainWork < Plain.size() << 1, "unquoted prose still scanned all bytes after finding its marks");
			}
			Previous = Work;
		}
	}
	// 終了
	return;
}

/**
 * コメントの本文・コードスパン・空白・論理行の共有走査の作業量の検査関数
 * 計算量：固定した規模の投影・規則適用・照合の総費用 Q に対し O(Q)
 * @throws std::runtime_error 手期待又は計数の条件と異なる場合
 * @throws FormatLimitExceeded 呼出先の作業予算又は解析の前提を満たさない場合
 * @throws std::bad_alloc ソース又は構文のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckCommentBodyWork() {
	// 長い Ruby の説明で，通常の字の括弧照合・行端整理・コードスパン対応付を増幅しない
	{
		const GrammarProfile &Profile = GrammarProfile::Resolve(Lang::Get(Lang::Ruby));
		size_t PrevSentence = 0, PrevEdges = 0, PrevTicks = 0, PrevEncoding = 0, PrevBodyFacts = 0, PrevSpacing = 0;
		size_t PrevLogicalText = 0;
		Check(CommentNormalizerAccess::GetBodyFactWork("a, b，c、d", true, true) == 8, "body facts repeated character decoding");
		CommentNormalizerAccess::GetBodyFactWork(std::string("a\xFF", 2), false, false);
		const Core::NotationRules &SpaceRules = Profile.GetRules().Notation;
		CommentNormalizerAccess::GetSpaceWork(SpaceRules, "@return （甲）", "@return （甲）", false);
		CommentNormalizerAccess::GetSpaceWork(SpaceRules, "第1章 説明", "第 1 章 説明", false);
		CommentNormalizerAccess::GetSpaceWork(SpaceRules, "第 1 章 説明", "第 1 章 説明", false);
		CommentNormalizerAccess::GetSpaceWork(SpaceRules, "第１章 説明", "第１章 説明", false);
		CommentNormalizerAccess::GetSpaceWork(SpaceRules, "第١٢章 説明", "第١٢章 説明", false);
		CommentNormalizerAccess::GetSpaceWork(SpaceRules, "第 ١٢ 章 説明", "第١٢章 説明", false);
		CommentNormalizerAccess::GetSpaceWork(SpaceRules, "第1２章 説明", "第 1 ２章 説明", false);
		CommentNormalizerAccess::GetSpaceWork(SpaceRules, "第章 説明", "第章説明", false);
		CommentNormalizerAccess::GetSpaceWork(SpaceRules, "Read  @return （甲）", "Read @return（甲）", false);
		CommentNormalizerAccess::GetSpaceWork(SpaceRules, "甲abc", "甲 abc", false);
		CommentNormalizerAccess::GetSpaceWork(SpaceRules, "𐐀と𐐁 A҂B", "𐐀 と 𐐁 A ҂ B", false);
		CommentNormalizerAccess::GetSpaceWork(SpaceRules, "記号҂を使う 記号᠀を使う", "記号҂を使う記号᠀を使う", false);
		for(const size_t Count : { 32U, 64U, 128U }) {
			std::string Prose, Sentences, Edges, ExpectedEdges, Spans, PlainSpacing, ChangedSpacing, ExpectedSpacing;
			std::vector<unsigned char> SpanMask;
			for(size_t Idx = 0; Idx < Count; ++Idx) {
				Prose += "Read the operator and return the value. Next.\n";
				PlainSpacing += "Read a\tb\rc\fd value\n";
				ChangedSpacing += "Read  the value\n𐐀と𐐁 A҂B\n";
				ExpectedSpacing += "Read the value\n𐐀 と 𐐁 A ҂ B\n";
				Sentences += "Read the operator and return the value\nNext\n";
				Edges += " \ta\tb\rc\fd \u3000\r\n";
				ExpectedEdges += "a\tb\rc\fd\n";
				Spans += "Read `x` once\n";
				SpanMask.insert(SpanMask.end(), { 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0 });
				// Ruby に登録の無い Doxygen の数式と Javadoc のインラインコードは，本文として保護しない
				Spans += "Read @f$ a. b. @f$ once\nRead {@code a. b.} once\n";
				SpanMask.insert(SpanMask.end(), 48, 0);
				Spans += "café('a. Return.'); Next.\n";
				SpanMask.insert(SpanMask.end(), 6, 0);
				SpanMask.insert(SpanMask.end(), 12, 1);
				SpanMask.insert(SpanMask.end(), 9, 0);
			}
			size_t SentenceWork = 0;
			ExpectedEdges.pop_back();
			Check(CommentNormalizerAccess::Split(Prose, SentenceWork) == Sentences, "Ruby prose changed sentence boundaries");
			Check(SentenceWork < Prose.size() << 1, "ordinary prose still compares each byte against every bracket");
			const size_t EdgeWork = CommentNormalizerAccess::GetEdgeWork(Edges, ExpectedEdges);
			Check(EdgeWork < 3 * Edges.size(), "logical edges still require character-by-character full copying");
			const size_t TickWork = CommentNormalizerAccess::GetTickWork(Profile.GetRules().Notation.CodeExamples, Spans, SpanMask);
			const size_t PlainWork = CommentNormalizerAccess::GetTickWork(
				Profile.GetRules().Notation.CodeExamples,
				Prose,
				std::vector<unsigned char>(Prose.size(), 0)
			);
			Check(PlainWork <= Prose.size() + 1, "prose without code spans still builds tick correspondence");
			const size_t BodyFactWork = CommentNormalizerAccess::GetBodyFactWork(PlainSpacing, false, true);
			Check(BodyFactWork == PlainSpacing.size(), "body facts do not use one character scan");
			Check(
				!CommentNormalizerAccess::GetSpaceWork(SpaceRules, PlainSpacing, PlainSpacing, false),
				"unchanged spacing copied the body"
			);
			const size_t SpacingWork = CommentNormalizerAccess::GetSpaceWork(SpaceRules, ChangedSpacing, ExpectedSpacing, false);
			Check(SpacingWork == ExpectedSpacing.size(), "changed spacing copied the body more than once");
			Check(
				!CommentNormalizerAccess::GetSpaceWork(SpaceRules, ChangedSpacing, ChangedSpacing, true),
				"protected spacing copied the body"
			);
			CommentRecord LogicalRecord { "\n" + PlainSpacing, { CommentPlacement::Before } };
			LogicalRecord.Notation = CommentNotation::RubyEmbeddedDocument;
			Core::CommentRepository::AttachRules(SpaceRules, LogicalRecord);
			const size_t LogicalTextWork =
			Core::DocCommentReaderAccess::GetLogicalTextWork(LogicalRecord, std::string_view(LogicalRecord.Text), 0);
			Check(LogicalTextWork <= LogicalRecord.Text.size() + 1, "logical text revisited the body");
			CommentRecord BlockLogicalRecord { "\n" + PlainSpacing, { CommentPlacement::Before } };
			BlockLogicalRecord.Notation = CommentNotation::Block;
			Core::CommentRepository::AttachRules(GrammarProfile::Resolve(Lang::Get(Lang::Cpp)).GetRules().Notation, BlockLogicalRecord);
			const size_t BlockLogicalWork = Core::DocCommentReaderAccess::GetLogicalTextWork(
				BlockLogicalRecord,
				std::string_view(PlainSpacing).substr(0, PlainSpacing.size() - 1),
				1
			);
			Check(BlockLogicalWork <= BlockLogicalRecord.Text.size() + 1, "block logical text revisited the body");
			Core::StructureWorkAccount EncodingWork;
			const Core::CommentRepository::RegisteredLineForm Form = Core::CommentRepository::RegisteredLineFormOf(
				Profile.GetRules().Notation.OutputForms.Conditions,
				"# " + Prose,
				std::nullopt,
				std::nullopt,
				&EncodingWork
			);
			Check(!Form.IsMagic && !Form.IsEncoding, "plain prose became a registered directive");
			Check(
				(!PrevSentence || SentenceWork <= 3 * PrevSentence) && (!PrevEdges || EdgeWork <= 3 * PrevEdges) &&
				(!PrevTicks || TickWork <= 3 * PrevTicks) && (!PrevEncoding || EncodingWork.Used <= 3 * PrevEncoding),
				"Ruby body or registered-form work exceeded linear growth"
			);
			Check(
				(!PrevBodyFacts || BodyFactWork <= 3 * PrevBodyFacts) && (!PrevSpacing || SpacingWork <= 3 * PrevSpacing) &&
				(!PrevLogicalText || LogicalTextWork <= 3 * PrevLogicalText),
				"shared Ruby body work exceeded linear growth"
			);
			PrevBodyFacts = BodyFactWork;
			PrevSpacing = SpacingWork;
			PrevLogicalText = LogicalTextWork;
			std::fprintf(
				stdout,
				"Ruby shared body n=%zu facts=%zu spacingCopies=%zu logical=%zu\n",
				Count,
				BodyFactWork,
				SpacingWork,
				LogicalTextWork
			);
			PrevSentence = SentenceWork;
			PrevEdges = EdgeWork;
			PrevTicks = TickWork;
			PrevEncoding = EncodingWork.Used;
			std::fprintf(
				stdout,
				"Ruby comment body n=%zu sentence=%zu edges=%zu ticks=%zu encoding=%zu\n",
				Count,
				SentenceWork,
				EdgeWork,
				TickWork,
				EncodingWork.Used
			);
		}
	}
	// 終了
	return;
}

/**
 * コメントの分離とドキュメントの判定・説明対象の投影の検査関数
 * 計算量：固定した規模の投影・規則適用・照合の総費用 Q に対し O(Q)
 * @throws std::runtime_error 手期待又は計数の条件と異なる場合
 * @throws FormatLimitExceeded 呼出先の作業予算又は解析の前提を満たさない場合
 * @throws std::bad_alloc ソース又は構文のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckDocumentationProjectionWork() {
	// 独立したコメントの分離を故意に省いた入力を，検査用指定を使わず投影自身が拒否する
	{
		const GrammarProfile &Profile = GrammarProfile::Resolve(Lang::Get(Lang::Cpp));
		InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, "// body\nint x;\n");
		ParseInput Input = ParseAdapter::Adapt(Profile, Normalized);
		const ParsedTree Parsed = SyntaxParser::Parse(*Input.Profile, Input);
		Parsed.CollectCommentRanges(ParseAdapter::GetOuterTextBounds(Parsed).second);
		Check(Core::SourceProjectorAccess::IsUnseparatedRejected(Parsed, Input), "projection accepted an unseparated comment node");
	}
	// 中間表現と同期が使う構文の判定は，正規化の一時表の解放で失わない
	{
		Core::ProjectionResult Projection = ProjectWithCtx(Lang::Get(Lang::Cpp), "void f(int x) {}\n");
		Core::SyntaxModel &Syntax = Projection.Syntax;
		const Core::NodeId Function = GetNodesWithRole(Syntax, Core::Role::DocumentedDef).front();
		const Core::DocumentationFact Fact = Core::SignatureExtractor::GetDocumentationFact(Syntax, Function);
		Check(Fact.IsDocumentable && Fact.Anchor == Function, "ordinary definition changed documentation eligibility");
		Syntax.ReleaseFacts();
		const size_t Before = Syntax.GetWorkAccount()->Used;
		const Core::DocumentationFact Reused = Core::SignatureExtractor::GetDocumentationFact(Syntax, Function);
		Check(Reused.IsDocumentable && Reused.Anchor == Fact.Anchor, "released temporary facts changed documentation eligibility");
		Check(Syntax.GetWorkAccount()->Used == Before + 1, "documentation eligibility was extracted again");
		// 別のモデルの本体を除いても，元のモデルの判定へ影響させない
		Core::SyntaxModel Changed = Syntax;
		const Core::NodeId Body = Changed.GetFirstNamedChildWithRole(Function, Core::Role::FunctionBodyContainer);
		Check(Body != Core::NoNode, "definition fixture has no implementation body");
		Core::RewritePlan Plan;
		Plan.Steps.push_back({ Core::RewriteKind::Remove, Body });
		Changed.Apply(Plan);
		Check(
			!Core::SignatureExtractor::GetDocumentationFact(Changed, Function).IsDocumentable,
			"removed body retained stale documentation eligibility"
		);
		Check(
			Core::SignatureExtractor::GetDocumentationFact(Syntax, Function).IsDocumentable,
			"independent models shared mutable documentation eligibility"
		);
	}
	// 名前付定義・式本体・ラッパーと，実装を持たない宣言の選別
	struct SiteFixture {
		::Lang::ID Lang; // 検査する言語
		const char *Src; // 定義又は本体を持たない宣言
		bool IsDocumentable; // ドキュメント化の対象かの手期待
	};
	constexpr SiteFixture SiteInputs[] {
		{ Lang::CSharp, "class C { int F(int x) => x; }", true },
		{ Lang::CSharp, "int Local(int x) => x;", true },
		{ Lang::CSharp, "class C { ~C() {} }", true },
		{ Lang::CSharp, "class C { public static implicit operator int(C x) => 1; }", true },
		{ Lang::CSharp, "interface C { int F(int x); }", false },
		{ Lang::Java, "record C(int x) { C {} }", true },
		{ Lang::Swift, "class C { deinit {} }", true }
	};
	for(const SiteFixture &Input : SiteInputs) {
		const Core::SyntaxModel Syntax = Project(Lang::Get(Input.Lang), Input.Src);
		const std::vector<Core::NodeId> Functions = GetNodesWithRole(Syntax, Core::Role::DocumentedDef);
		Check(Functions.size() == 1, "documentation site fixture lost its named definition", Input.Src);
		Check(
			Core::SignatureExtractor::GetDocumentationFact(Syntax, Functions.front()).IsDocumentable == Input.IsDocumentable,
			"definition body or enclosing sequence changed documentation eligibility",
			Input.Src
		);
	}
	// 同じ本体の定義数を倍増し，説明対象の表を構築する実際の訪問回数を確認する
	{
		size_t Previous = 0;
		for(const size_t Count : { 16U, 32U, 64U }) {
			std::string Src;
			for(size_t Idx = 0; Idx < Count; ++Idx) Src += "void f" + std::to_string(Idx) + "(int x){}\n";
			const Core::ProjectionResult Projection = ProjectWithCtx(Lang::Get(Lang::Cpp), Src);
			const size_t Work = Core::DocCommentReaderAccess::GetWork(Projection.Syntax);
			Check(
				Work >= Projection.Syntax.GetNodeCount() + Count && Work < Projection.Syntax.GetNodeCount() << 1,
				"document target table repeated its owner walk"
			);
			Check(!Previous || Work <= 3 * Previous, "document target table work exceeded linear growth");
			Previous = Work;
		}
	}
	// 同じ外側の領域の IIFE は，構造の失効で供給値の逆引を繰り返さない
	for(const Lang::ID Language : { Lang::JavaScript, Lang::TypeScript }) {
		size_t Previous = 0;
		for(const size_t Count : { 16U, 32U, 64U, 128U }) {
			std::string Src = "(function(){";
			for(size_t Idx = 0; Idx < Count; ++Idx) {
				Src += "var S" + std::to_string(Idx) + "=(function(){var alias=G;function Inner(x){if(x){alias(x);}}return Inner;})();";
			}
			Src += "})();";
			Core::SyntaxModel Syntax = Project(Lang::Get(Language), Src);
			const size_t Work = Core::SyntaxNormalizerAccess::Run(Syntax).Work;
			Check(GetNodesWithRole(Syntax, Core::Role::CallNode).size() == (Count << 1) + 1, "IIFE normalization lost its calls");
			Check(GetNodesWithRole(Syntax, Core::Role::FlowReturn).size() == Count << 1, "IIFE normalization changed return completion");
			Check(!Previous || Work <= (Previous << 1) + 2048, "IIFE normalization exceeded linear work");
			Previous = Work;
		}
	}
	// 終了
	return;
}

/**
 * ドキュメントの所属・同期・インデックス更新と断片の合成の作業量の検査関数
 * 計算量：固定した規模の投影・規則適用・照合の総費用 Q に対し O(Q)
 * @throws std::runtime_error 手期待又は計数の条件と異なる場合
 * @throws FormatLimitExceeded 呼出先の作業予算又は解析の前提を満たさない場合
 * @throws std::bad_alloc ソース又は構文のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckDocumentationCompositionWork() {
	// 通常コメントを除いた後も，本文・対象・内外の区分は同じ記録から取る
	for(const std::string_view Inner : { "/*! Inner */\n", "//! Inner\n" }) {
		const Core::ProjectionResult Projection = ProjectDiagnosticCase(
			Lang::Get(Lang::Rust),
			"// Ordinary\nmod m {\n" + std::string(Inner) + "/** Outer */\nfn f() {}\n// Tail\n}\n",
			true
		);
		const std::vector<ValidationContext::DocFact> &Docs = Projection.Validation.ProtectedRegions.Docs;
		Check(Docs.size() == 2, "filtered document facts lost a document or included an ordinary comment");
		Check(Docs[0].IsInner && !Docs[1].IsInner, "filtered document facts took the notation side from another record");
		Check(Docs[0].Text.find("Inner") != std::string_view::npos, "filtered inner document facts lost their own body");
		Check(Docs[1].Text.find("Outer") != std::string_view::npos, "filtered outer document facts lost their own body");
	}
	// 未展開のトークン列の説明を実在する宣言へ結び付けず，通常コメントとして比較する
	{
		const std::string Src =
		"macro_rules! m { () => {\n/// Line\n//! Inner line\n/** Block */\n/*! Inner block */\nstruct S;\n}; }\n"
		"/** Function */\nfn f() {}\n";
		const Core::ProjectionResult Projection = ProjectDiagnosticCase(Lang::Get(Lang::Rust), Src, true);
		const std::vector<ValidationContext::DocFact> &Docs = Projection.Validation.ProtectedRegions.Docs;
		Check(
			Docs.size() == 1 && !Docs[0].IsInner && Docs[0].Text.find("Function") != std::string_view::npos,
			"macro documentation lost its ordinary or containing target"
		);
		Check(Projection.Validation.ProtectedRegions.KeptComments.empty(), "macro comments entered protected code text");
		// コメントの内容をコードの構造比較へ含めず，本文は独立したコメント比較で確かめる
		Core::ProjectionResult Input = ProjectWithCtx(Lang::Get(Lang::Rust), Src);
		Input.Comments.Remap(Core::SyntaxNormalizer().Normalize(Input.Syntax));
		Core::FormatIR Intermediate = Core::Canonicalizer::Build(std::make_shared<const Core::SyntaxModel>(std::move(Input.Syntax)));
		Intermediate.CloseWork();
		const Core::BrokenIR Broken = Core::LineBreaker(128).Resolve(std::move(Intermediate));
		Check(
			SyntaxValidatorCmp::IsKept(Lang::Get(Lang::Rust), Input, Projection, Broken.Expectation),
			"unchanged macro token structure was rejected"
		);
		for(const std::string_view Replacement : { "", "/// Changed\n" }) {
			std::string Changed = Src;
			constexpr std::string_view Original = "/// Line\n";
			const size_t At = Changed.find(Original);
			Check(At != std::string::npos, "a macro comment mutation did not match its input");
			Changed.replace(At, Original.size(), Replacement);
			const Core::ProjectionResult Candidate = ProjectDiagnosticCase(Lang::Get(Lang::Rust), Changed, true);
			Check(
				SyntaxValidatorCmp::IsKept(Lang::Get(Lang::Rust), Input, Candidate, Broken.Expectation),
				"macro comments changed code structure comparison"
			);
			Check(
				!SyntaxValidatorCmp::HasSameOrdinaryComments(Projection, Candidate),
				"changed ordinary macro comments escaped independent comparison"
			);
		}
	}
	// 内側の記法を説明する本体の中へ置き，外側の説明と同じ検証を通す
	for(const std::string_view Inner : { "/*! Inner */\n", "//! Inner\n" }) for(const uint32_t Width : { 0U, 12U, 128U }) {
		const std::string Src = "mod m {\n" + std::string(Inner) + "/** Outer */\nfn f() {}\n}\n";
		const FormatResult Result = Formatter{ Core::LineBreaker(Width) }.Format(Src, Lang::Get(Lang::Rust), false);
		Check(
			Result.Reason == FailureReason::None &&
			Result.Text == "mod m {\n\t/*!\n\t * Inner\n\t */\n\t/**\n\t * Outer\n\t */\n\tfn f() {}\n}\n",
			"inner documentation moved before its containing declaration"
		);
	}
	// 返戻のラッパーの前後へ移譲し，値の内部のアンカーは其の値に保つ
	struct ReturnAnchorFixture {
		Lang::ID Language; // 返戻を明示する言語
		const char *Src; // 値の直前の独立コメントを持つ関数
	};
	const ReturnAnchorFixture ReturnAnchors[] {
		{ Lang::Rust, "fn f(x: i32) -> i32 {\n// Leading\nx\n}\n" },
		{ Lang::Swift, "func f(_ x: Int) -> Int {\n// Leading\nx\n}\n" },
		{ Lang::Ruby, "def f(x)\n# Leading\nx\nend\n" }
	};
	for(const ReturnAnchorFixture &Input : ReturnAnchors) {
		Core::ProjectionResult Projection = ProjectWithCtx(Lang::Get(Input.Language), Input.Src);
		const Core::NodeId Anchor = Projection.Comments.GetRecords().front().Anchor;
		// 本体の先行コメントは本体へ残し，ラッパーで包む末尾の値の移譲だけを検査する
		const Core::NodeId Original =
		Projection.Syntax.Is(Anchor, Core::Role::StmtSeq) ? Projection.Syntax.GetLastNamedChild(Anchor) : Anchor;
		const Core::RewriteTrace Trace = Core::SyntaxNormalizer().Normalize(Projection.Syntax);
		for(const Core::AnchorSide Side : { Core::AnchorSide::After, Core::AnchorSide::Before, Core::AnchorSide::Inside }) {
			const uint64_t Key = Core::RewriteTrace::GetMoveKey(Original, Side, false);
			Check(Trace.Moves.contains(Key), "a generated return wrapper did not transfer its anchors", Input.Src);
			const Core::AnchorMove &Move = Trace.Moves.at(Key);
			Check(Move.Side == Side, "a generated return wrapper changed an anchor side", Input.Src);
			if(Side == Core::AnchorSide::Inside) {
				Check(Move.Node == Original, "a return wrapper moved a value's interior anchor", Input.Src);
			} else {
				Check(
					Projection.Syntax.GetLexemeOf(Projection.Syntax.GetFirstEdge(Move.Node).Leaf) == "return",
					"a return wrapper left an exterior anchor on its value",
					Input.Src
				);
			}
		}
		Projection.Comments.Remap(Trace);
		Check(
			Projection.Comments.GetRecords().front().Anchor ==
			(Anchor == Original ? Trace.Moves.at(Core::RewriteTrace::GetMoveKey(Original, Core::AnchorSide::Before, false)).Node : Anchor),
			"a return wrapper lost the original comment anchor",
			Input.Src
		);
	}
	// 内側のドキュメントは，末尾式の明示化でも同じ関数の説明として同期・配置する
	for(const std::string_view Inner : { "/*! Value */\n", "//! Value\n" }) {
		for(const std::string_view Tail : { "return x;\n", "x\n" }) {
			const std::string Src = "fn f(x: i32) -> i32 {\n" + std::string(Inner) + std::string(Tail) + "}\n";
			for(const uint32_t Width : { 0U, 40U, 128U }) {
				const FormatResult Result = Formatter{ Core::LineBreaker(Width) }.Format(Src, Lang::Get(Lang::Rust), false);
				Check(
					Result.Reason == FailureReason::None && Result.Text ==
					"fn f(x: i32) -> i32 {\n\t/*!\n\t * Value\n\t * # Arguments\n\t * * `x`\n\t * # Returns\n\t */\n\treturn x;\n}\n",
					"a tail return changed inner documentation"
				);
			}
		}
	}
	std::array<size_t, 4> PreviousCandidateQueries {}, PreviousDocQueries {};
	// 記録数の倍増で，所属の読取・同期の表・空行の転写・断片の複写が線形である事の検査
	for(const size_t Count : { 16U, 32U, 64U }) {
		// 比較毎に同じ所属を読み直さず，原文順とディレクティブの除外を保つ
		CommentRecordList Records(Count + 2);
		for(size_t Idx = 0; Idx < Count; ++Idx) {
			Records[Idx].Format.Group = 0;
			Records[Idx].OriginalOrder = Count - (Idx >> 1);
		}
		Records[Count].Role = CommentRole::Directive;
		size_t GroupWork = 0;
		const std::vector<std::vector<size_t>> Groups = CommentNormalizerAccess::GetGroups(Records, GroupWork);
		Check(GroupWork == Records.size(), "group sorting repeated record lookups");
		Check(
			Groups.size() == 2 && Groups[0].size() == Count && Groups[1] == std::vector<size_t>{ Count + 1 },
			"group sorting changed membership"
		);
		for(size_t Idx = 0; Idx < Count; ++Idx) {
			Check(Groups[0][Idx] == Count - (Idx >> 1 << 1) - 2 + Idx % 2, "group sorting changed stable source order");
		}
		// 同期の表を共有した説明対象の照会は，記録を１度読むだけ
		std::string Docs;
		for(size_t Idx = 0; Idx < Count; ++Idx) {
			Docs += "/** Summary\n * @param x Value\n */\nvoid f" + std::to_string(Idx) + "(int x){}\n";
		}
		// ドキュメントの事実は完成候補の投影（検証側）で作る
		std::array<size_t, 4> CandidateQueries {}, DocQueries {};
		Core::ProjectionResult Projection = ProjectDiagnosticCase(Lang::Get(Lang::Cpp), Docs, true, &CandidateQueries, &DocQueries);
		for(size_t Idx = 0; Idx < DocQueries.size(); ++Idx) {
			Check(
				DocQueries[Idx] && (!PreviousDocQueries[Idx] || DocQueries[Idx] <= 3 * PreviousDocQueries[Idx]),
				"document description exceeded linear growth"
			);
		}
		PreviousDocQueries = DocQueries;
		Check(
			!CandidateQueries[1] && !CandidateQueries[2] && !CandidateQueries[3],
			"verification prepared unused notation copies, reading checks or layout adjacency"
		);
		Check(CandidateQueries[0] <= Projection.Syntax.GetNodeCount(), "verification reread argument protection children");
		Check(
			!PreviousCandidateQueries[0] || CandidateQueries[0] <= 3 * PreviousCandidateQueries[0],
			"verification argument protection exceeded linear growth"
		);
		PreviousCandidateQueries = CandidateQueries;
		const std::pair<size_t, size_t> Storage = Core::SourceProjectorAccess::GetDocStorage(
			Projection.Comments.GetRecords(),
			std::move(Projection.Validation.ProtectedRegions.Docs)
		);
		size_t BodyBytes = 0;
		for(const CommentRecord &Record : Projection.Comments.GetRecords()) {
			BodyBytes += Core::CommentRepository::GetOutputTextOf(Record).size();
		}
		Check(
			Storage.first == BodyBytes && Storage.second <= BodyBytes + Count,
			"document facts copied each logical line into another string"
		);
		// 完成候補の投影が作った表を，所属の検査と静的解析の対応付も共有する
		size_t DefinitionWork = 0;
		const std::vector<Core::NodeId> Definitions =
		Core::DocCommentReader::GetDocDefinitionTargets(Projection.Syntax, Projection.Comments, &DefinitionWork);
		Check(DefinitionWork == Projection.Comments.GetRecords().size(), "candidate definitions rebuilt the projected table");
		for(const Core::NodeId Definition : Definitions) {
			Check(Definition != Core::NoNode, "candidate document lost its function definition");
		}
		size_t TableWork = 0;
		Core::DocCommentSynchronizer::Synchronize(Projection.Syntax, Projection.Comments, &TableWork);
		size_t TargetWork = 0;
		const std::vector<Core::NodeId> Targets =
		Core::DocCommentReader::GetDocTargets(Projection.Syntax, Projection.Comments, nullptr, &TargetWork);
		Check(!TableWork, "synchronization rebuilt the unchanged candidate table");
		Check(TargetWork == Projection.Comments.GetRecords().size(), "document targets rebuilt their shared table");
		for(const Core::NodeId Target : Targets) Check(Target != Core::NoNode, "independent docs lost their definition");
		// 本文世代だけの更新ではインデックスを共有し，アンカーを変えた更新だけ全記録を再登録する
		Core::CommentRepository &Comments = Projection.Comments;
		size_t IndexWork = 0;
		Comments.SetStructure(
			CommentNormalizer::Inspect(ReadOnlyCommentView(Comments.GetRecords()), Comments.GetStructure()),
			&IndexWork
		);
		Check(!IndexWork, "body-only structure replacement rebuilt the anchor index");
		const Core::NodeId OldAnchor = Comments.GetRecords().front().Anchor, NewAnchor = Comments.GetRecords().back().Anchor;
		Check(OldAnchor != NewAnchor, "index fixture did not contain distinct definitions");
		const size_t OldCount = Comments.Get(OldAnchor).size(), NewCount = Comments.Get(NewAnchor).size();
		Comments.EditRecords().front().Anchor = NewAnchor;
		Comments.SetStructure(
			CommentNormalizer::Inspect(ReadOnlyCommentView(Comments.GetRecords()), Comments.GetStructure()),
			&IndexWork
		);
		Check(IndexWork == Comments.GetRecords().size(), "changed anchors were not reindexed once");
		Check(
			Comments.Get(OldAnchor).size() == OldCount - 1 && Comments.Get(NewAnchor).size() == NewCount + 1,
			"reindexing did not move a record between its definition anchors"
		);
		Comments.EditRecords().front().Anchor = OldAnchor;
		Comments.SetStructure(CommentNormalizer::Inspect(ReadOnlyCommentView(Comments.GetRecords()), Comments.GetStructure()));
		Comments.Remap(Core::RewriteTrace{});
		Check(
			Comments.Get(OldAnchor).size() == OldCount && Comments.Get(NewAnchor).size() == NewCount,
			"an empty transfer discarded the anchor index"
		);
		// 無効化と追加を続けても，追加分だけの不完全なインデックスを確定済と扱わない
		CommentRecord Extra = Comments.EditRecords().front();
		Comments.Add(std::move(Extra));
		IndexWork = 0;
		Comments.SetStructure(
			CommentNormalizer::Inspect(ReadOnlyCommentView(Comments.GetRecords()), Comments.GetStructure()),
			&IndexWork
		);
		Check(IndexWork == Comments.GetRecords().size(), "appending to invalidated records left a partial anchor index");
		Check(Comments.Get(OldAnchor).size() == OldCount + 1, "appending lost existing records at the same anchor");
		// 空の記録と同期する定義の無い木は，所属表を構築しない
		std::string Json = "[";
		for(size_t Idx = 0; Idx < Count; ++Idx) Json += Idx ? ",1" : "1";
		Json += "]\n";
		Core::ProjectionResult Empty = ProjectWithCtx(Lang::Get(Lang::JSON), Json);
		size_t EmptyWork = 0;
		Core::DocCommentSynchronizer::Synchronize(Empty.Syntax, Empty.Comments, &EmptyWork);
		Check(!EmptyWork && Empty.Comments.GetStructure(), "empty docs built a table or lost their shared structure");
		// 本文途中のタブ・単独 CR・書式送りを保ち，空行だけを同じバッファで詰める
		std::string Text, Expected, Plain;
		for(size_t Idx = 0; Idx < Count; ++Idx) {
			Text += "a\tb\rc\fd\n\n";
			Expected += Idx ? "\na\tb\rc\fd" : "a\tb\rc\fd";
			Plain += "int v" + std::to_string(Idx) + ";\n";
		}
		const size_t BlankWork = CommentNormalizerAccess::GetEdgeWork(Text, Expected);
		Check(BlankWork <= 3 * Text.size() + 1, "logical-line normalization exceeded linear work");
		const GrammarProfile &Profile = GrammarProfile::Resolve(Lang::Get(Lang::Cpp));
		InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, Plain);
		const std::span<const uint32_t> LineStarts = Normalized.GetLineStarts();
		ParseInput Input = ParseAdapter::Adapt(Profile, Normalized);
		const ParsedTree Parsed = SyntaxParser::Parse(*Input.Profile, Input);
		Check(
			Core::SourceProjectorAccess::GetDecodedWork(Parsed, Input, 0, static_cast<uint32_t>(Parsed.size()), Plain) == Plain.size(),
			"unmodified fragments still made a temporary text copy"
		);
		const std::vector<uint32_t> Transferred = std::move(Normalized).TakeLineStarts();
		Check(
			Transferred.data() == LineStarts.data() && !Transferred.front() && Transferred.back() == Plain.size(),
			"moving the line table lost its storage or final input offset"
		);
	}
	// 埋込の有無に従う分解の経路でも，記録数の倍増で作業を増幅しない
	for(const bool IsEmbedded : { false, true }) {
		size_t Previous = 0, PreviousLiteralWork = 0;
		std::array<size_t, 3> PreviousCommentQueries {};
		for(const size_t Count : { 16U, 32U, 64U }) {
			std::string Src = IsEmbedded ? "=begin\nx =end x\n" : "";
			std::string Tail;
			for(size_t Idx = 0; Idx < Count; ++Idx) {
				Src += IsEmbedded ? "Read\n" : "# Read\n";
				Tail += "x = 1\n";
			}
			Src += IsEmbedded ? "=end\n" + Tail : "x = 1\n";
			const GrammarProfile &Profile = GrammarProfile::Resolve(Lang::Get(Lang::Ruby));
			InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, Src);
			ParseInput Input = ParseAdapter::Adapt(Profile, Normalized);
			const ParsedTree Parsed = SyntaxParser::Parse(*Input.Profile, Input);
			std::array<size_t, 3> CommentQueries {};
			const size_t Work = Core::SourceProjectorAccess::GetCommentWork(Parsed, Input, &CommentQueries);
			Check(!CommentQueries[2], "Ruby comments were moved through an unused merge buffer");
			for(size_t Idx = 0; Idx < 2; ++Idx) {
				Check(
					!PreviousCommentQueries[Idx] || CommentQueries[Idx] <= 3 * PreviousCommentQueries[Idx],
					"comment child-table reads exceeded linear growth"
				);
			}
			PreviousCommentQueries = CommentQueries;
			size_t LiteralWork = 0;
			Check(EstimateWorkingBytes(Profile, Src, &LiteralWork) < MaxWorkingBytes, "small comment input exceeded working memory");
			Check(
				LiteralWork && (!PreviousLiteralWork || LiteralWork <= 3 * PreviousLiteralWork),
				"literal admission queries exceeded linear growth"
			);
			PreviousLiteralWork = LiteralWork;
			Check(Work && (!Previous || Work <= 3 * Previous), "comment decomposition exceeded linear growth");
			if(IsEmbedded) {
				Check(Parsed.GetAnalysisMark(), "embedded text did not request marker decoding");
				const uint32_t End = static_cast<uint32_t>(Parsed.size());
				const size_t Copies =
				Core::SourceProjectorAccess::GetDecodedWork(Parsed, Input, End - static_cast<uint32_t>(Tail.size()), End, Tail);
				Check(Copies == Tail.size(), "a continuous decoded fragment was copied twice");
				std::fprintf(stdout, "decoded fragment n=%zu copies=%zu\n", Count, Copies);
			}
			Previous = Work;
		}
	}
	// 埋込ドキュメント内の `=end` 毎に末尾を移動しない事と，正規の投影で本文が戻る事の検査
	{
		const GrammarProfile &Profile = GrammarProfile::Resolve(Lang::Get(Lang::Ruby));
		size_t PrevWork = 0;
		for(const size_t Count : { 128U, 256U, 512U, 1024U }) {
			std::string Src = "=begin\n";
			for(size_t Idx = 0; Idx < Count; ++Idx) Src += "x =end ";
			Src += "\n=end\nx = 1\n";
			InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, Src);
			Check(Normalized.Reason == FailureReason::None, "embedded marker input was rejected before parsing");
			ParseInput Input = ParseAdapter::Adapt(Profile, Normalized);
			Check(
				Input.Decision == ParseDecision::Continue && Input.Src && Input.Src->GetAnalysisMark(),
				"embedded marker input did not reach its prepared form"
			);
			std::string Restored(Input.Src->GetCodeText());
			const size_t Work = Core::SourceProjectorAccess::GetRestoreWork(Restored, Input.Src->GetAnalysisMark());
			Check(Restored == Src, "embedded marker removal changed the original body or code");
			Check(Work && (!PrevWork || Work <= 3 * PrevWork), "embedded marker removal grows quadratically");
			// 同じ入力の正規の投影で，本文の記録に解析用マーカーが残らない事の照合
			const ParsedTree Parsed = SyntaxParser::Parse(*Input.Profile, Input);
			Check(Parsed.GetParseDecision() == ParseDecision::Continue, "embedded marker input was rejected during parsing");
			Core::SourceProjectorAccess::CheckUnaffectedFragment(Parsed, static_cast<uint32_t>(Parsed.find("x = 1\n")), "x = 1\n");
			const Core::ProjectionResult Projection = Core::SourceProjector::Project(Parsed, *Input.Profile, Input);
			size_t BodyCount = 0;
			for(const CommentRecord &Record : Projection.Comments.GetRecords()) if(!Record.IsEmbeddedEdge) {
				Check(Record.Text == std::string_view(Src).substr(7, 7 * Count), "projected embedded text retained a marker");
				++BodyCount;
			}
			Check(BodyCount == 1, "embedded marker input changed its body count");
			std::fprintf(stdout, "embedded document markers n=%zu work=%zu\n", Count, Work);
			PrevWork = Work;
		}
	}
	// 埋込ドキュメントの各文境界で末尾全体を複写しない事の，製品の分離経路の作業量の検査
	for(const bool HasMagic : { false, true }) {
		size_t PrevWork = 0;
		for(const size_t Count : { 1024U, 2048U, 4096U, 8192U }) {
			std::string Src = "=begin\n";
			for(size_t Idx = 0; Idx < Count; ++Idx) Src += "Read. ";
			Src += HasMagic ? "warn_indent: true\n=end\nx = 1\n" : "Finish.\n=end\nx = 1\n";
			Core::ProjectionResult Projection = ProjectWithCtx(Lang::FromName("ruby"), Src);
			const size_t Work = Core::DocCommentSynchronizerAccess::GetSplitWork(Projection.Comments);
			Check(Work && (!PrevWork || Work <= 3 * PrevWork), "embedded document sentence work grows quadratically");
			std::vector<std::string_view> Bodies;
			for(const CommentRecord &Record : Projection.Comments.GetRecords()) if(!Record.IsEmbeddedEdge) Bodies.push_back(Record.Text);
			Check(Bodies.size() == (HasMagic ? 2 : 1), "embedded prose changed its handwritten partition count");
			if(HasMagic) {
				Check(Bodies.front() == std::string_view(Src).substr(7, 6 * Count), "embedded prose lost its first partition");
				Check(Bodies.back() == "warn_indent: true", "embedded prose lost the registered value suffix");
			} else Check(Bodies.front() == std::string_view(Src).substr(7, 6 * Count + 7), "unsplit embedded prose changed its body");
			std::fprintf(stdout, "embedded document sentences magic=%u n=%zu work=%zu\n", static_cast<unsigned>(HasMagic), Count, Work);
			PrevWork = Work;
		}
	}
	// 終了
	return;
}

/**
 * 正準化の候補列の所有領域とフレームの再使用の作業量の検査関数
 * 計算量：固定した規模の投影・規則適用・照合の総費用 Q に対し O(Q)
 * @throws std::runtime_error 手期待又は計数の条件と異なる場合
 * @throws FormatLimitExceeded 呼出先の作業予算又は解析の前提を満たさない場合
 * @throws std::bad_alloc ソース又は構文のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckCanonicalCandidateWork() {
	// 入子の候補列が親の領域を保ち，兄弟の倍増時に構築とフレームの再使用が線形に収まる事の照合
	size_t PrevCandidateBuildWork = 0, PrevCandidateFrameAllocations = 0;
	for(const size_t CandidateCount : { 16U, 32U, 64U }) {
		std::string CandidateSrc = "void f(){";
		for(size_t Candidate = 0; Candidate < CandidateCount; ++Candidate) CandidateSrc += "g(1,h(2,3),4);";
		CandidateSrc += "}";
		Core::SyntaxModel CandidateSyntax = Project(Lang::Get(Lang::Cpp), CandidateSrc);
		Core::SyntaxNormalizerAccess::Run(CandidateSyntax);
		const std::shared_ptr<const Core::SyntaxModel> CandidateModel =
		std::make_shared<const Core::SyntaxModel>(std::move(CandidateSyntax));
		std::pair<size_t, size_t> CandidateFrames;
		const size_t CandidateStart = CandidateModel->GetWorkAccount()->Used;
		Core::FormatIR CandidateIR = Core::CanonicalizerAccess::BuildWithWork(CandidateModel, CandidateFrames);
		const size_t CandidateWork = CandidateModel->GetWorkAccount()->Used - CandidateStart;
		Check(
			CandidateWork && (!PrevCandidateBuildWork || CandidateWork <= (PrevCandidateBuildWork << 1) + 64),
			"nested candidates exceeded linear construction work"
		);
		Check(
			CandidateFrames.first > CandidateFrames.second &&
			(!PrevCandidateFrameAllocations || CandidateFrames.second <= PrevCandidateFrameAllocations + 8),
			"sibling candidates retained coroutine frames"
		);
		PrevCandidateBuildWork = CandidateWork;
		PrevCandidateFrameAllocations = CandidateFrames.second;
		CandidateIR.CloseWork();
		std::fprintf(
			stdout,
			"canonical candidates n=%zu work=%zu requests=%zu allocations=%zu\n",
			CandidateCount,
			CandidateWork,
			CandidateFrames.first,
			CandidateFrames.second
		);
	}
	// 終了
	return;
}

/**
 * 宣言の分割に依る束縛と依存の照会の作業量の検査関数
 * 計算量：固定した規模の投影・規則適用・照合の総費用 Q に対し O(Q)
 * @throws std::runtime_error 手期待又は計数の条件と異なる場合
 * @throws FormatLimitExceeded 呼出先の作業予算又は解析の前提を満たさない場合
 * @throws std::bad_alloc ソース又は構文のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckDeclarationSplitWork() {
	// 並列の束縛の開始を変える分割と，独立した初期化の分割を正誤対として照合する
	for(const bool IsDependent : { false, true }) {
		size_t PrevSplitWork = 0;
		for(const size_t SplitCount : { 16U, 32U, 64U }) {
			std::string SplitSrc = "package p\nfunc f(value int){\n";
			for(size_t Idx = 0; Idx < SplitCount; ++Idx) {
				SplitSrc +=
				IsDependent ? "{var value, other = 7, value; println(value,other)}\n" : "{var value, other = 7, 4; println(value,other)}\n";
			}
			SplitSrc += "}\n";
			Core::SyntaxModel SplitSyntax = Project(Lang::Get(Lang::Go), SplitSrc);
			const size_t SplitWork = Core::CanonicalizerAccess::GetDeclarationSplitWork(SplitSyntax, IsDependent);
			Check(SplitWork && (!PrevSplitWork || SplitWork <= 3 * PrevSplitWork), "declaration splitting repeated dependency scans");
			PrevSplitWork = SplitWork;
			std::fprintf(
				stdout,
				"declaration split dependent=%u n=%zu work=%zu\n",
				static_cast<unsigned>(IsDependent),
				SplitCount,
				SplitWork
			);
		}
	}
	// 終了
	return;
}

/**
 * 正準化で使う文脈の共通準備と限定した照会の作業量の検査関数
 * 計算量：固定した規模の投影・規則適用・照合の総費用 Q に対し O(Q)
 * @throws std::runtime_error 手期待又は計数の条件と異なる場合
 * @throws FormatLimitExceeded 呼出先の作業予算又は解析の前提を満たさない場合
 * @throws std::bad_alloc ソース又は構文のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckCanonicalContextWork() {
	// エラーの兄弟・空文・長い通常の式について，共通の文脈準備と限定した照会の作業量を測る
	for(const bool HasErrors : { false, true }) {
		size_t PrevLimitedContextWork = 0;
		for(const size_t ContextCount : { 16U, 32U, 64U }) {
			std::string ContextSrc = "void f(){";
			for(size_t Idx = 0; Idx < ContextCount; ++Idx) ContextSrc += HasErrors ? "; g(1, @, 2);" : "; g(alpha + beta + gamma);";
			ContextSrc += "}";
			const Core::SyntaxModel ContextSyntax = Project(Lang::Get(Lang::Cpp), ContextSrc);
			const size_t LimitedContextWork = Core::CanonicalizerAccess::GetLimitedContextWork(ContextSyntax);
			Check(
				LimitedContextWork && (!PrevLimitedContextWork || LimitedContextWork <= 3 * PrevLimitedContextWork),
				"limited context work grows quadratically"
			);
			PrevLimitedContextWork = LimitedContextWork;
			std::fprintf(
				stdout,
				"limited canonical contexts errors=%u n=%zu work=%zu\n",
				static_cast<unsigned>(HasErrors),
				ContextCount,
				LimitedContextWork
			);
		}
	}
	// 終了
	return;
}

/**
 * マークアップの閉タグと中間表現の共通組立の作業量の検査関数
 * 計算量：固定した規模の投影・規則適用・照合の総費用 Q に対し O(Q)
 * @throws std::runtime_error 手期待又は計数の条件と異なる場合
 * @throws FormatLimitExceeded 呼出先の作業予算又は解析の前提を満たさない場合
 * @throws std::bad_alloc ソース又は構文のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckMarkupConstructionWork() {
	// 閉タグを探す別走査を増やさず，兄弟の倍増時にドキュメントの組立を共有する
	size_t PrevMarkupBuildWork = 0;
	for(const size_t MarkupCount : { 16U, 32U, 64U }) {
		std::string MarkupSrc = "<div>";
		for(size_t Idx = 0; Idx < MarkupCount; ++Idx) MarkupSrc += "<span>x</span> ";
		MarkupSrc += "<!-- close --></div>";
		Core::SyntaxModel MarkupSyntax = Project(Lang::Get(Lang::HTML), MarkupSrc);
		const std::shared_ptr<const Core::SyntaxModel> MarkupModel = std::make_shared<const Core::SyntaxModel>(std::move(MarkupSyntax));
		const size_t MarkupStart = MarkupModel->GetWorkAccount()->Used;
		Core::FormatIR MarkupIR = Core::Canonicalizer::Build(MarkupModel);
		const size_t MarkupBuildWork = MarkupModel->GetWorkAccount()->Used - MarkupStart;
		Check(
			MarkupBuildWork && (!PrevMarkupBuildWork || MarkupBuildWork <= 3 * PrevMarkupBuildWork),
			"markup canonical construction grows quadratically"
		);
		PrevMarkupBuildWork = MarkupBuildWork;
		MarkupIR.CloseWork();
		std::fprintf(stdout, "markup canonical construction n=%zu work=%zu\n", MarkupCount, MarkupBuildWork);
	}
	// 終了
	return;
}

/**
 * 役割インデックス・捕捉集合・メンバ区切と中間表現の構築の作業量の検査関数
 * 計算量：固定した規模の投影・規則適用・照合の総費用 Q に対し O(Q)
 * @throws std::runtime_error 手期待又は計数の条件と異なる場合
 * @throws FormatLimitExceeded 呼出先の作業予算又は解析の前提を満たさない場合
 * @throws std::bad_alloc ソース又は構文のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckRoleAndCaptureWork() {
	// 役割インデックス・捕捉集合・メンバ区切の作業量の検査の入力と手期待
	struct Fixture {
		::Lang::ID Lang;
		const char *Rule;
		const char *Header;
		const char *Body;
		const char *Tail;
	};
	const Fixture Inputs[] {
		{ Lang::Cpp, nullptr, "void f(){", "if(x) g();", "}" },
		{ Lang::Cpp, nullptr, "void f(){", "while(x){g();}", "}" },
		{ Lang::Go, nullptr, "package p\nfunc f(){", "if x{g()}\n", "}\n" },
		{ Lang::Kotlin, nullptr, "", "val f = { if(a) first else second; if(b) third else fourth }\n", "" },
		{ Lang::JavaScript, nullptr, "function f(){", "var a=1;", "}" },
		{ Lang::JavaScript, nullptr, "function f(){", "if(x) g(a,b,c);", "}" },
		{ Lang::JavaScript, "Declarations", "", "", "" },
		{ Lang::TypeScript, "Terminator", "interface I{\n", "", "}\n" },
		{ Lang::Ruby, nullptr, "", "foo { first; second }\n", "" }
	};
	for(const Fixture &Input : Inputs) {
		size_t PrevWork = 0, PrevDeclWork = 0, PrevFieldWork = 0, PrevFrameRequests = 0, PrevFrameAllocations = 0, PrevBuildWork = 0;
		size_t PreviousBoolQueries = 0, PrevItems = 0, PrevBodyWork = 0, PreviousArgTextReads = 0, PreviousReadingQueries = 0;
		size_t PreviousAdjacencyQueries = 0;
		// 見送りの退行は小さい入力から倍々にし，実際の共有予算で中間表現迄構築する
		static constexpr size_t AllCounts[] { 250U, 500U, 1000U, 2000U, 4000U };
		const std::span<const size_t> Counts =
		&Input < Inputs + 2 ? std::span<const size_t>(AllCounts) : std::span<const size_t>(AllCounts).subspan(2, 2);
		for(const size_t Count : Counts) {
			std::string Src = Input.Header, Expected = Input.Header;
			for(size_t Idx = 0; Idx < Count; ++Idx) {
				const std::string Num = std::to_string(Idx);
				if(!Input.Rule) Src += Input.Body;
				else if(Input.Lang == Lang::JavaScript) {
					const std::string Val = "x" + Num + "=\"" + std::string(256, 'a') + "\";\n";
					Src += "export let " + Val;
					Expected += "export const " + Val;
				} else {
					const std::string Member = "x" + Num + "_" + std::string(128, 'a') + ":number\n";
					Src += Member;
					if(Idx) Expected += ",";
					Expected += Member;
				}
			}
			if(Input.Rule && Input.Lang == Lang::JavaScript) for(size_t Idx = 0; Idx < Count; ++Idx) {
				const std::string Function = "function f" + std::to_string(Idx) + "(){}\n";
				Src += Function;
				Expected += Function;
			}
			Src += Input.Tail;
			Expected += Input.Tail;
			const ::Lang Lang = Lang::Get(Input.Lang);
			std::array<size_t, 5> Relations {};
			std::array<size_t, 2> Facts {};
			std::array<size_t, 4> ProjectionQueries {};
			Core::SyntaxModel Syntax = Project(
				Lang,
				Src,
				&Relations,
				Input.Lang == Lang::Cpp ? &Facts : nullptr,
				Input.Lang == Lang::Cpp ? &ProjectionQueries : nullptr
			);
			if(Input.Lang == Lang::Cpp) {
				Check(Facts[0] && Facts[0] < Syntax.GetNodeCount() << 1, "boolean binding queries were repeated at the same position");
				Check(!PreviousBoolQueries || Facts[0] <= 3 * PreviousBoolQueries, "boolean binding queries exceeded linear growth");
				Check(Facts[1] <= Syntax.GetNodeCount(), "first token preparation reread a child");
				PreviousBoolQueries = Facts[0];
				Check(ProjectionQueries[0] <= Syntax.GetNodeCount(), "argument protection reread parser children");
				Check(!ProjectionQueries[1], "C++ retained regions rebuilt text without a notation separator");
				Check(
					ProjectionQueries[2] <= Count && ProjectionQueries[2] < Syntax.GetNodeCount(),
					"reading checks visited noncandidate node kinds"
				);
				Check(
					(!PreviousArgTextReads || ProjectionQueries[0] <= 3 * PreviousArgTextReads) &&
					(!PreviousReadingQueries || ProjectionQueries[2] <= 3 * PreviousReadingQueries) &&
					(!PreviousAdjacencyQueries || ProjectionQueries[3] <= 3 * PreviousAdjacencyQueries),
					"projection queries exceeded linear growth"
				);
				PreviousArgTextReads = ProjectionQueries[0];
				PreviousReadingQueries = ProjectionQueries[2];
				PreviousAdjacencyQueries = ProjectionQueries[3];
			}
			Check(Relations[3] <= Relations[4] * (Relations[0] + 1), "field completion repeated a parent query");
			Check(!PrevFieldWork || Relations[3] <= 3 * PrevFieldWork, "field completion exceeded linear growth");
			PrevFieldWork = Relations[3];
			if(Input.Lang == Lang::Cpp) {
				Check(Relations[1] == Count && Relations[1] == Relations[2], "declaration repair visited noncandidates or repeated candidates");
				Check(!PrevDeclWork || Relations[1] <= 3 * PrevDeclWork, "declaration candidates exceeded linear growth");
				PrevDeclWork = Relations[1];
			}
			if(Input.Lang == Lang::Kotlin || Input.Lang == Lang::Ruby) {
				const std::array<size_t, 3> BodyWork = Core::CanonicalizerAccess::GetBodySummaryWork(Syntax);
				Check(!PrevBodyWork || BodyWork[0] <= (PrevBodyWork << 1) + 64, "body counting exceeded linear growth");
				PrevBodyWork = BodyWork[0];
				std::fprintf(stdout, "body n=%zu first=%zu reused=%zu bodies=%zu\n", Count, BodyWork[0], BodyWork[1], BodyWork[2]);
			}
			const size_t Before = Syntax.GetWorkAccount()->Used;
			if(Input.Rule) {
				const Core::SyntaxModel Oracle = Project(Lang, Expected);
				Core::SyntaxNormalizerAccess::RunIndividualRule(Syntax, Core::SyntaxNormalizerAccess::GetFixtureRule(Input.Rule));
				Check(GetOracleLexemes(Syntax) == GetOracleLexemes(Oracle), "a fifth review result differs from its handwritten oracle");
				Check(GetOracleBranches(Syntax) == GetOracleBranches(Oracle), "a fifth review result changed branch ownership");
			} else {
				std::vector<std::pair<std::string_view, size_t>> Rules;
				Core::SyntaxNormalizerAccess::Run(Syntax, &Rules);
				if(&Input < Inputs + 2) {
					// 任意の本体の呼出を保ち，単一文の波括弧を除き，到達する無値の末尾へ返戻だけを補う
					Expected = Input.Header;
					for(size_t Idx = 0; Idx < Count; ++Idx) Expected += &Input == Inputs ? Input.Body : "while(x)g();";
					Expected += "return;";
					Expected += Input.Tail;
					const Core::SyntaxModel Oracle = Project(Lang, Expected);
					Check(GetOracleLexemes(Syntax) == GetOracleLexemes(Oracle), "conditional body return completion changed tokens");
					Check(GetOracleBranches(Syntax) == GetOracleBranches(Oracle), "conditional body return completion changed branches");
					#if !defined(ShaveFormatRefFacts)
					Check(!Core::SyntaxNormalizerAccess::GetBindingRegionCount(Syntax), "unbound calls built a binding use region");
					Check(
						Core::SyntaxNormalizerAccess::GetCalleeStorage(Syntax) == std::pair<size_t, size_t>(0, 0),
						"leaf call targets built a subtree table"
					);
					#endif
					for(const auto &[Rule, Used] : Rules) {
						if(Rule == "ExplicitReturn") {
							Check(Used <= (&Input == Inputs ? 96 * Count + 512 : 120 * Count + 1024), "return completion retained unnecessary call work");
						}
						if(&Input == Inputs + 1 && Rule == "Braces") {
							Check(Used <= 160 * Count + 1024, "brace removal retained unnecessary binding work");
						}
					}
				}
				const std::shared_ptr<const Core::SyntaxModel> Normalized = std::make_shared<const Core::SyntaxModel>(std::move(Syntax));
				std::pair<size_t, size_t> Frames;
				const size_t BeforeBuild = Normalized->GetWorkAccount()->Used;
				Core::FormatIR Intermediate = Core::CanonicalizerAccess::BuildWithWork(Normalized, Frames);
				const size_t BuildWork = Normalized->GetWorkAccount()->Used - BeforeBuild;
				// 投影・正規化の費用と分けて，正準化の共有要約と組立の作業量を倍増時に照合する
				Check(BuildWork && (!PrevBuildWork || BuildWork <= (PrevBuildWork << 1) + 64), "canonical construction repeated work");
				if(&Input < Inputs + 2) {
					Check(BuildWork <= (&Input == Inputs ? 405 : 420) * Count + 2048, "conditional body canonical work exceeded its linear bound");
				}
				PrevBuildWork = BuildWork;
				Check(!PrevItems || Intermediate.Items.size() <= (PrevItems << 1) + 64, "canonical IR exceeded linear growth");
				PrevItems = Intermediate.Items.size();
				Check(Frames.first && Frames.second < Frames.first, "flat construction did not reuse coroutine frames");
				Check(!PrevFrameRequests || Frames.first <= (PrevFrameRequests << 1) + 64, "coroutine requests exceeded linear growth");
				Check(!PrevFrameAllocations || Frames.second <= 3 * PrevFrameAllocations, "coroutine allocations exceeded linear growth");
				PrevFrameRequests = Frames.first;
				PrevFrameAllocations = Frames.second;
				std::fprintf(
					stdout,
					"shared projection n=%zu relation=%zu decl=%zu fields=%zu frames=%zu allocations=%zu\n",
					Count,
					Relations[0],
					Relations[1],
					Relations[3],
					Frames.first,
					Frames.second
				);
				const size_t Work = Normalized->GetWorkAccount()->Used - Before;
				Intermediate.CloseWork();
				std::fprintf(
					stdout,
					"fifth review pipeline lang=%u n=%zu work=%zu build=%zu\n",
					static_cast<unsigned>(Input.Lang),
					Count,
					Work,
					BuildWork
				);
				Check(Work && (!PrevWork || Work <= 3 * PrevWork), "flat pipeline work grows quadratically");
				if(&Input < Inputs + 2 && PrevWork) Check(Work <= (PrevWork << 1) + 64, "conditional body pipeline exceeded linear work");
				if(&Input == Inputs + 1) Check(Work <= 900 * Count + 4096, "loop pipeline retained unnecessary work");
				PrevWork = Work;
				continue;
			}
			const size_t Work = Syntax.GetWorkAccount()->Used - Before;
			std::fprintf(stdout, "fifth review rule=%s n=%zu work=%zu\n", Input.Rule, Count, Work);
			Check(Work && (!PrevWork || Work <= 3 * PrevWork), "fifth review work grows quadratically");
			PrevWork = Work;
		}
	}
	// 終了
	return;
}

/**
 * 深い呼出の行末コメントの配置と親の照会の作業量の検査関数
 * 計算量：固定した規模の投影・規則適用・照合の総費用 Q に対し O(Q)
 * @throws std::runtime_error 手期待又は計数の条件と異なる場合
 * @throws FormatLimitExceeded 呼出先の作業予算又は解析の前提を満たさない場合
 * @throws std::bad_alloc ソース又は構文のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckTrailingCommentWork() {
	// 呼出の各閉じに行末コメントを持つ深い構文の，役割・親の照会数の線形性
	for(const char *const LangName : { "cpp", "tsx" }) for(const size_t Chars : { 0U, 40U }) {
		size_t PrevWork = 0;
		for(const size_t Count : { 128U, 256U, 512U, 1024U }) {
			std::string Src = std::string(LangName) == "cpp" ? "int X=" : "const X=<div>{";
			for(size_t Idx = 0; Idx < Count; ++Idx) Src += "F(\n";
			Src += "1\n";
			for(size_t Idx = 0; Idx < Count; ++Idx) Src += ") /* c */\n";
			Src += std::string(LangName) == "cpp" ? ";\n" : "}</div>;\n";
			// 正規の投影・同期・本文正規化を通し，構文の予算とは独立して配置の作業を数える
			Core::ProjectionResult Projection = ProjectWithCtx(Lang::FromName(LangName), Src);
			Projection.Comments.Remap(Core::SyntaxNormalizer().Normalize(Projection.Syntax));
			const std::shared_ptr<const Core::SyntaxModel> Syntax = std::make_shared<Core::SyntaxModel>(std::move(Projection.Syntax));
			Core::FormatIR IR = Core::Canonicalizer::Build(Syntax);
			IR.CloseWork();
			const Core::BrokenIR Broken = Core::LineBreaker(Chars).Resolve(std::move(IR));
			const std::shared_ptr<const LaidOutCode> Code = std::make_shared<const LaidOutCode>(Core::CodeLayout::Materialize(Broken));
			Core::DocCommentSynchronizer::Synchronize(*Syntax, Projection.Comments);
			Core::DocCommentSynchronizer::ConfirmGroups(*Code, Projection.Comments);
			CommentNormalizer::Normalize(Projection.Comments);
			const size_t Work = Core::CommentPlacerAccess::GetCandidateWork(*Code, Projection.Comments, Count);
			Check(Work && (!PrevWork || Work <= 3 * PrevWork), "nested call comment ancestry work grows quadratically");
			// 行末の種別・記法・本文は配置後も保持し，個数だけの検査で脱落や移動を見逃さない
			const CommentPlacementPlan Plan = Core::CommentPlacer::Place(*Code, Projection.Comments);
			Check(Plan.GetInsertions().size() == Count, "nested call comments lost an insertion");
			for(const CommentInsertion &Insertion : Plan.GetInsertions()) {
				Check(Insertion.GetSlot().Kind == CommentSlotKind::LineEnd, "a nested call comment moved away from the line end");
				const std::span<const std::string> Lines = Insertion.GetComment().GetLines();
				Check(Lines.size() == 1 && Lines.front() == " // c", "a nested call comment changed its notation or body");
			}
			std::fprintf(stdout, "trailing comment ancestry lang=%s chars=%zu n=%zu work=%zu\n", LangName, Chars, Count, Work);
			PrevWork = Work;
		}
	}
	// 終了
	return;
}

/**
 * 不変指定候補が空の時の未使用要約の省略の検査関数
 * 固定した束縛１つに，判定と無関係な本文を倍増して追加する
 * 計算量：固定した倍増規模の投影・役割インデックス構築の総費用 Q に対し O(Q)
 * @throws std::runtime_error 候補が空でないか，無関係な本文の増加で収集作業が増える場合
 * @throws FormatLimitExceeded 投影・インデックス又は候補収集の作業予算を超える場合
 * @throws std::bad_alloc ソース又は構文のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckEmptyImmutableWork() {
	// 前の規模で使った作業量の記録準備
	size_t PrevWork = 0, PreviousSlots = 0;
	for(const size_t Count : { 128U, 256U }) {
		std::string Src = "fn f(){let x=1;";
		for(size_t Idx = 0; Idx < Count; ++Idx) Src += "consume(1);";
		Src += "}";
		Core::SyntaxModel Syntax = Project(Lang::Get(Lang::Rust), Src);
		Syntax.BuildRoleIdx();
		const std::pair<size_t, size_t> Storage = Core::SyntaxNormalizerAccess::GetRoleStorage(Syntax);
		Check(Storage.second == Syntax.GetNodeCount(), "role ranges did not cover the node table");
		Check(Storage.first && (!PreviousSlots || Storage.first <= 3 * PreviousSlots), "flat role storage exceeded linear growth");
		PreviousSlots = Storage.first;
		const size_t Before = Syntax.GetWorkAccount()->Used;
		const std::pair<std::unordered_set<Core::NodeId>, Core::RewritePlan> Candidates =
		Core::SyntaxNormalizerAccess::CollectImmutableCandidates(Syntax);
		const size_t Work = Syntax.GetWorkAccount()->Used - Before;
		Check(Candidates.first.empty() && Candidates.second.Steps.empty(), "an immutable binding produced a mutable candidate");
		Check(Work && (!PrevWork || Work == PrevWork), "an empty candidate set rebuilt unrelated scope facts");
		std::fprintf(stdout, "empty immutable candidates n=%zu work=%zu\n", Count, Work);
		PrevWork = Work;
	}
	// 終了
	return;
}

/**
 * 通常・被覆・悲観の仮定を混同しない末尾判定の検査関数
 * 計算量：固定テストケースの投影・終端判定の総費用 Q に対し O(Q)
 * @throws std::runtime_error 到達可能性が手期待と異なる場合
 * @throws FormatLimitExceeded 投影又は判定が作業予算を超える場合
 * @throws std::bad_alloc 構文又は判定のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckTerminationModes() {
	// 同じリテラルの真偽を，各言語の反復形と脱出の有無で共有する
	for(const Lang::ID Language : { Lang::C, Lang::Cpp, Lang::PHP, Lang::JavaScript, Lang::TypeScript, Lang::Python }) {
		for(const std::string_view Condition : { "\"\"", "\"0\"", "\"x\"", "((2))", "-1", "0", "0x2", "1.0", "2" }) {
			const bool IsTrue = Condition != "0" && (Condition != "\"\"" || Language == Lang::C || Language == Lang::Cpp) &&
			(Condition != "\"0\"" || Language != Lang::PHP);
			for(const unsigned Form : { 0U, 1U, 2U }) {
				if(Language == Lang::Python && Form) continue;
				for(const bool HasBreak : { false, true }) {
					std::string Source;
					if(Language == Lang::Python) {
						Source = "def f():\n\twhile " + std::string(Condition) + ":\n\t\t" + (HasBreak ? "break" : "pass") + "\n";
					} else {
						Source = Language == Lang::PHP ?
						"<?php function f(): void{" :
						Language == Lang::TypeScript ? "function f(): void{" : Language == Lang::JavaScript ? "function f(){" : "void f(){";
						const std::string Body = HasBreak ? "{break;}" : "{}";
						if(!Form) Source += "while(" + std::string(Condition) + ")" + Body;
						else if(Form == 1) Source += "for(;" + std::string(Condition) + ";)" + Body;
						else Source += "do" + Body + "while(" + std::string(Condition) + ");";
						Source += "}";
					}
					Core::SyntaxModel Syntax = Project(Lang::Get(Language), Source);
					Syntax.BuildRoleIdx();
					const std::vector<Core::NodeId> Functions = GetNodesWithRole(Syntax, Core::Role::OrdinaryFunctionDef);
					Check(Functions.size() == 1, "a constant-condition input lost its function");
					Core::SyntaxNormalizerAccess::CheckSimpleEndReach(Syntax, Functions.front());
					const Core::ControlFlow Flow(Syntax);
					const std::optional<Core::FunctionEnd> End = Flow.ClassifyFunction(Functions.front());
					Check(
						End && Flow.EndReach(Functions.front(), *End) ==
						(IsTrue && !HasBreak ? Core::EndReachability::Unreachable : Core::EndReachability::Reachable),
						(std::string("a literal condition changed loop reachability: ") + Source).c_str()
					);
				}
			}
		}
	}
	// 評価位置の雛形へ終端を１つ差し込み，言語の外枠と期待を独立して組み立てる
	struct EvaluationLanguage {
		::Lang::ID Lang;
		std::string_view Prefix;
		std::string_view Exit;
		std::string_view Suffix;
	};
	const EvaluationLanguage Languages[] { { Lang::Cpp, "[[noreturn]] int stop();void f(int x){", "stop()", "}" } };
	struct EvaluationPosition {
		::Lang::ID Lang;
		std::string_view Pattern;
		Core::EvaluationClass Class;
		std::string_view Setup {}; // 外枠より前の導入条件
	};
	using Eval = Core::EvaluationClass;
	const EvaluationPosition Positions[] {
		{ Lang::Cpp, "g(@exit@);", Eval::Required },
		{ Lang::Cpp, "x && @exit@;", Eval::Conditional },
		{ Lang::Cpp, "sizeof(@exit@);", Eval::Unevaluated },
		{ Lang::Cpp, "auto g=[] { @exit@; };", Eval::Deferred },
		{ Lang::Cpp, "new(std::nothrow) int(@exit@);", Eval::Unknown },
		{ Lang::Cpp, "g(@exit@);", Eval::Unknown, "#include <new>\n#undef stop\n" }
	};
	for(const EvaluationPosition &Position : Positions) {
		const EvaluationLanguage *const Language = std::ranges::find(Languages, Position.Lang, &EvaluationLanguage::Lang);
		Check(Language != std::end(Languages), "an evaluation position lost its language envelope");
		std::string Body(Position.Pattern);
		const size_t Marker = Body.find("@exit@");
		Check(Marker != std::string::npos, "an evaluation position lost its terminal marker");
		Body.replace(Marker, 6, Language->Exit);
		std::string Prefix(Language->Prefix);
		const std::string Src = std::string(Position.Setup) + Prefix + Body + std::string(Language->Suffix);
		Core::SyntaxModel Syntax = Project(Lang::Get(Position.Lang), Src);
		Syntax.BuildRoleIdx();
		const std::vector<Core::NodeId> Functions = GetNodesWithRole(Syntax, Core::Role::OrdinaryFunctionDef);
		Check(!Functions.empty(), "an evaluation position lost its function");
		const Core::NodeId Function = Functions.front();
		const Core::ControlFlow Flow(Syntax);
		std::optional<Core::FunctionEnd> End = Flow.ClassifyFunction(Function);
		// 静的解析と同じ順で先に文列を照会し，末尾の判定の契約が共用後も変わらない事を確かめる
		if(End) Flow.GetReturningStmtsAfterTermination(End->Stmts, Flow.CollectGotoTargets(End->Stmts));
		const bool IsRequired = Position.Class == Eval::Required, IsUnknown = Position.Class == Eval::Unknown;
		const Core::EndReachability Expected =
		IsUnknown ? Core::EndReachability::Unknown : IsRequired ? Core::EndReachability::Unreachable : Core::EndReachability::Reachable;
		Check(
			End && Flow.EndReach(Function, *End) == Expected,
			(std::string("an evaluation position changed reachability: ") + Src).c_str()
		);
		if(End->Kind == Core::ReturnKind::Void) {
			const Core::RewritePlan Plan = Core::SyntaxNormalizerAccess::CollectExplicitReturns(Syntax, Function);
			Check(
				Plan.Steps.empty() == (IsRequired || IsUnknown),
				(std::string("an evaluation position changed tail completion: ") + Src).c_str()
			);
		}
	}
	// 同じ祖先・連鎖の右辺・導入時の供給値を，全ての別名の照会で共有する
	const Formatter AliasShaver { Core::LineBreaker(0) };
	for(const bool IsChain : { false, true }) for(const bool IsModule : { false, true }) {
		size_t Previous = 0;
		for(const size_t Count : { 32U, 64U, 128U, 256U, 512U }) {
			std::string Text = "import sys\n";
			for(size_t Idx = 0; Idx < Count; ++Idx) {
				Text += "g" + std::to_string(Idx) + " = ";
				if(!IsChain) Text += Idx ? "g" + std::to_string(Idx - 1) + "\n" : IsModule ? "sys\n" : "sys.exit\n";
			}
			if(IsChain) Text += IsModule ? "sys\n" : "sys.exit\n";
			for(size_t Idx = 0; Idx < Count; ++Idx) {
				Text += "def f" + std::to_string(Idx) + "():\n g" + std::to_string(Idx) + (IsModule ? ".exit()\n" : "()\n");
			}
			const Core::SyntaxModel Syntax = Project(Lang::Get(Lang::Python), Text);
			const std::vector<Core::NodeId> Calls = GetNodesWithRole(Syntax, Core::Role::CallNode);
			Check(Calls.size() == Count, "an alias-work fixture lost its calls");
			const Core::ControlFlow Flow(Syntax);
			for(const Core::NodeId Call : Calls) {
				const std::pair<std::string, bool> Result = Core::SyntaxNormalizerAccess::GetCallResolution(Flow, Call);
				Check(Result.first == "sys.exit" && !Result.second, "alias sharing changed a confirmed supply value");
			}
			const size_t Work = Syntax.GetWorkAccount()->Used;
			if(Previous) Check(Work <= 5 * Previous >> 1, "alias resolution grows quadratically");
			Previous = Work;
			std::fprintf(stdout, "alias chain=%d module=%d n=%zu work=%zu\n", IsChain, IsModule, Count, Work);
			const FormatResult Formatted = AliasShaver.Format(Text, Lang::Get(Lang::Python), true);
			Check(
				Formatted.Classification == FormatClassification::Changed || Formatted.Classification == FormatClassification::Unchanged,
				"a shared alias chain exceeded the whole formatting budget"
			);
		}
	}
	// 同じ根の無関係なメンバ代入を，独立した関数の照会毎に再走査しない
	for(const size_t Count : { 64U, 128U, 256U, 512U }) {
		std::string Text = "import sys\nclass O: pass\no = O()\n";
		for(size_t Idx = 0; Idx < Count; ++Idx) Text += "o.data = " + std::to_string(Idx) + "\n";
		for(size_t Idx = 0; Idx < Count; ++Idx) Text += "def f" + std::to_string(Idx) + "():\n sys.exit()\n";
		Core::SyntaxModel Syntax = Project(Lang::Get(Lang::Python), Text);
		const std::vector<Core::NodeId> Calls = GetNodesWithRole(Syntax, Core::Role::CallNode);
		Check(Calls.size() == Count + 1, "a member-work fixture lost its constructor or standard calls");
		const Core::ReferenceProjection Flow(GrammarProfile::Resolve(Lang::Get(Lang::Python)), Syntax);
		const std::pair<bool, size_t> First = Core::SyntaxNormalizerAccess::GetMemberQueryWork(Syntax, Flow, Calls[1], "sys.exit");
		const std::pair<bool, size_t> Last = Core::SyntaxNormalizerAccess::GetMemberQueryWork(Syntax, Flow, Calls.back(), "sys.exit");
		Check(!First.first && !Last.first, "unrelated member assignments shadowed a standard call");
		if(Count == 512) Check(Last.second < Count, "a shared member summary scanned every unrelated assignment again");
		std::fprintf(stdout, "member summary n=%zu first=%zu repeated=%zu\n", Count, First.second, Last.second);
	}
	// 終了
	return;
}

/**
 * テキストを保って構文事実を変えた反例の観測の検査関数
 * 計算量：固定した反例の観測の総量 Q に対し O(Q)
 * @throws std::runtime_error 構文事実の違いを観測が見落した場合
 * @throws std::bad_alloc 構文又は観測のメモリを確保出来ない場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckObservationFacts() {
	// 観測の保持を照合する原文の投影
	// ノード表の移動と所有者の破棄を跨いでも，複写した子列は独立して生存する
	Core::SyntaxNode DetachedNode;
	const Core::SyntaxModel Original = [&DetachedNode]() -> Core::SyntaxModel {
		Core::SyntaxModel Pooled = Project(Lang::Get(Lang::C), "int value=1;\n");
		Core::SyntaxModel Moved = std::move(Pooled);
		DetachedNode = Moved.GetNode(Moved.GetRoot());
		// 所有者の異なる構文の返戻
		return Core::SyntaxModel(Moved);
	}();
	Check(
		std::ranges::equal(DetachedNode.Children, Original.GetChildrenOf(Original.GetRoot())),
		"copied children outlived their storage"
	);
	const size_t BeforeObservation = Original.GetWorkAccount()->Used;
	const std::string Expected = ObservationOf(Original);
	Check(Original.GetWorkAccount()->Used == BeforeObservation, "read-only observation changed the product work budget");
	const Core::NodeId Scope = Original.GetNamedChild(Original.GetRoot(), 0);
	for(const size_t Idx : { 0U, 1U, 2U, 3U, 4U, 5U, 6U }) {
		Core::SyntaxModel Changed = Original;
		Core::SyntaxNormalizerAccess::EditObservationFact(
			Changed,
			Changed.GetRoot(),
			[Idx, Scope](Core::SyntaxFacts &Facts) -> void {
				// 対象の種類に従う処理の選択
				switch(Idx) {
				case 0:
					++Facts.BlankKey;
					break;
				case 1:
					Facts.Add(Core::ConstraintKind::NoOptionalBreak);
					break;
				case 2:
					Facts.HasTrueName ^= true;
					break;
				case 3:
					Facts.Boundaries.Edit().push_back({ 0, Core::BoundaryKind::RequiredSpace });
					break;
				case 4:
					Facts.HasFalseName ^= true;
					break;
				case 5:
					Facts.NeedsAngleParens ^= true;
					break;
				case 6:
					Facts.ImportScope = Scope;
					break;
				}
				// 終了
				return;
			}
		);
		Check(ObservationOf(Changed) != Expected, "syntax observation ignored a changed fact");
	}
	// 導入先の対応を保つ番号の移動は観測を変えない
	Core::SyntaxModel Scoped = Original;
	Core::SyntaxNormalizerAccess::EditObservationFact(
		Scoped,
		Scoped.GetRoot(),
		[Scope](Core::SyntaxFacts &Facts) -> void {
			Facts.ImportScope = Scope;
			// 終了
			return;
		}
	);
	const std::string ScopeObservation = ObservationOf(Scoped);
	Core::SyntaxNormalizerAccess::ShiftObservationNodes(Scoped);
	Check(ObservationOf(Scoped) == ScopeObservation, "syntax observation compared raw import scope identifiers");
	// 評価契約だけを変える反例の基準
	Core::KindDef BaseDef = Core::SyntaxNormalizerAccess::GetObservationKind(Original, Original.GetRoot());
	BaseDef.Evaluation = Core::EvaluationDef();
	BaseDef.Evaluation.Inputs.emplace_back();
	Core::SyntaxModel Base = Original;
	Core::SyntaxNormalizerAccess::SetObservationKind(Base, Base.GetRoot(), BaseDef);
	const std::string BaseObservation = ObservationOf(Base);
	// 各フィールドを１つだけ変え，任意値の無指定と値０，列の要素数も区別する
	for(size_t Idx = 0; Idx < 29; ++Idx) {
		Core::KindDef Def = BaseDef;
		Core::EvaluationDef &Evaluation = Def.Evaluation;
		Core::EvaluationInput &Input = Evaluation.Inputs.front();
		// 反例で変える１つの契約の選択
		switch(Idx) {
		case 0:
			Evaluation.Rule = Core::EvaluationRule::Branch;
			break;
		case 1:
			Evaluation.Children = Core::EvaluationClass::Required;
			break;
		case 2:
			Evaluation.Invocation = Core::EvaluationClass::Unknown;
			break;
		case 3:
			Evaluation.FirstInput = Core::ChildRole::Target;
			break;
		case 4:
			Evaluation.CatchInput = Core::ChildRole::Body;
			break;
		case 5:
			Evaluation.ConditionField = Core::ChildRole::Val;
			break;
		case 6:
			Evaluation.ConditionIndex = 0;
			break;
		case 7:
			Evaluation.UnevaluatedHead = "sizeof";
			break;
		case 8:
			Evaluation.UnknownHead = "typeid";
			break;
		case 9:
			Evaluation.HasDeferredArguments = true;
			break;
		case 10:
			Evaluation.DoesEvaluateChildInputs = true;
			break;
		case 11:
			Evaluation.HasEntry = true;
			break;
		case 12:
			Evaluation.IsContainer = true;
			break;
		case 13:
			Evaluation.IsOrdered = true;
			break;
		case 14:
			Evaluation.MaySkipInputs = true;
			break;
		case 15:
			Evaluation.IsRequiredFirst = true;
			break;
		case 16:
			Evaluation.ThrowBeforeField = Core::ChildRole::Body;
			break;
		case 17:
			Evaluation.ConditionalOperators.push_back("&&");
			break;
		case 18:
			Evaluation.Inputs.emplace_back();
			break;
		case 19:
			Input.Field = Core::ChildRole::Val;
			break;
		case 20:
			Input.NamedIndex = 1;
			break;
		case 21:
			Input.Class = Core::EvaluationClass::Deferred;
			break;
		case 22:
			Input.WhenPresent = Core::ChildRole::Body;
			break;
		case 23:
			Input.ChildKind = Core::Role::Identifier;
			break;
		case 24:
			Input.WhenToken = "?";
			break;
		case 25:
			Input.AfterToken = ":";
			break;
		case 26:
			Input.UnlessKind = Core::Role::Identifier;
			break;
		case 27:
			Input.WhenSiblingKind = Core::Role::Identifier;
			break;
		case 28:
			Evaluation.ConditionIndex = 1;
			break;
		}
		Core::SyntaxModel Changed = Original;
		Core::SyntaxNormalizerAccess::SetObservationKind(Changed, Changed.GetRoot(), Def);
		Check(ObservationOf(Changed) != BaseObservation, "syntax observation ignored a changed evaluation contract");
	}
	// 終了
	return;
}

// 診断検査の１度の事実収集で共有参照とドキュメント属性を照合する型
struct LintFactsVerification {
	/**
	 * 共有参照と先行する属性を照合し，既存の警告の検査を進める関数
	 * 計算量：静的解析の費用 Q と期待属性の位置の探索費用 B に対し O(Q + B)
	 * @param Output 検証済の最終投影と位置情報
	 * @param ExpectedAttribute 手期待の属性の綴り（空なら属性の位置を照合しない）
	 * @param Docs 論理行の複写と警告位置の照合の計数（無ければ観測しない）
	 * @return 最終構文に対する警告
	 * @throws std::runtime_error 共有参照又は属性の位置が契約と異なる場合
	 * @throws FormatLimitExceeded 静的解析が作業予算を超える場合
	 * @throws std::bad_alloc 事実又は警告のメモリを確保出来ない場合
	 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
	 * @throws std::length_error 作業列の要素数上限を超える場合
	 * @throws std::out_of_range 照会するノードが表に無い場合
	 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
	 */
	static std::vector<LintWarning> Analyze(
		const Core::ValidatedOutput &Output,
		const std::string_view ExpectedAttribute,
		std::array<size_t, 5> *const Docs = nullptr
	) {
		const StaticLint::OutputFacts Facts = LintAnalyzer::ReadFacts(Output);
		Check(
			Facts.Positions.data() == Output.Locations.Nodes.data() && Facts.Positions.size() == Output.Locations.Nodes.size() &&
			Facts.LineStarts.data() == Output.Locations.LineStarts.data() &&
			Facts.LineStarts.size() == Output.Locations.LineStarts.size() &&
			Facts.DirectiveStarts.data() == Output.Locations.DirectiveStarts.data() &&
			Facts.DirectiveStarts.size() == Output.Locations.DirectiveStarts.size(),
			"diagnostic facts copied validated locations"
		);
		if(!ExpectedAttribute.empty()) {
			Check(Facts.Functions.size() == 1, "an attribute fixture lost its function");
			const StaticLint::FunctionFacts &Function = Facts.Functions.begin()->second;
			Check(Function.HasDocAttribute, "a leading documentation attribute was lost");
			Check(
				Facts.Positions[Function.DocAttribute].Start == Output.Locations.Text.find(ExpectedAttribute),
				"a later attribute replaced the first documentation attribute"
			);
		}
		// 既存の各入力の解析だけで，ノード番号の表が１つの固定容量を保つ事を照合する
		LintAnalyzer Analyzer(Output.FinalSyntax, Output.Locations.Text, Output.FinalComments, Facts);
		LintAnalyzer::DocumentationWork DocWork;
		if(Docs) Analyzer.DocWork = &DocWork;
		std::vector<LintWarning> Warnings = Analyzer.Run();
		if(Docs) *Docs = { DocWork.Bindings, DocWork.Bytes, DocWork.Lines, DocWork.Points, DocWork.RuleCopies };
		const size_t NodeCount = Output.FinalSyntax.GetNodeCount();
		Check(
			Output.FinalSyntax.GetRoot() == Core::NoNode || Analyzer.BindingRefs.size() == NodeCount,
			"binding tables did not use the syntax node range"
		);
		// １度収集した事実を使う警告の返戻
		return Warnings;
	}
};

/**
 * 手ソースの投影に対する読取専用の警告の取得関数
 * 計算量：投影・位置の取得・静的解析の総費用 Q に対し O(Q)
 * @param Lang テストケースの言語
 * @param Src 最終テキストとして読む手ソース
 * @param ExpectedAttribute 手期待の属性の綴り（空なら属性の位置を照合しない）
 * @param Work 静的解析だけの作業量の格納先（無ければ計数の結果を渡さない）
 * @param Docs 論理行の複写と警告位置の照合の計数（無ければ観測しない）
 * @return 最終構文に対する警告
 * @throws std::runtime_error 手ソースの投影が失敗した場合
 * @throws FormatLimitExceeded 投影又は静的解析の上限を超えた場合
 * @throws std::bad_alloc 投影・診断位置又は警告の確保に失敗した場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static std::vector<LintWarning> AnalyzeDiagnosticCase(
	const ::Lang Lang,
	const std::string &Src,
	const std::string_view ExpectedAttribute = std::string_view(),
	size_t *Work = nullptr,
	std::array<size_t, 5> *const Docs = nullptr
) {
	// 診断対象の原文の投影
	Core::ProjectionResult Projection = ProjectDiagnosticCase(Lang, Src, false);
	Core::DiagnosticLocationMap Locations;
	Locations.Text = Src;
	Locations.Nodes = std::move(Projection.Validation.InputDiagnostics.Positions);
	Locations.LineStarts.push_back(0);
	for(size_t Idx = 0; Idx < Src.size(); ++Idx) if(Src[Idx] == '\n') {
		Locations.LineStarts.push_back(static_cast<uint32_t>(Idx + 1));
	}
	// 製品と同じく，遅延した名前の事実と準備前の診断位置を静的解析より先に確定する
	Check(static_cast<bool>(Projection.PrepareLint), "a diagnostic fixture did not defer lint facts");
	Projection.PrepareLint(Projection.Syntax, Projection.Comments, Locations);
	const Core::ValidatedOutput Output { std::move(Projection.Syntax), std::move(Projection.Comments), std::move(Locations) };
	const std::shared_ptr<const Core::StructureWorkAccount> Account = Output.FinalSyntax.GetWorkAccount();
	const size_t Before = Account->Used;
	std::vector<LintWarning> Warnings = LintFactsVerification::Analyze(Output, ExpectedAttribute, Docs);
	if(Work) *Work = Account->Used - Before;
	// 独立して投影した手ソースの警告の返戻
	return Warnings;
}

/**
 * 括弧・宣言・静的解析の倍増検査関数
 * 計算量：各入力の投影・正規化の総量 Q に対し O(Q)
 * @throws std::runtime_error 手期待又は倍増時の作業量が仕様と異なる場合
 * @throws FormatLimitExceeded 所定の予算を超える場合
 * @throws std::bad_alloc 入力又は構文のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckNormalizationRegressionScale() {
	// 末尾の返戻が明示された本体は，補完しない子孫の分類・到達可能性の判定を要しない
	for(const Lang::ID Language : { Lang::Go, Lang::Kotlin, Lang::JavaScript }) {
		size_t Previous = 0;
		for(const size_t Count : { 64U, 128U, 256U }) {
			std::string Src =
			Language == Lang::Go ? "package p\nfunc F(){\n" : Language == Lang::Kotlin ? "fun F(): Unit {\n" : "function F(){\n";
			for(size_t Idx = 0; Idx < Count; ++Idx) Src += "G();\n";
			Src += "return\n}\n";
			Core::SyntaxModel Syntax = Project(Lang::Get(Language), Src);
			Check(
				!Syntax.GetNode(Syntax.GetRoot()).Facts.Has(Core::ConstraintKind::ContainsError),
				"explicit return scale source is unreadable"
			);
			const std::vector<Core::NodeId> Functions = GetNodesWithRole(Syntax, Core::Role::ExplicitResultFunction);
			Check(Functions.size() == 1, "explicit return scale source lost its function");
			Syntax.BuildRoleIdx();
			const size_t Before = Syntax.GetWorkAccount()->Used;
			const Core::RewritePlan Plan = Core::SyntaxNormalizerAccess::CollectExplicitReturns(Syntax, Functions.front());
			const size_t Work = Syntax.GetWorkAccount()->Used - Before;
			Check(Plan.Steps.empty(), "an explicit terminal return acquired a completion plan");
			#if !defined(ShaveFormatRefFacts)
			Check(Work && (!Previous || Work == Previous), "an explicit terminal return rescanned its growing body");
			#else
			static_cast<void>(Previous);
			#endif
			std::printf("explicit terminal return lang=%u n=%zu work=%zu\n", static_cast<unsigned>(Language), Count, Work);
			Previous = Work;
		}
	}
	// 長い YARD の説明文は１回だけ所有列へコピーし，形式と説明が揃えば警告位置の表を作らない
	const ::Lang Ruby = Lang::Get(Lang::Ruby);
	size_t PreviousDocBytes = 0, PreviousDocMatchWork = 0;
	std::array<size_t, 4> PreviousLabelQueries {};
	for(const size_t Count : { 16U, 32U, 64U }) {
		std::string Src = "# Summary\n";
		for(size_t Idx = 0; Idx < Count; ++Idx) Src += "# More context\n";
		Src += "# @param x explanation\n# @return [Object] value\ndef f(x)\n  x\nend\n";
		std::array<size_t, 5> Docs {};
		const std::vector<LintWarning> Warnings = AnalyzeDiagnosticCase(Ruby, Src, std::string_view(), nullptr, &Docs);
		const size_t ExpectedBytes = 7 + Count * 12 + 20 + 22;
		Check(Docs[1] == ExpectedBytes && Docs[2] == Count + 3, "YARD lines were copied more than once or lost content");
		Check(!Docs[0] && !Docs[3] && !Docs[4], "complete YARD docs rebuilt warning facts");
		for(const LintWarning &Warning : Warnings) {
			Check(
				Warning.Rule != LintRule::DocumentationSummary && Warning.Rule != LintRule::DocumentationParameter &&
				Warning.Rule != LintRule::DocumentationReturn,
				"complete YARD docs acquired a missing-description warning"
			);
		}
		if(PreviousDocBytes) Check(Docs[1] <= PreviousDocBytes << 1, "document copies exceeded linear growth");
		PreviousDocBytes = Docs[1];
		// 未変更の本文は，公開する保護マスクを前の構造と共有する
		Core::ProjectionResult InputProjection = ProjectWithCtx(Ruby, Src);
		const std::vector<ValidationContext::DocFact> ExpectedDocs = Core::SourceProjector::DescribeDocs(
			InputProjection.Syntax,
			InputProjection.Comments.GetRecords(),
			Core::DocCommentReader::GetDocTargets(InputProjection.Syntax, InputProjection.Comments)
		);
		size_t InitialIndexWork = 0;
		Core::DocCommentSynchronizer::Synchronize(InputProjection.Syntax, InputProjection.Comments, nullptr, &InitialIndexWork);
		Check(!InitialIndexWork, "initial document structure rebuilt the projected anchor index");
		std::array<size_t, 4> LabelQueries {};
		Core::ProjectionResult Projection = ProjectDiagnosticCase(Ruby, Src, true, nullptr, &LabelQueries);
		const std::vector<ValidationContext::DocFact> &CandidateDocs = Projection.Validation.ProtectedRegions.Docs;
		Check(!CandidateDocs.empty(), "YARD verification did not describe its records");
		Check(LabelQueries[3] == Count + 3, "document description reread its logical lines");
		for(size_t Idx = 0; Idx < 3; ++Idx) {
			Check(
				LabelQueries[Idx] && (!PreviousLabelQueries[Idx] || LabelQueries[Idx] <= 3 * PreviousLabelQueries[Idx]),
				"document label queries exceeded linear growth"
			);
		}
		Check(
			!PreviousLabelQueries[0] || LabelQueries[0] == PreviousLabelQueries[0],
			"same-target records repeated name text retrieval"
		);
		PreviousLabelQueries = LabelQueries;
		Check(
			CandidateDocs.front().Owner->Targets.size() == CandidateDocs.front().Target.size(),
			"same-target records copied their label repeatedly"
		);
		for(const ValidationContext::DocFact &Fact : CandidateDocs) {
			Check(
				Fact.Target.data() == CandidateDocs.front().Target.data(),
				"consecutive same-target records did not share their label range"
			);
		}
		size_t DocMatchWork = 0, DocTargetQueries = 0;
		Check(
			SyntaxValidatorCmp::HasSameDocs(ExpectedDocs, CandidateDocs, DocMatchWork, DocTargetQueries),
			"independent YARD document facts did not match"
		);
		Check(
			DocMatchWork == ExpectedDocs.size() + CandidateDocs.size() + Count + 3,
			"document matching expanded or reread logical lines"
		);
		Check(DocTargetQueries == 2, "consecutive document facts repeated target hash queries");
		Check(!PreviousDocMatchWork || DocMatchWork <= 3 * PreviousDocMatchWork, "document matching exceeded linear growth");
		PreviousDocMatchWork = DocMatchWork;
		Projection.Comments.SetStructure(CommentNormalizer::Inspect(ReadOnlyCommentView(Projection.Comments.GetRecords())));
		const CommentStructure &Previous = Projection.Comments.GetOutputStructure();
		const CommentStructure Shared = CommentNormalizer::Inspect(ReadOnlyCommentView(Projection.Comments.GetRecords()), &Previous);
		for(size_t Idx = 0; Idx < Projection.Comments.GetRecords().size(); ++Idx) {
			Check(
				Shared.GetCodeMask(Idx).data() == Previous.GetCodeMask(Idx).data(),
				"unchanged document structure did not share its read-only facts"
			);
		}
		std::vector<std::string> Lines(Count + 1, "More context");
		Lines.front() = "Summary";
		const std::vector<Core::DocItem> Items { { 0, Lines.size(), Core::DocItemKind::Summary, std::string_view() } };
		size_t SummaryWork = 0;
		Check(
			Core::DocCommentReader::HasSummary(Projection.Syntax.GetRules().Notation, Lines, Items, "f", &SummaryWork),
			"an existing YARD summary was lost"
		);
		Check(SummaryWork == 9, "summary existence concatenated the complete document");
		// 文末判定が参照する２種類の閉じは１つの表とし，閉じの無い本文では確保しない
		std::string Plain;
		for(size_t Idx = 0; Idx < Count; ++Idx) Plain += "More context ";
		size_t SentenceWork = 0, EndingBytes = 0;
		Check(CommentNormalizerAccess::Split(Plain, SentenceWork, &EndingBytes) == Plain, "unmarked prose changed");
		Check(!EndingBytes, "prose without endings allocated an ending table");
		const std::string Bracketed = "(" + Plain + ")";
		Check(
			CommentNormalizerAccess::Split(Bracketed, SentenceWork, &EndingBytes) == Bracketed && EndingBytes == Bracketed.size(),
			"enclosed prose did not use a single ending table"
		);
		std::fprintf(stdout, "YARD doc work n=%zu copied=%zu lines=%zu summary=%zu\n", Count, Docs[1], Docs[2], SummaryWork);
	}
	// 標準の出力マクロの実引数は，終端と補完の不確定性の両経路で共有する
	for(const char *Name : { "println" }) {
		size_t PreviousMacroWork = 0;
		for(const size_t Count : { 8U, 16U, 32U }) {
			std::string Src = std::string("fn f(){") + Name + "!(\"{:?}\", [1";
			for(size_t Idx = 1; Idx < Count; ++Idx) Src += ",1";
			Src += "]);}";
			Core::SyntaxModel Syntax = Project(Lang::Get(Lang::Rust), Src);
			Syntax.BuildRoleIdx();
			const Core::ControlFlow &Flow = Core::SyntaxNormalizerAccess::GetSharedFlow(Syntax);
			const std::vector<Core::NodeId> Functions = GetNodesWithRole(Syntax, Core::Role::OrdinaryFunctionDef);
			const std::vector<Core::NodeId> Macros = GetNodesWithRole(Syntax, Core::Role::MacroCall);
			Check(Functions.size() == 1 && Macros.size() == 1, "macro reuse fixture lost its function or invocation");
			const Core::NodeId Function = Functions.front(), Args = Syntax.GetLastNamedChild(Macros.front());
			const std::optional<Core::FunctionEnd> End = Flow.ClassifyFunction(Function);
			Check(End.has_value(), "macro reuse fixture lost its body");
			const size_t Before = Syntax.GetWorkAccount()->Used;
			Check(Flow.EndReach(Function, *End) == Core::EndReachability::Reachable, "ordinary output macro changed fallthrough");
			const size_t AfterReach = Syntax.GetWorkAccount()->Used;
			Check(!Core::SyntaxNormalizerAccess::HasUnknownMacroArguments(Flow, Args), "literal macro arguments became unknown");
			const size_t SummaryWork = Syntax.GetWorkAccount()->Used - AfterReach;
			Check(SummaryWork == 1, "completion rebuilt the macro argument summary");
			std::unordered_map<Core::NodeId, bool> Attributes;
			const bool IsSingle = Flow.IsSingleImplicitReturnBody(Function, *End);
			const bool CanMakeExplicit = Flow.CanMakeImplicitReturnExplicit(Function, *End, IsSingle);
			Check(
				!Flow.IsReturnCompletionUncertain(Function, *End, IsSingle, CanMakeExplicit, Attributes),
				"ordinary macro lost completion evidence"
			);
			const size_t Work = Syntax.GetWorkAccount()->Used - Before;
			if(PreviousMacroWork) Check(Work <= 3 * PreviousMacroWork, "shared macro arguments grow quadratically");
			PreviousMacroWork = Work;
			// トークンの変更は外側の要約だけを失効し，変更の無い配列の要約は保持する
			Core::NodeId Literal = Syntax.GetNamedChild(Args, 0);
			while(Syntax.GetChildCount(Literal)) Literal = Syntax.GetNamedChild(Literal, 0);
			Core::RewritePlan Plan;
			Core::RewriteStep Change { Core::RewriteKind::SetLexeme, Literal };
			Change.Text = Syntax.GetLexemeOf(Literal) == "{:?}" ? "value {:?}" : "\"value {:?}\"";
			Plan.Steps.push_back(std::move(Change));
			Syntax.Apply(Plan);
			const size_t AfterEdit = Syntax.GetWorkAccount()->Used;
			Check(!Core::SyntaxNormalizerAccess::HasUnknownMacroArguments(Flow, Args), "macro argument edit changed literal evidence");
			Check(Syntax.GetWorkAccount()->Used - AfterEdit > 1, "macro argument edit retained a stale summary");
			std::fprintf(stdout, "macro reuse %s n=%zu work=%zu summary=%zu\n", Name, Count, Work, SummaryWork);
		}
	}
	// 入子の呼出名の遮蔽を変えた場合は，可視範囲を共有する要約も失効する
	{
		Core::SyntaxModel Syntax = Project(
			Lang::Get(Lang::Rust),
			"macro_rules! panic { () => {} } fn f(){println!(\"{:?}\", panic!());} fn g(){println!(\"ok\");}"
		);
		const Core::ControlFlow &Flow = Core::SyntaxNormalizerAccess::GetSharedFlow(Syntax);
		const std::vector<Core::NodeId> Macros = GetNodesWithRole(Syntax, Core::Role::MacroCall);
		Check(Macros.size() == 2, "macro scope fixture lost its invocations");
		const Core::NodeId BoundArgs = Syntax.GetLastNamedChild(Macros.front()), PlainArgs = Syntax.GetLastNamedChild(Macros.back());
		Check(
			Core::SyntaxNormalizerAccess::HasUnknownMacroArguments(Flow, BoundArgs),
			"shadowed nested macro acquired a standard contract"
		);
		Check(!Core::SyntaxNormalizerAccess::HasUnknownMacroArguments(Flow, PlainArgs), "literal macro unexpectedly used bindings");
		const Core::NodeId Definition = GetNodesWithRole(Syntax, Core::Role::MacroRuleDef).front();
		for(const char *Name : { "other", "panic" }) {
			Core::RewritePlan RenamePlan;
			Core::RewriteStep Rename { Core::RewriteKind::SetLexeme, Syntax.GetFieldChild(Definition, Core::ChildRole::Name) };
			Rename.Text = Name;
			RenamePlan.Steps.push_back(std::move(Rename));
			Syntax.Apply(RenamePlan);
			Check(
				Core::SyntaxNormalizerAccess::HasUnknownMacroArguments(Flow, BoundArgs) == (std::string_view(Name) == "panic"),
				"nested macro retained a renamed binding"
			);
		}
		Core::RewritePlan Plan;
		Plan.Steps.push_back({ Core::RewriteKind::Remove, Definition });
		Syntax.Apply(Plan);
		Check(!Core::SyntaxNormalizerAccess::HasUnknownMacroArguments(Flow, BoundArgs), "nested macro retained a removed binding");
		const size_t BeforePlain = Syntax.GetWorkAccount()->Used;
		Check(!Core::SyntaxNormalizerAccess::HasUnknownMacroArguments(Flow, PlainArgs), "unrelated literal arguments changed");
		std::fprintf(stdout, "macro binding edit independent=%zu\n", Syntax.GetWorkAccount()->Used - BeforePlain);
		Check(Syntax.GetWorkAccount()->Used - BeforePlain == 1, "binding invalidation discarded unrelated literal arguments");
	}
	// 戻値の推論は同じ本体の２度目の照会で文の列を辿らず，返戻の除去で失効する
	size_t PreviousReturnShapeWork = 0;
	for(const size_t Count : { 8U, 16U, 32U }) {
		std::string Src = "def f():\n";
		for(size_t Idx = 0; Idx < Count; ++Idx) Src += " g()\n";
		Src += " return 1\ndef generator():\n yield 1\n";
		Core::SyntaxModel Syntax = Project(Lang::Get(Lang::Python), Src);
		Syntax.BuildRoleIdx();
		const Core::ControlFlow &Flow = Core::SyntaxNormalizerAccess::GetSharedFlow(Syntax);
		const Core::NodeId Function = GetNodesWithRole(Syntax, Core::Role::OrdinaryFunctionDef).front();
		const size_t Before = Syntax.GetWorkAccount()->Used;
		Check(Flow.ClassifyFunction(Function)->Kind == Core::ReturnKind::Val, "return inference lost its value");
		const size_t First = Syntax.GetWorkAccount()->Used - Before;
		Check(Flow.ClassifyFunction(Function)->Kind == Core::ReturnKind::Val, "shared return inference changed its value");
		const size_t Second = Syntax.GetWorkAccount()->Used - Before - First;
		std::fprintf(stdout, "return shape n=%zu first=%zu second=%zu\n", Count, First, Second);
		Check(Second < First, "return inference repeated its body scan");
		if(PreviousReturnShapeWork) Check(First <= 3 * PreviousReturnShapeWork, "return inference grows quadratically");
		PreviousReturnShapeWork = First;
		Core::RewritePlan Plan;
		Plan.Steps.push_back({ Core::RewriteKind::Remove, GetNodesWithRole(Syntax, Core::Role::FlowReturn).front() });
		Syntax.Apply(Plan);
		Check(Flow.ClassifyFunction(Function)->Kind == Core::ReturnKind::Void, "return inference retained a removed value return");
	}
	// goto のジャンプ先と範囲は本体毎に共有し，ラベルの除去で再構築する
	size_t PreviousGotoWork = 0;
	for(const size_t Count : { 8U, 16U, 32U }) {
		std::string Src = "void f(){goto Done;";
		for(size_t Idx = 0; Idx < Count; ++Idx) Src += "g();";
		Src += "Done:g();}";
		Core::SyntaxModel Syntax = Project(Lang::Get(Lang::Cpp), Src);
		Syntax.BuildRoleIdx();
		const Core::ControlFlow &Flow = Core::SyntaxNormalizerAccess::GetSharedFlow(Syntax);
		const Core::NodeId Function = GetNodesWithRole(Syntax, Core::Role::OrdinaryFunctionDef).front();
		const Core::NodeId Body = Flow.GetStmtsOf(Function);
		const size_t Before = Syntax.GetWorkAccount()->Used;
		Check(!Flow.CollectGotoTargets(Body).HasUnresolvedTarget, "goto reuse lost its label");
		const size_t First = Syntax.GetWorkAccount()->Used - Before;
		Check(!Flow.CollectGotoTargets(Body).HasUnresolvedTarget, "shared goto facts changed their target");
		Check(Syntax.GetWorkAccount()->Used - Before - First == 1, "goto target lookup rebuilt the body index");
		if(PreviousGotoWork) Check(First <= 3 * PreviousGotoWork, "goto target collection grows quadratically");
		PreviousGotoWork = First;
		Core::RewritePlan Plan;
		Plan.Steps.push_back({ Core::RewriteKind::Remove, GetNodesWithRole(Syntax, Core::Role::Labeled).front() });
		Syntax.Apply(Plan);
		Check(Flow.CollectGotoTargets(Body).HasUnresolvedTarget, "goto facts retained a removed target");
	}
	// 構文順の再利用と移動・除去後の修復を同じ候補列で照合する
	size_t PreviousOrderWork = 0;
	for(const size_t Count : { 16U, 32U, 64U }) {
		std::string Src;
		for(size_t Idx = 0; Idx < Count; ++Idx) Src += "void f" + std::to_string(Idx) + "(){int n=0;n++;}";
		Core::SyntaxModel Syntax = Project(Lang::Get(Lang::Cpp), Src);
		const std::span<const Core::NodeId> Children = Syntax.GetChildrenOf(Syntax.GetRoot());
		std::vector<Core::NodeId> Expected(Children.begin(), Children.end());
		Check(Expected.size() == Count, "order fixture lost a function");
		std::vector<Core::NodeId> Found(Expected.rbegin(), Expected.rend());
		Found.push_back(Expected.front());
		Found.push_back(Core::NoNode);
		const Core::SyntaxNormalizer OrderNormalizer;
		const size_t Before = Syntax.GetWorkAccount()->Used;
		Check(
			Core::SyntaxNormalizerAccess::SortCandidates(Syntax, Found, OrderNormalizer) == Expected,
			"unprimed candidate sorting changed source order"
		);
		Check(!Syntax.GetNode(Syntax.GetRoot()).OrderFirst, "candidate sorting numbered unrelated subtrees");
		Check(Syntax.IsBefore(Expected.front(), Expected.back()), "numbered order lost the first function");
		const size_t NumberedWork = Syntax.GetWorkAccount()->Used;
		Check(Syntax.IsBefore(Expected.front(), Expected.back()), "repeated numbered order changed");
		Check(Syntax.GetWorkAccount()->Used - NumberedWork == 2, "unchanged order was numbered again");
		Check(
			Core::SyntaxNormalizerAccess::SortCandidates(Syntax, Found, OrderNormalizer) == Expected,
			"candidate sorting changed source order"
		);
		const std::array<size_t, 4> OrderStorage = Core::SyntaxNormalizerAccess::GetOrderStorage(OrderNormalizer);
		Check(
			Core::SyntaxNormalizerAccess::SortCandidates(Syntax, Found, OrderNormalizer) == Expected,
			"reused candidate order changed"
		);
		Check(
			Core::SyntaxNormalizerAccess::GetOrderStorage(OrderNormalizer) == OrderStorage,
			"repeated candidate sorting grew its scratch buffers"
		);
		Core::RewritePlan MovePlan;
		Core::RewriteStep Move { Core::RewriteKind::Move, Expected.back() };
		Move.Dst = Syntax.GetRoot();
		MovePlan.Steps.push_back(std::move(Move));
		Syntax.Apply(MovePlan);
		std::rotate(Expected.begin(), Expected.end() - 1, Expected.end());
		Check(Syntax.IsBefore(Expected.front(), Expected.back()), "numbered order retained a moved position");
		Check(
			Core::SyntaxNormalizerAccess::SortCandidates(Syntax, Found, OrderNormalizer) == Expected,
			"candidate order retained a moved position"
		);
		Core::RewritePlan RemovePlan;
		RemovePlan.Steps.push_back({ Core::RewriteKind::Remove, Expected.front() });
		Syntax.Apply(RemovePlan);
		Expected.erase(Expected.begin());
		Check(Syntax.IsBefore(Expected.front(), Expected.back()), "numbered order retained a removed position");
		Check(
			Core::SyntaxNormalizerAccess::SortCandidates(Syntax, Found, OrderNormalizer) == Expected,
			"detached candidates retained their old order"
		);
		Syntax.CheckNesting();
		const Core::SyntaxNormalizerAccess::RuleFixture &Increment = Core::SyntaxNormalizerAccess::GetFixtureRule("Increment");
		Check(Core::SyntaxNormalizerAccess::RunIndividualRule(Syntax, Increment) == 1, "scalar bindings lost prefix normalization");
		const Core::ControlFlow Flow(Syntax);
		for(const Core::NodeId Function : Expected) {
			const std::optional<Core::FunctionEnd> End = Flow.ClassifyFunction(Function);
			Check(End.has_value(), "reused termination storage lost a function");
			Check(Flow.EndReach(Function, *End) == Core::EndReachability::Reachable, "termination storage retained another body");
		}
		const size_t Work = Syntax.GetWorkAccount()->Used - Before;
		if(PreviousOrderWork) Check(Work <= 3 * PreviousOrderWork, "normalization order grows quadratically");
		PreviousOrderWork = Work;
	}
	// 同じ本体の静的解析と末尾照会で，既知の終端を再走査しない（整形仕様 9.2）
	struct FlowScaleCase {
		::Lang::ID Lang;
		const char *Prefix;
		const char *Part;
		const char *Suffix;
		Core::EndReachability Expected = Core::EndReachability::Reachable;
	};
	const FlowScaleCase FlowInputs[] {
		{ Lang::Cpp, "void f(int x){", "g(h(x));", "}" },
		{ Lang::Cpp, "void f(int x){", "g(h(x));", "return;}", Core::EndReachability::Unreachable },
		{ Lang::Go, "package p;func f(x bool) bool{", "for {if x {break}};", "}" },
		{ Lang::Python, "def f(self):\n", " self.assertEqual(1, 1)\n", "" }
	};
	for(const FlowScaleCase &Input : FlowInputs) {
		size_t Previous = 0, PreviousInvalidationWork = 0, PreviousExitRecords = 0;
		for(const size_t Count : { 16U, 32U, 64U }) {
			std::string Src = Input.Prefix;
			for(size_t Idx = 0; Idx < Count; ++Idx) Src += Input.Part;
			Src += Input.Suffix;
			Core::SyntaxModel Syntax = Project(Lang::Get(Input.Lang), Src);
			Syntax.BuildRoleIdx();
			const std::vector<Core::NodeId> Functions = GetNodesWithRole(Syntax, Core::Role::OrdinaryFunctionDef);
			Check(Functions.size() == 1, "flow scale input lost its function");
			const Core::ControlFlow &Flow = Core::SyntaxNormalizerAccess::GetSharedFlow(Syntax);
			const Core::NodeId Function = Functions.front();
			const std::optional<Core::FunctionEnd> End = Flow.ClassifyFunction(Function);
			Check(End.has_value(), "flow scale input lost its body");
			// 値を作れない返戻補完では，既存の倍々の本体を評価せず計画を空に保つ
			if(Input.Lang == Lang::Go) {
				Check(End->Kind == Core::ReturnKind::Val, "Go value return case lost its declared result");
				const Core::RewritePlan GoValueReturnPlan = Core::SyntaxNormalizerAccess::CollectExplicitReturns(Syntax, Function);
				Check(GoValueReturnPlan.Steps.empty(), "Go value return completion invented a result");
				Check(!Core::SyntaxNormalizerAccess::GetLoopExitStorage(Flow)[0], "value return completion built an unused loop exit index");
			}
			const Core::GotoIdx Gotos = Flow.CollectGotoTargets(End->Stmts);
			if(Input.Lang == Lang::Python) {
				const std::pair<size_t, size_t> Storage = Core::SyntaxNormalizerAccess::GetDetailedFlowStorage(Flow);
				Check(!Storage.first && !Storage.second, "member body classification built detailed flow tables");
			}
			const size_t Before = Syntax.GetWorkAccount()->Used;
			Check(Flow.GetReturningStmtsAfterTermination(End->Stmts, Gotos).empty(), "ordinary calls became unreachable");
			Check(Flow.EndReach(Function, *End) == Input.Expected, "shared evaluation changed fallthrough");
			const size_t First = Syntax.GetWorkAccount()->Used - Before;
			const std::pair<size_t, size_t> FirstFlowStorage = Core::SyntaxNormalizerAccess::GetDetailedFlowStorage(Flow);
			if(Input.Lang == Lang::Cpp) Check(FirstFlowStorage.first, "first tail lookup discarded the body summary");
			std::array<const void *, 4> ExitIdentity;
			const std::array<size_t, 6> ExitStorage = Core::SyntaxNormalizerAccess::GetLoopExitStorage(Flow, &ExitIdentity);
			Check(Flow.EndReach(Function, *End) == Input.Expected, "repeated tail lookup changed fallthrough");
			const size_t Second = Syntax.GetWorkAccount()->Used - Before - First;
			Check(
				Core::SyntaxNormalizerAccess::GetDetailedFlowStorage(Flow) == FirstFlowStorage,
				"repeated tail lookup changed the retained body storage"
			);
			std::array<const void *, 4> TailExitIdentity;
			Check(
				Core::SyntaxNormalizerAccess::GetLoopExitStorage(Flow, &TailExitIdentity) == ExitStorage && TailExitIdentity == ExitIdentity,
				"tail lookup rebuilt exit records or scratch"
			);
			if(Input.Lang == Lang::Go) {
				Check(ExitStorage[0] == 1 && ExitStorage[1], "Go loops did not share one exit index");
				Check(ExitStorage[1] <= Syntax.GetNodeCount(), "Go exit index duplicated node records");
				Check(ExitStorage[3] == ExitStorage[1], "Go exit index lost plain subtree minima");
				Check(ExitStorage[4] == Count, "Go exit index lost or duplicated break events");
				Check(!ExitStorage[2] && !ExitStorage[5], "Go plain exits rebuilt subtree minima or scratch");
				Check(!PreviousExitRecords || ExitStorage[1] <= 3 * PreviousExitRecords, "loop exit records grew quadratically");
				PreviousExitRecords = ExitStorage[1];
			}
			if(Input.Lang == Lang::Python) {
				const std::pair<size_t, size_t> Storage = Core::SyntaxNormalizerAccess::GetDetailedFlowStorage(Flow);
				Check(!Storage.first && !Storage.second, "ordinary members built detailed termination or binding tables");
			}
			Check(Second < First, "tail reachability repeated the body evaluation");
			std::fprintf(stdout, "flow reuse n=%zu first=%zu second=%zu\n", Count, First, Second);
			const size_t Work = First + Second;
			if(Previous) Check(Work <= 3 * Previous, "evaluation positions or callable lookups grow quadratically");
			Previous = Work;
			if(Input.Lang == Lang::Cpp && Input.Expected == Core::EndReachability::Reachable) {
				Check(Flow.GetReturningStmtsAfterTermination(End->Stmts, Gotos).empty(), "body reevaluation changed unreachable returns");
				const size_t BeforeInvalidation = Syntax.GetWorkAccount()->Used;
				Core::SyntaxNormalizerAccess::InvalidateFlowStructure(Flow, End->Stmts);
				const size_t InvalidationWork = Syntax.GetWorkAccount()->Used - BeforeInvalidation;
				Check(
					!Core::SyntaxNormalizerAccess::GetDetailedFlowStorage(Flow).first,
					"structure invalidation retained the active body summary"
				);
				if(PreviousInvalidationWork) Check(InvalidationWork <= 3 * PreviousInvalidationWork, "flow invalidation grows quadratically");
				PreviousInvalidationWork = InvalidationWork;
				Check(Flow.EndReach(Function, *End) == Input.Expected, "structure invalidation changed fallthrough");
				std::fprintf(stdout, "flow invalidation n=%zu work=%zu\n", Count, InvalidationWork);
			}
			if(Input.Expected == Core::EndReachability::Unreachable) {
				Core::RewritePlan RemoveTail;
				RemoveTail.Steps.push_back({ Core::RewriteKind::Remove, End->Last });
				Syntax.Apply(RemoveTail);
				const std::optional<Core::FunctionEnd> ChangedEnd = Flow.ClassifyFunction(Function);
				Check(ChangedEnd.has_value(), "tail removal lost the function body");
				Check(
					Flow.EndReach(Function, *ChangedEnd) == Core::EndReachability::Reachable,
					"tail removal retained unreachable fallthrough"
				);
			}
		}
	}
	// 導入が無い呼出も探索を終え，再照会では可視範囲を辿り直さない
	struct UnboundCallCase {
		::Lang::ID Lang;
		const char *Prefix;
		const char *Part;
		const char *Suffix;
	};
	const UnboundCallCase UnboundCalls[] { { Lang::Cpp, "void f(){", "do{g();}while(false);", "}" } };
	for(const UnboundCallCase &Input : UnboundCalls) {
		size_t Previous = 0;
		for(const size_t Count : { 16U, 32U, 64U }) {
			std::string Src = Input.Prefix;
			for(size_t Idx = 0; Idx < Count; ++Idx) Src += Input.Part;
			Src += Input.Suffix;
			Core::SyntaxModel Syntax = Project(Lang::Get(Input.Lang), Src.c_str());
			Syntax.BuildRoleIdx();
			const std::vector<Core::NodeId> Functions = GetNodesWithRole(Syntax, Core::Role::OrdinaryFunctionDef);
			Check(Functions.size() == 1, "unbound call input lost its function", Src.c_str());
			const Core::ControlFlow &Flow = Core::SyntaxNormalizerAccess::GetSharedFlow(Syntax);
			const Core::NodeId Function = Functions.front();
			const std::optional<Core::FunctionEnd> End = Flow.ClassifyFunction(Function);
			Check(End.has_value(), "unbound call input lost its body", Src.c_str());
			Core::NodeId Call = Core::NoNode;
			for(Core::NodeId Node = 0; Node < Syntax.GetNodeCount(); ++Node) if(Syntax.GetNode(Node).Lexeme == "g") {
				Call = Node;
				break;
			}
			Check(Call != Core::NoNode, "unbound call input lost its name", Src.c_str());
			const size_t Before = Syntax.GetWorkAccount()->Used;
			Check(!Syntax.GetBindingFact(Call, std::string_view()).Source, "unbound call acquired a source", Src.c_str());
			Check(!Syntax.GetBindingFact(Call, std::string_view(), true).Source, "unbound global call acquired a source", Src.c_str());
			Check(Flow.EndReach(Function, *End) == Core::EndReachability::Reachable, "unbound call lost fallthrough", Src.c_str());
			const size_t First = Syntax.GetWorkAccount()->Used - Before;
			Check(Flow.EndReach(Function, *End) == Core::EndReachability::Reachable, "reused unbound call lost fallthrough", Src.c_str());
			const size_t Second = Syntax.GetWorkAccount()->Used - Before - First;
			Check(First && Second < First, "unbound call repeated scope exploration", Src.c_str());
			if(Previous) Check(First + Second <= 3 * Previous, "unbound call work grows quadratically", Src.c_str());
			Previous = First + Second;
			std::fprintf(
				stdout,
				"unbound call lang=%u n=%zu first=%zu second=%zu\n",
				static_cast<unsigned>(Input.Lang),
				Count,
				First,
				Second
			);
		}
	}
	// 別関数への補完で候補集合を全体から作り直さず，変更部分木だけを確認する
	size_t PreviousFilterWork = 0;
	for(const size_t Count : { 4U, 8U, 16U }) {
		std::string Src;
		for(size_t Idx = 0; Idx < Count; ++Idx) Src += "def f" + std::to_string(Idx) + "(self):\n self.assertEqual(1, 1)\n";
		Core::SyntaxModel Syntax = Project(Lang::Get(Lang::Python), Src);
		const Core::SyntaxNormalizerAccess::RuleFixture &ReturnRule = Core::SyntaxNormalizerAccess::GetFixtureRule("ExplicitReturn");
		const size_t Before = Syntax.GetWorkAccount()->Used;
		Check(Core::SyntaxNormalizerAccess::RunIndividualRule(Syntax, ReturnRule) == 1, "ordinary member bodies lost tail returns");
		const size_t Work = Syntax.GetWorkAccount()->Used - Before;
		Check(GetNodesWithRole(Syntax, Core::Role::FlowReturn).size() == Count, "ordinary member functions lost explicit returns");
		if(PreviousFilterWork) Check(Work <= 3 * PreviousFilterWork, "member filtering rebuilt all function scopes after each return");
		PreviousFilterWork = Work;
		std::fprintf(stdout, "member returns n=%zu work=%zu\n", Count, Work);
	}
	// 保持済の繰返の後で別関数へ返戻を挿入しても，無関係な所有者を照合し直さない
	const Core::SyntaxNormalizerAccess::RuleFixture &Returns = Core::SyntaxNormalizerAccess::GetFixtureRule("ExplicitReturn");
	size_t PrevReturnWork = 0;
	for(const size_t Count : { 32U, 64U, 128U }) {
		std::string Src = "#define FULL 1\n#define EMPTY\n";
		for(size_t Idx = 0; Idx < Count; ++Idx) Src += "void a" + std::to_string(Idx) + "(){while(true) tick();}";
		for(size_t Idx = 0; Idx < Count; ++Idx) Src += "void b" + std::to_string(Idx) + "(){tick();}";
		Core::SyntaxModel Syntax = Project(Lang::Get(Lang::Cpp), Src);
		// 同じ倍増入力で，共有子列から空・有値マクロを区別する経路も通す
		const Core::NodeId Full = Syntax.GetNamedChild(Syntax.GetRoot(), 0), Empty = Syntax.GetNamedChild(Syntax.GetRoot(), 1);
		Check(
			Syntax.GetNode(Full).Facts.Has(Core::ConstraintKind::ValuedMacro) && Syntax.Is(Empty, Core::Role::MacroDef) &&
			!Syntax.GetNode(Empty).Facts.Has(Core::ConstraintKind::ValuedMacro),
			"shared child roles changed the macro body distinction"
		);
		const size_t Before = Syntax.GetWorkAccount()->Used;
		Check(Core::SyntaxNormalizerAccess::RunIndividualRule(Syntax, Returns) == 1, "reachable functions lost explicit returns");
		const size_t Work = Syntax.GetWorkAccount()->Used - Before;
		Check(GetNodesWithRole(Syntax, Core::Role::FlowReturn).size() == Count, "loop functions acquired unreachable returns");
		if(PrevReturnWork) Check(Work <= 3 * PrevReturnWork, "unrelated loop invalidation grows quadratically");
		PrevReturnWork = Work;
	}
	// リテラル内の `) // ` は修復候補でなく，本文の長さで行末の再探索を増やさない（整形仕様 9.2）
	const GrammarProfile &CSharp = GrammarProfile::Resolve(Lang::Get(Lang::CSharp));
	ParsedTreeAccess::CheckInputWorkBudget(CSharp, "class C{int F(int d,int a){return (d)-a;}}");
	size_t PreviousCastWork = 0;
	for(const size_t Count : { 256U, 512U, 1024U }) {
		std::string Src = "class C{string S=\"";
		for(size_t Idx = 0; Idx < Count; ++Idx) Src += ") // ";
		Src += "\";int F(int d,int a){return (d)-a;}}";
		InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(CSharp, Src);
		ParseInput Input = ParseAdapter::Adapt(CSharp, Normalized);
		const ParsedTree Parsed = SyntaxParser::Parse(CSharp, Input);
		Check(Parsed.GetParseDecision() == ParseDecision::Continue, "a literal was read as a cast candidate");
		Check(Parsed.find("return d-a;") != std::string::npos, "a bound value cast was not repaired");
		Check(
			Parsed.find(std::string_view(Src).substr(Src.find('"'), 2 + 5 * Count)) != std::string::npos,
			"cast repair changed literal bytes"
		);
		const size_t Work = Parsed.GetCastRepairWork();
		Check(Work && (!PreviousCastWork || Work == PreviousCastWork), "literal text increased cast candidate visits");
		const Core::ProjectionResult Projection = Core::SourceProjector::Project(Parsed, *Input.Profile, Input);
		Check(Parsed.GetNameWork(), "cast repair did not exercise binding lookup");
		Check(
			Projection.Syntax.GetWorkAccount()->Used == Parsed.GetNameWork() + Parsed.GetCastRepairWork() &&
			Projection.Syntax.GetWorkAccount()->Used <= Projection.Syntax.GetWorkLimit(),
			"input binding and cast work did not enter the product budget exactly once"
		);
		PreviousCastWork = Work;
	}
	const Core::SyntaxNormalizerAccess::RuleFixture &Parens = Core::SyntaxNormalizerAccess::GetFixtureRule("Parentheses");
	size_t PrevParenWork = 0;
	// 型引数と紛れる比較の引数列の規模倍増（閉じの有無を同じ部分木の事実から読む）
	struct AngleCase {
		const char *Prefix;
		const char *Suffix;
		::Lang::ID Lang;
	};
	const AngleCase AngleInputs[] { { "void f(){g(", "(c > d));}", Lang::Cpp } };
	for(const AngleCase &Input : AngleInputs) {
		size_t PrevAngleWork = 0;
		for(const size_t Count : { 32U, 64U, 128U }) {
			std::string Src = Input.Prefix;
			for(size_t Idx = 0; Idx < Count; ++Idx) Src += "(a < b),";
			Src += Input.Suffix;
			Core::SyntaxModel Syntax = Project(Lang::Get(Input.Lang), Src);
			const size_t Before = Syntax.GetWorkAccount()->Used;
			Check(
				Core::SyntaxNormalizerAccess::RunIndividualRule(Syntax, Parens) == 1,
				"a later angle close lost necessary comparison parentheses"
			);
			const size_t Work = Syntax.GetWorkAccount()->Used - Before;
			Check(
				GetNodesWithRole(Syntax, Core::Role::GroupingParen).size() == Count,
				"comparison parentheses differ from the expected count"
			);
			if(PrevAngleWork) Check(Work <= 3 * PrevAngleWork, "angle-close queries grow quadratically");
			PrevAngleWork = Work;
		}
	}
	// 重なる括弧の規模倍増でも，非マクロの仮引数を包む全てのラッパーを除く
	size_t PreviousNestedProjectionReading = 0;
	for(const size_t Count : { 32U, 64U, 128U }) {
		std::string Src = "int f(int x){return ";
		Src.append(Count, '(');
		Src += "x";
		Src.append(Count, ')');
		Src += ";}";
		std::array<size_t, 4> NestedProjectionQueries {};
		Core::SyntaxModel Syntax = Project(Lang::Get(Lang::Cpp), Src, nullptr, nullptr, &NestedProjectionQueries);
		Check(
			NestedProjectionQueries[2] && NestedProjectionQueries[2] < Syntax.GetNodeCount(),
			"nested parentheses lost their candidate reading checks or checked every node"
		);
		Check(
			!PreviousNestedProjectionReading || NestedProjectionQueries[2] <= 3 * PreviousNestedProjectionReading,
			"nested parenthesis reading checks exceeded linear growth"
		);
		PreviousNestedProjectionReading = NestedProjectionQueries[2];
		Check(Core::SyntaxNormalizerAccess::RunIndividualRule(Syntax, Parens) == 1, "a parenthesis chain lost its removable shape");
		Check(GetNodesWithRole(Syntax, Core::Role::GroupingParen).empty(), "a nonmacro parameter retained a parenthesis wrapper");
	}
	for(const size_t Count : { 128U, 256U }) {
		std::string Src = "int f(int a){return a";
		for(size_t Idx = 0; Idx < Count; ++Idx) Src += "-(a";
		Src += "-a";
		Src.append(Count, ')');
		Src += ";}";
		Core::SyntaxModel Syntax = Project(Lang::Get(Lang::Cpp), Src);
		const size_t Before = Syntax.GetWorkAccount()->Used;
		Check(
			!Core::SyntaxNormalizerAccess::RunIndividualRule(Syntax, Parens),
			"right nested subtraction lost a necessary parenthesis"
		);
		const size_t ParenWork = Syntax.GetWorkAccount()->Used - Before;
		Check(
			GetOracleLexemes(Syntax) == GetOracleLexemes(Project(Lang::Get(Lang::Cpp), Src)),
			"necessary parentheses changed their hand structure"
		);
		if(PrevParenWork) Check(ParenWork <= 3 * PrevParenWork, "kept parentheses grow quadratically");
		PrevParenWork = ParenWork;
	}
	// 単一実引数の呼出の祖先を共有し，返戻・初期化子・式の文の括弧の採否を保つ
	struct DeclChainCase {
		::Lang::ID Lang;
		const char *Decl;
		const char *Callee;
	};
	const DeclChainCase DeclChainInputs[] { { Lang::Cpp, "int f(int x);", "f" } };
	for(const DeclChainCase &Input : DeclChainInputs) for(const size_t Context : { 0U, 2U }) { // 返戻・式の文の順
		size_t PreviousDeclChainWork = 0;
		for(const size_t Count : { 64U, 128U, 256U, 512U }) {
			std::string Src = std::string(Input.Decl) + "int g(int x){";
			if(!Context) Src += "return ";
			std::string Expected = Src;
			for(size_t Idx = 0; Idx < Count; ++Idx) {
				Src += std::string(Input.Callee) + "((";
				Expected += std::string(Input.Callee) + "(";
				// 式の文の最初の実引数だけ，宣言と読ませないラッパーを保つ
				if(Context == 2 && !Idx) Expected += "(";
			}
			Src += "x";
			Src.append(Count << 1, ')');
			Expected += "x";
			Expected.append(Count + static_cast<size_t>(Context == 2), ')');
			const char *const Suffix = !Context ? ";}" : ";return x;}";
			Src += Suffix;
			Expected += Suffix;
			Core::SyntaxModel Syntax = Project(Lang::Get(Input.Lang), Src);
			const size_t Before = Syntax.GetWorkAccount()->Used;
			Check(
				Core::SyntaxNormalizerAccess::RunIndividualRule(Syntax, Parens) == 1,
				"a declaration-call chain lost removable parentheses"
			);
			const size_t Work = Syntax.GetWorkAccount()->Used - Before;
			Check(
				GetOracleLexemes(Syntax) == GetOracleLexemes(Project(Lang::Get(Input.Lang), Expected)),
				"a declaration-call chain differs from its hand-specified parentheses"
			);
			Check(
				GetNodesWithRole(Syntax, Core::Role::GroupingParen).size() == static_cast<size_t>(Context == 2),
				"a declaration-call chain changed the required wrapper count"
			);
			if(PreviousDeclChainWork) Check(Work <= 3 * PreviousDeclChainWork, "declaration-call ancestors grow quadratically");
			PreviousDeclChainWork = Work;
		}
	}
	const Core::SyntaxNormalizerAccess::RuleFixture &Lexical = Core::SyntaxNormalizerAccess::GetFixtureRule("Declarations");
	size_t PrevDeclWork = 0;
	for(const size_t Count : { 100U, 200U, 400U }) {
		std::string Src = "function f(){", Expected = "function f(){const ";
		for(size_t Idx = 0; Idx < Count; ++Idx) {
			const std::string Item = "x" + std::to_string(Idx) + "=" + std::to_string(Idx);
			Src += "let " + Item + ";";
			if(Idx) Expected += ",";
			Expected += Item;
		}
		Src += "}";
		Expected += ";}";
		Core::SyntaxModel Syntax = Project(Lang::Get(Lang::JavaScript), Src);
		const size_t Before = Syntax.GetWorkAccount()->Used;
		Check(
			Core::SyntaxNormalizerAccess::RunIndividualRule(Syntax, Lexical) == 1,
			"lexical declarations did not reach their final form in one plan"
		);
		const size_t Work = Syntax.GetWorkAccount()->Used - Before;
		Check(
			GetOracleLexemes(Syntax) == GetOracleLexemes(Project(Lang::Get(Lang::JavaScript), Expected)),
			"lexical declarations differ from the specified single const group"
		);
		if(PrevDeclWork) Check(Work <= 3 * PrevDeclWork, "lexical declaration work grows quadratically");
		PrevDeclWork = Work;
		if(Count == 100) {
			const Core::LineBreaker Breaking(128); // 規約の最大行幅
			const Formatter Engine(Breaking);
			const FormatResult Result = Engine.Format(Src, Lang::Get(Lang::JavaScript), false);
			Check(
				Result.Reason == FailureReason::None &&
				(Result.Classification == FormatClassification::Changed || Result.Classification == FormatClassification::Unchanged),
				"the hundred-declaration public input exceeded its unchanged budget"
			);
		}
	}
	// ラベルの共有，短い本体の共有，統合相手の無い分岐の読取の規模倍増
	struct LintScaleCase {
		::Lang::ID Lang;
		const char *Prefix;
		const char *Part;
		const char *Suffix;
		LintRule Rule;
	};
	const LintScaleCase LintInputs[] {
		{ Lang::Cpp, "void F(int X){", "if(X) return;", "}", LintRule::MissingReturnComment },
		{ Lang::Rust, "fn f(x:f32){", "consume(x>=1f32);consume(x<=1_0f64);", "}", LintRule::ComparisonBoundary },
		{ Lang::Cpp, "void F(){const char *S=R\"(", "\n\t\t\t\ttext", "\n)\";}", LintRule::NestingDepth }
	};
	for(const LintScaleCase &Input : LintInputs) {
		size_t Previous = 0;
		for(const size_t Count : { 100U, 200U, 400U }) {
			std::string Src = Input.Prefix;
			for(size_t Idx = 0; Idx < Count; ++Idx) Src += Input.Part;
			Src += Input.Suffix;
			size_t Work = 0;
			const std::vector<LintWarning> Warnings = AnalyzeDiagnosticCase(Lang::Get(Input.Lang), Src, std::string_view(), &Work);
			Check(Work, "static analysis did not record traversal work");
			Check(
				std::ranges::none_of(
					Warnings,
					[&](const LintWarning &Warning) -> bool {
						return Warning.Rule == Input.Rule;
					}
				),
				"a scaled syntactic candidate changed its hand expectation"
			);
			if(Previous) Check(Work <= 3 * Previous, "static-analysis facts grow quadratically");
			Previous = Work;
		}
	}
	// 前処理の条件内の名前だけを除外し，長い条件の各子を１度の事実の走査で読む（整形仕様 8.4, 9.2）
	for(const Lang::ID Language : { Lang::Cpp }) {
		size_t Previous = 0;
		for(const size_t Count : { 32U, 64U, 128U }) {
			std::string Src = "#define OUTSIDE_CONDITION 1\n";
			for(size_t Idx = 0; Idx < Count; ++Idx) Src += "#define LONG_CONDITION_NAME_" + std::to_string(Idx) + " 1\n";
			Src += "#if ";
			for(size_t Idx = 0; Idx < Count; ++Idx) {
				if(Idx) Src += " && ";
				Src += "defined(LONG_CONDITION_NAME_" + std::to_string(Idx) + ")";
			}
			Src += "\nvoid F(){Consume(OUTSIDE_CONDITION); }\n#endif\n";
			size_t Work = 0, Constants = 0;
			const std::vector<LintWarning> Warnings = AnalyzeDiagnosticCase(Lang::Get(Language), Src, std::string_view(), &Work);
			for(const LintWarning &Warning : Warnings) {
				if(Warning.Rule != LintRule::ConstantMacro) continue;
				++Constants;
				Check(!Warning.Row && Warning.Column == 8, "a condition changed the constant warning's position");
				Check(
					Warning.Msg == "Review: Replace this constant macro with a language constant",
					"a preprocessor condition changed the constant warning's text"
				);
			}
			Check(Constants == 1, "a condition name was missed or an ordinary use suppressed a constant warning");
			Check(Work, "preprocessor condition facts did not record traversal work");
			if(Previous) Check(Work <= 3 * Previous, "preprocessor condition facts grow quadratically");
			Previous = Work;
		}
	}
	for(const Lang::ID Language : { Lang::Cpp }) {
		size_t Previous = 0;
		for(const size_t Count : { 100U, 200U, 400U }) {
			std::string Src = "void F(int X){";
			for(size_t Idx = 0; Idx < Count; ++Idx) Src += "if(X)goto L" + std::to_string(Idx) + ";";
			for(size_t Idx = 0; Idx < Count; ++Idx) Src += "L" + std::to_string(Idx) + ":G();";
			Src += "}";
			size_t Work = 0;
			const std::vector<LintWarning> Warnings = AnalyzeDiagnosticCase(Lang::Get(Language), Src, std::string_view(), &Work);
			Check(Work, "static analysis did not record traversal work");
			const size_t Jumps = std::ranges::count_if(
				Warnings,
				[](const LintWarning &Warning) -> bool {
					return Warning.Rule == LintRule::JumpUsage;
				}
			);
			Check(Jumps == Count, "a function label index changed the jump-warning count");
			if(Previous) Check(Work <= 3 * Previous, "function label queries grow quadratically");
			Previous = Work;
		}
	}
	// 分岐毎に別の代替節を持つ為，統合候補は無い（整形仕様 8.4）
	size_t PreviousIfWork = 0;
	for(const size_t Count : { 32U, 64U, 128U }) {
		std::string Src = "function f(x:boolean):void{";
		for(size_t Idx = 0; Idx < Count; ++Idx) Src += "if(x){g();";
		Src += "h();";
		for(size_t Idx = 0; Idx < Count; ++Idx) Src += "}else{k();}";
		Src += "}";
		size_t Work = 0;
		const std::vector<LintWarning> Warnings = AnalyzeDiagnosticCase(Lang::Get(Lang::TypeScript), Src, std::string_view(), &Work);
		Check(Work, "static analysis did not record traversal work");
		Check(
			std::ranges::none_of(
				Warnings,
				[](const LintWarning &Warning) -> bool {
					return Warning.Rule == LintRule::MergeIf;
				}
			),
			"a branch without a merge partner acquired a merge warning"
		);
		if(PreviousIfWork) Check(Work <= 3 * PreviousIfWork, "unused branch text grows quadratically");
		PreviousIfWork = Work;
	}
	size_t PreviousLabelWork = 0;
	for(const size_t Count : { 64U, 128U, 256U }) {
		std::string Src = "function f():void{";
		for(size_t Idx = 0; Idx < Count; ++Idx) Src += "L" + std::to_string(Idx) + ":";
		Src += "G();}";
		size_t Work = 0;
		const std::vector<LintWarning> Warnings = AnalyzeDiagnosticCase(Lang::Get(Lang::TypeScript), Src, std::string_view(), &Work);
		Check(Work, "static analysis did not record traversal work");
		Check(
			std::ranges::none_of(
				Warnings,
				[](const LintWarning &Warning) -> bool {
					return Warning.Rule == LintRule::JumpUsage;
				}
			),
			"labels without a jump acquired a jump warning"
		);
		if(PreviousLabelWork) Check(Work <= 3 * PreviousLabelWork, "nested label queries grow quadratically");
		PreviousLabelWork = Work;
	}
	size_t PreviousNumericWork = 0;
	for(const size_t Count : { 32U, 64U, 128U }) {
		std::string Src = "<?php function F($x){";
		for(size_t Idx = 0; Idx < Count; ++Idx) Src += "while($x){";
		for(size_t Idx = 0; Idx < Count; ++Idx) Src += "if($x)break " + std::to_string(Count) + ";";
		Src.append(Count + 1, '}');
		size_t Work = 0;
		const std::vector<LintWarning> Warnings = AnalyzeDiagnosticCase(Lang::Get(Lang::PHP), Src, std::string_view(), &Work);
		Check(Work, "static analysis did not record traversal work");
		Check(
			std::ranges::none_of(
				Warnings,
				[](const LintWarning &Warning) -> bool {
					return Warning.Rule == LintRule::JumpUsage;
				}
			),
			"a numeric jump lost the multiple-loop exception"
		);
		if(PreviousNumericWork) Check(Work <= 3 * PreviousNumericWork, "numeric jump queries grow quadratically");
		PreviousNumericWork = Work;
	}
	// 入子の型は各本体の唯一のメンバなので，役割の順位の後退は無い
	size_t PreviousMemberWork = 0;
	for(const size_t Count : { 32U, 64U, 128U }) {
		std::string Src;
		for(size_t Idx = 0; Idx < Count; ++Idx) Src += "class C" + std::to_string(Idx) + "{";
		Src += "int Value;";
		Src.append(Count, '}');
		size_t Work = 0;
		const std::vector<LintWarning> Warnings = AnalyzeDiagnosticCase(Lang::Get(Lang::Java), Src, std::string_view(), &Work);
		Check(Work, "static analysis did not record traversal work");
		Check(
			std::ranges::none_of(
				Warnings,
				[](const LintWarning &Warning) -> bool {
					return Warning.Rule == LintRule::MemberOrder;
				}
			),
			"a single member acquired a role-order warning"
		);
		if(PreviousMemberWork) Check(Work <= 3 * PreviousMemberWork, "nested member headers grow quadratically");
		PreviousMemberWork = Work;
	}
	// 値を返す脱出の式全体を，ラベル名のインデックスへ入れない
	size_t PreviousValueExitWork = 0;
	for(const size_t Count : { 32U, 64U, 128U }) {
		std::string Src = "fn f(){'a:loop{";
		for(size_t Idx = 0; Idx < Count; ++Idx) Src += "break {";
		Src += "break 0;";
		for(size_t Idx = 0; Idx < Count; ++Idx) Src += "};";
		Src += "};}";
		size_t Work = 0;
		const std::vector<LintWarning> Warnings = AnalyzeDiagnosticCase(Lang::Get(Lang::Rust), Src, std::string_view(), &Work);
		Check(Work, "static analysis did not record traversal work");
		Check(
			std::ranges::none_of(
				Warnings,
				[](const LintWarning &Warning) -> bool {
					return Warning.Rule == LintRule::JumpUsage;
				}
			),
			"an unlabeled value exit acquired a label warning"
		);
		if(PreviousValueExitWork) Check(Work <= 3 * PreviousValueExitWork, "value exits were repeatedly read as label text");
		PreviousValueExitWork = Work;
	}
	// 終了
	return;
}

/**
 * 重なるテキスト・欠落・コメント位置の独立した領域照合の検査関数
 * 計算量：各候補の解析と領域照合の総費用 Q に対し O(Q)
 * @throws FormatLimitExceeded 領域の識別と所有範囲の数が一致しない場合
 * @throws std::runtime_error 手で定めた欠落数と異なる場合
 * @throws std::bad_alloc ソース・照合インデックス又は失敗説明の確保に失敗した場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
static void CheckErrorZoneMatching() {
	// ソースを読む文法の取得
	const GrammarProfile &Profile = GrammarProfile::Resolve(Lang::Get(Lang::Cpp));
	const ParsedTree Repeated(Profile, "ABABA;ABABA;");
	Check(Repeated.IsParsed(), "the error-zone text could not be parsed");
	const std::vector<ValidationContext::ErrorZone> Ordered { { "missing", 0 }, { "ABA", 4 }, { "gone", 0 }, { "BA", 4 } };
	Check(
		SyntaxValidatorCmp::LostZones(Repeated, Ordered, { { 0, 0 }, { 0, 3 }, { 3, 3 }, { 3, 5 } }) == 2,
		"a missing pattern changed the cursor of a later matching zone"
	);
	const std::vector<ValidationContext::ErrorZone> Duplicates { { "ABA", 4 }, { "ABA", 4 } };
	Check(
		!SyntaxValidatorCmp::LostZones(Repeated, Duplicates, { { 0, 3 }, { 6, 9 } }),
		"duplicate overlapping patterns were not matched in source order"
	);
	// 同じテキストが後に残っても，失った最初の領域の代わりに充てない
	const ParsedTree Changed(Profile, "AXABA;ABABA;");
	Check(Changed.IsParsed(), "the changed error-zone text could not be parsed");
	Check(
		SyntaxValidatorCmp::LostZones(Changed, Duplicates, { { 0, 3 }, { 6, 9 } }) == 1,
		"a later identical occurrence replaced the lost first error zone"
	);
	const ParsedTree Comments(Profile, "A/*one*/B/*two*/;");
	Check(Comments.IsParsed(), "the comment marker text could not be parsed");
	const std::vector<ValidationContext::ErrorZone> WithMark { { std::string("A\0B", 3), 1 } };
	Check(
		!SyntaxValidatorCmp::LostZones(Comments, WithMark, { { 0, 2 } }),
		"an added comment mark prevented an existing required mark from matching"
	);
	const ParsedTree MissingMark(Profile, "A/*one*/B;");
	Check(MissingMark.IsParsed(), "the missing comment marker text could not be parsed");
	const std::vector<ValidationContext::ErrorZone> RequiredTail { { std::string("AB\0", 3), 1 } };
	Check(
		SyntaxValidatorCmp::LostZones(MissingMark, RequiredTail, { { 0, 2 } }) == 1,
		"a missing required comment mark was ignored"
	);
	const std::vector<ValidationContext::ErrorZone> LessFrequent { { std::string("A\0B", 3), 2 } };
	Check(
		!SyntaxValidatorCmp::LostZones(MissingMark, LessFrequent, { { 0, 2 } }),
		"a preserved error zone was rejected after an independent occurrence changed"
	);
	Check(
		SyntaxValidatorCmp::LostZones(MissingMark, LessFrequent, { { 3, 5 } }) == 1,
		"a missing output range reused another occurrence of the error-zone text"
	);
	// 終了
	return;
}

/**
 * 前処理の見出の保持照合と分岐本体・行末空白の非干渉の検査関数
 * 計算量：固定テストケースの解析・投影・正規化・保持照合の総費用 Q に対し O(Q)
 * @throws std::runtime_error 許可する配置・空白を拒否，又は本文の喪失・改変・複製を受理する場合
 * @throws FormatLimitExceeded 解析・投影・正規化の上限を超える場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の上限を超える場合
 * @throws std::out_of_range アンカー又は逐語コメントの範囲が不正の場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckDirectiveCommentValidation() {
	SyntaxValidatorCmp::CheckPlacedLines();
	struct Fixture {
		std::string_view Src;
		std::string_view Candidate;
		std::string_view Text;
		bool ShouldExcludeBlankLines = false;
		std::string_view ChangedKindCandidate = "";
	}; // 入力，許可する候補，破壊する本文
	const Fixture Inputs[] {
		{ "#/* keep.\n body */\n", "# // keep // body\n", "keep" },
		{ "#/* keep `a // b`.\n body */\n", "# // keep `a // b` // body\n", "keep" },
		{ "#define X 1 // keep \t\nint y = X;\n", "#define X 1 // keep\nint y = X;\n", "keep" },
		{ "#define X /* keep\n body */ 1\n", "#define X /* keep\n body */ 1\n", "keep" },
		{ "#define X(a /* keep\n body */, b) a + b\n", "#define X(a /* keep\n body */, b) a + b\n", "keep" },
		{ "#include /* keep\n body */ <stddef.h>\n", "#include /* keep\n body */ <stddef.h>\n", "keep" },
		{ "#undef /* keep\n body */ X\n", "#undef /* keep\n body */ X\n", "keep" },
		{ "#define X /* keep\n body */ 1\n#define Y 2\n", "#define X /* keep\n body */ 1\n#define Y 2\n", "keep", true },
		{ "#define X 1 /* keep\n body */\n#define Y 2\n", "#define X 1 /* keep\n body */\n#define Y 2\n", "keep", true },
		{ "#if A && /* keep\n body */ B\nint x;\n#endif\n", "#if A && /* keep */ /* body */ B\nint x;\n#endif\n", "keep" },
		{
			"#if A && \\\n/* keep */ B\nint x;\n#endif\n",
			"#if A && /* keep */ B\nint x;\n#endif\n",
			"keep",
			false,
			"// keep\n#if A && B\nint x;\n#endif\n"
		},
		{ "#if A && \\\n/* keep */ \\\nB\nint x;\n#endif\n", "// keep\n#if A && B\nint x;\n#endif\n", "keep" },
		{ "#if A\nint x;\n#else // keep \t\nint y;\n#endif\n", "#if A\nint x;\n#else // keep\nint y;\n#endif\n", "keep" },
		{ "#if A\nint x;\n#elif B // keep \t\nint y;\n#endif\n", "#if A\nint x;\n#elif B // keep\nint y;\n#endif\n", "keep" },
		{ "#if A\nint x;\n#elifdef B // keep\nint y;\n#endif\n", "#if A\nint x;\n#elifdef B // keep\nint y;\n#endif\n", "keep" },
		{ "#if A\nint x;\n#elifndef B // keep\nint y;\n#endif\n", "#if A\nint x;\n#elifndef B // keep\nint y;\n#endif\n", "keep" },
		{ "#if A\nint x;\n#endif // keep \t\n", "#if A\nint x;\n#endif // keep\n", "keep" },
		{ "#if A\nint x;\n#endif // keep\nint y;\n", "#if A\nint x;\n#endif // keep\nint y;\n", "keep" },
		{ "#if A\n#else // keep\n#endif\n", "#if A\n#else // keep\n#endif\n", "keep" },
		{ "#if A\n#if B\n#else // keep\n#endif\n#endif\n", "#if A\n#if B\n#else // keep\n#endif\n#endif\n", "keep" },
		{ "#if A\nint x;\n#else /* keep */\nint y;\n#endif\n", "#if A\nint x;\n#else /* keep */\nint y;\n#endif\n", "keep" },
		{ "#if A\nint x;\n#endif /* keep */\n", "#if A\nint x;\n#endif /* keep */\n", "keep" },
		{ "#if A\nint x;\n#endif /* keep.\n body */\n", "#if A\nint x;\n#endif // keep // body\n", "keep" },
		{ "#if A\nint x;\n// keep\n#else\nint y;\n#endif\n", "#if A\nint x;\n// keep\n#else\nint y;\n#endif\n", "keep" },
		{ "#if A\nint x;\n// keep\n#elif B\nint y;\n#endif\n", "#if A\nint x;\n// keep\n#elif B\nint y;\n#endif\n", "keep" },
		{ "#if A\nint x;\n// keep\n#endif\n", "#if A\nint x;\n// keep\n#endif\n", "keep" },
		{ "#if A\nint x;\n#else\nint y;\n// keep\n#endif\n", "#if A\nint x;\n#else\nint y;\n// keep\n#endif\n", "keep" },
		{
			"#if A /* keep */\nint x;\n// then\n#else\nint y;\n// else\n#endif\n",
			"#if A /* keep */\nint x;\n// then\n#else\nint y;\n// else\n#endif\n",
			"then"
		},
		{
			"#if A /* keep */\nint x;\n// then\n#else\nint y;\n// else\n#endif\n",
			"#if A /* keep */\nint x;\n// then\n#else\nint y;\n// else\n#endif\n",
			"keep"
		}
	};
	for(const Lang::ID Language : { Lang::C, Lang::Cpp }) for(const Fixture &Input : Inputs) {
		Core::ProjectionResult Expected = ProjectWithCtx(Lang::Get(Language), std::string(Input.Src));
		Expected.Comments.Remap(Core::SyntaxNormalizer().Normalize(Expected.Syntax));
		const std::shared_ptr<const Core::SyntaxModel> Syntax = std::make_shared<Core::SyntaxModel>(std::move(Expected.Syntax));
		Core::FormatIR IR = Core::Canonicalizer::Build(Syntax);
		IR.CloseWork();
		const Core::BrokenIR Broken = Core::LineBreaker(128).Resolve(std::move(IR));
		const LaidOutCode Code = Core::CodeLayout::Materialize(Broken);
		Core::DocCommentSynchronizer::Synchronize(*Syntax, Expected.Comments);
		Core::DocCommentSynchronizer::ConfirmGroups(Code, Expected.Comments);
		CommentNormalizer::Normalize(Expected.Comments);
		Check(
			Expected.Comments.GetDocTargets().size() == Expected.Comments.GetRecords().size(),
			"the input directive records have no confirmed documentation targets"
		);
		for(const unsigned Change : { 0U, 1U, 2U, 3U, 4U, 5U }) {
			// 種別を変えた反例は，文中を独立へ置き換えた入力だけで照合する
			if(Change == 5 && Input.ChangedKindCandidate.empty()) continue;
			std::string Candidate(Change == 5 ? Input.ChangedKindCandidate : Input.Candidate);
			const size_t At = Candidate.find(Input.Text);
			Check(At != std::string::npos, "the directive fixture lost its deliberately changed body");
			if(Change == 1) Candidate.replace(At, Input.Text.size(), "lost");
			else if(Change == 2) {
				const size_t Begin = Candidate.rfind('/', At), End = Candidate.find('\n', At);
				Check(Begin != std::string::npos, "the directive fixture lost its comment opener");
				if(Candidate[Begin] == '/' && Begin && Candidate[Begin - 1] == '/') Candidate.erase(Begin - 1, End - Begin + 1);
				else {
					const size_t Open = Candidate.rfind("/*", At), Close = Candidate.find("*/", At);
					Check(Open != std::string::npos && Close != std::string::npos, "the directive fixture lost its block bounds");
					Candidate.erase(Open, Close + 2 - Open);
				}
			} else if(Change == 3) Candidate.insert(At, std::string(Input.Text) + " ");
			else if(Change == 4) {
				const size_t Open = Candidate.rfind("/*", At);
				if(Open != std::string::npos && Candidate.find("*/", Open) > At) {
					const size_t End = Candidate.find("*/", At) + 2;
					Candidate.insert(Open, Candidate.substr(Open, End - Open) + " ");
				} else {
					const size_t Begin = Candidate.rfind("//", At), End = Candidate.find('\n', At);
					Check(Begin != std::string::npos, "the directive fixture lost its line opener");
					if(Begin && Candidate[Begin - 1] != '\n') Candidate.insert(End, " " + Candidate.substr(Begin, End - Begin));
					else Candidate.insert(Begin, Candidate.substr(Begin, End - Begin) + "\n");
				}
			}
			Core::ProjectionResult Actual = ProjectDiagnosticCase(Lang::Get(Language), Candidate);
			Check(
				Actual.Comments.GetDocTargets().size() == Actual.Comments.GetRecords().size(),
				"the candidate directive records have no projected documentation targets"
			);
			Check(
				SyntaxValidatorCmp::HasSameDirectiveComments(*Syntax, Expected, Actual) == !Change,
				(
					"directive comment preservation accepted damage or rejected a permitted form: change " + std::to_string(Change) + ": " +
					Candidate
				).c_str()
			);
		}
		for(const uint32_t Width : { 0U, 12U, 40U, 1000000U }) {
			const FormatResult Result = Formatter{ Core::LineBreaker(Width) }.Format(std::string(Input.Src), Lang::Get(Language), false);
			Check(
				Result.Classification == FormatClassification::Changed || Result.Classification == FormatClassification::Unchanged,
				"valid directive comments failed formatting at a different width"
			);
			if(Input.ShouldExcludeBlankLines) {
				Check(Result.Text.find("\n\n") == std::string::npos, "comment newlines inserted a code blank line");
			}
		}
	}
	// 分岐の本体のコメントを，見出の保持事実へ混ぜない対照例
	for(const Lang::ID Language : { Lang::C, Lang::Cpp }) {
		const Core::ProjectionResult Expected =
		ProjectDiagnosticCase(Lang::Get(Language), "#if A\nint x /* body */;\n#else // keep\nint y;\n#endif\n");
		const Core::ProjectionResult Actual =
		ProjectDiagnosticCase(Lang::Get(Language), "#if A\nint x /* changed */;\n#else // keep\nint y;\n#endif\n");
		Check(
			SyntaxValidatorCmp::HasSameDirectiveComments(Expected.Syntax, Expected, Actual),
			"a branch body comment was incorrectly counted as a directive header"
		);
	}
	// コメントとマクロの非意味的な改行を除き，文字列値の改行だけを複数行の根拠として保つ
	struct VerbatimFixture {
		Lang::ID Language;
		std::string_view Src;
		size_t BlankLines; // コードの構造から必要な空行数（コメント本文に空行を持つ入力は含まない）
	};
	const VerbatimFixture VerbatimInputs[] {
		{ Lang::C, "#define S(x) #x\nconst char *x = S(/* keep\n body */ a);\nconst char *y = S(b);\n", 1 },
		{ Lang::Cpp, "#define S(x) #x\nconst char *x = S(/* keep\n body */ a);\nconst char *y = S(b);\n", 1 },
		{ Lang::Cpp, "const char *x = R\"(a\nb)\" /* keep\n body */;\nconst char *y = \"c\";\n", 1 },
		{ Lang::Rust, "macro_rules! x { () => { /* keep\n body */ 1 }; }\nmacro_rules! y { () => { 2 }; }\n", 0 },
		{ Lang::Rust, "m!(/* keep\n body */ 1);\nm!(2);\n", 0 },
		{ Lang::Rust, "m!(1,\n/* keep\n body */ 2);\nm!(3);\n", 0 },
		{ Lang::Rust, "m!(\"a\nb\", /* keep\n body */ 1);\nm!(2);\n", 1 }
	};
	// マクロの定義と文字列化の実引数のコメントは，最初の投影と完成候補の投影の何方でもコメント経路へ分離する
	for(const Lang::ID Language : { Lang::C, Lang::Cpp, Lang::Rust }) for(const bool IsVerification : { false, true }) {
		const std::string Src = Language == Lang::Rust ?
		"macro_rules! x { () => { /* keep\n body */ 1 }; }\n" :
		"#define S(x) #x\nconst char *x = S(/* keep\n body */ a);\n";
		const Core::ProjectionResult Projection =
		IsVerification ? ProjectDiagnosticCase(Lang::Get(Language), Src) : ProjectWithCtx(Lang::Get(Language), Src);
		for(Core::NodeId Id = 0; Id < Projection.Syntax.GetNodeCount(); ++Id) {
			const Core::SyntaxNode &Node = Projection.Syntax.GetNode(Id);
			Check(Node.Lexeme.find("keep") == std::string::npos, "a macro comment was projected into code text");
			for(const Core::SemanticBoundary &Boundary : Node.Facts.Boundaries) {
				Check(Boundary.Gap.find("keep") == std::string::npos, "a macro comment was projected into a code gap");
			}
		}
		Check(Projection.Comments.GetRecords().size() == 1, "a macro comment did not enter the comment repository");
	}
	for(const VerbatimFixture &Input : VerbatimInputs) for(const uint32_t Width : { 0U, 128U }) {
		const FormatResult Result =
		Formatter{ Core::LineBreaker(Width) }.Format(std::string(Input.Src), Lang::Get(Input.Language), false);
		Check(
			Result.Classification == FormatClassification::Changed || Result.Classification == FormatClassification::Unchanged,
			"a verbatim comment or value failed formatting"
		);
		size_t BlankLines = 0;
		for(size_t Pos = Result.Text.find("\n\n"); Pos != std::string::npos; Pos = Result.Text.find("\n\n", Pos + 2)) ++BlankLines;
		Check(BlankLines == Input.BlankLines, "a verbatim comment or value changed the code blank lines");
		Check(Result.Text.find("keep") != std::string::npos, "a verbatim comment was lost while counting code newlines");
		if(Input.Src.find("a\nb") != std::string_view::npos) {
			Check(Result.Text.find("a\nb") != std::string::npos, "a string value lost its physical newline");
		}
	}
	// コメント無と同行のコメントを持つ対照も，同じトークン列の定義として空行無で並べる
	for(const std::string_view Comment : { "", "/* keep body */ " }) for(const uint32_t Width : { 0U, 40U, 128U }) {
		const std::string Src = "macro_rules! x { () => { " + std::string(Comment) + "1 }; }\nmacro_rules! y { () => { 2 }; }\n";
		const FormatResult Result = Formatter{ Core::LineBreaker(Width) }.Format(Src, Lang::Get(Lang::Rust), false);
		Check(
			(Result.Classification == FormatClassification::Changed || Result.Classification == FormatClassification::Unchanged) &&
			Result.Text == Src,
			"a macro definition token sequence was expanded as a statement body"
		);
	}
	// 終了
	return;
}

/**
 * 検証の位置データと保護原文の所有の検査関数
 * 計算量：固定テストケースの投影・静的解析・項目構造化・同期の総費用 Q に対し O(Q)
 * @throws std::runtime_error 位置・原文所有・ディレクティブの範囲・ドキュメントの構造・同期の結果が要求と異なる場合
 * @throws FormatLimitExceeded 投影又は照合の上限を超えた場合
 * @throws std::bad_alloc 投影又は照合データの確保に失敗した場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckDiagnosticProjection() {
	// 名前の一致でなく，確認した解決先だけがメンバの順位を決める
	struct MemberBindingCase {
		::Lang::ID Lang; // テストケースの言語
		const char *Src; // 手書の入力
		size_t Warnings; // メンバの順位の警告数の手期待
	};
	static constexpr MemberBindingCase MemberBindings[] {
		{ Lang::Kotlin, "object C{fun f(){}\n @JvmStatic fun g(){}\n}", 1 },
		{ Lang::Kotlin, "import kotlin.jvm.JvmStatic as S\nobject C{fun f(){}\n @S fun g(){}\n}", 1 },
		{ Lang::Kotlin, "object C{fun f(){}\n @kotlin.jvm.JvmStatic fun g(){}\n}", 1 },
		{ Lang::Kotlin, "annotation class JvmStatic\nobject C{fun f(){}\n @JvmStatic fun g(){}\n}", 0 },
		{ Lang::Kotlin, "import other.JvmStatic\nobject C{fun f(){}\n @JvmStatic fun g(){}\n}", 0 },
		{ Lang::Kotlin, "object C{fun f(){}\n @JvmField val x=1\n}", 1 },
		{ Lang::Ruby, "class C\ndef f; 1; end\ninclude M\nend\n", 1 },
		{ Lang::Ruby, "class C\ndef self.include(x); x; end\ninclude M\nend\n", 0 },
		{ Lang::Ruby, "class C\ndef f; 1; end\nattr_reader :a\nend\n", 1 },
		{ Lang::Ruby, "class C\ndef self.attr_reader(x); x; end\nattr_reader :a\nend\n", 0 },
		{ Lang::Ruby, "class C\ndef f; 1; end\nself.include M\nend\n", 1 },
		{ Lang::Ruby, "class C\ndef f; 1; end\nself.extend M\nend\n", 1 },
		{ Lang::Ruby, "class C\ndef f; 1; end\nself.prepend M\nend\n", 1 },
		{ Lang::Ruby, "class C\ndef f; 1; end\nself.attr_reader :a\nend\n", 1 },
		{ Lang::Ruby, "class C\ndef f; 1; end\n(self).attr_reader :a\nend\n", 1 },
		{ Lang::Ruby, "class C\ndef self.include(x); x; end\nself.include M\nend\n", 0 },
		{ Lang::Ruby, "class C\ndef self.attr_reader(x); x; end\nself.attr_reader :a\nend\n", 0 },
		{ Lang::Python, "class C:\n def f(self):\n  pass\n @staticmethod\n def g(x):\n  pass\n", 1 },
		{ Lang::Python, "from builtins import staticmethod as s\nclass C:\n def f(self):\n  pass\n @s\n def g(x):\n  pass\n", 1 },
		{ Lang::Python, "import builtins as b\ns=b.staticmethod\nclass C:\n def f(self):\n  pass\n @s\n def g(x):\n  pass\n", 1 },
		{ Lang::Python, "def staticmethod(f):\n return f\nclass C:\n def f(self):\n  pass\n @staticmethod\n def g(x):\n  pass\n", 0 },
		{
			Lang::Python,
			"s=staticmethod\nstaticmethod=custom\nclass C:\n def f(self):\n  pass\n @staticmethod\n def g(x):\n  pass\n",
			0
		},
		{
			Lang::Python,
			"import builtins as b\nb.staticmethod=custom\nclass C:\n def f(self):\n  pass\n @b.staticmethod\n def g(x):\n  pass\n",
			0
		},
		{ Lang::Python, "class C:\n def f(self):\n  pass\n @classmethod\n def g(cls):\n  pass\n", 1 },
		{ Lang::Python, "class C:\n def f(self):\n  pass\n @property\n def g(self):\n  pass\n", 1 },
		{ Lang::Python, "property=custom\nclass C:\n def f(self):\n  pass\n @property\n def g(self):\n  pass\n", 0 }
	};
	for(const MemberBindingCase &Input : MemberBindings) {
		const std::vector<LintWarning> Warnings = AnalyzeDiagnosticCase(Lang::Get(Input.Lang), Input.Src);
		Check(
			static_cast<size_t>(
				std::ranges::count_if(
					Warnings,
					[](const LintWarning &Warning) -> bool {
						return Warning.Rule == LintRule::MemberOrder; // メンバの順位の警告かの返戻
					}
				)
			) == Input.Warnings,
			"member ordering used an unresolved standard name",
			Input.Src
		);
	}
	// ドキュメントの対象も同じ投影を読み，利用者の操作や未知の受手を宣言へ昇格しない
	struct DeclarationBindingCase {
		const char *Src; // 手書の入力
		size_t Declarations; // ドキュメント化対象数の手期待
	};
	static constexpr DeclarationBindingCase DeclarationBindings[] {
		{ "class C\nattr_reader :a\nattr_writer :b\nattr_accessor :c\nattr :d\nend\n", 4 },
		{ "class C\ndef self.attr_reader(x); x; end\nattr_reader :a\nend\n", 0 },
		{ "class C\nother.attr_reader :a\nend\n", 0 },
		{ "attr_reader :a\n", 0 },
		{ "class C\ndef self.attr_reader(x); x; end\nself.attr_reader :a\nend\n", 0 },
		{ "class C\nself.attr_reader :a\nend\n", 1 },
		{ "class C\nself.attr_reader :a\nself.attr_writer :b\nself.attr_accessor :c\nself.attr :d\nend\n", 4 },
		{ "class C\n(self).attr_reader :a\nend\n", 1 },
		{ "class C\ndef self.attr_reader(x); x; end\n(self).attr_reader :a\nend\n", 0 },
		{ "class C\n(other).attr_reader :a\nend\n", 0 }
	};
	for(const DeclarationBindingCase &Input : DeclarationBindings) for(const bool IsVerification : { false, true }) {
		const Core::ProjectionResult Projection =
		IsVerification ? ProjectDiagnosticCase(Lang::Get(Lang::Ruby), Input.Src) : ProjectWithCtx(Lang::Get(Lang::Ruby), Input.Src);
		const size_t Declarations = Core::DocCommentReaderAccess::GetDeclarationCallCount(Projection.Syntax);
		Check(Declarations == Input.Declarations, "documentation used an unresolved declaration call", Input.Src);
	}
	const Formatter Shaver { Core::LineBreaker(128) };
	// エンコーディングの値・別名・成立する位置を入力で確定し，対応外の読みを原文の儘見送る
	const std::string MetaAtLimit =
	std::string(1024 - std::string_view("<meta charset=Shift_JIS>").size(), ' ') + "<meta charset=Shift_JIS>";
	const std::string MetaAfterLimit = " " + MetaAtLimit;
	const std::string MetaAfterLimitOutput = std::string(1001, ' ') + "<meta charset=Shift_JIS>\n";
	const std::string CharsetAtLimit =
	"@charset \"" + std::string(1024 - std::string_view("@charset \"Shift_JIS\";").size(), ' ') + "Shift_JIS\";";
	const std::string CharsetAfterLimit =
	"@charset \"" + std::string(1025 - std::string_view("@charset \"Shift_JIS\";").size(), ' ') + "Shift_JIS\";";
	struct EncodingCase {
		::Lang Lang;
		std::string_view Text;
		FailureReason::Kind Reason;
		std::optional<Core::OutputPoint> Point = std::nullopt;
		std::optional<std::string_view> Expected = std::nullopt;
	};
	const EncodingCase EncodingInputs[] {
		{ Lang::Get(Lang::Ruby), "\n# coding: Shift_JIS\nx=1\n", FailureReason::None },
		{ Lang::Get(Lang::Ruby), "\xEF\xBB\xBF# encoding: ISO-8859-1\nx=1\n", FailureReason::None },
		{ Lang::Get(Lang::Ruby), "# -*- coding: Shift_JIS; encoding: \"UTF-8\" -*-\nx=1\n", FailureReason::None },
		{
			Lang::Get(Lang::Ruby),
			"# -*- coding: UTF-8; encoding: \"Shift_JIS\" -*-\nx=1\n",
			FailureReason::UnsupportedEncodingDeclaration
		},
		{ Lang::Get(Lang::Ruby), "# coding: EUC-JP\nx=1\n", FailureReason::None },
		{ Lang::Get(Lang::Ruby), "# encoding: UTF_8\nx=1\n", FailureReason::UnsupportedEncodingDeclaration },
		{ Lang::Get(Lang::Ruby), "# encoding: nonexistent-encoding\nx=1\n", FailureReason::UnsupportedEncodingDeclaration },
		{ Lang::Get(Lang::Ruby), "# other: Shift_JIS\nx=1\n", FailureReason::None },
		{ Lang::Get(Lang::Ruby), "#!/usr/bin/env ruby\n# coding: cp932\nx=1\n", FailureReason::UnsupportedEncodingDeclaration },
		{ Lang::Get(Lang::Ruby), "x=1\n# encoding: Shift_JIS\n", FailureReason::None },
		{
			Lang::Get(Lang::Ruby),
			"#!/usr/bin/env ruby\r\n# coding: cp932\r\nx=1\r\n",
			FailureReason::UnsupportedEncodingDeclaration,
			Core::OutputPoint{ 1, 10 }
		},
		{
			Lang::Get(Lang::Python),
			"# coding: utf-7\nx=1\n",
			FailureReason::UnsupportedEncodingDeclaration,
			Core::OutputPoint{ 0, 10 }
		},
		{ Lang::Get(Lang::Python), "# coding: UTF_8\nx=1\n", FailureReason::None },
		{ Lang::Get(Lang::Python), "# coding: latin-1\nx=1\n", FailureReason::None },
		{ Lang::Get(Lang::Python), "\xEF\xBB\xBF# coding: latin-1\nx=1\n", FailureReason::InputSyntaxError },
		{ Lang::Get(Lang::Python), "\xEF\xBB\xBF# coding: utf8\nx=1\n", FailureReason::InputSyntaxError },
		{ Lang::Get(Lang::Python), "\xEF\xBB\xBF# coding: cp65001\nx=1\n", FailureReason::InputSyntaxError },
		{ Lang::Get(Lang::Python), "\xEF\xBB\xBF# coding: UTF_8\nx=1\n", FailureReason::None },
		{ Lang::Get(Lang::Python), "\xEF\xBB\xBF# coding: utf-8-extra\nx=1\n", FailureReason::None },
		{
			Lang::Get(Lang::Python),
			"\xEF\xBB\xBF#!/usr/bin/env python\r\n# coding: latin-1\r\nx=1\r\n",
			FailureReason::InputSyntaxError
		},
		{ Lang::Get(Lang::Python), "\xEF\xBB\xBF\n# coding: latin-1", FailureReason::InputSyntaxError },
		{ Lang::Get(Lang::Python), "\xEF\xBB\xBF# coding: latin-1", FailureReason::InputSyntaxError },
		{ Lang::Get(Lang::Python), "\xEF\xBB\xBFx=1\n# coding: latin-1\n", FailureReason::None },
		{ Lang::Get(Lang::Python), "\xEF\xBB\xBF# note\n# note\n# coding: latin-1\nx=1\n", FailureReason::None },
		{ Lang::Get(Lang::Python), "\xEF\xBB\xBFx=1\n", FailureReason::None },
		{
			Lang::Get(Lang::HTML),
			"<meta charset=Shift_JIS><script>// note\xE2\x80\xA8globalThis.hit=true;\n</script>\n",
			FailureReason::UnsupportedEncoding
		},
		{ Lang::Get(Lang::HTML), "<meta charset=' ISO-8859-1 '><p>x</p>\n", FailureReason::None },
		{ Lang::Get(Lang::HTML), "\xEF\xBB\xBF<meta charset=latin1><p>x</p>\n", FailureReason::InputSyntaxError },
		{ Lang::Get(Lang::HTML), "\xEF\xBB\xBF<meta charset=UTF8><p>x</p>\n", FailureReason::None },
		{ Lang::Get(Lang::HTML), "\xEF\xBB\xBF<meta charset=UTF-16BE><p>x</p>\n", FailureReason::None },
		{ Lang::Get(Lang::HTML), "<META/CHARSET='x-sjis'><p>x</p>\n", FailureReason::UnsupportedEncoding },
		{
			Lang::Get(Lang::HTML),
			"<meta content='text/html; CHARSET=Shift_JIS' HTTP-EQUIV=Content-Type>\n",
			FailureReason::UnsupportedEncoding
		},
		{ Lang::Get(Lang::HTML), "<meta content='text/html; charset=Shift_JIS'><p>x</p>\n", FailureReason::None },
		{ Lang::Get(Lang::HTML), "<meta http-equiv=' content-type ' content='charset=Shift_JIS'>\n", FailureReason::None },
		{ Lang::Get(Lang::HTML), "<!--<meta charset=Shift_JIS>--><meta charset=utf-8>\n", FailureReason::None },
		{ Lang::Get(Lang::HTML), "<div title='<meta charset=Shift_JIS>'></div><meta charset=utf-8>\n", FailureReason::None },
		{ Lang::Get(Lang::HTML), "<!--><meta charset=Shift_JIS>\n", FailureReason::UnsupportedEncoding },
		{ Lang::Get(Lang::HTML), "<meta charset=utf-8 charset=Shift_JIS>\n", FailureReason::None },
		{ Lang::Get(Lang::HTML), "<meta charset=Shift_JIS charset=utf-8>\n", FailureReason::UnsupportedEncoding },
		{ Lang::Get(Lang::HTML), "<meta http-equiv=wrong http-equiv=content-type content='charset=Shift_JIS'>\n", FailureReason::None },
		{ Lang::Get(Lang::HTML), "<meta charset=unknown><meta charset=Shift_JIS>\n", FailureReason::UnsupportedEncoding },
		{ Lang::Get(Lang::HTML), "<meta charset=utf-8><meta charset=Shift_JIS>\n", FailureReason::None },
		{ Lang::Get(Lang::HTML), "<meta content='charset=Shift_JIS' http-equiv=content-type charset=utf-8>\n", FailureReason::None },
		{ Lang::Get(Lang::HTML), "<meta charset=unknown http-equiv=content-type content='charset=Shift_JIS'>\n", FailureReason::None },
		{ Lang::Get(Lang::HTML), "<meta charset='&#117;tf-8'><meta charset=Shift_JIS>\n", FailureReason::UnsupportedEncoding },
		{
			Lang::Get(Lang::HTML),
			"<meta http-equiv=content-type content='charset wrong; charset = \"x-sjis\"'>\n",
			FailureReason::UnsupportedEncoding
		},
		{ Lang::Get(Lang::HTML), "<meta charset=x-user-defined>\n", FailureReason::None },
		{ Lang::Get(Lang::HTML), "<meta charset=euc-kr>\n", FailureReason::UnsupportedEncoding },
		{ Lang::Get(Lang::HTML), "<?xml encoding='Shift_JIS'?><p>x</p>\n", FailureReason::UnsupportedEncoding },
		{ Lang::Get(Lang::HTML), "<?xml encoding='Shift_JIS'?><meta charset=utf-8>\n", FailureReason::None },
		{ Lang::Get(Lang::HTML), MetaAtLimit, FailureReason::UnsupportedEncoding },
		{ Lang::Get(Lang::HTML), MetaAfterLimit, FailureReason::None, std::nullopt, std::string_view(MetaAfterLimitOutput) },
		{ Lang::Get(Lang::CSS), "@charset \"Shift_JIS\";a{content:\"x\"}\n", FailureReason::UnsupportedEncoding },
		{ Lang::Get(Lang::CSS), "@charset \"iso-8859-1\";a{color:red}\n", FailureReason::None },
		{ Lang::Get(Lang::CSS), "@charset \" UTF8 \";a{color:red}\n", FailureReason::None },
		{ Lang::Get(Lang::CSS), "@charset \"utf-16le\";a{color:red}\n", FailureReason::None },
		{ Lang::Get(Lang::CSS), "@charset \"x-euc-jp\";a{color:red}\n", FailureReason::None },
		{ Lang::Get(Lang::CSS), "@charset \"gb2312\";a{color:red}\n", FailureReason::UnsupportedEncoding },
		{ Lang::Get(Lang::CSS), "@charset \"replacement\";a{color:red}\n", FailureReason::UnsupportedEncoding },
		{ Lang::Get(Lang::CSS), "\xEF\xBB\xBF@charset \"iso-8859-1\";a{color:red}\n", FailureReason::InputSyntaxError },
		{ Lang::Get(Lang::CSS), "\xEF\xBB\xBF@charset \"UTF8\";a{color:red}\n", FailureReason::None },
		{ Lang::Get(Lang::CSS), "@CHARSET \"Shift_JIS\";a{color:red}\n", FailureReason::None },
		{
			Lang::Get(Lang::CSS),
			" @charset \"Shift_JIS\";a{color:red}\n",
			FailureReason::None,
			std::nullopt,
			" @charset \"Shift_JIS\";\n\na {\n\tcolor: red;\n}\n"
		},
		{ Lang::Get(Lang::CSS), "/*note*/@charset \"Shift_JIS\";a{color:red}\n", FailureReason::None },
		{
			Lang::Get(Lang::CSS),
			"@charset 'Shift_JIS';a{color:red}\n",
			FailureReason::None,
			std::nullopt,
			"@charset 'Shift_JIS';\n\na {\n\tcolor: red;\n}\n"
		},
		{
			Lang::Get(Lang::CSS),
			"@charset  \"Shift_JIS\";a{color:red}\n",
			FailureReason::None,
			std::nullopt,
			"@charset  \"Shift_JIS\";\n\na {\n\tcolor: red;\n}\n"
		},
		{
			Lang::Get(Lang::CSS),
			"@charset \"Shift_JIS\" ;a{color:red}\n",
			FailureReason::None,
			std::nullopt,
			"@charset \"Shift_JIS\" ;\n\na {\n\tcolor: red;\n}\n"
		},
		{ Lang::Get(Lang::CSS), "@charset \"unknown\";a{color:red}\n", FailureReason::None },
		{ Lang::Get(Lang::CSS), CharsetAtLimit, FailureReason::UnsupportedEncoding },
		{ Lang::Get(Lang::CSS), CharsetAfterLimit, FailureReason::None },
		{ Lang::FromName("scss"), "@charset \"Shift_JIS\";a{color:red}\n", FailureReason::None }
	};
	for(const EncodingCase &Input : EncodingInputs) {
		const GrammarProfile &Profile = GrammarProfile::Resolve(Input.Lang);
		const InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, std::string(Input.Text));
		Check(Normalized.Reason == Input.Reason, "encoding admission ignored a codec, an alias, or a header position");
		if(Input.Expected) {
			const FormatResult Result = Shaver.Format(std::string(Input.Text), Input.Lang, false);
			Check(
				Result.Classification == FormatClassification::Changed && Result.Reason == FailureReason::None &&
				Result.Text == *Input.Expected,
				"an inactive encoding declaration became active during formatting"
			);
		} else if(Input.Reason != FailureReason::None) {
			const FormatResult Result = Shaver.Format(std::string(Input.Text), Input.Lang, false);
			if(Input.Point) {
				Check(
					Result.ReasonPoint && Result.ReasonPoint->Row == Input.Point->Row && Result.ReasonPoint->Column == Input.Point->Column,
					"an unsupported encoding declaration lost its original position"
				);
			}
			Check(
				Result.Classification == FormatClassification::Skipped && Result.Reason == Input.Reason && Result.Text == Input.Text,
				"a rejected encoding declaration was formatted or lost its original bytes"
			);
		}
	}
	// 親のエンコーディングでデコード済の埋込スタイルでは，バイト列の宣言を適用しない
	Check(
		InputNormalizer::Normalize(GrammarProfile::Resolve(Lang::Get(Lang::CSS)), "@charset \"Shift_JIS\";a{color:red}\n", false)
		.Reason == FailureReason::None,
		"a decoded embedded stylesheet selected its own byte encoding"
	);
	// 診断座標の入力と手期待
	struct PosFixture {
		::Lang Lang;
		const char *Src;
		uint32_t Row;
		uint32_t Column;
	};
	const PosFixture PosInputs[] {
		{ Lang::FromName("css"), "a {\r  color: red;\r}\r// x\rb { color: blue; }\r", 3, 0 },
		{ Lang::FromName("css"), "a {\r\n  color: red;\r\n}\r\n// x\r\nb { color: blue; }\r\n", 3, 0 },
		{ Lang::Get(Lang::C), "#define A 1 + \\\n\t2\nconst char *s = \"abc;\n", 2, 0 },
		{ Lang::Get(Lang::CSharp), "class C {\u2028string S = \"bad;\u2028}\n", 1, 0 },
		{ Lang::Get(Lang::Python), "def f():\ng()\n", 0, 8 }
	};
	for(const PosFixture &Input : PosInputs) {
		const FormatResult Result = Shaver.Format(Input.Src, Input.Lang, false);
		Check(Result.Classification == FormatClassification::Skipped && Result.Text == Input.Src, "a rejected input changed bytes");
		if(Input.Lang == Lang::Python) {
			Check(Result.InputIssue == ParseIssue::InputSyntax, "a missing Python body became a parser limitation");
		}
		Check(
			Result.ReasonPoint && Result.ReasonPoint->Row == Input.Row && Result.ReasonPoint->Column == Input.Column,
			"a skip diagnostic used prepared text instead of the input coordinates"
		);
	}
	// 解析用の連結指定の前後で，局所的なマクロの未認識を部分整形として検証する
	const std::string Headers[] {
		"#if defined(__cplusplus)\nextern \"C\" {\n#endif\nint CALL_MODE api(int value);\n" "#if defined(__cplusplus)\n}\n#endif\n",
		"#if defined(__cplusplus)\nextern \"C\" { // linkage\n#endif\nint CALL_MODE api(int value);\n"
		"#if defined(__cplusplus)\n}\n#endif\n",
		"#if defined(__cplusplus)\nextern \"C\" {\n#endif\n// declaration\nint CALL_MODE api(int value);\n"
		"#if defined(__cplusplus)\n}\n#endif\n",
		"#if defined(__cplusplus)\nextern \"C\" { // open\n#endif\nint f(int value);\n"
		"#if defined(__cplusplus)\n} /* close */\n#endif\n"
		"#if defined(__cplusplus)\nextern \"C\" {\n#endif\nint CALL_MODE api(int value);\n"
		"#if defined(__cplusplus)\n} // close again\n#endif\n   "
	};
	for(const std::string &Header : Headers) {
		for(const ::Lang Language : { Lang::Get(Lang::C), Lang::Get(Lang::Cpp), Lang::FromName("h") }) {
			for(const size_t Width : { 0U, 20U, 128U }) {
				const FormatResult Result = Formatter{ Core::LineBreaker(Width) }.Format(Header, Language, false);
				Check(
					Result.Classification == FormatClassification::Partial,
					"a guarded local parser error became a structure failure",
					Header.c_str()
				);
				Check(Result.Text.find("CALL_MODE") != std::string::npos, "partial formatting lost the calling convention", Header.c_str());
			}
		}
	}
	const FormatResult PresentBody = Shaver.Format("def f():\n\tg()\n", Lang::Get(Lang::Python), false);
	Check(
		PresentBody.Classification == FormatClassification::Changed || PresentBody.Classification == FormatClassification::Unchanged,
		"an indented Python body was rejected with the missing body"
	);
	// 終了
	return;
}

/**
 * 仮引数・捕捉型の不足の固定テストケースの検査関数
 * 計算量：固定テストケースの投影・静的解析・照合の総費用 Q に対し O(Q)
 * @throws std::runtime_error 手期待の警告の個数又は区分と異なる場合
 * @throws FormatLimitExceeded 投影又は静的解析の上限を超えた場合
 * @throws std::bad_alloc 投影又は警告の確保に失敗した場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckParamAndDocDiagnostics() {
	// 仮引数・捕捉型の不足の固定テストケースの検査の入力と手期待
	struct Fixture {
		::Lang::ID Lang;
		const char *Src;
		LintRule Rule;
		size_t Warnings;
		const char *Msg;
		std::string_view Attribute = std::string_view(); // 先行するドキュメント属性の手期待（指定しなければ空）
	};
	constexpr Fixture Inputs[] {
		{ Lang::C, "int f(int x){return x;}", LintRule::ParameterType, 0, "" },
		{ Lang::Cpp, "auto f()->int; auto (*p)()->int=f; template<class T> auto g(T x)->T{return x;}", LintRule::InferredType, 0, "" },
		{ Lang::Cpp, "auto f()->decltype(auto){return 1;} decltype(auto) g(){return 1;} auto x=1;", LintRule::InferredType, 3, "" },
		{ Lang::CSharp, "class C{System.Func<int,int> f=x=>x; System.Func<int,int> g=(x)=>x;}", LintRule::ParameterType, 2, "" },
		{ Lang::CSharp, "class C{System.Func<int,int> f=(int x)=>x;}", LintRule::ReturnType, 1, "" },
		{ Lang::CSharp, "class C{System.Func<int,int> f=int (int x)=>x;}", LintRule::ReturnType, 0, "" },
		{ Lang::Go, "package p\nvar x=1\nvar y,z=2,3\nfunc f(){a,b:=1,2;_=a;_=b;}", LintRule::DeclarationType, 3, "" },
		{ Lang::Go, "package p\nvar x,y int=1,2\nconst(a int=1;b;c=2;d)\n", LintRule::DeclarationType, 1, "" },
		{ Lang::Rust, "fn f(){let c=|mut x|();}", LintRule::ParameterType, 1, "Review: Add an explicit type to parameter 'x'" },
		{ Lang::Rust, "fn f(){let c=|&(x,y)|();}", LintRule::ParameterType, 1, "Review: Add an explicit type to parameter #1" },
		{ Lang::Rust, "fn f(){let c=|x|x+1; let d=|x:i32|{x};}", LintRule::ReturnType, 3, "" },
		{ Lang::Rust, "fn f()->(){let x:i32=1;let c=|x:i32|->i32{x};}", LintRule::DeclarationType, 0, "" },
		{ Lang::Swift, "let f={x in x+1}", LintRule::ParameterType, 1, "" },
		{ Lang::TypeScript, "function f(...xs){}", LintRule::ParameterType, 1, "Review: Add an explicit type to parameter 'xs'" },
		{ Lang::Python, "def f(x):\n\treturn x\n", LintRule::MissingDocumentation, 0, "" },
		{ Lang::Python, "def f(x):\n\t\"\"\"\"\"\"\n\treturn x\n", LintRule::DocumentationSummary, 0, "" },
		{ Lang::Python, "def f(x):\n\t\"\"\"Summary\"\"\"\n\treturn x\n", LintRule::DocumentationParameter, 0, "" },
		{ Lang::Python, "def f(x):\n\t\"\"\"Summary\"\"\"\n\treturn x\n", LintRule::DocumentationReturn, 0, "" },
		{ Lang::Python, "def f(x):\n\t'Summary'\n\treturn x\n", LintRule::DocumentationNotation, 0, "" },
		{
			Lang::CSharp,
			"class C {\n/** <summary></summary><param name=\"x\"></param><returns></returns> */\nint F(int x) => x; }\n",
			LintRule::DocumentationSummary,
			1,
			""
		},
		{
			Lang::CSharp,
			"class C {\n/** <summary></summary><param name=\"x\"></param><returns></returns> */\nint F(int x) => x; }\n",
			LintRule::DocumentationParameter,
			1,
			""
		},
		{
			Lang::CSharp,
			"class C {\n/** <summary></summary><param name=\"x\"></param><returns></returns> */\nint F(int x) => x; }\n",
			LintRule::DocumentationReturn,
			1,
			""
		},
		{ Lang::Go, "package p\n// F\nfunc F(a,b int,xs ...int)(int,int){return a,b}\n", LintRule::DocumentationParameter, 3, "" },
		{
			Lang::Cpp,
			"void F() { Consume((int) Value); }\n",
			LintRule::CStyleCast,
			1,
			"Violation: Replace the C-style cast with the appropriate C++ cast"
		},
		{ Lang::Cpp, "void F() { Consume((Widget *) Value); }\n", LintRule::CStyleCast, 0, "" },
		{
			Lang::JavaScript,
			"/** Card\n * @param {Object} props Properties\n * @param {string} props.title Title\n * @return Title\n */\nfunction card({title,body}) { return title; }\n",
			LintRule::DocumentationParameter,
			0,
			""
		},
		{
			Lang::JavaScript,
			"/** Card\n * @param {Object} props\n * @param {string} props.title Title\n * @return Title\n */\nfunction card({title,body}) { return title; }\n",
			LintRule::DocumentationParameter,
			1,
			"Violation: Documentation is missing a description for parameter #1"
		},
		{
			Lang::JavaScript,
			"/** f\n * @param renamed Description\n */\nfunction f({key: renamed, ...rest}) {}\n",
			LintRule::DocumentationParameter,
			1,
			"Violation: Documentation is missing a description for parameter #1"
		},
		{
			Lang::TypeScript,
			"/** Card\n * @param {Object} root0 Properties\n * @param {string} root0.title Title\n * @return Title\n */\nfunction card({title,body}: {title:string;body:string}): string { return title; }\n",
			LintRule::DocumentationParameter,
			0,
			""
		}
	};
	for(const Fixture &Input : Inputs) {
		size_t Count = 0;
		for(const LintWarning &Warning : AnalyzeDiagnosticCase(Lang::Get(Input.Lang), Input.Src, Input.Attribute)) {
			if(Warning.Rule != Input.Rule) continue;
			++Count;
			Check(std::string_view(Input.Msg).empty() || Warning.Msg == Input.Msg, "review-six diagnostic text differs", Input.Src);
		}
		if(Count != Input.Warnings) {
			Check(
				false,
				(
					std::string("review-six diagnostic count differs from the hand expectation: lang=") +
					std::to_string(static_cast<size_t>(Input.Lang)) + " rule=" + std::to_string(static_cast<size_t>(Input.Rule)) + " expected=" +
					std::to_string(Input.Warnings) + " actual=" + std::to_string(Count)
				).c_str(),
				Input.Src
			);
		}
	}
	// 無名の仮引数の序数と，ラッパー内の名前の診断位置の手期待
	struct ParamFixture {
		::Lang Lang;
		const char *Src;
		std::array<uint32_t, 2> Columns;
		std::array<const char *, 2> Subjects;
		size_t Count;
	};
	const ParamFixture ParamInputs[] {
		{ Lang::Get(Lang::Rust), "fn f(){let g=|_,(x,y)|();}", { 14, 16 }, { "#1", "#2" }, 2 },
		{ Lang::Get(Lang::Rust), "fn f(){let g=|x:i32,_,(a,b)|();}", { 20, 22 }, { "#2", "#3" }, 2 },
		{ Lang::Get(Lang::Rust), "fn f(){let g=|&x|();}", { 15, 0 }, { "'x'", "" }, 1 },
		{ Lang::Get(Lang::TypeScript), "const f = (...xs) => xs;", { 14, 0 }, { "'xs'", "" }, 1 },
		{ Lang::Get(Lang::TypeScript), "const f=(x:number,[y])=>y;", { 18, 0 }, { "#2", "" }, 1 }
	};
	for(const ParamFixture &Input : ParamInputs) {
		size_t Count = 0;
		for(const LintWarning &Warning : AnalyzeDiagnosticCase(Input.Lang, Input.Src)) {
			if(Warning.Rule != LintRule::ParameterType) continue;
			Check(Count < Input.Count, "a parameter emitted extra type warnings", Input.Src);
			Check(
				!Warning.Row && Warning.Column == Input.Columns[Count] &&
				Warning.Msg == "Review: Add an explicit type to parameter " + std::string(Input.Subjects[Count]),
				"a parameter lost its name, ordinal or position",
				Input.Src
			);
			++Count;
		}
		Check(Count == Input.Count, "a parameter lost its type warning", Input.Src);
	}
	// 後置戻値型は矢印でも定義名でもなく，明示された型の開始位置へ警告を結び付ける
	struct ResultFixture {
		const char *Src;
		uint32_t Column; // 後置戻値型の位置の手期待
	};
	constexpr ResultFixture ResultInputs[] {
		{ "/**\n * Summary\n * @return\n */\nauto f() -> int { return 1; }\n", 12 },
		{ "/**\n * Summary\n * @return\n */\nauto f() -> /* Type */ int { return 1; }\n", 23 }
	};
	for(const ResultFixture &Input : ResultInputs) {
		size_t ReturnDescriptions = 0;
		for(const LintWarning &Warning : AnalyzeDiagnosticCase(Lang::Get(Lang::Cpp), Input.Src)) {
			if(Warning.Rule == LintRule::DocumentationReturn) {
				++ReturnDescriptions;
				Check(Warning.Row == 4 && Warning.Column == Input.Column, "a trailing return type lost its explicit type position", Input.Src);
			}
		}
		Check(ReturnDescriptions == 1, "a trailing return type changed the return description count", Input.Src);
	}
	// 終了
	return;
}

/**
 * 静的解析の候補形と固定文面の検査関数
 * 計算量：固定テストケースの投影・静的解析・文面照合の総費用 Q に対し O(Q)
 * @throws std::runtime_error 候補形・固定文面又は件数が手期待と異なる場合
 * @throws FormatLimitExceeded 投影又は静的解析の上限を超えた場合
 * @throws std::bad_alloc 投影又は警告の確保に失敗した場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckReviewFacts() {
	// 同じ規則の候補形・対象外の形と固定文面の検査
	struct Fixture {
		::Lang::ID Lang;
		LintRule Rule;
		const char *Src;
		size_t Count;
		const char *Msg;
	};
	const Fixture Inputs[] {
		{
			Lang::Python,
			LintRule::UnreachableReturn,
			"def f():\n import sys\n try:\n  sys.exit()\n except (BaseException, sys.exit()):\n  pass\n return\n",
			1,
			"Violation: Remove the unreachable trailing `return`"
		},
		{
			Lang::Python,
			LintRule::UnreachableReturn,
			"def f():\n import sys\n try:\n  sys.exit()\n except BaseException:\n  pass\n return\n",
			0,
			""
		},
		{
			Lang::Java,
			LintRule::ComparisonBoundary,
			"class C{int x;boolean b=x\\u003e\\u003d0;}\n",
			1,
			"Review: Use a strict inequality with an equivalent adjusted boundary"
		},
		{ Lang::Java, LintRule::ComparisonBoundary, "class C{int x,bound;boolean b=x>=bound\\u002b\\u0032;}\n", 0, "" },
		{
			Lang::Java,
			LintRule::UnitAssignment,
			"class C{float x;void f(){x+=1\\u002e0f;}}\n",
			1,
			"Review: Use the increment or decrement form required for this target"
		},
		{ Lang::Java, LintRule::UnitAssignment, "class C{float x;void f(){x+=1\\u002e5f;}}\n", 0, "" },
		{
			Lang::C,
			LintRule::JumpUsage,
			"void f(int x){again:;while(x){while(x){goto again;}}}",
			1,
			"Review: Replace this jump with structured control flow"
		},
		{ Lang::Cpp, LintRule::JumpUsage, "void f(int x){while(x){while(x){goto next;}next:;}}", 0, "" },
		{
			Lang::TypeScript,
			LintRule::JumpUsage,
			"function f(x:boolean):void{a:while(x){if(x)break a;}a:while(x){if(x)break a;}}",
			2,
			"Review: Replace this jump with structured control flow"
		},
		{
			Lang::Cpp,
			LintRule::UsingNamespace,
			"using /* x */ namespace std;\n",
			1,
			"Review: Avoid `using namespace`; qualify names explicitly"
		},
		{ Lang::Cpp, LintRule::UsingNamespace, "using /* using namespace */ std::string;\nusing enum E;\n", 0, "" },
		{
			Lang::Cpp,
			LintRule::UnusedPostfix,
			"void F(){Thing V;V++;for(;;V--){}Consume(V++);}\n",
			2,
			"Review: Use prefix increment or decrement when the old value is unused"
		},
		{
			Lang::Go,
			LintRule::UnitAssignment,
			"package p\nfunc F(i int){i+=1;i-=(1);i+=2}\n",
			2,
			"Review: Use the increment or decrement form required for this target"
		},
		{ Lang::Cpp, LintRule::InferredType, "void F(){auto V=[]{};}\n", 0, "" },
		{
			Lang::Cpp,
			LintRule::BooleanNegation,
			"void F(){x=!x;x=x^true;x^=true;}\n",
			2,
			"Review: Use the boolean-negation assignment required for this target"
		},
		{ Lang::Cpp, LintRule::BooleanNegation, "void F(){o.x=!o.x;x=!y;}\n", 0, "" },
		{ Lang::Cpp, LintRule::MergeIf, "void F(){if(a)if(int b=Get();b)if(c)G();}\n", 0, "" },
		{
			Lang::Cpp,
			LintRule::MergeIf,
			"void F(){if(a)G();if(b)G();if(c)G();if(d)H();if(e)H();}\n",
			2,
			"Review: Merge the conditions of these identical branch bodies"
		},
		{
			Lang::Cpp,
			LintRule::MergeIf,
			"void F(){if(a)G();else if(b)G();else if(c)G();}\n",
			1,
			"Review: Merge the conditions of these identical branch bodies"
		},
		{
			Lang::JavaScript,
			LintRule::NullishComparison,
			"function f(x){return x==null||undefined===x||x===null;}\n",
			1,
			"Review: Combine the `null` and `undefined` checks"
		},
		{
			Lang::JavaScript,
			LintRule::NullishComparison,
			"function f(x,y){return x==null||x==undefined||work()||y==null||y==undefined;}\n",
			2,
			"Review: Combine the `null` and `undefined` checks"
		},
		{ Lang::JavaScript, LintRule::NullishComparison, "function f(o){return o.x==null||o.x==undefined;}\n", 0, "" },
		{
			Lang::Cpp,
			LintRule::ConstantMacro,
			"#define A ((-0x2U))\n#define B 2+3\n#define C Name\n#define D 1\n#if D\n#endif\n",
			1,
			"Review: Replace this constant macro with a language constant"
		},
		{ Lang::Cpp, LintRule::ConstantMacro, "#define A 1\n#ifdef A\n#endif\n", 0, "" },
		{ Lang::Python, LintRule::ParameterType, "class S:\n\tdef f(self,*args,**kwargs):pass\n", 3, "" },
		{
			Lang::Rust,
			LintRule::ParameterType,
			"impl S { fn f(self) {} fn g(self: Box<Self>) {} }\n",
			1,
			"Review: Add an explicit type to parameter 'self'"
		},
		{
			Lang::Ruby,
			LintRule::ZeroComparison,
			"consume(x==0)\nconsume(0==x)\nconsume(x==(0))\nconsume(x!=0)\nconsume(x>0)\n",
			3,
			"Review: Use the zero-test form required for this numeric type"
		},
		{
			Lang::Cpp,
			LintRule::ZeroComparison,
			"void F(){Consume(-1>0);Consume(0<-1);Consume(x!=0.0);}\n",
			1,
			"Review: Use the zero-test form required for this numeric type"
		},
		{ Lang::Cpp, LintRule::ZeroComparison, "void F(){int d=0;d+=Delta();Consume(d>0);Consume(0<d);}", 0, "" },
		{ Lang::Cpp, LintRule::ZeroComparison, "void F(){int d=-1;Consume(d>0);Consume(d!=0);}", 1, "" },
		{
			Lang::Cpp,
			LintRule::ComparisonBoundary,
			"void F(){Consume(x>=0);Consume(x<=60);Consume(x>=b+1);}\n",
			3,
			"Review: Use a strict inequality with an equivalent adjusted boundary"
		},
		{
			Lang::Kotlin,
			LintRule::ComparisonBoundary,
			"fun f(x:Int){consume(x>=-1);consume(x<=+1);consume(-1<=x);consume(+1>=x);consume(x>=(-1));consume((+1)<=x);}",
			6,
			""
		},
		{ Lang::Python, LintRule::ZeroComparison, "consume(x==0j)\nconsume(x>0j)\nconsume(x==0.0j)\nconsume(x>0E0j)\n", 4, "" },
		{ Lang::PHP, LintRule::MemberOrder, "<?php class C{function __Construct(){} function f():void{}}", 1, "" },
		{ Lang::PHP, LintRule::MemberOrder, "<?php class C{function f():void{} function __Construct(){}}", 0, "" },
		{ Lang::Rust, LintRule::JumpUsage, "fn f(flag:bool,xs:[i32;1])->(){'outer:{side();break 'outer;}}", 1, "" },
		{ Lang::TypeScript, LintRule::AccessOrder, "class C{public a:number=1;#b:number=2;}", 1, "" },
		{ Lang::TypeScript, LintRule::AccessOrder, "class C{#b:number=2; public a:number=1;}", 0, "" },
		{ Lang::Kotlin, LintRule::BooleanNegation, "fun f(){x=x /* name */ xor true;x=true xor /* value */ x}\n", 2, "" }
	};
	for(const Fixture &Input : Inputs) {
		size_t Count = 0;
		for(const LintWarning &Warning : AnalyzeDiagnosticCase(Lang::Get(Input.Lang), Input.Src)) {
			if(Warning.Rule != Input.Rule) continue;
			++Count;
			// 到達不能な返戻は違反，其の他の表の候補は要確認
			Check(
				Warning.Msg.starts_with(Input.Rule == LintRule::UnreachableReturn ? "Violation: " : "Review: "),
				(std::string("a candidate changed its category: ") + Input.Src).c_str()
			);
			if(*Input.Msg) Check(Warning.Msg == Input.Msg, "a review candidate changed its fixed message", Input.Src);
		}
		Check(
			Count == Input.Count,
			(
				std::string("a syntactic candidate differed from the hand expectation: rule=") +
				std::to_string(static_cast<size_t>(Input.Rule)) + " expected=" + std::to_string(Input.Count) + " actual=" +
				std::to_string(Count) + " input=" + Input.Src
			).c_str()
		);
	}
	// 隣接する比較の演算子の位置の手期待（診断用の内部座標は０始まり）
	struct BoundaryCase {
		const char *Lang;
		const char *Src;
		std::vector<uint32_t> Columns;
	};
	const BoundaryCase Boundaries[] {
		{ "java", "class C{int x;boolean b=x\\u003e\\u003d0;}\n", { 25 } },
		{ "python", "consume(len(a) < 60 <= b)\n", { 20 } },
		{ "python", "consume(len(a) <= 60 <= len(b) <= 70)\n", { 15, 21, 31 } },
		{ "python", "consume(a <= (b - 1) <= n)\n", { 10 } },
		{ "cpp", "void F(){use(a<=b-1);use(a>=b+1);use(b-1>=a);use(b+1<=a);}\n", { 14, 26, 40, 52 } },
		{ "python", "use(a<=b+1);use(a>=b-1)\n", {} },
		{ "scss", "a{v:$a<=($b-1);w:$a>=($b+1);x:($b-1)>=$a;y:($b+1)<=$a;z:$a<=10;}\n", {} },
		{ "cpp", "void F(){use(a <= (b /* bound */ - 1));}\n", { 15 } }
	};
	for(const BoundaryCase &Input : Boundaries) {
		std::vector<uint32_t> Columns;
		for(const LintWarning &Warning : AnalyzeDiagnosticCase(Lang::FromName(Input.Lang), Input.Src)) {
			if(Warning.Rule != LintRule::ComparisonBoundary) continue;
			Check(!Warning.Row, "a chained comparison lost its source row");
			Columns.push_back(Warning.Column);
		}
		std::string Actual;
		for(const uint32_t Column : Columns) Actual += " " + std::to_string(Column);
		Check(
			Columns == Input.Columns,
			(std::string("a chained boundary lost an adjacent literal or its operator position: columns=") + Actual + " input=" + Input.Src)
			.c_str()
		);
	}
	// 終了
	return;
}

/**
 * 選択した生成部品と既存の部分木の直接照合の検査関数
 * 計算量：固定テストケースの投影・選択・構造比較の総費用 Q に対し O(Q)
 * @throws std::runtime_error 許可した生成形式の拒否，トークン変更の受入又は期待構文の変更が有る場合
 * @throws FormatLimitExceeded 投影・生成又は照合の上限を超えた場合
 * @throws std::bad_alloc 投影・選択又は照合データの確保に失敗した場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckSelectedFormCmp() {
	// 選択した生成部品と既存の部分木の直接照合の検査の入力と手期待
	struct Fixture {
		::Lang::ID Lang;
		Core::Role Role;
		Core::FormOperation Operation;
		const char *Src;
		const char *Ref;
		const char *Changed;
	};
	const Fixture Inputs[] {
		{
			Lang::Python,
			Core::Role::BinaryExpr,
			Core::FormOperation::Continuation,
			"value = a + b\n",
			"value = (a + b)\n",
			"value = (a - b)\n"
		},
		{
			Lang::Python,
			Core::Role::BareTuple,
			Core::FormOperation::Continuation,
			"value = a, b, c\n",
			"value = (a, b, c)\n",
			"value = (a, b, d)\n"
		},
		{
			Lang::Python,
			Core::Role::TypeSyntax,
			Core::FormOperation::Continuation,
			"value: Outer[Inner[int], str]\n",
			"value: (Outer[Inner[int], str])\n",
			"value: (Outer[Inner[int], bytes])\n"
		},
		{
			Lang::JavaScript,
			Core::Role::ExprStmt,
			Core::FormOperation::BraceBody,
			"if (ready) Work();\n",
			"if (ready) { Work(); }\n",
			"if (ready) { Other(); }\n"
		},
		{
			Lang::Go,
			Core::Role::ArgSeq,
			Core::FormOperation::TrailingComma,
			"package main\nfunc Run() { Work(first, second) }\n",
			"package main\nfunc Run() { Work(first, second,) }\n",
			"package main\nfunc Run() { Work(first, other,) }\n"
		}
	};
	for(const Fixture &InputCase : Inputs) {
		const ::Lang Lang = Lang::Get(InputCase.Lang);
		Core::ProjectionResult Input = ProjectWithCtx(Lang, InputCase.Src);
		const std::vector<Core::NodeId> Nodes = GetNodesWithRole(Input.Syntax, InputCase.Role);
		Check(!Nodes.empty(), "a selected-form fixture lost its target role");
		const Core::NodeId Node = Nodes.front();
		const std::span<const Core::NodeId> Children = Input.Syntax.GetChildrenOf(Node);
		const std::vector<Core::NodeId> OriginalChildren(Children.begin(), Children.end());
		const size_t Count = Input.Syntax.GetNodeCount();
		Core::OutputExpectation Expectation;
		Expectation.NormalizedSyntax = std::make_shared<const Core::SyntaxModel>(std::move(Input.Syntax));
		Expectation.SelectedForms.push_back({ 1, { Node }, { { Node, InputCase.Operation } } });
		// 製品の検証用投影で，意味上の区別と選択済の構造対応を共に保持する
		const Core::ProjectionResult Candidate = ProjectDiagnosticCase(Lang, InputCase.Ref);
		Check(
			Candidate.Validation.SemanticChecks.Checks == Input.Validation.SemanticChecks.Checks,
			"verification projection lost independent semantic checks"
		);
		Check(SyntaxValidatorCmp::IsKept(Lang, Input, Candidate, Expectation), "direct comparison rejected a selected grammar form");
		Check(
			!SyntaxValidatorCmp::IsKept(Lang, Input, ProjectDiagnosticCase(Lang, InputCase.Changed), Expectation),
			"direct comparison accepted a changed child lexeme"
		);
		Check(
			Expectation.NormalizedSyntax->GetNodeCount() == Count &&
			std::ranges::equal(Expectation.NormalizedSyntax->GetChildrenOf(Node), OriginalChildren),
			"selected-form comparison mutated the shared expected structure"
		);
	}
	// 終了
	return;
}

/**
 * 表示前の警告の位置・構文順・置換値の整列の検査関数（整形仕様 8.4）
 * 計算量：固定件数の警告と置換値長 B に対し O(B)
 * @throws std::runtime_error 位置・識別子・構文順・整数又は Unicode の値の順が異なる場合
 * @throws std::bad_alloc テストケース又は整列の作業域の確保に失敗した場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
static void CheckWarningOrder() {
	// 順序を照合する警告のテストケースの構築
	std::vector<LintWarning> Warnings {
		{ 0, 0, "ten", LintRule::DocumentationParameter, 7, { int64_t{ 10 } } },
		{ 0, 0, "unicode", LintRule::DocumentationParameter, 8, { "é" } },
		{ 0, 0, "first notice", LintRule::Other },
		{ 1, 0, "later row", LintRule::NestingDepth },
		{ 0, 0, "two", LintRule::DocumentationParameter, 7, { int64_t{ 2 } } },
		{ 0, 0, "later notice", LintRule::Other },
		{ 0, 0, "ascii", LintRule::DocumentationParameter, 8, { "z" } },
		{ 0, 0, "earlier target", LintRule::DocumentationParameter, 6, { "z" } },
		{ 0, 0, "earlier rule", LintRule::NestingDepth }
	};
	std::stable_sort(Warnings.begin(), Warnings.end());
	const std::string_view Expected[] {
		"earlier rule",
		"earlier target",
		"two",
		"ten",
		"ascii",
		"unicode",
		"first notice",
		"later notice",
		"later row"
	};
	for(size_t Idx = 0; Idx < Warnings.size(); ++Idx) {
		Check(Warnings[Idx].Msg == Expected[Idx], "warning order used rendered messages or the wrong syntax order");
	}
	// 終了
	return;
}

/**
 * 行差分と作業域の見積の独立した検査関数
 * 計算量：テストケース数 C と最大行数 L に対し O(CL²)
 * @throws std::runtime_error 最短編集数又は行の対応が期待と異なる場合
 * @throws std::bad_alloc テストケース・比較表又は差分の領域を確保出来ない場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
static void CheckLineDiffAndMemory() {
	// 共通行が無い列と，一致が１行だけの列の手期待（全変更行の削除と追加）
	for(const bool HasCommon : { false, true }) {
		std::string Original, Candidate;
		for(size_t Idx = 0; Idx < 4000; ++Idx) {
			if(HasCommon && Idx == 2000) {
				Original += "common\n";
				Candidate += "common\n";
			} else {
				Original += "old" + std::to_string(Idx) + "\n";
				Candidate += "new" + std::to_string(Idx) + "\n";
			}
		}
		const std::vector<LineChange> Changes = CompareLines(Original, Candidate);
		Check(Changes.size() == (HasCommon ? 7998 : 8000), "sparse LCS retained a changed line");
		for(const LineChange &Change : Changes) {
			Check(Change.Text != "common\n", "sparse LCS lost its only common line");
			Check(
				Change.Text == (Change.IsInsertion ? "new" : "old") + std::to_string(Change.Line) + "\n",
				"sparse LCS changed a source row"
			);
		}
	}
	// 多数の重複一致の列でも，探索を打ち切った差分が双方の行の対応を失わない
	{
		std::vector<std::string_view> Old(8000), New(8000);
		std::string Original, Candidate;
		for(size_t Idx = 0; Idx < Old.size(); ++Idx) {
			Old[Idx] = Idx < 4000 ? "a\n" : "b\n";
			New[Idx] = Idx < 4000 ? "b\n" : "a\n";
			Original += Old[Idx];
			Candidate += New[Idx];
		}
		std::vector<bool> Removed(Old.size()), Added(New.size());
		for(const LineChange &Change : CompareLines(Original, Candidate)) {
			if(Change.IsInsertion) {
				Check(Change.Line < New.size() && !Added[Change.Line] && Change.Text == New[Change.Line], "dense insertion differs");
				Added[Change.Line] = true;
			} else {
				Check(Change.Line < Old.size() && !Removed[Change.Line] && Change.Text == Old[Change.Line], "dense deletion differs");
				Removed[Change.Line] = true;
			}
		}
		std::vector<std::string_view> OldCommon, NewCommon;
		for(size_t Idx = 0; Idx < Old.size(); ++Idx) if(!Removed[Idx]) OldCommon.push_back(Old[Idx]);
		for(size_t Idx = 0; Idx < New.size(); ++Idx) if(!Added[Idx]) NewCommon.push_back(New[Idx]);
		Check(OldCommon == NewCommon, "bounded dense diff changed a retained line");
	}
	// 属性はコードとして数え，実際のコメントと補間の無い引用だけを領域へ畳む（整形仕様 9.1）
	struct AttributeMemoryCase {
		std::string_view Src; // 原文
		size_t Units; // 領域外の語・記号
		size_t Regions; // 確定したコメント・引用
		size_t Lines; // 原文の LF
		::Lang Lang = ::Lang::Get(::Lang::PHP); // 入口の候補文法
	};
	for(
		const AttributeMemoryCase &Case :
		{
			AttributeMemoryCase{ "<?php\n#[A]\nfunction f() {}\n", 13, 0, 3 },
			AttributeMemoryCase{ "<?php\n#[A] # c\nclass X {}\n", 11, 1, 3 },
			AttributeMemoryCase{ "<?php\n#[A(\"#\")] class X {}\n", 13, 1, 2 },
			AttributeMemoryCase{ "<?php\n#[A(\"$x\")] class X {}\n", 16, 0, 2 },
			AttributeMemoryCase{ "<?php $s = \"{$a[#[A] function(){}]}\"; \"x\";", 23, 1, 0 },
			AttributeMemoryCase{ "# tail\n", 2, 0, 1 },
			AttributeMemoryCase{ "<?php # tail\n", 3, 1, 1 },
			AttributeMemoryCase{ "<?php ?> \"x\"\n", 8, 0, 1 },
			AttributeMemoryCase{ "<?php __halt_compiler(); # tail\n", 9, 0, 1 },
			AttributeMemoryCase{ "x=1\n__END__\n# tail\n", 6, 0, 3, Lang::Get(Lang::Ruby) },
			AttributeMemoryCase{ "<p title=\"1;2\">\"x\"</p>\n", 12, 1, 1, Lang::Get(Lang::HTML) },
			AttributeMemoryCase{ "<!-- x -->\n", 0, 1, 1, Lang::Get(Lang::HTML) },
			AttributeMemoryCase{ "const x=<p>\"1;2\"</p>;\n", 16, 0, 1, Lang::FromName("javascriptreact") },
			AttributeMemoryCase{ "const x=<p a=\"1;2\"/>;\n", 10, 1, 1, Lang::FromName("javascriptreact") },
			AttributeMemoryCase{ "#include <a/*b*/c>\n", 11, 0, 1, Lang::Get(Lang::C) },
			AttributeMemoryCase{ "a{background:url(a/*b*/c)}\n", 15, 0, 1, Lang::Get(Lang::CSS) },
			AttributeMemoryCase{ "a{background:url(\"a/*b*/c\")}\n", 8, 1, 1, Lang::Get(Lang::CSS) }
		}
	) {
		const size_t Expected = MaxInputBytes + (Case.Src.size() << 4) + Case.Units * 0X500 + (Case.Regions << 11) + Case.Lines * 0X30;
		const GrammarProfile &Profile = GrammarProfile::Resolve(Case.Lang);
		Check(EstimateWorkingBytes(Profile, Case.Src) == Expected, "attribute code disappeared from memory admission");
		const auto Read = [&](const size_t Position, const std::span<char> Buffer) -> size_t {
			const size_t Len = std::min(Buffer.size(), Case.Src.size() - Position);
			Case.Src.copy(Buffer.data(), Len, Position);
			// 読取バイト数の返戻
			return Len;
		};
		Check(EstimateWorkingBytes(Profile, Case.Src.size(), Read) == Expected, "descriptor admission lost attribute code");
	}
	// 逐語要素内のコメント風の本文は領域へ畳まず，前後のタグも通常の語・記号へ数える
	for(const std::string_view Name : { "textarea" }) {
		const std::string Src = "<" + std::string(Name) + "><!-- x --></" + std::string(Name) + ">\n";
		Check(
			EstimateWorkingBytes(GrammarProfile::Resolve(Lang::Get(Lang::HTML)), Src) ==
			MaxInputBytes + (Src.size() << 4) + 15 * 0X500 + 0X30,
			"raw element text became a comment region"
		);
	}
	for(const Lang ProfileLang : { Lang::Get(Lang::Python) }) {
		Check(
			EstimateWorkingBytes(GrammarProfile::Resolve(ProfileLang), "#[A]\nx=1\n") == MaxInputBytes + 9 * 16 + 3 * 1280 + 2048 + 2 * 48,
			"an ordinary hash comment became attribute code"
		);
	}
	const GrammarProfile &JsonMemory = GrammarProfile::Resolve(Lang::Get(Lang::JSON));
	const GrammarProfile &CodeMemory = GrammarProfile::Resolve(Lang::Get(Lang::C));
	Check(
		EstimateWorkingBytes(JsonMemory, "[1,1]\n") == MaxInputBytes + 6 * 16 + 5 * 1280 + 48,
		"memory estimate changed its lexical unit"
	);
	// 解析前の受理見積は逐語データも含む原文全体で求める（整形仕様 9.1，`B = 16, T = 7, C = 0, L = 3`）
	Check(
		EstimateWorkingBytes(GrammarProfile::Resolve(Lang::Get(Lang::Ruby)), std::string_view("x=1\n__END__\na\0b\n", 16)) ==
		MaxInputBytes + (16 << 4) + 7 * 0X500 + 3 * 0X30,
		"trailing data disappeared from the raw memory estimate"
	);
	std::string Dense;
	for(size_t Idx = 0; Idx < 300000; ++Idx) Dense += "1,";
	Check(EstimateWorkingBytes(JsonMemory, Dense) > MaxWorkingBytes, "dense input bypassed memory admission");
	Check(EstimateWorkingBytes(CodeMemory, std::string(0X100000, 'a')) < MaxWorkingBytes, "sparse input was measured only by size");
	// １６倍の本文と１２８０倍のトークンから，受理上限の直前・一致・直後を手で求める
	const std::string Boundary = std::string(401306, ';') + std::string(70, ' ');
	Check(
		EstimateWorkingBytes(CodeMemory, Boundary.substr(0, Boundary.size() - 1)) == MaxWorkingBytes - 16,
		"working-memory boundary before admission differs"
	);
	Check(EstimateWorkingBytes(CodeMemory, Boundary) == MaxWorkingBytes, "exact working-memory boundary was rejected");
	Check(
		EstimateWorkingBytes(CodeMemory, Boundary + " ") == MaxWorkingBytes + 1,
		"working-memory boundary did not saturate after admission"
	);
	Check(
		EstimateWorkingBytes(CodeMemory, "/* NOLINTNEXTLINE */ ") == MaxInputBytes + 21 * 16 + 2048,
		"comment payload was charged as syntax"
	);
	Check(EstimateWorkingBytes(CodeMemory, "\"/* */\"") == MaxInputBytes + 7 * 16 + 2048, "literal opened a comment");
	Check(EstimateWorkingBytes(CodeMemory, "/* { ;") == MaxInputBytes + (6 << 4) + (1280 << 2), "unclosed comment hid code units");
	// 未確定の領域は畳まず，確定した大量の本文は保留計数だけで拒まない（整形仕様 9.1）
	const std::string Comment = "/*" + std::string(500000, ';') + "*/\n";
	Check(
		EstimateWorkingBytes(CodeMemory, Comment) == MaxInputBytes + (Comment.size() << 4) + 2048 + 48,
		"pending comment units caused an early rejection"
	);
	Check(
		EstimateWorkingBytes(CodeMemory, Comment.substr(0, Comment.size() - 3)) == MaxWorkingBytes + 1,
		"unclosed comment did not saturate its lexical estimate"
	);
	// 除算の後でも，閉コメントの内部計数を保留値から取り除く（整形仕様 9.1）
	for(const Lang::ID Lang : { Lang::JavaScript }) {
		const GrammarProfile &Profile = GrammarProfile::Resolve(Lang::Get(Lang));
		const std::string Decl = "const x = 8 / 2;\n";
		const std::string Block = "/* " + std::string(420000, ';') + " */\n";
		const size_t Expected = MaxInputBytes + (Decl.size() + Block.size() << 4) + 7 * 1280 + 2048 + (48 << 1);
		Check(EstimateWorkingBytes(Profile, Decl + Block) == Expected, "division prevented a following comment from collapsing");
		Check(EstimateWorkingBytes(Profile, Block + Decl) == Expected, "comment order changed admission counts");
	}
	// 正規表現の未確定範囲は T に保ち，井桁の確定領域と候補の終端の後の閉コメントを C にする
	struct SlashFixture {
		::Lang::ID Lang;
		const char *Src;
		size_t Units;
		size_t Regions;
		size_t Lines;
	};
	const SlashFixture SlashInputs[] {
		{ Lang::JavaScript, "const x = f() / 2;\n/* ; */\n", 9, 1, 2 },
		{ Lang::JavaScript, "const r = /[/*]/;\n/* ; */\n", 10, 1, 2 },
		{ Lang::Swift, "let r = #/[/*]/#;\n/* ; */\n", 4, 2, 2 },
		{ Lang::Swift, "let r = #/\\#(x)/#;\n/* ; */\n", 13, 1, 2 },
		{ Lang::C, "/\\\n/---\n", 0, 1, 2 },
		{ Lang::Cpp, "/\\\n*---\n", 6, 0, 2 },
		{ Lang::Java, "class C {}\\u000a//---\n", 6, 1, 1 },
		{ Lang::Java, "\\u0022//---", 7, 0, 0 },
		{ Lang::Java, "\\u0022//---\\u0022 //---\n", 0, 2, 1 },
		{ Lang::Java, "//\\u00G0\n//---\n", 9, 0, 2 },
		{ Lang::Ruby, "x = <<'DOC'\n---\nDOC\n", 2, 1, 3 },
		{ Lang::CSharp, "var s = $\"{\"a\" + (1+1) + \"b\"}\";\n//---\n", 22, 1, 2 },
		{ Lang::CSharp, "var s = \"{text}\";\n//---\n", 4, 2, 2 },
		{ Lang::CSharp, "var s = $\"{\"a\" + (1+1) + \"b\"\n//---\n", 24, 0, 2 }
	};
	for(const SlashFixture &Input : SlashInputs) {
		const GrammarProfile &Profile = GrammarProfile::Resolve(Lang::Get(Input.Lang));
		const std::string_view Src = Input.Src;
		const size_t Expected = MaxInputBytes + (Src.size() << 4) + Input.Units * 0X500 + (Input.Regions << 11) + Input.Lines * 0X30;
		Check(EstimateWorkingBytes(Profile, Src) == Expected, "slash context changed a confirmed region or hid uncertain units");
		// ソースと記述子の入口は，同じ領域・語・記号・改行を計数する
		const auto Read = [Src](const size_t Offset, const std::span<char> Buf) -> size_t {
			const size_t Len = std::min(Buf.size(), Src.size() - Offset);
			std::copy_n(Src.begin() + static_cast<std::ptrdiff_t>(Offset), Len, Buf.begin());
			// 同じ原文からコピーした量の返戻
			return Len;
		};
		Check(EstimateWorkingBytes(Profile, Src.size(), Read) == Expected, "descriptor admission changed slash context");
	}
	// 固定長の三重引用符の直後の引用符は本文で，C# の連続数の区切とは区別する
	for(const Lang::ID Lang : { Lang::Python }) {
		const GrammarProfile &Profile = GrammarProfile::Resolve(Lang::Get(Lang));
		const std::string Src = "x = \"\"\"\"" + std::string(410000, '.') + "\"\"\"\n";
		Check(
			EstimateWorkingBytes(Profile, Src) == MaxInputBytes + (Src.size() << 4) + (1280 << 1) + 2048 + 48,
			"fixed triple delimiter consumed the first payload quote"
		);
	}
	const GrammarProfile &CSharpMemory = GrammarProfile::Resolve(Lang::Get(Lang::CSharp));
	// `B = 2N + 30`，`T = 2N + 20`，`C = 0`，`L = 1` から上限の直前・一致・直後を手で求める
	std::string InterpolatedBoundary = "var s = $\"{\"a\" + (";
	for(size_t Idx = 0; Idx < 200643; ++Idx) InterpolatedBoundary += "1+";
	InterpolatedBoundary += "1) + \"b\"}\";\n";
	InterpolatedBoundary.append(57, ' '); // 残り９１２バイトの見積を１６倍の空白で埋める
	Check(
		EstimateWorkingBytes(CSharpMemory, std::string_view(InterpolatedBoundary).substr(0, InterpolatedBoundary.size() - 1)) ==
		MaxWorkingBytes - 16,
		"interpolation estimate before the admission boundary differs"
	);
	const auto EstimateInterpolation = [&CSharpMemory, &InterpolatedBoundary]() -> size_t {
		// 同じ受理走査の見積の返戻
		return EstimateWorkingBytes(CSharpMemory, InterpolatedBoundary);
	};
	Check(
		EstimateInterpolation() == MaxWorkingBytes &&
		AcceptInput(InterpolatedBoundary.size(), EstimateInterpolation) == FailureReason::None,
		"interpolation at the admission boundary was rejected"
	);
	InterpolatedBoundary += ' ';
	Check(
		EstimateInterpolation() == MaxWorkingBytes + 1 &&
		AcceptInput(InterpolatedBoundary.size(), EstimateInterpolation) == FailureReason::WorkingMemoryExceeded,
		"interpolation above the admission boundary bypassed resource rejection"
	);
	Check(
		EstimateWorkingBytes(CSharpMemory, "x = \"\"\"\"text\"\"\"\"\n") == MaxInputBytes + (17 << 4) + (1280 << 1) + 2048 + 48,
		"variable raw delimiter lost its opening quote count"
	);
	Check(
		EstimateWorkingBytes(CSharpMemory, "x = \"\"\"\"text\"\"\"\n") == MaxInputBytes + (16 << 4) + 10 * 1280 + 48,
		"unclosed variable raw delimiter collapsed an uncertain region"
	);
	const GrammarProfile &Header = GrammarProfile::Resolve(Lang::FromName("h"));
	Check(
		EstimateWorkingBytes(Header, "R\"(a b)\"") == MaxInputBytes + (8 << 4) + 7 * 1280,
		"ambiguous header collapsed a C++-only raw literal"
	);
	const GrammarProfile &Java = GrammarProfile::Resolve(Lang::Get(Lang::Java));
	Check(
		EstimateWorkingBytes(Java, "/*a*/ \\u0041") == MaxInputBytes + (12 << 4) + (1280 << 1) + 2048,
		"Unicode prelexing was counted in a separate or optimistic scan"
	);
	// 無害な Unicode 展開は後続の確定したコメントを未確定にしない
	const std::string EscapedBefore = "class A { int \\u0078=1; /*" + std::string(410000, ';') + "*/ }\n";
	const std::string EscapedAfter = "class A { /*" + std::string(410000, ';') + "*/ int \\u0078=1; }\n";
	// 原文の語と記号の計数は T=10, C=1, L=1（エスケープのバックスラッシュも原文の記号１バイト）
	const size_t JavaEstimate = MaxInputBytes + (EscapedBefore.size() << 4) + 10 * 1280 + 2048 + 48;
	Check(EstimateWorkingBytes(Java, EscapedBefore) == JavaEstimate, "an earlier escape prevented comment admission");
	Check(EstimateWorkingBytes(Java, EscapedAfter) == JavaEstimate, "escape position changed confirmed region counts");
	Check(
		EstimateWorkingBytes(Java, "/*\\u0078 ;*/") == MaxInputBytes + (12 << 4) + 2048,
		"a harmless expansion inside a comment made its boundary uncertain"
	);
	Check(
		EstimateWorkingBytes(Java, "\\u0022 /* ; */") > MaxInputBytes + (14 << 4) + 2048,
		"an escaped quote allowed an uncertain comment boundary to collapse"
	);
	Check(
		EstimateWorkingBytes(Java, "\"\\\\u0022\"") == MaxInputBytes + (9 << 4) + 2048,
		"an ineligible escaped backslash changed a confirmed literal boundary"
	);
	Check(
		EstimateWorkingBytes(Java, "//\\u") == MaxInputBytes + (4 << 4) + (1280 << 2),
		"an incomplete Unicode escape was hidden inside a confirmed line comment"
	);
	// 固定長の読出口と既存のソースが同じ T, C, L を求める
	for(const size_t Unit : { size_t{ 1 }, size_t{ 7 }, size_t{ 64 }, size_t{ 65536 } }) {
		// 記述子上の原文の読取
		const auto Read = [&Comment, Unit](const size_t Offset, const std::span<char> Buf) -> size_t {
			// 残余量と領域容量からの読込単位の決定
			const size_t Len = std::min({ Unit, Buf.size(), Comment.size() - Offset });
			std::copy_n(Comment.begin() + static_cast<std::ptrdiff_t>(Offset), Len, Buf.begin());
			// 既存のソースからコピーした量の返戻
			return Len;
		};
		Check(
			EstimateWorkingBytes(CodeMemory, Comment.size(), Read) == MaxInputBytes + (Comment.size() << 4) + 2048 + 48,
			"input transport unit changed admission counts"
		);
	}
	// 実読取量は読出単位と独立し，上限一致の EOF と更なる読出を分ける（整形仕様 9.3）
	const std::string Src(65, 'x');
	for(const size_t Unit : { size_t{ 1 }, size_t{ 7 }, size_t{ 63 }, size_t{ 64 } }) {
		ParserReadBudget Budget { 64 };
		for(size_t Pos = 0; Pos < 64;) {
			const std::string_view Chunk = Budget.Take(std::string_view(Src).substr(Pos, std::min(Unit, 64 - Pos)), 0);
			Check(!Chunk.empty() && Chunk.size() < 65, "parser read exceeded the fixed chunk size");
			Pos += Chunk.size();
		}
		Check(!Budget.Remaining && !Budget.IsCancelled, "exact parser budget caused an early interruption");
		Check(Budget.Take(Src, Src.size()).empty() && !Budget.IsCancelled, "EOF was counted as another read");
		Check(Budget.Take(Src, 64).empty() && Budget.IsCancelled, "read after budget exhaustion was not interrupted");
	}
	SyntaxParser Parser;
	Check(Parser.CanSetGrammar(Lang::Get(Lang::JSON).GetTreeSitterLang()), "parser budget fixture could not initialize");
	bool IsExpired = false;
	ParserReadBudget EmptyBudget { 0 };
	TSTree *const Interrupted = Parser.Parse(nullptr, "{broken", IsExpired, &EmptyBudget);
	Check(!Interrupted && EmptyBudget.IsCancelled && !IsExpired, "read cancellation became a syntax error or timeout");
	ParserReadBudget Available { 1024 };
	TSTree *const Broken = Parser.Parse(nullptr, "{broken", IsExpired, &Available);
	Check(
		Broken && ts_node_has_error(ts_tree_root_node(Broken)) && !Available.IsCancelled,
		"ordinary input error became budget exhaustion"
	);
	ts_tree_delete(Broken);
	// 完成候補も独立解析前に元の入口条件で受理し，原予約を変更しない
	{
		ValidationContext Ctx;
		Ctx.EntryProfile = &JsonMemory;
		const Core::OutputExpectation Expectation;
		const Core::CommentRepository Comments;
		const SyntaxValidator::ValidationResult Rejected =
		SyntaxValidator().Validate(JsonMemory, nullptr, Dense, Core::OutputExpectation(Expectation), Comments, std::move(Ctx));
		Check(
			Rejected.GetStatus() == SyntaxValidator::ValidationStatus::Failed &&
			Rejected.GetReason() == FailureReason::WorkingMemoryExceeded,
			"dense candidate bypassed admission before validation"
		);
	}
	// 終了
	return;
}

/**
 * 深い所属から外側の同じ値を照会する実入力の作業量と製品の予算の検査関数
 * 計算量：入力長 B，実際の名前の解決費用 Q に対し O(B + Q)
 * @param Profile キャストの曖昧さを名前で解決する文法
 * @param ClassDepth 入子クラスの数
 * @param CastCnt 外側の値の使用数
 * @return 入力側の名前の作業単位
 * @throws FormatLimitExceeded 入力又は投影が処理予算を超える場合
 * @throws std::bad_alloc 入力又は投影のメモリを確保出来ない場合
 * @throws std::length_error 入力又は投影の要素数上限を超える場合
 * @throws std::runtime_error 祖先の再探索又は予算の超過を検出した場合
 */
static size_t CheckDeepNameWork(const GrammarProfile &Profile, const size_t ClassDepth, const size_t CastCnt) {
	std::string Text = "class C{const int d=1;";
	for(size_t Idx = 0; Idx < ClassDepth; ++Idx) Text += "class N" + std::to_string(Idx) + "{";
	Text += "int F(int a){";
	for(size_t Idx = 0; Idx < CastCnt; ++Idx) Text += "var r" + std::to_string(Idx) + "=(d)-a;";
	Text += "return a;}";
	Text.append(ClassDepth + 1, '}');
	InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, Text);
	ParseInput Input = ParseAdapter::Adapt(Profile, Normalized);
	const ParsedTree Src = SyntaxParser::Parse(*Input.Profile, Input);
	const size_t Work = Src.GetNameWork();
	Check(Work && Work < (ClassDepth << 5) + (CastCnt << 5), "deep binding lookup repeated the ancestor search");
	const Core::ProjectionResult Projection = Core::SourceProjector::Project(Src, *Input.Profile, Input);
	Check(
		Projection.Syntax.GetWorkAccount()->Used <= Projection.Syntax.GetWorkLimit(),
		"shared deep input work exceeded the product budget"
	);
	std::printf("deep name scopes: %zu levels, %zu uses, %zu work\n", ClassDepth, CastCnt, Work);
	// 製品の名前の解決で計上した作業量の返戻
	return Work;
}

/**
 * 倍々の規模で区間探索と共有する導入関係の作業量を計測する検査関数
 * 入力長 B，宣言数 N に対し O(B + N log(N + 1)) を検査する
 * @throws std::bad_alloc テスト又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error テストの文字列の要素数上限を超える場合
 * @throws std::runtime_error 名前の解決又は作業量の上限が期待と異なる場合
 */
static void CheckNameScopesScale() {
	const GrammarProfile &Profile = GrammarProfile::Resolve(Lang::Get(Lang::Cpp));
	// 照会して居ない名前の位置を作らず，照会した名前だけを同じ表で共有する
	const Core::SyntaxModel PositionSyntax = Project(Lang::Get(Lang::Cpp), "int spare;int selected;");
	const Core::ControlFlow PositionFlow(PositionSyntax);
	const Core::NodeId PositionScope = PositionSyntax.GetRoot();
	Check(
		Core::SyntaxNormalizerAccess::GetCallablePositionDemand(PositionFlow, PositionScope, std::string_view()) ==
		std::pair<size_t, size_t>(2, 0),
		"a scope-only name query eagerly built declaration positions"
	);
	Check(
		Core::SyntaxNormalizerAccess::GetCallablePositionDemand(PositionFlow, PositionScope, "selected") ==
		std::pair<size_t, size_t>(2, 1),
		"a selected name query built positions for unrelated names"
	);
	Check(
		Core::SyntaxNormalizerAccess::GetCallablePositionDemand(PositionFlow, PositionScope, "selected") ==
		std::pair<size_t, size_t>(2, 1),
		"a repeated name query did not share its declaration positions"
	);
	size_t PreviousChildQueries = 0;
	// 規模の倍増順は作業量の増加率を比較する順である
	for(const size_t Cnt : { 64, 128, 256, 512, 1024 }) {
		std::string Closed = "using T=int;void F(){";
		for(size_t Idx = 0; Idx < Cnt; ++Idx) Closed += "{int T;}";
		Closed += "T *p;}";
		ParsedTree Parsed(Profile, Closed);
		Check(!Parsed.HasCollectedNames() && !Parsed.GetRelationWork(), "unused tree eagerly collected names or relations");
		const TSNode Root = Parsed.GetRoot();
		// 候補でないノードの拒否は，入力規模に依らず親子表を構築しない
		Check(!Parsed.ReadParenthesizedDecl(Root), "a translation unit became a declaration");
		Check(!Parsed.GetRelationWork(), "rejected declarations eagerly constructed relations");
		const std::span<const ParsedChild> Children = Parsed.GetChildrenOf(Root);
		Check(Children.size() == 2, "cached root lost the type alias or function");
		Check(
			ts_node_eq(Children[1].Node, ts_node_child(Root, 1)) && Children[1].Symbol == ts_node_symbol(Children[1].Node),
			"cached child lost its node or symbol"
		);
		const std::span<const ParsedChild> FunctionChildren = Parsed.GetChildrenOf(Children[1].Node);
		Check(
			FunctionChildren.size() == 3 && Profile.GetChildRoleOf(FunctionChildren.back().Field) == Core::ChildRole::Body,
			"cached child lost its body field"
		);
		ParsedTreeAccess::CheckFieldReads(Parsed, Children[1].Node, FunctionChildren.back().Node);
		const size_t RelationWork = Parsed.GetRelationWork();
		Check(RelationWork < (Cnt << 4) + 64, "shared child collection exceeded linear visits");
		Check(!Parsed.HasCollectedNames(), "parent and child queries eagerly collected names");
		// 最初の型の照会だけで束縛を収集する
		Check(
			Parsed.IsDeclaredTypeName("T", static_cast<uint32_t>(Parsed.find("T *p") + 1)) && Parsed.HasCollectedNames(),
			"lazy collection lost the outer type"
		);
		const uint32_t Use = static_cast<uint32_t>(Parsed.find("T *p") + 1);
		const std::pair<size_t, size_t> Visits = ParsedTreeAccess::GetBindingVisits(Parsed);
		Check(Visits.first && Visits.first <= RelationWork && !Visits.second, "declaration collection repeated or added a tree scan");
		const std::array<size_t, 5> Metrics = ParsedTreeAccess::GetAdmissionMetrics(Parsed);
		Check(
			Metrics[2] && Metrics[3] && Metrics[2] + Metrics[3] <= Visits.first + 1,
			"declaration collection queried recorded children for leaves"
		);
		Check(!PreviousChildQueries || Metrics[2] <= 3 * PreviousChildQueries, "declaration child queries exceeded linear growth");
		PreviousChildQueries = Metrics[2];
		const size_t Before = Parsed.GetNameWork();
		for(size_t Idx = 0; Idx < Cnt; ++Idx) {
			Check(Parsed.IsDeclaredTypeName("T", Use), "expired local hid T");
			Check(Parsed.GetChildrenOf(Root).data() == Children.data(), "cached children were copied or rebuilt");
		}
		Check(Parsed.GetRelationWork() == RelationWork, "child queries repeated the tree walk");
		const size_t ClosedWork = Parsed.GetNameWork() - Before;
		// 終了済の区間を１つずつ読む線形探索の退行を検出する
		size_t Levels = 1;
		for(size_t Size = Cnt; Size >>= 1;) ++Levels;
		Check(ClosedWork < Cnt * ((Levels << 3) + 16), "name interval lookup exceeded logarithmic work");
		// 導入名を持たない長い式の部分木を，宣言収集で辿り直さない
		std::string LongVal = "using T=int;int F(){int x=0;return x";
		for(size_t Idx = 0; Idx < Cnt; ++Idx) LongVal += "+1";
		LongVal += ";}";
		ParsedTree WithLongVal(Profile, LongVal);
		WithLongVal.CollectDeclaredNames();
		const uint32_t ReturnUse = static_cast<uint32_t>(WithLongVal.find("return x") + 7);
		Check(
			WithLongVal.IsDeclaredName("x", ReturnUse) && WithLongVal.IsDeclaredTypeName("T", ReturnUse),
			"name pruning lost a declaration visible inside a skipped value"
		);
		Check(
			ParsedTreeAccess::GetBindingVisits(WithLongVal).first < 64 && WithLongVal.GetRelationWork() > Cnt,
			"name collection descended into a value without declarations"
		);
		std::string Imported = "#define FLAG 1\n";
		for(size_t Idx = 0; Idx < Cnt; ++Idx) {
			const std::string Suffix = std::to_string(Idx);
			Imported += "namespace N" + Suffix + "{";
			if(Idx) Imported += "using namespace N" + std::to_string(Idx - 1) + ";";
			Imported += "using T" + Suffix + "=int;}";
		}
		for(size_t Idx = 0; Idx < Cnt; ++Idx) Imported += "using namespace N" + std::to_string(Idx) + ";";
		Imported += "void F(){}";
		Parsed = ParsedTree(Profile, Imported);
		Parsed.CollectDeclaredNames();
		const std::pair<size_t, size_t> ImportedVisits = ParsedTreeAccess::GetBindingVisits(Parsed);
		Check(ImportedVisits.first <= Parsed.GetRelationWork(), "imported declarations repeated a tree scan");
		Check(ImportedVisits.second == 2, "macro state collection descended into unrelated code");
		const size_t ImportsBefore = Parsed.GetNameWork();
		// 収集中の名前空間の解決でも，導入先全体を繰り返し走査しない
		Check(ImportsBefore < Cnt * ((Levels << 5) + 64), "name collection repeatedly scanned imports");
		for(size_t Idx = 0; Idx < Cnt; ++Idx) {
			Check(
				Parsed.IsDeclaredTypeName("T" + std::to_string(Idx), static_cast<uint32_t>(Imported.size() - 2)),
				"imported type was not resolved"
			);
		}
		const size_t ImportWork = Parsed.GetNameWork() - ImportsBefore;
		// 名前毎に全ての導入先を走査する２乗の退行を検出する
		Check(ImportWork < Cnt * ((Levels << 3) + 32), "name imports were rescanned for every name");
		// 導入済の所属から次の導入先を解決する，成長中の経路の退行を検出する
		std::string Growing = "namespace A{";
		for(size_t Idx = 0; Idx < Cnt; ++Idx) {
			const std::string Suffix = std::to_string(Idx);
			Growing += "namespace B" + Suffix + "{using T" + Suffix + "=int;}";
		}
		Growing += "}using namespace A;";
		for(size_t Idx = 0; Idx < Cnt; ++Idx) Growing += "using namespace B" + std::to_string(Idx) + ";";
		Growing += "void F(){}";
		Parsed = ParsedTree(Profile, Growing);
		Parsed.CollectDeclaredNames();
		Check(Parsed.GetNameWork() < Cnt * ((Levels << 5) + 64), "each additional import rebuilt all earlier routes");
		for(size_t Idx = 0; Idx < Cnt; ++Idx) {
			Check(
				Parsed.IsDeclaredTypeName("T" + std::to_string(Idx), static_cast<uint32_t>(Growing.size() - 2)),
				"an incrementally imported type was lost"
			);
		}
		std::printf(
			"name scopes: %zu types, %zu interval steps, %zu import steps, %zu collection steps\n",
			Cnt,
			ClosedWork,
			ImportWork,
			ImportsBefore
		);
	}
	// 同じ外側の値の照会は，入子の深さと使用数の積でなく未解決の所属と使用毎の参照だけを数える
	const GrammarProfile &CSharp = GrammarProfile::Resolve(Lang::Get(Lang::CSharp));
	size_t PreviousDeepWork = 0, SharedDeepWork = 0;
	for(const size_t ClassDepth : { 125U, 250U, 500U, 1000U, 2000U, 4000U }) {
		const size_t Work = CheckDeepNameWork(CSharp, ClassDepth, 512);
		Check(!PreviousDeepWork || Work < 3 * PreviousDeepWork, "doubling nesting exceeded linear name-resolution growth");
		if(ClassDepth == 1000) SharedDeepWork = Work;
		PreviousDeepWork = Work;
	}
	PreviousDeepWork = 0;
	for(const size_t CastCnt : { 64U, 128U, 256U, 512U, 1024U, 2048U }) {
		const size_t Work = CastCnt == 512 ? SharedDeepWork : CheckDeepNameWork(CSharp, 1000, CastCnt);
		Check(!PreviousDeepWork || Work < 3 * PreviousDeepWork, "doubling uses repeated the ancestor search");
		PreviousDeepWork = Work;
	}
	// 型・値・マクロと間接の名前空間の導入を同じ共有木から照会
	const ParsedTree Shared(
		Profile,
		"#define KEEP(x) #x\nnamespace A{using T=int;int V;}namespace B{using namespace A;}" "using namespace B;void F(){T *p;V;}"
	);
	ParsedTreeAccess::CheckParallelNameReads(Shared, static_cast<uint32_t>(Shared.find("T *p") + 1));
	// 終了
	return;
}

/**
 * リソース不足と内部の不具合の失敗分類の検査関数
 * 計算量：O(1)
 * @throws std::bad_alloc テスト用の例外の本文を確保出来ない場合
 * @throws std::runtime_error 原因に対応しない理由又は対処を返す場合
 */
static void CheckExceptionFailures() {
	// メモリ・保持領域と，スレッドの起動に要るリソースの不足
	Check(FailureReason::FromException(std::bad_alloc()) == FailureReason::ResourceFailure, "allocation failure lost its cause");
	Check(
		FailureReason::FromException(std::length_error("storage limit")) == FailureReason::ResourceFailure,
		"storage limit lost its cause"
	);
	Check(
		FailureReason::FromException(std::system_error(std::make_error_code(std::errc::not_enough_memory))) ==
		FailureReason::ResourceFailure &&
		FailureReason::FromException(std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again))) ==
		FailureReason::ResourceFailure,
		"execution resource failure lost its cause"
	);
	// 論理エラー・同期の不具合・未分類の例外をリソース不足と案内しない
	Check(
		FailureReason::FromException(std::logic_error("invariant")) == FailureReason::FormatterDefect &&
		FailureReason::FromException(std::runtime_error("unknown")) == FailureReason::FormatterDefect &&
		FailureReason::FromException(std::exception()) == FailureReason::FormatterDefect &&
		FailureReason::FromException(std::system_error(std::make_error_code(std::errc::resource_deadlock_would_occur))) ==
		FailureReason::FormatterDefect &&
		FailureReason::FromException(std::system_error(std::make_error_code(std::errc::permission_denied))) ==
		FailureReason::FormatterDefect,
		"internal failure was reported as resource exhaustion"
	);
	// 終了
	return;
}

/**
 * 閉じた構築予算でも中断を受け付ける事の検査関数
 * 計算量：固定した構文モデル数に対し O(1)
 * @throws std::bad_alloc 構文モデルのメモリを確保出来ない場合
 * @throws std::runtime_error 中断又は予算の契約に反する場合
 */
static void CheckWorkInterruption() {
	// 原文に依存しない２つの構文モデルと，正規の中間表現の終了口
	const GrammarProfile &Profile = GrammarProfile::Resolve(Lang::Get(Lang::JSON));
	Core::SyntaxModel Open(Profile.GetRules(), 0);
	const std::shared_ptr<Core::SyntaxModel> Closed = std::make_shared<Core::SyntaxModel>(Profile.GetRules(), 0);
	Closed->Charge(1);
	Core::FormatIR IR;
	IR.NormalizedSyntax = Closed;
	IR.CloseWork();
	const size_t Used = Closed->GetWorkAccount()->Used;
	Closed->Charge(std::numeric_limits<size_t>::max());
	Check(Closed->GetWorkAccount()->Used == Used, "closed construction budget charged later work");
	ParserAllocator::RequestInterruption();
	const Core::SyntaxModel *const Models[] { &Open, Closed.get() };
	for(const Core::SyntaxModel *const Model : Models) {
		bool WasInterrupted = false;
		try {
			Model->Charge(1);
		} catch(const ProcessingInterrupted &) {
			WasInterrupted = true;
		}
		Check(WasInterrupted, "syntax work ignored interruption after closing its construction budget");
	}
	// 終了
	return;
}

/**
 * ドキュメントの所属と共有接頭部の照合の検査関数
 * 計算量：固定した投影・照合の総費用 Q に対し O(Q)
 * @throws std::runtime_error 種類の同異，接頭部の照合又は保存量が異なる場合
 * @throws FormatLimitExceeded 投影の予算を超える場合
 * @throws std::bad_alloc 検査の記録を確保出来ない場合
 * @throws std::length_error 記録又はテキストの上限を超える場合
 * @throws std::out_of_range 種別又はノードが存在しない場合
 */
static void CheckDocIdentityStorage() {
	// 両側の番号の割当が異なっても，親と表示が同じ対象を照合する
	{
		const std::shared_ptr<ValidationContext::DocStorage> ExpectedOwner = std::make_shared<ValidationContext::DocStorage>(),
		ActualOwner = std::make_shared<ValidationContext::DocStorage>();
		ExpectedOwner->Targets = ActualOwner->Targets = "3:F#0";
		ExpectedOwner->Bodies = ActualOwner->Bodies = "Summary";
		ExpectedOwner->Lines = ActualOwner->Lines = { { 0, 7 } };
		ExpectedOwner->Scopes = { {}, { 0, "1:A" }, { 1, "2:B" } };
		ActualOwner->Scopes = { {}, { 0, "1:Other" }, { 0, "1:A" }, { 2, "2:B" } };
		const std::vector<ValidationContext::DocFact> Expected {
			{ ExpectedOwner, ExpectedOwner->Targets, ExpectedOwner->Bodies, ExpectedOwner->Lines, true, 2 }
		}, Actual { { ActualOwner, ActualOwner->Targets, ActualOwner->Bodies, ActualOwner->Lines, true, 3 } };
		size_t Work = 0, Queries = 0;
		Check(
			SyntaxValidatorCmp::HasSameDocs(Expected, Actual, Work, Queries),
			"independent document scope numbers changed a matching target"
		);
		ActualOwner->Scopes.back().Label = "2:C";
		Check(!SyntaxValidatorCmp::HasSameDocs(Expected, Actual, Work, Queries), "a changed document owner was not rejected");
	}
	// 深い宣言でも祖先の表示を対象毎に複写しない
	size_t PreviousBytes = 0, PreviousVisits = 0;
	for(const size_t Count : { 16U, 32U, 64U }) {
		std::string Text;
		for(size_t Idx = 0; Idx < Count; ++Idx) Text += "namespace N { /** Summary */ void F() {}\n";
		Text.append(Count, '}');
		std::array<size_t, 4> Queries {};
		const Core::ProjectionResult Projection = ProjectDiagnosticCase(Lang::Get(Lang::Cpp), Text, false, nullptr, &Queries);
		const std::vector<ValidationContext::DocFact> &Docs = Projection.Validation.ProtectedRegions.Docs;
		Check(Docs.size() == Count, "nested documents lost their targets");
		const ValidationContext::DocStorage &Owner = *Docs.front().Owner;
		size_t Bytes = Owner.Targets.size();
		for(size_t Idx = 1; Idx < Owner.Scopes.size(); ++Idx) {
			Check(Owner.Scopes[Idx].Parent < Idx, "document scope storage is not ordered from parent to child");
			Bytes += Owner.Scopes[Idx].Label.size();
		}
		Check(!PreviousBytes || Bytes <= 3 * PreviousBytes, "document target storage grows quadratically with nesting");
		Check(!PreviousVisits || Queries[1] <= 3 * PreviousVisits, "document ancestors were revisited for each target");
		PreviousBytes = Bytes;
		PreviousVisits = Queries[1];
	}
	// 終了
	return;
}

/**
 * 仮引数の導入の照会と利用集合の照会の一致の検査関数
 * 既存の golden の名前付の葉を全て照合し，単純なシグネチャの利用集合の不要な生成も検出する
 * 計算量：全入力の読取・投影・照会の総費用 Q に対し O(Q)
 * @throws std::runtime_error 判定・導入名・作業量が契約と異なる場合
 * @throws FormatLimitExceeded 解析・投影・照会の予算を超える場合
 * @throws std::bad_alloc 入力・構文・作業列を確保出来ない場合
 * @throws std::length_error 入力又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会するノードが表に無い場合
 * @throws std::filesystem::filesystem_error golden の入力を列挙出来ない場合
 */
static void CheckParamBindingIntroductions() {
	// 期待はシグネチャの構造から手で定め，出力テキストの期待を増やさない
	struct Fixture {
		::Lang::ID Lang; // 検査する言語
		const char *Prefix; // 関数の前の構造
		const char *Head; // 関数の名前より前
		const char *Tail; // 仮引数と本体
		const char *Suffix; // 関数の後の構造
	};
	const Fixture Fixtures[] {
		{ Lang::C, "", "int F", "(int a, int b) { return a+b; }\n", "" },
		{ Lang::Cpp, "", "int F", "(int a, int b) { return a+b; }\n", "" },
		{ Lang::Java, "class C {\n", "int F", "(int a, int b) { return a+b; }\n", "}\n" },
		{ Lang::JavaScript, "", "function F", "(a, b) { return a+b; }\n", "" }
	};
	#if !defined(ShaveFormatRefFacts)
	for(const Fixture &Input : Fixtures) {
		size_t Previous = 0;
		for(const size_t Count : { 16, 32, 64 }) {
			std::string Src(Input.Prefix);
			for(size_t Idx = 0; Idx < Count; ++Idx) Src += std::string(Input.Head) + std::to_string(Idx) + Input.Tail;
			Src += Input.Suffix;
			const Core::SyntaxModel Syntax = Project(Lang::Get(Input.Lang), Src);
			const std::vector<Core::NodeId> Functions = GetNodesWithRole(Syntax, Core::Role::DocumentedDef);
			Check(Functions.size() == Count, "a signature work fixture lost a function");
			const Core::ControlFlow Flow(Syntax);
			const size_t Before = Syntax.GetWorkAccount()->Used;
			for(const Core::NodeId Function : Functions) {
				const Core::Signature Signature = Core::SignatureExtractor::Extract(Syntax, Flow, Function);
				Check(Signature.Params == std::vector<std::string>{ "a", "b" }, "simple parameter names changed");
			}
			const size_t Used = Syntax.GetWorkAccount()->Used - Before;
			Check(!Core::SyntaxNormalizerAccess::GetBindingRegionCount(Syntax), "signature extraction built a binding use region");
			Check(!Previous || Used <= 3 * Previous, "signature binding work grew quadratically");
			std::printf(
				"param introduction work lang=%d functions=%zu nodes=%zu work=%zu regions=0\n",
				Input.Lang,
				Count,
				Syntax.GetNodeCount(),
				Used
			);
			Previous = Used;
		}
	}
	#else
	static_cast<void>(Fixtures);
	#endif
	// 全 golden の受理した構文の葉で，必要な３項目を完全な束縛と照合する
	const std::filesystem::path Root = std::filesystem::path(__FILE__).parent_path().parent_path() / "test_data/golden";
	Check(std::filesystem::is_directory(Root), "golden parameter binding inputs are missing");
	std::vector<std::filesystem::path> Paths;
	for(const std::filesystem::directory_entry &Entry : std::filesystem::recursive_directory_iterator(Root)) {
		if(Entry.is_regular_file() && Entry.path().extension() == ".in") Paths.push_back(Entry.path());
	}
	std::sort(Paths.begin(), Paths.end());
	size_t Inputs = 0, Leaves = 0;
	for(const std::filesystem::path &Path : Paths) {
		const std::string Language = Path.parent_path().filename().string();
		const ::Lang Lang = ::Lang::FromName(Language == "javascriptreact" ? "jsx" : Language == "typescriptreact" ? "tsx" : Language);
		Check(Lang.Id != ::Lang::Unknown, "a golden language has no parameter binding coverage", Language.c_str());
		std::ifstream File(Path, std::ios::binary);
		Check(static_cast<bool>(File), "a golden parameter binding input cannot be read");
		const std::string Src { std::istreambuf_iterator<char>(File), std::istreambuf_iterator<char>() };
		Check(!File.bad(), "a golden parameter binding input failed reading");
		const GrammarProfile &Profile = GrammarProfile::Resolve(Lang);
		InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, Src);
		if(Normalized.Reason != FailureReason::None) continue;
		ParseInput Input = ParseAdapter::Adapt(Profile, Normalized);
		if(Input.Decision != ParseDecision::Continue) continue;
		const ParsedTree Parsed = SyntaxParser::Parse(*Input.Profile, Input);
		if(Parsed.GetParseDecision() != ParseDecision::Continue) continue;
		const Core::ProjectionResult Projected = Core::SourceProjector::Project(Parsed, *Input.Profile, Input);
		const Core::SyntaxModel &Syntax = Projected.Syntax;
		++Inputs;
		for(const Core::NodeId Function : GetNodesWithRole(Syntax, Core::Role::OrdinaryFunctionDef)) {
			Core::SyntaxNormalizerAccess::CheckSimpleEndReach(Syntax, Function);
		}
		for(Core::NodeId Name = 0; Name < Syntax.GetNodeCount(); ++Name) {
			if(!Syntax.IsNamed(Name) || Syntax.GetNamedChildCount(Name)) continue;
			const Core::BindingIntroduction Introduction = Syntax.GetBindingIntroduction(Name);
			const Core::BindingFact Complete = Syntax.GetBindingFact(Name);
			Check(
				Introduction.IsDecl == Complete.IsDecl && Introduction.Namespace == Complete.Namespace &&
				Introduction.IsUncertain == Complete.IsUncertain,
				"parameter introduction differs from complete binding",
				Path.string().c_str()
			);
			++Leaves;
		}
	}
	std::printf("golden parameter introductions inputs=%zu leaves=%zu\n", Inputs, Leaves);
	// 終了
	return;
}

/**
 * 指定した入力の構造の工程別の作業量の検査関数
 * 計算量：読込・投影・正規化・中間表現の構築の総費用 Q に対し O(Q)
 * @param Path 入力ファイルのパス（拡張子から文法を選ぶ）
 * @return 全工程を予算内で完了すれば０，予算超過なら１
 * @throws std::runtime_error 読込を完了出来ない場合
 * @throws FormatLimitExceeded 投影が作業予算の上限を超える場合
 * @throws std::bad_alloc ソース又は検査のメモリを確保出来ない場合
 * @throws std::invalid_argument 文法又は投影の契約が成立しない場合
 * @throws std::length_error ソース又は構文の上限を超える場合
 * @throws std::out_of_range 工程が読む識別子が表に無い場合
 */
static int MeasureStructureWork(const char *const Path) {
	std::unique_ptr<std::FILE, decltype(&std::fclose)> Input(std::fopen(Path, "rb"), std::fclose);
	Check(static_cast<bool>(Input), "cannot open work input", Path);
	std::string Text;
	char Buffer[4096];
	while(const size_t Count = std::fread(Buffer, 1, sizeof(Buffer), Input.get())) Text.append(Buffer, Count);
	Check(!std::ferror(Input.get()) && std::feof(Input.get()), "cannot read work input", Path);
	Check(!std::fclose(Input.release()), "cannot close work input", Path);
	const std::shared_ptr<Core::SyntaxModel> Syntax = std::make_shared<Core::SyntaxModel>(Project(Lang::Detect(Path), Text));
	std::vector<std::pair<std::string_view, size_t>> Rules;
	size_t NormalizationWork = 0;
	bool IsComplete = false;
	const char *Stage = "normalization";
	try {
		NormalizationWork = Core::SyntaxNormalizerAccess::Run(*Syntax, &Rules).Work;
		Stage = "canonicalization";
		Core::Canonicalizer::Build(Syntax);
		IsComplete = true;
	} catch(const FormatLimitExceeded &) {
		std::printf("structure limit stage=%s used=%zu limit=%zu\n", Stage, Syntax->GetWorkAccount()->Used, Syntax->GetWorkLimit());
	}
	for(const auto &[Rule, Used] : Rules) std::printf("rule=%.*s work=%zu\n", static_cast<int>(Rule.size()), Rule.data(), Used);
	if(NormalizationWork) {
		std::printf("normalization=%zu canonicalization=%zu\n", NormalizationWork, Syntax->GetWorkAccount()->Used - NormalizationWork);
	}
	std::printf("structure complete=%d used=%zu limit=%zu\n", IsComplete, Syntax->GetWorkAccount()->Used, Syntax->GetWorkLimit());
	// 工程を完了したかの返戻
	return IsComplete ? 0 : 1;
}

/**
 * テストグループの実行関数
 * 失敗を報告して返し，呼出側が残りのテストグループを続けられる様にする
 * 計算量：テストグループの総費用 Q に対し O(Q)
 * @param Group 実行するテストグループ
 * @return テストグループの全ての条件が成り立てば真
 */
static bool RunGroup(const std::function<void()> &Group) {
	try {
		Group();
	} catch(const FormatLimitExceeded &E) {
		std::fprintf(stderr, "syntax model: %.*s\n", static_cast<int>(E.Reason.size()), E.Reason.data());
		// 構文表の処理の失敗の返戻
		return false;
	} catch(const std::exception &E) {
		std::fprintf(stderr, "syntax model: %s\n", E.what());
		// 失敗の返戻
		return false;
	}
	// 成功の返戻
	return true;
}

/**
 * 独自の構文表の投影と書換のテスト関数
 * 計算量：各検査の総費用 Q に対し O(Q)
 * @param Argc 実引数の数
 * @param Argv 名前・中断の検査，又は工程別の作業量と入力パスの指定又は空
 * @return 検査の成否に応じた終了状態
 */
int main(const int Argc, char **const Argv) {
	if(Argc == 3 && std::string_view(Argv[1]) == "--structure-work") {
		int Status = 1;
		// 指定した入力の工程別の作業量の返戻（読込又は契約の失敗も１）
		return RunGroup(
			[&]() -> void {
				Status = MeasureStructureWork(Argv[2]);
			}
		) ? Status : 1;
	}
	// 指定した名前の検査の結果の返戻
	if(Argc == 2 && std::string_view(Argv[1]) == "--name-scopes") return RunGroup(CheckNameScopesScale) ? 0 : 1;
	// 指定した中断の検査の結果の返戻
	if(Argc == 2 && std::string_view(Argv[1]) == "--work-interruption") return RunGroup(CheckWorkInterruption) ? 0 : 1;
	// 仮引数の導入と作業量の検査の結果の返戻
	if(Argc == 2 && std::string_view(Argv[1]) == "--param-bindings") return RunGroup(CheckParamBindingIntroductions) ? 0 : 1;
	// 失敗したテストグループの後も残りを流し，全ての失敗を１回の実行で出す
	bool IsPassed = true;
	for(
		void (*const Group)() :
		{
			CheckWorkSort,
			CheckParamBindingIntroductions,
			CheckExceptionFailures,
			CheckNameScopesScale,
			CheckDocIdentityStorage,
			CheckQualifiedNameProjection,
			CheckResultTypeBindings,
			CheckNestedPartitions,
			CheckErrorZoneValidation,
			CheckLiteralProjection,
			CheckIndividualRuleContracts,
			CheckTrailingLambdaCalls,
			CheckNormalizationRegressionScale,
			CheckNormalizationWorkScale,
			CheckVisibleScopeWork,
			CheckCallableScopeWork,
			CheckTailBindingTemps,
			CheckParseAdmissionWork,
			CheckProjectionWork,
			CheckSentenceBoundaryWork,
			CheckCommentBodyWork,
			CheckDocumentationProjectionWork,
			CheckDocumentationCompositionWork,
			CheckCanonicalCandidateWork,
			CheckDeclarationSplitWork,
			CheckCanonicalContextWork,
			CheckMarkupConstructionWork,
			CheckRoleAndCaptureWork,
			CheckTrailingCommentWork,
			CheckEmptyImmutableWork,
			CheckObservationFacts,
			CheckTerminationModes,
			CheckErrorZoneMatching,
			CheckDirectiveCommentValidation,
			CheckDiagnosticProjection,
			CheckParamAndDocDiagnostics,
			CheckReviewFacts,
			CheckSelectedFormCmp,
			CheckWarningOrder,
			CheckLineDiffAndMemory,
			CheckWorkInterruption // 中断の要求は以後の検査へ残さない為，最後に照合する
		}
	) IsPassed = RunGroup(Group) && IsPassed;
	// 全てのテストグループの成否の返戻
	return IsPassed ? 0 : 1;
}
