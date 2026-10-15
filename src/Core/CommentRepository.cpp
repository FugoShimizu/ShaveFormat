#include "CommentRepository.hpp"
#include "SyntaxModel.hpp"
#include "SyntaxRole.hpp"
#include "../Profile/ReadingRules.hpp"
#include "../Util/CharClass.hpp"
#include "../Util/CommentNormalizer.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include "../Util/ParseAdapter.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 名前迄を記号に含むディレクティブ記法かの判定関数（整形仕様 3.2, 4.5）
	 * @param Notation 記法
	 * @return 処理系が名前を定めるディレクティブ記法の値なら true
	 */
	bool CommentRepository::IsNamedDirective(const CommentNotation Notation) {
		// 名前付のディレクティブ記法の値かの返戻
		return Notation >= CommentNotation::GoBuildLine && Notation <= CommentNotation::GccgoExternLine;
	}

	/**
	 * 記号と本文の間の空白を揃えるディレクティブ記法かの判定関数（整形仕様 3.2）
	 * @param Notation 記法
	 * @return `//` と同じく記号の後をスペース１つに揃える記法なら true（位置の指示の後の空白はファイル名の一部）
	 */
	bool CommentRepository::IsSpacedDirective(const CommentNotation Notation) {
		// 揃える記法かの返戻
		return IsNamedDirective(Notation) && Notation != CommentNotation::GoLineLine && Notation != CommentNotation::GoLineBlock;
	}

	/**
	 * 囲みの記法かの判定関数
	 * @param Notation 記法
	 * @return 終端記号で閉じる囲みの記法なら true
	 */
	bool CommentRepository::IsBlockNotation(const CommentNotation Notation) {
		// 囲みの記法かの返戻
		return Notation == CommentNotation::Block || Notation == CommentNotation::DoubleStarBlock ||
		Notation == CommentNotation::StarBangBlock || Notation == CommentNotation::GoLineBlock;
	}

	/**
	 * 記録の記法の定義の取得関数
	 * @param Record 記法の定義を記録したコメントの記録
	 * @return 共有する言語の記法の定義
	 * @throws FormatLimitExceeded 記法の定義を記録して居ない場合（フォーマッターの不具合）
	 */
	const NotationRules &CommentRepository::GetRulesOf(const CommentRecord &Record) {
		// 記法の定義を持たない記録はフォーマッターの不具合とする
		if(!Record.Format.Rules) throw FormatLimitExceeded("a comment record has no notation rules", FailureReason::FormatterDefect);
		// 記法の定義の返戻
		return *Record.Format.Rules;
	}

	/**
	 * 前の区切空白を除いたテキストの取得関数（整形仕様 7.6）
	 * 半角スペース・全角スペース・タブの有無・個数・混在に依らず，行頭の空白の並びを区切空白とする
	 * 計算量：先頭の区切空白のバイト数 N に対し O(N + 1)
	 * @param Line 記号の後のテキスト
	 * @return 行頭の半角スペース・全角スペース・タブを除いたテキスト
	 */
	std::string_view CommentRepository::TrimLeadingSeparator(std::string_view Line) {
		// 全角スペースのテキスト
		constexpr std::string_view WideSpace = "\u3000";
		while(true) {
			if(Line.starts_with(' ') || Line.starts_with('\t')) Line.remove_prefix(1);
			else if(Line.starts_with(WideSpace)) Line.remove_prefix(WideSpace.size());
			// 区切空白を除いたテキストの返戻
			else return Line;
		}
	}

	/**
	 * 後の区切空白を除いたテキストの取得関数（整形仕様 7.6）
	 * 半角スペース・全角スペース・タブの有無・個数・混在に依らず，行末の空白の並びを区切空白とする
	 * 計算量：末尾の区切空白のバイト数 N に対し O(N + 1)
	 * @param Line 終端記号の前のテキスト
	 * @return 行末の半角スペース・全角スペース・タブを除いたテキスト
	 */
	std::string_view CommentRepository::TrimTrailingSeparator(std::string_view Line) {
		constexpr std::string_view WideSpace = "\u3000";
		while(true) {
			if(Line.ends_with(' ') || Line.ends_with('\t')) Line.remove_suffix(1);
			else if(Line.ends_with(WideSpace)) Line.remove_suffix(WideSpace.size());
			// 後置の区切空白を除いたテキストの返戻
			else return Line;
		}
	}

	/**
	 * 記法の開始記号の取得関数（整形仕様 3.2）
	 * @param Notation 記法
	 * @return 開始記号（Go のディレクティブ記法は名前迄を含む）
	 */
	std::string_view CommentRepository::GetOpenerOf(const CommentNotation Notation) {
		// コメント記法毎の開始記号の選択
		switch(Notation) {
		case CommentNotation::SlashLine:
			// 一般の行コメントの記号の返戻
			return "//";
		case CommentNotation::TripleSlashLine:
			// ３個のスラッシュの記号の返戻
			return "///";
		case CommentNotation::SlashBangLine:
			// `//!` の記号の返戻
			return "//!";
		case CommentNotation::HashLine:
			// `#` の記号の返戻
			return "#";
		case CommentNotation::HashBangLine:
			// シバンの記号の返戻
			return "#!";
		case CommentNotation::HashColonLine:
			// `#:` の記号の返戻
			return "#:";
		case CommentNotation::Block:
			// 一般の囲みの記号の返戻
			return "/*";
		case CommentNotation::DoubleStarBlock:
			// ドキュメントの囲みの記号の返戻
			return "/**";
		case CommentNotation::StarBangBlock:
			// `/*!` の記号の返戻
			return "/*!";
		case CommentNotation::RubyEmbeddedDocument:
			// 埋込ドキュメントの記号の返戻
			return "=begin";
		case CommentNotation::GoBuildLine:
			// ビルド制約の記号の返戻
			return "//go:build";
		case CommentNotation::GoDebugLine:
			// 実行時の設定の記号の返戻
			return "//go:debug";
		case CommentNotation::GoEmbedLine:
			// 埋込リソースの記号の返戻
			return "//go:embed";
		case CommentNotation::GoGenerateLine:
			// 生成命令の記号の返戻
			return "//go:generate";
		case CommentNotation::GoLinknameLine:
			// リンク名の記号の返戻
			return "//go:linkname";
		case CommentNotation::GoNoescapeLine:
			// エスケープ解析の指示の記号の返戻
			return "//go:noescape";
		case CommentNotation::GoNoinlineLine:
			// インライン化禁止の記号の返戻
			return "//go:noinline";
		case CommentNotation::GoNoraceLine:
			// 競合検出の除外の記号の返戻
			return "//go:norace";
		case CommentNotation::GoNosplitLine:
			// スタックの分割チェックの除外の記号の返戻
			return "//go:nosplit";
		case CommentNotation::GoUintptrescapesLine:
			// 整数の指す先のエスケープの記号の返戻
			return "//go:uintptrescapes";
		case CommentNotation::GoWasmexportLine:
			// WebAssembly のエクスポートの記号の返戻
			return "//go:wasmexport";
		case CommentNotation::GoWasmimportLine:
			// WebAssembly のインポートの記号の返戻
			return "//go:wasmimport";
		case CommentNotation::GoLineLine:
			// 行の位置の指示の記号の返戻
			return "//line";
		case CommentNotation::GoLineBlock:
			// 囲みの位置の指示の記号の返戻
			return "/*line";
		case CommentNotation::CgoExportLine:
			// エクスポートのディレクティブの記号の返戻
			return "//export";
		case CommentNotation::GccgoExternLine:
			break;
		}
		// 外部名の指示の記号の返戻
		return "//extern";
	}

	/**
	 * 記法の終端記号の取得関数（整形仕様 3.2）
	 * @param Notation 記法
	 * @return 終端記号（行末迄を本文とする記法は空）
	 */
	std::string_view CommentRepository::GetCloserOf(const CommentNotation Notation) {
		// 埋込ドキュメントの行頭の終端記号の返戻
		if(Notation == CommentNotation::RubyEmbeddedDocument) return "=end";
		// 囲みの終端記号と，行の記法の空の返戻
		return IsBlockNotation(Notation) ? "*/" : "";
	}

	/**
	 * 実行時設定の値を登録値と照合する関数
	 * 大小文字を問わない値の判定を，登録形の識別と適用範囲の選択で共有する
	 * 計算量：値の長さ L と登録値数 V に対し O((L + 1)(V + 1))
	 * @param Val 読んだ値
	 * @param Vals 小文字で登録した有効な値
	 * @return 登録値と一致するなら `true`
	 */
	bool CommentRepository::IsMagicCommentValue(const std::string_view Val, const std::span<const std::string_view> Vals) {
		// 登録値との一致の返戻
		return std::ranges::any_of(
			Vals,
			[Val](const std::string_view Registered) -> bool {
				// 大小文字を問わない値の照合結果の返戻
				return Val.size() == Registered.size() && std::ranges::equal(
					Val,
					Registered,
					[](const unsigned char Left, const unsigned char Right) -> bool {
						// 小文字へ揃えた字の比較結果の返戻
						return std::tolower(Left) == Right;
					}
				);
			}
		);
	}

	/**
	 * 実行時設定の組の並びの取得関数（整形仕様 4.5）
	 * 処理系と同じく，囲みのマーカーが在れば其の中の区切った組を，無ければ注記全体を１つの組として読み，全体が組に為らない注記は組の並びとしない
	 * 名前は空白・引用符・区切で終え，値は値を囲む引用符の内側か，引用符・組の区切・空白の前迄とする
	 * 計算量：コメントの長さ L に対し O(L)
	 * @param Forms 行コメントの登録形の判定条件
	 * @param Text コメント全文
	 * @param MarkerAt 開始記号を除いた本文の囲みの開始位置（無ければ探し，`std::string_view::npos` なら囲無）
	 * @param Work 組の読取の作業計数（無ければ計上しない）
	 * @return 組の並び（組として読めない注記と実行時設定を持たない言語は無）
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws FormatLimitExceeded 作業計数が上限を超える場合
	 */
	std::optional<std::vector<MagicCommentPair>> CommentRepository::MagicCommentPairsOf(
		const RegisteredLineForms &Forms,
		std::string_view Text,
		const std::optional<size_t> MarkerAt,
		StructureWorkAccount *const Work
	) {
		// 組の区切と引用符を定めない（実行時設定を持たない）言語の返戻
		if(Forms.MagicCommentSeparators.empty() || Forms.MagicCommentQuotes.empty()) return std::nullopt;
		Text.remove_prefix(std::min(GetOpenerOf(CommentNotation::HashLine).size(), Text.size()));
		// 囲みのマーカーの内側への組の並びの限定
		const std::string_view Marker = Forms.MagicCommentMarker;
		const size_t Open = MarkerAt ? *MarkerAt : Text.find(Marker);
		const bool IsModeline = Open != std::string_view::npos;
		if(IsModeline) {
			const size_t Close = Text.find(Marker, Open + Marker.size());
			// 閉じのマーカーの無い囲みの返戻
			if(Close == std::string_view::npos) return std::nullopt;
			Text = Text.substr(Open + Marker.size(), Close - Open - Marker.size());
		}
		const std::string_view Quotes = Forms.MagicCommentQuotes, Separators = Forms.MagicCommentSeparators;
		const std::string_view Delimiters = Forms.MagicCommentDelimiters, Joiners = Forms.MagicCommentNameJoiners;
		// 名前を終える字か
		const auto IsNameEnd = [&](const unsigned char Char) -> bool {
			// 空白・引用符・区切かの返戻
			return std::isspace(Char) || Quotes.find(Char) != std::string_view::npos || Separators.find(Char) != std::string_view::npos ||
			Delimiters.find(Char) != std::string_view::npos;
		};
		std::vector<MagicCommentPair> Pairs;
		size_t Pos = 0;
		// 空白の読飛
		const auto SkipSpaces = [&Text, &Pos]() -> void {
			// 組の間の空白の読飛
			while(Pos < Text.size() && std::isspace(static_cast<unsigned char>(Text[Pos]))) ++Pos;
			// 終了
			return;
		};
		// 設定名と値の組の読取
		while(Pos < Text.size()) {
			while(Pos < Text.size() && IsNameEnd(Text[Pos])) ++Pos;
			const size_t NameStart = Pos;
			while(Pos < Text.size() && !IsNameEnd(Text[Pos])) ++Pos;
			const size_t NameEnd = Pos;
			SkipSpaces();
			if(Pos == Text.size()) break;
			if(Separators.find(Text[Pos]) == std::string_view::npos) {
				if(!IsModeline) {
					if(Work) Work->Charge(Pos + 1);
					// 注記全体が組に為らない事の返戻
					return std::nullopt;
				}
				continue;
			}
			++Pos;
			SkipSpaces();
			if(Pos == Text.size()) break;
			// 引用符の有無に応じた値の範囲の取得
			std::string_view Val;
			if(Text[Pos] == Quotes.front()) {
				// 引用符の内側（バックスラッシュの次の字を値に含める）
				const size_t ValStart = ++Pos;
				while(Pos < Text.size() && Text[Pos] != Quotes.front()) Pos += Text[Pos] == '\\' ? 2 : 1;
				Pos = std::min(Pos, Text.size());
				Val = Text.substr(ValStart, Pos - ValStart);
				if(Pos < Text.size()) ++Pos;
			} else {
				const size_t ValStart = Pos;
				while(
					Pos < Text.size() && Text[Pos] != Quotes.front() && Delimiters.find(Text[Pos]) == std::string_view::npos &&
					!std::isspace(static_cast<unsigned char>(Text[Pos]))
				) ++Pos;
				Val = Text.substr(ValStart, Pos - ValStart);
			}
			if(IsModeline) {
				while(
					Pos < Text.size() &&
					(Delimiters.find(Text[Pos]) != std::string_view::npos || std::isspace(static_cast<unsigned char>(Text[Pos])))
				) ++Pos;
			} else {
				SkipSpaces();
				if(Pos < Text.size()) {
					if(Work) Work->Charge(Pos + 1);
					// 組の後に続く字を持つ注記の返戻
					return std::nullopt;
				}
			}
			// 大小文字と連結の字を揃えた名前
			std::string Name(Text.substr(NameStart, NameEnd - NameStart));
			ToLowerAscii(Name);
			for(char &Char : Name) if(Joiners.find(Char) != std::string_view::npos) Char = Joiners.back();
			Pairs.push_back({ std::move(Name), Val });
		}
		// 名前・値の読取に進んだバイトの計上
		if(Work) Work->Charge(Pos + 1);
		// 組の並びの返戻
		return Pairs;
	}

	/**
	 * エンコーディング宣言の開始位置の取得関数（整形仕様 4.5）
	 * 登録語と区切と名前を処理系毎の登録定義（登録語の大小文字，区切の前の空白，名前の字）で探す
	 * 計算量：本文長 L と登録語長 W に対し O((W + 1)(L + 1))
	 * @param Forms 行コメントの登録形の判定条件
	 * @param Text コメント全文
	 * @param ShouldFindLast 最後の登録語迄探すか（成立だけを見る時は最初の一致で終える）
	 * @param Work 読取の作業計数（無ければ計上しない）
	 * @return 条件に従う最初又は最後の登録語と名前の範囲（無ければ無）
	 * @throws FormatLimitExceeded 作業計数が上限を超える場合
	 */
	std::optional<CommentRepository::EncodingDecl> CommentRepository::FindEncodingDecl(
		const RegisteredLineForms &Forms,
		const std::string_view Text,
		const bool ShouldFindLast,
		StructureWorkAccount *const Work
	) {
		// エンコーディング宣言の登録語の取得
		const std::string_view Word = Forms.EncodingDeclWord;
		// 登録語を持たない言語の返戻
		if(Word.empty()) return std::nullopt;
		// 登録語と照合する字だけを小文字化し，照合用の本文コピーを持たない
		// 登録語の次の開始位置を探すローカル関数
		// @param Begin 探索の開始位置
		// @return 次の一致の位置，無ければ `std::string_view::npos`
		// @throws FormatLimitExceeded 読取の作業予算を超える場合
		const auto FindWord = [&](const size_t Begin) -> size_t {
			// 大小文字を区別する登録語の位置の返戻
			if(!Forms.IsEncodingWordCaseless) {
				const size_t Found = Text.find(Word, Begin);
				if(Work) Work->Charge((Found == std::string_view::npos ? Text.size() : Found) - Begin + 1);
				// 次の一致の位置の返戻
				return Found;
			}
			const std::string_view::const_iterator Found = std::search(
				Text.begin() + Begin,
				Text.end(),
				Word.begin(),
				Word.end(),
				[Work](const unsigned char Char, const char Expected) -> bool {
					if(Work) Work->Charge(1);
					// 従来の本文小文字化と同じ変換に依る一致の返戻
					return static_cast<char>(std::tolower(Char)) == Expected;
				}
			);
			// 登録語の開始位置の返戻
			return Found == Text.end() ? std::string_view::npos : static_cast<size_t>(Found - Text.begin());
		};
		std::optional<EncodingDecl> Last;
		for(size_t Found = FindWord(0); Found != std::string_view::npos; Found = FindWord(Found + 1)) {
			size_t Pos = Found + Word.size();
			if(Forms.CanAllowEncodingSeparatorSpace) while(Pos < Text.size() && std::isspace(static_cast<unsigned char>(Text[Pos]))) ++Pos;
			if(Pos == Text.size() || Forms.EncodingDeclSeparators.find(Text[Pos]) == std::string_view::npos) continue;
			do ++Pos;
			while(Pos < Text.size() && (Text[Pos] == ' ' || Text[Pos] == '\t'));
			// 登録語と区切の後に名前が続く事の照合
			if(Work) Work->Charge(Pos - Found + 1);
			if(
				Pos < Text.size() && (
					Forms.EncodingNameChars.empty() ?
					!std::isspace(static_cast<unsigned char>(Text[Pos])) :
					std::isalnum(static_cast<unsigned char>(Text[Pos])) || Forms.EncodingNameChars.find(Text[Pos]) != std::string_view::npos
				)
			) {
				size_t End = Pos;
				while(
					End < Text.size() && (
						Forms.EncodingNameChars.empty() ?
						!std::isspace(static_cast<unsigned char>(Text[End])) :
						std::isalnum(static_cast<unsigned char>(Text[End])) || Forms.EncodingNameChars.find(Text[End]) != std::string_view::npos
					)
				) ++End;
				if(Work) Work->Charge(End - Pos);
				Last = EncodingDecl{ Found, Pos, End };
				// 成立だけを見る照会への最初の一致の返戻
				if(!ShouldFindLast) return Last;
			}
		}
		// 最後の成立する登録語の位置の返戻
		return Last;
	}

	/**
	 * 登録定義に従うエンコーディング宣言の値の取得関数
	 * 計算量：本文長 B と登録語長 W に対し O((W + 1)(B + 1))
	 * @param Forms 行コメントの登録定義
	 * @param Text コメント全文
	 * @return 宣言の名前（組の並びなら最後の指定，其れ以外は最初の指定，無ければ空）
	 * @throws std::bad_alloc 実行時設定の組を確保出来ない場合
	 * @throws std::length_error 組の名前又は並びの長さが格納先の上限を超える場合
	 */
	std::string_view CommentRepository::GetEncodingName(const RegisteredLineForms &Forms, const std::string_view Text) {
		if(const std::optional<std::vector<MagicCommentPair>> Pairs = MagicCommentPairsOf(Forms, Text)) {
			std::string_view Name;
			for(const MagicCommentPair &Pair : *Pairs) {
				if(std::ranges::find(Forms.EncodingMagicCommentNames, Pair.Name) != Forms.EncodingMagicCommentNames.end()) Name = Pair.Val;
			}
			// 成立した組の並びの最後の指定の返戻
			return Name;
		}
		const std::optional<EncodingDecl> Found = FindEncodingDecl(Forms, Text, false);
		// 登録された名前の字の範囲の返戻
		return Found ? Text.substr(Found->NameStart, Found->NameEnd - Found->NameStart) : std::string_view();
	}

	/**
	 * 原文のエンコーディング宣言の位置と値の範囲の読取関数
	 * 行の成立条件・シバン・区切・名前の範囲を入力検査と投影へ同じ形で渡す
	 * 計算量：頭部の長さ B と固定の登録語長 W に対し O((W + 1)(B + 1))
	 * @param Reading 文法の読取規則
	 * @param Forms 行コメントの登録定義
	 * @param Text BOM を除いた原文
	 * @return 処理系が読む頭部と，最初の有効な宣言の名前の範囲
	 * @throws std::bad_alloc 実行時設定の組のメモリを確保出来ない場合
	 * @throws std::length_error 組の名前又は並びの長さが格納先の上限を超える場合
	 */
	CommentRepository::EncodingHeader CommentRepository::ReadEncodingHeader(
		const ReadingRules &Reading,
		const RegisteredLineForms &Forms,
		const std::string_view Text
	) {
		EncodingHeader Header;
		// エンコーディング宣言を読む頭部を持たない文法の返戻
		if(!Reading.HasSingleLineEncodingHeader && !Reading.HasConditionalSecondLineEncoding) return Header;
		const std::string_view Breaks = Reading.ShouldReadLoneCrAsNewline ? "\r\n" : "\n";
		Header.FirstLineEnd = std::min(Text.find_first_of(Breaks), Text.size());
		size_t Start = 0;
		if(Reading.HasSingleLineEncodingHeader && Text.starts_with(GetOpenerOf(CommentNotation::HashBangLine))) {
			Start = Header.FirstLineEnd;
			if(Start < Text.size() && Text[Start++] == '\r' && Start < Text.size() && Text[Start] == '\n') ++Start;
		}
		const size_t Rows = Reading.HasConditionalSecondLineEncoding ? 2 : 1;
		for(size_t Row = 0; Row < Rows && Start < Text.size(); ++Row) {
			const size_t End = std::min(Text.find_first_of(Breaks, Start), Text.size());
			const size_t CommentStart =
			Start + std::min(Text.substr(Start, End - Start).find_first_not_of(Reading.EncodingLineIndent), End - Start);
			const std::string_view Line = Text.substr(CommentStart, End - CommentStart);
			// コードに続く行の宣言を読まない
			if(!Line.empty() && !Line.starts_with(GetOpenerOf(CommentNotation::HashLine))) break;
			Header.End = End;
			const std::string_view Name = GetEncodingName(Forms, Line);
			if(!Name.empty()) {
				if(Header.NameStart == Header.NameEnd) {
					Header.DeclarationStart = CommentStart;
					Header.NameStart = static_cast<size_t>(Name.data() - Text.data());
					Header.NameEnd = Header.NameStart + Name.size();
				}
				if(Row) Header.HasSecondLineEncoding = true;
			}
			Start = End;
			if(Start < Text.size() && Text[Start++] == '\r' && Start < Text.size() && Text[Start] == '\n') ++Start;
		}
		// 原文上の宣言の位置と値の範囲の返戻
		return Header;
	}

	/**
	 * シバンの起動オプションから原文のエンコーディングを読む関数
	 * 引数の語をオプションと誤読せず，同じ語の連結と後の指定を登録定義に従って読む
	 * 計算量：頭部の長さ B と固定の登録定義の総長 R に対し O((R + 1)(B + 1))
	 * @param Reading 処理系の起動オプションの読取定義
	 * @param Line １行目のシバン
	 * @return 先の指定を読む処理系と後の指定を読む処理系の原文のエンコーディング（指定が無ければ空）
	 */
	std::pair<std::string_view, std::string_view> CommentRepository::ReadShebangEncoding(
		const ReadingRules &Reading,
		std::string_view Line
	) {
		// シバンの起動オプションを読まない処理系での返戻
		if(Reading.ShebangInterpreterName.empty()) return {};
		Line = Line.substr(0, std::min(Line.find('\0'), Line.size()));
		const size_t Interpreter = Line.find(Reading.ShebangInterpreterName);
		// 登録された処理系名が無い行での返戻
		if(Interpreter == std::string_view::npos) return {};
		const size_t Options = Line.find(" -", Interpreter);
		// 起動オプションの開始が無い行での返戻
		if(Options == std::string_view::npos) return {};
		Line.remove_prefix(Options + 1);
		std::string_view Encoding, First;
		// 処理系が使う ASCII の空白だけで次の語を切り出す
		const auto TakeWord = [&Line]() -> std::string_view {
			Line.remove_prefix(std::min(Line.find_first_not_of("\t\n\v\f\r "), Line.size()));
			const size_t End = std::min(Line.find_first_of("\t\n\v\f\r "), Line.size());
			const std::string_view Word = Line.substr(0, End);
			Line.remove_prefix(End);
			// 空白で区切った語の返戻
			return Word;
		};
		while(!Line.empty()) {
			std::string_view Word = TakeWord();
			if(Word.size() < 2 || Word.front() != '-' || Word == "--") break;
			if(Word.starts_with("--")) {
				Word.remove_prefix(2);
				bool IsKnown = false;
				for(const ShebangOptionReading &Option : Reading.ShebangOptions) {
					if(Option.Name.size() < 2 || !Word.starts_with(Option.Name)) continue;
					const std::string_view Tail = Word.substr(Option.Name.size());
					if(!Tail.empty() && Option.Separators.find(Tail.front()) == std::string_view::npos) continue;
					IsKnown = true;
					if(Tail.empty() && Option.Argument == ShebangArgument::Required) TakeWord();
					break;
				}
				if(!IsKnown) break;
				continue;
			}
			for(size_t Pos = 1; Pos < Word.size();) {
				const char Name = Word[Pos++];
				if(Name == Reading.ShebangEncodingOption) {
					if(Pos < Word.size()) {
						const char Val = Word[Pos++];
						for(const auto &[Code, Canonical] : Reading.ShebangEncodingNames) if(Code == Val) {
							Encoding = Canonical;
							if(First.empty()) First = Canonical;
						}
					}
					continue;
				}
				if(Reading.ShebangFlagOptions.find(Name) != std::string_view::npos) continue;
				const ShebangOptionReading *Found = nullptr;
				for(const ShebangOptionReading &Option : Reading.ShebangOptions) if(Option.Name.size() == 1 && Option.Name.front() == Name) {
					Found = &Option;
					break;
				}
				// 処理系が認識しないオプションでの読取の終了
				if(!Found) return { First, Encoding };
				if(Found->ArgumentChars.empty() || Pos < Word.size() && Found->Separators.find(Word[Pos]) != std::string_view::npos) {
					if(Pos == Word.size() && Found->Argument == ShebangArgument::Required) TakeWord();
					break;
				}
				for(uint8_t Count = 0; Count < Found->ArgumentLimit && Pos < Word.size(); ++Count) {
					if(Found->ArgumentChars.find(Word[Pos]) == std::string_view::npos) break;
					++Pos;
				}
			}
		}
		// 登録された起動オプションの値の返戻
		return { First, Encoding };
	}

	/**
	 * エンコーディング宣言の判定関数
	 * 計算量：本文長 N と登録語長 W に対し O((W + 1)(N + 1))
	 * @param Forms 行コメントの登録形の判定条件
	 * @param Text コメント全文
	 * @return 登録語・区切・値が続くなら `true`
	 */
	bool CommentRepository::HasEncodingDecl(const RegisteredLineForms &Forms, const std::string_view Text) {
		// 共通の登録形の照合結果の返戻
		return FindEncodingDecl(Forms, Text, false).has_value();
	}

	/**
	 * 空白除去に依るエンコーディング宣言の新設の判定関数
	 * 値の最初の字迄を共通の登録定義で照合し，無効な区切を有効化する空白除去を避ける
	 * 計算量：登録語長 W と区切後の空白長 G に対し O(W + G + 1)
	 * @param Forms 行コメントの登録形の判定条件
	 * @param Text 正規化中の本文
	 * @param GapStart 区切前の空白の開始
	 * @param GapEnd 区切前の空白の終わり
	 * @return 空白を除くと登録形が現れるなら `true`
	 * @throws std::bad_alloc 照合用のテキストのメモリを確保出来ない場合
	 * @throws std::length_error 照合用のテキストの長さが格納先の上限を超える場合
	 */
	bool CommentRepository::WouldJoinEncodingDecl(
		const RegisteredLineForms &Forms,
		const std::string_view Text,
		const size_t GapStart,
		const size_t GapEnd
	) {
		const size_t Width = Forms.EncodingDeclWord.size();
		// 登録語の直後の区切を必要としない場合の返戻
		if(
			!Width || Forms.CanAllowEncodingSeparatorSpace || GapStart < Width || GapEnd == Text.size() ||
			Forms.EncodingDeclSeparators.find(Text[GapEnd]) == std::string_view::npos
		) return false;
		// 区切と値の最初の字だけを持つ候補（本文の末尾全体は複写しない）
		size_t End = GapEnd + 1;
		while(End < Text.size() && (Text[End] == ' ' || Text[End] == '\t')) ++End;
		std::string Joined(Text.substr(GapStart - Width, Width));
		Joined.append(Text.substr(GapEnd, std::min(End + 1, Text.size()) - GapEnd));
		// 元の登録語と区切が成立するかの返戻
		return HasEncodingDecl(Forms, Joined);
	}

	/**
	 * 行記法の登録形へ読替わる後続文の開始位置の取得関数
	 * エンコーディング宣言の最後の位置と，隣接する囲マーカー間の判定結果を共有する
	 * 囲みの無い組は先頭の名前と値で照合し，末尾全体の記録化と合成を行わない
	 * 計算量：本文長 N，文境界数 S，固定した登録定義の照合費用 R に対し O(N + S + R)
	 * @param Forms 行コメントの登録形の判定条件
	 * @param Text 物理改行の無い共有本文
	 * @param Starts 位置順の後続文の開始位置
	 * @param Work 読取・共有表・候補照会の作業計数（無ければ計上しない）
	 * @return 行記法へ変えると登録形に為る後続文の位置
	 * @throws std::bad_alloc 共有本文又は位置表のメモリを確保出来ない場合
	 * @throws std::length_error 共有本文又は位置表の要素数が格納先の上限を超える場合
	 * @throws FormatLimitExceeded 作業計数が上限を超える場合
	 */
	std::vector<size_t> CommentRepository::GetRegisteredSentenceStarts(
		const RegisteredLineForms &Forms,
		const std::string_view Text,
		const std::span<const size_t> Starts,
		StructureWorkAccount *const Work
	) {
		// 後続文の無い本文には登録形の共有表を作らない
		if(Starts.empty()) return {};
		// 先頭記号を除いた共通の本文ビューを得る為の，１バイトの共有する前置
		const std::string Headed = std::string(GetOpenerOf(CommentNotation::HashLine)).append(Text);
		std::optional<size_t> LastEncoding;
		bool HasReadEncoding = false;
		const std::string_view Marker = Forms.MagicCommentMarker;
		std::vector<size_t> Markers;
		if(!Marker.empty()) for(size_t Pos = Text.find(Marker); Pos != std::string_view::npos;) {
			Markers.push_back(Pos);
			Pos = Text.find(Marker, Pos + Marker.size());
		}
		if(Work) {
			Work->Charge(Text.size());
			Work->Charge(Text.size() + 1);
		}
		// 共有する判定の区間は隣接するマーカーで終わり，各バイトが属する区間は高々２つ
		size_t CachedMarker = std::string_view::npos;
		RegisteredLineForm CachedForm;
		std::vector<size_t> Result;
		for(size_t NextMarker = 0; const size_t Start : Starts) {
			while(NextMarker < Markers.size() && Markers[NextMarker] < Start) ++NextMarker;
			bool IsRegistered = false, NeedsEncoding = false;
			if(NextMarker + 1 < Markers.size()) {
				if(CachedMarker != NextMarker) {
					const size_t Begin = Markers[NextMarker], End = Markers[NextMarker + 1] + Marker.size();
					CachedForm = RegisteredLineFormOf(Forms, std::string_view(Headed).substr(Begin, End - Begin + 1), 0, false, Work);
					CachedMarker = NextMarker;
				}
				IsRegistered = CachedForm.IsMagic || CachedForm.IsEncoding;
				NeedsEncoding = !CachedForm.Pairs;
			} else if(NextMarker < Markers.size()) NeedsEncoding = true;
			else {
				// 記号の１バイトを除く読取へ，同じ本文の先行１バイトを渡す
				const RegisteredLineForm Form =
				RegisteredLineFormOf(Forms, std::string_view(Headed).substr(Start), std::string_view::npos, false, Work);
				IsRegistered = Form.IsEncoding || Form.IsMagic;
				NeedsEncoding = !Form.Pairs;
			}
			// 組の名前で判定出来ない時だけ，共通の登録語の位置を１度求める
			if(NeedsEncoding) {
				if(!HasReadEncoding) {
					if(const std::optional<EncodingDecl> Found = FindEncodingDecl(Forms, Text, true, Work)) LastEncoding = Found->Start;
					HasReadEncoding = true;
				}
				IsRegistered = LastEncoding && Start <= *LastEncoding;
			}
			if(Work) Work->Charge(1);
			if(IsRegistered) Result.push_back(Start);
		}
		// 登録形の後続文の位置の返戻
		return Result;
	}

	/**
	 * 前のメンバのドキュメントかの判定関数
	 * 記号へ密着する本文の頭で前のメンバを指すドキュメント（Doxygen の `///<` `//!<` 等の行と囲みのドキュメント）は，次の宣言のドキュメントにも通常コメントにも揃えない
	 * 記号の後に区切空白を置いた頭（`/// <pre>` の要素）は前のメンバを指さない
	 * 計算量：記号の頭のバイト数 N に対し O(N + 1)
	 * @param Rules 投影済の記法
	 * @param Record コメントの記録
	 * @return 前のメンバを指す頭が記号へ密着する本文なら true
	 */
	bool CommentRepository::IsMemberDoc(const NotationRules &Rules, const CommentRecord &Record) {
		// 前のメンバを指す頭を持つ文法で其の頭が記号へ密着するかの返戻
		return !Rules.MemberDocHead.empty() && Record.Format.ShouldAttachFirstLine && Record.Text.starts_with(Rules.MemberDocHead);
	}

	/**
	 * 行コメントの登録形の判定関数（整形仕様 4.5）
	 * 注記を１度だけ実行時設定の名前と値の組として読む
	 * 組の名前がエンコーディングの登録語ならエンコーディングの指定の形とする
	 * 実行時設定は登録された名前と有効な値の組とし，位置の有効性は呼出側が判定する
	 * 組として読めない注記は，登録語・区切・名前が続く時にエンコーディングの指定の形とする
	 * 形だけを見る為，処理系が読む先頭の行に在るかは呼出側が確かめる（Python は先頭の２行，Ruby は１行目とシバンの次の行だけを読む）
	 * 計算量：コメントの長さ L，登録語数 W，登録値数 V に対し O((W + V + 1) · L)
	 * @param Forms 行コメントの登録形の判定条件
	 * @param Text コメント全文
	 * @param MarkerAt 開始記号を除いた本文の囲みの開始位置（無ければ探し，`std::string_view::npos` なら囲無）
	 * @param HasEncoding 共有したエンコーディング宣言の成立（無ければ探す）
	 * @param Work 組の読取の作業計数（無ければ計上しない）
	 * @return 実行時設定の組の並びと，エンコーディングの指定・実行時設定の形か
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws FormatLimitExceeded 作業計数が上限を超える場合
	 */
	CommentRepository::RegisteredLineForm CommentRepository::RegisteredLineFormOf(
		const RegisteredLineForms &Forms,
		const std::string_view Text,
		const std::optional<size_t> MarkerAt,
		const std::optional<bool> HasEncoding,
		StructureWorkAccount *const Work
	) {
		// 実行時設定の組と登録された名前・値の照合
		RegisteredLineForm Form { MagicCommentPairsOf(Forms, Text, MarkerAt, Work) };
		// 名前が表に在る組を持つか
		const auto HasPair = [&Form](const std::vector<std::string_view> &Names) -> bool {
			// 名前の照合結果の返戻
			return Form.Pairs && std::ranges::any_of(
				*Form.Pairs,
				[&Names](const MagicCommentPair &Pair) -> bool {
					// 表に在る名前かの返戻
					return std::ranges::find(Names, Pair.Name) != Names.end();
				}
			);
		};
		Form.IsEncoding = Form.Pairs ?
		HasPair(Forms.EncodingMagicCommentNames) :
		HasEncoding ? *HasEncoding : FindEncodingDecl(Forms, Text, false, Work).has_value();
		// 設定の名前だけで説明文を登録形へ読替えず，投影した有効な値も照合する
		Form.IsMagic = Form.Pairs && std::ranges::any_of(
			*Form.Pairs,
			[&Forms](const MagicCommentPair &Pair) -> bool {
				const std::unordered_map<std::string_view, std::vector<std::string_view>>::const_iterator Found =
				Forms.MagicCommentValues.find(Pair.Name);
				// 登録された名前と大小文字を問わない値の照合結果の返戻
				return Found != Forms.MagicCommentValues.end() && IsMagicCommentValue(Pair.Val, Found->second);
			}
		);
		// 判定結果の返戻
		return Form;
	}

	/**
	 * コメント記法の判定関数
	 * 計算量：使用可能な記法の登録数 W に対し O(W + 1)（開始記号は固定長）
	 * @param Rules 投影済の記法
	 * @param Text コメントのテキスト
	 * @param IsSrcHead 原文の先頭か
	 * @return 記号に対応する記法
	 */
	CommentNotation CommentRepository::NotationOf(const NotationRules &Rules, const std::string_view Text, const bool IsSrcHead) {
		// 名前付のディレクティブ記法の登録形の照合
		for(const AvailableNotation &Available : Rules.AvailableNotations) {
			if(!IsNamedDirective(Available.Notation)) continue;
			const std::string_view Opener = GetOpenerOf(Available.Notation);
			if(!Text.starts_with(Opener)) continue;
			// 名前迄を開始記号に含むディレクティブ記法の返戻
			if(
				const unsigned char After = Text.size() == Opener.size() ? '\n' : Text[Opener.size()];
				After == '\n' || After == ' ' || Available.IsTabSeparated && After == '\t'
			) return Available.Notation;
		}
		for(const CommentNotation Notation : { CommentNotation::RubyEmbeddedDocument, CommentNotation::StarBangBlock }) {
			// 行頭の埋込ドキュメントと，囲みの `!` を本文へ分けない記法（C / C++ の Doxygen，Rust の内部ドキュメント，SCSS の出力へ残すコメント）の返戻
			if(Rules.IsAvailable(Notation) && Text.starts_with(GetOpenerOf(Notation))) return Notation;
		}
		// 囲記法の場合
		if(Text.starts_with(GetOpenerOf(CommentNotation::Block))) {
			// ドキュメントの開始記号の後に続く字（空の囲み `/**/` と飾りの `/***` はドキュメントでない）
			const std::string_view Doc = GetOpenerOf(CommentNotation::DoubleStarBlock);
			const unsigned char Following = Text.size() > Doc.size() ? Text[Doc.size()] : '\0';
			// ドキュメントの囲みを持つ言語の専用記法と，本文を持たない空の囲み・飾りを区別した記法の返戻（ドキュメントの対象かは別に判定する）
			return Rules.IsAvailable(CommentNotation::DoubleStarBlock) && Text.starts_with(Doc) && Following != '/' && Following != '*' ?
			CommentNotation::DoubleStarBlock :
			CommentNotation::Block;
		}
		// 行記法 `//` の場合
		if(Text.starts_with(GetOpenerOf(CommentNotation::SlashLine))) {
			if(Rules.IsAvailable(CommentNotation::SlashBangLine) && Text.starts_with(GetOpenerOf(CommentNotation::SlashBangLine))) {
				// `!` を本文へ分けない専用記法の返戻
				return CommentNotation::SlashBangLine;
			}
			const std::string_view Triple = GetOpenerOf(CommentNotation::TripleSlashLine);
			// ３個のスラッシュを記号に定める言語の記法を飾りの罫線（４個以上）と区別した記法の返戻（C / C++ の Doxygen，C# ･ Java ･ Rust ･ Swift のドキュメント，TypeScript のディレクティブ）
			return Rules.IsAvailable(CommentNotation::TripleSlashLine) && Text.starts_with(Triple) &&
			(Text.size() == Triple.size() || Text[Triple.size()] != Triple.back()) ?
			CommentNotation::TripleSlashLine :
			CommentNotation::SlashLine;
		}
		for(const CommentNotation Notation : { CommentNotation::HashBangLine, CommentNotation::HashColonLine }) {
			// 原文の先頭のシバンと，Ruby の `#:` を一般の行コメントと区別した記法の返戻
			if(
				Rules.IsAvailable(Notation) && (IsSrcHead || Notation != CommentNotation::HashBangLine) &&
				Text.starts_with(GetOpenerOf(Notation))
			) return Notation;
		}
		// 残る行記法 `#` の返戻
		return CommentNotation::HashLine;
	}

	/**
	 * 行頭の語かの判定関数（整形仕様 4.3，行頭の開始・終端の行条件を持つ埋込ドキュメントの記号）
	 * 処理系と同じく，語の後に空白（改行・書式送等を含む）か行末が続く物だけを語とする
	 * 計算量：行頭語の長さ N に対し O(N)
	 * @param Line 行のテキスト
	 * @param Word 語
	 * @return 行が語で始まり，後に空白か行末が続けば true
	 */
	bool CommentRepository::IsLineHeadWord(const std::string_view Line, const std::string_view Word) {
		// 語の後が空白か行末かの返戻
		return Line.starts_with(Word) && (Line.size() == Word.size() || std::isspace(static_cast<unsigned char>(Line[Word.size()])));
	}

	/**
	 * 本文を１バイトも変えない記録かの判定関数（整形仕様 4.5, 7.5）
	 * @param Record コメントの記録
	 * @return 処理系が読むディレクティブコメント（空白を揃える Go のディレクティブ記法を除く）とシバンなら true
	 */
	bool CommentRepository::IsVerbatimRecord(const CommentRecord &Record) {
		// 本文を其の儘保つ記録かの返戻
		return Record.Notation == CommentNotation::HashBangLine ||
		Record.Role == CommentRole::Directive && !IsSpacedDirective(Record.Notation);
	}

	/**
	 * 開始記号の一部として密着させる本文の頭の取得関数（整形仕様 7.6）
	 * 前のメンバのドキュメントの頭（Doxygen の `///<` 等）だけを記号の一部として開始記号へ密着させ，其の他の記号で始まる本文は区切空白の後の本文とする
	 * 計算量：本文のバイト数 N，記法の登録語数 W に対し O((W + 1)(N + 1))
	 * @param Record 記法の定義を記録したコメントの記録（此の記法で頭を判定する）
	 * @param Body 開始記号の直後のテキスト
	 * @return 本文が始まる記号の一部の頭（無ければ空）
	 * @throws FormatLimitExceeded 記法の定義を記録して居ない場合（フォーマッターの不具合）
	 */
	std::string_view CommentRepository::GetSymbolHeadOf(const CommentRecord &Record, const std::string_view Body) {
		// 採用記法と密着する本文の頭の取得
		const NotationRules &Rules = GetRulesOf(Record);
		if(!Rules.MemberDocHead.empty() && Rules.GetDocFormOf(Record.Notation) && Body.starts_with(Rules.MemberDocHead)) {
			// ドキュメントの記法の前のメンバのドキュメントの頭の返戻
			return Rules.MemberDocHead;
		}
		// 頭の無い本文の返戻
		return {};
	}

	/**
	 * 本文の無い記号だけの行かの判定関数
	 * 計算量：論理行のバイト数 N に対し O(N + 1)
	 * @param Line 論理行
	 * @return ASCII の記号だけから成れば true（空の行を含む）
	 */
	bool CommentRepository::IsSymbolsOnly(const std::string_view Line) {
		// 記号以外の字の有無の返戻
		return std::all_of(
			Line.begin(),
			Line.end(),
			[](const unsigned char Char) -> bool {
				// ASCII の記号かの返戻
				return std::ispunct(Char);
			}
		);
	}

	/**
	 * 配置の段が採用する行記法で置く記録の種類の取得関数（整形仕様 7.6 の記法の選択，行毎に保てるかは本文で決める）
	 * 一般の囲みとドキュメントでない位置のドキュメントの囲み，採用記法の行コメントと埋込ドキュメントの独立した記録，採用しない行記法（記号へ密着しない物）とドキュメントでない位置のドキュメントの行記法を，採用する行記法で置く
	 * 処理系が読む記録と，採用記法が囲みの文法は揃えない
	 * 計算量：本文のバイト数 N，記法の登録語数 W に対し O((W + 1)(N + 1))
	 * @param Record コメントの記録
	 * @param IsDoc 配置先の説明対象が確定したドキュメントの記録か
	 * @return 行記法で置く記録の種類（揃えない記録は `None`）
	 * @throws FormatLimitExceeded 記法の定義を記録して居ない場合（フォーマッターの不具合）
	 */
	CommentRepository::LinePlacement CommentRepository::GetLinePlacementOf(const CommentRecord &Record, const bool IsDoc) {
		// 採用する行記法と記録の種類の取得
		const NotationRules &Rules = GetRulesOf(Record);
		const CommentOutputForm *const Plain = Rules.GetAdoptedFormOf(CommentUse::Plain);
		// 揃えない記録の返戻
		if(!Plain || IsBlockNotation(Plain->Notation) || Record.Role == CommentRole::Directive) return LinePlacement::None;
		// ドキュメントでない位置のドキュメントの記法（メンバの後のドキュメント `///<` 等を除いて通常コメントとする）
		const bool IsUndocumented = Rules.GetDocFormOf(Record.Notation) && !IsDoc && !IsMemberDoc(Rules, Record);
		if(Record.Notation == CommentNotation::Block || IsBlockNotation(Record.Notation) && IsUndocumented) {
			// 一般の囲みの返戻
			return LinePlacement::GeneralBlock;
		}
		// 論理行毎に記法を選ぶ採用記法の行コメントと埋込ドキュメントの独立した記録の返戻
		if(
			Record.Kind == CommentKind::OwnLine &&
			(Record.Notation == Plain->Notation || Record.Notation == CommentNotation::RubyEmbeddedDocument)
		) return LinePlacement::LineRecord;
		// 採用しない行記法（一般の行記法は記号へ密着しない物，ドキュメントでない位置のドキュメントの記法は密着に依らず，密着は配置の段が保つ）の返戻
		if(
			Record.Notation != Plain->Notation && (
				IsUndocumented || !Record.Format.ShouldAttachFirstLine &&
				(Record.Notation == CommentNotation::SlashLine || Record.Notation == CommentNotation::HashLine)
			)
		) return LinePlacement::OtherLine;
		// 行形式へ揃える対象でない記録の返戻
		return LinePlacement::None;
	}

	/**
	 * 出力記法の取得関数（整形仕様 7.6 の記法の選択，記録をコピーしない）
	 * 計算量：未確定の出力記法の選択費用 Q に対し O(Q + 1)（確定済は O(1)）
	 * @param Record 記法の定義を記録したコメントの記録
	 * @return 出力する記法
	 */
	CommentNotation CommentRepository::GetOutputNotationOf(const CommentRecord &Record) {
		// 変方に従う出力記法の返戻
		return Record.Output ? Record.Output->Notation : Record.Notation;
	}

	/**
	 * 囲みのドキュメントへ揃える行形式のドキュメントかの判定関数（整形仕様 7.6 の記法の選択，記号だけでなく本文のドキュメント形式も照合する）
	 * 採用する囲みのドキュメントと同じドキュメント形式の独立した説明の行形式のドキュメントを対象とし，C / C++ のメンバの後の `///<` `//!<` は行末のドキュメントで除く
	 * 計算量：本文のバイト数 N，記法の登録語数 W に対し O((W + 1)(N + 1))
	 * @param Record 記法の定義を記録したコメントの記録
	 * @return 囲みのドキュメントへ揃える行形式のドキュメントなら true
	 * @throws FormatLimitExceeded 記法の定義を記録して居ない場合（フォーマッターの不具合）
	 */
	bool CommentRepository::IsBlockLineDoc(const CommentRecord &Record) {
		// 囲みへ揃える行形式のドキュメントの条件の取得
		const NotationRules &Rules = GetRulesOf(Record);
		const CommentOutputForm *const Doc = Rules.GetAdoptedFormOf(CommentUse::Doc, Rules.IsInnerDoc(Record.Notation));
		// 採用する囲みのドキュメントと同じドキュメント形式を持つ独立した行形式のドキュメントかの返戻
		return Record.Kind == CommentKind::OwnLine && !IsBlockNotation(Record.Notation) && Record.Format.InputDocument && Doc &&
		IsBlockNotation(Doc->Notation) && Record.Format.InputDocument == Rules.GetDocFormOf(Doc->Notation) &&
		!IsMemberDoc(Rules, Record);
	}

	/**
	 * 本文を囲みのドキュメントへ入れれるかの判定関数（整形仕様 7.6, 7.7）
	 * 本文が囲みを閉じる記号（ネストの囲みを持つ記法では開く記号も）を含めば，囲みへ入れるとドキュメントが途中で閉じる
	 * 計算量：本文のバイト数 N に対し O(N + 1)
	 * @param Rules 文法の記法の定義
	 * @param Text ドキュメントの本文
	 * @return 囲みのドキュメントへ入れてもドキュメントが途中で閉じなければ true
	 */
	bool CommentRepository::CanFrame(const NotationRules &Rules, const std::string_view Text) {
		// 囲みの終端とネストの開始を本文に持たないかの返戻
		return Text.find(GetCloserOf(CommentNotation::DoubleStarBlock)) == std::string_view::npos &&
		(!Rules.IsBlockCommentNested || Text.find(GetOpenerOf(CommentNotation::Block)) == std::string_view::npos);
	}

	/**
	 * 通常コメントの行のテキストかの判定関数（整形仕様 4.5, 7.6）
	 * 記法を変えた通常コメントの各行が，別の記法（シバン・三重スラッシュ等）にも処理系の登録形にも為らない事を確かめる
	 * 計算量：テキストの長さ L と登録語の数 W に対し O(W · L)
	 * @param Rules 投影済の記法と其の成立条件
	 * @param Text 行コメントのテキスト（複数行は行毎に見る）
	 * @param Notation 行コメントの記法
	 * @return 全ての行を其の記法の通常コメントとして読むなら true
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	bool CommentRepository::IsPlainLineText(
		const NotationRules &Rules,
		const std::string_view Text,
		const CommentNotation Notation
	) {
		// 論理行毎の通常コメントの記法の照合
		for(size_t Cursor = 0; Cursor <= Text.size();) {
			const size_t End = std::min(Text.find('\n', Cursor), Text.size());
			const std::string_view Line = Text.substr(Cursor, End - Cursor);
			// 行の登録形の判定
			// 別の記法かエンコーディング宣言・実行時設定の登録形に為る行を持つ事の返戻
			if(
				const RegisteredLineForm Form = RegisteredLineFormOf(Rules.OutputForms.Conditions, Line);
				NotationOf(Rules, Line, true) != Notation || Form.IsEncoding || Form.IsMagic
			) return false;
			Cursor = End + 1;
		}
		// 全ての行を通常コメントとして読む事の返戻
		return true;
	}

	/**
	 * 記号と本文の合成関数（整形仕様 7.6）
	 * 非空の本文との区切空白をスペース１つとし，本文の無い形と記号の一部の頭に密着する最初の論理行には区切空白を出さない（記号・タブで始まる本文は本文の一部）
	 * 囲みの複数の論理行は，改行を持たない文中の物と，一般の囲みの行末の物・改行を持たない物を閉じた囲みの並びにし，其の他を継続行の `*` を持つ１つの囲みにする
	 * 埋込ドキュメントは行頭の開始記号と終端記号の行の間へ本文の行を置く（行頭の終端記号に読める本文の行はインデントする）
	 * 計算量：本文と開始・終端記号のバイト数の和 N に対し O(N + 1)
	 * @param Record コメントの記録
	 * @return 記録の記法で記号と本文を合成したテキスト
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws FormatLimitExceeded 記法の定義を記録して居ない場合（フォーマッターの不具合）
	 */
	std::string CommentRepository::Compose(const CommentRecord &Record) {
		// 開始・終端記号と本文の取得
		const std::string_view Opener = GetOpenerOf(Record.Notation), Closer = GetCloserOf(Record.Notation), Text = Record.Text;
		// 埋込ドキュメントの開始行と本文と終端行の合成
		if(Record.Notation == CommentNotation::RubyEmbeddedDocument) {
			std::string Output(Opener);
			for(size_t Cursor = 0; Cursor <= Text.size();) {
				const size_t End = std::min(Text.find('\n', Cursor), Text.size());
				const std::string_view Line = Text.substr(Cursor, End - Cursor);
				// 行頭の終端記号に読める本文の行のインデント
				Output.append(IsLineHeadWord(Line, Closer) ? "\n\t" : "\n").append(Line);
				Cursor = End + 1;
			}
			// 行頭の開始記号と終端記号の行で本文の行を囲む埋込ドキュメントの返戻
			return Output.append("\n").append(Closer);
		}
		const bool IsVerbatim = IsVerbatimRecord(Record);
		// ディレクティブコメントの内側の儘の囲みの返戻
		if(IsVerbatim && IsBlockNotation(Record.Notation)) return std::string(Opener).append(Text).append(Closer);
		std::string Output;
		std::vector<std::string_view> Lines;
		// 概要を記述する為の空の論理行だけを本文の前へ置く
		if(Record.Format.HasSummaryPlaceholder && !Text.empty()) Lines.emplace_back();
		for(size_t Cursor = 0;;) {
			const size_t End = std::min(Text.find('\n', Cursor), Text.size());
			Lines.push_back(Text.substr(Cursor, End - Cursor));
			if(End == Text.size()) break;
			Cursor = End + 1;
		}
		// 記号へ密着する論理行か（文毎に分けた続きの行は最初の論理行と同じ記号の一部の頭で始まる）
		const std::string_view Run = Record.Format.ShouldAttachFirstLine ? GetSymbolHeadOf(Record, Lines.front()) : std::string_view();
		// 行記号へ密着する論理行の判定
		const auto IsGluedLine = [&Lines, &Record, Run](const size_t Idx) -> bool {
			// 密着する論理行かの返戻
			return Record.Format.ShouldAttachFirstLine && (!Idx || !Run.empty() && Lines[Idx].starts_with(Run));
		};
		if(IsBlockNotation(Record.Notation)) {
			// １行の囲み（記号へ密着する記号だけの本文は本文を持たない形として終端記号とも密着させる）
			const auto Single = [Opener, Closer](const std::string_view Line, const bool IsGlued) -> std::string {
				// 区切空白を挟んだ囲みの返戻
				return std::string(Opener).append(Line.empty() || IsGlued ? "" : " ").append(Line).append(
					IsSymbolsOnly(Line) && (Line.empty() || IsGlued) ? "" : " "
				).append(Closer);
			};
			// １行の囲みの返戻
			if(Lines.size() == 1 && !Record.Format.ShouldSeparateLogicalLines) return Single(Lines.front(), IsGluedLine(0));
			// 改行を持たない文中の囲み，行末と改行を持たない一般の囲みの閉じた囲みの並び（行末は文毎に積む）
			if(
				Record.Kind == CommentKind::Inline ?
				!Record.Format.ShouldSeparateLogicalLines :
				Record.Notation == CommentNotation::Block && (Record.Kind == CommentKind::Trailing || !Record.Format.ShouldSeparateLogicalLines)
			) {
				for(size_t Idx = 0; Idx < Lines.size(); ++Idx) {
					// 開始記号・終端記号だけの行に当たる空の論理行は閉じた囲みにしない
					if(Lines[Idx].empty()) continue;
					Output.append(Output.empty() ? "" : Record.Kind == CommentKind::Inline ? " " : "\n").append(
						Single(Lines[Idx], IsGluedLine(Idx))
					);
				}
				// 閉じた囲みの並びの返戻
				return Output;
			}
			// 開始・終端記号の行は本文の空の論理行とは別に合成し，一般の囲みへドキュメントの枠を補わない
			Output.append(Opener);
			for(size_t Idx = 0; Idx < Lines.size(); ++Idx) {
				if(!Idx && !Record.Format.ShouldSeparateOpener) {
					Output.append(Lines[Idx].empty() || IsGluedLine(Idx) ? "" : " ").append(Lines[Idx]);
				} else {
					Output.append("\n *");
					if(!Lines[Idx].empty()) Output.append(" ").append(Lines[Idx]);
				}
			}
			// 本文が１行へ減っても，投影済の囲み内の改行条件を失わない
			Output.append(
				Record.Format.ShouldSeparateCloser ||
				Lines.size() == 1 && Record.Format.ShouldSeparateLogicalLines && !Record.Format.ShouldSeparateOpener ? "\n " : " "
			).append(Closer);
			// 継続行の `*` を持つ囲みの返戻
			return Output;
		}
		for(size_t Idx = 0; Idx < Lines.size(); ++Idx) {
			if(Idx) Output += '\n';
			Output.append(Opener);
			// 位置指示の必須スペースを置き，其の他は記号の一部の頭に密着する行と本文を其の儘保つ続きの行の区切空白を省く（記号・タブで始まる本文も区切空白の後に置く）
			if(!Lines[Idx].empty()) {
				const bool HasSeparator = Record.Notation == CommentNotation::GoLineLine || !(IsGluedLine(Idx) || Idx && IsVerbatim);
				Output.append(HasSeparator ? " " : "").append(Lines[Idx]);
			}
		}
		// 行記法の返戻
		return Output;
	}

	/**
	 * 出力記法への変方の取得関数（整形仕様 7.6 の記法の選択）
	 * 囲みのドキュメントへ揃える行形式のドキュメントは１つの囲みのドキュメントへ揃える（囲みを閉じる又はネストを開く本文は行形式の儘とする）
	 * 採用する囲みのドキュメントと同じドキュメント形式の別の囲み（C / C++ の感嘆符付の囲み）は其のドキュメントの記法へ揃える
	 * 行毎の記号を持たない埋込ドキュメントは通常コメントの記法へ揃える（行コメントが別の記法・登録形に為る物は除く）
	 * 処理系が読む記録と其の他は入力の記法の儘とする（入力の記法は記録に残し，配置に依る通常コメントの行形式は配置の段が選ぶ）
	 * 計算量：本文のバイト数 N，記法の登録語数 W に対し O((W + 1)(N + 1))
	 * @param Record 記法の定義を記録したコメントの記録
	 * @return 出力記法への変方
	 * @throws FormatLimitExceeded 記法の定義を記録して居ない場合（フォーマッターの不具合）
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	CommentRepository::OutputChange CommentRepository::OutputChangeOf(const CommentRecord &Record) {
		// 入力の記法で出力する処理系が読む記録の返戻
		if(Record.Role == CommentRole::Directive) return OutputChange::None;
		const NotationRules &Rules = GetRulesOf(Record);
		// 囲みのドキュメントへ揃える行形式のドキュメントの返戻
		if(IsBlockLineDoc(Record) && CanFrame(Rules, Record.Text)) return OutputChange::Frame;
		const CommentOutputForm *const Doc = Rules.GetAdoptedFormOf(CommentUse::Doc, Rules.IsInnerDoc(Record.Notation));
		// 採用する囲みのドキュメントと同じドキュメント形式の別の囲みの独立した説明のドキュメント（C / C++ のメンバの後の `///<` `//!<` は行末のドキュメントで除く）の返戻
		if(
			Record.Kind == CommentKind::OwnLine && !IsMemberDoc(Rules, Record) && Doc && IsBlockNotation(Doc->Notation) &&
			IsBlockNotation(Record.Notation) && Record.Notation != Doc->Notation && Record.Format.InputDocument &&
			Record.Format.InputDocument == Rules.GetDocFormOf(Doc->Notation) && !Record.Format.ShouldAttachFirstLine
		) return OutputChange::DocNotation;
		// 空の論理行も記号付の行で表し，行コメントが別の記法・登録形に為らない埋込ドキュメントの返戻（整形仕様 4.5, 7.3, 7.6）
		if(
			const CommentOutputForm *const Plain = Rules.GetAdoptedFormOf(CommentUse::Plain);
			Record.Notation == CommentNotation::RubyEmbeddedDocument && Plain
		) {
			CommentRecord Line = Record;
			Line.Notation = Plain->Notation;
			// 通常コメントの行のテキストに為る埋込ドキュメントの返戻
			if(IsPlainLineText(Rules, Compose(Line), Plain->Notation)) return OutputChange::PlainLine;
		}
		// 入力の記法の儘の記録の返戻
		return OutputChange::None;
	}

	/**
	 * 変方に従う出力記法の取得関数
	 * @param Record 記法の定義を記録したコメントの記録
	 * @param Change 出力記法への変方
	 * @return 出力する記法
	 * @throws FormatLimitExceeded 記法の定義を記録して居ない場合（フォーマッターの不具合）
	 */
	CommentNotation CommentRepository::GetOutputNotationFor(const CommentRecord &Record, const OutputChange Change) {
		// 出力の変方毎の記法の選択
		switch(Change) {
		case OutputChange::Frame:
		case OutputChange::DocNotation:
			// ドキュメントの採用記法の返戻
			return GetRulesOf(Record).GetAdoptedFormOf(CommentUse::Doc, GetRulesOf(Record).IsInnerDoc(Record.Notation))->Notation;
		case OutputChange::PlainLine:
			// 通常コメントの採用記法の返戻
			return GetRulesOf(Record).GetAdoptedFormOf(CommentUse::Plain)->Notation;
		case OutputChange::None:
			break;
		}
		// 入力の記法の返戻
		return Record.Notation;
	}

	/**
	 * 記号の後の空白を処理系が値として読むディレクティブの定義かの判定関数（整形仕様 4.5, 7.5, 7.6）
	 * Example の出力注記・cgo の前文・位置指示は記号の後の空白も値に含む為，区切空白を揃えない
	 * @param Def ディレクティブの登録定義
	 * @return 記号の後の空白が値に属する定義なら true
	 */
	bool CommentRepository::IsValSpacing(const DirectiveDefinitionId Def) {
		// 値に属する空白を持つ定義かの返戻
		return Def == DirectiveDefinitionId::ExampleOutput || Def == DirectiveDefinitionId::CgoPreamble ||
		Def == DirectiveDefinitionId::LinePos || Def == DirectiveDefinitionId::BlockLinePos;
	}

	/**
	 * 継続行の装飾と区切空白の長さの取得関数
	 * 最初の空でない行で `*` と区切空白を確認し，続く行では同じ装飾を読む
	 * 計算量：行頭の区切空白のバイト数 N に対し O(N + 1)
	 * @param Line 行頭のインデントを除いた継続行
	 * @param IsDecorated 最初の装飾を確認済か（装飾を確認した時に true を記録する）
	 * @return 装飾と区切空白の長さ（空の行は０，装飾の揃わない行は無い）
	 */
	std::optional<size_t> CommentRepository::GetDecorationWidth(const std::string_view Line, bool &IsDecorated) {
		// 空の行は装飾の揃い方を変えない事の返戻
		if(Line.empty()) return 0;
		// 継続記号の無い行の返戻
		if(!Line.starts_with('*')) return std::nullopt;
		const std::string_view AfterStar = TrimLeadingSeparator(Line.substr(1));
		// 最初の継続記号が本文と密着して居る行の返戻
		if(!IsDecorated && Line.size() > 1 && AfterStar.size() == Line.size() - 1) return std::nullopt;
		IsDecorated = true;
		// 継続記号と其の区切空白の長さの返戻
		return Line.size() - AfterStar.size();
	}

	/**
	 * 囲みの内側の論理行の取得関数
	 * 開始記号の後と終端記号の前の区切空白，継続行のインデントを除く
	 * 継続行全体の装飾を確認した時だけ，飾りの `*` と其の後の区切空白も除く
	 * コード例の判定は，物理行のインデントと継続記号を除いた本文で行う
	 * 計算量：囲みの内側のバイト数 N とコード例の判定費用 Q に対し O(N + Q + 1)
	 * @param Rules 投影済の記法
	 * @param Interior 開始記号と終端記号の間
	 * @return 論理行（開始記号・終端記号だけの行は空の行）
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	std::vector<std::string> CommentRepository::GetBlockLines(const NotationRules &Rules, const std::string_view Interior) {
		std::optional<LexemeReading> Reading;
		if(Rules.ShouldDecodeUnicodeEscapes && Interior.find("\\u") != std::string_view::npos) {
			Reading = ParseAdapter::ReadUnicodeEscapes(Interior);
		}
		const std::string_view ReadText = Reading ? std::string_view(Reading->Text) : Interior;
		// 元の物理行で囲みの枠を識別し，展開が作る改行はコード例の判定後に分ける
		std::vector<std::string_view> Physical;
		bool HasEscapedBreak = false;
		for(size_t Cursor = 0;;) {
			size_t End = std::min(ReadText.find_first_of("\r\n", Cursor), ReadText.size());
			while(End < ReadText.size()) {
				const bool IsEscaped = Reading && Reading->OriginalOffsets[End + 1] - Reading->OriginalOffsets[End] > 1;
				HasEscapedBreak = HasEscapedBreak || IsEscaped;
				if(!IsEscaped && ReadText[End] == '\n') break;
				End = std::min(ReadText.find_first_of("\r\n", End + 1), ReadText.size());
			}
			Physical.push_back(TrimLeadingSeparator(ReadText.substr(Cursor, End - Cursor)));
			if(End == ReadText.size()) break;
			Cursor = End + 1;
		}
		// 最初の空でない継続行の `*` と区切空白，及び継続行全体の揃った装飾を確認する
		bool IsDecorated = false, CanDecorate = true;
		for(size_t Idx = 1; Idx < Physical.size(); ++Idx) if(!GetDecorationWidth(Physical[Idx], IsDecorated)) {
			IsDecorated = false;
			CanDecorate = false;
			break;
		}
		std::vector<std::string> Lines;
		Lines.reserve(Physical.size());
		// 枠を除いた本文だけがコード例の判定へ進み，展開済の文字と元の綴りの対応を継ぐ
		std::string Body;
		LexemeReading Logical;
		if(HasEscapedBreak) {
			Body.reserve(Interior.size());
			Logical.Text.reserve(ReadText.size());
			Logical.OriginalOffsets.reserve(Reading->OriginalOffsets.size());
		}
		for(size_t Idx = 0; Idx < Physical.size(); ++Idx) {
			std::string_view Line = Physical[Idx];
			if(Idx && IsDecorated && Line.starts_with('*')) Line = TrimLeadingSeparator(Line.substr(1));
			if(Idx + 1 == Physical.size()) Line = TrimTrailingSeparator(Line);
			const size_t Begin = Line.data() - ReadText.data(), End = Begin + Line.size();
			const size_t RawBegin = Reading ? Reading->OriginalOffsets[Begin] : Begin;
			Lines.emplace_back(Interior.substr(RawBegin, (Reading ? Reading->OriginalOffsets[End] : End) - RawBegin));
			if(HasEscapedBreak) {
				if(Idx) {
					Logical.Text += '\n';
					Logical.OriginalOffsets.push_back(Body.size());
					Body += '\n';
				}
				const size_t Offset = Body.size();
				Logical.Text.append(Line);
				Body.append(Lines.back());
				for(size_t At = Begin; At < End; ++At) Logical.OriginalOffsets.push_back(Offset + Reading->OriginalOffsets[At] - RawBegin);
			}
		}
		// 翻訳段階の改行が無い本文は，物理行から確定した論理行を使う
		if(!HasEscapedBreak) return Lines;
		Logical.OriginalOffsets.push_back(Body.size());
		// 元の枠への参照と転写済の行を破棄し，枠を除いた本文だけをコード例の判定へ渡す
		Physical.clear();
		Lines.clear();
		Reading.reset();
		bool IsUnclosed = false;
		const std::vector<unsigned char> Protected = CommentNormalizer::GetCodeMask(Rules.CodeExamples, Logical.Text, IsUnclosed);
		const std::string_view Text = Logical.Text;
		// 元の物理行の枠は除去済の為，展開が作った継続行だけで装飾を判定する
		std::vector<size_t> DecorationWidths;
		bool IsGenerated = false;
		for(size_t Cursor = 0;;) {
			size_t End = std::min(Text.find_first_of("\r\n", Cursor), Text.size());
			while(End < Text.size()) {
				const bool IsEscaped = Logical.OriginalOffsets[End + 1] - Logical.OriginalOffsets[End] > 1;
				if(IsEscaped ? !Protected[End] : Text[End] == '\n') break;
				End = std::min(Text.find_first_of("\r\n", End + 1), Text.size());
			}
			Physical.push_back(Text.substr(Cursor, End - Cursor));
			DecorationWidths.push_back(0);
			if(IsGenerated) {
				Physical.back() = TrimLeadingSeparator(Physical.back());
				if(const std::optional<size_t> Width = GetDecorationWidth(Physical.back(), IsDecorated)) DecorationWidths.back() = *Width;
				else CanDecorate = false;
			}
			if(End == Text.size()) break;
			Cursor = End + 1;
			IsGenerated = Logical.OriginalOffsets[Cursor] - Logical.OriginalOffsets[End] > 1;
			if(Text[End] == '\r' && Cursor < Text.size() && Text[Cursor] == '\n') {
				IsGenerated = Logical.OriginalOffsets[Cursor + 1] - Logical.OriginalOffsets[Cursor] > 1;
				++Cursor;
			}
		}
		for(size_t Idx = 0; Idx < Physical.size(); ++Idx) {
			std::string_view Line = Physical[Idx];
			if(CanDecorate && IsDecorated) Line.remove_prefix(DecorationWidths[Idx]);
			const size_t Begin = Line.data() - Text.data(), End = Begin + Line.size();
			Lines.emplace_back(Body.substr(Logical.OriginalOffsets[Begin], Logical.OriginalOffsets[End] - Logical.OriginalOffsets[Begin]));
		}
		// 論理行の返戻
		return Lines;
	}

	/**
	 * 記号の一部の頭と本文の間の区切空白を揃えた論理行の取得関数（整形仕様 7.6）
	 * 頭は記号の一部の為，頭と本文の間も区切の有無・個数・混在に依らず半角スペース１つとする
	 * 計算量：行と記号の頭のバイト数の和 N に対し O(N + 1)
	 * @param Line 頭で始まる論理行
	 * @param Head 記号の一部の頭
	 * @return 頭と区切空白と本文を繋いだ論理行
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	std::string CommentRepository::GetHeadedLine(const std::string_view Line, const std::string_view Head) {
		// 記号の頭の後の区切空白の除去
		const std::string_view Rest = TrimLeadingSeparator(Line.substr(Head.size()));
		// 頭の後に本文が在れば半角スペース１つを挟んだ論理行の返戻
		return std::string(Head).append(Rest.empty() ? "" : " ").append(Rest);
	}

	/**
	 * ディレクティブの前の区切空白を除いたテキストの取得関数（整形仕様 4.5, 7.6）
	 * 処理系が区切と読む字だけを除く（TypeScript の三重スラッシュディレクティブは全角スペースも区切と読み，其の他は半角スペースとタブ）
	 * 計算量：先頭の区切空白のバイト数 N に対し O(N + 1)
	 * @param Def ディレクティブの登録定義（無ければ半角スペースとタブ）
	 * @param Line 記号の後のテキスト
	 * @return 行頭の区切空白を除いたテキスト
	 */
	std::string_view CommentRepository::TrimDirectiveSeparator(
		const std::optional<DirectiveDefinitionId> Def,
		const std::string_view Line
	) {
		// 全角スペースも区切と読むディレクティブの返戻
		if(Def == DirectiveDefinitionId::TypeScriptRef) return TrimLeadingSeparator(Line);
		// 半角スペースとタブを除いたテキストの返戻
		return Line.substr(std::min(Line.find_first_not_of(" \t"), Line.size()));
	}

	/**
	 * 位置指示の本文の成立判定関数
	 * 右端から行又は行と列を読み，ファイル名に含まれるコロンを残す
	 * 計算量：位置情報のテキストのバイト数 N に対し O(N + 1)
	 * @param Text 開始記号から終端記号迄のテキスト
	 * @param Notation 行又は囲みの位置指示の記法
	 * @return 処理系が受け付ける位置情報なら true
	 */
	bool CommentRepository::HasValidLinePos(const std::string_view Text, const CommentNotation Notation) {
		// 位置指示以外の記法の返戻
		if(Notation != CommentNotation::GoLineLine && Notation != CommentNotation::GoLineBlock) return false;
		const std::string_view Opener = GetOpenerOf(Notation);
		// 必須の区切空白を持たないテキストの返戻
		if(!Text.starts_with(Opener) || Text.size() <= Opener.size() || Text[Opener.size()] != ' ') return false;
		std::string_view Body = Text.substr(Opener.size() + 1);
		if(Notation == CommentNotation::GoLineBlock) {
			const std::string_view Closer = GetCloserOf(Notation);
			// 終端記号の無い囲みの返戻
			if(!Body.ends_with(Closer)) return false;
			Body.remove_suffix(Closer.size());
		} else if(Body.ends_with('\r')) Body.remove_suffix(1);
		// 複数行の囲みと行内の制御文字の返戻
		if(Body.find('\n') != std::string_view::npos || Body.find('\r') != std::string_view::npos) return false;
		// 右端からの行番号と列番号の読取
		const size_t LastColon = Body.rfind(':');
		// 位置区切の無いテキストの返戻
		if(LastColon == std::string_view::npos) return false;
		// １０進数の全桁を読むローカル関数
		const auto NumOf = [](const std::string_view Digits) -> std::optional<uint64_t> {
			// １０進数の読取先と結果の取得
			uint64_t Num = 0;
			const std::from_chars_result Read = std::from_chars(Digits.data(), Digits.data() + Digits.size(), Num);
			// １０進数の全桁を読めた時だけの数値の返戻
			return Read.ec == std::errc() && Read.ptr == Digits.data() + Digits.size() && !Digits.empty() ?
			std::optional<uint64_t>(Num) :
			std::nullopt;
		};
		const std::optional<uint64_t> Last = NumOf(Body.substr(LastColon + 1));
		// 末尾の数値が成立しないテキストの返戻
		if(!Last) return false;
		const size_t PrevColon = LastColon ? Body.rfind(':', LastColon - 1) : std::string_view::npos;
		const std::optional<uint64_t> Prev =
		PrevColon == std::string_view::npos ? std::nullopt : NumOf(Body.substr(PrevColon + 1, LastColon - PrevColon - 1));
		constexpr uint64_t Max = static_cast<uint64_t>(1) << 30; // 位置指示に許す行・列の上限
		const uint64_t LastVal = *Last;
		// 右端の数値と，有れば手前の数値（右端が列の時の行）が範囲内かの返戻
		return LastVal && LastVal < Max + 1 && (!Prev || *Prev && *Prev < Max + 1);
	}

	/**
	 * グループを纏める記法の取得関数（整形仕様 3.3, 7.3, 7.6）
	 * 配置の段が採用する行記法で置く記録は出力で隣接する其の記法のグループと並ぶ為に採用する行記法とし，其の他は出力記法とする（囲みの記法はグループを作らない）
	 * 囲みのドキュメントへ揃える行形式のドキュメントは，行毎に揃えるかを決めず入力の記法で纏める（投影が１つのドキュメントの記録へ統合する行と同じグループ）
	 * 計算量：本文のバイト数 N，記法の登録語数 W に対し O((W + 1)(N + 1))
	 * @param Record 記法の定義を記録し本文を分解したコメントの記録
	 * @param IsDoc 配置先の説明対象が確定したドキュメントの記録か
	 * @return グループを纏める記法
	 * @throws FormatLimitExceeded 記法の定義を記録して居ない場合（フォーマッターの不具合）
	 */
	CommentNotation CommentRepository::GetGroupNotationOf(const CommentRecord &Record, const bool IsDoc) {
		// 配置に採用する記法の選択
		if(GetLinePlacementOf(Record, IsDoc) != LinePlacement::None) {
			// 採用する行記法で置く記録の採用記法の返戻
			return GetRulesOf(Record).GetAdoptedFormOf(CommentUse::Plain)->Notation;
		}
		const OutputChange Change = Record.Output ? Record.Output->Transformation : OutputChange::None;
		// 囲みのドキュメントへ揃える行形式のドキュメントの入力の記法と，其の他の出力記法の返戻
		return Change == OutputChange::Frame ? Record.Notation : GetOutputNotationOf(Record);
	}

	/**
	 * 共有する出力本文の取得関数
	 * 計算量：O(1)
	 * @param Record 出力記法を確定した記録
	 * @return 変換した本文又は元の本文
	 */
	std::string_view CommentRepository::GetOutputTextOf(const CommentRecord &Record) {
		// 出力の本文の返戻
		return Record.Output && Record.Output->Text ? std::string_view(*Record.Output->Text) : std::string_view(Record.Text);
	}

	/**
	 * 本文世代の出力記法の確定関数
	 * 計算量：本文長 B と登録語数 W に対し O((W + 1)(B + 1))
	 * @param Record 記法・本文を確定した記録
	 * @throws FormatLimitExceeded 記法の定義が無い場合
	 * @throws std::bad_alloc 変換する本文のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	void CommentRepository::ResolveOutput(CommentRecord &Record) {
		// 本文世代の出力記法の確定
		Record.StructureIndex.reset();
		CommentOutput Output;
		Output.Transformation = OutputChangeOf(Record);
		Output.Notation = GetOutputNotationFor(Record, Output.Transformation);
		Output.ShouldAttachFirstLine = Record.Format.ShouldAttachFirstLine;
		switch(Output.Transformation) {
		case OutputChange::Frame:
			Output.ShouldAttachFirstLine = false;
			break;
		case OutputChange::None:
		case OutputChange::DocNotation:
		case OutputChange::PlainLine:
			break;
		}
		Record.Output = std::move(Output);
		// 終了
		return;
	}

	/**
	 * ファイルの頭部へ置く登録定義かの判定関数（整形仕様 4.5）
	 * `package` 等の宣言より前の頭部に置き，後続の宣言へ作用しない登録定義（ビルド条件・実行時設定）を区分する
	 * @param Def 登録定義
	 * @return 頭部へ置く登録定義なら true
	 */
	bool CommentRepository::IsFileHeadDirective(const DirectiveDefinitionId Def) {
		// 頭部の登録定義かの返戻
		return Def == DirectiveDefinitionId::GoBuild || Def == DirectiveDefinitionId::GoDebug;
	}

	/**
	 * 原文の形のテキストから記号と区切空白を除いた本文への分解関数（整形仕様 3.2, 4.3）
	 * 区切空白は記号と本文の間の半角スペース・全角スペース・タブの並び（記号の後の空白を値として読むディレクティブは半角スペース１つ）とし，区切空白の無い本文の内，記号の一部の頭（前のメンバのドキュメント）に密着する本文と本文を其の儘保つ記録の本文は密着として記録する
	 * インデントは記号の前に置く物で本文に持たない為，本文を其の儘保つ記録を除く各論理行の行頭の空白を除く（本文は記法に依らない）
	 * 行毎の記号を持たない埋込ドキュメントは物理行毎の論理行とする（開始記号・終端記号だけの行は空の行）
	 * 計算量：原文のバイト数 N，記法の登録語数 W に対し O((W + 1)(N + 1))
	 * @param Record テキストを本文に持つコメントの記録（本文・密着・改行の有無を書き換える）
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws FormatLimitExceeded 記法の定義を記録して居ない場合（フォーマッターの不具合）
	 */
	void CommentRepository::Decompose(CommentRecord &Record) {
		// 原文のテキストと記法の分解条件の取得
		const std::string Raw = std::move(Record.Text);
		const std::string_view View = Raw, Opener = GetOpenerOf(Record.Notation);
		const bool IsSpaced = IsSpacedDirective(Record.Notation), IsVerbatim = IsVerbatimRecord(Record);
		// ディレクティブの登録定義と，記号の後の空白を値として読むディレクティブか（区切空白は１つだけ）
		const std::optional<DirectiveDefinitionId> Def =
		Record.Directive ? std::optional<DirectiveDefinitionId>(Record.Directive->Definition) : std::nullopt;
		const bool HasValSpacing = IsVerbatim && Def && IsValSpacing(*Def);
		Record.StructureIndex.reset();
		Record.Text.clear();
		Record.Format.ShouldAttachFirstLine = false;
		Record.Format.ShouldSeparateLogicalLines = View.find('\n') != std::string_view::npos;
		Record.Format.ShouldSeparateOpener = false;
		Record.Format.ShouldSeparateCloser = false;
		const std::string_view Closer = GetCloserOf(Record.Notation);
		if(GetRulesOf(Record).HasMarkupComments) {
			// HTML のコメントの検証用の本文の１度の分解と共有構造への引渡
			const NotationRules &Markup = GetRulesOf(Record);
			std::string_view Body = View;
			if(Body.starts_with(Markup.MarkupCommentOpen)) Body.remove_prefix(Markup.MarkupCommentOpen.size());
			if(Body.ends_with(Markup.MarkupCommentClose)) Body.remove_suffix(Markup.MarkupCommentClose.size());
			for(size_t Cursor = 0;;) {
				const size_t End = std::min(Body.find('\n', Cursor), Body.size());
				std::string_view Line = Body.substr(Cursor, End - Cursor);
				Line.remove_prefix(std::min(Line.find_first_not_of(" \t"), Line.size()));
				Record.Text.append(Line);
				if(End == Body.size()) break;
				Record.Text.push_back('\n');
				Cursor = End + 1;
			}
			// HTML の本文の分解の終了
			return;
		}
		if(Record.Notation == CommentNotation::RubyEmbeddedDocument) {
			// 物理行毎の論理行（開始記号の行の残り，本文の行，行頭の終端記号の行の残り）
			std::vector<std::string_view> Lines;
			for(size_t Cursor = std::min(Opener.size(), View.size());;) {
				const size_t End = std::min(View.find('\n', Cursor), View.size());
				Lines.push_back(View.substr(Cursor, End - Cursor));
				if(End == View.size()) break;
				Cursor = End + 1;
			}
			// ２行目以降の最後の行が空白か行末の続く行頭の終端記号で始まるか
			if(Lines.size() > 1 && IsLineHeadWord(Lines.back(), Closer)) Lines.back().remove_prefix(Closer.size());
			for(size_t Idx = 0; Idx < Lines.size(); ++Idx) {
				std::string_view Line = Lines[Idx];
				// 開始記号・終端記号の後の区切空白と，本文の行のインデントの除去
				Line = TrimLeadingSeparator(Line);
				Record.Text.append(Idx ? "\n" : "").append(Line);
			}
			// 埋込ドキュメントの本文の分解の終了
			return;
		}
		if(IsBlockNotation(Record.Notation) && View.size() >= Opener.size() + Closer.size() && View.ends_with(Closer)) {
			const std::string_view Interior = View.substr(Opener.size(), View.size() - Opener.size() - Closer.size());
			if(IsVerbatim) {
				// ディレクティブコメントの内側の本文の格納
				Record.Text = Interior;
				// 内側の本文の分解の終了
				return;
			}
			const std::string_view Head = GetSymbolHeadOf(Record, Interior);
			Record.Format.ShouldAttachFirstLine = !Head.empty();
			std::vector<std::string> Lines = GetBlockLines(GetRulesOf(Record), Interior);
			Record.Format.ShouldSeparateLogicalLines = Record.Format.ShouldSeparateLogicalLines || Lines.size() > 1;
			Record.Format.ShouldSeparateOpener = Lines.size() > 1 && Lines.front().empty();
			Record.Format.ShouldSeparateCloser = Lines.size() > 1 && Lines.back().empty();
			// 記号の一部の頭と本文の間の区切空白
			if(!Head.empty()) Lines.front() = GetHeadedLine(Lines.front(), Head);
			for(const std::string &Line : Lines) Record.Text.append(Line).push_back('\n');
			Record.Text.pop_back();
			// 囲みの本文の分解の終了
			return;
		}
		// 行記法の行毎の記号と区切空白の除去
		for(size_t Cursor = 0;;) {
			const size_t End = std::min(View.find('\n', Cursor), View.size());
			std::string_view Line = View.substr(Cursor, End - Cursor);
			if(Line.starts_with(Opener)) Line.remove_prefix(Opener.size());
			if(IsSpaced) {
				// 記号と本文の間と本文の後の全ての空白
				Line.remove_prefix(std::min(Line.find_first_not_of(" \t"), Line.size()));
				Line.remove_suffix(Line.size() - std::min(Line.find_last_not_of(" \t") + 1, Line.size()));
			} else if(!IsVerbatim || !Cursor) {
				// 記号と本文の間の空白の並び（区切空白，ディレクティブは処理系が区切と読む字）の除去と，区切空白の無い本文の密着の記録（本文を其の儘保つ記録の続きの行は記号の後を其の儘保つ）
				const std::string_view Body = HasValSpacing ?
				Line.substr(Line.starts_with(' ') ? 1 : 0) :
				IsVerbatim ? TrimDirectiveSeparator(Def, Line) : TrimLeadingSeparator(Line);
				// 値として読むディレクティブは其の儘密着させ，其の他のディレクティブは区切空白を補い，説明は記号の一部の頭だけを密着させる
				if(Body.size() == Line.size() && !Cursor && !Line.empty()) {
					Record.Format.ShouldAttachFirstLine = IsVerbatim ? HasValSpacing : !GetSymbolHeadOf(Record, Line).empty();
				}
				Line = Body;
			}
			// 記号の一部の頭で始まる最初の論理行の区切空白の統一
			if(!Cursor && Record.Format.ShouldAttachFirstLine && !IsVerbatim) {
				Record.Text.append(GetHeadedLine(Line, GetSymbolHeadOf(Record, Line)));
			} else Record.Text.append(Line);
			if(End == View.size()) break;
			Record.Text.push_back('\n');
			Cursor = End + 1;
		}
		// 終了
		return;
	}

	/**
	 * 行頭（桁０）へ置くと成立する記法かの判定関数
	 * 計算量：位置情報のテキストのバイト数 N に対し O(N + 1)
	 * @param Text 記号から後のテキスト
	 * @param Notation 記法
	 * @return 行頭の `//go:generate` と，位置情報の成立する行頭の `//line` なら true
	 */
	bool CommentRepository::ShouldActivateAtLineHead(const std::string_view Text, const CommentNotation Notation) {
		// 行頭で成立する記法と本文かの返戻
		return Notation == CommentNotation::GoGenerateLine ||
		Notation == CommentNotation::GoLineLine && HasValidLinePos(Text, Notation);
	}

	/**
	 * 記録への記法の定義と入力のドキュメント形式の記録関数（整形仕様 3.2）
	 * 説明のコメントがドキュメントに使う記法には其のドキュメント形式を記録し，ディレクティブコメントとドキュメントに使わない記法には記録しない
	 * 計算量：ドキュメント形式の登録数 D に対し O(D + 1)
	 * @param Rules 共有する言語の記法の定義
	 * @param Record 記法と役割を定めた記録
	 */
	void CommentRepository::AttachRules(const NotationRules &Rules, CommentRecord &Record) {
		// 共有する記法の定義とドキュメント形式の確定
		Record.Format.Rules = &Rules;
		Record.Format.InputDocument = Record.Role == CommentRole::Presentation ? Rules.GetDocFormOf(Record.Notation) : std::nullopt;
		// 終了
		return;
	}

	/**
	 * 入力の読取と出力の行形式が共有する行コメントの終端の判定関数
	 * 計算量：本文のバイト数 N，終端の登録数 W に対し O(W(N + 1))
	 * @param Rules 記法の定義
	 * @param Text 照合するテキスト
	 * @return LF 以外の行コメントの終端を含む場合 true
	 */
	bool CommentRepository::HasLineCommentBreak(const NotationRules &Rules, const std::string_view Text) {
		// 登録された終端を含むかの返戻
		return std::ranges::any_of(
			Rules.LineCommentBreaks,
			[Text](const std::string_view Break) -> bool {
				// 終端の綴りとの一致の返戻
				return Text.find(Break) != std::string_view::npos;
			}
		);
	}

	/**
	 * 論理行の本文を行コメントで保てるかの判定関数（整形仕様 7.6, 7.7）
	 * 行コメントの本文を行の途中で終えるテキスト・改行（トークン化前に展開する Unicode のエスケープで書いた物を含む）と，行末で次の行を繋ぐテキストを持つ本文は保てない
	 * 計算量：本文のバイト数 N，記法の登録語数 W に対し O((W + 1)(N + 1))
	 * @param Rules 投影済の記法
	 * @param Body 論理行の本文
	 * @param IsDecoded 既にトークン化前のエスケープを展開したテキストか
	 * @return 行コメントの本文として同じ行で終わるなら true
	 */
	bool CommentRepository::CanHoldInLineComment(const NotationRules &Rules, const std::string_view Body, const bool IsDecoded) {
		// 行を終えるテキスト・繋ぐテキストの返戻
		if(
			!Rules.LineCommentTerminator.empty() && Body.find(Rules.LineCommentTerminator) != std::string_view::npos ||
			HasLineCommentBreak(Rules, Body) || std::ranges::any_of(
				Rules.LineSplices,
				[Body](const std::string_view Splice) -> bool {
					// 行末に繋ぐテキストを持つかの返戻
					return Body.ends_with(Splice);
				}
			)
		) return false;
		// トークン化前に LF ･ CR へ展開するエスケープを持たず行コメントで保てるかの返戻
		return IsDecoded || !Rules.ShouldDecodeUnicodeEscapes || !ParseAdapter::HasEscapedLineBreak(Body);
	}

	/**
	 * グループの所属の取得関数（整形仕様 3.3, 7.3）
	 * 投影が記録した原文で続く独立行の説明のコメントの並び（同じアンカーの同じ側で空白だけを挟む物）を，ドキュメントの位置に従うグループを纏める記法の同じ続き毎のグループに分ける
	 * 計算量：記録の数 C に対し O(C log C)
	 * @param Records 記録の並び
	 * @param IsDocs 記録毎のドキュメントの位置に在る独立した記録か
	 * @return 記録毎のグループ（並びに属さない記録は `NoCommentGroup`）
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws FormatLimitExceeded 記法の定義を記録して居ない場合（フォーマッターの不具合）
	 */
	std::vector<CommentGroupId> CommentRepository::MembershipOf(
		const std::span<const CommentRecord> Records,
		const std::span<const uint8_t> IsDocs
	) {
		// 原文のグループと順に従う記録の整列
		std::vector<size_t> Order;
		Order.reserve(Records.size());
		for(size_t Idx = 0; Idx < Records.size(); ++Idx) if(Records[Idx].Format.Group != NoCommentGroup) Order.push_back(Idx);
		std::stable_sort(
			Order.begin(),
			Order.end(),
			[&Records](const size_t Left, const size_t Right) -> bool {
				// 並びと原文の順の比較の返戻
				return std::pair(Records[Left].Format.Group, Records[Left].OriginalOrder) <
				std::pair(Records[Right].Format.Group, Records[Right].OriginalOrder);
			}
		);
		std::vector<CommentGroupId> Groups(Records.size(), NoCommentGroup);
		CommentGroupId Next = 0;
		CommentNotation PrevNotation = {};
		// 並びの中のグループを纏める記法の同じ続き（囲みの記法は単独）
		for(size_t Pos = 0; Pos < Order.size(); ++Pos) {
			const size_t Idx = Order[Pos];
			const CommentNotation Notation = GetGroupNotationOf(Records[Idx], IsDocs[Idx]);
			const bool ShouldJoin =
			Pos && Records[Order[Pos - 1]].Format.Group == Records[Idx].Format.Group && !IsBlockNotation(Notation) &&
			PrevNotation == Notation;
			Groups[Idx] = ShouldJoin ? Groups[Order[Pos - 1]] : Next++;
			PrevNotation = Notation;
		}
		// 記録毎のグループの返戻
		return Groups;
	}

	/**
	 * 出力記法の記録の取得関数（整形仕様 7.6 の記法の選択）
	 * 計算量：元の記録と出力記法の本文のバイト数の和 N に対し O(N + 1)
	 * @param Record 記法の定義を記録したコメントの記録
	 * @return 出力する記法と本文を持つ記録のコピー
	 * @throws std::bad_alloc 記録又は本文のコピーに必要なメモリを確保出来ない場合
	 */
	CommentRecord CommentRepository::GetOutputFormOf(const CommentRecord &Record) {
		// 出力記法で読む記録のコピー
		CommentRecord Output = Record;
		Output.Notation = GetOutputNotationOf(Record);
		if(Record.Output && Record.Output->Text) Output.Text = *Record.Output->Text;
		if(Record.Output) Output.Format.ShouldAttachFirstLine = Record.Output->ShouldAttachFirstLine;
		// 出力記法の記録の返戻
		return Output;
	}

	/**
	 * アンカーのインデックスを更新済の記録から確定する関数
	 * 計算量：記録数 C に対し平均 O(C)
	 * @param Work 再登録した記録数（無ければ計上しない）
	 * @throws std::bad_alloc アンカーのインデックス又は記録位置の列のメモリを確保出来ない場合
	 */
	void CommentRepository::Reindex(size_t *const Work) {
		AnchorIdx.clear();
		for(size_t Idx = 0; Idx < Records.size(); ++Idx) {
			if(Work) ++*Work;
			AnchorIdx[Records[Idx].Anchor].push_back(Idx);
		}
		// 終了
		return;
	}

	/**
	 * アンカーに属する記録を原文順に取得する関数
	 * @param Anchor アンカーのノード
	 * @return 本文世代と同時に確定したインデックスで引いた記録の列
	 * @throws std::bad_alloc 記録の列又は本文のコピーに必要なメモリを確保出来ない場合
	 */
	CommentRecordList CommentRepository::Get(const NodeId Anchor) const {
		CommentRecordList Result;
		if(
			const std::unordered_map<NodeId, std::vector<size_t>>::const_iterator Found = AnchorIdx.find(Anchor);
			Found != AnchorIdx.end()
		) {
			Result.reserve(Found->second.size());
			for(const size_t Idx : Found->second) Result.push_back(Records[Idx]);
		}
		// アンカーの記録の返戻
		return Result;
	}

	/**
	 * 原文順の全記録の取得関数
	 * @return 読取専用の記録の並び
	 */
	std::span<const CommentRecord> CommentRepository::GetRecords() const {
		// 記録の並びの返戻
		return Records;
	}

	/**
	 * 未判定を含む共有本文構造の取得関数
	 * 計算量：O(1)
	 * @return 確定済の構造へのポインタ（未判定なら `nullptr`）
	 */
	const CommentStructure *CommentRepository::GetStructure() const {
		// 未判定なら空の共有構造の返戻
		return Structure.get();
	}

	/**
	 * 本文更新段で確定した構造の取得関数
	 * 本文構造が確定済である事を前提とする
	 * @return 配置・検証・静的解析が共有する構造
	 */
	const CommentStructure &CommentRepository::GetOutputStructure() const {
		// 共有本文構造の返戻
		return *Structure;
	}

	/**
	 * 所属確定済の説明対象の取得関数
	 * 計算量：O(1)
	 * @return 記録毎の説明対象
	 */
	std::span<const NodeId> CommentRepository::GetDocTargets() const {
		// 説明対象の返戻
		return DocTargets;
	}

	/**
	 * 配置確定済の説明対象の取得関数
	 * 計算量：O(1)
	 * @return 記録毎の配置対象
	 */
	std::span<const NodeId> CommentRepository::GetPlacementTargets() const {
		// 配置対象の返戻
		return PlacementTargets;
	}

	/**
	 * 本文構造の確定関数
	 * 計算量：差替前の記録数 C とグループ数 G に対し O(C + G + 1)
	 * @param Replacement 同じ本文世代の構造
	 * @param Work 再登録したアンカーの記録数（無ければ計上しない）
	 * @throws std::bad_alloc 共有する構造又はアンカーのインデックスのメモリを確保出来ない場合
	 */
	void CommentRepository::SetStructure(CommentStructure &&Replacement, size_t *const Work) {
		// 共有する本文構造の差替
		Structure = std::make_shared<const CommentStructure>(std::move(Replacement));
		for(size_t Idx = 0; Idx < Records.size(); ++Idx) Records[Idx].StructureIndex = Idx;
		// 本文構造だけの更新はアンカーを変えず，既存のインデックスを共有
		if(AnchorIdx.empty() && !Records.empty()) Reindex(Work);
		// 終了
		return;
	}

	/**
	 * 本文更新段の記録の取得関数
	 * 計算量：O(1)
	 * @return 更新段が所有する記録
	 */
	std::span<CommentRecord> CommentRepository::EditRecords() {
		// 更新する記録の返戻
		AnchorIdx.clear();
		return Records;
	}

	/**
	 * ドキュメント化対象の定義の取得関数
	 * @return 原文順のドキュメント化対象の定義の読取専用の並び
	 */
	std::span<const DocumentationSite> CommentRepository::GetDocumentationSites() const {
		// ドキュメント化対象の定義の並びの返戻
		return DocumentationSites;
	}

	/**
	 * ドキュメント化対象の定義の追加関数（整形仕様 3.2）
	 * 計算量：登録済の定義数 D に対し最悪 O(D + 1)，償却 O(1)
	 * @param Site 定義のアンカーと記法の定義
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	void CommentRepository::AddDocSite(const DocumentationSite &Site) {
		// ドキュメント化対象の定義の登録
		DocumentationSites.push_back(Site);
		// 終了
		return;
	}

	/**
	 * 記録の追加関数
	 * 計算量：既存の記録数 C，出力記法の確定費用 Q に対し最悪 O(C + Q + 1)
	 * @param Record 追加する記録（アンカーのノードを持つ）
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws FormatLimitExceeded 記法の定義が無い場合
	 */
	void CommentRepository::Add(CommentRecord &&Record) {
		// 出力記法の確定と記録の追加
		if(!Record.Output) ResolveOutput(Record);
		else Record.StructureIndex.reset();
		// 無効化済のインデックスへ一部だけを登録せず，次の確定で全記録を登録する
		if(!AnchorIdx.empty() || Records.empty()) AnchorIdx[Record.Anchor].push_back(Records.size());
		Records.push_back(std::move(Record));
		// 終了
		return;
	}

	/**
	 * 記録の差替関数
	 * ドキュメントの同期が作り直した記録の並びへ差し替える
	 * 計算量：旧記録と新記録の数の和 C，出力記法の確定費用の和 Q に対し O(C + Q + 1)
	 * @param Replacement 差し替える記録の並び
	 * @throws FormatLimitExceeded 記法の定義が無い場合
	 * @throws std::bad_alloc 変換する本文のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	void CommentRepository::Update(CommentRecordList &&Replacement) {
		AnchorIdx.clear();
		// 出力記法の確定と記録の差替
		for(CommentRecord &Record : Replacement) if(!Record.Output) ResolveOutput(Record);
		Records = std::move(Replacement);
		Reindex();
		// 終了
		return;
	}

	/**
	 * アンカーの移譲の適用関数（整形仕様 7.2）
	 * 構文正規化が合成した最終の行先を記録毎に１度だけ引き，消えたノードの先行・末尾・内部のアンカーを行先のノードの前後へ移す
	 * 計算量：記録の数 K に対し平均 O(K)
	 * @param Trace 構文正規化のアンカーの移譲の記録
	 * @throws FormatLimitExceeded 移る先の無いアンカーが残る場合（構文変換の不具合で，置けないコメントとして隠さない，整形仕様 7.7）
	 * @throws std::bad_alloc アンカーのインデックス又は記録位置の列のメモリを確保出来ない場合
	 */
	void CommentRepository::Remap(const RewriteTrace &Trace) {
		// 移譲の無い時の終了
		if(Trace.Moves.empty()) return;
		// 移譲前の構文への所属は，新しい構文から独立に求め直す
		TargetTables.reset();
		bool HasNewAnchor = false;
		// 記録の配置と置方の対応
		constexpr std::array<std::pair<CommentPlacement::Relation, AnchorSide>, 3> Sides {
			{
				{ CommentPlacement::After, AnchorSide::After },
				{ CommentPlacement::Before, AnchorSide::Before },
				{ CommentPlacement::Inside, AnchorSide::Inside }
			}
		};
		// 記録毎の移譲先と配置関係の更新
		for(CommentRecord &Record : Records) {
			AnchorSide Side = AnchorSide::Before;
			for(const std::pair<CommentPlacement::Relation, AnchorSide> &Mapping : Sides) if(Record.Placement.Side == Mapping.first) {
				Side = Mapping.second;
			}
			const std::unordered_map<uint64_t, AnchorMove>::const_iterator Found =
			Trace.Moves.find(RewriteTrace::GetMoveKey(Record.Anchor, Side, Record.Kind == CommentKind::Inline));
			if(Found == Trace.Moves.end()) continue;
			const AnchorMove &Move = Found->second;
			if(Move.Node == NoNode) throw FormatLimitExceeded("a comment anchor has no transfer target", FailureReason::FormatterDefect);
			HasNewAnchor = HasNewAnchor || Record.Anchor != Move.Node;
			Record.Anchor = Move.Node;
			for(const std::pair<CommentPlacement::Relation, AnchorSide> &Mapping : Sides) if(Mapping.second == Move.Side) {
				Record.Placement.Side = Mapping.first;
			}
			Record.Placement.IsEmptyBody = Record.Placement.IsEmptyBody || Move.IsEmptyBody;
			Record.Placement.IsRemovedCaseBody = Record.Placement.IsRemovedCaseBody || Move.IsRemovedCaseBody;
			if(Move.Reset == TokenReset::None) Record.Placement.TokensBefore = 0;
			if(Move.Reset == TokenReset::AllInsideAnchor) Record.Placement.TokensBefore = std::numeric_limits<uint16_t>::max();
		}
		// ドキュメント化対象の定義のアンカーは定義の前のアンカーと同じ移譲に従う
		for(DocumentationSite &Site : DocumentationSites) {
			const std::unordered_map<uint64_t, AnchorMove>::const_iterator Found =
			Trace.Moves.find(RewriteTrace::GetMoveKey(Site.Anchor, AnchorSide::Before, false));
			if(Found == Trace.Moves.end()) continue;
			if(Found->second.Node == NoNode) {
				throw FormatLimitExceeded("a documentation site has no transfer target", FailureReason::FormatterDefect);
			}
			Site.Anchor = Found->second.Node;
		}
		if(HasNewAnchor || AnchorIdx.empty() && !Records.empty()) Reindex();
		// 終了
		return;
	}
} // namespace Core
