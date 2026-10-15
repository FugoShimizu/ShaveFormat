#include "SourceProjector.hpp"
#include "CommentRepository.hpp"
#include "DocCommentReader.hpp"
#include "SyntaxModel.hpp"
#include "SyntaxRole.hpp"
#include "../Profile/GrammarProfile.hpp"
#include "../Profile/ReadingRules.hpp"
#include "../Util/CharClass.hpp"
#include "../Util/CommentNormalizer.hpp"
#include "../Util/LayoutTypes.hpp"
#include "../Util/Parallel.hpp"
#include "../Util/ParseAdapter.hpp"
#include "../Util/ParsedTree.hpp"
#include "../Util/TextEdit.hpp"
#include "../Util/ValidationContext.hpp"
#include <tree_sitter/api.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace Core {
	/**
	 * Go の Example 関数の出力の注記かの判定関数
	 * 計算量：出力注記の登録範囲数 R に対し O(log(R + 1))
	 * @param Head `GetDirectiveHeadOf` で求めた頭部の位置と出力の注記の範囲
	 * @param TokenStart コメントの始まりの位置
	 * @return 出力の注記のグループに属するコメントなら true
	 */
	bool SourceProjector::IsOutputComment(const DirectiveHead &Head, const uint32_t TokenStart) {
		// 位置以前に始まる注記グループの探索
		const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator After = std::upper_bound(
			Head.ExampleOutputs.begin(),
			Head.ExampleOutputs.end(),
			TokenStart,
			[](const uint32_t Val, const std::pair<uint32_t, uint32_t> &Range) -> bool {
				// 範囲の始まりとの比較の返戻
				return Val < Range.first;
			}
		);
		// 位置以前に始まる最後の範囲に含まれるかの返戻
		return After != Head.ExampleOutputs.begin() && TokenStart < (After - 1)->second;
	}

	/**
	 * Go の公式ツールチェーンのディレクティブコメントかの判定関数（整形仕様 4.5, 7.5）
	 * `go` コマンド・コンパイラ・cgo・embed のドキュメントが名前・記法・位置条件を定める物だけを対象とする
	 * `//go:` で始まる未知の名前は含めない
	 * 名前と区切は記法の値で識別し，此処では位置と行位置の数値条件を判ずる
	 * 計算量：前の間隙のテキスト長 L，登録注記の判定費用 Q に対し O(L + Q)
	 * @param Src 解析済のテキスト
	 * @param Head `GetDirectiveHeadOf` で求めた頭部の位置
	 * @param Raw コメント全文
	 * @param TokenStart コメントの始まりの位置
	 * @param Notation コメントの記法
	 * @param Next 同じ親の中で後に続く最初のコメントでない名前付のノード（無ければ空）
	 * @return 位置条件を満たして効いて居るディレクティブコメントなら true
	 * @throws std::bad_alloc 位置表のメモリを確保出来ない場合
	 */
	bool SourceProjector::IsToolchainDirective(
		const ::ParsedTree &Src,
		const DirectiveHead &Head,
		const std::string_view Raw,
		const uint32_t TokenStart,
		const CommentNotation Notation,
		const TSNode Next
	) {
		// `/*line` は閉じの直後のトークンの位置を定め行の何処でも効く為（コンパイラの行のディレクティブ），位置の妥当性の返戻
		if(Notation == CommentNotation::GoLineBlock) return CommentRepository::HasValidLinePos(Raw, Notation);
		const std::string_view Text = Src;
		// 行の前にコードの在る物（コンパイラは独立した行のディレクティブだけを読む）の返戻
		if(
			const size_t Before = TokenStart ? Text.find_last_not_of(" \t", TokenStart - 1) : std::string_view::npos;
			Before != std::string_view::npos && Text[Before] != '\n'
		) return false;
		switch(Notation) {
		case CommentNotation::GoLineLine:
		case CommentNotation::GoGenerateLine:
			// 行頭（桁０）の物だけを読む行のディレクティブと `go generate` のディレクティブの返戻
			return (!TokenStart || Text[TokenStart - 1] == '\n') && CommentRepository::ShouldActivateAtLineHead(Raw, Notation);
		case CommentNotation::GoBuildLine:
		case CommentNotation::GoDebugLine:
			// `package` より前の頭部で読むビルド制約・GODEBUG の設定の返戻（古い記法の `// +build` は対応せず通常のコメントとする）
			return TokenStart < Head.HeaderEnd;
		case CommentNotation::GoLinknameLine:
			// 位置に依らず効く記号名の対応付の返戻
			return true;
		case CommentNotation::GoEmbedLine:
			// 直後の変数宣言へ作用する埋込のディレクティブの返戻
			return Src.Is(Next, Role::VarDeclNode);
		case CommentNotation::CgoExportLine:
		case CommentNotation::GccgoExternLine:
		case CommentNotation::GoNoescapeLine:
		case CommentNotation::GoNoinlineLine:
		case CommentNotation::GoNoraceLine:
		case CommentNotation::GoNosplitLine:
		case CommentNotation::GoUintptrescapesLine:
		case CommentNotation::GoWasmexportLine:
		case CommentNotation::GoWasmimportLine:
			// 直後の関数宣言へ作用する cgo の公開・gccgo の外部名・コンパイラの関数のディレクティブの返戻
			return Src.Is(Next, Role::PragmaTargetFunction);
		default:
			// インポートの宣言の前だけを cgo の前文の候補とし，コメントの終わりからインポート迄に空行を挟む物を除く
			// 前文の内側の空行は数えない
			if(
				!Src.Is(Next, Role::ImportDeclNode) ||
				Src.HasBlankLineBetween(TokenStart + static_cast<uint32_t>(Raw.size()), Src.GetStart(Next))
			// cgo の前文でない事の返戻
			) return false;
			// 取り込むパッケージが cgo の `"C"` かの返戻
			return Src.CanAbsorbGoComment(Next);
		}
	}

	/**
	 * ディレクティブの適用先の受手・名前・型の取得関数
	 * 計算量：対象の子の数 C に対し O(C)
	 * @param Src 解析済のテキスト
	 * @param Node 対象のノード
	 * @return 識別に使う３つの子
	 */
	std::array<TSNode, 3> SourceProjector::GetDirectiveParts(const ::ParsedTree &Src, const TSNode Node) {
		// 適用先の名前ノードの探索
		TSNode Named = Src.GetFieldChild(Node, ChildRole::Name);
		if(ts_node_is_null(Named) && Src.Is(Node, Role::VarDeclNode)) {
			TSNode DirectiveSpec = ts_node_named_child(Node, 0);
			if(!ts_node_is_null(DirectiveSpec) && Src.Is(DirectiveSpec, Role::VarSpecList)) {
				DirectiveSpec = ts_node_named_child(DirectiveSpec, 0);
			}
			Named = ts_node_is_null(DirectiveSpec) ? TSNode{} : Src.GetFieldChild(DirectiveSpec, ChildRole::Name);
		}
		// 識別に使う子の返戻
		return { Src.GetFieldChild(Node, ChildRole::ReceiverField), Named, Src.GetFieldChild(Node, ChildRole::Type) };
	}

	/**
	 * ディレクティブの適用先の種類とテキストの表示関数
	 * 区切の空白・カンマとコメントは展開で変わる表現差（整形仕様 8.1）の為に表示へ含めない
	 * 計算量：対象の子の部分木のノード数 C と表示に使うテキストの長さ L に対し O(C + L)
	 * @param Src 解析済のテキスト
	 * @param Node 対象のノード
	 * @param TargetParts 適用先の受手・名前・型の子（此の順の配列）
	 * @return ノードの種類名と，空白・カンマ・コメントを除いた各部品の表示
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	std::string SourceProjector::DirectiveLabel(
		const ::ParsedTree &Src,
		const TSNode Node,
		const std::array<TSNode, 3> &TargetParts
	) {
		// 適用先の種類とテキストの組立
		std::string Result(Src.GetProfile().GetSymbolName(ts_node_symbol(Node)));
		for(const TSNode Part : TargetParts) {
			Result.push_back(':');
			if(ts_node_is_null(Part)) continue;
			WalkAst(
				Part,
				[&Src, &Result](const TSNode Leaf) -> void {
					// トークンでないノードと区切のカンマ・コメントの場合の終了
					if(ts_node_child_count(Leaf) || Src.Is(Leaf, Role::CommaToken) || Src.Is(Leaf, Role::Comment)) return;
					for(const unsigned char Char : Src.GetView(Leaf)) if(!std::isspace(Char)) Result.push_back(Char);
					// 終了
					return;
				}
			);
		}
		// 表示の返戻
		return Result;
	}

	/**
	 * ディレクティブの所有範囲の識別の取得関数（整形仕様 8.1）
	 * 根からノードの親迄の名前・受手・型を持つノード（所有範囲）を種類と其のテキストで表す
	 * 計算量：親インデックスの初回費用 Q，深さ D，所有範囲の子の数の和 C，表示長 L に対し期待値 O(Q + D + C + L)
	 * @param Src 解析済のテキスト
	 * @param Node ノード
	 * @return 所有範囲の表示の並び
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	std::string SourceProjector::DirectiveScope(const ::ParsedTree &Src, const TSNode Node) {
		// 構文の工程も共有する親インデックスに依る道筋の探索（テキストの開始位置では推測しない）
		std::vector<TSNode> Ancestors;
		for(TSNode Cur = Src.GetParentOf(Node); !ts_node_is_null(Cur); Cur = Src.GetParentOf(Cur)) Ancestors.push_back(Cur);
		std::reverse(Ancestors.begin(), Ancestors.end());
		// 所有範囲の表示の組立
		std::string Result;
		for(const TSNode Up : Ancestors) {
			// 名前・受手・型の子の取得と，子を持つ祖先への所有範囲の限定
			const std::array<TSNode, 3> Parts = GetDirectiveParts(Src, Up);
			if(
				std::ranges::all_of(
					Parts,
					[](const TSNode Part) -> bool {
						// 子の無い事の返戻
						return ts_node_is_null(Part);
					}
				)
			) continue;
			Result.append(DirectiveLabel(Src, Up, Parts)).push_back('/');
		}
		// 所有範囲の表示の並びの返戻
		return Result;
	}

	/**
	 * 標準のディレクティブコメントかの判定関数（整形仕様 4.5, 7.5）
	 * 言語仕様書・公式の言語参照・公式ツールチェーンのコンパイル・ビルド仕様が記法・意味・適用条件を定める物だけを対象とし，似た接頭部の注記を推測で含めない
	 * 計算量：ディレクティブコメントの本文のバイト数 N，登録語数 W に対し O((W + 1)(N + 1))
	 * @param Src 解析済のテキスト
	 * @param Reading 入力の読取規則
	 * @param Head `GetDirectiveHeadOf` で求めた頭部の位置
	 * @param Raw コメント全文
	 * @param TokenStart コメントの始まりの位置
	 * @param Notation コメントの記法（整形仕様 3.2）
	 * @param Next 同じ親の中で後に続く最初のコメントでない名前付のノード（無ければ空）
	 * @return 標準のディレクティブコメントなら true
	 * @throws std::bad_alloc 位置表のメモリを確保出来ない場合
	 */
	bool SourceProjector::IsStandardDirective(
		const ::ParsedTree &Src,
		const ReadingRules &Reading,
		const DirectiveHead &Head,
		const std::string_view Raw,
		const uint32_t TokenStart,
		const CommentNotation Notation,
		const TSNode Next
	) {
		// 標準が認める先頭の `#!` （整形仕様 3.2 の記法で決める）の返戻
		if(Notation == CommentNotation::HashBangLine) return true;
		// Go の公式ツールチェーンのディレクティブかの返戻
		if(Reading.HasToolchainCommentDirectives) {
			// Example の範囲より先の位置指示の囲みの位置情報の読取
			// 有効性の返戻
			return Notation != CommentNotation::GoLineBlock && IsOutputComment(Head, TokenStart) ||
			IsToolchainDirective(Src, Head, Raw, TokenStart, Notation, Next);
		}
		// `go` コマンドが cgo の C・C++ の頭部から読むビルド制約かの返戻
		if(Reading.HasHeaderBuildConstraints) return Notation == CommentNotation::GoBuildLine && TokenStart < Head.HeaderEnd;
		// 登録した３種の成立条件に当たらない事の返戻
		return false;
	}

	/**
	 * TypeScript の三重スラッシュディレクティブの形かの判定関数（整形仕様 4.5）
	 * TypeScript の言語手引が定める `reference` / `amd-module` / `amd-dependency` の要素だけを対象とする
	 * 計算量：本文と登録要素名のバイト数の和 N に対し O(N + 1)
	 * @param Reading 登録済の指示要素
	 * @param Raw コメント全文
	 * @return 三重スラッシュの後に定めた要素が続けば true
	 */
	bool SourceProjector::IsRefDirective(const ReadingRules &Reading, std::string_view Raw) {
		// 三重スラッシュの後の区切空白（TypeScript は全角スペースも区切と読む）
		Raw = CommentRepository::TrimDirectiveSeparator(
			DirectiveDefinitionId::TypeScriptRef,
			Raw.substr(std::min(CommentRepository::GetOpenerOf(CommentNotation::TripleSlashLine).size(), Raw.size()))
		);
		// 要素名の後が空白か閉じかの返戻
		return std::any_of(
			Reading.RefDirectiveElements.begin(),
			Reading.RefDirectiveElements.end(),
			[Raw, &Reading](const std::string_view Element) -> bool {
				// 要素名と其の後の区切の照合結果の返戻
				return Raw.size() > Element.size() && Raw.starts_with(Element) &&
				Reading.RefDirectiveTerminators.find(Raw[Element.size()]) != std::string_view::npos;
			}
		);
	}

	/**
	 * 処理系が読む位置のエンコーディング宣言かの判定関数（整形仕様 4.5, 7.5）
	 * Python の２行目の宣言の成立条件である１行目のコメントも含める
	 * @param Head `GetDirectiveHeadOf` で求めた頭部の位置
	 * @param IsEncoding コメントがエンコーディングの指定の形か
	 * @param TokenStart コメントの始まりの位置
	 * @return 処理系がエンコーディングの指定として読む位置の宣言又は其の成立条件なら true
	 */
	bool SourceProjector::IsEncodingDirective(const DirectiveHead &Head, const bool IsEncoding, const uint32_t TokenStart) {
		// 読む範囲の宣言と，２行目の宣言の前行の返戻
		return TokenStart < Head.EncodingEnd && IsEncoding || Head.IsSecondLineEncoding && TokenStart < Head.FirstLineEnd;
	}

	/**
	 * 実行時設定のコメントの読まれる範囲の取得関数（整形仕様 4.5）
	 * 値が有効な組の登録語の内で処理系が最も広く読む範囲を求める（無効な値は処理系が警告して読まない）
	 * 計算量：組の数 P と登録語の数 W に対し O(P · W)
	 * @param Reading 実行時設定の登録語と其の有効な値・読まれる範囲
	 * @param Pairs コメントの実行時設定の組の並び
	 * @return 有効な組の最も広く読む範囲（有効な組が無ければ無い）
	 */
	std::optional<MagicCommentScope> SourceProjector::MagicCommentScopeOf(
		const ReadingRules &Reading,
		const std::vector<MagicCommentPair> &Pairs
	) {
		// 登録済の値と適用範囲の照合
		std::optional<MagicCommentScope> Scope;
		for(const MagicCommentPair &Pair : Pairs) {
			for(const MagicCommentWord &Word : Reading.MagicCommentWords) {
				if(
					const bool IsValid = Pair.Name == Word.Name && CommentRepository::IsMagicCommentValue(Pair.Val, Word.Vals);
					IsValid && (!Scope || *Scope < Word.Scope)
				) Scope = Word.Scope;
			}
		}
		// 最も広く読む範囲の返戻
		return Scope;
	}

	/**
	 * コメントだけの行に在るかの判定関数
	 * 計算量：行頭からコメント迄の長さ L に対し O(L)
	 * @param Text 原文
	 * @param TokenStart コメントの始まりの位置
	 * @return 行頭からコメント迄が空白だけなら true
	 */
	bool SourceProjector::IsCommentLine(const std::string_view Text, const uint32_t TokenStart) {
		// コメントの前の改行位置の探索
		const size_t LineEnd = Text.rfind('\n', TokenStart ? TokenStart - 1 : 0);
		const size_t LineStart = LineEnd == std::string_view::npos || !TokenStart ? 0 : LineEnd + 1;
		// 行頭からコメント迄に空白の他の字を持たないかの返戻
		return Text.substr(LineStart, TokenStart - LineStart).find_first_not_of(" \t") == std::string_view::npos;
	}

	/**
	 * 本文の各行の共通のインデントを除いた形の取得関数
	 * 継続行の共通のインデントと，各行の行末の空白を除き，ディレクティブの照合用のテキストを作る
	 * 開始記号の後の区切空白も，定義上同値な記法へ揃える（値に属する空白を読む記法は除く）
	 * 計算量：本文のテキスト長 L に対し O(L)
	 * @param Raw コメント全文
	 * @param Opener 記法の開始記号（記号の後の空白を値として読むディレクティブは空）
	 * @param Def ディレクティブの登録定義（処理系が区切と読む字を決める）
	 * @return ２行目以降の共通のインデントと各行の行末の空白を除き，開始記号の後の区切空白（区切の無い物を含む）を半角スペース１つへ揃えた本文
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	std::string SourceProjector::DedentedBody(
		const std::string_view Raw,
		const std::string_view Opener,
		const DirectiveDefinitionId Def
	) {
		// ２行目以降の空でない行の共通のインデント
		std::string_view Common;
		bool HasCommon = false;
		for(size_t Cursor = Raw.find('\n'); Cursor != std::string_view::npos;) {
			const size_t Start = Cursor + 1;
			Cursor = Raw.find('\n', Start);
			const std::string_view Line = Raw.substr(Start, std::min(Cursor, Raw.size()) - Start);
			const std::string_view Indent = Line.substr(0, std::min(Line.find_first_not_of(" \t"), Line.size()));
			if(Indent.size() == Line.size()) continue;
			const size_t Shared = HasCommon ?
			static_cast<size_t>(std::mismatch(Common.begin(), Common.end(), Indent.begin(), Indent.end()).first - Common.begin()) :
			Indent.size();
			Common = Indent.substr(0, Shared);
			HasCommon = true;
		}
		// 行毎にインデントと行末の空白を除いた本文
		std::string Body;
		Body.reserve(Raw.size());
		for(size_t Cursor = 0;;) {
			const size_t End = std::min(Raw.find('\n', Cursor), Raw.size());
			std::string_view Line = Raw.substr(Cursor, End - Cursor);
			if(Cursor && Line.starts_with(Common)) Line.remove_prefix(Common.size());
			Line.remove_suffix(Line.size() - std::min(Line.find_last_not_of(" \t") + 1, Line.size()));
			if(!Cursor && !Opener.empty() && Line.starts_with(Opener)) {
				// 開始記号の後の区切空白（本文が在れば区切の有無に依らず半角スペース１つ，整形仕様 7.6）
				const std::string_view Rest = CommentRepository::TrimDirectiveSeparator(Def, Line.substr(Opener.size()));
				Body.append(Opener).append(Rest.empty() ? "" : " ");
				Line = Rest;
			}
			Body.append(Line);
			if(End == Raw.size()) break;
			Body += '\n';
			Cursor = End + 1;
		}
		// 照合に使う本文の返戻
		return Body;
	}

	/**
	 * Go の Example 関数の出力の注記の範囲の収集関数
	 * 登録した名前境界とシグネチャの条件を満たす最上位の関数の本体から，最後のコメントのグループを調べる
	 * `Output:`・`Unordered output:` で始まるグループが対象である（大文字・小文字は区別しない）
	 * `go test` は此のグループの本文を関数の標準出力と比べる（`go help testfunc`）
	 * グループは空行とコードを挟まずに続くコメントの並びとし，本文の空白は比べる値の一部である
	 * 計算量：最上位の宣言・シグネチャと対象の本体のノード数 V，注記と間隙の読取長 B に対し O(V + B)
	 * @param Src 解析済のテキスト
	 * @return 出力の注記のグループの始まりから本体の終わり迄の範囲（位置順）
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 収集するテキスト又は記録の要素数が格納先の上限を超える場合
	 */
	std::vector<std::pair<uint32_t, uint32_t>> SourceProjector::ExampleOutputRanges(const ::ParsedTree &Src) {
		// 出力注記の範囲と読取規則の準備
		std::vector<std::pair<uint32_t, uint32_t>> Ranges;
		const std::string_view Text = Src;
		const ReadingRules &Reading = Src.GetReading();
		ForEachNamedChild(
			Src.GetRoot(),
			[&](const TSNode Decl) -> void {
				// 関数宣言でない子の場合の終了
				if(!Src.Is(Decl, Role::FunctionDeclNode)) return;
				const TSNode Name = Src.GetFieldChild(Decl, ChildRole::Name);
				const TSNode Body = Src.GetFieldChild(Decl, ChildRole::Body);
				// 入力で名前と本体を確認出来ない宣言の終了
				if(ts_node_is_null(Name) || ts_node_is_null(Body) || Src.HasError(Decl)) return;
				const std::string_view FunctionName = Src.GetView(Name);
				// 登録した接頭辞を持たない名前の終了
				if(!FunctionName.starts_with(Reading.OutputFunctionPrefix)) return;
				// 接頭辞の直後の文字種は Unicode の一般カテゴリで照合する
				if(FunctionName.size() > Reading.OutputFunctionPrefix.size()) {
					size_t Pos = Reading.OutputFunctionPrefix.size();
					char32_t Point = 0;
					if(!TextEdit::IsUtf8Decoded(FunctionName, Pos, Point) || FindUnicodeRange(Reading.OutputFunctionExcludedInitialRanges, Point)) {
						// デコード出来ない字又は登録した除外文字種の終了
						return;
					}
				}
				// 登録したシグネチャの場が，コメントを除いて空かを確認する
				for(const ChildRole Field : Reading.OutputFunctionEmptyFields) {
					const TSNode Part = Src.GetFieldChild(Decl, Field);
					if(ts_node_is_null(Part)) continue;
					// 空の並び以外のシグネチャの場を持つ宣言の終了
					if(!Src.Is(Part, Role::DeclaratorParams)) return;
					bool HasItem = false;
					ForEachNamedChild(
						Part,
						[&Src, &HasItem](const TSNode Item) -> bool {
							HasItem = !Src.Is(Item, Role::Comment);
							// 最初のシグネチャの項目で打切る事の返戻
							return !HasItem;
						}
					);
					// シグネチャの項目が在る宣言の終了
					if(HasItem) return;
				}
				// 本体の中のコメントの範囲（位置順）
				std::vector<std::pair<uint32_t, uint32_t>> Comments;
				WalkChildrenCursor(
					Body,
					[&Src, &Comments](const TSNode Node) -> bool {
						// コメントでないノードへ降りる事の返戻
						if(!Src.Is(Node, Role::Comment)) return true;
						Comments.push_back(Src.GetCommentRangeOf(Node));
						// コメントの内側へ降りない事の返戻
						return false;
					}
				);
				// 最後のコメントのグループの始まり（前のコメントとの間が空白と改行１つ迄）
				size_t Group = Comments.size();
				while(Group > 1) {
					if(
						const std::string_view Gap = Text.substr(Comments[Group - 2].second, Comments[Group - 1].first - Comments[Group - 2].second);
						Gap.find_first_not_of(" \t\r\n") != std::string_view::npos || std::count(Gap.begin(), Gap.end(), '\n') > 1
					) break;
					--Group;
				}
				// グループの最初のコメントの記号の後の本文
				std::string Head;
				if(Group) {
					const std::string_view First = Text.substr(Comments[Group - 1].first, Comments[Group - 1].second - Comments[Group - 1].first);
					Head = First.substr(
						std::min(
							CommentRepository::GetOpenerOf(CommentRepository::NotationOf(Src.GetRules().Notation, First, false)).size(),
							First.size()
						)
					);
				}
				Head.erase(0, std::min(Head.find_first_not_of(" \t\r\n"), Head.size()));
				ToLowerAscii(Head);
				if(
					std::any_of(
						Reading.OutputCommentWords.begin(),
						Reading.OutputCommentWords.end(),
						[&Head](const std::string_view Word) -> bool {
							// 出力を比べる登録語かの返戻
							return Head.starts_with(Word);
						}
					)
				) Ranges.emplace_back(Comments[Group - 1].first, Src.GetEnd(Body));
				// 終了
				return;
			}
		);
		// 出力の注記の範囲の返戻
		return Ranges;
	}

	/**
	 * 宣言子毎に名前を持つ宣言の部分かの判定関数（整形仕様 7.4, 8.1）
	 * @param Syntax 投影した構文
	 * @param Node ノード
	 * @return 宣言子のフィールドを持つノードか変数の宣言子の項目なら true
	 * @throws FormatLimitExceeded 構文の照会・走査が作業予算を超える場合
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 */
	bool SourceProjector::IsDeclaratorPart(const SyntaxModel &Syntax, const NodeId Node) {
		// 宣言子のフィールドを持つか変数の宣言子の項目かの返戻
		return Syntax.HasField(Node, ChildRole::Declarator) || Syntax.Is(Node, Role::VarDeclaratorItem);
	}

	/**
	 * ディレクティブの適用先の識別の取得関数（整形仕様 8.1，構文・所有範囲・境界の対応で照合し，同名又は記号の一致だけで同一視しない）
	 * 所有範囲の表示の並びに，適用先の種類と其のテキスト，同じ親の中で同じ表示を持つ先行する兄弟の数（境界の順）を添える（変数宣言は最初の変数の名前で表す）
	 * 計算量：親インデックスの初回費用 Q，深さ D，所有範囲の子の数の和 C，表示長 L に対し期待値 O(Q + D + C + L)
	 * @param Src 解析済のテキスト
	 * @param Decl 宣言のノード
	 * @param Ordinal 同じ親の中で同じ表示を持つ先行する兄弟の数
	 * @return 所有範囲の表示の並びと，適用先の表示と境界の順
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	std::string SourceProjector::DirectiveTarget(const ::ParsedTree &Src, const TSNode Decl, const uint32_t Ordinal) {
		// 所有範囲と適用先の表示と境界の順の返戻
		return DirectiveScope(Src, Decl).append(DirectiveLabel(Src, Decl, GetDirectiveParts(Src, Decl))).append("#").append(
			std::to_string(Ordinal)
		);
	}

	/**
	 * 登録済のディレクティブ記法の記録の取得関数（整形仕様 3.2, 4.5）
	 * 登録定義に当たる記法を，無効な位置・区切・数値の物も含めて記録する
	 * Go の固定記法は `IsStandardDirective` で有効性を判定する
	 * 其の他は，出力注記・参照・エンコーディング・マジックコメントの登録条件から判定する
	 * 計算量：ディレクティブコメントの本文のバイト数 N，登録語数 W に対し O((W + 1)(N + 1))
	 * @param Src 解析済のテキスト
	 * @param Reading 入力の読取規則
	 * @param Head ディレクティブコメントを判定する頭部の位置
	 * @param Raw コメント全文
	 * @param TokenStart コメントの始まり
	 * @param Notation コメントの記法
	 * @param Next コメントの後に続く最初のコメントでない兄弟
	 * @return 登録定義に当たる記法の記録（当たらない通常コメントは無い）
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws FormatLimitExceeded 記法の定義を記録して居ない場合（フォーマッターの不具合）
	 */
	std::optional<DirectiveInfo> SourceProjector::DirectiveOf(
		const ::ParsedTree &Src,
		const ReadingRules &Reading,
		const DirectiveHead &Head,
		const std::string_view Raw,
		const uint32_t TokenStart,
		const CommentNotation Notation,
		const TSNode Next
	) {
		// `#` の行コメントの登録形（注記を１度だけ読む）
		const CommentRepository::RegisteredLineForm Form = Notation == CommentNotation::HashLine ?
		CommentRepository::RegisteredLineFormOf(Src.GetRules().Notation.OutputForms.Conditions, Raw) :
		CommentRepository::RegisteredLineForm();
		bool IsActive = false;
		// 記法から登録定義を取得するローカル関数
		const auto GetNotationDef = [Notation]() -> std::optional<DirectiveDefinitionId> {
			// 記法毎の登録定義の選択
			switch(Notation) {
			case CommentNotation::HashBangLine:
				// 登録定義の返戻
				return DirectiveDefinitionId::Shebang;
			case CommentNotation::GoBuildLine:
				return DirectiveDefinitionId::GoBuild;
			case CommentNotation::GoDebugLine:
				return DirectiveDefinitionId::GoDebug;
			case CommentNotation::GoEmbedLine:
				return DirectiveDefinitionId::GoEmbed;
			case CommentNotation::GoGenerateLine:
				return DirectiveDefinitionId::GoGenerate;
			case CommentNotation::GoLinknameLine:
				return DirectiveDefinitionId::GoLinkname;
			case CommentNotation::GoNoescapeLine:
			case CommentNotation::GoNoinlineLine:
			case CommentNotation::GoNoraceLine:
			case CommentNotation::GoNosplitLine:
			case CommentNotation::GoUintptrescapesLine:
			case CommentNotation::GoWasmexportLine:
			case CommentNotation::GoWasmimportLine:
				return DirectiveDefinitionId::GoFunctionPragma;
			case CommentNotation::GoLineLine:
				return DirectiveDefinitionId::LinePos;
			case CommentNotation::GoLineBlock:
				return DirectiveDefinitionId::BlockLinePos;
			case CommentNotation::CgoExportLine:
				return DirectiveDefinitionId::CgoExport;
			case CommentNotation::GccgoExternLine:
				return DirectiveDefinitionId::GccgoExtern;
			}
			// 記法だけでは定まらない事の返戻
			return std::nullopt;
		};
		// 記法に対応する登録定義の選択
		std::optional<DirectiveDefinitionId> Def = GetNotationDef();
		// 記法だけで決まらない定義（位置で決まる Go の出力注記と cgo の前文，言語仕様のエンコーディング宣言・マジックコメント）
		// 記法で定まった定義の位置条件の判定
		if(Def) IsActive = IsStandardDirective(Src, Reading, Head, Raw, TokenStart, Notation, Next);
		else if(Reading.HasToolchainCommentDirectives) {
			const bool IsExampleOutput = IsOutputComment(Head, TokenStart);
			IsActive = IsExampleOutput || IsToolchainDirective(Src, Head, Raw, TokenStart, Notation, Next);
			if(IsActive) Def = IsExampleOutput ? DirectiveDefinitionId::ExampleOutput : DirectiveDefinitionId::CgoPreamble;
		} else if(
			!Reading.RefDirectiveElements.empty() && Notation == CommentNotation::TripleSlashLine && IsRefDirective(Reading, Raw)
		) {
			Def = DirectiveDefinitionId::TypeScriptRef;
			IsActive = TokenStart < Head.HeaderEnd;
		} else if(Notation == CommentNotation::HashLine) {
			const bool IsEncoding = IsEncodingDirective(Head, Form.IsEncoding, TokenStart);
			const std::optional<MagicCommentScope> Scope =
			!IsEncoding && Form.Pairs ? MagicCommentScopeOf(Reading, *Form.Pairs) : std::nullopt;
			// 適用範囲から位置の成立を判定するローカル関数
			const auto IsMagicCommentActive = [TokenStart, &Head, &Src](const MagicCommentScope Scope) -> bool {
				// 適用範囲毎の位置の判定
				switch(Scope) {
				case MagicCommentScope::Anywhere:
					// 全ての位置で成立する事の返戻
					return true;
				case MagicCommentScope::CommentLine:
					// コメント行又は頭部に在るかの返戻
					return IsCommentLine(Src, TokenStart) || TokenStart < Head.HeaderEnd;
				case MagicCommentScope::Header:
					// 頭部に在るかの返戻
					return TokenStart < Head.HeaderEnd;
				}
				// 登録された範囲でない事の返戻
				return false;
			};
			IsActive = IsEncoding || Scope && IsMagicCommentActive(*Scope);
			if(IsEncoding) Def = Form.IsEncoding ? DirectiveDefinitionId::EncodingDecl : DirectiveDefinitionId::EncodingPrecondition;
			else if(Form.IsMagic) Def = DirectiveDefinitionId::MagicComment;
			else if(Form.IsEncoding) Def = DirectiveDefinitionId::EncodingDecl;
		}
		// 登録定義に当たらない通常コメントの返戻
		if(!Def) return std::nullopt;
		DirectiveInfo DirectiveInfo;
		DirectiveInfo.Definition = *Def;
		DirectiveInfo.State = IsActive ? DirectiveState::Active : DirectiveState::Inactive;
		// Go のディレクティブは記号の後の空白を揃えて合成した形で照合する（揃えても処理系の読みは変わらない）
		if(CommentRepository::IsSpacedDirective(Notation)) {
			CommentRecord Composed { std::string(Raw), { CommentPlacement::Before } };
			Composed.Notation = Notation;
			Composed.Role = CommentRole::Directive;
			CommentRepository::AttachRules(Src.GetRules().Notation, Composed);
			CommentRepository::Decompose(Composed);
			DirectiveInfo.Conditions.push_back(CommentRepository::Compose(Composed));
		} else {
			DirectiveInfo.Conditions.push_back(
				DedentedBody(
					Raw,
					CommentRepository::IsValSpacing(DirectiveInfo.Definition) ? std::string_view() : CommentRepository::GetOpenerOf(Notation),
					DirectiveInfo.Definition
				)
			);
		}
		// 登録済の記法の記録の返戻
		return DirectiveInfo;
	}

	/**
	 * 標準のディレクティブコメントを判定する頭部の位置の取得関数（整形仕様 4.5, 7.5）
	 * 頭部だけで読むディレクティブの境界を，最初のコードの位置と行の境界から求める
	 * Go の出力注記の範囲も収集し，頭部の情報と共に返戻する
	 * 計算量：頭部のバイト数 N，最上位の宣言と例の本体のノード数 V に対し O(N + V + 1)
	 * @param Src 解析済のテキスト
	 * @return 頭部・エンコーディング宣言の境界，１行目の終わり，Python の２行目の宣言の有無，Go の出力注記の範囲
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 収集するテキスト又は記録の要素数が格納先の上限を超える場合
	 */
	SourceProjector::DirectiveHead SourceProjector::GetDirectiveHeadOf(const ::ParsedTree &Src) {
		// 頭部の位置と読取規則の準備
		DirectiveHead Head;
		const ReadingRules &Reading = Src.GetReading();
		// 頭部の注記を読まない言語の返戻
		if(!Reading.HasDirectiveHeader) return Head;
		// コメントでない最初の子の始まり（処理系は其処迄の注記だけを読む）
		Head.HeaderEnd = static_cast<uint32_t>(Src.size());
		ForEachNamedChild(
			Src.GetRoot(),
			[&Src, &Head](const TSNode Child) -> bool {
				// コメントでない最初の子の始まりの記録
				const bool IsComment = Src.Is(Child, Role::Comment);
				if(!IsComment) Head.HeaderEnd = Src.GetStart(Child);
				// 最初のコードで打切る事の返戻
				return IsComment;
			}
		);
		if(Reading.HasOutputCommentGroups) Head.ExampleOutputs = ExampleOutputRanges(Src);
		// ビルド制約・三重スラッシュディレクティブを読む頭部の位置の返戻
		if(!Reading.HasSingleLineEncodingHeader && !Reading.HasConditionalSecondLineEncoding) return Head;
		const CommentRepository::EncodingHeader Encoding =
		CommentRepository::ReadEncodingHeader(Reading, Src.GetRules().Notation.OutputForms.Conditions, Src);
		Head.FirstLineEnd = static_cast<uint32_t>(Encoding.FirstLineEnd);
		Head.EncodingEnd = static_cast<uint32_t>(std::min<size_t>(Head.HeaderEnd, Encoding.End));
		Head.IsSecondLineEncoding = Encoding.HasSecondLineEncoding && Encoding.DeclarationStart < Head.HeaderEnd;
		// 頭部の位置の返戻
		return Head;
	}

	/**
	 * ========== 構文とコメントの分離 ==========
	 * 構文の末尾が空の波括弧の本体かの判定関数
	 * 計算量：末尾へ辿る子の数の和 C に対し O(C)
	 * @param Src 投影する解析木
	 * @param Node 節又は其の所有構文
	 * @return 最後の本体が構文の内容を持たなければ true
	 */
	bool SourceProjector::HasEmptyFinalBody(const ::ParsedTree &Src, TSNode Node) {
		// コメントを除く末尾の本体の探索
		while(!ts_node_is_null(Node)) {
			// コメントを除く最後の子と，其の前の開波括弧・内容の有無
			TSNode Last {};
			bool HasOpen = false, HasContent = false;
			ForEachChild(
				Node,
				[&Src, &Last, &HasOpen, &HasContent](const TSNode Child) -> void {
					// コメントを除く前後の子の更新
					if(!Src.Is(Child, Role::Comment)) {
						if(!ts_node_is_null(Last)) (Src.Is(Last, Role::OpeningBraceToken) ? HasOpen : HasContent) = true;
						Last = Child;
					}
					// 終了
					return;
				}
			);
			// 空の本体かの返戻
			if(Src.Is(Last, Role::ClosingBraceToken)) return HasOpen && !HasContent;
			Node = Last;
		}
		// 最後の本体を持たない事の返戻
		return false;
	}

	/**
	 * 説明対象の識別の組立関数（整形仕様 7.4, 8.1）
	 * 説明対象を，所有する宣言の種類と名前の並び，対象の種類と名前，同じ親の中で同じ種類と名前を持つ先行する兄弟の数で表す
	 * 型・初期化子のテキストとノード番号を識別に含めず，区切の正規化等の選択済の表現差で識別を変えない（整形仕様 8.1）
	 * 同じ親の中の順は親毎に子を１度だけ辿って数え，宣言子毎に名前を持つ宣言は全ての宣言子の名前で数える（宣言の分割・統合で順を変えない）
	 * 計算量：対象の数 D，対象の祖先のノード数 H，対象の親の子の数の和 C，名前を探す部分木の大きさの和 N と名前の長さ L に対し O(D + H + C + N + L)
	 * @param Syntax 投影した構文
	 * @param Entries 説明対象と其の名前を表すノード（宣言子の中のドキュメントは其の宣言子）の組の並び
	 * @param Output 説明対象の識別と共有接頭部を追記する所有者
	 * @param Work 名前取得・祖先の初回訪問・表示の照会・論理行の観測値（不要なら nullptr）
	 * @return 組毎の説明対象の識別の開始と長さ
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws FormatLimitExceeded 構文の照会・走査が作業予算を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 */
	std::vector<std::pair<size_t, size_t>> SourceProjector::LabelTargets(
		const SyntaxModel &Syntax,
		const std::span<const std::pair<NodeId, NodeId>> Entries,
		ValidationContext::DocStorage &Output,
		std::array<size_t, 4> *const Work
	) {
		std::string &Out = Output.Targets;
		std::pmr::monotonic_buffer_resource Storage;
		std::pmr::unordered_map<NodeId, std::pmr::string> Names(&Storage);
		std::pmr::vector<NodeId> Pending(&Storage);
		// ノードの名前（名前のフィールド，宣言子の最も内側の名前，本体の外で最初の名前・型名のトークン，本体の外で最初の名前付のトークンの順に探す）
		const auto NameOf = [&](const NodeId Node) -> std::string_view {
			if(const std::pmr::unordered_map<NodeId, std::pmr::string>::const_iterator Found = Names.find(Node); Found != Names.end()) {
				// 同じノードから取得済の名前の返戻
				return Found->second;
			}
			// 宣言子からの名前の探索
			NodeId Named = Syntax.GetFieldChild(Node, ChildRole::Name), FirstLeaf = NoNode;
			for(NodeId Declarator = Syntax.GetFieldChild(Node, ChildRole::Declarator); Named == NoNode && Declarator != NoNode;) {
				Named = Syntax.GetFieldChild(Declarator, ChildRole::Name);
				const NodeId Inner = Syntax.GetFieldChild(Declarator, ChildRole::Declarator);
				if(Named == NoNode && Inner == NoNode) Named = Declarator;
				Declarator = Inner;
			}
			// 本体の外からの名前候補の探索
			Pending.clear();
			Pending.push_back(Node);
			while(Named == NoNode && !Pending.empty()) {
				const NodeId Part = Pending.back();
				Pending.pop_back();
				if(Part != Node && (Syntax.Is(Part, Role::NameToken) || Syntax.Is(Part, Role::TypeName))) Named = Part;
				else if(Part == Node || !Syntax.HasField(Part, ChildRole::Body)) {
					if(FirstLeaf == NoNode && Part != Node && Syntax.IsNamed(Part) && !Syntax.GetChildCount(Part)) FirstLeaf = Part;
					const std::span<const NodeId> Children = Syntax.GetChildrenOf(Part);
					Pending.insert(Pending.end(), Children.rbegin(), Children.rend());
				}
			}
			if(Named == NoNode) Named = FirstLeaf;
			// 名前のテキストの保持
			std::pmr::string &Name = Names[Node];
			if(Named != NoNode) {
				if(Work) ++(*Work)[0];
				Name = Syntax.GetText(Named);
			}
			// 名前のテキストの返戻
			return Name;
		};
		// 種類の表示と名前の組で比較し，表示のテキストは最後の追記時だけ組み立てる
		struct Label {
			std::string_view Kind; // 文法で固定した同じ内容の種類の番号
			std::string_view Name; // 保持した名前のテキスト

			bool operator==(const Label &) const = default; // 種類の表示と名前の比較関数
		};
		struct LabelHash {
			/**
			 * 種類の表示と名前のハッシュの取得関数
			 * 計算量：名前の長さ N に対し O(N)
			 * @param Value 比較する種類と名前
			 * @return 種類の表示と名前のハッシュ
			 */
			size_t operator()(const Label &Value) const noexcept {
				return std::hash<std::string_view>()(Value.Kind) ^ std::hash<std::string_view>()(Value.Name);
			}
		};
		// 種類の表示は文法の構築時に固定した番号だけを参照する
		const auto LabelOf = [&](const NodeId Node, const std::string_view Name) -> Label {
			if(Work) ++(*Work)[2];
			// 種類の番号と保持した名前の返戻
			return { Syntax.GetNode(Node).Kind.Def->DocIdentity, Name };
		};
		// 兄弟を数える表示（宣言子毎に名前を持つ宣言は全ての宣言子の名前で表し，宣言の分割・統合で兄弟の順を変えない）
		std::pmr::vector<Label> SiblingBuffer(&Storage);
		const auto SiblingLabels = [&](const NodeId Node) -> void {
			SiblingBuffer.clear();
			for(const NodeId Child : Syntax.GetChildrenOf(Node)) if(IsDeclaratorPart(Syntax, Child)) {
				SiblingBuffer.push_back(LabelOf(Node, NameOf(Child)));
			}
			if(SiblingBuffer.empty()) SiblingBuffer.push_back(LabelOf(Node, NameOf(Node)));
			// 終了
			return;
		};
		// 組毎の対象の表示と，親毎の其の組の並び
		std::pmr::vector<Label> Labels(Entries.size(), &Storage);
		std::pmr::vector<uint32_t> Ordinals(Entries.size(), 0, &Storage);
		using Groups = std::pmr::unordered_map<NodeId, std::pmr::vector<size_t>>;
		Groups ByParent(&Storage);
		for(size_t Idx = 0; Idx < Entries.size(); ++Idx) {
			const std::pair<NodeId, NodeId> Entry = Entries[Idx];
			const NodeId Target = Entry.first, Named = Entry.second;
			Labels[Idx] = LabelOf(Target, NameOf(Named));
			ByParent[Syntax.GetParentOf(Target)].push_back(Idx);
		}
		// 親毎に子を１度だけ辿り，同じ表示を持つ先行する兄弟の数を数える
		for(const Groups::value_type &Group : ByParent) {
			Groups ByTarget(&Storage);
			for(const size_t Idx : Group.second) ByTarget[Entries[Idx].first].push_back(Idx);
			std::pmr::unordered_map<Label, uint32_t, LabelHash> Counts(&Storage);
			for(const NodeId Child : Syntax.GetChildrenOf(Group.first)) {
				if(!Syntax.IsNamed(Child)) continue;
				if(const Groups::const_iterator Found = ByTarget.find(Child); Found != ByTarget.end()) for(const size_t Idx : Found->second) {
					Ordinals[Idx] = Counts[Labels[Idx]];
				}
				SiblingLabels(Child);
				for(const Label &Value : SiblingBuffer) ++Counts[Value];
			}
		}
		// 所有する宣言（宣言子毎に名前を持つ宣言は其の宣言子）毎の表示（同じ所有者を持つ組で求め直さない）
		std::pmr::unordered_map<NodeId, Label> Owners(&Storage);
		std::vector<std::pair<size_t, size_t>> Descriptions;
		std::pmr::unordered_map<NodeId, size_t> ScopeContexts(&Storage);
		std::pmr::vector<NodeId> ScopePending(&Storage);
		struct ScopeKey {
			size_t Parent; // 共有接頭部の番号
			Label Value; // 此の宣言だけの表示

			bool operator==(const ScopeKey &) const = default; // 接頭部と宣言の一致の判定関数
		};
		struct ScopeHash {
			/**
			 * 接頭部と宣言の表示のハッシュの取得関数
			 * 計算量：名前の長さ L に対し O(L)
			 * @param Key 接頭部と宣言
			 * @return 共有する接頭部の照会に使うハッシュ
			 */
			size_t operator()(const ScopeKey &Key) const noexcept {
				return std::hash<size_t>()(Key.Parent) ^ LabelHash()(Key.Value);
			}
		};
		std::pmr::unordered_map<ScopeKey, size_t, ScopeHash> ScopeIds(&Storage);
		Output.Scopes.push_back({});
		// 未取得の祖先だけを辿り，共有接頭部の表を親から子へ作る
		const auto ScopeOf = [&](const NodeId Target) -> size_t {
			ScopePending.clear();
			NodeId Current = Target;
			while(Current != NoNode && !ScopeContexts.contains(Current)) {
				if(Work) ++(*Work)[1];
				ScopePending.push_back(Current);
				Current = Syntax.GetParentOf(Current);
			}
			size_t Prefix = Current == NoNode ? 0 : ScopeContexts.at(Current);
			while(!ScopePending.empty()) {
				const NodeId Below = ScopePending.back(), Up = Syntax.GetParentOf(Below);
				ScopePending.pop_back();
				if(Up != NoNode && Syntax.Is(Up, Role::DocumentableDecl)) {
					const NodeId Named = IsDeclaratorPart(Syntax, Below) ? Below : Up;
					using OwnerIter = std::pmr::unordered_map<NodeId, Label>::iterator;
					const std::pair<OwnerIter, bool> Owner = Owners.try_emplace(Named);
					if(Owner.second) Owner.first->second = LabelOf(Up, NameOf(Named));
					const Label Value = Owner.first->second;
					const auto [Found, Added] = ScopeIds.try_emplace(ScopeKey{ Prefix, Value }, Output.Scopes.size());
					if(Added) Output.Scopes.push_back({ Prefix, std::string(Value.Kind) + ":" + std::string(Value.Name) });
					Prefix = Found->second;
				}
				ScopeContexts.emplace(Below, Prefix);
			}
			// 此の投影だけで共有する接頭部の番号の返戻
			return Prefix;
		};
		// 共有する種類と名前への参照を先に並べ，連結先の再確保で既存の本文を複写しない
		std::pmr::vector<std::string_view> Parts(&Storage);
		std::pmr::vector<std::pmr::string> Suffixes(&Storage);
		Suffixes.reserve(Entries.size());
		size_t TotalSize = Out.size();
		const auto AppendPart = [&](const std::string_view Text) -> void {
			if(Text.size() > Out.max_size() - TotalSize) throw std::length_error("document target text is too large");
			TotalSize += Text.size();
			Parts.push_back(Text);
			// 終了
			return;
		};
		const auto AppendLabel = [&](const Label Value) -> void {
			AppendPart(Value.Kind);
			AppendPart(":");
			AppendPart(Value.Name);
			// 終了
			return;
		};
		std::pair<NodeId, NodeId> PreviousEntry { NoNode, NoNode };
		std::pair<size_t, size_t> PreviousDescription { 0, 0 };
		Descriptions.reserve(Entries.size());
		for(size_t Idx = 0; Idx < Entries.size(); ++Idx) {
			// 連続する同じ説明対象の識別は同じ範囲を共有する
			if(Entries[Idx] == PreviousEntry) {
				Descriptions.push_back(PreviousDescription);
				Output.TargetScopes.push_back(Output.TargetScopes.back());
				continue;
			}
			Output.TargetScopes.push_back(ScopeOf(Entries[Idx].first));
			const size_t Begin = TotalSize;
			AppendLabel(Labels[Idx]);
			Suffixes.emplace_back("#" + std::to_string(Ordinals[Idx]));
			AppendPart(Suffixes.back());
			PreviousEntry = Entries[Idx];
			PreviousDescription = { Begin, TotalSize - Begin };
			Descriptions.push_back(PreviousDescription);
		}
		Out.reserve(TotalSize);
		for(const std::string_view Part : Parts) Out.append(Part);
		// 組毎の説明対象の識別の返戻
		return Descriptions;
	}

	/**
	 * 標準のディレクティブコメントの一覧の取得関数（整形仕様 4.5, 8.1）
	 * 入力と最終テキストの双方から同じ規則で集め，本文・記法・有効性・位置条件・適用先の宣言が保たれたかを検証側が比べる
	 * 計算量：共通インデックスの初回費用 Q，コメント数 N，表示の総長 L，適用先の兄弟数の和 C と深さの和 D に対し O(Q + N + L + C + D)
	 * @param Src 解析済のテキスト
	 * @param Head コメントの分離で求めたディレクティブの頭部
	 * @param Starts ディレクティブコメントの始まりの位置の昇順の格納先（不要なら nullptr）
	 * @param Work コメントの照会数の観測用計数（構造の予算から独立，無ければ計上しない）
	 * @return 原文順のディレクティブコメントの本文（継続行の共通のインデントと行末の空白を除き，宣言へ作用する物は其の名前を後へ添えた形）
	 * @throws FormatLimitExceeded 観測用計数の上限を超える場合
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 収集するテキスト又は記録の要素数が格納先の上限を超える場合
	 */
	std::vector<std::string> SourceProjector::CollectDirectives(
		const ::ParsedTree &Src,
		const DirectiveHead &Head,
		std::vector<uint32_t> *const Starts,
		StructureWorkAccount *const Work
	) {
		// ディレクティブコメントの収集先の準備
		std::vector<std::string> Directives;
		// 構文木の無いテキストの返戻
		if(!Src.IsParsed()) return Directives;
		const std::string_view Text = Src;
		const NotationRules &Rules = Src.GetRules().Notation;
		const ReadingRules &Reading = Src.GetReading();
		const TSNode Root = Src.GetRoot();
		// コメントの有無・後続兄弟は，範囲の共通走査で確定した結果だけを読む
		Src.GetCommentSpans();
		const ::ParsedTree::CommentFacts &Facts = *Src.Syntax.Comments;
		// コメントノードが無い木にはディレクティブ候補も無い事の返戻
		if(Facts.Nodes.empty()) return Directives;
		const std::pmr::unordered_map<const void *, TSNode> &Following = Facts.Following;
		// 同じ親の兄弟の順位を最初の必要時に１度だけ数える
		std::unordered_map<const void *, uint32_t> Ordinals;
		std::unordered_set<const void *> ScannedParents;
		// 共通走査が記録した後続兄弟の照会
		const auto Lookup = [](const std::pmr::unordered_map<const void *, TSNode> &Table, const TSNode Node) -> TSNode {
			// ノードに対応する表の項目の探索
			const std::pmr::unordered_map<const void *, TSNode>::const_iterator Found =
			ts_node_is_null(Node) ? Table.end() : Table.find(Node.id);
			// 表のノードの返戻
			return Found == Table.end() ? TSNode{} : Found->second;
		};
		// 適用先の表示の控え
		std::unordered_map<const void *, std::string> Targets;
		// ディレクティブの適用先の表示の取得
		const auto TargetOf = [&Src, &Ordinals, &Targets, &ScannedParents](const TSNode Target) -> const std::string & {
			// 保存済の適用先の表示の返戻
			if(
				const std::unordered_map<const void *, std::string>::const_iterator Found = Targets.find(Target.id);
				Found != Targets.end()
			) return Found->second;
			// 親の照会は構文の工程も共有する既存のインデックスを使う
			if(const TSNode Parent = Src.GetParentOf(Target); !ts_node_is_null(Parent) && ScannedParents.insert(Parent.id).second) {
				std::unordered_map<TSSymbol, std::unordered_map<std::string, uint32_t>> Counts;
				std::vector<TSNode> Unnamed;
				if(HeldCursor Cursor(Parent); ts_tree_cursor_goto_first_child(&Cursor)) {
					do {
						if(const TSNode Child = ts_tree_cursor_current_node(&Cursor); !ts_node_is_named(Child)) Unnamed.push_back(Child);
						else {
							uint32_t &Rank = Counts[ts_node_symbol(Child)][DirectiveLabel(Src, Child, GetDirectiveParts(Src, Child))];
							Ordinals.emplace(Child.id, Rank++);
						}
					} while(ts_tree_cursor_goto_next_sibling(&Cursor));
					for(const TSNode Child : Unnamed) {
						const std::unordered_map<TSSymbol, std::unordered_map<std::string, uint32_t>>::const_iterator Symbol =
						Counts.find(ts_node_symbol(Child));
						if(Symbol == Counts.end()) continue;
						if(
							const std::unordered_map<std::string, uint32_t>::const_iterator Label =
							Symbol->second.find(DirectiveLabel(Src, Child, GetDirectiveParts(Src, Child)));
							Label != Symbol->second.end()
						) Ordinals.emplace(Child.id, Label->second);
					}
				}
			}
			const std::unordered_map<const void *, uint32_t>::const_iterator Ordinal = Ordinals.find(Target.id);
			const uint32_t Rank = Ordinal == Ordinals.end() ? 0 : Ordinal->second;
			// 適用先の表示を保存した結果の返戻
			return Targets.emplace(Target.id, DirectiveTarget(Src, Target, Rank)).first->second;
		};
		// 原文順のコメントと，後続のコメントでない名前付の兄弟の取得
		for(const TSNode Node : Facts.Nodes) {
			if(Work) Work->Charge(1);
			if(!ts_node_is_named(Node) || ts_node_eq(Node, Root)) continue;
			const TSNode Next = Lookup(Following, Node);
			const std::pair<uint32_t, uint32_t> CommentBounds = Src.GetCommentRangeOf(Node);
			const uint32_t TokenStart = CommentBounds.first, TextEnd = CommentBounds.second;
			// 本文の無いコメントの読飛
			if(TokenStart >= TextEnd) continue;
			const std::string_view Raw = Text.substr(TokenStart, TextEnd - TokenStart);
			const CommentNotation Notation = CommentRepository::NotationOf(Rules, Raw, !TokenStart);
			// ディレクティブコメントでないコメントの読飛
			const std::optional<DirectiveInfo> DirectiveInfo = DirectiveOf(Src, Reading, Head, Raw, TokenStart, Notation, Next);
			if(!DirectiveInfo) continue;
			if(Starts) Starts->push_back(TokenStart);
			// 無効な位置・区切の物も入力時の状態と共に比べ，有効化と記法・本文の変更を見逃さない（整形仕様 8.1）
			Directives.push_back(
				DirectiveInfo->State == DirectiveState::Active ?
				DirectiveInfo->Conditions.front() :
				"inactive " + DirectiveInfo->Conditions.front()
			);
			// 宣言へ作用する Go のディレクティブは適用先も比べ，別の宣言の前へ移って居ない事を確かめる
			if(
				!ts_node_is_null(Next) && Reading.HasToolchainCommentDirectives &&
				(Src.Is(Next, Role::VarDeclNode) || Src.Is(Next, Role::PragmaTargetFunction))
			) Directives.back().append("\n->").append(TargetOf(Next));
			// 後の定数へ有効範囲毎に作用する実行時設定は所有範囲を比べ，別の範囲へ移って居ない事を確かめる（後の文の構文の種類は正規化で変わり得る為に比べない）
			else if(!ts_node_is_null(Next) && DirectiveInfo->Definition == DirectiveDefinitionId::MagicComment) {
				Directives.back().append("\n->").append(DirectiveScope(Src, Next));
			}
		}
		// ディレクティブコメントの一覧の返戻
		return Directives;
	}

	/**
	 * 投影出来ない候補のディレクティブコメントの取得関数
	 * 計算量：共通走査の初回費用 V，頭部の判定費用 H，ディレクティブの収集費用 Q に対し O(V + H + Q + 1)
	 * @param Src 解析済の候補
	 * @param Starts ディレクティブコメントの開始位置の格納先
	 * @param Work コメントの照会数の観測用計数（無ければ計上しない）
	 * @return ディレクティブコメントの本文と位置条件の並び
	 * @throws FormatLimitExceeded 観測用計数の上限を超える場合
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 収集するテキスト又は記録の要素数が格納先の上限を超える場合
	 */
	std::vector<std::string> SourceProjector::CollectDirectives(
		const ::ParsedTree &Src,
		std::vector<uint32_t> *const Starts,
		StructureWorkAccount *const Work
	) {
		// コメントの共通走査の結果の確定
		Src.GetCommentSpans();
		// 候補が無い木では頭部も調べずに終了する事の返戻
		if(!Src.Syntax.Comments || Src.Syntax.Comments->Nodes.empty()) return {};
		// 同じ世代のコメントに対する頭部とディレクティブの返戻
		return CollectDirectives(Src, GetDirectiveHeadOf(Src), Starts, Work);
	}

	/**
	 * アンカー毎のコメントの検索関数
	 * 計算量：アンカーの数 A に対し期待 O(1)，最悪 O(A + 1)
	 * @param Map 参照する表（先行又は末尾）
	 * @param Node 対象のアンカー
	 * @param ShouldFind 識別子の表を引く必要が有るか
	 * @return 紐付いたコメントの列（無ければ共有の空列）
	 */
	const std::vector<CommentRecord> &SourceProjector::CommentSeparation::FindAttached(
		const std::unordered_map<const void *, std::vector<CommentRecord>> &Map,
		const TSNode Node,
		const bool ShouldFind
	) {
		// 未紐付の時に返戻する共有の空列
		static constexpr std::vector<CommentRecord> Empty;
		// 記録番号で未紐付と確定した列の返戻
		if(!ShouldFind) return Empty;
		const std::unordered_map<const void *, std::vector<CommentRecord>>::const_iterator Iter = Map.find(Node.id);
		// アンカーに紐付いたコメントの返戻
		return Iter == Map.end() ? Empty : Iter->second;
	}

	/**
	 * 位置を含む分離したコメントの除く範囲の取得関数（整形仕様 7.1）
	 * 計算量：分離範囲の数 R に対し O(log(R + 1))
	 * @param Pos 原文の位置
	 * @return 位置を含む除く範囲（無ければ nullptr）
	 */
	const SourceProjector::CommentSeparation::Range *SourceProjector::CommentSeparation::FindSeparation(const uint32_t Pos) const {
		// 位置より後に始まる最初の範囲
		const std::vector<Range>::const_iterator After = std::upper_bound(
			Separations.begin(),
			Separations.end(),
			Pos,
			[](const uint32_t At, const Range &Candidate) -> bool {
				// 範囲が位置より後に始まるかの返戻
				return At < Candidate.Begin;
			}
		);
		// 位置より前に始まる範囲が無い場合の返戻
		if(After == Separations.begin()) return nullptr;
		const Range &Prev = *std::prev(After);
		// 位置を含む直前の範囲の返戻
		return Pos < Prev.End ? &Prev : nullptr;
	}

	/**
	 * アンカーへのコメントの紐付関数
	 * 計算量：初回の番号列の確保はノード数 N に対し O(N)，紐付はアンカー数 A に対し期待償却 O(log(A + 1))，最悪 O(N + A + C + 1)（既存記録数 C）
	 * @param Src アンカーの記録番号を所有する同じ世代の解析結果
	 * @param Node 対象のアンカー
	 * @param IsLeading 先行コメントか（偽は末尾コメント）
	 * @param Comment 紐付けるコメント
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws std::out_of_range アンカーの記録番号が同じ世代の列の範囲外の場合
	 */
	void SourceProjector::CommentSeparation::Attach(
		const ::ParsedTree &Src,
		const TSNode Node,
		const bool IsLeading,
		CommentRecord &&Comment
	) {
		// アンカーの位置への記録の追加
		(IsLeading ? LeadingByNode : TrailingByNode)[Node.id].push_back(std::move(Comment));
		AttachedStartBytes.insert(ts_node_start_byte(Node));
		// 表の世代と番号が確定したアンカーだけを直接照会の対象にする
		if(const ParsedChild *const Record = Src.FindRecordedNode(Node); Src.HasRecordedRelations() && Record) {
			if(AttachmentsByRelation.empty()) AttachmentsByRelation.resize(Src.GetRelationWork() + 1);
			AttachmentsByRelation.at(Record->Relation) |= IsLeading ? 1 : 2;
		} else HasUnindexedAttachments = true;
		// 終了
		return;
	}

	/**
	 * 先行コメント取得関数
	 * 計算量：アンカーの数 A に対し期待 O(1)，最悪 O(A + 1)
	 * @param Node 対象ノード
	 * @return 紐付けられたコメント列（無ければ空リストへの参照）
	 */
	const std::vector<CommentRecord> &SourceProjector::CommentSeparation::GetLeading(const TSNode Node) const {
		// 先行の表からの検索の返戻
		return FindAttached(LeadingByNode, Node);
	}

	/**
	 * 記録番号で未紐付の表の検索を省く先行コメント取得関数
	 * 計算量：未紐付なら O(1)，紐付済なら表の検索と同じ
	 * @param Node 同じ木で記録したアンカー
	 * @return 紐付けられたコメント列（無ければ共有の空列）
	 * @throws std::out_of_range アンカーの記録番号が同じ世代の列の範囲外の場合
	 */
	const std::vector<CommentRecord> &SourceProjector::CommentSeparation::GetLeading(const ParsedChild &Node) const {
		// 番号の無いアンカーが在る場合と紐付済の場合だけ識別子で検索する
		return FindAttached(
			LeadingByNode,
			Node.Node,
			HasUnindexedAttachments || !AttachmentsByRelation.empty() && (AttachmentsByRelation.at(Node.Relation) & 1)
		);
	}

	/**
	 * 末尾コメント取得関数
	 * 計算量：アンカーの数 A に対し期待 O(1)，最悪 O(A + 1)
	 * @param Node 対象ノード
	 * @return 紐付けられたコメント列（無ければ空リストへの参照）
	 */
	const std::vector<CommentRecord> &SourceProjector::CommentSeparation::GetTrailing(const TSNode Node) const {
		// 末尾の表からの検索の返戻
		return FindAttached(TrailingByNode, Node);
	}

	/**
	 * 記録番号で未紐付の表の検索を省く末尾コメント取得関数
	 * 計算量：未紐付なら O(1)，紐付済なら表の検索と同じ
	 * @param Node 同じ木で記録したアンカー
	 * @return 紐付けられたコメント列（無ければ共有の空列）
	 * @throws std::out_of_range アンカーの記録番号が同じ世代の列の範囲外の場合
	 */
	const std::vector<CommentRecord> &SourceProjector::CommentSeparation::GetTrailing(const ParsedChild &Node) const {
		// 番号の無いアンカーが在る場合と紐付済の場合だけ識別子で検索する
		return FindAttached(
			TrailingByNode,
			Node.Node,
			HasUnindexedAttachments || !AttachmentsByRelation.empty() && (AttachmentsByRelation.at(Node.Relation) & 2)
		);
	}

	/**
	 * 範囲内紐付コメント有無の判定関数
	 * 計算量：紐付けたコメントの開始位置の数 C に対し O(log(C + 1))
	 * @param From 範囲開始（含む）
	 * @param To 範囲終了（含まず）
	 * @return 範囲内に紐付済コメントが有る場合 true
	 */
	bool SourceProjector::CommentSeparation::HasAttachedCommentInRange(const uint32_t From, const uint32_t To) const {
		// 範囲内の最初のコメント位置の探索
		const std::set<uint32_t>::const_iterator Iter = AttachedStartBytes.lower_bound(From);
		// 範囲に紐付済のアンカーが在るかの返戻
		return Iter != AttachedStartBytes.end() && *Iter < To;
	}

	/**
	 * 分離したコメントのノードかの判定関数（整形仕様 7.1）
	 * 計算量：分離範囲の数 R に対し O(log(R + 1))
	 * @param Src 解析結果
	 * @param Node ノード
	 * @return アンカーへ紐付けて原文のテキストから除くコメントのノードなら true
	 */
	bool SourceProjector::CommentSeparation::IsSeparatedComment(const ::ParsedTree &Src, const TSNode Node) const {
		// 分離したコメントの始まりに一致するコメントのノードかの返戻
		return Src.Is(Node, Role::Comment) &&
		std::binary_search(SeparatedStarts.begin(), SeparatedStarts.end(), ts_node_start_byte(Node));
	}

	/**
	 * 分離したコメントを除く範囲の中の位置かの判定関数（整形仕様 7.1）
	 * 計算量：分離範囲の数 R に対し O(log(R + 1))
	 * @param Pos 原文の位置
	 * @return アンカーへ紐付けて原文のテキストから除く範囲の中なら true
	 */
	bool SourceProjector::CommentSeparation::IsSeparatedAt(const uint32_t Pos) const {
		// 除く範囲の有無の返戻
		return FindSeparation(Pos);
	}

	/**
	 * 除いた跡へ固定の区切記法を置く分離したコメントかの判定関数（整形仕様 6.1, 7.1）
	 * 計算量：分離範囲の数 R に対し O(log(R + 1))
	 * @param Comment 分離したコメントのノード
	 * @return コメントを含む除く範囲の跡へ固定の区切記法を置くなら true
	 */
	bool SourceProjector::CommentSeparation::IsNotationSeparated(const TSNode Comment) const {
		// コメントの分離範囲の取得
		const Range *const Found = FindSeparation(ts_node_start_byte(Comment));
		// 範囲の区切が固定の区切記法かの返戻
		return Found && Found->IsNotation;
	}

	/**
	 * 分離したコメントを除いた範囲のテキストの合成関数（整形仕様 4.3, 7.1）
	 * 除く範囲を跡へ補う区切へ置き換え，範囲の端に掛かる除く範囲は範囲の内側だけを除き，残る原文の連続した断片を其の儘追加関数へ渡す
	 * 計算量：範囲の長さ N と除く範囲の数 K に対し O(N + log(K + 1))
	 * @param From 範囲の始まり
	 * @param To 範囲の終わり
	 * @param Fragment 連続した断片（テキストと始まりと終わり）の追加関数
	 * @param Notations 跡へ置いた区切記法（CSS の空の囲み）の結果の中の範囲の格納先（不要なら nullptr）
	 * @return 分離したコメントを除いたテキスト
	 * @throws std::bad_alloc 合成するテキスト又は区切範囲のメモリを確保出来ない場合
	 * @throws std::length_error 合成するテキスト又は区切範囲が格納先の上限を超える場合
	 */
	template<class Append> std::string SourceProjector::CommentSeparation::JoinFragments(
		const uint32_t From,
		const uint32_t To,
		Append &&Fragment,
		std::vector<std::pair<uint32_t, uint32_t>> *const Notations
	) const {
		// コメントを除いたテキストの格納先
		std::string Text;
		// 空の範囲の返戻
		if(To <= From) return Text;
		std::vector<Range>::const_iterator Found = std::upper_bound(
			Separations.begin(),
			Separations.end(),
			From,
			[](const uint32_t Pos, const Range &Candidate) -> bool {
				// 範囲が位置より後に終わるかの返戻
				return Pos < Candidate.End;
			}
		);
		// 分離範囲を含まない物は，断片の結果を其の儘返す
		if(Found == Separations.end() || Found->Begin >= To) {
			Fragment(Text, From, To);
			// 連続した断片の返戻
			return Text;
		}
		Text.reserve(To - From);
		uint32_t Cursor = From;
		for(; Found != Separations.end() && Found->Begin < To; ++Found) {
			Fragment(Text, Cursor, std::max(Found->Begin, From));
			// 範囲の内側に収まる除く範囲だけが跡へ区切を補う（行頭からの行の区切は前の改行が既に表す為に補わない）
			if(Found->Begin >= From && Found->End <= To && !(Found->Separator == "\n" && Text.ends_with('\n'))) {
				if(Notations && Found->IsNotation) {
					Notations->emplace_back(static_cast<uint32_t>(Text.size()), static_cast<uint32_t>(Text.size() + Found->Separator.size()));
				}
				Text.append(Found->Separator);
			}
			Cursor = std::min(Found->End, To);
		}
		Fragment(Text, Cursor, To);
		// 分離したコメントを除いたテキストの返戻
		return Text;
	}

	/**
	 * 分離したコメントを除いた範囲のテキストの取得関数（整形仕様 4.3, 7.1）
	 * 計算量：分離範囲数 R，対象内の範囲数 K，原文と区切の総バイト数 N に対し O(log(R + 1) + K + N)
	 * @param Src 原文
	 * @param From 範囲の始まり
	 * @param To 範囲の終わり
	 * @param Notations 跡へ置いた区切記法（CSS の空の囲み）の結果の中の範囲の格納先（不要なら nullptr）
	 * @param Work 原文の断片を合成先へ複写したバイト数（無ければ計上しない）
	 * @return 分離したコメントを除いたテキスト
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	std::string SourceProjector::CommentSeparation::BuildSeparatedText(
		const std::string &Src,
		const uint32_t From,
		const uint32_t To,
		std::vector<std::pair<uint32_t, uint32_t>> *const Notations,
		size_t *const Work
	) const {
		// 原文の断片を其の儘繋いだテキストの返戻
		return JoinFragments(
			From,
			To,
			[&Src, Work](std::string &Text, const uint32_t Begin, const uint32_t End) -> void {
				if(Work) *Work += End - Begin;
				// 原文の断片のコピー
				Text.append(Src.data() + Begin, End - Begin);
				// 終了
				return;
			},
			Notations
		);
	}

	/**
	 * 分離したコメントを除き断片毎にデコードした範囲のテキストの取得関数（整形仕様 4.2, 4.4）
	 * 解析用の置換とマーカーは，コメントを除いて不連続に為った断片毎にデコードし，断片を跨いで位置を推定しない
	 * 計算量：分離範囲数 R，対象内の範囲数 K，デコードと追記の総費用 Q に対し O(log(R + 1) + K + Q)
	 * @param Src コメントを分離した解析結果
	 * @param From 範囲の始まり
	 * @param To 範囲の終わり
	 * @param Work 断片の一時コピーと合成先への複写のバイト数（デコーダーの内部は含まず，無ければ計上しない）
	 * @return 分離したコメントを除き本来のトークンへ戻したテキスト
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	std::string SourceProjector::CommentSeparation::GetDecodedText(
		const ::ParsedTree &Src,
		const uint32_t From,
		const uint32_t To,
		size_t *const Work
	) const {
		// 復元データもマーカーも無い本文は，断片の一時コピーを作らず直接合成
		if(Src.GetLexemeRestorations().empty() && !Src.AnalysisMark) return BuildSeparatedText(Src, From, To, nullptr, Work);
		// 断片毎にデコードして繋いだテキストの返戻
		return JoinFragments(
			From,
			To,
			[&Src, Work](std::string &Text, const uint32_t Begin, const uint32_t End) -> void {
				// 解析用の保護をデコードした断片の追記
				const std::string_view Fragment(Src.data() + Begin, End - Begin);
				const std::vector<LexemeRestoration> &Restorations = Src.GetLexemeRestorations();
				const std::vector<LexemeRestoration>::const_iterator First = std::lower_bound(
					Restorations.cbegin(),
					Restorations.cend(),
					Begin,
					[](const LexemeRestoration &Record, const uint32_t Pos) -> bool {
						// 断片より前に在る復元の開始かの返戻
						return Record.Start < Pos;
					}
				);
				if(
					(First == Restorations.cend() || First->Start >= End) &&
					(!Src.AnalysisMark || Fragment.find(Src.AnalysisMark) == std::string_view::npos)
				) {
					Text.append(Fragment);
					if(Work) *Work += Fragment.size();
					// デコードするデータの無い断片の終了
					return;
				}
				std::string Decoded = ParseAdapter::DecodeLexeme(Src, std::string(Fragment), Begin);
				if(Work) *Work += Fragment.size() + (Text.empty() ? 0 : Decoded.size());
				if(Text.empty()) Text = std::move(Decoded);
				else Text.append(Decoded);
				// 終了
				return;
			},
			nullptr
		);
	}

	/**
	 * 埋込ドキュメントの解析用マーカーの除去関数
	 * 終端記号の最初の字の後へ挟んだマーカーだけを除き，本文を其の場で詰める
	 * 計算量：本文のバイト数 B に対し O(B)（終端記号の長さは固定）
	 * @param Text マーカーを挟んだ埋込ドキュメント
	 * @param Marker 解析の為に挟んだ制御文字
	 * @param Work 読取・複写の観測用計数（構文の予算とは独立，要らなければ無い）
	 * @throws FormatLimitExceeded 観測用計数の上限を超える場合
	 * @throws std::bad_alloc 照合する終端記号のメモリを確保出来ない場合
	 * @throws std::length_error 照合する終端記号の長さが文字列の上限を超える場合
	 */
	void SourceProjector::RestoreEmbeddedDoc(std::string &Text, const unsigned char Marker, StructureWorkAccount *Work) {
		const std::string_view Closer = CommentRepository::GetCloserOf(CommentNotation::RubyEmbeddedDocument);
		const std::string Escaped = std::string(1, Closer.front()) + static_cast<char>(Marker) + std::string(Closer.substr(1));
		// 書込位置は読取位置を越えず，未読の照合対象を保つ
		size_t Write = 0;
		for(size_t Read = 0; Read < Text.size();) {
			if(Work) Work->Charge(1);
			size_t Skip = 1;
			if(Text[Read] == Escaped.front()) {
				size_t Matched = 1;
				while(Matched < Escaped.size() && Matched < Text.size() - Read) {
					if(Work) Work->Charge(1);
					if(Text[Read + Matched] != Escaped[Matched]) break;
					++Matched;
				}
				if(Matched == Escaped.size()) Skip = 2;
			}
			if(Write != Read) {
				Text[Write] = Text[Read];
				if(Work) Work->Charge(1);
			}
			++Write;
			Read += Skip;
		}
		Text.resize(Write);
		// 終了
		return;
	}

	/**
	 * 原文からコメントを分離してアンカーへ紐付ける投影関数（整形仕様 3.1, 7.1）
	 * コメントノードを直前／直後の名前付非コメントノードに紐付け，原文を解析し直さずに本文から除く範囲と跡へ補う区切を記録する
	 * 計算量：ソース長 N，ノード数 V，コメント数 K，アンカー判定・本文正規化の総費用 A に対し平均 O(N + V + K * log(K + 1) + A)
	 * @param Src 投影するソース（書き換えない）
	 * @param Input 投影の種別を含む入力
	 * @param Work アンカーの収集のノードと，分解・埋込の分割の記録・行の訪問回数（無ければ計上しない）
	 * @param Observation 隣接表・最後の子で読んだ直接子と統合用の移動の観測値（不要なら nullptr）
	 * @return アンカーへ紐付けたコメントと本文から除く範囲・跡へ補う区切（投影の間だけ持つ）
	 * @throws FormatLimitExceeded 観測用計数の上限を超える場合
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 収集するテキスト又は記録の要素数が格納先の上限を超える場合
	 */
	SourceProjector::CommentSeparation SourceProjector::SeparateComments(
		const ::ParsedTree &Src,
		const ParseInput &Input,
		StructureWorkAccount *const Work,
		std::array<size_t, 3> *const Observation
	) {
		// 分離結果と入力条件の準備
		CommentSeparation Result;
		// 未解析ソースと，コメントがドキュメントの内容であるソースの分離の無い結果の返戻
		if(!Src.IsParsed() || Src.GetReading().HasCommentDocContent) return Result;
		// ノードにもトークンの隙間にもコメントが無く，退避したコメントも無ければ分離は不要
		const std::vector<::ParsedTree::CommentSpan> &CommentSpans = Src.GetCommentSpans();
		const ::ParsedTree::CommentFacts &Facts = *Src.Syntax.Comments;
		// ノード・隙間・退避の何れにもコメントが無い場合の返戻
		if(Facts.Nodes.empty() && Facts.Ranges.empty() && Src.EscapedComments.empty()) return Result;
		const bool IsVerification = std::holds_alternative<ParseInput::VerificationSrc>(Input.Origin);
		const SyntaxRules &Rules = Src.GetRules();
		const ReadingRules &Reading = Src.GetReading();
		const bool HasDirectiveRestorations = Reading.HasPreprocMacros && Src.HasCodeTextDecoding();
		// Ruby の埋込ドキュメントの本文の `=end` へ解析の為に挟んだマーカー（整形仕様 4.2，挟んで居なければ無い）
		const unsigned char DocMarker =
		Rules.Notation.IsAvailable(CommentNotation::RubyEmbeddedDocument) ? Src.GetAnalysisMark() : '\0';
		// 言語標準のディレクティブコメントを判定する頭部の位置（整形仕様 4.5, 7.5）
		const DirectiveHead Head = GetDirectiveHeadOf(Src);
		Result.Head = Head;
		// PHP の閉タグの後の地の文の始まり（其の中のパーサーがコメントと読んだテキストは地の文の儘保つ，整形仕様 4.4）
		const uint32_t BodyEnd = ParseAdapter::GetOuterTextBounds(Src).second;
		// 最後の子・ラッパーの降下先・空の末尾の本体の判定の再利用表
		struct LineScan {
			size_t ChildReads = 0; // 最後の子の照会で読んだ直接子の数
			size_t Visits = 0; // アンカーの収集で訪ねたノード数（スレッド毎の観測値）
			std::pmr::monotonic_buffer_resource Storage; // 再利用表をスレッド毎に一括で解放するメモリ
			std::pmr::unordered_map<const void *, TSNode> LastChild { &Storage }; // コメントでない最後の名前付子
			std::pmr::unordered_map<const void *, std::pair<TSNode, uint16_t>> Descent { &Storage }; // ラッパーから降りたアンカーと越えた無名トークンの数
			std::pmr::vector<std::pair<TSNode, uint16_t>> DescentPath { &Storage }; // 降りたラッパーと其の手前迄に越えた無名トークンの数
			std::pmr::unordered_map<const void *, bool> EmptyFinalBodies { &Storage }; // 所有構文毎の末尾の本体が空か
		};
		// コメント本文の削除範囲
		struct DeleteRange {
			uint32_t Begin;
			uint32_t End;
			bool IsJoinable = false; // 改行を含んでも跡に改行を残さないコメントか（JS / TS は文の中の物，Go ･ Swift は無い，其の他の言語は全て）
			bool IsSpaceLike = false; // 空白と同じに読まれるコメントか（Sass はセレクタの中のコメントを子孫結合子の空白と読む）
		};
		// アンカーの確定後に取り込むコメントの保留記録
		struct Pending {
			uint32_t CommentStart; // コメントの原文の開始位置
			TSNode Anchor {}; // アンカーのノード
			CommentRecord Comment;
			uint32_t CommentEnd = 0; // 原文の終端位置（０は開始位置とアンカーから求める）
			bool ShouldNormalizeLeading = false; // 原文で独立コメントだったか（グループの統合条件に使う）
			bool IsVerbatim = false; // 逐語領域の中へ残す記録か
		};
		// 同じ親の前後の名前付非コメント子と祖先側の前後の再利用情報（１度控えて並列収集中は読取専用とし，兄弟・祖先の再照会に依る３乗費用を回避）
		struct Neighbors {
			TSNode Prev;
			TSNode Next;
			TSNode OuterPrev; // 親から遡った直前の名前付非コメントのノード
			TSNode OuterNext;
			TSNode ParentPrev;
			TSNode ParentNext;
			TSNode Following; // 後に続く最初のコメントでない子（トークンを含み，親の中に無ければコメントと共に終わる祖先の後に続く物）
			bool IsAfterOpener;
			bool IsAfterCloser;
			bool IsAfterToken;
			uint16_t UnnamedBefore; // 親の中の前の名前無のトークンの数（数の上限で飽和，以下同じ）
			uint16_t UnnamedAfterPrev; // Prev との間の名前無のトークンの数
			uint16_t UnnamedToNext; // Next との間の名前無のトークンの数
			uint16_t HiddenToNext; // Next との間の文法が子に出さない隠れたトークンの数（エラーを含む親の読み飛ばしたトークンは数えない）
			bool IsCommentOnlyExpr; // 名前付子がコメントだけの式ラッパーか（親の走査で１度確定）
		};
		// 並列処理前の隣接関係確定と各スレッドからの読取専用化
		std::pmr::monotonic_buffer_resource AdjacencyStorage;
		std::pmr::unordered_map<const void *, Neighbors> Adjacent(&AdjacencyStorage);
		// 位置表と，コメントを持つ祖先は並列処理前の共通走査が確定した結果を共有する
		const std::pmr::unordered_set<const void *> &ParentsWithComments = Facts.Parents;
		// 祖先経路と最後の子を再利用し，コメントの保留分・削除範囲をスレッド別に収集
		const auto LastChildOf = [&Src, Observation](const TSNode Node, LineScan &Memo) -> TSNode {
			// 非コメント末尾子の取得と保持
			const std::pair<std::pmr::unordered_map<const void *, TSNode>::iterator, bool> Entry = Memo.LastChild.try_emplace(Node.id);
			if(Entry.second) {
				const std::span<const ParsedChild> Children = Src.GetChildrenOf(Node);
				if(Observation) Memo.ChildReads += Children.size();
				for(const ParsedChild &Child : Children) if(ts_node_is_named(Child.Node) && !Src.Is(Child.Node, Role::Comment)) {
					Entry.first->second = Child.Node;
				}
			}
			// コメントでない最後の名前付の子の返戻
			return Entry.first->second;
		};
		// コメント毎のアンカーと除去範囲の決定
		const auto ProcessComment =
		[&Src, DocMarker, Head, BodyEnd, HasDirectiveRestorations, &Adjacent, &CommentSpans, &LastChildOf, &Rules, &Reading](
			const TSNode Node,
			const std::span<const TSNode> Ancestors,
			std::vector<Pending> &PendingList,
			std::vector<DeleteRange> &DeleteRanges,
			std::vector<uint32_t> &Separated,
			LineScan &LastScan
		) -> void {
			// 親と祖父（根の直下のコメントは祖父を持たず，根其の物のコメントは親も持たない）
			const TSNode NodeParent = Ancestors.empty() ? TSNode{} : Ancestors.back();
			// コメントの祖父ノード
			const TSNode NodeGrandparent = Ancestors.size() > 1 ? Ancestors[Ancestors.size() - 2] : TSNode{};
			// コメントを含む原文
			const std::string &Self = Src;
			// 本文の範囲（末尾の改行とエラー回復で頭へ付いた空白を除き，幅０のノードは直前のブロックコメントの本文を回収する）
			const std::pair<uint32_t, uint32_t> CommentBounds = Src.GetCommentRangeOf(Node);
			const uint32_t TokenStart = CommentBounds.first, TextEnd = CommentBounds.second;
			// 回収出来ない幅０のノードと，PHP の本体の外の地の文の中のテキストを扱わない事の終了
			if(TokenStart >= TextEnd || TokenStart >= BodyEnd) return;
			// 以下の何れの経路も本文を原文のテキストから除き，投影は此のノードを読み飛ばす
			Separated.push_back(ts_node_start_byte(Node));
			// 同じ親の直前・直後の名前付非コメント子と，同じ親に無ければ親から遡った隣接と行頭のコメントか（控えた物を引く）
			const std::pmr::unordered_map<const void *, Neighbors>::const_iterator Neighbor = Adjacent.find(Node.id);
			const Neighbors Nearby = Neighbor == Adjacent.end() ? Neighbors{} : Neighbor->second;
			// 原文の連続領域
			const char *const SelfData = Self.data();
			const uint32_t SelfSize = static_cast<uint32_t>(Self.size());
			// 括弧・ブロックの所有構文の取得（ラッパーは祖父，直置のブロックは自身，括弧を直に持つ構文は親）
			const auto BlockOwner = [&]() -> TSNode {
				// 持主の構文の返戻
				return IsOpenBracketChar(SelfData[Src.GetStart(NodeParent)]) && !ts_node_is_null(NodeGrandparent) &&
				!Src.Is(NodeGrandparent, Role::StmtHost) ? NodeGrandparent : NodeParent;
			};
			// 語だけの `then`・`do`（空の本体），又は其れで終わる節か
			const auto DoesEndWithEmptyKeywordBody = [&Src](const TSNode Node) -> bool {
				// 所有構文の末尾の子の取得
				const TSNode Last = ts_node_child_count(Node) ? ts_node_child(Node, ts_node_child_count(Node) - 1) : TSNode{};
				// 空の本体か其れで終わる節かの返戻
				return Src.Is(Node, Role::TransparentSection) && !ts_node_named_child_count(Node) ||
				!ts_node_is_null(Last) && Src.Is(Last, Role::TransparentSection) && !ts_node_named_child_count(Last);
			};
			// 畳んで空に為る末尾の本体を持つ所有構文
			const auto EmptyClauseOwner = [&]() -> TSNode {
				// 空の本体を所有する構文の取得
				const TSNode Owner = BlockOwner();
				const std::pair<std::pmr::unordered_map<const void *, bool>::iterator, bool> Entry =
				LastScan.EmptyFinalBodies.try_emplace(Owner.id);
				if(Entry.second) Entry.first->second = HasEmptyFinalBody(Src, Owner);
				// 空の末尾の本体を持つ所有構文の返戻
				return Entry.first->second ? Owner : TSNode{};
			};
			// 原文でのコメントの種類（コメントの分離と `KindOf` で同じ規則を使う）
			bool IsAtLineStart = false;
			uint32_t AfterComments;
			const CommentKind Kind = Src.ClassifyComment(CommentSpans, TokenStart, TextEnd, &IsAtLineStart, &AfterComments);
			// 行頭に在っても後ろに同じ行のコードが続くコメントは其のコードの説明の為，行内のコメントとして後ろの要素へ紐付ける
			const bool IsLeading = Kind == CommentKind::OwnLine, IsInline = Kind == CommentKind::Inline;
			// 原文から除く範囲の終端
			uint32_t DeleteEnd = TextEnd;
			if(IsLeading && DeleteEnd < Self.size() && Self[DeleteEnd] == '\n') ++DeleteEnd;
			// 本文の無い CSS / SCSS の囲みはトークンの区切の記法で，除いた跡に要れば区切を置き直す為に記録しない（整形仕様 6.1, 6.6）
			if(
				!Rules.LexicalSeparator.empty() && std::string_view(Src.data() + TokenStart, TextEnd - TokenStart) == Rules.LexicalSeparator
			) {
				DeleteRanges.push_back({ TokenStart, TextEnd });
				// トークンの区切を記録した場合の終了
				return;
			}
			CommentRecord Attach;
			// 解析用に置換した範囲を戻し，記法と本文で同じ復元済のテキストを使う
			Attach.Text.assign(Src.data() + TokenStart, TextEnd - TokenStart);
			if(HasDirectiveRestorations) Attach.Text = ParseAdapter::DecodeLexeme(Src, std::move(Attach.Text), TokenStart);
			const std::string_view Raw(Attach.Text);
			// 記号と本文の境界を決める記法（整形仕様 3.2，原文の記号の形だけを表す）
			const CommentNotation Notation = CommentRepository::NotationOf(Rules.Notation, Raw, !TokenStart);
			// 言語標準のディレクティブコメントは本文・記法・位置を保つ（整形仕様 4.5, 7.5，同じ形の普通の説明は位置で見分ける）
			Attach.Directive = DirectiveOf(Src, Reading, Head, Raw, TokenStart, Notation, Nearby.Next);
			Attach.Role = Attach.Directive ? CommentRole::Directive : CommentRole::Presentation;
			Attach.Notation = Notation;
			CommentRepository::AttachRules(Rules.Notation, Attach);
			// Ruby の埋込ドキュメントの本文へ解析の為に挟んだマーカーを除き，本文を原文へ戻す (`ParseAdapter::EscapeRubyEmbeddedDoc`)
			if(DocMarker && Raw.starts_with(CommentRepository::GetOpenerOf(CommentNotation::RubyEmbeddedDocument))) {
				RestoreEmbeddedDoc(Attach.Text, DocMarker);
			}
			// 独立コメントの先行紐付の既定値
			Attach.Placement.Side = IsLeading ? CommentPlacement::Before : CommentPlacement::After;
			// 原文での種別（整形仕様 7.6 に従い整形の間中保持する，所属から組み直すと種別の意味を変えた時に静かにずれる）
			Attach.Kind = Kind;
			// 後段で付ける原文順
			Attach.OriginalOrder = 0;
			// Go の `/*line` が位置を定める後続トークンとの密着の保持
			if(Reading.HasPosDirectiveComments && Attach.Role == CommentRole::Directive) {
				Attach.Placement.IsGluedAfter =
				Notation == CommentNotation::GoLineBlock && TextEnd < Self.size() && Self[TextEnd] != ' ' && Self[TextEnd] != '\n';
			}
			// 直前・直後の兄弟（無ければ親から遡った直前・直後の兄弟）
			const TSNode PrevOrOuter = ts_node_is_null(Nearby.Prev) ? Nearby.OuterPrev : Nearby.Prev;
			const TSNode NextOrOuter = ts_node_is_null(Nearby.Next) ? Nearby.OuterNext : Nearby.Next;
			// 子文字列境界を保つコメントだけの JSX 式ラッパーのアンカー化
			if(Nearby.IsCommentOnlyExpr) {
				Attach.Placement.Side = CommentPlacement::Before;
				PendingList.push_back({ TokenStart, NodeParent, std::move(Attach), TextEnd, IsLeading });
				// ラッパー内本文の除去範囲
				DeleteRanges.push_back({ TokenStart, TextEnd });
				// 最終出力時にラッパー内へ戻すコメント本文だけを分離した事の終了
				return;
			}
			// セレクタコメントの規則前への移動と CSS / SCSS の空白の意味・数値と単位の境界の保持
			if(Rules.IsWhitespaceSemantic) {
				if(!ts_node_is_null(NodeGrandparent) && Src.Is(NodeParent, Role::DimensionNum)) {
					// 数と単位の間のコメントは値を含むノードの後へ移し，除いた跡でトークンが融合すれば区切記法を置く（整形仕様 6.1, 6.6）
					DeleteRanges.push_back({ TokenStart, TextEnd });
					// CSS 数値内コメントの保留情報（値を含むノードの末尾コメントとして紐付ける）
					Pending Entry;
					Entry.CommentStart = TokenStart;
					Entry.CommentEnd = TextEnd;
					Entry.ShouldNormalizeLeading = IsLeading;
					Entry.Anchor = NodeGrandparent;
					Attach.Placement.Side = CommentPlacement::After;
					// 復元情報の保留
					Entry.Comment = std::move(Attach);
					PendingList.push_back(std::move(Entry));
					// 値を含むノードの末尾コメントとして紐付けた後の終了
					return;
				}
				// セレクタの並びの祖先の位置の次（無ければ 0，規則は其の親）
				size_t SelectorsNext = 0;
				for(size_t Up = Ancestors.size(); Up--;) {
					if(Src.Is(Ancestors[Up], Role::SelectorList)) {
						SelectorsNext = Up + 1;
						break;
					}
					if(Src.Is(Ancestors[Up], Role::ScopeBody)) break;
				}
				if(const TSNode Rule = SelectorsNext > 1 ? Ancestors[SelectorsNext - 2] : TSNode{}; !ts_node_is_null(Rule)) {
					DeleteRanges.push_back({ TokenStart, TextEnd, false, Reading.CanSelectorCommentActAsSpace });
					// セレクタコメントの保留情報
					Pending Entry;
					Entry.CommentStart = TokenStart;
					Entry.CommentEnd = TextEnd;
					Entry.ShouldNormalizeLeading = IsLeading;
					Entry.Anchor = Rule;
					Attach.Placement.Side = CommentPlacement::Before;
					Entry.Comment = std::move(Attach);
					PendingList.push_back(std::move(Entry));
					// 規則の前の先行コメントとして紐付けた後の終了
					return;
				}
			}
			// Swift 範囲は片側空白の誤読を避け式末尾へ，前後のトークンへ密着する CSS 値の中の物は宣言の末尾へ紐付
			// 空白の無いトークンの間へ空白と共に差し込むとトークンの並びが変わり，値のトークンをアンカーにすると区切を置いた出力を読み直す時にアンカーを失う（整形仕様 6.6）
			TSNode Anchor = IsInline && Src.Is(NodeParent, Role::SpacingSensitiveRange) ? NodeParent : TSNode{};
			// 前後のトークンへ密着するか，識別子に接して出力を読み直すと値のトークンが組み変わり得る物
			const bool IsGlued = TokenStart && !std::isspace(static_cast<unsigned char>(SelfData[TokenStart - 1])) && TextEnd < SelfSize &&
			!std::isspace(static_cast<unsigned char>(SelfData[TextEnd])) ||
			IsNamedNode(Nearby.Prev) && Src.Is(Nearby.Prev, Role::Identifier) ||
			IsNamedNode(Nearby.Next) && Src.Is(Nearby.Next, Role::Identifier);
			for(size_t Up = Ancestors.size(); IsInline && Rules.IsWhitespaceSemantic && IsGlued && ts_node_is_null(Anchor) && Up--;) {
				if(Src.Is(Ancestors[Up], Role::TypedDecl)) Anchor = Ancestors[Up];
				else if(Src.Is(Ancestors[Up], Role::ScopeBody)) break;
			}
			if(!ts_node_is_null(Anchor)) Attach.Placement.Side = CommentPlacement::After;
			// 行内：直接兄弟優先で解決し，見付からなければ後段の汎用代替経路（祖先兄弟 → ルート）でアンカー化
			if(IsInline && ts_node_is_null(Anchor)) {
				// 区切・閉前は直前兄弟の説明として末尾紐付（空実引数内は被呼出側へ）
				// 解析用に補った文の区切も，出力で其処へ `;` を置き（`;` を省く記法を除く），同じ行の前のトークンの後の改行で読みが変わらなければ区切と同じに扱う
				const uint32_t BeforeComment = TextEdit::SkipSpaceLeft(Self, Reading, TokenStart);
				const uint32_t PreviousEnd = Nearby.UnnamedAfterPrev || ts_node_is_null(Nearby.Prev) ? 0 : Src.GetEnd(Nearby.Prev);
				const bool IsAfterTerminator =
				BeforeComment && Src.GetProfile().IsSpelling(std::string_view(Self).substr(BeforeComment - 1, 1), Word::StmtEnd) ||
				PreviousEnd && Src.GetProfile().IsSpelling(std::string_view(Self).substr(PreviousEnd - 1, 1), Word::StmtEnd);
				const uint32_t Next = AfterComments;
				if(
					const bool IsBeforeInsertedBreak = !Rules.CanOmitTrailingSemicolon && !IsAtLineStart && !IsAfterTerminator && Next < SelfSize &&
					std::binary_search(Src.GetInsertedBreaks().begin(), Src.GetInsertedBreaks().end(), Next) &&
					!Src.CanChangeReadingAt(TextEdit::SkipSpaceLeft(Self, Reading, TokenStart), Next);
					Next < SelfSize && (IsListSeparatorChar(SelfData[Next]) || IsCloseBracketChar(SelfData[Next]) || IsBeforeInsertedBreak)
				) {
					// 独立した空文の区切は其の先行，宣言を終える区切は直前の内容の末尾
					const TSNode Separator = Src.Is(NodeParent, Role::EmptyStmtContainer) && Src.Is(Nearby.Following, Role::StmtTerminatorToken) ?
					Nearby.Following :
					Nearby.Next;
					if(!ts_node_is_null(Separator) && Src.GetStart(Separator) == Next) {
						Anchor = Separator;
						Attach.Placement.Side = CommentPlacement::Before;
					} else if(!ts_node_is_null(Nearby.Prev) && !(IsAtLineStart && IsCloseBracketChar(SelfData[Next]))) {
						// 親の中に前の兄弟が在れば其の末尾（間の無名トークン `label:` の `:` 等の数を控え，其のトークンの後へ置く，行頭で閉トークンの前の物は其の閉じの説明の為に除く）
						Anchor = Nearby.Prev;
						Attach.Placement.TokensBefore = Nearby.UnnamedAfterPrev;
					} else if(
						// 空の括弧（実引数の無い呼出）の中は，括弧が外れても残る様に祖先から遡った括弧の直前の被呼出側の末尾
						!ts_node_is_null(NodeParent) && ts_node_is_null(Nearby.Prev) && ts_node_is_null(Nearby.Next) &&
						IsParenthesized(Src.GetView(NodeParent)) && !ts_node_is_null(Nearby.OuterPrev) &&
						TextEdit::SkipSpaceLeft(Self, Reading, Src.GetStart(NodeParent)) == Src.GetEnd(Nearby.OuterPrev)
					) Anchor = Nearby.OuterPrev;
					// 其の他は祖先から遡ると前の枝・文へ移る為，親の中の区切・閉じのトークンの前（其の前の親の名前無のトークンの数を控える）
					else if(!ts_node_is_null(NodeParent)) {
						Anchor = NodeParent;
						Attach.Placement.Side = CommentPlacement::Inside;
						Attach.Placement.TokensBefore = Nearby.UnnamedBefore;
					}
				}
				if(ts_node_is_null(Anchor)) {
					// 名前付の兄弟が無い場合も，同じ親の内部のトークン境界へ結び付ける
					if(
						ts_node_is_null(Nearby.Next) && ts_node_is_null(Nearby.Prev) && !ts_node_is_null(Nearby.Following) &&
						!IsNamedNode(Nearby.Following) && ts_node_eq(ts_node_parent(Nearby.Following), NodeParent)
					) {
						Anchor = NodeParent;
						Attach.Placement.Side = CommentPlacement::Inside;
						Attach.Placement.TokensBefore = Nearby.UnnamedBefore;
					} else {
						Anchor = Nearby.Next;
						// 直後の兄弟を説明する `g(/* 個数 */ 3)` 等は先行紐付し，配置でアンカーの直前の行内へ置く（行末へ移さない）
						if(!ts_node_is_null(Anchor)) Attach.Placement.Side = CommentPlacement::Before;
						else Anchor = Nearby.Prev;
					}
				}
			} else if(IsLeading) {
				// 直接後兄弟→祖先側の順に先行アンカーを探索（Ruby の節前・空本体の閉前は本体末尾）
				const bool IsBeforeKeywordClause = Reading.HasKeywordClosedBlocks && !ts_node_is_null(Nearby.Next) && (
					Src.Is(Nearby.Next, Role::ClauseHeading) ||
					Src.Is(Nearby.Next, Role::BodyTriggerWord) && !ts_node_named_child_count(Nearby.Next)
				);
				// PHP の空の代替節のコメントは次の枝へ移さず，見出後へ独立配置
				const TSNode ColonBody = ts_node_is_null(Nearby.Prev) || !Src.Is(Nearby.Prev, Role::IfFollowingBranch) ?
				Nearby.Prev :
				LastChildOf(Nearby.Prev, LastScan);
				const bool IsInEmptyColonBody =
				!ts_node_is_null(ColonBody) && Src.Is(ColonBody, Role::ColonBody) && !ts_node_named_child_count(ColonBody);
				// 直後が閉トークンなら，後の節の先行でなくブロックの末尾として紐付
				Anchor =
				Src.Is(Nearby.Following, Role::PreprocDirectiveClose) ? Nearby.Following : IsBeforeKeywordClause || IsInEmptyColonBody ||
				!ts_node_is_null(Nearby.Following) && !IsNamedNode(Nearby.Following) && Src.Is(Nearby.Following, Role::CloserToken) ?
				TSNode{} :
				Nearby.Next;
				// Go の最上位の宣言との間に空行を挟む通常コメントは，godoc のドキュメント・cgo の前文として取り込まれない様，前の兄弟の後の独立した行へ置く（整形仕様 7.6）
				// cgo の前文の判定と共有する原文の空行の位置
				if(
					Reading.HasCommentAbsorbingDecls && Attach.Role == CommentRole::Presentation && !ts_node_is_null(Anchor) &&
					!ts_node_is_null(Nearby.Prev) && Src.CanAbsorbGoComment(Anchor) && Src.Is(ts_node_parent(Anchor), Role::SrcFileNode) &&
					Src.HasBlankLineBetween(TextEnd, Src.GetStart(Anchor))
				) {
					Anchor = Nearby.Prev;
					Attach.Placement.Side = CommentPlacement::After;
				}
				if(ts_node_is_null(Anchor)) {
					// 祖先側も含め構文木の後続トークンで末尾判定し，Ruby の次節も閉扱
					bool IsScopeEnd = IsBeforeKeywordClause || IsInEmptyColonBody || (
						!ts_node_is_null(Nearby.Following) && ts_node_is_named(Nearby.Following) ?
						Reading.HasKeywordClosedBlocks && Src.Is(Nearby.Following, Role::ClauseHeading) :
						Src.Is(Nearby.Following, Role::CloserToken)
					);
					// 手前の兄弟が見出の部品（条件・名前・仮引数等）かの判定
					// 本体のノードを持たない構文（Ruby の `if`~`end`）はコメントが見出の部品の兄弟に為り，ブロックの末尾でなくコメント自身が本体に当たる
					// スコープ末尾は直前兄弟後の独立行へ固定（見出との間に開きが在れば空ブロック扱い）
					if(
						IsScopeEnd && !ts_node_is_null(Nearby.Following) && !ts_node_is_null(Nearby.Prev) && DoesEndWithEmptyKeywordBody(Nearby.Prev)
					) {
						// 語だけの `then` / `do` の後の空の本体の物は，後に続く境界（閉じの語か後続の節）へ結び付ける（整形仕様 7.1, 7.2）
						Anchor = Nearby.Following;
						Attach.Placement.Side = CommentPlacement::Before;
						Attach.Placement.IsEmptyBody = true;
						IsScopeEnd = false;
					} else if(
						(IsScopeEnd || !ts_node_is_null(NodeParent) && Src.Is(NodeParent, Role::SequentialStmts)) && !ts_node_is_null(Nearby.Prev) &&
						!Nearby.IsAfterOpener && (IsOpenBracketChar(SelfData[Src.GetStart(NodeParent)]) || !Src.IsHeaderField(NodeParent, Nearby.Prev))
					) {
						// ブロックの閉じの後から始まる節の語（Go の裸のブロック・Kotlin の本体のコンテナは節のノードに為らない為，語で見分ける）
						const uint32_t AfterCloser =
						ts_node_is_null(Nearby.Following) ? SelfSize : TextEdit::SkipSpaceRight(Self, Reading, Src.GetEnd(Nearby.Following));
						const bool IsBeforeSectionWord = AfterCloser < SelfSize &&
						Src.Is(ts_node_descendant_for_byte_range(Src.GetRoot(), AfterCloser, AfterCloser + 1), Role::SectionKeyword);
						// 後続節が親の後に続くか
						const bool HasParentClause =
						IsBeforeSectionWord || !ts_node_is_null(Nearby.ParentNext) && Src.Is(Nearby.ParentNext, Role::FollowingBodyClause);
						// 後続節の直前のコメントは，原文のインデントに依らず其の節へ所属させる（節の見出の深さに置く，整形仕様 7.6，畳んで空に為る節は其の節の中）
						const TSNode NextClause = HasParentClause ? Nearby.ParentNext : Nearby.OuterNext, EmptyClause = EmptyClauseOwner();
						const bool IsEmptyClause = !ts_node_is_null(EmptyClause);
						const bool IsClauseComment =
						IsEmptyClause || IsBeforeSectionWord || !ts_node_is_null(NextClause) && Src.Is(NextClause, Role::FollowingBodyClause);
						// 畳んで空に為る節の物は，後に続く境界（閉じの語か後続の節）へ結び付ける（整形仕様 7.1, 7.2）
						Anchor = IsEmptyClause && !ts_node_is_null(Nearby.Following) ?
						Nearby.Following :
						IsEmptyClause ? EmptyClause : IsClauseComment && !HasParentClause ? BlockOwner() : Nearby.Prev;
						Attach.Placement.IsEmptyBody = IsEmptyClause;
						Attach.Placement.Side = IsEmptyClause ? CommentPlacement::Before : CommentPlacement::After;
						IsScopeEnd = false;
						// 直前の節の最後の文の後へ配置し，文が無ければ見出後へ独立配置
						if(!IsClauseComment) {
							while(Src.Is(Anchor, Role::ClauseHeading) || Src.Is(Anchor, Role::BodyEndingClause)) {
								const TSNode Last = LastChildOf(Anchor, LastScan);
								// 本体の文の無い節の見出での停止（ネストのラベル `a: b: for(…)` はインデントを戻す行の外の本体迄降りる）
								if(
									ts_node_is_null(Last) || ts_node_eq(Last, Src.GetFieldChild(Anchor, ChildRole::Val)) || Src.Is(Last, Role::ClauseHeading) &&
									!(Src.Is(Anchor, Role::LabeledStmt) && IsNamedNode(Last) && Src.Is(Last, Role::LabeledStmt))
								) break;
								Anchor = Last;
							}
						}
					}
					// コメントだけのブロックの物は，其のブロックを所有する構文をアンカーの候補とする（以下で閉じのトークン又は所有構文へ結び付ける）
					if(IsScopeEnd && !ts_node_is_null(NodeParent)) {
						Anchor = BlockOwner();
						// 空の本体・括弧の物は其の終端境界（閉じのトークン）へ結び付け，其の閉じの在る行の前へ置く（整形仕様 7.1, 7.2, 7.6）
						// ドキュメントを付けれる定義の波括弧の本体の物は定義の先行コメントとし，定義のドキュメントへ同じ回で統合する（整形仕様 7.4）
						if(
							Src.Is(Nearby.Following, Role::CloserToken) && !(
								Nearby.IsAfterOpener && !Reading.HasKeywordClosedBlocks && Src.Is(Anchor, Role::DocumentedDef) &&
								!Rules.Notation.IsInnerDoc(Attach.Notation)
							)
						) Anchor = Nearby.Following;
						// 閉じのトークンと波括弧で本体を開く構文は畳むと其の行に残る為に行の前へ置き，開括弧の無い構文は見出の後の独立した行へ置く
						Attach.Placement.Side =
						!ts_node_eq(Anchor, Nearby.Following) && (Src.Is(Anchor, Role::ClauseHeading) || !Nearby.IsAfterOpener) ?
						CommentPlacement::After :
						CommentPlacement::Before;
						// 畳む本体の中のコメントのマーカー（配置の段がアンカーの頭でなく畳んだ本体の行を基準にする）
						Attach.Placement.IsEmptyBody = true;
					}
					if(ts_node_is_null(Anchor)) Anchor = NextOrOuter;
				}
				// 前後のアンカーが無ければ根の末尾のコメントにする（直前の兄弟へ付けると先行コメントとして先頭へ出るか行内へ入る為）
				if(ts_node_is_null(Anchor)) {
					Anchor = Src.GetRoot();
					// 根への先行紐付は先頭へ出る為に末尾紐付へ替える
					if(Attach.Placement.Side == CommentPlacement::Before) Attach.Placement.Side = CommentPlacement::After;
				}
			} else if(!ts_node_is_null(Nearby.Next) && Nearby.IsAfterToken && (ts_node_is_null(Nearby.Prev) || Nearby.IsAfterCloser)) {
				// 見出行末の後の作用対象への先行紐付と構文編集後の配置の保持
				const bool IsBranchHeader =
				Src.Is(NodeParent, Role::PreprocBranch) && ts_node_start_point(NodeParent).row == ts_node_start_point(Node).row;
				Anchor = IsBranchHeader ? ts_node_child(NodeParent, 0) : NextOrOuter;
				Attach.Placement.Side = IsBranchHeader ? CommentPlacement::After : CommentPlacement::Before;
			} else {
				// 空のブロック・Ruby の空の `begin` は所有構文の末尾へ固定し，前の文への移動を防ぐ
				if(
					!ts_node_is_null(NodeParent) && (
						Src.Is(NodeParent, Role::BracedStmtBlock) && ts_node_is_null(LastChildOf(NodeParent, LastScan)) ||
						Nearby.IsAfterToken && ts_node_is_null(Nearby.Prev) && ts_node_is_null(Nearby.Next) &&
						!IsOpenBracketChar(SelfData[Src.GetStart(NodeParent)]) && !Src.Is(NodeParent, Role::IterationSection)
					)
				) Anchor = BlockOwner();
				// 閉後はブロック又は直持構文へ末尾紐付（同親の後続節が在れば直前兄弟側に保持）
				if(ts_node_is_null(Anchor) && Nearby.IsAfterCloser && ts_node_is_null(Nearby.Next)) Anchor = NodeParent;
				// 末尾アンカーは直前兄弟を優先し，制御見出と本体の間だけ直後兄弟にも退避
				if(ts_node_is_null(Anchor)) Anchor = PrevOrOuter;
				// 空括弧は同行の直前の名前・捕捉へ紐付（兄弟無は現アンカーを保持して除去時だけ再紐付）
				if(!ts_node_is_null(Anchor)) {
					if(
						const std::string_view Text = Src.GetView(Anchor);
						IsParenthesized(Text) && Text.find_first_not_of(" \t\n", 1) == Text.size() - 1
					) {
						if(
							const TSNode Before = Src.GetSiblingBeforeImmediate(Anchor);
							!ts_node_is_null(Before) &&
							!std::memchr(SelfData + ts_node_end_byte(Before), '\n', TokenStart - std::min(TokenStart, ts_node_end_byte(Before)))
						) Anchor = Before;
					}
				}
				// 空括弧の開き後は同じ行から始まる所有文へ末尾紐付し，文の並びの外へは遡らない
				if(!ts_node_is_null(Anchor) && !ts_node_is_null(NodeParent)) {
					if(
						const uint32_t Opener = TextEdit::SkipCharsLeftBounded(Self, TokenStart, 0, " \t"), AnchorEnd = ts_node_end_byte(Anchor);
						Opener && IsOpenBracketChar(SelfData[Opener - 1]) && AnchorEnd < Opener && ts_node_is_null(Nearby.ParentPrev)
					) {
						const uint32_t LineStart = TextEdit::GetLineStartOf(Self, Opener);
						// 親が文の並び又は根に届く迄の祖先位置に依る遡行
						size_t OwnerAt = Ancestors.size() - 1;
						bool IsStmt = false;
						while(OwnerAt) {
							const TSNode Up = Ancestors[OwnerAt - 1];
							IsStmt = OwnerAt == 1 || Src.Is(Up, Role::SequentialStmts);
							if(IsStmt || ts_node_start_byte(Up) < LineStart) break;
							--OwnerAt;
						}
						if(
							const TSNode Owner = Ancestors[OwnerAt];
							IsStmt && ts_node_start_byte(Owner) >= LineStart && AnchorEnd <= ts_node_start_byte(Owner)
						) Anchor = Owner;
					}
				}
				if(ts_node_is_null(Anchor)) Anchor = NextOrOuter;
				// 制御構文の同行末尾アンカーは本体へ降り，ラッパー後に閉波括弧へ移る事を防止
				if(!ts_node_is_null(Anchor)) {
					// ２位置の間の改行の有無だけを調べ，同じ行かを判定（位置の差を D として O(D)，全体の走査を避ける）
					const auto IsSameLineAs = [&Self](const uint32_t FirstPos, const uint32_t SecondPos) -> bool {
						// ２位置の間の原文内の区間
						const uint32_t Low = std::min(FirstPos, SecondPos), High = std::max(FirstPos, SecondPos);
						const uint32_t Limit = std::min<uint32_t>(High, static_cast<uint32_t>(Self.size()));
						// 同行判定（区間内に改行を含まない）で真の返戻
						return Limit <= Low || !std::memchr(Self.data() + Low, '\n', Limit - Low);
					};
					while(Src.Is(Anchor, Role::Control)) {
						// コメントより前で同行の最後の名前付子を線形探索（直下の閉後は構文末尾の儘）
						TSNode Next {};
						ForEachChild(
							Anchor,
							[&](const TSNode Child) -> bool {
								// アンカーの非コメント子の探索でコメントを読み飛ばす事の返戻
								if(Src.Is(Child, Role::Comment)) return true;
								const uint32_t ChildEnd = ts_node_end_byte(Child);
								// トークンより後の子で打ち切る事の返戻
								if(ChildEnd > TokenStart) return false;
								if(!IsNamedNode(Child) && Src.Is(Child, Role::ClosingBraceToken)) Next = TSNode{};
								else if(ts_node_is_named(Child) && IsSameLineAs(ChildEnd, TokenStart)) Next = Child;
								// 次の子の走査を続ける事の返戻
								return true;
							}
						);
						if(ts_node_is_null(Next)) break;
						Anchor = Next;
					}
				}
			}
			// 直接兄弟で未発見なら祖先側の兄弟へ退避（整形への影響を防ぐ為，コメント本文は必ずソースから除去）
			if(ts_node_is_null(Anchor)) Anchor = NextOrOuter;
			if(ts_node_is_null(Anchor)) Anchor = PrevOrOuter;
			// アンカーが無ければ根へ末尾紐付し，コメントだけのソースも本文に残さない
			if(ts_node_is_null(Anchor)) Anchor = Src.GetRoot();
			// アンカーとの間の無名トークンの計数と先行コメントの其のトークンより前への配置（トークンとの順序を保持）
			if(
				Attach.Placement.Side == CommentPlacement::Before && !Rules.IsWhitespaceSemantic && !ts_node_is_null(Nearby.Next) &&
				ts_node_eq(Anchor, Nearby.Next)
			) {
				// 間隙にだけ残る隠れたトークンも数える（投影時に無名トークンのノードにする）
				Attach.Placement.TokensBefore =
				static_cast<uint16_t>(std::min<uint32_t>(Nearby.UnnamedToNext + Nearby.HiddenToNext, std::numeric_limits<uint16_t>::max()));
			}
			// 先行アンカーだけの透過ラッパーの解包
			const TSNode RootAnchor = Src.GetRoot();
			// 閉トークンの無い文の並び・末尾改行を含むノードは最後の文へ紐付し，閉行への移動を防止
			while(
				Attach.Placement.Side != CommentPlacement::Before && Attach.Placement.Side != CommentPlacement::Inside &&
				!Nearby.IsAfterCloser && !ts_node_is_null(Anchor) && !ts_node_eq(Anchor, RootAnchor)
			) {
				if(
					const uint32_t Count = ts_node_child_count(Anchor), AnchorEnd = ts_node_end_byte(Anchor);
					!(Count && ts_node_is_named(ts_node_child(Anchor, Count - 1)) && Src.Is(Anchor, Role::TransparentWrapper)) &&
					!(AnchorEnd > ts_node_start_byte(Anchor) && SelfData[AnchorEnd - 1] == '\n')
				) break;
				const TSNode LastChild = LastChildOf(Anchor, LastScan);
				if(ts_node_is_null(LastChild)) break;
				Anchor = LastChild;
			}
			// ラッパーより前のコメントに対する降下先と，降りて越えるラッパーの無名トークン (`{`) の数の再利用（配置の段は波括弧が残れば其の前へ置く）
			const bool IsBeforeAnchor =
			Attach.Placement.Side == CommentPlacement::Before && !ts_node_is_null(Anchor) && TokenStart < ts_node_start_byte(Anchor);
			LastScan.DescentPath.clear();
			uint16_t Crossed = 0;
			while(Attach.Placement.Side == CommentPlacement::Before && !ts_node_is_null(Anchor) && !ts_node_eq(Anchor, RootAnchor)) {
				if(IsBeforeAnchor) if(
					const std::pmr::unordered_map<const void *, std::pair<TSNode, uint16_t>>::const_iterator Hit = LastScan.Descent.find(Anchor.id);
					Hit != LastScan.Descent.end()
				) {
					// 既知の降下先再利用
					Anchor = Hit->second.first;
					Crossed = static_cast<uint16_t>(std::min<uint32_t>(Crossed + Hit->second.second, std::numeric_limits<uint16_t>::max()));
					break;
				}
				// 透過ラッパーでないアンカーでの停止（丸括弧は外れれば再紐付が中身へ寄せ，残れば前のコメントを括弧の前に保つ為に降ろさない）
				if(!Src.Is(Anchor, Role::TransparentWrapper)) break;
				// 整形中に消えるコメントをアンカーにせず，最初の名前付非コメント子へ降下
				TSNode InnerChild {};
				ForEachNamedChild(
					Anchor,
					[&Src, &InnerChild](const TSNode Child) -> bool {
						// コメントを飛ばして次の子へ進む事の返戻
						if(Src.Is(Child, Role::Comment)) return true;
						InnerChild = Child;
						// 最初のコメント以外の子で打ち切る事の返戻
						return false;
					}
				);
				// 複数子のラッパーはコメントが先頭子より前なら降下し，既存ドキュメントの見落し・重複生成を防止
				if(ts_node_is_null(InnerChild) || ts_node_named_child_count(Anchor) != 1 && TokenStart >= ts_node_start_byte(InnerChild)) break;
				if(IsBeforeAnchor) {
					LastScan.DescentPath.emplace_back(Anchor, Crossed);
					// ラッパーの中の子の前の無名トークンの計数（兄弟を順に辿るカーソルで，数の上限で飽和）
					HeldCursor Cursor(Anchor);
					for(
						bool IsWalking = ts_tree_cursor_goto_first_child(&Cursor);
						IsWalking && ts_node_start_byte(ts_tree_cursor_current_node(&Cursor)) < ts_node_start_byte(InnerChild);
						IsWalking = ts_tree_cursor_goto_next_sibling(&Cursor)
					) if(!ts_node_is_named(ts_tree_cursor_current_node(&Cursor)) && Crossed != std::numeric_limits<uint16_t>::max()) ++Crossed;
				}
				Anchor = InnerChild;
			}
			for(const std::pair<TSNode, uint16_t> &Step : LastScan.DescentPath) {
				LastScan.Descent[Step.first.id] = { Anchor, static_cast<uint16_t>(Crossed - Step.second) };
			}
			Attach.Placement.TokensBefore =
			static_cast<uint16_t>(std::min<uint32_t>(Attach.Placement.TokensBefore + Crossed, std::numeric_limits<uint16_t>::max()));
			// 分岐全体の本体でなく，開始又は終端の語へ境界のコメントを対応付ける
			if(!ts_node_is_null(Anchor) && Src.Is(Anchor, Role::PreprocBranch) && ts_node_child_count(Anchor)) {
				if(
					Attach.Kind == CommentKind::OwnLine && Attach.Placement.Side == CommentPlacement::Before ||
					Attach.Kind == CommentKind::Trailing && ts_node_start_point(Anchor).row == ts_node_start_point(Node).row
				) Anchor = ts_node_child(Anchor, 0);
				else if(
					const TSNode End = ts_node_child(Anchor, ts_node_child_count(Anchor) - 1);
					Attach.Kind == CommentKind::Trailing && Src.Is(End, Role::PreprocDirectiveClose) &&
					ts_node_end_point(End).row == ts_node_start_point(Node).row
				) Anchor = End;
			}
			if(!ts_node_is_null(Anchor)) {
				// 投影したノードへ対応付けるコメントの原文位置とアンカーの記録
				Pending Entry;
				Entry.CommentStart = TokenStart;
				Entry.CommentEnd = TextEnd;
				// 原文が独立コメントである事を，後段のグループ化の条件として記録
				Entry.ShouldNormalizeLeading = IsLeading;
				// 本文を正規化せずグループへも纏めないディレクティブコメントか
				Entry.IsVerbatim = Attach.Role == CommentRole::Directive;
				Entry.Anchor = Anchor;
				// ルート代替経路の場合は末尾コメントとして紐付（先行コメントだとファイル先頭に来て仕舞う）
				if(ts_node_eq(Anchor, RootAnchor) && Attach.Placement.Side == CommentPlacement::Before) {
					Attach.Placement.Side = CommentPlacement::After;
				}
				Entry.Comment = std::move(Attach);
				PendingList.push_back(std::move(Entry));
			}
			if(IsInline) {
				// 行内コメントの除去範囲
				uint32_t DeleteStartInline = TokenStart, DeleteEndInline = TextEnd;
				// 隣接１空白を仮消費し，単語融合は削除適用時に補完（CSS の子孫空白は保つ為，前後の両方が空白の時だけ１つ消費する）
				if(!Rules.IsWhitespaceSemantic) {
					if(DeleteEndInline < Self.size() && Self[DeleteEndInline] == ' ') ++DeleteEndInline;
					else if(DeleteStartInline && Self[DeleteStartInline - 1] == ' ') --DeleteStartInline;
				} else if(
					DeleteEndInline < Self.size() && Self[DeleteEndInline] == ' ' && DeleteStartInline && Self[DeleteStartInline - 1] == ' '
				) ++DeleteEndInline;
				// ブロックコメント内の改行が文を終えず，他の行依存の境界も無い言語では，跡に改行を残さない
				// JavaScript・TypeScript の文中のコメントも，前後のトークンが改行で読みを変えなければ跡に改行を残さない
				// 不要な改行は，次回の部分整形でコメントのアンカーを行頭へ移す原因に為る
				const bool IsJoinable = Reading.CanCommentLineSeparatorsEndStmts ?
				!Src.Is(NodeParent, Role::StmtOrMemberSeq) &&
				!Src.CanChangeReadingAt(TextEdit::SkipSpaceLeft(Self, Reading, TokenStart), TextEdit::SkipSpaceRight(Self, Reading, TextEnd)) :
				!Reading.HasLineSensitiveBoundary;
				DeleteRanges.push_back({ DeleteStartInline, DeleteEndInline, IsJoinable });
				// 行内のコメントの除去範囲を記録した場合の終了
				return;
			}
			// 前置区切（全角スペースを含み，独立行の物は行頭のインデント）と独立行の終端改行を含む範囲の除去
			DeleteRanges.push_back({ TextEdit::SeparatorBefore(Self, Reading, TokenStart).first, DeleteEnd });
			// 終了
			return;
		};
		// 逐語本文のコメント形は保護し，リテラルの補間コードのコメントは抽出
		// マクロのトークン列のコメントも後続のトークンへ結び付け，コメント経路へ分離する
		const auto DoesHoldLiteralComments = [&Src, &Rules](const TSNode Node) -> bool {
			// 内容其の物としてコメントを抽出しないノードかの返戻（Ruby の区切を持つリテラルも内容が意味を持つ）
			return Src.Is(Node, Role::LiteralInterior) && !Src.Is(Node, Role::StrLikeAll) ||
			Src.Is(Node, Role::PreprocNode) && !ParseAdapter::IsNullDirective(Src, Node) &&
			(!Src.Is(Node, Role::PreprocBranch) || !ts_node_named_child_count(Node)) ||
			Rules.IsWhitespaceSemantic && Src.IsCssUrlArgs(Node);
		};
		const TSNode RootNode = Src.GetRoot();
		// 部分木のコメントの分離処理
		const auto WalkSubtree = [&Src, &ProcessComment, &DoesHoldLiteralComments, &ParentsWithComments, RootNode](
			const TSNode SubtreeRoot,
			std::vector<Pending> &PendingOutput,
			std::vector<DeleteRange> &DeleteRanges,
			std::vector<uint32_t> &Separated,
			LineScan &LastScan
		) -> void {
			// 根から現在のノードの親迄の祖先と，同じ世代の未訪問の子の範囲
			std::vector<TSNode> Path { RootNode, SubtreeRoot };
			std::vector<std::span<const ParsedChild>> Children { Src.GetChildrenOf(SubtreeRoot) };
			while(!Children.empty()) {
				std::span<const ParsedChild> &Remaining = Children.back();
				if(Remaining.empty()) {
					Children.pop_back();
					Path.pop_back();
					continue;
				}
				const TSNode Node = Remaining.front().Node;
				Remaining = Remaining.subspan(1);
				++LastScan.Visits;
				// コメント判定と内容判定で共有する型名の単一取得
				const bool IsComment = Src.Is(Node, Role::Comment);
				if(IsComment) ProcessComment(Node, Path, PendingOutput, DeleteRanges, Separated, LastScan);
				// コメント又はコメント様内容を持つノード内部の走査・抽出除外
				if(!IsComment && ParentsWithComments.contains(Node.id) && !DoesHoldLiteralComments(Node)) {
					const std::span<const ParsedChild> ChildRows = Src.GetChildrenOf(Node);
					if(ChildRows.empty()) continue;
					Path.push_back(Node);
					Children.push_back(ChildRows);
				}
			}
			// 終了
			return;
		};
		// 根の直下の子の振分（コメントは直接処理，其の他は部分木走査へ委譲，逐次枝・並列枝で共用）
		const auto DispatchTop = [&](
			const TSNode Child,
			std::vector<Pending> &PendingOutput,
			std::vector<DeleteRange> &DeleteOut,
			std::vector<uint32_t> &SeparatedOut,
			LineScan &LastScan
		) -> void {
			// 最上位子のコメント又は部分木への振分
			++LastScan.Visits;
			if(Src.Is(Child, Role::Comment)) ProcessComment(Child, { &RootNode, 1 }, PendingOutput, DeleteOut, SeparatedOut, LastScan);
			else if(ParentsWithComments.contains(Child.id) && !DoesHoldLiteralComments(Child)) {
				WalkSubtree(Child, PendingOutput, DeleteOut, SeparatedOut, LastScan);
			}
			// 終了
			return;
		};
		// 同親の前後の子を保存し，不在時は祖先側を継承（親の後の最初の非コメント子も保持）
		struct ParentFrame {
			TSNode Node;
			TSNode Prev;
			TSNode Next;
			TSNode OuterPrev;
			TSNode OuterNext;
			TSNode Following; // 親の後に続く最初のコメントでない子（トークンを含み，親の兄弟に無ければ祖先から遡った物）
		};
		// 未走査親のスタック
		std::vector<ParentFrame> Parents { { RootNode, TSNode{}, TSNode{}, TSNode{}, TSNode{}, TSNode{} } };
		std::vector<TSNode> NextOf, FollowOf;
		std::vector<const RoleSet *> ChildRoles;
		std::vector<uint8_t> Anchorless;
		std::vector<uint32_t> Unnamed, Hidden;
		std::vector<size_t> NextAt;
		// 深い構文木での呼出スタック消費を避ける親の明示スタック走査
		while(!Parents.empty() && !ParentsWithComments.empty()) {
			// 未処理の親ノードの深さ優先走査
			const ParentFrame Top = Parents.back();
			Parents.pop_back();
			const std::span<const ParsedChild> ChildRows = Src.GetChildrenOf(Top.Node);
			if(Observation) (*Observation)[0] += ChildRows.size();
			// アンカーに為らない子（ソースへ残さないコメント・空白化する Python の行継続・構造化で省く最上位の空の式文 `;`）のマーカーと，各子の前の名前無のトークンの数
			Anchorless.assign(ChildRows.size(), 0);
			ChildRoles.resize(ChildRows.size());
			Unnamed.assign(ChildRows.size() + 1, 0);
			// 各子の前の間隙に残る隠れたトークンの数の累積（エラーを含む親の読み飛ばしたトークンは数えず，最初の子の前は使わない）
			Hidden.assign(ChildRows.size() + 1, 0);
			const bool ShouldCountHidden = !ts_node_has_error(Top.Node);
			// 名前付子がコメントだけかは兄弟の分類と同時に確定する
			bool IsCommentOnlyExpr = Src.Is(Top.Node, Role::EmbeddedExpr);
			TSNode PrevCode {};
			for(size_t Idx = 0; Idx < ChildRows.size(); ++Idx) {
				const TSNode Child = ChildRows[Idx].Node;
				const RoleSet &Roles = Src.GetProfile().GetKindOf(ChildRows[Idx].Symbol).Roles;
				ChildRoles[Idx] = &Roles;
				const bool IsComment = Roles.test(static_cast<size_t>(Role::Comment)), IsNamed = ts_node_is_named(Child);
				if(IsNamed && !IsComment) IsCommentOnlyExpr = false;
				Anchorless[Idx] = IsComment || Roles.test(static_cast<size_t>(Role::LineContinuation)) ||
				ts_node_eq(Top.Node, RootNode) && Src.IsOmittedSemicolon(Child, PrevCode);
				if(!IsComment && IsNamed) PrevCode = Child;
				Unnamed[Idx + 1] = Unnamed[Idx] + !IsNamed;
				// 前の子との間の空白で区切ったトークンの数
				uint32_t Gap = 0;
				if(ShouldCountHidden && Idx) {
					bool IsInToken = false;
					const uint32_t End = ts_node_start_byte(Child);
					for(uint32_t At = ts_node_end_byte(ChildRows[Idx - 1].Node); At < End;) {
						if(const size_t Width = TextEdit::IntertokenSpaceAt(Src, Reading, At); Width && At + Width <= End) {
							At += static_cast<uint32_t>(Width);
							IsInToken = false;
							continue;
						}
						Gap += !IsInToken;
						IsInToken = true;
						++At;
					}
				}
				Hidden[Idx + 1] = Hidden[Idx] + Gap;
			}
			// 後続子を逆順に記録する兄弟関係インデックスの構築
			NextOf.assign(ChildRows.size(), TSNode{});
			NextAt.assign(ChildRows.size(), ChildRows.size());
			FollowOf.assign(ChildRows.size(), Top.Following);
			for(size_t Idx = ChildRows.size(); Idx > 1; --Idx) {
				const TSNode After = ChildRows[Idx - 1].Node;
				const bool IsNext = !Anchorless[Idx - 1] && Unnamed[Idx] == Unnamed[Idx - 1];
				NextOf[Idx - 2] = IsNext ? After : NextOf[Idx - 1];
				NextAt[Idx - 2] = IsNext ? Idx - 1 : NextAt[Idx - 1];
				FollowOf[Idx - 2] = Anchorless[Idx - 1] ? FollowOf[Idx - 1] : After;
			}
			// 名前無のトークンの数の上限での飽和
			const auto Saturate = [](const uint32_t Count) -> uint16_t {
				// 飽和した数の返戻
				return static_cast<uint16_t>(std::min<uint32_t>(Count, std::numeric_limits<uint16_t>::max()));
			};
			// 直前の名前付非コメント子（と其の位置）と，其の後に開波括弧・トークンが在ったか，前に閉波括弧が在ったか
			TSNode Prev {};
			size_t PrevAt = 0;
			bool IsAfterOpener = false, IsAfterCloser = false, IsAfterToken = false;
			for(size_t Idx = 0; Idx < ChildRows.size(); ++Idx) {
				const TSNode Child = ChildRows[Idx].Node, Next = NextOf[Idx];
				if(Work) Work->Charge(1);
				if(Unnamed[Idx + 1] != Unnamed[Idx]) {
					IsAfterOpener = IsAfterOpener || ChildRoles[Idx]->test(static_cast<size_t>(Role::OpenBrace));
					IsAfterCloser = IsAfterCloser || ChildRoles[Idx]->test(static_cast<size_t>(Role::ClosingBraceToken));
					IsAfterToken = true;
					continue;
				}
				if(ChildRoles[Idx]->test(static_cast<size_t>(Role::Comment))) {
					Adjacent[Child.id] = {
						Prev,
						Next,
						Top.OuterPrev,
						Top.OuterNext,
						Top.Prev,
						Top.Next,
						FollowOf[Idx],
						IsAfterOpener,
						IsAfterCloser,
						IsAfterToken,
						Saturate(Unnamed[Idx]),
						Saturate(Unnamed[Idx] - Unnamed[PrevAt]),
						Saturate(Unnamed[NextAt[Idx]] - Unnamed[Idx]),
						Saturate(Hidden[std::min(NextAt[Idx] + 1, ChildRows.size())] - Hidden[Idx + 1]),
						IsCommentOnlyExpr
					};
				}
				if(ParentsWithComments.contains(Child.id) && ts_node_named_child_count(Child)) {
					Parents.push_back(
						{ Child, Prev, Next, ts_node_is_null(Prev) ? Top.OuterPrev : Prev, ts_node_is_null(Next) ? Top.OuterNext : Next, FollowOf[Idx] }
					);
				}
				if(Anchorless[Idx]) continue;
				Prev = Child;
				PrevAt = Idx;
				IsAfterOpener = IsAfterCloser = IsAfterToken = false;
			}
		}
		// 根の子は同じ世代の親子表の範囲を共有し，カーソルで集め直さない
		const std::span<const ParsedChild> RootChildren = Src.GetChildrenOf(RootNode);
		const size_t RootChildCount = RootChildren.size();
		// 原文順に統合する紐付候補
		std::vector<Pending> PendingList;
		std::vector<DeleteRange> DeleteRanges;
		std::vector<uint32_t> &Separated = Result.SeparatedStarts;
		// 分離のスレッド数の上限
		constexpr size_t MaxThreads = 8; // コメントの分離で使うスレッド数の上限
		const bool IsRootComment = Src.Is(RootNode, Role::Comment);
		// 並列化の固定費を避ける小さな木とコメント単体の逐次処理
		if(const size_t ThreadCount = IsRootComment ? 1 : Parallel::DecideThreads(RootChildCount, MaxThreads); ThreadCount < 2) {
			// 逐次走査の再利用状態
			LineScan LastScan;
			// 根自体がコメントのノードである場合も処理対象に含める
			if(IsRootComment) {
				++LastScan.Visits;
				ProcessComment(RootNode, {}, PendingList, DeleteRanges, Separated, LastScan);
			}
			// 共通走査で得たコメント数を収集列の初期容量に使う
			PendingList.reserve(Facts.Nodes.size());
			DeleteRanges.reserve(Facts.Nodes.size());
			Separated.reserve(Facts.Nodes.size());
			for(const ParsedChild &Child : RootChildren) DispatchTop(Child.Node, PendingList, DeleteRanges, Separated, LastScan);
			if(Work) Work->Charge(LastScan.Visits);
			if(Observation) (*Observation)[1] += LastScan.ChildReads;
		} else {
			std::vector<std::vector<Pending>> ThreadPending(ThreadCount);
			std::vector<std::vector<DeleteRange>> ThreadDeletes(ThreadCount);
			std::vector<std::vector<uint32_t>> ThreadSeparated(ThreadCount);
			// スレッド別の再利用状態
			std::vector<LineScan> ThreadScans(ThreadCount);
			// 遅延して作るキャッシュを並列処理前に構築（`mutable` のキャッシュは同期を持たない）
			Src.GetRoot();
			Src.RecordNodeRelations();
			{
				// マクロの実引数の保持判定を使う文法だけ，名前の表を並列読取前に確定
				std::optional<ParsedTree::NameReadGuard> Guard;
				if(Reading.HasPreprocMacros) Guard.emplace(Src);
				// コメント収集中の同期を不要にするスレッド別書込先の分離
				Parallel::ForChunks(
					RootChildCount,
					ThreadCount,
					[&](const size_t ChunkStart, const size_t ChunkEnd, const size_t ThreadIdx) -> void {
						// 担当区間の実行
						for(size_t Idx = ChunkStart; Idx < ChunkEnd; ++Idx) {
							DispatchTop(
								RootChildren[Idx].Node,
								ThreadPending[ThreadIdx],
								ThreadDeletes[ThreadIdx],
								ThreadSeparated[ThreadIdx],
								ThreadScans[ThreadIdx]
							);
						}
						// 終了
						return;
					}
				);
			}
			size_t TotalPending = 0, TotalDeletes = 0, TotalSeparated = 0;
			for(size_t Idx = 0; Idx < ThreadCount; ++Idx) {
				if(Work) Work->Charge(ThreadScans[Idx].Visits);
				if(Observation) (*Observation)[1] += ThreadScans[Idx].ChildReads;
				if(
					ThreadPending[Idx].size() > PendingList.max_size() - TotalPending ||
					ThreadDeletes[Idx].size() > DeleteRanges.max_size() - TotalDeletes ||
					ThreadSeparated[Idx].size() > Separated.max_size() - TotalSeparated
				) throw std::length_error("separated comment lists exceed their capacity");
				TotalPending += ThreadPending[Idx].size();
				TotalDeletes += ThreadDeletes[Idx].size();
				TotalSeparated += ThreadSeparated[Idx].size();
			}
			PendingList.reserve(TotalPending);
			// 削除範囲の統合領域予約
			DeleteRanges.reserve(TotalDeletes);
			Separated.reserve(TotalSeparated);
			for(size_t Idx = 0; Idx < ThreadCount; ++Idx) {
				std::vector<Pending> &Local = ThreadPending[Idx];
				PendingList.insert(PendingList.end(), std::make_move_iterator(Local.begin()), std::make_move_iterator(Local.end()));
				DeleteRanges.insert(DeleteRanges.end(), ThreadDeletes[Idx].begin(), ThreadDeletes[Idx].end());
				Separated.insert(Separated.end(), ThreadSeparated[Idx].begin(), ThreadSeparated[Idx].end());
			}
		}
		// 解析用の入力から退避したコメントの記録（整形仕様 4.2, 7.6，所属だけを決め，置けるかと配置はコメント経路が決める）
		// 退避位置を含む保持領域の中，行末では位置で終わる構文の後，其の他では位置で始まる構文の前へのコメントの紐付
		for(const EscapedComment &Escaped : Src.EscapedComments) {
			Pending Entry;
			Entry.CommentStart = Entry.CommentEnd = Escaped.Pos;
			Entry.ShouldNormalizeLeading = Escaped.Kind == CommentKind::OwnLine;
			Entry.Comment.Text = Escaped.Text;
			Entry.Comment.Kind = Escaped.Kind;
			Entry.Comment.Notation = CommentRepository::NotationOf(Rules.Notation, Escaped.Text, false);
			CommentRepository::AttachRules(Rules.Notation, Entry.Comment);
			Entry.Comment.Placement.Side = Escaped.Kind == CommentKind::Trailing ? CommentPlacement::After : CommentPlacement::Before;
			const bool IsAfter = Escaped.Kind == CommentKind::Trailing && Escaped.Pos;
			const uint32_t Probe = IsAfter ? Escaped.Pos - 1 : Escaped.Pos;
			TSNode Anchor = ts_node_named_descendant_for_byte_range(RootNode, Probe, Probe);
			// 独立コメントを退避した間隙では，包含する構文の中の空白の後のトークンを説明対象とする
			// 範囲の演算子直後の空白を，左辺から始まる範囲全体の先行と読まない（整形仕様 7.1, 7.2, 7.6）
			if(Escaped.Kind == CommentKind::OwnLine && !ts_node_is_null(Anchor)) {
				uint32_t Following = Probe;
				const uint32_t End = ts_node_end_byte(Anchor);
				while(Following < End) {
					const size_t Width = TextEdit::IntertokenSpaceAt(Src, Reading, Following);
					if(!Width) break;
					Following += static_cast<uint32_t>(Width);
				}
				if(Probe < Following && Following < End) Anchor = ts_node_named_descendant_for_byte_range(Anchor, Following, Following);
			}
			// 位置で始まる（行末の物は位置で終わる）最も外の構文
			for(TSNode Up = ts_node_parent(Anchor); !ts_node_is_null(Up) && !ts_node_eq(Up, RootNode); Up = ts_node_parent(Up)) {
				if((IsAfter ? ts_node_end_byte(Up) : ts_node_start_byte(Up)) != (IsAfter ? Escaped.Pos : ts_node_start_byte(Anchor))) break;
				Anchor = Up;
			}
			// テキストを保つ領域の中の物は最も外の領域の中，前処理の見出の独立コメントは見出の前
			for(TSNode Up = Anchor; !ts_node_is_null(Up); Up = ts_node_parent(Up)) {
				// 解析の為にマーカーへ置き換えた連結指定はコードの為，其の前後
				if(ParseAdapter::IsLinkageGuard(Src, Up)) {
					Anchor = Up;
					Entry.Comment.Placement.Side = Escaped.Kind == CommentKind::Trailing ? CommentPlacement::After : CommentPlacement::Before;
					break;
				}
				if(DoesHoldLiteralComments(Up)) {
					Anchor = Up;
					Entry.Comment.Placement.Side =
					Src.Is(Up, Role::PreprocNode) && Escaped.Kind == CommentKind::OwnLine ? CommentPlacement::Before : CommentPlacement::Inside;
					continue;
				}
				// 領域の外へ出た所での打切
				if(Entry.Comment.Placement.Side == CommentPlacement::Inside) break;
				if(!Src.Is(Up, Role::PreprocBranch)) continue;
				// 条件のブロックを成すディレクティブの見出の行の中の物（見出の外なら打切）
				TSNode Header = Src.GetFieldChild(Up, ChildRole::Cond);
				if(ts_node_is_null(Header)) Header = Src.GetFieldChild(Up, ChildRole::Name);
				if(Escaped.Kind == CommentKind::OwnLine && !ts_node_is_null(Header) && Escaped.Pos <= Src.GetEnd(Header)) {
					Anchor = ts_node_child(Up, 0);
					Entry.Comment.Placement.Side = CommentPlacement::Before;
				}
				break;
			}
			Entry.Anchor = ts_node_is_null(Anchor) ? RootNode : Anchor;
			PendingList.push_back(std::move(Entry));
		}
		// 投影が二分探索で引く分離したコメントの始まりの整列
		std::sort(Separated.begin(), Separated.end());
		// 並列収集の完了順に依らない原文コメント順への復元
		std::stable_sort(
			PendingList.begin(),
			PendingList.end(),
			[](const Pending &Left, const Pending &Right) -> bool {
				// コメントの開始位置順の比較結果の返戻
				return Left.CommentStart < Right.CommentStart;
			}
		);
		// 原文の形のテキストの記号と区切空白を除いた本文への分解（記号は記法が表す，整形仕様 3.2）
		bool HasEmbeddedDoc = false, HasBlockLineDoc = false;
		CommentGroupId InputGroup = 0;
		for(size_t Idx = 0; Idx < PendingList.size(); ++Idx) {
			if(Work) Work->Charge(1);
			Pending &Entry = PendingList[Idx];
			CommentRepository::Decompose(Entry.Comment);
			if(HasDirectiveRestorations && Entry.Comment.Kind == CommentKind::Inline && !Entry.IsVerbatim) {
				// 本文の復元で戻した改行と，解析用の構文に残る改行を区別する
				Entry.Comment.Format.ShouldSeparateLogicalLines =
				std::string_view(Src.data() + Entry.CommentStart, Entry.CommentEnd - Entry.CommentStart).find('\n') != std::string_view::npos;
			}
			HasEmbeddedDoc = HasEmbeddedDoc || Entry.Comment.Notation == CommentNotation::RubyEmbeddedDocument;
			HasBlockLineDoc = HasBlockLineDoc || CommentRepository::IsBlockLineDoc(Entry.Comment);
			// 原文のドキュメント境界は，空行を越えて纏める本文正規化の所属と区別する
			if(Idx) {
				const Pending &Prev = PendingList[Idx - 1];
				if(
					Prev.Comment.Notation != Entry.Comment.Notation || CommentRepository::IsBlockNotation(Prev.Comment.Notation) ||
					Prev.Comment.Notation == CommentNotation::RubyEmbeddedDocument || !ts_node_eq(Prev.Anchor, Entry.Anchor) ||
					Prev.Comment.Placement.Side != Entry.Comment.Placement.Side || Entry.CommentStart < Prev.CommentEnd ||
					!CommentNormalizer::IsCommentGap(std::string_view(Src.data() + Prev.CommentEnd, Entry.CommentStart - Prev.CommentEnd), false)
				) ++InputGroup;
			}
			Entry.Comment.Format.InputGroup = InputGroup;
		}
		// 行毎の記号を持たない埋込ドキュメントの，出力する行コメントと同じ論理行毎の記録への分割（整形仕様 7.3, 7.6）
		// 開始記号・終端記号の行の残りも記録とし，囲みの開始記号・終端記号だけの行と同じく，空の行は構造として保つ時だけ配置の段が残す
		if(HasEmbeddedDoc && Rules.Notation.IsAvailable(CommentNotation::RubyEmbeddedDocument)) {
			// 論理行の本文と原文の物理行の範囲
			struct LineSpan {
				std::string_view Text; // 論理行の本文
				uint32_t Start; // 物理行の始まり
				uint32_t End; // 物理行の終わり
			};
			std::vector<Pending> Split;
			Split.reserve(PendingList.size());
			// 後の記録が同じアンカーで空白だけを挟んで続く埋込ドキュメントか（境の開始記号・終端記号の行の空の記録は，続く埋込ドキュメントの論理行でない為に作らない）
			std::vector<uint8_t> JoinsNext(PendingList.size(), 0);
			for(size_t Idx = 0; Idx + 1 < PendingList.size(); ++Idx) {
				if(Work) Work->Charge(1);
				const Pending &Left = PendingList[Idx], &Right = PendingList[Idx + 1];
				JoinsNext[Idx] = Left.Comment.Notation == CommentNotation::RubyEmbeddedDocument &&
				Right.Comment.Notation == CommentNotation::RubyEmbeddedDocument && ts_node_eq(Left.Anchor, Right.Anchor) &&
				CommentNormalizer::IsCommentGap(std::string_view(Src.data() + Left.CommentEnd, Right.CommentStart - Left.CommentEnd), true);
			}
			for(size_t Idx = 0; Idx < PendingList.size(); ++Idx) {
				if(Work) Work->Charge(1);
				Pending &Entry = PendingList[Idx];
				if(Entry.Comment.Notation != CommentNotation::RubyEmbeddedDocument) {
					Split.push_back(std::move(Entry));
					continue;
				}
				// 本文に在る終端記号は１つの囲みで保ち，行毎の記法の選択とは区別する
				const std::string_view Closer = CommentRepository::GetCloserOf(CommentNotation::RubyEmbeddedDocument);
				bool HasCloser = false;
				for(size_t Pos = 0; Pos < Entry.Comment.Text.size();) {
					if(CommentRepository::IsLineHeadWord(std::string_view(Entry.Comment.Text).substr(Pos), Closer)) {
						HasCloser = true;
						break;
					}
					Pos = std::min(Entry.Comment.Text.find('\n', Pos), Entry.Comment.Text.size()) + 1;
				}
				if(HasCloser) {
					// 終端記号を含む本文を１つの記録に保ち，枠だけの空の縁を除く
					std::string &Text = Entry.Comment.Text;
					if(Text.starts_with('\n')) Text.erase(0, 1);
					if(Text.ends_with('\n')) Text.pop_back();
					Split.push_back(std::move(Entry));
					continue;
				}
				const std::string Body = std::move(Entry.Comment.Text);
				Entry.Comment.Format.ShouldSeparateLogicalLines = false;
				// 分解が物理行毎に作った論理行
				std::vector<LineSpan> Lines;
				for(size_t Cursor = 0, LineStart = Entry.CommentStart; Cursor <= Body.size();) {
					if(Work) Work->Charge(1);
					const size_t End = std::min(Body.find('\n', Cursor), Body.size());
					const size_t LineEnd = std::min(Src.find('\n', LineStart), static_cast<size_t>(Entry.CommentEnd));
					Lines.push_back(
						{ std::string_view(Body).substr(Cursor, End - Cursor), static_cast<uint32_t>(LineStart), static_cast<uint32_t>(LineEnd) }
					);
					Cursor = End + 1;
					LineStart = LineEnd + 1;
				}
				// 続く埋込ドキュメントとの境の空の開始記号・終端記号の行を除く範囲
				size_t First = 0, Last = Lines.size();
				if(Idx && JoinsNext[Idx - 1] && Lines[First].Text.empty()) ++First;
				if(JoinsNext[Idx] && Last > First && Lines[Last - 1].Text.empty()) --Last;
				size_t ContentFirst = First, ContentLast = Last;
				while(ContentFirst < ContentLast && Lines[ContentFirst].Text.empty()) ++ContentFirst;
				while(ContentLast > ContentFirst && Lines[ContentLast - 1].Text.empty()) --ContentLast;
				for(size_t Idx = First; Idx < Last; ++Idx) {
					Pending &Piece = Split.emplace_back(Entry);
					Piece.Comment.Text = Lines[Idx].Text;
					Piece.Comment.IsEmbeddedEdge = Idx < ContentFirst || Idx >= ContentLast;
					Piece.CommentStart = Lines[Idx].Start;
					Piece.CommentEnd = Lines[Idx].End;
				}
			}
			PendingList = std::move(Split);
		}
		// 同じアンカーの同じ側に，空白だけを挟んで続く独立コメントのグループを記録（整形仕様 3.3, 3.4, 7.3）
		// ドキュメントの位置が確定した後，記法に従ってグループを分けて本文を正規化する
		CommentGroupId NextGroup = 0;
		for(size_t Begin = 0; Begin < PendingList.size();) {
			// 本文を正規化しないディレクティブコメントの読飛（整形仕様 7.5）
			if(PendingList[Begin].IsVerbatim) {
				++Begin;
				continue;
			}
			size_t End = Begin + 1;
			while(End < PendingList.size()) {
				const Pending &Prev = PendingList[End - 1], &Next = PendingList[End];
				// 同じアンカーの同じ側のコメントは出力で空行を除いて並ぶ為に空行を越えて纏め，別のアンカー・側のコメントは出力の配置で空行を挟み得る為に纏めない
				// グループを纏める記法はドキュメントの位置で変わる為，此処では並びだけを記録し，ドキュメントの位置が確定した後に分ける（整形仕様 3.3）
				if(
					!Prev.ShouldNormalizeLeading || !Next.ShouldNormalizeLeading || Next.IsVerbatim || Next.CommentStart < Prev.CommentEnd ||
					!ts_node_eq(Prev.Anchor, Next.Anchor) || Prev.Comment.Placement.Side != Next.Comment.Placement.Side ||
					!CommentNormalizer::IsCommentGap(std::string_view(Src.data() + Prev.CommentEnd, Next.CommentStart - Prev.CommentEnd), true)
				) break;
				++End;
			}
			for(size_t Idx = Begin; Idx < End; ++Idx) PendingList[Idx].Comment.Format.Group = NextGroup;
			++NextGroup;
			Begin = End;
		}
		// 同じアンカーの連続する行形式のドキュメントを，１つの記録へ統合（整形仕様 7.6）
		// 記法の定義が同じドキュメント形式として採用する行と囲みを共通経路へ通す
		// 入力の記法は保ち，出力の記法は配置の段で選ぶ
		if(HasBlockLineDoc) {
			std::vector<Pending> Merged;
			Merged.reserve(PendingList.size());
			for(size_t Idx = 0; Idx < PendingList.size();) {
				if(!CommentRepository::IsBlockLineDoc(PendingList[Idx].Comment)) {
					Merged.push_back(std::move(PendingList[Idx++]));
					if(Observation) ++(*Observation)[2];
					continue;
				}
				// 同じアンカーの同じ側で改行１つだけを挟んで続く同じ記法のドキュメントの行
				const Pending &First = PendingList[Idx];
				size_t End = Idx + 1;
				while(End < PendingList.size()) {
					const Pending &Next = PendingList[End];
					if(!CommentRepository::IsBlockLineDoc(Next.Comment) || Next.Comment.Format.InputGroup != First.Comment.Format.InputGroup) break;
					++End;
				}
				Pending Doc = std::move(PendingList[Idx]);
				for(size_t Next = Idx + 1; Next < End; ++Next) Doc.Comment.Text.append("\n").append(PendingList[Next].Comment.Text);
				Doc.Comment.Format.ShouldSeparateLogicalLines = Doc.Comment.Text.find('\n') != std::string::npos;
				Doc.CommentEnd = PendingList[End - 1].CommentEnd;
				Merged.push_back(std::move(Doc));
				if(Observation) ++(*Observation)[2];
				Idx = End;
			}
			PendingList = std::move(Merged);
		}
		// 統合後の原文順に依る出力通し番号の確定
		for(size_t Idx = 0; Idx < PendingList.size(); ++Idx) {
			PendingList[Idx].Comment.OriginalOrder = Idx;
			Result.RecordRanges.emplace_back(
				Src.OffsetInInput(PendingList[Idx].CommentStart),
				Src.OffsetInInput(PendingList[Idx].CommentEnd)
			);
		}
		// 除く範囲の昇順整列（投影が二分探索で引く）
		std::sort(
			DeleteRanges.begin(),
			DeleteRanges.end(),
			[](const DeleteRange &Left, const DeleteRange &Right) -> bool {
				// 削除範囲の開始位置順の比較結果の返戻
				return Left.Begin < Right.Begin;
			}
		);
		// 重複・隣接する範囲の結合（投影は重ならない範囲を前提に引く）
		if(!DeleteRanges.empty()) {
			// 圧縮済範囲の末尾位置
			size_t Write = 0;
			// 整列済範囲の其の場圧縮
			for(size_t Read = 1; Read < DeleteRanges.size(); ++Read) {
				if(DeleteRanges[Read].Begin <= DeleteRanges[Write].End) {
					if(DeleteRanges[Read].End > DeleteRanges[Write].End) DeleteRanges[Write].End = DeleteRanges[Read].End;
					DeleteRanges[Write].IsJoinable = DeleteRanges[Write].IsJoinable && DeleteRanges[Read].IsJoinable;
					DeleteRanges[Write].IsSpaceLike = DeleteRanges[Write].IsSpaceLike || DeleteRanges[Read].IsSpaceLike;
				} else DeleteRanges[++Write] = DeleteRanges[Read];
			}
			DeleteRanges.resize(Write + 1);
		}
		// 除いた跡へ補う区切（トークンの融合境界と文の境界を保つ物）の確定と，投影が原文を読む時に除く範囲の記録
		Result.Separations.reserve(DeleteRanges.size());
		for(const DeleteRange &Range : DeleteRanges) {
			// 除く範囲の隣接字
			const unsigned char Before = Range.Begin ? Src.data()[Range.Begin - 1] : '\0';
			const unsigned char After = Range.End < Src.size() ? Src.data()[Range.End] : '\0';
			// コメント除去で演算子・単位を融合させず（括弧・区切に接する所は融合しない為に補わない），CSS の加減算と値の境界を保持
			const bool IsTokenBoundary = IsIdentifierChar(Before) && IsIdentifierChar(After) ||
			Before && After && !std::isspace(Before) && !std::isspace(After) && !IsOpenBracketChar(Before) && !IsCloseBracketChar(Before) &&
			!IsListSeparatorChar(Before) && !IsOpenBracketChar(After) && !IsCloseBracketChar(After) && !IsListSeparatorChar(After);
			const std::string_view Removed(Src.data() + Range.Begin, Range.End - Range.Begin);
			// 改行を含むコメントの自動セミコロン挿入の境界の保持
			const bool IsLineBoundary = !Rules.IsWhitespaceSemantic && !Range.IsJoinable && (
				Removed.find('\n') != std::string_view::npos || Reading.CanCommentLineSeparatorsEndStmts && std::ranges::any_of(
					Reading.CommentLineBreaks,
					[&Removed](const std::string_view Break) -> bool {
						// 行の区切のテキストを含むかの返戻
						return Removed.find(Break) != std::string_view::npos;
					}
				)
			);
			// CSS / SCSS のコメントは空白に為らず，空白では同じトークン列を表せない為，融合する境界だけへ固定の区切記法を置く（整形仕様 6.1, 6.6）
			// Sass が空白と読むセレクタの中のコメントは，前後が密着して居れば空白１つで表す
			const bool IsGlued = Before && After && !std::isspace(Before) && !std::isspace(After);
			const bool IsNotation = Rules.IsWhitespaceSemantic && !Range.IsSpaceLike && IsGlued &&
			SyntaxRules::CanFuseDimensionTokens(Before, std::string_view(Src).substr(Range.End));
			const std::string_view Separator =
			IsNotation ? Rules.LexicalSeparator : Rules.IsWhitespaceSemantic && Range.IsSpaceLike && IsGlued ?
			" " :
			Rules.IsWhitespaceSemantic ? "" : IsLineBoundary ? "\n" : IsTokenBoundary ? " " : "";
			Result.Separations.push_back({ Range.Begin, Range.End, Separator, IsNotation });
		}
		// アンカーのノードへの紐付
		for(Pending &Entry : PendingList) {
			// 原文の行末の物の前置区切に含む水平タブの数と，検証の投影だけで照合する区切の形（整形仕様 7.1, 8.1）
			if(Entry.Comment.Kind == CommentKind::Trailing && Entry.CommentStart < Entry.CommentEnd) if(
				const std::pair<uint32_t, size_t> Separator = TextEdit::SeparatorBefore(Src, Src.GetReading(), Entry.CommentStart);
				Separator.first && Src[Separator.first - 1] != '\n'
			) {
				Entry.Comment.Format.LeadingTabCount = Separator.second;
				if(IsVerification) {
					const std::string Marker = CommentNormalizer::GetStackMarkerOf(Entry.Comment.Notation);
					std::string_view Rest = Entry.Comment.Text;
					// 直前の区切を含む，最初の記号と積んだ記号の原文順での記録
					Result.LeadingSeparators.push_back(
						{
							std::string(CommentNormalizer::GetFirstLogicalLine(Rest, Marker)),
							Entry.CommentStart - Separator.first == Separator.second && Separator.second ||
							Entry.CommentStart - Separator.first == 1 && Src[Separator.first] == ' ' ? Separator.second : SIZE_MAX
						}
					);
					if(!Marker.empty()) {
						size_t Next = Rest.find(Marker);
						while(Next != std::string_view::npos) {
							if(!Next || Rest[Next - 1] != ' ' && Rest[Next - 1] != '\t') {
								Next = Rest.find(Marker, Next + 1);
								continue;
							}
							size_t Begin = Next;
							while(Begin && (Rest[Begin - 1] == ' ' || Rest[Begin - 1] == '\t')) --Begin;
							const std::string_view Gap = Rest.substr(Begin, Next - Begin);
							const size_t Count = std::ranges::count(Gap, '\t');
							Rest.remove_prefix(Next + Marker.size());
							Result.LeadingSeparators.push_back(
								{
									std::string(CommentNormalizer::GetFirstLogicalLine(Rest, Marker)),
									Count && Count == Gap.size() || Gap == " " ? Count : SIZE_MAX
								}
							);
							Next = Rest.find(Marker);
						}
					}
				}
			}
			Result.Attach(Src, Entry.Anchor, Entry.Comment.Placement.Side == CommentPlacement::Before, std::move(Entry.Comment));
		}
		// 分離の結果の返戻
		return Result;
	}

	/**
	 * ドキュメントの説明対象と本文の取得関数（整形仕様 7.4, 8.1）
	 * 同期・本文正規化済の期待コメントと最終テキストの双方から同じ規則で集め，整形仕様 7.4 で認識したドキュメントの説明対象と本文が保たれたかを検証側が比べる
	 * 宣言子の中にアンカーが在るドキュメントは其の宣言子の名前で表し，宣言の分割で説明対象を取り違えない
	 * 整形仕様 7.4 の認識と同じ構文モデルで表す（ディレクティブの適用先は投影出来ない候補からも集める為に解析木で表す，`CollectDirectives`）
	 * 計算量：記録の数 C，本文の長さの和 N，識別の組立 (`LabelTargets`) の費用 Q に対し O(C + N + Q)
	 * @param Syntax 記録のアンカーを持つ構文
	 * @param Records アンカーを持つ記録の並び
	 * @param Targets 記録毎の説明対象（ドキュメントでない記録は `NoNode`）
	 * @param Work 名前取得・祖先の初回訪問・表示の照会・論理行の観測値（不要なら nullptr）
	 * @return 記録順の説明対象・論理行・専用ドキュメントかの判定
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws FormatLimitExceeded 構文の照会・走査が作業予算を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 */
	std::vector<ValidationContext::DocFact> SourceProjector::DescribeDocs(
		const SyntaxModel &Syntax,
		const std::span<const CommentRecord> Records,
		const std::span<const NodeId> Targets,
		std::array<size_t, 4> *const Work
	) {
		// ドキュメント毎の説明対象と其の名前を表すノード（宣言子の中にアンカーが在るドキュメントは其の宣言子）
		std::vector<std::pair<NodeId, NodeId>> Entries;
		std::vector<size_t> LabelIndices;
		size_t BodySize = 0;
		for(size_t Idx = 0; Idx < Targets.size(); ++Idx) {
			const NodeId Target = Targets[Idx];
			if(Target == NoNode) continue;
			BodySize += CommentRepository::GetOutputTextOf(Records[Idx]).size();
			// 同じアンカーの連続する行形式の説明は所属の探索から共有
			if(Idx && Targets[Idx - 1] == Target && Records[Idx - 1].Anchor == Records[Idx].Anchor) {
				LabelIndices.push_back(Entries.size() - 1);
				continue;
			}
			NodeId Part = Records[Idx].Anchor;
			while(Part != NoNode) {
				const NodeId Parent = Syntax.GetParentOf(Part);
				if(Parent == Target) break;
				Part = Parent;
			}
			const std::pair<NodeId, NodeId> Entry { Target, Part != NoNode && IsDeclaratorPart(Syntax, Part) ? Part : Target };
			// 同じ識別を持つ続きは，ラベルの抽出・兄弟の計数に渡す前に纏める
			if(Entries.empty() || Entries.back() != Entry) Entries.push_back(Entry);
			LabelIndices.push_back(Entries.size() - 1);
		}
		std::vector<ValidationContext::DocFact> Descriptions;
		// ドキュメントの無い並びの返戻
		if(Entries.empty()) return Descriptions;
		const std::shared_ptr<ValidationContext::DocStorage> Storage = std::make_shared<ValidationContext::DocStorage>();
		const std::vector<std::pair<size_t, size_t>> Labels = LabelTargets(Syntax, Entries, *Storage, Work);
		struct Extent {
			size_t Begin; // 本文の開始
			size_t Size; // 本文の長さ
			size_t LinesBegin; // 行の範囲の開始
			size_t LinesSize; // 行の範囲の数
			bool IsDedicated; // 通常コメントと区別する記法か
			bool IsInner; // 囲む本体の説明か
		};
		std::vector<Extent> Extents;
		Extents.reserve(LabelIndices.size());
		Storage->Bodies.reserve(BodySize);
		for(size_t Idx = 0; Idx < Targets.size(); ++Idx) if(Targets[Idx] != NoNode) {
			const size_t Begin = Storage->Bodies.size(), LinesBegin = Storage->Lines.size();
			Storage->Bodies.append(CommentRepository::GetOutputTextOf(Records[Idx]));
			DocCommentReader::AppendLogicalLineRanges(Records[Idx], Storage->Lines);
			if(Work) (*Work)[3] += Storage->Lines.size() - LinesBegin;
			Extents.push_back(
				{
					Begin,
					Storage->Bodies.size() - Begin,
					LinesBegin,
					Storage->Lines.size() - LinesBegin,
					DocCommentReader::IsDedicatedDoc(CommentRepository::GetRulesOf(Records[Idx]), Records[Idx]),
					CommentRepository::GetRulesOf(Records[Idx]).IsInnerDoc(CommentRepository::GetOutputNotationOf(Records[Idx]))
				}
			);
		}
		Descriptions.reserve(LabelIndices.size());
		// 全てのバッファの伸長を終えてから，変更不能な共有の所有者とビューを公開する
		for(size_t Idx = 0; Idx < LabelIndices.size(); ++Idx) {
			const Extent &Part = Extents[Idx];
			Descriptions.push_back(
				{
					Storage,
					std::string_view(Storage->Targets).substr(Labels[LabelIndices[Idx]].first, Labels[LabelIndices[Idx]].second),
					std::string_view(Storage->Bodies).substr(Part.Begin, Part.Size),
					std::span<const std::pair<size_t, size_t>>(Storage->Lines).subspan(Part.LinesBegin, Part.LinesSize),
					Part.IsDedicated,
					Storage->TargetScopes[LabelIndices[Idx]],
					Part.IsInner
				}
			);
		}
		// 記録順の説明対象・論理行・専用ドキュメントかの判定の返戻
		return Descriptions;
	}
} // namespace Core
