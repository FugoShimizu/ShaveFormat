#pragma once

#include "CommentRepository.hpp"
#include "OwnedInputRoutes.hpp"
#include "SyntaxModel.hpp"
#include "ValidatedOutput.hpp"
#include "../Util/ParsedTree.hpp"
#include "../Util/ValidationContext.hpp"
#include <tree_sitter/api.h>
#include <array>
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
		SyntaxModel Syntax; // 独自のノード表
		CommentRepository Comments; // アンカーのノード毎のコメント
		ValidationContext Validation; // 検証側のデータ
		LintPreparation PrepareLint {}; // 静的解析で初めて必要な事実の生成処理
	};

	// 構文・コメント・検証データの分離クラス（整形仕様 3.1）
	class SourceProjector {
	private:

		friend struct SourceProjectorAccess;

		using ParsedChild = ::ParsedChild; // 解析結果で共有する直接子の情報

		// 標準のディレクティブコメントを判定する頭部の位置（整形仕様 4.5, 7.5）
		struct DirectiveHead {
			uint32_t HeaderEnd = 0; // 頭部の指示を読む文法では最初のコード迄，其の他は０
			uint32_t EncodingEnd = 0; // 処理系がエンコーディング宣言を読む範囲の終わり
			uint32_t FirstLineEnd = 0; // １行目の終わり
			bool IsSecondLineEncoding = false; // Python の２行目にエンコーディング宣言が在るか（１行目のコメントが成立条件と為る）
			std::vector<std::pair<uint32_t, uint32_t>> ExampleOutputs; // Go の Example 関数の出力の注記のグループの始まりから本体の終わり迄の範囲（位置順）
		};

		// パーサーの読違は投影の作業中だけ持ち，意味上の境界と役割へ変換する
		struct ReadingFact {
			bool IsCall : 1; // ジェネリック呼出を比較と誤読したか
			bool IsComparison : 1; // 比較を型引数と誤読したか
			bool IsProduct : 1; // 乗算を型と誤読したか
			bool IsLeadArgParen : 1; // 外すと型引数リストが比較と読まれる最初の型引数の括弧か
			bool HasCallContext : 1; // 祖先の呼出の所属を確定済か
			bool IsInCallArguments : 1; // 先頭の子の経路を介して誤読した呼出に属するか

			ReadingFact() noexcept; // コンストラクタ（判定と所属を未取得にする）
		};

		// 隠れたトークンの分割と照会の観測値
		struct HiddenWork {
			size_t AdjacencyQueries = 0; // 配置用の隣接条件を照会した回数
			size_t ArgTextReads = 0; // 実引数の保持判定で記録済の直接子を読んだ数
			size_t BoolQueries = 0; // 使用位置の真偽値名を照会した回数
			size_t Bytes = 0; // 分割した範囲の総バイト数
			size_t CtxVisits = 0; // 投影後に破損を分類するノードの訪問数
			std::array<size_t, 4> Docs {}; // 名前取得・種別の表示・表示の照会・論理行の観測値
			size_t EdgeReads = 0; // 最初のトークンを継承する為に読んだ子の数
			size_t KeptCopies = 0; // 保持する区切記法の照合でテキストを合成した回数
			size_t Queries = 0; // 非空の間隙の照会数
			size_t ReadingQueries = 0; // 種別を絞った後に読違の判定又は控えを照会した回数
			size_t SpaceReads = 0; // 短い間隙の空白を照会した回数
			size_t Splits = 0; // 分割の実行数
			size_t TokenSearches = 0; // 空白だけと確定せず共有トークン表を検索した回数
		};

		// 子の間のテキストを読む時に除く範囲と位置（投影の最初に１度作って共有する）
		struct GapExclusions {
			std::vector<std::pair<uint32_t, uint32_t>> CommentRanges; // 原文のコメントの範囲（原文順）
			std::vector<uint32_t> SyntheticPositions; // 解析用の括弧の位置（昇順）
			std::vector<std::pair<uint32_t, uint32_t>> Tokens {}; // 本文を１度だけ分割したトークンの範囲
			std::vector<std::pair<uint32_t, uint32_t>> Buffer {}; // 端を切る照会とコメント内の照会の再使用する作業領域
			HiddenWork *Work = nullptr; // 分割と照会の観測用計数
		};

		// 投影の間だけ持つコメントの分離の結果（入力の解析結果を書き換えずに投影が所有する，整形仕様 3.4, 7.1）
		struct CommentSeparation {
			// 分離したコメントを除く原文の範囲と，除いた跡へ補う区切（投影は原文のテキストを此の範囲を除いて読む）
			struct Range {
				uint32_t Begin; // 除く範囲の始まり
				uint32_t End; // 除く範囲の終わり
				std::string_view Separator; // 跡へ補う区切（トークンの融合・文の境界を保つ空白・改行・固定の区切記法，要らなければ空）
				bool IsNotation; // 跡へ補う区切が固定の区切記法か
			};

			DirectiveHead Head; // コメントの分離とディレクティブの照合で共有する頭部
			std::unordered_map<const void *, std::vector<CommentRecord>> LeadingByNode; // アンカーのノード毎の先行コメント
			std::unordered_map<const void *, std::vector<CommentRecord>> TrailingByNode; // アンカーのノード毎の末尾コメント
			std::vector<uint8_t> AttachmentsByRelation; // 前順の記録毎の紐付先（先行は１，末尾は２，抽出後は過大側に保つ）
			bool HasUnindexedAttachments = false; // 記録番号の無いアンカーが在り，識別子の表で照会する必要が有るか
			std::set<uint32_t> AttachedStartBytes; // 紐付けたアンカーの始まりの昇順の集合（部分木の枝刈専用）
			std::vector<Range> Separations; // 分離したコメントを除く範囲（原文順，重ならない）
			std::vector<uint32_t> SeparatedStarts; // 分離したコメントのノードの始まり（昇順）
			std::vector<std::pair<uint32_t, uint32_t>> RecordRanges; // 原文順の記録毎の修復前の範囲（投影の間だけ所有）
			std::vector<ValidationContext::LeadingSeparator> LeadingSeparators; // 行末の各記号の論理行と前置区切（原文順，整形仕様 8.1）

			static const std::vector<CommentRecord> &FindAttached(
				const std::unordered_map<const void *, std::vector<CommentRecord>> &Map,
				const TSNode Node,
				const bool ShouldFind = true
			); // アンカー毎のコメントの検索関数

			const std::vector<CommentRecord> &GetLeading(const TSNode Node) const; // 先行コメント取得関数
			const std::vector<CommentRecord> &GetLeading(const ParsedChild &Node) const; // 記録番号で未紐付の表の検索を省く先行コメント取得関数
			const std::vector<CommentRecord> &GetTrailing(const TSNode Node) const; // 末尾コメント取得関数
			const std::vector<CommentRecord> &GetTrailing(const ParsedChild &Node) const; // 記録番号で未紐付の表の検索を省く末尾コメント取得関数
			const Range *FindSeparation(const uint32_t Pos) const; // 位置を含む分離したコメントの除く範囲の取得関数
			bool HasAttachedCommentInRange(const uint32_t From, const uint32_t To) const; // 範囲内紐付コメント有無の判定関数
			bool IsSeparatedComment(const ::ParsedTree &SrcTree, const TSNode Node) const; // 分離したコメントのノードかの判定関数
			bool IsSeparatedAt(const uint32_t Pos) const; // 分離したコメントを除く範囲の中の位置かの判定関数
			bool IsNotationSeparated(const TSNode Comment) const; // 除いた跡へ固定の区切記法を置く分離したコメントかの判定関数

			template<class Append> std::string JoinFragments(
				const uint32_t From,
				const uint32_t To,
				Append &&Fragment,
				std::vector<std::pair<uint32_t, uint32_t>> *const Notations
			) const; // 分離したコメントを除いた範囲のテキストの合成関数

			std::string BuildSeparatedText(
				const std::string &Src,
				const uint32_t From,
				const uint32_t To,
				std::vector<std::pair<uint32_t, uint32_t>> *const Notations = nullptr,
				size_t *const Work = nullptr
			) const; // 分離したコメントを除いた範囲のテキストの取得関数

			std::string GetDecodedText(
				const ::ParsedTree &SrcTree,
				const uint32_t From,
				const uint32_t To,
				size_t *const Work = nullptr
			) const; // 分離したコメントを除き断片毎にデコードした範囲のテキストの取得関数

			void Attach(const ::ParsedTree &Src, const TSNode Node, const bool IsLeading, CommentRecord &&Comment); // アンカーへのコメントの紐付関数
		};

		// 投影したノードの原文の出所（文法が子に出さない隠れたトークンはパーサーのノードを持たず，原文の範囲だけを持つ）
		struct Origin {
			uint32_t NodeIdx; // 検証列又は例外列のノードの位置（例外列は `ExtraNodeBit` を足した値，隠れたトークンは `NoNode`）
			uint32_t Start; // 原文の開始位置
			uint32_t End; // 原文の終了位置
		};

		// 外部の値の列と，同じ解析木の記録への参照列を，コピーせず同じ取得口で読む
		class ParsedNodes {
		private:

			std::span<const TSNode> Nodes; // 呼出側が所有する値の列
			std::span<const TSNode *const> References; // 解析木又は別の解析文脈が所有するノードの参照列

		public:

			size_t GetSize() const; // ノード数の取得関数
			TSNode operator[](const size_t Idx) const; // 元の解析文脈を保つノードの取得関数

			explicit ParsedNodes(const std::span<const TSNode> Nodes); // 外部の値の列を参照する生成関数
			ParsedNodes(const std::vector<const TSNode *> &References); // 解析木の記録の参照列からの生成関数
		};

		static constexpr uint8_t MisreadTemplate = 1; // 比較を型引数と誤読した実体化・型引数リスト（山括弧の読みの控えの値）
		static constexpr uint8_t KeptLeadParen = 2; // 外すと比較と読まれる最初の型引数の括弧（山括弧の読みの控えの値）
		static constexpr uint32_t ExtraNodeBit = 0X80000000U; // 出所の位置が検証列でなく例外列を指すマーカー

		static void ProjectReadings(
			SyntaxModel &Syntax,
			const ReadingRules &Reading,
			const std::span<const uint16_t> FirstChars,
			const bool HasReadingCalls,
			std::vector<ReadingFact> &Readings
		); // パーサーの読違を，実引数・値の並びと意味を保つ境界へ変換する関数

		static bool IsCodeTripleReadSuccessful(const ::ParsedTree &SrcTree, const TSNode Node, std::array<TSNode, 3> &Children); // コメント以外の３つの子を取得する関数
		static std::array<TSNode, 3> GetDirectiveParts(const ::ParsedTree &Src, const TSNode Node); // ディレクティブの受手・名前・型の取得関数
		static std::string DirectiveLabel(const ::ParsedTree &Src, const TSNode Node, const std::array<TSNode, 3> &TargetParts); // ディレクティブの適用先の種類とテキストの表示関数
		static bool IsOutputComment(const DirectiveHead &Head, const uint32_t TokenStart); // Go の Example 関数の出力の注記かの判定関数

		static bool IsToolchainDirective(
			const ::ParsedTree &SrcTree,
			const DirectiveHead &Head,
			const std::string_view Raw,
			const uint32_t TokenStart,
			const CommentNotation Notation,
			const TSNode Next
		); // Go の公式ツールチェーンのディレクティブコメントかの判定関数

		static std::string DirectiveScope(const ::ParsedTree &SrcTree, const TSNode Node); // ディレクティブの所有範囲の識別の取得関数

		static bool IsStandardDirective(
			const ::ParsedTree &SrcTree,
			const ReadingRules &Reading,
			const DirectiveHead &Head,
			const std::string_view Raw,
			const uint32_t TokenStart,
			const CommentNotation Notation,
			const TSNode Next
		); // 標準のディレクティブコメントかの判定関数

		static bool IsRefDirective(const ReadingRules &Reading, std::string_view Raw); // 三重スラッシュディレクティブの形かの判定関数
		static bool IsEncodingDirective(const DirectiveHead &Head, const bool IsEncoding, const uint32_t TokenStart); // 処理系が読む位置のエンコーディング宣言かの判定関数

		static std::optional<MagicCommentScope> MagicCommentScopeOf(
			const ReadingRules &Reading,
			const std::vector<MagicCommentPair> &Pairs
		); // 実行時設定のコメントの読まれる範囲の取得関数

		static bool IsCommentLine(const std::string_view Text, const uint32_t TokenStart); // コメントだけの行に在るかの判定関数
		static std::string DedentedBody(const std::string_view Raw, const std::string_view Opener, const DirectiveDefinitionId Def); // 本文の各行の共通のインデントを除いた形の取得関数
		static std::vector<std::pair<uint32_t, uint32_t>> ExampleOutputRanges(const ::ParsedTree &SrcTree); // Go の Example 関数の出力の注記の範囲の収集関数

		static void CollectHiddenTokens(
			const ::ParsedTree &SrcTree,
			const uint32_t From,
			const uint32_t To,
			const GapExclusions &Gaps,
			std::vector<std::pair<uint32_t, uint32_t>> &Tokens
		); // 空白・コメント・解析用の括弧を除くトークンの範囲の収集関数

		static std::span<const std::pair<uint32_t, uint32_t>> HiddenTokens(
			const ::ParsedTree &SrcTree,
			const uint32_t From,
			const uint32_t To,
			GapExclusions &Gaps
		); // 子の間の隠れたトークンの範囲の取得関数

		static const KindDef *GetHiddenLexemeKind(const ::ParsedTree &SrcTree, const TSNode Host); // 文法が子に出さない語の種別の取得関数
		static bool IsMisparsedGenericCall(const ::ParsedTree &SrcTree, const TSNode Node); // 比較と誤読したジェネリック呼出の判定関数
		static bool HasLopsidedOperator(const ::ParsedTree &SrcTree, const std::span<const ParsedChild> Children); // 片側空白の演算子の判定関数

		static uint32_t CountCodeChildren(
			const ::ParsedTree &SrcTree,
			const CommentSeparation &Separated,
			const TSNode Node,
			const bool IsNamedOnly,
			TSNode *const First
		); // アンカーへ紐付けたコメントを除いた子の数の取得関数

		static std::string GetCssUrlArgsText(const std::string_view Args); // CSS の `url(...)` の引数の並びのテキストの取得関数
		static TSNode SrcNode(const Origin &Src, const ParsedNodes ValidationNodes, const std::span<const TSNode> ExtraNodes); // 出所のパーサーノードの取得関数
		static std::string DirectiveTarget(const ::ParsedTree &SrcTree, const TSNode Decl, const uint32_t Ordinal); // 適用先の識別の取得関数

		static std::optional<DirectiveInfo> DirectiveOf(
			const ::ParsedTree &SrcTree,
			const ReadingRules &Reading,
			const DirectiveHead &Head,
			const std::string_view Raw,
			const uint32_t TokenStart,
			const CommentNotation Notation,
			const TSNode Next
		); // 登録済のディレクティブ記法の記録の取得関数

		static DirectiveHead GetDirectiveHeadOf(const ::ParsedTree &SrcTree); // 標準のディレクティブコメントを判定する頭部の位置の取得関数
		static std::unordered_map<const void *, uint8_t> PrepareTemplateContexts(const ::ParsedTree &SrcTree); // 比較を型引数と誤読した山括弧の一括判定関数
		static bool IsPreprocDirectiveAt(const std::string_view Raw, const size_t Pos); // プリプロセッサディレクティブかの判定関数

		static bool HasOpaqueHidden(
			const ::ParsedTree &SrcTree,
			const TSNode Host,
			const uint32_t From,
			const uint32_t To,
			GapExclusions &Gaps
		); // トークンにしない隠れたテキストを持つ間隙かの判定関数

		static uint8_t GetTemplateReading(const std::unordered_map<const void *, uint8_t> &Misreads, const TSNode Node); // 山括弧の読みの控えの取得関数
		static bool IsMisparsedGenericOpener(const ::ParsedTree &SrcTree, const TSNode Node); // 一覧の要素へ分けて誤読したジェネリック呼出の型引数の開きの判定関数
		static bool IsCSharpProduct(const ::ParsedTree &SrcTree, const TSNode PtrType); // C# の宣言式と誤読した乗算かの判定関数

		static std::optional<std::string> CommentFreeGap(
			const ::ParsedTree &SrcTree,
			const CommentSeparation &Separated,
			const uint32_t From,
			const uint32_t To,
			const std::vector<std::pair<uint32_t, uint32_t>> &CommentRanges
		); // コメントを除いた間隙の取得関数

		static bool ShouldPreserveAdjacency(const SyntaxModel &Syntax, const NodeId Parent, const NodeId Next); // 原文の隣接が読みを決める境界かの判定関数
		static void RecordGoBindingFacts(SyntaxModel &Syntax); // Go の短い宣言の既存束縛への代入の記録関数
		static bool HasDecodedIdentifierEscape(const ReadingRules &Reading, const std::string_view Name, size_t &Pos, char32_t &Point); // 識別子の中の Unicode のエスケープのデコード関数
		static bool IsFormatChar(const char32_t Point); // 識別子の書式文字の判定関数
		static bool IsDeclaratorPart(const SyntaxModel &Syntax, const NodeId Node); // 宣言子毎に名前を持つ宣言の部分かの判定関数
		static bool HasEmptyFinalBody(const ::ParsedTree &Src, TSNode Node); // 構文の末尾が空の波括弧の本体かの判定関数

		static std::pair<TSNode, TSNode> SignedDimensionParts(
			const ::ParsedTree &SrcTree,
			const CommentSeparation &Separated,
			const TSNode Node
		); // 空の左辺として誤読された数値の符号と被演算子の取得関数

		static bool IsVerbatimNode(
			const ::ParsedTree &SrcTree,
			const CommentSeparation &Separated,
			const TSNode Node,
			const RoleSet &Roles,
			const std::span<const ParsedChild> Children,
			const bool HasFilePreamble,
			HiddenWork *const Observation = nullptr
		); // 子へ降りずテキストを保つノードかの判定関数

		static std::string GetVerbatimText(const ::ParsedTree &SrcTree, const CommentSeparation &Separated, const TSNode Node); // テキストを保つノードのテキストの取得関数
		static bool IsTokenizedHidden(const ::ParsedTree &SrcTree, const TSNode Host, const std::string_view Text); // トークンのノードにする隠れたテキストかの判定関数

		static void RebindParenthesizedDecls(
			const ::ParsedTree &SrcTree,
			const GrammarProfile &Profile,
			const ParsedNodes ValidationNodes,
			const std::span<const TSNode> ExtraNodes,
			const bool IsVerification,
			std::vector<Origin> &Sources,
			ProjectionResult &Result
		); // 呼出又は其れへの代入と読まれた括弧付宣言子の投影の修復関数

		static std::vector<std::string> CollectDirectives(
			const ::ParsedTree &SrcTree,
			const DirectiveHead &Head,
			std::vector<uint32_t> *const Starts,
			StructureWorkAccount *const Work = nullptr
		); // 分離時の頭部を使うディレクティブコメントの取得関数

		static void RecordAngleParens(SyntaxModel &Syntax, const std::vector<ReadingFact> &Readings); // 型引数の中の比較・シフトの括弧の保持条件の記録関数

		static void RecordFacts(
			const ::ParsedTree &SrcTree,
			SyntaxModel &Syntax,
			const GrammarProfile &Profile,
			const CommentSeparation &Separated,
			const std::vector<Origin> &Sources,
			const std::span<const TSSymbol> Symbols,
			const ParsedNodes ValidationNodes,
			const std::span<const TSNode> ExtraNodes,
			const std::unordered_set<const void *> &BrokenMissingParents,
			const std::unordered_set<const void *> &BrokenSubtrees,
			const std::vector<uint8_t> &SeparatedBefore,
			const bool IsVerification,
			const bool HasFilePreamble,
			GapExclusions &Gaps,
			StructureWorkAccount *const Work = nullptr
		); // 構文事実の記録関数

		static void RecordMarkupFacts(
			const ::ParsedTree &SrcTree,
			SyntaxModel &Syntax,
			const std::vector<Origin> &Sources,
			const bool HasFilePreamble
		); // ドキュメントの表示境界の投影関数

		static void RecordOwnedRegions(
			const ::ParsedTree &SrcTree,
			const std::vector<Origin> &Sources,
			const ParsedNodes ValidationNodes,
			const std::span<const TSNode> ExtraNodes,
			const OwnedInputRoutes &Owned,
			ProjectionResult &Result
		); // 子の所有領域と３経路の投影関数

		static void RebindDeferredLiteralBodies(SyntaxModel &Syntax, const std::vector<Origin> &Sources); // 閉括弧の内側へ読まれた逐語本文を文の後へ移す関数
		static void RebindEmbeddedBranches(SyntaxModel &Syntax, const ReadingRules &Reading, const bool IsVerification); // 埋込コードと枝の所属の修復関数
		static void RebindBinaryOperators(SyntaxModel &Syntax, const ReadingRules &Reading, std::vector<Origin> &Sources); // 誤読した二項演算子の連鎖を優先順位で結び直す関数
		static void RebindSwiftPostfix(SyntaxModel &Syntax); // Swift の後置の式の結付の修復関数
		static void RecordImportFields(SyntaxModel &Syntax, const bool NeedsBindings); // 関数インポートの対象・別名と名前空間の投影関数
		static void RecordNames(SyntaxModel &Syntax, const ReadingRules &Reading, const bool NeedsSpelling, const bool NeedsBindings); // 比較名と必要な綴りの事実の記録関数
		static void RecordMemberRoles(const GrammarProfile &Profile, SyntaxModel &Syntax, const bool ForModifiers); // 確認済の標準対象の効果を役割へ投影する関数
		static void RecordImportScopes(SyntaxModel &Syntax); // 関数インポートの名前空間の所属だけを記録する関数

		static LintPreparation DeferLintFacts(
			const ::ParsedTree &SrcTree,
			std::vector<Origin> Sources,
			const std::vector<std::pair<uint32_t, uint32_t>> &CommentRanges,
			std::vector<uint32_t> DirectiveStarts
		); // 投影済の出所を所有して静的解析用の事実を遅延生成する関数

		static void RecordBindingFields(SyntaxModel &Syntax, const NodeId Id); // 束縛構造から子のフィールドを補う関数
		static std::string GetCmpNameOf(const ReadingRules &Reading, std::string_view Name, const bool IsLabel); // 識別子の比較用の名前の取得関数

		static std::vector<std::pair<size_t, size_t>> LabelTargets(
			const SyntaxModel &Syntax,
			const std::span<const std::pair<NodeId, NodeId>> Entries,
			ValidationContext::DocStorage &Out,
			std::array<size_t, 4> *const Work
		); // 説明対象の識別の組立関数

		static void RestoreEmbeddedDoc(std::string &Text, const unsigned char Marker, StructureWorkAccount *Work = nullptr); // マーカー除去

		static CommentSeparation SeparateComments(
			const ::ParsedTree &SrcTree,
			const ParseInput &Input,
			StructureWorkAccount *const Work = nullptr,
			std::array<size_t, 3> *const Observation = nullptr
		); // 原文のコメント分離関数（任意の観測用計数は構造の予算から独立）

		static std::vector<ValidationContext::ErrorZone> CollectErrorZones(
			const ::ParsedTree &SrcTree,
			const ParsedNodes Nodes,
			const std::vector<TSNode> &Errors,
			std::vector<std::pair<uint32_t, uint32_t>> *const ProtectedRanges
		); // 構文エラーの周りのテキストの収集関数

		static std::vector<ValidationContext::SemanticCheck> CollectSemanticChecks(
			const ::ParsedTree &SrcTree,
			const std::unordered_set<const void *> *const UnrepairedSubtrees
		); // 意味上の区別の収集関数

		static ProjectionResult ProjectNodes(
			const ::ParsedTree &SrcTree,
			const GrammarProfile &Profile,
			const ParseInput &Input,
			const OwnedInputRoutes &Owned,
			CommentSeparation &Separated,
			const bool NeedsPositions = true,
			StructureWorkAccount *const Work = nullptr,
			HiddenWork *const Hidden = nullptr
		); // コメントを分離した解析結果のノード表への投影関数

		static ValidationContext CollectCtxFromNodes(
			const ::ParsedTree &SrcTree,
			const GrammarProfile &Profile,
			const ParseInput &Input,
			const ParsedNodes Nodes,
			const std::span<const std::pair<uint32_t, uint32_t>> TemplateDecls,
			std::unordered_set<const void *> *const BrokenMissingParents,
			std::unordered_set<const void *> *const BrokenSubtrees,
			std::vector<std::pair<uint32_t, uint32_t>> *const ErrorRanges,
			std::vector<std::pair<uint32_t, uint32_t>> *const CommentRanges,
			HiddenWork *const Observation
		); // 同じ木のノードの参照列からの検証データの組立関数

		static void RetainValidationInput(const GrammarProfile &Profile, ProjectionResult &Result); // 解析用の入力を手放した後の検証ビューの寿命の確保関数

	public:

		static std::vector<std::string> CollectDirectives(
			const ::ParsedTree &SrcTree,
			std::vector<uint32_t> *const Starts = nullptr,
			StructureWorkAccount *const Work = nullptr
		); // 共有するコメントの事実からの標準のディレクティブコメントの取得関数

		static std::vector<ValidationContext::DocFact> DescribeDocs(
			const SyntaxModel &Syntax,
			const std::span<const CommentRecord> Records,
			const std::span<const NodeId> Targets,
			std::array<size_t, 4> *const Work = nullptr
		); // ドキュメントの説明対象と構造の取得関数

		static void AppendCode(
			const ::ParsedTree &SrcTree,
			uint32_t Start,
			const uint32_t End,
			const std::vector<std::pair<uint32_t, uint32_t>> &Comments,
			std::string &Output
		); // 空白を除きコメントをマーカーとしたテキストの追記関数

		static std::vector<uint32_t> CountZones(const std::string_view Code, const std::vector<std::string_view> &Texts); // テキストの出現数の一括計数関数

		static ValidationContext CollectCtx(
			const ::ParsedTree &SrcTree,
			const GrammarProfile &Profile,
			const ParseInput &Input,
			const std::span<const TSNode> Nodes,
			std::unordered_set<const void *> *const BrokenMissingParents = nullptr,
			std::unordered_set<const void *> *const BrokenSubtrees = nullptr,
			std::vector<std::pair<uint32_t, uint32_t>> *const ErrorRanges = nullptr,
			std::vector<std::pair<uint32_t, uint32_t>> *const CommentRanges = nullptr,
			HiddenWork *const Observation = nullptr,
			const std::span<const std::pair<uint32_t, uint32_t>> TemplateDecls = std::span<const std::pair<uint32_t, uint32_t>>()
		); // 投影の走査からの検証データの組立関数

		static ProjectionResult Project(
			const ::ParsedTree &SrcTree,
			const GrammarProfile &Profile,
			const ParseInput &Input,
			const bool NeedsPositions = true
		); // 構文・コメント・検証データへの投影関数

		SourceProjector() = delete; // コンストラクタ（禁止）
	};
} // namespace Core
