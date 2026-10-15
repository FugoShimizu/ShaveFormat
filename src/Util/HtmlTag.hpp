#pragma once

#include "Lang.hpp"
#include "ParsedTree.hpp"
#include "TextEdit.hpp"
#include "../Profile/GrammarProfile.hpp"
#include <tree_sitter/api.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

using HtmlNameBuf = std::array<char, 16>;

/**
 * HTML 要素のタグ名取得関数
 * 計算量：O(1)
 * @param Src ソース (HTML)
 * @param Element 対象のノード
 * @return タグ名（要素でない場合と名前を持たない場合は空）
 */
inline std::string_view HtmlTagName(const ParsedTree &Src, const TSNode Element) {
	// 要素ノードと名前付子の有無の検査
	// 要素でない場合の返戻
	if(ts_node_is_null(Element) || !Src.Is(Element, Core::Role::ContentElement) || !ts_node_named_child_count(Element)) return {};
	// 開始タグの名前の取得
	if(const TSNode Tag = ts_node_named_child(Element, 0); ts_node_named_child_count(Tag)) {
		// 開始タグ（自己終端を含む）の最初の子がタグ名の場合の返戻
		if(const TSNode Name = ts_node_named_child(Tag, 0); Src.Is(Name, Core::Role::TagNameNode)) return Src.GetView(Name);
	}
	// 名前を持たない場合の返戻
	return {};
}

/**
 * HTML 名の小文字化関数
 * 計算量：名前の長さ N に対し O(N)
 * @param Name 名前
 * @param Buf 小文字化の作業領域
 * @return 小文字の名前（作業領域より長い名前は照合する何れの集合にも属さない為，空）
 */
inline std::string_view HtmlLowerName(const std::string_view Name, HtmlNameBuf &Buf) {
	// 照合する集合の何れの名前より長い場合の返戻
	if(Name.size() > Buf.size()) return {};
	// 名前の各バイトの小文字化
	for(size_t Idx = 0; Idx < Name.size(); ++Idx) {
		const unsigned char Char = Name[Idx];
		Buf[Idx] = Char >= 'A' && Char <= 'Z' ? static_cast<char>(Char + 'a' - 'A') : Char;
	}
	// 小文字の名前の返戻
	return { Buf.data(), Name.size() };
}

/**
 * HTML 名の小文字化の作業領域（照合する集合の最も長い名前が収まる大きさ）
 * HTML 名の ASCII 大小文字を無視した比較関数
 * 計算量：名前の長さ N に対し O(N)
 * @param Name 比較対象の名前
 * @param Lower 小文字で表した名前
 * @return 名前が一致すれば true
 */
inline bool IsHtmlNameEqual(const std::string_view Name, const std::string_view Lower) {
	// 長さが違う場合の不一致の返戻
	if(Name.size() != Lower.size()) return false;
	// 名前の各バイトの比較
	for(size_t Idx = 0; Idx < Name.size(); ++Idx) {
		// 大小文字を揃えても違う場合の不一致の返戻
		if(
			const unsigned char Char = Name[Idx];
			(Char >= 'A' && Char <= 'Z' ? Char + 'a' - 'A' : Char) != static_cast<unsigned char>(Lower[Idx])
		) return false;
	}
	// 全文字が一致した事の返戻
	return true;
}

/**
 * 内容を文字として読む要素かの判定関数
 * 計算量：O(1)
 * @param Src ソース
 * @param Element 判定対象の要素
 * @return 内容を逐語保持する要素なら true
 */
inline bool IsRawContentElement(const ParsedTree &Src, const TSNode Element) {
	// 対象名の取得
	const std::string_view Name = HtmlTagName(Src, Element);
	// 名前の無い要素と自己終端の要素の内容を文字として読まない事の返戻
	if(Name.empty() || !Src.Is(ts_node_named_child(Element, 0), Core::Role::StartTag)) return false;
	HtmlNameBuf Buf;
	// 要素の内容を文字として読むかの返戻
	return GrammarProfile::GetMarkupTraits(GrammarProfile::MarkupContext::Html, HtmlLowerName(Name, Buf)) & GrammarProfile::Raw;
}

/**
 * HTML の script / style の中身の言語の判定関数（言語を確定出来ない type はデータのブロックで，中身を文字の儘保つ）
 * 計算量：属性数 A と照合総バイト数 B に対し O(A + B)
 * @param Src ソース (HTML)
 * @param Element 判定対象の script_element / style_element
 * @return 中身の言語（JavaScript / JSON / CSS，確定出来なければ `Unknown`）
 * @throws std::bad_alloc 読取用の属性値を確保出来ない場合
 */
inline ::Lang HtmlEmbeddedLang(const ParsedTree &Src, const TSNode Element) {
	// 対象文法の記法
	const ReadingRules &Rules = Src.GetReading();
	// 型を指定する属性の値と有無の格納域の準備
	std::string_view Val;
	bool HasType = false;
	ForEachNamedChild(
		ts_node_named_child(Element, 0),
		[&](const TSNode Attribute) -> bool {
			// 型を指定する属性の名前と値の照合
			// 型を指定する属性以外を走査し続ける事の返戻
			if(!Src.Is(Attribute, Core::Role::Attribute)) return true;
			const uint32_t Count = ts_node_named_child_count(Attribute);
			// 名前を持たない属性と型を指定しない属性を読み飛ばす事の返戻
			if(!Count || !IsHtmlNameEqual(Src.GetView(ts_node_named_child(Attribute, 0)), Rules.EmbeddedTypeAttribute)) return true;
			HasType = true;
			if(Count > 1) Val = Src.GetView(ts_node_named_child(Attribute, 1));
			// 型を指定する属性で打ち切る事の返戻
			return false;
		}
	);
	const bool IsScript = Src.Is(Element, Core::Role::EmbeddedScriptElement);
	// 要素の既定の言語
	::Lang Default = Lang::Get(IsScript ? Rules.EmbeddedScriptLang : Rules.EmbeddedStyleLang);
	if(IsScript) Default.Goal = Lang::SrcGoal::Script;
	// 型を指定しない中身の既定の言語の返戻
	if(!HasType) return Default;
	// 構文上の外側の引用符１対だけの除去（値の中の引用符は保持）
	if(Val.size() > 1 && Val.front() == Val.back() && Rules.AttributeValQuotes.find(Val.front()) != std::string_view::npos) {
		Val = Val.substr(1, Val.size() - 2);
	}
	const std::optional<std::string> Decoded = GrammarProfile::DecodeMarkupAttribute(Val);
	// 型名の文字参照を識別出来ない内容の返戻
	if(!Decoded) return Lang::Get(Lang::Unknown);
	Val = *Decoded;
	TextEdit::TrimView(Val);
	// 空の型の既定の言語の返戻
	if(Val.empty()) return Default;
	// 様式の型かの返戻
	// 条件に該当する値の返戻
	if(!IsScript) return Lang::Get(IsHtmlNameEqual(Val, Rules.EmbeddedStyleType) ? Rules.EmbeddedStyleLang : Lang::Unknown);
	if(IsHtmlNameEqual(Val, Rules.EmbeddedModuleType)) {
		Default.Goal = Lang::SrcGoal::Module;
		// モジュールの言語と読みの目標の返戻
		return Default;
	}
	// JavaScript の MIME 型だけはパラメータを型名から分けて照合する
	std::string_view Mime = Val.substr(0, Val.find(';'));
	TextEdit::TrimView(Mime);
	// 実行可能な内容の型かの返戻
	for(const std::string_view Type : Rules.EmbeddedScriptTypes) if(IsHtmlNameEqual(Mime, Type)) return Default;
	for(const std::string_view Type : Rules.EmbeddedDataTypes) if(IsHtmlNameEqual(Val, Type)) {
		// 構造化された内容の言語の返戻
		return Lang::Get(Rules.EmbeddedDataLang);
	}
	if(
		const std::string_view Prefix = Rules.StructuredDataTypePrefix, Suffix = Rules.StructuredDataTypeSuffix;
		Val.size() > Prefix.size() + Suffix.size() && IsHtmlNameEqual(Val.substr(0, Prefix.size()), Prefix) &&
		IsHtmlNameEqual(Val.substr(Val.size() - Suffix.size()), Suffix)
	// 構造化された接尾辞を持つ型の返戻
	) return Lang::Get(Rules.EmbeddedDataLang);
	// データのブロックである事の返戻
	return Lang::Get(Lang::Unknown);
}
