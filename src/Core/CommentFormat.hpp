#pragma once

#include "SyntaxRole.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>

namespace Core {
	enum class DocDescription : uint8_t { Failure, Inheritance, Other, Param, ParamList, Return, Summary }; // 文書形式に依らない説明項目
	enum class DocItemForm : uint8_t { Heading, HeadingList, List, Prose, Tag, XmlElement }; // 文書の項目の外形
	enum class DocStylePolicy : uint8_t { Standard, NameSummary, SourceOwned }; // 文書の所有と概要の扱い

	// 文書形式毎の読取語と生成語
	struct DocVocabularyEntry {
		DocumentationStyle Style; // 文書形式
		DocDescription Item; // 共通の説明項目
		DocItemForm Form; // 項目の外形
		std::string_view Canonical; // 生成時の規約の綴り
		std::string_view Prefix; // 名前を持つ項目の開始字面
		std::string_view Suffix; // 名前を持つ項目の終端字面
		std::array<std::string_view, 28> Aliases; // 読取時に同義とする綴り
		std::string_view Skeleton = {}; // 名前を持たない項目の生成字面
		std::array<std::string_view, 5> Qualifiers {}; // 札の後の修飾語
		std::string_view Attribute = {}; // 名前を取る要素の属性名
		DocStylePolicy Policy = DocStylePolicy::Standard; // 概要とソース所有の扱い
	};

	using CommentGroupId = uint32_t; // 本文構造を纏めて判定するコメントの群の識別子（整形仕様 3.3）

	inline constexpr CommentGroupId NoCommentGroup = std::numeric_limits<CommentGroupId>::max(); // 群を持たない単独の記録の印

	// 投影時に定める記法の構造（本文の正規化と出力の記法の選択が読み，群は文書の位置が確定した後に分け直す）
	struct CommentFormat {
		const NotationRules *Rules = nullptr; // 共有する言語の記法の定義
		std::optional<DocumentFormId> InputDocument {}; // 入力の記法を文書と読む時の文書形式（文書に使わない記法と指示コメントは無し，通常コメントと記号を兼ねる記法の文書かは錨と所属で決める）
		CommentGroupId Group = NoCommentGroup; // 本文構造を纏めて判定する群（投影が原文で続く独立行の説明のコメントの並びを記録し，文書の位置が確定した後に群を纏める記法の同じ続き毎に分ける，同期が組み直した行形式の文書は１つの群，整形仕様 3.3, 3.4, 7.3）
		bool AttachFirstLine = false; // 最初の論理行が区切空白を挟まず記号へ密着するか（前の成員の文書・区切の記法の頭等の記号の一部と，空白を挟まない指示コメント）
		bool SeparateLogicalLines = false; // 囲みの論理行を改行で分けるか（偽は文毎に分けた閉じた囲みの並びで，文中の囲みの改行は自動セミコロン挿入等の読みに関わる）
		size_t LeadingTabCount = 0; // 行末の物の同じ行の先行コード又はコメントとの区切空白の水平タブの数（其れ以外は０，整形仕様 7.1, 7.6）
	};

	inline constexpr DocVocabularyEntry DocVocabulary[] = {
		{
			DocumentationStyle::AtTag,
			DocDescription::Failure,
			DocItemForm::Tag,
			"@throws",
			"",
			"",
			{ "@exception", "@raise", "@throw", "@throws", "\\exception", "\\throw", "\\throws" }
		},
		{ DocumentationStyle::AtTag, DocDescription::Inheritance, DocItemForm::Tag, "@inheritdoc", "", "", { "@inheritdoc" } },
		{ DocumentationStyle::AtTag, DocDescription::Other, DocItemForm::Tag, "", "", "", {} },
		{
			DocumentationStyle::AtTag,
			DocDescription::Param,
			DocItemForm::Tag,
			"@param",
			"@param ",
			"",
			{ "@arg", "@argument", "@param", "\\param" },
			{},
			{ "in", "in,out", "inout", "out", "out,in" }
		},
		{ DocumentationStyle::AtTag, DocDescription::ParamList, DocItemForm::Tag, "", "", "", {} },
		{
			DocumentationStyle::AtTag,
			DocDescription::Return,
			DocItemForm::Tag,
			"@return",
			"@return",
			"",
			{ "@result", "@return", "@returns", "\\result", "\\return", "\\returns" },
			"@return"
		},
		{
			DocumentationStyle::AtTag,
			DocDescription::Summary,
			DocItemForm::Tag,
			"@brief",
			"",
			"",
			{ "@brief", "@desc", "@description", "@short", "@summary", "\\brief", "\\short" }
		},
		{ DocumentationStyle::Callout, DocDescription::Failure, DocItemForm::List, "Throws", "- Throws:", "", { "throws" } },
		{ DocumentationStyle::Callout, DocDescription::Inheritance, DocItemForm::Prose, "", "", "", {} },
		{
			DocumentationStyle::Callout,
			DocDescription::Other,
			DocItemForm::List,
			"",
			"",
			"",
			{
				"attention",
				"author",
				"authors",
				"bug",
				"complexity",
				"copyright",
				"date",
				"experiment",
				"important",
				"invariant",
				"keyword",
				"localizationkey",
				"mutatingvariant",
				"nonmutatingvariant",
				"note",
				"postcondition",
				"precondition",
				"recommended",
				"recommendedover",
				"remark",
				"remarks",
				"requires",
				"seealso",
				"since",
				"tag",
				"todo",
				"version",
				"warning"
			}
		},
		{ DocumentationStyle::Callout, DocDescription::Param, DocItemForm::List, "Parameter", "- Parameter ", ":", { "parameter" } },
		{
			DocumentationStyle::Callout,
			DocDescription::ParamList,
			DocItemForm::List,
			"Parameters",
			"- Parameters:",
			"",
			{ "parameters" }
		},
		{
			DocumentationStyle::Callout,
			DocDescription::Return,
			DocItemForm::List,
			"Returns",
			"- Returns:",
			"",
			{ "returns" },
			"- Returns:"
		},
		{ DocumentationStyle::Callout, DocDescription::Summary, DocItemForm::Prose, "", "", "", {} },
		{ DocumentationStyle::Docstring, DocDescription::Failure, DocItemForm::Prose, "", "", "", {} },
		{ DocumentationStyle::Docstring, DocDescription::Inheritance, DocItemForm::Prose, "", "", "", {} },
		{ DocumentationStyle::Docstring, DocDescription::Other, DocItemForm::Prose, "", "", "", {} },
		{ DocumentationStyle::Docstring, DocDescription::Param, DocItemForm::Prose, "", "", "", {} },
		{ DocumentationStyle::Docstring, DocDescription::ParamList, DocItemForm::Prose, "", "", "", {} },
		{ DocumentationStyle::Docstring, DocDescription::Return, DocItemForm::Prose, "", "", "", {} },
		{
			DocumentationStyle::Docstring,
			DocDescription::Summary,
			DocItemForm::Prose,
			"",
			"",
			"",
			{},
			{},
			{},
			{},
			DocStylePolicy::SourceOwned
		},
		{
			DocumentationStyle::MarkdownHeading,
			DocDescription::Failure,
			DocItemForm::Heading,
			"Errors",
			"# Errors",
			"",
			{ "Errors", "Panics" }
		},
		{ DocumentationStyle::MarkdownHeading, DocDescription::Inheritance, DocItemForm::Prose, "", "", "", {} },
		{ DocumentationStyle::MarkdownHeading, DocDescription::Other, DocItemForm::Heading, "", "", "", {} },
		{ DocumentationStyle::MarkdownHeading, DocDescription::Param, DocItemForm::HeadingList, "", "", "", {} },
		{
			DocumentationStyle::MarkdownHeading,
			DocDescription::ParamList,
			DocItemForm::Heading,
			"Arguments",
			"# Arguments",
			"",
			{ "Arguments" }
		},
		{
			DocumentationStyle::MarkdownHeading,
			DocDescription::Return,
			DocItemForm::Heading,
			"Returns",
			"# Returns",
			"",
			{ "Returns" }
		},
		{ DocumentationStyle::MarkdownHeading, DocDescription::Summary, DocItemForm::Prose, "", "", "", {} },
		{ DocumentationStyle::Prose, DocDescription::Failure, DocItemForm::Prose, "", "", "", {} },
		{ DocumentationStyle::Prose, DocDescription::Inheritance, DocItemForm::Prose, "", "", "", {} },
		{ DocumentationStyle::Prose, DocDescription::Other, DocItemForm::Prose, "", "", "", {} },
		{ DocumentationStyle::Prose, DocDescription::Param, DocItemForm::Prose, "", "", "", {} },
		{ DocumentationStyle::Prose, DocDescription::ParamList, DocItemForm::Prose, "", "", "", {} },
		{ DocumentationStyle::Prose, DocDescription::Return, DocItemForm::Prose, "", "", "", {} },
		{
			DocumentationStyle::Prose,
			DocDescription::Summary,
			DocItemForm::Prose,
			"",
			"",
			"",
			{},
			{},
			{},
			{},
			DocStylePolicy::NameSummary
		},
		{
			DocumentationStyle::XmlTag,
			DocDescription::Failure,
			DocItemForm::XmlElement,
			"exception",
			"<exception",
			"",
			{ "exception" }
		},
		{
			DocumentationStyle::XmlTag,
			DocDescription::Inheritance,
			DocItemForm::XmlElement,
			"inheritdoc",
			"",
			"",
			// 先頭の綴りを継承項目の識別に用いる
			{ "inheritdoc", "include" }
		},
		{ DocumentationStyle::XmlTag, DocDescription::Other, DocItemForm::XmlElement, "", "", "", {} },
		{
			DocumentationStyle::XmlTag,
			DocDescription::Param,
			DocItemForm::XmlElement,
			"param",
			"<param name=\"",
			"\"></param>",
			{ "param" },
			{},
			{},
			"name"
		},
		{ DocumentationStyle::XmlTag, DocDescription::ParamList, DocItemForm::XmlElement, "", "", "", {} },
		{
			DocumentationStyle::XmlTag,
			DocDescription::Return,
			DocItemForm::XmlElement,
			"returns",
			"<returns>",
			"</returns>",
			{ "returns" },
			"<returns></returns>"
		},
		{
			DocumentationStyle::XmlTag,
			DocDescription::Summary,
			DocItemForm::XmlElement,
			"summary",
			"<summary>",
			"</summary>",
			{ "summary" },
			"<summary></summary>"
		}
	}; // 共通項目と形式の語彙表

	inline constexpr DocVocabularyEntry EmptyVocabulary {
		DocumentationStyle::AtTag,
		DocDescription::Other,
		DocItemForm::Prose,
		"",
		"",
		"",
		{}
	}; // 項目の定義が無い時の空の語彙

	/**
	 * 文書形式と共通項目に対応する語彙の取得関数
	 * @param Style 文書形式
	 * @param Item 共通項目
	 * @return 対応する語彙（定義が無ければ空の語彙）
	 */
	constexpr const DocVocabularyEntry &VocabularyOf(const DocumentationStyle Style, const DocDescription Item) {
		const DocVocabularyEntry *const Found = std::ranges::find_if(
			DocVocabulary,
			[Style, Item](const DocVocabularyEntry &Entry) -> bool {
				// 形式と項目の一致の返戻
				return Entry.Style == Style && Entry.Item == Item;
			}
		);
		// 対応する語彙か，定義の無い組合せの空の語彙の返戻
		return Found == std::end(DocVocabulary) ? EmptyVocabulary : *Found;
	}
} // namespace Core
