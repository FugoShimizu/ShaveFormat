#pragma once

#include "NodeKind.hpp"
#include "TsSource.hpp"
#include <tree_sitter/api.h>
#include <array>
#include <cstdint>
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
	// 長さが違う場合の不一致返戻
	if(Name.size() != Lower.size()) return false;
	// 名前の各バイトの比較
	for(size_t Idx = 0; Idx < Name.size(); ++Idx) {
		// 大小文字を揃えても違う場合の不一致返戻
		if(const char Char = Name[Idx]; (Char >= 'A' && Char <= 'Z' ? Char + 'a' - 'A' : Char) != Lower[Idx]) return false;
	}
	// 全文字の一致返戻
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
inline std::string_view HtmlTagName(const TSSource &Src, const TSNode Element) {
	// 要素ノードと名前付子の有無の検査
	if(ts_node_is_null(Element) || std::string_view(ts_node_type(Element)) != "element" || !ts_node_named_child_count(Element)) {
		// 要素でない場合の返戻
		return {};
	}
	// 開始タグの名前の取得
	if(const TSNode Tag = ts_node_named_child(Element, 0); ts_node_named_child_count(Tag)) {
		// 開始タグ（自己終端を含む）の最初の子がタグ名の場合の返戻
		if(const TSNode Name = ts_node_named_child(Tag, 0); std::string_view(ts_node_type(Name)) == "tag_name") return Src.View(Name);
	}
	// 名前を持たない場合の返戻
	return {};
}

enum class HtmlVerbatim : uint8_t { None, Preformatted, RawContent }; // HTML 内容の整形，空白保持，逐語保持を表す区分

/**
 * HTML の要素の内容の逐語保持の区分判定関数
 * 計算量：属性数 A と照合総バイト数 B に対し O(A + B)
 * @param Src ソース (HTML)
 * @param Element 判定対象のノード（内容を持つ `element` 以外は保持不要）
 * @return 内容の逐語保持の区分
 */
inline HtmlVerbatim HtmlVerbatimOf(const TSSource &Src, const TSNode Element) {
	// 内容を持つ開始タグの確認
	const std::string_view Name = HtmlTagName(Src, Element);
	// 内容を持つ要素として解析されていない場合の返戻
	if(Name.empty()) return HtmlVerbatim::None;
	const TSNode StartTag = ts_node_named_child(Element, 0);
	// 自己終端の要素は内容を持たない為，保持不要の返戻
	if(std::string_view(ts_node_type(StartTag)) != "start_tag") return HtmlVerbatim::None;
	// 集合照合用の小文字タグ名の準備
	HtmlNameBuffer Buffer;
	const std::string_view Lower = HtmlLowerName(Name, Buffer);
	// 内容を文字として読む要素の返戻
	if(NodeKind::HtmlRawContentTag.Contains(Lower)) return HtmlVerbatim::RawContent;
	// 空白を描画する要素の返戻
	if(NodeKind::HtmlPreformattedTag.Contains(Lower)) return HtmlVerbatim::Preformatted;
	// `xml:space="preserve"` の要素は描画上有意な内部空白を逐語保持する
	bool ShouldPreserve = false;
	ForEachNamedChild(
		StartTag,
		[&](const TSNode Attribute) -> void {
			// 名前と値を持つ属性でない場合の返戻
			if(std::string_view(ts_node_type(Attribute)) != "attribute" || ts_node_named_child_count(Attribute) < 2) return;
			if(
				const TSNode AttributeName = ts_node_named_child(Attribute, 0);
				std::string_view(ts_node_type(AttributeName)) == "attribute_name" && IsHtmlNameEqual(Src.View(AttributeName), "xml:space")
			) {
				TSNode Value = ts_node_named_child(Attribute, 1);
				if(std::string_view(ts_node_type(Value)) == "quoted_attribute_value") Value = ts_node_named_child(Value, 0);
				ShouldPreserve = Src.View(Value) == "preserve";
			}
		}
	);
	// xml:space="preserve" 属性の有無に依る区分の返戻（タグ名で決まる要素は上で確定済）
	return ShouldPreserve ? HtmlVerbatim::Preformatted : HtmlVerbatim::None;
}

/**
 * HTML の script / style の中身が JS / CSS かの判定関数（其れ以外の type はデータの塊で，中身を文字の儘保つ）
 * 計算量：属性数 A と照合総バイト数 B に対し O(A + B)
 * @param Src ソース (HTML)
 * @param Element 判定対象の script_element / style_element
 * @return type 属性が無いか，JS (script) / CSS (style) の型なら true
 */
inline bool DoesHtmlEmbedCode(const TSSource &Src, const TSNode Element) {
	// type 属性値と有無の格納域の準備
	std::string_view Value;
	bool HasType = false;
	ForEachNamedChild(
		ts_node_named_child(Element, 0),
		[&](const TSNode Attribute) -> bool {
			// type 属性の名前と値の照合
			if(
				std::string_view(ts_node_type(Attribute)) != "attribute" || !ts_node_named_child_count(Attribute) ||
				!IsHtmlNameEqual(Src.View(ts_node_named_child(Attribute, 0)), "type")
			) return true; // type 以外の属性を走査し続ける事の返戻
			HasType = true;
			if(ts_node_named_child_count(Attribute) > 1) Value = Src.View(ts_node_named_child(Attribute, 1));
			// type 属性で打ち切る事の返戻
			return false;
		}
	);
	// type 属性の無い中身は JS / CSS である事の返戻
	if(!HasType) return true;
	// 属性値の囲み文字と空白の除去
	while(!Value.empty() && (Value.front() == '"' || Value.front() == '\'' || Value.front() == ' ' || Value.front() == '\t')) {
		Value.remove_prefix(1);
	}
	while(!Value.empty() && (Value.back() == '"' || Value.back() == '\'' || Value.back() == ' ' || Value.back() == '\t')) {
		Value.remove_suffix(1);
	}
	// 空の type は既定 (JS / CSS) の返戻
	if(Value.empty()) return true;
	// HTML 仕様の JavaScript の MIME 型と `module`（importmap は JSON で JS として読まない）
	static constexpr std::string_view ScriptTypes[] = {
		"application/ecmascript",
		"application/javascript",
		"application/x-ecmascript",
		"application/x-javascript",
		"module",
		"text/ecmascript",
		"text/javascript",
		"text/javascript1.0",
		"text/javascript1.1",
		"text/javascript1.2",
		"text/javascript1.3",
		"text/javascript1.4",
		"text/javascript1.5",
		"text/jscript",
		"text/livescript",
		"text/x-ecmascript",
		"text/x-javascript"
	};
	// CSS の型かの返戻
	if(std::string_view(ts_node_type(Element)) != "script_element") return IsHtmlNameEqual(Value, "text/css");
	// JS の型の返戻
	for(const std::string_view Type : ScriptTypes) if(IsHtmlNameEqual(Value, Type)) return true;
	// データの塊である事の返戻
	return false;
}
