#pragma once

#include "SyntaxRole.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace Core {
	using CommentGroupId = uint32_t; // 本文構造を纏めて判定するコメントのグループの識別子（整形仕様 3.3）

	inline constexpr CommentGroupId NoCommentGroup = std::numeric_limits<CommentGroupId>::max(); // グループを持たない単独の記録のマーカー

	// 投影時に定める記法の構造（本文の正規化と出力の記法の選択が読み，グループはドキュメントの位置が確定した後に分け直す）
	struct CommentFormat {
		const NotationRules *Rules = nullptr; // 共有する言語の記法の定義
		std::optional<DocumentFormId> InputDocument {}; // 入力の記法をドキュメントと読む時のドキュメント形式（ドキュメントに使わない記法とディレクティブコメントは無，通常コメントと記号を兼ねる記法のドキュメントかはアンカーと所属で決める）
		CommentGroupId Group = NoCommentGroup; // 本文構造を共有するグループ（投影が続く独立コメントを記録，位置確定後に記法の続き毎に分割，同期後の行形式のドキュメントは１グループ）
		CommentGroupId InputGroup = NoCommentGroup; // 原文のドキュメントの境界（本文正規化のグループとは独立し，同期時だけ読む）
		bool HasSummaryPlaceholder = false; // 概要を記述する為に空の論理行を補ったか
		bool ShouldAttachFirstLine = false; // 最初の論理行が区切空白を挟まず記号へ密着するか（前のメンバのドキュメント等の記号の一部と，空白を挟まないディレクティブコメント）
		bool ShouldSeparateLogicalLines = false; // 囲みの論理行を改行で分けるか（偽は文毎に分けた閉じた囲みの並びで，文中の囲みの改行は自動セミコロン挿入等の読みに関わる）
		bool ShouldSeparateOpener = false; // 囲みの開始記号を本文の前の独立行へ置くか（本文の空の論理行とは区別する）
		bool ShouldSeparateCloser = false; // 囲みの終端記号を本文の後の独立行へ置くか（本文の空の論理行とは区別する）
		size_t LeadingTabCount = 0; // 行末の物の同じ行の先行コード又はコメントとの区切空白の水平タブの数（其れ以外は０，整形仕様 7.1，7.6）
	};

	inline constexpr DocVocabularyEntry EmptyVocabulary { DocDescription::Other, DocItemForm::Prose, "", "", "", {} }; // 項目の定義が無い時の空の語彙

	/**
	 * ドキュメント形式と共通項目に対応する語彙の取得関数
	 * @param Vocabulary Profile が登録した変更不能な語彙
	 * @param Item 共通項目
	 * @return 対応する語彙（定義が無ければ空の語彙）
	 */
	constexpr const DocVocabularyEntry &VocabularyOf(
		const std::span<const DocVocabularyEntry> Vocabulary,
		const DocDescription Item
	) {
		// ドキュメント形式と説明項目に一致する語彙の探索
		const std::span<const DocVocabularyEntry>::iterator Found = std::ranges::find_if(
			Vocabulary,
			[Item](const DocVocabularyEntry &Entry) -> bool {
				// 形式と項目の一致の返戻
				return Entry.Item == Item;
			}
		);
		// 対応する語彙か，定義の無い組合せの空の語彙の返戻
		return Found == Vocabulary.end() ? EmptyVocabulary : *Found;
	}

	/**
	 * 登録済のドキュメント形式の判定関数
	 * 計算量：登録する記法数 F に対し O(F)
	 * @param Rules 投影済の記法
	 * @param Form ドキュメント形式
	 * @return 其の形式を登録する記法が在れば true
	 */
	inline bool HasDocumentForm(const NotationRules &Rules, const DocumentFormId Form) {
		// 投影済の形式との一致の返戻
		return std::ranges::any_of(
			Rules.DocumentForms,
			[Form](const DocumentNotation &Entry) -> bool {
				// 登録形式との一致の返戻
				return Entry.Form == Form;
			}
		);
	}

	/**
	 * タグの綴りと後続の区切に依る長さの取得関数
	 * 計算量：綴りの長さ L に対し O(L)
	 * @param Rules 投影済の記法
	 * @param Text タグから始まる本文
	 * @param Word タグの綴り
	 * @return 区切迄一致するタグの長さ（該当しなければ０）
	 */
	constexpr size_t DocTagWordLength(const NotationRules &Rules, const std::string_view Text, const std::string_view Word) {
		// 綴りが無いか一致しない事の返戻
		if(Word.empty() || !Text.starts_with(Word)) return 0;
		const std::string_view Rest = Text.substr(Word.size());
		// タグの終端の返戻
		if(Rest.empty()) return Word.size();
		const unsigned char Char = Rest.front();
		// 登録した区切文字との一致の返戻
		if(!Rules.DocTagTerminators.empty()) return Rules.DocTagTerminators.find(Char) == std::string_view::npos ? 0 : Word.size();
		const bool IsNameChar = Char >= 'A' && Char <= 'Z' || Char >= 'a' && Char <= 'z' || Char >= '0' && Char <= '9' || Char == '_';
		// 英数字と下線で構成するタグ名の終端の返戻
		return IsNameChar ? 0 : Word.size();
	}

	/**
	 * 登録済の項目タグの長さの取得関数
	 * 正規タグと投影済の同義タグだけを読み，別形式の綴りを項目へ読み替えない
	 * 計算量：登録数 S，候補の綴りの長さ L に対し O(S L + 1)
	 * @param Rules 投影済の記法
	 * @param Text タグから始まる本文
	 * @param Item 説明項目
	 * @return 登録済のタグの長さ（該当しなければ０）
	 */
	inline size_t DocItemTagLength(const NotationRules &Rules, const std::string_view Text, const DocDescription Item) {
		// タグ以外のドキュメント形式の返戻
		if(VocabularyOf(Rules.Vocabulary, DocDescription::Other).Form != DocItemForm::Tag) return 0;
		std::string_view Canonical = VocabularyOf(Rules.TagVocabulary, Item).Canonical;
		if(Item == DocDescription::Failure) Canonical = Rules.FailureTag;
		else if(Item == DocDescription::TypeParam) Canonical = Rules.TypeParamTag;
		else if(Item == DocDescription::Summary) Canonical = Rules.SummaryTag;
		// 其の形式に項目のタグが無い事の返戻
		if(Canonical.empty()) return 0;
		// JSDoc の短い概要は説明の同義語へ変換せず，別の綴りの概要として読む
		if(Item == DocDescription::Summary && !Rules.AlternateSummaryTag.empty()) {
			const size_t Length = DocTagWordLength(Rules, Text, Rules.AlternateSummaryTag);
			// 既存の短い概要のタグの長さの返戻
			if(Length) return Length;
		}
		// 正規タグの長さの返戻
		if(DocTagWordLength(Rules, Text, Canonical)) return Canonical.size();
		const std::vector<DocTagSynonym>::const_iterator Found = std::ranges::find_if(
			Rules.DocTagSynonyms,
			[&Rules, Canonical, Text](const DocTagSynonym &Synonym) -> bool {
				// 正規タグと登録済の同義タグとの一致の返戻
				return Synonym.To == Canonical && DocTagWordLength(Rules, Text, Synonym.From);
			}
		);
		// 登録済の同義タグの長さ，又は登録されて居ないタグの０の返戻
		return Found == Rules.DocTagSynonyms.end() ? 0 : Found->From.size();
	}

	/**
	 * 登録済の項目タグの消費関数
	 * @param Rules 投影済の記法
	 * @param Item 説明項目
	 * @param View 読取位置（成功時だけタグの直後へ進める）
	 * @return 登録済のタグを消費したら true
	 */
	inline bool ConsumeDocItemTag(const NotationRules &Rules, const DocDescription Item, std::string_view &View) {
		const size_t Length = DocItemTagLength(Rules, View, Item);
		// 該当するタグが無い事の返戻
		if(!Length) return false;
		View.remove_prefix(Length);
		// 登録済のタグの消費成功の返戻
		return true;
	}
} // namespace Core
