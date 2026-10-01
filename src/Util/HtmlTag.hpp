#pragma once

#include "../Profile/GrammarProfile.hpp"
#include "Lang.hpp"
#include "ParsedTree.hpp"
#include <tree_sitter/api.h>
#include <array>
#include <string_view>

using HtmlNameBuffer = std::array<char, 16>; // HTML 名の小文字化の作業領域（照合する集合の最も長い名前が収まる大きさ）

/**
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
		if(const char Char = Name[Idx]; (Char >= 'A' && Char <= 'Z' ? Char + 'a' - 'A' : Char) != Lower[Idx]) return false;
	}
	// 全文字が一致した事の返戻
	return true;
}

/**
 * HTML 名の小文字化関数
 * 計算量：名前の長さ N に対し O(N)
 * @param Name 名前
 * @param Buffer 小文字化の作業領域
 * @return 小文字の名前（作業領域より長い名前は照合する何れの集合にも属さない為，空）
 */
inline std::string_view HtmlLowerName(const std::string_view Name, HtmlNameBuffer &Buffer) {
	// 照合する集合の何れの名前より長い場合の返戻
	if(Name.size() > Buffer.size()) return {};
	// 名前の各バイトの小文字化
	for(size_t Idx = 0; Idx < Name.size(); ++Idx) {
		const char Char = Name[Idx];
		Buffer[Idx] = Char >= 'A' && Char <= 'Z' ? static_cast<char>(Char + 'a' - 'A') : Char;
	}
	// 小文字の名前の返戻
	return { Buffer.data(), Name.size() };
}

/**
 * HTML 要素のタグ名取得関数
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
		if(const TSNode Name = ts_node_named_child(Tag, 0); Src.Is(Name, Core::Role::TagNameNode)) return Src.View(Name);
	}
	// 名前を持たない場合の返戻
	return {};
}

/**
 * 内容を文字として読む要素かの判定関数
 * @param Src ソース
 * @param Element 判定対象の要素
 * @return 内容を逐語保持する要素なら true
 */
inline bool IsRawContentElement(const ParsedTree &Src, const TSNode Element) {
	const std::string_view Name = HtmlTagName(Src, Element);
	// 名前の無い要素の内容を文字として読まない事の返戻
	if(Name.empty()) return false;
	const TSNode StartTag = ts_node_named_child(Element, 0);
	// 自己終端の要素が内容を持たない事の返戻
	if(!Src.Is(StartTag, Core::Role::StartTag)) return false;
	HtmlNameBuffer Buffer;
	// 要素の内容を文字として読むかの返戻
	return GrammarProfile::MarkupTraits(HtmlLowerName(Name, Buffer)) & GrammarProfile::Raw;
}

/**
 * HTML の script / style の中身の言語の判定関数（言語を確定出来ない type はデータの塊で，中身を文字の儘保つ）
 * 計算量：属性数 A と照合総バイト数 B に対し O(A + B)
 * @param Src ソース (HTML)
 * @param Element 判定対象の script_element / style_element
 * @return 中身の言語（JavaScript / JSON / CSS，確定出来なければ `Unknown`）
 */
inline Lang::ID HtmlEmbeddedLanguage(const ParsedTree &Src, const TSNode Element) {
	const ReadingRules &Rules = Src.GetReading();
	// 型を指定する属性の値と有無の格納域の準備
	std::string_view Value;
	bool HasType = false;
	ForEachNamedChild(
		ts_node_named_child(Element, 0),
		[&](const TSNode Attribute) -> bool {
			// 型を指定する属性の名前と値の照合
			// 型を指定する属性以外を走査し続ける事の返戻
			if(!Src.Is(Attribute, Core::Role::Attribute)) return true;
			const uint32_t Count = ts_node_named_child_count(Attribute);
			// 名前を持たない属性と型を指定しない属性を読み飛ばす事の返戻
			if(!Count || !IsHtmlNameEqual(Src.View(ts_node_named_child(Attribute, 0)), Rules.EmbeddedTypeAttribute)) return true;
			HasType = true;
			if(Count > 1) Value = Src.View(ts_node_named_child(Attribute, 1));
			// 型を指定する属性で打ち切る事の返戻
			return false;
		}
	);
	const bool IsScript = Src.Is(Element, Core::Role::EmbeddedScriptElement);
	// 要素の既定の言語
	const Lang::ID Default = IsScript ? Rules.EmbeddedScriptLanguage : Rules.EmbeddedStyleLanguage;
	// 型を指定しない中身の既定の言語の返戻
	if(!HasType) return Default;
	// 属性値の囲み文字と空白の除去
	const size_t First = Value.find_first_not_of(Rules.AttributeValueEdges);
	Value = First == std::string_view::npos ?
	std::string_view() :
	Value.substr(First, Value.find_last_not_of(Rules.AttributeValueEdges) + 1 - First);
	// 空の型の既定の言語の返戻
	if(Value.empty()) return Default;
	// 様式の型かの返戻
	if(!IsScript) return IsHtmlNameEqual(Value, Rules.EmbeddedStyleType) ? Rules.EmbeddedStyleLanguage : Lang::Unknown;
	for(const std::string_view Type : Rules.EmbeddedScriptTypes) {
		// 実行可能な内容の型かの返戻
		if(IsHtmlNameEqual(Value, Type)) return Rules.EmbeddedScriptLanguage;
	}
	// 構造化された内容の型かの返戻
	for(const std::string_view Type : Rules.EmbeddedDataTypes) if(IsHtmlNameEqual(Value, Type)) return Rules.EmbeddedDataLanguage;
	const std::string_view Prefix = Rules.StructuredDataTypePrefix, Suffix = Rules.StructuredDataTypeSuffix;
	// 構造化された接尾辞を持つ型の返戻
	if(
		Value.size() > Prefix.size() + Suffix.size() && IsHtmlNameEqual(Value.substr(0, Prefix.size()), Prefix) &&
		IsHtmlNameEqual(Value.substr(Value.size() - Suffix.size()), Suffix)
	) return Rules.EmbeddedDataLanguage;
	// データの塊である事の返戻
	return Lang::Unknown;
}
