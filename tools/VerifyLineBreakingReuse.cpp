#include "Core/FormatIR.hpp"
#include "Core/LineBreaker.hpp"
#include "Util/FormatLimitExceeded.hpp"
#include "Util/Lang.hpp"
#include <tree_sitter/api.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Core {
	// 製品の非公開の基準手順を試験から呼ぶ為の入口
	struct LineBreakerComparison {
		struct Result {
			BrokenIR Output;
			size_t Work = 0;
			size_t Evaluations = 0;
			size_t LexemeRescanBytes = 0;
		};

		/**
		 * 同じ規則関数を使う何方かの評価手順の実行関数
		 * @param IR 幅非依存の中間表現
		 * @param MaxChars 最大文字数
		 * @param ShouldReuse 依存関係に依る再利用の有無
		 * @param FingerprintMask 衝突検体で使う要約値の印
		 * @return 解決結果と実際の作業量
		 */
		static Result Run(
			FormatIR IR,
			const size_t MaxChars,
			const bool ShouldReuse,
			const size_t FingerprintMask = std::numeric_limits<size_t>::max()
		) {
			// 評価器と作業量計測の準備
			const LineBreaker Breaker(MaxChars);
			LineBreaker::WorkMeter Meter;
			Result Checked;
			Checked.Output = Breaker.ResolveWithMode(std::move(IR), ShouldReuse, &Meter, FingerprintMask);
			Checked.Work = Meter.Used;
			Checked.Evaluations = Meter.Evaluations;
			Checked.LexemeRescanBytes = Meter.LexemeRescanBytes;
			// 評価結果と作業量の返戻
			return Checked;
		}
	};
} // namespace Core

/**
 * 試験条件の検査関数
 * @param IsSatisfied 成功条件
 * @param Message 失敗箇所
 * @throws std::runtime_error 条件を満たさない場合
 */
static void Check(const bool IsSatisfied, const char *const Message) {
	// 試験条件の確認
	if(!IsSatisfied) throw std::runtime_error(Message);
	// 終了
	return;
}

/**
 * 子を持つ群の追加関数
 * @param IR 中間表現
 * @param Group 群の番号
 * @param Children 子の並び
 * @return 群の要素番号
 */
static Core::FormatItemId AddGroup(Core::FormatIR &IR, const uint32_t Group, const std::vector<Core::FormatItemId> &Children) {
	// 群と識別番号の設定
	const Core::FormatItemId Id = IR.Parent(Core::FormatItemKind::Group, Children);
	IR.Items[Id].Group = Group;
	// 群の要素番号の返戻
	return Id;
}

/**
 * 展開時だけ出す字句の追加関数
 * @param IR 中間表現
 * @param Group 従う群の番号
 * @param Text 展開時の字面
 * @return 条件付要素の番号
 */
static Core::FormatItemId AddConditional(Core::FormatIR &IR, const uint32_t Group, const std::string_view Text) {
	// 条件付要素と群の関連付
	const Core::FormatItemId Token = IR.Leaf(Text, Core::FormatItemKind::Token);
	const std::vector<Core::FormatItemId> Children { Token };
	const Core::FormatItemId Id = IR.Parent(Core::FormatItemKind::IfBroken, Children);
	IR.Items[Id].Group = Group;
	// 条件付要素の番号の返戻
	return Id;
}

/**
 * ネストした群と相互に影響する包装を持つ中間表現の構築関数
 * 外側の展開で加わる括弧が内側の幅を変え，内側の分割が外側の右境界を変える
 * @param HasEdge 行端で値の変わる空白を使うか
 * @param HasHardBreak 必須改行を後続の宣言との間へ置くか
 * @return 試験に使う中間表現
 */
static Core::FormatIR MakeFixture(const bool HasEdge, const bool HasHardBreak) {
	// 内側の群と先頭字句の構築
	Core::FormatIR IR;
	const Core::FormatItemId Prefix = IR.Leaf("int value =", Core::FormatItemKind::Token);
	const std::vector<Core::FormatItemId> Inner {
		IR.Leaf("1111 +", Core::FormatItemKind::Token),
		IR.Leaf(" ", Core::FormatItemKind::SoftBreak),
		IR.Leaf("2222", Core::FormatItemKind::Token)
	};
	const Core::FormatItemId Expression = AddGroup(IR, 1, Inner);
	// 外側の区分と行端空白の構築
	std::vector<Core::FormatItemId> Outer { IR.Leaf(HasEdge ? "" : " ", Core::FormatItemKind::SoftBreak) };
	if(HasEdge) {
		const std::vector<Core::FormatItemId> Empty { IR.Leaf("", Core::FormatItemKind::Space) };
		const Core::FormatItemId Edge = IR.Parent(Core::FormatItemKind::LineEdgeSpace, Empty);
		IR.Items[Edge].Text = IR.Keep(" ");
		Outer.push_back(Edge);
	}
	Outer.push_back(AddConditional(IR, 0, "("));
	// 外側の包装と文の構築
	Outer.push_back(Expression);
	Outer.push_back(AddConditional(IR, 0, ")"));
	Outer.push_back(IR.Leaf(";", Core::FormatItemKind::Token));
	const Core::FormatItemId Statement = AddGroup(IR, 0, Outer);
	std::vector<Core::FormatItemId> Root { Prefix, Statement };
	// 後続宣言との必須改行の追加
	if(HasHardBreak) {
		Root.push_back(IR.Leaf("", Core::FormatItemKind::HardBreak));
		Root.push_back(IR.Leaf("int other = 3;", Core::FormatItemKind::Token));
	}
	IR.Root = IR.Parent(Core::FormatItemKind::Sequence, Root);
	// 構築した中間表現の返戻
	return IR;
}

/**
 * 左右の残存境界を巡に依って変える中間表現の構築関数
 * 外側の包装後に左の群が展開し，右の群は新しい左境界で平坦に戻る
 * @return 試験に使う中間表現
 */
static Core::FormatIR MakeNeighborFixture() {
	// 左右の群の字句の構築
	Core::FormatIR IR;
	const Core::FormatItemId Prefix = IR.Leaf("int value =", Core::FormatItemKind::Token);
	const std::vector<Core::FormatItemId> LeftParts {
		IR.Leaf("1111 +", Core::FormatItemKind::Token),
		IR.Leaf(" ", Core::FormatItemKind::SoftBreak),
		IR.Leaf("2222 +", Core::FormatItemKind::Token)
	};
	const std::vector<Core::FormatItemId> RightParts {
		IR.Leaf(" ", Core::FormatItemKind::SoftBreak),
		IR.Leaf("3333", Core::FormatItemKind::Token)
	};
	// 左右の群を包む外側の区分の構築
	const std::vector<Core::FormatItemId> Outer {
		IR.Leaf(" ", Core::FormatItemKind::SoftBreak),
		AddConditional(IR, 2, "("),
		AddGroup(IR, 0, LeftParts),
		AddGroup(IR, 1, RightParts),
		AddConditional(IR, 2, ")"),
		IR.Leaf(";", Core::FormatItemKind::Token)
	};
	const Core::FormatItemId Statement = AddGroup(IR, 2, Outer);
	const std::vector<Core::FormatItemId> Root { Prefix, Statement };
	IR.Root = IR.Parent(Core::FormatItemKind::Sequence, Root);
	// 構築した中間表現の返戻
	return IR;
}

/**
 * 宣言区分の字句再走査を計数する中間表現の構築関数
 * @return 単一宣言子を持つ中間表現
 */
static Core::FormatIR MakeDeclarationFixture() {
	// 宣言子と空の値の構築
	Core::FormatIR IR;
	const std::vector<Core::FormatItemId> Empty;
	const Core::FormatItemId Name = IR.Leaf("value", Core::FormatItemKind::Token);
	const Core::FormatItemId Value = IR.Parent(Core::FormatItemKind::Sequence, Empty);
	const std::vector<Core::FormatItemId> UnitParts { Name, Value };
	// 宣言区分の部品の構築
	const std::vector<Core::FormatItemId> Parts {
		IR.Leaf("int ", Core::FormatItemKind::Token),
		IR.Parent(Core::FormatItemKind::Sequence, Empty),
		IR.Leaf(";", Core::FormatItemKind::Token),
		IR.Leaf(", ", Core::FormatItemKind::Token),
		IR.Leaf("", Core::FormatItemKind::HardBreak),
		IR.Parent(Core::FormatItemKind::Sequence, UnitParts)
	};
	IR.Root = IR.Parent(Core::FormatItemKind::DeclarationSplit, Parts);
	IR.Items[IR.Root].Node = 1;
	// 宣言の中間表現の返戻
	return IR;
}

/**
 * 二つの群が交互に前巡の完全状態へ戻る中間表現の構築関数
 * @return 循環判定の検体
 */
static Core::FormatIR MakeCycleFixture() {
	// 交互に展開する二群の構築
	Core::FormatIR IR;
	const std::vector<Core::FormatItemId> Left { IR.Leaf(" ", Core::FormatItemKind::SoftBreak) };
	const std::vector<Core::FormatItemId> Right { IR.Leaf(" ", Core::FormatItemKind::SoftBreak) };
	// 群を組み込む文の構築
	const std::vector<Core::FormatItemId> Parts {
		IR.Leaf("int value =", Core::FormatItemKind::Token),
		AddGroup(IR, 0, Left),
		AddConditional(IR, 1, "("),
		IR.Leaf("1111 +", Core::FormatItemKind::Token),
		AddGroup(IR, 1, Right),
		IR.Leaf("2222", Core::FormatItemKind::Token),
		AddConditional(IR, 1, ")"),
		IR.Leaf(";", Core::FormatItemKind::Token)
	};
	IR.Root = IR.Parent(Core::FormatItemKind::Sequence, Parts);
	// 循環検体の返戻
	return IR;
}

/**
 * 左右の区間変更へ右側の必須境界を加えた中間表現の構築関数
 * @return 両側の境界を比較する中間表現
 */
static Core::FormatIR MakeBoundedNeighborFixture() {
	// 右側の必須境界の追加
	Core::FormatIR IR = MakeNeighborFixture();
	const std::vector<Core::FormatItemId> Root {
		IR.Root,
		IR.Leaf("", Core::FormatItemKind::HardBreak),
		IR.Leaf("int other = 3;", Core::FormatItemKind::Token)
	};
	IR.Root = IR.Parent(Core::FormatItemKind::Sequence, Root);
	// 右側の必須境界を持つ検体の返戻
	return IR;
}

/**
 * 一定の幅を持つ長い宣言列の構築関数
 * 計算量：宣言数 N に対し O(N)
 * @param Count 宣言の数（１０００未満）
 * @return 各宣言に条件付の包装を持つ中間表現
 */
static Core::FormatIR MakeLongDeclarationsFixture(const size_t Count) {
	// 宣言数の上限確認
	Check(Count < 1000, "the long declaration fixture has too many declarations");
	// 連続する宣言の構築
	Core::FormatIR IR;
	std::vector<Core::FormatItemId> Root;
	for(size_t Index = 0; Index < Count; ++Index) {
		if(Index) Root.push_back(IR.Leaf("", Core::FormatItemKind::HardBreak));
		const std::string Digits = std::to_string(Index);
		const std::string Name = "int v" + std::string(3 - Digits.size(), '0') + Digits + " =";
		Root.push_back(IR.Leaf(Name, Core::FormatItemKind::Token));
		const std::vector<Core::FormatItemId> Parts {
			IR.Leaf(" ", Core::FormatItemKind::SoftBreak),
			AddConditional(IR, static_cast<uint32_t>(Index), "("),
			IR.Leaf("1111", Core::FormatItemKind::Token),
			AddConditional(IR, static_cast<uint32_t>(Index), ")"),
			IR.Leaf(";", Core::FormatItemKind::Token)
		};
		Root.push_back(AddGroup(IR, static_cast<uint32_t>(Index), Parts));
	}
	IR.Root = IR.Parent(Core::FormatItemKind::Sequence, Root);
	// 宣言列の返戻
	return IR;
}

/**
 * 深い木の中へ同じ行分割検体を置く関数
 * 計算量：深さ D に対し O(D)
 * @param Depth 単一の子を持つ容器の段数
 * @return 深い木を持つ中間表現
 */
static Core::FormatIR MakeDeepFixture(const size_t Depth) {
	// 既存検体を包む深い木の構築
	Core::FormatIR IR = MakeFixture(false, false);
	for(size_t Index = 0; Index < Depth; ++Index) {
		const std::vector<Core::FormatItemId> Child { IR.Root };
		IR.Root = IR.Parent(Core::FormatItemKind::Sequence, Child);
	}
	// 深い木の返戻
	return IR;
}

/**
 * 隣の群の条件付包装が測定区間へ入る式の構築関数
 * 計算量：群の数 N に対し O(N)
 * @param Count 相互に接する群の数
 * @return 条件付断片の連鎖を持つ中間表現
 */
static Core::FormatIR MakeConditionalChainFixture(const size_t Count) {
	// 条件付断片を持つ群の連鎖の構築
	Core::FormatIR IR;
	std::vector<Core::FormatItemId> Root { IR.Leaf("int value = 1", Core::FormatItemKind::Token) };
	for(size_t Index = 0; Index < Count; ++Index) {
		Root.push_back(IR.Leaf(" +", Core::FormatItemKind::Token));
		const std::vector<Core::FormatItemId> Parts {
			IR.Leaf(" ", Core::FormatItemKind::SoftBreak),
			AddConditional(IR, static_cast<uint32_t>(Index), "("),
			IR.Leaf("1", Core::FormatItemKind::Token),
			AddConditional(IR, static_cast<uint32_t>(Index), ")")
		};
		Root.push_back(AddGroup(IR, static_cast<uint32_t>(Index), Parts));
	}
	Root.push_back(IR.Leaf(";", Core::FormatItemKind::Token));
	IR.Root = IR.Parent(Core::FormatItemKind::Sequence, Root);
	// 条件付断片の連鎖の返戻
	return IR;
}

/**
 * 解決済の要素から行分割の字面を得る関数
 * @param IR 解決済の中間表現
 * @return 出力の字面
 */
static std::string Render(const Core::BrokenIR &IR) {
	// 解決済要素からの字面の連結
	std::string Text;
	for(const Core::ResolvedItem &Item : IR.Items) {
		if(Item.Kind == Core::ResolvedKind::Newline) Text.push_back('\n');
		else if(
			Item.Kind == Core::ResolvedKind::Text || Item.Kind == Core::ResolvedKind::Separator || Item.Kind == Core::ResolvedKind::Verbatim
		) Text.append(IR.TextOf(Item));
	}
	// 字面の返戻
	return Text;
}

/**
 * 二つの解決結果の全ての選択と要素を照合する関数
 * @param Left 基準手順の結果
 * @param Right 再利用手順の結果
 * @return 完全に一致するなら true
 */
static bool IsSame(const Core::BrokenIR &Left, const Core::BrokenIR &Right) {
	// 字面と要素の数が違う事の返戻
	if(Left.Texts != Right.Texts || Left.Items.size() != Right.Items.size()) return false;
	// 解決済要素の一致の照合
	for(size_t Index = 0; Index < Left.Items.size(); ++Index) {
		const Core::ResolvedItem &A = Left.Items[Index], &B = Right.Items[Index];
		// 要素の種類・字面・節点が違う事の返戻
		if(
			A.Kind != B.Kind || A.IsForced != B.IsForced || A.Text.Begin != B.Text.Begin || A.Text.Size != B.Text.Size ||
			A.Node != B.Node || A.Before != B.Before || A.After != B.After || A.Generated != B.Generated
		) return false;
	}
	// 選択状態の一致の照合
	const Core::OutputExpectation &A = Left.Expectation, &B = Right.Expectation;
	// 選択の数と宣言の区分の数が違う事の返戻
	if(
		A.NormalizedSyntax != B.NormalizedSyntax || A.SelectedForms.size() != B.SelectedForms.size() ||
		A.DeclarationPartitions.size() != B.DeclarationPartitions.size()
	) return false;
	for(size_t Index = 0; Index < A.SelectedForms.size(); ++Index) {
		const Core::SelectedForm &First = A.SelectedForms[Index], &Second = B.SelectedForms[Index];
		if(First.IsBroken != Second.IsBroken || First.Nodes != Second.Nodes || First.Effects.size() != Second.Effects.size()) {
			// 選択の形・節点・効果の数が違う事の返戻
			return false;
		}
		for(size_t Effect = 0; Effect < First.Effects.size(); ++Effect) {
			const Core::SelectedFormEffect &One = First.Effects[Effect], &Two = Second.Effects[Effect];
			// 選択の効果が違う事の返戻
			if(
				One.Node != Two.Node || One.Operation != Two.Operation || One.DeclarationOwner != Two.DeclarationOwner ||
				One.DeclarationRow != Two.DeclarationRow
			) return false;
		}
	}
	for(size_t Index = 0; Index < A.DeclarationPartitions.size(); ++Index) {
		const Core::DeclarationPartition &First = A.DeclarationPartitions[Index], &Second = B.DeclarationPartitions[Index];
		// 宣言の区分の行の数が違う事の返戻
		if(First.Node != Second.Node || First.Rows.size() != Second.Rows.size()) return false;
		for(size_t Row = 0; Row < First.Rows.size(); ++Row) {
			// 宣言の区分の行の範囲が違う事の返戻
			if(First.Rows[Row].Begin != Second.Rows[Row].Begin || First.Rows[Row].End != Second.Rows[Row].End) return false;
		}
	}
	// 完全な選択状態と解決済の要素の一致の返戻
	return true;
}

/**
 * C++ の出力が有効な構文である事の検査関数
 * @param Text 解析する出力
 * @throws std::runtime_error 解析器を構築出来ない場合又は出力が構文として壊れて居る場合
 */
static void CheckSyntax(const std::string &Text) {
	// 構文解析器の準備と言語設定
	TSParser *const Parser = ts_parser_new();
	Check(Parser != nullptr, "the parser could not be created");
	if(!ts_parser_set_language(Parser, Lang::Get(Lang::Cpp).TsLang())) {
		ts_parser_delete(Parser);
		throw std::runtime_error("the C++ grammar could not be selected");
	}
	// 出力の構文木と誤り状態の取得
	TSTree *const Tree = ts_parser_parse_string(Parser, nullptr, Text.data(), static_cast<uint32_t>(Text.size()));
	if(!Tree) {
		ts_parser_delete(Parser);
		throw std::runtime_error("the output could not be parsed");
	}
	const TSNode Root = ts_tree_root_node(Tree);
	const bool HasError = ts_node_has_error(Root);
	ts_tree_delete(Tree);
	ts_parser_delete(Parser);
	Check(!HasError, "a fixture produced invalid C++ syntax");
	// 終了
	return;
}

/**
 * 宣言列の条文上の期待字面を作る関数
 * 計算量：宣言数 N に対し O(N)
 * @param Count 宣言の数
 * @return 上限１５で各宣言の区切位置を折った字面
 */
static std::string ExpectedLongDeclarations(const size_t Count) {
	// 宣言毎の期待字面の構築
	std::string Expected;
	for(size_t Index = 0; Index < Count; ++Index) {
		char Name[16];
		std::snprintf(Name, sizeof(Name), "v%03zu", Index);
		if(Index) Expected.push_back('\n');
		Expected += "int ";
		Expected += Name;
		Expected += " =\n(1111);";
	}
	// 宣言列の期待字面の返戻
	return Expected;
}

/**
 * 条件付断片の連鎖の条文上の期待字面を作る関数
 * 計算量：群の数 N に対し O(N)
 * @param Count 接する群の数
 * @return 上限１で全ての群を折った字面
 */
static std::string ExpectedConditionalChain(const size_t Count) {
	// 条件付断片を折った期待字面の構築
	std::string Expected = "int value = 1";
	for(size_t Index = 0; Index < Count; ++Index) Expected += " +\n(1)";
	Expected.push_back(';');
	// 条件付断片の連鎖の期待字面の返戻
	return Expected;
}

/**
 * 追加検体の字面・完全な選択状態・構文を照合する関数
 * @param Fixture 同じ規則へ渡す中間表現
 * @param MaxChars 最大文字数
 * @param Expected 条文から独立に導いた期待字面
 * @param GroupCount 期待する群の数
 * @param BrokenCount 展開すると期待する群の数
 * @param FingerprintMask 巡の要約値へ掛ける印
 */
static void CompareConstructed(
	const Core::FormatIR &Fixture,
	const size_t MaxChars,
	const std::string_view Expected,
	const size_t GroupCount,
	const size_t BrokenCount,
	const size_t FingerprintMask = std::numeric_limits<size_t>::max()
) {
	// 基準手順と再利用手順の実行
	const Core::LineBreakerComparison::Result Reference =
	Core::LineBreakerComparison::Run(Fixture, MaxChars, false, FingerprintMask);
	const Core::LineBreakerComparison::Result Reused = Core::LineBreakerComparison::Run(Fixture, MaxChars, true, FingerprintMask);
	// 字面と構文の照合
	Check(IsSame(Reference.Output, Reused.Output), "constructed fixture states differ");
	const std::string Actual = Render(Reused.Output);
	Check(std::string_view(Actual) == Expected, "constructed fixture disagrees with its expected output");
	CheckSyntax(Actual);
	// 群の選択状態の照合
	const std::vector<Core::SelectedForm> &Forms = Reused.Output.Expectation.SelectedForms;
	Check(Forms.size() == GroupCount, "constructed fixture has the wrong number of groups");
	size_t Broken = 0;
	for(const Core::SelectedForm &Form : Forms) Broken += Form.IsBroken;
	Check(Broken == BrokenCount, "constructed fixture has the wrong selection state");
	std::printf(
		"constructed groups=%zu width=%zu mask=%zu reference=%zu reused=%zu\n",
		GroupCount,
		MaxChars,
		FingerprintMask,
		Reference.Work,
		Reused.Work
	);
	// 終了
	return;
}

/**
 * 基準手順と再利用手順を同じ中間表現で比較する関数
 * @param MaxChars 最大文字数
 * @param HasEdge 行端で値の変わる空白を含めるか
 * @param Expected 条文から導いた期待字面
 * @param IsOuterBroken 外側の群を展開するか
 * @param IsInnerBroken 内側の群を展開するか
 * @param HasHardBreak 必須改行を後続の宣言との間へ置くか
 */
static void Compare(
	const size_t MaxChars,
	const bool HasEdge,
	const std::string_view Expected,
	const bool IsOuterBroken,
	const bool IsInnerBroken,
	const bool HasHardBreak = false
) {
	// 境界検体での両手順の実行
	const Core::FormatIR Fixture = MakeFixture(HasEdge, HasHardBreak);
	const Core::LineBreakerComparison::Result Reference = Core::LineBreakerComparison::Run(Fixture, MaxChars, false);
	const Core::LineBreakerComparison::Result Reused = Core::LineBreakerComparison::Run(Fixture, MaxChars, true);
	// 字面と構文の照合
	Check(IsSame(Reference.Output, Reused.Output), "the complete line breaking states differ");
	const std::string Actual = Render(Reused.Output);
	Check(std::string_view(Actual) == Expected, "line breaking disagrees with the specified width boundary");
	CheckSyntax(Actual);
	// 内外の群の選択と再利用量の照合
	Check(Reused.Output.Expectation.SelectedForms.size() == 2, "a group selection is missing");
	Check(Reused.Output.Expectation.SelectedForms[0].IsBroken == IsInnerBroken, "the inner group selection is wrong");
	Check(Reused.Output.Expectation.SelectedForms[1].IsBroken == IsOuterBroken, "the outer group selection is wrong");
	if(MaxChars == 13) Check(Reused.Evaluations < Reference.Evaluations, "a reusable evaluation was repeated");
	std::printf(
		"width=%zu edge=%d reference=%zu/%zu/%zu reused=%zu/%zu/%zu\n",
		MaxChars,
		HasEdge,
		Reference.Work,
		Reference.Evaluations,
		Reference.LexemeRescanBytes,
		Reused.Work,
		Reused.Evaluations,
		Reused.LexemeRescanBytes
	);
	// 終了
	return;
}

/**
 * 左右の区間変更と非単調な選択を比較する関数
 */
static void CompareNeighbors() {
	// 隣接する群での両手順の実行
	const Core::FormatIR Fixture = MakeNeighborFixture();
	const Core::LineBreakerComparison::Result Reference = Core::LineBreakerComparison::Run(Fixture, 13, false);
	const Core::LineBreakerComparison::Result Reused = Core::LineBreakerComparison::Run(Fixture, 13, true);
	// 字面と構文の照合
	Check(IsSame(Reference.Output, Reused.Output), "neighboring group states differ");
	const std::string Actual = Render(Reused.Output);
	Check(Actual == "int value =\n(1111 +\n2222 + 3333);", "a changed interval chose the wrong output");
	CheckSyntax(Actual);
	// 非単調な選択と再利用量の照合
	const std::vector<Core::SelectedForm> &Forms = Reused.Output.Expectation.SelectedForms;
	Check(Forms.size() == 3 && Forms[0].IsBroken && !Forms[1].IsBroken && Forms[2].IsBroken, "nonmonotone choices differ");
	Check(Reused.Evaluations < Reference.Evaluations, "neighboring evaluations were not reused");
	std::printf(
		"neighbors reference=%zu/%zu/%zu reused=%zu/%zu/%zu\n",
		Reference.Work,
		Reference.Evaluations,
		Reference.LexemeRescanBytes,
		Reused.Work,
		Reused.Evaluations,
		Reused.LexemeRescanBytes
	);
	// 終了
	return;
}

/**
 * 宣言の再走査量を比較する関数
 */
static void CompareDeclaration() {
	// 宣言区分での両手順の実行
	const Core::FormatIR Fixture = MakeDeclarationFixture();
	const Core::LineBreakerComparison::Result Reference = Core::LineBreakerComparison::Run(Fixture, 24, false);
	const Core::LineBreakerComparison::Result Reused = Core::LineBreakerComparison::Run(Fixture, 24, true);
	// 宣言の字面と構文の照合
	Check(IsSame(Reference.Output, Reused.Output), "declaration partitions differ");
	const std::string Actual = Render(Reused.Output);
	Check(Actual == "int value;", "the declaration output is wrong");
	CheckSyntax(Actual);
	// 字句再走査量の照合
	Check(
		Reference.LexemeRescanBytes != 0 && Reused.LexemeRescanBytes == Reference.LexemeRescanBytes,
		"lexeme rescans were not counted"
	);
	std::printf(
		"declaration reference=%zu/%zu/%zu reused=%zu/%zu/%zu\n",
		Reference.Work,
		Reference.Evaluations,
		Reference.LexemeRescanBytes,
		Reused.Work,
		Reused.Evaluations,
		Reused.LexemeRescanBytes
	);
	// 終了
	return;
}

/**
 * 完全な選択状態の循環を両手順が同じ失敗へ分類する事の検査関数
 * @param FingerprintMask 巡の要約値へ掛ける印
 */
static void CompareCycle(const size_t FingerprintMask = std::numeric_limits<size_t>::max()) {
	// 循環検体と失敗分類関数の準備
	const Core::FormatIR Fixture = MakeCycleFixture();
	const auto Failure = [&Fixture, FingerprintMask](const bool ShouldReuse) -> FailureReason::Kind {
		try {
			Core::LineBreakerComparison::Run(Fixture, 18, ShouldReuse, FingerprintMask);
		} catch(const FormatLimitExceeded &Error) {
			Check(Error.Reason == "line breaking does not converge", "the wrong cycle was reported");
			// 循環の失敗分類の返戻
			return Error.Kind;
		}
		throw std::runtime_error("a selection cycle was accepted");
	};
	// 両手順の循環失敗分類の照合
	Check(Failure(false) == FailureReason::FormatterDefect, "the reference cycle was misclassified");
	Check(Failure(true) == FailureReason::FormatterDefect, "the reused cycle was misclassified");
	// 終了
	return;
}

/**
 * 幅の境界と依存関係の比較試験の実行関数
 * @return 検査の成否に応じた終了状態
 */
int main() {
	// 幅の境界と依存関係の検体の照合
	try {
		std::puts("counts: work/evaluations/lexeme_rescan_bytes");
		Compare(13, false, "int value =\n(1111 +\n2222);", true, true);
		Compare(23, false, "int value =\n(1111 + 2222);", true, false);
		Compare(24, false, "int value = 1111 + 2222;", false, false);
		Compare(25, false, "int value = 1111 + 2222;", false, false);
		Compare(0, false, "int value = 1111 + 2222;\nint other = 3;", false, false, true);
		Compare(13, true, "int value =\n(1111 +\n2222);", true, true);
		CompareNeighbors();
		CompareDeclaration();
		CompareConstructed(MakeLongDeclarationsFixture(64), 15, ExpectedLongDeclarations(64), 64, 64);
		CompareConstructed(MakeDeepFixture(512), 13, "int value =\n(1111 +\n2222);", 2, 2);
		CompareConstructed(MakeConditionalChainFixture(16), 1, ExpectedConditionalChain(16), 16, 16);
		CompareConstructed(MakeBoundedNeighborFixture(), 13, "int value =\n(1111 +\n2222 + 3333);\nint other = 3;", 3, 2);
		CompareConstructed(MakeFixture(false, false), 13, "int value =\n(1111 +\n2222);", 2, 2, 0);
		CompareCycle();
		CompareCycle(0);
	} catch(const std::exception &Error) {
		std::fprintf(stderr, "line breaking reuse: %s\n", Error.what());
		// 照合の中の例外で失敗とする事の返戻
		return 1;
	}
	// 全ての照合を通った事の返戻
	return 0;
}
