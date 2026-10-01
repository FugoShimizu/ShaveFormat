#include "SourceProjector.hpp"
#include "../Profile/GrammarProfile.hpp"
#include "../Profile/ReadingRules.hpp"
#include "../Util/CharClass.hpp"
#include "../Util/CommentNormalizer.hpp"
#include "../Util/Parallel.hpp"
#include "../Util/ParseAdapter.hpp"
#include "../Util/ParsedTree.hpp"
#include "../Util/TextEdit.hpp"
#include "CommentRepository.hpp"
#include "DocCommentSynchronizer.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace Core {
	/** ========== 構文とコメントの分離 ========== */
	/**
	 * 本文の各行の共通の字下げを除いた形の取得関数
	 * 指示コメントの照合で，置く行の深さへ字下げし直した継続行と，開始記号の後の定義上同値な区切空白を揃え直した行を同じ本文と看做す為に用いる（行末の空白は出力の終端処理が除く）
	 * @param Raw コメント全文
	 * @param Opener 記法の開始記号（記号の後の空白を値として読む指示子は空）
	 * @param Definition 指示子の登録定義（処理系が区切りと読む字を決める）
	 * @return 二行目以降の共通の字下げと各行の行末の空白を除き，開始記号の後の区切空白（区切の無い物を含む）を半角スペース１つへ揃えた本文
	 */
	std::string SourceProjector::DedentedBody(
		const std::string_view Raw,
		const std::string_view Opener,
		const DirectiveDefinitionId Definition
	) {
		// 二行目以降の空でない行の共通の字下げ
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
		// 行毎に字下げと行末の空白を除いた本文
		std::string Body;
		Body.reserve(Raw.size());
		for(size_t Cursor = 0;;) {
			const size_t End = std::min(Raw.find('\n', Cursor), Raw.size());
			std::string_view Line = Raw.substr(Cursor, End - Cursor);
			if(Cursor && Line.starts_with(Common)) Line.remove_prefix(Common.size());
			Line.remove_suffix(Line.size() - std::min(Line.find_last_not_of(" \t") + 1, Line.size()));
			if(!Cursor && !Opener.empty() && Line.starts_with(Opener)) {
				// 開始記号の後の区切空白（本文が在れば区切の有無に依らず半角スペース１つ，整形仕様 7.6）
				const std::string_view Rest = CommentRepository::TrimDirectiveSeparator(Definition, Line.substr(Opener.size()));
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
	 * go test は名前が `Example` で始まる関数の本体の最後のコメントの群が `Output:` / `Unordered output:`（大小を問わない）で始まる時，其の群の本文を関数の標準出力と比べる (`go help testfunc`)
	 * 群は空行とコードを挟まずに続くコメントの並びとし，本文の空白は比べる値の一部である
	 * 計算量：最上位の宣言の数と Example 関数の本体の節点数の和 V に対し O(V)
	 * @param Src 解析済の字面
	 * @return 出力の注記の群の始まりから本体の終わり迄の範囲（位置順）
	 */
	std::vector<std::pair<uint32_t, uint32_t>> SourceProjector::ExampleOutputRanges(const ::ParsedTree &Src) {
		// 出力注記の範囲と読取規則の準備
		std::vector<std::pair<uint32_t, uint32_t>> Ranges;
		const std::string_view Text = Src;
		const ReadingRules &Reading = Src.GetReading();
		ForEachNamedChild(
			Src.GetRoot(),
			[&](const TSNode Declaration) -> void {
				// 関数宣言でない子の場合の終了
				if(!Src.Is(Declaration, Role::FunctionDeclarationNode)) return;
				const TSNode Name = Src.FieldChild(Declaration, ChildRole::Name);
				const TSNode Body = Src.FieldChild(Declaration, ChildRole::Body);
				// Example 関数でない関数を読飛ばす場合の終了
				if(ts_node_is_null(Name) || ts_node_is_null(Body) || !Src.View(Name).starts_with(Reading.OutputFunctionPrefix)) return;
				// 本体の中のコメントの範囲（位置順）
				std::vector<std::pair<uint32_t, uint32_t>> Comments;
				WalkChildrenCursor(
					Body,
					[&Src, &Comments](const TSNode Node) -> bool {
						// コメントでない節点へ降りる事の返戻
						if(!Src.Is(Node, Role::Comment)) return true;
						Comments.push_back(Src.CommentRangeOf(Node));
						// コメントの内側へ降りない事の返戻
						return false;
					}
				);
				// 最後のコメントの群の始まり（前のコメントとの間が空白と改行１つ迄）
				size_t Group = Comments.size();
				while(Group > 1) {
					if(
						const std::string_view Gap = Text.substr(Comments[Group - 2].second, Comments[Group - 1].first - Comments[Group - 2].second);
						Gap.find_first_not_of(" \t\r\n") != std::string_view::npos || std::count(Gap.begin(), Gap.end(), '\n') > 1
					) break;
					--Group;
				}
				// 群の最初のコメントの記号の後の本文
				std::string Head;
				if(Group) {
					const std::string_view First = Text.substr(Comments[Group - 1].first, Comments[Group - 1].second - Comments[Group - 1].first);
					Head = First.substr(
						std::min(CommentRepository::OpenerOf(CommentRepository::NotationOf(Src.GetRules().Notation, First, false)).size(), First.size())
					);
				}
				Head.erase(0, std::min(Head.find_first_not_of(" \t\r\n"), Head.size()));
				ToLowerAscii(Head);
				if(
					std::any_of(
						Reading.OutputCommentWords.begin(),
						Reading.OutputCommentWords.end(),
						[&Head](const std::string_view Word) {
							// 出力を比べる登録語かの返戻
							return Head.starts_with(Word);
						}
					)
				) Ranges.emplace_back(Comments[Group - 1].first, Src.End(Body));
			}
		);
		// 出力の注記の範囲の返戻
		return Ranges;
	}

	/**
	 * 標準の指示コメントを判定する頭部の位置の取得関数（整形仕様 4.5, 7.5）
	 * 処理系は符号化宣言・マジックコメント・ビルド制約をコードより前の決まった範囲でだけ読む為，其の範囲を構文木から求める
	 * @param Src 解析済の字面
	 * @return 頭部の終わり・符号化宣言を読む範囲の終わり・１行目の終わりと，Python の２行目の符号化宣言の有無
	 */
	SourceProjector::DirectiveHead SourceProjector::DirectiveHeadOf(const ::ParsedTree &Src) {
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
				if(!IsComment) Head.HeaderEnd = Src.Start(Child);
				// 最初のコードで打切る事の返戻
				return IsComment;
			}
		);
		if(Reading.HasOutputCommentGroups) Head.ExampleOutputs = ExampleOutputRanges(Src);
		// ビルド制約・三重スラッシュ指示子を読む頭部の位置の返戻
		if(!Reading.HasSingleLineEncodingHeader && !Reading.HasConditionalSecondLineEncoding) return Head;
		const std::string_view Text = Src;
		// 符号化宣言を処理系が読む範囲の終わり（Python は先頭の２行，Ruby は１行目とシバンの次の行）
		const size_t FirstLineEnd = std::min(Text.find('\n'), Text.size());
		const size_t SecondLineEnd =
		FirstLineEnd < Text.size() ? std::min(Text.find('\n', FirstLineEnd + 1), Text.size()) : FirstLineEnd;
		Head.FirstLineEnd = static_cast<uint32_t>(FirstLineEnd);
		Head.EncodingEnd = static_cast<uint32_t>(
			std::min<size_t>(
				Head.HeaderEnd,
				Reading.HasSingleLineEncodingHeader && !Text.starts_with(CommentRepository::OpenerOf(CommentNotation::HashBangLine)) ?
				FirstLineEnd :
				SecondLineEnd
			)
		);
		// Python の２行目の符号化宣言（処理系は１行目がコメントの時だけ２行目を読む為，１行目のコメントも成立条件と為る）
		const std::string_view SecondLine =
		SecondLineEnd < Head.HeaderEnd ? Text.substr(FirstLineEnd + 1, SecondLineEnd - FirstLineEnd - 1) : std::string_view();
		const std::string_view SecondLineComment =
		SecondLine.substr(std::min(SecondLine.find_first_not_of(Reading.EncodingLineIndent), SecondLine.size()));
		Head.IsSecondLineEncoding = Reading.HasConditionalSecondLineEncoding &&
		SecondLineComment.starts_with(CommentRepository::OpenerOf(CommentNotation::HashLine)) &&
		CommentRepository::RegisteredLineFormOf(Src.GetRules().Notation.OutputForms.Conditions, SecondLineComment).IsEncoding;
		// 頭部の位置の返戻
		return Head;
	}

	/**
	 * Go の Example 関数の出力の注記かの判定関数
	 * @param Head `DirectiveHeadOf` で求めた頭部の位置と出力の注記の範囲
	 * @param TokenStart コメントの始まりの位置
	 * @return 出力の注記の群に属するコメントなら true
	 */
	bool SourceProjector::IsOutputComment(const DirectiveHead &Head, const uint32_t TokenStart) {
		// 位置以前に始まる注記群の探索
		const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator After = std::upper_bound(
			Head.ExampleOutputs.begin(),
			Head.ExampleOutputs.end(),
			TokenStart,
			[](const uint32_t Value, const std::pair<uint32_t, uint32_t> &Range) -> bool {
				// 範囲の始まりとの比較の返戻
				return Value < Range.first;
			}
		);
		// 位置以前に始まる最後の範囲に含まれるかの返戻
		return After != Head.ExampleOutputs.begin() && TokenStart < (After - 1)->second;
	}

	/**
	 * Go の公式ツールチェーンの指示コメントかの判定関数（整形仕様 4.5, 7.5）
	 * go コマンド・コンパイラ・cgo・embed の文書が名前・記法・位置条件を定める物だけを対象とし，`//go:` で始まる未知の名前は含めない
	 * 名前と区切りは記法の値で識別し，此処では位置と行位置の数値条件を判ずる
	 * @param Src 解析済の字面
	 * @param Head `DirectiveHeadOf` で求めた頭部の位置
	 * @param Raw コメント全文
	 * @param TokenStart コメントの始まりの位置
	 * @param Notation コメントの記法
	 * @param Next 同じ親の中で後に続く最初のコメントでない名前付の節点（無ければ空）
	 * @return 位置条件を満たして効いて居る指示コメントなら true
	 */
	bool SourceProjector::IsToolchainDirective(
		const ::ParsedTree &Src,
		const DirectiveHead &Head,
		const std::string_view Raw,
		const uint32_t TokenStart,
		const CommentNotation Notation,
		const TSNode Next
	) {
		// `/*line` は閉じの直後の字句の位置を定め行の何処でも効く為（コンパイラの行の指示子），位置の妥当性の返戻
		if(Notation == CommentNotation::GoLineBlock) return CommentRepository::HasValidLinePosition(Notation, Raw);
		// go test が比べる Example 関数の出力の注記の返戻
		if(IsOutputComment(Head, TokenStart)) return true;
		const std::string_view Text = Src;
		const size_t Before = TokenStart ? Text.find_last_not_of(" \t", TokenStart - 1) : std::string_view::npos;
		// 行の前にコードの在る物の返戻（コンパイラは独立した行の指示子だけを読む）
		if(Before != std::string_view::npos && Text[Before] != '\n') return false;
		const bool IsColumnZero = !TokenStart || Text[TokenStart - 1] == '\n';
		switch(Notation) {
		case CommentNotation::GoLineLine:
		case CommentNotation::GoGenerateLine:
			// 行頭（桁０）の物だけを読む行の指示子と go generate の指示子の返戻
			return IsColumnZero && CommentRepository::ActivatesAtLineHead(Notation, Raw);
		case CommentNotation::GoBuildLine:
		case CommentNotation::GoDebugLine:
			// `package` より前の頭部で読むビルド制約・GODEBUG の設定の返戻（古い記法の `// +build` は対応せず通常のコメントとする）
			return TokenStart < Head.HeaderEnd;
		case CommentNotation::GoLinknameLine:
			// 位置に依らず効く記号名の対応付けの返戻
			return true;
		case CommentNotation::GoEmbedLine:
			// 直後の変数宣言へ作用する埋込の指示子の返戻
			return Src.Is(Next, Role::VariableDeclarationNode);
		case CommentNotation::CgoExportLine:
		case CommentNotation::GccgoExternLine:
		case CommentNotation::GoNoescapeLine:
		case CommentNotation::GoNoinlineLine:
		case CommentNotation::GoNoraceLine:
		case CommentNotation::GoNosplitLine:
		case CommentNotation::GoUintptrescapesLine:
		case CommentNotation::GoWasmexportLine:
		case CommentNotation::GoWasmimportLine:
			// 直後の関数宣言へ作用する cgo の公開・gccgo の外部名・コンパイラの関数の指示子の返戻
			return Src.Is(Next, Role::PragmaTargetFunction);
		default:
			// 取込の宣言の前に在る物だけを cgo の前文の候補とする事の返戻
			if(!Src.Is(Next, Role::ImportDeclarationNode)) return false;
			// コメントの終わりから取込迄の間の空行（空白だけの行を含む，前文の内側の空の行は前文の一部の為に数えない）
			const std::string_view Gap = Text.substr(TokenStart + Raw.size(), Src.Start(Next) - TokenStart - Raw.size());
			for(size_t Newline = Gap.find('\n'); Newline != std::string_view::npos;) {
				const size_t Following = Gap.find('\n', Newline + 1);
				// 空白だけの行を挟む物は前文でない事の返戻（cgo は空行を挟まず直前に在るコメントだけを前文と読む）
				if(
					Following != std::string_view::npos &&
					Gap.substr(Newline + 1, Following - Newline - 1).find_first_not_of(" \t\r") == std::string_view::npos
				) return false;
				Newline = Following;
			}
			// 取り込むパッケージが cgo の `"C"` かの返戻
			return Src.AbsorbsGoComment(Next);
		}
	}

	/**
	 * 処理系が読む位置の符号化宣言かの判定関数（整形仕様 4.5, 7.5）
	 * Python の２行目の宣言の成立条件である１行目のコメントも含める
	 * @param Head `DirectiveHeadOf` で求めた頭部の位置
	 * @param IsEncoding コメントが符号化の指定の形か
	 * @param TokenStart コメントの始まりの位置
	 * @return 処理系が符号化の指定として読む位置の宣言又は其の成立条件なら true
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
	 * @return 有効な組の最も広く読む範囲（有効な組が無ければ無し）
	 */
	std::optional<MagicCommentScope> SourceProjector::MagicCommentScopeOf(
		const ReadingRules &Reading,
		const std::vector<MagicCommentPair> &Pairs
	) {
		// 登録済の値と適用範囲の照合
		std::optional<MagicCommentScope> Scope;
		for(const MagicCommentPair &Pair : Pairs) for(const MagicCommentWord &Word : Reading.MagicCommentWords) {
			const bool IsValid = Pair.Name == Word.Name && std::ranges::any_of(
				Word.Values,
				[&Pair](const std::string_view Value) -> bool {
					// 大小文字を問わない値の照合結果の返戻
					return Pair.Value.size() == Value.size() && std::ranges::equal(
						Pair.Value,
						Value,
						[](const char Left, const char Right) -> bool {
							// 小文字へ揃えた字の比較結果の返戻
							return std::tolower(static_cast<unsigned char>(Left)) == Right;
						}
					);
				}
			);
			if(IsValid && (!Scope || *Scope < Word.Scope)) Scope = Word.Scope;
		}
		// 最も広く読む範囲の返戻
		return Scope;
	}

	/**
	 * TypeScript の三重スラッシュ指示子の形かの判定関数（整形仕様 4.5）
	 * TypeScript の言語手引が定める `reference` / `amd-module` / `amd-dependency` の要素だけを対象とする
	 * @param Reading 登録済の指示要素
	 * @param Raw コメント全文
	 * @return 三重スラッシュの後に定めた要素が続けば true
	 */
	bool SourceProjector::IsReferenceDirective(const ReadingRules &Reading, std::string_view Raw) {
		// 三重スラッシュの後の区切空白（TypeScript は全角スペースも区切りと読む）
		Raw = CommentRepository::TrimDirectiveSeparator(
			DirectiveDefinitionId::TypeScriptReference,
			Raw.substr(std::min(CommentRepository::OpenerOf(CommentNotation::TripleSlashLine).size(), Raw.size()))
		);
		// 要素名の後が空白か閉じかの返戻
		return std::any_of(
			Reading.ReferenceDirectiveElements.begin(),
			Reading.ReferenceDirectiveElements.end(),
			[Raw, &Reading](const std::string_view Element) -> bool {
				// 要素名と其の後の区切りの照合結果の返戻
				return Raw.size() > Element.size() && Raw.starts_with(Element) &&
				Reading.ReferenceDirectiveTerminators.find(Raw[Element.size()]) != std::string_view::npos;
			}
		);
	}

	/**
	 * コメントだけの行に在るかの判定関数
	 * 計算量：行頭からコメント迄の長さ L に対し O(L)
	 * @param Text 原文
	 * @param TokenStart コメントの始まりの位置
	 * @return 行頭からコメント迄が空白だけなら true
	 */
	static bool IsCommentLine(const std::string_view Text, const uint32_t TokenStart) {
		const size_t LineEnd = Text.rfind('\n', TokenStart ? TokenStart - 1 : 0);
		const size_t LineStart = LineEnd == std::string_view::npos || !TokenStart ? 0 : LineEnd + 1;
		// 行頭からコメント迄に空白の他の字を持たないかの返戻
		return Text.substr(LineStart, TokenStart - LineStart).find_first_not_of(" \t") == std::string_view::npos;
	}

	/**
	 * 標準の指示コメントかの判定関数（整形仕様 4.5, 7.5）
	 * 言語仕様書・公式の言語参照・公式ツールチェーンのコンパイル・ビルド仕様が記法・意味・適用条件を定める物だけを対象とし，似た接頭部の注記を推測で含めない
	 * @param Src 解析済の字面
	 * @param Reading 入力の読取規則
	 * @param Head `DirectiveHeadOf` で求めた頭部の位置
	 * @param Raw コメント全文
	 * @param TokenStart コメントの始まりの位置
	 * @param Notation コメントの記法（整形仕様 3.2）
	 * @param Next 同じ親の中で後に続く最初のコメントでない名前付の節点（無ければ空）
	 * @param Form `#` の行コメントの登録形の判定結果（其の他の記法は空）
	 * @return 標準の指示コメントなら true
	 */
	bool SourceProjector::IsStandardDirective(
		const ::ParsedTree &Src,
		const ReadingRules &Reading,
		const DirectiveHead &Head,
		const std::string_view Raw,
		const uint32_t TokenStart,
		const CommentNotation Notation,
		const TSNode Next,
		const CommentRepository::RegisteredLineForm &Form
	) {
		// 標準が認める先頭の `#!` の返戻（整形仕様 3.2 の記法で決める）
		if(Notation == CommentNotation::HashBangLine) return true;
		// Go の公式ツールチェーンの指示子かの返戻
		if(Reading.HasToolchainCommentDirectives) return IsToolchainDirective(Src, Head, Raw, TokenStart, Notation, Next);
		// go コマンドが cgo の C / C++ の頭部から読むビルド制約（C / C++ の指示記法の記号はビルド制約だけ）かの返戻
		if(Reading.HasHeaderBuildConstraints) return Notation == CommentNotation::GoBuildLine && TokenStart < Head.HeaderEnd;
		// Rust の文書属性を表すコメントの返戻
		if(CommentRepository::IsDocumentAttribute(Src.GetRules().Notation, Notation)) return true;
		if(!Reading.ReferenceDirectiveElements.empty()) {
			// TypeScript の三重スラッシュ指示子（コメントだけが前に在る頭部の物だけが有効）の返戻
			return Notation == CommentNotation::TripleSlashLine && TokenStart < Head.HeaderEnd && IsReferenceDirective(Reading, Raw);
		}
		// 符号化宣言・実行時設定を表さない `#` の行コメント以外の記法（処理系は埋込文書等を読まない）の返戻
		if(Notation != CommentNotation::HashLine) return false;
		// 処理系が読む位置の Python・Ruby の符号化宣言と，Python の２行目の宣言の成立条件である１行目のコメントの返戻
		if(IsEncodingDirective(Head, Form.IsEncoding, TokenStart)) return true;
		const std::optional<MagicCommentScope> Scope = Form.Pairs ? MagicCommentScopeOf(Reading, *Form.Pairs) : std::nullopt;
		// 処理系が読む範囲の Ruby のマジックコメントかの返戻
		return Scope && (
			*Scope == MagicCommentScope::Anywhere || *Scope == MagicCommentScope::CommentLine && IsCommentLine(Src, TokenStart) ||
			TokenStart < Head.HeaderEnd
		);
	}

	/**
	 * 登録済の指示記法の記録の取得関数（整形仕様 3.2, 4.5）
	 * 登録定義に当たる記法を無効な位置・区切り・数値の物も含めて記録し，成立は `IsStandardDirective` の位置・区切り・数値の条件で決める
	 * @param Src 解析済の字面
	 * @param Reading 入力の読取規則
	 * @param Head 指示コメントを判定する頭部の位置
	 * @param Raw コメント全文
	 * @param TokenStart コメントの始まり
	 * @param Notation コメントの記法
	 * @param Next コメントの後に続く最初のコメントでない兄弟
	 * @return 登録定義に当たる記法の記録（当たらない通常コメントは無し）
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
		// `#` の行コメントの登録形（注記を一度だけ読む）
		const CommentRepository::RegisteredLineForm Form = Notation == CommentNotation::HashLine ?
		CommentRepository::RegisteredLineFormOf(Src.GetRules().Notation.OutputForms.Conditions, Raw) :
		CommentRepository::RegisteredLineForm();
		const bool IsActive = IsStandardDirective(Src, Reading, Head, Raw, TokenStart, Notation, Next, Form);
		std::optional<DirectiveDefinitionId> Definition;
		// 記法に対応する登録定義の選択
		switch(Notation) {
		case CommentNotation::HashBangLine:
			Definition = DirectiveDefinitionId::Shebang;
			break;
		case CommentNotation::GoBuildLine:
			Definition = DirectiveDefinitionId::GoBuild;
			break;
		case CommentNotation::GoDebugLine:
			Definition = DirectiveDefinitionId::GoDebug;
			break;
		case CommentNotation::GoEmbedLine:
			Definition = DirectiveDefinitionId::GoEmbed;
			break;
		case CommentNotation::GoGenerateLine:
			Definition = DirectiveDefinitionId::GoGenerate;
			break;
		case CommentNotation::GoLinknameLine:
			Definition = DirectiveDefinitionId::GoLinkname;
			break;
		case CommentNotation::GoNoescapeLine:
		case CommentNotation::GoNoinlineLine:
		case CommentNotation::GoNoraceLine:
		case CommentNotation::GoNosplitLine:
		case CommentNotation::GoUintptrescapesLine:
		case CommentNotation::GoWasmexportLine:
		case CommentNotation::GoWasmimportLine:
			Definition = DirectiveDefinitionId::GoFunctionPragma;
			break;
		case CommentNotation::GoLineLine:
			Definition = DirectiveDefinitionId::LinePosition;
			break;
		case CommentNotation::GoLineBlock:
			Definition = DirectiveDefinitionId::BlockLinePosition;
			break;
		case CommentNotation::CgoExportLine:
			Definition = DirectiveDefinitionId::CgoExport;
			break;
		case CommentNotation::GccgoExternLine:
			Definition = DirectiveDefinitionId::GccgoExtern;
			break;
		}
		// 記法だけで決まらない定義（位置で決まる Go の出力注記と cgo の前文，言語仕様の文書属性・符号化宣言・マジックコメント）
		// 記法で決まった定義の儘とする
		if(!Definition) {
			if(Reading.HasToolchainCommentDirectives && IsActive) {
				Definition = IsOutputComment(Head, TokenStart) ? DirectiveDefinitionId::ExampleOutput : DirectiveDefinitionId::CgoPreamble;
			} else if(CommentRepository::IsDocumentAttribute(Src.GetRules().Notation, Notation)) {
				Definition = DirectiveDefinitionId::DocumentAttribute;
			} else if(
				!Reading.ReferenceDirectiveElements.empty() && Notation == CommentNotation::TripleSlashLine &&
				IsReferenceDirective(Reading, Raw)
			) Definition = DirectiveDefinitionId::TypeScriptReference;
			// 符号化宣言・実行時設定を言語仕様が定める Python・Ruby の `#` の行コメントだけの登録定義（整形仕様 4.5）
			// 符号化と実行時設定の組を併せ持つ注記は処理系が読む位置の定義とし，読む位置との間を移れば定義の違いとして検証が拒否する
			else if(Notation == CommentNotation::HashLine) {
				if(IsEncodingDirective(Head, Form.IsEncoding, TokenStart)) {
					Definition = Form.IsEncoding ? DirectiveDefinitionId::EncodingDeclaration : DirectiveDefinitionId::EncodingPrecondition;
				} else if(Form.IsMagic) Definition = DirectiveDefinitionId::MagicComment;
				else if(Form.IsEncoding) Definition = DirectiveDefinitionId::EncodingDeclaration;
			}
		}
		// 登録定義に当たらない通常コメントの返戻
		if(!Definition) return std::nullopt;
		DirectiveInfo Info;
		Info.Definition = *Definition;
		Info.State = IsActive ? DirectiveState::Active : DirectiveState::Inactive;
		// Go の指示子は記号の後の空白を揃えて合成した形で照合する（揃えても処理系の読みは変わらない）
		if(CommentRepository::IsSpacedDirective(Notation)) {
			CommentRecord Composed { std::string(Raw), { CommentPlacement::Before } };
			Composed.Notation = Notation;
			Composed.Role = CommentRole::Directive;
			CommentRepository::AttachRules(Src.GetRules().Notation, Composed);
			CommentRepository::Decompose(Composed);
			Info.Conditions.push_back(CommentRepository::Compose(Composed));
		} else {
			Info.Conditions.push_back(
				DedentedBody(
					Raw,
					CommentRepository::IsValueSpacing(Info.Definition) ? std::string_view() : CommentRepository::OpenerOf(Notation),
					Info.Definition
				)
			);
		}
		// 登録済の記法の記録の返戻
		return Info;
	}

	/**
	 * 指示子の適用先の名前・受け手・型の取得関数
	 * 計算量：対象の子の数 C に対し O(C)
	 * @param Src 解析済の字面
	 * @param Node 対象の節点
	 * @return 識別に使う三つの子
	 */
	static std::array<TSNode, 3> DirectiveParts(const ::ParsedTree &Src, const TSNode Node) {
		// 適用先の名前節点の探索
		TSNode Named = Src.FieldChild(Node, ChildRole::Name);
		if(ts_node_is_null(Named) && Src.Is(Node, Role::VariableDeclarationNode)) {
			TSNode Spec = ts_node_named_child(Node, 0);
			if(!ts_node_is_null(Spec) && Src.Is(Spec, Role::VariableSpecificationList)) Spec = ts_node_named_child(Spec, 0);
			Named = ts_node_is_null(Spec) ? TSNode{} : Src.FieldChild(Spec, ChildRole::Name);
		}
		// 識別に使う子の返戻
		return { Src.FieldChild(Node, ChildRole::ReceiverField), Named, Src.FieldChild(Node, ChildRole::Type) };
	}

	/**
	 * 指示子の適用先の種類と字面の表示関数
	 * 区切の空白・コンマとコメントは展開で変わる表現差（整形仕様 8.1）の為に表示へ含めない
	 * 計算量：対象の子の部分木の節点数 C と表示に使う字面の長さ L に対し O(C + L)
	 * @param Src 解析済の字面
	 * @param Node 対象の節点
	 * @return 空白・コンマ・コメントを除いた表示
	 */
	static std::string DirectiveLabel(const ::ParsedTree &Src, const TSNode Node) {
		// 適用先の種類と字面の組立
		std::string Result(Src.GetProfile().SymbolName(ts_node_symbol(Node)));
		for(const TSNode Part : DirectiveParts(Src, Node)) {
			Result.push_back(':');
			if(ts_node_is_null(Part)) continue;
			WalkAst(
				Part,
				[&Src, &Result](const TSNode Leaf) -> void {
					// 字句でない節点と区切のコンマ・コメントの場合の終了
					if(ts_node_child_count(Leaf) || Src.Is(Leaf, Role::CommaToken) || Src.Is(Leaf, Role::Comment)) return;
					for(const char Char : Src.View(Leaf)) if(!std::isspace(static_cast<unsigned char>(Char))) Result.push_back(Char);
				}
			);
		}
		// 表示の返戻
		return Result;
	}

	/**
	 * 指示子の所有範囲の識別の取得関数（整形仕様 8.1）
	 * 根から節点の親迄の名前・受け手・型を持つ節点（所有範囲）を種類と其の字面で表す
	 * 計算量：節点の深さ D，祖先の子の数の和 C，表示の長さ L に対し O(D + C + L)
	 * @param Src 解析済の字面
	 * @param Node 節点
	 * @return 所有範囲の表示の並び
	 */
	std::string SourceProjector::DirectiveScope(const ::ParsedTree &Src, const TSNode Node) {
		// 根から節点の親迄の祖先（節点の始まりへ向かって降りる）
		std::vector<TSNode> Ancestors;
		HeldCursor Cursor(Src.GetRoot());
		const uint32_t Start = ts_node_start_byte(Node);
		while(!ts_node_eq(ts_tree_cursor_current_node(&Cursor), Node)) {
			Ancestors.push_back(ts_tree_cursor_current_node(&Cursor));
			if(ts_tree_cursor_goto_first_child_for_byte(&Cursor, Start) == -1) break;
		}
		// 所有範囲の表示の組立
		std::string Result;
		for(const TSNode Up : Ancestors) {
			// 名前・受け手・型を持つ祖先だけを所有範囲とする
			if(
				std::ranges::all_of(
					DirectiveParts(Src, Up),
					[](const TSNode Part) -> bool {
						// 子の無い事の返戻
						return ts_node_is_null(Part);
					}
				)
			) continue;
			Result.append(DirectiveLabel(Src, Up)).push_back('/');
		}
		// 所有範囲の表示の並びの返戻
		return Result;
	}

	/**
	 * 指示子の適用先の識別の取得関数（整形仕様 8.1，構文・所有範囲・境界の対応で照合し，同名又は記号の一致だけで同一視しない）
	 * 所有範囲の表示の並びに，適用先の種類と其の字面，同じ親の中で同じ表示を持つ先行する兄弟の数（境界の順）を添える（変数宣言は最初の変数の名前で表す）
	 * 計算量：適用先の深さ D，祖先の子の数の和 C，表示の長さ L に対し O(D + C + L)
	 * @param Src 解析済の字面
	 * @param Declaration 宣言の節点
	 * @param Ordinal 同じ親の中で同じ表示を持つ先行する兄弟の数
	 * @return 所有範囲の表示の並びと，適用先の表示と境界の順
	 */
	std::string SourceProjector::DirectiveTarget(const ::ParsedTree &Src, const TSNode Declaration, const uint32_t Ordinal) {
		// 所有範囲と適用先の表示と境界の順の返戻
		return DirectiveScope(Src, Declaration).append(DirectiveLabel(Src, Declaration)).append("#").append(std::to_string(Ordinal));
	}

	/**
	 * 標準の指示コメントの一覧の取得関数（整形仕様 4.5, 8.1）
	 * 入力と最終字面の双方から同じ規則で集め，本文・記法・有効性・位置条件・適用先の宣言が保たれたかを検証側が比べる
	 * 計算量：節点数 V，表示の総長 L，異なる適用先の祖先の子の数の和 C と深さの和 D に対し O(V + L + C + D)
	 * @param Src 解析済の字面
	 * @param Head コメントの分離で求めた指示子の頭部
	 * @param Nodes 投影で原文順に訪ねた節点
	 * @param Starts 指示コメントの始まりの位置の昇順の格納先（不要なら nullptr）
	 * @param Definitions 指示コメントの登録定義の格納先（始まりの位置と同じ順，不要なら nullptr）
	 * @return 原文順の指示コメントの本文（継続行の共通の字下げと行末の空白を除き，宣言へ作用する物は其の名前を後へ添えた形）
	 */
	std::vector<std::string> SourceProjector::CollectDirectives(
		const ::ParsedTree &Src,
		const DirectiveHead &Head,
		const std::span<const TSNode> Nodes,
		std::vector<uint32_t> *const Starts,
		std::vector<DirectiveDefinitionId> *const Definitions
	) {
		// 指示コメントの収集先の準備
		std::vector<std::string> Directives;
		// 構文木の無い字面の返戻
		if(!Src.IsParsed()) return Directives;
		const std::string_view Text = Src;
		const NotationRules &Rules = Src.GetRules().Notation;
		const ReadingRules &Reading = Src.GetReading();
		const TSNode Root = Src.GetRoot();
		// 後続の兄弟に依存しない文法では，登録形が無ければ兄弟索引を作らない
		if(!Reading.HasToolchainCommentDirectives) {
			bool HasDirective = false;
			for(const TSNode Node : Nodes) {
				if(!ts_node_is_named(Node) || ts_node_eq(Node, Root) || !Src.Is(Node, Role::Comment)) continue;
				const auto [TokenStart, TextEnd] = Src.CommentRangeOf(Node);
				if(TokenStart >= TextEnd) continue;
				if(
					const std::string_view Raw = Text.substr(TokenStart, TextEnd - TokenStart);
					DirectiveOf(Src, Reading, Head, Raw, TokenStart, CommentRepository::NotationOf(Rules, Raw, !TokenStart), TSNode{})
				) {
					HasDirective = true;
					break;
				}
			}
			// 登録形の指示コメントが無い場合の空の索引の返戻
			if(!HasDirective) return Directives;
		}
		// 節点毎の親（文書を属性として読む文法だけ）と，コメント毎の後のコメントでない名前付の兄弟
		std::unordered_map<const void *, TSNode> Parents, Following;
		// 適用先が現れた時だけ木の親表を作り，同じ親の兄弟の順位を一度だけ数える
		std::unordered_map<const void *, uint32_t> Ordinals;
		std::unordered_map<const void *, TSNode> TargetParents;
		std::unordered_set<const void *> ScannedParents;
		bool HasTargetParents = false;
		// 節点毎の兄弟関係の索引
		std::vector<TSNode> Children;
		for(const TSNode Node : Nodes) {
			Children.clear();
			HeldCursor Cursor(Node);
			if(ts_tree_cursor_goto_first_child(&Cursor)) {
				do Children.push_back(ts_tree_cursor_current_node(&Cursor));
				while(ts_tree_cursor_goto_next_sibling(&Cursor));
			}
			TSNode Pending {};
			for(size_t Index = Children.size(); Index--;) {
				const TSNode Child = Children[Index];
				if(Rules.DocumentCommentIsAttribute) Parents.emplace(Child.id, Node);
				if(!ts_node_is_named(Child)) continue;
				if(Src.Is(Child, Role::Comment)) Following.emplace(Child.id, Pending);
				else Pending = Child;
			}
		}
		// 親又は後の兄弟の表の節点（表に無ければ空）
		const auto Lookup = [](const std::unordered_map<const void *, TSNode> &Table, const TSNode Node) -> TSNode {
			const std::unordered_map<const void *, TSNode>::const_iterator Found =
			ts_node_is_null(Node) ? Table.end() : Table.find(Node.id);
			// 表の節点の返戻
			return Found == Table.end() ? TSNode{} : Found->second;
		};
		// 適用先の表示の控え
		std::unordered_map<const void *, std::string> Targets;
		const auto TargetOf = [&Src, &Ordinals, &Targets, &TargetParents, &ScannedParents, &HasTargetParents, Root](const TSNode Target)
		-> const std::string & {
			// 保存済の適用先の表示の返戻
			if(
				const std::unordered_map<const void *, std::string>::const_iterator Found = Targets.find(Target.id);
				Found != Targets.end()
			) return Found->second;
			// 適用先の親表の遅延構築
			if(!HasTargetParents) {
				WalkChildrenCursor(
					Root,
					[&TargetParents](const TSNode Node, const TSNode Parent) -> bool {
						TargetParents.emplace(Node.id, Parent);
						// 子の走査を続ける事の返戻
						return true;
					}
				);
				HasTargetParents = true;
			}
			// 同じ表示の兄弟順位の計数
			if(
				const std::unordered_map<const void *, TSNode>::const_iterator Parent = TargetParents.find(Target.id);
				Parent != TargetParents.end() && ScannedParents.insert(Parent->second.id).second
			) {
				std::unordered_map<TSSymbol, std::unordered_map<std::string, uint32_t>> Occurrences;
				std::vector<TSNode> Unnamed;
				HeldCursor Cursor(Parent->second);
				if(ts_tree_cursor_goto_first_child(&Cursor)) {
					do {
						if(const TSNode Child = ts_tree_cursor_current_node(&Cursor); !ts_node_is_named(Child)) Unnamed.push_back(Child);
						else {
							uint32_t &Count = Occurrences[ts_node_symbol(Child)][DirectiveLabel(Src, Child)];
							Ordinals.emplace(Child.id, Count++);
						}
					} while(ts_tree_cursor_goto_next_sibling(&Cursor));
					for(const TSNode Child : Unnamed) {
						const std::unordered_map<TSSymbol, std::unordered_map<std::string, uint32_t>>::const_iterator Symbol =
						Occurrences.find(ts_node_symbol(Child));
						if(Symbol == Occurrences.end()) continue;
						if(
							const std::unordered_map<std::string, uint32_t>::const_iterator Label = Symbol->second.find(DirectiveLabel(Src, Child));
							Label != Symbol->second.end()
						) Ordinals.emplace(Child.id, Label->second);
					}
				}
			}
			const std::unordered_map<const void *, uint32_t>::const_iterator Ordinal = Ordinals.find(Target.id);
			// 適用先の表示を保存した結果の返戻
			return Targets.emplace(Target.id, DirectiveTarget(Src, Target, Ordinal == Ordinals.end() ? 0 : Ordinal->second)).first->second;
		};
		// 同じ投影走査の節点のコメントを原文順に読み，其の後のコメントでない名前付の兄弟を使う
		for(const TSNode Node : Nodes) {
			if(!ts_node_is_named(Node) || ts_node_eq(Node, Root) || !Src.Is(Node, Role::Comment)) continue;
			const TSNode Next = Lookup(Following, Node);
			const auto [TokenStart, TextEnd] = Src.CommentRangeOf(Node);
			// 本文の無いコメントの読飛
			if(TokenStart >= TextEnd) continue;
			const std::string_view Raw = Text.substr(TokenStart, TextEnd - TokenStart);
			const CommentNotation Notation = CommentRepository::NotationOf(Rules, Raw, !TokenStart);
			// 指示コメントでないコメントの読飛
			const std::optional<DirectiveInfo> Info = DirectiveOf(Src, Reading, Head, Raw, TokenStart, Notation, Next);
			if(!Info) continue;
			if(Starts) Starts->push_back(TokenStart);
			if(Definitions) Definitions->push_back(Info->Definition);
			// 無効な位置・区切りの物も入力時の状態と共に比べ，有効化と記法・本文の変更を見逃さない（整形仕様 8.1）
			Directives.push_back(Info->State == DirectiveState::Active ? Info->Conditions.front() : "inactive " + Info->Conditions.front());
			// 宣言へ作用する Go の指示子は適用先も比べ，別の宣言の前へ移って居ない事を確かめる
			if(
				!ts_node_is_null(Next) && Reading.HasToolchainCommentDirectives &&
				(Src.Is(Next, Role::VariableDeclarationNode) || Src.Is(Next, Role::PragmaTargetFunction))
			) Directives.back().append("\n->").append(TargetOf(Next));
			// 後の定数へ有効範囲毎に作用する実行時設定は所有範囲を比べ，別の範囲へ移って居ない事を確かめる（後の文の構文の種類は正規化で変わり得る為に比べない）
			else if(!ts_node_is_null(Next) && Info->Definition == DirectiveDefinitionId::MagicComment) {
				Directives.back().append("\n->").append(DirectiveScope(Src, Next));
			}
			// Rust の外側の文書は後の項目，内側の文書は本体を持つ項目（根では crate）へ作用する為，適用先も比べる
			if(Rules.DocumentCommentIsAttribute) {
				const bool IsInner = Notation == CommentNotation::SlashBangLine || Notation == CommentNotation::StarBangBlock;
				// 内側の文書の適用先はコメントの親を持つ項目（根では crate）
				const TSNode Parent = Lookup(Parents, Node), Outer = Lookup(Parents, Parent);
				if(const TSNode Target = IsInner ? ts_node_is_null(Outer) ? Parent : Outer : Next; !ts_node_is_null(Target)) {
					Directives.back().append(IsInner ? "\n<-" : "\n->").append(TargetOf(Target));
				}
			}
		}
		// 指示コメントの一覧の返戻
		return Directives;
	}

	/**
	 * 投影出来ない候補の指示コメントの取得関数
	 * @param Src 解析済の候補
	 * @param Nodes 候補の節点の原文順の並び
	 * @param Starts 指示コメントの開始位置の格納先
	 * @return 指示コメントの本文と位置条件の並び
	 */
	std::vector<std::string> SourceProjector::CollectDirectives(
		const ::ParsedTree &Src,
		const std::span<const TSNode> Nodes,
		std::vector<uint32_t> *const Starts
	) {
		// 候補を投影出来ない時だけ頭部を読んで集めた指示コメントの返戻
		return CollectDirectives(Src, DirectiveHeadOf(Src), Nodes, Starts, nullptr);
	}

	/**
	 * 錨毎のコメントの検索関数
	 * @param Map 参照する表（先行又は末尾）
	 * @param Node 対象の錨
	 * @return 紐付いたコメントの列（無ければ共有の空列）
	 */
	const std::vector<CommentRecord> &SourceProjector::CommentSeparation::FindAttached(
		const std::unordered_map<const void *, std::vector<CommentRecord>> &Map,
		const TSNode Node
	) {
		// 未紐付の時に返す共有の空列
		static constexpr std::vector<CommentRecord> Empty;
		const std::unordered_map<const void *, std::vector<CommentRecord>>::const_iterator Iter = Map.find(Node.id);
		// 錨に紐付いたコメントの返戻
		return Iter == Map.end() ? Empty : Iter->second;
	}

	/**
	 * 位置を含む分離したコメントの除く範囲の取得関数（整形仕様 7.1）
	 * @param Position 原文の位置
	 * @return 位置を含む除く範囲（無ければ nullptr）
	 */
	const SourceProjector::CommentSeparation::Range *SourceProjector::CommentSeparation::FindSeparation(
		const uint32_t Position
	) const {
		// 位置より後に始まる最初の範囲
		const std::vector<Range>::const_iterator After = std::upper_bound(
			Separations.begin(),
			Separations.end(),
			Position,
			[](const uint32_t At, const Range &Candidate) -> bool {
				// 範囲が位置より後に始まるかの返戻
				return At < Candidate.Begin;
			}
		);
		// 位置より前に始まる範囲が無い場合の返戻
		if(After == Separations.begin()) return nullptr;
		const Range &Previous = *std::prev(After);
		// 位置を含む直前の範囲の返戻
		return Position < Previous.End ? &Previous : nullptr;
	}

	/**
	 * 錨へのコメントの紐付関数
	 * @param Node 対象の錨
	 * @param IsLeading 先行コメントか（偽は末尾コメント）
	 * @param Comment 紐付けるコメント
	 */
	void SourceProjector::CommentSeparation::Attach(const TSNode Node, const bool IsLeading, CommentRecord &&Comment) {
		(IsLeading ? LeadingByNode : TrailingByNode)[Node.id].push_back(std::move(Comment));
		AttachedStartBytes.insert(ts_node_start_byte(Node));
		// 終了
		return;
	}

	/**
	 * 先行コメント取得関数
	 * @param Node 対象ノード
	 * @return 紐付けられたコメント列（無ければ空リストへの参照）
	 */
	const std::vector<CommentRecord> &SourceProjector::CommentSeparation::GetLeading(const TSNode Node) const {
		// 先行の表からの検索の返戻
		return FindAttached(LeadingByNode, Node);
	}

	/**
	 * 後続コメント取得関数
	 * @param Node 対象ノード
	 * @return 紐付けられたコメント列（無ければ空リストへの参照）
	 */
	const std::vector<CommentRecord> &SourceProjector::CommentSeparation::GetTrailing(const TSNode Node) const {
		// 末尾の表からの検索の返戻
		return FindAttached(TrailingByNode, Node);
	}

	/**
	 * 範囲内紐付コメント有無の判定関数
	 * @param From 範囲開始（含む）
	 * @param To 範囲終了（含まず）
	 * @return 範囲内に紐付済コメントが有る場合 true
	 */
	bool SourceProjector::CommentSeparation::HasAttachedCommentInRange(const uint32_t From, const uint32_t To) const {
		const std::set<uint32_t>::const_iterator Iter = AttachedStartBytes.lower_bound(From);
		// 範囲に紐付済の錨が在るかの返戻
		return Iter != AttachedStartBytes.end() && *Iter < To;
	}

	/**
	 * 分離したコメントの節点かの判定関数（整形仕様 7.1）
	 * @param Src 解析結果
	 * @param Node 節点
	 * @return 錨へ紐付けて原文の字面から除くコメントの節点なら true
	 */
	bool SourceProjector::CommentSeparation::IsSeparatedComment(const ::ParsedTree &Src, const TSNode Node) const {
		// 分離したコメントの始まりに一致するコメントの節点かの返戻
		return Src.Is(Node, Role::Comment) &&
		std::binary_search(SeparatedStarts.begin(), SeparatedStarts.end(), ts_node_start_byte(Node));
	}

	/**
	 * 分離したコメントを除く範囲の中の位置かの判定関数（整形仕様 7.1）
	 * @param Position 原文の位置
	 * @return 錨へ紐付けて原文の字面から除く範囲の中なら true
	 */
	bool SourceProjector::CommentSeparation::IsSeparatedAt(const uint32_t Position) const {
		// 除く範囲の有無の返戻
		return FindSeparation(Position);
	}

	/**
	 * 除いた跡へ固定の区切記法を置く分離したコメントかの判定関数（整形仕様 6.1, 7.1）
	 * @param Comment 分離したコメントの節点
	 * @return コメントを含む除く範囲の跡へ固定の区切記法を置くなら true
	 */
	bool SourceProjector::CommentSeparation::IsNotationSeparated(const TSNode Comment) const {
		const Range *const Found = FindSeparation(ts_node_start_byte(Comment));
		// 範囲の区切が固定の区切記法かの返戻
		return Found && Found->IsNotation;
	}

	/**
	 * 分離したコメントを除いた範囲の字面の合成関数（整形仕様 4.3, 7.1）
	 * 除く範囲を跡へ補う区切へ置き換え，範囲の端に掛かる除く範囲は範囲の内側だけを除き，残る原文の連続した断片を其の儘追加関数へ渡す
	 * 計算量：範囲の長さ N と除く範囲の数 K に対し O(N + log K)
	 * @param From 範囲の始まり
	 * @param To 範囲の終わり
	 * @param Fragment 連続した断片（字面と始まりと終わり）の追加関数
	 * @param Notations 跡へ置いた区切記法（CSS の空の囲み）の結果の中の範囲の格納先（不要なら nullptr）
	 * @return 分離したコメントを除いた字面
	 */
	template<class Append> std::string SourceProjector::CommentSeparation::JoinFragments(
		const uint32_t From,
		const uint32_t To,
		Append &&Fragment,
		std::vector<std::pair<uint32_t, uint32_t>> *const Notations
	) const {
		std::string Text;
		// 空の範囲の返戻
		if(To <= From) return Text;
		Text.reserve(To - From);
		uint32_t Cursor = From;
		for(
			std::vector<Range>::const_iterator Found = std::upper_bound(
				Separations.begin(),
				Separations.end(),
				From,
				[](const uint32_t Position, const Range &Candidate) -> bool {
					// 範囲が位置より後に終わるかの返戻
					return Position < Candidate.End;
				}
			);
			Found != Separations.end() && Found->Begin < To;
			++Found
		) {
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
		// 分離したコメントを除いた字面の返戻
		return Text;
	}

	/**
	 * 分離したコメントを除いた範囲の字面の取得関数（整形仕様 4.3, 7.1）
	 * @param Source 原文
	 * @param From 範囲の始まり
	 * @param To 範囲の終わり
	 * @param Notations 跡へ置いた区切記法（CSS の空の囲み）の結果の中の範囲の格納先（不要なら nullptr）
	 * @return 分離したコメントを除いた字面
	 */
	std::string SourceProjector::CommentSeparation::SeparatedText(
		const std::string &Source,
		const uint32_t From,
		const uint32_t To,
		std::vector<std::pair<uint32_t, uint32_t>> *const Notations
	) const {
		// 原文の断片を其の儘繋いだ字面の返戻
		return JoinFragments(
			From,
			To,
			[&Source](std::string &Text, const uint32_t Begin, const uint32_t End) -> void {
				Text.append(Source.data() + Begin, End - Begin);
			},
			Notations
		);
	}

	/**
	 * 分離したコメントを除き断片毎に復号した範囲の字面の取得関数（整形仕様 4.2, 4.4）
	 * 解析用の置換と印は，コメントを除いて不連続に為った断片毎に復号し，断片を跨いで位置を推定しない
	 * @param Src コメントを分離した解析結果
	 * @param From 範囲の始まり
	 * @param To 範囲の終わり
	 * @return 分離したコメントを除き本来の字句へ戻した字面
	 */
	std::string SourceProjector::CommentSeparation::DecodedText(
		const ::ParsedTree &Src,
		const uint32_t From,
		const uint32_t To
	) const {
		// 断片毎に復号して繋いだ字面の返戻
		return JoinFragments(
			From,
			To,
			[&Src](std::string &Text, const uint32_t Begin, const uint32_t End) -> void {
				Text.append(ParseAdapter::DecodeLexeme(Src, std::string(Src, Begin, End - Begin), Begin));
			},
			nullptr
		);
	}

	/**
	 * 原文からコメントを分離して錨へ紐付ける投影関数（整形仕様 3.1, 7.1）
	 * コメントノードを直前／直後の名前付非コメントノードに紐付け，原文を解析し直さずに本文から除く範囲と跡へ補う区切を記録する
	 * 計算量：原稿長 N，節点数 V，コメント数 K，錨判定・本文正規化の総費用 A に対し平均 O(N + V + K * log(K + 1) + A)
	 * @param Src 投影する原稿（書き換えない）
	 * @param Input 投影の種別を含む入力
	 * @return 錨へ紐付けたコメントと本文から除く範囲・跡へ補う区切（投影の間だけ持つ）
	 */
	SourceProjector::CommentSeparation SourceProjector::SeparateComments(const ::ParsedTree &Src, const ParseInput &Input) {
		// 分離結果と入力条件の準備
		CommentSeparation Result;
		// 未解析原稿と，コメントが文書の内容である原稿の分離の無い結果の返戻
		if(!Src.IsParsed() || Src.GetReading().HasCommentDocumentContent) return Result;
		const bool IsVerification = std::holds_alternative<ParseInput::VerificationSource>(Input.Source);
		const SyntaxRules &Rules = Src.GetRules();
		const ReadingRules &Reading = Src.GetReading();
		// Ruby の埋込ドキュメントの本文の `=end` へ解析の為に挟んだ印（整形仕様 4.2，挟んで居なければ無し）
		const char DocMarker = Rules.Notation.IsAvailable(CommentNotation::RubyEmbeddedDocument) ? Src.GetAnalysisMark() : '\0';
		// 言語標準の指示コメントを判定する頭部の位置（整形仕様 4.5, 7.5）
		const DirectiveHead Head = DirectiveHeadOf(Src);
		Result.Head = Head;
		// PHP の閉じタグの後の地の文の始まり（其の中の解析器がコメントと読んだ字面は地の文の儘保つ，整形仕様 4.4）
		const uint32_t BodyEnd = ParseAdapter::OuterTextBounds(Src).second;
		// 出力コメント群・最後の子・包装降下先の判定を再利用
		struct LineScan {
			std::unordered_map<const void *, TSNode> LastChild;
			std::unordered_map<const void *, std::pair<TSNode, uint16_t>> Descent; // 包装から降りた錨と越えた無名字句の数
			std::vector<std::pair<TSNode, uint16_t>> DescentPath; // 降りた包装と其の手前迄に越えた無名字句の数
		};
		// コメント本文の削除範囲
		struct DeleteRange {
			uint32_t Begin;
			uint32_t End;
			bool IsJoinable = false; // 改行を含んでも跡に改行を残さないコメントか（JS / TS は文の中の物，Go・Swift は無し，其の他の言語は全て）
			bool IsSpaceLike = false; // 空白と同じに読まれるコメントか（Sass は選択子の中のコメントを子孫結合子の空白と読む）
		};
		struct Pending {
			uint32_t CommentStart;
			TSNode Anchor {}; // 錨の節点
			CommentRecord Comment;
			uint32_t CommentEnd = 0;
			bool ShouldNormalizeLeading = false;
			bool IsVerbatim = false;
		};
		// 同親の前後の名前付非コメント子と祖先側の前後を１度控え，並列収集中は読取専用（兄弟・祖先の再照会に依る三乗費用を回避）
		struct Neighbors {
			TSNode Prev;
			TSNode Next;
			TSNode OuterPrev; // 親から遡った直前の名前付非コメントの節点
			TSNode OuterNext;
			TSNode ParentPrev;
			TSNode ParentNext;
			TSNode Following; // 後に続く最初のコメントでない子（字句を含み，親の中に無ければコメントと共に終わる祖先の後に続く物）
			bool IsAfterOpener;
			bool IsAfterCloser;
			bool IsAfterToken;
			uint16_t UnnamedBefore; // 親の中の前の名前無の字句の数（数の上限で飽和，以下同じ）
			uint16_t UnnamedAfterPrev; // Prev との間の名前無の字句の数
			uint16_t UnnamedToNext; // Next との間の名前無の字句の数
			uint16_t HiddenToNext; // Next との間の文法が子に出さない隠れた字句の数（誤りを含む親の読み飛ばした字句は数えない）
		};
		// 並列処理前の隣接関係確定と各走脈からの読取専用化
		std::unordered_map<const void *, Neighbors> Adjacent;
		// 原文順のコメントの範囲の表（行末の判定で後のコメントを二分探索で飛ばし，節点の引直に依る二乗化を防ぐ）
		// 走脈を分ける前に作り，並列の区間からは読むだけにする
		const std::vector<::ParsedTree::CommentSpan> &CommentSpans = Src.GetCommentSpans();
		// コメントを子孫に持つ親だけで，錨の兄弟関係を組み立てる
		std::unordered_set<const void *> ParentsWithComments;
		if(!CommentSpans.empty()) {
			WalkChildrenCursor(
				Src.GetRoot(),
				[&Src, &ParentsWithComments](const TSNode Node, const std::span<const TSNode> Ancestors) -> bool {
					if(Src.Is(Node, Role::Comment)) {
						for(size_t Index = Ancestors.size(); Index--;) if(!ParentsWithComments.insert(Ancestors[Index].id).second) break;
					}
					// 入れ子のコメントも元の兄弟走査と同じく調べる事の返戻
					return true;
				}
			);
		}
		// 祖先経路と最後の子を再利用し，コメントの保留分・削除範囲を走脈別に収集
		const auto LastChildOf = [&Src](const TSNode Node, LineScan &Memo) -> TSNode {
			// 非コメント末尾子の取得と保持
			const auto [Slot, IsNew] = Memo.LastChild.try_emplace(Node.id);
			if(IsNew) {
				ForEachNamedChild(
					Node,
					[&Src, & Last = Slot->second](const TSNode Child) -> void {
						// 非コメント子による末尾子の更新
						if(!Src.Is(Child, Role::Comment)) Last = Child;
					}
				);
			}
			// コメントでない最後の名前付の子の返戻
			return Slot->second;
		};
		// コメント毎の錨と除去範囲の決定
		const auto ProcessComment = [&Src, DocMarker, Head, BodyEnd, &Adjacent, &CommentSpans, &LastChildOf, &Rules, &Reading](
			const TSNode Node,
			const std::span<const TSNode> Ancestors,
			std::vector<Pending> &PendingList,
			std::vector<DeleteRange> &DeleteRanges,
			std::vector<uint32_t> &Separated,
			LineScan &LastScan
		) -> void {
			// 親と祖父（根の直下のコメントは祖父を持たず，根其の物のコメントは親も持たない）
			const TSNode NodeParent = Ancestors.empty() ? TSNode{} : Ancestors.back();
			// コメントの祖父節点
			const TSNode NodeGrandparent = Ancestors.size() > 1 ? Ancestors[Ancestors.size() - 2] : TSNode{};
			// コメントを含む原文
			const std::string &Self = Src;
			// 本文の範囲（末尾の改行と誤り回復で頭へ付いた空白を除き，幅０の節点は直前の囲みコメントの本文を回収する）
			const auto [TokenStart, TextEnd] = Src.CommentRangeOf(Node);
			// 回収出来ない幅０の節点と，PHP の本体の外の地の文の中の字面を扱わない事の返戻
			if(TokenStart >= TextEnd || TokenStart >= BodyEnd) return;
			// 以下の何れの経路も本文を原文の字面から除き，投影は此の節点を読み飛ばす
			Separated.push_back(ts_node_start_byte(Node));
			// 同じ親の直前・直後の名前付非コメント子と，同じ親に無ければ親から遡った隣接と行頭のコメントか（控えた物を引く）
			const std::unordered_map<const void *, Neighbors>::const_iterator Neighbor = Adjacent.find(Node.id);
			const auto [
				PrevImmediate,
				NextImmediate,
				OuterPrev,
				OuterNext,
				ParentPrev,
				ParentNext,
				Following,
				IsAfterOpener,
				IsAfterCloser,
				IsAfterToken,
				UnnamedBefore,
				UnnamedAfterPrev,
				UnnamedToNext,
				HiddenToNext
			] = Neighbor == Adjacent.end() ? Neighbors{} : Neighbor->second;
			// 原文の連続領域
			const char *const SelfData = Self.data();
			const uint32_t SelfSize = static_cast<uint32_t>(Self.size());
			// 括弧・塊の所有構文を取得（包装は祖父，直置の塊は自身，括弧を直に持つ構文は親）
			const auto BlockOwner = [&]() -> TSNode {
				// 持主の構文の返戻
				return IsOpenBracketChar(SelfData[Src.Start(NodeParent)]) && !ts_node_is_null(NodeGrandparent) &&
				!Src.Is(NodeGrandparent, Role::StatementHost) ? NodeGrandparent : NodeParent;
			};
			// 語だけの `then` / `do`（空の本体）か，其れで終わる節か
			const auto EndsWithEmptyKeywordBody = [&Src](const TSNode Node) -> bool {
				const TSNode Last = ts_node_child_count(Node) ? ts_node_child(Node, ts_node_child_count(Node) - 1) : TSNode{};
				// 空の本体か其れで終わる節かの返戻
				return Src.Is(Node, Role::TransparentSection) && !ts_node_named_child_count(Node) ||
				!ts_node_is_null(Last) && Src.Is(Last, Role::TransparentSection) && !ts_node_named_child_count(Last);
			};
			// 畳んで空に為る後の節を持つ構文（同じ行で閉じる祖先迄遡り，無ければ空の節点）
			const auto EmptyClauseOwner = [&]() -> TSNode {
				// 遡りの起点
				TSNode Up = BlockOwner();
				while(!ts_node_is_null(Up)) {
					// 空の本体で終わる構文の返戻
					if(TextEdit::EndsWithEmptyBody(Self, Src.End(Up), Reading)) return Up;
					const TSNode Parent = ts_node_parent(Up);
					// 閉じる行が変われば別の構文の為，遡りの打切
					if(ts_node_is_null(Parent) || ts_node_end_point(Parent).row != ts_node_end_point(Up).row) break;
					Up = Parent;
				}
				// 空の本体で終わる構文が無い事の返戻
				return TSNode{};
			};
			// 原文でのコメントの種類（コメントの分離と `KindOf` で同じ規則を使う）
			bool IsAtLineStart = false;
			const CommentKind Kind = Src.ClassifyComment(CommentSpans, TokenStart, TextEnd, &IsAtLineStart);
			// 行頭に在っても後ろに同じ行のコードが続くコメントは其のコードの説明の為，行内のコメントとして後ろの要素へ紐付ける
			const bool IsLeading = Kind == CommentKind::OwnLine, IsInline = Kind == CommentKind::Inline;
			// 原文から除く範囲の終端
			uint32_t DelEnd = TextEnd;
			if(IsLeading && DelEnd < Self.size() && Self[DelEnd] == '\n') ++DelEnd;
			// 本文の無い CSS / SCSS の囲みは字句の区切の記法で，除いた跡に要れば区切を置き直す為に記録しない（整形仕様 6.1, 6.6）
			if(
				!Rules.LexicalSeparator.empty() && std::string_view(Src.data() + TokenStart, TextEnd - TokenStart) == Rules.LexicalSeparator
			) {
				DeleteRanges.push_back({ TokenStart, TextEnd });
				// 終了
				return;
			}
			CommentRecord Attach;
			// 走査中不変の Self を指すコメントの字面
			const std::string_view Raw(Src.data() + TokenStart, TextEnd - TokenStart);
			// 記号と本文の境界を決める記法（整形仕様 3.2，原文の記号の形だけを表す）
			const CommentNotation Notation = CommentRepository::NotationOf(Rules.Notation, Raw, !TokenStart);
			// 言語標準の指示コメントは本文・記法・位置を保つ（整形仕様 4.5, 7.5，同じ形の普通の説明は位置で見分ける）
			Attach.Directive = DirectiveOf(Src, Reading, Head, Raw, TokenStart, Notation, NextImmediate);
			Attach.Role = Attach.Directive ? CommentRole::Directive : CommentRole::Presentation;
			Attach.Notation = Notation;
			CommentRepository::AttachRules(Rules.Notation, Attach);
			// 本文は分解が各論理行の行頭の空白を除く為，原文の字面を其の儘写す（整形仕様 7.6）
			Attach.Text = std::string(Raw);
			// Ruby の埋込ドキュメントの本文へ解析の為に挟んだ印を除き，本文を原文へ戻す (`ParseAdapter::EscapeRubyEmbeddedDocs`)
			if(DocMarker && Raw.starts_with(CommentRepository::OpenerOf(CommentNotation::RubyEmbeddedDocument))) {
				// 終端記号の最初の字の後へ印を挟んだ字面
				const std::string_view Closer = CommentRepository::CloserOf(CommentNotation::RubyEmbeddedDocument);
				const std::string Escaped = std::string(1, Closer.front()) + DocMarker + std::string(Closer.substr(1));
				for(size_t Found = Attach.Text.find(Escaped); Found != std::string::npos; Found = Attach.Text.find(Escaped, Found + 1)) {
					Attach.Text.erase(Found + 1, 1);
				}
			}
			// 独立コメントの先行紐付の既定値
			Attach.Placement.Side = IsLeading ? CommentPlacement::Before : CommentPlacement::After;
			// 原文での種別（整形仕様 7.6 に従い整形の間中保持する，所属から組み直すと種別の意味を変えた時に静かにずれる）
			Attach.Kind = Kind;
			// 後段で付ける原文順
			Attach.OriginalOrder = 0;
			// 閉じの直後の字句の位置を定める Go の `/*line` は，後の字句との密着を保つ
			if(Reading.HasPositionDirectiveComments && Attach.Role == CommentRole::Directive) {
				Attach.Placement.IsGluedAfter =
				Notation == CommentNotation::GoLineBlock && TextEnd < Self.size() && Self[TextEnd] != ' ' && Self[TextEnd] != '\n';
			}
			// 直前・直後の兄弟（無ければ親から遡った直前・直後の兄弟）
			const TSNode PrevOrOuter = ts_node_is_null(PrevImmediate) ? OuterPrev : PrevImmediate;
			const TSNode NextOrOuter = ts_node_is_null(NextImmediate) ? OuterNext : NextImmediate;
			// 子文字列境界を保つコメントだけの JSX 式包装の錨化
			if(!ts_node_is_null(NodeParent) && Src.Is(NodeParent, Role::EmbeddedExpression)) {
				bool IsAllComment = ts_node_named_child_count(NodeParent);
				ForEachNamedChild(
					NodeParent,
					[&Src, &IsAllComment](const TSNode Child) -> bool {
						// コメントを読み飛ばす事の返戻
						if(Src.Is(Child, Role::Comment)) return true;
						IsAllComment = false;
						// コメント以外を発見した為，走査打切の返戻
						return false;
					}
				);
				if(IsAllComment) {
					Attach.Placement.Side = CommentPlacement::Before;
					PendingList.push_back({ TokenStart, NodeParent, std::move(Attach), TextEnd, IsLeading });
					// 包装内本文の除去範囲
					DeleteRanges.push_back({ TokenStart, TextEnd });
					// 最終出力時に包装内へ戻すコメント本文だけを分離した事の終了
					return;
				}
			}
			// 選択子コメントは規則前へ移し，CSS / SCSS の空白の意味と数値・単位の境界を保持
			if(Rules.IsWhitespaceSemantic) {
				if(!ts_node_is_null(NodeGrandparent) && Src.Is(NodeParent, Role::DimensionNumber)) {
					// 数と単位の間のコメントは値を含む節点の後へ移し，除いた跡で字句が融合すれば区切記法を置く（整形仕様 6.1, 6.6）
					DeleteRanges.push_back({ TokenStart, TextEnd });
					// CSS 数値内コメントの保留情報（値を含む節点の末尾コメントとして紐付ける）
					Pending Entry;
					Entry.CommentStart = TokenStart;
					Entry.CommentEnd = TextEnd;
					Entry.ShouldNormalizeLeading = IsLeading;
					Entry.Anchor = NodeGrandparent;
					Attach.Placement.Side = CommentPlacement::After;
					// 復元情報の保留
					Entry.Comment = std::move(Attach);
					PendingList.push_back(std::move(Entry));
					// 値を含む節点の末尾コメントとして紐付けた後の終了
					return;
				}
				// 選択子の並びの祖先の位置の次（無ければ 0，規則は其の親）
				size_t SelectorsNext = 0;
				for(size_t Up = Ancestors.size(); Up--;) {
					if(Src.Is(Ancestors[Up], Role::SelectorList)) {
						SelectorsNext = Up + 1;
						break;
					}
					if(Src.Is(Ancestors[Up], Role::ScopeBody)) break;
				}
				if(const TSNode Rule = SelectorsNext > 1 ? Ancestors[SelectorsNext - 2] : TSNode{}; !ts_node_is_null(Rule)) {
					DeleteRanges.push_back({ TokenStart, TextEnd, false, Reading.SelectorCommentActsAsSpace });
					// 選択子コメントの保留情報
					Pending Entry;
					Entry.CommentStart = TokenStart;
					Entry.CommentEnd = TextEnd;
					Entry.ShouldNormalizeLeading = IsLeading;
					Entry.Anchor = Rule;
					Attach.Placement.Side = CommentPlacement::Before;
					// 復元情報の保留
					Entry.Comment = std::move(Attach);
					PendingList.push_back(std::move(Entry));
					// 規則の前の先行コメントとして紐付けた後の終了
					return;
				}
			}
			// Swift 範囲は片側空白の誤読を避け式末尾へ，前後の字句へ密着する CSS 値の中の物は宣言の末尾へ紐付
			// 空白の無い字句の間へ空白と共に差し込むと字句の並びが変わり，値の字句を錨にすると区切を置いた出力を読み直す時に錨を失う（整形仕様 6.6）
			TSNode Anchor = IsInline && Src.Is(NodeParent, Role::SpacingSensitiveRange) ? NodeParent : TSNode{};
			// 前後の字句へ密着するか，識別子に接して出力を読み直すと値の字句が組み変わり得る物
			const bool IsGlued = TokenStart && !std::isspace(static_cast<unsigned char>(SelfData[TokenStart - 1])) && TextEnd < SelfSize &&
			!std::isspace(static_cast<unsigned char>(SelfData[TextEnd])) ||
			IsNamedNode(PrevImmediate) && Src.Is(PrevImmediate, Role::Identifier) ||
			IsNamedNode(NextImmediate) && Src.Is(NextImmediate, Role::Identifier);
			for(size_t Up = Ancestors.size(); IsInline && Rules.IsWhitespaceSemantic && IsGlued && ts_node_is_null(Anchor) && Up--;) {
				if(Src.Is(Ancestors[Up], Role::TypedDeclaration)) Anchor = Ancestors[Up];
				else if(Src.Is(Ancestors[Up], Role::ScopeBody)) break;
			}
			if(!ts_node_is_null(Anchor)) Attach.Placement.Side = CommentPlacement::After;
			// 行内：直接兄弟優先で解決し，見付からなければ後段の汎用代替経路（祖先兄弟 → ルート）でアンカー化
			if(IsInline && ts_node_is_null(Anchor)) {
				// 区切り・閉じ前は直前兄弟の説明として末尾紐付（空実引数内は被呼出側へ）
				// 解析用に補った文の区切も，出力で其処へ `;` を置き（`;` を省く記法を除く），同じ行の前の字句の後の改行で読みが変わらなければ区切りと同じに扱う
				uint32_t Next = TextEnd;
				while(Next < SelfSize && (SelfData[Next] == ' ' || SelfData[Next] == '\t')) ++Next;
				const bool IsBeforeInsertedBreak = !Rules.CanOmitTrailingSemicolon && !IsAtLineStart && Next < SelfSize &&
				std::binary_search(Src.GetInsertedBreaks().begin(), Src.GetInsertedBreaks().end(), Next) &&
				!Src.BreaksReadingAt(TextEdit::SkipSpLeft(Self, TokenStart, Reading), Next);
				if(Next < SelfSize && (IsListSeparatorChar(SelfData[Next]) || IsCloseBracketChar(SelfData[Next]) || IsBeforeInsertedBreak)) {
					// 区切りが後の兄弟其の物（空の文 `;` 等）なら其の先行
					if(!ts_node_is_null(NextImmediate) && Src.Start(NextImmediate) == Next) {
						Anchor = NextImmediate;
						Attach.Placement.Side = CommentPlacement::Before;
					} else if(!ts_node_is_null(PrevImmediate) && !(IsAtLineStart && IsCloseBracketChar(SelfData[Next]))) {
						// 親の中に前の兄弟が在れば其の末尾（間の無名字句 `label:` の `:` 等の数を控え，其の字句の後へ置く，行頭で閉じ字句の前の物は其の閉じの説明の為に除く）
						Anchor = PrevImmediate;
						Attach.Placement.TokensBefore = UnnamedAfterPrev;
					} else if(
						// 空の括弧（実引数の無い呼出）の中は，括弧が外れても残る様に祖先から遡った括弧の直前の被呼出側の末尾
						!ts_node_is_null(NodeParent) && ts_node_is_null(PrevImmediate) && ts_node_is_null(NextImmediate) &&
						IsParenthesized(Src.View(NodeParent)) && !ts_node_is_null(OuterPrev) &&
						TextEdit::SkipSpLeft(Self, Src.Start(NodeParent), Reading) == Src.End(OuterPrev)
					) Anchor = OuterPrev;
					// 其の他は祖先から遡ると前の枝・文へ移る為，親の中の区切り・閉じの字句の前（其の前の親の名前無の字句の数を控える）
					else if(!ts_node_is_null(NodeParent)) {
						Anchor = NodeParent;
						Attach.Placement.Side = CommentPlacement::Inside;
						Attach.Placement.TokensBefore = UnnamedBefore;
					}
				}
				if(ts_node_is_null(Anchor)) {
					Anchor = NextImmediate;
					// 直後の兄弟を説明する `g(/* 個数 */ 3)` 等は先行紐付し，配置で錨の直前の行内へ置く（行末へ移さない）
					if(!ts_node_is_null(Anchor)) Attach.Placement.Side = CommentPlacement::Before;
					else Anchor = PrevImmediate;
				}
			} else if(IsLeading) {
				// 直接後兄弟→祖先側の順に先行錨を探索（Ruby の節前・空本体の閉じ前は本体末尾）
				const bool IsBeforeKeywordClause = Reading.HasKeywordClosedBlocks && !ts_node_is_null(NextImmediate) && (
					Src.Is(NextImmediate, Role::ClauseHeading) ||
					Src.Is(NextImmediate, Role::BodyTriggerWord) && !ts_node_named_child_count(NextImmediate)
				);
				// PHP の空の代替節のコメントは次の枝へ移さず，見出後へ独立配置
				const TSNode ColonBody = ts_node_is_null(PrevImmediate) || !Src.Is(PrevImmediate, Role::IfFollowingBranch) ?
				PrevImmediate :
				LastChildOf(PrevImmediate, LastScan);
				const bool IsInEmptyColonBody =
				!ts_node_is_null(ColonBody) && Src.Is(ColonBody, Role::ColonBody) && !ts_node_named_child_count(ColonBody);
				// 直後が閉じ字句なら，後の節の先行でなく塊の末尾として紐付
				Anchor = IsBeforeKeywordClause || IsInEmptyColonBody ||
				!ts_node_is_null(Following) && !IsNamedNode(Following) && Src.Is(Following, Role::CloserToken) ? TSNode{} : NextImmediate;
				// Go の最上位の宣言との間に空行を挟む通常コメントは，godoc の文書・cgo の前文として取り込まれない様，前の兄弟の後の独立した行へ置く（整形仕様 7.6）
				if(
					Reading.HasCommentAbsorbingDeclarations && Attach.Role == CommentRole::Presentation && !ts_node_is_null(Anchor) &&
					!ts_node_is_null(PrevImmediate) && Src.AbsorbsGoComment(Anchor) && Src.Is(ts_node_parent(Anchor), Role::SourceFileNode)
				) {
					// 空行を挟むか（改行の後に空白だけの行が続くか）
					bool HasBlankLine = false;
					for(uint32_t Pos = TextEnd; Pos < Src.Start(Anchor) && !HasBlankLine; ++Pos) {
						if(SelfData[Pos] != '\n') continue;
						uint32_t Next = Pos + 1;
						while(Next < Src.Start(Anchor) && (SelfData[Next] == ' ' || SelfData[Next] == '\t' || SelfData[Next] == '\r')) ++Next;
						HasBlankLine = Next < Src.Start(Anchor) && SelfData[Next] == '\n';
					}
					if(HasBlankLine) {
						Anchor = PrevImmediate;
						Attach.Placement.Side = CommentPlacement::After;
					}
				}
				if(ts_node_is_null(Anchor)) {
					// 祖先側も含め構文木の後続字句で末尾判定し，Ruby の次節も閉じ扱い
					bool IsScopeEnd = IsBeforeKeywordClause || IsInEmptyColonBody || (
						!ts_node_is_null(Following) && ts_node_is_named(Following) ?
						Reading.HasKeywordClosedBlocks && Src.Is(Following, Role::ClauseHeading) :
						Src.Is(Following, Role::CloserToken)
					);
					// 手前の兄弟が見出の部品（条件・名前・仮引数等）かの判定
					// 本体の節点を持たない構文（Ruby の `if`~`end`）はコメントが見出の部品の兄弟に為り，塊の末尾でなくコメント自身が本体に当たる
					// スコープ末尾は直前兄弟後の独立行へ固定（見出との間に開きが在れば空塊扱い）
					if(IsScopeEnd && !ts_node_is_null(Following) && !ts_node_is_null(PrevImmediate) && EndsWithEmptyKeywordBody(PrevImmediate)) {
						// 語だけの `then` / `do` の後の空の本体の物は，後に続く境界（閉じの語か後続の節）へ結び付ける（整形仕様 7.1, 7.2）
						Anchor = Following;
						Attach.Placement.Side = CommentPlacement::Before;
						Attach.Placement.IsEmptyBody = true;
						IsScopeEnd = false;
					} else if(
						(IsScopeEnd || !ts_node_is_null(NodeParent) && Src.Is(NodeParent, Role::SequentialStatements)) &&
						!ts_node_is_null(PrevImmediate) && !IsAfterOpener &&
						(IsOpenBracketChar(SelfData[Src.Start(NodeParent)]) || !Src.IsHeaderField(NodeParent, PrevImmediate))
					) {
						// 塊の閉じの後から始まる節の語（Go の裸の塊・Kotlin の本体の容器は節の節点に為らない為，語で見分ける）
						const uint32_t AfterCloser = ts_node_is_null(Following) ? SelfSize : TextEdit::SkipSpRight(Self, Src.End(Following), Reading);
						const bool IsBeforeSectionWord = AfterCloser < SelfSize &&
						Src.Is(ts_node_descendant_for_byte_range(Src.GetRoot(), AfterCloser, AfterCloser + 1), Role::SectionKeyword);
						// 後続節が親の後に続くか
						const bool HasParentClause =
						IsBeforeSectionWord || !ts_node_is_null(ParentNext) && Src.Is(ParentNext, Role::FollowingBodyClause);
						const TSNode NextClause = HasParentClause ? ParentNext : OuterNext;
						// 後続節の直前のコメントは，原文の字下げに依らず其の節へ所属させる（節の見出の深さに置く，整形仕様 7.6，畳んで空に為る節は其の節の中）
						const TSNode EmptyClause = EmptyClauseOwner();
						const bool IsEmptyClause = !ts_node_is_null(EmptyClause);
						const bool IsClauseComment =
						IsEmptyClause || IsBeforeSectionWord || !ts_node_is_null(NextClause) && Src.Is(NextClause, Role::FollowingBodyClause);
						// 畳んで空に為る節の物は，後に続く境界（閉じの語か後続の節）へ結び付ける（整形仕様 7.1, 7.2）
						Anchor = IsEmptyClause && !ts_node_is_null(Following) ?
						Following :
						IsEmptyClause ? EmptyClause : IsClauseComment && !HasParentClause ? BlockOwner() : PrevImmediate;
						Attach.Placement.IsEmptyBody = IsEmptyClause;
						Attach.Placement.Side = IsEmptyClause ? CommentPlacement::Before : CommentPlacement::After;
						IsScopeEnd = false;
						// 直前の節の最後の文の後へ配置し，文が無ければ見出後へ独立配置
						if(!IsClauseComment) while(Src.Is(Anchor, Role::ClauseHeading) || Src.Is(Anchor, Role::BodyEndingClause)) {
							const TSNode Last = LastChildOf(Anchor, LastScan);
							// 本体の文の無い節の見出での停止（ネストのラベル `a: b: for(…)` はインデントを戻す行の外の本体迄降りる）
							if(
								ts_node_is_null(Last) || ts_node_eq(Last, Src.FieldChild(Anchor, ChildRole::Value)) || Src.Is(Last, Role::ClauseHeading) &&
								!(Src.Is(Anchor, Role::LabeledStatement) && IsNamedNode(Last) && Src.Is(Last, Role::LabeledStatement))
							) break;
							Anchor = Last;
						}
					}
					// コメントだけの塊の物は，其の塊を所有する構文を錨の候補とする（以下で閉じの字句又は所有構文へ結び付ける）
					if(IsScopeEnd && !ts_node_is_null(NodeParent)) {
						Anchor = BlockOwner();
						// 空の本体・括弧の物は其の終端境界（閉じの字句）へ結び付け，其の閉じの在る行の前へ置く（整形仕様 7.1, 7.2, 7.6）
						// 文書を付けれる定義の波括弧の本体の物は定義の先行コメントとし，定義の文書へ同じ回で統合する（整形仕様 7.4）
						if(Src.Is(Following, Role::CloserToken) && !(IsAfterOpener && Src.Is(Anchor, Role::DocumentableDeclaration))) {
							Anchor = Following;
						}
						// 閉じの字句と波括弧で本体を開く構文は畳むと其の行に残る為に行の前へ置き，開き括弧の無い構文は見出の後の独立した行へ置く
						Attach.Placement.Side = !ts_node_eq(Anchor, Following) && (Src.Is(Anchor, Role::ClauseHeading) || !IsAfterOpener) ?
						CommentPlacement::After :
						CommentPlacement::Before;
						// 畳む本体の中のコメントの印（配置の段が錨の頭でなく畳んだ本体の行を基準にする）
						Attach.Placement.IsEmptyBody = true;
					}
					if(ts_node_is_null(Anchor)) Anchor = NextOrOuter;
				}
				// 前後の錨が無ければ根の末尾のコメントにする（直前の兄弟へ付けると先行コメントとして先頭へ出るか行内へ入る為）
				if(ts_node_is_null(Anchor)) {
					Anchor = Src.GetRoot();
					// 根への先行紐付は先頭へ出る為に末尾紐付へ替える
					if(Attach.Placement.Side == CommentPlacement::Before) Attach.Placement.Side = CommentPlacement::After;
				}
			} else if(!ts_node_is_null(NextImmediate) && IsAfterToken && (ts_node_is_null(PrevImmediate) || IsAfterCloser)) {
				// 見出行末は後の作用対象へ先行紐付し，構文編集後も配置を保持
				Anchor = NextOrOuter;
				Attach.Placement.Side = CommentPlacement::Before;
			} else {
				// 空塊・空の Ruby begin は所有構文末尾へ固定し，前の文への移動を防止
				if(
					!ts_node_is_null(NodeParent) && (
						Src.Is(NodeParent, Role::BracedStatementBlock) && ts_node_is_null(LastChildOf(NodeParent, LastScan)) ||
						IsAfterToken && ts_node_is_null(PrevImmediate) && ts_node_is_null(NextImmediate) &&
						!IsOpenBracketChar(SelfData[Src.Start(NodeParent)]) && !Src.Is(NodeParent, Role::IterationSection)
					)
				) Anchor = BlockOwner();
				// 閉じ後は塊又は直持ち構文へ末尾紐付（同親の後続節が在れば直前兄弟側に保持）
				if(ts_node_is_null(Anchor) && IsAfterCloser && ts_node_is_null(NextImmediate)) Anchor = NodeParent;
				// 末尾錨は直前兄弟を優先し，制御見出と本体の間だけ直後兄弟にも退避
				if(ts_node_is_null(Anchor)) Anchor = PrevOrOuter;
				// 空括弧は同行の直前の名前・捕捉へ紐付（兄弟無は現錨を保持して除去時だけ再紐付）
				if(!ts_node_is_null(Anchor)) if(
					const std::string_view Text = Src.View(Anchor);
					IsParenthesized(Text) && Text.find_first_not_of(" \t\n", 1) == Text.size() - 1
				) if(
					const TSNode Before = Src.SiblingBeforeImmediate(Anchor);
					!ts_node_is_null(Before) &&
					!std::memchr(SelfData + ts_node_end_byte(Before), '\n', TokenStart - std::min(TokenStart, ts_node_end_byte(Before)))
				) Anchor = Before;
				// 空括弧の開き後は同じ行から始まる所有文へ末尾紐付し，文の並びの外へは遡らない
				if(!ts_node_is_null(Anchor) && !ts_node_is_null(NodeParent)) if(
					const uint32_t Opener = TextEdit::SkipCharsLeftBounded(Self, TokenStart, 0, " \t"), AnchorEnd = ts_node_end_byte(Anchor);
					Opener && IsOpenBracketChar(SelfData[Opener - 1]) && AnchorEnd < Opener && ts_node_is_null(ParentPrev)
				) {
					const uint32_t LineStart = TextEdit::LineStartOf(Self, Opener);
					// 親が文の並び又は根に届く迄の祖先位置による遡行
					size_t OwnerAt = Ancestors.size() - 1;
					bool IsStatement = false;
					while(OwnerAt) {
						const TSNode Up = Ancestors[OwnerAt - 1];
						IsStatement = OwnerAt == 1 || Src.Is(Up, Role::SequentialStatements);
						if(IsStatement || ts_node_start_byte(Up) < LineStart) break;
						--OwnerAt;
					}
					if(
						const TSNode Owner = Ancestors[OwnerAt];
						IsStatement && ts_node_start_byte(Owner) >= LineStart && AnchorEnd <= ts_node_start_byte(Owner)
					) Anchor = Owner;
				}
				if(ts_node_is_null(Anchor)) Anchor = NextOrOuter;
				// 制御構文の同行末尾錨は本体へ降り，包装後に閉じ波括弧へ移る事を防止
				if(!ts_node_is_null(Anchor)) {
					// A..B 間に改行が無ければ同一行 O(|B-A|) の局所走査で充分（LineOf 全体走査を回避）
					const auto SameLineAs = [&Self](const uint32_t Pos1, const uint32_t Pos2) -> bool {
						// 二位置の間の原文内の区間
						const uint32_t Low = std::min(Pos1, Pos2), High = std::max(Pos1, Pos2);
						const uint32_t Lim = std::min<uint32_t>(High, static_cast<uint32_t>(Self.size()));
						// 同行判定（区間内に改行を含まない）で真の返戻
						return Lim <= Low || !std::memchr(Self.data() + Low, '\n', Lim - Low);
					};
					while(true) {
						if(!Src.Is(Anchor, Role::Control)) break;
						// コメントより前で同行の最後の名前付子を線形探索（直下の閉じ後は構文末尾の儘）
						TSNode Next {};
						ForEachChild(
							Anchor,
							[&](const TSNode Child) -> bool {
								// 錨の非コメント子の探索でコメントを読み飛ばす事の返戻
								if(Src.Is(Child, Role::Comment)) return true;
								const uint32_t ChildEnd = ts_node_end_byte(Child);
								// 字句より後の子で打ち切る事の返戻
								if(ChildEnd > TokenStart) return false;
								if(!IsNamedNode(Child) && Src.Is(Child, Role::ClosingBraceToken)) Next = TSNode{};
								else if(ts_node_is_named(Child) && SameLineAs(ChildEnd, TokenStart)) Next = Child;
								// 子孫探索結果（発見有無）の返戻
								return true;
							}
						);
						if(ts_node_is_null(Next)) break;
						Anchor = Next;
					}
				}
			}
			// 直接兄弟で未発見なら祖先側の兄弟へ退避（整形への影響を防ぐ為，コメント本文は必ず原稿から除去）
			if(ts_node_is_null(Anchor)) Anchor = NextOrOuter;
			if(ts_node_is_null(Anchor)) Anchor = PrevOrOuter;
			// 錨が無ければ根へ末尾紐付し，コメントだけの原稿も本文に残さない
			if(ts_node_is_null(Anchor)) Anchor = Src.GetRoot();
			// 錨との間の無名字句の数を控え，先行コメントを其の字句より前に置いて字句との順序を保持
			if(
				Attach.Placement.Side == CommentPlacement::Before && !Rules.IsWhitespaceSemantic && !ts_node_is_null(NextImmediate) &&
				ts_node_eq(Anchor, NextImmediate)
			) {
				// 文法が子に出さず間隙にだけ残る隠れた字句（節点表では無名の字句の節点にする）も数える
				Attach.Placement.TokensBefore =
				static_cast<uint16_t>(std::min<uint32_t>(UnnamedToNext + HiddenToNext, std::numeric_limits<uint16_t>::max()));
			}
			// 先行錨だけの透過包装の解包
			const TSNode RootAnchor = Src.GetRoot();
			// 閉じ字句の無い文の並び・末尾改行を含む節点は最後の文へ紐付し，閉じ行への移動を防止
			while(
				Attach.Placement.Side != CommentPlacement::Before && Attach.Placement.Side != CommentPlacement::Inside && !IsAfterCloser &&
				!ts_node_is_null(Anchor) && !ts_node_eq(Anchor, RootAnchor)
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
			// 包装より前のコメントに対する降下先と，降りて越える包装の無名字句 (`{`) の数の再利用（配置の段は波括弧が残れば其の前へ置く）
			const bool IsBeforeAnchor =
			Attach.Placement.Side == CommentPlacement::Before && !ts_node_is_null(Anchor) && TokenStart < ts_node_start_byte(Anchor);
			LastScan.DescentPath.clear();
			uint16_t Crossed = 0;
			while(Attach.Placement.Side == CommentPlacement::Before && !ts_node_is_null(Anchor) && !ts_node_eq(Anchor, RootAnchor)) {
				if(IsBeforeAnchor) if(
					const std::unordered_map<const void *, std::pair<TSNode, uint16_t>>::const_iterator Hit = LastScan.Descent.find(Anchor.id);
					Hit != LastScan.Descent.end()
				) {
					// 既知の降下先再利用
					Anchor = Hit->second.first;
					Crossed = static_cast<uint16_t>(std::min<uint32_t>(Crossed + Hit->second.second, std::numeric_limits<uint16_t>::max()));
					break;
				}
				// 透過包装でない錨での停止（丸括弧は外れれば再紐付が中身へ寄せ，残れば前のコメントを括弧の前に保つ為に降ろさない）
				if(!Src.Is(Anchor, Role::TransparentWrapper)) break;
				const uint32_t InnerCount = ts_node_named_child_count(Anchor);
				// 整形中に消えるコメントを錨にせず，最初の名前付非コメント子へ降下
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
				// 複数子の包装はコメントが先頭子より前なら降下し，既存文書の見落し・重複生成を防止
				if(ts_node_is_null(InnerChild) || InnerCount != 1 && TokenStart >= ts_node_start_byte(InnerChild)) break;
				if(IsBeforeAnchor) {
					LastScan.DescentPath.emplace_back(Anchor, Crossed);
					// 包装の中の子の前の無名字句の計数（兄弟を順に辿るカーソルで，数の上限で飽和）
					HeldCursor Cursor(Anchor);
					for(
						bool IsWalking = ts_tree_cursor_goto_first_child(&Cursor);
						IsWalking && ts_node_start_byte(ts_tree_cursor_current_node(&Cursor)) < ts_node_start_byte(InnerChild);
						IsWalking = ts_tree_cursor_goto_next_sibling(&Cursor)
					) if(!ts_node_is_named(ts_tree_cursor_current_node(&Cursor)) && Crossed != std::numeric_limits<uint16_t>::max()) ++Crossed;
				}
				Anchor = InnerChild;
			}
			for(const auto &[Visited, Before] : LastScan.DescentPath) {
				LastScan.Descent[Visited.id] = { Anchor, static_cast<uint16_t>(Crossed - Before) };
			}
			Attach.Placement.TokensBefore =
			static_cast<uint16_t>(std::min<uint32_t>(Attach.Placement.TokensBefore + Crossed, std::numeric_limits<uint16_t>::max()));
			if(!ts_node_is_null(Anchor)) {
				// 分離したコメントの原文の位置と錨を控え，投影した節点へ対応付ける
				Pending Entry;
				Entry.CommentStart = TokenStart;
				Entry.CommentEnd = TextEnd;
				// 句点での行の分割は独立コメントだけ（行末・文中のコメントは分けると後の行が独立した行に為る）
				Entry.ShouldNormalizeLeading = IsLeading;
				// 本文を正規化せず群へも纏めない指示コメントか
				Entry.IsVerbatim = Attach.Role == CommentRole::Directive;
				Entry.Anchor = Anchor;
				// ルート代替経路の場合は末尾コメントとして紐付（先行コメントだとファイル先頭に来て仕舞う）
				if(ts_node_eq(Anchor, RootAnchor) && Attach.Placement.Side == CommentPlacement::Before) {
					Attach.Placement.Side = CommentPlacement::After;
				}
				// 復元情報の保留
				Entry.Comment = std::move(Attach);
				PendingList.push_back(std::move(Entry));
			}
			if(IsInline) {
				// 行内コメントの除去範囲
				uint32_t DelStartInline = TokenStart, DelEndInline = TextEnd;
				// 隣接１空白を仮消費し，単語融合は削除適用時に補完（CSS の子孫空白は保つ為，前後の両方が空白の時だけ１つ消費する）
				if(!Rules.IsWhitespaceSemantic) {
					if(DelEndInline < Self.size() && Self[DelEndInline] == ' ') ++DelEndInline;
					else if(DelStartInline && Self[DelStartInline - 1] == ' ') --DelStartInline;
				} else if(DelEndInline < Self.size() && Self[DelEndInline] == ' ' && DelStartInline && Self[DelStartInline - 1] == ' ') {
					++DelEndInline;
				}
				// 囲みコメントの中の改行で文を終える言語（JS / TS・Go・Swift）以外は其の改行を改行と読まない為，跡に改行を残さない（残すと次の整形の錨が行頭へ移り，冪等を崩す）
				// JS/TS の文の中（文の並びの直下でない所）のコメントも，前後が改行で読みの変わる字句でなければ解析器も処理系も其処で文を終えない為，跡に改行を残さない（残すと構造化しない部分整形の範囲で次の整形の錨が行頭へ移る）
				const bool IsJoinable = Reading.CommentLineSeparatorsEndStatements ?
				!Src.Is(NodeParent, Role::StatementOrMemberSequence) &&
				!Src.BreaksReadingAt(TextEdit::SkipSpLeft(Self, TokenStart, Reading), TextEdit::SkipSpRight(Self, TextEnd, Reading)) :
				!Reading.HasLineSensitiveBoundary;
				DeleteRanges.push_back({ DelStartInline, DelEndInline, IsJoinable });
				// 終了
				return;
			}
			// 前置区切り（全角スペースを含み，独立行の物は行頭の字下げ）と独立行の終端改行を含む範囲の除去
			DeleteRanges.push_back({ TextEdit::SeparatorBefore(Self, TokenStart, Reading).first, DelEnd });
		};
		// 逐語内容のコメント形は保護し，コードとなる JS / TS のテンプレート置換内だけ抽出
		// マクロの定義は解析器が本体を字句の並びとしか読まず，中のコメントに錨と為る文が無い為に丸ごと逐語で保つ
		const auto HoldsLiteralComments = [&Src, &Rules](const TSNode Node) -> bool {
			// 内容其の物としてコメントを抽出しない節点かの返戻（Ruby の区切りを持つリテラルも内容が意味を持つ）
			return Src.Is(Node, Role::LiteralInterior) && !Src.Is(Node, Role::InterpolatedLiteral) || Src.StringizesArguments(Node) ||
			Src.Is(Node, Role::PreprocessorNode) && (!Src.Is(Node, Role::PreprocessorBranch) || !ts_node_named_child_count(Node)) ||
			Rules.IsWhitespaceSemantic && Src.IsCssUrlArguments(Node) || Src.Is(Node, Role::CommentKeepingHost) ||
			Src.Is(Node, Role::DelimitedLiteral);
		};
		const TSNode RootNode = Src.GetRoot();
		const auto WalkSubtree = [&Src, &ProcessComment, &HoldsLiteralComments, RootNode](
			const TSNode SubRoot,
			std::vector<Pending> &PendingOut,
			std::vector<DeleteRange> &DelRanges,
			std::vector<uint32_t> &Separated,
			LineScan &LastScan
		) -> void {
			// コメント毎の親探索を避けるカーソルと祖先列の同期
			HeldCursor Cursor(SubRoot);
			// 根から現在の節点の親迄の祖先（末尾が親）
			std::vector<TSNode> Path { RootNode, SubRoot };
			if(ts_tree_cursor_goto_first_child(&Cursor)) for(bool IsDone = false; !IsDone;) {
				const TSNode Node = ts_tree_cursor_current_node(&Cursor);
				// コメント判定と内容判定で共有する型名の単一取得
				const bool IsComment = Src.Is(Node, Role::Comment);
				if(IsComment) ProcessComment(Node, Path, PendingOut, DelRanges, Separated, LastScan);
				// コメント又はコメント様内容を持つ節点内部の走査・抽出除外
				if(!IsComment && !HoldsLiteralComments(Node) && ts_tree_cursor_goto_first_child(&Cursor)) {
					Path.push_back(Node);
					continue;
				}
				while(!ts_tree_cursor_goto_next_sibling(&Cursor)) {
					if(!ts_tree_cursor_goto_parent(&Cursor)) {
						IsDone = true;
						break;
					}
					Path.pop_back();
				}
			}
		};
		// ルート直下の子の振分：コメントは直接処理し，其れ以外は部分木走査へ委譲する（逐次枝／並列枝で共用）
		const auto DispatchTop = [&](
			const TSNode Child,
			std::vector<Pending> &PendingOut,
			std::vector<DeleteRange> &DelOut,
			std::vector<uint32_t> &SeparatedOut,
			LineScan &LastScan
		) -> void {
			// 最上位子のコメント又は部分木への振分
			if(Src.Is(Child, Role::Comment)) ProcessComment(Child, { &RootNode, 1 }, PendingOut, DelOut, SeparatedOut, LastScan);
			else if(!HoldsLiteralComments(Child)) WalkSubtree(Child, PendingOut, DelOut, SeparatedOut, LastScan);
		};
		// 同親の前後の子を保存し，不在時は祖先側を継承（親の後の最初の非コメント子も保持）
		struct ParentFrame {
			TSNode Node;
			TSNode Prev;
			TSNode Next;
			TSNode OuterPrev;
			TSNode OuterNext;
			TSNode Following; // 親の後に続く最初のコメントでない子（字句を含み，親の兄弟に無ければ祖先から遡った物）
		};
		// 未走査親のスタック
		std::vector<ParentFrame> Parents { { RootNode, TSNode{}, TSNode{}, TSNode{}, TSNode{}, TSNode{} } };
		std::vector<TSNode> Kids, NextOf, FollowOf;
		std::vector<uint8_t> Anchorless;
		std::vector<uint32_t> Unnamed, Hidden;
		std::vector<size_t> NextAt;
		// 深い構文木での呼出スタック消費を避ける親の明示スタック走査
		while(!Parents.empty() && !ParentsWithComments.empty()) {
			// 未処理の親節点の深さ優先走査
			const ParentFrame Top = Parents.back();
			Parents.pop_back();
			Kids.clear();
			ForEachChild(
				Top.Node,
				[&Kids](const TSNode Child) -> void {
					// 最上位節点の子の収集
					Kids.push_back(Child);
				}
			);
			// 錨に為らない子（原稿へ残さないコメント・空白化する Python の行継続・構造化で省く最上位の空の式文 `;`）の印と，各子の前の名前無の字句の数
			Anchorless.assign(Kids.size(), 0);
			Unnamed.assign(Kids.size() + 1, 0);
			// 各子の前の間隙に残る隠れた字句の数の累積（誤りを含む親の読み飛ばした字句は数えず，最初の子の前は使わない）
			Hidden.assign(Kids.size() + 1, 0);
			const bool CountsHidden = !ts_node_has_error(Top.Node);
			TSNode PrevCode {};
			for(size_t Idx = 0; Idx < Kids.size(); ++Idx) {
				const TSNode Child = Kids[Idx];
				const bool IsComment = Src.Is(Child, Role::Comment), IsNamed = ts_node_is_named(Child);
				Anchorless[Idx] =
				IsComment || Src.Is(Child, Role::LineContinuation) || ts_node_eq(Top.Node, RootNode) && Src.IsOmittedSemicolon(Child, PrevCode);
				if(!IsComment && IsNamed) PrevCode = Child;
				Unnamed[Idx + 1] = Unnamed[Idx] + !IsNamed;
				// 前の子との間の空白で区切った字句の数
				uint32_t Gap = 0;
				if(CountsHidden && Idx) {
					bool IsInToken = false;
					for(uint32_t At = ts_node_end_byte(Kids[Idx - 1]), End = ts_node_start_byte(Child); At < End;) {
						if(const size_t Width = TextEdit::IntertokenSpaceAt(Src, At, Reading); Width && At + Width <= End) {
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
			// 後続子を逆順に記録する兄弟関係索引の構築
			NextOf.assign(Kids.size(), TSNode{});
			NextAt.assign(Kids.size(), Kids.size());
			FollowOf.assign(Kids.size(), Top.Following);
			for(size_t Idx = Kids.size(); Idx > 1; --Idx) {
				const TSNode After = Kids[Idx - 1];
				const bool IsNext = !Anchorless[Idx - 1] && ts_node_is_named(After);
				NextOf[Idx - 2] = IsNext ? After : NextOf[Idx - 1];
				NextAt[Idx - 2] = IsNext ? Idx - 1 : NextAt[Idx - 1];
				FollowOf[Idx - 2] = Anchorless[Idx - 1] ? FollowOf[Idx - 1] : After;
			}
			// 名前無の字句の数の上限での飽和
			const auto Saturate = [](const uint32_t Count) -> uint16_t {
				// 飽和した数の返戻
				return static_cast<uint16_t>(std::min<uint32_t>(Count, std::numeric_limits<uint16_t>::max()));
			};
			// 直前の名前付非コメント子（と其の位置）と，其の後に開き波括弧・字句が在ったか，前に閉じ波括弧が在ったか
			TSNode Prev {};
			size_t PrevAt = 0;
			bool IsAfterOpener = false, IsAfterCloser = false, IsAfterToken = false;
			for(size_t Idx = 0; Idx < Kids.size(); ++Idx) {
				const TSNode Child = Kids[Idx], Next = NextOf[Idx];
				if(!ts_node_is_named(Child)) {
					IsAfterOpener = IsAfterOpener || Src.Is(Child, Role::OpenBrace);
					IsAfterCloser = IsAfterCloser || Src.Is(Child, Role::ClosingBraceToken);
					IsAfterToken = true;
					continue;
				}
				if(const bool IsComment = Src.Is(Child, Role::Comment); IsComment) {
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
						Saturate(Hidden[std::min(NextAt[Idx] + 1, Kids.size())] - Hidden[Idx + 1])
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
		// 根の子に対するコメントの収集
		const uint32_t RootChildCount = ts_node_child_count(RootNode);
		// 原文順に統合する紐付候補
		std::vector<Pending> PendingList;
		std::vector<DeleteRange> DeleteRanges;
		std::vector<uint32_t> &Separated = Result.SeparatedStarts;
		const bool IsRootComment = Src.Is(RootNode, Role::Comment);
		// 並列化の固定費を避ける小さな木とコメント単体の逐次処理
		if(const size_t NumThreads = IsRootComment ? 1 : Parallel::DecideThreads(RootChildCount, 8); NumThreads < 2) {
			// 逐次走査の再利用状態
			LineScan LastScan;
			// Root 自身が Comment の時も処理対象への包含
			if(IsRootComment) ProcessComment(RootNode, {}, PendingList, DeleteRanges, Separated, LastScan);
			ForEachChild(
				RootNode,
				[&](const TSNode Child) -> void {
					// 最上位子の走査処理への振分
					DispatchTop(Child, PendingList, DeleteRanges, Separated, LastScan);
				}
			);
		} else {
			// `ts_node_child(RootNode, Idx)` の累積 O(N²) を避ける為，１度の TSTreeCursor でベクタに収集
			std::vector<TSNode> RootKids;
			RootKids.reserve(RootChildCount);
			ForEachChild(
				RootNode,
				[&](const TSNode Child) -> void {
					// 根直下の子の原文順収集
					RootKids.push_back(Child);
				}
			);
			std::vector<std::vector<Pending>> ThreadPending(NumThreads);
			std::vector<std::vector<DeleteRange>> ThreadDeletes(NumThreads);
			std::vector<std::vector<uint32_t>> ThreadSeparated(NumThreads);
			// 走脈別の再利用状態
			std::vector<LineScan> ThreadScans(NumThreads);
			// 遅延して作る控えの並列区間内での構築を避ける暖機（`mutable` の控えは同期を持たない）
			Src.GetRoot();
			Src.GetCommentSpans();
			// コメント収集中の同期を不要にする走脈別書込先の分離
			Parallel::ForChunks(
				RootChildCount,
				NumThreads,
				[&](const size_t ChunkStart, const size_t ChunkEnd, const size_t Tid) -> void {
					// 担当区間の実行
					for(size_t Idx = ChunkStart; Idx < ChunkEnd; ++Idx) {
						DispatchTop(RootKids[Idx], ThreadPending[Tid], ThreadDeletes[Tid], ThreadSeparated[Tid], ThreadScans[Tid]);
					}
				}
			);
			size_t TotalPending = 0, TotalDeletes = 0;
			for(const std::vector<Pending> &Pendings : ThreadPending) TotalPending += Pendings.size();
			for(const std::vector<DeleteRange> &Deletes : ThreadDeletes) TotalDeletes += Deletes.size();
			PendingList.reserve(TotalPending);
			// 削除範囲の統合領域予約
			DeleteRanges.reserve(TotalDeletes);
			for(std::vector<Pending> &Local : ThreadPending) {
				PendingList.insert(PendingList.end(), std::make_move_iterator(Local.begin()), std::make_move_iterator(Local.end()));
			}
			for(const std::vector<DeleteRange> &Local : ThreadDeletes) DeleteRanges.insert(DeleteRanges.end(), Local.begin(), Local.end());
			for(const std::vector<uint32_t> &Local : ThreadSeparated) Separated.insert(Separated.end(), Local.begin(), Local.end());
		}
		// 解析用の入力から退避したコメントの記録（整形仕様 4.2, 7.6，所属だけを決め，置けるかと配置はコメント経路が決める）
		// 退避した位置を含む字面を保つ区画の中の物は其の区画の中，行末の物は位置で終わる構文の後，其の他は位置で始まる構文の前へ紐付ける
		for(const EscapedComment &Escaped : Src.EscapedComments) {
			Pending Entry;
			Entry.CommentStart = Entry.CommentEnd = Escaped.Position;
			Entry.ShouldNormalizeLeading = Escaped.Kind == CommentKind::OwnLine;
			Entry.Comment.Text = Escaped.Text;
			Entry.Comment.Kind = Escaped.Kind;
			Entry.Comment.Notation = CommentRepository::NotationOf(Rules.Notation, Escaped.Text, false);
			CommentRepository::AttachRules(Rules.Notation, Entry.Comment);
			Entry.Comment.Placement.Side = Escaped.Kind == CommentKind::Trailing ? CommentPlacement::After : CommentPlacement::Before;
			const bool IsAfter = Escaped.Kind == CommentKind::Trailing && Escaped.Position;
			const uint32_t Probe = IsAfter ? Escaped.Position - 1 : Escaped.Position;
			TSNode Anchor = ts_node_named_descendant_for_byte_range(RootNode, Probe, Probe);
			// 位置で始まる（行末の物は位置で終わる）最も外の構文
			for(TSNode Up = ts_node_parent(Anchor); !ts_node_is_null(Up) && !ts_node_eq(Up, RootNode); Up = ts_node_parent(Up)) {
				if((IsAfter ? ts_node_end_byte(Up) : ts_node_start_byte(Up)) != (IsAfter ? Escaped.Position : ts_node_start_byte(Anchor))) {
					break;
				}
				Anchor = Up;
			}
			// 字面を保つ区画の中の物は最も外の区画の中，条件の塊を成す前処理指令の見出の行の中の物は指令の語の中
			for(TSNode Up = Anchor; !ts_node_is_null(Up); Up = ts_node_parent(Up)) {
				// 解析の為に印へ置き換えた連結指定はコードの為，其の前後
				if(ParseAdapter::IsLinkageGuard(Src, Up)) {
					Anchor = Up;
					Entry.Comment.Placement.Side = Escaped.Kind == CommentKind::Trailing ? CommentPlacement::After : CommentPlacement::Before;
					break;
				}
				if(HoldsLiteralComments(Up)) {
					Anchor = Up;
					Entry.Comment.Placement.Side = CommentPlacement::Inside;
					continue;
				}
				// 区画の外へ出た所での打切
				if(Entry.Comment.Placement.Side == CommentPlacement::Inside) break;
				if(!Src.Is(Up, Role::PreprocessorBranch)) continue;
				// 条件の塊を成す指令の見出の行の中の物（見出の外なら打切）
				TSNode Header = Src.FieldChild(Up, ChildRole::Condition);
				if(ts_node_is_null(Header)) Header = Src.FieldChild(Up, ChildRole::Name);
				if(!ts_node_is_null(Header) && Escaped.Position <= Src.End(Header)) {
					Anchor = ts_node_child(Up, 0);
					Entry.Comment.Placement.Side = CommentPlacement::Inside;
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
			[](const Pending &Lhs, const Pending &Rhs) -> bool {
				// コメントの開始位置順の比較結果の返戻
				return Lhs.CommentStart < Rhs.CommentStart;
			}
		);
		// 原文の形の字面の記号と区切空白を除いた本文への分解（記号は記法が表す，整形仕様 3.2）
		for(Pending &Entry : PendingList) CommentRepository::Decompose(Entry.Comment);
		// 行毎の記号を持たない埋込文書の，出力する行コメントと同じ論理行毎の記録への分割（整形仕様 7.3, 7.6）
		// 開始記号・終端記号の行の残りも記録とし，囲みの開始記号・終端記号だけの行と同じく，空の行は構造として保つ時だけ配置の段が残す
		if(Rules.Notation.IsAvailable(CommentNotation::RubyEmbeddedDocument)) {
			const std::string &Source = Src;
			// 論理行の本文と原文の物理行の範囲
			struct LineSpan {
				std::string_view Text; // 論理行の本文
				uint32_t Start; // 物理行の始まり
				uint32_t End; // 物理行の終わり
			};
			std::vector<Pending> Split;
			Split.reserve(PendingList.size());
			// 後の記録が同じ錨で空白だけを挟んで続く埋込文書か（境の開始記号・終端記号の行の空の記録は，続く埋込文書の論理行でない為に作らない）
			std::vector<uint8_t> JoinsNext(PendingList.size(), 0);
			for(size_t Index = 0; Index + 1 < PendingList.size(); ++Index) {
				const Pending &Left = PendingList[Index], &Right = PendingList[Index + 1];
				JoinsNext[Index] = Left.Comment.Notation == CommentNotation::RubyEmbeddedDocument &&
				Right.Comment.Notation == CommentNotation::RubyEmbeddedDocument && ts_node_eq(Left.Anchor, Right.Anchor) &&
				CommentNormalizer::IsCommentGap(std::string_view(Source.data() + Left.CommentEnd, Right.CommentStart - Left.CommentEnd), true);
			}
			for(size_t Index = 0; Index < PendingList.size(); ++Index) {
				Pending &Entry = PendingList[Index];
				if(Entry.Comment.Notation != CommentNotation::RubyEmbeddedDocument) {
					Split.push_back(std::move(Entry));
					continue;
				}
				const std::string Body = std::move(Entry.Comment.Text);
				Entry.Comment.Format.SeparateLogicalLines = false;
				// 分解が物理行毎に作った論理行
				std::vector<LineSpan> Lines;
				for(size_t Cursor = 0, LineStart = Entry.CommentStart; Cursor <= Body.size();) {
					const size_t End = std::min(Body.find('\n', Cursor), Body.size());
					const size_t LineEnd = std::min(Source.find('\n', LineStart), static_cast<size_t>(Entry.CommentEnd));
					Lines.push_back(
						{ std::string_view(Body).substr(Cursor, End - Cursor), static_cast<uint32_t>(LineStart), static_cast<uint32_t>(LineEnd) }
					);
					Cursor = End + 1;
					LineStart = LineEnd + 1;
				}
				// 続く埋込文書との境の空の開始記号・終端記号の行を除く範囲
				size_t First = 0, Last = Lines.size();
				if(Index && JoinsNext[Index - 1] && Lines[First].Text.empty()) ++First;
				if(JoinsNext[Index] && Last > First && Lines[Last - 1].Text.empty()) --Last;
				for(size_t Idx = First; Idx < Last; ++Idx) {
					Pending &Piece = Split.emplace_back(Entry);
					Piece.Comment.Text = Lines[Idx].Text;
					Piece.CommentStart = Lines[Idx].Start;
					Piece.CommentEnd = Lines[Idx].End;
				}
			}
			PendingList = std::move(Split);
		}
		// 原文で同じ錨の同じ側に空白だけを挟んで続く独立行の説明のコメントの並び（文書の位置が確定した後に群を纏める記法で群へ分け，正規化の段が群毎に正規化する，整形仕様 3.3, 3.4, 7.3）
		CommentGroupId NextGroup = 0;
		for(size_t Begin = 0; Begin < PendingList.size();) {
			// 本文を正規化しない指示コメントの読飛（整形仕様 7.5）
			if(PendingList[Begin].IsVerbatim) {
				++Begin;
				continue;
			}
			size_t End = Begin + 1;
			while(End < PendingList.size()) {
				const Pending &Previous = PendingList[End - 1], &Next = PendingList[End];
				// 同じ錨の同じ側のコメントは出力で空行を除いて並ぶ為に空行を越えて纏め，別の錨・側のコメントは出力の配置で空行を挟み得る為に纏めない
				// 群を纏める記法は文書の位置で変わる為，此処では並びだけを記録し，文書の位置が確定した後に分ける（整形仕様 3.3）
				if(
					!Previous.ShouldNormalizeLeading || !Next.ShouldNormalizeLeading || Next.IsVerbatim ||
					Next.CommentStart < Previous.CommentEnd || !ts_node_eq(Previous.Anchor, Next.Anchor) ||
					Previous.Comment.Placement.Side != Next.Comment.Placement.Side || !CommentNormalizer::IsCommentGap(
						std::string_view(Src.data() + Previous.CommentEnd, Next.CommentStart - Previous.CommentEnd),
						true
					)
				) break;
				++End;
			}
			for(size_t Idx = Begin; Idx < End; ++Idx) PendingList[Idx].Comment.Format.Group = NextGroup;
			++NextGroup;
			Begin = End;
		}
		// 囲みの文書と同じ文書形式の行形式の文書（C / C++ の Doxygen の `///` `//!`，C# と Swift の `///`）の，同じ錨の連続する行の１つの文書の記録への統合（記法は入力の儘とし，出力記法は整形仕様 7.6 で選ぶ）
		std::vector<Pending> Merged;
		Merged.reserve(PendingList.size());
		for(size_t Idx = 0; Idx < PendingList.size();) {
			if(!CommentRepository::IsBlockLineDocument(PendingList[Idx].Comment)) {
				Merged.push_back(std::move(PendingList[Idx++]));
				continue;
			}
			// 同じ錨の同じ側で改行１つだけを挟んで続く同じ記法の文書の行
			size_t End = Idx + 1;
			while(
				End < PendingList.size() && CommentRepository::IsBlockLineDocument(PendingList[End].Comment) &&
				PendingList[End].Comment.Notation == PendingList[Idx].Comment.Notation &&
				ts_node_eq(PendingList[End].Anchor, PendingList[Idx].Anchor) &&
				PendingList[End].Comment.Placement.Side == PendingList[Idx].Comment.Placement.Side && CommentNormalizer::CanJoinComments(
					PendingList[End - 1].Comment.Notation,
					PendingList[End].Comment.Notation,
					std::string_view(Src.data() + PendingList[End - 1].CommentEnd, PendingList[End].CommentStart - PendingList[End - 1].CommentEnd)
				)
			) ++End;
			Pending Doc = std::move(PendingList[Idx]);
			for(size_t Next = Idx + 1; Next < End; ++Next) Doc.Comment.Text.append("\n").append(PendingList[Next].Comment.Text);
			Doc.Comment.Format.SeparateLogicalLines = Doc.Comment.Text.find('\n') != std::string::npos;
			Doc.CommentEnd = PendingList[End - 1].CommentEnd;
			Merged.push_back(std::move(Doc));
			Idx = End;
		}
		PendingList = std::move(Merged);
		// 統合後の原文順による出力通し番号の確定
		for(size_t Idx = 0; Idx < PendingList.size(); ++Idx) PendingList[Idx].Comment.OriginalOrder = Idx;
		// 除く範囲の昇順整列（投影が二分探索で引く）
		std::sort(
			DeleteRanges.begin(),
			DeleteRanges.end(),
			[](const DeleteRange &Lhs, const DeleteRange &Rhs) -> bool {
				// 削除範囲の開始位置順の比較結果の返戻
				return Lhs.Begin < Rhs.Begin;
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
		// 除いた跡へ補う区切（字句の融合境界と文の境界を保つ物）の確定と，投影が原文を読む時に除く範囲の記録
		Result.Separations.reserve(DeleteRanges.size());
		for(const DeleteRange &Range : DeleteRanges) {
			// 除く範囲の隣接字
			const char Before = Range.Begin ? Src.data()[Range.Begin - 1] : '\0';
			const char After = Range.End < Src.size() ? Src.data()[Range.End] : '\0';
			// コメント除去で演算子・単位を融合させず（括弧・区切りに接する所は融合しない為に補わない），CSS の加減算と値の境界を保持
			const bool IsTokenBoundary = IsIdentifierChar(Before) && IsIdentifierChar(After) ||
			Before && After && !std::isspace(static_cast<unsigned char>(Before)) && !std::isspace(static_cast<unsigned char>(After)) &&
			!IsOpenBracketChar(Before) && !IsCloseBracketChar(Before) && !IsListSeparatorChar(Before) && !IsOpenBracketChar(After) &&
			!IsCloseBracketChar(After) && !IsListSeparatorChar(After);
			const std::string_view Removed(Src.data() + Range.Begin, Range.End - Range.Begin);
			// 改行を含むコメントの自動セミコロン挿入の境界の保持
			const bool IsLineBoundary = !Rules.IsWhitespaceSemantic && !Range.IsJoinable && (
				Removed.find('\n') != std::string_view::npos || Reading.CommentLineSeparatorsEndStatements && std::ranges::any_of(
					Reading.CommentLineBreaks,
					[&Removed](const std::string_view Break) -> bool {
						// 行の区切の字面を含むかの返戻
						return Removed.find(Break) != std::string_view::npos;
					}
				)
			);
			// CSS / SCSS のコメントは空白に為らず，空白では同じ字句列を表せない為，融合する境界だけへ固定の区切記法を置く（整形仕様 6.1, 6.6）
			// Sass が空白と読む選択子の中のコメントは，前後が密着して居れば空白１つで表す
			const bool IsGlued =
			Before && After && !std::isspace(static_cast<unsigned char>(Before)) && !std::isspace(static_cast<unsigned char>(After));
			const bool IsNotation = Rules.IsWhitespaceSemantic && !Range.IsSpaceLike && IsGlued &&
			SyntaxRules::FusesDimensionTokens(Before, std::string_view(Src).substr(Range.End));
			const std::string_view Separator = IsNotation ?
			Rules.LexicalSeparator :
			Rules.IsWhitespaceSemantic ? Range.IsSpaceLike && IsGlued ? " " : "" : IsLineBoundary ? "\n" : IsTokenBoundary ? " " : "";
			Result.Separations.push_back({ Range.Begin, Range.End, Separator, IsNotation });
		}
		// 錨の節点への紐付
		for(Pending &Entry : PendingList) {
			// 原文の行末の物の前置区切りに含む水平タブの数と，検証の投影だけで照合する区切りの形（整形仕様 7.1, 8.1）
			if(Entry.Comment.Kind == CommentKind::Trailing && Entry.CommentStart < Entry.CommentEnd) {
				if(const auto [At, Tabs] = TextEdit::SeparatorBefore(Src, Entry.CommentStart, Src.GetReading()); At && Src[At - 1] != '\n') {
					Entry.Comment.Format.LeadingTabCount = Tabs;
					if(IsVerification) {
						const std::string Marker = CommentNormalizer::StackMarkerOf(Entry.Comment.Notation);
						std::string_view Rest = Entry.Comment.Text;
						// 最初の記号と積んだ記号を，各々の直前の区切りと共に原文順で記録する
						Result.LeadingSeparators.push_back(
							{
								std::string(CommentNormalizer::FirstLogicalLine(Rest, Marker)),
								Entry.CommentStart - At == Tabs && Tabs || Entry.CommentStart - At == 1 && Src[At] == ' ' ? Tabs : SIZE_MAX
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
										std::string(CommentNormalizer::FirstLogicalLine(Rest, Marker)),
										Count && Count == Gap.size() || Gap == " " ? Count : SIZE_MAX
									}
								);
								Next = Rest.find(Marker);
							}
						}
					}
				}
			}
			Result.Attach(Entry.Anchor, Entry.Comment.Placement.Side == CommentPlacement::Before, std::move(Entry.Comment));
		}
		// 分離の結果の返戻
		return Result;
	}

	/**
	 * 分離されずに残るコメント節点の計数関数
	 * 経路の分離（整形仕様 1.2）の検証に用いる（錨へ紐付けたコメントは投影が読み飛ばす為に数えない）
	 * 原文の儘写す領域（文字列・生テキスト・`url()` の引数・文字列化するマクロの実引数）の中の物は，コメントでなくソースの為に数えない
	 * @param Src 解析結果
	 * @param Separated コメントの分離の結果
	 * @param BodyEnd PHP の本体の終わり（其の後の地の文の中の物は，解析器がコメントと読んでも地の文の為に数えない）
	 * @return 分離の対象でありながら構文木へ残って居るコメント節点の数
	 */
	size_t SourceProjector::CountUnseparatedComments(
		const ::ParsedTree &Src,
		const CommentSeparation &Separated,
		const uint32_t BodyEnd
	) {
		const SyntaxRules &Rules = Src.GetRules();
		// HTML のコメントは文書内の位置と内容を保つソースであり，通常コメントとして分離しない事の返戻（整形仕様 4.4）
		if(Src.GetReading().HasCommentDocumentContent) return 0;
		// 数えたコメント節点
		size_t Found = 0;
		TSTreeCursor Cursor = ts_tree_cursor_new(Src.GetRoot());
		for(bool IsGoing = true; IsGoing;) {
			// 走査中の節点
			const TSNode Current = ts_tree_cursor_current_node(&Cursor);
			// 原文の儘写す領域か
			const bool IsVerbatimHost =
			Src.Is(Current, Role::SourceLeaf) || Src.Is(Current, Role::SpaceSkip) || Src.Is(Current, Role::PreprocessorNode) ||
			Src.IsCssUrlArguments(Current) || Src.StringizesArguments(Current);
			// CSS / SCSS の固定の区切記法 `/**/` は字句の境界を表すコードの一部で，分離の対象でない（整形仕様 6.1）
			if(
				Src.Is(Current, Role::Comment) && !(!Rules.LexicalSeparator.empty() && Src.View(Current) == Rules.LexicalSeparator) &&
				!Separated.IsSeparatedComment(Src, Current) && Src.Start(Current) < BodyEnd
			) ++Found;
			if(!IsVerbatimHost && ts_tree_cursor_goto_first_child(&Cursor)) continue;
			while(!ts_tree_cursor_goto_next_sibling(&Cursor)) if(!ts_tree_cursor_goto_parent(&Cursor)) {
				IsGoing = false;
				break;
			}
		}
		ts_tree_cursor_delete(&Cursor);
		// 残って居るコメント節点の数の返戻
		return Found;
	}

	/**
	 * 宣言子毎に名前を持つ宣言の部分かの判定関数（整形仕様 7.4, 8.1）
	 * @param Syntax 投影した構文
	 * @param Node 節点
	 * @return 宣言子の欄を持つ節点か変数の宣言子の項目なら true
	 */
	bool SourceProjector::IsDeclaratorPart(const SyntaxModel &Syntax, const NodeId Node) {
		// 宣言子の欄を持つか変数の宣言子の項目かの返戻
		return Syntax.HasField(Node, ChildRole::Declarator) || Syntax.Is(Node, Role::VariableDeclaratorItem);
	}

	/**
	 * 説明対象の識別の組立関数（整形仕様 7.4, 8.1）
	 * 説明対象を，所有する宣言の種類と名前の並び，対象の種類と名前，同じ親の中で同じ種類と名前を持つ先行する兄弟の数で表す
	 * 型・初期化子の字面と節点番号を識別に含めず，区切の正規化等の選択済の表現差で識別を変えない（整形仕様 8.1）
	 * 同じ親の中の順は親毎に子を一度だけ辿って数え，宣言子毎に名前を持つ宣言は全ての宣言子の名前で数える（宣言の分割・統合で順を変えない）
	 * 計算量：対象の数 D，対象の深さの和 H，対象の親の子の数の和 C，名前を探す部分木の大きさの和 N に対し O(D + H + C + N)
	 * @param Syntax 投影した構文
	 * @param Entries 説明対象と其の名前を表す節点（宣言子の中の文書は其の宣言子）の組の並び
	 * @return 組毎の説明対象の識別
	 */
	std::vector<std::string> SourceProjector::LabelTargets(
		const SyntaxModel &Syntax,
		const std::span<const std::pair<NodeId, NodeId>> Entries
	) {
		// 節点の名前（名前の欄，宣言子の最も内側の名前，本体の外で最初の名前・型名の字句，本体の外で最初の名前付の字句の順に探す）
		const auto NameOf = [&Syntax](const NodeId Node) -> std::string {
			// 宣言子からの名前の探索
			NodeId Named = Syntax.FieldChild(Node, ChildRole::Name), FirstLeaf = NoNode;
			for(NodeId Declarator = Syntax.FieldChild(Node, ChildRole::Declarator); Named == NoNode && Declarator != NoNode;) {
				Named = Syntax.FieldChild(Declarator, ChildRole::Name);
				const NodeId Inner = Syntax.FieldChild(Declarator, ChildRole::Declarator);
				if(Named == NoNode && Inner == NoNode) Named = Declarator;
				Declarator = Inner;
			}
			// 本体の外からの名前候補の探索
			for(std::vector<NodeId> Pending { Node }; Named == NoNode && !Pending.empty();) {
				const NodeId Part = Pending.back();
				Pending.pop_back();
				if(Part != Node && (Syntax.Is(Part, Role::NameToken) || Syntax.Is(Part, Role::TypeName))) Named = Part;
				else if(Part == Node || !Syntax.HasField(Part, ChildRole::Body)) {
					if(FirstLeaf == NoNode && Part != Node && Syntax.IsNamed(Part) && !Syntax.ChildCount(Part)) FirstLeaf = Part;
					const std::span<const NodeId> Children = Syntax.ChildrenOf(Part);
					Pending.insert(Pending.end(), Children.rbegin(), Children.rend());
				}
			}
			if(Named == NoNode) Named = FirstLeaf;
			// 名前の字面の返戻
			return Named == NoNode ? std::string() : Syntax.Text(Named);
		};
		// 種類と名前の表示（種類は入力・完成候補・正規化後の構文が同じ文法の定義から共有する節点種別で表し，正規化が作った節点も元の節点と同じ表示に為る）
		const auto LabelOf = [&Syntax](const NodeId Node, const std::string &Name) -> std::string {
			// 種別と名前の表示の返戻
			return std::to_string(reinterpret_cast<uintptr_t>(Syntax.GetNode(Node).Kind.Definition)) + ":" + Name;
		};
		// 兄弟を数える表示（宣言子毎に名前を持つ宣言は全ての宣言子の名前で表し，宣言の分割・統合で兄弟の順を変えない）
		const auto SiblingLabels = [&Syntax, &NameOf, &LabelOf](const NodeId Node) -> std::vector<std::string> {
			std::vector<std::string> Labels;
			for(const NodeId Child : Syntax.ChildrenOf(Node)) {
				if(IsDeclaratorPart(Syntax, Child)) Labels.push_back(LabelOf(Node, NameOf(Child)));
			}
			if(Labels.empty()) Labels.push_back(LabelOf(Node, NameOf(Node)));
			// 兄弟を数える表示の返戻
			return Labels;
		};
		// 組毎の対象の表示と，親毎の其の組の並び
		std::vector<std::string> Labels(Entries.size());
		std::vector<uint32_t> Ordinals(Entries.size(), 0);
		std::unordered_map<NodeId, std::vector<size_t>> ByParent;
		for(size_t Index = 0; Index < Entries.size(); ++Index) {
			const auto [Target, Named] = Entries[Index];
			Labels[Index] = LabelOf(Target, NameOf(Named));
			ByParent[Syntax.ParentOf(Target)].push_back(Index);
		}
		// 親毎に子を一度だけ辿り，同じ表示を持つ先行する兄弟の数を数える
		for(const std::pair<const NodeId, std::vector<size_t>> &Group : ByParent) {
			std::unordered_map<NodeId, std::vector<size_t>> ByTarget;
			for(const size_t Index : Group.second) ByTarget[Entries[Index].first].push_back(Index);
			std::unordered_map<std::string, uint32_t> Counts;
			for(const NodeId Child : Syntax.ChildrenOf(Group.first)) {
				if(!Syntax.IsNamed(Child)) continue;
				if(
					const std::unordered_map<NodeId, std::vector<size_t>>::const_iterator Found = ByTarget.find(Child);
					Found != ByTarget.end()
				) for(const size_t Index : Found->second) Ordinals[Index] = Counts[Labels[Index]];
				for(std::string &Label : SiblingLabels(Child)) ++Counts[std::move(Label)];
			}
		}
		// 所有する宣言（宣言子毎に名前を持つ宣言は其の宣言子）毎の表示（同じ所有者を持つ組で求め直さない）
		std::unordered_map<NodeId, std::string> Owners;
		std::vector<std::string> Descriptions;
		Descriptions.reserve(Entries.size());
		for(size_t Index = 0; Index < Entries.size(); ++Index) {
			// 根から対象の親迄の所有する宣言の表示（宣言子毎に名前を持つ宣言は対象を含む宣言子の名前で表し，宣言の分割・統合で所有者の表示を変えない）
			std::vector<std::string_view> Scope;
			for(NodeId Below = Entries[Index].first, Up = Syntax.ParentOf(Below); Up != NoNode; Below = Up, Up = Syntax.ParentOf(Up)) {
				if(!Syntax.Is(Up, Role::DocumentableDeclaration)) continue;
				const NodeId Named = IsDeclaratorPart(Syntax, Below) ? Below : Up;
				std::string &Owner = Owners[Named];
				if(Owner.empty()) Owner = LabelOf(Up, NameOf(Named));
				Scope.push_back(Owner);
			}
			std::string Description;
			for(size_t Depth = Scope.size(); Depth--;) Description.append(Scope[Depth]).push_back('/');
			Descriptions.push_back(Description.append(Labels[Index]).append("#").append(std::to_string(Ordinals[Index])));
		}
		// 組毎の説明対象の識別の返戻
		return Descriptions;
	}

	/**
	 * 文書の説明対象と本文の取得関数（整形仕様 7.4, 8.1）
	 * 同期・本文正規化済の期待コメントと最終字面の双方から同じ規則で集め，整形仕様 7.4 で認識した文書の説明対象と本文が保たれたかを検証側が比べる
	 * 宣言子の中に錨が在る文書は其の宣言子の名前で表し，宣言の分割で説明対象を取り違えない
	 * 整形仕様 7.4 の認識と同じ構文モデルで表す（指示子の適用先は投影出来ない候補からも集める為に解析木で表す，`CollectDirectives`）
	 * 計算量：記録の数 C，本文の長さの和 N と識別の組立 (`LabelTargets`) の量に対し O(C + N) と其の量
	 * @param Syntax 記録の錨を持つ構文
	 * @param Records 錨を持つ記録の並び
	 * @param Targets 記録毎の文書として扱う説明対象（文書でない記録は無し）
	 * @return 原文順の文書の説明対象と本文
	 */
	std::vector<ValidationContext::DocumentFact> SourceProjector::DescribeDocuments(
		const SyntaxModel &Syntax,
		const std::span<const CommentRecord> Records,
		const std::span<const NodeId> Targets
	) {
		// 文書毎の説明対象と其の名前を表す節点（宣言子の中に錨が在る文書は其の宣言子）
		std::vector<std::pair<NodeId, NodeId>> Entries;
		for(size_t Index = 0; Index < Targets.size(); ++Index) {
			const NodeId Target = Targets[Index];
			if(Target == NoNode) continue;
			NodeId Part = Records[Index].Anchor;
			while(Part != NoNode && Syntax.ParentOf(Part) != Target) Part = Syntax.ParentOf(Part);
			Entries.emplace_back(Target, Part != NoNode && IsDeclaratorPart(Syntax, Part) ? Part : Target);
		}
		std::vector<ValidationContext::DocumentFact> Descriptions;
		// 文書の無い並びの返戻
		if(Entries.empty()) return Descriptions;
		// 文書の識別と本文の組立
		std::vector<std::string> Labels = LabelTargets(Syntax, Entries);
		Descriptions.reserve(Labels.size());
		for(size_t Index = 0, Entry = 0; Index < Targets.size(); ++Index) if(Targets[Index] != NoNode) {
			Descriptions.push_back(
				{
					std::move(Labels[Entry++]),
					DocCommentSynchronizer::LogicalLines(Records[Index]),
					DocCommentSynchronizer::IsDedicatedDocument(CommentRepository::RulesOf(Records[Index]), Records[Index])
				}
			);
		}
		// 原文順の文書の説明対象と本文の返戻
		return Descriptions;
	}

	/**
	 * 最終字面のコメントの記録の投影関数（整形仕様 3.3 の `ValidatedOutput`，8.4）
	 * 検証が読んだ最終字面のコメントを，逐語区画に残る物も含め原文順に記法・役割・本文へ分けて記録する（錨を持たない）
	 * 改行１つだけを挟んで続く群を纏める記法の同じ独立行の説明のコメントは，正規化と同じ群として静的検査がコード例を判定する（整形仕様 7.3）
	 * @param Rules 文法の記法の定義
	 * @param Text 補修前の最終字面
	 * @param CommentRanges コメントの原文順の範囲
	 * @param DirectiveStarts 指示コメントの補修前の開始位置（昇順）
	 * @param Definitions 指示コメントの登録定義（開始位置と同じ順，本文の区切空白の読みを入力と揃える）
	 * @param Reading 文法の読取規則
	 * @return 最終字面のコメントの記録（コメントの範囲と同じ順）
	 */
	CommentRepository SourceProjector::ProjectOutputComments(
		const NotationRules &Rules,
		const std::string_view Text,
		const std::span<const std::pair<uint32_t, uint32_t>> CommentRanges,
		const std::span<const uint32_t> DirectiveStarts,
		const std::span<const DirectiveDefinitionId> Definitions,
		const ReadingRules &Reading
	) {
		// 最終字面の記録先と指示子の位置
		CommentRepository Comments;
		CommentGroupId Group = 0;
		std::span<const uint32_t>::iterator Directive = DirectiveStarts.begin();
		// コメント範囲毎の記録の生成
		for(size_t Index = 0; Index < CommentRanges.size(); ++Index) {
			const auto [Start, End] = CommentRanges[Index];
			const std::string_view Raw = Text.substr(Start, End - Start);
			CommentRecord Record { std::string(Raw), { CommentPlacement::Before } };
			Record.Notation = CommentRepository::NotationOf(Rules, Raw, !Start);
			// 標準の指示コメントの位置の物を指示の役割とし，其の登録定義を付ける
			while(Directive != DirectiveStarts.end() && *Directive < Start) ++Directive;
			if(Directive != DirectiveStarts.end() && *Directive == Start) {
				Record.Role = CommentRole::Directive;
				Record.Directive.emplace().Definition = Definitions[static_cast<size_t>(Directive - DirectiveStarts.begin())];
			}
			CommentRepository::AttachRules(Rules, Record);
			CommentRepository::Decompose(Record);
			// 前の独立行の説明のコメントへ改行１つだけを挟んで続く，群を纏める記法の同じ独立行の説明のコメントは同じ群（最終字面の文書の記法は文書の位置に在り，入力で同じ錨の同じ側の空行を越えて纏めた群は，出力では空行を挟まずに並ぶ為に此の並びに当たる）
			const CommentRecord *const Previous = Index ? &Comments.GetRecords().back() : nullptr;
			const bool Joins = Previous && Previous->Role == CommentRole::Presentation && Record.Role == CommentRole::Presentation &&
			!TextEdit::IsAfterCode(Text, CommentRanges[Index - 1].first, Reading) && !TextEdit::IsAfterCode(Text, Start, Reading) &&
			CommentNormalizer::CanJoinComments(
				CommentRepository::GroupNotationOf(*Previous, true),
				CommentRepository::GroupNotationOf(Record, true),
				Text.substr(CommentRanges[Index - 1].second, Start - CommentRanges[Index - 1].second)
			);
			Record.Format.Group = Joins ? Group : ++Group;
			Comments.Add(std::move(Record));
		}
		// 原文順の記録の返戻
		return Comments;
	}
} // namespace Core
