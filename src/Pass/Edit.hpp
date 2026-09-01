#pragma once

#include "../Util/DeclEdit.hpp"
#include "../Util/Lang.hpp"
#include "../Util/TextEdit.hpp"
#include "../Util/TsSource.hpp"
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// 編集パスクラス
class EditPass {
private:

	enum class CppDeclarationHead { Other, Name, Call }; // 宣言先頭の構文分類（名前だけでは関数形式に為らない）

	// キーワード置換・削除候補の対象トークンと宣言識別子
	struct KeywordSwapCandidate {
		TSNode Tok; // 置換又は削除するキーワード節点
		TSNode IdNode; // 候補を識別する宣言名節点
	};

	// Ruby の局所変数の導入の索引
	struct RubyLocals {
		std::unordered_map<const void *, std::unordered_map<std::string_view, uint32_t>> First; // スコープ毎の名前の最初の導入位置
		std::unordered_map<const void *, std::vector<const void *>> Chains; // 対象の節点毎の，導入の見えるスコープ（内から外へ）
		bool IsDefined(const TSNode Node, const uint32_t Start, const std::string_view Name) const; // 局所変数の導入済判定関数
	};

	static constexpr int UnknownInnerPrec = std::numeric_limits<int>::max(); // 未登録演算子の内側式優先度
	static RubyLocals IndexRubyLocals(const TSSource &Src, const NodeKind::NodeTypeSet &Targets); // Ruby の局所変数の導入の索引の構築関数
	static void CollectKotlinJointEdit(const TSSource &Src, const TSNode Node, std::vector<TextEdit> &Edits); // Kotlin の改行を跨ぐ継ぎ目の区切り編集の収集関数
	static bool IsOperatorChar(const char Char); // 演算子を成す記号の判定関数
	static TSNode SwiftPrefixArguments(const TSSource &Src, const TSNode Node); // Swift の前置の演算子の呼出と読んだ括弧の実引数の並びの取得関数
	static bool HasBodyLikeExpression(const TSSource &Src, const TSNode Node, const Lang Language); // 見出の本体の始まりと紛れる式の判定関数
	static bool MaskLinkageGuards(std::string &Text, const std::string &Source, char &Marker); // 前処理の条件を跨ぐ `extern "C"` の波括弧の印への置換関数
	static int OpPrecedence(const TSSource &Src, const TSNode OpNode, const Lang Language); // 演算子優先度の取得関数
	static TSNode RightEdgeChild(const TSSource &Src, const TSNode Node); // 右端の子の取得関数
	static bool IsPostfixAtom(const TSSource &Src, TSNode Node, const Lang Language); // 後置の連鎖の原子式の判定関数
	static bool KeepsPostfixReceiver(const TSSource &Src, const TSNode Paren, const TSNode Host, const Lang Language); // 後置の式の受け手を包む括弧の保持判定関数

	static bool HasCppDeclarationHead(
		const TSSource &Src,
		TSNode Node,
		std::unordered_map<const void *, CppDeclarationHead> &Cache
	); // 宣言にも読める C++ の式先頭の判定関数

	static bool ReadsAsJsStatementHead(const TSSource &Src, const TSNode Paren, const TSNode Parent, const TSNode Inner); // JS/TS の括弧を外すと内側が文頭の塊・宣言・指示として読まれるかの判定関数
	static bool ShieldsJsForIn(const TSSource &Src, const TSNode Inner); // JS/TS の for 文の初期化子の `in` を守る括弧かの判定関数
	static bool FusesPrefixOperator(const Lang Language, const char Operator, const char Operand); // 前置の演算子と被演算子の先頭の記号の融合判定関数
	static bool OpensTsTypeArguments(const TSSource &Src, TSNode Operand, const Lang Language); // TS の型引数の閉じ `>` の直後に来る被演算子かの判定関数
	static void CollapseParenLines(const TSSource &Src, const TSNode Inner, std::vector<TextEdit> &Edits); // 括弧の直下の改行の畳込関数
	static void PrepareKotlinValueContexts(const TSSource &Src); // Kotlin の文と枝を持つ式の値が使われるかの一括判定関数
	static bool IsKotlinStatementValueUsed(const TSSource &Src, const TSNode Statement); // Kotlin の文の値が使われるかの判定関数
	static void CollectBraceEdits(const TSSource &Src, const Lang Language, std::vector<TextEdit> &Edits); // 行幅に依らない波括弧編集の収集関数

	static void MaybeCollectIncrementEdit(
		const TSSource &Src,
		const TSNode StmtChild,
		const Lang Language,
		std::vector<TextEdit> &Edits
	); // 増分演算子編集の収集関数

	static void MaybeCollectInfiniteForEdit(
		const TSSource &Src,
		const TSNode Node,
		const Lang Language,
		std::vector<TextEdit> &Edits
	); // 無限ループ for 編集の収集関数

	static void MaybeCollectBoolLiteralEdit(const TSSource &Src, const TSNode Node, std::vector<TextEdit> &Edits); // 真偽値リテラル編集の収集関数
	static size_t RustNumericSuffixStart(const std::string_view Text); // Rust 数値リテラルの型接尾辞開始位置の取得関数
	static std::string NormalizeNumericLiteral(const std::string_view Text, const Lang Language, const bool ShouldPad); // 数値リテラルの正規化関数（揺れの有る英字を大文字化）

	static void CollectParenPairEdit(
		const TSSource &Src,
		const uint32_t Start,
		const uint32_t End,
		const uint32_t Depth,
		const TSNode UnaryOp,
		const bool IsSymbolGap,
		std::vector<TextEdit> &Edits
	); // 括弧の対の除去編集の収集関数

	static void CollectNestedLayerEdit(const TSSource &Src, const TSNode Outer, const TSNode Content, std::vector<TextEdit> &Edits); // ネストの括弧の内側の層の除去編集の収集関数

	static void MaybeCollectOptionalParenEdit(
		const TSSource &Src,
		const TSNode Node,
		const TSNode OpAncestor,
		const TSNode OperandTop,
		const Lang Language,
		std::vector<TextEdit> &Edits
	); // 省ける括弧の除去編集の収集関数

	static TSNode TypeParenContent(const TSSource &Src, const TSNode Node, const Lang Language); // 型を包む括弧の中身の取得関数

	static void MaybeCollectTypeParenEdit(
		const TSSource &Src,
		const TSNode Node,
		const TSNode Parent,
		const TSNode NextSibling,
		const Lang Language,
		std::unordered_set<const void *> &NestedLayers,
		std::vector<TextEdit> &Edits
	); // 型を包む冗長な括弧の除去編集の収集関数

	static bool CanBarePythonTuple(const TSSource &Src, const TSNode Tuple, const TSNode Host, const TSNode Slot); // Python の組の括弧を外せるかの判定関数

	static void CollectConsolidatedNamedWalk(
		const TSSource &Src,
		const TSNode StartNode,
		const TSNode StartParent,
		const TSNode StartPrev,
		const TSNode StartNext,
		const Lang Language,
		std::vector<TextEdit> &Edits
	); // 統合名前付走査の編集収集関数

	static bool IsAnyNameMutated(
		const std::vector<std::string_view> &Names,
		const std::unordered_set<std::string_view> &MutatedNames
	); // 宣言子が変更識別子集合に含まれるかの判定関数

	static void CollectJavaFinalEdits(const TSSource &Src, std::vector<TextEdit> &Edits); // Java final 付与編集の収集関数
	static void CollectRustMutRemoveEdits(const TSSource &Src, std::vector<TextEdit> &Edits); // Rust mut 除去編集の収集関数
	static void CollectVarToValEdits(const TSSource &Src, std::vector<TextEdit> &Edits); // var から val への編集収集関数
	static void CollectLetToConstEdits(const TSSource &Src, std::vector<TextEdit> &Edits); // let から const への編集収集関数
	static void CollectRubyExplicitReturnEdits(const TSSource &Src, std::vector<TextEdit> &Edits); // Ruby 明示 return 編集の収集関数
	static void CollectRubySemicolonEdits(const TSSource &Src, std::vector<TextEdit> &Edits); // Ruby セミコロン編集の収集関数

	static void CollectRubyModifierFormEdits(
		TSSource &Src,
		std::vector<TextEdit> &Edits,
		std::vector<DeclEdit::PostEditAttach> &PostAttaches
	); // Ruby 修飾子形式化編集の収集関数

	static void CollectNamedImportSortEdits(
		TSSource &Src,
		std::vector<TextEdit> &Edits,
		std::vector<DeclEdit::PostEditAttach> &PostAttaches
	); // TS/JS 名前付 import 整列編集の収集関数

	static bool IsDeclSplitScope(const TSNode Container); // 宣言分離スコープ判定関数

	static bool CollectVarDeclMergeEditsImpl(
		TSSource &Src,
		const Lang Language,
		std::vector<TextEdit> &MergeEdits,
		std::vector<DeclEdit::PostEditAttach> &MergePostAttaches
	); // 宣言マージ本体走査の編集収集関数

	static void RunVarDeclSplitAndRemerge(TSSource &Src, const Lang Language, const bool HadMergeEdit); // 宣言の分割・再統合関数（本体走査と初回の再紐付後に呼ぶ）

public:

	static void ApplyLineContinuations(TSSource &Src); // Python の行継続の結合関数
	static void ApplyRubyCommandArguments(TSSource &Src); // Ruby の曖昧な実引数の読みの統一関数
	static void ApplyKotlinJoints(TSSource &Src); // Kotlin の改行を跨ぐ継ぎ目の区切り関数
	static void ApplyKotlinSemicolons(TSSource &Src); // Kotlin の自動の `;` の補修関数
	static void ApplyJsJoints(TSSource &Src); // JS/TS の改行を跨ぐ継ぎ目の読みの統一関数
	static void ApplyCSharpMisreadCasts(TSSource &Src); // C# のキャストと誤読した二項式の読みの統一関数
	static bool ApplySwiftMisreads(TSSource &Src); // Swift の解析器の読違の補正関数
	static std::string PrepareCSource(const std::string &Source, char &Marker); // C / C++ の行継続と前処理指令の整理関数
	static void RestoreLinkageGuards(std::string &Text, const char Marker); // 印へ置き換えた `extern "C"` の波括弧の復元関数
	static bool IsStmtBlockType(const TSNode Node, const Lang Language); // 文ブロック型かの判定関数
	static bool ApplyBraceEdits(TSSource &Src, const Lang Language); // 行幅に依らない波括弧編集の適用関数
	static void ApplyConsolidated(TSSource &Src, const Lang Language); // 統合編集の適用関数
	static bool ApplyRubyModifierForm(TSSource &Src, const Lang Language); // Ruby 修飾子形式への変換の適用関数（変換の有無を返す）
	static void ApplyNamedImportSort(TSSource &Src, const Lang Language); // TS/JS の名前付 import 指定子の整列の適用関数
	static void ApplyImmutableQualifiers(TSSource &Src, const Lang Language); // 不変修飾 (`const` / `final`) の付与の適用関数
	static void ApplyVarDeclMerge(TSSource &Src, const Lang Language); // 宣言マージの適用関数
	static void ApplyDocComment(TSSource &Src, const Lang Language); // ドキュメントコメントの適用関数

	EditPass() = delete; // コンストラクタ（禁止）
};
