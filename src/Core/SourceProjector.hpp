#pragma once

#include "../Util/ParsedTree.hpp"
#include "../Util/ValidationContext.hpp"
#include "CommentRepository.hpp"
#include "SyntaxModel.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class GrammarProfile;

struct ParseInput;
struct ReadingRules;

enum class MagicCommentScope : uint8_t;

namespace Core {
	// 投影の結果（整形仕様 3.3，呼出側が所有物を受け取る為の結果で，中核へ結果全体を渡さない）
	struct ProjectionResult {
		SyntaxModel Syntax; // 独自の節点表
		CommentRepository Comments; // 錨の節点毎のコメント
		ValidationContext Validation; // 検証側の資料
	};

	// 構文・コメント・検証資料の分離クラス（整形仕様 3.1）
	class SourceProjector {
	private:

		// 投影した節点の原文の出所（文法が子に出さない隠れた字句は解析器の節点を持たず，原文の範囲だけを持つ）
		struct Origin {
			uint32_t NodeIndex; // 検証列又は例外列の節点の位置（例外列は `ExtraNodeBit` を足した値，隠れた字句は `NoNode`）
			uint32_t Start; // 原文の開始位置
			uint32_t End; // 原文の終了位置
		};

		// 標準の指示コメントを判定する頭部の位置（整形仕様 4.5, 7.5）
		struct DirectiveHead {
			uint32_t HeaderEnd = 0; // 頭部の指示を読む文法では最初のコード迄，其の他は０
			uint32_t EncodingEnd = 0; // 処理系が符号化宣言を読む範囲の終わり
			uint32_t FirstLineEnd = 0; // １行目の終わり
			bool IsSecondLineEncoding = false; // Python の２行目に符号化宣言が在るか（１行目のコメントが成立条件と為る）
			std::vector<std::pair<uint32_t, uint32_t>> ExampleOutputs; // Go の Example 関数の出力の注記の群の始まりから本体の終わり迄の範囲（位置順）
		};

		// 型引数を持つ型の宣言と確定する名前の束縛（整形仕様 4.2，名前空間の範囲の名前だけを控え，内側の範囲の名前は隠し得る名前とする）
		struct TemplateBindings {
			std::unordered_map<std::string, uint32_t> Templates; // 名前空間の範囲で宣言したクラス・別名のテンプレートの名前（名前空間の名前と `::` で繋ぐ）毎の最も前の宣言の位置
			std::unordered_set<std::string> Namespaces; // 最も外の範囲で宣言した名前空間の名前
			std::unordered_set<std::string> Shadows; // テンプレート・名前空間の名前を隠し得る名前（識別子・成員名，其の他の型・内側又は無名の名前空間の中の名前空間・名前空間の別名・型の別名・型引数の宣言名）
			bool IsOpen = false; // 他の範囲の名前を取り込む using の宣言又は指令を持つか
		};

		// 子の間の字面を読む時に除く範囲と位置（投影の最初に一度作って共有する）
		struct GapExclusions {
			std::vector<std::pair<uint32_t, uint32_t>> CommentRanges; // 原文のコメントの範囲（原文順）
			std::vector<uint32_t> SyntheticPositions; // 解析用の括弧の位置（昇順）
		};

		// 投影の間だけ持つコメントの分離の結果（入力の解析結果を書き換えずに投影が所有する，整形仕様 3.4, 7.1）
		struct CommentSeparation {
			// 分離したコメントを除く原文の範囲と，除いた跡へ補う区切（投影は原文の字面を此の範囲を除いて読む）
			struct Range {
				uint32_t Begin; // 除く範囲の始まり
				uint32_t End; // 除く範囲の終わり
				std::string_view Separator; // 跡へ補う区切（字句の融合・文の境界を保つ空白・改行・固定の区切記法，要らなければ空）
				bool IsNotation; // 跡へ補う区切が固定の区切記法か
			};

			DirectiveHead Head; // コメントの分離と指示子の照合で共有する頭部
			std::unordered_map<const void *, std::vector<CommentRecord>> LeadingByNode; // 錨の節点毎の先行コメント
			std::unordered_map<const void *, std::vector<CommentRecord>> TrailingByNode; // 錨の節点毎の末尾コメント
			std::set<uint32_t> AttachedStartBytes; // 紐付けた錨の始まりの昇順の集合（部分木の枝刈り専用）
			std::vector<Range> Separations; // 分離したコメントを除く範囲（原文順，重ならない）
			std::vector<uint32_t> SeparatedStarts; // 分離したコメントの節点の始まり（昇順）
			std::vector<ValidationContext::LeadingSeparator> LeadingSeparators; // 行末の各記号の論理行と前置区切り（原文順，整形仕様 8.1）

			static const std::vector<CommentRecord> &FindAttached(
				const std::unordered_map<const void *, std::vector<CommentRecord>> &Map,
				const TSNode Node
			); // 錨毎のコメントの検索関数

			const Range *FindSeparation(const uint32_t Position) const; // 位置を含む分離したコメントの除く範囲の取得関数
			const std::vector<CommentRecord> &GetLeading(const TSNode Node) const; // 先行コメント取得関数
			const std::vector<CommentRecord> &GetTrailing(const TSNode Node) const; // 後続コメント取得関数
			bool HasAttachedCommentInRange(const uint32_t From, const uint32_t To) const; // 範囲内紐付コメント有無の判定関数
			bool IsSeparatedComment(const ::ParsedTree &Src, const TSNode Node) const; // 分離したコメントの節点かの判定関数
			bool IsSeparatedAt(const uint32_t Position) const; // 分離したコメントを除く範囲の中の位置かの判定関数
			bool IsNotationSeparated(const TSNode Comment) const; // 除いた跡へ固定の区切記法を置く分離したコメントかの判定関数

			template<class Append> std::string JoinFragments(
				const uint32_t From,
				const uint32_t To,
				Append &&Fragment,
				std::vector<std::pair<uint32_t, uint32_t>> *const Notations
			) const; // 分離したコメントを除いた範囲の字面の合成関数

			std::string SeparatedText(
				const std::string &Source,
				const uint32_t From,
				const uint32_t To,
				std::vector<std::pair<uint32_t, uint32_t>> *const Notations = nullptr
			) const; // 分離したコメントを除いた範囲の字面の取得関数

			std::string DecodedText(const ::ParsedTree &Src, const uint32_t From, const uint32_t To) const; // 分離したコメントを除き断片毎に復号した範囲の字面の取得関数
			void Attach(const TSNode Node, const bool IsLeading, CommentRecord &&Comment); // 錨へのコメントの紐付関数
		};

		static constexpr uint32_t ExtraNodeBit = 0X80000000U; // 出所の位置が検証列でなく例外列を指す印
		static bool IsReferenceDirective(const ReadingRules &Reading, std::string_view Raw); // 三重スラッシュ指示子の形かの判定関数

		static std::string DedentedBody(
			const std::string_view Raw,
			const std::string_view Opener,
			const DirectiveDefinitionId Definition
		); // 本文の各行の共通の字下げを除いた形の取得関数

		static std::vector<std::pair<uint32_t, uint32_t>> ExampleOutputRanges(const ::ParsedTree &Src); // Go の Example 関数の出力の注記の範囲の収集関数
		static DirectiveHead DirectiveHeadOf(const ::ParsedTree &Src); // 標準の指示コメントを判定する頭部の位置の取得関数
		static bool IsEncodingDirective(const DirectiveHead &Head, const bool IsEncoding, const uint32_t TokenStart); // 処理系が読む位置の符号化宣言かの判定関数

		static std::optional<MagicCommentScope> MagicCommentScopeOf(
			const ReadingRules &Reading,
			const std::vector<MagicCommentPair> &Pairs
		); // 実行時設定のコメントの読まれる範囲の取得関数

		static bool IsOutputComment(const DirectiveHead &Head, const uint32_t TokenStart); // Go の Example 関数の出力の注記かの判定関数

		static bool IsToolchainDirective(
			const ::ParsedTree &Src,
			const DirectiveHead &Head,
			const std::string_view Raw,
			const uint32_t TokenStart,
			const CommentNotation Notation,
			const TSNode Next
		); // Go の公式ツールチェーンの指示コメントかの判定関数

		static bool IsStandardDirective(
			const ::ParsedTree &Src,
			const ReadingRules &Reading,
			const DirectiveHead &Head,
			const std::string_view Raw,
			const uint32_t TokenStart,
			const CommentNotation Notation,
			const TSNode Next,
			const CommentRepository::RegisteredLineForm &Form
		); // 標準の指示コメントかの判定関数

		static std::optional<DirectiveInfo> DirectiveOf(
			const ::ParsedTree &Src,
			const ReadingRules &Reading,
			const DirectiveHead &Head,
			const std::string_view Raw,
			const uint32_t TokenStart,
			const CommentNotation Notation,
			const TSNode Next
		); // 登録済の指示記法の記録の取得関数

		static std::string DirectiveScope(const ::ParsedTree &Src, const TSNode Node); // 指示子の所有範囲の識別の取得関数
		static std::string DirectiveTarget(const ::ParsedTree &Src, const TSNode Declaration, const uint32_t Ordinal); // 指示子の適用先の識別の取得関数

		static std::vector<std::string> CollectDirectives(
			const ::ParsedTree &Src,
			const DirectiveHead &Head,
			const std::span<const TSNode> Nodes,
			std::vector<uint32_t> *const Starts,
			std::vector<DirectiveDefinitionId> *const Definitions
		); // 分離時の頭部を使う指示コメントの取得関数

		static bool IsDeclaratorPart(const SyntaxModel &Syntax, const NodeId Node); // 宣言子毎に名前を持つ宣言の部分かの判定関数

		static std::vector<std::string> LabelTargets(
			const SyntaxModel &Syntax,
			const std::span<const std::pair<NodeId, NodeId>> Entries
		); // 説明対象の識別の組立関数

		static CommentRepository ProjectOutputComments(
			const NotationRules &Rules,
			const std::string_view Text,
			const std::span<const std::pair<uint32_t, uint32_t>> CommentRanges,
			const std::span<const uint32_t> DirectiveStarts,
			const std::span<const DirectiveDefinitionId> Definitions,
			const ReadingRules &Reading
		); // 補修前の字面からの最終コメントの記録の投影関数

		static CommentSeparation SeparateComments(const ::ParsedTree &Src, const ParseInput &Input); // 原文のコメント分離関数
		static size_t CountUnseparatedComments(const ::ParsedTree &Src, const CommentSeparation &Separated, const uint32_t BodyEnd); // 分離されずに残るコメント節点の計数関数（経路の分離の検証用）
		static bool IsFormatCharacter(const uint32_t Point); // 識別子の書式文字の判定関数
		static bool DecodeIdentifierEscape(const ReadingRules &Reading, const std::string_view Name, size_t &Pos, uint32_t &Point); // 識別子の中の Unicode の逃避の復号関数
		static std::string ComparisonNameOf(const ReadingRules &Reading, std::string_view Name, const bool IsLabel); // 識別子の比較用の名前の取得関数
		static void RebindDeferredLiteralBodies(const std::vector<Origin> &Sources, SyntaxModel &Syntax); // 閉じ括弧の内側へ読まれた逐語本文を文の後へ移す関数
		static void RebindEmbeddedBranches(const ReadingRules &Reading, SyntaxModel &Syntax); // 埋込コードと枝の所属の補修関数
		static std::vector<TSNode> CodeChildrenOf(const ::ParsedTree &Src, const TSNode Node); // コメントを除いた子の取得関数
		static std::unordered_map<const void *, uint8_t> PrepareTemplateContexts(const ::ParsedTree &Src); // 比較を型引数と誤読した山括弧の一括判定関数
		static bool IsMisparsedTemplate(const std::unordered_map<const void *, uint8_t> &Misreads, const TSNode Node); // 比較を型引数と誤読した山括弧かの判定関数
		static bool ReadsAsCsProduct(const ::ParsedTree &Src, const TSNode PointerType); // C# の宣言式と誤読した乗算かの判定関数
		static bool IsTypeScope(const ::ParsedTree &Src, const TSNode Node); // 型を定義する容器かの判定関数
		static bool IsMisparsedGenericCall(const ::ParsedTree &Src, const TSNode Node); // 比較と誤読した総称呼出の判定関数
		static std::vector<std::string> NamesOf(const ::ParsedTree &Src, const TSNode Node); // 修飾を含む名前の並びの取得関数
		static std::optional<std::pair<TSNode, TSNode>> MisreadDeclarationParts(const ::ParsedTree &Src, const TSNode Node); // 比較と誤読した型引数を持つ型の宣言の型と宣言子の取得関数
		static TemplateBindings TemplateBindingsOf(const ::ParsedTree &Src); // 型引数を持つ型の宣言と確定する名前の束縛の取得関数

		static bool IsMisparsedTemplateDeclaration(
			const ::ParsedTree &Src,
			const TSNode Node,
			std::optional<TemplateBindings> &Bindings
		); // 比較と誤読した型引数を持つ型の宣言の判定関数

		static bool HoldsLopsidedOperator(const ::ParsedTree &Src, const TSNode Node); // 片側だけ空白の有る Swift の利用者定義演算子を持つかの判定関数
		static std::string CssUrlArgumentsText(const std::string_view Arguments); // CSS の `url(...)` の引数の並びの字面の取得関数
		static bool IsPreprocDirectiveAt(const std::string_view Raw, const size_t Pos); // 前処理指令かの判定関数

		static uint32_t CountCodeChildren(
			const ::ParsedTree &Src,
			const CommentSeparation &Separated,
			const TSNode Node,
			const bool IsNamedOnly,
			TSNode *const First
		); // 錨へ紐付けたコメントを除いた子の数の取得関数

		static bool IsVerbatimNode(const ::ParsedTree &Src, const CommentSeparation &Separated, const TSNode Node); // 子へ降りず字面を保つ節点かの判定関数
		static std::string VerbatimText(const ::ParsedTree &Src, const CommentSeparation &Separated, const TSNode Node); // 字面を保つ節点の字面の取得関数
		static bool KeepsAdjacency(const SyntaxModel &Syntax, const NodeId Parent, const NodeId Next); // 原文の隣接が読みを決める境界かの判定関数

		static std::vector<std::pair<uint32_t, uint32_t>> HiddenTokens(
			const ::ParsedTree &Src,
			const uint32_t From,
			const uint32_t To,
			const GapExclusions &Gaps
		); // 子の間の隠れた字句の範囲の取得関数

		static const KindDefinition *HiddenLexemeKind(const ::ParsedTree &Src, const TSNode Host); // 文法が子に出さない語の種別の取得関数
		static bool IsTokenizedHidden(const ::ParsedTree &Src, const TSNode Host, const std::string_view Text); // 字句の節点にする隠れた字面かの判定関数

		static bool HasOpaqueHidden(
			const ::ParsedTree &Src,
			const TSNode Host,
			const uint32_t From,
			const uint32_t To,
			const GapExclusions &Gaps
		); // 字句にしない隠れた字面を持つ間隙かの判定関数

		static std::string CommentFreeGap(
			const ::ParsedTree &Src,
			const CommentSeparation &Separated,
			const uint32_t From,
			const uint32_t To,
			const std::vector<std::pair<uint32_t, uint32_t>> &CommentRanges
		); // コメントを除いた間隙の取得関数

		static TSNode SourceNode(
			const Origin &Source,
			const std::span<const TSNode> ValidationNodes,
			const std::span<const TSNode> ExtraNodes
		); // 出所の解析器節点の取得関数

		static void RecordFacts(
			const ::ParsedTree &Src,
			const CommentSeparation &Separated,
			const GapExclusions &Gaps,
			const std::vector<Origin> &Sources,
			const std::span<const TSNode> ValidationNodes,
			const std::span<const TSNode> ExtraNodes,
			const std::vector<uint8_t> &SeparatedBefore,
			SyntaxModel &Syntax
		); // 構文事実の記録関数

		static void RecordMarkupFacts(
			const ::ParsedTree &Src,
			const std::vector<Origin> &Sources,
			const std::span<const TSNode> ValidationNodes,
			const std::span<const TSNode> ExtraNodes,
			SyntaxModel &Syntax
		); // 文書の表示境界の投影関数

		static void RecordGoBindingFacts(SyntaxModel &Syntax); // Go の短い宣言の既存束縛への代入の記録関数
		static void RebindSwiftPostfix(SyntaxModel &Syntax); // Swift の後置の式の結付の補修関数

		static std::vector<std::pair<uint32_t, uint32_t>> CollectObservedPositions(
			const ::ParsedTree &Src,
			const GrammarProfile &Profile,
			const std::string_view BeforeEdits,
			const std::span<const TSNode> Nodes
		); // 位置を観測する構文の位置の収集関数

		static std::vector<ValidationContext::ErrorZone> CollectErrorZones(
			const ::ParsedTree &Src,
			const std::span<const TSNode> Nodes,
			const std::unordered_map<const void *, TSNode> &Parents
		); // 構文誤りの周りの字面の収集関数

		static std::vector<ValidationContext::SemanticCheck> CollectSemanticChecks(
			const ::ParsedTree &Src,
			const std::span<const TSNode> Nodes
		); // 意味上の区別の収集関数

		static ProjectionResult ProjectNodes(
			const ::ParsedTree &Src,
			const GrammarProfile &Profile,
			const ParseInput &Input,
			CommentSeparation &Separated
		); // コメントを分離した解析結果の節点表への投影関数

	public:

		static std::vector<std::string> CollectDirectives(
			const ::ParsedTree &Src,
			const std::span<const TSNode> Nodes,
			std::vector<uint32_t> *const Starts = nullptr
		); // 投影で訪ねたコメントからの標準の指示コメントの取得関数

		static std::vector<ValidationContext::DocumentFact> DescribeDocuments(
			const SyntaxModel &Syntax,
			const std::span<const CommentRecord> Records,
			const std::span<const NodeId> Targets
		); // 文書の説明対象と構造の取得関数

		static void AppendCode(
			const ::ParsedTree &Src,
			uint32_t Start,
			const uint32_t End,
			const std::vector<std::pair<uint32_t, uint32_t>> &Comments,
			std::string &Out
		); // 空白を除きコメントを印とした字面の追記関数

		static std::vector<uint32_t> CountZones(const std::string_view Code, const std::vector<std::string_view> &Texts); // 字面の出現数の一括計数関数

		static ValidationContext CollectContext(
			const ::ParsedTree &Src,
			const GrammarProfile &Profile,
			const ParseInput &Input,
			const std::span<const TSNode> Nodes
		); // 投影の走査からの検証資料の組立関数

		static ProjectionResult Project(const ::ParsedTree &Src, const GrammarProfile &Profile, const ParseInput &Input); // 構文・コメント・検証資料への投影関数

		SourceProjector() = delete; // コンストラクタ（禁止）
	};
} // namespace Core
